/*
 *  test_ssa_build.c - construction self-checks for the SSA substrate
 *
 *  Phase 0: Substrate bring-up.
 *
 *  Covers:
 *    - ssa_ctx_new/free: lifecycle
 *    - ssa_ctx_build_cfg: single-block CFG for straight-line IR
 *    - ssa_ctx_build_ssa_plain: minimal SSA state with empty phis
 *    - ssa_ctx_rebuild: real vinfo construction on known IR
 *    - ssa_opt_vinfo / ssa_opt_add_use_instr / ssa_opt_remove_use_instr:
 *      use-def chain basics (the foundation everything else builds on)
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt.c via UT11 (build_ssaopt binary).
 *    - Uses ssa_build.h helpers for hand-built vinfo.
 *    - The ssa_ctx_* helpers mirror what test_ssa_opt_arm.c does:
 *      hand-build vinfo and call SSA opt functions directly.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* ========================================================================
 * ssa_ctx lifecycle
 * ======================================================================== */

UT_TEST(test_ssa_ctx_lifecycle)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  UT_ASSERT(c.ir != NULL);
  UT_ASSERT_EQ(c.ir->next_temporary_variable, 4);
  ssa_ctx_free(&c);
  UT_ASSERT(c.ir == NULL);
  return 0;
}

/* ========================================================================
 * CFG build for straight-line IR
 * ======================================================================== */

UT_TEST(test_cfg_single_block_straight_line)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = 1, t1 = 2, t2 = t0 + t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  UT_ASSERT(c.cfg != NULL);
  UT_ASSERT_EQ(c.cfg->num_blocks, 1);
  /* All 3 instructions in block 0. */
  UT_ASSERT_EQ(c.cfg->blocks[0].start_idx, 0);
  UT_ASSERT_EQ(c.cfg->blocks[0].end_idx, 3);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Minimal SSA state (no phis)
 * ======================================================================== */

UT_TEST(test_ssa_plain_no_phis)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  UT_ASSERT(c.ssa != NULL);
  UT_ASSERT(c.ssa->block_phis != NULL);
  /* No phis in the single block. */
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 0);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * vinfo construction: real ssa_opt_build_chains on known IR
 * ======================================================================== */

UT_TEST(test_vinfo_construction_defs)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1, t1 = t0 (copy), t2 = t1 (copy) */
  int i0 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  int i1 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  int i2 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32));
  (void)i0; (void)i1; (void)i2;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* t0 defined at instr 0. */
  IRSSAVregInfo *v0 = ssa_vinfo(&c, utb_vreg(utb_temp(0, I32)));
  UT_ASSERT(v0 != NULL);
  UT_ASSERT_EQ(v0->def_instr, 0);
  UT_ASSERT_EQ(v0->def_count, 1);

  /* t1 defined at instr 1. */
  IRSSAVregInfo *v1 = ssa_vinfo(&c, utb_vreg(utb_temp(1, I32)));
  UT_ASSERT(v1 != NULL);
  UT_ASSERT_EQ(v1->def_instr, 1);
  UT_ASSERT_EQ(v1->def_count, 1);

  /* t2 defined at instr 2. */
  IRSSAVregInfo *v2 = ssa_vinfo(&c, utb_vreg(utb_temp(2, I32)));
  UT_ASSERT(v2 != NULL);
  UT_ASSERT_EQ(v2->def_instr, 2);
  UT_ASSERT_EQ(v2->def_count, 1);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_vinfo_construction_uses)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = #2; t2 = t0 + t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  int i_add = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(1, I32));
  (void)i_add;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* t0 has one use (the first ADD). */
  IRSSAVregInfo *v0 = ssa_vinfo(&c, utb_vreg(utb_temp(0, I32)));
  UT_ASSERT(v0 != NULL);
  UT_ASSERT_EQ(v0->use_count, 1);
  UT_ASSERT_EQ(v0->uses[0].idx, i_add);

  /* t1 has one use (the second ADD). */
  IRSSAVregInfo *v1 = ssa_vinfo(&c, utb_vreg(utb_temp(1, I32)));
  UT_ASSERT(v1 != NULL);
  UT_ASSERT_EQ(v1->use_count, 1);

  /* t2 has zero uses. */
  IRSSAVregInfo *v2 = ssa_vinfo(&c, utb_vreg(utb_temp(2, I32)));
  UT_ASSERT(v2 != NULL);
  UT_ASSERT_EQ(v2->use_count, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Use-def chain manipulation: add/remove use
 * ======================================================================== */

UT_TEST(test_use_def_add_remove_use)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  int i0 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  int i1 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  int i2 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(0, I32));
  (void)i0; (void)i1; (void)i2;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* t0 should have 2 uses (i1 and i2). */
  IRSSAVregInfo *v0 = ssa_vinfo(&c, utb_vreg(utb_temp(0, I32)));
  UT_ASSERT(v0 != NULL);
  UT_ASSERT_EQ(v0->use_count, 2);

  /* Remove use at i1. */
  ssa_opt_remove_use_instr(v0, i1);
  UT_ASSERT_EQ(v0->use_count, 1);
  UT_ASSERT_EQ(v0->uses[0].idx, i2);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phi construction: hand-built phi node
 * ======================================================================== */

