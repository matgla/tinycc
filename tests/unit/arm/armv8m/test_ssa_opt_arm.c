/*
 *  test_ssa_opt_arm.c - suite for arch/arm/ssa_opt_arm.c (ARM SSA-level
 *  target-specific peephole fusion generators)
 *
 *  Each ssa_gen_arm_* function is a `int fn(IRSSAOptCtx *ctx, int instr_idx)`
 *  generator invoked by the generic SSA-opt engine (ir/opt/ssa_opt.c) on one
 *  instruction at a time, matched by opcode via the table at the bottom of
 *  ssa_opt_arm.c. That driver (and the full ssa/cfg/dominator construction
 *  it needs) is NOT linked into this isolated harness -- see
 *  test_metamorphic_ssa.c's file header. Instead, each test here builds a
 *  tiny straight-line instruction sequence by hand (ir_build.h) and a
 *  matching IRSSAVregInfo def/use chain by hand (mirroring what
 *  ssa_opt_build_chains() would compute for that snippet), then calls the
 *  generator directly and asserts on the resulting IR shape.
 *
 *  ra_link_stubs.c (also under tests/unit/arm/armv8m/, so editable per this
 *  task's ground rules) supplies the real, non-stub semantics for
 *  ssa_opt_vinfo / ssa_opt_add_use_instr / ssa_opt_remove_use_instr /
 *  ssa_opt_nop_instr / tcc_ir_ssa_opt_init / tcc_ir_ssa_opt_free needed to
 *  drive these generators; before this change ssa_opt_vinfo unconditionally
 *  returned NULL, which made every ssa_gen_arm_* function bail out on its
 *  first line (0% coverage was structural, not just "no tests written yet").
 *  The other individual ssa_opt_<pass> stubs (cprop/dce/sccp/...) remain
 *  no-ops; nothing here calls them.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License.
 */

#include "ir_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* ---- entry points under test ------------------------------------------- */
/* Declared in ssa_opt_arm.h: */
int ssa_gen_arm_fuse_mul_add_to_mla(IRSSAOptCtx *ctx, int instr_idx);
int ssa_gen_arm_fuse_shl_add_to_load_indexed(IRSSAOptCtx *ctx, int instr_idx);
int ssa_gen_arm_fuse_shl_add_to_store_indexed(IRSSAOptCtx *ctx, int instr_idx);
int ssa_gen_arm_reduce_mul_to_shift(IRSSAOptCtx *ctx, int instr_idx);
int ssa_gen_arm_fuse_load_through_add_imm(IRSSAOptCtx *ctx, int instr_idx);
int ssa_gen_arm_fuse_store_through_add_imm(IRSSAOptCtx *ctx, int instr_idx);
/* Not exposed via ssa_opt_arm.h (only reachable through the static
 * dispatchers in production), but still exported C symbols -- forward
 * declare them here, matching how other UT suites reach non-public-header
 * pass entry points (e.g. test_opt_loop_dead.c). */
int ssa_gen_arm_fuse_mla_accum_through_add_imm(IRSSAOptCtx *ctx, int instr_idx);
int ssa_gen_arm_fuse_store_src_through_add_imm(IRSSAOptCtx *ctx, int instr_idx);

/* ---- IR construction helpers -------------------------------------------- */

/* Generous fixed cap for vinfo[] (indexed by TEMP vreg *position*, see
 * ssa_opt_vinfo/tcc_ir_ssa_opt_init in ra_link_stubs.c). Every test's TEMP
 * positions stay well under this, so tests don't need to precisely track
 * "highest TEMP position used + 1" -- an off-by-one there would silently
 * turn ssa_opt_vinfo() into a NULL-returning stub again for the missed
 * position (exactly the bug this whole suite exists to avoid tripping over). */
#define UTB_SSA_MAX_TEMPS 64

/* utb_new() leaves iroperand_pool_capacity/temporary_variables_live_intervals
 * at 0; the fusion generators grow the operand pool via tcc_ir_pool_add(),
 * which hangs/no-ops growing from a zero capacity (see test_opt_fusion.c's
 * utb_fusion_new() comment for the same class of hazard). Pre-allocate
 * generously. manual_temp_count is accepted for readability at call sites
 * documenting how many TEMPs a test *intends* to use, but next_temporary_
 * variable is always set to UTB_SSA_MAX_TEMPS so vinfo[] sizing (driven by
 * this field, see tcc_ir_ssa_opt_init) can't fall short of a test's actual
 * highest TEMP position. */
