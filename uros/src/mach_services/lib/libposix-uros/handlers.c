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

/*
 * Mach trap stubs we reach for from this freestanding TU.  Declared
 * locally to keep handlers.c independent of `<mach.h>` (which would
 * pull in a lot of musl-vs-Mach type juggling).  Names match
 * libmach_core's public exports verbatim — the linker resolves them
 * to the SYSENTER stubs <mach/syscall_sw.h> emits.
 */
extern void                  mach_print(const char *);
extern __uros_port_t         mach_task_self(void);
extern __uros_kern_return_t  syscall_vm_allocate(__uros_port_t task,
                                                 unsigned long *addr,
                                                 unsigned long size,
                                                 int anywhere);
extern __uros_kern_return_t  syscall_vm_deallocate(__uros_port_t task,
                                                   unsigned long addr,
                                                   unsigned long size);
extern __uros_kern_return_t  syscall_vm_protect(__uros_port_t task,
                                                unsigned long addr,
                                                unsigned long size,
                                                int set_max,
                                                int new_prot);
extern __uros_kern_return_t  syscall_task_terminate(__uros_port_t task);
extern __uros_kern_return_t  thread_terminate(__uros_port_t thread);
extern __uros_port_t         mach_thread_self(void);

/*
 * POSIX fd layer (#262) — implemented in posix_fd.c, which owns the
 * libvfs include and the fd<->vfs_fd mapping.  handlers.c stays
 * freestanding-clean and just forwards the dispatch slots.
 */
extern long __uros_open(const char *path, int flags, int mode);
extern long __uros_openat(int dirfd, const char *path, int flags, int mode);
extern long __uros_read(int fd, void *buf, size_t count);
extern long __uros_write(int fd, const void *buf, size_t count);
extern long __uros_readv(int fd, const void *iov, int iovcnt);
extern long __uros_writev(int fd, const void *iov, int iovcnt);
extern long __uros_close(int fd);
extern long __uros_llseek(int fd, unsigned long off_hi, unsigned long off_lo,
                          void *result, unsigned int whence);
extern long __uros_statx(int dirfd, const char *path, int flags,
                         unsigned int mask, void *statxbuf);

/* ------------------------------------------------------------------ */
/* Linux i386 syscall numbers we care about.  Subset of the table musl */
/* builds from arch/i386/bits/syscall.h.in.                            */
/* ------------------------------------------------------------------ */
#define UROS_SYS_exit             1
#define UROS_SYS_fork             2
#define UROS_SYS_read             3
#define UROS_SYS_write            4
#define UROS_SYS_open             5
#define UROS_SYS_close            6
#define UROS_SYS_waitpid          7
#define UROS_SYS_execve          11
#define UROS_SYS_getpid          20
#define UROS_SYS_getuid          24
#define UROS_SYS_kill            37
#define UROS_SYS_brk             45
#define UROS_SYS__llseek        140
#define UROS_SYS_wait4          114
#define UROS_SYS_mprotect       125
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
#define UROS_SYS_futex          240
#define UROS_SYS_set_thread_area 243
#define UROS_SYS_exit_group     252
#define UROS_SYS_set_tid_address 258
#define UROS_SYS_tgkill         270
#define UROS_SYS_openat         295
#define UROS_SYS_statx          383

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Cache mach_task_self() — the kernel returns the same port name for
 * the lifetime of the task, so a single trap on the first VM call is
 * enough.  Initialised lazily to keep startup free of Uros-specific
 * hooks during Phase 2.
 */
/*
 * The cached task-self port name.  Non-static because posix_fork.c
 * resets it in the child path before the first IPC call (the parent's
 * task port name is invalid in the child's freshly-created IPC space).
 */
__uros_port_t __cached_task_self;

static __uros_port_t task_self(void)
{
    if (__cached_task_self == 0)
        __cached_task_self = mach_task_self();
    return __cached_task_self;
}

/* ------------------------------------------------------------------ */
/* Real handlers                                                      */
/* ------------------------------------------------------------------ */

/*
 * Phase 3 (#262): file I/O is owned by the POSIX fd layer in
 * posix_fd.c.  These dispatch slots forward to it; the fd layer routes
 * 0/1/2 to the console (mach_print) and everything else through libvfs.
 */
static long h_write(long fd, long buf, long count,
                    long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    if (count < 0)
        return -EINVAL;
    if (count == 0)
        return 0;
    return __uros_write((int)fd, (const void *)buf, (size_t)count);
}

static long h_writev(long fd, long iov_ptr, long iovcnt,
                     long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    return __uros_writev((int)fd, (const void *)iov_ptr, (int)iovcnt);
}

static long h_read(long fd, long buf, long count,
                   long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    if (count < 0)
        return -EINVAL;
    if (count == 0)
        return 0;
    return __uros_read((int)fd, (void *)buf, (size_t)count);
}

static long h_readv(long fd, long iov_ptr, long iovcnt,
                    long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    return __uros_readv((int)fd, (const void *)iov_ptr, (int)iovcnt);
}

static long h_open(long path, long flags, long mode,
                   long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    return __uros_open((const char *)path, (int)flags, (int)mode);
}

