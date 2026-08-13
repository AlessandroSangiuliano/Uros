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

/*
 * libelf.c — implementation of the pure ELF parser declared in
 * libelf.h.  Single translation unit for v0.1; split per-table when
 * the file crosses ~1k lines or when modular linkage is needed.
 *
 * Original Uros code, see [[feedback-study-never-copy]] in the
 * project memory: written from the System V ABI specification, not
 * derived from FreeBSD / NetBSD / Linux ELF code.
 *
 * Strategy:
 *   - Validate at elf_open(): cache header pointers and sizes after
 *     bounds-checking every offset against the buffer length.
 *   - Read accessors decode straight from the buffer; no copying.
 *     The cost is one bounds check + a structure-of-arrays load.
 *   - Lazy caches for symbol and dynamic tables: filled on the first
 *     query that needs them, never freed (lifetime == elf_image_t).
 */

#include "libelf.h"

#include <mach/elf.h>           /* Elf32_Ehdr / Phdr / Shdr / Sym / Dyn / Rel */
#include <string.h>             /* memcmp, strcmp */

/* ------------------------------------------------------------------ */
/*  Helpers — bounds checks                                             */
/* ------------------------------------------------------------------ */

static int
range_ok(const elf_image_t *img, uint64_t offset, uint64_t length)
{
    /* offset + length must not overflow and must fit inside the buffer. */
    if (offset > img->_len)
        return 0;
    if (length > img->_len - offset)
        return 0;
    return 1;
}

/* Accessors below assume the caller already validated.  Centralising
 * the bounds check at elf_open() time keeps the hot accessors lean. */

static const Elf32_Ehdr *
ehdr_of(const elf_image_t *img)
{
    return (const Elf32_Ehdr *)img->_ehdr;
}

static const Elf32_Phdr *
phdr_at(const elf_image_t *img, int idx)
{
    const Elf32_Phdr *base = (const Elf32_Phdr *)img->_phdr;
    return &base[idx];
}

static const Elf32_Shdr *
shdr_at(const elf_image_t *img, int idx)
{
    const Elf32_Shdr *base = (const Elf32_Shdr *)img->_shdr;
    return &base[idx];
}

/* ------------------------------------------------------------------ */
/*  elf_open — validate, populate elf_image_t                           */
/* ------------------------------------------------------------------ */

/*
 * The ELF64 half of elf_open (#422).
 *
 * Written as its own function rather than as branches inside the one below,
 * because almost every line of that one names a type: Elf32_Ehdr for the size
 * check, Elf32_Phdr for the table extent, Elf32_Shdr for the sections.  Two
 * bodies that each say one thing are shorter than one body that says which
 * thing at every step, and neither can quietly read the other's layout.
 *
 * ⚠️ It is the PROGRAM headers this handles, and deliberately not the
 * sections.  Loading an image needs PT_LOAD and nothing else, and that is
 * what the bootstrap server and exec_server ask for.  Sections, symbols and
 * the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn, which
 * <mach/elf.h> does not define yet -- so _shnum stays zero and the accessors
 * refuse by class instead of reporting an image with no sections, which is a
 * different and false statement.
 */
static int
elf_open64(elf_image_t *img)
{
    const Elf64_Ehdr *e;
    uint64_t phdr_off, phdr_span;

    if (img->_len < sizeof(Elf64_Ehdr))
        return ELF_ERR_TRUNCATED;

    e = (const Elf64_Ehdr *)(const void *)img->_buf;
    img->_ehdr = e;

    if (e->e_machine != EM_X86_64)
        return ELF_ERR_BAD_MACHINE;

    if (e->e_ehsize != sizeof(Elf64_Ehdr))
        return ELF_ERR_BAD_VERSION;
    if (e->e_phnum != 0 && e->e_phentsize != sizeof(Elf64_Phdr))
        return ELF_ERR_BAD_VERSION;

    if (e->e_phnum > 0) {
        phdr_off  = e->e_phoff;
        phdr_span = (uint64_t)e->e_phnum * sizeof(Elf64_Phdr);
        if (!range_ok(img, phdr_off, phdr_span))
            return ELF_ERR_TRUNCATED;
        img->_phdr  = img->_buf + phdr_off;
        img->_phnum = (int)e->e_phnum;
    }

    return ELF_OK;
}


