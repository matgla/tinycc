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

/* ELF: filling the GOT, binding dynamic symbols, section sizes and order,
 * section and program-header layout, and the dynamic section. */

#include "tccelf_priv.h"

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
void fill_local_got_entries(TCCState *s1)
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
      /* Store the ELF symbol type (e.g. STT_FUNC vs STT_NOTYPE) in the
         second word of the 8-byte GOT entry.  The YAFF writer reads this
         to distinguish function pointers (which need thunks) from plain
         code addresses such as labels used by goto *&&label.  */
      write32le(s1->got->data + offset + PTR_SIZE, ELFW(ST_TYPE)(sym->st_info));
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
void bind_exe_dynsyms(TCCState *s1, int is_PIE)
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
      sym_index = tcc_dynsym_find(s1, name);
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
void bind_libs_dynsyms(TCCState *s1)
{
  const char *name;
  int dynsym_index;
  ElfW(Sym) * sym, *esym;

  for_each_elem(symtab_section, 1, sym, ElfW(Sym))
  {
    name = (char *)symtab_section->link->data + sym->st_name;
    dynsym_index = tcc_dynsym_find(s1, name);
    if (sym->st_shndx != SHN_UNDEF)
    {
      if (ELFW(ST_BIND)(sym->st_info) != STB_LOCAL && !elf_sym_is_module_local(sym) &&
          (dynsym_index || s1->rdynamic))
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
   symbols to be resolved by other shared libraries or by the executable.
   Hidden and internal definitions stay inside the library. */
void export_global_syms(TCCState *s1)
{
  int dynindex, index;
  const char *name;
  ElfW(Sym) * sym;
  for_each_elem(symtab_section, 1, sym, ElfW(Sym))
  {
    if (ELFW(ST_BIND)(sym->st_info) != STB_LOCAL && !elf_sym_is_module_local(sym))
    {
      name = (char *)symtab_section->link->data + sym->st_name;
      dynindex = set_elf_sym(s1->dynsym, sym->st_value, sym->st_size, sym->st_info, 0, sym->st_shndx, name);
      index = sym - (ElfW(Sym) *)symtab_section->data;
      get_sym_attr(s1, index, 1)->dyn_index = dynindex;
    }
  }
}

/* decide if an unallocated section should be output. */
int set_sec_sizes(TCCState *s1)
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
#ifdef TCC_TARGET_YAFF
      else if (s1->output_format == TCC_OUTPUT_FORMAT_YAFF && (s1->sections[s->sh_info]->sh_flags & SHF_ALLOC))
      {
        /* Preserve data relocation sections for YAFF output.
           The YAFF writer needs R_ARM_ABS32 entries to generate
           data relocations for the dynamic loader. Setting sh_size
           ensures alloc_sec_names assigns a name, preventing
           reorder_sections from pruning the section. */
        s->sh_size = s->data_offset;
      }
#endif
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
    if (s->name[0] == '.' && (s->name[1] == 's' || s->name[1] == 'r') &&
        (!strcmp(s->name, ".stab") || !strcmp(s->name, ".stabstr") || !strncmp(s->name, ".rel.stab", 9) ||
         !strncmp(s->name, ".rela.stab", 10)))
    {
      s->sh_flags = 0;
      s->sh_size = 0;
    }
  }
  return textrel;
}

/* Find the linker script output section index and pattern index for a given
   section name. Returns output section index via return value (-1 if not
   found), and sets *pat_idx to the pattern index within that output section.
   Patterns are checked first (in order) since they define the ordering within
   the output section. If no pattern matches but the section name exactly
   matches an output section name, pat_idx is set to a value after all patterns
   to indicate it should come last within that output section. */
int ld_find_output_section_idx(TCCState *s1, const char *name, int *pat_idx)
{
  return ld_find_output_section(s1->ld_script, name, pat_idx);
}

/* Check if a section name matches a specific output section index */
int ld_section_matches_output(TCCState *s1, const char *name, int os_idx)
{
  int pat_idx = -1;
  int found = ld_find_output_section_idx(s1, name, &pat_idx);
  return found == os_idx;
}

/* Decide the layout of sections loaded in memory. This must be done before
   program headers are filled since they contain info about the layout.
   We do the following ordering: interp, symbol tables, relocations, progbits,
   nobits */
/* sort_sections groups linker-script sections by output section (class
 * 0x100 + index); order each group by the pattern that placed a section,
 * then -- for SORT() patterns -- by name, then by input order.  Pattern
 * indices are looked up once per section (a group can hold thousands of
 * -fdata-sections inputs). */
static void ld_order_within_output_sections(TCCState *s1, int *sec_order, int *sec_cls, int nb_sections)
{
  int *pat = tcc_malloc(nb_sections * sizeof(int));
  unsigned char *by_name = tcc_mallocz(nb_sections);
  int i = 1;
  while (i < nb_sections)
  {
    int k = sec_cls[i], j = i, a, b;
    while (j < nb_sections && sec_cls[j] == k)
      j++;
    if (k >= 0x100 && k < 0x120 && j - i > 1)
    {
      for (a = i; a < j; a++)
      {
        int sec = sec_order[a], p = -1;
        int os = ld_find_output_section_idx(s1, s1->sections[sec]->name, &p);
        pat[sec] = p;
        by_name[sec] = os >= 0 && p >= 0 && p < s1->ld_script->output_sections[os].nb_patterns &&
                       s1->ld_script->output_sections[os].patterns[p].sort;
      }
      for (a = i + 1; a < j; a++) /* stable insertion sort */
      {
        int v = sec_order[a];
        for (b = a; b > i; b--)
        {
          int u = sec_order[b - 1], before;
          if (pat[v] != pat[u])
            before = pat[v] < pat[u];
          else if (by_name[v] && strcmp(s1->sections[v]->name, s1->sections[u]->name) != 0)
            before = strcmp(s1->sections[v]->name, s1->sections[u]->name) < 0;
          else
            before = v < u;
          if (!before)
            break;
          sec_order[b] = u;
        }
        sec_order[b] = v;
      }
    }
    i = j;
  }
  tcc_free(pat);
  tcc_free(by_name);
}

static int sort_sections(TCCState *s1, int *sec_order, struct dyn_inf *d)
{
  Section *s;
  int i, j, k, f, f0, n, ld_idx;
  int nb_sections = s1->nb_sections;
  int *sec_cls = sec_order + nb_sections;
  int *cls = tcc_malloc(nb_sections * sizeof *cls), *count, max_k = 0;

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
        /* Limit ld_idx to fit in 5 bits (0-31 output sections).  The
         * pattern order inside an output section is applied after this
         * pass (ld_order_within_output_sections): folding it into k took
         * only 4 bits, so the 16th pattern of .text wrapped round to the
         * front, and k could land on 0x240, the RELRO class. */
        if (ld_idx > 31)
          ld_idx = 31;
        k = 0x100 + ld_idx;
      }
      else
      {
        /* Section not in linker script: place after all linker script sections
           but still within ALLOC range. Use 0x6xx + original sub-class. */
        k = 0x600 + ((k & 0x7f) >> 4);
      }
    }

    cls[i] = k;
    if (k > max_k)
      max_k = k;
  }
  /* Counting sort by class, sections of one class in index order: what
   * inserting each section after every earlier one of a class <= its own
   * gave, in quadratic time over the hundreds of sections of a
   * --gc-sections link.  Classes are small (< 0xa00). */
  count = tcc_mallocz((max_k + 2) * sizeof *count);
  for (i = 1; i < nb_sections; i++)
    count[cls[i] + 1]++;
  for (k = 0, n = 1; k <= max_k; k++)
  {
    f = count[k + 1];
    count[k + 1] = n; /* first slot of class k */
    n += f;
  }
  for (i = 1; i < nb_sections; i++)
  {
    n = count[cls[i] + 1]++;
    sec_cls[n] = cls[i];
    sec_order[n] = i;
  }
  tcc_free(count);
  tcc_free(cls);
  sec_order[0] = 0;
  if (s1->ld_script && s1->ld_script->nb_output_sections > 0)
    ld_order_within_output_sections(s1, sec_order, sec_cls, nb_sections);
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
    LOG_RELOC("ph %d sec %02d : %3X %3X  %x  %04X  %s", (f > 0) * n, i, f, k, s->sh_type, (int)s->sh_size, s->name);
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
int layout_sections(TCCState *s1, int *sec_order, struct dyn_inf *d)
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

