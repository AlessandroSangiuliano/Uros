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
USE_AHCI2=false
USE_VIRTIO=false
BENCH_ARGS=""
EXTRA_ARGS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --no-disk) USE_DISK=false; shift ;;
        --ahci2) USE_AHCI=true; USE_AHCI2=true; shift ;;
        --ahci) USE_AHCI=true; shift ;;
        --virtio) USE_VIRTIO=true; shift ;;
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
if [ -n "$BENCH_ARGS" ]; then
    echo "Bench suites:$BENCH_ARGS"
    "$REPO_ROOT/scripts/make-disk-image.sh" --bench $BENCH_ARGS
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

# Optionally add an ICH9 AHCI controller with partitioned test disk(s).
# The boot disk stays on IDE; the AHCI controller appears as a
# separate PCI device that the ahci_driver can detect and probe.
#
# Each AHCI disk has MBR + 2 ext2 partitions with different hello.txt
# content so multi-mount can be verified.
#
# create_ahci_disk <path> <label_suffix>
#   Creates a 16 MB disk with 2 x 4 MB ext2 partitions.
#   Partition 0: hello.txt = "Hello from <label_suffix> partition 0"
#   Partition 1: hello.txt = "Hello from <label_suffix> partition 1"
create_ahci_disk() {
    _disk="$1"
    _label="$2"

    echo "  Creazione disco AHCI partitioned: $_disk"
    dd if=/dev/zero of="$_disk" bs=1M count=16 status=none

    # MBR: 2 partitions, each 4 MB (8192 sectors)
    sfdisk --quiet "$_disk" <<SFDISK
label: dos
start=2048, size=8192, type=83
start=10240, size=8192, type=83
SFDISK

    # Format and populate each partition
    _pidx=0
    for _start in 2048 10240; do
        _part=$(mktemp /tmp/osfmk-ahci-part.XXXXXX.img)
        dd if=/dev/zero of="$_part" bs=512 count=8192 status=none
        mke2fs -t ext2 -q -F -b 4096 -I 256 -r 1 -O filetype "$_part"

        _htxt=$(mktemp)
        printf 'Hello from %s partition %d\n' "$_label" "$_pidx" > "$_htxt"

        # Add bench.dat only on first partition of first disk
        if [ "$_pidx" = 0 ] && [ "$_label" = "disk0" ]; then
            _bdat=$(mktemp)
            dd if=/dev/urandom of="$_bdat" bs=1K count=4096 status=none 2>/dev/null
            debugfs -w -f /dev/stdin "$_part" <<DBGFS 2>/dev/null
write $_htxt hello.txt
write $_bdat bench.dat
DBGFS
            rm -f "$_bdat"
        else
            debugfs -w -f /dev/stdin "$_part" <<DBGFS 2>/dev/null
write $_htxt hello.txt
DBGFS
        fi
        rm -f "$_htxt"

        dd if="$_part" of="$_disk" bs=512 seek="$_start" conv=notrunc status=none
        rm -f "$_part"
        _pidx=$((_pidx + 1))
    done
    echo "    MBR + 2 ext2 partitions (4 MB each)"
}

AHCI_DISK0="$BUILD_DIR/ahci-test0.img"
AHCI_DISK1="$BUILD_DIR/ahci-test1.img"
if [ "$USE_AHCI" = true ]; then
    # Always recreate to ensure correct layout
    create_ahci_disk "$AHCI_DISK0" "disk0"

    QEMU_ARGS="$QEMU_ARGS -device ich9-ahci,id=ahci0"
    QEMU_ARGS="$QEMU_ARGS -drive id=ahcidisk0,file=$AHCI_DISK0,format=raw,if=none"
    QEMU_ARGS="$QEMU_ARGS -device ide-hd,drive=ahcidisk0,bus=ahci0.0"
    echo "AHCI port 0: $AHCI_DISK0"

    if [ "$USE_AHCI2" = true ]; then
        create_ahci_disk "$AHCI_DISK1" "disk1"
        QEMU_ARGS="$QEMU_ARGS -drive id=ahcidisk1,file=$AHCI_DISK1,format=raw,if=none"
        QEMU_ARGS="$QEMU_ARGS -device ide-hd,drive=ahcidisk1,bus=ahci0.1"
        echo "AHCI port 1: $AHCI_DISK1"
    fi
fi

# Optionally add a virtio-blk-pci device with a test disk.
# The boot disk stays on IDE; the virtio-blk controller appears as a
# separate PCI device that the virtio_blk driver can detect and probe.
VIRTIO_DISK="$BUILD_DIR/virtio-test.img"
if [ "$USE_VIRTIO" = true ]; then
    if [ ! -f "$VIRTIO_DISK" ]; then
        echo "Creazione disco virtio-blk di test (8 MB, ext2)..."
        dd if=/dev/zero of="$VIRTIO_DISK" bs=1M count=8 status=none
        mke2fs -t ext2 -q -F -b 4096 -I 256 -r 1 -O filetype "$VIRTIO_DISK"
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
