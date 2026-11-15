/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * blk_write_label_test LABEL -- the block server, started with -W LABEL,
 * hands out a write right for the partition whose ext2 volume is labelled
 * LABEL and for no other (#915).
 *
 * It waits for "label:LABEL", which the block server publishes after every
 * partition's own name, so the table is all there when it comes.  Then it
 * asks each partition it can find for a write open and a read open, and reads
 * the label itself through the read: the server's answer to the write must be
 * what the label says -- the open taken where the label is LABEL, D_READ_ONLY
 * everywhere else.  The label is read here and not taken from the server, so
 * a server that misread a label cannot agree with itself into a PASS.
 *
 * The opens write nothing, so the test runs on lapsus as it does in QEMU, and
 * there it is the proof that the disks of Windows and of the Linux that runs
 * there cannot be written.
 *
 *   [1] every write open answered as the partition's label says
 *   [2] every read open taken, so the refusals are of writes alone
 *   [3] exactly one partition took a write, the one "label:LABEL" names
 */

#include <stdio.h>
#include <string.h>
#include <mach.h>
#include <mach/bootstrap.h>
#include <mach/cap_types.h>
#include <sa_mach.h>			/* printf_init */
#include <mach_init.h>			/* name_server_port */
#include <device/device.h>
#include <device/device_types.h>
#include <libcap.h>			/* cap_request */
#include <servers/netname.h>
#include <servers/netname_defs.h>	/* netname_notify_msg_t */

#define LABEL_MAX	16u		/* s_volume_name's bytes */
#define SB_OFFSET	1024u		/* the superblock, bytes in */
#define SB_OFF_MAGIC	56u
#define SB_OFF_LABEL	120u
#define SB_MAGIC	0xEF53u

/* The names a partition can have: the driver's, disk and slot. */
static const struct {
	const char	*driver;
	int		disks;
} drivers[] = {
	{ "ahci", 8 },
	{ "virtio_blk", 4 },
};

/* Wait for NAME to be checked in, and return its port. */
static kern_return_t
wait_for_name(const char *name, mach_port_t *port)
{
	mach_port_t		recv;
	netname_notify_msg_t	nmsg;
	kern_return_t		kr;

	*port = MACH_PORT_NULL;
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&recv);
	if (kr != KERN_SUCCESS)
		return kr;
	kr = netname_notify(name_server_port, (char *)name, recv);
	if (kr == KERN_SUCCESS)
		kr = mach_msg(&nmsg.head, MACH_RCV_MSG, 0, sizeof(nmsg), recv,
			      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr == KERN_SUCCESS)
		*port = nmsg.service.name;
	(void)mach_port_destroy(mach_task_self(), recv);
	return kr;
}

/*
 * An open of PART for MODE, with a capability asked for just what the mode
 * needs: the open's answer, or *no_cap set when cap_server gave none.
 */
static kern_return_t
open_for(mach_port_t part, const char *name, dev_mode_t mode,
	 mach_port_t *handle, int *no_cap)
{
	struct uros_cap		tok;
	security_token_t	null_sec = { { 0, 0 } };
	char			blob[CAP_TOKEN_MAX];
	uint32_t		ops = 0;

	*handle = MACH_PORT_NULL;
	*no_cap = 0;
	if (mode & D_READ)
		ops |= CAP_OP_BLK_READ;
	if (mode & D_WRITE)
		ops |= CAP_OP_BLK_WRITE;
	if (cap_request(RESOURCE_BLK_DEVICE, cap_name_hash(name), ops, 0, &tok)
	    != KERN_SUCCESS) {
		*no_cap = 1;
		return KERN_FAILURE;
	}
	memcpy(blob, &tok, sizeof(tok));
	return device_open_cap(part, MACH_PORT_NULL, mode, null_sec,
			       (char *)name, blob,
			       (mach_msg_type_number_t)sizeof(tok), handle);
}

/* The label the partition's superblock holds, read through HANDLE. */
static kern_return_t
read_label(mach_port_t handle, int *is_ext2, char label[LABEL_MAX + 1])
{
	io_buf_ptr_t		data = 0;
	mach_msg_type_number_t	cnt = 0;
	const unsigned char	*b;
	kern_return_t		kr;
	unsigned int		i;

	*is_ext2 = 0;
	label[0] = '\0';
	kr = device_read(handle, D_READ, 0, 2048, &data, &cnt);
	if (kr != KERN_SUCCESS)
		return kr;
	if (cnt < SB_OFFSET + SB_OFF_LABEL + LABEL_MAX) {
		(void)vm_deallocate(mach_task_self(), (vm_address_t)data, cnt);
		return KERN_FAILURE;
	}
	b = (const unsigned char *)data;
	if ((b[SB_OFFSET + SB_OFF_MAGIC] | (b[SB_OFFSET + SB_OFF_MAGIC + 1] << 8))
	    == SB_MAGIC) {
		*is_ext2 = 1;
		for (i = 0; i < LABEL_MAX; i++)
			label[i] = (char)b[SB_OFFSET + SB_OFF_LABEL + i];
		label[LABEL_MAX] = '\0';
	}
	(void)vm_deallocate(mach_task_self(), (vm_address_t)data, cnt);
	return KERN_SUCCESS;
}

