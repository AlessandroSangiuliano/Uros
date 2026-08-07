# Uros — System Design

## 1. Overview

Uros is a multiserver operating system based on OSF Mach (from MkLinux DR3).
The kernel provides IPC, virtual memory, scheduling, and minimal hardware
abstraction. Everything else — filesystems, drivers, networking, POSIX
personality — lives in userspace as Mach tasks communicating via IPC.

**Architecture class**: microkernel, multiserver, capability-secured.

**Target**: i386 (32-bit) today, x86-64 planned.

```
┌─────────────────────────────────────────────────────────┐
│  Applications (POSIX personality via libvfs + libposix)  │
├──────────┬───────────┬───────────┬──────────────────────┤
│ ext2     │ net       │ block     │ other fs/driver      │
│ server   │ server    │ device    │ servers              │
│          │ (future)  │ server    │                      │
├──────────┴───────────┴─────┬─────┴──────────────────────┤
│  HAL server (discovery,    │  cap_server (capability     │
│  resource policy, hotplug) │  authority, HMAC tokens)    │
├────────────────────────────┴────────────────────────────┤
│  name_server  │  default_pager  │  bootstrap            │
├─────────────────────────────────────────────────────────┤
│  OSF Mach kernel                                        │
│  (IPC, VM, scheduling, pmap, device_master primitives)  │
└─────────────────────────────────────────────────────────┘
```

### 1.1 Design principles

1. **Kernel is minimal** — IPC, VM, threads, scheduling, physical resource
   primitives (PCI config, IRQ forwarding, DMA alloc). No policy.
2. **Userspace does everything else** — drivers, filesystems, naming,
   paging, security policy.
3. **Two IPC planes** — Mach IPC for control/RPC (secure, capability-based),
   FLIPC v2 for data hot path (shared memory, lock-free, zero kernel trap).
4. **Server per device class** — one task per class (block, network, ...),
   not one task per hardware device. Modules loaded at runtime.
5. **Capabilities for security** — unforgeable, HMAC-signed tokens mediate
   all access to system resources.

---

## 2. Kernel primitives for userspace drivers

The kernel exports a small set of RPCs via `device_master.defs` that give
userspace drivers controlled access to hardware. These are the **only**
kernel interfaces that touch hardware on behalf of drivers:

| RPC | Purpose |
|-----|---------|
| `device_pci_config_read/write` | Read/write PCI configuration space (CF8h/CFCh, future ECAM) |
| `device_intr_register/unregister` | Bind IRQ to Mach port — kernel sends notification message on interrupt |
| `device_dma_alloc/free` | Allocate/free physically contiguous wired memory for DMA |
| `device_map` | Map physical ranges (MMIO BARs) into driver address space |

**Security**: all RPCs require `master_device_port`. Only bootstrap and
tasks it explicitly delegates to hold a send right.

**Future x86-64 evolution**:
- PCI config via ECAM (memory-mapped, no port I/O)
- IOMMU (VT-d) for DMA isolation — driver maps IOVA, kernel manages
  IOMMU page tables
- MSI/MSI-X interrupt remapping directly to driver task

---

## 3. IPC architecture

### 3.1 Mach IPC (control plane)

Standard Mach IPC with ports, rights, and messages. Used for:
- RPC (MIG-generated stubs): `device_open`, `device_read`, `device_write`,
  name server lookups, capability requests
- One-time setup operations
- Cross-task notifications (dead name, port-destroyed)

