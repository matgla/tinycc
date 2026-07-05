/*
 *  elfsec_stubs.c - shared UT stub, dual-build split.
 *
 *  This file is linked into several unit-test binaries with different needs:
 *    - The main (run_unit_tests), backend (UT2) and other binaries do NOT
 *      link libtcc.c, so they need the full stub layer below (allocators,
 *      the tcc_state global, ELF/section fakes, etc.).
 *    - build_ssaopt (UT11) links the REAL libtcc.c + ir/opt/ssa_opt*.c, which
 *      already own those symbols; it compiles every TU with -DUT_SSA_OPT_REAL
 *      (see the build_ssaopt rules in the Makefile), so the shared stubs must
 *      be skipped there to avoid multiple-definition clashes.
 *
 *  Same guard idiom as ra_link_stubs.c. Keep the two branches in sync when a
 *  new shared symbol is genuinely needed by BOTH builds (define it outside
 *  the guard in that case).
 */
#ifndef UT_SSA_OPT_REAL
/* ===== shared builds (main / backend / tccpp / ... ): full stub layer ===== */
/*
 *  elfsec_stubs.c - minimal ELF/section stub layer for ir/opt_switch_data.c
 *
 *  See elfsec_stubs.h. section_add()/section_realloc() are verbatim (minus
 *  the section-type/alignment bookkeeping opt_switch_data.c never inspects)
 *  reimplementations of tccelf.c's bump allocator; get_sym_ref()/greloc()
 *  are call-recording fakes.
 */

#include "elfsec_stubs.h"

/* CType int_type is ST_DATA'd by tcc.h (tcc.h:2115) and read directly by
 * tcc_ir_opt_switch_to_data()'s get_sym_ref(&int_type, ...) call. The real
 * definition lives in tccgen.c (not linked here -- needs the full frontend
 * type system). func_old_type/char_pointer_type are already stubbed in
 * stubs.c; only int_type is missing. */
CType int_type = { VT_INT, NULL };

#define ELFSEC_MAX_SECTIONS 8
static Section *elfsec_sections[ELFSEC_MAX_SECTIONS];
static int elfsec_section_count;

Section *elfsec_new_section(const char *name)
{
  size_t namelen = strlen(name) + 1;
  Section *sec = (Section *)tcc_mallocz(sizeof(Section) + namelen);
  memcpy(sec->name, name, namelen);
  sec->sh_type = 1 /* SHT_PROGBITS */;
  sec->sh_addralign = 1;
  if (elfsec_section_count < ELFSEC_MAX_SECTIONS)
    elfsec_sections[elfsec_section_count++] = sec;
  return sec;
}

#define ELFSEC_MAX_CALLS 32
static ElfSecSymRefCall elfsec_sym_ref_calls[ELFSEC_MAX_CALLS];
static int elfsec_sym_ref_call_n;
static ElfSecRelocCall elfsec_reloc_calls[ELFSEC_MAX_CALLS];
static int elfsec_reloc_call_n;

void elfsec_reset(void)
{
  for (int i = 0; i < elfsec_section_count; i++)
  {
    tcc_free(elfsec_sections[i]->data);
    tcc_free(elfsec_sections[i]);
  }
  elfsec_section_count = 0;
  rodata_section = NULL;
  data_section = NULL;
  tcc_state->share_rodata = 0;
  elfsec_sym_ref_call_n = 0;
  elfsec_reloc_call_n = 0;
}

int elfsec_sym_ref_call_count(void) { return elfsec_sym_ref_call_n; }
const ElfSecSymRefCall *elfsec_nth_sym_ref_call(int n)
{
  return (n >= 0 && n < elfsec_sym_ref_call_n) ? &elfsec_sym_ref_calls[n] : NULL;
}

int elfsec_reloc_call_count(void) { return elfsec_reloc_call_n; }
const ElfSecRelocCall *elfsec_nth_reloc_call(int n)
{
  return (n >= 0 && n < elfsec_reloc_call_n) ? &elfsec_reloc_calls[n] : NULL;
}

/* ---- real symbols opt_switch_data.c links against ---- */

/* Verbatim reimplementation of tccelf.c's section_realloc/section_add (the
 * real file is not linked here -- it needs the full ELF writer). */
static void elfsec_section_realloc(Section *sec, unsigned long new_size)
{
  unsigned long size = sec->data_allocated;
  if (size == 0)
  {
    size = 256;
    while (size < new_size)
      size *= 2;
  }
  else
  {
    while (size < new_size)
      size *= 2;
  }
  unsigned char *data = (unsigned char *)tcc_realloc(sec->data, size);
  memset(data + sec->data_allocated, 0, size - sec->data_allocated);
  sec->data = data;
  sec->data_allocated = size;
}

size_t section_add(Section *sec, addr_t size, int align)
{
  size_t offset = (sec->data_offset + align - 1) & -(unsigned long)align;
  size_t offset1 = offset + size;
  if (offset1 > sec->data_allocated)
    elfsec_section_realloc(sec, offset1);
  sec->data_offset = offset1;
  if (align > sec->sh_addralign)
    sec->sh_addralign = align;
  return offset;
}

Sym *get_sym_ref(CType *type, Section *sec, unsigned long offset, unsigned long size)
{
  Sym *sym = (Sym *)tcc_mallocz(sizeof(Sym));
  if (type)
    sym->type = *type;
  sym->r = VT_CONST | VT_SYM;

  if (elfsec_sym_ref_call_n < ELFSEC_MAX_CALLS)
  {
    ElfSecSymRefCall *c = &elfsec_sym_ref_calls[elfsec_sym_ref_call_n++];
    c->type = type;
    c->sec = sec;
    c->offset = offset;
    c->size = size;
    c->returned = sym;
  }
  return sym;
}

void greloc(Section *s, Sym *sym, unsigned long offset, int type)
{
  if (elfsec_reloc_call_n < ELFSEC_MAX_CALLS)
  {
    ElfSecRelocCall *c = &elfsec_reloc_calls[elfsec_reloc_call_n++];
    c->sec = s;
    c->sym = sym;
    c->offset = offset;
    c->type = type;
  }
}

#else /* UT_SSA_OPT_REAL — build_ssaopt (UT11) ===================== */
/*
 *  elfsec_stubs.c - stubs for ELF section handling
 *
 *  Provides stubs for ELF section functions that are not needed in the
 *  isolated unit test environment.
 */

#define USING_GLOBALS
#include "tcc.h"

/* Stub: tcc ELF section functions. */
void tcc_elf_add_sec(void *s, const char *name, unsigned long addr,
                     unsigned long size, unsigned long flags)
{
  /* Do nothing. */
}

void tcc_elf_add_sec_idx(void *s, const char *name, unsigned long addr,
                         unsigned long size, unsigned long flags,
                         unsigned long idx)
{
  /* Do nothing. */
}
#endif /* UT_SSA_OPT_REAL */
