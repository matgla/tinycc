/* This file defines standard ELF types, structures, and macros.
   Copyright (C) 1995-2012 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <http://www.gnu.org/licenses/>.  */

#ifndef _ELF_H
#define _ELF_H 1

#ifndef _WIN32
#include <inttypes.h>
#else
#ifndef __int8_t_defined
#define __int8_t_defined
typedef signed char int8_t;
typedef short int int16_t;
typedef int int32_t;
typedef long long int int64_t;
typedef unsigned char uint8_t;
typedef unsigned short int uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long int uint64_t;
#endif
#endif

/* Type for a 16-bit quantity.  */
typedef uint16_t Elf32_Half;
typedef uint16_t Elf64_Half;

/* Types for signed and unsigned 32-bit quantities.  */
typedef uint32_t Elf32_Word;
typedef int32_t Elf32_Sword;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;

/* Types for signed and unsigned 64-bit quantities.  */
typedef uint64_t Elf32_Xword;
typedef int64_t Elf32_Sxword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;

/* Type of addresses.  */
typedef uint32_t Elf32_Addr;
typedef uint64_t Elf64_Addr;

/* Type of file offsets.  */
typedef uint32_t Elf32_Off;
typedef uint64_t Elf64_Off;

/* Type for section indices, which are 16-bit quantities.  */
typedef uint16_t Elf32_Section;
typedef uint16_t Elf64_Section;

/* Type for version symbol information.  */
typedef Elf32_Half Elf32_Versym;
typedef Elf64_Half Elf64_Versym;

/* The ELF file header.  This appears at the start of every ELF file.  */

#define EI_NIDENT (16)

typedef struct {
  unsigned char e_ident[EI_NIDENT]; /* Magic number and other info */
  Elf32_Half e_type;                /* Object file type */
  Elf32_Half e_machine;             /* Architecture */
  Elf32_Word e_version;             /* Object file version */
  Elf32_Addr e_entry;               /* Entry point virtual address */
  Elf32_Off e_phoff;                /* Program header table file offset */
  Elf32_Off e_shoff;                /* Section header table file offset */
  Elf32_Word e_flags;               /* Processor-specific flags */
  Elf32_Half e_ehsize;              /* ELF header size in bytes */
  Elf32_Half e_phentsize;           /* Program header table entry size */
  Elf32_Half e_phnum;               /* Program header table entry count */
  Elf32_Half e_shentsize;           /* Section header table entry size */
  Elf32_Half e_shnum;               /* Section header table entry count */
  Elf32_Half e_shstrndx;            /* Section header string table index */
} Elf32_Ehdr;

typedef struct {
  unsigned char e_ident[EI_NIDENT]; /* Magic number and other info */
  Elf64_Half e_type;                /* Object file type */
  Elf64_Half e_machine;             /* Architecture */
  Elf64_Word e_version;             /* Object file version */
  Elf64_Addr e_entry;               /* Entry point virtual address */
  Elf64_Off e_phoff;                /* Program header table file offset */
  Elf64_Off e_shoff;                /* Section header table file offset */
  Elf64_Word e_flags;               /* Processor-specific flags */
  Elf64_Half e_ehsize;              /* ELF header size in bytes */
  Elf64_Half e_phentsize;           /* Program header table entry size */
  Elf64_Half e_phnum;               /* Program header table entry count */
  Elf64_Half e_shentsize;           /* Section header table entry size */
  Elf64_Half e_shnum;               /* Section header table entry count */
  Elf64_Half e_shstrndx;            /* Section header string table index */
} Elf64_Ehdr;

/* Fields in the e_ident array.  The EI_* macros are indices into the
   array.  The macros under each EI_* macro are the values the byte
   may have.  */

#define ELFMAG0 0x7f /* Magic number byte 0 */

#define ELFMAG1 'E' /* Magic number byte 1 */

#define ELFMAG2 'L' /* Magic number byte 2 */

#define ELFMAG3 'F' /* Magic number byte 3 */

/* Conglomeration of the identification bytes, for easy testing as a word.  */
#define ELFMAG "\177ELF"

#define EI_CLASS 4     /* File class byte index */
#define ELFCLASS32 1   /* 32-bit objects */
#define ELFCLASS64 2   /* 64-bit objects */

#define ELFDATA2LSB 1 /* 2's complement, little endian */

                     /* Value must be EV_CURRENT */

#define EI_OSABI 7                  /* OS ABI identification */
#define ELFOSABI_NONE 0             /* UNIX System V ABI */
#define ELFOSABI_FREEBSD 9          /* FreeBSD.  */
#define ELFOSABI_ARM 97         /* ARM */

