/*
 *  test_opt_promote_extra.c - suite for the remaining ir/opt_promote.c entry
 *  points not covered by test_opt_branch_cascade.c (var_tmp_fwd) or
 *  test_opt_var_to_tmp.c (var_to_tmp):
 *
 *    tcc_ir_opt_redundant_loop_check    - loop-body CMP redundant with a
 *                                          header guard CMP folds to an
 *                                          unconditional jump / dead branch.
 *    tcc_ir_opt_setif_neg_to_select     - `0 - (cond?1:0)` -> SELECT(-1,0,cond)
 *    tcc_ir_opt_select                  - if/else diamond -> SELECT (four
 *                                          sub-patterns: RETURNVALUE diamond,
 *                                          ASSIGN diamond, call diamond,
 *                                          SETIF+ASSIGN(0) collapse) plus the
 *                                          `JUMPIF C->A; JUMP->B` fallthrough
 *                                          normalization that feeds them.
 *    tcc_ir_opt_returnvalue_merge       - later RETURNVALUE #same_const sites
 *                                          become JUMPs to the first site.
 *    tcc_ir_opt_backedge_phi_hoist      - CMP+JUMPIF+phi-ASSIGNs+backward JUMP
 *                                          inverts to hoist the phi copies
 *                                          before the (now-inverted) guard.
 *    tcc_ir_opt_post_ra_forward_diamond - post-RA: a forward diamond whose
 *                                          "then" leg is only coalesced no-op
 *                                          ASSIGNs (dest reg == src reg)
 *                                          collapses to an inverted JUMPIF.
 *    tcc_ir_opt_abort_tail_merge        - post-RA: multiple `guard; call
 *                                          noreturn` sites for the same
 *                                          callee tail-merge to one shared
 *                                          sink.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 *  Oracle asserts (exact opcodes / operand values / instruction counts), not
 *  characterization.
 *
 *  backedge_phi_hoist / post_ra_forward_diamond additionally read register-
 *  allocation state (ir->ls.intervals, a LSLiveIntervalState) to decide
 *  whether a phi-copy ASSIGN is "safe" (both sides already share a physical
 *  register / neither is spilled).  This file hand-populates that array via
 *  tcc_ls_initialize()+tcc_ls_add_live_interval() -- see
 *  utb_ls_new()/utb_ls_free() below -- following the same "build only the
 *  state fields the pass under test reads" discipline as ir_build.h.
 */

#include "ir_build.h"

#include "ut.h"

#include "tccls.h"

/* Pass entry points (defined in ir/opt_promote.c; forward-declared here to
 * avoid pulling in the optimizer engine headers). */
int tcc_ir_opt_redundant_loop_check(TCCIRState *ir);
int tcc_ir_opt_setif_neg_to_select(TCCIRState *ir);
int tcc_ir_opt_select(TCCIRState *ir);
int tcc_ir_opt_returnvalue_merge(TCCIRState *ir);
int tcc_ir_opt_backedge_phi_hoist(TCCIRState *ir);
int tcc_ir_opt_post_ra_forward_diamond(TCCIRState *ir);
int tcc_ir_opt_abort_tail_merge(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64
#define F64 IROP_BTYPE_FLOAT64

#define TOK_ULT 0x92
#define TOK_UGE 0x93
#define TOK_EQ  0x94
#define TOK_NE  0x95
#define TOK_ULE 0x96
#define TOK_UGT 0x97
#define TOK_LT  0x9c
#define TOK_GE  0x9d
#define TOK_LE  0x9e
#define TOK_GT  0x9f

/* ------------------------------------------------------------------ helpers */

/* utb_new() leaves iroperand_pool_capacity at 0; tcc_ir_pool_add's growth
 * (`capacity *= 2`) never advances from 0. select / setif_neg_to_select both
 * grow the pool for the SELECT operand quad, so give them a real capacity
 * (same pattern as test_opt_branch_cascade.c's utb_pool_new() /
 * test_opt_licm.c's utb_loop_new()). */
static TCCIRState *utb_pool_new(void)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  return ir;
}

/* A JUMP/JUMPIF target operand the way these passes decode it via
 * irop_get_imm32()/irop_get_imm64_ex(): irop_make_imm32(-1, target, I32). */
static IROperand utb_jtarget(int target)
{
  return irop_make_imm32(-1, target, I32);
}

/* backedge_phi_hoist / post_ra_forward_diamond read ir->ls (a
 * LSLiveIntervalState) to check whether phi-copy ASSIGN operands are already
 * co-located in the same physical register and unspilled.  Initialize it via
 * the real tccls.c allocator entry points (not a hand-rolled struct) so the
 * layout always matches production. */
static void utb_ls_new(TCCIRState *ir)
{
  tcc_ls_initialize(&ir->ls);
}

static void utb_ls_free(TCCIRState *ir)
{
  tcc_ls_deinitialize(&ir->ls);
}

/* Register vreg `vr` as live in physical register `preg` (r1 defaults to -1,
 * i.e. a plain 32-bit value, not a 64-bit pair), unspilled. */
static void utb_ls_reg(TCCIRState *ir, int32_t vr, int preg)
{
  tcc_ls_add_live_interval(&ir->ls, vr, 0, 1000, /*crosses_call*/ 0, /*addrtaken*/ 0,
                           LS_REG_TYPE_INT, /*lvalue*/ 0, preg);
}

