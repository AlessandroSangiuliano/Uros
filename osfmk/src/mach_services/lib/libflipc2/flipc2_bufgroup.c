/*
 * FLIPC v2 — Buffer group (shared memory pool for multi-channel data).
 *
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
 * libflipc2/flipc2_bufgroup.c
 *
 * Slow-path operations for buffer groups: create, destroy, attach,
 * detach, share.  The fast-path (alloc/free) is inlined in flipc2.h.
 *
 * A buffer group is a shared memory pool of fixed-size slots with a
 * lock-free LIFO free list (CAS-based, ABA-safe via tagged pointer).
 * Multiple SPSC channels can reference slots in the same pool,
 * reducing total memory usage when a server handles many clients.
 */

#include <flipc2.h>
#include <mach/mach_interface.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/mach_host.h>
#include <string.h>

static int
bg_is_power_of_2(uint32_t n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

static void
flipc2_bufgroup_init_handle(struct flipc2_bufgroup *bg,
                            struct flipc2_bufgroup_header *hdr)
{
    bg->hdr  = hdr;
    bg->next = (uint32_t *)((uint8_t *)hdr + hdr->next_offset);
    bg->data = (uint8_t *)hdr + hdr->data_offset;
    bg->mapped_size = 0;
}

/*
 * flipc2_bufgroup_create — Allocate and initialize a buffer group.
 *
 * pool_size:  requested total memory (will be rounded up as needed)
 * slot_size:  usable data bytes per slot (64..65536)
 * bg:         [out] buffer group handle
 */
flipc2_return_t
flipc2_bufgroup_create(
    uint32_t            pool_size,
    uint32_t            slot_size,
    flipc2_bufgroup_t  *bg_out)
{
    kern_return_t       kr;
    vm_address_t        addr;
    struct flipc2_bufgroup *bg;
    struct flipc2_bufgroup_header *hdr;
    uint32_t            slot_stride;
    uint32_t            next_offset, next_size;
    uint32_t            data_offset;
    uint32_t            slot_count;
    uint32_t            total_size;
    uint32_t            i;

    if (!bg_out)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    if (slot_size < FLIPC2_BUFGROUP_SLOT_SIZE_MIN ||
        slot_size > FLIPC2_BUFGROUP_SLOT_SIZE_MAX)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    if (pool_size < FLIPC2_BUFGROUP_POOL_SIZE_MIN ||
        pool_size > FLIPC2_BUFGROUP_POOL_SIZE_MAX)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Align slot size to 8 bytes */
    slot_stride = (slot_size + 7) & ~7u;

    /* Compute layout */
    next_offset = FLIPC2_BUFGROUP_HEADER_SIZE;
    slot_count  = pool_size / slot_stride;
    if (slot_count == 0)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    next_size   = slot_count * sizeof(uint32_t);
    data_offset = (next_offset + next_size + 7) & ~7u;
    total_size  = data_offset + slot_count * slot_stride;

    /* Allocate shared memory region */
    addr = 0;
    kr = vm_allocate(mach_task_self(), &addr, total_size, TRUE);
    if (kr != KERN_SUCCESS)
        return FLIPC2_ERR_RESOURCE_SHORTAGE;

    /* Wire the memory (best-effort) */
    (void)vm_wire(mach_host_self(), mach_task_self(), addr, total_size,
                  VM_PROT_READ | VM_PROT_WRITE);

    /* Initialize header */
    hdr = (struct flipc2_bufgroup_header *)addr;
    memset(hdr, 0, total_size);

    hdr->magic       = FLIPC2_BUFGROUP_MAGIC;
    hdr->version     = FLIPC2_BUFGROUP_VERSION;
    hdr->pool_size   = total_size;
    hdr->slot_size   = slot_size;
    hdr->slot_count  = slot_count;
    hdr->next_offset = next_offset;
    hdr->data_offset = data_offset;
    hdr->slot_stride = slot_stride;

    /* Build initial free list chain: 0→1→2→...→(n-1)→NONE */
    {
        uint32_t *next_arr = (uint32_t *)((uint8_t *)hdr + next_offset);

        for (i = 0; i < slot_count - 1; i++)
            next_arr[i] = i + 1;
        next_arr[slot_count - 1] = FLIPC2_BUFGROUP_SLOT_NONE;
    }

    /* Head points to slot 0, version 0 */
    hdr->free_head   = flipc2_bufgroup_make_head(0, 0);
    hdr->alloc_total = 0;
    hdr->free_total  = 0;

    /* Allocate the handle */
    kr = vm_allocate(mach_task_self(), (vm_address_t *)&bg,
                     sizeof(struct flipc2_bufgroup), TRUE);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(mach_task_self(), addr, total_size);
        return FLIPC2_ERR_RESOURCE_SHORTAGE;
    }

    flipc2_bufgroup_init_handle(bg, hdr);

    *bg_out = bg;
    return FLIPC2_SUCCESS;
}

