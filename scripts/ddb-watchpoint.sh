#!/bin/sh
# Does a watchpoint stop the kernel when memory is written? (#428)
#
# Set on clock_tick_delivered, which the timer handler increments a hundred
# times a second on every processor.  A watchpoint that works fires within ten
# milliseconds; one that does not is not slow, it is broken.
#
# ⚠️ The point of the test is the SECOND half: that it stops on a WRITE, and
# that removing it stops that.  A stop that happened for any other reason
# would read the same in the log, so the reason is checked, not just the stop.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
LOG=${1:-$HOME/uros-tests/ddb-watchpoint.log}
K="$REPO/uros/build-x86_64/iso-x86_64/boot/mach_kernel"
TARGET=$(nm "$K" | awk '$3=="clock_tick_delivered"{print $1}')
[ -n "$TARGET" ] || { echo "FAILED: no clock_tick_delivered symbol"; exit 1; }
IN=$(mktemp -u /tmp/ddb-wp.XXXXXX); mkfifo "$IN"; : > "$LOG"
qemu-system-x86_64 -cpu max -smp 4 -cdrom "$REPO/uros/build-x86_64/uros-x86_64.iso" \
	-nographic -serial mon:stdio -no-reboot < "$IN" > "$LOG" 2>&1 &
Q=$!; exec 3>"$IN"
w() { i=0; while [ $i -lt 900 ]; do grep -aq "$1" "$LOG" && return 0
	kill -0 "$Q" 2>/dev/null || return 1; sleep 0.1; i=$((i+1)); done; return 1; }
ask() { b=$(grep -ac 'ddb> ' "$LOG"); printf '%s\n' "$1" >&3
	i=0; while [ $i -lt 200 ]; do [ "$(grep -ac 'ddb> ' "$LOG")" -gt "$b" ] && return 0
	sleep 0.1; i=$((i+1)); done; return 1; }
fail() { echo "FAILED: $1"; kill $Q 2>/dev/null; rm -f "$IN"; exit 1; }

w 'staying up on request' || fail "the kernel did not stay up"
sleep 2
printf '\034' >&3
w 'ddb> ' || fail "could not get in"

ask "w $TARGET" || true
grep -aq 'watchpoint 0 at' "$LOG" || fail "the watchpoint was refused"
ask "b" || true
printf 'c\n' >&3

w 'ddb: watchpoint' || fail "the machine ran on and never stopped on the write"
echo "PASS: the watchpoint fired on a write"

ask "r" || true
ask "d 0" || true
printf 'c\n' >&3; sleep 2
n=$(grep -ac 'ddb: watchpoint' "$LOG"); sleep 3
m=$(grep -ac 'ddb: watchpoint' "$LOG")
kill $Q 2>/dev/null || true; wait $Q 2>/dev/null || true; exec 3>&-; rm -f "$IN"
[ "$n" = "$m" ] && echo "PASS: removed, and it stopped firing" \
                || echo "FAILED: it kept firing after being removed ($n -> $m)"