/* Legal values for e_type (object file type).  */

#define ET_REL 1         /* Relocatable file */
#define ET_EXEC 2        /* Executable file */
#define ET_DYN 3         /* Shared object file */

/* Legal values for e_machine (architecture).  */

#define EM_386 3          /* Intel 80386 */

#define EM_ARM 40        /* ARM */

/* Legal values for e_version (version).  */

#define EV_CURRENT 1 /* Current version */

/* Section header.  */

typedef struct {
  Elf32_Word sh_name;      /* Section name (string tbl index) */
  Elf32_Word sh_type;      /* Section type */
  Elf32_Word sh_flags;     /* Section flags */
  Elf32_Addr sh_addr;      /* Section virtual addr at execution */
  Elf32_Off sh_offset;     /* Section file offset */
  Elf32_Word sh_size;      /* Section size in bytes */
  Elf32_Word sh_link;      /* Link to another section */
  Elf32_Word sh_info;      /* Additional section information */
  Elf32_Word sh_addralign; /* Section alignment */
  Elf32_Word sh_entsize;   /* Entry size if section holds table */
} Elf32_Shdr;

typedef struct {
  Elf64_Word sh_name;       /* Section name (string tbl index) */
  Elf64_Word sh_type;       /* Section type */
  Elf64_Xword sh_flags;     /* Section flags */
  Elf64_Addr sh_addr;       /* Section virtual addr at execution */
  Elf64_Off sh_offset;      /* Section file offset */
  Elf64_Xword sh_size;      /* Section size in bytes */
  Elf64_Word sh_link;       /* Link to another section */
  Elf64_Word sh_info;       /* Additional section information */
  Elf64_Xword sh_addralign; /* Section alignment */
  Elf64_Xword sh_entsize;   /* Entry size if section holds table */
} Elf64_Shdr;

/* Special section indices.  */

#define SHN_UNDEF 0          /* Undefined section */
#define SHN_LORESERVE 0xff00 /* Start of reserved indices */
#define SHN_ABS 0xfff1       /* Associated symbol is absolute */
#define SHN_COMMON 0xfff2    /* Associated symbol is common */

/* Legal values for sh_type (section type).  */

#define SHT_NULL 0                    /* Section header table entry unused */
#define SHT_PROGBITS 1                /* Program data */
#define SHT_SYMTAB 2                  /* Symbol table */
#define SHT_STRTAB 3                  /* String table */
#define SHT_RELA 4                    /* Relocation entries with addends */
#define SHT_HASH 5                    /* Symbol hash table */
#define SHT_DYNAMIC 6                 /* Dynamic linking information */
#define SHT_NOTE 7                    /* Notes */
#define SHT_NOBITS 8                  /* Program space with no data (bss) */
#define SHT_REL 9                     /* Relocation entries, no addends */
#define SHT_DYNSYM 11                 /* Dynamic linker symbol table */
#define SHT_INIT_ARRAY 14             /* Array of constructors */
#define SHT_FINI_ARRAY 15             /* Array of destructors */
#define SHT_PREINIT_ARRAY 16          /* Array of pre-constructors */
#define SHT_GNU_HASH 0x6ffffff6       /* GNU-style hash table.  */
#define SHT_GNU_verdef 0x6ffffffd  /* Version definition section.  */
#define SHT_GNU_verneed 0x6ffffffe /* Version needs section.  */
#define SHT_GNU_versym 0x6fffffff  /* Version symbol table.  */
#define SHT_LOPROC 0x70000000      /* Start of processor-specific */

/* Legal values for sh_flags (section flags).  */

#define SHF_WRITE (1 << 0)      /* Writable */
#define SHF_ALLOC (1 << 1)      /* Occupies memory during execution */
#define SHF_EXECINSTR (1 << 2)  /* Executable */
#define SHF_MERGE (1 << 4)      /* Might be merged */
#define SHF_STRINGS (1 << 5)    /* Contains nul-terminated strings */
#define SHF_GROUP (1 << 9)       /* Section is member of a group.  */
#define SHF_TLS (1 << 10)        /* Section hold thread-local data.  */
#define SHF_COMPRESSED (1 << 11) /* Section with compressed data. */

/* Symbol table entry.  */

