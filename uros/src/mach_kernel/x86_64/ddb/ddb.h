/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The debugger (#428).
 *
 * Small on purpose, and the smallness is the point: what the port needs is
 * not a full debugger but the difference between a fault that says something
 * and a machine that stops. Registers, a named backtrace, and the ability to
 * look at memory cover almost every question asked of a kernel that has just
 * died, and each of them is a few lines on top of what #428 already built.
 *
 * ── Entered only when asked ───────────────────────────────────────────
 *
 * Behind a boot flag, `-r`, the same one the 32-bit kernel uses. Without it
 * an unhandled fault reports and halts exactly as before, which is what
 * keeps every automated boot ending in a halt instead of a prompt nobody is
 * there to answer. A debugger that seizes the console by default is a
 * debugger that breaks the test loop.
 *
 * ── What it does not do ───────────────────────────────────────────────
 *
 * It does not disassemble. x86-64 adds REX prefixes, changes several opcodes
 * and extends the addressing forms, so a decoder carried over from the
 * 32-bit tree prints something confident and wrong — worse than nothing,
 * because a wrong mnemonic is believed. The fault report prints the bytes at
 * the instruction pointer instead, which any disassembler will take.
 *
 * It does not set breakpoints, single-step, or read another address space.
 * Those want the debug registers, the trap flag, and a notion of "another
 * task" — the first two exist here (#440 armed DR0 to open the swapgs
 * window) and the third does not yet.
 */

#ifndef _X86_64_DDB_DDB_H_
#define _X86_64_DDB_DDB_H_

#include <stdint.h>

struct trap_frame;

/*
 * Read the boot command line and note whether the debugger was asked for.
 *
 * Separate from entering it, because the answer is wanted long before the
 * first fault and the command line is only readable while the boot
 * information is still around.
 */
void ddb_init(uint32_t info_pa);

/* Whether `-r` was on the command line. */
int ddb_enabled(void);

/*
 * Take the console and answer questions about `frame` until told to
 * continue.
 *
 * Returns when the operator says so. Whether continuing is *sensible* is the
 * caller's business: resuming from an unhandled fault usually means taking
 * the same fault again, and the caller that knows this halts instead.
 */
void ddb_enter(struct trap_frame *frame, const char *why);

#endif	/* _X86_64_DDB_DDB_H_ */
