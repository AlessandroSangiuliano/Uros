# IPC Performance — Analysis and Possible Future Implementations

## Current state

ipc_bench measures ~500k msg/s on KVM (single core, small inline messages).
The critical path for every message is:

```
userspace: mach_msg_trap (SYSENTER)
kernel:    ipc_kmsg_alloc()
           ipc_kmsg_copyin()          # copies data from the sender task
           ipc_mqueue_send()          # queues or transfers
           -- context switch --
           ipc_mqueue_receive()       # receiver wakes up
           ipc_kmsg_copyout()         # copies data into the receiver task
           ipc_kmsg_free()
userspace: return from mach_msg_trap (SYSEXIT)
```

Every round trip involves 2 SYSENTER/SYSEXIT, 2 kmsg allocations, 2 bcopy,
2 locked port lookups, and at least 1 full context switch (with FXSAVE/FXRSTOR).

---

## 1. Continuations — Analysis of the state in the codebase

### The infrastructure already exists

OSF Mach has an almost complete continuations support, but it is **disabled**:

- `thread_t.continuation` (kern/thread.h:387) — field for the function pointer
- `thread_block(continuation)` (kern/sched_prim.c:1632) — accepts a continuation
- `Switch_context` (i386/cswitch.S:180) — saves the continuation in the thread and
  supports the resume through `Thread_continue` (cswitch.S:220)
- `call_continuation` (i386/locore.S:1186) — direct jump to the continuation
- `ipc_mqueue_receive` (ipc/ipc_mqueue.c:958) — accepts a continuation parameter

### How it is neutralized

In `thread_block_reason()` (kern/sched_prim.c:1520-1528):

```c
thread->at_safe_point = (int) continuation;   /* saves it as a flag */
/* ... */
continuation = (void (*)(void)) 0;            /* ZEROED */
```

The continuation is used only as a tag for the "safe point" mechanism
(safe abort/halt during IPC). Then it is forced to zero. As a consequence
`thread_invoke` always passes 0 to `switch_context`:

```c
old_thread = switch_context(old_thread, 0, new_thread);  /* continuation = 0, ALWAYS */
```

In `cswitch.S`, when the continuation is 0, the kernel does the full save
of the callee-saved registers and of the stack pointer — as if continuations
did not exist.

Moreover, the value passed by IPC is `SAFE_EXTERNAL_RECEIVE` (= -3), not
a real function pointer — it is a sentinel for thread_act.c, not an
executable continuation.

### Why it was disabled

Probable cause: OSF Mach's thread-activation model (thread_shuttle +
thread_activation split) is more complex than CMU Mach 3.0's. The safe-point
mechanism checks whether a thread can be interrupted during a receive.
Implementing real continuations required correctly separating the state
between shuttle and activation — work that was not completed.

### What it takes to re-enable them

1. **Do not zero the continuation** in `thread_block_reason` — propagate it
   all the way to `thread_invoke` → `switch_context` → `Switch_context`

2. **Write `ipc_mqueue_receive_continue()`** — a real function that:
   - Re-acquires `imq_lock(mqueue)`
   - Does `reset_timeout_check` if needed
   - Reads `self->wait_result` and `self->ith_state`
   - Completes the receive (copyout, seqno, etc.)
   - Returns to userspace via `return_from_trap`

3. **Pass a real continuation** (not -3) from `mach_msg.c` and `ipc_mig.c`

4. **Handle abort/interrupt** — if a thread with a continuation receives
   TH_ABORT, the kernel must call the continuation with an error, not do a
   stack unwind (there is no stack)

### Risks and drawbacks

| Risk | Detail |
|------|--------|
| **Debugging** | A thread blocked with a continuation has no kernel stack; backtrace is impossible in DDB and kgdb. db_trace must be extended to show `thread->continuation` |
| **Activation model** | Every activation has its own PCB; the thread_shuttle migrates between activations. The continuation must be saved/restored correctly during migrations |
| **Timer** | `reset_timeout_check(&self->timer)` after `thread_block` (ipc_mqueue.c:1019) must be replicated in the continuation |
| **Safe-point interaction** | The at_safe_point mechanism uses the continuation as a flag. Re-enabling it requires separating the two uses |
| **Subtle regressions** | The post-block code in `ipc_mqueue_receive` (lines 1018-1029) handles wait_result and lock re-acquire; all of it must be replicated without bugs in the continuation |
| **Test coverage** | Extensive IPC regression tests are needed before and after — the current ipc_bench does not cover timeout, abort, port death |

