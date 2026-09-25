#!/usr/bin/env sh
# Boots Uros (i386) under QEMU: the kernel by multiboot (-kernel), the
# bootstrap server, the stage-1 bundle and the DDB symbols as multiboot
# modules (-initrd).  The disk made by make-disk-image.sh is attached as port 0
# of an AHCI controller (#224; the kernel's IDE driver is gone).
#
# Usage:
#   ./scripts/run-qemu.sh                           # standard boot
#   ./scripts/run-qemu.sh -nographic -serial mon:stdio  # headless
#   ./scripts/run-qemu.sh --no-disk                 # no disk
#   ./scripts/run-qemu.sh --fresh-disk              # regenerate disk.img before
#                                                   # the boot (when the previous
#                                                   # run was cut off
#                                                   # mid-writeback)
#   ./scripts/run-qemu.sh --diskregen               # the same.  Unasked, the
#                                                   # disk is regenerated when it
#                                                   # is missing, with --minimal,
#                                                   # and when something it
#                                                   # carries was rebuilt after
#                                                   # it (not for --bench: the
#                                                   # suite rides the bundle)
#   ./scripts/run-qemu.sh --ahci2-image IMG         # IMG as the second AHCI
#                                                   # disk, NOT recreated: it is
#                                                   # /mnt/disk2, where x86-64
#                                                   # leaves the file this
#                                                   # target reads (#498)
#   ./scripts/run-qemu.sh --virtio                  # add a virtio-blk disk, a
#                                                   # copy of disk.img, after the
#                                                   # AHCI controller (#592)
#   ./scripts/run-qemu.sh --virtio-first            # the same, before it
#   ./scripts/run-qemu.sh --build-only              # only the build, which every
#                                                   # boot runs first; a caller
#                                                   # that times the boot runs
#                                                   # this before its clock
#
# The disk has three MBR partitions (make-disk-image.sh): a, ext2 with
# /mach_servers/ (bootstrap.conf and the servers); b, a small ext2; c, swap.
# The block server publishes them under the driver's prefix, ahci0a/b/c, and
# under the driver-agnostic aliases disk0a/b/c (#184, #224).  Bootstrap stage 2
# reads disk0a, default_pager pages to disk0c, and ext_server mounts ahci0a as
# /, by that name whichever controller is disk0.
set -e

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)

# What the run was taken under (#516).  Shared with run-x86_64.sh, because each
# of the two harnesses used to record the half the other omitted: this one
# sampled the clock and never named its accelerator, that one named its
# accelerator and never sampled the clock.
. "$(dirname "$0")/run-conditions.sh"
# Overridable so a measurement can run against a build nobody else touches.
#
# ⚠️ Not a convenience.  A campaign was once launched against uros/build while
# the same directory was being edited and re-ninja'd for unrelated work, so each
# boot picked up whatever that other work had last built, and the two arms of
# the A/B no longer differed by one thing.  The logs were discarded.
#
# This script builds $BUILD_DIR before every boot (below), and a build
# directory builds from the source tree it was configured from.  So an arm that
# must stay old needs its own build directory configured from its own worktree,
# not just its own build directory.
#
#	UROS_BUILD_DIR=/path/to/build-measure scripts/run-qemu.sh ...
BUILD_DIR="${UROS_BUILD_DIR:-$REPO_ROOT/uros/build}"
KERNEL="$BUILD_DIR/export/uros/boot/mach_kernel"
ARCH="$(cat "$BUILD_DIR/uros-arch" 2>/dev/null || uname -m)"   # #404 stamp
BOOTSTRAP="$BUILD_DIR/export/uros/$ARCH/user/sbin/bootstrap"
DISK_IMG="$BUILD_DIR/disk.img"
BUNDLE_IMG="$BUILD_DIR/bootstrap.bundle"

# Parse flags
USE_DISK=true
# Issue #224: IDE driver is no longer used.  The primary disk is always
# attached as AHCI (port 0), populated by make-disk-image.sh with the
# 3-partition layout disk0a/disk0b/disk0c.  --ahci is kept for backward
# compatibility but is a no-op when USE_DISK=true.
USE_AHCI=true
USE_AHCI2=false
AHCI2_IMAGE=""      # --ahci2-image PATH: attach PATH as AHCI port 1 AS IT IS (#498)
USE_VIRTIO=false
VIRTIO_FIRST=false  # --virtio-first: the virtio-blk controller goes before the AHCI one (#592)
USE_BUNDLE=true     # Issue #186: stage-1 multiboot bundle (mod[1]) on by default
USE_SHA_NI=false    # Issue #180: --sha-ni → TCG + Icelake-Server,+sha-ni
# #516: this target has only ever been run under KVM, and until now there was
# no way to ask for the other accelerator without also changing the CPU model.
USE_TCG=false
FRESH_DISK=false    # --fresh-disk: regen disk.img before launch (avoids stale
                    # stage-2 binaries after rebuild + ext2 writeback corruption
                    # from previous ungraceful QEMU exit)
