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

## Planned future patches (Phase 2+)

- `arch/i386/syscall_arch.h` — replace `__syscall_*` dispatch with calls
  into `__uros_syscall(n, ...)` provided by libposix-uros.  (Phase 2.)
- `src/internal/libc.h` and friends — adapt thread-pointer / TLS setup
  to Uros if needed.  (Phase 6, with pthread integration.)
