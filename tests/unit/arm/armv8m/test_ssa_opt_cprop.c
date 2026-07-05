/*
 *  test_ssa_opt_cprop.c - SSA copy/constant propagation pass
 *
 *  Phase 4: Large pass.
 *
 *  Covers:
 *    - ssa_opt_cprop(): the full pass driver
 *    - ssa_gen_cprop_assign: TEMP ← TEMP copy forwarding
 *    - ssa_gen_cprop_copy_param: PARAM/VAR copy forwarding
 *    - ssa_gen_cprop_copy_var_stackoff: STACKOFF copy forwarding
 *    - ssa_gen_cprop_load_redundant: redundant LOAD elimination
 *    - ssa_gen_cprop_symref_cse: duplicate SYMREF materialization
 *    - ssa_opt_symref_operand_cse: SYMREF deref operand CSE
 *    - ssa_opt_var_const_fold: VAR self-update constant fold
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_cprop.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 *    - The cprop pass runs the generator table (cprop_gens) which dispatches
 *      to the right generator based on src tag, plus symref_operand_cse and
 *      var_const_fold as standalone sub-passes.
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
 * ssa_gen_cprop_assign: TEMP ← TEMP copy → replace uses
 *
 * t0 = #5; t1 = t0; use(t1) → use(t0)
 * ======================================================================== */

UT_TEST(test_cprop_assign_basic)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #5; t1 = t0; t2 = t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  int use_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                            utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  UT_ASSERT(changed >= 1);

  /* The use should now reference t0 directly. */
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, use_i)), IROP_VR(utb_temp(0, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_assign: different btypes → no copy (width conversion)
 *
 * t0 = #5 [INT32]; t1 = t0 [INT64] is a width conversion, not a pure copy.
 * ======================================================================== */

UT_TEST(test_cprop_assign_different_btype_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #5 [INT32]; t1 = t0 [INT64]; t2 = t1 + #1 [INT64] */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I64), utb_temp(0, I32));
  int add_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I64),
                            utb_temp(1, I64));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* Width mismatch → no copy forwarding. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, add_i)), IROP_VR(utb_temp(1, I64)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_assign: dest has multiple defs → no copy
 * ======================================================================== */

UT_TEST(test_cprop_assign_multi_def_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #1; t1 = t0; t1 = t0 (re-def); t2 = t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  int use_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                            utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* Multi-def dest → no copy. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, use_i)), IROP_VR(utb_temp(1, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_assign: dest feeds a phi → no copy (seed 2698)
 *
 * A copy feeding a phi operand resolves a loop back-edge value. Folding it
 * away would reintroduce the lost-copy problem at phi resolution.
 * ======================================================================== */

UT_TEST(test_cprop_assign_phi_dest_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/6);
  ssa_ctx_init_manual(&c);

  /* t0 = #5; t1 = t0 (copy); phi T2 = [T1, T1] */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(2, I32)),
              (int32_t[]){ utb_vreg(utb_temp(1, I32)),
                           utb_vreg(utb_temp(1, I32)) }, 2);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = ssa_opt_cprop(c.ctx);
  /* Copy feeding phi → no fold. */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_assign: lval src → no copy
 *
 * t1 = *t0 carries an lvalue source operand, not a register copy.
 * ======================================================================== */

