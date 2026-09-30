/*
 *  ELF file handling for TCC
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* Declarations shared by the ELF writer/linker's source files (tccelf.c and
 * tccelf_{sym,reloc,layout,output,load,ld}.c), which used to be one
 * 7.3k-line TU.  Each function declared at the bottom is defined in the
 * file its comment names. */

#ifndef TCCELF_PRIV_H
#define TCCELF_PRIV_H

#include "tcc.h"
#include "tccld.h"
#include "tccyaff.h"

/********************************************************/
/* global variables */

/* elf version information */
struct sym_version
{
  char *lib;
  char *version;
  int out_index;
  int prev_same_lib;
};

#define nb_sym_versions s1->nb_sym_versions
#define sym_versions s1->sym_versions
#define nb_sym_to_version s1->nb_sym_to_version
#define sym_to_version s1->sym_to_version
#define dt_verneednum s1->dt_verneednum
#define versym_section s1->versym_section
#define verneed_section s1->verneed_section

/* special flag to indicate that the section should not be linked to the other
 * ones */
#define SHF_PRIVATE 0x80000000
/* section is dynsymtab_section */
#define SHF_DYNSYM 0x40000000
/* Larger initial ELF hash tables reduce rebuild churn without changing lookup semantics. */
#define SYMTAB_INITIAL_HASH_BUCKETS 512

#if defined(TCC_TARGET_PE) || defined(TCC_TARGET_ARM_THUMB)
#define shf_RELRO SHF_ALLOC
#else
#define shf_RELRO SHF_ALLOC /* eventually made SHF_WRITE in sort_sections() */
#endif
#ifndef ELF_OBJ_ONLY
/* various data used under elf_output_file() */
struct dyn_inf
{
  Section *dynamic;
  Section *dynstr;
  struct
  {
    /* Info to be copied in dynamic section */
    unsigned long data_offset;
    addr_t rel_addr;
    addr_t rel_size;
  };

  ElfW(Phdr) * phdr;
  int phnum;
  int shnum;
  Section *interp;
  Section *note;
  Section *gnu_hash;

  /* read only segment mapping for GNU_RELRO */
  Section _roinf, *roinf;
};
#endif

typedef struct SectionMergeInfo
{
  Section *s;            /* corresponding existing section */
  unsigned long offset;  /* offset of the new section in the existing section */
  uint8_t new_section;   /* true if section 's' was added */
  uint8_t link_once;     /* true if link once section */
  const char *merged_to; /* canonical name if section was merged */
} SectionMergeInfo;

typedef struct ArchiveHeader
{
  char ar_name[16]; /* name of this member */
  char ar_date[12]; /* file mtime */
  char ar_uid[6];   /* owner uid; printed as decimal */
  char ar_gid[6];   /* owner gid; printed as decimal */
  char ar_mode[8];  /* file mode, printed as octal   */
  char ar_size[10]; /* file size, printed as decimal */
  char ar_fmag[2];  /* should contain ARFMAG */
} ArchiveHeader;

#define ARFMAG "`\n"

/* Defined in tccelf.c. */
int should_defer_section(const char *name, int sh_type);
void free_deferred_chunks(Section *sec);
void section_add_deferred(TCCState *s1, Section *sec, const char *path, unsigned long file_off,
                                 unsigned long size, unsigned long dest_off);
ST_FUNC void section_ensure_loaded(TCCState *s1, Section *sec);
int section_write_streaming(TCCState *s1, Section *sec, FILE *f);
ST_FUNC void tcc_free_lazy_objfiles(TCCState *s1);
ST_FUNC int tcc_load_object_file_lazy(TCCState *s1, int fd, unsigned long file_offset);
ST_FUNC void tcc_gc_mark_phase(TCCState *s1);
ST_FUNC void tcc_load_referenced_sections(TCCState *s1);

/* Defined in tccelf_sym.c. */
Section *section_ht_find(TCCState *s1, const char *name);
ST_FUNC Section *new_section(TCCState *s1, const char *name, int sh_type, int sh_flags);
ST_FUNC Section *new_symtab(TCCState *s1, const char *symtab_name, int sh_type, int sh_flags, const char *strtab_name,
                            const char *hash_name, int hash_sh_flags);