typedef struct {
  Elf32_Word st_name;     /* Symbol name (string tbl index) */
  Elf32_Addr st_value;    /* Symbol value */
  Elf32_Word st_size;     /* Symbol size */
  unsigned char st_info;  /* Symbol type and binding */
  unsigned char st_other; /* Symbol visibility */
  Elf32_Section st_shndx; /* Section index */
} Elf32_Sym;

typedef struct {
  Elf64_Word st_name;     /* Symbol name (string tbl index) */
  unsigned char st_info;  /* Symbol type and binding */
  unsigned char st_other; /* Symbol visibility */
  Elf64_Section st_shndx; /* Section index */
  Elf64_Addr st_value;    /* Symbol value */
  Elf64_Xword st_size;    /* Symbol size */
} Elf64_Sym;

/* The syminfo section if available contains additional information about
   every dynamic symbol.  */

typedef struct {
  Elf32_Half si_boundto; /* Direct bindings, symbol bound to */
  Elf32_Half si_flags;   /* Per symbol flags */
} Elf32_Syminfo;

typedef struct {
  Elf64_Half si_boundto; /* Direct bindings, symbol bound to */
  Elf64_Half si_flags;   /* Per symbol flags */
} Elf64_Syminfo;

/* How to extract and insert information held in the st_info field.  */

#define ELF32_ST_BIND(val) (((unsigned char)(val)) >> 4)
#define ELF32_ST_TYPE(val) ((val) & 0xf)
#define ELF32_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))

/* Both Elf32_Sym and Elf64_Sym use the same one-byte st_info field.  */
#define ELF64_ST_BIND(val) ELF32_ST_BIND(val)
#define ELF64_ST_TYPE(val) ELF32_ST_TYPE(val)
#define ELF64_ST_INFO(bind, type) ELF32_ST_INFO((bind), (type))

/* Legal values for ST_BIND subfield of st_info (symbol binding).  */

#define STB_LOCAL 0       /* Local symbol */
#define STB_GLOBAL 1      /* Global symbol */
#define STB_WEAK 2        /* Weak symbol */

/* Legal values for ST_TYPE subfield of st_info (symbol type).  */

#define STT_NOTYPE 0     /* Symbol type is unspecified */
#define STT_OBJECT 1     /* Symbol is a data object */
#define STT_FUNC 2       /* Symbol is a code object */
#define STT_SECTION 3    /* Symbol associated with a section */
#define STT_FILE 4       /* Symbol's name is file name */
#define STT_GNU_IFUNC 10 /* Symbol is indirect code object */

/* How to extract and insert information held in the st_other field.  */

#define ELF32_ST_VISIBILITY(o) ((o) & 0x03)

/* For ELF64 the definitions are the same.  */
#define ELF64_ST_VISIBILITY(o) ELF32_ST_VISIBILITY(o)

/* Symbol visibility specification encoded in the st_other field.  */
#define STV_DEFAULT 0   /* Default symbol visibility rules */
#define STV_INTERNAL 1  /* Processor specific hidden class */
#define STV_HIDDEN 2    /* Sym unavailable in other modules */
#define STV_PROTECTED 3 /* Not preemptible, not exported */

/* Relocation table entry without addend (in section of type SHT_REL).  */

typedef struct {
  Elf32_Addr r_offset; /* Address */
  Elf32_Word r_info;   /* Relocation type and symbol index */
} Elf32_Rel;

/* The following, at least, is used on Sparc v9, MIPS, and Alpha.  */

typedef struct {
  Elf64_Addr r_offset; /* Address */
  Elf64_Xword r_info;  /* Relocation type and symbol index */
} Elf64_Rel;

/* Relocation table entry with addend (in section of type SHT_RELA).  */

typedef struct {
  Elf32_Addr r_offset;  /* Address */
  Elf32_Word r_info;    /* Relocation type and symbol index */
  Elf32_Sword r_addend; /* Addend */
} Elf32_Rela;

typedef struct {
  Elf64_Addr r_offset;   /* Address */
  Elf64_Xword r_info;    /* Relocation type and symbol index */
  Elf64_Sxword r_addend; /* Addend */
} Elf64_Rela;

/* How to extract and insert information held in the r_info field.  */

#define ELF32_R_SYM(val) ((val) >> 8)
#define ELF32_R_TYPE(val) ((val) & 0xff)
#define ELF32_R_INFO(sym, type) (((sym) << 8) + ((type) & 0xff))

#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
#define ELF64_R_INFO(sym, type) ((((Elf64_Xword)(sym)) << 32) + (type))

/* Program segment header.  */

