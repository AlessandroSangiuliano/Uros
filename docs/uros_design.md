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

## 10. Future roadmap

### 10.1 x86-64 migration

- **PCID**: Process Context IDs to avoid TLB flush on context switch
- **ECAM**: memory-mapped PCI Express configuration (no port I/O)
- **IOMMU (VT-d)**: DMA isolation per driver task
- **MSI/MSI-X**: direct interrupt routing to driver tasks
- **Long mode pmap**: 4-level page tables, larger address space

### 10.2 Networking

- `net_server` with modular drivers (e1000.so, virtio_net.so, ...)
- TCP/IP stack as userspace server or library
- Same architecture as block_device_server: HAL notifies, server loads modules

### 10.3 GPU and display

- `gpu_server` for framebuffer, 2D acceleration
- Command buffer submission via FLIPC v2 channels
- Separate from HAL — GPU server is a driver server, not discovery

### 10.4 USB

- `usb_server` managing host controllers (UHCI, OHCI, EHCI, xHCI)
- Class drivers as modules: storage, HID, audio, ...
- Full hotplug via HAL notifications

### 10.5 SMP

- Per-CPU scheduling, per-CPU kmsg pools (already implemented)
- Slab allocator with per-CPU magazines (#80)
- Fine-grained kernel locking (IPC, VM subsystems)