/* Register vreg `vr` as spilled (stack_location != 0, no physical register). */
static void utb_ls_spill(TCCIRState *ir, int32_t vr)
{
  tcc_ls_add_live_interval(&ir->ls, vr, 0, 1000, 0, 0, LS_REG_TYPE_INT, 0, -1);
  ir->ls.intervals[ir->ls.next_interval_index - 1].stack_location = 8;
}

/* var_to_tmp-style TEMP live-interval table (post_ra_forward_diamond's
 * phi_pinned guard calls tcc_ir_vreg_live_interval(), which exit(1)s on an
 * out-of-bounds TEMP position). */
static void utb_alloc_temp_intervals(TCCIRState *ir, int count)
{
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->temporary_variables_live_intervals_size = count;
}

/* A SYMREF callee operand whose token is `tok` (utb_set_tok_str maps `tok` to
 * a name for get_tok_str()-gated logic; here only tcc_ir_callee_is_noreturn's
 * name fallback in opt_dce.c, used by abort_tail_merge). */
static IROperand utb_callee_named(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* ================================================================== redundant_loop_check */

/* POSITIVE: a header guard `CMP V,#100; JUMPIF GE -> exit` establishes
 * "inside the loop body, V < 100" (guard_body_fact = negate(GE) = LT).  A
 * body CMP against the SAME constant with the SAME implied condition (LT)
 * is therefore always true -- its CMP+JUMPIF collapses to an unconditional
 * JUMP to the inner JUMPIF's own target.
 *
 *   0: T0 = #10                 preheader
 *   1: CMP T0, #100              header guard CMP
 *   2: JUMPIF GE -> 8            exit (outside [1,6]) => guard_body_fact=LT
 *   3: CMP T0, #100              body CMP, same const
 *   4: JUMPIF LT -> 7            inner cond LT: guard(LT) implies LT => fold
 *   5: T0 = T0 + #1
 *   6: JUMPIF NE -> 1            back-edge (target 1 < i=6) => loop [1,6]
 *   7: RETURNVOID
 *   8: RETURNVOID
 */
UT_TEST(test_redundant_loop_check_body_cmp_implied_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32), UTB_NONE);      /* 0 */
  int cmp_hdr = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(100, I32)); /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(8), utb_imm(TOK_GE, I32), UTB_NONE);    /* 2 */
  int cmp_body = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(100, I32)); /* 3 */
  int jif_body = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(7), utb_imm(TOK_LT, I32), UTB_NONE); /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(1, I32));  /* 5 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_imm(TOK_NE, I32), UTB_NONE);    /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 8 */

  int changes = tcc_ir_opt_redundant_loop_check(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, cmp_hdr), TCCIR_OP_CMP);   /* header guard CMP survives */
  UT_ASSERT_EQ(utb_op(ir, cmp_body), TCCIR_OP_NOP);  /* redundant body CMP removed */
  UT_ASSERT_EQ(utb_op(ir, jif_body), TCCIR_OP_JUMP); /* JUMPIF -> unconditional JUMP */
  IROperand jd = utb_dest(ir, jif_body);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, jd), 7);   /* target preserved */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the body CMP's condition (UGT, an UNSIGNED compare) is
 * NOT implied by the header guard fact (LT, a signed compare) nor is its
 * negation (ULE) implied -- vrp_cmp_implies() never crosses the signed/
 * unsigned families, so CMP+JUMPIF must survive untouched.  (A same-family
 * choice like GT is the wrong negative case here: negate(GT)==LE, and LE
 * IS implied by LT, so it would wrongly hit the "both NOP'd" fold path --
 * see test_redundant_loop_check_negated_cond_both_nopped below for that.) */
UT_TEST(test_redundant_loop_check_unrelated_cond_kept)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32), UTB_NONE);      /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(100, I32));        /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(8), utb_imm(TOK_GE, I32), UTB_NONE);    /* 2 */
  int cmp_body = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(100, I32)); /* 3 */
  int jif_body = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(7), utb_imm(TOK_UGT, I32), UTB_NONE); /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(1, I32));  /* 5 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_imm(TOK_NE, I32), UTB_NONE);    /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 8 */

  int changes = tcc_ir_opt_redundant_loop_check(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, cmp_body), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, jif_body), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* POSITIVE: the body CMP's condition (GE) is the NEGATION of the guard fact
 * (LT) -- inside the loop LT always holds, so GE is always false: both the
 * body CMP and its JUMPIF are dead (NOP'd), not converted to a jump. */
UT_TEST(test_redundant_loop_check_negated_cond_both_nopped)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32), UTB_NONE);      /* 0 */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(100, I32));        /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(8), utb_imm(TOK_GE, I32), UTB_NONE);    /* 2 */
  int cmp_body = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(100, I32)); /* 3 */
  int jif_body = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(7), utb_imm(TOK_GE, I32), UTB_NONE); /* 4 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(1, I32));  /* 5 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(1), utb_imm(TOK_NE, I32), UTB_NONE);    /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 7 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                  /* 8 */

  int changes = tcc_ir_opt_redundant_loop_check(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, cmp_body), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jif_body), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* ================================================================== setif_neg_to_select */

