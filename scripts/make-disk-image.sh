#!/usr/bin/env bash
# ==========================================================================
# make-disk-image.sh — Crea un'immagine disco per OSFMK/Uros (#224)
#
# Genera un disco MBR con tre partizioni, layout compatibile con il
# block_device_server modulare (ahci.so) — niente più disco IDE:
#   disk0a (MBR entry 0) — ext2  — /mach_servers/ con config e binari
#   disk0b (MBR entry 1) — ext2  — hello.txt + bench.dat (test data)
#   disk0c (MBR entry 2) — raw   — paging/swap per il default_pager
#
# Il driver AHCI userspace (block_device_server/modules/ahci.so) mappa
# le entry MBR 1:1 su 'ahci0[abc]' e BDS le ri-pubblica come 'disk0[abc]'.
# Stage-2 bootstrap (Issue #185) e default_pager (argv="disk0c") aprono
# queste partizioni via cap_request + device_open_cap.
#
# Uso:
#   ./scripts/make-disk-image.sh                    # default 40 MB
#   ./scripts/make-disk-image.sh -o disk.img        # path output custom
#   ./scripts/make-disk-image.sh -s 64              # dimensione totale in MB
#
# Requisiti: sfdisk, mke2fs, debugfs (pacchetti: fdisk, e2fsprogs)
# Non richiede root/sudo.
# ==========================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Overridable, like run-qemu.sh: a measurement must be able to run against a
# tree nobody else is rebuilding (#485).  ⚠️ It has to be honoured in EVERY
# script the run touches -- run-qemu.sh alone was not enough, and the first
# attempt booted a kernel from the measurement tree with a bundle from the
# working one.
BUILD_DIR="${UROS_BUILD_DIR:-$REPO_ROOT/uros/build}"
# #404: the build stamps its target arch; uname -m names the HOST
ARCH="$(cat "$BUILD_DIR/uros-arch" 2>/dev/null || uname -m)"

# --- Parametri di default ---
IMG_SIZE_MB=512
DISK_IMG="$BUILD_DIR/disk.img"
SECT_SIZE=512
PART0_START_SECT=2048        # disk0a — /mach_servers/, 1 MiB aligned
FS0_SIZE_MB=128              # 8 -> 128 MB (#266): 8x the 16 MB DMA page cache,
                             # generous headroom for content growth
PART1_START_SECT=264192      # disk0b — test data (2048 + 128 MiB)
FS1_SIZE_MB=8
PART2_START_SECT=280576      # disk0c — raw swap (264192 + 8 MiB) -> ~375 MB


# ipc_bench suite selection (empty = all)
BENCH_ARGS=""
MINIMAL=0

# --- Parse argomenti ---
while [ $# -gt 0 ]; do
    case "$1" in
        -o) DISK_IMG="$2"; shift 2 ;;
        -s) IMG_SIZE_MB="$2"; shift 2 ;;
        --bench)
            shift
            while [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; do
                BENCH_ARGS="$BENCH_ARGS $1"
                shift
            done
            ;;
        --minimal) MINIMAL=1; shift ;;
        -h|--help)
            echo "Uso: $0 [-o output.img] [-s size_mb] [--bench suite ...] [--minimal]"
            echo ""
            echo "  --bench suite ...   Passa suite names a ipc_bench"
            echo "                      (syscall intra slow inter comb port pp ool flipc2 all)"
            echo "  --minimal           Omit hello_server/ipc_bench/pthread_test/cap_test/"
            echo "                      gpustat from disk bootstrap.conf (stage-2)"
            exit 0
            ;;
        *) echo "Opzione sconosciuta: $1" >&2; exit 1 ;;
    esac
done

# --- Verifica dipendenze ---
for cmd in dd sfdisk mke2fs debugfs; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERRORE: '$cmd' non trovato."
        echo "  Su Debian/Ubuntu: sudo apt install fdisk e2fsprogs"
        echo "  Su Fedora:        sudo dnf install util-linux e2fsprogs"
        exit 1
    fi
done

