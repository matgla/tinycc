/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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

#include <stdbool.h>

#include "tccyaff.h"

/* Debug output for YAFF local relocations - disabled by default
 * Enable with: -DYAFF_DEBUG_ENABLED or #define YAFF_DEBUG_ENABLED */
#ifdef YAFF_DEBUG_ENABLED
#define YAFF_DEBUG(...) fprintf(stderr, __VA_ARGS__)
#else
#define YAFF_DEBUG(...) ((void)0)
#endif

#define TCC_YAFF_MAX_SYMBOL_ENTRY_SIZE 255

#define SHF_DYNSYM 0x40000000

// This implementation is based on elf hashing function
uint32_t tcc_yaff_hash(const char *name)
{
  uint32_t h = 0, g;
  while (*name)
  {
    h = (h << 4) + *name++;
    if ((g = h & 0xf0000000))
    {
      h ^= g >> 24;
      h &= ~g;
    }
  }
  return h;
}

void tcc_allocate_hash_table(YaffHashTable *ht, uint32_t number_of_buckets, uint32_t count)
{
  ht->nbucket = number_of_buckets;
  ht->nchain = count;
  ht->bucket = tcc_malloc(ht->nbucket * sizeof(uint32_t));
  ht->chain = tcc_malloc(ht->nchain * sizeof(uint32_t));
  memset(ht->bucket, 0, ht->nbucket * sizeof(uint32_t));
  memset(ht->chain, 0, ht->nchain * sizeof(uint32_t));
}

void tcc_add_hash_entry(YaffHashTable *ht, const char *name, uint32_t i)
{
  uint32_t h = tcc_yaff_hash(name);
  uint32_t b = h % ht->nbucket;
  if (ht->bucket[b] == 0)
  {
    ht->bucket[b] = i;
  }
  else
  {
    uint32_t idx = ht->bucket[b];
    while (ht->chain[idx] != 0)
      idx = ht->chain[idx];
    ht->chain[idx] = i;
  }
}

void tcc_free_hash_table(YaffHashTable *ht)
{
  tcc_free(ht->bucket);
  tcc_free(ht->chain);
}

void tcc_write_hash_table(YaffHashTable *ht, FILE *f)
{
  fwrite(&ht->nbucket, 1, sizeof(uint32_t), f);
  fwrite(&ht->nchain, 1, sizeof(uint32_t), f);
  fwrite(ht->bucket, sizeof(uint32_t), ht->nbucket, f);
  fwrite(ht->chain, sizeof(uint32_t), ht->nchain, f);
}

uint32_t tcc_yaff_align(YaffHeader *header, uint32_t size)
{
  return (size + header->alignment - 1) & ~(header->alignment - 1);
}

const char *tcc_parse_object_name(YaffHeader *header)
{
  return (const char *)(header) + sizeof(YaffHeader);
}

uint32_t tcc_get_offset_to_imported_libraries(YaffHeader *header)
{
  const uint32_t name_length = strlen(tcc_parse_object_name(header)) + 1;
  return sizeof(YaffHeader) + tcc_yaff_align(header, name_length);
}

ST_FUNC int tcc_load_yaff(TCCState *s1, int fd, const char *filename, int level)
{
  int ret = 0;
  const char *soname = tcc_basename(filename);
  YaffHeader header;
  char buffer[TCC_YAFF_MAX_SYMBOL_ENTRY_SIZE];
  uint32_t offset = 0;
  full_read(fd, &header, sizeof(YaffHeader));
  if (memcmp(header.magic, "YAFF", 4) != 0)
  {
    return tcc_error_noabort("not a valid YAFF file");
  }

  offset = header.exported_symbols_offset;

  for (int i = 0; i < header.exported_symbols_amount; ++i)
  {
    YaffSymbolEntry *entry = (YaffSymbolEntry *)buffer;
    size_t len = 0;
    lseek(fd, offset, SEEK_SET);
    full_read(fd, buffer, sizeof(buffer));
    len = strnlen(entry->name, sizeof(buffer) - sizeof(YaffSymbolEntry));
    if (len == sizeof(buffer))
    {
      return tcc_error_noabort("symbol entry too long");
    }
    set_elf_sym(s1->dynsymtab_section, entry->offset, 1, STB_GLOBAL << 4, STV_DEFAULT, 1, entry->name);
    offset += sizeof(uint32_t) + len + 1;
    offset = tcc_yaff_align(&header, offset);
  }

  /* if the dll is already loaded, do not load it */
  tcc_add_dllref(s1, soname, level);

  return ret;
}

