/*
 *  test_opt_pack64.c - suite for ir/opt_pack64.c (64-bit register-pair pack)
 *
 *  Covers the six independent entry points in this TU:
 *
 *  1. tcc_ir_opt_pack64 — folds `(ZEXT(hi) SHL #32) OR ZEXT(lo)` into a single
 *     PACK64(lo, hi), NOPing the three feeder instructions.
 *
 *  2. tcc_ir_opt_pack64_from_stack_stores — folds an 8-byte LOAD from a *direct*
 *     stack slot pair (two adjacent 32-bit STOREs at [A, A+4)) into
 *     PACK64(lo_val, hi_val).  REGRESSION GUARD (fuzz seed 7 / test 239): a
 *     STACKOFF operand with vreg_type != 0 is a VAR/PARAM's spill-home *hint*,
 *     not a real memory location — matching stores by that phantom offset must
 *     NOT fire the fold.
 *
 *  3. tcc_ir_opt_pack64_implicit — folds the SAR/SHL/OR widening idiom that
 *     lacks explicit ZEXTs (`(hi32 SHL #32) OR lo32` where both operands are
 *     32-bit) into PACK64(lo, hi), leaving the SHL feeder for DCE. Guards against
 *     firing when the OR's operands are 64-bit (would corrupt the packed hi) or
 *     when both halves are compile-time constants (forfeits later constant
 *     folding of the whole chain).
 *
 *  4. tcc_ir_opt_pack64_tautology — folds `PACK64(low_half(X), X SHR #32)` (a
 *     64-bit value split into two halves and immediately reassembled) into a
 *     plain `ASSIGN dest = X`, since the pack is the identity when both halves
 *     trace back to the same 64-bit vreg X.
 *
 *  5. tcc_ir_opt_cmp_narrow_64 — narrows a 64-bit CMP to 32-bit when src1's
 *     high half is provably zero (ZEXT or SHR-by->=32 producer) and src2 is a
 *     compile-time constant whose high 32 bits are zero, gated on the
 *     comparison being EQ/NE or an *unsigned* relational (narrowing a signed
 *     compare would flip sign-bit semantics).
 *
 *  6. tcc_ir_opt_shl32_or_chain — collapses the `((X SAR 31) SHL 32) OR X`
 *     sign-extension idiom when the whole 64-bit OR result is itself narrowed
 *     right back down by a trailing `SHL #32` or `AND 0xFFFFFFFF`: the dead
 *     high/low half producer chain (SAR/SHL1/OR) is NOPed and the final
 *     consumer reads X directly.
 *
 *  tcc_ir_opt_shift64_dead_half is a pure *annotation* pass (writes
 *  ir->shift64_dead_half[orig_index], no IR mutation) — covered separately at
 *  the end of this file.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions/annotations are inspected
 *  directly, per Pattern B (see tests/unit/README.md, test_opt_loop_dead.c).
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry points (declared in ir/opt.h; forward-declared here to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_pack64(TCCIRState *ir);
int tcc_ir_opt_pack64_from_stack_stores(TCCIRState *ir);
int tcc_ir_opt_pack64_implicit(TCCIRState *ir);
int tcc_ir_opt_pack64_tautology(TCCIRState *ir);
int tcc_ir_opt_cmp_narrow_64(TCCIRState *ir);
int tcc_ir_opt_shl32_or_chain(TCCIRState *ir);
int tcc_ir_opt_shift64_dead_half(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* JUMPIF condition tokens (match evaluate_compare_condition / opt_pack64.c). */
#define TOK_EQ  0x94
#define TOK_NE  0x95
#define TOK_ULT 0x92
#define TOK_UGE 0x93
#define TOK_ULE 0x96
#define TOK_UGT 0x97
#define TOK_LT  0x9c

/* VAR-position live-interval table: tcc_ir_opt_pack64_tautology reads
 * tcc_ir_get_live_interval() for X's vreg (VAR or TEMP) to confirm it is a
 * 64-bit value (is_llong / is_double). utb_new() zeroes the interval
 * pointers/sizes, which makes tcc_ir_get_live_interval() exit(1) on any
 * lookup -- allocate a table sized for the positions the test uses. */
static void utb_alloc_var_intervals(TCCIRState *ir, int count)
{
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->variables_live_intervals_size = count;
}

/* ================================================================ pack64 */

/* POSITIVE: explicit ZEXT/SHL/OR pattern.
 *   0: T0(i64) = ZEXT T_hi32        ; zero-extend hi half
 *   1: T1(i64) = T0 SHL #32         ; shift into high word
 *   2: T2(i64) = ZEXT T_lo32        ; zero-extend lo half
 *   3: T3(i64) = T1 OR T2           ; combine -> matches the fold
 * After: instr 3 becomes PACK64(lo=T_lo32, hi=T_hi32); 0/1/2 become NOP. */
