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

/* --gc-sections implementation: remove unused sections */
void gc_sections(TCCState *s1)
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
    if (s1->rdynamic && ELFW(ST_BIND)(sym->st_info) != STB_LOCAL)
    {
      sec_used[sym->st_shndx] = 1;
      continue;
    }
    /* Exported symbols are runtime roots regardless of -rdynamic: under
     * -fvisibility=hidden the dynamic-symbol set is exactly the defined
     * GLOBAL/WEAK symbols with default visibility, and other yasld modules
     * resolve against them at runtime with no relocation in this image to
     * witness the use.  Collecting one would leave a .dynsym entry pointing
     * at reclaimed bytes -- a silent wrong-address at runtime, not a link
     * error.  Hidden globals and statics stay collectable. */
    if (ELFW(ST_BIND)(sym->st_info) != STB_LOCAL && ELFW(ST_VISIBILITY)(sym->st_other) == STV_DEFAULT)
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
      if (!sr || sr->sh_type != SHT_RELX)
        continue;
      /* Get the section this relocation applies to */
      s = s1->sections[sr->sh_info];
      if (!s || !sec_used[sr->sh_info])
        continue;
      /* References from non-allocated sections must not confer liveness:
       * DWARF describes every function (each .text.<name> is referenced by
       * .rel.debug_info via its section symbol), so following these would
       * pin the whole program and -g would silently disable GC.  Dropped
       * targets get the SHN_ABS tombstone in the sweep instead, which is
       * how debug info for collected code is conventionally marked. */
      if (!(s->sh_flags & SHF_ALLOC))
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
         * coalesce_function_sections, which must not fold a collected
         * .text.<name> back into .text. */
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
  }

  tcc_free(sec_used);
}

/* Fold every per-function .text.* input section (created under
 * -ffunction-sections; unreferenced ones already emptied by gc_sections)
 * back into the single text_section before GOT build and layout.
 *
 * Everything downstream of this point -- sbrel classification, layout, the
 * export table and the YAFF writer -- addresses code through the
 * text_section global (tccyaff.c writes text_section->data as the entire
 * code payload), so coalescing here lets all of it stay ignorant of split
 * text.  Sections merge in creation order, which is emission order, so the
 * resulting layout matches what a non-split build produces.
 *
 * Symbol fixup relies on two properties verified in this tree:
 *  - relocate_syms() adds a section base only for sh_num < SHN_LORESERVE,
 *    so retargeted symbols behave exactly like natively-.text ones;
 *  - DWARF references code via STT_SECTION symbols + addend
 *    (tcc_debug_funcstart records dwarf_register_text_section(cur_text_section)),
 *    so bumping the section symbol's st_value by the merge base keeps every
 *    debug address correct without touching the DWARF bytes. */
static void coalesce_function_sections(TCCState *s1)
{
  int i, have_split = 0;
  addr_t *base_of;
  unsigned char *merged;
  ElfW(Sym) * sym;
  /* Snapshot the section count: put_elf_reloc below can create .rel.text
   * mid-loop, growing s1->nb_sections past the size of the arrays.  New
   * sections are reloc sections, never merge candidates, so the snapshot
   * loses nothing. */
  int nb_sections = s1->nb_sections;

  for (i = 1; i < nb_sections; i++)
  {
    Section *s = s1->sections[i];
    if (s && s != text_section && s->sh_type == SHT_PROGBITS && (s->sh_flags & SHF_EXECINSTR) &&
        (s->sh_flags & SHF_ALLOC) && !strncmp(s->name, ".text.", 6))
    {
      have_split = 1;
      break;
    }
  }
  if (!have_split)
    return;

  base_of = tcc_mallocz(sizeof(addr_t) * nb_sections);
  merged = tcc_mallocz(nb_sections);

  for (i = 1; i < nb_sections; i++)
  {
    Section *s = s1->sections[i];
    if (!s || s == text_section || s->sh_type != SHT_PROGBITS)
      continue;
    if ((s->sh_flags & (SHF_EXECINSTR | SHF_ALLOC)) != (SHF_EXECINSTR | SHF_ALLOC))
      continue;
    if (strncmp(s->name, ".text.", 6))
      continue;

    /* Thumb literal pools are PC-relative loads with PC aligned down to 4;
     * every function must therefore start 4-aligned, exactly as it would
     * have in a monolithic .text. */
    text_section->data_offset += -text_section->data_offset & (addr_t)3;
    base_of[i] = text_section->data_offset;
    merged[i] = 1;
    if (s->data_offset)
    {
      void *p = section_ptr_add(text_section, s->data_offset);
      memcpy(p, s->data, s->data_offset);
    }

    /* Move the section's relocations into .rel.text, rebased.  Addends live
     * in the copied code bytes (REL format), so only r_offset changes;
     * symbol indices are global to symtab and stay valid. */
    if (s->reloc && s->reloc->data_offset)
    {
      Section *sr = s->reloc;
      int nb = sr->data_offset / sizeof(ElfW_Rel);
      ElfW_Rel *rel = (ElfW_Rel *)sr->data;
      int r;
      for (r = 0; r < nb; r++)
        put_elf_reloc(symtab_section, text_section, rel[r].r_offset + base_of[i], ELFW(R_TYPE)(rel[r].r_info),
                      ELFW(R_SYM)(rel[r].r_info));
      sr->data_offset = 0;
    }
  }

  /* Retarget every symbol defined in a merged section: function symbols
   * (the thumb LSB tag survives the addition), $t/$d mapping markers, and
   * the STT_SECTION symbols DWARF relocates against (st_value 0 -> base). */
  for_each_elem(symtab_section, 1, sym, ElfW(Sym))
  {
    if (sym->st_shndx < nb_sections && merged[sym->st_shndx])
    {
      sym->st_value += base_of[sym->st_shndx];
      sym->st_shndx = text_section->sh_num;
    }
  }

  /* Retire the merged shells: no ALLOC flag and no size means
   * alloc_sec_names() skips them and they never reach the output. */
  for (i = 1; i < nb_sections; i++)
  {
    if (merged[i])
    {
      Section *s = s1->sections[i];
      s->sh_flags &= ~SHF_ALLOC;
      s->data_offset = 0;
      s->sh_size = 0;
    }
  }

  tcc_free(base_of);
  tcc_free(merged);
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

  /* Per-function .text.* sections (from -ffunction-sections objects) must be
   * folded back into text_section before build_got/layout -- see the comment
   * on coalesce_function_sections.  Runs unconditionally: it is a no-op when
   * no split-text input exists. */
  coalesce_function_sections(s1);

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
