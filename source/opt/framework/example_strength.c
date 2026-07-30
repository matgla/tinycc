/*
 *  TCC IR — Optimization DSL: Example Strength Reduction Generator
 *
 *  Compile-checked smoke test for the DSL macros. NOT part of the main tcc build.
 *  Build with: make cross OPT_DSL_EXAMPLES=yes  (or: make opt-dsl-check)
 */

#define USING_GLOBALS
#include "ir.h"
#include "opt_dsl.h"

static int is_power_of_2(uint32_t v, int *shift)
{
  if (v == 0 || (v & (v - 1)) != 0)
    return 0;
  int s = 0;
  while ((v >> s) != 1)
    s++;
  *shift = s;
  return 1;
}

/* MUL(x, 2^n) -> SHL(x, n) */
OPT_GEN_SSA(dsl_strength_mul_power2, TCCIR_OP_MUL) {
  int shift = 0;
  PATTERN(
    .constraints = { .dest = IR_CONSTRAINT_VREG,
                     .src1 = IR_CONSTRAINT_VREG,
                     .src2 = IR_CONSTRAINT_IMM });
  GUARD(
    when(is_power_of_2((uint32_t)imm(src2), &shift)));
  REWRITE(
    .new_op = TCCIR_OP_SHL,
    .src2   = mk_imm(shift));
}

static const IRSSAOptGen dsl_strength_gens[] = {
  OPT_GEN_ENTRY(dsl_strength_mul_power2, TCCIR_OP_MUL),
};

static const int dsl_strength_gens_count =
  OPT_DSL_TABLE_COUNT(dsl_strength_gens);

#ifdef OPT_DSL_EXAMPLE_VERIFY
#include <stdio.h>

void dsl_strength_verify(void)
{
  printf("DSL strength reduction generators:\n");
  for (int i = 0; i < dsl_strength_gens_count; i++) {
    printf("  [%d] op=%d name=%s fn=%p\n",
           i,
           dsl_strength_gens[i].op,
           dsl_strength_gens[i].name,
           (void*)dsl_strength_gens[i].fn);
  }
}
#endif