UT_TEST(test_cprop_assign_lval_src_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t1 = *t0; t2 = t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                utb_lval(utb_temp(0, I32)));
  int use_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                            utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* lval src → no copy forwarding. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, use_i)), IROP_VR(utb_temp(1, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_param: PARAM copy forwarding
 *
 * t0 = P0 [LOAD]; t1 = t0; use(t1) → forward P0 into use.
 * ======================================================================== */

UT_TEST(test_cprop_copy_param_basic)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = P0 [LOAD]; t1 = t0; t2 = t1 + #1 */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_param(0, I32));
  int copy_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                             utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* Should forward P0 into the use, NOP the copy. */
  UT_ASSERT(changed >= 1);

  /* The copy should be NOP'd. */
  UT_ASSERT_EQ(utb_op(c.ir, copy_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_param: VAR copy forwarding
 *
 * t0 = V0 [LOAD]; t1 = t0; use(t1) → forward V0 into use.
 * ======================================================================== */

UT_TEST(test_cprop_copy_var_basic)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = V0 [LOAD]; t1 = t0; t2 = t1 + #1 */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_var(0, I32));
  int copy_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                             utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, copy_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_param: cross-block uses → no copy
 * ======================================================================== */

UT_TEST(test_cprop_copy_param_cross_block_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/5);
  ssa_ctx_init_manual(&c);

  /* Block 0: t1 = P0 [LOAD] */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                utb_param(0, I32));

  /* Block 1: t2 = t1 + #1 (use in different block) */
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32));
  ssa_ctx_manual_block_range(&c, /*block=*/0, /*start=*/0, /*end=*/1);
  ssa_ctx_manual_block_range(&c, /*block=*/1, /*start=*/1, /*end=*/2);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = ssa_opt_cprop(c.ctx);
  /* Cross-block use → no copy forwarding. */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_param: sub-word LOAD (INT8) → no copy (AAPCS promotion)
 *
 * A sub-word LOAD performs UXTB/SXTB sign/zero extension. Forwarding the
 * raw PARAM would skip that extension.
 * ======================================================================== */

UT_TEST(test_cprop_copy_param_subword_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = P0 [LOAD] (INT8); t1 = t0; t2 = t1 + #1 */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, IROP_BTYPE_INT8),
                utb_param(0, IROP_BTYPE_INT8));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                             utb_temp(0, IROP_BTYPE_INT8));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* Sub-word LOAD → no copy forwarding (would skip extension). */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_param: address-taken PARAM → no copy (combo 74935)
 *
 * If P0 is address-taken, a store through a pointer aliasing &P0 could
 * clobber P0's value between the copy and the use.
 * ======================================================================== */

UT_TEST(test_cprop_copy_param_addrtaken_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  c.ir->parameters_live_intervals_size = 1;
  c.ir->parameters_live_intervals = tcc_mallocz(sizeof(IRLiveInterval));
  c.ir->parameters_live_intervals[0].addrtaken = 1;

  /* t1 = P0 [LOAD]; t2 = &P0 [LEA]; *t2 = #5 [STORE]; t3 = t1 */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                utb_param(0, I32));
  IROperand stack = utb_stackoff(0, 0, 1, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(2, I32), stack);
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(2, I32)),
                utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* Address-taken PARAM with intervening store → no copy. */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_var_stackoff: STACKOFF copy forwarding
 *
 * t0 = V0 [ASSIGN via STACKOFF]; t1 = t0; use(t1) → forward V0 into use.
 * ======================================================================== */

UT_TEST(test_cprop_copy_var_stackoff_basic)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = V0 [ASSIGN]; t1 = t0; t2 = t1 + #1 */
  /* Encode V0 as STACKOFF: tag=STACKOFF, is_lval=1, is_local=1 */
  IROperand v0_stackoff =
      irop_make_stackoff(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 0), 0,
                         /*is_lval=*/1, /*is_llocal=*/0, /*is_param=*/0, I32);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), v0_stackoff);
  int copy_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                             utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* STACKOFF copy → forward V0 into use, NOP the copy. */
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, copy_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_load_redundant: redundant LOAD in same BB
 *
 * Two LOADs from the same vreg in the same block with no intervening write.
 * ======================================================================== */

UT_TEST(test_cprop_load_redundant_basic)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* r0 = #ptr; t0 = *r0 [LOAD]; t1 = t0; t2 = *r0 [LOAD] */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0x1000, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32));
  int load2_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(3, I32),
                              utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* Redundant LOAD → rewrite to ASSIGN from first LOAD's dest. */
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load2_i), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_load_redundant: intervening def → no redundant LOAD
 * ======================================================================== */

UT_TEST(test_cprop_load_redundant_intervening_def)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  /* r0 = #ptr; t0 = *r0; t1 = #5; t2 = *r0 (intervening def of r0) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0x1000, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0x2000, I32));
  int load2_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                              utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* Intervening def of source vreg → no redundant LOAD. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load2_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_load_redundant: cross-block → no redundant LOAD
 * ======================================================================== */

UT_TEST(test_cprop_load_redundant_cross_block)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/5);
  ssa_ctx_init_manual(&c);

  /* Block 0: r0 = #ptr; t0 = *r0 [LOAD] */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0x1000, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), utb_temp(0, I32));

  /* Block 1: t2 = *r0 [LOAD] (same source, different block) */
  int load2_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                              utb_temp(0, I32));
  ssa_ctx_manual_block_range(&c, /*block=*/0, /*start=*/0, /*end=*/2);
  ssa_ctx_manual_block_range(&c, /*block=*/1, /*start=*/2, /*end=*/3);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = ssa_opt_cprop(c.ctx);
  /* Cross-block → no redundant LOAD. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load2_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_load_redundant: llocal source → no redundant LOAD
 * ======================================================================== */

