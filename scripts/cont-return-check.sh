#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# #771: the verdict on a -l boot (GRUB entry 58, "a continuation that returns").
#
# The kernel does not survive the question -- a continuation that returns has
# nowhere to go, and the machine stops -- so the answer is read from the log
# run-x86_64.sh wrote:
#
#   PASS       thread_begin_trampoline's panic, naming cont_return_resume, and
#              not context_thread_start's, which knows only that "a thread
#              function" returned: the named one came first and stopped it;
#   WRONG      the nameless panic, or none at all after the probe woke its
#              thread, or one of the probe's own WRONG lines;
#   NOT ASKED  the probe never woke its thread, so nothing returned.
#
# Prints one line, PASS, WRONG or NOT ASKED; exits 0, 1 or 2.
set -u
LOG=${1:?usage: cont-return-check.sh LOG}
if [ ! -r "$LOG" ]; then
	echo "cont-return-check: NOT ASKED — no log at $LOG"
	exit 2
fi

named='thread [^ ]+: its continuation [^ ]+ <cont_return_resume\+0x0> returned'
woke='cont_return: thread [^ ]+ is parked with a continuation that returns'

if LC_ALL=C grep -aqE 'cont_return: WRONG' "$LOG"; then
	echo "cont-return-check: WRONG — $(LC_ALL=C grep -aoE 'cont_return: WRONG.*' "$LOG" | head -n 1)"
	exit 1
fi
if ! LC_ALL=C grep -aqE "$woke" "$LOG"; then
	echo "cont-return-check: NOT ASKED — the probe never woke its thread into the continuation"
	exit 2
fi
if LC_ALL=C grep -aqF 'a thread function returned' "$LOG"; then
	echo "cont-return-check: WRONG — the continuation returned into context_thread_start's nameless panic: the trampoline did not name it"
	exit 1
fi
if LC_ALL=C grep -aqE "panic\(cpu [0-9]+\): $named" "$LOG"; then
	echo "cont-return-check: PASS — $(LC_ALL=C grep -aoE "$named" "$LOG" | head -n 1)"
	exit 0
fi
echo "cont-return-check: WRONG — the probe woke its thread and no panic named its continuation"
exit 1
