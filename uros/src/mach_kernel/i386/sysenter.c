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

#include <i386/sysenter.h>
#include <i386/proc_reg.h>
#include <i386/cpuid.h>
#include <i386/seg.h>
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
 * Program all three SYSENTER MSRs for the calling CPU.
 *
 *   IA32_SYSENTER_CS  (0x174) — GDT selector for the kernel code
 *                                segment used on SYSENTER.  The CPU
 *                                auto-derives SS, user CS, and user SS
 *                                as offsets +8, +16, +24 from this value.
 *   IA32_SYSENTER_EIP (0x176) — Kernel entry point (sysenter_entry).
 *   IA32_SYSENTER_ESP (0x175) — Kernel stack pointer.  We set this to
 *                                the current thread's PCB stack top,
 *                                updated on every context switch in
 *                                act_machine_switch_pcb().
 *
 * SYSENTER hardcodes CS.base=0 and SS.base=0 in the descriptor
 * cache, regardless of the GDT entry values.  With the flat memory
 * model (all segment bases=0), this matches the kernel segments
 * exactly — no linear-to-segmented conversion is needed.
 * The MSR values are plain kernel virtual addresses.
 */
static void
sysenter_setup_msrs(unsigned int pcb_stack_top)
{
	wrmsr(MSR_IA32_SYSENTER_CS,  SYSENTER_CS, 0);
	wrmsr(MSR_IA32_SYSENTER_EIP,
	      (unsigned int)&sysenter_entry, 0);
	wrmsr(MSR_IA32_SYSENTER_ESP,
	      pcb_stack_top, 0);
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
	 * Program the MSRs.  The initial ESP value is zero — it will be
	 * set properly on the first context switch (act_machine_switch_pcb).
	 */
	sysenter_setup_msrs(0);

	sysenter_available = 1;

	printf("sysenter: fast syscall entry enabled "
	       "(SYSENTER_CS=0x%x EIP=0x%x CR0=0x%x)\n",
	       SYSENTER_CS,
	       (unsigned int)&sysenter_entry,
	       get_cr0());
}

/*
 * Update IA32_SYSENTER_ESP for the current CPU.
 * Called from act_machine_switch_pcb() alongside TSS.esp0 update.
 */
void
sysenter_update_esp(unsigned int new_esp)
{
	if (sysenter_available)
		wrmsr(MSR_IA32_SYSENTER_ESP, new_esp, 0);
}

/*
 * Program all three SYSENTER MSRs for a secondary CPU.
 */
void
sysenter_ap_init(unsigned int pcb_stack_top)
{
	extern unsigned int	cpuid_feature;

	if (!(cpuid_feature & CPUID_FEATURE_SEP))
		return;

	sysenter_setup_msrs(pcb_stack_top);
}
