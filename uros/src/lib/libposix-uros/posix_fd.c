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
#include <mach/thread_switch.h>	/* SWITCH_OPTION_WAIT (#583) */
#include <mach/mach_syscalls.h>	/* syscall_thread_switch (#583) */

#include "posix_pipe.h"         /* pipes (#904) */
#include "pipe_types.h"         /* PIPE_END_READ, PIPE_END_WRITE (#904) */

/* From signals.c: proc_server send right + caller's pid. */
extern mach_port_t  __uros_proc_port;
extern unsigned int __uros_my_pid;
extern int          __uros_kill(unsigned int pid, int signo);  /* SIGPIPE (#904) */

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
#define LX_O_NONBLOCK  04000
#define LX_O_DIRECTORY 0200000
#define LX_O_NOFOLLOW  0400000
#define LX_O_CLOEXEC  02000000

/* struct iovec on Linux: { void *iov_base; size_t iov_len; }. */
struct uros_iovec { unsigned long base; unsigned long len; };
_Static_assert(sizeof(struct uros_iovec) == sizeof(struct uros_pipe_iov) &&
               offsetof(struct uros_iovec, len) ==
               offsetof(struct uros_pipe_iov, len),
               "a writev()'s pieces go to posix_pipe.c as they are");

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

/*
 * 🔑 A DESCRIPTOR NAMES AN OPEN FILE DESCRIPTION, AND dup() GIVES THE SAME
 * DESCRIPTION A SECOND NAME (#897).
 *
 * POSIX keeps the two apart.  The descriptor is a number with one flag of its
 * own, close-on-exec.  The open file description is what open() made: here
 * the libvfs fd and its offset, or the terminal it reaches.  dup(), dup2()
 * and dup3() give one description a second number, so both numbers read and
 * write at one offset, and the file closes when the last of them does.  This
 * table kept one description per descriptor, so dup could not be written.
 *
 * At most one description per descriptor is ever alive, so the two tables
 * are the same length and a free descriptor always finds a free description.
 */
enum ofd_kind {
    OFD_FREE = 0,
    OFD_CONSOLE,            /* mach_print; a read finds nothing */
    OFD_TTY,                /* char_server's tty (#275.4-bis) */
    OFD_VFS,                /* a libvfs fd */
    OFD_PIPE_READ,          /* a pipe's read end (#904, posix_pipe.c) */
    OFD_PIPE_WRITE,         /* and its write end */
};

#define OFD_IS_PIPE(k)  ((k) == OFD_PIPE_READ || (k) == OFD_PIPE_WRITE)
/* The end a pipe description is, as pipe_server names it (pipe_types.h). */
#define OFD_PIPE_END(k) ((k) == OFD_PIPE_WRITE ? PIPE_END_WRITE : PIPE_END_READ)

struct posix_ofd {
    int       refs;         /* descriptors of this task naming it; 0 = free */
    int       kind;         /* enum ofd_kind */
    int       std_probed;   /* console: tried to bind to the session ctty (#286) */
    int       nonblock;     /* O_NONBLOCK, a flag of the description (#904) */
    vfs_fd_t  vfd;          /* OFD_VFS; VFS_FD_INVALID otherwise */
    unsigned  tty_dev_id;   /* OFD_TTY: char_server's dev_id */
    struct uros_pipe *pipe; /* OFD_PIPE_*: holds one of the pipe's refs */
    unsigned  token;        /* a freed description's copy only: the token of
                               its end, to give back when it was the end's
                               last (#904), or 0 */
};

struct posix_fd {
    struct posix_ofd *ofd;  /* NULL: the number is free */
    int       cloexec;      /* close on execve, the descriptor's own flag */
};

static struct posix_ofd ofds[POSIX_OPEN_MAX];
static struct posix_fd  pfds[POSIX_OPEN_MAX];
static pthread_mutex_t  pfd_lock = PTHREAD_MUTEX_INITIALIZER;
static int              pfd_inited;

static void pfd_init_locked(void)
{
    if (pfd_inited)
        return;
    memset(ofds, 0, sizeof(ofds));
    memset(pfds, 0, sizeof(pfds));
    for (int i = 0; i < 3; i++) {
        ofds[i].refs = 1;
        ofds[i].kind = OFD_CONSOLE;
        ofds[i].vfd  = VFS_FD_INVALID;
        pfds[i].ofd  = &ofds[i];
    }
    pfd_inited = 1;
}

/* The lowest free descriptor >= from, or -1.  Caller holds pfd_lock. */
static int pfd_alloc_locked(int from)
{
    for (int i = from; i < POSIX_OPEN_MAX; i++)
        if (pfds[i].ofd == NULL)
            return i;
    return -1;
}

/* A free description, set up as `kind` with no descriptor naming it yet.
 * Caller holds pfd_lock. */
static struct posix_ofd *ofd_alloc_locked(int kind, vfs_fd_t vfd,
                                          unsigned tty_dev_id)
{
    for (int i = 0; i < POSIX_OPEN_MAX; i++) {
        struct posix_ofd *o = &ofds[i];
        if (o->refs != 0)
            continue;
        o->kind       = kind;
        o->std_probed = 0;
        o->nonblock   = 0;
        o->vfd        = vfd;
        o->tty_dev_id = tty_dev_id;
        o->pipe       = NULL;
        o->token      = 0;
        return o;
    }
    return NULL;
}

