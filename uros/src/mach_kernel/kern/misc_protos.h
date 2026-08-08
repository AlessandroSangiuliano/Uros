/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */

#ifndef	_MISC_PROTOS_H_
#define	_MISC_PROTOS_H_

#include <dipc.h>

#include <stdarg.h>
#include <string.h>
#include <machine/setjmp.h>
#include <mach/boolean.h>
#include <mach/message.h>
#include <mach/machine/vm_types.h>
#include <ipc/ipc_types.h>

/* Set a bit in a bit array */
extern void setbit(
	int		which,
	int		*bitmap);

/* Clear a bit in a bit array */
extern void clrbit(
	int		which,
	int		*bitmap);

/* Find the first set bit in a bit array */
extern int ffsbit(
	int		*bitmap);
extern int ffs(
	unsigned int	mask);

/* Move overlapping, arbitrarily aligned data from one array to another */
/* Not present on all ports */
extern void ovbcopy(
	const char	*from,
	char		*to,
	vm_size_t	nbytes);

/* Move arbitrarily-aligned data from one array to another */
extern void bcopy(
	const char	*from,
	char		*to,
	vm_size_t	nbytes);

extern int bcmp(
		const char *a,
		const char *b,
		vm_size_t len);

/* Zero an arbitrarily aligned array */
extern void bzero(
	char	*from,
	vm_size_t	nbytes);

/* Move arbitrarily-aligned data from a user space to kernel space */
extern boolean_t copyin(
	const char	*user_addr,
	char		*kernel_addr,
	vm_size_t	nbytes);

/* Move a NUL-terminated string from a user space to kernel space */
extern boolean_t copyinstr(
	const char	*user_addr,
	char		*kernel_addr,
	vm_size_t	max,
	vm_size_t	*actual);

/* Move arbitrarily-aligned data from a user space to kernel space */
extern boolean_t copyinmsg(
	const char	*user_addr,
	char		*kernel_addr,
	mach_msg_size_t nbytes);

/* Move arbitrarily-aligned data from a kernel space to user space */
extern boolean_t copyout(
	const char	*kernel_addr,
	char		*user_addr,
	vm_size_t	 nbytes);

/* Move arbitrarily-aligned data from a kernel space to user space */
extern boolean_t copyoutmsg(
	const char	*kernel_addr,
	char		*user_addr,
	mach_msg_size_t nbytes);

/*
 * #415: the print family says what it is.
 *
 * -Wformat was suppressed in the kernel's flags, and removing the suppression
 * changed nothing, because there was nothing to check: this kernel builds
 * -fno-builtin with its own printf, so the compiler had no reason to believe
 * any of these took a format string.  A warning that cannot fire is worse
 * than no warning -- from a distance it reads like a tree that has been
 * checked.
 *
 * These could not be added while _doprnt still spoke the debugger's dialect,
 * because %n means something else there and a check against the wrong grammar
 * reports on code that is right while staying quiet about code that is not.
 * The dialect now lives in ddb/db_output.c, and db_printf is deliberately not
 * annotated below, because it is not printf.
 */
extern int sscanf(const char *input, const char *fmt, ...)
	__attribute__((format(scanf, 2, 3)));

