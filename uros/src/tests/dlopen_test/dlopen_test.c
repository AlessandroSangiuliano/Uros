/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * dlopen_test.c — end-to-end validation of musl dlfcn on Uros
 * (#234 Phase 7 increment 4).
 *
 * Dynamic PIE image like hello_dyn; running it exercises the
 * remaining piece of the dynamic-linker story:
 *
 *   ld-musl already brought us up against libc.so (proven by
 *   hello_dyn).  Here we call into dlopen(), which makes ld-musl
 *   open a *second* shared object via the standard file path —
 *   "/lib/libfoo.so" — through musl's open()/mmap() syscall
 *   backends, which on Uros translate to vfs.open + vm_map of the
 *   ELF segments.  Then dlsym() walks the loaded object's hash
 *   table and returns the address of foo_answer; calling it must
 *   return FOO_ANSWER_MAGIC.
 *
 * As with hello_dyn, we announce via the mach_print SYSENTER trap
 * so the smoke test does not depend on stdout being wired.  We
 * compare against a constant printed in plain hex so the result
 * is unambiguous in the serial log.
 *
 * Current status (2026-08-18): PASSes.  The whole chain runs --
 * vfs_open on /lib/libfoo.so, mmap2 against an ext_server memory
 * object, the fault, data_supply, and a call into the mapped code
 * that returns FOO_ANSWER_MAGIC.
 *
 * ⚠️ It was written expecting to fail "until file-backed mmap lands",
 * and file-backed mmap landed in #276 while this went on dying -- at
 * the first message of the pager protocol, because ext_server did not
 * ask for the seqno trailer libfspager's -DSEQNOS stubs read.  So for
 * a while the comment and the log agreed on the symptom and were both
 * wrong about the cause, which is exactly how a regression target
 * stops being one.  It is a regression target again now: it fails if
 * anything in that chain stops working, and it says so.
 */

#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

/* Must match libfoo/libfoo.c. */
#define FOO_ANSWER_MAGIC  0x46010003u

/* mach_print kernel trap (SYSENTER, PC-relative resume — PIC-safe). */
static __attribute__((naked, noinline)) void
mach_print(const char *s)
{
    (void)s;
    __asm__ volatile (
        "movl $-14, %%eax\n\t"
        "movl %%esp, %%ecx\n\t"
        "call 1f\n"
        "1:\n\t"
        "popl %%edx\n\t"
        "addl $(2f - 1b), %%edx\n\t"
        ".byte 0x0f, 0x34\n"
        "2:\n\t"
        "ret\n"
        ::: "eax", "ecx", "edx", "memory"
    );
}

/* Minimal "%08x" without bringing in printf — keeps the result line
 * readable even if libc's stdio path is broken. */
static void
print_hex32(const char *label, uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[64];
    size_t i = 0;
    while (label[i] && i < sizeof(buf) - 12) {
        buf[i] = label[i];
        i++;
    }
    buf[i++] = '0';
    buf[i++] = 'x';
    for (int s = 28; s >= 0; s -= 4)
        buf[i++] = hex[(v >> s) & 0xf];
    buf[i++] = '\n';
    buf[i]   = '\0';
    mach_print(buf);
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    mach_print("dlopen_test: starting\n");

    void *h = dlopen("/lib/libfoo.so", RTLD_NOW);
    if (h == NULL) {
        /* dlerror() may itself drag in stdio paths; keep the message
         * static so we always print *something*. */
        mach_print("dlopen_test: FAIL dlopen(/lib/libfoo.so) returned NULL\n");
        const char *err = dlerror();
        if (err)
            mach_print(err);
        for (;;) { }
    }
    mach_print("dlopen_test: dlopen OK\n");

    uint32_t (*foo_answer)(void) = (uint32_t (*)(void))dlsym(h, "foo_answer");
    if (foo_answer == NULL) {
        mach_print("dlopen_test: FAIL dlsym(foo_answer) returned NULL\n");
        for (;;) { }
    }
    mach_print("dlopen_test: dlsym OK\n");

    uint32_t r = foo_answer();
    print_hex32("dlopen_test: foo_answer() = ", r);

    if (r == FOO_ANSWER_MAGIC)
        mach_print("dlopen_test: PASS — dlopen end-to-end works\n");
    else
        mach_print("dlopen_test: FAIL — magic mismatch\n");

    /* Don't bother with dlclose for a smoke test; parent task_terminates us. */
    for (;;) { }
    return 0;
}