# --- Percorsi dei binari ---
NAME_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/name_server"
DEFAULT_PAGER="$BUILD_DIR/export/uros/$ARCH/user/sbin/default_pager"
HELLO_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/hello_server"
IPC_BENCH="$BUILD_DIR/export/uros/$ARCH/user/sbin/ipc_bench"
BLOCK_DEVICE_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/block_device_server"
AHCI_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/block/ahci.so"
VIRTIO_BLK_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/block/virtio_blk.so"
HAL_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/hal_server"
HAL_PCI_SCAN_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/hal/pci_scan.so"
GPU_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/gpu_server"
GPU_VGA_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/gpu/vga.so"
CHAR_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/char_server"
CHAR_PS2_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/char/ps2.so"
EXT2_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/ext_server"
PTHREAD_TEST="$BUILD_DIR/export/uros/$ARCH/user/sbin/pthread_test"
CAP_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/cap_server"
CAP_TEST="$BUILD_DIR/export/uros/$ARCH/user/sbin/cap_test"
GPUSTAT="$BUILD_DIR/export/uros/$ARCH/user/sbin/gpustat"
EXEC_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/exec_server"
HELLO_EXEC="$BUILD_DIR/export/uros/$ARCH/user/sbin/hello_exec"
FD_EXEC_TEST="$BUILD_DIR/export/uros/$ARCH/user/sbin/fd_exec_test"
HELLO_WORLD="$BUILD_DIR/export/uros/$ARCH/user/sbin/hello_world"
PTHREAD_MIN="$BUILD_DIR/export/uros/$ARCH/user/sbin/pthread_min"  # #291 musl pthread regression
FLIPC_BENCH="$BUILD_DIR/export/uros/$ARCH/user/sbin/flipc_bench"  # #272 standalone FLIPC A/B
CPUSTAT="$BUILD_DIR/export/uros/$ARCH/user/sbin/cpustat"          # #375 per-CPU load monitor (dynamic)
USH="$BUILD_DIR/export/uros/$ARCH/user/sbin/ush"
VTS="$BUILD_DIR/export/uros/$ARCH/user/sbin/virtual_terminal_server"  # #365 VT shell supervisor
PROC_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/proc_server"
# #234 Phase 7: dynamic linker + first dynamic binary.  ld-musl-i386.so.1 is
# the umbrella libc.so; it goes to /lib/ where hello_dyn's PT_INTERP points.
HELLO_DYN="$BUILD_DIR/export/uros/$ARCH/user/sbin/hello_dyn"
# #289: dynamic twin of hello_world (printf + return) — verifies the
# dynamic clean-exit/reap path.  Same PT_INTERP as hello_dyn.
HELLO_DYN_WORLD="$BUILD_DIR/export/uros/$ARCH/user/sbin/hello_dyn_world"
MUSL_LDSO="$BUILD_DIR/src/contrib/musl-install/lib/libc.so"
# #234 Phase 7 incr 4: dlopen end-to-end test.  dlopen_test goes to / next
# to hello_dyn; libfoo.so goes to /lib/ where dlopen("/lib/libfoo.so") finds it.
DLOPEN_TEST="$BUILD_DIR/export/uros/$ARCH/user/sbin/dlopen_test"
LIBFOO_SO="$BUILD_DIR/export/uros/$ARCH/user/lib/libfoo.so"

if [ ! -f "$NAME_SERVER" ]; then
    echo "ERRORE: name_server non trovato: $NAME_SERVER"
    echo "  Build con: cd $BUILD_DIR && ninja name_server_bin"
    exit 1
fi

if [ ! -f "$DEFAULT_PAGER" ]; then
    echo "ERRORE: default_pager non trovato: $DEFAULT_PAGER"
    echo "  Build con: cd $BUILD_DIR && ninja default_pager_server"
    exit 1
fi

if [ ! -f "$HELLO_SERVER" ]; then
    echo "ERRORE: hello_server non trovato: $HELLO_SERVER"
    echo "  Build con: cd $BUILD_DIR && ninja hello_server_server"
    exit 1
fi

if [ ! -f "$IPC_BENCH" ]; then
    echo "ERRORE: ipc_bench non trovato: $IPC_BENCH"
    echo "  Build con: cd $BUILD_DIR && ninja ipc_bench_server"
    exit 1
fi

if [ ! -f "$BLOCK_DEVICE_SERVER" ]; then
    echo "ERRORE: block_device_server non trovato: $BLOCK_DEVICE_SERVER"
    echo "  Build con: cd $BUILD_DIR && ninja block_device_server"
    exit 1
fi

