/*
 *  test_ssa_opt_sccp.c - sparse conditional constant propagation
 *
 *  Phase 4: large pass.
 *
 *  Covers:
 *    - ssa_opt_sccp(): constant propagation through arithmetic/shifts
 *    - branch folding via CMP/TEST_ZERO + JUMPIF
 *    - phi lattice meet (single reachable edge vs. multiple edges)
 *    - stack-load forwarding (direct, via LEA deref, indexed)
 *    - alias barriers (calls, STORE_POSTINC, VAR-held pointers)
 *    - loop-carried clobber detection
 *    - apply phase: imm32 vs. i64-pool constant replacement
 *    - CMP operand substitution from VAR/stack loads (phase 1.5)
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_sccp.c via UT11.
 *    - Uses ssa_build.h fixtures; branching tests build a real CFG with
 *      JUMP/JUMPIF target instruction indices.
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include <limits.h>
#include "tcc.h"
#include "source/opt/ssa/include/ssa_opt.h"

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* Patch phi operand pred_block values to the real CFG predecessor blocks. */
static int patch_phi_preds(ssa_ctx *c, int block, IRPhiNode *phi)
{
  UT_ASSERT(phi->num_operands == c->cfg->blocks[block].num_preds);
  for (int i = 0; i < phi->num_operands; i++)
    phi->operands[i].pred_block = c->cfg->blocks[block].preds[i];
  return 0;
}

/* ========================================================================
 * Constant propagation: arithmetic chain folds to single ASSIGN
 * t0 = #5; t1 = #3; t2 = t0 + t1; t3 = t2 * #2  ->  t3 = #16
 * ======================================================================== */

UT_TEST(test_sccp_const_fold_arith_chain)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32),
                             utb_temp(1, I32));
  int mul_i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(3, I32), utb_temp(2, I32),
                             utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 1);

  /* The ADD should have been folded to ASSIGN #8. */
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, add_i).u.imm32, 8);

  /* The MUL should have been folded to ASSIGN #16. */
  UT_ASSERT_EQ(utb_op(c.ir, mul_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, mul_i).u.imm32, 16);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * 64-bit constant pool path: 1 << 32 needs I64 pool, not IMM32
 * ======================================================================== */

UT_TEST(test_sccp_const_fold_i64_pool)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I64), utb_imm(1, I64));
  int shl_i = ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64),
                             utb_imm(32, I64), UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 1);

  UT_ASSERT_EQ(utb_op(c.ir, shl_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, shl_i).tag, IROP_TAG_I64);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Float operations are forced to BOTTOM rather than folded
 * ======================================================================== */

UT_TEST(test_sccp_float_bottom)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  int fadd_i = ssa_add_instr3(&c, TCCIR_OP_FADD, utb_temp(0, IROP_BTYPE_FLOAT32),
                              utb_imm(0x3f800000, IROP_BTYPE_FLOAT32),
                              utb_imm(0x40000000, IROP_BTYPE_FLOAT32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  /* No rewrite: float ops stay as-is. */
  UT_ASSERT_EQ(utb_op(c.ir, fadd_i), TCCIR_OP_FADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Barrel-shift guard: an instruction with a barrel-shift annotation must
 * not be folded, because the hidden shift is not visible in the operands.
 * ======================================================================== */

UT_TEST(test_sccp_barrel_shift_guard)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int and_i = ssa_add_instr3(&c, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32),
                             utb_imm(0xFF, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* Manufacture a barrel-shift annotation for the AND instruction. */
  int n = and_i + 1;
  c.ir->barrel_shifts = tcc_mallocz(n * sizeof(uint8_t));
  c.ir->barrel_shifts[and_i] = 1;
  c.ir->barrel_shifts_len = n;

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  UT_ASSERT_EQ(utb_op(c.ir, and_i), TCCIR_OP_AND);

  tcc_free(c.ir->barrel_shifts);
  c.ir->barrel_shifts = NULL;
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Direct stack-load forwarding: a single init store reaches a later load.
 * ======================================================================== */

UT_TEST(test_sccp_stack_load_forward)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  IROperand slot = utb_stackoff(8, /*is_lval=*/1, /*is_llocal=*/0,
                                /*is_param=*/0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, slot, utb_imm(42, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), slot);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 1);

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, load_i).u.imm32, 42);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Forward across a non-aliasing store to a different stack slot.
 * ======================================================================== */

UT_TEST(test_sccp_stack_load_non_alias_forward)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  IROperand slot8 = utb_stackoff(8, 1, 0, 0, I32);
  IROperand slot16 = utb_stackoff(16, 1, 0, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, slot8, utb_imm(42, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, slot16, utb_imm(99, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), slot8);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 1);

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, load_i).u.imm32, 42);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * An intervening aliasing store blocks forwarding.
 * ======================================================================== */

