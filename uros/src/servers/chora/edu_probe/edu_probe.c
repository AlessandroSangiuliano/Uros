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
 * edu_probe -- a test module of chora, for QEMU's educational device: it maps
 * a region, takes one interrupt by message (#977), and moves a buffer through
 * the device and back by DMA (#978).
 *
 * edu (vendor 1234h, device 11e8h, docs/specs/edu.rst in QEMU 11.1.2) has MSI
 * with one message and no MSI-X table, which is why it is here: a device of
 * the kind that took its INTx line before #977, and on q35 could not even do
 * that, since libdriver takes no line above 15.  Written to its register 60h,
 * it raises an interrupt and keeps the value in its status, 24h, until the
 * value is written to 64h -- which the handler does, as edu.rst says a driver
 * using MSI must.
 *
 * It also has a buffer of its own, 4096 bytes at 40000h of its address space,
 * and a DMA engine that copies between that buffer and an address of the
 * machine's: told a source, a destination, a count and a direction, it moves
 * the bytes 100 ms later by its clock, then raises 100h.  That is the round
 * trip below: a pattern from a buffer of driver_dma_alloc() into the device,
 * the buffer zeroed, and the device's copy back into it -- so a byte that
 * reads right came through the device, at the address libdriver gave.
 *
 * So: the device's first region mapped; the interrupt asked of libdriver,
 * which must be a message; 977h raised; up to a second for the handler; the
 * round trip at the device's whole reach; the device's reach declared as
 * edu's default 28 bits, and the round trip again, through an address the
 * kernel must place below them (#417); then the reach declared as 20 bits,
 * where neither memory nor addresses are free, and a buffer that must be
 * refused -- the system going on after it (#821).  chora said the claim, and
 * this says the rest.
 *
 * ⚠️ A test module: what it was asked to see and did not is a WRONG.
 */

#include "libdriver.h"
#include "chora_module.h"

#include <mach.h>
#include <mach/thread_switch.h>
#include <stdio.h>			/* snprintf */

#define EDU_INTR_STATUS		0x24u
#define EDU_INTR_RAISE		0x60u
#define EDU_INTR_ACK		0x64u
#define EDU_INTR_VALUE		0x977u

/* The DMA engine's registers, eight bytes wide (edu.rst). */
#define EDU_DMA_SRC		0x80u
#define EDU_DMA_DST		0x88u
#define EDU_DMA_COUNT		0x90u
#define EDU_DMA_CMD		0x98u
#define EDU_DMA_RUN		0x1u
#define EDU_DMA_TO_RAM		0x2u		/* 0: from the machine's memory */
#define EDU_DMA_RAISE		0x4u
#define EDU_DMA_DONE		0x100u		/* the status it raises */

#define EDU_BUF			0x40000u	/* in the device's own space */
#define EDU_BUF_BYTES		4096u

/*
 * What edu's DMA reaches.  Its default is 28 bits, for students to honour; the
 * harness gives it all 64 (run-x86_64.sh's --edu), so that the first round
 * trip, at the whole reach libdriver starts a device with, can go through an
 * IOVA from 4 GiB up (IOMMU_IOVA_BASE).  edu has no register that says, so
 * that is the harness's word.  ⚠️ Booted without it, edu cuts every address to
 * 28 bits: an IOVA becomes one its domain never granted, and the round trip
 * faults and says WRONG; a physical address above 256 MiB would become
 * someone else's memory.
 *
 * Then the reach is declared as edu's default, and the kernel must place the
 * address below it (#417); and as 20 bits, 1 MiB, below which the kernel
 * hands out no memory -- the first megabyte is the firmware's, under
 * bootmem.c's low water mark -- and the low IOVA window does not begin.
 */
#define EDU_DMA_REACH_DEFAULT	((1ULL << 28) - 1)
#define EDU_DMA_REACH_NONE	((1ULL << 20) - 1)

#define PROBE_WAIT_STEPS	100		/* of 10 ms: a second */
#define DMA_WAIT_STEPS		300		/* of 10 ms: three seconds */

static volatile uint32_t	*regs;
static volatile uint64_t	*regs64;	/* the same, by eight bytes */
static volatile int		rang;
static volatile uint32_t	rang_status;

/* On the interrupt thread: what was raised, read and acknowledged. */
static void
edu_interrupt(struct driver *d, void *arg)
{
	(void)d;
	(void)arg;
	rang_status = regs[EDU_INTR_STATUS / 4];
	regs[EDU_INTR_ACK / 4] = rang_status;
	rang++;
}

/* Byte `i' of the round trip's pattern: no two blocks of 256 bytes alike. */
static uint8_t
edu_pattern(unsigned int i)
{
	return (uint8_t)(i ^ (i >> 8) ^ 0xA5u);
}

/*
 * One transfer of the device's whole buffer, and the interrupt that says it
 * is done: 0, or -1 with a line.  ⚠️ On -1 the device may be moving the bytes
 * still, and the caller keeps the buffer rather than give back memory a
 * device may yet write.
 */
static int
edu_dma(struct driver *d, uint64_t src, uint64_t dst, uint32_t cmd)
{
	int	before = rang;
	int	i;

	regs64[EDU_DMA_SRC / 8] = src;
	regs64[EDU_DMA_DST / 8] = dst;
	regs64[EDU_DMA_COUNT / 8] = EDU_BUF_BYTES;
	regs64[EDU_DMA_CMD / 8] = cmd | EDU_DMA_RUN | EDU_DMA_RAISE;

	for (i = 0; i < DMA_WAIT_STEPS && rang == before; i++)
		(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 10);

	if (rang == before) {
		driver_log(d, "WRONG -- told to move %u bytes from 0x%llx to "
			   "0x%llx, the device raised nothing in 3 s",
			   EDU_BUF_BYTES, (unsigned long long)src,
			   (unsigned long long)dst);
		return -1;
	}
	if ((rang_status & EDU_DMA_DONE) == 0) {
		driver_log(d, "WRONG -- told to move %u bytes from 0x%llx to "
			   "0x%llx, the device raised 0x%x, not 0x%x",
			   EDU_BUF_BYTES, (unsigned long long)src,
			   (unsigned long long)dst, (unsigned)rang_status,
			   EDU_DMA_DONE);
		return -1;
	}
	return 0;
}

/*
 * What the kernel says of the device's DMA between two readings, as the end
 * of a line: `said' is for the ones that carry numbers.
 */
static const char *
edu_refusals(const struct driver_dma_faults *before,
	     const struct driver_dma_faults *after, char *said, size_t len)
{
	switch (driver_dma_verdict(before, after)) {
	case DRIVER_DMA_NOT_CONFINED:
		return "on a device nothing confines";
	case DRIVER_DMA_NOT_REFUSED:
		return "and the kernel refused none of it";
	case DRIVER_DMA_REFUSED:
		(void)snprintf(said, len, "and the kernel refused it %u times, "
			       "the last at 0x%llx",
			       (unsigned)(after->count - before->count),
			       (unsigned long long)after->address);
		return said;
	case DRIVER_DMA_UNKNOWN:
		break;
	}
	(void)snprintf(said, len, "and the kernel cannot say what it refused: "
		       "%u refusals may have gone uncounted",
		       (unsigned)(after->lost - before->lost));
	return said;
}

/*
 * The round trip: the pattern into the device's buffer from one of
 * driver_dma_alloc()'s, that buffer zeroed, and the device's copy back into
 * it.  Every byte must read as it was written, and where the machine confines
 * the device the kernel must have refused none of its DMA: read once the
 * buffer is granted, when the device has its domain, and once both halves are
 * done.
 */
static int
edu_round_trip(struct driver *d, const char *within)
{
	struct driver_dma		buf;
	struct driver_dma_faults	before, after;
	enum driver_dma_verdict		verdict;
	volatile uint8_t		*p;
	kern_return_t			kr;
	unsigned int			i, wrong = 0, first = 0;
	char				said[96];

	kr = driver_dma_alloc(d, EDU_BUF_BYTES, &buf);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "WRONG -- no DMA buffer of %u bytes%s: 0x%x",
			   EDU_BUF_BYTES, within, kr);
		return -1;
	}
	if (driver_dma_faults(d, &before) != KERN_SUCCESS) {
		driver_log(d, "WRONG -- no reading of the device's refused DMA "
			   "to compare with");
		driver_dma_free(d, &buf);
		return -1;
	}
	p = (volatile uint8_t *)buf.va;

	for (i = 0; i < EDU_BUF_BYTES; i++)
		p[i] = edu_pattern(i);
	if (edu_dma(d, buf.device_addr, EDU_BUF, 0) != 0)
		return -1;

	for (i = 0; i < EDU_BUF_BYTES; i++)
		p[i] = 0;
	if (edu_dma(d, EDU_BUF, buf.device_addr, EDU_DMA_TO_RAM) != 0)
		return -1;

	if (driver_dma_faults(d, &after) != KERN_SUCCESS) {
		driver_log(d, "WRONG -- no reading of the device's refused DMA "
			   "after the round trip");
		driver_dma_free(d, &buf);
		return -1;
	}
	verdict = driver_dma_verdict(&before, &after);

	for (i = 0; i < EDU_BUF_BYTES; i++)
		if (p[i] != edu_pattern(i) && wrong++ == 0)
			first = i;
	if (wrong != 0) {
		driver_log(d, "WRONG -- a DMA round trip of %u bytes through the "
			   "device's buffer%s, at device address 0x%llx: %u "
			   "bytes came back different, the first at %u, 0x%02x "
			   "for 0x%02x, %s", EDU_BUF_BYTES, within,
			   (unsigned long long)buf.device_addr, wrong, first,
			   (unsigned)p[first], (unsigned)edu_pattern(first),
			   edu_refusals(&before, &after, said, sizeof said));
		driver_dma_free(d, &buf);
		return -1;
	}
	if (verdict == DRIVER_DMA_REFUSED || verdict == DRIVER_DMA_UNKNOWN) {
		driver_log(d, "WRONG -- a DMA round trip of %u bytes through the "
			   "device's buffer%s, at device address 0x%llx: every "
			   "byte came back, %s", EDU_BUF_BYTES, within,
			   (unsigned long long)buf.device_addr,
			   edu_refusals(&before, &after, said, sizeof said));
		driver_dma_free(d, &buf);
		return -1;
	}
	driver_log(d, "a DMA round trip of %u bytes through the device's "
		   "buffer%s, at device address 0x%llx: every byte came back, "
		   "%s", EDU_BUF_BYTES, within,
		   (unsigned long long)buf.device_addr,
		   edu_refusals(&before, &after, said, sizeof said));
	driver_dma_free(d, &buf);
	return 0;
}

/*
 * The reach declared (#417): edu's default 28 bits, and the round trip again,
 * through an address the kernel placed below them -- from the low IOVA window
 * where the machine translates, memory below 256 MiB where it does not; then
 * 20 bits, below which no memory is free and the low window does not begin,
 * and a buffer that must be refused.  That refusal is the kernel's to make,
 * not libdriver's: libdriver's own line, that the kernel placed a buffer past
 * the reach, would be the kernel's promise broken, and the merge test asks
 * for its absence.
 */
static int
edu_reaches(struct driver *d)
{
	struct driver_dma	buf;
	kern_return_t		kr;

	if (driver_dma_reach(d, EDU_DMA_REACH_DEFAULT) != KERN_SUCCESS) {
		driver_log(d, "WRONG -- the device's reach, edu's default 28 "
			   "bits, was not taken");
		return -1;
	}
	if (edu_round_trip(d, " within edu's default 28 bits") != 0)
		return -1;

	if (driver_dma_reach(d, EDU_DMA_REACH_NONE) != KERN_SUCCESS) {
		driver_log(d, "WRONG -- the device's reach, 20 bits, was not "
			   "taken");
		return -1;
	}
	kr = driver_dma_alloc(d, EDU_BUF_BYTES, &buf);
	if (kr == KERN_SUCCESS) {
		driver_log(d, "WRONG -- a DMA buffer within 20 bits, where "
			   "nothing is free: given, at device address 0x%llx",
			   (unsigned long long)buf.device_addr);
		driver_dma_free(d, &buf);
		return -1;
	}
	driver_log(d, "a DMA buffer within 20 bits, where nothing is free: "
		   "refused, 0x%x", kr);
	return 0;
}

static int
edu_attach(struct driver *d)
{
	volatile void	*va = NULL;
	vm_size_t	size = 0;
	kern_return_t	kr;
	int		i;

	kr = driver_map_region(d, 0, &va, &size);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "WRONG -- region 0, the registers, would not map: "
			   "0x%x", kr);
		return -1;
	}
	regs = (volatile uint32_t *)va;
	regs64 = (volatile uint64_t *)va;
	driver_log(d, "mapped region 0, %lu bytes at 0x%lx", (unsigned long)size,
		   (unsigned long)(vm_address_t)va);

	kr = driver_interrupt(d, edu_interrupt, NULL);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "WRONG -- no interrupt to take: 0x%x", kr);
		return -1;
	}
	if (!d->irq_message) {
		driver_log(d, "WRONG -- given INTx line %u, where its MSI was a "
			   "message to take", (unsigned)d->irq_slot);
		return -1;
	}

	regs[EDU_INTR_RAISE / 4] = EDU_INTR_VALUE;
	for (i = 0; i < PROBE_WAIT_STEPS && rang == 0; i++)
		(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 10);

	if (rang == 0) {
		driver_log(d, "WRONG -- told to raise 0x%x, the device sent "
			   "nothing in a second to message slot %u",
			   EDU_INTR_VALUE, (unsigned)d->irq_slot);
		return -1;
	}
	driver_log(d, "took an interrupt, message slot %u, status 0x%x",
		   (unsigned)d->irq_slot, (unsigned)rang_status);

	if (edu_round_trip(d, "") != 0)
		return -1;
	return edu_reaches(d);
}

const struct chora_module_ops edu_probe_chora_ops = {
	.name	= "edu_probe",
	.attach	= edu_attach,
};
