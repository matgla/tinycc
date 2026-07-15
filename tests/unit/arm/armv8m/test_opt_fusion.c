/*
 *  test_opt_fusion.c - suite for the Phase 3 fusion family (docs/plan_ut_next_steps.md
 *  Phase 3): bool_simplify, fusion_mla, deref_indexed, disp_fusion, chain_fold,
 *  pair_reorder (source/opt/flat/scalar/bool.c, ir/opt_gens_fusion.c, driven through their
 *  non-static `_ex(IROptCtx*)` pipeline adapters in ir/opt_pipeline.c).
 *
 *  Each `_ex` adapter is a thin, non-static wrapper around
 *  `tcc_ir_opt_run_gens(ctx, <table>, <count>)` over a *distinct* generator
 *  table per pass (unlike the Phase 2 cascade wrappers, which all orchestrate
 *  already-tested passes) -- so each of these is genuinely new coverage, not
 *  duplicate exercising of shared logic.
 *
 *  Test sections below the "opt_fusion.c direct-entry
 *  passes" banner (and its shared utb_fusion_new()/utb_jtarget_fusion()/
 *  utb_stackoff_vreg() helpers) cover the *rest* of ir/opt_fusion.c's own
 *  functions -- these are NOT registered in ir/opt_pipeline.c's
 *  PASS/PASS_GATED tables at all (so they're outside check_pass_coverage.py's
 *  ledger); they're bare `int tcc_ir_opt_<name>(TCCIRState *ir)` entries
 *  called directly and unconditionally-per-flag from tccgen.c's
 *  IR-generation driver. Same "call the legacy entry directly" harness
 *  contract as every other bare pass (docs/plan_ut_next_steps.md S1):
 *  add_deref_fold, barrel_shift_fusion (void return --
 *  side-table output), shift_pair_to_ubfx, call_chain_rename,
 *  stackoff_addr_cse, lea_cse, lea_fold, lea_rmw_fold, assign_fuse.
 */

#include "ir_build.h"
#include "opt_engine.h"

#include "ut.h"

/* Pass entry points. The gens_* passes take IROptCtx* (defined in
 * ir/opt_pipeline.c). */
int tcc_ir_opt_gens_bool_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_fusion_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_deref_indexed_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_disp_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_chain_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_pair_reorder_ex(IROptCtx *ctx);

/* The rest of ir/opt_fusion.c's own (non-generator-table) entry points --
 * all plain `int fn(TCCIRState *ir)` (barrel_shift_fusion returns void). */
int tcc_ir_opt_add_deref_fold(TCCIRState *ir);
void tcc_ir_barrel_shift_fusion(TCCIRState *ir);
int tcc_ir_opt_shift_pair_to_ubfx(TCCIRState *ir);
int tcc_ir_opt_call_chain_rename(TCCIRState *ir);
int tcc_ir_opt_stackoff_addr_cse(TCCIRState *ir);
int tcc_ir_opt_lea_fold(TCCIRState *ir);
int tcc_ir_opt_lea_rmw_fold(TCCIRState *ir);
int tcc_ir_opt_assign_fuse(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* ------------------------------------------------------------------ helpers */

static IROperand utb_deref_temp(int pos, int btype)
{
  return utb_lval(utb_temp(pos, btype));
}

/* Some generators (deref_indexed, disp_fusion) grow the operand pool
 * (tcc_ir_pool_ensure) and/or allocate a fresh TEMP vreg
 * (tcc_ir_vreg_alloc_temp). utb_new() leaves iroperand_pool_capacity and
 * temporary_variables_live_intervals_size at 0; growing from 0 either hangs
 * (tcc_ir_pool_ensure's `while (cap < needed) cap *= 2` never advances past
 * 0) or silently keeps the backing array zero-sized (same `<<= 1` shape in
 * tcc_ir_vreg_alloc_temp). Pre-allocate generously and tell it how many TEMP
 * positions the test already used by hand, so a freshly allocated vreg can't
 * collide with one of them. See test_opt_licm.c's utb_loop_new() and
 * test_opt_branch_cascade.c's utb_pool_new() for the same pattern. */
static TCCIRState *utb_gens_new(int manual_temp_count)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  ir->temporary_variables_live_intervals_size = 64;
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 64);
  ir->next_temporary_variable = manual_temp_count;
  return ir;
}

/* Run a gens_*_ex pass on a plain TCCIRState* by wrapping it in an IROptCtx,
 * mirroring what tcc_ir_opt_branch_folding/tcc_ir_opt_setif_branch_fuse do
 * internally in ir/opt_branch.c. */
static int run_ctx_pass(TCCIRState *ir, int (*pass_ex)(IROptCtx *ctx))
{
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = pass_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}

/* ============================================================== opt_fusion.c direct-entry passes */

/* Shared builder for the rest of ir/opt_fusion.c's bare-entry passes below.
 * Covers every growth-from-zero hazard exercised by *any* of them in one
 * place (see utb_gens_new()'s comment above for the general shape of the
 * bug class):
 *   - iroperand_pool_capacity      (tcc_ir_pool_add / tcc_ir_pool_ensure)
 *   - temporary_variables_live_intervals[_size] (tcc_ir_vreg_alloc_temp)
 *   - compact_instructions_size     (gsym_cse_insert_before realloc, used by
 *     stackoff_addr_cse)
 *   - next_local_variable / next_parameter (ir_opt_du_build's IR_DU_MODE_FULL,
 *     used by add_deref_fold [TMP_ONLY, doesn't need these two but harmless],
 *     lea_fold, assign_fuse, barrel_shift_fusion)
 *   - max_orig_index                (tcc_ir_barrel_shift_fusion sizes its
 *     barrel_shifts[] side-table from this; orig_index == instruction index
 *     for every hand-built utb_emit() instruction here, so it must be set to
 *     at least the highest instruction index or the pass writes OOB) */
static TCCIRState *utb_fusion_new(int manual_temp_count)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  ir->temporary_variables_live_intervals_size = 64;
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 64);
  ir->next_temporary_variable = manual_temp_count;
  ir->compact_instructions_size = UTB_MAX_INSTR;
  ir->next_local_variable = 8;
  ir->next_parameter = 8;
  ir->max_orig_index = UTB_MAX_INSTR - 1;
  return ir;
}

