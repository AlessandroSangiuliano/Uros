#!/bin/sh
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# Run the kernel's _doprnt at both target widths, on the host (#415).
#
#   scripts/doprnt-harness.sh
#
# The machine-independent tree does not compile for x86-64 yet, so the only
# place its printf can be run at that width today is here.  That matters
# because _doprnt spent thirty-nine years accepting an `l' and throwing it
# away, with a comment saying it would therefore not work where sizeof(long)
# differs from sizeof(int) -- and x86-64 is where.
#
# The functions under test are CUT OUT OF THE SOURCE by this script every
# time it runs, with sed, and are never transcribed.  A checked-in copy would
# drift from kern/printf.c and then what gets verified is the copy: the same
# mistake as measuring a generator against a second copy of its own
# arithmetic.  If the extraction stops matching, this fails loudly rather
# than testing something stale.
#
# Exits non-zero if any case is wrong at either width.

set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
KERN="$REPO/uros/src/mach_kernel"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

PRINTF_C="$KERN/kern/printf.c"
DBOUT_C="$KERN/ddb/db_output.c"

# ---------------------------------------------------------------------------
# Extraction.  Each function is taken from its definition line to the first
# closing brace in column one, which is the layout this tree uses throughout.
# ---------------------------------------------------------------------------
extract() {			# extract <file> <name>
	awk -v name="$2" '
		$0 ~ "^" name "\\(" { started = 1; print prev }
		started { print }
		started && /^}/     { exit }
		{ prev = $0 }
	' "$1"
}

for fn in printnum _doprnt_signed_arg _doprnt_unsigned_arg _doprnt_number \
	  _doprnt_ext; do
	extract "$PRINTF_C" "$fn" >> "$WORK/extracted.inc"
	if ! grep -q "^$fn(" "$WORK/extracted.inc"; then
		echo "doprnt-harness: could not extract $fn from $PRINTF_C" >&2
		echo "  (the definition moved or changed shape -- fix the" >&2
		echo "   extraction rather than checking in a copy)" >&2
		exit 2
	fi
done

extract "$DBOUT_C" db_doprnt_ext | sed '1s/^static boolean_t/boolean_t/' \
	> "$WORK/dbext.inc"
if ! grep -q '^db_doprnt_ext(' "$WORK/dbext.inc"; then
	echo "doprnt-harness: could not extract db_doprnt_ext from $DBOUT_C" >&2
	exit 2
fi

# ---------------------------------------------------------------------------
# Just enough of the kernel for those functions to compile on the host.
# ---------------------------------------------------------------------------
cat > "$WORK/harness.c" <<'PRELUDE'
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef int		boolean_t;
#define TRUE		1
#define FALSE		0
typedef unsigned long	vm_offset_t;
typedef unsigned int	mach_msg_type_number_t;
typedef unsigned int	mach_port_t;
typedef void	       *ipc_object_t;

static boolean_t _doprnt_truncates = FALSE;

#define	DOPRNT_LEN_INT		0
#define	DOPRNT_LEN_LONG		1
#define	DOPRNT_LEN_LONGLONG	2
#define	LEN_INT			DOPRNT_LEN_INT
#define	LEN_LONG		DOPRNT_LEN_LONG
#define	LEN_LONGLONG		DOPRNT_LEN_LONGLONG

struct doprnt_spec {
	char		ds_conv;
	int		ds_length;
	int		ds_prec;
	boolean_t	ds_ladjust;
	char		ds_padc;
	boolean_t	ds_altfmt;
	int		ds_plus_sign;
	int		ds_lensize;
	int		ds_radix;
};

typedef boolean_t (*doprnt_ext_t)(const struct doprnt_spec *, va_list *,
				  void (*)(char));

#define isdigit(d)	((d) >= '0' && (d) <= '9')
#define Ctod(c)		((c) - '0')
#define MAXBUF		(sizeof(long int) * 8)

void printnum(unsigned int, int, void (*)(char));
long _doprnt_signed_arg(const struct doprnt_spec *, va_list *);
unsigned long _doprnt_unsigned_arg(const struct doprnt_spec *, va_list *);
void _doprnt_number(unsigned long, int, int, int,
		    const struct doprnt_spec *, void (*)(char));
void _doprnt_ext(const char *, va_list *, void (*)(char), int, doprnt_ext_t);
boolean_t db_doprnt_ext(const struct doprnt_spec *, va_list *, void (*)(char));

/* The debugger's %b prints through this; nothing else here needs it. */
static void ipc_print_type_name(int t) { (void) t; }

#include "extracted.inc"
#include "dbext.inc"

static char	out[512];
static unsigned	outn, checked, wrong;

