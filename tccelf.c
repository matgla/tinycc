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

#include "tcc.h"
#include "tccld.h"
#include "tccyaff.h"

/* Define this to get some debug output during relocation processing.  */
// #define DEBUG_RELOC

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

#if defined(TCC_TARGET_PE)
#define shf_RELRO SHF_ALLOC
static const char rdata[] = ".rdata";
#elif defined(TCC_TARGET_ARM_THUMB)
#define shf_RELRO SHF_ALLOC
static const char rdata[] = ".rodata";
#else
#define shf_RELRO SHF_ALLOC /* eventually made SHF_WRITE in sort_sections() */
static const char rdata[] = ".data.ro";
#endif

/* ------------------------------------------------------------------------- */

ST_FUNC void tccelf_new(TCCState *s)
{
  TCCState *s1 = s;

  /* no section zero */
  dynarray_add(&s->sections, &s->nb_sections, NULL);

  /* create standard sections */
  text_section = new_section(s, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
  data_section = new_section(s, ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
  /* create ro data section (make ro after relocation done with GNU_RELRO) */
  rodata_section = new_section(s, rdata, SHT_PROGBITS, shf_RELRO);
  bss_section = new_section(s, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);
  common_section = new_section(s, ".common", SHT_NOBITS, SHF_PRIVATE);
  common_section->sh_num = SHN_COMMON;

  /* symbols are always generated for linking stage */
  symtab_section = new_symtab(s, ".symtab", SHT_SYMTAB, 0, ".strtab", ".hashtab", SHF_PRIVATE);

  /* private symbol table for dynamic symbols */
  s->dynsymtab_section =
      new_symtab(s, ".dynsymtab", SHT_SYMTAB, SHF_PRIVATE | SHF_DYNSYM, ".dynstrtab", ".dynhashtab", SHF_PRIVATE);
  get_sym_attr(s, 0, 1);

  if (s->do_debug)
  {
    /* add debug sections */
    tcc_debug_new(s);
  }

#if TCC_EH_FRAME
  if (s->output_format != TCC_OUTPUT_FORMAT_ELF)
    s->unwind_tables = 0;
  tcc_eh_frame_start(s);
#endif

#ifdef CONFIG_TCC_BCHECK
  if (s->do_bounds_check)
  {
    /* if bound checking, then add corresponding sections */
    /* (make ro after relocation done with GNU_RELRO) */
    bounds_section = new_section(s, ".bounds", SHT_PROGBITS, shf_RELRO);
    lbounds_section = new_section(s, ".lbounds", SHT_PROGBITS, shf_RELRO);
  }
#endif

#ifdef TCC_TARGET_PE
  /* to make sure that -ltcc1 -Wl,-e,_start will grab the startup code
     from libtcc1.a (unless _start defined) */
  if (s->elf_entryname)
    set_global_sym(s, s->elf_entryname, NULL, 0); /* SHN_UNDEF */
#endif
}

/* -------------------------------------------------- */
/* Lazy section loading support */

/* Check if section should use lazy loading */
static int should_defer_section(const char *name, int sh_type)
{
  /* Always defer DWARF debug sections (original behavior) */
  if (strncmp(name, ".debug_", 7) == 0)
    return 1;

  /* Never defer relocation sections - needed by GC */
  if (sh_type == SHT_REL || sh_type == SHT_RELA)
    return 0;

  /* Never defer ARM exception handling sections - needed for runtime */
  if (strncmp(name, ".ARM", 4) == 0)
    return 0;

  /* Never defer eh_frame - needed for stack unwinding */
  if (strncmp(name, ".eh_frame", 9) == 0)
    return 0;

  /* Defer all other sections (full deferred loading) */
  return 1;
}

/* Forward declarations for lazy loading functions */
static void free_reloc_patches(Section *s);
static void apply_reloc_patches(Section *sec, unsigned char *data, size_t size);
static void sort_patches_by_offset(Section *sec);
static void add_reloc_patch(Section *s, uint32_t offset, uint32_t value);

/* Free all deferred chunks for a section */
static void free_deferred_chunks(Section *sec)
{
  DeferredChunk *c = sec->deferred_head;
  while (c)
  {
    DeferredChunk *next = c->next;
    /* source_path is now duplicated, so free it */
    if (c->source_path)
      tcc_free((void *)c->source_path);
    tcc_free(c);
    c = next;
  }
  sec->deferred_head = sec->deferred_tail = NULL;
}

/* Add a deferred chunk to a section */
static void section_add_deferred(TCCState *s1, Section *sec, const char *path, unsigned long file_off,
                                 unsigned long size, unsigned long dest_off)
{
  DeferredChunk *chunk = tcc_mallocz(sizeof(DeferredChunk));
  /* Duplicate the path string so it survives after loading context changes */
  chunk->source_path = path ? tcc_strdup(path) : NULL;
  /* For archives, the file_offset is already relative to the member start */
  chunk->file_offset = (uint32_t)file_off;
  chunk->size = (uint32_t)size;
  chunk->dest_offset = (uint32_t)dest_off;
  chunk->materialized = 0;

  if (sec->deferred_tail)
  {
    sec->deferred_tail->next = chunk;
  }
  else
  {
    sec->deferred_head = chunk;
  }
  sec->deferred_tail = chunk;
  sec->lazy = 1;
  sec->has_deferred_chunks = 1;

  (void)path; /* silence warning when debug disabled */
}

/* Load all deferred data for a section */
ST_FUNC void section_materialize(TCCState *s1, Section *sec)
{
  DeferredChunk *c;
  int fd;

  /* section_materialize */

  if (!sec->lazy || sec->materialized)
    return;

  /* Allocate buffer for full section */
  if (sec->sh_type != SHT_NOBITS)
  {
    /* Reallocating section for materialization */
    section_realloc(sec, sec->data_offset);
  }

  /* Load each deferred chunk - file_offset is absolute for regular files,
   * relative to archive member for archive files (handled by caller) */
  for (c = sec->deferred_head; c; c = c->next)
  {
    if (c->materialized)
    {
      /* Chunk already materialized */
      continue;
    }
    /* Loading chunk */
    fd = open(c->source_path, O_RDONLY | O_BINARY);
    if (fd < 0)
    {
      fprintf(stderr, "tcc: cannot reopen '%s' for lazy loading\n", c->source_path);
      continue;
    }
    lseek(fd, c->file_offset, SEEK_SET);

    if (full_read(fd, sec->data + c->dest_offset, c->size) != c->size)
    {
      fprintf(stderr, "tcc: short read from '%s'\n", c->source_path);
    }
    else
    {
      c->materialized = 1;
      /* Successfully loaded */
    }
    close(fd);
  }

  /* Apply any relocation patches */
  if (sec->nb_reloc_patches > 0)
  {
    /* Applying relocation patches */
    apply_reloc_patches(sec, sec->data, sec->data_offset);
  }

  /* Free deferred chunk metadata to save memory */
  free_deferred_chunks(sec);
  sec->lazy = 0;

  /* Materialized section */
  sec->materialized = 1;
}

/* Ensure section data is available (call before accessing sec->data) */
ST_FUNC void section_ensure_loaded(TCCState *s1, Section *sec)
{
  /* section_ensure_loaded */
  /* Skip if section was garbage collected (zeroed by GC) */
  if (sec->data_offset == 0)
  {
    /* Free any deferred chunks since we won't need them */
    if (sec->lazy && sec->has_deferred_chunks)
    {
      free_deferred_chunks(sec);
      sec->lazy = 0;
      sec->has_deferred_chunks = 0;
    }
    return;
  }
  if (sec->lazy && !sec->materialized)
    section_materialize(s1, sec);
}

/* Sort patches by offset using simple insertion sort.
 * Returns new head of sorted list. */
/* Sort patches by offset using insertion sort (efficient for small arrays) */
static void sort_patches_by_offset(Section *sec)
{
  int i, j;
  int n = sec->nb_reloc_patches;
  uint32_t *offsets = sec->reloc_patch_offsets;
  uint32_t *values = sec->reloc_patch_values;

  for (i = 1; i < n; i++)
  {
    uint32_t key_offset = offsets[i];
    uint32_t key_value = values[i];
    j = i - 1;
    while (j >= 0 && offsets[j] > key_offset)
    {
      offsets[j + 1] = offsets[j];
      values[j + 1] = values[j];
      j--;
    }
    offsets[j + 1] = key_offset;
    values[j + 1] = key_value;
  }
}

/* Apply relocation patches to a memory buffer */
static void apply_reloc_patches(Section *sec, unsigned char *data, size_t size)
{
  int i;
  for (i = 0; i < sec->nb_reloc_patches; i++)
  {
    uint32_t offset = sec->reloc_patch_offsets[i];
    if (offset + 4 <= size)
    {
      add32le(data + offset, sec->reloc_patch_values[i]);
    }
  }
}

/* Apply patches to a buffer during streaming.
 * Applies all patches in [buf_start, buf_end) range starting from patch_idx.
 * Returns the number of patches applied and updates patch_idx. */
static int apply_patches_to_buffer(Section *sec, int *patch_idx, uint32_t buf_start, uint32_t buf_end,
                                   unsigned char *buffer, size_t buf_size)
{
  int applied = 0;
  int i = *patch_idx;

  while (i < sec->nb_reloc_patches && sec->reloc_patch_offsets[i] < buf_end)
  {
    uint32_t offset = sec->reloc_patch_offsets[i];
    if (offset >= buf_start)
    {
      uint32_t buf_offset = offset - buf_start;
      if (buf_offset + 4 <= buf_size)
      {
        write32le(buffer + buf_offset, sec->reloc_patch_values[i]);
        applied++;
      }
    }
    i++;
  }

  *patch_idx = i; /* Update to first unapplied patch */
  return applied;
}

/* Write a lazy section directly to output file without materializing to memory.
 * This avoids the memory allocation for sections that are only written to output.
 * Applies relocation patches inline during streaming if present.
 * Returns 0 on success, -1 on error. */
static int section_write_streaming(TCCState *s1, Section *sec, FILE *f)
{
  DeferredChunk *c;
  int fd;
  unsigned char buffer[1024];
  size_t to_read, n;
  int patch_idx = 0;

  if (!sec->lazy || sec->materialized)
  {
    /* Already materialized, use regular write */
    if (sec->data && sec->data_offset > 0)
    {
      fwrite(sec->data, 1, sec->data_offset, f);
    }
    return 0;
  }

  /* If there are relocation patches, sort them by offset for efficient streaming */
  if (sec->nb_reloc_patches > 0)
  {
    sort_patches_by_offset(sec);
  }

  /* Stream each chunk directly from source file to output */
  for (c = sec->deferred_head; c; c = c->next)
  {
    fd = open(c->source_path, O_RDONLY | O_BINARY);
    if (fd < 0)
    {
      fprintf(stderr, "tcc: cannot reopen '%s' for lazy loading\n", c->source_path);
      continue;
    }
    lseek(fd, c->file_offset, SEEK_SET);

    /* Stream data in chunks to avoid large buffers */
    to_read = c->size;
    uint32_t chunk_written = 0;

    while (to_read > 0)
    {
      n = to_read < sizeof(buffer) ? to_read : sizeof(buffer);
      if (read(fd, buffer, n) != n)
      {
        fprintf(stderr, "tcc: short read from '%s'\n", c->source_path);
        break;
      }

      /* Apply any patches that fall within this buffer */
      if (patch_idx < sec->nb_reloc_patches)
      {
        uint32_t buf_start = c->dest_offset + chunk_written;
        uint32_t buf_end = buf_start + n;
        apply_patches_to_buffer(sec, &patch_idx, buf_start, buf_end, buffer, n);
      }

      fwrite(buffer, 1, n, f);
      chunk_written += n;
      to_read -= n;
    }
    close(fd);
  }

  /* Write padding if needed */
  if (sec->data_offset > sec->sh_size)
  {
    size_t padding = sec->data_offset - sec->sh_size;
    while (padding > 0)
    {
      size_t pad = padding < sizeof(buffer) ? padding : sizeof(buffer);
      memset(buffer, 0, pad);
      fwrite(buffer, 1, pad, f);
      padding -= pad;
    }
  }

  return 0;
}

/* -------------------------------------------------- */
/* Phase 2: Garbage Collection During Loading */
/* -------------------------------------------------- */

/* Free a LazyObjectFile and all its resources */
static void free_lazy_objfile(LazyObjectFile *obj)
{
  int i;
  if (!obj)
    return;

  for (i = 0; i < obj->nb_sections; i++)
  {
    tcc_free(obj->sections[i].name);
  }
  tcc_free(obj->sections);
  tcc_free(obj->shdr);
  tcc_free(obj->strsec);
  tcc_free(obj->symtab);
  tcc_free(obj->strtab);
  tcc_free(obj->old_to_new_syms);
  tcc_free(obj->filename);

  /* Don't close fd here - it's managed by caller */
  tcc_free(obj);
}

/* Free all lazy object files in TCCState */
ST_FUNC void tcc_free_lazy_objfiles(TCCState *s1)
{
  int i;
  if (!s1->lazy_objfiles)
    return;

  for (i = 0; i < s1->nb_lazy_objfiles; i++)
  {
    free_lazy_objfile(s1->lazy_objfiles[i]);
  }
  tcc_free(s1->lazy_objfiles);
  s1->lazy_objfiles = NULL;
  s1->nb_lazy_objfiles = 0;
}

/* Check if section name indicates it should always be loaded (not subject to GC) */
static int section_is_mandatory(const char *name)
{
  /* These sections are always needed for linking */
  if (strcmp(name, ".text") == 0 || strncmp(name, ".text.", 6) == 0)
    return 1;
  if (strcmp(name, ".data") == 0 || strncmp(name, ".data.", 6) == 0)
    return 1;
  if (strcmp(name, ".rodata") == 0 || strncmp(name, ".rodata.", 8) == 0)
    return 1;
  if (strcmp(name, ".bss") == 0 || strncmp(name, ".bss.", 5) == 0)
    return 1;
  if (strcmp(name, ".init") == 0 || strcmp(name, ".fini") == 0)
    return 1;
  if (strncmp(name, ".init_array", 11) == 0 || strncmp(name, ".fini_array", 11) == 0)
    return 1;
  if (strncmp(name, ".preinit_array", 14) == 0)
    return 1;
  return 0;
}

/* Load an object file with lazy section loading (Phase 2)
 * This loads symbols immediately but defers section data until GC phase.
 * Returns 0 on success, -1 on error. */
ST_FUNC int tcc_load_object_file_lazy(TCCState *s1, int fd, unsigned long file_offset)
{
  LazyObjectFile *obj;
  ElfW(Ehdr) ehdr;
  ElfW(Shdr) * shdr, *sh;
  char *strsec, *sh_name;
  int i, nb_syms, sym_index;
  ElfW(Sym) * sym, *symtab;
  char *strtab;

  lseek(fd, file_offset, SEEK_SET);

  /* Verify object file type */
  if (tcc_object_type(fd, &ehdr) != AFF_BINTYPE_REL)
  {
    return tcc_error_noabort("invalid object file");
  }

  if (ehdr.e_ident[5] != ELFDATA2LSB || ehdr.e_machine != EM_TCC_TARGET)
  {
    return tcc_error_noabort("invalid object file");
  }

  /* Allocate LazyObjectFile */
  obj = tcc_mallocz(sizeof(LazyObjectFile));
  obj->ehdr = ehdr;
  obj->filename = tcc_strdup(s1->current_filename ? s1->current_filename : "<unknown>");
  /* Duplicate fd so it survives after caller closes the original */
  obj->fd = dup(fd);
  if (obj->fd < 0)
  {
    tcc_free(obj);
    return tcc_error_noabort("cannot duplicate file descriptor for lazy loading");
  }
  obj->file_offset = file_offset;

  /* Read section headers */
  shdr = load_data(fd, file_offset + obj->ehdr.e_shoff, sizeof(ElfW(Shdr)) * obj->ehdr.e_shnum);
  obj->shdr = shdr;

  /* Load section name string table */
  sh = &shdr[obj->ehdr.e_shstrndx];
  strsec = load_data(fd, file_offset + sh->sh_offset, sh->sh_size);
  obj->strsec = strsec;

  /* First pass: find symtab and strtab, count sections we care about */
  nb_syms = 0;
  symtab = NULL;
  strtab = NULL;

  for (i = 1; i < obj->ehdr.e_shnum; i++)
  {
    sh = &shdr[i];
    if (sh->sh_type == SHT_SYMTAB)
    {
      if (symtab)
      {
        tcc_error_noabort("object must contain only one symtab");
        goto fail;
      }
      nb_syms = sh->sh_size / sizeof(ElfW(Sym));
      symtab = load_data(fd, file_offset + sh->sh_offset, sh->sh_size);
      obj->symtab = symtab;

      /* Load associated string table */
      sh = &shdr[sh->sh_link];
      strtab = load_data(fd, file_offset + sh->sh_offset, sh->sh_size);
      obj->strtab = strtab;
    }
  }
  obj->nb_syms = nb_syms;

  /* Allocate sections array */
  obj->sections = tcc_mallocz(sizeof(LazySectionInfo) * obj->ehdr.e_shnum);
  obj->nb_sections = obj->ehdr.e_shnum;

  /* Fill in section info */
  for (i = 1; i < obj->ehdr.e_shnum; i++)
  {
    sh = &shdr[i];
    sh_name = strsec + sh->sh_name;

    obj->sections[i].name = tcc_strdup(sh_name);
    obj->sections[i].size = sh->sh_size;
    obj->sections[i].file_offset = sh->sh_offset;
    obj->sections[i].archive_offset = s1->current_archive_offset;
    obj->sections[i].sh_type = sh->sh_type;
    obj->sections[i].sh_flags = sh->sh_flags;
    obj->sections[i].sh_addralign = sh->sh_addralign;
    obj->sections[i].section = NULL;
    obj->sections[i].referenced = section_is_mandatory(sh_name);

    /* Track relocation section association */
    if (sh->sh_type == SHT_RELX)
    {
      int target_idx = sh->sh_info;
      if (target_idx > 0 && target_idx < obj->ehdr.e_shnum)
      {
        obj->sections[target_idx].reloc_index = i;
      }
    }
  }

  /* Free section name string table - names are now stored in sections array */
  tcc_free(strsec);
  obj->strsec = NULL;

  /* Allocate symbol mapping array */
  obj->old_to_new_syms = tcc_mallocz(nb_syms * sizeof(int));

  /* Add symbols to global symbol table immediately */
  sym = symtab + 1;
  for (i = 1; i < nb_syms; i++, sym++)
  {
    const char *name = strtab + sym->st_name;
    int shndx = sym->st_shndx;

    if (shndx != SHN_UNDEF && shndx < SHN_LORESERVE)
    {
      /* Defined symbol - mark its section as referenced */
      if (shndx < obj->ehdr.e_shnum)
      {
        obj->sections[shndx].referenced = 1;
      }
    }

    /* Add symbol to global symbol table */
    sym_index = set_elf_sym(symtab_section, sym->st_value, sym->st_size, sym->st_info, sym->st_other, shndx, name);
    obj->old_to_new_syms[i] = sym_index;
  }

  /* Add to lazy object file list */
  dynarray_add(&s1->lazy_objfiles, &s1->nb_lazy_objfiles, obj);

  return 0;

fail:
  free_lazy_objfile(obj);
  return -1;
}

/* Recursively mark a symbol and all sections it references */
static void mark_symbol_recursive(TCCState *s1, const char *name)
{
  int sym_index;
  ElfW(Sym) * sym;
  int i, j, r;
  LazyObjectFile *obj;

  if (!name || !name[0])
    return;

  /* Find symbol in global symbol table */
  sym_index = find_elf_sym(symtab_section, name);
  if (!sym_index)
    return;

  sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];

  /* If symbol is undefined, can't mark anything */
  if (sym->st_shndx == SHN_UNDEF)
    return;

  /* Mark all sections in all lazy object files that contain this symbol */
  for (i = 0; i < s1->nb_lazy_objfiles; i++)
  {
    obj = s1->lazy_objfiles[i];
    for (j = 1; j < obj->nb_syms; j++)
    {
      if (obj->old_to_new_syms[j] == sym_index)
      {
        int shndx = obj->symtab[j].st_shndx;
        if (shndx > 0 && shndx < obj->nb_sections)
        {
          if (!obj->sections[shndx].referenced)
          {
            obj->sections[shndx].referenced = 1;
            /* Recursively process relocations in this section */
            if (obj->sections[shndx].reloc_index)
            {
              int reloc_idx = obj->sections[shndx].reloc_index;
              ElfW(Shdr) *rel_sh = &obj->shdr[reloc_idx];
              ElfW(Rel) * rel;
              int nb_relocs = rel_sh->sh_size / sizeof(ElfW(Rel));

              /* Load and process relocations */
              rel = load_data(obj->fd, obj->file_offset + rel_sh->sh_offset, rel_sh->sh_size);
              for (r = 0; r < nb_relocs; r++)
              {
                int sym_idx = ELFW(R_SYM)(rel[r].r_info);
                if (sym_idx > 0 && sym_idx < obj->nb_syms)
                {
                  const char *ref_name = obj->strtab + obj->symtab[sym_idx].st_name;
                  mark_symbol_recursive(s1, ref_name);
                }
              }
              tcc_free(rel);
            }
          }
        }
        break;
      }
    }
  }
}

/* GC Mark Phase: Mark all reachable sections starting from entry points */
ST_FUNC void tcc_gc_mark_phase(TCCState *s1)
{
  int changed, i, j;

  if (!s1->gc_sections_aggressive || s1->nb_lazy_objfiles == 0)
    return;

  /* Start with root symbols */
  mark_symbol_recursive(s1, "_start");
  mark_symbol_recursive(s1, "main");
  mark_symbol_recursive(s1, s1->elf_entryname);

  /* Iteratively mark until no more changes */
  do
  {
    changed = 0;

    for (i = 0; i < s1->nb_lazy_objfiles; i++)
    {
      LazyObjectFile *obj = s1->lazy_objfiles[i];

      for (j = 1; j < obj->nb_sections; j++)
      {
        LazySectionInfo *sec = &obj->sections[j];

        if (!sec->referenced)
          continue;

        /* If section has relocations, mark all target symbols */
        if (sec->reloc_index)
        {
          int reloc_idx = sec->reloc_index;
          ElfW(Shdr) *rel_sh = &obj->shdr[reloc_idx];
          ElfW(Rel) * rel;
          int nb_relocs = rel_sh->sh_size / sizeof(ElfW(Rel));
          int r;

          rel = load_data(obj->fd, obj->file_offset + rel_sh->sh_offset, rel_sh->sh_size);
          for (r = 0; r < nb_relocs; r++)
          {
            int sym_idx = ELFW(R_SYM)(rel[r].r_info);
            if (sym_idx > 0 && sym_idx < obj->nb_syms)
            {
              int target_shndx = obj->symtab[sym_idx].st_shndx;
              if (target_shndx > 0 && target_shndx < obj->nb_sections)
              {
                if (!obj->sections[target_shndx].referenced)
                {
                  obj->sections[target_shndx].referenced = 1;
                  changed = 1;
                }
              }
            }
          }
          tcc_free(rel);
        }
      }
    }
  } while (changed);
}

/* Load all referenced sections from lazy object files */
ST_FUNC void tcc_load_referenced_sections(TCCState *s1)
{
  int i, j;

  if (!s1->gc_sections_aggressive || s1->nb_lazy_objfiles == 0)
    return;

  for (i = 0; i < s1->nb_lazy_objfiles; i++)
  {
    LazyObjectFile *obj = s1->lazy_objfiles[i];

    for (j = 1; j < obj->nb_sections; j++)
    {
      LazySectionInfo *ls = &obj->sections[j];

      /* Skip if not referenced */
      if (!ls->referenced)
        continue;

      /* Skip if already loaded */
      if (ls->section)
        continue;

      /* Skip symbol table sections - already processed */
      if (ls->sh_type == SHT_SYMTAB || ls->sh_type == SHT_STRTAB)
        continue;

      /* Skip relocation sections - we'll process them separately */
      if (ls->sh_type == SHT_RELX)
        continue;

      /* Skip section name string table */
      if (j == obj->ehdr.e_shstrndx)
        continue;

      /* Create the section */
      ls->section = new_section(s1, ls->name, ls->sh_type, ls->sh_flags & ~SHF_GROUP);
      ls->section->sh_addralign = ls->sh_addralign;

      /* Load the data */
      if (ls->sh_type != SHT_NOBITS && ls->size > 0)
      {
        unsigned char *ptr;
        unsigned long abs_offset = obj->file_offset + ls->file_offset;

        lseek(obj->fd, abs_offset, SEEK_SET);
        ptr = section_ptr_add(ls->section, ls->size);
        full_read(obj->fd, ptr, ls->size);
      }

      /* Update section offset in lazy info */
      ls->section->data_offset = ls->size;
    }

    /* Second pass: handle relocations */
    for (j = 1; j < obj->nb_sections; j++)
    {
      LazySectionInfo *ls = &obj->sections[j];

      if (ls->sh_type != SHT_RELX)
        continue;

      /* Only load relocations if target section is referenced */
      int target_idx = obj->shdr[j].sh_info;
      if (target_idx <= 0 || target_idx >= obj->nb_sections)
        continue;

      LazySectionInfo *target_ls = &obj->sections[target_idx];
      if (!target_ls->referenced || !target_ls->section)
        continue;

      /* Create relocation section */
      Section *rel_sec = new_section(s1, ls->name, SHT_RELX, ls->sh_flags);
      rel_sec->sh_info = target_ls->section->sh_num;
      rel_sec->link = symtab_section;
      target_ls->section->reloc = rel_sec;

      /* Load and process relocations */
      ElfW(Rel) * rel;
      int nb_relocs = ls->size / sizeof(ElfW(Rel));
      int r;

      rel = load_data(obj->fd, obj->file_offset + ls->file_offset, ls->size);

      for (r = 0; r < nb_relocs; r++)
      {
        int type = ELFW(R_TYPE)(rel[r].r_info);
        int old_sym = ELFW(R_SYM)(rel[r].r_info);
        int new_sym = 0;

        if (old_sym > 0 && old_sym < obj->nb_syms)
        {
          new_sym = obj->old_to_new_syms[old_sym];
        }

        /* Add relocation to section */
        ElfW(Rel) *new_rel = section_ptr_add(rel_sec, sizeof(ElfW(Rel)));
        new_rel->r_offset = rel[r].r_offset;
        new_rel->r_info = ELFW(R_INFO)(new_sym, type);
      }

      tcc_free(rel);
    }

    /* Close file descriptor for this object */
    if (obj->fd >= 0)
    {
      close(obj->fd);
      obj->fd = -1;
    }
  }
}

/* -------------------------------------------------- */

ST_FUNC void free_section(Section *s)
{
  if (!s)
    return;
  free_deferred_chunks(s); /* Clean up lazy loading metadata */
  free_reloc_patches(s);   /* Clean up relocation patches */
  tcc_free(s->str_hash);   /* Clean up string hash table */
  tcc_free(s->data);
  s->data = NULL;
  s->data_allocated = s->data_offset = 0;
  s->str_hash = NULL;
  s->str_hash_size = 0;
  s->str_hash_count = 0;
  s->nb_reloc_patches = 0;
  s->alloc_reloc_patches = 0;
}

ST_FUNC void tccelf_delete(TCCState *s1)
{
  int i;

#ifndef ELF_OBJ_ONLY
  /* free symbol versions */
  for (i = 0; i < nb_sym_versions; i++)
  {
    tcc_free(sym_versions[i].version);
    tcc_free(sym_versions[i].lib);
  }
  tcc_free(sym_versions);
  tcc_free(sym_to_version);
#endif

  /* free all sections */
  for (i = 1; i < s1->nb_sections; i++)
    free_section(s1->sections[i]);
  dynarray_reset(&s1->sections, &s1->nb_sections);

  for (i = 0; i < s1->nb_priv_sections; i++)
    free_section(s1->priv_sections[i]);
  dynarray_reset(&s1->priv_sections, &s1->nb_priv_sections);

  tcc_free(s1->sym_attrs);
  symtab_section = NULL; /* for tccrun.c:rt_printline() */
}

/* save section data state */
ST_FUNC void tccelf_begin_file(TCCState *s1)
{
  Section *s;
  int i;
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    s->sh_offset = s->data_offset;
  }
  /* disable symbol hashing during compilation */
  s = s1->symtab, s->reloc = s->hash, s->hash = NULL;
#if defined TCC_TARGET_X86_64 && defined TCC_TARGET_PE
  s1->uw_sym = 0;
  s1->uw_offs = 0;
#endif
}

