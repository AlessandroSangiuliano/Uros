#include "safestr.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"

int __attribute__((noinline)) SafeSnprintf(char *buf, size_t bufsiz, const char *fmt, ...)
{
    va_list ap;
    int needed;

    va_start(ap, fmt);
    needed = vsnprintf(buf, bufsiz, fmt, ap);
    va_end(ap);

    if (needed < 0) {
        fatal("SafeSnprintf: vsnprintf failed");
        return -1;
    }
    if ((size_t)needed >= bufsiz) {
        /* truncated */
        fatal("SafeSnprintf: formatting truncated (needed=%d, bufsiz=%zu) fmt='%s'", needed, bufsiz, fmt);
        return -1;
    }
    return needed;
}
