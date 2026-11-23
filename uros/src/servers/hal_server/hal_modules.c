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
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * hal_modules.c -- which module of its pool drives a device (#964).
 *
 * A device's class names its pool, as #163 laid the pools out: a network
 * card's modules are in modules/net, a disk controller's in modules/block.
 * The HAL fetches each module of the pool from bootstrap as bytes, and reads
 * what the module says it drives, its .uros_drives section (chora_drives.h),
 * with libelf.
 *
 * 🔴 IT NEVER LOADS ONE.  modload_load_class() would dlopen the pool, and a
 * module's code -- its DT_INIT first of all -- would run inside the HAL, with
 * the bus the HAL holds.  The bytes are read and given back; chora_absent,
 * whose DT_INIT says WRONG, is in modules/net to show that it stays so.
 *
 * A pool is read once, the first time a device of its class needs it, and
 * what each module declares is kept; the module's file is not.
 */

#include <mach.h>
#include <mach/module_pool.h>		/* before <mach/bootstrap.h> */
#include <mach/bootstrap.h>
#include <stdio.h>
#include <string.h>

#include <servers/netname_wait.h>
#include <libcap.h>			/* cap_manifest_build, mach_cap_port */
#include <mach/cap_types.h>		/* RESOURCE_PCI_FUNCTION, CAP_OP_PCI_* */

#include "hal_server.h"
#include "chora_drives.h"
#include "chora_given.h"		/* the module a chora is given */
#include "libelf.h"
#include "exec_types.h"			/* EXEC_OK */
#include "exec.h"			/* exec_load_image */

/*
 * The pool of a class, by its base class: what the build installs into
 * modules/<pool> for that class.  Everything else is misc, edu's (ffh)
 * among them.
 */
static const struct {
	uint8_t		base;
	const char	*pool;
} class_pools[] = {
	{ 0x01, "block" },	/* mass storage */
	{ 0x02, "net" },	/* network */
	{ 0x03, "gpu" },	/* display */
};
#define POOL_OF_THE_REST	"misc"

#define HAL_MAX_POOLS		4	/* the four names above */
#define HAL_POOL_MODULES	16
#define DRIVES_SAID		8	/* lines of a declaration said */
#define CONSOLE_LINE_MAX	255	/* and the newline: 256, kept whole */

struct pool_module {
	char			name[MODULE_NAME_MAX];
	int			lines;		/* 0: declares nothing */
	struct chora_drives	drives[CHORA_DRIVES_MAX];
};

struct pool {
	char			name[MODULE_NAME_MAX];
	int			listed;		/* bootstrap listed it */
	int			n;
	struct pool_module	mods[HAL_POOL_MODULES];
};

static struct pool	pools[HAL_MAX_POOLS];
static int		n_pools;

/*
 * The choras the HAL started, one per device (#964): the task's port, which
 * the HAL alone holds, with bootstrap's loader long done with it.  A slot is
 * free while its task is MACH_PORT_NULL.  DEVICE_MAX_CLAIMS bounds them anyway:
 * a chora holds a claim.
 */
#define HAL_MAX_CHORAS		16

static struct chora_record {
	unsigned int	bus, slot, func;
	mach_port_t	task;
} choras[HAL_MAX_CHORAS];

/*
 * What a start needs, fetched the first time and kept (#964): chora's image,
 * from the bundle -- so that a chora starts again without the disk, which may
 * be the one it drives -- the drivers' bootstrap port, and exec_server's image
 * port.  Every chora is the one program, chora, told its module and device.
 */
static vm_offset_t		chora_image;
static mach_msg_type_number_t	chora_image_len;
static mach_port_t		drivers_bootstrap = MACH_PORT_NULL;
static mach_port_t		exec_image = MACH_PORT_NULL;

static int
start_ready(void)
{
	module_name_t	prog;
	kern_return_t	kr;

	if (chora_image == 0) {
		strcpy(prog, "chora");
		kr = bootstrap_get_program(bootstrap_port, prog, &chora_image,
					   &chora_image_len);
		if (kr != KERN_SUCCESS || chora_image_len == 0) {
			printf("hal: bootstrap gave no image of chora (0x%x): "
			       "no driver is started (#964)\n", kr);
			chora_image = 0;
			return -1;
		}
		printf("hal: chora's image, %u bytes from the bundle, kept for "
		       "every start (#964)\n", (unsigned)chora_image_len);
	}
	if (drivers_bootstrap == MACH_PORT_NULL) {
		kr = bootstrap_drivers_port(bootstrap_port, &drivers_bootstrap);
		if (kr != KERN_SUCCESS) {
			printf("hal: bootstrap gave no drivers' bootstrap port "
			       "(0x%x): no driver is started (#964)\n", kr);
			drivers_bootstrap = MACH_PORT_NULL;
			return -1;
		}
	}
	if (exec_image == MACH_PORT_NULL) {
		kr = netname_wait(name_server_port, "exec_image", &exec_image);
		if (kr != KERN_SUCCESS) {
			printf("hal: no \"exec_image\" (0x%x): no driver is "
			       "started (#964)\n", kr);
			exec_image = MACH_PORT_NULL;
			return -1;
		}
	}
	return 0;
}

/* NUL-separated strings, then the empty string that ends them (exec_types.h). */
static size_t
blob_put(char *blob, size_t at, size_t cap, const char *s)
{
	size_t	len = strlen(s) + 1;

	if (at >= cap || at + len >= cap)
		return cap;
	memcpy(blob + at, s, len);
	return at + len;
}

/*
 * The module `file' of the pool `pool', as bytes, for a chora (#964): from
 * bootstrap, which serves the bundle's pools.  0, or -1 said.
 */
static int
module_fetch(const char *pool, const char *file, vm_offset_t *data,
	     mach_msg_type_number_t *len)
{
	module_name_t	class_buf, name_buf;
	kern_return_t	kr;

	*data = 0;
	*len = 0;
	if (strlen(pool) >= sizeof class_buf || strlen(file) >= sizeof name_buf)
		return -1;
	strcpy(class_buf, pool);
	strcpy(name_buf, file);
	kr = bootstrap_get_module(bootstrap_port, class_buf, name_buf, data,
				  len);
	if (kr != KERN_SUCCESS || *len == 0) {
		printf("hal: bootstrap would not give %s/%s again (0x%x) "
		       "(#964)\n", pool, file, kr);
		if (kr == KERN_SUCCESS && *data != 0)
			(void)vm_deallocate(mach_task_self(), *data, *len);
		*data = 0;
		return -1;
	}
	return 0;
}

/*
 * How well the module in `data' declares `dev', read from those bytes (#964):
 * what a chora is given is what was checked for its device, and not a file
 * read once and fetched again.  0 when it does not declare it, or nothing.
 */
static int
module_score(const void *data, size_t size, const struct hal_device_info *dev)
{
	elf_image_t			img;
	elf_shdr_view_t			sh;
	struct chora_drives_fault	f;
	struct chora_drives		d[CHORA_DRIVES_MAX];
	int				n, score = 0;

	memset(&img, 0, sizeof img);
	if (elf_open(data, size, &img) != ELF_OK)
		return 0;
	if (elf_shdr_find(&img, CHORA_DRIVES_SECTION, &sh) == ELF_OK &&
	    sh.data != NULL &&
	    (n = chora_drives_check(sh.data, (size_t)sh.size, &f)) > 0) {
		memcpy(d, sh.data, (size_t)n * sizeof d[0]);
		score = chora_drives_score(d, n, dev->vendor_device,
					   dev->class_rev);
	}
	elf_close(&img);
	return score;
}

/*
 * Give the chora in `task' its module (chora_given.h): the name of the port it
 * was sent on, in the chora's task, or MACH_PORT_NULL said.
 */
static mach_port_t
module_give(mach_port_t task, vm_offset_t data, mach_msg_type_number_t len,
	    const char *where)
{
	kern_return_t	kr;
	mach_port_t	box = chora_give_module(task, data, len, &kr);

	if (box == MACH_PORT_NULL)
		printf("hal: the chora of %s could not be given its module "
		       "(0x%x) (#964)\n", where, kr);
	return box;
}

/*
 * A chora for `dev', told the module `file' of the pool `pool' (#964): 0 when
 * it is started, -1 said.  The task is made here, given the drivers' bootstrap
 * port -- the driver's port, never the master port -- and loaded by exec_server
 * from chora's image, with a manifest naming this one function, which
 * exec_server cuts to the HAL's `delegatable' line.  It is given the module's
 * bytes, read again here and checked again for `dev', on a port of its own
 * whose name is its fifth argument.  It is recorded and its death asked
 * before its thread runs, so that the HAL hears of every end.
 */
static int
chora_start(const struct hal_device_info *dev, const char *pool,
	    const char *file)
{
	static const char	envp_blob[] = "";
	char			argv_blob[3 * MODULE_NAME_MAX + 32];
	char			module[MODULE_NAME_MAX], where[16], boxname[16];
	char			blob[256];
	cap_manifest_entry_t	want;
	unsigned int		blen;
	size_t			n, at = 0;
	mach_port_t		task = MACH_PORT_NULL, thread = MACH_PORT_NULL;
	mach_port_t		prev = MACH_PORT_NULL, box = MACH_PORT_NULL;
	vm_offset_t		mod = 0;
	mach_msg_type_number_t	mod_len = 0;
	kern_return_t		kr;
	int			rc = EXEC_OK, i, slot = -1;

	if (start_ready() != 0)
		return -1;
	for (i = 0; i < HAL_MAX_CHORAS && slot < 0; i++)
		if (choras[i].task == MACH_PORT_NULL)
			slot = i;
	if (slot < 0) {
		printf("hal: all %d choras' slots are taken (#964)\n",
		       HAL_MAX_CHORAS);
		return -1;
	}

	/* The module's name without its .so, which chora puts back. */
	n = strlen(file);
	if (n > 3 && strcmp(file + n - 3, ".so") == 0)
		n -= 3;
	if (n >= sizeof module)
		return -1;
	memcpy(module, file, n);
	module[n] = '\0';
	(void)snprintf(where, sizeof where, "%02x:%02x.%x", dev->bus,
		       dev->slot, dev->func);

	memset(&want, 0, sizeof want);
	want.type = RESOURCE_PCI_FUNCTION;
	want.resource_id = (uint64_t)(dev->bus << 8 | dev->slot << 3 |
				      dev->func);
	want.ops = CAP_OP_PCI_DMA_MAP | CAP_OP_PCI_MMIO_MAP | CAP_OP_PCI_IRQ;
	blen = cap_manifest_build("chora", &want, 1, NULL, 0, blob,
				  sizeof blob);
	if (blen == 0) {
		printf("hal: no manifest for the chora of %s (#964)\n", where);
		return -1;
	}

	/* The module the chora will be given, and what it declares, again. */
	if (module_fetch(pool, file, &mod, &mod_len) != 0)
		return -1;
	if (module_score((const void *)mod, mod_len, dev) <= 0) {
		printf("hal: %s/%s, as bootstrap gives it now, does not declare "
		       "%s: no chora is started (#964)\n", pool, file, where);
		(void)vm_deallocate(mach_task_self(), mod, mod_len);
		return -1;
	}

	kr = task_create(mach_task_self(), NULL, 0, FALSE, &task);
	if (kr == KERN_SUCCESS)
		kr = task_set_bootstrap_port(task, drivers_bootstrap);
	if (kr == KERN_SUCCESS) {
		box = module_give(task, mod, mod_len, where);
		if (box == MACH_PORT_NULL)
			kr = KERN_FAILURE;
	}
	(void)vm_deallocate(mach_task_self(), mod, mod_len);
	if (kr == KERN_SUCCESS) {
		(void)snprintf(boxname, sizeof boxname, "0x%x", (unsigned)box);
		at = blob_put(argv_blob, at, sizeof argv_blob, "chora");
		at = blob_put(argv_blob, at, sizeof argv_blob, pool);
		at = blob_put(argv_blob, at, sizeof argv_blob, module);
		at = blob_put(argv_blob, at, sizeof argv_blob, where);
		at = blob_put(argv_blob, at, sizeof argv_blob, boxname);
		if (at >= sizeof argv_blob)
			kr = KERN_INVALID_ARGUMENT;
		else
			argv_blob[at++] = '\0';
	}
	if (kr == KERN_SUCCESS)
		kr = exec_load_image(exec_image, task, mach_cap_port,
				     (pointer_t)chora_image, chora_image_len,
				     (pointer_t)blob, blen,
				     (pointer_t)argv_blob,
				     (mach_msg_type_number_t)at,
				     (pointer_t)envp_blob, sizeof envp_blob,
				     &thread, &rc);
	if (kr != KERN_SUCCESS || rc != EXEC_OK) {
		printf("hal: the chora of %s was not loaded: 0x%x, exec_server's "
		       "%d (#964)\n", where, kr, rc);
		if (task != MACH_PORT_NULL) {
			(void)task_terminate(task);
			(void)mach_port_deallocate(mach_task_self(), task);
		}
		if (thread != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(), thread);
		return -1;
	}

	choras[slot].bus = dev->bus;
	choras[slot].slot = dev->slot;
	choras[slot].func = dev->func;
	choras[slot].task = task;
	kr = mach_port_request_notification(mach_task_self(), task,
					    MACH_NOTIFY_DEAD_NAME, 0,
					    hal_service_port,
					    MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev);
	if (kr != KERN_SUCCESS) {
		printf("hal: the chora of %s, its death not asked (0x%x): "
		       "ended (#964)\n", where, kr);
		choras[slot].task = MACH_PORT_NULL;
		(void)task_terminate(task);
		(void)mach_port_deallocate(mach_task_self(), task);
		(void)mach_port_deallocate(mach_task_self(), thread);
		return -1;
	}
	if (prev != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), prev);
	kr = thread_resume(thread);
	(void)mach_port_deallocate(mach_task_self(), thread);
	if (kr != KERN_SUCCESS) {
		/* Its death notice gives the slot back. */
		printf("hal: the chora of %s not resumed (0x%x): ended (#964)\n",
		       where, kr);
		(void)task_terminate(task);
		return -1;
	}
	return 0;
}

