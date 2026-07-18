/*
 * power_save.h - Power save configuration.
 * POWER_SAVE=1 enables the #357 idle HLT (i386/AT386/power_save.c);
 * the -S boot flag reverts to the legacy always-poll idle at runtime.
 */
#ifndef _POWER_SAVE_H_
#define _POWER_SAVE_H_
#define POWER_SAVE 1

#ifndef __ASSEMBLER__
extern void	machine_idle(int mycpu);	/* one dry poll pass */
extern void	machine_idle_exit(int mycpu);	/* idle stint ended */
extern void	machine_idle_wake(int cpu);	/* doorbell if halted */
extern int	sched_idle_hlt;			/* -S clears */
#endif

#endif
