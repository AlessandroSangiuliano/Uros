#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <execinfo.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>

static int has_nonascii(const unsigned char *buf, size_t len, size_t *off, unsigned char *byte) {
    for (size_t i = 0; i < len; ++i) {
        unsigned char b = buf[i];
        if ((b < 32 && b != 9 && b != 10 && b != 13) || b > 126) {
            if (off) *off = i;
            if (byte) *byte = b;
            return 1;
        }
    }
    return 0;
}

static void print_backtrace_and_info(int fd, const void *buf, size_t len, size_t off, unsigned char b) {
    void *bt[32];
    int n = backtrace(bt, 32);
    char path[64];
    char linkbuf[512] = {0};
    ssize_t l = 0;
    if (fd >= 0) {
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        l = readlink(path, linkbuf, sizeof(linkbuf)-1);
        if (l > 0) linkbuf[l] = '\0';
    }
    fprintf(stderr, "[nonascii-detect] fd=%d file=%s off=%zu byte=0x%02x len=%zu\n", fd, l>0?linkbuf:"(unknown)", off, b, len);
    backtrace_symbols_fd(bt, n, fileno(stderr));
    // print hex around offset
    size_t start = (off > 32) ? (off - 32) : 0;
    size_t end = (off + 128 < len) ? (off + 128) : len;
    fprintf(stderr, "context hex:");
    for (size_t i = start; i < end; ++i) fprintf(stderr, " %02x", ((unsigned char*)buf)[i]);
    fprintf(stderr, "\n");
}

ssize_t write(int fd, const void *buf, size_t count) {
    static ssize_t (*real_write)(int, const void*, size_t) = NULL;
    if (!real_write) real_write = dlsym(RTLD_NEXT, "write");
    size_t off; unsigned char b;
    if (has_nonascii(buf, count, &off, &b)) {
        print_backtrace_and_info(fd, buf, count, off, b);
        /* handle abort/pause modes: if MIG_ABORT_ON_NONASCII==2, create a pause file and stop the process
         * otherwise default to raising SIGTRAP (existing behavior) */
        const char *mode = getenv("MIG_ABORT_ON_NONASCII");
        if (mode && mode[0] == '2') {
            char pfn[64];
            snprintf(pfn, sizeof(pfn), "/tmp/nonascii_pause_%d", getpid());
            int pf = open(pfn, O_CREAT | O_WRONLY, 0600);
            if (pf >= 0) {
                dprintf(pf, "%d\n", (int)getpid());
                close(pf);
            }
            fprintf(stderr, "MIG_ABORT_ON_NONASCII=2 set - pause file %s created, stopping process\n", pfn);
            kill(getpid(), SIGSTOP);
        } else {
            fprintf(stderr, "MIG_ABORT_ON_NONASCII set - raising SIGTRAP\n");
            raise(SIGTRAP);
        }
    }
    return real_write(fd, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    static ssize_t (*real_writev)(int, const struct iovec*, int) = NULL;
    if (!real_writev) real_writev = dlsym(RTLD_NEXT, "writev");
    for (int i = 0; i < iovcnt; ++i) {
        size_t off; unsigned char b;
        if (has_nonascii(iov[i].iov_base, iov[i].iov_len, &off, &b)) {
            print_backtrace_and_info(fd, iov[i].iov_base, iov[i].iov_len, off, b);
            const char *mode = getenv("MIG_ABORT_ON_NONASCII");
            if (mode && mode[0] == '2') {
                char pfn[64];
                snprintf(pfn, sizeof(pfn), "/tmp/nonascii_pause_%d", getpid());
                int pf = open(pfn, O_CREAT | O_WRONLY, 0600);
                if (pf >= 0) {
                    dprintf(pf, "%d\n", (int)getpid());
                    close(pf);
                }
                fprintf(stderr, "MIG_ABORT_ON_NONASCII=2 set - pause file %s created, stopping process\n", pfn);
                kill(getpid(), SIGSTOP);
                break;
            } else {
                fprintf(stderr, "MIG_ABORT_ON_NONASCII set - raising SIGTRAP\n");
                raise(SIGTRAP);
                break;
            }
        }
    }
    return real_writev(fd, iov, iovcnt);
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    static size_t (*real_fwrite)(const void*, size_t, size_t, FILE*) = NULL;
    if (!real_fwrite) real_fwrite = dlsym(RTLD_NEXT, "fwrite");
    size_t len = size * nmemb;
    size_t off; unsigned char b;
    if (has_nonascii(ptr, len, &off, &b)) {
        int fd = fileno(stream);
        print_backtrace_and_info(fd, ptr, len, off, b);
        const char *mode = getenv("MIG_ABORT_ON_NONASCII");
        if (mode && mode[0] == '2') {
            char pfn[64];
            snprintf(pfn, sizeof(pfn), "/tmp/nonascii_pause_%d", getpid());
            int pf = open(pfn, O_CREAT | O_WRONLY, 0600);
            if (pf >= 0) {
                dprintf(pf, "%d\n", (int)getpid());
                close(pf);
            }
            fprintf(stderr, "MIG_ABORT_ON_NONASCII=2 set - pause file %s created, stopping process\n", pfn);
            kill(getpid(), SIGSTOP);
        } else {
            fprintf(stderr, "MIG_ABORT_ON_NONASCII set - raising SIGTRAP\n");
            raise(SIGTRAP);
        }
    }
    return real_fwrite(ptr, size, nmemb, stream);
}