typedef struct {
  Elf32_Word p_type;   /* Segment type */
  Elf32_Off p_offset;  /* Segment file offset */
  Elf32_Addr p_vaddr;  /* Segment virtual address */
  Elf32_Addr p_paddr;  /* Segment physical address */
  Elf32_Word p_filesz; /* Segment size in file */
  Elf32_Word p_memsz;  /* Segment size in memory */
  Elf32_Word p_flags;  /* Segment flags */
  Elf32_Word p_align;  /* Segment alignment */
} Elf32_Phdr;

typedef struct {
  Elf64_Word p_type;    /* Segment type */
  Elf64_Word p_flags;   /* Segment flags */
  Elf64_Off p_offset;   /* Segment file offset */
  Elf64_Addr p_vaddr;   /* Segment virtual address */
  Elf64_Addr p_paddr;   /* Segment physical address */
  Elf64_Xword p_filesz; /* Segment size in file */
  Elf64_Xword p_memsz;  /* Segment size in memory */
  Elf64_Xword p_align;  /* Segment alignment */
} Elf64_Phdr;

/* Legal values for p_type (segment type).  */

#define PT_NULL 0                  /* Program header table entry unused */
#define PT_LOAD 1                  /* Loadable program segment */
#define PT_DYNAMIC 2               /* Dynamic linking information */
#define PT_INTERP 3                /* Program interpreter */
#define PT_NOTE 4                  /* Auxiliary information */
#define PT_PHDR 6                  /* Entry for header table itself */
#define PT_TLS 7                   /* Thread-local storage segment */
#define PT_GNU_EH_FRAME 0x6474e550 /* GCC .eh_frame_hdr segment */
#define PT_GNU_STACK 0x6474e551    /* Indicates stack executability */
#define PT_GNU_RELRO 0x6474e552    /* Read-only after relocation */

/* Legal values for p_flags (segment flags).  */

#define PF_X (1 << 0)          /* Segment is executable */
#define PF_W (1 << 1)          /* Segment is writable */
#define PF_R (1 << 2)          /* Segment is readable */

/* Dynamic section entry.  */

typedef struct {
  Elf32_Sword d_tag; /* Dynamic entry type */
  union {
    Elf32_Word d_val; /* Integer value */
    Elf32_Addr d_ptr; /* Address value */
  } d_un;
} Elf32_Dyn;

typedef struct {
  Elf64_Sxword d_tag; /* Dynamic entry type */
  union {
    Elf64_Xword d_val; /* Integer value */
    Elf64_Addr d_ptr;  /* Address value */
  } d_un;
} Elf64_Dyn;

/* Legal values for d_tag (dynamic entry type).  */

#define DT_NULL 0              /* Marks end of dynamic section */
#define DT_NEEDED 1            /* Name of needed library */
#define DT_PLTRELSZ 2          /* Size in bytes of PLT relocs */
#define DT_PLTGOT 3            /* Processor defined value */
#define DT_HASH 4              /* Address of symbol hash table */
#define DT_STRTAB 5            /* Address of string table */
#define DT_SYMTAB 6            /* Address of symbol table */
#define DT_RELA 7              /* Address of Rela relocs */
#define DT_RELASZ 8            /* Total size of Rela relocs */
#define DT_RELAENT 9           /* Size of one Rela reloc */
#define DT_STRSZ 10            /* Size of string table */
#define DT_SYMENT 11           /* Size of one symbol table entry */
#define DT_INIT 12             /* Address of init function */
#define DT_FINI 13             /* Address of termination function */
#define DT_SONAME 14           /* Name of shared object */
#define DT_RPATH 15            /* Library search path (deprecated) */
#define DT_SYMBOLIC 16         /* Start symbol search here */
#define DT_REL 17              /* Address of Rel relocs */
#define DT_RELSZ 18            /* Total size of Rel relocs */
#define DT_RELENT 19           /* Size of one Rel reloc */
#define DT_PLTREL 20           /* Type of reloc in PLT */
#define DT_DEBUG 21            /* For debugging; unspecified */
#define DT_TEXTREL 22          /* Reloc might modify .text */
#define DT_JMPREL 23           /* Address of PLT relocs */
#define DT_INIT_ARRAY 25       /* Array with addresses of init fct */
#define DT_FINI_ARRAY 26       /* Array with addresses of fini fct */
#define DT_INIT_ARRAYSZ 27     /* Size in bytes of DT_INIT_ARRAY */
#define DT_FINI_ARRAYSZ 28     /* Size in bytes of DT_FINI_ARRAY */
#define DT_RUNPATH 29          /* Library search path */
#define DT_FLAGS 30            /* Flags for the object being loaded */
#define DT_PREINIT_ARRAY 32    /* Array with addresses of preinit fct*/
#define DT_PREINIT_ARRAYSZ 33  /* size in bytes of DT_PREINIT_ARRAY */

