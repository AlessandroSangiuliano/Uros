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
# Overridable, like run-qemu.sh: a measurement must be able to run against a
# tree nobody else is rebuilding (#485).  ⚠️ It has to be honoured in EVERY
# script the run touches -- run-qemu.sh alone was not enough, and the first
# attempt booted a kernel from the measurement tree with a bundle from the
# working one.
BUILD_DIR="${UROS_BUILD_DIR:-$REPO_ROOT/uros/build}"
# #404: the build stamps its target arch; uname -m names the HOST
ARCH="$(cat "$BUILD_DIR/uros-arch" 2>/dev/null || uname -m)"

BUNDLE_OUT="$BUILD_DIR/bootstrap.bundle"
MKBUNDLE="$BUILD_DIR/tools/mkbundle"
BENCH_ARGS=""
MINIMAL=0
DISKLESS=0
WITH_CONSOLE=0

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
        --minimal)
            MINIMAL=1; shift ;;
        --diskless)
            DISKLESS=1; shift ;;
        --with-console)
            WITH_CONSOLE=1; shift ;;
        -h|--help)
            echo "Uso: $0 [-o output.bundle] [--bench suite ...] [--minimal] [--diskless]"
            echo "  --minimal: skip test/bench tasks (ipc_bench, pthread_test, cap_test,"
            echo "             kernel242_test, sig_test, gpustat) from bootstrap.conf"
            echo "  --diskless: run ipc_bench in stage-1 (before block_device_server) so the"
            echo "              IPC suite completes without a boot disk (#344, omen USB boot)"
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

SBIN="$BUILD_DIR/export/uros/$ARCH/user/sbin"
MANIFESTS="$BUILD_DIR/export/uros/$ARCH/user/manifests"

# #216 v2.1: compiled per-server manifests (.cmf TLV) that bootstrap
# hands to cap_server at task_create time.  Optional — servers
# without a .cmf fall back to the legacy permissive cap_server path.
HELLO_SERVER_CMF="$MANIFESTS/hello_server.cmf"

NAME_SERVER="$SBIN/name_server"
CAP_SERVER="$SBIN/cap_server"
CAP_TEST="$SBIN/cap_test"
GPUSTAT="$SBIN/gpustat"
HAL_SERVER="$SBIN/hal_server"
BLOCK_DEVICE_SERVER="$SBIN/block_device_server"
DEFAULT_PAGER="$SBIN/default_pager"
HELLO_SERVER="$SBIN/hello_server"
IPC_BENCH="$SBIN/ipc_bench"
EXT2_SERVER="$SBIN/ext_server"
PTHREAD_TEST="$SBIN/pthread_test"
EXEC_SERVER="$SBIN/exec_server"
HELLO_EXEC="$SBIN/hello_exec"
PROC_SERVER="$SBIN/proc_server"
KERNEL242_TEST="$SBIN/kernel242_test"
SIG_TEST="$SBIN/sig_test"

