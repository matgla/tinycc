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
 *    - Links the real source/opt/ssa/cfg/phi.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + phis.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "opt/ssa/phi.h"

#define I32 IROP_BTYPE_INT32

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

UT_TEST(test_phi_simplify_two_phi_scc)
{
  ssa_ctx c = ssa_ctx_new(2, 10);
  ssa_ctx_init_manual(&c);
  int32_t x = utb_vreg(utb_temp(3, I32));
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));

  ssa_add_phi(&c, 0, a, (int32_t[]){ x, b }, 2);
  ssa_add_phi(&c, 1, b, (int32_t[]){ x, a }, 2);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32),
                            utb_temp(5, I32));
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 2);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 0);
  UT_ASSERT_EQ(irop_get_vreg(ssa_instr_src1(&c, use_i)), x);
  UT_ASSERT_EQ(ssa_vinfo(&c, a)->def_phi_block, -1);
  UT_ASSERT_EQ(ssa_vinfo(&c, b)->def_phi_block, -1);
  UT_ASSERT_EQ(ssa_vinfo(&c, a)->use_count, 0);
  UT_ASSERT_EQ(ssa_vinfo(&c, b)->use_count, 0);
  UT_ASSERT_EQ(ssa_vinfo(&c, x)->use_count, 1);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_phi_simplify_three_phi_scc_across_blocks)
{
  ssa_ctx c = ssa_ctx_new(3, 12);
  ssa_ctx_init_manual(&c);
  int32_t x = utb_vreg(utb_temp(3, I32));
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));
  int32_t d = utb_vreg(utb_temp(7, I32));

  ssa_add_phi(&c, 0, a, (int32_t[]){ x, b }, 2);
  ssa_add_phi(&c, 1, b, (int32_t[]){ x, d }, 2);
  ssa_add_phi(&c, 2, d, (int32_t[]){ x, a }, 2);
  int use_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(8, I32),
                             utb_temp(7, I32), utb_temp(5, I32));
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 3);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 2), 0);
  UT_ASSERT_EQ(irop_get_vreg(ssa_instr_src1(&c, use_i)), x);
  UT_ASSERT_EQ(irop_get_vreg(ssa_instr_src2(&c, use_i)), x);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_phi_simplify_scc_two_external_values_kept)
{
  ssa_ctx c = ssa_ctx_new(2, 10);
  ssa_ctx_init_manual(&c);
  int32_t x = utb_vreg(utb_temp(3, I32));
  int32_t y = utb_vreg(utb_temp(4, I32));
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));

  ssa_add_phi(&c, 0, a, (int32_t[]){ x, b }, 2);
  ssa_add_phi(&c, 1, b, (int32_t[]){ y, a }, 2);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 1);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_phi_simplify_scc_incompatible_types_kept)
{
  ssa_ctx c = ssa_ctx_new(2, 10);
  ssa_ctx_init_manual(&c);
  int32_t x = utb_vreg(utb_temp(3, I32));
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));

  ssa_add_phi(&c, 0, a, (int32_t[]){ x, b }, 2);
  ssa_add_phi(&c, 1, b, (int32_t[]){ x, a }, 2);
  c.ssa->block_phis[1]->btype = IROP_BTYPE_FLOAT32;
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 1);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_phi_simplify_scc_without_external_value_kept)
{
  ssa_ctx c = ssa_ctx_new(3, 10);
  ssa_ctx_init_manual(&c);
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));
  int32_t d = utb_vreg(utb_temp(7, I32));

  ssa_add_phi(&c, 0, a, (int32_t[]){ -1, b, d }, 3);
  ssa_add_phi(&c, 1, b, (int32_t[]){ a, b, d }, 3);
  ssa_add_phi(&c, 2, d, (int32_t[]){ a, b, d }, 3);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 2), 1);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_phi_simplify_scc_protected_use_is_atomic)
{
  ssa_ctx c = ssa_ctx_new(2, 10);
  ssa_ctx_init_manual(&c);
  int32_t x = utb_vreg(utb_temp(3, I32));
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));

  ssa_add_phi(&c, 0, a, (int32_t[]){ x, b }, 2);
  ssa_add_phi(&c, 1, b, (int32_t[]){ x, a }, 2);
  int use_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(7, I32),
                             utb_temp(3, I32), utb_temp(6, I32));
  ssa_ctx_rebuild(&c);

  int oi = c.ir->compact_instructions[use_i].orig_index;
  uint8_t *barrel_shifts = tcc_mallocz((size_t)(oi + 1));
  barrel_shifts[oi] = 1;
  c.ir->barrel_shifts = barrel_shifts;
  c.ir->barrel_shifts_len = oi + 1;

  int x_uses = ssa_vinfo(&c, x)->use_count;
  int a_uses = ssa_vinfo(&c, a)->use_count;
  int b_uses = ssa_vinfo(&c, b)->use_count;
  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 1);
  UT_ASSERT_EQ(irop_get_vreg(ssa_instr_src2(&c, use_i)), b);
  UT_ASSERT_EQ(ssa_vinfo(&c, x)->use_count, x_uses);
  UT_ASSERT_EQ(ssa_vinfo(&c, a)->use_count, a_uses);
  UT_ASSERT_EQ(ssa_vinfo(&c, b)->use_count, b_uses);

  c.ir->barrel_shifts = NULL;
  c.ir->barrel_shifts_len = 0;
  tcc_free(barrel_shifts);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Congruent phis: two phis with identical (pred_block -> vreg) maps merge
 * ======================================================================== */

