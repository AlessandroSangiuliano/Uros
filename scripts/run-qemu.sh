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

# Parse flags
USE_DISK=true
USE_AHCI=false
USE_VIRTIO=false
EXTRA_ARGS=""
for arg in "$@"; do
    case "$arg" in
        --no-disk) USE_DISK=false ;;
        --ahci) USE_AHCI=true ;;
        --virtio) USE_VIRTIO=true ;;
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
QEMU_ARGS="-m 512M -enable-kvm -cpu host -kernel $KERNEL -initrd $BOOTSTRAP -no-reboot"

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

# Optionally add an ICH9 AHCI controller with a test disk.
# The boot disk stays on IDE; the AHCI controller appears as a
# separate PCI device that the ahci_driver can detect and probe.
AHCI_DISK="$BUILD_DIR/ahci-test.img"
if [ "$USE_AHCI" = true ]; then
    if [ ! -f "$AHCI_DISK" ]; then
        echo "Creazione disco AHCI di test (8 MB, ext2)..."
        dd if=/dev/zero of="$AHCI_DISK" bs=1M count=8 status=none
        mke2fs -t ext2 -q -F -b 1024 -I 256 -r 1 -O filetype "$AHCI_DISK"
        HELLO_TXT=$(mktemp)
        printf 'Hello from ext2 on AHCI!\n' > "$HELLO_TXT"
        BENCH_DAT=$(mktemp)
        dd if=/dev/urandom of="$BENCH_DAT" bs=1K count=4096 status=none 2>/dev/null
        debugfs -w -f /dev/stdin "$AHCI_DISK" <<DBGFS 2>/dev/null
write $HELLO_TXT hello.txt
write $BENCH_DAT bench.dat
DBGFS
        rm -f "$HELLO_TXT" "$BENCH_DAT"
        echo "  Disco AHCI formattato ext2 con /hello.txt + /bench.dat (4 MB)"
    fi
    echo "AHCI: $AHCI_DISK (ICH9 controller)"
    QEMU_ARGS="$QEMU_ARGS -device ich9-ahci,id=ahci0"
    QEMU_ARGS="$QEMU_ARGS -drive id=ahcidisk0,file=$AHCI_DISK,format=raw,if=none"
    QEMU_ARGS="$QEMU_ARGS -device ide-hd,drive=ahcidisk0,bus=ahci0.0"
fi

# Optionally add a virtio-blk-pci device with a test disk.
# The boot disk stays on IDE; the virtio-blk controller appears as a
# separate PCI device that the virtio_blk driver can detect and probe.
VIRTIO_DISK="$BUILD_DIR/virtio-test.img"
if [ "$USE_VIRTIO" = true ]; then
    if [ ! -f "$VIRTIO_DISK" ]; then
        echo "Creazione disco virtio-blk di test (8 MB, ext2)..."
        dd if=/dev/zero of="$VIRTIO_DISK" bs=1M count=8 status=none
        mke2fs -t ext2 -q -F -b 1024 -I 256 -r 1 -O filetype "$VIRTIO_DISK"
        HELLO_TXT=$(mktemp)
        printf 'Hello from ext2 on virtio-blk!\n' > "$HELLO_TXT"
        debugfs -w -f /dev/stdin "$VIRTIO_DISK" <<DBGFS 2>/dev/null
write $HELLO_TXT hello.txt
DBGFS
        rm -f "$HELLO_TXT"
        echo "  Disco virtio-blk formattato ext2 con /hello.txt"
    fi
    echo "Virtio-blk: $VIRTIO_DISK"
    QEMU_ARGS="$QEMU_ARGS -drive id=virtiodisk0,file=$VIRTIO_DISK,format=raw,if=none"
    QEMU_ARGS="$QEMU_ARGS -device virtio-blk-pci,drive=virtiodisk0"
fi

# shellcheck disable=SC2086
exec qemu-system-i386 $QEMU_ARGS $EXTRA_ARGS