static void update_relocs(TCCState *s1, Section *s, int *old_to_new_syms, int first_sym);

/* At the end of compilation, convert any UNDEF syms to global, and merge
   with previously existing symbols */
ST_FUNC void tccelf_end_file(TCCState *s1)
{
  Section *s = s1->symtab;
  int first_sym, nb_syms, *tr, i;

  first_sym = s->sh_offset / sizeof(ElfSym);
  nb_syms = s->data_offset / sizeof(ElfSym) - first_sym;
  s->data_offset = s->sh_offset;
  s->link->data_offset = s->link->sh_offset;
  s->hash = s->reloc, s->reloc = NULL;
  tr = tcc_mallocz(nb_syms * sizeof *tr);

  for (i = 0; i < nb_syms; ++i)
  {
    ElfSym *sym = (ElfSym *)s->data + first_sym + i;
    if (sym->st_shndx == SHN_UNDEF)
    {
      int sym_bind = ELFW(ST_BIND)(sym->st_info);
      int sym_type = ELFW(ST_TYPE)(sym->st_info);
      if (sym_bind == STB_LOCAL)
        sym_bind = STB_GLOBAL;
#ifndef TCC_TARGET_PE
      if (sym_bind == STB_GLOBAL && s1->output_type == TCC_OUTPUT_OBJ)
      {
        /* undefined symbols with STT_FUNC are confusing gnu ld when
           linking statically to STT_GNU_IFUNC */
        sym_type = STT_NOTYPE;
      }
#endif
      sym->st_info = ELFW(ST_INFO)(sym_bind, sym_type);
    }
    tr[i] = set_elf_sym(s, sym->st_value, sym->st_size, sym->st_info, sym->st_other, sym->st_shndx,
                        (char *)s->link->data + sym->st_name);
  }
  /* now update relocations */
  update_relocs(s1, s, tr, first_sym);
  tcc_free(tr);
  /* record text/data/bss output for -bench info */
  for (i = 0; i < 4; ++i)
  {
    s = s1->sections[i + 1];
    s1->total_output[i] += s->data_offset - s->sh_offset;
  }
}

ST_FUNC Section *new_section(TCCState *s1, const char *name, int sh_type, int sh_flags)
{
  Section *sec;

  sec = tcc_mallocz(sizeof(Section) + strlen(name));
  sec->s1 = s1;
  strcpy(sec->name, name);
  sec->sh_type = sh_type;
  sec->sh_flags = sh_flags;
  switch (sh_type)
  {
  case SHT_GNU_versym:
    sec->sh_addralign = 2;
    break;
  case SHT_HASH:
  case SHT_GNU_HASH:
  case SHT_REL:
  case SHT_RELA:
  case SHT_DYNSYM:
  case SHT_SYMTAB:
  case SHT_DYNAMIC:
  case SHT_GNU_verneed:
  case SHT_GNU_verdef:
    sec->sh_addralign = 8; // PTR_SIZE;
    break;
  case SHT_STRTAB:
    sec->sh_addralign = 1;
    break;
  default:
    sec->sh_addralign = 8; // PTR_SIZE; /* gcc/pcc default alignment */
    break;
  }

  if (sh_flags & SHF_PRIVATE)
  {
    dynarray_add(&s1->priv_sections, &s1->nb_priv_sections, sec);
  }
  else
  {
    sec->sh_num = s1->nb_sections;
    dynarray_add(&s1->sections, &s1->nb_sections, sec);
  }

  return sec;
}

ST_FUNC void init_symtab(Section *s)
{
  int *ptr, nb_buckets = 1;
  put_elf_str(s->link, "");
  section_ptr_add(s, sizeof(ElfW(Sym)));
  ptr = section_ptr_add(s->hash, (2 + nb_buckets + 1) * sizeof(int));
  ptr[0] = nb_buckets;
  ptr[1] = 1;
  memset(ptr + 2, 0, (nb_buckets + 1) * sizeof(int));
}

ST_FUNC Section *new_symtab(TCCState *s1, const char *symtab_name, int sh_type, int sh_flags, const char *strtab_name,
                            const char *hash_name, int hash_sh_flags)
{
  Section *symtab, *strtab, *hash;
  symtab = new_section(s1, symtab_name, sh_type, sh_flags);
  symtab->sh_entsize = sizeof(ElfW(Sym));
  strtab = new_section(s1, strtab_name, SHT_STRTAB, sh_flags);
  symtab->link = strtab;
  hash = new_section(s1, hash_name, SHT_HASH, hash_sh_flags);
  hash->sh_entsize = sizeof(int);
  symtab->hash = hash;
  hash->link = symtab;
  init_symtab(symtab);
  return symtab;
}

/* realloc section and set its content to zero */
ST_FUNC void section_realloc(Section *sec, unsigned long new_size)
{
  unsigned long size;
  unsigned char *data;

  size = sec->data_allocated;
  if (size == 0)
  {
    /* First allocation: round up to power of 2 with minimum 256 bytes
       to reduce future reallocations */
    size = 256;
    while (size < new_size)
      size = size * 2;
  }
  else
  {
    while (size < new_size)
      size = size * 2;
  }
  data = tcc_realloc(sec->data, size);
  memset(data + sec->data_allocated, 0, size - sec->data_allocated);
  sec->data = data;
  sec->data_allocated = size;
}

/* reserve at least 'size' bytes aligned per 'align' in section
   'sec' from current offset, and return the aligned offset */
ST_FUNC size_t section_add(Section *sec, addr_t size, int align)
{
  size_t offset, offset1;

  offset = (sec->data_offset + align - 1) & -align;
  offset1 = offset + size;
  if (sec->sh_type != SHT_NOBITS && offset1 > sec->data_allocated)
    section_realloc(sec, offset1);
  sec->data_offset = offset1;
  if (align > sec->sh_addralign)
    sec->sh_addralign = align;
  return offset;
}

/* reserve at least 'size' bytes in section 'sec' from
   sec->data_offset. */
ST_FUNC void *section_ptr_add(Section *sec, addr_t size)
{
  size_t offset = section_add(sec, size, 1);
  return sec->data + offset;
}

/* Pre-allocate section capacity without changing data_offset.
   Use this when you know the total size needed to avoid multiple reallocations. */
ST_FUNC void section_prealloc(Section *sec, unsigned long size)
{
  unsigned long needed = sec->data_offset + size;
  if (needed > sec->data_allocated)
    section_realloc(sec, needed);
}

#ifndef ELF_OBJ_ONLY
/* reserve at least 'size' bytes from section start */
static void section_reserve(Section *sec, unsigned long size)
{
  if (size > sec->data_allocated)
    section_realloc(sec, size);
  if (size > sec->data_offset)
    sec->data_offset = size;
}
#endif

static Section *have_section(TCCState *s1, const char *name)
{
  Section *sec;
  int i;
  for (i = 1; i < s1->nb_sections; i++)
  {
    sec = s1->sections[i];
    if (!strcmp(name, sec->name))
      return sec;
  }
  return NULL;
}

/* return a reference to a section, and create it if it does not
   exists */
ST_FUNC Section *find_section(TCCState *s1, const char *name)
{
  Section *sec = have_section(s1, name);
  if (sec)
    return sec;
  /* sections are created as PROGBITS */
  return new_section(s1, name, SHT_PROGBITS, SHF_ALLOC);
}

/* ------------------------------------------------------------------------- */

/* String table deduplication hash table functions - DISABLED due to issues */
#if 0
/* Initialize hash table for string deduplication in a section */
static void strtab_init_hash(Section *s)
{
    if (s->str_hash)
        return;
    s->str_hash_size = 256;
    s->str_hash = tcc_mallocz(s->str_hash_size * sizeof(uint32_t));
    s->str_hash_count = 0;
}

static uint32_t str_hash_func(const char *str)
{
    uint32_t h = 5381;
    int c;
    while ((c = *str++))
        h = ((h << 5) + h) + c;
    return h;
}

static int strtab_find(Section *s, const char *str, uint32_t hash)
{
    /* ... */
    return -1;
}

static void strtab_insert(Section *s, const char *str, uint32_t offset, uint32_t hash)
{
    /* ... */
}
#endif

ST_FUNC int put_elf_str(Section *s, const char *sym)
{
  int offset, len;
  char *ptr;

  if (!sym)
    sym = "";
  len = strlen(sym) + 1;
  offset = s->data_offset;
  ptr = section_ptr_add(s, len);
  memmove(ptr, sym, len);
  return offset;
}

