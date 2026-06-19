#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# #329: catch the rare near-NULL AP crash and pull a DDB backtrace.
#
# Boots ONE smp8 qemu at a time (never concurrent).  COM1 is a bidirectional
# unix socket so we can inject DDB commands; a separate monitor socket lets us
# quit qemu cleanly.  We retry until the guest drops into DDB (db{N}>), then
# send `trace`, `show all acts`, and per-cpu `trace` and capture the output.
import os, socket, subprocess, sys, time, signal

ROOT = "/home/slex/Scrivania/osfmk-mklinux"
RUN  = ROOT + "/scripts/run-qemu.sh"
SER  = "/tmp/uros-ddb-com1.sock"
MON  = "/tmp/uros-ddb-mon.sock"
LOG  = ROOT + "/uros/build/ddb-catch.log"
MAX  = int(sys.argv[1]) if len(sys.argv) > 1 else 25

CUR_Q = None   # currently-running qemu Popen (orphan-proofing)

def kill_cur():
    global CUR_Q
    if CUR_Q is None:
        return
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(MON)
        s.sendall(b"quit\n"); time.sleep(0.3); s.close()
    except Exception:
        pass
    try:
        CUR_Q.wait(timeout=6)
    except Exception:
        try: os.killpg(os.getpgid(CUR_Q.pid), signal.SIGKILL)
        except Exception: pass
    CUR_Q = None

def _sig(*_):
    kill_cur(); os._exit(1)

signal.signal(signal.SIGTERM, _sig)
signal.signal(signal.SIGINT, _sig)

def mon_cmd(cmd):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(MON)
        s.sendall((cmd + "\n").encode()); time.sleep(0.3)
        s.settimeout(1.0)
        try:
            while s.recv(4096): pass
        except Exception: pass
        s.close()
    except Exception: pass

def attempt(n):
    for p in (SER, MON):
        try: os.unlink(p)
        except FileNotFoundError: pass
    global CUR_Q
    q = subprocess.Popen(
        [RUN, "--smp", "8", "--bench", "comb", "cc", "inter", "pp",
         "-append", "-r",                       # -> cons_is_com1: DDB reads COM1
         "-serial", f"unix:{SER},server,nowait",
         "-display", "none",
         "-monitor", f"unix:{MON},server,nowait"],
        stdout=open(ROOT+"/uros/build/ddb-run.out","w"),
        stderr=subprocess.STDOUT, preexec_fn=os.setsid)
    CUR_Q = q
    try:
        # wait for COM1 socket
        for _ in range(40):
            if os.path.exists(SER): break
            time.sleep(0.5)
        c = socket.socket(socket.AF_UNIX)
        for _ in range(40):
            try: c.connect(SER); break
            except Exception: time.sleep(0.5)
        c.settimeout(1.0)
        buf = b""; logf = open(LOG, "wb")
        deadline = time.time() + 240
        wedged = False
        while time.time() < deadline:
            try: data = c.recv(4096)
            except socket.timeout: data = b""
            except Exception: break
            if data:
                buf += data; logf.write(data); logf.flush()
            tail = buf[-4000:]
            if b"db{" in tail:
                wedged = True; break
            if b"ush$" in tail and b"Benchmark complete" in tail:
                break
        if wedged:
            print(f"=== WEDGE on attempt {n} — pulling DDB trace ===", flush=True)
            def send(b):
                try: c.sendall(b)
                except Exception: pass
            def run_ddb(cmd, secs=6.0):
                # send a command, then read for `secs`, feeding spaces to defeat
                # the DDB "--db_more--" pager whenever the stream stalls.
                logf.write(b"\n>>> " + cmd.encode() + b"\n"); logf.flush()
                send(b"\r"); time.sleep(0.3)
                send((cmd + "\r").encode())
                end = time.time() + secs
                while time.time() < end:
                    try:
                        d = c.recv(8192)
                    except socket.timeout:
                        send(b" ")               # advance pager / wake prompt
                        continue
                    except Exception:
                        break
                    if d:
                        logf.write(d); logf.flush()
                        if b"--db_more--" in d[-32:] or b"--more--" in d[-32:]:
                            send(b" ")
            time.sleep(0.8)
            run_ddb("trace", 8)
            run_ddb("show all acts", 12)
            for cpu in range(8):
                run_ddb(f"cpu {cpu}", 2)
                run_ddb("trace", 5)
            logf.close()
            return True
        logf.close()
        return False
    finally:
        kill_cur()

for n in range(1, MAX+1):
    if attempt(n):
        print("CAUGHT — log at", LOG); sys.exit(0)
    print(f"attempt {n}: clean, retry", flush=True)
print("no wedge in", MAX, "attempts")
