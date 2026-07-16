#!/usr/bin/env bash
#
# smoke-ush.sh — thin wrapper around smoke-ush.exp (#275.6).
#
# Runs the ush acceptance smoke test and forwards expect's exit code.
# Use --log <file> to also capture the full transcript (boot output
# is noisy; the expect script itself only emits PASS/FAIL lines).
#
# Use --smp N to run the smoke on N CPUs (forwarded to run-qemu.sh).  The
# v0.2.0 acceptance checklist (#376) asks for the smoke at -smp 8; without
# this the harness could only ever run the default, so that box could not be
# checked honestly.
#
# Examples:
#   ./scripts/smoke-ush.sh
#   ./scripts/smoke-ush.sh --log /tmp/smoke.log
#   ./scripts/smoke-ush.sh --smp 8
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
EXP_ARGS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --log) LOGFILE="$2"; shift 2 ;;
        --smp) EXP_ARGS="$EXP_ARGS --smp $2"; shift 2 ;;
        -h|--help) sed -n '3,20p' "$0"; exit 0 ;;
        *) echo "smoke-ush: unknown option: $1" >&2; exit 2 ;;
    esac
done

# $EXP_ARGS is deliberately unquoted: it must word-split into separate argv
# entries for the expect script.
if [ -n "$LOGFILE" ]; then
    "$EXP" $EXP_ARGS 2>&1 | tee "$LOGFILE"
    exit "${PIPESTATUS[0]}"
else
    exec "$EXP" $EXP_ARGS
fi
