# OSFMK Source Architecture Analysis

## Overview

- **1066 .c files** | **1045 .h files** | **151 .s/.S files**
- **61 .defs files** (require migcom)
- **~30MB** source code

---

## Memory Model: Flat Segments (#47)

The kernel uses a **flat memory model**: all GDT segment bases are zero
and limits span the full 4 GB address space.  Kernel/user separation is
done entirely via paging (PDE 768+ = kernel, U/S=0).

| Property | Value |
|----------|-------|
| Segment bases (CS/DS/SS) | 0 |
| Segment limits | 0xFFFFFFFF (4 GB) |
| Kernel virtual range | 0xC0000000 – 0xFFFFFFFF |
| User virtual range | 0x00000000 – 0xBFFFFFFF |
| Physical load address | 0x100000 (1 MB) |
| Virtual load address | 0xC0100000 |

### Implications

- **virtual == linear**: no address translation between segment-relative
  and linear addresses.  The old `LINEAR_KERNEL_ADDRESS` / `kvtolinear()`
  abstraction has been fully removed.
- **SYSENTER/SYSEXIT**: CPU-hardcoded CS.base=0 matches KERNEL_CS exactly,
  so the fast syscall entry needs no trampoline conversion.
- **GDT descriptors** (LDT, TSS, CPU_DATA, IOPB, FPE): base fields
  receive raw kernel virtual addresses — no offset needed.

---

## Components

### 1. mach_kernel/ (20MB) - THE MICROKERNEL

The heart of the system.

| Module | Description |
|--------|-------------|
| `kern/` | Task, thread, scheduler, timer, kalloc, zalloc |
| `vm/` | Virtual memory, page fault, pmap |
| `ipc/` | Message passing, ports, rights |
| `device/` | Device framework, drivers |
| `i386/` | x86 specific (trap, pmap, locore.S, interrupt.S) |
| `ppc/` | PowerPC specific |
| `hp_pa/` | HP PA-RISC specific |
| `ddb/` | Integrated kernel debugger |
| `mach/` | MIG interfaces (mach.defs, mach_port.defs, etc.) |
| `flipc/` | Fast Local IPC |
| `dipc/` | Distributed IPC |
| `xmm/` | External Memory Management |
| `scsi/` | SCSI subsystem |
| `busses/` | Bus drivers |
| `chips/` | Chip drivers |

**Entry point**: `kern/startup.c` → `setup_main()`

**Key subsystems initialization order** (from startup.c):
1. `printf_init()`, `panic_init()`
2. `sched_init()` - Scheduler
3. `vm_mem_bootstrap()`, `vm_mem_init()` - Virtual memory
4. `ipc_bootstrap()`, `ipc_init()` - IPC
5. `machine_init()` - Machine-specific
6. `clock_init()` - Clocks
7. `task_init()`, `thread_init()` - Tasks and threads

---

### 2. mach_services/ (5.2MB) - LIBRARIES AND SERVICES

#### Libraries (lib/):

| Library | Description | Status |
|---------|-------------|--------|
| `migcom` | MIG compiler | ✅ DONE |
| `libmach` | Mach userspace API (mach_msg, vm_allocate, task_create...) | TODO |
| `libcthreads` | C threads library | TODO |
| `libpthreads` | POSIX threads | TODO |
| `libsa_mach` | Standalone Mach (no libc dependency) | TODO |
| `libservice` | Service location | TODO |
| `libnetname` | Network name service client | TODO |
| `libnetmemory` | Network memory client | TODO |
| `libmachid` | Mach ID client | TODO |
| `libxmm` | External memory management | TODO |
| `libflipc` | Fast Local IPC | TODO |
| `librthreads` | Real-time threads | TODO |

#### Commands (cmds/):

| Command | Description |
|---------|-------------|
| `config/` | Kernel configuration tool |
| `mach_perf/` | Performance benchmarks |

#### Servers (servers/):

| Server | Description |
|--------|-------------|
| `netname/` | Network name server |
| `machid/` | Mach ID server |
| `netmemoryserver/` | Network memory server |

---

### 3. bootstrap/ (228KB) - FIRST USERSPACE TASK

- Loaded by kernel as the first process
- Loads other servers (default_pager, file systems)
- Supports formats: a.out, ELF, COFF, SOM

**Depends on**: libmach, libcthreads, libsa_mach, libsa_fs

**Key files**:
- `bootstrap.c` - Main bootstrap logic
- `load.c` - Binary loader
- `elf.c` - ELF loader
- `a_out.c` - a.out loader

---

### 4. default_pager/ (224KB) - PAGING/SWAP

- Manages swap/paging
- External memory manager
- Uses MIG: `default_pager_object.defs`

**Depends on**: libmach, libcthreads, libsa_mach

**Key files**:
- `default_pager.c` - Main pager
- `dp_memory_object.c` - Memory object handling
- `dp_backing_store.c` - Backing store management
- `wiring.c` - Memory wiring

---

