/*
 * Uros patch (#251 + #256): main-thread TCB used by musl-linked Uros
 * tasks.  Phase 3 introduced a single shared pthread struct
 * (__uros_main_thread) to bridge the gap before real TLS landed.
 * Phase 6a (#256) keeps that struct but turns it into a proper
 * TCB layout: pthread followed by a TLS "self" word, with the LDT
 * descriptor installed by libposix-uros pointing at the self word.
 *
 *   layout (low → high addr)
 *     +-----------------+
 *     |  pthread struct |   ← __uros_main_thread.thr
 *     +-----------------+
 *     |   tp_word       |   ← LDT base, holds &tp_word so %gs:0 = TP
 *     +-----------------+
 *
 *   __get_tp()        = %gs:0          = &tp_word
 *   __pthread_self()  = TP - sizeof(struct pthread) = &thr  ✓
 *
 * Note: __uros_main_thread used to be `struct pthread`; we keep
 * the symbol but it's now a longer composite.  External references
 * to it stop at the `.thr` field via standard struct member access.
 *
 * SSP NOTE: __stack_chk_guard stays at 0 (we still don't call
 * __init_ssp()).  hello_server compiles with -fno-stack-protector
 * to make this explicit; Phase 6b/c wires up real canary seeding.
 */

#include "pthread_impl.h"
#include "libc.h"

struct __uros_main_tcb {
	struct pthread thr;
	uintptr_t      tp_word;
};
struct __uros_main_tcb __uros_main_thread;

/*
 * Phase 4 / 6a entry points provided by libposix-uros.  Declared weak
 * so musl still builds standalone for the Phase 1 host smoke (no
 * libposix-uros archive linked).
 */
extern void __uros_signals_init(void)  __attribute__((__weak__));
extern void __uros_main_tls_init(void) __attribute__((__weak__));

/*
 * Tiny accessor that hands libposix-uros (which doesn't have the
 * `struct pthread` type) the address of the tp_word field — used to
 * install the main thread's LDT-based TLS descriptor.
 */
void *__uros_main_tcb_tp_addr(void)
{
	return &__uros_main_thread.tp_word;
}

void __uros_libc_init(void)
{
	__uros_main_thread.thr.self = &__uros_main_thread.thr;

	/*
	 * Phase 6a (#256): musl's pthread_create gates on
	 * libc.can_do_threads (set by __init_tls on Linux).  We bypass
	 * __init_libc and friends, so set it here once the synthetic
	 * main thread is wired up.
	 */
	libc.can_do_threads = 1;

	/* Phase 6a (#256): install an LDT descriptor for the main thread
	 * with base = &tp_word and load it into %gs.  After this call,
	 * %gs:0 reads tp_word, which __uros_main_tls_init sets to its
	 * own address — wires up the standard __pthread_self() arithmetic. */
	if (__uros_main_tls_init)
		__uros_main_tls_init();

	if (__uros_signals_init)
		__uros_signals_init();
}
