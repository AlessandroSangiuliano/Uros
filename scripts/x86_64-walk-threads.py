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
# 2. Read the frame an entry from ring 3 WRITES, not one a pointer names.
#    act->mact.pcb->user once named the last interrupt's frame: two threads
#    blocked in mach_msg and a futex both showed a rip inside a function
#    neither had called.  The frame the entries write is where the switch
#    points kernel_rsp, pcb->ctx.user_frame: since #770 that is the frame in
#    the pcb itself, and before #770 it was the top of the thread's kernel
#    stack, (pcb->ctx.kernel_stack_top & ~15) - sizeof (struct trap_frame),
#    which on a newer kernel holds C frames.  A kernel without user_frame
#    gets the old address.  rip at +136, rsp at +160.  The kernel stack's
#    words are scanned down from its top, which since #770 is where C begins.
#
# 3. User memory is not readable with `x' while the machine idles on the
#    kernel's CR3.  It is read with a page walk from the task's own root
#    (task->map->pmap->root_pa) through `monitor xp', which the gdb stub
#    forwards to qemu's monitor; PS bit at the 1G and 2M levels.
#
# What a thread says: state (W=1 S=2 R=4 U=8 I=0x80), wait_event (a kernel
# symbol names the queue; a large hash is a futex, and futex_uaddr names the
# word), ith_state 0x10004001 = a receive in progress on saved.receive.mqueue,
# continuation, user_timer/system_timer in 10 ms ticks.  One asleep on a mutex
# has a line of its own with the holder, from own_thr.
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


def waker(v):
    # WAKER_FROM_HOTPATH and WAKER_FROM_DTS are 1 and 2, not addresses.
    v = int(v)
    return {0: "0", 1: "hot-path-claim", 2: "direct-switch"}.get(v) or ksym(v)


def ring(first, nxt, limit=16):
    # A circular list of elements linked through their own field: the element
    # addresses from `first', at most `limit' of them.
    out, e = [], first
    while e and len(out) < limit:
        out.append(e)
        e = nxt(e)
        if e == first:
            break
    return out