static TCCIRState *utb_ssa_new(int manual_temp_count)
{
  (void)manual_temp_count;
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  ir->next_temporary_variable = UTB_SSA_MAX_TEMPS;
  ir->max_orig_index = UTB_MAX_INSTR - 1;
  return ir;
}

/* A tiny hand-rolled IRSSAOptCtx: vinfo[] is sized/zeroed like
 * tcc_ir_ssa_opt_init would (see ra_link_stubs.c), but def/use chains are
 * populated by each test to match the exact snippet it built -- there is no
 * ssa/cfg construction here, only straight-line IR. */
static void utb_ssa_ctx_init(IRSSAOptCtx *ctx, TCCIRState *ir)
{
  tcc_ir_ssa_opt_init(ctx, ir, NULL, NULL);
}

static void utb_ssa_ctx_free(IRSSAOptCtx *ctx)
{
  tcc_ir_ssa_opt_free(ctx);
}

/* Record that `def_idx` defines vreg `vr` (single definition, SSA-style).
 * Aborts loudly (rather than silently no-op-ing) if `vr` doesn't map to a
 * TEMP vinfo slot -- a helper-usage bug in the test itself, not something a
 * generator-under-test could ever trigger. */
static void utb_def(IRSSAOptCtx *ctx, int32_t vr, int def_idx)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi) {
    fprintf(stderr, "utb_def: vr %d has no vinfo slot (not a TEMP vreg?)\n", vr);
    abort();
  }
  vi->def_instr = def_idx;
  vi->def_count = 1;
}

/* Record that instruction `use_idx` uses vreg `vr`. */
static void utb_use(IRSSAOptCtx *ctx, int32_t vr, int use_idx)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi) {
    fprintf(stderr, "utb_use: vr %d has no vinfo slot (not a TEMP vreg?)\n", vr);
    abort();
  }
  ssa_opt_add_use_instr(vi, use_idx);
}

/* ========================================================================
 * ssa_gen_arm_fuse_mul_add_to_mla
 * t1 = MUL(a, b); t2 = ADD(t1, c) -> t2 = MLA(a, b, c); NOP the MUL
 * ======================================================================== */

/* Headline case: single-use MUL result feeds an ADD as one operand, plain
 * (non-shift-defined) accumulator -> fuses to MLA. */
UT_TEST(test_mla_fuse_basic)
{
  TCCIRState *ir = utb_ssa_new(4);
  /* t0=a t1=b t2=c inputs; t3 = mul result; t4 = add result */
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(3, I32), utb_temp(2, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), mul);
  utb_def(&ctx, utb_vreg(utb_temp(4, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), add); /* mul result used once, by ADD */

  int r = ssa_gen_arm_fuse_mul_add_to_mla(&ctx, mul);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_MLA);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, add)), utb_vreg(utb_temp(4, I32)));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, add)), utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, add)), utb_vreg(utb_temp(1, I32)));
  UT_ASSERT_EQ(utb_vreg(utb_op4(ir, add)), utb_vreg(utb_temp(2, I32)));

  /* mul's vinfo should be dead now (use removed, def cleared). */
  IRSSAVregInfo *mvi = ssa_opt_vinfo(&ctx, utb_vreg(utb_temp(3, I32)));
  UT_ASSERT(mvi != NULL);
  UT_ASSERT_EQ(mvi->def_instr, -1);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Accumulator is the ADD's src1 instead of src2 -- still fuses, accum picked
 * correctly regardless of which side the MUL result is on. */
UT_TEST(test_mla_fuse_accum_on_src1)
{
  TCCIRState *ir = utb_ssa_new(4);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(2, I32), utb_temp(3, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), mul);
  utb_def(&ctx, utb_vreg(utb_temp(4, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), add);

  int r = ssa_gen_arm_fuse_mul_add_to_mla(&ctx, mul);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_MLA);
  UT_ASSERT_EQ(utb_vreg(utb_op4(ir, add)), utb_vreg(utb_temp(2, I32)));

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: MUL result used twice -> must NOT fuse (would drop the other use). */
UT_TEST(test_mla_no_fuse_multi_use)
{
  TCCIRState *ir = utb_ssa_new(5);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(3, I32), utb_temp(2, I32));
  int extra = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_temp(3, I32), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), mul);
  utb_def(&ctx, utb_vreg(utb_temp(4, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), extra); /* 2nd use -> use_count == 2 */

  int r = ssa_gen_arm_fuse_mul_add_to_mla(&ctx, mul);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MUL);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: MUL's single use is not an ADD -> must not fuse. */
