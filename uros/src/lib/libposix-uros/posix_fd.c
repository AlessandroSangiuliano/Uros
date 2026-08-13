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
#include <servers/netname.h>    /* netname_look_up */
#include "proc.h"               /* proc_getsid */
#include "char_server.h"        /* char_tty_get_ctty/read/write/set_tostop */
#include <mach/mach_traps.h>	/* the traps, declared once (#426) */

/* Console write sink — same SYSENTER trap handlers.c reaches for. */
extern void mach_print(const char *);

/* From signals.c: proc_server send right + caller's pid. */
extern mach_port_t  __uros_proc_port;
extern unsigned int __uros_my_pid;

/* char_server send right — resolved lazily via netname on first /dev/tty
 * open.  Shared with the tty fd I/O path below. */
static mach_port_t      uros_char_port = MACH_PORT_NULL;
static pthread_mutex_t  char_port_lock = PTHREAD_MUTEX_INITIALIZER;

static mach_port_t char_port_resolve(void)
{
    mach_port_t got;

    pthread_mutex_lock(&char_port_lock);
    if (uros_char_port == MACH_PORT_NULL && name_server_port != MACH_PORT_NULL) {
        char host[80] = "";
        char serv[80] = "char";
        (void)netname_look_up(name_server_port, host, serv, &uros_char_port);
    }
    got = uros_char_port;
    pthread_mutex_unlock(&char_port_lock);
    return got;
}

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
    int       is_tty;       /* /dev/tty — routed to char_server (#275.4-bis) */
    int       cloexec;      /* close on execve */
    int       std_probed;   /* 0/1/2 — tried to bind to the session ctty (#286) */
    vfs_fd_t  vfd;          /* underlying libvfs fd (-1 for console/tty) */
    unsigned  tty_dev_id;   /* char_server dev_id for is_tty fds */
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

/*
 * /dev/tty open path (#275.4-bis): resolve the caller's session, ask
 * char_server which dev_id is bound as ctty, allocate a tty-kind fd.
 * Returns -ENOTTY when the session has no controlling tty (matches
 * Linux open("/dev/tty") for a session leader without one).
 */
/*
 * Resolve the caller's controlling-tty dev_id (or 0 if none): ask
 * proc_server for our sid, then char_server which dev is bound as the
 * session ctty.  Shared by open("/dev/tty") and the std-fd binding
 * below.  Does RPCs — never call while holding pfd_lock.
 */
static unsigned int ctty_dev_id(void)
{
    if (__uros_proc_port == 0 || __uros_my_pid == 0)
        return 0;

    mach_port_t cport = char_port_resolve();
    if (cport == MACH_PORT_NULL)
        return 0;

    unsigned int sid = 0;
    int rc = 0;
    if (proc_getsid(__uros_proc_port, __uros_my_pid, &sid, &rc) != 0
        || rc != 0)
        return 0;

    unsigned int dev_id = 0;
    int cres = 0;
    if (char_tty_get_ctty(cport, (int)sid, &dev_id, &cres) != 0
        || cres != 0)
        return 0;
    return dev_id;
}

/*
 * #286: lazily back the standard streams (fds 0/1/2) with the session's
 * controlling tty so a plain printf/read reaches the terminal, exactly
 * like every Unix.  A musl child execs into ush's session and inherits
 * its ctty, so its first stdout write upgrades fd 1 from the mach_print
 * console fallback to the real char_server TTY path.  Tasks with no ctty
 * (servers, early boot) keep the console fallback.  Probed once per fd:
 * a task that has no ctty when it first writes stays on the console
 * rather than re-issuing RPCs on every write.
 */
static void maybe_upgrade_std_fd(int fd)
{
    if (fd < 0 || fd > 2)
        return;

    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    if (!pfds[fd].is_console || pfds[fd].std_probed) {
        pthread_mutex_unlock(&pfd_lock);
        return;
    }
    pfds[fd].std_probed = 1;
    pthread_mutex_unlock(&pfd_lock);

    unsigned int dev_id = ctty_dev_id();   /* RPCs — lock dropped */
    if (dev_id == 0)
        return;                            /* no ctty: stay on console */

    pthread_mutex_lock(&pfd_lock);
    if (pfds[fd].is_console) {              /* re-check under lock */
        pfds[fd].is_console = 0;
        pfds[fd].is_tty     = 1;
        pfds[fd].tty_dev_id = dev_id;
    }
    pthread_mutex_unlock(&pfd_lock);
}

static long open_dev_tty(int flags)
{
    unsigned int dev_id = ctty_dev_id();
    if (dev_id == 0)
        return -ENOTTY;

    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    int fd = pfd_alloc_locked();
    if (fd < 0) {
        pthread_mutex_unlock(&pfd_lock);
        return -EMFILE;
    }
    pfds[fd].in_use     = 1;
    pfds[fd].is_console = 0;
    pfds[fd].is_tty     = 1;
    pfds[fd].cloexec    = (flags & LX_O_CLOEXEC) ? 1 : 0;
    pfds[fd].vfd        = VFS_FD_INVALID;
    pfds[fd].tty_dev_id = dev_id;
    pthread_mutex_unlock(&pfd_lock);
    return fd;
}

static int path_eq(const char *p, const char *s)
{
    while (*p && *s && *p == *s) { p++; s++; }
    return *p == 0 && *s == 0;
}