#ifdef TCC_TARGET_YAFF
  /* YAFF format uses 0-based offsets for all sections.
     Force text address to 0 unless the user explicitly set one. */
  if (s1->output_format == TCC_OUTPUT_FORMAT_YAFF && !s1->has_text_addr)
    addr = 0;
#endif

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

  /* `base` is what the first PT_LOAD claims as its p_vaddr, so it has to be
     where the first section actually lands -- which, when a linker script
     supplies MEMORY regions, is the first region's origin and not the text
     address above.  A YASOS-targeted compiler defaults to text_addr 0 with
     has_text_addr set (tcc_new), so taking `base` any earlier described a
     script-placed image as starting at 0: p_vaddr 0 against sections at the
     region origin, and p_memsz stretched from 0 to the end of the segment.
     QEMU loads such a segment on top of the vector table it just wrote (the
     0x00000000 and 0x10000000 SSRAM aliases on mps2-an505 are the same
     memory), so the CPU reset-fetched SP=0/PC=0 and locked up before the
     first instruction. */
  base = addr;

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

  /* Fill other headers.  They take the slots after the PT_LOADs -- counted
   * by n, not found through `ph`, which is still NULL when nothing
   * allocatable has any size (a -static link of an object without code). */
  ph = d->phdr + phfill + n;
  if (d->note)
    fill_phdr(ph++, PT_NOTE, d->note);
  if (d->dynamic)
  {
    ElfW(Phdr) *dph = fill_phdr(ph++, PT_DYNAMIC, d->dynamic);
    dph->p_flags |= PF_W;
  }
  if (eh_frame_hdr_section)
    fill_phdr(ph++, PT_GNU_EH_FRAME, eh_frame_hdr_section);
  if (d->roinf)
  {
    ElfW(Phdr) *rph = fill_phdr(ph++, PT_GNU_RELRO, d->roinf);
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
void put_dt(Section *dynamic, int dt, addr_t val)
{
  ElfW(Dyn) * dyn;
  dyn = section_ptr_add(dynamic, sizeof(ElfW(Dyn)));
  dyn->d_tag = dt;
  dyn->d_un.d_val = val;
}

/* Fill the dynamic section with tags describing the address and size of
   sections */
void fill_dynamic(TCCState *s1, struct dyn_inf *dyninf)
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
void update_reloc_sections(TCCState *s1, struct dyn_inf *dyninf)
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
