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
 * Revision 2.1.1.1.2.1  92/03/03  16:16:56  jeffreyh
 * 	Remove keep_wired argument from call to vm_map_copyin.
 * 	[92/02/26            dlb]
 * 	Changes from TRUNK
 * 	[92/02/26  11:36:41  jeffreyh]
 * 
 * Revision 2.2  92/01/03  20:10:02  dbg
 * 	Created.
 * 	[91/08/20            dbg]
 * 
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991 Carnegie Mellon University
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
 * User LDT management.
 * Each thread in a task may have its own LDT.
 */

#include <kern/kalloc.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <mach/mach_i386_server.h>

#include <vm/vm_kern.h>

#include <i386/seg.h>
#include <i386/thread.h>
#include <i386/user_ldt.h>
#include <i386/misc_protos.h>          /* desc_fake_to_real */
#include <i386/proc_reg.h>             /* set_ldt */
#if	NCPUS > 1
#include <i386/mp_desc.h>              /* mp_gdt */
#endif

char	acc_type[8][3] = {
    /*	code	stack	data */
    {	0,	0,	1	},	/* data */
    {	0,	1,	1	},	/* data, writable */
    {	0,	0,	1	},	/* data, expand-down */
    {	0,	1,	1	},	/* data, writable, expand-down */
    {	1,	0,	0	},	/* code */
    {	1,	0,	1	},	/* code, readable */
    {	1,	0,	0	},	/* code, conforming */
    {	1,	0,	1	},	/* code, readable, conforming */
};

extern struct fake_descriptor ldt[];	/* for system call gate */

/* Forward */

extern boolean_t	selector_check(
				thread_t		thread,
				int			sel,
				int			type);

boolean_t
selector_check(
	thread_t		thread,
	int			sel,
	int			type)
{
	struct user_ldt	*ldt;
	int	access;

	ldt = thread->top_act->mact.pcb->ims.ldt;
	if (ldt == 0) {
	    switch (type) {
		case S_CODE:
		    return sel == USER_CS;
		case S_STACK:
		    return sel == USER_DS;
		case S_DATA:
		    return sel == 0 ||
			   sel == USER_CS ||
			   sel == USER_DS;
	    }
	}

	if (type != S_DATA && sel == 0)
	    return FALSE;
	if ((sel & (SEL_LDT|SEL_PL)) != (SEL_LDT|SEL_PL_U)
	  || sel > ldt->desc.limit_low)
		return FALSE;

	access = ldt->ldt[sel_idx(sel)].access;
	
	if ((access & (ACC_P|ACC_PL|ACC_TYPE_USER))
		!= (ACC_P|ACC_PL_U|ACC_TYPE_USER))
	    return FALSE;
		/* present, pl == pl.user, not system */

	return acc_type[(access & 0xe)>>1][type];
}

/*
 * Add the descriptors to the LDT, starting with
 * the descriptor for 'first_selector'.
 */

