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
 * Revision 2.6.7.5  92/09/15  17:15:33  jeffreyh
 * 	Fix to be able to compile STD+COMPAQ (Single cpu).
 * 	Fixed garbled comments.
 * 	[92/06/09            bernadat]
 * 
 * Revision 2.6.7.4  92/04/30  11:56:56  bernadat
 * 	Adaptations for Corollary and Systempro
 * 	[92/04/08            bernadat]
 * 
 * 	Get the boot string passed by the boot and initialize
 * 	server name and arguments if typed at boot prompt.
 * 	Convert RB_ASKNAME to RB_ASKNAME << RB_SHIFT for the server
 * 	and use RB_HALT for initial halt. 
 * 	Detect number of cpus for Corollary before overwriting
 * 	boothowto with RB_ASKNAME << RB_SHIFT
 * 	[92/03/19            bernadat]
 * 
 * Revision 2.6.7.3  92/03/28  10:06:45  jeffreyh
 * 	Comment out line that does a warm reboot instead of a full
 * 	reboot. This fixes problems with vga cards that do not reset
 * 	correctly. It would be nice to find a way to do a soft reset
 * 	on all machines.
 * 	[92/03/10            jeffreyh]
 * 
 * Revision 2.6.7.2  92/03/03  16:17:40  jeffreyh
 * 	Changes from TRUNK
 * 	[92/02/26  11:39:10  jeffreyh]
 * 
 * Revision 2.7  92/01/03  20:11:49  dbg
 * 	Fixed so that mem_size can be patched to limit physical memory
 * 	use.
 * 	[91/09/29            dbg]
 * 
 * 	Rename kdb_init to ddb_init.
 * 	[91/09/11            dbg]
 * 
 * Revision 2.6.7.1  92/02/18  18:56:57  jeffreyh
 * 	Moved initialization of debug_user_with_kdb to FALSE to
 * 	kern/exception.c file
 * 	[91/12/09            bernadat]
 * 
 * 	Added call to cbus_stack_alloc for Corollary
 * 	[91/08/30            bernadat]
 * 
 * 	Support for Corollary MP
 * 	Added code for -h (halt option )
 * 	[91/06/25            bernadat]
 * 
 * Revision 2.6  91/07/31  17:42:41  dbg
 * 	Remove call to pcb_module_init (in machine-independent code).
 * 	[91/07/26            dbg]
 * 
 * Revision 2.5  91/06/19  11:55:48  rvb
 * 	cputypes.h->platforms.h
 * 	[91/06/12  13:45:37  rvb]
 * 
 * Revision 2.4  91/05/18  14:30:38  rpd
 * 	Changed pmap_bootstrap arguments.
 * 	Moved pmap_free_pages and pmap_next_page here.
 * 	[91/05/15            rpd]
 * 
 * Revision 2.3  91/05/14  16:29:13  mrt
 * 	Correcting copyright
 * 
 * Revision 2.2  91/05/08  12:44:52  dbg
 * 	Initialization for i386 AT bus machines only.
 * 	Combine code that was in i386/init.c and i386/i386_init.c.
 * 	[91/04/26  14:40:43  dbg]
 * 
 * Revision 2.3  90/09/23  17:45:10  jsb
 * 	Added support for iPSC2.
 * 	[90/09/21  16:39:41  jsb]
 * 
 * Revision 2.2  90/05/03  15:27:39  dbg
 * 	Alter for pure kernel.
 * 	[90/02/15            dbg]
 * 
 * Revision 1.5.1.4  90/02/01  13:36:37  rvb
 * 	esym must always be defined.  This is as good a place as any.
 * 	[90/01/31            rvb]
 * 
 * Revision 1.5.1.3  89/12/28  12:43:10  rvb
 * 	Fix av_start & esym initialization, esp for MACH_KDB.
 * 	[89/12/26            rvb]
 * 
 * Revision 1.5.1.2  89/12/21  17:59:49  rvb
 * 	enable esym processing.
 * 
 * 
 * Revision 1.5.1.1  89/10/22  11:30:41  rvb
 * 	Setup of rootdevice should not be here.  And it was wrong.
 * 	[89/10/17            rvb]
 * 
 * 	Scary!  We've changed sbss to edata.  AND the coff loader
 * 	following the vuifile spec was actually aligning the bss 
 * 	on 4k boundaries.
 * 	[89/10/16            rvb]
 * 
 * Revision 1.5  89/04/05  12:57:39  rvb
 * 	Move extern out of function scope for gcc.
 * 	[89/03/04            rvb]
 * 
 * Revision 1.4  89/02/26  12:31:25  gm0w
 * 	Changes for cleanup.
 * 
 * 31-Dec-88  Robert Baron (rvb) at Carnegie-Mellon University
 *	Derived from MACH2.0 vax release.
 *
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989, 1988 Carnegie Mellon University
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

