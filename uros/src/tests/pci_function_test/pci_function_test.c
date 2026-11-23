/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * pci_function_test -- a capability for one PCI function claims that function
 * and no other of its class, and is derived for a child with the address the
 * parent names (#982).
 *
 * It needs two devices of one class, and asks the HAL for two of QEMU's edu
 * (run-x86_64.sh's --edu2).  Elsewhere it says NOT ASKED and counts nothing.
 *
 *   [1] the HAL names two edu devices, A and B, of the same class;
 *   [2] a capability for A's function claims A;
 *   [3] the same capability is refused B, of the same class -- where a class
 *       capability would claim it, and the kernel says so;
 *   [4] a child's manifest written here at run time, naming A's address, is
 *       derived from this task's `delegatable 12 any': the child's port grants
 *       a capability for A and refuses one for B;
 *   [5] a grandchild whose parent, that child, has no `delegatable' line gets
 *       no port at all;
 *   [6] revoking [2]'s capability ends the claim on A;
 *   [7] a capability for one function, asked with the bus's own number, is
 *       refused the bus.  The HAL holds the bus, so the claim would be refused
 *       anyway: why it was, "a capability for one function claims no bus", is
 *       the kernel's line, which the merge test reads (pcifn).
 *
 * The child never runs: what its manifest grants is read by asking its port,
 * which answers by that manifest, for the task asking.
 */

#include <mach.h>
#include <mach/bootstrap.h>		/* bootstrap_ports() */
#include <mach/cap_types.h>
#include <mach/cap_manifest.h>
#include <mach/message.h>
#include <mach_init.h>			/* name_server_port */
#include <device/device_types.h>	/* DEVICE_BDF_BUS */

#include <stdio.h>
#include <string.h>

#include <servers/netname_wait.h>

#include <libcap.h>
#include "cap_server.h"			/* mig_cap_acquire, on a port of choice */

#include "hal_server.h"			/* struct hal_device_info */
#include "hal.h"			/* hal_list_devices */
#include "device_master.h"		/* device_claim, device_claim_holder */

#define EDU_VENDOR_DEVICE	0x11E81234u
#define ARMS			7
#define OPS			(CAP_OP_PCI_DMA_MAP | CAP_OP_PCI_MMIO_MAP | \
				 CAP_OP_PCI_IRQ)

static int		passed;
static mach_port_t	pf_master = MACH_PORT_NULL;
static mach_port_t	pf_self = MACH_PORT_NULL;	/* a port this task receives on */

static unsigned int
bdf_of(const struct hal_device_info *d)
{
	return ((unsigned)d->bus << 8) | ((unsigned)d->slot << 3) |
	       (unsigned)d->func;
}

static const char *
bdf_str(unsigned int bdf, char *buf, size_t n)
{
	snprintf(buf, n, "%02x:%02x.%x", bdf >> 8, (bdf >> 3) & 0x1f, bdf & 7);
	return buf;
}

/* The HAL's port, waited for on its check-in itself (#696). */
static mach_port_t
hal_port(void)
{
	mach_port_t	p = MACH_PORT_NULL;

	if (netname_wait(name_server_port, "hal", &p) != KERN_SUCCESS)
		return MACH_PORT_NULL;
	return p;
}

/* [1]: the first two edu devices the HAL lists, A and B. */
static int
find_two_edu(unsigned int *a, unsigned int *b, uint32_t *class_out)
{
	mach_port_t		hal = hal_port();
	vm_offset_t		data = 0;
	mach_msg_type_number_t	cnt = 0;
	const struct hal_device_info *d;
	unsigned int		i, n = 0, found = 0;

	if (hal == MACH_PORT_NULL)
		return -1;
	if (hal_list_devices(hal, &data, &cnt, &n) != KERN_SUCCESS) {
		(void)mach_port_deallocate(mach_task_self(), hal);
		return -1;
	}
	(void)mach_port_deallocate(mach_task_self(), hal);
	d = (const struct hal_device_info *)data;
	if (n > cnt / sizeof(*d))
		n = cnt / sizeof(*d);
	for (i = 0; i < n && found < 2; i++) {
		if (d[i].vendor_device != EDU_VENDOR_DEVICE)
			continue;
		if (found == 0) {
			*a = bdf_of(&d[i]);
			*class_out = d[i].class_rev >> 8;
		} else
			*b = bdf_of(&d[i]);
		found++;
	}
	if (cnt != 0)
		(void)vm_deallocate(mach_task_self(), data, cnt);
	return (int)found;
}

/* A capability asked on `port', which answers by that port's manifest. */
static kern_return_t
acquire_on(mach_port_t port, uint32_t type, uint64_t id, struct uros_cap *out)
{
	char			buf[CAP_TOKEN_MAX];
	mach_msg_type_number_t	len = CAP_TOKEN_MAX;
	kern_return_t		kr;

	kr = mig_cap_acquire(port, type, id, OPS, 0, buf, &len);
	if (kr == KERN_SUCCESS && len != sizeof(*out))
		return CAP_ERR_INVALID_TOKEN;
	if (kr == KERN_SUCCESS && out != NULL)
		memcpy(out, buf, sizeof(*out));
	return kr;
}

/* Whether this task holds the claim on bdf, as the kernel says: asked of the
 * task receiving on a port of this task's.  device_dma_owned() would answer the
 * same 0 for a device of this task's and for a device of nobody's. */
static int
owned(unsigned int bdf)
{
	natural_t	held = 0;

	return device_claim_holder(pf_master, bdf, pf_self, &held)
	       == KERN_SUCCESS && held == 1;
}

int
main(int argc, char **argv)
{
	mach_port_t		hostp = MACH_PORT_NULL, devp = MACH_PORT_NULL;
	mach_port_t		wired = MACH_PORT_NULL, paged = MACH_PORT_NULL;
	mach_port_t		security = MACH_PORT_NULL;
	mach_port_t		child = MACH_PORT_NULL, grandchild = MACH_PORT_NULL;
	mach_port_t		child_cap = MACH_PORT_NULL, gc_cap = MACH_PORT_NULL;
	struct uros_cap		tok_a, probe;
	cap_manifest_entry_t	want;
	char			blob[512], sa[16], sb[16];
	unsigned int		a = 0, b = 0, len;
	uint32_t		cls = 0;
	kern_return_t		kr, kr_b;
	int			n;

	(void)argc;
	(void)argv;
	printf("pci_function_test: started\n");

	if (bootstrap_ports(bootstrap_port, &hostp, &devp, &wired, &paged,
			    &security) != KERN_SUCCESS || devp == MACH_PORT_NULL) {
		printf("pci_function_test: [1] no master device port -- WRONG\n");
		printf("pci_function_test: %d of %d arms passed\n", passed, ARMS);
		return 0;
	}
	pf_master = devp;
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &pf_self) != KERN_SUCCESS ||
	    mach_port_insert_right(mach_task_self(), pf_self, pf_self,
				   MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
		printf("pci_function_test: [1] no port of its own -- WRONG\n");
		printf("pci_function_test: %d of %d arms passed\n", passed, ARMS);
		return 0;
	}

	/* [1] */
	n = find_two_edu(&a, &b, &cls);
	if (n < 2) {
		printf("pci_function_test: NOT ASKED -- %s, and two devices of one "
		       "class are what it asks about (--edu2)\n",
		       n < 0 ? "the HAL could not be asked" :
		       n == 0 ? "the HAL names no edu device" :
		       "the HAL names one edu device");
		printf("pci_function_test: 0 of 0 arms passed\n");
		return 0;
	}
	printf("pci_function_test: [1] the HAL names two edu devices of class "
	       "0x%06x, A at %s and B at %s\n", (unsigned)cls,
	       bdf_str(a, sa, sizeof sa), bdf_str(b, sb, sizeof sb));
	passed++;

	/* [2] */
	kr = cap_request(RESOURCE_PCI_FUNCTION, a, OPS, 0, &tok_a);
	if (kr == KERN_SUCCESS)
		kr = device_claim(pf_master, a, (char *)&tok_a,
				  sizeof(tok_a));
	if (kr == KERN_SUCCESS && owned(a)) {
		printf("pci_function_test: [2] a capability for function %s "
		       "claimed it\n", sa);
		passed++;
	} else
		printf("pci_function_test: [2] a capability for function %s did "
		       "not claim it: 0x%x -- WRONG\n", sa, kr);

	/* [3] */
	kr_b = device_claim(pf_master, b, (char *)&tok_a, sizeof(tok_a));
	if (kr_b == KERN_NO_ACCESS && !owned(b)) {
		printf("pci_function_test: [3] the capability for function %s was "
		       "refused %s, of the same class: KERN_NO_ACCESS\n", sa, sb);
		passed++;
	} else
		printf("pci_function_test: [3] the capability for function %s "
		       "claimed %s, of the same class: 0x%x -- WRONG, it claims "
		       "any device of its class\n", sa, sb, kr_b);

	/* [4] */
	memset(&want, 0, sizeof want);
	want.type = RESOURCE_PCI_FUNCTION;
	want.resource_id = a;
	want.ops = OPS;
	len = cap_manifest_build("pci_function_child", &want, 1, NULL, 0,
				 blob, sizeof blob);
	kr = len == 0 ? KERN_INVALID_ARGUMENT :
	     task_create(mach_task_self(), NULL, 0, FALSE, &child);
	if (kr == KERN_SUCCESS)
		kr = cap_derive_child(mach_cap_port, child, blob, len,
				      &child_cap);
	if (kr != KERN_SUCCESS)
		printf("pci_function_test: [4] no child port derived from a "
		       "manifest naming %s: 0x%x -- WRONG\n", sa, kr);
	else {
		kr = acquire_on(child_cap, RESOURCE_PCI_FUNCTION, a, &probe);
		kr_b = acquire_on(child_cap, RESOURCE_PCI_FUNCTION, b, NULL);
		if (kr == KERN_SUCCESS && kr_b != KERN_SUCCESS) {
			printf("pci_function_test: [4] a child derived with %s's "
			       "address is granted a capability for %s and refused "
			       "one for %s: 0x%x\n", sa, sa, sb, kr_b);
			passed++;
		} else
			printf("pci_function_test: [4] a child derived with %s's "
			       "address: for %s 0x%x, for %s 0x%x -- WRONG, its "
			       "manifest is not the address it was given\n",
			       sa, sa, kr, sb, kr_b);
	}

	/* [5] */
	if (child_cap == MACH_PORT_NULL)
		printf("pci_function_test: [5] no child to derive from -- "
		       "WRONG\n");
	else {
		kr = task_create(mach_task_self(), NULL, 0, FALSE, &grandchild);
		if (kr == KERN_SUCCESS)
			kr = cap_derive_child(child_cap, grandchild, blob, len,
					      &gc_cap);
		if (kr != KERN_SUCCESS && gc_cap == MACH_PORT_NULL) {
			printf("pci_function_test: [5] a grandchild whose parent "
			       "has no delegatable line got no port: 0x%x\n", kr);
			passed++;
		} else
			printf("pci_function_test: [5] a grandchild whose parent "
			       "has no delegatable line got a port: 0x%x -- WRONG\n",
			       kr);
	}

	/* [6] */
	kr = cap_revoke(tok_a.cap_id);
	if (kr == KERN_SUCCESS && !owned(a)) {
		printf("pci_function_test: [6] revoking the capability for %s "
		       "ended its claim\n", sa);
		passed++;
	} else
		printf("pci_function_test: [6] revoking the capability for %s: "
		       "0x%x, and the claim is %s -- WRONG\n", sa, kr,
		       owned(a) ? "still held" : "gone");

	/* [7] */
	kr = cap_request(RESOURCE_PCI_FUNCTION, DEVICE_BDF_BUS, OPS, 0, &probe);
	if (kr != KERN_SUCCESS)
		printf("pci_function_test: [7] no capability for one function with "
		       "the bus's number: 0x%x -- WRONG\n", kr);
	else {
		kr = device_claim(pf_master, DEVICE_BDF_BUS, (char *)&probe,
				  sizeof(probe));
		if (kr != KERN_SUCCESS) {
			printf("pci_function_test: [7] a capability for one "
			       "function, with the bus's number, was refused the "
			       "bus: 0x%x\n", kr);
			passed++;
		} else
			printf("pci_function_test: [7] a capability for one "
			       "function, with the bus's number, claimed the bus "
			       "-- WRONG\n");
	}

	if (grandchild != MACH_PORT_NULL)
		(void)task_terminate(grandchild);
	if (child != MACH_PORT_NULL)
		(void)task_terminate(child);
	printf("pci_function_test: %d of %d arms passed\n", passed, ARMS);
	return 0;
}
