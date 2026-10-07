#!/bin/bash
#
# merge-test-x86_64.sh -- the x86-64 entries a merge into the epic runs, and how each is judged (#616).
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
# Why this exists.  Each merge test so far was a script of its own, with its
# own list of entries, and entry 7 (state_test, #408) was in none of them: it
# said WRONG on every tree from #422 (12/08) to #616, and nothing ran it.  The
# list lives here now, beside the entries, and says for each one how it ends --
# the harness's verdict, or a verdict of its own for an entry that ends on
# purpose in a panic or a stop -- and, for the ones an unattended run cannot
# judge, why they are not run.  An entry added to grub.cfg and to nothing here
# is an entry no merge will run: say it here, one way or the other.
#
# Usage:  scripts/merge-test-x86_64.sh [-a tcg|kvm|both] [-s SMP] [-o DIR] [-e "N ..."] [-G]
#   -a  accelerator(s), default both     -s  processors, default 4
#   -o  directory for the logs, default ./merge-test-logs
#   -e  only these entries (to see one fail, or to rerun one); a merge runs them all
#   -G  no gdb stub; by default every run has one (-s, port 1234), so a run cut
#       short is walked by run-x86_64.sh before it is killed
# UROS_BUILD_DIR is passed through to run-x86_64.sh, so a build other than the
# default is tested the same way.  The exit status is the number of entries
# judged wrong (0: every judged entry as it should be).

set -u
ACC=both
SMP=4
OUT=./merge-test-logs
ONLY=""
STUB="-s"	# every run with the gdb stub: a run cut short is walked before it is killed (#526); -G turns it off
while getopts "a:s:o:e:G" o; do
	case $o in
	a) ACC=$OPTARG ;;
	s) SMP=$OPTARG ;;
	o) OUT=$OPTARG ;;
	e) ONLY=" $OPTARG " ;;
	G) STUB="" ;;
	*) sed -n '/^# Usage:/,/^# judged wrong/p' "$0"; exit 2 ;;
	esac
done
case $ACC in
tcg) ACCS="tcg" ;;
kvm) ACCS="kvm" ;;
both) ACCS="tcg kvm" ;;
*) echo "merge-test: -a is tcg, kvm or both, not '$ACC'" >&2; exit 2 ;;
esac
HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"
WRONG=0

# The entries, one per line: number, verdict, extra run-x86_64.sh options, and
# the processor count when it is not the run's.
#   harness   the harness's own verdict: "passed: reached the end, nothing unexplained"
#   stop13    the name server stops at service_checkin, which grub.cfg says is
#             the designed end: a boot task that IS the name server has nothing
#             above it to answer the check-in
#   ioapic    -Y's own line: "ioapic_race: PASS"
#   panic2    -Z panics on purpose; scripts/double-panic-check.sh judges it
ENTRIES="
0 harness
1 harness
5 harness
6 harness
7 harness
8 harness
9 harness
13 stop13
14 harness
14 harness - 1
15 harness
16 harness --iommu_amd
16 harness --iommu_intel
33 harness --iommu_intel
34 harness --iommu_intel
35 harness --iommu_intel
17 harness
18 harness
21 harness
22 harness
23 ioapic
24 panic2
25 harness
26 harness
27 harness
28 harness
29 harness
29 harness - 1
30 harness
30 harness - 1
31 harness
31 harness - 1
32 harness
32 harness - 1
3 harness
3 harness - 1
4 harness
4 harness - 1
20 harness
20 harness - 1
"

# Not run, and why -- said on every run so the list cannot shrink in silence.
SKIPPED="
2: the double-fault self-test ends in 'no handler -- halted' on purpose; it needs a verdict of its own
10: -rB opens the debugger's prompt and waits for someone to type
11: -rL opens the debugger's prompt on a thread with a continuation and waits
12: -rS stays up for the console door and never ends
19: -B panics on purpose; it needs a verdict of its own
"

judge() {	# entry verdict log-of-the-harness full-log
	local e=$1 v=$2 h=$3 f=$4
	case $v in
	harness) grep -aq 'passed: reached the end, nothing unexplained' "$h" ;;
	stop13) grep -aq 'name_server: service_checkin: (ipc/send) invalid destination port' "$f" ;;
	ioapic) grep -aq 'ioapic_race: PASS' "$f" ;;
	panic2) "$HERE/scripts/double-panic-check.sh" "$f" | grep -q 'PASS' ;;
	esac
}

# The stub listens on one port, and a qemu already on it would make every run here refuse to start.
if [ -n "$STUB" ] && ss -ltn 2>/dev/null | grep -q ':1234 '; then
	echo "merge-test: port 1234 is in use, so no run could start with the gdb stub -- free it, or -G to run without" >&2
	exit 2
fi
echo "merge-test: tree $(git -C "$HERE" describe --always --dirty), build ${UROS_BUILD_DIR:-default}, accelerators: $ACCS, -smp $SMP${ONLY:+, only entries$ONLY}${STUB:+, gdb stub on every run}"
echo "$SKIPPED" | sed '/^$/d; s/^/merge-test: not run -- /'
while read -r e v opt cpus; do
	[ -n "$e" ] || continue
	[ -z "$ONLY" ] || [[ "$ONLY" == *" $e "* ]] || continue
	[ "$opt" = "-" ] && opt=""
	opt=${opt//_/ }
	n=${cpus:-$SMP}
	for a in $ACCS; do
		budget=1200
		k=""
		[ $a = kvm ] && { budget=600; k="--kvm"; }
		tag="e$e-$a$n${opt:+-${opt##* }}"
		h="$OUT/$tag.log"
		f="$OUT/$tag-full.log"
		( cd "$HERE" && UROS_X86_64_LOG="$f" ./scripts/run-x86_64.sh $k $opt --entry "$e" $budget -smp "$n" $STUB ) > "$h" 2>&1
		if judge "$e" "$v" "$h" "$f"; then
			echo "merge-test: entry $e $a -smp $n ${opt:+($opt) }-- as it should be ($v)"
		else
			echo "merge-test: entry $e $a -smp $n ${opt:+($opt) }-- WRONG ($v): $h"
			WRONG=$((WRONG + 1))
		fi
	done
done <<< "$ENTRIES"
echo "merge-test: $WRONG wrong"
exit $WRONG
