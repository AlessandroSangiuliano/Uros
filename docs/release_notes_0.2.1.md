# Uros v0.2.1 — release notes

**Released:** 2026-07-23
**Kernel:** UrMach 0.2.1 (`URMACH_VERSION_STRING`)
**Target arch:** i386 (SMP, up to `NCPUS=64`)
**Companion docs:** [README.md](../README.md), [CHANGELOG.md](../CHANGELOG.md), [release notes 0.2.0](release_notes_0.2.0.md)

---

## Headline

v0.2.1 is a patch release, and it exists because of what shipping v0.2.0 taught us.

Preparing that release meant driving the system the way a user would rather than the way its test suite does: booting the shipped images on real machines, sitting at the prompt, pressing the keys people press. That turned up three defects the suites had never touched. Pulling on the threads those exposed turned up three more. All six are fixed here; nothing new was added.

The most visible one to anyone using the system: **`^C` now works**. The second most visible: **programs that print and keep working now show their output** instead of appearing to hang.

None of these were regressions from v0.2.0. Every one of them had been there for a long time, quietly, waiting for someone to do the ordinary thing that would expose it.

---

## The theme: suites test what you thought of

Three of the six fixes were found by using the system, not by testing it — and each one had been invisible precisely because the tests approached from a direction that avoided it.

- `sig_test` drives signals directly on the signal port and never calls `sigaction()`. So the `rt_sigaction` ABI bug, which smashed the caller's stack, could not be reached: the first program to call `sigaction()` was the shell, when we asked it to ignore SIGINT.
- Every disk in the test rig is small, so the 28-bit capacity field never saturated. The first real 1 TB disk reported itself as 128 GiB.
- Every terminal check went through code that never asked whether it was talking to a terminal.

The corollary is the honest one: this list is what we found by using it *once*, on a couple of machines, for a couple of days. It is not the bottom of the barrel.

---

## What landed

### 1. The terminal finally behaves like a terminal

