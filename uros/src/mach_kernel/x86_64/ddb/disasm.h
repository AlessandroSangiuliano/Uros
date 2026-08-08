/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * One x86-64 instruction, read without guessing (#428).
 *
 * Answers its LENGTH in bytes, and writes its mnemonic into `buf'.  Zero
 * means the encoding form is not known, and the caller must stop rather than
 * step by a guess: a wrong length desynchronises the stream and every line
 * after it is a well-formed instruction that was never there.
 *
 * An instruction whose form is known but whose name is not is NOT zero -- it
 * answers its true length and writes `?'.  That distinction is the whole
 * design: one unknown instruction costs one line, not the rest of the dump.
 *
 * ⚠️ Depends on <stdint.h> and nothing else, deliberately.  It is compiled
 * into the kernel and, by scripts/disasm-coverage.sh, into a host program
 * that runs it over every instruction of our own .text and compares the
 * answers with objdump.  A single kernel header here would cost that oracle,
 * and the oracle is what makes adding an instruction a procedure with a
 * verdict rather than a claim.
 */

#ifndef _X86_64_DDB_DISASM_H_
#define _X86_64_DDB_DISASM_H_

#include <stdint.h>

unsigned disasm(const uint8_t *code, unsigned avail, uint64_t addr,
		char *buf, unsigned buflen);

#endif	/* _X86_64_DDB_DISASM_H_ */
