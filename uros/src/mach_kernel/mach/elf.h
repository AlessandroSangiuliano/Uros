/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 * 
 */
/*
 * MkLinux
 */
/*
 * OSF/1 HISTORY
 * Revision 1.1.2.7  1993/07/20  17:10:05  boot
 * 	Move HISTORY to top of file from bottom
 * 	[1993/07/20  14:40:01  boot]
 *
 * 	Machine-dependent Elf stuff.
 * 	[1993/07/20  12:43:46  boot]
 *
 * Revision 1.1.2.6  1993/06/11  13:23:20  smk
 * 	Moved from ./kernel/sys to ./osf1_server/sys
 * 	[1993/06/10  21:10:20  smk]
 * 
 * Revision 1.1.2.8  1993/05/13  17:26:24  duthie
 * 	Add DT_RELAENT
 * 	[1993/05/13  17:26:07  duthie]
 * 
 * Revision 1.1.2.7  1993/05/12  19:14:33  meissner
 * 	CR 9052 -- add dynamic link support.
 * 	[1993/05/12  19:14:12  meissner]
 * 
 * Revision 1.1.2.6  1993/05/07  19:59:13  meissner
 * 	CR 9034 -- add relocation entries.
 * 	[1993/05/07  19:02:45  meissner]
 * 
 * Revision 1.1.2.5  1993/05/07  14:00:22  meissner
 * 	CR 9033 -- add section attribute flags.
 * 	[1993/05/07  14:00:05  meissner]
 * 
 * Revision 1.1.2.4  1993/04/21  16:15:43  smk
 * 	Added special section numbers
 * 	(SHN_UNDEF etc. )
 * 	[1993/04/21  16:15:24  smk]
 * 
 * Revision 1.1.2.3  1993/04/06  20:07:03  smk
 * 	Added Elf32_Sym structure and related macros
 * 	[1993/04/06  20:06:41  smk]
 * 
 * Revision 1.1.2.2  1993/03/31  14:53:29  boot
 * 	New files for ELF
 * 	[1993/03/31  14:49:33  boot]
 * 
 */

#ifndef _SYS_ELF_H_
#define _SYS_ELF_H_

/*
 * #415: these are the widths the ELF32 format specifies, not the widths of
 * whatever the compiler calls a long.
 *
 * They were `unsigned long', which on i386 is thirty-two bits and so has been
 * right for as long as this has been the only target.  It is not a property
 * of the file format that it agrees with the host's long -- the format says
 * four bytes, and on x86-64 a long is eight.  Every structure below would
 * have grown, and the offsets used to walk a program header table would have
 * stepped past the fields they were reading: not a wrong number, a header
 * parsed out of alignment from its second field onward.
 *
 * This is what the issue means by a structure that crosses a boundary.  The
 * boundary here is the file on disk, and the other side of it is every ELF
 * object the bootstrap loads.  The static assertions below say the widths out
 * loud, because a typedef that has been accidentally right for thirty years
 * is exactly the kind that gets quietly changed back.
 */
typedef unsigned int	Elf32_Addr;
typedef unsigned int	Elf32_Off;
typedef unsigned int	Elf32_Word;

typedef unsigned short 	Elf32_Half;
typedef int		Elf32_Sword;

_Static_assert(sizeof(Elf32_Addr)  == 4, "ELF32: an address is four bytes");
_Static_assert(sizeof(Elf32_Off)   == 4, "ELF32: an offset is four bytes");
_Static_assert(sizeof(Elf32_Word)  == 4, "ELF32: a word is four bytes");
_Static_assert(sizeof(Elf32_Sword) == 4, "ELF32: a signed word is four bytes");
_Static_assert(sizeof(Elf32_Half)  == 2, "ELF32: a half is two bytes");

/* ELF Header - figure 4-3, page 4-4 */

#define EI_NIDENT 16

typedef struct {
  unsigned char		e_ident[EI_NIDENT];
  Elf32_Half		e_type;
  Elf32_Half		e_machine;
  Elf32_Word		e_version;
  Elf32_Addr		e_entry;
  Elf32_Off		e_phoff;
  Elf32_Off		e_shoff;
  Elf32_Word		e_flags;
  Elf32_Half		e_ehsize;
  Elf32_Half		e_phentsize;
  Elf32_Half		e_phnum;
  Elf32_Half		e_shentsize;
  Elf32_Half		e_shnum;
  Elf32_Half		e_shstrndx;
} Elf32_Ehdr;


