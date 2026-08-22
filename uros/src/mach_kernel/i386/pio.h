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
/* CMU_HIST */
/*
 * Revision 2.5  91/05/14  16:14:20  mrt
 * 	Correcting copyright
 * 
 * Revision 2.4  91/02/05  17:13:56  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  17:37:08  mrt]
 * 
 * Revision 2.3  90/12/20  16:36:37  jeffreyh
 * 	changes for __STDC__
 * 	[90/12/07            jeffreyh]
 * 
 * Revision 2.2  90/11/26  14:48:41  rvb
 * 	Pulled from 2.5
 * 	[90/11/22  10:09:38  rvb]
 * 
 * 	[90/08/14            mg32]
 * 
 * 	Now we know how types are factor in.
 * 	Cleaned up a bunch: eliminated ({ for output and flushed unused
 * 	output variables.
 * 	[90/08/14            rvb]
 * 
 * 	This is how its done in gcc:
 * 		Created.
 * 	[90/03/26            rvb]
 * 
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/* 
 */
#ifndef I386_PIO_H
#define I386_PIO_H
#include <cpus.h>
#include <mach_assert.h>

typedef unsigned short i386_ioport_t;

/* read a longword */
extern unsigned long	inl(
				i386_ioport_t	port);
/* read a shortword */
extern unsigned short	inw(
				i386_ioport_t	port);
/* read a byte */
extern unsigned char	inb(
				i386_ioport_t	port);
/* write a longword */
extern void		outl(
				i386_ioport_t	port,
				unsigned long	datum);
/* write a word */
extern void		outw(
				i386_ioport_t	port,
				unsigned short	datum);
/* write a longword */
extern void		outb(
				i386_ioport_t	port,
				unsigned char	datum);

/* input an array of longwords */
extern void		linl(
				i386_ioport_t	port,
				int		* data,
				int		count);
/* output an array of longwords */
extern void		loutl(
				i386_ioport_t	port,
				int		* data,
				int		count);

/* input an array of words */
extern void		linw(
				i386_ioport_t	port,
				int		* data,
				int		count);
/* output an array of words */
extern void		loutw(
				i386_ioport_t	port,
				int		* data,
				int		count);

/* input an array of bytes */
extern void		linb(
				i386_ioport_t	port,
				char		* data,
				int		count);
/* output an array of bytes */
extern void		loutb(
				i386_ioport_t	port,
				char		* data,
				int		count);

/*
 * 🔥 The gate is on the wrong switch, and #485 is where that gets decided.
 *
 * "Should port I/O be inline" and "should assertions run" are two unrelated
 * questions, and tying the first to the second is why this block existed for
 * the life of the tree without once being compiled -- MACH_ASSERT has never
 * been off.  The out-of-line versions in i386/locore.S are what actually runs,
 * and they are a second copy of the same six accessors.
 *
 * Kept and made to compile rather than deleted: which heritage stays as
 * history is #433's question, not this one's.  What #485 needs from it is only
 * that -DUROS_MACH_ASSERT=OFF should produce a kernel rather than an
 * assembler error.
 */
#if defined(__GNUC__) && (!MACH_ASSERT)
/*
 * ⚠️ `gnu_inline\' (#485).  Under the GNU89 model `extern __inline__\' meant
 * "inline at every call site, defined elsewhere"; under -std=gnu11, which
 * this tree builds with, it EMITS an external definition -- and i386/locore.S
 * already exports all six, so the link failed with `multiple definition of
 * inl\'.  The attribute asks for the old meaning back, which is what the code
 * was written against.  `static\' would not do: these are also declared
 * extern at the top of this header, and the two disagree.
 *
 * 🔑 The keyword changed meaning underneath code that no build ever compiled,
 * so nothing said so for as long as the standard has been gnu11.
 */
extern __inline__ __attribute__((gnu_inline)) unsigned long	inl(
				i386_ioport_t port)
{
	unsigned long datum;
	__asm__ volatile("inl %1, %0" : "=a" (datum) : "d" (port));
	return(datum);
}

/*
 * ⚠️ `inw\' and not `.byte 0x66; inl\' (#485).
 *
 * The prefix was written by hand because a 1990s assembler had no 16-bit
 * mnemonic here; a current one has, and rejects the old form outright --
 * "incorrect register `%ax\' used with `l\' suffix", because gcc quite
 * reasonably allocates %ax for an unsigned short and the mnemonic says
 * long.  Nobody found out, because this whole block is behind
 * `#if !MACH_ASSERT\' and MACH_ASSERT has been on for the life of the tree:
 * code that is not compiled does not know it is broken.
 */
extern __inline__ __attribute__((gnu_inline)) unsigned short inw(
				i386_ioport_t port)
{
	unsigned short datum;
	__asm__ volatile("inw %1, %0" : "=a" (datum) : "d" (port));
	return(datum);
}

extern __inline__ __attribute__((gnu_inline)) unsigned char inb(
				i386_ioport_t port)
{
	unsigned char datum;
	__asm__ volatile("inb %1, %0" : "=a" (datum) : "d" (port));
	return(datum);
}

extern __inline__ __attribute__((gnu_inline)) void outl(
				i386_ioport_t port,
				unsigned long datum)
{
	__asm__ volatile("outl %0, %1" : : "a" (datum), "d" (port));
}

extern __inline__ __attribute__((gnu_inline)) void outw(		/* `outw\', see inw above (#485) */
				i386_ioport_t port,
				unsigned short datum)
{
	__asm__ volatile("outw %0, %1" : : "a" (datum), "d" (port));
}

extern __inline__ __attribute__((gnu_inline)) void outb(
				i386_ioport_t port,
				unsigned char datum)
{
	__asm__ volatile("outb %0, %1" : : "a" (datum), "d" (port));
}
#endif /* defined(__GNUC__) && (!MACH_ASSERT) */
#endif /* I386_PIO_H */
