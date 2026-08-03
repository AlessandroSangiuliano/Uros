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

/*
 * etext, edata and end are the linker's, and are declared where they are
 * used -- `extern char etext;' and so on, taking the address.
 *
 * They used to be defined here as `char *etext = (char *)0;', described as
 * placeholders "until the linker script defines them properly".  Two of the
 * three already were: a linker-script assignment overrides an object file's
 * definition, so edata and end came out right and these definitions were dead.
 * etext was not assigned by the i386 script, so this one won -- and every
 * reader that said `extern char etext;' was looking at a null pointer variable
 * in BSS while believing it had the end of the text segment.
 *
 * Two incompatible declarations of one name in different translation units:
 * the C language compares nothing across them, and the linker sees one symbol
 * (#448).  Fixed where it belonged, in the two linker scripts (#453).
 */

/*
 * master_cpu used to be defined here, next to etext/edata/end, because
 * something needed a definition and this file had no opinion.  It is data
 * about the machine -- which processor the firmware started -- so it now
 * lives with each machine: i386/AT386/model_dep.c and x86_64/cpu/model.c.
 *
 * It collided the moment a second machine claimed it, which is the first time
 * anything compared the two (#453).
 */

/* Prof queue (declared extern in profile.h, MACH_PROF disabled) */
#include <kern/queue.h>
mpqueue_head_t prof_queue;
