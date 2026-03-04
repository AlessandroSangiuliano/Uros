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

# --- Parse argomenti ---
while getopts "o:s:h" opt; do
    case "$opt" in
        o) DISK_IMG="$OPTARG" ;;
        s) IMG_SIZE_MB="$OPTARG" ;;
        h)
            echo "Uso: $0 [-o output.img] [-s size_mb]"
            exit 0
            ;;
        *) echo "Opzione sconosciuta: -$opt" >&2; exit 1 ;;
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
DEFAULT_PAGER="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/default_pager"
HELLO_SERVER="$BUILD_DIR/export/osfmk/$ARCH/user/sbin/hello_server"

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

# --- File di configurazione del bootstrap ---
# Formato: <symtab_name> <path> [args...]
# Il path relativo viene risolto come /dev/boot_device/mach_servers/<path>
# L'argomento "hd0b" dopo il path diventa argv[1] del default_pager,
# che lo apre con device_open() e lo usa come backing store di paging.
BOOTSTRAP_CONF=$(mktemp)
cat > "$BOOTSTRAP_CONF" <<'CONF'
default_pager default_pager hd0b
hello_server hello_server
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
    -b 1024 \
    -I 256 \
    -r 1 \
    -L "mach_servers" \
    -O filetype \
    "$PART_IMG"

# --- 4. Copia file nel filesystem con debugfs ---
echo "[4/6] Copia file nel filesystem ext2..."
debugfs -w -f /dev/stdin "$PART_IMG" <<DBGFS 2>/dev/null
mkdir mach_servers
cd mach_servers
write $BOOTSTRAP_CONF bootstrap.conf
write $DEFAULT_PAGER default_pager
write $HELLO_SERVER hello_server
DBGFS

echo "  /mach_servers/bootstrap.conf → 'default_pager default_pager hd0b'"
echo "  /mach_servers/bootstrap.conf → 'hello_server hello_server'"
echo "  /mach_servers/default_pager  → $(stat -c%s "$DEFAULT_PAGER") bytes"
echo "  /mach_servers/hello_server   → $(stat -c%s "$HELLO_SERVER") bytes"

# --- 5. Inserimento partizione ext2 nell'immagine disco ---
echo "[5/6] Assemblaggio partizione ext2..."
dd if="$PART_IMG" of="$DISK_IMG" bs="$SECT_SIZE" seek="$PART1_START_SECT" conv=notrunc status=none

# --- 6. La partizione swap è già zero-filled (nessun formato necessario) ---
# Il default_pager usa la partizione raw: chiama device_open("hd0b"),
# ottiene la dimensione con DEV_GET_SIZE, e la usa come backing store.
echo "[6/6] Partizione swap (hd0b) pronta (zero-filled)."

echo ""
echo "=== Immagine disco creata con successo ==="
echo "  $DISK_IMG ($(stat -c%s "$DISK_IMG") bytes)"
echo ""
echo "Struttura disco (${IMG_SIZE_MB} MB):"
echo "  MBR:    settore 0"
echo "  hd0a:   settori ${PART1_START_SECT}-$((PART1_START_SECT + FS_SIZE_SECTS - 1))  (ext2, ${FS_SIZE_MB} MB)"
echo "    └── /mach_servers/"
echo "        ├── bootstrap.conf"
echo "        ├── default_pager"
echo "        └── hello_server"
echo "  hd0b:   settori ${PART2_START_SECT}-$((PART2_START_SECT + SWAP_SIZE_SECTS - 1))  (swap, ${SWAP_SIZE_MB} MB)"
echo ""
echo "Flusso di boot:"
echo "  1. Kernel boot_device → hd0a (d_partitions[0])"
echo "  2. Bootstrap legge /dev/boot_device/mach_servers/bootstrap.conf"
echo "  3. Bootstrap carica /dev/boot_device/mach_servers/default_pager"
echo "  4. Bootstrap carica /dev/boot_device/mach_servers/hello_server"
echo "  5. default_pager argv[1]='hd0b' → device_open('hd0b') → ${SWAP_SIZE_MB} MB swap"
echo ""
echo "Per avviare:"
echo "  ./scripts/run-qemu.sh"