/*
 *	File:	model_dep.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *
 *	Copyright (C) 1986, Avadis Tevanian, Jr., Michael Wayne Young
 *
 *	Basic initialization for I386 - ISA bus machines.
 */

#include <cpus.h>
#include <platforms.h>
#include <mp_v1_1.h>
#include <mach_kdb.h>
#include <eisa.h>
#include <himem.h>
#include <dipc.h>
#include <kernel_test.h>
#include <fast_idle.h>
#include <dipc_xkern.h>
#include <pci.h>

#include <mach/i386/vm_param.h>

#include <string.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <mach/machine.h>
#include <mach/time_value.h>
#include <kern/etap_macros.h>
#include <kern/spl.h>
#include <kern/assert.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <kern/clock.h>
#include <kern/time_out.h>
#include <kern/xpr.h>
#include <vm/vm_page.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <device/misc_protos.h>
#include <device/ds_routines.h>
#include <device/conf.h>
#include <device/subrs.h>
#include <i386/fpu.h>
#include <i386/pmap.h>
#include <i386/ipl.h>
#include <i386/pio.h>
#include <i386/misc_protos.h>
#include <i386/cpuid.h>
#include <i386/rtclock_entries.h>
#include <i386/multiboot.h>
#include <i386/AT386/mp/mp.h>
#include <i386/AT386/eisa.h>
#include <i386/AT386/eisa_entries.h>
#include <i386/AT386/misc_protos.h>
/* #208: kdsoft.h removed with kd.c; no actual references here. */
#if	DIPC
#include <dipc/dipc_funcs.h>
#endif	/* DIPC */
#if	MACH_KDB
#include <ddb/db_aout.h>
#endif /* MACH_KDB */
#include <ddb/tr.h>

#if	CBUS
#include <busses/cbus/cbus.h>
#endif	/* CBUS */

#if	MBUS
#include <busses/mbus/mbus.h>
#endif	/* MBUS */

#if	NCPUS > 1
#include <i386/mp_desc.h>
#endif	/* NCPUS */
#include <i386/sysenter.h>

#if	HIMEM
#include <i386/AT386/himem.h>
#endif

#if	MP_V1_1
#include <i386/AT386/mp/mp_v1_1.h>
#endif	/* MP_V1_1 */

#if	NPCI > 0
#include <device/dma_support.h>
#endif	/* NPCI > 0 */

int		loadpt;

vm_size_t	mem_size = 0; 
vm_offset_t	first_addr = 0;	/* set by start.s - keep out of bss */
vm_offset_t	first_avail = 1;/* set by start.s - keep out of bss */
vm_offset_t	last_addr;

vm_offset_t	avail_start, avail_end;
vm_offset_t	virtual_avail, virtual_end;
vm_offset_t	hole_start, hole_end;
vm_offset_t	avail_next;
unsigned int	avail_remaining;

/* parameters passed from GRUB/multiboot bootloader
 * MUST be in .data section, not BSS, because BSS is zeroed AFTER
 * start.S copies the multiboot_info structure here!
 */
int mb_info_size = sizeof(struct multiboot_info);
struct multiboot_info mb_info __attribute__((section(".data"))) = { .flags = 0xDEADBEEF };
extern vm_offset_t boot_start;
extern vm_size_t boot_size;
extern vm_offset_t exec_start;
extern vm_size_t exec_size;
extern pt_entry_t *kpde;

int		cnvmem __attribute__((section(".data"))) = 0;	/* must be in .data section */
int		extmem __attribute__((section(".data"))) = 0;

extern char	edata, end;

extern char	version[];

int		rebootflag = 0;	/* exported to com.c halt-char path */

