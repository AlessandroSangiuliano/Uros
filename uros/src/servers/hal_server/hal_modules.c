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
 *
 * ── The disk's pools (#964) ──
 *
 * The modules a boot needs travel in the bundle; the others are on the disk,
 * in HAL_DISK_POOLS<pool>/, which the HAL reads through libvfs once "/" is
 * served -- a machine may want two hundred drivers, and the bundle need not
 * carry them.  A device no module of the bundle declares waits for the disk,
 * unless its class is left to others; the HAL asks the name server to tell it
 * when "/" is mounted, on a port of its own, and decides then.  A chora is
 * given its module's bytes wherever they came from (chora_given.h), so no
 * driver reads the disk.
 */

#include <mach.h>
#include <mach/module_pool.h>		/* before <mach/bootstrap.h> */
#include <mach/bootstrap.h>
#include <mach/clock.h>			/* how long a chora lived, #964 */
#include <mach/clock_types.h>		/* URMACH_CAP_CLOCK, time since boot */
#include <mach/thread_switch.h>		/* a restart's delay, #964 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <servers/netname_wait.h>
#include <servers/netname.h>		/* netname_notify_mount() */
#include <libvfs.h>			/* the disk's pools */
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

#define HAL_MAX_POOLS		8	/* the four names above, bundle and disk */
#define HAL_DISK_POOLS		"/mach_servers/modules/"
#define HAL_MAX_WAITING		16	/* devices waiting for the disk's pools */
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
	int			disk;		/* the disk's, not the bundle's */
	int			listed;		/* bootstrap, or the disk, listed it */
	int			n;
	struct pool_module	mods[HAL_POOL_MODULES];
};

static struct pool	pools[HAL_MAX_POOLS];
static int		n_pools;

/*
 * The choras the HAL started, one per device (#964): the task's port, which
 * the HAL alone holds, with exec_server long done with it.  A record is a
 * device's from its first start on, and keeps what a start needs -- the
 * device, its module and where the module is -- so that the chora is started
 * again when it dies (hal_chora_dead()).  DEVICE_MAX_CLAIMS bounds them
 * anyway: a chora holds a claim.
 */
#define HAL_MAX_CHORAS		16

