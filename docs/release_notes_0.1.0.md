# Uros v0.1.0 — release notes

**Released:** 2026-05-30
**Kernel:** UrMach 0.1.0 (`URMACH_VERSION_STRING`)
**Target arch:** i386
**Companion docs:** [README.md](../README.md), [CHANGELOG.md](../CHANGELOG.md), [docs/uros_design.md](uros_design.md)

---

## Headline

If v0.0.2 was the release that proved Uros could boot a full multiserver stack and hit microsecond IPC numbers, **v0.1.0 is the release where you can sit at the prompt and type**. After a clean boot the system lands at:

```
ush v0.1.0 — Uros shell (#275.5)
ush$
```

…and `fork`+`execve`+`waitpid` actually work. Static and dynamically-linked musl binaries both run. Job control behaves (foreground vs background, `tcsetpgrp`, `SIGCONT`/`SIGTSTP`/`SIGINT`). `printf("hello\n")` goes to the controlling tty. The shell exits cleanly on `shutdown`/`halt` and the filesystem is marked clean for the next boot.

This is the version where Uros stops being "a microkernel that runs servers" and starts being a usable Unix-style environment built **on top of** that microkernel. The Mach + capabilities + FLIPC fabric is still underneath unchanged; what changed is that the POSIX layer is now thick enough to host real programs.

---

## Audience and goals

These notes are for:

- **Contributors and reviewers** who want to know what landed and why.
- **Future maintainers** debugging a regression and trying to find the commit that introduced a behavior.
- **People reading the code** who want a map of where the v0.1.0 work lives.

If you only need the user-facing summary, the README's "Current status" section is enough. If you need the precise issue-by-issue list, the CHANGELOG covers that. This document sits between the two: it tells the story of how the pieces fit together and what motivated each decision.

---

## The shift, in one diagram

```
v0.0.2                                  v0.1.0
------                                  ------
QEMU                                    QEMU
 └── UrMach                              └── UrMach 0.1.0 ── banner
      └── bootstrap                           └── bootstrap
           ├── default_pager                       ├── default_pager
           ├── name_server                         ├── name_server + mount registry
           ├── hal_server                          ├── cap_server                    *
           ├── block_device_server                 ├── hal_server
           ├── ext_server                          ├── block_device_server
           ├── hello_server                        ├── gpu_server                    *
           └── ipc_bench                           ├── char_server (UART tty)        *
                                                   ├── ext_server (multi-mount)
                                                   ├── proc_server (fork/exec/...)   *
                                                   ├── exec_server (ELF + auxv)      *
                                                   ├── ush ── prompt over ctty       *
                                                   └── (user can now type)           *

                                        Userland: musl libc (static + libc.so + ld-musl)
                                        Runtime:  fork, execve, waitpid, signals,
                                                  ctty + job control, /proc, libvfs,
                                                  POSIX fd layer, dlopen
```

Lines marked `*` are new in v0.1.0.

---

## What landed, by subsystem

The work fell into seven natural areas. Each subsection lists the issues that drove it, the user-visible result, and where the code lives.

### 1. UrMach 0.1.0

**User-visible:** the kernel now prints its own version banner; the rest of the work is invisible by design.