/* Forced into .data so parse_arguments() can set this BEFORE
 * i386_init()'s bzero(&edata, &end-&edata) — which would otherwise
 * wipe the BSS copy back to 0 and silently swallow -h. */
int		halt_in_debugger __attribute__((section(".data"))) = 0;

/* `-H` counterpart: enters DDB AFTER setup_main() has spun up psets,
 * tasks, IPC, scheduler.  Same .data trick so it survives the BSS
 * clear.  See setup_main() + #213. */
int		halt_in_debugger_late __attribute__((section(".data"))) = 0;

extern int	cons_is_com1;

void		parse_arguments(void);
void		parse_multiboot(void);
const char	*getenv(const char *);

#define 	BOOT_LINE_LENGTH 160
char		boot_string_store[BOOT_LINE_LENGTH] = {0};
char 		*boot_string = (char *)0;
int		boot_string_sz = BOOT_LINE_LENGTH;
int		boottype = 0;


/*
 *	Cpu initialization.  Running virtual, but without MACH VM
 *	set up.  First C routine called.
 */
void
machine_startup(void)
{
	/*
	 * #214: drain any byte the BIOS left sitting in the 8042 output
	 * buffer.  Without this, IRQ 1 fires the moment the PIC unmasks
	 * before ps2.so has had a chance to register a handler, and
	 * intnull() prints a cosmetic "intnull(1)" line into the boot log.
	 * The drain is cheap (at most a few inb's) and idempotent.
	 */
	{
		extern unsigned char inb(unsigned short);
		int i;
		for (i = 0; i < 16 && (inb(0x64) & 0x01); i++)
			(void)inb(0x60);
	}

	/*
	 * Prepare multiboot information
	 */
	parse_multiboot();

	/*
	 * Parse startup arguments
	 */
	parse_arguments();

	/*
	 * Do basic VM initialization
	 */
	i386_init();

	/*
	 * Initialize the console so we can print.
	 */
	cninit();

#if	MACH_KDB

	/*
	 * Issue #211: scan multiboot modules for the "KSYM" magic and hand
	 * the blob to the DDB ELF backend before ddb_init() runs.  Without
	 * this DDB has no symbols and `b foo_func` falls back to numeric
	 * addresses.  parse_multiboot() has already populated mb_info.
	 */
	{
		extern struct multiboot_info mb_info;
		extern boolean_t elf_db_register(const void *base, size_t size);
		if (mb_info.mods_addr != 0) {
			struct multiboot_module *mods =
				(struct multiboot_module *)
					phystokv(mb_info.mods_addr);
			unsigned i;
			for (i = 0; i < mb_info.mods_count; i++) {
				const char *base =
					(const char *)phystokv(mods[i].mod_start);
				size_t sz = mods[i].mod_end - mods[i].mod_start;
				if (sz < 4) continue;
				if (base[0] == 'K' && base[1] == 'S' &&
				    base[2] == 'Y' && base[3] == 'M') {
					(void)elf_db_register(base, sz);
					break;
				}
			}
		}
	}

	/*
	 * Initialize the kernel debugger.
	 */
	ddb_init();

	/*
	 * Cause a breakpoint trap to the debugger before proceeding
	 * any further if `-h` was passed on the multiboot cmdline
	 * (parse_arguments() sets halt_in_debugger).  QEMU example:
	 *
	 *     run-qemu.sh -append "-h"
	 */

	if (halt_in_debugger) {
		printf("inline call to debugger(machine_startup)\n");
	        Debugger("");
	}
#endif	/* MACH_KDB */
	TR_INIT();

	printf(version);

	machine_slot[0].is_cpu = TRUE;
	machine_slot[0].running = TRUE;
	machine_slot[0].cpu_type = cpuid_cputype(0);
	machine_slot[0].cpu_subtype = CPU_SUBTYPE_AT386;

	/*
	 * Start the system.
	 */
#if	NCPUS > 1
	mp_desc_init(0);
#endif	/* NCPUS */

	/*
	 * Initialise SYSENTER/SYSEXIT fast system calls (Feature #44).
	 * Must be called after set_cpu_model() and mp_desc_init().
	 */
	sysenter_init();

	setup_main();
}

extern vm_offset_t kern_args_start;
extern vm_size_t kern_args_size;
extern vm_offset_t boot_args_start;
extern vm_size_t boot_args_size;
struct multiboot_module *mb_module = 0;

