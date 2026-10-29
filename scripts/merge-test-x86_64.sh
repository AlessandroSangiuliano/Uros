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
# is an entry no merge will run: say it here, one way or the other.  That
# sentence did not keep it so -- entry 49 (#659) was in neither list from 11/10
# to #755 -- so the script refuses to run until each entry of grub.cfg is in
# exactly one list and each list names only entries grub.cfg has (lists_cover).
#
# Usage:  scripts/merge-test-x86_64.sh [-a tcg|kvm|both] [-s SMP] [-o DIR] [-e "N ..."] [-G] [-L] [-P]
#   -a  accelerator(s), default both     -s  processors, default 4
#   -o  directory for the logs, default ./merge-test-logs, or beside the log
#       UROS_X86_64_LOG names, as ablate.sh's Script: runs name each arm's (#591)
#   -e  only these entries (to see one fail, or to rerun one); a merge runs them all
#   -G  no gdb stub; by default every run has one (-s, port 1234), so a run cut
#       short is walked by run-x86_64.sh before it is killed
#   -L  only ask whether each grub.cfg entry is run or said not run, and say so:
#       0 if each is, 1 if not.  ci/job.sh asks it before it builds (#755)
#   -P  only ask whether every patch of ablations/ still applies to this tree,
#       and say so: 0 if each does, 1 if not (#769).  A whole run asks it too
# UROS_BUILD_DIR is passed through to run-x86_64.sh, so a build other than the
# default is tested the same way; its char build, which the entries marked @char
# boot, is that build's path with -char after it (#765).  A run that refuses to
# start -- a bad option, the stub's port taken, the lists and grub.cfg not
# agreeing, the patches not asked -- exits 2, and otherwise the exit status is
# the number of entries judged wrong, and one more if a patch of ablations/ no
# longer applies (0: every judged entry as it should be, every patch applying).

set -u
ACC=both
SMP=4
OUT=./merge-test-logs
# #755: run as a Script: of ablate.sh, which names the arm's own log in
# UROS_X86_64_LOG (#591), the boots' logs go beside it, a directory an arm,
# and not into a ./merge-test-logs of the caller's that every arm would share.
[ -n "${UROS_X86_64_LOG:-}" ] && OUT=${UROS_X86_64_LOG%.log}-merge-test
ONLY=""
STUB="-s"	# every run with the gdb stub: a run cut short is walked before it is killed (#526); -G turns it off
LISTS_ONLY=0
PATCHES_ONLY=0
while getopts "a:s:o:e:GLP" o; do
	case $o in
	a) ACC=$OPTARG ;;
	s) SMP=$OPTARG ;;
	o) OUT=$OPTARG ;;
	e) ONLY=" $OPTARG " ;;
	G) STUB="" ;;
	L) LISTS_ONLY=1 ;;
	P) PATCHES_ONLY=1 ;;
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
WRONG=0
# #705: the boots whose judge passed them with a question not asked, counted
# and named at the end, so that a question going unasked is seen in the total
# and not only read off its line.
UNASKED=0 UNASKED_WHERE=""
# And the boots that passed because a breakpoint nobody here armed was cleared
# and resumed (#742): what armed it was outside the guest, so they are as they
# should be, and counted apart so that how often it happens stays seen.
CAMEBACK=0 CAMEBACK_WHERE=""

