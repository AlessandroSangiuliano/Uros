/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The console kept in RAM for Linux to read back after a reset (#373).
 */

#ifndef	_X86_64_DDB_RAMOOPS_H_
#define	_X86_64_DDB_RAMOOPS_H_

#include <stdint.h>

/*
 * The two sizes Linux is given on each machine, and this side must agree
 * with them (~/uros-tests/ramoops/ramoops-setup.sh): memmap=1M$ADDR and
 * ramoops.mem_size=0x100000 reserve the megabyte, and with
 * ramoops.record_size=0 the console zone is its first console_size=0x80000
 * bytes.  The whole megabyte is kept from the page allocator; only the
 * console zone is written.
 */
#define	RAMOOPS_RESERVED	0x100000ULL
#define	RAMOOPS_CONSOLE		0x80000ULL

/*
 * `ramoops=ADDR' on the command line: 1 and the address when it is given and
 * is page-aligned, 0 when it is not given, -1 when it is given and is not an
 * address (ramoops_init() says so).  Read wherever it is asked, like every
 * boot argument here.
 */
int	ramoops_wanted(uint64_t *pa);

/*
 * Once the pmap can split the direct map: the zone made uncached, started,
 * and given what the console kept before it existed (#666).  Says what it
 * did, or why there is no zone.
 */
void	ramoops_init(void);

/* A byte the console said: to the zone, from any context, never waiting long. */
void	ramoops_putc(char c);

/* Whether the zone is being written, and how many bytes it has taken. */
int	ramoops_live(uint64_t *pa, uint32_t *bytes, uint32_t *dropped);

/*
 * The zone read back as ramoops will read it and compared with what was
 * written, said in one line ending WRONG when it does not match.  For the
 * reset, the moment it matters; nothing when there is no zone.
 */
void	ramoops_check(void);

#endif	/* _X86_64_DDB_RAMOOPS_H_ */
