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
                   uint32_t ring_entries,
                   uint32_t flags)
{
    uint32_t ring_offset;
    uint32_t ring_size = ring_entries * FLIPC2_DESC_SIZE;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t layout;

    memset(hdr, 0, channel_size);

    if (flags & FLIPC2_CREATE_ISOLATED) {
        layout      = FLIPC2_LAYOUT_ISOLATED;
        ring_offset = FLIPC2_ISO_RING_OFFSET;
    } else {
        layout      = FLIPC2_LAYOUT_FLAT;
        ring_offset = FLIPC2_HEADER_SIZE;
    }
    data_offset = ring_offset + ring_size;
    data_size   = channel_size - data_offset;

    hdr->magic        = FLIPC2_MAGIC;
    hdr->version      = FLIPC2_VERSION;
    hdr->channel_size = channel_size;
    hdr->ring_offset  = ring_offset;
    hdr->ring_entries = ring_entries;
    hdr->ring_mask    = ring_entries - 1;
    hdr->data_offset  = data_offset;
    hdr->data_size    = data_size;
    hdr->desc_size    = FLIPC2_DESC_SIZE;
    hdr->layout_flags = layout;

    if (layout == FLIPC2_LAYOUT_ISOLATED) {
        /*
         * Volatile fields live on separate pages.
         * Producer page at offset 4096, consumer page at 8192.
         */
        uint8_t *base = (uint8_t *)hdr;
        volatile uint32_t *pp = (volatile uint32_t *)(base + FLIPC2_ISO_PROD_OFFSET);
        volatile uint32_t *cp = (volatile uint32_t *)(base + FLIPC2_ISO_CONS_OFFSET);
        pp[0] = 0;  /* prod_tail */
        pp[1] = 0;  /* prod_sleeping */
        cp[0] = 0;  /* cons_head */
        cp[1] = 0;  /* cons_sleeping */
    } else {
        hdr->prod_tail     = 0;
        hdr->prod_sleeping = 0;
        hdr->cons_head     = 0;
        hdr->cons_sleeping = 0;
        hdr->prod_total    = 0;
        hdr->cons_total    = 0;
        hdr->wakeups       = 0;
    }
}

static void
flipc2_init_handle(struct flipc2_channel *ch,
                   struct flipc2_channel_header *hdr)
{
    uint8_t *base = (uint8_t *)hdr;

    ch->hdr  = hdr;
    ch->ring = (struct flipc2_desc *)(base + hdr->ring_offset);
    ch->data = base + hdr->data_offset;
    ch->mem_port = MACH_PORT_NULL;

    /* Cache constants from header */
    ch->ring_entries = hdr->ring_entries;
    ch->ring_mask    = hdr->ring_mask;

    /* Set up direct pointers based on layout */
    if (hdr->layout_flags & FLIPC2_LAYOUT_ISOLATED) {
        uint8_t *pp = base + FLIPC2_ISO_PROD_OFFSET;
        uint8_t *cp = base + FLIPC2_ISO_CONS_OFFSET;
        ch->prod_tail     = (volatile uint32_t *)(pp + 0);
        ch->prod_sleeping = (volatile uint32_t *)(pp + 4);
        ch->prod_total    = (volatile uint64_t *)(pp + 8);
        ch->wakeups       = (volatile uint64_t *)(pp + 16);
        ch->cons_head     = (volatile uint32_t *)(cp + 0);
        ch->cons_sleeping = (volatile uint32_t *)(cp + 4);
        ch->cons_total    = (volatile uint64_t *)(cp + 8);
    } else {
        ch->prod_tail     = &hdr->prod_tail;
        ch->prod_sleeping = &hdr->prod_sleeping;
        ch->prod_total    = &hdr->prod_total;
        ch->wakeups       = &hdr->wakeups;
        ch->cons_head     = &hdr->cons_head;
        ch->cons_sleeping = &hdr->cons_sleeping;
        ch->cons_total    = &hdr->cons_total;
    }

    ch->cached_cons_head = 0;
    ch->cached_prod_tail = 0;
    ch->mapped_size = 0;
    ch->spurious_wakeups = 0;
    ch->sem_port = hdr->wakeup_sem;
    ch->sem_port_prod = hdr->wakeup_sem_prod;
    ch->bufgroup = (void *)0;
}

/*
 * flipc2_channel_create_ex — Allocate and initialize a new channel with flags.
 *
 * When FLIPC2_CREATE_ISOLATED is set, the channel header is split across
 * page-aligned sections so that vm_protect can enforce per-role protections.
 */