UT_TEST(test_mla_no_fuse_use_not_add)
{
  TCCIRState *ir = utb_ssa_new(4);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));
  int sub = utb_emit(ir, TCCIR_OP_SUB, utb_temp(4, I32), utb_temp(3, I32), utb_temp(2, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), mul);
  utb_def(&ctx, utb_vreg(utb_temp(4, I32)), sub);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), sub);

  int r = ssa_gen_arm_fuse_mul_add_to_mla(&ctx, mul);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MUL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: 64-bit MUL result -> Cortex-M has no 64-bit MLA, must not fuse. */
UT_TEST(test_mla_no_fuse_64bit)
{
  TCCIRState *ir = utb_ssa_new(4);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(3, I64), utb_temp(0, I64), utb_temp(1, I64));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I64), utb_temp(3, I64), utb_temp(2, I64));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(3, I64)), mul);
  utb_def(&ctx, utb_vreg(utb_temp(4, I64)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I64)), add);

  int r = ssa_gen_arm_fuse_mul_add_to_mla(&ctx, mul);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MUL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: accumulator is defined by a SHL -- the backend can fold that
 * shift into the ADD's barrel-shifter operand, which MLA cannot express;
 * fusing would silently drop the shift. Must not fuse. */
UT_TEST(test_mla_no_fuse_accum_is_shl)
{
  TCCIRState *ir = utb_ssa_new(6);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(4, I32), utb_imm(2, I32));
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, I32), utb_temp(3, I32), utb_temp(2, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), shl);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), mul);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), add);

  int r = ssa_gen_arm_fuse_mul_add_to_mla(&ctx, mul);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MUL);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: the ADD instruction found via the MUL result's recorded use does
 * not actually reference mul_vr in either operand (a stale/inconsistent
 * use-def record) -- defensive guard, must not fuse. */
UT_TEST(test_mla_no_fuse_add_operand_mismatch)
{
  TCCIRState *ir = utb_ssa_new(6);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(3, I32), utb_temp(0, I32), utb_temp(1, I32));
  /* ADD's operands are unrelated temps -- neither is t3. */
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(1, I32), utb_temp(2, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), mul);
  utb_def(&ctx, utb_vreg(utb_temp(4, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), add); /* recorded use, but ADD doesn't read t3 */

  int r = ssa_gen_arm_fuse_mul_add_to_mla(&ctx, mul);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MUL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* ========================================================================
 * ssa_gen_arm_fuse_shl_add_to_load_indexed
 * t1=SHL(idx,#scale); t2=ADD(base,t1); t3=LOAD(t2) -> t3=LOAD_INDEXED(base,idx,#scale)
 * ======================================================================== */

UT_TEST(test_shl_load_indexed_fuse_basic)
{
  TCCIRState *ir = utb_ssa_new(6);
  /* t0 = idx, t1 = base (address, non-lval), t2 = shl result, t3 = add result
   * (address), t4 = load result. */
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(0, I32), utb_imm(2, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(4, I32), utb_lval(utb_temp(3, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), shl);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), ld);

  int r = ssa_gen_arm_fuse_shl_add_to_load_indexed(&ctx, shl);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, ld), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, ld)), utb_vreg(utb_temp(4, I32)));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ld)), utb_vreg(utb_temp(1, I32))); /* base */
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, ld)), utb_vreg(utb_temp(0, I32))); /* idx */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, ld)), 2);         /* scale */
  /* base's is_lval must have been cleared (LOAD_INDEXED derefs internally). */
  UT_ASSERT_EQ(utb_src1(ir, ld).is_lval, 0);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: scale out of [0,3] range (no barrel-shift encoding for it). */
UT_TEST(test_shl_load_indexed_no_fuse_scale_out_of_range)
{
  TCCIRState *ir = utb_ssa_new(6);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(0, I32), utb_imm(4, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(4, I32), utb_lval(utb_temp(3, I32)), UTB_NONE);
  (void)add; (void)ld;

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);

  int r = ssa_gen_arm_fuse_shl_add_to_load_indexed(&ctx, shl);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_SHL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: SHL result used more than once -> must not fuse. */
UT_TEST(test_shl_load_indexed_no_fuse_multi_use_shl)
{
  TCCIRState *ir = utb_ssa_new(7);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(0, I32), utb_imm(2, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(4, I32), utb_lval(utb_temp(3, I32)), UTB_NONE);
  int extra = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(6, I32), utb_temp(2, I32), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), shl);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), extra);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), ld);

  int r = ssa_gen_arm_fuse_shl_add_to_load_indexed(&ctx, shl);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_SHL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: the ADD result feeds something other than a LOAD -> no fuse. */
