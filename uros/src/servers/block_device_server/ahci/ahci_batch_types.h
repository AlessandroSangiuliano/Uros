/*
 * C type definitions for ahci_batch.defs MIG interface.
 *
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex)
 * SPDX-License-Identifier: MIT
 */

#ifndef AHCI_BATCH_TYPES_H
#define AHCI_BATCH_TYPES_H

#include <device/device_types.h>

#ifndef unsigned32
#define unsigned32	unsigned int
#endif

typedef recnum_t	*batch_recnum_array_t;
typedef unsigned int	*batch_size_array_t;
/*
 * 🔴 vm_address_t since #520.  This one stays INLINE -- it is an argument of
 * device_read_phys on every disk request, where out-of-line would be
 * page-table work to move eight bytes.  See ahci_batch.defs.
 */
typedef vm_address_t	*dma_sg_addr_t;

#endif /* AHCI_BATCH_TYPES_H */
