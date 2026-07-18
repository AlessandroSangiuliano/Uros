# Changelog

All notable changes to Uros are tracked in this file. Per-release narrative and deep technical context live in `docs/release_notes_<version>.md`.

The numbering tracks Uros as a whole (multiserver OS + userland). The UrMach kernel carries its own BSD-style version macros in `<mach/urmach_version.h>` (#295); from this release the kernel banner reads `UrMach 0.1.0 — Uros microkernel`.

---

## 0.2.0 — 2026-07-XX

**Theme**: SMP. Uros goes from a single-core system to a symmetric multiprocessor: UrMach boots, schedules and benchmarks on 32 logical CPUs on real hardware (Intel i9-13900K hybrid P/E), with modern per-CPU foundations, a phased kernel-locking modernization, idle power management, and a long tail of concurrency bugs found and fixed by running real workloads on real silicon. Kernel banner: `UrMach 0.2.0`.

See [docs/release_notes_0.2.0.md](docs/release_notes_0.2.0.md) for the full narrative.

### Added

- **SMP bring-up** — ACPI MADT CPU discovery, INIT/SIPI AP startup, per-AP CPU state init (CR4/FPU/SYSENTER/LAPIC LVT), IOAPIC interrupt routing with per-CPU LAPIC TPR, per-CPU LAPIC timer (clock off the PIT), cross-CPU reschedule IPIs, TLB shootdown via IPI (#300, #302, #304, #305, #309, #311, #312, #316).
- **Modern AP bring-up** (#367) — retired the 1994 MP-spec fixed delays for an attempt+retry fast path (INIT + one SIPI + TSC-deadline poll; full spec sequence with margin as the retry), then pipelined the whole bring-up: the BSP kicks every AP back-to-back and paces only the shared-trampoline transit through a post-lock-acquire funnel counter, so AP init (mp_desc/LAPIC/FPU/HWP) runs concurrently. ~120× faster per-AP kick under KVM; 31 APs online with visible overlap on OMEGA. A failed AP no longer hangs boot (the rendezvous barrier counts APs actually online).
- **Per-CPU data via `%gs`** (#301, #321) — modern per-CPU area replacing the cr3/array[cpu] CPU-number model; per-CPU SYSENTER trampoline drops the per-switch `IA32_SYSENTER_ESP` WRMSR (#348, ~144 cycles measured on bare metal).
- **Soft-spl** (#322) — interrupt-priority management without the per-transition hardware cost on the IPC fast path.
- **futex primitives** (#324, #325) — `urmach_futex` wait/wake/requeue and `urmach_futex_waitv` multi-wait; libpthreads mutexes/condvars rebuilt on the futex fast path (uncontended lock/unlock never enters the kernel).
- **Kernel locking modernization** (phased):
  - lock-primitive audit for `NCPUS > 1` — every primitive verified to compile to real atomics (#303, [docs/lock_audit_smp.md](docs/lock_audit_smp.md));
  - reader-writer lock for `ipc_space` on read-mostly paths (#327);
  - pmap giant-lock removal (#329) and per-pmap locking with lockless read paths, zone magazines, dynamic per-CPU allocation (#330);
  - modern capability table — radix tree + seqlock, lock-free name→entry lookups (#331);
  - split page-table locks for concurrent `pmap_enter`/`pmap_remove` (#338), with the enter↔activate TLB-shootdown barrier **formally validated** under x86-TSO with herd7 litmus tests (#350, [uros/tools/litmus/](uros/tools/litmus/));
  - cache-line padding for the per-CPU kmsg pool (#323).
- **Idle → HLT** (#357) — idle CPUs halt (two-phase Dekker handshake against the wakeup path) instead of PAUSE-spinning, freeing the shared package power budget; `-S` boot flag restores spin. 4–6.4× on all-32 bare-metal IPC latency.
- **HWP** (#358) — hardware P-states enabled at CPU bring-up with per-CPU MSR reporting; `-E` biases the energy/performance preference, `-Q` skips enabling (HWP enable is one-way until cold boot). −20/−40% verified in a controlled P-core A/B on top of the #357 win.
- **Synchronous-RPC hand-off, increment 1** (#356) — `-P` boot flag: hand-off-on-block wakeup placement; −11/−12% on same-task suites on bare metal.
- **Framebuffer console for UEFI bare metal** (#342, #371, #372) — multiboot2 boot path with ACPI RSDP handoff (#343), in-kernel GOP fbcons with write-combining mapping, full-panel char-cell renderer at native resolution with adaptive 2× font and jump-scroll.
- **On-screen console + virtual terminals** (#363, #364, #365) — keyboard+GPU console TTY through char_server↔gpu_server, Ctrl-Alt-Fn VT switching, lazy per-VT shells with a per-VT supervisor.
- **cpustat** (#375) — per-CPU `processor_info`/cpu_ticks plumbing verified under SMP, consumed by `cpustat` — the first dynamically-linked userland tool shipped in the disk image.
- **Debug doors for SMP crash hunts** — serial-break and PS/2 Ctrl+D DDB entry re-armed via RPC single-pass IPI (#335, #337, #382); per-CPU perf-counter NMI hard-lockup watchdog (`-W`); zone poisoning (`-Z`); owner-tracked mutexes (`MUTEX_OWNER_TRACK`).
- **Bench + harness** — ipc_bench concurrency suites `scale` (thread sweep) and `cc` (concurrent client pairs) (#351); smoke test at a chosen CPU count (`scripts/smoke-ush.sh --smp N`, #376); bare-metal bench ISOs via `scripts/make-omen-boot.sh --iso --bench-only <suite>` with a GRUB menu exposing `-c`/`-P`/`-E`/`-Q`/`-S` variants.

### Fixed

- **Bring-up races and UB** — early AP without `current_thread` page-faulting into `vm_fault`/`assert_wait` (#370); 3rd-CPU concurrent bring-up triple fault (#328); AP `%gs`/`cpu_number` window (#346); `lapic_to_slot[]` vs high APIC IDs on hybrid P/E parts (#354); `cpu_set` signed-shift UB hanging the scale sweep on 32 real cores (#355); `thread_resume` losing the race against embryo parking, leaving resumed-but-never-parked threads in limbo (#361).
- **Multiboot1 modules above 16 MB silently overwritten by the boot page tables** (#359, the #241 class on the mb1 path).
- **TLB shootdown vs `simple_lock` deadlock** when a CPU spins with IPIs masked (#317); cross-CPU shootdown/OOL-churn performance tax (#313); sub-tick time-of-day interpolation was `NCPUS == 1` only (#314).
- **SMP kernel races flushed out by kill×fork/exec storms**:
  - futex handoff dispatched a victim thread still `TH_RUN` on another CPU — double dispatch, double kernel page fault (#360);
  - SIGKILL racing exec's self-terminate: an error-bail path leaked `act_lock(rcv_act)`, deadlocking both terminators; an interrupted sender in `ip_blocked` took a phantom wakeup (#383);
  - SIGKILL racing fork's COW window: COW protection applied to the top object instead of the backing one corrupted both sides of the fork; the same hunt found `i386_set_ldt` passing a length where `vm_map_remove` expects an end address (wired `ipc_kernel_map` page leaked per exec) and ext_server fid exhaustion cured via dead-name notifications (#385);
  - `mmot_hotpath: bad ith_state` panic — the reply could complete between an unlocked state check and `imq_lock`; `MACH_MSG_SUCCESS` is now accepted as a slow-path outcome, not a panic (#387);
  - port-name cache: `is_generation` not bumped on `mod_refs`/`rename`/`copyin` let a dropped send right stay usable through the per-thread cache (#390, found by the #386 audit — the rest of the fast-path sweep verified clean);
  - `task_terminate` on a task with a thread live on another CPU deadlocked in `act_lock_thread`; fixed with a flat liveness wait, plus a `kill` builtin in ush to exercise it (#380);
  - `thread_wait` single-shot cross-CPU stop could block forever; serial input freeze traced to an edge-triggered IRQ4 lost while masked at the IOAPIC (#381).
- **Kernel receive-path overflow** — `msg_receive_error` copied a fixed 32 bytes without clamping to `rcv_size`, smashing the return linkage of a zero-slack concurrent receiver (#374; found by the scale sweep, unblocked the first-ever valid `cc` numbers).
- **Storage**: AHCI batch of exactly 32 slots issued no command (`ci_mask = (1u << 32) - 1` UB → silent zero-filled reads) (#362); ext2 writeback corruption under concurrent FLIPC writers — the ext2 server's historical no-op mutexes made real (`v_lock` + generation counter + `pc_busy` interlock) (#384); ext_server fid slot freed while `ext2fs_close_file` still walked it (#388).
- **proc_server**: pid-table slot never freed on reap — the 254-process wall (#378); `fork` returned 0 to the parent when `proc_register` failed on a full pid table (shell suicide), background zombies now reaped (#389); `reboot` no longer force-kills processes still handling SIGTERM (#379).
- **libpthreads**: `free_stacks` pop/push had no lock — two threads could win the same stack under concurrent create/destroy (#377); the thread-pool futex word was per-slot, so a recycled slot stranded one of two waiters (#352).
- **Test defects that could not fail** (#393, #394, #395) — the pattern from the `-smp 8` acceptance pass: the timedlock test treated `thread_switch(DEPRESS)` as synchronization (a UP assumption; libpthreads itself was correct) and its failure path deadlocked the suite, hiding three later tests (#393); the SHA-NI SHA-256 compress computed wrong digests (a missing `SHA256MSG1` in the last two message-schedule quartets) and its known-answer test did not gate the suite (#394); the kernel's SHA-NI HMAC ran on the caller's live FPU state under lazy FPU (the kernel is `-mno-sse` for exactly this reason) — hardware SHA-NI removed from the kernel entirely; userland libcap keeps it behind a self-test gate, and cap_test carries an XMM-preservation regression probe (#395).

### Changed

- **`NCPUS=64` is the release build** — one kernel binary runs 1..64 CPUs; the `-c N` boot flag caps how many are brought up.
- **Kernel SHA-256 is portable C only** (#395) — the kernel is SSE-free by construction again; SHA-NI acceleration lives in userland libcap only.
- **Default idle is HLT** (#357) and **HWP is enabled at bring-up** (#358) — `-S` and `-Q` opt out.
- **Direct thread switch is opt-in on SMP** (`-D`) — cross-CPU DTS pays an i386 cr3 TLB flush; the UP win returns with x86-64 + PCID.
- **#341 closed by design decision** — scaling a hot Mach port beyond its fixed floor is FLIPC v2's job (the data plane owns throughput); the Mach control plane is not micro-optimized further.
- **#319 closed by measurement** — the global run queue does **not** contend at this scale (0.2% of samples at saturation); the measured cause of the pre-saturation collapse is RPC-pair placement across idle CPUs, and the lever is #356's hand-off (`-P`). Per-CPU run queues are not the next move.
- **#347 closed by measurement** — eager XSAVE fires on ~0.01% of context switches on the IPC path (FPU hypothesis refuted); the concrete win extracted was skipping redundant LLDT reloads.

### Known limitations

- **Pure-UEFI machines with no PS/2 (OMEGA class) are headless/bench-only** in 0.2.0: no input until the USB stack (#353) and no on-screen userspace console until the fbcons→gpu_server handoff (#369). Interactive use = BIOS/CSM machines (omen) or QEMU. This is a declared perimeter, not a bug.
- **Pre-saturation scaling hump on many-core** — at 12–24 threads on 32 CPUs, RPC pairs scattered across idle CPUs pay cold caches and the wake path (up to ~55k ns/RPC, recovering to ~12.6k at exact saturation). Diagnosed (#319 verdict), lever identified (#356 `-P`); full placement work is next-cycle.
- **fork() rough edges from v0.1.0 remain** (#269 class); POSIX `uname()` backed by the version macros (#296) slipped to the next cycle.
- **THREAD_SWAPPER is still compiled into hot paths** — kernel rework planned post-SMP.
- **i386 only** — x86-64 (PCID, SYSCALL, 15 GP registers, higher-half + direct map) is the v0.3.0 theme.

---

## 0.1.0 — 2026-05-30

**Theme**: from "boots and runs servers" to "interactive POSIX environment". Uros now reaches an interactive shell (`ush`) over the controlling tty with a working musl-based runtime: `fork`+`execve`+`waitpid`, job control, signals, `/proc`, file I/O through libvfs, and runtime ELF loading through ld-musl + libc.so.

See [docs/release_notes_0.1.0.md](docs/release_notes_0.1.0.md) for the full narrative.

### Added

- **musl libc** as the system libc — static archive and shared `libc.so` (#218, #234, #289, #291, #292).
- **ld-musl-i386.so.1** dynamic linker — PT_INTERP loader, AT_BASE plumbing, dlopen/dlsym/dlclose end-to-end (#234, dlopen_test + libfoo.so).
- **libposix-uros** — POSIX shim sitting under musl: signals, TLS via LDT, stdio→ctty lazy-bind, stack canary init (#218, #259, #286).
- **libpthreads native** — POSIX threads from scratch on UrMach `thread_create`, with real TLS, mutex types, robust/pshared, signals, naming, `setschedparam` write-through, `SCHED_OTHER`/`FIFO`/`RR` (#153, #156, #220, #257, #273, #274).
- **POSIX fd layer** — process-wide fd table, fork inheritance, exec fd-handoff, `close-on-exec` (#262, supersedes #233).
- **proc_server v0.5** — POSIX process model owner: pid/ppid/pgrp/sid, fork/exec_handoff/waitpid, signal delivery, ctty (`tcsetpgrp`/`tcgetpgrp`), durable `SIGCONT`/`SIGTSTP`/`SIGINT` on pgrp, `proc_shutdown` drain + clean unmount (#237, #238, #247, #275).
- **exec_server v0.4** — userspace ELF loader: PT_INTERP for dynamic binaries, System V i386 entry stack (argc/argv/envp/auxv), pid-preserving exec (`proc_exec_handoff` + `replace_self`) (#228, #234, #287).
- **ush** — the Uros shell: ctty acquisition + setsid, foreground/background jobs with `tcsetpgrp` + `waitpid`, clean exit on `shutdown`/`halt` (#275, #275.5, #275.6).
- **libvfs v0.5** — client-side VFS with in-process mount cache, FLIPC v2 read-ahead fast path (≥2× over Mach `fs_read` at 16K/64K with 192 KiB pipeline), write-behind, file-backed `mmap` via memory objects (#220, #232, #276 Phase B).
- **/proc filesystem** — registered by proc_server through the name_server mount registry.
- **char_server + uart.so** — character device server with hot-pluggable modules, COM1 read/write, ctty job-control hooks (SIGTTIN/SIGTTOU with TOSTOP) (#205, #207).
- **gpu_server + vga.so + libgpu_console** — userspace GPU/display server with capability-gated discovery, VGA text-mode module, debug-mirror console (#194, #203, #268).
- **cap_server + libcap** — Uros capability authority with subscribe-revoke notifications (#197).
- **UrMach kernel versioning** — `<mach/urmach_version.h>` with MAJOR/MINOR/PATCH/STRING macros, boot banner from `URMACH_VERSION_STRING` (#295).
- **MIG `UserDealloc` keyword** — emit `deallocate = TRUE` only in the userspace server stub of a kernel/userspace-shared `.defs`, leaving the in-kernel stub unchanged (#298).
- **MIG smoke + regression harnesses** — `scripts/smoke-ush.sh` (release gate driven by `smoke-ush.exp`), `scripts/diag293-mach.exp` (focused OOL stress), `sig_test` (proc_server signals/job-control 14/14), `pthread_test` (libpthreads 22/22 after #257+#273+#274).
- **Standalone flipc_bench binary** — median-of-N FLIPC v2 A/B benchmark launched from the shell on a quiescent system (#272).
- **scripts/run-ush.sh** — convenience launcher: minimal bundle, serial-only, straight to `ush$`.

### Fixed

- **Mach OOL fs_read churn corruption** — `_Xfs_read` MIG server stub emitted `deallocate = FALSE`, leaking one vm_map_entry per fs_read in ext_server; after ~2500 reads (~160 MB of COW shadow pages pinned), kernel-wide memory pressure corrupted the client's control flow (eip jumped into the read buffer). Fix: `, Dealloc` on `vfs.defs` fs_read/fs_readdir; same pattern in `ext2fs_server.defs`; `device.defs.device_read` switched to `UserDealloc` (#293, #297, #298).
- **Kernel `%gs` leak through interrupt frames** — `all_intrs` did not save/restore `%gs`, so interrupts entering with one TLS selector and returning with another corrupted the user's TLS. The earlier #279 workaround (reload `%gs` post-syscall) is no longer needed (#280).
- **PIC cascade IRQ4 starvation** — cascade ack ordering was starving COM1 IRQs under load (#278).
- **execve argv lost for execve'd images** — `bootstrap_arguments` only answers for bootstrap-loaded servers; execve'd images carry argv on the System V i386 entry stack. `crt0` now captures `%esp` before the C prologue and falls back to the stack convention when `bootstrap_arguments` returns nothing (#294).
- **pid-preserving execve** — without it, exec replaced the calling task with a fresh pid, breaking everything downstream of `waitpid`. `proc_exec_handoff` + idempotent register + `replace_self` keep the original pid across exec (#287).
- **ext_server clean unmount** — `EXT2_VALID_FS` is now set on `proc_shutdown` drain, so fresh boots see a clean filesystem (#283, #284).
- **gpu_server console stall** — VGA mirror was drop-newest under stress and would freeze mid-frame; switched to keep-newest in `text_render.c`, kernel msg qlimit bumped 16→32 (#268).
- **libvfs send-right leak in mount-cache fast path** — `vfs_resolve_mount` no longer leaks one send right per cache hit.
- **pthread_create EAGAIN under disk stress** — fixed by the #257 native libpthreads runtime + #273/#274 scheduling work (residual hello_server EAGAIN under `--bench disk` is pre-existing noise, see project_hello_server_pthread_eagain.md).

### Changed

- **No more `-Wl,-u` workarounds** for musl static binaries (#291) — the static `libc-musl.a` no longer carries weak `-ENOSYS` stubs that used to shadow the strong libposix-uros dispatcher; the strong defs are pulled automatically. `__uros_clone` is safe to pull into forking binaries since #292.
- **Kernel-side device buffer handling stays on `kmem_io_map`** — `UserDealloc` (#298) ensures `device_read.data` emits `deal = TRUE` in the BDS stub while keeping the in-kernel stub at `deal = FALSE`, so the existing `kmem_io_map` / `io_alloc_size` continuation cleanup in `ds_routines.c` is preserved.
- **Build system honors `UROS_BUILD_MUSL`** — adds musl phase-1/phase-2, ld-musl, libc.so umbrella, and all the musl-linked applications (ush, hello_world, hello_dyn_world, flipc_bench, dlopen_test, pthread_min, fd_exec_test).
- **README** updated to describe the v0.1.0 interactive system; new sections on the POSIX runtime, shell, and dynamic linking.

### Known limitations

- The QEMU graphical window is read-only for now — interactive ush still goes through the serial console. Driving the shell from the VGA window (PS/2 keyboard + console TTY) is a v0.2.0 theme.
- `device.defs.device_read` follow-up audit (broader sweep of OOL `out`s in kernel-shared `.defs`) — covered for the immediate leak (#298), wider pass deferred.
- Static binaries are the default; the dynamic path (libc.so + ld-musl) is fully wired (#234 increment 4) but most stage-1 servers stay static by design (trusted base).
- No SMP, no x86_64 yet — both planned for after the v0.2.0 round.

---

## 0.0.2 — 2026-03 (prior baseline)

Recorded here for completeness — see the v0.0.2 README archived in the git history (commit f9c60b11 and its parents) for the full list.

### Added

- Userspace AHCI (NCQ, scatter-gather, 128K PRDT, request merging, async writeback, readahead, zero-copy DMA) and virtio-blk drivers, unified behind a modular block device server (#58–#78, #157).
- HAL server with userspace PCI discovery, module registry, dead-name cleanup, async event notifications (#160).
- Dynamic module loader: libdl runtime (#159), bootstrap ET_DYN + libs `-fPIC` (#165, #166), BDS PIE with libdl self-bootstrap (#167), libmodload shared library (#171), per-class module pool over MIG (#163).
- libpthreads as a replacement for libcthreads — mutex types, timedlock, robust, pshared, signals, naming, condvar clock, stack guard, contention scope, concurrency hint, thread pool (#82, #108–#146).
- FLIPC v2 — lock-free SPSC channels, endpoint registry, buffer groups, per-channel page isolation; sub-microsecond throughput, 4–150× faster than Mach IPC on batched workloads (#90, #119, #120, #121).
- Kernel: protected payloads (#81), child-task MIG (#62), DDB continuations (#61), HIGHMEM to 4 GB with kmap/kunmap (#70), `mach_print` bench trap (#71), netname dead-name notifications (#72).
- ext2: page cache (#66), write-back (#71), readahead/merging/zero-copy DMA/block layer (#74–#78), batch device write (#85), dirty inode list (#86), readv chunks (#89), SSE2 memcpy (#91), multi-mount (#92), inode cache (#93), file pool (#94), negative dcache (#96), batch open+read (#97), file-handle reuse (#98), PP dispatch (#99), VFS inode split (#101).
- Build: userspace `<mach/*.h>` regenerated from `.defs` at build time (#169), kernel-side MIG user stubs regenerated (#175), `export/powermac/` renamed to `export/include/` for multi-arch (#170).

---

## 0.0.1 — initial baseline

Boot of the OSF Mach + MkLinux DR3 base on modern toolchain (CMake/Ninja, GCC 15, `-std=gnu11`), with the kernel banner from the legacy `conf/version.*` files. Pre-history of issue tracking.
