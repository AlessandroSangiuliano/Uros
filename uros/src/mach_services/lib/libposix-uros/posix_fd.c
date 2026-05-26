/*
 * libposix-uros — POSIX file-descriptor layer (#262).
 *
 * Owns the POSIX fd numbering and maps each POSIX fd onto a libvfs
 * vfs_fd_t.  Routes open/openat/read/readv/write/writev/close/lseek/
 * stat/fstat through the libvfs dispatcher so musl-linked tasks can do
 * real file I/O.  fds 0/1/2 are reserved for the console (write goes to
 * mach_print until char_server's TTY lands; read returns EOF).
 *
 * Threading: a single mutex guards the POSIX fd table.  libvfs has its
 * own lock for the underlying vfs_fd table; the two never nest in a way
 * that can deadlock (we drop ours before any vfs_* call that itself
 * locks, except for table bookkeeping which holds no vfs lock).
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#include "libvfs.h"
#include <mach_init.h>          /* mach_task_self */
#include <mach/vm_inherit.h>
#include "mach_port.h"          /* MIG: mach_port_deallocate */
#include "mach.h"               /* MIG: vm_map */

/* Console write sink — same SYSENTER trap handlers.c reaches for. */
extern void mach_print(const char *);

/* ------------------------------------------------------------------ */
/*  Linux i386 O_* flags (musl passes these straight through)          */
/* ------------------------------------------------------------------ */
#define LX_O_ACCMODE    0003
#define LX_O_RDONLY     0000
#define LX_O_WRONLY     0001
#define LX_O_RDWR       0002
#define LX_O_CREAT      0100
#define LX_O_EXCL       0200
#define LX_O_TRUNC     01000
#define LX_O_APPEND    02000
#define LX_O_DIRECTORY 0200000
#define LX_O_NOFOLLOW  0400000
#define LX_O_CLOEXEC  02000000

/* statx / *at flags we recognise. */
#define LX_AT_FDCWD            (-100)
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_EMPTY_PATH       0x1000

/* POSIX mode S_IF* bits (octal), for encoding file type into st_mode. */
#define LX_S_IFMT   0170000
#define LX_S_IFREG  0100000
#define LX_S_IFDIR  0040000
#define LX_S_IFLNK  0120000
#define LX_S_IFCHR  0020000
#define LX_S_IFBLK  0060000
#define LX_S_IFIFO  0010000
#define LX_S_IFSOCK 0140000

/* ------------------------------------------------------------------ */
/*  POSIX fd table                                                     */
/* ------------------------------------------------------------------ */

#define POSIX_OPEN_MAX  64

struct posix_fd {
    int       in_use;
    int       is_console;   /* 0/1/2 — routed to mach_print, never vfs */
    int       cloexec;      /* close on execve */
    vfs_fd_t  vfd;          /* underlying libvfs fd (-1 for console) */
};

static struct posix_fd  pfds[POSIX_OPEN_MAX];
static pthread_mutex_t  pfd_lock = PTHREAD_MUTEX_INITIALIZER;
static int              pfd_inited;

static void pfd_init_locked(void)
{
    if (pfd_inited)
        return;
    memset(pfds, 0, sizeof(pfds));
    for (int i = 0; i < 3; i++) {
        pfds[i].in_use     = 1;
        pfds[i].is_console = 1;
        pfds[i].vfd        = VFS_FD_INVALID;
    }
    pfd_inited = 1;
}

