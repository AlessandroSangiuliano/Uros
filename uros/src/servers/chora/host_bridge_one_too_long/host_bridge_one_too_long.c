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
 * host_bridge_one_too_long -- a module of chora that no chora could run, in
 * the disk's pool misc (#964).  Its name has 24 characters, one more than a
 * driver's task name leaves a module's (DRIVER_MODULE_NAME_MAX), and it
 * declares the host bridge of either board QEMU gives, i440FX's 8086:1237 and
 * Q35's 8086:29c0, which every boot has.  The HAL must refuse it as it reads
 * the pool, and say so: a HAL that took it would start a chora on the host
 * bridge that died at each start, naming its task, until it gave up.  Its
 * operations are there under its name, so that the name alone refuses it;
 * its attach, which no chora reaches, says so if one ever does.
 */

#include <stdio.h>

#include "libdriver.h"
#include "chora_module.h"

static int
too_long_attach(struct driver *d)
{
	printf("%s: WRONG -- host_bridge_one_too_long attached, whose name no "
	       "chora can name its task after (#964)\n", d->name);
	return -1;
}

const struct chora_module_ops host_bridge_one_too_long_chora_ops = {
	.name	= "host_bridge_one_too_long",
	.attach	= too_long_attach,
};

/* i440FX's host bridge, and Q35's (#964). */
CHORA_DRIVES(CHORA_DRIVES_PCI_ID(0x8086, 0x1237),
	     CHORA_DRIVES_PCI_ID(0x8086, 0x29c0));
