#pragma once

#include <stdint.h>

typedef struct {
  uint32_t nbucket;
  uint32_t nchain;
  uint32_t *bucket;
  uint32_t *chain;
} YaffHashTable;

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
  uint16_t copy_relocations_amount;
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
  uint16_t imported_symbols_lookup_offset;
  uint16_t exported_symbols_lookup_offset;
  uint16_t imported_symbols_hash_table_offset;
  uint16_t exported_symbols_hash_table_offset;
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
  uint32_t function_pointer : 1;
  uint32_t plt_call : 1;
  uint32_t symbol_index : 30;
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

typedef struct __attribute__((packed)) YaffCopyRelocationEntry {
  uint32_t bss_offset;
  uint32_t symbol_index;
  uint32_t size;
} YaffCopyRelocationEntry;

typedef struct __attribute__((packed)) YaffLookupEntry {
  uint16_t symbol_offset;
} YaffLookupEntry;

typedef struct __attribute__((packed)) YaffSymbolEntry {
  uint32_t section : 2;
  uint32_t weak : 1;
  uint32_t offset : 29;
  char name[0];
} YaffSymbolEntry;