/* DT_* entries which fall between DT_VALRNGHI & DT_VALRNGLO use the
   Dyn.d_un.d_val field of the Elf*_Dyn structure.  This follows Sun's
   approach.  */
#define DT_VALRNGLO 0x6ffffd00
#define DT_FEATURE_1 0x6ffffdfc /* Feature selection (DTF_*).  */
#define DT_POSFLAG_1                                                           \
  0x6ffffdfd                   /* Flags for DT_* entries, effecting            \
                                  the following DT_* entry.  */
#define DT_VALRNGHI 0x6ffffdff

/* DT_* entries which fall between DT_ADDRRNGHI & DT_ADDRRNGLO use the
   Dyn.d_un.d_ptr field of the Elf*_Dyn structure.

   If any adjustment is made to the ELF object after it has been
   built these entries will need to be adjusted.  */
#define DT_ADDRRNGLO 0x6ffffe00
#define DT_GNU_HASH 0x6ffffef5 /* GNU-style hash table.  */
#define DT_ADDRRNGHI 0x6ffffeff

/* The versioning entry types.  The next are defined as part of the
   GNU extension.  */
#define DT_VERSYM 0x6ffffff0

#define DT_RELACOUNT 0x6ffffff9
#define DT_RELCOUNT 0x6ffffffa

/* These were chosen by Sun.  */
#define DT_FLAGS_1 0x6ffffffb /* State flags, see DF_1_* below.  */
#define DT_VERNEED                                                             \
  0x6ffffffe                     /* Address of table with needed               \
                                    versions */
#define DT_VERNEEDNUM 0x6fffffff /* Number of needed versions */

/* Values of `d_un.d_val' in the DT_FLAGS entry.  */
#define DF_BIND_NOW 0x00000008   /* No lazy binding for this object */

/* State flags selectable in the `d_un.d_val' element of the DT_FLAGS_1
   entry in the dynamic section.  */
#define DF_1_NOW 0x00000001       /* Set RTLD_NOW for this object.  */
#define DF_1_PIE 0x08000000

/* Version definition sections.  */

typedef struct {
  Elf32_Half vd_version; /* Version revision */
  Elf32_Half vd_flags;   /* Version information */
  Elf32_Half vd_ndx;     /* Version Index */
  Elf32_Half vd_cnt;     /* Number of associated aux entries */
  Elf32_Word vd_hash;    /* Version name hash value */
  Elf32_Word vd_aux;     /* Offset in bytes to verdaux array */
  Elf32_Word vd_next;    /* Offset in bytes to next verdef
                            entry */
} Elf32_Verdef;

typedef struct {
  Elf64_Half vd_version; /* Version revision */
  Elf64_Half vd_flags;   /* Version information */
  Elf64_Half vd_ndx;     /* Version Index */
  Elf64_Half vd_cnt;     /* Number of associated aux entries */
  Elf64_Word vd_hash;    /* Version name hash value */
  Elf64_Word vd_aux;     /* Offset in bytes to verdaux array */
  Elf64_Word vd_next;    /* Offset in bytes to next verdef
                            entry */
} Elf64_Verdef;

/* Legal values for vd_flags (version information flags).  */
#define VER_FLG_WEAK 0x2 /* Weak version identifier */

/* Auxiliary version information.  */

typedef struct {
  Elf32_Word vda_name; /* Version or dependency names */
  Elf32_Word vda_next; /* Offset in bytes to next verdaux
                          entry */
} Elf32_Verdaux;

typedef struct {
  Elf64_Word vda_name; /* Version or dependency names */
  Elf64_Word vda_next; /* Offset in bytes to next verdaux
                          entry */
} Elf64_Verdaux;

/* Version dependency section.  */

typedef struct {
  Elf32_Half vn_version; /* Version of structure */
  Elf32_Half vn_cnt;     /* Number of associated aux entries */
  Elf32_Word vn_file;    /* Offset of filename for this
                            dependency */
  Elf32_Word vn_aux;     /* Offset in bytes to vernaux array */
  Elf32_Word vn_next;    /* Offset in bytes to next verneed
                            entry */
} Elf32_Verneed;

