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
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * crash_probe -- a test module of chora that dies at its start, every time
 * (#964).  It declares QEMU's pci-testdev, vendor 1b36h and device 0005h,
 * which run-x86_64.sh --testdev puts on the board, and its attach says it
 * dies and fails: its chora ends as soon as it has claimed the device.  The
 * HAL starts it again after each of its delays and gives up after
 * HAL_CHORA_STARTS_MAX starts, which chora_test counts with the HAL's
 * get_driver_state().
 */

#include <stdio.h>

#include "libdriver.h"
#include "chora_module.h"

static int
crash_attach(struct driver *d)
{
	printf("%s: crash_probe dies at its start, as it is made to (#964)\n",
	       d->name);
	return -1;
}

const struct chora_module_ops crash_probe_chora_ops = {
	.name	= "crash_probe",
	.attach	= crash_attach,
};

CHORA_DRIVES(CHORA_DRIVES_PCI_ID(0x1b36, 0x0005));
