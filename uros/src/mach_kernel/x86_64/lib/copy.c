/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Moving data across the user boundary (#453).
 *
 * These are the only routines in the kernel that touch an address a task
 * chose.  The address may be unmapped, may have been unmapped since anyone
 * checked, or may never have been the task's to name -- and none of those is
 * a kernel bug.  So the copy has to be able to fail, and failing has to mean
 * an error return rather than a halt.
 *
 * ── Two things stop a bad address, and they are different ─────────────
 *
 * The RANGE CHECK below rejects any address that is not in the user half.
 * That is not about faults: it is about a kernel address arriving in a
 * user-address argument, which the page tables would happily satisfy.  A
 * copyin() that read from the kernel's own memory because a task passed
 * 0xffff800000000000 would answer success with the kernel's secrets in it.
 *
 * The FAULT RECOVERY handles what is left: an address that is in the user
 * half and is not mapped.  The instruction is annotated with EX_TABLE() so
 * the page fault handler resumes at the error path instead of reporting a
 * kernel fault (see <trap/extable.h>).
 *
 * Both are needed and neither covers the other.
 *
 * ⚠️ There is no SMAP handling here yet.  Supervisor Mode Access Prevention
 * makes a kernel touch of a user page fault unless the access is bracketed
 * by STAC/CLAC, and this kernel does not enable it -- so these work.  The
 * moment CR4.SMAP is set, every routine in this file faults on its first
 * byte and the recovery below turns that into "copy failed" for every copy
 * in the system, which would look like user memory being unreadable rather
 * than like a missing STAC.  Whoever turns SMAP on turns it on here first.
 */

#include <stdint.h>

#include <kern/misc_protos.h>
#include <mach/machine/vm_param.h>
#include <trap/extable.h>

/*
 * Is this whole range in the half a task may name?
 *
 * The end is computed as a sum, so the overflow has to be caught: a length
 * that wraps the address space would otherwise describe a range that starts
 * legitimately and ends anywhere.
 */
static inline int
user_range_ok(const void *addr, vm_size_t len)
{
	uint64_t	start = (uint64_t) addr;
	uint64_t	end   = start + len;

	if (len == 0)
		return 1;

	if (end < start)			/* wrapped */
		return 0;

	return start < VM_MAX_ADDRESS && end <= VM_MAX_ADDRESS;
}

/*
 * The copy itself, in one place, because copyin and copyout differ only in
 * which side the range check applies to.
 *
 * `rep movsb' for the same reason memcpy uses it -- ERMSB moves cache lines
 * where a byte loop moves bytes -- and because a single instruction is a
 * single entry in the recovery table.  A hand-unrolled loop would need an
 * entry per access and a way to work out how much had been copied when one
 * of them faulted.
 *
 * ⚠️ The recovery answers TRUE (failed) without saying how many bytes made
 * it.  Every caller here treats a partial copy as a failed one, and the
 * interface has nowhere to report a count -- so reporting one would be
 * inventing a contract the callers do not have.
 */
static inline boolean_t
copy_with_recovery(const void *from, void *to, vm_size_t len)
{
	boolean_t	failed = FALSE;

	__asm__ volatile(
		"1:\n\t"
		"rep movsb\n\t"
		"2:\n"
		EX_TABLE(1, 2)
		: "+D"(to), "+S"(from), "+c"(len), "+r"(failed)
		: : "memory");

	/*
	 * A fault resumes at 2 with the count register holding what was left,
	 * so anything remaining means the copy did not finish.
	 */
	return len != 0 ? TRUE : failed;
}

boolean_t
copyin(const char *user_addr, char *kernel_addr, vm_size_t nbytes)
{
	if (!user_range_ok(user_addr, nbytes))
		return TRUE;

	return copy_with_recovery(user_addr, kernel_addr, nbytes);
}

boolean_t
copyout(const char *kernel_addr, char *user_addr, vm_size_t nbytes)
{
	if (!user_range_ok(user_addr, nbytes))
		return TRUE;

	return copy_with_recovery(kernel_addr, user_addr, nbytes);
}

/*
 * The message forms.
 *
 * i386 has separate assembly for these because its message copies were worth
 * a specialised loop.  Here they are the same operation with a narrower
 * length type, and writing them as two more `rep movsb' sequences would be
 * two more recovery entries describing identical instructions.  If a
 * measurement ever shows message copies wanting something different -- an
 * alignment assumption, a non-temporal store for large ones -- this is where
 * it goes, and the separation will have been earned rather than inherited.
 */
boolean_t
copyinmsg(const char *user_addr, char *kernel_addr, mach_msg_size_t nbytes)
{
	return copyin(user_addr, kernel_addr, (vm_size_t) nbytes);
}

boolean_t
copyoutmsg(const char *kernel_addr, char *user_addr, mach_msg_size_t nbytes)
{
	return copyout(kernel_addr, user_addr, (vm_size_t) nbytes);
}

/*
 * A NUL-terminated string from user space, at most `max' bytes including the
 * terminator, with the length actually taken reported through `actual'.
 *
 * Byte at a time, and not because a faster form is unavailable: the copy has
 * to stop at the first NUL, and it may not read a byte past it.  A block
 * move would read ahead into a page the task does not have, and turn a
 * successful copy of a short string into a fault -- correct only when the
 * string happens not to sit near the end of a mapping, which is exactly the
 * case that would survive testing and fail in service.
 */
boolean_t
copyinstr(const char *user_addr, char *kernel_addr, vm_size_t max,
	  vm_size_t *actual)
{
	vm_size_t	i;

	if (max == 0)
		return TRUE;

	for (i = 0; i < max; i++) {
		char	c;

		if (!user_range_ok(user_addr + i, 1))
			return TRUE;

		if (copy_with_recovery(user_addr + i, &c, 1))
			return TRUE;

		kernel_addr[i] = c;

		if (c == '\0') {
			*actual = i + 1;
			return FALSE;
		}
	}

	/*
	 * Ran out of room before the string ended.  The bytes taken are
	 * reported so a caller that wants a truncated result can have one,
	 * and the answer is still failure: the string was not copied.
	 */
	*actual = max;
	return TRUE;
}
