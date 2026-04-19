# Uros

Uros is a multiserver operating system based on the OSF variant of Mach (osfmk) from the MkLinux DR3 project. The goal is to build a modern, secure, multiserver OS on top of the osfmk microkernel, originally developed by the Open Software Foundation (OSF) and Carnegie Mellon University (CMU).

**Target architecture:** i386 (32-bit x86)
**Compiler:** GCC 15, `-std=gnu11`
**Build system:** CMake / Ninja

## Current Status (v0.0.2)

The system boots on QEMU/KVM with a full multiserver stack: kernel -> bootstrap -> default_pager -> HAL server -> block device server -> ext2 file server, running hello_server and ipc_bench on top. Userspace storage drivers (AHCI and virtio-blk) are consolidated in a single block server with hot-pluggable modules. IPC stays at ~1.2 us/RPC round-trip on KVM; FLIPC v2 reaches sub-microsecond throughput on batched shared-memory channels.

### What boots

```
QEMU multiboot -> mach_kernel -> bootstrap server
  -> default_pager       (swap on hd0b)
  -> name_server         (netname_check_in/look_up)
  -> hal_server          (PCI enumeration, device registry, IRQ routing)
  -> block_device_server (AHCI + virtio-blk as dynamic modules)
  -> ext_server          (ext2 on IDE + AHCI + virtio partitions, multi-mount)
  -> hello_server, ipc_bench, pthread_test
```

### Release highlights since v0.0.1

