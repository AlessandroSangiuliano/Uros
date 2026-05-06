# QEMU TCG soft-MMU PT-write reproducer (work-in-progress)

Standalone i386 multiboot kernel built to isolate a suspected QEMU TCG
soft-MMU bug observed during early-boot of the
[osfmk-mklinux kernel](https://github.com/AlessandroSangiuliano/Uros)
under `-accel tcg`.  See upstream issue
[#192](https://github.com/AlessandroSangiuliano/Uros/issues/192) for the
full investigation.

## Symptom in the upstream kernel

Inside an in-progress `#PF` handler, writing a fresh PTE into the kernel
page table via its higher-half linear-map alias appears to succeed (no
fault, execution proceeds, volatile read-back happens), but the write
never reaches physical memory: subsequent walks find the same PTE slot
zero and the handler retries forever.  Verified directly with the QEMU
monitor (`xp /4xw <pa>` returns `0` after the alias store).

A neighbouring linear-map page that is *not* used as a PT accepts and
retains writes in the same handler context — which pointed at "writes
to a PT page via its data alias get dropped" as the working hypothesis.

KVM does not exhibit the issue.

## Why this reproducer exists

To bisect the trigger from the minimal possible setup, separate from
the rest of the kernel, so the case can either:

1. Be filed upstream as a clean QEMU bug report, or
2. Falsify the "TCG bug" diagnosis and force a re-investigation of the
   upstream kernel's own state.

## What it does

The kernel:

1. Identity-maps the first 4 MB via `pt0`.
2. Re-installs `pt0` at `PD[768]` so the same range is also reachable
   via `0xC0000000+` — kernel-style "linear map" / `phystokv` alias.
3. Installs an empty `pt1` at `PD[769]`, so `0xC0400000+` faults.
4. Enables paging with `CR4.PGE=1` and `CR0.WP=1`, with `G` set on
   every entry — matching the upstream kernel's CR0/CR4 state where
   the bug was first observed.
5. Runs two tests:
   - **Test 1**: from `kmain`, writes a sentinel into `pt0` via the
     higher-half alias.  No fault context.
   - **Test 2**: deliberately accesses `0xC0400000`, takes a `#PF`,
     and from inside the handler installs the missing PTE in `pt1`
     via its higher-half alias.  If the alias write is dropped, the
     handler retries forever — the bookkeeping in the handler aborts
     the test after 4 retries and prints `FAIL`.

## Current status

**The reproducer does NOT trigger the upstream symptom, but the upstream
diagnosis is confirmed by direct in-kernel instrumentation.**

```
$ make run-tcg-icelake
...
[Test 1] alias write to pt0[100] (no fault context)
  va=0xc0107190 before=0x00064103 after=0xdeadbeef  PASS
[Test 2] deliberate #PF on VA 0xC0400000 — handler will
  install a PT entry in pt1 via its higher-half alias.
  load completed, value = 0x00000000
  retries inside handler: 0x00000001  PASS
=== DONE ===
```

Same result with `-cpu qemu32`, `-cpu Icelake-Server,+sha-ni`, with and
without `CR4.PGE` / `G` bit / `CR0.WP`.

### What in-kernel instrumentation showed (2026-05-05)

Running the upstream kernel under TCG with a printf inside `pmap_enter`
that captured `*pte` immediately before and immediately after the
`WRITE_PTE`:

```
pmap_enter: kernel v=0xf4953000 pa=0x1f4bf000 ptep=0xc00d254c
            before=0x0 imm=0x0 tmpl=0x1f4c0203 pde=0xd2103
```

- `ptep = 0xc00d254c` — VA of a slot in the page-table page that
  maps `0xf4953000`, reached via the higher-half kernel alias of its
  physical address (`0xd2000`).
- `tmpl = 0x1f4c0203` — value we are storing (PA + WIRED + WRITE +
  VALID).
- `before = 0x0`, `imm = 0x0` — the slot is zero before the write
  and is **still zero** when re-read on the very next C statement
  through the same pointer.

The store is completely lost.  This was tested with an `invlpg` on
the alias VA inserted both before AND after the store; no effect.
A subsequent `pmap_pte()` lookup on the same VA returns a fresh
pointer that reads the same zero.

So the upstream hypothesis ("writes to a PT page via its data alias
get silently dropped under TCG") is **correct**.  The symptom is not
a TLB issue and not a kernel-side miswiring; the `mov` instruction
itself does not commit to RAM under TCG when the destination physical
page is currently in use as a page table by the active CR3.

### Why this reproducer still does not fire

The standalone case never sees the same drop, so something the
upstream kernel does is required to provoke TCG.  Plausible missing
ingredients:

- TCG translation-block cache state that only builds up after many
  thousands of instructions and faults (this reproducer hits the
  test write within hundreds of instructions of paging-on).
- Specific MMU access pattern: e.g., the failing PT page also acts
  as the linear-map alias for itself plus has been *read through the
  MMU as a translation source* enough times to enter a TCG-internal
  PT-page cache that the reproducer never primes.
- Specific register state (selectors, MTRR/PAT, XSAVE area) the
  reproducer does not replicate.

Capturing the exact upstream CR0/CR4/MTRR/PAT/XSAVE state at the
moment of the failing write and replaying it here is the next step
if this reproducer is to be turned into a QEMU bug report.

### Upstream consequence

The kernel cannot fix this on its own — every `*ptep = …` store in
`pmap_enter` is subject to the same drop.  The chosen mitigation in
osfmk-mklinux is to keep early-boot allocators away from the
`kmem_alloc_wired + bzero` pattern (BSS pages, pre-fill of the pv
free list, etc. — see issue #190 + follow-ups).

## Build and run

Requires `gcc` with `-m32` support, `binutils`, `qemu-system-i386`.

```
make
make run-tcg            # plain qemu32, TCG
make run-tcg-icelake    # Icelake-Server,+sha-ni, TCG (matches upstream)
make run-kvm            # KVM control (always passes)
```

Output goes to stdio (the `-serial mon:stdio` already implied by
`run-*` targets).  Press Ctrl-A then X to exit QEMU.
