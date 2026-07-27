# Memory-ordering litmus tests (herd7 / x86-TSO)

Formal validation of the kernel's hand-placed memory barriers, checked against the
formal **x86-TSO** model with [`herd7`](https://github.com/herd/herdtools7).

The bare-metal fault-stress (omen, 2–8 CPU, ~4.2M concurrent faults, all `ok`) is
reassuring but **statistical, not a proof**: a missing store→load fence is a rare
reorder that can survive millions of runs and still be wrong. A litmus test settles
it — it asks the formal model whether the bad outcome is reachable *at all*.

First subject: **#350** — the **#338** `pmap_enter` ↔ `set_dirbase` TLB-shootdown
barrier.

## The interaction (#338, a Dekker mutual exclusion)

Split page-table locks (#338) removed the giant per-pmap lock that used to
implicitly order `pmap_enter`'s PTE store before its `cpus_using` load. In its
place `PMAP_TLB_WRITE_BARRIER()` was added. Two CPUs race:

```
  P0 = pmap_enter        (thread A)      P1 = pmap_activate / set_dirbase (thread B)
  ------------------------------         -----------------------------------------
  store PTE = new                        set cpus_using bit = 1   (i_bit_set)
  PMAP_TLB_WRITE_BARRIER  <-- fence      set_cr3 (serializing)    <-- fence
  load  cpus_using                       TLB fill: load PTE
```

The bad outcome: **A** reads `cpus_using` *without* B's bit (so A never shoots
down B) **and** **B**'s TLB fill reads the *old* PTE → B runs on a stale mapping
that is never invalidated.

| litmus location | kernel object                  | `0` means           | `1` means       |
| --------------- | ------------------------------ | ------------------- | --------------- |
| `pte`           | the PTE `pmap_enter` publishes | old / stale mapping | new mapping     |
| `cpus`          | B's bit in `pmap->cpus_using`  | B not yet marked    | B marked active |

Catastrophe = `0:EAX=0 /\ 1:EAX=0` (A saw `cpus=0`, B saw `pte=0`).

Source: `PMAP_TLB_WRITE_BARRIER` and `PMAP_UPDATE_TLBS` in
`uros/src/mach_kernel/intel/pmap.c`, `set_dirbase` in
`uros/src/mach_kernel/intel/pmap.h`.

## Why this is the SB shape, and why x86-TSO decides it

On x86 the only architectural reordering is **store→load**. Each thread stores one
location then loads the other — the textbook **SB** (store-buffer) test. Under
x86-TSO the `(0,0)` outcome is forbidden **iff both threads fence** between their
store and their load. So the barrier is neither obviously redundant nor obviously
sufficient — exactly the question herd7 answers.

Barriers are modeled as `MFENCE` (full StoreLoad fence):

- **P0** `lock; addl $0,0(%esp)` (the #338 `PMAP_TLB_WRITE_BARRIER`) ≡ `MFENCE`.
- **P1** `set_cr3` (serializing CR3 load) ≡ `MFENCE`.

The real `cpus_using` write is itself a `lock; bts`, an *additional* implicit fence
on P1. Modeling it as a plain store plus the `set_cr3` fence is the weaker,
conservative choice: if the model says the pair is sufficient, the strictly
more-ordered real code is too.

## The suite

| file                  | A fence | B fence | represents                  | expected    |
| --------------------- | ------- | ------- | --------------------------- | ----------- |
| `sb-pmap-both.litmus` | yes     | yes     | shipped #338 state          | `Never`     |
| `sb-pmap-noA.litmus`  | no      | yes     | pre-#338 (A unfenced)       | `Sometimes` |
| `sb-pmap-noB.litmus`  | yes     | no      | B's serializer removed      | `Sometimes` |
| `sb-pmap-none.litmus` | no      | no      | baseline / harness liveness | `Sometimes` |

## Running

```sh
opam install herdtools7      # one-time; herd7 ships the x86tso model
eval $(opam env)
./run.sh                     # summary table; exits nonzero on any mismatch
./run.sh -v                  # + full herd7 output per test
```

`x86tso` is herd7's default model for the `X86` architecture, so no `-model` flag
is needed.

## Results

herd7 **7.58**, model **x86tso**. All four verdicts match expectation (`./run.sh`
exits 0):

| file           | verdict     | expected    | states | witnesses     |
| -------------- | ----------- | ----------- | ------ | ------------- |
| `sb-pmap-both` | `Never`     | `Never`     | 3      | Positive: `0` |
| `sb-pmap-noA`  | `Sometimes` | `Sometimes` | 4      | Positive: `1` |
| `sb-pmap-noB`  | `Sometimes` | `Sometimes` | 4      | Positive: `1` |
| `sb-pmap-none` | `Sometimes` | `Sometimes` | 4      | Positive: `1` |

The whole argument is visible in the state counts. With both barriers the model
admits **3** reachable states and the catastrophe is not among them. Remove either
barrier and a **4th** state appears — `0:EAX=0; 1:EAX=0`, the stale-and-unflushed
outcome, with a concrete witness. The difference between pre-#338 and post-#338 is
exactly that one state.

## Conclusion

Under x86-TSO the #338 barrier pair is **necessary and sufficient** for the
`pmap_enter` ↔ `set_dirbase` mutual exclusion:

- **Sufficient** — `sb-pmap-both` is `Never`: with `PMAP_TLB_WRITE_BARRIER` on A
  and the serializing `set_cr3` on B, no execution reaches a stale-and-unflushed
  TLB. This is a proof over the model, not a sampling result.
- **Necessary** — `sb-pmap-noA` is `Sometimes`: the pre-#338 code, where B still
  fenced via `set_cr3` but A did not fence, **admits the bug**. B's fencing alone
  never sufficed; #338's A-side barrier is what closes the hole. `sb-pmap-noB`
  shows the symmetric direction.
- **Not vacuous** — `sb-pmap-none` is `Sometimes`, so the catastrophe is
  expressible and reachable in this encoding; the `Never` above is a real result
  and not a test that forbids the state by construction.

No kernel change was required: #338 shipped the correct barrier. This suite is the
assurance artifact, and the reusable pattern for the kernel's other memory-ordering
sites (the TLB ack / `cpu_update_needed` protocol, future barriers).

### Beyond x86-TSO

x86-TSO is a strong model: it reorders only store→load, so on x86 a *single* full
fence per side suffices. This does **not** carry to weakly-ordered architectures —
under a model like RISC-V's RVWMO, loads and stores may be reordered far more
freely, and this same code would need explicit acquire/release or fence
instructions where x86 needs none. These tests are written against the `X86`
architecture; re-checking the same shapes under a weaker model is the natural
follow-up when the MD layer grows a second architecture.

**The second architecture has since arrived, and it is the one that needs no
re-checking.** x86-64 (v0.3.0, #403) is the same memory model as i386 —
x86-TSO in both cases, store→load the only reordering — so these verdicts
carry across the port unchanged and the suite is a regression for both
targets, not just the one it was written for. Confirmed green against the
x86-64 kernel while #410 was landing its barriers.

That is a gift this particular port received. It is worth being explicit
that it is the exception rather than the rule: the sentence above still
applies in full to a genuinely weak model, and the fact that one port cost
nothing here is not evidence that the next one will.
