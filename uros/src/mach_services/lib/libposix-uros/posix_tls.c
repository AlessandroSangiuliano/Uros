/*
 * libposix-uros — POSIX TLS / set_thread_area over Mach (#256 / 6a).
 *
 * Linux set_thread_area(2) installs a GDT entry for a per-thread
 * memory region; on Uros we install an LDT entry instead via the
 * i386_set_ldt MIG RPC.  LDT is per-thread on Mach, so we always use
 * slot LDTSZ (=4, the first user-available slot) — every thread
 * gets its own descriptor at the same selector value (0x27), no
 * allocation bookkeeping needed.
 *
 * The main thread's TCB is the static `__uros_main_thread` declared
 * in musl's src/internal/uros_main_thread.c (a `struct pthread`
 * followed by a `tp_word`).  __uros_main_tls_init writes the
 * self-reference into tp_word and installs the LDT descriptor so
 * `%gs:0` resolves to tp_word's content, which is tp_word's address —
 * making __pthread_self() (TP - sizeof(pthread)) land on &thr.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <stdint.h>
#include <stddef.h>

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>

#include "mach_i386.h"

/* Match the descriptor format mach_i386.defs expects: the kernel
 * unpacks a struct descriptor (fake_descriptor in seg.h) — offset,
 * lim_or_seg, size_or_wdct, access — as one 64-bit blob. */
struct __uros_descriptor {
    uint32_t offset;        /* base address */
    uint32_t lim_low : 20;  /* limit bits 0..19 */
    uint32_t gran    : 4;   /* SZ_32 | SZ_G = 0xC */
    uint32_t access  : 8;   /* ACC_P|ACC_PL_U|ACC_DATA_W = 0xF2 */
};

/* musl's struct user_desc (Linux ABI for set_thread_area). */
struct __uros_user_desc {
    unsigned int entry_number;
    unsigned int base_addr;
    unsigned int limit;
    unsigned int flags;     /* packed bitfield (seg_32bit/contents/...) */
};

/* LDT slot we always install at — first user-available, per i386 seg.h
 * (LDTSZ = 4).  Selector = (4 << 3) | TI(LDT,0x4) | RPL(3) = 0x27. */
#define UROS_TLS_LDT_SLOT      4
#define UROS_TLS_LDT_SELECTOR  ((UROS_TLS_LDT_SLOT << 3) | 0x7)

static kern_return_t
install_tls(mach_port_t target_thread, unsigned int base)
{
    struct __uros_descriptor desc = {0};
    desc.offset   = base;
    desc.lim_low  = 0xfffff;       /* 4 GiB / 4K-page granularity */
    desc.gran     = 0xC;           /* SZ_32 | SZ_G */
    desc.access   = 0xF2;          /* P|DPL=3|S=1|TYPE=DATA|W=1 */

    return i386_set_ldt(target_thread,
                        UROS_TLS_LDT_SELECTOR /* first_selector */,
                        (descriptor_list_t)&desc,
                        1 /* descriptor count */);
}

/* ------------------------------------------------------------------ */
/* Main-thread TLS install — called from __uros_libc_init             */
/* ------------------------------------------------------------------ */

/* From musl src/internal/uros_main_thread.c */
struct __uros_main_tcb {
    struct __uros_pthread_opaque {
        char raw[0];           /* opaque to the C side; size known via the
                                * sizeof in the linker-provided extern */
    } thr;
    uintptr_t tp_word;
};
extern struct __uros_main_tcb __uros_main_thread;

/*
 * sizeof(struct pthread) — provided by the patched syscall_arch.h
 * extern, via uros_main_thread.c's compile.  We need it to locate the
 * tp_word inside the linker-laid-out struct.  Easier than redeclaring
 * the full pthread layout here is to recompute it: the tp_word lives
 * immediately after the pthread struct, but the struct's size depends
 * on musl's internal layout.  Worst case we link a tiny helper from
 * the musl side to fetch &tp_word directly.
 */
extern void *__uros_main_tcb_tp_addr(void);

void
__uros_main_tls_init(void)
{
    /*
     * i386 musl uses TLS-below-TP and defines __pthread_self() as
     * __get_tp() with no adjustment.  That means the LDT base must
     * point AT the pthread struct, and the struct's first field
     * (.self) must hold its own address — %gs:0 then reads .self and
     * returns the pthread pointer.  __uros_libc_init seeds .self =
     * &thr already; we just install the LDT with thr as the base.
     *
     * `__uros_main_thread` is `{ struct pthread thr; uintptr_t tp_word; }`
     * laid out by the linker; .thr lives at offset 0 so &thr ==
     * &__uros_main_thread.  The tp_word field is leftover from the
     * earlier TLS_ABOVE_TP-flavoured attempt — kept for ABI stability,
     * the LDT base no longer points at it.
     *
     * Kernel-side, i386_set_ldt now reloads LDTR before returning when
     * the target is the calling thread (#258), so we can load %gs
     * immediately after the trap — no yield needed.
     */
    void *thr = &__uros_main_thread;
    if (install_tls(mach_thread_self(), (unsigned int)thr) != KERN_SUCCESS)
        return;

    __asm__ __volatile__("movw %w0, %%gs" :: "r"(UROS_TLS_LDT_SELECTOR));
}

/* ------------------------------------------------------------------ */
/* Public TLS install for new pthread threads (used by posix_clone.c)  */
/* ------------------------------------------------------------------ */

/*
 * Install a TLS descriptor for `target_thread` whose base is the
 * supplied user_desc.  Returns the selector value the new thread
 * should load into %gs, or 0 on failure.  The thread should be
 * suspended (newly created, not yet running) — i386_set_ldt
 * generally accepts running threads too but the contract is
 * cleaner this way.
 */
int
__uros_install_thread_tls(mach_port_t target_thread,
                          struct __uros_user_desc *desc)
{
    if (!desc)
        return 0;
    kern_return_t kr = install_tls(target_thread, desc->base_addr);
    if (kr != KERN_SUCCESS)
        return 0;
    desc->entry_number = UROS_TLS_LDT_SLOT;
    return UROS_TLS_LDT_SELECTOR;
}

/* Variant used by __uros_clone: musl's pthread_create passes a raw TP
 * pointer (TP_ADJ(new) = new on i386) as the tls argument, not a
 * struct user_desc.  Skip the user_desc indirection entirely. */
int
__uros_install_thread_tls_at(mach_port_t target_thread, unsigned int base_addr)
{
    if (install_tls(target_thread, base_addr) != KERN_SUCCESS)
        return 0;
    return UROS_TLS_LDT_SELECTOR;
}

/* ------------------------------------------------------------------ */
/* SYS_set_thread_area dispatcher target                              */
/* ------------------------------------------------------------------ */

int
__uros_set_thread_area(struct __uros_user_desc *desc)
{
    if (!desc)
        return -22;        /* -EINVAL */
    int sel = __uros_install_thread_tls(mach_thread_self(), desc);
    if (!sel)
        return -22;
    /* musl's __set_thread_area asm re-reads entry_number and uses it
     * to compute the gs selector — we already stored UROS_TLS_LDT_SLOT
     * there above; the asm then OR's with 7 (Uros LDT bit, patched
     * in src/thread/i386/__set_thread_area.s). */
    return 0;
}
