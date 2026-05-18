/*
 * ssp_module_stub.c — Stack protector support for .so modules.
 *
 * GCC emits calls to __stack_chk_fail_local (a hidden/local symbol)
 * in PIC code compiled with -fstack-protector.  This stub must be
 * linked into each .so module.  The __stack_chk_guard variable is
 * resolved at load time from the host server via libdl.
 */

extern unsigned long __stack_chk_guard;

void __attribute__((noreturn, visibility("hidden")))
__stack_chk_fail_local(void)
{
	for (;;)
		;
}
