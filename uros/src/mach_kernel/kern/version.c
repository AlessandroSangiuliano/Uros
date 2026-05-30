/*
 * Kernel version string and build identification.
 *
 * UrMach is the Uros microkernel.  The numeric version + string come from
 * <mach/urmach_version.h> (BSD-style MAJOR/MINOR/PATCH).  The historical
 * OSFMK / MkLinux numbering (conf/version.*) is heritage; UrMach tracks
 * its own.
 */

#include <mach/urmach_version.h>

/* Kernel boot banner — printed once by model_dep.c at machine_startup. */
const char version[] = URMACH_VERSION_STRING " \xE2\x80\x94 Uros microkernel\n";

/* These symbols are normally provided by the linker */
/* We define them here as placeholders - they should be
 * properly defined in the linker script */

/* Dummy definitions - actual values set by linker */
extern char _etext[], _edata[], _end[];

/* Compatibility aliases */
char *etext = (char *)0;
char *edata = (char *)0; 
char *end = (char *)0;

/* Master CPU identifier (declared extern in cpu_number.h) */
int master_cpu = 0;

/* Prof queue (declared extern in profile.h, MACH_PROF disabled) */
#include <kern/queue.h>
mpqueue_head_t prof_queue;