/* POSITIVE: CMP + SETIF(EQ) [single use] + `T3 = #0 - T2` collapses to
 * CMP + SELECT(#-1, #0, EQ); the SETIF is NOP'd. */
UT_TEST(test_setif_neg_to_select_folds)
{
  TCCIRState *ir = utb_pool_new();

  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int setif = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  int sub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(3, I32), utb_imm(0, I32), utb_temp(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = tcc_ir_opt_setif_neg_to_select(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, setif), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, sub), TCCIR_OP_SELECT);

  /* SELECT operand quad: dest = old SUB dest (T3), src1 = #-1, src2 = #0,
   * op4 = cond (EQ, the SETIF's own condition). */
  IROperand sel_dest = utb_dest(ir, sub);
  UT_ASSERT_EQ(utb_vreg_pos(sel_dest), 3);
  IROperand sel_then = utb_src1(ir, sub);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, sel_then), -1);
  IROperand sel_else = utb_src2(ir, sub);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, sel_else), 0);
  IROperand sel_cond = utb_op4(ir, sub);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, sel_cond), TOK_EQ);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the SETIF result (T2) is read a second time, so it is not
 * single-use -- NOPing it would be unsound; the pattern must not fold. */
UT_TEST(test_setif_neg_to_select_multi_use_kept)
{
  TCCIRState *ir = utb_pool_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int setif = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(2, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  int sub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(3, I32), utb_imm(0, I32), utb_temp(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE); /* 2nd use of T2 */

  int changes = tcc_ir_opt_setif_neg_to_select(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, setif), TCCIR_OP_SETIF);
  UT_ASSERT_EQ(utb_op(ir, sub), TCCIR_OP_SUB);

  utb_free(ir);
  return 0;
}

/* ================================================================== select (RETURNVALUE diamond) */

/* POSITIVE: `CMP; JUMPIF EQ->else; RETURNVALUE #10; RETURNVALUE #20` (else
 * immediately follows the then-RETURNVALUE) collapses to a single SELECT
 * feeding one RETURNVALUE. */
UT_TEST(test_select_returnvalue_diamond_collapses)
{
  TCCIRState *ir = utb_pool_new();
  /* the RETURNVALUE-diamond fold allocates a fresh TEMP (tcc_ir_get_vreg_temp)
   * for the SELECT result -- give it a real interval table. */
  utb_alloc_temp_intervals(ir, 16);

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32)); /* 0 */
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(3), utb_imm(TOK_EQ, I32), UTB_NONE); /* 1 */
  int then_ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(10, I32), UTB_NONE); /* 2 */
  int else_ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(20, I32), UTB_NONE); /* 3 */

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_SELECT);
  UT_ASSERT_EQ(utb_op(ir, else_ret), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, then_ret), TCCIR_OP_RETURNVALUE);

  /* the SELECT's src1/src2 are the original then/else constants; its cond is
   * then_cond = negate(branch_cond EQ) = NE (fall-through/then runs when the
   * branch does NOT take, i.e. when NOT EQ). */
  IROperand sel = utb_dest(ir, jumpif);
  int32_t sel_vr = irop_get_vreg(sel);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, jumpif)), 10);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, jumpif)), 20);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, jumpif)), TOK_NE);

  /* the then RETURNVALUE now reads the SELECT's result vreg. */
  IROperand ret_src = utb_src1(ir, then_ret);
  UT_ASSERT_EQ(irop_get_vreg(ret_src), sel_vr);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the "else" RETURNVALUE's value is a vreg, not a
 * compile-time constant -- SELECT construction requires both arms to be
 * IMM32/SYMREF (removing the branches would disrupt the vreg's liveness), so
 * the diamond is left untouched. */
UT_TEST(test_select_returnvalue_diamond_vreg_arm_kept)
{
  TCCIRState *ir = utb_pool_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(3), utb_imm(TOK_EQ, I32), UTB_NONE);
  int then_ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(10, I32), UTB_NONE);
  int else_ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, then_ret), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_op(ir, else_ret), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  return 0;
}

/* ================================================================== select (ASSIGN diamond) */

/* POSITIVE: `CMP; JUMPIF EQ->else; V<-#1[ASSIGN]; JUMP->merge; V<-#2[ASSIGN]`
 * (else immediately falls to merge) collapses to a single SELECT ASSIGN. */
UT_TEST(test_select_assign_diamond_collapses)
{
  TCCIRState *ir = utb_pool_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));             /* 0 */
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(4), utb_imm(TOK_EQ, I32), UTB_NONE); /* 1: else_target=4 */
  int then_asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(1, I32), UTB_NONE); /* 2 then */
  int jump = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(5), UTB_NONE, UTB_NONE);            /* 3: -> merge(5) */
  int else_asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(2, I32), UTB_NONE); /* 4 else */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_lval(utb_temp(5, I32)), UTB_NONE);    /* 5 merge */

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_SELECT);
  UT_ASSERT_EQ(utb_op(ir, then_asg), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jump), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, else_asg), TCCIR_OP_NOP);

  /* SELECT dest is V5 (the shared ASSIGN target); src1/src2 are #1/#2; cond
   * is then_cond = negate(EQ) = NE. */
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, jumpif)), 5);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, jumpif)), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, jumpif)), 2);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, jumpif)), TOK_NE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the else block has an extra instruction between its
 * ASSIGN and the merge point -- NOPing the else ASSIGN would silently drop
 * that extra instruction's predecessor, so the diamond must be left alone. */
