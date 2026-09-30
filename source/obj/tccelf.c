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

/* ELF: TCCState ELF setup and teardown, deferred and lazily loaded
 * sections, and garbage-collection marking of referenced sections.  The rest
 * is in tccelf_{sym,reloc,layout,output,load,ld}.c; see tccelf_priv.h. */

#include "tccelf_priv.h"

static void apply_reloc_patches(Section *sec, unsigned char *data, size_t size);
static void sort_patches_by_offset(Section *sec);

#if defined(TCC_TARGET_PE)
static const char rdata[] = ".rdata";
#elif defined(TCC_TARGET_ARM_THUMB)
static const char rdata[] = ".rodata";
#else
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

/* Check if section should use lazy loading.
   Only defer debug sections — deferring everything else adds
   overhead (malloc + strdup + re-open) that exceeds the savings
   when most sections will be needed for linking anyway. */
int should_defer_section(const char *name, int sh_type)
{
  (void)sh_type;
  /* Defer DWARF debug sections */
  if (name[0] == '.' && name[1] == 'd' && strncmp(name, ".debug_", 7) == 0)
    return 1;
  /* Defer stab sections */
  if (name[0] == '.' && name[1] == 's' && strncmp(name, ".stab", 5) == 0)
    return 1;
  /* Load everything else immediately */
  return 0;
}

/* Free all deferred chunks for a section */
void free_deferred_chunks(Section *sec)
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
void section_add_deferred(TCCState *s1, Section *sec, const char *path, unsigned long file_off,
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
        add32le(buffer + buf_offset, sec->reloc_patch_values[i]);
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
int section_write_streaming(TCCState *s1, Section *sec, FILE *f)
{
  DeferredChunk *c;
  int fd;
  unsigned char buffer[1024];
  size_t to_read, n;
  int patch_idx = 0;
  size_t output_pos = 0; /* Track bytes written within this section */

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

    /* Fill alignment gap before this chunk */
    if (c->dest_offset > output_pos)
    {
      size_t gap = c->dest_offset - output_pos;

      /* Write from sec->data if it covers part of this gap (e.g. data from
         inline-assembled .S files written directly to the section buffer) */
      if (sec->data && output_pos < sec->data_allocated)
      {
        size_t from_data = sec->data_allocated - output_pos;
        if (from_data > gap)
          from_data = gap;
        fwrite(sec->data + output_pos, 1, from_data, f);
        output_pos += from_data;
        gap -= from_data;
      }

      /* Fill remaining gap with zeros */
      while (gap > 0)
      {
        n = gap < sizeof(buffer) ? gap : sizeof(buffer);
        memset(buffer, 0, n);
        fwrite(buffer, 1, n, f);
        gap -= n;
      }
      output_pos = c->dest_offset;
    }

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
    output_pos += c->size;
    close(fd);
  }

  /* Fill trailing gap to reach sh_size */
  if (output_pos < sec->sh_size)
  {
    size_t gap = sec->sh_size - output_pos;

    while (gap > 0)
    {
      n = gap < sizeof(buffer) ? gap : sizeof(buffer);
      memset(buffer, 0, n);
      fwrite(buffer, 1, n, f);
      gap -= n;
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
  free_deferred_chunks(s);     /* Clean up lazy loading metadata */
  free_reloc_patches(s);       /* Clean up relocation patches */
  tcc_free(s->str_hash);       /* Clean up string hash table */
  tcc_free(s->hash_val_cache); /* Clean up hash value cache */
  tcc_free(s->data);
  s->data = NULL;
  s->data_allocated = s->data_offset = 0;
  s->str_hash = NULL;
  s->str_hash_size = 0;
  s->str_hash_count = 0;
  s->hash_val_cache = NULL;
  s->hash_val_alloc = 0;
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
  tcc_free(s1->section_ht);
  s1->section_ht = NULL;
  s1->section_ht_mask = 0;
  s1->section_ht_count = 0;

  tcc_free(s1->undef_sym_list);
  s1->undef_sym_list = NULL;
  s1->nb_undef_syms = 0;
  s1->undef_sym_alloc = 0;

  for (i = 0; i < s1->nb_priv_sections; i++)
    free_section(s1->priv_sections[i]);
  dynarray_reset(&s1->priv_sections, &s1->nb_priv_sections);

  tcc_free(s1->sym_attrs);
  s1->sym_attrs = NULL;
  s1->nb_sym_attrs = 0;
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