UT_TEST(test_pack64_explicit_zext_shl_or_fires)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 5; /* T0..T5 used below */

  int i_zh = utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(0, I64), utb_temp(4, I32), UTB_NONE);
  int i_sh = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64), utb_imm(32, I32));
  int i_zl = utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(2, I64), utb_temp(5, I32), UTB_NONE);
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I64), utb_temp(1, I64), utb_temp(2, I64));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I64), UTB_NONE);

  int changes = tcc_ir_opt_pack64(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_PACK64);
  /* src1 = lo (T5), src2 = hi (T4). */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_or)), utb_vreg(utb_temp(5, I32)));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, i_or)), utb_vreg(utb_temp(4, I32)));
  UT_ASSERT_EQ(utb_op(ir, i_zh), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_sh), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_zl), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE (swapped operand order): the ZEXT-lo/ZEXT-hi OR operands can
 * appear in either order; `swap` loop must catch OR(zl, shl(zh)) too. */
UT_TEST(test_pack64_explicit_operand_order_swapped_fires)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 5;

  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(0, I64), utb_temp(4, I32), UTB_NONE); /* 0: zh */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64), utb_imm(32, I32)); /* 1: sh */
  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(2, I64), utb_temp(5, I32), UTB_NONE); /* 2: zl */
  /* OR(zl, sh) -- lo operand first this time. */
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I64), utb_temp(2, I64), utb_temp(1, I64));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I64), UTB_NONE);

  int changes = tcc_ir_opt_pack64(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_PACK64);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_or)), utb_vreg(utb_temp(5, I32))); /* lo */
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, i_or)), utb_vreg(utb_temp(4, I32))); /* hi */
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the OR's dest is not INT64 (e.g. a plain 32-bit OR) -- must not
 * be mistaken for a pack candidate. */
UT_TEST(test_pack64_explicit_non64_or_dest_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 3;

  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(0, I64), utb_temp(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64), utb_imm(32, I32));
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(1, I64), utb_temp(2, I32));

  int changes = tcc_ir_opt_pack64(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the SHL's shift amount is not 32 -- must not fire. */
UT_TEST(test_pack64_explicit_wrong_shift_amount_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 5;

  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(0, I64), utb_temp(4, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64), utb_imm(16, I32)); /* wrong amt */
  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(2, I64), utb_temp(5, I32), UTB_NONE);
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I64), utb_temp(1, I64), utb_temp(2, I64));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I64), UTB_NONE);

  int changes = tcc_ir_opt_pack64(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the SHL-feeding TEMP (T1) is used TWICE -- once by the OR under
 * test, once by an unrelated extra consumer.  Single-use is required so the
 * pass can safely NOP the feeder chain; must not fire. */
UT_TEST(test_pack64_explicit_multi_use_shl_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 6;

  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(0, I64), utb_temp(4, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64), utb_imm(32, I32));
  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(2, I64), utb_temp(5, I32), UTB_NONE);
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I64), utb_temp(1, I64), utb_temp(2, I64));
  /* Extra use of T1 keeps it multiply-used. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(6, I64), utb_temp(1, I64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I64), UTB_NONE);

  int changes = tcc_ir_opt_pack64(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ============================================== pack64_from_stack_stores */

/* POSITIVE: two adjacent 32-bit STOREs to a *direct* stack slot pair
 * (vreg_type == 0, the real StackLoc form), followed by an 8-byte LOAD from
 * the same base offset -- folds to PACK64(lo_val, hi_val). */
UT_TEST(test_pack64_stack_stores_direct_slot_fires)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS; /* tcc_ir_pool_ensure grows in place */

  IROperand slot_lo = utb_lval(utb_stackoff(16, 1, 0, 0, I32));
  IROperand slot_hi = utb_lval(utb_stackoff(20, 1, 0, 0, I32));
  IROperand slot_base_load = utb_lval(utb_stackoff(16, 1, 0, 0, I64));

  int i_store_lo = utb_emit(ir, TCCIR_OP_STORE, slot_lo, utb_temp(0, I32), UTB_NONE);
  int i_store_hi = utb_emit(ir, TCCIR_OP_STORE, slot_hi, utb_temp(1, I32), UTB_NONE);
  int i_load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I64), slot_base_load, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I64), UTB_NONE);

  int changes = tcc_ir_opt_pack64_from_stack_stores(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_load), TCCIR_OP_PACK64);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_load)), utb_vreg(utb_temp(0, I32))); /* lo */
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, i_load)), utb_vreg(utb_temp(1, I32))); /* hi */
  /* The two source stores remain intact (the pass only rewrites the LOAD). */
  UT_ASSERT_EQ(utb_op(ir, i_store_lo), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, i_store_hi), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* REGRESSION (mirrors test 239 / fuzz seed 7 root cause): the LOAD's source
 * STACKOFF operand has a non-zero vreg_type -- i.e. it's a VAR's *spill-home
 * hint*, not a direct stack-slot read (see the IROP_TAG_STACKOFF contract in
 * tccir_operand.h). Even though two 32-bit STOREs to the exact same numeric
 * offsets [16,20) are present in the code, the pass must NOT match them,
 * because a VAR/PARAM referenced this way is read via its vreg, not that
 * memory location -- folding here would silently substitute an unrelated
 * value (exactly the miscompile test 239 guards against). */