/* Write local relocations for GOT entries that reference local symbols.
 *
 * When a local (STB_LOCAL) symbol's address is taken, put_got_entry()
 * creates a GOT slot and an R_RELATIVE relocation in .rel.got.
 * fill_local_got_entries() resolves these by writing the symbol's final
 * address into the GOT slot (first 4 bytes) and zeroing the sym_index.
 *
 * The dynamic loader needs to know about these GOT entries so it can:
 *  - Relocate the address at load time (text/data can be loaded anywhere)
 *  - Wrap function pointers in thunks that set up R9 (GOT base) when the
 *    function is called back from another module
 *
 * We detect "code" vs "data" by checking whether the resolved address
 * falls within the text section.  Code-section local relocations trigger
 * thunk generation in the loader.
 */
static int tcc_yaff_write_local_relocations(TCCState *s1, FILE *f)
{
  ElfW_Rel *rel;
  int count = 0;

  if (!s1->got || !s1->got->reloc)
  {
    YAFF_DEBUG("[YAFF] no GOT or no GOT relocs (got=%p, reloc=%p)\n", s1->got, s1->got ? s1->got->reloc : NULL);
    return 0;
  }

  YAFF_DEBUG("[YAFF] scanning .rel.got: got->sh_addr=0x%x, text=0x%x..0x%x, rodata=0x%x..0x%x\n",
             (unsigned)s1->got->sh_addr, (unsigned)text_section->sh_addr,
             (unsigned)(text_section->sh_addr + text_section->sh_size), (unsigned)rodata_section->sh_addr,
             (unsigned)(rodata_section->sh_addr + rodata_section->sh_size));

  for_each_elem(s1->got->reloc, 0, rel, ElfW_Rel)
  {
    int rtype = ELFW(R_TYPE)(rel->r_info);
    YAFF_DEBUG("[YAFF]   rel: r_offset=0x%x, type=%d, sym=%d\n", (unsigned)rel->r_offset, rtype,
               ELFW(R_SYM)(rel->r_info));

    if (rtype != R_RELATIVE)
      continue;

    /* GOT entry offset within .got section */
    uint32_t got_offset = rel->r_offset - s1->got->sh_addr;
    /* Resolved address written by fill_local_got_entries() */
    uint32_t sym_value = read32le(s1->got->data + got_offset);

    YAFF_DEBUG("[YAFF]   R_RELATIVE: got_offset=0x%x, sym_value=0x%x\n", got_offset, sym_value);

    /* Determine which section this address belongs to */
    int section;
    uint32_t target_offset;
    if (sym_value >= text_section->sh_addr && sym_value < text_section->sh_addr + text_section->sh_size)
    {
      section = YAFF_SECTION_CODE;
      target_offset = sym_value - text_section->sh_addr;
    }
    else if (sym_value >= rodata_section->sh_addr && sym_value < rodata_section->sh_addr + rodata_section->sh_size)
    {
      section = YAFF_SECTION_DATA;
      target_offset = sym_value - rodata_section->sh_addr;
    }
    else if (sym_value >= data_section->sh_addr && sym_value < data_section->sh_addr + data_section->sh_size)
    {
      section = YAFF_SECTION_DATA;
      target_offset = sym_value - data_section->sh_addr + rodata_section->sh_size;
    }
    else
    {
      YAFF_DEBUG("[YAFF]   WARNING: sym_value 0x%x doesn't fall in any known section!\n", sym_value);
      section = YAFF_SECTION_DATA;
      target_offset = sym_value;
    }

    YAFF_DEBUG("[YAFF]   -> section=%s, index=%u, target_offset=0x%x\n", section == YAFF_SECTION_CODE ? "CODE" : "DATA",
               got_offset / 8, target_offset);

    YaffLocalRelocationEntry entry = {
        .section = section,
        .index = got_offset / 8,
        .target_offset = target_offset,
    };
    fwrite(&entry, 1, sizeof(entry), f);
    ++count;
  }

  YAFF_DEBUG("[YAFF] total local relocations: %d\n", count);
  return count;
}

