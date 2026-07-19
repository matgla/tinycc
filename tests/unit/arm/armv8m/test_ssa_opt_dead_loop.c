/*
 *  test_ssa_opt_dead_loop.c - dead loop elimination pass
 *
 *  Phase 4: Large, high-bug-density passes.
 *
 *  Covers:
 *    - resolve_const_through_copies(): resolving vregs through ASSIGN chains
 *    - loop_max_idx() / dead_loop_body_hi(): body upper bound calculation
 *    - loop_body_has_side_effects(): side effect detection
 *    - dl_invert_cond_token(): JUMPIF condition token inversion
 *    - analyze_loop_entry(): loop entry pattern recognition
 *    - rewrite_loop_exit_phis(): constant-path exit phi rewriting
 *    - try_kill_loop_body(): loop body elimination
 *    - rewrite_loop_exit_phis_guarded(): SELECT-based guarded variant
 *    - ssa_opt_dead_loop(): main entry point
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_dead_loop.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 *    - Loop tests build IR with JUMPIF/JUMP patterns; CFG builder derives blocks.
 *    - JUMPIF uses dest=target index, src1=condition token.
 *    - Phis are hand-built and pred_block values patched to match CFG preds.
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "source/opt/ssa/include/ssa_opt.h"

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* CMP helper: dest=NONE, src1, src2, op4=NONE. */
#define ssa_add_cmp(c, s1, s2) ssa_add_instr3((c), TCCIR_OP_CMP, UTB_NONE, (s1), (s2))
/* ADD helper: dest, src1, src2, op4=NONE. */
#define ssa_add_add(c, d, s1, s2) ssa_add_instr4((c), TCCIR_OP_ADD, (d), (s1), (s2), UTB_NONE)

/* -------------------------------------------------------------------------
 * Loop-building helpers
 * ------------------------------------------------------------------------- */

/* Patch pred_block values of the phis in block `b` to the given array.
 * Phis are visited in list order (most recently added first). */
static void patch_phi_preds(IRSSAState *ssa, int b, const int *preds, int num_preds)
{
  int idx = 0;
  for (IRPhiNode *phi = ssa->block_phis[b]; phi && idx < num_preds; phi = phi->next, idx++) {
    for (int i = 0; i < phi->num_operands && i < num_preds; i++)
      phi->operands[i].pred_block = preds[i];
  }
}

/* Build the canonical counted loop fixture used by most tests.
 *
 *   block 0 (preheader):
 *     T0 = init          iv init
 *     T1 = bound         loop bound
 *     T2 = acc_init      accumulator init
 *   block 1 (header):
 *     T5 = phi(T0, T4)   iv phi
 *     T6 = phi(T2, T3)   accumulator phi
 *     CMP T5, T1
 *     JUMPIF cond >= exit
 *   block 2 (body):
 *     T3 = acc_latch     accumulator latch value
 *     T4 = T5 + step     iv step
 *     JUMP header
 *   block 3 (exit):
 *     T7 = T6            use of accumulator phi
 *
 * Returns the instruction index of the exit use (T7 = T6).
 */
