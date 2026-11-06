# Micro-optimizations: what each one saved

One row per micro-optimization of a hot path: what it took out, where it is, and what it saved, as measured. A change that saves nothing measurable is listed too, with its number: a line of code has to pay for the reasoning it needs.

## How a saving is measured

- **Machine.** Bare metal, omen: i7-7700HQ, TSC at 2808 MHz, cores at 3408 MHz with `hwp_epp=0`, on AC.
- **Kernels.** Unprofiled, one per commit. All arms boot the same bundle: ipc_bench's `intra inter comb isweep icc`, with `-X`.
- **Boots.** Arms alternated (A B C D, then D C B A), four boots each.
- **The number.** Each row is the median of T/N, from ipc_bench's "(N iters, T us total)". That resolves 0.1 ns; the `us/op` column only resolves 10.
- **Units.** Cycles are TSC cycles at 2808 MHz, so 1 ns is 2.8 cycles. An inter round trip has two receives that block, so a saving per blocked receive counts twice.
- **Noise.** About ±5 ns per row with four boots. The sweep rows of inter, three repeats per boot, give twelve samples per arm and are the steadiest.
- **Layout.** Two builds also differ in where their code landed. In November arms that did not touch the hot path moved `comb` by 9 ns and `inter` by 11 ns, every boot the same way. Where a row that small is the question, one binary with a switch read at boot takes layout out of it.

## The ledger

| Change | Commit | Issue | Takes out | Saved per round trip | Status |
| --- | --- | --- | --- | --- | --- |
| A thread that blocks with a continuation is switched as i386's `Switch_context` does it: nothing saved for it, and its restart reached by a jump | `16fe677b` | #777 | The registers saved for a thread whose frames are abandoned, the frame written for it to resume through, and a `ret` into that frame that the processor had seen no call for | **inter: 30-47 ns (85-131 cycles) at every size of the sweep, median 36 ns (100 cycles); intra: 17-42 ns; comb: none (it does not block)** | On the branch |
| A thread resumed in its continuation starts with `sti`, and `clac` where SMAP is on, instead of `popfq` | `3528f7b6` | #777 | `popfq` at the fresh start | None measurable: +2 to +12 ns on the sweep, median +5 | Taken out: `popfq` again |
| A thread resumed from frames it saved gets its interrupt flag back by `sti` or `cli`, and `clac`, instead of `popfq` | `e8046d11` | #777 | `popfq` on every resume of saved frames, which includes the mach_msg hot path's direct switch | None measurable: comb -15 to +8 ns on four samples, sweep +3 to +11 ns | Taken out: `popfq` again |
| The per-thread port cache of mach_msg's hot path is gone: what it skipped, the space's lock, the lock-free resolve has skipped since #331 | `8f20cd51` | #868 | Four entries a thread, their lookup and two fills | Nothing measurable, by taking it out: +0.7 ns on `comb`'s null row, at most +8 ns on any row, none significant | Removed |

Measured 03/11/2026, against `9a803d02` (arm A), on `feature/777-hand-off-where-the-sender-blocks`. The first row was measured again the same day in a second session, against both A and the epic at `ccac34fd`: 30-38 ns a round trip across the sweep, and the branch as a whole about 27 ns faster than the epic.

These rows were read from `inter` and its sweep, and until #777 those rows received into 24 bytes: a null reply and its trailer are 32, so every receive ended in `MACH_RCV_TOO_LARGE` and the rows timed that error path, on both arms and since at least July. The differences above are in the switch, which both arms went through, and they stand. The totals do not, and `intra` and `inter` are to be measured again before they are compared with anything measured since.

### What the first row recovered

The continuations of #771 cost inter 50 ns at f6bd4a24. The switch was 31 of them: 40-47 TSC cycles more for every receive that blocks. The rest is the way out through the frame of a call that slept, which this ledger leaves as it is. The first row recovers more than the switch's share, because it also drops the registers that were saved for a thread blocking with a continuation, which every such switch paid before #771 as well.

### The port cache

#868 made a hit of the per-thread port cache ask, with both ports locked, whether its mapping still held. That cost 2.4 ns on `comb`, four boots an arm, below the noise, and it left the cache reading an entry on every hit, so the question was what the cache still saved.

Two builds could not answer that. Without the cache `comb` read 11.8 ns higher, every boot the same way, but arms that do not touch the hot path had moved it by 8.6 ns. So the measure was one kernel, with a patch outside the tree that turned the lookup and the fills off from a boot value, six boots an arm, 6 November 2026. Under QEMU, with counters, a boot of these rows made 458735 hits with the cache on and none with it off.

| ns per RPC, mean of the boot | With | Without | Difference | p |
| --- | --- | --- | --- | --- |
| `comb`, null | 1154.1 | 1154.8 | +0.7 | 0.65 |
| `comb`, 128 B | 1177.1 | 1179.5 | +2.4 | 0.88 |
| `comb`, 1024 B | 1193.6 | 1199.7 | +6.1 | 0.17 |
| `comb`, 4096 B | 1293.8 | 1300.9 | +7.1 | 0.12 |
| `ccomb`, one space, one client | 1167.5 | 1174.1 | +6.6 | 0.15 |
| `ccomb`, one space, two clients | 815.3 | 816.9 | +1.6 | 0.60 |
| `ccomb`, one space, four clients | 712.4 | 707.7 | -4.7 | 0.61 |
| `ccomb`, across tasks, one client | 1373.9 | 1381.9 | +8.0 | 0.17 |
| `ccomb`, across tasks, two clients | 873.8 | 874.6 | +0.7 | 0.66 |
| `ccomb`, across tasks, four clients | 539.0 | 536.8 | -2.2 | 0.66 |

`p` is an exact permutation test over the boots. Only these rows can say: a request reaches the cache only when it sends and receives in one call on its reply port, as MIG's stubs make it. `intra`, `inter`, the sweep, `icc`, `cc` and `scale` make two calls, which is why `ccomb` was written. The cache was added in March to keep RPCs off the space's lock, and the lock-free resolve of #331 has kept them off it since June, so several clients in one space show nothing.