# The entries, one per line: number, verdict, extra run-x86_64.sh options, and
# the processor count when it is not the run's.  @char among the options boots
# the char build instead of the build the run was given (#765; char_build below).
#   harness   the harness's own verdict: "passed: reached the end, nothing unexplained"
#   char      the harness's verdict, of the char build: COM1 handed over to
#             char_server, its klog forwarder started, and char_test's count of
#             arms -- what "harness" would pass a build without char_server
#             on, since nothing asks for them there (#765)
#   stop13    the name server stops at service_checkin, which grub.cfg says is
#             the designed end: a boot task that IS the name server has nothing
#             above it to answer the check-in
#   ioapic    -Y's own line: "ioapic_race: PASS"
#   msirace   -m's own line: "msi_race: PASS"
#   panic2    -Z panics on purpose; scripts/double-panic-check.sh judges it
#   reach     the harness's verdict, entries for 32-bit apic ids, and a pin and an
#             MSI-X slot seen arriving at the processor they were aimed at (#659)
#   keptreach reach, of an x2APIC mode GRUB left on and the kernel kept: its
#             "kept as the firmware left it" line as well (#755)
#   earlypanic  -p panics on purpose before the boot processor has its page:
#             its message and the backtrace after it, and no protection fault
#             in their place (#665)
#   pcidx     the harness's verdict and, where the processor offers PCID, -x's
#             mode (PCID without INVPCID) with pcid_test's [1] and [2] lines;
#             a pass on a question not asked says NOT ASKED in its verdict (#412)
#   pcidn     the harness's verdict, -n's line and pcid_test NOT ASKED, or no
#             PCID offered, said as NOT ASKED (#412)
#   pcidd     the harness's verdict and, where the processor offers PCID, the
#             default mode -- with INVPCID where the host has it, without where
#             it does not -- with pcid_test's [1], [2] and [3] lines (#412)
#   taskpanic -t panics inside a system call while another processor is held
#             in a task's code, and no processor reports a trap of its own
#             after it; NOT ASKED when no second processor came (#688)
#   ddbreport -t's panic with the debugger armed: before its first prompt the
#             debugger said the backtrace and every processor
#             (scripts/ddb-report-check.sh, #750); NOT ASKED when -t never
#             asked, so the prompt never opened
#   halted    the harness's verdict, and the console's final copy whole after
#             the machine stops (scripts/final-copy-check.sh, #702)
#   refusedfault  -j: a fault from ring 0 on a task's present page that SMAP or
#             SMEP refused is reported, not retried for ever; the probe's own
#             PASS/WRONG/NOT ASKED verdict (refused_fault_test, #750)
# A processor count may be a whole -smp value: entry 14 with --socket1-cpu has
# two processors, APIC ids 0 and 64, in a MADT of 128 entries (#663).
# Entries 46 and 47 run in x2APIC mode: turned on by the kernel (-2), and left on
# by GRUB before the kernel, as a firmware that hands over in it does (#659).
# Entry 48 adds -i with engines that take x2APIC ids (--iommu intel-eim, amd-xt),
# and 37 runs on processors 0 and 256, x2APIC mode left on by SeaBIOS, so that
# the interrupts aimed at 256 need all 32 bits; under KVM the harness asks
# those engines for the split irqchip.  Entry 49 is 47's mode with -i: GRUB
# leaves x2APIC mode on, the kernel keeps it, and the same two engines remap
# it through entries for 32-bit ids (#755).
ENTRIES="
0 pcidd
1 harness
5 harness
6 harness
7 harness
8 harness
9 harness
13 stop13
14 harness
14 harness - 1
14 harness --socket1-cpu 1,sockets=2,cores=64,maxcpus=128
14 char @char
15 harness
16 harness --iommu_amd
16 harness --iommu_intel
36 harness --iommu_intel
37 harness --iommu_intel
38 harness --iommu_intel
36 harness --iommu_amd
37 harness --iommu_amd
38 harness --iommu_amd
38 hpetfsb --iommu_intel_--hpet_fsb
45 harness --iommu_intel
45 harness --iommu_amd
17 harness
18 harness
21 halted
22 harness
22 hpetgaps - 6,sockets=2,cores=3
22 hpetfsb --hpet_fsb
23 ioapic
39 msirace --iommu_intel
24 panic2
40 earlypanic
53 taskpanic
55 ddbreport
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
33 harness
33 harness - 1
34 harness
34 harness - 1
35 harness
35 harness - 1
3 harness
3 harness - 1
4 harness
4 harness - 1
20 harness
20 harness - 1
41 early
42 restart
42 restart --iommu_intel
43 restartcensus
44 restartpanic
52 restarthold
46 harness
46 harness - 1
47 harness
47 harness - 1
48 reach --iommu_intel-eim
48 reach --iommu_amd-xt
37 reach --socket1-cpu_--iommu_intel-eim 1,sockets=2,cores=256,maxcpus=512
37 reach --socket1-cpu_--iommu_amd-xt 1,sockets=2,cores=256,maxcpus=512
49 keptreach --iommu_intel-eim
49 keptreach --iommu_amd-xt
50 pcidx
51 pcidn
56 refusedfault
56 refusedfault - 1
"

