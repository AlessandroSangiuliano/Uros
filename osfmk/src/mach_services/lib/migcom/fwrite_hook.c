/* In-process fwrite hook to detect non-ASCII bytes being written by migcom
 * Enabled via environment variable MIG_CHECK_WRITEBUF=1
 * When a non-ASCII byte is detected in the buffer being written, print
 * context, hex dump, backtrace and raise SIGTRAP to stop in debugger.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <string.h>

typedef size_t (*fwrite_fn_t)(const void *, size_t, size_t, FILE *);

size_t
fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    static fwrite_fn_t real_fwrite = NULL;
    const char *env = getenv("MIG_CHECK_WRITEBUF");

    if (!real_fwrite) {
        real_fwrite = (fwrite_fn_t)dlsym(RTLD_NEXT, "fwrite");
        if (!real_fwrite) {
            /* If dlsym failed, fall back to returning 0 to avoid
             * attempting to call through a NULL pointer. */
            return 0;
        }
    }

    if (env && env[0] == '1' && ptr != NULL && size > 0 && nmemb > 0) {
        size_t bytes = size * nmemb;
        const unsigned char *b = (const unsigned char *)ptr;
        size_t i;
        for (i = 0; i < bytes; ++i) {
            unsigned char c = b[i];
            if ((c < 32 && c != 9 && c != 10 && c != 13) || c > 126) {
                /* Unsafe to use fprintf here (may call fwrite). Use write(2) or
                 * low-level diagnostics, and also use backtrace helpers. */
                char hdr[256];
                int n = snprintf(hdr, sizeof(hdr), "[fwrite-hook] non-ASCII byte 0x%02x at offset %zu (size=%zu,nmemb=%zu)\n",
                                 c, i, size, nmemb);
                if (n > 0) write(STDERR_FILENO, hdr, (size_t)n);

                /* Dump a small surrounding hex context */
                size_t start = (i > 64) ? i - 64 : 0;
                size_t end = (i + 192 < bytes) ? i + 192 : bytes;
                size_t j;
                const size_t linebuf = 64 + (end - start) * 3 + 128;
                char *line = malloc(linebuf);
                if (line) {
                    size_t off = 0;
                    off += snprintf(line + off, linebuf - off, "context hex:");
                    for (j = start; j < end && off + 4 < linebuf; ++j) {
                        off += snprintf(line + off, linebuf - off, " %02x", (unsigned int)b[j]);
                    }
                    off += snprintf(line + off, linebuf - off, "\n");
                    write(STDERR_FILENO, line, off);
                    free(line);
                }

                /* Print a backtrace to stderr */
                void *bt[32];
                int cnt = backtrace(bt, 32);
                backtrace_symbols_fd(bt, cnt, STDERR_FILENO);

                /* Trigger a trap so gdb can catch the event */
                raise(SIGTRAP);
                break;
            }
        }
    }

    return real_fwrite(ptr, size, nmemb, stream);
}

/* Also intercept low-level write(2) so we can inspect buffers that vfprintf
 * writes directly into the kernel buffer (stdio may call writev/write).
 * Enabled with the same MIG_CHECK_WRITEBUF=1 environment variable.
 */
#include <sys/types.h>
#include <sys/uio.h>

ssize_t
write(int fd, const void *buf, size_t count)
{
    static ssize_t (*real_write)(int, const void *, size_t) = NULL;
    const char *env = getenv("MIG_CHECK_WRITEBUF");
    static __thread int in_hook = 0;

    if (!real_write) {
        real_write = (ssize_t(*)(int,const void*,size_t))dlsym(RTLD_NEXT, "write");
        if (!real_write) return -1;
    }

    if (in_hook) return real_write(fd, buf, count);

    if (env && env[0] == '1' && buf != NULL && count > 0 && fd != STDERR_FILENO && fd != STDOUT_FILENO) {
        const unsigned char *b = (const unsigned char *)buf;
        size_t i;
        for (i = 0; i < count; ++i) {
            unsigned char c = b[i];
            if ((c < 32 && c != 9 && c != 10 && c != 13) || c > 126) {
                in_hook = 1;
                char hdr[256];
                int n = snprintf(hdr, sizeof(hdr), "[write-hook] non-ASCII byte 0x%02x at offset %zu (fd=%d,count=%zu)\n",
                                 c, i, fd, count);
                if (n > 0) (void)real_write(STDERR_FILENO, hdr, (size_t)n);

                size_t start = (i > 64) ? i - 64 : 0;
                size_t end = (i + 192 < count) ? i + 192 : count;
                size_t j;
                const size_t linebuf = 64 + (end - start) * 3 + 128;
                char *line = malloc(linebuf);
                if (line) {
                    size_t off = 0;
                    off += snprintf(line + off, linebuf - off, "context hex:");
                    for (j = start; j < end && off + 4 < linebuf; ++j) {
                        off += snprintf(line + off, linebuf - off, " %02x", (unsigned int)b[j]);
                    }
                    off += snprintf(line + off, linebuf - off, "\n");
                    (void)real_write(STDERR_FILENO, line, off);
                    free(line);
                }

                void *bt[32];
                int cnt = backtrace(bt, 32);
                backtrace_symbols_fd(bt, cnt, STDERR_FILENO);

                raise(SIGTRAP);
                in_hook = 0;
                break;
            }
        }
    }

    return real_write(fd, buf, count);
}

