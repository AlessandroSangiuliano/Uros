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
#       (where two lines are then one boot -- -s 1 makes fourteen entries'
#       line without a count their "- 1" line -- it is booted once, and said)
#   -o  directory for the logs, default ./merge-test-logs, or beside the log
#       UROS_X86_64_LOG names, as ablate.sh's Script: runs name each arm's (#591)
#   -e  only these entries (to see one fail, or to rerun one); a merge runs them all
#   -G  no gdb stub; by default every run has one (-s, port 1234), so a run cut
#       short is walked by run-x86_64.sh before it is killed
#   -L  only ask whether each grub.cfg entry is run or said not run, and each
#       line's boot has a name of its own at the count the list is written
#       for, and say so: 0 if so, 1 if not.  ci/job.sh asks it before it
#       builds (#755, #776)
#   -P  only ask whether every patch of ablations/ still applies to this tree,
#       and say so: 0 if each does, 1 if not (#769).  A whole run asks it too
# UROS_BUILD_DIR is passed through to run-x86_64.sh, so a build other than the
# default is tested the same way; its char build, which the entries marked @char
# boot, is that build's path with -char after it (#765).  A run that refuses to
# start -- a bad option, the stub's port taken, the lists and grub.cfg not
# agreeing, the patches not asked, two lines that are not one boot with one
# name -- exits 2; otherwise it exits 1 if an entry was judged wrong or a patch
# of ablations/ no longer applies, and 0 if every judged entry is as it should
# be and every patch applies.  How many were wrong is the last line's, "N
# wrong": a count as the status said 2 and 3 as well, which ablate.sh reads as
# a run that did not start (#782).  Sent SIGTERM, a run stops the boot it is
# running, says so of it in a verdict line whose result is "stopped", starts
# no other, prints its totals and dies of the signal (#951).

set -u
ACC=both
# The processor count ENTRIES is written for, and -s's default (#776).
SMP_LISTED=4
SMP=$SMP_LISTED
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
# the char build instead of the build the run was given (#765; side_build
# below), and @ush the ush build, typed at by scripts/ush-x86_64.py instead of
# booted by run-x86_64.sh (#952).
#   harness   the harness's own verdict: "passed: reached the end, nothing unexplained"
#   char      the harness's verdict, of the char build: COM1 handed over to
#             char_server, its klog forwarder started, and char_test's count of
#             arms -- what "harness" would pass a build without char_server
#             on, since nothing asks for them there (#765)
#   ush       ush-x86_64.py's count, of the ush build: every command it types
#             at the prompt printed its lines and gave the prompt back -- a
#             program's stdout on the controlling terminal, fork and execve
#             from a shell, a forked child's write on its terminal (#952)
#   stop13    the name server stops at service_checkin, which grub.cfg says is
#             the designed end: a boot task that IS the name server has nothing
#             above it to answer the check-in.  Since #744 it checks in on its
#             own bootstrap port, which for a boot task is the kernel's master
#             port, and the answer is MIG's bad request ID, no longer an
#             invalid destination
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
#   contret   -l: a continuation that returns stops the machine NAMING it --
#             thread_begin_trampoline's panic with cont_return_resume, and not
#             context_thread_start's, which knows only that "a thread
#             function" returned; scripts/cont-return-check.sh judges it (#771)
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
#   wakee     -w: wakee_test's PASS, and the harness's verdict -- the hand-off
#             reached the thread it named, and that thread ran elsewhere when
#             a thread above it took its waker's processor; NOT ASKED on one
#             processor (#777, #954)
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
14 ush @ush
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
21 halted
22 harness
22 hpetgaps - 6,sockets=2,cores=3
22 hpetfsb --hpet_fsb
23 ioapic
39 msirace --iommu_intel
24 panic2
40 earlypanic
58 contret
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
59 harness
59 harness - 1
60 harness
60 harness - 1
61 harness
61 harness - 1
63 harness
63 harness - 1
64 harness
64 harness - 1
65 harness
65 harness - 1
67 wakee
67 wakee - 1
"

