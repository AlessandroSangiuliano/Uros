/*
 * FLIPC v2 — Fast Local IPC Channel
 *
 * Copyright (c) 2026 Alessandro Sangiuliano <alex22_7@hotmail.com>
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
 * libflipc2/flipc2_channel.c
 *
 * Channel setup (slow path): create, accept, destroy.
 *
 * Sharing model:
 *   - channel_create() allocates and initializes the shared region + semaphore
 *   - For intra-task (threads): both producer and consumer call
 *     flipc2_channel_attach() with the same base address
 *   - For inter-task (parent/child): the child inherits the VM mapping
 *     and attaches with the inherited address
 *   - For inter-task (unrelated): the creator sends the base address and
 *     channel size via Mach IPC, the peer uses vm_read/vm_remap
 *   - The semaphore port is stored in the header and usable by any task
 *     that has the mapping
 */

#include <flipc2.h>
#include <mach/mach_interface.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/mach_host.h>
#include <string.h>

static int
is_power_of_2(uint32_t n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

static void
flipc2_init_header(struct flipc2_channel_header *hdr,
                   uint32_t channel_size,
                   uint32_t ring_entries)
{
    uint32_t ring_offset = FLIPC2_HEADER_SIZE;
    uint32_t ring_size   = ring_entries * FLIPC2_DESC_SIZE;
    uint32_t data_offset = ring_offset + ring_size;
    uint32_t data_size   = channel_size - data_offset;

    memset(hdr, 0, channel_size);

    hdr->magic        = FLIPC2_MAGIC;
    hdr->version      = FLIPC2_VERSION;
    hdr->channel_size = channel_size;
    hdr->ring_offset  = ring_offset;
    hdr->ring_entries = ring_entries;
    hdr->ring_mask    = ring_entries - 1;
    hdr->data_offset  = data_offset;
    hdr->data_size    = data_size;
    hdr->desc_size    = FLIPC2_DESC_SIZE;

    hdr->prod_tail     = 0;
    hdr->cons_head     = 0;
    hdr->cons_sleeping = 0;
    hdr->prod_total    = 0;
    hdr->cons_total    = 0;
    hdr->wakeups       = 0;
}

static void
flipc2_init_handle(struct flipc2_channel *ch,
                   struct flipc2_channel_header *hdr)
{
    ch->hdr  = hdr;
    ch->ring = (struct flipc2_desc *)((uint8_t *)hdr + hdr->ring_offset);
    ch->data = (uint8_t *)hdr + hdr->data_offset;
    ch->mem_port = MACH_PORT_NULL;
    ch->cached_cons_head = 0;
    ch->cached_prod_tail = 0;
}

/*
 * flipc2_channel_create — Allocate and initialize a new channel.
 *
 * Allocates a wired shared memory region and a Mach semaphore.
 * Returns the channel handle and the base address (for passing to
 * the peer via flipc2_channel_attach or Mach IPC).
 *
 * The semaphore port is embedded in the channel header, so any task
 * with the mapping can use it for wakeup.
 */
flipc2_return_t
flipc2_channel_create(
    uint32_t            channel_size,
    uint32_t            ring_entries,
    flipc2_channel_t   *channel,
    mach_port_t        *sem_port)
{
    kern_return_t       kr;
    vm_address_t        addr;
    mach_port_t         sem;
    struct flipc2_channel *ch;
    struct flipc2_channel_header *hdr;
    uint32_t            ring_size;
    uint32_t            min_size;

    if (!channel || !sem_port)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    if (channel_size < FLIPC2_CHANNEL_SIZE_MIN ||
        channel_size > FLIPC2_CHANNEL_SIZE_MAX)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    if (!is_power_of_2(ring_entries) ||
        ring_entries < FLIPC2_RING_ENTRIES_MIN ||
        ring_entries > FLIPC2_RING_ENTRIES_MAX)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    ring_size = ring_entries * FLIPC2_DESC_SIZE;
    min_size  = FLIPC2_HEADER_SIZE + ring_size;
    if (channel_size < min_size)
        return FLIPC2_ERR_SIZE_ALIGNMENT;

    /* Allocate shared memory region */
    addr = 0;
    kr = vm_allocate(mach_task_self(), &addr, channel_size, TRUE);
    if (kr != KERN_SUCCESS)
        return FLIPC2_ERR_RESOURCE_SHORTAGE;

    /* Wire the memory so it won't be paged out (best-effort) */
    (void)vm_wire(mach_host_self(), mach_task_self(), addr, channel_size,
                  VM_PROT_READ | VM_PROT_WRITE);

    /* Create semaphore for adaptive wakeup */
    kr = semaphore_create(mach_task_self(), &sem, SYNC_POLICY_FIFO, 0);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(mach_task_self(), addr, channel_size);
        return FLIPC2_ERR_KERNEL;
    }

    /* Initialize header */
    hdr = (struct flipc2_channel_header *)addr;
    flipc2_init_header(hdr, channel_size, ring_entries);
    hdr->wakeup_sem = sem;

    /* Allocate the handle */
    kr = vm_allocate(mach_task_self(), (vm_address_t *)&ch,
                     sizeof(struct flipc2_channel), TRUE);
    if (kr != KERN_SUCCESS) {
        semaphore_destroy(mach_task_self(), sem);
        vm_deallocate(mach_task_self(), addr, channel_size);
        return FLIPC2_ERR_RESOURCE_SHORTAGE;
    }

    flipc2_init_handle(ch, hdr);

    *channel  = ch;
    *sem_port = sem;

    return FLIPC2_SUCCESS;
}