/* Allocate the lowest free POSIX fd >= 3.  Caller holds pfd_lock. */
static int pfd_alloc_locked(void)
{
    for (int i = 3; i < POSIX_OPEN_MAX; i++)
        if (!pfds[i].in_use)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Flag translation                                                   */
/* ------------------------------------------------------------------ */

static int xlate_open_flags(int lx)
{
    int v;
    switch (lx & LX_O_ACCMODE) {
    case LX_O_WRONLY: v = VFS_O_WRONLY; break;
    case LX_O_RDWR:   v = VFS_O_RDWR;   break;
    default:          v = VFS_O_RDONLY; break;
    }
    if (lx & LX_O_CREAT)     v |= VFS_O_CREAT;
    if (lx & LX_O_EXCL)      v |= VFS_O_EXCL;
    if (lx & LX_O_TRUNC)     v |= VFS_O_TRUNC;
    if (lx & LX_O_APPEND)    v |= VFS_O_APPEND;
    if (lx & LX_O_DIRECTORY) v |= VFS_O_DIRECTORY;
    if (lx & LX_O_NOFOLLOW)  v |= VFS_O_NOFOLLOW;
    return v;
}

static unsigned vfs_type_to_ifmt(uint8_t t)
{
    switch (t) {
    case VFS_FT_DIR:     return LX_S_IFDIR;
    case VFS_FT_SYMLINK: return LX_S_IFLNK;
    case VFS_FT_CHAR:    return LX_S_IFCHR;
    case VFS_FT_BLOCK:   return LX_S_IFBLK;
    case VFS_FT_FIFO:    return LX_S_IFIFO;
    case VFS_FT_SOCK:    return LX_S_IFSOCK;
    case VFS_FT_REG:
    default:             return LX_S_IFREG;
    }
}

/* ------------------------------------------------------------------ */
/*  Console I/O (fds 0/1/2)                                             */
/* ------------------------------------------------------------------ */

static void console_write(const char *p, size_t n)
{
    char chunk[128];
    while (n > 0) {
        size_t take = n < sizeof(chunk) - 1 ? n : sizeof(chunk) - 1;
        memcpy(chunk, p, take);
        chunk[take] = '\0';
        mach_print(chunk);
        p += take;
        n -= take;
    }
}

/* ------------------------------------------------------------------ */
/*  Public handlers (called from handlers.c dispatch table)            */
/* ------------------------------------------------------------------ */

long __uros_open(const char *path, int flags, int mode)
{
    if (!path)
        return -EFAULT;
    /* No cwd yet: only absolute paths resolve through libvfs. */
    if (path[0] != '/')
        return -ENOENT;

    vfs_fd_t vfd = vfs_open(path, xlate_open_flags(flags), mode);
    if (vfd == VFS_FD_INVALID)
        return -ENOENT;

    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    int fd = pfd_alloc_locked();
    if (fd < 0) {
        pthread_mutex_unlock(&pfd_lock);
        vfs_close(vfd);
        return -EMFILE;
    }
    pfds[fd].in_use     = 1;
    pfds[fd].is_console = 0;
    pfds[fd].cloexec    = (flags & LX_O_CLOEXEC) ? 1 : 0;
    pfds[fd].vfd        = vfd;
    pthread_mutex_unlock(&pfd_lock);
    return fd;
}

long __uros_openat(int dirfd, const char *path, int flags, int mode)
{
    /* Without a cwd / dir-fd namespace, only AT_FDCWD with an absolute
     * path is meaningful.  Everything else is unsupported for now. */
    if (path && path[0] == '/')
        return __uros_open(path, flags, mode);
    if (dirfd == LX_AT_FDCWD)
        return __uros_open(path, flags, mode);   /* relative -> ENOENT */
    return -ENOTSUP;
}

/* Public helper: resolve a POSIX fd to its libvfs fd.  Mirrors the
 * internal resolve_fd but exported for h_mmap2 (#276) and any other
 * handler that needs the libvfs side of an fd it received as POSIX.
 * Returns -1 for invalid / closed fds and -2 for the console pseudo-
 * fds (0/1/2 before they are redirected to real files). */
vfs_fd_t __uros_pfd_to_vfs(int fd)
{
    if (fd < 0 || fd >= POSIX_OPEN_MAX) return -1;
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    if (!pfds[fd].in_use) { pthread_mutex_unlock(&pfd_lock); return -1; }
    int console = pfds[fd].is_console;
    vfs_fd_t vfd = pfds[fd].vfd;
    pthread_mutex_unlock(&pfd_lock);
    return console ? -2 : vfd;
}

/* Resolve a POSIX fd to its libvfs fd.  Returns -1 and sets *err for an
 * invalid fd; returns -2 (with *err=0) for a console fd. */
static vfs_fd_t resolve_fd(int fd, int *err)
{
    *err = 0;
    if (fd < 0 || fd >= POSIX_OPEN_MAX) { *err = -EBADF; return -1; }
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    if (!pfds[fd].in_use) { pthread_mutex_unlock(&pfd_lock); *err = -EBADF; return -1; }
    int console = pfds[fd].is_console;
    vfs_fd_t vfd = pfds[fd].vfd;
    pthread_mutex_unlock(&pfd_lock);
    if (console)
        return -2;
    return vfd;
}

long __uros_read(int fd, void *buf, size_t count)
{
    int err;
    vfs_fd_t vfd = resolve_fd(fd, &err);
    if (err) return err;
    if (vfd == -2)                    /* console: nothing to read yet */
        return 0;
    ssize_t n = vfs_read(vfd, buf, count);
    return n < 0 ? -EIO : (long)n;
}

long __uros_write(int fd, const void *buf, size_t count)
{
    int err;
    vfs_fd_t vfd = resolve_fd(fd, &err);
    if (err) return err;
    if (vfd == -2) {                  /* console */
        if (count) console_write((const char *)buf, count);
        return (long)count;
    }
    ssize_t n = vfs_write(vfd, buf, count);
    return n < 0 ? -EIO : (long)n;
}

/* struct iovec on Linux i386: { void *iov_base; size_t iov_len; }. */
struct uros_iovec { unsigned long base; unsigned long len; };

long __uros_readv(int fd, const void *iov_ptr, int iovcnt)
{
    const struct uros_iovec *iov = (const struct uros_iovec *)iov_ptr;
    if (iovcnt < 0) return -EINVAL;
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].len == 0) continue;
        long n = __uros_read(fd, (void *)iov[i].base, iov[i].len);
        if (n < 0) return total ? total : n;
        total += n;
        if ((unsigned long)n < iov[i].len) break;   /* short read */
    }
    return total;
}