/* e_version - object file version - page 4-6 */

#define EV_NONE         0
#define EV_CURRENT      1

/* e_ident[] identification indexes - figure 4-4, page 4-7 */
  
#define EI_MAG0		0
#define EI_MAG1		1
#define EI_MAG2		2
#define EI_MAG3		3
#define EI_CLASS	4
#define EI_DATA		5
#define EI_VERSION	6
#define EI_PAD		7

/* magic number - pg 4-8 */

#define ELFMAG0		0x7f
#define ELFMAG1		'E'
#define ELFMAG2		'L'
#define ELFMAG3		'F'

/* file class or capacity - page 4-8 */

#define ELFCLASSNONE	0
#define ELFCLASS32	1
#define ELFCLASS64	2

/* date encoding - page 4-9 */

#define ELFDATANONE	0
#define ELFDATA2LSB	1
#define ELFDATA2MSB	2

/* object file types - page 4-5 */

#define ET_NONE		0
#define ET_REL		1
#define ET_EXEC		2
#define ET_DYN		3
#define ET_CORE		4

#define ET_LOPROC	0xff00
#define ET_HIPROC	0xffff

/* architecture - page 4-5 */

#define EM_NONE		0
#define EM_M32		1
#define EM_SPARC	2
#define EM_386		3
#define EM_68K		4
#define EM_88K		5
#define EM_860		7
#define EM_MIPS		8
#define EM_PARISC      15

/*
 * Not in the 1990 table this file was copied from, because the architecture
 * did not exist (#422).  Sixty-two, assigned by the ABI supplement.
 */
#define EM_X86_64	62

/* version - page 4-6 */

#define EV_NONE		0
#define EV_CURRENT	1

/* special section indexes - page 4-11, figure 4-7 */

#define SHN_UNDEF       0
#define SHN_LORESERVE   0xff00
#define SHN_LOPROC      0xff00
#define SHN_HIPROC      0xff1f
#define SHN_ABS         0xfff1
#define SHN_COMMON      0xfff2
#define SHN_HIRESERVE   0xffff

/* section header - page 4-13, figure 4-8 */

typedef struct {
  Elf32_Word		sh_name;
  Elf32_Word		sh_type;
  Elf32_Word		sh_flags;
  Elf32_Addr		sh_addr;
  Elf32_Off		sh_offset;
  Elf32_Word		sh_size;
  Elf32_Word		sh_link;
  Elf32_Word		sh_info;
  Elf32_Word		sh_addralign;
  Elf32_Word		sh_entsize;
} Elf32_Shdr;

/* section types - page 4-15, figure 4-9 */

#define SHT_NULL	0
#define SHT_PROGBITS	1
#define SHT_SYMTAB	2
#define SHT_STRTAB	3
#define SHT_RELA	4
#define SHT_HASH	5
#define SHT_DYNAMIC	6
#define SHT_NOTE	7
#define SHT_NOBITS	8
#define SHT_REL		9
#define SHT_SHLIB      10
#define SHT_DYNSYM     11

#define SHT_LOPROC	0x70000000
#define SHT_HIPROC	0x7fffffff
#define SHT_LOUSER	0x80000000
#define SHT_HIUSER	0xffffffff

/* section attribute flags - page 4-18, figure 4-11 */

#define	SHF_WRITE	0x1
#define SHF_ALLOC	0x2
#define	SHF_EXECINSTR	0x4
#define SHF_MASKPROC	0xf0000000

/* symbol table - page 4-25, figure 4-15 */
typedef struct
{
    Elf32_Word    st_name;
    Elf32_Addr    st_value;
    Elf32_Word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half    st_shndx;
} Elf32_Sym;

/* symbol type and binding attributes - page 4-26 */

#define ELF32_ST_BIND(i)    ((i) >> 4)
#define ELF32_ST_TYPE(i)    ((i) & 0xf)
#define ELF32_ST_INFO(b,t)  (((b)<<4)+((t)&0xf))

/* symbol binding - page 4-26, figure 4-16 */