static int tcc_yaff_write_data_relocations(TCCState *s1, FILE *f)
{
  int i;
  Section *s;
  int number_of_data_relocations = 0;
  for (i = 0; i < s1->nb_sections; ++i)
  {
    if (i)
    {
      s = s1->sections[i];
      if (s->sh_type == SHT_REL)
      {
        /* Skip .rel.got — its R_RELATIVE entries are already emitted as
           local relocations by tcc_yaff_write_local_relocations().
           Processing them here as well would produce duplicate data
           relocations that overwrite the thunk addresses the dynamic
           loader places into GOT slots for function-pointer callbacks. */
        if (s1->got && s == s1->got->reloc)
          continue;
        ElfW_Rel *rel;
        for_each_elem(s, 0, rel, ElfW_Rel)
        {
          int type = ELFW(R_TYPE)(rel->r_info);
          switch (type)
          {
          // same as R_ARM_GOT_BREL
          case R_ARM_ABS32:
          case R_ARM_RELATIVE:
          {
            // first data section is rodata
            uint32_t from_address = rel->r_offset;
            uint32_t original_offset = 0;
            bool towards_code = false;
            YaffDataRelocationEntry entry;
            /* If the relocation section does not have SHF_ALLOC,
               r_offset is section-relative. Convert to absolute
               virtual address by adding the target section base. */
            if (!(s->sh_flags & SHF_ALLOC))
            {
              Section *target_sec = s1->sections[s->sh_info];
              from_address += target_sec->sh_addr;
            }
            if (from_address < rodata_section->sh_addr)
            {
              tcc_error_noabort("R_ARM_ABS32 relocation outside of data sections");
            }
            from_address -= rodata_section->sh_addr;
            if (from_address < rodata_section->sh_size)
            {
              // relocation inside .rodata
              original_offset = *(uint32_t *)(rodata_section->data + from_address);
            }
            else if (from_address < rodata_section->sh_size + data_section->sh_size)
            {

              original_offset = *(uint32_t *)(data_section->data + from_address - rodata_section->sh_size);
            }
            else if (from_address <
                     rodata_section->sh_size + data_section->sh_size + bss_section->sh_size + s1->got->sh_size)
            {
              original_offset = *(uint32_t *)(s1->got->data + rel->r_offset - s1->got->sh_addr);
            }
            else
            {
              tcc_error_noabort("R_ARM_ABS32 relocation outside of data "
                                "sections or inside bss");
            }

            towards_code = original_offset < rodata_section->sh_addr;
            if (!towards_code)
            {
              original_offset -= rodata_section->sh_addr;
            }

            entry = (YaffDataRelocationEntry){
                .to = from_address,
                .section = towards_code ? YAFF_SECTION_CODE : YAFF_SECTION_DATA,
                .from = original_offset,
            };
            fwrite(&entry, 1, sizeof(entry), f);
            ++number_of_data_relocations;
          }
          break;
          case R_ARM_CALL:
          case R_ARM_JUMP24:
          case R_ARM_GOT32:
          case R_ARM_GLOB_DAT:
          case R_ARM_JUMP_SLOT:
          case R_ARM_REL32:
          case R_ARM_THM_JUMP24:
          case R_ARM_PREL31:
          case R_ARM_TARGET1:
          case R_ARM_NONE:
          case R_ARM_GOTOFF:
          case R_ARM_GOTPC:
          case R_ARM_GOT_PREL:
          case R_ARM_PC24:
          case R_ARM_PLT32:
          case R_ARM_THM_PC22:
          case R_ARM_THM_JUMP19:
          case R_ARM_V4BX:
          case R_ARM_THM_ALU_PREL_11_0:
          case R_ARM_THM_JUMP6:
          case R_ARM_THM_PC12:
          case R_ARM_THM_PC8:
          case R_ARM_COPY:
          case R_ARM_MOVT_ABS:
          case R_ARM_MOVW_ABS_NC:
          case R_ARM_THM_MOVT_ABS:
          case R_ARM_THM_MOVW_ABS_NC:
          case R_ARM_MOVT_PREL:
          case R_ARM_MOVW_PREL_NC:
          {
            // relocations resolved at link time - no runtime data relocation
            // needed (PC-relative, GOT-relative, or already patched inline)
            continue;
          }
          default:
          {
            tcc_warning("unknown relocation type %d", type);
            break;
          }
          }
        }
      }
    }
  }
  return number_of_data_relocations;
}

