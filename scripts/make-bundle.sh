#!/usr/bin/env bash
# ==========================================================================
# make-bundle.sh — Crea il bundle stage-1 multiboot per Uros (Issue #186).
#
# Il bundle è un archivio flat name->bytes che il kernel passa al
# bootstrap server come secondo modulo multiboot (mod[1]).  Contiene la
# bootstrap.conf e i binari/moduli necessari prima che il
# block_device_server sia in piedi (stage-0), così bootstrap.c non deve
# più affidarsi al driver IDE in-kernel per leggere /mach_servers/.
#
# Uso:
#   ./scripts/make-bundle.sh                  # output di default
#   ./scripts/make-bundle.sh -o bundle.bin    # path output custom
#   ./scripts/make-bundle.sh --bench all ...  # passa suite a ipc_bench
# ==========================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/osfmk/build"
ARCH="$(uname -m)"

BUNDLE_OUT="$BUILD_DIR/bootstrap.bundle"
MKBUNDLE="$BUILD_DIR/tools/mkbundle"
BENCH_ARGS=""

while [ $# -gt 0 ]; do
    case "$1" in
        -o) BUNDLE_OUT="$2"; shift 2 ;;
        --bench)
            shift
            while [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; do
                BENCH_ARGS="$BENCH_ARGS $1"
                shift
            done
            ;;
        -h|--help)
            echo "Uso: $0 [-o output.bundle] [--bench suite ...]"
            exit 0
            ;;
        *) echo "Opzione sconosciuta: $1" >&2; exit 1 ;;
    esac
done

if [ ! -x "$MKBUNDLE" ]; then
    echo "ERRORE: mkbundle non trovato: $MKBUNDLE"
    echo "  Build con: cd $BUILD_DIR && ninja mkbundle"
    exit 1
fi

SBIN="$BUILD_DIR/export/osfmk/$ARCH/user/sbin"

NAME_SERVER="$SBIN/name_server"
CAP_SERVER="$SBIN/cap_server"
CAP_TEST="$SBIN/cap_test"
HAL_SERVER="$SBIN/hal_server"
BLOCK_DEVICE_SERVER="$SBIN/block_device_server"
DEFAULT_PAGER="$SBIN/default_pager"
HELLO_SERVER="$SBIN/hello_server"
IPC_BENCH="$SBIN/ipc_bench"
EXT2_SERVER="$SBIN/ext_server"
PTHREAD_TEST="$SBIN/pthread_test"

HAL_PCI_SCAN_MODULE="$BUILD_DIR/src/hal_server/modules/pci_scan.so"
AHCI_MODULE="$BUILD_DIR/src/block_device_server/modules/ahci.so"
VIRTIO_BLK_MODULE="$BUILD_DIR/src/block_device_server/modules/virtio_blk.so"
GPU_SERVER="$SBIN/gpu_server"
GPU_VGA_MODULE="$BUILD_DIR/src/gpu_server/modules/vga.so"
CHAR_SERVER="$SBIN/char_server"

REQUIRED_FILES=(
    "$NAME_SERVER" "$HAL_SERVER" "$BLOCK_DEVICE_SERVER"
    "$DEFAULT_PAGER" "$HELLO_SERVER" "$IPC_BENCH"
    "$EXT2_SERVER" "$PTHREAD_TEST"
    "$HAL_PCI_SCAN_MODULE" "$AHCI_MODULE" "$VIRTIO_BLK_MODULE"
)
# gpu_server is optional in 0.1.0 (OSFMK_BUILD_GPU_SERVER off by default)
[ -f "$GPU_SERVER" ] && REQUIRED_FILES+=("$GPU_SERVER" "$GPU_VGA_MODULE")
for f in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$f" ]; then
        echo "ERRORE: file mancante: $f"
        exit 1
    fi
done

# bootstrap.conf — stesso contenuto di make-disk-image.sh, perché la
# personalità di stage-0 deve essere indistinguibile da quella stage-2.
BOOTSTRAP_CONF=$(mktemp)
trap 'rm -f "$BOOTSTRAP_CONF"' EXIT

CAP_SERVER_CONF_LINE=""
[ -f "$CAP_SERVER" ] && CAP_SERVER_CONF_LINE="cap_server cap_server"
CAP_TEST_CONF_LINE=""
[ -f "$CAP_TEST" ] && CAP_TEST_CONF_LINE="cap_test cap_test"

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
ipc_bench ipc_bench${BENCH_ARGS}
ext_server ext_server
pthread_test pthread_test
${CAP_TEST_CONF_LINE}
CONF

ARGS=(-o "$BUNDLE_OUT")
ARGS+=("bootstrap.conf:$BOOTSTRAP_CONF")
ARGS+=("name_server:$NAME_SERVER")
[ -f "$CAP_SERVER" ] && ARGS+=("cap_server:$CAP_SERVER")
ARGS+=("hal_server:$HAL_SERVER")
ARGS+=("block_device_server:$BLOCK_DEVICE_SERVER")
ARGS+=("default_pager:$DEFAULT_PAGER")
ARGS+=("hello_server:$HELLO_SERVER")
ARGS+=("ipc_bench:$IPC_BENCH")
ARGS+=("ext_server:$EXT2_SERVER")
ARGS+=("pthread_test:$PTHREAD_TEST")
[ -f "$CAP_TEST" ] && ARGS+=("cap_test:$CAP_TEST")
ARGS+=("modules/hal/pci_scan.so:$HAL_PCI_SCAN_MODULE")
ARGS+=("modules/block/ahci.so:$AHCI_MODULE")
ARGS+=("modules/block/virtio_blk.so:$VIRTIO_BLK_MODULE")
if [ -f "$GPU_SERVER" ]; then
    ARGS+=("gpu_server:$GPU_SERVER")
    ARGS+=("modules/gpu/vga.so:$GPU_VGA_MODULE")
fi
# /mach_servers/modules/char/ filled by #206 (ps2.so) + #207 (uart.so);
# nothing to ship here in #205 (skeleton-only).
[ -f "$CHAR_SERVER" ] && ARGS+=("char_server:$CHAR_SERVER")

"$MKBUNDLE" "${ARGS[@]}"

echo "Bundle: $BUNDLE_OUT ($(stat -c%s "$BUNDLE_OUT") bytes)"
