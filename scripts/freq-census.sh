#!/usr/bin/env bash
#
# freq-census.sh — what this machine says about its clocks' frequencies, on
# the host and inside every guest configuration the suites use (#508).
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# Usage: scripts/freq-census.sh [output-dir]
#
# Two halves, and the second takes about ten boots:
#
#   1. the host's own processor, read from user space: CPUID's highest basic
#      leaf, 0x15 and 0x16 where they exist, the hypervisor bit, the
#      invariant-TSC bit and the vendor.  Bare metal is the only place where
#      0x15's crystal is a fact rather than whatever a hypervisor chose to
#      pass on.
#   2. the kernel's own census and rulers, entry 14, under TCG {max, qemu64}
#      and KVM {max, qemu64, host}, each on the pc and the q35 board.  Each
#      boot keeps its whole serial log; the summary keeps, per boot, the
#      "frequency census" lines, the 8254 calibration, the TSC measured
#      against the PM timer and the HPET, the vote between the three rulers,
#      the refinement, and the verdict.
#
# Around the boots the summary also records the host's NTP state, before and
# after.  Inside a guest the 8254, the PM timer and the HPET are QEMU's, and
# they run on the host's CLOCK_MONOTONIC: with clocksource tsc, that is the
# TSC corrected by NTP's frequency and by whatever offset the kernel's PLL is
# still slewing.  So the rulers read the host's correction, not a defect of
# either side, and the numbers cannot be read without it (OMEGA, victus and
# omen each had to take it by hand, #508).
#
# ⚠️ run-x86_64.sh rebuilds the kernel from THIS tree on every boot, so the
# tree must not be edited while this runs; the summary's first line records
# the commit and how many files differ from it.  And one QEMU at a time: the
# boots are sequential, and nothing else should be booting beside them.
#
# The summary is meant to be pasted, whole, into #508.

set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
HOST=$(hostname)
OUT=${1:-$HOME/uros-tests/freq-census-$HOST-$(date +%Y%m%d-%H%M)}
mkdir -p "$OUT" || exit 1
SUM=$OUT/summary.txt

{
	echo "freq-census on $HOST, tree $(git -C "$REPO" rev-parse --short HEAD)" \
	     "($(git -C "$REPO" status --short | wc -l) files differ)"
	bash -c ". '$REPO/scripts/run-conditions.sh'; uros_host_state" 2>/dev/null
	echo "host CPU: $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')"
} > "$SUM"

# ── 1. the host ──────────────────────────────────────────────────────────
cat > "$OUT/cpuid-host.c" <<'EOF'
#include <cpuid.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	unsigned a, b, c, d, max;
	char vendor[13];

	__cpuid(0, max, b, c, d);
	memcpy(vendor, &b, 4);
	memcpy(vendor + 4, &d, 4);
	memcpy(vendor + 8, &c, 4);
	vendor[12] = 0;
	printf("host: vendor %s, CPUID highest leaf 0x%x", vendor, max);
	if (max >= 0x15) {
		__cpuid_count(0x15, 0, a, b, c, d);
		printf("; 0x15 crystal %u Hz, TSC/crystal %u/%u", c, b, a);
	} else
		printf("; 0x15 not offered");
	if (max >= 0x16) {
		__cpuid_count(0x16, 0, a, b, c, d);
		printf("; 0x16 base %u max %u bus %u MHz",
		       a & 0xffff, b & 0xffff, c & 0xffff);
	} else
		printf("; 0x16 not offered");
	__cpuid(1, a, b, c, d);
	printf("; hypervisor bit %u", (c >> 31) & 1);
	__cpuid(0x80000007, a, b, c, d);
	printf("; invariant TSC %u\n", (d >> 8) & 1);
	return 0;
}
EOF
if cc -O2 -o "$OUT/cpuid-host" "$OUT/cpuid-host.c"; then
	"$OUT/cpuid-host" >> "$SUM"