static int tcc_yaff_write_symbol_table_relocations(TCCState *s1, FILE *f)
{
  int i;
  Section *s;
  ElfW(Sym) * sym;
  int number_of_symbol_table_relocations = 0;
  int number_of_imported_symbols = 0;

  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym))
  {
    if (sym->st_shndx == SHN_UNDEF)
    {
      ++number_of_imported_symbols;
    }
  }

  /* Pre-scan: build a set of symbol indices that have JUMP_SLOT (PLT)
   * relocations. These are known function symbols. This is needed because
   * tccelf.c strips STT_FUNC to STT_NOTYPE for undefined symbols in
   * TCC_OUTPUT_OBJ mode, so the GLOB_DAT handler below can no longer
   * rely solely on st_info to detect function pointers.  Symbols that
   * appear in both JUMP_SLOT (direct call) and GLOB_DAT (address taken)
   * are functions whose GLOB_DAT entry needs a thunk. */
  int dynsym_count = s1->dynsym->data_offset / sizeof(ElfW(Sym));
  unsigned char *has_jump_slot = tcc_mallocz(dynsym_count);
  for (int j = 0; j < s1->nb_sections; ++j)
  {
    Section *sec;
    if (!j)
      continue;
    sec = s1->sections[j];
    if (sec->sh_type != SHT_REL || sec->link != s1->dynsym)
      continue;
    ElfW_Rel *r;
    for_each_elem(sec, 0, r, ElfW_Rel)
    {
      if (ELFW(R_TYPE)(r->r_info) == R_ARM_JUMP_SLOT)
      {
        int idx = ELFW(R_SYM)(r->r_info);
        if (idx >= 0 && idx < dynsym_count)
          has_jump_slot[idx] = 1;
      }
    }
  }

  for (i = 0; i < s1->nb_sections; ++i)
  {
    if (i)
    {
      s = s1->sections[i];
      if (s->sh_type == SHT_REL)
      {
        /* Only process relocation sections linked to dynsym.
           Sections linked to symtab_section (e.g. .rel.data, .rel.rodata)
           contain static relocations with symbol indices into the static
           symbol table - those are handled by tcc_yaff_write_data_relocations
           instead. */
        if (s->link != s1->dynsym)
          continue;
        ElfW_Rel *rel;
        for_each_elem(s, 0, rel, ElfW_Rel)
        {
          int symbol_index = ELFW(R_SYM)(rel->r_info);
          int type = ELFW(R_TYPE)(rel->r_info);
          // index is not necessarily the same in the symtab and imported
          // symbols table
          ElfW(Sym) *sym = &((ElfW(Sym) *)s1->dynsym->data)[symbol_index];
          YaffSymbolTableRelocationEntry entry;
          switch (type)
          {
          // same as R_ARM_GOT_BREL
          case R_ARM_GLOB_DAT:
          case R_ARM_JUMP_SLOT:
          case R_ARM_GOT32:
          {
            // this is exported symbol
            int is_exported = (sym->st_shndx != SHN_UNDEF);
            int symbol_table_index = symbol_index - 1;
            int is_function_pointer = 0;
            if (type == R_ARM_GLOB_DAT &&
                (ELFW(ST_TYPE)(sym->st_info) == STT_FUNC ||
                 (has_jump_slot && symbol_index < dynsym_count && has_jump_slot[symbol_index])))
            {
              is_function_pointer = 1;
            }
            if (is_exported)
            {
              symbol_table_index = symbol_index - 1 - number_of_imported_symbols;
            }
            entry = (YaffSymbolTableRelocationEntry){
                .is_exported_symbol = is_exported,
                .index = (rel->r_offset - s1->got->sh_addr) / 8,
                .function_pointer = is_function_pointer,
                .symbol_index = symbol_table_index + 1,
            };
            fwrite(&entry, 1, sizeof(entry), f);
            ++number_of_symbol_table_relocations;
          }
          break;
          case R_ARM_REL32:
          case R_ARM_RELATIVE:
          case R_ARM_CALL:
          case R_ARM_JUMP24:
          case R_ARM_THM_JUMP24:
          case R_ARM_ABS32:
          case R_ARM_PREL31:
          case R_ARM_TARGET1:
          case R_ARM_NONE:
          {
            // relocations that are safe to ignore due to their PC relative
            // nature
            continue;
          }
          default:
          {
            tcc_warning("unknown relocation type %d", type);
            break;
          }
          }
        }
      }
    }
  }
  tcc_free(has_jump_slot);
  return number_of_symbol_table_relocations;
}