UT_TEST(test_shl_load_indexed_no_fuse_not_a_load)
{
  TCCIRState *ir = utb_ssa_new(6);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(0, I32), utb_imm(2, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  int use = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(4, I32), utb_temp(3, I32), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), shl);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), use);

  int r = ssa_gen_arm_fuse_shl_add_to_load_indexed(&ctx, shl);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_SHL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Same fusion, but the ADD's operand order is swapped (shl result is src2,
 * base is src1) -- exercises the `else if` branch that picks base=add_src1. */
UT_TEST(test_shl_load_indexed_fuse_swapped_add_operands)
{
  TCCIRState *ir = utb_ssa_new(6);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(0, I32), utb_imm(2, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(1, I32), utb_temp(2, I32));
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(4, I32), utb_lval(utb_temp(3, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), shl);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), ld);

  int r = ssa_gen_arm_fuse_shl_add_to_load_indexed(&ctx, shl);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, ld), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ld)), utb_vreg(utb_temp(1, I32))); /* base */
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, ld)), utb_vreg(utb_temp(0, I32))); /* idx */

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* ========================================================================
 * ssa_gen_arm_fuse_shl_add_to_store_indexed
 * t1=SHL(idx,#scale); t2=ADD(base,t1); STORE(t2,val) -> STORE_INDEXED(base,val,idx,#scale)
 * ======================================================================== */

UT_TEST(test_shl_store_indexed_fuse_basic)
{
  TCCIRState *ir = utb_ssa_new(6);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(0, I32), utb_imm(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(1, I32), utb_temp(2, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(3, I32)), utb_temp(4, I32), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), shl);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), st);

  int r = ssa_gen_arm_fuse_shl_add_to_store_indexed(&ctx, shl);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, st)), utb_vreg(utb_temp(1, I32))); /* base */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, st)), utb_vreg(utb_temp(4, I32))); /* value */
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, st)), utb_vreg(utb_temp(0, I32))); /* idx */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, st)), 1);         /* scale */
  UT_ASSERT_EQ(utb_dest(ir, st).is_lval, 0);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: STORE's dest vreg is not the ADD's result -> no fuse. */
UT_TEST(test_shl_store_indexed_no_fuse_dest_mismatch)
{
  TCCIRState *ir = utb_ssa_new(7);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(0, I32), utb_imm(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(1, I32), utb_temp(2, I32));
  /* STORE targets an unrelated pointer temp (6), not the ADD's result. */
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(6, I32)), utb_temp(4, I32), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), shl);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), st); /* recorded, but STORE doesn't actually read t3 */

  int r = ssa_gen_arm_fuse_shl_add_to_store_indexed(&ctx, shl);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_SHL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: base operand is itself an lval (would need its own deref;
 * STORE_INDEXED's base is a plain address, not a further-dereffed lvalue). */
UT_TEST(test_shl_store_indexed_no_fuse_base_is_lval)
{
  TCCIRState *ir = utb_ssa_new(6);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(0, I32), utb_imm(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_lval(utb_temp(1, I32)), utb_temp(2, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(3, I32)), utb_temp(4, I32), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), shl);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), st);

  int r = ssa_gen_arm_fuse_shl_add_to_store_indexed(&ctx, shl);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_SHL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Same fusion with the ADD's operand order swapped (shl result is src1,
 * base is src2) -- exercises the `if` branch that picks base=add_src2. */
UT_TEST(test_shl_store_indexed_fuse_swapped_add_operands)
{
  TCCIRState *ir = utb_ssa_new(6);
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(0, I32), utb_imm(1, I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(3, I32)), utb_temp(4, I32), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), shl);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), add);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), st);

  int r = ssa_gen_arm_fuse_shl_add_to_store_indexed(&ctx, shl);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, st)), utb_vreg(utb_temp(1, I32))); /* base */
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, st)), utb_vreg(utb_temp(0, I32))); /* idx */

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* ========================================================================
 * ssa_gen_arm_reduce_mul_to_shift
 * dest = MUL(src, #pow2) -> dest = SHL(src, #log2(pow2))
 * ======================================================================== */

UT_TEST(test_mul_to_shl_pow2_src2)
{
  TCCIRState *ir = utb_ssa_new(2);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(8, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);

  int r = ssa_gen_arm_reduce_mul_to_shift(&ctx, mul);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, mul)), utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, mul)), 3); /* log2(8) */

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Immediate on src1 instead of src2 -- var_op/imm_op still identified correctly. */
UT_TEST(test_mul_to_shl_pow2_src1)
{
  TCCIRState *ir = utb_ssa_new(2);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(1, I32), utb_imm(16, I32), utb_temp(0, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);

  int r = ssa_gen_arm_reduce_mul_to_shift(&ctx, mul);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, mul)), utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, mul)), 4); /* log2(16) */

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* val == 1 -> shift by 0; still a valid (degenerate) reduction. */
UT_TEST(test_mul_to_shl_pow2_one)
{
  TCCIRState *ir = utb_ssa_new(2);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);

  int r = ssa_gen_arm_reduce_mul_to_shift(&ctx, mul);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_SHL);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, mul)), 0);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: neither operand is an immediate -> no reduction possible. */
