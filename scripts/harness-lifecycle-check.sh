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
#
# Each line ends PASS or WRONG; a check that could not be posed says NOT
# ASKED.  The boots are entry 6's, which ends as soon as ring 3 has answered.
#
# Usage:  harness-lifecycle-check.sh [--kvm] [--only CHECK[,CHECK...]] [--out DIR]
# Exit:   0 every check PASS · 1 a check WRONG · 2 it could not be run (the
#         lock already held, no build)
#
# The build is UROS_BUILD_DIR's, or uros/build-x86_64, as run-x86_64.sh's.

set -u
set -m			# its own process group for each job: a job sent INT gets it

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${UROS_BUILD_DIR:-$REPO/uros/build-x86_64}
LOCK=/tmp/uros-x86_64-run.lock
ENTRY=6
KVM=()
ONLY=bystander,signals,lockover,refuse,tools,orphan,killed
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

run_harness() {	# NAME: run-x86_64.sh in the background, its log under OUT
	UROS_X86_64_LOG="$OUT/$1.log" "$REPO/scripts/run-x86_64.sh" "${KVM[@]}" \
		--entry "$ENTRY" 120 > "$OUT/$1.out" 2>&1 &
	RUN=$!
	STARTED+=("$RUN")
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
		run_harness "signal-$sig"
		if ! q=$(first_qemu_of "$RUN"); then
			wait "$RUN"
			say "signal $sig" "the run never started its qemu (exit $?, $OUT/signal-$sig.out)" "NOT ASKED"
			continue
		fi
		sleep 2
		kill -s "$sig" "$RUN"
		if ! ended_within "$RUN" 30; then
			kill -KILL "$RUN" 2>/dev/null
			wait "$RUN" 2>/dev/null
			say "signal $sig" "the run WENT ON for 30 s after SIG$sig$(leftovers "$q")" WRONG
			continue
		fi
		wait "$RUN"
		rc=$?
		want=$(( 128 + $(kill -l "$sig") ))
		left=$(leftovers "$q")
		[ "$rc" = "$want" ] || left="$left; exited $rc, not $want: it did not end by the signal"
		if [ -n "$left" ]; then
			say "signal $sig" "sent SIG$sig with its qemu (pid $q) up${left}" WRONG
		else
			say "signal $sig" "sent SIG$sig with its qemu (pid $q) up, the run exited $rc, its qemu gone and the lock given back" PASS
		fi
	done
fi

# ── lockover ──────────────────────────────────────────────────────────
if wanted lockover; then
	run_harness lockover
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
	run_harness orphan-killed
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

echo "lifecycle: $pass PASS, $wrong WRONG, $unasked NOT ASKED -- $OUT"
[ $wrong = 0 ]