typedef struct {
  Elf64_Half vn_version; /* Version of structure */
  Elf64_Half vn_cnt;     /* Number of associated aux entries */
  Elf64_Word vn_file;    /* Offset of filename for this
                            dependency */
  Elf64_Word vn_aux;     /* Offset in bytes to vernaux array */
  Elf64_Word vn_next;    /* Offset in bytes to next verneed
                            entry */
} Elf64_Verneed;

/* Auxiliary needed version information.  */

typedef struct {
  Elf32_Word vna_hash;  /* Hash value of dependency name */
  Elf32_Half vna_flags; /* Dependency specific information */
  Elf32_Half vna_other; /* Unused */
  Elf32_Word vna_name;  /* Dependency name string offset */
  Elf32_Word vna_next;  /* Offset in bytes to next vernaux
                           entry */
} Elf32_Vernaux;

typedef struct {
  Elf64_Word vna_hash;  /* Hash value of dependency name */
  Elf64_Half vna_flags; /* Dependency specific information */
  Elf64_Half vna_other; /* Unused */
  Elf64_Word vna_name;  /* Dependency name string offset */
  Elf64_Word vna_next;  /* Offset in bytes to next vernaux
                           entry */
} Elf64_Vernaux;

/* Legal values for vna_flags.  */
#define VER_FLG_WEAK 0x2 /* Weak version identifier */

/* This vector is normally only used by the program interpreter.  The
   usual definition in an ABI supplement uses the name auxv_t.  The
   vector is not usually defined in a standard <elf.h> file, but it
   can't hurt.  We rename it to avoid conflicts.  The sizes of these
   types are an arrangement between the exec server and the program
   interpreter, so we don't fully specify them here.  */

typedef struct {
  uint32_t a_type; /* Entry type */
  union {
    uint32_t a_val; /* Integer value */
    /* We use to have pointer elements added here.  We cannot do that,
       though, since it does not work when using 32-bit definitions
       on 64-bit platforms and vice versa.  */
  } a_un;
} Elf32_auxv_t;

typedef struct {
  uint64_t a_type; /* Entry type */
  union {
    uint64_t a_val; /* Integer value */
    /* We use to have pointer elements added here.  We cannot do that,
       though, since it does not work when using 32-bit definitions
       on 64-bit platforms and vice versa.  */
  } a_un;
} Elf64_auxv_t;

/* Note section contents.  Each entry in the note section begins with
   a header of a fixed form.  */

typedef struct {
  Elf32_Word n_namesz; /* Length of the note's name.  */
  Elf32_Word n_descsz; /* Length of the note's descriptor.  */
  Elf32_Word n_type;   /* Type of the note.  */
} Elf32_Nhdr;

typedef struct {
  Elf64_Word n_namesz; /* Length of the note's name.  */
  Elf64_Word n_descsz; /* Length of the note's descriptor.  */
  Elf64_Word n_type;   /* Type of the note.  */
} Elf64_Nhdr;

/* ABI information.  The descriptor consists of words:
   word 0: OS descriptor
   word 1: major version of the ABI
   word 2: minor version of the ABI
   word 3: subminor version of the ABI
*/
#define NT_GNU_ABI_TAG 1

/* Known OSes.  These values can appear in word 0 of an
   NT_GNU_ABI_TAG note section entry.  */
#define ELF_NOTE_OS_GNU 1

/* Move records.  */
typedef struct {
  Elf32_Xword m_value;  /* Symbol value.  */
  Elf32_Word m_info;    /* Size and index.  */
  Elf32_Word m_poffset; /* Symbol offset.  */
  Elf32_Half m_repeat;  /* Repeat count.  */
  Elf32_Half m_stride;  /* Stride info.  */
} Elf32_Move;

typedef struct {
  Elf64_Xword m_value;   /* Symbol value.  */
  Elf64_Xword m_info;    /* Size and index.  */
  Elf64_Xword m_poffset; /* Symbol offset.  */
  Elf64_Half m_repeat;   /* Repeat count.  */
  Elf64_Half m_stride;   /* Stride info.  */
} Elf64_Move;

/* i386 relocs.  */

#define R_386_32 1       /* Direct 32 bit  */
#define R_386_PC32 2     /* PC relative 32 bit */
#define R_386_RELATIVE 8 /* Adjust by program base */

/* Legal values for sh_type field of Elf32_Shdr.  */

