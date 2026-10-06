/*
 * power_save.h - Power save configuration.
 * POWER_SAVE=1 enables the #357 idle HLT (i386/AT386/power_save.c);
 * the -S boot flag reverts to the legacy always-poll idle at runtime.
 */
#ifndef _POWER_SAVE_H_
#define _POWER_SAVE_H_
#define POWER_SAVE 1

#ifndef __ASSEMBLER__
#include <i386/ipl.h>			/* #526: SPL0 */

/* #526: the level the idle loop waits at; see x86_64/power_save.h. */
#define	MACHINE_IDLE_SPL	SPL0

/* #526: x86-64 records an idle loop that found its level raised; i386 does
 * not (yet), and says nothing. */
#define	machine_idle_found_level(level)	((void) (level))

extern void	machine_idle(int mycpu);	/* one dry poll pass */
extern void	machine_idle_exit(int mycpu);	/* idle stint ended */
extern void	machine_idle_wake(int cpu);	/* doorbell if halted */
extern int	sched_idle_hlt;			/* -S clears */
#endif

#endif