UT_TEST(test_mul_to_shl_no_fuse_no_imm)
{
  TCCIRState *ir = utb_ssa_new(2);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32), utb_temp(2, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);

  int r = ssa_gen_arm_reduce_mul_to_shift(&ctx, mul);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MUL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: immediate is not a power of 2 -> no reduction. */
UT_TEST(test_mul_to_shl_no_fuse_not_pow2)
{
  TCCIRState *ir = utb_ssa_new(2);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(6, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);

  int r = ssa_gen_arm_reduce_mul_to_shift(&ctx, mul);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MUL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: immediate is <= 0 -> not a valid shift amount, no reduction. */
UT_TEST(test_mul_to_shl_no_fuse_nonpositive)
{
  TCCIRState *ir = utb_ssa_new(2);
  int mul = utb_emit(ir, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);

  int r = ssa_gen_arm_reduce_mul_to_shift(&ctx, mul);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, mul), TCCIR_OP_MUL);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* ========================================================================
 * ssa_gen_arm_fuse_load_through_add_imm
 * t_lea = ADD(base, #imm); t_val = LOAD(*t_lea) -> t_val = LOAD_INDEXED(base, #imm, 0)
 * ======================================================================== */

UT_TEST(test_load_add_imm_fuse_basic)
{
  TCCIRState *ir = utb_ssa_new(4);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(12, I32));
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), ld);

  int r = ssa_gen_arm_fuse_load_through_add_imm(&ctx, ld);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, ld), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ld)), utb_vreg(utb_temp(0, I32))); /* base */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, ld)), 12);       /* imm index */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, ld)), 0);         /* scale=0 */
  /* The defining ADD (lea) itself is left in place (DCE cleans it up later
   * in the real pipeline); only the LOAD's own use of lea_vr is dropped. */
  IRSSAVregInfo *lvi = ssa_opt_vinfo(&ctx, utb_vreg(utb_temp(1, I32)));
  UT_ASSERT(lvi != NULL);
  UT_ASSERT_EQ(lvi->use_count, 0);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Multiple LOADs through the same LEA each get rewritten independently
 * (unlike the SHL-indexed fusion, this one does not require single-use). */
UT_TEST(test_load_add_imm_fuse_multi_use_lea)
{
  TCCIRState *ir = utb_ssa_new(4);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int ld1 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);
  int ld2 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(3, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), ld1);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), ld2);

  int r1 = ssa_gen_arm_fuse_load_through_add_imm(&ctx, ld1);
  UT_ASSERT_EQ(r1, 1);
  UT_ASSERT_EQ(utb_op(ir, ld1), TCCIR_OP_LOAD_INDEXED);

  int r2 = ssa_gen_arm_fuse_load_through_add_imm(&ctx, ld2);
  UT_ASSERT_EQ(r2, 1);
  UT_ASSERT_EQ(utb_op(ir, ld2), TCCIR_OP_LOAD_INDEXED);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: immediate offset out of LDR encoding range (> 4095). */
UT_TEST(test_load_add_imm_no_fuse_out_of_range)
{
  TCCIRState *ir = utb_ssa_new(4);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4096, I32));
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), ld);

  int r = ssa_gen_arm_fuse_load_through_add_imm(&ctx, ld);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, ld), TCCIR_OP_LOAD);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: 64-bit load -- deliberately excluded (LDRD alignment trap). */
UT_TEST(test_load_add_imm_no_fuse_64bit)
{
  TCCIRState *ir = utb_ssa_new(4);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(8, I32));
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I64), utb_lval(utb_temp(1, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), ld);

  int r = ssa_gen_arm_fuse_load_through_add_imm(&ctx, ld);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, ld), TCCIR_OP_LOAD);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: the LEA temp has a non-address ("value") use in addition to the
 * LOAD deref -- e.g. it also feeds an ADD directly (typical of an induction
 * variable). Fusing would extend base's liveness unsoundly; must not fuse. */
UT_TEST(test_load_add_imm_no_fuse_value_use)
{
  TCCIRState *ir = utb_ssa_new(5);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);
  /* t1 used as plain data operand elsewhere too. */
  int other = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(1, I32), utb_temp(4, I32));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), ld);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), other);

  int r = ssa_gen_arm_fuse_load_through_add_imm(&ctx, ld);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, ld), TCCIR_OP_LOAD);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: base operand is a SYMREF, not a plain vreg -- refused. */
