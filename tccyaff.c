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

#define TCC_YAFF_MAX_SYMBOL_ENTRY_SIZE 255

#define SHF_DYNSYM 0x40000000

typedef struct __attribute__((packed)) YaffHeader {
  uint8_t magic[4];
  uint8_t module_type;
  uint16_t arch;
  uint8_t yaff_version;
  uint32_t code_length;
  uint32_t init_length;
  uint32_t data_length;
  uint32_t bss_length;
  uint32_t entry;
  uint16_t external_libraries_amount;
  uint8_t alignment;
  uint8_t text_and_data_separation;
  uint16_t version_major;
  uint16_t version_minor;
  uint16_t symbol_table_relocations_amount;
  uint16_t local_relocations_amount;
  uint16_t data_relocations_amount;
  uint16_t _reserved2;
  uint16_t exported_symbols_amount;
  uint16_t imported_symbols_amount;
  uint32_t got_length;
  uint32_t got_plt_length;
  uint32_t plt_length;
  // TODO: remove or move to the arch section
  uint16_t arch_section_offset;
  uint16_t imported_libraries_offset;
  uint16_t relocations_offset;
  uint16_t imported_symbols_offset;
  uint16_t exported_symbols_offset;
  uint16_t text_offset;
} YaffHeader;

typedef enum YaffSectionCode {
  YAFF_SECTION_CODE = 0,
  YAFF_SECTION_DATA = 1,
  YAFF_SECTION_INIT = 2,
  YAFF_SECTION_UNKNOWN = 3,
} YaffSectionCode;

typedef struct __attribute__((packed)) YaffSymbolTableRelocationEntry {
  uint32_t is_exported_symbol : 1;
  uint32_t index : 31;
  uint32_t symbol_index;
} YaffSymbolTableRelocationEntry;

typedef struct __attribute__((packed)) YaffDataRelocationEntry {
  uint32_t to;
  uint32_t section : 2;
  uint32_t from : 30;
} YaffDataRelocationEntry;

typedef struct __attribute__((packed)) YaffLocalRelocationEntry {
  uint32_t section : 2;
  uint32_t index : 30;
  uint32_t target_offset;
} YaffLocalRelocationEntry;

typedef struct __attribute__((packed)) YaffSymbolEntry {
  uint32_t section : 2;
  uint32_t offset : 30;
  char name[0];
} YaffSymbolEntry;

uint32_t tcc_yaff_align(YaffHeader *header, uint32_t size) {
  return (size + header->alignment - 1) & ~(header->alignment - 1);
}

const char *tcc_parse_object_name(YaffHeader *header) {
  return (const char *)(header) + sizeof(YaffHeader);
}

uint32_t tcc_get_offset_to_imported_libraries(YaffHeader *header) {
  const uint32_t name_length = strlen(tcc_parse_object_name(header)) + 1;
  return sizeof(YaffHeader) + tcc_yaff_align(header, name_length);
}

ST_FUNC int tcc_load_yaff(TCCState *s1, int fd, const char *filename,
                          int level) {
  int ret = 0;
  const char *soname = tcc_basename(filename);
  YaffHeader header;
  full_read(fd, &header, sizeof(YaffHeader));
  if (memcmp(header.magic, "YAFF", 4) != 0) {
    return tcc_error_noabort("not a valid YAFF file");
  }

  char buffer[TCC_YAFF_MAX_SYMBOL_ENTRY_SIZE];
  uint32_t offset = header.exported_symbols_offset;

  for (int i = 0; i < header.exported_symbols_amount; ++i) {
    lseek(fd, offset, SEEK_SET);
    full_read(fd, buffer, sizeof(buffer));

    YaffSymbolEntry *entry = (YaffSymbolEntry *)buffer;
    size_t len = strnlen(entry->name, sizeof(buffer));
    if (len == sizeof(buffer)) {
      return tcc_error_noabort("symbol entry too long");
    }
    set_elf_sym(s1->dynsymtab_section, entry->offset, 1, STB_GLOBAL << 4,
                STV_DEFAULT, 1, entry->name);
    offset += sizeof(uint32_t) + len + 1;
    offset = tcc_yaff_align(&header, offset);
  }

  /* if the dll is already loaded, do not load it */
  tcc_add_dllref(s1, soname, level);

  return ret;
}