UT_TEST(test_select_assign_diamond_extra_else_instr_kept)
{
  TCCIRState *ir = utb_pool_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));             /* 0 */
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(4), utb_imm(TOK_EQ, I32), UTB_NONE); /* 1 */
  int then_asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(1, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(6), UTB_NONE, UTB_NONE);                      /* 3 -> merge(6) */
  int else_asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(2, I32), UTB_NONE); /* 4 else */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(6, I32), utb_temp(6, I32), utb_imm(1, I32));      /* 5 extra else instr */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(5, I32), UTB_NONE);             /* 6 merge */

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, then_asg), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, else_asg), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* ================================================================== select (LOAD diamond, variable arm) */

/* POSITIVE: a LOAD diamond whose arms are a plain non-lvalue register value and
 * an immediate -- `T5<-T6[LOAD]` / `T5<-#2[LOAD]`, same dest -- collapses to a
 * SELECT.  This is the `x>c ? x : k` shape (one arm is a variable, not const),
 * enabled by the ir_ifconv_arm_value_safe broadening. */
UT_TEST(test_select_load_diamond_variable_arm_collapses)
{
  TCCIRState *ir = utb_pool_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));             /* 0 */
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(4), utb_imm(TOK_EQ, I32), UTB_NONE); /* 1: else_target=4 */
  int then_ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(5, I32), utb_temp(6, I32), UTB_NONE); /* 2 then: T5<-T6 (reg) */
  int jump = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(5), UTB_NONE, UTB_NONE);            /* 3: -> merge(5) */
  int else_ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(5, I32), utb_imm(2, I32), UTB_NONE); /* 4 else: T5<-#2 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_lval(utb_temp(5, I32)), UTB_NONE);    /* 5 merge */

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_SELECT);
  UT_ASSERT_EQ(utb_op(ir, then_ld), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jump), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, else_ld), TCCIR_OP_NOP);

  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, jumpif)), 5);
  UT_ASSERT_EQ(irop_get_vreg(utb_src1(ir, jumpif)), irop_get_vreg(utb_temp(6, I32))); /* then val = T6 */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, jumpif)), 2);                  /* else val = #2 */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, jumpif)), TOK_NE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a LOAD arm that dereferences an lvalue (a pointer/global
 * memory read) may fault on the not-taken path once hoisted into an
 * unconditional SELECT, so ir_ifconv_arm_value_safe rejects it -- the diamond
 * is left as branches. */
UT_TEST(test_select_load_diamond_deref_arm_kept)
{
  TCCIRState *ir = utb_pool_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));             /* 0 */
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(4), utb_imm(TOK_EQ, I32), UTB_NONE); /* 1 */
  int then_ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(5, I32), utb_temp(6, I32), UTB_NONE); /* 2 then: T5<-T6 (reg) */
  int jump = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(5), UTB_NONE, UTB_NONE);            /* 3 */
  int else_ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(5, I32), utb_lval(utb_temp(7, I32)), UTB_NONE); /* 4 else: T5<-*T7 (deref) */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_lval(utb_temp(5, I32)), UTB_NONE);    /* 5 merge */

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, then_ld), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_op(ir, else_ld), TCCIR_OP_LOAD);
  (void)jump;

  utb_free(ir);
  return 0;
}

/* ============================================ select (Stage 2: computed arm + merge temp) */

/* POSITIVE (Stage 2 target -- the `x<0?-x:x` / abs2 shape).  A diamond whose
 * then-arm computes a value into its own temp and then merges it into the
 * result var, while the else-arm writes the result var directly:
 *
 *   0: CMP T9,#0
 *   1: JUMPIF GE -> 4          (else_target=4; then runs when !GE == LT)
 *   2: T0 <- #0 SUB T9         (then: compute -x, a pure ALU op)
 *   3: JUMP -> 6
 *   4: T1 <- T9  [LOAD]        (else: result var = x, written directly)
 *   5: JUMP -> 7
 *   6: T1 <- T0  [ASSIGN]      (then-side merge-assign: result var = -x)
 *   7: RETURNVALUE T1          (merge)
 *
 * should collapse to `T1 <- SELECT(T0, T9, LT)` with the pure compute (SUB)
 * kept and the branches / merge-assign / else-load NOP'd.  Currently
 * tcc_ir_opt_select leaves it as a jump diamond -- this test reproduces the
 * gap and pins the Stage 2 behavior. */