UT_TEST(test_pack64_stack_stores_var_spill_alias_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  IROperand slot_lo = utb_lval(utb_stackoff(16, 1, 0, 0, I32));
  IROperand slot_hi = utb_lval(utb_stackoff(20, 1, 0, 0, I32));
  /* LOAD src operand: same tag/is_local/is_lval/offset as a direct slot, but
   * carries a VAR vreg (vreg_type == TCCIR_VREG_TYPE_VAR) -- this is the
   * "phantom spill offset" shape from the bug report, built directly via the
   * raw irop_make_stackoff() vreg argument (ir_build.h's utb_stackoff always
   * passes vreg=0 / vreg_type=0, so the aliasing shape is constructed here by
   * hand to mirror production's VAR-with-STACKOFF-tag encoding). */
  IROperand slot_base_load_var_alias = irop_make_stackoff(
      TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 7), 16, /*is_lval=*/1, /*is_llocal=*/0, /*is_param=*/0, I64);

  int i_store_lo = utb_emit(ir, TCCIR_OP_STORE, slot_lo, utb_temp(0, I32), UTB_NONE);
  int i_store_hi = utb_emit(ir, TCCIR_OP_STORE, slot_hi, utb_temp(1, I32), UTB_NONE);
  int i_load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I64), slot_base_load_var_alias, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I64), UTB_NONE);

  /* Sanity: the alias operand really does carry a vreg (the guard condition
   * the production code checks: irop_get_vreg(src) != -1). */
  UT_ASSERT(irop_get_vreg(slot_base_load_var_alias) != -1);

  int changes = tcc_ir_opt_pack64_from_stack_stores(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_load), TCCIR_OP_LOAD); /* untouched */
  UT_ASSERT_EQ(utb_op(ir, i_store_lo), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, i_store_hi), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: only the low half is stored (hi half missing) -- the pass
 * requires BOTH adjacent stores to be found before folding. */