HAL_PCI_SCAN_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/hal/pci_scan.so"
AHCI_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/block/ahci.so"
VIRTIO_BLK_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/block/virtio_blk.so"
GPU_SERVER="$SBIN/gpu_server"
GPU_VGA_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/gpu/vga.so"
CHAR_SERVER="$SBIN/char_server"
CHAR_PS2_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/char/ps2.so"
CHAR_UART_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/char/uart.so"
CHAR_PS2_MOUSE_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/char/ps2_mouse.so"
CHAR_CONSOLE_MODULE="$BUILD_DIR/export/uros/$ARCH/user/modules/char/console.so"

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
KERNEL242_TEST_CONF_LINE=""
[ -f "$KERNEL242_TEST" ] && KERNEL242_TEST_CONF_LINE="kernel242_test kernel242_test"
SIG_TEST_CONF_LINE=""
[ -f "$SIG_TEST" ] && SIG_TEST_CONF_LINE="sig_test sig_test"
EXEC_SERVER_CONF_LINE=""
[ -f "$EXEC_SERVER" ] && EXEC_SERVER_CONF_LINE="exec_server exec_server"
PROC_SERVER_CONF_LINE=""
[ -f "$PROC_SERVER" ] && PROC_SERVER_CONF_LINE="proc_server proc_server"
GPUSTAT_CONF_LINE=""
[ -f "$GPUSTAT" ] && GPUSTAT_CONF_LINE="gpustat gpustat"
# ush (#275.5): kept off the bundle (no `ARGS+=` entry below) so it
# always loads from /dev/boot_device/mach_servers/ush on disk.  The
# bootstrap.conf line still names it as a stage-2 task; bootstrap's
# fallback path picks it up from the disk fs.
#
# #365: the virtual_terminal_server, when built, takes ush's stage-2 launch
# slot and forks ush onto each VT.  Like ush it is kept off the bundle and
# loaded from disk; only the launch line changes.
USH="$SBIN/ush"
VTS="$SBIN/virtual_terminal_server"
USH_CONF_LINE=""
if [ -f "$VTS" ]; then
    USH_CONF_LINE="virtual_terminal_server virtual_terminal_server"
elif [ -f "$USH" ]; then
    USH_CONF_LINE="ush ush"
fi

GPU_SERVER_CONF_LINE=""
[ -f "$GPU_SERVER" ] && GPU_SERVER_CONF_LINE="gpu_server gpu_server"
CHAR_SERVER_CONF_LINE=""
[ -f "$CHAR_SERVER" ] && CHAR_SERVER_CONF_LINE="char_server char_server"

if [ "$MINIMAL" = "1" ]; then
    HELLO_SERVER_LINE=""
    IPC_BENCH_LINE=""
    PTHREAD_TEST_LINE=""
    CAP_TEST_CONF_LINE=""
    KERNEL242_TEST_CONF_LINE=""
    SIG_TEST_CONF_LINE=""
    GPUSTAT_CONF_LINE=""
else
    HELLO_SERVER_LINE="hello_server hello_server"
    IPC_BENCH_LINE="ipc_bench ipc_bench${BENCH_ARGS}"
    PTHREAD_TEST_LINE="pthread_test pthread_test"
fi

# #344: diskless bench.  bootstrap.c blocks at stage-2 waiting for the boot
# disk immediately after launching block_device_server, so any task listed
# after it (ipc_bench included) never runs without a disk.  In --diskless mode
# run ipc_bench in stage-1, ahead of block_device_server, so the IPC suite
# completes from RAM on a USB boot (omen, #332); its disk-I/O sub-bench
# self-skips when no ext_server/disk is present.
IPC_BENCH_STAGE1_LINE=""
if [ "$DISKLESS" = "1" ]; then
    IPC_BENCH_STAGE1_LINE="$IPC_BENCH_LINE"
    IPC_BENCH_LINE=""
fi

# ⚠️ `-w' sul name_server (#492): bootstrap non carica il server successivo
# finche' questo non ha mandato bootstrap_completed.
#
# 🔥 Le due meta' dell'handshake esistevano gia' ed erano scollegate.
# name_server/netname.c chiama bootstrap_completed() da sempre, e bootstrap
# onora SERVER_SERIALIZE_F da sempre -- ma il flag che li unisce non era mai
# stato messo, quindi il name server era soltanto PRIMO NELLA LISTA, che
# decide l'ordine in cui i task vengono creati e non l'ordine in cui parte la
# loro prima istruzione.  netname_test vinceva quella corsa in 5 boot su 5:
# stampava il verdetto, falliva entrambi gli arm con MIG_SERVER_DIED, e veniva
# terminato PRIMA che il name server stampasse "started".
#
# ⚠️ I flag stanno all'INIZIO della riga: parse_boot_args() gira prima che
# venga letto symtab_name e si ferma al primo token che non comincia per `-'.
# Un flag scritto dopo il nome viene ignorato in silenzio.
cat > "$BOOTSTRAP_CONF" <<CONF
-w name_server name_server
${CAP_SERVER_CONF_LINE}
${GPU_SERVER_CONF_LINE}
${CHAR_SERVER_CONF_LINE}
hal_server hal_server
${IPC_BENCH_STAGE1_LINE}
block_device_server block_device_server
default_pager default_pager disk0c
${HELLO_SERVER_LINE}
ext_server ext_server
${EXEC_SERVER_CONF_LINE}
${PROC_SERVER_CONF_LINE}
${IPC_BENCH_LINE}
${PTHREAD_TEST_LINE}
${CAP_TEST_CONF_LINE}
${KERNEL242_TEST_CONF_LINE}
${SIG_TEST_CONF_LINE}
${GPUSTAT_CONF_LINE}
${USH_CONF_LINE}
CONF