/* Build a JUMP/JUMPIF target operand the way licm/cfg decode it (matches
 * test_opt_licm.c's utb_jtarget): irop_make_imm32(-1, target, INT32) -- no
 * vreg, imm32 = instruction index. */
static IROperand utb_jtarget_fusion(int target)
{
  return irop_make_imm32(-1, target, I32);
}

/* A vreg-backed STACKOFF operand (`Addr[StackLoc[X]]` for an anonymous temp
 * local, e.g. `&?N`): irop_get_vreg() decodes `vreg` back out (must be <= -2
 * to be "vreg-backed" per lea_cse's own classification -- see its comment).
 * ir_build.h's utb_stackoff() always passes vreg=0 (irop_get_vreg() == -1,
 * "no vreg"), so lea_cse's own target shape needs this dedicated builder. */
static IROperand utb_stackoff_vreg(int32_t vreg, int32_t offset, int btype)
{
  return irop_make_stackoff(vreg, offset, /*is_lval*/ 0, /*is_llocal*/ 0, /*is_param*/ 0, btype);
}

/* ================================================================== bool_simplify */

/* POSITIVE: `a && a` is idempotent -> ASSIGN a. */
UT_TEST(test_bool_simplify_and_self_folds_to_assign)
{
  TCCIRState *ir = utb_gens_new(3);

  int op = utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(2, I32), utb_temp(0, I32), utb_temp(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_bool_ex);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, op), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, op)), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): distinct operands -- not idempotent, kept as BOOL_AND. */
UT_TEST(test_bool_simplify_and_distinct_kept)
{
  TCCIRState *ir = utb_gens_new(3);

  int op = utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_bool_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, op), TCCIR_OP_BOOL_AND);

  utb_free(ir);
  return 0;
}

/* POSITIVE: `a || a` is idempotent -> ASSIGN a (the BOOL_OR gen path). */
UT_TEST(test_bool_simplify_or_self_folds_to_assign)
{
  TCCIRState *ir = utb_gens_new(3);

  int op = utb_emit(ir, TCCIR_OP_BOOL_OR, utb_temp(2, I32), utb_temp(0, I32), utb_temp(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_bool_ex);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, op), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, op)), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: `a && 1` -> ASSIGN a (AND neutral element). */
UT_TEST(test_bool_simplify_and_one_folds_to_assign)
{
  TCCIRState *ir = utb_gens_new(3);

  int op = utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(2, I32), utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_bool_ex);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, op), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, op)), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: `a || 0` -> ASSIGN a (OR neutral element). */
UT_TEST(test_bool_simplify_or_zero_folds_to_assign)
{
  TCCIRState *ir = utb_gens_new(3);

  int op = utb_emit(ir, TCCIR_OP_BOOL_OR, utb_temp(2, I32), utb_temp(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_bool_ex);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, op), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, op)), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: `a && 0` -> ASSIGN 0 (AND annihilator; operands are boolean 0/1). */
UT_TEST(test_bool_simplify_and_zero_folds_to_zero)
{
  TCCIRState *ir = utb_gens_new(3);

  int op = utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(2, I32), utb_temp(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_bool_ex);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, op), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_is_immediate(utb_src1(ir, op)), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, op)), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: `a || 1` -> ASSIGN 1 (OR annihilator). */
UT_TEST(test_bool_simplify_or_one_folds_to_one)
{
  TCCIRState *ir = utb_gens_new(3);

  int op = utb_emit(ir, TCCIR_OP_BOOL_OR, utb_temp(2, I32), utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_bool_ex);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, op), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_is_immediate(utb_src1(ir, op)), 1);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, op)), 1);

  utb_free(ir);
  return 0;
}

/* ================================================================== fusion_mla */

/* POSITIVE: rotate-fusion pattern in the fusion_gens table (the same entry
 * point/table "fusion_mla" registers): (x<<n) | (x>>(32-n)) -> ROR(x, 32-n).
 *   0: T1 = T0 SHL #12
 *   1: T2 = T0 SHR #20     (12+20=32)
 *   2: T3 = T1 OR T2
 */
UT_TEST(test_fusion_mla_rotate_pattern_collapses_to_ror)
{
  TCCIRState *ir = utb_gens_new(4);

  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(12, I32));
  int shr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(0, I32), utb_imm(20, I32));
  int op_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(1, I32), utb_temp(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, shr), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, op_or), TCCIR_OP_ROR);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, op_or)), 0);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, op_or)), 20);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): shift amounts don't sum to 32 -- not a rotate, left alone. */
UT_TEST(test_fusion_mla_non_rotate_shift_sum_kept)
{
  TCCIRState *ir = utb_gens_new(4);

  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(10, I32));
  int shr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(0, I32), utb_imm(15, I32));
  int op_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(1, I32), utb_temp(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_op(ir, shr), TCCIR_OP_SHR);
  UT_ASSERT_EQ(utb_op(ir, op_or), TCCIR_OP_OR);

  utb_free(ir);
  return 0;
}

/* POSITIVE: `mla_fusion` generator in the same fusion_gens table / same
 * "fusion_mla" pass name (a distinct generator from rotate_fusion, keyed off
 * TCCIR_OP_ADD instead of TCCIR_OP_OR) -- MUL feeding ADD's src2 slot fuses
 * to MLA at the MUL's own instruction index; the ADD is NOP'd.
 *   0: T2 = T0 MUL T1        (single use)
 *   1: T4 = T3 ADD T2        (accum = T3, mul on src2 side)
 */
UT_TEST(test_fusion_mla_mul_add_src2_folds_to_mla)
{
  TCCIRState *ir = utb_fusion_new(5);
  tcc_state->opt_mla_fusion = 1;

  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(3, I32), utb_temp(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MLA);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, mul)), 4);   /* final dest = ADD's dest T4 */
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, mul)), 0);   /* MUL operands preserved */
  UT_ASSERT_EQ(utb_vreg_pos(utb_src2(ir, mul)), 1);
  UT_ASSERT_EQ(utb_vreg_pos(utb_op4(ir, mul)), 3);    /* accumulator = T3 */

  tcc_state->opt_mla_fusion = 0;
  utb_free(ir);
  return 0;
}

