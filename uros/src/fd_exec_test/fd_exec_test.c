/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * fd_exec_test — musl-linked target for the execve fd-inheritance smoke
 * (#262 step 3).  The parent opens /posix_smoke.txt, fork+execve's this
 * binary; the kept-open fd 3 must survive the exec.  We read from fd 3
 * (the inherited descriptor, whose offset the parent left at 0) and exit
 * 0 on the expected content, non-zero otherwise — the result is observed
 * by the parent via waitpid so it's robust to console interleaving.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#define INHERITED_FD  3

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const char expect[] = "libposix-uros POSIX fd layer smoke\n";
    char buf[64];

    ssize_t n = read(INHERITED_FD, buf, sizeof buf - 1);
    if (n < 0) {
        printf("(fd_exec_test): read(fd=%d) failed — fd not inherited\n",
               INHERITED_FD);
        _exit(1);
    }
    buf[n] = '\0';
    int match = ((size_t)n == sizeof(expect) - 1)
                && memcmp(buf, expect, n) == 0;
    printf("(fd_exec_test): read %d bytes from inherited fd=%d: \"%s\" -> %s\n",
           (int)n, INHERITED_FD, buf, match ? "MATCH" : "MISMATCH");
    _exit(match ? 0 : 1);
}
