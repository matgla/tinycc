/*
 *  test_opt_xform.c - suite for ir/opt_xform.c (in-place store-arith fusion)
 *
 *  tcc_ir_opt_store_inplace_arith fuses the codegen idiom
 *      T <-- (any) OP src        (T is a single-use INT32 TEMP, OP is simple arith)
 *      V <-- T  [STORE]          (the very next non-NOP op, same basic block)
 *  into
 *      V <-- (same) OP src       (the arith now writes V directly, in place)
 *      NOP                       (the STORE is removed)
 *  saving one register move.  V must be a register-promotable scalar (a VAR or
 *  PARAM whose live interval is not address-taken, not an lvalue, and not a
 *  64-bit / float / complex value), and the widths of T and V must match and be
 *  INT32.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.  The
 *  pass reads V's live-interval flags via tcc_ir_vreg_live_interval(), so each
 *  test installs zeroed VAR / PARAM / TEMP interval tables (clean => promotable)
 *  and sets individual flags to drive the negative paths.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt_xform.h; forward-declared here to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_store_inplace_arith(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64
#define I8  IROP_BTYPE_INT8
#define I16 IROP_BTYPE_INT16

/* ----------------------------------------------------------- helpers */

/* Install zeroed live-interval tables for all three vreg classes so that
 * tcc_ir_vreg_is_valid()/tcc_ir_vreg_live_interval() succeed for any position
 * below `size`.  All flags start clear, so a VAR/PARAM is fully promotable
 * (not addrtaken, not lvalue, not llong/double/complex) unless the test pokes
 * a flag afterward via utb_vli(). */
static void utb_alloc_all_intervals(TCCIRState *ir, int size)
{
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * size);
  ir->temporary_variables_live_intervals_size = size;
  ir->next_temporary_variable = 0;

  ir->variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * size);
  ir->variables_live_intervals_size = size;
  ir->next_local_variable = 0;

  ir->parameters_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * size);
  ir->parameters_live_intervals_size = size;
  ir->next_parameter = 0;
}

static inline IRLiveInterval *utb_vli(TCCIRState *ir, IROperand v)
{
  return tcc_ir_vreg_live_interval(ir, irop_get_vreg(v));
}

static inline int vreg_param(int pos)
{
  return TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, pos);
}

static inline int vreg_var(int pos)
{
  return TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, pos);
}

/* Emit `T<tpos> = src1 OP src2` then `STORE V = T<tpos>`; returns the STORE's
 * index (the arith is at store_idx - 1). */
static int utb_emit_arith_then_store(TCCIRState *ir, TccIrOp op, int tpos,
                                     IROperand v, IROperand src1, IROperand src2)
{
  utb_emit(ir, op, utb_temp(tpos, I32), src1, src2);
  return utb_emit(ir, TCCIR_OP_STORE, v, utb_temp(tpos, I32), UTB_NONE);
}

/* ------------------------------------------------------ POSITIVE tests */

/* Canonical fold against a PARAM (strncmp's `p = p + 1` loop tail):
 *   0: T0 = P0 + #1
 *   1: STORE P0 = T0
 * becomes
 *   0: P0 = P0 + #1     (in place, dest redirected to P0, is_lval cleared)
 *   1: NOP
 */
UT_TEST(test_xform_add_param_inplace)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand p = utb_param(0, I32);
  int st = utb_emit_arith_then_store(ir, TCCIR_OP_ADD, 0, p, utb_param(0, I32), utb_imm(1, I32));
  int ar = st - 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 1);
  /* arith op unchanged, but its dest now writes P0 in place. */
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, ar)), vreg_param(0));
  UT_ASSERT_EQ(utb_dest(ir, ar).is_lval, 0);
  /* src operands of the arith are untouched. */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ar)), vreg_param(0));
  /* STORE removed. */
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* SUB against a VAR folds the same way (this is the `subs r2,r2,#1` case). */
UT_TEST(test_xform_sub_var_inplace)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(3, I32);
  int st = utb_emit_arith_then_store(ir, TCCIR_OP_SUB, 0, v, utb_var(3, I32), utb_imm(1, I32));
  int ar = st - 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, ar)), vreg_var(3));
  UT_ASSERT_EQ(utb_dest(ir, ar).is_lval, 0);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Every whitelisted arith op (ADD/SUB/AND/OR/XOR/SHL/SAR/SHR) fuses; ops not on
 * the list (e.g. MUL) do not. */
