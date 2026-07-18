/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef _I386_AT386_FBCONS_H_
#define _I386_AT386_FBCONS_H_

/*
 * Framebuffer descriptor filled from the multiboot2 framebuffer tag (type 8)
 * by mb2_parse() in model_dep.c.  Shared with the emergency framebuffer
 * console (fbcons.c) that lets us see boot/panic output on pure-UEFI machines,
 * which have neither a serial port nor legacy VGA text memory (#342).
 */
struct mb2_framebuffer {
	unsigned long long	addr;		/* physical base of the LFB     */
	unsigned int		pitch;		/* bytes per scanline           */
	unsigned int		width;		/* pixels                       */
	unsigned int		height;		/* pixels                       */
	unsigned char		bpp;		/* bits per pixel               */
	unsigned char		fb_type;	/* 1 = direct RGB, 2 = EGA text */
	unsigned char		present;
};

extern struct mb2_framebuffer	mb2_fb;

/*
 * fbcons_init() maps the framebuffer and clears the screen; it must run after
 * the pmap is up (i386_init()).  It is a no-op when no usable framebuffer was
 * handed to us.  fbcons_putc() draws one character and is itself a no-op until
 * fbcons_init() succeeds, so cnputc() may call it unconditionally.
 */
extern void	fbcons_init(void);
extern void	fbcons_late_init(void);	/* #372: alloc the pixel shadow (post-VM) */
extern void	fbcons_putc(char c);

/* Opt-in flag set by the '-f' boot argument; fbcons_init() is a no-op unless
 * it is set.  Off by default so the userspace gpu_server owns the display. */
extern int	fbcons_enabled;

#endif	/* _I386_AT386_FBCONS_H_ */