int
elf_open(const void *buf, size_t len, elf_image_t *img)
{
    const Elf32_Ehdr *e;
    uint64_t phdr_off, phdr_end;
    uint64_t shdr_off, shdr_end;

    if (!buf || !img)
        return ELF_ERR_INVAL;

    /* Zero the state: every accessor checks whether tables exist by
     * looking at counts / pointers, so a clean zero gives sensible
     * "not present" semantics for unset fields. */
    memset(img, 0, sizeof(*img));
    img->_buf = (const uint8_t *)buf;
    img->_len = len;

    /* Header must fit. */
    if (len < sizeof(Elf32_Ehdr))
        return ELF_ERR_TRUNCATED;

    e = (const Elf32_Ehdr *)buf;
    img->_ehdr = e;

    /* Magic. */
    if (e->e_ident[EI_MAG0] != ELFMAG0 ||
        e->e_ident[EI_MAG1] != ELFMAG1 ||
        e->e_ident[EI_MAG2] != ELFMAG2 ||
        e->e_ident[EI_MAG3] != ELFMAG3)
        return ELF_ERR_BAD_MAGIC;

    /* Both classes now (#422).  e_ident is the one part of the header
     * that is identical in ELF32 and ELF64 -- sixteen bytes, same
     * layout -- which is what makes it safe to read the class through
     * an Elf32_Ehdr pointer before knowing which one this is. */
    if (e->e_ident[EI_CLASS] != ELFCLASS32 &&
        e->e_ident[EI_CLASS] != ELFCLASS64)
        return ELF_ERR_BAD_CLASS;
    if (e->e_ident[EI_DATA] != ELFDATA2LSB)
        return ELF_ERR_BAD_DATA;
    if (e->e_ident[EI_VERSION] != EV_CURRENT)
        return ELF_ERR_BAD_VERSION;
    img->_is_64 = (e->e_ident[EI_CLASS] == ELFCLASS64);

    if (img->_is_64)
        return elf_open64(img);

    /* Architecture allowlist — i386 for this class.  Reject anything
     * else loudly so we notice when somebody hands us an aarch64
     * binary by accident. */
    if (e->e_machine != EM_386)
        return ELF_ERR_BAD_MACHINE;

    /* Sanity check the header sizes the file declares match what we
     * are compiled against.  Catches files written by future-format
     * toolchains that we cannot decode safely. */
    if (e->e_ehsize != sizeof(Elf32_Ehdr))
        return ELF_ERR_BAD_VERSION;
    if (e->e_phnum != 0 && e->e_phentsize != sizeof(Elf32_Phdr))
        return ELF_ERR_BAD_VERSION;
    if (e->e_shnum != 0 && e->e_shentsize != sizeof(Elf32_Shdr))
        return ELF_ERR_BAD_VERSION;

    /* Program headers — bounds check the table extent. */
    if (e->e_phnum > 0) {
        phdr_off = e->e_phoff;
        phdr_end = (uint64_t)e->e_phnum * sizeof(Elf32_Phdr);
        if (!range_ok(img, phdr_off, phdr_end))
            return ELF_ERR_TRUNCATED;
        img->_phdr  = img->_buf + phdr_off;
        img->_phnum = (int)e->e_phnum;
    }

    /* Section headers + shstrtab.  Both optional; some ELF objects
     * (stripped or core dumps) ship without them. */
    if (e->e_shnum > 0) {
        shdr_off = e->e_shoff;
        shdr_end = (uint64_t)e->e_shnum * sizeof(Elf32_Shdr);
        if (!range_ok(img, shdr_off, shdr_end))
            return ELF_ERR_TRUNCATED;
        img->_shdr  = img->_buf + shdr_off;
        img->_shnum = (int)e->e_shnum;

        if (e->e_shstrndx != SHN_UNDEF && e->e_shstrndx < e->e_shnum) {
            const Elf32_Shdr *s = shdr_at(img, e->e_shstrndx);
            if (range_ok(img, s->sh_offset, s->sh_size)) {
                img->_shstrtab    = (const char *)(img->_buf + s->sh_offset);
                img->_shstrtab_sz = s->sh_size;
            }
            /* If the shstrtab itself is out of range we silently drop
             * it — section names become NULL but the parser keeps
             * working.  The alternative (fatal error) would refuse
             * legitimate stripped binaries with broken trailing
             * data. */
        }
    }

    return ELF_OK;
}

