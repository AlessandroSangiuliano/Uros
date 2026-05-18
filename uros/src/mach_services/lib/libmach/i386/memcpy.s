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
 * SSE2-optimized memcpy, memmove, bcopy for i386.
 *
 * Strategy:
 *   n < 16    — scalar GPR copies (byte/dword)
 *   16..127   — unaligned SSE2 loads/stores (movdqu), overlapping head+tail
 *   >= 128    — align dst to 16 bytes, then 64-byte unrolled SSE2 loop
 */

#include <i386/asm.h>

/* ===================================================================
 * memcpy(dst, src, n) -> dst
 *
 * Forward copy only.  Does NOT handle overlapping regions.
 * =================================================================== */
.globl memcpy; .align 2; memcpy:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%esi

	movl	B_ARG0, %edi		/* dst */
	movl	B_ARG1, %esi		/* src */
	movl	B_ARG2, %ecx		/* n   */

	cmpl	$16, %ecx
	jb	.Lsmall
	cmpl	$128, %ecx
	jb	.Lmedium

	/* --- Large copy (>= 128 bytes): align dst, then 64B SSE2 loop --- */

	/* Copy head bytes to align dst to 16 */
	movl	%edi, %edx
	negl	%edx
	andl	$15, %edx		/* edx = bytes to 16-align dst */
	jz	.Laligned

	/* Unaligned head: copy 16 bytes, advance by alignment count.
	 * The overlapping portion will be overwritten by the main loop. */
	movdqu	(%esi), %xmm0
	movdqu	%xmm0, (%edi)
	addl	%edx, %esi
	addl	%edx, %edi
	subl	%edx, %ecx

.Laligned:
	/* dst is 16-byte aligned; ecx >= 113 */
	movl	%ecx, %edx
	shrl	$6, %edx		/* edx = iterations (64B each) */
	andl	$63, %ecx		/* ecx = remaining bytes */

	.align	16
.Lloop64:
	movdqu	  (%esi), %xmm0
	movdqu	16(%esi), %xmm1
	movdqu	32(%esi), %xmm2
	movdqu	48(%esi), %xmm3
	movdqa	%xmm0,   (%edi)	/* dst aligned: use movdqa */
	movdqa	%xmm1, 16(%edi)
	movdqa	%xmm2, 32(%edi)
	movdqa	%xmm3, 48(%edi)
	addl	$64, %esi
	addl	$64, %edi
	decl	%edx
	jnz	.Lloop64

	/* Handle remainder (0-63 bytes) */
	testl	%ecx, %ecx
	jz	.Ldone
	cmpl	$16, %ecx
	jb	.Lsmall

	/* --- Medium copy (16-127 bytes): overlapping head+tail movdqu --- */
.Lmedium:
	cmpl	$32, %ecx
	jb	.Lmed16

	cmpl	$64, %ecx
	jb	.Lmed32

	/* 64-127 bytes: head 64 + tail 64 (overlapping) */
	movdqu	  (%esi), %xmm0
	movdqu	16(%esi), %xmm1
	movdqu	32(%esi), %xmm2
	movdqu	48(%esi), %xmm3
	movdqu	%xmm0,   (%edi)
	movdqu	%xmm1, 16(%edi)
	movdqu	%xmm2, 32(%edi)
	movdqu	%xmm3, 48(%edi)
	movdqu	-64(%esi,%ecx), %xmm0
	movdqu	-48(%esi,%ecx), %xmm1
	movdqu	-32(%esi,%ecx), %xmm2
	movdqu	-16(%esi,%ecx), %xmm3
	movdqu	%xmm0, -64(%edi,%ecx)
	movdqu	%xmm1, -48(%edi,%ecx)
	movdqu	%xmm2, -32(%edi,%ecx)
	movdqu	%xmm3, -16(%edi,%ecx)
	jmp	.Ldone

.Lmed32:
	/* 32-63 bytes: head 32 + tail 32 */
	movdqu	  (%esi), %xmm0
	movdqu	16(%esi), %xmm1
	movdqu	%xmm0,   (%edi)
	movdqu	%xmm1, 16(%edi)
	movdqu	-32(%esi,%ecx), %xmm0
	movdqu	-16(%esi,%ecx), %xmm1
	movdqu	%xmm0, -32(%edi,%ecx)
	movdqu	%xmm1, -16(%edi,%ecx)
	jmp	.Ldone

.Lmed16:
	/* 16-31 bytes: head 16 + tail 16 */
	movdqu	(%esi), %xmm0
	movdqu	%xmm0, (%edi)
	movdqu	-16(%esi,%ecx), %xmm0
	movdqu	%xmm0, -16(%edi,%ecx)
	jmp	.Ldone

	/* --- Small copy (0-15 bytes): scalar --- */
