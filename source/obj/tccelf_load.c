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

/* A file of names for the linker: one per line, '#' starts a comment.  Sorted:
 * it is asked once per input section, thousands of times on the device. */
static int name_list_cmp(const void *a, const void *b)
{
  return strcmp(*(char *const *)a, *(char *const *)b);
}

ST_FUNC void tcc_name_list_sort(char **names, int count)
{
  if (count)
    tcc_qsort(names, count, sizeof(*names), name_list_cmp);
}

ST_FUNC char **tcc_name_list_read(TCCState *s1, const char *path, const char *option, int *count)
{
  FILE *f = fopen(path, "r");
  char line[256];
  char **names = NULL;
  int n = 0;
  *count = 0;
  if (!f)
  {
    tcc_error_noabort("%s: cannot read '%s'", option, path);
    return NULL;
  }
  while (fgets(line, sizeof(line), f))
  {
    line[strcspn(line, " \t\r\n#")] = 0;
    if (line[0])
      dynarray_add(&names, &n, tcc_strdup(line));
  }
  fclose(f);
  tcc_name_list_sort(names, n);
  *count = n;
  return names;
}

ST_FUNC int tcc_name_list_has(char **names, int count, const char *name, size_t len)
{
  int lo = 0, hi = count - 1;
  while (lo <= hi)
  {
    const int mid = (lo + hi) / 2;
    int c = strncmp(name, names[mid], len);
    if (!c)
      c = -(unsigned char)names[mid][len];
    if (!c)
      return 1;
    if (c < 0)
      hi = mid - 1;
    else
      lo = mid + 1;
  }
  return 0;
}

/* `name` is in the sorted list, or is with Zig's numbers dropped: the C backend
 * numbers functions (fn__1234) and inner functions (fn__func_12__345) afresh on every
 * change, so a list made from a profile names them without.  A number is "__" and
 * digits ending the name or followed by '_' ("_28..._29" spells "(...)": not one). */
ST_FUNC int tcc_name_list_has_unnumbered(char **names, int count, const char *name)
{
  char buf[256];
  size_t i = 0, o = 0, len = strlen(name);
  if (tcc_name_list_has(names, count, name, len))
    return 1;
  if (len >= sizeof(buf))
    return 0;
  while (i < len)
  {
    size_t j = i;
    if (name[i] == '_' && name[i + 1] == '_')
    {
      j = i + 2;
      if (!strncmp(name + j, "func_", 5))
        j += 5;
      if (name[j] >= '0' && name[j] <= '9')
      {
        while (name[j] >= '0' && name[j] <= '9')
          j++;
        if (name[j] == 0 || name[j] == '_')
        {
          i = j;
          continue;
        }
      }
      j = i;
    }
    buf[o++] = name[i++];
  }
  return o != len && tcc_name_list_has(names, count, buf, o);
}

/* --ram-text: a listed function's section goes where the linker script runs
 * functions from RAM, as if the source had asked for it.  Zig's C backend numbers
 * its functions (name__1234) differently on every change, so the list names them
 * without the number.  Returns the new name, kept until tcc_delete, or NULL. */
static const char *ram_text_section_name(TCCState *s1, const char *sh_name)
{
  const int rel = !strncmp(sh_name, ".rel.text.", 10);
  const char *fn = rel ? sh_name + 10 : sh_name + 6;
  size_t len, stripped;
  char *renamed;
  if (!s1->ram_text_file || (!rel && strncmp(sh_name, ".text.", 6)) || *fn == '.')
    return NULL;
  len = strlen(fn);
  stripped = len;
  while (stripped && fn[stripped - 1] >= '0' && fn[stripped - 1] <= '9')
    stripped--;
  if (stripped == len || stripped < 3 || fn[stripped - 1] != '_' || fn[stripped - 2] != '_')
    stripped = len;
  else
    stripped -= 2;
  if (!tcc_name_list_has(s1->ram_text_names, s1->nb_ram_text_names, fn, stripped) &&
      !tcc_name_list_has(s1->ram_text_names, s1->nb_ram_text_names, fn, len))
    return NULL;
  renamed = tcc_malloc(len + 32);
  snprintf(renamed, len + 32, "%s.time_critical.%s", rel ? ".rel" : "", fn);
  dynarray_add(&s1->ram_text_section_names, &s1->nb_ram_text_section_names, renamed);
  return renamed;
}