UT_TEST(test_xform_all_simple_ops_fold)
{
  TccIrOp ops[] = {TCCIR_OP_ADD, TCCIR_OP_SUB, TCCIR_OP_AND, TCCIR_OP_OR,
                   TCCIR_OP_XOR, TCCIR_OP_SHL, TCCIR_OP_SAR, TCCIR_OP_SHR};
  for (unsigned t = 0; t < sizeof(ops) / sizeof(ops[0]); ++t)
  {
    TCCIRState *ir = utb_new();
    utb_alloc_all_intervals(ir, 16);

    IROperand v = utb_var(1, I32);
    int st = utb_emit_arith_then_store(ir, ops[t], 0, v, utb_var(1, I32), utb_imm(2, I32));
    int ar = st - 1;

    int changes = tcc_ir_opt_store_inplace_arith(ir);

    UT_ASSERT_EQ(changes, 1);
    UT_ASSERT_EQ(utb_op(ir, ar), ops[t]);
    UT_ASSERT_EQ(utb_vreg(utb_dest(ir, ar)), vreg_var(1));
    UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_NOP);

    utb_free(ir);
  }
  return 0;
}

/* MUL is not a whitelisted op -> no fusion even with a textbook pattern. */
UT_TEST(test_xform_mul_not_folded)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I32);
  int st = utb_emit_arith_then_store(ir, TCCIR_OP_MUL, 0, v, utb_var(1, I32), utb_imm(2, I32));
  int ar = st - 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_MUL);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, ar)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 0));
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* An intervening NOP is skipped: the STORE is still found as the next real op. */
UT_TEST(test_xform_skips_intervening_nop)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(2, I32);
  int ar = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(2, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  int st = utb_emit(ir, TCCIR_OP_STORE, v, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, ar)), vreg_var(2));
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------ NEGATIVE tests */

/* V's address is taken -> not register-promotable -> must NOT fuse. */
UT_TEST(test_xform_addrtaken_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I32);
  int st = utb_emit_arith_then_store(ir, TCCIR_OP_ADD, 0, v, utb_var(1, I32), utb_imm(1, I32));
  int ar = st - 1;
  utb_vli(ir, v)->addrtaken = 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, ar)), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 0));
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* V is flagged as an lvalue (memory-resident) -> must NOT fuse. */
UT_TEST(test_xform_v_is_lvalue_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I32);
  int st = utb_emit_arith_then_store(ir, TCCIR_OP_ADD, 0, v, utb_var(1, I32), utb_imm(1, I32));
  int ar = st - 1;
  utb_vli(ir, v)->is_lvalue = 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* V is a 64-bit value (is_llong) -> no single-register in-place form -> no fuse. */
UT_TEST(test_xform_v_is_llong_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I32);
  int st = utb_emit_arith_then_store(ir, TCCIR_OP_ADD, 0, v, utb_var(1, I32), utb_imm(1, I32));
  int ar = st - 1;
  utb_vli(ir, v)->is_llong = 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* V is a double (is_double) -> not an integer arith candidate -> no fuse. */
UT_TEST(test_xform_v_is_double_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I32);
  int st = utb_emit_arith_then_store(ir, TCCIR_OP_ADD, 0, v, utb_var(1, I32), utb_imm(1, I32));
  int ar = st - 1;
  utb_vli(ir, v)->is_double = 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* Sub-word width: the pass only fuses INT32 (sub-word carries narrowing).
 * T and V are both INT8 here, so t_btype != IROP_BTYPE_INT32 -> no fuse. */
UT_TEST(test_xform_int8_width_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I8);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I8), utb_var(1, I8), utb_imm(1, I8));
  int st = utb_emit(ir, TCCIR_OP_STORE, v, utb_temp(0, I8), UTB_NONE);
  int ar = st - 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* Width mismatch: T is INT32 but the STORE writes an INT64 slot.  t_btype !=
 * v_btype, so even before the INT32 gate the pass must bail. */
UT_TEST(test_xform_width_mismatch_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I64);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(1, I32), utb_imm(1, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, v, utb_temp(0, I32), UTB_NONE);
  int ar = st - 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* STORE writes through a pointer (store_dest.is_lval): V is a memory address,
 * not a promotable register -> must NOT fuse. */
UT_TEST(test_xform_store_dest_lval_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_lval(utb_var(1, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(1, I32), utb_imm(1, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, v, utb_temp(0, I32), UTB_NONE);
  int ar = st - 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* STORE reads *T (store_src.is_lval): the value stored is the dereference of T,
 * not T itself -> the in-place rewrite would change semantics -> no fuse. */
UT_TEST(test_xform_store_src_lval_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I32);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(1, I32), utb_imm(1, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, v, utb_lval(utb_temp(0, I32)), UTB_NONE);
  int ar = st - 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* T has a second use (read again after the STORE): folding would drop a needed
 * value, so the pass must NOT fuse. */
UT_TEST(test_xform_extra_use_of_t_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I32);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(1, I32), utb_imm(1, I32));     /* 0 */
  int st = utb_emit(ir, TCCIR_OP_STORE, v, utb_temp(0, I32), UTB_NONE);               /* 1 */
  /* 2: another consumer of T0 -> extra_uses=1. */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_imm(7, I32));    /* 2 */
  int ar = st - 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* The next real op is not a STORE -> nothing to fuse with. */
UT_TEST(test_xform_next_not_store_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  int ar = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(1, I32), utb_imm(1, I32));
  int as = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, as), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* STORE destination is a TEMP, not a VAR/PARAM -> V is not a promotable named
 * variable -> must NOT fuse (v_type gate). */
