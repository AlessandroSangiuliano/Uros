/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The canary's pair (#733): this object goes twice, under one member name, into
 * the canary's second part, so uros_libc_check_canary_pair is a name exactly
 * two members define, and they share a name.  A check that wanted more than
 * two, or that told members apart by name alone, would let it through.
 * Nothing links it.
 */
int	uros_libc_check_canary_pair = 1;