BENCH_ARGS=""
EXTRA_ARGS=""
MINIMAL_ARG=""
NO_REBOOT="-no-reboot"
# Kept before the loop below eats them (#516): a condition nobody can reproduce
# the command for is half a condition.
ORIG_ARGS="$*"
SMP_COUNT=""
DISK_REGEN=false    # --diskregen: regenerate disk.img this launch even if it is
                    # current (see the regeneration block for when it is not;
                    # --bench alone never asks: the suite rides the bundle)
REUSE_BUNDLE=false  # --reuse-bundle: skip the make-bundle.sh step and reuse the
                    # existing bootstrap.bundle while nothing it is packed from
                    # changed (repeated launches of the same kernel+servers,
                    # e.g. SMP reliability loops)
BUILD_ONLY=false    # --build-only: run the build step below and exit (#592)
CONSOLE_ARG=""      # --with-console: ship the on-screen console TTY (#363) in the
                    # bundle so ush binds the graphical window instead of the UART.
                    # Off by default → serial/headless/bench paths unchanged.
while [ $# -gt 0 ]; do
    case "$1" in
        --no-disk) USE_DISK=false; USE_AHCI=false; shift ;;
        --with-console) CONSOLE_ARG="--with-console"; shift ;;
        --window) EXTRA_ARGS="$EXTRA_ARGS -display gtk"; shift ;;
        --no-bundle) USE_BUNDLE=false; shift ;;
        --ahci2) USE_AHCI=true; USE_AHCI2=true; shift ;;
        --ahci2-image) USE_AHCI=true; USE_AHCI2=true; AHCI2_IMAGE="$2"; shift 2 ;;
        --ahci) USE_AHCI=true; shift ;;
        --virtio) USE_VIRTIO=true; shift ;;
        --virtio-first) USE_VIRTIO=true; VIRTIO_FIRST=true; shift ;;
        --sha-ni) USE_SHA_NI=true; shift ;;
        --tcg) USE_TCG=true; shift ;;
        --fresh-disk) FRESH_DISK=true; shift ;;
        --diskregen) DISK_REGEN=true; shift ;;
        --reuse-bundle) REUSE_BUNDLE=true; shift ;;
        --build-only) BUILD_ONLY=true; shift ;;
        --minimal) MINIMAL_ARG="--minimal"; FRESH_DISK=true; shift ;;
        --allow-reboot) NO_REBOOT=""; shift ;;
        --smp) shift; SMP_COUNT="$1"; shift ;;
        --bench)
            shift
            while [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; do
                BENCH_ARGS="$BENCH_ARGS $1"
                shift
            done
            ;;
        *) EXTRA_ARGS="$EXTRA_ARGS $1"; shift ;;
    esac
done

# A virtio disk beside no AHCI disk would never finish booting: ext_server
# mounts ahci0a as / and waits for it (ext_server.c), whichever controller the
# block server numbers first.  Said before anything is built.
if [ "$USE_VIRTIO" = true ] && [ "$USE_AHCI" != true ]; then
    echo "ERROR: --virtio needs the AHCI boot disk beside it: ext_server mounts ahci0a as / and waits for it"
    exit 1
fi

# Build what is about to boot (#592).
#
# This script used to boot whatever $BUILD_DIR already held: the kernel however
# old, a bundle packed from servers however old, and a disk regenerated only
# when asked.  Its own comment said it rebuilt them, so an edit followed by a
# boot without a ninja in between tested the previous build and reported on the
# edit.  Now it runs ninja first, as run-x86_64.sh does, and then re-packs the
# bundle and regenerates the disk whenever a binary they carry is newer than
# they are (below).  ush, the terminal server and the programs on the ext2 root
# travel only on the disk, so the disk has to follow the build too.
#
# Everything, not a list of targets: make-bundle.sh and make-disk-image.sh pack
# whatever lies under export/, and a list would go stale the day a server is
# added to the bundle.  The output goes to a file and two lines are printed,
# because the callers keep this script's output as the run's log.
#
# ⚠️ A caller that times the boot runs `run-qemu.sh --build-only' first.
# Otherwise the build falls inside its clock: smoke-ush.exp would call forty
# silent seconds of compiling a machine that stopped talking, and a total
# budget would shrink by however long the compiler took.
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "ERROR: $BUILD_DIR is not a configured build directory (no build.ninja)"
    exit 1