if [ ! -f "$AHCI_MODULE" ]; then
    echo "ERRORE: ahci.so non trovato: $AHCI_MODULE"
    echo "  Build con: cd $BUILD_DIR && ninja ahci_module"
    exit 1
fi

if [ ! -f "$VIRTIO_BLK_MODULE" ]; then
    echo "ERRORE: virtio_blk.so non trovato: $VIRTIO_BLK_MODULE"
    echo "  Build con: cd $BUILD_DIR && ninja virtio_blk_module"
    exit 1
fi

if [ ! -f "$HAL_SERVER" ]; then
    echo "ERRORE: hal_server non trovato: $HAL_SERVER"
    echo "  Build con: cd $BUILD_DIR && ninja hal_server"
    exit 1
fi

if [ ! -f "$HAL_PCI_SCAN_MODULE" ]; then
    echo "ERRORE: pci_scan.so non trovato: $HAL_PCI_SCAN_MODULE"
    echo "  Build con: cd $BUILD_DIR && ninja hal_pci_scan_module"
    exit 1
fi

if [ ! -f "$EXT2_SERVER" ]; then
    echo "ERRORE: ext_server non trovato: $EXT2_SERVER"
    echo "  Build con: cd $BUILD_DIR && ninja ext_server_bin"
    exit 1
fi

if [ ! -f "$PTHREAD_TEST" ]; then
    echo "ERRORE: pthread_test non trovato: $PTHREAD_TEST"
    echo "  Build con: cd $BUILD_DIR && ninja pthread_test_server"
    exit 1
fi

# --- File di configurazione del bootstrap ---
# Formato: <symtab_name> <path> [args...]
# Il path relativo viene risolto come /dev/boot_device/mach_servers/<path>
# L'argomento "hd0b" dopo il path diventa argv[1] del default_pager,
# che lo apre con device_open() e lo usa come backing store di paging.
BOOTSTRAP_CONF=$(mktemp)
# cap_server (if built) goes right after name_server: it publishes its
# port via netname_check_in so the name_server must be up first.
CAP_SERVER_CONF_LINE=""
if [ -f "$CAP_SERVER" ]; then
    CAP_SERVER_CONF_LINE="cap_server cap_server"
fi
# cap_test (if built) must run AFTER cap_server has registered the
# HMAC key: the test's whole point is that a non-cap_server task
# attempting urmach_cap_register must be rejected.
CAP_TEST_CONF_LINE=""
if [ -f "$CAP_TEST" ]; then
    CAP_TEST_CONF_LINE="cap_test cap_test"
fi
EXEC_SERVER_CONF_LINE=""
if [ -f "$EXEC_SERVER" ]; then
    EXEC_SERVER_CONF_LINE="exec_server exec_server"
fi
PROC_SERVER_CONF_LINE=""
if [ -f "$PROC_SERVER" ]; then
    PROC_SERVER_CONF_LINE="proc_server proc_server"
fi
# gpustat (if built) runs after gpu_server / cap_server are up so it
# can netname_look_up "gpu" and cap_request DEV_ADMIN.  It is a
# single-shot probe (#203): one printf with the counters, then exits.
GPUSTAT_CONF_LINE=""
if [ -f "$GPUSTAT" ]; then
    GPUSTAT_CONF_LINE="gpustat gpustat"
fi
# ush (#275.5): Uros shell.  Needs proc_server + char_server + ext_server
# up so it can setsid, acquire ctty, and open /dev/tty.
#
# #365: when the virtual_terminal_server is present it becomes the last
# stage-2 task instead of ush; it forks ush onto each virtual terminal.
# ush stays on disk either way (the supervisor execs /mach_servers/ush);
# only the launch line moves from "ush ush" to the supervisor.
USH_CONF_LINE=""
if [ -f "$VTS" ]; then
    USH_CONF_LINE="virtual_terminal_server virtual_terminal_server"
elif [ -f "$USH" ]; then
    USH_CONF_LINE="ush ush"
fi
#
# Issue #184: default_pager ora apre la sua partizione di swap via BDS
# (cap_request + device_open_cap) anziché via il driver IDE in-kernel.
# Conseguenza sull'ordine: hal_server e block_device_server devono
# partire PRIMA di default_pager, altrimenti blk_open("disk0c") bloccherebbe
# su netname_notify in attesa che BDS pubblichi la partizione.
#
GPU_SERVER_CONF_LINE=""
[ -f "$GPU_SERVER" ] && GPU_SERVER_CONF_LINE="gpu_server gpu_server"
CHAR_SERVER_CONF_LINE=""
[ -f "$CHAR_SERVER" ] && CHAR_SERVER_CONF_LINE="char_server char_server"