static int tcc_yaff_write_imported_symbols(TCCState *s1, FILE *f, YaffHeader *h)
{
  ElfW(Sym) * sym;
  int number_of_imported_symbols = 1;
  /* Allocate common symbols in BSS.  */
  YaffSymbolEntry entry = {
      0,
      0,
  };
  fwrite(&entry, 1, sizeof(entry), f);
  for (int j = 0; j < 4; ++j)
  {
    fputc(0, f);
  }
  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym))
  {
    YaffSymbolEntry entry = {};
    int name_len = 0, aligned_name_len = 0;
    char *name = NULL;
    if (sym->st_shndx != SHN_UNDEF)
    {
      continue;
    }
    entry = (YaffSymbolEntry){
        .section = 0,
        .offset = sym->st_value,
    };

    number_of_imported_symbols++;
    fwrite(&entry, 1, sizeof(entry), f);
    name = (char *)s1->dynsym->link->data + sym->st_name;
    name_len = strlen(name) + 1;

    fwrite(name, 1, name_len, f);
    aligned_name_len = tcc_yaff_align(h, name_len);
    for (int j = 0; j < aligned_name_len - name_len; ++j)
    {
      fputc(0, f);
    }
  }
  return number_of_imported_symbols;
}

static int tcc_yaff_write_exported_symbols(TCCState *s1, FILE *f, YaffHeader *h)
{
  ElfW(Sym) * sym;
  int number_of_exported_symbols = 1;
  YaffSymbolEntry entry = {
      0,
      0,
  };
  fwrite(&entry, 1, sizeof(entry), f);
  for (int j = 0; j < 4; ++j)
  {
    fputc(0, f);
  }
  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym))
  {
    int section_code = 0;
    YaffSymbolEntry entry = {};
    int name_len = 0, aligned_name_len = 0;
    char *name = NULL;
    uint32_t offset = 0;
    unsigned vis = ELFW(ST_VISIBILITY)(sym->st_other);
    unsigned bind = ELFW(ST_BIND)(sym->st_info);
    if (sym->st_shndx == SHN_UNDEF || (bind != STB_GLOBAL && bind != STB_WEAK) ||
        (vis != STV_DEFAULT && vis != STV_PROTECTED))
    {
      continue;
    }
    if (sym->st_shndx == text_section->sh_num)
    {
      section_code = YAFF_SECTION_CODE;
    }
    else if (sym->st_shndx == data_section->sh_num || sym->st_shndx == rodata_section->sh_num ||
             sym->st_shndx == bss_section->sh_num)
    {
      section_code = YAFF_SECTION_DATA;
    }
    else
    {
      section_code = YAFF_SECTION_CODE;
    }
    offset = sym->st_value;
    if (section_code == YAFF_SECTION_DATA)
    {
      offset -= rodata_section->sh_addr;
    }
    entry = (YaffSymbolEntry){
        .section = section_code,
        .offset = offset,
    };
    number_of_exported_symbols++;
    fwrite(&entry, 1, sizeof(entry), f);
    name = (char *)s1->dynsym->link->data + sym->st_name;
    name_len = strlen(name) + 1;
    fwrite(name, 1, name_len, f);
    aligned_name_len = tcc_yaff_align(h, name_len);
    for (int j = 0; j < aligned_name_len - name_len; ++j)
    {
      fputc(0, f);
    }
  }
  return number_of_exported_symbols;
}