UT_TEST(test_pack64_stack_stores_missing_hi_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  IROperand slot_lo = utb_lval(utb_stackoff(16, 1, 0, 0, I32));
  IROperand slot_base_load = utb_lval(utb_stackoff(16, 1, 0, 0, I64));

  utb_emit(ir, TCCIR_OP_STORE, slot_lo, utb_temp(0, I32), UTB_NONE);
  int i_load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I64), slot_base_load, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I64), UTB_NONE);

  int changes = tcc_ir_opt_pack64_from_stack_stores(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: an intervening FUNCCALLVOID between the stores and the LOAD can
 * clobber arbitrary memory -- the backward scan must bail out (break) rather
 * than match through the call. */
UT_TEST(test_pack64_stack_stores_call_between_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  IROperand slot_lo = utb_lval(utb_stackoff(16, 1, 0, 0, I32));
  IROperand slot_hi = utb_lval(utb_stackoff(20, 1, 0, 0, I32));
  IROperand slot_base_load = utb_lval(utb_stackoff(16, 1, 0, 0, I64));

  utb_emit(ir, TCCIR_OP_STORE, slot_lo, utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, slot_hi, utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_temp(3, I32), utb_imm(0, I32)); /* clobbers memory */
  int i_load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I64), slot_base_load, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I64), UTB_NONE);

  int changes = tcc_ir_opt_pack64_from_stack_stores(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the low-half source TEMP is redefined between its STORE and the
 * LOAD -- the value observed by the (would-be) PACK64 would be wrong, so the
 * pass must not fold. */
UT_TEST(test_pack64_stack_stores_redef_between_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  IROperand slot_lo = utb_lval(utb_stackoff(16, 1, 0, 0, I32));
  IROperand slot_hi = utb_lval(utb_stackoff(20, 1, 0, 0, I32));
  IROperand slot_base_load = utb_lval(utb_stackoff(16, 1, 0, 0, I64));

  utb_emit(ir, TCCIR_OP_STORE, slot_lo, utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, slot_hi, utb_temp(1, I32), UTB_NONE);
  /* T0 redefined after being stored -- the stored value is now stale. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(99, I32), UTB_NONE);
  int i_load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I64), slot_base_load, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I64), UTB_NONE);

  int changes = tcc_ir_opt_pack64_from_stack_stores(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: LOAD dest is not INT64 -- not a candidate for this fold. */
UT_TEST(test_pack64_stack_stores_non64_load_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  IROperand slot_lo = utb_lval(utb_stackoff(16, 1, 0, 0, I32));
  IROperand slot_hi = utb_lval(utb_stackoff(20, 1, 0, 0, I32));
  IROperand slot_base_load = utb_lval(utb_stackoff(16, 1, 0, 0, I32));

  utb_emit(ir, TCCIR_OP_STORE, slot_lo, utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, slot_hi, utb_temp(1, I32), UTB_NONE);
  int i_load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), slot_base_load, UTB_NONE);

  int changes = tcc_ir_opt_pack64_from_stack_stores(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* ===================================================== pack64_implicit */

/* POSITIVE: SAR/SHL/OR widening idiom without explicit ZEXTs.
 *   0: T0(i64) = T_hi32 SHL #32   ; hi half shifted up (32-bit input)
 *   1: T1(i64) = T0 OR T_lo32     ; combine with 32-bit lo -> PACK64 candidate
 * After: instr 1 becomes PACK64(lo=T_lo32, hi=T_hi32); instr 0 NOPed. */
UT_TEST(test_pack64_implicit_shl_or_fires)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 5;

  int i_shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(0, I64), utb_temp(4, I32), utb_imm(32, I32));
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I64), utb_temp(0, I64), utb_temp(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I64), UTB_NONE);

  int changes = tcc_ir_opt_pack64_implicit(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_PACK64);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_or)), utb_vreg(utb_temp(5, I32))); /* lo */
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, i_or)), utb_vreg(utb_temp(4, I32))); /* hi */
  UT_ASSERT_EQ(utb_op(ir, i_shl), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the "lo" OR operand is itself 64-bit -- its implicit
 * zero-extension into the i64 OR would not be hi=0 (it may carry real high
 * bits), so folding would corrupt the packed high half. Must not fire. */
UT_TEST(test_pack64_implicit_lo_operand_64bit_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 5;

  utb_emit(ir, TCCIR_OP_SHL, utb_temp(0, I64), utb_temp(4, I32), utb_imm(32, I32));
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I64), utb_temp(0, I64), utb_temp(5, I64)); /* lo is i64 */

  int changes = tcc_ir_opt_pack64_implicit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_OR);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the SHL's input (destined for "hi") is itself 64-bit -- bits
 * above bit 31 would survive PACK64's implicit hi truncation and corrupt the
 * result. Must not fire. */
UT_TEST(test_pack64_implicit_hi_input_64bit_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 5;

  utb_emit(ir, TCCIR_OP_SHL, utb_temp(0, I64), utb_temp(4, I64), utb_imm(32, I32)); /* hi input is i64 */
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I64), utb_temp(0, I64), utb_temp(5, I32));

  int changes = tcc_ir_opt_pack64_implicit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_OR);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: both halves resolve to compile-time constants via VAR stores --
 * const_prop would have folded the whole SHL+OR chain to a literal, but
 * PACK64 is opaque to const_prop, so converting forfeits that fold. The
 * const-resolution guard (pack64_operand_resolves_const) must suppress the
 * fold in this case.
 *   V4 <- #7 (hi, VAR const)
 *   V5 <- #9 (lo, VAR const)
 *   T0(i64) = ASSIGN V4            ; hi chain root: VAR read
 *   T1(i64) = T0 SHL #32
 *   T2(i64) = ASSIGN V5            ; lo chain root: VAR read
 *   T3(i64) = T1 OR T2
 * Both V4 and V5 have a single constant-valued STORE reachable before the
 * OR, so pack64_operand_resolves_const succeeds for both operands and the
 * pass must skip the fold (changes == 0). */
UT_TEST(test_pack64_implicit_both_const_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 5;
  utb_alloc_var_intervals(ir, 8);

  utb_emit(ir, TCCIR_OP_STORE, utb_var(4, I32), utb_imm(7, I32), UTB_NONE); /* 0: V4 <- 7 */
  utb_emit(ir, TCCIR_OP_STORE, utb_var(5, I32), utb_imm(9, I32), UTB_NONE); /* 1: V5 <- 9 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I64), utb_var(4, I32), UTB_NONE); /* 2: hi root */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64), utb_imm(32, I32)); /* 3 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I64), utb_var(5, I32), UTB_NONE); /* 4: lo root */
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I64), utb_temp(1, I64), utb_temp(2, I64)); /* 5 */

  int changes = tcc_ir_opt_pack64_implicit(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_OR);

  utb_free(ir);
  return 0;
}

