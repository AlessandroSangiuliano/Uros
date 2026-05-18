/*
 * Uros patch (#251 / Phase 3): synthetic main-thread struct for
 * single-threaded musl-linked Uros tasks.
 *
 * musl's __pthread_self() is the bottleneck for everything thread-
 * local (errno, stdio FLOCK, SSP canary, locale).  Until Phase 6
 * brings up real pthreads with set_thread_area, every musl-linked
 * task points its __get_tp() at this single static struct.  Each
 * task owns its own copy because musl-linked binaries are statically
 * linked today.
 *
 * Initialisation contract: __uros_libc_init() must be the very first
 * call inside main() (the libmach_core crt0 hands control to main
 * before any libc code runs, so we can't autoload via a constructor
 * — those need __libc_start_main, which we deliberately bypass).
 *
 * SECURITY NOTE: __stack_chk_guard stays at its default 0 because we
 * do not call musl's __init_ssp().  Compiler-emitted SSP checks
 * therefore compare 0 to 0 and always pass — overrun detection is
 * effectively disabled.  hello_server compiles with
 * -fno-stack-protector to make this explicit.  Phase 6 wires up real
 * canary seeding alongside pthread support.
 */

#include "pthread_impl.h"

struct pthread __uros_main_thread;
unsigned long  __uros_tp;

/*
 * Phase 4 (#252) — once the synthetic TP is wired, kick the signal
 * personality startup.  The function is provided by libposix-uros and
 * declared weak so musl can build standalone without a libposix-uros
 * archive (Phase 1 host-only smoke).  See uros/src/mach_services/lib/
 * libposix-uros/signals.c.
 *
 * The attribute uses __weak__ (not weak): musl's internal
 * features.h #defines `weak` as `__attribute__((__weak__))`, so the
 * shorter spelling collides with the macro inside this TU.
 */
extern void __uros_signals_init(void) __attribute__((__weak__));

void __uros_libc_init(void)
{
	__uros_main_thread.self   = &__uros_main_thread;
	__uros_tp = (unsigned long)((char *)&__uros_main_thread
	                            + sizeof(struct pthread));
	if (__uros_signals_init)
		__uros_signals_init();
}
