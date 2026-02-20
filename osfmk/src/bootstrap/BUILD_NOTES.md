# Bootstrap Server Build Notes

This document records the issues discovered and resolved while completing
the bootstrap server build (Issue #8).  It serves as a reference for
known migcom/MIG bugs and build-system quirks.

## Summary

Converting the bootstrap server from a static library to a fully linked
32-bit ELF executable (`-nostdlib`, entry point `__start_mach`) exposed
a cascade of latent issues across libmach, libcthreads, libsa_mach, and
the shared MIG infrastructure.  Building libraries individually as `.a`
archives hid these problems; only the final link step revealed them.

---

## Issues Found

### 1. MIG-generated header shadowing real userspace headers

**Symptom:** `vm_page_size` and `bootstrap_port` undeclared in libcthreads.

**Root cause:** `add_mig_defs()` placed the MIG-generated `mach.h` (which
contains only RPC stub declarations) into `generated/include/mach.h`.
This shadowed the real userspace `mach.h` from
`src/mach_services/include/mach.h`, which includes `mach_init.h`
(declaring `vm_page_size`, `bootstrap_port`, etc.).

**Fix:** MIG headers are now placed in `generated/` (alongside the `.c`
files), not in `generated/include/`.  The generated `.c` files use
`#include "name.h"` (quotes), so the compiler finds the co-located
header first without polluting the system include path.

---

### 2. MIG: user header vs server header mismatch

**Symptom:** `conflicting types for 'clock_alarm'` — the generated
`clock.h` had 5 parameters while `clock_user.c` expected 4.

**Root cause:** `add_mig_defs()` generated a single header using
`-sheader` (server header).  Both `_user.c` and `_server.c` included
it via `#include "name.h"`.  Server headers have different function
signatures for polymorphic arguments (extra `mach_msg_type_name_t`
parameter).

**Fix:** Generate **both** headers: `-header name.h` (user) and
`-sheader name_server.h` (server).  When both flags are given, migcom
automatically makes `_user.c` include the `-header` file and
`_server.c` include the `-sheader` file.

---

### 3. Kernel .defs vs Export .defs incompatibility

**Symptom:** `alarm_port_t` unknown type; `SEQNOS`-related conflicting
types in clock and memory_object stubs.

**Root cause:** The kernel tree's `.defs` files (`src/mach_kernel/mach/`)
contain kernel-internal definitions (`alarm_port_t`, `SEQNOS`
conditionals, `KERNEL_SERVER` guards) that produce MIG output
incompatible with userspace code.  The original build used the
export tree's `.defs` (`export/powermac/include/`), which are the
published userspace interfaces.

**Fix:** Switched to an explicit list of 16 `.defs` files from the
export tree, matching the original `common.mk` `MIG_DEFS` variable.
No more globbing of `src/mach_kernel/mach/*.defs`.

---

### 4. MIG bug: `prof.defs` fails to parse

**Symptom:** `syntax error` on lines containing `simpleroutine` and
`type sample_array_t` declarations.

**Root cause:** Not fully diagnosed.  `prof.defs` uses `UserPrefix send_`
and `ServerPrefix receive_` with `simpleroutine` declarations.
The same constructs work in isolation but fail when combined with
the full `mach_types.defs` include chain.  May be a parser bug in
migcom related to prefix handling after large type definition blocks.

**Status:** Not in the original `common.mk` MIG_DEFS list, so excluded.
Profiling interfaces are not needed by userspace libraries.

---

### 5. MIG bug: `polymorphic|TYPE` syntax unsupported

**Symptom:** `syntax error` when processing `device_reply.defs`.

**Root cause:** `device_reply.defs` uses the type declaration:
```
type reply_port_t = polymorphic|MACH_MSG_TYPE_PORT_SEND_ONCE
    ctype: mach_port_t;
```
The `|` operator for combining a polymorphic type with a default
message type is not recognized by our migcom's lexer/parser.  The
lexer has no token for `|` (`BAR`/`PIPE`), and the parser grammar
has no production rule for this syntax.

**Status:** `device_reply.defs` excluded.  The bootstrap server uses
synchronous device I/O and does not need async reply stubs.
This is a genuine migcom limitation that should be fixed to support
the full set of MIG interfaces.

---

### 6. MIG bug: `null` (lowercase) emitted in generated code

**Symptom:** `'null' undeclared` in `mach_host_user.c` and
`mach_port_user.c`.

**Root cause:** migcom generates `(void)memcpy(dest, (char *)(null), count)`
in inline array copy code.  The identifier `null` (lowercase) is not
defined anywhere — it should be `NULL`, `0`, or `(void *)0`.

**Affected routines:** `processor_set_policy_control`,
`task_policy`, `thread_policy`, `act_set_state`,
`mach_port_set_attributes`, and similar routines with inline array
parameters.

**Workaround:** `-Dnull=0` added to libmach compile flags.

**Status:** This is a genuine migcom code generation bug.

---

### 7. `ms_*.c` wrappers require sed-based MIG output renaming

**Symptom:** `mig_vm_allocate`, `mig_thread_create`, etc. undefined.

**Root cause:** The original build (in `common.mk`) generates MIG stubs
with normal names (`vm_allocate`), then uses a sed script to rename
ALL function names to `mig_vm_allocate`, `mig_thread_create`, etc.
The `ms_*.c` wrapper files define the public `vm_allocate()` etc.
which call the renamed `mig_*` versions, adding retry logic for
`MACH_SEND_INTERRUPTED` and RPC short-circuit checks.

**Fix:** Excluded all 58 `ms_*.c` wrapper files.  Without the sed
rename, the MIG stubs directly provide the public function names.
Thin wrappers for `thread_switch` and `device_read_overwrite` are
provided in `nostdlib_stubs.c` (calling the corresponding Mach traps).

**Trade-off:** The `ms_*.c` wrappers add `MACH_SEND_INTERRUPTED` retry
logic.  For the bootstrap server (simple, single-threaded, early-boot),
this is unnecessary.  A future full libmach build should implement the
sed rename or use `UserPrefix mig_` in a preprocessing step.

---

### 8. GNU C89 `extern __inline__` requires out-of-line copy

**Symptom:** `cthread_sp` undefined at link time.

**Root cause:** `cthread_sp()` is declared `extern __inline__` in
`i386/cthreads.h`.  In GNU C89 semantics, `extern inline` means
"always inline this, but an out-of-line copy MUST exist elsewhere."
The out-of-line definition in `i386/thread.c` was disabled (`#if 0`).
The original build used an AWK script (`cthread_inline.awk`) to
post-process compiler output and insert inline assembly sequences.

**Fix:** Out-of-line `cthread_sp()` provided in `nostdlib_stubs.c`.

---

### 9. Static link order: circular library dependencies

**Symptom:** `host_get_clock_service` and `clock_sleep` undefined
(from libsa_mach's `sleep.c` referencing libmach symbols).

**Root cause:** With static linking, the linker processes libraries
left-to-right.  `libsa_mach` needs symbols from `libmach`, but if
`libmach` was already processed, those symbols are gone.

**Fix:** Wrap all libraries in `-Wl,--start-group` / `--end-group`
so the linker iterates until all references are resolved.

---

### 10. ELF symbol naming: `__NO_UNDERSCORES__`

**Symptom:** `_cthread_filter`, `_get_cs`, `_get_efl`, `_mach_task_self`
etc. undefined (with underscore prefix).

**Root cause:** The `ENTRY()` macro in `i386/asm.h` uses `EXT(x)` which
prepends `_` to symbol names unless `__NO_UNDERSCORES__` is defined.
ELF (Linux) does not use underscore-prefixed symbols; this was a
Mach-O / a.out convention.

**Fix:** Added `-D__NO_UNDERSCORES__` to assembly preprocessing flags
in libmach, libcthreads, and libsa_mach.

---

## migcom Bugs Summary (for GitHub Issues)

| Bug | Severity | Workaround |
|-----|----------|------------|
| `polymorphic\|TYPE` syntax not supported | Medium | Exclude affected `.defs` |
| `null` (lowercase) emitted instead of `NULL`/`0` | Low | `-Dnull=0` |
| `prof.defs` parse failure with prefix + simpleroutine | Low | Exclude (not needed) |
| User/server header conflation (single `-sheader`) | Design | Use both `-header` and `-sheader` |
