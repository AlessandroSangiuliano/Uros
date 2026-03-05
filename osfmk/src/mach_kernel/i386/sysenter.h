/*
 * SYSENTER/SYSEXIT fast system call support (Feature #44).
 *
 * Author: Slex (Alessandro Sangiuliano) <alex22_7@hotmail.com>
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
