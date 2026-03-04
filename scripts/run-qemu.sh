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
#
# L'immagine disco contiene /mach_servers/ con:
#   bootstrap.conf   — configurazione del bootstrap
#   default_pager    — server di paging
#
# Il driver IDE del kernel (hd.c) vede il disco QEMU come hd0.
# boot_device → d_partitions[0] → prima partizione MBR (ext2).
set -e

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$REPO_ROOT/osfmk/build"
KERNEL="$BUILD_DIR/export/osfmk/boot/mach_kernel"
BOOTSTRAP="$BUILD_DIR/export/osfmk/$(uname -m)/user/sbin/bootstrap"
DISK_IMG="$BUILD_DIR/disk.img"

# Parse --no-disk flag
USE_DISK=true
EXTRA_ARGS=""
for arg in "$@"; do
    case "$arg" in
        --no-disk) USE_DISK=false ;;
        *) EXTRA_ARGS="$EXTRA_ARGS $arg" ;;
    esac
done

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

# Costruisci la riga di comando QEMU
QEMU_ARGS="-m 128M -kernel $KERNEL -initrd $BOOTSTRAP -no-reboot"

if [ "$USE_DISK" = true ] && [ -f "$DISK_IMG" ]; then
    echo "Disco: $DISK_IMG"
    # The format=raw backend does not support CHS geometry options.
    # The hd.c driver falls back to ATA IDENTIFY DEVICE data for CHS
    # when the BIOS FDPT is empty, so no explicit geometry is needed.
    QEMU_ARGS="$QEMU_ARGS -drive file=$DISK_IMG,format=raw,if=ide,index=0,media=disk"
elif [ "$USE_DISK" = true ]; then
    echo "ATTENZIONE: immagine disco non trovata: $DISK_IMG"
    echo "  Crea con: ./scripts/make-disk-image.sh"
    echo "  Avvio senza disco (il bootstrap non troverà i server)."
    echo ""
fi

# shellcheck disable=SC2086
exec qemu-system-i386 $QEMU_ARGS $EXTRA_ARGS