/* POSITIVE: same shape, but the MUL feeds the ADD's *src1* slot (accum on
 * src2) -- exercises the mirror-image operand-order branch. */
UT_TEST(test_fusion_mla_mul_add_src1_folds_to_mla)
{
  TCCIRState *ir = utb_fusion_new(5);
  tcc_state->opt_mla_fusion = 1;

  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MLA);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_vreg_pos(utb_op4(ir, mul)), 3); /* accumulator = T3 (src2 side this time) */

  tcc_state->opt_mla_fusion = 0;
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a second, independent MUL with the *same* operand pair
 * exists elsewhere in the function -- ir_gen_mla_fusion's dup_mul scan (it
 * conservatively assumes the duplicate might be relied on for CSE/scheduling
 * downstream) must block fusion for both ADDs, leaving all four instructions
 * untouched. */
UT_TEST(test_fusion_mla_duplicate_mul_blocks_fusion)
{
  TCCIRState *ir = utb_fusion_new(7);
  tcc_state->opt_mla_fusion = 1;

  int mul1 = utb_emit(ir, TCCIR_OP_MUL, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int mul2 = utb_emit(ir, TCCIR_OP_MUL, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));
  int add1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, I32), utb_temp(4, I32), utb_temp(2, I32));
  int add2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(6, I32), utb_temp(4, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(6, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, mul1), TCCIR_OP_MUL);
  UT_ASSERT_EQ(utb_op(ir, mul2), TCCIR_OP_MUL);
  UT_ASSERT_EQ(utb_op(ir, add1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, add2), TCCIR_OP_ADD);

  tcc_state->opt_mla_fusion = 0;
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): opt_mla_fusion disabled -- an otherwise-matching
 * MUL+ADD pair must be left completely alone. */
UT_TEST(test_fusion_mla_disabled_flag_keeps_mul_and_add)
{
  TCCIRState *ir = utb_fusion_new(5);
  tcc_state->opt_mla_fusion = 0;

  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(3, I32), utb_temp(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MUL);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* POSITIVE: `indexed_load_fusion` generator (also in fusion_gens, keyed off
 * TCCIR_OP_LOAD) -- an unscaled register-index address (`T2 = T0 ADD T1`,
 * both plain TEMP registers, neither constant) folds a following LOAD
 * through T2 into LOAD_INDEXED[base=T0, index=T1, scale=0]; the ADD is
 * NOP'd. */
UT_TEST(test_fusion_indexed_load_unscaled_register_index_folds)
{
  TCCIRState *ir = utb_fusion_new(3);
  tcc_state->opt_indexed_memory = 1;

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(3, I32), utb_deref_temp(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load)), 0); /* base = T0 */
  IROperand idx_op = ir->iroperand_pool[ir->compact_instructions[load].operand_base + 2];
  UT_ASSERT_EQ(utb_vreg_pos(idx_op), 1);              /* index = T1 (plain register, not scaled) */
  IROperand scale_op = utb_op4(ir, load);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, scale_op), 0);

  tcc_state->opt_indexed_memory = 0;
  utb_free(ir);
  return 0;
}

/* POSITIVE: `indexed_store_fusion` generator, same table -- the mirror-image
 * STORE case: STORE's address operand (dest slot) resolves through an
 * unscaled register-index ADD; folds to STORE_INDEXED[base=T0, index=T1]. */
UT_TEST(test_fusion_indexed_store_unscaled_register_index_folds)
{
  TCCIRState *ir = utb_fusion_new(3);
  tcc_state->opt_indexed_memory = 1;

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(2, I32), utb_temp(3, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE_INDEXED);
  IROperand base_op = ir->iroperand_pool[ir->compact_instructions[store].operand_base + 0];
  IROperand idx_op = ir->iroperand_pool[ir->compact_instructions[store].operand_base + 2];
  UT_ASSERT_EQ(utb_vreg_pos(base_op), 0); /* base = T0 */
  UT_ASSERT_EQ(utb_vreg_pos(idx_op), 1);  /* index = T1 */
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, store)), 3); /* stored value = T3, unchanged */

  tcc_state->opt_indexed_memory = 0;
  utb_free(ir);
  return 0;
}

/* POSITIVE: `indexed_load_fusion`'s *scaled* addressing sub-path -- a
 * `T1 = T0 SHL #2` (scale=2, single use) feeding `T2 = T5 ADD T1` (single
 * use) whose result is dereferenced by a LOAD folds to
 * LOAD_INDEXED[base=T5, index=T0, scale=2]; both the SHL and the ADD are
 * NOP'd (distinguishing this from the unscaled path, which only NOPs the
 * ADD). */
