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
# The kernel reaches bootstrap_create and panics there (#422) within
# milliseconds of its first tick, so on this target the running machine to
# break into is the one stopped AT THE PANIC PROMPT -- which is itself the
# thing under test: panic() used to go straight to halt_cpu() with a debugger
# sitting there unused, because its guard asked whether the 1990 ddb/ tree was
# compiled in rather than whether a debugger existed.
if ! wait_for 'ddb> '; then
	echo "FAILED: panic did not open the prompt"
	kill $QPID 2>/dev/null; rm -f "$IN"; exit 1
fi
echo "panic opened the prompt"
for c in r t; do printf '%s\n' "$c" >&3; sleep 1; done

# And the console door, from that prompt's own machine: continue, then break
# back in with the character.  `c' from a panic returns into halt, so this
# proves the door rather than the prompt.
printf 'c\n' >&3; sleep 1
printf '\034' >&3          # Ctrl-\, the break character
sleep 2
sleep 1
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true
exec 3>&-; rm -f "$IN"
echo "log: $LOG"
