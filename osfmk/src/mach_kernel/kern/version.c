/*
 * Kernel version string and build identification
 * Auto-generated for CMake build
 */

/* Kernel version string */
const char version[] = "OSFMK 1.1 CMake Build";

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
