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

/* ELF: relocating symbols and sections, dynamic relocations, the GOT, and
 * adding the runtime (crt objects, libtcc1, floating-point library). */

#include "tccelf_priv.h"

static void add_reloc_patch(Section *s, uint32_t offset, uint32_t value);

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
          LOG_RELOC("relocate_sym: %s -> 0x%lx", name, sym->st_value);
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
void free_reloc_patches(Section *s)
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
      if (type == R_DATA_32DW)
      {
        uint32_t value;
        if (sym->st_shndx >= s1->dwlo && sym->st_shndx < s1->dwhi)
          /* dwarf-to-dwarf section relocation (e.g., .debug_info -> .debug_str) */
          value = tgt - s1->sections[sym->st_shndx]->sh_addr;
        else
          /* code/data reference from debug section (e.g., DW_AT_low_pc -> .text) */
          value = tgt;
        add_reloc_patch(s, (uint32_t)rel->r_offset, value);
      }
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
#ifdef TCC_TARGET_ARM
    /* Skip relocations for suppressed ARM exception index sections.
       set_sec_sizes() clears SHF_ALLOC on .ARM.exidx (stack unwinding
       not used on bare-metal), but relocation sections survive.  If we
       still process them, R_ARM_PREL31 entries that reference orphan
       .ARM.extab (placed far away in RAM) overflow the 31-bit range. */
    if (s->sh_type == SHT_ARM_EXIDX && !(s->sh_flags & SHF_ALLOC))
      continue;
#endif
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
int prepare_dynamic_rel(TCCState *s1, Section *sr)
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
#if defined(TCC_TARGET_YASOS)
  /* + a reserved slot (index YAFF_RODATA_ANCHOR_GOT_INDEX = 3) holding the
   * runtime base of the shared .rodata segment. Reserved up front so its GOT
   * offset is a compile-time constant the codegen addresses as [R9,#24],
   * independent of the final GOT layout; the loader fills it (the YAFF writer
   * emits its relocation when -share-rodata is active). */
  section_ptr_add(s1->got, 4 * PTR_SIZE * 2);
#else
  section_ptr_add(s1->got, 3 * PTR_SIZE * 2);
#endif
  return set_elf_sym(symtab_section, 0, 0, ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), STV_HIDDEN, s1->got->sh_num,
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
    /* A hidden definition takes the STB_LOCAL path too: through a dynamic
       symbol it would be exported (and preemptible). */
    if (ELFW(ST_BIND)(sym->st_info) == STB_LOCAL ||
        (elf_sym_is_module_local(sym) && sym->st_shndx < SHN_LORESERVE))
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
    /* Relocations from non-allocated sections (DWARF) never execute at
     * runtime and must not fabricate GOT/PLT entries.  Besides being pure
     * GOT bloat, under --gc-sections they reference the SHN_ABS 0xFFFFFFFF
     * tombstones of collected code, and AUTO_GOTPLT_ENTRY would turn each
     * into an out-of-range GOT slot that the yasld loader refuses to map
     * (boot-time "Can't find section for GOT[n]: OffsetOutOfRange"). */
    if (s->sh_info < s1->nb_sections && s1->sections[s->sh_info] &&
        !(s1->sections[s->sh_info]->sh_flags & SHF_ALLOC))
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

/* tcc's own boundary symbols are defaults: a linker script that assigns the
 * same name owns it (GNU ld lets script assignments win).  Defining both was
 * "'__fini_array_start' defined twice" against PROVIDE_HIDDEN(... = .).
 * They are hidden: each module's bounds are its own, so a shared library must
 * neither export them nor bind its references to another module's (GNU ld's
 * scripts PROVIDE_HIDDEN them).  A definition from an input keeps its own
 * visibility -- the merge in set_elf_sym would otherwise hide it. */