UT_TEST(test_phi_simplify_congruent_exact_merge)
{
  ssa_ctx c = ssa_ctx_new(1, 12);
  ssa_ctx_init_manual(&c);
  int32_t x = utb_vreg(utb_temp(3, I32));
  int32_t y = utb_vreg(utb_temp(4, I32));
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));

  /* a added first, b second: b is the list head, so b is the representative. */
  ssa_add_phi(&c, 0, a, (int32_t[]){ x, y }, 2);
  ssa_add_phi(&c, 0, b, (int32_t[]){ x, y }, 2);
  int use_a = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32),
                            utb_temp(5, I32));
  int use_b = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(8, I32),
                            utb_temp(6, I32));
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 1);
  UT_ASSERT_EQ(irop_get_vreg(ssa_instr_src1(&c, use_a)), b);
  UT_ASSERT_EQ(irop_get_vreg(ssa_instr_src1(&c, use_b)), b);
  UT_ASSERT_EQ(ssa_vinfo(&c, a)->def_phi_block, -1);
  UT_ASSERT_EQ(ssa_vinfo(&c, a)->use_count, 0);
  UT_ASSERT_EQ(ssa_vinfo(&c, b)->use_count, 2);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Different value on a shared edge → not congruent, kept
 * ======================================================================== */

UT_TEST(test_phi_simplify_congruent_diff_value_kept)
{
  ssa_ctx c = ssa_ctx_new(1, 12);
  ssa_ctx_init_manual(&c);
  int32_t x = utb_vreg(utb_temp(3, I32));
  int32_t y = utb_vreg(utb_temp(4, I32));
  int32_t z = utb_vreg(utb_temp(9, I32));
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));

  ssa_add_phi(&c, 0, a, (int32_t[]){ x, y }, 2);
  ssa_add_phi(&c, 0, b, (int32_t[]){ x, z }, 2);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 2);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Same operand set, swapped predecessor mapping → not congruent, kept
 * ======================================================================== */

UT_TEST(test_phi_simplify_congruent_swapped_preds_kept)
{
  ssa_ctx c = ssa_ctx_new(1, 12);
  ssa_ctx_init_manual(&c);
  int32_t x = utb_vreg(utb_temp(3, I32));
  int32_t y = utb_vreg(utb_temp(4, I32));
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));

  ssa_add_phi(&c, 0, a, (int32_t[]){ x, y }, 2);
  ssa_add_phi(&c, 0, b, (int32_t[]){ y, x }, 2);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 2);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Type mismatch → not congruent, kept
 * ======================================================================== */

UT_TEST(test_phi_simplify_congruent_type_mismatch_kept)
{
  ssa_ctx c = ssa_ctx_new(1, 12);
  ssa_ctx_init_manual(&c);
  int32_t x = utb_vreg(utb_temp(3, I32));
  int32_t y = utb_vreg(utb_temp(4, I32));
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));

  ssa_add_phi(&c, 0, a, (int32_t[]){ x, y }, 2);
  ssa_add_phi(&c, 0, b, (int32_t[]){ x, y }, 2);
  c.ssa->block_phis[0]->btype = IROP_BTYPE_FLOAT32;
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 2);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Protected use on the duplicate → merge skipped, both kept
 * ======================================================================== */

UT_TEST(test_phi_simplify_congruent_protected_use_kept)
{
  ssa_ctx c = ssa_ctx_new(1, 12);
  ssa_ctx_init_manual(&c);
  int32_t x = utb_vreg(utb_temp(3, I32));
  int32_t y = utb_vreg(utb_temp(4, I32));
  int32_t a = utb_vreg(utb_temp(5, I32));
  int32_t b = utb_vreg(utb_temp(6, I32));

  ssa_add_phi(&c, 0, a, (int32_t[]){ x, y }, 2);
  ssa_add_phi(&c, 0, b, (int32_t[]){ x, y }, 2);
  int use_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(7, I32),
                             utb_temp(3, I32), utb_temp(5, I32));
  ssa_ctx_rebuild(&c);

  int oi = c.ir->compact_instructions[use_i].orig_index;
  uint8_t *barrel_shifts = tcc_mallocz((size_t)(oi + 1));
  barrel_shifts[oi] = 1;
  c.ir->barrel_shifts = barrel_shifts;
  c.ir->barrel_shifts_len = oi + 1;

  UT_ASSERT_EQ(ssa_opt_phi_simplify(c.ctx), 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 2);
  UT_ASSERT_EQ(irop_get_vreg(ssa_instr_src2(&c, use_i)), a);

  c.ir->barrel_shifts = NULL;
  c.ir->barrel_shifts_len = 0;
  tcc_free(barrel_shifts);
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
  UT_RUN(test_phi_simplify_two_phi_scc);
  UT_RUN(test_phi_simplify_three_phi_scc_across_blocks);
  UT_RUN(test_phi_simplify_scc_two_external_values_kept);
  UT_RUN(test_phi_simplify_scc_incompatible_types_kept);
  UT_RUN(test_phi_simplify_scc_without_external_value_kept);
  UT_RUN(test_phi_simplify_scc_protected_use_is_atomic);
  UT_RUN(test_phi_simplify_congruent_exact_merge);
  UT_RUN(test_phi_simplify_congruent_diff_value_kept);
  UT_RUN(test_phi_simplify_congruent_swapped_preds_kept);
  UT_RUN(test_phi_simplify_congruent_type_mismatch_kept);
  UT_RUN(test_phi_simplify_congruent_protected_use_kept);
}