# Not run, and why -- said on every run so the list cannot shrink in silence.
SKIPPED="
2: the double-fault self-test ends in 'no handler -- halted' on purpose; it needs a verdict of its own
10: -rB opens the debugger's prompt and waits for someone to type
11: -rL opens the debugger's prompt on a thread with a continuation and waits
12: -rS stays up for the console door and never ends
19: -B panics on purpose; it needs a verdict of its own
54: -e asks only of a build with UROS_SYSCALL_PROFILE, which a merge build is not; ablate.sh runs it with the widen patch, which compiles the profile in (#622)
57: -u asks only of a build whose mutex_unlock() calls the release-window probe, which a merge build's does not; ablate.sh runs it with the widen patch, which adds the call (#753)
"

# #755: the two lists and grub.cfg agree, or nothing runs.  Entry 49, x2APIC
# mode left on with interrupts remapped (#659), went into grub.cfg on 11/10
# and into neither list, and no merge and no campaign booted it until #755:
# only the sentence at the top asked for it.  So an entry of grub.cfg in
# neither list, an entry in both, and a list naming an entry grub.cfg does not
# have are each a refusal, and named.  Entries are counted, not read by title:
# an entry's number is where it sits in grub.cfg.
GRUB_CFG=$HERE/uros/src/mach_kernel/x86_64/boot/grub.cfg
lists_cover() {
	local n e title run skip bad=0
	n=$(grep -c '^menuentry ' "$GRUB_CFG")
	if [ "$n" -eq 0 ]; then
		echo "merge-test: lists: no menuentry read out of $GRUB_CFG" >&2
		return 1
	fi
	run=$(awk '$1 ~ /^[0-9]+$/ { print $1 }' <<< "$ENTRIES" | sort -un)
	skip=$(awk -F: '$1 ~ /^[0-9]+$/ { print $1 }' <<< "$SKIPPED" | sort -un)
	for ((e = 0; e < n; e++)); do
		grep -qx "$e" <<< "$run" && continue
		grep -qx "$e" <<< "$skip" && continue
		title=$(awk -v n="$e" '/^menuentry / { if (i++ == n) { sub(/^menuentry "/, ""); sub(/" \{$/, ""); print; exit } }' "$GRUB_CFG")
		echo "merge-test: lists: entry $e is in grub.cfg and in neither list -- run it in ENTRIES with a verdict, or say in SKIPPED why not: \"$title\"" >&2
		bad=1
	done
	for e in $run; do
		grep -qx "$e" <<< "$skip" || continue
		echo "merge-test: lists: entry $e is run in ENTRIES and said not run in SKIPPED" >&2
		bad=1
	done
	for e in $run $skip; do
		[ "$e" -lt "$n" ] && continue
		echo "merge-test: lists: entry $e is in a list, and grub.cfg has no entry $e: its $n entries are 0 to $((n - 1))" >&2
		bad=1
	done
	[ $bad = 0 ] &&
		echo "merge-test: lists: each of grub.cfg's $n entries is run or said not run: $(wc -w <<< "$run") run, $(wc -w <<< "$skip") not"
	return $bad
}
# #769: whether every patch of ablations/ still applies to this tree, asked of
# ablate.sh --check-all, which ci/job.sh asks too.  A merge moves the code under
# the patches, and one that rots a patch must not pass the test it is decided
# on: #757's merge passed this script, 160 boots and 0 wrong, rotted three
# patches, and only the campaign after the push said so, by failing.  Returns
# 0 if each applies, 1 if one does not, 2 if the question could not be asked;
# the verdict, one line, in PATCHES_SAID.
PATCHES_SAID=""
patches_apply() {
	local scratch out rc
	scratch=$(mktemp -d) || { PATCHES_SAID="merge-test: ablations: no scratch directory to ask in"; return 2; }
	out=$(UROS_ABLATE_DIR=$scratch "$HERE/scripts/ablate.sh" --check-all 2>&1)
	rc=$?
	rm -rf "$scratch"
	case $rc in
	0)	PATCHES_SAID=$(sed -n 's/^ablate: \(all .*\)$/merge-test: ablations: \1/p' <<< "$out")
		[ -n "$PATCHES_SAID" ] && return 0 ;;
	2)	PATCHES_SAID=$(sed -n 's/^ablate: \([0-9]* of the .* no longer apply .*\)$/merge-test: ablations: \1 -- WRONG (#769)/p' <<< "$out")
		[ -n "$PATCHES_SAID" ] && return 1 ;;
	esac
	printf '%s\n' "$out" | tail -n 10 >&2
	PATCHES_SAID="merge-test: ablations: ablate.sh --check-all did not answer (status $rc)"
	return 2
}
if [ $LISTS_ONLY = 1 ]; then
	lists_cover
	exit
fi
if [ $PATCHES_ONLY = 1 ]; then
	patches_apply
	rc=$?
	echo "$PATCHES_SAID"
	exit $rc
fi
lists_cover || exit 2
patches_apply
ROTTEN=$?
echo "$PATCHES_SAID"
[ $ROTTEN -le 1 ] || exit 2
mkdir -p "$OUT"

# #412: the PCID entries' half of judge().  "Not offered" is taken at the
# kernel's word only where it can be true: under KVM on a host whose own
# processor has PCID it is a detection lost, WRONG; under TCG, or on a host
# without PCID, the flag had nothing to change, and the verdict says NOT ASKED,
# as it does when [1] or [2] could not judge -- a pass that asked nothing is
# not written as one that did (#412's second review).  NOTE carries the words.
host_has_pcid() { grep -qw pcid /proc/cpuinfo; }
host_has_invpcid() { grep -qw invpcid /proc/cpuinfo; }

