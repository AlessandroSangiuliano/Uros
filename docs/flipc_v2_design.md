# FLIPC v2 — Design Document

## 1. Objective

High-performance IPC channel for hot-path communication between trusted
servers/drivers on Uros.  Complementary to Mach IPC (security, capability), not
a replacement.

**Target**: hundreds of nanoseconds per message; zero kernel traps in the fast
path.

**Use cases**:
- ext2_server ↔ ahci_driver (disk I/O commands)
- client ↔ ext2_server (high-throughput read/write)
- game engine ↔ GPU driver (command buffer + texture streaming)
- audio server ↔ client (low-latency buffer)
- any pair of trusted servers with frequent communication

## 2. Architectural principles

1. **Setup via Mach IPC, data via pure shared memory.**
   The kernel allocates the shared region, maps it into both tasks, provides
   a Mach semaphore for the wakeup.  After the setup, sender and receiver
   communicate *without* involving the kernel in the data path.

2. **Dual plane: control + data.**
   - *Control plane*: ring of small descriptors (64 bytes) — commands, acks,
     metadata.  Lock-free, in the shared ring buffer.
   - *Data plane*: pages mapped directly (COW or read-only mapping) —
     disk blocks, textures, audio buffers.  Zero-copy.

3. **Adaptive wakeup.**
   Spin-poll → fallback to the Mach semaphore.  The fast path never enters the kernel.

4. **No kernel Message Engine.**
   Unlike FLIPC v1, the kernel does not take part in the routing of messages.
   The routing is direct: a channel connects exactly two endpoints (point to
   point).

## 3. Fundamental concepts

### 3.1 Channel

A **channel** is the FLIPC v2 primitive.  It connects exactly two tasks:
a *producer* and a *consumer*.  Every channel contains:

- A shared memory region mapped into both tasks
- A **descriptor ring** (control plane)
- An optional **data region** (data plane, shared pages)
- Synchronization state (pointers, wakeup flags)

A task can have several channels open (e.g. ext2_server has one channel to
ahci_driver and N channels to the clients).

### 3.2 Descriptor

A **descriptor** is a 64-byte record in the ring.  It contains:

```c
struct flipc2_desc {
    uint32_t    opcode;         /* type of operation                     */
    uint32_t    flags;          /* opcode-specific flags                 */
    uint64_t    cookie;         /* operation ID (for req/reply correlation)   */
    uint64_t    data_offset;    /* offset in the data region (0 = no data) */
    uint64_t    data_length;    /* data length in bytes                  */
    uint64_t    param[3];       /* opcode-specific parameters            */
    uint32_t    status;         /* completion status (in replies)        */
    uint32_t    _reserved;      /* alignment to 64 bytes                */
};
```

**64 bytes = 1 cache line**.  The ring contains no payload, only metadata.
The bulk data are referenced through `data_offset` + `data_length` in the
data region.

### 3.3 Data Region

Portion of the shared memory (or of separately mapped pages) where the bulk
data reside.  Two modes:

- **Inline**: the data region is part of the channel's shared memory.
  The producer writes data at `data_offset`, the consumer reads it.
  Suited to medium-sized messages (< a few KB).

- **Page-mapped**: the producer registers pages from its own address space
  through a setup call (Mach IPC).  The kernel maps them read-only
  into the consumer.  Suited to large data (textures, disk blocks).
  The descriptor contains the offset in the mapped region.

### 3.4 Adaptive Wakeup

```
PRODUCER                              CONSUMER
   │                                     │
   │  write descriptor in the ring       │  spin-poll on the ring
   │  WRITE_FENCE()                      │  (busy loop, N iterations)
   │  advance tail pointer               │
   │                                     │  ← sees new descriptor
   │                                     │     → processes, zero traps
   │                                     │
   │  ┌─ if sleeping_flag == 1 ──┐       │  ┌─ spin timeout ──────┐
   │  │  semaphore_signal()      │       │  │  set sleeping_flag=1│
   │  │  (only kernel trap)      │       │  │  WRITE_FENCE()      │
   │  └──────────────────────────┘       │  │  check ring again   │
   │                                     │  │  semaphore_wait()   │
   │                                     │  │  clear sleeping_flag│
   │                                     │  └─────────────────────┘
```