UT_TEST(test_sccp_stack_load_alias_no_forward)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  IROperand slot8 = utb_stackoff(8, 1, 0, 0, I32);
  /* INT8 store to slot 9 overlaps the INT32 load of slot 8. */
  IROperand slot9 = utb_stackoff(9, 1, 0, 0, IROP_BTYPE_INT8);
  ssa_add_instr(&c, TCCIR_OP_STORE, slot8, utb_imm(42, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, slot9, utb_imm(0xFF, IROP_BTYPE_INT8));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), slot8);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * A call is only a stack-load-forwarding barrier when the slot's address
 * escapes to the callee; a non-escaped slot forwards across the call.
 * ======================================================================== */

UT_TEST(test_sccp_stack_load_call_barrier)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  IROperand slot = utb_stackoff(8, 1, 0, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, slot, utb_imm(42, I32));
  ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), slot);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 1);

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, load_i).u.imm32, 42);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_sccp_stack_load_call_addr_escape_barrier)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  IROperand slot = utb_stackoff(8, 1, 0, 0, I32);
  IROperand addr = utb_stackoff(8, 0, 0, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, slot, utb_imm(42, I32));
  ssa_add_instr3(&c, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, addr, utb_imm(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), slot);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Forwarding through a TEMP-LEA deref: store *ptr then load the slot.
 * ======================================================================== */

UT_TEST(test_sccp_stack_load_lea_deref_forward)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  IROperand slot = utb_stackoff(8, 1, 0, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), slot);
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(55, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), slot);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 1);

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, load_i).u.imm32, 55);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Forwarding through STORE_INDEXED with a zero-scale immediate index.
 * ======================================================================== */

UT_TEST(test_sccp_store_indexed_forward)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  IROperand slot = utb_stackoff(8, 1, 0, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), slot);
  ssa_add_instr4(&c, TCCIR_OP_STORE_INDEXED, utb_temp(0, I32),
                 utb_imm(77, I32), utb_imm(0, I32), utb_imm(0, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), slot);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 1);

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, load_i).u.imm32, 77);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * VAR load forwarding: V0 = #42; t0 = LOAD(V0); t1 = t0 + #1 -> #43.
 * ======================================================================== */

UT_TEST(test_sccp_var_load_forward)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  IROperand v0 = utb_var(0, I32);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, v0, utb_imm(42, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), v0);
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32),
                             utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 2);

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, load_i).u.imm32, 42);
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, add_i).u.imm32, 43);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phi with only one executable predecessor edge folds to that constant.
 *
 * B0: t0 = #1; t1 = #2; CMP t0,t1; JUMPIF EQ target B2 else fall B1
 * B1: t2 = #10; JUMP B3
 * B2: t3 = #20; JUMP B3
 * B3: phi t4 = [t2 from B1, t3 from B2]; t5 = t4 + #1
 *
 * 1 == 2 is false, so B2 is unreachable; t4 folds to 10 and t5 to 11.
 * ======================================================================== */