void
parse_multiboot(void)
{
	/* Get memory info */
	cnvmem = mb_info.mem_lower;
	extmem = mb_info.mem_upper;

	/* 
         * Get information about the bootstrap. Currently we only
	 * support loading one module.
	 */
	mb_module = (struct multiboot_module *) mb_info.mods_addr;
 	boot_start = mb_module[0].mod_start;
 	boot_size = mb_module[0].mod_end - mb_module[0].mod_start;
 
 	if (mb_info.mods_count >= 2) {
 		exec_start = mb_module[1].mod_start;
 		exec_size  = mb_module[1].mod_end - mb_module[1].mod_start;
 	}


	kern_args_start = mb_info.cmdline;
	kern_args_size = strlen((char *) kern_args_start);

 	//boot_args_start = mb_module->cmdline;
 	//boot_args_size = strlen(boot_args_start);
}

/*
 * Parse command line arguments.
 */
void
parse_arguments(void)
{
	char *p = (char *) kern_args_start;
	char *endp;
	char ch;

	if (kern_args_start == 0 || kern_args_size == 0)
	    return;

	endp = p + kern_args_size - 1;

	/*
	 * QEMU's multiboot cmdline starts with the kernel file path as
	 * argv[0] (e.g. ".../osfmk-mklinux/.../mach_kernel -h").  The
	 * historical parser scanned for any '-' anywhere — which then
	 * happily matched 'h' inside "mach_kernel", 'm' inside "osfmk"
	 * (parsing "mem=0"), and 'r' inside "Scrivania" (forcing serial
	 * console).  Require the '-' to start a real argument: either
	 * preceded by whitespace or sitting at the very beginning.
	 *
	 * Each flag is a single character; the inner loop terminates at
	 * the next whitespace, so subsequent characters within the same
	 * token are read by per-flag arg parsers (e.g. '-m128' lets the
	 * atoi_term consumer pick up the digits).
	 */
	while (p < endp) {
	    char prev = (p == (char *)kern_args_start) ? ' ' : *(p - 1);
	    if (*p != '-' || (prev != ' ' && prev != '\t')) {
		p++;
		continue;
	    }
	    p++;	/* skip the '-' */
	    while ((ch = *p) && ch != ' ' && ch != '\t') {
		p++;
		switch (ch) {
		case 'h':
		    halt_in_debugger = 1;
		    break;
		case 'H':
		    /* #213: enter DDB AFTER setup_main() — required for
		     * `show all threads`, `show ipc_port`, anything that
		     * walks scheduler / IPC state. */
		    halt_in_debugger_late = 1;
		    break;
		case 'r':
		    cons_is_com1 = 1;
		    break;
		case 'm':	/* -m??:  memory size Mbytes*/
		    mem_size = atoi_term(p, &p)*1024*1024;
		    break;
		case 'k':	/* -k??:  memory size Kbytes */
		    mem_size = atoi_term(p, &p)*1024;
		    break;
		default:
#if	NCPUS > 1 && AT386
		    if (ch > '0' && ch <= '9')
			wncpu = ch - '0';
#endif	/* NCPUS > 1 && AT386 */
		    break;
		}
	    }
	}
}


/*
 * Return boot information in buf.
 */
#define BOOTDEVNAME "disk"
#define BOOTDNLEN	(sizeof(BOOTDEVNAME) - 1)
char *
machine_boot_info(char *buf, vm_size_t size)
{
	dev_ops_t disk_ops;
	int unit, len;
	extern int startup_single_user;
	static char devname[BOOTDNLEN + 1 + 1];

	unit = (boottype >> 16) & 0xf;
	strcpy(devname, BOOTDEVNAME);
	devname[BOOTDNLEN] = '0' + unit;
	devname[BOOTDNLEN+1] = '\0';
	if (dev_name_lookup(devname, &disk_ops, &unit)) {
	    char *p;

	    strcpy(buf, disk_ops->d_name);
	    itoa(unit, buf + strlen(buf));
	    p = buf + strlen(buf);
	    *p++ = 'a' + ((boottype >> 8) & 0xff);
	    *p = 0;
	} else
	    strcpy(buf, "boot_device");
	if (startup_single_user)
	    strcat(buf, " -s");
	len = strlen(boot_string);
	if (len != 0) {
		char *p = buf + strlen(buf);
		*p++ = ' ';
		strcpy(p, boot_string);
	}
	return buf;
}