#define STB_LOCAL    0
#define STB_GLOBAL   1
#define STB_WEAK     2
#define STB_LOPROC  13
#define STB_HIPROC  15

/* symbol types - page 4-28, figure 4-17 */

#define STT_NOTYPE   0
#define STT_OBJECT   1
#define STT_FUNC     2
#define STT_SECTION  3
#define STT_FILE     4
#define STT_LOPROC  13
#define STT_HIPROC  15


/* relocation entries - page 4-31, figure 4-19 */

typedef struct
{
    Elf32_Addr	r_offset;
    Elf32_Word	r_info;
} Elf32_Rel;

typedef struct
{
    Elf32_Addr	r_offset;
    Elf32_Word	r_info;
    Elf32_Sword r_addend;
} Elf32_Rela;

/* Macros to split/combine relocation type and symbol page 4-32 */

#define ELF32_R_SYM(__i)	((__i)>>8)
#define ELF32_R_TYPE(__i)	((unsigned char) (__i))
#define ELF32_R_INFO(__s, __t)	(((__s)<<8) + (unsigned char) (__i))


/* program header - page 5-2, figure 5-1 */

typedef struct {
  Elf32_Word		p_type;
  Elf32_Off		p_offset;
  Elf32_Addr		p_vaddr;
  Elf32_Addr		p_paddr;
  Elf32_Word		p_filesz;
  Elf32_Word		p_memsz;
  Elf32_Word		p_flags;
  Elf32_Word		p_align;
} Elf32_Phdr;

/*
 * ── The 64-bit forms, which are not the 32-bit ones widened (#422) ────
 *
 * ⚠️ The program header REORDERS ITS FIELDS.  In ELF32 the flags are second
 * from last; in ELF64 they are second, immediately after the type.  That is
 * not a consequence of the widths and no amount of care with typedefs
 * produces it — a loader that widened Elf32_Phdr field by field would read
 * `p_flags' out of `p_offset' and get a file offset where it expected
 * PF_R|PF_X, which for the segments this kernel loads is a number that
 * happens to name no known combination and so is silently skipped.  The image
 * would then load with no text.
 *
 * The header does not reorder, so its fields are the widened ones and nothing
 * else.  Both are written out in full rather than generated from a macro: a
 * macro would hide exactly the asymmetry that matters.
 */
typedef unsigned long long	Elf64_Addr;
typedef unsigned long long	Elf64_Off;
typedef unsigned long long	Elf64_Xword;
typedef long long		Elf64_Sxword;
typedef unsigned int		Elf64_Word;
typedef int			Elf64_Sword;
typedef unsigned short		Elf64_Half;

_Static_assert(sizeof(Elf64_Addr)  == 8, "ELF64: an address is eight bytes");
_Static_assert(sizeof(Elf64_Off)   == 8, "ELF64: an offset is eight bytes");
_Static_assert(sizeof(Elf64_Xword) == 8, "ELF64: an extended word is eight bytes");
_Static_assert(sizeof(Elf64_Word)  == 4, "ELF64: a word is still four bytes");
_Static_assert(sizeof(Elf64_Half)  == 2, "ELF64: a half is still two bytes");

typedef struct {
  unsigned char		e_ident[EI_NIDENT];
  Elf64_Half		e_type;
  Elf64_Half		e_machine;
  Elf64_Word		e_version;
  Elf64_Addr		e_entry;
  Elf64_Off		e_phoff;
  Elf64_Off		e_shoff;
  Elf64_Word		e_flags;
  Elf64_Half		e_ehsize;
  Elf64_Half		e_phentsize;
  Elf64_Half		e_phnum;
  Elf64_Half		e_shentsize;
  Elf64_Half		e_shnum;
  Elf64_Half		e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  Elf64_Word		p_type;
  Elf64_Word		p_flags;	/* ⚠️ SECOND here, second-to-last in ELF32 */
  Elf64_Off		p_offset;
  Elf64_Addr		p_vaddr;
  Elf64_Addr		p_paddr;
  Elf64_Xword		p_filesz;
  Elf64_Xword		p_memsz;
  Elf64_Xword		p_align;
} Elf64_Phdr;

_Static_assert(sizeof(Elf64_Ehdr) == 64, "ELF64: the header is sixty-four bytes");
_Static_assert(sizeof(Elf64_Phdr) == 56, "ELF64: a program header is fifty-six bytes");