/* elf symbol hashing function */
static ElfW(Word) elf_hash(const unsigned char *name)
{
  ElfW(Word) h = 0, g;

  while (*name)
  {
    h = (h << 4) + *name++;
    g = h & 0xf0000000;
    if (g)
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

/* rebuild hash table of section s */
/* NOTE: we do factorize the hash table code to go faster */
static void rebuild_hash(Section *s, unsigned int nb_buckets)
{
  ElfW(Sym) * sym;
  int *ptr, *hash, nb_syms, sym_index, h;
  unsigned char *strtab;

  strtab = s->link->data;
  nb_syms = s->data_offset / sizeof(ElfW(Sym));

  if (!nb_buckets)
    nb_buckets = ((int *)s->hash->data)[0];

  s->hash->data_offset = 0;
  ptr = section_ptr_add(s->hash, (2 + nb_buckets + nb_syms) * sizeof(int));
  ptr[0] = nb_buckets;
  ptr[1] = nb_syms;
  ptr += 2;
  hash = ptr;
  memset(hash, 0, (nb_buckets + 1) * sizeof(int));
  ptr += nb_buckets + 1;

  sym = (ElfW(Sym) *)s->data + 1;
  for (sym_index = 1; sym_index < nb_syms; sym_index++)
  {
    if (ELFW(ST_BIND)(sym->st_info) != STB_LOCAL)
    {
      h = elf_hash(strtab + sym->st_name) % nb_buckets;
      *ptr = hash[h];
      hash[h] = sym_index;
    }
    else
    {
      *ptr = 0;
    }
    ptr++;
    sym++;
  }
}

/* return the symbol number */
ST_FUNC int put_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name)
{
  int name_offset, sym_index;
  int nbuckets, h;
  ElfW(Sym) * sym;
  Section *hs;

  /* Validate name pointer - catch garbage early */
  if (name && name[0])
  {
    unsigned char first = (unsigned char)name[0];
    if (first < 0x20 || first > 0x7e)
    {
      /* name pointer contains garbage - treat as unnamed */
      name = NULL;
    }
  }

  sym = section_ptr_add(s, sizeof(ElfW(Sym)));
  if (name && name[0])
    name_offset = put_elf_str(s->link, name);
  else
    name_offset = 0;
  /* XXX: endianness */
  sym->st_name = name_offset;
  sym->st_value = value;
  sym->st_size = size;
  sym->st_info = info;
  sym->st_other = other;
  sym->st_shndx = shndx;
  sym_index = sym - (ElfW(Sym) *)s->data;
  hs = s->hash;
  if (hs)
  {
    int *ptr, *base;
    ptr = section_ptr_add(hs, sizeof(int));
    base = (int *)hs->data;
    /* only add global or weak symbols. */
    if (ELFW(ST_BIND)(info) != STB_LOCAL)
    {
      /* add another hashing entry */
      nbuckets = base[0];
      h = elf_hash((unsigned char *)s->link->data + name_offset) % nbuckets;
      *ptr = base[2 + h];
      base[2 + h] = sym_index;
      base[1]++;
      /* we resize the hash table */
      hs->nb_hashed_syms++;
      if (hs->nb_hashed_syms > 2 * nbuckets)
      {
        rebuild_hash(s, 2 * nbuckets);
      }
    }
    else
    {
      *ptr = 0;
      base[1]++;
    }
  }
  return sym_index;
}

ST_FUNC int find_elf_sym(Section *s, const char *name)
{
  ElfW(Sym) * sym;
  Section *hs;
  int nbuckets, sym_index, h;
  const char *name1;

  hs = s->hash;
  if (!hs)
    return 0;
  nbuckets = ((int *)hs->data)[0];
  h = elf_hash((unsigned char *)name) % nbuckets;
  sym_index = ((int *)hs->data)[2 + h];

  while (sym_index != 0)
  {
    sym = &((ElfW(Sym) *)s->data)[sym_index];
    name1 = (char *)s->link->data + sym->st_name;
    if (!strcmp(name, name1))
      return sym_index;
    sym_index = ((int *)hs->data)[2 + nbuckets + sym_index];
  }
  return 0;
}

/* return elf symbol value, signal error if 'err' is nonzero, decorate
   name if FORC */
ST_FUNC addr_t get_sym_addr(TCCState *s1, const char *name, int err, int forc)
{
  int sym_index;
  ElfW(Sym) * sym;
  char buf[256];
  if (forc &&
      s1->leading_underscore
#ifdef TCC_TARGET_PE
      /* win32-32bit stdcall symbols always have _ already */
      && !strchr(name, '@')
#endif
  )
  {
    buf[0] = '_';
    pstrcpy(buf + 1, sizeof(buf) - 1, name);
    name = buf;
  }
  sym_index = find_elf_sym(s1->symtab, name);
  sym = &((ElfW(Sym) *)s1->symtab->data)[sym_index];
  if (!sym_index || sym->st_shndx == SHN_UNDEF)
  {
    if (err)
      tcc_error_noabort("%s not defined", name);
    return (addr_t)-1;
  }
  return sym->st_value;
}

/* return elf symbol value */
LIBTCCAPI void *tcc_get_symbol(TCCState *s, const char *name)
{
  addr_t addr = get_sym_addr(s, name, 0, 1);
  return addr == -1 ? NULL : (void *)(uintptr_t)addr;
}

/* list elf symbol names and values */
ST_FUNC void list_elf_symbols(TCCState *s, void *ctx, void (*symbol_cb)(void *ctx, const char *name, const void *val))
{
  ElfW(Sym) * sym;
  Section *symtab;
  int sym_index, end_sym;
  const char *name;
  unsigned char sym_vis, sym_bind;

  symtab = s->symtab;
  end_sym = symtab->data_offset / sizeof(ElfSym);
  for (sym_index = 0; sym_index < end_sym; ++sym_index)
  {
    sym = &((ElfW(Sym) *)symtab->data)[sym_index];
    if (sym->st_value)
    {
      name = (char *)symtab->link->data + sym->st_name;
      sym_bind = ELFW(ST_BIND)(sym->st_info);
      sym_vis = ELFW(ST_VISIBILITY)(sym->st_other);
      if (sym_bind == STB_GLOBAL && sym_vis == STV_DEFAULT)
        symbol_cb(ctx, name, (void *)(uintptr_t)sym->st_value);
    }
  }
}

/* list elf symbol names and values */
LIBTCCAPI void tcc_list_symbols(TCCState *s, void *ctx, void (*symbol_cb)(void *ctx, const char *name, const void *val))
{
  list_elf_symbols(s, ctx, symbol_cb);
}

#ifndef ELF_OBJ_ONLY
static void version_add(TCCState *s1)
{
  int i;
  ElfW(Sym) * sym;
  ElfW(Verneed) *vn = NULL;
  Section *symtab;
  int sym_index, end_sym, nb_versions = 2, nb_entries = 0;
  ElfW(Half) * versym;
  const char *name;

  if (0 == nb_sym_versions)
    return;
  versym_section = new_section(s1, ".gnu.version", SHT_GNU_versym, SHF_ALLOC);
  versym_section->sh_entsize = sizeof(ElfW(Half));
  versym_section->link = s1->dynsym;

  /* add needed symbols */
  symtab = s1->dynsym;
  end_sym = symtab->data_offset / sizeof(ElfSym);
  versym = section_ptr_add(versym_section, end_sym * sizeof(ElfW(Half)));
  for (sym_index = 1; sym_index < end_sym; ++sym_index)
  {
    int dllindex, verndx;
    sym = &((ElfW(Sym) *)symtab->data)[sym_index];
    name = (char *)symtab->link->data + sym->st_name;
    dllindex = find_elf_sym(s1->dynsymtab_section, name);
    verndx = (dllindex && dllindex < nb_sym_to_version) ? sym_to_version[dllindex] : -1;
    if (verndx >= 0
        /* XXX: on android, clang refuses to link with a libtcc.so made by tcc
           when defined symbols have a version > 1 or when the version is '0'.
           Whereas version '1' for example for 'signal' in an exe defeats
           bcheck's signal_redir. */
        && (sym->st_shndx == SHN_UNDEF || (s1->output_type & TCC_OUTPUT_EXE)))
    {
      if (!sym_versions[verndx].out_index)
        sym_versions[verndx].out_index = nb_versions++;
      versym[sym_index] = sym_versions[verndx].out_index;
    }
    else
    {
      versym[sym_index] = 1; /* (*global*) */
    }
    // printf("SYM %d %s\n", versym[sym_index], name);
  }
  /* generate verneed section, but not when it will be empty.  Some
     dynamic linkers look at their contents even when DTVERNEEDNUM and
     section size is zero.  */
  if (nb_versions > 2)
  {
    verneed_section = new_section(s1, ".gnu.version_r", SHT_GNU_verneed, SHF_ALLOC);
    verneed_section->link = s1->dynsym->link;
    for (i = nb_sym_versions; i-- > 0;)
    {
      struct sym_version *sv = &sym_versions[i];
      int n_same_libs = 0, prev;
      size_t vnofs;
      ElfW(Vernaux) *vna = 0;
      if (sv->out_index < 1)
        continue;

      /* make sure that a DT_NEEDED tag is put */
      /* abitest-tcc fails on older i386-linux with "ld-linux.so.2" DT_NEEDED
         ret_int_test... Inconsistency detected by ld.so: dl-minimal.c: 148:
         realloc: Assertion `ptr == alloc_last_block' failed! */
      if (strcmp(sv->lib, "ld-linux.so.2"))
        tcc_add_dllref(s1, sv->lib, 0);

      vnofs = section_add(verneed_section, sizeof(*vn), 1);
      vn = (ElfW(Verneed) *)(verneed_section->data + vnofs);
      vn->vn_version = 1;
      vn->vn_file = put_elf_str(verneed_section->link, sv->lib);
      vn->vn_aux = sizeof(*vn);
      do
      {
        prev = sv->prev_same_lib;
        if (sv->out_index > 0)
        {
          vna = section_ptr_add(verneed_section, sizeof(*vna));
          vna->vna_hash = elf_hash((const unsigned char *)sv->version);
          vna->vna_flags = 0;
          vna->vna_other = sv->out_index;
          sv->out_index = -2;
          vna->vna_name = put_elf_str(verneed_section->link, sv->version);
          vna->vna_next = sizeof(*vna);
          // printf("LIB %d %s %s\n", vna->vna_other, sv->lib,
          // verneed_section->link->data + vna->vna_name);
          n_same_libs++;
        }
        if (prev >= 0)
          sv = &sym_versions[prev];
      } while (prev >= 0);
      vna->vna_next = 0;
      vn = (ElfW(Verneed) *)(verneed_section->data + vnofs);
      vn->vn_cnt = n_same_libs;
      vn->vn_next = sizeof(*vn) + n_same_libs * sizeof(*vna);
      nb_entries++;
    }
    if (vn)
      vn->vn_next = 0;
    verneed_section->sh_info = nb_entries;
  }
  dt_verneednum = nb_entries;
}
#endif /* ndef ELF_OBJ_ONLY */

/* add an elf symbol : check if it is already defined and patch
   it. Return symbol index. NOTE that sh_num can be SHN_UNDEF. */
ST_FUNC int set_elf_sym(Section *s, addr_t value, unsigned long size, int info, int other, int shndx, const char *name)
{
  TCCState *s1 = s->s1;
  ElfW(Sym) * esym;
  int sym_bind, sym_index, sym_type, esym_bind;
  unsigned char sym_vis, esym_vis, new_vis;

  sym_bind = ELFW(ST_BIND)(info);
  sym_type = ELFW(ST_TYPE)(info);
  sym_vis = ELFW(ST_VISIBILITY)(other);

  if (sym_bind != STB_LOCAL)
  {
    /* we search global or weak symbols */
    sym_index = find_elf_sym(s, name);
    if (!sym_index)
      goto do_def;
    esym = &((ElfW(Sym) *)s->data)[sym_index];
    if (esym->st_value == value && esym->st_size == size && esym->st_info == info && esym->st_other == other &&
        esym->st_shndx == shndx)
      return sym_index;
    if (esym->st_shndx != SHN_UNDEF)
    {
      esym_bind = ELFW(ST_BIND)(esym->st_info);
      /* propagate the most constraining visibility */
      /* STV_DEFAULT(0)<STV_PROTECTED(3)<STV_HIDDEN(2)<STV_INTERNAL(1) */
      esym_vis = ELFW(ST_VISIBILITY)(esym->st_other);
      if (esym_vis == STV_DEFAULT)
      {
        new_vis = sym_vis;
      }
      else if (sym_vis == STV_DEFAULT)
      {
        new_vis = esym_vis;
      }
      else
      {
        new_vis = (esym_vis < sym_vis) ? esym_vis : sym_vis;
      }
      esym->st_other = (esym->st_other & ~ELFW(ST_VISIBILITY)(-1)) | new_vis;
      if (shndx == SHN_UNDEF)
      {
        /* ignore adding of undefined symbol if the
           corresponding symbol is already defined */
      }
      else if (sym_bind == STB_GLOBAL && esym_bind == STB_WEAK)
      {
        /* global overrides weak, so patch */
        goto do_patch;
      }
      else if (sym_bind == STB_WEAK && esym_bind == STB_GLOBAL)
      {
        /* weak is ignored if already global */
      }
      else if (sym_bind == STB_WEAK && esym_bind == STB_WEAK)
      {
        /* keep first-found weak definition, ignore subsequents */
      }
      else if (sym_vis == STV_HIDDEN || sym_vis == STV_INTERNAL)
      {
        /* ignore hidden symbols after */
      }
      else if ((esym->st_shndx == SHN_COMMON || esym->st_shndx == bss_section->sh_num) &&
               (shndx < SHN_LORESERVE && shndx != bss_section->sh_num))
      {
        /* data symbol gets precedence over common/bss */
        goto do_patch;
      }
      else if (shndx == SHN_COMMON || shndx == bss_section->sh_num)
      {
        /* data symbol keeps precedence over common/bss */
      }
      else if (s->sh_flags & SHF_DYNSYM)
      {
        /* we accept that two DLL define the same symbol */
      }
      else if (esym->st_other & ST_ASM_SET)
      {
        /* If the existing symbol came from an asm .set
           we can override.  */
        goto do_patch;
      }
      else
      {
#if 0
                printf("new_bind=%x new_shndx=%x new_vis=%x old_bind=%x old_shndx=%x old_vis=%x\n",
                       sym_bind, shndx, new_vis, esym_bind, esym->st_shndx, esym_vis);
#endif
        tcc_error_noabort("'%s' defined twice", name);
      }
    }
    else
    {
      esym->st_other = other;
    do_patch:
      esym->st_info = ELFW(ST_INFO)(sym_bind, sym_type);
      esym->st_shndx = shndx;
      s1->new_undef_sym = 1;
      esym->st_value = value;
      esym->st_size = size;
    }
  }
  else
  {
  do_def:
    sym_index = put_elf_sym(s, value, size, ELFW(ST_INFO)(sym_bind, sym_type), other, shndx, name);
  }
  return sym_index;
}

/* put relocation */
ST_FUNC void put_elf_reloca(Section *symtab, Section *s, unsigned long offset, int type, int symbol, addr_t addend)
{
  TCCState *s1 = s->s1;
  char buf[256];
  Section *sr;
  ElfW_Rel *rel;

  /* Validate symbol index */
  int num_syms = symtab->data_offset / sizeof(ElfW(Sym));
  if (symbol < 0 || symbol >= num_syms)
  {
    return; /* Skip invalid symbol index */
  }

  sr = s->reloc;
  if (!sr)
  {
    /* if no relocation section, create it */
    snprintf(buf, sizeof(buf), REL_SECTION_FMT, s->name);
    /* if the symtab is allocated, then we consider the relocation
       are also */
    sr = new_section(s->s1, buf, SHT_RELX, symtab->sh_flags);
    sr->sh_entsize = sizeof(ElfW_Rel);
    sr->link = symtab;
    sr->sh_info = s->sh_num;
    s->reloc = sr;
  }
  rel = section_ptr_add(sr, sizeof(ElfW_Rel));
  rel->r_offset = offset;
  rel->r_info = ELFW(R_INFO)(symbol, type);
#if SHT_RELX == SHT_RELA
  rel->r_addend = addend;
#endif
  if (SHT_RELX != SHT_RELA && addend)
    tcc_error_noabort("non-zero addend on REL architecture");
}

ST_FUNC void put_elf_reloc(Section *symtab, Section *s, unsigned long offset, int type, int symbol)
{
  put_elf_reloca(symtab, s, offset, type, symbol, 0);
}

ST_FUNC struct sym_attr *get_sym_attr(TCCState *s1, int index, int alloc)
{
  int n;
  struct sym_attr *tab;

  if (index >= s1->nb_sym_attrs)
  {
    if (!alloc)
      return s1->sym_attrs;
    /* find immediately bigger power of 2 and reallocate array */
    n = 1;
    while (index >= n)
      n *= 2;
    tab = tcc_realloc(s1->sym_attrs, n * sizeof(*s1->sym_attrs));
    s1->sym_attrs = tab;
    memset(s1->sym_attrs + s1->nb_sym_attrs, 0, (n - s1->nb_sym_attrs) * sizeof(*s1->sym_attrs));
    s1->nb_sym_attrs = n;
  }
  return &s1->sym_attrs[index];
}

static void update_relocs(TCCState *s1, Section *s, int *old_to_new_syms, int first_sym)
{
  int i, type, sym_index;
  Section *sr;
  ElfW_Rel *rel;

  for (i = 1; i < s1->nb_sections; i++)
  {
    sr = s1->sections[i];
    if (sr->sh_type == SHT_RELX && sr->link == s)
    {
      for_each_elem(sr, 0, rel, ElfW_Rel)
      {
        sym_index = ELFW(R_SYM)(rel->r_info);
        type = ELFW(R_TYPE)(rel->r_info);
        if ((sym_index -= first_sym) < 0)
          continue; /* zero sym_index in reloc (can happen with asm) */
        sym_index = old_to_new_syms[sym_index];
        rel->r_info = ELFW(R_INFO)(sym_index, type);
      }
    }
  }
}

/* In an ELF file symbol table, the local symbols must appear below
   the global and weak ones. Since TCC cannot sort it while generating
   the code, we must do it after. All the relocation tables are also
   modified to take into account the symbol table sorting */
ST_FUNC void tcc_elf_sort_syms(TCCState *s1, Section *s)
{
  int *old_to_new_syms;
  ElfW(Sym) * new_syms;
  int nb_syms, i;
  ElfW(Sym) * p, *q;

  nb_syms = s->data_offset / sizeof(ElfW(Sym));
  new_syms = tcc_malloc(nb_syms * sizeof(ElfW(Sym)));
  old_to_new_syms = tcc_malloc(nb_syms * sizeof(int));

  /* first pass for local symbols */
  p = (ElfW(Sym) *)s->data;
  q = new_syms;
  for (i = 0; i < nb_syms; i++)
  {
    if (ELFW(ST_BIND)(p->st_info) == STB_LOCAL)
    {
      old_to_new_syms[i] = q - new_syms;
      *q++ = *p;
    }
    p++;
  }
  /* save the number of local symbols in section header */
  if (s->sh_size) /* this 'if' makes IDA happy */
    s->sh_info = q - new_syms;

  /* then second pass for non local symbols */
  p = (ElfW(Sym) *)s->data;
  for (i = 0; i < nb_syms; i++)
  {
    if (ELFW(ST_BIND)(p->st_info) != STB_LOCAL)
    {
      old_to_new_syms[i] = q - new_syms;
      *q++ = *p;
    }
    p++;
  }

  /* we copy the new symbols to the old */
  memcpy(s->data, new_syms, nb_syms * sizeof(ElfW(Sym)));
  tcc_free(new_syms);

  update_relocs(s1, s, old_to_new_syms, 0);
  tcc_free(old_to_new_syms);
}

#ifndef ELF_OBJ_ONLY
/* See: https://flapenguin.me/elf-dt-gnu-hash */
#define ELFCLASS_BITS (PTR_SIZE * 8)

static Section *create_gnu_hash(TCCState *s1)
{
  int nb_syms, i, ndef, nbuckets, symoffset, bloom_size, bloom_shift;
  ElfW(Sym) * p;
  Section *gnu_hash;
  Section *dynsym = s1->dynsym;
  Elf32_Word *ptr;

  gnu_hash = new_section(s1, ".gnu.hash", SHT_GNU_HASH, SHF_ALLOC);
  gnu_hash->link = dynsym->hash->link;

  nb_syms = dynsym->data_offset / sizeof(ElfW(Sym));

  /* count def symbols */
  ndef = 0;
  p = (ElfW(Sym) *)dynsym->data;
  for (i = 0; i < nb_syms; i++, p++)
    ndef += p->st_shndx != SHN_UNDEF;

  /* calculate gnu hash sizes and fill header */
  nbuckets = ndef / 4 + 1;
  symoffset = nb_syms - ndef;
  bloom_shift = PTR_SIZE == 8 ? 6 : 5;
  bloom_size = 1; /* must be power of two */
  while (ndef >= bloom_size * (1 << (bloom_shift - 3)))
    bloom_size *= 2;
  ptr = section_ptr_add(gnu_hash, 4 * 4 + PTR_SIZE * bloom_size + nbuckets * 4 + ndef * 4);
  ptr[0] = nbuckets;
  ptr[1] = symoffset;
  ptr[2] = bloom_size;
  ptr[3] = bloom_shift;
  return gnu_hash;
}

static Elf32_Word elf_gnu_hash(const unsigned char *name)
{
  Elf32_Word h = 5381;
  unsigned char c;

  while ((c = *name++))
    h = h * 33 + c;
  return h;
}

static void update_gnu_hash(TCCState *s1, Section *gnu_hash)
{
  int *old_to_new_syms;
  ElfW(Sym) * new_syms;
  int nb_syms, i, nbuckets, bloom_size, bloom_shift;
  ElfW(Sym) * p, *q;
  Section *vs;
  Section *dynsym = s1->dynsym;
  Elf32_Word *ptr, *buckets, *chain, *hash;
  unsigned int *nextbuck;
  addr_t *bloom;
  unsigned char *strtab;
  struct
  {
    int first, last;
  } *buck;

  strtab = dynsym->link->data;
  nb_syms = dynsym->data_offset / sizeof(ElfW(Sym));
  new_syms = tcc_malloc(nb_syms * sizeof(ElfW(Sym)));
  old_to_new_syms = tcc_malloc(nb_syms * sizeof(int));
  hash = tcc_malloc(nb_syms * sizeof(Elf32_Word));
  nextbuck = tcc_malloc(nb_syms * sizeof(int));

  /* calculate hashes and copy undefs */
  p = (ElfW(Sym) *)dynsym->data;
  q = new_syms;
  for (i = 0; i < nb_syms; i++, p++)
  {
    if (p->st_shndx == SHN_UNDEF)
    {
      old_to_new_syms[i] = q - new_syms;
      *q++ = *p;
    }
    else
      hash[i] = elf_gnu_hash(strtab + p->st_name);
  }

  ptr = (Elf32_Word *)gnu_hash->data;
  nbuckets = ptr[0];
  bloom_size = ptr[2];
  bloom_shift = ptr[3];
  bloom = (addr_t *)(void *)&ptr[4];
  buckets = (Elf32_Word *)(void *)&bloom[bloom_size];
  chain = &buckets[nbuckets];
  buck = tcc_malloc(nbuckets * sizeof(*buck));

  if (gnu_hash->data_offset != 4 * 4 + PTR_SIZE * bloom_size + nbuckets * 4 + (nb_syms - (q - new_syms)) * 4)
    tcc_error_noabort("gnu_hash size incorrect");

  /* find buckets */
  for (i = 0; i < nbuckets; i++)
    buck[i].first = -1;

  p = (ElfW(Sym) *)dynsym->data;
  for (i = 0; i < nb_syms; i++, p++)
    if (p->st_shndx != SHN_UNDEF)
    {
      int bucket = hash[i] % nbuckets;

      if (buck[bucket].first == -1)
        buck[bucket].first = buck[bucket].last = i;
      else
      {
        nextbuck[buck[bucket].last] = i;
        buck[bucket].last = i;
      }
    }

  /* fill buckets/chains/bloom and sort symbols */
  p = (ElfW(Sym) *)dynsym->data;
  for (i = 0; i < nbuckets; i++)
  {
    int cur = buck[i].first;

    if (cur != -1)
    {
      buckets[i] = q - new_syms;
      for (;;)
      {
        old_to_new_syms[cur] = q - new_syms;
        *q++ = p[cur];
        *chain++ = hash[cur] & ~1;
        bloom[(hash[cur] / ELFCLASS_BITS) % bloom_size] |=
            (addr_t)1 << (hash[cur] % ELFCLASS_BITS) | (addr_t)1 << ((hash[cur] >> bloom_shift) % ELFCLASS_BITS);
        if (cur == buck[i].last)
          break;
        cur = nextbuck[cur];
      }
      chain[-1] |= 1;
    }
  }

  memcpy(dynsym->data, new_syms, nb_syms * sizeof(ElfW(Sym)));
  tcc_free(new_syms);
  tcc_free(hash);
  tcc_free(buck);
  tcc_free(nextbuck);

  update_relocs(s1, dynsym, old_to_new_syms, 0);

  /* modify the versions */
  vs = versym_section;
  if (vs)
  {
    ElfW(Half) * newver, *versym = (ElfW(Half) *)vs->data;

    if (1 /*versym*/)
    {
      newver = tcc_malloc(nb_syms * sizeof(*newver));
      for (i = 0; i < nb_syms; i++)
        newver[old_to_new_syms[i]] = versym[i];
      memcpy(vs->data, newver, nb_syms * sizeof(*newver));
      tcc_free(newver);
    }
  }

  tcc_free(old_to_new_syms);

  /* rebuild hash */
  ptr = (Elf32_Word *)dynsym->hash->data;
  rebuild_hash(dynsym, ptr[0]);
}
#endif /* ELF_OBJ_ONLY */

/* relocate symbol table, resolve undefined symbols if do_resolve is
   true and output error if undefined symbol. */
ST_FUNC void relocate_syms(TCCState *s1, Section *symtab, int do_resolve)
{
  ElfW(Sym) * sym;
  int sym_bind, sh_num;
  const char *name;
  int sym_idx = 0;

  for_each_elem(symtab, 1, sym, ElfW(Sym))
  {
    sym_idx++;
    sh_num = sym->st_shndx;
    if (sh_num == SHN_UNDEF)
    {
      if (do_resolve == 2) /* relocating dynsym */
        continue;
      /* Validate st_name offset before using it */
      if (sym->st_name >= s1->symtab->link->data_offset)
      {
        tcc_error_noabort("internal error: symbol %d has invalid st_name offset 0x%x (strtab size: 0x%lx)", sym_idx,
                          sym->st_name, (unsigned long)s1->symtab->link->data_offset);
        continue;
      }
      name = (char *)s1->symtab->link->data + sym->st_name;
      /* Debug: print symbol info when name is empty or looks wrong */
      /* Use ld.so to resolve symbol for us (for tcc -run) */
      if (do_resolve)
      {
#if defined TCC_IS_NATIVE && !defined TCC_TARGET_PE
        /* dlsym() needs the undecorated name.  */
        void *addr = dlsym(RTLD_DEFAULT, &name[s1->leading_underscore]);
#if TARGETOS_OpenBSD || TARGETOS_FreeBSD || TARGETOS_NetBSD || TARGETOS_ANDROID || TARGETOS_YasOS
        if (addr == NULL)
        {
          int i;
          for (i = 0; i < s1->nb_loaded_dlls; i++)
            if ((addr = dlsym(s1->loaded_dlls[i]->handle, name)))
              break;
        }
#endif
        if (addr)
        {
          sym->st_value = (addr_t)addr;
#ifdef DEBUG_RELOC
          printf("relocate_sym: %s -> 0x%lx\n", name, sym->st_value);
#endif
          goto found;
        }
#endif
        /* if dynamic symbol exist, it will be used in relocate_section */
      }
      else if (s1->dynsym && find_elf_sym(s1->dynsym, name))
        goto found;
      /* XXX: _fp_hw seems to be part of the ABI, so we ignore
         it */
      if (!strcmp(name, "_fp_hw"))
        goto found;
      /* only weak symbols are accepted to be undefined. Their
         value is zero */
      sym_bind = ELFW(ST_BIND)(sym->st_info);
      if (sym_bind == STB_WEAK)
        sym->st_value = 0;
      else
        tcc_error_noabort("undefined symbol '%s'", name);
    }
    else if (sh_num < SHN_LORESERVE)
    {
      /* add section base */
      sym->st_value += s1->sections[sym->st_shndx]->sh_addr;
    }
  found:;
  }
}
/* Add a relocation patch for lazy section streaming.
 * Uses dynamic arrays instead of linked list for memory efficiency.
 * Each patch is 8 bytes (2 x uint32_t) vs 24 bytes with linked list. */
static void add_reloc_patch(Section *s, uint32_t offset, uint32_t value)
{
  /* Ensure capacity */
  if (s->nb_reloc_patches >= s->alloc_reloc_patches)
  {
    int new_alloc = s->alloc_reloc_patches ? s->alloc_reloc_patches * 2 : 16;
    s->reloc_patch_offsets = tcc_realloc(s->reloc_patch_offsets, new_alloc * sizeof(uint32_t));
    s->reloc_patch_values = tcc_realloc(s->reloc_patch_values, new_alloc * sizeof(uint32_t));
    s->alloc_reloc_patches = new_alloc;
  }
  /* Append patch */
  s->reloc_patch_offsets[s->nb_reloc_patches] = offset;
  s->reloc_patch_values[s->nb_reloc_patches] = value;
  s->nb_reloc_patches++;
}

/* Free all relocation patches for a section */
static void free_reloc_patches(Section *s)
{
  tcc_free(s->reloc_patch_offsets);
  tcc_free(s->reloc_patch_values);
  s->reloc_patch_offsets = NULL;
  s->reloc_patch_values = NULL;
  s->nb_reloc_patches = 0;
  s->alloc_reloc_patches = 0;
}

/* relocate a given section (CPU dependent) by applying the relocations
   in the associated relocation section */
static void relocate_section(TCCState *s1, Section *s, Section *sr)
{
  ElfW_Rel *rel;
  ElfW(Sym) * sym;
  int type, sym_index;
  unsigned char *ptr;
  addr_t tgt, addr;
  int is_dwarf = s->sh_num >= s1->dwlo && s->sh_num < s1->dwhi;

  /* Always materialize non-debug sections */
  if (!is_dwarf)
    section_ensure_loaded(s1, s);

  section_ensure_loaded(s1, sr);
  section_ensure_loaded(s1, symtab_section);

  /* For lazy debug sections, we store patches instead of materializing */
  if (is_dwarf && s->lazy && !s->materialized)
  {
    for_each_elem(sr, 0, rel, ElfW_Rel)
    {
      sym_index = ELFW(R_SYM)(rel->r_info);
      sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
      type = ELFW(R_TYPE)(rel->r_info);
      tgt = sym->st_value;
#if SHT_RELX == SHT_RELA
      tgt += rel->r_addend;
#endif
      if (type == R_DATA_32DW && sym->st_shndx >= s1->dwlo && sym->st_shndx < s1->dwhi)
      {
        /* dwarf section relocation - store patch for streaming */
        uint32_t value = tgt - s1->sections[sym->st_shndx]->sh_addr;
        add_reloc_patch(s, (uint32_t)rel->r_offset, value);
      }
      /* Other relocation types would require materialization - skip for now */
    }
    return;
  }

  qrel = (ElfW_Rel *)sr->data;

  for_each_elem(sr, 0, rel, ElfW_Rel)
  {
    ptr = s->data + rel->r_offset;
    sym_index = ELFW(R_SYM)(rel->r_info);
    sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
    type = ELFW(R_TYPE)(rel->r_info);
    tgt = sym->st_value;
#if SHT_RELX == SHT_RELA
    tgt += rel->r_addend;
#endif
    if (is_dwarf && type == R_DATA_32DW && sym->st_shndx >= s1->dwlo && sym->st_shndx < s1->dwhi)
    {
      /* dwarf section relocation to each other */
      add32le(ptr, tgt - s1->sections[sym->st_shndx]->sh_addr);
      continue;
    }
    addr = s->sh_addr + rel->r_offset;
    relocate(s1, rel, type, ptr, addr, tgt);
  }
#ifndef ELF_OBJ_ONLY
  /* if the relocation is allocated, we change its symbol table */
  if (sr->sh_flags & SHF_ALLOC)
  {
    sr->link = s1->dynsym;
    if (s1->output_type & TCC_OUTPUT_DYN)
    {
      size_t r = (uint8_t *)qrel - sr->data;
      sr->data_offset = sr->sh_size = r;
#ifdef CONFIG_TCC_PIE
      if (r && (s->sh_flags & SHF_EXECINSTR))
        tcc_warning("%d relocations to %s", (unsigned)(r / sizeof *qrel), s->name);
#endif
    }
  }
#endif
}

/* relocate all sections */
ST_FUNC void relocate_sections(TCCState *s1)
{
  int i;
  Section *s, *sr;

  for (i = 1; i < s1->nb_sections; ++i)
  {
    sr = s1->sections[i];
    if (sr->sh_type != SHT_RELX)
      continue;
    s = s1->sections[sr->sh_info];
#ifndef TCC_TARGET_MACHO
    if (s != s1->got || s1->static_link || s1->output_type == TCC_OUTPUT_MEMORY)
#endif
    {
      relocate_section(s1, s, sr);
    }
#ifndef ELF_OBJ_ONLY
    if (sr->sh_flags & SHF_ALLOC)
    {
      ElfW_Rel *rel;
      /* relocate relocation table in 'sr' */
      for_each_elem(sr, 0, rel, ElfW_Rel) rel->r_offset += s->sh_addr;
    }
#endif
  }
}

#ifndef ELF_OBJ_ONLY
/* count the number of dynamic relocations so that we can reserve
   their space */
static int prepare_dynamic_rel(TCCState *s1, Section *sr)
{
  int count = 0;
#if defined(TCC_TARGET_I386) || defined(TCC_TARGET_X86_64) || defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM64) ||  \
    defined(TCC_TARGET_RISCV64)
  ElfW_Rel *rel;
  for_each_elem(sr, 0, rel, ElfW_Rel)
  {
    int sym_index = ELFW(R_SYM)(rel->r_info);
    int type = ELFW(R_TYPE)(rel->r_info);
    switch (type)
    {
#if defined(TCC_TARGET_I386)
    case R_386_32:
      if (!get_sym_attr(s1, sym_index, 0)->dyn_index &&
          ((ElfW(Sym) *)symtab_section->data + sym_index)->st_shndx == SHN_UNDEF)
      {
        /* don't fixup unresolved (weak) symbols */
        rel->r_info = ELFW(R_INFO)(sym_index, R_386_RELATIVE);
        break;
      }
#elif defined(TCC_TARGET_X86_64)
    case R_X86_64_32:
    case R_X86_64_32S:
    case R_X86_64_64:
#elif defined(TCC_TARGET_ARM)
    case R_ARM_ABS32:
    case R_ARM_TARGET1:
#elif defined(TCC_TARGET_ARM64)
    case R_AARCH64_ABS32:
    case R_AARCH64_ABS64:
#elif defined(TCC_TARGET_RISCV64)
    case R_RISCV_32:
    case R_RISCV_64:
#endif
      count++;
      break;
#if defined(TCC_TARGET_I386)
    case R_386_PC32:
#elif defined(TCC_TARGET_X86_64)
    case R_X86_64_PC32:
    {
      ElfW(Sym) *sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
      /* Hidden defined symbols can and must be resolved locally.
         We're misusing a PLT32 reloc for this, as that's always
         resolved to its address even in shared libs.  */
      if (sym->st_shndx != SHN_UNDEF && ELFW(ST_VISIBILITY)(sym->st_other) == STV_HIDDEN)
      {
        rel->r_info = ELFW(R_INFO)(sym_index, R_X86_64_PLT32);
        break;
      }
    }
#elif defined(TCC_TARGET_ARM64)
    case R_AARCH64_PREL32:
#endif
      if (s1->output_type != TCC_OUTPUT_DLL)
        break;
      if (get_sym_attr(s1, sym_index, 0)->dyn_index)
        count++;
      break;
    default:
      break;
    }
  }
#endif
  return count;
}
#endif

#ifdef NEED_BUILD_GOT
int build_got(TCCState *s1)
{
  /* if no got, then create it */
  s1->got = new_section(s1, ".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
  s1->got->sh_entsize = 8;
  /* keep space for _DYNAMIC pointer and two dummy got entries */
  section_ptr_add(s1->got, 3 * PTR_SIZE * 2);
  return set_elf_sym(symtab_section, 0, 0, ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), 0, s1->got->sh_num,
                     "_GLOBAL_OFFSET_TABLE_");
}

/* Create a GOT and (for function call) a PLT entry corresponding to a symbol
   in s1->symtab. When creating the dynamic symbol table entry for the GOT
   relocation, use 'size' and 'info' for the corresponding symbol metadata.
   Returns the offset of the GOT or (if any) PLT entry. */
static struct sym_attr *put_got_entry(TCCState *s1, int dyn_reloc_type, int sym_index)
{
  int need_plt_entry;
  const char *name;
  ElfW(Sym) * sym;
  struct sym_attr *attr;
  unsigned got_offset;
  char plt_name[200];
  int len;
  Section *s_rel;

  need_plt_entry = (dyn_reloc_type == R_JMP_SLOT);
  attr = get_sym_attr(s1, sym_index, 1);

  /* In case a function is both called and its address taken 2 GOT entries
     are created, one for taking the address (GOT) and the other for the PLT
     entry (PLTGOT).  */
  if (need_plt_entry ? attr->plt_offset : attr->got_offset)
    return attr;

  s_rel = s1->got;
  if (need_plt_entry)
  {
    if (!s1->plt)
    {
      s1->plt = new_section(s1, ".plt", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
      s1->plt->sh_entsize = 4;
    }
    s_rel = s1->plt;
  }

  /* create the GOT entry */
  got_offset = s1->got->data_offset;
  section_ptr_add(s1->got, PTR_SIZE * 2);

  /* Create the GOT relocation that will insert the address of the object or
     function of interest in the GOT entry. This is a static relocation for
     memory output (dlsym will give us the address of symbols) and dynamic
     relocation otherwise (executable and DLLs). The relocation should be
     done lazily for GOT entry with *_JUMP_SLOT relocation type (the one
     associated to a PLT entry) but is currently done at load time for an
     unknown reason. */

  sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
  name = (char *)symtab_section->link->data + sym->st_name;
  // printf("sym %d %s\n", need_plt_entry, name);

  if (s1->dynsym)
  {
    if (ELFW(ST_BIND)(sym->st_info) == STB_LOCAL)
    {
      /* Hack alarm.  We don't want to emit dynamic symbols
         and symbol based relocs for STB_LOCAL symbols, but rather
         want to resolve them directly.  At this point the symbol
         values aren't final yet, so we must defer this.  We will later
         have to create a RELATIVE reloc anyway, so we misuse the
         relocation slot to smuggle the symbol reference until
         fill_local_got_entries.  Not that the sym_index is
         relative to symtab_section, not s1->dynsym!  Nevertheless
         we use s1->dyn_sym so that if this is the first call
         that got->reloc is correctly created.  Also note that
         RELATIVE relocs are not normally created for the .got,
         so the types serves as a marker for later (and is retained
         also for the final output, which is okay because then the
         got is just normal data).  */
      put_elf_reloc(s1->dynsym, s1->got, got_offset, R_RELATIVE, sym_index);
    }
    else
    {
      if (0 == attr->dyn_index)
        attr->dyn_index = set_elf_sym(s1->dynsym, sym->st_value, sym->st_size, sym->st_info, 0, sym->st_shndx, name);
      put_elf_reloc(s1->dynsym, s_rel, got_offset, dyn_reloc_type, attr->dyn_index);
    }
  }
  else
  {
    put_elf_reloc(symtab_section, s1->got, got_offset, dyn_reloc_type, sym_index);
  }

  if (need_plt_entry)
  {
    attr->plt_offset = create_plt_entry(s1, got_offset, attr);

    /* create a symbol 'sym@plt' for the PLT jump vector */
    len = strlen(name);
    if (len > sizeof plt_name - 5)
      len = sizeof plt_name - 5;
    memcpy(plt_name, name, len);
    strcpy(plt_name + len, "@plt");
    attr->plt_sym =
        put_elf_sym(s1->symtab, attr->plt_offset, 0, ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC), 0, s1->plt->sh_num, plt_name);
  }
  else
  {
    attr->got_offset = got_offset;
  }

  return attr;
}

/* build GOT and PLT entries */
/* Two passes because R_JMP_SLOT should become first. Some targets
   (arm, arm64) do not allow mixing R_JMP_SLOT and R_GLOB_DAT. */
ST_FUNC void build_got_entries(TCCState *s1, int got_sym)
{
  Section *s;
  ElfW_Rel *rel;
  ElfW(Sym) * sym;
  int i, type, gotplt_entry, reloc_type, sym_index;
  struct sym_attr *attr;
  int pass = 0;
redo:
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (s->sh_type != SHT_RELX)
      continue;
    /* no need to handle got relocations */
    if (s->link != symtab_section)
      continue;
    for_each_elem(s, 0, rel, ElfW_Rel)
    {
      type = ELFW(R_TYPE)(rel->r_info);
      gotplt_entry = gotplt_entry_type(type);
      if (gotplt_entry == -1)
      {
        tcc_error_noabort("Unknown relocation type for got: %d", type);
        continue;
      }
      sym_index = ELFW(R_SYM)(rel->r_info);
      sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];

      if (gotplt_entry == NO_GOTPLT_ENTRY)
      {
        continue;
      }

      /* Automatically create PLT/GOT [entry] if it is an undefined
         reference (resolved at runtime), or the symbol is absolute,
         probably created by tcc_add_symbol, and thus on 64-bit
         targets might be too far from application code.  */
      if (gotplt_entry == AUTO_GOTPLT_ENTRY)
      {
        if (sym->st_shndx == SHN_UNDEF)
        {
          ElfW(Sym) * esym;
          int dynindex;
          if (!PCRELATIVE_DLLPLT && (s1->output_type & TCC_OUTPUT_DYN))
            continue;
          /* Relocations for UNDEF symbols would normally need
             to be transferred into the executable or shared object.
             If that were done AUTO_GOTPLT_ENTRY wouldn't exist.
             But TCC doesn't do that (at least for exes), so we
             need to resolve all such relocs locally.  And that
             means PLT slots for functions in DLLs and COPY relocs for
             data symbols.  COPY relocs were generated in
             bind_exe_dynsyms (and the symbol adjusted to be defined),
             and for functions we were generated a dynamic symbol
             of function type.  */
          if (s1->dynsym)
          {
            /* dynsym isn't set for -run :-/  */
            dynindex = get_sym_attr(s1, sym_index, 0)->dyn_index;
            esym = (ElfW(Sym) *)s1->dynsym->data + dynindex;
            if (dynindex && (ELFW(ST_TYPE)(esym->st_info) == STT_FUNC ||
                             (ELFW(ST_TYPE)(esym->st_info) == STT_NOTYPE && ELFW(ST_TYPE)(sym->st_info) == STT_FUNC)))
              goto jmp_slot;
          }
        }
        else if (sym->st_shndx == SHN_ABS)
        {
          if (sym->st_value == 0) /* from tcc_add_btstub() */
            continue;
#ifndef TCC_TARGET_ARM
          if (PTR_SIZE != 8)
            continue;
#endif
          /* from tcc_add_symbol(): on 64 bit platforms these
             need to go through .got */
        }
        else
          continue;
      }

#ifdef TCC_TARGET_X86_64
      if ((type == R_X86_64_PLT32 || type == R_X86_64_PC32) && sym->st_shndx != SHN_UNDEF &&
          (ELFW(ST_VISIBILITY)(sym->st_other) != STV_DEFAULT || ELFW(ST_BIND)(sym->st_info) == STB_LOCAL ||
           s1->output_type & TCC_OUTPUT_EXE))
      {
        if (pass != 0)
          continue;
        rel->r_info = ELFW(R_INFO)(sym_index, R_X86_64_PC32);
        continue;
      }
#endif
      reloc_type = code_reloc(type);
      if (reloc_type == -1)
      {
        tcc_error_noabort("Unknown relocation type: %d", type);
        continue;
      }

      if (reloc_type != 0)
      {
      jmp_slot:
        if (pass != 0)
          continue;
        reloc_type = R_JMP_SLOT;
      }
      else
      {
        if (pass != 1)
          continue;
        reloc_type = R_GLOB_DAT;
      }

      if (!s1->got)
        got_sym = build_got(s1);

      if (gotplt_entry == BUILD_GOT_ONLY)
        continue;

      attr = put_got_entry(s1, reloc_type, sym_index);

      if (reloc_type == R_JMP_SLOT)
        rel->r_info = ELFW(R_INFO)(attr->plt_sym, type);
    }
  }
  if (++pass < 2)
    goto redo;
  /* .rel.plt refers to .got actually */
  if (s1->plt && s1->plt->reloc)
    s1->plt->reloc->sh_info = s1->got->sh_num;
  if (got_sym) /* set size */
    ((ElfW(Sym) *)symtab_section->data)[got_sym].st_size = s1->got->data_offset;
}
#endif /* def NEED_BUILD_GOT */

