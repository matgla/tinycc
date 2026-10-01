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

/* ELF: loading object files and archives (whole and a la carte). */

#include "tccelf_priv.h"

/* load an object file and merge it with current files */
/* XXX: handle correctly stab (debug) info */
static Section *find_existing_section(TCCState *s1, const char *name);

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
static const char *get_merged_section_name(TCCState *s1, const char *name)
{
  /* Under --gc-sections, per-function text sections from input objects must
   * keep their identity: fold them here and section-granularity GC has
   * nothing left to collect (the .o path is exactly how the self-hosted tcc
   * links).  Same-named sections from different objects still merge, which
   * is conservative.  Survivors are folded back into one .text after GC by
   * coalesce_function_sections().  Data/rodata folding stays unconditional
   * -- their GC granularity is a separate project, and the YAFF writer
   * reads those through fixed section globals. */
  int keep_text_split = s1->gc_sections;
  if (name[0] != '.')
    return name;
  switch (name[1])
  {
  case 't':
    if (!strncmp(name, ".text.", 6))
      return keep_text_split ? name : ".text";
    break;
  case 'r':
    if (!strncmp(name, ".rodata.", 8))
      return ".rodata";
    /* Relocation sections MUST fold in lockstep with their targets.  If
     * .text.error folds into .text but .rel.text.error keeps its name, two
     * disasters follow when the in-process compile (under
     * -ffunction-sections) already created a section of that exact name:
     * the member's entries -- rebased for .text -- merge into a reloc
     * section whose sh_info points at the compiled 8-byte .text.error, and
     * the loader's back-link update leaves text_section->reloc aimed at a
     * random .rel.text.<fn>.  Both corrupt relocation application. */
    if (!strncmp(name, ".rel.text.", 10))
      return keep_text_split ? name : ".rel.text";
    if (!strncmp(name, ".rel.rodata.", 12))
      return ".rel.rodata";
    if (!strncmp(name, ".rel.data.", 10))
      return ".rel.data";
    if (!strncmp(name, ".rela.text.", 11))
      return keep_text_split ? name : ".rela.text";
    if (!strncmp(name, ".rela.rodata.", 13))
      return ".rela.rodata";
    if (!strncmp(name, ".rela.data.", 11))
      return ".rela.data";
    break;
  case 'd':
    if (!strncmp(name, ".data.", 6))
      return ".data";
    break;
  case 'b':
    if (!strncmp(name, ".bss.", 5))
      return ".bss";
    break;
  }
  return name;
}