kern_return_t
i386_set_ldt(
	thread_act_t		thr_act,
	int			first_selector,
	descriptor_list_t	desc_list,
	mach_msg_type_number_t	count)
{
	user_ldt_t	new_ldt, old_ldt, temp;
	struct real_descriptor *dp;
	int		i;
	int		min_selector = 0;
	pcb_t		pcb;
	vm_size_t	ldt_size_needed;
	int		first_desc = sel_idx(first_selector);
	vm_map_copy_t	old_copy_object;
	thread_t		thread;

	if (first_desc < min_selector || first_desc > 8191)
	    return KERN_INVALID_ARGUMENT;
	if (first_desc + count >= 8192)
	    return KERN_INVALID_ARGUMENT;
	if (thr_act == THR_ACT_NULL)
	    return KERN_INVALID_ARGUMENT;
	if ((thread = act_lock_thread(thr_act)) == THREAD_NULL) {
		act_unlock_thread(thr_act);
	    return KERN_INVALID_ARGUMENT;
	}
	if (thread == current_thread())
		min_selector = LDTSZ;
	act_unlock_thread(thr_act);

	/*
	 * We must copy out desc_list to the kernel map, and wire
	 * it down (we touch it while the PCB is locked).
	 *
	 * We make a copy of the copyin object, and clear
	 * out the old one, so that the MIG stub will have a
	 * a empty (but valid) copyin object to discard.
	 */
	{
	    kern_return_t	kr;
	    vm_offset_t		dst_addr;

	    old_copy_object = (vm_map_copy_t) desc_list;

	    kr = vm_map_copyout(ipc_kernel_map, &dst_addr,
				vm_map_copy_copy(old_copy_object));
	    if (kr != KERN_SUCCESS)
		return kr;

	    (void) vm_map_wire(ipc_kernel_map,
			trunc_page(dst_addr),
			round_page(dst_addr +
				count * sizeof(struct real_descriptor)),
			VM_PROT_READ|VM_PROT_WRITE, FALSE);
	    desc_list = (descriptor_list_t) dst_addr;
	}

	/*
	 * The MIG contract (mach_i386.defs / descriptor_list_t) says
	 * userspace sends `struct fake_descriptor`, but the validation
	 * below and the bcopy at the bottom both read/store the entries
	 * as `struct real_descriptor` — the hardware format the CPU
	 * loads from the LDT.  Convert in-place via desc_fake_to_real() so the
	 * two layouts agree.  Without this call, the access byte ends
	 * up read from byte 5 (real's slot) of fake-laid-out memory,
	 * which happens to coincide with bits 8..15 of the limit field
	 * and produces spurious "valid code segment" matches in the
	 * switch — fix #256.
	 */
	desc_fake_to_real(desc_list, count);

	for (i = 0, dp = (struct real_descriptor *) desc_list;
	     i < count;
	     i++, dp++)
	{
	    switch (dp->access & ~ACC_A) {
		case 0:
		case ACC_P:
		    /* valid empty descriptor */
		    break;
		case ACC_P | ACC_CALL_GATE:
		    /* Mach kernel call */
		    *dp = *(struct real_descriptor *)
				&ldt[sel_idx(USER_SCALL)];
		    break;
		case ACC_P | ACC_PL_U | ACC_DATA:
		case ACC_P | ACC_PL_U | ACC_DATA_W:
		case ACC_P | ACC_PL_U | ACC_DATA_E:
		case ACC_P | ACC_PL_U | ACC_DATA_EW:
		case ACC_P | ACC_PL_U | ACC_CODE:
		case ACC_P | ACC_PL_U | ACC_CODE_R:
		case ACC_P | ACC_PL_U | ACC_CODE_C:
		case ACC_P | ACC_PL_U | ACC_CODE_CR:
		case ACC_P | ACC_PL_U | ACC_CALL_GATE_16:
		case ACC_P | ACC_PL_U | ACC_CALL_GATE:
		    break;
		default:
		    /* #385: end is an absolute address, not a length — free
		     * the whole page the copyout/wire reserved, else every
		     * i386_set_ldt leaks a wired page in ipc_kernel_map. */
		    (void) vm_map_remove(ipc_kernel_map,
					 trunc_page((vm_offset_t) desc_list),
					 round_page((vm_offset_t) desc_list
					     + count * sizeof(struct real_descriptor)),
					 VM_MAP_REMOVE_KUNWIRE);
		    return KERN_INVALID_ARGUMENT;
	    }
	}
	/*
	 * Always reserve room for the LDTSZ system slots (USER_SCALL,
	 * USER_RPC, USER_CS, USER_DS).  Even when the caller writes a
	 * smaller LDT, the running CPU still expects those selectors to
	 * resolve — copyout/copyin reload %es with USER_DS, the SYSENTER
	 * return path uses USER_CS, and any LDT_relative segment register
	 * that survives a yield will trip the next time it's loaded.
	 * Pre-#258 nobody noticed because i386_set_ldt didn't reload
	 * LDTR; the small LDT lived in pcb->ims.ldt but the CPU kept
	 * running on KERNEL_LDT until the next context switch.
	 */
	{
	    int end = first_desc + count;
	    if (end < LDTSZ)
	        end = LDTSZ;
	    ldt_size_needed = sizeof(struct real_descriptor) * end;
	}

	pcb = thr_act->mact.pcb;
	new_ldt = 0;
	for (;;) {
	    simple_lock(&pcb->lock);
	    old_ldt = pcb->ims.ldt;
	    if (!(old_ldt == 0 ||
		  old_ldt->desc.limit_low + 1 < ldt_size_needed))
		break;
	    if (new_ldt != 0)
		break;
	    /*
	     * No old LDT, or not big enough — drop lock, allocate, retry.
	     */
	    simple_unlock(&pcb->lock);

	    new_ldt = (user_ldt_t) kalloc(ldt_size_needed
					  + sizeof(struct real_descriptor));
	    new_ldt->desc.limit_low   = ldt_size_needed - 1;
	    new_ldt->desc.limit_high  = 0;
	    new_ldt->desc.base_low    =
			    ((vm_offset_t)&new_ldt->ldt[0]) & 0xffff;
	    new_ldt->desc.base_med    =
			    (((vm_offset_t)&new_ldt->ldt[0]) >> 16)
					     & 0xff;
	    new_ldt->desc.base_high   =
			    ((vm_offset_t)&new_ldt->ldt[0]) >> 24;
	    new_ldt->desc.access      = ACC_P | ACC_LDT;
	    new_ldt->desc.granularity = 0;
	}
	if (old_ldt == 0 ||
	    old_ldt->desc.limit_low + 1 < ldt_size_needed)
	{

	    /*
	     * Have new LDT.  If there was a an old ldt, copy descriptors
	     * from old to new.  Otherwise copy the default ldt — for any
	     * target activation, not just current_act().  Skipping this
	     * for non-current targets (pthread spawn calling i386_set_ldt
	     * on a freshly-created suspended thread, #258) leaves entries
	     * 0..first_desc-1 as kalloc'd garbage; when the kernel then
	     * loads that LDT via lldt on the next context switch, the
	     * CPU's micro-loaded LDTR can prefetch a garbage entry and
	     * we GP fault on the first user-side instruction even though
	     * %gs only references the (valid) entry at first_desc.
	     */
	    {
		struct real_descriptor template = {0, 0, 0, ACC_P, 0, 0 ,0};
		int n_total = (ldt_size_needed
		               / sizeof(struct real_descriptor));

		/* Slots 0..LDTSZ-1 are the kernel's standard user gates +
		 * USER_CS/USER_DS — always overlay them so the LDT is a
		 * legal one for the CPU to run on. */
		for (i = 0, dp = &new_ldt->ldt[0]; i < LDTSZ; i++, dp++)
		    *dp = *(struct real_descriptor *) &ldt[i];

		/* Carry over any existing per-thread descriptors above
		 * LDTSZ; the caller's incoming descriptors will overlay
		 * their slots via the bcopy further below. */
		if (old_ldt && old_ldt->desc.limit_low + 1 >
		    LDTSZ * sizeof(struct real_descriptor)) {
		    int carry = old_ldt->desc.limit_low + 1
		                - LDTSZ * sizeof(struct real_descriptor);
		    bcopy((char *)&old_ldt->ldt[LDTSZ],
		          (char *)&new_ldt->ldt[LDTSZ],
		          carry);
		}

		/* Fill any gap between the carried region (or LDTSZ) and
		 * first_desc with empty-but-present placeholders, matching
		 * the original behaviour for non-overlapping caller writes. */
		for (i = LDTSZ, dp = &new_ldt->ldt[LDTSZ];
		     i < first_desc && i < n_total; i++, dp++) {
		    if (!old_ldt ||
		        i * sizeof(struct real_descriptor)
		        >= old_ldt->desc.limit_low + 1)
		        *dp = template;
		}
	    }

	    temp = old_ldt;
	    old_ldt = new_ldt;	/* use new LDT from now on */
	    new_ldt = temp;	/* discard old LDT */

	    pcb->ims.ldt = old_ldt;	/* new LDT for thread */
	}

	/*
	 * Install new descriptors.
	 */
	bcopy((char *)desc_list,
	      (char *)&old_ldt->ldt[first_desc],
	      count * sizeof(struct real_descriptor));

	simple_unlock(&pcb->lock);

	/*
	 * If the target is the calling thread, reload LDTR right here so
	 * the caller can use the new descriptors without having to yield
	 * just to get act_machine_switch_pcb to run.  Done with
	 * preemption disabled to keep the GDT-then-lldt window from
	 * racing with a scheduler tick that would itself reload LDTR.
	 *
	 * Phase 6a tried this and saw a `mov %cx,%es` panic in
	 * copyout_retry — but that was upstream of the kalloc-garbage
	 * default-entries bug fixed earlier in this function: lldt'ing an
	 * LDT whose low slots held invalid present/type bits could
	 * destabilise segment-register loads done from kernel-side
	 * recovery paths.  With the bcopy guard dropped above, every
	 * slot of the new LDT is a valid copy of the kernel default or
	 * the caller's supplied descriptor, and the reload is safe.
	 */
	if (thr_act == current_act()) {
	    int mycpu;
	    mp_disable_preemption();
	    mycpu = cpu_number();
#if	NCPUS > 1
	    *(struct real_descriptor *)&mp_gdt[mycpu][sel_idx(USER_LDT)]
	        = pcb->ims.ldt->desc;
#else
	    (void)mycpu;
	    *(struct real_descriptor *)&gdt[sel_idx(USER_LDT)]
	        = pcb->ims.ldt->desc;
#endif
	    set_ldt(USER_LDT);
	    mp_enable_preemption();
	}

	if (new_ldt)
	    kfree((vm_offset_t)new_ldt,
		  new_ldt->desc.limit_low+1+sizeof(struct real_descriptor));

	/*
	 * Free the descriptor list.
	 *
	 * #385: vm_map_remove's third argument is the END address, not a
	 * length.  Passing count*sizeof(descriptor) made end < start (a
	 * few bytes vs the high kernel dst_addr), so vm_map_delete removed
	 * nothing and the wired page copyout/wire reserved leaked.  musl
	 * calls i386_set_ldt on every TLS install (each exec and fork), so
	 * ipc_kernel_map (1 MB) filled after ~256 installs; the next
	 * vm_map_copyout then returned KERN_NO_SPACE and TLS setup failed —
	 * the fork child faulted on %gs:0 and exec'd tasks hlt in __init_tp.
	 * Free the whole reserved page, mirroring the wire range above.
	 */
	(void) vm_map_remove(ipc_kernel_map,
			trunc_page((vm_offset_t) desc_list),
			round_page((vm_offset_t) desc_list
			    + count * sizeof(struct real_descriptor)),
			VM_MAP_REMOVE_KUNWIRE);
	return KERN_SUCCESS;
}

