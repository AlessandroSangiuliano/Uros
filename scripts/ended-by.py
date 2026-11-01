#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
"""ended-by.py STATUS -- COMMAND [ARGS...]

Runs COMMAND as its child, with the child's own pid -- fork, then exec --
writes that pid to STATUS.pid as soon as the child exists, and when the child
ends writes how into STATUS: "signal N" if signal N ended it, "exit N" if it
exited with status N.

That difference is what a shell's wait cannot report: a child killed by signal
N and a child that exited with 128+N both read 128+N there.  run-x86_64.sh's
trap ends the run by the signal it was sent, so that a shell waiting for it in
the foreground stops too, and a run that only exits with the same number looks
the same to `wait' -- harness-lifecycle-check.sh's signals check asks this
instead (#801).

The child gets SIGPIPE and SIGXFSZ back at their default, which Python ignores
for itself and an exec would otherwise pass on.  This process ignores INT,
TERM and HUP once the child exists, so that only the child is the one signalled,
and exits 0 once STATUS is written, 2 on a wrong command line.  A COMMAND that
cannot be run is reported as "exit 127", as a shell reports it.
"""

import os
import signal
import sys


def main():
    if len(sys.argv) < 4 or sys.argv[2] != "--":
        sys.stderr.write("usage: ended-by.py STATUS -- COMMAND [ARGS...]\n")
        return 2
    status, cmd = sys.argv[1], sys.argv[3:]

    pid = os.fork()
    if pid == 0:
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
        signal.signal(signal.SIGXFSZ, signal.SIG_DFL)
        try:
            os.execvp(cmd[0], cmd)
        except OSError as e:
            sys.stderr.write(f"ended-by: {cmd[0]}: {e}\n")
            os._exit(127)

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, signal.SIG_IGN)
    with open(status + ".pid.tmp", "w") as f:
        f.write(f"{pid}\n")
    os.rename(status + ".pid.tmp", status + ".pid")

    _, st = os.waitpid(pid, 0)
    with open(status + ".tmp", "w") as f:
        if os.WIFSIGNALED(st):
            f.write(f"signal {os.WTERMSIG(st)}\n")
        else:
            f.write(f"exit {os.WEXITSTATUS(st)}\n")
    os.rename(status + ".tmp", status)
    return 0


if __name__ == "__main__":
    sys.exit(main())