extern integer_t sprintf(char *buf, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

extern void printf(const char *format, ...)
	__attribute__((format(printf, 1, 2)));

extern void printf_init(void);

extern void panic(const char *string, ...)
	__attribute__((format(printf, 1, 2)));

extern void panic_init(void);

/*
 * The message of the panic in progress, or NULL.
 *
 * Machine-dependent shutdown code reads it to tell an orderly halt from a
 * fatal one: on the way out of panic() there is a report worth making, on the
 * way out of a reboot there is not.
 *
 * ⚠️ `const char *', and declared here for that reason.  Every reader used to
 * write its own `extern char *panicstr;' inside a function body -- dropping
 * the const, and never once compared with the definition in kern/debug.c
 * (#448, #453).
 */
extern const char *panicstr;

/*
 * Set while the first panicking processor is still printing its message, and
 * cleared when it has finished (#461).
 *
 * panic() raises it before the message and drops it after, so that the
 * processors it beat to panicstr have something to wait on.  They already do
 * under MACH_KDB, where each one goes on to enter the debugger; a machine
 * without a debugger has the same need for a different reason -- it prints a
 * backtrace on its way to a halt, and several of those arriving during the
 * message leave nothing readable.
 *
 * ⚠️ volatile, and declared here rather than as an `extern' at each point of
 * use, for the same reason as panicstr above: a reader that dropped the
 * volatile would be entitled to read it once and spin on the copy (#448).
 */
extern volatile int panicwait;

extern void log(int level, char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/*
 * A conversion, as the parser found it (#415).
 *
 * _doprnt understands C's conversions and nothing else.  It used to
 * understand several more -- %r and %n printed in a caller-supplied radix,
 * %z was signed hex, %b decoded a register into bit names, and the
 * capitalised forms meant "long" from before C had `l' to say it with.  All
 * of them belong to the debugger, which is the only thing in this kernel that
 * still uses them, and ddb/db_output.c is where they live now.
 *
 * The reason for the move is %n.  In this dialect it means "unsigned, in the
 * current radix"; in C it means "store the count so far through a pointer
 * argument".  While both meanings live in one formatter, no declaration can
 * carry format(printf,...) honestly, because the compiler would be checking
 * against a grammar the callee does not implement -- and a check against the
 * wrong grammar is worse than none, since it reports on code that is right
 * and stays quiet about code that is not.
 *
 * So the debugger extends rather than reimplements: it is handed what the
 * parser already worked out and the two helpers below, and does not carry a
 * second copy of the flag handling or of the padding rules.
 */
struct doprnt_spec {
	char		ds_conv;	/* the conversion character */
	int		ds_length;	/* field width, 0 if none */
	int		ds_prec;	/* precision, -1 if none */
	boolean_t	ds_ladjust;	/* '-' seen */
	char		ds_padc;	/* ' ' or '0' */
	boolean_t	ds_altfmt;	/* '#' seen */
	int		ds_plus_sign;	/* '+' or ' ' or 0 */
	int		ds_lensize;	/* DOPRNT_LEN_* below */
	int		ds_radix;	/* the radix this caller printed with */
};

#define	DOPRNT_LEN_INT		0
#define	DOPRNT_LEN_LONG		1
#define	DOPRNT_LEN_LONGLONG	2

/*
 * Handle a conversion _doprnt does not know, or return FALSE to have it
 * treated as the unknown character it is.
 */
typedef boolean_t (*doprnt_ext_t)(
	const struct doprnt_spec	*spec,
	va_list				*argp,
	void				(*putc)(char));

/*
 * Print a number in a given base, with no padding or sign handling.  Used by
 * the debugger's %b, which prints a register value and then the names of the
 * bits set in it.
 */
extern void printnum(
	register unsigned int	u,
	register int		base,
	void			(*putc)(char));

/* Take the next integer argument at the width the conversion asked for. */
extern long _doprnt_signed_arg(
	const struct doprnt_spec	*spec,
	va_list				*argp);
extern unsigned long _doprnt_unsigned_arg(
	const struct doprnt_spec	*spec,
	va_list				*argp);

/*
 * Emit an already-fetched number, honouring width, padding, sign and the
 * alternate-form prefix.  `capitals' is 16 for upper-case hex digits, 0 for
 * lower; `sign_char' is what to print before it, or 0.
 */
extern void _doprnt_number(
	unsigned long			u,
	int				base,
	int				capitals,
	int				sign_char,
	const struct doprnt_spec	*spec,
	void				(*putc)(char));

void
_doprnt(
	register const char	*fmt,
	va_list			*argp,
	void			(*putc)(char),
	int			radix);

/* As _doprnt, with a handler for conversions outside C's set. */
extern void _doprnt_ext(
	register const char	*fmt,
	va_list			*argp,
	void			(*putc)(char),
	int			radix,
	doprnt_ext_t		ext);

extern void safe_gets(
	char	*str,
	int	maxlen);

extern void cnputc(char);

extern int cngetc(void);

extern int cnmaygetc(void);

extern int _setjmp(
	jmp_buf_t	*jmp_buf);

extern int _longjmp(
	jmp_buf_t	*jmp_buf,
	int		value);

extern void bootstrap_create(void);

extern void halt_cpu(void);

extern void halt_all_cpus(
		boolean_t	reboot);

extern void Debugger(
		const char	* message);

extern void delay(
		int		n);

extern char *machine_boot_info(
		char		*buf,
		vm_size_t	buf_len);

/*
 * Machine-dependent routine to fill in an array with up to callstack_max
 * levels of return pc information.
 */
extern void machine_callstack(
		natural_t	*buf,
		vm_size_t	callstack_max);

extern void consider_machine_collect(void);

extern void norma_bootstrap(void);

#if	DIPC
extern boolean_t	no_bootstrap_task(void);
extern ipc_port_t	get_root_master_device_port(void);
#endif	/* DIPC */

/*
 * Whether Debugger() would actually enter a debugger if called now (#428).
 *
 * A question only the machine can answer, and one the machine-independent
 * code needs because MACH_KDB no longer means what it used to: a target can
 * have a debugger of its own without the ddb/ tree in its build, and on that
 * target the answer also depends on a boot flag.  Callers use it to avoid
 * asking for a debugger that is not there -- from panic(), where the
 * alternative is a recursive panic.
 */
extern boolean_t	debugger_available(void);

#endif	/* _MISC_PROTOS_H_ */
