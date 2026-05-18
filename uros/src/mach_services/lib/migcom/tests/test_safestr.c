#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "safestr.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* Provide a small fatal() stub for the test so SafeSnprintf can call it.
 * The real project fatal() is complex and pulls in global state; here we just
 * print and exit to observe truncation behavior.
 */
void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(2);
}

int main(int argc, char **argv) {
    char buf[64];
    int r = SafeSnprintf(buf, sizeof(buf), "hello %s", "world");
    if (r <= 0) return 2;
    if (strcmp(buf, "hello world") != 0) return 3;

    if (argc > 1 && strcmp(argv[1], "trunc") == 0) {
        /* this should abort via fatal() inside SafeSnprintf when truncated */
        SafeSnprintf(buf, 4, "longstring");
        /* should not reach here */
        return 4;
    }
    return 0;
}