int
hal_chora_dead(mach_port_t name)
{
	mach_port_urefs_t	refs = 0;
	int			i;

	for (i = 0; i < HAL_MAX_CHORAS; i++)
		if (choras[i].task != MACH_PORT_NULL && choras[i].task == name)
			break;
	if (i == HAL_MAX_CHORAS)
		return 0;
	printf("hal: the chora of %02x:%02x.%x has ended (#964)\n",
	       choras[i].bus, choras[i].slot, choras[i].func);
	/* The right the HAL held, now dead, and the one the notice brought. */
	if (mach_port_get_refs(mach_task_self(), name,
			       MACH_PORT_RIGHT_DEAD_NAME, &refs) == KERN_SUCCESS &&
	    refs > 0)
		(void)mach_port_mod_refs(mach_task_self(), name,
					 MACH_PORT_RIGHT_DEAD_NAME,
					 -(mach_port_delta_t)refs);
	choras[i].task = MACH_PORT_NULL;
	return 1;
}

mach_port_t
hal_chora_task(unsigned int bus, unsigned int slot, unsigned int func)
{
	int	i;

	for (i = 0; i < HAL_MAX_CHORAS; i++)
		if (choras[i].task != MACH_PORT_NULL && choras[i].bus == bus &&
		    choras[i].slot == slot && choras[i].func == func)
			return choras[i].task;
	return MACH_PORT_NULL;
}

