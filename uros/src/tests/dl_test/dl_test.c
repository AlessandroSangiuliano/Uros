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
 *	dl_test.c — does libdl relocate a shared object on this machine (#423)
 *
 *	NOT the same subject as tests/dlopen_test, which goes through musl's
 *	ld-musl and belongs to #234.  This one uses libdl -- the loader the
 *	driver plug-ins go through -- and asks the question four servers are
 *	waiting on: hal_server, block_device_server, char_server and gpu_server
 *	all load their modules this way.
 *
 *	🔑 Each arm is written so a WRONG relocation gives a wrong ANSWER where
 *	it can.  "The module loaded" proves nothing on its own: an unapplied
 *	RELATIVE leaves a pointer that still looks like a pointer, and nothing
 *	traps until something follows it -- by which time the loader is not
 *	what gets blamed.
 *
 *	⚠️ Where it can, and not everywhere: arms [3] and [4] have to CALL
 *	through relocated pointers, and a wrong one there ends the task rather
 *	than the arm.  They are gated on the arms that can check a pointer's
 *	value without following it, which is why those come first.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <stdio.h>
#include <string.h>

#include "dlfcn.h"
#include "dl_internal.h"

#define DL_MODULE_MAGIC		0x4d4f4431u
#define DL_HOST_SEED		0x5eed0423u

#define MODULE_PATH		"/dl_module.so"

/*
 * The module, compiled in.  See this test's CMakeLists for why it travels
 * inside the binary rather than being fetched from bootstrap's module pool:
 * the pool reads off a disk stage that x86-64 does not have, and a relocation
 * does not know where its bytes came from.
 */
extern const unsigned char	dl_module_blob[];
extern const unsigned int	dl_module_blob_size;

static int
blob_read_file(const char *path, void **buf, unsigned int *size)
{
	if (strcmp(path, MODULE_PATH) != 0)
		return -1;
	/*
	 * ⚠️ Handed out as-is, not copied.  libdl's contract is that the buffer
	 * only has to outlive dl_map_object(), and dl_api.c calls free_buf
	 * right after it -- so free_buf below must not free something static.
	 */
	*buf = (void *)(uintptr_t)dl_module_blob;
	*size = dl_module_blob_size;
	return 0;
}

static void
blob_free_buf(void *buf, unsigned int size)
{
	(void)buf;
	(void)size;
}

static const struct dl_io_ops blob_io_ops = {
	.read_file = blob_read_file,
	.free_buf  = blob_free_buf,
};

/*
 * The undefined symbol the module calls through its PLT.  It is here, in the
 * program that loads the module, which is what makes the module's call a
 * JUMP_SLOT the loader has to fill -- a direct call inside the module would
 * need no relocation at all and would prove nothing.
 */
uint32_t
dl_host_seed(void)
{
	return DL_HOST_SEED;
}

struct dl_module_ops {
	uint32_t	magic;
	uint32_t	(*answer)(void);
	uint32_t	(*call_host)(void);
	const uint32_t	*answer_ptr;
};

static int passed;
static int failed;

static void
arm(const char *name, int ok, const char *detail,
    unsigned int got, unsigned int want)
{
	if (ok) {
		printf("dl_test: %s: OK\n", name);
		passed++;
		return;
	}
	printf("dl_test: %s: WRONG — %s (got 0x%08x, want 0x%08x)\n",
	       name, detail, got, want);
	failed++;
}

int
main(int argc, char **argv)
{
	void *h;
	const struct dl_module_ops *ops;
	const uint32_t **answer_ptr;

	(void)argc;
	(void)argv;

	printf("dl_test: starting (#423) — libdl, not ld-musl\n");

	dl_set_io_ops(&blob_io_ops);

	/*
	 * The module calls back into this program, so this program's own
	 * symbols have to be findable.  ⚠️ Its failure is reported and the run
	 * continues: arms [1] and [2] need no host symbol at all, and stopping
	 * here would hide which half is broken.
	 */
	if (dl_bootstrap_self() != 0)
		printf("dl_test: WRONG — dl_bootstrap_self failed: %s "
		       "(arm [3] cannot resolve and will say so)\n",
		       dlerror());

	h = dlopen(MODULE_PATH, RTLD_NOW);
	if (h == NULL) {
		printf("dl_test: WRONG — dlopen(%s) failed: %s\n",
		       MODULE_PATH, dlerror());
		printf("dl_test: 0 of 4 arms passed\n");
		return 1;
	}
	printf("dl_test: [0] the module mapped and relocated\n");

	/*
	 * ⚠️ The arms are named for the relocation each one actually reaches,
	 * which took reading the module's own relocation table to get right.
	 * The first version called arm [1] "GLOB_DAT" because it fetched a
	 * global -- but dlsym() resolves through the symbol table and the load
	 * bias, and touches no GOT slot at all.  The single R_X86_64_GLOB_DAT
	 * in dl_module.so is for dl_module_answer_ptr, and the code that goes
	 * through it is the module reading that pointer: ops->answer().
	 *
	 * 🔑 An arm named for something it does not exercise is worse than a
	 * missing arm, because it reports a pass for it.
	 */

	/*
	 * [1] RELATIVE — a pointer variable in the module initialised to the
	 * address of another object in the same module.  The linker emitted a
	 * base-relative entry; the loader must add the load address exactly
	 * once.  Applied twice, or not at all, the pointer is not inside the
	 * module and what it names is not the magic.
	 */
	answer_ptr = (const uint32_t **)dlsym(h, "dl_module_answer_ptr");
	if (answer_ptr == NULL || *answer_ptr == NULL) {
		arm("[1] RELATIVE", 0, "dlsym(dl_module_answer_ptr) gave nothing",
		    0, DL_MODULE_MAGIC);
	} else {
		arm("[1] RELATIVE", **answer_ptr == DL_MODULE_MAGIC,
		    "the base was applied the wrong number of times",
		    (unsigned)**answer_ptr, DL_MODULE_MAGIC);
	}

	/*
	 * [2] The ops table — the shape the driver plug-ins use.  Its magic
	 * needs no relocation and each of its pointers needs one.
	 *
	 * 🔥 Checked by COMPARING the pointer, not by following it.  The first
	 * version read `*ops->answer_ptr' to see whether it named the magic --
	 * which is the same mistake as the guard below it, one field over: an
	 * unrelocated pointer holds a link-time address, and dereferencing it
	 * is either a fault or a garbage read, neither of which is a verdict.
	 * It cost a run whose arm [2] was counted and printed nothing.
	 *
	 * ⚠️ And it needs no dereference to be decided: arm [1] has already
	 * validated the SAME address by a route that cannot lie -- dlsym plus
	 * the load bias -- so the two must be equal.  A pointer compared
	 * against a known-good pointer is a stronger check than a value read
	 * through it, because a wrong pointer that happens to point at the
	 * right bytes would pass the second and fails this one.
	 */
	ops = (const struct dl_module_ops *)dlsym(h, "dl_test_module_ops");
	if (ops == NULL) {
		arm("[2] ops table", 0, "dlsym(dl_test_module_ops) returned NULL",
		    0, DL_MODULE_MAGIC);
	} else if (ops->magic != DL_MODULE_MAGIC) {
		arm("[2] ops table", 0,
		    "the struct itself did not survive relocation",
		    ops->magic, DL_MODULE_MAGIC);
	} else if (answer_ptr == NULL || failed > 0) {
		/*
		 * 🔥 Gated on arm [1] having PASSED, not merely having run.
		 *
		 * Its reference is arm [1]'s pointer, so if that one is wrong
		 * this comparison is between two values that are wrong in the
		 * same way -- and they agree, because nothing relocated either
		 * of them.  Measured: under the ablation this arm reported OK
		 * one line after arm [1] reported WRONG about the same address.
		 * A comparison is only a check when one side is known good.
		 */
		printf("dl_test: [2] ops table: SKIPPED — arm [1] did not "
		       "validate the pointer this one compares against\n");
	} else {
		arm("[2] ops table",
		    ops->answer_ptr == *answer_ptr,
		    "the relocated pointer in the struct is not the one arm [1] "
		    "validated",
		    (unsigned)(uintptr_t)ops->answer_ptr,
		    (unsigned)(uintptr_t)*answer_ptr);
	}

	/*
	 * ⚠️ The two arms below CALL through pointers the loader relocated, and
	 * they are gated on the two above having passed.
	 *
	 * 🔥 Not caution: measured.  An earlier version guarded only on
	 * ops->magic -- a plain constant in the struct, which needs no
	 * relocation and is therefore right even when every pointer beside it
	 * is wrong.  Under the ablation that leaves the module unrelocated,
	 * that guard passed and the call went to a link-time address: the task
	 * took an exception, the run lost every arm after it, and the verdict
	 * was a task that stopped rather than a loader that was wrong.  A check
	 * that reads the half that cannot fail is not a check.
	 */
	if (failed > 0 || ops == NULL) {
		printf("dl_test: [3] and [4] SKIPPED — an arm above already "
		       "says the pointers are wrong, and calling through one "
		       "of those ends the task instead of the test\n");
	} else {
		/*
		 * [3] GLOB_DAT — the module reads dl_module_answer_ptr through
		 * the GOT slot the loader filled.  The single R_X86_64_GLOB_DAT
		 * in this object is that slot.
		 */
		uint32_t r = ops->answer();
		arm("[3] GLOB_DAT", r == DL_MODULE_MAGIC,
		    "the module read its own pointer through a GOT slot that "
		    "does not name it",
		    r, DL_MODULE_MAGIC);

		/*
		 * [4] JUMP_SLOT — the module calls back into this program
		 * through its PLT.  The seed comes from here and the magic
		 * from there, so no other slot value produces this number.
		 */
		r = ops->call_host();
		arm("[4] JUMP_SLOT", r == (DL_HOST_SEED ^ DL_MODULE_MAGIC),
		    "the call through the PLT did not reach this program",
		    r, DL_HOST_SEED ^ DL_MODULE_MAGIC);
	}

	dlclose(h);

	printf("dl_test: %d of %d arms passed\n", passed, passed + failed);
	return failed == 0 ? 0 : 1;
}
