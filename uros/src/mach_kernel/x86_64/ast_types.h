/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/ast_types.h> for x86-64 (#450).
 *
 * kern/ast.h asks the machine for the type of an AST check.  An int is what
 * the check is: a small count, not an address.
 */

#ifndef _X86_64_AST_TYPES_H_
#define _X86_64_AST_TYPES_H_

typedef int	ast_check_t;

#endif /* _X86_64_AST_TYPES_H_ */
