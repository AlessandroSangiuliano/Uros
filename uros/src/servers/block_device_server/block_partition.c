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
 * block_partition.c — MBR parsing and partition registration
 *
 * Reads sector 0 from each disk via the module's read_sectors(),
 * parses MBR partition entries, and registers each Linux (0x83)
 * partition with the name server using protected payloads.
 */

#include <mach.h>
#include <mach/mach_port.h>
#include <servers/netname.h>
#include <stdio.h>
#include <string.h>
#include "block_server.h"

/* ================================================================
 * The payload table (#520) — see the comment in block_server.h
 * ================================================================ */

/*
 * Slot zero is never handed out, so that a payload of zero -- which is what a
 * port with no payload set delivers -- can mean "no object" and not "the first
 * one".  The array is indexed by payload - 1.
 */
static void *blk_payload_objects[BLK_MAX_PAYLOAD_OBJECTS];

natural_t
blk_payload_register(void *object)
{
	unsigned i;

	if (object == NULL)
		return 0;

	for (i = 0; i < BLK_MAX_PAYLOAD_OBJECTS; i++)
		if (blk_payload_objects[i] == NULL) {
			blk_payload_objects[i] = object;
			return (natural_t)(i + 1);
		}

	/*
	 * ⚠️ Reported and refused, rather than grown or wrapped.  A payload
	 * table that wrapped would hand two ports the same name and the second
	 * object would answer for the first -- a wrong disk, not an error.
	 */
	printf("blk: payload table full at %u objects — port not published\n",
	       (unsigned)BLK_MAX_PAYLOAD_OBJECTS);
	return 0;
}

void *
blk_object_for(natural_t payload)
{
	if (payload == 0 || payload > BLK_MAX_PAYLOAD_OBJECTS)
		return NULL;

	return blk_payload_objects[payload - 1];
}

void
blk_payload_release(natural_t payload)
{
	if (payload == 0 || payload > BLK_MAX_PAYLOAD_OBJECTS)
		return;

	blk_payload_objects[payload - 1] = NULL;
}

/* ================================================================
 * MBR parsing
 *
 * Reads sector 0, parses four primary MBR entries, populates
 * the global partitions[] array for Linux (0x83) entries.
 * If no valid MBR is found, exposes the whole disk.
 * ================================================================ */

int
blk_read_mbr(struct blk_controller *ctrl, int disk_idx,
	     const char *prefix, int stable_disk_idx)
{
	vm_offset_t buf;
	unsigned int buf_size;
	struct mbr_block *mbr;
	int i;

	/* Read sector 0 via module ops */
	if (ctrl->ops->read_sectors(ctrl->priv, disk_idx, 0, 1,
				    &buf, &buf_size) < 0) {
		printf("blk: %s disk %d: failed to read MBR\n",
		       prefix, disk_idx);
		return -1;
	}

	mbr = (struct mbr_block *)buf;

	if (mbr->signature != MBR_MAGIC) {
		/* No MBR — expose whole disk as single partition */
		printf("blk: %s disk %d: no MBR (sig=0x%04X), "
		       "exposing whole disk\n",
		       prefix, disk_idx, mbr->signature);
		if (n_partitions >= MAX_PARTITIONS) {
			printf("blk: the partition table is full at %d — %s "
			       "disk %d is NOT being published\n",
			       MAX_PARTITIONS, prefix, disk_idx);
			vm_deallocate(mach_task_self(), buf, buf_size);
			return 0;
		}
		partitions[n_partitions].ctrl       = ctrl;
		partitions[n_partitions].disk_index = disk_idx;
		partitions[n_partitions].start_lba  = 0;
		partitions[n_partitions].num_sectors =
			ctrl->disks[disk_idx].total_sectors;
		snprintf(partitions[n_partitions].name,
			 sizeof(partitions[n_partitions].name),
			 "%s%d%c", prefix, disk_idx, 'a');
		snprintf(partitions[n_partitions].stable_name,
			 sizeof(partitions[n_partitions].stable_name),
			 "disk%d%c", stable_disk_idx, 'a');
		n_partitions++;
		vm_deallocate(mach_task_self(), buf, buf_size);
		return 0;
	}

	printf("blk: %s disk %d MBR:\n", prefix, disk_idx);
	for (i = 0; i < MBR_NUMPART; i++) {
		struct mbr_part_entry *p = &mbr->parts[i];

		if (p->numsect == 0 || p->systid == 0)
			continue;

		printf("  part %d: type=0x%02X  start=%u  size=%u "
		       "(%u MB)\n", i, p->systid,
		       p->relsect, p->numsect, p->numsect / 2048);

		/*
		 * Issue #400: name the one unsupported layout we are certain
		 * to meet on any modern machine.  Without this the entry just
		 * falls through the type filter below and the disk goes by in
		 * silence, indistinguishable from a blank one.
		 */
		if (p->systid == MBR_TYPE_GPT_PROTECTIVE) {
			printf("blk: %s disk %d is GPT-partitioned "
			       "(protective MBR) — GPT is not supported, "
			       "ignoring disk\n", prefix, disk_idx);
			continue;
		}

		/*
		 * Issue #184: also accept Linux swap (0x82) — default_pager
		 * opens its backing store as a raw partition via BDS.
		 */
		if (p->systid != MBR_TYPE_LINUX &&
		    p->systid != MBR_TYPE_LINUX_SWAP)
			continue;
		if (n_partitions >= MAX_PARTITIONS) {
			/*
			 * 🔴 Said, not skipped.  The limit is real and this
			 * is where it bites; a partition that quietly does
			 * not appear is indistinguishable from a disk that
			 * does not have one.
			 */
			printf("blk: the partition table is full at %d — "
			       "%s disk %d partition %d and any after it are "
			       "NOT being published\n",
			       MAX_PARTITIONS, prefix, disk_idx, i + 1);
			break;
		}

		partitions[n_partitions].ctrl       = ctrl;
		partitions[n_partitions].disk_index = disk_idx;
		partitions[n_partitions].start_lba  = p->relsect;
		partitions[n_partitions].num_sectors = p->numsect;
		snprintf(partitions[n_partitions].name,
			 sizeof(partitions[n_partitions].name),
			 "%s%d%c", prefix, disk_idx, 'a' + i);
		snprintf(partitions[n_partitions].stable_name,
			 sizeof(partitions[n_partitions].stable_name),
			 "disk%d%c", stable_disk_idx, 'a' + i);
		n_partitions++;
	}

	vm_deallocate(mach_task_self(), buf, buf_size);
	return 0;
}

