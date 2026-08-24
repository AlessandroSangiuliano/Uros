# Shared mechanics for the ddb harnesses (#428).  Sourced, not run.
#
# The callers set LOG (the file QEMU's console is being written to), Q (the
# QEMU process id) and file descriptor 3 (the write end of its input fifo)
# before using anything here.
#
# ── Why count() exists ────────────────────────────────────────────────────
#
# 🔥 `grep -c' EXITS 1 WHEN THE COUNT IS ZERO, and prints the zero anyway.
#
# Every script here runs under `set -e', and every one of them counted lines
# with a plain assignment:
#
#	before=$(grep -ac 'ddb> ' "$LOG")
#
# Under `set -e' an assignment whose command substitution fails kills the
# shell.  So each of these harnesses exited, silently and with a status that
# looks like a caught error, at the exact moment its pattern was ABSENT --
# which is the moment a harness has something to report.  A test that can only
# survive its own passing case is not a test.
#
# ⚠️ `|| true', never `|| echo 0': grep prints the count before it exits, so a
# fallback that echoes a zero appends a SECOND number and the variable becomes
# "0\n0".  That is precisely how run-x86_64.sh broke.
#
# 🔑 One function rather than fifteen corrected call sites: the gotcha is
# converted into a mechanism, so the next script written here cannot
# reintroduce it by forgetting.
count() {
	_c_n=$(grep -ac "$1" "$2" 2>/dev/null || true)
	[ -n "$_c_n" ] || _c_n=0
	printf '%s\n' "$_c_n"
}

# The same count taken over a pipeline instead of a file.  Separate rather
# than clever: a single function that guessed whether it had a filename would
# be one more thing to get wrong, and the pipeline's exit status is grep's, so
# it is the identical trap.
count_stdin() {
	_s_n=$(grep -ac "$1" 2>/dev/null || true)
	[ -n "$_s_n" ] || _s_n=0
	printf '%s\n' "$_s_n"
}

# Wait for PATTERN to appear in $LOG, giving up if the machine dies first.
#
# $1 pattern, $2 tenths of a second to wait (default 900 = ninety seconds).
wait_for() {
	_w_i=0
	_w_max=${2:-900}
	while [ "$_w_i" -lt "$_w_max" ]; do
		grep -aq "$1" "$LOG" && return 0
		kill -0 "$Q" 2>/dev/null || return 1
		sleep 0.1
		_w_i=$((_w_i + 1))
	done
	return 1
}

# Send a debugger command and wait for the prompt it produces.
#
# The prompt is counted rather than matched: `ddb> ' is already on the screen
# when the command is sent, so the evidence that it was answered is one MORE
# of them, not the presence of one.
#
# $1 command, $2 tenths of a second to wait (default 200 = twenty seconds).
ask() {
	_a_before=$(count 'ddb> ' "$LOG")
	printf '%s\n' "$1" >&3
	_a_i=0
	_a_max=${2:-200}
	while [ "$_a_i" -lt "$_a_max" ]; do
		[ "$(count 'ddb> ' "$LOG")" -gt "$_a_before" ] && return 0
		sleep 0.1
		_a_i=$((_a_i + 1))
	done
	echo "FAILED: '$1' never answered"
	return 1
}