ST_FUNC int set_global_sym(TCCState *s1, const char *name, Section *sec, addr_t offs)
{
  int shn = sec ? sec->sh_num : offs || !name ? SHN_ABS : SHN_UNDEF;
  if (sec && offs == -1)
    offs = sec->data_offset;
  return set_elf_sym(symtab_section, offs, 0, ELFW(ST_INFO)(name ? STB_GLOBAL : STB_LOCAL, STT_NOTYPE), 0, shn, name);
}

static void add_init_array_defines(TCCState *s1, const char *section_name)
{
  Section *s;
  addr_t end_offset;
  char buf[1024];
  s = have_section(s1, section_name);
  if (!s || !(s->sh_flags & SHF_ALLOC))
  {
    end_offset = 0;
    s = text_section;
  }
  else
  {
    end_offset = s->data_offset;
  }
  snprintf(buf, sizeof(buf), "__%s_start", section_name + 1);
  set_global_sym(s1, buf, s, 0);
  snprintf(buf, sizeof(buf), "__%s_end", section_name + 1);
  set_global_sym(s1, buf, s, end_offset);
}

ST_FUNC void add_array(TCCState *s1, const char *sec, int c)
{
  Section *s;
  s = find_section(s1, sec);
  s->sh_flags = shf_RELRO;
  s->sh_type = sec[1] == 'i' ? SHT_INIT_ARRAY : SHT_FINI_ARRAY;
  put_elf_reloc(s1->symtab, s, s->data_offset, R_DATA_PTR, c);
  section_ptr_add(s, PTR_SIZE);
}

#ifdef CONFIG_TCC_BCHECK
ST_FUNC void tcc_add_bcheck(TCCState *s1)
{
  if (0 == s1->do_bounds_check)
    return;
  section_ptr_add(bounds_section, sizeof(addr_t));
}
#endif

/* set symbol to STB_LOCAL and resolve. The point is to not export it as
   a dynamic symbol to allow so's to have one each with a different value. */
static void set_local_sym(TCCState *s1, const char *name, Section *s, int offset)
{
  int c = find_elf_sym(s1->symtab, name);
  if (c)
  {
    ElfW(Sym) *esym = (ElfW(Sym) *)s1->symtab->data + c;
    esym->st_info = ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE);
    esym->st_value = offset;
    esym->st_shndx = s->sh_num;
  }
}

/* avoid generating debug/test_coverage code for stub functions */
static void tcc_compile_string_no_debug(TCCState *s, const char *str)
{
  int save_do_debug = s->do_debug;
  int save_test_coverage = s->test_coverage;

  s->do_debug = 0;
  s->test_coverage = 0;
  tcc_compile_string(s, str);
  s->do_debug = save_do_debug;
  s->test_coverage = save_test_coverage;
}

#ifdef CONFIG_TCC_BACKTRACE
static void put_ptr(TCCState *s1, Section *s, int offs)
{
  int c;
  c = set_global_sym(s1, NULL, s, offs);
  s = data_section;
  put_elf_reloc(s1->symtab, s, s->data_offset, R_DATA_PTR, c);
  section_ptr_add(s, PTR_SIZE);
}

ST_FUNC void tcc_add_btstub(TCCState *s1)
{
  Section *s;
  int n, o, *p;
  CString cstr;
  const char *__rt_info = &"___rt_info"[!s1->leading_underscore];

  s = data_section;
  /* Align to PTR_SIZE */
  section_ptr_add(s, -s->data_offset & (PTR_SIZE - 1));
  o = s->data_offset;
  /* create a struct rt_context (see tccrun.c) */
  if (s1->dwarf)
  {
    put_ptr(s1, dwarf_line_section, 0);
    put_ptr(s1, dwarf_line_section, -1);
    if (s1->dwarf >= 5)
      put_ptr(s1, dwarf_line_str_section, 0);
    else
      put_ptr(s1, dwarf_str_section, 0);
  }
  else
  {
    /* stabs removed - emit zeroes as placeholder */
    section_ptr_add(s, 3 * PTR_SIZE);
  }

  /* skip esym_start/esym_end/elf_str (not loaded) */
  section_ptr_add(s, 3 * PTR_SIZE);

  if (s1->output_type == TCC_OUTPUT_MEMORY && 0 == s1->dwarf)
  {
    put_ptr(s1, text_section, 0);
  }
  else
  {
    /* prog_base : local nameless symbol with offset 0 at SHN_ABS */
    put_ptr(s1, NULL, 0);
#if defined TCC_TARGET_MACHO
    /* adjust for __PAGEZERO */
    if (s1->dwarf == 0 && s1->output_type == TCC_OUTPUT_EXE)
      write64le(data_section->data + data_section->data_offset - PTR_SIZE, (uint64_t)1 << 32);
#endif
  }
  n = 3 * PTR_SIZE;
#ifdef CONFIG_TCC_BCHECK
  if (s1->do_bounds_check)
  {
    put_ptr(s1, bounds_section, 0);
    n -= PTR_SIZE;
  }
#endif
  section_ptr_add(s, n);
  p = section_ptr_add(s, 2 * sizeof(int));
  p[0] = s1->rt_num_callers;
  p[1] = s1->dwarf;
  // if (s->data_offset - o != 10*PTR_SIZE + 2*sizeof (int)) exit(99);

  if (s1->output_type == TCC_OUTPUT_MEMORY)
  {
    set_global_sym(s1, __rt_info, s, o);
    return;
  }

  cstr_new(&cstr);
  cstr_printf(&cstr, "extern void __bt_init(),__bt_exit(),__bt_init_dll();"
                     "static void *__rt_info[];"
                     "__attribute__((constructor)) static void __bt_init_rt(){");
#ifdef TCC_TARGET_PE
  if (s1->output_type == TCC_OUTPUT_DLL)
#ifdef CONFIG_TCC_BCHECK
    cstr_printf(&cstr, "__bt_init_dll(%d);", s1->do_bounds_check);
#else
    cstr_printf(&cstr, "__bt_init_dll(0);");
#endif
#endif
  cstr_printf(&cstr, "__bt_init(__rt_info,%d);}", s1->output_type != TCC_OUTPUT_DLL);
  /* In case dlcose is called by application */
  cstr_printf(&cstr, "__attribute__((destructor)) static void __bt_exit_rt(){"
                     "__bt_exit(__rt_info);}");
  tcc_compile_string_no_debug(s1, cstr.data);
  cstr_free(&cstr);
  set_local_sym(s1, __rt_info, s, o);
}
#endif /* def CONFIG_TCC_BACKTRACE */

static void tcc_tcov_add_file(TCCState *s1, const char *filename)
{
  CString cstr;
  void *ptr;
  char wd[1024];

  if (tcov_section == NULL)
    return;
  section_ptr_add(tcov_section, 1);
  write32le(tcov_section->data, tcov_section->data_offset);

  cstr_new(&cstr);
  if (filename[0] == '/')
    cstr_printf(&cstr, "%s.tcov", filename);
  else
  {
    getcwd(wd, sizeof(wd));
    cstr_printf(&cstr, "%s/%s.tcov", wd, filename);
  }
  ptr = section_ptr_add(tcov_section, cstr.size + 1);
  strcpy((char *)ptr, cstr.data);
  unlink((char *)ptr);
#ifdef _WIN32
  normalize_slashes((char *)ptr);
#endif
  cstr_free(&cstr);

  cstr_new(&cstr);
  cstr_printf(&cstr, "extern char *__tcov_data[];"
                     "extern void __store_test_coverage ();"
                     "__attribute__((destructor)) static void __tcov_exit() {"
                     "__store_test_coverage(__tcov_data);"
                     "}");
  tcc_compile_string_no_debug(s1, cstr.data);
  cstr_free(&cstr);
  set_local_sym(s1, &"___tcov_data"[!s1->leading_underscore], tcov_section, 0);
}

#if !defined TCC_TARGET_PE && !defined TCC_TARGET_MACHO
/* add libc crt1/crti objects */
ST_FUNC void tccelf_add_crtbegin(TCCState *s1)
{
#if TARGETOS_OpenBSD
  if (s1->output_type != TCC_OUTPUT_DLL)
    tcc_add_crt(s1, "crt0.o");
  if (s1->output_type == TCC_OUTPUT_DLL)
    tcc_add_crt(s1, "crtbeginS.o");
  else
    tcc_add_crt(s1, "crtbegin.o");
#elif TARGETOS_FreeBSD || TARGETOS_NetBSD
  if (s1->output_type != TCC_OUTPUT_DLL)
#if TARGETOS_FreeBSD
    tcc_add_crt(s1, "crt1.o");
#else
    tcc_add_crt(s1, "crt0.o");
#endif
  tcc_add_crt(s1, "crti.o");
  if (s1->static_link)
    tcc_add_crt(s1, "crtbeginT.o");
  else if (s1->output_type == TCC_OUTPUT_DLL)
    tcc_add_crt(s1, "crtbeginS.o");
  else
    tcc_add_crt(s1, "crtbegin.o");
#elif TARGETOS_ANDROID
  if (s1->output_type == TCC_OUTPUT_DLL)
    tcc_add_crt(s1, "crtbegin_so.o");
  else
    tcc_add_crt(s1, "crtbegin_dynamic.o");
#else
  if (s1->output_type != TCC_OUTPUT_DLL)
    tcc_add_crt(s1, "crt1.o");
  tcc_add_crt(s1, "crti.o");
#endif
}

ST_FUNC void tccelf_add_crtend(TCCState *s1)
{
#if TARGETOS_OpenBSD
  if (s1->output_type == TCC_OUTPUT_DLL)
    tcc_add_crt(s1, "crtendS.o");
  else
    tcc_add_crt(s1, "crtend.o");
#elif TARGETOS_FreeBSD || TARGETOS_NetBSD
  if (s1->output_type == TCC_OUTPUT_DLL)
    tcc_add_crt(s1, "crtendS.o");
  else
    tcc_add_crt(s1, "crtend.o");
  tcc_add_crt(s1, "crtn.o");
#elif TARGETOS_ANDROID
  if (s1->output_type == TCC_OUTPUT_DLL)
    tcc_add_crt(s1, "crtend_so.o");
  else
    tcc_add_crt(s1, "crtend_android.o");
#else
  tcc_add_crt(s1, "crtn.o");
#endif
}
#endif /* !defined TCC_TARGET_PE && !defined TCC_TARGET_MACHO */

#if defined TCC_TARGET_ARM
/* Add ARM floating-point library based on compiler flags
 * Selects the correct FP library variant based on -mfpu and -mfloat-abi
 */
ST_FUNC void tccelf_add_arm_fp_lib(TCCState *s1)
{
  static char lib_path[256];
  const char *target = NULL;

  /* Determine target architecture suffix */
#if defined(TCC_TARGET_ARM_THUMB)
  target = "armv8m";
#else
  target = "arm";
#endif

  /* Determine which FP library to link based on fpu_type and float_abi */
  if (s1->fpu_type)
  {
    /* Check FPU type */
    switch (s1->fpu_type)
    {
    case ARM_FPU_AUTO:
    case ARM_FPU_NONE:
    case ARM_FPU_VFP:
    case ARM_FPU_VFPV3:
      /* Soft float or older VFP - use soft FP library */
      snprintf(lib_path, sizeof(lib_path), "fp/libtcc1-fp-soft-%s.a", target);
      break;
    case ARM_FPU_VFPV4:
    case ARM_FPU_FPV4_SP_D16:
    case ARM_FPU_FPV5_SP_D16:
      /* VFPv4/VFPv5 single-precision - use vfpv4-sp library */
      snprintf(lib_path, sizeof(lib_path), "fp/libtcc1-fp-vfpv4-sp-%s.a", target);
      break;
    case ARM_FPU_FPV5_D16:
    case ARM_FPU_NEON:
    case ARM_FPU_NEON_VFPV4:
    case ARM_FPU_NEON_FP_ARMV8:
      /* VFPv5 double-precision - use vfpv5-dp library */
      snprintf(lib_path, sizeof(lib_path), "fp/libtcc1-fp-vfpv5-dp-%s.a", target);
      break;
    default:
      return;
    }
  }
  else
  {
    /* Default to soft float if no FPU specified */
    snprintf(lib_path, sizeof(lib_path), "fp/libtcc1-fp-soft-%s.a", target);
  }

  /* Add the selected FP library */
  if (s1->verbose)
    printf("Adding ARM FP library: %s\n", lib_path);
  tcc_add_dll(s1, lib_path, AFF_PRINT_ERROR);
}
#endif

#ifndef TCC_TARGET_PE
/* add tcc runtime libraries */
ST_FUNC void tcc_add_runtime(TCCState *s1)
{
  s1->filetype = 0;

#ifdef CONFIG_TCC_BCHECK
  tcc_add_bcheck(s1);
#endif
  tcc_add_pragma_libs(s1);

  /* add libc */
  if (!s1->nostdlib)
  {
    int lpthread = s1->option_pthread;

#ifdef CONFIG_TCC_BCHECK
    if (s1->do_bounds_check && s1->output_type != TCC_OUTPUT_DLL)
    {
      tcc_add_support(s1, "bcheck.o");
#if !(TARGETOS_OpenBSD || TARGETOS_NetBSD)
      tcc_add_library(s1, "dl");
#endif
      lpthread = 1;
    }
#endif
#ifdef CONFIG_TCC_BACKTRACE
    if (s1->do_backtrace)
    {
      if (s1->output_type & TCC_OUTPUT_EXE)
        tcc_add_support(s1, "bt-exe.o");
      if (s1->output_type != TCC_OUTPUT_DLL)
        tcc_add_support(s1, "bt-log.o");
      tcc_add_btstub(s1);
      lpthread = 1;
    }
#endif
    if (lpthread)
      tcc_add_library(s1, "pthread");
    tcc_add_library(s1, "c");
#ifdef TCC_LIBGCC
    if (!s1->static_link)
    {
      if (TCC_LIBGCC[0] == '/')
        tcc_add_file(s1, TCC_LIBGCC);
      else
        tcc_add_dll(s1, TCC_LIBGCC, AFF_PRINT_ERROR);
    }
#endif
#if defined TCC_TARGET_ARM && TARGETOS_FreeBSD
    tcc_add_library(s1, "gcc_s"); // unwind code
#endif
    if (TCC_LIBTCC1[0])
      tcc_add_support(s1, TCC_LIBTCC1);
#if defined TCC_TARGET_ARM
    /* Add ARM floating-point library based on -mfpu and -mfloat-abi flags */
    tccelf_add_arm_fp_lib(s1);
#endif
#ifndef TCC_TARGET_MACHO
    if (s1->output_type != TCC_OUTPUT_MEMORY)
      tccelf_add_crtend(s1);
#endif
  }
}
#endif /* ndef TCC_TARGET_PE */

/* add various standard linker symbols (must be done after the
   sections are filled (for example after allocating common
   symbols)) */
static void tcc_add_linker_symbols(TCCState *s1)
{
  char buf[1024];
  int i;
  Section *s;

  set_global_sym(s1, "_etext", text_section, -1);
  set_global_sym(s1, "_edata", data_section, -1);
  set_global_sym(s1, "_end", bss_section, -1);
#if TARGETOS_OpenBSD
  set_global_sym(s1, "__executable_start", NULL, ELF_START_ADDR);
#endif
#ifdef TCC_TARGET_RISCV64
  /* XXX should be .sdata+0x800, not .data+0x800 */
  set_global_sym(s1, "__global_pointer$", data_section, 0x800);
#endif
  /* horrible new standard ldscript defines */
  add_init_array_defines(s1, ".preinit_array");
  add_init_array_defines(s1, ".init_array");
  add_init_array_defines(s1, ".fini_array");
  /* add start and stop symbols for sections whose name can be
     expressed in C */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if ((s->sh_flags & SHF_ALLOC) &&
        (s->sh_type == SHT_PROGBITS || s->sh_type == SHT_NOBITS || s->sh_type == SHT_STRTAB))
    {
      /* check if section name can be expressed in C */
      const char *p0, *p;
      p0 = s->name;
      if (*p0 == '.')
        ++p0;
      p = p0;
      for (;;)
      {
        int c = *p;
        if (!c)
          break;
        if (!isid(c) && !isnum(c))
          goto next_sec;
        p++;
      }
      snprintf(buf, sizeof(buf), "__start_%s", p0);
      set_global_sym(s1, buf, s, 0);
      snprintf(buf, sizeof(buf), "__stop_%s", p0);
      set_global_sym(s1, buf, s, -1);
    }
  next_sec:;
  }
}

ST_FUNC void resolve_common_syms(TCCState *s1)
{
  ElfW(Sym) * sym;

  /* Allocate common symbols in BSS.  */
  for_each_elem(symtab_section, 1, sym, ElfW(Sym))
  {
    if (sym->st_shndx == SHN_COMMON)
    {
      /* symbol alignment is in st_value for SHN_COMMONs */
      sym->st_value = section_add(bss_section, sym->st_size, sym->st_value);
      sym->st_shndx = bss_section->sh_num;
    }
  }

  /* Now assign linker provided symbols their value.  */
  tcc_add_linker_symbols(s1);
}

#ifndef ELF_OBJ_ONLY
ST_FUNC void fill_got_entry(TCCState *s1, ElfW_Rel *rel)
{
  int sym_index = ELFW(R_SYM)(rel->r_info);
  ElfW(Sym) *sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
  struct sym_attr *attr = get_sym_attr(s1, sym_index, 0);
  unsigned offset = attr->got_offset;

  if (0 == offset)
    return;
  section_reserve(s1->got, offset + PTR_SIZE);
#if PTR_SIZE == 8
  write64le(s1->got->data + offset, sym->st_value);
#else
  write32le(s1->got->data + offset, sym->st_value);
#endif
}

/* Perform relocation to GOT or PLT entries */
ST_FUNC void fill_got(TCCState *s1)
{
  Section *s;
  ElfW_Rel *rel;
  int i;

  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (s->sh_type != SHT_RELX)
      continue;
    /* no need to handle got relocations */
    if (s->link != symtab_section)
      continue;
    for_each_elem(s, 0, rel, ElfW_Rel)
    {
      switch (ELFW(R_TYPE)(rel->r_info))
      {
      case R_X86_64_GOT32:
      case R_X86_64_GOTPCREL:
      case R_X86_64_GOTPCRELX:
      case R_X86_64_REX_GOTPCRELX:
      case R_X86_64_PLT32:
        fill_got_entry(s1, rel);
        break;
      }
    }
  }
}

/* See put_got_entry for a description.  This is the second stage
   where GOT references to local defined symbols are rewritten.  */
static void fill_local_got_entries(TCCState *s1)
{
  ElfW_Rel *rel;
  if (!s1->got->reloc)
    return;
  for_each_elem(s1->got->reloc, 0, rel, ElfW_Rel)
  {
    if (ELFW(R_TYPE)(rel->r_info) == R_RELATIVE)
    {
      int sym_index = ELFW(R_SYM)(rel->r_info);
      ElfW(Sym) *sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
      struct sym_attr *attr = get_sym_attr(s1, sym_index, 0);
      unsigned offset = attr->got_offset;
      if (offset != rel->r_offset - s1->got->sh_addr)
        tcc_error_noabort("fill_local_got_entries: huh?");
      rel->r_info = ELFW(R_INFO)(0, R_RELATIVE);
#if SHT_RELX == SHT_RELA
      rel->r_addend = sym->st_value;
#else
      /* All our REL architectures also happen to be 32bit LE.  */
      write32le(s1->got->data + offset, sym->st_value);
#endif
    }
  }
}

/* Bind symbols of executable: resolve undefined symbols from exported symbols
   in shared libraries */
static void bind_exe_dynsyms(TCCState *s1, int is_PIE)
{
  const char *name;
  int sym_index, index;
  ElfW(Sym) * sym, *esym;
  int type;

  /* Resolve undefined symbols from dynamic symbols. When there is a match:
     - if STT_FUNC or STT_GNU_IFUNC symbol -> add it in PLT
     - if STT_OBJECT symbol -> add it in .bss section with suitable reloc */
  for_each_elem(symtab_section, 1, sym, ElfW(Sym))
  {
    if (sym->st_shndx == SHN_UNDEF)
    {
      /* Validate st_name before using it */
      if (sym->st_name >= symtab_section->link->data_offset)
      {
        int sym_idx = sym - (ElfW(Sym) *)symtab_section->data;
        tcc_error_noabort(
            "internal error (bind_exe_dynsyms): symbol %d has invalid st_name offset 0x%x (strtab size: 0x%lx)",
            sym_idx, sym->st_name, (unsigned long)symtab_section->link->data_offset);
        continue;
      }
      name = (char *)symtab_section->link->data + sym->st_name;
      sym_index = find_elf_sym(s1->dynsymtab_section, name);
      if (sym_index)
      {
        if (is_PIE)
          continue;
        esym = &((ElfW(Sym) *)s1->dynsymtab_section->data)[sym_index];
        type = ELFW(ST_TYPE)(esym->st_info);
        if ((type == STT_FUNC) || (type == STT_GNU_IFUNC))
        {
          /* Indirect functions shall have STT_FUNC type in executable
           * dynsym section. Indeed, a dlsym call following a lazy
           * resolution would pick the symbol value from the
           * executable dynsym entry which would contain the address
           * of the function wanted by the caller of dlsym instead of
           * the address of the function that would return that
           * address */
          int dynindex = put_elf_sym(s1->dynsym, 0, esym->st_size, ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC), 0, 0, name);
          int index = sym - (ElfW(Sym) *)symtab_section->data;
          get_sym_attr(s1, index, 1)->dyn_index = dynindex;
        }
        else if (type == STT_OBJECT)
        {
          unsigned long offset;
          ElfW(Sym) * dynsym;
          offset = bss_section->data_offset;
          /* XXX: which alignment ? */
          offset = (offset + 16 - 1) & -16;
          set_elf_sym(s1->symtab, offset, esym->st_size, esym->st_info, 0, bss_section->sh_num, name);
          index = put_elf_sym(s1->dynsym, offset, esym->st_size, esym->st_info, 0, bss_section->sh_num, name);

          /* Ensure R_COPY works for weak symbol aliases */
          if (ELFW(ST_BIND)(esym->st_info) == STB_WEAK)
          {
            for_each_elem(s1->dynsymtab_section, 1, dynsym, ElfW(Sym))
            {
              if ((dynsym->st_value == esym->st_value) && (ELFW(ST_BIND)(dynsym->st_info) == STB_GLOBAL))
              {
                char *dynname = (char *)s1->dynsymtab_section->link->data + dynsym->st_name;
                put_elf_sym(s1->dynsym, offset, dynsym->st_size, dynsym->st_info, 0, bss_section->sh_num, dynname);
                break;
              }
            }
          }

          put_elf_reloc(s1->dynsym, bss_section, offset, R_COPY, index);
          offset += esym->st_size;
          bss_section->data_offset = offset;
        }
      }
      else
      {
        /* STB_WEAK undefined symbols are accepted */
        /* XXX: _fp_hw seems to be part of the ABI, so we ignore it */
        if (ELFW(ST_BIND)(sym->st_info) == STB_WEAK || !strcmp(name, "_fp_hw"))
        {
        }
        else
        {
          tcc_error_noabort("undefined symbol '%s'", name);
        }
      }
    }
  }
}

/* Bind symbols of libraries: export all non local symbols of executable that
   are referenced by shared libraries. The reason is that the dynamic loader
   search symbol first in executable and then in libraries. Therefore a
   reference to a symbol already defined by a library can still be resolved by
   a symbol in the executable.   With -rdynamic, export all defined symbols */