static void set_default_linker_sym(TCCState *s1, const char *name, Section *sec, addr_t offs)
{
  int shn, sym_index;
  if (ld_script_defines_symbol(s1, name))
    return;
  sym_index = find_elf_sym(symtab_section, name);
  if (sym_index && ((ElfW(Sym) *)symtab_section->data)[sym_index].st_shndx != SHN_UNDEF)
  {
    set_global_sym(s1, name, sec, offs);
    return;
  }
  shn = sec ? sec->sh_num : offs ? SHN_ABS : SHN_UNDEF;
  if (sec && offs == -1)
    offs = sec->data_offset;
  set_elf_sym(symtab_section, offs, 0, ELFW(ST_INFO)(STB_GLOBAL, STT_NOTYPE), STV_HIDDEN, shn, name);
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
  set_default_linker_sym(s1, buf, s, 0);
  snprintf(buf, sizeof(buf), "__%s_end", section_name + 1);
  set_default_linker_sym(s1, buf, s, end_offset);
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
void tcc_tcov_add_file(TCCState *s1, const char *filename)
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
/* Determine the FP library base name (without lib prefix / extension).
 * Same name is used for both static (.a) and shared (.so) variants.
 * Returns the name for use with tcc_add_library() or manual path construction.
 *
 * Returns NULL if hard float ABI (no FP library needed).
 */
ST_FUNC const char *tccelf_get_fp_lib_name(TCCState *s1)
{
  if (s1->float_abi == ARM_HARD_FLOAT)
    return NULL;

  /* -mfloat-abi=soft means "no FP instructions anywhere in this image", and the
   * runtime the image links is part of the image.  Pairing it with a hardware
   * -mfpu used to be the only way to say "hardware, behind a call" and so
   * quietly linked librp2350fp -- DCP instructions, on a build that had just
   * declared it emits none, with a YAFF header that asked the loader for
   * nothing.  That mode now has its own flag (-mfp-inline=none), which leaves
   * this one free to mean what it says. */
  if (s1->float_abi == ARM_SOFT_FLOAT)
    return "softfp";

  if (s1->fpu_type)
  {
    switch (s1->fpu_type)
    {
    case ARM_FPU_VFPV4:
    case ARM_FPU_FPV4_SP_D16:
    case ARM_FPU_FPV5_SP_D16:
      return "vfpv4sp";
    case ARM_FPU_RP2350:
      /* RP2350 double coprocessor: doubles go through the DCP sequences in
       * librp2350fp rather than the generic soft-float implementations. */
      return "rp2350fp";
    case ARM_FPU_FPV5_D16:
    case ARM_FPU_NEON:
    case ARM_FPU_NEON_VFPV4:
    case ARM_FPU_NEON_FP_ARMV8:
      return "vfpv5dp";
    default:
      break;
    }
  }

  return "softfp";
}

/* Add ARM floating-point library based on compiler flags.
 * Selects the correct FP library variant based on -mfpu and -mfloat-abi.
 *
 * Library names follow the short convention:
 *   libsoftfp.{a,so}   - Pure software floating point
 *   libvfpv4sp.{a,so}  - VFPv4 single-precision HW, double SW
 *   libvfpv5dp.{a,so}  - VFPv5 full double-precision HW
 *   librp2350fp.{a,so} - RP2350 double coprocessor
 *
 * On YasOS native (dynamic linking): uses tcc_add_library() which searches
 *   library_paths for libsoftfp.so in /usr/lib/, etc.
 *
 * On cross-compiler / static: uses tcc_add_dll() with "fp/libsoftfp.a" path,
 *   resolved relative to tcc_lib_path (CONFIG_TCCDIR), e.g. lib/tcc/fp/.
 *
 * Hard float ABI needs no FP library (uses raw FP instructions).
 */
/* Is the __aeabi_ runtime bound as a shared object rather than copied in from
 * an archive?  -mfp-lib decides; AUTO keeps the historical split, where only
 * the on-device compiler binds the shared object. */
ST_FUNC int tccelf_arm_fp_lib_is_shared(TCCState *s1)
{
  if (s1->static_link)
    return 0;
  if (s1->fp_lib == ARM_FP_LIB_SHARED)
    return 1;
  if (s1->fp_lib == ARM_FP_LIB_STATIC)
    return 0;
#if TARGETOS_YasOS && defined(TCC_IS_NATIVE)
  return 1;
#else
  return 0;
#endif
}

ST_FUNC void tccelf_add_arm_fp_lib(TCCState *s1)
{
  const char *fp_lib = tccelf_get_fp_lib_name(s1);
  if (!fp_lib)
  {
    if (s1->verbose)
      printf("Hard float ABI: no FP library needed\n");
    return;
  }

  if (tccelf_arm_fp_lib_is_shared(s1))
  {
    /* Search library_paths for libXXX.so then libXXX.a (e.g.
     * /usr/lib/libsoftfp.so), so the __aeabi_ entry points are resolved by the
     * OS's dynamic loader at exec instead of copied into this module. */
    if (s1->verbose)
      printf("Adding ARM FP shared library: lib%s\n", fp_lib);
    tcc_add_library(s1, fp_lib);
    return;
  }

  /* Cross-compiler or static link: find via tcc lib path (fp/ subdirectory).
   * tcc_add_dll() searches library_paths which includes {B} = tcc_lib_path,
   * so "fp/libsoftfp.a" resolves to e.g. lib/tcc/fp/libsoftfp.a */
  {
    char fp_path[64];
    snprintf(fp_path, sizeof(fp_path), "fp/lib%s.a", fp_lib);
    if (s1->verbose)
      printf("Adding ARM FP library: %s\n", fp_path);
    tcc_add_dll(s1, fp_path, AFF_PRINT_ERROR);
  }
}
#endif

#ifndef TCC_TARGET_PE
/* add tcc runtime libraries
 *
 * Two levels of library suppression (matching GCC semantics):
 *
 *   -nostdlib       : skip standard libraries (libc, libpthread, libdl, ...)
 *                     but still link compiler runtime (libtcc1, softfp, crt).
 *
 *   -nodefaultlibs  : skip everything including compiler runtime (libtcc1,
 *                     softfp, crt).  Used when building the runtime libs
 *                     themselves to avoid circular dependencies.
 */
ST_FUNC void tcc_add_runtime(TCCState *s1)
{
  s1->filetype = 0;

  /* -nodefaultlibs: skip all implicit libraries and runtime support */
  if (s1->nodefaultlibs)
    return;

#ifdef CONFIG_TCC_BCHECK
  tcc_add_bcheck(s1);
#endif
  tcc_add_pragma_libs(s1);

  /* A shared FP runtime has to be bound BEFORE libc.
   *
   * Every shared object in this rootfs that was ever linked against the FP
   * archive absorbed a copy of the __aeabi_ set and re-exports it -- libc.so
   * alone exports fifteen doubles entry points.  The loader resolves an import
   * by walking the module's dependencies in the order the image lists them
   * (Module.find_symbol over `children`, appended in import order), so an
   * image that named libc first would silently run libc's copy of every
   * operation and the -mfpu it was built with would decide nothing.  Binding
   * the runtime the link actually selected first is what makes the choice
   * stick.  The archive path keeps the old position: an archive is searched a
   * la carte, so it has to come after the objects that reference it. */
#if defined TCC_TARGET_ARM
  if (tccelf_arm_fp_lib_is_shared(s1))
    tccelf_add_arm_fp_lib(s1);
#endif

  /* Standard libraries (skipped by -nostdlib) */
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
    tcc_add_library(s1, "m");
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
  }

  /* Compiler runtime (always linked unless -nodefaultlibs).
   * Order matters: softfp objects reference libtcc1 symbols (__aeabi_memset,
   * __aeabi_lcmp, etc.), so the FP library must come first so that its
   * undefined references exist when libtcc1.a is processed à la carte. */
#if defined TCC_TARGET_ARM
  if (!tccelf_arm_fp_lib_is_shared(s1))
    tccelf_add_arm_fp_lib(s1);
#endif
  if (TCC_LIBTCC1[0])
    tcc_add_support(s1, TCC_LIBTCC1);
#ifndef TCC_TARGET_MACHO
  if (s1->output_type != TCC_OUTPUT_MEMORY && !s1->nostdlib)
    tccelf_add_crtend(s1);
#endif
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

  /* What a __rodata_relative pointer is an offset from (rodata_rel.c);
   * defined only for a module that names it. */
  {
    int i_base = find_elf_sym(symtab_section, "__tcc_rodata_base");
    if (i_base && ((ElfW(Sym) *)symtab_section->data)[i_base].st_shndx == SHN_UNDEF)
      set_default_linker_sym(s1, "__tcc_rodata_base", rodata_section, 0);
  }
  set_default_linker_sym(s1, "_etext", text_section, -1);
  set_default_linker_sym(s1, "_edata", data_section, -1);
  set_default_linker_sym(s1, "_end", bss_section, -1);
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
      set_default_linker_sym(s1, buf, s, 0);
      snprintf(buf, sizeof(buf), "__stop_%s", p0);
      set_default_linker_sym(s1, buf, s, -1);
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