void
elf_close(elf_image_t *img)
{
    if (img)
        memset(img, 0, sizeof(*img));
}

/* ------------------------------------------------------------------ */
/*  Header inspection                                                  */
/* ------------------------------------------------------------------ */

uint32_t
elf_machine(const elf_image_t *img)
{
    return ehdr_of(img)->e_machine;
}

uint32_t
elf_type(const elf_image_t *img)
{
    return ehdr_of(img)->e_type;
}

/*
 * ⚠️ Three of the header accessors could NOT stay as they were, and which
 * three is not guessable from the field names (#422).
 *
 * e_machine and e_type are at the same offset and the same width in both
 * classes, so reading them through an Elf32_Ehdr is correct for either.
 * e_entry is at the same offset and is FOUR bytes in one and EIGHT in the
 * other -- so the ELF32 read returns the low half of a 64-bit entry point,
 * which is right for every image we load today at 0x400000 and silently wrong
 * for the first one above four gigabytes.  And e_phoff is at a different
 * OFFSET as well as a different width, because the eight-byte e_entry ahead of
 * it moved everything along.
 *
 * That is the argument for views in one paragraph: the fields that need a
 * branch are not the ones that look like they would.
 */
uintptr_t
elf_entry(const elf_image_t *img)
{
    if (img->_is_64)
        return (uintptr_t)((const Elf64_Ehdr *)img->_ehdr)->e_entry;
    return (uintptr_t)ehdr_of(img)->e_entry;
}

int
elf_is_32bit(const elf_image_t *img)
{
    return img->_is_64 == 0;
}

int
elf_is_64bit(const elf_image_t *img)
{
    return img->_is_64 != 0;
}

uint64_t
elf_phoff(const elf_image_t *img)
{
    if (img->_is_64)
        return (uint64_t)((const Elf64_Ehdr *)img->_ehdr)->e_phoff;
    return (uint64_t)ehdr_of(img)->e_phoff;
}

uint32_t
elf_phentsize(const elf_image_t *img)
{
    if (img->_is_64)
        return (uint32_t)((const Elf64_Ehdr *)img->_ehdr)->e_phentsize;
    return (uint32_t)ehdr_of(img)->e_phentsize;
}

/* ------------------------------------------------------------------ */
/*  Program headers                                                    */
/* ------------------------------------------------------------------ */

int
elf_phdr_count(const elf_image_t *img)
{
    return img->_phnum;
}

int
elf_phdr_get(const elf_image_t *img, int idx, elf_phdr_view_t *out)
{
    const Elf32_Phdr *p;
    uint64_t off, fsz;

    if (idx < 0 || idx >= img->_phnum || !out)
        return ELF_ERR_OUT_OF_RANGE;

    /*
     * ⚠️ The two program headers hold the same eight fields and NOT in the
     * same order: p_flags is second in ELF64 and second-to-last in ELF32.
     * <mach/elf.h> says so where it defines them.  A cast from one to the
     * other would therefore read the flags out of the offset -- which is why
     * this fills a view instead of handing the caller a struct, and why the
     * view has been sixty-four bits wide since before there was anything to
     * put in it.
     */
    if (img->_is_64) {
        const Elf64_Phdr *q = (const Elf64_Phdr *)img->_phdr + idx;

        out->type   = q->p_type;
        out->flags  = q->p_flags;
        out->offset = q->p_offset;
        out->vaddr  = (uintptr_t)q->p_vaddr;
        out->paddr  = (uintptr_t)q->p_paddr;
        out->filesz = q->p_filesz;
        out->memsz  = q->p_memsz;
        out->align  = q->p_align;

        off = q->p_offset;
        fsz = q->p_filesz;
        out->data = range_ok(img, off, fsz) ? img->_buf + off : NULL;
        return ELF_OK;
    }

    p = phdr_at(img, idx);
    out->type   = p->p_type;
    out->flags  = p->p_flags;
    out->offset = p->p_offset;
    out->vaddr  = (uintptr_t)p->p_vaddr;
    out->paddr  = (uintptr_t)p->p_paddr;
    out->filesz = p->p_filesz;
    out->memsz  = p->p_memsz;
    out->align  = p->p_align;

    /* Convenience pointer — only set if filesz fits.  Caller can
     * check view->data != NULL to know whether streaming directly
     * from the buffer is safe. */
    if (range_ok(img, p->p_offset, p->p_filesz))
        out->data = img->_buf + p->p_offset;
    else
        out->data = NULL;

    return ELF_OK;
}

