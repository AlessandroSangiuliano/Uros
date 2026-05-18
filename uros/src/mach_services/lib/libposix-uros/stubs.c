/*
 * libposix-uros — compatibility stubs that bridge libmach_core's
 * Mach-personality crt0 to musl's libc.
 *
 * init_stack_guard()
 *   libmach's stack_protector.c is excluded from libmach_core (its
 *   __stack_chk_fail collides with musl's strong definition).  crt0.c
 *   still calls init_stack_guard() unconditionally though, so we
 *   provide a no-op landing pad.  musl owns SSP from here on; in
 *   Phase 3 the guard stays at 0 (overrun detection effectively off
 *   for musl-linked tasks) and Phase 6 wires up real seeding via
 *   __init_ssp() once we have pthread support.
 *
 * __stack_chk_fail_local()
 *   Likewise emitted by gcc into PIE binaries as a hidden symbol;
 *   musl provides it as a weak alias to __stack_chk_fail, but only
 *   inside the libc archive — code linked against musl from outside
 *   may still need a definition.  Forward to musl.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

extern void __stack_chk_fail(void);

void init_stack_guard(void)
{
    /* No-op: SSP is handled by musl when -fstack-protector is on.
     * hello_server (Phase 3 pilot) is built with -fno-stack-protector;
     * other migrated servers can opt in once Phase 6 lands. */
}