#define SHT_MIPS_LIBLIST 0x70000000 /* Shared objects used in link */
#define SHT_MIPS_CONFLICT 0x70000002 /* Conflicting symbols */
#define SHT_MIPS_GPTAB 0x70000003    /* Global data area sizes */
#define SHT_MIPS_REGINFO 0x70000006  /* Register usage information */
#define SHT_MIPS_OPTIONS 0x7000000d /* Miscellaneous options.  */

/* Entries found in sections of type SHT_MIPS_GPTAB.  */

typedef union {
  struct {
    Elf32_Word gt_current_g_value; /* -G value used for compilation */
    Elf32_Word gt_unused;          /* Not used */
  } gt_header;                     /* First entry in section */
  struct {
    Elf32_Word gt_g_value; /* If this value were used for -G */
    Elf32_Word gt_bytes;   /* This many bytes would be used */
  } gt_entry;              /* Subsequent entries in section */
} Elf32_gptab;

/* Entry found in sections of type SHT_MIPS_REGINFO.  */

typedef struct {
  Elf32_Word ri_gprmask;    /* General registers used */
  Elf32_Word ri_cprmask[4]; /* Coprocessor registers used */
  Elf32_Sword ri_gp_value;  /* $gp register value */
} Elf32_RegInfo;

/* Entries found in sections of type SHT_MIPS_OPTIONS.  */

typedef struct {
  unsigned char kind;    /* Determines interpretation of the
                            variable part of descriptor.  */
  unsigned char size;    /* Size of descriptor, including header.  */
  Elf32_Section section; /* Section header index of section affected,
                            0 for global options.  */
  Elf32_Word info;       /* Kind-specific information.  */
} Elf_Options;

/* Values for `kind' field in Elf_Options.  */

#define ODK_EXCEPTIONS 2 /* Exception processing options.  */
#define ODK_HWPATCH 4    /* Hardware workarounds performed */
#define ODK_HWAND 7      /* HW workarounds.  'AND' bits when merging. */
#define ODK_HWOR 8       /* HW workarounds.  'OR' bits when merging.  */

/* Entry found in `.options' section.  */

typedef struct {
  Elf32_Word hwp_flags1; /* Extra flags.  */
  Elf32_Word hwp_flags2; /* Extra flags.  */
} Elf_Options_Hw;

/* Legal values for d_tag field of Elf32_Dyn.  */

#define DT_MIPS_FLAGS 0x70000005        /* Flags */

/* Entries found in sections of type SHT_MIPS_LIBLIST.  */

typedef struct {
  Elf32_Word l_name;       /* Name (string table index) */
  Elf32_Word l_time_stamp; /* Timestamp */
  Elf32_Word l_checksum;   /* Checksum */
  Elf32_Word l_version;    /* Interface version */
  Elf32_Word l_flags;      /* Flags */
} Elf32_Lib;

typedef struct {
  Elf64_Word l_name;       /* Name (string table index) */
  Elf64_Word l_time_stamp; /* Timestamp */
  Elf64_Word l_checksum;   /* Checksum */
  Elf64_Word l_version;    /* Interface version */
  Elf64_Word l_flags;      /* Flags */
} Elf64_Lib;

/* Entries found in sections of type SHT_MIPS_CONFLICT.  */

typedef Elf32_Addr Elf32_Conflict;

/* Legal values for e_flags field of Elf32_Ehdr.  */

#define EF_PARISC_ARCH 0x0000ffff     /* Architecture version.  */

/* Processor specific flags for the ELF header e_flags field.  */
#define EF_ARM_SOFT_FLOAT 0x200
#define EF_ARM_VFP_FLOAT 0x400

/* Constants defined in AAELF.  */

#define EF_ARM_EABI_VER5 0x05000000

/* Processor specific values for the Shdr sh_type field.  */
#define SHT_ARM_EXIDX (SHT_LOPROC + 1)      /* ARM unwind section.  */
#define SHT_ARM_ATTRIBUTES (SHT_LOPROC + 3) /* ARM attributes section.  */

/* AArch64 relocs.  */

#define R_AARCH64_ABS64 257            /* Direct 64 bit. */
#define R_AARCH64_ABS32 258            /* Direct 32 bit.  */
#define R_AARCH64_PREL32 261           /* PC-relative 32-bit.  */

/* ARM relocs.  */