- **Fast path** (consumer active/spinning): zero kernel traps.
- **Slow path** (consumer idle): one kernel trap for the wakeup (semaphore_signal).
- The consumer re-checks the ring *after* setting `sleeping_flag`, to
  avoid a race condition (lost wakeup).

## 4. Memory layout of the Channel

The shared memory of a channel has a fixed layout:

```
Offset 0                                           Offset = channel_size
┌──────────────┬──────────────┬─────────────────────┐
│  Channel     │  Descriptor  │     Data Region     │
│  Header      │  Ring        │  (inline buffers)   │
│  (256 byte)  │  (N × 64 B) │                     │
└──────────────┴──────────────┴─────────────────────┘
```

### 4.1 Channel Header (256 bytes)

```c
struct flipc2_channel_header {
    /* --- constants (set at setup, immutable) --- */
    uint32_t    magic;              /* FLIPC2_MAGIC = 0x464C5032 ("FLP2") */
    uint32_t    version;            /* protocol version                   */
    uint32_t    channel_size;       /* total shared memory size           */
    uint32_t    ring_offset;        /* offset of the ring from the start  */
    uint32_t    ring_entries;       /* number of ring slots (power of 2)  */
    uint32_t    data_offset;        /* data region offset                 */
    uint32_t    data_size;          /* data region size                   */
    uint32_t    desc_size;          /* sizeof(flipc2_desc), for extensibility */

    /* --- producer-owned (written by the producer, read by the consumer) --- */
    volatile uint32_t   prod_tail;      /* next slot to write            */
    uint32_t            _pad_prod[15];  /* padding to a cache line       */

    /* --- consumer-owned (written by the consumer, read by the producer) --- */
    volatile uint32_t   cons_head;      /* next slot to read             */
    volatile uint32_t   cons_sleeping;  /* 1 = consumer blocked on sem   */
    uint32_t            _pad_cons[14];  /* padding to a cache line       */

    /* --- Mach semaphore (set at setup) --- */
    mach_port_t         wakeup_sem;     /* semaphore port for wakeup     */
    uint32_t            _pad_sem[15];

    /* --- statistics (best-effort, not synchronized) --- */
    volatile uint64_t   prod_total;     /* descriptors produced          */
    volatile uint64_t   cons_total;     /* descriptors consumed          */
    volatile uint64_t   wakeups;        /* semaphore_signal count        */
    uint8_t             _reserved[232 - 3*8]; /* up to 256 bytes total   */
};
```

`prod_tail` and `cons_head` are on separate cache lines to avoid false
sharing.

### 4.2 Descriptor Ring

Circular array of `ring_entries` descriptors (power of 2).  Index
computed with a mask:

```c
#define FLIPC2_RING_IDX(ptr, hdr)  ((ptr) & ((hdr)->ring_entries - 1))
```

The ring is full when `prod_tail - cons_head == ring_entries`.
The ring is empty when `prod_tail == cons_head`.

The `prod_tail` and `cons_head` pointers increase monotonically (they wrap
at 32 bits).  This removes the full/empty ambiguity of classic ring buffers.

### 4.3 Data Region

Free space after the ring.  Managed by the producer with a simple allocator
(circular bump allocator or free list).  The consumer does not write to the data
region — it only reads it.

For page-mapped data, `data_offset` in the descriptor points to the region of
separately mapped pages (not to the inline data region).  A bit in `flags`
distinguishes the two cases.

## 5. API

### 5.1 Setup (slow path — via Mach IPC)

```c
/* Creates a channel. Returns a handle and the send right to invite the peer. */
flipc2_return_t
flipc2_channel_create(
    uint32_t            channel_size,   /* total size (min 4KB, max 16MB)        */
    uint32_t            ring_entries,   /* slots in the ring (power of 2)        */
    flipc2_channel_t   *channel,        /* [out] local handle                    */
    mach_port_t        *invite_port     /* [out] send right to pass to the peer  */
);

/* Accepts an invitation and connects to the channel (peer). */
flipc2_return_t
flipc2_channel_accept(
    mach_port_t         invite_port,    /* send right received from the creator  */
    flipc2_channel_t   *channel         /* [out] local handle                    */
);

/* Closes the channel and releases the resources. */
flipc2_return_t
flipc2_channel_destroy(
    flipc2_channel_t    channel
);

/* Maps pages of the producer into the consumer for the zero-copy data plane. */
flipc2_return_t
flipc2_channel_map_pages(
    flipc2_channel_t    channel,
    vm_offset_t         source_addr,    /* address in the producer               */
    vm_size_t           size,           /* mapping size                          */
    uint64_t           *region_id       /* [out] region ID for the descriptors   */
);
```

