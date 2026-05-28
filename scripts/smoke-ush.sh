#!/usr/bin/env bash
#
# smoke-ush.sh — thin wrapper around smoke-ush.exp (#275.6).
#
# Runs the ush acceptance smoke test and forwards expect's exit code.
# Use --log <file> to also capture the full transcript (boot output
# is noisy; the expect script itself only emits PASS/FAIL lines).
#
# Examples:
#   ./scripts/smoke-ush.sh
#   ./scripts/smoke-ush.sh --log /tmp/smoke.log
#
# Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# License: MIT
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXP="$SCRIPT_DIR/smoke-ush.exp"

if ! command -v expect >/dev/null 2>&1; then
    echo "smoke-ush: 'expect' not found — install it (Arch: pacman -S expect)" >&2
    exit 2
fi

LOGFILE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --log) LOGFILE="$2"; shift 2 ;;
        -h|--help) sed -n '3,14p' "$0"; exit 0 ;;
        *) echo "smoke-ush: unknown option: $1" >&2; exit 2 ;;
    esac
done

if [ -n "$LOGFILE" ]; then
    "$EXP" 2>&1 | tee "$LOGFILE"
    exit "${PIPESTATUS[0]}"
else
    exec "$EXP"
fi