static struct chora_record {
	int			used;		/* the record is a device's */
	struct hal_device_info	dev;
	char			pool[MODULE_NAME_MAX];
	char			file[MODULE_NAME_MAX];
	int			disk;		/* its module from the disk's pool */
	mach_port_t		task;		/* MACH_PORT_NULL while none runs */
	int			state;		/* HAL_DRIVER_* */
	int			starts;		/* every start */
	int			quick;		/* starts in a row that died soon */
	tvalspec_t		started;	/* its last start, by the clock */
	unsigned int		ticket;		/* the restart it waits for */
	vm_offset_t		given;		/* its module from an update, */
	mach_msg_type_number_t	given_len;	/* kept for every start: or 0 */
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

static int disk_file(const char *path, vm_offset_t *data,
		     mach_msg_type_number_t *len);

/*
 * The module `file' of the pool `pool', as bytes, for a chora (#964): from
 * bootstrap, which serves the bundle's pools, or from the disk's.  0, or -1
 * said.
 */
static int
module_fetch(const char *pool, const char *file, int disk, vm_offset_t *data,
	     mach_msg_type_number_t *len)
{
	module_name_t	class_buf, name_buf;
	char		path[VFS_PATH_MAX];
	kern_return_t	kr;

	*data = 0;
	*len = 0;
	if (disk) {
		(void)snprintf(path, sizeof path, "%s%s/%s", HAL_DISK_POOLS, pool,
			       file);
		return disk_file(path, data, len);
	}
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
 * #964: the clock a chora's life is measured by, asked for once.  -1 when
 * there is none: every death then counts as one that came soon.
 */
static int
hal_now(tvalspec_t *t)
{
	static mach_port_t	clk = MACH_PORT_NULL;
	static int		asked;

	if (!asked) {
		asked = 1;
		if (host_get_clock_service(host_port, URMACH_CAP_CLOCK, &clk)
		    != KERN_SUCCESS)
			clk = MACH_PORT_NULL;
	}
	if (clk == MACH_PORT_NULL || clock_get_time(clk, t) != KERN_SUCCESS) {
		t->tv_sec = 0;
		t->tv_nsec = 0;
		return -1;
	}
	return 0;
}

/*
 * A start of the chora of the record `r', for its device, told its module
 * (#964): 0 when it is started, -1 said.  The task is made here, given the drivers' bootstrap
 * port -- the driver's port, never the master port -- and loaded by exec_server
 * from chora's image, with a manifest naming this one function, which
 * exec_server cuts to the HAL's `delegatable' line.  It is given the module's
 * bytes, read again here and checked again for `dev', on a port of its own
 * whose name is its fifth argument.  It is recorded and its death asked
 * before its thread runs, so that the HAL hears of every end.  A start is
 * counted as it begins: one that fails is a chora that died soon.
 */
static int
chora_launch(struct chora_record *r)
{
	const struct hal_device_info	*dev = &r->dev;
	const char			*pool = r->pool, *file = r->file;
	int				disk = r->disk;
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
	int			rc = EXEC_OK;

	r->starts++;
	r->quick++;
	(void)hal_now(&r->started);
	if (start_ready() != 0)
		return -1;

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

	/*
	 * The module the chora will be given, and what it declares, again: the
	 * bytes an update gave, which the record keeps, or the pool's, fetched.
	 */
	if (r->given != 0) {
		mod = r->given;
		mod_len = r->given_len;
	} else if (module_fetch(pool, file, disk, &mod, &mod_len) != 0)
		return -1;
	if (module_score((const void *)mod, mod_len, dev) <= 0) {
		printf("hal: %s/%s, as %s, does not declare %s: no chora is "
		       "started (#964)\n", pool, file, r->given != 0 ?
		       "an update gave it" : disk ? "the disk gives it now" :
		       "bootstrap gives it now", where);
		if (r->given == 0)
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
	/* The message carried a copy: an update's bytes stay the record's. */
	if (r->given == 0)
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

	r->task = task;
	kr = mach_port_request_notification(mach_task_self(), task,
					    MACH_NOTIFY_DEAD_NAME, 0,
					    hal_service_port,
					    MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev);
	if (kr != KERN_SUCCESS) {
		printf("hal: the chora of %s, its death not asked (0x%x): "
		       "ended (#964)\n", where, kr);
		r->task = MACH_PORT_NULL;
		(void)task_terminate(task);
		(void)mach_port_deallocate(mach_task_self(), task);
		(void)mach_port_deallocate(mach_task_self(), thread);
		return -1;
	}
	if (prev != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), prev);
	r->state = HAL_DRIVER_RUNNING;
	kr = thread_resume(thread);
	(void)mach_port_deallocate(mach_task_self(), thread);
	if (kr != KERN_SUCCESS) {
		/* Its death notice comes, as any chora's that died soon. */
		printf("hal: the chora of %s not resumed (0x%x): ended (#964)\n",
		       where, kr);
		(void)task_terminate(task);
	}
	return 0;
}

/*
 * A chora for `dev', told the module `file' of the pool `pool', from the disk
 * when `disk' (#964): 0 when it is started, -1 said.  Its record keeps what a
 * restart needs; a first start that fails leaves no record.
 */
static int
chora_start(const struct hal_device_info *dev, const char *pool,
	    const char *file, int disk)
{
	struct chora_record	*r = NULL;
	int			i;

	for (i = 0; i < HAL_MAX_CHORAS && r == NULL; i++)
		if (!choras[i].used)
			r = &choras[i];
	if (r == NULL) {
		printf("hal: all %d choras' slots are taken (#964)\n",
		       HAL_MAX_CHORAS);
		return -1;
	}
	if (strlen(pool) >= sizeof r->pool || strlen(file) >= sizeof r->file)
		return -1;
	memset(r, 0, sizeof *r);
	r->used = 1;
	r->dev = *dev;
	strcpy(r->pool, pool);
	strcpy(r->file, file);
	r->disk = disk;
	if (chora_launch(r) != 0) {
		memset(r, 0, sizeof *r);
		return -1;
	}
	return 0;
}

/*
 * #964: a restart's delay, on a thread of its own: it sleeps, then sends the
 * HAL's restart port its slot and ticket, and ends.  The HAL never waits for
 * a chora, nor for a delay: its loop goes on serving, and the restart is a
 * message like any other.
 */
#define HAL_RESTART_ID		29640	/* the timers' messages */

struct restart_msg {
	mach_msg_header_t	head;
	unsigned int		slot;
	unsigned int		ticket;
};

struct restart_timer {
	unsigned int		slot, ticket, ms;
};

static mach_port_t		restart_port = MACH_PORT_NULL;

static void *
restart_timer(void *arg)
{
	struct restart_timer	t = *(struct restart_timer *)arg;
	struct restart_msg	m;

	free(arg);
	(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, t.ms);
	memset(&m, 0, sizeof m);
	m.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m.head.msgh_size = sizeof m;
	m.head.msgh_remote_port = restart_port;
	m.head.msgh_local_port = MACH_PORT_NULL;
	m.head.msgh_id = HAL_RESTART_ID;
	m.slot = t.slot;
	m.ticket = t.ticket;
	(void)mach_msg(&m.head, MACH_SEND_MSG, sizeof m, 0, MACH_PORT_NULL,
		       MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	return NULL;
}

mach_port_t
hal_modules_restart_port(void)
{
	kern_return_t	kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&restart_port);
	if (kr == KERN_SUCCESS)
		kr = mach_port_insert_right(mach_task_self(), restart_port,
					    restart_port,
					    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("hal: no restart port (0x%x): a chora that dies is not "
		       "started again (#964)\n", kr);
		restart_port = MACH_PORT_NULL;
	}
	return restart_port;
}

/* `where', "BB:DD.F", of a record's device. */
static void
record_where(const struct chora_record *r, char *where, size_t size)
{
	(void)snprintf(where, size, "%02x:%02x.%x", r->dev.bus, r->dev.slot,
		       r->dev.func);
}

static void chora_died(struct chora_record *r);

/*
 * #964: a start the HAL was asked for -- by start_driver() or update_driver(),
 * or at the end of a chora ended to be started again.  Now, with no delay, and
 * a new row: the deaths before it were another module's, or an
 * administrator's to forgive.  A failure is a crash's, with the delays and the
 * limit.
 */
static void
chora_start_asked(struct chora_record *r)
{
	char	where[16];

	record_where(r, where, sizeof where);
	r->quick = 0;
	r->ticket++;			/* a delay still to come no longer counts */
	r->state = HAL_DRIVER_RESTARTING;
	if (chora_launch(r) == 0)
		printf("hal: the chora of %s is started on request, with %s/%s: "
		       "start %d (#964)\n", where, r->pool, r->file, r->starts);
	else
		chora_died(r);
}

/*
 * #964: the chora of `r' has died, or a start of it failed.  A death the HAL
 * asked for is no crash: a stopped driver stays ended, and one ended to be
 * started again -- updated, or started while it was still ending -- is started
 * now.  Otherwise one that lived HAL_CHORA_STEADY_S had run: its next start
 * begins a new row.  After HAL_CHORA_STARTS_MAX starts in a row that each died
 * soon, the HAL gives up and says so; otherwise it starts it again after
 * HAL_CHORA_BACKOFF_MS, doubled with each start of the row.
 */
static void
chora_died(struct chora_record *r)
{
	struct restart_timer	*t;
	tvalspec_t		now;
	pthread_t		th;
	char			where[16];
	unsigned int		ms;

	record_where(r, where, sizeof where);
	if (r->state == HAL_DRIVER_STOPPED) {
		printf("hal: the chora of %s was stopped on request: it is not "
		       "started again (#964)\n", where);
		return;
	}
	if (r->state == HAL_DRIVER_UPDATING) {
		chora_start_asked(r);
		return;
	}
	if (hal_now(&now) == 0 && r->started.tv_sec != 0 &&
	    now.tv_sec - r->started.tv_sec >= HAL_CHORA_STEADY_S)
		r->quick = 0;
	if (r->quick >= HAL_CHORA_STARTS_MAX) {
		r->state = HAL_DRIVER_GAVE_UP;
		printf("hal: the chora of %s died soon after each of its last %d "
		       "starts: it is not started again (#964)\n", where,
		       HAL_CHORA_STARTS_MAX);
		return;
	}
	ms = HAL_CHORA_BACKOFF_MS << (r->quick > 0 ? r->quick - 1 : 0);
	r->state = HAL_DRIVER_RESTARTING;
	r->ticket++;
	t = malloc(sizeof *t);
	if (t != NULL && MACH_PORT_VALID(restart_port)) {
		t->slot = (unsigned int)(r - choras);
		t->ticket = r->ticket;
		t->ms = ms;
		if (pthread_create(&th, NULL, restart_timer, t) == 0) {
			(void)pthread_detach(th);
			printf("hal: the chora of %s is started again in %u ms, "
			       "after %d start(s) (#964)\n", where, ms, r->starts);
			return;
		}
	}
	free(t);
	r->state = HAL_DRIVER_GAVE_UP;
	printf("hal: the chora of %s has no timer to start it again: it is not "
	       "started again (#964)\n", where);
}

void
hal_modules_restart_due(const mach_msg_header_t *in)
{
	const struct restart_msg	*m = (const struct restart_msg *)in;
	struct chora_record		*r;
	char				where[16];

	if (in->msgh_id != HAL_RESTART_ID || in->msgh_size < sizeof *m ||
	    m->slot >= HAL_MAX_CHORAS) {
		printf("hal: a message on the restart port that is no timer's: "
		       "id %d, %u bytes (#964)\n", (int)in->msgh_id,
		       (unsigned)in->msgh_size);
		return;
	}
	r = &choras[m->slot];
	/* A ticket that is not the record's is a delay that no longer counts. */
	if (!r->used || r->state != HAL_DRIVER_RESTARTING ||
	    r->ticket != m->ticket)
		return;
	record_where(r, where, sizeof where);
	if (chora_launch(r) == 0)
		printf("hal: the chora of %s is started again: start %d (#964)\n",
		       where, r->starts);
	else
		chora_died(r);
}

int
hal_chora_dead(mach_port_t name)
{
	mach_port_urefs_t	refs = 0;
	char			where[16];
	int			i;

	for (i = 0; i < HAL_MAX_CHORAS; i++)
		if (choras[i].used && choras[i].task != MACH_PORT_NULL &&
		    choras[i].task == name)
			break;
	if (i == HAL_MAX_CHORAS)
		return 0;
	record_where(&choras[i], where, sizeof where);
	printf("hal: the chora of %s has ended (#964)\n", where);
	/* The right the HAL held, now dead, and the one the notice brought. */
	if (mach_port_get_refs(mach_task_self(), name,
			       MACH_PORT_RIGHT_DEAD_NAME, &refs) == KERN_SUCCESS &&
	    refs > 0)
		(void)mach_port_mod_refs(mach_task_self(), name,
					 MACH_PORT_RIGHT_DEAD_NAME,
					 -(mach_port_delta_t)refs);
	choras[i].task = MACH_PORT_NULL;
	chora_died(&choras[i]);
	return 1;
}

static struct chora_record *
record_of(unsigned int bus, unsigned int slot, unsigned int func)
{
	int	i;

	for (i = 0; i < HAL_MAX_CHORAS; i++)
		if (choras[i].used && choras[i].dev.bus == bus &&
		    choras[i].dev.slot == slot && choras[i].dev.func == func)
			return &choras[i];
	return NULL;
}

mach_port_t
hal_chora_task(unsigned int bus, unsigned int slot, unsigned int func)
{
	struct chora_record	*r = record_of(bus, slot, func);

	return r != NULL ? r->task : MACH_PORT_NULL;
}

int
hal_chora_state(unsigned int bus, unsigned int slot, unsigned int func,
		int *starts)
{
	struct chora_record	*r = record_of(bus, slot, func);

	*starts = r != NULL ? r->starts : 0;
	return r != NULL ? r->state : HAL_DRIVER_NONE;
}

/*
 * #964: stop_driver().  The device's driver ends and stays ended.  It is
 * marked before its task is ended, so that its death is not taken for a
 * crash, and a delay still to come no longer counts.
 */
kern_return_t
hal_driver_stop(unsigned int bus, unsigned int slot, unsigned int func)
{
	struct chora_record	*r = record_of(bus, slot, func);
	char			where[16];

	if (r == NULL)
		return KERN_INVALID_ARGUMENT;
	if (r->state == HAL_DRIVER_STOPPED)
		return KERN_SUCCESS;
	record_where(r, where, sizeof where);
	r->state = HAL_DRIVER_STOPPED;
	r->ticket++;
	if (r->task == MACH_PORT_NULL) {
		printf("hal: the driver of %s, which does not run, is stopped on "
		       "request (#964)\n", where);
		return KERN_SUCCESS;
	}
	printf("hal: the chora of %s is stopped on request, after %d start(s) "
	       "(#964)\n", where, r->starts);
	/* Its death notice comes, and finds it stopped. */
	(void)task_terminate(r->task);
	return KERN_SUCCESS;
}

/*
 * #964: start_driver().  A driver stopped, or given up after its starts, is
 * started again with the module it has; one stopped whose chora has not yet
 * ended is started at its end.  KERN_FAILURE for one that runs, or is to be
 * started.
 */
kern_return_t
hal_driver_start(unsigned int bus, unsigned int slot, unsigned int func)
{
	struct chora_record	*r = record_of(bus, slot, func);
	char			where[16];

	if (r == NULL)
		return KERN_INVALID_ARGUMENT;
	if (r->state != HAL_DRIVER_STOPPED && r->state != HAL_DRIVER_GAVE_UP)
		return KERN_FAILURE;
	if (r->task != MACH_PORT_NULL) {
		record_where(r, where, sizeof where);
		r->state = HAL_DRIVER_UPDATING;
		printf("hal: the chora of %s, still ending, is started again at "
		       "its end, on request (#964)\n", where);
		return KERN_SUCCESS;
	}
	chora_start_asked(r);
	return KERN_SUCCESS;
}

/* A module's name: letters, digits and _, with room for ".so" after it. */
static int
module_name_ok(const char *name, size_t room)
{
	size_t	i, n = strlen(name);

	if (n == 0 || n + sizeof ".so" > room)
		return 0;
	for (i = 0; i < n; i++)
		if (!((name[i] >= 'a' && name[i] <= 'z') ||
		      (name[i] >= 'A' && name[i] <= 'Z') ||
		      (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
			return 0;
	return 1;
}

/*
 * #964: update_driver().  The device's module becomes the `len' bytes at
 * `data', named `name', once they are read to declare the device; the record
 * keeps them for every start after, the restarts after a crash among them.  A
 * chora that runs is ended and started with them at its end; a driver that
 * does not run is started with them now.  `data' is the record's when the
 * answer is KERN_SUCCESS, and the caller's otherwise.
 */
kern_return_t
hal_driver_update(unsigned int bus, unsigned int slot, unsigned int func,
		  const char *name, vm_offset_t data,
		  mach_msg_type_number_t len)
{
	struct chora_record	*r = record_of(bus, slot, func);
	char			where[16];

	if (r == NULL || !module_name_ok(name, sizeof r->file))
		return KERN_INVALID_ARGUMENT;
	record_where(r, where, sizeof where);
	if (data == 0 || len == 0 || len > CHORA_GIVEN_MODULE_MAX ||
	    module_score((const void *)data, len, &r->dev) <= 0) {
		printf("hal: %s.so, given to update the driver of %s, does not "
		       "declare it: refused (#964)\n", name, where);
		return KERN_INVALID_VALUE;
	}
	if (r->given != 0)
		(void)vm_deallocate(mach_task_self(), r->given, r->given_len);
	r->given = data;
	r->given_len = len;
	(void)snprintf(r->file, sizeof r->file, "%s.so", name);
	r->disk = 0;
	printf("hal: the driver of %s is updated on request: %s/%s, %u bytes "
	       "given (#964)\n", where, r->pool, r->file, (unsigned)len);

	if (r->state == HAL_DRIVER_UPDATING)
		return KERN_SUCCESS;	/* started with it at its end */
	if (r->task != MACH_PORT_NULL) {
		/*
		 * Running, or stopped and still ending: started at its end.
		 * Marked first, as a stop is.
		 */
		int	runs = r->state == HAL_DRIVER_RUNNING;

		r->state = HAL_DRIVER_UPDATING;
		r->ticket++;
		if (runs)
			(void)task_terminate(r->task);
		return KERN_SUCCESS;
	}
	chora_start_asked(r);
	return KERN_SUCCESS;
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
 * `on' follows the module's name where the HAL names it: " on the disk" for
 * the disk's (#964), "" for the bundle's.
 */
static void
module_read(struct pool_module *m, const char *pool, const char *on,
	    const void *data, size_t size)
{
	elf_image_t			img;
	elf_shdr_view_t			sh;
	struct chora_drives_fault	f;
	int				rc, n, i;

	m->lines = 0;
	memset(&img, 0, sizeof img);
	rc = elf_open(data, size, &img);
	if (rc != ELF_OK) {
		printf("hal: %s/%s%s is no ELF the HAL reads (%d): it declares "
		       "nothing\n", pool, m->name, on, rc);
		return;
	}
	rc = elf_shdr_find(&img, CHORA_DRIVES_SECTION, &sh);
	if (rc != ELF_OK) {
		elf_close(&img);
		return;			/* no declaration: not an error */
	}
	if (sh.data == NULL) {
		printf("hal: %s/%s%s: its %s lies outside its file: it declares "
		       "nothing\n", pool, m->name, on, CHORA_DRIVES_SECTION);
		elf_close(&img);
		return;
	}
	n = chora_drives_check(sh.data, (size_t)sh.size, &f);
	if (n < 0) {
		if (f.line < 0)
			printf("hal: %s/%s%s: its %s has %s: it declares "
			       "nothing\n", pool, m->name, on,
			       CHORA_DRIVES_SECTION, f.what);
		else
			printf("hal: %s/%s%s: line %d of its %s is %s: it "
			       "declares nothing\n", pool, m->name, on, f.line,
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

		at = snprintf(line, sizeof line, "hal: %s/%s%s drives", pool,
			      m->name, on);
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
		if (!pools[i].disk && strcmp(pools[i].name, name) == 0)
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
		module_read(&p->mods[p->n], name, "", (const void *)data,
			    data_count);
		p->n++;
		(void)vm_deallocate(mach_task_self(), data, data_count);
		q += len + 1;
	}
	if (names != 0)
		(void)vm_deallocate(mach_task_self(), names, names_count);
	return p;
}

/*
 * #964: a file of the disk, whole, into memory of its own: 0, or -1 said.
 * A module's size is bounded as a chora's is (chora_given.h).
 */
static int
disk_file(const char *path, vm_offset_t *data, mach_msg_type_number_t *len)
{
	vfs_stat_t	st;
	vfs_fd_t	fd;
	vm_offset_t	buf = 0;
	uint64_t	done = 0;
	ssize_t		r;

	*data = 0;
	*len = 0;
	fd = vfs_open(path, VFS_O_RDONLY, 0);
	if (fd == VFS_FD_INVALID) {
		printf("hal: %s would not open (#964)\n", path);
		return -1;
	}
	if (vfs_fstat(fd, &st) != 0) {
		printf("hal: %s: no size (#964)\n", path);
		(void)vfs_close(fd);
		return -1;
	}
	if (st.st_size == 0 || st.st_size > CHORA_GIVEN_MODULE_MAX ||
	    vm_allocate(mach_task_self(), &buf, (vm_size_t)st.st_size, TRUE)
	    != KERN_SUCCESS) {
		printf("hal: %s: %llu bytes, no module's size (#964)\n", path,
		       (unsigned long long)st.st_size);
		(void)vfs_close(fd);
		return -1;
	}
	while (done < st.st_size) {
		r = vfs_read(fd, (char *)buf + done, (size_t)(st.st_size - done));
		if (r <= 0)
			break;
		done += (uint64_t)r;
	}
	(void)vfs_close(fd);
	if (done != st.st_size) {
		printf("hal: %s: %llu of its %llu bytes read (#964)\n", path,
		       (unsigned long long)done, (unsigned long long)st.st_size);
		(void)vm_deallocate(mach_task_self(), buf, (vm_size_t)st.st_size);
		return -1;
	}
	*data = buf;
	*len = (mach_msg_type_number_t)done;
	return 0;
}

/*
 * #964: the disk's pool `name', HAL_DISK_POOLS<name>/, read the first time it
 * is asked for, as the bundle's is: each file of it, a module whose
 * declaration is read and kept.  A pool the disk does not have is one with
 * no module, and said.  NULL when there is no room for it, said.
 */
static struct pool *
pool_read_disk(const char *name)
{
	static vfs_dirent_t	ents[16];
	struct pool		*p;
	char			dir[VFS_PATH_MAX], path[VFS_PATH_MAX];
	uint64_t		cookie = 0;
	vfs_fd_t		fd;
	int			i, n;

	for (i = 0; i < n_pools; i++)
		if (pools[i].disk && strcmp(pools[i].name, name) == 0)
			return &pools[i];
	if (n_pools >= HAL_MAX_POOLS || strlen(name) >= sizeof pools[0].name) {
		printf("hal: no room to read the disk's pool %s\n", name);
		return NULL;
	}
	p = &pools[n_pools++];
	memset(p, 0, sizeof *p);
	strcpy(p->name, name);
	p->disk = 1;
	p->listed = 1;

	(void)snprintf(dir, sizeof dir, "%s%s", HAL_DISK_POOLS, name);
	fd = vfs_open(dir, VFS_O_RDONLY, 0);
	if (fd == VFS_FD_INVALID) {
		printf("hal: the disk has no pool %s, at %s (#964)\n", name, dir);
		return p;
	}
	do {
		n = vfs_readdir(fd, ents, sizeof ents / sizeof ents[0], &cookie);
		for (i = 0; i < n; i++) {
			vm_offset_t		data = 0;
			mach_msg_type_number_t	len = 0;

			if (ents[i].d_type != VFS_FT_REG)
				continue;	/* "." and ".." among them */
			if (strlen(ents[i].d_name) >= sizeof p->mods[0].name) {
				printf("hal: %s/%s, on the disk: a name longer "
				       "than a module's\n", name, ents[i].d_name);
				continue;
			}
			if (p->n >= HAL_POOL_MODULES) {
				printf("hal: the disk's pool %s has more than %d "
				       "modules: %s and those after it are not "
				       "read\n", name, HAL_POOL_MODULES,
				       ents[i].d_name);
				cookie = 0;
				break;
			}
			(void)snprintf(path, sizeof path, "%s/%s", dir,
				       ents[i].d_name);
			if (disk_file(path, &data, &len) != 0)
				continue;
			strcpy(p->mods[p->n].name, ents[i].d_name);
			module_read(&p->mods[p->n], name, " on the disk",
				    (const void *)data, len);
			p->n++;
			(void)vm_deallocate(mach_task_self(), data, len);
		}
	} while (n > 0 && cookie != 0);
	if (n < 0)
		printf("hal: the disk's pool %s could not be listed (#964)\n",
		       name);
	(void)vfs_close(fd);
	return p;
}

int
hal_module_for(const struct hal_device_info *dev,
	       struct hal_module_choice *c, int disk)
{
	struct pool	*p;
	int		score[HAL_POOL_MODULES];
	int		i, chosen = -1, rival = -1;

	memset(c, 0, sizeof *c);
	c->pool = pool_of_class(dev->class_rev);
	c->disk = disk;
	p = disk ? pool_read_disk(c->pool) : pool_read(c->pool);
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

/* "hal: BB:DD.F VVVV:DDDD class CCCCCC", how each of the HAL's lines on a device starts. */
static void
device_at(const struct hal_device_info *dev, char *at, size_t size)
{
	(void)snprintf(at, size, "hal: %02x:%02x.%x %04x:%04x class %06x",
		       dev->bus, dev->slot, dev->func,
		       dev->vendor_device & 0xffffu, dev->vendor_device >> 16,
		       dev->class_rev >> 8);
}

/*
 * #964: the devices no module of the bundle declares, whose class is not left
 * to others: the disk's pools are read for them once "/" is served.
 */
static struct hal_device_info	waiting[HAL_MAX_WAITING];
static int			n_waiting;

void
hal_module_report(const struct hal_device_info *dev)
{
	struct hal_module_choice	c;
	char				at[48];
	int				l;

	device_at(dev, at, sizeof at);
	l = left_by(dev->class_rev);
	switch (hal_module_for(dev, &c, 0)) {
	case HAL_MODULE_ONE:
		if (l >= 0) {
			printf("%s: %s/%s declares it, and the HAL's line leaves "
			       "class %06x/%06x to others\n", at, c.pool,
			       c.module, (unsigned)leaves[l].code,
			       (unsigned)leaves[l].mask);
			break;
		}
		printf("%s: %s/%s declares it, %s\n", at, c.pool, c.module,
		       chora_start(dev, c.pool, c.module, 0) == 0 ?
		       "and its chora is started" :
		       "and its chora could not be started");
		break;
	case HAL_MODULE_TIE:
		printf("%s: %s/%s and %s/%s declare it alike, and the HAL "
		       "chooses neither\n", at, c.pool, c.module, c.pool,
		       c.rival);
		break;
	case HAL_MODULE_NONE:
		if (l >= 0)
			printf("%s: no module of the bundle's %s declares it, and "
			       "the HAL's line leaves class %06x/%06x to "
			       "others\n", at, c.pool, (unsigned)leaves[l].code,
			       (unsigned)leaves[l].mask);
		else if (n_waiting < HAL_MAX_WAITING) {
			waiting[n_waiting++] = *dev;
			printf("%s: no module of the bundle's %s declares it; the "
			       "disk's are read once \"/\" is served\n", at,
			       c.pool);
		} else
			printf("%s: no module of the bundle's %s declares it, and "
			       "%d devices wait for the disk already\n", at,
			       c.pool, HAL_MAX_WAITING);
		break;
	default:
		printf("%s: its pool, %s, could not be read\n", at, c.pool);
		break;
	}
}

/*
 * #964: a port the name server tells, once, when "/" is mounted -- at once if
 * it is -- or MACH_PORT_NULL when no device waits for the disk, or the name
 * server would not watch, said.  The HAL alone holds it, and the name server
 * the one send-once right to it: a message on it is the name server's.
 */
mach_port_t
hal_modules_disk_watch(void)
{
	mach_port_t	watch = MACH_PORT_NULL;
	kern_return_t	kr;

	if (n_waiting == 0)
		return MACH_PORT_NULL;
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&watch);
	if (kr == KERN_SUCCESS)
		kr = netname_notify_mount(name_server_port, "/", watch);
	if (kr != KERN_SUCCESS) {
		printf("hal: \"/\" could not be watched (0x%x): %d device(s) "
		       "wait for the disk's pools for good (#964)\n", kr,
		       n_waiting);
		if (watch != MACH_PORT_NULL)
			(void)mach_port_mod_refs(mach_task_self(), watch,
						 MACH_PORT_RIGHT_RECEIVE, -1);
		return MACH_PORT_NULL;
	}
	printf("hal: %d device(s) wait for the disk's pools, read once \"/\" "
	       "is served (#964)\n", n_waiting);
	return watch;
}

/*
 * #964: "/" is served: each device that waited is decided from the disk's
 * pools, and started as one from the bundle's would be, with the bytes read
 * from the disk.
 */
void
hal_modules_disk_served(void)
{
	struct hal_module_choice	c;
	char				at[48];
	int				i;

	printf("hal: \"/\" is served: the disk's pools are read for %d "
	       "device(s) (#964)\n", n_waiting);
	for (i = 0; i < n_waiting; i++) {
		const struct hal_device_info *dev = &waiting[i];

		device_at(dev, at, sizeof at);
		switch (hal_module_for(dev, &c, 1)) {
		case HAL_MODULE_ONE:
			printf("%s: %s/%s, on the disk, declares it, %s\n", at,
			       c.pool, c.module,
			       chora_start(dev, c.pool, c.module, 1) == 0 ?
			       "and its chora is started" :
			       "and its chora could not be started");
			break;
		case HAL_MODULE_TIE:
			printf("%s: %s/%s and %s/%s, on the disk, declare it "
			       "alike, and the HAL chooses neither\n", at,
			       c.pool, c.module, c.pool, c.rival);
			break;
		case HAL_MODULE_NONE:
			printf("%s: no module of %s, in the bundle or on the "
			       "disk, declares it\n", at, c.pool);
			break;
		default:
			printf("%s: its pool on the disk, %s, could not be "
			       "read\n", at, c.pool);
			break;
		}
	}
	n_waiting = 0;
}
