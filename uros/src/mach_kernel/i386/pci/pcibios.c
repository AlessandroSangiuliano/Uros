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
 * 
 */
/*
 * MkLinux
 */
/*
 * Taken from
 *
 *  Copyright (c) 1994	Wolfgang Stanglmeier, Koeln, Germany
 *                             <wolf@dentaro.GUN.de>
*/


#include <i386/pci/pci.h>
#include <i386/pci/pcibios.h>
#include <i386/pio.h>
#include <i386/eflags.h>		/* EFL_IF, for the port-pair lock below */
#include <kern/misc_protos.h>
#include <mach/std_types.h>

static char pci_mode;

/*--------------------------------------------------------------------
 *
 *      Determine configuration mode
 *
 *--------------------------------------------------------------------
*/


#define CONF1_ENABLE       0x80000000ul
#define CONF1_ADDR_PORT    0x0cf8
#define CONF1_DATA_PORT    0x0cfc


#define CONF2_ENABLE_PORT  0x0cf8
#define CONF2_FORWARD_PORT 0x0cfa


int pci_conf_mode (void)
{
	unsigned long result, oldval;

	int *v;

	for(v=(int *)phystokv(0xe0000); v< (int *)phystokv(0xfffff); v += 4)
	    if (*v == 0x5f32335f) {
		    break;
		    printf("BIOS32 Service Directory found at %p\n",v);
	    }


	/*---------------------------------------
	 *      Configuration mode 2 ?
	 *---------------------------------------
	*/

	outb (CONF2_ENABLE_PORT,     0);
	outb (CONF2_FORWARD_PORT, 0);
	if (!inb (CONF2_ENABLE_PORT) && !inb (CONF2_FORWARD_PORT)) {
		pci_mode = 2;
		return (2);
	};

	/*---------------------------------------
	 *      Configuration mode 1 ?
	 *---------------------------------------
	*/

	oldval = inl (CONF1_ADDR_PORT);
	outl (CONF1_ADDR_PORT, CONF1_ENABLE);
	result = inl (CONF1_ADDR_PORT);
	outl (CONF1_ADDR_PORT, oldval);

	if (result == CONF1_ENABLE) {
		pci_mode = 1;
		return (1);
	};

	/*---------------------------------------
	 *      No PCI bus available.
	 *---------------------------------------
	*/
	return (0);
}

/*--------------------------------------------------------------------
 *
 *      Build a pcitag from bus, device and function number
 *
 *--------------------------------------------------------------------
*/


pcici_t pcitag (unsigned char bus, 
		unsigned char device,
		unsigned char func)
{
	pcici_t tag;

	tag.cfg1 = 0;
	if (device >= 32) return tag;
	if (func   >=  8) return tag;

	switch (pci_mode) {

	case 1:
		tag.cfg1 = CONF1_ENABLE
			| (((unsigned long) bus   ) << 16ul)
			| (((unsigned long) device) << 11ul)
			| (((unsigned long) func  ) <<  8ul);
		break;
	case 2:
		if (device >= 16) break;
		tag.cfg2.port    = 0xc000 | (device << 8ul);
		tag.cfg2.enable  = 0xf1 | (func << 1ul);
		tag.cfg2.forward = bus;
		break;
	};
	return tag;
}

/*
 * ── The address and the data are two ports, and the pair is one access ──
 *
 * 🔴 IT WAS NOT SERIALISED ON THIS TARGET (#597).  Mechanism 1 writes the
 * address to 0xCF8 and then reads or writes the datum at 0xCFC; mechanism 2
 * selects the function through 0xCF8 and 0xCFA and then touches the port.
 * Anything that moves those between the two steps makes the access answer
 * about -- or write into -- a DIFFERENT device, and the caller cannot tell:
 * the value is a well-formed register.
 *
 * 🔥 Seen at -smp 4 and never on one processor, where no caller runs in an
 * interrupt path and the kernel does not preempt kernel code: the HAL's
 * rescan counted a device on bus 8 that is not there, its scan read 00:00.0's
 * class word as a BAR of the ISA bridge, and a claim of 0:0.0 was refused
 * because its class read back as all ones.  x86-64 had paid for the same
 * defect three times and fixed it in its own file only (1b4c138d).  This is
 * the same lock under the same name, the one device_master.c's lock order
 * names on both targets.
 *
 * ⚠️ The same shape as x86-64's: an xchg spin taken with interrupts off, and
 * the flag saved and restored by hand.  It needs nothing initialised, so it
 * serves the boot-time enumeration too, and the interrupts-off half closes the
 * one-processor case of a handler touching configuration space between the
 * two steps.
 */
static volatile unsigned char	pci_cfg_port_lock;

