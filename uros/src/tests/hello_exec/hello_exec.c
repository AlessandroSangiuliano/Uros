/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * hello_exec.c — minimal test binary for exec_server (#228 v0.1.0;
 * extended in v0.4.0 / #236 to also dump the AUXV).
 *
 * Loaded by exec_server, runs in a fresh task with empty IPC space.
 * Cannot use Mach IPC (no ports), only kernel traps:
 *   - mach_print  (trap 14, prints to kernel console)
 *   - mach_null   (trap 15, no-op for cycle counting)
 *
 * Strategy: greet via mach_print, walk the System V ABI initial
 * stack to enumerate the AUXV entries exec_server set up, then loop
 * forever waiting to be torn down by the parent.  The AUXV dump
 * doubles as the acceptance check for #236 — a non-empty list with
 * AT_PAGESZ + AT_RANDOM (always) plus AT_SYSINFO_EHDR + AT_ENTRY
 * (when exec_server v0.4.0 is in place) proves the wire is right.
 *
 * No libc, no libmach, no libpthreads — bare assembly stubs for the
 * two traps + handcrafted hex/decimal formatters.
 */

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Mach kernel-trap stubs                                              */
/* ------------------------------------------------------------------ */

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

static __attribute__((naked, noinline)) void
mach_null(void)
{
    __asm__ volatile (
        "movl $-15, %%eax\n\t"
        "movl %%esp, %%ecx\n\t"
        "call 1f\n"
        "1:\n\t"
        "popl %%edx\n\t"
        "addl $(2f - 1b), %%edx\n\t"
        ".byte 0x0f, 0x34\n"
        "2:\n\t"
        "ret\n"
        ::: "eax", "ecx", "edx"
    );
}

/* ------------------------------------------------------------------ */
/*  String helpers (no libc)                                            */
/* ------------------------------------------------------------------ */

static char *
write_hex32(char *dst, uint32_t v)
{
    int i;
    *dst++ = '0';
    *dst++ = 'x';
    for (i = 7; i >= 0; i--) {
        int nib = (v >> (i * 4)) & 0xf;
        *dst++ = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
    return dst;
}

static char *
write_dec(char *dst, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { *dst++ = '0'; return dst; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) *dst++ = tmp[n];
    return dst;
}

static char *
write_str(char *dst, const char *s)
{
    while (*s) *dst++ = *s++;
    return dst;
}

/* ------------------------------------------------------------------ */
/*  AUXV walker                                                         */
/* ------------------------------------------------------------------ */

#define AT_NULL          0
#define AT_PHDR          3
#define AT_PHENT         4
#define AT_PHNUM         5
#define AT_PAGESZ        6
#define AT_ENTRY         9
#define AT_RANDOM       25
#define AT_SYSINFO_EHDR 33

static const char *
auxv_tag_name(uint32_t tag)
{
    switch (tag) {
    case AT_NULL:          return "AT_NULL";
    case AT_PHDR:          return "AT_PHDR";
    case AT_PHENT:         return "AT_PHENT";
    case AT_PHNUM:         return "AT_PHNUM";
    case AT_PAGESZ:        return "AT_PAGESZ";
    case AT_ENTRY:         return "AT_ENTRY";
    case AT_RANDOM:        return "AT_RANDOM";
    case AT_SYSINFO_EHDR:  return "AT_SYSINFO_EHDR";
    default:               return "AT_?";
    }
}

/*
 * `init_sp` points at the initial stack as exec_server arranged it:
 *   [argc]
 *   [argv[0] ... argv[argc-1]] [NULL]
 *   [envp[0] ... envp[envc-1]] [NULL]
 *   [auxv pairs ... AT_NULL]
 *   [strings + AT_RANDOM payload]
 */
static void
dump_auxv(uint32_t *init_sp)
{
    uint32_t argc = init_sp[0];
    uint32_t *p   = &init_sp[1 + argc + 1];   /* past argv + NULL */
    int n        = 0;
    char buf[128];
    char *w;

    /* Skip envp to its terminating NULL, then step past it. */
    while (*p) p++;
    p++;

    w = buf;
    w = write_str(w, "hello_exec: argc=");
    w = write_dec(w, argc);
    w = write_str(w, "\n");
    *w = 0;
    mach_print(buf);

    while (*p != AT_NULL) {
        uint32_t tag = *p++;
        uint32_t val = *p++;

        w = buf;
        w = write_str(w, "hello_exec: AUXV ");
        w = write_str(w, auxv_tag_name(tag));
        w = write_str(w, " (tag=");
        w = write_dec(w, tag);
        w = write_str(w, ") = ");
        w = write_hex32(w, val);
        w = write_str(w, "\n");
        *w = 0;
        mach_print(buf);
    }

    mach_print("hello_exec: AUXV end\n");
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */

/*
 * _start is naked so we can capture the initial ESP before any
 * prologue mutates it.  The System V ABI hands argc/argv/envp/auxv
 * on the stack; we pass the raw pointer to c_main which walks it.
 */
extern void c_main(uint32_t *init_sp) __attribute__((noreturn, used));

void
__attribute__((noreturn, used, naked))
_start(void)
{
    __asm__ volatile (
        /* ABI: ESP at entry == &argc, 16-byte aligned. */
        "movl %%esp, %%eax\n\t"   /* arg0 = &argc */
        "andl $-16, %%esp\n\t"    /* keep 16-byte align for call */
        "subl $12, %%esp\n\t"     /* reserve so esp+4(arg) is aligned */
        "pushl %%eax\n\t"
        "call c_main\n\t"
        "ud2\n"
        ::: "eax", "memory"
    );
    __builtin_unreachable();
}

void
__attribute__((noreturn, used))
c_main(uint32_t *init_sp)
{
    mach_print("hello_exec: hello from exec_server v0.4.0\n");
    dump_auxv(init_sp);

    /* Loop forever; the parent task_terminates us. */
    for (;;)
        mach_null();
}
