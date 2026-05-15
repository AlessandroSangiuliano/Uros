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
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#ifndef _LIBELF_H_
#define _LIBELF_H_

/*
 * libelf — pure ELF parser, original Uros code (#227).
 *
 * libelf reads a complete ELF image already loaded into memory and
 * exposes the file structure through a small set of "view" structs.
 * It does not perform any I/O, any VM mapping, any memory allocation,
 * any IPC, or any file system access — every API call returns a view
 * that points back into the caller-provided buffer.
 *
 * Lifecycle:
 *   1. Caller obtains the raw ELF bytes (e.g. via vfs_read or by being
 *      handed a multiboot module).
 *   2. Caller stack-allocates an elf_image_t and calls elf_open().
 *   3. Caller queries headers / segments / symbols / relocations.
 *   4. Caller calls elf_close() before discarding the buffer.
 *
 * The buffer lifetime must outlast the elf_image_t — every returned
 * pointer (including the strings inside elf_*_view_t) is valid only
 * while the underlying buffer is.
 */

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Library version (BSD-style major.minor.patch)                      */
/* ------------------------------------------------------------------ */

#define LIBELF_VERSION_MAJOR    0
#define LIBELF_VERSION_MINOR    1
#define LIBELF_VERSION_PATCH    0
#define LIBELF_VERSION_STRING   "0.1.0"

/* ------------------------------------------------------------------ */
/*  Return codes                                                       */
/* ------------------------------------------------------------------ */

#define ELF_OK              0
#define ELF_ERR_TRUNCATED  -1   /* buffer too small for declared structure */
#define ELF_ERR_BAD_MAGIC  -2   /* e_ident magic mismatch */
#define ELF_ERR_BAD_CLASS  -3   /* unsupported ELFCLASS (32/64) */
#define ELF_ERR_BAD_DATA   -4   /* unsupported byte order */
#define ELF_ERR_BAD_VERSION -5
#define ELF_ERR_BAD_MACHINE -6  /* e_machine not in allowlist */
#define ELF_ERR_OUT_OF_RANGE -7 /* index outside count */
#define ELF_ERR_NOT_FOUND  -8   /* lookup miss */
#define ELF_ERR_NOT_PRESENT -9  /* requested table absent (e.g. no SHT_SYMTAB) */
#define ELF_ERR_INVAL      -10  /* malformed argument */

/* ------------------------------------------------------------------ */
/*  Internal state — caller stack-allocates, fields are not stable     */
/* ------------------------------------------------------------------ */

/*
 * elf_image_t holds cached pointers + sizes derived from the raw
 * buffer at elf_open() time.  Direct field access from outside is
 * not supported; layout may change between releases.  Use the
 * accessor functions declared below.
 */
typedef struct elf_image {
    /* --- The buffer the caller handed us --- */
    const uint8_t *_buf;
    size_t         _len;

    /* --- Header pointers, validated at elf_open --- */
    const void    *_ehdr;       /* Elf32_Ehdr * (or future Elf64_Ehdr *) */
    int            _is_64;      /* 1 == ELF64; v0.1 always 0 */

    /* --- Program headers --- */
    const void    *_phdr;       /* base of PHDR table */
    int            _phnum;

    /* --- Section headers --- */
    const void    *_shdr;       /* base of SHDR table */
    int            _shnum;
    const char    *_shstrtab;   /* SHN_UNDEF if e_shstrndx == 0 */
    size_t         _shstrtab_sz;

    /* --- Lazily-cached: symbol table (SHT_SYMTAB / SHT_DYNSYM) --- */
    const void    *_symtab;
    int            _sym_count;
    const char    *_symstr;
    size_t         _symstr_sz;

    /* --- Lazily-cached: dynamic table (PT_DYNAMIC) --- */
    const void    *_dyntab;
    int            _dyn_count;
} elf_image_t;

/* ------------------------------------------------------------------ */
/*  Setup                                                              */
/* ------------------------------------------------------------------ */

/*
 * Validate magic / class / data / version / machine, cache header
 * pointers.  Returns ELF_OK or one of ELF_ERR_*.
 *
 * v0.1: only ELFCLASS32 + ELFDATA2LSB + EM_386 are accepted.  The
 * ELF64 path is reserved for the future x86_64 port.
 */
int  elf_open(const void *buf, size_t len, elf_image_t *img);

/*
 * Discard cached state.  Does NOT free the underlying buffer (the
 * caller owns it).  Calling elf_close on a zero-initialised
 * elf_image_t is a no-op.
 */
void elf_close(elf_image_t *img);