UT_TEST(test_select_computed_arm_merge_temp_diamond_collapses)
{
  TCCIRState *ir = utb_pool_new();
  utb_alloc_temp_intervals(ir, 16);

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(9, I32), utb_imm(0, I32));               /* 0 */
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(4), utb_imm(TOK_GE, I32), UTB_NONE); /* 1: else=4 */
  int sub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(0, I32), utb_imm(0, I32), utb_temp(9, I32));   /* 2: T0=-x */
  int jmp_then = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(6), UTB_NONE, UTB_NONE);         /* 3 */
  int else_ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_temp(9, I32), UTB_NONE);/* 4: T1=x */
  int jmp_else = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(7), UTB_NONE, UTB_NONE);         /* 5 */
  int merge_asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE); /* 6: T1=T0 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_lval(utb_temp(1, I32)), UTB_NONE);     /* 7 merge */

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT(changes >= 1);
  /* SELECT lands on the merge-assign slot; the pure compute stays unconditional. */
  UT_ASSERT_EQ(utb_op(ir, merge_asg), TCCIR_OP_SELECT);
  UT_ASSERT_EQ(utb_op(ir, sub), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jmp_then), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, else_ld), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jmp_else), TCCIR_OP_NOP);

  /* SELECT: dest=T1, src1=then value (T0=-x), src2=else value (T9=x), cond=LT. */
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, merge_asg)), 1);
  UT_ASSERT_EQ(irop_get_vreg(utb_src1(ir, merge_asg)), irop_get_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(irop_get_vreg(utb_src2(ir, merge_asg)), irop_get_vreg(utb_temp(9, I32)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, merge_asg)), TOK_LT);

  utb_free(ir);
  return 0;
}

/* ================================================================== select (SETIF+ASSIGN(0) collapse) */

/* POSITIVE: `then: T=SETIF(NE) [setif_tok==then_cond]; JUMP->merge; else:
 * T=#0[ASSIGN]` collapses to a bare SETIF -- the JUMPIF/JUMP/else-ASSIGN are
 * all NOP'd, SETIF is untouched (it already produces the diamond's result). */
UT_TEST(test_select_setif_zero_diamond_collapses_to_bare_setif)
{
  TCCIRState *ir = utb_pool_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));           /* 0 */
  /* branch_cond = EQ -> then_cond = negate(EQ) = NE; SETIF must carry NE to match. */
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(4), utb_imm(TOK_EQ, I32), UTB_NONE); /* 1: else_target=4 */
  int setif = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(5, I32), utb_imm(TOK_NE, I32), UTB_NONE); /* 2 then */
  int jump = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(5), UTB_NONE, UTB_NONE);          /* 3 -> merge(5) */
  int else_asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(0, I32), UTB_NONE); /* 4 else */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(5, I32), UTB_NONE);            /* 5 merge */

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, setif), TCCIR_OP_SETIF); /* untouched -- IS the result */
  UT_ASSERT_EQ(utb_op(ir, jump), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, else_asg), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the else ASSIGN's constant is #1, not #0 -- the SETIF's
 * own 0/1 result would not reproduce the diamond's value, so the collapse
 * must not fire. */
UT_TEST(test_select_setif_nonzero_else_kept)
{
  TCCIRState *ir = utb_pool_new();

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(4), utb_imm(TOK_EQ, I32), UTB_NONE);
  int setif = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(5, I32), utb_imm(TOK_NE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(5), UTB_NONE, UTB_NONE);
  int else_asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(1, I32), UTB_NONE); /* #1, not #0 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(5, I32), UTB_NONE);

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, setif), TCCIR_OP_SETIF);
  UT_ASSERT_EQ(utb_op(ir, else_asg), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* ================================================================== select (call diamond) */

/* POSITIVE: both arms call the SAME function with only their param-0 value
 * differing (both compile-time constants) -- collapses to a single SELECT
 * feeding one shared PARAM+CALL. */
UT_TEST(test_select_call_diamond_collapses)
{
  TCCIRState *ir = utb_pool_new();
  utb_pools_init(ir);
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  /* the call-diamond fold allocates a fresh TEMP (tcc_ir_get_vreg_temp) for
   * the SELECT result -- give it a real interval table. */
  utb_alloc_temp_intervals(ir, 16);

  static Sym callee;
  IROperand fn = utb_callee_named(ir, &callee, 20);

  /* then-block layout is PARAM,CALL,JUMP (3 instrs), so else_target must be
   * 5 (right after the JUMP) for the else block to start exactly there:
   *   0 CMP
   *   1 JUMPIF EQ -> 5                (else_target = 5)
   *   2 PARAM(then call_id=1, #11)
   *   3 CALL(then, argc=1)
   *   4 JUMP -> 7                     (-> merge)
   *   5 PARAM(else call_id=2, #22)
   *   6 CALL(else, argc=1)            (same callee `fn`)
   *   7 RETURNVOID                    (merge) */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));              /* 0 */
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(5), utb_imm(TOK_EQ, I32), UTB_NONE); /* 1: else=5 */
  int then_param = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(11, I32),
                            utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));             /* 2 */
  int then_call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
                           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));               /* 3 */
  int jump = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(7), UTB_NONE, UTB_NONE);             /* 4 -> merge(7) */
  int else_param = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(22, I32),
                            utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));              /* 5 else */
  int else_call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
                           utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));                /* 6 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 7 merge */

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_SELECT);
  UT_ASSERT_EQ(utb_op(ir, then_param), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, then_call), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jump), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, else_call), TCCIR_OP_FUNCCALLVOID); /* the kept, shared call */

  /* else PARAM0 now reads the SELECT result instead of the literal #22. */
  IROperand sel = utb_dest(ir, jumpif);
  int32_t sel_vr = irop_get_vreg(sel);
  IROperand else_param_val = utb_src1(ir, else_param);
  UT_ASSERT_EQ(irop_get_vreg(else_param_val), sel_vr);

  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, jumpif)), 11);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, jumpif)), 22);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the two arms call DIFFERENT functions -- the diamond must
 * not collapse (the call target itself differs, not just an argument). */