#define R_ARM_NONE 0  /* No reloc */
#define R_ARM_PC24 1  /* PC relative 26 bit branch */
#define R_ARM_ABS32 2 /* Direct 32 bit  */
#define R_ARM_REL32 3 /* PC relative 32 bit */
#define R_ARM_THM_PC22 10
#define R_ARM_THM_PC8 11
#define R_ARM_COPY 20         /* Copy symbol at runtime */
#define R_ARM_GLOB_DAT 21     /* Create GOT entry */
#define R_ARM_JUMP_SLOT 22    /* Create PLT entry */
#define R_ARM_RELATIVE 23     /* Adjust by program base */
#define R_ARM_GOTOFF 24       /* 32 bit offset to GOT */
#define R_ARM_GOTPC 25        /* 32 bit PC relative offset to GOT */
#define R_ARM_GOT32 26        /* 32 bit GOT entry, same as R_ARM_GOT_BREL */
#define R_ARM_PLT32 27        /* 32 bit PLT address */
#define R_ARM_CALL 28
#define R_ARM_JUMP24 29
#define R_ARM_THM_JUMP24 30
#define R_ARM_TARGET1 38
#define R_ARM_V4BX 40
#define R_ARM_TARGET2 41
#define R_ARM_PREL31 42
#define R_ARM_MOVW_ABS_NC 43
#define R_ARM_MOVT_ABS 44
#define R_ARM_MOVW_PREL_NC 45 /* PC relative 16-bit (MOVW).  */
#define R_ARM_MOVT_PREL 46    /* PC relative (MOVT).  */
#define R_ARM_THM_MOVW_ABS_NC 47
#define R_ARM_THM_MOVT_ABS 48
#define R_ARM_THM_JUMP19 51
#define R_ARM_THM_JUMP6 52
#define R_ARM_THM_ALU_PREL_11_0 53
#define R_ARM_THM_PC12 54
/* Values from 49 to 89 are not yet used/handled by tcc. */
#define R_ARM_GOT_PREL 96
/* YASOS RELRO: 32-bit offset of a symbol within .rodata (S - rodata base).
 * Emitted for references to shared (pure-const) .rodata symbols; the runtime
 * address is anchor(rodata base from a fixed GOT slot) + this offset. Resolved
 * at link time and baked into the .text literal, so it never reaches the YAFF
 * writer (like R_ARM_GOTOFF). Uses a free value in the 130-159 ABI gap. */
#define R_ARM_RODATA_OFF 137
/* YASOS SB-relative GOT: the symbol's GOT slot offset written into the imm12
 * field of a `ldr.w Rt,[r9,#imm12]`, replacing the literal+add+load sequence.
 * Caps the GOT at 4096 bytes; -mno-sb-relative-got selects the literal path.
 * See docs/sb_relative_got.md. Uses a free value in the 130-159 ABI gap. */
#define R_ARM_GOT_SBREL12 138
/* YASOS module-local call marker: sits on a BL/B.W (next to its
 * R_ARM_THM_JUMP24) whose R9 reload the compiler dropped because it assumed
 * the callee binds inside this module (-fmodule-local-calls). Patches nothing;
 * the linker errors if the call binds to the PLT, since a cross-module callee
 * returns with its own R9 and nothing would restore ours. Never reaches the
 * YAFF writer. Uses a free value in the 130-159 ABI gap. */
#define R_ARM_YASOS_LOCAL_CALL 139
/* Keep this the last entry.  */
#define R_ARM_NUM 256

/* AMD x86-64 relocations.  */
#define R_X86_64_64 1        /* Direct 64 bit  */
#define R_X86_64_PC32 2      /* PC relative 32 bit signed */
#define R_X86_64_GOT32 3     /* 32 bit GOT entry */
#define R_X86_64_PLT32 4     /* 32 bit PLT address */
#define R_X86_64_GOTPCREL                                                      \
  9                          /* 32 bit signed PC relative                      \
                                offset to GOT */
#define R_X86_64_32 10       /* Direct 32 bit zero extended */
#define R_X86_64_32S 11      /* Direct 32 bit sign extended */
#define R_X86_64_GOTPCRELX                                                     \
  41 /* like GOTPCREL, but optionally with                                     \
        linker optimizations */
#define R_X86_64_REX_GOTPCRELX                                                 \
  42 /* like GOTPCRELX, but a REX prefix                                       \
        is present */

/* x86-64 sh_type values.  */
#define SHT_X86_64_UNWIND 0x70000001 /* Unwind information.  */

/* RISC-V ELF Flags */
#define EF_RISCV_FLOAT_ABI_DOUBLE 0x0004

/* RISC-V relocations.  */
#define R_RISCV_32 1
#define R_RISCV_64 2
#define R_RISCV_ALIGN 43
#define R_RISCV_RELAX 51
#define R_RISCV_32_PCREL 57

#endif /* elf.h */