/*
 * The classes the HAL's line leaves to others, "--leave CODE" (#964): a
 * device of one is said and not started, while another server drives that
 * class -- irq_claim_test the network card, block_device_server the disks,
 * pci_function_test edu -- until their drivers are the HAL's (#966, #968).
 * The digits say how much of the class code is compared: two the base class,
 * four the subclass too, six the interface too.
 */
#define HAL_MAX_LEAVES		8

static struct {
	uint32_t	code;
	uint32_t	mask;
} leaves[HAL_MAX_LEAVES];
static int		n_leaves;

int
hal_modules_leave(const char *arg)
{
	const char	*p;
	uint32_t	v = 0;
	int		digits = 0;

	if (arg == NULL || arg[0] != '0' || (arg[1] != 'x' && arg[1] != 'X'))
		return -1;
	for (p = arg + 2; *p != '\0'; p++) {
		int d;

		if (*p >= '0' && *p <= '9')
			d = *p - '0';
		else if (*p >= 'a' && *p <= 'f')
			d = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F')
			d = *p - 'A' + 10;
		else
			return -1;
		v = v << 4 | (uint32_t)d;
		digits++;
	}
	if ((digits != 2 && digits != 4 && digits != 6) ||
	    n_leaves >= HAL_MAX_LEAVES)
		return -1;
	leaves[n_leaves].code = v << (24 - 4 * digits);
	leaves[n_leaves].mask = 0xffffffu & ~((1u << (24 - 4 * digits)) - 1u);
	n_leaves++;
	return 0;
}