static void bind_libs_dynsyms(TCCState *s1)
{
  const char *name;
  int dynsym_index;
  ElfW(Sym) * sym, *esym;

  for_each_elem(symtab_section, 1, sym, ElfW(Sym))
  {
    name = (char *)symtab_section->link->data + sym->st_name;
    dynsym_index = find_elf_sym(s1->dynsymtab_section, name);
    if (sym->st_shndx != SHN_UNDEF)
    {
      if (ELFW(ST_BIND)(sym->st_info) != STB_LOCAL && (dynsym_index || s1->rdynamic))
        set_elf_sym(s1->dynsym, sym->st_value, sym->st_size, sym->st_info, 0, sym->st_shndx, name);
    }
    else if (dynsym_index)
    {
      esym = (ElfW(Sym) *)s1->dynsymtab_section->data + dynsym_index;
      if (esym->st_shndx == SHN_UNDEF)
      {
        /* weak symbols can stay undefined */
        if (ELFW(ST_BIND)(esym->st_info) != STB_WEAK)
          tcc_warning("undefined dynamic symbol '%s'", name);
      }
    }
  }
}

/* Export all non local symbols. This is used by shared libraries so that the
   non local symbols they define can resolve a reference in another shared
   library or in the executable. Correspondingly, it allows undefined local
   symbols to be resolved by other shared libraries or by the executable. */
static void export_global_syms(TCCState *s1)
{
  int dynindex, index;
  const char *name;
  ElfW(Sym) * sym;
  for_each_elem(symtab_section, 1, sym, ElfW(Sym))
  {
    if (ELFW(ST_BIND)(sym->st_info) != STB_LOCAL)
    {
      name = (char *)symtab_section->link->data + sym->st_name;
      dynindex = set_elf_sym(s1->dynsym, sym->st_value, sym->st_size, sym->st_info, 0, sym->st_shndx, name);
      index = sym - (ElfW(Sym) *)symtab_section->data;
      get_sym_attr(s1, index, 1)->dyn_index = dynindex;
    }
  }
}

/* decide if an unallocated section should be output. */
static int set_sec_sizes(TCCState *s1)
{
  int i;
  Section *s;
  int textrel = 0;
  int file_type = s1->output_type;

  /* Allocate strings for section names */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (s->sh_type == SHT_RELX && !(s->sh_flags & SHF_ALLOC))
    {
      /* when generating a DLL, we include relocations but
         we may patch them */
      if ((file_type & TCC_OUTPUT_DYN) && (s1->sections[s->sh_info]->sh_flags & SHF_ALLOC))
      {
        int count = prepare_dynamic_rel(s1, s);
        if (count)
        {
          /* allocate the section */
          s->sh_flags |= SHF_ALLOC;
          s->sh_size = count * sizeof(ElfW_Rel);
          if (s1->sections[s->sh_info]->sh_flags & SHF_EXECINSTR)
            textrel += count;
        }
      }
    }
    else if ((s->sh_flags & SHF_ALLOC)
#ifdef TCC_TARGET_ARM
             || s->sh_type == SHT_ARM_ATTRIBUTES
#endif
             || s1->do_debug)
    {
      s->sh_size = s->data_offset;
    }

#ifdef TCC_TARGET_ARM
    /* XXX: Suppress stack unwinding section. */
    if (s->sh_type == SHT_ARM_EXIDX)
    {
      s->sh_flags = 0;
      s->sh_size = 0;
    }
#endif

    /* Suppress legacy stabs sections. */
    if (!strcmp(s->name, ".stab") || !strcmp(s->name, ".stabstr") || !strncmp(s->name, ".rel.stab", 9) ||
        !strncmp(s->name, ".rela.stab", 10))
    {
      s->sh_flags = 0;
      s->sh_size = 0;
    }
  }
  return textrel;
}

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

/* Find the linker script output section index and pattern index for a given
   section name. Returns output section index via return value (-1 if not
   found), and sets *pat_idx to the pattern index within that output section.
   Patterns are checked first (in order) since they define the ordering within
   the output section. If no pattern matches but the section name exactly
   matches an output section name, pat_idx is set to a value after all patterns
   to indicate it should come last within that output section. */
static int ld_find_output_section_idx(TCCState *s1, const char *name, int *pat_idx)
{
  LDScript *ld = s1->ld_script;
  int i, j;

  if (pat_idx)
    *pat_idx = -1;

  if (!ld || ld->nb_output_sections == 0)
    return -1;

  for (i = 0; i < ld->nb_output_sections; i++)
  {
    LDOutputSection *os = &ld->output_sections[i];
    /* Check patterns first - they define the ordering within the output section
     */
    for (j = 0; j < os->nb_patterns; j++)
    {
      if (ld_section_matches_pattern(name, os->patterns[j].pattern))
      {
        if (pat_idx)
          *pat_idx = j;
        return i;
      }
    }
    /* Check exact name match - comes after all patterns */
    if (!strcmp(name, os->name))
    {
      if (pat_idx)
        *pat_idx = os->nb_patterns; /* after all patterns */
      return i;
    }
  }
  return -1;
}

/* Check if a section name matches a specific output section index */
static int ld_section_matches_output(TCCState *s1, const char *name, int os_idx)
{
  int pat_idx = -1;
  int found = ld_find_output_section_idx(s1, name, &pat_idx);
  return found == os_idx;
}

/* Decide the layout of sections loaded in memory. This must be done before
   program headers are filled since they contain info about the layout.
   We do the following ordering: interp, symbol tables, relocations, progbits,
   nobits */
static int sort_sections(TCCState *s1, int *sec_order, struct dyn_inf *d)
{
  Section *s;
  int i, j, k, f, f0, n, ld_idx;
  int nb_sections = s1->nb_sections;
  int *sec_cls = sec_order + nb_sections;

  for (i = 1; i < nb_sections; i++)
  {
    s = s1->sections[i];
    if (0 == s->sh_name)
    {
      j = 0x900; /* no sh_name: won't go to file */
    }
    else if (s->sh_flags & SHF_ALLOC)
    {
      j = 0x100;
    }
    else
    {
      j = 0x700;
    }
    if (j >= 0x700 && s1->output_format != TCC_OUTPUT_FORMAT_ELF)
      s->sh_size = 0, j = 0x900;

    if (s->sh_type == SHT_SYMTAB || s->sh_type == SHT_DYNSYM)
    {
      k = 0xff;
    }
    else if (s->sh_type == SHT_STRTAB && strcmp(s->name, ".stabstr"))
    {
      k = 0xff;
      if (i == nb_sections - 1) /* ".shstrtab" assumed to stay last */
        k = 0xff;
    }
    else if (s->sh_type == SHT_HASH || s->sh_type == SHT_GNU_HASH)
    {
      k = 0xff;
    }
    else if (s->sh_type == SHT_GNU_verdef || s->sh_type == SHT_GNU_verneed || s->sh_type == SHT_GNU_versym)
    {
      k = 0x13;
    }
    else if (s->sh_type == SHT_RELX)
    {
      k = 0x80;
      if (s1->plt && s == s1->plt->reloc)
        k = 0x81;
    }
    else if (s->sh_flags & SHF_EXECINSTR)
    {
      k = 0x30;
      if (s == s1->plt)
      {
        k = 0x32;
      }
      /* RELRO sections --> */
    }
    else if (s->sh_type == SHT_PREINIT_ARRAY)
    {
      k = 0x41;
    }
    else if (s->sh_type == SHT_INIT_ARRAY)
    {
      k = 0x42;
    }
    else if (s->sh_type == SHT_FINI_ARRAY)
    {
      k = 0x43;
    }
    else if (s->sh_type == SHT_DYNAMIC)
    {
      k = 0x280;
    }
    else if (s == s1->got)
    {
      k = 0x70; /* .got as RELRO needs BIND_NOW in DT_FLAGS */
    }
    else if (s->reloc && (s->reloc->sh_flags & SHF_ALLOC) && j == 0x100)
    {
      if (s == rodata_section)
      {
        k = 0x43;
      }
      else
      {
        k = 0x44;
      }
      /* <-- */
    }
    else if (s->sh_type == SHT_NOTE)
    {
      k = 0x60;
    }
    else if (s->sh_type == SHT_NOBITS)
    {
      k = 0x70; /* bss */
    }
    else if (s == d->interp)
    {
      k = 0xff;
    }
    else if (s == rodata_section)
    {
      k = 0x40; /* rodata */
    }
    else
    {
      k = 0x50; /* data */
    }

    k += j;

    /* Check for RELRO sections before potentially modifying k for linker
       script ordering. RELRO sections are in range 0x141-0x14f. */
    if ((k & 0xfff0) == 0x140)
    {
      /* make RELRO section writable */
      s->sh_flags |= SHF_WRITE;
    }

    /* If linker script has output sections defined, use linker script order
       for ALLOC program sections (not relocation, symbol, or other special
       sections). The linker script output section index becomes the primary
       sort key, with the pattern index within the output section as the
       secondary key. This ensures sections are ordered according to the
       pattern order in the linker script (e.g., KEEP(*(.isr_vector)) before
       *(.text)).
       Sections not in the linker script are placed after all linker script
       sections.

       Classification encoding for linker script sections (within 0x100-0x6ff):
       - Upper nibble (0x100-0x600): output section index (up to 6 sections)
       - Lower byte: pattern index * 2 (to leave room for sub-classes)

       For sections not in linker script, we use 0x6xx range.

       Skip relocation sections (SHT_RELX), symbol tables, string tables,
       hash tables, and other special sections - they should keep their
       default ordering. */
    if (j == 0x100 && s1->ld_script && s1->ld_script->nb_output_sections > 0 && s->sh_type != SHT_RELX &&
        s->sh_type != SHT_SYMTAB && s->sh_type != SHT_DYNSYM && s->sh_type != SHT_STRTAB && s->sh_type != SHT_HASH &&
        s->sh_type != SHT_GNU_HASH && s->sh_type != SHT_DYNAMIC)
    {
      int pat_idx = 0;
      ld_idx = ld_find_output_section_idx(s1, s->name, &pat_idx);
      if (ld_idx >= 0)
      {
        /* Section is in linker script: use ld_idx as primary key,
           pattern index as secondary key.
           pat_idx is the index of the matching pattern (0+), or nb_patterns
           for exact name match (comes after all patterns).
           Keep values in 0x100-0x5ff range to ensure proper handling. */
        /* Limit ld_idx to fit in 5 bits (0-31 output sections) */
        if (ld_idx > 31)
          ld_idx = 31;
        /* pat_idx in lower bits, ld_idx in upper bits, all within 0x100-0x6ff
         */
        k = 0x100 + (ld_idx << 4) + (pat_idx & 0x0f);
      }
      else
      {
        /* Section not in linker script: place after all linker script sections
           but still within ALLOC range. Use 0x6xx + original sub-class. */
        k = 0x600 + ((k & 0x7f) >> 4);
      }
    }

    for (n = i; n > 1 && k < (f = sec_cls[n - 1]); --n)
      sec_cls[n] = f, sec_order[n] = sec_order[n - 1];
    sec_cls[n] = k, sec_order[n] = i;
  }
  sec_order[0] = 0;
  d->shnum = 1;

  /* count PT_LOAD headers needed */
  n = f0 = 0;
  for (i = 1; i < nb_sections; i++)
  {
    s = s1->sections[sec_order[i]];
    k = sec_cls[i];
    f = 0;
    if (k < 0x900)
      ++d->shnum;
    if (k < 0x700)
    {
      f = s->sh_flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR | SHF_TLS);
#if TARGETOS_NetBSD
      /* NetBSD only supports 2 PT_LOAD sections.
         See: https://blog.netbsd.org/tnf/entry/the_first_report_on_lld */
      if ((f & SHF_WRITE) == 0)
        f |= SHF_EXECINSTR;
#else
      if ((k & 0xfff0) == 0x240) /* RELRO sections */
        f |= 1 << 4;
#endif
      /* start new header when flags changed or relro, but avoid zero memsz */
      if (f != f0 && s->sh_size)
        f0 = f, ++n, f |= 1 << 8;
    }
    sec_cls[i] = f;
#ifdef DEBUG_RELOC
    printf("ph %d sec %02d : %3X %3X  %x  %04X  %s\n", (f > 0) * n, i, f, k, s->sh_type, (int)s->sh_size, s->name);
#endif
  }
  return n;
}

static ElfW(Phdr) * fill_phdr(ElfW(Phdr) * ph, int type, Section *s)
{
  if (s)
  {
    ph->p_offset = s->sh_offset;
    ph->p_vaddr = s->sh_addr;
    ph->p_filesz = s->sh_size;
    ph->p_align = s->sh_addralign;
  }
  ph->p_type = type;
  ph->p_flags = PF_R;
  ph->p_paddr = ph->p_vaddr;
  ph->p_memsz = ph->p_filesz;
  return ph;
}

/* Assign sections to segments and decide how are sections laid out when loaded
   in memory. This function also fills corresponding program headers. */
static int layout_sections(TCCState *s1, int *sec_order, struct dyn_inf *d)
{
  Section *s;
  addr_t addr, tmp, align, s_align, base;
  ElfW(Phdr) *ph = NULL;
  int i, f, n, phnum, phfill;
  int file_offset;
  int elf_header_offset = 0;

  /* compute number of program headers */
  phnum = sort_sections(s1, sec_order, d);
  phfill = 0; /* set to 1 to have dll's with a PT_PHDR */
  if (d->interp)
    phfill = 2;
  phnum += phfill;
  if (d->note)
    ++phnum;
  if (d->dynamic)
    ++phnum;
  if (eh_frame_hdr_section)
    ++phnum;
  if (d->roinf)
    ++phnum;
  /* Add extra segments for memory regions (each region may need new PT_LOAD) */
  if (s1->ld_script && s1->ld_script->nb_memory_regions > 1)
    phnum += s1->ld_script->nb_memory_regions - 1;
  d->phnum = phnum;
  d->phdr = tcc_mallocz(phnum * sizeof(ElfW(Phdr)));

  file_offset = 0;
  if (s1->output_format == TCC_OUTPUT_FORMAT_ELF)
  {
    file_offset = (sizeof(ElfW(Ehdr)) + phnum * sizeof(ElfW(Phdr)) + 3) & -4;
    file_offset += d->shnum * sizeof(ElfW(Shdr));
  }

  s_align = ELF_PAGE_SIZE;
  if (s1->section_align)
    s_align = s1->section_align;

  addr = ELF_START_ADDR;
  if (s1->output_type & TCC_OUTPUT_DYN)
    addr = 0;

  /* Use linker script MEMORY origin if available */
  if (s1->ld_script && s1->ld_script->nb_memory_regions > 0)
  {
    addr = s1->ld_script->memory_regions[0].origin;
  }

  if (s1->has_text_addr)
  {
    addr = s1->text_addr;
    if (0)
    {
      int a_offset, p_offset;
      /* we ensure that (addr % ELF_PAGE_SIZE) == file_offset %
         ELF_PAGE_SIZE */
      a_offset = (int)(addr & (s_align - 1));
      p_offset = file_offset & (s_align - 1);
      if (a_offset < p_offset)
        a_offset += s_align;
      file_offset += (a_offset - p_offset);
    }
  }
  base = addr;
  /* compute address after headers */
  // addr += file_offset;
  elf_header_offset = file_offset;

  /* Track per-memory-region address counters for linker script placement */
  addr_t mr_addr[LD_MAX_MEMORY_REGIONS];
  int cur_mr = 0;
  if (s1->ld_script && s1->ld_script->nb_memory_regions > 0)
  {
    for (int mr = 0; mr < s1->ld_script->nb_memory_regions; mr++)
    {
      LDMemoryRegion *region = &s1->ld_script->memory_regions[mr];
      mr_addr[mr] = region->origin;
    }
    addr = mr_addr[0];
  }
  else
  {
    mr_addr[0] = addr;
  }

  n = 0;
  for (i = 1; i < s1->nb_sections; i++)
  {
    int sec_idx = sec_order[i];
    int sec_flags_idx = i + s1->nb_sections;
    s = s1->sections[sec_idx];
    f = sec_order[sec_flags_idx];
    align = s->sh_addralign - 1;

    if (f == 0)
    { /* no alloc */
      file_offset = (file_offset + align) & ~align;
      s->sh_offset = file_offset;
      if (s->sh_type != SHT_NOBITS)
        file_offset += s->sh_size;
      continue;
    }

    /* Check if this section should be placed in a different memory region */
    if (s1->ld_script && s1->ld_script->nb_memory_regions > 0 && s1->ld_script->nb_output_sections > 0)
    {
      int pat_idx = -1;
      int ld_idx = ld_find_output_section_idx(s1, s->name, &pat_idx);
      if (ld_idx >= 0)
      {
        LDOutputSection *os = &s1->ld_script->output_sections[ld_idx];
        int new_mr = os->memory_region_idx;
        if (new_mr >= 0 && new_mr < s1->ld_script->nb_memory_regions)
        {
          if (new_mr != cur_mr)
          {
            /* Save current region's address and switch to new region */
            mr_addr[cur_mr] = addr;
            cur_mr = new_mr;
            addr = mr_addr[cur_mr];
            /* Force new program header when changing memory regions */
            f |= 1 << 8;
          }
        }
      }
    }

    if ((f & 1 << 8) && n)
    {
      /* different rwx section flags */
      if (s1->output_format == TCC_OUTPUT_FORMAT_ELF)
      {
        /* if in the middle of a page, w e duplicate the page in
           memory so that one copy is RX and the other is RW */
        if ((addr & (s_align - 1)) != 0)
          addr += s_align;
      }
      else
      {
        align = s_align - 1;
      }
    }

    tmp = addr;
    addr = (addr + align) & ~align;
    file_offset += (int)(addr - tmp);
    s->sh_offset = file_offset;
    s->sh_addr = addr;
    s->sh_size = (s->sh_size + align) & ~align;

    if (f & 1 << 8)
    {
      /* set new program header */
      int ph_idx = phfill + n;
      ph = &d->phdr[ph_idx];
      ph->p_type = PT_LOAD;
      ph->p_align = s_align;
      ph->p_flags = PF_R;
      if (f & SHF_WRITE)
        ph->p_flags |= PF_W;
      if (f & SHF_EXECINSTR)
        ph->p_flags |= PF_X;
      if (f & SHF_TLS)
      {
        ph->p_type = PT_TLS;
        ph->p_align = align + 1;
      }

      ph->p_offset = file_offset;
      ph->p_vaddr = addr;

      if (n == 0)
      {
        /* Make the first PT_LOAD segment include the program
           headers itself (and the ELF header as well), it'll
           come out with same memory use but will make various
           tools like binutils strip work better.  */
        // ph->p_offset = 0;
        ph->p_vaddr = base;
      }
      ph->p_paddr = ph->p_vaddr;
      ++n;
    }

    if (f & 1 << 4)
    {
      Section *roinf = &d->_roinf;
      if (roinf->sh_size == 0)
      {
        roinf->sh_offset = s->sh_offset;
        roinf->sh_addr = s->sh_addr;
        roinf->sh_addralign = 1;
      }
      roinf->sh_size = (addr - roinf->sh_addr) + s->sh_size;
    }

    addr += s->sh_size;
    if (s->sh_type != SHT_NOBITS)
      file_offset += s->sh_size;

    if (ph)
    {
      ph->p_filesz = file_offset - ph->p_offset;
      ph->p_memsz = addr - ph->p_vaddr;
      if (n == 1)
      {
        ph->p_memsz += elf_header_offset;
      }
    }
  }

  /* Fill other headers */
  if (d->note)
    fill_phdr(++ph, PT_NOTE, d->note);
  if (d->dynamic)
  {
    ElfW(Phdr) *dph = fill_phdr(++ph, PT_DYNAMIC, d->dynamic);
    dph->p_flags |= PF_W;
  }
  if (eh_frame_hdr_section)
    fill_phdr(++ph, PT_GNU_EH_FRAME, eh_frame_hdr_section);
  if (d->roinf)
  {
    ElfW(Phdr) *rph = fill_phdr(++ph, PT_GNU_RELRO, d->roinf);
    rph->p_flags |= PF_W;
  }
  if (d->interp)
  {
    ElfW(Phdr) *iph = &d->phdr[1];
    fill_phdr(iph, PT_INTERP, d->interp);
  }
  if (phfill)
  {
    ph = &d->phdr[0];
    ph->p_offset = sizeof(ElfW(Ehdr));
    ph->p_vaddr = base + ph->p_offset;
    {
      int phdr_sz = phnum * sizeof(ElfW(Phdr));
      ph->p_filesz = phdr_sz;
    }
    ph->p_align = 4;
    fill_phdr(ph, PT_PHDR, NULL);
  }
  return 0;
}

/* put dynamic tag */
static void put_dt(Section *dynamic, int dt, addr_t val)
{
  ElfW(Dyn) * dyn;
  dyn = section_ptr_add(dynamic, sizeof(ElfW(Dyn)));
  dyn->d_tag = dt;
  dyn->d_un.d_val = val;
}

/* Fill the dynamic section with tags describing the address and size of
   sections */
static void fill_dynamic(TCCState *s1, struct dyn_inf *dyninf)
{
  Section *dynamic = dyninf->dynamic;
  Section *s;

  /* put dynamic section entries */
  put_dt(dynamic, DT_HASH, s1->dynsym->hash->sh_addr);
  put_dt(dynamic, DT_GNU_HASH, dyninf->gnu_hash->sh_addr);
  put_dt(dynamic, DT_STRTAB, dyninf->dynstr->sh_addr);
  put_dt(dynamic, DT_SYMTAB, s1->dynsym->sh_addr);
  put_dt(dynamic, DT_STRSZ, dyninf->dynstr->data_offset);
  put_dt(dynamic, DT_SYMENT, sizeof(ElfW(Sym)));
#if PTR_SIZE == 8
  put_dt(dynamic, DT_RELA, dyninf->rel_addr);
  put_dt(dynamic, DT_RELASZ, dyninf->rel_size);
  put_dt(dynamic, DT_RELAENT, sizeof(ElfW_Rel));
  if (s1->plt && s1->plt->reloc)
  {
    put_dt(dynamic, DT_PLTGOT, s1->got->sh_addr);
    put_dt(dynamic, DT_PLTRELSZ, s1->plt->reloc->data_offset);
    put_dt(dynamic, DT_JMPREL, s1->plt->reloc->sh_addr);
    put_dt(dynamic, DT_PLTREL, DT_RELA);
  }
  put_dt(dynamic, DT_RELACOUNT, 0);
#else
  put_dt(dynamic, DT_REL, dyninf->rel_addr);
  put_dt(dynamic, DT_RELSZ, dyninf->rel_size);
  put_dt(dynamic, DT_RELENT, sizeof(ElfW_Rel));
  if (s1->plt && s1->plt->reloc)
  {
    put_dt(dynamic, DT_PLTGOT, s1->got->sh_addr);
    put_dt(dynamic, DT_PLTRELSZ, s1->plt->reloc->data_offset);
    put_dt(dynamic, DT_JMPREL, s1->plt->reloc->sh_addr);
    put_dt(dynamic, DT_PLTREL, DT_REL);
  }
  put_dt(dynamic, DT_RELCOUNT, 0);
#endif
  if (versym_section && verneed_section)
  {
    /* The dynamic linker can not handle VERSYM without VERNEED */
    put_dt(dynamic, DT_VERSYM, versym_section->sh_addr);
    put_dt(dynamic, DT_VERNEED, verneed_section->sh_addr);
    put_dt(dynamic, DT_VERNEEDNUM, dt_verneednum);
  }
  s = have_section(s1, ".preinit_array");
  if (s && s->data_offset)
  {
    put_dt(dynamic, DT_PREINIT_ARRAY, s->sh_addr);
    put_dt(dynamic, DT_PREINIT_ARRAYSZ, s->data_offset);
  }
  s = have_section(s1, ".init_array");
  if (s && s->data_offset)
  {
    put_dt(dynamic, DT_INIT_ARRAY, s->sh_addr);
    put_dt(dynamic, DT_INIT_ARRAYSZ, s->data_offset);
  }
  s = have_section(s1, ".fini_array");
  if (s && s->data_offset)
  {
    put_dt(dynamic, DT_FINI_ARRAY, s->sh_addr);
    put_dt(dynamic, DT_FINI_ARRAYSZ, s->data_offset);
  }
  s = have_section(s1, ".init");
  if (s && s->data_offset)
  {
    put_dt(dynamic, DT_INIT, s->sh_addr);
  }
  s = have_section(s1, ".fini");
  if (s && s->data_offset)
  {
    put_dt(dynamic, DT_FINI, s->sh_addr);
  }
  if (s1->do_debug)
    put_dt(dynamic, DT_DEBUG, 0);
  put_dt(dynamic, DT_NULL, 0);
}

/* Remove gaps between RELX sections.
   These gaps are a result of final_sections_reloc. Here some relocs are
   removed. The gaps are then filled with 0 in tcc_output_elf. The 0 is
   intepreted as R_...NONE reloc. This does work on most targets but on
   OpenBSD/arm64 this is illegal. OpenBSD/arm64 does not support R_...NONE
   reloc. */
static void update_reloc_sections(TCCState *s1, struct dyn_inf *dyninf)
{
  int i;
  unsigned long file_offset = 0;
  Section *s;
  Section *relocplt = s1->plt ? s1->plt->reloc : NULL;

  /* dynamic relocation table information, for .dynamic section */
  dyninf->rel_addr = dyninf->rel_size = 0;

  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (s->sh_type == SHT_RELX && s != relocplt)
    {
      if (dyninf->rel_size == 0)
      {
        dyninf->rel_addr = s->sh_addr;
        file_offset = s->sh_offset;
      }
      else
      {
        s->sh_addr = dyninf->rel_addr + dyninf->rel_size;
        s->sh_offset = file_offset + dyninf->rel_size;
      }
      dyninf->rel_size += s->sh_size;
    }
  }
}
#endif /* ndef ELF_OBJ_ONLY */

/* Create an ELF file on disk.
   This function handle ELF specific layout requirements */