if [ "$MINIMAL" = "1" ]; then
    HELLO_SERVER_LINE=""
    IPC_BENCH_LINE=""
    PTHREAD_TEST_LINE=""
    CAP_TEST_CONF_LINE=""
    GPUSTAT_CONF_LINE=""
else
    HELLO_SERVER_LINE="hello_server hello_server"
    IPC_BENCH_LINE="ipc_bench ipc_bench${BENCH_ARGS}"
    PTHREAD_TEST_LINE="pthread_test pthread_test"
fi

# ⚠️ Il name server NON porta un flag `-w' qui: bootstrap lo aspetta da
# solo (#492).  La decisione sta in parse_config_file(), perche' questo
# file e' uno di QUATTRO che scrivono la stessa riga.
cat > "$BOOTSTRAP_CONF" <<CONF
name_server name_server
${CAP_SERVER_CONF_LINE}
${GPU_SERVER_CONF_LINE}
${CHAR_SERVER_CONF_LINE}
hal_server hal_server
block_device_server block_device_server
default_pager default_pager disk0c
${HELLO_SERVER_LINE}
ext_server ext_server
${EXEC_SERVER_CONF_LINE}
${PROC_SERVER_CONF_LINE}
${IPC_BENCH_LINE}
${PTHREAD_TEST_LINE}
${CAP_TEST_CONF_LINE}
${GPUSTAT_CONF_LINE}
${USH_CONF_LINE}
CONF

# --- Calcoli geometria ---
TOTAL_SECTS=$((IMG_SIZE_MB * 1024 * 1024 / SECT_SIZE))
FS0_SIZE_SECTS=$((FS0_SIZE_MB * 1024 * 1024 / SECT_SIZE))
FS1_SIZE_SECTS=$((FS1_SIZE_MB * 1024 * 1024 / SECT_SIZE))
SWAP_SIZE_SECTS=$((TOTAL_SECTS - PART2_START_SECT))
SWAP_SIZE_MB=$((SWAP_SIZE_SECTS * SECT_SIZE / 1024 / 1024))

echo "=== Creazione immagine disco OSFMK ==="
echo "  Output:      $DISK_IMG"
echo "  Dimensione:  ${IMG_SIZE_MB} MB"
echo "  disk0a: ext2, ${FS0_SIZE_MB} MB  — /mach_servers/"
echo "  disk0b: ext2, ${FS1_SIZE_MB} MB  — hello.txt + bench.dat (test data)"
echo "  disk0c: raw,  ${SWAP_SIZE_MB} MB — paging/swap"
echo ""

# --- 1. Immagine vuota ---
echo "[1/6] Creazione immagine vuota (${IMG_SIZE_MB} MB)..."
dd if=/dev/zero of="$DISK_IMG" bs=1M count="$IMG_SIZE_MB" status=none

# --- 2. Tabella partizioni MBR ---
echo "[2/6] Scrittura tabella partizioni MBR (3 entries)..."
sfdisk --quiet "$DISK_IMG" <<EOF
label: dos
start=$PART0_START_SECT, size=$FS0_SIZE_SECTS, type=83
start=$PART1_START_SECT, size=$FS1_SIZE_SECTS, type=83
start=$PART2_START_SECT, size=$SWAP_SIZE_SECTS, type=82
EOF

# --- 3. Formattare le due partizioni ext2 ---
echo "[3/6] Formattazione ext2 (disk0a + disk0b)..."
PART_IMG=$(mktemp /tmp/osfmk-part.XXXXXX.img)
PART1_IMG=$(mktemp /tmp/osfmk-part1.XXXXXX.img)
trap 'rm -f "$PART_IMG" "$PART1_IMG" "$BOOTSTRAP_CONF"' EXIT

dd if=/dev/zero of="$PART_IMG" bs="$SECT_SIZE" count="$FS0_SIZE_SECTS" status=none
mke2fs -t ext2 -q -F \
    -b 4096 \
    -I 256 \
    -r 1 \
    -L "mach_servers" \
    -O filetype \
    "$PART_IMG"

