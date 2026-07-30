/*
 *  test_opt_memset_fold.c - suite for the two memset-folding passes in ir/opt.c:
 *
 *    int tcc_ir_opt_small_memset_to_store(ir);         (stack-local dest)
 *    int tcc_ir_opt_small_global_memset_to_store(ir);  (global, symref dest)
 *
 *  Both recognise `memset(&dst, 0, N)` / `__aeabi_memset(dst, N, 0)` and replace
 *  the call with 1-2 direct zero stores when N is small.  Corner cases pinned:
 *  every power-of-two size 1/2/4/8, the 2-chunk decomposition (3, 6), the
 *  size-7 bail (needs 3 stores), size>cap, non-zero fill, non-matching callee
 *  name, wrong dest form, and (global) the FUNCCALLVAL reader gate plus the
 *  aeabi/memset argument-order swap.
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_opt_small_memset_to_store(TCCIRState *ir);
int tcc_ir_opt_small_global_memset_to_store(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

#define TOK_MEMSET 20
#define TOK_AEABI_MEMSET 21

/* A SYMREF callee operand bound to `sym` whose token is `tok`. */
static IROperand utb_callee(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* Emit a 3-arg memset/aeabi_memset call in argument order [p0, p1, p2].
 * Returns the FUNCCALLVOID index. */
static int emit_memset_call(TCCIRState *ir, IROperand callee, int call_id,
                            IROperand p0, IROperand p1, IROperand p2)
{
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, p0,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, p1,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, p2,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));
}

/* ==================================================== small_memset_to_store */