static int tcc_output_elf(TCCState *s1, FILE *f, int phnum, ElfW(Phdr) * phdr)
{
  int i, shnum, offset, size, file_type;
  Section *s;
  ElfW(Ehdr) ehdr;
  ElfW(Shdr) shdr, *sh;

  file_type = s1->output_type;
  shnum = s1->nb_sections;

  memset(&ehdr, 0, sizeof(ehdr));
  if (phnum > 0)
  {
    ehdr.e_phentsize = sizeof(ElfW(Phdr));
    ehdr.e_phnum = phnum;
    ehdr.e_phoff = sizeof(ElfW(Ehdr));
  }

  /* fill header */
  ehdr.e_ident[0] = ELFMAG0;
  ehdr.e_ident[1] = ELFMAG1;
  ehdr.e_ident[2] = ELFMAG2;
  ehdr.e_ident[3] = ELFMAG3;
  ehdr.e_ident[4] = ELFCLASSW;
  ehdr.e_ident[5] = ELFDATA2LSB;
  ehdr.e_ident[6] = EV_CURRENT;

#if TARGETOS_FreeBSD || TARGETOS_FreeBSD_kernel
  ehdr.e_ident[EI_OSABI] = ELFOSABI_FREEBSD;
#elif defined TCC_TARGET_ARM && defined TCC_ARM_EABI
  ehdr.e_flags = EF_ARM_EABI_VER5;
  ehdr.e_flags |= s1->float_abi == ARM_HARD_FLOAT ? EF_ARM_VFP_FLOAT : EF_ARM_SOFT_FLOAT;
#elif defined TCC_TARGET_ARM
  ehdr.e_ident[EI_OSABI] = ELFOSABI_ARM;
#elif defined TCC_TARGET_RISCV64
  /* XXX should be configurable */
  ehdr.e_flags = EF_RISCV_FLOAT_ABI_DOUBLE;
#endif

  if (file_type == TCC_OUTPUT_OBJ)
  {
    ehdr.e_type = ET_REL;
  }
  else
  {
    if (file_type & TCC_OUTPUT_DYN)
      ehdr.e_type = ET_DYN;
    else
      ehdr.e_type = ET_EXEC;
    if (s1->elf_entryname)
      ehdr.e_entry = get_sym_addr(s1, s1->elf_entryname, 1, 0);
    else
      ehdr.e_entry = get_sym_addr(s1, "_start", !!(file_type & TCC_OUTPUT_EXE), 0);
    if (ehdr.e_entry == (addr_t)-1)
      ehdr.e_entry = text_section->sh_addr;
    if (s1->nb_errors)
      return -1;
  }

  tcc_elf_sort_syms(s1, s1->symtab);

  ehdr.e_machine = EM_TCC_TARGET;
  ehdr.e_version = EV_CURRENT;
  ehdr.e_shoff = (sizeof(ElfW(Ehdr)) + phnum * sizeof(ElfW(Phdr)) + 3) & -4;
  ehdr.e_ehsize = sizeof(ElfW(Ehdr));
  ehdr.e_shentsize = sizeof(ElfW(Shdr));
  ehdr.e_shnum = shnum;
  ehdr.e_shstrndx = shnum - 1;

  offset = fwrite(&ehdr, 1, sizeof(ElfW(Ehdr)), f);
  if (phdr)
    offset += fwrite(phdr, 1, phnum * sizeof(ElfW(Phdr)), f);

  /* output section headers */
  while (offset < ehdr.e_shoff)
  {
    fputc(0, f);
    offset++;
  }

  for (i = 0; i < shnum; i++)
  {
    sh = &shdr;
    memset(sh, 0, sizeof(ElfW(Shdr)));
    if (i)
    {
      s = s1->sections[i];
      sh->sh_name = s->sh_name;
      sh->sh_type = s->sh_type;
      sh->sh_flags = s->sh_flags;
      sh->sh_entsize = s->sh_entsize;
      sh->sh_info = s->sh_info;
      if (s->link)
        sh->sh_link = s->link->sh_num;
      sh->sh_addralign = s->sh_addralign;
      sh->sh_addr = s->sh_addr;
      sh->sh_offset = s->sh_offset;
      sh->sh_size = s->sh_size;
    }
    offset += fwrite(sh, 1, sizeof(ElfW(Shdr)), f);
  }

  /* output sections - use streaming for lazy sections to avoid memory allocation */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (s->sh_type != SHT_NOBITS)
    {
      while (offset < s->sh_offset)
      {
        fputc(0, f);
        offset++;
      }
      size = s->sh_size;
      if (size)
      {
        if (s->lazy && !s->materialized)
        {
          /* Stream directly from source files without loading into memory */
          section_write_streaming(s1, s, f);
          offset += size;
        }
        else
        {
          /* Already materialized, write from memory */
          const int to_write = size < s->data_allocated ? size : s->data_allocated;
          offset += fwrite(s->data, 1, to_write, f);
        }
      }
    }
  }
  return 0;
}

static int tcc_output_binary(TCCState *s1, FILE *f)
{
  Section *s;
  int i, offset, size;

  offset = 0;
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (s->sh_type != SHT_NOBITS && (s->sh_flags & SHF_ALLOC))
    {
      while (offset < s->sh_offset)
      {
        fputc(0, f);
        offset++;
      }
      size = s->sh_size;
      if (s->lazy && !s->materialized)
      {
        /* Stream directly from source files without loading into memory */
        section_write_streaming(s1, s, f);
      }
      else
      {
        fwrite(s->data, 1, size, f);
      }
      offset += size;
    }
  }
  return 0;
}

/* Write an elf, coff or "binary" file */
static int tcc_write_elf_file(TCCState *s1, const char *filename, int phnum, ElfW(Phdr) * phdr)
{
  int fd, mode, file_type, ret;
  FILE *f;

  file_type = s1->output_type;
  if (file_type == TCC_OUTPUT_OBJ)
    mode = 0666;
  else
    mode = 0777;
  unlink(filename);
  fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, mode);
  if (fd < 0 || (f = fdopen(fd, "wb")) == NULL)
    return tcc_error_noabort("could not write '%s: %s'", filename, strerror(errno));
  if (s1->verbose)
    printf("<- %s\n", filename);
#ifdef TCC_TARGET_COFF
  if (s1->output_format == TCC_OUTPUT_FORMAT_COFF)
    tcc_output_coff(s1, f);
  else
#endif
#ifdef TCC_TARGET_YAFF
      if (s1->output_format == TCC_OUTPUT_FORMAT_YAFF)
    ret = tcc_output_yaff(s1, f, filename);
  else
#endif
      if (s1->output_format == TCC_OUTPUT_FORMAT_ELF)
    ret = tcc_output_elf(s1, f, phnum, phdr);
  else
    ret = tcc_output_binary(s1, f);
  fclose(f);

  return ret;
}

#ifndef ELF_OBJ_ONLY
/* order sections according to sec_order, remove sections
   that we aren't going to output.  */
static void reorder_sections(TCCState *s1, int *sec_order)
{
  int i, nnew, k, *backmap;
  Section **snew, *s;
  ElfW(Sym) * sym;

  backmap = tcc_malloc(s1->nb_sections * sizeof(backmap[0]));
  for (i = 0, nnew = 0, snew = NULL; i < s1->nb_sections; i++)
  {
    k = sec_order[i];
    s = s1->sections[k];
    if (!i || s->sh_name)
    {
      backmap[k] = nnew;
      dynarray_add(&snew, &nnew, s);
    }
    else
    {
      backmap[k] = 0;
      /* just remember to free them later */
      dynarray_add(&s1->priv_sections, &s1->nb_priv_sections, s);
    }
  }
  for (i = 1; i < nnew; i++)
  {
    s = snew[i];
    s->sh_num = i;
    if (s->sh_type == SHT_RELX)
      s->sh_info = backmap[s->sh_info];
    else if (s->sh_type == SHT_SYMTAB || s->sh_type == SHT_DYNSYM)
      for_each_elem(s, 1, sym, ElfW(Sym)) if (sym->st_shndx < s1->nb_sections) sym->st_shndx = backmap[sym->st_shndx];
  }
  tcc_free(s1->sections);
  s1->sections = snew;
  s1->nb_sections = nnew;
  tcc_free(backmap);
}

#ifdef TCC_TARGET_ARM
static void create_arm_attribute_section(TCCState *s1)
{
  // Needed for DLL support.
  static const unsigned char arm_attr[] = {
      0x41,                               // 'A'
      0x2c, 0x00, 0x00, 0x00,             // size 0x2c
      'a',  'e',  'a',  'b',  'i',  0x00, // "aeabi"
      0x01, 0x22, 0x00, 0x00, 0x00,       // 'File Attributes', size 0x22
      0x05, 0x36, 0x00,                   // 'CPU_name', "6"
      0x06, 0x06,                         // 'CPU_arch', 'v6'
      0x08, 0x01,                         // 'ARM_ISA_use', 'Yes'
      0x09, 0x01,                         // 'THUMB_ISA_use', 'Thumb-1'
      0x0a, 0x02,                         // 'FP_arch', 'VFPv2'
      0x12, 0x04,                         // 'ABI_PCS_wchar_t', 4
      0x14, 0x01,                         // 'ABI_FP_denormal', 'Needed'
      0x15, 0x01,                         // 'ABI_FP_exceptions', 'Needed'
      0x17, 0x03,                         // 'ABI_FP_number_model', 'IEEE 754'
      0x18, 0x01,                         // 'ABI_align_needed', '8-byte'
      0x19, 0x01,                         // 'ABI_align_preserved', '8-byte, except leaf SP'
      0x1a, 0x02,                         // 'ABI_enum_size', 'int'
      0x1c, 0x01,                         // 'ABI_VFP_args', 'VFP registers'
      0x22, 0x01                          // 'CPU_unaligned_access', 'v6'
  };
  Section *attr = new_section(s1, ".ARM.attributes", SHT_ARM_ATTRIBUTES, 0);
  unsigned char *ptr = section_ptr_add(attr, sizeof(arm_attr));
  attr->sh_addralign = 1;
  memcpy(ptr, arm_attr, sizeof(arm_attr));
  if (s1->float_abi != ARM_HARD_FLOAT)
  {
    ptr[26] = 0x00; // 'FP_arch', 'No'
    ptr[41] = 0x1e; // 'ABI_optimization_goals'
    ptr[42] = 0x06; // 'Aggressive Debug'
  }
}
#endif

#if TARGETOS_OpenBSD || TARGETOS_NetBSD
static Section *create_bsd_note_section(TCCState *s1, const char *name, const char *value)
{
  Section *s = find_section(s1, name);

  if (s->data_offset == 0)
  {
    char *ptr = section_ptr_add(s, sizeof(ElfW(Nhdr)) + 8 + 4);
    ElfW(Nhdr) *note = (ElfW(Nhdr) *)ptr;

    s->sh_type = SHT_NOTE;
    note->n_namesz = 8;
    note->n_descsz = 4;
    note->n_type = ELF_NOTE_OS_GNU;
    strcpy(ptr + sizeof(ElfW(Nhdr)), value);
  }
  return s;
}
#endif

static void alloc_sec_names(TCCState *s1, int is_obj);
static void ld_apply_symbols(TCCState *s1, LDScript *ld);
static void ld_update_symbol_values(TCCState *s1, LDScript *ld);
ST_FUNC void ld_export_standard_symbols(TCCState *s1);

/* --gc-sections implementation: remove unused sections */
static void gc_sections(TCCState *s1)
{
  int i, sym_index, changed;
  Section *s, *sr;
  ElfW(Sym) * sym, *symtab;
  ElfW_Rel *rel;
  unsigned char *sec_used;
  int nb_syms;
  const char *name;

  /* Allocate array to track which sections are used */
  sec_used = tcc_mallocz(s1->nb_sections);

  /* Always keep certain essential sections */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (!s)
      continue;
    /* Keep symtab, strtab, shstrtab, and relocation sections */
    if (s->sh_type == SHT_SYMTAB || s->sh_type == SHT_STRTAB || s->sh_type == SHT_HASH || s->sh_type == SHT_DYNSYM ||
        s->sh_type == SHT_GNU_HASH || s->sh_type == SHT_GNU_versym || s->sh_type == SHT_GNU_verneed ||
        s->sh_type == SHT_GNU_verdef || s->sh_type == SHT_RELX || s->sh_type == SHT_DYNAMIC || s->sh_type == SHT_NOTE)
    {
      sec_used[i] = 1;
      continue;
    }
    /* Keep init/fini arrays and special sections */
    if (!strcmp(s->name, ".init") || !strcmp(s->name, ".fini") || !strcmp(s->name, ".init_array") ||
        !strcmp(s->name, ".fini_array") || !strcmp(s->name, ".preinit_array") || !strcmp(s->name, ".ctors") ||
        !strcmp(s->name, ".dtors") || !strcmp(s->name, ".got") || !strcmp(s->name, ".got.plt") ||
        !strcmp(s->name, ".plt") || !strcmp(s->name, ".interp") || !strcmp(s->name, ".eh_frame") ||
        !strcmp(s->name, ".eh_frame_hdr") || !strcmp(s->name, ".ARM.attributes") || !strcmp(s->name, ".ARM.exidx"))
    {
      sec_used[i] = 1;
      continue;
    }
    /* Keep sections marked with KEEP() in linker script */
    if (s1->ld_script && ld_section_should_keep(s1->ld_script, s->name))
    {
      sec_used[i] = 1;
      continue;
    }
    /* Keep debug sections if debugging enabled */
    if (s1->do_debug && !strncmp(s->name, ".debug", 6))
    {
      sec_used[i] = 1;
      continue;
    }
    /* Keep sections that are not SHF_ALLOC (like comments) if not allocatable
     */
    if (!(s->sh_flags & SHF_ALLOC))
    {
      sec_used[i] = 1;
      continue;
    }
  }

  /* Mark sections containing entry point and other root symbols */
  symtab = (ElfW(Sym) *)symtab_section->data;
  nb_syms = symtab_section->data_offset / sizeof(ElfW(Sym));

  for (sym_index = 1; sym_index < nb_syms; sym_index++)
  {
    sym = &symtab[sym_index];
    if (sym->st_shndx == SHN_UNDEF || sym->st_shndx >= SHN_LORESERVE)
      continue;
    if (sym->st_shndx >= s1->nb_sections)
      continue;

    name = (char *)symtab_section->link->data + sym->st_name;

    /* Mark entry point section */
    if (s1->elf_entryname && !strcmp(name, s1->elf_entryname))
    {
      sec_used[sym->st_shndx] = 1;
      continue;
    }
    if (!strcmp(name, "_start") || !strcmp(name, "main") || !strcmp(name, "_main") || !strcmp(name, "__start"))
    {
      sec_used[sym->st_shndx] = 1;
      continue;
    }
    /* Mark global/weak symbols that are exported */
    if (s1->rdynamic && ELFW(ST_BIND)(sym->st_info) != STB_LOCAL)
    {
      sec_used[sym->st_shndx] = 1;
      continue;
    }
  }

  /* Iteratively mark sections referenced by relocations from used sections */
  do
  {
    changed = 0;
    for (i = 1; i < s1->nb_sections; i++)
    {
      sr = s1->sections[i];
      if (sr->sh_type != SHT_RELX)
        continue;
      /* Get the section this relocation applies to */
      s = s1->sections[sr->sh_info];
      if (!s || !sec_used[sr->sh_info])
        continue;

      /* Iterate through relocations */
      for_each_elem(sr, 0, rel, ElfW_Rel)
      {
        sym_index = ELFW(R_SYM)(rel->r_info);
        if (sym_index == 0 || sym_index >= nb_syms)
          continue;
        sym = &symtab[sym_index];
        if (sym->st_shndx == SHN_UNDEF || sym->st_shndx >= SHN_LORESERVE)
          continue;
        if (sym->st_shndx >= s1->nb_sections)
          continue;
        if (!sec_used[sym->st_shndx])
        {
          sec_used[sym->st_shndx] = 1;
          changed = 1;
        }
      }
    }
  } while (changed);

  /* Also mark relocation sections for used sections */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (s && s->reloc && sec_used[i])
    {
      sec_used[s->reloc->sh_num] = 1;
    }
  }

  /* Remove unused sections by zeroing their data */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (!s)
      continue;
    if (!sec_used[i] && (s->sh_flags & SHF_ALLOC) && s->data_offset > 0)
    {
      if (s1->verbose)
        printf("GC: removing unused section '%s' (%d bytes)\n", s->name, (int)s->data_offset);
      /* Zero the section - it will be skipped in output */
      s->data_offset = 0;
      s->sh_size = 0;
      if (s->reloc)
      {
        s->reloc->data_offset = 0;
        s->reloc->sh_size = 0;
      }
      /* Free deferred chunks for lazy sections to save memory */
      if (s->lazy && s->has_deferred_chunks)
      {
        free_deferred_chunks(s);
        s->lazy = 0;
        s->has_deferred_chunks = 0;
      }
    }
  }

  tcc_free(sec_used);
}

/* Output an elf, coff or binary file */
/* XXX: suppress unneeded sections */
static int elf_output_file(TCCState *s1, const char *filename)
{
  int i, ret, file_type, *sec_order;
  struct dyn_inf dyninf = {0};
  Section *interp, *dynstr, *dynamic;
  int textrel, got_sym, dt_flags_1;

  file_type = s1->output_type;
  s1->nb_errors = 0;
  ret = -1;
  interp = dynstr = dynamic = NULL;
  sec_order = NULL;
  dyninf.roinf = &dyninf._roinf;

  /* Load linker script if specified */
  if (s1->linker_script)
  {
    if (tcc_load_linker_script(s1, s1->linker_script) < 0)
      return -1;
    /* Apply linker script symbols early so they're available for resolution.
     * Values for symbols in NOLOAD sections will be updated after layout. */
    ld_apply_symbols(s1, s1->ld_script);
  }

#ifdef TCC_TARGET_ARM
  create_arm_attribute_section(s1);
#endif

#if TARGETOS_OpenBSD
  dyninf.note = create_bsd_note_section(s1, ".note.openbsd.ident", "OpenBSD");
#endif

#if TARGETOS_NetBSD
  dyninf.note = create_bsd_note_section(s1, ".note.netbsd.ident", "NetBSD");
#endif

#if TARGETOS_FreeBSD || TARGETOS_NetBSD
  dyninf.roinf = NULL;
#endif
  /* if linking, also link in runtime libraries (libc, libgcc, etc.) */
  tcc_add_runtime(s1);
  resolve_common_syms(s1);

  /* Phase 2: Garbage Collection During Loading - mark and load referenced sections */
  if (s1->gc_sections_aggressive)
  {
    tcc_gc_mark_phase(s1);
    tcc_load_referenced_sections(s1);
    tcc_free_lazy_objfiles(s1);
  }

  /* Garbage collect unused sections if requested (skip if aggressive GC already ran) */
  if (s1->gc_sections && !s1->gc_sections_aggressive)
  {
    gc_sections(s1);
  }

  if (!s1->static_link)
  {
    if (file_type & TCC_OUTPUT_EXE)
    {
      /* allow override the dynamic loader */
      const char *elfint = getenv("LD_SO");
      if (elfint == NULL)
        elfint = DEFAULT_ELFINTERP(s1);
      /* add interpreter section only if executable */
      // interp = new_section(s1, ".interp", SHT_PROGBITS, SHF_ALLOC);
      // interp->sh_addralign = 1;
      // ptr = section_ptr_add(interp, 1 + strlen(elfint));
      // strcpy(ptr, elfint);
      dyninf.interp = interp;
    }

    /* add dynamic symbol table */
    s1->dynsym = new_symtab(s1, ".dynsym", SHT_DYNSYM, SHF_ALLOC, ".dynstr", ".hash", SHF_ALLOC);
    /* Number of local symbols (readelf complains if not set) */
    s1->dynsym->sh_info = 1;
    dynstr = s1->dynsym->link;
    /* add dynamic section */
    dynamic = new_section(s1, ".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE);
    dynamic->link = dynstr;
    dynamic->sh_entsize = sizeof(ElfW(Dyn));

    got_sym = build_got(s1);
    if (file_type & TCC_OUTPUT_EXE)
    {
      bind_exe_dynsyms(s1, file_type & TCC_OUTPUT_DYN);
      if (s1->nb_errors)
        goto the_end;
    }
    build_got_entries(s1, got_sym);
    if (file_type & TCC_OUTPUT_EXE)
    {
      bind_libs_dynsyms(s1);
    }
    else
    {
      /* shared library case: simply export all global symbols */
      export_global_syms(s1);
    }
#if TCC_EH_FRAME
    /* fill with initial data */
    tcc_eh_frame_hdr(s1, 0);
#endif
    dyninf.gnu_hash = create_gnu_hash(s1);
  }
  else
  {
    build_got_entries(s1, 0);
  }
  version_add(s1);

  textrel = set_sec_sizes(s1);

  if (!s1->static_link)
  {
    /* add a list of needed dlls */
    for (i = 0; i < s1->nb_loaded_dlls; i++)
    {
      DLLReference *dllref = s1->loaded_dlls[i];
      if (dllref->level == 0)
        put_dt(dynamic, DT_NEEDED, put_elf_str(dynstr, dllref->name));
    }

    if (s1->rpath)
      put_dt(dynamic, s1->enable_new_dtags ? DT_RUNPATH : DT_RPATH, put_elf_str(dynstr, s1->rpath));

    dt_flags_1 = DF_1_NOW;
    if (file_type & TCC_OUTPUT_DYN)
    {
      if (s1->soname)
        put_dt(dynamic, DT_SONAME, put_elf_str(dynstr, s1->soname));
      /* XXX: currently, since we do not handle PIC code, we
         must relocate the readonly segments */
      if (textrel)
        put_dt(dynamic, DT_TEXTREL, 0);
      if (file_type & TCC_OUTPUT_EXE)
        dt_flags_1 = DF_1_NOW | DF_1_PIE;
    }
    put_dt(dynamic, DT_FLAGS, DF_BIND_NOW);
    put_dt(dynamic, DT_FLAGS_1, dt_flags_1);
    if (s1->symbolic)
      put_dt(dynamic, DT_SYMBOLIC, 0);

    dyninf.dynamic = dynamic;
    dyninf.dynstr = dynstr;
    /* remember offset and reserve space for 2nd call below */
    dyninf.data_offset = dynamic->data_offset;
    fill_dynamic(s1, &dyninf);
    dynamic->sh_size = dynamic->data_offset;
    dynstr->sh_size = dynstr->data_offset;
  }

  /* create and fill .shstrtab section */
  alloc_sec_names(s1, 0);
  /* this array is used to reorder sections in the output file */
  sec_order = tcc_malloc(sizeof(int) * 2 * s1->nb_sections);
  /* compute section to program header mapping */
  layout_sections(s1, sec_order, &dyninf);

  /* Export standard linker symbols after layout (addresses now known) */
  /* Skip if linker script is loaded - it provides its own symbol definitions */
  if (!s1->ld_script)
  {
    ld_export_standard_symbols(s1);
  }

  /* Update and apply linker script symbols with final addresses */
  if (s1->ld_script)
  {
    ld_update_symbol_values(s1, s1->ld_script);
    ld_apply_symbols(s1, s1->ld_script);

    /* Fix p_paddr for LOAD segments of sections with AT > (LMA != VMA).
       The boot code copies .data from the LMA (Flash) to the VMA (RAM),
       so the ELF loader must place the content at the LMA, not the VMA. */
    if (s1->ld_script->has_loadaddrs)
    {
      LDScript *ld = s1->ld_script;
      int j, k;
      for (j = 0; j < ld->nb_output_sections; j++)
      {
        if (ld->output_sections[j].load_memory_region_idx >= 0 && ld->output_sections[j].memory_region_idx >= 0 &&
            ld->output_sections[j].load_memory_region_idx != ld->output_sections[j].memory_region_idx)
        {
          /* This output section has AT > (LMA in different region than VMA).
             Find the LOAD segment whose p_vaddr matches and fix p_paddr. */
          addr_t vma = 0;
          /* Find the VMA of this output section from its first ELF section */
          for (k = 1; k < s1->nb_sections; k++)
          {
            Section *sec = s1->sections[k];
            if (sec->sh_addr && ld_section_matches_output(s1, sec->name, j))
            {
              vma = sec->sh_addr;
              break;
            }
          }
          if (vma)
          {
            for (k = 0; k < dyninf.phnum; k++)
            {
              ElfW(Phdr) *ph = &dyninf.phdr[k];
              if (ph->p_type == PT_LOAD && ph->p_vaddr <= vma && vma < ph->p_vaddr + ph->p_memsz)
              {
                ph->p_paddr = ld->output_section_loadaddrs[j];
                break;
              }
            }
          }
        }
      }
    }
  }

  if (dynamic)
  {
    /* put in GOT the dynamic section address and relocate PLT */
    write32le(s1->got->data, dynamic->sh_addr);
    if (file_type == TCC_OUTPUT_EXE || (RELOCATE_DLLPLT && (file_type & TCC_OUTPUT_DYN)))
      relocate_plt(s1);
    /* relocate symbols in .dynsym now that final addresses are known */
    relocate_syms(s1, s1->dynsym, 2);
  }

  /* if building executable or DLL, then relocate each section
     except the GOT which is already relocated */
  relocate_syms(s1, s1->symtab, 0);
  if (s1->nb_errors != 0)
    goto the_end;
  relocate_sections(s1);
  if (dynamic)
  {
    update_reloc_sections(s1, &dyninf);
    dynamic->data_offset = dyninf.data_offset;
    fill_dynamic(s1, &dyninf);
  }
  /* Perform relocation to GOT or PLT entries */
  if (file_type == TCC_OUTPUT_EXE && s1->static_link)
    fill_got(s1);
  else if (s1->got)
    fill_local_got_entries(s1);

  if (dyninf.gnu_hash)
    update_gnu_hash(s1, dyninf.gnu_hash);

  reorder_sections(s1, sec_order);
#if TCC_EH_FRAME
  /* fill with final data */
  tcc_eh_frame_hdr(s1, 1);
#endif
  /* Create the ELF file with name 'filename' */
  ret = tcc_write_elf_file(s1, filename, dyninf.phnum, dyninf.phdr);
the_end:
  tcc_free(sec_order);
  tcc_free(dyninf.phdr);
  return ret;
}
#endif /* ndef ELF_OBJ_ONLY */

/* Allocate strings for section names */
static void alloc_sec_names(TCCState *s1, int is_obj)
{
  int i;
  Section *s, *strsec;

  strsec = new_section(s1, ".shstrtab", SHT_STRTAB, 0);
  put_elf_str(strsec, "");
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (is_obj)
      s->sh_size = s->data_offset;
    if (s->sh_size || s == strsec || (s->sh_flags & SHF_ALLOC) || is_obj)
      s->sh_name = put_elf_str(strsec, s->name);
  }
  strsec->sh_size = strsec->data_offset;
}

/* Output an elf .o file */
static int elf_output_obj(TCCState *s1, const char *filename)
{
  Section *s;
  int i, ret, file_offset;
  s1->nb_errors = 0;
  /* Allocate strings for section names */
  alloc_sec_names(s1, 1);
  file_offset = (sizeof(ElfW(Ehdr)) + 3) & -4;
  file_offset += s1->nb_sections * sizeof(ElfW(Shdr));
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    file_offset = (file_offset + 15) & -16;
    s->sh_offset = file_offset;
    if (s->sh_type != SHT_NOBITS)
      file_offset += s->sh_size;
  }
  /* Create the ELF file with name 'filename' */
  ret = tcc_write_elf_file(s1, filename, 0, NULL);
  return ret;
}

