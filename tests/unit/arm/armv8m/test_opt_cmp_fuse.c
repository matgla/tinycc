/*
 *  test_opt_cmp_fuse.c - suite for ir/opt_cmp_fuse.c (aggregate field-compare
 *  fusion, tcc_ir_opt_cmp_field_fuse)
 *
 *  The pass collapses the `a.f1 != b.f1 || a.f2 != b.f2 || ...` idiom — a run of
 *  >=2 bitfield-extract `!=`-compares that all branch to the same target — into a
 *  single masked word compare:
 *
 *      CMP extract_i(A), extract_i(B) ; JUMPIF "!=" -> L   (per field i)
 *  ->  t  = A XOR B ;  t &= (union of field masks) ;  CMP t,#0 ; JUMPIF "!=" -> L
 *
 *  cmpf_trace() walks each CMP operand back through an AND/SHL+SHR/SHR extract
 *  chain to a base word + a 32-bit field mask. The two sides must agree on the
 *  mask (mA == mB), the run must share base words + branch target, and there
 *  must be >=2 units. cmpf_same_base() refuses to line two base words up when
 *  their `is_lval` flags differ (the Tier-1 lvalue/memory-deref guard): a value
 *  read directly from memory must not be fused with a register value.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_cmp_field_fuse(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* TOK_* condition codes the JUMPIF condition operand carries (see tcc.h). The
 * pass only fuses `!=` (TOK_NE) branches. */
#define UT_TOK_NE 0x95
#define UT_TOK_EQ 0x94

/* The fused-target label encoded in every JUMPIF dest operand's imm. */
#define LBL 99

/* ----------------------------------------------------------- helpers */

/* The positive path calls tcc_ir_get_vreg_temp() to allocate the XOR/AND result
 * temps; give the IR a temp live-interval table so the allocator's bounds check
 * passes without taking the realloc-from-zero branch. Positions [base..size) are
 * available; the pass hands out the next two (base, base+1). */
static void utb_alloc_temp_intervals(TCCIRState *ir, int base, int size)
{
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * size);
  ir->temporary_variables_live_intervals_size = size;
  ir->next_temporary_variable = base;
}

/* An AND-extract feeder: dest(TEMP pos) = AND src(P<src_pos>), #mask. Returns the
 * instruction index. If src_is_lval, the (param) base word is flagged as a memory
 * lvalue so cmpf_same_base() will treat it as a distinct base. */
static int utb_emit_and_extract(TCCIRState *ir, int dest_pos, int src_param_pos,
                                int32_t mask, int src_is_lval)
{
  IROperand src = utb_param(src_param_pos, I32);
  src.is_lval = src_is_lval ? 1 : 0;
  return utb_emit(ir, TCCIR_OP_AND, utb_temp(dest_pos, I32), src, utb_imm(mask, I32));
}

/* ------------------------------------------------------ POSITIVE test */

/* Two field-compare units that branch to the same label fuse into XOR(+AND)+CMP:
 *
 *   0: T0 = AND P0, #0x00FF      ; A.f1
 *   1: T1 = AND P1, #0x00FF      ; B.f1
 *   2: CMP T0, T1                ; unit 1   (i)
 *   3: JUMPIF !=  -> L99
 *   4: T2 = AND P0, #0xFF00      ; A.f2
 *   5: T3 = AND P1, #0xFF00      ; B.f2
 *   6: CMP T2, T3                ; unit 2   (last_cmp)
 *   7: JUMPIF !=  -> L99
 *
 * Both units share bases P0/P1, target L99; masks per unit are symmetric
 * (mA==mB). union_mask = 0x00FF | 0xFF00 = 0xFFFF != 0xffffffff, so the AND
 * masking step is needed. xor_slot = last_cmp - 2 = 4. The pass rewrites:
 *   @4 -> XOR Tx = P0 ^ P1
 *   @5 -> AND Ty = Tx & #0xFFFF
 *   @6 -> CMP Ty, #0
 * and NOPs the span [2..5] except where rebuilt; the last JUMPIF survives. */