UT_TEST(test_select_call_diamond_different_callee_kept)
{
  TCCIRState *ir = utb_pool_new();
  utb_pools_init(ir);
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  static Sym callee_a, callee_b;
  IROperand fn_a = utb_callee_named(ir, &callee_a, 20);
  IROperand fn_b = utb_callee_named(ir, &callee_b, 21);

  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));               /* 0 */
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(5), utb_imm(TOK_EQ, I32), UTB_NONE); /* 1: else=5 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(11, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));                              /* 2 */
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn_a,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));                               /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(7), UTB_NONE, UTB_NONE);                        /* 4 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(22, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));                              /* 5 */
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn_b,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));                               /* 6 (different callee) */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                        /* 7 */

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* ================================================================== select (fallthrough normalization) */

/* POSITIVE: `JUMPIF C -> A; JUMP -> B` where A is the instruction right after
 * the JUMP normalizes to `JUMPIF !C -> B` (dropping the extra JUMP), which
 * then exposes a RETURNVALUE diamond for the main select fold in the SAME
 * pass invocation (both changes are counted). */
UT_TEST(test_select_fallthrough_normalize_then_collapses)
{
  TCCIRState *ir = utb_pool_new();
  /* the RETURNVALUE-diamond fold this normalization exposes allocates a fresh
   * TEMP (tcc_ir_get_vreg_temp) for the SELECT result. */
  utb_alloc_temp_intervals(ir, 16);

  /*
   *  0: CMP
   *  1: JUMPIF EQ -> 3     (else_target=3, "A")
   *  2: JUMP -> 4          (unconditional; B=4; A(3) is the next instr after this JUMP)
   *  3: RETURNVALUE #10    (A: the original else_target)
   *  4: RETURNVALUE #20    (B)
   *
   * Normalization: JUMPIF's dest A(3) is replaced with the JUMP's own target
   * B(4), and its cond negated (EQ->NE, ir_negate_condition = cond^1); the
   * JUMP is NOPed.  then_start now skips the NOPed JUMP and lands on A(3)
   * (RETURNVALUE #10) -- so A becomes the diamond's "then" arm and B(4)
   * (RETURNVALUE #20, the JUMP's own target) becomes "else".  The resulting
   * 2-arm RETURNVALUE diamond (JUMPIF ...->4; RETURNVALUE#10; RETURNVALUE#20)
   * is immediately collapsed to a SELECT by the RETURNVALUE-diamond fold in
   * the SAME pass call.
   */
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));   /* 0 */
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(3), utb_imm(TOK_EQ, I32), UTB_NONE); /* 1 */
  int jump = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(4), UTB_NONE, UTB_NONE);  /* 2 */
  int a_ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(10, I32), UTB_NONE); /* 3: "then" (A) */
  int b_ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(20, I32), UTB_NONE); /* 4: "else" (B) */

  int changes = tcc_ir_opt_select(ir);

  UT_ASSERT(changes >= 1);
  UT_ASSERT_EQ(utb_op(ir, jump), TCCIR_OP_NOP);
  /* Either the JUMPIF was consumed by the immediately-following SELECT fold
   * (becomes SELECT) or (if the fold's own preconditions on A/B happened to
   * fail) it at least carries the negated condition/new target -- assert the
   * strong oracle: on this exact shape the cascade DOES complete to SELECT. */
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_SELECT);
  UT_ASSERT_EQ(utb_op(ir, a_ret), TCCIR_OP_RETURNVALUE); /* "then" -- kept, now reads the SELECT result */
  UT_ASSERT_EQ(utb_op(ir, b_ret), TCCIR_OP_NOP);          /* "else" -- unreachable now */

  IROperand sel = utb_dest(ir, jumpif);
  int32_t sel_vr = irop_get_vreg(sel);
  IROperand ret_src = utb_src1(ir, a_ret);
  UT_ASSERT_EQ(irop_get_vreg(ret_src), sel_vr);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, jumpif)), 10);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, jumpif)), 20);

  utb_free(ir);
  return 0;
}

/* ================================================================== returnvalue_merge */

/* POSITIVE: three RETURNVALUE sites, two returning the same constant (#1) --
 * the SECOND #1 site becomes a JUMP to the first; the #2 site (distinct
 * value) and the first #1 site are untouched. */
UT_TEST(test_returnvalue_merge_duplicate_becomes_jump)
{
  TCCIRState *ir = utb_new();

  int first_one = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);  /* 0 */
  int two = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);         /* 1 */
  int second_one = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);  /* 2 */

  int changes = tcc_ir_opt_returnvalue_merge(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, first_one), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_op(ir, two), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_op(ir, second_one), TCCIR_OP_JUMP);
  IROperand jd = utb_dest(ir, second_one);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, jd), first_one);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a RETURNVALUE of an INT64 constant is skipped even when
 * duplicated -- 64-bit materialization is more than one instruction, so the
 * pass explicitly excludes it. */
UT_TEST(test_returnvalue_merge_int64_kept)
{
  TCCIRState *ir = utb_new();

  int first = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(7, I64), UTB_NONE);
  int second = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(7, I64), UTB_NONE);

  int changes = tcc_ir_opt_returnvalue_merge(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, first), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_op(ir, second), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  return 0;
}

