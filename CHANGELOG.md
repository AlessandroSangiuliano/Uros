# Changelog

All notable changes to Uros are tracked in this file. Per-release narrative and deep technical context live in `docs/release_notes_<version>.md`.

The numbering tracks Uros as a whole (multiserver OS + userland). The UrMach kernel carries its own BSD-style version macros in `<mach/urmach_version.h>` (#295); from this release the kernel banner reads `UrMach 0.1.0 — Uros microkernel`.

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