static int tcc_yaff_write_data_relocations(TCCState *s1, FILE *f) {
  int i;
  Section *s;
  int number_of_data_relocations = 0;
  for (i = 0; i < s1->nb_sections; ++i) {
    if (i) {
      s = s1->sections[i];
      if (s->sh_type == SHT_REL) {
        ElfW_Rel *rel;
        for_each_elem(s, 0, rel, ElfW_Rel) {
          int symbol_index = ELFW(R_SYM)(rel->r_info);
          int type = ELFW(R_TYPE)(rel->r_info);
          switch (type) {
          // same as R_ARM_GOT_BREL
          case R_ARM_ABS32:
          case R_ARM_RELATIVE: {
            // first data section is rodata
            uint32_t from_address = rel->r_offset;
            uint32_t original_offset = 0;
            uint32_t sections_size_sum = 0;
            bool towards_code = false;
            if (from_address < rodata_section->sh_addr) {
              tcc_error_noabort(
                  "R_ARM_ABS32 relocation outside of data sections");
            }
            from_address -= rodata_section->sh_addr;
            if (from_address < rodata_section->sh_size) {
              // relocation inside .rodata
              original_offset =
                  *(uint32_t *)(rodata_section->data + from_address);

            } else if (from_address <
                       rodata_section->sh_size + data_section->sh_size) {

              original_offset =
                  *(uint32_t *)(data_section->data + rel->r_offset -
                                data_section->sh_addr);
            } else if (from_address <
                       rodata_section->sh_size + data_section->sh_size +
                           bss_section->sh_size + s1->got->sh_size) {
              original_offset = *(uint32_t *)(s1->got->data + rel->r_offset -
                                              s1->got->sh_addr);
            } else {
              tcc_error_noabort("R_ARM_ABS32 relocation outside of data "
                                "sections or inside bss");
            }

            towards_code = original_offset < rodata_section->sh_addr;
            if (!towards_code) {
              original_offset -= rodata_section->sh_addr;
            }

            YaffDataRelocationEntry entry = {
                .to = from_address,
                .section = towards_code ? YAFF_SECTION_CODE : YAFF_SECTION_DATA,
                .from = original_offset,
            };
            fwrite(&entry, 1, sizeof(entry), f);
            ++number_of_data_relocations;
          } break;
          case R_ARM_CALL:
          case R_ARM_JUMP24:
          case R_ARM_GOT32:
          case R_ARM_GLOB_DAT:
          case R_ARM_JUMP_SLOT:
          case R_ARM_REL32:
          case R_ARM_THM_JUMP24:
          case R_ARM_PREL31:
          case R_ARM_TARGET1:
          case R_ARM_NONE: {
            // relocations that are safe to ignore due to their PC relative
            // nature
            continue;
          }
          default: {
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

static int tcc_yaff_write_symbol_table_relocations(TCCState *s1, FILE *f) {
  int i;
  Section *s;
  ElfW(Sym) * sym;
  int number_of_symbol_table_relocations = 0;
  int number_of_imported_symbols = 0;

  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym)) {
    if (sym->st_shndx == SHN_UNDEF) {
      ++number_of_imported_symbols;
    }
  }

  for (i = 0; i < s1->nb_sections; ++i) {
    if (i) {
      s = s1->sections[i];
      if (s->sh_type == SHT_REL) {
        ElfW_Rel *rel;
        for_each_elem(s, 0, rel, ElfW_Rel) {
          int symbol_index = ELFW(R_SYM)(rel->r_info);
          int type = ELFW(R_TYPE)(rel->r_info);
          // index is not necessarily the same in the symtab and imported
          // symbols table
          ElfW(Sym) *sym = &((ElfW(Sym) *)s1->dynsym->data)[symbol_index];
          int visibility = ELFW(ST_VISIBILITY)(sym->st_other);
          switch (type) {
          // same as R_ARM_GOT_BREL
          case R_ARM_GLOB_DAT:
          case R_ARM_JUMP_SLOT:
          case R_ARM_GOT32: {
            // this is exported symbol
            int is_exported = (sym->st_shndx != SHN_UNDEF);
            int symbol_table_index = symbol_index - 1;
            if (is_exported) {
              symbol_table_index =
                  symbol_index - 1 - number_of_imported_symbols;
            }
            printf("symbol table index: %d, for: %s\n", symbol_table_index,
                   (char *)s1->dynsym->link->data + sym->st_name);
            YaffSymbolTableRelocationEntry entry = {
                .is_exported_symbol = is_exported,
                .index = (rel->r_offset - s1->got->sh_addr) / 8,
                .symbol_index = symbol_table_index,
            };
            fwrite(&entry, 1, sizeof(entry), f);
            ++number_of_symbol_table_relocations;
          } break;
          case R_ARM_REL32:
          case R_ARM_RELATIVE:
          case R_ARM_CALL:
          case R_ARM_JUMP24:
          case R_ARM_THM_JUMP24:
          case R_ARM_ABS32:
          case R_ARM_PREL31:
          case R_ARM_TARGET1:
          case R_ARM_NONE: {
            // relocations that are safe to ignore due to their PC relative
            // nature
            continue;
          }
          default: {
            tcc_warning("unknown relocation type %d", type);
            break;
          }
          }
        }
      }
    }
  }
  return number_of_symbol_table_relocations;
}

static int tcc_yaff_write_imported_symbols(TCCState *s1, FILE *f,
                                           YaffHeader *h) {
  int i;
  ElfW(Sym) * sym;
  int number_of_imported_symbols = 0;
  /* Allocate common symbols in BSS.  */
  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym)) {
    int section_code = 0;
    YaffSymbolEntry entry = {};
    int name_len = 0, aligned_name_len = 0;
    char *name = NULL;
    unsigned vis = ELFW(ST_VISIBILITY)(sym->st_other);
    unsigned bind = ELFW(ST_BIND)(sym->st_info);
    if (sym->st_shndx != SHN_UNDEF) {
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
    for (int j = 0; j < aligned_name_len - name_len; ++j) {
      fputc(0, f);
    }
  }
  return number_of_imported_symbols;
}

static int tcc_yaff_write_exported_symbols(TCCState *s1, FILE *f,
                                           YaffHeader *h) {
  int i;
  ElfW(Sym) * sym;
  int number_of_exported_symbols = 0;
  for_each_elem(s1->dynsym, 1, sym, ElfW(Sym)) {
    int section_code = 0;
    YaffSymbolEntry entry = {};
    int name_len = 0, aligned_name_len = 0;
    char *name = NULL;
    uint32_t offset = 0;
    unsigned vis = ELFW(ST_VISIBILITY)(sym->st_other);
    unsigned bind = ELFW(ST_BIND)(sym->st_info);
    if (sym->st_shndx == SHN_UNDEF ||
        (bind != STB_GLOBAL && bind != STB_WEAK) ||
        (vis != STV_DEFAULT && vis != STV_PROTECTED)) {
      continue;
    }
    if (sym->st_shndx == text_section->sh_num) {
      section_code = YAFF_SECTION_CODE;
    } else if (sym->st_shndx == data_section->sh_num ||
               // add rodata there after verification
               sym->st_shndx == bss_section->sh_num) {
      section_code = YAFF_SECTION_DATA;
    } else {
      section_code = YAFF_SECTION_CODE; // fix rodata and change to unknown
    }
    offset = sym->st_value;
    if (section_code == YAFF_SECTION_DATA) {
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
    for (int j = 0; j < aligned_name_len - name_len; ++j) {
      fputc(0, f);
    }
  }
  return number_of_exported_symbols;
}

ST_FUNC int tcc_output_yaff(TCCState *s1, FILE *f, const char *filename) {
  int i, shnum, offset, size, file_type;
  Section *s;
  ElfW(Ehdr) ehdr;
  ElfW(Shdr) shdr, *sh;
  printf("Writing YAFF file: %s\n", filename);
  fflush(stdout);
  YaffHeader header = {};
  char *name;
  uint32_t aligned_name_len = 0;
  uint32_t text_offset = 0, aligned_text_offset = 0;
  name = tcc_basename(filename);

  file_type = s1->output_type;
  shnum = s1->nb_sections;

  memcpy(header.magic, YAFFMAG, sizeof(YAFFMAG) - 1);
  header.module_type = file_type == TCC_OUTPUT_DYN ? 2 : 1;
  printf("Getting entry\n");
  if (s1->elf_entryname) {
    header.entry = get_sym_addr(s1, s1->elf_entryname, 1, 0);
  } else {
    header.entry =
        get_sym_addr(s1, "_start", !!(file_type & TCC_OUTPUT_EXE), 0);
  }
  if (header.entry == (addr_t)-1) {
    header.entry = 0;
  }
  if (s1->nb_errors) {
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
  if (s1->text_and_data_separation) {
    // header.text_and_data_separation = 1;
  } else {
    header.text_and_data_separation = 0;
  }

  fwrite(&header, 1, sizeof(YaffHeader), f);
  aligned_name_len = strlen(name) + 1;
  aligned_name_len = tcc_yaff_align(&header, aligned_name_len);
  fwrite(name, 1, strlen(name) + 1, f);
  for (i = 0; i < aligned_name_len - strlen(name) - 1; ++i) {
    fputc(0, f);
  }

  header.arch_section_offset = 0; // ftell(f);
  header.imported_libraries_offset = ftell(f);
  printf("number of loaded dlls: %d\n", s1->nb_loaded_dlls);
  for (i = 0; i < s1->nb_loaded_dlls; ++i) {
    printf("Processing DLL: %s\n", s1->loaded_dlls[i]->name);
    DLLReference *dll = s1->loaded_dlls[i];
    aligned_name_len = strlen(dll->name) + 1;
    aligned_name_len = tcc_yaff_align(&header, aligned_name_len);
    fwrite(dll->name, 1, strlen(dll->name) + 1, f);
    for (int j = 0; j < aligned_name_len - strlen(dll->name) - 1; ++j) {
      fputc(0, f);
    }
  }

  // tcc_elf_sort_syms(s1, s1->symtab);
  header.relocations_offset = ftell(f);
  header.symbol_table_relocations_amount =
      tcc_yaff_write_symbol_table_relocations(s1, f);
  header.data_relocations_amount = tcc_yaff_write_data_relocations(s1, f);

  header.imported_symbols_offset = ftell(f);
  header.imported_symbols_amount =
      tcc_yaff_write_imported_symbols(s1, f, &header);
  header.exported_symbols_offset = ftell(f);
  header.exported_symbols_amount =
      tcc_yaff_write_exported_symbols(s1, f, &header);
  header.got_length = s1->got->sh_size;
  text_offset = ftell(f);
  aligned_text_offset = (text_offset + 15) & ~15;
  for (i = 0; i < aligned_text_offset - text_offset; ++i) {
    fputc(0, f);
  }
  header.text_offset = aligned_text_offset;

  fwrite(text_section->data, 1, text_section->sh_size, f);
  if (s1->plt) {
    fwrite(s1->plt->data, 1, s1->plt->sh_size, f);
    header.plt_length = s1->plt->sh_size;
  }
  fwrite(rodata_section->data, 1, rodata_section->sh_size, f);
  fwrite(data_section->data, 1, data_section->sh_size, f);
  int foff = ftell(f);
  printf("Writing at: %x, size of got: %d\n", foff, s1->got->sh_size);
  fwrite(s1->got->data, 1, s1->got->sh_size, f);

  printf("Writing sr: %d\n", header.symbol_table_relocations_amount);
  fseek(f, 0, SEEK_SET);
  fwrite(&header, 1, sizeof(YaffHeader), f);
  fflush(f);

  return 0;
}