/* The --leave a device's class falls under, or -1. */
static int
left_by(uint32_t class_rev)
{
	uint32_t	code = class_rev >> 8;
	int		i;

	for (i = 0; i < n_leaves; i++)
		if ((code & leaves[i].mask) == leaves[i].code)
			return i;
	return -1;
}

static const char *
pool_of_class(uint32_t class_rev)
{
	unsigned int	i;
	uint8_t		base = (uint8_t)(class_rev >> 24);

	for (i = 0; i < sizeof class_pools / sizeof class_pools[0]; i++)
		if (class_pools[i].base == base)
			return class_pools[i].pool;
	return POOL_OF_THE_REST;
}

/*
 * What one module declares, read from its file: m->lines lines, or 0 when it
 * declares nothing -- no section, or one the HAL refuses, which it says.
 */
static void
module_read(struct pool_module *m, const char *pool, const void *data,
	    size_t size)
{
	elf_image_t			img;
	elf_shdr_view_t			sh;
	struct chora_drives_fault	f;
	int				rc, n, i;

	m->lines = 0;
	memset(&img, 0, sizeof img);
	rc = elf_open(data, size, &img);
	if (rc != ELF_OK) {
		printf("hal: %s/%s is no ELF the HAL reads (%d): it declares "
		       "nothing\n", pool, m->name, rc);
		return;
	}
	rc = elf_shdr_find(&img, CHORA_DRIVES_SECTION, &sh);
	if (rc != ELF_OK) {
		elf_close(&img);
		return;			/* no declaration: not an error */
	}
	if (sh.data == NULL) {
		printf("hal: %s/%s: its %s lies outside its file: it declares "
		       "nothing\n", pool, m->name, CHORA_DRIVES_SECTION);
		elf_close(&img);
		return;
	}
	n = chora_drives_check(sh.data, (size_t)sh.size, &f);
	if (n < 0) {
		if (f.line < 0)
			printf("hal: %s/%s: its %s has %s: it declares "
			       "nothing\n", pool, m->name,
			       CHORA_DRIVES_SECTION, f.what);
		else
			printf("hal: %s/%s: line %d of its %s is %s: it "
			       "declares nothing\n", pool, m->name, f.line,
			       CHORA_DRIVES_SECTION, f.what);
		elf_close(&img);
		return;
	}
	memcpy(m->drives, sh.data, (size_t)n * sizeof m->drives[0]);
	m->lines = n;
	elf_close(&img);

	/*
	 * Written whole, with one printf: the console keeps a line whole up to
	 * 256 bytes, and only when it is written in one piece (#578).  So the
	 * line is made in a buffer of 255 and the newline, cut there if it must
	 * be; the first DRIVES_SAID lines of the declaration are said.
	 */
	{
		char	line[CONSOLE_LINE_MAX];
		int	at;

		at = snprintf(line, sizeof line, "hal: %s/%s drives", pool,
			      m->name);
		for (i = 0; i < n && i < DRIVES_SAID &&
			    at >= 0 && at < (int)sizeof line; i++) {
			const struct chora_drives *d = &m->drives[i];

			if (d->kind == CHORA_DRIVES_ID)
				at += snprintf(line + at, sizeof line - at,
					       " %04x:%04x", d->vendor,
					       d->device);
			else
				at += snprintf(line + at, sizeof line - at,
					       " class %06x/%06x",
					       (unsigned)d->class_code,
					       (unsigned)d->class_mask);
		}
		if (n > DRIVES_SAID && at >= 0 && at < (int)sizeof line)
			(void)snprintf(line + at, sizeof line - at,
				       " and %d more", n - DRIVES_SAID);
		printf("%s\n", line);
	}
}

