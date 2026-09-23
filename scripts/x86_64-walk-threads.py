#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# x86_64-walk-threads.py — every task and every thread of the LIVE kernel,
# what each one waits on, and (for one task) its user-space stacks (#538).
#
# Run as a gdb script against a qemu started with `-s' (add it to the harness
# invocation: run-x86_64.sh passes trailing arguments to qemu):
#
#   gdb -batch -x scripts/x86_64-walk-threads.py \
#       uros/build-x86_64/export/uros/boot/mach_kernel
#
# Environment: WALK_TASK=0x<task address> and WALK_NM=<nm -n of the PIE binary>
# to also dump that task's threads' user stacks with symbols (PIE bias
# 0x40000000, bootstrap's).  WALK_PORT overrides the stub port (1234).
#
# 🔴 THREE THINGS THIS GOT WRONG BEFORE IT GOT THEM RIGHT (23/09/2026):
#
# 1. Mach queues link the ELEMENT, not the chain field.
#    queue_enter(&pset->tasks, task, task_t, pset_tasks) stores `task':
#    default_pset.tasks.next IS a task_t.  Subtracting offsetof (the Linux
#    list_head convention) reads plausible garbage -- 0xf000ff53f000ff53 is
#    the BIOS IVT.
#
# 2. act->mact.pcb->user is the frame of the last INTERRUPT or trap, not of
#    the syscall the thread is blocked in.  Two threads blocked in mach_msg
#    and a futex both showed a rip inside a function neither had called: an
#    old interrupt frame.  The frame of a syscall is at the top of the
#    thread's kernel stack: (pcb->ctx.kernel_stack_top & ~15) - sizeof
#    (struct trap_frame); rip at +136, rsp at +160.
#
# 3. User memory is not readable with `x' while the machine idles on the
#    kernel's CR3.  It is read with a page walk from the task's own root
#    (task->map->pmap->root_pa) through `monitor xp', which the gdb stub
#    forwards to qemu's monitor; PS bit at the 1G and 2M levels.
#
# What a thread says: state (W=1 S=2 R=4 U=8 I=0x80), wait_event (a kernel
# symbol names the queue; a large hash is a futex, and futex_uaddr names the
# word), ith_state 0x10004001 = a receive in progress on saved.receive.mqueue,
# continuation, user_timer/system_timer in 10 ms ticks.
import gdb, os, bisect

PORT = os.environ.get("WALK_PORT", "1234")
TASK = int(os.environ["WALK_TASK"], 16) if os.environ.get("WALK_TASK") else None
NM = os.environ.get("WALK_NM")
BIAS = 0x40000000

syms = []
if NM:
    for l in open(NM):
        p = l.split()
        if len(p) == 3 and p[1] in "tTwWbBdD":
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
addrs = [a for a, _ in syms]


def symb(v):
    off = v - BIAS
    if syms and 0 <= off < 0x100000:
        i = bisect.bisect_right(addrs, off) - 1
        return "%s+%#x" % (syms[i][1], off - syms[i][0]) if i >= 0 else "?"
    return ""


def ev(e):
    try:
        return gdb.parse_and_eval(e)
    except Exception:
        return None


def ksym(addr):
    try:
        s = gdb.execute("info symbol %d" % int(addr), to_string=True).strip()
        return s.split(" in section")[0] if "No symbol" not in s else "%#x" % int(addr)
    except Exception:
        return "?"


def xp(pa, n):
    out = gdb.execute("monitor xp /%dgx %#x" % (n, pa), to_string=True)
    vals = []
    for l in out.splitlines():
        if ":" in l:
            vals += [int(t, 16) for t in l.split(":", 1)[1].split()]
    return vals


