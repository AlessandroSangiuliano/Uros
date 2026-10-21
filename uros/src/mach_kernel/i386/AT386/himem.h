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

#ifndef _I386AT_HIMEM_H_
#define _I386AT_HIMEM_H_

/*
 * support of memory above 16 Megs for DMA limited to memory
 * below 16 Megs.
 */

#include <mach/vm_types.h>
#include <platforms.h>

#define HIMEM_STATS 0

#if	HIMEM_STATS
extern int himem_request;
extern int himem_used;
#endif	/* HIMEM_STATS */

struct himem_link {
	struct himem_link *next;
	vm_offset_t	high_addr;	/* physical address */
	vm_offset_t	low_page;	/* physical page */
	vm_offset_t offset;		/* offset on page */
	vm_size_t	length;
};
 
typedef struct himem_link *hil_t;	

#define HIGH_MEM		((vm_offset_t) 0xf00000)

#define _high_mem_page(x)	((vm_offset_t)(x) >= HIGH_MEM)

#if	HIMEM_STATS
#define high_mem_page(x) \
	(++himem_request && _high_mem_page(x) && ++himem_used)

#else	/* HIMEM_STATS */
#define high_mem_page(x) 	_high_mem_page(x)
#endif	/* HIMEM_STATS */

extern void		himem_init(void);
extern void		himem_reserve(
				int		npages);
extern vm_offset_t	himem_convert(
				vm_offset_t	paddr,
				vm_size_t	len,
				int		op,
				hil_t		* hil);
extern void		himem_revert(
				hil_t		hil);

#endif /* _I386AT_HIMEM_H_ */