### Impact estimate

With continuations active on the synchronous IPC path, the switch does not save/restore
kernel registers and does not keep the stack allocated. Conservative estimate: 30-50%
reduction of IPC round-trip latency.

---

## 2. Dedicated kmsg Pool

### What it is
`ipc_kmsg_alloc` calls `kalloc(sizeof(ipc_kmsg_t) + msg_size)` for every
message. `kalloc` is a general-purpose allocator with a global lock. For
frequent, small messages this is a constant overhead.

A pre-allocated pool of `ipc_kmsg_t` + inline buffer up to a threshold (e.g.
256 bytes) allows O(1) lockless allocation (with a per-CPU free list).

### Structure

```c
/* ipc/ipc_kmsg.h */
#define IPC_KMSG_POOL_SIZE      512     /* pre-allocated messages per CPU */
#define IPC_KMSG_INLINE_MAX     256     /* threshold for pool vs kalloc */

struct ipc_kmsg_pool {
    ipc_kmsg_t  *freelist;              /* lockless LIFO stack */
    int          count;
    char         _pad[CACHE_LINE - sizeof(ipc_kmsg_t*) - sizeof(int)];
} __attribute__((aligned(64)));

/* one structure per CPU */
PERCPU_DEFINE(struct ipc_kmsg_pool, ipc_kmsg_pool);
```

### Flow

```c
ipc_kmsg_t ipc_kmsg_alloc(mach_msg_size_t msg_size) {
    if (msg_size <= IPC_KMSG_INLINE_MAX) {
        kmsg = percpu_pool_pop();
        if (kmsg) return kmsg;
    }
    /* fallback to kalloc for large messages or an exhausted pool */
    return kalloc(sizeof(ipc_kmsg_t) + msg_size);
}

void ipc_kmsg_free(ipc_kmsg_t kmsg) {
    if (kmsg->ikm_size <= IPC_KMSG_INLINE_MAX) {
        if (percpu_pool_push(kmsg)) return;
    }
    kfree(kmsg, sizeof(ipc_kmsg_t) + kmsg->ikm_size);
}
```

### Estimated gain
- `kalloc` on the hot path: ~50-100 cycles with lock.
- Pool hit: ~5-10 cycles (load + store on the free list).
- At 500k msg/s with 2 allocs/msg: ~50-100M cycles/s saved.

### Files involved

| File | Change |
|------|----------|
| `ipc/ipc_kmsg.c` | `ipc_kmsg_alloc()`, `ipc_kmsg_free()` |
| `ipc/ipc_kmsg.h` | pool definitions, threshold |
| `kern/cpu_data.h` | add `ipc_kmsg_pool` to `cpu_data_t` |
| `kern/startup.c` | initialize the pool for every CPU |

### Risks and drawbacks
- Fixed pre-allocated memory even when IPC is idle
  (512 × ~300 bytes = ~150 KB per CPU, acceptable).
- With SMP, stealing between CPUs adds complexity. For now (single core)
  not relevant.
- Pool exhaustion under load: the fallback to kalloc guarantees correctness
  but loses the benefit.

---

## 3. Zero-Copy for Large Messages

### What it is
For OOL (out-of-line) data, the kernel copies with bcopy. For large data
(> threshold) it is more efficient to transfer the ownership of the physical pages
from the sender task to the receiver without copying bytes.

### Existing mechanism

OSF Mach has `vm_map_copy_t` with three internal strategies:
- `VM_MAP_COPY_ENTRY_LIST` — list of vm_map_entry (large OOL)
- `VM_MAP_COPY_OBJECT` — vm_object with pages (CoW)
- `VM_MAP_COPY_KERNEL_BUFFER` — bcopy into a kernel buffer (small)

The problem: `ipc_kmsg_copyin_body` forces `VM_MAP_COPY_KERNEL_BUFFER`
even for data that would benefit from page remapping. The threshold must be
calibrated and the entry_list path completed.

### Recommended threshold

```c
/* ipc/ipc_kmsg.c */
#define IPC_KMSG_ZERO_COPY_THRESHOLD    (4 * PAGE_SIZE)   /* 16 KB */

if (ool_size >= IPC_KMSG_ZERO_COPY_THRESHOLD)
    vm_map_copyin(map, addr, size, TRUE /* destroy src */, &copy);
else
    /* classic bcopy */
```

### Files involved

