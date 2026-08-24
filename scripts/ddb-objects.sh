#!/bin/sh
# Do the debugger's three views of the kernel agree? (#428)
#
# A command that walks a kernel structure is exactly as trustworthy as the
# walk, and this issue already produced one that was not: Mach's typed queues
# read as intrusive lists gave addresses, flag names and wait events that were
# all noise, and it LOOKED like a thread list.
#
# So this does not check that something was printed.  It reads three counters
# maintained by three subsystems that never consult each other --
#
#   the zone named "threads", counted by zalloc
#   the processor set's thread list, linked by the scheduler
#   every task's own activation list, linked by task_create
#
# -- off a single prompt, and refuses the run if they disagree.  A wrong walk
# breaks the agreement instead of producing plausible output.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$(dirname "$0")/ddb-common.sh"	# count, wait_for, ask (#428)
LOG=${1:-$HOME/uros-tests/ddb-objects.log}
IN=$(mktemp -u /tmp/ddb-obj.XXXXXX); mkfifo "$IN"; : > "$LOG"
qemu-system-x86_64 -cpu max -smp 4 -cdrom "$REPO/uros/build-x86_64/uros-x86_64.iso" \
	-nographic -serial mon:stdio -no-reboot < "$IN" > "$LOG" 2>&1 &
Q=$!; exec 3>"$IN"
w() { i=0; while [ $i -lt 900 ]; do grep -aq "$1" "$LOG" && return 0
	kill -0 "$Q" 2>/dev/null || return 1; sleep 0.1; i=$((i+1)); done; return 1; }
fail() { echo "FAILED: $1"; kill $Q 2>/dev/null; rm -f "$IN"; exit 1; }

w 'staying up on request' || fail "the kernel did not stay up"
sleep 2; printf '\034' >&3; w 'ddb> ' || fail "could not get in"
ask l || true; ask k || true; ask z || true
printf 'c\n' >&3; sleep 1
kill $Q 2>/dev/null || true; wait $Q 2>/dev/null || true; exec 3>&-; rm -f "$IN"

# The scheduler's view: the count the set declares, and the rows actually walked.
DECLARED=$(grep -a 'threads in the default set' "$LOG" | head -1 | awk '{print $1}')
WALKED=$(sed -n "/threads in the default set/,/ddb> /p" "$LOG" | count_stdin '^  0xffff')
# zalloc's view.
ZONED=$(grep -aE '^  [0-9]+\s+[0-9]+\s+[0-9]+\s+threads' "$LOG" | head -1 | awk '{print $2}')
# The tasks' view.
ACTS=$(grep -a 'activations in total' "$LOG" | head -1 | awk '{print $1}')

echo "pset declares $DECLARED · list walked $WALKED · zone \"threads\" $ZONED · task activations $ACTS"
[ -n "$DECLARED" ] && [ -n "$WALKED" ] && [ -n "$ZONED" ] && [ -n "$ACTS" ] \
	|| { echo "FAILED: one of the four numbers was not reported"; exit 1; }
[ "$DECLARED" = "$WALKED" ] || { echo "FAILED: the set declares $DECLARED threads and the walk found $WALKED"; exit 1; }
[ "$ZONED" -ge "$WALKED" ] || { echo "FAILED: zalloc has $ZONED shuttles but the list walked $WALKED"; exit 1; }
[ "$ACTS" = "$WALKED" ] || { echo "FAILED: the tasks own $ACTS activations and the set has $WALKED threads"; exit 1; }
echo "PASS: three independent counters agree"
