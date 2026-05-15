# Uros Resource Model — capabilities, budgets, accounting

> Design document — synthesises decisions taken during the #220 / #217 /
> #229 / #216 discussions.  Companion to the per-issue tracking on
> GitHub.  Living document; revise when decisions change.
>
> **Status**: design finalised, implementation pending (#229 + #216 v2.4).

## Goal

Define the three orthogonal mechanisms Uros uses to control resource
usage in a multiserver setting, the responsibilities of each, and how
they interact.

## The Mach principal-of-account problem

In a microkernel multiserver design, server `S` does work on behalf of
client `C`.  When `S` allocates RAM, burns CPU cycles, performs I/O,
or holds a cache entry to serve `C`'s request, the cost lands on `S`'s
own task — not on `C`.

```
   client C                 server S (task)
   ────────                 ──────────────
   fs_open("/foo")  ──MIG──►  alloc 4 KB struct file
                              read inode (CPU + I/O)
                              cache 16 KB block (RAM)
                  ◄───────  return handle

After 1000 such calls without close():
   C: 0 bytes used (it just holds a handle integer)
   S: 20 MB used, 1000 dangling structs
```

Result with classical Mach: a misbehaving (or compromised) `C` can
exhaust shared servers via volume — **the wrong principal pays**.
Eventually `S` is OOM-killed; `C` is fine.

Mach 3's original answer was the "account port" idea: pass an opaque
accounting principal with every RPC.  It was never widely deployed —
too invasive, too imprecise, no clear charging model.

Uros's answer: **three orthogonal mechanisms**, each solving a
distinct part of the problem.

## The three axes

| Axis | Question | Component | Frequency | Cost |
|------|----------|-----------|-----------|------|
| **Permission** | Does `C` have the right to op `X` on resource `R`? | libcap / cap_server | once per cap, then cached | ~ns (cached HMAC verify) |
| **Quota** | Can `C` afford the cost?  Has it exceeded its budget? | **libbudget** | every operation, hot path | ~5 ns (1 atomic) |
| **Accounting** | How much has `C` actually used so far? | kernel `task_info` + proc_server | only when ps/top reads | ~1 µs (RPC, only on demand) |

All three are needed for a healthy system:

- **Permission alone** (today): once `C` has the cap, it can spam
  unbounded volume → server falls over.
- **Permission + quota**: `C` has the cap but exhausts its budget →
  server returns EAGAIN → `C` fails, server lives on.  ✓
- **Accounting alone**: we *see* the damage as it happens but cannot
  prevent it.

The three components are loosely coupled — the same RPC may consult
all three in succession, but each has its own state, its own update
cadence, and its own observation surface.

## libcap / cap_server — permission layer

Already in tree.  Responsibility: certify "principal `P` is allowed
to perform op `X` on resource `R`".

- Caps are wire-format tokens (HMAC-signed by cap_server) carried in
  RPC arguments
- `cap_verify` is an inline HMAC check — no IPC needed on the hot
  path
- Caps can be delegated, revoked, expire on a deadline
- Tomorrow (#216 v2): manifests + principal-aware policy + concurrency
  / rate quotas on cap_request itself

What it does NOT do today: anything quantitative about consumption.
A holder of a valid `BLK_OPEN` cap can call `device_open` 10⁶ times
in a loop and cap_server will say "yes, yes, yes, ...".

## libbudget — quota layer (new, #229)

The piece designed in this round.  Responsibility: bound how much a
principal can consume of any given resource class, *before* the
consumption happens.

### Wire layout — one shared page per (principal, resource-class)

```c
struct budget_acct {
    uint32_t  magic;              /* "BDGT" */
    uint32_t  version;            /* 1 */

    uint64_t  remaining;          /* atomic_fetch_sub on each charge */
    uint64_t  quotum;             /* refilled to this on each period */
    uint64_t  period_ns;          /* refill interval, 0 = no refill */
    uint64_t  last_refill_ns;     /* monotonic */

    uint32_t  flags;              /* BUDGET_FLAG_* */
    uint32_t  _pad;
};
```

Same engineering pattern as FLIPC v2 channels: shared memory between
two parties, hot-path is a lock-free atomic, the kernel is involved
only at setup and refill.

### Hot path — inline, no syscall

```c
static inline int
budget_charge(budget_t b, uint64_t cost) {
    int64_t after = (int64_t)__atomic_sub_fetch(
        &b->acct->remaining, cost, __ATOMIC_RELAXED);
    if (after < 0) {
        __atomic_add_fetch(&b->acct->remaining, cost, __ATOMIC_RELAXED);
        return -1; /* over budget */
    }
    return 0;
}

static inline void
budget_refund(budget_t b, uint64_t cost) {
    __atomic_add_fetch(&b->acct->remaining, cost, __ATOMIC_RELAXED);
}
```

Cost: ~5 ns on a hot cache line.  Negligible compared to the ~1.4 µs
Mach RPC every fs_open takes anyway.

### Server-side use pattern

```c
/* Inside ext_server, dispatching fs_open(C, "/foo") */
if (budget_charge(C.budget_fs_ops, 1)    < 0) return EAGAIN;
if (budget_charge(C.budget_mem,    4096) < 0) {
    budget_refund(C.budget_fs_ops, 1);
    return ENOMEM;
}
allocate_struct_file();
read_inode();
/* ...do the actual work... */
return KERN_SUCCESS;

/* On fs_close: budget_refund both counters */
```

Servers are trusted to estimate cost honestly.  A buggy server that
under-estimates does not break the safety property *for other clients*
— it only over-spends from its own per-server reserve.

### No separate budget_server

Decided after design discussion: budget lifecycle (creation, refill,
revocation) lives inside `cap_server`.  Reasons:

- cap_server already does issue/revoke of caps and the corresponding
  `vm_remap` of shared state
- Budget pages have the same lifecycle as caps: issued together,
  revoked together
- One less binary, no inter-server sync to coordinate

`libbudget` itself is just a static lib with the wire layout and the
inline hot-path helpers.  No new server process.

### Resource classes (v0.1)

Plain `uint64_t` counters; the class is convention.  Initial set:

- `BUDGET_CPU_NS`     — accumulated CPU time
- `BUDGET_MEM_BYTES`  — currently allocated bytes (refunded on free)
- `BUDGET_FS_OPS`     — open file descriptors / outstanding handles
- `BUDGET_IPC_BYTES`  — bytes in flight / queued

Adding a new class is just defining a new constant — no code change
in libbudget.

## Accounting — proc_server + kernel `task_info`

The reportistic layer.  Responsibility: tell `ps`, `top`, system
monitors *how much* a process has consumed.

Decision: accounting data is **NOT cached** in proc_server.  It lives
where it naturally is and gets fetched on demand.

| Datum | Owner | Read path |
|-------|-------|-----------|
| `pid`, `ppid`, `cmdline`, `start_time` | proc_server `pid_entry` | local read in proc_server |
| `state` (R/S/Z/T) | proc_server (it owns wait/signal/exit) | local read |
| `current_rss`, `vsize` | kernel `vm_map` | `task_info(task_port, BASIC_INFO)` RPC |
| `cpu_time` | kernel scheduler | `task_info(task_port, THREAD_TIMES_INFO)` RPC |
| `n_threads` | kernel | `task_threads(task_port, ...)` RPC |
| `n_open_fds` | libposix-uros of the target task | per-task RPC, only `lsof`-class tools |

`ps` flow:

```
ps -ef
   ↓ readdir("/proc")
libvfs.fs_readdir → proc_server enumerates pid_entries  (1 RPC)
   ↓ for each pid: open + read /proc/N/stat
libvfs.fs_open + fs_read → proc_server:
   1. lookup pid_entry                            (~ns)
   2. task_info(entry.task_port, BASIC_INFO)      (~1.4 µs RPC)
   3. printf Linux-procfs-compatible format
   4. return buffer
```

Per `ps` on 100 processes: ~280 µs total — humanly imperceptible.
On Linux the same call costs ~50–100 µs (direct syscalls).  We pay
~3–5× more on a tool that runs once per human action.

What we deliberately don't do: have the kernel push notifications
into proc_server on every page allocation or scheduler tick.  That
would add real overhead on the hot path with zero observable benefit
(no one reads `current_rss` 1000 times per second).  Pull on demand
keeps the model simple and the steady-state cost zero.

## Why three layers and not two

A reasonable counter-proposal: fold budget into accounting (one
counter, "used N MB out of M MB allowed").  We rejected it because:

1. **Different cadence**: budget updates 10⁵–10⁶ times per second
   (every IPC), accounting updates only on demand.  Bundling them
   forces accounting onto the hot path.
2. **Different correctness regime**: budget is *hard enforcement*
   (must not race, must not over-grant), accounting is *best-effort
   visibility* (a brief stale read is fine).  Different memory
   ordering, different invariants.
3. **Different consumers**: budget is checked by every server doing
   work for a client; accounting is read by `ps` / `top` only.

A reasonable counter-proposal in the other direction: fold permission
into budget ("0 budget = no permission").  We rejected it because:

1. **Different lifecycle**: permission is granted at task spawn
   (manifest), expires on revocation.  Budget refills periodically.
2. **Different failure mode**: lacking permission is "you may never
   do this"; lacking budget is "you may, but not now".  Distinct
   error codes, distinct user-facing messages.
3. **Different storage**: permission is sparse (~tens of caps per
   task); budget is dense (one counter per resource class per
   principal — ~tens of counters that get touched constantly).

## Concrete end-to-end example

A new task `firefox` is spawned.  Its manifest grants:

- caps for `BLK_READ`, `BLK_WRITE` on a specific partition
- caps for `GPU_DISPLAY_SCANOUT`
- caps for `FS_OPS` and `NET_ACCESS`

Plus budgets:

- `BUDGET_CPU_NS`    quotum = 500 ms / second (50% of one core)
- `BUDGET_MEM_BYTES` quotum = 1 GiB  (no period: hard cap)
- `BUDGET_FS_OPS`    quotum = 1024  (no period: hard cap)
- `BUDGET_IPC_BYTES` quotum = 100 MB / second

### Normal operation

```
firefox: vm_allocate(100 MB)
   → kernel: cap_check(firefox, RESOURCE_VM, ALLOC, 100MB)        OK
   → kernel: budget_charge(firefox.mem, 100MB)  → remaining 924MB OK
   → vm_allocate succeeds
   → (no notification to proc_server — kernel vm_map is the source of truth)

firefox: open("/some/file")
   → libposix → libvfs.vfs_open → fs_open RPC → ext_server
   → ext_server: budget_charge(firefox.fs_ops, 1)  → remaining 1023 OK
   → ext_server: budget_charge(firefox.mem, 4096)  → remaining 924MB OK
   → ext_server: do the open, return handle
```

### Resource exhaustion

```
firefox: leaks file handles in a loop
   → after 1024 opens without close:
   → ext_server: budget_charge(firefox.fs_ops, 1)  → -1 → return EAGAIN
   → firefox sees EAGAIN, must close some handles before opening more
   → ext_server is healthy, other clients unaffected
```

### Reporting

```
user: top
   → for each pid: open /proc/N/stat → proc_server
   → proc_server: task_info(firefox.task_port) → kernel
   → kernel: rss = 924 MB, cpu_time = ...
   → top displays: firefox  924M  47%CPU
```

Note: `top` shows `cpu_time` and `rss` from kernel state, **not from
libbudget**.  Budget says "what you may consume going forward";
accounting says "what you have consumed up to now".

If you also want to display the budget remaining (debug-friendly),
proc_server can read the shared `budget_acct` page and expose it
under `/proc/N/budget`:

```
$ cat /proc/firefox_pid/budget
cpu      remaining=210ms quotum=500ms period=1000ms
mem      remaining=100MB quotum=1024MB period=infinite
fs_ops   remaining=830 quotum=1024 period=infinite
ipc      remaining=80MB quotum=100MB period=1000ms
```

This is purely cosmetic — the page is read non-atomically, the value
may be stale by a few ns, fine for human eyes.

## Implementation order

| # | Issue | What lands |
|---|-------|------------|
| 1 | #227 | `libelf` — refactor preliminare, sblocca exec_server |
| 2 | #228 | `exec_server` — usa libelf |
| 3 | #217 | `proc_server` — usa exec_server, owns pid/wait/signals |
| 4 | #229 | `libbudget` — primitiva standalone (può procedere in parallelo) |
| 5 | #216 v2.x | `cap_server v2` — manifest, then quota, then budget binding |

Items 1-3 are sequential.  Item 4 is independent and can be done in
any order (it just needs cap_server to host budget pages, and
cap_server v2.4 is the integration step).

## References

- Walfield & Brinkmann, "A critique of the GNU Hurd Multi-Server
  operating system", 2007 — read for the **problem statement** of
  principal-of-account in microkernel design.  Uros does not adopt
  any of Hurd's solutions.  See `feedback_no_hurd.md` and
  `feedback_study_never_copy.md` in the auto-memory.
- FLIPC v2 design (`docs/flipc2.md`) — the shared-memory + atomics
  pattern libbudget reuses.
- Issue #216 — cap_server v2 policy (manifest / principal / quota /
  budget integration).
- Issue #229 — libbudget tracking issue.
- Issue #217 — proc_server tracking issue (process state, accounting
  consumer).

## Open questions / future work

- Hierarchical budgets (parent refills child) — needed for cgroup-like
  semantics, but no clear use case yet on Uros.  v0.3 of #229.
- Latency-class budgets — separate from quantity budgets, for
  real-time work.  v0.4 of #229.
- Server self-attribution: a server that does background work not
  tied to a client (e.g. cache eviction thread) — does it pay from
  its own server-wide budget?  Yes, treat the server as another
  principal.
- DMA budgets: device DMA operations bypass CPU but consume bus and
  memory.  Probably a `BUDGET_DMA_BYTES` class enforced in the device
  driver server.
- Budget for kernel-side resources (port rights, vm_map entries).
  Today they are unbounded; long-term should fall under the same
  model.