| File | Change |
|------|----------|
| `ipc/ipc_kmsg.c` | `ipc_kmsg_copyin_body()`, `ipc_kmsg_copyout_body()` |
| `vm/vm_map.c` | check `vm_map_copyin` with `destroy=TRUE` |

### Risks and drawbacks
- "Move" semantics: the sender loses the pages after the send.
- CoW introduces a lazy page fault in the receiver at the first write.
- A bug in the ownership transfer corrupts the VM of one of the two tasks.

---

## 4. Per-Thread Port Lookup Cache

### What it is
`ipc_port_translate_send(space, name)` does a hash table lookup + `ip_lock`
on every operation. A per-thread cache of the last N (name → ipc_port_t*).

### Structure

```c
/* kern/thread.h */
#define IPC_PORT_CACHE_SIZE     4

struct ipc_port_cache_entry {
    mach_port_name_t    name;
    ipc_port_t         *port;
    ipc_space_t        *space;      /* for invalidation */
};

struct thread {
    /* ... */
    struct ipc_port_cache_entry port_cache[IPC_PORT_CACHE_SIZE];
};
```

### Invalidation
The cache must be flushed when:
- The port is destroyed (`ipc_port_destroy`)
- The name is deallocated from the space
- The thread changes task/space

### Risks and drawbacks
- 64 extra bytes per thread (negligible).
- Incorrect invalidation → use-after-free. A generation counter
  or a validity check before using the cache is needed.
- Complexity in the port destruction path.

---

## 5. Direct Thread Switch (Send→Receive Fast Path)

### What it is
When the sender calls send and the receiver is already blocked in receive,
the kernel transfers the message and does a direct context switch, bypassing
the scheduler.

### Flow

```
mach_msg_send (client):
    if (receiver blocked on this port):
        transfer kmsg directly to receiver->ith_kmsg
        Switch_context(client, receiver)   /* immediate switch */
```

### Candidate code in the codebase

`mach_msg.c:1269` already checks whether the receiver is in `SAFE_EXTERNAL_RECEIVE`:

```c
if (!(((receiver->at_safe_point == SAFE_EXTERNAL_RECEIVE) || ...
```

This check is the natural hook for the direct switch.

### Files involved

| File | Change |
|------|----------|
| `ipc/ipc_mqueue.c` | `ipc_mqueue_send()` — detect a ready receiver |
| `kern/sched_prim.c` | `thread_handoff(old, new)` |
| `i386/cswitch.S` | handoff support |

### Risks and drawbacks
- On SMP, a direct switch makes sense only if sender and receiver are
  on the same CPU.
- It bypasses the scheduling policy (the receiver might have lower
  priority). To be decided whether that is acceptable.
- Critical window in which both threads change state atomically.

---

## Priorities and Suggested Roadmap

| # | Optimization | Impact | Complexity | Dependencies |
|---|---------------|---------|-------------|------------|
| 1 | kmsg pool | high | low | none |
| 2 | Continuations (re-enable) | very high | medium-high | safe-point analysis |
| 3 | Direct thread switch | high | medium | benefits from continuations |
| 4 | Port lookup cache | medium | medium | none |
| 5 | Zero-copy OOL | medium | high | stable vm_map_copy |

**Suggested sequence**: kmsg pool → continuations → direct switch.

The continuations exist in the code but must be re-enabled with care: the
safe-point mechanism must be separated, the IPC continuation must be written, and the
debugging tools (DDB, kgdb) must be extended. It is not work from scratch but
it requires attention to the details of the activation-shuttle model.

## Post-optimization target estimate

| Configuration | estimated msg/s |
|----------------|---------------|
| Current (baseline) | ~500k |
| + kmsg pool | ~600k |
| + continuations | ~1.2M |
| + direct switch | ~2M |
| + port cache | ~2.2M |

Conservative estimates, based on CMU Mach 3.0 / Mach 4 data on comparable
hardware (i486/Pentium). Modern hardware with large caches and branch
predictors might give better results.

## References

- Draves, R. et al. — "The Case for User-level Resource Management" (1993)
- Bershad, B. et al. — "Lightweight Remote Procedure Call" (SOSP 1989)
- Liedtke, J. — "On Micro-Kernel Construction" (SOSP 1995)
- OSF Mach: `ipc/ipc_mqueue.c`, `kern/sched_prim.c`, `kern/mach_msg.c`
- Mach 4 (Utah): continuations implementation in `kern/thread.h`
