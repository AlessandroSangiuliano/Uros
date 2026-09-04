/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * hal_state.h — what the HAL says about a device's driver (#513)
 *
 * 🔑 ITS OWN HEADER because two servers need the same three numbers, and the
 * alternative is what #427 found four times over in this subsystem: private
 * copies of a shared definition, agreeing until the day one of them is edited.
 * The HAL sets these values and the drivers report them, so neither side owns
 * the file.
 */

#ifndef _HAL_STATE_H_
#define _HAL_STATE_H_

/*
 * 🔴 THREE VALUES, AND EACH ONE IS WRITTEN BY SOMEBODY.  #427 removed a
 * four-value `status' from this registry because nothing ever set it -- every
 * scan wrote UNBOUND and the setter was called by no code -- and a field two
 * RPCs report and nothing can change is worse than a field that is not there.
 *
 * There is deliberately no PROBING here.  Nothing would write it: a driver
 * claims a device from the kernel and probes it in one stretch, with no point
 * in between at which it would tell the HAL it had begun.  A fourth value
 * nobody sets would put back exactly what was removed.
 */
#define HAL_DEV_UNCLAIMED	0	/* what a scan produces */
#define HAL_DEV_BOUND		1	/* a driver probed it, and holds it */
#define HAL_DEV_PROBE_FAILED	2	/* a driver tried and gave it back */

#endif /* _HAL_STATE_H_ */
