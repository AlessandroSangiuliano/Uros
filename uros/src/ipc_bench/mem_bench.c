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
 * mem_bench.c — Memory bandwidth benchmark module for ipc_bench
 *
 * Measures throughput of memcpy, memmove, memset, and bzero at
 * various buffer sizes (1 KB to 1 MB).
 *
 * Uses best-of-5 runs to filter out KVM scheduling jitter:
 * each run times a batch of iterations, and the shortest
 * (least-disturbed) run is reported.
 */

#include <mach.h>
#include <mach/clock.h>
#include <mach/clock_types.h>
#include <sa_mach.h>
#include <stdio.h>
#include <string.h>

#include "mem_bench.h"

extern void	bzero(void *, unsigned int);

/* ===================================================================
 * Timing helpers
 * =================================================================== */

static mach_port_t	mem_clock_port;

static void
mget_time(tvalspec_t *tv)
{
	clock_get_time(mem_clock_port, tv);
}

static unsigned long
melapsed_us(const tvalspec_t *before, const tvalspec_t *after)
{
	unsigned long us;
	us  = (unsigned long)(after->tv_sec  - before->tv_sec)  * 1000000UL;
	us += (unsigned long)(after->tv_nsec - before->tv_nsec) / 1000UL;
	return us;
}

/* ===================================================================
 * Prevent dead-code elimination
 * =================================================================== */

static volatile unsigned char sink_byte;

static inline void
use_buffer(const void *buf, unsigned int size)
{
	sink_byte = ((const volatile unsigned char *)buf)[size - 1];
	__asm__ __volatile__("" ::: "memory");
}

/* ===================================================================
 * Buffer sizes to benchmark
 * =================================================================== */

struct bench_size {
	unsigned int	size;
	const char	*label;
	unsigned int	iters;
};

static const struct bench_size sizes[] = {
	{    1024,  "1 KB",  100000 },
	{    4096,  "4 KB",   50000 },
	{   16384, "16 KB",   20000 },
	{   65536, "64 KB",    5000 },
	{  262144, "256 KB",   2000 },
	{ 1048576,  "1 MB",    500 },
};

#define NSIZES	(sizeof(sizes) / sizeof(sizes[0]))
#define MAX_BUF	1048576
#define NRUNS	5

/* ===================================================================
 * Generic timing: run a batch NRUNS times, keep best (shortest)
 * =================================================================== */

typedef void (*mem_op_fn)(void *dst, const void *src,
			  unsigned int size, unsigned int iters);

static void
do_memcpy(void *dst, const void *src, unsigned int size, unsigned int iters)
{
	unsigned int i;
	for (i = 0; i < iters; i++) {
		memcpy(dst, src, size);
		use_buffer(dst, size);
	}
}

static void
do_memmove_fwd(void *dst, const void *src,
	       unsigned int size, unsigned int iters)
{
	unsigned int	i;
	unsigned int	off = (size > 64) ? 64 : size / 4;
	(void)src;
	for (i = 0; i < iters; i++) {
		memmove((char *)dst + off, dst, size - off);
		use_buffer((char *)dst + off, size - off);
	}
}

static void
do_memmove_bwd(void *dst, const void *src,
	       unsigned int size, unsigned int iters)
{
	unsigned int	i;
	unsigned int	off = (size > 64) ? 64 : size / 4;
	(void)src;
	for (i = 0; i < iters; i++) {
		memmove(dst, (char *)dst + off, size - off);
		use_buffer(dst, size - off);
	}
}

static void
do_memset(void *dst, const void *src, unsigned int size, unsigned int iters)
{
	unsigned int i;
	(void)src;
	for (i = 0; i < iters; i++) {
		memset(dst, 0xAA, size);
		use_buffer(dst, size);
	}
}

static void
do_bzero(void *dst, const void *src, unsigned int size, unsigned int iters)
{
	unsigned int i;
	(void)src;
	for (i = 0; i < iters; i++) {
		bzero(dst, size);
		use_buffer(dst, size);
	}
}

static void
run_bench(const char *name, mem_op_fn fn,
	  void *dst, const void *src, const struct bench_size *bs)
{
	tvalspec_t	t0, t1;
	unsigned long	best_us, run_us, throughput;
	int		r;

	/* warmup */
	fn(dst, src, bs->size, 100);

	best_us = 0xFFFFFFFFUL;
	for (r = 0; r < NRUNS; r++) {
		mget_time(&t0);
		fn(dst, src, bs->size, bs->iters);
		mget_time(&t1);
		run_us = melapsed_us(&t0, &t1);
		if (run_us < best_us)
			best_us = run_us;
	}

	if (best_us == 0)
		best_us = 1;

	throughput = (unsigned long)bs->iters * (bs->size / 1024) * 1000 /
		     best_us * 1000 / 1024;

	printf("  %-14s %-6s  %8lu us  (%u iters, best of %d, %lu MB/s)\n",
	       name, bs->label, best_us, bs->iters, NRUNS, throughput);
}

/* ===================================================================
 * Public entry point
 * =================================================================== */

void
bench_mem_run(mach_port_t clock_port)
{
	vm_offset_t	src_addr, dst_addr;
	kern_return_t	kr;
	unsigned int	s;

	mem_clock_port = clock_port;

	kr = vm_allocate(mach_task_self(), &src_addr, MAX_BUF, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("mem_bench: vm_allocate src failed: %d\n", kr);
		return;
	}

	kr = vm_allocate(mach_task_self(), &dst_addr, MAX_BUF, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("mem_bench: vm_allocate dst failed: %d\n", kr);
		vm_deallocate(mach_task_self(), src_addr, MAX_BUF);
		return;
	}

	/* Fault in all pages */
	memset((void *)src_addr, 0x55, MAX_BUF);
	memset((void *)dst_addr, 0xAA, MAX_BUF);

	printf("--- Memory bandwidth (best of %d runs) ---\n", NRUNS);

	for (s = 0; s < NSIZES; s++)
		run_bench("memcpy", do_memcpy,
			  (void *)dst_addr, (void *)src_addr, &sizes[s]);

	for (s = 0; s < NSIZES; s++)
		run_bench("memmove fwd", do_memmove_fwd,
			  (void *)src_addr, NULL, &sizes[s]);

	for (s = 0; s < NSIZES; s++)
		run_bench("memmove bwd", do_memmove_bwd,
			  (void *)src_addr, NULL, &sizes[s]);

	for (s = 0; s < NSIZES; s++)
		run_bench("memset", do_memset,
			  (void *)dst_addr, NULL, &sizes[s]);

	for (s = 0; s < NSIZES; s++)
		run_bench("bzero", do_bzero,
			  (void *)dst_addr, NULL, &sizes[s]);

	vm_deallocate(mach_task_self(), src_addr, MAX_BUF);
	vm_deallocate(mach_task_self(), dst_addr, MAX_BUF);
}
