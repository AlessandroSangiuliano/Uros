/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The load -t needs (#705).
 *
 * -t (#688, entry 53) asks one question of a boot, and it needs two events in
 * this order (x86_64/time/clock_event.c): a processor B whose tick lands in a
 * task's code in ring 3, which the tick holds a fifth of a second, and while B
 * is held another processor A whose tick lands inside the kernel on a task's
 * thread, which panics there.  The bundle's programs gave both only by chance.
 * On OMEGA under TCG a boot takes about ten seconds, and 5 boots of 16 ended
 * before -t had asked: no panic, and the merge test said WRONG.
 *
 * So when the kernel's command line has -t, this task makes both certain: one
 * thread that never leaves ring 3, and two that hardly leave the kernel, each
 * asking it for its own task's basic information.  The spinner's processor is
 * B at its first tick; a caller's processor is A at the first tick of the hold
 * that finds it inside its call.  On any other boot it says so and ends.
 *
 * The command line is the kernel's, from host_get_boot_info(), which needs the
 * master host port: the line in bootstrap.conf carries -H.  It is read the way
 * boot_flag() reads it (x86_64/boot/bootarg.c): a word that starts with a dash
 * is a word of flags, and any other word is not.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <mach/mach_host.h>
#include <mach/bootstrap.h>
#include <mach/host_info.h>
#include <mach/task_info.h>
#include <pthread.h>

#include <stdio.h>
#include <string.h>

#include <sa_mach.h>		/* printf_init */

/*
 * How long the load runs before it says that -t did not ask.  -t holds B a
 * fifth of a second and rests a second (at 4 GHz; a third longer at 3), so
 * thirty seconds are some twenty holds, each needing one tick of A's in a call.
 */
#define	LOAD_SECONDS	30

/* boot_flag(), on the copy host_get_boot_info() gives. */
static int
has_flag(const char *p, char c)
{
	int	in_flag_word = 0;

	for (; *p != '\0'; p++) {
		if (*p == ' ' || *p == '\t') {
			in_flag_word = 0;
			continue;
		}
		if (!in_flag_word) {
			in_flag_word = (*p == '-') ? 1 : -1;
			continue;
		}
		if (in_flag_word == 1 && *p == c)
			return 1;
	}
	return 0;
}

/* B: never leaves ring 3. */
static void *
spinner(void *arg)
{
	(void) arg;
	for (;;)
		__asm__ __volatile__("pause");
	return 0;
}

/* A: inside the kernel for all but the stub's packing of each call. */
static void *
caller(void *arg)
{
	task_basic_info_data_t	info;
	mach_msg_type_number_t	count;
	mach_port_t		self = mach_task_self();

	(void) arg;
	for (;;) {
		count = TASK_BASIC_INFO_COUNT;
		(void) task_info(self, TASK_BASIC_INFO, (task_info_t) &info,
				 &count);
	}
	return 0;
}

int
main(void)
{
	mach_port_t		host = MACH_PORT_NULL, device = MACH_PORT_NULL;
	mach_port_t		wired = MACH_PORT_NULL, paged = MACH_PORT_NULL;
	mach_port_t		security = MACH_PORT_NULL;
	static kernel_boot_info_t line;
	pthread_t		t;
	kern_return_t		kr;
	int			i;

	/*
	 * The first line through mach_print, which needs no port, like
	 * boot_ports_test's: run-x86_64.sh waits on it.
	 */
	mach_print("taskpanic_load: started\n");
	kr = bootstrap_ports(bootstrap_port, &host, &device, &wired, &paged,
			     &security);
	printf_init(device);

	if (kr != KERN_SUCCESS || host == MACH_PORT_NULL) {
		printf("taskpanic_load: WRONG — bootstrap_ports() answered kr=%d "
		       "and the host port %s, and its line in bootstrap.conf "
		       "carries -H: nothing to read the command line with "
		       "(#705)\n", (int) kr,
		       host != MACH_PORT_NULL ? "given" : "null");
		printf("taskpanic_load: no load\n");
		return 1;
	}
	kr = host_get_boot_info(host, line);
	if (kr != KERN_SUCCESS) {
		printf("taskpanic_load: WRONG — host_get_boot_info() answered "
		       "kr=%d on the master host port (#705)\n", (int) kr);
		printf("taskpanic_load: no load\n");
		return 1;
	}
	if (!has_flag(line, 't')) {
		printf("taskpanic_load: no -t on the kernel's command line "
		       "\"%s\": no load\n", line);
		return 0;
	}

	printf("taskpanic_load: -t on the kernel's command line \"%s\": one "
	       "thread in ring 3 and two in the kernel until it asks (#705)\n",
	       line);
	if (pthread_create(&t, 0, spinner, 0) != 0)
		printf("taskpanic_load: WRONG — the thread for ring 3 was not "
		       "created (#705)\n");
	for (i = 0; i < 2; i++)
		if (pthread_create(&t, 0, caller, 0) != 0)
			printf("taskpanic_load: WRONG — a thread for the kernel "
			       "was not created (#705)\n");

	(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
			     LOAD_SECONDS * 1000);
	printf("taskpanic_load: %d s of load and -t did not ask (#705)\n",
	       LOAD_SECONDS);
	printf("taskpanic_load: no load\n");
	return 0;
}
