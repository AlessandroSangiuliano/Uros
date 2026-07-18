# Uros v0.2.0 — release notes

**Released:** 2026-07-XX
**Kernel:** UrMach 0.2.0 (`URMACH_VERSION_STRING`)
**Target arch:** i386 (SMP, up to `NCPUS=64`)
**Companion docs:** [README.md](../README.md), [CHANGELOG.md](../CHANGELOG.md), [docs/uros_design.md](uros_design.md), [docs/lock_audit_smp.md](lock_audit_smp.md)

---

## Headline

v0.1.0 was the release where you could sit at the prompt and type. **v0.2.0 is the release where the machine underneath is the whole machine.** The same multiserver system now boots, schedules, and benchmarks on **32 logical CPUs of an Intel i9-13900K** (hybrid P/E cores), on real hardware, from one kernel binary built at `NCPUS=64`:

```
smp: 31 AP(s) online in … us total (pipelined bring-up)
...
ush$
```

The numbers moved the way SMP numbers are supposed to move. On bare metal, a combined send/receive null RPC completes in **~1.1 µs on a P-core** — in the same class as the old single-CPU kernel, except now there are 31 other CPUs doing useful work at the same time. Idle CPUs sit in HLT instead of burning the package power budget (worth **4–6.4×** on its own — see §Benchmarks). Concurrent page-fault handling parallelizes across cores. And the long list of "works on one CPU, breaks on eight" bugs that every kernel has to pay for — lock-leaks on error paths, TOCTOU wakeups, use-after-free under concurrent close, undefined-behavior shifts — has been hunted down on real silicon, not just under emulation.

This is the release where UrMach stops being "a microkernel that happens to have SMP branches" and becomes a multiprocessor kernel with measured, explained scaling behavior.

---

## Audience and goals

These notes are for:

- **Contributors and reviewers** who want to know what landed and why.
- **Future maintainers** debugging a regression and trying to find the commit that introduced a behavior.
- **People reading the code** who want a map of where the v0.2.0 work lives.

If you only need the user-facing summary, the README's status section is enough. The CHANGELOG covers the issue-by-issue list. This document sits between the two: it tells the story of how the pieces fit together, what motivated each decision, and — this release more than any other — how the bugs were actually caught.

---

## The shift, in one diagram

```
v0.1.0                                   v0.2.0
------                                   ------
1 CPU                                    up to 64 CPUs (32 validated on silicon)

UrMach 0.1.0                             UrMach 0.2.0
  locks compile out on UP                  lock primitives audited for NCPUS>1   (#303)
  one run queue, one pmap lock             pipelined INIT/SIPI AP bring-up       (#367)
  cr3-derived cpu_number                   %gs per-CPU data + SYSENTER ramp      (#321,#348)
  PIT clock                                per-CPU LAPIC timer + IPIs            (#312,#316)
  no idle policy (single CPU)              idle → HLT, HWP on                    (#357,#358)
                                           ipc_space rwlock                      (#327)
                                           per-pmap + split page-table locks     (#329,#330,#338)
                                           lock-free capability lookups          (#331)
                                           futex + futex_waitv                   (#324,#325)
                                           TLB shootdown w/ proven barrier       (#304,#350)

Serial console only                      + GOP framebuffer console (UEFI)        (#342,#371,#372)
                                         + on-screen TTY, Ctrl-Alt-Fn VTs        (#363-#365)

QEMU only                                + bare metal: omen (i7), OMEGA (i9-13900K)
```

---

## What landed, by subsystem

### 1. Bring-up: from power-on to 32 CPUs

**User-visible:** the kernel discovers every CPU via ACPI, starts them all, and prints one line per AP with its bring-up latency and attempt count. A dead AP degrades the boot instead of hanging it.