ARGS=(-o "$BUNDLE_OUT")
ARGS+=("bootstrap.conf:$BOOTSTRAP_CONF")
ARGS+=("name_server:$NAME_SERVER")
[ -f "$CAP_SERVER" ] && ARGS+=("cap_server:$CAP_SERVER")
ARGS+=("hal_server:$HAL_SERVER")
ARGS+=("block_device_server:$BLOCK_DEVICE_SERVER")
ARGS+=("default_pager:$DEFAULT_PAGER")
ARGS+=("hello_server:$HELLO_SERVER")
[ -f "$HELLO_SERVER_CMF" ] && ARGS+=("hello_server.cmf:$HELLO_SERVER_CMF")
ARGS+=("ipc_bench:$IPC_BENCH")
ARGS+=("ext_server:$EXT2_SERVER")
ARGS+=("pthread_test:$PTHREAD_TEST")
[ -f "$EXEC_SERVER" ] && ARGS+=("exec_server:$EXEC_SERVER")
[ -f "$PROC_SERVER" ] && ARGS+=("proc_server:$PROC_SERVER")
[ -f "$CAP_TEST" ] && ARGS+=("cap_test:$CAP_TEST")
[ -f "$KERNEL242_TEST" ] && ARGS+=("kernel242_test:$KERNEL242_TEST")
[ -f "$SIG_TEST" ] && ARGS+=("sig_test:$SIG_TEST")
[ -f "$GPUSTAT" ] && ARGS+=("gpustat:$GPUSTAT")
ARGS+=("modules/hal/pci_scan.so:$HAL_PCI_SCAN_MODULE")
ARGS+=("modules/block/ahci.so:$AHCI_MODULE")
ARGS+=("modules/block/virtio_blk.so:$VIRTIO_BLK_MODULE")
if [ -f "$GPU_SERVER" ]; then
    ARGS+=("gpu_server:$GPU_SERVER")
    ARGS+=("modules/gpu/vga.so:$GPU_VGA_MODULE")
fi
if [ -f "$CHAR_SERVER" ]; then
    ARGS+=("char_server:$CHAR_SERVER")
    [ -f "$CHAR_PS2_MODULE" ]       && ARGS+=("modules/char/ps2.so:$CHAR_PS2_MODULE")
    [ -f "$CHAR_UART_MODULE" ]      && ARGS+=("modules/char/uart.so:$CHAR_UART_MODULE")
    [ -f "$CHAR_PS2_MOUSE_MODULE" ] && ARGS+=("modules/char/ps2_mouse.so:$CHAR_PS2_MOUSE_MODULE")
    # #363: the on-screen console TTY ships only when explicitly asked
    # (--with-console).  Without it ush binds the UART TTY and the serial
    # console behaves exactly as before — headless/bench boots untouched.
    if [ "$WITH_CONSOLE" = "1" ] && [ -f "$CHAR_CONSOLE_MODULE" ]; then
        ARGS+=("modules/char/console.so:$CHAR_CONSOLE_MODULE")
    fi
fi

"$MKBUNDLE" "${ARGS[@]}"

echo "Bundle: $BUNDLE_OUT ($(stat -c%s "$BUNDLE_OUT") bytes)"
