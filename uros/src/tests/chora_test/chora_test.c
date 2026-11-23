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
 *       a module of the bundle, started at the HAL's scan.
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
#include "exec_types.h"			/* EXEC_OK */

/*
 * Not static: hal_server.h declares it extern, for the HAL's own use, and a
 * static one here would contradict that declaration.
 */
mach_port_t	master_device;

#define CT_NET_CLASS	0x020000u
#define CT_EDU_ID	0x11E81234u	/* QEMU's edu: device 11e8h, vendor 1234h */
#define CT_HAL_TRIES	200
#define CT_WAIT_STEPS	300		/* of 100 ms: 30 s */
#define CT_ROOT_WAIT_MS	120000		/* "/" has 120 s, as mount_test gives it */
#define CT_DISK_POOLS	"/mach_servers/modules/"	/* the HAL's, #964 */

static int	arms_run;
static int	arms_passed;

static void
verdict(int arm, int ok, const char *fmt, ...)
{
	char	text[300];
	va_list	ap;

	va_start(ap, fmt);
	(void)vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	arms_run++;
	if (ok)
		arms_passed++;
	printf("chora_test: [%d] %s%s\n", arm, ok ? "" : "WRONG -- ", text);
}

static void
not_asked(int arm, const char *why)
{
	printf("chora_test: [%d] NOT ASKED -- %s\n", arm, why);
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

/* #964: a module of the disk's pools, CT_DISK_POOLS<pool>/<file>, whole. */
static kern_return_t
disk_module(const char *pool, const char *file, vm_offset_t *data,
	    mach_msg_type_number_t *len)
{
	char		path[VFS_PATH_MAX];
	vfs_stat_t	st;
	vfs_fd_t	fd;
	vm_offset_t	buf = 0;
	uint64_t	done = 0;
	ssize_t		r;

	(void)snprintf(path, sizeof path, "%s%s/%s", CT_DISK_POOLS, pool, file);
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

/* The four arms, in order; each that cannot go on says so and ends them. */
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
	if (!find_the_edu(&edu))
		return;
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

int
main(void)
{
	printf("chora_test: started\n");
	run_the_arms();
	printf("chora_test: %d of %d arms passed\n", arms_passed, arms_run);
	return 0;
}