UT_TEST(test_cprop_load_redundant_llocal_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* r0 = &StackLoc[0] [LEA]; t0 = *r0 [LOAD]; t1 = *r0 [LOAD] */
  IROperand stack = utb_stackoff(0, 0, 1, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), stack);
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_lval(utb_temp(0, I32)),
                utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                              utb_lval(utb_temp(0, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* llocal source → no redundant LOAD. */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_const_fold: V = V + #imm where V = #const → fold
 *
 * V0 = #5; V0 = V0 + #3 → V0 = #8 (self-update constant fold)
 * ======================================================================== */

UT_TEST(test_var_const_fold_add)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* V0 = #5; V0 = V0 + #3 */
  IROperand v0 = utb_var(0, I32);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, v0, utb_imm(5, I32));
  int self_upd_i = ssa_add_instr3(&c, TCCIR_OP_ADD, v0, v0,
                                  utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_const_fold(c.ctx);
  UT_ASSERT(changed >= 1);

  /* The self-update should be folded to ASSIGN #8. */
  UT_ASSERT_EQ(utb_op(c.ir, self_upd_i), TCCIR_OP_ASSIGN);
  IROperand src1 = utb_src1(c.ir, self_upd_i);
  UT_ASSERT_EQ(src1.tag, IROP_TAG_IMM32);
  UT_ASSERT_EQ(src1.u.imm32, 8);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_const_fold: V = V & #imm where V = #const → fold
 * ======================================================================== */

UT_TEST(test_var_const_fold_and)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* V0 = #0xFF; V0 = V0 & 0xF */
  IROperand v0 = utb_var(0, I32);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, v0, utb_imm(0xFF, I32));
  int self_upd_i = ssa_add_instr3(&c, TCCIR_OP_AND, v0, v0,
                                  utb_imm(0xF, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_const_fold(c.ctx);
  UT_ASSERT(changed >= 1);

  /* The self-update should be folded to ASSIGN #0xF. */
  UT_ASSERT_EQ(utb_op(c.ir, self_upd_i), TCCIR_OP_ASSIGN);
  IROperand src1 = utb_src1(c.ir, self_upd_i);
  UT_ASSERT_EQ(src1.tag, IROP_TAG_IMM32);
  UT_ASSERT_EQ(src1.u.imm32, 0xF);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_const_fold: intervening use → don't NOP prior def
 *
 * V0 = #5; use(V0); V0 = V0 + #3 → fold but keep prior def alive.
 * ======================================================================== */

UT_TEST(test_var_const_fold_intervening_use)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* V0 = #5; t0 = V0; V0 = V0 + #3 */
  IROperand v0 = utb_var(0, I32);
  int store_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, v0, utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I32), v0);
  int self_upd_i = ssa_add_instr3(&c, TCCIR_OP_ADD, v0, v0,
                                  utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_const_fold(c.ctx);
  UT_ASSERT(changed >= 1);

  /* The self-update should be folded. */
  UT_ASSERT_EQ(utb_op(c.ir, self_upd_i), TCCIR_OP_ASSIGN);

  /* But the prior def should NOT be NOP'd (intervening use). */
  UT_ASSERT_NE(utb_op(c.ir, store_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_const_fold: no prior const → no fold
 * ======================================================================== */

UT_TEST(test_var_const_fold_no_prior_const)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* V0 = V1; V0 = V0 + #3 (V1 is not a constant) */
  IROperand v0 = utb_var(0, I32);
  IROperand v1 = utb_var(1, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, v0, v1);
  int self_upd_i = ssa_add_instr(&c, TCCIR_OP_ADD, v0, v0);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_const_fold(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, self_upd_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_const_fold: intervening store → no fold
 * ======================================================================== */

UT_TEST(test_var_const_fold_intervening_store)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* V0 = #5; *r0 = #99; V0 = V0 + #3 (store between def and self-update) */
  IROperand v0 = utb_var(0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, v0, utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)),
                utb_imm(99, I32));
  int self_upd_i = ssa_add_instr(&c, TCCIR_OP_ADD, v0, v0);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_const_fold(c.ctx);
  /* Intervening store → no fold. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, self_upd_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_symref_operand_cse: rewrite *sym to *Tn when Tn = sym exists
 * ======================================================================== */

UT_TEST(test_symref_operand_cse_basic)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  utb_pools_init(c.ir);
  static Sym g;
  memset(&g, 0, sizeof(g));
  g.v = 1;

  /* t0 = &g; t1 = *g [LOAD] */
  int addr_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                             utb_symref(c.ir, &g, 0, 0, 0, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_symref(c.ir, &g, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* The deref SYMREF should be replaced by a deref through t0. */
  UT_ASSERT(changed >= 1);
  IROperand src = utb_src1(c.ir, load_i);
  UT_ASSERT_EQ(irop_get_tag(src), IROP_TAG_VREG);
  UT_ASSERT_EQ(irop_get_vreg(src), irop_get_vreg(utb_dest(c.ir, addr_i)));
  UT_ASSERT(src.is_lval);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_symref_operand_cse: barrier between materialization and deref
 * ======================================================================== */

UT_TEST(test_symref_operand_cse_call_barrier_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  utb_pools_init(c.ir);
  static Sym g;
  memset(&g, 0, sizeof(g));
  g.v = 2;

  /* t0 = &g; call(); t1 = *g */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                utb_symref(c.ir, &g, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE);
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_symref(c.ir, &g, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(c.ir, load_i)), IROP_TAG_SYMREF);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_to_param_forward: single-def VAR value forwarded across blocks
 * ======================================================================== */

UT_TEST(test_var_to_param_forward_basic)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);
  c.ir->next_local_variable = 1;

  /* Block 0: V0 = #5; JUMP Block 1
   * Block 1: t0 = V0 [LOAD] -> t0 = #5 [ASSIGN] */
  IROperand v0 = utb_var(0, I32);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE, v0, utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), v0);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_to_param_forward(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  IROperand src = utb_src1(c.ir, load_i);
  UT_ASSERT_EQ(src.tag, IROP_TAG_IMM32);
  UT_ASSERT_EQ(src.u.imm32, 5);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_to_param_forward: address-taken VAR cannot be forwarded
 * ======================================================================== */

UT_TEST(test_var_to_param_forward_addrtaken_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  c.ir->next_local_variable = 1;

  /* V0 = #5; t0 = &V0; t1 = V0 [LOAD] */
  IROperand v0 = utb_var(0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, v0, utb_imm(5, I32));
  IROperand v0_addr = irop_make_stackoff(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 0),
                                          0, /*is_lval=*/0, /*is_llocal=*/0,
                                          /*is_param=*/0, I32);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), v0_addr);
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), v0);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_to_param_forward(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_to_param_forward: unsafe stored value (stack address) no fold
 * ======================================================================== */

UT_TEST(test_var_to_param_forward_stackaddr_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  c.ir->next_local_variable = 1;

  /* V0 = &loc; t0 = V0 [LOAD] */
  IROperand v0 = utb_var(0, I32);
  IROperand loc = irop_make_stackoff(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 1),
                                      0, /*is_lval=*/0, /*is_llocal=*/0,
                                      /*is_param=*/0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, v0, loc);
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), v0);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_to_param_forward(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_forward: single-def VAR value forwarded in same block
 * ======================================================================== */

UT_TEST(test_var_forward_basic)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  c.ir->next_local_variable = 1;

  /* V0 = #5 [STORE]; t0 = V0 [LOAD] -> t0 = #5 [ASSIGN] */
  IROperand v0 = utb_var(0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, v0, utb_imm(5, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), v0);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_forward(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  IROperand src = utb_src1(c.ir, load_i);
  UT_ASSERT_EQ(src.tag, IROP_TAG_IMM32);
  UT_ASSERT_EQ(src.u.imm32, 5);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_forward: call between def and use blocks forwarding
 * ======================================================================== */

UT_TEST(test_var_forward_call_barrier_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);
  c.ir->next_local_variable = 1;

  /* V0 = #5; JUMP; call(); t0 = V0 */
  IROperand v0 = utb_var(0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, v0, utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE);
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), v0);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_forward(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_param: src/dest btype mismatch -> no fold
 * ======================================================================== */

UT_TEST(test_cprop_copy_param_btype_mismatch_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = P0 [LOAD I32->I64]; t1 = t0 + #1 (use directly) */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I64),
                utb_param(0, I32));
  int use_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I64),
                            utb_temp(0, I64));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* LOAD btype mismatch -> P0 is not forwarded into t0's use. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, use_i)), IROP_VR(utb_temp(0, I64)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_param: src/dest unsigned mismatch -> no fold
 * ======================================================================== */

