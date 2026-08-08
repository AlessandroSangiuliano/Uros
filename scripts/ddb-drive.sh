#!/bin/sh
# Drive the x86-64 debugger prompt headlessly (#428).
#
# The one done-when of #428 that had never been demonstrated is "DDB can be
# entered": every log on this target is a self-test run, and a self-test run
# never opens the prompt.  Reading the code and believing it is exactly the
# habit that let five defects through in #453.
#
# So this boots GRUB entry 9 (-rB: debugger armed, Debugger() called from
# ordinary kernel context), waits for the prompt to ACTUALLY appear on the
# wire, then types at it and reads the answers back.
#
# ⚠️ Waits for the prompt rather than sleeping a guessed number of seconds.
# A driver that types before the prompt is there feeds its commands to the
# boot messages and then reports that the debugger answered nothing.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
LOG=${1:-$HOME/uros-tests/ddb-drive.log}
IN=$(mktemp -u /tmp/ddb-in.XXXXXX)

ninja -C "$REPO/uros/build-x86_64" uros_iso >/dev/null
UROS_X86_64_BOOT_ENTRY=9 ninja -C "$REPO/uros/build-x86_64" uros_iso >/dev/null 2>&1 || true

mkfifo "$IN"
: > "$LOG"
# shellcheck disable=SC2086
qemu-system-x86_64 -cpu max -smp 4 \
	-cdrom "$REPO/uros/build-x86_64/uros-x86_64.iso" \
	-nographic -serial mon:stdio -no-reboot < "$IN" > "$LOG" 2>&1 &
QPID=$!
exec 3>"$IN"

wait_for() {
	i=0
	while [ $i -lt 600 ]; do
		grep -aq "$1" "$LOG" && return 0
		kill -0 "$QPID" 2>/dev/null || return 1
		sleep 0.1
		i=$((i+1))
	done
	return 1
}

if ! wait_for 'ddb> '; then
	echo "FAILED: the prompt never appeared — the debugger was not entered"
	kill "$QPID" 2>/dev/null || true
	rm -f "$IN"
	exit 1
fi
echo "prompt reached"

for cmd in r t c; do
	printf '%s\n' "$cmd" >&3
	sleep 1
done

wait_for 'Debugger() RETURNED' || true
sleep 1
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
exec 3>&-
rm -f "$IN"
echo "log: $LOG"
