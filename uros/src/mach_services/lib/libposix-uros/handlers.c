/*
 * libposix-uros — Phase 2 syscall handlers (#250).
 *
 * Real implementations:
 *   write/writev     → mach_print trap (DEBUG console).  Temporary —
 *                      Phase 3 replaces this with a proper write path
 *                      via libvfs / char_server.  Tracked in
 *                      project_mach_print_temp memory.
 *   mmap2 (anon)     → syscall_vm_allocate(mach_task_self(), ...)
 *   munmap           → syscall_vm_deallocate
 *   exit/exit_group  → syscall_task_terminate(mach_task_self()); never returns
 *
 * Stubs (return sensible POSIX errno / fixed value):
 *   brk            → -ENOMEM   (musl mallocng falls back to mmap)
 *   set_thread_area→ 0         (TLS comes in Phase 6)
 *   set_tid_address→ 1
 *   rt_sigprocmask → 0         (Phase 4 wires real signals)
 *   rt_sigaction   → 0
 *   ioctl          → -ENOTTY
 *   read/readv     → -EBADF
 *   open/openat    → -EACCES   (libvfs lands in Phase 3)
 *   close          → 0
 *   fstat64/stat64 → -EBADF
 *   getpid         → 1
 *   getuid/geteuid/getgid/getegid → 0  (no users yet)
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "internal.h"

/* ------------------------------------------------------------------ */
/* Linux i386 syscall numbers we care about.  Subset of the table musl */
/* builds from arch/i386/bits/syscall.h.in.                            */
/* ------------------------------------------------------------------ */
#define UROS_SYS_exit             1
#define UROS_SYS_read             3
#define UROS_SYS_write            4
#define UROS_SYS_open             5
#define UROS_SYS_close            6
#define UROS_SYS_getpid          20
#define UROS_SYS_getuid          24
#define UROS_SYS_kill            37
#define UROS_SYS_brk             45
#define UROS_SYS_getgid          47
#define UROS_SYS_geteuid         49
#define UROS_SYS_getegid         50
#define UROS_SYS_ioctl           54
#define UROS_SYS_munmap          91
#define UROS_SYS_readv          145
#define UROS_SYS_writev         146
#define UROS_SYS_rt_sigaction   174
#define UROS_SYS_rt_sigprocmask 175
#define UROS_SYS_mmap2          192
#define UROS_SYS_fstat64        197
#define UROS_SYS_tkill          238
#define UROS_SYS_set_thread_area 243
#define UROS_SYS_exit_group     252
#define UROS_SYS_set_tid_address 258
#define UROS_SYS_tgkill         270
#define UROS_SYS_openat         295

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Cache mach_task_self() — the kernel returns the same port name for
 * the lifetime of the task, so a single trap on the first VM call is
 * enough.  Initialised lazily to keep startup free of Uros-specific
 * hooks during Phase 2.
 */
static __uros_port_t __cached_task_self;

static __uros_port_t task_self(void)
{
    if (__cached_task_self == 0)
        __cached_task_self = __uros_trap_mach_task_self();
    return __cached_task_self;
}

/*
 * mach_print expects a NUL-terminated string.  POSIX write() gives us a
 * length-prefixed buffer with no terminator, so we copy through a small
 * stack chunk.  Phase 3 throws this away in favour of a real I/O path.
 */
static void debug_write(const char *p, size_t n)
{
    char chunk[128];
    while (n > 0) {
        size_t take = n < sizeof(chunk) - 1 ? n : sizeof(chunk) - 1;
        for (size_t i = 0; i < take; i++)
            chunk[i] = p[i];
        chunk[take] = '\0';
        __uros_trap_mach_print(chunk);
        p += take;
        n -= take;
    }
}

/* ------------------------------------------------------------------ */
/* Real handlers                                                      */
/* ------------------------------------------------------------------ */

static long h_write(long fd, long buf, long count,
                    long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    if (fd != 1 && fd != 2)
        return -EBADF;
    if (count < 0)
        return -EINVAL;
    if (count == 0)
        return 0;
    debug_write((const char *)buf, (size_t)count);
    return count;
}

/*
 * struct iovec layout on Linux i386: { void *iov_base; size_t iov_len; }
 * — two 32-bit words.  We decode by hand to avoid pulling in sys/uio.h
 * from a libc we don't yet own.
 */
struct uros_iovec { unsigned long base; unsigned long len; };

static long h_writev(long fd, long iov_ptr, long iovcnt,
                     long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    if (fd != 1 && fd != 2)
        return -EBADF;
    if (iovcnt < 0)
        return -EINVAL;

    const struct uros_iovec *iov = (const struct uros_iovec *)iov_ptr;
    long total = 0;
    for (long i = 0; i < iovcnt; i++) {
        if (iov[i].len == 0)
            continue;
        debug_write((const char *)iov[i].base, iov[i].len);
        total += (long)iov[i].len;
    }
    return total;
}

static long h_exit(long status, long a2, long a3,
                   long a4, long a5, long a6)
{
    (void)status; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    (void)__uros_trap_task_terminate(task_self());
    for (;;) { /* unreachable */ }
    return 0; /* unreachable, satisfies -Wreturn-type */
}

/*
 * mmap2 signature (Linux i386):
 *   void *mmap2(void *addr, size_t len, int prot,
 *               int flags, int fd, off_t pgoff);
 *
 * Phase 2 supports anonymous mappings only.  File-backed mmap requires
 * libvfs and arrives in Phase 3.
 */
#define UROS_MAP_ANONYMOUS  0x20
#define UROS_MAP_FIXED      0x10