UT_TEST(test_fusion_indexed_load_scaled_index_folds)
{
  TCCIRState *ir = utb_fusion_new(6);
  tcc_state->opt_indexed_memory = 1;

  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(2, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(5, I32), utb_temp(1, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(3, I32), utb_deref_temp(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load)), 5); /* base = T5 */
  IROperand idx_op = ir->iroperand_pool[ir->compact_instructions[load].operand_base + 2];
  UT_ASSERT_EQ(utb_vreg_pos(idx_op), 0);              /* index = T0 */
  IROperand scale_op = utb_op4(ir, load);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, scale_op), 2);

  tcc_state->opt_indexed_memory = 0;
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): one ADD operand is an immediate -- that shape belongs to
 * disp_fusion (base + #imm), not the register-index path, so
 * indexed_load_fusion's `add_src1.is_const || add_src2.is_const` guard must
 * refuse it and leave the ADD + LOAD untouched. */
UT_TEST(test_fusion_indexed_load_const_operand_kept_for_disp_fusion)
{
  TCCIRState *ir = utb_fusion_new(3);
  tcc_state->opt_indexed_memory = 1;

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_imm(8, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(3, I32), utb_deref_temp(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  tcc_state->opt_indexed_memory = 0;
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the LOAD's address vreg is VAR-typed (a named local
 * pointer variable, not a TEMP) -- ir_gen_indexed_memory_fusion explicitly
 * refuses to fold loads through a VAR base (line: `if (!is_store &&
 * TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR) return 0;`), so
 * the ADD + LOAD must be left untouched even though the register-index shape
 * otherwise matches. */
UT_TEST(test_fusion_indexed_load_var_base_kept)
{
  TCCIRState *ir = utb_fusion_new(3);
  tcc_state->opt_indexed_memory = 1;

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_var(2, I32), utb_temp(0, I32), utb_temp(1, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(3, I32), utb_lval(utb_var(2, I32)), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_fusion_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  tcc_state->opt_indexed_memory = 0;
  utb_free(ir);
  return 0;
}

/* ================================================================== deref_indexed */

/* POSITIVE: an ADD-scaled-index deref folds into LOAD_INDEXED with a fresh
 * result temp:
 *   0: T0 = ASSIGN #3            (index)
 *   1: T1 = T0 SHL #2             (scale=2, single use)
 *   2: T5 = ASSIGN #100           (fake base pointer)
 *   3: T2 = T5 ADD T1             (single use)
 *   4: T3 = T2***DEREF*** ADD #5   (trigger: any op w/ a genuine pointer deref)
 */
UT_TEST(test_deref_indexed_scaled_add_folds_to_load_indexed)
{
  TCCIRState *ir = utb_gens_new(6); /* T0..T5 used by hand; fresh alloc starts at T6 */
  tcc_state->opt_indexed_memory = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(3, I32), UTB_NONE);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(100, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(5, I32), utb_temp(1, I32));
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_deref_temp(2, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_deref_indexed_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, add)), 5); /* base = T5 */
  IROperand new_src1 = utb_src1(ir, use);
  UT_ASSERT(!new_src1.is_lval); /* deref replaced by the loaded value */

  tcc_state->opt_indexed_memory = 0;
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): opt_indexed_memory disabled -- the pass must no-op
 * entirely, even given an otherwise-matching shape. */
UT_TEST(test_deref_indexed_disabled_flag_keeps_deref)
{
  TCCIRState *ir = utb_gens_new(6);
  tcc_state->opt_indexed_memory = 0;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(3, I32), UTB_NONE);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(100, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(5, I32), utb_temp(1, I32));
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_deref_temp(2, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_deref_indexed_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT(utb_src1(ir, use).is_lval);

  utb_free(ir);
  return 0;
}

/* ================================================================== disp_fusion */

/* POSITIVE: LOAD through `base + #imm` (imm in Thumb-2 ldr range) folds into
 * LOAD_INDEXED, NOPing the ADD. */
UT_TEST(test_disp_fusion_load_const_offset_folds)
{
  TCCIRState *ir = utb_gens_new(3);
  tcc_state->opt_disp_fusion = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(8, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_deref_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_disp_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load)), 0); /* base = T0 */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, load)), 8);

  tcc_state->opt_disp_fusion = 0;
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): displacement exceeds the Thumb-2 ldr immediate range --
 * left as a separate ADD + deref. */
UT_TEST(test_disp_fusion_out_of_range_offset_kept)
{
  TCCIRState *ir = utb_gens_new(3);
  tcc_state->opt_disp_fusion = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5000, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_deref_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_disp_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  tcc_state->opt_disp_fusion = 0;
  utb_free(ir);
  return 0;
}

/* POSITIVE: a SUB-immediate address (`*(p - 3)`) folds into a negative indexed
 * displacement LOAD_INDEXED[base, #-imm] instead of leaving a separate SUB. */
UT_TEST(test_disp_fusion_sub_offset_folds_negative)
{
  TCCIRState *ir = utb_gens_new(3);
  tcc_state->opt_disp_fusion = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int sub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_temp(0, I32), utb_imm(12, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_deref_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_disp_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, sub), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load)), 0);                /* base = T0 */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, load)), -12); /* negated SUB imm */

  tcc_state->opt_disp_fusion = 0;
  utb_free(ir);
  return 0;
}

/* POSITIVE: STORE through `base + #imm` folds into STORE_INDEXED[base, #imm],
 * carrying the stored value across into the new value slot and NOPing the ADD. */
UT_TEST(test_disp_fusion_store_const_offset_folds)
{
  TCCIRState *ir = utb_gens_new(3);
  tcc_state->opt_disp_fusion = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(8, I32));
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(1, I32), utb_temp(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_disp_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE_INDEXED);
  IROperand base_op = ir->iroperand_pool[ir->compact_instructions[store].operand_base + 0];
  IROperand val_op = ir->iroperand_pool[ir->compact_instructions[store].operand_base + 1];
  IROperand idx_op = ir->iroperand_pool[ir->compact_instructions[store].operand_base + 2];
  UT_ASSERT_EQ(utb_vreg_pos(base_op), 0);                  /* base = T0 */
  UT_ASSERT_EQ(utb_vreg_pos(val_op), 2);                   /* stored value = T2 */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, idx_op), 8);

  tcc_state->opt_disp_fusion = 0;
  utb_free(ir);
  return 0;
}

/* POSITIVE: an ASSIGN whose source is an lval deref of `base + #imm` is treated
 * as a load and folds to LOAD_INDEXED[base, #imm], NOPing the ADD. */
UT_TEST(test_disp_fusion_assign_load_const_offset_folds)
{
  TCCIRState *ir = utb_gens_new(3);
  tcc_state->opt_disp_fusion = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(8, I32));
  int assign = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_deref_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_disp_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, assign), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, assign)), 0); /* base = T0 */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, assign)), 8);

  tcc_state->opt_disp_fusion = 0;
  utb_free(ir);
  return 0;
}

/* ================================================================== chain_fold */

/* POSITIVE: LOAD_INDEXED[base, #imm2] whose base resolves to `new_base + #imm1`
 * merges into LOAD_INDEXED[new_base, #(imm1+imm2)], NOPing the ADD. */