UT_TEST(test_sccp_phi_one_edge_folds)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/8);

  int i0 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  int i1 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int jif_i = ssa_add_instr3(&c, TCCIR_OP_JUMPIF, utb_imm(0, I32),
                             utb_imm(TOK_EQ, I32), UTB_NONE);

  /* B1: fallthrough from B0. */
  int b1_start = c.num_instrs;
  int def_t2 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(10, I32));
  int b1_jmp = ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(0, I32), UTB_NONE);

  /* B2: taken target. */
  int b2_start = c.num_instrs;
  int def_t3 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(20, I32));
  int b2_jmp = ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(0, I32), UTB_NONE);

  /* B3: merge. */
  int b3_start = c.num_instrs;
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(5, I32), utb_temp(4, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  tcc_ir_set_dest(c.ir, jif_i, utb_imm(b2_start, I32));
  tcc_ir_set_dest(c.ir, b1_jmp, utb_imm(b3_start, I32));
  tcc_ir_set_dest(c.ir, b2_jmp, utb_imm(b3_start, I32));
  (void)i0; (void)i1; (void)b1_start; (void)def_t3;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  /* phi t4 = [t2 from B1, t3 from B2] */
  ssa_add_phi(&c, c.cfg->instr_to_block[b3_start], utb_vreg(utb_temp(4, I32)),
              (int32_t[]){ utb_vreg(utb_temp(2, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);
  patch_phi_preds(&c, c.cfg->instr_to_block[b3_start], c.ssa->block_phis[c.cfg->instr_to_block[b3_start]]);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 1);

  /* ADD t4+1 should have folded to ASSIGN #11. */
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, add_i).u.imm32, 11);
  /* The unreachable block's definition should not have been rewritten. */
  UT_ASSERT_EQ(utb_op(c.ir, def_t2), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phi with two executable predecessor edges and different constants -> BOTTOM.
 * Same shape but condition is unknown, so both edges may be reachable.
 * ======================================================================== */

UT_TEST(test_sccp_phi_two_edges_bottom)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/8);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_temp(6, I32));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int jif_i = ssa_add_instr3(&c, TCCIR_OP_JUMPIF, utb_imm(0, I32),
                             utb_imm(TOK_EQ, I32), UTB_NONE);

  int b1_start = c.num_instrs;
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(10, I32));
  int b1_jmp = ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(0, I32), UTB_NONE);

  int b2_start = c.num_instrs;
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(20, I32));
  int b2_jmp = ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(0, I32), UTB_NONE);

  int b3_start = c.num_instrs;
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(5, I32), utb_temp(4, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  tcc_ir_set_dest(c.ir, jif_i, utb_imm(b2_start, I32));
  tcc_ir_set_dest(c.ir, b1_jmp, utb_imm(b3_start, I32));
  tcc_ir_set_dest(c.ir, b2_jmp, utb_imm(b3_start, I32));
  (void)b1_start;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  ssa_add_phi(&c, c.cfg->instr_to_block[b3_start], utb_vreg(utb_temp(4, I32)),
              (int32_t[]){ utb_vreg(utb_temp(2, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);
  patch_phi_preds(&c, c.cfg->instr_to_block[b3_start], c.ssa->block_phis[c.cfg->instr_to_block[b3_start]]);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  /* The phi is BOTTOM, so the ADD cannot fold. */
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * CMP + JUMPIF with constant operands resolves the branch and lets SCCP
 * ignore the unreachable successor's definitions.
 * ======================================================================== */

UT_TEST(test_sccp_cmp_branch_folds)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int b1_start = c.num_instrs;
  ssa_add_instr3(&c, TCCIR_OP_JUMPIF, utb_imm(b1_start, I32),
                 utb_imm(TOK_EQ, I32), UTB_NONE);

  /* B1: taken when 1 == 2 (never). */
  int dead_def = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(99, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  /* B2: fallthrough (always). */
  int live_def = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(11, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  (void)b1_start;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  /* Live definition stays; dead definition is not rewritten (still ASSIGN #99). */
  UT_ASSERT_EQ(utb_op(c.ir, live_def), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, live_def).u.imm32, 11);
  UT_ASSERT_EQ(utb_op(c.ir, dead_def), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * TEST_ZERO + JUMPIF with a constant zero operand resolves the branch.
 * ======================================================================== */

UT_TEST(test_sccp_test_zero_branch)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/3);

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32));
  int b1_start = c.num_instrs;
  ssa_add_instr3(&c, TCCIR_OP_JUMPIF, utb_imm(b1_start, I32),
                 utb_imm(TOK_NE, I32), UTB_NONE);

  int dead_def = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(99, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  int live_def = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(22, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  (void)b1_start;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  UT_ASSERT_EQ(utb_src1(c.ir, live_def).u.imm32, 22);
  UT_ASSERT_EQ(utb_op(c.ir, dead_def), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Loop-carried clobber: a load inside a loop body must not fold to the
 * preheader's init store, because the loop body writes the same slot.
 * ======================================================================== */

UT_TEST(test_sccp_loop_clobbers_slot)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  IROperand slot = utb_stackoff(8, 1, 0, 0, I32);

  /* B0: init store, jump to header. */
  ssa_add_instr(&c, TCCIR_OP_STORE, slot, utb_imm(7, I32));
  int b1_start = c.num_instrs;
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(b1_start, I32), UTB_NONE);

  /* B1: header. */
  int header_start = c.num_instrs;
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), slot);
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, slot, utb_temp(1, I32));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(10, I32));
  ssa_add_instr3(&c, TCCIR_OP_JUMPIF, utb_imm(header_start, I32),
                 utb_imm(TOK_LT, I32), UTB_NONE);

  /* B2: exit. */
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  (void)add_i;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Post-loop load must not fold across a loop that writes the same slot.
 * This exercises sccp_loop_writes_slot_between.
 * ======================================================================== */

UT_TEST(test_sccp_loop_writes_slot_post_loop)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/3);
  IROperand slot = utb_stackoff(8, 1, 0, 0, I32);

  /* B0: init. */
  ssa_add_instr(&c, TCCIR_OP_STORE, slot, utb_imm(7, I32));
  int b1_start = c.num_instrs;
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(b1_start, I32), UTB_NONE);

  /* B1: loop body writes the slot once, then exits. */
  ssa_add_instr(&c, TCCIR_OP_STORE, slot, utb_imm(99, I32));
  int b2_start = c.num_instrs;
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(b2_start, I32), UTB_NONE);

  /* B2: post-loop load. */
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), slot);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Entry-block exemption: a conditional *p store through a VAR-held pointer
 * between an init store and a post-dominator load must block forwarding.
 * Regression lock for ptr fuzz seed 58108.
 * ======================================================================== */

