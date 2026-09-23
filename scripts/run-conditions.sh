#!/usr/bin/env sh
#
# run-conditions.sh — what a run was taken under, said once and written down
# (#516).
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# Sourced by run-qemu.sh and run-x86_64.sh.  Not executable on its own.
#
# ── Why this is one file and not two ──────────────────────────────────
#
# 🔑 EACH HARNESS RECORDED THE HALF THE OTHER OMITTED, which is how #516 was
# hiding in plain sight on both targets at once:
#
#   run-qemu.sh (i386)  samples the governor, the clock and the power source
#                       (#460) — and never says which accelerator it used on
#                       its default path, which is KVM.
#   run-x86_64.sh       says the accelerator, the board and the memory (#477)
#                       — and never samples the clock at all.
#
# So a comparison across the two targets could not be made honestly from their
# own output, and neither could a comparison between two runs of the same one.
# The rule this serves wants FOUR things to coincide before two numbers may be
# compared: the machine, the accelerator, the clock condition — sampled, not
# assumed from the governor's name — and how the run was driven.  A fifth is
# what else was running.  One of them missing is enough.
#
# ── Why it is written to the LOG and not only to the screen ───────────
#
# 🔴 Both scripts already printed most of this, to stdout, where it is gone the
# moment the terminal scrolls.  Grepping what this project keeps:
#
#	$ grep -ac accelerator ~/uros-tests/515-x86_64-kvm.log   ->  0
#	$ grep -ac accelerator <the same run's stdout>           ->  1
#
# The grep can match; the logs simply never carried it.  So every saved x86-64
# log was, in this issue's own words, a verdict about an unknown machine — and
# a re-judge of an old log could not recover what made it.
#
# ── Why the clock is sampled TWICE ────────────────────────────────────
#
# 🔥 Because it moves, and the movement is deterministic rather than random: a
# build heats the package, so the first boots after one run at a lower clock
# than the later ones.  #560 lost a whole campaign to exactly that — arm A ran
# at 3000 MHz and arm B at 3993, and the difference read as the change under
# test.  A run that changed clock mid-flight is a run nobody may compare, and
# now it says so instead of being found out afterwards.

# One line: the host's half of the four axes.  The FREQUENCY and not only the
# governor's name — on this laptop `performance' on battery still reported
# 3.99 GHz while `powersave' gave 1.40, so the name alone would have compared
# runs 2.85x apart without noticing (#460).
#
# 🔴 AND THE CEILING, not only where the clock happens to be (#544).  An idle
# machine under `powersave' with scaling_max_freq at 3.0 GHz reports the same
# 1397 MHz as one PINNED at 1.4, and the two are different experiments: the
# first climbs to 3.0 the moment the run loads it, the second cannot.  Without
# the cap, "measured at low clock" and "measured at a clock that was low when I
# looked" print the same line -- which is the same defect this issue is about,
# one layer down: two states, one label.
uros_host_state() {
	_gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor \
	       2>/dev/null || echo "?")
	_mhz=$(awk '/cpu MHz/ {printf "%.0f", $4; exit}' /proc/cpuinfo \
	       2>/dev/null || echo "?")
	_capk=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq \
	        2>/dev/null || echo "")
	_mink=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq \
	        2>/dev/null || echo "")
	_drv=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver \
	       2>/dev/null || echo "?")

	# 🔑 The two facts that say what `powersave' MEANS on this machine, both
	# read from it rather than looked up by driver name -- see
	# uros_clock_policy, and #564 for what the lookup cost.
	_avail=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors \
	         2>/dev/null || echo "")
	# ⚠️ A SAMPLE AND NOT A CONCLUSION.  Under a driver that takes the policy
	# itself the governor's name has stopped being the whole of it: two runs
	# at `powersave' with different preferences are two conditions, and
	# nothing else on the line would tell them apart.  What it does to the
	# clock is measured by the run, not here: on victus balance_power ran
	# at 2096 on battery and 3981 on AC (#579), so the power source is part
	# of the condition too, and the line carries it.
	_epp=$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference \
	       2>/dev/null || echo "")
	# Whether boost may carry the core past the ceiling is decided in
	# uros_clock_policy, from this; see the head of it (#579).
	_boost=$(uros_boost_state \
		"$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null)" \
		"$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null)")

	# The one half of this line that can be wrong while every sample above
	# is right, kept in a function of its own for that reason.
	uros_clock_policy
	_ac="?"
	for _p in /sys/class/power_supply/A*/online; do
		[ -r "$_p" ] && _ac=$(cat "$_p") && break
	done
	case "$_ac" in
	1)	_ac="AC" ;;
	0)	_ac="battery" ;;
	*)	_ac="AC?" ;;
	esac
	echo "governor=$_gov${_epp:+ epp=$_epp} cpu=${_mhz}MHz cap=$_cap" \
	     "effective=$_eff power=$_ac at=$(date +%H:%M:%S)"
}

# uros_boost_state <cpufreq/boost> <intel_pstate/no_turbo> -- 1, 0 or empty.
#
# ⚠️ TWO FILES SAY IT, AND ONE OF THEM BACKWARDS.  cpufreq/boost exists only
# for a driver that implements the core's boost switch; intel_pstate, in both
# of its modes, does not, and keeps its own switch as intel_pstate/no_turbo,
# where 0 means turbo is ON.  Reading only the first left every Intel machine
# without HWP -- whose default is intel_pstate's passive mode, intel_cpufreq,
# a core-driven driver -- with no boost at all on the line (#579, third
# review).  The general file wins where both exist.
uros_boost_state() {
	case "$1" in
	0|1)	echo "$1" ;;
	*)	case "$2" in
		0)	echo 1 ;;
		1)	echo 0 ;;
		*)	echo "" ;;
		esac ;;
	esac
}