/*
 * And which of the two this kernel loads.
 *
 * ⚠️ One, not either.  A kernel reads a boot image into a task whose address
 * space it is building, so the class it accepts is fixed by the architecture
 * and an image of the other class is not a variant to be handled but a file
 * for a different machine.  Naming the pair here means the loader has no
 * conditionals in it and the check that refuses the other class has one place
 * to be, which is what keeps the refusal from being forgotten.
 */
#if defined(__x86_64__)
typedef Elf64_Ehdr	Elf_Ehdr;
typedef Elf64_Phdr	Elf_Phdr;
#define ELF_TARGET_CLASS	ELFCLASS64
#define ELF_TARGET_MACHINE	EM_X86_64
#define ELF_TARGET_NAME		"x86-64"
#else
typedef Elf32_Ehdr	Elf_Ehdr;
typedef Elf32_Phdr	Elf_Phdr;
#define ELF_TARGET_CLASS	ELFCLASS32
#define ELF_TARGET_MACHINE	EM_386
#define ELF_TARGET_NAME		"i386"
#endif

/* The rest of the pair -- Elf_Sym, Elf_Dyn, Elf_Rela and the relocation
 * tags -- is at the END of this file, because it names the ELF64 structures
 * and those are declared after the dynamic-array tags below. */

/* segment types - page 5-3, figure 5-2 */

#define PT_NULL		0
#define PT_LOAD		1
#define PT_DYNAMIC	2
#define PT_INTERP	3
#define PT_NOTE		4
#define PT_SHLIB	5
#define PT_PHDR		6

#define PT_LOPROC	0x70000000
#define PT_HIPROC	0x7fffffff

/* segment permissions - page 5-6 */

#define PF_X		0x1
#define PF_W		0x2
#define PF_R		0x4
#define PF_MASKPROC	0xf0000000


/* dynamic structure - page 5-15, figure 5-9 */

typedef struct {
	Elf32_Sword	d_tag;
	union {
	    Elf32_Word	d_val;
	    Elf32_Addr	d_ptr;
	} d_un;
} Elf32_Dyn;

/* Dynamic array tags - page 5-16, figure 5-10.  */

#define DT_NULL		0
#define DT_NEEDED	1
#define DT_PLTRELSZ	2
#define DT_PLTGOT	3
#define DT_HASH		4
#define DT_STRTAB	5
#define DT_SYMTAB	6
#define DT_RELA		7
#define DT_RELASZ	8
#define DT_RELAENT      9
#define DT_STRSZ	10
#define DT_SYMENT	11
#define DT_INIT		12
#define DT_FINI		13
#define DT_SONAME	14
#define DT_RPATH	15
#define DT_SYMBOLIC	16
#define DT_REL		17
#define DT_RELSZ	18
#define DT_RELENT	19
#define DT_PLTREL	20
#define DT_DEBUG	21
#define DT_TEXTREL	22
#define DT_JMPREL	23

/* Special symbol table index */

#define STN_UNDEF	0

/* i386 relocation types (System V ABI, Intel 386 Architecture Supplement) */

#define R_386_NONE	0
#define R_386_32	1
#define R_386_PC32	2
#define R_386_GOT32	3
#define R_386_PLT32	4
#define R_386_COPY	5
#define R_386_GLOB_DAT	6
#define R_386_JMP_SLOT	7
#define R_386_RELATIVE	8
#define R_386_GOTOFF	9
#define R_386_GOTPC	10

