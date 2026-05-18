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
#include "block_server.h"

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
		 * Issue #184: also accept Linux swap (0x82) — default_pager
		 * opens its backing store as a raw partition via BDS.
		 */
		if (p->systid != MBR_TYPE_LINUX &&
		    p->systid != MBR_TYPE_LINUX_SWAP)
			continue;
		if (n_partitions >= MAX_PARTITIONS)
			break;

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
	int i;

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

		kr = mach_port_set_protected_payload(mach_task_self(),
			part->recv_port, (unsigned long)part);
		if (kr != KERN_SUCCESS) {
			printf("blk: set_protected_payload failed for %s\n",
			       part->name);
			continue;
		}

		mach_port_move_member(mach_task_self(),
			part->recv_port, port_set);

		kr = netname_check_in(name_server_port, part->name,
				      mach_task_self(), part->recv_port);
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
					      mach_task_self(),
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
}