flipc2_return_t
flipc2_channel_create_ex(
    uint32_t            channel_size,
    uint32_t            ring_entries,
    uint32_t            flags,
    flipc2_channel_t   *channel,
    mach_port_t        *sem_port)
{
    kern_return_t       kr;
    vm_address_t        addr;
    mach_port_t         sem;
    struct flipc2_channel *ch;
    struct flipc2_channel_header *hdr;
    uint32_t            ring_size;
    uint32_t            ring_offset;
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

    if (flags & FLIPC2_CREATE_ISOLATED) {
        ring_offset = FLIPC2_ISO_RING_OFFSET;
        if (channel_size < FLIPC2_CHANNEL_SIZE_MIN_ISOLATED)
            return FLIPC2_ERR_SIZE_ALIGNMENT;
    } else {
        ring_offset = FLIPC2_HEADER_SIZE;
    }

    min_size = ring_offset + ring_size;
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

    /* Create semaphore for adaptive wakeup (consumer) */
    kr = semaphore_create(mach_task_self(), &sem, SYNC_POLICY_FIFO, 0);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(mach_task_self(), addr, channel_size);
        return FLIPC2_ERR_KERNEL;
    }

    /* Create semaphore for producer backpressure (ring full) */
    {
        mach_port_t sem_prod;
        kr = semaphore_create(mach_task_self(), &sem_prod, SYNC_POLICY_FIFO, 0);
        if (kr != KERN_SUCCESS) {
            semaphore_destroy(mach_task_self(), sem);
            vm_deallocate(mach_task_self(), addr, channel_size);
            return FLIPC2_ERR_KERNEL;
        }

        /* Initialize header */
        hdr = (struct flipc2_channel_header *)addr;
        flipc2_init_header(hdr, channel_size, ring_entries, flags);
        hdr->wakeup_sem = sem;
        hdr->wakeup_sem_prod = sem_prod;
    }

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
 * flipc2_channel_create — Allocate and initialize a new channel (flat layout).
 *
 * Convenience wrapper around flipc2_channel_create_ex with flags=0.
 */