UT_TEST(test_chain_fold_merges_chained_add_into_indexed_offset)
{
  TCCIRState *ir = utb_gens_new(3);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int load = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32), utb_temp(1, I32), utb_imm(8, I32),
                        utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_chain_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load)), 0); /* base = T0 */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, load)), 12); /* 4+8 */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): merged displacement exceeds the ldr immediate range --
 * the chain is left unmerged. */
UT_TEST(test_chain_fold_out_of_range_total_kept)
{
  TCCIRState *ir = utb_gens_new(3);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4090, I32));
  int load = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32), utb_temp(1, I32), utb_imm(8, I32),
                        utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_chain_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load)), 1); /* base still T1 */

  utb_free(ir);
  return 0;
}

/* POSITIVE: the STORE_INDEXED mirror of the merge above -- ir_gen_indexed_chain
 * dispatches on TCCIR_OP_STORE_INDEXED too (fusion_chain_gens' second entry),
 * with `base_slot = 0` (base lives in the dest slot for stores, value in
 * src1) instead of LOAD_INDEXED's `base_slot = 1`; exercises that branch. */
UT_TEST(test_chain_fold_merges_chained_add_into_store_indexed_offset)
{
  TCCIRState *ir = utb_gens_new(3);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int store = utb_emit4(ir, TCCIR_OP_STORE_INDEXED, utb_temp(1, I32), utb_temp(2, I32), utb_imm(8, I32),
                         utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_chain_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE_INDEXED);
  IROperand new_base = ir->iroperand_pool[ir->compact_instructions[store].operand_base + 0];
  IROperand new_index = ir->iroperand_pool[ir->compact_instructions[store].operand_base + 2];
  UT_ASSERT_EQ(utb_vreg_pos(new_base), 0);                          /* base = T0 */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, new_index), 12);          /* 4+8 */
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, store)), 2);               /* value operand untouched */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the ADD base feeds two indexed accesses -- folding it into
 * one displacement would strand the other use, so ir_opt_du_uses(base) != 1
 * blocks the merge and the ADD is kept. */
UT_TEST(test_chain_fold_multi_use_base_kept)
{
  TCCIRState *ir = utb_gens_new(4);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int load1 = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32), utb_temp(1, I32), utb_imm(8, I32),
                        utb_imm(0, I32));
  utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(3, I32), utb_temp(1, I32), utb_imm(12, I32),
            utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_chain_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load1)), 1); /* base still T1 */

  utb_free(ir);
  return 0;
}

/* POSITIVE: a SUB producer folds too -- base = new_base - #imm1 merges into
 * LOAD_INDEXED[new_base, #(imm2 - imm1)] (the SUB immediate is negated). */
UT_TEST(test_chain_fold_sub_producer_folds_negated)
{
  TCCIRState *ir = utb_gens_new(3);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int sub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int load = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32), utb_temp(1, I32), utb_imm(8, I32),
                       utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_chain_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, sub), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load)), 0);              /* base = T0 */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, load)), 4); /* 8 + (-4) */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the base is defined by a producer that is neither ADD nor
 * SUB (here MUL), so the `base +/- K` shape doesn't hold and the access is kept. */
UT_TEST(test_chain_fold_non_add_sub_producer_kept)
{
  TCCIRState *ir = utb_gens_new(3);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1000, I32), UTB_NONE);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int load = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32), utb_temp(1, I32), utb_imm(8, I32),
                       utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_chain_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MUL);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load)), 1); /* base still T1 */

  utb_free(ir);
  return 0;
}

/* ================================================================== pair_reorder */

/* POSITIVE: two LOAD_INDEXED ops on the same base at adjacent 4-byte-aligned
 * offsets, separated by one safe instruction, get reordered adjacent to each
 * other. */
UT_TEST(test_pair_reorder_adjacent_indexed_loads_move_together)
{
  TCCIRState *ir = utb_gens_new(6);

  int load1 = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0, I32),
                         utb_imm(0, I32));
  int mid = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(99, I32), UTB_NONE);
  int load2 = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32), utb_temp(0, I32), utb_imm(4, I32),
                         utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_pair_reorder_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, load1), TCCIR_OP_LOAD_INDEXED); /* stays put */
  /* load2 swapped into the slot right after load1; mid pushed out to load2's
   * old slot. */
  UT_ASSERT_EQ(utb_op(ir, mid), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, mid)), 2);
  UT_ASSERT_EQ(utb_op(ir, load2), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): offsets are not adjacent (differ by 8, not 4) -- no
 * reorder. */
UT_TEST(test_pair_reorder_non_adjacent_offsets_kept)
{
  TCCIRState *ir = utb_gens_new(6);

  int load1 = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0, I32),
                         utb_imm(0, I32));
  int mid = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(99, I32), UTB_NONE);
  int load2 = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32), utb_temp(0, I32), utb_imm(8, I32),
                         utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_pair_reorder_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, load1), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_op(ir, mid), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, load2), TCCIR_OP_LOAD_INDEXED);

  utb_free(ir);
  return 0;
}

/* POSITIVE: the STORE_INDEXED mirror -- ir_gen_indexed_pair_reorder's
 * `q1_is_load` branch flips `q1_base_slot`/`q3_dv` slot selection (base at
 * slot 0, the stored *value* -- not a result temp -- read from slot 1) for
 * stores; exercises that branch pairing two adjacent-offset STORE_INDEXED
 * ops on the same base. */
