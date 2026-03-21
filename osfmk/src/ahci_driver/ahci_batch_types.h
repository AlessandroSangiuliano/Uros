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

#endif /* AHCI_BATCH_TYPES_H */
