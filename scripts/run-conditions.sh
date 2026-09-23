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
	# nothing else on the line would tell them apart.  What it does to this
	# machine's clock is unmeasured, and the line does not say that it does.
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
# - Under a driver that takes the policy, the ceiling is the clock: measured
#   on victus (amd-pstate-epp, kernel 7.1), and in the kernel's source
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
			# 10% is uros_clock_moved's threshold and for its
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

# Did the clock move between the two samples, and by enough to matter?
#
# 🔑 A verdict, not a number, because the question a reader has is not "what
# was the clock" but "may I compare this run with another one".  10% is the
# threshold: this machine's two states are 1.4 GHz and 3.99, so anything real
# is far above it, and jitter between two samples of the same state is far
# below.
uros_clock_moved() {
	_a=$(echo "${UROS_HOST_AT_START:-}" | sed -n 's/.*cpu=\([0-9]*\)MHz.*/\1/p')
	_b=$(echo "$1" | sed -n 's/.*cpu=\([0-9]*\)MHz.*/\1/p')
	[ -n "$_a" ] && [ -n "$_b" ] || return 1
	[ "$_a" -gt 0 ] 2>/dev/null || return 1
	_d=$(( _b - _a ))
	[ "$_d" -lt 0 ] && _d=$(( -_d ))
	[ $(( _d * 100 / _a )) -ge 10 ]
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
# 🔴 WHY NOT THE FASTEST CORE OF THE MACHINE, which was the first version.
# On x86 every clock reading in sysfs and /proc -- cpuinfo_avg_freq and
# /proc/cpuinfo's cpu MHz alike -- is arch_freq_get_on_cpu(): APERF/MPERF
# over the last tick, and for a core whose last tick is older than 20 ms,
# the POLICY's number (policy->cur, or cpu_khz where there is no cpufreq).
# On victus an idle core answers 1108930 kHz to the kHz, which is
# scaling_min_freq and no counter ratio; under acpi-cpufreq `performance' an
# idle core would answer the ceiling, and a run slowed by heat would have
# been reported at the ceiling -- the deduction back in through the reading
# written to replace it (fourth review).  A core running a vCPU ticks, so its
# reading is the counter's.  One source then serves, /proc/cpuinfo, which
# every kernel here has.

# uros_clock_now <pid> -- one sample, in MHz: the fastest core that one of
# <pid>'s threads is running on right now.  Prints nothing when none of its
# threads is running.
uros_clock_now() {
	_cpus=$(cat /proc/"$1"/task/*/stat 2>/dev/null |
		awk '{ sub(/^.*\) /, ""); if ($1 == "R") printf " %s", $37 }')
	[ -n "$_cpus" ] || return 0
	awk -v want="$_cpus " '
		/^processor/ { p = $3 }
		/^cpu MHz/ {
			v = int($4 + 0.5)
			if (index(want, " " p " ") && v > m) m = v
		}
		END { if (m) print m }' /proc/cpuinfo 2>/dev/null
}

# uros_clock_run_line <ceiling MHz, or empty> <seconds asked> [sample ...]
#
# The summary, from the samples alone, so that --self-test can drive it.
# 🔑 The ceiling is compared with the MEDIAN and not the maximum: one boosted
# second is boost, a run that sat above the ceiling is a ceiling that did not
# hold.  10% for the reason every other threshold in this file uses it.
# With no sample it says which of the two reasons: qemu gone before the first
# second, or seconds in which none of its threads was on a core.
uros_clock_run_line() {
	_ceil=$1
	_asked=$2
	shift 2
	if [ $# -eq 0 ]; then
		if [ "${_asked:-0}" -eq 0 ]; then
			echo "not measured: qemu had exited before the first second"
		else
			echo "not measured: in $_asked seconds none of qemu's threads was found running"
		fi
		return
	fi
	_sorted=$(printf '%s\n' "$@" | sort -n)
	_med=$(printf '%s\n' "$_sorted" | sed -n "$(( ($# + 1) / 2 ))p")
	_max=$(printf '%s\n' "$_sorted" | tail -n 1)
	_n="$# samples"
	[ $# -eq 1 ] && _n="1 sample"
	_line="median ${_med}MHz, max ${_max}MHz over $_n"
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
# 🔴 THE SPECIMENS ARE SAMPLED, not invented to please the classifier.  Row 2
# is this laptop, byte for byte out of sysfs on the day #564 was opened; row 4
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

	# The measured clock (#579): name|ceiling MHz|seconds asked|samples|the line it must read
	while IFS='|' read -r _name _ceil _asked _samples _want
	do
		[ -n "$_name" ] || continue
		case "$_name" in \#*) continue ;; esac
		_total=$(( _total + 1 ))
		# shellcheck disable=SC2086
		_got=$(uros_clock_run_line "$_ceil" "$_asked" $_samples)
		if [ "$_got" = "$_want" ]; then
			echo "  ok    $_name"
		else
			echo "  BAD   $_name"
			echo "        wanted <<$_want>>"
			echo "        read   <<$_got>>"
			_fails=$(( _fails + 1 ))
		fi
	done <<'EOF'
# sampled: the "clock samples:" line of a real run, ~/uros-tests/<log>
victus, powersave/balance_power, battery (579-misurato-thread-qemu-163424)|4280|10|2096 2096 2096 2096 2096 2096 2096 2096 1884 2171|median 2096MHz, max 2171MHz over 10 samples
victus, performance, battery (579-misurato-thread-performance-164122)|4280|7|3893 3893 3918 3918 3202 3943 3952|median 3918MHz, max 3952MHz over 7 samples
victus, ceiling 1400, battery (579-misurato-thread-soffitto-1400-164551)|1400|16|1397 1397 1397 1397 1397 1397 1397 1397 1397 1397 1397 1397 1397 1397 1397 1396|median 1397MHz, max 1397MHz over 16 samples
# probes
pavillion's two readings (idle 3992, six busy 3918) over its 3000 ceiling|3000|2|3918 3992|median 3918MHz, max 3992MHz over 2 samples -- ABOVE the 3000MHz ceiling
one second of boost is boost, not a ceiling that failed|1400|3|1397 1397 3700|median 1397MHz, max 3700MHz over 3 samples
exactly 10% over, by the median|1400|1|1540|median 1540MHz, max 1540MHz over 1 sample
just past 10%, by the median|1400|1|1541|median 1541MHz, max 1541MHz over 1 sample -- ABOVE the 1400MHz ceiling
an even count takes the lower middle|4280|4|1000 2000 3000 4000|median 2000MHz, max 4000MHz over 4 samples
sorted by value, not by text|4280|3|900 3268 1200|median 1200MHz, max 3268MHz over 3 samples
no ceiling, samples still summarised||2|3268 3269|median 3268MHz, max 3269MHz over 2 samples
qemu gone before the first second|1400|0||not measured: qemu had exited before the first second
asked, and no qemu thread was ever running|1400|7||not measured: in 7 seconds none of qemu's threads was found running
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

	if [ "$_fails" -gt 0 ]; then
		echo "run-conditions --self-test: $_fails of $_total read otherwise"
		exit 1
	fi
	echo "run-conditions --self-test: all $_total read as sampled"
	exit 0
	;;
esac
