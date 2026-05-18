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
 * SSE2-optimized memset and bzero for i386.
 *
 * Strategy:
 *   n < 16    — scalar byte/dword stores
 *   16..127   — unaligned movdqu stores
 *   >= 128    — align dst to 16 bytes, then 64-byte unrolled movdqa loop
 */

#include <i386/asm.h>

/* ===================================================================
 * memset(dst, c, n) -> dst
 * =================================================================== */
.globl memset; .align 2; memset:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi

	movl	B_ARG0, %edi		/* dst */
	movzbl	B_ARG1, %eax		/* c (byte) */
	movl	B_ARG2, %ecx		/* n */

	/* Replicate byte to all 4 bytes of eax */
	movl	%eax, %edx
	shll	$8, %edx
	orl	%edx, %eax		/* 0x00cc -> 0xcccc */
	movl	%eax, %edx
	shll	$16, %edx
	orl	%edx, %eax		/* 0xcccc -> 0xcccccccc */

	/* Splat 32-bit pattern into xmm0 */
	movd	%eax, %xmm0
	pshufd	$0, %xmm0, %xmm0	/* xmm0 = [c,c,c,c,c,c,c,c,...] */

	jmp	.Lms_dispatch

/* ===================================================================
 * bzero(dst, n) — memset(dst, 0, n) without return value
 * =================================================================== */
.globl bzero; .align 2; bzero:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi

	movl	B_ARG0, %edi		/* dst */
	movl	B_ARG1, %ecx		/* n */
	xorl	%eax, %eax		/* c = 0 */

	pxor	%xmm0, %xmm0		/* xmm0 = all zeros */

	/* Fall through to common dispatch */

.Lms_dispatch:
	cmpl	$16, %ecx
	jb	.Lms_small
	cmpl	$128, %ecx
	jb	.Lms_medium

	/* --- Large fill (>= 128 bytes): align dst, then 64B loop --- */

	movl	%edi, %edx
	negl	%edx
	andl	$15, %edx		/* bytes to align dst to 16 */
	jz	.Lms_aligned

	movdqu	%xmm0, (%edi)
	addl	%edx, %edi
	subl	%edx, %ecx

.Lms_aligned:
	movl	%ecx, %edx
	shrl	$6, %edx		/* iterations (64B each) */
	andl	$63, %ecx		/* remaining bytes */

	.align	16
.Lms_loop64:
	movdqa	%xmm0,   (%edi)
	movdqa	%xmm0, 16(%edi)
	movdqa	%xmm0, 32(%edi)
	movdqa	%xmm0, 48(%edi)
	addl	$64, %edi
	decl	%edx
	jnz	.Lms_loop64

	testl	%ecx, %ecx
	jz	.Lms_done
	cmpl	$16, %ecx
	jb	.Lms_small

	/* --- Medium fill (16-127 bytes) --- */
.Lms_medium:
	cmpl	$32, %ecx
	jb	.Lms_med16

	cmpl	$64, %ecx
	jb	.Lms_med32

	/* 64-127 bytes */
	movdqu	%xmm0,   (%edi)
	movdqu	%xmm0, 16(%edi)
	movdqu	%xmm0, 32(%edi)
	movdqu	%xmm0, 48(%edi)
	movdqu	%xmm0, -64(%edi,%ecx)
	movdqu	%xmm0, -48(%edi,%ecx)
	movdqu	%xmm0, -32(%edi,%ecx)
	movdqu	%xmm0, -16(%edi,%ecx)
	jmp	.Lms_done

.Lms_med32:
	/* 32-63 bytes */
	movdqu	%xmm0,   (%edi)
	movdqu	%xmm0, 16(%edi)
	movdqu	%xmm0, -32(%edi,%ecx)
	movdqu	%xmm0, -16(%edi,%ecx)
	jmp	.Lms_done

.Lms_med16:
	/* 16-31 bytes */
	movdqu	%xmm0, (%edi)
	movdqu	%xmm0, -16(%edi,%ecx)
	jmp	.Lms_done

	/* --- Small fill (0-15 bytes) --- */
.Lms_small:
	testl	%ecx, %ecx
	jz	.Lms_done

	cmpl	$4, %ecx
	jb	.Lms_bytes

	cmpl	$8, %ecx
	jb	.Lms_s4

	/* 8-15 bytes */
	movl	%eax,  (%edi)
	movl	%eax, 4(%edi)
	movl	%eax, -8(%edi,%ecx)
	movl	%eax, -4(%edi,%ecx)
	jmp	.Lms_done

.Lms_s4:
	/* 4-7 bytes */
	movl	%eax, (%edi)
	movl	%eax, -4(%edi,%ecx)
	jmp	.Lms_done

.Lms_bytes:
	/* 1-3 bytes */
	movb	%al, (%edi)
	cmpl	$1, %ecx
	je	.Lms_done
	movb	%al, -1(%edi,%ecx)
	cmpl	$2, %ecx
	je	.Lms_done
	movb	%al, 1(%edi)

.Lms_done:
	movl	B_ARG0, %eax		/* return dst */
	popl	%edi
	leave
	ret