UT_TEST(test_cmp_fuse_two_field_units_fuse)
{
  TCCIRState *ir = utb_new();
  /* IR uses TEMP 0..3; pass allocates Tx=TEMP4, Ty=TEMP5. */
  utb_alloc_temp_intervals(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  utb_emit_and_extract(ir, 2, 0, 0xFF00, 0);
  utb_emit_and_extract(ir, 3, 1, 0xFF00, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  int j2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  /* One fused run -> one change. Non-vacuous: a no-op pass would leave the two
   * CMPs and would fail the op assertions below. */
  UT_ASSERT_EQ(changes, 1);

  /* @4 became XOR Tx = P0 ^ P1 (xor_slot = last_cmp(6) - 2 = 4). */
  int xor_slot = 4;
  UT_ASSERT_EQ(utb_op(ir, xor_slot), TCCIR_OP_XOR);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, xor_slot)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 4));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, xor_slot)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, xor_slot)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 1));

  /* @5 became AND Ty = Tx & #0xFFFF (union mask of the two fields). */
  int and_slot = 5;
  UT_ASSERT_EQ(utb_op(ir, and_slot), TCCIR_OP_AND);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, and_slot)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 5));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, and_slot)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 4));
  UT_ASSERT(irop_is_immediate(utb_src2(ir, and_slot)));
  UT_ASSERT_EQ((uint32_t)irop_get_imm64_ex(ir, utb_src2(ir, and_slot)), 0xFFFFu);

  /* The surviving CMP (last_cmp) is now `CMP Ty, #0`. */
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, c2)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 5));
  UT_ASSERT(irop_is_immediate(utb_src2(ir, c2)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, c2)), 0);

  /* The first unit's CMP was NOP'd; the last JUMPIF survives unchanged. */
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, j2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, j2)), UT_TOK_NE);
  UT_ASSERT_EQ(utb_dest(ir, j2).u.imm32, LBL);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------ NEGATIVE tests */

/* is_lval / memory-deref guard (Tier-1 bug class): identical to the positive
 * case, except unit 2's base word A is read as an lvalue (P0 with is_lval=1).
 * cmpf_same_base() compares is_lval first and refuses to line two bases up when
 * the flags differ, so the forward walk breaks after unit 1, units stays 1, and
 * nothing is fused. A pass that ignored the lvalue flag would (incorrectly)
 * fuse a register field with a memory dereference here. */
UT_TEST(test_cmp_fuse_lval_base_blocks_fusion)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int j1 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  /* unit 2: base word A is an lvalue (memory) — differs from unit 1's P0. */
  utb_emit_and_extract(ir, 2, 0, 0xFF00, 1);
  utb_emit_and_extract(ir, 3, 1, 0xFF00, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  /* Blocked: no change, both CMPs preserved. */
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j1), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* Asymmetric-mask guard: a single unit whose two compared sides extract
 * *different* fields (mA = 0x00FF, mB = 0xFF00). The pass requires mA == mB for
 * a clean field compare; here mA != mB, so the unit is rejected outright. */
UT_TEST(test_cmp_fuse_asymmetric_mask_no_fuse)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0xFF00, 0); /* different mask than side A */
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  /* a second matching unit, so only the mask asymmetry is what blocks it */
  utb_emit_and_extract(ir, 2, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 3, 1, 0xFF00, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);

  utb_free(ir);
  return 0;
}

/* Run-length guard: a single field-compare unit (no second `!=`-to-same-label
 * unit) has nothing to OR together; units < 2 -> no fusion. */
UT_TEST(test_cmp_fuse_single_unit_no_fuse)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  int j1 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j1), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* Condition guard: the same two-unit shape, but the branches are `==` (TOK_EQ),
 * not `!=`. The OR-of-inequalities identity only holds for `!=`, so the pass
 * skips the run entirely (the outer loop's TOK_NE filter). */
UT_TEST(test_cmp_fuse_non_ne_condition_no_fuse)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_EQ, I32), UTB_NONE);
  utb_emit_and_extract(ir, 2, 0, 0xFF00, 0);
  utb_emit_and_extract(ir, 3, 1, 0xFF00, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_EQ, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_cmp_fuse)
{
  UT_COVERS("cmp_field_fuse");
  UT_RUN(test_cmp_fuse_two_field_units_fuse);
  UT_RUN(test_cmp_fuse_lval_base_blocks_fusion);
  UT_RUN(test_cmp_fuse_asymmetric_mask_no_fuse);
  UT_RUN(test_cmp_fuse_single_unit_no_fuse);
  UT_RUN(test_cmp_fuse_non_ne_condition_no_fuse);
}
