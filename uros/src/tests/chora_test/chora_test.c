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
 * chora_test -- the choras the HAL starts in chora.conf (#964), on the network
 * card and on QEMU's educational device where the board has one (#977), and a
 * second chora on the card, started here as a driver's parent does (#963),
 * which the claim of the HAL's must refuse.
 *
 * bootstrap starts it from chora.conf, a list of its own, in a GRUB entry of
 * its own: every other bundle entry reads bootstrap.conf, where
 * irq_claim_test owns the card.  Its line has -D, so that a chora it starts
 * inherits the master device port with its bootstrap port; and its manifest
 * may hand on the network class and edu's and nothing else, so that what a
 * chora holds is what this task may give (#743).
 *
 *   [1] the HAL names the network card;
 *   [2] the HAL's chora claims it: once "/" is served -- the card's module is
 *       on the disk (#964) -- the kernel says the card is claimed within 30 s,
 *       and this task never started one;
 *   [3] the HAL says its driver is the task named after the card, read with
 *       hal_get_device_driver(): chora_probe@BB:DD.F;
 *   [4] a second chora on the same card, started here the HAL's way, with the
 *       card's module read from the disk, ends within 30 s, and the card is
 *       still claimed -- the claim refused it;
 *   [7] the card's module is on the disk alone: bootstrap gives no
 *       net/chora_probe.so, the disk holds it, and [3]'s driver drives the
 *       card -- a module the HAL read from the disk (#964);
 *   [5] the HAL names QEMU's edu device, vendor 1234h and device 11e8h,
 *       which has MSI and no MSI-X table (run-x86_64.sh --edu);
 *   [6] the HAL's chora claims it within 30 s, and the HAL says its driver is
 *       edu_probe@BB:DD.F.  That its module took the interrupt by message is
 *       edu_probe's line, the merge test's to read (#977);
 *   [8] edu's module is in the bundle alone: bootstrap gives
 *       misc/edu_probe.so, the disk holds none, and [6]'s driver drives edu --
 *       a module of the bundle, started at the HAL's scan;
 *   [9] where the board has QEMU's pci-testdev (run-x86_64.sh --testdev),
 *       crash_probe, which dies at its start every time, is started by the
 *       HAL HAL_CHORA_STARTS_MAX times and then given up: get_driver_state()
 *       says HAL_DRIVER_GAVE_UP within 30 s, with that many starts (#964);
 *  [10] the card's chora, killed by this task -- found by its name among the
 *       tasks the master host port lists, which this line's -H gives -- is
 *       started again by the HAL within 30 s, and its consumer's next
 *       request is served: chora_status() on the control port looked up
 *       again under the card's name, which the dead chora's answered too.
 *  [11] without the capability to administer the card's drivers
 *       (RESOURCE_DRIVER_ADMIN for its class), the HAL's stop_driver(),
 *       start_driver() and update_driver() are refused, and the card's
 *       driver goes on as it was: no token, one for edu's class, one for the
 *       card's without each routine's operation, one issued to another task
 *       -- and cap_server will not issue one for a class this manifest does
 *       not name;
 *  [12] the card's driver stopped through the HAL stays stopped: 3 s on, past
 *       every delay the HAL waits before a restart, it has the starts it had,
 *       the card is claimed by nobody and its control port is gone;
 *  [13] updated while stopped, it stays stopped with the new module;
 *  [14] started through the HAL, it runs again with one start more; a second
 *       start, while it runs, is refused;
 *  [15] edu's driver is refused updates no chora could run -- a module that
 *       does not declare edu, a name too long for a driver's task, a name
 *       the bytes have no operations for -- and goes on as it was;
 *  [16] given edu_next, read from the disk's updates, it comes back with the
 *       new module without a second start: one start more, still one 3 s
 *       on, edu_next answers on its control port, and the HAL names it.
 *
 * Every arm says one line: an arm the arms before it ended before is said
 * NOT ASKED at the end, so that none is gone from the log without a word.
 *
 * chora's own lines -- the claim, the region, the interrupt, the second
 * one's refusal -- and the kernel's line naming both tasks are the merge
 * test's to read.  The HAL's choras drive their devices for as long as they
 * run: a driver stays, and the run waits for it to say so (run-x86_64.sh).
 */

#include <mach.h>
#include <mach/bootstrap.h>		/* bootstrap_ports() */
#include <mach/mach_interface.h>	/* urmach_task_get_name() */
#include <mach/task_info.h>
#include <mach/task_name.h>		/* URMACH_TASK_NAME_MAX */
#include <mach/mach_host.h>		/* processor_set_*(): [10] */
#include <mach/thread_switch.h>
#include <mach_init.h>			/* name_server_port */
#include <servers/netname.h>
#include <servers/netname_defs.h>	/* NETNAME_SUCCESS, netname_name_t */
#include <libvfs.h>			/* the disk's pools, #964 */
#include <mach/module_pool.h>		/* module_name_t */
#include <libcap.h>			/* mach_cap_port */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "device_master.h"		/* device_dma_owned() */
#include "hal_server.h"			/* struct hal_device_info */
#include "hal.h"			/* hal_list_devices() */
#include "exec.h"			/* exec_load_image() */
#include "chora_given.h"		/* chora_give_module() */
#include "chora.h"			/* chora_status(): [10] */
#include "exec_types.h"			/* EXEC_OK */

/*
 * Not static: hal_server.h declares it extern, for the HAL's own use, and a
 * static one here would contradict that declaration.
 */
mach_port_t	master_device;

#define CT_NET_CLASS	0x020000u
#define CT_EDU_ID	0x11E81234u	/* QEMU's edu: device 11e8h, vendor 1234h */
#define CT_TESTDEV_ID	0x00051B36u	/* QEMU's pci-testdev: 0005h, 1b36h */
#define CT_HAL_TRIES	200
#define CT_WAIT_STEPS	300		/* of 100 ms: 30 s */
#define CT_ROOT_WAIT_MS	120000		/* "/" has 120 s, as mount_test gives it */
#define CT_DISK_POOLS	"/mach_servers/modules/"	/* the HAL's, #964 */
#define CT_UPDATES	"/mach_servers/updates/"	/* in no pool, #964 */
#define CT_LINE_MAX	255		/* the console's whole line, #964 */
#define CT_SETTLE_MS	3000		/* past 100+200+400+800 ms of delays */
#define CT_EDU_CLASS	0x00ff00u	/* edu's class, as its manifest line */
#define CT_SATA_CLASS	0x010601u	/* a class the manifest does not name */
#define CT_ADMIN_ALL	(CAP_OP_DRIVER_STOP | CAP_OP_DRIVER_START | \
			 CAP_OP_DRIVER_UPDATE)
#define CT_ARMS		16		/* [1] to [16], each saying one line */

#define CT_BUS(b)	((unsigned)((b) >> 8))
#define CT_SLOT(b)	((unsigned)(((b) >> 3) & 0x1F))
#define CT_FUNC(b)	((unsigned)((b) & 0x7))

static int		arms_run;
static int		arms_passed;
static unsigned char	said[CT_ARMS + 1];	/* the arms that said a line */

/* The card [1] found, for [10] to [12]. */
static natural_t	card_bdf;
static int		card_found;

/* The edu device [5] found, and its class, for [15] and [16]. */
static natural_t	edu_bdf;
static uint32_t		edu_class;
static int		edu_found;
static int		edu_looked;	/* [5] asked for it */

/*
 * An arm's line, said.  #964: the console keeps a line whole up to
 * CT_LINE_MAX bytes and its newline, and the harness reads lines, so a longer
 * one, which would come out in two, fails its arm in a line of its own, and
 * follows it for the reader.  The buffer is twice that, so a line too long is
 * measured and not cut.
 */
static void
verdict(int arm, int ok, const char *fmt, ...)
{
	char	text[2 * CT_LINE_MAX + 2], head[32];
	va_list	ap;
	size_t	len;

	va_start(ap, fmt);
	(void)vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	arms_run++;
	if (arm >= 1 && arm <= CT_ARMS)
		said[arm] = 1;
	(void)snprintf(head, sizeof head, "chora_test: [%d] %s", arm,
		       ok ? "" : "WRONG -- ");
	len = strlen(head) + strlen(text);
	if (len > CT_LINE_MAX) {
		printf("chora_test: [%d] WRONG -- its line is %u bytes, more than "
		       "the %u the console keeps whole; it follows\n", arm,
		       (unsigned)len, (unsigned)CT_LINE_MAX);
		printf("%s%s\n", head, text);
		return;
	}
	if (ok)
		arms_passed++;
	printf("%s%s\n", head, text);
}

static void
not_asked(int arm, const char *why)
{
	if (arm >= 1 && arm <= CT_ARMS)
		said[arm] = 1;
	printf("chora_test: [%d] NOT ASKED -- %s\n", arm, why);
}

/*
 * #964: every arm says one line.  One the arms before it ended before -- [3]
 * after a [2] with no "/", [13] after a [12] whose stop was refused -- is said
 * here, NOT ASKED, so that no arm is gone from the log without a word, and
 * the merge test, which reads chora_test's NOT ASKED, sees it (the review of
 * 24/11: the update's arms could go and nothing said so).
 */
static void
the_arms_not_reached(void)
{
	int	arm;

	for (arm = 1; arm <= CT_ARMS; arm++)
		if (!said[arm])
			not_asked(arm, "not reached: an arm before it ended");
}

static void
nap(unsigned int ms)
{
	(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, ms);
}

/* The HAL's devices, listed; 0, said for `arm', when they cannot be. */
static int
list_devices(int arm, vm_offset_t *buf, mach_msg_type_number_t *bytes,
	     unsigned int *n)
{
	mach_port_t	hal = MACH_PORT_NULL;
	int		tries;

	for (tries = 0; tries < CT_HAL_TRIES; tries++) {
		if (netname_look_up(name_server_port, "", (char *)"hal",
				    &hal) == KERN_SUCCESS)
			break;
		(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1);
	}
	if (!MACH_PORT_VALID(hal)) {
		not_asked(arm, "the HAL never appeared");
		return 0;
	}
	if (hal_list_devices(hal, buf, bytes, n) != KERN_SUCCESS) {
		(void)mach_port_deallocate(mach_task_self(), hal);
		verdict(arm, 0, "the HAL would not list its devices");
		return 0;
	}
	(void)mach_port_deallocate(mach_task_self(), hal);
	return 1;
}

/*
 * The name of the driver the HAL started for `bdf' (#964), waited for until it
 * reads `want' or 30 s have gone, since a chora names its task as it starts.
 * Answers what it read last, "" when the HAL named none.
 */
static void
hal_driver(natural_t bdf, const char *want, char *name, size_t size)
{
	mach_port_t		hal = MACH_PORT_NULL;
	mach_msg_type_number_t	cnt;
	int			i;

	name[0] = '\0';
	if (netname_look_up(name_server_port, "", (char *)"hal", &hal) !=
	    KERN_SUCCESS)
		return;
	for (i = 0; i < CT_WAIT_STEPS; i++) {
		memset(name, 0, size);
		cnt = (mach_msg_type_number_t)size;
		if (hal_get_device_driver(hal, (unsigned)(bdf >> 8),
					  (unsigned)((bdf >> 3) & 0x1F),
					  (unsigned)(bdf & 0x7), name, &cnt) !=
		    KERN_SUCCESS)
			cnt = 0;
		name[cnt < size ? cnt : size - 1] = '\0';
		if (strcmp(name, want) == 0)
			break;
		nap(100);
	}
	(void)mach_port_deallocate(mach_task_self(), hal);
}

/* [1]: the HAL's first device of the network class. */
static int
find_the_card(natural_t *bdf)
{
	vm_offset_t		buf = 0;
	mach_msg_type_number_t	bytes = 0;
	unsigned int		n = 0, i;
	const struct hal_device_info *devs;
	int			found = 0;

	if (!list_devices(1, &buf, &bytes, &n))
		return 0;
	devs = (const struct hal_device_info *)buf;
	for (i = 0; i < n && !found; i++)
		if ((devs[i].class_rev >> 8) == CT_NET_CLASS) {
			*bdf = (natural_t)((devs[i].bus << 8)
					   | (devs[i].slot << 3) | devs[i].func);
			found = 1;
		}
	if (bytes != 0)
		(void)vm_deallocate(mach_task_self(), buf, bytes);
	if (!found) {
		not_asked(1, "the HAL lists no device of the network class");
		return 0;
	}
	verdict(1, 1, "the HAL names the network card at %02x:%02x.%x",
		(unsigned)(*bdf >> 8), (unsigned)((*bdf >> 3) & 0x1F),
		(unsigned)(*bdf & 0x7));
	return 1;
}

/* [5]: QEMU's educational device, which has MSI and no MSI-X table (#977). */
static int
find_the_edu(natural_t *bdf)
{
	vm_offset_t		buf = 0;
	mach_msg_type_number_t	bytes = 0;
	unsigned int		n = 0, i;
	const struct hal_device_info *devs;
	int			found = 0;

	if (!list_devices(5, &buf, &bytes, &n))
		return 0;
	devs = (const struct hal_device_info *)buf;
	for (i = 0; i < n && !found; i++)
		if (devs[i].vendor_device == CT_EDU_ID) {
			*bdf = (natural_t)((devs[i].bus << 8)
					   | (devs[i].slot << 3) | devs[i].func);
			edu_class = devs[i].class_rev >> 8;	/* [13] */
			found = 1;
		}
	if (bytes != 0)
		(void)vm_deallocate(mach_task_self(), buf, bytes);
	if (!found) {
		not_asked(5, "no edu device on this board (run-x86_64.sh --edu)");
		return 0;
	}
	verdict(5, 1, "the HAL names the edu device at %02x:%02x.%x",
		(unsigned)(*bdf >> 8), (unsigned)((*bdf >> 3) & 0x1F),
		(unsigned)(*bdf & 0x7));
	return 1;
}

/* NUL-separated strings, then the empty string that ends them (exec_types.h). */
static size_t
blob_add(char *blob, size_t at, size_t cap, const char *s)
{
	size_t	len = strlen(s) + 1;

	if (at + len >= cap)
		return cap;
	memcpy(blob + at, s, len);
	return at + len;
}

/*
 * A chora on `bdf' with the module `module' of the class `class', started the
 * way the HAL starts one (#964): a task made here, given the drivers' bootstrap
 * port, and loaded by exec_server from chora's image, which bootstrap gives,
 * with a manifest naming this one function, and given the module's bytes
 * `mod', `mod_len', on a port of its own.  So it holds the driver's port and
 * the one capability, as the HAL's does, and passes the checks every driver
 * makes as it starts.  Its first thread is resumed.
 */
static kern_return_t
start_chora(mach_port_t image_port, natural_t bdf, const char *class,
	    const char *module, vm_offset_t mod, mach_msg_type_number_t mod_len,
	    mach_port_t *task_out, int *rc_out)
{
	static char		envp_blob[] = "";
	char			blob[80], where[8], manifest[256], boxname[16];
	size_t			at = 0;
	mach_port_t		task = MACH_PORT_NULL, thread = MACH_PORT_NULL;
	mach_port_t		drivers = MACH_PORT_NULL, box = MACH_PORT_NULL;
	vm_offset_t		image = 0;
	mach_msg_type_number_t	image_len = 0;
	module_name_t		prog;
	cap_manifest_entry_t	want;
	unsigned int		mlen;
	kern_return_t		kr;
	int			rc = -1;

	(void)snprintf(where, sizeof where, "%02x:%02x.%x",
		       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
		       (unsigned)(bdf & 0x7));
	memset(&want, 0, sizeof want);
	want.type = RESOURCE_PCI_FUNCTION;
	want.resource_id = bdf;
	want.ops = CAP_OP_PCI_DMA_MAP | CAP_OP_PCI_MMIO_MAP | CAP_OP_PCI_IRQ;
	mlen = cap_manifest_build("chora", &want, 1, NULL, 0, manifest,
				  sizeof manifest);
	if (mlen == 0)
		return KERN_INVALID_ARGUMENT;

	strcpy(prog, "chora");
	kr = bootstrap_drivers_port(bootstrap_port, &drivers);
	if (kr == KERN_SUCCESS)
		kr = bootstrap_get_program(bootstrap_port, prog, &image,
					   &image_len);
	if (kr == KERN_SUCCESS)
		kr = task_create(mach_task_self(), NULL, 0, FALSE, &task);
	if (kr == KERN_SUCCESS)
		kr = task_set_bootstrap_port(task, drivers);
	if (kr == KERN_SUCCESS)
		box = chora_give_module(task, mod, mod_len, &kr);
	if (kr == KERN_SUCCESS) {
		(void)snprintf(boxname, sizeof boxname, "0x%x", (unsigned)box);
		at = blob_add(blob, at, sizeof blob, "chora");
		at = blob_add(blob, at, sizeof blob, class);
		at = blob_add(blob, at, sizeof blob, module);
		at = blob_add(blob, at, sizeof blob, where);
		at = blob_add(blob, at, sizeof blob, boxname);
		if (at >= sizeof blob)
			kr = KERN_INVALID_ARGUMENT;
		else
			blob[at++] = '\0';
	}
	if (kr == KERN_SUCCESS)
		kr = exec_load_image(image_port, task, mach_cap_port,
				     (pointer_t)image, image_len,
				     (pointer_t)manifest, mlen,
				     (pointer_t)blob, (mach_msg_type_number_t)at,
				     (pointer_t)envp_blob,
				     (mach_msg_type_number_t)1, &thread, &rc);
	*rc_out = rc;
	if (kr == KERN_SUCCESS && rc == EXEC_OK)
		kr = thread_resume(thread);
	if (MACH_PORT_VALID(thread))
		(void)mach_port_deallocate(mach_task_self(), thread);
	if (image != 0)
		(void)vm_deallocate(mach_task_self(), image, image_len);
	if (MACH_PORT_VALID(drivers))
		(void)mach_port_deallocate(mach_task_self(), drivers);
	if (kr != KERN_SUCCESS || rc != EXEC_OK) {
		if (MACH_PORT_VALID(task)) {
			(void)task_terminate(task);
			(void)mach_port_deallocate(mach_task_self(), task);
		}
		return kr != KERN_SUCCESS ? kr : KERN_FAILURE;
	}
	*task_out = task;
	return KERN_SUCCESS;
}

/*
 * #964: whether "/" is mounted within CT_ROOT_WAIT_MS, on the name server's
 * word: netname_notify_mount(), and its one message, waited for with a bound.
 */
static int
root_served(void)
{
	netname_notify_msg_t	m;
	mach_port_t		recv = MACH_PORT_NULL;
	kern_return_t		kr;
	int			ok = 0;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&recv);
	if (kr == KERN_SUCCESS)
		kr = netname_notify_mount(name_server_port, "/", recv);
	if (kr == KERN_SUCCESS) {
		memset(&m, 0, sizeof m);
		kr = mach_msg(&m.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			      sizeof m, recv, CT_ROOT_WAIT_MS, MACH_PORT_NULL);
		if (kr == KERN_SUCCESS) {
			ok = (m.head.msgh_id == NETNAME_NOTIFY_MOUNT_MSID);
			mach_msg_destroy(&m.head);
		}
	}
	if (MACH_PORT_VALID(recv))
		(void)mach_port_mod_refs(mach_task_self(), recv,
					 MACH_PORT_RIGHT_RECEIVE, -1);
	return ok;
}

/* #964: a file of the disk, whole, in memory of its own. */
static kern_return_t
disk_file(const char *path, vm_offset_t *data, mach_msg_type_number_t *len)
{
	vfs_stat_t	st;
	vfs_fd_t	fd;
	vm_offset_t	buf = 0;
	uint64_t	done = 0;
	ssize_t		r;

	fd = vfs_open(path, VFS_O_RDONLY, 0);
	if (fd == VFS_FD_INVALID)
		return KERN_INVALID_NAME;
	if (vfs_fstat(fd, &st) != 0 || st.st_size == 0 ||
	    st.st_size > CHORA_GIVEN_MODULE_MAX ||
	    vm_allocate(mach_task_self(), &buf, (vm_size_t)st.st_size, TRUE)
	    != KERN_SUCCESS) {
		(void)vfs_close(fd);
		return KERN_INVALID_VALUE;
	}
	while (done < st.st_size &&
	       (r = vfs_read(fd, (char *)buf + done,
			     (size_t)(st.st_size - done))) > 0)
		done += (uint64_t)r;
	(void)vfs_close(fd);
	if (done != st.st_size) {
		(void)vm_deallocate(mach_task_self(), buf, (vm_size_t)st.st_size);
		return KERN_FAILURE;
	}
	*data = buf;
	*len = (mach_msg_type_number_t)done;
	return KERN_SUCCESS;
}

/* #964: a module of the disk's pools, CT_DISK_POOLS<pool>/<file>, whole. */
static kern_return_t
disk_module(const char *pool, const char *file, vm_offset_t *data,
	    mach_msg_type_number_t *len)
{
	char	path[VFS_PATH_MAX];

	(void)snprintf(path, sizeof path, "%s%s/%s", CT_DISK_POOLS, pool, file);
	return disk_file(path, data, len);
}

/*
 * #964: where a module is: whether bootstrap gives `pool'/`file' from the
 * bundle, and whether the disk's pools hold it.
 */
static void
module_where(const char *pool, const char *file, int *in_bundle, int *on_disk)
{
	module_name_t		c, f;
	vm_offset_t		data = 0;
	mach_msg_type_number_t	len = 0;
	char			path[VFS_PATH_MAX];
	vfs_stat_t		st;

	strcpy(c, pool);
	strcpy(f, file);
	*in_bundle = bootstrap_get_module(bootstrap_port, c, f, &data, &len)
		     == KERN_SUCCESS;
	if (*in_bundle && data != 0)
		(void)vm_deallocate(mach_task_self(), data, len);
	(void)snprintf(path, sizeof path, "%s%s/%s", CT_DISK_POOLS, pool, file);
	*on_disk = vfs_stat(path, &st) == 0;
}

/* Whether somebody holds a claim on `bdf'; this task holds none. */
static int
claimed(natural_t bdf)
{
	natural_t by_other = 0;

	return device_dma_owned(master_device, bdf, &by_other) == KERN_SUCCESS
	       && by_other != 0;
}

/* Whether `task' has ended: its port is a dead name, and asking it fails. */
static int
ended(mach_port_t task)
{
	struct task_basic_info	info;
	mach_msg_type_number_t	count = TASK_BASIC_INFO_COUNT;

	return task_info(task, TASK_BASIC_INFO, (task_info_t)&info, &count)
	       != KERN_SUCCESS;
}

/*
 * [1] to [8], in order; one that cannot go on says so and ends them, and
 * the_arms_not_reached() says the rest.
 */
static void
run_the_arms(void)
{
	mach_port_t	host = MACH_PORT_NULL, root_wired = MACH_PORT_NULL;
	mach_port_t	root_paged = MACH_PORT_NULL, security = MACH_PORT_NULL;
	mach_port_t	exec_port = MACH_PORT_NULL;
	mach_port_t	second = MACH_PORT_NULL;
	natural_t	bdf = 0, edu = 0;
	char		name[URMACH_TASK_NAME_MAX], want[URMACH_TASK_NAME_MAX];
	char		card_driver[URMACH_TASK_NAME_MAX];
	vm_offset_t	mod = 0;
	mach_msg_type_number_t mod_len = 0;
	kern_return_t	kr;
	int		i, rc = 0, in_bundle, on_disk, card_ok;

	kr = bootstrap_ports(bootstrap_port, &host, &master_device,
			     &root_wired, &root_paged, &security);
	if (kr != KERN_SUCCESS || !MACH_PORT_VALID(master_device)) {
		not_asked(1, "no master device port: this line has no -D");
		return;
	}
	if (!find_the_card(&bdf))
		return;
	card_bdf = bdf;
	card_found = 1;

	/* [2]: the card's module is on the disk, read once "/" is served */
	if (!root_served()) {
		verdict(2, 0, "no \"/\" in %u s, and the card's module is on "
			"the disk, which the HAL reads once \"/\" is served",
			CT_ROOT_WAIT_MS / 1000);
		return;
	}
	for (i = 0; i < CT_WAIT_STEPS && !claimed(bdf); i++)
		nap(100);
	verdict(2, claimed(bdf), "the HAL's chora on the card, and %s",
		claimed(bdf) ? "the kernel says the card is claimed" :
		"30 s on the kernel says nobody claims it");

	/* [3] */
	(void)snprintf(want, sizeof want, "chora_probe@%02x:%02x.%x",
		       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
		       (unsigned)(bdf & 0x7));
	hal_driver(bdf, want, name, sizeof name);
	card_ok = strcmp(name, want) == 0;
	(void)snprintf(card_driver, sizeof card_driver, "%s", name);
	verdict(3, card_ok, "the HAL says the card's driver is "
		"\"%s\", and the card's name is \"%s\"", name, want);

	/* [4]: a chora this task starts, the HAL's way */
	for (i = 0; i < CT_WAIT_STEPS; i++) {
		if (netname_look_up(name_server_port, "", (char *)"exec_image",
				    &exec_port) == KERN_SUCCESS)
			break;
		nap(100);
	}
	if (!MACH_PORT_VALID(exec_port)) {
		not_asked(4, "exec_server's image port did not come in 30 s");
		return;
	}
	kr = disk_module("net", "chora_probe.so", &mod, &mod_len);
	if (kr != KERN_SUCCESS) {
		verdict(4, 0, "the card's module, %snet/chora_probe.so, could not "
			"be read from the disk: 0x%x", CT_DISK_POOLS, kr);
		return;
	}
	kr = start_chora(exec_port, bdf, "net", "chora_probe", mod, mod_len,
			 &second, &rc);
	(void)vm_deallocate(mach_task_self(), mod, mod_len);
	if (kr != KERN_SUCCESS) {
		verdict(4, 0, "a second chora did not start: kr 0x%x, result %d",
			kr, rc);
		return;
	}
	for (i = 0; i < CT_WAIT_STEPS && !ended(second); i++)
		nap(100);
	verdict(4, ended(second) && claimed(bdf), "a second chora on the same "
		"card %s, and the card is %s", ended(second) ? "ended" :
		"was still there after 30 s", claimed(bdf) ? "still claimed" :
		"claimed by nobody");
	(void)mach_port_deallocate(mach_task_self(), second);

	/* [7]: the card's module, on the disk alone */
	module_where("net", "chora_probe.so", &in_bundle, &on_disk);
	verdict(7, !in_bundle && on_disk && card_ok, "the card's module, "
		"net/chora_probe.so, %s the bundle and %s the disk, and the "
		"card's driver is \"%s\"", in_bundle ? "IS in" : "is not in",
		on_disk ? "on" : "NOT on", card_driver);

	/* [5], [6], and [8] */
	edu_looked = 1;
	if (!find_the_edu(&edu))
		return;
	edu_bdf = edu;
	edu_found = 1;
	for (i = 0; i < CT_WAIT_STEPS && !claimed(edu); i++)
		nap(100);
	(void)snprintf(want, sizeof want, "edu_probe@%02x:%02x.%x",
		       (unsigned)(edu >> 8), (unsigned)((edu >> 3) & 0x1F),
		       (unsigned)(edu & 0x7));
	hal_driver(edu, want, name, sizeof name);
	verdict(6, claimed(edu) && strcmp(name, want) == 0, "the HAL's chora on "
		"the edu device, %s, and the HAL says its driver is \"%s\", and "
		"the device's name is \"%s\"", claimed(edu) ?
		"which the kernel says is claimed" :
		"which 30 s on nobody claims", name, want);

	/* [8]: edu's module, in the bundle alone */
	module_where("misc", "edu_probe.so", &in_bundle, &on_disk);
	verdict(8, in_bundle && !on_disk && strcmp(name, want) == 0, "edu's "
		"module, misc/edu_probe.so, %s the bundle and %s the disk, and "
		"edu's driver is \"%s\"", in_bundle ? "is in" : "is NOT in",
		on_disk ? "IS on" : "not on", name);
}

/*
 * [9] (#964): crash_probe on the pci-testdev, started by the HAL again after
 * each death, at most HAL_CHORA_STARTS_MAX times, then given up.  The delays
 * add up to well under 30 s: 100, 200, 400 and 800 ms.
 */
static void
the_crash_arm(void)
{
	vm_offset_t		buf = 0;
	mach_msg_type_number_t	bytes = 0;
	const struct hal_device_info *devs;
	mach_port_t		hal = MACH_PORT_NULL;
	unsigned int		n = 0, i, bus = 0, slot = 0, func = 0;
	int			found = 0, state = -1, starts = -1, steps;
	kern_return_t		kr = KERN_FAILURE;

	if (!list_devices(9, &buf, &bytes, &n))
		return;
	devs = (const struct hal_device_info *)buf;
	for (i = 0; i < n && !found; i++)
		if (devs[i].vendor_device == CT_TESTDEV_ID) {
			bus = devs[i].bus;
			slot = devs[i].slot;
			func = devs[i].func;
			found = 1;
		}
	if (bytes != 0)
		(void)vm_deallocate(mach_task_self(), buf, bytes);
	if (!found) {
		not_asked(9, "no pci-testdev on this board (run-x86_64.sh "
			  "--testdev)");
		return;
	}
	if (netname_look_up(name_server_port, "", (char *)"hal", &hal)
	    != KERN_SUCCESS) {
		verdict(9, 0, "the HAL's name is gone");
		return;
	}
	for (steps = 0; steps < CT_WAIT_STEPS; steps++) {
		kr = hal_get_driver_state(hal, bus, slot, func, &state, &starts);
		if (kr != KERN_SUCCESS || state == HAL_DRIVER_GAVE_UP)
			break;
		nap(100);
	}
	(void)mach_port_deallocate(mach_task_self(), hal);
	verdict(9, kr == KERN_SUCCESS && state == HAL_DRIVER_GAVE_UP &&
		starts == HAL_CHORA_STARTS_MAX, "crash_probe on the pci-testdev "
		"at %02x:%02x.%x: the HAL says state %d (given up is %d) after "
		"%d start(s), where the limit is %d (0x%x)", bus, slot, func,
		state, HAL_DRIVER_GAVE_UP, starts, HAL_CHORA_STARTS_MAX, kr);
}

/*
 * #964 [10]: the task named `name' among those the master host port lists --
 * this line's -H gives it -- terminated.  0, or why not.
 */
static const char *
kill_by_name(const char *name)
{
	mach_port_t		host = MACH_PORT_NULL, dev = MACH_PORT_NULL;
	mach_port_t		wired = MACH_PORT_NULL, paged = MACH_PORT_NULL;
	mach_port_t		sec = MACH_PORT_NULL, pset_name = MACH_PORT_NULL;
	mach_port_t		pset = MACH_PORT_NULL, me = mach_task_self();
	task_port_array_t	tasks = 0;
	mach_msg_type_number_t	ntasks = 0, i;
	const char		*why = "no task of that name";
	kern_return_t		kr;

	kr = bootstrap_ports(bootstrap_port, &host, &dev, &wired, &paged, &sec);
	if (kr == KERN_SUCCESS)
		kr = processor_set_default(host, &pset_name);
	if (kr == KERN_SUCCESS &&
	    host_processor_set_priv(host, pset_name, &pset) != KERN_SUCCESS)
		return "not asked: the host port this line has is not the master "
		       "host port (no -H)";
	if (kr == KERN_SUCCESS)
		kr = processor_set_tasks(pset, &tasks, &ntasks);
	if (kr != KERN_SUCCESS)
		return "not asked: the tasks could not be listed";
	for (i = 0; i < ntasks; i++) {
		char			got[URMACH_TASK_NAME_MAX];
		mach_msg_type_number_t	cnt = sizeof got;

		memset(got, 0, sizeof got);
		if (why != NULL && tasks[i] != me &&
		    urmach_task_get_name(tasks[i], got, &cnt) == KERN_SUCCESS &&
		    strcmp(got, name) == 0) {
			why = task_terminate(tasks[i]) == KERN_SUCCESS ? NULL
			      : "task_terminate() refused";
		}
		(void)mach_port_deallocate(me, tasks[i]);
	}
	if (tasks != 0)
		(void)vm_deallocate(me, (vm_address_t)tasks,
				    ntasks * sizeof(*tasks));
	if (MACH_PORT_VALID(pset))
		(void)mach_port_deallocate(me, pset);
	if (MACH_PORT_VALID(pset_name))
		(void)mach_port_deallocate(me, pset_name);
	return why;
}

/* #964 [10]: chora_status() on the port checked in as `name', or why not. */
static kern_return_t
ask_chora(const char *name, mach_port_t *port, char *module, size_t size,
	  unsigned int *bdf)
{
	mach_msg_type_number_t	cnt = (mach_msg_type_number_t)size;
	kern_return_t		kr;

	*port = MACH_PORT_NULL;
	kr = netname_look_up(name_server_port, "", (char *)name, port);
	if (kr != KERN_SUCCESS)
		return kr;
	memset(module, 0, size);
	return chora_status(*port, module, &cnt, bdf);
}

static void
the_kill_arm(void)
{
	char		name[URMACH_TASK_NAME_MAX], module[64];
	mach_port_t	before = MACH_PORT_NULL, after = MACH_PORT_NULL;
	mach_port_t	hal = MACH_PORT_NULL;
	unsigned int	bdf = 0, bus, slot, func;
	const char	*why;
	kern_return_t	kr;
	int		steps, state = -1, starts = -1, served = 0;

	if (!card_found) {
		not_asked(10, "no network card");
		return;
	}
	bus = (unsigned)(card_bdf >> 8);
	slot = (unsigned)((card_bdf >> 3) & 0x1F);
	func = (unsigned)(card_bdf & 0x7);
	(void)snprintf(name, sizeof name, "chora_probe@%02x:%02x.%x", bus, slot,
		       func);

	/* The consumer's request, to the chora as it runs. */
	kr = ask_chora(name, &before, module, sizeof module, &bdf);
	if (kr != KERN_SUCCESS || strcmp(module, "chora_probe") != 0 ||
	    bdf != card_bdf) {
		verdict(10, 0, "the card's chora, asked on the control port "
			"checked in as \"%s\" before the kill: 0x%x, module "
			"\"%s\", device 0x%x", name, kr, module, bdf);
		return;
	}

	why = kill_by_name(name);
	if (why != NULL) {
		if (strncmp(why, "not asked", 9) == 0)
			not_asked(10, why);
		else
			verdict(10, 0, "the card's chora \"%s\" not killed: %s",
				name, why);
		return;
	}

	/* The HAL starts it again; the consumer asks again, by the name. */
	if (netname_look_up(name_server_port, "", (char *)"hal", &hal)
	    != KERN_SUCCESS) {
		verdict(10, 0, "the HAL's name is gone");
		return;
	}
	for (steps = 0; steps < CT_WAIT_STEPS && !served; steps++) {
		(void)hal_get_driver_state(hal, bus, slot, func, &state,
					   &starts);
		if (state == HAL_DRIVER_RUNNING && starts >= 2) {
			kr = ask_chora(name, &after, module, sizeof module,
				       &bdf);
			served = kr == KERN_SUCCESS && after != before &&
				 strcmp(module, "chora_probe") == 0 &&
				 bdf == card_bdf;
			if (!served && MACH_PORT_VALID(after))
				(void)mach_port_deallocate(mach_task_self(),
							   after);
		}
		if (!served)
			nap(100);
	}
	(void)mach_port_deallocate(mach_task_self(), hal);
	verdict(10, served && claimed(card_bdf), "the card's chora killed by "
		"this task: the HAL says state %d (running is %d) after %d "
		"start(s); asked again under \"%s\", %s, module \"%s\", device "
		"0x%x; the card %s", state, HAL_DRIVER_RUNNING, starts, name,
		served ? "a new port answered" : "no answer from a new port",
		module, bdf, claimed(card_bdf) ? "claimed again" :
		"claimed by nobody");
	if (MACH_PORT_VALID(before))
		(void)mach_port_deallocate(mach_task_self(), before);
	if (served)
		(void)mach_port_deallocate(mach_task_self(), after);
}

/* #964 [11]-[13]: a RESOURCE_DRIVER_ADMIN token of this task's. */
static kern_return_t
admin_token(uint32_t class, uint64_t ops, struct uros_cap *t)
{
	memset(t, 0, sizeof *t);
	return cap_request(RESOURCE_DRIVER_ADMIN, class, ops, 0, t);
}

/* #964 [11]-[13]: the HAL's state of the driver of `bdf', and its starts. */
static void
driver_state(mach_port_t hal, natural_t bdf, int *state, int *starts)
{
	*state = -1;
	*starts = -1;
	(void)hal_get_driver_state(hal, CT_BUS(bdf), CT_SLOT(bdf), CT_FUNC(bdf),
				   state, starts);
}

/* #964 [11]-[13]: the port the name server gives for "hal", or none. */
static mach_port_t
the_hal(void)
{
	mach_port_t	hal = MACH_PORT_NULL;

	if (netname_look_up(name_server_port, "", (char *)"hal", &hal) !=
	    KERN_SUCCESS)
		return MACH_PORT_NULL;
	return hal;
}

/*
 * [11] (#964): without the capability, the HAL's three routines are refused
 * for the card, each with the kernel's answer, and the card's driver goes on
 * as it was -- the same state, the same starts, the same port.  update_driver()
 * is given the card's own module, so that only the capability can refuse it.
 * Each routine is refused a token for the card's class without its own
 * operation: the operation is what tells the three apart.
 */
static void
the_refusal_arm(void)
{
	struct uros_cap		all, start_only, no_start, edu_tok, foreign;
	struct uros_cap		sata;
	mach_port_t		hal, before = MACH_PORT_NULL, after = MACH_PORT_NULL;
	char			name[URMACH_TASK_NAME_MAX], module[64];
	vm_offset_t		mod = 0;
	mach_msg_type_number_t	mod_len = 0;
	natural_t		b = card_bdf;
	unsigned int		bdf = 0;
	kern_return_t		kr, k_sata, k_none, k_stop, k_start, k_update;
	kern_return_t		k_nostop, k_nostart, k_noupdate, k_foreign;
	kern_return_t		k_before, k_after;
	int			state0, starts0, state1, starts1, same;

	if (!card_found) {
		not_asked(11, "no network card");
		return;
	}
	hal = the_hal();
	if (!MACH_PORT_VALID(hal)) {
		verdict(11, 0, "the HAL's name is gone");
		return;
	}
	(void)snprintf(name, sizeof name, "chora_probe@%02x:%02x.%x", CT_BUS(b),
		       CT_SLOT(b), CT_FUNC(b));

	/*
	 * This task's tokens: the card's class, all three operations, the start
	 * alone, and all but the start; edu's class; and a child of the first
	 * issued to the HAL.
	 */
	kr = admin_token(CT_NET_CLASS, CT_ADMIN_ALL, &all);
	if (kr == KERN_SUCCESS)
		kr = admin_token(CT_NET_CLASS, CAP_OP_DRIVER_START, &start_only);
	if (kr == KERN_SUCCESS)
		kr = admin_token(CT_NET_CLASS, CAP_OP_DRIVER_STOP |
				 CAP_OP_DRIVER_UPDATE, &no_start);
	if (kr == KERN_SUCCESS)
		kr = admin_token(CT_EDU_CLASS, CT_ADMIN_ALL, &edu_tok);
	if (kr == KERN_SUCCESS)
		kr = cap_derive_for(&all, CAP_OP_DRIVER_STOP, 0, hal, &foreign);
	if (kr == KERN_SUCCESS)
		kr = disk_module("net", "chora_probe.so", &mod, &mod_len);
	if (kr != KERN_SUCCESS) {
		verdict(11, 0, "no tokens for the card's class and edu's, or no "
			"card's module from the disk: %d", kr);
		(void)mach_port_deallocate(mach_task_self(), hal);
		return;
	}
	k_sata = admin_token(CT_SATA_CLASS, CAP_OP_DRIVER_STOP, &sata);

	driver_state(hal, b, &state0, &starts0);
	k_before = ask_chora(name, &before, module, sizeof module, &bdf);

	k_none = hal_stop_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				 (char *)&all, 0);
	k_stop = hal_stop_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				 (char *)&edu_tok, sizeof edu_tok);
	k_start = hal_start_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				   (char *)&edu_tok, sizeof edu_tok);
	k_update = hal_update_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				     (char *)&edu_tok, sizeof edu_tok,
				     "chora_probe", sizeof "chora_probe" - 1,
				     mod, mod_len);
	k_nostop = hal_stop_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				   (char *)&start_only, sizeof start_only);
	k_nostart = hal_start_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				     (char *)&no_start, sizeof no_start);
	k_noupdate = hal_update_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				       (char *)&start_only, sizeof start_only,
				       "chora_probe",
				       sizeof "chora_probe" - 1, mod, mod_len);
	k_foreign = hal_stop_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				    (char *)&foreign, sizeof foreign);
	(void)vm_deallocate(mach_task_self(), mod, mod_len);

	driver_state(hal, b, &state1, &starts1);
	k_after = ask_chora(name, &after, module, sizeof module, &bdf);
	same = k_before == KERN_SUCCESS && k_after == KERN_SUCCESS &&
	       after == before;
	verdict(11, k_sata == CAP_ERR_NOT_IN_MANIFEST &&
		k_none == CAP_ERR_INVALID_TOKEN &&
		k_stop == CAP_ERR_RESOURCE_MISMATCH &&
		k_start == CAP_ERR_RESOURCE_MISMATCH &&
		k_update == CAP_ERR_RESOURCE_MISMATCH &&
		k_nostop == CAP_ERR_OP_NOT_ALLOWED &&
		k_nostart == CAP_ERR_OP_NOT_ALLOWED &&
		k_noupdate == CAP_ERR_OP_NOT_ALLOWED &&
		k_foreign == CAP_ERR_NOT_OWNER && state0 == HAL_DRIVER_RUNNING &&
		state1 == HAL_DRIVER_RUNNING && starts1 == starts0 && same,
		"refused without the capability: cap_server SATA %d; HAL no "
		"token %d, edu's %d %d %d, no stop %d, no start %d, no update "
		"%d, HAL's own %d; driver %d then %d, %d then %d start(s), asked "
		"%d %d, %s", k_sata, k_none, k_stop, k_start, k_update, k_nostop,
		k_nostart, k_noupdate, k_foreign, state0, state1, starts0,
		starts1, k_before, k_after, same ? "same port" : "port CHANGED");
	if (MACH_PORT_VALID(before))
		(void)mach_port_deallocate(mach_task_self(), before);
	if (MACH_PORT_VALID(after))
		(void)mach_port_deallocate(mach_task_self(), after);
	(void)mach_port_deallocate(mach_task_self(), hal);
}