/* ================================================================== backedge_phi_hoist */

/* POSITIVE: `CMP; JUMPIF GE->exit; ASSIGN(phi, unspilled reg); JUMP->header`
 * inverts to `ASSIGN; JUMPIF LT->body; ...` -- the phi copy is hoisted before
 * the (now-inverted) guard and the old unconditional JUMP is NOP'd. */
UT_TEST(test_backedge_phi_hoist_inverts_and_hoists)
{
  TCCIRState *ir = utb_new();
  utb_ls_new(ir);
  /* T1 (phi dest) and T2 (phi src) both live in r4, unspilled. */
  utb_ls_reg(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1), 4);
  utb_ls_reg(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 2), 4);

  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(10, I32));  /* 0 */
  int jif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(5), utb_imm(TOK_GE, I32), UTB_NONE); /* 1: exit=5 */
  int asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(2, I32), UTB_NONE);  /* 2: phi copy */
  int jmp = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(0), UTB_NONE, UTB_NONE);              /* 3: body_target=0 < i=1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(3, I32), utb_imm(1, I32));        /* 4: filler */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);               /* 5: exit; reads T3, not T1 */

  int changes = tcc_ir_opt_backedge_phi_hoist(ir);

  UT_ASSERT_EQ(changes, 1);
  /* i = jif's index (1). [i..i+num_assigns-1] = [1..1] becomes the ASSIGN(s)
   * (so the JUMPIF's OLD slot now holds the hoisted phi copy);
   * [i+num_assigns] = [2] (the ASSIGN's old slot) becomes the inverted
   * JUMPIF; the old unconditional JUMP (3) is NOPed.  cmp's own slot (0) is
   * untouched -- only [i..jump_idx] is rewritten. */
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);      /* slot 0 untouched */
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_ASSIGN);   /* slot 1 now holds the hoisted ASSIGN */
  UT_ASSERT_EQ(utb_op(ir, asg), TCCIR_OP_JUMPIF);   /* slot 2 now holds the (inverted) JUMPIF */
  UT_ASSERT_EQ(utb_op(ir, jmp), TCCIR_OP_NOP);      /* old JUMP gone */

  /* The hoisted ASSIGN (now at slot 1) still copies T2 -> T1. */
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, jif)), 1);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, jif)), 2);

  IROperand new_cond = utb_src1(ir, asg);
  UT_ASSERT_EQ((int)new_cond.u.imm32, TOK_LT); /* invert_condition(GE) == LT */
  IROperand new_dest = utb_dest(ir, asg);
  UT_ASSERT_EQ((int)new_dest.u.imm32, 0); /* retargeted to body_target (the old JUMP's target) */

  utb_free(ir);
  utb_ls_free(ir);
  return 0;
}

/* NEGATIVE (guard): the phi ASSIGN's source vreg is spilled -- hoisting a
 * spilled copy across the (now-inverted) guard is unsafe (a stack load/store
 * can disturb pending flags), so the transform must not fire. */
UT_TEST(test_backedge_phi_hoist_spilled_operand_kept)
{
  TCCIRState *ir = utb_new();
  utb_ls_new(ir);
  utb_ls_reg(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1), 4);
  utb_ls_spill(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 2)); /* src spilled */

  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(10, I32));
  int jif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(5), utb_imm(TOK_GE, I32), UTB_NONE);
  int asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(2, I32), UTB_NONE);
  int jmp = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(0), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(3, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = tcc_ir_opt_backedge_phi_hoist(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, asg), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, jmp), TCCIR_OP_JUMP);

  utb_free(ir);
  utb_ls_free(ir);
  return 0;
}

/* ================================================================== post_ra_forward_diamond */

/* POSITIVE: a strict forward diamond `JUMPIF cond->T; ASSIGN(coalesced
 * no-op, same physical reg both sides); JUMP->M; T: ...` (T==jump_idx+1)
 * inverts to `JUMPIF !cond->M`, NOPing the no-op ASSIGN and the JUMP. This
 * also exercises the live in-progress phi_pinned guard added around the
 * transform: both the copy's dest and src TEMP intervals must come out
 * pinned afterward (documents CURRENT behavior of the fix in the diff under
 * ir/opt_promote.c:tcc_ir_opt_post_ra_forward_diamond -- see
 * docs/plan_ut_next_steps.md; that fix is live/uncommitted parallel work, not
 * touched by this file). */