/*
 * The pool, read the first time it is asked for: NULL when there is no room
 * for it, said.  A pool bootstrap would not list is kept as such, so that it
 * is asked once.
 */
static struct pool *
pool_read(const char *name)
{
	struct pool		*p;
	module_name_t		class_buf;
	vm_offset_t		names = 0;
	mach_msg_type_number_t	names_count = 0;
	const char		*q;
	kern_return_t		kr;
	int			i;

	for (i = 0; i < n_pools; i++)
		if (strcmp(pools[i].name, name) == 0)
			return &pools[i];
	if (n_pools >= HAL_MAX_POOLS || strlen(name) >= sizeof class_buf) {
		printf("hal: no room to read the pool %s\n", name);
		return NULL;
	}
	p = &pools[n_pools++];
	memset(p, 0, sizeof *p);
	strcpy(p->name, name);
	strcpy(class_buf, name);

	kr = bootstrap_list_modules(bootstrap_port, class_buf, &names,
				    &names_count);
	if (kr != KERN_SUCCESS) {
		printf("hal: bootstrap would not list the pool %s (0x%x)\n",
		       name, kr);
		return p;
	}
	p->listed = 1;

	for (q = (const char *)names; q < (const char *)names + names_count; ) {
		size_t			len = strlen(q);
		module_name_t		name_buf;
		vm_offset_t		data = 0;
		mach_msg_type_number_t	data_count = 0;

		if (len == 0)
			break;
		if (len >= sizeof name_buf) {
			printf("hal: %s/%s: a name longer than a module's\n",
			       name, q);
			q += len + 1;
			continue;
		}
		if (p->n >= HAL_POOL_MODULES) {
			printf("hal: the pool %s has more than %d modules: "
			       "%s and those after it are not read\n", name,
			       HAL_POOL_MODULES, q);
			break;
		}
		strcpy(name_buf, q);
		kr = bootstrap_get_module(bootstrap_port, class_buf, name_buf,
					  &data, &data_count);
		if (kr != KERN_SUCCESS) {
			printf("hal: bootstrap would not give %s/%s (0x%x)\n",
			       name, q, kr);
			q += len + 1;
			continue;
		}
		strcpy(p->mods[p->n].name, q);
		module_read(&p->mods[p->n], name, (const void *)data,
			    data_count);
		p->n++;
		(void)vm_deallocate(mach_task_self(), data, data_count);
		q += len + 1;
	}
	if (names != 0)
		(void)vm_deallocate(mach_task_self(), names, names_count);
	return p;
}