static void tcc_yaff_write_imported_symbols_lookup(TCCState *s1, FILE *f, YaffHeader *h, YaffHashTable *hashtable)
{
  int i = 0;
  ElfW(Sym) * sym;
  int current_offset = 0;
  YaffLookupEntry entry = {
      .symbol_offset = current_offset,
  };

  tcc_add_hash_entry(hashtable, "", i++);
  fwrite(&entry, sizeof(entry), 1, f);
  current_offset += sizeof(uint32_t) + 4;
  /* Allocate common symbols in BSS.  */
  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym))
  {
    int name_len = 0, aligned_name_len = 0;
    char *name = NULL;
    entry.symbol_offset = current_offset;
    if (sym->st_shndx != SHN_UNDEF)
    {
      continue;
    }
    name = (char *)s1->dynsym->link->data + sym->st_name;
    tcc_add_hash_entry(hashtable, name, i++);
    name_len = strlen(name) + 1;
    aligned_name_len = tcc_yaff_align(h, name_len);
    current_offset += sizeof(uint32_t) + aligned_name_len;
    fwrite(&entry, sizeof(entry), 1, f);
  }
}

static void tcc_yaff_write_exported_symbols_lookup(TCCState *s1, FILE *f, YaffHeader *h, YaffHashTable *hashtable)
{
  int i = 0;
  ElfW(Sym) * sym;
  int current_offset = 0; // sizeof(uint32_t) + 4;
  YaffLookupEntry entry = {
      .symbol_offset = current_offset,
  };
  tcc_add_hash_entry(hashtable, "", i++);
  fwrite(&entry, sizeof(entry), 1, f);
  current_offset += sizeof(uint32_t) + 4;
  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym))
  {
    int name_len = 0, aligned_name_len = 0;
    char *name = NULL;
    unsigned vis = ELFW(ST_VISIBILITY)(sym->st_other);
    unsigned bind = ELFW(ST_BIND)(sym->st_info);
    if (sym->st_shndx == SHN_UNDEF || (bind != STB_GLOBAL && bind != STB_WEAK) ||
        (vis != STV_DEFAULT && vis != STV_PROTECTED))
    {
      continue;
    }

    entry.symbol_offset = current_offset;
    name = (char *)s1->dynsym->link->data + sym->st_name;
    tcc_add_hash_entry(hashtable, name, i++);
    name_len = strlen(name) + 1;
    aligned_name_len = tcc_yaff_align(h, name_len);
    current_offset += sizeof(uint32_t) + aligned_name_len;
    fwrite(&entry, sizeof(entry), 1, f);
  }
}

