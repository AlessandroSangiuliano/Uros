/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * SYSENTER/SYSEXIT fast system call support (Feature #44).
 */

#include <cpus.h>
#include <i386/sysenter.h>
#include <i386/proc_reg.h>
#include <i386/cpuid.h>
#include <i386/seg.h>
#include <i386/cpu_number.h>
#include <kern/misc_protos.h>

/*
 * Global flag read by the assembly fast-return path in locore.S.
 * Zero until sysenter_init() succeeds on a CPU that supports SEP.
 */
int	sysenter_available = 0;

/*
 * The SYSENTER entry point defined in locore.S.
 */
extern void	sysenter_entry(void);

/*
 * #348: per-CPU SYSENTER trampoline slot.
 *
 * IA32_SYSENTER_ESP is programmed ONCE per CPU (at boot / AP bring-up) to point
 * at this CPU's .kstack field, instead of being re-written with a WRMSR on every
 * context switch.  On entry, SYSENTER lands with %esp == &.kstack and the stub
 * (locore.S sysenter_entry) does a single `movl (%esp),%esp` to switch to the
 * current thread's real kernel stack.  The per-switch update then becomes a
 * plain memory store into .kstack (sysenter_update_esp) -- a WRMSR(SYSENTER_ESP)
 * costs ~144 cyc on real hardware (~346 under KVM, vm-exit); the store is ~3.
 *
 * .scratch sits at LOWER addresses than .kstack on purpose.  An NMI or #MC taken
 * in the one-instruction window where %esp == &.kstack (interrupts are masked,
 * but NMI/#MC are not) pushes its hardware frame downward, into .scratch, and
 * never overwrites .kstack.  t_nmi pushes ~68 bytes before switching to its own
 * per-CPU stack (locore.S), so 256 bytes of scratch is a wide margin.
 */
#define	SYSENTER_STUB_SCRATCH	64		/* words below the kstack slot */

static struct sysenter_stub {
	unsigned int	scratch[SYSENTER_STUB_SCRATCH];
	unsigned int	kstack;			/* %esp points here after SYSENTER */
} sysenter_stub[NCPUS] __attribute__((aligned(16)));

/*
 * Program all three SYSENTER MSRs for the calling CPU.
 *
 *   IA32_SYSENTER_CS  (0x174) — GDT selector for the kernel code
 *                                segment used on SYSENTER.  The CPU
 *                                auto-derives SS, user CS, and user SS
 *                                as offsets +8, +16, +24 from this value.
 *   IA32_SYSENTER_EIP (0x176) — Kernel entry point (sysenter_entry).
 *   IA32_SYSENTER_ESP (0x175) — Kernel stack pointer.  #348: set ONCE per CPU
 *                                to this CPU's sysenter_stub.kstack slot; the
 *                                entry stub switches to the thread's real
 *                                kernel stack, so this MSR is no longer touched
 *                                on context switch.
 *
 * SYSENTER hardcodes CS.base=0 and SS.base=0 in the descriptor
 * cache, regardless of the GDT entry values.  With the flat memory
 * model (all segment bases=0), this matches the kernel segments
 * exactly — no linear-to-segmented conversion is needed.
 * The MSR values are plain kernel virtual addresses.
 */
static void
sysenter_setup_msrs(unsigned int sysenter_esp)
{
	wrmsr(MSR_IA32_SYSENTER_CS,  SYSENTER_CS, 0);
	wrmsr(MSR_IA32_SYSENTER_EIP,
	      (unsigned int)&sysenter_entry, 0);
	wrmsr(MSR_IA32_SYSENTER_ESP,
	      sysenter_esp, 0);
}

/*
 * Initialise SYSENTER/SYSEXIT for the boot CPU.
 */
void
sysenter_init(void)
{
	extern unsigned int	cpuid_feature;

	if (!(cpuid_feature & CPUID_FEATURE_SEP)) {
		printf("sysenter: CPU does not support SYSENTER/SYSEXIT\n");
		return;
	}

	/*
	 * The Pentium Pro (family 6, model 1, stepping < 3) has a
	 * buggy SEP implementation.  Disable fast syscalls on those
	 * chips, just as Linux does.
	 */
	{
		extern unsigned char cpuid_family, cpuid_model, cpuid_stepping;

		if (cpuid_family == 6 && cpuid_model < 3 &&
		    cpuid_stepping < 3) {
			printf("sysenter: disabled on buggy Pentium Pro\n");
			return;
		}
	}

	/*
	 * #348: point IA32_SYSENTER_ESP at the BSP's (cpu 0) trampoline slot.
	 * The .kstack value is filled in by the first context switch
	 * (act_machine_switch_pcb -> sysenter_update_esp) before any user
	 * thread runs, exactly like the old per-switch WRMSR.
	 */
	sysenter_setup_msrs((unsigned int)&sysenter_stub[0].kstack);

	sysenter_available = 1;

	printf("sysenter: fast syscall entry enabled "
	       "(SYSENTER_CS=0x%x EIP=0x%x CR0=0x%x)\n",
	       SYSENTER_CS,
	       (unsigned int)&sysenter_entry,
	       get_cr0());
}

/*
 * Record the current thread's kernel stack top for the SYSENTER entry stub.
 * Called from act_machine_switch_pcb() alongside the TSS.esp0 update.
 *
 * #348: this is now a plain store into the current CPU's trampoline slot
 * (read by `movl (%esp),%esp` in sysenter_entry) instead of a ~144 cyc WRMSR.
 * Called with preemption disabled, so cpu_number() is stable.
 */
void
sysenter_update_esp(unsigned int new_esp)
{
	if (sysenter_available)
		sysenter_stub[cpu_number()].kstack = new_esp;
}

/*
 * Program the SYSENTER MSRs for a secondary (AP) CPU.  Runs ON the AP
 * (mp_desc_init, called from the AP trampoline in start.S), so the WRMSRs
 * target this AP's own MSRs.  IA32_SYSENTER_ESP points at the AP's own
 * trampoline slot; mycpu is the AP's dense CPU number.
 */
void
sysenter_ap_init(int mycpu)
{
	extern unsigned int	cpuid_feature;

	if (!(cpuid_feature & CPUID_FEATURE_SEP))
		return;

	sysenter_setup_msrs((unsigned int)&sysenter_stub[mycpu].kstack);
}
