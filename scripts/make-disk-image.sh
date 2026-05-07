#!/usr/bin/env bash
# ==========================================================================
# make-disk-image.sh — Crea un'immagine disco per OSFMK/Uros
#
# Genera un disco MBR con due partizioni:
#   hd0a (MBR entry 0) — ext2  — /mach_servers/ con config e binari server
#   hd0b (MBR entry 1) — raw   — paging/swap per il default_pager
#
# Il driver hd del kernel Mach mappa le entry MBR 1:1 su d_partitions[]:
#   MBR entry 0 → d_partitions[0] → device name "hd0a"
#   MBR entry 1 → d_partitions[1] → device name "hd0b"
#
# boot_device viene impostato su hd0a (partizione 0) dal kernel.
# Il default_pager riceve "hd0b" come argomento via bootstrap.conf.
#
# Uso:
#   ./scripts/make-disk-image.sh                    # usa i default
#   ./scripts/make-disk-image.sh -o disk.img        # path output custom
#   ./scripts/make-disk-image.sh -s 32              # dimensione totale in MB
#
# Requisiti: sfdisk, mke2fs, debugfs (pacchetti: fdisk, e2fsprogs)
# Non richiede root/sudo.
# ==========================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/osfmk/build"
ARCH="$(uname -m)"

# --- Parametri di default ---
IMG_SIZE_MB=32
DISK_IMG="$BUILD_DIR/disk.img"
SECT_SIZE=512
PART1_START_SECT=2048       # ext2 filesystem — allineato a 1 MiB
FS_SIZE_MB=4                # dimensione filesystem (piccola: solo server binari)
# Il resto del disco diventa swap per il default_pager

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
NAME_SERVER="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/name_server"
DEFAULT_PAGER="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/default_pager"
HELLO_SERVER="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/hello_server"
IPC_BENCH="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/ipc_bench"
BLOCK_DEVICE_SERVER="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/block_device_server"
AHCI_MODULE="$BUILD_DIR/src/block_device_server/modules/ahci.so"
VIRTIO_BLK_MODULE="$BUILD_DIR/src/block_device_server/modules/virtio_blk.so"
HAL_SERVER="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/hal_server"
HAL_PCI_SCAN_MODULE="$BUILD_DIR/src/hal_server/modules/pci_scan.so"
GPU_SERVER="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/gpu_server"
GPU_VGA_MODULE="$BUILD_DIR/src/gpu_server/modules/vga.so"
EXT2_SERVER="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/ext_server"
PTHREAD_TEST="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/pthread_test"
CAP_SERVER="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/cap_server"
CAP_TEST="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/cap_test"

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
#
# Issue #184: default_pager ora apre la sua partizione di swap via BDS
# (cap_request + device_open_cap) anziché via il driver IDE in-kernel.
# Conseguenza sull'ordine: hal_server e block_device_server devono
# partire PRIMA di default_pager, altrimenti blk_open("disk0c") bloccherebbe
# su netname_notify in attesa che BDS pubblichi la partizione.
#
GPU_SERVER_CONF_LINE=""
[ -f "$GPU_SERVER" ] && GPU_SERVER_CONF_LINE="gpu_server gpu_server"

cat > "$BOOTSTRAP_CONF" <<CONF
name_server name_server
${CAP_SERVER_CONF_LINE}
${GPU_SERVER_CONF_LINE}
hal_server hal_server
block_device_server block_device_server
default_pager default_pager disk0c
hello_server hello_server
ipc_bench ipc_bench${BENCH_ARGS}
ext_server ext_server
pthread_test pthread_test
${CAP_TEST_CONF_LINE}
CONF

# --- Calcoli geometria ---
TOTAL_SECTS=$((IMG_SIZE_MB * 1024 * 1024 / SECT_SIZE))
FS_SIZE_SECTS=$((FS_SIZE_MB * 1024 * 1024 / SECT_SIZE))
PART2_START_SECT=$((PART1_START_SECT + FS_SIZE_SECTS))
SWAP_SIZE_SECTS=$((TOTAL_SECTS - PART2_START_SECT))
SWAP_SIZE_MB=$((SWAP_SIZE_SECTS * SECT_SIZE / 1024 / 1024))

echo "=== Creazione immagine disco OSFMK ==="
echo "  Output:      $DISK_IMG"
echo "  Dimensione:  ${IMG_SIZE_MB} MB"
echo "  Partizione 1 (hd0a): ext2, ${FS_SIZE_MB} MB  — /mach_servers/"
echo "  Partizione 2 (hd0b): swap, ${SWAP_SIZE_MB} MB — paging"
echo ""