static long h_openat(long dirfd, long path, long flags,
                     long mode, long a5, long a6)
{
    (void)a5; (void)a6;
    return __uros_openat((int)dirfd, (const char *)path,
                         (int)flags, (int)mode);
}

static long h_close(long fd, long a2, long a3,
                    long a4, long a5, long a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return __uros_close((int)fd);
}

static long h_llseek(long fd, long off_hi, long off_lo,
                     long result, long whence, long a6)
{
    (void)a6;
    return __uros_llseek((int)fd, (unsigned long)off_hi,
                         (unsigned long)off_lo, (void *)result,
                         (unsigned int)whence);
}

static long h_statx(long dirfd, long path, long flags,
                    long mask, long statxbuf, long a6)
{
    (void)a6;
    return __uros_statx((int)dirfd, (const char *)path, (int)flags,
                        (unsigned int)mask, (void *)statxbuf);
}

/*
 * Phase 5b (#255): record our exit code with proc_server before we
 * vanish, so waitpid's WEXITSTATUS sees the real value instead of 0.
 * Implemented in signals.c (where the mach headers already live).
 * handlers.c stays freestanding-clean.
 */
extern void __uros_record_exit_code(int status);

static long h_exit(long status, long a2, long a3,
                   long a4, long a5, long a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    __uros_record_exit_code((int)status);
    (void)syscall_task_terminate(task_self());
    for (;;) { /* unreachable */ }
    return 0; /* unreachable, satisfies -Wreturn-type */
}

/*
 * Linux SYS_exit (1) terminates the calling thread only; SYS_exit_group
 * (252) takes down the whole task.  pthread_exit (worker thread end)
 * runs SYS_exit, and mapping it to task_terminate would kill the
 * joiner before pthread_join could observe the result.  Route SYS_exit
 * to thread_terminate(mach_thread_self()) and keep h_exit (task-wide)
 * for SYS_exit_group / process exit.
 */