# uros_clock_policy — WHICH of the two clocks this machine will actually run
# at, decided from facts already sampled into _drv _gov _avail _mink _capk
# _boost _mhz.  Sets _cap, _top and _eff.
#
# 🔑 Kept apart from the sampling because it is the one half of that line that
# can be wrong while every sample in it is right — #564 printed cpu=3703MHz and
# effective=1108MHz beside each other for a day — and because a decision whose
# inputs are variables can be driven over machines this one is not, which is
# what --self-test at the foot of this file does.
#
# 🔴 THE CEILING IS NOT THE CLOCK, and the governor decides which.
#
# With `acpi-cpufreq' the `powersave' governor is STATIC: it pins the core to
# scaling_min_freq and the ceiling above it means nothing.  Raising
# scaling_max_freq from 1400 to 3000 while leaving that governor in place
# changes the recorded ceiling and not one megahertz of the machine.
#
# That cost a campaign: eleven runs were taken as the "high clock" arm with
#
#     governor=powersave cpu=1397MHz cap=3000MHz
#
# and every fact needed to notice was on that line.  The block reported and did
# not conclude, so the ceiling got read and the governor did not.  A passing run
# took 454-463s in both arms -- identical, which is what settled it, because the
# same work at twice the clock is not the same number of seconds.
#
# 🔴🔴 AND THE SAME WORD NAMES THE OPPOSITE POLICY ON AN ACTIVE DRIVER.  Under
# `intel_pstate' or `amd-pstate-epp' the word `powersave' is not a governor at
# all: the driver takes the policy itself, scales the WHOLE range, and the name
# means an energy bias.  One name on the line, two opposite machines behind it.
#
# ⚠️ WHICH OF THE TWO IS A QUESTION FOR THE MACHINE.  It used to be a list of
# driver names kept here, and #564 is what the list cost: `intel_pstate' was on
# it, `amd-pstate-epp' was not, so every measurement taken on this laptop
# carried effective=1108MHz beside a processor sampled at 3.9 GHz — a false
# clock on the one line that exists so that two runs may be compared at all.
#
# The machine answers in scaling_available_governors.  A driver that takes the
# policy has no governors to list, so the kernel prints a FIXED string there;
# one the core drives lists the governors it has registered instead, which is a
# longer list.  Measured here, byte for byte under
# amd-pstate-epp: `performance powersave' and the newline, nothing else.  So
# the test is that literal rather than a driver name, and a driver nobody here
# has heard of is read correctly the day it turns up.
#
# 🔴 AND NOTHING IN SYSFS SAYS WHETHER BOOST PASSES THE CEILING (#579).  With
# boost on and cpuinfo_max_freq above scaling_max_freq, the same picture has
# been measured both ways:
#
#   pavillion, acpi-cpufreq, `performance', ceiling 3000 = its nominal clock
#                  -> 3992 MHz idle, 3918 with six busy loops: OVER (#544)
#   victus, amd-pstate-epp, `performance', ceiling 3300 = its nominal clock
#                  -> 3268 MHz on all twelve under a full load, UNDER; with
#                     no ceiling (under `powersave', balance_power) the same
#                     load ran at 3693-3793, so boost was there to be had
#   victus, ceiling 1400
#                  -> boots of 20.9 s against 12.0 s at 4280: UNDER
#
# The first version of this reading believed pavillion and printed
# effective=4280MHz for every capped run on victus.  The second asked
# uros_clock_policy's question -- does the driver take the policy -- and
# review refuted that from the kernel's source: amd-pstate in passive or
# guided mode lists every governor, so it reads as the core's, and it writes
# the ceiling into the same CPPC request -epp does.  Intel's acpi-cpufreq
# reaches turbo only through a table entry the ceiling can exclude.  Whether
# boost gets past depends on how each driver meets the hardware, and no file
# here states it.
#
# So the reading no longer concludes where it cannot know:
#
# - Where the core drives the clock and boost is on, the line gives the
#   ceiling and says a clock above it is not excluded -- with no number and
#   whether or not cpuinfo_max_freq stands above the ceiling.  Intel's
#   acpi-cpufreq without CPPC shows cpuinfo_max_freq == scaling_max_freq ==
#   the turbo entry (base + 1 MHz) and still turbos far past it.
# - Under a driver that takes the policy, the ceiling HOLDS -- boost does
#   not pass it -- and the line gives the ceiling as the most the run can
#   get; below it the clock is the driver's, from the bias AND the power
#   source (victus under balance_power, ceiling 4280: 2096 on battery, 3981
#   on AC, 3693-3793 under a 12-thread load).  Measured on victus
#   (amd-pstate-epp, kernel 7.1), and in the kernel's source
#   intel_pstate with HWP writes it into HWP_MAX_PERF.  Two exceptions are
#   known from that source and occur on no machine here: intel_pstate in
#   ACTIVE mode without HWP lets turbo pass a ceiling set inside the turbo
#   range, and amd-pstate-epp before 6.8 never read scaling_max_freq at
#   all.  (Without HWP intel_pstate defaults to PASSIVE mode, intel_cpufreq,
#   which lists governors, reads as the core's and gets the hedge: see
#   uros_boost_state for where its turbo switch is.)
# - Wherever it runs, the ceiling is a claim that cpu= can refute; see below
#   for how coarse that is.
#
# 🔑 What settles it is the clock measured while the run is on the
# processor, and that is not this line's job: see uros_clock_run_line.
uros_clock_policy() {
	case "$_avail" in
	"performance powersave")	_who=driver ;;
	"")				_who=unknown ;;
	*)				_who=core ;;
	esac

	if [ -n "$_capk" ]; then
		_cap="$(( _capk / 1000 ))MHz"
	else
		_cap="?"
	fi
	_top="$_cap"
	if [ "$_who" = core ] && [ "$_boost" = 1 ] && [ -n "$_capk" ]; then
		_top="$_cap (boost on: a clock above it is not excluded, #579)"
	fi
	# 🔴 A processor sampled more than 10% above the ceiling refutes it,
	# whatever the driver.  That is the same threshold as the floor, for
	# the same reason.  It is coarse on purpose, and it only catches a
	# clock far above the ceiling: pavillion's 3992 over 3000 (133%), or a
	# 1400 ceiling under a 3.7 GHz sample.  All-core boost on victus is
	# 12-15% above nominal, and a sample taken outside the load can sit
	# inside the margin while the loaded cores were past it: six of the
	# eight host-end samples under `performance' there, taken as qemu
	# exits, read 3243-3444 around a nominal of 3300.
	if [ -n "$_capk" ] && [ "${_mhz:-?}" -gt 0 ] 2>/dev/null &&
	   [ $(( _mhz * 100 )) -gt $(( _capk / 1000 * 110 )) ]; then
		_top="unsettled: ceiling ${_cap}, processor at ${_mhz}MHz (#579)"
	fi

	case "$_who:$_gov" in
	driver:performance)
		_eff="$_top" ;;
	driver:*)
		_eff="$_top (the driver scales the range, $_gov is its bias)" ;;
	core:performance|core:ondemand|core:conservative|core:schedutil)
		_eff="$_top" ;;
	core:powersave)
		if [ -z "$_mink" ]; then
			_eff="?"
		else
			_floor=$(( _mink / 1000 ))
			# 🔴 A CLAIM THE FIELD BESIDE IT CAN REFUTE.  "pins
			# it to the floor" says the core CANNOT be above the
			# floor, and cpu= says where it is.  #564 printed the
			# two contradicting each other on every line for a
			# day and nothing read them together, so the reading
			# is no longer asserted over a processor standing
			# above it.  The list of driver names has gone; this
			# is what catches whatever takes its place.
			#
			# 10% is uros_clock_run_moved's threshold and for its
			# reason: two real states are far apart (1108 against
			# 3703 here) and two samples of one state are not.
			if [ "${_mhz:-?}" -gt 0 ] 2>/dev/null &&
			   [ $(( _mhz * 100 )) -gt $(( _floor * 110 )) ]; then
				_eff="unsettled: floor ${_floor}MHz, processor at ${_mhz}MHz (#564)"
			else
				_eff="${_floor}MHz (governor pins it to the floor)"
			fi
		fi ;;
	*)
		_eff="$_top (driver $_drv, governor $_gov: unknown policy)" ;;
	esac
}
# uros_conditions_open  — call BEFORE the run.  Remembers the starting host
# state in UROS_HOST_AT_START.
uros_conditions_open() {
	UROS_HOST_AT_START=$(uros_host_state)
	export UROS_HOST_AT_START
}

