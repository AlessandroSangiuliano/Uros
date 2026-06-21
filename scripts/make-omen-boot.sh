#!/usr/bin/env sh
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# make-omen-boot.sh — stage a bare-metal boot image for "omen" (i7 UEFI, no
# serial port), to measure the SMP lock-free work (#330/#331) on real silicon
# (#332).  Diskless: GRUB loads kernel + bootstrap + bundle (which already
# contains ipc_bench) as multiboot2 modules straight into RAM, so Uros never
# needs a USB/AHCI driver — the firmware + GRUB do all the reading at boot.
#
# Unlike the QEMU harness (fbcons-uefi-test.py) and the Bochs script
# (run-bochs.sh), the GRUB config here has NO serial dependency: omen has no
# COM port, so `serial`/`terminal_output serial` would stall GRUB.  Output goes
# to the in-kernel framebuffer console (fbcons, #342), enabled with `-f`.
#
# It produces TWO ready-to-use paths (pick one):
#
#   1. USB stick  — a hybrid (BIOS + UEFI) ISO you `dd` onto a stick, then boot
#                   via omen's one-time boot menu (F9).  Zero changes to omen's
#                   disk or its Manjaro/Windows GRUB.
#   2. Existing GRUB — a menuentry snippet for omen's installed (Manjaro) GRUB,
#                   so it boots Uros from disk with no USB at all.  Additive:
#                   it does not touch the Manjaro/Windows entries.
#
# Building the ISO needs grub-mkrescue + xorriso + mtools (no root).  The `dd`
# and the on-omen copy are manual steps YOU run (this script never writes a
# device and never needs sudo).
#
# Usage:  scripts/make-omen-boot.sh            # build everything (default)
#         scripts/make-omen-boot.sh --iso      # only the USB ISO
#         scripts/make-omen-boot.sh --grub     # only the existing-GRUB snippet

set -eu

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$REPO_ROOT/uros/build"

KERNEL="$BUILD/export/uros/boot/mach_kernel"
BOOTSTRAP="$BUILD/export/uros/$(uname -m)/user/sbin/bootstrap"
BUNDLE="$BUILD/bootstrap.bundle"
KSYMS="$BUILD/export/uros/boot/ksyms.bin"

ISO_TREE="$BUILD/omen-iso"
ISO="$BUILD/uros-omen.iso"
SNIPPET="$BUILD/omen-grub-snippet.cfg"

WANT_ISO=true
WANT_GRUB=true
case "${1:-}" in
    --iso)  WANT_GRUB=false ;;
    --grub) WANT_ISO=false ;;
    "")     ;;
    *) echo "Unknown flag: $1" >&2; exit 2 ;;
esac

for f in "$KERNEL" "$BOOTSTRAP" "$BUNDLE"; do
    [ -f "$f" ] || { echo "ERROR: missing $f (build the kernel first)"; exit 1; }
done
HAVE_KSYMS=false
[ -f "$KSYMS" ] && HAVE_KSYMS=true

# The ksyms module line is only emitted when ksyms.bin exists: a multiboot2
# `module2` pointing at a missing file makes GRUB abort the boot.
ksyms_iso_line=""
ksyms_grub_line=""
if [ "$HAVE_KSYMS" = true ]; then
    ksyms_iso_line="    module2    /boot/ksyms.bin        ksyms"
    ksyms_grub_line="    module2    /uros/ksyms.bin        ksyms"
fi