static long h_exit_thread(long status, long a2, long a3,
                          long a4, long a5, long a6)
{
    (void)status; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    (void)thread_terminate(mach_thread_self());
    for (;;) { /* unreachable */ }
    return 0;
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

/* File-backed branch lives in posix_fd.c (#276 Phase B.3) so handlers.c
 * stays freestanding-clean (no <mach.h>, no libvfs).  Returns mapped
 * address or -errno; we forward the value verbatim from h_mmap2. */
extern long __uros_mmap_fd(long addr, unsigned long len, long prot,
                           long flags, int fd, long pgoff);

static long h_mmap2(long addr, long len, long prot,
                    long flags, long fd, long pgoff)
{
    if (len <= 0)
        return -EINVAL;

    /* File-backed: route through libvfs -> fs_server -> libfspager. */
    if (fd != -1)
        return __uros_mmap_fd(addr, (unsigned long)len, prot,
                              flags, (int)fd, pgoff);

    /* Anonymous: kernel vm_allocate as before. */
    if (!(flags & UROS_MAP_ANONYMOUS))
        return -ENOSYS;

    (void)prot; (void)pgoff;
    unsigned long out = (unsigned long)addr;
    int anywhere = !(flags & UROS_MAP_FIXED);
    __uros_kern_return_t kr = syscall_vm_allocate(task_self(),
                                                     &out,
                                                     (unsigned long)len,
                                                     anywhere);
    if (kr != 0)
        return -ENOMEM;
    return (long)out;
}

/*
 * mprotect(addr, len, prot).  We map Linux prot bits onto Mach
 * VM_PROT_* and call syscall_vm_protect with set_maximum=FALSE
 * (Mach's "current" protection, the one the CPU consults).
 */
#define UROS_PROT_READ  0x1
#define UROS_PROT_WRITE 0x2
#define UROS_PROT_EXEC  0x4
#define UROS_VM_PROT_READ    0x1
#define UROS_VM_PROT_WRITE   0x2
#define UROS_VM_PROT_EXECUTE 0x4

static long h_mprotect(long addr, long len, long prot,
                       long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    if (len <= 0)
        return -EINVAL;
    int new_prot = 0;
    if (prot & UROS_PROT_READ)  new_prot |= UROS_VM_PROT_READ;
    if (prot & UROS_PROT_WRITE) new_prot |= UROS_VM_PROT_WRITE;
    if (prot & UROS_PROT_EXEC)  new_prot |= UROS_VM_PROT_EXECUTE;
    __uros_kern_return_t kr = syscall_vm_protect(task_self(),
                                                    (unsigned long)addr,
                                                    (unsigned long)len,
                                                    0 /* not set_maximum */,
                                                    new_prot);
    return kr == 0 ? 0 : -EACCES;
}

static long h_munmap(long addr, long len, long a3,
                     long a4, long a5, long a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (len <= 0)
        return -EINVAL;
    __uros_kern_return_t kr = syscall_vm_deallocate(task_self(),
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
STUB(h_set_tid_address, 1)
STUB(h_ioctl,           -ENOTTY)
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
/* Phase 5 (#254): execve / waitpid / wait4                           */
/* ------------------------------------------------------------------ */

extern int __uros_execve(const char *path,
                         char *const argv[], char *const envp[]);
extern int __uros_waitpid(int pid, int *status, int options);
extern int __uros_fork(void);

static long h_fork(long a1, long a2, long a3,
                   long a4, long a5, long a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return __uros_fork();
}

static long h_execve(long path, long argv, long envp,
                     long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    return __uros_execve((const char *)path,
                         (char *const *)argv,
                         (char *const *)envp);
}

static long h_waitpid(long pid, long status, long options,
                      long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    return __uros_waitpid((int)pid, (int *)status, (int)options);
}

/* wait4(pid, status*, options, rusage*) — we ignore rusage entirely
 * in Phase 5 and route to waitpid.  Resource accounting in proc_server
 * (#240) can be exposed later if anyone cares. */
static long h_wait4(long pid, long status, long options, long rusage,
                    long a5, long a6)
{
    (void)rusage; (void)a5; (void)a6;
    return __uros_waitpid((int)pid, (int *)status, (int)options);
}

/* ------------------------------------------------------------------ */
/* Phase 6a (#256): set_thread_area / futex                            */
/* ------------------------------------------------------------------ */

extern int __uros_set_thread_area(void *desc);

static long h_set_thread_area(long desc, long a2, long a3,
                              long a4, long a5, long a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return __uros_set_thread_area((void *)desc);
}

/*
 * Real futex via mach_msg (#260).  Logic lives in posix_futex.c; this
 * is just the dispatcher slot.
 *
 * Linux futex signature on i386:
 *   futex(uint32_t *uaddr, int op, uint32_t val,
 *         const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)
 *
 * Operations we handle:
 *   FUTEX_WAIT (0) / FUTEX_WAIT_BITSET (9):
 *       block on a per-`uaddr` Mach port until FUTEX_WAKE arrives.
 *   FUTEX_WAKE (1) / FUTEX_WAKE_BITSET (10):
 *       send up to `val` empty messages to the per-`uaddr` port.
 *   PRIVATE bit (128) and CLOCK_REALTIME bit (256): ignored — every
 *       futex is intra-task and we treat timeouts as relative.
 */
#define UROS_FUTEX_OP_MASK     0x7f
#define UROS_FUTEX_WAIT        0
#define UROS_FUTEX_WAKE        1
#define UROS_FUTEX_WAIT_BITSET 9
#define UROS_FUTEX_WAKE_BITSET 10

extern int __uros_futex_wait(unsigned int *uaddr, unsigned int val,
                             const void *timeout);
extern int __uros_futex_wake(unsigned int *uaddr, int n);

static long h_futex(long uaddr, long op, long val, long timeout,
                    long uaddr2, long val3)
{
    (void)uaddr2; (void)val3;
    int bare_op = (int)(op & UROS_FUTEX_OP_MASK);
    switch (bare_op) {
    case UROS_FUTEX_WAIT:
    case UROS_FUTEX_WAIT_BITSET:
        return __uros_futex_wait((unsigned int *)uaddr,
                                 (unsigned int)val,
                                 (const void *)timeout);
    case UROS_FUTEX_WAKE:
    case UROS_FUTEX_WAKE_BITSET:
        return __uros_futex_wake((unsigned int *)uaddr, (int)val);
    default:
        return -38;           /* -ENOSYS */
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                     */
/* ------------------------------------------------------------------ */

struct entry { long n; __uros_handler_t h; };

static const struct entry table[] = {
    { UROS_SYS_exit,             h_exit_thread     },
    { UROS_SYS_fork,             h_fork            },
    { UROS_SYS_read,             h_read            },
    { UROS_SYS_write,            h_write           },
    { UROS_SYS_open,             h_open            },
    { UROS_SYS_close,            h_close           },
    { UROS_SYS_waitpid,          h_waitpid         },
    { UROS_SYS_execve,           h_execve          },
    { UROS_SYS_getpid,           h_getpid          },
    { UROS_SYS_wait4,            h_wait4           },
    { UROS_SYS_getuid,           h_getuid          },
    { UROS_SYS_kill,             h_kill            },
    { UROS_SYS_brk,              h_brk             },
    { UROS_SYS_getgid,           h_getgid          },
    { UROS_SYS_geteuid,          h_geteuid         },
    { UROS_SYS_getegid,          h_getegid         },
    { UROS_SYS_ioctl,            h_ioctl           },
    { UROS_SYS_munmap,           h_munmap          },
    { UROS_SYS_mprotect,         h_mprotect        },
    { UROS_SYS_readv,            h_readv           },
    { UROS_SYS_writev,           h_writev          },
    { UROS_SYS_rt_sigaction,     h_rt_sigaction    },
    { UROS_SYS_rt_sigprocmask,   h_rt_sigprocmask  },
    { UROS_SYS_mmap2,            h_mmap2           },
    { UROS_SYS__llseek,          h_llseek          },
    { UROS_SYS_statx,            h_statx           },
    { UROS_SYS_tkill,            h_tkill           },
    { UROS_SYS_futex,            h_futex           },
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
