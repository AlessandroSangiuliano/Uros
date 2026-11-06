/*
 * Uros patch (#259): main-thread init for musl-linked Uros tasks.
 *
 * Bootstrap-loaded servers don't start via __libc_start_main and have no
 * Linux-style stack carrying argc/argv/envp/auxv (libmach_core's crt0
 * fetches args over MIG and jumps straight to main()).  So we can't run
 * the normal __init_libc.  Instead __uros_libc_init() — called first
 * thing in main() — synthesizes the minimal auxv that __init_tls needs
 * and drives musl's real init path:
 *
 *   __init_tls(aux)   builds the TCB in musl's builtin_tls, then
 *                     __init_tp -> __set_thread_area installs the LDT
 *                     (i386 stub tail-calls __uros_set_thread_area_tp)
 *                     and sets libc.can_do_threads, the circular thread
 *                     list, tid, robust_list, dtv, etc.
 *   __init_ssp(rnd)   seeds __stack_chk_guard and the per-thread canary.
 *
 * This replaces the hand-rolled __uros_main_thread TCB + manual
 * tls_size/tls_align/list seeding that earlier phases used as a bridge
 * before real TLS landed.
 *
 * #414: __init_tls finds a program's __thread variables through its program
 * headers, and a task bootstrap loaded has no auxv to say where they are.  The
 * linker does: __ehdr_start is the ELF header of this image, in the first
 * segment it loads.  So the auxv below carries the three entries that walk
 * reads, and a program with no PT_TLS walks past nothing, as before.  Without
 * them a __thread variable read its initial value from whatever lay under the
 * thread pointer, in silence.
 */

#include <elf.h>
#include <limits.h>
#include "pthread_impl.h"
#include "libc.h"

#if ULONG_MAX == 0xffffffff
typedef Elf32_Ehdr Ehdr;
#else
typedef Elf64_Ehdr Ehdr;
#endif

/* Weak because a linker defines it only when the headers are in a segment. */
extern const Ehdr __ehdr_start __attribute__((__weak__, __visibility__("hidden")));

/* Provided by libposix-uros (#252); weak so musl still links for the
 * Phase 1 host-only smoke that doesn't pull in the archive. */
extern void __uros_signals_init(void) __attribute__((__weak__));

/* Provided by libposix-uros (#262 step 3): absorb the fd table handed
 * over by execve, if this task was spawned with one.  Weak so musl still
 * links for smokes that don't pull in the archive. */
extern void __uros_absorb_inherited_fds(void) __attribute__((__weak__));

/*
 * #414: where the calling thread keeps its MIG reply port.  libmach asks for it
 * through mig_init(), and the structure is here because libc.so cannot have
 * __thread variables of its own: musl's dynamic linker gives its own image no
 * TLS block.
 */
void *__uros_mach_thread_state(void)
{
	return &__pthread_self()->uros_mach_state;
}

void __uros_libc_init(void)
{
	/* Minimal synthetic auxv.  Indexed up to AT_RANDOM; everything
	 * not set stays zero. */
	size_t aux[AT_RANDOM + 1] = { 0 };
	aux[AT_PAGESZ] = 4096;
	if (&__ehdr_start) {
		aux[AT_PHDR]  = (size_t)&__ehdr_start + __ehdr_start.e_phoff;
		aux[AT_PHENT] = __ehdr_start.e_phentsize;
		aux[AT_PHNUM] = __ehdr_start.e_phnum;
	}

	/* 16 bytes of canary entropy from the cycle counter — boot- and
	 * call-site-unique.  Not cryptographic (that waits on a kernel
	 * entropy source, #248 item 4) but far better than the fixed 0
	 * the SSP guard sat at while we bypassed __init_ssp. */
	static size_t canary_seed[4];
	unsigned int lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	canary_seed[0] = lo;
	canary_seed[1] = hi;
	canary_seed[2] = lo ^ 0x9e3779b9u;
	canary_seed[3] = hi ^ (size_t)&canary_seed;
	aux[AT_RANDOM] = (size_t)canary_seed;

	/*
	 * #414: libc.auxv, for what reads it after this.  It is a list of
	 * pairs ending in a zero, as Linux leaves it on the stack, and aux[]
	 * above is a table by type, which is what __init_tls wants.  mallocng
	 * walks it for AT_RANDOM on the first malloc(); left NULL, that was a
	 * read of address 0, which went unnoticed on i386 for as long as page
	 * zero was mapped.
	 */
	static size_t auxv[2 * (AT_RANDOM + 1)];
	size_t type, used = 0;

	for (type = 1; type <= AT_RANDOM; type++)
		if (aux[type]) {
			auxv[used++] = type;
			auxv[used++] = aux[type];
		}
	libc.auxv = auxv;

	__init_tls(aux);
	__init_ssp((void *)aux[AT_RANDOM]);

	if (__uros_signals_init)
		__uros_signals_init();

	/* If we were execve'd with an inherited fd table, rebuild it now —
	 * before main() so the new image's first open/read sees the fds. */
	if (__uros_absorb_inherited_fds)
		__uros_absorb_inherited_fds();
}
