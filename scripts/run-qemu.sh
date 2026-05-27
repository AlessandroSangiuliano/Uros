#!/usr/bin/env sh
# Avvia Uros sotto QEMU: kernel multiboot + bootstrap server come modulo.
#
# Il kernel Mach viene caricato via multiboot (-kernel), il bootstrap server
# come modulo multiboot (-initrd). Se presente un'immagine disco con i server
# (creata da make-disk-image.sh), viene aggiunta come IDE primary (-hda).
#
# Uso:
#   ./scripts/run-qemu.sh                           # avvio standard
#   ./scripts/run-qemu.sh -nographic -serial mon:stdio  # headless
#   ./scripts/run-qemu.sh --no-disk                 # senza disco
#   ./scripts/run-qemu.sh --fresh-disk              # rigenera disk.img prima
#                                                   # del boot (utile dopo
#                                                   # rebuild o se la run
#                                                   # precedente è stata
#                                                   # chiusa a metà writeback)
#
# L'immagine disco contiene /mach_servers/ con:
#   bootstrap.conf   — configurazione del bootstrap
#   default_pager    — server di paging
#
# Il driver IDE del kernel (hd.c) vede il disco QEMU come hd0.
# boot_device → d_partitions[0] → prima partizione MBR (ext2).
set -e

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$REPO_ROOT/uros/build"
KERNEL="$BUILD_DIR/export/uros/boot/mach_kernel"
BOOTSTRAP="$BUILD_DIR/export/uros/$(uname -m)/user/sbin/bootstrap"
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
USE_VIRTIO=false
USE_BUNDLE=true     # Issue #186: stage-1 multiboot bundle (mod[1]) on by default
USE_SHA_NI=false    # Issue #180: --sha-ni → TCG + Icelake-Server,+sha-ni
FRESH_DISK=false    # --fresh-disk: regen disk.img before launch (avoids stale
                    # stage-2 binaries after rebuild + ext2 writeback corruption
                    # from previous ungraceful QEMU exit)
BENCH_ARGS=""
EXTRA_ARGS=""
MINIMAL_ARG=""
NO_REBOOT="$NO_REBOOT"
while [ $# -gt 0 ]; do
    case "$1" in
        --no-disk) USE_DISK=false; USE_AHCI=false; shift ;;
        --no-bundle) USE_BUNDLE=false; shift ;;
        --ahci2) USE_AHCI=true; USE_AHCI2=true; shift ;;
        --ahci) USE_AHCI=true; shift ;;
        --virtio) USE_VIRTIO=true; shift ;;
        --sha-ni) USE_SHA_NI=true; shift ;;
        --fresh-disk) FRESH_DISK=true; shift ;;
        --minimal) MINIMAL_ARG="--minimal"; FRESH_DISK=true; shift ;;
        --allow-reboot) NO_REBOOT=""; shift ;;
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

# Regenerate disk image if --bench was specified (pass suite selection)
# or if --fresh-disk was explicitly requested.  --bench implies fresh.
if [ -n "$BENCH_ARGS" ]; then
    echo "Bench suites:$BENCH_ARGS"
    "$REPO_ROOT/scripts/make-disk-image.sh" --bench $BENCH_ARGS $MINIMAL_ARG
elif [ "$FRESH_DISK" = true ]; then
    echo "Regenerating disk image (--fresh-disk$([ -n "$MINIMAL_ARG" ] && echo " --minimal"))…"
    "$REPO_ROOT/scripts/make-disk-image.sh" $MINIMAL_ARG
fi

# Issue #186: (re)build the stage-1 bundle so its bootstrap.conf and
# binaries stay in sync with the on-disk copy (especially with --bench).
if [ "$USE_BUNDLE" = true ]; then
    if [ -n "$BENCH_ARGS" ]; then
        "$REPO_ROOT/scripts/make-bundle.sh" --bench $BENCH_ARGS $MINIMAL_ARG
    else
        "$REPO_ROOT/scripts/make-bundle.sh" $MINIMAL_ARG
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
if [ "$USE_SHA_NI" = true ]; then
    QEMU_ARGS="-m 512M -accel tcg -cpu Icelake-Server,+sha-ni -kernel $KERNEL -initrd $INITRD $NO_REBOOT"
    echo "Acceleration: TCG (--sha-ni: Icelake-Server,+sha-ni)"
else
    QEMU_ARGS="-m 512M -enable-kvm -cpu host -kernel $KERNEL -initrd $INITRD $NO_REBOOT"
fi

# Issue #224: stage-2 e default_pager girano interamente su AHCI; il
# driver IDE in-kernel non serve più.  La disk.img prodotta da
# make-disk-image.sh ha layout MBR/3-partizioni (disk0a /mach_servers/,
# disk0b test data, disk0c swap) e viene attaccata direttamente come
# AHCI port 0.
#
# create_ahci_test_disk <path> <label> — secondario, solo per multi-mount.
# Disco da 40 MB con due partizioni ext2 (hello.txt diverso) + raw swap,
# senza /mach_servers/.
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

if [ "$USE_AHCI" = true ]; then
    if [ ! -f "$DISK_IMG" ]; then
        echo "ERRORE: $DISK_IMG non trovato — esegui ./scripts/make-disk-image.sh prima"
        exit 1
    fi
    echo "AHCI port 0: $DISK_IMG (contiene /mach_servers/, hello.txt, swap)"
    QEMU_ARGS="$QEMU_ARGS -device ich9-ahci,id=ahci0"
    QEMU_ARGS="$QEMU_ARGS -drive id=ahcidisk0,file=$DISK_IMG,format=raw,if=none"
    QEMU_ARGS="$QEMU_ARGS -device ide-hd,drive=ahcidisk0,bus=ahci0.0"

    if [ "$USE_AHCI2" = true ]; then
        AHCI_DISK1="$BUILD_DIR/ahci-test1.img"
        create_ahci_test_disk "$AHCI_DISK1" "disk1"
        QEMU_ARGS="$QEMU_ARGS -drive id=ahcidisk1,file=$AHCI_DISK1,format=raw,if=none"
        QEMU_ARGS="$QEMU_ARGS -device ide-hd,drive=ahcidisk1,bus=ahci0.1"
        echo "AHCI port 1: $AHCI_DISK1"
    fi
elif [ "$USE_DISK" = true ]; then
    echo "ATTENZIONE: disco richiesto ma --ahci non attivo; bootstrap stage-2"
    echo "  non potrà aprire disk0a — avvio degraded (solo bundle stage-1)."
fi

# Optionally add a virtio-blk-pci device with a test disk.
# The boot disk stays on IDE; the virtio-blk controller appears as a
# separate PCI device that the virtio_blk driver can detect and probe.
#
# Same layout as the AHCI test disk (Issue #184): MBR + 4 MB ext2 + 4 MB
# ext2 + ~32 MB raw swap, so default_pager finds "disk0c" regardless of
# whether the backing module is AHCI or virtio-blk.
VIRTIO_DISK="$BUILD_DIR/virtio-test.img"
if [ "$USE_VIRTIO" = true ]; then
    # Always recreate to ensure correct layout
    create_ahci_disk "$VIRTIO_DISK" "disk0"
    echo "Virtio-blk: $VIRTIO_DISK"
    QEMU_ARGS="$QEMU_ARGS -drive id=virtiodisk0,file=$VIRTIO_DISK,format=raw,if=none"
    QEMU_ARGS="$QEMU_ARGS -device virtio-blk-pci,drive=virtiodisk0"
fi

# shellcheck disable=SC2086
exec qemu-system-i386 $QEMU_ARGS $EXTRA_ARGS