**Optimizations implemented**:
- Per-CPU kmsg pool (#52) — O(1) lockless alloc for msg <= 256 bytes
- IPC continuations (#53) — zero kernel stack consumption while waiting
- Direct Thread Switch (#54) — `thread_run(receiver)` in mqueue deliver
- Per-thread port lookup cache (#55) — 100% hit rate on hot paths
- Zero-copy OOL (#56) — COW from sender map, ~2 us constant 4-64 KB
- SYSENTER/SYSEXIT (#44) — fast path system call entry/exit
- Protected payloads — receiver-side O(1) dispatch, no port lookup

### 3.2 FLIPC v2 (data plane)

Lock-free shared memory channels for high-throughput, low-latency data
transfer between trusted servers. Complements Mach IPC, does not replace it.

**Architecture**: point-to-point SPSC channels. Setup via Mach IPC (kernel
allocates shared region, maps in both tasks, provides semaphore for wakeup).
After setup, producer and consumer communicate without kernel involvement.

**Two planes per channel**:
- **Descriptor ring** (control): 64-byte command descriptors, lock-free
- **Data region**: shared pages, zero-copy (COW or direct mapping)

**Adaptive wakeup**: spin-poll -> Mach semaphore fallback. Fast path never
enters kernel.

**Use cases**: ext2_server <-> block_device_server (I/O commands),
client <-> fs server (bulk read/write), audio/GPU streaming.

**Performance** (KVM benchmark):
- Fire-and-forget: 20-30 ns
- Batching: 60 commands in 0.38 us
- Throughput: 19x-150x over Mach IPC depending on payload

---

## 4. Capability system

### 4.1 Overview

Uros uses unforgeable capability tokens to mediate all access to system
resources. The capability system is enforced by a dedicated `cap_server`
that is part of the Trusted Computing Base (TCB).

### 4.2 cap_server

The `cap_server` is a privileged Mach task that:
- Issues capabilities as HMAC-signed tokens
- Validates capabilities on behalf of servers
- Manages capability delegation, revocation, and attenuation
- Maintains the authority database

**Token format**: compact binary token containing resource identifier,
permission bitmask, expiration, and HMAC-SHA256 signature. The HMAC key
is known only to `cap_server`.

**Fast path** (~50 ns target): for hot-path validation, servers can use
a kernel-assisted syscall that validates the HMAC without a full IPC
round-trip to `cap_server`.

### 4.3 Capability flow

```
1. Client requests capability from cap_server
   (e.g., "give me read access to /dev/hd0a")
2. cap_server checks policy, issues signed token
3. Client presents token to block_device_server in device_open()
4. block_device_server validates token (fast path or cap_server RPC)
5. On success, block_device_server returns a Mach port (send right)
   for subsequent I/O operations
```

**Delegation**: a capability holder can request `cap_server` to derive
a sub-capability with reduced permissions (e.g., read-only from read-write).

**Revocation**: `cap_server` can invalidate tokens. Servers periodically
re-validate long-lived capabilities, or receive revocation notifications.

---

## 5. HAL server (Hardware Abstraction Layer)

### 5.1 Role

The HAL server is a **discovery and resource policy orchestrator**. It:

1. **Discovers hardware** — enumerates PCI bus (and in future, ACPI tables)
2. **Maintains a device registry** — tracks all known devices, their BDF,
   class, vendor/device ID, assigned driver, status
3. **Matches devices to driver servers** — based on PCI class codes
4. **Notifies driver servers** — "load module X for device at BDF Y"
5. **Manages resource policy** — IRQ allocation, DMA domain assignment,
   BAR arbitration, MSI/MSI-X vector distribution

The HAL server **does NOT**:
- Access hardware registers (MMIO, I/O ports)
- Transfer data
- Sit in any data path
- Manage capabilities (that's `cap_server`)

The HAL is a pure control-plane service. After it tells a driver server
which module to load and which device to probe, the driver server accesses
hardware directly via kernel `device_master` primitives.

### 5.2 Architecture

```
┌─────────────────────────────────────────────┐
│                 HAL server                   │
│                                              │
│  ┌─────────────┐  ┌──────────────────────┐  │
│  │ pci_scan.so  │  │ acpi_tables.so       │  │
│  │ (enumerate   │  │ (parse ACPI for IRQ  │  │
│  │  PCI bus)    │  │  routing, topology)  │  │
│  └──────┬───────┘  └──────────┬───────────┘  │
│         │                     │              │
│  ┌──────▼─────────────────────▼───────────┐  │
│  │          Device Registry               │  │
│  │  BDF → { vendor, class, driver_port,   │  │
│  │          irq, bars[], status }          │  │
│  └──────────────────┬─────────────────────┘  │
│                     │                        │
│  ┌──────────────────▼─────────────────────┐  │
│  │       Resource Policy Engine           │  │
│  │  - IRQ allocation & conflict resolve   │  │
│  │  - BAR assignment & arbitration        │  │
│  │  - DMA domain policy (future IOMMU)    │  │
│  │  - MSI/MSI-X vector distribution       │  │
│  └────────────────────────────────────────┘  │
│                                              │
└──────┬────────────────────────────────┬──────┘
       │ Mach IPC                       │ Mach IPC
       ▼                                ▼
  block_device_server              net_server
  (loads ahci.so,                  (loads e1000.so,
   virtio_blk.so, ...)             virtio_net.so, ...)
```

### 5.3 Modules

The HAL server itself is modular. Modules are loaded via `libmodule`
(see section 7):

| Module | Purpose |
|--------|---------|
| `pci_scan.so` | PCI bus enumeration via `device_pci_config_read` |
| `acpi_tables.so` | ACPI table parsing for IRQ routing, NUMA topology, power management |

These modules perform **discovery only** — they read configuration data
(PCI config space, ACPI memory tables) but do not program hardware.

### 5.4 Device registry

The HAL maintains an in-memory device registry — the authoritative list of
all hardware devices in the system. Each entry contains:

```c
struct hal_device {
    unsigned        bus, slot, func;    /* PCI BDF */
    unsigned        vendor_device;      /* PCI vendor:device ID */
    unsigned        class_rev;          /* PCI class code + revision */
    unsigned        irq;                /* assigned IRQ */
    unsigned        bars[6];            /* BAR addresses */
    mach_port_t     driver_port;        /* port of owning driver server */
    const char      *module_name;       /* e.g. "ahci", "virtio_blk" */
    unsigned        status;             /* unbound, probing, active, error */
};
```

**Query interface** (MIG RPC):
- `hal_list_devices()` — return all devices
- `hal_get_device_info(bus, slot, func)` — return info for one device
- `hal_register_driver(class_mask, port)` — driver server registers interest

Useful for debugging, monitoring, and future `devctl` / `lspci` userspace
utilities.

### 5.5 Driver server registration

When a driver server starts, it registers with the HAL:

```
block_device_server → hal_register_driver(class=0x01xx, port)
net_server          → hal_register_driver(class=0x02xx, port)
```

The HAL maintains a **class → driver port** mapping. When a new device is
discovered (boot or hotplug), the HAL looks up the matching driver server
and sends a notification.

### 5.6 Boot flow

```
1. Kernel boots, starts bootstrap server
2. Bootstrap launches: name_server, default_pager, cap_server, hal_server,
   block_device_server, ext2_server, ...
3. hal_server starts:
   a. Loads pci_scan.so module
   b. Enumerates PCI bus via device_pci_config_read
   c. Populates device registry
4. block_device_server starts:
   a. Registers with HAL: "I handle mass storage (class 0x01)"
   b. HAL sends notifications: "AHCI controller at 0:1F.2", "virtio-blk at 0:04.0"
5. block_device_server receives notifications:
   a. Loads ahci.so for the AHCI device
   b. Loads virtio_blk.so for the virtio device
   c. Each module probes hardware directly (MMIO, DMA, IRQ)
   d. Discovers disks, parses MBR, registers partitions via name_server
6. ext2_server connects to block_device_server, mounts root filesystem
7. System is ready
```

### 5.7 Hotplug

The HAL handles dynamic device arrival and removal:

**Device arrival** (e.g., USB storage, external SATA, NVMe hot-add):
```
1. Hardware generates hotplug interrupt
2. Kernel forwards IRQ notification to HAL server
3. HAL re-scans affected bus segment
4. HAL discovers new device, adds to registry
5. HAL looks up class → driver server mapping
6. HAL notifies driver server: "new device at BDF X:Y.Z, class=0x01, vendor=0xABCD"
7. Driver server loads appropriate module (e.g., nvme.so)
8. Module probes device, registers partitions
```

**Device removal**:
```
1. Hardware generates removal interrupt
2. Kernel forwards to HAL
3. HAL notifies driver server: "device at BDF X:Y.Z removed"
4. Driver server:
   a. Quiesces I/O on affected device
   b. Notifies filesystem servers (dead name notification on device port)
   c. Unloads module, frees resources (IRQ, DMA, MMIO mappings)
5. HAL removes device from registry
```

### 5.8 Relationship with other components

| Component | HAL's relationship |
|-----------|--------------------|
| Kernel (`device_master`) | HAL calls `device_pci_config_read/write` for discovery. HAL does **not** call `device_intr_register`, `device_dma_alloc`, or `device_map` — those are for driver servers. |
| `cap_server` | HAL may request capabilities for PCI config access. HAL does not issue or manage capabilities. |
| Driver servers | HAL notifies them of devices to manage. Driver servers access hardware directly. |
| `name_server` | HAL registers itself; driver servers register partitions/devices. |

---

## 6. Driver servers

### 6.1 Architecture: one server per device class

Instead of one task per hardware device, Uros uses one task per **device
class**. A `block_device_server` manages all block devices (AHCI, virtio-blk,
NVMe, USB mass storage). A future `net_server` manages all network interfaces.

Each driver server is a framework that:
1. Receives device notifications from the HAL
2. Loads the appropriate hardware module (`.so`) via `libmodule`
3. Provides a unified interface to clients (MIG `device.defs` for control,
   FLIPC v2 for data)

```
block_device_server (single task)
├── Framework: message loop, MIG dispatch, partition management,
│              readahead cache, MBR parsing, FLIPC v2 data channels
├── ahci.so        → probes AHCI controller, serves SATA disks
├── virtio_blk.so  → probes virtio-blk device, serves virtual disks
├── nvme.so        → (future) NVMe controller
└── usb_storage.so → (future) USB mass storage
```

### 6.2 Module interface

Every block driver module implements this vtable:

```c
struct block_driver_ops {
    const char *name;           /* "ahci", "virtio_blk", "nvme" */

    /* PCI match — framework calls for each discovered device */
    int  (*match)(unsigned vendor_device, unsigned class_rev);

    /* Init HW: receives BDF, master_device, irq_port. Returns priv */
    int  (*probe)(unsigned bus, unsigned slot, unsigned func,
                  mach_port_t master_device, mach_port_t irq_port,
                  void **priv);

    /* Enumerate disks on this controller */
    int  (*get_disks)(void *priv, struct blk_disk_info *info, int max);

    /* I/O */
    int  (*read_sectors)(void *priv, int disk, uint32_t lba,
                         unsigned count, vm_offset_t *buf, unsigned *size);
    int  (*write_sectors)(void *priv, int disk, uint32_t lba,
                          unsigned count, vm_offset_t buf, unsigned size);

    /* IRQ handler */
    void (*irq_handler)(void *priv);

    /* Max transfer size in bytes */
    unsigned (*max_transfer_bytes)(void *priv);

    /* Optional: physical I/O, batch write */
    int  (*read_sectors_phys)(void *priv, int disk, uint32_t lba,
                              unsigned count, vm_offset_t *pa);
    int  (*write_batch)(void *priv, int disk,
                        uint32_t *lbas, unsigned *sizes, unsigned n,
                        vm_offset_t buf);
};
```

### 6.3 Module loading

Modules are currently statically linked. With `libmodule` (section 7), they
will be loaded dynamically at runtime:

**Static (current)**:
```c
static const struct block_driver_ops *modules[] = {
    &ahci_module_ops,
    &virtio_blk_module_ops,
    NULL
};
```

**Dynamic (with libmodule)**:
```c
/* HAL notifies: "AHCI device at 0:1F.2" */
void *mod = module_load("/mach_servers/modules/ahci.so");
struct block_driver_ops *ops = module_symbol(mod, "ahci_module_ops");
/* Framework calls ops->probe(), etc. */
```

### 6.4 Data path

```
Client (read request)
  │
  │  Mach IPC: ds_device_read(port, offset, size)
  ▼
block_device_server
  │  Protected payload → struct blk_partition *part
  │  Bounds check, readahead cache lookup
  │  part->ctrl->ops->read_sectors(priv, disk, lba, count)
  │      │
  │      │  (module accesses hardware directly: MMIO, DMA)
  │      ▼
  │  Hardware (AHCI/virtio/NVMe)
  │
  │  OOL reply (zero-copy COW)
  ▼
Client (receives data)
```

For high-throughput bulk I/O (e.g., ext2_server reading many blocks):
```
ext2_server ←──FLIPC v2 channel──→ block_device_server
              (request ring)           │
              (reply ring)             │ ops->read_sectors()
                                       ▼
                                   Hardware
```

FLIPC v2 eliminates per-request IPC overhead for the data path. Mach IPC
is used only for `device_open`, `device_close`, `device_set_status`.

### 6.5 Fault isolation

A bug in one module (e.g., `ahci.so`) crashes the entire
`block_device_server`. Mitigations:
- **Watchdog**: the HAL or bootstrap server monitors driver servers via
  dead name notifications. If a server dies, it is restarted.
- **Module restart**: in future, modules may be isolated into separate
  threads with controlled cleanup, allowing module-level restart without
  restarting the entire server.
- **Untrusted drivers**: for untrusted third-party drivers, a separate
  task can be used (falling back to the traditional one-task-per-driver
  model) with IOMMU isolation.

---

## 7. libmodule — shared ELF module loader

### 7.1 Purpose

A single shared library for loading `.so` modules at runtime. Used by
any server that needs dynamic module loading: `block_device_server`,
`net_server`, `hal_server`, future servers.

One loader to maintain, test, and evolve.

### 7.2 Interface

```c
/* Load a shared object from the filesystem */
void *module_load(const char *path);

/* Look up a symbol in a loaded module */
void *module_symbol(void *handle, const char *name);

/* Unload a module (cleanup, munmap) */
int module_unload(void *handle);

/* List loaded modules (for debug/monitoring) */
int module_list(struct module_info *out, int max);
```

### 7.3 Implementation

The loader is a minimal userspace ELF parser:
1. Read ELF shared object from filesystem (via ext2_server or boot device)
2. Parse ELF headers: PT_LOAD segments, dynamic section, relocation tables
3. `vm_allocate` pages for text, data, BSS segments
4. Apply relocations (R_386_32, R_386_PC32, R_386_GLOB_DAT, R_386_JMP_SLOT)
5. Resolve symbols against the host server's symbol table
6. Call `.init` / `__attribute__((constructor))` functions if present
7. Return opaque handle

**Dependencies**: modules can depend on symbols exported by the host
server (e.g., `device_dma_alloc` wrappers, logging functions). Modules
should NOT depend on other modules — keep the dependency graph flat.

**Unloading**: call `.fini` / destructors, release `vm_allocate`'d pages,
remove from module list.

### 7.4 Evolution

| Phase | Capability |
|-------|-----------|
| Phase 0 (current) | Static linking, no loader needed |
| Phase 1 | Basic ELF loader: load/symbol/unload, no TLS, no lazy binding |
| Phase 2 | Full dynamic linking: GOT/PLT lazy resolution, TLS support |
| Phase 3 | Signed modules: `cap_server` validates module signature before loading |

---

## 8. Filesystem architecture

### 8.1 Filesystem servers

Each filesystem type is a separate Mach task:
- `ext2_server` — ext2 filesystem
- Future: `fat_server`, `ufs_server`, `tmpfs_server`, ...

Each server implements a common MIG interface (`vfs.defs`) and communicates
with the block device server for I/O.

### 8.2 libvfs — client-side VFS

Instead of a centralized VFS server (which adds an IPC hop), Uros uses
a client-side library `libvfs` that:

1. Provides POSIX API (`open`, `read`, `write`, `stat`, `close`)
2. Maintains a **mount table cache** in-process: path prefix → fs server port
3. Routes operations directly to the correct fs server

```
Application
  │ open("/mnt/ext/file.txt")
  ▼
libvfs (in-process)
  │ lookup prefix "/mnt/ext" → ext2_server port
  │ ext2_open(port, "file.txt") → fd
  ▼
ext2_server (separate task)
  │ MIG RPC / FLIPC v2
  ▼
block_device_server
```

**Mount table**: authoritative table lives in name_server (or dedicated
mount server). libvfs caches locally, invalidates via dead name
notifications when servers die or mounts change.

---

## 9. Boot sequence

```
1.  BIOS/GRUB loads kernel + bootstrap multiboot modules
2.  Kernel initializes: VM, IPC, scheduling, pmap, PCI (minimal)
3.  Kernel starts bootstrap server (first userspace task)
4.  Bootstrap reads /mach_servers/bootstrap.conf from boot device
5.  Bootstrap launches servers in order:
    a. name_server      — Mach name service
    b. default_pager    — VM paging to hd0b (swap partition)
    c. cap_server       — capability authority (future)
    d. hal_server       — hardware discovery
    e. block_device_server — block devices
    f. ext2_server      — root filesystem
    g. (other servers as configured)
6.  hal_server enumerates PCI, notifies block_device_server
7.  block_device_server loads modules, probes devices, registers partitions
8.  ext2_server mounts root partition via block_device_server
9.  System is operational
```

---

## 10. SMP architecture

Landed in v0.2.0. The release build is `NCPUS=64` with `MP_V1_1=1`; one binary
runs 1..64 CPUs (`-c N` boot flag caps bring-up). Validated on 32 logical CPUs
(Intel i9-13900K hybrid P/E) on bare metal. The issue-level narrative lives in
[release_notes_0.2.0.md](release_notes_0.2.0.md); this chapter records the
architecture.

### 10.1 CPU discovery and AP bring-up

- **Discovery**: ACPI MADT is the primary CPU enumeration; the 1994 MP table
  remains as fallback. Hybrid parts report sparse/high APIC IDs — the
  `lapic_to_slot[]` mapping is masked and bounds-checked, never assumed dense.
- **Startup protocol**: modern attempt+retry instead of the MP-spec fixed
  delays. Fast path: INIT + 10 µs + a *single* SIPI + TSC-deadline poll
  (10 ms budget). Only if the AP stays silent does the full spec ceremony run
  (INIT + 10 ms, SIPI ×2 + 200 µs, 2 s margin). The retry is the compatibility
  mechanism — no per-CPU-family quirk tables.
- **Pipelined bring-up**: the BSP kicks all APs back-to-back and overlaps
  their init (mp_desc, LAPIC, FPU/XSAVE, SYSENTER MSRs, HWP). The only paced
  resource is the shared real-mode trampoline: its stack frames are
  *phase-dependent* (a slower AP's `call` return push lands where a faster
  AP keeps its far-return target), so transit must be exclusive. The pacing
  signal is a funnel counter the AP increments immediately after acquiring
  the historical boot-stack lock (`start_lock`) — verified on the assembly
  that no further pushes occur between that acquire and the switch to the
  AP's own stack.
- **Failure containment**: the rendezvous barrier counts APs *actually
  online*, so a dead AP degrades the boot instead of hanging it. Stragglers
  are retried serially; a visibly stuck trampoline lock is reported but never
  INIT-reset (an INIT there would freeze the funnel for everyone behind it).
- **Early-AP discipline**: until an AP has a `current_thread`, fault paths
  that would dereference it are guarded and fail loudly.
- **Scaling endgame** (documented, not built): per-AP trampoline stacks
  selected by LAPIC ID, if serial trampoline transit ever shows up at
  100+ cores.

### 10.2 Per-CPU data model

- **`%gs`-based per-CPU area**: `cpu_number()` and per-CPU state are a
  segment-relative load, not a cr3-derived array walk.
- Per CPU: SYSENTER trampoline (retires the per-switch `IA32_SYSENTER_ESP`
  WRMSR), LAPIC timer (clock is not PIT-funneled), kmsg pool with
  cache-line-padded counters, run state.
- **Dynamic per-CPU allocator** for new subsystems (no static `[NCPUS]`
  arrays for hot data).

### 10.3 Locking architecture

Layered, converted incrementally with measurement at each step (audit in
[lock_audit_smp.md](lock_audit_smp.md)):

```
hw_lock_*        raw xchg/cmpxchg spinlocks          (i386_lock.S)
usimple_lock_*   portable simple lock                (kern/lock.c)
mutex_*          blocking mutex over an interlock    (owner-tracked builds: MUTEX_OWNER_TRACK)
is_lock          reader-writer lock for ipc_space    (read-mostly port-name resolution)
seqlock + radix  capability table                    (lock-free name→entry lookups)
```

- **pmap**: per-pmap locking (the giant lock is gone) plus **split
  page-table locks** — `pmap_enter`/`pmap_remove` on different page tables
  of one pmap run concurrently. Read paths are lockless where profitable.
- **TLB shootdown**: IPI-based, with two hard rules: a CPU never spins on a
  simple_lock with IPIs masked, and the PTE-publish ↔ CPUs-active-read
  ordering on the enter/activate edge carries an explicit fence — proved
  necessary and sufficient under x86-TSO with herd7 (§10.5).
- **Error-path symmetry**: every fast-path bail must release exactly the
  locks/references the success path releases. This class (a bail leaking an
  `act_lock`, a cache honoring a revoked right) produced real bugs and a
  standing audit rule.
- **Userland**: futex wait/wake/requeue + `futex_waitv` multi-wait;
  libpthreads mutexes/condvars ride the futex fast path and stay out of the
  kernel when uncontended.

### 10.4 Scheduler, idle, and power

- **Global run queue, kept by measurement**: at 32-CPU saturation the run
  queue lock accounts for ~0.2% of samples — per-CPU run queues are
  deliberately *not* built. The measured scaling limiter is RPC-pair
  *placement* (pairs scattered across idle CPUs pay cold caches and the wake
  path), which is scheduler-policy work, not lock work.
- **Wakeup order is FIFO** and load-bearing for fairness under RPC storms.
- **Hand-off-on-block** (`-P`): wake the synchronous-RPC partner on the
  blocking CPU — first increment of the placement work.
- **Idle = HLT** with a two-phase Dekker handshake against the wakeup path
  (closes the lost-wakeup window without an IPI per wakeup). PAUSE-spin
  idle is `-S`, kept for A/B only: on modern packages 31 spinners eat the
  shared power budget (worth 4–6.4× on IPC latency on a 13900K).
- **HWP** enabled at bring-up on capable CPUs (one-way until cold power-off);
  `-E` biases EPP, `-Q` skips enabling.
- **Direct thread switch** is `-D` opt-in on i386 SMP: cross-CPU DTS pays a
  cr3 TLB flush. The win returns with x86-64 + PCID.

### 10.5 Memory-model discipline (x86-TSO)

- The only reordering x86 performs is **store→load** (the store buffer), so
  cross-CPU protocols are written against x86-TSO and each protocol
  documents the one fence it needs.
- The canonical shape in the pmap shootdown protocol is the store-buffer
  (SB) litmus pattern: CPU A publishes a PTE then reads the active-CPU set;
  CPU B publishes its activation then reads the PTE. Both sides fence, or
  both can read stale.
- **Formal regression**: `uros/tools/litmus/` holds four herd7 tests (fence
  on both sides / only A / only B / neither) proving the barrier is both
  necessary and sufficient — herd7 enumerates *all* executions, so this is a
  proof, not sampling. `run.sh` fails on any verdict drift. The "neither"
  control exists so the suite is always *able* to fail.

### 10.6 Debug facilities for SMP

- **NMI hard-lockup watchdog** (`-W`): per-CPU perf-counter NMI that dumps a
  wedged CPU's context even with interrupts off.
- **DDB doors**: serial break and PS/2 Ctrl+D (`-K`) enter DDB via an RPC
  re-arm + single-pass IPI that parks the other CPUs (NMI park).
- **Post-mortem practice**: QEMU gdbstub for frozen-state autopsies; TCG
  with `-d int` for full exception cascades (KVM wipes context on reset
  paths); owner-tracked mutexes for deadlock attribution; zone poisoning
  (`-Z`) for use-after-free.
- **Boot-flag rule**: flag globals live in `.data` — argument parsing runs
  before the BSS clear, and a BSS-resident flag silently resets.

---

## 11. x86-64 address-space layout (v0.3.0)

The port's foundational design decision, settled in #405 before the boot (#406)
and pmap (#407) contracts encode it. Both read these constants; deciding them
once, here, is what keeps the two from disagreeing.

### 11.1 Two halves

i386 is a 3G/1G split in a *shared* 32-bit space: user `0 – 0xc0000000`, kernel
`0xc0000000 – 0xffffffff`. x86-64 replaces that with two canonical halves either
side of the non-canonical hole:

```
0x0000000000000000  ┬ user space          (128 TiB)
0x00007fffffffffff  ┘
        ──────────── non-canonical hole ────────────
0xffff800000000000  ┬ kernel space        (128 TiB)
0xffffffffffffffff  ┘
```

The kernel stays mapped into the top half of *every* address space, exactly as
on i386 — so a syscall or trap never reloads `cr3` and never flushes the TLB on
kernel entry. The hole between the halves does for free what a guard region does
by hand: a stray pointer that walks off the end of user space lands in
non-canonical territory and faults.

### 11.2 Kernel-half region map

A **minimal** set of regions — only what a contract genuinely needs — each at a
**fixed, widely-spaced base with an implicit guard gap** (the regions sit 16–32
TiB apart while using far less, so the unmapped span between them is enormous).
We do **not** carry a Linux-style `vmalloc` or loadable-module area: Uros modules
are userspace `.so` files, not kernel objects. The bases below are shown as
4-level absolute values but are defined symbolically as offsets from a single
`KERNEL_HALF_BASE` (see §11.3).

| base | region | contents | protection |
|---|---|---|---|
| `0xffff800000000000` | **direct map** | all physical RAM at a fixed offset, 1 GiB huge pages | NX; global iff KPTI off |
| `0xffffc00000000000` | **kernel heap** | dynamic kernel allocations | NX by default |
| `0xffffe00000000000` | **per-CPU** | `%gs`-based per-CPU areas | kernel-only |
| `0xfffff00000000000` | **CPU entry area** | per-CPU: entry `.text`, IST/trampoline stacks, GDT, TSS | the *only* kernel region mapped in the user table under KPTI |
| `0xfffff80000000000` | **device registers** | MMIO made reachable: local APIC, IOAPIC, HPET, PCI windows | uncached, NX, kernel-only |
| `0xffffffff80000000` | **kernel image** | `.text` / `.rodata` / `.data` / `.bss` | W^X per section |

The kernel image sits in the top 2 GiB because `-mcmodel=kernel` requires it:
only there do RIP-relative references and 32-bit sign-extended immediates reach
the whole image, which keeps the code compact. This is a codegen constraint, not
a preference.

### 11.3 Paging: four levels, five-ready

Four-level paging (PML4 → PDPT → PD → PT), 48-bit canonical, 128 TiB per half.
Universal on every x86-64 part. Five-level (PML5, 57-bit, 128 PiB) needs Ice
Lake-class hardware and buys space we have no use for, so it is deferred — but
not designed out.

The catch that makes "five-ready" a real promise rather than a slogan: the
canonical higher-half base **moves** between four and five levels (from the
bit-47 sign extension at `0xffff800000000000` to the bit-56 extension at
`0xff00000000000000`). So the region bases in §11.2 are not hardcoded hex; they
are offsets from `KERNEL_HALF_BASE`, and adding a paging level is changing that
one anchor plus the walk depth — the whole map relocates consistently, no
region-by-region repaint.

### 11.4 The direct map is the performance

The single largest win of the port lives here. With all of physical memory
mapped at a fixed offset in 1 GiB huge pages:

- reaching any physical page is an addition, not a temporary mapping — the i386
  `HIGHMEM` machinery (#70), the pmap self-map (#333) and the user-PT alias
  (#334) lose their reason to exist and become i386-only or are deleted;
- a handful of TLB entries cover all of RAM instead of one per 4 KiB page, so
  the kernel touching physical memory stops thrashing the TLB.

This is why the roadmap has always said x86-64 is the real performance jump, not
`-march`.

### 11.5 Protection posture

- **NX**: the direct map and every data region are non-executable — an attacker
  who corrupts a physical page cannot then execute it as code.
- **W^X**: the kernel image is mapped per section — `.text` is RX, never
  writable; `.rodata` is read-only; only `.data`/`.bss` are RW.
- **SMEP/SMAP** (CR4): the kernel cannot inadvertently execute or read user
  pages. Orthogonal to the layout but part of the same posture.
- **KASLR** is deferred, deliberately: randomizing a layout that does not boot
  yet is premature. But the image is built position-independent and the region
  bases are symbolic (§11.3), so KASLR is a later drop-in, not a rewrite — the
  same discipline as five-level paging.

### 11.6 Memory model: the one thing that does not change

x86-64 is x86-TSO, exactly as i386 is. Store→load remains the only
reordering the hardware performs, so every cross-CPU protocol written
against §10.5 keeps its reasoning, and the herd7 suite in
`uros/tools/litmus/` keeps its verdicts — the proof that the pmap shootdown
barrier is necessary *and* sufficient (#350) is inherited rather than
redone. The suite runs green against the x86-64 kernel.

This is the only contract in the port that costs nothing, and saying so is
the point: the same code on a weakly-ordered architecture would need
acquire/release where this one needs a single fence per side. Barriers in
the x86-64 tree are therefore documented by **what they order**, never by
which instruction they emit (#410) — the instruction is what changes on the
day a weak model arrives, and the claim is what has to survive it.

### 11.7 KPTI, chosen at runtime per CPU

The shared higher-half design that buys the performance above is exactly the
condition Meltdown (rogue data-cache load) exploits: userspace speculating
against kernel memory that is mapped in its own page table. The mitigation is
**KPTI** — user mode runs on a page table where the kernel half is unmapped save
the CPU entry area — and Uros selects it **at runtime from what the silicon
reports**, so one binary is correct on vulnerable and immune parts alike:

1. CPUID vendor `AuthenticAMD` → immune by construction → KPTI **off**.
2. else if `IA32_ARCH_CAPABILITIES` is present (`CPUID.(7,0):EDX[29]`) and its
   `RDCL_NO` bit (bit 0) is set → the part declares itself not vulnerable to the
   rogue data-cache load → KPTI **off**.
3. otherwise → assume vulnerable → KPTI **on**.

On the project's own hardware this yields KPTI off on the i9-13900K (RDCL_NO
set) and the Ryzen (AMD), on for the Kaby Lake i7 (pre-fix) — and pre-2018 parts
will be in the field for years, so the capability is not optional.

For one binary to do both, the layout is built KPTI-able from the start:

- **The CPU entry area (§11.2) is its own region** precisely so it can be the
  one kernel mapping left in the user page table. It holds the entry `.text`
  that switches `cr3`, the IST and trampoline stacks, the GDT and TSS.
- **Page tables become a pair** when KPTI is on: a kernel PGD and a user PGD (the
  latter maps only user space plus the entry area). Entry and exit swap `cr3`.
- **This path depends on PCID (#412).** Without it, every `cr3` swap on kernel
  entry/exit flushes the TLB and KPTI-on is punishingly slow; with it, kernel
  and user carry distinct ASIDs and the swap flushes nothing. The KPTI-on path
  is therefore gated on #412 for its performance, and #412 is where the two meet.
- **Global pages are conditional**: kernel regions are marked global only when
  KPTI is off. A global kernel page cannot live in a Meltdown-safe user table,
  and the runtime choice resolves the tension by construction.

None of this is transliterated from another kernel: the MSR and CPUID bits are
Intel's architectural contract, and separating the entry area is geometry the
problem forces. The regions, their bases, and the symbolic anchoring are ours.

### 11.8 Revoking a mapping the other processors are using

A processor caches the translations it walks, and nothing tells it when
another processor edits the tables underneath. So changing an entry is two
operations, and only the first is a store: after it, the table says one
thing while every processor that has walked that address still believes
another. Forgetting the second produces no fault and no report — only
unrelated code, later, reading through a translation that should not exist.

The mechanism is one **cross-call**, not a family of special messages
(#438). A message between processors carries a vector and nothing else, so
everything above it is an arrangement in memory that the sender writes and
the receiver reads. The shootdown is the first user of that arrangement and
will not be the last: a scheduler asking a processor to reconsider what it
runs, a debugger asking them all to stop. Those differ in what is asked, not
in how the asking works. The call returns when every processor has
*finished*, because the caller's next act is usually to reuse the frame.

Two departures from the i386 path, both deliberate:

- **Ranges, not everything.** i386 flushes the whole table on every
  shootdown — correct, and the right choice for its first SMP pass. Here an
  address at a time up to a threshold, wholesale beyond it. The threshold is
  a single constant, unmeasured until #431.
- **The global-page trap is closed in advance.** A `cr3` reload does not
  evict a global entry; that is what global means. Since §11.7 marks kernel
  regions global whenever KPTI is off, a whole-table flush written as a
  `cr3` reload would silently stop flushing the kernel's own mappings the
  day #437 lands. The flush therefore toggles `CR4.PGE` when global pages
  are on, which is the architecture's own answer.

The ordering this rests on costs nothing here and §11.6 says why: the
page-table store must reach the other processor before it is asked to
flush, and x86-TSO does not reorder a store with a later store — the
message is itself a store, to the interrupt command register. On a weakly
ordered machine this is exactly where a release barrier goes, and the
requirement is documented at that point in the code rather than the
instruction that currently satisfies it.

What is **not** yet narrowed: the shootdown goes to every online processor
rather than to those actually using the address space in question. Narrowing
it needs a record of which processors have a given `pmap` loaded, which is
the scheduler's to keep (#408, tracked as #439).

### 11.9 The syscall contract, and what it deliberately does not preserve

Every Mach trap and every POSIX call passes through one entry path, so what
that path costs is a floor under the whole system. The contract is therefore
a decision, recorded here and implemented in `x86_64/syscall/` (#411):

```
rax                      call number in, result out
rdi rsi rdx r10 r8 r9    arguments one to six
rcx r11                  taken by the instruction — return address and flags
rbx rbp r12 r13 r14 r15  preserved
everything else          destroyed
```

`r10` rather than `rcx` for the fourth argument because `SYSCALL` takes
`rcx`; the same substitution Linux makes, for the same reason, which costs
nothing and keeps a musl port (#414, #424) a substitution rather than a
rewrite.

**Destroying the argument registers is the choice.** Linux preserves them,
which obliges its entry to save six on the way in and restore six on the way
out on every call — a compatibility contract thirty years old. Uros controls
both sides and carries no such debt, so a syscall behaves exactly as a call
to a C function: the caller assumes it clobbers what a call clobbers.

The cost is narrow and worth stating exactly, because it is a debugging cost
and those are the ones that get discovered late:

- **Unwinding is unaffected.** The registers a backtrace needs — the
  callee-saved set, the stack and instruction pointers — survive *for free*,
  since the C ABI already preserves them and the dispatcher's own compiled
  code does the work. No entry-path stores are involved.
- **Signal delivery is unaffected.** A handler returns to the instruction
  after `SYSCALL`, where the destroyed registers are dead by this contract.
- **What is lost** is reading or altering a call's *arguments* after it has
  begun: strace-shaped tooling, and ptrace-style interception at the exit
  stop. At the entry stop they are still live.

That loss is recoverable, and the recovery is **not an ABI change** —
userspace cannot observe whether the kernel kept a copy of registers it was
entitled to destroy. So the full register image can be saved the day
something wants it, conditionally, off the fast path; the site is marked in
the entry. The reverse is the one-way door, and that asymmetry is the whole
reason this contract was chosen now rather than deferred: preserving first
and stopping later would break every userland wrapper that had come to rely
on it.

**Two things the hardware layout forces, recorded so they are not rearranged
by a later tidy-up.** `SYSCALL` and `SYSRET` take no selectors — they take
one number in `STAR` and derive four by addition, which makes the order of
the descriptor table load-bearing (§11.2's kernel/user split is not free to
be reordered). And `SYSRET` faults *in ring 0, on the user's stack*, if the
return address is non-canonical: CVE-2012-0217, a privilege escalation
rather than a crash, guarded by three instructions and a branch never taken.

**What the entry path does not do, and why that is a saving.** `SYSCALL`
does not consult the task-state segment, so there is no per-switch stack
register to keep current — which is precisely what #348's per-CPU trampoline
existed to avoid on i386, at a measured ~144 cycles. That machinery is not
ported because the cost it removed does not exist here. The kernel stack
comes from the per-CPU block through `%gs`, one load, which is why those
fields sit together at fixed offsets near the front of the block.

**`swapgs` is a duty in two places.** Neither entry mechanism exchanges the
segment base on its own: the syscall path does it as its first instruction,
and the trap path does it only when the saved code segment says ring 3.
Missing it means kernel code reading `%gs:0` follows an address a user
program chose.

### 11.10 The swapgs window

Deciding from the saved code segment is right for every vector whose arrival
the kernel controls, and wrong for the ones whose arrival it does not. There
are two instruction boundaries where the processor is at ring 0 and `%gs`
still belongs to a user program: after `SYSCALL` and before the entry's own
`swapgs`, and after the exit's `swapgs` and before `SYSRET`. A vector
delivered there reads a ring-0 code segment, concludes correctly that it must
not swap, and runs the kernel on a base a user chose (#440).

**Which vectors is not a matter of taste.** Both windows run with interrupts
disabled — `SYSCALL` clears `IF` through `FMASK`, and the way out never sets
it — so every maskable vector is excluded by the flag, and the set that
remains is exactly its complement: `#DB`, `NMI`, `#DF`, `#MC`. Those four
take a separate entry; everything else keeps the cheaper rule.

**They decide from the segment base.** `IA32_GS_BASE` holds what is loaded
now, which is the one fact the window cannot misrepresent. §11.1 gives the
two halves of the address space to two different owners, so the sign bit of
the base is the answer — `WRMSR` refuses a non-canonical base, which makes
"bit 63 set" and "at or above `KERNEL_HALF_BASE`" the same statement. The
cost is one `RDMSR` on four vectors, none of which is a path anything is
optimising.

The exit cannot recompute the decision — by then the base is the kernel's
either way — and cannot write the old one back either, because `swapgs`
moves *both* halves of the pair and restoring one would leave the user's base
where the next entry swaps to it. So the entry carries one bit forward: the
inverse of `swapgs` is `swapgs`.

**Two conditions the check depends on, both decided rather than assumed.**
`CR4.FSGSBASE` is cleared on every processor, so ring 3 cannot write a base
at all; enabling it later — it is a real gain for thread-local storage —
means teaching the entry to find the per-CPU block without `%gs`, from
`RDPID` or the descriptor limit. And any base arriving from outside through
thread state must be refused unless it is in the lower half. Whoever changes
either owns the entry as well.

**`#DB` also needs a stack of its own,** and for the same window. A trap at
ring 0 does not switch stacks, and `SYSCALL` has not switched one yet, so
`%rsp` still holds whatever a user program left in it — a debug exception
delivered there would push its frame at that address, at ring 0, through the
kernel's mapping. That is a write, not a disclosure. The other three vectors
already had interrupt-stack slots for unrelated reasons; this is the fourth.

---

### 11.11 The message ABI at sixty-four bits

**One decision decides the rest: `natural_t` does not widen (#413).** The
i386 header defines it as whatever the register size is, and uses it both for
plain unsigned numbers and for casting between integers and pointers. Those
are the same type there and two types here. Taken literally it would make
`natural_t` sixty-four bits — which is not one typedef but `mach_port_t`,
`mach_msg_size_t`, `mach_msg_type_number_t`, every count, every id and every
sequence number, doubling the message header and every port array in every
message to express numbers that were never near four billion.

So `natural_t` keeps its width and gives up its second job. An address is a
`vm_offset_t` and nothing else:

| type | i386 | x86-64 | why |
|---|---|---|---|
| `natural_t`, `integer_t` | 32 | 32 | numbers the interfaces exchange |
| `mach_port_t` | 32 | 32 | a name in a task's port space, not a pointer |
| `mach_msg_size_t` | 32 | 32 | no message approaches four gigabytes |
| `boolean_t`, `kern_return_t` | 32 | 32 | fields in messages; width is wire format |
| `vm_offset_t`, `vm_address_t`, `vm_size_t` | 32 | **64** | an address, and a distance between two |

**On i386 the last row was the same type as the first, and MI code was
entitled to assume it.** Every place that stored an address in a `natural_t`,
or printed a `vm_offset_t` with `%x`, still compiles and is now wrong. That is
what #415 goes looking for, and it is why the split is written down rather
than discovered.

**The message header does not move.** Six 32-bit fields, twenty-four bytes,
on both targets — the one part of the wire format that a widening pointer
never touches, because nothing in it is a pointer.

#### What the table does not settle: how wide a port name should be

Keeping `natural_t` narrow is not the same as deciding that everything made
of it should stay narrow, and one of them has an argument on the other side.
A name today is **24 bits of index and 8 bits of generation** — and the
generation is worth less than that says. `IE_BITS_GEN_ONE` is `0x04000000`,
which advances the eight-bit field by four, so it takes **64 distinct
values**, and the low two bits of every kernel-allocated name are always
zero. They were reserved for a fast-path tag on port names that is not
implemented: the generator emits the test for it disabled, as
`if (0 /* Should be: !(x & 0x3) XXX */)`.

So the real recycle window is **64 allocate/deallocate cycles at the same
slot**, after which a stale name comes back meaning a different port. That is
a capability question, not a capacity one, and it is the only entry in the
table above where more bits buy something real.

**How many index bits are actually reachable is a measured number, not
`2^24`.** A task's names come from its entry table, and `ipc_table_fill`
grows that table through 511 sizes and stops: **967,168 entries on i386 and
484,608 on x86-64** — the same 14.8 MiB of table, half as many entries
because an entry is twice the size. Twenty bits of index covers the first,
nineteen the second. **Everything above bit 20 is unreachable**, which is
what makes the trade look the way it does.

| split | index limit | recycle window | what it costs |
|---|---|---|---|
| 24 / 8 (today) | 16.7M — table stops at 967k | **64** | — |
| 22 / 10 | 4.2M | **1024** (×16) | the disabled low-bit tag |
| 20 / 12 | 1.05M | 4096 (×64) | that, plus 2 bits from `ie_bits` |
| 16 / 16 | **65,536** | 65,536 | **a real cut**: ~967k ports per task → 65k |

**The second constraint is `ie_bits`, and it is tighter than the name.** The
generation lives there too: 16 bits of user-references, 5 of capability type,
1 for collisions, 8 for generation — and bits 21 and 22 free. So **ten** bits
of generation fit with nothing displaced; twelve would take two bits from the
user-reference count, dropping its ceiling from 65,535 to 16,383.

**Taken: 22 bits of index and 10 of generation, incrementing by one.**
Sixteen times today's window, no bit spent anywhere, and the index given up
was never reachable. What is given up is the low-bit tag, which is disabled
at the generator.

**It is not a compromise, because it is per-target.** The number that bounds
it — the table ceiling — is per-target already, so the split lives beside the
widths in `mach/machine/port_name.h`, exactly as `vm_offset_t` does. Each
target states two things, how many bits are generation and how many of them
are held still, and everything else is derived from those: the name macros in
`mach/port.h`, and in `ipc/ipc_entry.h` the generation mask, its increment,
and **the collision flag, which is placed immediately below the generation
field rather than at a fixed bit** — it is what the generation grows into.
i386 comes out of those formulas bit for bit as it always was
(`0xff000000`, `0x04000000`, `0x00800000`, `0x007fffff`), which was checked by
rebuilding it: 0 of 205 kernel objects changed in `.text`, `.data` or
`.rodata`.

**This does not close the sixty-four-bit question, it de-urgents it.** A
wider name would buy a window of millions rather than a thousand, for eight
bytes on every message header and a restructuring of `ie_bits`. Worth
deciding on its own terms, once there is something to measure it with (#431).

**Widening `natural_t` is the expensive way to buy it.** The header goes from
24 bytes to 48 and every message pays the copy, to widen counts and sizes
that had no reason to grow. **Widening `mach_port_t` alone is the cheap way:**
the header goes to 32, and the message stops growing there — the port
descriptor's padding *shrinks* to one word, because the padding is the space
an address needs and a name does not, so a 16-byte descriptor comes out
either way. That is why the padding is written as a subtraction rather than
as `sizeof(void *)`: the day a name widens, `mach/message.h` is already
right, and the test that measures the type byte already covers it.

**Not decided here, and for a reason that is not timidity.** The generation
does not live only in the name: it is packed into `ie_bits` in
`ipc/ipc_entry.h` (eight bits at `0xff000000`), and `ipc_entry.c` orders its
tree assuming the generation is in the low bits of the name. That is 138 uses
across ten machine-independent files, none of which compiles for x86-64 yet —
so changing it today means untested surgery whose only running configuration
is the one it would break. It belongs with #416/#415, when the tree builds
and the change can be run.

**A cheaper option deserves weighing at the same time:** rebalancing the
split — 20 bits of index and 12 of generation, or 16 and 16 — costs *zero*
bytes and multiplies the recycle window by 16 to 256. It is a mitigation
rather than an answer, but it is free, and "free" changes what the wider name
has to justify.

**A descriptor is an address, a 32-bit count and a 32-bit flags word**, so it
is twelve bytes on i386 and sixteen here. All four descriptor structures must
be exactly that size, for two reasons and only the first is obvious: the
kernel walks the descriptors of a message as an array of the union, so a
short one puts every later step inside the previous descriptor; and it learns
*which* kind it is holding by reading the `type` byte through the generic
member, so that byte must sit at the same offset in all of them.

A port descriptor carries a name where the others carry an address, so it is
padded — by exactly the space an out-of-line descriptor spends on the part of
its address a name does not need. One word on i386, two here, and neither
target is named in the declaration:

```c
mach_msg_size_t pad1[sizeof(void *) / sizeof(mach_msg_size_t)];
```

**The descriptors begin where the body ends**, because that is how the kernel
finds them — `(mach_msg_descriptor_t *) (body + 1)`. They begin with an
address, so the body is aligned like one: four bytes of padding after the
count on x86-64, moving the first descriptor from offset 28 to 32, and
nothing at all on i386 where a count and a pointer are the same width. x86
would have loaded them unaligned without complaining; the next target will
not be so forgiving, and a fault inside a message walk is a hard thing to
read backwards.

**Checked in three places, deliberately.** The sizes and offsets are repeated
as `_Static_assert` in `mach/message.h`, so a change that moves a field fails
the build of every consumer. What no assertion can state is where the `type`
byte lands — a bit-field has no address to take — so that is measured at run
time on every x86-64 boot, by writing through each descriptor and looking at
the bytes, and by telling a port descriptor what it is and asking the walk
what it heard. And the widths are stated a third time in
`mach/machine/machine_types.defs`, where the stub generator can read them:
the header decides how wide a field is when the compiler lays out a message,
the `.defs` decides how wide `migcom` thinks it is when it computes offsets
and message sizes, and disagreement between them is not a build failure but a
server reading a field from the wrong place.

**⚠️ What `migcom` still owes (#416): alignment, not width.** It adds field
widths; C adds widths *and* padding. Widening `vm_address_t` moves
`vm_allocate`'s reply from 40 bytes to 44 in the generator's arithmetic, and
the compiler makes it 48 — the 64-bit field after `kern_return_t` is aligned
to eight and the generator does not know it. Measured on this tree, not
predicted.

**No 32-bit userland on a 64-bit kernel.** A compatibility layer is a project
of its own — a second marshalling path in the kernel, a second descriptor
layout, and a per-task decision on every message — and pretending it might
arrive later costs design decisions everywhere in the meantime. The answer for
this release is no, recorded here so nobody builds half of one by accident.

---

## 12. Future roadmap

### 12.1 x86-64 migration (v0.3.0 theme)

The address-space layout this migration builds on is specified in chapter 11.

- **PCID**: context switches without TLB flush — re-enables direct thread
  switch (the measured i386 loss behind `-D`)
- **SYSCALL** entry path and the 15-GP-register file
- **Higher-half kernel + direct map**
- **ECAM**: memory-mapped PCI Express configuration (no port I/O)
- **IOMMU (VT-d)**: DMA isolation per driver task
- **MSI/MSI-X**: direct interrupt routing to driver tasks
- **Long mode pmap**: 4-level page tables, larger address space

### 12.2 Networking

- `net_server` with modular drivers (e1000.so, virtio_net.so, ...)
- TCP/IP stack as userspace server or library
- Same architecture as block_device_server: HAL notifies, server loads modules

### 12.3 GPU and display

- `gpu_server` for framebuffer, 2D acceleration
- Command buffer submission via FLIPC v2 channels
- Separate from HAL — GPU server is a driver server, not discovery
- fbcons→gpu_server console handoff (#369): userspace console on pure-UEFI
  machines

### 12.4 USB

- `usb_server` managing host controllers (UHCI, OHCI, EHCI, xHCI) (#353)
- Class drivers as modules: storage, HID, audio, ...
- Full hotplug via HAL notifications
- Unlocks input on pure-UEFI machines (with 11.3, ends their
  headless/bench-only perimeter)

### 12.5 SMP follow-ups

Core SMP shipped in v0.2.0 (chapter 10). Remaining threads:

- Wakeup placement beyond the first hand-off increment (#356) — the measured
  lever for the many-core pre-saturation hump
- `ipc_space` write-side scaling (#340), deferred pmap follow-ups (#318, #349)
- IPC profiling (#392) before any IPC redesign (#391)
- Per-AP trampoline stacks if bring-up transit shows at 100+ cores