UT_TEST(test_load_add_imm_no_fuse_symref_base)
{
  TCCIRState *ir = utb_ssa_new(4);
  utb_pools_init(ir);
  static Sym g;
  memset(&g, 0, sizeof(g));
  g.v = 50;
  IROperand sym = utb_symref(ir, &g, 0, 0, 0, I32);

  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), sym, utb_imm(4, I32));
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), ld);

  int r = ssa_gen_arm_fuse_load_through_add_imm(&ctx, ld);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, ld), TCCIR_OP_LOAD);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Immediate on the LEA's src1 (base on src2) -- exercises the `imm_op = a`
 * branch of arm_extract_add_imm_base's operand-order detection. */
UT_TEST(test_load_add_imm_fuse_imm_on_src1)
{
  TCCIRState *ir = utb_ssa_new(4);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(28, I32), utb_temp(0, I32));
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), ld);

  int r = ssa_gen_arm_fuse_load_through_add_imm(&ctx, ld);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, ld), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ld)), utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, ld)), 28);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* ========================================================================
 * ssa_gen_arm_fuse_store_through_add_imm
 * t_lea = ADD(base, #imm); STORE(*t_lea, val) -> STORE_INDEXED(base, val, #imm, 0)
 * ======================================================================== */

UT_TEST(test_store_add_imm_fuse_basic)
{
  TCCIRState *ir = utb_ssa_new(4);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(20, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_temp(2, I32), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), st);

  int r = ssa_gen_arm_fuse_store_through_add_imm(&ctx, st);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, st)), utb_vreg(utb_temp(0, I32))); /* base */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, st)), utb_vreg(utb_temp(2, I32))); /* value */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, st)), 20);       /* imm index */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, st)), 0);         /* scale=0 */

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: STORE's dest isn't a plain vreg (e.g. is_local) -- refused
 * up-front before even looking at the LEA chain. */
UT_TEST(test_store_add_imm_no_fuse_local_dest)
{
  TCCIRState *ir = utb_ssa_new(4);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(20, I32));
  IROperand d = utb_lval(utb_temp(1, I32));
  d.is_local = 1;
  int st = utb_emit(ir, TCCIR_OP_STORE, d, utb_temp(2, I32), UTB_NONE);
  (void)lea;

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);

  int r = ssa_gen_arm_fuse_store_through_add_imm(&ctx, st);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: 64-bit store value -- excluded like the LOAD side. */
UT_TEST(test_store_add_imm_no_fuse_64bit)
{
  TCCIRState *ir = utb_ssa_new(4);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(8, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_temp(2, I64), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), st);

  int r = ssa_gen_arm_fuse_store_through_add_imm(&ctx, st);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: the LEA's def is not an ADD (e.g. a LOAD) -- chain doesn't
 * resolve, no fuse. */
UT_TEST(test_store_add_imm_no_fuse_def_not_add)
{
  TCCIRState *ir = utb_ssa_new(4);
  int notlea = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_lval(utb_temp(0, I32)), UTB_NONE);
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_temp(2, I32), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), notlea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), st);

  int r = ssa_gen_arm_fuse_store_through_add_imm(&ctx, st);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative (shared arm_extract_add_imm_base helper): the LEA temp's only
 * use is a STORE where it is BOTH the address and the *value* being stored
 * (`*t1 = t1`) -- must reject, since it would otherwise mistake the data use
 * for a second address use. */
UT_TEST(test_store_add_imm_no_fuse_lea_is_also_store_value)
{
  TCCIRState *ir = utb_ssa_new(4);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  /* STORE *t1 = t1 -- both dest (address) and src1 (value) reference lea_vr. */
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_temp(1, I32), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(1, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(1, I32)), st);

  int r = ssa_gen_arm_fuse_store_through_add_imm(&ctx, st);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* ========================================================================
 * ssa_gen_arm_fuse_mla_accum_through_add_imm
 * t_lea = ADD(base, #imm); MLA dest, s1, s2 + *t_lea -> t_lea = LOAD_INDEXED(base,#imm,0);
 * MLA dest, s1, s2 + t_lea (non-deref)
 * ======================================================================== */

UT_TEST(test_mla_accum_add_imm_fuse_basic)
{
  TCCIRState *ir = utb_ssa_new(5);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(0, I32), utb_imm(16, I32));
  int mla = utb_emit4(ir, TCCIR_OP_MLA, utb_temp(4, I32), utb_temp(1, I32), utb_temp(2, I32),
                      utb_lval(utb_temp(3, I32)));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), mla);

  int r = ssa_gen_arm_fuse_mla_accum_through_add_imm(&ctx, mla);

  UT_ASSERT_EQ(r, 1);
  /* The LEA's instruction slot is now a LOAD_INDEXED. */
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, lea)), utb_vreg(utb_temp(3, I32)));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, lea)), utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, lea)), 16);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, lea)), 0);
  /* MLA still MLA, accum now non-deref t3. */
  UT_ASSERT_EQ(utb_op(ir, mla), TCCIR_OP_MLA);
  IROperand accum = utb_op4(ir, mla);
  UT_ASSERT_EQ(utb_vreg(accum), utb_vreg(utb_temp(3, I32)));
  UT_ASSERT_EQ(accum.is_lval, 0);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: accumulator isn't a dereferenced vreg (is_lval == 0) -- nothing
 * to fold, no fuse. */