/*
 * flipc2_channel_attach — Attach to an existing channel via its base address.
 *
 * Used by:
 *   - Intra-task: another thread in the same task
 *   - Inter-task: child task that inherited the VM mapping
 *   - Inter-task: peer that received the address + mapped via vm_remap
 */
flipc2_return_t
flipc2_channel_attach(
    vm_address_t        base_addr,
    flipc2_channel_t   *channel)
{
    kern_return_t       kr;
    struct flipc2_channel *ch;
    struct flipc2_channel_header *hdr;

    if (!base_addr || !channel)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    hdr = (struct flipc2_channel_header *)base_addr;

    /* Validate the header */
    if (hdr->magic != FLIPC2_MAGIC || hdr->version != FLIPC2_VERSION)
        return FLIPC2_ERR_NOT_CONNECTED;

    /* Allocate the handle */
    kr = vm_allocate(mach_task_self(), (vm_address_t *)&ch,
                     sizeof(struct flipc2_channel), TRUE);
    if (kr != KERN_SUCCESS)
        return FLIPC2_ERR_RESOURCE_SHORTAGE;

    flipc2_init_handle(ch, hdr);

    *channel = ch;

    return FLIPC2_SUCCESS;
}

/*
 * flipc2_channel_destroy — Tear down channel and release resources.
 *
 * Only the creator should call this (it destroys the semaphore and
 * deallocates the shared memory).  Peers should call
 * flipc2_channel_detach() instead.
 */
flipc2_return_t
flipc2_channel_destroy(
    flipc2_channel_t    channel)
{
    struct flipc2_channel_header *hdr;
    uint32_t size;

    if (!channel || !channel->hdr)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    hdr  = channel->hdr;
    size = hdr->channel_size;

    /* Destroy the semaphore */
    if (hdr->wakeup_sem != MACH_PORT_NULL)
        semaphore_destroy(mach_task_self(), hdr->wakeup_sem);

    /* Unmap shared memory */
    vm_deallocate(mach_task_self(), (vm_address_t)hdr, size);

    /* Free the handle */
    vm_deallocate(mach_task_self(), (vm_address_t)channel,
                  sizeof(struct flipc2_channel));

    return FLIPC2_SUCCESS;
}

/*
 * flipc2_channel_detach — Detach from a channel without destroying it.
 *
 * Peers (non-creators) call this to free their local handle.
 * For inter-task peers the shared memory should be unmapped separately;
 * for intra-task peers the creator owns the mapping.
 */
flipc2_return_t
flipc2_channel_detach(
    flipc2_channel_t    channel)
{
    if (!channel)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Free the handle only — do NOT unmap shared memory.
     * For intra-task: creator and peer share the same mapping.
     * For inter-task: the child owns its inherited copy, which
     * is reclaimed when the child task terminates. */
    vm_deallocate(mach_task_self(), (vm_address_t)channel,
                  sizeof(struct flipc2_channel));

    return FLIPC2_SUCCESS;
}

/*
 * flipc2_channel_map_pages — Map producer pages into consumer (stub).
 *
 * TODO: implement when data plane zero-copy is needed (phase 6).
 */
flipc2_return_t
flipc2_channel_map_pages(
    flipc2_channel_t    channel,
    vm_offset_t         source_addr,
    vm_size_t           size,
    uint64_t           *region_id)
{
    (void)channel;
    (void)source_addr;
    (void)size;
    (void)region_id;
    return FLIPC2_ERR_INVALID_ARGUMENT;
}

/*
 * flipc2_channel_share — Map a channel into another task via vm_remap.
 *
 * Creates a true shared memory mapping (not COW) of the channel's
 * entire region in the target task.  After this call, both the creator
 * and the target task read/write the same physical pages.
 *
 * The target task can then call flipc2_channel_attach() with the
 * returned address to get a local handle.
 */
flipc2_return_t
flipc2_channel_share(
    flipc2_channel_t    channel,
    mach_port_t         target_task,
    vm_address_t       *out_addr)
{
    kern_return_t   kr;
    vm_address_t    target_addr;
    vm_prot_t       cur_prot, max_prot;

    if (!channel || !channel->hdr || !out_addr)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    if (target_task == MACH_PORT_NULL)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    target_addr = 0;
    kr = vm_remap(target_task,
                  &target_addr,
                  channel->hdr->channel_size,
                  0,             /* mask */
                  TRUE,          /* anywhere */
                  mach_task_self(),
                  (vm_address_t)channel->hdr,
                  FALSE,         /* copy=FALSE: true shared memory */
                  &cur_prot,
                  &max_prot,
                  VM_INHERIT_SHARE);
    if (kr != KERN_SUCCESS)
        return FLIPC2_ERR_MAP_FAILED;

    *out_addr = target_addr;
    return FLIPC2_SUCCESS;
}

/*
 * flipc2_semaphore_share — Insert a semaphore port into another task.
 *
 * Gives the target task a send right to the semaphore under the
 * specified port name.  Used during inter-task channel setup so
 * the child task can call semaphore_signal/semaphore_wait.
 */
flipc2_return_t
flipc2_semaphore_share(
    mach_port_t         sem,
    mach_port_t         target_task,
    mach_port_t         target_name)
{
    kern_return_t   kr;

    if (sem == MACH_PORT_NULL || target_task == MACH_PORT_NULL)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    kr = mach_port_insert_right(target_task,
                                target_name,
                                sem,
                                MACH_MSG_TYPE_COPY_SEND);
    if (kr != KERN_SUCCESS)
        return FLIPC2_ERR_KERNEL;

    return FLIPC2_SUCCESS;
}