long __uros_open(const char *path, int flags, int mode)
{
    if (!path)
        return -EFAULT;
    /* No cwd yet: only absolute paths resolve through libvfs. */
    if (path[0] != '/')
        return -ENOENT;

    /* /dev/tty — magic per-session ctty device (#275.4-bis). */
    if (path_eq(path, "/dev/tty"))
        return open_dev_tty(flags);

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
    pfds[fd].is_tty     = 0;
    pfds[fd].cloexec    = (flags & LX_O_CLOEXEC) ? 1 : 0;
    pfds[fd].vfd        = vfd;
    pfds[fd].tty_dev_id = 0;
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
    int special = pfds[fd].is_console || pfds[fd].is_tty;
    vfs_fd_t vfd = pfds[fd].vfd;
    pthread_mutex_unlock(&pfd_lock);
    if (special)
        return -2;     /* caller distinguishes console vs tty via tty_fd_snapshot */
    return vfd;
}

/* Snapshot a tty-kind fd's (char_port, dev_id) under the lock.
 * Returns 0 on success, -EBADF if fd is invalid, -ENOTTY if not a tty. */
static int tty_fd_snapshot(int fd, mach_port_t *port, unsigned *dev_id)
{
    if (fd < 0 || fd >= POSIX_OPEN_MAX) return -EBADF;
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    if (!pfds[fd].in_use) { pthread_mutex_unlock(&pfd_lock); return -EBADF; }
    int tty = pfds[fd].is_tty;
    unsigned id = pfds[fd].tty_dev_id;
    pthread_mutex_unlock(&pfd_lock);
    if (!tty) return -ENOTTY;
    *port   = char_port_resolve();
    *dev_id = id;
    return (*port == MACH_PORT_NULL) ? -EIO : 0;
}

long __uros_read(int fd, void *buf, size_t count)
{
    int err;
    maybe_upgrade_std_fd(fd);          /* #286: bind 0/1/2 to ctty if any */
    vfs_fd_t vfd = resolve_fd(fd, &err);
    if (err) return err;
    if (vfd == -2) {
        /* fds 0/1/2 or /dev/tty: distinguish via is_tty. */
        mach_port_t cport;
        unsigned    dev_id;
        if (tty_fd_snapshot(fd, &cport, &dev_id) == 0) {
            char     cap[256] = { 0 };    /* MIG chr_token_t; session-owner bypass uses count=0 */
            char     rbuf[4096];
            unsigned int rlen = sizeof(rbuf);
            int      cres = 0;
            unsigned int want = count > sizeof(rbuf)
                                ? sizeof(rbuf) : (unsigned)count;
            /* Block until data is available — uart.so drains its IRQ
             * ring into the RPC buf; if the ring is empty we yield and
             * retry instead of busy-spinning the demux thread.  No
             * O_NONBLOCK support yet (v0.1 ush doesn't need it). */
            for (;;) {
                rlen = sizeof(rbuf);
                if (char_tty_read(cport, cap, 0, dev_id,
                                  (int)__uros_my_pid,
                                  want, rbuf, &rlen, &cres) != 0)
                    return -EIO;
                if (cres == -1)           /* CHR_TTY_BACKGROUND */
                    return -EINTR;
                if (cres != 0)
                    return -EIO;
                if (rlen > 0)
                    break;
                (void)swtch_pri(0);
            }
            if (rlen > count) rlen = count;
            if (rlen) memcpy(buf, rbuf, rlen);
            return (long)rlen;
        }
        return 0;                          /* plain console: no input */
    }
    ssize_t n = vfs_read(vfd, buf, count);
    return n < 0 ? -EIO : (long)n;
}

long __uros_write(int fd, const void *buf, size_t count)
{
    int err;
    maybe_upgrade_std_fd(fd);          /* #286: bind 0/1/2 to ctty if any */
    vfs_fd_t vfd = resolve_fd(fd, &err);
    if (err) return err;
    if (vfd == -2) {
        mach_port_t cport;
        unsigned    dev_id;
        if (tty_fd_snapshot(fd, &cport, &dev_id) == 0) {
            char     cap[256] = { 0 };
            char     wbuf[4096];
            int      cres = 0;
            unsigned int want = count > sizeof(wbuf)
                                ? sizeof(wbuf) : (unsigned)count;
            if (want) memcpy(wbuf, buf, want);
            if (char_tty_write(cport, cap, 0, dev_id, (int)__uros_my_pid,
                               wbuf, want, &cres) != 0)
                return -EIO;
            if (cres == -1)               /* CHR_TTY_BACKGROUND (TOSTOP) */
                return -EINTR;
            if (cres != 0)
                return -EIO;
            return (long)want;
        }
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
    int tty     = pfds[fd].is_tty;
    vfs_fd_t vfd = pfds[fd].vfd;
    if (!console) {
        pfds[fd].in_use     = 0;
        pfds[fd].is_tty     = 0;
        pfds[fd].vfd        = VFS_FD_INVALID;
        pfds[fd].tty_dev_id = 0;
    }
    pthread_mutex_unlock(&pfd_lock);
    /* Closing a std stream is a no-op (console stays open).  /dev/tty
     * has no per-open server-side state to release — char_server tracks
     * subscriptions, not fds — so just dropping the slot is enough. */
    if (!console && !tty)
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
 * True when the descriptor is a terminal: one of the console streams
 * bound at startup, or a /dev/tty opened later.  #402: ioctl needs the
 * distinction to answer TIOCGWINSZ per descriptor, which is what musl
 * builds isatty() and its stdout buffering decision on.
 */
int __uros_pfd_is_terminal(int fd)
{
    int r;
    if (fd < 0 || fd >= POSIX_OPEN_MAX)
        return 0;
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    r = pfds[fd].in_use && (pfds[fd].is_console || pfds[fd].is_tty);
    pthread_mutex_unlock(&pfd_lock);
    return r;
}

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
