#!/bin/sh
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ddb-user-address.sh -- does the debugger refuse an address outside the
# kernel's half, at the prompt, instead of reading it through whoever is at
# the prompt? (#750)
#
# A debugger read asks the KERNEL's page tables whether an address is mapped
# and then reads it through whatever cr3 is live.  For an address in the lower
# half the two disagree: boot.S leaves the low gigabyte identity-mapped in the
# kernel's tables (#640), so a user address there was called mapped and then
# read through the task at the prompt -- a fault inside the debugger, or
# well-formed nonsense out of the task's memory.  That is the mismatch #518
# took out of the fault report, and #750 takes out of every ddb read and walk.
#
# This boots the "debugger entry" entry (-rB, the prompt opens from ordinary
# kernel context), and at the prompt asks `t', `x' and `i' about a user
# address that is canonical and not mapped.  With the fix each refuses it as
# "not a kernel address" before reading anything, and the prompt comes back.
# Without it the address is canonical, the kernel's tables have nothing there,
# and the answer is "not mapped"/"unmapped" instead -- a different word, and
# the sign that the read went as far as asking the tables about the low half.
#
# ⚠️ The address is high (0x00007ffffff00000), so no identity map covers it and
# nothing is ever dereferenced either way: the point to prove here is the
# refusal, said with the right reason, not a fault.  The walk's behaviour on an
# address the identity map DOES cover is held open by the ablation's widen.
#
# Builds in UROS_BUILD_DIR and boots --kvm when asked, so scripts/ablate.sh can
# run it as a Script: arm (#618).  Prints one line, PASS or WRONG, and exits 0
# or 1; a boot it could not make exits 2 and says nothing about the kernel.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$(dirname "$0")/ddb-common.sh"	# iso_for_entry, wait_for, ask (#428, #750)

KVM=
if [ "${1:-}" = --kvm ]; then
	KVM=-enable-kvm
	shift
fi

BUILD=${UROS_BUILD_DIR:-$REPO/uros/build-x86_64}
LOG=${UROS_X86_64_LOG:-$BUILD/ddb-user-address.log}
USER_VA=7ffffff00000		# canonical, user half, not mapped

iso_for_entry "debugger entry" || exit 2

IN=$(mktemp -u /tmp/ddb-user.XXXXXX)
mkfifo "$IN"
: > "$LOG"
# shellcheck disable=SC2086
qemu-system-x86_64 $KVM -cpu max -smp 4 \
	-cdrom "$BUILD/uros-x86_64.iso" \
	-nographic -serial mon:stdio -no-reboot < "$IN" > "$LOG" 2>&1 &
Q=$!
exec 3>"$IN"

if ! wait_for 'ddb> '; then
	echo "ddb-user-address: the prompt never opened — the boot was not made"
	kill "$Q" 2>/dev/null || true
	rm -f "$IN"
	exit 2
fi

# Each command, waiting for the prompt it produces -- which is itself the
# evidence the read did not wedge the debugger.
ask "t $USER_VA" || true
ask "x $USER_VA" || true
ask "i $USER_VA" || true
printf 'c\n' >&3
sleep 1
kill "$Q" 2>/dev/null || true
wait "$Q" 2>/dev/null || true
exec 3>&-
rm -f "$IN"

# The fix refuses all three as "not a kernel address".  One refusal that names
# the mapping instead ("not mapped"/"unmapped") is the walk or the read having
# asked the kernel's tables about an address in the lower half, and a command
# that printed neither is a walk that stopped without saying why.
#
# ⚠️ Counted over the session only, from the first prompt to "ddb: continuing":
# the boot before it and the kernel's output after `c' are not answers to these
# commands, and either may say "unmapped" about something else.
SESSION="$LOG.session"
tr -d '\r' < "$LOG" | awk '/ddb> / { on = 1 } on { print } /^ddb: continuing/ { exit }' > "$SESSION"
refused=$(count 'not a kernel address' "$SESSION")
tables=$(count '\(not mapped\|unmapped\)' "$SESSION")
if [ "$refused" -ge 3 ] && [ "$tables" -eq 0 ]; then
	echo "ddb-user-address: PASS — t, x and i each refused $USER_VA as not a kernel address, and the prompt came back (#750)"
	exit 0
fi
echo "ddb-user-address: WRONG — a user address was not refused as not a kernel address ($refused of 3 refused, $tables asked the tables) (#750)"
grep -aE 'ddb> |no backtrace:|not a kernel address|not mapped|unmapped' "$SESSION" | head -20
exit 1
