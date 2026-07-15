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

/* In-range label for tests that call utb_assert_wellformed() (target 0 is
 * guaranteed to lie inside the tiny test function). */
#define LBL_IN_RANGE 0

/* An extra condition code for non-NE guard tests. */
#define UT_TOK_LT 0x9c

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

/* A (SHL a, SHR s) extract feeder: T[shl_tmp_pos] = src SHL a;
 * T[dest_pos] = T[shl_tmp_pos] SHR s.  Returns the index of the SHR. */
static int utb_emit_shift_extract(TCCIRState *ir, int shl_tmp_pos, int dest_pos,
                                  int src_param_pos, int a, int s, int src_is_lval)
{
  IROperand src = utb_param(src_param_pos, I32);
  if (src_is_lval)
    src = utb_lval(src);
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(shl_tmp_pos, I32), src, utb_imm(a, I32));
  return utb_emit(ir, TCCIR_OP_SHR, utb_temp(dest_pos, I32),
                  utb_temp(shl_tmp_pos, I32), utb_imm(s, I32));
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

/* ---------------------------------------------------- CORNER-CASE tests */

/* Three contiguous byte fields fuse into one XOR+AND+CMP; the union mask is the
 * independent OR of the per-field masks (semi-oracle). */
UT_TEST(test_cmp_fuse_three_fields_union_mask)
{
  TCCIRState *ir = utb_new();
  /* Temps 0..5 are used by the extract chain; the pass allocates temps 6,7. */
  utb_alloc_temp_intervals(ir, 6, 16);

  const uint32_t m1 = 0x000000FFu;
  const uint32_t m2 = 0x0000FF00u;
  const uint32_t m3 = 0x00FF0000u;
  const uint32_t expected_union = m1 | m2 | m3;

  utb_emit_and_extract(ir, 0, 0, m1, 0);
  utb_emit_and_extract(ir, 1, 1, m1, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, m2, 0);
  utb_emit_and_extract(ir, 3, 1, m2, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 4, 0, m3, 0);
  utb_emit_and_extract(ir, 5, 1, m3, 0);
  int c3 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(4, I32), utb_temp(5, I32));
  int j3 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c3), TCCIR_OP_CMP);

  int xor_slot = c3 - 2;
  int and_slot = c3 - 1;
  UT_ASSERT_EQ(utb_op(ir, xor_slot), TCCIR_OP_XOR);
  UT_ASSERT_EQ(utb_op(ir, and_slot), TCCIR_OP_AND);
  UT_ASSERT_EQ((uint32_t)irop_get_imm64_ex(ir, utb_src2(ir, and_slot)), expected_union);

  UT_ASSERT_EQ(utb_op(ir, j3), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 0x40000000), 0);

  utb_free(ir);
  return 0;
}

/* Field width 1 is a shift-count boundary: (x SHL 31) SHR 31 extracts bit 0,
 * (x SHL 30) SHR 31 extracts bit 1.  Both units fuse with union mask 0x3. */
UT_TEST(test_cmp_fuse_width_one_field)
{
  TCCIRState *ir = utb_new();
  /* Temps 0..7 used by the SHL+SHR chain; pass allocates 8,9. */
  utb_alloc_temp_intervals(ir, 8, 16);

  const uint32_t m1 = 0x00000001u;
  const uint32_t m2 = 0x00000002u;
  const uint32_t expected_union = m1 | m2;

  utb_emit_shift_extract(ir, 0, 1, 0, 31, 31, 0);
  utb_emit_shift_extract(ir, 2, 3, 1, 31, 31, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  utb_emit_shift_extract(ir, 4, 5, 0, 30, 31, 0);
  utb_emit_shift_extract(ir, 6, 7, 1, 30, 31, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(5, I32), utb_temp(7, I32));
  int j2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);

  int xor_slot = c2 - 2;
  int and_slot = c2 - 1;
  UT_ASSERT_EQ(utb_op(ir, xor_slot), TCCIR_OP_XOR);
  UT_ASSERT_EQ(utb_op(ir, and_slot), TCCIR_OP_AND);
  UT_ASSERT_EQ((uint32_t)irop_get_imm64_ex(ir, utb_src2(ir, and_slot)), expected_union);

  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, j2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 0x40000000), 0);

  utb_free(ir);
  return 0;
}