/*
 * [12], [13], [14] (#964): the card's driver stopped through the HAL stays
 * stopped -- 3 s on, past every delay the HAL waits before a restart, with the
 * starts it had, the card claimed by nobody and its control port gone; updated
 * while stopped, it stays stopped with the new module, which an update does
 * not start (decided with the user, 24/11); started through the HAL, it runs
 * again with one start more, and a second start, while it runs, is refused.
 */
static void
the_stop_arm(void)
{
	struct uros_cap		tok;
	mach_port_t		hal, port = MACH_PORT_NULL, gone = MACH_PORT_NULL;
	char			name[URMACH_TASK_NAME_MAX], module[64];
	vm_offset_t		mod = 0;
	mach_msg_type_number_t	mod_len = 0;
	natural_t		b = card_bdf;
	unsigned int		bdf = 0;
	kern_return_t		kr, k_gone, k_upd, k_again;
	int			i, state0, starts0, state1, starts1, state2;
	int			starts2, state3 = -1, starts3 = -1, state4, starts4;
	int			freed, freed2, claimed3, answered = 0;

	if (!card_found) {
		not_asked(12, "no network card");
		return;
	}
	hal = the_hal();
	if (!MACH_PORT_VALID(hal)) {
		verdict(12, 0, "the HAL's name is gone");
		return;
	}
	(void)snprintf(name, sizeof name, "chora_probe@%02x:%02x.%x", CT_BUS(b),
		       CT_SLOT(b), CT_FUNC(b));
	kr = admin_token(CT_NET_CLASS, CT_ADMIN_ALL, &tok);
	if (kr == KERN_SUCCESS)
		kr = disk_module("net", "chora_probe.so", &mod, &mod_len);
	if (kr != KERN_SUCCESS) {
		verdict(12, 0, "no token for the card's class, or no card's "
			"module from the disk: %d", kr);
		(void)mach_port_deallocate(mach_task_self(), hal);
		return;
	}

	/* [12]: stopped, and still so once every restart's delay has gone by. */
	driver_state(hal, b, &state0, &starts0);
	kr = hal_stop_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
			     (char *)&tok, sizeof tok);
	if (kr != KERN_SUCCESS) {
		verdict(12, 0, "stop_driver refused for the card: %d", kr);
		(void)vm_deallocate(mach_task_self(), mod, mod_len);
		(void)mach_port_deallocate(mach_task_self(), hal);
		return;
	}
	for (i = 0; i < CT_WAIT_STEPS; i++) {
		driver_state(hal, b, &state1, &starts1);
		if (state1 == HAL_DRIVER_STOPPED && !claimed(b))
			break;
		nap(100);
	}
	nap(CT_SETTLE_MS);
	driver_state(hal, b, &state1, &starts1);
	freed = !claimed(b);
	k_gone = netname_look_up(name_server_port, "", name, &gone);
	if (k_gone == KERN_SUCCESS)
		(void)mach_port_deallocate(mach_task_self(), gone);
	verdict(12, state0 == HAL_DRIVER_RUNNING &&
		state1 == HAL_DRIVER_STOPPED && starts1 == starts0 && freed &&
		k_gone != KERN_SUCCESS, "the card's driver stopped through the "
		"HAL: state %d, 3 s on %d (stopped is %d), %d start(s) then %d, "
		"the card %s, its port %s", state0, state1, HAL_DRIVER_STOPPED,
		starts0, starts1, freed ? "free" : "CLAIMED",
		k_gone == KERN_SUCCESS ? "STILL THERE" : "gone");

	/* [13]: updated while stopped, it stays stopped. */
	k_upd = hal_update_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				  (char *)&tok, sizeof tok, "chora_probe",
				  sizeof "chora_probe" - 1, mod, mod_len);
	(void)vm_deallocate(mach_task_self(), mod, mod_len);
	nap(1000);
	driver_state(hal, b, &state2, &starts2);
	freed2 = !claimed(b);
	verdict(13, k_upd == KERN_SUCCESS && state2 == HAL_DRIVER_STOPPED &&
		starts2 == starts1 && freed2, "the card's driver updated while "
		"stopped (%d): 1 s on state %d (stopped is %d), %d start(s) then "
		"%d, the card %s", k_upd, state2, HAL_DRIVER_STOPPED, starts1,
		starts2, freed2 ? "free" : "CLAIMED");

	/* [14]: started, one start more, and a new port answers. */
	kr = hal_start_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
			      (char *)&tok, sizeof tok);
	for (i = 0; kr == KERN_SUCCESS && i < CT_WAIT_STEPS && !answered; i++) {
		driver_state(hal, b, &state3, &starts3);
		if (state3 == HAL_DRIVER_RUNNING && starts3 == starts2 + 1 &&
		    ask_chora(name, &port, module, sizeof module, &bdf) ==
		    KERN_SUCCESS && strcmp(module, "chora_probe") == 0 &&
		    bdf == b && claimed(b))
			answered = 1;
		else {
			if (MACH_PORT_VALID(port))
				(void)mach_port_deallocate(mach_task_self(),
							   port);
			port = MACH_PORT_NULL;
			nap(100);
		}
	}
	k_again = hal_start_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				   (char *)&tok, sizeof tok);
	driver_state(hal, b, &state4, &starts4);
	claimed3 = claimed(b);
	verdict(14, kr == KERN_SUCCESS && answered && claimed3 &&
		k_again == KERN_FAILURE && state4 == HAL_DRIVER_RUNNING &&
		starts4 == starts2 + 1, "the card's driver started through the "
		"HAL (%d): state %d, %d start(s) of %d, %s, the card %s; a second "
		"start %d (%d is refused): state %d, %d start(s)", kr, state3,
		starts3, starts2, answered ? "a new port answered" :
		"NO PORT ANSWERED", claimed3 ? "claimed" : "NOT CLAIMED",
		k_again, KERN_FAILURE, state4, starts4);
	if (answered)
		(void)mach_port_deallocate(mach_task_self(), port);
	(void)mach_port_deallocate(mach_task_self(), hal);
}