ssize_t
writev(int fd, const struct iovec *iov, int iovcnt)
{
    static ssize_t (*real_writev)(int, const struct iovec *, int) = NULL;
    const char *env = getenv("MIG_CHECK_WRITEBUF");
    static __thread int in_hook = 0;

    if (!real_writev) {
        real_writev = (ssize_t(*)(int,const struct iovec*,int))dlsym(RTLD_NEXT, "writev");
        if (!real_writev) return -1;
    }

    if (in_hook) return real_writev(fd, iov, iovcnt);

    if (env && env[0] == '1' && iov != NULL && iovcnt > 0 && fd != STDERR_FILENO && fd != STDOUT_FILENO) {
        size_t tot = 0;
        int k;
        for (k = 0; k < iovcnt; ++k) tot += iov[k].iov_len;
        /* For large iovec sets, we only scan up to a reasonable cap */
        size_t scanned = 0;
        for (k = 0; k < iovcnt && scanned < tot; ++k) {
            const unsigned char *b = (const unsigned char *)iov[k].iov_base;
            size_t len = iov[k].iov_len;
            size_t i;
            for (i = 0; i < len; ++i) {
                unsigned char c = b[i];
                if ((c < 32 && c != 9 && c != 10 && c != 13) || c > 126) {
                    in_hook = 1;
                    char hdr[256];
                    int n = snprintf(hdr, sizeof(hdr), "[writev-hook] non-ASCII byte 0x%02x in iov[%d] offset %zu (fd=%d)\n",
                                     c, k, i, fd);
                    if (n > 0) (void)write(STDERR_FILENO, hdr, (size_t)n);
                    {
                        void *bt[32]; int cnt = backtrace(bt, 32);
                        backtrace_symbols_fd(bt, cnt, STDERR_FILENO);
                    }
                    raise(SIGTRAP);
                    in_hook = 0;
                    break;
                }
            }
            scanned += len;
            if (scanned >= tot) break;
        }
    }

    return real_writev(fd, iov, iovcnt);
}

ssize_t
pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    static ssize_t (*real_pwrite)(int, const void *, size_t, off_t) = NULL;
    const char *env = getenv("MIG_CHECK_WRITEBUF");
    static __thread int in_hook = 0;

    if (!real_pwrite) {
        real_pwrite = (ssize_t(*)(int,const void*,size_t,off_t))dlsym(RTLD_NEXT, "pwrite");
        if (!real_pwrite) return -1;
    }

    if (in_hook) return real_pwrite(fd, buf, count, offset);

    if (env && env[0] == '1' && buf != NULL && count > 0 && fd != STDERR_FILENO && fd != STDOUT_FILENO) {
        const unsigned char *b = (const unsigned char *)buf;
        size_t i;
        for (i = 0; i < count; ++i) {
            unsigned char c = b[i];
            if ((c < 32 && c != 9 && c != 10 && c != 13) || c > 126) {
                in_hook = 1;
                char hdr[256];
                int n = snprintf(hdr, sizeof(hdr), "[pwrite-hook] non-ASCII byte 0x%02x at offset %zu (fd=%d)\n",
                                 c, i, fd);
                if (n > 0) (void)write(STDERR_FILENO, hdr, (size_t)n);
                {
                    void *bt[32]; int cnt = backtrace(bt, 32);
                    backtrace_symbols_fd(bt, cnt, STDERR_FILENO);
                }
                raise(SIGTRAP);
                in_hook = 0;
                break;
            }
        }
    }

    return real_pwrite(fd, buf, count, offset);
}