int
hal_module_for(const struct hal_device_info *dev,
	       struct hal_module_choice *c)
{
	struct pool	*p;
	int		score[HAL_POOL_MODULES];
	int		i, chosen = -1, rival = -1;

	memset(c, 0, sizeof *c);
	c->pool = pool_of_class(dev->class_rev);
	p = pool_read(c->pool);
	if (p == NULL || !p->listed) {
		c->verdict = HAL_MODULE_UNREAD;
		return c->verdict;
	}
	for (i = 0; i < p->n; i++)
		score[i] = p->mods[i].lines == 0 ? 0 :
			chora_drives_score(p->mods[i].drives, p->mods[i].lines,
					   dev->vendor_device, dev->class_rev);
	switch (chora_drives_pick(score, p->n, &chosen, &rival)) {
	case CHORA_PICK_ONE:
		c->verdict = HAL_MODULE_ONE;
		c->module = p->mods[chosen].name;
		break;
	case CHORA_PICK_TIE:
		c->verdict = HAL_MODULE_TIE;
		c->module = p->mods[chosen].name;
		c->rival = p->mods[rival].name;
		break;
	default:
		c->verdict = HAL_MODULE_NONE;
		break;
	}
	return c->verdict;
}

void
hal_module_report(const struct hal_device_info *dev)
{
	struct hal_module_choice	c;
	char				at[48];

	(void)snprintf(at, sizeof at, "hal: %02x:%02x.%x %04x:%04x class %06x",
		       dev->bus, dev->slot, dev->func,
		       dev->vendor_device & 0xffffu, dev->vendor_device >> 16,
		       dev->class_rev >> 8);
	switch (hal_module_for(dev, &c)) {
	case HAL_MODULE_ONE: {
		int	l = left_by(dev->class_rev);

		if (l >= 0) {
			printf("%s: %s/%s declares it, and the HAL's line leaves "
			       "class %06x/%06x to others\n", at, c.pool,
			       c.module, (unsigned)leaves[l].code,
			       (unsigned)leaves[l].mask);
			break;
		}
		printf("%s: %s/%s declares it, %s\n", at, c.pool, c.module,
		       chora_start(dev, c.pool, c.module) == 0 ?
		       "and its chora is started" :
		       "and its chora could not be started");
		break;
	}
	case HAL_MODULE_TIE:
		printf("%s: %s/%s and %s/%s declare it alike, and the HAL "
		       "chooses neither\n", at, c.pool, c.module, c.pool,
		       c.rival);
		break;
	case HAL_MODULE_NONE:
		printf("%s: no module of %s declares it\n", at, c.pool);
		break;
	default:
		printf("%s: its pool, %s, could not be read\n", at, c.pool);
		break;
	}
}