def receive_port(th):
    # The queue a receive in progress waits on (#925): the port or the set it
    # was asked on, the count and the limit a send is measured against, the
    # messages on the queue and the receivers on its list, and whether this
    # thread is one of them.  A message it never took is either queued with
    # the receiver missing from the list, or was refused at the limit.
    #
    # Only for a thread asleep in mach_msg_receive_continue: that park writes
    # the queue and the object it waits on (ipc_mqueue_receive() and
    # mach_msg_receive()).  The hot path's park and a kernel receive write
    # neither, and a receive that ended leaves both behind, naming memory that
    # may since be another port's -- nap()'s port, freed, came back as arm 39's.
    st = int(th["ith_state"]) & 0xffffffff
    if st not in (0x10004001, 0x10004011):              # MACH_RCV_IN_PROGRESS[_TIMED]
        return
    if not (int(th["state"]) & 1) or int(th["wait_event"]):
        return                                          # not parked on an IPC queue
    if "mach_msg_receive_continue" not in ksym(th["continuation"]):
        print("      receive parked by a path that records no queue")
        return
    mq = int(th["saved"]["receive"]["mqueue"])
    obj = int(th["saved"]["receive"]["object"])
    try:
        # Every field the two lists are read through, asked for even when a
        # list is empty: an empty one never follows a link, and a name this
        # kernel lacks would otherwise pass unseen until a list had something.
        KM = gdb.lookup_type("struct ipc_kmsg")
        KM["ikm_next"], KM["ikm_header"].type.strip_typedefs()["msgh_id"]
        th.type.strip_typedefs().target()["ith_next"]
        MQ = gdb.Value(mq).cast(gdb.lookup_type("struct ipc_mqueue").pointer())
        kt = KM.pointer()
        msgs = ["%#x:%d" % (k, int(gdb.Value(k).cast(kt)["ikm_header"]["msgh_id"]))
                for k in ring(int(MQ["imq_messages"]["ikmq_base"]),
                              lambda k: int(gdb.Value(k).cast(kt)["ikm_next"]))]
        rcvs = ["%#x%s" % (r, "*" if r == int(th) else "")
                for r in ring(int(MQ["imq_threads"]["ithq_base"]),
                              lambda r: int(gdb.Value(r).cast(th.type)["ith_next"]))]
        lists = "messages=[%s] receivers=[%s] (* this thread)" % (" ".join(msgs), " ".join(rcvs))
        bits = int(gdb.Value(obj).cast(gdb.lookup_type("struct ipc_object").pointer())["io_bits"])
        bits &= 0xffffffff
        otype = (bits >> 16) & 0x7fff
        if otype in (0, 1):                             # IOT_PORT, IOT_PORT_SET
            tname, field = ("struct ipc_port", "ip_messages") if otype == 0 else \
                ("struct ipc_pset", "ips_messages")
            O = gdb.Value(obj).cast(gdb.lookup_type(tname).pointer())
            own = int(O[field].address)
            # The two halves of one record (#925): the object asked on, and
            # the queue the thread sleeps on.  A park that wrote one and not
            # the other names two different things.
            odd = "" if own == mq else " -- its own queue is %#x and the thread's %#x" % (own, mq)
            if otype == 0:
                print("      port %#x active=%d msgcount=%u qlimit=%u seqno=%u pset=%#x %s%s" % (
                    obj, bits >> 31, int(O["ip_msgcount"]) & 0xffffffff,
                    int(O["ip_qlimit"]) & 0xffffffff, int(O["ip_seqno"]) & 0xffffffff,
                    int(O["ip_pset"]), lists, odd))
            else:
                print("      port set %#x active=%d %s%s" % (obj, bits >> 31, lists, odd))
        else:
            print("      receive on object %#x of type %d, queue %#x: %s" % (obj, otype, mq, lists))
    except Exception as x:
        print("      receive queue %#x unreadable: %s" % (mq, x))


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
# The run queues and processor 0's quantum and pending ASTs (#889): a thread
# runnable at a better priority than the one running says nothing until the
# queue it should be on is read beside it.
print("=== pset runq count=%s low=%s depress=%s; cpu0 runq count=%s low=%s; "
      "cpu0 quantum=%s first_quantum=%s state=%s; need_ast[0]=%s" % (
          ev("default_pset.runq.count"), ev("default_pset.runq.low"),
          ev("default_pset.runq.depress_count"),
          ev("processor_array[0].runq.count"), ev("processor_array[0].runq.low"),
          ev("processor_array[0].quantum"), ev("processor_array[0].first_quantum"),
          ev("processor_array[0].state"), ev("need_ast[0]")))
# What else decides whether a tick takes the processor from the thread on it
# (#889): ast_check() asks csw_needed() only while the processor is RUNNING or
# VIDLE, a processor DISPATCHING holds its next thread in next_thread and on no
# queue, and the active thread's activation carries the ASTs that a tick
# propagates -- a suspension among them.
print("=== cpu0 next_thread=%s pset idle_count=%s; active: state=%s act.ast=%s "
      "suspend=%s stops=%s handoff_target=%s handoff_wakee=%s" % (
          ev("processor_array[0].next_thread"), ev("default_pset.idle_count"),
          ev("cpu_data[0].active_thread->state"),
          ev("cpu_data[0].active_thread->top_act->ast"),
          ev("cpu_data[0].active_thread->top_act->suspend_count"),
          ev("cpu_data[0].active_thread->top_act->user_stop_count"),
          ev("cpu_data[0].active_thread->handoff_target"),
          ev("cpu_data[0].active_thread->handoff_wakee")))
