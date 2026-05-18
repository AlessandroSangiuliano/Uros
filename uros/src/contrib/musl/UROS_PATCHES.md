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

## Planned future patches (Phase 3+)

- `src/internal/libc.h` and friends — adapt thread-pointer / TLS setup
  to Uros if needed.  (Phase 6, with pthread integration.)