def va2pa(root, va):
    tbl = root & 0x000ffffffffff000
    for shift in (39, 30, 21, 12):
        e = xp(tbl + ((va >> shift) & 511) * 8, 1)[0]
        if not (e & 1):
            return None
        if shift in (30, 21) and (e & 0x80):
            mask = (1 << shift) - 1
            return (e & 0x000fffffffffffff & ~mask) + (va & mask)
        tbl = e & 0x000ffffffffff000
    return tbl + (va & 0xfff)


def uread(root, va, n):
    vals = []
    while n > 0:
        pa = va2pa(root, va)
        room = min(n, (0x1000 - (va & 0xfff)) // 8)
        vals += [None] * room if pa is None else xp(pa, room)
        va += room * 8
        n -= room
    return vals


def show_words(root, va, n, tag):
    print("--- %s: %d words from %#x" % (tag, n, va))
    for i, v in enumerate(uread(root, va, n)):
        if v is None:
            print("  %#x: <not resident>" % (va + 8 * i))
        elif v:
            print("  %#x: %#018x  %s" % (va + 8 * i, v, symb(v)))


gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("target remote :" + PORT)
print("=== active=%s task_count=%s sched_tick=%s" % (
    ev("cpu_data[0].active_thread"), ev("default_pset.task_count"), ev("sched_tick")))
tt = gdb.lookup_type("struct task").pointer()
at = gdb.lookup_type("struct thread_activation").pointer()
head = int(ev("&default_pset.tasks"))
q = int(ev("default_pset.tasks.next"))
nt = 0
chosen = []
while q != head and nt < 64:
    t = gdb.Value(q).cast(tt)
    print("task %#x acts=%d active=%d map=%#x" % (q, int(t["thr_act_count"]), int(t["active"]), int(t["map"])))
    ahead = int(ev("&((struct task *)%d)->thr_acts" % q))
    a = int(t["thr_acts"]["next"])
    na = 0
    while a != ahead and na < 64:
        A = gdb.Value(a).cast(at)
        th = A["thread"]
        try:
            st = int(th["state"])
            flags = "".join(k for k, b in (("W", 1), ("S", 2), ("R", 4), ("U", 8), ("I", 0x80)) if st & b)
            kst = int(A["mact"]["pcb"]["ctx"]["kernel_stack_top"])
            fr = (kst & ~15) - 176
            rip = int(ev("*(unsigned long *)%d" % (fr + 136)))
            rsp = int(ev("*(unsigned long *)%d" % (fr + 160)))
            print("  act %#x th %#x state=%#x[%s] wait_event=%s ith_state=%#x mq=%#x futex=%#x "
                  "timer.set=%d utime=%u stime=%u cont=%s urip=%#x %s ursp=%#x" % (
                      a, int(th), st, flags, ksym(th["wait_event"]), int(th["ith_state"]) & 0xffffffff,
                      int(th["saved"]["receive"]["mqueue"]), int(th["futex_uaddr"]), int(th["timer"]["set"]),
                      int(th["user_timer"]["low_bits"]), int(th["system_timer"]["low_bits"]),
                      ksym(th["continuation"]) if int(th["continuation"]) else "0", rip, symb(rip), rsp))
            for k in range(fr - 8 * 120, fr, 8):
                s = ksym(int(ev("*(unsigned long *)%d" % k)))
                if "+" in s and "usimple_lock" not in s:
                    print("      k %s" % s)
            if q == TASK:
                chosen.append(rsp)
        except Exception as x:
            print("  act %#x unreadable: %s" % (a, x))
        a = int(A["thr_acts"]["next"])
        na += 1
    q = int(t["pset_tasks"]["next"])
    nt += 1
print("=== %d tasks walked" % nt)
if TASK is not None and chosen:
    root = int(ev("((struct task *)%d)->map->pmap->root_pa" % TASK))
    print("=== task %#x root_pa=%#x" % (TASK, root))
    for rsp in chosen:
        if rsp:
            show_words(root, rsp, 0x400 // 8, "user stack from rsp %#x" % rsp)
gdb.execute("detach")