extern vm_offset_t env_start;
extern vm_size_t env_size;

const char *
getenv(const char *name)
{
	int len = strlen(name);
	const char *p = (const char *)env_start;
	const char *endp = p + env_size;

	while (p < endp) {
		if (len >= endp - p)
			break;
		if (strncmp(name, p, len) == 0 && *(p + len) == '=')
			return p + len + 1;
		while (*p++)
			;
	}
	return NULL;
}

extern void
calibrate_delay(void);

/*
 * Find devices.  The system is alive.
 */
void
machine_init(void)
{
	dev_ops_t boot_dev_ops;
	int unit;
	const char *p;
	int n;
	char bootdev_name[10] = "hd0s1";

	/*
	 * Adjust delay count before entering drivers
	 */

	calibrate_delay();

	/*
	 * Display CPU identification
	 */
	cpuid_cpu_display("CPU identification", 0);
	cpuid_cache_display("CPU configuration", 0);

#if	MP_V1_1
	mp_v1_1_init();
	/*
	 * #302: enable the BSP local APIC now that mp_v1_1_init's phase 2
	 * has io_map()'d the LAPIC MMIO window into lapic_start.  This is
	 * what arms lapic_send_ipi() / lapic_eoi() for the rest of the boot.
	 * AP enable lands in slave_machine_init in a later increment.
	 */
	{
		extern void lapic_enable(void);
		lapic_enable();
	}

	/*
	 * #311: bring up the I/O APIC and switch device-IRQ delivery off the
	 * global 8259 onto LAPIC vectors with per-CPU TPR masking.  Must run
	 * after lapic_enable() (BSP LAPIC live) and with kernel_map alive
	 * (mp_v1_1_init's phase 2 io_map'd the LAPIC just above).  Falls back
	 * silently to the 8259 if no usable I/O APIC is present.
	 */
	{
		extern void ioapic_init(void);
		ioapic_init();
	}
#endif	/* MP_V1_1 */

	/*
	 * Set up to use floating point.
	 */
	init_fpu();

	/*
	 * Issue #180: install the SHA-NI fast path for sha256_compress
	 * if the CPU advertises Intel SHA Extensions in CPUID leaf 7.
	 * Must run after init_fpu() (which enables CR4.OSFXSR / OSXMMEXCPT
	 * — required before any XMM-using code can execute).
	 */
	{
		extern void sha256_dispatch_init(void);
		extern int  sha256_using_sha_ni(void);
		sha256_dispatch_init();
		if (sha256_using_sha_ni())
			printf("SHA: Intel SHA Extensions enabled (HMAC fast path)\n");
	}

#if	NPCI > 0
	dma_zones_init();
#endif	/* NPCI > 0 */
	/*
	 * Look for eisa bus
	 */
	probe_eisa();

	/*
	 * Find the devices
	 */
	probeio();

	/*
	 * Set the boot device to disk0 or whatever
	 */
	if (p = getenv("BOOTDEV"))
		strcpy(bootdev_name, p);
	if (p = getenv("BOOTUNIT")) {
		strcpy(bootdev_name + strlen(bootdev_name), p);
	} else {
		/* default is "disk0" */
		strcpy(bootdev_name + strlen(bootdev_name), "0");
	}

	if (dev_name_lookup(bootdev_name, &boot_dev_ops, &unit)) {

		if (p = getenv("BOOTPART")) {
			n = 0;
			while (*p >= '0' && *p <= '9')
				n = (n * 10) + (*p++ - '0');
		} else
			n = ((boottype >> 8) & 0xff);
		dev_set_indirection("boot_device", boot_dev_ops, unit+n);
	} else
		printf("Warning: unable to set boot_device\n");

	/*
	 * Configure clock devices.
	 */
	clock_config();
}

/*
 * Halt a cpu.
 */
void
halt_cpu(void)
{
	halt_all_cpus(FALSE);
}

int reset_mem_on_reboot = 1;

/*
 * Halt the system or reboot.
 */