# -------- 1) USB ISO -------------------------------------------------------
if [ "$WANT_ISO" = true ]; then
    rm -rf "$ISO_TREE"
    mkdir -p "$ISO_TREE/boot/grub"
    cp "$KERNEL"    "$ISO_TREE/boot/mach_kernel"
    cp "$BOOTSTRAP" "$ISO_TREE/boot/bootstrap"
    cp "$BUNDLE"    "$ISO_TREE/boot/bootstrap.bundle"
    [ "$HAVE_KSYMS" = true ] && cp "$KSYMS" "$ISO_TREE/boot/ksyms.bin"

    # No serial.  `insmod all_video` pulls the right backend for the platform
    # (efi_gop on UEFI, vbe on legacy/CSM); gfxpayload makes GRUB hand the
    # kernel a linear framebuffer via the mb2 framebuffer tag -> fbcons.  `-f`
    # turns fbcons on (off by default so the userspace gpu_server owns the FB).
    {
        printf 'set timeout=5\n'
        printf 'set default=0\n'
        printf 'terminal_input  console\n'
        printf 'terminal_output console\n'
        printf 'insmod all_video\n'
        printf 'set gfxpayload=1024x768x32\n\n'
        printf 'menuentry "Uros (UrMach, fbcons)" {\n'
        printf '    multiboot2 /boot/mach_kernel -f\n'
        printf '    module2    /boot/bootstrap        bootstrap\n'
        printf '    module2    /boot/bootstrap.bundle bundle\n'
        [ -n "$ksyms_iso_line" ] && printf '%s\n' "$ksyms_iso_line"
        printf '    boot\n'
        printf '}\n'
    } >"$ISO_TREE/boot/grub/grub.cfg"

    echo "building hybrid ISO (BIOS + UEFI): $ISO"
    grub-mkrescue -o "$ISO" "$ISO_TREE" >/dev/null 2>&1
fi

# -------- 2) Existing-GRUB menuentry --------------------------------------
if [ "$WANT_GRUB" = true ]; then
    # `search --file` locates whichever partition holds /uros/mach_kernel and
    # sets it as root, so the entry works wherever you drop the files (Manjaro
    # root, the ESP, ...).  Append to /etc/grub.d/40_custom on omen.
    {
        printf 'menuentry "Uros (UrMach, fbcons)" {\n'
        printf '    insmod multiboot2\n'
        printf '    insmod all_video\n'
        printf '    search --no-floppy --file --set=root /uros/mach_kernel\n'
        printf '    set gfxpayload=1024x768x32\n'
        printf '    multiboot2 /uros/mach_kernel -f\n'
        printf '    module2    /uros/bootstrap        bootstrap\n'
        printf '    module2    /uros/bootstrap.bundle bundle\n'
        [ -n "$ksyms_grub_line" ] && printf '%s\n' "$ksyms_grub_line"
        printf '}\n'
    } >"$SNIPPET"
    echo "existing-GRUB menuentry: $SNIPPET"
fi

# -------- instructions -----------------------------------------------------
echo
echo "================================================================"
echo " Done.  Two ways to boot omen (pick one):"
echo "================================================================"
if [ "$WANT_ISO" = true ]; then
    cat <<EOF

 [1] USB stick  (zero changes to omen's disk / dual-boot GRUB)
     a) Identify the stick — CHECK THE SIZE, a wrong device wipes Windows:
            lsblk -o NAME,SIZE,MODEL,TRAN
     b) Unmount any of its partitions, then write the whole device:
            sudo dd if=$ISO of=/dev/sdX bs=4M oflag=sync status=progress
     c) On omen: power on, tap F9 (HP one-time boot menu), pick the stick.
        Try the LEGACY entry first (closest to our tested SeaBIOS/Bochs path);
        the UEFI entry is the GOP path.  Secure Boot must be OFF for UEFI.
EOF
fi
if [ "$WANT_GRUB" = true ]; then
    cat <<EOF

 [2] Existing Manjaro GRUB  (no USB; boots Uros from disk)
     On omen:
            sudo mkdir -p /uros
            sudo cp $KERNEL /uros/mach_kernel
            sudo cp $BOOTSTRAP /uros/bootstrap
            sudo cp $BUNDLE /uros/bootstrap.bundle
EOF
    [ "$HAVE_KSYMS" = true ] && echo "            sudo cp $KSYMS /uros/ksyms.bin"
    cat <<EOF
        (put /uros on any partition GRUB can read — the Manjaro ext4 root is fine)
        then append the menuentry and regenerate grub.cfg:
            cat $SNIPPET | sudo tee -a /etc/grub.d/40_custom
            sudo grub-mkconfig -o /boot/grub/grub.cfg
        Reboot and pick "Uros (UrMach, fbcons)".  Additive — Manjaro/Windows
        entries are untouched; to undo, delete the snippet from 40_custom and
        re-run grub-mkconfig.
EOF
fi
echo
echo " Either way: diskless, runs from RAM, ipc_bench prints to the screen via"
echo " fbcons.  Read the numbers off the display (no serial, no keyboard needed)."