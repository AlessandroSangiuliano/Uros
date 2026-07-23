#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <execinfo.h>

/* Read a generated file and check for non-ASCII bytes. If found, log details
 * and optionally abort if MIG_ABORT_ON_NONASCII=1 is set in environment. */
void
CheckAsciiFile(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) return;
    unsigned char buf[4096];
    size_t off = 0;
    while (1) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        {
            size_t i;
            for (i = 0; i < n; ++i) {
                unsigned char b = buf[i];
                if ((b < 32 && b != 9 && b != 10 && b != 13) || b > 126) {
                    fprintf(stderr, "[CheckAsciiFile] non-ASCII byte 0x%02x at file %s offset %zu\n", b, filename, off + i);
                    /* dump context */
                    size_t start = (i > 32) ? i - 32 : 0;
                    size_t end = (i + 128 < n) ? i + 128 : n;
                    fprintf(stderr, "context hex:");
                    {
                        size_t j;
                        for (j = start; j < end; ++j) fprintf(stderr, " %02x", buf[j]);
                    }
                    fprintf(stderr, "\n");

                    /* print an in-process backtrace to stderr to make captures deterministic */
                    void *bt[32];
                    int n = backtrace(bt, 32);
                    backtrace_symbols_fd(bt, n, fileno(stderr));

                    fclose(f);
                    if (getenv("MIG_ABORT_ON_NONASCII") && strcmp(getenv("MIG_ABORT_ON_NONASCII"), "1") == 0) {
                        fprintf(stderr, "MIG_ABORT_ON_NONASCII set - raising SIGTRAP\n");
                        raise(SIGTRAP);
                    }
                    return;
                }
            }
        }
        off += n;
    }
    fclose(f);
}