/*
 * ================================================================
 * ELF64: the tables a loader reads after the program headers (#423)
 * ================================================================
 *
 * The header above stops at Elf64_Ehdr and Elf64_Phdr, which is what a boot
 * image needs -- PT_LOAD and nothing else.  A dynamic loader needs more, and
 * libelf says so at six of its accessors: "sections, symbols and the dynamic
 * table need Elf64_Shdr, Elf64_Sym and Elf64_Dyn, which <mach/elf.h> does not
 * define yet -- so _shnum stays zero and the accessors refuse by class".
 *
 * They are defined here now because libdl loads driver modules and reads all
 * three.  ⚠️ Defining them does not implement libelf's ELF64 accessors: those
 * still refuse, and their comments are updated to say the types now exist
 * rather than that they do not.  Writing six accessors nobody calls is the
 * mistake libelf's own ELF64 was reported as done by.
 *
 * 🔑 The asymmetries, which are why these are written out rather than derived
 * from the ELF32 ones by a macro:
 *
 *   Elf64_Sym    st_name, st_info, st_other, st_shndx come FIRST -- ELF32 puts
 *                st_value and st_size before st_info.  A macro would hide it.
 *   r_info       is 64 bits wide, and the split moves: the symbol index is the
 *                top 32 bits and the type is the bottom 32, where ELF32 uses
 *                24 and 8.  ELF64_R_TYPE is NOT a cast to unsigned char.
 *   Rela         is the norm on x86-64 and REL is not used at all, which is a
 *                difference in the relocation LOOP and not only in the type:
 *                RELA replaces the slot with addend + base, REL adds to what
 *                the slot already holds (#422 in the bootstrap loader).
 */

typedef struct {
	Elf64_Word	sh_name;
	Elf64_Word	sh_type;
	Elf64_Xword	sh_flags;
	Elf64_Addr	sh_addr;
	Elf64_Off	sh_offset;
	Elf64_Xword	sh_size;
	Elf64_Word	sh_link;
	Elf64_Word	sh_info;
	Elf64_Xword	sh_addralign;
	Elf64_Xword	sh_entsize;
} Elf64_Shdr;

typedef struct {
	Elf64_Word	st_name;
	unsigned char	st_info;	/* ⚠️ third here, sixth in ELF32 */
	unsigned char	st_other;
	Elf64_Half	st_shndx;
	Elf64_Addr	st_value;
	Elf64_Xword	st_size;
} Elf64_Sym;

typedef struct {
	Elf64_Addr	r_offset;
	Elf64_Xword	r_info;
} Elf64_Rel;

typedef struct {
	Elf64_Addr	r_offset;
	Elf64_Xword	r_info;
	Elf64_Sxword	r_addend;
} Elf64_Rela;

typedef struct {
	Elf64_Sxword	d_tag;
	union {
	    Elf64_Xword	d_val;
	    Elf64_Addr	d_ptr;
	} d_un;
} Elf64_Dyn;

_Static_assert(sizeof(Elf64_Shdr) == 64, "ELF64: a section header is sixty-four bytes");
_Static_assert(sizeof(Elf64_Sym)  == 24, "ELF64: a symbol is twenty-four bytes");
_Static_assert(sizeof(Elf64_Rel)  == 16, "ELF64: a REL entry is sixteen bytes");
_Static_assert(sizeof(Elf64_Rela) == 24, "ELF64: a RELA entry is twenty-four bytes");
_Static_assert(sizeof(Elf64_Dyn)  == 16, "ELF64: a dynamic entry is sixteen bytes");

#define ELF64_ST_BIND(i)	((i) >> 4)
#define ELF64_ST_TYPE(i)	((i) & 0xf)

#define ELF64_R_SYM(__i)	((Elf64_Word) ((__i) >> 32))
#define ELF64_R_TYPE(__i)	((Elf64_Word) ((__i) & 0xffffffffULL))

/*
 * x86-64 relocations.  A separate namespace from R_386_*, not a superset:
 * R_X86_64_GLOB_DAT is 6 and R_386_GLOB_DAT is also 6, and they mean the same
 * thing -- but R_X86_64_64 is 1 where R_386_32 is 1, and the two describe
 * different widths.  Nothing may compare a type number without knowing which
 * machine produced it.
 */
#define R_X86_64_NONE		0
#define R_X86_64_64		1
#define R_X86_64_PC32		2
#define R_X86_64_GOT32		3
#define R_X86_64_PLT32		4
#define R_X86_64_COPY		5
#define R_X86_64_GLOB_DAT	6
#define R_X86_64_JUMP_SLOT	7
#define R_X86_64_RELATIVE	8
#define R_X86_64_GOTPCREL	9
#define R_X86_64_32		10
#define R_X86_64_32S		11

