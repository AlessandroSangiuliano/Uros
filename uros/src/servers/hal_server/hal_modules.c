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

#include "hal_server.h"
#include "chora_drives.h"
#include "libelf.h"

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
		printf("%s: %s/%s declares it\n", at, c.pool, c.module);
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