fi
BUILD_LOG="$BUILD_DIR/run-qemu-build.log"
BUILD_T0=$(date +%s)
echo "Build:   ninja -C $BUILD_DIR (output in $BUILD_LOG)"
if ! ninja -C "$BUILD_DIR" > "$BUILD_LOG" 2>&1; then
    tail -n 30 "$BUILD_LOG"
    echo "ERROR: ninja -C $BUILD_DIR failed (whole output in $BUILD_LOG); nothing was booted"
    exit 1
fi
# Ninja's last line is the last step to finish, not a total, so the steps are
# counted, leaving out its re-check of globbed directories, which is not one.
if grep -q '^ninja: no work to do' "$BUILD_LOG"; then
    echo "Build:   up to date"
else
    echo "Build:   $(grep '^\[[0-9]*/[0-9]*\]' "$BUILD_LOG" | grep -vc 'Re-checking globbed') steps in $(( $(date +%s) - BUILD_T0 ))s"
fi
[ "$BUILD_ONLY" = true ] && exit 0

# Whether anything an image is packed from is newer than REF; a missing REF
# counts as older than everything.  The inputs are read off the two
# generators: both pack export/uros/$ARCH/user, make-disk-image.sh also carries
# musl's libc.so as /lib/ld-musl-$ARCH.so.1, and the bundle's format is
# mkbundle's.
inputs_newer_than() {
    _ref=$1; shift
    [ -f "$_ref" ] || return 0
    [ -n "$(find "$@" -type f -newer "$_ref" -print 2>/dev/null | head -n 1)" ]
}
USER_EXPORT="$BUILD_DIR/export/uros/$ARCH/user"
MUSL_LIBC="$BUILD_DIR/src/contrib/musl-install/lib/libc.so"

# The disk's age is its stamp's, not its own mtime: qemu writes the image
# (ext2 on disk0a and disk0b, paging on disk0c), so disk.img's mtime says when
# the guest last wrote, and a binary built while a guest ran would look older
# than the disk.  The stamp is taken when a regeneration starts and kept only
# once it has finished, so a regeneration cut off halfway leaves the disk
# stale rather than current.  A disk with no stamp -- made by hand, or before
# #592 -- is regenerated once.
DISK_STAMP="$DISK_IMG.stamp"
STALE_DISK=false
if [ "$USE_AHCI" = true ] && [ -f "$DISK_IMG" ] &&
   inputs_newer_than "$DISK_STAMP" "$USER_EXPORT" "$MUSL_LIBC"; then
    STALE_DISK=true
fi

# Written beside the image and moved over it, so a qemu that still has the old
# disk open keeps its own inode instead of seeing its filesystems rewritten.
regenerate_disk() {
    rm -f "$DISK_IMG.new"
    touch "$DISK_STAMP.new"
    "$REPO_ROOT/scripts/make-disk-image.sh" -o "$DISK_IMG.new" "$@"
    mv -f "$DISK_IMG.new" "$DISK_IMG"
    mv -f "$DISK_STAMP.new" "$DISK_STAMP"
}

# Disk-image regeneration happens with --diskregen, --fresh-disk or --minimal,
# when disk.img is missing, and, when the disk is attached, when something it
# carries is newer than its stamp (#592).  Otherwise the existing disk is
# reused -- even with --bench -- so iterating on the kernel alone doesn't pay
# the disk-format cost every launch.
# The bench suite reaches ipc_bench through the stage-1 bundle (rebuilt below),
# so changing --bench suites does NOT need a disk regen.
if [ "$DISK_REGEN" = true ] || [ "$FRESH_DISK" = true ] || [ ! -f "$DISK_IMG" ] || [ "$STALE_DISK" = true ]; then
    [ -f "$DISK_IMG" ] || echo "disk.img missing — regenerating."
    [ "$STALE_DISK" = true ] && echo "disk.img is older than something it carries, or has no stamp — regenerating."
    if [ -n "$BENCH_ARGS" ]; then
        echo "Regenerating disk image (--bench):$BENCH_ARGS"
        regenerate_disk --bench $BENCH_ARGS $MINIMAL_ARG
    else
        echo "Regenerating disk image$([ -n "$MINIMAL_ARG" ] && echo " (--minimal)")…"
        regenerate_disk $MINIMAL_ARG
    fi
