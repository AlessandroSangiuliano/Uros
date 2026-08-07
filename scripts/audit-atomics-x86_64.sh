#!/bin/sh
# #410: every atomic in the x86-64 kernel must be a real one.
#
# The contract asks for each primitive to be verified as a genuinely atomic
# instruction rather than an optimistic sequence that happens to work. One
# CPU cannot show that; the disassembly can, because a read-modify-write on
# memory either carries the lock prefix or it does not.
#
# Two things this script learned the hard way, both of which made an earlier
# version report success while checking nothing:
#
#   - the mnemonics carry a size suffix (btsq, not bts), so a word-boundary
#     match finds none of them;
#   - finding none has to be reported, not skipped. A pattern that matches
#     nothing and a kernel that contains nothing look identical from here,
#     and only one of them is good news.
#
# xchg is the deliberate exception: with a memory operand it asserts the lock
# on its own and objdump prints it bare.
#
# What this does NOT check, said plainly so the pass is not read as more than
# it is: an ordinary read-modify-write like `addq $1,(%rax)` is a perfectly
# legitimate non-atomic update in single-threaded code, and the kernel is full
# of them. There is no way to tell one of those from an atomic that lost its
# lock by looking at the instruction alone. What is checked is the set of
# instructions that exist only to be atomic — those have no innocent form.
#
# Showing that it can fail, which is the only thing that makes a pass mean
# anything. An audit that has never rejected anything and an audit that
# cannot reject anything read the same from the outside:
#
#	cat > /tmp/ctl.S <<'EOF'
#		.text
#		.globl _start
#	_start:
#		lock xaddq %rbx, (%rax)
#		xaddq %rcx, (%rdx)
#	EOF
#	cc -m64 -nostdlib -static -o /tmp/ctl /tmp/ctl.S
#	scripts/audit-atomics-x86_64.sh /tmp/ctl	# must print UNLOCKED, exit 1
set -e
UROS=$(cd "$(dirname "$0")/../uros" && pwd)
K=${1:-$UROS/build-x86_64/export/uros/boot/mach_kernel}

if [ ! -f "$K" ]; then
	echo "no kernel at $K -- build the x86-64 target first" >&2
	exit 2
fi

echo "=== auditing $K ==="
BAD=0

check() {
	INSN=$1
	# Allow the size suffix, and take only the memory forms: a register-only
	# bts needs no lock and is not what this is looking for.
	ALL=$(objdump -d "$K" | grep -E "[[:space:]](lock )?${INSN}[bwlq]?[[:space:]]" | grep "(%r" || true)

	if [ -z "$ALL" ]; then
		echo "  $INSN: none on memory"
		return
	fi

	COUNT=$(printf '%s\n' "$ALL" | wc -l)
	UNLOCKED=$(printf '%s\n' "$ALL" | grep -v "lock " || true)

	if [ -n "$UNLOCKED" ]; then
		echo "  $INSN: $COUNT on memory, UNLOCKED:"
		printf '    %s\n' "$UNLOCKED"
		BAD=1
	else
		echo "  $INSN: $COUNT on memory, all locked"
	fi
}

for I in xadd cmpxchg16b cmpxchg bts btr btc; do check "$I"; done

XCHG=$(objdump -d "$K" | grep -E "[[:space:]]xchg[bwlq]?[[:space:]]" | grep "(%r" || true)
if [ -n "$XCHG" ]; then
	echo "  xchg: $(printf '%s\n' "$XCHG" | wc -l) on memory, bare by design (implicitly locked)"
else
	echo "  xchg: none on memory"
fi

[ "$BAD" = 0 ] && echo "=== all atomics are locked ===" || { echo "=== AUDIT FAILED ==="; exit 1; }
