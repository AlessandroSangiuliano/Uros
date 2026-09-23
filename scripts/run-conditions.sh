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
	_boost=$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo "")

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
#   known from that source and occur on no machine here: intel_pstate
#   without HWP lets turbo pass a ceiling set inside the turbo range, and
#   amd-pstate-epp before 6.8 never read scaling_max_freq at all.
# - Wherever it runs, the ceiling is a claim that cpu= can refute; see below
#   for how coarse that is.
#
# 🔑 The measured clock during the run is what would settle all of it, and
# this line does not take one.
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
# probes: no boost to speak of -- switched off, or no boost file at all
acpi-cpufreq, boost off|acpi-cpufreq|ondemand|conservative ondemand userspace powersave performance schedutil |1400000|3000000|0|2990|3000MHz
acpi-cpufreq, no boost file|acpi-cpufreq|ondemand|conservative ondemand userspace powersave performance schedutil |1400000|3000000||2990|3000MHz
# probes: the ceiling is a claim wherever it is read, and 10% is its margin
the processor stands above a driver's ceiling|amd-pstate-epp|powersave|performance powersave|1108930|1400000|1|3703|unsettled: ceiling 1400MHz, processor at 3703MHz (#579) (the driver scales the range, powersave is its bias)
above the ceiling with boost off|acpi-cpufreq|performance|conservative ondemand userspace powersave performance schedutil |1400000|3000000|0|3993|unsettled: ceiling 3000MHz, processor at 3993MHz (#579)
above the ceiling under userspace|acpi-cpufreq|userspace|conservative ondemand userspace powersave performance schedutil |1400000|3000000|1|3993|unsettled: ceiling 3000MHz, processor at 3993MHz (#579) (driver acpi-cpufreq, governor userspace: unknown policy)
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

	if [ "$_fails" -gt 0 ]; then
		echo "run-conditions --self-test: $_fails of $_total read otherwise"
		exit 1
	fi
	echo "run-conditions --self-test: all $_total read as sampled"
	exit 0
	;;
esac