### 5.2 Produce / Consume (fast path — pure user space)

```c
/* Get the next free slot in the ring (NULL if the ring is full). */
static inline struct flipc2_desc *
flipc2_produce_reserve(flipc2_channel_t channel);

/* Publish the descriptor: advance prod_tail, wake the consumer if necessary. */
static inline void
flipc2_produce_commit(flipc2_channel_t channel);

/* Publish N descriptors in a batch. */
static inline void
flipc2_produce_commit_n(flipc2_channel_t channel, uint32_t n);

/* Get the next available descriptor (NULL if the ring is empty). */
static inline struct flipc2_desc *
flipc2_consume_peek(flipc2_channel_t channel);

/* Release the descriptor: advance cons_head. */
static inline void
flipc2_consume_release(flipc2_channel_t channel);

/* Release N descriptors in a batch. */
static inline void
flipc2_consume_release_n(flipc2_channel_t channel, uint32_t n);

/* Wait for a descriptor (spin + semaphore fallback). */
static inline struct flipc2_desc *
flipc2_consume_wait(flipc2_channel_t channel, uint32_t spin_count);

/* Poll: how many descriptors are available? */
static inline uint32_t
flipc2_available(flipc2_channel_t channel);
```

All the fast-path functions are `static inline` in a header, compiled
directly into the code of the server/driver.  No library to link
in the fast path.

### 5.3 Data Region helpers

```c
/* Allocates space in the inline data region. Returns an offset or -1. */
static inline uint64_t
flipc2_data_alloc(flipc2_channel_t channel, uint32_t size);

/* Frees space in the inline data region. */
static inline void
flipc2_data_free(flipc2_channel_t channel, uint64_t offset, uint32_t size);

/* Pointer to the data given an offset. */
static inline void *
flipc2_data_ptr(flipc2_channel_t channel, uint64_t offset);
```

## 6. Fast-path protocol (detail)

### 6.1 Produce (sender)

```c
struct flipc2_desc *d = flipc2_produce_reserve(ch);
if (!d) { /* ring full — backpressure */ }

d->opcode = MY_OP;
d->cookie = request_id;
d->data_offset = flipc2_data_alloc(ch, payload_size);
memcpy(flipc2_data_ptr(ch, d->data_offset), payload, payload_size);
d->data_length = payload_size;

WRITE_FENCE();
flipc2_produce_commit(ch);
```

`flipc2_produce_commit` increments `prod_tail`, then:
```c
WRITE_FENCE();
if (hdr->cons_sleeping) {
    semaphore_signal(hdr->wakeup_sem);
    hdr->wakeups++;
}
```

### 6.2 Consume (receiver)

```c
struct flipc2_desc *d = flipc2_consume_wait(ch, SPIN_DEFAULT);

/* process the descriptor */
void *data = flipc2_data_ptr(ch, d->data_offset);
handle_request(d->opcode, d->cookie, data, d->data_length);

flipc2_data_free(ch, d->data_offset, d->data_length);
flipc2_consume_release(ch);
```

`flipc2_consume_wait` implements the adaptive wakeup:
```c
for (uint32_t i = 0; i < spin_count; i++) {
    if (prod_tail != cons_head) return &ring[cons_head & mask];
    PAUSE();    /* hint to the processor: spin-wait loop */
}
/* spin exhausted — block */
hdr->cons_sleeping = 1;
WRITE_FENCE();
/* re-check to avoid lost wakeup */
if (prod_tail != cons_head) {
    hdr->cons_sleeping = 0;
    return &ring[cons_head & mask];
}
semaphore_wait(hdr->wakeup_sem);
hdr->cons_sleeping = 0;
return &ring[cons_head & mask];
```