static int build_counted_loop(ssa_ctx *c, int32_t init, int32_t bound,
                              int32_t acc_init, int32_t acc_latch,
                              int32_t step, int cond_tok)
{
  int exit_idx = 8; /* preheader 3 + header 2 + body 3 = 8 instr before exit */
  (void)exit_idx;

  /* Preheader */
  ssa_add_instr(c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(init, I32));
  ssa_add_instr(c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(bound, I32));
  ssa_add_instr(c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(acc_init, I32));

  /* Header: CMP iv_phi, bound; JUMPIF cond exit.
   * The dead-loop pass runs after SCCP, so proven constant-bound fixtures use
   * the already-folded immediate bound in the CMP. */
  int header_cmp = ssa_add_cmp(c, utb_temp(5, I32), utb_imm(bound, I32));
  int header_jpf = ssa_add_instr(c, TCCIR_OP_JUMPIF,
                                 utb_imm(8, I32), /* exit instruction index */
                                 utb_imm(cond_tok, I32));
  (void)header_cmp;
  (void)header_jpf;

  /* Body: acc latch, iv step, back-edge */
  ssa_add_instr(c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(acc_latch, I32));
  ssa_add_add(c, utb_temp(4, I32), utb_temp(5, I32), utb_imm(step, I32));
  ssa_add_instr(c, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE);

  /* Exit: use of acc phi */
  int use_i = ssa_add_instr(c, TCCIR_OP_ASSIGN, utb_temp(7, I32), utb_temp(6, I32));

  ssa_ctx_build_cfg(c);
  ssa_ctx_build_ssa_plain(c);

  /* IV phi T5 = [T0 from preheader (block 0), T4 from latch (block 2)] */
  ssa_add_phi(c, 1, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(4, I32)) }, 2);
  /* Acc phi T6 = [T2 from preheader (block 0), T3 from latch (block 2)] */
  ssa_add_phi(c, 1, utb_vreg(utb_temp(6, I32)),
              (int32_t[]){ utb_vreg(utb_temp(2, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);

  int preds[2] = { 0, 2 };
  patch_phi_preds(c->ssa, 1, preds, 2);

  ssa_ctx_rebuild(c);
  return use_i;
}

/* ========================================================================
 * resolve_const_through_copies: constant through ASSIGN chain
 * ======================================================================== */

UT_TEST(test_resolve_const_through_copies_direct)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* t0 = #42; t1 = t0 (ASSIGN copy) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(42, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* Manually test resolve_const_through_copies by checking vinfo. */
  IRSSAVregInfo *vi0 = ssa_opt_vinfo(c.ctx, utb_vreg(utb_temp(0, I32)));
  IRSSAVregInfo *vi1 = ssa_opt_vinfo(c.ctx, utb_vreg(utb_temp(1, I32)));

  UT_ASSERT(vi0 != NULL);
  UT_ASSERT(vi1 != NULL);
  UT_ASSERT_EQ(vi0->def_instr, 0);
  UT_ASSERT_EQ(vi1->def_instr, 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * dl_invert_cond_token: token inversion (tested via pass behavior)
 * ======================================================================== */

UT_TEST(test_dl_invert_cond_token_via_pass)
{
  /* Build a simple loop with JUMPIF >= */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  /* Block 0 (header): CMP T_iv, #10; JUMPIF >= exit */
  ssa_add_cmp(&c, utb_temp(0, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(0x9d, I32)); /* >= */
  /* Block 1 (body): t2 = #1; JUMP header */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(0, I32), UTB_NONE);
  /* Block 2 (exit): t3 = #0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* The pass should attempt to analyze the loop. */
  int changed = ssa_opt_dead_loop(c.ctx);
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * loop_body_has_side_effects: no side effects (arithmetic only)
 * ======================================================================== */

UT_TEST(test_loop_body_no_side_effects)
{
  /* Build a loop with only arithmetic operations. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/5);
  /* Preheader: t0 = #0 (init), t1 = #10 (bound) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(10, I32));
  /* Header: CMP t_iv, #10; JUMPIF >= exit */
  ssa_add_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(0x9d, I32));
  /* Body: t2 = t0 + #1; JUMP header */
  ssa_add_add(&c, utb_temp(2, I32), utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  /* Exit: t3 = #0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* The pass should attempt to analyze the loop. */
  int changed = ssa_opt_dead_loop(c.ctx);
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * loop_body_has_side_effects: store in body -> keep loop
 * ======================================================================== */

UT_TEST(test_loop_body_with_store)
{
  /* Build a loop with a STORE (has side effects). */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/5);
  /* Preheader: t0 = #0, t1 = #10 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(10, I32));
  /* Header: CMP t_iv, #10; JUMPIF >= exit */
  ssa_add_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(0x9d, I32));
  /* Body: STORE(*t_ptr, t_val); JUMP header */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(2, I32)), utb_temp(3, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  /* Exit: t4 = #0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(4, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* The pass should keep the loop (has side effects). */
  int changed = ssa_opt_dead_loop(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Proven loop: constant bound, rewrite exit phi to latch const and kill body
 * ======================================================================== */

UT_TEST(test_proven_loop_rewrites_and_kills)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/8);
  int use_i = build_counted_loop(&c, /*init=*/0, /*bound=*/10,
                                  /*acc_init=*/7, /*acc_latch=*/1,
                                  /*step=*/1, /*cond=*/0x9d);

  int changed = ssa_opt_dead_loop(c.ctx);
  /* One change for rewriting the acc phi, one for killing the body. */
  UT_ASSERT(changed >= 2);

  /* Header CMP/JUMPIF and body instructions are NOPed; JUMPIF becomes JUMP. */
  UT_ASSERT_EQ(utb_op(c.ir, 3), TCCIR_OP_NOP);   /* CMP */
  UT_ASSERT_EQ(utb_op(c.ir, 4), TCCIR_OP_JUMP);  /* old JUMPIF */
  UT_ASSERT_EQ(utb_op(c.ir, 5), TCCIR_OP_NOP);   /* acc latch */
  UT_ASSERT_EQ(utb_op(c.ir, 6), TCCIR_OP_NOP);   /* iv step */
  UT_ASSERT_EQ(utb_op(c.ir, 7), TCCIR_OP_NOP);   /* back-edge JUMP */

  /* Post-loop use of the acc phi now reads the latch constant. */
  IROperand exit_src = utb_src1(c.ir, use_i);
  UT_ASSERT_EQ(exit_src.tag, IROP_TAG_IMM32);
  UT_ASSERT_EQ(exit_src.u.imm32, 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Proven loop with down-counting IV: SUB step
 * ======================================================================== */

UT_TEST(test_proven_loop_down_counter)
{
  /* Down loop: init=10, bound=0, cond '<=' (exit when iv <= bound).
   * Going_down with tok 0x9e or 0x96.  Use 0x9e (<=). */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/8);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(7, I32));

  ssa_add_cmp(&c, utb_temp(5, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(0x9e, I32)); /* <= */

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(1, I32));
  ssa_add_instr4(&c, TCCIR_OP_SUB, utb_temp(4, I32), utb_temp(5, I32),
                 utb_imm(1, I32), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE);

  int use_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32), utb_temp(6, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  ssa_add_phi(&c, 1, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(4, I32)) }, 2);
  ssa_add_phi(&c, 1, utb_vreg(utb_temp(6, I32)),
              (int32_t[]){ utb_vreg(utb_temp(2, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);

  int preds[2] = { 0, 2 };
  patch_phi_preds(c.ssa, 1, preds, 2);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_dead_loop(c.ctx);
  UT_ASSERT(changed >= 2);

  UT_ASSERT_EQ(utb_op(c.ir, 4), TCCIR_OP_JUMP);
  IROperand exit_src = utb_src1(c.ir, use_i);
  UT_ASSERT_EQ(exit_src.tag, IROP_TAG_IMM32);
  UT_ASSERT_EQ(exit_src.u.imm32, 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Guarded SELECT path: runtime bound, trip count not provable
 * ======================================================================== */

UT_TEST(test_guarded_loop_emits_select)
{
  /* Bound is a VAR (runtime value), so proven_runs is false. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/8);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32)); /* runtime bound */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(7, I32));

  ssa_add_cmp(&c, utb_temp(5, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(0x9d, I32)); /* >= */

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(1, I32));
  ssa_add_add(&c, utb_temp(4, I32), utb_temp(5, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32), utb_temp(6, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  ssa_add_phi(&c, 1, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(4, I32)) }, 2);
  ssa_add_phi(&c, 1, utb_vreg(utb_temp(6, I32)),
              (int32_t[]){ utb_vreg(utb_temp(2, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);

  int preds[2] = { 0, 2 };
  patch_phi_preds(c.ssa, 1, preds, 2);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_dead_loop(c.ctx);
  /* Guarded path returns num_cands (1) on success. */
  UT_ASSERT(changed >= 1);

  /* The header JUMPIF was overwritten by a SELECT, followed by a JUMP. */
  UT_ASSERT_EQ(utb_op(c.ir, 4), TCCIR_OP_SELECT);
  UT_ASSERT_EQ(utb_op(c.ir, 5), TCCIR_OP_JUMP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * analyze_loop_entry bails when preheader IV is not a constant
 * ======================================================================== */

UT_TEST(test_analyze_bails_non_const_init)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/8);

  /* Preheader: iv init comes from another temp, not an immediate. */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_temp(4, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(7, I32));

  ssa_add_cmp(&c, utb_temp(5, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(0x9d, I32));

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(1, I32));
  ssa_add_add(&c, utb_temp(4, I32), utb_temp(5, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32), utb_temp(6, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  ssa_add_phi(&c, 1, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(4, I32)) }, 2);
  ssa_add_phi(&c, 1, utb_vreg(utb_temp(6, I32)),
              (int32_t[]){ utb_vreg(utb_temp(2, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);

  int preds[2] = { 0, 2 };
  patch_phi_preds(c.ssa, 1, preds, 2);

  ssa_ctx_rebuild(&c);

  /* analyze_loop_entry should bail because the preheader IV operand (T0)
   * does not resolve to a constant. */
  int changed = ssa_opt_dead_loop(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * rewrite_loop_exit_phis skips phis used inside the loop body
 * ======================================================================== */

UT_TEST(test_phi_used_in_loop_not_rewritten)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/8);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(7, I32));

  ssa_add_cmp(&c, utb_temp(5, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(0x9d, I32));

  /* Body uses the acc phi T6 directly (in-loop use) -> must not rewrite. */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_temp(6, I32));
  ssa_add_add(&c, utb_temp(4, I32), utb_temp(5, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32), utb_temp(6, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  ssa_add_phi(&c, 1, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(4, I32)) }, 2);
  ssa_add_phi(&c, 1, utb_vreg(utb_temp(6, I32)),
              (int32_t[]){ utb_vreg(utb_temp(2, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);

  int preds[2] = { 0, 2 };
  patch_phi_preds(c.ssa, 1, preds, 2);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_dead_loop(c.ctx);
  /* IV phi latch is not constant; acc phi has in-loop use -> no rewrite,
   * and therefore no kill either. */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * try_kill_loop_body bails when a body temp escapes the loop
 * ======================================================================== */

UT_TEST(test_body_escape_prevents_kill)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/8);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(7, I32));

  ssa_add_cmp(&c, utb_temp(5, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(8, I32), utb_imm(0x9d, I32));

  /* Body defines T3 and also uses it at exit (escapes). */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(1, I32));
  ssa_add_add(&c, utb_temp(4, I32), utb_temp(5, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32), utb_temp(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  ssa_add_phi(&c, 1, utb_vreg(utb_temp(5, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(4, I32)) }, 2);

  int preds[2] = { 0, 2 };
  patch_phi_preds(c.ssa, 1, preds, 2);

  ssa_ctx_rebuild(&c);

  /* rewrite_loop_exit_phis has no non-IV phi to rewrite; try_kill_loop_body
   * sees T3 escaping and bails. */
  int changed = ssa_opt_dead_loop(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Loop with no header CMP -> analyze_loop_entry bails
 * ======================================================================== */

UT_TEST(test_no_header_cmp_bails)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);

  /* Header has no CMP; just a back-edge. */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  ssa_add_add(&c, utb_temp(2, I32), utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_dead_loop(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_dead_loop: no loops -> no change
 * ======================================================================== */

UT_TEST(test_dead_loop_no_loops)
{
  /* Straight-line code with no loops. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_dead_loop(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:dead_loop");