/* ================================================================
 * Partition registration
 *
 * For each partition in partitions[]:
 *   - allocate a receive port
 *   - set protected payload = pointer to the partition struct
 *   - add to the global port set
 *   - register with the name server
 * ================================================================ */

void
blk_register_partitions(void)
{
	kern_return_t kr;
	mach_port_t sig = MACH_PORT_NULL;
	int i;

	/*
	 * #744: every name is signed with a port only this task holds --
	 * mach_task_self() gave the name server a send right to this task's
	 * port.  Without one nothing here can be registered, and saying so once
	 * is better than a refusal per partition.
	 */
	kr = urmach_netname_signature(&sig);
	if (kr != KERN_SUCCESS) {
		printf("blk: no name signature (kr=%d), no partition is "
		       "registered\n", kr);
		return;
	}

	for (i = 0; i < n_partitions; i++) {
		struct blk_partition *part = &partitions[i];

		part->magic = BLK_MAGIC_PART;

		kr = mach_port_allocate(mach_task_self(),
			MACH_PORT_RIGHT_RECEIVE, &part->recv_port);
		if (kr != KERN_SUCCESS) {
			printf("blk: port alloc failed for %s\n", part->name);
			continue;
		}
		kr = mach_port_insert_right(mach_task_self(),
			part->recv_port, part->recv_port,
			MACH_MSG_TYPE_MAKE_SEND);
		if (kr != KERN_SUCCESS) {
			printf("blk: port right failed for %s\n", part->name);
			continue;
		}

		part->payload = blk_payload_register(part);
		if (part->payload == 0)
			continue;

		kr = mach_port_set_protected_payload(mach_task_self(),
			part->recv_port, part->payload);
		if (kr != KERN_SUCCESS) {
			printf("blk: set_protected_payload failed for %s\n",
			       part->name);
			blk_payload_release(part->payload);
			part->payload = 0;
			continue;
		}

		mach_port_move_member(mach_task_self(),
			part->recv_port, port_set);

		kr = netname_check_in(name_server_port, part->name,
				      sig, part->recv_port);
		if (kr != KERN_SUCCESS)
			printf("blk: netname_check_in(%s) failed (kr=%d)\n",
			       part->name, kr);
		else
			printf("blk: registered '%s' (LBA %u+%u)\n",
			       part->name,
			       part->start_lba, part->num_sectors);

		/*
		 * Driver-agnostic alias (Issue #184): publish the same
		 * recv port under a stable "disk<N><letter>" name so
		 * default_pager / bootstrap can address partitions without
		 * caring whether AHCI or virtio-blk is the backing module.
		 */
		if (part->stable_name[0] != '\0') {
			kr = netname_check_in(name_server_port,
					      part->stable_name,
					      sig,
					      part->recv_port);
			if (kr != KERN_SUCCESS)
				printf("blk: netname_check_in(%s) "
				       "failed (kr=%d)\n",
				       part->stable_name, kr);
			else
				printf("blk: registered alias '%s' -> '%s'\n",
				       part->stable_name, part->name);
		}
	}

	/*
	 * #915: and a partition by its label, "label:UROSLOG" -- a name that
	 * does not move when the disks do, as "ahci2a" does.  After every
	 * partition's own name, so a client that waits for a label knows the
	 * table is all published when it comes.  Only a label no other
	 * partition carries: two would make the name a choice.  The labels
	 * are read with -W alone (blk_write_label_read()), so without it
	 * there is none to publish.
	 */
	for (i = 0; i < n_partitions; i++) {
		struct blk_partition *part = &partitions[i];
		char lname[sizeof("label:") + sizeof(part->label)];
		int j, same = 0;

		if (!part->is_ext2 || part->label[0] == '\0'
		    || part->payload == 0)
			continue;
		for (j = 0; j < n_partitions; j++)
			if (partitions[j].is_ext2
			    && strcmp(partitions[j].label, part->label) == 0)
				same++;
		if (same != 1) {
			printf("blk: label \"%s\" is on %d partitions, so it "
			       "names none of them (#915)\n", part->label,
			       same);
			continue;
		}
		snprintf(lname, sizeof(lname), "label:%s", part->label);
		kr = netname_check_in(name_server_port, lname, sig,
				      part->recv_port);
		if (kr != KERN_SUCCESS)
			printf("blk: netname_check_in(%s) failed (kr=%d)\n",
			       lname, kr);
		else
			printf("blk: registered '%s' -> '%s' (#915)\n", lname,
			       part->name);
	}
}