LIBTCCAPI int tcc_output_file(TCCState *s, const char *filename)
{
  if (s->test_coverage)
    tcc_tcov_add_file(s, filename);
  if (s->output_type == TCC_OUTPUT_OBJ)
    return elf_output_obj(s, filename);
#ifdef TCC_TARGET_PE
  return pe_output_file(s, filename);
#elif defined TCC_TARGET_MACHO
  return macho_output_file(s, filename);
#else
  return elf_output_file(s, filename);
#endif
}

ST_FUNC ssize_t full_read(int fd, void *buf, size_t count)
{
  char *cbuf = buf;
  size_t rnum = 0;
  for (;;)
  {
    ssize_t num = read(fd, cbuf, count - rnum);
    if (num < 0)
      return num;
    if (num == 0)
      break;
    rnum += num;
    cbuf += num;
  }
  return rnum;
}

ST_FUNC void *load_data(int fd, unsigned long file_offset, unsigned long size)
{
  void *data;

  data = tcc_malloc(size);
  lseek(fd, file_offset, SEEK_SET);
  full_read(fd, data, size);
  return data;
}

/* Return the canonical section name for function/data sections.
 * Merges .text.foo -> .text, .rodata.bar -> .rodata, .data.baz -> .data, .bss.qux -> .bss
 * Returns original name if no match.
 */
static const char *get_merged_section_name(const char *name)
{
  static const struct
  {
    const char *prefix;
    const char *canonical;
    int prefix_len;
  } merge_map[] = {
      {".text.", ".text", 6},
      {".rodata.", ".rodata", 8},
      {".data.", ".data", 6},
      {".bss.", ".bss", 5},
  };
  size_t i;
  for (i = 0; i < sizeof(merge_map) / sizeof(merge_map[0]); i++)
  {
    if (!strncmp(name, merge_map[i].prefix, merge_map[i].prefix_len))
    {
      return merge_map[i].canonical;
    }
  }
  return name;
}

typedef struct SectionMergeInfo
{
  Section *s;            /* corresponding existing section */
  unsigned long offset;  /* offset of the new section in the existing section */
  uint8_t new_section;   /* true if section 's' was added */
  uint8_t link_once;     /* true if link once section */
  const char *merged_to; /* canonical name if section was merged */
} SectionMergeInfo;

ST_FUNC int tcc_object_type(int fd, ElfW(Ehdr) * h)
{
  int size = full_read(fd, h, sizeof *h);
  if (size == sizeof *h && 0 == memcmp(h, ELFMAG, 4))
  {
    if (h->e_type == ET_REL)
      return AFF_BINTYPE_REL;
    if (h->e_type == ET_DYN)
      return AFF_BINTYPE_DYN;
  }
  else if (size >= 8)
  {
    if (0 == memcmp(h, ARMAG, 8))
      return AFF_BINTYPE_AR;
#ifdef TCC_TARGET_COFF
    if (((struct filehdr *)h)->f_magic == COFF_C67_MAGIC)
      return AFF_BINTYPE_C67;
#endif
  }
  if (0 == memcmp(h, YAFFMAG, 4))
  {
    return AFF_BINTYPE_YAFF;
  }

  return 0;
}

/* load an object file and merge it with current files */
/* XXX: handle correctly stab (debug) info */
ST_FUNC int tcc_load_object_file(TCCState *s1, int fd, unsigned long file_offset)
{
  ElfW(Ehdr) ehdr;
  ElfW(Shdr) * shdr, *sh;
  unsigned long size, offset, offseti;
  int i, j, nb_syms, sym_index, ret, seencompressed;
  char *strsec, *strtab;
  int stab_index = 0, stabstr_index = 0;
  (void)stab_index;
  (void)stabstr_index;
  int *old_to_new_syms;
  char *sh_name, *name;
  SectionMergeInfo *sm_table, *sm;
  ElfW(Sym) * sym, *symtab;
  ElfW_Rel *rel;
  Section *s;

  /* Use lazy loading for aggressive GC mode */
  if (s1->gc_sections_aggressive)
  {
    return tcc_load_object_file_lazy(s1, fd, file_offset);
  }

  lseek(fd, file_offset, SEEK_SET);

  if (tcc_object_type(fd, &ehdr) != AFF_BINTYPE_REL)
  {
    goto invalid;
  }
  /* test CPU specific stuff */

  if (ehdr.e_ident[5] != ELFDATA2LSB || ehdr.e_machine != EM_TCC_TARGET)
  {
  invalid:
    return tcc_error_noabort("invalid object file");
  }
  /* read sections */
  shdr = load_data(fd, file_offset + ehdr.e_shoff, sizeof(ElfW(Shdr)) * ehdr.e_shnum);
  sm_table = tcc_mallocz(sizeof(SectionMergeInfo) * ehdr.e_shnum);

  /* load section names */
  sh = &shdr[ehdr.e_shstrndx];
  strsec = load_data(fd, file_offset + sh->sh_offset, sh->sh_size);

  /* load symtab and strtab */
  old_to_new_syms = NULL;
  symtab = NULL;
  strtab = NULL;
  nb_syms = 0;
  seencompressed = 0;
  stab_index = stabstr_index = 0;
  ret = -1;

  for (i = 1; i < ehdr.e_shnum; i++)
  {
    sh = &shdr[i];
    if (sh->sh_type == SHT_SYMTAB)
    {
      if (symtab)
      {
        tcc_error_noabort("object must contain only one symtab");
        goto the_end;
      }
      nb_syms = sh->sh_size / sizeof(ElfW(Sym));
      symtab = load_data(fd, file_offset + sh->sh_offset, sh->sh_size);
      sm_table[i].s = symtab_section;

      /* now load strtab */
      sh = &shdr[sh->sh_link];
      strtab = load_data(fd, file_offset + sh->sh_offset, sh->sh_size);
    }
    if (sh->sh_flags & SHF_COMPRESSED)
      seencompressed = 1;
  }

  /* now examine each section and try to merge its content with the
     ones in memory */
  for (i = 1; i < ehdr.e_shnum; i++)
  {
    /* no need to examine section name strtab */
    if (i == ehdr.e_shstrndx)
      continue;
    sh = &shdr[i];
    if (sh->sh_type == SHT_RELX)
      sh = &shdr[sh->sh_info];
    /* ignore sections types we do not handle (plus relocs to those) */
    sh_name = strsec + sh->sh_name;
    if (0 == strncmp(sh_name, ".debug_", 7) || 0 == strncmp(sh_name, ".stab", 5))
    {
      if (!s1->do_debug || seencompressed)
        continue;
#if !(TARGETOS_OpenBSD || TARGETOS_FreeBSD || TARGETOS_NetBSD)
    }
    else if (0 == strncmp(sh_name, ".eh_frame", 9))
    {
      if (NULL == eh_frame_section)
        continue;
#endif
    }
    else if (sh->sh_type != SHT_PROGBITS && sh->sh_type != SHT_NOTE && sh->sh_type != SHT_NOBITS &&
             sh->sh_type != SHT_PREINIT_ARRAY && sh->sh_type != SHT_INIT_ARRAY && sh->sh_type != SHT_FINI_ARRAY
#ifdef TCC_ARM_EABI
             && sh->sh_type != SHT_ARM_EXIDX
#endif
#if TARGETOS_OpenBSD || TARGETOS_FreeBSD || TARGETOS_NetBSD
             && sh->sh_type != SHT_X86_64_UNWIND
#endif
    )
      continue;

    sh = &shdr[i];
    sh_name = strsec + sh->sh_name;
    if (sh->sh_addralign < 1)
      sh->sh_addralign = 1;
    /* find corresponding section, if any */
    /* Use merged name for .text.*, .rodata.*, .data.*, .bss.* sections */
    {
      const char *lookup_name = get_merged_section_name(sh_name);
      for (j = 1; j < s1->nb_sections; j++)
      {
        s = s1->sections[j];
        if (strcmp(s->name, lookup_name))
          continue;
        if (sh->sh_type != s->sh_type && strcmp(s->name, ".eh_frame"))
        {
          tcc_error_noabort("section type conflict: %s %02x <> %02x", s->name, sh->sh_type, s->sh_type);
          goto the_end;
        }
        if (!strncmp(sh_name, ".gnu.linkonce", 13))
        {
          /* if a 'linkonce' section is already present, we
             do not add it again. It is a little tricky as
             symbols can still be defined in
             it. */
          sm_table[i].link_once = 1;
          goto next;
        }
        /* stab section tracking removed - DWARF only */
        /* Track if this section was merged (original name differs from lookup name) */
        if (strcmp(sh_name, lookup_name))
          sm_table[i].merged_to = lookup_name;
        goto found;
      }
      /* not found: create new section with merged name */
      s = new_section(s1, lookup_name, sh->sh_type, sh->sh_flags & ~SHF_GROUP);
      /* take as much info as possible from the section. sh_link and
         sh_info will be updated later */
      s->sh_addralign = sh->sh_addralign;
      s->sh_entsize = sh->sh_entsize;
      sm_table[i].new_section = 1;
      /* Track if this section was merged */
      if (strcmp(sh_name, lookup_name))
        sm_table[i].merged_to = lookup_name;
    }
  found:
    /* align start of section */
    s->data_offset += -s->data_offset & (sh->sh_addralign - 1);
    if (sh->sh_addralign > s->sh_addralign)
      s->sh_addralign = sh->sh_addralign;
    sm_table[i].offset = s->data_offset;
    sm_table[i].s = s;
    /* concatenate sections */
    size = sh->sh_size;
    if (sh->sh_type != SHT_NOBITS)
    {
      if (should_defer_section(sh_name, sh->sh_type))
      {
        /* Lazy loading: just record position for debug sections */
        unsigned long dest_off = s->data_offset;
        s->data_offset += size; /* Reserve space without allocating */

        /* Record where to load from later - include archive member offset if in archive */
        unsigned long abs_offset =
            s1->current_archive_offset ? s1->current_archive_offset + sh->sh_offset : file_offset + sh->sh_offset;
        /* Use archive path if loading from archive, otherwise use current file */
        const char *source_path = s1->current_archive_path ? s1->current_archive_path : s1->current_filename;
        /* Track source path for materialization */
        section_add_deferred(s1, s, source_path, abs_offset, size, dest_off);
      }
      else
      {
        /* Immediate loading */
        unsigned char *ptr;
        lseek(fd, file_offset + sh->sh_offset, SEEK_SET);
        /* section_ptr_add will handle allocation as needed */
        ptr = section_ptr_add(s, size);
        full_read(fd, ptr, size);
      }
    }
    else
    {
      s->data_offset += size;
    }
    /* align end of section */
    /* This is needed if we compile a c file after this */
    if (s == text_section || s == data_section || s == rodata_section || s == bss_section || s == common_section)
      s->data_offset += -s->data_offset & (s->sh_addralign - 1);
  next:;
  }

  /* stab string relocation removed - only DWARF debug info supported */

  /* second short pass to update sh_link and sh_info fields of new
     sections */
  for (i = 1; i < ehdr.e_shnum; i++)
  {
    s = sm_table[i].s;
    if (!s || !sm_table[i].new_section)
      continue;
    sh = &shdr[i];
    if (sh->sh_link > 0)
      s->link = sm_table[sh->sh_link].s;
    if (sh->sh_type == SHT_RELX)
    {
      s->sh_info = sm_table[sh->sh_info].s->sh_num;
      /* update backward link */
      s1->sections[s->sh_info]->reloc = s;
    }
  }

  /* resolve symbols */
  old_to_new_syms = tcc_mallocz(nb_syms * sizeof(int));

  sym = symtab + 1;
  for (i = 1; i < nb_syms; i++, sym++)
  {
    if (sym->st_shndx != SHN_UNDEF && sym->st_shndx < SHN_LORESERVE)
    {
      sm = &sm_table[sym->st_shndx];
      if (sm->link_once)
      {
        /* if a symbol is in a link once section, we use the
           already defined symbol. It is very important to get
           correct relocations */
        if (ELFW(ST_BIND)(sym->st_info) != STB_LOCAL)
        {
          name = strtab + sym->st_name;
          sym_index = find_elf_sym(symtab_section, name);
          if (sym_index)
            old_to_new_syms[i] = sym_index;
        }
        continue;
      }
      /* if no corresponding section added, no need to add symbol */
      if (!sm->s)
        continue;
      /* convert section number */
      sym->st_shndx = sm->s->sh_num;
      /* offset value */
      sym->st_value += sm->offset;
    }
    /* add symbol */
    name = strtab + sym->st_name;
    sym_index =
        set_elf_sym(symtab_section, sym->st_value, sym->st_size, sym->st_info, sym->st_other, sym->st_shndx, name);
    old_to_new_syms[i] = sym_index;
  }

  /* third pass to patch relocation entries */
  for (i = 1; i < ehdr.e_shnum; i++)
  {
    s = sm_table[i].s;
    if (!s)
      continue;
    sh = &shdr[i];
    offset = sm_table[i].offset;
    size = sh->sh_size;
    switch (s->sh_type)
    {
    case SHT_RELX:
      /* take relocation offset information */
      offseti = sm_table[sh->sh_info].offset;
      for (rel = (ElfW_Rel *)s->data + (offset / sizeof(*rel));
           rel < (ElfW_Rel *)s->data + ((offset + size) / sizeof(*rel)); rel++)
      {
        int type;
        unsigned sym_index;
        /* convert symbol index */
        type = ELFW(R_TYPE)(rel->r_info);
        sym_index = ELFW(R_SYM)(rel->r_info);
        /* NOTE: only one symtab assumed */
        if (sym_index >= nb_syms)
          goto invalid_reloc;
        sym_index = old_to_new_syms[sym_index];
        /* ignore link_once in rel section. */
        if (!sym_index && !sm_table[sh->sh_info].link_once
#ifdef TCC_TARGET_ARM
            && type != R_ARM_V4BX
#elif defined TCC_TARGET_RISCV64
            && type != R_RISCV_ALIGN && type != R_RISCV_RELAX
#endif
        )
        {
        invalid_reloc:
          tcc_error_noabort("Invalid relocation entry [%2d] '%s' @ %.8x", i, strsec + sh->sh_name, (int)rel->r_offset);
          goto the_end;
        }
        rel->r_info = ELFW(R_INFO)(sym_index, type);
        /* offset the relocation offset */
        rel->r_offset += offseti;
#ifdef TCC_TARGET_ARM
        /* Jumps and branches from a Thumb code to a PLT entry need
           special handling since PLT entries are ARM code.
           Unconditional bl instructions referencing PLT entries are
           handled by converting these instructions into blx
           instructions. Other case of instructions referencing a PLT
           entry require to add a Thumb stub before the PLT entry to
           switch to ARM mode. We set bit plt_thumb_stub of the
           attribute of a symbol to indicate such a case. */
        if (type == R_ARM_THM_JUMP24)
          get_sym_attr(s1, sym_index, 1)->plt_thumb_stub = 1;
#endif
      }
      break;
    default:
      break;
    }
  }

  ret = 0;
the_end:
  tcc_free(symtab);
  tcc_free(strtab);
  tcc_free(old_to_new_syms);
  tcc_free(sm_table);
  tcc_free(strsec);
  tcc_free(shdr);
  return ret;
}

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

static unsigned long long get_be(const uint8_t *b, int n)
{
  unsigned long long ret = 0;
  while (n)
    ret = (ret << 8) | *b++, --n;
  return ret;
}

static int read_ar_header(int fd, int offset, ArchiveHeader *hdr)
{
  char *p, *e;
  int len;
  lseek(fd, offset, SEEK_SET);
  len = full_read(fd, hdr, sizeof(ArchiveHeader));
  if (len != sizeof(ArchiveHeader))
    return len ? -1 : 0;
  if (memcmp(hdr->ar_fmag, ARFMAG, sizeof hdr->ar_fmag))
    return -1;
  p = hdr->ar_name;
  for (e = p + sizeof hdr->ar_name; e > p && e[-1] == ' ';)
    --e;
  *e = '\0';
  hdr->ar_size[sizeof hdr->ar_size - 1] = 0;
  return len;
}

/* load only the objects which resolve undefined symbols */
static int tcc_load_alacarte(TCCState *s1, int fd, int size, int entrysize)
{
  int i, bound, nsyms, sym_index, len, ret = -1;
  unsigned long long off;
  uint8_t *data;
  const char *ar_names, *p;
  const uint8_t *ar_index;
  ElfW(Sym) * sym;
  ArchiveHeader hdr;
  /* Save archive state for restoration */
  unsigned long saved_archive_offset = s1->current_archive_offset;
  const char *saved_archive_path = s1->current_archive_path;
  s1->current_archive_path = s1->current_filename;

  data = tcc_malloc(size);
  if (full_read(fd, data, size) != size)
    goto invalid;
  nsyms = get_be(data, entrysize);
  ar_index = data + entrysize;
  ar_names = (char *)ar_index + nsyms * entrysize;

  do
  {
    bound = 0;
    for (p = ar_names, i = 0; i < nsyms; i++, p += strlen(p) + 1)
    {
      Section *s = symtab_section;
      sym_index = find_elf_sym(s, p);
      if (!sym_index)
        continue;
      sym = &((ElfW(Sym) *)s->data)[sym_index];
      if (sym->st_shndx != SHN_UNDEF)
        continue;
      off = get_be(ar_index + i * entrysize, entrysize);
      len = read_ar_header(fd, off, &hdr);
      if (len <= 0 || memcmp(hdr.ar_fmag, ARFMAG, 2))
      {
      invalid:
        tcc_error_noabort("invalid archive");
        goto the_end;
      }
      off += len;
      if (s1->verbose == 2)
        printf("   -> %s\n", hdr.ar_name);
      /* Set archive offset for lazy loading */
      s1->current_archive_offset = (unsigned long)off;
      if (tcc_load_object_file(s1, fd, off) < 0)
        goto the_end;
      s1->current_archive_offset = saved_archive_offset;
      ++bound;
    }
  } while (bound);
  ret = 0;
the_end:
  s1->current_archive_offset = saved_archive_offset;
  s1->current_archive_path = saved_archive_path;
  tcc_free(data);
  return ret;
}

/* load a '.a' file */
ST_FUNC int tcc_load_archive(TCCState *s1, int fd, int alacarte)
{
  ArchiveHeader hdr;
  /* char magic[8]; */
  int size, len;
  unsigned long file_offset;
  ElfW(Ehdr) ehdr;
  unsigned long saved_archive_offset;
  const char *saved_archive_path;

  /* skip magic which was already checked */
  /* full_read(fd, magic, sizeof(magic)); */
  file_offset = sizeof ARMAG - 1;

  /* Save archive state for restoration */
  saved_archive_offset = s1->current_archive_offset;
  saved_archive_path = s1->current_archive_path;
  s1->current_archive_path = s1->current_filename;

  for (;;)
  {
    len = read_ar_header(fd, file_offset, &hdr);
    if (len == 0)
    {
      s1->current_archive_offset = saved_archive_offset;
      s1->current_archive_path = saved_archive_path;
      return 0;
    }
    if (len < 0)
    {
      s1->current_archive_offset = saved_archive_offset;
      s1->current_archive_path = saved_archive_path;
      return tcc_error_noabort("invalid archive");
    }
    file_offset += len;
    size = strtol(hdr.ar_size, NULL, 0);
    if (alacarte)
    {
      /* coff symbol table : we handle it */
      if (!strcmp(hdr.ar_name, "/"))
      {
        int ret = tcc_load_alacarte(s1, fd, size, 4);
        s1->current_archive_offset = saved_archive_offset;
        s1->current_archive_path = saved_archive_path;
        return ret;
      }
      if (!strcmp(hdr.ar_name, "/SYM64/"))
      {
        int ret = tcc_load_alacarte(s1, fd, size, 8);
        s1->current_archive_offset = saved_archive_offset;
        s1->current_archive_path = saved_archive_path;
        return ret;
      }
    }
    else if (tcc_object_type(fd, &ehdr) == AFF_BINTYPE_REL)
    {
      if (s1->verbose == 2)
        printf("   -> %s\n", hdr.ar_name);
      /* Set archive offset for lazy loading */
      s1->current_archive_offset = file_offset;
      if (tcc_load_object_file(s1, fd, file_offset) < 0)
      {
        s1->current_archive_offset = saved_archive_offset;
        s1->current_archive_path = saved_archive_path;
        return -1;
      }
      s1->current_archive_offset = saved_archive_offset;
    }
    /* align to even */
    file_offset = (file_offset + size + 1) & ~1;
  }
  /* Unreachable for non-alacarte mode; required to silence
     'function might return no value' warning in TCC self-build. */
  s1->current_archive_offset = saved_archive_offset;
  s1->current_archive_path = saved_archive_path;
  return 0;
}

#ifndef ELF_OBJ_ONLY
/* Set LV[I] to the global index of sym-version (LIB,VERSION).  Maybe resizes
   LV, maybe create a new entry for (LIB,VERSION).  */
static void set_ver_to_ver(TCCState *s1, int *n, int **lv, int i, char *lib, char *version)
{
  while (i >= *n)
  {
    *lv = tcc_realloc(*lv, (*n + 1) * sizeof(**lv));
    (*lv)[(*n)++] = -1;
  }
  if ((*lv)[i] == -1)
  {
    int v, prev_same_lib = -1;
    for (v = 0; v < nb_sym_versions; v++)
    {
      if (strcmp(sym_versions[v].lib, lib))
        continue;
      prev_same_lib = v;
      if (!strcmp(sym_versions[v].version, version))
        break;
    }
    if (v == nb_sym_versions)
    {
      sym_versions = tcc_realloc(sym_versions, (v + 1) * sizeof(*sym_versions));
      sym_versions[v].lib = tcc_strdup(lib);
      sym_versions[v].version = tcc_strdup(version);
      sym_versions[v].out_index = 0;
      sym_versions[v].prev_same_lib = prev_same_lib;
      nb_sym_versions++;
    }
    (*lv)[i] = v;
  }
}

/* Associates symbol SYM_INDEX (in dynsymtab) with sym-version index
   VERNDX.  */
static void set_sym_version(TCCState *s1, int sym_index, int verndx)
{
  if (sym_index >= nb_sym_to_version)
  {
    int newelems = sym_index ? sym_index * 2 : 1;
    sym_to_version = tcc_realloc(sym_to_version, newelems * sizeof(*sym_to_version));
    memset(sym_to_version + nb_sym_to_version, -1, (newelems - nb_sym_to_version) * sizeof(*sym_to_version));
    nb_sym_to_version = newelems;
  }
  if (sym_to_version[sym_index] < 0)
    sym_to_version[sym_index] = verndx;
}

struct versym_info
{
  int nb_versyms;
  ElfW(Verdef) * verdef;
  ElfW(Verneed) * verneed;
  ElfW(Half) * versym;
  int nb_local_ver, *local_ver;
};

static void store_version(TCCState *s1, struct versym_info *v, char *dynstr)
{
  char *lib, *version;
  uint32_t next;
  int i;

#define DEBUG_VERSION 0

  if (v->versym && v->verdef)
  {
    ElfW(Verdef) *vdef = v->verdef;
    lib = NULL;
    do
    {
      ElfW(Verdaux) *verdaux = (ElfW(Verdaux) *)(((char *)vdef) + vdef->vd_aux);

#if DEBUG_VERSION
      printf("verdef: version:%u flags:%u index:%u, hash:%u\n", vdef->vd_version, vdef->vd_flags, vdef->vd_ndx,
             vdef->vd_hash);
#endif
      if (vdef->vd_cnt)
      {
        version = dynstr + verdaux->vda_name;

        if (lib == NULL)
          lib = version;
        else
          set_ver_to_ver(s1, &v->nb_local_ver, &v->local_ver, vdef->vd_ndx, lib, version);
#if DEBUG_VERSION
        printf("  verdaux(%u): %s\n", vdef->vd_ndx, version);
#endif
      }
      next = vdef->vd_next;
      vdef = (ElfW(Verdef) *)(((char *)vdef) + next);
    } while (next);
  }
  if (v->versym && v->verneed)
  {
    ElfW(Verneed) *vneed = v->verneed;
    do
    {
      ElfW(Vernaux) *vernaux = (ElfW(Vernaux) *)(((char *)vneed) + vneed->vn_aux);

      lib = dynstr + vneed->vn_file;
#if DEBUG_VERSION
      printf("verneed: %u %s\n", vneed->vn_version, lib);
#endif
      for (i = 0; i < vneed->vn_cnt; i++)
      {
        if ((vernaux->vna_other & 0x8000) == 0)
        { /* hidden */
          version = dynstr + vernaux->vna_name;
          set_ver_to_ver(s1, &v->nb_local_ver, &v->local_ver, vernaux->vna_other, lib, version);
#if DEBUG_VERSION
          printf("  vernaux(%u): %u %u %s\n", vernaux->vna_other, vernaux->vna_hash, vernaux->vna_flags, version);
#endif
        }
        vernaux = (ElfW(Vernaux) *)(((char *)vernaux) + vernaux->vna_next);
      }
      next = vneed->vn_next;
      vneed = (ElfW(Verneed) *)(((char *)vneed) + next);
    } while (next);
  }

#if DEBUG_VERSION
  for (i = 0; i < v->nb_local_ver; i++)
  {
    if (v->local_ver[i] > 0)
    {
      printf("%d: lib: %s, version %s\n", i, sym_versions[v->local_ver[i]].lib, sym_versions[v->local_ver[i]].version);
    }
  }
#endif
}
/* load a library / DLL
   'level = 0' means that the DLL is referenced by the user
   (so it should be added as DT_NEEDED in the generated ELF file) */