ST_FUNC void section_realloc(Section *sec, unsigned long new_size);
ST_FUNC size_t section_add(Section *sec, addr_t size, int align);
ST_FUNC void *section_ptr_add(Section *sec, addr_t size);
#ifndef ELF_OBJ_ONLY
void section_reserve(Section *sec, unsigned long size);
#endif
Section *have_section(TCCState *s1, const char *name);
ST_FUNC Section *find_section(TCCState *s1, const char *name);
ST_FUNC int put_elf_str(Section *s, const char *sym);
ElfW(Word) elf_hash(const unsigned char *name);
ST_FUNC int put_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name);
ST_FUNC int find_elf_sym(Section *s, const char *name);
ST_FUNC int tcc_dynsym_find(TCCState *s1, const char *name);
ST_FUNC addr_t get_sym_addr(TCCState *s1, const char *name, int err, int forc);
#ifndef ELF_OBJ_ONLY
void version_add(TCCState *s1);
#endif
ST_FUNC int set_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name);
ST_FUNC void put_elf_reloc(Section *symtab, Section *s, unsigned long offset, int type, int symbol);
ST_FUNC struct sym_attr *get_sym_attr(TCCState *s1, int index, int alloc);
void update_relocs(TCCState *s1, Section *s, int *old_to_new_syms, int first_sym);
ST_FUNC void tcc_elf_sort_syms(TCCState *s1, Section *s);
#ifndef ELF_OBJ_ONLY
Section *create_gnu_hash(TCCState *s1);
void update_gnu_hash(TCCState *s1, Section *gnu_hash);
#endif

/* Defined in tccelf_reloc.c. */
ST_FUNC void relocate_syms(TCCState *s1, Section *symtab, int do_resolve);
void free_reloc_patches(Section *s);
ST_FUNC void relocate_sections(TCCState *s1);
#ifndef ELF_OBJ_ONLY
int prepare_dynamic_rel(TCCState *s1, Section *sr);
#endif
#ifdef NEED_BUILD_GOT
int build_got(TCCState *s1);
ST_FUNC void build_got_entries(TCCState *s1, int got_sym);
#endif
ST_FUNC int set_global_sym(TCCState *s1, const char *name, Section *sec, addr_t offs);
void tcc_tcov_add_file(TCCState *s1, const char *filename);
#ifndef TCC_TARGET_PE
ST_FUNC void tcc_add_runtime(TCCState *s1);
#endif
ST_FUNC void resolve_common_syms(TCCState *s1);

/* Defined in tccelf_layout.c. */
#ifndef ELF_OBJ_ONLY
ST_FUNC void fill_got(TCCState *s1);
void fill_local_got_entries(TCCState *s1);
void bind_exe_dynsyms(TCCState *s1, int is_PIE);
void bind_libs_dynsyms(TCCState *s1);
void export_global_syms(TCCState *s1);
int set_sec_sizes(TCCState *s1);
int ld_find_output_section_idx(TCCState *s1, const char *name, int *pat_idx);
int ld_section_matches_output(TCCState *s1, const char *name, int os_idx);
int layout_sections(TCCState *s1, int *sec_order, struct dyn_inf *d);
void put_dt(Section *dynamic, int dt, addr_t val);
void fill_dynamic(TCCState *s1, struct dyn_inf *dyninf);
void update_reloc_sections(TCCState *s1, struct dyn_inf *dyninf);
#endif

/* Defined in tccelf_output.c. */
#ifndef ELF_OBJ_ONLY
void gc_sections(TCCState *s1);
#endif

/* Defined in tccelf_load.c. */
ST_FUNC ssize_t full_read(int fd, void *buf, size_t count);
ST_FUNC void *load_data(int fd, unsigned long file_offset, unsigned long size);
ST_FUNC int tcc_object_type(int fd, ElfW(Ehdr) * h);
ST_FUNC int tcc_group_has_satisfiable_undefs(TCCState *s1);

/* Defined in tccelf_ld.c. */
#ifndef ELF_OBJ_ONLY
ST_FUNC int tcc_load_linker_script(TCCState *s1, const char *filename);
void ld_apply_symbols(TCCState *s1, LDScript *ld);
void ld_update_symbol_values(TCCState *s1, LDScript *ld);
ST_FUNC void ld_export_standard_symbols(TCCState *s1);
#endif

#endif /* TCCELF_PRIV_H */