/*
 * The rest of the target-class pair, for a userspace loader (#423).
 *
 * 🔑 The same argument as the Elf_Ehdr/Elf_Phdr block above, one level out:
 * dlopen maps an object into the address space it is already running in, so
 * the class it can accept is fixed by the loader itself, and an object of the
 * other class is not a variant to handle but a file for a different machine.
 * That is why libdl compares elf_machine() against ELF_TARGET_MACHINE from
 * here rather than defining its own -- libelf reads both classes now, so every
 * caller that has to refuse one needs the same answer to "which am I".
 */
#if defined(__x86_64__)
typedef Elf64_Shdr	Elf_Shdr;
typedef Elf64_Sym	Elf_Sym;
typedef Elf64_Dyn	Elf_Dyn;
typedef Elf64_Rel	Elf_Rel;
typedef Elf64_Rela	Elf_Rela;
typedef Elf64_Addr	Elf_Addr;
typedef Elf64_Xword	Elf_Xword;
typedef Elf64_Word	Elf_Word;
#define ELF_R_SYM(i)	ELF64_R_SYM(i)
#define ELF_R_TYPE(i)	ELF64_R_TYPE(i)
#define ELF_ST_BIND(i)	ELF64_ST_BIND(i)
#define ELF_ST_TYPE(i)	ELF64_ST_TYPE(i)
/*
 * ⚠️ Which table the relocations come out of, and it is not a style choice:
 * x86-64 uses RELA exclusively and i386 uses REL, so DT_RELA/DT_RELASZ are the
 * tags to read on one and DT_REL/DT_RELSZ on the other.  A loop that reads the
 * wrong pair does not fail -- it finds no table and returns success, and an
 * empty relocation table is indistinguishable from a pass over nothing.
 */
#define ELF_DT_REL	DT_RELA
#define ELF_DT_RELSZ	DT_RELASZ
#define ELF_RELA_HAS_ADDEND	1
/* The entry the dynamic relocation tables actually hold on this machine.
 * ⚠️ A separate name from Elf_Rel because the two are different SIZES -- 24
 * bytes against 16 -- so a pointer walk over the wrong one lands between
 * entries rather than failing. */
typedef Elf64_Rela	Elf_Reloc;
#else
typedef Elf32_Shdr	Elf_Shdr;
typedef Elf32_Sym	Elf_Sym;
typedef Elf32_Dyn	Elf_Dyn;
typedef Elf32_Rel	Elf_Rel;
typedef Elf32_Rela	Elf_Rela;
typedef Elf32_Addr	Elf_Addr;
typedef Elf32_Word	Elf_Xword;
typedef Elf32_Word	Elf_Word;
#define ELF_R_SYM(i)	ELF32_R_SYM(i)
#define ELF_R_TYPE(i)	ELF32_R_TYPE(i)
#define ELF_ST_BIND(i)	ELF32_ST_BIND(i)
#define ELF_ST_TYPE(i)	ELF32_ST_TYPE(i)
#define ELF_DT_REL	DT_REL
#define ELF_DT_RELSZ	DT_RELSZ
#define ELF_RELA_HAS_ADDEND	0
typedef Elf32_Rel	Elf_Reloc;
#endif

/*
 * 🔑 One name per relocation ACTION, not per number.  The two namespaces are
 * separate -- R_386_JMP_SLOT is 7 and R_X86_64_JUMP_SLOT is 7, which is a
 * coincidence of numbering and not a shared meaning -- so the loop matches on
 * these and a type number never crosses machines.
 */
#if defined(__x86_64__)
#define ELF_R_NONE	R_X86_64_NONE
#define ELF_R_DIRECT	R_X86_64_64		/* S + A, pointer-wide */
#define ELF_R_PC32	R_X86_64_PC32
#define ELF_R_GLOB_DAT	R_X86_64_GLOB_DAT
#define ELF_R_JMP_SLOT	R_X86_64_JUMP_SLOT
#define ELF_R_RELATIVE	R_X86_64_RELATIVE
#else
#define ELF_R_NONE	R_386_NONE
#define ELF_R_DIRECT	R_386_32
#define ELF_R_PC32	R_386_PC32
#define ELF_R_GLOB_DAT	R_386_GLOB_DAT
#define ELF_R_JMP_SLOT	R_386_JMP_SLOT
#define ELF_R_RELATIVE	R_386_RELATIVE
#endif

/*
 *	Bootstrap doesn't need machine dependent extensions.
 */

#endif /* _SYS_ELF_H_ */
