/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -j: a fault from ring 0 on a task's present page that the processor's
 * protection refused is reported, not retried for ever (#750).
 *
 * With SMAP on, a kernel data access to a user page outside a copy faults
 * though the page is present; with SMEP on, a kernel fetch from one does.
 * trap()'s page-fault path hands such a fault to vm_fault() first, so that a
 * copy reaching a page the task has never touched succeeds -- and vm_fault()
 * finds nothing to do for a page that is mapped and resident, answers success,
 * and the instruction runs again, faults again, for ever, with nothing on the
 * wire.  fault_in() refuses it instead (#750), and here is the test that would
 * otherwise never return.
 *
 * ⚠️ A thread of a TASK, not of the kernel.  The loop is on a fault whose map
 * is a task's, chosen by the faulting address being in the lower half -- the
 * kernel's own pages do not reach it.  So this makes a task, gives it a data
 * page and a code page and faults them in, and runs the probe on a kernel
 * thread created IN that task, whose current_act()->map is the task's.
 *
 * ⚠️ One processor at a time: trap_expect() holds one expectation for the whole
 * machine (as cr_probe_here() in boot_c.c says).  Each processor's probe is a
 * thread bound to it, and the next is not started until the last has answered
 * -- or has not, which is the defect: a probe that does not answer within the
 * budget is one spinning on the retried fault, and the test says WRONG and
 * stops, since one is proof.
 *
 * ⚠️ Said where SMAP or SMEP is actually on, and NOT ASKED on a processor that
 * has neither: the store or the fetch would simply complete, which is not this
 * question.  The emulated default part has neither; -cpu max and real parts
 * have both.
 */

#include <stdint.h>

#include <kern/cpu_number.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>		/* thread_bind */
#include <kern/task.h>
#include <kern/thread.h>

#include <mach/machine.h>		/* machine_slot[] */
#include <mach/vm_prot.h>
#include <vm/vm_fault.h>		/* vm_fault, to make the pages resident */
#include <vm/vm_map.h>			/* vm_map_pmap */

#include <cpu/regs.h>			/* read_cr4, cpu_pause */
#include <pmap/layout.h>		/* phys_to_direct */
#include <pmap/pmap.h>			/* pmap_smap_on, pmap_extract */
#include <time/tsc.h>			/* rdtsc_ordered, tsc_hz */
#include <trap/trap.h>			/* trap_expect, trap_probe_write, T_PAGE_FAULT */

kern_return_t vm_allocate(vm_map_t, vm_offset_t *, vm_size_t, boolean_t);

#define REFUSED_FAULT_WAIT_S	10	/* a probe that spins longer is looping */

/* xor %eax, %eax ; ret -- a call that is allowed returns 0. */
static const uint8_t refused_fault_code[3] = { 0x31, 0xc0, 0xc3 };

static volatile vm_offset_t	rf_data_va;	/* the task's data page */
static volatile vm_offset_t	rf_code_va;	/* and its code page */
static volatile int		rf_cpu;		/* which processor probes now */
static volatile int		rf_go;		/* it may start */
static volatile int		rf_done;	/* it has answered */
static volatile int		rf_store_faulted;
static volatile int		rf_fetch_faulted;

static void
refused_fault_probe(void)
{
	int	(*fetch)(void) = (int (*)(void)) (uintptr_t) rf_code_va;

	thread_bind(current_thread(), cpu_to_processor(rf_cpu));
	thread_block((void (*)(void)) 0);	/* runs next where it is bound */
	while (!rf_go)
		cpu_pause();

	/*
	 * SMAP: a store to the task's data page, outside any copy bracket.
	 * trap_probe_write() returns 1 if the store faulted and was stepped
	 * over to trap_probe_faulted, 0 if it completed.  With the fix a
	 * refused fault steps over; without it this call never returns.
	 */
	if (pmap_smap_on) {
		trap_expect(T_PAGE_FAULT, (uint64_t)(uintptr_t) trap_probe_faulted);
		rf_store_faulted = trap_probe_write((volatile void *)(uintptr_t) rf_data_va);
		trap_expect_cancel();
	}

	/*
	 * SMEP: a fetch from the task's code page.  The page holds `xor
	 * %eax,%eax; ret', so a call that is allowed returns 0; with SMEP it
	 * faults before the first byte runs, and the fix steps over it.
	 */
	if ((read_cr4() & CR4_SMEP) != 0) {
		trap_expect(T_PAGE_FAULT, (uint64_t)(uintptr_t) trap_probe_faulted);
		rf_fetch_faulted = fetch();	/* 1 if it faulted, 0 if it ran */
		trap_expect_cancel();
	}

	rf_done = 1;
	thread_terminate_self();
}

/*
 * Make `va' resident in `map', readable and writable, by faulting it in as the
 * trap path would -- in thread context here, interrupts on.
 */
static int
refused_fault_touch(vm_map_t map, vm_offset_t va)
{
	return vm_fault(map, va, VM_PROT_READ | VM_PROT_WRITE, FALSE)
	       == KERN_SUCCESS;
}

void
refused_fault_test(void)
{
	task_t		probe_task = TASK_NULL;
	vm_offset_t	frame;
	unsigned	i, tested = 0, wrong = 0, asked = 0;

	printf("refused_fault_test: starting (#750)\n");

	if (task_create_local(kernel_task, FALSE, FALSE, &probe_task)
	    != KERN_SUCCESS || probe_task == TASK_NULL) {
		printf("refused_fault_test: NOT ASKED — no task to probe\n");
		return;
	}

	rf_data_va = rf_code_va = 0;
	if (vm_allocate(probe_task->map, (vm_offset_t *) &rf_data_va,
			PAGE_SIZE, TRUE) != KERN_SUCCESS
	    || vm_allocate(probe_task->map, (vm_offset_t *) &rf_code_va,
			   PAGE_SIZE, TRUE) != KERN_SUCCESS
	    || !refused_fault_touch(probe_task->map, rf_data_va)
	    || !refused_fault_touch(probe_task->map, rf_code_va)) {
		printf("refused_fault_test: NOT ASKED — could not make the "
		       "task's pages resident\n");
		return;
	}

	/* The code page's bytes, written through the direct map. */
	frame = pmap_extract(vm_map_pmap(probe_task->map), rf_code_va);
	if (frame == 0) {
		printf("refused_fault_test: NOT ASKED — the code page did not "
		       "stay resident\n");
		return;
	}
	for (i = 0; i < sizeof refused_fault_code; i++)
		((volatile uint8_t *)(uintptr_t) phys_to_direct(frame))[i] =
			refused_fault_code[i];

	for (i = 0; i < (unsigned) NCPUS; i++) {
		uint64_t	start;
		uint64_t	budget = tsc_hz() * REFUSED_FAULT_WAIT_S;

		if (!machine_slot[i].is_cpu || !machine_slot[i].running)
			continue;

		rf_store_faulted = rf_fetch_faulted = rf_done = 0;
		rf_cpu = (int) i;
		if (kernel_thread(probe_task, refused_fault_probe, (char *) 0)
		    == THREAD_NULL) {
			printf("refused_fault_test: NOT ASKED — no probe thread "
			       "for cpu %u\n", i);
			continue;
		}
		rf_go = 1;

		start = rdtsc_ordered();
		while (!rf_done && rdtsc_ordered() - start < budget)
			cpu_pause();
		rf_go = 0;

		if (!rf_done) {
			printf("refused_fault_test: WRONG — [cpu %u] the probe "
			       "did not return in %d s: a fault the processor "
			       "refused was retried for ever (#750)\n",
			       i, REFUSED_FAULT_WAIT_S);
			return;	/* one looping processor is proof; stop here */
		}

		tested++;
		if (!pmap_smap_on && (read_cr4() & CR4_SMEP) == 0) {
			printf("refused_fault_test: [cpu %u] NOT ASKED — "
			       "neither SMAP nor SMEP on\n", i);
			continue;
		}
		asked++;
		if (pmap_smap_on && !rf_store_faulted) {
			printf("refused_fault_test: [cpu %u] WRONG — SMAP is on "
			       "and the store to the task's page was not "
			       "refused\n", i);
			wrong++;
		}
		if ((read_cr4() & CR4_SMEP) != 0 && !rf_fetch_faulted) {
			printf("refused_fault_test: [cpu %u] WRONG — SMEP is on "
			       "and the fetch from the task's page was not "
			       "refused\n", i);
			wrong++;
		}
		if (!wrong)
			printf("refused_fault_test: [cpu %u] a refused fault on "
			       "a present page was reported, not retried%s%s\n",
			       i, pmap_smap_on ? " (SMAP)" : "",
			       (read_cr4() & CR4_SMEP) ? " (SMEP)" : "");
	}

	if (wrong)
		printf("refused_fault_test: WRONG — %u processor(s) wrong of "
		       "%u tested (#750)\n", wrong, tested);
	else if (asked == 0)
		printf("refused_fault_test: NOT ASKED — no processor had SMAP "
		       "or SMEP on (#750)\n");
	else
		printf("refused_fault_test: PASS — a fault the processor "
		       "refused on a task's present page was reported on %u "
		       "processor(s), not retried (#750)\n", asked);
}
