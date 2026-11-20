/*
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
 * elf32_probe -- an i386 program on the x86-64 root disk, for exec_server to
 * refuse (#963).
 *
 * exec_server runs this machine's images only, and musl_test's arm [51] asks
 * it for this one and must be told ENOEXEC.  It is not meant to run, and it
 * does nothing that could pass for success if it did: its first instruction
 * is UD2, which faults in either mode, so a server that started it leaves a
 * child killed by a signal and not one that exited.
 */
	.text
	.globl	_start
_start:
	ud2