long __uros_writev(int fd, const void *iov_ptr, int iovcnt)
{
    const struct uros_iovec *iov = (const struct uros_iovec *)iov_ptr;
    if (iovcnt < 0) return -EINVAL;
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].len == 0) continue;
        long n = __uros_write(fd, (const void *)iov[i].base, iov[i].len);
        if (n < 0) return total ? total : n;
        total += n;
        if ((unsigned long)n < iov[i].len) break;   /* short write */
    }
    return total;
}

long __uros_close(int fd)
{
    if (fd < 0 || fd >= POSIX_OPEN_MAX)
        return -EBADF;
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    if (!pfds[fd].in_use) { pthread_mutex_unlock(&pfd_lock); return -EBADF; }
    int console = pfds[fd].is_console;
    vfs_fd_t vfd = pfds[fd].vfd;
    if (!console) {
        pfds[fd].in_use = 0;
        pfds[fd].vfd    = VFS_FD_INVALID;
    }
    pthread_mutex_unlock(&pfd_lock);
    /* Closing a std stream is a no-op here (console stays open). */
    if (!console)
        vfs_close(vfd);
    return 0;
}

/* _llseek(fd, off_hi, off_lo, result*, whence) — i386 large-file lseek. */
long __uros_llseek(int fd, unsigned long off_hi, unsigned long off_lo,
                   void *result, unsigned int whence)
{
    int err;
    vfs_fd_t vfd = resolve_fd(fd, &err);
    if (err) return err;
    if (vfd == -2)                    /* console is not seekable */
        return -ESPIPE;
    int64_t off = (int64_t)(((uint64_t)off_hi << 32) | (uint64_t)off_lo);
    off_t r = vfs_lseek(vfd, (off_t)off, (int)whence);
    if (r == (off_t)-1)
        return -EINVAL;
    if (result)
        *(int64_t *)result = (int64_t)r;
    return 0;
}