- **Userspace drivers** — AHCI (NCQ, scatter-gather, 128K PRDT, request merging, async writeback, readahead, zero-copy DMA) and virtio-blk, unified behind a modular block device server (#58-#78, #157)
- **HAL server** (#160) — userspace PCI discovery with module registry, dead-name driver cleanup, async event notifications; BDS subscribes as HAL client
- **Dynamic module loader** — libdl runtime (#159), bootstrap ET_DYN + libs `-fPIC` (#165, #166), BDS linked PIE with libdl self-bootstrap (#167), libmodload shared library (#171), per-class module pool served over MIG (#163)
- **libpthreads** (#82, #108-#146) — replaced libcthreads; mutex types (errorcheck/recursive), timedlock, robust, pshared, POSIX signals, thread naming, condvar clock, stack guard, contention scope, concurrency hint, thread pool
- **FLIPC v2** (#90, #119, #120, #121) — lock-free SPSC channels, endpoint registry, buffer groups, per-channel page isolation; sub-microsecond throughput, 4-150x faster than Mach IPC on batched workloads
- **Kernel** — protected payloads (#81), child-task MIG (#62), DDB continuations (#61), HIGHMEM up to 4GB with kmap/kunmap (#70), mach_print bench trap (#71), netname dead-name notifications (#72)
- **ext2** — page cache (#66), write-back (#71), readahead/merging/zero-copy DMA/block layer (#74-#78), batch device write (#85), dirty inode list (#86), readv chunks (#89), SSE2 memcpy (#91), multi-mount (#92), inode cache (#93), file pool (#94), negative dcache (#96), batch open+read (#97), file-handle reuse (#98), PP dispatch (#99), VFS inode split (#101)
- **Build** — userspace `<mach/*.h>` regenerated from `.defs` at build time (#169), kernel-side MIG user stubs regenerated (#175), `export/powermac/` renamed to `export/include/` for multi-arch (#170)

### Kernel features

- **Flat memory model** — segment bases = 0, LINEAR_KERNEL_ADDRESS removed
- **SYSENTER/SYSEXIT** — fast system call entry/exit
- **SSE/SSE2 + XSAVE/XRSTOR** — FXSAVE/FXRSTOR and AVX/AVX-512 state management
- **GCC stack protector** — `-fstack-protector-strong`
- **HIGHMEM up to 4GB** — 64-slot kmap/kunmap, separate free lists for low/high memory
- **DDB with continuations** — in-kernel debugger aware of `mach_msg_receive_continue`
- **Protected payloads** — 1-word receiver hint in message header, no lookup on hotpath
- **Modern ext2** — 256-byte inodes, rev 1
- **ELF multi-segment + ET_DYN** — modern GCC binaries and PIE executables

### IPC optimizations

| Optimization | Description |
|---|---|
| kmsg pool LIFO | O(1) lockless per-CPU message allocation for msg <= 256 bytes |
| IPC continuations | Re-enabled `mach_msg_receive_continue`, avoids register save/restore on wakeup |
| Direct Thread Switch | `thread_run(receiver)` in `ipc_mqueue_deliver`, skips scheduler |
| Port lookup cache | 4-entry per-thread cache mapping port name -> port pointer, validated by generation counter |
| Zero-copy OOL | COW `vm_map_copyin()` from sender map for large OOL data, ~2 us constant 4-64 KB |
| Protected payloads | Receiver pointer cached in message header, skips ipc_object_translate |

### Performance (KVM, single core)

**Mach IPC**

| Test | Latency |
|---|---|
| Intra-task null RPC | ~1.2 us |
| Inter-task null RPC | ~1.7 us |
| 4 KB OOL (zero-copy) | ~2.0 us |
| 64 KB OOL (zero-copy) | ~2.0 us |

**FLIPC v2** (shared-memory channels)

| Test | Throughput |
|---|---|
| null desc, batch=64 (intra) | 5 ns/op |
| 128B produce+consume | 17 ns/op |
| 4 KB produce+consume | 151 ns/op |
| inter-task batch=64 (vm_remap) | 45 ns/op |
| 60-draw game frame (intra, batched) | 0.34 us/frame |

### Build system

- CMake/Ninja replacing the legacy ODE/Makefiles
- MIG compiler (migcom) ported to modern Flex/Bison
- All MIG stubs (userspace and kernel-side) generated at build time, no checked-in copies
- Userspace libraries compiled `-fPIC`; bootstrap and block_device_server linked PIE
- Transitive header dependency tracking via depfiles
- Multi-arch friendly layout (`export/include/`, ready for x86_64, aarch64, riscv64)

### User-space libraries

libmach, libsa_mach, libpthreads, libdl, libmodload, libflipc, libflipc2, libnetname, libnetmemory, libmachid, libblk, libservice, librthreads, libxmm — all build successfully.

## Building

Requirements: CMake >= 3.16, Ninja, GCC (i686 multilib or cross-compiler), Flex, Bison.

```sh
cd osfmk/build
cmake -DOSFMK_BUILD_KERNEL=ON -DOSFMK_BUILD_BOOTSTRAP=ON .. -G Ninja
ninja
```

## Running on QEMU

```sh
# Create the disk image (needs: sfdisk, mke2fs, debugfs)
./scripts/make-disk-image.sh

# Boot with KVM (graphical)
./scripts/run-qemu.sh

# Boot headless
./scripts/run-qemu.sh -nographic -serial mon:stdio
```

## Roadmap

### Next steps
- [x] Userspace AHCI (SATA) driver (v0.0.2)
- [x] Userspace virtio-blk driver (v0.0.2)
- [x] HAL server with PCI enumeration and driver registry (v0.0.2)
- [x] Dynamic module loader for userspace servers (v0.0.2)
- [ ] SMP support (slab allocator with per-CPU magazines as prerequisite)
- [ ] x86_64 port (PCID, MMCONFIG, IOMMU/VT-d)

### Future
- [ ] Capability-based security system (modern, performant)
- [ ] Unix personality (VFS library client, process server)
- [ ] libvfs client-side VFS with in-process mount table
- [ ] Additional architectures: aarch64, riscv64, optionally aarch32
- [ ] Self-hosting

## Project Structure

```
osfmk/
├── src/
│   ├── mach_kernel/           # Microkernel source
│   ├── bootstrap/             # Bootstrap server (PIE, libdl self-bootstrap)
│   ├── default_pager/         # Default pager
│   ├── name_server/           # netname server
│   ├── hal_server/            # Userspace HAL: PCI enum, device registry
│   ├── block_device_server/   # Block server with AHCI + virtio-blk modules
│   ├── ext_server/            # ext2 file server (multi-mount)
│   ├── hello_server/          # Test server (Mach4 port)
│   ├── ipc_bench/             # IPC + FLIPC v2 benchmark
│   ├── pthread_test/          # pthreads test harness
│   └── mach_services/
│       └── lib/
│           ├── libmach/       # Core Mach user-space library
│           ├── libsa_mach/    # Standalone Mach library
│           ├── libpthreads/   # POSIX threads over Mach
│           ├── libdl/         # Runtime ELF loader (dlopen/dlsym)
│           ├── libmodload/    # Shared module loader
│           ├── libflipc/      # FLIPC v1
│           ├── libflipc2/     # FLIPC v2 (SPSC channels, endpoints, bufgroups)
│           ├── libnetname/    # netname client
│           ├── libblk/        # Block I/O helpers
│           └── migcom/        # MIG compiler (Flex/Bison)
├── export/include/            # Public headers (multi-arch: i386, <arch>/...)
├── build/                     # Build output
│   └── export/osfmk/boot/     # mach_kernel binary
scripts/
├── run-qemu.sh                # QEMU launch (--ahci, --virtio, --bench)
└── make-disk-image.sh         # Disk image builder
docs/
└── bench/                     # Benchmark results
```

## Origins

Based on the osfmk code from MkLinux DR3, originally developed by the Open Software Foundation (OSF) and Carnegie Mellon University (CMU).

## License

The original osfmk code retains its OSF/CMU licenses as stated in each source file.
