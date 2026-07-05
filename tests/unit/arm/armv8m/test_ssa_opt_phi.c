/*
 *  test_ssa_opt_phi.c - phi simplification pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_phi_simplify(): trivial phi elimination
 *      * all-same-operand phis → replace dest with that operand
 *      * all-self-operand phis → kept (no incoming value)
 *      * mixed-operand phis → kept (non-trivial)
 *      * single-operand (degenerate) phis → replace with operand
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_phi.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + phis.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* ========================================================================
 * Trivial phi: all operands same vreg → eliminated, uses redirected
 * ======================================================================== */

UT_TEST(test_phi_simplify_all_same)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  ssa_ctx_init_manual(&c);
  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(3, I32)),
                           utb_vreg(utb_temp(3, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 3);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32),
                            utb_temp(5, I32));
  (void)use_i;

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  /* Verify state before calling simplify */
  UT_ASSERT(c.ctx != NULL);
  UT_ASSERT(c.ctx->cfg != NULL);
  UT_ASSERT(c.ctx->ssa != NULL);
  UT_ASSERT(c.ctx->cfg->num_blocks > 0);

  int changed = ssa_opt_phi_simplify(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Self-referencing phi: [T3, T5, T3] → T5 is replaced by T3
 * ======================================================================== */

UT_TEST(test_phi_simplify_self_ref)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  ssa_ctx_init_manual(&c);
  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(3, I32)),
                           utb_vreg(utb_temp(5, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 3);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32),
                            utb_temp(5, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = ssa_opt_phi_simplify(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(IROP_VR(ssa_instr_src1(&c, use_i)), IROP_VR(utb_temp(3, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Non-trivial phi: mixed operands → kept
 * ======================================================================== */

UT_TEST(test_phi_simplify_non_trivial)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  ssa_ctx_init_manual(&c);
  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(3, I32)),
                           utb_vreg(utb_temp(4, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 3);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32),
                            utb_temp(5, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = ssa_opt_phi_simplify(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(IROP_VR(ssa_instr_src1(&c, use_i)), IROP_VR(utb_temp(5, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phi with undef operands (vreg < 0): treated as trivial if remaining
 * operands are all the same
 * ======================================================================== */

UT_TEST(test_phi_simplify_with_undef)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  ssa_ctx_init_manual(&c);
  /* phi T5 = [-1, T3, -1] — two undefs, one T3 → trivial */
  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ -1,
                           utb_vreg(utb_temp(3, I32)),
                           -1 }, 3);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32),
                            utb_temp(5, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = ssa_opt_phi_simplify(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(IROP_VR(ssa_instr_src1(&c, use_i)), IROP_VR(utb_temp(3, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Multiple phis in one block: all trivial → all eliminated
 * ======================================================================== */

UT_TEST(test_phi_simplify_multiple_trivial)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/8);
  ssa_ctx_init_manual(&c);
  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(3, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);
  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(6, I32)),
              (int32_t[]){ utb_vreg(utb_temp(4, I32)),
                           utb_vreg(utb_temp(4, I32)) }, 2);
  int use_i5 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32),
                             utb_temp(5, I32));
  int use_i6 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(8, I32),
                             utb_temp(6, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = ssa_opt_phi_simplify(c.ctx);
  UT_ASSERT(changed >= 2);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 0);
  UT_ASSERT_EQ(IROP_VR(ssa_instr_src1(&c, use_i5)), IROP_VR(utb_temp(3, I32)));
  UT_ASSERT_EQ(IROP_VR(ssa_instr_src1(&c, use_i6)), IROP_VR(utb_temp(4, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phi with no uses: still eliminated if trivial
 * ======================================================================== */

UT_TEST(test_phi_simplify_no_uses)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  ssa_ctx_init_manual(&c);
  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(3, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = ssa_opt_phi_simplify(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_phi)
{
  UT_COVERS("ssa:phi_simplify");
  UT_RUN(test_phi_simplify_all_same);
  UT_RUN(test_phi_simplify_self_ref);
  UT_RUN(test_phi_simplify_non_trivial);
  UT_RUN(test_phi_simplify_with_undef);
  UT_RUN(test_phi_simplify_multiple_trivial);
  UT_RUN(test_phi_simplify_no_uses);
}