void
halt_all_cpus(
	boolean_t	reboot)
{
	if (reboot) {
	    /*
	     * Tell the BIOS not to clear and test memory.
	     */
	    if (! reset_mem_on_reboot)
		*(unsigned short *)phystokv(0x472) = 0x1234;

	    kdreboot();
	}
	else {
	    rebootflag = 1;

	    /*
	     * Modern halt = power off.  Try the ACPI shutdown registers
	     * used by QEMU/PIIX4 (0x604), Bochs / newer QEMU (0xB004) and
	     * VirtualBox (0x4004).  The value 0x2000 is SLP_TYP=0 |
	     * SLP_EN, which transitions the chipset to S5 (soft-off).
	     * On real hardware without ACPI poweroff support this is a
	     * no-op and we fall through to the legacy cli/hlt loop, the
	     * operator can then trigger ctl-alt-del.
	     */
	    outw(0x604,  0x2000);   /* QEMU PIIX4 PM1a_CNT */
	    outw(0xB004, 0x2000);   /* Bochs / newer QEMU  */
	    outw(0x4004, 0x3400);   /* VirtualBox          */

	    printf("System halted.\n");
	    (void) spllo();
	}
	__asm__ __volatile__("cli");
	for (;;)
	    __asm__ __volatile__("hlt");
}

/*
 * Basic VM initialization.
 */

