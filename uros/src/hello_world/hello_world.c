/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * hello_world.c — the textbook Unix hello world (#286).
 *
 * A plain musl-linked program that prints with printf and returns.
 * No mach_print, no kept-open fd tricks, no manual write(tty_fd): just
 * stdout, the way every standard Unix program does it.  Launched from
 * ush ("/hello_world"), its stdout is the session's controlling tty, so
 * "hello" lands on the terminal and the process exits 0 — which ush
 * reaps via waitpid.
 *
 * The plumbing that makes this work: libposix-uros lazily binds fds
 * 0/1/2 to the session ctty on first I/O (see maybe_upgrade_std_fd in
 * posix_fd.c), so printf -> write(1) -> char_server TTY.
 */

#include <stdio.h>

int
main(void)
{
    printf("hello\n");
    return 0;
}