int
main(int argc, char **argv)
{
	mach_port_t	host_port, device_port, wired, paged, security;
	mach_port_t	labelled = MACH_PORT_NULL;
	const char	*want;
	kern_return_t	kr;
	unsigned int	d, s, k;
	int		found = 0, as_label = 0, read_ok = 0, took = 0;
	int		took_labelled = 0;
	char		lname[sizeof("label:") + LABEL_MAX];

	kr = bootstrap_ports(bootstrap_port, &host_port, &device_port,
			     &wired, &paged, &security);
	if (kr != KERN_SUCCESS)
		_exit(1);	/* no console yet -- nothing to say it with */
	printf_init(device_port);

	want = argc > 1 ? argv[1] : NULL;
	printf("blk_write_label_test: started (#915)\n");
	if (want == NULL || want[0] == '\0' || strlen(want) > LABEL_MAX) {
		printf("blk_write_label_test: NOT ASKED — no label of at most "
		       "%u bytes on the command line (#915)\n", LABEL_MAX);
		return 0;
	}

	snprintf(lname, sizeof(lname), "label:%s", want);
	kr = wait_for_name(lname, &labelled);
	if (kr != KERN_SUCCESS) {
		printf("blk_write_label_test: NOT ASKED — waiting for \"%s\" "
		       "failed (kr=%d) (#915)\n", lname, kr);
		return 0;
	}

	for (k = 0; k < sizeof(drivers) / sizeof(drivers[0]); k++)
	for (d = 0; d < (unsigned int)drivers[k].disks; d++)
	for (s = 0; s < 4; s++) {
		char		name[32];
		char		label[LABEL_MAX + 1];
		mach_port_t	part = MACH_PORT_NULL, wh, rh;
		kern_return_t	wkr, rkr, lkr = KERN_FAILURE;
		int		wnc, rnc, is_ext2 = 0, ok;

		snprintf(name, sizeof(name), "%s%u%c", drivers[k].driver, d,
			 (char)('a' + s));
		if (netname_look_up(name_server_port, "", name, &part)
		    != KERN_SUCCESS || part == MACH_PORT_NULL)
			continue;
		found++;

		wkr = open_for(part, name, D_WRITE, &wh, &wnc);
		if (wkr == KERN_SUCCESS) {
			took++;
			if (part == labelled)
				took_labelled = 1;
			(void)mach_port_deallocate(mach_task_self(), wh);
		}
		rkr = open_for(part, name, D_READ, &rh, &rnc);
		if (rkr == KERN_SUCCESS) {
			read_ok++;
			lkr = read_label(rh, &is_ext2, label);
			(void)mach_port_deallocate(mach_task_self(), rh);
		}

		ok = !wnc && lkr == KERN_SUCCESS
		     && (is_ext2 && strcmp(label, want) == 0
			 ? wkr == KERN_SUCCESS : wkr == D_READ_ONLY);
		if (ok)
			as_label++;
		printf("blk_write_label_test: %s, %s%s%s: write open %s "
		       "(kr=%d), read open %s (kr=%d)%s — %s (#915)\n", name,
		       lkr != KERN_SUCCESS ? "label not read"
		       : is_ext2 ? "ext2 labelled \"" : "not ext2",
		       lkr == KERN_SUCCESS && is_ext2 ? label : "",
		       lkr == KERN_SUCCESS && is_ext2 ? "\"" : "",
		       wnc ? "not asked, no capability"
		       : wkr == KERN_SUCCESS ? "taken" : "refused", wkr,
		       rnc ? "not asked, no capability"
		       : rkr == KERN_SUCCESS ? "taken" : "refused", rkr,
		       part == labelled ? ", the partition the label names"
		       : "",
		       ok ? "as its label says" : "WRONG");
		(void)mach_port_deallocate(mach_task_self(), part);
	}

	if (found == 0) {
		printf("blk_write_label_test: NOT ASKED — \"%s\" is published "
		       "and no partition name was found (#915)\n", lname);
		return 0;
	}
	printf("blk_write_label_test: [1] %d of %d partitions answered a "
	       "write open as their label says — %s (#915)\n", as_label, found,
	       as_label == found ? "PASS" : "WRONG");
	printf("blk_write_label_test: [2] %d of %d read opens taken — %s "
	       "(#915)\n", read_ok, found, read_ok == found ? "PASS" : "WRONG");
	printf("blk_write_label_test: [3] %d partition(s) took a write, %s "
	       "the one \"%s\" names — %s (#915)\n", took,
	       took_labelled ? "among them" : "and not", lname,
	       took == 1 && took_labelled ? "PASS" : "WRONG");
	printf("blk_write_label_test: %d of 3 arms passed\n",
	       (as_label == found) + (read_ok == found)
	       + (took == 1 && took_labelled));
	return 0;
}