/*
 * statx(dirfd, path, flags, mask, statxbuf).  musl tries this first on
 * i386 (time64).  We fill the subset of struct statx that musl copies
 * out in fstatat.c.  Layout mirrors the kernel UAPI struct statx.
 */
struct lx_statx_timestamp { int64_t tv_sec; uint32_t tv_nsec; int32_t pad; };
struct lx_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t pad1;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct lx_statx_timestamp stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor;
    uint32_t stx_dev_major,  stx_dev_minor;
    uint64_t spare[14];
};

#define LX_STATX_BASIC_STATS 0x7ff

static void fill_statx(struct lx_statx *sx, const vfs_stat_t *vs)
{
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask    = LX_STATX_BASIC_STATS;
    sx->stx_blksize = vs->st_blksize ? vs->st_blksize : 4096;
    sx->stx_nlink   = vs->st_nlink ? vs->st_nlink : 1;
    sx->stx_uid     = vs->st_uid;
    sx->stx_gid     = vs->st_gid;
    sx->stx_mode    = (uint16_t)((vs->st_mode & 07777)
                                 | vfs_type_to_ifmt(vs->st_type));
    sx->stx_ino     = vs->st_ino;
    sx->stx_size    = vs->st_size;
    sx->stx_blocks  = vs->st_blocks;
    sx->stx_atime.tv_sec = vs->st_atime_sec;
    sx->stx_mtime.tv_sec = vs->st_mtime_sec;
    sx->stx_ctime.tv_sec = vs->st_ctime_sec;
}

/* Console fds present as character devices so isatty()/buffering work. */
static void fill_statx_console(struct lx_statx *sx)
{
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask    = LX_STATX_BASIC_STATS;
    sx->stx_blksize = 1024;
    sx->stx_nlink   = 1;
    sx->stx_mode    = (uint16_t)(0620 | LX_S_IFCHR);
}