# uros_conditions_block <target> <accelerator> [ "label: value" ... ]
#
# The whole block on stdout, one fact per line.  The caller decides where it
# goes; both callers send it to the screen and append it to the log.
#
# ⚠️ No word here may be one the verdict greps for.  run-x86_64.sh judges a
# boot by looking for WRONG and FAIL in the log, so a conditions block using
# either would turn every run red.
uros_conditions_block() {
	_target=$1
	_accel=$2
	shift 2

	echo "=== run conditions (#516) ==="
	echo "  target:       $_target"
	echo "  accelerator:  $_accel"
	for _f in "$@"; do
		[ -n "$_f" ] && echo "  $_f"
	done
	# 🔴 WHICH MACHINE, because a baseline belongs to one and no saved log in
	# this project could say which.  #564 asks whether the other AMD laptop's
	# recorded numbers carry the same false clock line, and every log here can
	# be read for its governor, its ceiling and its accelerator while not one
	# of them names the host that produced it -- so that question has to be
	# answered by remembering instead of by grepping, which is the failure
	# #516 was opened about, one field along.
	echo "  host:         $(hostname -s 2>/dev/null || echo '?')"
	echo "  host start:   ${UROS_HOST_AT_START:-not sampled}"
	# ⚠️ A closing sample only where the harness is still alive to take one.
	# run-qemu.sh ends in `exec qemu', deliberately -- without it that shell
	# stays qemu's parent for the whole run -- so on i386 nothing runs after
	# the machine does.  Said out loud rather than left as a missing line,
	# because a reader comparing two runs needs to know which of them could
	# have noticed its clock moving.
	echo "  host end:     ${UROS_HOST_AT_END:-not sampled (this harness execs qemu)}"
	echo "=== end run conditions ==="
}

