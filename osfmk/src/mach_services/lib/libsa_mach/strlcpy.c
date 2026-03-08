/*
 * Copyright (c) 1998 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stddef.h>

/*
 * Copy src to string dst of size dstsize.  At most dstsize-1 characters
 * will be copied.  Always NUL terminates (unless dstsize == 0).
 * Returns strlen(src); if retval >= dstsize, truncation occurred.
 */
size_t
strlcpy(char *dst, const char *src, size_t dstsize)
{
	const char *osrc = src;

	if (dstsize != 0) {
		while (--dstsize != 0) {
			if ((*dst++ = *src++) == '\0')
				return (src - osrc - 1);
		}
		*dst = '\0';
	}
	while (*src++)
		;
	return (src - osrc - 1);
}