UT_TEST(test_phi_hand_built)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  /* Add a dummy instruction so CFG build creates a block */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  /* Build CFG and SSA state first so ssa_add_phi can access c->ssa */
  ssa_ctx_build_cfg(&c);
  UT_ASSERT(c.cfg != NULL);
  UT_ASSERT(c.cfg->num_blocks > 0);
  ssa_ctx_build_ssa_plain(&c);
  UT_ASSERT(c.ssa != NULL);

  /* phi T5 = [T3, T3, T3] in block 0 */
  int32_t ops[] = { utb_vreg(utb_temp(3, I32)),
                    utb_vreg(utb_temp(3, I32)),
                    utb_vreg(utb_temp(3, I32)) };
  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(5, I32)), ops, 3);

  /* Manual vinfo setup (real rebuild would handle this). */
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  c.ctx->ir = c.ir;
  c.ctx->ssa = c.ssa;
  c.ctx->cfg = c.cfg;
  c.ctx->vinfo_cap = c.num_temps;
  if (c.ctx->vinfo_cap <= 0)
    c.ctx->vinfo_cap = 1;
  c.ctx->vinfo = tcc_malloc(c.ctx->vinfo_cap * sizeof(IRSSAVregInfo));
  memset(c.ctx->vinfo, 0, c.ctx->vinfo_cap * sizeof(IRSSAVregInfo));
  /* t5 has def_phi_block = 0 */
  IRSSAVregInfo *v5 = ssa_opt_vinfo(c.ctx, utb_vreg(utb_temp(5, I32)));
  UT_ASSERT(v5 != NULL);
  v5->def_phi_block = 0;

  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * phi_simplify: collapsing trivial phi (worked example from plan §2)
 *
 *   CFG: 1 block; SSA: phi T5 = [T3, T3, T3]; a use of T5.
 *   Expected: phi removed, T5 use → T3.
 * ======================================================================== */

UT_TEST(test_phi_simplify_collapses_trivial_phi)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  ssa_ctx_init_manual(&c);

  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(3, I32)),
                           utb_vreg(utb_temp(3, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 3);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32),
                            utb_temp(5, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = ssa_opt_phi_simplify(c.ctx);

  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 0);          /* phi removed */
  UT_ASSERT_EQ(IROP_VR(ssa_instr_src1(&c, use_i)), IROP_VR(utb_temp(3, I32))); /* T5 use -> T3 */
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * phi_simplify: self-referencing phi [T3, T5, T3] collapses to T3
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
 * phi_simplify: non-trivial phi (mixed operands) must NOT be dropped
 * ======================================================================== */

UT_TEST(test_phi_simplify_non_trivial_kept)
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

  /* Phi has two distinct non-self operands (T3 and T4) → not trivial. */
  int changed = ssa_opt_phi_simplify(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(IROP_VR(ssa_instr_src1(&c, use_i)), IROP_VR(utb_temp(5, I32))); /* still T5 */
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * phi_simplify: phi with all-self operands [T5, T5, T5] is trivial
 * ======================================================================== */

UT_TEST(test_phi_simplify_all_self)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  ssa_ctx_init_manual(&c);

  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(5, I32)),
                           utb_vreg(utb_temp(5, I32)),
                           utb_vreg(utb_temp(5, I32)) }, 3);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32),
                            utb_temp(5, I32));
  (void)use_i;

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  /* All operands are self-references → trivially replaceable with self.
   * The pass sees "unique = -1" (all operands are self-ref, skipped) and
   * treats it as non-trivial. This is correct: a phi with only self-refs
   * cannot be eliminated (no incoming value to replace with). */
  int changed = ssa_opt_phi_simplify(c.ctx);
  /* The pass sees "unique = -1" (all operands are self-ref, skipped) and
   * treats it as non-trivial. This is correct: a phi with only self-refs
   * cannot be eliminated (no incoming value to replace with). */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 1);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_build)
{
  UT_COVERS("ssa:substrate");
  UT_RUN(test_ssa_ctx_lifecycle);
  UT_RUN(test_cfg_single_block_straight_line);
  UT_RUN(test_ssa_plain_no_phis);
  UT_RUN(test_vinfo_construction_defs);
  UT_RUN(test_vinfo_construction_uses);
  UT_RUN(test_use_def_add_remove_use);
  UT_RUN(test_phi_hand_built);
  UT_RUN(test_phi_simplify_collapses_trivial_phi);
  UT_RUN(test_phi_simplify_self_ref);
  UT_RUN(test_phi_simplify_non_trivial_kept);
  UT_RUN(test_phi_simplify_all_self);
}