else
	echo "host: the CPUID reader did not compile" >> "$SUM"
fi
# What the host kernel calibrated, and which clock it trusts: the number a
# guest's paravirtual clock will repeat, and the reason it may not be the TSC.
# The refined value is the one kvmclock repeats, and it can sit thousands of
# ppm from the quick "Detected" one (omen: 2799.927 then 2808.000 MHz).  A
# host that disabled its HPET says so here too (omen: dysfunctional in PC10).
journalctl -k -b --no-pager 2>/dev/null \
	| grep -o "tsc: Detected [0-9.]* MHz.*\|tsc: Refined TSC clocksource calibration: [0-9.]* MHz\|Marking TSC unstable.*\|Switched to clocksource [a-z_-]*\|hpet: HPET dysfunctional.*" \
	| sed 's/^/host kernel: /' >> "$SUM"

# The host's NTP, as timesyncd reports it and as the kernel's PLL holds it.
# adjtimex's offset is what the PLL is still slewing; in nanosecond mode with
# time constant c it slews 1/2^(2+c) of it every second (#508, omen).
host_ntp() {
	local when=$1 ts
	ts=$(timedatectl timesync-status 2>/dev/null \
		| sed -n 's/^ *\(Offset\|Frequency\|Packet count\|Poll interval\): */\1 /p' \
		| paste -sd ';' -)
	echo "host NTP $when ($(date +%H:%M:%S)): timesyncd ${ts:-not available}"
	python3 - "$when" <<'EOF' 2>/dev/null || echo "host NTP $1: adjtimex not read"
import ctypes, sys, time

class Timex(ctypes.Structure):
    _fields_ = [("modes", ctypes.c_uint), ("offset", ctypes.c_long),
                ("freq", ctypes.c_long), ("maxerror", ctypes.c_long),
                ("esterror", ctypes.c_long), ("status", ctypes.c_int),
                ("constant", ctypes.c_long), ("rest", ctypes.c_long * 32)]

t = Timex()
state = ctypes.CDLL(None).adjtimex(ctypes.byref(t))
unit = 1e6 if t.status & 0x2000 else 1e3   # STA_NANO: ns, otherwise us
print("host NTP %s (%s): adjtimex state %d, status 0x%x, offset %+.3f ms, "
      "freq %+.3f ppm, constant %d" % (sys.argv[1], time.strftime("%H:%M:%S"),
      state, t.status, t.offset / unit, t.freq / 65536.0, t.constant))
EOF
}
host_ntp before >> "$SUM"

# ── 2. the guests ────────────────────────────────────────────────────────
cd "$REPO" || exit 1
for acc in tcg kvm; do
	if [ $acc = tcg ]; then cpus="max qemu64"; flag=""; else cpus="max qemu64 host"; flag="--kvm"; fi
	for cpu in $cpus; do
		for board in pc q35; do
			tag=$acc-$cpu-$board
			args="-cpu $cpu"
			[ $board = q35 ] && args="$args -machine q35"
			echo "== $tag ($(date +%H:%M:%S))" >> "$SUM"
			UROS_X86_64_LOG=$OUT/$tag.log \
				./scripts/run-x86_64.sh $flag --entry 14 300 $args \
				> "$OUT/$tag.out" 2>&1
			echo "   exit $?" >> "$SUM"
			grep -a "frequency census\|timestamp counter measured\|ACPI PM timer: \|UrMach x86-64: HPET: \|the TSC against each ruler\|the TSC refined" \
				"$OUT/$tag.log" | sed 's/UrMach x86-64: //' | cut -c1-330 >> "$SUM"
			grep -a "=== verdict\|^  passed\|^  FAILED" "$OUT/$tag.out" >> "$SUM"
		done
	done
done
host_ntp after >> "$SUM"
echo "done $(date +%H:%M:%S)" >> "$SUM"
echo "summary: $SUM"