/*
 * [15], [16] (#964): edu's driver is refused an update no chora could run --
 * the card's module, which does not declare edu; edu_next's bytes under a name
 * longer than a driver's task name has room for; and edu_next's bytes under a
 * name it has no operations for -- and goes on as it was.  Then, given
 * edu_next, it comes back with the new module without a second start: the HAL
 * ends edu_probe and starts edu_next at its end, with one start more; its
 * control port answers, the HAL names it, edu is claimed, and 3 s on there has
 * been no other start.
 */
#define CT_LONG_NAME	"edu_next_and_a_name_too_long"	/* past 23 */

static void
the_update_arm(void)
{
	struct uros_cap		tok;
	mach_port_t		hal, port = MACH_PORT_NULL;
	char			old[URMACH_TASK_NAME_MAX], new[URMACH_TASK_NAME_MAX];
	char			driver[URMACH_TASK_NAME_MAX], module[64];
	vm_offset_t		bad = 0, next = 0;
	mach_msg_type_number_t	bad_len = 0, next_len = 0;
	natural_t		b = edu_bdf;
	unsigned int		bdf = 0;
	kern_return_t		kr, k_bad, k_long, k_noops, k_good;
	int			i, state0, starts0, state1, starts1, state3;
	int			starts3, answered = 0, held;

	if (!edu_found) {
		not_asked(15, edu_looked ? "no edu device on this board "
			  "(run-x86_64.sh --edu)" : "the arms before [5] ended "
			  "before it looked for edu");
		return;
	}
	if (edu_class != CT_EDU_CLASS) {
		not_asked(15, "edu's class is not the one this task's manifest "
			  "names, 00ff00");
		return;
	}
	hal = the_hal();
	if (!MACH_PORT_VALID(hal)) {
		verdict(15, 0, "the HAL's name is gone");
		return;
	}
	(void)snprintf(old, sizeof old, "edu_probe@%02x:%02x.%x", CT_BUS(b),
		       CT_SLOT(b), CT_FUNC(b));
	(void)snprintf(new, sizeof new, "edu_next@%02x:%02x.%x", CT_BUS(b),
		       CT_SLOT(b), CT_FUNC(b));
	kr = admin_token(CT_EDU_CLASS, CAP_OP_DRIVER_UPDATE, &tok);
	if (kr == KERN_SUCCESS)
		kr = disk_module("net", "chora_probe.so", &bad, &bad_len);
	if (kr == KERN_SUCCESS)
		kr = disk_file(CT_UPDATES "edu_next.so", &next, &next_len);
	if (kr != KERN_SUCCESS) {
		verdict(15, 0, "no token for edu's class, or no card's module or "
			"edu_next from the disk: %d", kr);
		if (bad != 0)
			(void)vm_deallocate(mach_task_self(), bad, bad_len);
		(void)mach_port_deallocate(mach_task_self(), hal);
		return;
	}

	/* edu_probe drives edu: its port answers once it has attached. */
	for (i = 0; i < CT_WAIT_STEPS; i++) {
		kr = ask_chora(old, &port, module, sizeof module, &bdf);
		if (MACH_PORT_VALID(port))
			(void)mach_port_deallocate(mach_task_self(), port);
		port = MACH_PORT_NULL;
		if (kr == KERN_SUCCESS && strcmp(module, "edu_probe") == 0)
			break;
		nap(100);
	}
	if (i == CT_WAIT_STEPS) {
		verdict(15, 0, "\"%s\" did not answer on its control port in "
			"30 s: %d", old, kr);
		(void)vm_deallocate(mach_task_self(), bad, bad_len);
		(void)vm_deallocate(mach_task_self(), next, next_len);
		(void)mach_port_deallocate(mach_task_self(), hal);
		return;
	}
	driver_state(hal, b, &state0, &starts0);

	/* [15]: three updates no chora could run, refused. */
	k_bad = hal_update_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				  (char *)&tok, sizeof tok, "chora_probe",
				  sizeof "chora_probe" - 1, bad, bad_len);
	k_long = hal_update_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				   (char *)&tok, sizeof tok, CT_LONG_NAME,
				   sizeof CT_LONG_NAME - 1, next, next_len);
	k_noops = hal_update_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				    (char *)&tok, sizeof tok, "edu_nexx",
				    sizeof "edu_nexx" - 1, next, next_len);
	(void)vm_deallocate(mach_task_self(), bad, bad_len);
	driver_state(hal, b, &state1, &starts1);
	verdict(15, k_bad == KERN_INVALID_VALUE &&
		k_long == KERN_INVALID_ARGUMENT &&
		k_noops == KERN_INVALID_VALUE && state1 == HAL_DRIVER_RUNNING &&
		starts1 == starts0, "edu's driver refused updates no chora could "
		"run: the card's module %d, a name too long %d, a name with no "
		"operations %d, where %d %d %d refuse; state %d then %d, %d "
		"start(s) then %d", k_bad, k_long, k_noops, KERN_INVALID_VALUE,
		KERN_INVALID_ARGUMENT, KERN_INVALID_VALUE, state0, state1,
		starts0, starts1);

	/* [16]: edu_next, from the disk's updates. */
	k_good = hal_update_driver(hal, CT_BUS(b), CT_SLOT(b), CT_FUNC(b),
				   (char *)&tok, sizeof tok, "edu_next",
				   sizeof "edu_next" - 1, next, next_len);
	(void)vm_deallocate(mach_task_self(), next, next_len);
	for (i = 0; k_good == KERN_SUCCESS && i < CT_WAIT_STEPS && !answered;
	     i++) {
		int	state2, starts2;

		driver_state(hal, b, &state2, &starts2);
		if (state2 == HAL_DRIVER_RUNNING && starts2 > starts1 &&
		    ask_chora(new, &port, module, sizeof module, &bdf) ==
		    KERN_SUCCESS && strcmp(module, "edu_next") == 0 && bdf == b)
			answered = 1;
		else
			nap(100);
		if (MACH_PORT_VALID(port))
			(void)mach_port_deallocate(mach_task_self(), port);
		port = MACH_PORT_NULL;
	}
	nap(CT_SETTLE_MS);
	driver_state(hal, b, &state3, &starts3);
	hal_driver(b, new, driver, sizeof driver);
	held = claimed(b);
	verdict(16, k_good == KERN_SUCCESS && answered &&
		state3 == HAL_DRIVER_RUNNING && starts3 == starts1 + 1 &&
		strcmp(driver, new) == 0 && held, "edu's driver updated through "
		"the HAL to edu_next (%d): %s, 3 s on state %d, %d start(s) of "
		"%d, the HAL names \"%s\", edu %s", k_good, answered ?
		"it answers" : "NO ANSWER", state3, starts3, starts1, driver,
		held ? "claimed" : "NOT CLAIMED");
	(void)mach_port_deallocate(mach_task_self(), hal);
}

int
main(void)
{
	printf("chora_test: started\n");
	run_the_arms();
	the_crash_arm();
	the_kill_arm();
	the_refusal_arm();
	the_stop_arm();
	the_update_arm();
	the_arms_not_reached();
	printf("chora_test: %d of %d arms passed\n", arms_passed, arms_run);
	return 0;
}
