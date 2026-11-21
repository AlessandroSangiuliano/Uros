#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# harness-lifecycle-check.sh -- what run-x86_64.sh and the tools that share
# its lock leave behind, and what they take from others (#618).
#
# run-x86_64.sh killed every qemu on the machine by name before it booted, and
# that hid what each of them left: an interrupted run its qemu, a tool sent
# TERM its qemu and the lock.  Nothing is killed by name now, so each has to
# give back what it took, and only that.  This asks them, one check a line:
#
#   bystander  a qemu started by hand survives a run, and the run's conditions
#              name it, at the start and at the end
#   signals    a run sent TERM, INT and HUP once its qemu is up ends by that
#              signal, and leaves neither its qemu nor the lock
#   lockover   a run whose lock was removed and taken again by someone else
#              while it ran leaves that lock where it is, and so does a tool
#   refuse     with the lock held, run-x86_64.sh and every tool that shares the
#              lock refuse, and none of them has made a disk first
#   tools      each tool that shares the lock, sent TERM once its qemu is up,
#              leaves neither its qemu nor the lock
#   orphan     a run killed with SIGKILL, which no trap can answer, leaves its
#              qemu, and the next run names it in its conditions
#   killed     a tool killed with SIGKILL takes its qemu with it
#              (PR_SET_PDEATHSIG); the lock it cannot give back is said
#   unitstop   each of them, run in a transient systemd user unit under a
#              script with no trap, and the unit stopped with its qemu up --
#              which signals what is left of the unit twice -- leaves neither
#              its qemu nor the lock, and run-x86_64.sh says it was stopped
#              (#801)
#   mtstop     merge-test-x86_64.sh, sent TERM during a boot, says that boot
#              was stopped, says where it stopped, prints its total and ends
#              by the signal, and leaves neither the boot's qemu nor the lock:
#              what ci/job.sh stops it with at a campaign job's time (#951)
#   abstop     ablate.sh, sent TERM during a boot, writes that boot's row as
#              stopped, says where it stopped and what its boots said, removes
#              its worktrees and ends by the signal, and leaves neither the
#              boot's qemu nor the lock: an A/B or a sweep stopped so (#951)
#   jobstop    ci/job.sh, sent TERM during a boot of its merge test's, writes
#              in job.txt where it stopped and the boot it stopped, that boot's
#              row and logs, and exits 3, leaving neither the boot's qemu nor
#              the lock; and ci/pull-results.sh --job says it stopped short by
#              that SIGTERM, its boot counted as stopped and nothing more: a
#              cancelled campaign run (#951)
#   jobfail    ci/job.sh, whose configure fails, says why in job.txt, in its
#              "failed:" line, and exits 3, and ci/pull-results.sh --job says
#              it FAILED, and why, not that it was cut off: a campaign job
#              whose build fails, as the Ubuntu jobs of 18/11 (#951); and
#              job.txt says the run's kind CAMPAIGN_KIND gives it (#961)
#   jobnokvm   ci/pull-results.sh --job says a job of a run on GitHub that
#              could not open /dev/kvm as a fault, on a line of its own, and
#              nothing of the kind of one that opened it, nor of a job run
#              here: the nightly's jobs of 11/11 to 18/11 (#961)
#
# Each line ends PASS or WRONG; a check that could not be posed says NOT
# ASKED.  The boots are entry 6's, which ends as soon as ring 3 has answered --
# under KVM before a signal could come, so a run to be signalled or killed
# boots with its guest paused (#801).  mtstop's boot is the merge test's own,
# entry 1's, which it gives no -S.  jobfail boots nothing: its configure fails
# before the build.
#
# Usage:  harness-lifecycle-check.sh [--kvm] [--only CHECK[,CHECK...]] [--out DIR]
# Exit:   0 every check PASS · 1 a check WRONG · 2 it could not be run (the
#         lock already held, no build)
#
# The build is UROS_BUILD_DIR's, or uros/build-x86_64, as run-x86_64.sh's.

set -u
set -m			# its own process group for each job: a job sent INT gets it

# #951: a signal ignored when a shell starts can be neither trapped nor reset
# by it, and every run this script starts inherits it.  Started under nohup,
# or as an asynchronous command of a shell without job control, this script
# had SIGHUP and SIGINT ignored, run-x86_64.sh could not stop at them, and
# the signals check said WRONG twice of a run that was never asked (OMEGA,
# 18/11, the union of #957 and #951).  So the script starts itself again
# with them at their defaults.
if [ -z "${LIFECYCLE_SIGNALS_GIVEN_BACK:-}" ] &&
   ign=$(sed -n 's/^SigIgn:[[:space:]]*//p' /proc/$$/status) && [ -n "$ign" ] &&
   (( 0x$ign & 0x3 )); then
	LIFECYCLE_SIGNALS_GIVEN_BACK=1 exec env --default-signal=HUP,INT "$0" "$@"
fi

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${UROS_BUILD_DIR:-$REPO/uros/build-x86_64}
LOCK=/tmp/uros-x86_64-run.lock
ENTRY=6
KVM=()
ONLY=bystander,signals,lockover,refuse,tools,orphan,killed,unitstop,mtstop,abstop,jobstop,jobfail,jobnokvm
OUT=$HOME/uros-tests/lifecycle/$(date +%Y%m%d-%H%M%S)

while [ $# -gt 0 ]; do
	case $1 in
	--kvm)	KVM=(--kvm); shift ;;
	--only)	ONLY=$2; shift 2 ;;
	--out)	OUT=$2; shift 2 ;;
	*)	echo "usage: $0 [--kvm] [--only CHECK[,CHECK...]] [--out DIR]" >&2; exit 2 ;;
	esac
done

[ -x "$BUILD/export/uros/boot/mach_kernel" ] || [ -f "$BUILD/export/uros/boot/mach_kernel" ] ||
	{ echo "lifecycle: no kernel in $BUILD" >&2; exit 2; }
[ -d "$LOCK" ] && { echo "lifecycle: $LOCK is held: another run is in flight, refusing" >&2; exit 2; }
mkdir -p "$OUT"
export UROS_BUILD_DIR=$BUILD