- **Discovery and startup** (#300, #302, #305, #309, #311). ACPI MADT parsing for CPU enumeration (the 1994 MP table stays as fallback), LAPIC + IPI primitives, per-AP state initialization — CR4, FPU/XSAVE, SYSENTER MSRs, LAPIC LVT — and IOAPIC interrupt routing. Hybrid P/E parts exposed two real-world quirks the emulators never showed: high APIC IDs overflowing `lapic_to_slot[]` (#354) and a signed-shift UB in `cpu_set` that hung the 32-core scale sweep (#355).
- **Modern AP bring-up** (#367, two increments). The MP-spec ceremony (INIT + 10 ms, SIPI + 200 µs, twice) is a 1994 contract; modern CPUs come up in tens of microseconds. Increment 1 replaces the fixed delays with **attempt+retry**: INIT + 10 µs + a single SIPI + TSC-deadline poll, and only if the AP stays silent does the full spec sequence run with a 2-second margin. The retry *is* the compatibility story — no per-family quirk tables. Increment 2 **pipelines** the bring-up: the BSP kicks every AP back-to-back and overlaps their init, pacing only the one truly serial resource — the shared real-mode trampoline — via a counter the AP bumps right after acquiring the boot-stack lock. The trampoline's stack frames are *phase-dependent*: a slower AP's 2-byte return push lands exactly where a faster AP keeps its kernel far-return address, which is a lottery triple-fault unless transit is paced (this is why other kernels put a bit-spinlock in the real-mode stub). Result: ~120× faster per-AP kick under KVM, 31 APs online with visibly concurrent init on OMEGA, and a rendezvous barrier that counts *online* APs, so a failed CPU no longer wedges boot.
- **Early-AP hardening** (#370, #361, #328, #346). An AP that faults before it has a `current_thread` used to dereference NULL in `thread_setrun`; three guards make that path fail loudly instead. `thread_resume` racing embryo parking left threads suspended forever. The 3rd-CPU concurrent bring-up race and an AP-side `%gs`/`cpu_number` window round out the class.

### 2. Per-CPU foundations

**User-visible:** none, by design — this is the floor everything else stands on.

- **`%gs`-based per-CPU data** (#301, #321) replaces the cr3-derived CPU-number model: `cpu_number()` is a segment load, not a page-table walk.
- **Per-CPU SYSENTER trampoline** (#348) removes the per-context-switch `IA32_SYSENTER_ESP` WRMSR — measured at ~144 cycles net on bare metal. The same issue's profiling refuted the eager-XSAVE hypothesis (it fires on ~0.01% of IPC context switches) and extracted an LLDT-reload skip instead.
- **Soft-spl** (#322) takes the interrupt-priority transitions off the IPC fast path.
- **Per-CPU LAPIC timer** (#312) takes the clock off the PIT; sub-tick time interpolation works on every CPU (#314).
- **TLB shootdown** (#304) with the two hard lessons attached: a CPU must not spin on a simple_lock with IPIs masked (#317), and the shootdown/OOL-churn tax on cross-CPU workloads is real and measured (#313).
- **False-sharing pass** (#323) pads the per-CPU kmsg pool counters to cache lines.

### 3. Locking modernization, layer by layer

**User-visible:** the system scales instead of serializing. Concurrent page faults in one address space parallelize almost linearly (2.097 M faults: **~2000 ns/fault at 2 CPUs → 140–400 ns at 7–8 CPUs** on bare metal).

The approach was phased and evidence-first — audit, then convert the hot read paths, then split the writers:

- **Audit** (#303): every lock primitive verified to compile to real atomics at `NCPUS > 1`, written up in [lock_audit_smp.md](lock_audit_smp.md).
- **`ipc_space` reader-writer lock** (#327): port-name resolution is read-mostly; writers are the rare case.
- **pmap** (#329 → #330): the giant pmap lock is gone. Per-pmap locks, lockless read paths, zone allocator magazines, and a dynamic per-CPU allocator (the #330 epic).
- **Capability table** (#331): the splay tree is retired for a radix tree + seqlock — name→entry lookup is lock-free on the fast path.
- **Split page-table locks** (#338): `pmap_enter`/`pmap_remove` on different page tables of the same pmap no longer serialize. The one subtle piece — the barrier ordering a PTE publish against the CPUs-active read on the enter↔activate edge — was **formally validated with herd7** (#350): four litmus tests prove the fence is both necessary and sufficient under x86-TSO, checked by enumerating *all* executions, not by sampling. The tests live in `uros/tools/litmus/` and run as a regression via `run.sh`.
- **futex** (#324, #325): userland synchronization gets a modern kernel primitive — wait/wake/requeue plus `futex_waitv` multi-wait — and libpthreads mutexes/condvars sit on it; the uncontended path never enters the kernel.

### 4. Scheduler, idle, and power

**User-visible:** an idle Uros no longer heats the room, and a busy one runs at boost clocks.

This subsystem is where measurement kept overruling intuition:

- **Idle → HLT** (#357). The idle loop used to PAUSE-spin. On a modern package that is not a neutral choice: 31 spinners eat the shared power budget and drag every core's clock down. Idling CPUs now HLT (a two-phase Dekker handshake against the wakeup path keeps the lost-wakeup window closed). This single change explained the "OMEGA is mysteriously slow" datapoint entirely: **4–6.4× across every IPC suite** on all-32 bare metal, and the freed budget lets the hardware boost autonomously. `-S` restores spin for A/B work.
- **HWP** (#358). Hardware P-states are enabled at bring-up (one-way until cold boot; `-Q` skips, `-E` biases the energy/performance preference). On top of HLT it is worth a further **−20/−40%**, verified in a controlled P-core A/B.
- **The run queue did not contend** (#319). The obvious SMP move — per-CPU run queues — was gated on measurement first, and the measurement said no: the global run-queue lock accounts for **0.2% of samples at saturation** (instrumentation kept in-tree behind a build flag). The real cause of the pre-saturation scaling hump (12–24 threads on 32 CPUs) is **placement**: RPC pairs scattered across idle CPUs pay cold caches and the wake path, and the curve recovers exactly at saturation when every handoff is same-CPU and warm. The issue closed with the diagnosis, and the lever is:
- **Hand-off-on-block** (#356, increment 1). `-P` wakes the RPC partner on the blocking CPU — −11/−12% on same-task suites on bare metal. The full placement work is the natural next-cycle item, now with a precise target.
- **futex under load** (#352, #360): a per-slot pool futex word stranded one of two waiters on a recycled slot; a futex handoff could dispatch a thread still running on another CPU. Both are the kind of bug only oversubscription on real cores exposes.

### 5. The crash hunts — SMP correctness on real silicon

**User-visible:** kill-storms, fork-storms, and concurrent I/O no longer take the system down. The debug tooling that caught each bug ships in the tree.

Most of the v0.2.0 tail was this: run brutal workloads (`kill × exec`, `kill × fork`, concurrent writers, full-bundle boots in a loop) on 8–32 CPUs, and every few thousand iterations something breaks. Each hunt ended with a small fix and usually a reusable technique:

- **Error paths that skip a step the success path keeps.** SIGKILL racing exec's self-terminate leaked an `act_lock` on a bail path and deadlocked both terminators (#383). A systematic audit of every fast-path bail (#386) found one more real bug — the port-name cache honored a *revoked* send right because `is_generation` wasn't bumped on `mod_refs`/`rename`/`copyin` (#390) — and verified the rest clean.
- **TOCTOU on the IPC hot path.** The `mmot_hotpath: bad ith_state` panic: a reply can complete between the unlocked state check and `imq_lock`, so `MACH_MSG_SUCCESS` at that point is a legitimate slow-path outcome, not a corruption signal (#387).
- **The COW window.** SIGKILL racing fork corrupted both sides: COW protection was applied to the top object instead of the backing object (#385). The same hunt found an `i386_set_ldt` call passing a *length* where `vm_map_remove` expects an *end address* — one wired kernel page leaked per exec — and an ext_server fid-exhaustion cured with dead-name notifications.
- **Locks that were never real.** The ext2 server predates its multithreaded life; its historical mutexes were no-ops. Concurrent FLIPC writers corrupted the writeback queue with a 0x04040404-stride signature until the locks became real (`v_lock` + generation counter + `pc_busy`) (#384). Same class in libpthreads: the `free_stacks` list had no lock, so two threads could win the same stack (#377).
- **Plain UB.** AHCI's 32-slot batch built `ci_mask` as `(1u << nslots) - 1`; at `nslots = 32` the shift is undefined and evaluates to a zero mask on i386 — no command issued, reads silently zero-filled, and ush died thousands of steps later (#362). `cpu_set` had a signed shift (#355). Both found the hard way, on hardware.
- **The kernel smashing userland.** `msg_receive_error` copied a fixed 32 bytes without clamping to `rcv_size` — a zero-slack concurrent receiver got its return linkage overwritten *by the kernel* (#374). Fixing it produced the first-ever valid `cc` suite numbers.
- **Tests that could not fail** (#393, #394, #395). The `-smp 8` acceptance pass found that three of four failures were in the tests, not the system: a timedlock test used `thread_switch(DEPRESS)` as if it were synchronization (a UP assumption — libpthreads was correct), and its failure path deadlocked the suite, hiding three later tests; the SHA-NI SHA-256 compress computed **wrong digests** (a missing `SHA256MSG1` in the last two message-schedule quartets — invisible because both HMAC sides used the same wrong function) and its known-answer test didn't gate anything; and the kernel's SHA-NI path ran on the caller's live FPU state under lazy FPU. The kernel is `-mno-sse` for exactly this reason, so hardware SHA-NI was **removed from the kernel entirely** — portable C only, SSE-free by construction — while userland libcap keeps the accelerated path behind a self-test gate. `cap_test` now carries a raw-trap XMM-preservation probe as a permanent regression.

The arsenal that ships with the release: a per-CPU **perf-counter NMI hard-lockup watchdog** (`-W`), **DDB entry doors** that survive SMP (serial break and PS/2 Ctrl+D re-armed via RPC single-pass IPI — #335, #337, #382), **owner-tracked mutexes** (`MUTEX_OWNER_TRACK`), zone poisoning (`-Z`), and the workflow notes for post-mortem gdb via the QEMU gdbstub and TCG `-d int` autopsies.

### 6. Console and bare metal

**User-visible:** Uros boots on real laptops and desktops. On a UEFI machine the kernel paints its own framebuffer console; on a machine with a keyboard you get virtual terminals with Ctrl-Alt-Fn.

- **UEFI boot path** (#342, #343): multiboot2 with ACPI RSDP handoff, hybrid GRUB images (`scripts/make-omen-boot.sh`), and an in-kernel **GOP framebuffer console** for early output on machines with no serial port and no VGA text mode.
- **fbcons at full panel** (#371, #372): native-resolution char-cell renderer with an adaptive 2× font, jump-scroll, and a write-combining framebuffer mapping (an uncached GOP framebuffer is unusable; WC is the difference between a console and a slideshow).
- **On-screen terminal stack** (#363, #364, #365): a real keyboard+GPU console TTY through char_server↔gpu_server, **virtual terminals** on Ctrl-Alt-Fn, and lazy per-VT shells under a per-VT supervisor. The serial console remains the headless path.
- **Bare-metal validation**: omen (i7-gen7, BIOS/CSM) and OMEGA (i9-13900K, pure UEFI, 32 logical CPUs — bench workhorse), both for boot/output/bench. The early triple-fault class on real firmware (#344, #346) is fixed; multiboot1 modules crossing 16 MB no longer get clobbered by the boot page tables (#359). The interactive console + VT stack is validated end-to-end under QEMU (see Known limitations for the bare-metal precondition).

### 7. Storage, proc, and userland under SMP

**User-visible:** the process table doesn't run out, reboot is graceful, and there's a new tool: `cpustat`.

- **proc_server**: pid-table slots are freed at reap — the "254 processes and the shell dies" wall is gone (#378, #389); `fork` reports honest `EAGAIN` instead of returning 0 to the parent on a full table (#389); `reboot` gives SIGTERM handlers time to run instead of force-killing survivors (#379).
- **AHCI + ext2 under concurrency**: the #362/#384/#388 fixes above make the storage stack safe for the multithreaded servers it now serves.
- **cpustat** (#375): per-CPU `processor_info`/`cpu_ticks` verified under SMP, consumed by the **first dynamically-linked userland tool** shipped in the image — the dynamic path (ld-musl + libc.so) is now load-bearing for real utilities.
- **Capability tokens, post-#394/#395**: cap_server mints HMAC-SHA256 bearer tokens; resource servers hold no key and delegate verification to the kernel trap. The kernel side computes that HMAC in portable C (~1 µs per verification, a per-open cost — not on the per-I/O path); userland libcap uses SHA-NI where the CPU has it, gated by a known-answer self-test since #394.

---

## Build and tooling changes

- **`NCPUS=64` is the release configuration** (with `MP_V1_1=1`); the same binary boots 1..64 CPUs. `-c N` caps bring-up for A/B work.
- **Kernel boot flags** grew a family of SMP/power switches: `-S` (idle spin, disable HLT), `-Q` (skip HWP enable), `-E` (HWP EPP bias), `-P` (synchronous-RPC hand-off), `-D` (direct thread switch on SMP — opt-in, it pays a cr3 flush on i386), `-W` (NMI watchdog), `-Z` (zone poisoning), `-K` (PS/2 Ctrl+D DDB break), `-c N` (CPU cap).
- **Boot-flag parsing runs before BSS clear** — boot-flag globals must live in `.data` (#337). Documented because it *will* bite again.
- **smoke at any CPU count**: `scripts/smoke-ush.sh --smp N` (#376) — the acceptance gate runs the full ush pipeline at `-smp 8`.
- **Bench ISOs for input-less machines**: `scripts/make-omen-boot.sh --iso --bench-only <suite>` builds a bootable ISO whose bundle runs a chosen ipc_bench suite at boot, with a GRUB menu exposing the `-c`/`-P`/`-E`/`-Q`/`-S` variants — the workflow that produced every OMEGA number in these notes (dd, boot, photograph the panel).
- **ipc_bench suites** `scale` (concurrency sweep, median-of-5) and `cc` (concurrent pairs) (#351); `cc` produces valid numbers since #374.
- **herd7 litmus regression** in `uros/tools/litmus/` (#350): `run.sh` re-verifies the #338 barrier matrix on every run and fails on any verdict mismatch.
- **MUTEX_OWNER_TRACK** build option: mutexes record their owner for deadlock autopsies.

---

## Benchmarks (bare metal, 2026-07-18, kernel `f5f50186`)

Rules first, because hybrid parts punish sloppy method: OMEGA's P/E scheduling is a per-boot lottery, so **compare absolute ns/RPC at saturated points across boots — never the speedup ratios — and prefer medians / pinned runs**. `-c4` pins bring-up to P-cores and removes the big variance.

**Mach IPC, µs/op (null/128B/1K/4K), ipc_bench micro suites:**

| suite | OMEGA all-32 | OMEGA `-c4` (P-cores) | omen 8/8 |
|---|---|---|---|
| intra | 2.87 / 2.98 / 3.23 / 3.66 | 2.32 / 2.40 / 2.60 / 2.90 | 3.60 / 3.37 / 2.92 / 3.07 |
| slow  | 3.00 / 3.04 / 3.29 / 3.68 | 2.29 / 2.32 / 2.49 / 2.87 | 2.74 / 2.78 / 2.90 / 3.07 |
| inter | 2.04 / 2.59 / 2.25 / 2.57 | 2.12 / 2.18 / 2.36 / 2.67 | 2.94 / 2.51 / 2.62 / 2.78 |
| comb  | 1.47 / 1.49 / 1.51 / 1.63 | **1.23 / 1.08 / 1.09 / 1.14** | 1.68 / 1.68 / 1.70 / 1.82 |

The de-lotteried truth: **a combined null RPC on a P-core is ~1.1 µs** — with 32 CPUs online, on a kernel carrying the full SMP locking stack. (All-32 single runs swing up to ~40% boot-to-boot on this part; the same configuration has produced 0.99 and 1.38 µs on different boots. Medians or `-c4` are the honest instruments.)

**What the power work is worth** (controlled A/Bs on OMEGA):

- Idle → HLT (#357): all-32 comb null **12.39 → 1.93 µs (6.4×)**; every suite 4–6.4×. Even at `-c4`, three spinners cost 1.55×.
- HWP (#358), on top of HLT: **−20/−40%** uniform at `-c4` vs HWP-off.

**Concurrency (`scale`, same-space RPC pairs, ns/RPC, median-of-5):**

| threads | OMEGA (32 CPUs) | omen (8 CPUs) |
|---|---|---|
| 1  | 2586 | 3571 |
| 2  | 2582 | 2845 |
| 4  | 4174 | 5076 |
| 8  | 10308 | 10452 |
| 16 | 32827 | ~10.5k (flat) |
| 24 | 54699 | ~11k (flat) |
| 32 | **12602** | 10096 |

Two findings worth the table: (1) **at all-CPUs-busy both machines converge to the same ~10–12k ns/RPC ceiling** — the same-space serialization floor is machine-independent; (2) OMEGA's 12–24-thread hump is the **placement** effect from §4 (pairs scattered over idle CPUs; it recovers 4.3× the moment the machine saturates and every handoff is warm). The hump is diagnosed, the lever (`-P`) is in the GRUB menu, and the curve is already **2.7× better at 32 threads than two weeks ago** (33.6k → 12.6k ns/RPC). Also worth noting: omen's single-thread latency **halved** since late June (6981 → 3571 ns/RPC) — the accumulated fix dividend, HLT and HWP included.

**Page-fault scaling** (#338 stress, one address space, 2.097 M faults): ~2000 ns/fault at 2 CPUs → **140–400 ns/fault at 7–8 CPUs**. With the old single pmap lock, 8 CPUs faulted at 1-CPU speed.

**FLIPC v2** is unchanged this cycle and keeps its role: Mach IPC wins single-RPC latency, FLIPC v2 owns throughput (#341 closed on exactly that division of labor).

KVM numbers are deliberately absent from this section: KVM lies about time on privileged paths (and post-#357, idle HLT changes host saturation entirely). KVM remains the control-flow instrument; bare metal is the truth instrument.

---

## Known limitations

- **Pure-UEFI machines with no PS/2 input (the OMEGA class) are headless/bench-only.** No USB stack yet (#353) and no fbcons→gpu_server console handoff yet (#369), so such machines have output but no input. This is a declared perimeter of 0.2.0, not a bug.
- **Interactive use lives on QEMU in 0.2.0.** The on-screen console + VT stack (boot to `ush$` on the display, foreground/background jobs, Ctrl-Alt-Fn switching with lazy per-VT shells, clean `shutdown` drain) is validated end-to-end under QEMU with PS/2 + VGA text. Bare-metal interactive additionally requires dedicating a SATA disk with the Uros MBR layout for the root filesystem — there is no USB storage yet, and no current test machine can spare its disk. A precondition, not a bug. (Trying it on a GPT-partitioned disk also surfaced two driver gaps for the backlog: the AHCI IDENTIFY parser reads the 28-bit LBA field — a 1 TB disk reports as 128 GiB — #399, and the partition scanner does not recognize GPT — #400.)
- **Line-discipline signals on the on-screen console**: `^C`/`^Z` generate signals on the serial ctty path; the on-screen VT keyboard does not wire them yet (#397, found by the release validation drive). A cross-session `kill` defect found in the same drive is tracked as #398. Serial job control is unaffected.
- **The 12–24-thread placement hump on 32 CPUs** is understood but not yet engineered away; #356's remaining increments are the plan. Same-space RPC at full saturation sits at the ~10–12k ns/RPC ceiling on both test machines.
- **fork() rough edges from v0.1.0 remain** (#269 class). `uname()` wiring (#296) slipped again.
- **THREAD_SWAPPER still compiles into hot paths** — a post-SMP kernel rework will retire it.
- **i386 only.** x86-64 is not a port of convenience but the next performance floor: PCID (context switches without TLB flush — the reason direct thread switch is `-D` opt-in on i386), SYSCALL, 15 GP registers, higher-half + direct map. That is v0.3.0.

---

## Upgrade and compatibility

- **Disk format unchanged** (MBR + 3 partitions, ext2 + ext2 + swap). v0.1.0 disks boot under v0.2.0.
- **One kernel for UP and SMP.** The release binary is `NCPUS=64`; on a single-CPU machine it behaves as before. There is no UP-only build variant to maintain.
- **Behavioral defaults changed:** idle is HLT (was spin — `-S` reverts), HWP is enabled at bring-up on capable CPUs (`-Q` skips; note HWP enable is one-way until a cold power-off).
- **Kernel banner** is `UrMach 0.2.0 — Uros microkernel`; harnesses that grep the banner must be updated.
- **In-kernel SHA-256 users**: the kernel API is unchanged, but the implementation is portable C only; SHA-NI acceleration exists solely in userland libcap.

---

## What's next — v0.3.0 outlook

v0.3.0 is the **x86-64 port** (milestone open, contracts drafted): PCID, SYSCALL, higher-half kernel with direct map, the larger register file — the floor-lowering move that every remaining IPC microsecond points at. Carried along with it:

- **ipc_space write-side scaling** (#340) and the deferred pmap follow-ups (#318, #349).
- **Wakeup placement, the rest** (#356) — the measured lever for the many-core hump.
- **IPC profiling before IPC redesign** (#392): the register-passing IPC idea (#391) is deliberately gated on profile data, not enthusiasm.
- **USB stack** (#353) and the fbcons→gpu_server handoff (#369) — the two items that turn pure-UEFI machines interactive.
- **login** (#366) and the first multi-user conveniences.

---

## Acknowledgements

UrMach evolves OSF Mach (OSF / CMU heritage licenses preserved where inherited). The SMP work in this release leaned on public architecture documentation and the published practice of mature kernels — studied, compared, never copied — and on **herd7** and the x86-TSO formalization for turning one barrier argument into a proof. The bring-up pipeline, the idle-HLT discovery, and the crash-hunt arsenal were all shaped by one rule that v0.2.0 validated over and over: **measure on real silicon, believe the measurement, write down the technique.**