/* Make descriptor `fd`, which is free, name `o`.  Caller holds pfd_lock. */
static void pfd_bind_locked(int fd, struct posix_ofd *o, int cloexec)
{
    o->refs++;
    pfds[fd].ofd     = o;
    pfds[fd].cloexec = cloexec ? 1 : 0;
}

/*
 * Free descriptor `fd`, which is in use.  When it was the last name of its
 * description, the description is freed too and copied to *gone, so that the
 * caller can close its libvfs fd once pfd_lock is dropped (#262: no vfs_*
 * call under this lock).  Returns 1 then, else 0.  Caller holds pfd_lock.
 */
static int pfd_unbind_locked(int fd, struct posix_ofd *gone)
{
    struct posix_ofd *o = pfds[fd].ofd;

    pfds[fd].ofd     = NULL;
    pfds[fd].cloexec = 0;
    if (--o->refs > 0)
        return 0;
    *gone = *o;
    /* #904: the end's token goes back when this was the task's last
     * description of the end, so that pipe_server counts one holder of it
     * fewer.  And a pipe end gives back its hold on the pipe; *gone keeps the
     * pipe only when that was the last, for ofd_release() to let it go. */
    if (OFD_IS_PIPE(o->kind))
        gone->token = __uros_pipe_end_put(o->pipe, OFD_PIPE_END(o->kind));
    if (OFD_IS_PIPE(o->kind) && --o->pipe->refs > 0)
        gone->pipe = NULL;
    o->kind       = OFD_FREE;
    o->std_probed = 0;
    o->nonblock   = 0;
    o->vfd        = VFS_FD_INVALID;
    o->tty_dev_id = 0;
    o->pipe       = NULL;
    return 1;
}

/* What a freed description leaves to do, with pfd_lock dropped. */
static void ofd_release(const struct posix_ofd *gone)
{
    if (gone->kind == OFD_VFS)
        vfs_close(gone->vfd);
    if (OFD_IS_PIPE(gone->kind))
        __uros_pipe_token_put(gone->token);     /* 0: not the end's last */
    if (OFD_IS_PIPE(gone->kind) && gone->pipe != NULL)
        __uros_pipe_free(gone->pipe);   /* this task's last hold on it */
    /* A console has nothing to close, and a tty has no per-open state at
     * char_server: it tracks subscriptions, not descriptors. */
}

/*
 * A read or write in a pipe holds the pipe, so that a close() of the last
 * descriptor by another thread meanwhile does not unmap it under the call:
 * the pipe goes with the last of the two.  pipe_put() lets go of what
 * resolve_fd() took.
 */
struct pipe_hold {
    struct uros_pipe *pipe;
    int               kind;     /* OFD_PIPE_READ or OFD_PIPE_WRITE */
    int               nonblock;
};

static void pipe_put(struct uros_pipe *p)
{
    int last;

    pthread_mutex_lock(&pfd_lock);
    last = --p->refs == 0;
    pthread_mutex_unlock(&pfd_lock);
    if (last)
        __uros_pipe_free(p);
}

