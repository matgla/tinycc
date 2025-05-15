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

#define TCC_YAFF_MAX_SYMBOL_ENTRY_SIZE 255

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
  uint8_t _reserved;
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
#define SHF_DYNSYM 0x40000000
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
  int ret = -1;
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

  return ret;
}
