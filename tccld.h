/*
 *  TCC - Tiny C Compiler
 *
 *  Linker Script Support
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

#ifndef TCC_LD_H
#define TCC_LD_H

#include <stdint.h>

#define LD_MAX_MEMORY_REGIONS 16
#define LD_MAX_OUTPUT_SECTIONS 64
#define LD_MAX_PHDRS 16
#define LD_MAX_SYMBOLS 128
#define LD_MAX_SECTION_PATTERNS 32

/* Memory region attributes */
#define LD_MEM_READ 0x01
#define LD_MEM_WRITE 0x02
#define LD_MEM_EXEC 0x04
#define LD_MEM_ALLOC 0x08

/* Symbol visibility */
#define LD_SYM_GLOBAL 0
#define LD_SYM_HIDDEN 1
#define LD_SYM_PROVIDE 2
#define LD_SYM_PROVIDE_HIDDEN 3

/* Section pattern types */
#define LD_PAT_EXACT 0 /* exact match */
#define LD_PAT_GLOB 1  /* wildcard match like *(.text*) */
#define LD_PAT_KEEP 2  /* KEEP() - don't garbage collect */

typedef struct LDMemoryRegion {
  char name[64];
  uint32_t attributes;
  addr_t origin;
  addr_t length;
  addr_t current; /* current allocation position */
} LDMemoryRegion;

typedef struct LDPhdr {
  char name[64];
  uint32_t type;  /* PT_LOAD, PT_NULL, etc */
  uint32_t flags; /* PF_R, PF_W, PF_X */
} LDPhdr;

typedef struct LDSectionPattern {
  char pattern[128];
  int type; /* LD_PAT_EXACT, LD_PAT_GLOB, LD_PAT_KEEP */
  int keep; /* 1 if KEEP() */
} LDSectionPattern;

typedef struct LDOutputSection {
  char name[64];
  addr_t address;        /* explicit address if set, otherwise 0 */
  addr_t align;          /* alignment requirement */
  addr_t current_offset; /* current offset within section */
  int memory_region_idx; /* index into memory_regions, -1 if none */
  int phdr_idx;          /* index into phdrs, -1 if none */
  int has_address;       /* 1 if address explicitly set */

  /* Section patterns to include */
  LDSectionPattern patterns[LD_MAX_SECTION_PATTERNS];
  int nb_patterns;
} LDOutputSection;

typedef struct LDSymbol {
  char name[128];
  addr_t value;
  int visibility;          /* LD_SYM_GLOBAL, LD_SYM_HIDDEN, etc */
  int defined;             /* 1 if value is defined */
  int is_location_counter; /* 1 if value is current location counter */
  int section_idx; /* output section index where defined, -1 if absolute */
} LDSymbol;

typedef struct LDScript {
  /* MEMORY regions */
  LDMemoryRegion memory_regions[LD_MAX_MEMORY_REGIONS];
  int nb_memory_regions;

  /* PHDRS (program headers) */
  LDPhdr phdrs[LD_MAX_PHDRS];
  int nb_phdrs;

  /* Output sections from SECTIONS command */
  LDOutputSection output_sections[LD_MAX_OUTPUT_SECTIONS];
  int nb_output_sections;

  /* Symbols (PROVIDE, assignments, etc) */
  LDSymbol symbols[LD_MAX_SYMBOLS];
  int nb_symbols;

  /* Entry point */
  char entry_point[128];
  int has_entry;

  /* Current parsing state */
  addr_t location_counter;
  int current_section_idx;
  int current_memory_region_idx;
} LDScript;

/* Forward declaration */
struct TCCState;

/* Initialize linker script structure */
void ld_script_init(LDScript *ld);

/* Parse a linker script file */
int ld_script_parse(struct TCCState *s1, LDScript *ld, int fd);

/* Parse a linker script from string */
int ld_script_parse_string(struct TCCState *s1, LDScript *ld,
                           const char *script);

/* Apply linker script to section layout */
int ld_script_apply(struct TCCState *s1, LDScript *ld);

/* Add standard symbols (__end__, _end, __bss_start__, etc) */
int ld_script_add_standard_symbols(struct TCCState *s1, LDScript *ld);

/* Find memory region by name */
int ld_script_find_memory_region(LDScript *ld, const char *name);

/* Find output section by name */
int ld_script_find_output_section(LDScript *ld, const char *name);

/* Find or create a symbol */
int ld_script_find_or_create_symbol(LDScript *ld, const char *name);

/* Check if section matches a pattern */
int ld_section_matches_pattern(const char *section_name, const char *pattern);

/* Debug: print linker script contents */
void ld_script_dump(LDScript *ld);

#endif /* TCC_LD_H */