dd if=/dev/zero of="$PART1_IMG" bs="$SECT_SIZE" count="$FS1_SIZE_SECTS" status=none
mke2fs -t ext2 -q -F \
    -b 4096 \
    -I 256 \
    -r 1 \
    -L "disk0b" \
    -O filetype \
    "$PART1_IMG"

# --- 4. Copia file nel filesystem con debugfs ---
# cap_server and cap_test are optional: write only when the binaries
# exist so boot images still build when the corresponding flags are off.
CAP_SERVER_WRITE_LINE=""
if [ -f "$CAP_SERVER" ]; then
    CAP_SERVER_WRITE_LINE="write $CAP_SERVER cap_server"
fi
CAP_TEST_WRITE_LINE=""
if [ -f "$CAP_TEST" ]; then
    CAP_TEST_WRITE_LINE="write $CAP_TEST cap_test"
fi
GPUSTAT_WRITE_LINE=""
if [ -f "$GPUSTAT" ]; then
    GPUSTAT_WRITE_LINE="write $GPUSTAT gpustat"
fi
EXEC_SERVER_WRITE_LINE=""
if [ -f "$EXEC_SERVER" ]; then
    EXEC_SERVER_WRITE_LINE="write $EXEC_SERVER exec_server"
fi
PROC_SERVER_WRITE_LINE=""
if [ -f "$PROC_SERVER" ]; then
    PROC_SERVER_WRITE_LINE="write $PROC_SERVER proc_server"
fi
echo "[4/6] Copia file nel filesystem ext2..."
# ipc_bench's disk_bench tests open hello.txt / bench.dat at the root
# of the default ext2 mount (ext_server → ahci0a / hd0a), so seed both
# files here.  bench.dat only needs the first 64 bytes — a 1 KB blob
# is plenty and keeps the partition small.
HELLO_TXT=$(mktemp)
POSIX_SMOKE=$(mktemp)
BENCH_DAT=$(mktemp)
BENCH_LARGE=$(mktemp)
BENCH_4M=$(mktemp)
printf 'Hello from /mach_servers/ root\n' > "$HELLO_TXT"
# Read-only fixture for hello_server's POSIX fd-layer smoke (#262).
# Kept separate from hello.txt, which disk_bench uses as a write
# scratch file — so the smoke passes on a non-fresh disk too.
printf 'libposix-uros POSIX fd layer smoke\n' > "$POSIX_SMOKE"
dd if=/dev/urandom of="$BENCH_DAT" bs=1K count=1 status=none
# bench_large.dat (#267): 12 MB — bigger than the old 4 MB page cache but
# within the 16 MB DMA cache, so disk_bench can show a cold-vs-warm
# (miss-vs-hit) read speedup that the old cache could not cache whole.
dd if=/dev/urandom of="$BENCH_LARGE" bs=1M count=12 status=none
# bench_4m.dat (#267): 4 MB — apples-to-apples with the historical
# file-pool cached-read baseline (~930 MB/s at 64 KB, warm).
dd if=/dev/urandom of="$BENCH_4M" bs=1M count=4 status=none
trap 'rm -f "$PART_IMG" "$BOOTSTRAP_CONF" "$HELLO_TXT" "$POSIX_SMOKE" "$BENCH_DAT" "$BENCH_LARGE" "$BENCH_4M"' EXIT

# hello_exec is optional (#228 v0.1.0): copy to / so exec_server can
# load "/hello_exec" via libvfs.
HELLO_EXEC_WRITE_LINE=""
if [ -f "$HELLO_EXEC" ]; then
    HELLO_EXEC_WRITE_LINE="write $HELLO_EXEC hello_exec"
fi

# fd_exec_test is optional (#262 step 3): copy to / so hello_server can
# fork+execve "/fd_exec_test" and prove fds survive across exec.
FD_EXEC_TEST_WRITE_LINE=""
if [ -f "$FD_EXEC_TEST" ]; then
    FD_EXEC_TEST_WRITE_LINE="write $FD_EXEC_TEST fd_exec_test"
fi

# hello_world (#286): standard printf hello world; copy to / so ush can
# launch "/hello_world" and prove stdout reaches the controlling tty.
HELLO_WORLD_WRITE_LINE=""
if [ -f "$HELLO_WORLD" ]; then
    HELLO_WORLD_WRITE_LINE="write $HELLO_WORLD hello_world"