### 6.3 Batch produce/consume

For maximum throughput, N descriptors can be reserved/released in
a block, with a single fence and a single wakeup check.  Useful for
GPU command buffers or batches of I/O requests.

## 7. Kernel support required

The kernel provides only setup services (slow path):

### 7.1 System call / MIG RPC

```
flipc2_channel_create(task, size, ring_entries) → (memory_object, semaphore)
```

The kernel:
1. Allocates `size` bytes of contiguous physical memory (wired)
2. Creates a memory object mappable into both tasks
3. Creates a Mach semaphore
4. Initializes the channel header (magic, version, offsets)
5. Returns memory object + semaphore send right

```
flipc2_channel_map_pages(task, source_va, size, dest_task) → region_id
```

The kernel:
1. Validates that `source_va..source_va+size` is valid in the source task
2. Creates a read-only (or COW) mapping in the dest task
3. Returns a `region_id` for the descriptors

### 7.2 What the kernel does NOT do

- Does not touch the descriptors
- Does not route messages
- Has no Message Engine
- Does not intervene in the data path after the setup
- Does not poll or process buffers

## 8. Security

1. **Isolation via mapping**: every channel is mapped only into the two
   participating tasks.  A task cannot access other tasks' channels.

2. **Read-only data region for the consumer**: the page-mapped pages are
   read-only in the consumer — the producer keeps the write.

3. **Validation at setup**: the kernel checks the parameters (size, alignment,
   task permissions) only during creation.  After the setup, the tasks communicate
   directly — errors in the protocol (e.g. offset out of range) are
   the tasks' responsibility.

4. **Trust model**: FLIPC v2 is for *trusted* tasks (system servers).
   It does not have the fine-grained isolation of Mach IPC (capabilities, port rights).
   A compromised server can corrupt the channel.  For untrusted clients,
   traditional Mach IPC is used.

5. **Future with IOMMU (x86-64)**: on hardware with VT-d, the kernel will be able to
   protect the DMA pages too — the driver will not be able to DMA outside the
   authorized region.

## 9. Comparison with FLIPC v1

| Aspect               | FLIPC v1                          | FLIPC v2                        |
|----------------------|-----------------------------------|---------------------------------|
| Topology             | Multi-node (NORMA/KKT)            | Local point-to-point            |
| Message Engine       | In the kernel, does the routing    | None — direct communication     |
| Buffer model         | Fixed-size pool, fixed 1MB         | Ring of 64B descriptors + configurable data region |
| Data transfer        | Buffer ref in the ring             | Descriptor + zero-copy page mapping |
| Channel size         | Fixed 1MB                          | Configurable 4KB – 16MB        |
| Wakeup               | Semaphore (always a kernel trap)   | Adaptive spin + semaphore       |
| Endpoint model       | Separate Send/Receive              | Bidirectional (producer/consumer by role)   |
| Kernel involvement   | Every message (ME)                 | Setup only                      |
| API                  | ~25 functions + macros             | ~12 inline functions            |

## 10. Implementation plan

### Phase 1 — Data structures and headers
- `flipc2.h`: public header with structs, constants, fast-path inlines
- `flipc2_types.h`: types and return codes

### Phase 2 — Kernel support
- New MIG RPCs in `device_master.defs` or a new `flipc2.defs`
- Shared memory allocation, semaphore creation, mapping

### Phase 3 — Userspace library
- `libflipc2/`: channel_create, channel_accept, channel_destroy
- Setup via Mach IPC, shared memory mapping

### Phase 4 — Tests and benchmarks
- `flipc2_test` server: single-descriptor latency, batch throughput,
  comparison with Mach IPC
- Measure: ns per descriptor, MB/s throughput with page mapping

### Phase 5 — Integration
- ext2_server ↔ ahci_driver via FLIPC v2 (first real channel)
- End-to-end I/O benchmark with and without FLIPC v2

### Phase 6 — Page mapping and zero-copy
- flipc2_channel_map_pages
- Integration with the VM system for COW / read-only mapping
- Benchmark with bulk data (4KB, 64KB, 1MB)