ST_FUNC int tcc_object_type(int fd, ElfW(Ehdr) * h)
{
  int size = full_read(fd, h, sizeof *h);
  if (size == sizeof *h && 0 == memcmp(h, ELFMAG, 4) &&
      h->e_ident[EI_CLASS] == ELFCLASSW)
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

ST_FUNC int tcc_load_object_file(TCCState *s1, int fd, unsigned long file_offset)
{
  ElfW(Ehdr) ehdr;
  ElfW(Shdr) * shdr, *sh;
  unsigned long size, offset, offseti;
  int i, nb_syms, sym_index, ret, seencompressed;
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
  const char *last_lookup_name = NULL;
  Section *last_lookup_section = NULL;
  unsigned object_start = 0;

  if (s1->do_bench)
    object_start = tcc_getclock_us();

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
    if (sh_name[0] == '.' && (sh_name[1] == 'd' || sh_name[1] == 's') &&
        (0 == strncmp(sh_name, ".debug_", 7) || 0 == strncmp(sh_name, ".stab", 5)))
    {
      if (!s1->do_debug || seencompressed)
        continue;
#if !(TARGETOS_OpenBSD || TARGETOS_FreeBSD || TARGETOS_NetBSD)
    }
    else if (sh_name[1] == 'e' && 0 == strncmp(sh_name, ".eh_frame", 9))
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
      const char *lookup_name = get_merged_section_name(s1, sh_name);
      s = NULL;
      if (lookup_name == last_lookup_name)
      {
        s = last_lookup_section;
      }
      else
      {
        s = find_existing_section(s1, lookup_name);
      }
      last_lookup_name = lookup_name;
      last_lookup_section = s;
      if (s)
      {
        if (sh->sh_type != s->sh_type && strcmp(s->name, ".eh_frame"))
        {
          tcc_error_noabort("section type conflict: %s %02x <> %02x", s->name, sh->sh_type, s->sh_type);
          goto the_end;
        }
        if (sh_name[1] == 'g' && !strncmp(sh_name, ".gnu.linkonce", 13))
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
        if (sh_name != lookup_name)
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
      last_lookup_name = lookup_name;
      last_lookup_section = s;
      /* Track if this section was merged */
      if (sh_name != lookup_name)
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
  if (s1->do_bench)
  {
    unsigned elapsed = tcc_getclock_us() - object_start;
    s1->bench_object_load_time += elapsed;
    s1->bench_object_load_count++;
    if (s1->current_archive_offset)
      s1->bench_archive_member_count++;
    else
      tcc_bench_log(s1, "load-obj", s1->current_filename, elapsed);
  }
  tcc_free(symtab);
  tcc_free(strtab);
  tcc_free(old_to_new_syms);
  tcc_free(sm_table);
  tcc_free(strsec);
  tcc_free(shdr);
  return ret;
}

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

static int alacarte_member_seen(unsigned long long *loaded_members, unsigned int mask, unsigned long long offset)
{
  unsigned long long key = offset + 1;
  unsigned int idx = ((unsigned int)offset ^ (unsigned int)(offset >> 32)) & mask;

  while (loaded_members[idx])
  {
    if (loaded_members[idx] == key)
      return 1;
    idx = (idx + 1) & mask;
  }
  return 0;
}

static void alacarte_mark_member(unsigned long long *loaded_members, unsigned int mask, unsigned long long offset)
{
  unsigned long long key = offset + 1;
  unsigned int idx = ((unsigned int)offset ^ (unsigned int)(offset >> 32)) & mask;

  while (loaded_members[idx] && loaded_members[idx] != key)
  {
    idx = (idx + 1) & mask;
  }
  loaded_members[idx] = key;
}

static Section *find_existing_section(TCCState *s1, const char *name)
{
  return section_ht_find(s1, name);
}

/* Check whether any currently undefined symbol is satisfiable by cached
   archive symbol tables.  Returns 1 if at least one such undef exists,
   meaning a group rescan may still be productive. */
ST_FUNC int tcc_group_has_satisfiable_undefs(TCCState *s1)
{
  Section *s = symtab_section;
  ElfW(Sym) *syms = (ElfW(Sym) *)s->data;
  Section *hs = s->hash;
  unsigned int *sym_hcache = hs ? hs->hash_val_cache : NULL;
  int ci, ui, si;
  if (s1->nb_archive_sym_caches == 0)
    return 0;
  for (ui = 0; ui < s1->nb_undef_syms; ui++)
  {
    unsigned int h;
    int aidx;
    si = s1->undef_sym_list[ui];
    if (syms[si].st_shndx != SHN_UNDEF)
      continue;
    const char *name = (char *)s->link->data + syms[si].st_name;
    h = (sym_hcache && si < hs->hash_val_alloc) ? sym_hcache[si] : elf_hash((const unsigned char *)name);
    for (ci = 0; ci < s1->nb_archive_sym_caches; ci++)
    {
      ArchiveSymbolCache *cache = &s1->archive_sym_caches[ci];
      for (aidx = cache->ar_ht_buckets[h & cache->ar_ht_mask]; aidx >= 0; aidx = cache->ar_ht_next[aidx])
      {
        if (cache->name_hashes[aidx] == h && !strcmp(cache->sym_names[aidx], name))
          return 1;
      }
    }
  }
  return 0;
}

/* Find an existing archive symbol cache entry by filename */
static ArchiveSymbolCache *find_archive_sym_cache(TCCState *s1, const char *filename)
{
  int i;
  for (i = 0; i < s1->nb_archive_sym_caches; i++)
  {
    if (!strcmp(s1->archive_sym_caches[i].filename, filename))
      return &s1->archive_sym_caches[i];
  }
  return NULL;
}

/* Create and populate a new archive symbol cache entry */
static ArchiveSymbolCache *create_archive_sym_cache(TCCState *s1, const char *filename, int fd, int size, int entrysize)
{
  int i, nsyms, ar_ht_size;
  unsigned int loaded_member_size;
  uint8_t *data;
  const uint8_t *ar_index;
  const char *ar_names, *p;
  ArchiveSymbolCache *cache;

  data = tcc_malloc(size);
  if (full_read(fd, data, size) != size)
  {
    tcc_free(data);
    return NULL;
  }
  nsyms = get_be(data, entrysize);
  ar_index = data + entrysize;
  ar_names = (char *)ar_index + nsyms * entrysize;

  /* Grow cache array */
  s1->archive_sym_caches =
      tcc_realloc(s1->archive_sym_caches, (s1->nb_archive_sym_caches + 1) * sizeof(ArchiveSymbolCache));
  cache = &s1->archive_sym_caches[s1->nb_archive_sym_caches++];
  memset(cache, 0, sizeof(*cache));

  cache->filename = tcc_strdup(filename);
  cache->data = data;
  cache->nsyms = nsyms;
  cache->entrysize = entrysize;

  /* Allocate symbol arrays */
  cache->sym_names = tcc_malloc(nsyms * sizeof(const char *));
  cache->name_hashes = tcc_malloc(nsyms * sizeof(unsigned int));
  cache->member_offsets = tcc_malloc(nsyms * sizeof(unsigned long long));

  /* Build chained hash table */
  ar_ht_size = 1;
  while (ar_ht_size < nsyms * 2)
    ar_ht_size <<= 1;
  cache->ar_ht_mask = ar_ht_size - 1;
  cache->ar_ht_buckets = tcc_malloc(ar_ht_size * sizeof(int));
  memset(cache->ar_ht_buckets, 0xff, ar_ht_size * sizeof(int));
  cache->ar_ht_next = tcc_malloc(nsyms * sizeof(int));
  memset(cache->ar_ht_next, 0xff, nsyms * sizeof(int));

  /* Size loaded_members dedup set */
  loaded_member_size = 256;
  if ((unsigned int)nsyms / 8 > loaded_member_size)
    loaded_member_size = (unsigned int)nsyms / 8;
  {
    unsigned int v = loaded_member_size;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    loaded_member_size = v + 1;
  }
  cache->loaded_member_mask = loaded_member_size - 1;
  cache->loaded_members = tcc_mallocz(loaded_member_size * sizeof(unsigned long long));

  /* Populate symbol arrays and hash table */
  for (p = ar_names, i = 0; i < nsyms; i++, p += strlen(p) + 1)
  {
    unsigned int h;
    int bucket;
    cache->sym_names[i] = p;
    cache->name_hashes[i] = h = elf_hash((const unsigned char *)p);
    cache->member_offsets[i] = get_be(ar_index + i * entrysize, entrysize);
    bucket = h & cache->ar_ht_mask;
    cache->ar_ht_next[i] = cache->ar_ht_buckets[bucket];
    cache->ar_ht_buckets[bucket] = i;
  }

  return cache;
}

ST_FUNC int tcc_archive_index_load(TCCState *s1, const char *path)
{
  char magic[sizeof ARMAG - 1];
  ArchiveHeader hdr;
  int fd, len, ok = 0;

  if (find_archive_sym_cache(s1, path))
    return 1;
  fd = open(path, O_RDONLY | O_BINARY);
  if (fd < 0)
    return 0;
  /* The index is the first member, named "/" (32-bit offsets) or "/SYM64/". */
  if (full_read(fd, magic, sizeof magic) == sizeof magic && memcmp(magic, ARMAG, sizeof magic) == 0 &&
      (len = read_ar_header(fd, sizeof magic, &hdr)) > 0)
  {
    int entrysize = !strcmp(hdr.ar_name, "/") ? 4 : !strcmp(hdr.ar_name, "/SYM64/") ? 8 : 0;
    if (entrysize)
      ok = create_archive_sym_cache(s1, path, fd, strtol(hdr.ar_size, NULL, 0), entrysize) != NULL;
  }
  close(fd);
  return ok;
}

ST_FUNC int tcc_archive_index_has(TCCState *s1, const char *path, const char *name)
{
  ArchiveSymbolCache *cache = find_archive_sym_cache(s1, path);
  unsigned int h;
  int aidx;
  if (!cache)
    return 0;
  h = elf_hash((const unsigned char *)name);
  for (aidx = cache->ar_ht_buckets[h & cache->ar_ht_mask]; aidx >= 0; aidx = cache->ar_ht_next[aidx])
    if (cache->name_hashes[aidx] == h && strcmp(cache->sym_names[aidx], name) == 0)
      return 1;
  return 0;
}

/* Free all cached archive symbol tables */
ST_FUNC void tcc_archive_cache_free(TCCState *s1)
{
  int i;
  for (i = 0; i < s1->nb_archive_sym_caches; i++)
  {
    ArchiveSymbolCache *c = &s1->archive_sym_caches[i];
    tcc_free(c->filename);
    tcc_free(c->loaded_members);
    tcc_free(c->member_offsets);
    tcc_free(c->name_hashes);
    tcc_free(c->sym_names);
    tcc_free(c->ar_ht_next);
    tcc_free(c->ar_ht_buckets);
    tcc_free(c->data);
  }
  tcc_free(s1->archive_sym_caches);
  s1->archive_sym_caches = NULL;
  s1->nb_archive_sym_caches = 0;
}

/* load only the objects which resolve undefined symbols */
static int tcc_load_alacarte(TCCState *s1, int fd, int size, int entrysize)
{
  int bound, len, ret = -1;
  unsigned long long off;
  ElfW(Sym) * sym;
  ArchiveHeader hdr;
  ArchiveSymbolCache *cache;
  /* Save archive state for restoration */
  unsigned long saved_archive_offset = s1->current_archive_offset;
  const char *saved_archive_path = s1->current_archive_path;
  s1->current_archive_path = s1->current_filename;

  /* Look up or create cached archive symbol table.  On --start-group
     rescans this avoids re-reading the index, re-computing elf_hash
     for all symbols, and re-allocating hash tables. */
  cache = find_archive_sym_cache(s1, s1->current_filename);
  if (!cache)
  {
    cache = create_archive_sym_cache(s1, s1->current_filename, fd, size, entrysize);
    if (!cache)
    {
      tcc_error_noabort("invalid archive");
      s1->current_archive_offset = saved_archive_offset;
      s1->current_archive_path = saved_archive_path;
      return -1;
    }
  }

  /* Inverted resolution: iterate tracked undefined symbols,
     look up each in archive hash table.  O(n_undef) per pass instead
     of O(n_symtab) in the original forward scan. */
  do
  {
    Section *s = symtab_section;
    Section *hs = s->hash;
    unsigned int *sym_hcache = hs ? hs->hash_val_cache : NULL;
    int ui, si;
    bound = 0;

    for (ui = 0; ui < s1->nb_undef_syms; ui++)
    {
      unsigned int h;
      int aidx;

      si = s1->undef_sym_list[ui];
      sym = &((ElfW(Sym) *)s->data)[si];
      if (sym->st_shndx != SHN_UNDEF)
        continue;

      /* Use cached hash from symtab hash table when available,
         avoiding elf_hash recomputation across archive calls. */
      h = (sym_hcache && si < hs->hash_val_alloc)
              ? sym_hcache[si]
              : elf_hash((const unsigned char *)((char *)s->link->data + sym->st_name));

      for (aidx = cache->ar_ht_buckets[h & cache->ar_ht_mask]; aidx >= 0; aidx = cache->ar_ht_next[aidx])
      {
        if (cache->name_hashes[aidx] != h)
          continue;
        {
          const char *sym_name = (char *)s->link->data + sym->st_name;
          if (strcmp(cache->sym_names[aidx], sym_name))
            continue;
        }
        off = cache->member_offsets[aidx];
        if (alacarte_member_seen(cache->loaded_members, cache->loaded_member_mask, off))
          break;

        len = read_ar_header(fd, off, &hdr);
        if (len <= 0 || memcmp(hdr.ar_fmag, ARFMAG, 2))
        {
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
        alacarte_mark_member(cache->loaded_members, cache->loaded_member_mask, cache->member_offsets[aidx]);
        ++bound;
        ++s1->group_rescan_loaded;
        /* symtab/hash may have been reallocated by tcc_load_object_file */
        hs = s->hash;
        sym_hcache = hs ? hs->hash_val_cache : NULL;
        break;
      }
    }
  } while (bound);
  /* Compact the list, removing symbols now defined.  Shrinks iteration
     cost for subsequent archive loads and group-rescan checks. */
  {
    ElfW(Sym) *syms = (ElfW(Sym) *)symtab_section->data;
    int ri, wi = 0;
    for (ri = 0; ri < s1->nb_undef_syms; ri++)
    {
      int idx = s1->undef_sym_list[ri];
      if (syms[idx].st_shndx == SHN_UNDEF)
        s1->undef_sym_list[wi++] = idx;
    }
    s1->nb_undef_syms = wi;
  }
  ret = 0;
the_end:
  s1->current_archive_offset = saved_archive_offset;
  s1->current_archive_path = saved_archive_path;
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
  unsigned archive_start = 0;
  unsigned members_before = 0;
  char archive_desc[1088];

  if (s1->do_bench)
  {
    archive_start = tcc_getclock_us();
    members_before = s1->bench_archive_member_count;
  }

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
      if (s1->do_bench)
      {
        unsigned elapsed = tcc_getclock_us() - archive_start;
        unsigned members_loaded = s1->bench_archive_member_count - members_before;
        s1->bench_archive_load_time += elapsed;
        s1->bench_archive_load_count++;
        snprintf(archive_desc, sizeof(archive_desc), "%s (%u members)",
                 s1->current_filename ? s1->current_filename : "<archive>", members_loaded);
        tcc_bench_log(s1, "load-archive", archive_desc, elapsed);
      }
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
        if (s1->do_bench)
        {
          unsigned elapsed = tcc_getclock_us() - archive_start;
          unsigned members_loaded = s1->bench_archive_member_count - members_before;
          s1->bench_archive_load_time += elapsed;
          s1->bench_archive_load_count++;
          snprintf(archive_desc, sizeof(archive_desc), "%s (%u members)",
                   s1->current_filename ? s1->current_filename : "<archive>", members_loaded);
          tcc_bench_log(s1, "load-archive", archive_desc, elapsed);
        }
        s1->current_archive_offset = saved_archive_offset;
        s1->current_archive_path = saved_archive_path;
        return ret;
      }
      if (!strcmp(hdr.ar_name, "/SYM64/"))
      {
        int ret = tcc_load_alacarte(s1, fd, size, 8);
        if (s1->do_bench)
        {
          unsigned elapsed = tcc_getclock_us() - archive_start;
          unsigned members_loaded = s1->bench_archive_member_count - members_before;
          s1->bench_archive_load_time += elapsed;
          s1->bench_archive_load_count++;
          snprintf(archive_desc, sizeof(archive_desc), "%s (%u members)",
                   s1->current_filename ? s1->current_filename : "<archive>", members_loaded);
          tcc_bench_log(s1, "load-archive", archive_desc, elapsed);
        }
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