UT_TEST(test_xform_store_dest_temp_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(1, I32), utb_imm(1, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_temp(5, I32), utb_temp(0, I32), UTB_NONE);
  int ar = st - 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* The arith's destination must be a TEMP.  A VAR-dest arith is ignored, so even
 * a matching STORE cannot be folded. */
UT_TEST(test_xform_arith_dest_not_temp_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  /* dest is VAR2, store reads VAR2: arith dest is not a TEMP -> skipped. */
  int ar = utb_emit(ir, TCCIR_OP_ADD, utb_var(2, I32), utb_var(1, I32), utb_imm(1, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, utb_var(1, I32), utb_var(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* The arith's TEMP destination is itself an lvalue (a store-through) -> the pass
 * skips lval-dest arith, so no fuse. */
UT_TEST(test_xform_arith_dest_lval_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I32);
  utb_emit(ir, TCCIR_OP_ADD, utb_lval(utb_temp(0, I32)), utb_var(1, I32), utb_imm(1, I32));
  int st = utb_emit(ir, TCCIR_OP_STORE, v, utb_temp(0, I32), UTB_NONE);
  int ar = st - 1;

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* A basic-block boundary (JUMP) between the arith and the STORE blocks the fuse:
 * the STORE may not actually post-dominate the arith. */
UT_TEST(test_xform_block_boundary_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_var(1, I32);
  int ar = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(1, I32), utb_imm(1, I32)); /* 0 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);                        /* 1 */
  int st = utb_emit(ir, TCCIR_OP_STORE, v, utb_temp(0, I32), UTB_NONE);                    /* 2 */

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ar), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, st), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* Idempotence / fixpoint: the first application folds the single opportunity; a
 * second application reports no further changes. */
UT_TEST(test_xform_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  IROperand v = utb_param(0, I32);
  int st = utb_emit_arith_then_store(ir, TCCIR_OP_ADD, 0, v, utb_param(0, I32), utb_imm(1, I32));
  (void)st;

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_store_inplace_arith, 10);
  UT_ASSERT_EQ(total, 1);
  UT_ASSERT_EQ(tcc_ir_opt_store_inplace_arith(ir), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Two independent fold opportunities in one pass produce two changes. */
UT_TEST(test_xform_two_independent_folds)
{
  TCCIRState *ir = utb_new();
  utb_alloc_all_intervals(ir, 16);

  /* P0 = P0 + 1 ; STORE P0 */
  int st0 = utb_emit_arith_then_store(ir, TCCIR_OP_ADD, 0, utb_param(0, I32),
                                      utb_param(0, I32), utb_imm(1, I32));
  /* V2 = V2 - 1 ; STORE V2  (distinct T1, distinct slot) */
  int st1 = utb_emit_arith_then_store(ir, TCCIR_OP_SUB, 1, utb_var(2, I32),
                                      utb_var(2, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_store_inplace_arith(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_op(ir, st0 - 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, st0 - 1)), vreg_param(0));
  UT_ASSERT_EQ(utb_op(ir, st0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, st1 - 1), TCCIR_OP_SUB);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, st1 - 1)), vreg_var(2));
  UT_ASSERT_EQ(utb_op(ir, st1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_xform)
{
  UT_COVERS("store_inplace_arith");
  UT_RUN(test_xform_add_param_inplace);
  UT_RUN(test_xform_sub_var_inplace);
  UT_RUN(test_xform_all_simple_ops_fold);
  UT_RUN(test_xform_mul_not_folded);
  UT_RUN(test_xform_skips_intervening_nop);
  UT_RUN(test_xform_addrtaken_no_fold);
  UT_RUN(test_xform_v_is_lvalue_no_fold);
  UT_RUN(test_xform_v_is_llong_no_fold);
  UT_RUN(test_xform_v_is_double_no_fold);
  UT_RUN(test_xform_int8_width_no_fold);
  UT_RUN(test_xform_width_mismatch_no_fold);
  UT_RUN(test_xform_store_dest_lval_no_fold);
  UT_RUN(test_xform_store_src_lval_no_fold);
  UT_RUN(test_xform_extra_use_of_t_no_fold);
  UT_RUN(test_xform_next_not_store_no_fold);
  UT_RUN(test_xform_store_dest_temp_no_fold);
  UT_RUN(test_xform_arith_dest_not_temp_no_fold);
  UT_RUN(test_xform_arith_dest_lval_no_fold);
  UT_RUN(test_xform_block_boundary_no_fold);
  UT_RUN(test_xform_idempotent);
  UT_RUN(test_xform_two_independent_folds);
}