void
i386_init(void)
{
	int i,j;			/* Standard index vars. */
	vm_size_t	bios_hole_size;	

	/*
	 * Zero the BSS.
	 */
	bzero((char *)&edata,(unsigned)(&end - &edata));

	boot_string = &boot_string_store[0];

	/*
	 * Initialize the pic prior to any possible call to an spl.
	 */
	picinit();
	set_cpu_model();
	vm_set_page_size();

	/*
	 * Initialize the Event Trace Analysis Package
	 * Static Phase: 1 of 2
	 */
	etap_init_phase1();

	/*
	 * Compute the memory size.
	 */

#if	defined(CBUS) || defined(MBUS)

#if	CBUS
	loadpt = CBUS_START;
	first_addr = round_page(CBUS_START);
	last_addr = CBUS_START + cbus_memsz() * MB(1);
#endif	/* CBUS */

#if	MBUS
	/*
	 * XXX
	 * Figuring out how much real memory there is on a SystemPro
	 * is tough. You must use BIOS int calls to probe the EISA
	 * bus for each board.
	 * For the present time I assume that two configurations exist:
	 * Base machine with 8 Megs
	 * Base machine with 8 Megs + 32 Megs Memory extension
	 * For the second configuration extmem is slightly less than 16 Megs.
	 */
	 
	first_addr = 0;
	if (extmem > (8*1024))	
	  	extmem += (24 * 1024);	
	last_addr = 1024*1024 + extmem*1024;
#endif	/* MBUS */

#else	/* defined(CBUS) || defined(MBUS) */

#if 0
#if	MP_V1_1
	/*
	 * Memory size is stored in CMOS 0x34,0x35 in 64k regions
	 */

	outb(0x70, 0x35);
	extmem = inb(0x71)<<8;
	outb(0x70, 0x34);
	extmem += inb(0x71);
	extmem *= 64;
#endif
#endif

	first_addr = 0x1000;
		/* BIOS leaves data in low memory */
	last_addr = 1024*1024 + extmem*1024;
	/* extended memory starts at 1MB */
       
#endif	/* defined(CBUS) || defined(MBUS) */

#if	NCPUS > 1

	/*
	 * Do not use the 2 first memory pages, they are used to
	 * boot the other cpus
	 */
	first_addr += 0x2000;

#endif	/* NCPUS > 1 */

#ifdef	CBUS
	bios_hole_size = 0;
#else	/* CBUS */
	bios_hole_size = 1024*1024 - trunc_page((vm_offset_t)(1024 * cnvmem));
#endif	/* CBUS */

	/*
	 *	Initialize for pmap_free_pages and pmap_next_page.
	 *	These guys should be page-aligned.
	 */

#ifdef	CBUS
	hole_start = CBUS_START + MB(1);
#else	/* CBUS */
	hole_start = trunc_page((vm_offset_t)(1024 * cnvmem));
#endif	/* CBUS */
	hole_end = round_page((vm_offset_t)first_avail);

	/*
	 * compute mem_size
	 */

	if (mem_size != 0) {
	    if (mem_size < (last_addr - loadpt) - bios_hole_size)
		last_addr = loadpt + mem_size + bios_hole_size;
	}
	first_addr = round_page(first_addr);
	last_addr = trunc_page(last_addr);
	mem_size = (last_addr - loadpt) - bios_hole_size;
	avail_start = first_addr;
	avail_end = last_addr;
	printf("cnvmem: %d KB, extmem: %d KB, mem_size %d KB\n",
	       cnvmem, extmem, mem_size/1024);

	/*
	 *	Initialize kernel physical map, mapping the
	 *	region from loadpt to avail_start.
	 *	Kernel virtual address starts at VM_KERNEL_MIN_ADDRESS.
	 */


#if	NCPUS > 1 && AT386
	/*
	 * Must Allocate interrupt stacks before kdb is called and also
	 * before vm is initialized. Must find out number of cpus first.
	 */
	/*
	 * Get number of cpus to boot, passed as an optional argument
	 * boot: mach [-sah#]	# from 0 to 9 is the number of cpus to boot
	 */
	if (wncpu == -1) {
		/*
		 * "-1" check above is to allow for old boot loader to pass
		 * wncpu through boothowto. New boot loader uses environment.
		 */
		const char *cpus;
		if ((cpus = getenv("cpus")) != NULL) {
			/* only a single digit for now */
			if ((*cpus > '0') && (*cpus <= '9'))
				wncpu = *cpus - '0';
		} else
			wncpu = NCPUS;
	}
	mp_probe_cpus();
	interrupt_stack_alloc();

#endif	/* NCPUS > 1 && AT386 */

	set_cr4(get_cr4() | CR4_PGE);
	pmap_bootstrap(loadpt);

	/*
	 * pmap_bootstrap steals page-table pages from physical addresses
	 * starting at avail_start, advancing it to a value inside the memory
	 * hole (hole_start..hole_end covers BIOS/VGA area plus kernel text,
	 * data and page tables).  pmap_next_page only skips the hole when
	 * avail_next hits hole_start exactly; since avail_start lands above
	 * hole_start the skip never fires, and kernel/page-table pages get
	 * handed out as free physical memory.  Advance past the hole here.
	 */
	if (avail_start < hole_end)
		avail_start = hole_end;

	/* Steal the contiguous memory that's been requested by various
	   kernel subsystems.  */

	/* For each entry in the reserve table.  */
	for (i = 0; i < pmem_reserve_ctl_size; i++) {
	    /*
	     * Force the size requested to be a integer multiple of the
	     * page size.
	     */
	    unsigned long size_req =
		(((*pmem_reserve_ctl[i].pmem_size - 1)/I386_PGBYTES)
		 + 1) * I386_PGBYTES;

	    /* Try to fit it into each of the holes available on the
	       386.  Since we have MBUS or CBUS, but not both, I may
	       safely assume that the order of holes in memory is
	       regular hole (hole_start,hole_end) followed by the MBUS
	       hole, if it exists.  This allows us to try top of
	       memory, bottom of memory, and then top of hole (the
	       last only being tried if mbus).  */

	    /* End of memory.  */
	    if (avail_end - size_req >= 
#if	MBUS
		MBUS_BIOS_REMAP_END
#else	/* MBUS */
		hole_end
#endif
		) {
		avail_end -= *pmem_reserve_ctl[i].pmem_size;
		*pmem_reserve_ctl[i].pmem_addr = phystokv(avail_end);
		continue;
	    }

	    /* Beginning of memory.  */
	    if (avail_start + size_req < hole_start) {
		*pmem_reserve_ctl[i].pmem_addr = phystokv(avail_start);
		avail_start += size_req;
		continue;
	    }

#if	MBUS
	    /* With MBUS, there's a second memory hole, so it's worth
	       trying above the first hole as well as under it.  */
	    if (hole_end + size_req <
		MBUS_BIOS_REMAP_START) {
		*pmem_reserve_ctl[i].pmem_addr = phystokv(hole_end);
		hole_end += size_req;
		continue;
	    }
#endif	/* MBUS */

	    /* Couldn't allocate the physical memory.  Bummer, dude.  */
	    panic("Couldn't allocate physical memory for pmem_reserve_ctl entry.");
	}

	/* avail_start is now >= hole_end, so the hole is entirely below the
	   available range; do not subtract it a second time. */
	avail_remaining = atop(avail_end - avail_start);
#if	MBUS
	avail_remaining -= atop(MBUS_BIOS_REMAP_END-MBUS_BIOS_REMAP_START);
#endif	/* MBUS */

#if	!HIMEM
	avail_next = avail_start;
#else	/* !HIMEM */
	avail_next = avail_end - PAGE_SIZE;
	himem_init();
#endif	/* !HIMEM */
}