UT_TEST(test_pair_reorder_adjacent_indexed_stores_move_together)
{
  TCCIRState *ir = utb_gens_new(6);

  int store1 = utb_emit4(ir, TCCIR_OP_STORE_INDEXED, utb_temp(0, I32), utb_temp(1, I32), utb_imm(0, I32),
                          utb_imm(0, I32));
  int mid = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(99, I32), UTB_NONE);
  int store2 = utb_emit4(ir, TCCIR_OP_STORE_INDEXED, utb_temp(0, I32), utb_temp(2, I32), utb_imm(4, I32),
                          utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_pair_reorder_ex);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, store1), TCCIR_OP_STORE_INDEXED); /* stays put */
  UT_ASSERT_EQ(utb_op(ir, mid), TCCIR_OP_STORE_INDEXED);    /* store2 swapped into this slot */
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, mid)), 2);         /* carries store2's value operand */
  UT_ASSERT_EQ(utb_op(ir, store2), TCCIR_OP_ASSIGN);        /* mid pushed out here */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): q3 is found (adjacent offsets) but the intervening
 * ASSIGN reads the second load's own result vreg -- swapping it past that
 * read would use the value before it is defined, so the swap loop's
 * conflict check must stop immediately (swap_pos stays at q3_idx) and the
 * pass reports no change, distinct from the "q3 never found" negative
 * above. */
UT_TEST(test_pair_reorder_raw_hazard_blocks_swap)
{
  TCCIRState *ir = utb_gens_new(7);

  int load1 = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0, I32),
                         utb_imm(0, I32));
  /* mid reads T2 -- load2's own destination -- before load2 (re)defines it. */
  int mid = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(6, I32), utb_temp(2, I32), UTB_NONE);
  int load2 = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32), utb_temp(0, I32), utb_imm(4, I32),
                         utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(6, I32), UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_pair_reorder_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, load1), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_op(ir, mid), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, load2), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, mid)), 2); /* order/content unchanged */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (correctness guard): the intervening non-lval ASSIGN *defines* the
 * second store's value operand (T2). Hoisting store2 above it would store an
 * undefined T2 -- a RAW hazard on the store's data input. The conflict check
 * keys on the stored value being *produced* by an instruction in the range,
 * distinct from the load case where the hazard is on the loaded *result*. */
UT_TEST(test_pair_reorder_store_value_raw_hazard_blocks_swap)
{
  TCCIRState *ir = utb_gens_new(6);

  int store1 = utb_emit4(ir, TCCIR_OP_STORE_INDEXED, utb_temp(0, I32), utb_temp(1, I32), utb_imm(0, I32),
                          utb_imm(0, I32));
  int mid = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(7, I32), UTB_NONE);
  int store2 = utb_emit4(ir, TCCIR_OP_STORE_INDEXED, utb_temp(0, I32), utb_temp(2, I32), utb_imm(4, I32),
                          utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = run_ctx_pass(ir, tcc_ir_opt_gens_pair_reorder_ex);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store1), TCCIR_OP_STORE_INDEXED);
  UT_ASSERT_EQ(utb_op(ir, mid), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, store2), TCCIR_OP_STORE_INDEXED);

  utb_free(ir);
  return 0;
}

/* ================================================================== add_deref_fold */

/* POSITIVE: `T1 = P0 ADD #8` (single use) whose only use is a same-block
 * deref (`T1***DEREF*** ADD #1`) folds to LOAD_INDEXED[P0, #8]; the deref
 * flag on the consumer's src1 clears (the value is now loaded directly). */
UT_TEST(test_add_deref_fold_param_base_folds_to_load_indexed)
{
  TCCIRState *ir = utb_fusion_new(2);

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_param(0, I32), utb_imm(8, I32));
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp(1, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_add_deref_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, add)), 0); /* base = P0 */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, add)), 8);
  UT_ASSERT(!utb_src1(ir, use).is_lval); /* deref cleared on the consumer */

  utb_free(ir);
  return 0;
}

/* POSITIVE (peep-through): base is a TEMP whose immediately-preceding def is
 * a plain `ASSIGN T0 <- P0` copy -- the PARAM behind the copy is used as the
 * effective base, same fold as the direct-PARAM case.  The ASSIGN copy
 * itself is left in place (only DCE would remove it, not this pass). */
UT_TEST(test_add_deref_fold_peep_through_assign_from_param)
{
  TCCIRState *ir = utb_fusion_new(3);

  int cp = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp(1, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_add_deref_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, cp), TCCIR_OP_ASSIGN); /* copy left intact */
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, add)), 0); /* base rewritten to P0 */
  UT_ASSERT(!utb_src1(ir, use).is_lval);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): base is a plain TEMP with no PARAM ancestor in the
 * backward-scan window -- the pass must never fold a non-PARAM-rooted base
 * (stack loads could then be exposed to unsafe cross-call const-prop). */
UT_TEST(test_add_deref_fold_non_param_base_kept)
{
  TCCIRState *ir = utb_fusion_new(3);

  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_deref_temp(9, I32), UTB_NONE); /* T0 <- some load, not PARAM */
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp(1, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_add_deref_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT(utb_src1(ir, use).is_lval);

  utb_free(ir);
  return 0;
}

/* ================================================================== barrel_shift_fusion */

/* POSITIVE: a single-use `T1 = T0 SHR #3` feeding `T2 = T5 SUB T1` (SUB only
 * tries the src2 slot, attempt=0) folds into the barrel shifter: the SHR
 * becomes NOP, the SUB's src2 is rewritten to the shift's own source (T0),
 * and ir->barrel_shifts[] records (type=SHR=2, amount=3) at the SUB's
 * orig_index.  tcc_ir_barrel_shift_fusion returns void -- the side-table and
 * the NOP'd SHL/SHR are the only observable effects. */
UT_TEST(test_barrel_shift_fusion_shr_folds_into_sub)
{
  TCCIRState *ir = utb_fusion_new(6);

  int shr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  int sub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_temp(5, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  tcc_ir_barrel_shift_fusion(ir);

  UT_ASSERT_EQ(utb_op(ir, shr), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, sub), TCCIR_OP_SUB); /* consumer op unchanged */
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, sub)), 5);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src2(ir, sub)), 0); /* src2 rewritten to the shift's own source T0 */
  UT_ASSERT(ir->barrel_shifts != NULL);
  UT_ASSERT_EQ((int)ir->barrel_shifts[sub], (2 << 5) | 3); /* stype=SHR(2), amount=3 */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the shift result has a second use elsewhere -- folding
 * it into the barrel shifter would silently drop that other use's value, so
 * the pass's ir_opt_du_uses()==1 guard must keep the SHR (and the SUB's
 * src2) untouched. */
