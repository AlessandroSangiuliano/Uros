#!/usr/bin/env bash
#
# run-ush.sh — boot Uros in minimal mode (no bench/test tasks) and
# drop into an interactive ush session over the serial console.
#
# Usage:
#   ./scripts/run-ush.sh                  # boot to ush prompt (Ctrl-A x to quit)
#   ./scripts/run-ush.sh --log out.log    # also tee serial output to a file
#
# Notes:
#   - QEMU runs with `-nographic` so the terminal becomes the COM1 of
#     the guest in raw mode.  Exit with Ctrl-A then x.
#   - The bundle is rebuilt with --minimal: only name_server, cap_server,
#     gpu_server, char_server, hal_server, block_device_server,
#     default_pager, ext_server, exec_server, proc_server, and ush.
#   - Boot output (ext2 mounts, etc.) still appears before ush$ — scroll
#     up or use --log to inspect.  After ush$ you can type commands.
#
# Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# License: MIT

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

LOGFILE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --log) LOGFILE="$2"; shift 2 ;;
        -h|--help)
            sed -n '3,18p' "$0"
            exit 0
            ;;
        *) echo "Opzione sconosciuta: $1" >&2; exit 1 ;;
    esac
done

echo "[run-ush] launching QEMU --minimal (Ctrl-A x to quit)..."
EXTRA="-display none -serial mon:stdio"
if [ -n "$LOGFILE" ]; then
    EXTRA="$EXTRA -D $LOGFILE"
fi
exec "$SCRIPT_DIR/run-qemu.sh" --minimal $EXTRA
