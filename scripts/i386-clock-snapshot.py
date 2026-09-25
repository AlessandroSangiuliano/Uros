#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# i386-clock-snapshot.py — the state that tells #599's hypotheses apart, read
# from the LIVE i386 kernel through qemu's gdb stub.
#
#   gdb -batch -x scripts/i386-clock-snapshot.py \
#       uros/build/export/uros/boot/mach_kernel
#
# qemu must have been started with `-s' (run-qemu.sh passes trailing arguments
# to qemu).  SNAP_PORT overrides the stub port (1234).  The script attaches,
# reads and DETACHES: it never kills and never resumes anything it did not
# stop, so the harness can run it twice a few seconds apart on the same stop
# and compare.  A state seen once and gone the second time is a transient
# (softspl_replay caught between clearing the pending bit and unmasking, for
# one), not the stop.
#
# The I/O APIC and the local APICs are NOT read here.  The harness asks qemu
# for them over QMP (info pic, info lapic) before this attaches, because
# attaching stops the machine, and a host-side state can change across a stop
# and a continue.
#
# What it prints, and what each line answers:
#
#   every processor   eip, eflags (IF), esp, ebp, the next instructions and
#                     a backtrace -- where each one is;
#   the tick          nmi_cpu_tick[] per processor, timeout_ticks, the TSC
#                     anchor -- whose tick stopped and whether the clock and
#                     the timer count stopped with it;
#   the level         curr_ipl[] and softspl_pending[] -- a raised level with
#                     the tick pending is #599's second hypothesis;
#   the line          ivect[0] and intpri[0] -- whether hardclock still owns
#                     IRQ 0 (#600: a task can take it);
#   the timers        the first timer elements against timeout_ticks -- an
#                     expired head nobody fired is the softclock thread, not
#                     the tick;
#   RCU               the grace period and queued/retired, which only
#                     hardclock advances on i386;
#   the counters      whatever #599's instruments count, when the kernel has
#                     them (a missing symbol is said, not skipped).
import gdb, os

PORT = os.environ.get("SNAP_PORT", "1234")


def ev(expr):
    return gdb.parse_and_eval(expr)


def say(label, expr, fmt="{}"):
    try:
        print("  %-28s %s" % (label, fmt.format(int(ev(expr)))))
    except gdb.error as e:
        print("  %-28s (not readable: %s)" % (label, str(e).splitlines()[0]))


gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("target remote :%s" % PORT)

threads = sorted(gdb.selected_inferior().threads(), key=lambda t: t.num)
ncpu = len(threads)

print("== processors (%d)" % ncpu)
for t in threads:
    t.switch()
    cpu = t.num - 1
    eip = int(ev("$eip")) & 0xFFFFFFFF
    efl = int(ev("$eflags")) & 0xFFFFFFFF
    print("-- processor %d: eip 0x%08x eflags 0x%08x (interrupts %s) "
          "esp 0x%08x ebp 0x%08x"
          % (cpu, eip, efl, "on" if efl & 0x200 else "off",
             int(ev("$esp")) & 0xFFFFFFFF, int(ev("$ebp")) & 0xFFFFFFFF))
    try:
        gdb.execute("x/3i $pc")
    except gdb.error as e:
        print("  (no instructions: %s)" % str(e).splitlines()[0])
    try:
        gdb.execute("bt 12")
    except gdb.error as e:
        print("  (no backtrace: %s)" % str(e).splitlines()[0])

threads[0].switch()

print("== the tick")
for c in range(ncpu):
    say("nmi_cpu_tick[%d]" % c, "nmi_cpu_tick[%d]" % c)
say("nmi_heartbeat", "nmi_heartbeat")
say("timeout_ticks", "timeout_ticks")
say("rtclock_tsc_at_tick", "rtclock_tsc_at_tick", "0x{:x}")
say("mp_tsc_per_us", "mp_tsc_per_us")

print("== the level")
for c in range(ncpu):
    say("curr_ipl[%d]" % c, "curr_ipl[%d]" % c)
    say("softspl_pending[%d]" % c, "softspl_pending[%d]" % c, "0x{:x}")
for c in range(ncpu):
    say("cpu_data[%d].active_thread" % c,
        "(unsigned int)cpu_data[%d].active_thread" % c, "0x{:08x}")

print("== the line")
say("ivect[0]", "(unsigned int)ivect[0]", "0x{:08x}")
try:
    print("  %-28s %s" % ("  (hardclock is)", str(ev("&hardclock"))))
except gdb.error:
    pass
say("intpri[0]", "intpri[0]")
say("cpus_active", "cpus_active", "0x{:x}")
say("cpus_idle", "cpus_idle", "0x{:x}")

print("== the timers (the first eight, against timeout_ticks)")
try:
    head = ev("&timer_head")
    e = ev("timer_head.chain.next")
    n = 0
    while int(e) != int(head) and n < 8:
        telt = e.cast(ev("(timer_elt_t)0").type)
        print("  elt 0x%08x ticks %d set %d fcn %s"
              % (int(e), int(telt["ticks"]), int(telt["set"]),
                 str(telt["fcn"])))
        e = telt["chain"]["next"]
        n += 1
    if n == 0:
        print("  (empty)")
except gdb.error as e:
    print("  (not readable: %s)" % str(e).splitlines()[0])

print("== RCU")
say("rcu_gp", "rcu_gp")
say("urmach_rcu_queued", "urmach_rcu_queued")
say("urmach_rcu_retired", "urmach_rcu_retired")

print("== #599 counters")
say("ioapic_overlaps", "ioapic_overlaps")
say("ioapic_inside", "ioapic_inside")
for c in range(ncpu):
    say("ioapic_rmw_count[%d]" % c, "ioapic_rmw_count[%d]" % c)
say("spl_to_user_count", "spl_to_user_count")

gdb.execute("detach")
