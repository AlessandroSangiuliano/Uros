# Kernel lock-primitive audit for SMP (#303)

This is the audit summary required by issue #303 (v0.2.0 SMP epic).
Goal: verify that every locking primitive the kernel relies on compiles
to real atomic instructions when `NCPUS > 1`, and that the usage pattern
holds up under multi-CPU contention.

## Scope

Primitives covered:

- `hw_lock_*` — the raw hardware lock layer in `i386/i386_lock.S`.
- `usimple_lock_*` — portable simple lock in `kern/lock.c`, built on
  top of `hw_lock_*`.
- `simple_lock` family — `kern/lock.h` macros; on `NCPUS > 1` they
  alias to `usimple_lock_*`.
- `mutex_lock` / `mutex_try` / `mutex_unlock` — `i386/lock.h` macros;
  on `NCPUS > 1` they call `_mutex_lock` / `_mutex_try` from
  `kern/lock.c`, which in turn use `hw_lock_try` on the interlock.
- `decl_simple_lock_data(class, name)` — `kern/lock.h` storage
  declarator; expands to a `usimple_lock_data_t` instance on
  `NCPUS > 1`, to nothing on UP.

Special-cased bootstrap locks:

- `start_lock` — `i386/start.S`, hand-coded `xchgl` against a 4-byte
  word so it can run before the C stack / hw_lock library are usable.
  Already correct: the locked path is `xchgl %eax, PA(start_lock)`
  which is atomic on i386 (XCHG with memory operand always asserts
  LOCK#).

## Code-gen verification (`-DUROS_NCPUS=2`, `-O3`)

Disassembly of the SMP build confirms the hot primitives compile to
genuine atomic instructions:

```
c01a4b12 <hw_lock_try>:
    mov    0x4(%esp),%edx          ; lock pointer
    mov    $0x1,%cl
    xchg   %cl,(%edx)               ; <-- atomic swap (lock implicit)
    test   %cl,%cl
    jne    .Lfail
    mov    $0x1,%eax
    ret
.Lfail:
    xor    %eax,%eax
    ret
```

`xchg` with a memory operand has an implicit `LOCK#` assertion on
i386/x86-64 (Intel SDM Vol. 3A §9.1.2.1), so no explicit `lock` prefix
is required.  `hw_lock_unlock` and `hw_lock_lock` follow the same
pattern (`xchgb 0(%edx), %al/%cl`); `hw_lock_init` is a plain store
which is fine for one-time init before the lock is published.

`hw_lock_held` is intentionally not atomic — it's a "is the bit set?"
hint used by `usimple_lock` to spin in cache before retrying the
`xchg` (the standard test-and-test-and-set pattern).  Documented in
the source.

Higher-level primitives bottom out into `hw_lock_try`:

- `usimple_lock` (kern/lock.c:281) — spin-loop on `hw_lock_try`, with
  a `hw_lock_held` cache-friendly inner spin.
- `_mutex_lock` (kern/lock.c:401) — same pattern: `while (!hw_lock_try(&l->interlock))`.
- `_mutex_try` (kern/lock.c:384 / kern/lock.c:436) — single
  `hw_lock_try`.

The `simple_lock` macros in `kern/lock.h` alias to `usimple_lock` when
`NCPUS > 1` (lock.h:578), so they inherit the same atomic core.

## Spin policy

The kernel default is **plain spin**: no adaptive back-off, no sleep
on contention, no MCS / queued locks.  Rationale: SMP is brand new
(v0.2.0) and we haven't measured anything yet — adding fancier locks
before there's contention data would be premature optimisation.

If/when contention becomes measurable, the upgrade path is contained:
swap `hw_lock_try`'s tight spin for an exponential-back-off / PAUSE
spin, then a queued lock — all of `usimple_lock` / `_mutex_lock`
inherit it.

## Known follow-ups

The audit surfaced one concrete usage-level issue that does *not*
belong to a primitive but is exposed by the SMP build:

- **AP boot completion via `cpu_start()`** — split into its own
  sub-issue **#308** because the failure mode turned out to be
  outside the lock layer entirely.  Polled-COM1 byte probes (write
  `'Y'` at the very entry of `svstart` in `i386/start.S`, write `'M'`
  in `kern/startup.c::slave_main`) produced no output, meaning the
  AP stalls *before* reaching `svstart` — somewhere in the
  `slave_boot.S` trampoline, the shared `pstart` fork, or
  `slave_start`'s paging setup.  None of that is locking; the audit's
  contribution here was ruling that layer out as the culprit.

- Console driver `printf` is not cross-CPU safe.  The AP-side
  diagnostic printf from #301 incr 2 was visibly racing with the BSP
  output (mojibake / lost characters / hang).  Already worked around
  by having the BSP print on the AP's behalf, but the long-term fix
  is a per-CPU log ring with a single BSP-side drainer, or a real
  spinlock around the COM1 driver.  Not blocking #303 acceptance.

## Acceptance checklist

- [x] Every primitive's `NCPUS > 1` codegen verified atomic.
- [x] Sanity-counter check (`kern/lock_smoke.c`, called from
      `setup_main()` between `subsystem_init` and `cap_init`).
      Runs 1000 `simple_lock` / `counter++` / `simple_unlock` cycles
      on the BSP, panics if the final value isn't 1000.  Observed
      output on both `-DUROS_NCPUS=1` and `-DUROS_NCPUS=2`:
      `lock_smoke: 1000 acquire/release cycles, counter ok (#303)`.
      Multi-CPU contention is gated on #308 (AP boot completion);
      the test grows a second arm when an AP can actually contend.
- [x] Audit summary committed (this file).
