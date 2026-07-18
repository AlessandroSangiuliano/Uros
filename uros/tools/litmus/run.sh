#!/bin/sh
# #350 - run the pmap #338 barrier litmus suite under x86-TSO and summarize.
#
# Requires herd7 (opam install herdtools7).  x86tso is herd7's default model for
# the X86 architecture, so no -model flag is needed.
#
#   ./run.sh        summary table (VERDICT vs EXPECT; '!' flags a mismatch)
#   ./run.sh -v     also dump full herd7 output for each test
#
# Exit status is nonzero if any verdict disagrees with the expected one, so this
# doubles as a regression check for the #338 barrier model.
cd "$(dirname "$0")" || exit 1

verbose=0
[ "$1" = "-v" ] && verbose=1

if ! command -v herd7 >/dev/null 2>&1; then
	echo "herd7 not found -- run:  opam install herdtools7  &&  eval \$(opam env)" >&2
	exit 2
fi

printf '%-16s  %-10s  %s\n' TEST VERDICT EXPECT
printf '%-16s  %-10s  %s\n' ---------------- ---------- ------
rc=0
for t in sb-pmap-both:Never sb-pmap-noA:Sometimes sb-pmap-noB:Sometimes sb-pmap-none:Sometimes; do
	f=${t%%:*}
	want=${t##*:}
	out=$(herd7 "$f.litmus" 2>&1)
	got=$(printf '%s\n' "$out" | awk '/^Observation/ {print $3; exit}')
	[ -z "$got" ] && got=ERROR
	mark=""
	[ "$got" != "$want" ] && { mark="  <-- MISMATCH"; rc=1; }
	printf '%-16s  %-10s  %s%s\n' "$f" "$got" "$want" "$mark"
	[ "$verbose" = 1 ] && printf '%s\n\n' "$out"
done
exit $rc
