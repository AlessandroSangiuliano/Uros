#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# #329: hunt the rare near-NULL AP crash under HOST CPU PRESSURE.  Oversubscribing
# the 8 host cores preempts qemu vCPUs mid-critical-section and widens the SMP
# race window (the crash clustered when host load was ~7-8, vanished at ~4).
# We add CPU burners, then boot smp8 with the full bench until the in-kernel
# KFAULT auto-backtrace fires.  ONE qemu at a time.  Burners killed by PID only
# (never qemu, never pkill).
set -u
# ⚠️ Derived, not written down.  It used to be the literal path
# /home/slex/Scrivania/osfmk-mklinux, which does not exist -- so this script
# could not run, and nothing said so, because a tool nobody invokes is
# checked by nothing.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NBURN=${1:-8}
MAXR=${2:-15}

BURN_PIDS=()
for i in $(seq 1 "$NBURN"); do
    ( while :; do :; done ) &
    BURN_PIDS+=($!)
done
echo "started $NBURN burners: ${BURN_PIDS[*]}"
cleanup() { for p in "${BURN_PIDS[@]}"; do kill "$p" 2>/dev/null; done; echo "burners killed"; }
trap cleanup EXIT TERM INT

hit=0
for n in $(seq 1 "$MAXR"); do
    L="$ROOT/uros/build/kfl-$n.log"; rm -f "$L"
    timeout 260 "$ROOT/scripts/run-qemu.sh" --smp 8 --bench comb cc inter pp \
        -serial file:"$L" -display none > /dev/null 2>&1
    if grep -aqE "KFAULT|ds_read_done: NULL io_data" "$L"; then
        if grep -aq "KFAULT" "$L"; then
            echo "=== KFAULT (FIX FAILED) on attempt $n (load $(cut -d' ' -f1 /proc/loadavg)) ==="
        else
            echo "=== CLAMP HIT (fix caught it) on attempt $n (load $(cut -d' ' -f1 /proc/loadavg)) ==="
            grep -a "ds_read_done: NULL io_data" "$L"
        fi
        cp "$L" "$ROOT/uros/build/kf-HIT.log"
        hit=1; break
    fi
    echo "attempt $n: clean (load $(cut -d' ' -f1 /proc/loadavg), $(grep -ac 'Benchmark complete' "$L") bench-done)"
    rm -f "$L"
done
[ "$hit" = 0 ] && echo "no KFAULT in $MAXR loaded attempts"
