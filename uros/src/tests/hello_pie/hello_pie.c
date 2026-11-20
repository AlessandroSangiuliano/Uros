/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * hello_pie -- a position-independent program that names no dynamic linker,
 * which exec_server has to relocate (#963).
 *
 * Its data holds four addresses, each written by the linker as an offset from
 * zero, with an R_X86_64_RELATIVE for the loader: pointer 0 to a string, 1 to
 * a function, 2 to a variable, and 3 to the table itself.  The code finds where each of the four
 * really is by RIP-relative arithmetic, which needs no relocation, and prints
 * both.  Relocated, each pointer holds where its object is.  Not relocated,
 * each holds the linker's number, and its line says WRONG.
 *
 * No library under it, as under hello_exec, and nothing it does to print
 * takes an address out of its data: it says what it found even when nothing
 * was relocated.  Then it parks, for its parent to kill.
 */

#include <stdint.h>

#if !defined(__x86_64__)
#error "hello_pie is for exec_server, which is built for x86-64 only"
#endif

/* ------------------------------------------------------------------ */
/*  The two traps, as hello_exec makes them                             */
/* ------------------------------------------------------------------ */

#define HP_TRAP_BASE	0x100
#define HP_TRAP_PRINT	14
#define HP_TRAP_SWITCH	61

/* SWITCH_OPTION_WAIT, from <mach/thread_switch.h>, which this file cannot
 * include: it has no headers but <stdint.h>. */
#define HP_SWITCH_OPTION_WAIT	2

/*
 * ⚠️ A trap keeps what a function keeps and no more, so every register the C
 * ABI lets a callee destroy is named (hello_exec found r8 come back zero).
 */
static long
hp_trap(long number, long a1, long a2, long a3)
{
    long ret;

    __asm__ volatile ("syscall"
                      : "=a"(ret), "+D"(a1), "+S"(a2), "+d"(a3)
                      : "a"(HP_TRAP_BASE + number)
                      : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static void
mach_print(const char *s)
{
    (void)hp_trap(HP_TRAP_PRINT, (long)s, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  Formatting, with no table of any kind                               */
/* ------------------------------------------------------------------ */

static char *
write_str(char *dst, const char *s)
{
    while (*s)
        *dst++ = *s++;
    return dst;
}

static char *
write_hex(char *dst, uint64_t v)
{
    int i;

    *dst++ = '0';
    *dst++ = 'x';
    for (i = 15; i >= 0; i--) {
        int nib = (int)((v >> (i * 4)) & 0xf);

        *dst++ = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
    return dst;
}

static char *
write_dec(char *dst, unsigned v)
{
    char tmp[12];
    int  n = 0;

    if (v == 0) {
        *dst++ = '0';
        return dst;
    }
    while (v) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n--)
        *dst++ = tmp[n];
    return dst;
}

/* ------------------------------------------------------------------ */
/*  What the table points at, and the table                             */
/* ------------------------------------------------------------------ */

static const char hp_string[] __attribute__((used)) = "hello_pie";

static void __attribute__((used, noinline))
hp_function(void)
{
    __asm__ volatile ("");
}

static int hp_variable __attribute__((used));

/* Each entry is an address in data, so each needs an R_X86_64_RELATIVE. */
static const void *const hp_table[4] __attribute__((used)) = {
    hp_string, (const void *)hp_function, &hp_variable, hp_table,
};

/*
 * Where an object is, by RIP-relative arithmetic.  The instruction carries
 * the distance from itself, which the linker fixed, and no loader touches it.
 */
#define HP_WHERE(sym) ({                                                    \
    const void *where_;                                                     \
    __asm__ ("leaq " #sym "(%%rip), %0" : "=r"(where_));                    \
    where_;                                                                 \
})

extern const char __ehdr_start[] __attribute__((visibility("hidden")));

/*
 * ⚠️ No names for the four on their lines, and the reason is the point of
 * this program: a table of names is addresses in data, and so is the lookup
 * table a compiler may make of a switch that returns them.  Either would need
 * the relocation this program is asking about, to say what it found.
 */

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */

void c_main(void) __attribute__((noreturn, used));

void
__attribute__((noreturn, used, naked))
_start(void)
{
    __asm__ volatile (
        "andq $-16, %%rsp\n\t"    /* 16-byte aligned for the call */
        "call c_main\n\t"
        "ud2\n"
        ::: "memory");
}

void
c_main(void)
{
    const void *want[4];
    char        buf[160];
    char       *w;
    int         i, right = 0;

    want[0] = HP_WHERE(hp_string);
    want[1] = HP_WHERE(hp_function);
    want[2] = HP_WHERE(hp_variable);
    want[3] = HP_WHERE(hp_table);

    w = write_str(buf, "hello_pie: started, its ELF header at ");
    w = write_hex(w, (uint64_t)(uintptr_t)HP_WHERE(__ehdr_start));
    w = write_str(w, "\n");
    *w = '\0';
    mach_print(buf);

    for (i = 0; i < 4; i++) {
        /* Read through volatile: the compiler knows what the linker wrote,
         * and would otherwise compare that instead of what is there. */
        const void *got = ((const void *const volatile *)hp_table)[i];

        w = write_str(buf, "hello_pie: ");
        if (got != want[i])
            w = write_str(w, "WRONG -- ");
        else
            right++;
        w = write_str(w, "pointer ");
        w = write_dec(w, (unsigned)i);
        w = write_str(w, " holds ");
        w = write_hex(w, (uint64_t)(uintptr_t)got);
        w = write_str(w, ", and its object is at ");
        w = write_hex(w, (uint64_t)(uintptr_t)want[i]);
        w = write_str(w, "\n");
        *w = '\0';
        mach_print(buf);
    }

    w = write_str(buf, "hello_pie: ");
    w = write_dec(w, (unsigned)right);
    w = write_str(w, " of 4 pointers relocated\n");
    *w = '\0';
    mach_print(buf);

    /* Parked asleep, as hello_exec, until the parent kills it (#576). */
    for (;;)
        (void)hp_trap(HP_TRAP_SWITCH, 0, HP_SWITCH_OPTION_WAIT, 1000);
}
