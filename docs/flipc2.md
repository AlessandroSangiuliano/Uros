# FLIPC v2 — High-Performance IPC for Uros

**Library version: 0.0.3**

## Table of Contents

1. [Introduction](#1-introduction)
2. [Architecture](#2-architecture)
3. [When to Use FLIPC v2 vs Mach IPC](#3-when-to-use-flipc-v2-vs-mach-ipc)
4. [API Reference](#4-api-reference)
5. [Tutorial](#5-tutorial)
6. [Performance](#6-performance)

---

## 1. Introduction

FLIPC v2 is a high-performance IPC mechanism for the Uros operating system. It
provides shared-memory, lock-free communication channels between tasks, designed
for hot-path data transfer where Mach IPC overhead is unacceptable.

**FLIPC v2 is complementary to Mach IPC, not a replacement.** Mach IPC provides
capabilities, port rights, and fine-grained security. FLIPC v2 provides raw
speed for trusted server-to-server and driver-to-server communication.

### Key Properties

- **Zero kernel traps** on the fast path (produce/consume)
- **Lock-free** SPSC (Single Producer, Single Consumer) ring buffer protocol
- **Adaptive wakeup**: spin-poll first, fall back to Mach semaphore
- **Cache-line aligned** structures to eliminate false sharing
- **Corruption detection** and DoS defense (spurious wakeup limit)
- **Named endpoint discovery** via the Mach name server
- **Shared buffer pools** for multi-channel memory pooling

### Use Cases

| Scenario | Example |
|----------|---------|
| Disk I/O pipeline | ext2_server → ahci_driver (commands + completion) |
| Block read/write | client → ext2_server (high-throughput data) |
| GPU rendering | game engine → GPU driver (command buffer + textures) |
| Audio streaming | audio server → client (low-latency PCM frames) |
| Inter-server data bus | any pair of trusted servers |

---

## 2. Architecture

### 2.1 Channel

A **channel** is the core FLIPC v2 primitive. It connects exactly two tasks in a
point-to-point, unidirectional link: one **producer** and one **consumer**.

A channel consists of a single shared memory region containing:

```
Offset 0                                              Offset channel_size
┌───────────────────┬─────────────────┬──────────────────────┐
│  Channel Header   │  Descriptor     │    Inline Data       │
│  (256 bytes)      │  Ring           │    Region            │
│  4 cache lines    │  (N × 64 B)    │                      │
└───────────────────┴─────────────────┴──────────────────────┘
```

- **Channel Header** (256 bytes): magic, version, layout offsets, producer/consumer
  pointers on separate cache lines, semaphore ports, statistics.
- **Descriptor Ring**: circular array of 64-byte descriptors (one cache line each).
  Power-of-2 sizing enables fast modulo via bitmask.
- **Inline Data Region**: bulk payload area referenced by descriptors via offset+length.

For bidirectional communication (RPC pattern), create **two channels**: one for
requests (forward), one for replies (reverse).

### 2.2 Descriptor

Each ring slot is a 64-byte descriptor:

```c
struct flipc2_desc {
    uint32_t    opcode;         /* application-defined operation type     */
    uint32_t    flags;          /* FLIPC2_FLAG_* (data location, batch)   */
    uint64_t    cookie;         /* request/reply correlation ID           */
    uint64_t    data_offset;    /* byte offset into data region           */
    uint64_t    data_length;    /* payload size in bytes                  */
    uint64_t    param[3];       /* opcode-specific parameters             */
    uint32_t    status;         /* completion status (reply descriptors)  */
    uint32_t    _reserved;      /* padding to 64 bytes                   */
};
```

The descriptor carries **metadata only**. Bulk data lives in the data region (or
a buffer group), referenced by `data_offset` and `data_length`.

Data location is indicated by `flags`:

| Flag | Value | Meaning |
|------|-------|---------|
| `FLIPC2_FLAG_DATA_INLINE` | 0x0 | Data in channel's inline data region |
| `FLIPC2_FLAG_DATA_MAPPED` | 0x1 | Data in page-mapped zero-copy region (reserved) |
| `FLIPC2_FLAG_BATCH` | 0x2 | More descriptors follow (batch hint) |
| `FLIPC2_FLAG_DATA_BUFGROUP` | 0x4 | Data in a shared buffer group |

### 2.3 Lock-Free Protocol

The ring uses monotonically increasing 32-bit counters (`prod_tail`, `cons_head`)
that wrap naturally. No full/empty ambiguity — the ring is:

- **Empty** when `prod_tail == cons_head`
- **Full** when `prod_tail - cons_head == ring_entries`

The protocol requires only compiler barriers on x86 (hardware guarantees store
and load ordering). On other architectures, full memory fences are used.

```
PRODUCER                                CONSUMER
    │                                       │
    │ d = produce_reserve()                 │
    │ fill d->opcode, data_offset, ...      │
    │ WRITE_FENCE()                         │
    │ produce_commit()  ──────────────────► peek()  → sees new descriptor
    │   └─ advance prod_tail                │   └─ read data via data_offset
    │   └─ if cons_sleeping:                │   consume_release()
    │        semaphore_signal()             │     └─ advance cons_head
    │                                       │
```

### 2.4 Adaptive Wakeup

The consumer uses a three-phase wait strategy:

1. **Spin**: poll `prod_tail` for N iterations (default 4096), using `PAUSE`
   instruction to save power and avoid pipeline flushes.
2. **Set sleeping flag**: write `cons_sleeping = 1`, issue a write fence, then
   re-check the ring. This avoids the lost-wakeup race: the producer checks
   `cons_sleeping` *after* advancing `prod_tail`.
3. **Block**: `semaphore_wait()` — single kernel trap. The producer will
   `semaphore_signal()` on the next produce_commit.

The same protocol works symmetrically for **producer backpressure** (producer
blocks when the ring is full, consumer signals on release).

### 2.5 Corruption Detection and DoS Defense

- **Ring corruption**: if `prod_tail - cons_head > ring_entries`, the ring is
  declared corrupt. All operations return NULL/error.
- **Spurious wakeup limit**: if the consumer wakes from `semaphore_wait` but no
  data is available 64 consecutive times, the channel is declared dead (protects
  against semaphore flooding DoS).

### 2.6 Endpoint Registry

For named channel discovery, FLIPC v2 provides an **endpoint** abstraction built
on top of raw channels. A server creates a named endpoint, registers it with the
Mach name server, and accepts client connections. Each connection creates a pair
of channels (forward + reverse).

```
┌──────────┐   netname_look_up("disk_io")   ┌──────────────┐
│  Client   │ ─────────────────────────────► │  Server      │
│           │                                │  (endpoint)  │
│           │ ◄── MIG RPC: connect ────────► │              │
│           │     vm_remap channels           │              │
│           │                                │              │
│  fwd_ch ──│────── shared memory ───────────│── fwd_ch     │
│  rev_ch ──│────── shared memory ───────────│── rev_ch     │
└──────────┘                                └──────────────┘
```

The endpoint handles:
- Client connection/disconnection via MIG RPC
- Dead-name notifications (client crash cleanup)
- Up to 64 simultaneous connections per endpoint

### 2.7 Buffer Groups

A **buffer group** is a shared memory pool that multiple channels can reference.
Instead of each channel having its own data region, channels share a common pool
of fixed-size buffer slots.

```
┌──────────────────────────────────────────────┐
│  Buffer Group (shared memory)                │
│  ┌────────┬─────────┬──────────────────────┐ │
│  │ Header │ next[]  │  Data Slots          │ │
│  │ (64 B) │ free    │  [slot0][slot1]...   │ │
│  │        │ list    │                      │ │
│  └────────┴─────────┴──────────────────────┘ │
└──────────────────────────────────────────────┘
       ▲              ▲              ▲
       │              │              │
   Channel A      Channel B      Channel C
```

**Allocation** is lock-free: a CAS-based LIFO free list with 64-bit tagged
pointers (ABA-safe, uses `cmpxchg8b` on i686). Alloc/free costs ~23 ns.

Use buffer groups when:
- Multiple channels transfer similar-sized data (avoid per-channel data region waste)
- You need dynamic allocation (channels' inline regions use bump allocation)
- You want to decouple buffer lifetime from channel lifetime

### 2.8 Page-Isolated Layout

The **flat layout** (default) packs all channel data contiguously: header, ring,
and inline data share the same virtual memory pages. This is fine for mutually
trusted tasks, but a misbehaving peer could overwrite fields it shouldn't touch.

The **isolated layout** (`FLIPC2_CREATE_ISOLATED` flag) splits the channel into
page-aligned sections so that `vm_protect` can enforce per-role access:

```
Flat layout (default):                 Isolated layout:
┌──────────────────────────────┐       ┌──────────────────────────────┐
│  Header + Ring + Data        │       │  Page 0: Constants (4 KB)    │ RO both
│  (contiguous, RW both)       │       │  magic, version, offsets     │
└──────────────────────────────┘       ├──────────────────────────────┤
                                       │  Page 1: Producer (4 KB)     │ RW producer
                                       │  prod_tail, prod_sleeping,   │ RO consumer
                                       │  prod_total, wakeups         │
                                       ├──────────────────────────────┤
                                       │  Page 2: Consumer (4 KB)     │ RO producer
                                       │  cons_head, cons_sleeping,   │ RW consumer
                                       │  cons_total                  │
                                       ├──────────────────────────────┤
                                       │  Page 3+: Ring + Data        │ RW producer
                                       │  descriptor ring + inline    │ RO consumer
                                       │  data region                 │
                                       └──────────────────────────────┘
```

**Key properties:**
- **Zero fast-path overhead**: the handle stores direct pointers to volatile
  fields (`ch->prod_tail`, `ch->cons_head`), so inline functions work identically
  for both flat and isolated layouts — no extra indirection.
- **Opt-in**: pass `FLIPC2_CREATE_ISOLATED` to `flipc2_channel_create_ex()`.
  Old code using `flipc2_channel_create()` continues to create flat channels.
- **Minimum size**: 16 KB (3 metadata pages + 1+ ring/data page). The flat
  layout minimum remains 4 KB.
- **Auto-detected on attach**: `flipc2_channel_attach` reads `layout_flags`
  from the header and sets up pointers accordingly.

**Sharing with protections:**

Use `flipc2_channel_share_isolated()` instead of `flipc2_channel_share()`:

```c
/* Share fwd channel: child is the consumer */
flipc2_channel_share_isolated(fwd_ch, child_task,
                               FLIPC2_ROLE_CONSUMER, &child_addr);

/* Share rev channel: child is the producer */
flipc2_channel_share_isolated(rev_ch, child_task,
                               FLIPC2_ROLE_PRODUCER, &child_addr);
```

This performs one `vm_remap` for the full region, then four `vm_protect` calls
to enforce per-section access based on the role.

**Endpoint integration**: pass `FLIPC2_CREATE_ISOLATED` as flags to
`flipc2_endpoint_create()` and connections will automatically use isolated
channels with per-role protections.

### Decision Matrix

| Communication Pattern | Use | Why |
|----------------------|-----|-----|
| Single RPC (request + reply) | **Mach IPC** | Direct Thread Switch + continuations make single-roundtrip ~30% faster |
| Streaming / pipeline | **FLIPC v2** | 19x–150x faster (no kernel traps) |
| Batching (N ops per frame) | **FLIPC v2** | 60 commands in 0.38 µs vs ~84 µs with Mach |
| Fire-and-forget notifications | **FLIPC v2** | No reply overhead, no port lookup |
| Setup / teardown (rare) | **Mach IPC** | Capability-based, port naming, security checks |
| Untrusted client communication | **Mach IPC** | Fine-grained capabilities, port rights isolation |

### Driver ↔ Server Communication

```
                    Mach IPC                 FLIPC v2
                    (control plane)          (data plane)
                         │                       │
  ┌─────────┐  device_open()  ┌────────────┐     │
  │  Client  │ ──────────────►│   Server   │     │
  │          │  device_close() │            │     │
  └─────────┘                 │            │  cmd channel
                              │            │ ════════════► ┌──────────┐
                              │            │               │  Driver  │
                              │            │ ◄════════════ │          │
                              └────────────┘  data channel └──────────┘
```

**Data path → FLIPC v2 always**, not just for high throughput:

The driver↔server data path is a **two-channel pipeline** (command + completion),
not a true RPC. Even a single 4 KB produce+consume costs 86 ns on FLIPC v2
vs 1.6 µs on Mach IPC. There is no reason to use Mach IPC for data transfer
between trusted servers, regardless of frequency.

**Control path → Mach IPC**:

`device_open`, `device_close`, `device_set_status` — rare operations that benefit
from Mach's capability checking and port-based security model.

### Performance Summary

| Scenario | Mach IPC | FLIPC v2 | Winner |
|----------|----------|----------|--------|
| Intra-task RPC (null) | 1.38 µs | 1.98 µs | Mach (+30%) |
| Inter-task RPC (null) | 1.80 µs | 2.59 µs | Mach (+30%) |
| Unidirectional 128B | ~1.50 µs | 12 ns | **FLIPC (125x)** |
| Unidirectional 4 KB | ~1.61 µs | 86 ns | **FLIPC (19x)** |
| Batch 16 descriptors | N/A | 6 ns/desc | **FLIPC** |
| Batch 64 descriptors | N/A | 5 ns/desc | **FLIPC** |
| Buffer group alloc+free | N/A | 23 ns | **FLIPC** |

---

## 4. API Reference

### 4.1 Header

```c
#include <flipc2.h>
```

Link with `libflipc2`. Fast-path functions are `static inline` in the header —
no library call overhead on the hot path.

### 4.2 Return Codes

All API functions return `flipc2_return_t` (typedef for `int`):

| Code | Value | Meaning |
|------|-------|---------|
| `FLIPC2_SUCCESS` | 0 | Operation succeeded |
| `FLIPC2_ERR_INVALID_ARGUMENT` | -1 | NULL pointer or out-of-range parameter |
| `FLIPC2_ERR_RESOURCE_SHORTAGE` | -2 | `vm_allocate` or `semaphore_create` failed |
| `FLIPC2_ERR_CHANNEL_FULL` | -3 | Ring is full |
| `FLIPC2_ERR_CHANNEL_EMPTY` | -4 | Ring is empty |
| `FLIPC2_ERR_NOT_CONNECTED` | -5 | Invalid header (magic/version mismatch) |
| `FLIPC2_ERR_ALREADY_CONNECTED` | -6 | Already connected |
| `FLIPC2_ERR_KERNEL` | -7 | Mach kernel call failed |
| `FLIPC2_ERR_DATA_REGION_FULL` | -8 | Data region exhausted |
| `FLIPC2_ERR_SIZE_ALIGNMENT` | -9 | Channel too small for header + ring |
| `FLIPC2_ERR_MAP_FAILED` | -10 | `vm_remap` failed |
| `FLIPC2_ERR_RING_CORRUPT` | -11 | Ring pointers inconsistent |
| `FLIPC2_ERR_NAME_EXISTS` | -12 | Endpoint name already registered |
| `FLIPC2_ERR_NAME_NOT_FOUND` | -13 | Endpoint name not found |
| `FLIPC2_ERR_MAX_CLIENTS` | -14 | Endpoint client limit reached |
| `FLIPC2_ERR_PEER_DEAD` | -15 | Peer task has died |
| `FLIPC2_ERR_POOL_EMPTY` | -16 | Buffer group pool exhausted |

### 4.3 Constants

```c
/* Channel sizing */
#define FLIPC2_CHANNEL_SIZE_MIN      4096          /* 4 KB  */
#define FLIPC2_CHANNEL_SIZE_MAX      (16*1024*1024) /* 16 MB */
#define FLIPC2_CHANNEL_SIZE_DEFAULT  (256*1024)     /* 256 KB */

#define FLIPC2_RING_ENTRIES_MIN      16
#define FLIPC2_RING_ENTRIES_MAX      16384
#define FLIPC2_RING_ENTRIES_DEFAULT  256

/* Structure sizes */
#define FLIPC2_HEADER_SIZE           256    /* channel header, 4 cache lines */
#define FLIPC2_DESC_SIZE             64     /* descriptor, 1 cache line */

/* Wakeup tuning */
#define FLIPC2_SPIN_DEFAULT          4096
#define FLIPC2_MAX_SPURIOUS_WAKEUPS  64

/* Buffer group */
#define FLIPC2_BUFGROUP_SLOT_SIZE_MIN   64
#define FLIPC2_BUFGROUP_SLOT_SIZE_MAX   65536
#define FLIPC2_BUFGROUP_POOL_SIZE_MIN   4096
#define FLIPC2_BUFGROUP_POOL_SIZE_MAX   (16*1024*1024)
#define FLIPC2_BUFGROUP_SLOT_NONE       0xFFFFFFFF

/* Isolated layout */
#define FLIPC2_LAYOUT_FLAT              0x00000000
#define FLIPC2_LAYOUT_ISOLATED          0x00000001
#define FLIPC2_CREATE_ISOLATED          0x00000001
#define FLIPC2_ROLE_PRODUCER            1
#define FLIPC2_ROLE_CONSUMER            2
#define FLIPC2_CHANNEL_SIZE_MIN_ISOLATED 16384  /* 16 KB minimum */
```

### 4.4 Channel API

#### Create and Destroy

```c
flipc2_return_t
flipc2_channel_create(uint32_t channel_size,
                      uint32_t ring_entries,
                      flipc2_channel_t *channel,
                      mach_port_t *sem_port);
```

Allocates shared memory, creates two Mach semaphores (consumer wakeup + producer
backpressure), initializes the channel header. `channel_size` must be in
`[4KB, 16MB]`. `ring_entries` must be a power of 2 in `[16, 16384]`.

Returns the channel handle and consumer semaphore port. The creator is the
**producer** of this channel.

```c
flipc2_return_t
flipc2_channel_create_ex(uint32_t channel_size,
                          uint32_t ring_entries,
                          uint32_t flags,
                          flipc2_channel_t *channel,
                          mach_port_t *sem_port);
```

Extended version with flags. Pass `FLIPC2_CREATE_ISOLATED` for page-isolated
layout (minimum 16 KB). Pass 0 for flat layout (same as `flipc2_channel_create`).

```c
flipc2_return_t flipc2_channel_destroy(flipc2_channel_t channel);
```

Zeroes the magic (peers detect a dead channel), destroys semaphores, deallocates
shared memory. Only the creator calls this.

#### Attach (Peer Side)

```c
flipc2_return_t
flipc2_channel_attach(vm_address_t base_addr,
                      flipc2_channel_t *channel);
```

Creates a handle for an existing channel at `base_addr`. Validates the header
(magic, version, ring parameters). Used for intra-task peers or after `vm_remap`.

```c
flipc2_return_t
flipc2_channel_attach_remote(vm_address_t base_addr,
                             uint32_t mapped_size,
                             flipc2_channel_t *channel);
```

Same as `attach`, but records `mapped_size` so that `detach` will `vm_deallocate`
the mapping. Use for inter-task peers.

```c
flipc2_return_t flipc2_channel_detach(flipc2_channel_t channel);
```

Frees the handle. If `mapped_size > 0`, deallocates the `vm_remap`'d region.
Does NOT destroy semaphores. Peers (non-creators) call this.

#### Inter-Task Sharing

```c
flipc2_return_t
flipc2_channel_share(flipc2_channel_t channel,
                     mach_port_t target_task,
                     vm_address_t *out_addr);
```

Maps the channel's shared memory into `target_task` via `vm_remap` with
`VM_INHERIT_SHARE` (true shared mapping, not COW). Returns the address in the
target task. The target then calls `flipc2_channel_attach_remote()`.

```c
flipc2_return_t
flipc2_channel_share_isolated(flipc2_channel_t channel,
                               mach_port_t target_task,
                               uint32_t target_role,
                               vm_address_t *out_addr);
```

Same as `flipc2_channel_share`, but applies per-role page protections via
`vm_protect`. `target_role` is `FLIPC2_ROLE_PRODUCER` or `FLIPC2_ROLE_CONSUMER`.
Only works on channels created with `FLIPC2_CREATE_ISOLATED`.

```c
flipc2_return_t
flipc2_semaphore_share(mach_port_t sem,
                       mach_port_t target_task,
                       mach_port_t target_name);
```

Inserts a send right for semaphore `sem` into `target_task`'s IPC space with
name `target_name`. Uses `mach_port_insert_right(MACH_MSG_TYPE_COPY_SEND)`.

#### Configuration

```c
void flipc2_channel_set_semaphores(flipc2_channel_t ch,
                                   mach_port_t sem,
                                   mach_port_t sem_prod);
```

Overrides the semaphore port names in the handle. Used by inter-task peers after
`attach_remote` to set local IPC-space port names.

```c
void flipc2_channel_set_bufgroup(flipc2_channel_t ch,
                                 flipc2_bufgroup_t bg);
```

Associates a buffer group with this channel. After this, `flipc2_resolve_data()`
dispatches to the buffer group for descriptors with `FLIPC2_FLAG_DATA_BUFGROUP`.

### 4.5 Producer Fast Path (inline)

```c
static inline struct flipc2_desc *
flipc2_produce_reserve(flipc2_channel_t ch);
```
Returns a pointer to the next writable ring slot, or `NULL` if full/corrupt.
Non-blocking. Uses a cached copy of `cons_head` to avoid false sharing.

```c
static inline void
flipc2_produce_commit(flipc2_channel_t ch);
```
Commits one descriptor: advances `prod_tail`, wakes consumer if sleeping.

```c
static inline void
flipc2_produce_commit_n(flipc2_channel_t ch, uint32_t n);
```
Batch commit of N descriptors. One fence, one wakeup check.

```c
static inline struct flipc2_desc *
flipc2_produce_wait(flipc2_channel_t ch, uint32_t spin_count);
```
Blocking reserve: spin-poll, then block on producer backpressure semaphore.
Returns `NULL` only on ring corruption.

### 4.6 Consumer Fast Path (inline)

```c
static inline struct flipc2_desc *
flipc2_consume_peek(flipc2_channel_t ch);
```
Returns a pointer to the next available descriptor, or `NULL` if empty/corrupt.
Non-blocking. Does NOT advance `cons_head`.

```c
static inline void
flipc2_consume_release(flipc2_channel_t ch);
```
Releases one descriptor: advances `cons_head`, wakes producer if sleeping.

```c
static inline void
flipc2_consume_release_n(flipc2_channel_t ch, uint32_t n);
```
Batch release of N descriptors.

```c
static inline struct flipc2_desc *
flipc2_consume_wait(flipc2_channel_t ch, uint32_t spin_count);
```
Blocking wait with adaptive wakeup (spin → semaphore). Returns `NULL` on
corruption or DoS (64 consecutive spurious wakeups).

```c
static inline struct flipc2_desc *
flipc2_consume_timedwait(flipc2_channel_t ch,
                         uint32_t spin_count,
                         uint32_t timeout_ms);
```
Same as `consume_wait` but with a timeout. Returns `NULL` on timeout.

```c
static inline uint32_t
flipc2_available(flipc2_channel_t ch);
```
Returns the number of descriptors available for consumption.

### 4.7 Data Helpers (inline)

```c
static inline void *
flipc2_data_ptr(flipc2_channel_t ch, uint64_t offset);
```
Unchecked pointer to inline data at `offset`. Use on hot paths with validated offsets.

```c
static inline void *
flipc2_data_ptr_safe(flipc2_channel_t ch, uint64_t offset, uint64_t length);
```
Bounds-checked version. Returns `NULL` if `offset + length > data_size` or overflow.

```c
static inline void *
flipc2_resolve_data(flipc2_channel_t ch, uint32_t flags, uint64_t offset);
```
Dispatches based on `flags`: returns buffer group data if `FLIPC2_FLAG_DATA_BUFGROUP`,
otherwise inline data.

### 4.8 Endpoint API

#### Server Side

```c
flipc2_return_t
flipc2_endpoint_create(const char *name,
                       uint32_t max_clients,
                       uint32_t channel_size,
                       uint32_t ring_entries,
                       uint32_t flags,
                       flipc2_endpoint_t *ep);
```
Creates a named endpoint, registers it with the Mach name server. `max_clients`
is the maximum simultaneous connections (1–64). `channel_size` and `ring_entries`
are defaults for new connections. Pass `FLIPC2_CREATE_ISOLATED` in `flags` for
page-isolated channels with per-role protections; pass 0 for flat layout.

```c
flipc2_return_t
flipc2_endpoint_accept(flipc2_endpoint_t ep,
                       flipc2_channel_t *fwd_ch,
                       flipc2_channel_t *rev_ch);
```
Blocks until a client connects. Creates a channel pair (forward: server→client,
reverse: client→server), maps them into the client via `vm_remap`, and returns
the server-side handles. Also handles dead-name notifications and disconnects
transparently.

```c
flipc2_return_t flipc2_endpoint_destroy(flipc2_endpoint_t ep);
```
Destroys all connections, unregisters from name server, frees resources.

```c
mach_port_t flipc2_endpoint_port(flipc2_endpoint_t ep);
```
Returns the endpoint's receive port for custom port-set multiplexing.

#### Client Side

```c
flipc2_return_t
flipc2_endpoint_connect(const char *name,
                        uint32_t channel_size,
                        uint32_t ring_entries,
                        flipc2_channel_t *fwd_ch,
                        flipc2_channel_t *rev_ch);
```
Looks up `name` in the Mach name server, connects to the endpoint. Returns
channel handles (forward: server→client, reverse: client→server). Pass 0 for
`channel_size`/`ring_entries` to use server defaults.

```c
flipc2_return_t
flipc2_endpoint_connect_port(mach_port_t server_port,
                             uint32_t channel_size,
                             uint32_t ring_entries,
                             flipc2_channel_t *fwd_ch,
                             flipc2_channel_t *rev_ch);
```
Same as `endpoint_connect` but takes a send right directly (skip name lookup).

```c
flipc2_return_t
flipc2_endpoint_disconnect(flipc2_channel_t fwd_ch,
                           flipc2_channel_t rev_ch);
```
Deallocates semaphore send rights and detaches both channels.

### 4.9 Buffer Group API

#### Setup

```c
flipc2_return_t
flipc2_bufgroup_create(uint32_t pool_size,
                       uint32_t slot_size,
                       flipc2_bufgroup_t *bg);
```
Creates a shared buffer pool. `pool_size` is the requested total memory (actual
allocation may be larger). `slot_size` is the usable data bytes per slot (stride
is 8-byte aligned). On creation, all slots are chained in a free list.

```c
flipc2_return_t flipc2_bufgroup_destroy(flipc2_bufgroup_t bg);
```
Zeroes magic, deallocates shared memory, frees handle.

```c
flipc2_return_t
flipc2_bufgroup_attach(vm_address_t base_addr,
                       flipc2_bufgroup_t *bg);
```
Creates a handle for an existing buffer group. Validates header.

```c
flipc2_return_t
flipc2_bufgroup_attach_remote(vm_address_t base_addr,
                              uint32_t mapped_size,
                              flipc2_bufgroup_t *bg);
```
Same as `attach`, records `mapped_size` for automatic cleanup on detach.

```c
flipc2_return_t flipc2_bufgroup_detach(flipc2_bufgroup_t bg);
```
Frees handle, deallocates mapping if remote.

```c
flipc2_return_t
flipc2_bufgroup_share(flipc2_bufgroup_t bg,
                      mach_port_t target_task,
                      vm_address_t *out_addr);
```
Maps the buffer group into `target_task` via `vm_remap` with `VM_INHERIT_SHARE`.

#### Fast Path (inline)

```c
static inline uint64_t
flipc2_bufgroup_alloc(flipc2_bufgroup_t bg);
```
Lock-free LIFO pop. Returns a byte offset into the data region, or `(uint64_t)-1`
if the pool is empty. Uses CAS with 64-bit tagged pointers (ABA-safe).

```c
static inline void
flipc2_bufgroup_free(flipc2_bufgroup_t bg, uint64_t offset);
```
Lock-free LIFO push. Returns slot to the free list.

```c
static inline void *
flipc2_bufgroup_data(flipc2_bufgroup_t bg, uint64_t offset);
```
Unchecked pointer to buffer slot data.

```c
static inline void *
flipc2_bufgroup_data_safe(flipc2_bufgroup_t bg,
                          uint64_t offset,
                          uint64_t length);
```
Bounds-checked version. Returns `NULL` if out of range.

---

## 5. Tutorial

### 5.1 Basic Channel — Intra-Task Produce/Consume

The simplest use: one thread produces, another consumes, within the same task.

```c
#include <flipc2.h>
#include <mach.h>
#include <string.h>

#define MY_OP_WRITE  1

void producer(flipc2_channel_t ch) {
    const char *message = "Hello, FLIPC!";

    /* Reserve a ring slot */
    struct flipc2_desc *d = flipc2_produce_reserve(ch);
    if (!d) return;  /* ring full */

    /* Copy payload into inline data region */
    uint64_t offset = 0;  /* simple fixed offset for this example */
    memcpy(flipc2_data_ptr(ch, offset), message, strlen(message));

    /* Fill descriptor */
    d->opcode      = MY_OP_WRITE;
    d->flags       = FLIPC2_FLAG_DATA_INLINE;
    d->data_offset = offset;
    d->data_length = strlen(message);
    d->cookie      = 42;

    /* Publish */
    flipc2_produce_commit(ch);
}

void consumer(flipc2_channel_t ch) {
    /* Wait for a descriptor (spin 4096 iterations, then block) */
    struct flipc2_desc *d = flipc2_consume_wait(ch, FLIPC2_SPIN_DEFAULT);
    if (!d) return;  /* corruption or DoS */

    /* Read data */
    void *data = flipc2_data_ptr(ch, d->data_offset);
    /* ... process d->opcode, data, d->data_length ... */

    /* Release the slot */
    flipc2_consume_release(ch);
}

int main(void) {
    flipc2_channel_t ch;
    mach_port_t sem;

    /* Create a 64 KB channel with 64 ring entries */
    flipc2_return_t rc = flipc2_channel_create(
        64 * 1024,   /* channel_size */
        64,          /* ring_entries */
        &ch,
        &sem
    );
    if (rc != FLIPC2_SUCCESS) return 1;

    /* For intra-task: both producer and consumer use the same handle */
    producer(ch);
    consumer(ch);

    flipc2_channel_destroy(ch);
    return 0;
}
```

### 5.2 Batch Produce/Consume

Batching amortizes the wakeup cost over multiple descriptors.

```c
#define BATCH_SIZE 16

void producer_batch(flipc2_channel_t ch) {
    for (int i = 0; i < BATCH_SIZE; i++) {
        struct flipc2_desc *d = flipc2_produce_reserve(ch);
        if (!d) break;

        d->opcode = MY_OP_DRAW_CMD;
        d->flags  = FLIPC2_FLAG_DATA_INLINE;
        if (i < BATCH_SIZE - 1)
            d->flags |= FLIPC2_FLAG_BATCH; /* hint: more coming */
        d->param[0] = i;  /* draw command index */
    }

    /* Single commit + single wakeup for all 16 descriptors */
    flipc2_produce_commit_n(ch, BATCH_SIZE);
}

void consumer_batch(flipc2_channel_t ch) {
    /* Wait for first descriptor */
    struct flipc2_desc *d = flipc2_consume_wait(ch, FLIPC2_SPIN_DEFAULT);
    if (!d) return;

    /* Process all available descriptors */
    uint32_t count = flipc2_available(ch);
    for (uint32_t i = 0; i < count; i++) {
        d = flipc2_consume_peek(ch);
        if (!d) break;
        /* ... process d ... */
        flipc2_consume_release(ch);
    }
}
```

### 5.3 Inter-Task Channel (Endpoint API)

The **endpoint API** is the recommended way to set up inter-task channels. It
handles everything automatically: name registration, channel creation, `vm_remap`,
semaphore sharing, and dead-name notifications.

```c
/*
 * Server: creates a named endpoint, accepts connections in a loop.
 */
void disk_io_server(void) {
    flipc2_endpoint_t ep;
    flipc2_return_t rc;

    /* Register endpoint "disk_io" with up to 8 clients */
    rc = flipc2_endpoint_create(
        "disk_io",         /* name (registered with Mach name server) */
        8,                 /* max simultaneous clients */
        256 * 1024,        /* default channel size */
        256,               /* default ring entries */
        0,                 /* flags: 0=flat, FLIPC2_CREATE_ISOLATED=isolated */
        &ep
    );
    if (rc != FLIPC2_SUCCESS) return;

    /* Accept loop */
    for (;;) {
        flipc2_channel_t fwd_ch, rev_ch;

        /* Block until a client connects */
        rc = flipc2_endpoint_accept(ep, &fwd_ch, &rev_ch);
        if (rc != FLIPC2_SUCCESS) continue;

        /* fwd_ch: server produces (send commands to client)
         * rev_ch: server consumes (receive requests from client) */

        /* Process requests on rev_ch */
        for (;;) {
            struct flipc2_desc *d = flipc2_consume_wait(rev_ch,
                                                         FLIPC2_SPIN_DEFAULT);
            if (!d) break;  /* channel dead */

            /* Handle request */
            switch (d->opcode) {
            case OP_READ_BLOCK:
                handle_read(fwd_ch, d);
                break;
            case OP_WRITE_BLOCK:
                handle_write(d);
                break;
            }
            flipc2_consume_release(rev_ch);
        }
    }

    flipc2_endpoint_destroy(ep);
}

/*
 * Client: connects to the "disk_io" endpoint by name.
 */
void disk_io_client(void) {
    flipc2_channel_t fwd_ch, rev_ch;

    /* Connect — looks up "disk_io" in the Mach name server,
     * server creates channels and maps them into our task */
    flipc2_return_t rc = flipc2_endpoint_connect(
        "disk_io",         /* endpoint name */
        0,                 /* channel_size: 0 = use server default */
        0,                 /* ring_entries: 0 = use server default */
        &fwd_ch,
        &rev_ch
    );
    if (rc != FLIPC2_SUCCESS) return;

    /* fwd_ch: client consumes (receive from server)
     * rev_ch: client produces (send requests to server) */

    /* Send a read request */
    struct flipc2_desc *d = flipc2_produce_reserve(rev_ch);
    if (d) {
        d->opcode  = OP_READ_BLOCK;
        d->param[0] = 42;  /* block number */
        d->cookie  = 1;
        flipc2_produce_commit(rev_ch);
    }

    /* Wait for response */
    d = flipc2_consume_wait(fwd_ch, FLIPC2_SPIN_DEFAULT);
    if (d) {
        void *block_data = flipc2_data_ptr(fwd_ch, d->data_offset);
        /* ... use block_data ... */
        flipc2_consume_release(fwd_ch);
    }

    /* Disconnect */
    flipc2_endpoint_disconnect(fwd_ch, rev_ch);
}
```

### 5.4 Buffer Groups

Buffer groups let multiple channels share a common memory pool.

```c
/*
 * Server manages a buffer pool shared with a driver.
 * Multiple command channels reference the same pool.
 */
void bufgroup_example(mach_port_t driver_task) {
    flipc2_bufgroup_t bg;
    flipc2_channel_t cmd_ch, reply_ch;
    mach_port_t cmd_sem, reply_sem;

    /* Create a buffer pool: 64 KB total, 256-byte slots */
    flipc2_bufgroup_create(64 * 1024, 256, &bg);

    /* Create channels */
    flipc2_channel_create(64 * 1024, 256, &cmd_ch, &cmd_sem);
    flipc2_channel_create(64 * 1024, 256, &reply_ch, &reply_sem);

    /* Associate buffer group with channel */
    flipc2_channel_set_bufgroup(cmd_ch, bg);

    /* Share everything with driver task */
    vm_address_t bg_addr, cmd_addr, reply_addr;
    flipc2_bufgroup_share(bg, driver_task, &bg_addr);
    flipc2_channel_share(cmd_ch, driver_task, &cmd_addr);
    flipc2_channel_share(reply_ch, driver_task, &reply_addr);
    /* ... also share semaphores ... */

    /* Send a command using buffer group */
    uint64_t slot_offset = flipc2_bufgroup_alloc(bg);
    if (slot_offset == (uint64_t)-1) {
        /* pool empty — handle backpressure */
        return;
    }

    /* Fill buffer slot */
    void *buf = flipc2_bufgroup_data(bg, slot_offset);
    memcpy(buf, my_payload, 200);

    /* Produce descriptor referencing the buffer group slot */
    struct flipc2_desc *d = flipc2_produce_reserve(cmd_ch);
    d->opcode      = OP_DMA_WRITE;
    d->flags       = FLIPC2_FLAG_DATA_BUFGROUP;
    d->data_offset = slot_offset;
    d->data_length = 200;
    flipc2_produce_commit(cmd_ch);

    /* The driver processes the command, reads data from the shared
     * buffer pool, then frees the slot:
     *
     *   void *data = flipc2_bufgroup_data(bg, d->data_offset);
     *   process(data, d->data_length);
     *   flipc2_bufgroup_free(bg, d->data_offset);
     */

    /* ... cleanup ... */
    flipc2_channel_destroy(cmd_ch);
    flipc2_channel_destroy(reply_ch);
    flipc2_bufgroup_destroy(bg);
}
```

### 5.5 Advanced: Manual Inter-Task Setup

The endpoint API (Section 5.3) is the recommended way to establish inter-task
channels. However, if you need fine-grained control (e.g., custom channel sizes
per direction, or integration with an existing protocol), you can use the
low-level sharing primitives directly.

Under the hood, `flipc2_endpoint_accept` performs these steps automatically:

```c
/*
 * Manual setup — what the endpoint API does internally.
 * Use this only if you need control that the endpoint API doesn't provide.
 */
void manual_inter_task_setup(mach_port_t client_task) {
    flipc2_channel_t fwd_ch, rev_ch;
    mach_port_t fwd_sem, rev_sem;

    /* 1. Create channels */
    flipc2_channel_create(256 * 1024, 256, &fwd_ch, &fwd_sem);
    flipc2_channel_create(256 * 1024, 256, &rev_ch, &rev_sem);

    /* 2. Map shared memory into client task (vm_remap) */
    vm_address_t fwd_addr, rev_addr;
    flipc2_channel_share(fwd_ch, client_task, &fwd_addr);
    flipc2_channel_share(rev_ch, client_task, &rev_addr);

    /* 3. Share all 4 semaphore send rights with client */
    flipc2_semaphore_share(fwd_sem, client_task, fwd_sem);
    flipc2_semaphore_share(fwd_ch->hdr->wakeup_sem_prod,
                           client_task, fwd_ch->hdr->wakeup_sem_prod);
    flipc2_semaphore_share(rev_sem, client_task, rev_sem);
    flipc2_semaphore_share(rev_ch->hdr->wakeup_sem_prod,
                           client_task, rev_ch->hdr->wakeup_sem_prod);

    /* 4. Communicate fwd_addr, rev_addr, channel_size to client
     *    (via Mach IPC message, MIG RPC, or inherited memory) */
}

/*
 * Client side: attach to channels created by the server.
 */
void manual_client_attach(vm_address_t fwd_addr, uint32_t size,
                          vm_address_t rev_addr) {
    flipc2_channel_t fwd_ch, rev_ch;

    /* Attach with mapped_size so detach will vm_deallocate */
    flipc2_channel_attach_remote(fwd_addr, size, &fwd_ch);
    flipc2_channel_attach_remote(rev_addr, size, &rev_ch);

    /* Semaphore port names are read from the header automatically.
     * Override with set_semaphores() if the names differ in this
     * task's IPC space. */
}
```

---

## 6. Performance

All measurements: best of 3 runs, KVM (QEMU/KVM on Linux host), single vCPU,
i686 target. Measured with `ipc_bench` on Uros.

### 6.1 FLIPC v2 Throughput (unidirectional, no kernel)

| Test | Latency |
|------|---------|
| null descriptor (single) | 9 ns |
| null descriptor (batch 16) | 6 ns/desc |
| null descriptor (batch 64) | 5 ns/desc |
| 128B produce+consume | 12 ns |
| 1024B produce+consume | 26 ns |
| 4096B produce+consume | 86 ns |

### 6.2 FLIPC v2 RPC (request + reply, two channels)

| Test | Intra-Task | Inter-Task |
|------|-----------|------------|
| null RPC | 1.98 µs | 2.59 µs |
| 128B RPC | 2.64 µs | 3.60 µs |
| 1024B RPC | 1.96 µs | 2.73 µs |
| 4096B RPC | 2.83 µs | 2.96 µs |

### 6.3 Endpoint (named discovery)

| Test | Latency |
|------|---------|
| create + destroy | 10.40 µs |
| null RPC (endpoint) | 2.12 µs |
| 128B RPC (endpoint) | 2.76 µs |

### 6.4 Buffer Group

| Test | Latency |
|------|---------|
| alloc + free (CAS) | 23 ns |
| 256B RPC (intra-task) | 1.70 µs |
| 256B RPC (inter-task) | 2.24 µs |

### 6.5 Page-Isolated Channels

| Test | Flat | Isolated | Delta |
|------|------|----------|-------|
| null RPC (intra-task) | 2.70 µs | 1.54 µs | Isolated 43% faster |
| 128B RPC (intra-task) | 2.85 µs | 1.95 µs | Isolated 32% faster |
| null RPC (inter-task) | 2.89 µs | 2.12 µs | Isolated 27% faster |
| 128B RPC (inter-task) | 3.39 µs | 2.58 µs | Isolated 24% faster |

The isolated layout is actually **faster** than flat — producer and consumer
fields on separate pages eliminate false sharing on cache lines.

### 6.6 Application Scenarios

| Scenario | Latency |
|----------|---------|
| 60 draw commands/frame (batched) | 0.38 µs/frame (6 ns/cmd) |
| Texture 16 KB chunk | 324 ns |
| Audio 4 KB PCM frame | 89 ns |
| Mixed frame (60 draw + tex + audio, intra) | 0.73 µs/frame |
| Mixed frame (62 desc, inter-task) | 4.10 µs/frame |

### 6.7 Comparison with Mach IPC

| Pattern | Mach IPC | FLIPC v2 | Ratio |
|---------|----------|----------|-------|
| Single RPC (intra, null) | 1.38 µs | 1.98 µs | Mach 1.4x faster |
| Single RPC (inter, null) | 1.80 µs | 2.59 µs | Mach 1.4x faster |
| Unidirectional 128B | ~1.50 µs | 12 ns | **FLIPC 125x faster** |
| Unidirectional 4 KB | ~1.61 µs | 86 ns | **FLIPC 19x faster** |
| Batch 16 desc | N/A | 6 ns/desc | **FLIPC only** |
| Batch 64 desc | N/A | 5 ns/desc | **FLIPC only** |
| OOL zero-copy 4 KB | 2.03 µs | 86 ns | **FLIPC 24x faster** |
| OOL zero-copy 64 KB | 8.39 µs | N/A | Mach only |
