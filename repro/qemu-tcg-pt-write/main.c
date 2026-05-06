/*
 * Minimal i386 kernel demonstrating a QEMU TCG softmmu bug.
 *
 * Setup:
 *   - PD[0]   identity-maps the first 4 MB via pt0.
 *   - PD[768] re-maps the same range at VA 0xC0000000+ (kernel-style
 *     "linear map" / phystokv pattern).  pt0 is therefore reachable
 *     via the higher-half alias too.
 *   - PD[769] points at pt1, but pt1 is initially all zeros, so any
 *     access to VA 0xC0400000+ triggers a #PF.
 *
 * Test 1: from kmain, with no pending fault, write a sentinel into a
 *   pt0 slot via the higher-half alias.  This succeeds on TCG too —
 *   the bug only shows up under fault-handling context.
 *
 * Test 2: deliberately access VA 0xC0400000 (no PT entry).  The #PF
 *   handler installs a valid PT entry into pt1 via its higher-half
 *   alias and returns.  Under KVM the faulting load completes; under
 *   QEMU TCG the alias write to pt1 is silently dropped, the handler
 *   returns, the CPU retries, faults again — infinite loop.  The
 *   handler counts retries and aborts the test after a small budget,
 *   reporting FAIL.
 */

#include <stdint.h>

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_init(void) {
    outb(COM1+1, 0x00);
    outb(COM1+3, 0x80);
    outb(COM1+0, 0x01);
    outb(COM1+1, 0x00);
    outb(COM1+3, 0x03);
    outb(COM1+2, 0xC7);
    outb(COM1+4, 0x0B);
}

static void putc(char c) {
    while ((inb(COM1+5) & 0x20) == 0);
    outb(COM1, c);
}

static void puts(const char *s) {
    while (*s) {
        if (*s == '\n') putc('\r');
        putc(*s++);
    }
}

static void puthex(uint32_t v) {
    putc('0'); putc('x');
    for (int i = 7; i >= 0; i--) {
        uint32_t n = (v >> (i*4)) & 0xF;
        putc(n < 10 ? '0' + n : 'a' + n - 10);
    }
}

/* Page directory and two page tables, all 4 KB page-aligned. */
static uint32_t pd[1024]  __attribute__((aligned(4096)));
static uint32_t pt0[1024] __attribute__((aligned(4096)));
static uint32_t pt1[1024] __attribute__((aligned(4096)));

/* IDT — only entry 14 (#PF) is populated. */
struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  zero;
    uint8_t  type;
    uint16_t off_hi;
} __attribute__((packed));
static struct idt_entry idt[256];

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

extern void pf_stub(void);
static volatile int pf_count;