pcid_judge() {	# x|n|d accelerator full-log
	local flag=$1 a=$2 f=$3 lacks
	if grep -aq 'UrMach x86-64: PCID not offered by this cpu' "$f"; then
		if [ "$a" = kvm ] && host_has_pcid; then
			NOTE="no PCID seen under KVM on a host that has it"
			return 1
		fi
		NOTE="NOT ASKED: no PCID offered"
		return 0
	fi
	# A refusal is a failure of x and d, named rather than silent: neither
	# reaches the merge test's machines -- KVM is exempt from the erratum
	# and every board has global pages -- so one that does is news (the
	# cloud audit of 14/10).
	if [ "$flag" != n ] && grep -aq 'UrMach x86-64: PCID off: ' "$f"; then
		NOTE="refused: $(grep -a -m1 -o 'UrMach x86-64: PCID off: [^,]*' "$f" | cut -c26-)"
		return 1
	fi
	# Which INVPCID words a mode may say is the host's to decide: where it
	# has INVPCID, -x must be what took it away, and the default must have
	# it (the cloud audit of 14/10: "which this cpu lacks" passed on a host
	# that had it, and nothing asked what the default mode was).
	if host_has_invpcid; then lacks='as -x asks'; else lacks='which this cpu lacks'; fi
	case $flag in
	x)	grep -aq "UrMach x86-64: PCID on, without INVPCID, $lacks" "$f" &&
			grep -aq 'pcid_test: \[1\] ' "$f" && grep -aq 'pcid_test: \[2\] ' "$f" || return 1
		grep -aqE 'pcid_test: \[[12]\] .*NOT ASKED' "$f" && NOTE="NOT ASKED by [1] or [2]"
		return 0 ;;
	d)	if host_has_invpcid; then
			grep -aq 'UrMach x86-64: PCID on, with INVPCID' "$f"
		else
			grep -aq 'UrMach x86-64: PCID on, without INVPCID, which this cpu lacks' "$f"
		fi && grep -aq 'pcid_test: \[1\] ' "$f" && grep -aq 'pcid_test: \[2\] ' "$f" &&
			grep -aq 'pcid_test: \[3\] ' "$f" || return 1
		grep -aqE 'pcid_test: \[[123]\] .*NOT ASKED' "$f" && NOTE="NOT ASKED by [1], [2] or [3]"
		return 0 ;;
	n)	grep -aq 'UrMach x86-64: PCID off, as -n asks' "$f" &&
			grep -aq 'pcid_test: NOT ASKED' "$f" ;;
	esac
}

# #688: whether -t held a processor at all, which the kernel says at the first
# hold.  A boot with no -t panic is then one of two things to look into: a
# processor held while none came inside the kernel, or no processor held.
taskpanic_held() {	# full-log
	if grep -aq "clock_event: -t holds cpu [0-9]* in a task's code, its first hold" "$1"; then
		echo "a processor was held, and no other came inside the kernel on a task's thread"
	else
		echo "no processor was ever held in a task's code"
	fi
}