UT_TEST(test_post_ra_forward_diamond_collapses_and_pins_phi)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 8);
  utb_ls_new(ir);
  /* T1 (dest) and T2 (src) share r5, unspilled -- a coalesced no-op copy. */
  utb_ls_reg(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1), 5);
  utb_ls_reg(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 2), 5);

  int jif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(3), utb_imm(TOK_EQ, I32), UTB_NONE); /* 0: T=3 */
  int asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(2, I32), UTB_NONE);   /* 1: no-op copy */
  int jmp = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(4), UTB_NONE, UTB_NONE);               /* 2: -> merge(4) */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(3, I32), utb_imm(1, I32));         /* 3: then-target (T) */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                         /* 4: merge (M) */

  int changes = tcc_ir_opt_post_ra_forward_diamond(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  IROperand new_cond = utb_src1(ir, jif);
  UT_ASSERT_EQ((int)new_cond.u.imm32, TOK_NE); /* invert_condition(EQ) == NE */
  IROperand new_dest = utb_dest(ir, jif);
  UT_ASSERT_EQ((int)new_dest.u.imm32, 4); /* retargeted to merge */
  UT_ASSERT_EQ(utb_op(ir, asg), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jmp), TCCIR_OP_NOP);

  /* phi_pinned guard: both T1 and T2's live intervals are now pinned so a
   * later codegen scratch-conflict fixup cannot silently move just one side
   * out of the shared register. */
  IRLiveInterval *dli = tcc_ir_vreg_live_interval(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1));
  IRLiveInterval *sli = tcc_ir_vreg_live_interval(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 2));
  UT_ASSERT_EQ(dli->phi_pinned, 1);
  UT_ASSERT_EQ(sli->phi_pinned, 1);

  utb_free(ir);
  utb_ls_free(ir);
  return 0;
}

/* NEGATIVE (guard): the ASSIGN's dest and src are in DIFFERENT physical
 * registers -- not a coalesced no-op, so eliminating it would drop a real
 * value move; the diamond must be left untouched. */
UT_TEST(test_post_ra_forward_diamond_different_regs_kept)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 8);
  utb_ls_new(ir);
  utb_ls_reg(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1), 5);
  utb_ls_reg(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 2), 6); /* different reg */

  int jif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(3), utb_imm(TOK_EQ, I32), UTB_NONE);
  int asg = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(2, I32), UTB_NONE);
  int jmp = utb_emit(ir, TCCIR_OP_JUMP, utb_jtarget(4), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(3, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_post_ra_forward_diamond(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, asg), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, jmp), TCCIR_OP_JUMP);

  utb_free(ir);
  utb_ls_free(ir);
  return 0;
}

/* ================================================================== abort_tail_merge */

/* POSITIVE: two independent `cmp; guard; PARAM0; call abort` sites for the
 * SAME noreturn callee -- the second (non-zero-eligible... both are EQ/NE
 * here so both are "zero-eligible"; the FIRST becomes the shared sink) tail-
 * merges: its guard is inverted and retargeted to the first site's entry,
 * and its local PARAM+CALL are NOP'd. */
UT_TEST(test_abort_tail_merge_two_sites_merge_to_one_sink)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym abort_sym;
  IROperand fn = utb_callee_named(ir, &abort_sym, 30);
  utb_set_tok_str(30, "abort");

  /* Site 1 (becomes the sink -- kept inline):
   *   0: TEST_ZERO T0
   *   1: JUMPIF NE -> 3        (guard: continue past the call when T0 != 0)
   *   2: FUNCCALLVOID abort, argc=0
   *   3: <continue / site 2>
   * Site 2:
   *   3: TEST_ZERO T1
   *   4: JUMPIF NE -> 6
   *   5: FUNCCALLVOID abort, argc=0
   *   6: RETURNVOID
   */
  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32), UTB_NONE);              /* 0 */
  int jif1 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(3), utb_imm(TOK_NE, I32), UTB_NONE); /* 1 */
  int call1 = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));                /* 2 */
  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(1, I32), UTB_NONE);              /* 3 */
  int jif2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(6), utb_imm(TOK_NE, I32), UTB_NONE); /* 4 */
  int call2 = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 0), I32));                /* 5 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                     /* 6 */

  int changes = tcc_ir_opt_abort_tail_merge(ir);

  UT_ASSERT_EQ(changes, 1);
  /* Site 1 (the sink) is untouched. */
  UT_ASSERT_EQ(utb_op(ir, jif1), TCCIR_OP_JUMPIF);
  IROperand jif1_cond = utb_src1(ir, jif1);
  UT_ASSERT_EQ((int)jif1_cond.u.imm32, TOK_NE);
  UT_ASSERT_EQ(utb_op(ir, call1), TCCIR_OP_FUNCCALLVOID);

  /* Site 2's guard is inverted (NE->EQ) and retargeted to site 1's entry
   * (index 2, the first PARAM/CALL slot -- here just the CALL since argc=0). */
  IROperand jif2_cond = utb_src1(ir, jif2);
  UT_ASSERT_EQ((int)jif2_cond.u.imm32, TOK_EQ);
  IROperand jif2_dest = utb_dest(ir, jif2);
  UT_ASSERT_EQ((int)jif2_dest.u.imm32, 2);
  /* Site 2's own call is NOPed (shares site 1's call now). */
  UT_ASSERT_EQ(utb_op(ir, call2), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a single guarded abort site has no sibling to merge with
 * -- ir_abort_guard_site's own entry IS the (only) sink, so `entry == sink`
 * and the pass makes no change. */
UT_TEST(test_abort_tail_merge_single_site_no_change)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym abort_sym;
  IROperand fn = utb_callee_named(ir, &abort_sym, 30);
  utb_set_tok_str(30, "abort");

  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32), UTB_NONE);          /* 0 */
  int jif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_jtarget(3), utb_imm(TOK_NE, I32), UTB_NONE); /* 1 */
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));             /* 2 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                 /* 3 */

  int changes = tcc_ir_opt_abort_tail_merge(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  return 0;
}
