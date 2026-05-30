/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * urmach_version.h — UrMach kernel version macros (BSD-style).
 *
 * UrMach is the Uros microkernel: an evolution of OSF Mach.  Its version
 * track is independent of the historical OSF/MkLinux numbering (the
 * "OSFMK 1.1" / conf/version.* metadata is heritage and not bumped here).
 *
 * Bump rules (BSD-style):
 *   - MAJOR  — incompatible ABI / kernel-call breakage
 *   - MINOR  — backwards-compatible new feature surface
 *   - PATCH  — bug fixes, optimisations, no API change
 *
 * Update URMACH_VERSION_STRING in lock-step with the numeric macros.
 */

#ifndef _MACH_URMACH_VERSION_H_
#define _MACH_URMACH_VERSION_H_

#define URMACH_VERSION_MAJOR   0
#define URMACH_VERSION_MINOR   1
#define URMACH_VERSION_PATCH   0
#define URMACH_VERSION_STRING  "UrMach 0.1.0"

#endif /* _MACH_URMACH_VERSION_H_ */