/*
 * flipc2_bufgroup_destroy — Tear down and release resources.
 */
flipc2_return_t
flipc2_bufgroup_destroy(
    flipc2_bufgroup_t   bg)
{
    uint32_t size;

    if (!bg || !bg->hdr)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    size = bg->hdr->pool_size;

    /* Invalidate magic */
    bg->hdr->magic = 0;
    FLIPC2_WRITE_FENCE();

    /* Free shared memory region */
    vm_deallocate(mach_task_self(), (vm_address_t)bg->hdr, size);

    /* Free the handle */
    vm_deallocate(mach_task_self(), (vm_address_t)bg,
                  sizeof(struct flipc2_bufgroup));

    return FLIPC2_SUCCESS;
}

/*
 * flipc2_bufgroup_attach — Attach to an existing buffer group.
 */
flipc2_return_t
flipc2_bufgroup_attach(
    vm_address_t        base_addr,
    flipc2_bufgroup_t  *bg_out)
{
    kern_return_t       kr;
    struct flipc2_bufgroup *bg;
    struct flipc2_bufgroup_header *hdr;

    if (!base_addr || !bg_out)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    hdr = (struct flipc2_bufgroup_header *)base_addr;

    /* Validate header */
    if (hdr->magic != FLIPC2_BUFGROUP_MAGIC ||
        hdr->version != FLIPC2_BUFGROUP_VERSION)
        return FLIPC2_ERR_NOT_CONNECTED;

    if (hdr->slot_count == 0 || hdr->slot_stride == 0)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    if (hdr->next_offset != FLIPC2_BUFGROUP_HEADER_SIZE)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Allocate handle */
    kr = vm_allocate(mach_task_self(), (vm_address_t *)&bg,
                     sizeof(struct flipc2_bufgroup), TRUE);
    if (kr != KERN_SUCCESS)
        return FLIPC2_ERR_RESOURCE_SHORTAGE;

    flipc2_bufgroup_init_handle(bg, hdr);

    *bg_out = bg;
    return FLIPC2_SUCCESS;
}

/*
 * flipc2_bufgroup_attach_remote — Attach with owned mapping.
 */
flipc2_return_t
flipc2_bufgroup_attach_remote(
    vm_address_t        base_addr,
    uint32_t            mapped_size,
    flipc2_bufgroup_t  *bg_out)
{
    flipc2_return_t ret;

    ret = flipc2_bufgroup_attach(base_addr, bg_out);
    if (ret != FLIPC2_SUCCESS)
        return ret;

    (*bg_out)->mapped_size = mapped_size;
    return FLIPC2_SUCCESS;
}

/*
 * flipc2_bufgroup_detach — Detach without destroying.
 */
flipc2_return_t
flipc2_bufgroup_detach(
    flipc2_bufgroup_t   bg)
{
    if (!bg)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Inter-task peer owns its mapping */
    if (bg->mapped_size > 0 && bg->hdr) {
        vm_deallocate(mach_task_self(), (vm_address_t)bg->hdr,
                      bg->mapped_size);
    }

    /* Free the handle */
    vm_deallocate(mach_task_self(), (vm_address_t)bg,
                  sizeof(struct flipc2_bufgroup));

    return FLIPC2_SUCCESS;
}

/*
 * flipc2_bufgroup_share — Map buffer group into another task.
 */
flipc2_return_t
flipc2_bufgroup_share(
    flipc2_bufgroup_t   bg,
    mach_port_t         target_task,
    vm_address_t       *out_addr)
{
    kern_return_t   kr;
    vm_address_t    target_addr;
    vm_prot_t       cur_prot, max_prot;

    if (!bg || !bg->hdr || !out_addr)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    if (target_task == MACH_PORT_NULL)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    target_addr = 0;
    kr = vm_remap(target_task,
                  &target_addr,
                  bg->hdr->pool_size,
                  0,
                  TRUE,
                  mach_task_self(),
                  (vm_address_t)bg->hdr,
                  FALSE,
                  &cur_prot,
                  &max_prot,
                  VM_INHERIT_SHARE);
    if (kr != KERN_SUCCESS)
        return FLIPC2_ERR_MAP_FAILED;

    if (!(cur_prot & VM_PROT_READ) || !(cur_prot & VM_PROT_WRITE)) {
        vm_deallocate(target_task, target_addr, bg->hdr->pool_size);
        return FLIPC2_ERR_MAP_FAILED;
    }

    *out_addr = target_addr;
    return FLIPC2_SUCCESS;
}

/*
 * flipc2_channel_set_bufgroup — Associate a buffer group with a channel.
 */
void
flipc2_channel_set_bufgroup(
    flipc2_channel_t    ch,
    flipc2_bufgroup_t   bg)
{
    ch->bufgroup = bg;
}
