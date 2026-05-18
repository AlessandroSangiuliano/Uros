# Uros

Uros is a multiserver operating system originally based on the OSF variant of Mach (the `osfmk` codebase from the MkLinux DR3 project, by the Open Software Foundation and Carnegie Mellon University). The source tree under `uros/` was historically named `osfmk/` and the kernel inside it has since evolved into UrMach. The goal is to build a modern, secure, multiserver OS on top of the UrMach microkernel.

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

### Requirements

- CMake >= 3.16
- Ninja
- GCC (i686 multilib or cross-compiler; tested with GCC 15)
- Flex, Bison (for the in-tree `migcom`)
- `sfdisk`, `mke2fs`, `debugfs` (for the disk image)
- QEMU (`qemu-system-i386`), optionally with KVM

### Quick start — full system

The shortest path to a booting system on QEMU:

```sh
mkdir -p uros/build && cd uros/build
cmake -G Ninja \
  -DOSFMK_BUILD_KERNEL=ON \
  -DOSFMK_BUILD_BOOTSTRAP=ON \
  -DOSFMK_BUILD_DEFAULT_PAGER=ON \
  -DOSFMK_BUILD_NAME_SERVER=ON \
  -DOSFMK_BUILD_HAL_SERVER=ON \
  -DOSFMK_BUILD_BLOCK_SERVER=ON \
  -DOSFMK_BUILD_AHCI_DRIVER=ON \
  -DOSFMK_BUILD_VIRTIO_BLK=ON \
  -DOSFMK_BUILD_EXT2_SERVER=ON \
  -DOSFMK_BUILD_HELLO_SERVER=ON \
  -DOSFMK_BUILD_IPC_BENCH=ON \
  -DOSFMK_BUILD_PTHREAD_TEST=ON \
  -DOSFMK_BUILD_CAP_SERVER=ON \
  -DOSFMK_BUILD_CAP_TEST=ON \
  -DOSFMK_BUILD_GPU_SERVER=ON \
  -DOSFMK_BUILD_CHAR_SERVER=ON \
  -DOSFMK_BUILD_GPUSTAT=ON \
  ..
ninja
cd ../..
./scripts/run-qemu.sh --fresh-disk --ahci          # graphical
./scripts/run-qemu.sh --fresh-disk --ahci -nographic -serial mon:stdio
```

`run-qemu.sh` rebuilds `disk.img` and `bootstrap.bundle` automatically when `--fresh-disk` or `--bench` is passed.

### CMake configuration options

Every userspace component is opt-in via a CMake flag.  Defaults keep a kernel-only build minimal; pass `-D<flag>=ON` to enable.