ST_FUNC int tcc_output_yaff(TCCState *s1, FILE *f, const char *filename)
{
  int i, file_type;
  YaffHeader header = {};
  char *name;
  uint32_t aligned_name_len = 0;
  uint32_t text_offset = 0, aligned_text_offset = 0;
  YaffHashTable imported_symbols_hashtable;
  YaffHashTable exported_symbols_hashtable;
  fflush(stdout);

  /* Materialize lazy sections before accessing their data pointers */
  section_materialize(s1, text_section);
  section_materialize(s1, rodata_section);
  section_materialize(s1, data_section);
  if (s1->got)
    section_materialize(s1, s1->got);
  if (s1->plt)
    section_materialize(s1, s1->plt);

  name = tcc_basename(filename);

  file_type = s1->output_type;

  memcpy(header.magic, YAFFMAG, sizeof(YAFFMAG) - 1);
  header.module_type = file_type == TCC_OUTPUT_DYN ? 2 : 1;
  if (s1->elf_entryname)
  {
    header.entry = get_sym_addr(s1, s1->elf_entryname, 1, 0);
  }
  else
  {
    header.entry = get_sym_addr(s1, "_start", !!(file_type & TCC_OUTPUT_EXE), 0);
  }
  if (header.entry == (addr_t)-1)
  {
    header.entry = 0;
  }
  if (s1->nb_errors)
  {
    return -1;
  }

  header.yaff_version = 1;
  header.arch = 1;
  header.code_length = text_section->sh_size;
  header.init_length = 0;
  header.data_length = data_section->sh_size + rodata_section->sh_size;
  header.bss_length = bss_section->sh_size;
  header.external_libraries_amount = 0;
  header.alignment = 4;
  header.version_major = 0;
  header.version_minor = 0;
  header.external_libraries_amount = s1->nb_loaded_dlls;
  if (s1->text_and_data_separation)
  {
    header.text_and_data_separation = 1;
  }
  else
  {
    header.text_and_data_separation = 0;
  }

  fwrite(&header, 1, sizeof(YaffHeader), f);
  aligned_name_len = strlen(name) + 1;
  aligned_name_len = tcc_yaff_align(&header, aligned_name_len);
  fwrite(name, 1, strlen(name) + 1, f);
  for (i = 0; i < aligned_name_len - strlen(name) - 1; ++i)
  {
    fputc(0, f);
  }

  header.arch_section_offset = 0; // ftell(f);
  header.imported_libraries_offset = ftell(f);
  for (i = 0; i < s1->nb_loaded_dlls; ++i)
  {
    DLLReference *dll = s1->loaded_dlls[i];
    aligned_name_len = strlen(dll->name) + 1;
    aligned_name_len = tcc_yaff_align(&header, aligned_name_len);
    fwrite(dll->name, 1, strlen(dll->name) + 1, f);
    for (int j = 0; j < aligned_name_len - strlen(dll->name) - 1; ++j)
    {
      fputc(0, f);
    }
  }

  // tcc_elf_sort_syms(s1, s1->symtab);
  header.relocations_offset = ftell(f);
  header.symbol_table_relocations_amount = tcc_yaff_write_symbol_table_relocations(s1, f);
  header.local_relocations_amount = tcc_yaff_write_local_relocations(s1, f);
  header.data_relocations_amount = tcc_yaff_write_data_relocations(s1, f);

  header.imported_symbols_offset = ftell(f);
  header.imported_symbols_amount = tcc_yaff_write_imported_symbols(s1, f, &header);
  header.exported_symbols_offset = ftell(f);
  header.exported_symbols_amount = tcc_yaff_write_exported_symbols(s1, f, &header);
  header.imported_symbols_lookup_offset = ftell(f);

  tcc_allocate_hash_table(&imported_symbols_hashtable, header.imported_symbols_amount + 1,
                          header.imported_symbols_amount);
  tcc_yaff_write_imported_symbols_lookup(s1, f, &header, &imported_symbols_hashtable);
  header.exported_symbols_lookup_offset = ftell(f);

  tcc_allocate_hash_table(&exported_symbols_hashtable, header.exported_symbols_amount + 1,
                          header.exported_symbols_amount);
  tcc_yaff_write_exported_symbols_lookup(s1, f, &header, &exported_symbols_hashtable);

  header.imported_symbols_hash_table_offset = ftell(f);
  if (header.imported_symbols_hash_table_offset % header.alignment != 0)
  {
    for (i = 0; i < header.alignment - (header.imported_symbols_hash_table_offset % header.alignment); ++i)
    {
      fputc(0, f);
    }
    header.imported_symbols_hash_table_offset = ftell(f); // align after padding
  }
  tcc_write_hash_table(&imported_symbols_hashtable, f);
  header.exported_symbols_hash_table_offset = ftell(f);
  if (header.exported_symbols_hash_table_offset % header.alignment != 0)
  {
    for (i = 0; i < header.alignment - (header.exported_symbols_hash_table_offset % header.alignment); ++i)
    {
      fputc(0, f);
    }
    header.exported_symbols_hash_table_offset = ftell(f); // align after padding
  }

  tcc_write_hash_table(&exported_symbols_hashtable, f);

  header.got_length = s1->got->sh_size;
  text_offset = ftell(f);
  aligned_text_offset = (text_offset + 15) & ~15;
  for (i = 0; i < aligned_text_offset - text_offset; ++i)
  {
    fputc(0, f);
  }
  header.text_offset = aligned_text_offset;

  fwrite(text_section->data, 1, text_section->sh_size, f);
  if (s1->plt)
  {
    fwrite(s1->plt->data, 1, s1->plt->sh_size, f);
    header.plt_length = s1->plt->sh_size;
  }

  fwrite(rodata_section->data, 1, rodata_section->sh_size, f);
  fwrite(data_section->data, 1, data_section->sh_size, f);
  fwrite(s1->got->data, 1, s1->got->sh_size, f);
  fseek(f, 0, SEEK_SET);
  fwrite(&header, 1, sizeof(YaffHeader), f);
  fflush(f);

  return 0;
}