elif [ -n "$BENCH_ARGS" ]; then
    echo "Bench suites:$BENCH_ARGS (via bundle; disk reused — pass --diskregen to rebuild it)"
fi

# Issue #186: (re)build the stage-1 bundle so its bootstrap.conf and
# binaries stay in sync with the on-disk copy (especially with --bench).
# --reuse-bundle keeps the existing one only while nothing it is packed from
# is newer (#592): reusing it after the build above changed a server would
# boot a new kernel with old servers.  The bundle's own mtime is its age:
# nothing but make-bundle.sh writes it.
if [ "$USE_BUNDLE" = true ]; then
    if [ "$REUSE_BUNDLE" = true ] && [ -f "$BUNDLE_IMG" ] &&
       ! inputs_newer_than "$BUNDLE_IMG" "$USER_EXPORT" "$BUILD_DIR/tools/mkbundle"; then
        echo "Bundle:  reusing $BUNDLE_IMG (--reuse-bundle, skipped rebuild)"
    elif [ -n "$BENCH_ARGS" ]; then
        "$REPO_ROOT/scripts/make-bundle.sh" --bench $BENCH_ARGS $MINIMAL_ARG $CONSOLE_ARG
    else
        "$REPO_ROOT/scripts/make-bundle.sh" $MINIMAL_ARG $CONSOLE_ARG
    fi
fi

if [ ! -f "$KERNEL" ]; then
    echo "ERROR: kernel non trovato: $KERNEL"
    echo "Build con:"
    echo "  cd $BUILD_DIR && cmake -DOSFMK_BUILD_KERNEL=ON -DOSFMK_BUILD_BOOTSTRAP=ON .. -G Ninja && ninja"
    exit 1
fi

if [ ! -f "$BOOTSTRAP" ]; then
    echo "ERROR: bootstrap server non trovato: $BOOTSTRAP"
    exit 1
fi

# Costruisci la riga di comando QEMU.
#
# Issue #186: se è presente il bundle stage-1 lo passiamo come secondo
# modulo multiboot via -initrd "<bootstrap>,<bundle>".  QEMU multiboot
# accetta una lista comma-separated; il kernel (kern/bootstrap.c) mappa
# mod[1] nello spazio del bootstrap server e gli passa --bundle=ADDR,SIZE.
INITRD="$BOOTSTRAP"
if [ "$USE_BUNDLE" = true ] && [ -f "$BUNDLE_IMG" ]; then
    INITRD="$BOOTSTRAP,$BUNDLE_IMG"
    echo "Bundle:  $BUNDLE_IMG"
fi
# Issue #211: ksyms.bin (kernel DDB symbol table) shipped as mod[2] when
# present.  The kernel scans multiboot mods for the "KSYM" magic; the
# module-position is not significant.
KSYMS_IMG="$BUILD_DIR/export/uros/boot/ksyms.bin"
if [ -f "$KSYMS_IMG" ]; then
    INITRD="$INITRD,$KSYMS_IMG"
    echo "DDB syms: $KSYMS_IMG"
fi
# Issue #180: --sha-ni forces TCG with Icelake-Server,+sha-ni so the
# kernel and libcap exercise the SHA-NI compress fast path even when
# the host CPU (or KVM-restricted CPUID) doesn't expose it.  TCG is
# slower than KVM but produces correct architectural behaviour.
# ── Which accelerator, and said on EVERY path (#516) ──────────────────
#
# 🔴 This used to announce itself only on the --sha-ni path.  The default is
# KVM, it said nothing, and so every i386 result this project holds was taken
# under KVM without any of them recording it -- the exact mirror of #516's
# complaint about x86-64, where every result was TCG.
#
# 🔴 AND THE CPU MODEL CANNOT BE HELD FIXED ACROSS THE TWO.  `-cpu host' exists
# only under KVM: it means "pass the host's CPUID through", which an emulator
# has no way to honour.  So --tcg takes `max', the most any accelerator will
# offer, and the model goes into the conditions block rather than being assumed
# away.  A disagreement between the two runs might be the accelerator or might
# be the model, and pretending otherwise would be inventing a control this
# machine cannot give.
if [ "$USE_SHA_NI" = true ]; then
    ACCEL="TCG (--sha-ni, which also changes the CPU model)"
    ACCEL_ARGS="-accel tcg -cpu Icelake-Server,+sha-ni"