/* ==================================================== pack64_tautology */

/* POSITIVE: PACK64(low_half(X), X SHR #32) where both halves trace back to
 * the same 64-bit VAR X -- the pack is the identity, rewritten to a plain
 * ASSIGN dest = X (X's lvalue read).
 *   0: T0(i64) = ASSIGN &V0 (lval)   ; lo chain root: reads V0's value
 *   1: T1(i64) = &V0(lval) SHR #32   ; hi chain root: V0's high half
 *   2: T2(i64) = T0 PACK64 T1
 * V0 is marked is_llong so the "X is 64-bit" gate passes.
 * After: instr 2 becomes ASSIGN T2 = V0 (lo_src, the lvalue read of X). */
UT_TEST(test_pack64_tautology_identity_fires)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 2;
  ir->next_local_variable = 0;
  utb_alloc_var_intervals(ir, 1);
  ir->variables_live_intervals[0].is_llong = 1;

  IROperand x_lval = utb_lval(utb_var(0, I64));

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I64), x_lval, UTB_NONE);           /* 0: lo root */
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I64), x_lval, utb_imm(32, I32));      /* 1: hi root */
  int i_pk = utb_emit(ir, TCCIR_OP_PACK64, utb_temp(2, I64), utb_temp(0, I64), utb_temp(1, I64)); /* 2 */

  int changes = tcc_ir_opt_pack64_tautology(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_pk), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_pk)), utb_vreg(utb_var(0, I64)));
  UT_ASSERT_EQ(utb_src1(ir, i_pk).is_lval, 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: lo and hi trace back to two DIFFERENT 64-bit VARs -- not the
 * identity, the pass must not fire. */
UT_TEST(test_pack64_tautology_different_vars_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 2;
  ir->next_local_variable = 1;
  utb_alloc_var_intervals(ir, 2);
  ir->variables_live_intervals[0].is_llong = 1;
  ir->variables_live_intervals[1].is_llong = 1;

  IROperand x_lval = utb_lval(utb_var(0, I64));
  IROperand y_lval = utb_lval(utb_var(1, I64));

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I64), x_lval, UTB_NONE);       /* 0: lo root = X */
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I64), y_lval, utb_imm(32, I32)); /* 1: hi root = Y (different!) */
  int i_pk = utb_emit(ir, TCCIR_OP_PACK64, utb_temp(2, I64), utb_temp(0, I64), utb_temp(1, I64)); /* 2 */

  int changes = tcc_ir_opt_pack64_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_pk), TCCIR_OP_PACK64);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the SHR amount is not 32 -- not `X >> 32`, must not fire. */
UT_TEST(test_pack64_tautology_wrong_shr_amount_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 2;
  ir->next_local_variable = 0;
  utb_alloc_var_intervals(ir, 1);
  ir->variables_live_intervals[0].is_llong = 1;

  IROperand x_lval = utb_lval(utb_var(0, I64));

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I64), x_lval, UTB_NONE);
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I64), x_lval, utb_imm(16, I32)); /* wrong amount */
  int i_pk = utb_emit(ir, TCCIR_OP_PACK64, utb_temp(2, I64), utb_temp(0, I64), utb_temp(1, I64));

  int changes = tcc_ir_opt_pack64_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_pk), TCCIR_OP_PACK64);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: X is not marked is_llong/is_double in its live interval -- the
 * pass cannot prove `X SHR #32` != 0 identically zero, so it must not treat
 * the pack as an identity. */
UT_TEST(test_pack64_tautology_not_64bit_var_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 2;
  ir->next_local_variable = 0;
  utb_alloc_var_intervals(ir, 1); /* is_llong left 0 */

  IROperand x_lval = utb_lval(utb_var(0, I64));

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I64), x_lval, UTB_NONE);
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I64), x_lval, utb_imm(32, I32));
  int i_pk = utb_emit(ir, TCCIR_OP_PACK64, utb_temp(2, I64), utb_temp(0, I64), utb_temp(1, I64));

  int changes = tcc_ir_opt_pack64_tautology(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_pk), TCCIR_OP_PACK64);

  utb_free(ir);
  return 0;
}

/* =================================================== cmp_narrow_64 */

/* POSITIVE: src1's high half is provably zero (ZEXT producer), src2 is an
 * immediate whose high 32 bits are zero, and the consumer is JUMPIF(EQ) --
 * the narrowing is bit-exact for equality, so the CMP is rewritten to 32-bit
 * operands. */
