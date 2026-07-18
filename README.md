# Uros

Uros is a multiserver operating system originally based on the OSF variant of Mach (the `osfmk` codebase from the MkLinux DR3 project, by the Open Software Foundation and Carnegie Mellon University). The source tree under `uros/` was historically named `osfmk/` and the kernel inside it has since evolved into **UrMach**. The goal is to build a modern, secure, multiserver OS on top of UrMach.

**Target architecture:** i386 (32-bit x86), SMP up to `NCPUS=64`
**Kernel:** UrMach 0.2.0
**Compiler:** GCC, `-std=gnu11` (tested with GCC 15/16)
**Build system:** CMake / Ninja
**libc:** musl (static + shared, with an Uros syscall dispatcher)

## Current status (v0.2.0)

Uros boots through the full multiserver stack and lands in an interactive shell (`ush`) over the controlling tty, with a working musl-based POSIX runtime: `fork`+`execve`+`waitpid`, job control (`tcsetpgrp`/`tcgetpgrp`, `SIGCONT`/`SIGTSTP`/`SIGINT`), signals via per-thread exception ports, `/proc`, file I/O through libvfs (with a FLIPC v2 read-ahead fast path), and runtime ELF loading through ld-musl + a libc.so umbrella.

As of v0.2.0 the whole stack is **SMP**: one kernel binary boots up to **32 logical CPUs on real hardware** (Intel i9-13900K) with pipelined AP bring-up, a modernized kernel-locking stack, idle-HLT + HWP power management, and an **on-screen console with Ctrl-Alt-Fn virtual terminals**. A combined null RPC on a P-core runs at ~1.1 µs with the full SMP locking stack underneath.

You can type into the QEMU window or the serial console and run static or dynamically-linked musl binaries:

```
ush$ /hello_world
hello
ush$ /hello_dyn_world
hello from libc.so
ush$ /hello_exec &
[bg] pid=4
ush$ /flipc_bench -n 1 -c 64,1024
flipc_bench: runs=1 chunks=2
flipc_bench: done
ush$ shutdown
```

### What boots

```
QEMU / bare metal (BIOS mb1, UEFI mb2 + GOP fbcons) -> UrMach 0.2.0 (SMP) -> bootstrap
  -> default_pager       (swap on disk0c)
  -> name_server         (netname_check_in/look_up + mount registry)
  -> cap_server          (capability authority + libcap)
  -> hal_server          (PCI enumeration, device registry, IRQ routing)
  -> block_device_server (AHCI + virtio-blk as dynamic modules)
  -> gpu_server          (VGA text console, libgpu_console mirror)
  -> char_server         (UART tty, PS/2 keyboard, on-screen console TTY, job control)
  -> virtual_terminal_server (Ctrl-Alt-Fn VTs, lazy per-VT shells)
  -> ext_server          (ext2 multi-mount: '/', '/mnt/disk1')
  -> proc_server         (POSIX process model: fork/exec/wait/signals, /proc)
  -> exec_server         (userspace ELF loader, PT_INTERP + AT_BASE + auxv)
  -> ush                 (Uros shell — interactive prompt over ctty)
```

### v0.2.0 release highlights

v0.2.0 is the SMP release. Full narrative in [docs/release_notes_0.2.0.md](docs/release_notes_0.2.0.md); issue-by-issue list in the [CHANGELOG](CHANGELOG.md).

#### SMP kernel