ST_FUNC ssize_t full_read(int fd, void *buf, size_t count)
{
  char *cbuf = buf;
  size_t rnum = 0;
  /* Stop once `count` bytes are in: a read() of the remaining 0 bytes would
   * only report what we already know (about half of a link's read() calls
   * were these empty tails). */
  while (rnum < count)
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

/* --gc-sections collects whole sections, so input sections stay apart until
 * after GC.  A relocatable (-r) output writes sections out as they are and
 * never runs the GC, so it keeps the classic merging. */
static int gc_keeps_input_sections(TCCState *s1)
{
  return s1->gc_sections && s1->output_type != TCC_OUTPUT_OBJ;
}

/* An object's plain .text and its data sections (and their reloc sections)
 * merged by name with every other object's would make one GC node of all of
 * them: a single function-pointer table in any object's .data pinned every
 * function any object's .data points at, and --gc-sections removed 7.5 KB
 * of the 850 KB the O0-only tcc could lose.  Under GC each object gets its
 * own section instead, named "<name>..<n>" so no lookup by name finds it;
 * the survivors are folded back into the canonical section after GC
 * (coalesce_split_sections, which folds data only from sections named so). */
static int gc_private_input_section(TCCState *s1, const char *name)
{
  if (!gc_keeps_input_sections(s1) || name[0] != '.')
    return 0;
  if (name[1] == 'r' && name[2] == 'e')
  {
    if (!strncmp(name, ".rela.", 6))
      name += 5;
    else if (!strncmp(name, ".rel.", 5))
      name += 4;
  }
  switch (name[1]) /* every input section of every object comes here */
  {
  case 't':
    return !strcmp(name, ".text");
  case 'd':
    return !strcmp(name, ".data") || !strncmp(name, ".data.", 6);
  case 'r':
    return !strcmp(name, ".rodata") || !strncmp(name, ".rodata.", 8);
  case 'b':
    return !strcmp(name, ".bss") || !strncmp(name, ".bss.", 5);
  }
  return 0;
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
   * coalesce_split_sections().  Data sections never get here under GC: see
   * gc_private_input_section. */
  /* --hot-text picks functions by these sections too, with or without GC. */
  int keep_text_split = gc_keeps_input_sections(s1) || s1->hot_text_file;
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

/* A function's bytes are read by nothing until coalesce_split_sections folds it
 * into .text: --gc-sections walks relocations.  Left on disk until then, they
 * are in memory once rather than twice, and a collected function not at all. */
static int defer_split_text(TCCState *s1, const Section *s, const ElfW(Shdr) * sh)
{
  const char *path = s1->current_archive_path ? s1->current_archive_path : s1->current_filename;

  return (s1->output_type == TCC_OUTPUT_EXE || s1->output_type == TCC_OUTPUT_DLL) && (sh->sh_flags & SHF_EXECINSTR) &&
         sh->sh_size && !strncmp(s->name, ".text.", 6) && path && strcmp(path, "-");
}

/* One block for what a link holds only until coalesce_split_sections: the Section
 * of each function and its relocations, a third of the heap at that point.
 * Freed one by one they leave holes of a few KB that .text, the largest block of a
 * link, cannot use, and it goes on top of them; freed as one they are the hole
 * .text goes in.  Sized from the objects' section headers, plus a margin for
 * archive members, which are not known yet: anything that does not fit is
 * allocated as usual, so a wrong estimate costs only some of the gain. */
#define LINK_ARENA_MIN (128 * 1024)
ST_FUNC void tcc_link_arena_prepare(TCCState *s1)
{
  unsigned long total = 0;
  int n;

  if (s1->output_type != TCC_OUTPUT_EXE && s1->output_type != TCC_OUTPUT_DLL)
    return;
  for (n = 0; n < s1->nb_files; n++)
  {
    ElfW(Ehdr) ehdr;
    ElfW(Shdr) * shdr;
    int i, fd = s1->files[n]->name[0] == '-' ? -1 : open(s1->files[n]->name, O_RDONLY | O_BINARY);

    if (fd < 0)
      continue;
    if (full_read(fd, &ehdr, sizeof ehdr) == sizeof ehdr && !memcmp(ehdr.e_ident, ELFMAG, 4) &&
        ehdr.e_type == ET_REL && ehdr.e_shentsize == sizeof(ElfW(Shdr)) && ehdr.e_shnum)
    {
      shdr = tcc_malloc(sizeof(ElfW(Shdr)) * ehdr.e_shnum);
      lseek(fd, ehdr.e_shoff, SEEK_SET);
      if (full_read(fd, shdr, sizeof(ElfW(Shdr)) * ehdr.e_shnum) == sizeof(ElfW(Shdr)) * ehdr.e_shnum)
        for (i = 1; i < ehdr.e_shnum; i++)
        {
          /* The name is a guess: the string table is not worth reading for it. */
          if (shdr[i].sh_type == SHT_RELX && !(shdr[i].sh_info < ehdr.e_shnum &&
                                                 (shdr[shdr[i].sh_info].sh_flags & SHF_EXECINSTR)))
            total += sizeof(Section) + 40 + ((shdr[i].sh_size + 7) & ~7ul);
          else if (shdr[i].sh_type == SHT_PROGBITS && (shdr[i].sh_flags & SHF_EXECINSTR))
            total += sizeof(Section) + 40;
        }
      tcc_free(shdr);
    }
    close(fd);
  }
  total += total / 8;
  if (total < LINK_ARENA_MIN)
    return;
  s1->link_arena = tcc_malloc(total);
  s1->link_arena_size = total;
  s1->link_arena_used = 0;
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
  LazyRels **lazy = NULL; /* this object's relocations left on disk, for their remap */
  int nb_lazy = 0;
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
  /* Here: an error while linking has to fail this file, the count is reset later. */
  if (s1->ram_text_file && !s1->ram_text_read)
  {
    s1->ram_text_read = 1;
    s1->ram_text_names = tcc_name_list_read(s1, s1->ram_text_file, "--ram-text", &s1->nb_ram_text_names);
    if (s1->nb_errors)
      return -1;
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
    if (tcc_typesig_section(s1, fd, file_offset, strsec + sh->sh_name, sh))
      continue;
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
    /* The relocations of a function whose bytes stay in this file stay here
     * too, read back when needed: "relocations left on disk" in tccelf.c.
     * Only when the function is already loaded, which tcc's own objects
     * always have (a section's relocations follow it). */
    if (sh->sh_type == SHT_RELX && sh->sh_info < ehdr.e_shnum && sm_table[sh->sh_info].s &&
        !sm_table[sh->sh_info].link_once && sh->sh_size % sizeof(ElfW_Rel) == 0 && !s1->gc_sections_aggressive &&
        sm_table[sh->sh_info].s->has_deferred_chunks &&
        defer_split_text(s1, sm_table[sh->sh_info].s, &shdr[sh->sh_info]))
    {
      Section *t = sm_table[sh->sh_info].s;
      LazyRels *lr = tcc_mallocz(sizeof *lr), **tail = &t->lazy_rels;
      const char *source_path = s1->current_archive_path ? s1->current_archive_path : s1->current_filename;
      lr->path = deferred_path(s1, source_path);
      lr->file_offset = s1->current_archive_offset ? s1->current_archive_offset + sh->sh_offset
                                                   : file_offset + sh->sh_offset;
      lr->count = sh->sh_size / sizeof(ElfW_Rel);
      lr->offset = sm_table[sh->sh_info].offset;
      lr->nb_syms = nb_syms;
      while (*tail)
        tail = &(*tail)->next;
      *tail = lr;
      dynarray_add(&lazy, &nb_lazy, lr);
      continue;
    }
    /* find corresponding section, if any */
    /* Use merged name for .text.*, .rodata.*, .data.*, .bss.* sections */
    if (gc_private_input_section(s1, sh_name))
    {
      char private_name[256];
      snprintf(private_name, sizeof private_name, "%s..%d", sh_name, s1->nb_sections);
      s = new_section(s1, private_name, sh->sh_type, sh->sh_flags & ~SHF_GROUP);
      s->sh_addralign = sh->sh_addralign;
      s->sh_entsize = sh->sh_entsize;
      sm_table[i].new_section = 1;
      last_lookup_name = NULL;
      last_lookup_section = NULL;
    }
    else
    {
      const char *lookup_name = ram_text_section_name(s1, sh_name);
      if (!lookup_name)
        lookup_name = get_merged_section_name(s1, sh_name);
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
      if (!strncmp(lookup_name, ".text.", 6) || !strncmp(lookup_name, ".rel.text.", 10))
        s = new_input_section(s1, lookup_name, sh->sh_type, sh->sh_flags & ~SHF_GROUP);
      else
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
      if (should_defer_section(sh_name, sh->sh_type) || defer_split_text(s1, s, sh))
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
        /* Most sections are one object's, or a function's: no room to grow.
         * section_ptr_add will handle allocation as needed */
        if (sm_table[i].new_section)
          section_reserve_exact(s, size);
        ptr = section_ptr_add(s, size);
        if (size)
        {
          lseek(fd, file_offset + sh->sh_offset, SEEK_SET);
          full_read(fd, ptr, size);
        }
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
  /* What relocations left on disk need to be read: the remap outlives this
   * function (handed over at the end, the passes below still use it). */
  for (i = 0; i < nb_lazy; i++)
    lazy[i]->remap = old_to_new_syms;

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
  if (nb_lazy && old_to_new_syms)
    dynarray_add(&s1->reloc_remaps, &s1->nb_reloc_remaps, old_to_new_syms);
  else
    tcc_free(old_to_new_syms);
  tcc_free(lazy);
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

/* The hash of an archive index's symbol names.  Every name of every archive
 * a link loads (libc.a alone has thousands) is hashed once per link, and
 * elf_hash's per-character loop was most of that work; this reads the
 * length -- which walking the name table needs anyway -- and a few
 * characters, weighted to the end since the names share prefixes
 * ("__aeabi_", "_"). Collisions only lengthen a chain: lookups compare
 * names. */
static unsigned int archive_name_hash(const char *name, size_t len)
{
  const unsigned char *u = (const unsigned char *)name;
  unsigned int h = (unsigned int)len * 0x9E3779B1u;
  if (len)
    h ^= u[len - 1] | (unsigned int)u[len >> 1] << 8 | (unsigned int)u[0] << 16;
  if (len >= 4)
    h ^= (u[len - 2] | (unsigned int)u[len - 3] << 8 | (unsigned int)u[len - 4] << 16 |
          (unsigned int)u[(len * 3) >> 2] << 24) * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 13;
  return h;
}

/* Index of the archive symbol named `name` (the one a chain walk met first,
 * as before), or -1. */
static int archive_cache_lookup(const ArchiveSymbolCache *cache, const char *name, unsigned int h)
{
  int aidx;
  for (aidx = cache->ar_ht_buckets[h & cache->ar_ht_mask]; aidx >= 0; aidx = cache->ar_ht_next[aidx])
    if (cache->name_hashes[aidx] == h && !strcmp(cache->sym_names[aidx], name))
      return aidx;
  return -1;
}

/* Check whether any currently undefined symbol is satisfiable by cached
   archive symbol tables.  Returns 1 if at least one such undef exists,
   meaning a group rescan may still be productive. */
ST_FUNC int tcc_group_has_satisfiable_undefs(TCCState *s1)
{
  Section *s = symtab_section;
  ElfW(Sym) *syms = (ElfW(Sym) *)s->data;
  int ci, ui, si;
  if (s1->nb_archive_sym_caches == 0)
    return 0;
  for (ui = 0; ui < s1->nb_undef_syms; ui++)
  {
    unsigned int h;
    si = s1->undef_sym_list[ui];
    if (syms[si].st_shndx != SHN_UNDEF)
      continue;
    const char *name = (char *)s->link->data + syms[si].st_name;
    h = archive_name_hash(name, strlen(name));
    for (ci = 0; ci < s1->nb_archive_sym_caches; ci++)
      if (archive_cache_lookup(&s1->archive_sym_caches[ci], name, h) >= 0)
        return 1;
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
  cache->ar_index = ar_index;

  /* Build chained hash table */
  ar_ht_size = 1;
  while (ar_ht_size < nsyms * 2)
    ar_ht_size <<= 1;
  cache->ar_ht_mask = ar_ht_size - 1;
  cache->ar_ht_buckets = tcc_malloc(ar_ht_size * sizeof(int));
  memset(cache->ar_ht_buckets, 0xff, ar_ht_size * sizeof(int));
  cache->ar_ht_next = tcc_malloc(nsyms * sizeof(int)); /* every entry set below */

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
  for (p = ar_names, i = 0; i < nsyms; i++)
  {
    unsigned int h;
    int bucket;
    size_t len = strlen(p);
    cache->sym_names[i] = p;
    cache->name_hashes[i] = h = archive_name_hash(p, len);
    p += len + 1;
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
  if (!cache)
    return 0;
  return archive_cache_lookup(cache, name, archive_name_hash(name, strlen(name))) >= 0;
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
    int ui, si;
    bound = 0;

    for (ui = 0; ui < s1->nb_undef_syms; ui++)
    {
      int aidx;
      const char *sym_name;

      si = s1->undef_sym_list[ui];
      sym = &((ElfW(Sym) *)s->data)[si];
      if (sym->st_shndx != SHN_UNDEF)
        continue;

      sym_name = (char *)s->link->data + sym->st_name;
      aidx = archive_cache_lookup(cache, sym_name, archive_name_hash(sym_name, strlen(sym_name)));
      if (aidx >= 0)
      {
        off = get_be(cache->ar_index + (size_t)aidx * cache->entrysize, cache->entrysize);
        if (alacarte_member_seen(cache->loaded_members, cache->loaded_member_mask, off))
          continue;

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
        alacarte_mark_member(cache->loaded_members, cache->loaded_member_mask,
                             get_be(cache->ar_index + (size_t)aidx * cache->entrysize, cache->entrysize));
        ++bound;
        ++s1->group_rescan_loaded;
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

/* ------------------------------------------------------------------------ */
/* -Wcross-tu, the link side: the type signatures of the global symbols each
 * unit defined or used (frontend/gen/decl/typesig.c), from the objects'
 * ".tcc.typesig" sections and from the units compiled into this link, with
 * the layouts of the named structs / unions those types reach.  A name whose
 * types do not match across units is reported at the link, and so is a
 * struct two of its units lay out differently. */

struct TypeSig
{
  char *name, *sig, *loc, *unit;
  int kind; /* 'D' defined, 'U' used, 'S' a struct's layout */
  int order;
};

ST_FUNC void tcc_typesig_add(TCCState *s1, const char *name, const char *sig, int kind, const char *loc,
                             const char *unit)
{
  struct TypeSig *t;
  if (!(s1->nb_typesigs & (s1->nb_typesigs - 1)))
    s1->typesigs = tcc_realloc(s1->typesigs, (s1->nb_typesigs ? 2 * s1->nb_typesigs : 16) * sizeof *t);
  t = &s1->typesigs[s1->nb_typesigs];
  t->name = tcc_strdup(name);
  t->sig = tcc_strdup(sig);
  t->loc = tcc_strdup(loc);
  t->unit = tcc_strdup(unit);
  t->kind = kind;
  t->order = s1->nb_typesigs++;
}

/* a ".tcc.typesig" section's records (name, type, kind + where, unit; each
   NUL-ended) */
static void tcc_typesig_load(TCCState *s1, const char *data, unsigned long size)
{
  const char *p = data, *end = data + size, *f[4];
  while (p < end)
  {
    if (!*p)
    {
      p++; /* padding between two merged sections (an older `-r`) */
      continue;
    }
    for (int i = 0; i < 4; i++)
    {
      const char *z = memchr(p, 0, end - p);
      if (!z)
        return; /* truncated: the rest says nothing */
      f[i] = p;
      p = z + 1;
    }
    if (f[2][0])
      tcc_typesig_add(s1, f[0], f[1], f[2][0], f[2] + 1, f[3]);
  }
}

/* an object file's ".tcc.typesig" section: read (with -Wcross-tu) and left
   out of a linked output; `-r` keeps it.  Returns 1 when it was consumed. */
ST_FUNC int tcc_typesig_section(TCCState *s1, int fd, unsigned long file_offset, const char *sh_name,
                                ElfW(Shdr) * sh)
{
  char *data;
  if (strcmp(sh_name, ".tcc.typesig") || s1->output_type == TCC_OUTPUT_OBJ)
    return 0;
  if ((s1->warn_cross_tu & WARN_ON) && sh->sh_size)
  {
    data = load_data(fd, file_offset + sh->sh_offset, sh->sh_size);
    tcc_typesig_load(s1, data, sh->sh_size);
    tcc_free(data);
  }
  return 1;
}

static int typesig_cmp(const void *a, const void *b)
{
  const struct TypeSig *x = a, *y = b;
  int c = strcmp(x->name, y->name);
  return c ? c : x->order - y->order;
}

/* A, with an open end ("R ()", "T []"), and B the same but for it */
static int typesig_open_match(const char *a, const char *b)
{
  size_t la = strlen(a), lb = strlen(b);
  char open, close;
  if (la < 3 || a[la - 3] != ' ' || lb < la)
    return 0;
  open = a[la - 2], close = a[la - 1];
  if (!((open == '(' && close == ')') || (open == '[' && close == ']')))
    return 0;
  /* the prefix names no parameter list of its own: a plain return / element type */
  for (size_t i = 0; i < la - 3; i++)
    if (a[i] == '(' || a[i] == '[')
      return 0;
  return !strncmp(a, b, la - 1) && b[lb - 1] == close;
}

static int typesig_compatible(const char *a, const char *b)
{
  return !strcmp(a, b) || typesig_open_match(a, b) || typesig_open_match(b, a);
}

/* where a record is, for a report: "at a.h:3 in a.c", "at a.c:3", "in a.c" */
static const char *typesig_where(char *buf, int size, const struct TypeSig *t)
{
  const char *colon = strrchr(t->loc, ':');
  if (!t->loc[0] || !colon)
    snprintf(buf, size, "in %s", t->unit);
  else if ((size_t)(colon - t->loc) == strlen(t->unit) && !strncmp(t->loc, t->unit, colon - t->loc))
    snprintf(buf, size, "at %s", t->loc);
  else
    snprintf(buf, size, "at %s in %s", t->loc, t->unit);
  return buf;
}

/* The struct layouts: records "struct s" / "union u", sorted with the rest */
typedef struct TsCheck
{
  TCCState *s1;
  struct TypeSig *t;
  int n;
  char **reported; /* tags already reported */
  int nb_reported;
} TsCheck;

static struct TypeSig *typesig_layout(TsCheck *c, const char *tag, const char *unit)
{
  int lo = 0, hi = c->n;
  while (lo < hi) /* the first record of TAG */
  {
    int mid = (lo + hi) / 2;
    if (strcmp(c->t[mid].name, tag) < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  for (; lo < c->n && !strcmp(c->t[lo].name, tag); lo++)
    if (c->t[lo].kind == 'S' && !strcmp(c->t[lo].unit, unit))
      return &c->t[lo];
  return NULL;
}

/* the next "struct X" / "union X" a type names, from *P: copied to TAG */
static int typesig_next_tag(const char **p, char *tag, int size)
{
  const char *s = *p;
  for (; *s; s++)
  {
    int len = !strncmp(s, "struct ", 7) ? 7 : !strncmp(s, "union ", 6) ? 6 : 0;
    const char *e;
    if (!len || (s > *p && (isid(s[-1]) || isnum(s[-1]))))
      continue;
    e = s + len;
    if (!isid(*e))
      continue; /* "struct {": written out, its members are in the text */
    while (isid(*e) || isnum(*e))
      e++;
    snprintf(tag, size, "%.*s", (int)(e - s), s);
    *p = e;
    return 1;
  }
  *p = s;
  return 0;
}

/* member I (from 0) of a layout: its name, type and offset fields */
static int typesig_member(const char *layout, int i, const char *f[3], int len[3])
{
  const char *p = strchr(layout, '\x1f');
  for (; p && i > 0; i--)
    p = strchr(p + 1, '\x1f');
  if (!p)
    return 0;
  p++;
  for (int k = 0; k < 3; k++)
  {
    const char *e = p;
    while (*e && *e != '\x1e' && *e != '\x1f')
      e++;
    f[k] = p, len[k] = e - p;
    p = *e == '\x1e' ? e + 1 : e;
  }
  return 1;
}

/* "'b' ('char *' at offset 4)" or, with SAME_NAME, "'char *' at offset 4" */
static void typesig_member_text(char *buf, int size, const char *f[3], const int len[3], int same_name)
{
  const char *dot = memchr(f[2], '.', len[2]);
  char off[48];
  if (dot)
    snprintf(off, sizeof off, "offset %.*s bit %.*s", (int)(dot - f[2]), f[2], (int)(f[2] + len[2] - dot - 1),
             dot + 1);
  else
    snprintf(off, sizeof off, "offset %.*s", len[2], f[2]);
  if (same_name)
    snprintf(buf, size, "'%.*s' at %s", len[1], f[1], off);
  else
    snprintf(buf, size, "'%.*s' ('%.*s' at %s)", len[0] ? len[0] : 11, len[0] ? f[0] : "<anonymous>", len[1], f[1],
             off);
}

/* how layouts A and B differ (their first member that does); 2 when only
   an offset or the size does (a member's own struct may be the cause) */
static int typesig_layout_diff(char *buf, int size, const struct TypeSig *a, const struct TypeSig *b)
{
  char ta[300], tb[300];
  for (int i = 0;; i++)
  {
    const char *fa[3], *fb[3];
    int la[3], lb[3];
    int ha = typesig_member(a->sig, i, fa, la), hb = typesig_member(b->sig, i, fb, lb);
    if (!ha && !hb)
    {
      snprintf(buf, size, "size/alignment %.*s in %s, %.*s in %s", (int)strcspn(a->sig, "\x1f"), a->sig, a->unit,
               (int)strcspn(b->sig, "\x1f"), b->sig, b->unit);
      return 2;
    }
    if (ha != hb)
    {
      typesig_member_text(ta, sizeof ta, ha ? fa : fb, ha ? la : lb, 0);
      snprintf(buf, size, "member %s is only in %s", ta, ha ? a->unit : b->unit);
      return 1;
    }
    if (la[0] == lb[0] && la[1] == lb[1] && la[2] == lb[2] && !memcmp(fa[0], fb[0], la[0]) &&
        !memcmp(fa[1], fb[1], la[1]) && !memcmp(fa[2], fb[2], la[2]))
      continue;
    if (la[0] == lb[0] && !memcmp(fa[0], fb[0], la[0]) && la[0])
    {
      typesig_member_text(ta, sizeof ta, fa, la, 1);
      typesig_member_text(tb, sizeof tb, fb, lb, 1);
      snprintf(buf, size, "member '%.*s' is %s in %s, %s in %s", la[0], fa[0], ta, a->unit, tb, b->unit);
    }
    else
    {
      typesig_member_text(ta, sizeof ta, fa, la, 0);
      typesig_member_text(tb, sizeof tb, fb, lb, 0);
      snprintf(buf, size, "member %d is %s in %s, %s in %s", i + 1, ta, a->unit, tb, b->unit);
    }
    return la[0] == lb[0] && la[1] == lb[1] && !memcmp(fa[0], fb[0], la[0]) && !memcmp(fa[1], fb[1], la[1]) ? 2 : 1;
  }
}

/* TAG as units UA and UB lay it out, and the tags its members name
   (depth- and cycle-guarded by SEEN); reported once per tag.  Returns 1 when
   TAG differs (reported now or before). */
static int typesig_layout_check(TsCheck *c, const char *tag, const char *ua, const char *ub, const char *user,
                                char seen[][128], int nseen)
{
  TCCState *s1 = c->s1;
  struct TypeSig *a, *b;
  const char *p;
  char sub[128], wa[300], wb[300], diff[800];
  int nested = 0, how;
  for (int i = 0; i < nseen; i++)
    if (!strcmp(seen[i], tag))
      return 0;
  for (int i = 0; i < c->nb_reported; i++)
    if (!strcmp(c->reported[i], tag))
      return 1;
  a = typesig_layout(c, tag, ua);
  b = typesig_layout(c, tag, ub);
  if (!a || !b) /* incomplete in one of them: nothing to compare */
    return 0;
  if (nseen < 16) /* deep enough */
  {
    pstrcpy(seen[nseen], 128, tag);
    for (p = a->sig; !nested && typesig_next_tag(&p, sub, sizeof sub);)
      nested = typesig_layout_check(c, sub, ua, ub, user, seen, nseen + 1);
  }
  if (!strcmp(a->sig, b->sig))
    return nested;
  how = typesig_layout_diff(diff, sizeof diff, a, b);
  if (how == 2 && nested)
    return 1; /* moved by a member's struct, which is reported */
  tcc_warning_c(warn_cross_tu)("'%s' has different layouts %s and %s ('%s' uses it): %s", tag,
                               typesig_where(wa, sizeof wa, a), typesig_where(wb, sizeof wb, b), user, diff);
  dynarray_add(&c->reported, &c->nb_reported, tcc_strdup(tag));
  return 1;
}

/* the structs signatures A and B (compatible) name, as their units lay them
   out */
static void typesig_layouts(TsCheck *c, const struct TypeSig *a, const struct TypeSig *b)
{
  char seen[16][128], tag[128];
  const char *p;
  if (!strcmp(a->unit, b->unit))
    return;
  for (p = a->sig; typesig_next_tag(&p, tag, sizeof tag);)
    if (typesig_layout_check(c, tag, a->unit, b->unit, a->name, seen, 0))
      return;
}

/* the link: compare every name's records, then forget them */
ST_FUNC void tcc_typesig_check(TCCState *s1)
{
  int n = s1->nb_typesigs;
  struct TypeSig *t = s1->typesigs;
  TsCheck c = {s1, t, n, NULL, 0};
  if (n > 1)
    tcc_qsort(t, n, sizeof *t, typesig_cmp);
  for (int i = 0; i < n;)
  {
    int j = i, ref = -1;
    while (j < n && !strcmp(t[j].name, t[i].name))
      j++;
    for (int k = i; k < j; k++)
      if (t[k].kind == 'D')
      {
        ref = k;
        break;
      }
    if (ref < 0)
      for (int k = i; k < j; k++)
        if (t[k].kind == 'U')
        {
          ref = k;
          break;
        }
    for (int k = i; ref >= 0 && k < j; k++)
    {
      struct TypeSig *r = &t[ref], *o = &t[k];
      char wr[300], wo[300];
      if (k == ref || o->kind == 'S')
        continue;
      if (typesig_compatible(r->sig, o->sig))
      {
        typesig_layouts(&c, r, o);
        continue;
      }
      typesig_where(wr, sizeof wr, r);
      typesig_where(wo, sizeof wo, o);
      if (r->kind == 'D' && o->kind == 'D')
        tcc_warning_c(warn_cross_tu)("'%s' defined as '%s' %s and as '%s' %s", r->name, r->sig, wr, o->sig, wo);
      else if (r->kind == 'D')
        tcc_warning_c(warn_cross_tu)("'%s' declared as '%s' %s, defined as '%s' %s", r->name, o->sig, wo, r->sig,
                                     wr);
      else
        tcc_warning_c(warn_cross_tu)("'%s' declared as '%s' %s and as '%s' %s", r->name, r->sig, wr, o->sig, wo);
      break; /* one report per name */
    }
    i = j;
  }
  dynarray_reset(&c.reported, &c.nb_reported);
  tcc_typesig_free(s1);
}

ST_FUNC void tcc_typesig_free(TCCState *s1)
{
  for (int i = 0; i < s1->nb_typesigs; i++)
  {
    tcc_free(s1->typesigs[i].name);
    tcc_free(s1->typesigs[i].sig);
    tcc_free(s1->typesigs[i].loc);
    tcc_free(s1->typesigs[i].unit);
  }
  tcc_free(s1->typesigs);
  s1->typesigs = NULL;
  s1->nb_typesigs = 0;
}