long __uros_statx(int dirfd, const char *path, int flags,
                  unsigned int mask, void *statxbuf)
{
    (void)mask;
    struct lx_statx *sx = (struct lx_statx *)statxbuf;
    if (!sx)
        return -EFAULT;

    /* fstat(fd): musl calls statx(fd, "", AT_EMPTY_PATH, ...). */
    if ((flags & LX_AT_EMPTY_PATH) && path && path[0] == '\0') {
        int err;
        vfs_fd_t vfd = resolve_fd(dirfd, &err);
        if (err) return err;
        if (vfd == -2) { fill_statx_console(sx); return 0; }
        vfs_stat_t vs;
        if (vfs_fstat(vfd, &vs) != 0)
            return -EIO;
        fill_statx(sx, &vs);
        return 0;
    }

    /* stat(path): AT_FDCWD + absolute path. */
    if (!path)
        return -EFAULT;
    if (path[0] != '/')
        return -ENOENT;             /* no cwd for relative paths yet */
    vfs_stat_t vs;
    if (vfs_stat(path, &vs) != 0)
        return -ENOENT;
    fill_statx(sx, &vs);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  execve fd handoff hooks (#262 step 3)                              */
/* ------------------------------------------------------------------ */

/*
 * Enumerate the POSIX fds that survive execve: in use, not a console
 * stream, and not O_CLOEXEC.  Writes parallel (posix_fd, vfs_fd) arrays
 * and returns the count (<= max).  posix_exec_fds.c turns each pair into
 * a handoff record by exporting the libvfs state behind vfd.
 */
int __uros_pfd_enum_inheritable(int *posix_fds, vfs_fd_t *vfds, int max)
{
    int n = 0;
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    for (int i = 3; i < POSIX_OPEN_MAX && n < max; i++) {
        if (!pfds[i].in_use || pfds[i].is_console || pfds[i].cloexec)
            continue;
        posix_fds[n] = i;
        vfds[n]      = pfds[i].vfd;
        n++;
    }
    pthread_mutex_unlock(&pfd_lock);
    return n;
}

/*
 * Bind POSIX fd 'posix_fd' to an already-imported libvfs fd 'vfd' in
 * the freshly-execed task (#262 step 3).  Cleared O_CLOEXEC: an fd that
 * crossed exec is by definition not close-on-exec.
 */
void __uros_pfd_install(int posix_fd, vfs_fd_t vfd)
{
    if (posix_fd < 3 || posix_fd >= POSIX_OPEN_MAX)
        return;
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    pfds[posix_fd].in_use     = 1;
    pfds[posix_fd].is_console = 0;
    pfds[posix_fd].cloexec    = 0;
    pfds[posix_fd].vfd        = vfd;
    pthread_mutex_unlock(&pfd_lock);
}

/* ------------------------------------------------------------------ */
/*  File-backed mmap (#276 Phase B.3)                                   */
/* ------------------------------------------------------------------ */

/* Linux i386 mmap2 protection / flag bits the dispatcher in
 * handlers.c maps onto Mach VM_PROT_*; mirrored here so we
 * stay independent of that header. */
#define LX_PROT_READ   0x1
#define LX_PROT_WRITE  0x2
#define LX_PROT_EXEC   0x4
#define LX_MAP_FIXED   0x10

/*
 * __uros_mmap_fd — file-backed branch of h_mmap2.  Translate POSIX fd
 * into a libvfs fd, fetch the memory_object via vfs_mmap (a fresh port
 * per call in Phase B; Phase D may dedup MAP_SHARED), and vm_map it
 * into the caller's task at byte offset pgoff * 4096.
 *
 * Returns the mapped address on success, -errno on failure.  h_mmap2
 * funnels the fd!=-1 case here and returns this value verbatim.
 */
long
__uros_mmap_fd(long addr, unsigned long len, long prot,
               long flags, int fd, long pgoff)
{
    vfs_fd_t   vfd;
    mach_port_t mem_obj = MACH_PORT_NULL;
    int        rc;
    kern_return_t kr;

    if (len == 0)
        return -EINVAL;

    vfd = __uros_pfd_to_vfs(fd);
    if (vfd < 0)
        return -EBADF;

    rc = vfs_mmap(vfd, (int)prot, (int)flags, &mem_obj);
    if (rc != 0 || mem_obj == MACH_PORT_NULL)
        return -ENODEV;

    /* Build vm_map args.  Mach vm_prot bits happen to match Linux
     * PROT_ values 1/2/4 — straight cast is correct on i386. */
    vm_address_t a = (vm_address_t)addr;
    int anywhere = !(flags & LX_MAP_FIXED);
    vm_prot_t cur_prot = (vm_prot_t)(prot & 7);
    if (cur_prot == 0)
        cur_prot = VM_PROT_READ;
    vm_prot_t max_prot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;

    /* MAP_FIXED semantics on Linux implicitly unmap any pre-existing
     * mapping covering [addr, addr+len).  Mach vm_map refuses to
     * overlap (KERN_INVALID_ADDRESS).  Replicate the Linux behaviour
     * by vm_deallocate'ing the range first — musl's ld-musl relies on
     * this when it MAP_FIXEDs each PT_LOAD over a previously-reserved
     * VA hole. */
    if (flags & LX_MAP_FIXED)
        (void)vm_deallocate(mach_task_self(), a, (vm_size_t)len);

    kr = vm_map(mach_task_self(),
                &a, (vm_size_t)len,
                /*mask=*/ 0, anywhere,
                mem_obj,
                /*offset=*/ (vm_offset_t)pgoff * 4096u,
                /*copy=*/   FALSE,
                cur_prot, max_prot,
                VM_INHERIT_DEFAULT);

    /* Drop the local send right — vm_map took its own. */
    (void)mach_port_deallocate(mach_task_self(), mem_obj);

    if (kr != KERN_SUCCESS)
        return -ENOMEM;
    return (long)a;
}