/* The description descriptor `fd` names, or NULL.  Caller holds pfd_lock. */
static struct posix_ofd *pfd_ofd_locked(int fd)
{
    if (fd < 0 || fd >= POSIX_OPEN_MAX)
        return NULL;
    return pfds[fd].ofd;
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

/*
 * The console, for fds 0/1/2 of a task with no controlling tty (#761).
 *
 * This cut the bytes into 127-byte pieces and handed each to mach_print as it
 * was, and mach_print takes a C string: a NUL among the bytes ended the piece
 * there, and whatever followed it in the same piece never reached the
 * console.  mach_print_bytes() leaves a NUL out instead, and keeps a line to a
 * trap while it fits -- the 127-byte cut split lines wherever it fell.
 *
 * 🔑 THE CONTRACT, which __uros_write() returns: every byte handed over counts
 * as written, a NUL included.  A terminal shows nothing for a NUL either, so
 * leaving it out changes nothing a reader sees; and a short count at a NUL
 * would be worse than wrong, because the caller -- musl's stdio, for one --
 * hands the rest back, NUL first, and a NUL can never go out.
 */
static void console_write(const char *p, size_t n)
{
    mach_print_bytes(p, (vm_size_t)n);
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
 *
 * #897: the probe belongs to the console description, not to the number:
 * once 0, 1 or 2 is bound, a dup() of it at another number reaches the tty
 * too, as they are one description.  Only those three ask, as they always
 * did: every read and write comes through here, and a higher number goes on
 * without taking pfd_lock a second time.  A dup of the console written
 * before any of the three stays on the console until one of them is.
 */
static void maybe_upgrade_std_fd(int fd)
{
    struct posix_ofd *o;

    if (fd < 0 || fd > 2)
        return;

    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    o = pfd_ofd_locked(fd);
    if (o == NULL || o->kind != OFD_CONSOLE || o->std_probed) {
        pthread_mutex_unlock(&pfd_lock);
        return;
    }
    o->std_probed = 1;
    pthread_mutex_unlock(&pfd_lock);

    unsigned int dev_id = ctty_dev_id();   /* RPCs — lock dropped */
    if (dev_id == 0)
        return;                            /* no ctty: stay on console */

    pthread_mutex_lock(&pfd_lock);
    /* Re-check under the lock: in between, `fd` may have been closed or
     * pointed elsewhere by dup2(), and `o` freed and handed to an open(). */
    if (pfds[fd].ofd == o && o->kind == OFD_CONSOLE) {
        o->kind       = OFD_TTY;
        o->tty_dev_id = dev_id;
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
    int fd = pfd_alloc_locked(3);
    struct posix_ofd *o = fd < 0 ? NULL
                                 : ofd_alloc_locked(OFD_TTY, VFS_FD_INVALID,
                                                    dev_id);
    if (o == NULL) {
        pthread_mutex_unlock(&pfd_lock);
        return -EMFILE;
    }
    pfd_bind_locked(fd, o, flags & LX_O_CLOEXEC);
    pthread_mutex_unlock(&pfd_lock);
    return fd;
}

static int path_eq(const char *p, const char *s)
{
    while (*p && *s && *p == *s) { p++; s++; }
    return *p == 0 && *s == 0;
}

/*
 * #599: the errno for what libvfs answered.  open and stat used to return
 * ENOENT for every failure and fstat EIO, so a directory refused as damaged
 * or a device that failed read as a missing file; libvfs now says which, and
 * this is where it becomes the errno a POSIX caller branches on.
 */
static long vfs_errno(int rc)
{
    switch (rc) {
    case VFS_ERR_NOENT:          return -ENOENT;
    case VFS_ERR_EXIST:          return -EEXIST;
    case VFS_ERR_NOTDIR:         return -ENOTDIR;
    case VFS_ERR_ISDIR:          return -EISDIR;
    case VFS_ERR_INVAL:          return -EINVAL;
    case VFS_ERR_NOSPC:          return -ENOSPC;
    case VFS_ERR_NAMETOOLONG:    return -ENAMETOOLONG;
    case VFS_ERR_PERM:           return -EACCES;
    case VFS_ERR_BADHANDLE:      return -EBADF;
    case VFS_ERR_NOTEMPTY:       return -ENOTEMPTY;
    case VFS_ERR_XDEV:           return -EXDEV;
    case KERN_RESOURCE_SHORTAGE: return -ENOMEM;
    default:                     return -EIO;
    }
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

    vfs_fd_t vfd;
    int rc = vfs_open_rc(path, xlate_open_flags(flags), mode, &vfd);
    if (rc != KERN_SUCCESS)
        return vfs_errno(rc);                   /* #599 */

    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    int fd = pfd_alloc_locked(3);
    struct posix_ofd *o = fd < 0 ? NULL : ofd_alloc_locked(OFD_VFS, vfd, 0);
    if (o == NULL) {
        pthread_mutex_unlock(&pfd_lock);
        vfs_close(vfd);
        return -EMFILE;
    }
    pfd_bind_locked(fd, o, flags & LX_O_CLOEXEC);
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
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    struct posix_ofd *o = pfd_ofd_locked(fd);
    if (o == NULL) { pthread_mutex_unlock(&pfd_lock); return -1; }
    int console = o->kind == OFD_CONSOLE;
    int pipe = OFD_IS_PIPE(o->kind);
    vfs_fd_t vfd = o->vfd;
    pthread_mutex_unlock(&pfd_lock);
    return console ? -2 : pipe ? -3 : vfd;
}

/* Resolve a POSIX fd to its libvfs fd.  Returns -1 and sets *err for an
 * invalid fd; returns -2 (with *err=0) for a console fd, and -3 for a pipe
 * end (#904), with the pipe held in *hold when the caller passes one: it
 * then lets go with pipe_put(). */
static vfs_fd_t resolve_fd(int fd, int *err, struct pipe_hold *hold)
{
    *err = 0;
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    struct posix_ofd *o = pfd_ofd_locked(fd);
    if (o == NULL) { pthread_mutex_unlock(&pfd_lock); *err = -EBADF; return -1; }
    int special = o->kind == OFD_CONSOLE || o->kind == OFD_TTY;
    vfs_fd_t vfd = o->vfd;
    if (OFD_IS_PIPE(o->kind)) {
        if (hold != NULL) {
            o->pipe->refs++;
            hold->pipe     = o->pipe;
            hold->kind     = o->kind;
            hold->nonblock = o->nonblock;
        }
        pthread_mutex_unlock(&pfd_lock);
        return -3;
    }
    pthread_mutex_unlock(&pfd_lock);
    if (special)
        return -2;     /* caller distinguishes console vs tty via tty_fd_snapshot */
    return vfd;
}

/*
 * read(), readv(), write() and writev() on a pipe end (#904): the pieces go
 * down whole, so that a writev() of up to PIPE_BUF bytes is one write, as
 * musl's stdio makes every line it flushes.  The wrong end answers EBADF, as
 * on Linux.
 *
 * A write on a pipe whose readers have all gone answers EPIPE and raises
 * SIGPIPE, as POSIX asks: a program that has not caught or ignored it ends,
 * which is how `yes | head -1' stops.  The signal goes the way kill() does,
 * through proc_server, so it may come a little after the write returns.
 */
static long pipe_io(struct pipe_hold *h, const struct uros_iovec *iov,
                    int iovcnt, int writing)
{
    long r;

    if (h->kind != (writing ? OFD_PIPE_WRITE : OFD_PIPE_READ))
        r = -EBADF;
    else if (writing) {
        r = __uros_pipe_write(h->pipe, (const struct uros_pipe_iov *)iov,
                              iovcnt, h->nonblock);
        if (r == -EPIPE && __uros_my_pid != 0)
            (void)__uros_kill(__uros_my_pid, PROC_SIGPIPE);
    } else
        r = __uros_pipe_read(h->pipe, (const struct uros_pipe_iov *)iov,
                             iovcnt, h->nonblock);
    pipe_put(h->pipe);
    return r;
}

/* Snapshot a tty-kind fd's (char_port, dev_id) under the lock.
 * Returns 0 on success, -EBADF if fd is invalid, -ENOTTY if not a tty. */
static int tty_fd_snapshot(int fd, mach_port_t *port, unsigned *dev_id)
{
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    struct posix_ofd *o = pfd_ofd_locked(fd);
    if (o == NULL) { pthread_mutex_unlock(&pfd_lock); return -EBADF; }
    int tty = o->kind == OFD_TTY;
    unsigned id = o->tty_dev_id;
    pthread_mutex_unlock(&pfd_lock);
    if (!tty) return -ENOTTY;
    *port   = char_port_resolve();
    *dev_id = id;
    return (*port == MACH_PORT_NULL) ? -EIO : 0;
}

/*
 * 🔴 A TTY READ THAT FINDS NOTHING WAITS FOR THE TTY, IT DOES NOT ASK AGAIN
 * (#583).
 *
 * char_server's tty_read never blocks: an empty ring is 0 bytes.  Its .defs
 * says how to wait -- subscribe to the "data available" notification and
 * read after each one -- and this file ignored that and looped on tty_read
 * with swtch_pri(0) between turns.  swtch_pri yields only when something
 * else is runnable, so on an idle system it returned at once: an i386 shell
 * at its prompt held the processor at 92%, char_server 16 s and proc_server
 * 8 s out of every 30, because each turn cost five RPCs (the session check
 * and tty_jobctl_check's three are all proc_server round trips).
 *
 * One receive right per (process, tty), subscribed once through
 * tty_subscribe_member -- the session-member access tty_read already has.
 * The notification is a queued message, so a byte that arrives between an
 * empty read and the wait is not lost; the only gap is the first time, when
 * data may have come before the subscription existed, and the read is
 * repeated right after subscribing to close it.
 *
 * ⚠️ The entry remembers WHICH PROCESS made it.  A fork() child inherits this
 * memory but not the IPC space, so the parent's port name means nothing
 * there -- it must neither be used nor released; the child subscribes anew.
 * The receive right dies with the process, and char_server drops a
 * subscriber whose port is dead the next time it sends.
 */
#define TTY_WAITERS 4

static struct tty_waiter {
    unsigned int pid;           /* the process that made it; 0 = empty */
    unsigned int dev_id;
    mach_port_t  port;          /* receive right; char_server has a send right */
    int          refused;       /* the subscription was refused: poll slowly */
} tty_waiters[TTY_WAITERS];
static pthread_mutex_t tty_waiters_lock = PTHREAD_MUTEX_INITIALIZER;

/* The port to wait on for (cport, dev_id), subscribing on first use.  *fresh
 * says the subscription was made just now.  -1: the tty refused it. */
static int tty_waiter_get(mach_port_t cport, unsigned dev_id,
                          mach_port_t *out, int *fresh)
{
    struct tty_waiter *e = NULL, *free_e = NULL;
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr;
    int i;

    *fresh = 0;
    pthread_mutex_lock(&tty_waiters_lock);
    for (i = 0; i < TTY_WAITERS; i++) {
        struct tty_waiter *w = &tty_waiters[i];
        if (w->pid == __uros_my_pid && w->dev_id == dev_id)
            e = w;
        else if (free_e == NULL && w->pid != __uros_my_pid)
            free_e = w;         /* empty, or a parent's: not ours to release */
    }
    if (e != NULL) {
        int refused = e->refused;
        *out = e->port;
        pthread_mutex_unlock(&tty_waiters_lock);
        return refused ? -1 : 0;
    }
    if (free_e == NULL) {
        pthread_mutex_unlock(&tty_waiters_lock);
        return -1;
    }
    e = free_e;
    e->pid = __uros_my_pid;
    e->dev_id = dev_id;
    e->port = MACH_PORT_NULL;
    e->refused = 1;

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (kr == KERN_SUCCESS) {
        kr = char_tty_subscribe_member(cport, dev_id, (int)__uros_my_pid, port);
        if (kr == KERN_SUCCESS) {
            e->port = port;
            e->refused = 0;
            *fresh = 1;
        } else {
            (void)mach_port_mod_refs(mach_task_self(), port,
                                     MACH_PORT_RIGHT_RECEIVE, -1);
        }
    }
    pthread_mutex_unlock(&tty_waiters_lock);
    if (e->refused) {
        mach_print("libposix: the tty refused a data-available subscription; "
                   "reads on it poll every 20 ms (#583)\n");
        return -1;
    }
    *out = port;
    return 0;
}

/*
 * Block until char_server says data arrived.  The message is header-only.
 *
 * No timeout.  A 250 ms one stood here for a while, blamed on a lost IRQ
 * front: it was hiding char_server's mach_msg_server destroying the send
 * right this port was subscribed with (#583).  A notification that does not
 * come is a defect to find, and a timeout makes it look like a slow tty.
 */
static void tty_waiter_wait(mach_port_t port)
{
    union {
        mach_msg_header_t hdr;
        char              room[128];
    } msg;

    (void)mach_msg(&msg.hdr, MACH_RCV_MSG, 0, sizeof(msg), port,
                   MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

long __uros_read(int fd, void *buf, size_t count)
{
    int err;
    struct pipe_hold hold = { NULL, 0, 0 };
    maybe_upgrade_std_fd(fd);          /* #286: bind 0/1/2 to ctty if any */
    vfs_fd_t vfd = resolve_fd(fd, &err, &hold);
    if (err) return err;
    if (vfd == -3) {
        struct uros_iovec one = { (unsigned long)buf, (unsigned long)count };
        return pipe_io(&hold, &one, 1, 0);
    }
    if (vfd == -2) {
        /* A console or a tty: tty_fd_snapshot() says which. */
        mach_port_t cport;
        unsigned    dev_id;
        if (tty_fd_snapshot(fd, &cport, &dev_id) == 0) {
            char     cap[256] = { 0 };    /* MIG chr_token_t; session-owner bypass uses count=0 */
            char     rbuf[4096];
            unsigned int rlen = sizeof(rbuf);
            int      cres = 0;
            unsigned int want = count > sizeof(rbuf)
                                ? sizeof(rbuf) : (unsigned)count;
            /* Block until data is available: an empty read waits for the
             * tty's notification (#583, see tty_waiter_get).  No
             * O_NONBLOCK support yet (v0.1 ush doesn't need it). */
            mach_port_t notify = MACH_PORT_NULL;
            int         fresh = 0;
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
                if (notify == MACH_PORT_NULL) {
                    if (tty_waiter_get(cport, dev_id, &notify, &fresh) != 0) {
                        notify = MACH_PORT_NULL;
                        (void)syscall_thread_switch(MACH_PORT_NULL,
                                                    SWITCH_OPTION_WAIT, 20);
                        continue;
                    }
                    if (fresh)
                        continue;       /* read again: the subscription is new */
                }
                tty_waiter_wait(notify);
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


/* One char_tty_write of n bytes, at most the 4096 chr_buf_t carries: the
 * bytes it took, or the error. */
static long tty_send(mach_port_t cport, unsigned dev_id, char *buf, size_t n)
{
    char cap[256] = { 0 };
    int  cres = 0;

    if (char_tty_write(cport, cap, 0, dev_id, (int)__uros_my_pid,
                       buf, (unsigned int)n, &cres) != 0)
        return -EIO;
    if (cres == -1)                       /* CHR_TTY_BACKGROUND (TOSTOP) */
        return -EINTR;
    if (cres != 0)
        return -EIO;
    return (long)n;
}

/*
 * The console's share of a full buffer: the whole lines in it go out, and
 * what follows the last newline moves to the front, for the bytes that will
 * finish it.  Returns how many bytes were kept.  A buffer with no newline in
 * it holds a line longer than itself -- longer than a trap carries, too
 * (MACH_PRINT_MAX) -- and goes out as it is.
 */
static size_t console_send_lines(char *buf, size_t have)
{
    size_t end = have;

    while (end > 0 && buf[end - 1] != '\n')
        end--;
    if (end == 0)
        end = have;
    console_write(buf, end);
    memmove(buf, buf + end, have - end);
    return have - end;
}

/*
 * 🔑 A TERMINAL GETS ONE WRITE, HOWEVER MANY PIECES IT CAME IN (#764).
 *
 * musl's stdio hands a line over as writev() of two pieces -- what its buffer
 * holds, and the piece with the newline (__stdio_write) -- and writev() used
 * to write each piece on its own: two console_write()s, so two mach_print
 * traps, and the line open between them.  At four processors another task's
 * line landed in that gap in 190 of 212 i386 boots, and at eight in every one:
 * `ush: setsid -> sid=13 pid=13gpu_server: text_puts(6): [ext]'.  One trap per
 * line is what closes it, not a narrower gap: the kernel prints the string of
 * a trap under printf_lock, whole.
 *
 * So the pieces are gathered first, and the terminal gets what write() of the
 * same bytes would have given it.  On a /dev/tty that is one char_tty_write of
 * at most 4096 bytes -- more is a short count, as a tty write always was.  The
 * console takes every byte: each time the buffer fills, its whole lines go out
 * and the start of an unfinished one waits for the rest, so nothing but
 * MACH_PRINT_MAX, inside console_write(), cuts a line.
 */
static long terminal_write(int fd, const struct uros_iovec *iov, int iovcnt)
{
    mach_port_t cport;
    unsigned    dev_id;
    char        wbuf[4096];
    size_t      have = 0;
    long        total = 0;
    int         tty = tty_fd_snapshot(fd, &cport, &dev_id) == 0;

    for (int i = 0; i < iovcnt; i++) {
        const char *p = (const char *)iov[i].base;
        size_t      n = iov[i].len;

        while (n > 0) {
            size_t take = n < sizeof(wbuf) - have ? n : sizeof(wbuf) - have;

            memcpy(wbuf + have, p, take);
            have  += take;
            total += (long)take;
            p     += take;
            n     -= take;
            if (have < sizeof(wbuf))
                continue;
            if (tty)
                return tty_send(cport, dev_id, wbuf, have);
            have = console_send_lines(wbuf, have);
        }
    }
    if (tty)
        return tty_send(cport, dev_id, wbuf, have);
    if (have)
        console_write(wbuf, have);
    return total;                       /* a NUL included: see console_write() */
}

long __uros_write(int fd, const void *buf, size_t count)
{
    int err;
    struct pipe_hold hold = { NULL, 0, 0 };
    maybe_upgrade_std_fd(fd);          /* #286: bind 0/1/2 to ctty if any */
    vfs_fd_t vfd = resolve_fd(fd, &err, &hold);
    if (err) return err;
    if (vfd == -3) {
        struct uros_iovec one = { (unsigned long)buf, (unsigned long)count };
        return pipe_io(&hold, &one, 1, 1);
    }
    if (vfd == -2) {
        struct uros_iovec one = { (unsigned long)buf, (unsigned long)count };
        return terminal_write(fd, &one, 1);
    }
    ssize_t n = vfs_write(vfd, buf, count);
    return n < 0 ? -EIO : (long)n;
}

long __uros_readv(int fd, const void *iov_ptr, int iovcnt)
{
    const struct uros_iovec *iov = (const struct uros_iovec *)iov_ptr;
    if (iovcnt < 0) return -EINVAL;

    /* A pipe takes the pieces in one read (#904). */
    int err;
    struct pipe_hold hold = { NULL, 0, 0 };
    if (resolve_fd(fd, &err, &hold) == -3 && err == 0)
        return pipe_io(&hold, iov, iovcnt, 0);

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

    /*
     * A terminal gets the pieces as one write: see terminal_write() (#764).
     * Nothing to write is 0 with no RPC, as it always was.
     */
    int err;
    struct pipe_hold hold = { NULL, 0, 0 };
    maybe_upgrade_std_fd(fd);          /* #286: bind 0/1/2 to ctty if any */
    vfs_fd_t vfd = resolve_fd(fd, &err, &hold);
    if (err) return err;
    if (vfd == -3)                     /* a pipe: one write too (#904) */
        return pipe_io(&hold, iov, iovcnt, 1);
    if (vfd == -2) {
        for (int i = 0; i < iovcnt; i++)
            if (iov[i].len)
                return terminal_write(fd, iov, iovcnt);
        return 0;
    }

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
    struct posix_ofd gone;
    int freed;

    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    struct posix_ofd *o = pfd_ofd_locked(fd);
    if (o == NULL) { pthread_mutex_unlock(&pfd_lock); return -EBADF; }
    /*
     * Closing a std stream that is still the console is a no-op: the
     * console stays open, as it always has.  Any other descriptor closes,
     * 0, 1 and 2 included once they name something else (#897): a dup()
     * of the console, a /dev/tty, or what dup2() put there.
     */
    if (fd <= 2 && o->kind == OFD_CONSOLE) {
        pthread_mutex_unlock(&pfd_lock);
        return 0;
    }
    freed = pfd_unbind_locked(fd, &gone);
    pthread_mutex_unlock(&pfd_lock);
    if (freed)
        ofd_release(&gone);
    return 0;
}

/*
 * dup2() and dup3(): make `newfd` name what `oldfd` names, closing what
 * `newfd` named first (#897).  Under the one lock, so no open() can take
 * `newfd` in between, which is what Linux answers EBUSY for.
 */
static long dup_onto(int oldfd, int newfd, int cloexec)
{
    struct posix_ofd gone;
    int freed = 0;

    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    struct posix_ofd *o = pfd_ofd_locked(oldfd);
    if (o == NULL || newfd < 0 || newfd >= POSIX_OPEN_MAX) {
        pthread_mutex_unlock(&pfd_lock);
        return -EBADF;
    }
    if (newfd == oldfd) {               /* dup2() only: dup3() refused it */
        pthread_mutex_unlock(&pfd_lock);
        return newfd;
    }
    if (pfds[newfd].ofd != NULL)
        freed = pfd_unbind_locked(newfd, &gone);
    pfd_bind_locked(newfd, o, cloexec);
    pthread_mutex_unlock(&pfd_lock);
    if (freed)
        ofd_release(&gone);
    return newfd;
}

/* dup(): the lowest free descriptor, naming what `oldfd` names. */
long __uros_dup(int oldfd)
{
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    struct posix_ofd *o = pfd_ofd_locked(oldfd);
    if (o == NULL) {
        pthread_mutex_unlock(&pfd_lock);
        return -EBADF;
    }
    int fd = pfd_alloc_locked(0);
    if (fd < 0) {
        pthread_mutex_unlock(&pfd_lock);
        return -EMFILE;
    }
    pfd_bind_locked(fd, o, 0);
    pthread_mutex_unlock(&pfd_lock);
    return fd;
}

long __uros_dup2(int oldfd, int newfd)
{
    return dup_onto(oldfd, newfd, 0);
}

long __uros_dup3(int oldfd, int newfd, int flags)
{
    if (flags & ~LX_O_CLOEXEC)
        return -EINVAL;
    if (oldfd == newfd)
        return -EINVAL;
    return dup_onto(oldfd, newfd, flags & LX_O_CLOEXEC);
}

/*
 * pipe() and pipe2() (#904): a new pipe, its read end at the lowest free
 * descriptor and its write end at the next.  O_CLOEXEC goes to both
 * descriptors, and O_NONBLOCK to both descriptions.  Each end's description
 * holds one of the pipe's refs (posix_pipe.h).
 */
long __uros_pipe2(int *fds, int flags)
{
    struct uros_pipe *p;
    struct posix_ofd *r, *w = NULL, gone;
    int fr, fw = -1, freed = 0, rc;
    int cloexec = flags & LX_O_CLOEXEC;
    int nonblock = (flags & LX_O_NONBLOCK) != 0;

    if (fds == NULL)
        return -EFAULT;
    if (flags & ~(LX_O_CLOEXEC | LX_O_NONBLOCK))
        return -EINVAL;
    rc = __uros_pipe_new(&p);
    if (rc != 0)
        return rc;

    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    fr = pfd_alloc_locked(0);
    r = fr < 0 ? NULL : ofd_alloc_locked(OFD_PIPE_READ, VFS_FD_INVALID, 0);
    if (r != NULL) {
        r->pipe = p;
        r->nonblock = nonblock;
        p->refs++;
        __uros_pipe_end_get(p, PIPE_END_READ);
        pfd_bind_locked(fr, r, cloexec);
        fw = pfd_alloc_locked(0);
        w = fw < 0 ? NULL : ofd_alloc_locked(OFD_PIPE_WRITE, VFS_FD_INVALID,
                                             0);
        if (w != NULL) {
            w->pipe = p;
            w->nonblock = nonblock;
            p->refs++;
            __uros_pipe_end_get(p, PIPE_END_WRITE);
            pfd_bind_locked(fw, w, cloexec);
        } else {
            freed = pfd_unbind_locked(fr, &gone);   /* takes the pipe along */
        }
    }
    pthread_mutex_unlock(&pfd_lock);
    if (r == NULL) {
        __uros_pipe_free(p);
        return -EMFILE;
    }
    if (w == NULL) {
        if (freed)
            ofd_release(&gone);
        return -EMFILE;
    }
    fds[0] = fr;
    fds[1] = fw;
    return 0;
}

#ifdef __x86_64__
/* lseek(fd, off, whence) — the offset fits a register here. */
long __uros_lseek(int fd, long off, unsigned int whence)
{
    int err;
    vfs_fd_t vfd = resolve_fd(fd, &err, NULL);
    if (err) return err;
    if (vfd == -2 || vfd == -3)       /* console and pipe: not seekable */
        return -ESPIPE;
    off_t r = vfs_lseek(vfd, (off_t)off, (int)whence);
    if (r == (off_t)-1)
        return -EINVAL;
    return (long)r;
}
#else
/* _llseek(fd, off_hi, off_lo, result*, whence) — i386 large-file lseek. */
long __uros_llseek(int fd, unsigned long off_hi, unsigned long off_lo,
                   void *result, unsigned int whence)
{
    int err;
    vfs_fd_t vfd = resolve_fd(fd, &err, NULL);
    if (err) return err;
    if (vfd == -2 || vfd == -3)       /* console and pipe: not seekable */
        return -ESPIPE;
    int64_t off = (int64_t)(((uint64_t)off_hi << 32) | (uint64_t)off_lo);
    off_t r = vfs_lseek(vfd, (off_t)off, (int)whence);
    if (r == (off_t)-1)
        return -EINVAL;
    if (result)
        *(int64_t *)result = (int64_t)r;
    return 0;
}
#endif

/*
 * statx(dirfd, path, flags, mask, statxbuf).  musl tries this first on
 * i386 (time64), and on x86-64 __uros_fstatat() below goes through it.  We fill the subset of struct statx that musl copies
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

/* A pipe end presents as a FIFO, which is what S_ISFIFO() asks (#904). */
static void fill_statx_fifo(struct lx_statx *sx)
{
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask    = LX_STATX_BASIC_STATS;
    sx->stx_blksize = 4096;
    sx->stx_nlink   = 1;
    sx->stx_mode    = (uint16_t)(0600 | LX_S_IFIFO);
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
        vfs_fd_t vfd = resolve_fd(dirfd, &err, NULL);
        if (err) return err;
        if (vfd == -2) { fill_statx_console(sx); return 0; }
        if (vfd == -3) { fill_statx_fifo(sx); return 0; }   /* #904 */
        vfs_stat_t vs;
        int rc = vfs_fstat(vfd, &vs);
        if (rc != 0)
            return vfs_errno(rc);               /* #599 */
        fill_statx(sx, &vs);
        return 0;
    }

    /* stat(path): AT_FDCWD + absolute path. */
    if (!path)
        return -EFAULT;
    if (path[0] != '/')
        return -ENOENT;             /* no cwd for relative paths yet */
    vfs_stat_t vs;
    int rc = vfs_stat(path, &vs);
    if (rc != 0)
        return vfs_errno(rc);                   /* #599 */
    fill_statx(sx, &vs);
    return 0;
}

#ifdef __x86_64__
/*
 * The kernel's struct stat on x86-64, which is what musl's fstatat() reads
 * (arch/x86_64/kstat.h, which is not a public header).  Spelled in widths
 * because it is an ABI, and #397 and #401 are what declaring these structures
 * by resemblance has cost.
 */
struct lx_kstat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime_sec, st_atime_nsec;
    int64_t  st_mtime_sec, st_mtime_nsec;
    int64_t  st_ctime_sec, st_ctime_nsec;
    int64_t  unused[3];
};
_Static_assert(sizeof(struct lx_kstat) == 144,
               "struct lx_kstat is not the size of the kernel's struct stat");

/*
 * stat, lstat, fstat and newfstatat, through the one path that knows a
 * descriptor from a name.  AT_SYMLINK_NOFOLLOW is accepted and has no effect,
 * as with statx on i386: libvfs has one stat.
 */
long __uros_fstatat(int dirfd, const char *path, void *kst, int flags)
{
    struct lx_statx sx;
    struct lx_kstat *k = kst;
    long rc;

    if (!k)
        return -EFAULT;
    rc = __uros_statx(dirfd, path, flags, LX_STATX_BASIC_STATS, &sx);
    if (rc != 0)
        return rc;

    memset(k, 0, sizeof(*k));
    k->st_ino     = sx.stx_ino;
    k->st_nlink   = sx.stx_nlink;
    k->st_mode    = sx.stx_mode;
    k->st_uid     = sx.stx_uid;
    k->st_gid     = sx.stx_gid;
    k->st_size    = (int64_t)sx.stx_size;
    k->st_blksize = sx.stx_blksize;
    k->st_blocks  = (int64_t)sx.stx_blocks;
    k->st_atime_sec = sx.stx_atime.tv_sec;
    k->st_mtime_sec = sx.stx_mtime.tv_sec;
    k->st_ctime_sec = sx.stx_ctime.tv_sec;
    return 0;
}
#endif

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
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    struct posix_ofd *o = pfd_ofd_locked(fd);
    r = o != NULL && (o->kind == OFD_CONSOLE || o->kind == OFD_TTY);
    pthread_mutex_unlock(&pfd_lock);
    return r;
}

/*
 * Enumerate the POSIX fds that survive execve: in use, not a console
 * stream, and not O_CLOEXEC.  Writes parallel (posix_fd, vfs_fd) arrays
 * and returns the count (<= max).  posix_exec_fds.c turns each pair into
 * a handoff record by exporting the libvfs state behind vfd.  Two
 * descriptors of one description carry the same vfd, which is how
 * posix_exec_fds.c tells a dup() from a second open() (#897).
 */
int __uros_pfd_enum_inheritable(int *posix_fds, vfs_fd_t *vfds, int max)
{
    int n = 0;
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    for (int i = 3; i < POSIX_OPEN_MAX && n < max; i++) {
        struct posix_ofd *o = pfds[i].ofd;
        /* #904: a pipe's ends cross an exec once the new image can map the
         * pipe, with #911's memory entries; until then they do not. */
        if (o == NULL || o->kind == OFD_CONSOLE || OFD_IS_PIPE(o->kind) ||
            pfds[i].cloexec)
            continue;
        posix_fds[n] = i;
        vfds[n]      = o->vfd;
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
    struct posix_ofd gone, *o;
    int freed = 0;

    if (posix_fd < 3 || posix_fd >= POSIX_OPEN_MAX)
        return;
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    if (pfds[posix_fd].ofd != NULL)
        freed = pfd_unbind_locked(posix_fd, &gone);
    o = ofd_alloc_locked(OFD_VFS, vfd, 0);
    if (o != NULL)
        pfd_bind_locked(posix_fd, o, 0);
    pthread_mutex_unlock(&pfd_lock);
    if (freed)
        ofd_release(&gone);
    if (o == NULL)
        vfs_close(vfd);
}

/*
 * Bind POSIX fd 'posix_fd' in the freshly-execed task to the description
 * 'of_fd' already names: the two were one description before the exec, a
 * dup() of each other (#897).  Returns 0, or -1 when 'of_fd' names nothing.
 */
int __uros_pfd_install_alias(int posix_fd, int of_fd)
{
    struct posix_ofd gone, *o;
    int freed = 0;

    if (posix_fd < 3 || posix_fd >= POSIX_OPEN_MAX || posix_fd == of_fd)
        return -1;
    pthread_mutex_lock(&pfd_lock);
    pfd_init_locked();
    o = pfd_ofd_locked(of_fd);
    if (o == NULL) {
        pthread_mutex_unlock(&pfd_lock);
        return -1;
    }
    if (pfds[posix_fd].ofd != NULL)
        freed = pfd_unbind_locked(posix_fd, &gone);
    pfd_bind_locked(posix_fd, o, 0);
    pthread_mutex_unlock(&pfd_lock);
    if (freed)
        ofd_release(&gone);
    return 0;
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
    if (vfd == -3)
        return -ENODEV;     /* a pipe end (#904): nothing to map, as on Linux */
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
