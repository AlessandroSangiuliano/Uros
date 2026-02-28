#!/usr/bin/env sh
# Avvia Uros sotto QEMU: kernel multiboot + bootstrap server come modulo.
# Uso: ./scripts/run-qemu.sh [opzioni-qemu-extra]
# Esempio headless: ./scripts/run-qemu.sh -nographic -serial mon:stdio
set -e

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$REPO_ROOT/osfmk/build"
KERNEL="$BUILD_DIR/export/osfmk/boot/mach_kernel"
BOOTSTRAP="$BUILD_DIR/export/osfmk/$(uname -m)/user/sbin/bootstrap"

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

exec qemu-system-i386 \
    -m 128M \
    -kernel "$KERNEL" \
    -initrd "$BOOTSTRAP" \
    -no-reboot \
    "$@"
