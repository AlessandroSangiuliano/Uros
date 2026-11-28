# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# run_lock.py — the lock every x86-64 run shares, taken and given back the
# same way by the tools that boot a machine beside run-x86_64.sh (#618).
#
# One run at a time on a machine: two qemus measure each other (run-x86_64.sh
# says why).  The lock is a directory, made by mkdir and removed by rmdir, with
# nothing inside.  Four rules, which the tools kept differently or not at all
# until #618:
#
#   - Taken BEFORE anything of the run is made.  make-disk-x86_64.sh rewrites
#     the images a run in flight is booting from, and a tool that made its disk
#     first and only then refused had already done it: the run's own
#     xfile-verify.py read a blank image and said WRONG for a kernel that had
#     done nothing wrong.
#   - Given back only while it is still the one this run made.  A lock removed
#     by hand and taken again by another run is that run's, and removing it
#     let a third one in.  Known by the directory's device, inode and change
#     time, as run-x86_64.sh and fbcons-readback.py (#661) know theirs.
#   - The run's qemu dies with the run, however the run dies, and is gone
#     before the lock is: a lock given back while the machine still runs lets
#     another run boot beside it.  run-x86_64.sh used to kill every qemu on
#     the machine by name before it booted, which is what cleaned up after a
#     tool that died with its qemu running; nothing does that any more.
#   - TERM and HUP end the run the way Ctrl-C does, through its finally: by
#     default Python dies of them on the spot, and the lock and the qemu stayed
#     behind (measured: char-roundtrip.py sent TERM exited 143, its qemu alive
#     and the lock still there).
#
# And a fifth, since #1016: the run's files in /tmp -- its FIFOs, its
# monitor's socket -- go with it, however it ends, as its qemu does.  Each tool
# removed its own, in code a signal could skip: stopped with their systemd
# units, char-roundtrip.py, rip-sample.py and uart-stall.py left theirs in 8
# stops of 8 on omen, and ramoops-readback.py in 3.  remove_at_end() names
# one, and give_back() removes it.
#
# Imported, with sys.dont_write_bytecode set first: nothing is to be left in
# scripts/.

import ctypes, os, signal, subprocess, sys

LOCK = "/tmp/uros-x86_64-run.lock"
PR_SET_PDEATHSIG = 1		# prctl(2): a signal for when the parent dies

_machines = []			# the qemus start_qemu() started
_paths = []			# #1016: the run's files, for give_back() to remove
_who = "run_lock"		# #1016: the run's name, as take() was given it
_STOP = (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)	# give_back() waits them out


def _owner():
    st = os.stat(LOCK)
    return (st.st_dev, st.st_ino, st.st_ctime_ns)


def take(who):
    """This run's lock, or None after saying, as `who', that another run has
    it.  Called before anything else of the run is made."""
    global _who
    _who = who
    try:
        os.mkdir(LOCK)
    except FileExistsError:
        print(f"{who}: another x86-64 run is in flight ({LOCK}) — refusing "
              f"to interleave", file=sys.stderr)
        return None
    return _owner()


def give_back(mine, who):
    """The run's qemu stopped, if the run has not stopped it already, the
    files remove_at_end() named removed, and then the lock removed, only
    while it is still `mine' -- said, as `who', when it is not.  The files
    are the run's whoever holds the lock: they go either way.

    🔴 With TERM, HUP and INT ignored until it is done (#801).  _stop() stays
    the handler, so a second TERM raised SystemExit again in here -- and
    systemctl sends one: a unit's main process, a script with no trap, dies
    on the first, and what is left of the unit is signalled again.  Stopped
    that way (harness-lifecycle-check.sh's unitstop), rip-sample.py and
    uart-stall.py left the lock in 6 boots of 6 and char-roundtrip.py in 1.
    Ignored rather than blocked: blocked in this thread, a signal can be
    taken by another, and Python runs the handler here all the same."""
    before = [(sig, signal.signal(sig, signal.SIG_IGN)) for sig in _STOP]
    try:
        for q in _machines:
            if q.poll() is None:
                q.kill()
                q.wait()
        for p in _paths:
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
            except OSError as e:
                print(f"{who}: {p} could not be removed: {e}", file=sys.stderr)
        try:
            ours = _owner() == mine
        except FileNotFoundError:
            ours = False
        if ours:
            os.rmdir(LOCK)
        else:
            print(f"{who}: the lock was not this run's any more when it ended "
                  f"({LOCK}): left as it is", file=sys.stderr)
    finally:
        for sig, handler in before:
            signal.signal(sig, handler)


def _stop(signum, _frame):
    """The run ends: from this signal on, TERM, HUP and INT are ignored until
    it has (#1016).  give_back() ignored them only once called, and the run's
    own finally runs before it: char-roundtrip.py's writes the boot's log and
    waits up to 10 s for its qemu, and systemctl's second SIGTERM, 2-3 ms
    after the first, raised SystemExit again in there.  Its FIFOs were left at
    every stop of its unit, and in 2 boots of 4 under KVM on omen the lock as
    well.

    It says which signal stopped the run, as take() was told the run's name,
    once: a second such line is a second signal taken, which
    harness-lifecycle-check.sh's tools and unitstop say WRONG -- nothing else
    showed it, give_back() running in the outer finally all the same.  Said
    with os.write() and not print(): a signal can come in the middle of the
    run's own print to stderr, and a print from the handler then raises
    RuntimeError, a reentrant call."""
    for sig in _STOP:
        signal.signal(sig, signal.SIG_IGN)
    try:
        os.write(2, f"{_who}: stopped by {signal.Signals(signum).name}\n".encode())
    except OSError:
        pass
    raise SystemExit(128 + signum)


def remove_at_end(path):
    """`path', a file the run makes -- a FIFO, a monitor's socket -- named
    for give_back() to remove, after the qemu, however the run ends; given
    back as it came (#1016)."""
    _paths.append(path)
    return path


def stop_like_ctrl_c():
    """TERM and HUP raise SystemExit, as INT raises KeyboardInterrupt, so the
    run's finally gives back what it took; one the run was started with
    ignored -- nohup's HUP -- stays ignored."""
    for sig in (signal.SIGTERM, signal.SIGHUP):
        if signal.getsignal(sig) is not signal.SIG_IGN:
            signal.signal(sig, _stop)


def start_qemu(argv, **popen):
    """The run's qemu: subprocess.Popen(argv, **popen), dying with the run
    and stopped by give_back() before the lock goes."""
    q = subprocess.Popen(argv, preexec_fn=dies_with_us(), **popen)
    _machines.append(q)
    return q


def dies_with_us():
    """A preexec_fn for the run's qemu: KILL when this process dies, by a
    signal it does not catch or any other way.  The parent is asked for by
    the PID it had before the fork, so one gone before the prctl is caught
    too."""
    parent = os.getpid()

    def preexec():
        ctypes.CDLL(None).prctl(PR_SET_PDEATHSIG,
                                ctypes.c_ulong(signal.SIGKILL))
        if os.getppid() != parent:
            os._exit(1)
    return preexec