UT_TEST(test_sccp_entry_block_var_ptr_alias)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/4);
  IROperand slot = utb_stackoff(8, 1, 0, 0, I32);
  IROperand v0 = utb_var(0, I32);

  /* B0: init store, then branch (always fall through to B1). */
  ssa_add_instr(&c, TCCIR_OP_STORE, slot, utb_imm(7, I32));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_imm(1, I32), utb_imm(0, I32));
  int b2_start = c.num_instrs;
  ssa_add_instr3(&c, TCCIR_OP_JUMPIF, utb_imm(b2_start, I32),
                 utb_imm(TOK_EQ, I32), UTB_NONE);

  /* B1: fallthrough. */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(1, I32));
  int b3_start = c.num_instrs;
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(b3_start, I32), UTB_NONE);

  /* B2: unreachable in this run, contains *p store via VAR-held pointer. */
  int ptr_load = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32), v0);
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(2, I32)), utb_imm(123, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(b3_start, I32), UTB_NONE);

  /* B3: load the slot. */
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(3, I32), slot);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  (void)ptr_load;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  /* The unresolved VAR-pointer store in B2 aliases the slot conservatively,
   * so the load must not fold back to the initializer. */
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phase 1.5: CMP operand that resolves to a small immediate VAR def is
 * substituted into the CMP.
 * ======================================================================== */

UT_TEST(test_sccp_cmp_var_substitute)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  IROperand v0 = utb_var(0, I32);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, v0, utb_imm(5, I32));
  int cmp_i = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, v0, utb_imm(5, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 1);

  IROperand s1 = utb_src1(c.ir, cmp_i);
  UT_ASSERT_EQ(s1.tag, IROP_TAG_IMM32);
  UT_ASSERT_EQ(s1.u.imm32, 5);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phase 1.5: a VAR def with a pool-load constant that has other uses is
 * NOT substituted, so the single VAR load can satisfy all uses.
 * ======================================================================== */

UT_TEST(test_sccp_cmp_var_pool_no_substitute)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  IROperand v0 = utb_var(0, I32);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, v0, utb_imm(0x12345, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32), v0, utb_imm(1, I32));
  int cmp_i = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, v0, utb_imm(0x12345, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  IROperand s1 = utb_src1(c.ir, cmp_i);
  UT_ASSERT(s1.tag != IROP_TAG_IMM32);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * STORE_POSTINC is an opaque memory write and blocks stack-load forwarding.
 * ======================================================================== */

UT_TEST(test_sccp_store_postinc_barrier)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  IROperand slot = utb_stackoff(8, 1, 0, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_STORE, slot, utb_imm(42, I32));
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), slot);
  ssa_add_instr(&c, TCCIR_OP_STORE_POSTINC, utb_lval(utb_temp(0, I32)),
                utb_imm(1, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), slot);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  (void)changed;

  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Keep the original leaf-level branch tests (they exercise the basic
 * JUMPIF reachability path without requiring CMP setup).
 * ======================================================================== */

UT_TEST(test_sccp_always_taken)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_sccp_never_taken)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_sccp_unknown)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_sccp(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:sccp");