# And every queue that holds a thread, head to tail: which thread a processor
# takes next is the head of the best queue.  `links' is a thread's first field.
for rq in ("default_pset.runq", "processor_array[0].runq"):
    for pri in range(32):
        hd = ev("&%s.runq[%d]" % (rq, pri))
        nx = ev("%s.runq[%d].next" % (rq, pri))
        if hd is None or nx is None:
            continue
        head, e, order = int(hd), int(nx), []
        while e != head and len(order) < 64:
            order.append("%#x" % e)
            n = ev("((queue_entry_t) %d)->next" % e)
            if n is None:
                break
            e = int(n)
        if order:
            print("=== %s[%d]: %s" % (rq, pri, " ".join(order)))
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
            ctx = A["mact"]["pcb"]["ctx"]
            kst = int(ctx["kernel_stack_top"]) & ~15
            try:
                fr = int(ctx["user_frame"])          # #770: in the pcb
            except gdb.error:
                fr = kst - 176                       # before #770: atop the stack
            rip = int(ev("*(unsigned long *)%d" % (fr + 136)))
            rsp = int(ev("*(unsigned long *)%d" % (fr + 160)))
            print("  act %#x th %#x state=%#x[%s] wait_event=%s ith_state=%#x mq=%#x futex=%#x "
                  "timer.set=%d utime=%u stime=%u cont=%s urip=%#x %s ursp=%#x" % (
                      a, int(th), st, flags, ksym(th["wait_event"]), int(th["ith_state"]) & 0xffffffff,
                      int(th["saved"]["receive"]["mqueue"]), int(th["futex_uaddr"]), int(th["timer"]["set"]),
                      int(th["user_timer"]["low_bits"]), int(th["system_timer"]["low_bits"]),
                      ksym(th["continuation"]) if int(th["continuation"]) else "0", rip, symb(rip), rsp))
            # What the scheduler chose by (#889): a thread runnable for twenty
            # minutes beside one that ran says nothing until its priority is
            # read next to the other's.
            print("      pri=%d max=%d sched_pri=%d policy=%d depress=%d usage=%u runq=%#x" % (
                int(th["priority"]), int(th["max_priority"]), int(th["sched_pri"]),
                int(th["policy"]), int(th["depress_priority"]), int(th["sched_usage"]) & 0xffffffff,
                int(th["runq"])))
            # Whether it was asked to stop, and by which hand-off it waits
            # (#889): a thread_terminate() that never returned waits for its
            # target's suspension to be taken.  Its own try, so that a field
            # this kernel lacks costs this line and not the thread's.
            try:
                print("      act.ast=%#x suspend=%d stops=%d stop_asked=%d wake_active=%d "
                      "handoff_target=%#x handoff_wakee=%#x" % (
                          int(A["ast"]), int(A["suspend_count"]), int(A["user_stop_count"]),
                          int(A["stop_asked"]), int(th["wake_active"]),
                          int(th["handoff_target"]), int(th["handoff_wakee"])))
            except gdb.error as x:
                print("      act.ast etc. unreadable: %s" % x)
            # Who last ended one of its waits, the IPC park it is in and the
            # one it was last taken off at (#687's record): a receiver taken
            # off its port's list at the park it still sleeps in was handed
            # something and never woken.
            try:
                print("      woke_from=%s cpu=%d result=%d event=%#x park=%u dq=%u wait_from=%s" % (
                    waker(th["woke_from"]), int(th["woke_cpu"]), int(th["woke_result"]),
                    int(th["woke_event"]), int(th["ith_park_seq"]) & 0xffffffff,
                    int(th["ith_dq_seq"]) & 0xffffffff,
                    ksym(th["wait_from"]) if int(th["wait_from"]) else "0"))
            except gdb.error as x:
                print("      waker record unreadable: %s" % x)
            # Who last put the special handler on it and who last asked it to
            # stop, with sched_tick then (#925): an AST_APC on a thread nobody
            # in its task meant to stop.
            try:
                print("      apc_from=%s hold_from=%s apc_by=%#x apc_tick=%u "
                      "stop_asked_by=%#x stop_asked_tick=%u" % (
                          ksym(A["apc_from"]) if int(A["apc_from"]) else "0",
                          ksym(A["hold_from"]) if int(A["hold_from"]) else "0",
                          int(A["apc_by"]), int(A["apc_tick"]),
                          int(A["stop_asked_by"]), int(A["stop_asked_tick"])))
            except gdb.error as x:
                print("      special handler record unreadable: %s" % x)
            receive_port(th)
            # A thread asleep on a mutex: who holds it, from own_thr, as
            # quiet_census does.  That census cannot speak while another
            # task polls, and this one asks no one (#889).
            if int(th["wait_from"]) and "_mutex_lock" in ksym(th["wait_from"]):
                lock = int(th["wait_event"]) - int(ev("(long) &((mutex_t *) 0)->locked"))
                mx = gdb.Value(lock).cast(gdb.lookup_type("mutex_t").pointer())
                print("      mutex %#x held-by=%#x taken-at=%s locked=%d" % (
                    lock, int(mx["own_thr"]), ksym(mx["own_pc"]), int(mx["locked"])))
            for k in range(kst - 8 * 120, kst, 8):
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