ST_FUNC int tcc_load_dll(TCCState *s1, int fd, const char *filename, int level)
{
  ElfW(Ehdr) ehdr;
  ElfW(Shdr) * shdr, *sh, *sh1;
  int i, nb_syms, nb_dts, sym_bind, ret = -1;
  ElfW(Sym) * sym, *dynsym;
  ElfW(Dyn) * dt, *dynamic;

  char *dynstr;
  int sym_index;
  const char *name, *soname;
  struct versym_info v;

  full_read(fd, &ehdr, sizeof(ehdr));

  /* test CPU specific stuff */
  if (ehdr.e_ident[5] != ELFDATA2LSB || ehdr.e_machine != EM_TCC_TARGET)
  {
    return tcc_error_noabort("bad architecture");
  }

  /* read sections */
  shdr = load_data(fd, ehdr.e_shoff, sizeof(ElfW(Shdr)) * ehdr.e_shnum);

  /* load dynamic section and dynamic symbols */
  nb_syms = 0;
  nb_dts = 0;
  dynamic = NULL;
  dynsym = NULL; /* avoid warning */
  dynstr = NULL; /* avoid warning */
  memset(&v, 0, sizeof v);

  for (i = 0, sh = shdr; i < ehdr.e_shnum; i++, sh++)
  {
    switch (sh->sh_type)
    {
    case SHT_DYNAMIC:
      nb_dts = sh->sh_size / sizeof(ElfW(Dyn));
      dynamic = load_data(fd, sh->sh_offset, sh->sh_size);
      break;
    case SHT_DYNSYM:
      nb_syms = sh->sh_size / sizeof(ElfW(Sym));
      dynsym = load_data(fd, sh->sh_offset, sh->sh_size);
      sh1 = &shdr[sh->sh_link];
      dynstr = load_data(fd, sh1->sh_offset, sh1->sh_size);
      break;
    case SHT_GNU_verdef:
      v.verdef = load_data(fd, sh->sh_offset, sh->sh_size);
      break;
    case SHT_GNU_verneed:
      v.verneed = load_data(fd, sh->sh_offset, sh->sh_size);
      break;
    case SHT_GNU_versym:
      v.nb_versyms = sh->sh_size / sizeof(ElfW(Half));
      v.versym = load_data(fd, sh->sh_offset, sh->sh_size);
      break;
    default:
      break;
    }
  }

  if (!dynamic)
    goto the_end;

  /* compute the real library name */
  soname = tcc_basename(filename);
  for (i = 0, dt = dynamic; i < nb_dts; i++, dt++)
    if (dt->d_tag == DT_SONAME)
      soname = dynstr + dt->d_un.d_val;

  /* if the dll is already loaded, do not load it */
  if (tcc_add_dllref(s1, soname, level)->found)
    goto ret_success;

  if (v.nb_versyms != nb_syms)
    tcc_free(v.versym), v.versym = NULL;
  else
    store_version(s1, &v, dynstr);

  /* add dynamic symbols in dynsym_section */
  for (i = 1, sym = dynsym + 1; i < nb_syms; i++, sym++)
  {
    sym_bind = ELFW(ST_BIND)(sym->st_info);
    if (sym_bind == STB_LOCAL)
      continue;
    name = dynstr + sym->st_name;
    sym_index = set_elf_sym(s1->dynsymtab_section, sym->st_value, sym->st_size, sym->st_info, sym->st_other,
                            sym->st_shndx, name);
    if (v.versym)
    {
      ElfW(Half) vsym = v.versym[i];
      if ((vsym & 0x8000) == 0 && vsym > 0 && vsym < v.nb_local_ver)
        set_sym_version(s1, sym_index, v.local_ver[vsym]);
    }
  }

  /* do not load all referenced libraries
     (recursive loading can break linking of libraries) */
  /* following DT_NEEDED is needed for the dynamic loader (libdl.so),
     but it is no longer needed, when linking a library or a program.
     When tcc output mode is OUTPUT_MEM,
     tcc calls dlopen, which handles DT_NEEDED for us */

#if 0
    for(i = 0, dt = dynamic; i < nb_dts; i++, dt++)
        if (dt->d_tag == DT_RPATH)
            tcc_add_library_path(s1, dynstr + dt->d_un.d_val);

    /* load all referenced DLLs */
    for(i = 0, dt = dynamic; i < nb_dts; i++, dt++) {
        switch(dt->d_tag) {
        case DT_NEEDED:
            name = dynstr + dt->d_un.d_val;
            if (tcc_add_dllref(s1, name, -1))
                continue;
            if (tcc_add_dll(s1, name, AFF_REFERENCED_DLL) < 0) {
                ret = tcc_error_noabort("referenced dll '%s' not found", name);
                goto the_end;
            }
        }
    }
#endif

ret_success:
  ret = 0;
the_end:
  tcc_free(dynstr);
  tcc_free(dynsym);
  tcc_free(dynamic);
  tcc_free(shdr);
  tcc_free(v.local_ver);
  tcc_free(v.verdef);
  tcc_free(v.verneed);
  tcc_free(v.versym);
  return ret;
}

#define LD_TOK_NAME 256
#define LD_TOK_EOF (-1)

static int ld_inp(TCCState *s1)
{
  char b;
  if (s1->cc != -1)
  {
    int c = s1->cc;
    s1->cc = -1;
    return c;
  }
  if (1 == read(s1->fd, &b, 1))
    return b;
  return CH_EOF;
}

/* return next ld script token */
static int ld_next(TCCState *s1, char *name, int name_size)
{
  int c, d, ch;
  char *q;

redo:
  ch = ld_inp(s1);
  switch (ch)
  {
  case ' ':
  case '\t':
  case '\f':
  case '\v':
  case '\r':
  case '\n':
    goto redo;
  case '/':
    ch = ld_inp(s1);
    if (ch == '*')
    { /* comment */
      for (d = 0;; d = ch)
      {
        ch = ld_inp(s1);
        if (ch == CH_EOF || (ch == '/' && d == '*'))
          break;
      }
      goto redo;
    }
    else
    {
      q = name;
      *q++ = '/';
      goto parse_name;
    }
    break;
  case '\\':
  /* case 'a' ... 'z': */
  case 'a':
  case 'b':
  case 'c':
  case 'd':
  case 'e':
  case 'f':
  case 'g':
  case 'h':
  case 'i':
  case 'j':
  case 'k':
  case 'l':
  case 'm':
  case 'n':
  case 'o':
  case 'p':
  case 'q':
  case 'r':
  case 's':
  case 't':
  case 'u':
  case 'v':
  case 'w':
  case 'x':
  case 'y':
  case 'z':
  /* case 'A' ... 'z': */
  case 'A':
  case 'B':
  case 'C':
  case 'D':
  case 'E':
  case 'F':
  case 'G':
  case 'H':
  case 'I':
  case 'J':
  case 'K':
  case 'L':
  case 'M':
  case 'N':
  case 'O':
  case 'P':
  case 'Q':
  case 'R':
  case 'S':
  case 'T':
  case 'U':
  case 'V':
  case 'W':
  case 'X':
  case 'Y':
  case 'Z':
  case '_':
  case '.':
  case '$':
  case '~':
    q = name;
  parse_name:
    for (;;)
    {
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            strchr("/.-_+=$:\\,~", ch)))
        break;
      if ((q - name) < name_size - 1)
      {
        *q++ = ch;
      }
      ch = ld_inp(s1);
    }
    s1->cc = ch;
    *q = '\0';
    c = LD_TOK_NAME;
    break;
  case CH_EOF:
    c = LD_TOK_EOF;
    break;
  default:
    c = ch;
    break;
  }
  return c;
}

static int ld_add_file(TCCState *s1, const char filename[])
{
  if (filename[0] == '/')
  {
    if (CONFIG_SYSROOT[0] == '\0' && tcc_add_file_internal(s1, filename, AFF_TYPE_BIN) == 0)
      return 0;
    filename = tcc_basename(filename);
  }
  return tcc_add_dll(s1, filename, AFF_PRINT_ERROR);
}

static int ld_add_file_list(TCCState *s1, const char *cmd, int as_needed)
{
  char filename[1024], libname[1016];
  int t, group, nblibs = 0, ret = 0;
  char **libs = NULL;

  group = !strcmp(cmd, "GROUP");
  if (!as_needed)
    s1->new_undef_sym = 0;
  t = ld_next(s1, filename, sizeof(filename));
  if (t != '(')
  {
    ret = tcc_error_noabort("( expected");
    goto lib_parse_error;
  }
  t = ld_next(s1, filename, sizeof(filename));
  for (;;)
  {
    libname[0] = '\0';
    if (t == LD_TOK_EOF)
    {
      ret = tcc_error_noabort("unexpected end of file");
      goto lib_parse_error;
    }
    else if (t == ')')
    {
      break;
    }
    else if (t == '-')
    {
      t = ld_next(s1, filename, sizeof(filename));
      if ((t != LD_TOK_NAME) || (filename[0] != 'l'))
      {
        ret = tcc_error_noabort("library name expected");
        goto lib_parse_error;
      }
      pstrcpy(libname, sizeof libname, &filename[1]);
      if (s1->static_link)
      {
        snprintf(filename, sizeof filename, "lib%s.a", libname);
      }
      else
      {
        snprintf(filename, sizeof filename, "lib%s.so", libname);
      }
    }
    else if (t != LD_TOK_NAME)
    {
      ret = tcc_error_noabort("filename expected");
      goto lib_parse_error;
    }
    if (!strcmp(filename, "AS_NEEDED"))
    {
      ret = ld_add_file_list(s1, cmd, 1);
      if (ret)
        goto lib_parse_error;
    }
    else
    {
      /* TODO: Implement AS_NEEDED support. */
      /*       DT_NEEDED is not used any more so ignore as_needed */
      if (1 || !as_needed)
      {
        ret = ld_add_file(s1, filename);
        if (ret)
          goto lib_parse_error;
        if (group)
        {
          /* Add the filename *and* the libname to avoid future conversions */
          dynarray_add(&libs, &nblibs, tcc_strdup(filename));
          if (libname[0] != '\0')
            dynarray_add(&libs, &nblibs, tcc_strdup(libname));
        }
      }
    }
    t = ld_next(s1, filename, sizeof(filename));
    if (t == ',')
    {
      t = ld_next(s1, filename, sizeof(filename));
    }
  }
  if (group && !as_needed)
  {
    while (s1->new_undef_sym)
    {
      int i;
      s1->new_undef_sym = 0;
      for (i = 0; i < nblibs; i++)
        ld_add_file(s1, libs[i]);
    }
  }
lib_parse_error:
  dynarray_reset(&libs, &nblibs);
  return ret;
}

/* interpret a subset of GNU ldscripts to handle the dummy libc.so
   files */
ST_FUNC int tcc_load_ldscript(TCCState *s1, int fd)
{
  char cmd[64];
  char filename[1024];
  int t, ret;

  s1->fd = fd;
  s1->cc = -1;
  for (;;)
  {
    t = ld_next(s1, cmd, sizeof(cmd));
    if (t == LD_TOK_EOF)
      return 0;
    else if (t != LD_TOK_NAME)
      return -1;
    if (!strcmp(cmd, "INPUT") || !strcmp(cmd, "GROUP"))
    {
      ret = ld_add_file_list(s1, cmd, 0);
      if (ret)
        return ret;
    }
    else if (!strcmp(cmd, "OUTPUT_FORMAT") || !strcmp(cmd, "TARGET"))
    {
      /* ignore some commands */
      t = ld_next(s1, cmd, sizeof(cmd));
      if (t != '(')
        return tcc_error_noabort("( expected");
      for (;;)
      {
        t = ld_next(s1, filename, sizeof(filename));
        if (t == LD_TOK_EOF)
        {
          return tcc_error_noabort("unexpected end of file");
        }
        else if (t == ')')
        {
          break;
        }
      }
    }
    else
    {
      return -1;
    }
  }
  return 0;
}

/* Load and parse a linker script file */
ST_FUNC int tcc_load_linker_script(TCCState *s1, const char *filename)
{
  int fd;
  int ret;

  fd = open(filename, O_RDONLY | O_BINARY);
  if (fd < 0)
  {
    return tcc_error_noabort("linker script '%s' not found", filename);
  }

  /* Allocate linker script structure if not already done */
  if (!s1->ld_script)
  {
    s1->ld_script = tcc_mallocz(sizeof(LDScript));
    ld_script_init(s1->ld_script);
  }

  ret = ld_script_parse(s1, s1->ld_script, fd);
  close(fd);

#if TCCELF_DUMP_LD_SCRIPT
  if (ret == 0 && s1->verbose)
  {
    printf("Loaded linker script: %s\n", filename);
    ld_script_dump(s1->ld_script);
  }
#endif

  /* Add standard symbols */
  if (ret == 0)
  {
    ld_script_add_standard_symbols(s1, s1->ld_script);
  }

  return ret;
}

/* Apply linker script symbols to the ELF symbol table */
static void ld_apply_symbols(TCCState *s1, LDScript *ld)
{
  int i;
  for (i = 0; i < ld->nb_symbols; i++)
  {
    LDSymbol *sym = &ld->symbols[i];
    if (sym->defined)
    {
      int sym_idx;
      int vis =
          (sym->visibility == LD_SYM_HIDDEN || sym->visibility == LD_SYM_PROVIDE_HIDDEN) ? STV_HIDDEN : STV_DEFAULT;

      /* For PROVIDE symbols, only define if not already defined */
      if (sym->visibility == LD_SYM_PROVIDE || sym->visibility == LD_SYM_PROVIDE_HIDDEN)
      {
        sym_idx = find_elf_sym(s1->symtab, sym->name);
        if (sym_idx)
        {
          ElfW(Sym) *esym = &((ElfW(Sym) *)s1->symtab->data)[sym_idx];
          if (esym->st_shndx != SHN_UNDEF)
            continue; /* Already defined, skip */
        }
      }

      /* Check if symbol already exists in symtab - if so, update it */
      sym_idx = find_elf_sym(s1->symtab, sym->name);
      if (sym_idx)
      {
        ElfW(Sym) *esym = &((ElfW(Sym) *)s1->symtab->data)[sym_idx];
        esym->st_value = sym->value;
        esym->st_shndx = SHN_ABS;
      }
      else
      {
        /* Use set_elf_sym directly with SHN_ABS to ensure symbols with value 0
         * are still defined as absolute (set_global_sym treats value 0 as
         * UNDEF)
         */
        set_elf_sym(s1->symtab, sym->value, 0, ELFW(ST_INFO)(STB_GLOBAL, STT_NOTYPE), vis, SHN_ABS, sym->name);
      }

      /* Also update in dynsym if it exists there */
      if (s1->dynsym)
      {
        sym_idx = find_elf_sym(s1->dynsym, sym->name);
        if (sym_idx)
        {
          ElfW(Sym) *esym = &((ElfW(Sym) *)s1->dynsym->data)[sym_idx];
          esym->st_value = sym->value;
          esym->st_shndx = SHN_ABS;
        }
      }
    }
  }
}

/* Update linker script symbol values based on actual section layout */
static void ld_update_symbol_values(TCCState *s1, LDScript *ld)
{
  Section *s;
  addr_t bss_start = 0, bss_end = 0;
  addr_t data_start = 0, data_end = 0;
  addr_t text_start = 0, text_end = 0;
  addr_t rodata_start = 0, rodata_end = 0;
  addr_t end_addr = 0;
  addr_t sec_end;
  int i, j;
  addr_t output_section_addrs[LD_MAX_OUTPUT_SECTIONS] = {0};
  int output_section_has_addr[LD_MAX_OUTPUT_SECTIONS] = {0};
  addr_t output_section_loadaddrs[LD_MAX_OUTPUT_SECTIONS] = {0};
  addr_t output_section_sizes[LD_MAX_OUTPUT_SECTIONS] = {0};
  addr_t output_section_align[LD_MAX_OUTPUT_SECTIONS] = {0};
  addr_t output_section_vma_end[LD_MAX_OUTPUT_SECTIONS] = {0};

  /* Find section addresses and map output sections to actual addresses */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (!s->sh_addr)
      continue;

    sec_end = s->sh_addr + s->sh_size;
    if (sec_end > end_addr)
      end_addr = sec_end;

    /* Match .bss and .bss.* sections */
    if (!strcmp(s->name, ".bss") || !strncmp(s->name, ".bss.", 5))
    {
      if (bss_start == 0 || s->sh_addr < bss_start)
        bss_start = s->sh_addr;
      if (sec_end > bss_end)
        bss_end = sec_end;
    }
    /* Match .data and .data.* sections */
    else if (!strcmp(s->name, ".data") || !strncmp(s->name, ".data.", 6))
    {
      if (data_start == 0 || s->sh_addr < data_start)
        data_start = s->sh_addr;
      if (sec_end > data_end)
        data_end = sec_end;
    }
    /* Match .text and .text.* sections */
    else if (!strcmp(s->name, ".text") || !strncmp(s->name, ".text.", 6))
    {
      if (text_start == 0 || s->sh_addr < text_start)
        text_start = s->sh_addr;
      if (sec_end > text_end)
        text_end = sec_end;
    }
    /* Match .rodata and .rodata.* sections */
    else if (!strcmp(s->name, ".rodata") || !strncmp(s->name, ".rodata.", 8))
    {
      if (rodata_start == 0 || s->sh_addr < rodata_start)
        rodata_start = s->sh_addr;
      if (sec_end > rodata_end)
        rodata_end = sec_end;
    }

    /* Map output section names to addresses */
    for (j = 0; j < ld->nb_output_sections; j++)
    {
      if (!strcmp(s->name, ld->output_sections[j].name))
      {
        output_section_addrs[j] = s->sh_addr;
        output_section_has_addr[j] = 1;
        break;
      }
    }

    /* Accumulate sizes/alignments for load address computation */
    if (ld)
    {
      int pat_idx = -1;
      int ld_idx = ld_find_output_section_idx(s1, s->name, &pat_idx);
      if (ld_idx >= 0 && ld_idx < ld->nb_output_sections)
      {
        addr_t align = s->sh_addralign ? s->sh_addralign : 1;
        if (align > output_section_align[ld_idx])
          output_section_align[ld_idx] = align;
        if (s->sh_type != SHT_NOBITS)
          output_section_sizes[ld_idx] += s->sh_size;
        /* Track VMA end address (includes alignment between sections) */
        if (s->sh_addr + s->sh_size > output_section_vma_end[ld_idx])
          output_section_vma_end[ld_idx] = s->sh_addr + s->sh_size;
      }
    }
  }

  /* Compute output section load addresses (LMA) */
  if (ld)
  {
    addr_t lma_cur[LD_MAX_MEMORY_REGIONS] = {0};
    if (ld->nb_memory_regions > 0)
    {
      for (i = 0; i < ld->nb_memory_regions; i++)
        lma_cur[i] = ld->memory_regions[i].origin;
      for (j = 0; j < ld->nb_output_sections; j++)
      {
        int load_mr = ld->output_sections[j].load_memory_region_idx;
        int vma_mr = ld->output_sections[j].memory_region_idx;
        int mr = (load_mr >= 0) ? load_mr : vma_mr;
        if (mr < 0)
          mr = 0;
        if (mr >= 0 && mr < ld->nb_memory_regions)
        {
          addr_t align = output_section_align[j] ? output_section_align[j] : 1;
          addr_t cur = lma_cur[mr];
          addr_t lma_start = (cur + align - 1) & ~(align - 1);
          output_section_loadaddrs[j] = lma_start;
          /* For sections where VMA region == LMA region (no AT > directive),
             use the actual VMA end address which includes alignment padding
             between input sections. Otherwise use the raw content size. */
          if (load_mr < 0 && output_section_vma_end[j] > lma_start)
            lma_cur[mr] = output_section_vma_end[j];
          else
            lma_cur[mr] = lma_start + output_section_sizes[j];
        }
      }
    }
    else
    {
      for (j = 0; j < ld->nb_output_sections; j++)
        output_section_loadaddrs[j] = output_section_addrs[j];
    }

    /* Save computed LMA values in LDScript for p_paddr fixup */
    for (j = 0; j < ld->nb_output_sections; j++)
      ld->output_section_loadaddrs[j] = output_section_loadaddrs[j];
    ld->has_loadaddrs = 1;
  }

  /* For NOLOAD sections (like .heap, .stack) that don't have actual ELF
   * sections, compute their addresses based on the memory region they're
   * assigned to in the linker script. Track per-region end addresses. */
  addr_t mr_end[LD_MAX_MEMORY_REGIONS] = {0};

  /* Initialize memory region end addresses from laid-out sections */
  for (j = 0; j < ld->nb_output_sections; j++)
  {
    if (output_section_has_addr[j])
    {
      int mr = ld->output_sections[j].memory_region_idx;
      if (mr < 0)
        mr = 0;
      if (mr >= 0 && mr < ld->nb_memory_regions)
      {
        addr_t sec_end_addr = output_section_addrs[j] + ld->output_sections[j].current_offset;
        if (sec_end_addr > mr_end[mr])
          mr_end[mr] = sec_end_addr;
      }
    }
  }

  /* For regions with no sections, start from their origin */
  for (i = 0; i < ld->nb_memory_regions; i++)
  {
    if (mr_end[i] == 0)
      mr_end[i] = ld->memory_regions[i].origin;
  }

  /* Place NOLOAD sections in their assigned memory regions */
  for (j = 0; j < ld->nb_output_sections; j++)
  {
    if (output_section_addrs[j] == 0)
    {
      /* This output section has no matching ELF section (NOLOAD) */
      int mr = ld->output_sections[j].memory_region_idx;
      if (mr < 0)
        mr = 0;
      if (mr >= 0 && mr < ld->nb_memory_regions)
      {
        output_section_addrs[j] = mr_end[mr];
        output_section_has_addr[j] = 1;
        mr_end[mr] += ld->output_sections[j].current_offset;
      }
    }
  }

  /* First pass: update symbols that are defined in output sections using
   * section_offset. This handles all symbols generically. */
  for (j = 0; j < ld->nb_symbols; j++)
  {
    LDSymbol *sym = &ld->symbols[j];
    if (sym->defined && sym->section_idx >= 0 && sym->section_idx < ld->nb_output_sections)
    {
      addr_t section_addr = output_section_addrs[sym->section_idx];
      if (output_section_has_addr[sym->section_idx])
      {
        /* Symbol value = section base address + offset within section */
        sym->value = section_addr + sym->section_offset;
      }
    }
  }

  /* Resolve LOADADDR() symbols using computed LMA addresses */
  for (j = 0; j < ld->nb_symbols; j++)
  {
    LDSymbol *sym = &ld->symbols[j];
    if (sym->has_loadaddr && sym->loadaddr_section_idx >= 0 && sym->loadaddr_section_idx < ld->nb_output_sections)
    {
      addr_t lma = output_section_loadaddrs[sym->loadaddr_section_idx];
      sym->value = lma;
      sym->defined = 1;
    }
  }

  /* Second pass: update standard section boundary symbols.
   * For boundary symbols like __data_start__/__data_end__, always use
   * the computed values based on actual section layout, because the
   * linker script values are only relative offsets that don't account
   * for all input sections. */
  for (j = 0; j < ld->nb_symbols; j++)
  {
    LDSymbol *sym = &ld->symbols[j];

    /* Update standard section symbols - these ALWAYS use computed values */
    if (!strcmp(sym->name, "__bss_start__") || !strcmp(sym->name, "__bss_start"))
    {
      sym->value = bss_start;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__bss_end__") || !strcmp(sym->name, "_bss_end__"))
    {
      sym->value = bss_end;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__data_start__"))
    {
      sym->value = data_start;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__data_end__") || !strcmp(sym->name, "_edata"))
    {
      sym->value = data_end;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__text_start__") || !strcmp(sym->name, "_stext"))
    {
      sym->value = text_start;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__text_end__") || !strcmp(sym->name, "_etext"))
    {
      sym->value = text_end;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__rodata_start__"))
    {
      sym->value = rodata_start;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__rodata_end__"))
    {
      sym->value = rodata_end;
      sym->defined = 1;
    }
    else if (!strcmp(sym->name, "__end__") || !strcmp(sym->name, "_end") || !strcmp(sym->name, "end"))
    {
      sym->value = end_addr;
      sym->defined = 1;
    }
  }
}

/* Set or update a global symbol. If the symbol already exists, update its value
   instead of trying to add it again (which would trigger "defined twice" error). */
static void set_or_update_global_sym(TCCState *s1, const char *name, addr_t value)
{
  int sym_index = find_elf_sym(symtab_section, name);
  if (sym_index)
  {
    ElfW(Sym) *esym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
    if (esym->st_shndx != SHN_UNDEF)
    {
      /* Symbol already defined - update its value */
      esym->st_value = value;
      esym->st_shndx = SHN_ABS;
      return;
    }
  }
  set_global_sym(s1, name, NULL, value);
}

/* Export standard end/heap symbols based on section layout */
ST_FUNC void ld_export_standard_symbols(TCCState *s1)
{
  Section *s;
  addr_t bss_start = 0, bss_end = 0;
  addr_t data_start = 0, data_end = 0;
  addr_t text_start = 0, text_end = 0;
  addr_t end_addr = 0;
  addr_t sec_end;
  int i;

  /* Find section addresses */
  for (i = 1; i < s1->nb_sections; i++)
  {
    s = s1->sections[i];
    if (!s->sh_addr)
      continue;

    sec_end = s->sh_addr + s->sh_size;
    if (sec_end > end_addr)
      end_addr = sec_end;

    /* Match .bss and .bss.* sections */
    if (!strcmp(s->name, ".bss") || !strncmp(s->name, ".bss.", 5))
    {
      if (bss_start == 0 || s->sh_addr < bss_start)
        bss_start = s->sh_addr;
      if (sec_end > bss_end)
        bss_end = sec_end;
    }
    /* Match .data and .data.* sections */
    else if (!strcmp(s->name, ".data") || !strncmp(s->name, ".data.", 6))
    {
      if (data_start == 0 || s->sh_addr < data_start)
        data_start = s->sh_addr;
      if (sec_end > data_end)
        data_end = sec_end;
    }
    /* Match .text and .text.* sections */
    else if (!strcmp(s->name, ".text") || !strncmp(s->name, ".text.", 6))
    {
      if (text_start == 0 || s->sh_addr < text_start)
        text_start = s->sh_addr;
      if (sec_end > text_end)
        text_end = sec_end;
    }
  }

  /* Set standard symbols, updating existing ones if already defined
     (e.g. by tcc_add_linker_symbols) */
  if (bss_start)
  {
    set_or_update_global_sym(s1, "__bss_start__", bss_start);
    set_or_update_global_sym(s1, "__bss_start", bss_start);
  }
  if (bss_end)
  {
    set_or_update_global_sym(s1, "__bss_end__", bss_end);
    set_or_update_global_sym(s1, "_bss_end__", bss_end);
  }
  if (data_start)
  {
    set_or_update_global_sym(s1, "__data_start__", data_start);
  }
  if (data_end)
  {
    set_or_update_global_sym(s1, "_edata", data_end);
    set_or_update_global_sym(s1, "__data_end__", data_end);
  }
  if (text_start)
  {
    set_or_update_global_sym(s1, "__text_start__", text_start);
    set_or_update_global_sym(s1, "_stext", text_start);
  }
  if (text_end)
  {
    set_or_update_global_sym(s1, "_etext", text_end);
    set_or_update_global_sym(s1, "__text_end__", text_end);
  }
  if (end_addr)
  {
    set_or_update_global_sym(s1, "__end__", end_addr);
    set_or_update_global_sym(s1, "_end", end_addr);
    set_or_update_global_sym(s1, "end", end_addr);
    /* Heap typically starts at end */
    set_or_update_global_sym(s1, "__heap_start__", end_addr);
  }
}

#endif /* !ELF_OBJ_ONLY */