/* ------------------------------------------------------------------ */
/*  Section headers                                                    */
/* ------------------------------------------------------------------ */

int
elf_shdr_count(const elf_image_t *img)
{
    /*
     * ⚠️ Refused by CLASS, not answered with "none" (#422).  ELF64 sections,
     * symbols and the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn,
     * which <mach/elf.h> does not define yet -- program headers are all a
     * loader needs and all that was added.  Reporting zero sections would be
     * a different statement, and a false one: the image has them.
     */
    if (img->_is_64)
        return ELF_ERR_BAD_CLASS;

    return img->_shnum;
}

static const char *
shstr(const elf_image_t *img, uint32_t name_off)
{
    if (!img->_shstrtab || name_off >= img->_shstrtab_sz)
        return NULL;
    return img->_shstrtab + name_off;
}

int
elf_shdr_get(const elf_image_t *img, int idx, elf_shdr_view_t *out)
{
    /*
     * ⚠️ Refused by CLASS, not answered with "none" (#422).  ELF64 sections,
     * symbols and the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn,
     * which <mach/elf.h> does not define yet -- program headers are all a
     * loader needs and all that was added.  Reporting zero sections would be
     * a different statement, and a false one: the image has them.
     */
    if (img->_is_64)
        return ELF_ERR_BAD_CLASS;

    const Elf32_Shdr *s;

    if (idx < 0 || idx >= img->_shnum || !out)
        return ELF_ERR_OUT_OF_RANGE;

    s = shdr_at(img, idx);
    out->name    = shstr(img, s->sh_name);
    out->type    = s->sh_type;
    out->flags   = s->sh_flags;
    out->addr    = (uintptr_t)s->sh_addr;
    out->offset  = s->sh_offset;
    out->size    = s->sh_size;
    out->link    = s->sh_link;
    out->info    = s->sh_info;
    out->entsize = s->sh_entsize;

    /* SHT_NOBITS sections have a meaningful sh_size but no on-disk
     * footprint; report data == NULL for them rather than pretending
     * it points somewhere safe. */
    if (s->sh_type != SHT_NOBITS &&
        range_ok(img, s->sh_offset, s->sh_size))
        out->data = img->_buf + s->sh_offset;
    else
        out->data = NULL;

    return ELF_OK;
}

