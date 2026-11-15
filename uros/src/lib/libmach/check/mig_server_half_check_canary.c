/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The routine every server half MIG writes defines, made to be seen:
 * mig_server_half_check.sh --must-refuse reads this object each time it
 * checks libmach's archives, and stops the build if it no longer finds it
 * (#931).  It is never linked into anything.
 */

void *canary_server_routine(void *in);

void *
canary_server_routine(void *in)
{
	return in;
}
