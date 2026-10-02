/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#ifndef	_I386_CN_NOLOCK_H_
#define	_I386_CN_NOLOCK_H_

/*
 * Console output that takes no lock (#344, #599), for code that must not
 * wait on one: an NMI handler, a processor spinning with interrupts off, a
 * path that can interrupt the holder of the console lock.  cnputc() goes to
 * com_putc() and fbcons_putc() and never takes printf_lock.
 *
 * ⚠️ Another processor's output can cut a line from here in two (#544).  A
 * caller whose line has to be found by a harness says it again.
 */

extern void	cnputc(char);

static __inline__ void
cn_puts(const char *s)
{
	while (*s)
		cnputc(*s++);
}

static __inline__ void
cn_hex(unsigned int v)
{
	int	i;

	cn_puts("0x");
	for (i = 28; i >= 0; i -= 4)
		cnputc("0123456789abcdef"[(v >> i) & 0xF]);
}

static __inline__ void
cn_dec(unsigned int v)
{
	char	buf[11];
	int	i = sizeof (buf);

	buf[--i] = '\0';
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	cn_puts(&buf[i]);
}

#endif	/* _I386_CN_NOLOCK_H_ */