unsigned int
pmap_free_pages(void)
{
	return avail_remaining;
}

#if	!HIMEM
boolean_t
pmap_next_page(
	vm_offset_t *addrp)
{
	if (avail_next == avail_end)
		return FALSE;

	/* skip the hole */

	if (avail_next == hole_start)
		avail_next = hole_end;

#if	MBUS

	/* skip BIOS ROM remapping */

	if (avail_next == MBUS_BIOS_REMAP_START)
	  	avail_next = MBUS_BIOS_REMAP_END;
#endif	/* MBUS */

	*addrp = avail_next;
	avail_next += PAGE_SIZE;
	avail_remaining--;
	return TRUE;
}

#else	/* !HIMEM */

	/*
	 * In case of HIMEM, let the low memory pages be at head of free
	 * list to prevent use of himem when machine underloaded
	 */
boolean_t
pmap_next_page(
	vm_offset_t *addrp)
{

	if (avail_next < avail_start)
		return FALSE;

	/* skip the hole */

	if (avail_next == hole_end - PAGE_SIZE)
		avail_next = hole_start - PAGE_SIZE;

#if	MBUS

	/* skip BIOS ROM remapping */
	if (avail_next == MBUS_BIOS_REMAP_END - PAGE_SIZE)
	  	avail_next = MBUS_BIOS_REMAP_START - PAGE_SIZE);
#endif	/* MBUS */

	*addrp = avail_next;
	avail_next -= PAGE_SIZE;
	avail_remaining--;
	return TRUE;
}

#endif	/* !HIMEM */

boolean_t
pmap_valid_page(
	vm_offset_t x)
{
	return ((avail_start <= x) && (x < avail_end));
}

/*
 * Universal (Posix) time map. This should go away when the
 * kern/posixtime.c routine is removed from the kernel.
 */
vm_offset_t
utime_map(
	dev_t			dev,
	vm_offset_t		off,
	int			prot)
{
	extern time_value_t	*mtime;

#ifdef	lint
	dev++; off++;
#endif	/* lint */

	if (prot & VM_PROT_WRITE)
		return (-1);
	return (i386_btop(pmap_extract(pmap_kernel(), (vm_offset_t) mtime)));
}

/*XXX*/
void fc_get(tvalspec_t *ts);
#include <kern/clock.h>
#include <i386/rtclock_entries.h>
void fc_get(tvalspec_t *ts) {
	(void )rtc_gettime(ts);
}

#if	DIPC
boolean_t
no_bootstrap_task(void)
{
	return(FALSE);
}

ipc_port_t
get_root_master_device_port(void)
{
	return(IP_NULL); /* for now */
}


void
dipc_machine_init(void)
{
#if	FAST_IDLE
	extern fast_idle_enabled;
#endif	/* FAST_IDLE */
#if	KERNEL_TEST
	extern int kkt_test_iterations;
#endif	/* KERNEL_TEST */

#if	FAST_IDLE
	fast_idle_enabled = TRUE;
#endif	/* FAST_IDLE */
#if	KERNEL_TEST
#if	DIPC_XKERN
	kkt_test_iterations = 1;
#else	/* DIPC_XKERN */
	kkt_test_iterations = 10;
#endif	/* DIPC_XKERN */

#endif	/* KERNEL_TEST */
}

#endif	/* DIPC */

void
Debugger(
	const char	*message)
{
#ifdef	lint
	message++;
#endif	/* lint */

	__asm__("int3");
}

#if	XPR_DEBUG && (NCPUS == 1 || MP_V1_1)

int	xpr_time(void)
{
        tvalspec_t	time;

	rtc_gettime_interrupts_disabled(&time);
	return(time.tv_sec*1000000 + time.tv_nsec/1000);
}
#endif	/* XPR_DEBUG && (NCPUS == 1 || MP_V1_1) */
