# What MACH_ASSERT switches, and what each thing under it is (#485)

Both targets built `CMAKE_BUILD_TYPE=Release` with `MACH_ASSERT=1`, and the
question "should Release carry assertions" had no answer while there was one
build configuration named after the one it is not. This is the record of
sorting out what was actually filed under that switch.

The mechanical half is `scripts/assert-census.py`; run it rather than trusting
the counts below, which are a snapshot. This file is the half a tool cannot
do: what each thing **is**.

## The rule

Four kinds, not three. The issue was written expecting three; reading the code
found a fourth that explains most of the damage.

| kind | belongs | may compile out |
|---|---|---|
| **invariant** | under `MACH_ASSERT` | yes — the caller is wrong, and a build may take their word |
| **integrity** | outside it, always on | **no** — it stands between corruption and a wild dereference |
| **hunt apparatus** | its own switch, default off | yes, and it should not have needed saying |
| **implementation swap** | nowhere near an assertion switch | — it is not a check at all |

The tree already knew the rule in two places before this issue: `first_free_check`
in `vm_map.c` and `vm_page_free_verify` in `vm_resident.c` are both expensive
checks behind their own runtime flag, default off. Everything below is bringing
the outliers into line with those two.

### Telling integrity from invariant

An invariant says *this cannot happen and the program is wrong if it does*. An
integrity check says *this word is not what it must be, and the next line
dereferences it*. The test is not how serious it sounds; it is whether removing
it turns a detected corruption into a followed pointer.

## What was found, and what was done

### Hunt apparatus wearing the word "assert"

Four, and together they were most of the cost.

| what | where | cost | now |
|---|---|---|---|
| #385 free-page poison | `vm_page_grab`, `vm_page_release` | **44% of a copy-on-write fault**; kernel fault 5,790 → 3,390 cycles when gated (−41%) | `VM_PAGE_POISON`, off (#482) |
| port allocation list | `ipc_port_init`, `ipc_object_destroy` | a **global mutex per port alloc and free** | `IPC_PORT_TRACK`, off |
| port debug fields | `struct ipc_port` | **96 bytes/port i386, 152 x86-64** — 41–44% of the struct; 6.7 MB of the port zone | follows `IPC_PORT_TRACK` |
| `-Z` zone poison | `ADD_TO_ZONE`, `REMOVE_FROM_ZONE` | already behind `zfree_clear`, default off — a load and a branch | left, and `-Z` now *says* when the machinery is absent |

### Integrity checks that could be compiled away

Four, now unconditional.

- **`REMOVE_FROM_ZONE`'s range check.** The free-list head is not a kernel data
  address ⇒ something wrote into freed memory, and the next line follows it.
- **`zfree`'s `from_zone_map` check.** A pointer from elsewhere about to be
  linked into this zone's free list, where the next allocation hands it out.
  Catching it here is catching it at the door.
- **`choose_thread` and `choose_pset_thread`.** `runq->count` and the bitmap say
  a thread sits at `runq->low`; if they disagree with the queue, `q->next` is
  the queue head cast to `thread_t` and the next line writes through it.

The other three of `zfree`'s checks — NULL zone, freeing into `zone_zone` — are
invariants and stay.

### Implementation swaps, which are not checks

`MACH_ASSERT` does not only add assertions. In three places it changes what the
code **is**, so that there is a function body to assert inside:

- `i386/pio.h` — the inline port accessors compile **only when assertions are
  off**, so they had never been compiled at all. They did not assemble (a
  hand-written `0x66` prefix a current assembler rejects) and did not link
  (`extern __inline__` emits a definition under `-std=gnu11`, colliding with
  `locore.S`). Fixed, not deleted: what heritage stays is #433's question.
- `i386/AT386/mp/mp.h` — `DISABLE_PREEMPTION` becomes a **call** with three
  registers saved around it instead of an inline sequence, and `interrupt.S`
  uses it on every interrupt entry and exit.
- `vm/vm_map.h` — `vm_map_reference` and its siblings become **functions**
  instead of macros that inline lock-and-increment. 24 call sites.

⚠️ This is why the measurement this issue owes is not "what do the assertions
cost". Part of what `MACH_ASSERT` costs is de-inlining.

### Correct already

`device/net_device.c` is the one place that got this right without being told:
its `#else` branch does not drop the unit bounds check, it rewrites it as an
explicit `if` and `panic`, because `assert()` would be dead there.

### Fields that only assertions read

Flagged by the tool's assignment heuristic and **not** defects, recorded so
nobody re-reads them: `kernel_stack_swapped_in` and `user_stack_swapped_in`
(`thread_swap.c`, `thread_act.c`) and `vm_object_template.paging_object`. The
one apparently unguarded read, `thread_act.c:1197`, is itself inside an
`assert()`. A tool cannot tell a field only assertions read from one the kernel
reads; this is the record of a person having checked.

### Behaviour that was hiding under the switch

Two, now unconditional, because they were not checks at all:

- `vm_page_grab_fictitious` set `m->fictitious = TRUE` only under
  `MACH_ASSERT || ZONE_DEBUG`, and 113 unguarded places read that field.
- `act_machine_switch_act` cleared `->lower` and `->higher` — the activation
  stack's own links, read from 55 unguarded places — only under `MACH_ASSERT`.

## The two kernels

`UROS_KERNEL_FLAVOR` is the knob; `CMAKE_BUILD_TYPE` is the optimisation axis
and answers a different question. Every configure prints which kernel it is
building, and the flavour drives userland's `assert()` as well — the kernel and
the userland beside it used to have opposite policies in the same build.

    cmake -DUROS_KERNEL_FLAVOR=development   # default: assertions on
    cmake -DUROS_KERNEL_FLAVOR=release       # off

**A release kernel is not a silent one.** `panic()` is in `kern/debug.c` with no
guard; `Assert()` is what sits under the switch. Every always-on check above
still fires, still prints, still backtraces. What a release kernel loses is
*earliness*: the report arrives at the consequence rather than at the cause,
where the stack no longer names the culprit. Getting a report off a machine in
the field is #373 and no setting here substitutes for it.

⚠️ Before this issue, `MACH_ASSERT=OFF` had **never once compiled**. Three
latent defects had to be fixed to make it a configuration that exists.

## Still open

- **The number.** What survives into a development kernel has not been measured
  — and it is not only the checks, it is the de-inlining above.
- **The #385 canary** in `vm_page_release`. Same category as the integrity
  checks moved out, but its cost is a lock per page free rather than a
  comparison, and no one has measured it. Deciding without that number would be
  the same kind of sentence as the one this work deleted from its comment
  ("tied to MACH_ASSERT so release/bench builds shed it" — never true).
- **The port-tracking A/B**, on a build directory nothing else is rebuilding.