# --- 1. Immagine vuota ---
echo "[1/6] Creazione immagine vuota (${IMG_SIZE_MB} MB)..."
dd if=/dev/zero of="$DISK_IMG" bs=1M count="$IMG_SIZE_MB" status=none

# --- 2. Tabella partizioni MBR ---
# Entry 0 → hd0a (ext2 filesystem con i server)
# Entry 1 → hd0b (raw swap per il default_pager)
echo "[2/6] Scrittura tabella partizioni MBR..."
sfdisk --quiet "$DISK_IMG" <<EOF
label: dos
start=$PART1_START_SECT, size=$FS_SIZE_SECTS, type=83
start=$PART2_START_SECT, size=$SWAP_SIZE_SECTS, type=82
EOF

# --- 3. Formattare la partizione ext2 ---
echo "[3/6] Formattazione ext2 (hd0a)..."
PART_IMG=$(mktemp /tmp/osfmk-part.XXXXXX.img)
trap 'rm -f "$PART_IMG" "$BOOTSTRAP_CONF"' EXIT

dd if=/dev/zero of="$PART_IMG" bs="$SECT_SIZE" count="$FS_SIZE_SECTS" status=none
mke2fs -t ext2 -q -F \
    -b 4096 \
    -I 256 \
    -r 1 \
    -L "mach_servers" \
    -O filetype \
    "$PART_IMG"

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
debugfs -w -f /dev/stdin "$PART_IMG" <<DBGFS 2>/dev/null
write $HELLO_TXT hello.txt
write $BENCH_DAT bench.dat
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
    debugfs -w -f - "$PART_IMG" >>"$DBGFS_LOG" 2>&1 <<DBGFS
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

# --- 5. Inserimento partizione ext2 nell'immagine disco ---
echo "[5/6] Assemblaggio partizione ext2..."
dd if="$PART_IMG" of="$DISK_IMG" bs="$SECT_SIZE" seek="$PART1_START_SECT" conv=notrunc status=none

# --- 6. La partizione swap è già zero-filled (nessun formato necessario) ---
# Issue #184: il default_pager non passa più dal driver IDE in-kernel.
# Apre la partizione tramite block_device_server: blk_open("disk0c") +
# cap_request(RESOURCE_BLK_DEVICE) + device_open_cap → handle autenticato
# per-(client, partizione) che usa come backing store.
echo "[6/6] Partizione swap (hd0b/disk0c) pronta (zero-filled)."

echo ""
echo "=== Immagine disco creata con successo ==="
echo "  $DISK_IMG ($(stat -c%s "$DISK_IMG") bytes)"
echo ""
echo "Struttura disco (${IMG_SIZE_MB} MB):"
echo "  MBR:    settore 0"
echo "  hd0a:   settori ${PART1_START_SECT}-$((PART1_START_SECT + FS_SIZE_SECTS - 1))  (ext2, ${FS_SIZE_MB} MB)"
echo "    └── /mach_servers/"
echo "        ├── bootstrap.conf"
echo "        ├── name_server"
echo "        ├── default_pager"
echo "        ├── hello_server"
echo "        ├── ipc_bench"
echo "        ├── block_device_server"
echo "        ├── hal_server"
echo "        ├── ext_server"
echo "        └── modules/"
echo "            ├── block/"
echo "            │   ├── ahci.so"
echo "            │   └── virtio_blk.so"
echo "            └── hal/"
echo "                └── pci_scan.so"
echo "  hd0b:   settori ${PART2_START_SECT}-$((PART2_START_SECT + SWAP_SIZE_SECTS - 1))  (swap, ${SWAP_SIZE_MB} MB)"
echo ""
echo "Flusso di boot:"
echo "  1. Kernel boot_device → hd0a (d_partitions[0])"
echo "  2. Bootstrap legge /dev/boot_device/mach_servers/bootstrap.conf"
echo "  3. Bootstrap carica /dev/boot_device/mach_servers/name_server"
echo "  4. Bootstrap carica /dev/boot_device/mach_servers/cap_server"
echo "  5. Bootstrap carica /dev/boot_device/mach_servers/hal_server"
echo "  6. Bootstrap carica /dev/boot_device/mach_servers/block_device_server"
echo "  7. Bootstrap carica /dev/boot_device/mach_servers/default_pager"
echo "  8. Bootstrap carica /dev/boot_device/mach_servers/hello_server"
echo "  9. Bootstrap carica /dev/boot_device/mach_servers/ipc_bench"
echo " 10. Bootstrap carica /dev/boot_device/mach_servers/ext_server"
echo " 11. default_pager argv[1]='disk0c' → device_open_cap via BDS → ${SWAP_SIZE_MB} MB swap"
echo ""
echo "Per avviare:"
echo "  ./scripts/run-qemu.sh"