/* ------------------------------------------------------------------ */
/*  Header inspection                                                  */
/* ------------------------------------------------------------------ */

uint32_t  elf_machine(const elf_image_t *img);   /* EM_386, ... */
uint32_t  elf_type(const elf_image_t *img);      /* ET_EXEC / ET_DYN / ET_REL / ET_CORE */
uintptr_t elf_entry(const elf_image_t *img);     /* e_entry virtual address */
int       elf_is_32bit(const elf_image_t *img);
int       elf_is_64bit(const elf_image_t *img);

/* ------------------------------------------------------------------ */
/*  Program headers                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t   type;     /* PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, ... */
    uint32_t   flags;    /* PF_R | PF_W | PF_X */
    uint64_t   offset;   /* file offset of the segment */
    uintptr_t  vaddr;    /* virtual address it wants to live at */
    uintptr_t  paddr;    /* physical (rarely used) */
    uint64_t   filesz;   /* bytes from the file */
    uint64_t   memsz;    /* bytes in memory (>= filesz; tail is zero-filled) */
    uint64_t   align;
    const void *data;    /* convenience: img->_buf + offset */
} elf_phdr_view_t;

int elf_phdr_count(const elf_image_t *img);
int elf_phdr_get(const elf_image_t *img, int idx, elf_phdr_view_t *out);

/* ------------------------------------------------------------------ */
/*  Section headers                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;    /* points into shstrtab, NULL if no shstrtab */
    uint32_t   type;     /* SHT_PROGBITS, SHT_SYMTAB, SHT_DYNAMIC, ... */
    uint32_t   flags;    /* SHF_ALLOC, SHF_WRITE, SHF_EXECINSTR */
    uintptr_t  addr;     /* runtime virtual address */
    uint64_t   offset;   /* file offset */
    uint64_t   size;
    uint32_t   link;     /* link-dependent meaning per type */
    uint32_t   info;     /* info-dependent meaning per type */
    uint64_t   entsize;  /* fixed-entry size for table sections */
    const void *data;    /* img->_buf + offset */
} elf_shdr_view_t;

int elf_shdr_count(const elf_image_t *img);
int elf_shdr_get(const elf_image_t *img, int idx, elf_shdr_view_t *out);

/* Find by name (linear scan over shstrtab).  Returns ELF_OK or
 * ELF_ERR_NOT_FOUND. */
int elf_shdr_find(const elf_image_t *img, const char *name,
                  elf_shdr_view_t *out);

/* ------------------------------------------------------------------ */
/*  Symbol table                                                       */
/*                                                                     */
/*  v0.1 looks at SHT_SYMTAB only.  SHT_DYNSYM (used by libdl-style    */
/*  dynamic linkers) is reachable through elf_shdr_find + the same    */
/*  decode helpers — exposed when a consumer needs it.                */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;    /* points into the matching string table */
    uintptr_t   value;   /* virtual address (or section offset for relocatable) */
    uint64_t    size;
    uint8_t     bind;    /* STB_LOCAL / STB_GLOBAL / STB_WEAK */
    uint8_t     type;    /* STT_NOTYPE / STT_OBJECT / STT_FUNC / ... */
    uint16_t    shndx;
} elf_sym_view_t;

int elf_sym_count(const elf_image_t *img);
int elf_sym_get(const elf_image_t *img, int idx, elf_sym_view_t *out);

/* Linear lookup by name.  Returns ELF_OK + filled view, or
 * ELF_ERR_NOT_FOUND. */
int elf_sym_lookup(const elf_image_t *img, const char *name,
                   elf_sym_view_t *out);

/* ------------------------------------------------------------------ */
/*  Dynamic table (PT_DYNAMIC content)                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t  tag;        /* DT_NEEDED, DT_STRTAB, DT_SYMTAB, ... */
    uint64_t value;
} elf_dyn_view_t;

int elf_dyn_count(const elf_image_t *img);
int elf_dyn_get(const elf_image_t *img, int idx, elf_dyn_view_t *out);

/* Find first entry with the given tag.  Returns ELF_OK + value, or
 * ELF_ERR_NOT_FOUND. */
int elf_dyn_find(const elf_image_t *img, int64_t tag, uint64_t *value);

/* ------------------------------------------------------------------ */
/*  Convenience                                                        */
/* ------------------------------------------------------------------ */

/*
 * Return the PT_INTERP string (path of the dynamic linker), or NULL
 * if the image is statically linked.  Pointer is into the caller's
 * buffer — valid until elf_close().
 */
const char *elf_interp(const elf_image_t *img);

#endif /* _LIBELF_H_ */