int
elf_shdr_find(const elf_image_t *img, const char *name,
              elf_shdr_view_t *out)
{
    /*
     * ⚠️ Refused by CLASS, not answered with "none" (#422).  ELF64 sections,
     * symbols and the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn,
     * which <mach/elf.h> does not define yet -- program headers are all a
     * loader needs and all that was added.  Reporting zero sections would be
     * a different statement, and a false one: the image has them.
     */
    if (img->_is_64)
        return ELF_ERR_BAD_CLASS;

    int i;

    if (!name || !out)
        return ELF_ERR_INVAL;
    if (!img->_shstrtab)
        return ELF_ERR_NOT_PRESENT;

    for (i = 0; i < img->_shnum; i++) {
        const Elf32_Shdr *s = shdr_at(img, i);
        const char *n = shstr(img, s->sh_name);
        if (n && strcmp(n, name) == 0)
            return elf_shdr_get(img, i, out);
    }
    return ELF_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/*  Symbol table — lazy cache                                           */
/* ------------------------------------------------------------------ */

static int
ensure_symtab(elf_image_t *img)
{
    int i;

    if (img->_symtab)
        return ELF_OK;

    /* Scan section headers for SHT_SYMTAB; record the matching string
     * table referenced by sh_link. */
    for (i = 0; i < img->_shnum; i++) {
        const Elf32_Shdr *s = shdr_at(img, i);
        const Elf32_Shdr *str;

        if (s->sh_type != SHT_SYMTAB)
            continue;
        if (s->sh_link == 0 || s->sh_link >= (uint32_t)img->_shnum)
            continue;
        if (!range_ok(img, s->sh_offset, s->sh_size))
            continue;
        if (s->sh_entsize != sizeof(Elf32_Sym))
            continue;

        str = shdr_at(img, s->sh_link);
        if (str->sh_type != SHT_STRTAB)
            continue;
        if (!range_ok(img, str->sh_offset, str->sh_size))
            continue;

        img->_symtab    = img->_buf + s->sh_offset;
        img->_sym_count = (int)(s->sh_size / sizeof(Elf32_Sym));
        img->_symstr    = (const char *)(img->_buf + str->sh_offset);
        img->_symstr_sz = str->sh_size;
        return ELF_OK;
    }
    return ELF_ERR_NOT_PRESENT;
}

int
elf_sym_count(const elf_image_t *img)
{
    /*
     * ⚠️ Refused by CLASS, not answered with "none" (#422).  ELF64 sections,
     * symbols and the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn,
     * which <mach/elf.h> does not define yet -- program headers are all a
     * loader needs and all that was added.  Reporting zero sections would be
     * a different statement, and a false one: the image has them.
     */
    if (img->_is_64)
        return ELF_ERR_BAD_CLASS;

    /* Const accessor — work on a non-const copy via cast.  The cache
     * is observably idempotent (same cached pointers regardless of
     * who triggered the fill), so this is a benign mutation. */
    elf_image_t *m = (elf_image_t *)img;
    if (ensure_symtab(m) != ELF_OK)
        return 0;
    return m->_sym_count;
}

static const char *
symstr(const elf_image_t *img, uint32_t name_off)
{
    if (!img->_symstr || name_off >= img->_symstr_sz)
        return NULL;
    return img->_symstr + name_off;
}

int
elf_sym_get(const elf_image_t *img, int idx, elf_sym_view_t *out)
{
    /*
     * ⚠️ Refused by CLASS, not answered with "none" (#422).  ELF64 sections,
     * symbols and the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn,
     * which <mach/elf.h> does not define yet -- program headers are all a
     * loader needs and all that was added.  Reporting zero sections would be
     * a different statement, and a false one: the image has them.
     */
    if (img->_is_64)
        return ELF_ERR_BAD_CLASS;

    elf_image_t *m = (elf_image_t *)img;
    const Elf32_Sym *t;
    const Elf32_Sym *e;
    int rc;

    if (!out)
        return ELF_ERR_INVAL;
    if ((rc = ensure_symtab(m)) != ELF_OK)
        return rc;
    if (idx < 0 || idx >= m->_sym_count)
        return ELF_ERR_OUT_OF_RANGE;

    t = (const Elf32_Sym *)m->_symtab;
    e = &t[idx];
    out->name  = symstr(m, e->st_name);
    out->value = (uintptr_t)e->st_value;
    out->size  = e->st_size;
    out->bind  = (uint8_t)ELF32_ST_BIND(e->st_info);
    out->type  = (uint8_t)ELF32_ST_TYPE(e->st_info);
    out->shndx = e->st_shndx;
    return ELF_OK;
}

int
elf_sym_lookup(const elf_image_t *img, const char *name,
               elf_sym_view_t *out)
{
    /*
     * ⚠️ Refused by CLASS, not answered with "none" (#422).  ELF64 sections,
     * symbols and the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn,
     * which <mach/elf.h> does not define yet -- program headers are all a
     * loader needs and all that was added.  Reporting zero sections would be
     * a different statement, and a false one: the image has them.
     */
    if (img->_is_64)
        return ELF_ERR_BAD_CLASS;

    elf_image_t *m = (elf_image_t *)img;
    const Elf32_Sym *t;
    int rc, i;

    if (!name || !out)
        return ELF_ERR_INVAL;
    if ((rc = ensure_symtab(m)) != ELF_OK)
        return rc;

    /* Linear scan.  A real dynamic linker would build a hash table;
     * libelf's primary consumers (bootstrap, exec_server) do at most
     * a handful of lookups per image, so the n^0 cost wins on
     * complexity. */
    t = (const Elf32_Sym *)m->_symtab;
    for (i = 0; i < m->_sym_count; i++) {
        const char *n = symstr(m, t[i].st_name);
        if (n && strcmp(n, name) == 0) {
            return elf_sym_get(m, i, out);
        }
    }
    return ELF_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/*  Dynamic table — lazy cache                                          */
/* ------------------------------------------------------------------ */

static int
ensure_dyntab(elf_image_t *img)
{
    int i;

    if (img->_dyntab)
        return ELF_OK;

    /* Find the PT_DYNAMIC segment first — that's the canonical
     * pointer at runtime.  Some objects also publish .dynamic as a
     * section, but PT_DYNAMIC is what the loader uses. */
    for (i = 0; i < img->_phnum; i++) {
        const Elf32_Phdr *p = phdr_at(img, i);
        if (p->p_type != PT_DYNAMIC)
            continue;
        if (!range_ok(img, p->p_offset, p->p_filesz))
            continue;

        img->_dyntab    = img->_buf + p->p_offset;
        img->_dyn_count = (int)(p->p_filesz / sizeof(Elf32_Dyn));
        return ELF_OK;
    }
    return ELF_ERR_NOT_PRESENT;
}

int
elf_dyn_count(const elf_image_t *img)
{
    /*
     * ⚠️ Refused by CLASS, not answered with "none" (#422).  ELF64 sections,
     * symbols and the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn,
     * which <mach/elf.h> does not define yet -- program headers are all a
     * loader needs and all that was added.  Reporting zero sections would be
     * a different statement, and a false one: the image has them.
     */
    if (img->_is_64)
        return ELF_ERR_BAD_CLASS;

    elf_image_t *m = (elf_image_t *)img;
    if (ensure_dyntab(m) != ELF_OK)
        return 0;
    return m->_dyn_count;
}

int
elf_dyn_get(const elf_image_t *img, int idx, elf_dyn_view_t *out)
{
    /*
     * ⚠️ Refused by CLASS, not answered with "none" (#422).  ELF64 sections,
     * symbols and the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn,
     * which <mach/elf.h> does not define yet -- program headers are all a
     * loader needs and all that was added.  Reporting zero sections would be
     * a different statement, and a false one: the image has them.
     */
    if (img->_is_64)
        return ELF_ERR_BAD_CLASS;

    elf_image_t *m = (elf_image_t *)img;
    const Elf32_Dyn *t;
    int rc;

    if (!out)
        return ELF_ERR_INVAL;
    if ((rc = ensure_dyntab(m)) != ELF_OK)
        return rc;
    if (idx < 0 || idx >= m->_dyn_count)
        return ELF_ERR_OUT_OF_RANGE;

    t = (const Elf32_Dyn *)m->_dyntab;
    out->tag   = (int64_t)t[idx].d_tag;
    out->value = (uint64_t)t[idx].d_un.d_val;
    return ELF_OK;
}

int
elf_dyn_find(const elf_image_t *img, int64_t tag, uint64_t *value)
{
    /*
     * ⚠️ Refused by CLASS, not answered with "none" (#422).  ELF64 sections,
     * symbols and the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn,
     * which <mach/elf.h> does not define yet -- program headers are all a
     * loader needs and all that was added.  Reporting zero sections would be
     * a different statement, and a false one: the image has them.
     */
    if (img->_is_64)
        return ELF_ERR_BAD_CLASS;

    elf_image_t *m = (elf_image_t *)img;
    const Elf32_Dyn *t;
    int rc, i;

    if (!value)
        return ELF_ERR_INVAL;
    if ((rc = ensure_dyntab(m)) != ELF_OK)
        return rc;

    t = (const Elf32_Dyn *)m->_dyntab;
    for (i = 0; i < m->_dyn_count; i++) {
        if ((int64_t)t[i].d_tag == tag) {
            *value = (uint64_t)t[i].d_un.d_val;
            return ELF_OK;
        }
        if (t[i].d_tag == DT_NULL)
            break;     /* end-of-table sentinel */
    }
    return ELF_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/*  Convenience: PT_INTERP                                              */
/* ------------------------------------------------------------------ */

const char *
elf_interp(const elf_image_t *img)
{
    /*
     * ⚠️ Refused by CLASS, not answered with "none" (#422).  ELF64 sections,
     * symbols and the dynamic table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn,
     * which <mach/elf.h> does not define yet -- program headers are all a
     * loader needs and all that was added.  Reporting zero sections would be
     * a different statement, and a false one: the image has them.
     */
    if (img->_is_64)
        return 0;

    int i;

    for (i = 0; i < img->_phnum; i++) {
        const Elf32_Phdr *p = phdr_at(img, i);
        if (p->p_type != PT_INTERP)
            continue;
        if (!range_ok(img, p->p_offset, p->p_filesz))
            return NULL;
        if (p->p_filesz == 0)
            return NULL;
        /* PT_INTERP must contain a NUL-terminated string. */
        if (img->_buf[p->p_offset + p->p_filesz - 1] != '\0')
            return NULL;
        return (const char *)(img->_buf + p->p_offset);
    }
    return NULL;
}