UT_TEST(test_mla_accum_add_imm_no_fuse_not_lval)
{
  TCCIRState *ir = utb_ssa_new(5);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(0, I32), utb_imm(16, I32));
  int mla = utb_emit4(ir, TCCIR_OP_MLA, utb_temp(4, I32), utb_temp(1, I32), utb_temp(2, I32),
                      utb_temp(3, I32) /* not lval */);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), mla);

  int r = ssa_gen_arm_fuse_mla_accum_through_add_imm(&ctx, mla);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, mla), TCCIR_OP_MLA);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: t_lea used more than once (not exclusively this MLA's accum). */
UT_TEST(test_mla_accum_add_imm_no_fuse_multi_use)
{
  TCCIRState *ir = utb_ssa_new(6);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(0, I32), utb_imm(16, I32));
  int mla = utb_emit4(ir, TCCIR_OP_MLA, utb_temp(4, I32), utb_temp(1, I32), utb_temp(2, I32),
                      utb_lval(utb_temp(3, I32)));
  int extra = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(5, I32), utb_lval(utb_temp(3, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), mla);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), extra);

  int r = ssa_gen_arm_fuse_mla_accum_through_add_imm(&ctx, mla);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_ADD);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: 64-bit MLA dest -- not supported on Cortex-M, no fuse. */
UT_TEST(test_mla_accum_add_imm_no_fuse_64bit_dest)
{
  TCCIRState *ir = utb_ssa_new(5);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_temp(0, I32), utb_imm(16, I32));
  int mla = utb_emit4(ir, TCCIR_OP_MLA, utb_temp(4, I64), utb_temp(1, I64), utb_temp(2, I64),
                      utb_lval(utb_temp(3, I32)));

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(3, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(3, I32)), mla);

  int r = ssa_gen_arm_fuse_mla_accum_through_add_imm(&ctx, mla);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_ADD);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* ========================================================================
 * ssa_gen_arm_fuse_store_src_through_add_imm
 * t_lea = ADD(base, #imm); STORE(V, *t_lea) -> t_lea = LOAD_INDEXED(base,#imm,0);
 * STORE(V, t_lea)
 * ======================================================================== */

UT_TEST(test_store_src_add_imm_fuse_basic)
{
  TCCIRState *ir = utb_ssa_new(5);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_imm(24, I32));
  /* STORE dest = V1 (some VAR), src1 = *t2 (deref). */
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_var(1, I32), utb_lval(utb_temp(2, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), st);

  int r = ssa_gen_arm_fuse_store_src_through_add_imm(&ctx, st);

  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_LOAD_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, lea)), utb_vreg(utb_temp(2, I32)));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, lea)), utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, lea)), 24);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, lea)), 0);

  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);
  IROperand new_src = utb_src1(ir, st);
  UT_ASSERT_EQ(utb_vreg(new_src), utb_vreg(utb_temp(2, I32)));
  UT_ASSERT_EQ(new_src.is_lval, 0);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: an intervening STORE between the LEA and this STORE could write
 * the same memory (GVN-CSE'd address hazard, see comment in ssa_opt_arm.c);
 * the hoist would then read a stale pre-store value. Must not fuse. */
UT_TEST(test_store_src_add_imm_no_fuse_intervening_store)
{
  TCCIRState *ir = utb_ssa_new(6);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_imm(24, I32));
  int clobber = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_temp(5, I32), UTB_NONE);
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_var(1, I32), utb_lval(utb_temp(2, I32)), UTB_NONE);
  (void)clobber;

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), st);

  int r = ssa_gen_arm_fuse_store_src_through_add_imm(&ctx, st);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: intervening function call could also clobber memory -- same
 * hoist hazard as a STORE, must not fuse. */
