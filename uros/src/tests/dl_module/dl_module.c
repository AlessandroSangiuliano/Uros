/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

/*
 *	dl_module.c — a shared object whose ONLY job is to need one of each
 *	relocation the driver modules use (#423).
 *
 *	The eight real plug-ins between them use exactly three types, counted
 *	on the binaries rather than assumed:
 *
 *		R_386_JUMP_SLOT  82      R_X86_64_JUMP_SLOT
 *		R_386_RELATIVE   59      R_X86_64_RELATIVE
 *		R_386_GLOB_DAT   20      R_X86_64_GLOB_DAT
 *
 *	🔑 Each one below is arranged so that getting it WRONG produces a wrong
 *	answer rather than a crash, and a different wrong answer per type.  A
 *	module that merely loads proves nothing: a relocation applied with the
 *	wrong bias yields a pointer that is valid-looking and off by a load
 *	address, and nothing traps until something follows it.
 *
 *	⚠️ Built with -nostdlib and --unresolved-symbols=ignore-all like the
 *	real modules, so the host supplies dl_host_seed() and the linker leaves
 *	it undefined here -- which is what makes it a PLT call and not a direct
 *	one.
 */

#include <stdint.h>

/* Provided by the loader, deliberately not defined here: an undefined
 * function called from a shared object is what a JUMP_SLOT is for. */
extern uint32_t dl_host_seed(void);

#define DL_MODULE_MAGIC		0x4d4f4431u	/* "MOD1" */

/*
 * RELATIVE: a pointer initialised to the address of something else in this
 * same object.  The linker cannot know the load address, so it emits a
 * RELATIVE entry and the loader must add the base exactly once.  Adding it
 * twice -- which is what treating a RELA entry as REL does -- leaves a
 * plausible pointer to nothing.
 */
static const uint32_t dl_module_answer = DL_MODULE_MAGIC;
const uint32_t *dl_module_answer_ptr = &dl_module_answer;

/*
 * GLOB_DAT: a global the host reads through the object's symbol table.  Its
 * address has to come out of the GOT slot the loader fills.
 */
uint32_t dl_module_global = DL_MODULE_MAGIC ^ 0xffffu;

/*
 * JUMP_SLOT: a call to an undefined external, which goes through the PLT.
 * Returns the seed the host handed back, so a slot filled with the wrong
 * address gives a wrong number rather than a fault -- if it is lucky.
 */
uint32_t
dl_module_call_host(void)
{
	return dl_host_seed() ^ DL_MODULE_MAGIC;
}

/*
 * And the shape the real driver plug-ins use: a struct of function pointers
 * that the server dlsym()s by name.  Every pointer in it is a RELATIVE.
 */
struct dl_module_ops {
	uint32_t	magic;
	uint32_t	(*answer)(void);
	uint32_t	(*call_host)(void);
	const uint32_t	*answer_ptr;
};

static uint32_t
dl_module_answer_fn(void)
{
	return *dl_module_answer_ptr;
}

const struct dl_module_ops dl_test_module_ops = {
	DL_MODULE_MAGIC,
	dl_module_answer_fn,
	dl_module_call_host,
	&dl_module_answer,
};