elif [ "$USE_TCG" = true ]; then
    ACCEL="TCG (--tcg)"
    ACCEL_ARGS="-accel tcg -cpu max"
else
    ACCEL="KVM (the default here; --tcg for the other one)"
    ACCEL_ARGS="-enable-kvm -cpu host"
fi
QEMU_ARGS="-m 512M $ACCEL_ARGS -kernel $KERNEL -initrd $INITRD $NO_REBOOT"
# #300: --smp N exposes N CPUs to the guest.  Requires the kernel to be
# built with -DUROS_NCPUS=N (or >=N).
if [ -n "$SMP_COUNT" ]; then
    QEMU_ARGS="$QEMU_ARGS -smp $SMP_COUNT"
    echo "SMP: $SMP_COUNT CPUs"
fi

# Issue #224: the kernel's IDE driver is gone, and disk.img (three MBR
# partitions, see the header) is attached directly as AHCI port 0.  Stage 2
# and default_pager reach it through the disk0 aliases, so under
# --virtio-first they run on the virtio copy instead (below).
#
# create_ahci_test_disk <path> <label> -- a second AHCI disk, for multi-mount
# only: 40 MB, two ext2 partitions each with its own hello.txt, raw swap, no
# /mach_servers/.
create_ahci_test_disk() {
    _disk="$1"
    _label="$2"

    echo "  Creazione disco AHCI secondario: $_disk ($_label)"
    dd if=/dev/zero of="$_disk" bs=1M count=40 status=none

    sfdisk --quiet "$_disk" <<SFDISK
label: dos
start=2048,  size=8192,  type=83
start=10240, size=8192,  type=83
start=18432,             type=82
SFDISK

    for _seek in 2048 10240; do
        _part=$(mktemp /tmp/osfmk-ahci-part.XXXXXX.img)
        dd if=/dev/zero of="$_part" bs=512 count=8192 status=none
        mke2fs -t ext2 -q -F -b 4096 -I 256 -r 1 -O filetype "$_part"
        _htxt=$(mktemp)
        printf 'Hello from %s partition (sect %s)\n' "$_label" "$_seek" > "$_htxt"
        debugfs -w -f /dev/stdin "$_part" <<DBGFS 2>/dev/null
write $_htxt hello.txt
DBGFS
        rm -f "$_htxt"
        dd if="$_part" of="$_disk" bs=512 seek="$_seek" conv=notrunc status=none
        rm -f "$_part"
    done
}

AHCI_ARGS=""
if [ "$USE_AHCI" = true ]; then
    if [ ! -f "$DISK_IMG" ]; then
        echo "ERRORE: $DISK_IMG non trovato — esegui ./scripts/make-disk-image.sh prima"
        exit 1
    fi
    echo "AHCI port 0: $DISK_IMG (contiene /mach_servers/, hello.txt, swap)"
    AHCI_ARGS="-device ich9-ahci,id=ahci0"
    AHCI_ARGS="$AHCI_ARGS -drive id=ahcidisk0,file=$DISK_IMG,format=raw,if=none"
    AHCI_ARGS="$AHCI_ARGS -device ide-hd,drive=ahcidisk0,bus=ahci0.0"

    if [ "$USE_AHCI2" = true ]; then
        # #498: --ahci2-image attaches a disk ANOTHER boot wrote -- the one an
        # x86-64 run leaves as disk-x86_64-ahci2.img, which ext_server mounts
        # at /mnt/disk2 on both targets.  It is never recreated here: the
        # bytes on it are the evidence.
        if [ -n "$AHCI2_IMAGE" ]; then
            if [ ! -f "$AHCI2_IMAGE" ]; then
                echo "ERRORE: --ahci2-image $AHCI2_IMAGE non esiste"
                exit 1
            fi
            AHCI_DISK1="$AHCI2_IMAGE"
        else
            AHCI_DISK1="$BUILD_DIR/ahci-test1.img"
            create_ahci_test_disk "$AHCI_DISK1" "disk1"
        fi
        AHCI_ARGS="$AHCI_ARGS -drive id=ahcidisk1,file=$AHCI_DISK1,format=raw,if=none"
        AHCI_ARGS="$AHCI_ARGS -device ide-hd,drive=ahcidisk1,bus=ahci0.1"
        echo "AHCI port 1: $AHCI_DISK1"
    fi