UT_TEST(test_store_src_add_imm_no_fuse_intervening_call)
{
  TCCIRState *ir = utb_ssa_new(6);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_imm(24, I32));
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_var(1, I32), utb_lval(utb_temp(2, I32)), UTB_NONE);
  (void)call;

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), st);

  int r = ssa_gen_arm_fuse_store_src_through_add_imm(&ctx, st);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* Negative: t_lea's single use is NOT this STORE's src1 (e.g. it's the
 * STORE's dest instead) -- the "used exactly once, as this instr's src deref"
 * invariant fails, no fuse. */
UT_TEST(test_store_src_add_imm_no_fuse_wrong_use_site)
{
  TCCIRState *ir = utb_ssa_new(5);
  int lea = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_imm(24, I32));
  /* Some other instruction (not `st`) is recorded as t2's use. */
  int other = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(4, I32), utb_lval(utb_temp(2, I32)), UTB_NONE);
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_var(1, I32), utb_lval(utb_temp(3, I32)), UTB_NONE);

  IRSSAOptCtx ctx;
  utb_ssa_ctx_init(&ctx, ir);
  utb_def(&ctx, utb_vreg(utb_temp(2, I32)), lea);
  utb_use(&ctx, utb_vreg(utb_temp(2, I32)), other);

  int r = ssa_gen_arm_fuse_store_src_through_add_imm(&ctx, st);

  UT_ASSERT_EQ(r, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_ssa_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_arm)
{
  UT_RUN(test_mla_fuse_basic);
  UT_RUN(test_mla_fuse_accum_on_src1);
  UT_RUN(test_mla_no_fuse_multi_use);
  UT_RUN(test_mla_no_fuse_use_not_add);
  UT_RUN(test_mla_no_fuse_64bit);
  UT_RUN(test_mla_no_fuse_accum_is_shl);
  UT_RUN(test_mla_no_fuse_add_operand_mismatch);

  UT_RUN(test_shl_load_indexed_fuse_basic);
  UT_RUN(test_shl_load_indexed_no_fuse_scale_out_of_range);
  UT_RUN(test_shl_load_indexed_no_fuse_multi_use_shl);
  UT_RUN(test_shl_load_indexed_no_fuse_not_a_load);
  UT_RUN(test_shl_load_indexed_fuse_swapped_add_operands);

  UT_RUN(test_shl_store_indexed_fuse_basic);
  UT_RUN(test_shl_store_indexed_no_fuse_dest_mismatch);
  UT_RUN(test_shl_store_indexed_no_fuse_base_is_lval);
  UT_RUN(test_shl_store_indexed_fuse_swapped_add_operands);

  UT_RUN(test_mul_to_shl_pow2_src2);
  UT_RUN(test_mul_to_shl_pow2_src1);
  UT_RUN(test_mul_to_shl_pow2_one);
  UT_RUN(test_mul_to_shl_no_fuse_no_imm);
  UT_RUN(test_mul_to_shl_no_fuse_not_pow2);
  UT_RUN(test_mul_to_shl_no_fuse_nonpositive);

  UT_RUN(test_load_add_imm_fuse_basic);
  UT_RUN(test_load_add_imm_fuse_multi_use_lea);
  UT_RUN(test_load_add_imm_no_fuse_out_of_range);
  UT_RUN(test_load_add_imm_no_fuse_64bit);
  UT_RUN(test_load_add_imm_no_fuse_value_use);
  UT_RUN(test_load_add_imm_no_fuse_symref_base);
  UT_RUN(test_load_add_imm_fuse_imm_on_src1);

  UT_RUN(test_store_add_imm_fuse_basic);
  UT_RUN(test_store_add_imm_no_fuse_local_dest);
  UT_RUN(test_store_add_imm_no_fuse_64bit);
  UT_RUN(test_store_add_imm_no_fuse_def_not_add);
  UT_RUN(test_store_add_imm_no_fuse_lea_is_also_store_value);

  UT_RUN(test_mla_accum_add_imm_fuse_basic);
  UT_RUN(test_mla_accum_add_imm_no_fuse_not_lval);
  UT_RUN(test_mla_accum_add_imm_no_fuse_multi_use);
  UT_RUN(test_mla_accum_add_imm_no_fuse_64bit_dest);

  UT_RUN(test_store_src_add_imm_fuse_basic);
  UT_RUN(test_store_src_add_imm_no_fuse_intervening_store);
  UT_RUN(test_store_src_add_imm_no_fuse_intervening_call);
  UT_RUN(test_store_src_add_imm_no_fuse_wrong_use_site);
}