UT_TEST(test_cmp_narrow64_zext_eq_fires)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 1;

  /* Dest TEMP position must be > 0: the pass sizes its def_idx table off
   * max_tmp_pos and bails immediately when it stays 0 (see max_tmp_pos == 0
   * early-return guard), so T0 alone would never reach the CMP logic. */
  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(1, I64), utb_temp(2, I32), UTB_NONE); /* 0: T1 hi=0 */
  int i_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I64), utb_imm(1000, I64)); /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_EQ, I32), UTB_NONE); /* 2 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE); /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE); /* 4 target */

  int changes = tcc_ir_opt_cmp_narrow_64(ir);

  UT_ASSERT_EQ(changes, 1);
  IROperand s1 = utb_src1(ir, i_cmp);
  IROperand s2 = utb_src2(ir, i_cmp);
  UT_ASSERT_EQ(irop_get_btype(s1), IROP_BTYPE_INT32);
  UT_ASSERT_EQ(irop_get_btype(s2), IROP_BTYPE_INT32);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s2), 1000);

  utb_free(ir);
  return 0;
}

/* POSITIVE: unsigned relational (ULT) also narrows -- both operands have
 * hi=0, so unsigned order is preserved at any width. */
UT_TEST(test_cmp_narrow64_zext_ult_fires)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 1;

  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(1, I64), utb_temp(2, I32), UTB_NONE);
  int i_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I64), utb_imm(5, I64));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_ULT, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_narrow_64(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(irop_get_btype(utb_src1(ir, i_cmp)), IROP_BTYPE_INT32);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a SIGNED relational (LT) is not safe to narrow -- a u64 value
 * like 0x00000000FFFF8000 is positive at 64-bit but negative when
 * misinterpreted as i32. Must not fire. */
UT_TEST(test_cmp_narrow64_signed_relational_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 1;

  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(1, I64), utb_temp(2, I32), UTB_NONE);
  int i_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I64), utb_imm(5, I64));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_LT, I32), UTB_NONE); /* signed */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_narrow_64(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(irop_get_btype(utb_src1(ir, i_cmp)), IROP_BTYPE_INT64);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: src2's constant has non-zero high 32 bits -- narrowing would
 * drop real information, must not fire. */
UT_TEST(test_cmp_narrow64_src2_high_bits_set_no_fire)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir); /* need a real pool_i64 for the >32-bit immediate below */
  ir->next_temporary_variable = 1;

  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(1, I64), utb_temp(2, I32), UTB_NONE);
  int64_t big = ((int64_t)1 << 40); /* high bits set -- must NOT fit in IMM32 */
  /* irop_make_imm32 truncates to 32 bits; build the real >32-bit immediate via
   * the I64 pool so the high bits actually survive into irop_get_imm64_ex(). */
  uint32_t i64_idx = tcc_ir_pool_add_i64(ir, big);
  IROperand src2_big = irop_make_i64(0, i64_idx, I64);
  int i_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I64), src2_big);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, src2_big), big);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_narrow_64(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(irop_get_btype(utb_src1(ir, i_cmp)), IROP_BTYPE_INT64);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: src1's producer is a plain ASSIGN (not ZEXT / SHR>=32) -- the
 * high half is not provably zero, must not fire. */
UT_TEST(test_cmp_narrow64_unproven_hi_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I64), utb_temp(2, I64), UTB_NONE); /* not hi-proven */
  int i_cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I64), utb_imm(5, I64));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_cmp_narrow_64(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(irop_get_btype(utb_src1(ir, i_cmp)), IROP_BTYPE_INT64);

  utb_free(ir);
  return 0;
}

/* =================================================== shl32_or_chain */

/* POSITIVE (pattern A, SHL32 consumer): the SAR/SHL1/OR sign-extension chain
 * feeding a trailing `SHL #32` -- the OR's high-contributing half is shifted
 * back out, so the whole SAR/SHL1/OR chain is dead; the final SHL is rewired
 * to read X directly and the dead chain is NOPed.
 *   0: T_sar(i64) = X SAR #31        ; (X here stands in as any i64 producer)
 *   1: T_shl1(i64) = T_sar SHL #32
 *   2: T_or(i64) = T_shl1 OR X2      ; X2 is the "keep" operand
 *   3: T_use(i64) = T_or SHL #32     ; consumer -> rewritten to X2 SHL #32
 */