UT_TEST(test_memset_local_size4_single_int32_store)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &ms, TOK_MEMSET);

  int icall = emit_memset_call(ir, callee, 1,
                               utb_stackoff(16, 0, 0, 0, I32), /* dst */
                               utb_imm(4, I32),                /* size */
                               utb_imm(0, I32));               /* fill */

  int changes = tcc_ir_opt_small_memset_to_store(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_dest(ir, icall).u.imm32, 16);
  UT_ASSERT_EQ(irop_get_btype(utb_dest(ir, icall)), IROP_BTYPE_INT32);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, icall)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, icall)), 0);
  /* The three PARAM slots were NOPed. */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_local_size8_single_int64_store)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_AEABI_MEMSET, "__aeabi_memset");
  IROperand callee = utb_callee(ir, &ms, TOK_AEABI_MEMSET);

  int icall = emit_memset_call(ir, callee, 1,
                               utb_stackoff(0, 0, 0, 0, I32),
                               utb_imm(8, I32), utb_imm(0, I32));

  UT_ASSERT_EQ(tcc_ir_opt_small_memset_to_store(ir), 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_STORE);
  UT_ASSERT_EQ(irop_get_btype(utb_dest(ir, icall)), IROP_BTYPE_INT64);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_local_size2_and_size1)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &ms, TOK_MEMSET);
  int c2 = emit_memset_call(ir, callee, 1, utb_stackoff(0, 0, 0, 0, I32),
                            utb_imm(2, I32), utb_imm(0, I32));
  int c1 = emit_memset_call(ir, callee, 2, utb_stackoff(8, 0, 0, 0, I32),
                            utb_imm(1, I32), utb_imm(0, I32));

  UT_ASSERT_EQ(tcc_ir_opt_small_memset_to_store(ir), 2);
  UT_ASSERT_EQ(irop_get_btype(utb_dest(ir, c2)), IROP_BTYPE_INT16);
  UT_ASSERT_EQ(irop_get_btype(utb_dest(ir, c1)), IROP_BTYPE_INT8);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_local_size3_two_stores_repurposes_param_slot)
{
  /* size 3 -> INT16@+0, INT8@+2.  The 2-chunk path repurposes the closest
   * preceding PARAM slot (the fill param, at icall-1) as the second STORE. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &ms, TOK_MEMSET);
  int icall = emit_memset_call(ir, callee, 1, utb_stackoff(16, 0, 0, 0, I32),
                               utb_imm(3, I32), utb_imm(0, I32));

  UT_ASSERT_EQ(tcc_ir_opt_small_memset_to_store(ir), 1);
  /* First STORE at the call slot: INT16 at offset 16. */
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_STORE);
  UT_ASSERT_EQ(irop_get_btype(utb_dest(ir, icall)), IROP_BTYPE_INT16);
  UT_ASSERT_EQ(utb_dest(ir, icall).u.imm32, 16);
  /* Second STORE at icall-1 (the fill PARAM): INT8 at offset 18. */
  UT_ASSERT_EQ(utb_op(ir, icall - 1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(irop_get_btype(utb_dest(ir, icall - 1)), IROP_BTYPE_INT8);
  UT_ASSERT_EQ(utb_dest(ir, icall - 1).u.imm32, 18);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_local_size7_bails_needs_three_stores)
{
  /* size 7 = 4+2+1 -> three chunks; after two, remaining=1 -> skip. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &ms, TOK_MEMSET);
  int icall = emit_memset_call(ir, callee, 1, utb_stackoff(0, 0, 0, 0, I32),
                               utb_imm(7, I32), utb_imm(0, I32));

  UT_ASSERT_EQ(tcc_ir_opt_small_memset_to_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_local_size_over_cap_bails)
{
  /* size 9 > 8 -> bail. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &ms, TOK_MEMSET);
  int icall = emit_memset_call(ir, callee, 1, utb_stackoff(0, 0, 0, 0, I32),
                               utb_imm(9, I32), utb_imm(0, I32));

  UT_ASSERT_EQ(tcc_ir_opt_small_memset_to_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_local_nonzero_fill_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &ms, TOK_MEMSET);
  int icall = emit_memset_call(ir, callee, 1, utb_stackoff(0, 0, 0, 0, I32),
                               utb_imm(4, I32), utb_imm(7, I32)); /* fill != 0 */

  UT_ASSERT_EQ(tcc_ir_opt_small_memset_to_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_local_non_stack_dest_kept)
{
  /* A global/vreg dest is not a stack local -> belongs to the sibling pass. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &ms, TOK_MEMSET);
  int icall = emit_memset_call(ir, callee, 1, utb_temp(0, I32) /* not stackoff */,
                               utb_imm(4, I32), utb_imm(0, I32));

  UT_ASSERT_EQ(tcc_ir_opt_small_memset_to_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_local_unknown_callee_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "not_memset");
  IROperand callee = utb_callee(ir, &ms, TOK_MEMSET);
  int icall = emit_memset_call(ir, callee, 1, utb_stackoff(0, 0, 0, 0, I32),
                               utb_imm(4, I32), utb_imm(0, I32));

  UT_ASSERT_EQ(tcc_ir_opt_small_memset_to_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_local_lval_dest_kept)
{
  /* dst passed as lval (a deref) is rejected: `!p_dst.is_lval` guard. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &ms, TOK_MEMSET);
  int icall = emit_memset_call(ir, callee, 1, utb_lval(utb_stackoff(0, 0, 0, 0, I32)),
                               utb_imm(4, I32), utb_imm(0, I32));

  UT_ASSERT_EQ(tcc_ir_opt_small_memset_to_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID);
  utb_free(ir);
  return 0;
}

/* ======================================== small_global_memset_to_store */

/* Build a global memset: dst is a SYMREF (is_local=0, is_lval=0).  arg_order
 * selects how [p1,p2] are laid out: "memset"= [fill,size], "aeabi"= [size,fill]. */
static int emit_global_memset(TCCIRState *ir, IROperand callee, int call_id,
                              IROperand dst, IROperand a, IROperand b, int is_aeabi)
{
  /* memset(dst, fill=a, size=b);  aeabi(dst, size=a, fill=b) */
  (void)is_aeabi;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, dst,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, a,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, b,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));
}

UT_TEST(test_memset_global_size4_folds)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym gv;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &gv, TOK_MEMSET);
  IROperand gdst = utb_symref(ir, &gv, 0, 0, 0, I32); /* global, non-lval */

  int icall = emit_global_memset(ir, callee, 1, gdst,
                                 utb_imm(0, I32), /* fill (memset arg1) */
                                 utb_imm(4, I32), /* size (memset arg2) */
                                 0);

  UT_ASSERT_EQ(tcc_ir_opt_small_global_memset_to_store(ir), 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_STORE);
  UT_ASSERT_EQ(irop_get_btype(utb_dest(ir, icall)), IROP_BTYPE_INT32);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_global_aeabi_arg_order_swap)
{
  /* __aeabi_memset(dst, size, fill): size is arg1, fill is arg2.  The pass
   * swaps the interpretation; a non-zero "fill" in arg2 position blocks it. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym gv;
  utb_set_tok_str(TOK_AEABI_MEMSET, "__aeabi_memset");
  IROperand callee = utb_callee(ir, &gv, TOK_AEABI_MEMSET);
  IROperand gdst = utb_symref(ir, &gv, 0, 0, 0, I32);

  /* aeabi order: arg1=size(4), arg2=fill(0) */
  int icall = emit_global_memset(ir, callee, 1, gdst,
                                 utb_imm(4, I32), utb_imm(0, I32), 1);
  UT_ASSERT_EQ(tcc_ir_opt_small_global_memset_to_store(ir), 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_STORE);

  /* Non-zero fill in the aeabi arg2 (fill) slot blocks the fold. */
  TCCIRState *ir2 = utb_new();
  utb_pools_init(ir2);
  static Sym gv2;
  IROperand callee2 = utb_callee(ir2, &gv2, TOK_AEABI_MEMSET);
  IROperand gdst2 = utb_symref(ir2, &gv2, 0, 0, 0, I32);
  int icall2 = emit_global_memset(ir2, callee2, 1, gdst2,
                                  utb_imm(4, I32), utb_imm(9, I32), 1);
  UT_ASSERT_EQ(tcc_ir_opt_small_global_memset_to_store(ir2), 0);
  UT_ASSERT_EQ(utb_op(ir2, icall2), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  utb_free(ir2);
  return 0;
}

UT_TEST(test_memset_global_size3_bails_single_store_only)
{
  /* Global pass emits exactly one store; size 3 doesn't fit a single str/strh
   * -> bail (unlike the local pass which can do two). */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym gv;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &gv, TOK_MEMSET);
  IROperand gdst = utb_symref(ir, &gv, 0, 0, 0, I32);
  int icall = emit_global_memset(ir, callee, 1, gdst,
                                 utb_imm(0, I32), utb_imm(3, I32), 0);

  UT_ASSERT_EQ(tcc_ir_opt_small_global_memset_to_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memset_global_funccallval_with_reader_kept)
{
  /* FUNCCALLVAL form whose result vreg is read somewhere cannot be dropped. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym gv;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = utb_callee(ir, &gv, TOK_MEMSET);
  IROperand gdst = utb_symref(ir, &gv, 0, 0, 0, I32);

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, gdst,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(4, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 2), I32));
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 3), I32));
  /* A reader of the result T0: */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_small_global_memset_to_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);
  utb_free(ir);
  return 0;
}

UT_COVERS("small_memset_to_store");
UT_COVERS("small_global_memset_to_store");