pass=0 wrong=0 unasked=0
say() {		# CHECK, what was seen, verdict
	echo "lifecycle: $1: $2 -- $3"
	case $3 in
	PASS)	pass=$((pass + 1)) ;;
	WRONG)	wrong=$((wrong + 1)) ;;
	*)	unasked=$((unasked + 1)) ;;
	esac
}
wanted() {
	case ",$ONLY," in *",$1,"*) return 0 ;; esac
	return 1
}

# The qemu-system children of a process, from /proc, as run-conditions.sh
# reads processes: no pgrep needed where the campaign runs.
qemu_children() {
	local d stat
	for d in /proc/[0-9]*; do
		case $(cat "$d/comm" 2>/dev/null) in qemu-system*) ;; *) continue ;; esac
		stat=$(cat "$d/stat" 2>/dev/null) || continue
		# shellcheck disable=SC2086
		set -- ${stat##*) }
		[ "${2:-}" = "$PARENT" ] && echo "${d#/proc/}"
	done
	return 0
}
# The first child of PARENT whose command name is NAME, waited for: the merge
# test's boot is its run-x86_64.sh (#951).
child_named() {	# PARENT NAME [seconds]
	local parent=$1 name=$2 secs=${3:-90} d stat i
	for i in $(seq 1 $(( secs * 10 ))); do
		for d in /proc/[0-9]*; do
			[ "$(cat "$d/comm" 2>/dev/null)" = "$name" ] || continue
			stat=$(cat "$d/stat" 2>/dev/null) || continue
			# shellcheck disable=SC2086
			set -- ${stat##*) }
			[ "${2:-}" = "$parent" ] && { echo "${d#/proc/}"; return 0; }
		done
		kill -0 "$parent" 2>/dev/null || return 1
		sleep 0.1
	done
	return 1
}
first_qemu_of() {	# PID [seconds]: the first qemu child of PID, waited for
	local i
	PARENT=$1
	for i in $(seq 1 $(( ${2:-90} * 10 ))); do
		q=$(qemu_children | head -n 1)
		[ -n "$q" ] && { echo "$q"; return 0; }
		kill -0 "$1" 2>/dev/null || return 1
		sleep 0.1
	done
	return 1
}
ended_within() {	# PID SECONDS: whether PID has gone within that long
	local i
	for i in $(seq 1 $(( $2 * 10 ))); do
		kill -0 "$1" 2>/dev/null || return 0
		sleep 0.1
	done
	return 1
}
# Whatever this check started and is still there, stopped by PID; the lock
# only if this script is the one holding it.
STARTED=()
HOLDING=""
cleanup() {
	local p
	for p in "${STARTED[@]}"; do
		kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null
	done
	[ -n "$HOLDING" ] && rmdir "$LOCK" 2>/dev/null
	return 0
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# What a run or a tool left after it ended, said and then cleared away: its
# qemu, killed by PID, and the lock, which nobody else can be holding while
# this script runs its checks one at a time.
leftovers() {	# QEMU-PID
	if kill -0 "$1" 2>/dev/null; then
		printf '; its qemu (pid %s) is STILL RUNNING' "$1"
		kill -KILL "$1" 2>/dev/null
	fi
	if [ -d "$LOCK" ] && [ -z "$HOLDING" ]; then
		printf '; the lock is STILL THERE'
		rmdir "$LOCK"
	fi
	return 0
}

run_harness() {	# NAME [QEMU-ARGS...]: run-x86_64.sh in the background, its log under OUT
	local name=$1
	shift
	UROS_X86_64_LOG="$OUT/$name.log" "$REPO/scripts/run-x86_64.sh" "${KVM[@]}" \
		--entry "$ENTRY" 120 "$@" > "$OUT/$name.out" 2>&1 &
	RUN=$!
	STARTED+=("$RUN")
}
# 🔴 AND HOW IT ENDED, WHICH `wait' CANNOT SAY (#801).  A run killed by signal N
# and a run that exited with 128+N both read 128+N to a shell's wait, and since
# the wait for qemu waits with `wait' (run-x86_64.sh, #801), a run whose trap
# no longer ends it by the signal -- ablations/618-trap-goes-on.patch -- mostly
# exits with that same number when set -e meets the interrupted wait: the check
# found it in 1 boot of 4.  So a run to be signalled is started under
# ended-by.py, which is its parent with the run's own pid and writes "signal N"
# or "exit N" into NAME.ended.  RUN is the run's pid, read from NAME.ended.pid;
# JOB is the watcher's, the one this shell waits for.
run_harness_watched() {	# NAME [QEMU-ARGS...]
	local name=$1 i
	shift
	rm -f "$OUT/$name.ended" "$OUT/$name.ended.pid"
	UROS_X86_64_LOG="$OUT/$name.log" python3 -I "$REPO/scripts/ended-by.py" \
		"$OUT/$name.ended" -- "$REPO/scripts/run-x86_64.sh" "${KVM[@]}" \
		--entry "$ENTRY" 120 "$@" > "$OUT/$name.out" 2>&1 &
	JOB=$!
	RUN=""
	for i in $(seq 1 100); do
		[ -s "$OUT/$name.ended.pid" ] && break
		sleep 0.05
	done
	RUN=$(cat "$OUT/$name.ended.pid" 2>/dev/null)
	STARTED+=(${RUN:+"$RUN"} "$JOB")
}
# 🔴 A RUN THAT IS TO BE SIGNALLED OR KILLED BOOTS PAUSED (#801).  Under KVM
# entry 6 ends within the two seconds a check waits after its qemu appears:
# the run stopped that qemu itself, at its own end, and only then had the
# signal -- so a trap that left the qemu running passed, 2 boots of 4 for
# ablations/618-trap-leaves-qemu, both under KVM.  A guest held at qemu's -S
# never ends: the qemu is there until something kills it, and the run is in
# its boot until its 120 s of silence run out.
PAUSED=(-S)
# And before each signal, the qemu is asked for: gone already, it is a check
# that cannot be posed, said as NOT ASKED -- the tools take no qemu flags.
qemu_gone_before() {	# QEMU-PID CHECK: true, after saying so, if it has gone
	kill -0 "$1" 2>/dev/null && return 1
	say "$2" "its qemu (pid $1) had ended before the signal, so what the signal leaves could not be asked" "NOT ASKED"
	return 0
}
images() {
	stat -c '%i:%Y' "$BUILD/disk-x86_64.img" "$BUILD/disk-x86_64-ahci.img" \
		"$BUILD/disk-x86_64-ahci2.img" 2>/dev/null | tr '\n' ' '
}

# ── bystander ─────────────────────────────────────────────────────────
if wanted bystander; then
	qemu-system-x86_64 -S -display none -serial null -monitor none \
		-name uros-lifecycle-bystander > "$OUT/bystander.out" 2>&1 &
	B=$!
	STARTED+=("$B")
	sleep 1
	if ! kill -0 "$B" 2>/dev/null; then
		say bystander "the qemu started by hand did not start: $(head -n 1 "$OUT/bystander.out")" "NOT ASKED"
	else
		run_harness bystander
		wait "$RUN"
		rc=$?
		if ! kill -0 "$B" 2>/dev/null; then
			say bystander "the qemu started by hand (pid $B) is gone after a run that exited $rc" WRONG
		elif ! grep -q "^  other qemu:   at the start and at the end, pid $B: " "$OUT/bystander.log"; then
			say bystander "the qemu started by hand (pid $B) survived, and the run's conditions do not name it at the start and the end: $(grep '^  other qemu:' "$OUT/bystander.log" | head -n 2 | tr '\n' ' ')" WRONG
		else
			say bystander "the qemu started by hand (pid $B) survived a run that exited $rc, which named it at the start and at the end" PASS
		fi
		kill "$B" 2>/dev/null
		wait "$B" 2>/dev/null
	fi
fi

# ── signals ───────────────────────────────────────────────────────────
if wanted signals; then
	for sig in TERM INT HUP; do
		run_harness_watched "signal-$sig" "${PAUSED[@]}"
		if [ -z "$RUN" ] || ! q=$(first_qemu_of "$RUN"); then
			wait "$JOB"
			say "signal $sig" "the run never started its qemu (it ended by $(cat "$OUT/signal-$sig.ended" 2>/dev/null || echo 'nothing said'), $OUT/signal-$sig.out)" "NOT ASKED"
			continue
		fi
		sleep 2
		if qemu_gone_before "$q" "signal $sig"; then
			ended_within "$RUN" 30 || kill -KILL "$RUN" 2>/dev/null
			wait "$JOB" 2>/dev/null
			continue
		fi
		kill -s "$sig" "$RUN"
		if ! ended_within "$RUN" 30; then
			kill -KILL "$RUN" 2>/dev/null
			wait "$JOB" 2>/dev/null
			say "signal $sig" "the run WENT ON for 30 s after SIG$sig$(leftovers "$q")" WRONG
			continue
		fi
		wait "$JOB"
		ended=$(cat "$OUT/signal-$sig.ended" 2>/dev/null)
		want="signal $(kill -l "$sig")"
		left=$(leftovers "$q")
		[ "$ended" = "$want" ] || left="$left; it ended by ${ended:-nothing said}, not by $want: it did not end by the signal"
		if [ -n "$left" ]; then
			say "signal $sig" "sent SIG$sig with its qemu (pid $q) up${left}" WRONG
		else
			say "signal $sig" "sent SIG$sig with its qemu (pid $q) up, the run ended by it, its qemu gone and the lock given back" PASS
		fi
	done
fi

# ── lockover ──────────────────────────────────────────────────────────
if wanted lockover; then
	run_harness lockover "${PAUSED[@]}"
	if ! q=$(first_qemu_of "$RUN"); then
		wait "$RUN"
		say lockover "the run never started its qemu (exit $?)" "NOT ASKED"
	else
		# Removed by hand, and taken again: by this script, as another run
		# would take it.
		rmdir "$LOCK" && mkdir "$LOCK" && HOLDING=1
		kill -s TERM "$RUN"
		ended_within "$RUN" 30 || kill -KILL "$RUN" 2>/dev/null
		wait "$RUN"
		rc=$?
		if [ -z "$HOLDING" ]; then
			say lockover "the lock could not be taken over while the run held it" "NOT ASKED"
		elif [ ! -d "$LOCK" ]; then
			say lockover "a run whose lock was taken over while it ran, sent SIGTERM (exit $rc), REMOVED the lock it no longer held" WRONG
		else
			say lockover "a run whose lock was taken over while it ran, sent SIGTERM (exit $rc), left that lock where it was" PASS
		fi
		kill -0 "$q" 2>/dev/null && kill -KILL "$q" 2>/dev/null
		[ -n "$HOLDING" ] && rmdir "$LOCK" 2>/dev/null
		HOLDING=""
	fi
	# And a tool, char-roundtrip.py, for the four that give the lock back
	# through run_lock.py.
	python3 "$REPO/scripts/char-roundtrip.py" --entry "$ENTRY" --build "$BUILD" \
		"${KVM[@]}" > "$OUT/lockover-char-roundtrip.out" 2>&1 &
	P=$!
	STARTED+=("$P")
	if ! q=$(first_qemu_of "$P"); then
		wait "$P"
		say "lockover char-roundtrip.py" "it never started its qemu (exit $?)" "NOT ASKED"
	else
		rmdir "$LOCK" && mkdir "$LOCK" && HOLDING=1
		kill -s TERM "$P"
		ended_within "$P" 30 || kill -KILL "$P" 2>/dev/null
		wait "$P"
		rc=$?
		if [ -z "$HOLDING" ]; then
			say "lockover char-roundtrip.py" "the lock could not be taken over while the tool held it" "NOT ASKED"
		elif [ ! -d "$LOCK" ]; then
			say "lockover char-roundtrip.py" "a tool whose lock was taken over while it ran, sent SIGTERM (exit $rc), REMOVED the lock it no longer held" WRONG
		else
			say "lockover char-roundtrip.py" "a tool whose lock was taken over while it ran, sent SIGTERM (exit $rc), left that lock where it was" PASS
		fi
		kill -0 "$q" 2>/dev/null && kill -KILL "$q" 2>/dev/null
		[ -n "$HOLDING" ] && rmdir "$LOCK" 2>/dev/null
		HOLDING=""
	fi
fi

# ── refuse ────────────────────────────────────────────────────────────
TOOLS=("scripts/run-x86_64.sh --entry $ENTRY 120"
	"scripts/fbcons-readback.py --entry $ENTRY --build $BUILD"
	"scripts/char-roundtrip.py --entry $ENTRY --build $BUILD"
	"scripts/ramoops-readback.py --entry $ENTRY --build $BUILD --out $OUT"
	"scripts/rip-sample.py --entry $ENTRY --build $BUILD"
	"scripts/uart-stall.py --entry $ENTRY --build $BUILD")
if wanted refuse; then
	mkdir "$LOCK" && HOLDING=1
	for t in "${TOOLS[@]}"; do
		name=$(basename "${t%% *}")
		before=$(images)
		# shellcheck disable=SC2086
		UROS_X86_64_LOG="$OUT/refuse-$name.log" timeout 120 "$REPO"/$t \
			> "$OUT/refuse-$name.out" 2>&1
		rc=$?
		after=$(images)
		if [ "$rc" != 2 ]; then
			say "refuse $name" "with the lock held it exited $rc, not 2 ($OUT/refuse-$name.out)" WRONG
		elif [ "$before" != "$after" ]; then
			say "refuse $name" "with the lock held it refused, and the disk images CHANGED under it (inode:mtime $before -> $after)" WRONG
		else
			say "refuse $name" "with the lock held it refused, the disk images untouched" PASS
		fi
	done
	rmdir "$LOCK" && HOLDING=""
fi

# ── tools ─────────────────────────────────────────────────────────────
if wanted tools; then
	for t in "${TOOLS[@]:1}"; do
		name=$(basename "${t%% *}")
		# shellcheck disable=SC2086
		python3 "$REPO"/$t "${KVM[@]}" > "$OUT/tool-$name.out" 2>&1 &
		P=$!
		STARTED+=("$P")
		if ! q=$(first_qemu_of "$P"); then
			wait "$P"
			say "tool $name" "it never started its qemu (exit $?, $OUT/tool-$name.out)" "NOT ASKED"
			continue
		fi
		sleep 2
		if qemu_gone_before "$q" "tool $name"; then
			ended_within "$P" 30 || kill -KILL "$P" 2>/dev/null
			wait "$P" 2>/dev/null
			continue
		fi
		kill -s TERM "$P"
		went_on=""
		ended_within "$P" 30 || { kill -KILL "$P" 2>/dev/null; went_on="; it WENT ON for 30 s"; }
		wait "$P"
		rc=$?
		sleep 0.5
		left="$went_on$(leftovers "$q")"
		if [ -n "$left" ]; then
			say "tool $name" "sent SIGTERM with its qemu (pid $q) up, it exited $rc$left" WRONG
		else
			say "tool $name" "sent SIGTERM with its qemu (pid $q) up, it exited $rc, its qemu gone and the lock given back" PASS
		fi
	done
fi

# ── orphan ────────────────────────────────────────────────────────────
if wanted orphan; then
	run_harness orphan-killed "${PAUSED[@]}"
	if ! q=$(first_qemu_of "$RUN"); then
		wait "$RUN"
		say orphan "the run never started its qemu (exit $?)" "NOT ASKED"
	else
		sleep 2
		kill -KILL "$RUN"
		wait "$RUN" 2>/dev/null
		# The lock dies with no one to give it back: cleared here, as by hand.
		[ -d "$LOCK" ] && rmdir "$LOCK"
		run_harness orphan-next
		wait "$RUN"
		rc=$?
		if ! kill -0 "$q" 2>/dev/null; then
			say orphan "the qemu (pid $q) of a run killed with SIGKILL was gone before the next run named it" "NOT ASKED"
		elif grep -q "^  other qemu:   at the start and at the end, pid $q: " "$OUT/orphan-next.log"; then
			say orphan "the qemu (pid $q) a run killed with SIGKILL left is named by the next run (exit $rc), at the start and at the end" PASS
		else
			say orphan "the qemu (pid $q) a run killed with SIGKILL left is NOT NAMED by the next run (exit $rc): $(grep '^  other qemu:' "$OUT/orphan-next.log" | head -n 2 | tr '\n' ' ')" WRONG
		fi
		kill -KILL "$q" 2>/dev/null
	fi
fi

# ── killed ────────────────────────────────────────────────────────────
if wanted killed; then
	t=${TOOLS[2]}		# char-roundtrip.py, one of the four run_lock.py serves
	name=$(basename "${t%% *}")
	# shellcheck disable=SC2086
	python3 "$REPO"/$t "${KVM[@]}" > "$OUT/killed-$name.out" 2>&1 &
	P=$!
	STARTED+=("$P")
	if ! q=$(first_qemu_of "$P"); then
		wait "$P"
		say "killed $name" "it never started its qemu (exit $?)" "NOT ASKED"
	else
		sleep 2
		kill -KILL "$P"
		wait "$P" 2>/dev/null
		sleep 0.5
		lock="the lock it could not give back is there"
		[ -d "$LOCK" ] || lock="and the lock is GONE, which nothing killed with SIGKILL can have done"
		[ -d "$LOCK" ] && rmdir "$LOCK"
		if kill -0 "$q" 2>/dev/null; then
			say "killed $name" "killed with SIGKILL, its qemu (pid $q) is STILL RUNNING; $lock" WRONG
			kill -KILL "$q" 2>/dev/null
		else
			say "killed $name" "killed with SIGKILL, its qemu (pid $q) went with it; $lock" PASS
		fi
	fi
fi

# ── unitstop ──────────────────────────────────────────────────────────
# #801: each stopped the way a campaign started with systemd-run is -- a
# script with no trap runs it in a transient user unit, so the unit's main
# process dies on the first SIGTERM and systemd signals what is left of the
# unit again.  On 31 October run-x86_64.sh's trap was in the middle of giving
# its lock back when the second one came, and the next run refused to start
# until the directory was removed by hand.
unit_qemu() {	# UNIT: the qemu in the unit's control group
	local cg p
	cg=$(systemctl --user show -p ControlGroup --value "$1" 2>/dev/null)
	[ -n "$cg" ] || return 1
	for p in $(cat "/sys/fs/cgroup$cg/cgroup.procs" 2>/dev/null); do
		case $(cat "/proc/$p/comm" 2>/dev/null) in
		qemu-system*)	echo "$p"; return 0 ;;
		esac
	done
	return 1
}
if wanted unitstop; then
	if ! systemctl --user show-environment > /dev/null 2>&1; then
		say unitstop "no systemd user manager to start a unit under" "NOT ASKED"
	else
		for t in "${TOOLS[@]}"; do
			name=$(basename "${t%% *}")
			case $name in
			*.py)	# shellcheck disable=SC2086
				cmd=(python3 "$REPO"/$t "${KVM[@]}") ;;
			*)	cmd=("$REPO/scripts/run-x86_64.sh" "${KVM[@]}" --entry "$ENTRY" 120 "${PAUSED[@]}") ;;
			esac
			U=uros-lifecycle-unitstop-$$-$name
			# Two commands, so that bash cannot exec the tool in its own
			# place: the wrapper stays the unit's main process.
			systemd-run --user --quiet --collect --unit="$U" \
				--setenv=UROS_BUILD_DIR="$BUILD" \
				--setenv=UROS_X86_64_LOG="$OUT/unitstop-$name.log" \
				--setenv=LIFECYCLE_OUT="$OUT/unitstop-$name.out" \
				/bin/bash -c '"$@" > "$LIFECYCLE_OUT" 2>&1; echo "lifecycle: it ended on its own, status $?" >> "$LIFECYCLE_OUT"' \
				bash "${cmd[@]}"
			q=""
			for i in $(seq 1 900); do
				q=$(unit_qemu "$U") && break
				systemctl --user is-active --quiet "$U" || break
				sleep 0.1
			done
			if [ -z "$q" ]; then
				systemctl --user stop "$U" 2>/dev/null
				say "unitstop $name" "it never started its qemu under a unit ($OUT/unitstop-$name.out)" "NOT ASKED"
				continue
			fi
			sleep 2
			if qemu_gone_before "$q" "unitstop $name"; then
				systemctl --user stop "$U" 2>/dev/null
				continue
			fi
			systemctl --user stop "$U"
			for i in $(seq 1 300); do
				systemctl --user is-active --quiet "$U" || break
				sleep 0.1
			done
			sleep 0.5
			left=$(leftovers "$q")
			if [ "$name" = run-x86_64.sh ] &&
			   ! grep -q "^run-x86_64.sh: stopped by SIG" "$OUT/unitstop-$name.out"; then
				left="$left; it never said it was stopped: its output ends $(tail -n 1 "$OUT/unitstop-$name.out")"
			fi
			if [ -n "$left" ]; then
				say "unitstop $name" "its unit stopped with its qemu (pid $q) up$left" WRONG
			else
				say "unitstop $name" "its unit stopped with its qemu (pid $q) up, its qemu gone and the lock given back" PASS
			fi
		done
	fi