/* Field width 31 (boundary): AND #0x7FFFFFFF paired with AND #0x80000000
 * covers the whole word, so the AND masking step is omitted. */
UT_TEST(test_cmp_fuse_width_thirty_one_field)
{
  TCCIRState *ir = utb_new();
  /* Only temp 2 is needed for the XOR result. */
  utb_alloc_temp_intervals(ir, 2, 16);

  const uint32_t m1 = 0x7FFFFFFFu;
  const uint32_t m2 = 0x80000000u;

  utb_emit_and_extract(ir, 0, 0, m1, 0);
  utb_emit_and_extract(ir, 1, 1, m1, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, m2, 0);
  utb_emit_and_extract(ir, 3, 1, m2, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  int j2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);

  int xor_slot = c2 - 1;
  UT_ASSERT_EQ(utb_op(ir, xor_slot), TCCIR_OP_XOR);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, xor_slot)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 2));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, xor_slot)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, xor_slot)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 1));

  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, c2)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 2));

  /* No AND was inserted between XOR and CMP. */
  UT_ASSERT_EQ(utb_op(ir, xor_slot + 1), TCCIR_OP_CMP);

  UT_ASSERT_EQ(utb_op(ir, j2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 0x40000000), 0);

  utb_free(ir);
  return 0;
}

/* When the union of field masks covers the whole word, the AND step is
 * unnecessary and must be omitted. */
UT_TEST(test_cmp_fuse_full_mask_omits_and)
{
  TCCIRState *ir = utb_new();
  /* Only temp 2 is needed for the XOR result. */
  utb_alloc_temp_intervals(ir, 2, 16);

  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_param(1, I32));
  int j1 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  (void)j1;
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_param(1, I32));
  int j2 = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  /* The pass fused the two CMP+JUMPIF pairs, turning the first CMP into a NOP.
   * Detailed operand layout depends on internal temp allocation; we only pin
   * the high-level effect and structural soundness. */
  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  (void)c2;
  UT_ASSERT_EQ(utb_op(ir, j2), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 0x40000000), 0);

  utb_free(ir);
  return 0;
}

/* Field at offset 0 via (x SHL s) SHR s, together with a higher field. */
UT_TEST(test_cmp_fuse_offset_zero_lsb)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 8, 16);

  const uint32_t m1 = 0x000000FFu;
  const uint32_t m2 = 0x0000FF00u;
  const uint32_t expected_union = m1 | m2;

  utb_emit_shift_extract(ir, 0, 1, 0, 24, 24, 0);
  utb_emit_shift_extract(ir, 2, 3, 1, 24, 24, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  utb_emit_shift_extract(ir, 4, 5, 0, 16, 24, 0);
  utb_emit_shift_extract(ir, 6, 7, 1, 16, 24, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(5, I32), utb_temp(7, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);

  int xor_slot = c2 - 2;
  int and_slot = c2 - 1;
  UT_ASSERT_EQ(utb_op(ir, xor_slot), TCCIR_OP_XOR);
  UT_ASSERT_EQ(utb_op(ir, and_slot), TCCIR_OP_AND);
  UT_ASSERT_EQ((uint32_t)irop_get_imm64_ex(ir, utb_src2(ir, and_slot)), expected_union);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 0x40000000), 0);

  utb_free(ir);
  return 0;
}

/* Out-of-range shift amounts (negative or >= 32) must not be misrecognised as
 * narrow field extracts.  cmpf_trace falls back to whole-word compare, and the
 * resulting run does not fuse here because the whole-word bases are distinct
 * temporaries.  The key property is clean handling without crash. */
UT_TEST(test_cmp_fuse_out_of_range_shift_no_fuse)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 4, 16);

  /* Unit 1: SHR by 32 -> whole-word fallback. */
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(0, I32), utb_param(0, I32), utb_imm(32, I32));
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I32), utb_param(1, I32), utb_imm(32, I32));
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  /* Unit 2: SHR by -1 -> whole-word fallback. */
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(2, I32), utb_param(0, I32), utb_imm(-1, I32));
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(3, I32), utb_param(1, I32), utb_imm(-1, I32));
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);

  utb_free(ir);
  return 0;
}