# Not run, and why -- said on every run so the list cannot shrink in silence.
SKIPPED="
2: the double-fault self-test ends in 'no handler -- halted' on purpose; it needs a verdict of its own
10: -rB opens the debugger's prompt and waits for someone to type
11: -rL opens the debugger's prompt on a thread with a continuation and waits
12: -rS stays up for the console door and never ends
18: the same boot as entry 14 since the hand-off went on by default (#777); it stays so that the numbers after it do
19: -B panics on purpose; it needs a verdict of its own
54: -e asks only of a build with UROS_SYSCALL_PROFILE, which a merge build is not; ablate.sh runs it with the widen patch, which compiles the profile in (#622)
57: -u asks only of a build whose mutex_unlock() calls the release-window probe, which a merge build's does not; ablate.sh runs it with the widen patch, which adds the call (#753)
62: -d asks only of a build whose master tick calls the dispatch probe, which a merge build's does not; ablate.sh runs it with the widen patch, which adds the call (#787)
66: -z asks only of a build whose thread_doassign() calls the window probe, which a merge build's does not; ablate.sh runs it with the widen patch, which adds the call (#787)
"

# A boot's name: what its two logs are called, and what ci/job.sh's row()
# rebuilds out of its verdict line to find them.  No comma in it: an entry's
# -smp value may be a whole topology (entry 14's 1,sockets=2,cores=64,
# maxcpus=128), and ci/job.sh writes the name into a CSV whose columns commas
# separate.
boot_name() {	# entry accelerator -smp options-as-shown
	echo "e$1-$2${3//,/_}${4:+-${4##* }}"
}

# #957: a boot's verdict line, every one of them, in the one shape ci/job.sh's
# VERDICT_RE reads: "merge-test: entry E ACC -smp N [(OPTIONS) ]-- RESULT
# (KIND)[: LOG]".  It reads the kind up to its first ")", and a line it cannot
# read is a boot with no row, its logs not copied to failed/, and a round
# reported short -- the job's status 3, not the 1 of a WRONG.  A note quoting
# a test's own line did that: refused_fault_test's WRONG ends in "(#750)".  So
# the kind -- the judge's word and the note after it, whoever wrote the note
# -- is printed here with no parenthesis and on one line.  The options are
# the list's own, and lists_cover() refuses a parenthesis in them.
verdict_line() {	# entry accelerator -smp options-as-shown result kind [log]
	local kind=${6//[()]/}
	kind=${kind//[$'\n\r']/ }
	echo "merge-test: entry $1 $2 -smp $3 ${4:+($4) }-- $5 ($kind)${7:+: $7}"
}

# #776: the boots a run makes at -smp $1 of the entries in $2 (" N ... ", or
# empty for all), one in PLAN each: entry, verdict, options as the verdict
# line shows them, and -smp, apart by a unit separator.  Every boot removes
# the logs under its name before it starts (#694), so two boots under one
# name left one boot's logs for two verdict lines, and a WRONG line could name
# the log of a boot that passed.  A line whose name an earlier line has is not
# planned: it goes in SAME when it is that line's boot -- the same verdict and
# options at the same count, as fourteen entries' line without a count and
# their "- 1" line are at -s 1 -- and in CLASH when it is not.
plan_boots() {	# smp entries
	local e v opt cpus n line key
	local -A first=() named=()
	PLAN=() SAME=() CLASH=()
	while read -r e v opt cpus; do
		[ -n "$e" ] || continue
		[ -z "$2" ] || [[ "$2" == *" $e "* ]] || continue
		line="$e $v${opt:+ $opt}${cpus:+ $cpus}"
		[ "$opt" = "-" ] && opt=""
		opt=${opt//_/ }
		n=${cpus:-$1}
		key=$(boot_name "$e" "" "$n" "$opt")
		if [ -z "${first[$key]:-}" ]; then
			first[$key]="$v|$opt|$n"
			named[$key]=$line
			PLAN+=("$e"$'\x1f'"$v"$'\x1f'"$opt"$'\x1f'"$n")
		elif [ "${first[$key]}" = "$v|$opt|$n" ]; then
			SAME+=("'$line' is the boot of '${named[$key]}' at -smp $n")
		else
			CLASH+=("'$line' and '${named[$key]}' are not one boot, and at -smp $n both would write their logs as $(boot_name "$e" "<accelerator>" "$n" "$opt")")
		fi
	done <<< "$ENTRIES"
}

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
	# #776: and no two lines with one name at the count the list is written
	# for, the one ci/job.sh boots it at: it counts a round as lines times
	# accelerators, and a line booted once would leave the round short.
	plan_boots "$SMP_LISTED" ""
	for e in "${SAME[@]}" "${CLASH[@]}"; do
		echo "merge-test: lists: $e -- at the count ENTRIES is written for, $SMP_LISTED, each line's boot needs a name of its own (#776)" >&2
		bad=1
	done
	# #957: and no parenthesis in a line's options, which its verdict lines
	# show between parentheses: ci/job.sh's VERDICT_RE ends them at the first
	# ")", and a verdict line it cannot read leaves its boot with no row.
	while read -r e _ opt _; do
		[[ $e =~ ^[0-9]+$ && $opt == *['()']* ]] || continue
		echo "merge-test: lists: entry $e's options $opt have a parenthesis, and its verdict lines show them between parentheses, which ci/job.sh reads only up to the first ')' (#957)" >&2
		bad=1
	done <<< "$ENTRIES"
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
# #951: a run sent SIGTERM stops the boot it is running, says so of that boot
# in a verdict line whose result is "stopped", starts no other, prints its
# totals as a whole run does, and dies of the signal.  ci/job.sh sends it at
# its time limit: a campaign job that ran out of GitHub's minutes was cut off
# in a boot, which left no row and no log, and its summary was never written.
# The trap passes the signal on to the boot, which run-x86_64.sh and
# ush-x86_64.py stop as they stop for a Ctrl-C, their qemu stopped and the
# lock given back.  So that it runs while a boot runs, the boot is a job
# waited for: a shell runs a trap only once the command it waits for in the
# foreground has ended (#801).  TERM and not INT: a SIGINT that comes while
# the shell waits for a $(...) is lost (#801), and a Ctrl-C reaches every
# process of the terminal's group anyway.
STOP=""
BOOT=""
trap 'STOP=TERM; [ -z "$BOOT" ] || kill -TERM "$BOOT" 2>/dev/null' TERM
lists_cover || exit 2
plan_boots "$SMP" "$ONLY"
if [ ${#CLASH[@]} -gt 0 ]; then
	printf 'merge-test: %s -- run them at another -s, or give one a last option of its own (#776)\n' "${CLASH[@]}" >&2
	exit 2
fi
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

# #779: a question one processor cannot pose.  There the test says NOT ASKED
# -- alone, no other processor to pose it with -- and the boot goes on, so the
# verdict is the harness's and NOT ASKED, counted with the other questions not
# asked (#705).  At more processors the same words mean processors that never
# came, and the verdict stays WRONG.
alone_ok() {	# -smp log-of-the-harness full-log the-test's-words-on-one-processor
	[ "${1%%,*}" = 1 ] &&
		grep -aq 'passed: reached the end, nothing unexplained' "$2" &&
		grep -aqF "$4" "$3" &&
		NOTE="NOT ASKED: one processor"
}

judge() {	# entry verdict log-of-the-harness full-log accelerator -smp
	local e=$1 v=$2 h=$3 f=$4 a=$5 n=${6:-} out
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
	# #952: the ush build, typed at by ush-x86_64.py.  Its count is the
	# verdict, every command of its list answered and at least one: a
	# session that stopped at a command says fewer, and one that never got
	# to the prompt says none.  #958: and when it is not, the note says why:
	# the count where the session got that far, or else the script's last
	# line, which says what it read when it stopped before typing -- at its
	# prompt, a root disk that ext2 never said was ready.
	ush) out=$(grep -aoE '^ush-x86_64: [0-9]+ of [0-9]+ commands answered' "$h" | tail -n 1)
		out=${out#ush-x86_64: }
		if [ -n "$out" ] && [ "${out%% *}" -gt 0 ] &&
		   [ "${out%% *}" = "$(echo "$out" | cut -d' ' -f3)" ]; then
			NOTE=$out
		else
			[ -n "$out" ] ||
				out=$(grep -av -e '^ *$' -e '^ush-x86_64: log: ' "$h" | tail -n 1 | sed 's/^ush-x86_64: //')
			NOTE=$out
			false
		fi ;;
	stop13) grep -aq 'name_server: service_checkin: (ipc/mig) bad request message ID' "$f" ;;
	ioapic) grep -aq 'ioapic_race: PASS' "$f" ||
		alone_ok "$n" "$h" "$f" 'ioapic_race: NOT ASKED — alone' ;;
	# #663: numbered with gaps -- APIC ids 0-2 and 4-6, as pavillion's are
	# 0-5 and 8-13 -- every processor takes the HPET's tick in window 0.
	hpetgaps) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		grep -aqE 'cpu 5 took [0-9]+ ticks in window 0' "$f" &&
		! grep -aqE 'took [0-9] ticks in window 0,' "$f" ;;
	msirace) grep -aq 'msi_race: PASS' "$f" ||
		alone_ok "$n" "$h" "$f" 'msi_race: NOT ASKED — alone' ;;
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
	# harness and test nothing, so the two lines are asked for -- except on
	# one processor, where the question does not arise and the verdict says
	# NOT ASKED (#779).
	reach) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		grep -aq 'entries for 32-bit apic ids' "$f" &&
		{ [ "$(grep -acE 'aimed at apic id [1-9][0-9]* arrived there [1-9][0-9]* times and elsewhere 0 — it reaches the processor its entry names' "$f")" -ge 2 ] ||
		  alone_ok "$n" "$h" "$f" 'where an interrupt arrives: NOT ASKED, one processor'; } ;;
	# #755: entry 49 asks reach's question of the x2APIC mode GRUB left on,
	# and the kept line as well, which is what makes it 49 and not 48: the
	# mode the entries were planned for is the one the kernel found on.
	# Under "harness" a board that remapped nothing passed: the plain Intel
	# engine under KVM, which has no split irqchip.
	keptreach) grep -aq 'local apic in x2APIC mode, kept as the firmware left it' "$f" &&
		judge "$e" reach "$h" "$f" "$a" "$n" ;;
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
	panic2) "$HERE/scripts/double-panic-check.sh" "$f" | grep -q '^double-panic-check: PASS' ||
		alone_ok "$n" "$h" "$f" 'double_panic: NOT ASKED — alone' ;;
	# #702: an orderly halt has every processor that passes the final copy's
	# plain word say a copy, and copies mixed a byte of each in turn passed
	# "harness" in 4 boots of 6 -- garbled-lines.awk rarely finds a name
	# whole in them -- so the copy itself is asked about.
	halted) grep -aq 'passed: reached the end, nothing unexplained' "$h" &&
		"$HERE/scripts/final-copy-check.sh" "$f" | grep -q '^final-copy-check: PASS' ;;
	earlypanic) grep -aq 'panic(cpu 0): early_panic: asked for with -p' "$f" &&
		grep -aq '<x86_64_boot+0x' "$f" &&
		! grep -aq 'trap general protection' "$f" ;;
	# #771: its verdict line, as panic2's -- the panic names the
	# continuation that returned, and context_thread_start's nameless one,
	# which a fall-through would have reached, is not in the log.
	contret) "$HERE/scripts/cont-return-check.sh" "$f" | grep -q '^cont-return-check: PASS' ;;
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
			*)	NOTE=$out
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
	# #954: -w, the hand-off's wakee.  Under "harness" a NOT ASKED passed
	# and asked nothing, and since #954 wakee_test says NOT ASKED where its
	# waker left with no bet open, the wakee having run first: what a waker
	# above the wakee, with preemption off, cannot let happen, and what one
	# below it did in each boot of 954-waker-outranked, all four of which
	# the harness passed.  So its PASS is asked for, except on one
	# processor, where the question does not arise and the verdict says
	# NOT ASKED (#779).  The note is the verdict's word, not its line.
	wakee)	out=$(grep -a 'wakee_test: \(PASS\|WRONG\|NOT ASKED\)' "$f" | head -n 1)
		case $out in
		*"wakee_test: PASS"*)
			grep -aq 'passed: reached the end, nothing unexplained' "$h" ;;
		"")	NOTE="no wakee_test verdict"; false ;;
		*)	alone_ok "$n" "$h" "$f" 'wakee_test: NOT ASKED — 1 processor(s)' ||
			{ out=${out#*wakee_test: }; NOTE="wakee_test said ${out%% —*}"; false; } ;;
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
#
# #952: the ush build is the char build and one more option, UROS_BUNDLE_USH:
# its bundle boots to ush on the serial line, and an entry marked @ush is not
# booted by run-x86_64.sh but typed at by scripts/ush-x86_64.py -- the one run
# that goes through a controlling terminal, which no other entry here had.
# Both are made by side_build(), so that what one is configured with the other
# is too, but for the options it names.
MAIN_BUILD=${UROS_BUILD_DIR:-$HERE/uros/build-x86_64}
CHAR_BUILD=$MAIN_BUILD-char
CHAR_REFUSED=""
USH_BUILD=$MAIN_BUILD-ush
USH_REFUSED=""
SIDE_REFUSED=""
side_build() {	# directory word what-it-turns-on -Doption...
	local dir=$1 word=$2 what=$3 cache=$MAIN_BUILD/CMakeCache.txt
	local src gen bt l n rest t v w o fresh=0 opts=() own=() named=" "
	shift 3
	own=("$@")
	for o in "${own[@]}"; do
		o=${o#-D}
		named="$named${o%%[:=]*} "
	done
	SIDE_REFUSED=""
	if [ ! -r "$cache" ]; then
		SIDE_REFUSED="$MAIN_BUILD has no CMakeCache.txt to take the $word build's options from"
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
		[[ "$named" == *" $n "* ]] && continue
		opts+=("-D$n:$t=$v")
	done < <(grep -E '^(OSFMK|UROS)_[A-Z0-9_]*:(BOOL|STRING|UNINITIALIZED)=' "$cache" | grep -v '_LAST:')
	[ -f "$dir/build.ninja" ] || fresh=1
	if ! cmake -S "$src" -B "$dir" -G "$gen" -DCMAKE_BUILD_TYPE="$bt" "${opts[@]}" \
		"${own[@]}" > "$OUT/$word-build-configure.log" 2>&1; then
		SIDE_REFUSED="the $word build did not configure: $OUT/$word-build-configure.log"
		return
	fi
	if ! LC_ALL=C ninja -C "$dir" mach_kernel boot_probe name_server_bin \
		bootstrap_server bootstrap_bundle > "$OUT/$word-build.log" 2>&1; then
		SIDE_REFUSED="the $word build failed: $OUT/$word-build.log"
		return
	fi
	w=$(grep -c 'warning:' "$OUT/$word-build.log")
	if [ "$w" != 0 ]; then
		SIDE_REFUSED="the $word build has $w warning(s), and the build it is made from none: $OUT/$word-build.log"
		return
	fi
	if [ $fresh = 1 ]; then
		echo "merge-test: $word build $dir, from $src with ${#opts[@]} options of $MAIN_BUILD and $what on: built from nothing, 0 warnings"
	else
		echo "merge-test: $word build $dir, from $src with ${#opts[@]} options of $MAIN_BUILD and $what on: rebuilt, so its 0 warnings cover only what it recompiled"
	fi
}
# Whether an entry this run makes is marked @$1.
marked() {
	grep -E '^[0-9]+ ' <<< "$ENTRIES" | while read -r e v opt cpus; do
		[ -z "$ONLY" ] || [[ "$ONLY" == *" $e "* ]] || continue
		[[ " ${opt//_/ } " == *" @$1 "* ]] && echo yes
	done | grep -q yes
}
# #951: and a run asked to stop builds no side build it will not boot.
if [ -z "$STOP" ] && marked char; then
	side_build "$CHAR_BUILD" char char_server -DOSFMK_BUILD_CHAR_SERVER=ON
	CHAR_REFUSED=$SIDE_REFUSED
	[ -z "$CHAR_REFUSED" ] || echo "merge-test: $CHAR_REFUSED -- the entries marked @char are WRONG, not run" >&2
fi
if [ -z "$STOP" ] && marked ush; then
	side_build "$USH_BUILD" ush "char_server and the ush bundle" \
		-DOSFMK_BUILD_CHAR_SERVER=ON -DUROS_BUNDLE_USH=ON
	USH_REFUSED=$SIDE_REFUSED
	[ -z "$USH_REFUSED" ] || echo "merge-test: $USH_REFUSED -- the entries marked @ush are WRONG, not run" >&2
fi
echo "merge-test: tree $(git -C "$HERE" describe --always --dirty), build ${UROS_BUILD_DIR:-default}, accelerators: $ACCS, -smp $SMP${ONLY:+, only entries$ONLY}${STUB:+, gdb stub on every run}"
echo "$SKIPPED" | sed '/^$/d; s/^/merge-test: not run -- /'
[ ${#SAME[@]} -eq 0 ] || printf 'merge-test: booted once -- %s (#776)\n' "${SAME[@]}"
# #951: the boots made so far, of all the run plans, for the line a stopped
# run ends with.
MADE=0
PLANNED=$(( ${#PLAN[@]} * $(wc -w <<< "$ACCS") ))
STOPPED_AT=""
for boot in "${PLAN[@]}"; do
	IFS=$'\x1f' read -r e v opt n <<< "$boot"
	# #765: @char is the merge test's, not run-x86_64.sh's: it picks the
	# build.  The options are shown and named with it -- ci/job.sh finds a
	# boot's log by the name it rebuilds from the verdict line's options.
	shown=$opt
	build=""
	side=""
	refused=""
	if [[ " $opt " == *" @char "* ]]; then
		build=$CHAR_BUILD
		side=char
		refused=$CHAR_REFUSED
		opt=$(echo " $opt " | sed 's/ @char / /; s/^ *//; s/ *$//')
	elif [[ " $opt " == *" @ush "* ]]; then
		build=$USH_BUILD
		side=ush
		refused=$USH_REFUSED
		opt=$(echo " $opt " | sed 's/ @ush / /; s/^ *//; s/ *$//')
	fi
	for a in $ACCS; do
		# #951: asked to stop between two boots, it starts no other.
		if [ -n "$STOP" ]; then
			STOPPED_AT="before boot $((MADE + 1)) of $PLANNED"
			break 2
		fi
		MADE=$((MADE + 1))
		budget=1200
		k=""
		# ush-x86_64.py's wait for each line a command prints (#952).
		wait=300
		[ $a = kvm ] && { budget=600; k="--kvm"; wait=120; }
		tag=$(boot_name "$e" "$a" "$n" "$shown")
		h="$OUT/$tag.log"
		f="$OUT/$tag-full.log"
		# #694: a boot's logs go before it.  run-x86_64.sh starts the full
		# log only once it holds the lock, so a run that did not start left
		# the last run's here, and the judges that read only the full log --
		# stop13, ioapic, msirace, panic2, earlypanic, restart* -- passed on
		# a boot that never happened.
		rm -f "$h" "$f" "$f.walk.txt"
		if [ -n "$refused" ]; then
			verdict_line "$e" "$a" "$n" "$shown" WRONG "did not run, the $side build: $refused" "$h"
			echo "$refused" > "$h"
			WRONG=$((WRONG + 1))
			continue
		fi
		rc=0
		runner=run-x86_64.sh
		[ "$side" = ush ] && runner=ush-x86_64.py
		# #952: an @ush entry is typed at, not judged by the harness:
		# ush-x86_64.py boots the ush build, waits for the prompt and the
		# root disk, and types its commands.  Its exit statuses are the
		# harness's -- 0, 1, and 2 for a run it could not make -- and the
		# line it carried is the full log.
		# #951: a job waited for, so that a SIGTERM's trap runs during the
		# boot and passes the signal on; its standard input named, which an
		# asynchronous command would otherwise have from /dev/null.  A
		# SIGTERM that came before $BOOT was set is passed on here.
		started=$SECONDS
		( cd "$HERE" || exit 2
		  [ -z "$build" ] || export UROS_BUILD_DIR=$build
		  if [ "$side" = ush ]; then
			exec ./scripts/ush-x86_64.py --build "$build" $k --entry "$e" --smp "$n" \
				--budget "$budget" --wait "$wait" --log "$f"
		  fi
		  UROS_X86_64_LOG="$f" exec ./scripts/run-x86_64.sh $k $opt --entry "$e" $budget -smp "$n" $STUB ) > "$h" 2>&1 <&0 &
		BOOT=$!
		[ -z "$STOP" ] || kill -TERM "$BOOT" 2>/dev/null
		wait "$BOOT" || rc=$?
		# A SIGTERM during the wait ran the trap and ended the wait at once,
		# with 143 whatever the boot did: the boot's own status is the next
		# wait's, once it has stopped.
		if [ -n "$STOP" ] && [ $rc -gt 128 ]; then
			rc=0
			wait "$BOOT" || rc=$?
		fi
		BOOT=""
		# Stopped by the signal, the boot is said to be, and judged no
		# further; one that ended by itself first is judged as any other,
		# and the run stops before the next.
		if [ -n "$STOP" ] && [ $rc -gt 128 ]; then
			verdict_line "$e" "$a" "$n" "$shown" stopped "SIG$STOP $((SECONDS - started)) s into the boot, which then ended with status $rc" "$h"
			STOPPED_AT="at boot $MADE of $PLANNED"
			break 2
		fi
		# A run not made is WRONG, and says which: not started (2), qemu
		# never ran or said nothing (3), stopped by a signal (above 128) --
		# the statuses ablate.sh reads the same way -- or no full log
		# written, whatever the status.  ci/job.sh knows such a boot by the
		# words "did not run, " and names its harness log, the only one.
		notrun=""
		case $rc in
		2)	notrun="$runner did not start: $(grep -av '^ *$' "$h" | tail -n 1 | sed 's/^ *//; s/^run-x86_64.sh: //; s/^ush-x86_64: //')" ;;
		3)	notrun="qemu never ran, or said nothing" ;;
		129|1[3-9][0-9]|2[0-5][0-9])
			notrun="$runner was stopped by SIG$(kill -l $((rc - 128)) 2>/dev/null || echo $((rc - 128)))" ;;
		*)	[ -e "$f" ] || notrun="no full log was written" ;;
		esac
		if [ -n "$notrun" ]; then
			verdict_line "$e" "$a" "$n" "$shown" WRONG "did not run, status $rc: $notrun" "$h"
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
		if ! judge "$e" "$v" "$h" "$f" "$a" "$n"; then
			verdict_line "$e" "$a" "$n" "$shown" WRONG "$v${NOTE:+: $NOTE}" "$h"
			WRONG=$((WRONG + 1))
		elif [ $ng_rc -ne 0 ]; then
			# One line, the last, which says why: ci/job.sh reads the
			# verdict a line at a time.
			why=$(printf '%s\n' "$ng" | tail -n 1)
			[ -n "$why" ] || why="nothing said on its output"
			printf 'NO GLYPH (#661): the check could not run (exit %s): %s\n' "$ng_rc" "$why" >> "$h"
			verdict_line "$e" "$a" "$n" "$shown" WRONG "the no-glyph check could not run: $why" "$h"
			WRONG=$((WRONG + 1))
		elif [ -n "$ng" ]; then
			printf '%s\n' "$ng" | sed 's/^/NO GLYPH (#661): line /' >> "$h"
			verdict_line "$e" "$a" "$n" "$shown" WRONG "no glyph" "$h"
			WRONG=$((WRONG + 1))
		else
			# #742: said in the row as well, which ci/job.sh writes from
			# this line.
			if grep -aq 'the debug registers came back' "$f"; then
				NOTE="${NOTE:+$NOTE; }#742: the debug registers came back"
				CAMEBACK=$((CAMEBACK + 1))
				CAMEBACK_WHERE="$CAMEBACK_WHERE, entry $e $a -smp $n"
			fi
			verdict_line "$e" "$a" "$n" "$shown" "as it should be" "$v${NOTE:+: $NOTE}"
			case $NOTE in
			"NOT ASKED"*)
				UNASKED=$((UNASKED + 1))
				UNASKED_WHERE="$UNASKED_WHERE, entry $e $a -smp $n" ;;
			esac
		fi
	done
done
# #951: a stopped run says where it stopped, and prints the totals of the boots
# it made as a whole run does.
if [ -n "$STOP" ]; then
	echo "merge-test: stopped by SIG$STOP ${STOPPED_AT:-after its last boot, $MADE of $PLANNED}"
fi
# ⚠️ Before the total and in a shape of its own: ci/job.sh reads the run's end
# as exactly "merge-test: N wrong".
echo "merge-test: $CAMEBACK where the debug registers came back (#742)${CAMEBACK_WHERE:+:${CAMEBACK_WHERE#,}}"
echo "merge-test: $UNASKED not asked${UNASKED_WHERE:+:${UNASKED_WHERE#,}}"
# #769: said again here, beside the totals a reader looks at, and outside the
# total: ci/job.sh counts that as the entries' WRONG lines, and asks the
# patches itself.
echo "$PATCHES_SAID"
echo "merge-test: $WRONG wrong"
# #951: and a stopped run dies of the signal, as run-x86_64.sh does, so that
# whoever sent it sees it obeyed.
if [ -n "$STOP" ]; then
	trap - TERM
	kill -TERM $$
fi
# #782: whether, not how many -- see the header.
[ $((WRONG + ROTTEN)) -eq 0 ] || exit 1
exit 0