elif [ "$USE_DISK" = true ]; then
    echo "ATTENZIONE: disco richiesto ma --ahci non attivo; bootstrap stage-2"
    echo "  non potrà aprire disk0a — avvio degraded (solo bundle stage-1)."
fi

# --virtio, --virtio-first: a virtio-blk-pci controller beside the AHCI one.
#
# Its disk is a byte copy of disk.img, made again at every launch (#592): the
# same partition table, the same /mach_servers/ and the same swap partition.
# So it serves stage 2's disk0a and default_pager's disk0c whichever of the two
# controllers the block server numbers first, and this script does not have to
# predict which one that is.  The block server numbers disks in the order the
# controllers bind, which is the order of their PCI slots, and qemu hands out
# slots in command-line order: --virtio attaches the controller after the AHCI
# one, --virtio-first before it.  The log names the winner:
# "blk: registered alias 'disk0a' -> '<controller>0a'".
#
# A copy and not the same file twice, because two controllers writing one image
# corrupt it.  Removed first, so a qemu that still has the old copy open keeps
# its own inode.  Not read-only, because as disk0 it takes default_pager's
# paging on disk0c.
#
# Before #224 this block built a new 40 MB disk and copied into it the first
# 4 MB of the IDE disk's first partition.  That function was renamed away and
# this call was left behind, so the flag exited with 127 before qemu started,
# from then until #592.
VIRTIO_DISK="$BUILD_DIR/virtio-test.img"
VIRTIO_ARGS=""
if [ "$USE_VIRTIO" = true ]; then
    rm -f "$VIRTIO_DISK"
    cp --sparse=always "$DISK_IMG" "$VIRTIO_DISK"
    VIRTIO_PLACE="$([ "$VIRTIO_FIRST" = true ] && echo before || echo after) the AHCI controller"
    echo "Virtio-blk: $VIRTIO_DISK (byte copy of disk.img), $VIRTIO_PLACE"
    VIRTIO_ARGS="-drive id=virtiodisk0,file=$VIRTIO_DISK,format=raw,if=none"
    VIRTIO_ARGS="$VIRTIO_ARGS -device virtio-blk-pci,drive=virtiodisk0"
fi
if [ "$VIRTIO_FIRST" = true ]; then
    QEMU_ARGS="$QEMU_ARGS $VIRTIO_ARGS $AHCI_ARGS"
else
    QEMU_ARGS="$QEMU_ARGS $AHCI_ARGS $VIRTIO_ARGS"
fi

# ------------------------------------------------------- run conditions
#
# Say what the host was doing and what the machine is, on the same output as
# the run.  The reasoning lives in scripts/run-conditions.sh, which both
# harnesses now share; what used to be here was the host half alone (#460).
#
# ⚠️ BEFORE the run and not after, because this script ends in `exec qemu'
# deliberately -- without it this shell stays qemu's parent for the whole run
# -- so nothing of ours runs once the machine does.  The block says so rather
# than leaving a missing line: an i386 run cannot notice its own clock moving,
# and a reader comparing it with an x86-64 run needs to know which of the two
# could have.
uros_conditions_open
uros_conditions_block "i386" "$ACCEL" \
	"clock in run: not measured (this harness execs qemu; see host end)" \
	"cpu/accel:    $ACCEL_ARGS" \
	"smp:          ${SMP_COUNT:-1}" \
	"disk:         $([ "$USE_DISK" = true ] && echo yes || echo no)" \
	"virtio:       $([ "$USE_VIRTIO" = true ] && echo "$VIRTIO_DISK, $VIRTIO_PLACE" || echo no)" \
	"command:      $0 $ORIG_ARGS"

# ⚠️ `exec' stays.  Without it this shell remains qemu's parent, and every
# harness here starts the script with Popen() and later calls terminate() on
# that pid -- which would then kill the shell and leave qemu running.  Stray
# qemus are not a cosmetic problem: two overlapping runs kill each other and
# manufacture the very failure being hunted (12 false failures in #438).
#
# So the elapsed time is not measured here: the start timestamp is printed
# above, and the callers already report per-verdict seconds.
# shellcheck disable=SC2086
exec qemu-system-i386 $QEMU_ARGS $EXTRA_ARGS