UT_TEST(test_barrel_shift_fusion_multi_use_shift_kept)
{
  TCCIRState *ir = utb_fusion_new(6);

  int shr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  int sub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(2, I32), utb_temp(5, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE); /* extra use of T1 */

  tcc_ir_barrel_shift_fusion(ir);

  UT_ASSERT_EQ(utb_op(ir, shr), TCCIR_OP_SHR);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src2(ir, sub)), 1); /* still reads T1 directly */

  utb_free(ir);
  return 0;
}

/* ================================================================== shift_pair_to_ubfx */

/* POSITIVE: `(x << 4) >> 10` (a=4 <= b=10, both in [1,31], SHL single-use,
 * same block, T0 unmodified between them) folds to UBFX with lsb=(b-a)=6,
 * width=(32-b)=22; the SHL is NOP'd. */
UT_TEST(test_shift_pair_to_ubfx_folds_shl_shr_pair)
{
  TCCIRState *ir = utb_fusion_new(3);

  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int shr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32), utb_imm(10, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_shift_pair_to_ubfx(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, shr), TCCIR_OP_UBFX);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, shr)), 0); /* recomputed from T0 directly */
  int32_t param = (int32_t)irop_get_imm64_ex(ir, utb_src2(ir, shr));
  UT_ASSERT_EQ(param & 0x1F, 6);         /* lsb = b - a = 10 - 4 */
  UT_ASSERT_EQ((param >> 5) & 0x1F, 22); /* width = 32 - b = 32 - 10 */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a > b (14 > 10) -- not a valid logical bitfield extract
 * (would require a negative shift), so the pair is left alone. */
UT_TEST(test_shift_pair_to_ubfx_a_greater_than_b_kept)
{
  TCCIRState *ir = utb_fusion_new(3);

  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(14, I32));
  int shr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32), utb_imm(10, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_shift_pair_to_ubfx(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_op(ir, shr), TCCIR_OP_SHR);

  utb_free(ir);
  return 0;
}

/* ================================================================== call_chain_rename */

/* POSITIVE: `V0 = CALL f() [FUNCCALLVAL]; PARAMVAL[0] V0; V0 = CALL g()
 * [redef, no intervening read]` renames V0 at just the CALL.dest and
 * PARAMVAL.src1 pair to a fresh TEMP, leaving V0's other def (the second
 * CALL) untouched. */
UT_TEST(test_call_chain_rename_renames_call_to_paramval_pair)
{
  TCCIRState *ir = utb_fusion_new(2);

  int call1 = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_var(0, I32), utb_imm(0, I32), utb_imm(0, I32));
  int pv = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_var(0, I32),
                     utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));
  int call2 = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_var(0, I32), utb_imm(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_var(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_call_chain_rename(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, call1), TCCIR_OP_FUNCCALLVAL);
  int renamed_vr = utb_vreg(utb_dest(ir, call1));
  UT_ASSERT(TCCIR_DECODE_VREG_TYPE(renamed_vr) == TCCIR_VREG_TYPE_TEMP); /* fresh TEMP, not V0 */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, pv)), renamed_vr);                  /* PARAMVAL rewritten to match */
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, call2)), 0);                    /* second CALL's V0 def untouched */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): V0 is read (via RETURNVALUE) before being redefined --
 * the rename is unsafe (an external reader still expects V0's original
 * identity across the whole segment), so nothing changes. */
UT_TEST(test_call_chain_rename_read_before_redef_kept)
{
  TCCIRState *ir = utb_fusion_new(2);

  int call1 = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_var(0, I32), utb_imm(0, I32), utb_imm(0, I32));
  int pv = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_var(0, I32),
                     utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_var(0, I32), UTB_NONE); /* read before any redef */

  int changes = tcc_ir_opt_call_chain_rename(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, call1)), 0);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, pv)), 0);

  utb_free(ir);
  return 0;
}

/* ================================================================== stackoff_addr_cse */

/* POSITIVE: the same StackLoc literal offset appears as the literal side of
 * two different `Addr[StackLoc[X]] ADD <reg-index>` ADDs -- both get
 * rewritten to read a single hoisted TEMP (inserted as an ASSIGN at index
 * 0), and IR length grows by exactly one instruction. */
UT_TEST(test_stackoff_addr_cse_hoists_repeated_offset)
{
  TCCIRState *ir = utb_fusion_new(4);

  IROperand slot = utb_stackoff(24, 0, 0, 0, I32); /* Addr[StackLoc[24]], is_lval=0 */
  int add1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), slot, utb_temp(0, I32));
  int add2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), slot, utb_temp(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int n_before = ir->next_instruction_index;

  int changes = tcc_ir_opt_stackoff_addr_cse(ir);

  UT_ASSERT_EQ(changes, 2); /* both ADD uses rewritten */
  UT_ASSERT_EQ(ir->next_instruction_index, n_before + 1); /* one hoisted ASSIGN inserted */
  /* Everything shifted by 1: the two ADDs (originally add1/add2) are now at add1+1/add2+1. */
  IROperand new_src1_a = utb_src1(ir, add1 + 1);
  IROperand new_src1_b = utb_src1(ir, add2 + 1);
  UT_ASSERT_EQ(irop_get_tag(new_src1_a), IROP_TAG_VREG);
  UT_ASSERT_EQ(irop_get_tag(new_src1_b), IROP_TAG_VREG);
  UT_ASSERT(TCCIR_DECODE_VREG_TYPE(utb_vreg(new_src1_a)) == TCCIR_VREG_TYPE_TEMP);
  UT_ASSERT_EQ(utb_vreg(new_src1_a), utb_vreg(new_src1_b)); /* same hoisted TEMP */
  /* The hoisted ASSIGN is at index 0 and reproduces the original StackLoc literal. */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, 0)), IROP_TAG_STACKOFF);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the StackLoc literal appears only once -- below the
 * count>=2 threshold, so no hoist happens and the IR is untouched. */