static void collect(char c)
{
	if (outn < sizeof(out) - 1)
		out[outn++] = c;
	out[outn] = '\0';
}

static void check(const char *want, const char *fmt, ...)
{
	va_list ap;

	outn = 0; out[0] = '\0';
	va_start(ap, fmt);
	_doprnt_ext(fmt, &ap, collect, 16, (doprnt_ext_t) 0);
	va_end(ap);

	checked++;
	if (strcmp(out, want) != 0) {
		wrong++;
		printf("  WRONG  fmt=%-12s got=%-24s want=%s\n",
		       fmt, out, want);
	}
}

/* The same, through the debugger's grammar. */
static void dcheck(const char *want, int radix, const char *fmt, ...)
{
	va_list ap;

	outn = 0; out[0] = '\0';
	va_start(ap, fmt);
	_doprnt_ext(fmt, &ap, collect, radix, db_doprnt_ext);
	va_end(ap);

	checked++;
	if (strcmp(out, want) != 0) {
		wrong++;
		printf("  WRONG  ddb fmt=%-10s radix=%-3d got=%-22s want=%s\n",
		       fmt, radix, out, want);
	}
}

int main(void)
{
	printf("== _doprnt: int=%zu long=%zu ptr=%zu ==\n",
	       sizeof(int), sizeof(long), sizeof(void *));

	/* The one the 1987 comment predicted. */
	check("-1", "%d", -1);
	check("-2147483648", "%d", -2147483647 - 1);
	check("2147483647", "%d", 2147483647);
	check("0", "%d", 0);
	check("42", "%d", 42);

	check("4294967295", "%u", 4294967295u);
	check("deadbeef", "%x", 0xdeadbeefu);
	check("DEADBEEF", "%X", 0xdeadbeefu);
	check("777", "%o", 0777u);

	/* The modifier that used to be read and thrown away. */
	check("-1", "%ld", -1L);
	check("-1", "%lld", -1LL);

	/*
	 * A value that only exists on the wide target.  Asking for it at
	 * -m32 would be asking long to hold what long is not, which is the
	 * test being wrong rather than the code.
	 */
	if (sizeof(long) == 8) {
		check("123456789012345", "%ld", 123456789012345L);
		check("123456789abcdef", "%lx", 0x123456789abcdefUL);
		/* The same value without the modifier: the low half. */
		check("89abcdef", "%x", (unsigned) 0x123456789abcdefUL);
	}

	/*
	 * The dialect belongs to the debugger now, so plain _doprnt echoes
	 * these as the unknown characters they are.
	 */
	check("D", "%D", -1L);
	check("r", "%r", -1L);

	check("abc", "%s", "abc");
	check("  abc", "%5s", "abc");
	check("abc  ", "%-5s", "abc");
	check("q", "%c", 'q');
	check("50%", "50%%");
	check("007", "%03d", 7);
	check("  -7", "%4d", -7);
	check("0x00ff", "%#06x", 0xffu);

	/* Two in a row: a wrong width desynchronises everything after it. */
	check("1 -2 three", "%d %d %s", 1, -2, "three");

	/* ---- the debugger's own conversions ---- */
	dcheck("255", 10, "%r", 255L);		/* signed, current radix   */
	dcheck("ff", 16, "%r", 255L);
	dcheck("-255", 10, "%r", -255L);
	dcheck("377", 8, "%n", 255L);		/* unsigned, current radix */
	dcheck("0xff", 16, "%#n", 255L);
	dcheck("      ff", 16, "%8n", 255L);
	dcheck("ff      ", 16, "%-8n", 255L);
	dcheck("ff", 16, "%z", 255L);		/* signed hexadecimal      */
	dcheck("-ff", 16, "%z", -255L);
	dcheck("FF", 16, "%Z", 255L);
	dcheck("-1", 16, "%D", -1L);		/* the pre-C89 long forms  */
	dcheck("377", 16, "%O", 255L);
	dcheck("255", 16, "%U", 255L);
	dcheck("3<BITTWO,BITONE>", 16, "%b", 3, "\10\2BITTWO\1BITONE");
	/* Claimed by nobody: echoed as itself. */
	dcheck("q", 16, "%q", 1);

	printf("== %u cases, %u wrong ==\n", checked, wrong);
	return wrong != 0;
}
PRELUDE

# ---------------------------------------------------------------------------
# Both widths.  i386 is the target that runs today; x86-64 is the one the
# question is about.
# ---------------------------------------------------------------------------
status=0
for bits in 32 64; do
	if ! gcc -m$bits -w -I "$WORK" -o "$WORK/h$bits" "$WORK/harness.c"; then
		echo "doprnt-harness: -m$bits build failed" >&2
		exit 2
	fi
	"$WORK/h$bits" || status=1
done

exit $status
