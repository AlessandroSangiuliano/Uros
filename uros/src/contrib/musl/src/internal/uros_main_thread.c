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
 * These binaries carry no PT_TLS segment, so __init_tls skips its phdr
 * walk (AT_PHNUM==0) and computes the no-TLS-segment tls_size itself —
 * a zeroed auxv is sufficient.  When dynamic linking / real __thread
 * arrives (Phase 7), crt must instead pass a true auxv with AT_PHDR/
 * AT_PHENT/AT_PHNUM (now emitted by exec_server, #248).
 */

#include <elf.h>
#include "pthread_impl.h"
#include "libc.h"

/* Provided by libposix-uros (#252); weak so musl still links for the
 * Phase 1 host-only smoke that doesn't pull in the archive. */
extern void __uros_signals_init(void) __attribute__((__weak__));

void __uros_libc_init(void)
{
	/* Minimal synthetic auxv.  Indexed up to AT_RANDOM; everything
	 * not set stays zero — notably AT_PHNUM, so __init_tls takes its
	 * no-PT_TLS branch. */
	size_t aux[AT_RANDOM + 1] = { 0 };
	aux[AT_PAGESZ] = 4096;

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

	__init_tls(aux);
	__init_ssp((void *)aux[AT_RANDOM]);

	if (__uros_signals_init)
		__uros_signals_init();
}
