#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# qmp-hmp.py — run human-monitor commands on a running qemu through its QMP
# socket, without stopping it (#599).
#
#   scripts/qmp-hmp.py <socket> 'info pic' 'info lapic 0' ...
#
# qemu must have been started with `-qmp unix:<socket>,server=on,wait=off'
# (run-qemu.sh passes trailing arguments to qemu).  Each command's output is
# printed under a `== <command>' line.  The machine keeps running: this is
# the read that has to come BEFORE a gdb attach, which stops it.
import json, socket, sys


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: qmp-hmp.py <socket> <command>...\n")
        return 2
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sys.argv[1])
    f = s.makefile("rw")

    def reply():
        # Skip asynchronous events; the answer is the first line with
        # `return' or `error'.
        while True:
            line = f.readline()
            if not line:
                raise EOFError("qemu closed the QMP socket")
            msg = json.loads(line)
            if "return" in msg or "error" in msg:
                return msg

    json.loads(f.readline())		# the greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    f.flush()
    reply()
    for cmd in sys.argv[2:]:
        f.write(json.dumps({"execute": "human-monitor-command",
                            "arguments": {"command-line": cmd}}) + "\n")
        f.flush()
        msg = reply()
        print("== " + cmd)
        if "error" in msg:
            print("  (refused: %s)" % msg["error"].get("desc", msg["error"]))
        else:
            sys.stdout.write(msg["return"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
