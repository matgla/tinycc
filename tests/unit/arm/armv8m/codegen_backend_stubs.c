/*
 *  codegen_backend_stubs.c - link stubs for the backend/ binary
 *
 *  See codegen_backend_stubs.h. Each stub's provenance/rationale is
 *  documented inline; found by trial-linking a fresh arm-thumb-gen.o against
 *  the existing UT_MODULE_OBJS set (docs on the investigation that produced
 *  this list live in the design-sweep session that scoped this binary).
 */

#include "codegen_backend_stubs.h"
#include "tccopt.h"

#include <stdio.h>
#include <stdlib.h>

/* From tccgen.c — true if the current function is variadic. Every mop test
 * builds a fixed-arity function, so 0 is correct for all of them. */
/* func_var now lives in ra_link_stubs.c, which this binary also links. */

/* From tccgen.c. Only reached via arm_init()'s func_float_type/
 * func_double_type setup -- those two CTypes are never read anywhere after
 * being set (confirmed by a whole-tree grep), so a NULL-returning stub is
 * provably correct, not just convenient. */
Sym *sym_push(int v, CType *type, int r, int c)
{
  (void)v; (void)type; (void)r; (void)c;
  return NULL;
}

/* From tccgen.c.  gsym() is reached from tcc_machine_load_jmp_result (a legacy
 * VT_JMP helper with zero product callers, exercised by
 * test_load_jmp_result_*).  Mirror the real tccgen.c gsym() exactly: t <= 0 is
 * "no chain" and a no-op (the -1 = no chain / 0 = offset-0 sentinels), while a
 * real chain (t > 0) backpatches via arm-thumb-gen.c's gsym_addr(), which is
 * linked into this binary.  The test passes t == 0, so the no-op path runs. */
void gsym(int t)
{
  if (t > 0)
    gsym_addr(t, ind);
}

void vpop(void)
{
  fprintf(stderr, "[test stub] vpop: unexpectedly called (frontend VLA decl "
                   "path is not supported by this harness)\n");
  abort();
}

/* From tccelf.c. arm-thumb-gen.c's literal-pool/relocation emission calls
 * put_extern_sym() to register a symbol for a value it just wrote into a
 * Section. A real ELF symbol table isn't available here, so hand out a
 * monotonically increasing fake index -- enough for tests to assert "a
 * symbol was registered" and for downstream sym->c reads to see a
 * plausible, distinct value per call. */
static int cgb_next_sym_index = 1;

void put_extern_sym(Sym *sym, Section *section, addr_t value, unsigned long size)
{
  (void)section; (void)value; (void)size;
  if (sym)
    sym->c = cgb_next_sym_index++;
}

void cgb_reset(void)
{
  cgb_next_sym_index = 1;
}

/* From tccelf.c — real, verbatim (minus the ELF_OBJ_ONLY-gated
 * section_reserve sibling this harness never calls) bump allocator, so
 * arm-thumb-gen.c's direct section writes (literal pools, value tables) land
 * real, readable bytes a test can assert on. */
void section_realloc(Section *sec, unsigned long new_size)
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

void section_prealloc(Section *sec, unsigned long size)
{
  unsigned long needed = sec->data_offset + size;
  if (needed > sec->data_allocated)
    section_realloc(sec, needed);
}

/* From tccelf.c — real little-endian writers; ir/opt.c's block-copy
 * initializer patches rodata bytes through them, so faking would corrupt
 * the very bytes the tests assert on. */
void write32le(unsigned char *p, uint32_t x)
{
  p[0] = (unsigned char)(x & 0xff);
  p[1] = (unsigned char)((x >> 8) & 0xff);
  p[2] = (unsigned char)((x >> 16) & 0xff);
  p[3] = (unsigned char)((x >> 24) & 0xff);
}

void write64le(unsigned char *p, uint64_t x)
{
  write32le(p, (uint32_t)x);
  write32le(p + 4, (uint32_t)(x >> 32));
}

/* From tccopt.c (coverage-only, not linked here). arm-thumb-gen.c's
 * mach_ensure_in_reg/tcc_machine_addr_of_stack_slot call these
 * unconditionally as part of one shared switch-per-MachineOperandKind
 * function body (compiled once regardless of which case a given test
 * exercises at runtime) -- always-miss/no-op is correct for every mop test:
 * none of them relies on cross-call FP materialization caching. */
int tcc_opt_fp_mat_cache_lookup(TCCIRState *ir, int offset, int *phys_reg)
{
  (void)ir; (void)offset; (void)phys_reg;
  return 0;
}

void tcc_opt_fp_mat_cache_record(TCCIRState *ir, int offset, int phys_reg)
{
  (void)ir; (void)offset; (void)phys_reg;
}
