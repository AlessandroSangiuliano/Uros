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

/*
 * Whether the breakpoint that just arrived was a Debugger() request, and if
 * so what it said.  Answers NULL for a breakpoint from anywhere else -- an
 * `int3' in the code under test, or the boot self-test's own -- so the two
 * can never be confused for one another.
 *
 * Consumes: a reason is reported once, to the trap that it belongs to.
 */
const char *ddb_debugger_taken(void);

/*
 * Look for the break character on the console and open the prompt if it is
 * there.  Called from the timer tick, on ONE processor -- the UART is a
 * single device and two readers would take each other's characters.
 *
 * ⚠️ Reaches only a processor whose interrupts are on.  A wedge with IF clear
 * gets no tick and therefore no poll; that door is an NMI and is not this one.
 */
void ddb_poll_console(struct trap_frame *frame);

/*
 * A character from ANY console — serial today, the frame buffer's keyboard
 * next, a user-space gpu_server after that.  Answers TRUE when it was the
 * break and has been taken, so the caller drops it.
 *
 * This is the single place that knows which key opens the debugger.  Every
 * source translates its own input to a character and asks here, so the
 * combination is the same everywhere by construction rather than by three
 * drivers agreeing to agree.
 *
 * ⚠️ It records the request; it does not enter.  The next return from a trap
 * opens the prompt with its own real frame, which is what lets a door exist
 * without being able to produce one — the position user space will be in.
 */
boolean_t ddb_break_char(int c);

/* Open the prompt if ddb_break_char() took a break.  Called from a trap
 * return, which is where a real frame exists. */
void ddb_take_break(struct trap_frame *frame);

/*
 * Park this processor because another one is at the prompt.  Answers TRUE
 * when this NMI was the debugger asking and the processor has been held and
 * released; FALSE when it is a real NMI and belongs to whoever reports those.
 *
 * ⚠️ An NMI and not an ordinary IPI, because the processors worth stopping
 * are the ones not taking interrupts.
 */
boolean_t ddb_park_here(struct trap_frame *frame);

#endif	/* _X86_64_DDB_DDB_H_ */
