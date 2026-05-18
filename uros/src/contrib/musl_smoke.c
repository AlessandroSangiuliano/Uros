/*
 * musl_smoke.c — Phase 1 standalone smoke test for the vendored musl
 * libc (#249).  Links against libc-musl.a only — no Uros runtime,
 * no MIG, no Mach traps.  Exercises printf/malloc/strX so we know
 * the static library is healthy.
 *
 * Build:  ninja musl_smoke    (only when -DUROS_BUILD_MUSL=ON)
 * Run:    ./musl_smoke        (on the host — i386 binary)
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *buf = malloc(64);
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }
    strcpy(buf, "musl libc ");
    strcat(buf, "phase 1 smoke OK");
    printf("%s (len=%zu)\n", buf, strlen(buf));
    free(buf);
    return 0;
}
