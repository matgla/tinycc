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

/* ELF: writing executables, shared objects, raw binaries and objects,
 * including section garbage collection and function-section coalescing. */

#include "tccelf_priv.h"

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
          int written = fwrite(s->data, 1, to_write, f);
          offset += written;
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
  int fd, mode, file_type, ret = 0;
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
  /* A writer that reported errors left a half-valid image; never leave it
     where a build would install it. */
  if (ret < 0 || (ret && s1->nb_errors))
    unlink(filename);

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
/* Emit the ARM EABI build-attributes section describing the code we produced.
 * Consumers (ld, gdb, objdump) use it to check ABI compatibility between
 * objects — most importantly Tag_ABI_VFP_args, which says whether FP arguments
 * travel in VFP registers (-mfloat-abi=hard) or GPRs (soft/softfp).
 *
 * Layout (ABI addenda §2.2):
 *   'A' | uint32 vendor_len | "aeabi\0" | 0x01 Tag_File | uint32 sub_len | pairs
 * where vendor_len counts everything after the 'A' (including itself) and
 * sub_len counts the Tag_File byte, its length field and the attribute pairs.
 * Every tag and value we emit is < 128, so each is a one-byte ULEB128. */
static void create_arm_attribute_section(TCCState *s1)
{
  ArmEabiAttrs a;
  arm_get_eabi_attrs(s1, &a);

  unsigned char body[96];
  int n = 0;

  body[n++] = 5; /* Tag_CPU_name (NUL-terminated string) */
  for (const char *p = a.cpu_name; *p; p++)
    body[n++] = (unsigned char)*p;
  body[n++] = 0;
  body[n++] = 6;
  body[n++] = (unsigned char)a.cpu_arch; /* Tag_CPU_arch */
  body[n++] = 7;
  body[n++] = 'M'; /* Tag_CPU_arch_profile: Microcontroller */
  body[n++] = 9;
  body[n++] = 3; /* Tag_THUMB_ISA_use: Yes.  No Tag_ARM_ISA_use:
                  * M-profile has no ARM instruction set. */
  if (a.fp_arch)
  {
    body[n++] = 10;
    body[n++] = (unsigned char)a.fp_arch; /* Tag_FP_arch */
  }
  body[n++] = 18;
  body[n++] = 4; /* Tag_ABI_PCS_wchar_t: 4 */
  body[n++] = 20;
  body[n++] = 1; /* Tag_ABI_FP_denormal: Needed */
  body[n++] = 21;
  body[n++] = 1; /* Tag_ABI_FP_exceptions: Needed */
  body[n++] = 23;
  body[n++] = 3; /* Tag_ABI_FP_number_model: IEEE 754 */
  body[n++] = 24;
  body[n++] = 1; /* Tag_ABI_align_needed: 8-byte */
  body[n++] = 25;
  body[n++] = 1; /* Tag_ABI_align_preserved: 8-byte, except leaf SP */
  body[n++] = 26;
  body[n++] = 2; /* Tag_ABI_enum_size: int (arm-none-eabi-gcc defaults to
                  * 'small' instead — a deliberate, declared difference). */
  if (a.hardfp_use)
  {
    body[n++] = 27;
    body[n++] = (unsigned char)a.hardfp_use; /* Tag_ABI_HardFP_use */
  }
  if (a.vfp_args)
  {
    body[n++] = 28;
    body[n++] = (unsigned char)a.vfp_args; /* Tag_ABI_VFP_args */
  }
  body[n++] = 30;
  body[n++] = 6; /* Tag_ABI_optimization_goals: Aggressive Debug */
  body[n++] = 34;
  body[n++] = 1; /* Tag_CPU_unaligned_access: v6 */

  const int sub_len = 1 + 4 + n;
  const int vendor_len = 4 + 6 + sub_len;

  Section *attr = new_section(s1, ".ARM.attributes", SHT_ARM_ATTRIBUTES, 0);
  attr->sh_addralign = 1;
  unsigned char *ptr = section_ptr_add(attr, 1 + vendor_len);
  int o = 0;
  ptr[o++] = 'A';
  write32le(ptr + o, vendor_len);
  o += 4;
  memcpy(ptr + o, "aeabi", 6);
  o += 6;
  ptr[o++] = 1; /* Tag_File */
  write32le(ptr + o, sub_len);
  o += 4;
  memcpy(ptr + o, body, n);
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

/* Sections --gc-sections always keeps by name: init/fini arrays and the
 * linker's own special sections.  Dispatches on the second character so the
 * hundreds of .text../.data../.rel... inputs of a link are rejected at once. */
static int gc_root_section_name(const char *name)
{
  static const char *const roots[] = {".init", ".fini", ".init_array", ".fini_array", ".preinit_array", ".ctors",
                                      ".dtors", ".got", ".got.plt", ".plt", ".interp", ".eh_frame",
                                      ".eh_frame_hdr", ".ARM.attributes", ".ARM.exidx"};
  size_t k;
  if (name[0] != '.')
    return 0;
  switch (name[1])
  {
  case 'i': case 'f': case 'p': case 'c': case 'd': case 'g': case 'e': case 'A':
    break;
  default:
    return 0;
  }
  for (k = 0; k < sizeof roots / sizeof roots[0]; k++)
    if (roots[k][1] == name[1] && !strcmp(name, roots[k]))
      return 1;
  return 0;
}

/* --gc-sections implementation: remove unused sections */
void gc_sections(TCCState *s1)
{
  int i, sym_index;
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
    if (gc_root_section_name(s->name))
    {
      sec_used[i] = 1;
      continue;
    }
    /* Keep sections marked with KEEP() in linker script */
    if (s1->ld_script)
    {
      /* A per-object section the loader renamed "<name>..<n>" is matched
       * by the name the object gave it. */
      const char *cut = strstr(s->name, "..");
      const char *match = s->name;
      char orig[256];
      if (cut && (size_t)(cut - s->name) < sizeof orig)
      {
        memcpy(orig, s->name, cut - s->name);
        orig[cut - s->name] = 0;
        match = orig;
      }
      if (ld_section_should_keep(s1->ld_script, match))
      {
        sec_used[i] = 1;
        continue;
      }
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
    if (s1->elf_entryname && name[0] == s1->elf_entryname[0] && !strcmp(name, s1->elf_entryname))
    {
      sec_used[sym->st_shndx] = 1;
      continue;
    }
    if ((name[0] == '_' || name[0] == 'm') &&
        (!strcmp(name, "_start") || !strcmp(name, "main") || !strcmp(name, "_main") || !strcmp(name, "__start")))
    {
      sec_used[sym->st_shndx] = 1;
      continue;
    }
    /* Mark global/weak symbols that are exported */
    if (s1->rdynamic && ELFW(ST_BIND)(sym->st_info) != STB_LOCAL && !elf_sym_is_module_local(sym))
    {
      sec_used[sym->st_shndx] = 1;
      continue;
    }
    /* Exported symbols are runtime roots: other yasld modules resolve
     * against them with no relocation in this image to witness the use.
     * Collecting one would leave a .dynsym entry pointing at reclaimed
     * bytes -- a silent wrong-address at runtime, not a link error.  The
     * root set is exactly what gets exported: a shared library exports
     * every global that is not hidden or internal (export_global_syms), an
     * executable only the globals a loaded library names
     * (bind_libs_dynsyms; all of them under -rdynamic, rooted above).
     * Rooting every global of an executable too pinned all of tcc, whose
     * globals were all STV_DEFAULT before -fvisibility=hidden worked, and
     * --gc-sections dropped 7.5 KB of the 850 KB that is unreachable from
     * main.  Hidden globals and statics stay collectable.  A -static link
     * has no .dynsym and nothing resolving against it later (the kernel),
     * so there every unreferenced function is collectable. */
    if (!s1->static_link && ELFW(ST_BIND)(sym->st_info) != STB_LOCAL && !elf_sym_is_module_local(sym) &&
        (!(s1->output_type & TCC_OUTPUT_EXE) || tcc_dynsym_find(s1, name)))
    {
      sec_used[sym->st_shndx] = 1;
      continue;
    }
  }

  /* Mark everything reachable through relocations from used sections: a
   * worklist of newly used sections, each scanning only the relocation
   * sections that apply to it (one pass over every relocation in total;
   * repeated full passes until nothing changed reach the same set). */
  {
    int nb = s1->nb_sections, top = 0;
    int *rel_head = tcc_malloc(nb * sizeof(int)); /* section -> first RELX section applying to it */
    int *rel_next = tcc_malloc(nb * sizeof(int));
    int *work = tcc_malloc(nb * sizeof(int));
    for (i = 0; i < nb; i++)
      rel_head[i] = -1;
    for (i = nb - 1; i >= 1; i--)
    {
      sr = s1->sections[i];
      if (!sr || sr->sh_type != SHT_RELX || sr->sh_info >= (unsigned)nb)
        continue;
      rel_next[i] = rel_head[sr->sh_info];
      rel_head[sr->sh_info] = i;
    }
    for (i = 1; i < nb; i++)
      if (sec_used[i])
        work[top++] = i;
    while (top > 0)
    {
      int t = work[--top], r;
      s = s1->sections[t];
      /* References from non-allocated sections must not confer liveness:
       * DWARF describes every function (each .text.<name> is referenced by
       * .rel.debug_info via its section symbol), so following these would
       * pin the whole program and -g would silently disable GC.  Dropped
       * targets get the SHN_ABS tombstone in the sweep instead, which is
       * how debug info for collected code is conventionally marked. */
      if (!s || !(s->sh_flags & SHF_ALLOC))
        continue;
      for (r = rel_head[t]; r >= 0; r = rel_next[r])
      {
        sr = s1->sections[r];
        for_each_elem(sr, 0, rel, ElfW_Rel)
        {
          int shndx;
          sym_index = ELFW(R_SYM)(rel->r_info);
          if (sym_index == 0 || sym_index >= nb_syms)
            continue;
          shndx = symtab[sym_index].st_shndx;
          if (shndx == SHN_UNDEF || shndx >= SHN_LORESERVE || shndx >= nb)
            continue;
          if (!sec_used[shndx])
          {
            sec_used[shndx] = 1;
            work[top++] = shndx;
          }
        }
      }
    }
    tcc_free(rel_head);
    tcc_free(rel_next);
    tcc_free(work);
  }

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
  {
    unsigned char *collected = tcc_mallocz(s1->nb_sections);
    int any_collected = 0;
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
        /* Clearing SHF_ALLOC keeps the shell out of layout AND out of
         * coalesce_split_sections, which must not fold a collected
         * .text.<name> back into .text.
         *
         * The linker's own output sections are the exception: they are only
         * emptied.  coalesce_split_sections appends every surviving
         * .text.<name> to text_section, and layout, the veneers and the YAFF
         * writer address all four through these globals.  A collected .text
         * (e.g. archive members pulled in only by dead code, all in plain
         * .text) that lost SHF_ALLOC took the live, coalesced code with it:
         * an image with no code at all, or a layout with no PT_LOAD. */
        if (s != text_section && s != data_section && s != rodata_section && s != bss_section)
          s->sh_flags &= ~SHF_ALLOC;
        collected[i] = 1;
        any_collected = 1;
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
    /* A symbol still pointing into a collected section would resolve to
     * (stale base + offset).  Exported symbols are rooted above, so only
     * locals and hidden globals land here; give them the DWARF tombstone.
     * SHN_ABS keeps relocate_syms' hands off the value, and 0xFFFFFFFF is
     * unmistakably dead -- 0 is a real code address under -Ttext=0x0, so
     * it must not be the marker. */
    if (any_collected)
    {
      for (sym_index = 1; sym_index < nb_syms; sym_index++)
      {
        sym = &symtab[sym_index];
        if (sym->st_shndx < s1->nb_sections && collected[sym->st_shndx])
        {
          sym->st_value = 0xFFFFFFFF;
          sym->st_shndx = SHN_ABS;
        }
      }
    }
    tcc_free(collected);

    /* An undefined symbol that only collected sections referred to is no
     * longer needed: GNU ld and lld report undefined symbols reached from
     * kept sections only.  The kernel's libc stdio.o calls fcntl() from a
     * function the kernel never uses, and the link failed on it.  Demote such
     * symbols to weak so the undefined-symbol checks let them through; no
     * relocation is left to read their value. */
    if (any_collected)
    {
      unsigned char *referenced = tcc_mallocz(nb_syms);
      for (i = 1; i < s1->nb_sections; i++)
      {
        sr = s1->sections[i];
        if (!sr || sr->sh_type != SHT_RELX || !sr->data_offset)
          continue;
        for_each_elem(sr, 0, rel, ElfW_Rel)
        {
          sym_index = ELFW(R_SYM)(rel->r_info);
          if (sym_index > 0 && sym_index < nb_syms)
            referenced[sym_index] = 1;
        }
      }
      for (sym_index = 1; sym_index < nb_syms; sym_index++)
      {
        sym = &symtab[sym_index];
        if (sym->st_shndx == SHN_UNDEF && !referenced[sym_index] &&
            ELFW(ST_BIND)(sym->st_info) == STB_GLOBAL)
          sym->st_info = ELFW(ST_INFO)(STB_WEAK, ELFW(ST_TYPE)(sym->st_info));
      }
      tcc_free(referenced);
    }
  }

  tcc_free(sec_used);
}

/* The canonical section a split input section folds back into, or NULL.
 * Split inputs are the per-function ".text.<fn>" sections and, under
 * --gc-sections, each object's own .text and data sections (".data..<n>",
 * ".rodata.str..<n>", see gc_private_input_section in tccelf_load.c). */
static Section *split_section_target(TCCState *s1, Section *s, Section **rodata)
{
  static const char *const base[] = {".text", ".data", ".rodata", ".bss"};
  int k;
  if (!(s->sh_flags & SHF_ALLOC) || s == text_section || s == data_section || s == rodata_section ||
      s == bss_section)
    return NULL;
  for (k = 0; k < 4; k++)
  {
    size_t n = strlen(base[k]);
    Section *t;
    if (strncmp(s->name, base[k], n) || s->name[n] != '.')
      continue;
    /* A data section the compiler made (section attribute) keeps its own
     * output section, as before; only the loader's per-object ones fold. */
    if (k != 0 && !strstr(s->name, ".."))
      return NULL;
    if (k == 0)
      t = text_section;
    else if (k == 1)
      t = data_section;
    else if (k == 3)
      t = bss_section;
    else
    {
      /* rodata_section is ".data.ro" on some targets; objects' .rodata
       * merged into a section of its own name there. */
      if (!*rodata)
      {
        if (!strcmp(rodata_section->name, ".rodata"))
          *rodata = rodata_section;
        else if (!(*rodata = section_ht_find(s1, ".rodata")))
          *rodata = new_section(s1, ".rodata", s->sh_type, s->sh_flags);
      }
      t = *rodata;
    }
    if (s == t || s->sh_type != t->sh_type)
      return NULL;
    if (k == 0 && !(s->sh_flags & SHF_EXECINSTR))
      return NULL;
    return t;
  }
  return NULL;
}

/* Fold every split input section (per-function .text.* under
 * -ffunction-sections, per-object .data/.rodata/.bss under --gc-sections;
 * unreferenced ones already emptied by gc_sections) back into its single
 * canonical section before GOT build and layout.
 *
 * Everything downstream of this point -- sbrel classification, layout, the
 * export table and the YAFF writer -- addresses code and data through the
 * section globals (tccyaff.c writes text_section->data as the entire code
 * payload), so coalescing here lets all of it stay ignorant of split
 * sections.  Sections merge in creation order, which is load and emission
 * order, so the resulting layout matches what a non-split build produces.
 *
 * Symbol fixup relies on two properties verified in this tree:
 *  - relocate_syms() adds a section base only for sh_num < SHN_LORESERVE,
 *    so retargeted symbols behave exactly like natively-.text ones;
 *  - DWARF references code via STT_SECTION symbols + addend
 *    (tcc_debug_funcstart records dwarf_register_text_section(cur_text_section)),
 *    so bumping the section symbol's st_value by the merge base keeps every
 *    debug address correct without touching the DWARF bytes. */
static void coalesce_split_sections(TCCState *s1)
{
  int i, have_split = 0;
  addr_t *base_of;
  Section **target_of;
  Section *rodata = NULL;
  ElfW(Sym) * sym;
  /* Snapshot the section count: put_elf_reloc below can create .rel.text
   * mid-loop, growing s1->nb_sections past the size of the arrays.  New
   * sections are reloc sections (or the canonical .rodata), never merge
   * candidates, so the snapshot loses nothing. */
  int nb_sections = s1->nb_sections;

  for (i = 1; i < nb_sections && !have_split; i++)
  {
    Section *s = s1->sections[i];
    if (s && (s->sh_flags & SHF_ALLOC) && s->name[0] == '.' && strchr(s->name + 1, '.') &&
        split_section_target(s1, s, &rodata))
      have_split = 1;
  }
  if (!have_split)
    return;

  base_of = tcc_mallocz(sizeof(addr_t) * nb_sections);
  target_of = tcc_mallocz(sizeof(Section *) * nb_sections);

  for (i = 1; i < nb_sections; i++)
  {
    Section *s = s1->sections[i];
    Section *t;
    addr_t align;
    if (!s || !(t = split_section_target(s1, s, &rodata)))
      continue;

    /* Thumb literal pools are PC-relative loads with PC aligned down to 4;
     * every function must therefore start 4-aligned, exactly as it would
     * have in a monolithic .text.  A whole object's .text keeps the
     * alignment it asked for, as the loader's concatenation gave it. */
    align = s->sh_addralign ? s->sh_addralign : 1;
    if (t == text_section)
      align = strncmp(s->name, ".text..", 7) ? 4 : (align > 4 ? align : 4);
    t->data_offset += -t->data_offset & (align - 1);
    if (align > t->sh_addralign)
      t->sh_addralign = align;
    base_of[i] = t->data_offset;
    target_of[i] = t;
    if (s->data_offset)
    {
      if (t->sh_type == SHT_NOBITS)
        t->data_offset += s->data_offset;
      else
      {
        void *p = section_ptr_add(t, s->data_offset);
        memcpy(p, s->data, s->data_offset);
      }
    }

    /* Move the section's relocations into the target's, rebased.  Addends
     * live in the copied bytes (REL format), so only r_offset changes;
     * symbol indices are global to symtab and stay valid. */
    if (s->reloc && s->reloc->data_offset)
    {
      Section *sr = s->reloc;
      int nb = sr->data_offset / sizeof(ElfW_Rel);
      ElfW_Rel *rel = (ElfW_Rel *)sr->data;
      int r;
      for (r = 0; r < nb; r++)
        put_elf_reloc(symtab_section, t, rel[r].r_offset + base_of[i], ELFW(R_TYPE)(rel[r].r_info),
                      ELFW(R_SYM)(rel[r].r_info));
      sr->data_offset = 0;
    }
  }

  /* Retarget every symbol defined in a merged section: function symbols
   * (the thumb LSB tag survives the addition), $t/$d mapping markers, and
   * the STT_SECTION symbols DWARF relocates against (st_value 0 -> base). */
  for_each_elem(symtab_section, 1, sym, ElfW(Sym))
  {
    if (sym->st_shndx < nb_sections && target_of[sym->st_shndx])
    {
      sym->st_value += base_of[sym->st_shndx];
      sym->st_shndx = target_of[sym->st_shndx]->sh_num;
    }
  }

  /* Retire the merged shells: no ALLOC flag and no size means
   * alloc_sec_names() skips them and they never reach the output. */
  for (i = 1; i < nb_sections; i++)
  {
    if (target_of[i])
    {
      Section *s = s1->sections[i];
      s->sh_flags &= ~SHF_ALLOC;
      s->data_offset = 0;
      s->sh_size = 0;
    }
  }

  tcc_free(base_of);
  tcc_free(target_of);
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

#ifdef TCC_TARGET_YAFF
  /* A YAFF module is relocated by yasld through its GOT and the relocations
   * recorded against it.  A -static link builds no GOT unless code needs one
   * and fills the entries with final link-time addresses and no relocations,
   * so the writer either dereferenced the missing GOT (SIGSEGV on any
   * GOT-free -static link, the default output of a YasOS-configured tcc) or
   * produced a module the loader cannot relocate. */
  if (s1->output_format == TCC_OUTPUT_FORMAT_YAFF && s1->static_link)
    return tcc_error_noabort("-static cannot produce a YAFF module; "
                             "use -Wl,-oformat=elf32-littlearm for a static image");
#endif

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
  {
    unsigned t = s1->do_bench ? tcc_getclock_us() : 0;
    tcc_add_runtime(s1);
    if (s1->do_bench)
      s1->bench_output_runtime_us += tcc_getclock_us() - t;
  }
  resolve_common_syms(s1);

#ifdef TCC_TARGET_YAFF
  /* Merge .init_array / .fini_array into .data early — before
     build_got_entries() — so that the __yaff_initfini symbol uses the
     R_RELATIVE (local) GOT path, and relocations pointing into the
     merged data are resolved naturally by relocate_sections(). */
  if (s1->output_format == TCC_OUTPUT_FORMAT_YAFF)
    tcc_yaff_prepare_init_fini(s1);
#endif

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

  /* Split input sections (.text.<fn>, and each object's own data under
   * --gc-sections) must be folded back into their canonical sections before
   * build_got/layout -- see the comment on coalesce_split_sections.  Runs
   * unconditionally: it is a no-op when no split input exists. */
  coalesce_split_sections(s1);

#ifdef TCC_TARGET_ARM
  /* Thumb branches between memory regions the script puts far apart (RAM
   * code calling flash on the RP2350) go through veneers: sized now, before
   * layout fixes the addresses. */
  if (s1->ld_script)
    arm_add_range_veneers(s1);
#endif

  /* Inputs of a (NOLOAD) output section occupy memory but have no file
   * contents: the RP2350 .ram_vector_table became a PT_LOAD at 0x20000000
   * with bytes in it, which a flasher would try to write. */
  if (s1->ld_script)
  {
    int i;
    for (i = 1; i < s1->nb_sections; i++)
    {
      Section *sec = s1->sections[i];
      int pat = -1, os;
      if (!sec || !(sec->sh_flags & SHF_ALLOC) || sec->sh_type != SHT_PROGBITS)
        continue;
      os = ld_find_output_section_idx(s1, sec->name, &pat);
      if (os >= 0 && os < s1->ld_script->nb_output_sections && s1->ld_script->output_sections[os].noload)
        sec->sh_type = SHT_NOBITS;
    }
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
    {
      unsigned t = s1->do_bench ? tcc_getclock_us() : 0;
      build_got_entries(s1, got_sym);
      if (s1->do_bench)
        s1->bench_output_got_us += tcc_getclock_us() - t;
    }
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
    {
      unsigned t = s1->do_bench ? tcc_getclock_us() : 0;
      build_got_entries(s1, 0);
      if (s1->do_bench)
        s1->bench_output_got_us += tcc_getclock_us() - t;
    }
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
  {
    unsigned t = s1->do_bench ? tcc_getclock_us() : 0;
    layout_sections(s1, sec_order, &dyninf);
    if (s1->do_bench)
      s1->bench_output_layout_us += tcc_getclock_us() - t;
  }

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
             Its inputs may sit in several LOAD segments (code, read-only
             and data get their own); each keeps its offset from the
             section's start in the load image. */
          addr_t vma = ld->output_section_vmas[j], vma_end = ld->output_section_vma_ends[j];
          if (vma)
          {
            for (k = 0; k < dyninf.phnum; k++)
            {
              ElfW(Phdr) *ph = &dyninf.phdr[k];
              if (ph->p_type == PT_LOAD && ph->p_vaddr >= vma && ph->p_vaddr < vma_end)
                ph->p_paddr = ld->output_section_loadaddrs[j] + (ph->p_vaddr - vma);
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
  {
    unsigned t = s1->do_bench ? tcc_getclock_us() : 0;
    relocate_syms(s1, s1->symtab, 0);
    if (s1->nb_errors != 0)
    {
      if (s1->do_bench)
        s1->bench_output_reloc_us += tcc_getclock_us() - t;
      goto the_end;
    }
    relocate_sections(s1);
    if (s1->do_bench)
      s1->bench_output_reloc_us += tcc_getclock_us() - t;
  }
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
  {
    unsigned t = s1->do_bench ? tcc_getclock_us() : 0;
    ret = tcc_write_elf_file(s1, filename, dyninf.phnum, dyninf.phdr);
    if (s1->do_bench)
      s1->bench_output_write_us += tcc_getclock_us() - t;
  }
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
#ifdef TCC_TARGET_ARM
  /* Objects carry the EABI attributes the linked image does: other linkers
   * read Tag_CPU_arch from every input, and lld took an untagged object for
   * ARMv4T and rejected the Thumb-2 branch relocations the assembler left. */
  if (!have_section(s1, ".ARM.attributes"))
    create_arm_attribute_section(s1);
#endif
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
  {
    unsigned t = s1->do_bench ? tcc_getclock_us() : 0;
    ret = tcc_write_elf_file(s1, filename, 0, NULL);
    if (s1->do_bench)
      s1->bench_output_write_us += tcc_getclock_us() - t;
  }
  return ret;
}

LIBTCCAPI int tcc_output_file(TCCState *s, const char *filename)
{
  unsigned output_start = 0;
  int ret;

  if (s->do_bench)
    output_start = tcc_getclock_us();

  if (s->test_coverage)
    tcc_tcov_add_file(s, filename);
  if (s->output_type == TCC_OUTPUT_OBJ)
    ret = elf_output_obj(s, filename);
#ifdef TCC_TARGET_PE
  else
    ret = pe_output_file(s, filename);
#elif defined TCC_TARGET_MACHO
  else
    ret = macho_output_file(s, filename);
#else
  else
    ret = elf_output_file(s, filename);
#endif
  if (s->do_bench)
  {
    unsigned elapsed = tcc_getclock_us() - output_start;
    s->bench_output_time += elapsed;
    s->bench_output_count++;
    tcc_bench_log(s, "output", filename, elapsed);
  }
  return ret;
}