UT_TEST(test_shl32_or_chain_shl32_consumer_fires)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 5; /* T0..T5 used below */

  int i_sar = utb_emit(ir, TCCIR_OP_SAR, utb_temp(0, I64), utb_temp(4, I64), utb_imm(31, I32)); /* 0 */
  int i_shl1 = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64), utb_imm(32, I32)); /* 1 */
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I64), utb_temp(1, I64), utb_temp(5, I64)); /* 2: keep=T5 */
  int i_use = utb_emit(ir, TCCIR_OP_SHL, utb_temp(3, I64), utb_temp(2, I64), utb_imm(32, I32)); /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I64), UTB_NONE);

  int changes = tcc_ir_opt_shl32_or_chain(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_use)), utb_vreg(utb_temp(5, I64))); /* reads X2 (T5) directly */
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_shl1), TCCIR_OP_NOP);
  /* The SAR itself isn't NOPed by this pass (only shl1/or); it becomes dead
   * and would be cleaned up by DCE separately -- assert it's still SAR here,
   * documenting that this pass only removes the two ops it directly matched. */
  UT_ASSERT_EQ(utb_op(ir, i_sar), TCCIR_OP_SAR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Pattern B (AND-low consumer): same chain, but the final consumer is
 * `AND #0xFFFFFFFF` instead of `SHL #32`. Regression lock for bugs.md #9
 * (fixed): irop_make_imm32 stores the mask as a sign-extending 32-bit
 * immediate, so irop_get_imm64_ex() sign-extends 0xFFFFFFFFu (int32_t -1) to
 * int64_t -1. The old check `(uint64_t)imm == 0xFFFFFFFFULL`
 * (ir/opt_pack64.c:1033) compared against 0x00000000FFFFFFFF and could never
 * match the sign-extended -1, so the AND-consumer half of this fusion was dead
 * code. The fix compares the low 32 bits `(uint32_t)imm == 0xFFFFFFFFu`, so the
 * fold now fires -- mirroring test_shl32_or_chain_shl32_consumer_fires. */
UT_TEST(test_shl32_or_chain_and_low_consumer_fires)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 5;

  utb_emit(ir, TCCIR_OP_SAR, utb_temp(0, I64), utb_temp(4, I64), utb_imm(31, I32));      /* 0 */
  int i_shl1 = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64), utb_imm(32, I32)); /* 1 */
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I64), utb_temp(5, I64), utb_temp(1, I64));    /* 2: keep first */
  int i_use = utb_emit(ir, TCCIR_OP_AND, utb_temp(3, I64), utb_temp(2, I64),
                       irop_make_imm32(0, (int32_t)0xFFFFFFFFu, I64)); /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I64), UTB_NONE);

  int changes = tcc_ir_opt_shl32_or_chain(ir);

  /* The fold now fires: the consumer's src1 is rewritten to the kept OR
   * operand (T5), and the OR + feeding SHL become dead NOPs. The consumer op
   * itself is unchanged (still AND), only its src1. */
  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_use), TCCIR_OP_AND);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_use)), utb_vreg(utb_temp(5, I64))); /* reads T5 directly */
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_shl1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the AND's mask is not exactly 0xFFFFFFFF -- must not fire. */
UT_TEST(test_shl32_or_chain_and_wrong_mask_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 5;

  utb_emit(ir, TCCIR_OP_SAR, utb_temp(0, I64), utb_temp(4, I64), utb_imm(31, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64), utb_imm(32, I32));
  utb_emit(ir, TCCIR_OP_OR, utb_temp(2, I64), utb_temp(5, I64), utb_temp(1, I64));
  int i_use = utb_emit(ir, TCCIR_OP_AND, utb_temp(3, I64), utb_temp(2, I64),
                       irop_make_imm32(0, 0x0000FFFF, I64)); /* wrong mask */

  int changes = tcc_ir_opt_shl32_or_chain(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_use), TCCIR_OP_AND);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: neither OR operand is a single-use `something SHL #32` TEMP --
 * both are single-use/single-def TEMPs, but their producer is ADD, not SHL --
 * no dead-bits half to eliminate, must not fire. Both operand TEMPs are kept
 * within DU-tracked range (positions <= next_temporary_variable) and given
 * real single-use ADD defs, so the rejection is driven by the `shl_q->op !=
 * TCCIR_OP_SHL` check itself rather than an incidental "out of DU range"
 * short-circuit. */
UT_TEST(test_shl32_or_chain_no_shl_operand_no_fire)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 4;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I64), utb_temp(6, I64), utb_imm(1, I32)); /* 0: T3, not a SHL */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I64), utb_temp(7, I64), utb_imm(1, I32)); /* 1: T4, not a SHL */
  int i_or = utb_emit(ir, TCCIR_OP_OR, utb_temp(0, I64), utb_temp(3, I64), utb_temp(4, I64)); /* 2 */
  int i_use = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_temp(0, I64), utb_imm(32, I32)); /* 3 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I64), UTB_NONE); /* 4 */

  int changes = tcc_ir_opt_shl32_or_chain(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_OR);
  UT_ASSERT_EQ(utb_op(ir, i_use), TCCIR_OP_SHL);

  utb_free(ir);
  return 0;
}

/* ================================================== shift64_dead_half */

/* POSITIVE: a 64-bit SHL whose single use is a SHR/SAR by >=32 -- the SHL's
 * low word is provably dead; the pass annotates
 * ir->shift64_dead_half[orig_index] with bit0 (skip_lo) set, no IR mutation. */