judge() {	# entry verdict log-of-the-harness full-log accelerator
	local e=$1 v=$2 h=$3 f=$4 a=$5 out
	NOTE=""
	case $v in
	harness) grep -aq 'passed: reached the end, nothing unexplained' "$h" ;;
	# #765: the char build -- char_server in the bundle.  Its three lines are
	# asked for because nothing else does: a bundle without char_server
	# passes "harness", and run-x86_64.sh's pair for char_test opens only on
	# its start.  The count goes into the verdict as char_test printed it.
	char) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		grep -aq 'console: COM1 handed over to its driver' "$f" &&
		grep -aq "char_server: forwarding the kernel's klog to the tty it now owns" "$f" &&
		out=$(grep -aoE 'char_test: [0-9]+ of [0-9]+ arms passed' "$f" | tail -n 1) &&
		[ -n "$out" ] && NOTE=$out ;;
	stop13) grep -aq 'name_server: service_checkin: (ipc/send) invalid destination port' "$f" ;;
	ioapic) grep -aq 'ioapic_race: PASS' "$f" ;;
	# #663: numbered with gaps -- APIC ids 0-2 and 4-6, as pavillion's are
	# 0-5 and 8-13 -- every processor takes the HPET's tick in window 0.
	hpetgaps) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		grep -aqE 'cpu 5 took [0-9]+ ticks in window 0' "$f" &&
		! grep -aqE 'took [0-9] ticks in window 0,' "$f" ;;
	msirace) grep -aq 'msi_race: PASS' "$f" ;;
	# #598: an HPET that can send an FSB message (--hpet fsb) sends it, and
	# while interrupts are remapped takes its pin instead, for the reason
	# it gives: QEMU's tables name no HPET, and its message could not go
	# through an entry of its own.  On q35 under KVM nothing is remapped.
	hpetfsb) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		if grep -aq 'interrupt remapping IS ON' "$f"; then
			grep -aq 'not the FSB message: no table names the HPET for interrupt remapping' "$f"
		else
			grep -aqE 'FSB message to cpu [0-9]+ \(APIC id [0-9]+\) on vector 0x[0-9a-f]+; each' "$f"
		fi ;;
	# #659: the boot passed, entries for 32-bit apic ids were built, and
	# irq 0 and the 82574L's MSI-X slot, aimed at a processor other than
	# the boot processor's id 0 -- 0 is 0 in every format -- arrived there
	# and nowhere else (irq_reach_selftest()).  A NOT ASKED would pass the
	# harness and test nothing, so the two lines are asked for.
	reach) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		grep -aq 'entries for 32-bit apic ids' "$f" &&
		[ "$(grep -acE 'aimed at apic id [1-9][0-9]* arrived there [1-9][0-9]* times and elsewhere 0 — it reaches the processor its entry names' "$f")" -ge 2 ] ;;
	# #755: entry 49 asks reach's question of the x2APIC mode GRUB left on,
	# and the kept line as well, which is what makes it 49 and not 48: the
	# mode the entries were planned for is the one the kernel found on.
	# Under "harness" a board that remapped nothing passed: the plain Intel
	# engine under KVM, which has no split irqchip.
	keptreach) grep -aq 'local apic in x2APIC mode, kept as the firmware left it' "$f" &&
		judge "$e" reach "$h" "$f" "$a" ;;
	# #412: -x and -n are honoured, not merely survived.  Where the
	# processor offers PCID -- KVM on omen, OMEGA, GitHub's runners -- entry
	# 50 must run PCID without INVPCID with [1] and [2] on its log, and entry
	# 51 must run without PCID and leave pcid_test NOT ASKED (pcid_judge()).
	# A renamed flag, or a pcid_test that never ran, failed nothing under
	# "harness" (#412's review).
	pcidx) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		pcid_judge x "$a" "$f" ;;
	pcidn) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		pcid_judge n "$a" "$f" ;;
	# Entry 0, the default: under "harness" a pcid_test that stopped
	# running, or a default that lost INVPCID, failed nothing (the cloud
	# audit of 14/10).
	pcidd) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		pcid_judge d "$a" "$f" ;;
	# #666: the boot passed, and with -f the screen drew what came before it.
	early) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		grep -aqE 'fbcons: -f: the [1-9][0-9]* lines printed before the screen existed are drawn above: [1-9][0-9]* bytes, [1-9][0-9]* glyphs' "$f" ;;
	# Its verdict line, not any "PASS": a WRONG quotes the log's line, and
	# a line run together with "xfile_test: PASS" passed (#661's review).
	panic2) "$HERE/scripts/double-panic-check.sh" "$f" | grep -q '^double-panic-check: PASS' ;;
	# #702: an orderly halt has every processor that passes the final copy's
	# plain word say a copy, and copies mixed a byte of each in turn passed
	# "harness" in 4 boots of 6 -- garbled-lines.awk rarely finds a name
	# whole in them -- so the copy itself is asked about.
	halted) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		"$HERE/scripts/final-copy-check.sh" "$f" | grep -q '^final-copy-check: PASS' ;;
	earlypanic) grep -aq 'panic(cpu 0): early_panic: asked for with -p' "$f" &&
		grep -aq '<x86_64_boot+0x' "$f" &&
		! grep -aq 'trap general protection' "$f" ;;
	# #373: -b restarted the machine for the reason the entry gives it --
	# its deadline, the quiet census, a panic -- and the zone Linux reads
	# back after the restart read back first as ramoops would read it.
	restart) grep -aq 'reset: -b: 20 seconds since the scheduler started' "$f" &&
		zone_kept "$f" ;;
	restartcensus) grep -aq 'quiet_census: -b: the boot is over; restarting the machine' "$f" &&
		zone_kept "$f" ;;
	restartpanic) grep -aq 'panic(cpu 0): early_panic: asked for with -p' "$f" &&
		grep -aq 'panic: -b: restarting the machine in 5 seconds' "$f" &&
		zone_kept "$f" ;;
	# #595: and with reset_hold=, in this order: the census said the
	# machine restarts seventy seconds later, a summary of the TSC watchdog
	# came while it was held, and the hold ended as asked by the TSC -- a
	# hold that ended early or late says WRONG instead.
	restarthold) LC_ALL=C awk '
		/quiet_census: -b: the boot is over; the machine restarts in 70 seconds, as reset_hold= asks/ { c = 1 }
		c && /UrMach x86-64: the TSC watchdog, [0-9]+ windows: / { w = 1 }
		w && /reset: -b: [0-9]+\.[0-9]+ seconds since the quiet census by the TSC, as reset_hold= asks; restarting the machine/ { r = 1 }
		END { exit !r }' "$f" &&
		zone_kept "$f" ;;
	# #688: -t's panic, on a processor inside a system call while another
	# is held in a task's code, and no trap after it.  halt_cpu() asks the
	# command line for -b on every processor a panic stops, and it was
	# read at the loader's physical address, which a task's CR3 maps to
	# the task's pages or to nothing: each processor inside a task
	# reported a page fault on top of #687's two panics.  Not anchored to
	# the line's start: those reports came out interleaved.  A boot in
	# which no second processor came says so, and the stop's way was not
	# asked.
	#
	# #705: and a boot in which -t did not ask at all, which was five of
	# sixteen on OMEGA under TCG before taskpanic_load made both events
	# certain.  With no -t panic and no trap it is NOT ASKED, not WRONG:
	# the panic missing is no defect by itself, and the note says whether
	# the load ran.  A trap, or a boot that did not reach its end, is still
	# WRONG, and for that.  ⚠️ No parenthesis in a note: ci/job.sh reads a
	# verdict up to the first closing one.
	#
	# #688: the note also says whether -t held a processor, from the line
	# the kernel prints at the first hold (taskpanic_held above), and so does
	# a WRONG with no -t panic.  The harness fails a boot that reached its
	# end with a test unexplained as well as one that did not reach it: a
	# boot whose processor -t held all through it reached its end with
	# io_claim_race [3] WRONG (#602), and this note said it did not.
	taskpanic) if grep -aq 'panic(cpu [0-9]*): task_panic: asked for with -t' "$f"; then
			! grep -aqE 'UrMach x86-64: trap |no handler' "$f" &&
			{ grep -aq "held in a task's code until the stop" "$f" ||
			  NOTE="NOT ASKED: no second processor came, so the stop met no task's space"; }
		elif grep -aqE 'UrMach x86-64: trap |no handler' "$f"; then
			NOTE="no -t panic, and a trap; $(taskpanic_held "$f")"
			false
		elif ! grep -aq 'passed: reached the end, nothing unexplained' "$h"; then
			NOTE="no -t panic, and the harness did not pass the boot; $(taskpanic_held "$f")"
			false
		elif grep -aq 'taskpanic_load: [0-9]* s of load and -t did not ask' "$f"; then
			NOTE="NOT ASKED: taskpanic_load loaded the machine and -t did not ask; $(taskpanic_held "$f")"
		else
			NOTE="NOT ASKED: the boot ended before -t asked, and nothing loaded it; $(taskpanic_held "$f")"
		fi ;;
	# #750: the same panic with the debugger armed, whose log ends at the
	# prompt; what came before it is ddb-report-check.sh's to judge, and its
	# line is the note when it is not a pass.
	ddbreport) if grep -aq 'panic(cpu [0-9]*): task_panic: asked for with -t' "$f"; then
			out=$("$HERE/scripts/ddb-report-check.sh" "$f" clock_event_tick)
			case $out in
			"ddb-report-check: PASS"*) ;;
			*)	NOTE="${out//[()]/}"
				false ;;
			esac
		elif grep -aqE 'UrMach x86-64: trap |no handler' "$f"; then
			NOTE="no -t panic, and a trap"
			false
		elif ! grep -aq 'passed: reached the end, nothing unexplained' "$h"; then
			NOTE="no -t panic, and the harness did not pass the boot"
			false
		else
			NOTE="NOT ASKED: the boot ended before -t asked, so the debugger never opened"
		fi ;;
	# #750: -j, a refused fault on a task's present page.  Its own verdict,
	# not the harness's end: the probe prints PASS, WRONG or NOT ASKED, and
	# a boot stopped after its start line is one probe looping on the fault,
	# which the harness's must_report pair already fails.
	refusedfault) out=$(grep -a 'refused_fault_test: \(PASS\|WRONG\|NOT ASKED\)' "$f" | head -n 1)
		case $out in
		*"refused_fault_test: PASS"*) ;;
		"")	NOTE="no refused_fault_test verdict"; false ;;
		*)	NOTE="${out#*refused_fault_test: }"; false ;;
		esac ;;
	esac
}

