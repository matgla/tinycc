/*
 *  elfsec_stubs.h - minimal ELF/section stub layer for ir/opt_switch_data.c
 *
 *  opt_switch_data.c's two passes are IR-only except for the value-table
 *  materialization step in tcc_ir_opt_switch_to_data(), which writes real
 *  bytes into a Section and asks the (real, frontend-owned) symbol table for
 *  a Sym* to reference them. section_add()/section_realloc() here are real,
 *  self-contained reimplementations of tccelf.c's bump allocator (so a test
 *  can read the materialized table bytes back and assert on them);
 *  get_sym_ref()/greloc() are call-logged fakes (so a test can assert a
 *  relocation/symbol was requested, without a real ELF symbol table).
 */

#ifndef TCC_UT_ELFSEC_STUBS_H
#define TCC_UT_ELFSEC_STUBS_H

#define USING_GLOBALS
#include "tcc.h"

/* Allocate a fresh, zeroed Section with the given name; assign the result to
 * rodata_section / data_section (bare identifiers -- under USING_GLOBALS
 * these already expand to tcc_state->rodata_section / ->data_section, so do
 * NOT write tcc_state->rodata_section by hand, that double-expands). */
Section *elfsec_new_section(const char *name);

/* Frees every section elfsec_new_section() has handed out, resets
 * rodata_section/data_section to NULL, tcc_state->share_rodata to 0, and
 * clears both call logs below. Call at the top of every UT_TEST that touches
 * opt_switch_data.c (tcc_state is a single process-lifetime static, so a
 * leftover section pointer from a previous test would otherwise alias). */
void elfsec_reset(void);

/* ---- get_sym_ref() call log ---- */

typedef struct ElfSecSymRefCall
{
  CType *type;
  Section *sec;
  unsigned long offset;
  unsigned long size;
  Sym *returned;
} ElfSecSymRefCall;

int elfsec_sym_ref_call_count(void);
const ElfSecSymRefCall *elfsec_nth_sym_ref_call(int n);

/* ---- greloc() call log ---- */

typedef struct ElfSecRelocCall
{
  Section *sec;
  Sym *sym;
  unsigned long offset;
  int type;
} ElfSecRelocCall;

int elfsec_reloc_call_count(void);
const ElfSecRelocCall *elfsec_nth_reloc_call(int n);

#endif /* TCC_UT_ELFSEC_STUBS_H */
