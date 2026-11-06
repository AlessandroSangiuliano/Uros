# Micro-optimizations: what each one saved

One row per micro-optimization of a hot path: what it took out, where it is, and what it saved, as measured. A change that saves nothing measurable is listed too, with its number: a line of code has to pay for the reasoning it needs.

## How a saving is measured

- **Machine.** Bare metal, omen: i7-7700HQ, TSC at 2808 MHz, cores at 3408 MHz with `hwp_epp=0`, on AC.
- **Kernels.** Unprofiled, one per commit. All arms boot the same bundle: ipc_bench's `intra inter comb isweep icc`, with `-X`.
- **Boots.** Arms alternated (A B C D, then D C B A), four boots each.
- **The number.** Each row is the median of T/N, from ipc_bench's "(N iters, T us total)". That resolves 0.1 ns; the `us/op` column only resolves 10.
- **Units.** Cycles are TSC cycles at 2808 MHz, so 1 ns is 2.8 cycles. An inter round trip has two receives that block, so a saving per blocked receive counts twice.
- **Noise.** About ±5 ns per row with four boots. The sweep rows of inter, three repeats per boot, give twelve samples per arm and are the steadiest.

## The ledger

| Change | Commit | Issue | Takes out | Saved per round trip | Status |
| --- | --- | --- | --- | --- | --- |
| A thread that blocks with a continuation is switched as i386's `Switch_context` does it: nothing saved for it, and its restart reached by a jump | `16fe677b` | #777 | The registers saved for a thread whose frames are abandoned, the frame written for it to resume through, and a `ret` into that frame that the processor had seen no call for | **inter: 30-47 ns (85-131 cycles) at every size of the sweep, median 36 ns (100 cycles); intra: 17-42 ns; comb: none (it does not block)** | On the branch |
| A thread resumed in its continuation starts with `sti`, and `clac` where SMAP is on, instead of `popfq` | `3528f7b6` | #777 | `popfq` at the fresh start | None measurable: +2 to +12 ns on the sweep, median +5 | Taken out: `popfq` again |
| A thread resumed from frames it saved gets its interrupt flag back by `sti` or `cli`, and `clac`, instead of `popfq` | `e8046d11` | #777 | `popfq` on every resume of saved frames, which includes the mach_msg hot path's direct switch | None measurable: comb -15 to +8 ns on four samples, sweep +3 to +11 ns | Taken out: `popfq` again |

Measured 03/11/2026, against `9a803d02` (arm A), on `feature/777-hand-off-where-the-sender-blocks`. The first row was measured again the same day in a second session, against both A and the epic at `ccac34fd`: 30-38 ns a round trip across the sweep, and the branch as a whole about 27 ns faster than the epic.

These rows were read from `inter` and its sweep, and until #777 those rows received into 24 bytes: a null reply and its trailer are 32, so every receive ended in `MACH_RCV_TOO_LARGE` and the rows timed that error path, on both arms and since at least July. The differences above are in the switch, which both arms went through, and they stand. The totals do not, and `intra` and `inter` are to be measured again before they are compared with anything measured since.

### What the first row recovered

The continuations of #771 cost inter 50 ns at f6bd4a24. The switch was 31 of them: 40-47 TSC cycles more for every receive that blocks. The rest is the way out through the frame of a call that slept, which this ledger leaves as it is. The first row recovers more than the switch's share, because it also drops the registers that were saved for a thread blocking with a continuation, which every such switch paid before #771 as well.