# uros_clock_run_moved [second ...] -- did the run's clock move, by enough to
# matter?  True when the median of the first half of its readings and that
# of the second half differ by 10% or more; with fewer than four readings it
# claims nothing.
#
# 🔑 A verdict, not a number, because the question a reader has is not "what
# was the clock" but "may I compare this run with another one".  10% because
# two real states are far apart (1.4 and 3.9 GHz here) and one boosted second
# is not a state: medians of halves let a single second pass.
#
# ⚠️ It used to compare cpu= at host start with cpu= at host end: cpu0, once
# before make-disk and once after qemu was killed -- neither inside the run,
# and at rest each is as likely the policy's number as a clock.  Beside the
# per-second readings it warned "do not compare" for runs whose readings sat
# within 1.5% of each other (#579, sixth review).
uros_clock_run_moved() {
	_nums=""
	for _t in "$@"; do
		case "$_t" in
		''|*[!0-9]*) ;;
		*) _nums="$_nums $_t" ;;
		esac
	done
	# shellcheck disable=SC2086
	set -- $_nums
	[ $# -ge 4 ] || return 1
	_h=$(( $# / 2 ))
	_a=$(printf '%s\n' "$@" | head -n "$_h" | sort -n | sed -n "$(( (_h + 1) / 2 ))p")
	_b=$(printf '%s\n' "$@" | tail -n "$_h" | sort -n | sed -n "$(( (_h + 1) / 2 ))p")
	_d=$(( _b - _a ))
	[ "$_d" -lt 0 ] && _d=$(( -_d ))
	[ $(( _d * 100 )) -ge $(( _a * 10 )) ]
}

# ── The clock the run ran at, measured while it ran (#579) ────────────────
#
# 🔴 effective= above is DEDUCED, and three issues have now been spent on the
# deduction: #544 (a static governor read as a ramping one), #564 (a driver
# read as the wrong kind) and #579 (boost read as passing a ceiling that
# held).  Each was a plausible rule about cpufreq that was false on some
# machine, and the last one ends by saying what no file here states.  The
# answer to "what clock did this run get" is not in sysfs.  It is in the
# processor, while the run is on it.
#
# So the harness asks the processor once a second while qemu is alive, and
# the conditions block says what it answered.  It asks about the cores that
# are running qemu's threads at that second, not about the machine: a host
# thread in state R is on a core, and /proc/<pid>/task/*/stat names which.
# The reading at a second is the fastest of those cores; the median over the
# run is the clock the run got, and the maximum is how far boost went.
#
# 🔴 AND A READING CAN BE THE POLICY'S NUMBER, NOT THE PROCESSOR'S.  On x86,
# cpuinfo_avg_freq, scaling_cur_freq and /proc/cpuinfo's cpu MHz are all
# arch_freq_get_on_cpu(): APERF/MPERF over the last tick -- and for a core
# whose last tick is older than 20 ms, the policy's number instead
# (policy->cur, or cpu_khz with no cpufreq).  On victus that number is
# 1108930 kHz to the kHz, scaling_min_freq; under acpi-cpufreq it is the
# table entry last asked for, the ceiling under `performance'.  The first
# version read the fastest core of the machine and so took idle cores'
# policy numbers (fourth review); reading only the cores qemu's threads are
# on is not enough either, because a thread just woken onto a core that had
# been idle is in state R before that core ticks -- 2-6% of such readings on
# victus came back as 1108.930 (fifth review).
#
# 🔑 So a reading equal, to the kHz, to a number the policy files state is
# not taken as a measurement.  A ratio of two counters lands on 1397.371,
# 1395.457, 2096.155; the policy's numbers are the ones written in
# cpufreq/*_freq and scaling_available_frequencies.  The acpi-cpufreq file
# cpuinfo_cur_freq is left out: it reads the P-state request, not the
# counters.
#
# ⚠️ WHERE THE FALLBACK IS A NUMBER NO FILE STATES, it cannot be recognised,
# and the line says so rather than counting it silently:
#   - no cpufreq at all: the fallback is cpu_khz, the TSC's nominal clock.
#     Busy cores still read their counters, but a stale one cannot be told
#     from them, so the harness does not measure.
#   - a driver the core drives that has no frequency table (amd-pstate in
#     passive or guided mode -- victus can be switched into it -- and
#     intel_cpufreq): policy->cur is the governor's last target, any kHz.
#     uros_clock_fallback_note reads that off the machine and the line
#     carries it.
#   - intel_pstate in active mode answers with its own get(); no file tells
#     it from amd-pstate-epp, whose fallback is scaling_min_freq (measured).
#     On no machine here.

# uros_clock_policy_khz [root] -- every frequency the policy files state, in
# kHz, sorted, one line each: the numbers a reading must not be mistaken for.
# root is /sys/devices/system/cpu; --self-test hands it a tree of its own.
uros_clock_policy_khz() {
	_r=${1:-/sys/devices/system/cpu}
	for _f in "$_r"/cpu[0-9]*/cpufreq/*_freq \
		  "$_r"/cpu[0-9]*/cpufreq/base_frequency \
		  "$_r"/cpu[0-9]*/cpufreq/scaling_available_frequencies
	do
		case "$_f" in
		*/cpuinfo_avg_freq|*/scaling_cur_freq|*/cpuinfo_cur_freq) continue ;;
		esac
		[ -r "$_f" ] && cat "$_f" 2>/dev/null
	done | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -un
}

# uros_clock_fallback_note <scaling_available_governors> <table: 1 or 0>
# -- the words the clock line carries when a stale core's fallback cannot be
# recognised: a driver the core drives (it lists governors) with no frequency
# table falls back to the governor's last target, which no file states.
uros_clock_fallback_note() {
	case "$1" in
	"performance powersave"|"") return 0 ;;
	esac
	[ "$2" = 1 ] && return 0
	echo "; a stale core's fallback here is the governor's last target, which no file states, and may be counted"
}

# uros_clock_cpus -- /proc/<pid>/task/*/stat on stdin; prints " c1 c2 ..." ,
# the cores that threads in state R are on.  The comm field may hold spaces
# and parentheses, so everything up to the LAST ") " goes; then the state is
# $1 and field 39, the processor, is $37.
uros_clock_cpus() {
	awk '{ sub(/^.*\) /, ""); if ($1 == "R") printf " %s", $37 }'
}

# uros_clock_pick <cores> <policy kHz, space-separated> -- /proc/cpuinfo on
# stdin; prints, in MHz, the fastest of <cores> whose reading is not one of
# the policy's numbers, or nothing.
uros_clock_pick() {
	awk -v want="$1 " -v pol=" $2 " '
		/^processor/ { p = $3 }
		/^cpu MHz/ {
			khz = int($4 * 1000 + 0.5)
			if (!index(want, " " p " ")) next
			if (index(pol, " " khz " ")) next
			if (khz > m) m = khz
		}
		END { if (m) print int(m / 1000 + 0.5) }'
}