/* C-level page-fault handler: invoked from pf_stub. */
void pf_handler(uint32_t err) {
    (void)err;
    pf_count++;

    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    /* Bail out before we loop forever, in case the alias write is
     * being silently dropped (the bug we're trying to surface). */
    if (pf_count > 4) {
        puts("\n[Test 2] FAIL — handler retried >4 times: TCG dropped\n"
             "  the alias write to pt1.  CR2="); puthex(cr2);
        puts("\n=== DONE (bug reproduced) ===\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* The fault is on a VA covered by PD[769] (4 MB range starting
     * at 0xC0400000) for which pt1[idx] was 0.  Install a valid
     * entry that maps the faulting page to its identity PA. */
    uint32_t pt_idx = (cr2 >> 12) & 0x3FF;
    uint32_t pa     = cr2 & ~0xFFFu;     /* identity-map: VA → PA */

    /* Write the new PTE into pt1 via its higher-half alias.
     * pt1 lives in the first 4 MB so its alias VA is 0xC0000000+pt1_pa. */
    uint32_t pt1_pa = (uint32_t)pt1;
    volatile uint32_t *pt1_alias =
        (volatile uint32_t *)(0xC0000000u + pt1_pa);

    pt1_alias[pt_idx] = (pa & ~0xFFFu) | 0x3u;  /* P + R/W */

    /* Flush any cached translation for the faulting page. */
    __asm__ volatile("invlpg (%0)" :: "r"(cr2) : "memory");
}

/* Assembly stub: save volatile regs, call C handler with err on stack,
 * pop the err code so iret sees the right frame. */
__asm__(
    ".globl pf_stub\n"
    "pf_stub:\n"
    "    pushal\n"
    "    movl 32(%esp), %eax\n"   /* error code */
    "    pushl %eax\n"
    "    call  pf_handler\n"
    "    addl  $4, %esp\n"
    "    popal\n"
    "    addl  $4, %esp\n"        /* discard error code */
    "    iret\n"
);

static void idt_set_gate(int n, void *handler) {
    uint32_t a = (uint32_t)handler;
    idt[n].off_lo = a & 0xFFFF;
    idt[n].off_hi = a >> 16;
    idt[n].sel    = 0x08;          /* multiboot: CS=0x08 flat 32-bit */
    idt[n].zero   = 0;
    idt[n].type   = 0x8E;          /* present, ring 0, 32-bit interrupt */
}

static void idt_load(void) {
    static struct idt_ptr p;
    p.limit = sizeof(idt) - 1;
    p.base  = (uint32_t)idt;
    __asm__ volatile("lidt (%0)" :: "r"(&p));
}

void kmain(void) {
    serial_init();
    puts("\n=== QEMU TCG softmmu PT-write reproducer ===\n");

    /* pt0: identity-map first 4 MB.  Mark each entry GLOBAL (bit 8)
     * to mirror the kernel's linear-map setup, where the soft-mmu
     * bug we are reproducing first showed up. */
    for (int i = 0; i < 1024; i++) {
        pt0[i] = (uint32_t)(i * 4096) | 0x103u;   /* P + R/W + G */
    }
    /* pt1: empty (handler will populate on demand). */

    uint32_t pt0_pa = (uint32_t)pt0;
    uint32_t pt1_pa = (uint32_t)pt1;
    uint32_t pd_pa  = (uint32_t)pd;

    pd[0]   = pt0_pa | 0x3u;     /* identity 0..4MB           */
    pd[768] = pt0_pa | 0x3u;     /* alias 0xC0000000..+4MB    */
    pd[769] = pt1_pa | 0x3u;     /* alias 0xC0400000..+4MB    */

    __asm__ volatile(
        "mov %0, %%cr3\n\t"
        "mov %%cr0, %%eax\n\t"
        "or  $0x80000000, %%eax\n\t"
        "mov %%eax, %%cr0\n\t"
        :: "r"(pd_pa) : "eax", "memory");

    puts("Paging on.\n  pt0_pa = "); puthex(pt0_pa);
    puts("\n  pt1_pa = ");           puthex(pt1_pa);
    puts("\n  pd_pa  = ");           puthex(pd_pa); puts("\n");

    /* ---- Test 1: write to pt0 via higher-half alias, no fault. ---- */
    {
        uint32_t alias = 0xC0000000u + pt0_pa + 100u * 4u;
        volatile uint32_t *p = (volatile uint32_t *)alias;
        puts("\n[Test 1] alias write to pt0[100] (no fault context)\n  va=");
        puthex(alias);
        puts(" before="); puthex(*p);
        *p = 0xDEADBEEFu;
        __asm__ volatile("invlpg (%0)" :: "r"(p) : "memory");
        puts(" after="); puthex(*p);
        puts(*p == 0xDEADBEEFu ? "  PASS\n"
                               : "  FAIL — TCG dropped write\n");
    }

    /* ---- Test 2: trigger a #PF whose handler must write the new
     * PTE into pt1 via the higher-half alias. ---- */
    idt_load();
    for (int i = 0; i < 256; i++) idt_set_gate(i, pf_stub);
    pf_count = 0;

    puts("\n[Test 2] deliberate #PF on VA 0xC0400000 — handler will\n"
         "  install a PT entry in pt1 via its higher-half alias.\n");

    volatile uint32_t *probe = (volatile uint32_t *)0xC0400000u;
    uint32_t got = *probe;       /* triggers #PF; handler installs PTE */

    puts("  load completed, value = "); puthex(got);
    puts("\n  retries inside handler: "); puthex((uint32_t)pf_count);
    puts(pf_count == 1 ? "  PASS\n" : "  FAIL\n");

    puts("\n=== DONE ===\n");
    for (;;) __asm__ volatile("hlt");
}
