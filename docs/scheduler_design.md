<!--
  UrMach scheduler modernization — design document.
  Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
  Licensed under the MIT License.
-->

# UrMach scheduler modernization — design doc

**Goal:** a modern, *ultra-performant* SMP scheduler for the UrMach microkernel,
where synchronous RPC between servers is the dominant workload.

**Ground rule (Uros, non-negotiable):** we **study** L4, FreeBSD ULE and
Linux (CFS/EEVDF) for *techniques and ideas*, and **reimplement them natively
and differently** for UrMach. We never copy their code. "Import the technique,
not the source."

---

## 1. Where we are (OSF Mach scheduler, measured)

Mechanisms that work and we keep:
- Priority run queues (`NRQS=128` levels, bitmap + per-level FIFO).
- AST-driven preemption; per-CPU LAPIC clock tick (#312) drives quanta.
- **Direct Thread Switch (DTS, #54):** on a synchronous send to a blocked
  receiver, switch to it immediately (`thread_run`) instead of queueing.

What is dated / the SMP bottleneck (measured in #315/#319):
- **Unbound threads all live on a single global `pset->runq`.** Per-CPU runqs
  (`processor->runq`) exist but are used *only* for bound threads.
- Every hand-off round-trips through that global runq: lock acquire/release +
  cross-CPU cache-line bounce on lock/count, ~15 000×/5 s from both CPUs. This
  is **the** cost of SMP RPC (~96 µs vs ~1.5 µs UP).
- The idle loop **busy-spins polling shared counters** (`pset->runq.count`),
  bouncing the line the active CPU writes.
- After a DTS the sender is still `TH_RUN`, so `thread_dispatch` parks it on a
  runq — the synchronous RPC pair pays a full scheduler round-trip per ping.

**The target is UP latency, not "better than broken SMP".** UP inter-task null
RPC is ~**1.7 µs** (intra ~1.2–1.3 µs). That is the number to approach. The
~96 µs SMP baseline and the 11 µs interim below are *both* far from it — 11 µs
is still ~6.5× over the UP inter-task target. We do not stop at 11 µs.

Hard data (from #315 / #319 increment 1):
- **The ceiling is reachable but not yet the goal:** when the pair aligns,
  per-CPU runqs give `4096B inline RPC = 11 µs`, `mach_port_names = 14.7 µs`
  (~6–10× over the SMP baseline, but still ~6.5× over the ~1.7 µs UP target).
- **Affinity / co-location is a dead end:** forcing the pair onto one CPU
  (do-not-steal window) cuts cross-CPU steals 5× but *doubles* latency — it
  serializes what the split ran in parallel, and the global lock is paid anyway.
- **Eager work-stealing is chaotic:** per-CPU runqs + steal-on-idle boots and is
  correct, but adjacent tests swing **40×** (10 µs ↔ 428 µs). The blocker is the
  *stealing policy*, not the structure.

**Key insight (sharpened by the 1.7 µs target):** a *synchronous* RPC pair is
**inherently serial** — the client blocks until the server replies and vice
versa, so the two threads can *never* run simultaneously. Therefore co-locating
them on one CPU loses **no** parallelism. So the do-not-steal "co-location"
attempt being 2× *worse* (197 µs) was **not** a parallelism loss; it was:
(a) the idle CPU busy-spin-polling shared runq lines, bouncing the active CPU's
cache; (b) collateral serialization of *unrelated* threads forced local; and
(c) the global runq lock still paid on every hand-off. Remove those three and
co-location becomes UP-like. This is exactly why L4 RPC is ~register-copy fast:
the pair runs on one CPU through a direct hand-off, the other CPU does not
interfere, and no run queue is touched.

So the path to ~1.7 µs is to make the RPC fast path **free of cross-CPU
operations**: either (axis B) an L4-style hand-off that never queues the
partner, or (axis A) a *CPU-local, uncontended* runq round-trip — and in BOTH
cases the other CPU must be **non-interfering** (no polling the active CPU's hot
lines, no stealing the fresh RPC partner) while still free to run/steal
genuinely-independent work. **Non-interfering idle is a first-class requirement,
not an optimization.**

Conclusion: **not a from-scratch rewrite — a deliberate modernization** of (a)
where threads are queued, (b) the steal/hand-off *policy*, and (c) idle-CPU
non-interference. The engine is fine; ~1.7 µs (UP inter-task) is the bar.

---

## 2. Techniques to study (and reimplement, never copy)

- **L4 (seL4 / L4Ka / Fiasco) — IPC-aware scheduling.** Synchronous IPC as the
  fast path: direct process switch, **time-slice donation** (client lends its
  slice to the server), treating the RPC pair as a co-scheduled unit so the
  hand-off never touches a run queue. This is the high-leverage idea for a
  microkernel and the most promising lever for our RPC latency.
- **FreeBSD ULE — per-CPU runqueues + load balancing.** Per-CPU queues, an
  interactivity/affinity scoring, and a *pull/push* balancer with hysteresis
  (steal throttling, victim selection) that avoids the mutual-steal thrash we
  hit. Study the *policy*, reimplement.
- **Linux CFS / EEVDF — fairness & vruntime ideas** for the general-purpose
  (non-RPC) timesharing side, if/when we want better fairness than Mach decay.
- **Cache-line discipline** (general): isolate hot polled fields from written
  lock/queue fields (`alignas(64)`) so idle polling does not bounce the active
  CPU's writes.

---

## 3. Target architecture (UrMach-native)

Two complementary axes:

### A. General multiprogramming: per-CPU runqs + a *good* stealing policy
- Route **all** threads (unbound too) to `processor->runq`; the common
  enqueue/dequeue is CPU-local (uncontended lock), the global lock disappears
  from the hot path.
- A disciplined **work-stealing policy** (the hard part):
  - steal only when truly idle and after brief hysteresis (avoid thrash);
  - victim selection (most-loaded peer, not round-robin);
  - throttle steal frequency; never re-introduce the #315 co-location
    serialization.
- Idle CPUs: stop hammering shared counters — cache-line-isolate the polled
  fields; consider `MONITOR/MWAIT` on capable parts.

### B. Microkernel RPC fast path: IPC-aware hand-off (the big win)
- A synchronous RPC pair (client blocked on reply, server blocked on request) is
  a **co-routine**, not two independent threads. The DTS already switches to the
  receiver; the missing half is to **not park the sender on a runq** — mark it
  "blocked, to be resumed by the matching reply" and resume it via a direct
  hand-off (L4-style donation), bypassing the run queue entirely for the ping.
- This avoids both the global-lock round-trip *and* the co-location
  serialization, because there is no queue and no idle CPU contending — exactly
  why L4 RPC is ~register-copy fast.

---

## 4. Phased plan (each increment validated on `--smp 2`: ush$ + full bench + sig_test)

0. **This doc + study notes** (L4 donation, ULE balancer, cache-line layout).
1. **Non-interfering idle + cache-line layout** (`alignas(64)` on `run_queue`/
   `processor` hot fields; stop the idle loop hammering the active CPU's lines;
   consider `MONITOR/MWAIT`): low-risk, first-class requirement, the user's
   padding idea. Measure.
2. **Per-CPU runqs for unbound + disciplined stealing policy** (#319 redone with
   hysteresis/throttle/victim-selection instead of the eager v1, and do NOT
   steal the fresh RPC partner while keeping genuinely-independent work
   stealable). Tame the 40× variance.
3. **IPC-aware hand-off** for synchronous RPC (axis B): sender not queued,
   direct donation resume, partner CPU non-interfering. The microkernel-specific
   lever and the real path to ~1.7 µs.
4. **Fairness/interactivity** revisit (EEVDF-style) for the timesharing side, if
   needed after 2–3.

**Success bar:** inter-task null RPC approaching the **~1.7 µs** UP figure on
`-smp 2` (intra ~1.2–1.3 µs). The 11 µs interim is a milestone, not the goal.

Instrumentation: reuse the per-CPU `s315_*` counters (dts, idle_push, pset_enq,
steal, steal_remote, invoke_xcpu) + periodic dump from the clock tick.

---

## 5. Risks / open questions
- Cross-CPU lock ordering with multiple runq locks (one-lock-at-a-time steal is
  deadlock-free; balancer needs care).
- Priority/fairness across per-CPU queues; starvation bounds.
- Donation accounting (whose quantum/priority runs the donated work?).
- Interaction with bound threads, `IDLEPRI`/idle thread, and the #312 per-CPU
  clock.
- Avoiding the proven traps: co-location serialization (#315), eager-steal chaos
  (#319 incr 1).

## 6. References (studied, not copied)
- L4 / seL4 / Fiasco.OC: synchronous IPC, direct process switch, time donation.
- FreeBSD `sched_ule(4)`: per-CPU runqueues, pull/push balancing.
- Linux CFS / EEVDF: fairness via virtual time.
- Internal: issues #315 (root cause), #319 (per-CPU runq), #312 (per-CPU clock),
  #318 (TSC time source).