- **Pipelined AP bring-up** (#367) — ACPI MADT discovery, attempt+retry INIT/SIPI (the 1994 fixed delays are gone), the BSP kicks every AP back-to-back and paces only the shared real-mode trampoline. ~120× faster per-AP kick; a dead AP degrades the boot instead of hanging it.
- **Per-CPU foundations** — `%gs` per-CPU data (#321), per-CPU SYSENTER trampoline (#348), per-CPU LAPIC timer (#312), soft-spl (#322), IOAPIC routing (#311), TLB shootdown via IPI (#304).
- **Locking modernization** — audited primitives (#303), `ipc_space` reader-writer lock (#327), per-pmap + split page-table locks (#329, #330, #338) with the shootdown barrier **formally proved** under x86-TSO via herd7 (#350, `uros/tools/litmus/`), lock-free capability lookups on a radix + seqlock table (#331), zone magazines + dynamic per-CPU allocation (#330).
- **futex primitives** (#324, #325) — wait/wake/requeue + `futex_waitv`; libpthreads mutexes/condvars ride the futex fast path.
- **Idle → HLT** (#357) and **HWP** (#358) — idle CPUs halt (worth 4–6.4× on all-32 bare-metal IPC latency) and hardware P-states are enabled at bring-up (a further −20/−40% on P-cores). `-S`/`-Q` boot flags opt out.
- **A season of SMP crash hunts** — kill×fork/exec storms, concurrent writers and 32-core sweeps flushed out the classic classes: error-path lock leaks (#383, #386→#390), TOCTOU wakeups (#387), COW-window fork corruption (#385), close/reclaim UAF (#388), UB shifts (#355, #362), a kernel receive-path overflow (#374), and historical no-op locks made real (#377, #384). The debug arsenal ships in-tree: per-CPU NMI hard-lockup watchdog (`-W`), DDB entry doors, owner-tracked mutexes, zone poisoning.

#### Console + bare metal

- **UEFI framebuffer console** (#342, #371, #372) — multiboot2 + GOP fbcons with write-combining mapping and a full-panel adaptive renderer.
- **On-screen console + virtual terminals** (#363, #364, #365) — keyboard+GPU console TTY, Ctrl-Alt-Fn switching, lazy per-VT shells.
- **Bare metal** — BIOS/CSM (omen, i7-gen7) and pure-UEFI (OMEGA, i9-13900K / 32 logical CPUs) validated for boot, output and benchmarks; bootable bench ISOs via `scripts/make-omen-boot.sh --iso --bench-only <suite>`.

#### Userland

- **cpustat** (#375) — per-CPU load monitor, the first dynamically-linked real tool in the image.
- **proc_server hardening** — pid-table slots reaped (#378, #389), honest `EAGAIN` from fork on a full table (#389), graceful reboot for SIGTERM handlers (#379), cross-CPU `task_terminate` (#380) with a `kill` builtin in ush.
- **Release gate at 8 CPUs** — `scripts/smoke-ush.sh --smp 8` (#376).

### v0.1.0 release highlights

Where v0.0.2 was "boots, runs servers, hits the IPC numbers", v0.1.0 is "boots to an interactive Unix-style shell with a working POSIX runtime". The big pieces:

#### POSIX runtime

- **musl libc** (#218+, #234, #289, #291, #292) — full musl port as the system libc, both static (`libc.a`) and dynamic (`libc.so`). The static archive carries no weak `-ENOSYS` stubs; the strong Uros syscall dispatcher in `libposix-uros` always wins. `__uros_clone` and `pthread_atfork`-based `post_fork_init` make fork-in-musl safe.
- **ld-musl dynamic linker** (#234) — musl's `ld-musl-i386.so.1` runs as PT_INTERP for dynamic executables, with `AT_BASE` and auxv plumbed by `exec_server`. `dlopen`/`dlsym`/`dlclose` exercised end-to-end through `dlopen_test` → `libfoo.so`.
- **libposix-uros** (#218, #259, #286) — POSIX shim sitting under musl: signals (sigaction/sigmask/per-thread exception ports), TLS install via LDT, stdio → ctty lazy-bind on fd 0/1/2, stack canary init before `main()` runs.
- **libpthreads native** (#257, #273, #274) — POSIX threads from scratch on top of UrMach `thread_create`, with real TLS (no more hardcoded `%gs = 0x1f`), `pthread_create`+`join`, mutex types (errorcheck/recursive), timedlock, robust, pshared, signals, naming, `setschedparam` write-through, `SCHED_OTHER`/`FIFO`/`RR`.
- **POSIX fd layer** (#262) — process-wide fd table with `dup`/`dup2`/`fcntl`/`close-on-exec`, fork inheritance, exec fd-handoff. Supersedes #233.

#### Process model + shell

- **proc_server v0.5** (#237, #238, #247, #275) — POSIX process model owner: pid/ppid/pgrp/sid tracking, `fork`/`exec_handoff`/`waitpid`, signal delivery (`sigaction`, `kill`, `killpg`, `pthread_kill`), controlling terminal (`tcsetpgrp`/`tcgetpgrp`), `SIGCONT`/`SIGTSTP`/`SIGINT` with durable pgrp semantics, `proc_shutdown` drain + clean unmount on `shutdown`/`halt`.
- **exec_server v0.4** (#228, #234, #287) — userspace ELF loader. Handles PT_INTERP for dynamic binaries, builds the System V i386 entry stack (`argc`/`argv[]`/`envp[]`/auxv), and supports **pid-preserving exec** (`proc_exec_handoff`+`replace_self`) so `execve()` keeps the caller's pid — required for a real shell.
- **ush** (#275.5) — the Uros shell. Acquires ctty + setsid, prints a `ush$` prompt, runs foreground (with `tcsetpgrp` + `waitpid` reaping) and background (`&`) jobs, exits cleanly on `shutdown`/`halt`.
- **/proc filesystem** — `proc_server` registers a mount at `/proc`, exposed through libvfs.

#### Storage + FS

- **libvfs v0.5** (#220, #232, #234 incr 4) — client-side VFS library: in-process mount table cache, name_server-backed dispatch, FLIPC v2 read-ahead fast path (≥2× over Mach `fs_read` at 16K/64K chunks with 192 KiB pipelined prefetch), write-behind buffer with flush-on-close. File-backed `mmap` via memory objects (#276 Phase B).
- **ext_server clean unmount** (#283, #284) — `EXT2_VALID_FS` set on `proc_shutdown` drain, so fresh boots see a clean filesystem.
- **disk image with full bundle** — `scripts/make-disk-image.sh` writes ush, hello_world, hello_dyn_world, hello_exec, flipc_bench, dlopen_test, libfoo.so, and `bench_large.dat`.

#### Drivers + I/O servers

- **char_server** (#205, #207, #275.x) — userspace character device server with hot-pluggable modules. `uart.so` provides COM1 read/write, line discipline, ctty job-control hooks (SIGTTIN/SIGTTOU on background reads/writes with TOSTOP).
- **gpu_server** (#194, #203, #268) — userspace GPU/display server with `vga.so` text-mode module, capability-gated discovery (cap_verify on every RPC), `libgpu_console` mirror for debug output. Keep-newest flow control on the console pipe under stress.
- **cap_server + libcap** (#197) — Uros capability authority with subscribe-revoke notifications; HAL/BDS/gpu/char all consume caps.
- **PCI userspace continues** — hal_server (#160) is the discovery + registry, BDS subscribes as HAL client, modules (`ahci.so`, `virtio_blk.so`) load through libmodload from `/mach_servers/modules/<class>/`.

#### Kernel (UrMach 0.1.0)

- **Versioning** (#295) — BSD-style `URMACH_VERSION_MAJOR/MINOR/PATCH/STRING` macros in `<mach/urmach_version.h>`; banner printed at `machine_startup` (`UrMach 0.1.0 — Uros microkernel`).
- **`%gs` leak fix** (#280) — `all_intrs` saves/restores `%gs` so interrupt entries no longer corrupt the user's TLS selector across return. General pattern documented for any new asm interrupt/trap frame.
- **PIC cascade fix** (#278) — kernel-side fix that unblocked COM1 IRQ4 from being starved.
- **MIG OOL leak fixes** (#293, #297, #298) — `_Xfs_read` server stub was emitting `deallocate = FALSE`, leaking one vm_map_entry per fs_read in the server's address space; ~2500 reads pinned ~160 MB of COW shadow pages and corrupted client control flow. Fixed with `, Dealloc` on `vfs.defs` and `ext2fs_server.defs`. For kernel/userspace-shared `.defs` (like `device.defs`), a new MIG keyword `UserDealloc` (#298) emits the dealloc flag only in the userspace stub, leaving the in-kernel `kmem_io_map` cleanup path intact.
- **CLI argv via SysV stack** (#294) — `crt0`'s asm trampoline captures the entry stack pointer and falls back to parsing `[argc][argv[]][NULL][envp[]]` for `execve`'d images; bootstrap-loaded servers still use `bootstrap_arguments`.
- All v0.0.2 kernel work still in: flat memory model, SYSENTER, SSE/SSE2 + XSAVE, stack protector, HIGHMEM to 4 GB, DDB continuations, protected payloads, ELF multi-segment + ET_DYN, kmsg pool LIFO, port-cache, direct thread switch, zero-copy OOL.

### IPC + performance

**Bare metal, v0.2.0** (2026-07-18, release kernel; medians / saturated points — hybrid P/E parts make single runs a lottery):

| Metric | OMEGA (i9-13900K, 32 CPUs) | omen (i7-gen7, 8 CPUs) |
|---|---|---|
| Combined null RPC, same-CPU | ~1.1 µs (P-core) / ~1.45 µs all-32 | 1.68 µs |
| Inter-task null RPC | 2.0–2.1 µs | 2.9 µs |
| Same-space concurrent RPC at saturation | ~12.6k ns/RPC @ 32 thr | ~10.1k ns/RPC @ 32 thr |
| Idle→HLT win (#357) | 4–6.4× across all suites | — |
| Page-fault scaling (#338) | — | ~2000 → 140–400 ns/fault (2 → 7-8 CPUs) |

Full tables, method rules and the placement analysis: [docs/release_notes_0.2.0.md](docs/release_notes_0.2.0.md) §Benchmarks.

#### Historical baseline (KVM, single core, v0.1.0)

Numbers from v0.0.2 still stand; v0.1.0 doesn't regress them. Mach IPC at ~1.2 µs intra-task null RPC, ~1.7 µs inter-task, zero-copy OOL constant ~2 µs from 4 KB to 64 KB. FLIPC v2 dominates throughput: 5 ns/op null batched, 17 ns/op 128 B produce+consume, 151 ns/op 4 KiB, 60-draw game frame in 0.34 µs batched.

**Mach IPC**

| Test | Latency |
|---|---|
| Intra-task null RPC | ~1.2 µs |
| Inter-task null RPC | ~1.7 µs |
| 4 KB OOL (zero-copy) | ~2.0 µs |
| 64 KB OOL (zero-copy) | ~2.0 µs |

**FLIPC v2** (shared-memory channels)

| Test | Throughput |
|---|---|
| null desc, batch=64 (intra) | 5 ns/op |
| 128 B produce+consume | 17 ns/op |
| 4 KB produce+consume | 151 ns/op |
| inter-task batch=64 (vm_remap) | 45 ns/op |
| 60-draw game frame (intra, batched) | 0.34 µs/frame |

Disk benchmarks (`disk_bench` via flipc_bench): raw AHCI 64 KB reads ~240–307 MB/s; warm page-cache speedup ~4.4×. libvfs FLIPC fast path beats the Mach OOL path on read-ahead workloads (≥2× at 16K/64K with 192 KiB pipeline). See `docs/bench/`.

### Build system

- CMake/Ninja, no legacy ODE/Makefiles
- MIG compiler (`migcom`) ported to modern Flex/Bison; supports `Dealloc`, `NotDealloc`, `UserDealloc` (#298), `CountInOut`, `Auto`, `Const`, `serverstripprefix` (#220), and the standard Mach 2.5 flag set
- All MIG stubs (user + kernel) generated at build time, no checked-in copies
- musl phase-1 prebuild for host tools; phase-2 build links userspace against the patched musl static + shared archives
- Userspace libraries `-fPIC`; bootstrap, BDS, ush, hello_dyn, dlopen_test linked PIE
- Multi-arch friendly layout (`export/include/`, ready for x86_64 / aarch64 / riscv64)

### Smoke + diagnostics

- `scripts/smoke-ush.sh` (driven by `smoke-ush.exp`) is the end-to-end acceptance run — boots Uros, anchors on the UrMach banner + ush prompt, exercises `hello_exec`, `hello_world`, `flipc_bench` standalone + `-R 60` FLIPC stress, FLIPC fd+read churn, and `shutdown` + clean unmount. Green on every release tag.
- `scripts/diag293-mach.exp` — focused stress for the Mach OOL `fs_read` path (`/flipc_bench -M 60`), used to bisect #293.
- `sig_test` — proc_server signal+job-control regression (14/14 across signal delivery, ctty, pgrp).
- `pthread_test` — 23/23 pthread regression (mutex types, condvars, rwlocks, barriers, futex paths).
- `cap_test` — capability suite with the #394 SHA-NI known-answer gate and the #395 raw-trap XMM-preservation probe (kernel FPU-discipline regression).
- The release gate runs the smoke at `--smp 8` plus all three suites on the release tip.

### User-space libraries

`libmach`, `libsa_mach`, `libposix-uros`, `libpthreads`, `libdl`, `libmodload`, `libflipc`, `libflipc2`, `libvfs`, `libnetname`, `libnetmemory`, `libmachid`, `libblk`, `libservice`, `librthreads`, `libxmm`, `libcap`, `libgpu_console`, `libc.so` (musl umbrella). All build successfully under the v0.1.0 toolchain.

## Building

### Requirements

- CMake >= 3.16
- Ninja
- GCC (i686 multilib or cross-compiler; tested with GCC 15)
- Flex, Bison (for the in-tree `migcom`)
- `sfdisk`, `mke2fs`, `debugfs` (for the disk image)
- QEMU (`qemu-system-i386`), optionally with KVM
- `expect` (for the smoke harness, optional)

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
  -DOSFMK_BUILD_PROC_SERVER=ON \
  -DOSFMK_BUILD_EXEC_SERVER=ON \
  -DOSFMK_BUILD_HELLO_SERVER=ON \
  -DOSFMK_BUILD_HELLO_EXEC=ON \
  -DOSFMK_BUILD_IPC_BENCH=ON \
  -DOSFMK_BUILD_PTHREAD_TEST=ON \
  -DOSFMK_BUILD_CAP_SERVER=ON \
  -DOSFMK_BUILD_CAP_TEST=ON \
  -DOSFMK_BUILD_GPU_SERVER=ON \
  -DOSFMK_BUILD_CHAR_SERVER=ON \
  -DOSFMK_BUILD_GPUSTAT=ON \
  -DUROS_BUILD_MUSL=ON \
  ..
ninja
cd ../..
./scripts/run-qemu.sh --fresh-disk --ahci          # graphical (QEMU window)
./scripts/run-ush.sh                               # minimal bundle, serial-only, drops into ush$
```

`run-qemu.sh` rebuilds `disk.img` and `bootstrap.bundle` automatically when `--fresh-disk` or `--bench` is passed.  `run-ush.sh` is the fastest path to an interactive shell over the serial console (Ctrl-A x to quit).

### CMake configuration options

Every userspace component is opt-in via a CMake flag.  Defaults keep a kernel-only build minimal; pass `-D<flag>=ON` to enable.

| Flag | Default | Component |
|---|---|---|
| `OSFMK_TARGET_AT386` | `ON` | Target arch (currently only AT386 / i386) |
| `OSFMK_BUILD_TOOLS` | `ON` | Host tools (`mig`, `migcom`, ...) |
| `OSFMK_BUILD_KERNEL` | `ON` | UrMach microkernel |
| `OSFMK_BUILD_BOOTSTRAP` | `ON` | Bootstrap server |
| `OSFMK_BUILD_DEFAULT_PAGER` | `OFF` | Default pager |
| `OSFMK_BUILD_NAME_SERVER` | `OFF` | `name_server` (netname + mount registry) |
| `OSFMK_BUILD_HAL_SERVER` | `OFF` | HAL server (PCI discovery + device registry) |
| `OSFMK_BUILD_BLOCK_SERVER` | `OFF` | Modular block device server |
| `OSFMK_BUILD_AHCI_DRIVER` | `OFF` | AHCI/SATA userspace driver module |
| `OSFMK_BUILD_VIRTIO_BLK` | `OFF` | virtio-blk userspace driver module |
| `OSFMK_BUILD_EXT2_SERVER` | `OFF` | ext2 file server |
| `OSFMK_BUILD_PROC_SERVER` | `OFF` | POSIX process server (#237) |
| `OSFMK_BUILD_EXEC_SERVER` | `OFF` | Userspace ELF loader (#228) |
| `OSFMK_BUILD_HELLO_SERVER` | `OFF` | Test server (Mach4 port) |
| `OSFMK_BUILD_HELLO_EXEC` | `OFF` | hello_exec acceptance binary (#228) |
| `OSFMK_BUILD_IPC_BENCH` | `OFF` | IPC + FLIPC v2 benchmark suite |
| `OSFMK_BUILD_PTHREAD_TEST` | `OFF` | libpthreads test harness |
| `OSFMK_BUILD_CAP_SERVER` | `OFF` | UrMach capability server + libcap |
| `OSFMK_BUILD_CAP_TEST` | `OFF` | Capability negative-test binary |
| `OSFMK_BUILD_GPU_SERVER` | `OFF` | userspace GPU/display server (#194) |
| `OSFMK_BUILD_GPUSTAT` | `OFF` | Probe for `gpu_query_stats` (#203) |
| `OSFMK_BUILD_CHAR_SERVER` | `OFF` | userspace character device server (#205) |
| `UROS_BUILD_MUSL` | `OFF` | musl libc port + shared `libc.so` + musl-linked apps (ush, hello_world, hello_dyn_world, flipc_bench, dlopen_test, ...) |

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

Each component has a Ninja target. Useful when iterating on one server / library. Always run from `uros/build/`.

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
ninja name_server_bin          # netname server + mount registry
ninja hal_server               # HAL: PCI discovery + registry
ninja hal_pci_scan_module      # HAL module: pci_scan.so
ninja block_device_server      # block server (loads AHCI/virtio modules)
ninja ahci_module              # block module: ahci.so
ninja virtio_blk_module        # block module: virtio_blk.so
ninja ext_server_bin           # ext2 file server
ninja proc_server_bin          # POSIX process server
ninja exec_server_bin          # ELF loader / process spawner
ninja cap_server_bin           # UrMach capability server
ninja gpu_server               # GPU/display server (#194)
ninja gpu_vga_module           # GPU module: vga.so
ninja char_server              # character device server (#205)
ninja char_uart_module         # char module: uart.so
```

**Musl-linked applications** (require `-DUROS_BUILD_MUSL=ON`):

```sh
ninja ush                      # Uros shell
ninja hello_world              # static musl hello (printf -> ctty)
ninja hello_dyn_world          # dynamic-linked twin (libc.so + ld-musl)
ninja hello_exec               # exec_server acceptance binary
ninja hello_dyn                # first dynamic ELF (PT_INTERP)
ninja flipc_bench              # standalone FLIPC v2 A/B benchmark (#272)
ninja dlopen_test              # dlopen/dlsym end-to-end test
ninja libfoo                   # dlopen target (.so)
ninja pthread_min              # minimal pthread regression (#291)
ninja fd_exec_test             # execve fd-inheritance smoke (#262)
```

**Test / bench binaries:**

```sh
ninja hello_server_server      # test server (Mach4 port)
ninja ipc_bench_server         # IPC + FLIPC v2 benchmark
ninja pthread_test_server      # libpthreads test harness
ninja cap_test_server          # cap_server negative tests
ninja gpustat_bin              # gpu_query_stats probe (#203)
ninja sig_test                 # proc_server signal exerciser (#238)
ninja kernel242_test           # kernel no-goto exerciser (#242)
```

**Libraries** (all output under `build/export/uros/<arch>/user/lib/`):

```sh
ninja libmach libsa_mach libpthreads libcthreads librthreads
ninja libposix-uros libposix-uros-pic
ninja libdl libmodload
ninja libflipc libflipc2
ninja libvfs
ninja libnetname libmachid libblk libservice libxmm libsa_fs
ninja libcap libgpu_console
ninja libc-musl                 # musl static archive
ninja libc-so                   # libc.so umbrella (musl + libposix-uros)
ninja ld-musl-i386              # dynamic linker
```

**Host tools:**

```sh
ninja migcom                   # MIG compiler (Flex/Bison)
```

### Disk image and stage-1 bundle

The disk and bundle are built by scripts in `scripts/`, not by CMake. Both pick up whatever binaries are present in `uros/build/export/.../user/sbin/`, so optional components are silently included only when their flag was enabled.

```sh
./scripts/make-disk-image.sh             # MBR + 3 partitions (ext2/ext2/raw swap)
./scripts/make-bundle.sh                 # multiboot stage-1 bundle for -initrd
./scripts/make-disk-image.sh --bench all # disk seeded for full ipc_bench suite
./scripts/make-bundle.sh --minimal       # stage-1 bundle for run-ush.sh
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
./scripts/run-qemu.sh --minimal                       # minimal bundle (boot straight into ush)
```

`scripts/run-ush.sh` is the convenience wrapper for `--minimal --allow-reboot -display none -serial mon:stdio`: it builds the minimal bundle and drops you straight at the `ush$` prompt over the serial console.

`--fresh-disk` is the safe default after a rebuild or an ungracefully-closed previous run — `disk.img` carries ext2 writeback state from the guest and a half-flushed image can cause spurious stage-2 hangs.

### Smoke test

```sh
./scripts/smoke-ush.sh                   # boots Uros, drives ush, asserts on every step
```

Exits 0 if every checkpoint passes (UrMach banner, ush prompt, hello_exec AUXV, hello_world stdout, flipc_bench standalone + readback, FLIPC stress, shutdown drain + clean unmount). Used as the v0.1.0 release gate.

## Roadmap

### v0.2.0 (this release)

- [x] SMP: pipelined AP bring-up to 32 logical CPUs on bare metal (#300-#316, #367)
- [x] Kernel locking modernization — pmap split locks, `ipc_space` rwlock, radix+seqlock capability table, zone magazines (#327-#331, #338)
- [x] herd7 formal validation of the pmap shootdown barrier under x86-TSO (#350)
- [x] futex + `futex_waitv` kernel primitives; libpthreads on the futex fast path (#324, #325)
- [x] Idle→HLT + HWP power management (#357, #358)
- [x] Interactive graphical console — on-screen TTY + Ctrl-Alt-Fn virtual terminals (#363-#365)
- [x] UEFI bare-metal boot with GOP framebuffer console (#342, #371, #372)
- [x] cpustat — first dynamically-linked tool (#375)
- [x] SMP crash-hunt hardening season (#317, #352-#362, #370-#395)
- [x] UrMach 0.2.0, `NCPUS=64` release build; smoke gate at `-smp 8` (#376)

### v0.1.0

- [x] Userspace AHCI (SATA) driver
- [x] Userspace virtio-blk driver
- [x] HAL server with PCI enumeration and driver registry
- [x] Dynamic module loader for userspace servers (libdl + libmodload + module pool)
- [x] musl libc port (static + shared)
- [x] ld-musl dynamic linker
- [x] POSIX process server (`fork`, `execve`, `waitpid`, signals, ctty, job control, `/proc`)
- [x] Userspace ELF loader with pid-preserving exec
- [x] Native libpthreads (TLS, mutex types, signals, scheduling)
- [x] libvfs v0.5 with FLIPC fast path
- [x] Interactive Uros shell (`ush`) over ctty
- [x] UrMach kernel versioning (0.1.0)
- [x] MIG OOL leak class fixed (`Dealloc` / `UserDealloc`)

### v0.3.0 — next release (planning)

- [ ] **x86-64 port** — PCID (context switch without TLB flush), SYSCALL entry, higher-half kernel + direct map: the next performance floor
- [ ] Wakeup placement, remaining increments (#356) — the measured lever for the many-core pre-saturation hump
- [ ] `ipc_space` write-side scaling (#340); deferred pmap follow-ups (#318, #349)
- [ ] IPC profiling (#392) before any IPC redesign (#391)
- [ ] USB stack (#353) + fbcons→gpu_server handoff (#369) — turns pure-UEFI machines interactive
- [ ] Source-tree reorganization (#396)
- [ ] Backlog from the 0.2.0 validation drive: on-screen `^C`/`^Z` (#397), cross-session kill (#398), AHCI 48-bit LBA (#399), GPT recognition (#400)

### Later / future

- [ ] FreeBSD userland port — `ls`, `cat`, `cp`, `sh`, basic utilities running on the POSIX runtime
- [ ] Capability system v1 — modern, performant cap layer (the design lives in `docs/uros_design.md` §4-5 and `cap_types.h` / `cap_manifest.h`)
- [ ] terminal stack — pipe/PTY with flow control; termios raw mode (the zsh-port prerequisites)
- [ ] libdl auto-bootstrap (#162) and per-class module pool maturity (#163); device.defs / BDS audit pass (#297 follow-ups)
- [ ] Unified Mach + FLIPC capability fabric (the "POST-Unix" goal: Unix-like personality on a modern microkernel + caps + high-performance IPC)
- [ ] Additional architectures: aarch64, riscv64, optionally aarch32
- [ ] Self-hosting

## Project structure

```
uros/
├── src/
│   ├── mach_kernel/           # UrMach microkernel
│   ├── bootstrap/             # Bootstrap server (PIE, libdl self-bootstrap)
│   ├── default_pager/         # Default pager
│   ├── name_server/           # netname server + mount registry
│   ├── hal_server/            # Userspace HAL: PCI enum, device registry
│   ├── block_device_server/   # Block server with AHCI + virtio-blk modules
│   ├── ext_server/            # ext2 file server (multi-mount)
│   ├── proc_server/           # POSIX process model (fork/exec/wait/signals, /proc)
│   ├── exec_server/           # Userspace ELF loader (PT_INTERP, auxv, pid-preserving exec)
│   ├── cap_server/            # Capability authority + revoke notifications
│   ├── gpu_server/            # GPU/display server (VGA text, libgpu_console mirror)
│   ├── char_server/           # Character devices (UART tty, PS/2 kbd, console TTY, job control)
│   ├── virtual_terminal_server/ # Ctrl-Alt-Fn VTs, lazy per-VT shells (#365)
│   ├── cpustat/               # per-CPU load monitor — first dynamic tool (#375)
│   ├── ush/                   # Uros shell
│   ├── hello_world/           # static musl printf hello
│   ├── hello_dyn_world/       # dynamic-linked twin (libc.so)
│   ├── hello_exec/            # exec_server acceptance binary
│   ├── hello_dyn/             # first dynamic ELF (PT_INTERP)
│   ├── flipc_bench/           # standalone FLIPC v2 A/B benchmark
│   ├── dlopen_test/           # dlopen/dlsym end-to-end
│   ├── libfoo/                # dlopen target .so
│   ├── pthread_min/           # minimal pthread regression
│   ├── fd_exec_test/          # execve fd-inheritance smoke
│   ├── sig_test/              # proc_server signal+job-control regression
│   ├── ipc_bench/             # IPC + FLIPC v2 benchmark
│   ├── pthread_test/          # libpthreads test harness
│   └── mach_services/
│       └── lib/
│           ├── libmach/       # Core Mach userspace library
│           ├── libsa_mach/    # Standalone Mach library
│           ├── libposix-uros/ # POSIX shim under musl (signals, TLS, ctty, syscalls)
│           ├── libpthreads/   # POSIX threads (native)
│           ├── libdl/         # Runtime ELF loader (dlopen/dlsym)
│           ├── libmodload/    # Shared module loader (per-class pool)
│           ├── libflipc/      # FLIPC v1
│           ├── libflipc2/     # FLIPC v2 (SPSC channels, endpoints, bufgroups)
│           ├── libvfs/        # Client-side VFS + FLIPC fast path
│           ├── libnetname/    # netname client
│           ├── libblk/        # Block I/O helpers
│           ├── libcap/        # Capability client (cap_verify, manifest)
│           ├── libgpu_console/# GPU debug console mirror
│           ├── musl/          # Patched musl libc (static + shared)
│           └── migcom/        # MIG compiler (Flex/Bison) — supports UserDealloc
├── export/include/            # Public headers (multi-arch: i386, <arch>/...)
├── tools/litmus/              # herd7 litmus proofs of the pmap barrier (#350)
├── build/                     # Build output
│   └── export/uros/boot/      # mach_kernel binary
scripts/
├── run-qemu.sh                # QEMU launch (--ahci, --virtio, --bench, --minimal, --smp N)
├── run-ush.sh                 # serial-only ush convenience launcher
├── smoke-ush.sh / .exp        # release smoke gate (--smp N)
├── diag293-mach.exp           # focused Mach OOL stress harness
├── make-disk-image.sh         # Disk image builder
├── make-bundle.sh             # Multiboot stage-1 bundle builder
└── make-omen-boot.sh          # Bare-metal hybrid GRUB image / bench ISOs (--iso --bench-only)
docs/
├── uros_design.md             # Full system design (HAL, drivers, caps, IPC, boot)
├── flipc2.md / flipc_v2_design.md
├── gpu_server_design.md
└── bench/                     # Benchmark results
```

## Origins

Based on the osfmk code from MkLinux DR3, originally developed by the Open Software Foundation (OSF) and Carnegie Mellon University (CMU). UrMach is Uros's evolution of that microkernel; the `urmach_*` prefix is reserved for new system calls while classic `mach_*` traps stay compatible.

## License

The original osfmk code retains its OSF/CMU licenses as stated in each source file. New code authored for Uros is MIT-licensed (`SPDX-License-Identifier: MIT`), with the author and license header at the top of each new file.