.Lsmall:
	testl	%ecx, %ecx
	jz	.Ldone

	cmpl	$4, %ecx
	jb	.Lbytes

	cmpl	$8, %ecx
	jb	.Lsmall4

	/* 8-15 bytes: overlapping dword pairs from head + tail */
	movl	  (%esi), %edx
	movl	%edx,     (%edi)
	movl	 4(%esi), %edx
	movl	%edx,    4(%edi)
	movl	-8(%esi,%ecx), %edx
	movl	%edx, -8(%edi,%ecx)
	movl	-4(%esi,%ecx), %edx
	movl	%edx, -4(%edi,%ecx)
	jmp	.Ldone

.Lsmall4:
	/* 4-7 bytes: overlapping dwords */
	movl	(%esi), %edx
	movl	%edx, (%edi)
	movl	-4(%esi,%ecx), %edx
	movl	%edx, -4(%edi,%ecx)
	jmp	.Ldone

.Lbytes:
	/* 1-3 bytes */
	movb	(%esi), %dl
	movb	%dl, (%edi)
	cmpl	$1, %ecx
	je	.Ldone
	movb	-1(%esi,%ecx), %dl
	movb	%dl, -1(%edi,%ecx)
	cmpl	$2, %ecx
	je	.Ldone
	movb	1(%esi), %dl
	movb	%dl, 1(%edi)

.Ldone:
	movl	B_ARG0, %eax		/* return dst */
	popl	%esi
	popl	%edi
	leave
	ret

/* ===================================================================
 * memmove(dst, src, n) -> dst
 *
 * Like memcpy but handles overlapping regions.
 * If dst <= src or no overlap: forward (tail-call memcpy).
 * If dst > src and overlap:   backward SSE2 copy.
 * =================================================================== */
.globl memmove; .align 2; memmove:
	pushl	%ebp
	movl	%esp, %ebp

	movl	B_ARG0, %eax		/* dst */
	movl	B_ARG1, %edx		/* src */
	movl	B_ARG2, %ecx		/* n   */

	/* If dst <= src, forward copy is safe */
	cmpl	%edx, %eax
	jbe	.Lmm_forward

	/* If dst >= src + n, no overlap — forward is safe */
	leal	(%edx,%ecx), %edx
	cmpl	%edx, %eax
	jae	.Lmm_forward

	/* Overlap with dst > src: must copy backward */
	leave
	jmp	.Lbackward_entry

.Lmm_forward:
	leave
	jmp	memcpy

/* --- Backward copy (dst > src, overlapping) --- */
.Lbackward_entry:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%esi

	movl	B_ARG0, %edi
	movl	B_ARG1, %esi
	movl	B_ARG2, %ecx

	/* Point to end of buffers */
	addl	%ecx, %edi
	addl	%ecx, %esi

	cmpl	$16, %ecx
	jb	.Lbk_small
	cmpl	$128, %ecx
	jb	.Lbk_medium

	/* Large backward: align dst-end to 16, then 64B loop */
	movl	%edi, %edx
	andl	$15, %edx		/* edx = tail misalignment */
	jz	.Lbk_aligned

	movdqu	-16(%esi), %xmm0
	movdqu	%xmm0, -16(%edi)
	subl	%edx, %esi
	subl	%edx, %edi
	subl	%edx, %ecx

.Lbk_aligned:
	movl	%ecx, %edx
	shrl	$6, %edx
	andl	$63, %ecx

	.align	16
.Lbk_loop64:
	subl	$64, %esi
	subl	$64, %edi
	movdqu	  (%esi), %xmm0
	movdqu	16(%esi), %xmm1
	movdqu	32(%esi), %xmm2
	movdqu	48(%esi), %xmm3
	movdqa	%xmm0,   (%edi)
	movdqa	%xmm1, 16(%edi)
	movdqa	%xmm2, 32(%edi)
	movdqa	%xmm3, 48(%edi)
	decl	%edx
	jnz	.Lbk_loop64

	testl	%ecx, %ecx
	jz	.Lbk_done

.Lbk_medium:
	cmpl	$16, %ecx
	jb	.Lbk_small

.Lbk_med_loop:
	subl	$16, %esi
	subl	$16, %edi
	movdqu	(%esi), %xmm0
	movdqu	%xmm0, (%edi)
	subl	$16, %ecx
	cmpl	$16, %ecx
	jae	.Lbk_med_loop

.Lbk_small:
	testl	%ecx, %ecx
	jz	.Lbk_done
.Lbk_byte_loop:
	decl	%esi
	decl	%edi
	movb	(%esi), %dl
	movb	%dl, (%edi)
	decl	%ecx
	jnz	.Lbk_byte_loop

.Lbk_done:
	movl	B_ARG0, %eax
	popl	%esi
	popl	%edi
	leave
	ret

/* ===================================================================
 * bcopy(src, dst, n) — same as memmove with swapped args, void return
 * =================================================================== */
.globl bcopy; .align 2; bcopy:
	pushl	%ebp
	movl	%esp, %ebp

	/* Swap src/dst on stack: bcopy(src, dst, n) → memmove(dst, src, n) */
	movl	B_ARG0, %eax
	movl	B_ARG1, %edx
	movl	%edx, B_ARG0
	movl	%eax, B_ARG1

	leave
	jmp	memmove