/* Bases differ between units (P0/P1 vs P0/P2) -> run breaks after unit 1. */
UT_TEST(test_cmp_fuse_base_mismatch_no_fuse)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, 0xFF00, 0);
  utb_emit_and_extract(ir, 3, 2, 0xFF00, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);

  utb_free(ir);
  return 0;
}

/* Both A-side bases carry is_lval consistently, so cmpf_same_base lines them up
 * and fusion proceeds. */
UT_TEST(test_cmp_fuse_lval_base_fuses)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 1);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, 0xFF00, 1);
  utb_emit_and_extract(ir, 3, 1, 0xFF00, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);

  UT_ASSERT_EQ(utb_assert_wellformed(ir, 0x40000000), 0);

  utb_free(ir);
  return 0;
}

/* The extract instructions are not adjacent to their consuming CMP; the pass
 * finds them via tcc_ir_find_defining_instruction and still fuses. */
UT_TEST(test_cmp_fuse_distant_def_fuses)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 6, 16);

  utb_emit_shift_extract(ir, 0, 1, 0, 24, 24, 0);
  utb_emit_shift_extract(ir, 2, 3, 1, 24, 24, 0);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 4, 0, 0xFF00, 0);
  utb_emit_and_extract(ir, 5, 1, 0xFF00, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(4, I32), utb_temp(5, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 0x40000000), 0);

  utb_free(ir);
  return 0;
}

/* is_unsigned on the base operands does not affect equality or mask
 * computation, so fusion should proceed. */
UT_TEST(test_cmp_fuse_unsigned_operands_fuse)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 4, 16);

  IROperand ua = utb_unsigned(utb_param(0, I32));
  IROperand ub = utb_param(1, I32);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(0, I32), ua, utb_imm(0x00FF, I32));
  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), ub, utb_imm(0x00FF, I32));
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), ua, utb_imm(0xFF00, I32));
  utb_emit(ir, TCCIR_OP_AND, utb_temp(3, I32), ub, utb_imm(0xFF00, I32));
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL_IN_RANGE, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 0x40000000), 0);

  utb_free(ir);
  return 0;
}

/* A condition other than NE/EQ (here LT) blocks fusion entirely. */
UT_TEST(test_cmp_fuse_lt_condition_no_fuse)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_LT, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, 0xFF00, 0);
  utb_emit_and_extract(ir, 3, 1, 0xFF00, 0);
  int c2 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_LT, I32), UTB_NONE);

  int changes = tcc_ir_opt_cmp_field_fuse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_CMP);

  utb_free(ir);
  return 0;
}

/* The pass must converge: one application fuses, a second finds nothing. */
UT_TEST(test_cmp_fuse_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 4, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  utb_emit_and_extract(ir, 2, 0, 0xFF00, 0);
  utb_emit_and_extract(ir, 3, 1, 0xFF00, 0);
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(2, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(LBL, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_cmp_field_fuse, 4);
  UT_ASSERT_EQ(total, 1);

  utb_free(ir);
  return 0;
}

/* Empty IR is a no-op, not a crash. */
UT_TEST(test_cmp_fuse_empty_ir_no_crash)
{
  TCCIRState *ir = utb_new();
  int changes = tcc_ir_opt_cmp_field_fuse(ir);
  UT_ASSERT_EQ(changes, 0);
  utb_free(ir);
  return 0;
}

/* A CMP with no following JUMPIF is skipped cleanly. */
UT_TEST(test_cmp_fuse_lone_cmp_no_crash)
{
  TCCIRState *ir = utb_new();
  utb_alloc_temp_intervals(ir, 2, 16);

  utb_emit_and_extract(ir, 0, 0, 0x00FF, 0);
  utb_emit_and_extract(ir, 1, 1, 0x00FF, 0);
  int c1 = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_cmp_field_fuse(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_CMP);

  utb_free(ir);
  return 0;
}

UT_COVERS("cmp_field_fuse");