**`^C` and `^Z` did nothing** (#397). The ISIG line discipline — the part of a Unix terminal that turns an interrupt character into a signal — **did not exist anywhere in the system**. Not in char_server, not in ush's line reader, not in libposix. The only terminal-driven signals were SIGTTIN/SIGTTOU from the background-access check. An interrupt character just arrived at whoever was reading, as a byte.

The fix puts signal generation in the **input producer** rather than in `tty_read`, and that placement is the whole design decision: while a foreground job is running, *nobody is reading the terminal* — the shell is parked in `waitpid`. If `^C` only took effect on the read path, it would take effect when the job you were trying to interrupt had already finished. Both terminal modules call it, so the serial line and the on-screen VTs behave identically.

The other half is obligatory and easy to forget: **the shell has to protect itself**. With no job running, the foreground process group is the shell's own, so `^C` at an idle prompt would kill the shell outright. ush now ignores SIGINT, SIGQUIT and SIGTSTP, and restores the default disposition in the child before `execve`.

**`kill` did not terminate a fork+exec'd job** (#398). `execve` replaced the task port but left the `signal_port` in place. That port had been registered by the child *before* it exec'd, while it was still running the shell's image — and its receive right died with that image. So `kill` found a non-null port, sent into a dead end, and the signal vanished. libposix maps the resulting error to success, so the caller saw nothing wrong.

A curiosity that turned out to be the diagnostic clue: the *second* `kill` always worked. The pre-existing error path cleared the slot when a send failed, so the next attempt found nothing registered and took the default action. The first `kill` was always wasted.

`proc_S_exec_handoff` now drops the port alongside the task-port swap, which is what POSIX requires anyway — `execve` resets handlers, and those handlers lived in the replaced image. A safety net was added underneath: `proc_kill` and `proc_killpg` apply the POSIX default disposition both when no port is registered *and* when the send fails.

### 2. Storage tells the truth about disks

**Every disk over 128 GiB reported the same size** (#399). Capacity was read from IDENTIFY words 60-61, a 28-bit sector count that the ATA specification **saturates** at `0x0FFFFFFF` for anything larger. Not truncates — saturates, so every large disk reported an identical ~131071 MB. A 1 TB drive and a 4 TB drive were indistinguishable.

The fix reads the 48-bit Max LBA from words 100-103 when the device advertises LBA48. Nothing else needed to change: the I/O path already used the EXT commands and NCQ, so the range was addressable all along — only the *reading* of the capacity was wrong.

One limit remains and is now explicit: sector counts are 32-bit through the block stack, so capacity is **clamped at ~2 TiB with a message** rather than silently truncated. Widening that is an ABI change, not patch-release work.

**A GPT disk was ignored without a word** (#400). A GPT-partitioned disk carries a valid MBR — that is what the protective MBR is for — so it reached the partition walker and matched nothing, because its single entry has type `0xEE`. block_device_server registered zero partitions and exited, ext_server had nothing to mount, and the boot waited forever. On screen it looked like a crash.

Two lines now separate a puzzled photograph from an instant verdict: the layout is named, and the exit says what it implies downstream — *the boot stops here by design, this is not a hang*. Parsing GPT for real is future work, no longer blocked now that #399 has lifted the ceiling the backup header sits behind.

### 3. An ABI class, hunted to the end

The `rt_sigaction` bug (#397, found while fixing `^C`) was the interesting one. musl passes the **kernel's** `struct k_sigaction` — handler, flags, restorer, mask: 20 bytes on i386. libposix declared the parameter as the **POSIX** `struct sigaction`, which is 140 bytes, because its `sigset_t` alone is 128. Asking for the old disposition therefore wrote 140 bytes into a 20-byte buffer: **120 bytes of smashed stack** across musl's own frame, return address included. The symptom was a jump to a null address on the very first `sigaction()` call in a process.

Having found one, we went looking for its siblings, and found `rt_sigprocmask` (#401) doing the same thing one function below — the 128-byte POSIX `sigset_t` aliased over the two-word set musl actually passes. That one never crashed: the overread lands above bit 31, nothing consults those bits, and every buffer musl passes for the *output* mask happens to be full-size. Undefined behaviour with no symptom — and one small buffer away from being the same frame smash.

The rest of the syscall shim was then checked deliberately rather than assumed: `statx` already carries a kernel-shaped struct, `ioctl` carries plain integers, `nanosleep` and the iovec calls are identical under both ABIs. `rt_sigprocmask` was the last of its kind.

Part of why these lasted so long is worth recording: **the two declarations of `__uros_sigprocmask` disagreed with each other** — `const sigset_t *` in one file, `const void *` in the other — and living in separate translation units, no compiler could ever say so.

### 4. Programs can be seen again

**`isatty()` was always false, and every program's stdout was fully buffered** (#402).

musl does not implement `isatty()` on top of `stat()`. It asks the terminal for its window size, and libposix answered every `ioctl` it did not recognise with `-ENOTTY`. So `isatty()` was false for every descriptor, including the console and the serial line.

The consequence that actually bites is in stdio, which decides buffering with the *same* request: when it fails, musl turns off line buffering. **Every musl-linked program on Uros therefore ran with a fully buffered stdout** — output held until the buffer filled or the process exited. A program that printed and then kept working showed *nothing at all*, which on a terminal is indistinguishable from one that has stopped responding.

We had been building around this for a while without naming it. ush reaches for `mach_print` "where printf may not flush". Its banner and prompt go out through `write(tty_fd)` rather than stdio. And a comment above the console `statx` path says console descriptors are reported as character devices "so isatty()/buffering work" — correct and useful, but neither of those two things consults `stat`. The v0.1.0 musl port even anticipated *"line-buffered on tty, block on file"* and left the choice open; the tty case simply never happened.

The request is now answered **per descriptor** — a file or a pipe still has to say no — which meant using the descriptor the handler had been discarding. Window size is the conventional 80x24 for both terminals: a serial line has no geometry, and char_server does not expose the console grid. Reporting the real console size belongs with whatever first needs it.

---

## Verification

Every fix was verified under QEMU against the boot suites — pthread_test 23/23, sig_test 14 PASS / 0 FAIL, cap_test PASS — plus a targeted test for the specific defect. Two of those tests are worth describing, because the defects were of a kind where a careless test passes trivially.

**#399** needed a disk larger than any in the rig. A sparse image gives you one for free: `truncate -s 200G` declares 200 GB while occupying nothing, and QEMU serves it happily. Before the fix the port reported ~131071 MB; after, ~204800 MB.

**#402** needed to distinguish "line buffered" from "fully buffered", and the naive test cannot: both flush at exit, so both look identical. The discriminator was a program that prints and then **never exits**. Under line buffering the newline flushes it; under full buffering it is never seen at all. That produced a clean A/B — without the fix, *not one* of the instrumented program's four output lines ever arrived.

The test harness itself needed correcting along the way. The `^C` script had been matching the string `hello`, which also matches the echo of the `/hello_world` command the script itself types — so its liveness checks could pass without the program ever having run. Checks now key on the shell's own job-exit line, which only appears after a real fork, exec and reap.

---

## Known limitations

Unchanged from v0.2.0 except where noted:

- **Disk capacity is clamped at ~2 TiB** (new in this release, previously a silent wrong answer at 128 GiB). Sector counts are 32-bit through the block stack.
- **GPT is recognised but not parsed.** A GPT disk is named and skipped; bare-metal interactive boot still requires a disk with the Uros MBR layout.
- **Terminal geometry is fixed at 80x24.** Enough for `isatty()` and buffering; full-screen programs will want the real console grid.
- **`termios` is not implemented.** ISIG is always on: a program cannot yet turn `^C` off the way an editor would.
- The v0.2.0 limitations list otherwise still applies.

---

## Upgrade and compatibility

A patch release: no interface changes, no on-disk format changes, no configuration changes. Same build recipe, same boot media layout as v0.2.0.

Two behaviour changes are worth knowing about even though both are corrections:

- **`isatty()` now returns true on terminals**, so musl gives stdout line buffering there. Output becomes more prompt, not less; code that relied on fully buffered stdout on a terminal did so unknowingly.
- **`^C` and `^Z` now generate signals.** A program that previously received those characters as literal input will now be interrupted or stopped instead. This is the standard Unix behaviour that was missing, but it *is* a change in what a running program sees.

---

## What's next

v0.3.0 is the **x86-64 port**: the machine-dependent contracts, PCID, invariant TSC, and the register and address-space room that the 32-bit target does not have. The tree reorganization and the IPC hot-path profiling that gates any further IPC work are queued behind it.

Nothing in this release changes that plan. It only means the v0.2.0 line is one worth standing on while the port is underway.