static long h_mmap2(long addr, long len, long prot,
                    long flags, long fd, long pgoff)
{
    (void)prot; (void)pgoff;
    if (!(flags & UROS_MAP_ANONYMOUS) || fd != -1)
        return -ENOSYS;
    if (len <= 0)
        return -EINVAL;

    unsigned long out = (unsigned long)addr;
    int anywhere = !(flags & UROS_MAP_FIXED);
    __uros_kern_return_t kr = __uros_trap_vm_allocate(task_self(),
                                                     &out,
                                                     (unsigned long)len,
                                                     anywhere);
    if (kr != 0)
        return -ENOMEM;
    return (long)out;
}

static long h_munmap(long addr, long len, long a3,
                     long a4, long a5, long a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (len <= 0)
        return -EINVAL;
    __uros_kern_return_t kr = __uros_trap_vm_deallocate(task_self(),
                                                       (unsigned long)addr,
                                                       (unsigned long)len);
    return kr == 0 ? 0 : -EINVAL;
}

/* ------------------------------------------------------------------ */
/* Stubs                                                              */
/* ------------------------------------------------------------------ */

#define STUB(name, ret)                                                \
    static long name(long a1, long a2, long a3,                        \
                     long a4, long a5, long a6)                        \
    {                                                                  \
        (void)a1; (void)a2; (void)a3;                                  \
        (void)a4; (void)a5; (void)a6;                                  \
        return (ret);                                                  \
    }

STUB(h_brk,             -ENOMEM)
STUB(h_set_thread_area, 0)
STUB(h_set_tid_address, 1)
STUB(h_ioctl,           -ENOTTY)
STUB(h_read,            -EBADF)
STUB(h_readv,           -EBADF)
STUB(h_open,            -EACCES)
STUB(h_openat,          -EACCES)
STUB(h_close,           0)
STUB(h_fstat64,         -EBADF)
STUB(h_getuid,          0)
STUB(h_geteuid,         0)
STUB(h_getgid,          0)
STUB(h_getegid,         0)

/* ------------------------------------------------------------------ */
/* Phase 4 real handlers — bridge POSIX syscalls to libposix-uros's   */
/* signal module (signals.c).                                         */
/* ------------------------------------------------------------------ */

/* From signals.c. */
extern unsigned int __uros_my_pid;
extern int  __uros_sigaction(int signo, const void *act, void *old);
extern int  __uros_sigprocmask(int how, const void *set, void *old);
extern int  __uros_kill(unsigned int pid, int signo);

static long h_rt_sigaction(long sig, long act, long old,
                           long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;     /* a4 = sigsetsize, ignored */
    return __uros_sigaction((int)sig, (const void *)act, (void *)old);
}

static long h_rt_sigprocmask(long how, long set, long old,
                             long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    return __uros_sigprocmask((int)how, (const void *)set, (void *)old);
}

static long h_getpid(long a1, long a2, long a3,
                     long a4, long a5, long a6)
{
    (void)a1; (void)a2; (void)a3;
    (void)a4; (void)a5; (void)a6;
    /* Fall back to 1 when proc_server registration didn't go through —
     * tasks can still print "[pid=1]" without crashing. */
    return __uros_my_pid ? __uros_my_pid : 1;
}

static long h_kill(long pid, long sig, long a3,
                   long a4, long a5, long a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    return __uros_kill((unsigned int)pid, (int)sig);
}

/*
 * tkill / tgkill arrive from raise() and pthread_kill().  Without per-
 * thread targeting (Phase 6) we ignore the tid/tgid and dispatch to
 * the whole task — fine for raise() in single-threaded musl tasks.
 */
static long h_tkill(long tid, long sig, long a3,
                    long a4, long a5, long a6)
{
    (void)tid; (void)a3; (void)a4; (void)a5; (void)a6;
    return __uros_kill(__uros_my_pid ? __uros_my_pid : 1, (int)sig);
}

static long h_tgkill(long tgid, long tid, long sig,
                     long a4, long a5, long a6)
{
    (void)tid; (void)a4; (void)a5; (void)a6;
    return __uros_kill((unsigned int)tgid, (int)sig);
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                     */
/* ------------------------------------------------------------------ */

struct entry { long n; __uros_handler_t h; };

static const struct entry table[] = {
    { UROS_SYS_exit,             h_exit            },
    { UROS_SYS_read,             h_read            },
    { UROS_SYS_write,            h_write           },
    { UROS_SYS_open,             h_open            },
    { UROS_SYS_close,            h_close           },
    { UROS_SYS_getpid,           h_getpid          },
    { UROS_SYS_getuid,           h_getuid          },
    { UROS_SYS_kill,             h_kill            },
    { UROS_SYS_brk,              h_brk             },
    { UROS_SYS_getgid,           h_getgid          },
    { UROS_SYS_geteuid,          h_geteuid         },
    { UROS_SYS_getegid,          h_getegid         },
    { UROS_SYS_ioctl,            h_ioctl           },
    { UROS_SYS_munmap,           h_munmap          },
    { UROS_SYS_readv,            h_readv           },
    { UROS_SYS_writev,           h_writev          },
    { UROS_SYS_rt_sigaction,     h_rt_sigaction    },
    { UROS_SYS_rt_sigprocmask,   h_rt_sigprocmask  },
    { UROS_SYS_mmap2,            h_mmap2           },
    { UROS_SYS_fstat64,          h_fstat64         },
    { UROS_SYS_tkill,            h_tkill           },
    { UROS_SYS_set_thread_area,  h_set_thread_area },
    { UROS_SYS_exit_group,       h_exit            },
    { UROS_SYS_set_tid_address,  h_set_tid_address },
    { UROS_SYS_tgkill,           h_tgkill          },
    { UROS_SYS_openat,           h_openat          },
};

__uros_handler_t __uros_lookup(long n)
{
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (table[i].n == n)
            return table[i].h;
    return NULL;
}