### 5. file_systems/ (600KB) - FILESYSTEMS

| FS | Description |
|----|-------------|
| `ext2fs/` | Linux ext2 |
| `bext2fs/` | Big-endian ext2 |
| `minixfs/` | Minix FS |
| `ufs/` | BSD UFS |

**Produces**: `libsa_fs.a` (used by bootstrap)

---

### 6. xkern/ (2.4MB) - X-KERNEL NETWORKING

Modular protocol framework:
- `protocols/` - TCP, UDP, IP, ARP, ICMP, Ethernet...
- `framework/` - Core x-kernel
- `include/` - Headers
- `gen/` - Generators

---

### 7. stand/ (824KB) - STANDALONE BOOT

Boot support without OS (for AT386, HP700)

---

### 8. tgdb/ (236KB) - TARGET GDB

GDB stub for remote kernel debugging

---

### 9. makedefs/ (76KB) - BUILD DEFINITIONS

Legacy ODE build system definitions

---

### 10. osc/ (16KB) - OSC CONFIGURATION

OSC build configuration

---

## MIG Definition Files (.defs)

These files require `migcom` to generate C stubs:

### mach_kernel/mach/ (Core Mach interfaces)
- `mach.defs` - Core Mach calls
- `mach_port.defs` - Port operations
- `mach_host.defs` - Host operations
- `mach_types.defs` - Type definitions
- `memory_object.defs` - Memory object interface
- `notify.defs` - Notifications
- `exc.defs` - Exceptions
- `clock.defs` - Clock interface
- `sync.defs` - Synchronization
- `ledger.defs` - Ledger interface
- `bootstrap.defs` - Bootstrap interface
- `prof.defs` - Profiling

### mach_kernel/mach_debug/
- `mach_debug.defs` - Debug interface

### mach_kernel/device/
- `device.defs` - Device interface
- `device_request.defs` - Device requests
- `device_reply.defs` - Device replies

### Architecture-specific
- `mach/i386/mach_i386.defs` - i386 specific
- `mach/i386/machine_types.defs`
- `mach/ppc/machine_types.defs`

### default_pager/mach/
- `default_pager_object.defs` - Pager interface

### mach_services/
- Various server-specific .defs files

**Total: 61 .defs files**

---

## Build Dependency Graph

```
                    migcom ✅
                       │
                       ▼
    ┌──────────────────┼──────────────────┐
    │                  │                  │
    ▼                  ▼                  ▼
 libmach          mach_kernel        servers/*.defs
    │                  │
    ▼                  ▼
libcthreads      (kernel binary)
libsa_mach
    │
    ▼
┌───┴────┐
│        │
▼        ▼
libsa_fs  default_pager
│              │
▼              │
bootstrap ◄────┘
```

---

## Suggested Build Order

1. **migcom** ✅ DONE
2. **mach_kernel** - The kernel (target: i386)
3. **libmach** + **libcthreads** + **libsa_mach**
4. **libsa_fs** (file_systems)
5. **default_pager**
6. **bootstrap**
7. **servers** (netname, machid...)

---

## Target Architectures

| Target | Directory | Description |
|--------|-----------|-------------|
| **AT386/i386** | `i386/`, `AT386/` | x86 32-bit (our target) |
| **POWERMAC/ppc** | `ppc/`, `POWERMAC/` | PowerPC |
| **HP700/hp_pa** | `hp_pa/`, `HP700/` | HP PA-RISC |

---

## Kernel Configuration

Configuration files in `mach_kernel/conf/`:
- `files` - List of all kernel source files with options
- `config.common` - Common configuration
- `config.debug` - Debug options
- `AT386/` - i386 specific configs
- `POWERMAC/` - PowerPC specific configs

Default configuration: `PRODUCTION`

---

## Key Kernel Entry Points (i386)

1. **Boot**: `i386/start.S` - Entry from bootloader
2. **C entry**: `kern/startup.c:setup_main()`
3. **First thread**: `kern/startup.c:start_kernel_threads()`
4. **User bootstrap**: Loads `bootstrap` task

---

## Memory Layout (i386)

- Kernel loaded at high memory (typically 0xC0000000+)
- Uses paging from the start
- Physical memory managed by `vm/vm_resident.c`
- Virtual memory by `vm/vm_map.c`

---

## IPC System

- Port-based message passing
- Rights: send, receive, send-once
- Kernel objects accessed via ports
- MIG generates stubs for RPC-style calls

---

## Notes for Modern Compilation

### Issues to expect:
1. **K&R C style** - Need `-std=gnu89` or similar
2. **Implicit declarations** - Many functions not declared
3. **32-bit assumptions** - int/long/pointer size assumptions
4. **Inline assembly** - AT&T syntax for i386
5. **Old GCC extensions** - May need adaptation

### Cross-compilation:
For i386 on x86_64 host: need `-m32` and 32-bit libraries

### Emulation:
- QEMU i386 for testing
- Can boot with multiboot-compatible bootloader (GRUB)
