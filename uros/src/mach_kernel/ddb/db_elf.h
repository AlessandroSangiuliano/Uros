/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/* ddb/db_elf.h — see db_elf.c (#211). */

#ifndef _DDB_DB_ELF_H_
#define _DDB_DB_ELF_H_

#include <stddef.h>
#include <mach/boolean.h>
#include <machine/db_machdep.h>
#include <ddb/db_sym.h>

extern boolean_t elf_db_register(const void *base, size_t size);

extern void      elf_db_init(void);
extern boolean_t elf_db_sym_init(char *, char *, char *, char *);
extern db_sym_t  elf_db_lookup(db_symtab_t *, char *);
extern db_sym_t  elf_db_search_symbol(db_symtab_t *, db_addr_t,
				      db_strategy_t, db_expr_t *);
extern boolean_t elf_db_line_at_pc(db_symtab_t *, db_sym_t, char **, int *,
				   db_expr_t);
extern void      elf_db_symbol_values(db_sym_t, char **, db_expr_t *);
extern db_sym_t  elf_db_search_by_addr(db_symtab_t *, db_addr_t, char **,
				       char **, int *, db_expr_t *, int *);
extern int       elf_db_print_completion(db_symtab_t *, char *);
extern int       elf_db_lookup_incomplete(db_symtab_t *, char *, char **,
					  int *, int *);

#endif /* _DDB_DB_ELF_H_ */
