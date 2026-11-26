/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * modload_test.c -- does libmodload know a module by its class (#973)
 *
 * Two pools of its own in the bundle, modload_a and modload_b.  Each has a
 * twin.so, the same file name in both, and a module of its own, only_a.so and
 * only_b.so.  Every module says which pool it was built for, under which file
 * name, and answers a letter from its own code (modload_test_module.h).
 *
 *   [1] modload_load_named("modload_a", "twin.so") gives modload_a's twin;
 *   [2] modload_load_named("modload_b", "twin.so") gives modload_b's twin,
 *       and not modload_a's again: the cache and libdl keep the two apart;
 *   [3] modload_load_class("modload_b") gives modload_b's two modules and
 *       none of modload_a's, whose twin is cached by then;
 *   [4] modload_load_class("modload_a") gives modload_a's two and none of
 *       modload_b's, both of which are cached by then.
 *   [5] modload_load_given() of modload_a/only_a.so's bytes as
 *       modload_given/only_a.so loads those bytes: a pool bootstrap does not
 *       have, so that only the bytes given can be what runs (#964);
 *   [6] modload_load_given() as modload_a/twin.so, which [1] cached, is
 *       refused: libdl knows an object by its path, and would hand back the
 *       cached module in place of the one given.
 *
 * It runs in chora.conf, before chora_test: libmodload is how a chora fetches
 * its module, and the pools travel in the bundle every entry reads.
 */

#include <mach.h>
#include <mach/module_pool.h>		/* before <mach/bootstrap.h> */
#include <mach/bootstrap.h>		/* [5], [6]: the bytes to give */
#include <stdio.h>
#include <string.h>

#include "modload.h"
#include "modload_test_module.h"

#define MAX_OPS		8
#define DESCRIBED	(2 * 64 + 8)	/* "<class>/<file> <letter>" */

static int	arms_run, arms_passed;

/* "modload_b/twin.so B", what a module says it is and answers. */
static void
describe(const struct modload_test_ops *ops, char *buf, size_t size)
{
	if (ops == NULL)
		(void) snprintf(buf, size, "nothing");
	else
		(void) snprintf(buf, size, "%s/%s %c", ops->class, ops->file,
				ops->answer());
}

/* [1] and [2]: <class>'s twin.so, by name. */
static void
arm_named(int n, const char *class, int answer)
{
	const struct modload_test_ops *ops;
	char got[DESCRIBED];

	arms_run++;
	ops = modload_load_named(class, "twin.so", MODLOAD_TEST_SUFFIX);
	describe(ops, got, sizeof got);
	if (ops == NULL || strcmp(ops->class, class) != 0 ||
	    strcmp(ops->file, "twin.so") != 0 || ops->answer() != answer) {
		printf("modload_test: [%d] modload_load_named(\"%s\", "
		       "\"twin.so\") gave %s, where %s/twin.so %c was asked "
		       "for -- WRONG\n", n, class, got, class, answer);
		return;
	}
	printf("modload_test: [%d] modload_load_named(\"%s\", \"twin.so\") "
	       "gave %s\n", n, class, got);
	arms_passed++;
}

/* [3] and [4]: <class>'s whole pool, its twin.so and its own module. */
static void
arm_class(int n, const char *class, int twin_answer, const char *own,
	  int own_answer)
{
	void *ops[MAX_OPS];
	char line[MAX_OPS * (DESCRIBED + 2)];
	size_t used = 0;
	int got, i, twin = 0, mine = 0, other = 0;

	arms_run++;
	got = modload_load_class(class, MODLOAD_TEST_SUFFIX, ops, MAX_OPS);
	line[0] = '\0';
	for (i = 0; i < got; i++) {
		const struct modload_test_ops *m = ops[i];
		char one[DESCRIBED];

		describe(m, one, sizeof one);
		if (used < sizeof line)
			used += (size_t) snprintf(line + used,
						  sizeof line - used, "%s%s",
						  i == 0 ? "" : ", ", one);
		if (strcmp(m->class, class) != 0)
			other++;
		else if (strcmp(m->file, "twin.so") == 0 &&
			 m->answer() == twin_answer)
			twin++;
		else if (strcmp(m->file, own) == 0 && m->answer() == own_answer)
			mine++;
		else
			other++;
	}
	if (got == 2 && twin == 1 && mine == 1 && other == 0) {
		printf("modload_test: [%d] modload_load_class(\"%s\") gave "
		       "its 2 modules and no other: %s\n", n, class, line);
		arms_passed++;
		return;
	}
	printf("modload_test: [%d] modload_load_class(\"%s\") gave %d "
	       "module(s): %s, where its twin.so and %s, and no other, were "
	       "asked for -- WRONG\n", n, class, got,
	       got == 0 ? "none" : line, own);
}

/* `class'/`file' of the bundle, as bytes: 0, or -1 said in arm `n'. */
static int
fetch_bytes(int n, const char *class, const char *file, vm_offset_t *data,
	    mach_msg_type_number_t *count)
{
	module_name_t	c, f;
	kern_return_t	kr;

	strcpy(c, class);
	strcpy(f, file);
	kr = bootstrap_get_module(bootstrap_port, c, f, data, count);
	if (kr != KERN_SUCCESS || *count == 0) {
		printf("modload_test: [%d] WRONG -- not asked: bootstrap gave no "
		       "%s/%s (0x%x)\n", n, class, file, kr);
		return -1;
	}
	return 0;
}

/*
 * [5] modload_load_given(): the bytes of modload_a/only_a.so, given as
 * modload_given/only_a.so.  bootstrap has no pool modload_given -- the arm
 * asks it, the control -- so a module that answers is the one given.
 */
static void
arm_given(int n)
{
	const struct modload_test_ops	*ops;
	vm_offset_t			data = 0, none = 0;
	mach_msg_type_number_t		count = 0, none_count = 0;
	module_name_t			c, f;
	kern_return_t			absent;
	char				got[DESCRIBED];

	arms_run++;
	if (fetch_bytes(n, "modload_a", "only_a.so", &data, &count) != 0)
		return;
	strcpy(c, "modload_given");
	strcpy(f, "only_a.so");
	absent = bootstrap_get_module(bootstrap_port, c, f, &none, &none_count);
	if (absent == KERN_SUCCESS) {
		(void)vm_deallocate(mach_task_self(), none, none_count);
		(void)vm_deallocate(mach_task_self(), data, count);
		printf("modload_test: [%d] WRONG -- not asked: bootstrap has a "
		       "modload_given/only_a.so, so a module loaded under that "
		       "name need not be the one given\n", n);
		return;
	}
	ops = modload_load_given("modload_given", "only_a.so", data, count,
				 MODLOAD_TEST_SUFFIX);
	describe(ops, got, sizeof got);
	if (ops == NULL || strcmp(ops->class, "modload_a") != 0 ||
	    strcmp(ops->file, "only_a.so") != 0 || ops->answer() != 'a') {
		printf("modload_test: [%d] modload_load_given() of modload_a/"
		       "only_a.so's %u bytes, as modload_given/only_a.so, gave "
		       "%s, where modload_a/only_a.so a was given -- WRONG\n", n,
		       (unsigned)count, got);
		if (ops == NULL)
			(void)vm_deallocate(mach_task_self(), data, count);
		return;
	}
	printf("modload_test: [%d] modload_load_given() of modload_a/only_a.so's "
	       "%u bytes, as modload_given/only_a.so, which bootstrap does not "
	       "have (0x%x), gave %s\n", n, (unsigned)count, absent, got);
	arms_passed++;
}

/*
 * [6] A module given as modload_a/twin.so, which [1] cached: refused, and the
 * cached one is what modload_a/twin.so still gives.
 */
static void
arm_given_cached(int n)
{
	const struct modload_test_ops	*given, *named;
	vm_offset_t			data = 0;
	mach_msg_type_number_t		count = 0;
	char				got[DESCRIBED], still[DESCRIBED];

	arms_run++;
	if (fetch_bytes(n, "modload_b", "twin.so", &data, &count) != 0)
		return;
	given = modload_load_given("modload_a", "twin.so", data, count,
				   MODLOAD_TEST_SUFFIX);
	if (given == NULL)
		(void)vm_deallocate(mach_task_self(), data, count);
	named = modload_load_named("modload_a", "twin.so", MODLOAD_TEST_SUFFIX);
	describe(given, got, sizeof got);
	describe(named, still, sizeof still);
	if (given != NULL || named == NULL || named->answer() != 'A') {
		printf("modload_test: [%d] modload_b/twin.so's bytes given as "
		       "modload_a/twin.so, which is cached: gave %s, where a "
		       "refusal was asked for; modload_a/twin.so gives %s -- "
		       "WRONG\n", n, got, still);
		return;
	}
	printf("modload_test: [%d] modload_b/twin.so's bytes given as "
	       "modload_a/twin.so, which is cached: refused, and "
	       "modload_a/twin.so still gives %s\n", n, still);
	arms_passed++;
}

int
main(void)
{
	printf("modload_test: started\n");
	modload_init("modload_test");

	arm_named(1, "modload_a", 'A');
	arm_named(2, "modload_b", 'B');
	arm_class(3, "modload_b", 'B', "only_b.so", 'b');
	arm_class(4, "modload_a", 'A', "only_a.so", 'a');
	arm_given(5);
	arm_given_cached(6);

	printf("modload_test: %d of %d arms passed\n", arms_passed, arms_run);
	return arms_passed == arms_run ? 0 : 1;
}