UT_TEST(test_cprop_copy_param_unsigned_mismatch_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = P0 [LOAD unsigned]; t1 = t0 + #1 (use directly) */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_unsigned(utb_param(0, I32)));
  int use_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                            utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* LOAD unsigned mismatch -> P0 is not forwarded into t0's use. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, use_i)), IROP_VR(utb_temp(0, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_var_stackoff: unsigned mismatch -> no fold
 * ======================================================================== */

UT_TEST(test_cprop_copy_var_stackoff_unsigned_mismatch_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  IROperand v0_stackoff =
      irop_make_stackoff(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 0), 0,
                         /*is_lval=*/1, /*is_llocal=*/0, /*is_param=*/0, I32);
  v0_stackoff.is_unsigned = 1;
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), v0_stackoff);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                            utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* STACKOFF unsigned mismatch -> V0 is not forwarded into t0's use. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, use_i)), IROP_VR(utb_temp(0, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_var_stackoff: btype mismatch (INT8 vs INT32) -> no fold
 * ======================================================================== */

UT_TEST(test_cprop_copy_var_stackoff_btype_mismatch_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  IROperand v0_stackoff =
      irop_make_stackoff(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 0), 0,
                         /*is_lval=*/1, /*is_llocal=*/0, /*is_param=*/0,
                         IROP_BTYPE_INT8);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), v0_stackoff);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                            utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  /* STACKOFF btype mismatch -> V0 is not forwarded into t0's use. */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, use_i)), IROP_VR(utb_temp(0, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_copy_var_stackoff: V holds stack addr + deref use -> no fold
 * ======================================================================== */

UT_TEST(test_cprop_copy_var_stackoff_deref_stackaddr_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  c.ir->next_local_variable = 2;

  /* V0 = &loc [STORE]; t0 = V0 [ASSIGN]; *t0 = #7 [STORE] */
  IROperand v0 = utb_var(0, I32);
  IROperand loc = irop_make_stackoff(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 1),
                                      0, /*is_lval=*/0, /*is_llocal=*/0,
                                      /*is_param=*/0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, v0, loc);
  int copy_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                             irop_make_stackoff(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 0),
                                                 0, /*is_lval=*/1, /*is_llocal=*/0,
                                                 /*is_param=*/0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)),
                utb_imm(7, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_NE(utb_op(c.ir, copy_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_load_redundant: call barrier between loads -> no fold
 * ======================================================================== */

UT_TEST(test_cprop_load_redundant_call_barrier_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* r0 = #ptr; t0 = *r0; call; t1 = *r0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0x1000, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE);
  int load2_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                              utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load2_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_gen_cprop_load_redundant: address-taken non-src def with lval src
 * ======================================================================== */

UT_TEST(test_cprop_load_redundant_addrtaken_def_lval_src_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  c.ir->parameters_live_intervals_size = 2;
  c.ir->parameters_live_intervals = tcc_mallocz(2 * sizeof(IRLiveInterval));
  c.ir->parameters_live_intervals[1].addrtaken = 1;

  /* t0 = *P0 [LOAD lval]; P1 = #5; t1 = *P0 [LOAD lval] */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                utb_lval(utb_param(0, I32)));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_param(1, I32), utb_imm(5, I32));
  int load2_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                              utb_lval(utb_param(0, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cprop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load2_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_var_const_fold: remaining operators
 * ======================================================================== */

static int var_const_fold_op(TccIrOp op, int32_t prior, int32_t imm,
                              int32_t expect)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  IROperand v0 = utb_var(0, I32);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, v0, utb_imm(prior, I32));
  int self_upd_i = ssa_add_instr3(&c, op, v0, v0, utb_imm(imm, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_var_const_fold(c.ctx);
  int ok = (changed >= 1 &&
            utb_op(c.ir, self_upd_i) == TCCIR_OP_ASSIGN &&
            utb_src1(c.ir, self_upd_i).tag == IROP_TAG_IMM32 &&
            utb_src1(c.ir, self_upd_i).u.imm32 == expect);
  ssa_ctx_free(&c);
  return ok;
}

UT_TEST(test_var_const_fold_sub)
{
  UT_ASSERT(var_const_fold_op(TCCIR_OP_SUB, 10, 3, 7));
  return 0;
}

UT_TEST(test_var_const_fold_mul)
{
  UT_ASSERT(var_const_fold_op(TCCIR_OP_MUL, 6, 7, 42));
  return 0;
}

UT_TEST(test_var_const_fold_or)
{
  UT_ASSERT(var_const_fold_op(TCCIR_OP_OR, 0xF0, 0x0F, 0xFF));
  return 0;
}

UT_TEST(test_var_const_fold_xor)
{
  UT_ASSERT(var_const_fold_op(TCCIR_OP_XOR, 0xFF, 0x0F, 0xF0));
  return 0;
}

UT_TEST(test_var_const_fold_shl)
{
  UT_ASSERT(var_const_fold_op(TCCIR_OP_SHL, 1, 4, 16));
  return 0;
}

UT_TEST(test_var_const_fold_shr)
{
  UT_ASSERT(var_const_fold_op(TCCIR_OP_SHR, 0x80000000U, 4, 0x08000000U));
  return 0;
}

UT_TEST(test_var_const_fold_sar)
{
  UT_ASSERT(var_const_fold_op(TCCIR_OP_SAR, 0x80000000U, 4, 0xF8000000U));
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_cprop)
{
  UT_COVERS("ssa:cprop");
  UT_RUN(test_cprop_assign_basic);
  UT_RUN(test_cprop_assign_different_btype_no_fold);
  UT_RUN(test_cprop_assign_multi_def_no_fold);
  UT_RUN(test_cprop_assign_phi_dest_no_fold);
  UT_RUN(test_cprop_assign_lval_src_no_fold);
  UT_RUN(test_cprop_copy_param_basic);
  UT_RUN(test_cprop_copy_var_basic);
  UT_RUN(test_cprop_copy_param_cross_block_no_fold);
  UT_RUN(test_cprop_copy_param_subword_no_fold);
  UT_RUN(test_cprop_copy_param_addrtaken_no_fold);
  UT_RUN(test_cprop_copy_var_stackoff_basic);
  UT_RUN(test_cprop_load_redundant_basic);
  UT_RUN(test_cprop_load_redundant_intervening_def);
  UT_RUN(test_cprop_load_redundant_cross_block);
  UT_RUN(test_cprop_load_redundant_llocal_no_fold);
  UT_RUN(test_var_const_fold_add);
  UT_RUN(test_var_const_fold_and);
  UT_RUN(test_var_const_fold_intervening_use);
  UT_RUN(test_var_const_fold_no_prior_const);
  UT_RUN(test_var_const_fold_intervening_store);
  UT_RUN(test_symref_operand_cse_basic);
  UT_RUN(test_symref_operand_cse_call_barrier_no_fold);
  UT_RUN(test_var_to_param_forward_basic);
  UT_RUN(test_var_to_param_forward_addrtaken_no_fold);
  UT_RUN(test_var_to_param_forward_stackaddr_no_fold);
  UT_RUN(test_var_forward_basic);
  UT_RUN(test_var_forward_call_barrier_no_fold);
  UT_RUN(test_cprop_copy_param_btype_mismatch_no_fold);
  UT_RUN(test_cprop_copy_param_unsigned_mismatch_no_fold);
  UT_RUN(test_cprop_copy_var_stackoff_unsigned_mismatch_no_fold);
  UT_RUN(test_cprop_copy_var_stackoff_btype_mismatch_no_fold);
  UT_RUN(test_cprop_copy_var_stackoff_deref_stackaddr_no_fold);
  UT_RUN(test_cprop_load_redundant_call_barrier_no_fold);
  UT_RUN(test_cprop_load_redundant_addrtaken_def_lval_src_no_fold);
  UT_RUN(test_var_const_fold_sub);
  UT_RUN(test_var_const_fold_mul);
  UT_RUN(test_var_const_fold_or);
  UT_RUN(test_var_const_fold_xor);
  UT_RUN(test_var_const_fold_shl);
  UT_RUN(test_var_const_fold_shr);
  UT_RUN(test_var_const_fold_sar);
}