kern_return_t
i386_get_ldt(
	thread_act_t		thr_act,
	int			first_selector,
	int			selector_count,	/* number wanted */
	descriptor_list_t	*desc_list,	/* in/out */
	mach_msg_type_number_t	*count)		/* in/out */
{
	struct user_ldt *user_ldt;
	pcb_t		pcb = thr_act->mact.pcb;
	int		first_desc = sel_idx(first_selector);
	unsigned int	ldt_count;
	vm_size_t	ldt_size;
	vm_size_t	size, size_needed;
	vm_offset_t	addr;
	thread_t		thread;

	if (thr_act == THR_ACT_NULL || (thread = thr_act->thread)==THREAD_NULL)
	    return KERN_INVALID_ARGUMENT;

	if (first_desc < 0 || first_desc > 8191)
	    return KERN_INVALID_ARGUMENT;
	if (first_desc + selector_count >= 8192)
	    return KERN_INVALID_ARGUMENT;

	addr = 0;
	size = 0;

	for (;;) {
	    simple_lock(&pcb->lock);
	    user_ldt = pcb->ims.ldt;
	    if (user_ldt == 0) {
		simple_unlock(&pcb->lock);
		if (addr)
		    kmem_free(ipc_kernel_map, addr, size);
		*count = 0;
		return KERN_SUCCESS;
	    }

	    /*
	     * Find how many descriptors we should return.
	     */
	    ldt_count = (user_ldt->desc.limit_low + 1) /
			sizeof (struct real_descriptor);
	    ldt_count -= first_desc;
	    if (ldt_count > selector_count)
		ldt_count = selector_count;

	    ldt_size = ldt_count * sizeof(struct real_descriptor);

	    /*
	     * Do we have the memory we need?
	     */
	    if (ldt_count <= *count)
		break;		/* fits in-line */

	    size_needed = round_page(ldt_size);
	    if (size_needed <= size)
		break;

	    /*
	     * Unlock the pcb and allocate more memory
	     */
	    simple_unlock(&pcb->lock);

	    if (size != 0)
		kmem_free(ipc_kernel_map, addr, size);

	    size = size_needed;

	    if (kmem_alloc(ipc_kernel_map, &addr, size)
			!= KERN_SUCCESS)
		return KERN_RESOURCE_SHORTAGE;
	}

	/*
	 * copy out the descriptors
	 */
	bcopy((char *)&user_ldt->ldt[first_desc],
	      (char *)addr,
	      ldt_size);
	*count = ldt_count;
	simple_unlock(&pcb->lock);

	if (addr) {
	    vm_size_t		size_used, size_left;
	    vm_map_copy_t	memory;

	    /*
	     * Free any unused memory beyond the end of the last page used
	     */
	    size_used = round_page(ldt_size);
	    if (size_used != size)
		kmem_free(ipc_kernel_map,
			addr + size_used, size - size_used);

	    /*
	     * Zero the remainder of the page being returned.
	     */
	    size_left = size_used - ldt_size;
	    if (size_left > 0)
		bzero((char *)addr + ldt_size, size_left);

	    /*
	     * Unwire the memory and make it into copyin form.
	     */
	    (void) vm_map_unwire(ipc_kernel_map, trunc_page(addr),
				 round_page(addr + size_used), FALSE);
	    (void) vm_map_copyin(ipc_kernel_map, addr, size_used,
				TRUE, &memory);
	    *desc_list = (descriptor_list_t) memory;
	}

	return KERN_SUCCESS;
}

void
user_ldt_free(
	user_ldt_t	user_ldt)
{
	kfree((vm_offset_t)user_ldt,
		user_ldt->desc.limit_low+1+sizeof(struct real_descriptor));
}