# uros_clock_now <pid> <policy kHz> -- one sample of the live machine: the
# two pieces above on /proc.  Prints nothing when no reading qualifies.
uros_clock_now() {
	_cpus=$(cat /proc/"$1"/task/*/stat 2>/dev/null | uros_clock_cpus)
	[ -n "$_cpus" ] || return 0
	uros_clock_pick "$_cpus" "$2" < /proc/cpuinfo 2>/dev/null
}

# uros_clock_run_line <ceiling MHz, or empty> [second ...]
#
# The summary, from what each second answered -- a number of MHz, or "-" for
# a second with no reading -- so that --self-test can drive it and the log
# keeps every second, not only the ones that answered.
# 🔑 The ceiling is compared with the MEDIAN and not the maximum: one boosted
# second is boost, a run that sat above the ceiling is a ceiling that did not
# hold.  10% for the reason every other threshold in this file uses it.
uros_clock_run_line() {
	_ceil=$1
	shift
	_asked=$#
	_nums=""
	for _t in "$@"; do
		case "$_t" in
		''|*[!0-9]*) ;;
		*) _nums="$_nums $_t" ;;
		esac
	done
	_secs="$_asked seconds"
	[ "$_asked" -eq 1 ] && _secs="1 second"
	if [ "$_asked" -eq 0 ]; then
		echo "not measured: the run ended before the first sample, 1-2 s after qemu started"
		return
	fi
	# shellcheck disable=SC2086
	set -- $_nums
	if [ $# -eq 0 ]; then
		echo "not measured: no reading in $_secs (no qemu thread running, or only the policy's number)"
		return
	fi
	_sorted=$(printf '%s\n' "$@" | sort -n)
	_med=$(printf '%s\n' "$_sorted" | sed -n "$(( ($# + 1) / 2 ))p")
	_max=$(printf '%s\n' "$_sorted" | tail -n 1)
	_line="median ${_med}MHz, max ${_max}MHz, read in $# of $_secs"
	if [ -n "$_ceil" ] && [ $(( _med * 100 )) -gt $(( _ceil * 110 )) ]; then
		_line="$_line -- ABOVE the ${_ceil}MHz ceiling"
	fi
	echo "$_line"
}

# ── --self-test: every reading, driven over machines this one is not ──
#
# 🔑 Wired into the build (uros/CMakeLists.txt) instead of left to be typed,
# for #549's reason: three checkers were written to make defects impossible
# and nothing ran any of them.  It boots nothing, touches no hardware and
# costs milliseconds -- it sets the same variables uros_host_state samples
# and reads back the one field they decide.
#
# 🔴 A ROW SAYS WHETHER IT WAS SAMPLED, and a sampled one is not invented to
# please the classifier: it names where it came from.  The rest are headed
# "probes" -- chosen inputs that pin one branch each, so that breaking the
# branch turns a row BAD.  Row 2 of the first table is this laptop, byte for
# byte out of sysfs on the day #564 was opened; row 4
# is pavillion, whose eleven runs were taken as the high-clock arm under a
# governor that was pinning them (#544).  A table written to match the code
# tests the code against itself.
#
# 🔑 Row 4 is also the one that settles who is affected, and it settles it by
# MEASUREMENT rather than by naming a driver: on that machine six busy loops
# left the clock at 1397 MHz, which is a thing an active driver's `powersave'
# cannot do.  So the reading it gets here is the reading it always got, and
# nothing recorded there was ever labelled by the false line.
#
# ⚠️ Guarded on $0, because this file is SOURCED: without it run-qemu.sh's own
# first argument would be read as this one's.
case "$0" in
*run-conditions.sh)
	if [ "${1:-}" != --self-test ]; then
		echo "run-conditions.sh is sourced, not run: see the head of it" >&2
		echo "usage: sh run-conditions.sh --self-test" >&2
		exit 2
	fi

	_fails=0
	_total=0
	echo "run-conditions --self-test (#564)"

	# name|_drv|_gov|_avail|_mink|_capk|_boost|_mhz|the effective= it must read
	while IFS='|' read -r _name _drv _gov _avail _mink _capk _boost _mhz _want
	do
		[ -n "$_name" ] || continue
		case "$_name" in \#*) continue ;; esac
		_total=$(( _total + 1 ))
		_eff=
		uros_clock_policy
		if [ "$_eff" = "$_want" ]; then
			echo "  ok    $_name"
		else
			echo "  BAD   $_name"
			echo "        wanted <<$_want>>"
			echo "        read   <<$_eff>>"
			_fails=$(( _fails + 1 ))
		fi
	done <<'EOF'
# an ACTIVE driver: `powersave' is the whole range with a bias
intel_pstate, powersave|intel_pstate|powersave|performance powersave|400000|3900000||1200|3900MHz (the driver scales the range, powersave is its bias)
amd-pstate-epp, powersave (victus, #564)|amd-pstate-epp|powersave|performance powersave|1108930|4280985|1|3703|4280MHz (the driver scales the range, powersave is its bias)
intel_pstate, performance|intel_pstate|performance|performance powersave|400000|3900000||3900|3900MHz
# a PASSIVE one: the core runs a governor and `powersave' is static
pavillion, powersave pinned (#544)|acpi-cpufreq|powersave|conservative ondemand userspace powersave performance schedutil |1400000|3000000|1|1397|1400MHz (governor pins it to the floor)
acpi-cpufreq, ondemand, at rest|acpi-cpufreq|ondemand|conservative ondemand userspace powersave performance schedutil |1400000|3000000|1|2100|3000MHz (boost on: a clock above it is not excluded, #579)
amd-pstate passive, powersave|amd-pstate|powersave|conservative ondemand userspace powersave performance schedutil |1108930|4280985|1|1108|1108MHz (governor pins it to the floor)
# #579: boost and the ceiling, sampled -- pavillion from 246c11ce, victus from
# ~/uros-tests/579-campioni.txt
pavillion, performance, boost over its ceiling (#544)|acpi-cpufreq|performance|conservative ondemand userspace powersave performance schedutil |1400000|3000000|1|3993|unsettled: ceiling 3000MHz, processor at 3993MHz (#579)
victus capped at 1400, the run #579 was opened on|amd-pstate-epp|powersave|performance powersave|1108930|1400000|1|1397|1400MHz (the driver scales the range, powersave is its bias)
victus capped at its nominal 3300, performance|amd-pstate-epp|performance|performance powersave|1108930|3300000|1|3265|3300MHz
# probes, from the kernel's source: passive amd-pstate meets the ceiling the way
# -epp does yet reads as the core's; Intel's acpi-cpufreq without CPPC has its
# ceiling on the turbo entry and turbos past it
amd-pstate passive under a ceiling|amd-pstate|schedutil|conservative ondemand userspace powersave performance schedutil |1108930|1400000|1|1397|1400MHz (boost on: a clock above it is not excluded, #579)
Intel acpi-cpufreq, ceiling on the turbo entry|acpi-cpufreq|ondemand|conservative ondemand userspace powersave performance schedutil |800000|2601000|1|1200|2601MHz (boost on: a clock above it is not excluded, #579)
conservative gets the same words|acpi-cpufreq|conservative|conservative ondemand userspace powersave performance schedutil |1400000|3000000|1|2100|3000MHz (boost on: a clock above it is not excluded, #579)
Intel without HWP: intel_cpufreq, turbo on by no_turbo=0|intel_cpufreq|schedutil|conservative ondemand userspace powersave performance schedutil |800000|3000000|1|2990|3000MHz (boost on: a clock above it is not excluded, #579)
boost on and no ceiling to read|acpi-cpufreq|ondemand|conservative ondemand userspace powersave performance schedutil |1400000||1|2100|?
# probes: no boost to speak of -- switched off, or no boost file at all
acpi-cpufreq, boost off|acpi-cpufreq|ondemand|conservative ondemand userspace powersave performance schedutil |1400000|3000000|0|2990|3000MHz
acpi-cpufreq, no boost file|acpi-cpufreq|ondemand|conservative ondemand userspace powersave performance schedutil |1400000|3000000||2990|3000MHz
# probes: the ceiling is a claim wherever it is read, and 10% is its margin
the processor stands above a driver's ceiling|amd-pstate-epp|powersave|performance powersave|1108930|1400000|1|3703|unsettled: ceiling 1400MHz, processor at 3703MHz (#579) (the driver scales the range, powersave is its bias)
above the ceiling with boost off|acpi-cpufreq|performance|conservative ondemand userspace powersave performance schedutil |1400000|3000000|0|3993|unsettled: ceiling 3000MHz, processor at 3993MHz (#579)
above the ceiling under userspace|acpi-cpufreq|userspace|conservative ondemand userspace powersave performance schedutil |1400000|3000000|1|3993|unsettled: ceiling 3000MHz, processor at 3993MHz (#579) (driver acpi-cpufreq, governor userspace: unknown policy)
above the ceiling where no governors are listed|acpi-cpufreq|performance||1400000|3000000|1|3993|unsettled: ceiling 3000MHz, processor at 3993MHz (#579) (driver acpi-cpufreq, governor performance: unknown policy)
exactly 10% over the ceiling|amd-pstate-epp|performance|performance powersave|1108930|1400000|1|1540|1400MHz
just past 10% over the ceiling|amd-pstate-epp|performance|performance powersave|1108930|1400000|1|1541|unsettled: ceiling 1400MHz, processor at 1541MHz (#579)
no ceiling to hold the sample against|amd-pstate-epp|performance|performance powersave|1108930||1|3000|?
no sample to hold against the ceiling|amd-pstate-epp|performance|performance powersave|1108930|1400000|1|?|1400MHz
# 🔑 the row that refutes repairing this by name: intel_pstate in passive mode
# calls itself intel_cpufreq, and there `powersave' really does pin
intel_cpufreq, powersave|intel_cpufreq|powersave|conservative ondemand userspace powersave performance schedutil |800000|4000000||798|800MHz (governor pins it to the floor)
# the literal and not a prefix of it
a list that starts with those two words|acpi-cpufreq|powersave|performance powersave schedutil |1400000|3000000|1|1397|1400MHz (governor pins it to the floor)
# the floor is a claim, and the field beside it can refute the claim
the processor stands above its own floor|acpi-cpufreq|powersave|conservative ondemand powersave performance schedutil |1108930|4280985|1|3703|unsettled: floor 1108MHz, processor at 3703MHz (#564)
# and what is not known is said instead of guessed
powersave with no floor to read|acpi-cpufreq|powersave|conservative ondemand powersave performance schedutil ||3000000||1400|?
a governor with no policy of its own|acpi-cpufreq|userspace|conservative ondemand userspace powersave performance schedutil |1400000|3000000||1400|3000MHz (driver acpi-cpufreq, governor userspace: unknown policy)
the same, with boost on|acpi-cpufreq|userspace|conservative ondemand userspace powersave performance schedutil |1400000|3000000|1|2100|3000MHz (boost on: a clock above it is not excluded, #579) (driver acpi-cpufreq, governor userspace: unknown policy)
a driver that lists no governors|acpi-cpufreq|performance||1400000|3000000|1|2100|3000MHz (driver acpi-cpufreq, governor performance: unknown policy)
a machine that will not say|?|?|||||?|? (driver ?, governor ?: unknown policy)
EOF

	# The measured clock (#579): name|ceiling MHz|what each second answered|the line it must read
	while IFS='|' read -r _name _ceil _secs _want
	do
		[ -n "$_name" ] || continue
		case "$_name" in \#*) continue ;; esac
		_total=$(( _total + 1 ))
		# shellcheck disable=SC2086
		_got=$(uros_clock_run_line "$_ceil" $_secs)
		if [ "$_got" = "$_want" ]; then
			echo "  ok    $_name"
		else
			echo "  BAD   $_name"
			echo "        wanted <<$_want>>"
			echo "        read   <<$_got>>"
			_fails=$(( _fails + 1 ))
		fi
	done <<'EOF'
# sampled: real runs' "clock by sec:" lines, victus on AC, ~/uros-tests/<log>
ceiling 1400 (579-politica-soffitto-1400-172406)|1400|1397 1397 1397 1396 1397 1397 1397 1397 1397 1397 1397 1397 1397 1397 -|median 1397MHz, max 1397MHz, read in 14 of 15 seconds
performance (579-politica-performance-172555)|4280|3940 3967 3926 3983 3946 - - -|median 3946MHz, max 3983MHz, read in 5 of 8 seconds
powersave, balance_power (579-politica-balance-power-172926)|4280|3893 3985 3927 3981 3998 - - -|median 3981MHz, max 3998MHz, read in 5 of 8 seconds
# probes
pavillion's two readings (idle 3992, six busy 3918) over its 3000 ceiling|3000|3918 3992|median 3918MHz, max 3992MHz, read in 2 of 2 seconds -- ABOVE the 3000MHz ceiling
one second of boost is boost, not a ceiling that failed|1400|1397 1397 3700|median 1397MHz, max 3700MHz, read in 3 of 3 seconds
exactly 10% over, by the median|1400|1540|median 1540MHz, max 1540MHz, read in 1 of 1 second
just past 10%, by the median|1400|1541|median 1541MHz, max 1541MHz, read in 1 of 1 second -- ABOVE the 1400MHz ceiling
an even count takes the lower middle|4280|1000 2000 3000 4000|median 2000MHz, max 4000MHz, read in 4 of 4 seconds
sorted by value, not by text|4280|900 3268 1200|median 1200MHz, max 3268MHz, read in 3 of 3 seconds
seconds with no reading count as asked, not as samples|4280|- 2096 - 2171 2096|median 2096MHz, max 2171MHz, read in 3 of 5 seconds
no ceiling, samples still summarised||3268 3269|median 3268MHz, max 3269MHz, read in 2 of 2 seconds
the run over before the first sample|1400||not measured: the run ended before the first sample, 1-2 s after qemu started
one second asked, no reading|1400|-|not measured: no reading in 1 second (no qemu thread running, or only the policy's number)
seven seconds asked, no reading|1400|- - - - - - -|not measured: no reading in 7 seconds (no qemu thread running, or only the policy's number)
EOF

	# The sampler (#579): name|threads comm@state@core;...|cores core:MHz,...|policy kHz|MHz it must pick
	# Each thread becomes a real /proc/<pid>/task/<tid>/stat line: fields 4-37
	# hold 104-137, field 38 (exit_signal) 17 and field 40 (rt_priority) 0.  A
	# reader one field early lands on core 17, which no row has; one field
	# late lands on core 0, which rows hold as a decoy with another clock.
	# Each core becomes a real /proc/cpuinfo pair.
	while IFS='|' read -r _name _threads _cores _pol _want
	do
		[ -n "$_name" ] || continue
		case "$_name" in \#*) continue ;; esac
		_total=$(( _total + 1 ))
		_stat=$(printf '%s\n' "$_threads" | tr ';' '\n' | awk -F@ 'NF == 3 {
			printf "%d (%s) %s", NR, $1, $2
			for (f = 4; f <= 37; f++) printf " %d", 100 + f
			printf " 17 %s", $3
			for (f = 40; f <= 52; f++) printf " 0"
			printf "\n" }')
		_info=$(printf '%s\n' "$_cores" | tr ',' '\n' | awk -F: 'NF == 2 {
			printf "processor\t: %s\nvendor_id\t: AuthenticAMD\ncpu MHz\t\t: %s\n\n", $1, $2 }')
		_got=$(printf '%s\n' "$_info" |
		       uros_clock_pick "$(printf '%s\n' "$_stat" | uros_clock_cpus)" "$_pol")
		if [ "$_got" = "$_want" ]; then
			echo "  ok    $_name"
		else
			echo "  BAD   $_name"
			echo "        wanted <<$_want>>"
			echo "        read   <<$_got>>"
			_fails=$(( _fails + 1 ))
		fi
	done <<'EOF'
# probes (cores 7 and 11 read as the prototype on victus read them; the decoys are chosen)
two running threads, their cores only|sh@R@7;sh@R@11|0:1676.000,7:2096.155,11:2096.143,5:3867.412|1108930 1400000 4280985|2096
a sleeping thread's core is not the run's|qemu-system-x86@S@3;CPU 0/KVM@R@5|3:3993.000,5:1397.412|1108930 1400000 4280985|1397
a comm with spaces and parentheses|qemu (a) b) c@R@5|5:1397.412,0:3000.001|1108930|1397
core 1 is not core 11|CPU 1/KVM@R@11|1:3900.123,11:1500.456|1108930|1500
the fastest of two, listed first|CPU 0/KVM@R@5;CPU 1/KVM@R@7|5:3867.000,7:1397.412|1108930|3867
the fastest of two, listed last|CPU 0/KVM@R@7;CPU 1/KVM@R@5|7:1397.412,5:3867.000|1108930|3867
a thread blocked in D is not running|worker@D@2;CPU 0/KVM@R@5|2:3900.000,5:1397.412,0:2500.000|1108930|1397
a stopped or dead thread is not running|worker@T@2;other@Z@3;CPU 0/KVM@R@5|2:3900.000,3:3800.000,5:1397.412|1108930|1397
the policy's number is not a reading|CPU 0/KVM@R@3;CPU 1/KVM@R@4|3:1108.930,4:1397.300|1108930 1400000 4280985|1397
only the policy's number: no reading|CPU 0/KVM@R@3|3:1108.930|1108930 1400000 4280985|
one kHz off the policy's number is a reading|CPU 0/KVM@R@3|3:1108.931|1108930 1400000 4280985|1109
a reading inside a longer policy number is a reading|CPU 0/KVM@R@3|3:400.000|1108930 1400000 4280985|400
MHz are rounded, not cut|CPU 0/KVM@R@3|3:1397.612|1108930 1400000 4280985|1398
a policy number floating point would cut a kHz short|CPU 0/KVM@R@3|3:2048.006|2048006|
a table entry is a policy number too (acpi-cpufreq)|CPU 0/KVM@R@2|2:3000.000|1400000 2100000 3000000 4000000|
no thread running: no reading|qemu-system-x86@S@2;CPU 0/KVM@S@6|2:3900.000,6:3901.000|1108930|
EOF

	# The boost switch (#579): name|cpufreq/boost|intel_pstate/no_turbo|reads
	while IFS='|' read -r _name _b _nt _want
	do
		[ -n "$_name" ] || continue
		case "$_name" in \#*) continue ;; esac
		_total=$(( _total + 1 ))
		_got=$(uros_boost_state "$_b" "$_nt")
		if [ "$_got" = "$_want" ]; then
			echo "  ok    $_name"
		else
			echo "  BAD   $_name"
			echo "        wanted <<$_want>>"
			echo "        read   <<$_got>>"
			_fails=$(( _fails + 1 ))
		fi
	done <<'EOF'
victus: the general switch, on|1||1
the general switch, off|0||0
intel_pstate: no general switch, no_turbo=0 is turbo ON||0|1
intel_pstate: no general switch, no_turbo=1 is turbo off||1|0
both present, the general one wins|0|0|0
neither file: not known|||
EOF

	# Did the clock move (#579): name|what each second answered|yes or no
	while IFS='|' read -r _name _secs _want
	do
		[ -n "$_name" ] || continue
		case "$_name" in \#*) continue ;; esac
		_total=$(( _total + 1 ))
		# shellcheck disable=SC2086
		if uros_clock_run_moved $_secs; then _got=yes; else _got=no; fi
		if [ "$_got" = "$_want" ]; then
			echo "  ok    $_name"
		else
			echo "  BAD   $_name"
			echo "        wanted <<$_want>>"
			echo "        read   <<$_got>>"
			_fails=$(( _fails + 1 ))
		fi
	done <<'EOF'
# sampled: 579-politica-performance-172555, which the old test called moved
performance on AC, steady within 1.5%|3940 3967 3926 3983 3946 - - -|no
# probes
the #560 shape: 3000 then 3993|3000 3000 3000 3993 3993 3993|yes
and back down|3993 3993 3993 3000 3000 3000|yes
one slow second is not a move|3893 3893 3918 3918 3202 3943 3952|no
exactly 10% is a move|1000 1000 1100 1100|yes
just under 10% is not|1000 1000 1099 1099|no
seconds with no reading are not readings|1000 - - 1000 1100 - 1100|yes
three readings: nothing claimed|1000 5000 9000|no
EOF

	# The fallback note (#579): name|scaling_available_governors|table 1/0|note
	while IFS='|' read -r _name _avail _table _want
	do
		[ -n "$_name" ] || continue
		case "$_name" in \#*) continue ;; esac
		_total=$(( _total + 1 ))
		_got=$(uros_clock_fallback_note "$_avail" "$_table")
		if [ "$_got" = "$_want" ]; then
			echo "  ok    $_name"
		else
			echo "  BAD   $_name"
			echo "        wanted <<$_want>>"
			echo "        read   <<$_got>>"
			_fails=$(( _fails + 1 ))
		fi
	done <<'EOF'
victus, amd-pstate-epp: its fallback is scaling_min_freq|performance powersave|0|
acpi-cpufreq: its fallback is a table entry|conservative ondemand userspace powersave performance schedutil|1|
amd-pstate passive, intel_cpufreq: no table|conservative ondemand userspace powersave performance schedutil|0|; a stale core's fallback here is the governor's last target, which no file states, and may be counted
no governors to read|||
EOF

	# The policy numbers (#579): two sysfs trees written here and read back.
	# 🔑 Made rather than described, because uros_clock_policy_khz's subject
	# is which FILES count, and only files can show a glob, an exclusion or a
	# sort gone wrong.
	_tree=$(mktemp -d "${TMPDIR:-/tmp}/run-conditions.XXXXXX")
	mkdir -p "$_tree/a/cpu0/cpufreq" "$_tree/a/cpu11/cpufreq" "$_tree/b/cpu0/cpufreq"
	_w() { printf '%s\n' "$2" > "$_tree/$1"; }
	# a: victus's cpu0 as sysfs has it, the two counters among them, and an
	# eleventh core whose ceiling only it states
	_w a/cpu0/cpufreq/scaling_min_freq 1108930
	_w a/cpu0/cpufreq/scaling_max_freq 1400000
	_w a/cpu0/cpufreq/cpuinfo_min_freq 412625
	_w a/cpu0/cpufreq/cpuinfo_max_freq 4280985
	_w a/cpu0/cpufreq/amd_pstate_max_freq 4280985
	_w a/cpu0/cpufreq/amd_pstate_lowest_nonlinear_freq 1108930
	_w a/cpu0/cpufreq/cpuinfo_avg_freq 1397371
	_w a/cpu0/cpufreq/scaling_cur_freq 1397425
	_w a/cpu0/cpufreq/scaling_setspeed '<unsupported>'
	_w a/cpu11/cpufreq/scaling_max_freq 2900000
	# b: acpi-cpufreq's table, and the request it reads as its current clock
	_w b/cpu0/cpufreq/scaling_available_frequencies '3000000 2100000 1400000 '
	_w b/cpu0/cpufreq/scaling_min_freq 1400000
	_w b/cpu0/cpufreq/scaling_max_freq 3000000
	_w b/cpu0/cpufreq/cpuinfo_max_freq 4000000
	_w b/cpu0/cpufreq/cpuinfo_cur_freq 2345678
	_w b/cpu0/cpufreq/base_frequency 2600000
	while IFS='|' read -r _name _root _want
	do
		[ -n "$_name" ] || continue
		case "$_name" in \#*) continue ;; esac
		_total=$(( _total + 1 ))
		_got=$(uros_clock_policy_khz "$_tree/$_root" | tr '\n' ' ')
		if [ "$_got" = "$_want" ]; then
			echo "  ok    $_name"
		else
			echo "  BAD   $_name"
			echo "        wanted <<$_want>>"
			echo "        read   <<$_got>>"
			_fails=$(( _fails + 1 ))
		fi
	done <<'EOF'
victus: the policy's numbers, not its counters, every core, in order|a|412625 1108930 1400000 2900000 4280985 
acpi-cpufreq: the table, base_frequency, not the request|b|1400000 2100000 2600000 3000000 4000000 
no cpufreq at all|none|
EOF
	rm -rf "$_tree"

	if [ "$_fails" -gt 0 ]; then
		echo "run-conditions --self-test: $_fails of $_total read otherwise"
		exit 1
	fi
	echo "run-conditions --self-test: all $_total read as expected"
	exit 0
	;;
esac
