# Uros SMP IPC/FLIPC2 scaling benchmark — 1→12 CPU (median N=3)

**Host**: pavillion — AMD Ryzen 5 4600H (6 core / 12 thread SMT), KVM, QEMU 11.0, GCC 16.1.1.
**Kernel**: branch `feature/302-tsc-bringup-delay` (TSC bring-up delay + SHA-NI stack-align fix).
**Date**: 2026-06-20. `./scripts/run-qemu.sh --bench --ahci -nographic -serial mon:stdio --smp N`, 3 runs per N, **median**.
**Units**: µs/op except where `(ns)` is indicated; `(ns/RPC)` for concurrent. Lower = faster.

> 🔴 **THIS i386 COLUMN HAS TWO ERAS, AND THE ONE BELOW IS THE FIRST (#560).**
>
> Every i386 number in this document was taken **before** the switch
> carried the state of the FPU. Until that commit the restore was *lazy*:
> `CR0.TS` armed at the switch and the state reloaded by `#NM` at the first
> floating-point instruction of the new thread — which is the pattern of
> **CVE-2018-3665**, and which made i386 fast here *because it was not safe*.
>
> Made eager, measured today (18/09/2026) on the same machine, KVM, 6 boots per
> arm **alternated**, clock sampled at 3993 MHz on both:
>
> | row | before (this document) | after | cost |
> |---|--:|--:|--:|
> | `smp1` futex WAKE_WAIT ping-pong | 0.678 | 0.852 | **+174 ns** |
> | `smp1` semaphore ping-pong | 2.625 | 2.790 | **+165 ns** |
> | `smp4` futex WAKE_WAIT ping-pong | 0.778 | 0.917 | **+139 ns** |
> | `smp4` semaphore ping-pong | 2.150 | 2.365 | **+215 ns** |
>
> 🔑 It is **the same number four times**: the cost is *per switch*, and a round
> trip is two switches in every row, so it comes out as the same absolute figure
> however slow the row is. That is why the futex shows +26% and the semaphore
> +6% for the same thing.
>
> ⇒ **An x86-64-against-i386 comparison that uses the rows below underestimates
> i386 by ~170 ns per round trip.** The rows stay as they are because they are true
> of what they were, not because they are still the baseline.
>
> ⚠️ And the i386 numbers below are from **20/06**: the median of 3 runs
> in a row does not hold the machine still (the build heats the package and the
> boost comes back only when it cools down), so a retake has to be done with
> **alternated** arms and with the clock sampled during every boot.

> **Notes**
> - Median of 3 runs (removes single-run outliers). Above 6 vCPUs = oversubscription (host 6c/12t).
> - The `Intra-task null RPC` outlier of ~15µs at smp1/2 is a constant warmup of the first sample of that suite (it stays in the median too).
> - Under KVM the lock-free win of #331 is NOT visible (no real cache coherence → see #332 bare-metal).

## IPC — all the measurements

| Section | Metric | smp1 | smp2 | smp4 | smp6 | smp12 |
|---|---|--:|--:|--:|--:|--:|
| Raw syscall | mach_null (noop trap) | 0.11 | 0.13 | 0.14 | 0.15 | 0.17 |
| | mach_print("") trap | 0.13 | 0.64 | 4.73 | 9.14 | 10.91 |
| Intra-task | null RPC | 15.65 | 18.61 | 4.38 | 5.16 | 7.31 |
| | 128B | 2.29 | 2.28 | 2.91 | 5.54 | 9.19 |
| | 1024B | 2.36 | 2.34 | 3.53 | 5.28 | 7.64 |
| | 4096B | 2.42 | 2.47 | 3.97 | 5.99 | 7.71 |
| Slow-path receive | null RPC | 2.30 | 2.52 | 3.88 | 5.13 | 7.48 |
| | 128B | 2.31 | 2.46 | 3.89 | 5.30 | 7.72 |
| | 1024B | 2.40 | 2.65 | 4.00 | 5.70 | 8.01 |
| | 4096B | 2.93 | 3.09 | 4.01 | 5.96 | 7.48 |
| Inter-task | null RPC | 3.07 | 3.04 | 4.82 | 4.86 | 7.44 |
| | 128B | 3.11 | 3.06 | 4.24 | 4.89 | 6.11 |
| | 1024B | 3.26 | 3.31 | 3.96 | 4.91 | 6.55 |
| | 4096B | 3.33 | 3.44 | 3.90 | 5.03 | 7.22 |
| Hotpath SEND\|RCV | null RPC | 1.40 | 1.45 | 2.26 | 2.90 | 3.84 |
| | 128B | 1.41 | 1.44 | 2.29 | 2.86 | 3.58 |
| | 1024B | 1.45 | 1.54 | 2.37 | 2.87 | 5.79 |
| | 4096B | 1.51 | 1.53 | 2.31 | 2.97 | 5.35 |
| Concurrent same-space (ns/RPC) | x1 | 2260 | 2399 | 3582 | 4576 | 7160 |
| | x2 | 2544 | 2278 | 2294 | 2702 | 3887 |
| | x4 | 2204 | 2244 | 2286 | 2596 | 3087 |
| Port ops | port alloc+destroy | 1.24 | 1.33 | 1.57 | 1.66 | 2.03 |
| | mach_port_names() | 8.57 | 24.36 | 50.47 | 42.52 | 121.27 |
| PP intra | null (no PP) | 2.22 | 2.33 | 3.59 | 5.02 | 7.99 |
| | null (w/ PP) | 2.21 | 2.28 | 3.63 | 5.04 | 8.23 |
| | 128B (no PP) | 2.23 | 2.27 | 3.64 | 5.16 | 7.34 |
| | 128B (w/ PP) | 2.23 | 4.19 | 3.61 | 5.25 | 6.85 |
| | 1024B (no PP) | 2.27 | 2.30 | 3.82 | 5.25 | 9.01 |
| | 1024B (w/ PP) | 2.28 | 2.29 | 3.85 | 5.03 | 8.99 |
| | 4096B (no PP) | 2.35 | 2.35 | 3.39 | 5.57 | 9.76 |
| | 4096B (w/ PP) | 2.35 | 2.35 | 3.33 | 5.37 | 8.80 |
| PP inter | null (no PP) | 3.06 | 3.02 | 4.63 | 4.73 | 6.84 |
| | null (w/ PP) | 3.04 | 3.05 | 4.51 | 4.77 | 6.68 |
| | 128B (no PP) | 3.06 | 3.05 | 4.76 | 4.71 | 6.61 |
| | 128B (w/ PP) | 3.05 | 3.08 | 4.39 | 4.85 | 6.33 |
| | 1024B (no PP) | 3.23 | 3.23 | 4.68 | 5.03 | 7.52 |
| | 1024B (w/ PP) | 3.23 | 3.27 | 4.78 | 5.08 | 8.62 |
| | 4096B (no PP) | 3.32 | 3.37 | 4.62 | 5.40 | 8.35 |
| | 4096B (w/ PP) | 3.34 | 3.34 | 4.80 | 5.25 | 7.65 |
| OOL intra (PHYS_COPY) | 4 KB | 3.92 | 3.92 | 6.13 | 19.98 | 68.95 |
| | 16 KB | 3.91 | 3.94 | 5.71 | 20.69 | 66.82 |
| | 64 KB | 3.92 | 3.91 | 5.82 | 20.54 | 73.97 |
| OOL inter (PHYS_COPY) | 4 KB | 3.37 | 3.44 | 4.84 | 5.15 | 9.44 |
| | 16 KB | 3.35 | 3.39 | 4.85 | 5.13 | 7.72 |
| | 64 KB | 5.24 | 5.16 | 6.66 | 6.91 | 13.07 |
| Disk raw read | 1 KB | 235.62 | 280.76 | 189.67 | 254.49 | 630.30 |
| | 4 KB | 234.14 | 224.61 | 175.06 | 241.73 | 276.88 |
| | 64 KB | 319.31 | 252.68 | 218.11 | 279.71 | 359.33 |
| Disk raw write | 1 KB | 236.59 | 244.92 | 168.27 | 249.29 | 454.58 |
| | 4 KB | 267.04 | 256.25 | 182.44 | 282.49 | 505.37 |
| | 64 KB | 288.93 | 245.56 | 205.12 | 236.36 | 626.47 |
| Disk ext2 | open+close | 806.81 | 908.40 | 997.59 | 985.45 | 1914.41 |
| | open non-existent | 1110.52 | 1251.13 | 1312.96 | 1353.44 | 2751.19 |
| | open+read+close (3 RPC) | 1201.66 | 951.50 | 977.09 | 1035.55 | 1592.02 |
| | open_read+close (2 RPC) | 2859.69 | 1002.08 | 971.09 | 1061.26 | 1685.17 |
| | open_read+read_close (2) | 1758.99 | 5260.26 | 971.15 | 1036.83 | 1771.12 |
| | write+sync (durable) | 671.99 | 762.84 | 640.52 | 818.57 | 4227.19 |
| | write+sync (2 open,1 dirty) | 427.35 | 553.98 | 368.47 | 469.63 | 1279.66 |

## FLIPC v2 — all the measurements

| Section | Metric | smp1 | smp2 | smp4 | smp6 | smp12 |
|---|---|--:|--:|--:|--:|--:|
| futex vs sem | futex WAKE_WAIT ping-pong | 0.43 | 1.23 | 1.48 | 2.20 | 6.67 |
| | semaphore ping-pong | 2.67 | 3.37 | 4.23 | 5.84 | 9.53 |
| Throughput no-kernel | null desc batch=1 (ns) | 29 | 34 | 34 | 36 | 40 |
| | null desc batch=16 (ns) | 5 | 6 | 6 | 6 | 9 |
| | null desc batch=64 (ns) | 4 | 5 | 5 | 5 | 8 |
| Throughput with data | 128B prod+cons (ns) | 32 | 38 | 39 | 40 | 42 |
| | 1024B prod+cons (ns) | 46 | 50 | 52 | 55 | 68 |
| | 4096B prod+cons (ns) | 94 | 110 | 110 | 115 | 520 |
| Intra RPC (semaphore) | null | 1.55 | 1.76 | 2.67 | 4.35 | 6.50 |
| | 128B | 1.88 | 2.31 | 3.05 | 5.22 | 6.59 |
| | 1024B | 1.89 | 2.69 | 3.46 | 5.64 | 6.89 |
| | 4096B | 1.99 | 2.71 | 3.45 | 6.06 | 7.27 |
| Inter RPC (vm_remap) | null | 2.53 | 3.19 | 3.78 | 4.84 | 5.37 |
| | 128B | 2.89 | 3.52 | 4.17 | 5.14 | 5.73 |
| | 1024B | 2.92 | 3.54 | 4.55 | 5.38 | 6.66 |
| | 4096B | 3.04 | 3.68 | 4.70 | 5.60 | 9.19 |
| Inter RPC (futex) | null | 2.33 | 2.84 | 3.14 | 4.34 | 10.12 |
| | 128B | 2.74 | 3.30 | 3.49 | 4.78 | 4.81 |
| | 1024B | 2.76 | 3.25 | 3.58 | 4.80 | 5.76 |
| | 4096B | 2.87 | 3.46 | 3.77 | 4.96 | 6.01 |
| Inter BATCH (vm_remap) | batch=1 | 2.46 | 3.18 | 3.81 | 4.58 | 4.60 |
| | batch=16 (ns) | 182 | 221 | 282 | 335 | 397 |
| | batch=64 (ns) | 67 | 78 | 87 | 108 | 129 |
| Game sim | per draw cmd (ns) | 5 | 6 | 6 | 6 | 8 |
| | texture 16KB (ns) | 293 | 344 | 345 | 350 | 434 |
| | audio 4KB PCM (ns) | 94 | 109 | 108 | 111 | 141 |
| Isolated channel | null (isolated intra) | 1.52 | 1.81 | 2.68 | 4.17 | 5.47 |
| | 128B (isolated intra) | 1.86 | 2.65 | 3.15 | 5.01 | 4.80 |
| | null (isolated inter) | 2.50 | 3.16 | 4.02 | 4.21 | 4.35 |
| | 128B (isolated inter) | 2.88 | 3.54 | 4.39 | 5.01 | 5.01 |
| Endpoint | create+destroy | 18.49 | 21.69 | 23.15 | 24.45 | 66.78 |
| | null (endpoint) | 2.46 | 3.15 | 3.76 | 4.66 | 4.78 |
| | 128B (endpoint) | 2.89 | 3.52 | 4.38 | 5.14 | 5.28 |
| Buffer group | alloc+free (ns) | 12 | 13 | 15 | 14 | 14 |
| | 256B (bufgroup) | 1.54 | 2.34 | 3.04 | 4.62 | 5.94 |
| | 256B (bufgroup inter) | 2.49 | 3.21 | 4.02 | 1.51 | 1.88 |

## Reading

- **More CPUs = slower per-op**, monotonic on almost everything (host oversubscription >6 vCPU + contention on shared kernel state).
- **Worst of all: OOL PHYSICAL_COPY intra** (4KB 3.92→68.95µs, ~18×) → signal for **#338** (split page-table locks, pmap writers).
- `mach_port_names()` ~14× and `concurrent same-space` degrade: a path already made lock-free by **#331**, but KVM does not show the win → **#332** (bare-metal).
- **FLIPC2 user-space throughput (no kernel) almost flat** (4→8 ns) + **futex hand-off** that scales better than the kernel-mediated paths → empirical proof of the **AMP/multikernel** direction (less shared state = scales better).
- Clean scaling measurement (no oversubscription) = **OMEGA (i9 32-core)**.


---

# 2026-09-13 — x86-64, smp1: the early-out of `spl_replay()` (#454, found by #392)

**Host**: pavillion — **the same machine as the table above**, AMD Ryzen 5 4600H (6 core / 12 thread), KVM, QEMU 11.0.
**Kernel**: branch `feature/454-spl-replay-early-out` = epic `v0.3.0-x86-64` + a guard in `spl_replay()`.
**Date**: 2026-09-13. `~/uros-tests/431-mediana.sh x64 3 1` — 3 boots per arm, **median**, **both arms in the same session**.
**Condition**: governor `performance` + `boost=1`, verified **before and after** each campaign; frequency sampled during (412 and 408 samples). On battery.
**Bundle**: **bench ALONE** (`bootstrap.conf` = `name_server` + `ipc_bench`), like the June baselines.
**Unit**: µs/op. Lower = faster.

🔴 **THE PHASE TABLES ARE IN TSC TICKS, and the µs are the derived
column.** The rest of the document is in µs/op, which is what the bench prints;
the per-phase breakdown is taken by `rdtsc`, and the natural unit of those rows
is the tick — that is where the resolution shows and where the declared cost
of the instrument is expressed.

🔴🔴 **AND THE DIVISOR IS NOT THE CORE'S FREQUENCY.** The first draft of these
tables divided by the **3993 MHz** sampled during the run, which is the
frequency the *core* ran at. Wrong by 33%: the TSC does not follow the core, and
this kernel measures it by itself at every boot against the 8254 —

    UrMach x86-64: timestamp counter measured against the 8254 at 2999 and 2998 MHz

that is, the **base** frequency of the 4600H, not the boost. ⇒ `µs = tick / 2999`.

🔑 We already had the proof that the TSC does not follow the core the same day:
when the laptop went to the battery profile and the core dropped to 1.4 GHz, the
counts went **UP** by 2.93× for the same work. If the TSC followed the core
they would have stayed the same.

❌ **And the cross-check I was pleased with passed for the wrong
reason.** With the wrong divisor the whole trap came out at 1.19 µs against the 1.35
µs/op of the bench and it looked like a nice agreement. With the right divisor it is **1.59 µs**
against 1.35 — and that makes more sense, because the trap is measured **with** the instrument
inside (21 marks) and the bench row without. An agreement that comes out right by chance is more
dangerous than a disagreement.

⚠️ **The grain is 30 ticks = 0.010 µs**: every figure is a multiple of it, and a row
that reads 60 ticks is *two grains*, that is, at the floor of the resolution — "too
little to be measured here", not "measured at 60".

## The checks, before the results

The rows that do **not** block and do not lower the interrupt level do not
move. Had they moved, the measurement would have been of something else.

| check | before | after |
|---|--:|--:|
| `mach_null` (bare trap) | 0.03 | 0.03 |
| `mach_print("")` | 0.05 | 0.05 |
| `port alloc + destroy` | 0.36 | 0.36 |
| FLIPC2 throughput no-kernel (batch=1) | 0.03 | 0.03 |
| FLIPC2 128B produce+consume | 0.03 | 0.03 |

And the correctness check, which is not "the suites are green": the self-test of the
deferred interrupts of #522 is the only one that exercises the replay **with pending bits**, and it is
green at `-smp 4` — *"lowering replayed 10 of the 10 owed, handler ran 10 more
times"*, ticks `99 99 99 99`. Without that line, everything else being green would
have been compatible with "the replay no longer happens".

## All the measurements

| Section | Metric | before | after | Δ |
|---|---|--:|--:|--:|
| #324 futex vs Mach semaphore ping-pong (block+wake round-trip) | futex WAKE_WAIT ping-pong | 1.34 | 0.58 | -57% |
|  | semaphore ping-pong | 4.87 | 1.95 | -60% |
| Combined SEND\|RCV intra-task (hotpath) | 1024B inline RPC | 1.70 | 0.91 | -46% |
|  | 128B inline RPC | 1.67 | 0.91 | -46% |
|  | 4096B inline RPC | 1.71 | 0.99 | -42% |
|  | null RPC | 1.65 | 0.87 | -47% |
| FLIPC2 buffer group benchmarks | 256B RPC (bufgroup inter) | 2.76 | 2.06 | -25% |
|  | 256B RPC (bufgroup) | 1.84 | 0.90 | -51% |
|  | bufgroup alloc+free | 0.01 | 0.01 | +0% |
| FLIPC2 endpoint benchmarks | 128B RPC (endpoint) | 2.80 | 1.95 | -30% |
|  | endpoint create+destroy | 13.62 | 12.40 | -9% |
|  | null RPC (endpoint) | 2.81 | 1.98 | -30% |
| FLIPC2 game simulation (intra-task throughput) | audio 4KB PCM frame | 0.10 | 0.10 | +0% |
|  | per draw command | 0.00 | 0.00 | below resolution |
|  | texture 16KB chunk | 0.29 | 0.29 | +0% |
| FLIPC2 inter-task BATCH (vm_remap, amortized) | batch=1 (inter) | 2.82 | 1.87 | -34% |
|  | batch=16 (inter) | 0.22 | 0.14 | -36% |
|  | batch=64 (inter) | 0.07 | 0.06 | -14% |
| FLIPC2 inter-task RPC (urmach_futex hand-off) | 1024B RPC (inter futex) | 2.49 | 1.75 | -30% |
|  | 128B RPC (inter futex) | 2.50 | 1.71 | -32% |
|  | 4096B RPC (inter futex) | 2.73 | 1.89 | -31% |
|  | null RPC (inter futex) | 2.38 | 1.90 | -20% |
| FLIPC2 inter-task RPC (vm_remap shared memory) | 1024B RPC (inter) | 2.83 | 1.93 | -32% |
|  | 128B RPC (inter) | 2.80 | 1.90 | -32% |
|  | 4096B RPC (inter) | 2.97 | 2.10 | -29% |
|  | null RPC (inter) | 2.81 | 1.96 | -30% |
| FLIPC2 intra-task RPC (semaphore path) | 1024B RPC (intra) | 1.90 | 0.92 | -52% |
|  | 128B RPC (intra) | 1.85 | 0.91 | -51% |
|  | 4096B RPC (intra) | 2.00 | 1.04 | -48% |
|  | null RPC (intra) | 1.92 | 0.93 | -52% |
| FLIPC2 isolated channel RPC | 128B RPC (isolated inter) | 2.86 | 1.94 | -32% |
|  | 128B RPC (isolated intra) | 1.85 | 0.89 | -52% |
|  | null RPC (isolated inter) | 2.83 | 1.89 | -33% |
|  | null RPC (isolated intra) | 1.83 | 0.89 | -51% |
| FLIPC2 throughput (single-thread, no kernel) | null desc (batch=1) | 0.03 | 0.03 | +0% |
|  | null desc (batch=16) | 0.00 | 0.00 | below resolution |
|  | null desc (batch=64) | 0.00 | 0.00 | below resolution |
| FLIPC2 throughput with data | 1024B produce+consume | 0.04 | 0.04 | +0% |
|  | 128B produce+consume | 0.03 | 0.03 | +0% |
|  | 4096B produce+consume | 0.10 | 0.09 | -10% |
| Inter-task (task-to-task) | 1024B inline RPC | 4.43 | 2.54 | -43% |
|  | 128B inline RPC | 4.25 | 2.39 | -44% |
|  | 4096B inline RPC | 4.51 | 2.65 | -41% |
|  | null RPC | 4.23 | 2.36 | -44% |
| Intra-task (thread-to-thread) | 1024B inline RPC | 3.63 | 1.50 | -59% |
|  | 128B inline RPC | 3.41 | 1.51 | -56% |
|  | 4096B inline RPC | 3.49 | 1.53 | -56% |
|  | null RPC | 3.38 | 1.44 | -57% |
| Kernel RPC (where the MIG checks are) | mach_port_type (kernel RPC) | 0.28 | 0.31 | +11% |
| OOL data (inter-task, PHYSICAL_COPY) | 16 KB OOL inter | 4.01 | 2.71 | -32% |
|  | 4 KB OOL inter | 4.19 | 2.75 | -34% |
|  | 64 KB OOL inter | 9.29 | 8.43 | -9% |
| OOL data (intra-task, PHYSICAL_COPY) | 16 KB OOL | 3.25 | 1.92 | -41% |
|  | 4 KB OOL | 3.23 | 1.85 | -43% |
|  | 64 KB OOL | 3.69 | 2.07 | -44% |
| PP inter-task | 1024B (no PP) | 3.87 | 2.55 | -34% |
|  | 1024B (w/ PP) | 4.16 | 2.50 | -40% |
|  | 128B (no PP) | 3.73 | 2.34 | -37% |
|  | 128B (w/ PP) | 3.84 | 2.36 | -39% |
|  | 4096B (no PP) | 3.90 | 2.59 | -34% |
|  | 4096B (w/ PP) | 3.95 | 2.60 | -34% |
|  | null (no PP) | 3.66 | 2.34 | -36% |
|  | null (w/ PP) | 3.67 | 2.43 | -34% |
| PP intra-task | 1024B (no PP) | 2.84 | 1.44 | -49% |
|  | 1024B (w/ PP) | 2.83 | 1.43 | -49% |
|  | 128B (no PP) | 2.97 | 1.42 | -52% |
|  | 128B (w/ PP) | 2.97 | 1.42 | -52% |
|  | 4096B (no PP) | 2.98 | 1.52 | -49% |
|  | 4096B (w/ PP) | 3.05 | 1.49 | -51% |
|  | null (no PP) | 2.77 | 1.40 | -49% |
|  | null (w/ PP) | 2.76 | 1.38 | -50% |
| Port operations | mach_port_names() | 3.37 | 3.53 | +5% |
|  | port alloc + destroy | 0.36 | 0.36 | +0% |
| Raw syscall (no IPC) | mach_null (noop trap) | 0.03 | 0.03 | +0% |
|  | mach_print("") trap | 0.05 | 0.05 | +0% |
| Slow-path receive (continuation path) | 1024B inline RPC (receiver blocked) | 3.42 | 1.46 | -57% |
|  | 128B inline RPC (receiver blocked) | 3.39 | 1.44 | -58% |
|  | 4096B inline RPC (receiver blocked) | 3.55 | 1.50 | -58% |
|  | null RPC (receiver blocked) | 3.34 | 1.44 | -57% |

⚠️ `mach_port_names()` +5% and `mach_port_type` +11% are the two rows that go up:
neither of the two blocks, both are within the noise between boots of this
suite, and neither has a measured explanation. They stay written like this instead of
being rounded to zero.

## Concurrent same-space (#327) — labels separately

The bench prints the raw time inside the label, so every boot has a different
row and the extractor does not pair them up. Medians over the three boots, computed
by hand from the logs:

| | before | after | Δ |
|---|--:|--:|--:|
| x1, 10000 RPC | 33.53 ms | 14.18 ms | −58% |
| x2, 20000 RPC | 68.50 ms | 27.50 ms | −60% |
| x4, 40000 RPC | 136.49 ms | 55.12 ms | −60% |

## Against the i386 baseline of 20/06 (the table at the top of this file)

Same machine, same accelerator, same bundle, same methodology
(median of 3). The four rows on which x86-64 was losing this morning:

| row | i386 20/06 | x86-64 before | x86-64 after | after vs i386 |
|---|--:|--:|--:|--:|
| Hotpath SEND\|RCV null | 1.40 | 1.65 | **0.87** | **1.6× faster** |
| Intra-task 128B | 2.29 | 3.41 | **1.51** | **1.5× faster** |
| Slow-path null | 2.30 | 3.34 | **1.44** | **1.6× faster** |
| Inter-task null | 3.07 | 4.23 | **2.36** | **1.3× faster** |
| futex WAKE_WAIT ping-pong | 0.43 | 1.34 | **0.58** | 1.35× slower (was 3.1×) |
| port alloc + destroy | 1.24 | 0.36 | 0.36 | 3.4× faster |
| mach_null | 0.11 | 0.03 | 0.03 | 3.7× faster |

🔑 **Every row on which x86-64 was slower than i386 is now faster.** The only one
that remains behind is the futex ping-pong (#554), and the gap went from
3.1× to 1.35×.

## Where the cycles go — breakdown by phase (#392)

The instrument is `kern/syscall_profile.{h,c}`, `cmake -DUROS_SYSCALL_PROFILE=ON`,
off by default. `urmach_msg` trap on the mmot hot path, `-smp 1`, KVM,
median of 16 consecutive traps, frequency 3993 MHz sampled.

🔴 **This is the table to redo when PCID (#412) arrives**: `inter` changes
address space and `intra` does not, so PCID must move `SWITCH`
on the first and leave it still on the second. If it moves both or neither, the
premise of #412 has to be reread.

| phase | ablated | with guard | ablated µs | guard µs | what it closes |
|---|--:|--:|--:|--:|---|
| entry | 60 | 60 | 0.020 | 0.020 | SYSCALL, swapgs, frame, dispatch |
| get buf | 60 | 60 | 0.020 | 0.020 | `ikm_cache_get` |
| **COPYIN** | **180** | **180** | 0.060 | 0.060 | `copyinmsg` — the copy on send |
| resolve | 150 | 120 | 0.050 | 0.040 | name → port |
| queue | 120 | 150 | 0.040 | 0.050 | rights, queue limits, the two locks |
| **pick rcv** | **690** | **150** | 0.230 | 0.050 | finds and checks the receiver (it contained an `splx`) |
| claim | 60 | 60 | 0.020 | 0.020 | state change under `thread_lock` |
| park snd | 120 | 120 | 0.040 | 0.040 | the sender on the reply queue |
| deliver | 120 | 120 | 0.040 | 0.040 | delivers the message, prepares the switch |
| wait | 3150 | 2040 | 1.050 | 0.680 | off the processor — **the time of the other end** |
| **SWITCH** | **450** | **450** | 0.150 | 0.150 | `switch_context` + `thread_dispatch` |
| **splx** | **600** | **60** | 0.200 | 0.020 | lowers the interrupt level |
| resume | 60 | 90 | 0.020 | 0.030 | wake-up, `ith_state`, trailer |
| copyout | 210 | 210 | 0.070 | 0.070 | the translated header: ports → names |
| **PUT** | **150** | **150** | 0.050 | 0.050 | `copyoutmsg` — the copy on receive |
| residue | 60 | 30 | 0.020 | 0.010 | what the marks do not name |
| return | 51 | 66 | 0.017 | 0.022 | return → SYSRET (mean, from the assembly path) |
| **total on a processor** | **3141** | **2076** | 1.047 | 0.692 | = 1.047 µs against 0.692 |

Readings:

- **The ceiling of #391 (register-IPC) is the two copies: 330 cycles out of 2076,
  10-12%.** The hand-off machinery is 63-68%.
- **`claim` is 60 cycles**, that is, at the floor of the instrument: the state change
  of the scheduler — the one #319 talks about — costs nothing. What cost was the
  search for the receiver, and inside it there was an `splx`.
- **At `-smp 4` nothing grows**: `pick rcv` 690→660, `SWITCH` 450→420,
  `splx` 600→630. Same shape as #482 on the COW fault at eight processors.
- ⚠️ **The TSC grain in this guest is 30 cycles** and a pair of timestamps
  costs 60: the four columns at 60 are **below the resolution**, not
  measured at 60.
- ⚠️ **17 marks × 60 = 1020 cycles of instrument** out of the 3141 measured. Declared.
  Every phase carries one, so the correction is per column and uniform.

## How to redo this measurement

    sudo cpupower frequency-set -g performance -d 1.4GHz -u 4GHz
    sudo sh -c 'echo 1 > /sys/devices/system/cpu/cpufreq/boost'

    cmake -S uros -B uros/build-x86_64 -DUROS_BUNDLE_IPC_BENCH=ON \
          -DUROS_BUNDLE_BENCH_ONLY=ON
    ~/uros-tests/431-mediana.sh x64 3 1      # the campaign, 3 boots, median
    ~/uros-tests/mediane.sh <log>...         # median per (suite, metric)
    ~/uros-tests/392-giro.sh 1 kvm 150 tag   # one boot with the phases and the clock

🔴 **`tlp` is active on this machine**: unplugging the charger applies
the battery profile and puts the governor back to `powersave`. The scripts check
the condition before and after and **refuse to measure** if it does not match — a run
already thrown away today had every number inflated by 2.93×, which is exactly
4.10/1.40, because the TSC is invariant and the core is not.

⚠️ To measure a single suite: `-DUROS_BENCH_SUITES=comb`. The default stays the
full list.


## 2026-09-13 — the same at `-smp 4`, and here the median of 3 is NOT enough

**Same conditions**, same session, 3 boots per arm, `~/uros-tests/431-mediana.sh x64 3 4`.
Governor verified before and after both campaigns; frequency sampled (415 and 282 samples).

🔴 **Only one row is clean, and the others are to be read as unresolved.**

| row | ablated (min / **median** / max) | with guard (min / **median** / max) |
|---|---|---|
| **comb null** | 1.61 / **1.66** / 1.99 | 0.86 / **0.88** / 0.89 |
| inter null | 3.44 / **3.48** / 3.56 | **2.48** / 3.82 / 3.83 |
| intra null | 3.45 / **3.57** / 3.70 | **2.53** / 2.62 / 3.00 |
| slow null | 3.58 / **3.84** / 3.96 | **2.97** / 3.98 / 4.09 |

`comb` reproduces smp1 to the digit: **−47%**, narrow spread on **both** arms.

⚠️ The other three are **bimodal in the arm with the guard**: one boot much
faster and the others in the slow mode, while the ablated arm is narrow. It is the
inter-task placement lottery already recorded (**#356**, **#446**: "inter
bimodal"). With three boots the median falls wherever two boots out of
three are — so **those three rows say neither better nor worse**, and their
minima (3.44→2.48, 3.45→2.53, 3.58→2.97) are the only thing that moves
coherently. Separating them takes many more boots, not a more careful reading.

The checks stay still here too: `mach_null` 0.03→0.03, `mach_port_type`
0.29→0.29, `mach_port_names()` 30.84→30.88.

### All the measurements, `-smp 4`

| Section | Metric | before | after | Δ |
|---|---|--:|--:|--:|
| #324 futex vs Mach semaphore ping-pong (block+wake round-trip) | futex WAKE_WAIT ping-pong | 1.47 | 0.63 | -57% |
|  | semaphore ping-pong | 4.17 | 2.94 | -29% |
| Combined SEND\|RCV intra-task (hotpath) | 1024B inline RPC | 1.75 | 0.95 | -46% |
|  | 128B inline RPC | 1.71 | 0.92 | -46% |
|  | 4096B inline RPC | 1.80 | 1.00 | -44% |
|  | null RPC | 1.66 | 0.88 | -47% |
| FLIPC2 buffer group benchmarks | 256B RPC (bufgroup inter) | 0.91 | 0.83 | -9% |
|  | 256B RPC (bufgroup) | 2.70 | 2.90 | +7% |
|  | bufgroup alloc+free | 0.01 | 0.01 | +0% |
| FLIPC2 endpoint benchmarks | 128B RPC (endpoint) | 2.93 | 2.80 | -4% |
|  | endpoint create+destroy | 18.17 | 16.06 | -12% |
|  | null RPC (endpoint) | 2.89 | 2.65 | -8% |
| FLIPC2 game simulation (intra-task throughput) | audio 4KB PCM frame | 0.10 | 0.10 | +0% |
|  | per draw command | 0.00 | 0.00 | below resolution |
|  | texture 16KB chunk | 0.29 | 0.29 | +0% |
| FLIPC2 inter-task BATCH (vm_remap, amortized) | batch=1 (inter) | 2.77 | 2.26 | -18% |
|  | batch=16 (inter) | 0.20 | 0.17 | -15% |
|  | batch=64 (inter) | 0.07 | 0.07 | +0% |
| FLIPC2 inter-task RPC (urmach_futex hand-off) | 1024B RPC (inter futex) | 2.66 | 1.87 | -30% |
|  | 128B RPC (inter futex) | 2.47 | 1.88 | -24% |
|  | 4096B RPC (inter futex) | 2.76 | 1.98 | -28% |
|  | null RPC (inter futex) | 2.49 | 1.81 | -27% |
| FLIPC2 inter-task RPC (vm_remap shared memory) | 1024B RPC (inter) | 2.86 | 3.63 | +27% |
|  | 128B RPC (inter) | 2.94 | 3.41 | +16% |
|  | 4096B RPC (inter) | 4.12 | 4.00 | -3% |
|  | null RPC (inter) | 3.01 | 3.52 | +17% |
| FLIPC2 intra-task RPC (semaphore path) | 1024B RPC (intra) | 2.88 | 2.19 | -24% |
|  | 128B RPC (intra) | 3.00 | 2.13 | -29% |
|  | 4096B RPC (intra) | 3.04 | 2.55 | -16% |
|  | null RPC (intra) | 2.94 | 2.53 | -14% |
| FLIPC2 isolated channel RPC | 128B RPC (isolated inter) | 3.00 | 2.93 | -2% |
|  | 128B RPC (isolated intra) | 4.06 | 2.12 | -48% |
|  | null RPC (isolated inter) | 2.98 | 2.72 | -9% |
|  | null RPC (isolated intra) | 4.11 | 2.15 | -48% |
| FLIPC2 throughput (single-thread, no kernel) | null desc (batch=1) | 0.03 | 0.03 | +0% |
|  | null desc (batch=16) | 0.00 | 0.00 | below resolution |
|  | null desc (batch=64) | 0.00 | 0.00 | below resolution |
| FLIPC2 throughput with data | 1024B produce+consume | 0.04 | 0.05 | +25% |
|  | 128B produce+consume | 0.03 | 0.03 | +0% |
|  | 4096B produce+consume | 0.09 | 0.10 | +11% |
| Inter-task (task-to-task) | 1024B inline RPC | 3.74 | 4.43 | +18% |
|  | 128B inline RPC | 3.67 | 2.64 | -28% |
|  | 4096B inline RPC | 3.96 | 2.93 | -26% |
|  | null RPC | 3.48 | 3.82 | +10% |
| Intra-task (thread-to-thread) | 1024B inline RPC | 3.80 | 3.03 | -20% |
|  | 128B inline RPC | 3.73 | 4.04 | +8% |
|  | 4096B inline RPC | 4.23 | 4.63 | +9% |
|  | null RPC | 3.57 | 2.62 | -27% |
| Kernel RPC (where the MIG checks are) | mach_port_type (kernel RPC) | 0.29 | 0.29 | +0% |
| OOL data (inter-task, PHYSICAL_COPY) | 16 KB OOL inter | 5.37 | 3.13 | -42% |
|  | 4 KB OOL inter | 6.18 | 3.45 | -44% |
|  | 64 KB OOL inter | 10.62 | 13.73 | +29% |
| OOL data (intra-task, PHYSICAL_COPY) | 16 KB OOL | 4.64 | 4.09 | -12% |
|  | 4 KB OOL | 6.34 | 3.85 | -39% |
|  | 64 KB OOL | 6.67 | 3.97 | -40% |
| PP inter-task | 1024B (no PP) | 4.43 | 2.96 | -33% |
|  | 1024B (w/ PP) | 4.05 | 2.81 | -31% |
|  | 128B (no PP) | 3.57 | 3.86 | +8% |
|  | 128B (w/ PP) | 3.59 | 2.59 | -28% |
|  | 4096B (no PP) | 4.09 | 3.99 | -2% |
|  | 4096B (w/ PP) | 5.80 | 4.65 | -20% |
|  | null (no PP) | 3.57 | 2.67 | -25% |
|  | null (w/ PP) | 3.55 | 2.76 | -22% |
| PP intra-task | 1024B (no PP) | 3.74 | 3.18 | -15% |
|  | 1024B (w/ PP) | 3.81 | 4.27 | +12% |
|  | 128B (no PP) | 3.58 | 2.71 | -24% |
|  | 128B (w/ PP) | 3.75 | 3.86 | +3% |
|  | 4096B (no PP) | 3.87 | 3.95 | +2% |
|  | 4096B (w/ PP) | 3.98 | 3.59 | -10% |
|  | null (no PP) | 3.55 | 2.99 | -16% |
|  | null (w/ PP) | 3.54 | 3.94 | +11% |
| Port operations | mach_port_names() | 30.84 | 30.88 | +0% |
|  | port alloc + destroy | 0.38 | 0.36 | -5% |
| Raw syscall (no IPC) | mach_null (noop trap) | 0.03 | 0.03 | +0% |
|  | mach_print("") trap | 0.05 | 0.05 | +0% |
| Slow-path receive (continuation path) | 1024B inline RPC (receiver blocked) | 4.15 | 4.55 | +10% |
|  | 128B inline RPC (receiver blocked) | 3.67 | 2.82 | -23% |
|  | 4096B inline RPC (receiver blocked) | 4.38 | 3.47 | -21% |
|  | null RPC (receiver blocked) | 3.84 | 3.98 | +4% |

⚠️ In the arm with the guard **boot 1 died** on
`panic(cpu 1): vm_page_release: page 0x2788000 still mapped (#385)`, so the
rows after the FLIPC2 BATCH suite have fewer samples in that arm. The defect
is **pre-existing**: this morning's campaign, on the epic without the guard, produced it
in 1 boot out of 3 identically. Three boots per arm do not separate 1/3 from
0/3 on a rare defect — the twenty-boot campaign is needed, like the original one.

## What PCID (#412) must do to this table

`inter` changes address space, `intra` does not. PCID must move the
`SWITCH` column of the per-phase breakdown **on the first and not on the
second**. If it moves both, or neither, the premise of #412 has to be reread
before the code.

⚠️ And before measuring PCID, `-smp 4` has to be made readable: with the current
bimodality three boots cannot tell a 20% gain from noise.


## 2026-09-13 (3) — `-smp 4` made readable: the `-X` flag (#356/#446), and the slow path broken down (#559)

**Same conditions**, same session, 3 boots per arm, `~/uros-tests/446-ab.sh 3 4`.
Governor `performance` + `boost=1` verified before and after. On battery. Suites `intra slow inter comb`.
**The two arms are the SAME IMAGE, one flag of difference** — GRUB entry 14 against 18.

🔴 **And the presence check is inside the measurement**: the arm with `-X` must
print `sched: -X ... is ON`. Seen 0/0/0 in the arm without and 1/1/1 in the one
with. A flag that does not take hold and a flag that does nothing are indistinguishable in the
numbers — and this flag was **read by two call sites and written by none** on
x86-64 until today.

### The lottery collapses, boot by boot

| | boot 1 | boot 2 | boot 3 |
|---|--:|--:|--:|
| `intra` null **without** `-X` | **5.51** | 2.59 | 2.43 |
| `intra` null **with** `-X` | 1.52 | 1.43 | 1.43 |
| `slow` null **without** | **4.61** | 2.69 | 2.46 |
| `slow` null **with** | 1.35 | 1.37 | 1.35 |

From a spread of 130% between boots to a number stable at 3%. It is the result
that **#446 had measured on omen** (±30-43% → ±1%), reproduced on this
machine for the first time — because until today the flag was not
reachable here.

### 🔑 The experiment DISCRIMINATES, and that is what makes it a measurement

| Section | Metric | before | after | Δ |
|---|---|--:|--:|--:|
| Combined SEND\|RCV intra-task (hotpath) | 1024B inline RPC | 0.92 | 1.04 | +13% |
|  | 128B inline RPC | 0.98 | 0.90 | -8% |
|  | 4096B inline RPC | 0.98 | 1.04 | +6% |
|  | null RPC | 0.87 | 0.90 | +3% |
| Inter-task (task-to-task) | 1024B inline RPC | 2.80 | 3.96 | +41% ⚠️ |
|  | 128B inline RPC | 2.63 | 2.26 | -14% ⚠️ |
|  | 4096B inline RPC | 3.01 | 2.76 | -8% ⚠️ |
|  | null RPC | 2.55 | 2.36 | -7% ⚠️ |
| Intra-task (thread-to-thread) | 1024B inline RPC | 2.91 | 1.42 | -51% |
|  | 128B inline RPC | 4.33 | 1.41 | -67% |
|  | 4096B inline RPC | 3.16 | 1.46 | -54% |
|  | null RPC | 2.59 | 1.43 | -45% |
| Slow-path receive (continuation path) | 1024B inline RPC (receiver blocked) | 2.90 | 1.41 | -51% |
|  | 128B inline RPC (receiver blocked) | 2.68 | 1.41 | -47% |
|  | 4096B inline RPC (receiver blocked) | 3.23 | 1.46 | -55% |
|  | null RPC (receiver blocked) | 2.69 | 1.35 | -50% |

⚠️ **The four `inter` rows marked are a single sample each** and are not
to be read as a Δ — see the retraction below. The other twelve rows are
reproducible (the spread between boots collapses, and that is the point).

`intra` and `slow` collapse by 45-67%; **`inter` and `comb` do not**. It is exactly
what the gate predicts: the hand-off of #356 is **same-task only**, so
`inter` (different tasks) does not take it, and `comb` does not go through the run queue because
it uses the direct hand-off inside `mach_msg`. Had they all dropped, something
else would have been measured.

### ⚠️ The four `inter` rows above are NOT to be read — retracted (13/09, evening)

This section used to say: *"three boots out of three high are a MODE, not a
lottery — at 1024B the arm with `-X` is reproducibly slower"*, and
concluded that `-X` costs +41% on a cross-task row. **That is wrong, and it is
wrong for a reason that holds well beyond this row.**

🔴 **The sampling unit was wrong.** Each `inter` row builds and
destroys its own child task, and the placement of the pair is decided there.
So **a row is ONE draw**, however many iterations you average inside it: the 10000
iterations take their median *inside* one draw, not *across* draws. Three boots are three
samples, not thirty thousand — and the +41% was a row that had drawn slow three
times compared with one that had drawn fast two times out of three.

#### The measurement that says so, with the control INSIDE the boot

Suite `isweep` (#446): twelve sizes, three repetitions each, **in the same
boot** — so clock, governor, bundle and scheduler history are identical
across the repetitions by construction rather than by assertion. Two boots per
arm ⇒ **72 measurements per arm**. pavillion, KVM, `-smp 4`, governor
`performance`, boost on, on AC, ~3.8 GHz sampled before and after, bundle
`name_server` + `ipc_bench` with `inter isweep`, profile OFF, GRUB entries 14 and 18
(same image, one flag of difference; `sched: -X ... is ON` verified
present in the arm with `-X` only).

The distribution **is not bimodal**: it is a **narrow peak** at ~2.5 µs plus a
**right tail**.

| over 72 measurements | without `-X` | with `-X` |
|---|--:|--:|
| draws in the peak (< 3.0 µs) | 42 (58%) | 42 (58%) |
| median of the peak | 2.54 | 2.52 |
| draws in the tail (≥ 3.0 µs) | 30 (42%) | 30 (42%) |
| median of the tail | 3.53 | **3.82** |
| **worst draw** | **13.10** | **4.26** |

🔑 **`-X` changes neither how often one ends up in the tail, nor the cost of the peak.**
What it does is **shorten the tail**: 5 of the 30 slow draws without `-X`
exceed the *worst* draw seen with `-X`. It moves the center of the tail up a
little and removes its extremity. ⚠️ On 30 slow draws per arm it is an
**indicative effect, not an established one**.

🔑 **And no size stands out.** Inside one boot the same size reads 2.53,
5.54 and 2.71; and in a whole boot (`senza-2`) **all** twelve sizes read
between 2.25 and 2.71 with a spread of 1-8%. Three equal repetitions happen with
probability 0.25 ⇒ over twelve sizes one expects **~3 "narrow" rows by
chance**, and 4 are observed: it is not structure, it is the expected count.

#### ❌ And a correction inside the correction: `HW_FOOTPRINT` IS compiled

The first draft of this section said that `HW_FOOTPRINT` is not compiled on
x86-64 and that therefore `last_processor` is not used for placement. **False.** The
generated `config-include/hw_footprint.h` sets it to **1 on BOTH** targets, and
the affinity block is in the preprocessed `sched_prim.c`.

🔴 **I had "proved" it with a broken instrument**: I had replaced `-c` with
`-E` in the command line leaving the `-o file.o` in it, so the preprocessed output
ended up in the object file and `grep` read an empty stdin. Zero occurrences, and I
read that as an absence. **An absence must be confirmed by a presence**: redone with
`thread_setrun` as the control (12 occurrences), `last_processor` has 5.

🔑 **And the real mechanism explains more than the one I had invented.**
The affinity is conditional:

```c
processor = th->last_processor;
if (processor->state == PROCESSOR_IDLE) { ...dispatch there...; return; }
/* otherwise: the FIRST idle CPU in the queue */
```

It prefers the last CPU **only if that CPU is IDLE**. But in a synchronous RPC,
at the instant the waker wakes the peer, **the waker is still running on
that CPU**: the check fails in exactly the case where co-locating
would help, and the pair is sent to the first idle CPU. ⇒ **the affinity cannot
co-locate a synchronous pair, and that is structural, not a defect.**

And that is exactly what the hand-off of #356 works around: it queues the woken one
on the runq of the **current** CPU, because the waker *knows* it is about to block —
information that `thread_setrun` alone does not have. Being same-task, `inter`
**never** takes it ⇒ the cross-task pair always splits.

#### ✅ And the prediction was verified: at `-smp 1` the tail is NOT there

If the tail is the pair split across two CPUs, with **a single CPU there is nowhere
to split it** and it must disappear. Same image, same GRUB entries, same bundle,
same `isweep`, governor `performance` + boost verified on AC before and after
(3612 → 3090 MHz, thermal drift), **two boots per arm = 72 draws per cell**:

| | in the peak | median | p90 | **worst** |
|---|--:|--:|--:|--:|
| **`-smp 1` without `-X`** | 94% | 2.51 | **2.68** | **3.35** |
| **`-smp 1` with `-X`** | **100%** | **2.34** | **2.43** | **2.62** |
| `-smp 4` without `-X` | 58% | 2.66 | 4.19 | **13.10** |
| `-smp 4` with `-X` | 58% | 2.61 | 3.92 | 4.26 |

🔑 **The peak stays where it is and only the tail moves**, and that is what makes the
measurement readable: if `-smp 1` were simply "another machine" the peak would
have moved too. **Only** what the hypothesis predicted has moved.

🔑 **And the ALTERNATION disappears too.** The draws at smp4 are not independent:
the autocorrelation of the fast/slow sequence in `conX-1` gives **lag 1 = 17%**
(two consecutive draws are almost always *different*) and lag 2 = 76% — period 2.
At `-smp 1` lag 1 climbs back to **94%**. The suspect is `queue_first(&pset->idle_queue)`
in the branch that the affinity does not take: **the idle-CPU queue rotates**, so consecutive
child tasks draw different CPUs in turn.

⚠️ This **also corrects an argument of mine**: I had written that three equal
repetitions happen with probability 0.25 *assuming independence*, which is violated.
The conclusion ("no size stands out") comes out **strengthened** — with
serial correlation equal repetitions are even **more** probable — but the
number 0.25 is a lower bound, not the probability.

⚠️ **Not explained, and with only two boots per arm**: at `-smp 1` the arm with
`-X` is also the fastest of all (median 2.34, the peak drops by one
bucket). `inter` is cross-task and the gate is same-task, so the flag
should not touch it; and the clock drift plays **against** this result,
because the arm with `-X` ran second, at 3090 MHz. It has to be retaken with
more boots before saying anything about it.

🔴 **The operating rule that comes out of it**: on `inter` a comparison needs
many draws, and the statistic to report is **the fraction in the peak and the
shape of the tail**, never the mean of a row — which is peak and tail mixed together.

⚠️ The cross-task lottery **remains**, and it is the one that PCID (#412) can
remove by opening the gate. The reason why it is closed is written in
`sched_prim.c` and it is an i386 cost: *"an inter-task switch on one CPU costs a
cr3 reload = full TLB flush on i386"*.

### Where the cycles of a SLOW path trap go (#559)

`-smp 1`, KVM, 3993 MHz sampled, median over a single shape per window.
🔴 **A slow-path round trip is TWO traps and they are two different shapes**: the
send does not block, the receive does. A median over the two together is a row that
never happened.

| phase | hot path | slow send | slow recv | hot µs | send µs | recv µs |
|---|--:|--:|--:|--:|--:|--:|
| entry | 60 | 60 | 60 | 0.020 | 0.020 | 0.020 |
| `get buf` + `COPYIN` | 210 | — | — | 0.070 | — | — |
| resolve | 150 | 210 | 210 | 0.050 | 0.070 | 0.070 |
| queueing / waking up | 570 | 120 | 180 | 0.190 | 0.040 | 0.060 |
| `wait` (the peer) *(wall)* | 2100 | — | 2520 | 0.700 | — | 0.840 |
| **`RUNQ`** | **0** | — | **390** | 0.000 | — | 0.130 |
| `SWITCH` | 450 | — | 540 | 0.150 | — | 0.180 |
| `splx` + `resume` | 150 | — | — | 0.050 | — | — |
| `copyout` + `PUT` | 540 | — | 450 | 0.180 | — | 0.150 |
| residue | 30 | 60 | 390 | 0.010 | 0.020 | 0.130 |
| **on a processor** | **2211** | **651** | **2271** | 0.737 | 0.217 | 0.757 |

🔑 **`RUNQ` + `SWITCH` = 930 out of 2271, 40%** of the work of a slow receive is
*being woken up and getting a processor*. And **`RUNQ` is exactly zero on the
hot path**, spread `[0..0]` over sixteen samples: zero where the mechanism says it
must be, because a direct hand-off never touches a run queue.

⇒ The two-trap round trip costs **651 + 2271 = 2922** cycles on a processor against
the **2211** of the hot path, **+32%**, and the single biggest item that separates them
is a phase that the hot path does not have at all.

### Which route the traffic really takes (#559, coverage)

A trap that arrives in `ipc/mach_msg.c` takes one of three routes, and the
breakdown means something different on each. One boot, the four suites:

| route | traps |
|---|--:|
| combined (hot path) | 65825, of which **65635 handed off** |
| **SEND only** | **242413** |
| **RCV only** | **242431** |

🔑 **The two-trap route is about 88% of the `mach_msg` traffic.** What #392
profiled is the minority — and that is the reason #559 exists.

❌ And the same counters corrected an error of mine: **0 of 242431** receives
resumed in `mach_msg_receive_continue()`. `ipc_mqueue_receive()` passes the
continuation to `thread_block()`, which invokes it only if the scheduler discards the
kernel stack — that never happens here. The marks stay there (they are right for the
case that takes it) but the honest description is "an arm that exists and is not
taken here".


### And the slow path at four processors costs 3× that at one — the hot path does not

Both readings with `-X`, so **only the number of processors changes**.
`-smp 1` and `-smp 4`, KVM, 3993 MHz sampled, median over the 8 blocked samples.

| phase | smp1 `-X` | smp4 `-X` | smp1 µs | smp4 µs | ratio |
|---|--:|--:|--:|--:|--:|
| entry | 60 | 60 | 0.020 | 0.020 | **1.0** |
| resolve | 210 | 360 | 0.070 | 0.120 | 1.7 |
| `wait` *(wall)* | 2520 | 5045 | 0.840 | 1.682 | 2.0 |
| **`RUNQ`** | **420** | **1405** | 0.140 | 0.468 | **3.3** |
| `SWITCH` | 540 | 1230 | 0.180 | 0.410 | 2.3 |
| `mq_recv` | 210 | 840 | 0.070 | 0.280 | 4.0 |
| `copyout` | 300 | 990 | 0.100 | 0.330 | 3.3 |
| **`PUT`** | **150** | **840** | 0.050 | 0.280 | **5.6** |
| residue | 360 | 1020 | 0.120 | 0.340 | 2.8 |
| **on a processor** | **2301** | **6797** | 0.767 | 2.266 | **3.0** |
| of which "getting the processor" | 960 (41%) | 2635 (38%) | 0.320 | 0.879 | |

🔑 **Three times, and #392 had found that the HOT path costs the same at 1 and at 4.**
Two opposite behaviors on the same kernel, now measured instead of assumed.
The *share* of "getting the processor", on the other hand, does not move: 41% against 38%.

⚠️ `entry` stays still at 60 on both, and it is the check that makes the
rest readable: the entry into the trap is per-CPU work and must not depend on how many
processors there are. Had it moved, the table would be measuring the clock or
the accelerator.

⚠️ `PUT` at 5.6× and `copyout` at 3.3× are the two copies towards user memory.
That it is TLB/cache with the threads moving between processors is **plausible and
not measured**, and it stays written like that instead of as an explanation.

## How to reproduce it

    sudo cpupower frequency-set -g performance -d 1.4GHz -u 4GHz
    sudo sh -c 'echo 1 > /sys/devices/system/cpu/cpufreq/boost'

    cmake -S uros -B uros/build-x86_64 -DUROS_BUNDLE_IPC_BENCH=ON \
          -DUROS_BUNDLE_BENCH_ONLY=ON -DUROS_BENCH_SUITES="intra slow inter comb"
    ~/uros-tests/446-ab.sh 3 4        # A/B of the -X flag, 3 boots per arm
    ~/uros-tests/392-giro.sh 1 kvm 240 tag 18   # one boot with the phases, entry 18

🔴 **The GRUB entry is not stable**: 14 = ordinary bundle, 18 = the same with
`-X`. It is asked of `uros/src/mach_kernel/x86_64/boot/grub.cfg`, where new entries
go **at the end** precisely because inserting one in the middle silently renumbers
every invocation that names one.

⚠️ The per-phase profile is `-DUROS_SYSCALL_PROFILE=ON`, **off by default**, and
is to be read only for the PHASES: with the dumps on, a `comb` null RPC reads 51 µs
against 2.21, because the printfs print inside the loop.


## 2026-10-23 — the #392 table on today's epic (#737)

The phase table above (13/09) predates #561, which moved the FPU's state into
the context switch, and #412, PCID.  It was taken again on the epic of 23/10
(d5e31edb), on victus (AMD Ryzen 5 5600H, Zen 3), under KVM, on AC, with the
performance governor, boost on and the maximum at the hardware's; the clock
each boot reported was 3.89-4.0 GHz.

### The hand-off is `comb`'s

The #392 table measured the hand-off: a send that gives the processor to its
receiver in the same trap.  Today's bench takes it in `comb` (Combined
SEND|RCV intra-task, the hot path).  `intra` and `inter` block, or never leave
the processor: their dumps say `[blocked, no hand-off]` and `[never left the
processor]`, with marks the hand-off has not -- `RUNQ` apart from `SWITCH`,
`mq_recv`, `mq_send`, `kmsg_get`.

⚠️ **One suite a boot.**  The profile keeps 16 dumps a thread (`SP_MAX_DUMPS`,
`kern/syscall_profile.h`), and the ping-pong's two threads spend them in the
first suites they run: with `intra inter comb` in one boot, `comb` had no dump
at all.  Each suite is built alone, `-DUROS_BENCH_SUITES=comb`.

### `comb`, the 13/09 tree against today's, on one machine

67cf1be4 (the 13/09 tree, before #561) and d5e31edb, `comb` alone, boots
alternated, 3 at `-smp 1` and 5 at `-smp 4` each; the median of each column
over 102 and 170 dumps, cycles as printed (each column holds one pair of
marks, see below):

| phase | 13/09 tree, smp 1 | today, smp 1 | 13/09 tree, smp 4 | today, smp 4 |
|---|--:|--:|--:|--:|
| entry | 66 | 66 | 66 | 66 |
| get buf | 66 | 66 | 66 | 66 |
| **COPYIN** | **165** | **165** | **165** | **165** |
| resolve | 132 | 132 | 132 | 132 |
| queue | 132 | 132 | 132 | 132 |
| pick rcv | 132 | 132 | 132 | 132 |
| claim | 99 | 99 | 66 | 99 |
| park snd | 99 | 99 | 99 | 99 |
| deliver | 66 | 66 | 66 | 66 |
| wait (of wall) | 2112 | 2145 | 2112 | 2145 |
| **SWITCH** | **528** | **528** | **528** | **528** |
| splx | 66 | 66 | 66 | 66 |
| resume | 66 | 66 | 66 | 66 |
| copyout | 198 | 198 | 198 | 198 |
| **PUT** | **132** | **132** | **132** | **132** |
| residue | 66 | 66 | 66 | 66 |
| **on a processor** | **2069** | **2068** | **2068** | **2035** |

- **Nothing on the hand-off's path moved since 13/09 by more than a grain.**
  The differences from the 13/09 table above -- `SWITCH` 450 there, 528 here
  -- are the machine's: that table was taken on another machine, whose
  timestamps moved in grains of 30 cycles; victus's move in grains of 33, and
  on victus the 13/09 tree measures 528 too.
- **#561 is not visible on this path, before it or after it.**  Whether the
  bench's threads touch the FPU at all, which would explain it, has not been
  checked.
- **At `-smp 4` it does not grow**, as on 13/09.

### The instrument

On victus every phase reads as a multiple of 33 cycles, and each column holds
one pair of marks, which the profile measures at each boot: one grain or two
-- 66 in the 13/09 tree's boots, 33 or 66 in today's.  Taking it out moves a
column by a grain either way, so the columns are compared as printed, each
good to a grain.  The fifteen columns of work hold fifteen pairs, 500 to 1000
of the 2068 cycles on a processor.

### Where the hand-off's cycles are now

Of the 2068 cycles a hand-off spends on a processor (`-smp 1`, today; each
row is a sum of column medians):

| | cycles | share |
|---|--:|--:|
| `SWITCH` (`switch_context` + `thread_dispatch`) | 528 | 26% |
| finding and queueing (resolve, queue, pick rcv) | 396 | 19% |
| **the two copies (COPYIN + PUT)** | **297** | **14%** |
| the header translated out (copyout) | 198 | 10% |
| the rest (entry, get buf, claim, park snd, deliver, splx, resume, residue) | 594 | 29% |

**#391's ceiling, restated**: the two copies are 297 of the 2068 cycles, 14%
(13/09: 330 of 2076, on the other machine).  ⚠️ The 13/09 reading above says
"10-12%" for those same 330 cycles, but 330 of 2076 is 16%, and the text does
not say what other total it was taken of.  What register-IPC could save on
this path is still at most the copies: a seventh of it, not a tenth.

### `intra` and `inter` today

Each suite alone, 3 boots at each count, medians as printed:

| | smp 1 | smp 4 |
|---|---|---|
| `intra`, blocked: on a processor / RUNQ / SWITCH | 2928 / 1122 / 528 | 3788 / 726 / 891 |
| `inter`, blocked: on a processor / RUNQ / SWITCH | 3454 / 1155 / 924 | 3983 / 907 / 957 |
| never left the processor, `intra` / `inter` | 850 / 848 | 1578 / 1641 |

At `-smp 4` these medians spread widely and move between runs more than at
`-smp 1`.

### On omen's metal (24/10)

omen is an Intel Kaby Lake laptop, four cores and eight threads, its TSC at
2808 MHz.  One suite a boot from one stick (`make-baremetal-x86_64.sh
--suites`), the console kept across the reset by ramoops (#373): 22 boots.
The cycles are TSC ticks as on victus, but of another clock on another
processor, so omen's numbers are compared with omen's.

**The clock under measurement is HWP's, not the one the boot reports.**  #736
turns HWP on with the request's minimum at 1, its maximum at 38 and EPP 0x80,
balanced, so each core's clock follows its own load.  The 10 ms spin that
reports 3407-3409 MHz on all eight processors at bring-up says nothing about a
suite whose threads block and wake.  The profile's pair of marks shows it: it
is measured once, at the first dump (`sp_measure_self()`, nine pairs of
back-to-back TSC reads), and reads 26-28 ticks on a core at full clock, 47-63
on a slow one.  With EPP 0x80 it read 47-63 in 5 boots of 11, and in those
boots `intra`'s traps that never left the processor cost about 2000 cycles
against about 1780.  With `hwp_epp=0` it read 26-28 in 10 boots of 10, and the
PCID comparison below is those ten boots.

Two more things the reading of the logs needed:

- **A suite is four tests in a row** -- null, 128 B, 1024 B and 4096 B RPCs --
  and a thread's dumps come at 16·2^k traps, so which tests they fall in
  depends on how many threads share the work, and changes from boot to boot.
  In one boot `comb`'s 128 B test had 11 dumps and in the next none, and its
  `copyout` read 289 against 186 with every other column equal.  Each dump is
  counted in the test it was printed in (the bench's `us/op` line ends each
  test), and only like is compared with like.
- **#757 splices the phase lines** of two dumps printed at once: of `intra`'s
  52 dumps a boot, 9 read phase by phase.  A dump's header -- the round trip's
  wall time and its time on a processor -- is one printf, and it arrived whole
  in every dump of every boot, so the totals below are the headers'.

`comb`, the hand-off, null RPCs, 40 dumps over two boots (PCID on, EPP 0x80,
the marks at 28 and 26; the two boots agree to 0.3%), cycles as printed:

| phase | cycles |
|---|--:|
| entry / get buf / **COPYIN** | 64 / 61 / **125** |
| resolve / queue / pick rcv | 126 / 120 / 238 |
| claim / park snd / deliver | 121 / 123 / 90 |
| wait (of wall) | 2287 |
| **SWITCH** | **377** |
| splx / resume / copyout | 53 / 131 / 188 |
| **PUT** / residue | **117** / 36 |
| **on a processor** | **2008** |

Of those 2008 cycles, `SWITCH` is 19%, finding and queueing 24%, **the two
copies 242 cycles, 12%**, the header translated out 9%, and the rest 34%.  On
metal #391's ceiling is an eighth of the hand-off.

PCID on and off (`-n`), `hwp_epp=0`, the headers' medians, every test that has
dumps:

| PCID off against on | on a processor | wall |
|---|---|---|
| `inter`, blocked (2 boots each) | +2.4% to +4.9% | +4.3% to +11% |
| `inter`, never left the processor | +2.7% to +5.8% | the same |
| `intra`, blocked (4 boots on, 2 off) | +2.3% to +7.5% | +4.8% to +7.1% |
| `intra`, never left the processor | +1% to +3% | the same |

For `inter`, every boot with PCID off is above every boot with it on, in every
test and both kinds of trap.  For `intra`'s blocked round trip the wall time
is, in every test.  `intra`'s never-left traps stay within the boots' spread,
except in the 4096 B test, which has two dumps a boot.  For scale, a null RPC
with PCID on: `inter` blocked 3894 and 3998 cycles on a processor, never left
1797 and 1837; `intra` blocked 4067 to 4142, never left 1762 to 1801.

Phase by phase, `inter` blocked, PCID on and then off (the 128 B, 1024 B and
4096 B tests; #757 left the null test's phases unreadable): `SWITCH` moves by
+19, -21 and +32 cycles, which is nothing; `mq_recv` by +93, +135 and +137,
and the wait for the other side by +410 to +495.

⚠️ One boot of the 22 is not explained: `intra` with EPP 0x80 and the marks at
28 read 3350 and 1457 cycles, where every other `intra` boot read 4000-4450
and 1760-2030.  It is an EPP 0x80 boot, so it is outside the comparison above.

### What the table says to #412

`comb` hands the processor from one thread of a task to another, so PCID has
nothing to do there.  `inter` changes address space at every switch.  `intra`
was read here as staying in one, and its traps that never leave the processor
do -- but its blocked round trip does not: a thread that blocks with nothing
else to run gives its processor to the idle thread, whose activation's map is
`kernel_map`, and `switch_context()` (`x86_64/thread/machine.c:980`) writes
CR3 for it, and again on the way back.

The KVM guest on victus has no PCID, and there `inter`'s `SWITCH` costs 396
cycles more than `intra`'s; on omen under KVM, with PCID, 234 more -- another
machine, so not the answer.  The answer is one machine with PCID on and off,
omen's metal (above): PCID takes 2.4-5.8% off the kernel's time of an `inter`
round trip and up to 11% off a blocked one's wall time, and up to 7.5% off
`intra`'s blocked round trip, which passes through the idle thread; `intra`'s
traps that never leave the processor do not move.  `SWITCH` does not move
either way.  Without PCID the flush is paid after the switch -- in the
receive, `mq_recv`, and in the wait for the other side.

### How to redo it

    sudo cpupower frequency-set -g performance
    cmake -S uros -B uros/build-x86_64 -DUROS_SYSCALL_PROFILE=ON \
          -DUROS_BUNDLE_IPC_BENCH=ON -DUROS_BUNDLE_BENCH_ONLY=ON \
          -DUROS_BENCH_SUITES=comb
    ninja -C uros/build-x86_64
    scripts/run-x86_64.sh --entry 14 120 -smp 1 -enable-kvm

and on a machine's metal, one stick for every suite and every flag
(`ADDR` is the machine's ramoops zone):

    scripts/make-baremetal-x86_64.sh --bench-only -o uros.iso \
        --suites comb --entry "-b ramoops=ADDR reset_after=60 hwp_epp=0" \
        --suites inter --entry "-b ramoops=ADDR reset_after=60 hwp_epp=0" \
        --suites inter --entry "-n -b ramoops=ADDR reset_after=60 hwp_epp=0"

⚠️ `hwp_epp=0` asks HWP for performance (#736).  Without it the request is
balanced, and the clock under measurement is whatever HWP chose: the pair of
marks printed in every dump says which, 26-28 cycles on omen at full clock.