fi

# pthread_min (#291): minimal musl pthread regression; copy to / so ush can
# launch "/pthread_min".
PTHREAD_MIN_WRITE_LINE=""
if [ -f "$PTHREAD_MIN" ]; then
    PTHREAD_MIN_WRITE_LINE="write $PTHREAD_MIN pthread_min"
fi

# flipc_bench (#272): standalone libvfs FLIPC A/B benchmark; copy to / so
# ush can launch "/flipc_bench" on a quiescent system after boot.
FLIPC_BENCH_WRITE_LINE=""
if [ -f "$FLIPC_BENCH" ]; then
    FLIPC_BENCH_WRITE_LINE="write $FLIPC_BENCH flipc_bench"
fi

# ush is optional (#275.5): copy to /mach_servers/ so bootstrap stage-2
# can load it as a session-leader after proc_server / char_server.  #365:
# the supervisor execs it from here, so it is written even when the direct
# "ush ush" launch line has been replaced by virtual_terminal_server.
USH_WRITE_LINE=""
if [ -f "$USH" ]; then
    USH_WRITE_LINE="write $USH ush"
fi
# virtual_terminal_server (#365): VT shell supervisor, written alongside ush.
VTS_WRITE_LINE=""
if [ -f "$VTS" ]; then
    VTS_WRITE_LINE="write $VTS virtual_terminal_server"
fi

# hello_dyn (#234 Phase 7): first dynamic binary.  Copy to / and install the
# interpreter (umbrella libc.so) at /lib/ld-musl-i386.so.1 where its
# PT_INTERP points.  Both optional (need UROS_BUILD_MUSL).
HELLO_DYN_WRITE_LINE=""
LDSO_MKDIR_LINE=""
if [ -f "$HELLO_DYN" ] && [ -f "$MUSL_LDSO" ]; then
    HELLO_DYN_WRITE_LINE="write $HELLO_DYN hello_dyn"
    LDSO_MKDIR_LINE="mkdir lib"   # ld-musl + libfoo written into it below
fi

# hello_dyn_world (#289): dynamic clean-exit verification binary.  Needs the
# same interpreter as hello_dyn, so gate it on the ld-musl presence too.
HELLO_DYN_WORLD_WRITE_LINE=""
if [ -f "$HELLO_DYN_WORLD" ] && [ -f "$MUSL_LDSO" ]; then
    HELLO_DYN_WORLD_WRITE_LINE="write $HELLO_DYN_WORLD hello_dyn_world"
    LDSO_MKDIR_LINE="mkdir lib"   # ensure /lib exists even if hello_dyn absent
fi

# dlopen_test (#234 Phase 7 incr 4): dynamic binary that dlopens libfoo.so.
DLOPEN_TEST_WRITE_LINE=""
if [ -f "$DLOPEN_TEST" ] && [ -f "$MUSL_LDSO" ]; then
    DLOPEN_TEST_WRITE_LINE="write $DLOPEN_TEST dlopen_test"
fi

# cpustat (#375): per-CPU load monitor, the first real dynamic tool.  Copy to
# / so ush can launch "/cpustat"; needs the same interpreter as hello_dyn.
CPUSTAT_WRITE_LINE=""
if [ -f "$CPUSTAT" ] && [ -f "$MUSL_LDSO" ]; then
    CPUSTAT_WRITE_LINE="write $CPUSTAT cpustat"
    LDSO_MKDIR_LINE="mkdir lib"   # ensure /lib exists for the interpreter
fi