/*
 * The #597 ablations (mach_kernel/CMakeLists.txt): ABLATE_597_NO_LOCK is the
 * access as it was, ABLATE_597_WIDEN=N puts N reads of port 0x80 between the
 * address and the data.  The first access says which is built, so a log can
 * never pass for an ordinary kernel's.
 */
#if defined(ABLATE_597_NO_LOCK) || defined(ABLATE_597_WIDEN)
static void
ablate_597_says(void)
{
	static int	said;

	if (said)
		return;
	said = 1;
#ifdef ABLATE_597_NO_LOCK
	printf("pci: #597 ablation -- the configuration access takes no lock\n");
#endif
#ifdef ABLATE_597_WIDEN
	printf("pci: #597 ablation -- %d port 0x80 reads between address and "
	       "data\n", ABLATE_597_WIDEN);
#endif
}
#endif

static inline void
ablate_597_widen(void)
{
#ifdef ABLATE_597_WIDEN
	int	w;

	for (w = 0; w < ABLATE_597_WIDEN; w++)
		(void) inb(0x80);
#endif
}

static inline unsigned long
pci_cfg_port_enter(void)
{
	unsigned long	flags;
	unsigned char	busy;

#if defined(ABLATE_597_NO_LOCK) || defined(ABLATE_597_WIDEN)
	ablate_597_says();
#endif
#ifdef ABLATE_597_NO_LOCK
	return 0;
#endif
	__asm__ volatile("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
	for (;;) {
		busy = 1;
		__asm__ volatile("xchgb %0, %1"
				 : "+q" (busy), "+m" (pci_cfg_port_lock)
				 : : "memory");
		if (busy == 0)
			break;
		__asm__ volatile("pause");
	}
	return flags;
}

static inline void
pci_cfg_port_leave(unsigned long flags)
{
#ifdef ABLATE_597_NO_LOCK
	return;
#endif
	__asm__ volatile("" : : : "memory");
	pci_cfg_port_lock = 0;
	if (flags & EFL_IF)
		__asm__ volatile("sti" : : : "memory");
}

/*--------------------------------------------------------------------
 *
 *      Read register from configuration space.
 *
 *--------------------------------------------------------------------
*/


unsigned long pci_conf_read (pcici_t tag, unsigned long reg)
{
	unsigned long addr, data = 0, flags;

	if (!tag.cfg1) return (0xfffffffful);

	flags = pci_cfg_port_enter();
	switch (pci_mode) {

	case 1:
		addr = tag.cfg1 | reg & 0xfc;
#ifdef PCI_DEBUG
		printf ("pci_conf_read(1): addr=%x ", addr);
#endif
		outl (CONF1_ADDR_PORT, addr);
		ablate_597_widen();
		data = inl (CONF1_DATA_PORT);
		outl (CONF1_ADDR_PORT, 0   );
		break;

	case 2:
		addr = tag.cfg2.port | reg & 0xfc;
#ifdef PCI_DEBUG
		printf ("pci_conf_read(2): addr=%x ", addr);
#endif
		outb (CONF2_ENABLE_PORT , tag.cfg2.enable );
		outb (CONF2_FORWARD_PORT, tag.cfg2.forward);

		data = inl ((unsigned short) addr);

		outb (CONF2_ENABLE_PORT,  0);
		outb (CONF2_FORWARD_PORT, 0);
		break;
	};
	pci_cfg_port_leave(flags);

#ifdef PCI_DEBUG
	printf ("data=%x\n", data);
#endif

	return (data);
}

/*--------------------------------------------------------------------
 *
 *      Write register into configuration space.
 *
 *--------------------------------------------------------------------
*/


void pci_conf_write (pcici_t tag, unsigned long reg, unsigned long data)
{
	unsigned long addr, flags;

	if (!tag.cfg1) return;

	flags = pci_cfg_port_enter();
	switch (pci_mode) {

	case 1:
		addr = tag.cfg1 | reg & 0xfc;
#ifdef PCI_DEBUG
		printf ("pci_conf_write(1): addr=%x data=%x\n",
			addr, data);
#endif
		outl (CONF1_ADDR_PORT, addr);
		ablate_597_widen();
		outl (CONF1_DATA_PORT, data);
		outl (CONF1_ADDR_PORT,   0 );
		break;

	case 2:
		addr = tag.cfg2.port | reg & 0xfc;
#ifdef PCI_DEBUG
		printf ("pci_conf_write(2): addr=%x data=%x\n",
			addr, data);
#endif
		outb (CONF2_ENABLE_PORT,  tag.cfg2.enable);
		outb (CONF2_FORWARD_PORT, tag.cfg2.forward);

		outl ((unsigned short) addr, data);

		outb (CONF2_ENABLE_PORT,  0);
		outb (CONF2_FORWARD_PORT, 0);
		break;
	};
	pci_cfg_port_leave(flags);
}
