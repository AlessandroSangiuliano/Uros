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
BUILD_DIR="$REPO_ROOT/uros/build"
ARCH="$(uname -m)"

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
        -h|--help)
            echo "Uso: $0 [-o output.img] [-s size_mb] [--bench suite ...]"
            echo ""
            echo "  --bench suite ...   Passa suite names a ipc_bench"
            echo "                      (syscall intra slow inter port pp ool flipc2 all)"
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
AHCI_MODULE="$BUILD_DIR/src/block_device_server/modules/ahci.so"
VIRTIO_BLK_MODULE="$BUILD_DIR/src/block_device_server/modules/virtio_blk.so"
HAL_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/hal_server"
HAL_PCI_SCAN_MODULE="$BUILD_DIR/src/hal_server/modules/pci_scan.so"
GPU_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/gpu_server"
GPU_VGA_MODULE="$BUILD_DIR/src/gpu_server/modules/vga.so"
CHAR_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/char_server"
CHAR_PS2_MODULE="$BUILD_DIR/src/char_server/modules/ps2.so"
EXT2_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/ext_server"
PTHREAD_TEST="$BUILD_DIR/export/uros/$ARCH/user/sbin/pthread_test"
CAP_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/cap_server"
CAP_TEST="$BUILD_DIR/export/uros/$ARCH/user/sbin/cap_test"
GPUSTAT="$BUILD_DIR/export/uros/$ARCH/user/sbin/gpustat"
EXEC_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/exec_server"
HELLO_EXEC="$BUILD_DIR/export/uros/$ARCH/user/sbin/hello_exec"
PROC_SERVER="$BUILD_DIR/export/uros/$ARCH/user/sbin/proc_server"

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

cat > "$BOOTSTRAP_CONF" <<CONF
name_server name_server
${CAP_SERVER_CONF_LINE}
${GPU_SERVER_CONF_LINE}
${CHAR_SERVER_CONF_LINE}
hal_server hal_server
block_device_server block_device_server
default_pager default_pager disk0c
hello_server hello_server
ext_server ext_server
${EXEC_SERVER_CONF_LINE}
${PROC_SERVER_CONF_LINE}
ipc_bench ipc_bench${BENCH_ARGS}
pthread_test pthread_test
${CAP_TEST_CONF_LINE}
${GPUSTAT_CONF_LINE}
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
BENCH_DAT=$(mktemp)
printf 'Hello from /mach_servers/ root\n' > "$HELLO_TXT"
dd if=/dev/urandom of="$BENCH_DAT" bs=1K count=1 status=none
trap 'rm -f "$PART_IMG" "$BOOTSTRAP_CONF" "$HELLO_TXT" "$BENCH_DAT"' EXIT

# hello_exec is optional (#228 v0.1.0): copy to / so exec_server can
# load "/hello_exec" via libvfs.
HELLO_EXEC_WRITE_LINE=""
if [ -f "$HELLO_EXEC" ]; then
    HELLO_EXEC_WRITE_LINE="write $HELLO_EXEC hello_exec"
fi

debugfs -w -f /dev/stdin "$PART_IMG" <<DBGFS 2>/dev/null
write $HELLO_TXT hello.txt
write $BENCH_DAT bench.dat
${HELLO_EXEC_WRITE_LINE}
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
