#!/usr/bin/env sh
# Avvia Uros sotto Bochs (interprete x86 puro, indipendente da QEMU TCG).
#
# Costruisce una ISO GRUB con kernel + bootstrap + bundle come moduli
# multiboot, e usa la disk.img IDE esistente come ata0-master per
# /mach_servers/.  L'output seriale (com1) viene reindirizzato a un file
# di log così possiamo osservare il boot anche se la finestra Bochs si
# blocca.
#
# Scopo (#192): se il kernel sotto Bochs si comporta come sotto KVM
# (boota correttamente col path kmem_alloc_wired originale ripristinato),
# allora il drop dei *ptep store via alias higher-half è specifico di
# QEMU TCG.  Se invece anche Bochs lo manifesta, il bug è nel kernel.
#
# Uso:
#   ./scripts/run-bochs.sh                  # X11 GUI + serial -> file
#   ./scripts/run-bochs.sh --no-disk        # senza ata0-master IDE
#   tail -F osfmk/build/bochs-serial.log    # in altro terminale
set -e

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$REPO_ROOT/osfmk/build"
KERNEL="$BUILD_DIR/export/osfmk/boot/mach_kernel"
BOOTSTRAP="$BUILD_DIR/export/osfmk/$(uname -m)/user/sbin/bootstrap"
BUNDLE="$BUILD_DIR/bootstrap.bundle"
DISK_IMG="$BUILD_DIR/disk.img"

ISO_DIR="$BUILD_DIR/bochs-iso"
ISO_IMG="$BUILD_DIR/uros.iso"
BOCHSRC="$BUILD_DIR/bochsrc"
SERIAL_LOG="$BUILD_DIR/bochs-serial.log"

USE_DISK=true
while [ $# -gt 0 ]; do
    case "$1" in
        --no-disk) USE_DISK=false; shift ;;
        *) echo "Unknown flag: $1" >&2; exit 2 ;;
    esac
done

for f in "$KERNEL" "$BOOTSTRAP" "$BUNDLE"; do
    [ -f "$f" ] || { echo "ERROR: missing $f (build first)"; exit 1; }
done
if [ "$USE_DISK" = true ] && [ ! -f "$DISK_IMG" ]; then
    echo "ATTENZIONE: $DISK_IMG non trovato; uso --no-disk"
    USE_DISK=false
fi

# 1) Staging della ISO GRUB --------------------------------------------------
rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR/boot/grub"
cp "$KERNEL"    "$ISO_DIR/boot/mach_kernel"
cp "$BOOTSTRAP" "$ISO_DIR/boot/bootstrap"
cp "$BUNDLE"    "$ISO_DIR/boot/bootstrap.bundle"

cat >"$ISO_DIR/boot/grub/grub.cfg" <<'GRUB'
set timeout=0
set default=0
serial --unit=0 --speed=9600
terminal_input  console serial
terminal_output console serial

menuentry "Uros (Bochs)" {
    multiboot /boot/mach_kernel
    module    /boot/bootstrap
    module    /boot/bootstrap.bundle
    boot
}
GRUB

echo "Building ISO: $ISO_IMG"
grub-mkrescue -o "$ISO_IMG" "$ISO_DIR" 2>&1 | tail -3

# 2) bochsrc -----------------------------------------------------------------
# Trova i BIOS files (Arch li mette in /usr/local/share/bochs).
BX_SHARE=/usr/local/share/bochs
[ -d "$BX_SHARE" ] || BX_SHARE=/usr/share/bochs
ROMBIOS="$BX_SHARE/BIOS-bochs-latest"
VGABIOS="$BX_SHARE/VGABIOS-lgpl-latest.bin"
[ -f "$ROMBIOS" ] || { echo "ERROR: missing $ROMBIOS"; exit 1; }
[ -f "$VGABIOS" ] || { echo "ERROR: missing $VGABIOS"; exit 1; }

DISK_LINE=""
if [ "$USE_DISK" = true ]; then
    DISK_BYTES=$(stat -c %s "$DISK_IMG")
    DISK_SECTORS=$(( DISK_BYTES / 512 ))
    DISK_CYL=$(( DISK_SECTORS / (16 * 63) ))
    DISK_LINE="ata0-master: type=disk, path=\"$DISK_IMG\", mode=flat, cylinders=$DISK_CYL, heads=16, spt=63"
fi

cat >"$BOCHSRC" <<EOF
# Auto-generato da scripts/run-bochs.sh
megs: 512
romimage:    file=$ROMBIOS
vgaromimage: file=$VGABIOS

cpu: model=corei7_sandy_bridge_2600k, count=1, ips=50000000, reset_on_triple_fault=0

ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14
$DISK_LINE

ata1: enabled=1, ioaddr1=0x170, ioaddr2=0x370, irq=15
ata1-master: type=cdrom, path="$ISO_IMG", status=inserted

boot: cdrom, disk
floppy_bootsig_check: disabled=0

com1: enabled=1, mode=file, dev="$SERIAL_LOG"

display_library: x
log: -
panic: action=ask
error: action=report
info: action=report
debug: action=ignore

clock: sync=none, time0=local
EOF

echo "Bochsrc:    $BOCHSRC"
echo "Serial log: $SERIAL_LOG"
echo
echo "Avvio Bochs.  In un altro terminale:"
echo "    tail -F $SERIAL_LOG"
echo

: > "$SERIAL_LOG"
# Bochs su Arch è compilato con --enable-debugger, quindi all'avvio entra
# nel prompt "<bochs:1>" aspettando un comando sullo stdin.  Gli serviamo
# un flusso continuo di `c` (continue) così non si blocca al primo break
# e non si pianta su EOF se si verifica un panic in seguito.
exec yes c | bochs -q -f "$BOCHSRC"