debugfs -w -f /dev/stdin "$PART_IMG" <<DBGFS 2>/dev/null
write $HELLO_TXT hello.txt
write $POSIX_SMOKE posix_smoke.txt
write $BENCH_DAT bench.dat
write $BENCH_LARGE bench_large.dat
write $BENCH_4M bench_4m.dat
${HELLO_EXEC_WRITE_LINE}
${FD_EXEC_TEST_WRITE_LINE}
${HELLO_WORLD_WRITE_LINE}
${PTHREAD_MIN_WRITE_LINE}
${FLIPC_BENCH_WRITE_LINE}
${HELLO_DYN_WRITE_LINE}
${HELLO_DYN_WORLD_WRITE_LINE}
${DLOPEN_TEST_WRITE_LINE}
${CPUSTAT_WRITE_LINE}
${LDSO_MKDIR_LINE}
mkdir mach_servers
cd mach_servers
write $BOOTSTRAP_CONF bootstrap.conf
write $NAME_SERVER name_server
write $DEFAULT_PAGER default_pager
write $HELLO_SERVER hello_server
write $IPC_BENCH ipc_bench
write $BLOCK_DEVICE_SERVER block_device_server
write $HAL_SERVER hal_server
write $EXT2_SERVER ext_server
write $PTHREAD_TEST pthread_test
${CAP_SERVER_WRITE_LINE}
${CAP_TEST_WRITE_LINE}
${GPUSTAT_WRITE_LINE}
${EXEC_SERVER_WRITE_LINE}
${PROC_SERVER_WRITE_LINE}
${USH_WRITE_LINE}
${VTS_WRITE_LINE}
mkdir modules
cd modules
mkdir block
cd block
write $AHCI_MODULE ahci.so
write $VIRTIO_BLK_MODULE virtio_blk.so
cd ..
mkdir hal
cd hal
write $HAL_PCI_SCAN_MODULE pci_scan.so
DBGFS

# #234 Phase 7: install the dynamic linker (umbrella libc.so) at
# /lib/ld-musl-i386.so.1 — where the dynamic binaries' PT_INTERP points.
# Separate debugfs pass so the /lib cd/write doesn't tangle with the main
# heredoc.  Installed if any dynamic binary (hello_dyn / hello_dyn_world)
# is present.
if { [ -f "$HELLO_DYN" ] || [ -f "$HELLO_DYN_WORLD" ]; } && [ -f "$MUSL_LDSO" ]; then
    # libfoo.so is optional; only write it if the build produced it
    # (incr 4 may be temporarily out-of-tree on side branches).
    LIBFOO_WRITE_LINE=""
    if [ -f "$LIBFOO_SO" ]; then
        LIBFOO_WRITE_LINE="write $LIBFOO_SO libfoo.so"
    fi
    debugfs -w -f /dev/stdin "$PART_IMG" 2>/dev/null <<DBGFS
cd /lib
write $MUSL_LDSO ld-musl-i386.so.1
${LIBFOO_WRITE_LINE}
DBGFS
    if [ -f "$LIBFOO_SO" ]; then
        echo "  / : hello_dyn + dlopen_test + /lib/{ld-musl-i386.so.1, libfoo.so} (#234)"
    else
        echo "  / : hello_dyn + /lib/ld-musl-i386.so.1 (dynamic linker, #234)"
    fi
fi

# gpu_server is optional in 0.1.0 (OSFMK_BUILD_GPU_SERVER off by default)
if [ -f "$GPU_SERVER" ] && [ -f "$GPU_VGA_MODULE" ]; then
    debugfs -w -f - "$PART_IMG" 2>/dev/null <<DBGFS
cd /mach_servers
write $GPU_SERVER gpu_server
cd modules
mkdir gpu
cd gpu
write $GPU_VGA_MODULE vga.so
DBGFS
    echo "  /mach_servers/gpu_server                  → $(stat -c%s "$GPU_SERVER") bytes"
    echo "  /mach_servers/modules/gpu/vga.so          → $(stat -c%s "$GPU_VGA_MODULE") bytes"
fi

# char_server is optional in 0.1.0 (OSFMK_BUILD_CHAR_SERVER off by default).
# /mach_servers/modules/char/ is filled by #206 (ps2.so) + #207 (uart.so);
# this skeleton ships only the executable.
if [ -f "$CHAR_SERVER" ]; then
    debugfs -w -f - "$PART_IMG" 2>/dev/null <<DBGFS
cd /mach_servers
write $CHAR_SERVER char_server
cd modules
mkdir char
DBGFS
    echo "  /mach_servers/char_server                 → $(stat -c%s "$CHAR_SERVER") bytes"
    if [ -f "$CHAR_PS2_MODULE" ]; then
        debugfs -w -f - "$PART_IMG" 2>/dev/null <<DBGFS
cd /mach_servers/modules/char
write $CHAR_PS2_MODULE ps2.so
DBGFS
        echo "  /mach_servers/modules/char/ps2.so         → $(stat -c%s "$CHAR_PS2_MODULE") bytes"
    fi
fi

