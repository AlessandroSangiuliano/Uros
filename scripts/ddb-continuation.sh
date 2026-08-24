#!/bin/sh
# Does the debugger refuse to walk a thread blocked with a continuation? (#428)
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$(dirname "$0")/ddb-common.sh"	# count, wait_for, ask (#428)
LOG=${1:-$HOME/uros-tests/ddb-continuation.log}
IN=$(mktemp -u /tmp/ddb-cont.XXXXXX); mkfifo "$IN"; : > "$LOG"
qemu-system-x86_64 -cpu max -smp 4 -cdrom "$REPO/uros/build-x86_64/uros-x86_64.iso" \
	-nographic -serial mon:stdio -no-reboot < "$IN" > "$LOG" 2>&1 &
Q=$!; exec 3>"$IN"

wait_for 'ddb> ' || { echo "FAILED: the prompt never opened"; kill $Q 2>/dev/null; rm -f "$IN"; exit 1; }
ask l || true
printf 'c\n' >&3; sleep 1
kill $Q 2>/dev/null || true; wait $Q 2>/dev/null || true; exec 3>&-; rm -f "$IN"

if grep -aqE 'no stack, resumes at +<cont_probe_resume' "$LOG"; then
	echo "PASS: the continuation was reported by its resume point"
else
	echo "FAILED: no thread was reported as blocked with a continuation"
	grep -a 'ddb> l' -A 20 "$LOG" | head -20
	exit 1
fi