UT_TEST(test_shift64_dead_half_marks_skip_lo)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 1;

  int i_shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(0, I64), utb_temp(2, I64), utb_imm(8, I32)); /* 0 */
  int i_shr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I64), utb_temp(0, I64), utb_imm(40, I32)); /* 1: >=32, sole use of T0 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I64), UTB_NONE); /* 2 */
  ir->max_orig_index = 2; /* highest orig_index assigned (tcc_ir_put bookkeeping, done by hand here) */

  int changes = tcc_ir_opt_shift64_dead_half(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT(ir->shift64_dead_half != NULL);
  UT_ASSERT_EQ(ir->shift64_dead_half[ir->compact_instructions[i_shl].orig_index] & 1, 1);
  /* No IR mutation: both ops keep their original opcodes. */
  UT_ASSERT_EQ(utb_op(ir, i_shl), TCCIR_OP_SHL);
  UT_ASSERT_EQ(utb_op(ir, i_shr), TCCIR_OP_SHR);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the SHL's result TEMP has a SECOND use elsewhere (not
 * single-use), so its low word might actually be read -- must not annotate. */
UT_TEST(test_shift64_dead_half_multi_use_no_mark)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 2;

  int i_shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(0, I64), utb_temp(3, I64), utb_imm(8, I32)); /* 0 */
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I64), utb_temp(0, I64), utb_imm(40, I32));             /* 1 */
  /* Extra use of T0 elsewhere -- no longer single-use. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I64), utb_temp(0, I64), UTB_NONE); /* 2 */
  ir->max_orig_index = 2;

  int changes = tcc_ir_opt_shift64_dead_half(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_shl), TCCIR_OP_SHL);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the consuming shift amount is < 32 -- reads BOTH words of the
 * source, so the SHL's low word is not dead. Must not fire. */
UT_TEST(test_shift64_dead_half_shift_amount_below_32_no_mark)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 1;

  int i_shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(0, I64), utb_temp(2, I64), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I64), utb_temp(0, I64), utb_imm(16, I32)); /* < 32 */
  ir->max_orig_index = 1;

  int changes = tcc_ir_opt_shift64_dead_half(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_shl), TCCIR_OP_SHL);

  utb_free(ir);
  return 0;
}

/* POSITIVE (skip_hi): a 64-bit SHR by >=32 fills its HIGH word with a constant
 * 0.  When the single use is a TRUNCATING ASSIGN (64-bit source, 32-bit dest)
 * that fill is never read, so the pass annotates bit1 (skip_hi).  Note the
 * ASSIGN's SOURCE operand stays I64 -- the DEST width is what decides that only
 * the low half is read, which is the case the rule must recognise. */
UT_TEST(test_shift64_dead_half_marks_skip_hi_truncating_assign)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 2;

  int i_shr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I64), utb_temp(0, I64), utb_imm(41, I32)); /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I64), UTB_NONE);                  /* 1 */
  ir->max_orig_index = 1;

  int changes = tcc_ir_opt_shift64_dead_half(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT(ir->shift64_dead_half != NULL);
  UT_ASSERT_EQ(ir->shift64_dead_half[ir->compact_instructions[i_shr].orig_index] & 2, 2);
  /* No IR mutation. */
  UT_ASSERT_EQ(utb_op(ir, i_shr), TCCIR_OP_SHR);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (skip_hi): the single use is a WIDE assign (64-bit dest), which
 * copies both halves -- the high fill is read, so it must be materialized. */
UT_TEST(test_shift64_dead_half_wide_use_no_skip_hi)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 2;

  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I64), utb_temp(0, I64), utb_imm(41, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I64), utb_temp(1, I64), UTB_NONE);
  ir->max_orig_index = 1;

  int changes = tcc_ir_opt_shift64_dead_half(ir);

  UT_ASSERT_EQ(changes, 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (skip_hi): shift amount < 32 keeps a data-dependent high word, and
 * the emitter does not honour skip_hi on that path anyway. */
UT_TEST(test_shift64_dead_half_skip_hi_below_32_no_mark)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 2;

  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I64), utb_temp(0, I64), utb_imm(20, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I64), UTB_NONE);
  ir->max_orig_index = 1;

  int changes = tcc_ir_opt_shift64_dead_half(ir);

  UT_ASSERT_EQ(changes, 0);

  utb_free(ir);
  return 0;
}

UT_COVERS("pack64");
UT_COVERS("pack64_from_stack_stores");
UT_COVERS("pack64_implicit");
UT_COVERS("pack64_tautology");
UT_COVERS("cmp_narrow_64");
UT_COVERS("shl32_or_chain");
UT_COVERS("shift64_dead_half");
