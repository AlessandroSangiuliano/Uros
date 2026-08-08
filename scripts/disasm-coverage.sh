#!/bin/sh
# What our disassembler can read, measured against objdump (#428).
#
# The oracle is already in the build: the kernel's own .text, a hundred and
# fifty thousand instructions of exactly the code this decoder exists to read.
#
# Two numbers, and they mean different things:
#
#   COVERAGE     how many it can NAME.  Rises as rows are added; it is the
#                progress, and it is allowed to be low.
#   DISAGREEMENTS where it and objdump differ on the LENGTH, or where it names
#                one thing and objdump names another.  This is the contract.
#                It must be zero.  A wrong name is one bad line; a wrong
#                length desynchronises everything after it.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
K="$REPO/uros/build-x86_64/iso-x86_64/boot/mach_kernel"
[ -f "$K" ] || { echo "no kernel at $K — build it first"; exit 2; }
OUT=$(mktemp -d /tmp/disasm.XXXXXX)
trap 'rm -rf "$OUT"' EXIT

cat > "$OUT/main.c" <<'EOF'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
unsigned disasm(const uint8_t *, unsigned, uint64_t, char *, unsigned);
int main(void)
{
	static uint8_t bytes[64];
	char line[512], want[64], got[64];
	unsigned long addr, len_want;
	unsigned n;
	unsigned long total = 0, named = 0, badlen = 0, badname = 0, refused = 0;

	/* stdin: "addr len mnemonic b0 b1 b2 ..." one per instruction */
	while (fgets(line, sizeof line, stdin)) {
		char *p = line;
		unsigned i = 0;
		addr = strtoul(p, &p, 16);
		len_want = strtoul(p, &p, 10);
		while (*p == ' ') p++;
		i = 0; while (*p && *p != ' ' && i + 1 < sizeof want) want[i++] = *p++;
		want[i] = 0;
		for (i = 0; i < len_want && i < sizeof bytes; i++)
			bytes[i] = (uint8_t) strtoul(p, &p, 16);

		total++;
		n = disasm(bytes, (unsigned) len_want, addr, got, sizeof got);
		if (n == 0) { refused++; continue; }
		if (n != len_want) { badlen++; if (badlen <= 5)
			fprintf(stderr, "length %lx: ours %u objdump %lu (%s vs %s)\n",
				addr, n, len_want, got, want); continue; }
		/* `?' and an unresolved group are NOT named, and must not be
		   scored as a wrong name -- refusing is the design. */
		if (got[0] == '?') continue;
		if (got[0] == 'g' && got[1] == 'r' && got[2] == 'p') continue;
		named++;
		if (strncmp(got, want, strlen(got)) != 0
		    && strncmp(want, got, strlen(want)) != 0) {
			badname++;
			if (badname <= 5)
				fprintf(stderr, "name %lx: ours %s objdump %s\n",
					addr, got, want);
		}
	}
	printf("instructions      %lu\n", total);
	printf("named             %lu  (%.1f%%)\n", named, 100.0 * named / total);
	printf("form not known    %lu  (%.1f%%)\n", refused, 100.0 * refused / total);
	printf("DISAGREEMENTS     %lu length, %lu name\n", badlen, badname);
	return (badlen || badname) ? 1 : 0;
}
EOF
gcc -O2 -w -I"$REPO/uros/src/mach_kernel/x86_64/ddb" \
    "$REPO/uros/src/mach_kernel/x86_64/ddb/disasm.c" "$OUT/main.c" -o "$OUT/probe"

# objdump gives "  addr:\tb0 b1 b2 \tmnemonic operands"
objdump -d "$K" 2>/dev/null | awk -F'\t' '
	NF >= 3 {
		split($1, a, ":"); gsub(/ /, "", a[1])
		nb = split($2, b, " ")
		# ⚠️ objdump puts a PREFIX in the mnemonic column -- "lock",
		# "rep", "data16", a bare "rex.*" -- so the first word is not
		# always the instruction.  Comparing against it scored a
		# thousand agreements as disagreements: the harness being
		# wrong about the decoder, which is the failure this file
		# exists to avoid one level down.
		nm = split($3, m, " ")
		w = 1
		while (w < nm && (m[w] == "lock" || m[w] == "rep" \
		       || m[w] == "repz" || m[w] == "repnz" \
		       || m[w] == "bnd" || m[w] == "data16" \
		       || m[w] ~ /^rex/))
			w++
		printf "%s %d %s", a[1], nb, m[w]
		for (i = 1; i <= nb; i++) printf " %s", b[i]
		printf "\n"
	}' > "$OUT/in.txt"

"$OUT/probe" < "$OUT/in.txt"