# #373: the zone's check said it reads back as ramoops would read it, nothing
# was dropped, and the reset was written after it -- the FADT's register, or
# 0xcf9 on a board whose FADT has none.
zone_kept() {
	grep -aqE 'reset: the zone at 0x[0-9a-f]+ reads back as ramoops would read it: signature, start [0-9]+ and size [1-9][0-9]* as written, the [1-9][0-9]* bytes hashing as written and beginning with the [1-9][0-9]* kept before it existed, 0 dropped' "$1" &&
		grep -aqE '^reset: (writing 0x[0-9a-f]+ to the FADT.s reset register|port 0xcf9, )' "$1" &&
		! grep -aq 'does NOT read back as ramoops would read it' "$1"
}

# The stub listens on one port, and a qemu already on it would make every run here refuse to start.
if [ -n "$STUB" ] && ss -ltn 2>/dev/null | grep -q ':1234 '; then
	echo "merge-test: port 1234 is in use, so no run could start with the gdb stub -- free it, or -G to run without" >&2
	exit 2
fi
# #765: the char build -- the build the run was given, and char_server in its
# bundle.  OSFMK_BUILD_CHAR_SERVER stays off in the default bundle, whose boots
# are the ones every earlier measurement was taken on (bootstrap's
# CMakeLists.txt says so), and so char_server, its klog forwarder, COM1's
# handover (#497) and char_test ran only in builds somebody configured by hand:
# none of 3874 merge-test logs kept on pavillion since 01/10 had the forwarder's
# line.  An entry marked @char boots this build instead.
#
# 🔑 Configured here, on every run that has such an entry, from the tree the
# given build was configured from and with every OSFMK_* and UROS_* option of
# its cache -- ablate.sh's configure(), for its reason: a build beside another
# has to be that other build and one difference, or a WRONG in it says nothing
# of which.  A cache left from an earlier run takes the options anew.  Built
# here too, before any boot, so that a build that fails or warns is said once,
# with its log, and not as a boot that did not run.  A count of warnings means
# something only of a build from nothing, which the first run here is.
MAIN_BUILD=${UROS_BUILD_DIR:-$HERE/uros/build-x86_64}
CHAR_BUILD=$MAIN_BUILD-char
CHAR_REFUSED=""
char_build() {
	local cache=$MAIN_BUILD/CMakeCache.txt src gen bt l n rest t v w fresh=0 opts=()
	if [ ! -r "$cache" ]; then
		CHAR_REFUSED="$MAIN_BUILD has no CMakeCache.txt to take the char build's options from"
		return
	fi
	src=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")
	gen=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache")
	bt=$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$cache")
	while IFS= read -r l; do
		n=${l%%:*}
		rest=${l#*:}
		t=${rest%%=*}
		v=${rest#*=}
		[ "$n" = OSFMK_BUILD_CHAR_SERVER ] && continue
		opts+=("-D$n:$t=$v")
	done < <(grep -E '^(OSFMK|UROS)_[A-Z0-9_]*:(BOOL|STRING|UNINITIALIZED)=' "$cache" | grep -v '_LAST:')
	[ -f "$CHAR_BUILD/build.ninja" ] || fresh=1
	if ! cmake -S "$src" -B "$CHAR_BUILD" -G "$gen" -DCMAKE_BUILD_TYPE="$bt" "${opts[@]}" \
		-DOSFMK_BUILD_CHAR_SERVER=ON > "$OUT/char-build-configure.log" 2>&1; then
		CHAR_REFUSED="the char build did not configure: $OUT/char-build-configure.log"
		return
	fi
	if ! LC_ALL=C ninja -C "$CHAR_BUILD" mach_kernel boot_probe name_server_bin \
		bootstrap_server bootstrap_bundle > "$OUT/char-build.log" 2>&1; then
		CHAR_REFUSED="the char build failed: $OUT/char-build.log"
		return
	fi
	w=$(grep -c 'warning:' "$OUT/char-build.log")
	if [ "$w" != 0 ]; then
		CHAR_REFUSED="the char build has $w warning(s), and the build it is made from none: $OUT/char-build.log"
		return
	fi
	if [ $fresh = 1 ]; then
		echo "merge-test: char build $CHAR_BUILD, from $src with ${#opts[@]} options of $MAIN_BUILD and char_server on: built from nothing, 0 warnings"
	else
		echo "merge-test: char build $CHAR_BUILD, from $src with ${#opts[@]} options of $MAIN_BUILD and char_server on: rebuilt, so its 0 warnings cover only what it recompiled"
	fi
}
if grep -E '^[0-9]+ ' <<< "$ENTRIES" | while read -r e v opt cpus; do
	[ -z "$ONLY" ] || [[ "$ONLY" == *" $e "* ]] || continue
	[[ " ${opt//_/ } " == *" @char "* ]] && echo yes
done | grep -q yes; then
	char_build
	[ -z "$CHAR_REFUSED" ] || echo "merge-test: $CHAR_REFUSED -- the entries marked @char are WRONG, not run" >&2
fi
echo "merge-test: tree $(git -C "$HERE" describe --always --dirty), build ${UROS_BUILD_DIR:-default}, accelerators: $ACCS, -smp $SMP${ONLY:+, only entries$ONLY}${STUB:+, gdb stub on every run}"
echo "$SKIPPED" | sed '/^$/d; s/^/merge-test: not run -- /'
while read -r e v opt cpus; do
	[ -n "$e" ] || continue
	[ -z "$ONLY" ] || [[ "$ONLY" == *" $e "* ]] || continue
	[ "$opt" = "-" ] && opt=""
	opt=${opt//_/ }
	n=${cpus:-$SMP}
	# #765: @char is the merge test's, not run-x86_64.sh's: it picks the
	# build.  The options are shown and named with it -- ci/job.sh finds a
	# boot's log by the name it rebuilds from the verdict line's options.
	shown=$opt
	build=""
	if [[ " $opt " == *" @char "* ]]; then
		build=$CHAR_BUILD
		opt=$(echo " $opt " | sed 's/ @char / /; s/^ *//; s/ *$//')
	fi
	for a in $ACCS; do
		budget=1200
		k=""
		[ $a = kvm ] && { budget=600; k="--kvm"; }
		# No comma in a log's name: an entry's -smp value may be a whole
		# topology (entry 14's 1,sockets=2,cores=64,maxcpus=128), and
		# ci/job.sh writes the name into a CSV whose columns commas separate.
		tag="e$e-$a${n//,/_}${shown:+-${shown##* }}"
		h="$OUT/$tag.log"
		f="$OUT/$tag-full.log"
		# #694: a boot's logs go before it.  run-x86_64.sh starts the full
		# log only once it holds the lock, so a run that did not start left
		# the last run's here, and the judges that read only the full log --
		# stop13, ioapic, msirace, panic2, earlypanic, restart* -- passed on
		# a boot that never happened.
		rm -f "$h" "$f" "$f.walk.txt"
		if [ -n "$build" ] && [ -n "$CHAR_REFUSED" ]; then
			echo "merge-test: entry $e $a -smp $n ($shown) -- WRONG (did not run, the char build: ${CHAR_REFUSED//[()]/}): $h"
			echo "$CHAR_REFUSED" > "$h"
			WRONG=$((WRONG + 1))
			continue
		fi
		rc=0
		( cd "$HERE" || exit 2
		  [ -z "$build" ] || export UROS_BUILD_DIR=$build
		  UROS_X86_64_LOG="$f" exec ./scripts/run-x86_64.sh $k $opt --entry "$e" $budget -smp "$n" $STUB ) > "$h" 2>&1 || rc=$?
		# A run not made is WRONG, and says which: not started (2), qemu
		# never ran or said nothing (3), stopped by a signal (above 128) --
		# the statuses ablate.sh reads the same way -- or no full log
		# written, whatever the status.  ci/job.sh knows such a boot by the
		# words "did not run, " and names its harness log, the only one.
		notrun=""
		case $rc in
		2)	notrun="run-x86_64.sh did not start: $(grep -av '^ *$' "$h" | tail -n 1 | sed 's/^ *//; s/^run-x86_64.sh: //')" ;;
		3)	notrun="qemu never ran, or said nothing" ;;
		129|1[3-9][0-9]|2[0-5][0-9])
			notrun="run-x86_64.sh was stopped by SIG$(kill -l $((rc - 128)) 2>/dev/null || echo $((rc - 128)))" ;;
		*)	[ -e "$f" ] || notrun="no full log was written" ;;
		esac
		if [ -n "$notrun" ]; then
			echo "merge-test: entry $e $a -smp $n ${shown:+($shown) }-- WRONG (did not run, status $rc: ${notrun//[()]/}): $h"
			WRONG=$((WRONG + 1))
			continue
		fi
		# #661: and no line the screen cannot draw, whoever judges the
		# entry.  Asked of the full log itself: the harness names one as
		# NO GLYPH only where its verdict gets that far, which neither an
		# entry that panics before setup_main (40, 44) nor a run cut short
		# reaches, and only the judges that ask for its verdict read it --
		# so a panic or a restart passed with an undrawable line in
		# exactly the messages a machine with no serial port needs most
		# (#661's review, two rounds).  A check that cannot run is said as
		# such.  For an entry its judge passed, what it found goes into the
		# log the verdict points at -- which for 40 and 44 the harness's own
		# verdict never reached -- and before the verdict, on which
		# ci/job.sh copies that log.
		ng_rc=0
		ng=$("$HERE/scripts/fbcons-no-glyph.py" "$f") || ng_rc=$?
		if ! judge "$e" "$v" "$h" "$f" "$a"; then
			echo "merge-test: entry $e $a -smp $n ${shown:+($shown) }-- WRONG ($v${NOTE:+: $NOTE}): $h"
			WRONG=$((WRONG + 1))
		elif [ $ng_rc -ne 0 ]; then
			# One line, the last, which says why: ci/job.sh reads the
			# verdict a line at a time.
			why=$(printf '%s\n' "$ng" | tail -n 1)
			[ -n "$why" ] || why="nothing said on its output"
			printf 'NO GLYPH (#661): the check could not run (exit %s): %s\n' "$ng_rc" "$why" >> "$h"
			echo "merge-test: entry $e $a -smp $n ${shown:+($shown) }-- WRONG (the no-glyph check could not run: ${why//[()]/}): $h"
			WRONG=$((WRONG + 1))
		elif [ -n "$ng" ]; then
			printf '%s\n' "$ng" | sed 's/^/NO GLYPH (#661): line /' >> "$h"
			echo "merge-test: entry $e $a -smp $n ${shown:+($shown) }-- WRONG (no glyph): $h"
			WRONG=$((WRONG + 1))
		else
			# #742: said in the row as well, which ci/job.sh writes from
			# this line.  No parenthesis in the note: its VERDICT_RE ends
			# the kind at the first one.
			if grep -aq 'the debug registers came back' "$f"; then
				NOTE="${NOTE:+$NOTE; }#742: the debug registers came back"
				CAMEBACK=$((CAMEBACK + 1))
				CAMEBACK_WHERE="$CAMEBACK_WHERE, entry $e $a -smp $n"
			fi
			echo "merge-test: entry $e $a -smp $n ${shown:+($shown) }-- as it should be ($v${NOTE:+: $NOTE})"
			case $NOTE in
			"NOT ASKED"*)
				UNASKED=$((UNASKED + 1))
				UNASKED_WHERE="$UNASKED_WHERE, entry $e $a -smp $n" ;;
			esac
		fi
	done
done <<< "$ENTRIES"
# ⚠️ Before the total and in a shape of its own: ci/job.sh reads the run's end
# as exactly "merge-test: N wrong".
echo "merge-test: $CAMEBACK where the debug registers came back (#742)${CAMEBACK_WHERE:+:${CAMEBACK_WHERE#,}}"
echo "merge-test: $UNASKED not asked${UNASKED_WHERE:+:${UNASKED_WHERE#,}}"
# #769: said again here, beside the totals a reader looks at, and outside the
# total: ci/job.sh counts that as the entries' WRONG lines, and asks the
# patches itself.
echo "$PATCHES_SAID"
echo "merge-test: $WRONG wrong"
exit $((WRONG + ROTTEN))
