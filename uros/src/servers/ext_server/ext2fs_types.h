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

#ifndef _EXT2FS_TYPES_H_
#define _EXT2FS_TYPES_H_

/*
 * The version, which this server did not have.
 *
 * exec_server has announced 0.4.0 and proc_server 0.5.0 from their own
 * _types.h for as long as they have existed; this one printed
 * "=== ext2 filesystem server ===" and no number at all.  So there was
 * nothing to bump, and asking what to bump is what found that out.
 *
 * ⚠️ 0.5.0 and not 0.1.0, which is what the project's rule says a NEW
 * component starts at.  This is its first declared version but it is not new:
 * the boot exercises mount, vnode sharing, file clone, the dirty list, batch
 * RPC, the negative dentry cache and FLIPC, and twenty-five checks across
 * seven areas pass.  A 0.1.0 sitting beside proc_server's 0.5.0 would tell
 * the next reader something false about which of the two to trust.
 *
 * Bump rules are the project's (BSD-flavoured semver): MAJOR for an ABI break
 * or an incompatible change of meaning, MINOR for a backward-compatible
 * feature, PATCH for a fix with no new interface.  The -v flag that arrives
 * with this number is a MINOR-shaped change, and it is inside 0.5.0 rather
 * than after it because there was no earlier number for it to follow.
 *
 * ⚠️ The MIG subsystem id is NOT this: it is the wire identifier and stays
 * fixed for the life of the server.
 */
#define EXT2_SERVER_VERSION_MAJOR	0
#define EXT2_SERVER_VERSION_MINOR	5
#define EXT2_SERVER_VERSION_PATCH	0
#define EXT2_SERVER_VERSION_STRING	"0.5.0"

typedef char ext2_path_t[1024];

#endif /* _EXT2FS_TYPES_H_ */
