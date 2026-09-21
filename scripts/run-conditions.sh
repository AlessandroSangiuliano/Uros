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
	if [ -n "$_capk" ]; then
		_cap="$(( _capk / 1000 ))MHz"
	else
		_cap="?"
	fi

	# ⚠️ AND BOOST GOES OVER THE POLICY CEILING.  Measured here, with
	# scaling_max_freq at 3000000 and boost enabled, the cores ran at
	# 3918-3992 MHz -- above the ceiling that had just been set.  So the
	# honest number with boost on is cpuinfo_max_freq (4000 MHz on this
	# machine), and reporting the policy ceiling would understate the
	# machine by a gigahertz.  Said from the measurement rather than from
	# what the driver is supposed to do.
	_boost=$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo "")
	_hwmaxk=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq \
	          2>/dev/null || echo "")
	_top="$_cap"
	if [ "$_boost" = 1 ] && [ -n "$_hwmaxk" ] && [ -n "$_capk" ] \
	   && [ "$_hwmaxk" -gt "$_capk" ]; then
		_top="$(( _hwmaxk / 1000 ))MHz (boost, over the ${_cap} ceiling)"
	fi

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

# uros_clock_policy — WHICH of the two clocks this machine will actually run
# at, decided from facts already sampled into _drv _gov _avail _mink _cap _top
# _mhz.  Sets _eff.
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
uros_clock_policy() {
	case "$_avail" in
	"performance powersave")	_who=driver ;;
	"")				_who=unknown ;;
	*)				_who=core ;;
	esac

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
		_eff="$_cap (driver $_drv, governor $_gov: unknown policy)" ;;
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
# is the machine of #544, whose eleven runs were taken as the high-clock arm
# under a governor that was pinning them.  A table written to match the code
# tests the code against itself.
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

	# name|_drv|_gov|_avail|_mink|_cap|_top|_mhz|the effective= it must read
	while IFS='|' read -r _name _drv _gov _avail _mink _cap _top _mhz _want
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
intel_pstate, powersave|intel_pstate|powersave|performance powersave|400000|3900MHz|3900MHz|1200|3900MHz (the driver scales the range, powersave is its bias)
amd-pstate-epp, powersave (victus, #564)|amd-pstate-epp|powersave|performance powersave|1108930|4280MHz|4280MHz|3703|4280MHz (the driver scales the range, powersave is its bias)
intel_pstate, performance|intel_pstate|performance|performance powersave|400000|3900MHz|3900MHz|3900|3900MHz
# a PASSIVE one: the core runs a governor and `powersave' is static
acpi-cpufreq, powersave pinned (#544)|acpi-cpufreq|powersave|conservative ondemand userspace powersave performance schedutil |1400000|3000MHz|3992MHz (boost, over the 3000MHz ceiling)|1397|1400MHz (governor pins it to the floor)
acpi-cpufreq, ondemand|acpi-cpufreq|ondemand|conservative ondemand userspace powersave performance schedutil |1400000|3000MHz|3992MHz (boost, over the 3000MHz ceiling)|2100|3992MHz (boost, over the 3000MHz ceiling)
amd-pstate passive, powersave|amd-pstate|powersave|conservative ondemand userspace powersave performance schedutil |1108930|4280MHz|4280MHz|1108|1108MHz (governor pins it to the floor)
# 🔑 the row that refutes repairing this by name: intel_pstate in passive mode
# calls itself intel_cpufreq, and there `powersave' really does pin
intel_cpufreq, powersave|intel_cpufreq|powersave|conservative ondemand userspace powersave performance schedutil |800000|4000MHz|4000MHz|798|800MHz (governor pins it to the floor)
# the literal and not a prefix of it
a list that starts with those two words|acpi-cpufreq|powersave|performance powersave schedutil |1400000|3000MHz|3000MHz|1397|1400MHz (governor pins it to the floor)
# the floor is a claim, and the field beside it can refute the claim
the processor stands above its own floor|acpi-cpufreq|powersave|conservative ondemand powersave performance schedutil |1108930|4280MHz|4280MHz|3703|unsettled: floor 1108MHz, processor at 3703MHz (#564)
# and what is not known is said instead of guessed
powersave with no floor to read|acpi-cpufreq|powersave|conservative ondemand powersave performance schedutil ||3000MHz|3000MHz|1400|?
a governor with no policy of its own|acpi-cpufreq|userspace|conservative ondemand userspace powersave performance schedutil |1400000|3000MHz|3000MHz|1400|3000MHz (driver acpi-cpufreq, governor userspace: unknown policy)
a machine that will not say|?|?|||?|?|?|? (driver ?, governor ?: unknown policy)
EOF

	if [ "$_fails" -gt 0 ]; then
		echo "run-conditions --self-test: $_fails of $_total read otherwise"
		exit 1
	fi
	echo "run-conditions --self-test: all $_total read as sampled"
	exit 0
	;;
esac