UT_TEST(test_stackoff_addr_cse_single_use_kept)
{
  TCCIRState *ir = utb_fusion_new(4);

  IROperand slot = utb_stackoff(24, 0, 0, 0, I32);
  int add1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), slot, utb_temp(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int n_before = ir->next_instruction_index;

  int changes = tcc_ir_opt_stackoff_addr_cse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(ir->next_instruction_index, n_before);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, add1)), IROP_TAG_STACKOFF);

  utb_free(ir);
  return 0;
}

/* ================================================================== lea_fold */

/* POSITIVE (Pattern A): `T1 = LEA Addr[StackLoc[16]]` (vreg=-1, the "plain"
 * non-vreg-backed shape lea_cse deliberately skips) whose single use is a
 * same-block deref in a CMP folds to a direct StackLoc access; the LEA is
 * NOP'd.  CMP (not ADD) is used as the consumer so the optional ADD-K
 * interposer branch never triggers, keeping this test to the simple single-
 * use substitution path. */
UT_TEST(test_lea_fold_single_deref_use_folds_to_stackloc)
{
  TCCIRState *ir = utb_fusion_new(3);

  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_stackoff(16, 0, 0, 0, I32), UTB_NONE);
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_deref_temp(1, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_lea_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_NOP);
  IROperand new_src1 = utb_src1(ir, cmp);
  UT_ASSERT_EQ(irop_get_tag(new_src1), IROP_TAG_STACKOFF);
  UT_ASSERT(new_src1.is_lval); /* still a direct-load deref, just no LEA */
  UT_ASSERT_EQ(irop_get_stack_offset(new_src1), 16);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the LEA result has two uses -- the single-use
 * precondition fails, so the LEA and both CMPs are left untouched. */
UT_TEST(test_lea_fold_multi_use_kept)
{
  TCCIRState *ir = utb_fusion_new(3);

  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_stackoff(16, 0, 0, 0, I32), UTB_NONE);
  int cmp1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_deref_temp(1, I32), utb_imm(5, I32));
  int cmp2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_deref_temp(1, I32), utb_imm(6, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_lea_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_LEA);
  UT_ASSERT(utb_src1(ir, cmp1).is_lval);
  UT_ASSERT(utb_src1(ir, cmp2).is_lval);

  utb_free(ir);
  return 0;
}

/* ================================================================== lea_rmw_fold */

/* POSITIVE: the `u.field++` read-modify-write idiom -- a plain LEA whose
 * every use in the function is a same-block 8-byte (long long) deref (one
 * LOAD, one STORE, both through the LEA's TEMP) folds both derefs to direct
 * StackLoc accesses at the LEA's offset and NOPs the LEA.  (lea_fold's
 * single-use precondition would refuse this shape outright.)
 *
 *   0: T1 = LEA Addr[StackLoc[32]]   (INT64 field address)
 *   1: T2 = T1***DEREF***            (LOAD, INT64)
 *   2: T3 = T2 ADD #1                (plain arithmetic RMW, not OR/AND)
 *   3: T1***DEREF*** = T3            (STORE, INT64)
 */
UT_TEST(test_lea_rmw_fold_load_add_store_folds_both_derefs)
{
  TCCIRState *ir = utb_fusion_new(4);

  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_stackoff(32, 0, 0, 0, I64), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I64), utb_deref_temp(1, I64), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I64), utb_temp(2, I64), utb_imm(1, I64));
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(1, I64), utb_temp(3, I64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_lea_rmw_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);
  IROperand load_src1 = utb_src1(ir, load);
  UT_ASSERT_EQ(irop_get_tag(load_src1), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_stack_offset(load_src1), 32);
  IROperand store_dest = utb_dest(ir, store);
  UT_ASSERT_EQ(irop_get_tag(store_dest), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_stack_offset(store_dest), 32);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD); /* arithmetic RMW body untouched */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a bitfield write-back idiom -- the STORE's value is
 * defined by an OR that itself consumes a load of the same slot (the masked
 * merge `(load & ~mask) | bits`).  lea_rmw_fold must conservatively leave
 * this as an LEA-deref (folding to a direct StackLoc store would make later
 * DSE treat it as a clean full-word overwrite and drop the merge). */
UT_TEST(test_lea_rmw_fold_bitfield_or_writeback_kept)
{
  TCCIRState *ir = utb_fusion_new(4);

  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_stackoff(32, 0, 0, 0, I64), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I64), utb_deref_temp(1, I64), UTB_NONE);
  int orv = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I64), utb_temp(2, I64), utb_imm(1, I64));
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(1, I64), utb_temp(3, I64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_lea_rmw_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_LEA);
  UT_ASSERT(utb_src1(ir, load).is_lval);
  UT_ASSERT(utb_dest(ir, store).is_lval);
  UT_ASSERT_EQ(utb_op(ir, orv), TCCIR_OP_OR);

  utb_free(ir);
  return 0;
}

/* ================================================================== assign_fuse */

/* POSITIVE: a single-use, single-def TEMP produced by a plain register-write
 * op (ADD) and immediately copied by an ASSIGN in the same block fuses: the
 * producer's dest becomes the ASSIGN's dest directly, and the ASSIGN is
 * NOP'd. */
UT_TEST(test_assign_fuse_producer_dest_absorbs_assign)
{
  TCCIRState *ir = utb_fusion_new(3);

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));
  int asn = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_assign_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, add)), 2); /* producer now writes directly to T2 */
  UT_ASSERT_EQ(utb_op(ir, asn), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the producer is a FUNCCALLVAL -- its dest lands in a
 * fixed ABI register, so redirecting it to the ASSIGN's dest is unsafe.
 * The pass's producer-op denylist must keep both instructions intact. */
UT_TEST(test_assign_fuse_call_producer_kept)
{
  TCCIRState *ir = utb_fusion_new(3);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), utb_imm(0, I32), utb_imm(0, I32));
  int asn = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_assign_fuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(ir, call)), 1); /* unchanged */
  UT_ASSERT_EQ(utb_op(ir, asn), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

UT_COVERS("bool_simplify");
UT_COVERS("fusion_mla");
UT_COVERS("deref_indexed");
UT_COVERS("disp_fusion");
UT_COVERS("chain_fold");
UT_COVERS("pair_reorder");
