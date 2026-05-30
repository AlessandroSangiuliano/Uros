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
 * bootstrap/elf.c — ELF format adapter for the bootstrap server.
 *
 * Reimplemented as a thin shim over libelf (#227) so the parser
 * lives in one place across the tree.  Bootstrap-specific bits
 * (file streaming, multi-segment load_segment population, kernel
 * symbol table push) stay here; raw header / phdr / shdr / symtab
 * decoding is delegated to libelf.
 *
 * Replaces the previous OSF/MkLinux implementation; both files
 * share a permissive license, no third-party derivation.
 */

#include "bootstrap.h"
#include "elf.h"

#include <ddb/nlist.h>
#include <libelf.h>

int elf_recog(struct file *, objfmt_t, void *);
int elf_load(struct file *, objfmt_t, void *);
void elf_symload(struct file *, mach_port_t, task_port_t, const char *,
                 objfmt_t);

struct objfmt_switch elf_switch = {
    "elf",
    elf_recog,
    elf_load,
    elf_symload
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Slurp the entire file into a freshly-malloc'd buffer.  Returns 0 on
 * success or the read_file errno; the caller frees `*out` regardless
 * of success (NULL-safe via free()).
 *
 * Bootstrap binaries are at most a few MiB; the wasted RAM for the
 * extra in-memory copy beats the complexity of plumbing a stream API
 * through libelf.
 */
static int
slurp_file(struct file *fp, void **out, vm_size_t *out_size)
{
    vm_size_t sz = file_size(fp);
    void *buf;
    int rc;

    *out = NULL;
    *out_size = 0;
    if (sz == 0)
        return -1;

    buf = malloc(sz);
    if (!buf)
        return -1;

    rc = read_file(fp, 0, (vm_offset_t)buf, sz);
    if (rc) {
        free(buf);
        return rc;
    }

    *out = buf;
    *out_size = sz;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Format recognition                                                 */
/* ------------------------------------------------------------------ */

int
elf_recog(struct file *fp, objfmt_t ofmt, void *hdr)
{
    /* Calling convention quirk inherited from the original loader:
     * recog receives a pointer-to-pointer (address of the buffer
     * pointer in ex_get_header).  Unwrap before handing to libelf,
     * which expects the actual buffer base. */
    elf_image_t img;
    void *real_hdr = *(void **)hdr;
    int rc;

    (void)fp; (void)ofmt;
    rc = elf_open(real_hdr, HEADER_MAX, &img);
    elf_close(&img);
    /* ELF_OK on a valid header; ELF_ERR_TRUNCATED is also "yes, looks
     * like ELF" since we only handed in HEADER_MAX bytes — but any
     * BAD_MAGIC / BAD_CLASS means the file is not for us. */
    return (rc == ELF_OK || rc == ELF_ERR_TRUNCATED);
}

/* ------------------------------------------------------------------ */
/*  Image load: walk PHDRs, populate loader_info segments              */
/* ------------------------------------------------------------------ */

int
elf_load(struct file *fp, objfmt_t ofmt, void *hdr)
{
    struct loader_info *lp = &ofmt->info;
    elf_image_t img;
    void *file_buf = NULL;
    vm_size_t file_sz = 0;
    int rc, i, n;

    (void)hdr;     /* full file is read below; HEADER_MAX is not enough */

    rc = slurp_file(fp, &file_buf, &file_sz);
    if (rc)
        return rc;

    rc = elf_open(file_buf, (size_t)file_sz, &img);
    if (rc != ELF_OK) {
        free(file_buf);
        return rc;
    }

    /*
     * ET_DYN (PIE) support: pick a fixed load bias above the kernel's
     * legacy static link base (1 MiB).  Every PT_LOAD vaddr, the entry
     * point, and PT_DYNAMIC's VA are shifted by this bias.  Relocations
     * are applied later by the loader (load.c) before vm_write.
     */
    if (elf_type(&img) == ET_DYN)
        lp->load_bias = 0x40000000;
    else
        lp->load_bias = 0;

    lp->dyn_vaddr  = 0;
    lp->dyn_filesz = 0;
    lp->entry_1    = (vm_offset_t)elf_entry(&img) + lp->load_bias;
    lp->entry_2    = 0;
    lp->num_segments = 0;

    n = elf_phdr_count(&img);
    for (i = 0; i < n; i++) {
        elf_phdr_view_t ph;
        if (elf_phdr_get(&img, i, &ph) != ELF_OK)
            continue;

        if (ph.type == PT_DYNAMIC) {
            lp->dyn_vaddr  = (vm_offset_t)ph.vaddr + lp->load_bias;
            lp->dyn_filesz = (vm_size_t)ph.filesz;
            continue;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0)
            continue;

        /* Record every PT_LOAD segment for the multi-segment loader. */
        if (lp->num_segments < MAX_LOAD_SEGMENTS) {
            struct load_segment *seg = &lp->segments[lp->num_segments++];
            seg->seg_vaddr  = (vm_offset_t)ph.vaddr + lp->load_bias;
            seg->seg_filesz = (vm_size_t)ph.filesz;
            seg->seg_memsz  = (vm_size_t)ph.memsz;
            seg->seg_offset = (vm_offset_t)ph.offset;
            seg->seg_prot   = VM_PROT_NONE;
            if (ph.flags & PF_R) seg->seg_prot |= VM_PROT_READ;
            if (ph.flags & PF_W) seg->seg_prot |= VM_PROT_WRITE;
            if (ph.flags & PF_X) seg->seg_prot |= VM_PROT_EXECUTE;
        }

        /* Legacy text/data fields, still consumed by symbol classifier. */
        if (ph.flags & PF_X) {
            lp->text_start  = trunc_page((vm_offset_t)ph.vaddr
                                         + lp->load_bias);
            lp->text_size   = (vm_size_t)ph.vaddr + lp->load_bias
                                + (vm_size_t)ph.filesz - lp->text_start;
            lp->text_offset = trunc_page((vm_offset_t)ph.offset);
        } else if (ph.flags & PF_W) {
            lp->data_start  = trunc_page((vm_offset_t)ph.vaddr
                                         + lp->load_bias);
            lp->data_size   = (vm_size_t)ph.vaddr + lp->load_bias
                                + (vm_size_t)ph.filesz - lp->data_start;
            lp->bss_size    = (vm_size_t)(ph.memsz - ph.filesz);
            lp->data_offset = trunc_page((vm_offset_t)ph.offset);
        }
    }

    /* Symbol table & string table — used later by elf_symload + the
     * legacy load_program_file path that still streams chunks via
     * read_file().  Locate via libelf's section-header view. */
    lp->sym_offset[0] = 0;
    lp->sym_size[0]   = 0;
    lp->sym_offset[1] = 0;
    lp->sym_size[1]   = 0;
    lp->str_offset    = 0;
    lp->str_size      = 0;

    {
        int snum = elf_shdr_count(&img);
        for (i = 0; i < snum; i++) {
            elf_shdr_view_t sh;
            if (elf_shdr_get(&img, i, &sh) != ELF_OK)
                continue;
            if (sh.type != SHT_SYMTAB || sh.entsize == 0)
                continue;

            lp->sym_offset[0] = (vm_offset_t)sh.offset;
            /* ELF32 sizes are 32-bit on the wire; cast both operands
             * to avoid pulling __udivdi3 from libgcc into the
             * nostdlib bootstrap binary. */
            lp->sym_size[0]   = ((vm_size_t)(uint32_t)sh.size
                                 / (vm_size_t)(uint32_t)sh.entsize)
                                * sizeof(struct nlist);

            /* sh.link points at the matching SHT_STRTAB. */
            if (sh.link != 0) {
                elf_shdr_view_t str;
                if (elf_shdr_get(&img, (int)sh.link, &str) == ELF_OK
                    && str.type == SHT_STRTAB) {
                    lp->str_offset = (vm_offset_t)str.offset;
                    lp->str_size   = (vm_size_t)str.size;
                }
            }
            break;
        }
    }

    elf_close(&img);
    free(file_buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Kernel symbol table push (DDB)                                     */
/* ------------------------------------------------------------------ */

/*
 * Convert a single ELF Sym into the nlist format DDB expects.  Returns
 * 1 if the entry should be kept in the output table, 0 if it should be
 * dropped (LOPROC/HIPROC/SECTION are not interesting to the debugger).
 */
static int
elf_to_nlist(const elf_sym_view_t *sv, struct nlist *nl,
             const struct loader_info *lp)
{
    nl->n_un.n_strx = (long)0;     /* filled by the caller after offset fixup */
    nl->n_other = (char)0;
    nl->n_desc  = (short)0;
    nl->n_value = (unsigned long)sv->value;

    switch (sv->type) {
    case STT_FILE:
        nl->n_type = N_FN;
        break;
    case STT_OBJECT:
    case STT_FUNC:
        if (sv->value >= lp->text_start
            && sv->value < lp->text_start + lp->text_size)
            nl->n_type = N_TEXT;
        else if (sv->value >= lp->data_start
                 && sv->value < lp->data_start + lp->data_size)
            nl->n_type = N_DATA;
        else if (sv->value >= lp->data_start + lp->data_size
                 && sv->value < lp->data_start + lp->data_size
                                 + lp->bss_size + sizeof(int))
            nl->n_type = N_BSS;
        else
            nl->n_type = N_ABS;
        break;
    case STT_NOTYPE:
        if (sv->shndx == SHN_UNDEF) {
            nl->n_type = N_UNDF;
            break;
        }
        /* fallthrough */
    case STT_LOPROC:
    case STT_HIPROC:
    case STT_SECTION:
        return 0;     /* drop */
    default:
        BOOTSTRAP_IO_LOCK();
        printf("elf_symload: unexpected ST_TYPE %d\n", sv->type);
        BOOTSTRAP_IO_UNLOCK();
        nl->n_type = N_ABS;
        break;
    }

    if (sv->bind == STB_GLOBAL)
        nl->n_type |= N_EXT;
    return 1;
}

void
elf_symload(struct file *fp,
            mach_port_t host_port,
            task_port_t task,
            const char *symtab_name,
            objfmt_t ofmt)
{
    struct loader_info *lp = &ofmt->info;
    void *file_buf = NULL;
    vm_size_t file_sz = 0;
    elf_image_t img;
    vm_offset_t symtab = 0;
    vm_size_t   table_size;
    struct nlist *nl;
    int n_syms, i, kept;
    kern_return_t kr;

    if (lp->sym_size[0] == 0 || lp->str_size == 0)
        return;

    if (slurp_file(fp, &file_buf, &file_sz) != 0)
        return;

    if (elf_open(file_buf, (size_t)file_sz, &img) != ELF_OK) {
        free(file_buf);
        return;
    }

    n_syms = elf_sym_count(&img);
    if (n_syms <= 0) {
        elf_close(&img);
        free(file_buf);
        return;
    }

    /*
     * Layout of the buffer pushed to host_load_symbol_table:
     *   [int symtab_size][nlist[]][int strtab_size][strings...]
     */
    table_size = sizeof(int) + (vm_size_t)n_syms * sizeof(struct nlist)
                 + lp->str_size + sizeof(int);

    kr = vm_allocate(mach_task_self(), &symtab, table_size, TRUE);
    if (kr != KERN_SUCCESS) {
        BOOTSTRAP_IO_LOCK();
        printf("[ error %d allocating space for %s symbol table ]\n",
               kr, symtab_name);
        BOOTSTRAP_IO_UNLOCK();
        elf_close(&img);
        free(file_buf);
        return;
    }

    nl = (struct nlist *)(symtab + sizeof(int));
    kept = 0;

    for (i = 0; i < n_syms; i++) {
        elf_sym_view_t sv;
        if (elf_sym_get(&img, i, &sv) != ELF_OK)
            continue;
        if (!elf_to_nlist(&sv, nl, lp))
            continue;

        /* String offset: read by DDB relative to the strings region
         * that follows the nlist array.  The +sizeof(int) skips the
         * leading symtab_size word the buffer starts with. */
        nl->n_un.n_strx = (long)(sv.name ? (sv.name - img._symstr) : 0)
                          + sizeof(int);
        nl++;
        kept++;
    }

    *(int32_t *)symtab = (int32_t)(kept * sizeof(struct nlist));
    *(int32_t *)nl = (int32_t)(lp->str_size + sizeof(int32_t));

    /* String table follows the nlist[] + the strtab-size word. */
    {
        vm_offset_t strings = (vm_offset_t)nl + sizeof(int);
        if (lp->str_offset + lp->str_size <= file_sz) {
            memcpy((void *)strings,
                   (const uint8_t *)file_buf + lp->str_offset,
                   (size_t)lp->str_size);
            host_load_symbol_table(host_port, task,
                                   (char *)symtab_name,
                                   symtab, table_size);
            /* Ignore error — kernel may not have DDB compiled in. */
        }
    }

    (void)vm_deallocate(mach_task_self(), symtab, table_size);
    elf_close(&img);
    free(file_buf);
}