| Flag | Default | Component |
|---|---|---|
| `OSFMK_TARGET_AT386` | `ON` | Target arch (currently only AT386 / i386) |
| `OSFMK_BUILD_TOOLS` | `ON` | Host tools (`mig`, `migcom`, ...) |
| `OSFMK_BUILD_KERNEL` | `ON` | Mach microkernel |
| `OSFMK_BUILD_BOOTSTRAP` | `ON` | Bootstrap server |
| `OSFMK_BUILD_DEFAULT_PAGER` | `OFF` | Default pager |
| `OSFMK_BUILD_NAME_SERVER` | `OFF` | `name_server` (netname) |
| `OSFMK_BUILD_HAL_SERVER` | `OFF` | HAL server (PCI discovery + device registry) |
| `OSFMK_BUILD_BLOCK_SERVER` | `OFF` | Modular block device server |
| `OSFMK_BUILD_AHCI_DRIVER` | `OFF` | AHCI/SATA userspace driver module |
| `OSFMK_BUILD_VIRTIO_BLK` | `OFF` | virtio-blk userspace driver module |
| `OSFMK_BUILD_EXT2_SERVER` | `OFF` | ext2 file server |
| `OSFMK_BUILD_HELLO_SERVER` | `OFF` | Test server (Mach4 port) |
| `OSFMK_BUILD_IPC_BENCH` | `OFF` | IPC + FLIPC v2 benchmark suite |
| `OSFMK_BUILD_PTHREAD_TEST` | `OFF` | libpthreads test harness |
| `OSFMK_BUILD_CAP_SERVER` | `OFF` | UrMach capability server + libcap |
| `OSFMK_BUILD_CAP_TEST` | `OFF` | Capability negative-test binary |
| `OSFMK_BUILD_GPU_SERVER` | `OFF` | userspace GPU/display server (#194) |
| `OSFMK_BUILD_GPUSTAT` | `OFF` | Probe for `gpu_query_stats` (#203) |
| `OSFMK_BUILD_CHAR_SERVER` | `OFF` | userspace character device server (#205) |

To start from a clean configuration, just wipe the build tree:

```sh
rm -rf uros/build && mkdir uros/build && cd uros/build
cmake -G Ninja -D... ..
```

### Building everything

After configuration, build the full enabled tree:

```sh
cd uros/build && ninja
```

### Building a single component

Each component has a Ninja target.  Useful when iterating on one server / library.  Always run from `uros/build/`.

**Kernel + boot:**

```sh
ninja mach_kernel              # microkernel ELF (export/uros/boot/mach_kernel)
ninja mach_kernel_ksyms        # ksyms.bin DDB symbol table (#211)
ninja locore                   # boot asm objects
ninja bootstrap                # bootstrap server (PIE)
ninja default_pager            # default pager
ninja mkbundle                 # tool: stage-1 bundle builder
```

**Userspace servers:**

```sh
ninja name_server_bin          # netname server
ninja hal_server               # HAL: PCI discovery + registry
ninja hal_pci_scan_module      # HAL module: pci_scan.so
ninja block_device_server      # block server (loads AHCI/virtio modules)
ninja ahci_module              # block module: ahci.so
ninja virtio_blk_module        # block module: virtio_blk.so
ninja ext_server_bin           # ext2 file server
ninja cap_server_bin           # UrMach capability server
ninja gpu_server               # GPU/display server (#194)
ninja gpu_vga_module           # GPU module: vga.so
ninja char_server              # character device server (#205)
ninja char_uart_module         # char module: uart.so
```

**Test / bench binaries:**

```sh
ninja hello_server_server      # test server (Mach4 port)
ninja ipc_bench_server         # IPC + FLIPC v2 benchmark
ninja pthread_test_server      # libpthreads test harness
ninja cap_test_server          # cap_server negative tests
ninja gpustat_bin              # gpu_query_stats probe (#203)
```

**Libraries** (all `static .a`, output under `build/export/uros/<arch>/user/lib/`):

```sh
ninja libmach libsa_mach libpthreads libcthreads librthreads
ninja libdl libmodload
ninja libflipc libflipc2
ninja libnetname libmachid libblk libservice libxmm libsa_fs
ninja libcap libgpu_console
```

**Host tools:**

```sh
ninja migcom                   # MIG compiler (Flex/Bison)
```

### Disk image and stage-1 bundle

The disk and bundle are built by scripts in `scripts/`, not by CMake.  Both pick up whatever binaries are present in `uros/build/export/.../user/sbin/`, so optional components are silently included only when their flag was enabled.

```sh
./scripts/make-disk-image.sh             # MBR + 3 partitions (ext2/ext2/raw swap)
./scripts/make-bundle.sh                 # multiboot stage-1 bundle for -initrd
./scripts/make-disk-image.sh --bench all # disk seeded for full ipc_bench suite
```

### Running on QEMU

`scripts/run-qemu.sh` wraps the QEMU invocation and (re-)builds the disk/bundle as needed.

```sh
./scripts/run-qemu.sh                                 # graphical (default --ahci)
./scripts/run-qemu.sh -nographic -serial mon:stdio    # headless
./scripts/run-qemu.sh --fresh-disk                    # regenerate disk.img first
./scripts/run-qemu.sh --bench all                     # full bench suite
./scripts/run-qemu.sh --ahci2                         # add a second AHCI disk
./scripts/run-qemu.sh --virtio                        # also expose a virtio-blk
./scripts/run-qemu.sh --sha-ni                        # force TCG + Icelake +sha-ni
./scripts/run-qemu.sh --no-disk                       # boot bundle only
./scripts/run-qemu.sh --no-bundle                     # boot disk only
```

`--fresh-disk` is the safe default after a rebuild or an ungracefully-closed previous run — `disk.img` carries ext2 writeback state from the guest and a half-flushed image can cause spurious stage-2 hangs.

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
uros/
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
│   └── export/uros/boot/     # mach_kernel binary
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