- **Versioning** (#295). `<mach/urmach_version.h>` exposes `URMACH_VERSION_{MAJOR,MINOR,PATCH,STRING}` macros with BSD-style bump rules. `kern/version.c` builds the banner from these and the kernel emits `UrMach 0.1.0 — Uros microkernel` at `machine_startup`. UrMach is versioned independently of the legacy OSFMK / MkLinux numbering, which stays as a heritage reference. `uname()` will be wired to these macros in v0.2.0 (#296).
- **`%gs` leak through interrupt frames** (#280). `all_intrs` previously did not save and restore `%gs`, so any interrupt entering with one TLS selector and returning with another corrupted the user's TLS slot. With native libpthreads the issue surfaced as the calling thread "losing" its TLS after almost any signal. Fix follows the canonical pattern documented in `project_280_gs_leak_fix.md`: a "gs-only lean" save/restore around the trap frame. The #279 workaround (reload `%gs` post-syscall) is now retired.
- **PIC cascade IRQ4 starvation** (#278). Under heavy IRQ activity (UART + AHCI completion + timer), cascade ack ordering was starving COM1 IRQs, which manifested as occasional hangs at the ush prompt. Fix in the master PIC handler.
- **All v0.0.2 kernel work preserved**: flat memory model, SYSENTER, SSE/SSE2 + XSAVE, stack protector, HIGHMEM to 4 GB, DDB continuations, protected payloads, kmsg pool LIFO, port-cache, direct thread switch, zero-copy OOL.

### 2. musl libc and dynamic linking

**User-visible:** binaries are linked against musl, both static and dynamic. `printf`, `fopen`, `mmap`, `sigaction`, `pthread_create` work the way a C programmer expects.

The big architectural decision behind v0.1.0 is consolidating on musl as the system libc instead of growing libsa_mach into a half-POSIX library. This was traced in `project_libc_so_roadmap.md` and converged in eight phases over #218, #219, #221, #234.

- **Patched musl** sits under `src/mach_services/lib/musl/` (port BSD-attributed where applicable; original Uros patches are MIT). It builds both as a static archive (`libc-musl.a`) and as a shared library participating in the libc.so umbrella.
- **libposix-uros** (#218) is the shim layer between musl and Uros. It owns the Uros-specific bits POSIX would otherwise abstract: signal delivery via per-thread exception ports (`sigaction`/`sigmask`/handlers), TLS install via LDT (real `%gs` slot, not the old hardcoded 0x1f from #259), syscalls (`__uros_syscallN` dispatcher pulled in by static musl since #291), stdio bound to the controlling tty on first use of fd 0/1/2 (#286), stack canary set up before `main()` runs.
- **No more `-Wl,-u` workarounds.** Pre-#291 musl shipped weak `-ENOSYS` stubs for the Uros syscall dispatcher that would shadow the strong libposix-uros dispatcher in a static link unless every binary forced the symbol with `-Wl,-u`. #291 strips those weak stubs from `libc-musl.a` so the strong defs are pulled automatically; #292 makes `__uros_clone` safe to pull in by moving the post-fork reinit into a `pthread_atfork` handler. Both are documented in `project_musl_weak_stubs_root.md`.
- **ld-musl-i386.so.1** (#234) is the dynamic linker. exec_server recognizes PT_INTERP, maps the interpreter, plumbs `AT_BASE` into the auxv, and hands control to the interpreter, which then loads libc.so and resolves the binary. `dlopen`/`dlsym`/`dlclose` are exercised end-to-end through `dlopen_test` → `libfoo.so`. Increment 4 of #234 (the "dynamic-task mach_init" piece — a ctor in libposix-uros-pic that seeds `mach_task_self_` + `mig_init` + `mach_init_ports` at libc.so startup) unblocks all dynamic apps doing I/O.
- **libc.so umbrella** (final stage of `project_libc_so_roadmap.md`) ties musl + libposix-uros into a single shared library. Static binaries see the same surface against `libc-musl.a`. The two paths share one set of headers.
- **First two real binaries on each path:**
  - Static: `hello_world` (#286) — `printf("hello\n")` going through stdio → ctty.
  - Dynamic: `hello_dyn_world` (#289) — same source, linked against libc.so, exercises the full ld-musl + umbrella libc.so loop including clean exit and reap.

### 3. POSIX process model — proc_server, exec_server

**User-visible:** `fork`+`execve`+`waitpid` work. Jobs back to foreground come back with `SIGCONT` to the pgrp. `Ctrl-C` kills the foreground process. The shell sees correct exit codes.

This is the "Unix personality" piece of the system. Uros's choice (`project_unix_personality.md`) is to distribute the personality across a few cooperating servers rather than a monolithic Unix server. The split for v0.1.0:

- **proc_server v0.5** (#237, #238, #247, #275). Owns pid/ppid/pgrp/sid tracking and is the authority for the POSIX process table. Implements:
  - `proc_fork` / `proc_exec_handoff` / `proc_exit` / `proc_wait` over its MIG interface.
  - **Signal delivery** — `sigaction` registered handlers, `kill` / `killpg` / `pthread_kill` routed via per-thread exception ports. Background-pgrp readers/writers get SIGTTIN / SIGTTOU with TOSTOP semantics (#275.3 path, char_server gates this on the foreground pgrp). `sig_test` exercises the full matrix (14/14 green).
  - **Controlling terminal + job control** (#247) — `tcsetpgrp` / `tcgetpgrp`, durable `SIGCONT` on a pgrp (a stopped pgrp coming back to foreground resumes correctly), `SIGINT` to the foreground pgrp on Ctrl-C.
  - **Clean shutdown** (#283, #284) — `proc_shutdown` drains tasks with SIGTERM, escalates to SIGKILL, syncs every mount via `fs_sync`, then unmounts cleanly (`EXT2_VALID_FS` is set so the next boot sees a clean filesystem).
  - **`/proc` mount** — registered through the name_server mount registry, exposed over libvfs.
- **exec_server v0.4** (#228, #234, #287). Userspace ELF loader. Reads the file via libvfs, validates the ELF header, maps PT_LOAD segments into the target task's vm_map, handles PT_INTERP for dynamic binaries (loading ld-musl at `AT_BASE`), and builds the System V i386 entry stack with `argc`, `argv[]`, `envp[]`, and a full auxv (`AT_PAGESZ`, `AT_RANDOM`, `AT_SYSINFO_EHDR`, `AT_ENTRY`, `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_BASE` when applicable).
  - **Pid-preserving exec** (#287) was the key insight that unlocked the shell. Before #287, exec replaced the calling task with a freshly-created task, getting a new pid in the process — which broke every higher-level expectation (`waitpid` saw an unknown pid, the shell's job table desynced). Fix: `proc_exec_handoff` notifies proc_server, exec_server idempotently re-registers under the original pid, and the calling task uses `replace_self` to swap its own image. Commits `6bbb073` + `51d0b81` on `feature/287-tty-output-ordering`. This is the foundation that makes a real shell possible.
- **execve's argv reaches the program** (#294). Bootstrap-loaded servers receive argv via the `bootstrap_arguments` RPC; execve'd images cannot — they're not in bootstrap's `servers[]` table, so the RPC returns `KERN_INVALID_ARGUMENT`. The System V i386 ABI expects `argc`/`argv[]`/`envp[]`/auxv on the entry stack. exec_server already laid that down; `crt0` was the missing piece — it now captures `%esp` in an asm trampoline before the C prologue perturbs it, and `__start_mach_c` parses the SysV stack as a fallback when `bootstrap_arguments` returns nothing. Bootstrap-loaded servers still take the RPC path.

### 4. Native libpthreads runtime

**User-visible:** `pthread_create`, `pthread_join`, mutexes, signals, scheduling all work from C as expected.

The pre-v0.1.0 libpthreads was a port of an older Mach threading library with several Uros-specific hardcodes (the LDT TLS slot at `%gs:0x14` was assumed to be the canary slot regardless of the actual TLS layout, the master thread used a static structure with `&tp_word` instead of `&thr` as the LDT base). All of that is now correct.

- **libpthreads completion** (#257, #273, #274, #153, #156, #220). The #257 sweep (`project_phase6b_pthread_pending.md`) fixed seven root causes that together prevented `pthread_create` + `pthread_join` from working:
  1. `libc.tls_size` manual init,
  2. circular list init in `__pthread_internal_seq`,
  3. LDT base set to `&thr` (not `&tp_word`),
  4. TLS install **on-thread** via `__uros_thread_bootstrap` in C,
  5. `install_thread_tls_at` variant taking a raw TP (not a `user_desc`),
  6. `SYS_exit` → `thread_terminate`,
  7. userspace `CLEARTID` in `__pthread_exit`.
- **Scheduling** (#273, #274). `pthread_setschedparam` writes through to the underlying Mach `thread_policy_set`, `pthread_getschedparam` refreshes from the live policy, the default policy is `SCHED_OTHER`, and `SCHED_FIFO`/`SCHED_RR` are wired into the processor set and respected on explicit creates. `pthread_test` is 22/22 green.

### 5. POSIX fd layer and ush — the user-facing layer

**User-visible:** a shell prompt, `&` for background jobs, `Ctrl-C` to interrupt, `shutdown` to power off.

- **POSIX fd layer** (#262, supersedes #233). Process-wide fd table living in libposix-uros, with `dup`/`dup2`/`fcntl`/`F_DUPFD`/`F_SETFD`/close-on-exec. Fork inheritance copies the table by reference (open-handle dup semantics) into the child; exec preserves non-CLOEXEC fds across the image swap via `proc_exec_handoff` carrying a serialized fd snapshot. `fd_exec_test` is the smoke for that round-trip.
- **ush** (#275, #275.5, #275.6). The Uros shell. End-to-end smoke documented in `project_275_5_done.md`:
  - Acquires the controlling terminal and `setsid`s as session leader.
  - Reads from stdin (the ctty), tokenizes, looks up the program in libvfs.
  - Foreground job: `fork` → child execve, parent does `setpgid` + `tcsetpgrp(child_pgrp)` + `waitpid` + `tcsetpgrp(self_pgrp)` on completion.
  - Background job: `fork` + `setpgid`, prints `[bg] pid=N`, does not `waitpid` synchronously (reaped on completion via SIGCHLD).
  - Builtins: `cd`, `exit`, `shutdown`, `halt`, `reboot`.
- **`scripts/run-ush.sh`** — convenience launcher: builds the minimal bundle, runs QEMU with `-display none -serial mon:stdio`, hands you straight to the `ush$` prompt over the terminal.

### 6. libvfs v0.5 and storage

**User-visible:** files in `/` and `/mnt/disk1` open, read, write, stat, mmap. Reads through libvfs are visibly faster than v0.0.2's direct Mach `fs_read` calls under read-ahead-friendly workloads.

- **libvfs v0.1** (#220). Client-side VFS library: in-process mount table cache, name_server-backed dispatch, naming convention split (`vfs_open`/`vfs_read`/... public; `fs_open`/`fs_read`/... private MIG); driven by migcom's new `serverstripprefix` keyword. Two-RPC retry on stale mounts (dead-name notification evicts the cache).
- **libvfs v0.5 with FLIPC fast path** (#232, #234 increment 1). Reads can bypass the Mach `fs_read` OOL path and go through a FLIPC v2 channel with a 192 KiB pipelined prefetch window. ≥ 2× over the Mach path at 16K/64K chunks. Falls back to Mach automatically when the server doesn't advertise the fast path (`fs_flipc_endpoint`).
- **File-backed mmap** (#276 Phase B). `fs_mmap` returns a memory object port that libvfs maps into the caller's address space; faults route to fs_server via `libfspager`. Used by ld-musl to map libc.so and other shared libraries on demand.
- **Mount registry** (`project_mount_registry.md`). `name_server`'s `mounts[]` list is the source of truth; `netname_look_up_mount` / `check_in_mount` are the RPCs. ext_server mounts `/` and `/mnt/disk1`, proc_server mounts `/proc`.
- **ext_server clean unmount** (#283, #284). `EXT2_VALID_FS` is set on `proc_shutdown` drain; latent vnode leak in `ext_pager_close_file` fixed (`vnode_put`); `FS_NO_RESOURCES` returned cleanly when the pool is full.

### 7. I/O servers — char_server, gpu_server, cap_server

**User-visible:** keystrokes echo, output reaches the screen and the serial console, capabilities are checked on each privileged RPC.

- **char_server** (#205, #207). Userspace character device server with hot-pluggable modules. `uart.so` is the COM1 module: `tty_read` blocks until data arrives, `tty_write` sends bytes out and respects TOSTOP for background pgrp writers, line discipline handles `^C` → SIGINT to the foreground pgrp, `^Z` → SIGTSTP. Job control hooks documented in `project_char_server_threading.md`.
- **gpu_server** (#194, #203, #268). Userspace GPU/display server with capability-gated discovery (cap_verify on every RPC) and a `vga.so` text-mode module for the VGA legacy path. `libgpu_console` is a debug-mirror layer that lets servers print to the screen during early boot before stdio is wired. The #268 fix (console stall under stress) switched the mirror's flow control from drop-newest to keep-newest in `text_render.c` and bumped the kernel msg `qlimit` from 16 to 32.
- **cap_server + libcap** (#197). Uros capability authority: cap allocation, manifest registration, `cap_verify` on RPC entry, `subscribe_revoke` notifications for resource revocation. HAL, BDS, gpu_server, char_server all consume caps. Phase compatibility note in `project_capability_design_reconstruct.md`: per-file enforcement stays classic uid/mode in ext_server for now; the capability layer covers device and IPC authorities. The modern cap design lives in `cap_types.h` + `cap_manifest.h` + `libcap.h` + `docs/uros_design.md` §4-5 and is the v0.2.0 / v1.0 target.

### 8. MIG correctness — the leak class

**User-visible:** the system survives sustained workloads that previously crashed it. The investigation harness (`scripts/diag293-mach.exp`) lives in the tree.

#293 was the most subtle bug fixed in v0.1.0 and the rest of the section is the story:

- **The symptom.** `/flipc_bench -M 60` (Mach OOL fast path, FLIPC disabled) deterministically crashed at the 13th–14th open, with a `user_page_fault_continue: FAILED eip=0x80819b5 cr2=<random>`. `eip = 0x80819b5` is `__bss_start` of `flipc_bench`, i.e. the first byte of `fb_buf[1MB]` — control flow had jumped into the read buffer and faulted on whatever the file's random bytes decoded as.
- **The investigation.** A kernel-side instrumentation pass on `vm_map_copyout` (per-map counter + nentries + start address) showed the client's vm_map was stable: `nent=12`, `start=0x48000` reused across 4000 receives. The leak was in the SERVER's vm_map (ext_server). After 7000 receives it had grown to 6688 entries. Documented in `project_293_mach_ool_churn.md`.
- **The root cause.** The MIG-generated `_Xfs_read` server stub emitted `OutP->data.deallocate = FALSE`, the default. The kernel's `ipc_kmsg_copyin_body` therefore called `vm_map_copyin(server_map, addr, len, src_destroy=FALSE)` on the buffer that `ds_ext2_read` had `vm_allocate`'d for the reply. With `src_destroy=FALSE`, the entry stays in the server's map. `ds_ext2_read` never deallocates the buffer manually. Each fs_read leaked one entry of `count / PAGE_SIZE` pages in ext_server. After ~2500 64 KiB reads, ~160 MB of COW shadow pages were pinned in a 512 MiB QEMU guest. The resulting kernel-wide memory pressure corrupted the client's stack.
- **The fix.** Add `, Dealloc` to the OOL `out data` parameter in `vfs.defs` (and `fs_readdir.entries`). MIG then emits `deallocate = TRUE`, which makes the kernel call `vm_map_copyin(..., src_destroy=TRUE)` — the buffer leaves the server's map as it's packaged into the reply. No server-side code change needed. Same pattern in `ext2fs_server.defs` (#297). Verified mechanically by `grep deal` on the regenerated stubs and end-to-end by re-running `/flipc_bench -M 60` (60 opens × 192 reads = 11 520 fs_read OOL receives, no crash).
- **The `UserDealloc` keyword** (#298). `device.defs` is shared between BDS (userspace, vm_allocate-and-reply) and the in-kernel device server (manages buffers through `device_io_map` + `io_alloc_size` continuation cleanup). Forcing `Dealloc` on the shared `.defs` would have the kernel MIG stub do `vm_map_copyin(kernel_map, ..., src_destroy=TRUE)` on an address that lives in `device_io_map` — silent failure at best. The new keyword resolves at MIG-generation time: `UserDealloc` emits `deal = TRUE` only when `!IsKernelServer`, leaving the kernel stub at `deal = FALSE`. One source of truth, two correct outputs.
- **Kernel-side audit** (`feedback_migcom_userdealloc.md`). All 16 kernel sites that allocate via `kmem_alloc(ipc_kernel_map | kernel_map)` for OOL reply are paired with `vm_map_copyin(<same map>, ..., src_destroy=TRUE)` on the success path and `kmem_free` on error paths — the canonical kernel pattern, documented for future contributors with three reference examples (`do_bootstrap_arguments`, `mach_port_names`, `ds_read_done`). No kernel-side leak equivalent to the `_Xfs_read` bug.

---

## Build and tooling changes

- **`UROS_BUILD_MUSL`** is the master CMake flag for the v0.1.0 userland. With it on, the build adds musl phase-1 host build, the patched musl static + shared archives, ld-musl, the libc.so umbrella, and all musl-linked binaries (ush, hello_world, hello_dyn_world, hello_exec, hello_dyn, flipc_bench, dlopen_test, pthread_min, fd_exec_test, libfoo).
- **migcom UserDealloc** (#298). Three plumbing sites in `src/mach_services/lib/migcom/`: `type.h` flag (`flUserDealloc = 0x400`), `lexxer.l` token, `routine.c` resolution. Validated by inspecting the regenerated `device_server.c` in both kernel and userspace builds.
- **Smoke harness.** `scripts/smoke-ush.sh` is the release gate. It boots Uros under QEMU, anchors on every checkpoint (UrMach banner, ush prompt, hello_exec AUXV, hello_world stdout, flipc_bench standalone + readback, FLIPC fd+read stress, shutdown drain, clean unmount), and exits 0 on success.
- **Diag harness.** `scripts/diag293-mach.exp` is the focused stress for the Mach OOL `fs_read` path. Used to bisect #293; kept in-tree as a reproducer for similar leak hunts.

---

## Benchmarks (KVM, single core)

v0.1.0 deliberately does **not** regress v0.0.2's IPC numbers. Re-measured on the same setup, post-fix:

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

**Disk** (AHCI, KVM)

- Raw 64K reads, cold: ~240 MB/s
- Raw 64K reads, hot path (warmer cache): ~307 MB/s
- Warm page-cache speedup: ~4.4× over cold
- libvfs FLIPC fast path vs Mach `fs_read` OOL: ≥ 2× at 16K/64K chunks with 192 KiB prefetch

KVM noise is real — use medians, not best-of. See `docs/bench/`.

---

## Known limitations

- **Graphical QEMU window is read-only.** Interactive `ush` is on the serial console only. Driving the shell from the VGA window (PS/2 keyboard + console TTY) is a v0.2.0 theme.
- **fork() under Mach has rough edges.** It works for the cases the shell needs, but combining `fork()` with `execve()` immediately afterward fails because `task_create` rejects the forked task as a parent (issue #269). Documented in `project_fork_mach_future.md`. v0.2.0 / later will revisit (possibly with spawn-style replacements rather than papering over fork).
- **The trusted base stays static.** Stage-1 servers (bootstrap, default_pager, name_server, cap_server, hal_server, BDS, gpu_server, char_server, ext_server, proc_server, exec_server, ush) are statically linked by design (`project_trusted_base_static.md`). Dynamic linking is fully wired and exercised, but it is the path for *applications*, not the trusted base. Two linkers for two worlds.
- **`device.defs` follow-up audit deferred.** The immediate BDS leak is fixed (#297 + UserDealloc), but the broader sweep across kernel-shared `.defs` is a v0.2.0 task.
- **SMP, x86_64, additional architectures** — none of these are in v0.1.0. They're planned (SMP needs the slab allocator from #80 as a prerequisite; x86_64 brings PCID, MMCONFIG, IOMMU/VT-d). Targeting v0.2.0+.

---

## Upgrade and compatibility

For an alpha-stage OS this section is mostly empty, but a few things are worth noting:

- **Disk format.** The disk image layout (MBR + 3 partitions, ext2 + ext2 + raw swap) is unchanged from v0.0.2. v0.0.2 disks boot under v0.1.0 — though you'll see the new banner and the v0.1.0 servers. The reverse is not supported (v0.1.0 servers require libposix-uros + musl runtime).
- **Bundle format.** Stage-1 bundle layout changed slightly: ush, hello_world, hello_dyn_world, flipc_bench, dlopen_test, libfoo.so, and (optionally) hello_exec/hello_dyn are now written to the disk's `/` by `make-disk-image.sh`. The minimal bundle used by `run-ush.sh` is smaller and excludes the bench utilities.
- **Kernel banner.** Build infrastructure that greps for the old `MkLinux DR3` banner must be updated; the new banner is `UrMach 0.1.0 — Uros microkernel` followed by `Uros 0.1.0 — userspace text path online.` once log_forwarder starts.

---

## What's next — v0.2.0 outlook

Themes under discussion (see the project memory for ongoing discussions):

- **Interactive QEMU window** — `ush` reachable from the graphical console (PS/2 keyboard + VGA tty), so you don't have to drop to the serial console to use Uros.
- **FreeBSD userland port** — `ls`, `cat`, `cp`, `sh`, basic utilities running on top of the v0.1.0 POSIX runtime. The userland reference is FreeBSD (the kernel design will stay independent and capability-first — see `project_unix_personality.md`).
- **Capability system v1** — the modern, performant cap layer that Uros has been targeting since the start (`project_capability_design_reconstruct.md`). v0.1.0 ships `cap_server` + `libcap` as the foundation; v0.2.0 is when the layer becomes load-bearing for application servers.
- **libdl auto-bootstrap** (#162) and per-class module pool maturity (#163).
- **device.defs / BDS audit pass** (#297 follow-ups beyond the immediate fix).
- **Terminal stack** — pipe / PTY with flow control replacing the libgpu_console debug mirror for application stdout (`project_terminal_stack.md`).

Further out: SMP (slab allocator first), x86_64 (PCID + IOMMU), aarch64 / riscv64 ports, and ultimately self-hosting.

---

## Acknowledgements

UrMach evolves OSF Mach, which originated at the Open Software Foundation and Carnegie Mellon University. The `osfmk` source under `uros/` retains its OSF / CMU licenses where they applied to the inherited code. Where musl, FreeBSD, or other permissive upstreams contributed code, attribution is preserved at the file level. New code is MIT-licensed under Alessandro Sangiuliano (Slex).

The bug class that drove #293 → #297 → #298 (and ultimately the kernel-side audit) was a productive thread to pull on — it left the system both correct and better documented for future contributors. That kind of "fix it once, write it down, prevent the next ten" loop is what we want more of going into v0.2.0.
