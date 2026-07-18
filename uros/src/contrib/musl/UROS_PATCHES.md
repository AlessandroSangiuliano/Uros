# Uros patches applied to vendored musl 1.2.5

Source: <https://git.musl-libc.org/git/musl> tag `v1.2.5`
(commit `0784374d561435f7c787a555aeab8ede699ed298`).

Vendored via `git subtree add --prefix=uros/src/contrib/musl ... v1.2.5 --squash`
on 2026-05-18 as part of issue #249 (Phase 1 of #219).

Patches are applied **in-place** in this subtree.  Any change to upstream
files MUST be documented in the table below with: file, rationale, and
the issue that introduced it.  When re-syncing with a newer musl, replay
each entry by hand.

## Patch log

| Date       | File(s)                              | Issue | Why |
|------------|--------------------------------------|-------|-----|
| 2026-05-18 | (none yet)                           | #249  | Phase 1 ships vanilla musl 1.2.5 — no source edits, only out-of-tree build glue. |
| 2026-05-18 | `arch/i386/syscall_arch.h`           | #250  | Replace inline `int $128` / `call *%gs:16` stubs with C-ABI calls to `__uros_syscallN(...)` (provided by libposix-uros).  Drops the vDSO macro block; Uros has its own fast-path mechanism (#236). |
| 2026-05-18 | `arch/i386/pthread_arch.h`           | #251  | Replace `movl %gs:0,%0` TLS read with a load from a global `__uros_tp` pointer.  Single-threaded shim until Phase 6 brings real pthreads + `set_thread_area`. |
| 2026-05-18 | `src/internal/uros_main_thread.c`    | #251  | NEW file.  Defines `struct pthread __uros_main_thread` + `__uros_tp` + `__uros_libc_init()`.  Every musl-linked Uros task points its synthetic thread pointer at this single struct.  __uros_libc_init() is called first thing in main() (libmach_core's crt0 hands control before any libc code runs). |
| 2026-05-18 | `src/internal/uros_main_thread.c`    | #252  | Extend `__uros_libc_init()` to call `__uros_signals_init()` (weak — falls through cleanly for the Phase 1 host-only smoke that doesn't link libposix-uros). |
| 2026-05-19 | `arch/i386/pthread_arch.h`           | #256  | Revert the #251 patch — restore upstream `movl %gs:0, %0`.  Phase 6a installs a real per-thread TLS descriptor via `i386_set_ldt`, so `%gs:0` now resolves correctly. |
| 2026-05-19 | `src/internal/uros_main_thread.c`    | #256  | Drop `__uros_tp`.  Keep `__uros_main_thread` (initialised + installed via set_thread_area). |
| 2026-05-19 | `src/thread/i386/__set_thread_area.s`| #256  | Change the selector-from-entry constant from `3` (Linux GDT, TI=0, RPL=3) to `7` (Uros LDT, TI=1, RPL=3).  Our SYS_set_thread_area returns an LDT slot index, not a GDT slot. |
| 2026-05-19 | `src/thread/i386/clone.s`            | #256  | Replace the int $0x80 path entirely.  __clone now thunks to libposix-uros's `__uros_clone` (C ABI), which does the Mach thread_create + thread_set_state + thread_resume dance directly — Linux clone(2) is a single syscall, on Mach it's several MIG RPCs. |
| 2026-05-20 | `src/internal/uros_main_thread.c`    | #259  | Rewrite `__uros_libc_init()` to drive musl's real `__init_tls` + `__init_ssp` from a synthesized minimal auxv (bootstrap-loaded servers have no Linux-style stack/auxv).  Drops the hand-rolled `__uros_main_thread` TCB, the manual `tls_size`/`tls_align`/circular-list seeding, and `__uros_main_tcb_tp_addr`.  Main TCB is now musl's `builtin_tls`; LDT installed via `__set_thread_area` → libposix-uros `__uros_set_thread_area_tp`.  Enables the stack canary.  No PT_TLS in these binaries, so `__init_tls` skips the phdr walk. |
| 2026-05-20 | `src/thread/i386/__set_thread_area.s`| #259  | Replace the whole int $0x80 body with a tail-call to libposix-uros `__uros_set_thread_area_tp` (same idiom as clone.s).  int $0x80 on Uros is a Mach trap, not the Linux dispatcher, so the old trap never reached our SYS_set_thread_area handler — this stub was effectively dead until `__init_tp` started calling it. |
| 2026-05-22 | `src/internal/uros_syscall_stub.c`   | #234  | NEW file.  WEAK stubs for `__uros_syscall0..6`, `__uros_clone`, `__uros_set_thread_area_tp` (all return `-ENOSYS`).  Phase 7 enables `--enable-shared`, and musl links `libc.so` with `-Wl,--no-undefined`; the real dispatcher lives in the separate libposix-uros archive, which the dynamic linker (== libc.so) cannot depend on.  These weak defs let `libc.so` / `ld-musl-i386.so.1` link standalone (incr 2a); incr 2b bundles the real dispatcher + Mach/VFS stack into libc.so and its strong symbols override these stubs. |

| 2026-05-22 | `Makefile`                           | #234  | Add `EXTRA_OBJS` (empty default) and fold it into the `lib/libc.so` link rule.  Incr 2b feeds our PIC archives (libposix-uros + libmach + libvfs + … built `-fPIC -DUROS_LIBC_UMBRELLA`) here so the dynamic linker (== libc.so) carries the real `__uros_syscall` dispatcher + Mach/VFS stack.  Empty default ⇒ vanilla build unchanged; the Uros CMake passes `EXTRA_OBJS=...` on the make line.  Normal archive semantics pull only the referenced closure.  musl stays the linker of libc.so (keeps `_dlstart`, `--dynamic-list`, `DT_TEXTREL`). |
| 2026-07-10 | `src/thread/pthread_cancel.c`        | #375  | `__syscall_cp_c` now routes cancellation points through the plain `__syscall` (→ patched `__uros_syscallN`) instead of `__syscall_cp_asm`.  That asm issues a raw `int $0x80`, which Uros traps as `EXC_SYSCALL` → SIGSYS (same reason `__set_thread_area.s` was rewritten for #259).  The umbrella `libc.so` always carries this strong gate, so every dynamic tool making a cancellable syscall (nanosleep, read, raw `write`, wait, …) crashed on its first one — surfaced by cpustat's live refresh (`nanosleep`).  Uros has no signal-driven pthread cancellation, so the uncancellable path loses nothing today. |

## Planned future patches (Phase 3+)

- `src/internal/libc.h` and friends — adapt thread-pointer / TLS setup
  to Uros if needed.  (Phase 6, with pthread integration.)
