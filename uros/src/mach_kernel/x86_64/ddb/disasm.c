/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Reading x86-64 instructions, in stages, without ever guessing (#428).
 *
 * ── The rule that shapes the whole file ───────────────────────────────
 *
 * LENGTH IS NOT THE SAME PROBLEM AS NAMING, and only one of them may fail.
 *
 * A wrong mnemonic costs one wrong line.  A wrong LENGTH desynchronises the
 * stream, and every line after it is a well-formed instruction that was never
 * there -- twenty confident lies from one mistake.  So the two are separated:
 * the length walk is a closed, tabular problem (prefixes, ModRM, SIB,
 * displacement, immediate) and does not need to know what the instruction is,
 * while the name is best-effort and prints `?' when the table has no entry.
 *
 * An unknown instruction therefore costs ONE `?' line and leaves the stream
 * aligned.  An unknown ENCODING FORM -- which is a different thing, and rarer
 * -- returns zero, and the caller must stop rather than step by a guess.
 *
 * ⚠️ REFUSAL IS THE DEFAULT ARM, structurally.  An opcode with no table entry
 * is OP_BAD, which has no length rule, so forgetting to add something is safe
 * rather than silently wrong.  A decoder that guesses is the failure this
 * project keeps designing against; a decoder that declines is not.
 *
 * ── Why this file depends on nothing ──────────────────────────────────
 *
 * <stdint.h> and nothing else.  It is compiled twice: into the kernel, and by
 * scripts/disasm-coverage.sh into a host program that runs it over every
 * instruction of our own .text and compares the answers with objdump.  That
 * oracle is what makes "add an instruction" a procedure with a verdict --
 * coverage rises, disagreements stay at zero -- instead of a claim.  A single
 * kernel header here would cost that.
 */

#include <stdint.h>

#include "disasm.h"

/*
 * What follows the opcode.  These are about ENCODING, not meaning: two
 * instructions that share a form share an entry however different they are.
 */
#define OP_BAD		0x0000	/* not in the table: no length rule at all */
#define OP_NONE		0x0001	/* opcode alone                            */
#define OP_MODRM	0x0002	/* a ModRM byte, and whatever it implies   */
#define OP_IMM8		0x0004
#define OP_IMM16	0x0008
#define OP_IMMZ		0x0010	/* 4 bytes, or 2 with a 0x66 prefix        */
#define OP_IMM64	0x0020	/* only movabs, and only with REX.W        */
#define OP_REL8		0x0040
#define OP_REL32	0x0080
#define OP_MOFFS	0x0100	/* an absolute address, 8 bytes in long mode */

struct opcode {
	uint16_t	form;
	const char	*name;
};

/*
 * One-byte opcodes.  Sparse on purpose: what is not here is OP_BAD, prints
 * `?', and is counted as uncovered rather than approximated.
 *
 * The order of entries is the opcode's, so a reader can check a row against
 * the manual by counting.  Names are the plain mnemonic without operands --
 * operands are a later stage, and printing half of them would be worse than
 * printing none, because a half-formed operand reads as a whole one.
 */
static const struct opcode one[256] = {
	[0x00] = { OP_MODRM, "add" },  [0x01] = { OP_MODRM, "add" },
	[0x02] = { OP_MODRM, "add" },  [0x03] = { OP_MODRM, "add" },
	[0x04] = { OP_IMM8,  "add" },  [0x05] = { OP_IMMZ,  "add" },
	[0x08] = { OP_MODRM, "or"  },  [0x09] = { OP_MODRM, "or"  },
	[0x0A] = { OP_MODRM, "or"  },  [0x0B] = { OP_MODRM, "or"  },
	[0x0C] = { OP_IMM8,  "or"  },  [0x0D] = { OP_IMMZ,  "or"  },
	[0x10] = { OP_MODRM, "adc" },  [0x11] = { OP_MODRM, "adc" },
	[0x12] = { OP_MODRM, "adc" },  [0x13] = { OP_MODRM, "adc" },
	[0x14] = { OP_IMM8,  "adc" },  [0x15] = { OP_IMMZ,  "adc" },
	[0x18] = { OP_MODRM, "sbb" },  [0x19] = { OP_MODRM, "sbb" },
	[0x1A] = { OP_MODRM, "sbb" },  [0x1B] = { OP_MODRM, "sbb" },
	[0x1C] = { OP_IMM8,  "sbb" },  [0x1D] = { OP_IMMZ,  "sbb" },
	[0x20] = { OP_MODRM, "and" },  [0x21] = { OP_MODRM, "and" },
	[0x22] = { OP_MODRM, "and" },  [0x23] = { OP_MODRM, "and" },
	[0x24] = { OP_IMM8,  "and" },  [0x25] = { OP_IMMZ,  "and" },
	[0x28] = { OP_MODRM, "sub" },  [0x29] = { OP_MODRM, "sub" },
	[0x2A] = { OP_MODRM, "sub" },  [0x2B] = { OP_MODRM, "sub" },
	[0x2C] = { OP_IMM8,  "sub" },  [0x2D] = { OP_IMMZ,  "sub" },
	[0x30] = { OP_MODRM, "xor" },  [0x31] = { OP_MODRM, "xor" },
	[0x32] = { OP_MODRM, "xor" },  [0x33] = { OP_MODRM, "xor" },
	[0x34] = { OP_IMM8,  "xor" },  [0x35] = { OP_IMMZ,  "xor" },
	[0x38] = { OP_MODRM, "cmp" },  [0x39] = { OP_MODRM, "cmp" },
	[0x3A] = { OP_MODRM, "cmp" },  [0x3B] = { OP_MODRM, "cmp" },
	[0x3C] = { OP_IMM8,  "cmp" },  [0x3D] = { OP_IMMZ,  "cmp" },

	[0x50] = { OP_NONE, "push" }, [0x51] = { OP_NONE, "push" },
	[0x52] = { OP_NONE, "push" }, [0x53] = { OP_NONE, "push" },
	[0x54] = { OP_NONE, "push" }, [0x55] = { OP_NONE, "push" },
	[0x56] = { OP_NONE, "push" }, [0x57] = { OP_NONE, "push" },
	[0x58] = { OP_NONE, "pop"  }, [0x59] = { OP_NONE, "pop"  },
	[0x5A] = { OP_NONE, "pop"  }, [0x5B] = { OP_NONE, "pop"  },
	[0x5C] = { OP_NONE, "pop"  }, [0x5D] = { OP_NONE, "pop"  },
	[0x5E] = { OP_NONE, "pop"  }, [0x5F] = { OP_NONE, "pop"  },

	[0x63] = { OP_MODRM, "movslq" },
	[0x68] = { OP_IMMZ,  "push" },
	[0x69] = { OP_MODRM | OP_IMMZ, "imul" },
	[0x6A] = { OP_IMM8,  "push" },
	[0x6B] = { OP_MODRM | OP_IMM8, "imul" },

	[0x70] = { OP_REL8, "jo"  }, [0x71] = { OP_REL8, "jno" },
	[0x72] = { OP_REL8, "jb"  }, [0x73] = { OP_REL8, "jae" },
	[0x74] = { OP_REL8, "je"  }, [0x75] = { OP_REL8, "jne" },
	[0x76] = { OP_REL8, "jbe" }, [0x77] = { OP_REL8, "ja"  },
	[0x78] = { OP_REL8, "js"  }, [0x79] = { OP_REL8, "jns" },
	[0x7A] = { OP_REL8, "jp"  }, [0x7B] = { OP_REL8, "jnp" },
	[0x7C] = { OP_REL8, "jl"  }, [0x7D] = { OP_REL8, "jge" },
	[0x7E] = { OP_REL8, "jle" }, [0x7F] = { OP_REL8, "jg"  },

	/*
	 * The group opcodes.  The mnemonic lives in ModRM.reg and this stage
	 * does not read it -- the LENGTH does not depend on which member of
	 * the group it is, which is the only part that must be right.  They
	 * are named for the group so the reader knows what was not resolved.
	 */
	[0x80] = { OP_MODRM | OP_IMM8, "grp1" },
	[0x81] = { OP_MODRM | OP_IMMZ, "grp1" },
	[0x83] = { OP_MODRM | OP_IMM8, "grp1" },

	[0x84] = { OP_MODRM, "test" }, [0x85] = { OP_MODRM, "test" },
	[0x86] = { OP_MODRM, "xchg" }, [0x87] = { OP_MODRM, "xchg" },
	[0x88] = { OP_MODRM, "mov"  }, [0x89] = { OP_MODRM, "mov"  },
	[0x8A] = { OP_MODRM, "mov"  }, [0x8B] = { OP_MODRM, "mov"  },
	[0x8D] = { OP_MODRM, "lea"  },
	[0x8F] = { OP_MODRM, "pop"  },

	[0x90] = { OP_NONE, "nop" },
	[0x98] = { OP_NONE, "cltq" },
	[0x99] = { OP_NONE, "cqto" },
	[0x9C] = { OP_NONE, "pushf" },
	[0x9D] = { OP_NONE, "popf" },

	[0xA8] = { OP_IMM8, "test" }, [0xA9] = { OP_IMMZ, "test" },

	[0xB0] = { OP_IMM8, "mov" }, [0xB1] = { OP_IMM8, "mov" },
	[0xB2] = { OP_IMM8, "mov" }, [0xB3] = { OP_IMM8, "mov" },
	[0xB4] = { OP_IMM8, "mov" }, [0xB5] = { OP_IMM8, "mov" },
	[0xB6] = { OP_IMM8, "mov" }, [0xB7] = { OP_IMM8, "mov" },
	/*
	 * B8-BF take an immediate the width of the operand: four bytes, or
	 * EIGHT with REX.W, which is the `movabs' form.  OP_IMM64 marks the
	 * dependency; the walk resolves it.
	 */
	[0xB8] = { OP_IMM64, "mov" }, [0xB9] = { OP_IMM64, "mov" },
	[0xBA] = { OP_IMM64, "mov" }, [0xBB] = { OP_IMM64, "mov" },
	[0xBC] = { OP_IMM64, "mov" }, [0xBD] = { OP_IMM64, "mov" },
	[0xBE] = { OP_IMM64, "mov" }, [0xBF] = { OP_IMM64, "mov" },

	[0xC0] = { OP_MODRM | OP_IMM8, "grp2" },
	[0xC1] = { OP_MODRM | OP_IMM8, "grp2" },
	[0xC2] = { OP_IMM16, "ret" },
	[0xC3] = { OP_NONE,  "ret" },
	[0xC6] = { OP_MODRM | OP_IMM8, "mov" },
	[0xC7] = { OP_MODRM | OP_IMMZ, "mov" },
	[0xC9] = { OP_NONE, "leave" },
	[0xCC] = { OP_NONE, "int3" },
	[0xCF] = { OP_NONE, "iret" },

	[0xD0] = { OP_MODRM, "grp2" }, [0xD1] = { OP_MODRM, "grp2" },
	[0xD2] = { OP_MODRM, "grp2" }, [0xD3] = { OP_MODRM, "grp2" },

	[0xE8] = { OP_REL32, "call" },
	[0xE9] = { OP_REL32, "jmp"  },
	[0xEB] = { OP_REL8,  "jmp"  },

	[0xF4] = { OP_NONE, "hlt" },
	/*
	 * F6/F7 are a group whose FIRST member takes an immediate and whose
	 * others do not -- test vs not/neg/mul/div.  The length therefore
	 * depends on ModRM.reg, which is the one place in this file where it
	 * does, and the walk asks for it rather than assuming either way.
	 */
	[0xF6] = { OP_MODRM, "grp3" },
	[0xF7] = { OP_MODRM, "grp3" },
	[0xFA] = { OP_NONE, "cli" },
	[0xFB] = { OP_NONE, "sti" },
	[0xFE] = { OP_MODRM, "grp4" },
	[0xFF] = { OP_MODRM, "grp5" },
};

/* Two-byte opcodes, after 0x0F. */
static const struct opcode two[256] = {
	[0x05] = { OP_NONE,  "syscall" },
	[0x07] = { OP_NONE,  "sysret" },
	[0x0B] = { OP_NONE,  "ud2" },
	[0x1E] = { OP_MODRM, "endbr" },
	[0x1F] = { OP_MODRM, "nop" },
	[0x20] = { OP_MODRM, "mov" },	/* from control register */
	[0x22] = { OP_MODRM, "mov" },	/* to control register   */
	[0x23] = { OP_MODRM, "mov" },	/* to debug register     */
	[0x30] = { OP_NONE,  "wrmsr" },
	[0x31] = { OP_NONE,  "rdtsc" },
	[0x32] = { OP_NONE,  "rdmsr" },
	[0x40] = { OP_MODRM, "cmovo"  }, [0x41] = { OP_MODRM, "cmovno" },
	[0x42] = { OP_MODRM, "cmovb"  }, [0x43] = { OP_MODRM, "cmovae" },
	[0x44] = { OP_MODRM, "cmove"  }, [0x45] = { OP_MODRM, "cmovne" },
	[0x46] = { OP_MODRM, "cmovbe" }, [0x47] = { OP_MODRM, "cmova"  },
	[0x48] = { OP_MODRM, "cmovs"  }, [0x49] = { OP_MODRM, "cmovns" },
	[0x4C] = { OP_MODRM, "cmovl"  }, [0x4D] = { OP_MODRM, "cmovge" },
	[0x4E] = { OP_MODRM, "cmovle" }, [0x4F] = { OP_MODRM, "cmovg"  },
	[0x80] = { OP_REL32, "jo"  }, [0x81] = { OP_REL32, "jno" },
	[0x82] = { OP_REL32, "jb"  }, [0x83] = { OP_REL32, "jae" },
	[0x84] = { OP_REL32, "je"  }, [0x85] = { OP_REL32, "jne" },
	[0x86] = { OP_REL32, "jbe" }, [0x87] = { OP_REL32, "ja"  },
	[0x88] = { OP_REL32, "js"  }, [0x89] = { OP_REL32, "jns" },
	[0x8A] = { OP_REL32, "jp"  }, [0x8B] = { OP_REL32, "jnp" },
	[0x8C] = { OP_REL32, "jl"  }, [0x8D] = { OP_REL32, "jge" },
	[0x8E] = { OP_REL32, "jle" }, [0x8F] = { OP_REL32, "jg"  },
	[0x90] = { OP_MODRM, "seto"  }, [0x91] = { OP_MODRM, "setno" },
	[0x92] = { OP_MODRM, "setb"  }, [0x93] = { OP_MODRM, "setae" },
	[0x94] = { OP_MODRM, "sete"  }, [0x95] = { OP_MODRM, "setne" },
	[0x96] = { OP_MODRM, "setbe" }, [0x97] = { OP_MODRM, "seta"  },
	[0x98] = { OP_MODRM, "sets"  }, [0x99] = { OP_MODRM, "setns" },
	[0x9C] = { OP_MODRM, "setl"  }, [0x9D] = { OP_MODRM, "setge" },
	[0x9E] = { OP_MODRM, "setle" }, [0x9F] = { OP_MODRM, "setg"  },
	[0xA2] = { OP_NONE,  "cpuid" },
	[0xA3] = { OP_MODRM, "bt"  },
	[0xAB] = { OP_MODRM, "bts" },
	[0xAF] = { OP_MODRM, "imul" },
	[0xB0] = { OP_MODRM, "cmpxchg" }, [0xB1] = { OP_MODRM, "cmpxchg" },
	[0xB3] = { OP_MODRM, "btr" },
	[0xB6] = { OP_MODRM, "movzbl" }, [0xB7] = { OP_MODRM, "movzwl" },
	[0xBA] = { OP_MODRM | OP_IMM8, "grp8" },
	[0xBB] = { OP_MODRM, "btc" },
	[0xBC] = { OP_MODRM, "bsf" }, [0xBD] = { OP_MODRM, "bsr" },
	[0xBE] = { OP_MODRM, "movsbl" }, [0xBF] = { OP_MODRM, "movswl" },
	[0xC0] = { OP_MODRM, "xadd" }, [0xC1] = { OP_MODRM, "xadd" },
	[0xC7] = { OP_MODRM, "grp9" },	/* cmpxchg16b and friends */
};

/*
 * The groups, whose mnemonic lives in ModRM.reg rather than in the opcode.
 *
 * ⚠️ Resolved for the NAME only.  The length of every member is the same --
 * with the single exception of F6/F7, handled where it happens -- which is
 * why the walk never needed this and why getting it wrong costs one line
 * rather than the dump.  A null entry is a member that does not exist and
 * prints `?', which is a real answer: `grp5' reg 7 is not an instruction.
 */
static const char * const grp1[8] = {
	"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"
};
static const char * const grp2[8] = {
	"rol", "ror", "rcl", "rcr", "shl", "shr", 0, "sar"
};
static const char * const grp3[8] = {
	"test", "test", "not", "neg", "mul", "imul", "div", "idiv"
};
static const char * const grp4[8] = {
	"inc", "dec", 0, 0, 0, 0, 0, 0
};
static const char * const grp5[8] = {
	"inc", "dec", "call", "lcall", "jmp", "ljmp", "push", 0
};
static const char * const grp8[8] = {
	0, 0, 0, 0, "bt", "bts", "btr", "btc"
};
static const char * const grp9[8] = {
	0, "cmpxchg16b", 0, 0, 0, 0, "rdrand", "rdseed"
};

static const char *group_member(const char *name, unsigned reg)
{
	const char * const *tbl = 0;

	if (name[0] != 'g' || name[1] != 'r' || name[2] != 'p')
		return name;

	switch (name[3]) {
	case '1': tbl = grp1; break;
	case '2': tbl = grp2; break;
	case '3': tbl = grp3; break;
	case '4': tbl = grp4; break;
	case '5': tbl = grp5; break;
	case '8': tbl = grp8; break;
	case '9': tbl = grp9; break;
	default:  return name;
	}

	return tbl[reg] ? tbl[reg] : "?";
}

static void put(char *buf, unsigned len, unsigned *at, const char *s)
{
	while (*s && *at + 1 < len)
		buf[(*at)++] = *s++;
	buf[*at] = 0;
}

unsigned disasm(const uint8_t *code, unsigned avail, uint64_t addr,
		char *buf, unsigned buflen)
{
	unsigned	n = 0;		/* bytes consumed so far          */
	unsigned	opsize = 4;	/* 4, or 2 with 0x66, or 8 REX.W  */
	int		rex_w = 0;
	int		rex_b = 0;
	int		saw_66 = 0;
	int		saw_f3 = 0;
	unsigned	at = 0;
	const struct opcode *op;
	uint16_t	form;

	(void) addr;
	buf[0] = 0;

	/*
	 * Prefixes.  Bounded by the architecture's own limit: an instruction
	 * is at most fifteen bytes, so a run of prefixes longer than that is
	 * not a long instruction, it is not an instruction.
	 */
	for (;;) {
		uint8_t b;

		if (n >= avail || n >= 15)
			return 0;
		b = code[n];

		if (b == 0x66) { opsize = 2; saw_66 = 1; n++; continue; }
		if (b == 0xF3) { saw_f3 = 1; n++; continue; }
		if (b == 0x67 || b == 0xF0 || b == 0xF2
		    || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26
		    || b == 0x64 || b == 0x65) {
			n++;
			continue;
		}
		/*
		 * ⚠️ REX must be the LAST prefix before the opcode, so this
		 * consumes it and stops looking.  A decoder that kept scanning
		 * would accept 0x40 0x66 as a valid pair, which the processor
		 * does not.
		 */
		if ((b & 0xF0) == 0x40) {
			rex_w = (b & 0x08) != 0;
			rex_b = (b & 0x01) != 0;
			n++;
		}
		break;
	}

	if (rex_w)
		opsize = 8;

	if (n >= avail)
		return 0;

	if (code[n] == 0x0F) {
		n++;
		if (n >= avail)
			return 0;
		op = &two[code[n]];

		/*
		 * Two more that an F3 prefix renames rather than modifies.
		 * The prefix is not decoration here: bsf and tzcnt differ on a
		 * zero input, which is exactly when somebody is reading a
		 * disassembly to find out why.
		 */
		if (saw_f3 && code[n] == 0xBC)
			op = &(const struct opcode){ OP_MODRM, "tzcnt" };
		else if (saw_f3 && code[n] == 0xBD)
			op = &(const struct opcode){ OP_MODRM, "lzcnt" };
	} else {
		op = &one[code[n]];
	}
	n++;

	form = op->form;
	if (form == OP_BAD) {
		/*
		 * No length rule, so there is nothing honest to say about
		 * where the next instruction starts.  The caller stops.
		 */
		put(buf, buflen, &at, "?");
		return 0;
	}

	/*
	 * ⚠️ 0x90 is `nop' only WITHOUT REX.B.  With it the instruction is
	 * `xchg %r8, %rax', which is not a no-op at all -- and a debugger
	 * that showed a register exchange as a nop would be the exact failure
	 * this file is built to avoid, in the one opcode a reader trusts most.
	 * The length is the same either way, which is why it took the oracle
	 * to notice.
	 */
	/*
	 * ⚠️ 0x90 is `nop' only ALONE.  With REX.B it is `xchg %r8, %rax', and
	 * with a 0x66 prefix it is `xchg %ax, %ax' -- both real exchanges.  A
	 * debugger that showed a register exchange as a no-op would be the
	 * exact failure this file exists to avoid, in the one opcode a reader
	 * trusts without looking.  The length is the same either way, which is
	 * why nothing but the oracle would have found it.
	 */
	if ((form & OP_MODRM) == 0) {
		const char *name = op->name;

		/*
		 * ⚠️ 0x90 is `nop' only ALONE.  With REX.B it is
		 * `xchg %r8, %rax', with 0x66 it is `xchg %ax, %ax', and with
		 * an F3 prefix it is `pause' -- three different instructions
		 * behind the one opcode a reader trusts without looking, and a
		 * debugger that called any of them a no-op would be lying in
		 * the most expensive place.  The length is the same for all
		 * four, which is why only the oracle could find it.
		 */
		/*
		 * Two whose name IS their operand size: 0x98 and 0x99 sign-
		 * extend, and what they extend depends on REX.W.  Calling a
		 * 32-bit widening a 64-bit one reads as an explanation for a
		 * value that is wrong in exactly the upper half.
		 */
		if (op == &one[0x98])
			name = rex_w ? "cltq" : "cwtl";
		else if (op == &one[0x99])
			name = rex_w ? "cqto" : "cltd";

		if (op == &one[0x90]) {
			if (saw_f3)
				name = "pause";
			else if (rex_b || saw_66)
				name = "xchg";
		}

		put(buf, buflen, &at, name);
	}

	if (form & OP_MODRM) {
		uint8_t modrm, mod, rm, reg;

		if (n >= avail)
			return 0;
		modrm = code[n++];
		mod = modrm >> 6;
		reg = (modrm >> 3) & 7;
		rm = modrm & 7;

		/*
		 * ⚠️ The one place the LENGTH depends on which member of a
		 * group it is: F6/F7 reg 0 and 1 are `test' and take an
		 * immediate; the rest do not.
		 */
		if (op->name[3] == '3' && op->name[0] == 'g' && reg <= 1)
			form |= (code[n - 2] == 0xF6) ? OP_IMM8 : OP_IMMZ;

		/* Now the name can be known: it was in this byte. */
		put(buf, buflen, &at, group_member(op->name, reg));

		if (mod != 3) {
			if (rm == 4) {		/* SIB */
				uint8_t sib;

				if (n >= avail)
					return 0;
				sib = code[n++];
				/* base 5 with mod 0 means a 32-bit displacement */
				if (mod == 0 && (sib & 7) == 5)
					n += 4;
			} else if (mod == 0 && rm == 5) {
				n += 4;		/* RIP-relative */
			}
			if (mod == 1)
				n += 1;
			else if (mod == 2)
				n += 4;
		}
	}

	if (form & OP_IMM8)   n += 1;
	if (form & OP_IMM16)  n += 2;
	if (form & OP_IMMZ)   n += (opsize == 2) ? 2 : 4;
	if (form & OP_IMM64)  n += (opsize == 8) ? 8 : (opsize == 2 ? 2 : 4);
	if (form & OP_REL8)   n += 1;
	if (form & OP_REL32)  n += (opsize == 2) ? 2 : 4;
	if (form & OP_MOFFS)  n += 8;

	if (n > avail || n > 15)
		return 0;

	return n;
}
