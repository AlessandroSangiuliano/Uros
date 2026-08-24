#!/bin/sh
# Does a breakpoint stop the kernel and let it go again? (#428)
#
# Set on clock_event_tick, which on an idle machine (-S) is called a hundred
# times a second on every processor -- so a breakpoint that works fires within
# ten milliseconds, and one that does not is not slow, it is broken.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$(dirname "$0")/ddb-common.sh"	# count, wait_for, ask (#428)
LOG=${1:-$HOME/uros-tests/ddb-breakpoint.log}
TARGET=$(nm "$REPO/uros/build-x86_64/iso-x86_64/boot/mach_kernel" | awk '$3=="clock_event_tick"{print $1}')
[ -n "$TARGET" ] || { echo "FAILED: no clock_event_tick symbol"; exit 1; }
IN=$(mktemp -u /tmp/ddb-bp.XXXXXX); mkfifo "$IN"; : > "$LOG"
qemu-system-x86_64 -cpu max -smp 4 -cdrom "$REPO/uros/build-x86_64/uros-x86_64.iso" \
	-nographic -serial mon:stdio -no-reboot < "$IN" > "$LOG" 2>&1 &
Q=$!; exec 3>"$IN"

wait_for 'staying up on request' || { echo "FAILED: the kernel did not stay up"; kill $Q 2>/dev/null; rm -f "$IN"; exit 1; }
sleep 2
printf '\034' >&3
wait_for 'ddb> ' || { echo "FAILED: could not get in"; kill $Q 2>/dev/null; rm -f "$IN"; exit 1; }

ask "b $TARGET" || true
ask "b" || true
printf 'c\n' >&3

# It must stop again, at the breakpoint, on its own.
if wait_for 'ddb: breakpoint'; then
	echo "PASS: the breakpoint fired"
else
	echo "FAILED: the machine continued and never stopped at the breakpoint"
	kill $Q 2>/dev/null; rm -f "$IN"; exit 1
fi
ask "r" || true
ask "d 0" || true
printf 'c\n' >&3; sleep 2

# And once removed it must NOT fire again.
n=$(count 'ddb: breakpoint' "$LOG")
sleep 2
m=$(count 'ddb: breakpoint' "$LOG")
kill $Q 2>/dev/null || true; wait $Q 2>/dev/null || true; exec 3>&-; rm -f "$IN"
[ "$n" = "$m" ] && echo "PASS: removed, and it stopped firing" \
                 || echo "FAILED: it kept firing after being removed ($n -> $m)"
