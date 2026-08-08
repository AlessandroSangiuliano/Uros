#!/bin/sh
# Enter the debugger on a RUNNING machine, from the console (#428).
#
# Boots entry 1 (-r: debugger armed, nothing else), waits until the kernel is
# past its self-tests and running threads, then types the break character and
# waits for the prompt.  No fault is involved: this is the door that was
# missing, the one for a machine that has stopped answering rather than died.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
LOG=${1:-$HOME/uros-tests/ddb-break.log}
IN=$(mktemp -u /tmp/ddb-brk.XXXXXX)
mkfifo "$IN"; : > "$LOG"
qemu-system-x86_64 -cpu max -smp 4 \
	-cdrom "$REPO/uros/build-x86_64/uros-x86_64.iso" \
	-nographic -serial mon:stdio -no-reboot < "$IN" > "$LOG" 2>&1 &
QPID=$!
exec 3>"$IN"
wait_for() {
	i=0
	while [ $i -lt 900 ]; do
		grep -aq "$1" "$LOG" && return 0
		kill -0 "$QPID" 2>/dev/null || return 1
		sleep 0.1; i=$((i+1))
	done
	return 1
}
# Wait for the kernel to be up and ticking -- the door is polled from the tick,
# so typing before the first tick would prove nothing about the door.
# One command, and wait for the prompt to come back before the next.
#
# ⚠️ Firing them a fixed interval apart mixes the answers into whatever the
# kernel is printing, and a reader then scores the command as having answered
# nothing.  That happened, and a working backtrace was nearly reported as
# broken.
ask() {
	before=$(grep -ac 'ddb> ' "$LOG")
	printf '%s\n' "$1" >&3
	i=0
	while [ $i -lt 200 ]; do
		[ "$(grep -ac 'ddb> ' "$LOG")" -gt "$before" ] && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "FAILED: '$1' never came back to a prompt"
	return 1
}

# ── The console door, on a machine that is actually running ───────────
#
# GRUB entry 11 is `-rS': the kernel parks the thread that would reach
# bootstrap_create and idles instead, so there is a live machine to break
# into.  Every other entry stops within milliseconds of its first tick, which
# is why this door went written and undemonstrated.
if ! wait_for 'staying up on request'; then
	echo "FAILED: the kernel did not stay up — wrong entry?"
	kill $QPID 2>/dev/null; rm -f "$IN"; exit 1
fi

# Let it idle a while, so the prompt cannot be an accident of the boot still
# being in progress.  Several ticks must pass with nothing happening.
sleep 3
grep -aq 'ddb> ' "$LOG" && { echo "FAILED: the prompt was already open before anything was typed"; kill $QPID 2>/dev/null; rm -f "$IN"; exit 1; }

printf '\034' >&3          # Ctrl-\, the break character
if ! wait_for 'ddb> '; then
	echo "FAILED: the break character did not open the prompt on a running machine"
	kill $QPID 2>/dev/null; rm -f "$IN"; exit 1
fi
echo "the break character opened the prompt on a running machine"

for c in r t p l; do ask "$c" || break; done

printf 'c\n' >&3; sleep 1
printf '\034' >&3          # Ctrl-\, the break character
sleep 2
sleep 1
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true
exec 3>&-; rm -f "$IN"
echo "log: $LOG"