fi

# ── mtstop ────────────────────────────────────────────────────────────
# #951: the merge test, sent SIGTERM during a boot, passes it on to the boot,
# says so of that boot in a verdict line whose result is "stopped", says where
# it stopped, prints its total and ends by the signal, the boot's qemu gone
# and the lock given back.  ci/job.sh stops it so at a campaign job's time,
# because GitHub's own limit stops only the step's shell.  The merge test
# gives run-x86_64.sh only its list's options, so its boot cannot be held at
# -S: it boots entry 1, its shortest line judged by the harness, and the
# signal goes as soon as that boot's qemu is there; a boot that ended first
# makes the check one that could not be posed.
if wanted mtstop; then
	acc=tcg
	[ ${#KVM[@]} = 0 ] || acc=kvm
	rm -rf "$OUT/mtstop" "$OUT/mtstop.ended" "$OUT/mtstop.ended.pid"
	python3 -I "$REPO/scripts/ended-by.py" "$OUT/mtstop.ended" -- \
		"$REPO/scripts/merge-test-x86_64.sh" -a "$acc" -e 1 -o "$OUT/mtstop" \
		> "$OUT/mtstop.out" 2>&1 &
	JOB=$!
	for i in $(seq 1 100); do
		[ -s "$OUT/mtstop.ended.pid" ] && break
		sleep 0.05
	done
	MT=$(cat "$OUT/mtstop.ended.pid" 2>/dev/null)
	STARTED+=(${MT:+"$MT"} "$JOB")
	# Its boot, after the lists and the patches are asked: run-x86_64.sh, the
	# merge test's child, and that run's qemu.
	r="" q=""
	[ -n "$MT" ] && r=$(child_named "$MT" run-x86_64.sh 300) && q=$(first_qemu_of "$r")
	if [ -z "$q" ]; then
		ended_within "$JOB" 300 || kill -KILL "$MT" 2>/dev/null
		wait "$JOB" 2>/dev/null
		say mtstop "the merge test never started its boot's qemu (it ended by $(cat "$OUT/mtstop.ended" 2>/dev/null || echo 'nothing said'), $OUT/mtstop.out)" "NOT ASKED"
	elif qemu_gone_before "$q" mtstop; then
		ended_within "$JOB" 300 || kill -KILL "$MT" 2>/dev/null
		wait "$JOB" 2>/dev/null
	else
		kill -s TERM "$MT"
		if ! ended_within "$MT" 30; then
			kill -KILL "$MT" 2>/dev/null
			wait "$JOB" 2>/dev/null
			say mtstop "the merge test WENT ON for 30 s after SIGTERM$(leftovers "$q")" WRONG
		else
			wait "$JOB"
			ended=$(cat "$OUT/mtstop.ended" 2>/dev/null)
			left=$(leftovers "$q")
			grep -aqE "^merge-test: entry 1 $acc -smp [0-9]+ -- stopped \(" "$OUT/mtstop.out" ||
				left="$left; no line says its boot was stopped"
			grep -aq '^merge-test: stopped by SIGTERM at boot 1 of 1$' "$OUT/mtstop.out" ||
				left="$left; it did not say it stopped at boot 1 of 1: $(grep -a '^merge-test: stopped' "$OUT/mtstop.out" || echo 'it said nothing of it')"
			grep -aqE '^merge-test: [0-9]+ wrong$' "$OUT/mtstop.out" ||
				left="$left; it printed no total"
			[ "$ended" = "signal $(kill -l TERM)" ] ||
				left="$left; it ended by ${ended:-nothing said}, not by the signal"
			if [ -n "$left" ]; then
				say mtstop "sent SIGTERM with its boot's qemu (pid $q) up$left" WRONG
			else
				say mtstop "sent SIGTERM with its boot's qemu (pid $q) up, it said that boot was stopped and where it stopped, printed its total and ended by the signal, the qemu gone and the lock given back" PASS
			fi
		fi
	fi
fi

# ── abstop ────────────────────────────────────────────────────────────
# #951: ablate.sh, sent SIGTERM during a boot, passes it on to the boot,
# writes that boot's row with the verdict "stopped", says where it stopped and
# what its boots said, removes its worktrees and ends by the signal, the
# boot's qemu gone and the lock given back -- how ci/job.sh stops an A/B or a
# sweep at a campaign job's time.  Its patch is a probe written here, which
# changes a line of ablations/README.md and so nothing that is built, and its
# Run: line boots entry 6 paused: the boot is under way until the signal
# comes, or its 120 s of silence run out.  One arm, two boots, so that the
# run is stopped at the first of two.
if wanted abstop; then
	acc=tcg
	[ ${#KVM[@]} = 0 ] || acc=kvm
	mkdir -p "$OUT/abstop-dir"
	# The probe: ablations/README.md with one line more, its paths the tree's.
	cp "$REPO/ablations/README.md" "$OUT/abstop-readme.new"
	echo "abstop probe of $(date '+%F %T')" >> "$OUT/abstop-readme.new"
	{
		printf '%s\n' 'Kind: ablation' 'Issue: #951' \
			"Takes out: nothing -- harness-lifecycle-check.sh's probe for abstop, which stops this run in its first boot" \
			"Run: --entry $ENTRY 120 ${PAUSED[*]}" \
			'Expect: a line no boot prints, since the boot is stopped before it ends' ''
		diff -u --label a/ablations/README.md --label b/ablations/README.md \
			"$REPO/ablations/README.md" "$OUT/abstop-readme.new" |
			sed '1i diff --git a/ablations/README.md b/ablations/README.md'
	} > "$OUT/abstop.patch"
	rm -f "$OUT/abstop-readme.new" "$OUT/abstop.ended" "$OUT/abstop.ended.pid"
	UROS_ABLATE_DIR="$OUT/abstop-dir" UROS_ABLATE_REF_BUILD="$BUILD" \
		python3 -I "$REPO/scripts/ended-by.py" "$OUT/abstop.ended" -- \
		"$REPO/scripts/ablate.sh" --boots 2 --accel "$acc" "$OUT/abstop.patch" \
		> "$OUT/abstop.out" 2>&1 &
	JOB=$!
	for i in $(seq 1 100); do
		[ -s "$OUT/abstop.ended.pid" ] && break
		sleep 0.05
	done
	AB=$(cat "$OUT/abstop.ended.pid" 2>/dev/null)
	STARTED+=(${AB:+"$AB"} "$JOB")
	# Its first boot, once the arm is built: run-x86_64.sh, ablate.sh's
	# child, and that run's qemu.
	r="" q=""
	[ -n "$AB" ] && r=$(child_named "$AB" run-x86_64.sh 900) && q=$(first_qemu_of "$r")
	if [ -z "$q" ]; then
		ended_within "$JOB" 300 || kill -KILL "$AB" 2>/dev/null
		wait "$JOB" 2>/dev/null
		say abstop "ablate.sh never started its boot's qemu (it ended by $(cat "$OUT/abstop.ended" 2>/dev/null || echo 'nothing said'), $OUT/abstop.out)" "NOT ASKED"
	else
		sleep 1
		kill -s TERM "$AB"
		if ! ended_within "$AB" 30; then
			kill -KILL "$AB" 2>/dev/null
			wait "$JOB" 2>/dev/null
			say abstop "ablate.sh WENT ON for 30 s after SIGTERM$(leftovers "$q")" WRONG
		else
			wait "$JOB"
			ended=$(cat "$OUT/abstop.ended" 2>/dev/null)
			left=$(leftovers "$q")
			run=$(ls -d "$OUT/abstop-dir/runs/"*/ 2>/dev/null | tail -n 1)
			grep -aq ",ablated,1,$acc,.*,stopped," "$OUT/abstop-dir/ablate.csv" 2>/dev/null ||
				left="$left; no row of ablate.csv says its boot was stopped"
			grep -aq "^ablate: ablated, $acc, boot 1: stopped (" "$OUT/abstop.out" ||
				left="$left; no line says its boot was stopped"
			grep -aq "^ablate: stopped by SIGTERM at boot 1 of 2, ablated, $acc, boot 1$" "$OUT/abstop.out" ||
				left="$left; it did not say it stopped at boot 1 of 2: $(grep -a '^ablate: stopped' "$OUT/abstop.out" || echo 'it said nothing of it')"
			grep -aq '^ablate: ablated tree caught 0 of 0$' "$OUT/abstop.out" ||
				left="$left; it did not say what its boots said"
			[ -n "$run" ] && [ ! -d "${run}ablated" ] ||
				left="$left; its worktree is still there, ${run:-no run directory}ablated"
			[ "$ended" = "signal $(kill -l TERM)" ] ||
				left="$left; it ended by ${ended:-nothing said}, not by the signal"
			if [ -n "$left" ]; then
				say abstop "sent SIGTERM with its boot's qemu (pid $q) up$left" WRONG
			else
				say abstop "sent SIGTERM with its boot's qemu (pid $q) up, it wrote that boot's row as stopped, said where it stopped and what its boots said, removed its worktree and ended by the signal, the qemu gone and the lock given back" PASS
			fi
		fi
	fi
	# An ablate.sh this check had to kill could not remove its worktree, and
	# the check does, once the verdict is said.
	for w in "$OUT"/abstop-dir/runs/*/ablated; do
		[ -d "$w" ] && git -C "$REPO" worktree remove --force "$w" 2>/dev/null
	done
	git -C "$REPO" worktree prune 2>/dev/null
fi

# ── jobstop ───────────────────────────────────────────────────────────
# #951: ci/job.sh, sent SIGTERM during a boot of its merge test's, passes it on
# to the merge test, which stops that boot, and writes in job.txt where it
# stopped -- "stopped: by SIGTERM from outside, ..., in round 1, at boot 1 of
# 2: entry 1 ..." -- the boot's row as stopped and its logs in stopped/r1/,
# and exits 3, the boot's qemu gone and the lock given back: what a cancelled
# campaign run now does, its step passing the runner's signal on.  And what an
# import says of that job, asked of it with pull-results.sh --job: "STOPPED
# SHORT, by SIGTERM from outside", and its merge test's line "0 boots, 0 WRONG,
# 1 stopped", which named the time limit as the cause of every stop, a cancel's
# too, until #951 had it name none.  job.sh runs
# with this build, configured again from the preset, which leaves a configured
# build as it is, and built; it asks whether the patches apply, and its merge
# test boots entries 1 and 5.  The signal goes as soon as entry 1's qemu is
# there, under KVM a few seconds before that boot ends by itself.
if wanted jobstop; then
	acc=tcg
	[ ${#KVM[@]} = 0 ] || acc=kvm
	rm -rf "$OUT/jobstop" "$OUT/jobstop.ended" "$OUT/jobstop.ended.pid"
	python3 -I "$REPO/scripts/ended-by.py" "$OUT/jobstop.ended" -- \
		"$REPO/scripts/ci/job.sh" --no-install --entries "1 5" --accel "$acc" --rounds 1 \
		--out "$OUT/jobstop" > "$OUT/jobstop.out" 2>&1 &
	JOB=$!
	for i in $(seq 1 100); do
		[ -s "$OUT/jobstop.ended.pid" ] && break
		sleep 0.05
	done
	JS=$(cat "$OUT/jobstop.ended.pid" 2>/dev/null)
	STARTED+=(${JS:+"$JS"} "$JOB")
	# Its merge test, whose command name the kernel cuts to fifteen
	# characters, that one's run-x86_64.sh, and its qemu.  job.sh asks the
	# merge test -L before it builds, a child of the same name that boots
	# nothing and is gone within a second: one found is asked for a
	# run-x86_64.sh child, and gone without one, looked for again.  Found
	# once and taken for the merge test, it made the check NOT ASKED in 4
	# boots of 4 of an A/B on omen.
	m="" r="" q=""
	while [ -n "$JS" ] && kill -0 "$JS" 2>/dev/null; do
		m=$(child_named "$JS" merge-test-x86_ 600) || break
		r=$(child_named "$m" run-x86_64.sh 300) && break
	done
	[ -n "$r" ] && q=$(first_qemu_of "$r")
	if [ -z "$q" ]; then
		ended_within "$JOB" 300 || kill -KILL "$JS" 2>/dev/null
		wait "$JOB" 2>/dev/null
		say jobstop "job.sh's merge test never started a boot's qemu (job.sh ended by $(cat "$OUT/jobstop.ended" 2>/dev/null || echo 'nothing said'), $OUT/jobstop.out)" "NOT ASKED"
	elif qemu_gone_before "$q" jobstop; then
		ended_within "$JOB" 300 || kill -KILL "$JS" 2>/dev/null
		wait "$JOB" 2>/dev/null
	else
		kill -s TERM "$JS"
		if ! ended_within "$JS" 30; then
			kill -KILL "$JS" 2>/dev/null
			[ -n "$m" ] && kill -KILL "$m" 2>/dev/null
			wait "$JOB" 2>/dev/null
			say jobstop "job.sh WENT ON for 30 s after SIGTERM$(leftovers "$q")" WRONG
		else
			wait "$JOB"
			ended=$(cat "$OUT/jobstop.ended" 2>/dev/null)
			left=$(leftovers "$q")
			grep -aqE "^stopped:  by SIGTERM from outside, .*, in round 1, at boot 1 of 2: entry 1 $acc -smp [0-9]+, .*: stopped/r1/e1-$acc[0-9]+-full.log$" \
				"$OUT/jobstop/job.txt" 2>/dev/null ||
				left="$left; job.txt does not say it stopped at boot 1 of 2 and which boot it was: $(grep -a '^stopped:' "$OUT/jobstop/job.txt" 2>/dev/null || echo 'it has no stopped: line')"
			grep -aqE ",$acc,1,[0-9;]+,,stopped \(" "$OUT/jobstop/boots.csv" 2>/dev/null ||
				left="$left; no row of boots.csv says its boot was stopped"
			ls "$OUT"/jobstop/stopped/r1/e1-"$acc"*-full.log > /dev/null 2>&1 ||
				left="$left; the stopped boot's log is not in stopped/r1/"
			[ "$ended" = "exit 3" ] ||
				left="$left; it ended by ${ended:-nothing said}, not exit 3"
			said=$("$REPO/scripts/ci/pull-results.sh" --job "$OUT/jobstop" 2>&1)
			grep -qF "pull-results: here: job $OUT/jobstop STOPPED SHORT, by SIGTERM from outside, " <<< "$said" ||
				left="$left; pull-results.sh --job did not say it stopped short by SIGTERM from outside: $(sed -n 1p <<< "$said")"
			grep -qF "pull-results: here: job $OUT/jobstop: 1 job(s), 0 boots, 0 WRONG, 1 stopped; " <<< "$said" ||
				left="$left; pull-results.sh --job did not count its boot as stopped and nothing more: $(sed -n 2p <<< "$said")"
			if [ -n "$left" ]; then
				say jobstop "sent SIGTERM with its merge test's qemu (pid $q) up$left" WRONG
			else
				say jobstop "sent SIGTERM with its merge test's qemu (pid $q) up, it wrote in job.txt where it stopped and the boot it stopped, that boot's row and logs, and exited 3, the qemu gone and the lock given back; pull-results.sh --job says it stopped short by SIGTERM from outside, its boot counted as stopped and nothing more" PASS
			fi
		fi
	fi
fi

# ── jobfail ───────────────────────────────────────────────────────────
# #951: ci/job.sh, whose configure fails once job.txt is begun, writes why in
# it -- "failed:   the configure failed: configure.log" -- and exits 3; and
# pull-results.sh --job says of it "FAILED: the configure failed:
# configure.log; 0 boots of the 2 planned".  The Ubuntu jobs of 18/11, whose
# build failed, ended with their reason on stderr alone, and pull-results.sh
# said of them that they were cut off before writing their summary.  The
# configure is made to fail in a build directory of the check's own, whose
# cache names another source tree: cmake refuses it at once, saying it "does
# not match the source", and nothing is built or booted.  A log that does not
# say so is a configure that failed for another reason, or did not fail, and
# the check was not posed.
if wanted jobfail; then
	rm -rf "$OUT/jobfail" "$OUT/jobfail-build" "$OUT/jobfail.ended" "$OUT/jobfail.ended.pid"
	mkdir -p "$OUT/jobfail-build"
	echo "CMAKE_HOME_DIRECTORY:INTERNAL=/nonexistent/jobfail" > "$OUT/jobfail-build/CMakeCache.txt"
	UROS_BUILD_DIR=$OUT/jobfail-build CAMPAIGN_KIND=nightly \
		python3 -I "$REPO/scripts/ended-by.py" "$OUT/jobfail.ended" -- \
		"$REPO/scripts/ci/job.sh" --no-install --entries "1 5" --accel tcg --rounds 1 \
		--out "$OUT/jobfail" > "$OUT/jobfail.out" 2>&1 &
	JOB=$!
	for i in $(seq 1 100); do
		[ -s "$OUT/jobfail.ended.pid" ] && break
		sleep 0.05
	done
	JS=$(cat "$OUT/jobfail.ended.pid" 2>/dev/null)
	STARTED+=(${JS:+"$JS"} "$JOB")
	if ! ended_within "$JOB" 120; then
		kill -s TERM "$JS" 2>/dev/null
		ended_within "$JOB" 30 || kill -KILL "$JS" 2>/dev/null
		wait "$JOB" 2>/dev/null
		say jobfail "job.sh had not ended 120 s after it started: its configure did not fail as it was made to ($OUT/jobfail.out)" "NOT ASKED"
	else
		wait "$JOB"
		ended=$(cat "$OUT/jobfail.ended" 2>/dev/null)
		if ! grep -aq "does not match the source" "$OUT/jobfail/configure.log" 2>/dev/null; then
			say jobfail "its configure did not fail as it was made to: no configure.log says the cache \"does not match the source\" (job.sh ended by ${ended:-nothing said}, $OUT/jobfail.out)" "NOT ASKED"
		else
			left=""
			grep -aqxF "failed:   the configure failed: configure.log" "$OUT/jobfail/job.txt" 2>/dev/null ||
				left="$left; job.txt does not say why it ended: $(grep -a '^failed:' "$OUT/jobfail/job.txt" 2>/dev/null || echo "it has no failed: line, and its last is: $(tail -n 1 "$OUT/jobfail/job.txt" 2>/dev/null)")"
			[ "$ended" = "exit 3" ] ||
				left="$left; it ended by ${ended:-nothing said}, not exit 3"
			# #961: the kind of run it was given, which tells pull-results.sh
			# the nightly from a dispatch started by hand.
			grep -aqxF "kind:     nightly" "$OUT/jobfail/job.txt" 2>/dev/null ||
				left="$left; job.txt does not say it was a nightly job, which CAMPAIGN_KIND said: $(grep -a '^kind:' "$OUT/jobfail/job.txt" 2>/dev/null || echo 'it has no kind: line')"
			said=$("$REPO/scripts/ci/pull-results.sh" --job "$OUT/jobfail" 2>&1)
			grep -qxF "pull-results: here: job $OUT/jobfail FAILED: the configure failed: configure.log; 0 boots of the 2 planned" <<< "$said" ||
				left="$left; pull-results.sh --job said: $(sed -n 1p <<< "$said")"
			if [ -n "$left" ]; then
				say jobfail "its configure failed$left" WRONG
			else
				say jobfail "its configure failed, job.txt says so in its failed: line and that it was a nightly job, job.sh exited 3, and pull-results.sh --job says it FAILED, and why, with 0 boots of the 2 planned" PASS
			fi
		fi
	fi
fi

# ── jobnokvm ──────────────────────────────────────────────────────────
# #961: ci/pull-results.sh says a job of a run on GitHub that could not open
# /dev/kvm as a fault, on a line of its own: the nightly's jobs from 11/11 to
# 18/11 ran every boot under TCG alone, and the only sign of it was a word at
# the end of a line.  Three job directories are made here as an artifact would
# hold them, job.txt and the header of the rows, and nothing is booted: one of
# run 4242424242 whose /dev/kvm could not be opened, which must be said as a
# fault; one of the same run whose /dev/kvm could, and one run here, "run:
# local", whose could not -- of which nothing of the kind may be said.  A
# guard that also fired for those two would be no guard.
if wanted jobnokvm; then
	left=""
	for c in "4242424242:cannot be opened:fault" "4242424242:can be opened:none" "local:cannot be opened:none"; do
		run=${c%%:*}; rest=${c#*:}; k=${rest%%:*}; want=${rest#*:}
		d=$OUT/jobnokvm-$run-${k// /-}
		rm -rf "$d"
		mkdir -p "$d"
		printf '%s\n' "tree:     00000000" "run:      $run, job 1-arch" \
			"kvm:      /dev/kvm $k; accelerators: tcg" \
			"plan:     2 boots, 1 round(s) of 2" "boots:    0" > "$d/job.txt"
		echo "date,time,run_id,job,round,tree,cpu_model,mhz,accel,entry,smp,opt,verdict,log" > "$d/boots.csv"
		said=$("$REPO/scripts/ci/pull-results.sh" --job "$d" 2>&1)
		line="pull-results: here: job $d BOOTED WITHOUT KVM, a fault: its /dev/kvm could not be opened, and every boot ran under TCG alone"
		if [ "$want" = fault ]; then
			grep -qxF "$line" <<< "$said" ||
				left="$left; of run $run's job whose /dev/kvm $k it said: $(tr '\n' '|' <<< "$said")"
		elif grep -q "WITHOUT KVM" <<< "$said"; then
			left="$left; of the job of run $run whose /dev/kvm $k it said a fault: $(grep 'WITHOUT KVM' <<< "$said")"
		fi
	done
	if [ -n "$left" ]; then
		say jobnokvm "pull-results.sh --job${left}" WRONG
	else
		say jobnokvm "pull-results.sh --job says the job of run 4242424242 that could not open /dev/kvm BOOTED WITHOUT KVM, a fault, and nothing of the kind of the one that could, nor of a job run here" PASS
	fi
fi

echo "lifecycle: $pass PASS, $wrong WRONG, $unasked NOT ASKED -- $OUT"
[ $wrong = 0 ]
