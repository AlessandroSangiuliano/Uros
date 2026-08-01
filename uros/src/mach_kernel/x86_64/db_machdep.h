/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/db_machdep.h> for x86-64 (#428).
 *
 * What DDB has to be told about a machine: how wide an address and an
 * expression are, where the registers live, how to plant and step over a
 * breakpoint, and how to recognise the instructions that end a frame.
 *
 * It blocks more than DDB.  kern/lock.c reaches it through <ddb/db_command.h>
 * and is otherwise ready to compile for this target, and twelve other
 * machine-independent sources stop on this one name -- which is why it is
 * worth writing before the rest of #428.
 *
 * ── The one decision that is not a transliteration ───────────────────
 *
 * db_expr_t is `int` on i386 and `long` here.
 *
 * An expression in the debugger holds whatever the operator types, and what
 * the operator types is usually an address.  At 32 bits both fit in an int
 * and nobody had to think about it.  Here an address does not: `int` would
 * silently truncate every symbol above 4 GiB -- which on this machine means
 * the whole kernel, since the image is linked in the top 2 GiB of a 64-bit
 * space.  Every examine and every breakpoint would land somewhere else, and
 * the debugger would report the address it had truncated to, so the display
 * would agree with itself and disagree with the machine.
 *
 * That is the worst shape a debugger bug can take: the tool you reach for
 * when you distrust the system becomes the thing lying to you.
 */

#ifndef _X86_64_DB_MACHDEP_H_
#define _X86_64_DB_MACHDEP_H_

#include <mach/machine/vm_types.h>
#include <mach/machine/vm_param.h>
#include <mach/boolean.h>
#include <trap/trap.h>

typedef	vm_offset_t	db_addr_t;	/* address -- unsigned, 64 bits    */
typedef	long		db_expr_t;	/* expression -- signed, and wide
					   enough for an address (above)   */

typedef struct trap_frame db_regs_t;

extern db_regs_t	ddb_regs;	/* register state                  */
#define	DDB_REGS	(&ddb_regs)

extern int		db_active;	/* ddb is running                  */

#define	PC_REGS(regs)	((db_addr_t)(regs)->rip)

/*
 * int3.  One byte, as on i386 -- the encoding did not change, and neither
 * did the fact that it is the only breakpoint that fits where any
 * instruction might start.
 */
#define	BKPT_INST	0xcc
#define	BKPT_SIZE	(1)
#define	BKPT_SET(inst)	(BKPT_INST)

/*
 * int3 traps with rip *after* the byte, so the reported address is one past
 * the breakpoint and has to be walked back before anything is reported to
 * the operator.
 */
#define	FIXUP_PC_AFTER_BREAK	ddb_regs.rip -= 1;

/*
 * Single-step is the trap flag in RFLAGS, bit 8 -- named here rather than
 * written as 0x100 at each use, because a bare constant in a flags word is
 * exactly the kind of thing that gets copied into the wrong flags word.
 */
#define	RFLAGS_TF	0x00000100UL

#define	db_clear_single_step(regs)	((regs)->rflags &= ~RFLAGS_TF)
#define	db_set_single_step(regs)	((regs)->rflags |=  RFLAGS_TF)

#define	IS_BREAKPOINT_TRAP(type, code)	((type) == T_BREAKPOINT)
#define	IS_WATCHPOINT_TRAP(type, code)	((type) == T_DEBUG)

/*
 * The instructions that end a frame, for the backtrace walker.
 *
 * The opcodes are the same bytes as on i386: long mode changed the operand
 * size of these, not their encoding.  `iret` is written by the assembler as
 * `iretq` here and is still 0xcf -- the REX.W prefix in front of it is what
 * makes it 64-bit, and the prefix is not what these tests look at.
 *
 * ⚠️ Only the one-byte forms are recognised, as on i386.  A call through a
 * register with a REX prefix is 0x41 0xff, so `inst_call` says no to it, and
 * the walker treats such a frame as a leaf.  That is the existing behaviour
 * and not a regression, but it is a real limit and it belongs written down
 * rather than discovered from a short backtrace.
 */
#define	I_CALL		0xe8
#define	I_CALLI		0xff
#define	I_RET		0xc3
#define	I_IRET		0xcf

#define	inst_trap_return(ins)	(((ins)&0xff) == I_IRET)
#define	inst_return(ins)	(((ins)&0xff) == I_RET)
#define	inst_call(ins)		(((ins)&0xff) == I_CALL ||		\
				 (((ins)&0xff) == I_CALLI &&		\
				  ((ins)&0x3800) == 0x1000))
#define	inst_load(ins)		0
#define	inst_store(ins)		0

/*
 * DDB may look at any address space.
 */
#define	DB_ACCESS_LEVEL		2

#define	DB_VALID_KERN_ADDR(addr)					\
	((addr) >= VM_MIN_KERNEL_ADDRESS)

#define	DB_VALID_ADDRESS(addr, user)					\
	((!(user) && DB_VALID_KERN_ADDR(addr)) ||			\
	 ((user) && (addr) < VM_MAX_ADDRESS))

#define	DB_CHECK_ACCESS(addr, size, task)				\
	db_check_access(addr, size, task)

#define	DB_PHYS_EQ(task1, addr1, task2, addr2)				\
	db_phys_eq(task1, addr1, task2, addr2)

/*
 * A trap from ring 3: the saved code segment's privilege level says so, and
 * it is the segment rather than the address that decides -- a kernel address
 * in rip proves nothing about where the trap came from.
 */
#define	IS_USER_TRAP(regs, etext)	(((regs)->cs & 3) != 0)

extern boolean_t	db_check_access(vm_offset_t addr, int size,
					task_t task);
extern boolean_t	db_phys_eq(task_t task1, vm_offset_t addr1,
				   task_t task2, vm_offset_t addr2);

#endif /* _X86_64_DB_MACHDEP_H_ */
