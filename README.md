# Uros

Uros is a multiserver operating system based on the OSF variant of Mach (osfmk) from the MkLinux DR3 project. The goal is to build a modern, secure, multiserver OS on top of the osfmk microkernel, originally developed by the Open Software Foundation (OSF) and Carnegie Mellon University (CMU).

**Target architecture:** i386 (32-bit x86)
**Compiler:** GCC 15, `-std=gnu11`
**Build system:** CMake / Ninja

## Current Status (v0.0.1)

The system boots on QEMU/KVM: kernel -> bootstrap server -> default pager -> user-space servers (hello_server, ipc_bench). IPC performance has been optimized to ~1.2 us/RPC round-trip on KVM.

### What boots

```
QEMU multiboot -> mach_kernel -> bootstrap server
  -> default_pager (with swap on hd0b)
  -> hello_server  (Mach4 port to OSF Mach)
  -> ipc_bench     (IPC performance benchmark)
```

### Kernel features implemented

- **Flat memory model** — segment bases = 0, LINEAR_KERNEL_ADDRESS removed
- **SYSENTER/SYSEXIT** — fast system call entry/exit
- **SSE/SSE2** — FXSAVE/FXRSTOR for kernel + userspace
- **XSAVE/XRSTOR** — AVX/AVX-512 state management
- **GCC stack protector** — `-fstack-protector-strong`
- **Modern ext2** — 256-byte inodes, rev 1
- **ELF multi-segment** — supports modern GCC binaries

### IPC optimizations

| Optimization | Description |
|---|---|
| kmsg pool LIFO | O(1) lockless per-CPU message allocation for msg <= 256 bytes |
| IPC continuations | Re-enabled `mach_msg_receive_continue`, avoids register save/restore on wakeup |
| Direct Thread Switch | `thread_run(receiver)` in `ipc_mqueue_deliver`, skips scheduler |
| Port lookup cache | 4-entry per-thread cache mapping port name -> port pointer, validated by generation counter |
| Zero-copy OOL | COW `vm_map_copyin()` from sender map for large OOL data, ~2 us constant 4-64 KB |

### IPC benchmark results (KVM, single core)

| Test | Latency |
|---|---|
| Intra-task null RPC | ~1.2 us |
| Inter-task null RPC | ~1.7 us |
| 4 KB OOL (zero-copy) | ~2.0 us |
| 64 KB OOL (zero-copy) | ~2.0 us |

### Build system

- CMake/Ninja replacing the legacy ODE/Makefiles
- MIG compiler (migcom) ported to modern Flex/Bison
- All MIG stubs generated at build time
- Transitive header dependency tracking via depfiles

### User-space libraries

libmach, libcthreads, libsa_mach, libflipc, libnetname, libmachid, libpthreads, libservice — all build successfully.

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
- [ ] Userspace AHCI (SATA) driver — first userspace driver, needed for real hardware boot
- [ ] Kernel primitives for userspace drivers (DMA allocation, interrupt forwarding, PCI config access)
- [ ] SMP support

### Future
- [ ] x86-64 port (PCID for TLB optimization, `syscall` instruction)
- [ ] Capability-based security system
- [ ] Dual IPC: traditional Mach IPC + high-performance channel
- [ ] Unix personality servers (VFS, networking, process server)
- [ ] Self-hosting

## Project Structure

```
osfmk/
├── src/
│   ├── mach_kernel/           # Microkernel source
│   ├── bootstrap/             # Bootstrap server
│   ├── default_pager/         # Default pager server
│   ├── hello_server/          # Test server (Mach4 port)
│   ├── ipc_bench/             # IPC performance benchmark
│   └── mach_services/
│       └── lib/
│           ├── libmach/       # Core Mach user-space library
│           ├── libcthreads/   # C threads (N:M threading)
│           ├── libsa_mach/    # Standalone Mach library
│           ├── libflipc/      # Fast Local IPC library
│           ├── libpthreads/   # POSIX threads over Mach
│           └── migcom/        # MIG compiler (Flex/Bison)
├── build/                     # Build output directory
│   └── export/osfmk/boot/    # mach_kernel binary
scripts/
├── run-qemu.sh                # QEMU launch script
└── make-disk-image.sh         # Disk image builder
docs/
└── bench/                     # Benchmark results
```

## Origins

Based on the osfmk code from MkLinux DR3, originally developed by the Open Software Foundation (OSF) and Carnegie Mellon University (CMU).

## License

The original osfmk code retains its OSF/CMU licenses as stated in each source file.