flipc2_return_t
flipc2_channel_create(
    uint32_t            channel_size,
    uint32_t            ring_entries,
    flipc2_channel_t   *channel,
    mach_port_t        *sem_port)
{
    return flipc2_channel_create_ex(channel_size, ring_entries, 0,
                                    channel, sem_port);
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

    /* Validate header field consistency */
    if (!is_power_of_2(hdr->ring_entries) ||
        hdr->ring_entries < FLIPC2_RING_ENTRIES_MIN ||
        hdr->ring_entries > FLIPC2_RING_ENTRIES_MAX)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    if (hdr->ring_mask != hdr->ring_entries - 1)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Validate offsets based on layout */
    if (hdr->layout_flags & FLIPC2_LAYOUT_ISOLATED) {
        if (hdr->ring_offset != FLIPC2_ISO_RING_OFFSET)
            return FLIPC2_ERR_INVALID_ARGUMENT;
        if (hdr->data_offset != FLIPC2_ISO_RING_OFFSET +
            hdr->ring_entries * FLIPC2_DESC_SIZE)
            return FLIPC2_ERR_INVALID_ARGUMENT;
        if (hdr->channel_size < FLIPC2_CHANNEL_SIZE_MIN_ISOLATED)
            return FLIPC2_ERR_INVALID_ARGUMENT;
    } else {
        if (hdr->ring_offset != FLIPC2_HEADER_SIZE)
            return FLIPC2_ERR_INVALID_ARGUMENT;
        if (hdr->data_offset != FLIPC2_HEADER_SIZE +
            hdr->ring_entries * FLIPC2_DESC_SIZE)
            return FLIPC2_ERR_INVALID_ARGUMENT;
    }

    if (hdr->channel_size < hdr->data_offset)
        return FLIPC2_ERR_INVALID_ARGUMENT;

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

    /* Invalidate magic so stale handles detect a dead channel */
    hdr->magic = 0;
    FLIPC2_WRITE_FENCE();

    /* Destroy the semaphores */
    if (hdr->wakeup_sem != MACH_PORT_NULL)
        semaphore_destroy(mach_task_self(), hdr->wakeup_sem);
    if (hdr->wakeup_sem_prod != MACH_PORT_NULL)
        semaphore_destroy(mach_task_self(), hdr->wakeup_sem_prod);

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
 * If the handle was created by flipc2_channel_attach_remote(), the
 * shared memory is also unmapped (mapped_size > 0).
 * For intra-task peers (mapped_size == 0), only the handle is freed.
 */
flipc2_return_t
flipc2_channel_detach(
    flipc2_channel_t    channel)
{
    if (!channel)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Inter-task peer owns its mapping — unmap on detach */
    if (channel->mapped_size > 0 && channel->hdr) {
        vm_deallocate(mach_task_self(), (vm_address_t)channel->hdr,
                      channel->mapped_size);
    }

    /* Free the handle */
    vm_deallocate(mach_task_self(), (vm_address_t)channel,
                  sizeof(struct flipc2_channel));

    return FLIPC2_SUCCESS;
}

/*
 * flipc2_channel_attach_remote — Attach for inter-task peers with owned mapping.
 *
 * Like flipc2_channel_attach, but records mapped_size so that
 * flipc2_channel_detach() will vm_deallocate the shared memory.
 * Use this when the peer has its own vm_remap'd mapping.
 */
flipc2_return_t
flipc2_channel_attach_remote(
    vm_address_t        base_addr,
    uint32_t            mapped_size,
    flipc2_channel_t   *channel)
{
    flipc2_return_t ret;

    ret = flipc2_channel_attach(base_addr, channel);
    if (ret != FLIPC2_SUCCESS)
        return ret;

    (*channel)->mapped_size = mapped_size;
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

    /* Verify the mapping has the protections we need */
    if (!(cur_prot & VM_PROT_READ) || !(cur_prot & VM_PROT_WRITE)) {
        vm_deallocate(target_task, target_addr,
                      channel->hdr->channel_size);
        return FLIPC2_ERR_MAP_FAILED;
    }

    *out_addr = target_addr;
    return FLIPC2_SUCCESS;
}

/*
 * flipc2_channel_share_isolated — Share with per-role page protections.
 *
 * Maps the entire channel into target_task (one vm_remap for contiguous
 * mapping), then applies vm_protect per section:
 *   - Constants page (page 0): VM_PROT_READ
 *   - Producer page (page 1): RW for producer, RO for consumer
 *   - Consumer page (page 2): RO for producer, RW for consumer
 *   - Ring + data (page 3+):  RW for producer, RO for consumer
 *
 * Requires the channel was created with FLIPC2_CREATE_ISOLATED.
 */
flipc2_return_t
flipc2_channel_share_isolated(
    flipc2_channel_t    channel,
    mach_port_t         target_task,
    uint32_t            target_role,
    vm_address_t       *out_addr)
{
    flipc2_return_t ret;
    kern_return_t   kr;
    vm_address_t    base;
    uint32_t        cs;
    vm_prot_t       prod_prot, cons_prot, ring_prot;

    if (!channel || !channel->hdr || !out_addr)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    if (!(channel->hdr->layout_flags & FLIPC2_LAYOUT_ISOLATED))
        return FLIPC2_ERR_INVALID_ARGUMENT;

    if (target_role != FLIPC2_ROLE_PRODUCER &&
        target_role != FLIPC2_ROLE_CONSUMER)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* First, map the entire channel (full RW) */
    ret = flipc2_channel_share(channel, target_task, &base);
    if (ret != FLIPC2_SUCCESS)
        return ret;

    cs = channel->hdr->channel_size;

    /* Determine protections based on role */
    if (target_role == FLIPC2_ROLE_PRODUCER) {
        prod_prot = VM_PROT_READ | VM_PROT_WRITE;
        cons_prot = VM_PROT_READ;
        ring_prot = VM_PROT_READ | VM_PROT_WRITE;
    } else {
        prod_prot = VM_PROT_READ;
        cons_prot = VM_PROT_READ | VM_PROT_WRITE;
        ring_prot = VM_PROT_READ;
    }

    /* Constants page (page 0): read-only for both */
    kr = vm_protect(target_task,
                    base + FLIPC2_ISO_CONST_OFFSET,
                    4096, FALSE, VM_PROT_READ);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(target_task, base, cs);
        return FLIPC2_ERR_MAP_FAILED;
    }

    /* Producer page (page 1) */
    kr = vm_protect(target_task,
                    base + FLIPC2_ISO_PROD_OFFSET,
                    4096, FALSE, prod_prot);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(target_task, base, cs);
        return FLIPC2_ERR_MAP_FAILED;
    }

    /* Consumer page (page 2) */
    kr = vm_protect(target_task,
                    base + FLIPC2_ISO_CONS_OFFSET,
                    4096, FALSE, cons_prot);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(target_task, base, cs);
        return FLIPC2_ERR_MAP_FAILED;
    }

    /* Ring + data (page 3 to end) */
    kr = vm_protect(target_task,
                    base + FLIPC2_ISO_RING_OFFSET,
                    cs - FLIPC2_ISO_RING_OFFSET,
                    FALSE, ring_prot);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(target_task, base, cs);
        return FLIPC2_ERR_MAP_FAILED;
    }

    *out_addr = base;
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

/*
 * flipc2_channel_set_semaphores — Override per-handle semaphore port names.
 *
 * For inter-task peers: after flipc2_channel_attach_remote(), call
 * this to set the correct local IPC-space port names.  The inline
 * fast-path functions use these port names for semaphore_signal/wait.
 */
void
flipc2_channel_set_semaphores(
    flipc2_channel_t    ch,
    mach_port_t         sem,
    mach_port_t         sem_prod)
{
    ch->sem_port      = sem;
    ch->sem_port_prod = sem_prod;
}