echo "  /mach_servers/bootstrap.conf → 'name_server name_server'"
echo "  /mach_servers/bootstrap.conf → 'cap_server cap_server'"
echo "  /mach_servers/bootstrap.conf → 'hal_server hal_server'"
echo "  /mach_servers/bootstrap.conf → 'block_device_server block_device_server'"
echo "  /mach_servers/bootstrap.conf → 'default_pager default_pager disk0c'"
echo "  /mach_servers/bootstrap.conf → 'hello_server hello_server'"
echo "  /mach_servers/bootstrap.conf → 'ipc_bench ipc_bench'"
echo "  /mach_servers/bootstrap.conf → 'ext_server ext_server'"
echo "  /mach_servers/name_server    → $(stat -c%s "$NAME_SERVER") bytes"
echo "  /mach_servers/default_pager  → $(stat -c%s "$DEFAULT_PAGER") bytes"
echo "  /mach_servers/hello_server   → $(stat -c%s "$HELLO_SERVER") bytes"
echo "  /mach_servers/ipc_bench      → $(stat -c%s "$IPC_BENCH") bytes"
echo "  /mach_servers/block_device_server → $(stat -c%s "$BLOCK_DEVICE_SERVER") bytes"
echo "  /mach_servers/modules/block/ahci.so       → $(stat -c%s "$AHCI_MODULE") bytes"
echo "  /mach_servers/modules/block/virtio_blk.so → $(stat -c%s "$VIRTIO_BLK_MODULE") bytes"
echo "  /mach_servers/hal_server    → $(stat -c%s "$HAL_SERVER") bytes"
echo "  /mach_servers/modules/hal/pci_scan.so     → $(stat -c%s "$HAL_PCI_SCAN_MODULE") bytes"
echo "  /mach_servers/ext_server    → $(stat -c%s "$EXT2_SERVER") bytes"

# --- 4b. Popola disk0b con hello.txt (test data) ---
DBHELLO=$(mktemp)
printf 'Hello from disk0b partition\n' > "$DBHELLO"
debugfs -w -f /dev/stdin "$PART1_IMG" <<DBGFS 2>/dev/null
write $DBHELLO hello.txt
DBGFS
rm -f "$DBHELLO"

# --- 5. Inserimento delle due partizioni ext2 nell'immagine disco ---
echo "[5/6] Assemblaggio partizioni ext2 (disk0a + disk0b)..."
dd if="$PART_IMG"  of="$DISK_IMG" bs="$SECT_SIZE" seek="$PART0_START_SECT" conv=notrunc status=none
dd if="$PART1_IMG" of="$DISK_IMG" bs="$SECT_SIZE" seek="$PART1_START_SECT" conv=notrunc status=none

# --- 6. La partizione swap è già zero-filled (nessun formato necessario) ---
echo "[6/6] Partizione swap (disk0c) pronta (zero-filled)."

echo ""
echo "=== Immagine disco creata con successo ==="
echo "  $DISK_IMG ($(stat -c%s "$DISK_IMG") bytes)"
echo ""
echo "Struttura disco (${IMG_SIZE_MB} MB, AHCI):"
echo "  MBR:     settore 0"
echo "  disk0a:  settori ${PART0_START_SECT}-$((PART0_START_SECT + FS0_SIZE_SECTS - 1))  (ext2, ${FS0_SIZE_MB} MB)"
echo "    └── /mach_servers/ + hello.txt + bench.dat"
echo "  disk0b:  settori ${PART1_START_SECT}-$((PART1_START_SECT + FS1_SIZE_SECTS - 1))  (ext2, ${FS1_SIZE_MB} MB) — test data"
echo "  disk0c:  settori ${PART2_START_SECT}-$((PART2_START_SECT + SWAP_SIZE_SECTS - 1))  (raw,  ${SWAP_SIZE_MB} MB) — paging"
echo ""
echo "Flusso di boot (#224 — no IDE):"
echo "  1. Stage-1: kernel mappa bootstrap.bundle → bootstrap legge i binari da RAM"
echo "  2. Stage-2: bootstrap apre disk0a via BDS (cap_request + device_open_cap)"
echo "  3. default_pager argv[1]='disk0c' → ${SWAP_SIZE_MB} MB swap via BDS"
echo ""
echo "Per avviare:"
echo "  ./scripts/run-qemu.sh"
