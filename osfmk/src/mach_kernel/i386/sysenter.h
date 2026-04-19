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
 *
 * On Pentium II and later processors, the SYSENTER/SYSEXIT pair
 * provides a fast privilege-level transition that avoids the overhead
 * of the call gate (LCALL) and IRET mechanisms used by the original
 * OSF Mach kernel.
 *
 * SYSENTER: ~10 cycles  vs  LCALL through call gate: ~100+ cycles
 * SYSEXIT:  ~10 cycles  vs  IRET:                    ~80+ cycles
 *
 * The three IA32_SYSENTER_* MSRs are programmed once per CPU at boot
 * and IA32_SYSENTER_ESP is updated on every context switch to track
 * the current thread's PCB stack top (kept in sync with TSS.esp0).
 */

#ifndef _I386_SYSENTER_H_
#define _I386_SYSENTER_H_

/*
 * Non-zero after sysenter_init() succeeds.
 * Tested by the assembly return path in locore.S to choose
 * between SYSEXIT (fast) and IRET (legacy) returns.
 */
extern int	sysenter_available;

/*
 * Initialise SYSENTER/SYSEXIT for the boot CPU.
 * Must be called after set_cpu_model() (so that cpuid_feature is valid)
 * and after the GDT has been loaded.
 */
extern void	sysenter_init(void);

/*
 * Program IA32_SYSENTER_ESP to the given PCB stack top address.
 * Called from act_machine_switch_pcb() on every context switch.
 */
extern void	sysenter_update_esp(unsigned int new_esp);

/*
 * Program all three SYSENTER MSRs for a secondary (AP) CPU.
 * Called from mp_desc_init() or the AP startup path.
 */
extern void	sysenter_ap_init(unsigned int pcb_stack_top);

#endif /* _I386_SYSENTER_H_ */
