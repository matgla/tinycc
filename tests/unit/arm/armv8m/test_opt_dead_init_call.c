/*
 *  test_opt_dead_init_call.c - suite for the Function-Write-Summary analysis and
 *  its sole consumer in ir/opt.c:
 *
 *    void tcc_ir_compute_func_write_summary(ir, func_sym);
 *    int  tcc_ir_opt_dead_init_via_call(ir);
 *
 *  FWS records, per pointer parameter, the byte ranges the function
 *  unconditionally writes before any read.  dead_init_via_call then kills a
 *  caller's stack-slot STORE when the callee it passes the slot's address to is
 *  provably going to overwrite exactly those bytes (and nothing reads them in
 *  between).  Corner cases pinned: full-coverage kill, partial-coverage keep,
 *  read-between-store-and-call keep, basic-block boundary bail, no-summary keep,
 *  empty IR, and FWS idempotency.
 */

#include "ir_build.h"

#include "ut.h"

void tcc_ir_compute_func_write_summary(TCCIRState *ir, Sym *func_sym);
int tcc_ir_opt_dead_init_via_call(TCCIRState *ir);
void tcc_ir_func_write_summary_clear_all(void);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Build a callee body that unconditionally writes `nbytes` bytes through
 * pointer parameter 0 at offset 0, then return its IR.  Caller frees. */
static TCCIRState *build_callee_writes_p0(int nbytes)
{
  TCCIRState *ir = utb_new();
  int bt = (nbytes >= 8) ? I64 : I32;
  /* STORE [P0] = #0  (write-through; dest is the param vreg, lval) */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_param(0, bt)), utb_imm(0, bt), UTB_NONE);
  return ir;
}

/* A SYMREF operand referencing `sym` (used as a callee in the caller). */
static IROperand utb_callee_ref(TCCIRState *ir, Sym *sym)
{
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* -------------------------------------------------- positive kill */

UT_TEST(test_dead_init_full_coverage_store_killed)
{
  tcc_ir_func_write_summary_clear_all();
  static Sym callee;
  TCCIRState *cir = build_callee_writes_p0(8);
  tcc_ir_compute_func_write_summary(cir, &callee);
  utb_free(cir);

  /* Caller: STORE [local@16] = #0 (INT64); pass &local@16 to callee. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(16, 1, 0, 0, I64),
                       utb_imm(0, I64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_stackoff(16, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_callee_ref(ir, &callee),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  int changes = tcc_ir_opt_dead_init_via_call(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);
  utb_free(ir);
  tcc_ir_func_write_summary_clear_all();
  return 0;
}

/* -------------------------------------------------- keeps */

UT_TEST(test_dead_init_partial_coverage_kept)
{
  /* Callee writes only 4 bytes [0..4); caller stores 8 bytes [0..8).  Byte 4
   * is not must-write -> fws_range_fully_set returns false -> store kept. */
  tcc_ir_func_write_summary_clear_all();
  static Sym callee;
  TCCIRState *cir = build_callee_writes_p0(4);
  tcc_ir_compute_func_write_summary(cir, &callee);
  utb_free(cir);

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(16, 1, 0, 0, I64),
                       utb_imm(0, I64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_stackoff(16, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_callee_ref(ir, &callee),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  UT_ASSERT_EQ(tcc_ir_opt_dead_init_via_call(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  utb_free(ir);
  tcc_ir_func_write_summary_clear_all();
  return 0;
}

UT_TEST(test_dead_init_read_between_store_and_call_kept)
{
  /* A LOAD of the slot between the STORE and the CALL makes the prior value
   * observable -> the init is not dead. */
  tcc_ir_func_write_summary_clear_all();
  static Sym callee;
  TCCIRState *cir = build_callee_writes_p0(8);
  tcc_ir_compute_func_write_summary(cir, &callee);
  utb_free(cir);

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(16, 1, 0, 0, I64),
                       utb_imm(0, I64), UTB_NONE);
  /* Read of the same bytes (lval stackoff, overlapping range): */
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_lval(utb_stackoff(16, 0, 0, 0, I32)), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_stackoff(16, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_callee_ref(ir, &callee),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  UT_ASSERT_EQ(tcc_ir_opt_dead_init_via_call(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  utb_free(ir);
  tcc_ir_func_write_summary_clear_all();
  return 0;
}

UT_TEST(test_dead_init_control_flow_boundary_kept)
{
  /* An intervening FUNCCALL is a basic-block boundary: the backward scan from
   * our call breaks at it, never reaching the store. */
  tcc_ir_func_write_summary_clear_all();
  static Sym callee, other;
  TCCIRState *cir = build_callee_writes_p0(8);
  tcc_ir_compute_func_write_summary(cir, &callee);
  utb_free(cir);

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(16, 1, 0, 0, I64),
                       utb_imm(0, I64), UTB_NONE);
  /* Barrier call (no summary for `other`) between store and our call. */
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_callee_ref(ir, &other),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_stackoff(16, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_callee_ref(ir, &callee),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  UT_ASSERT_EQ(tcc_ir_opt_dead_init_via_call(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  utb_free(ir);
  tcc_ir_func_write_summary_clear_all();
  return 0;
}

UT_TEST(test_dead_init_no_summary_kept)
{
  /* Callee with no computed FWS -> fws_lookup returns NULL -> nothing killed. */
  tcc_ir_func_write_summary_clear_all();
  static Sym callee; /* no compute_func_write_summary call */

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(16, 1, 0, 0, I64),
                       utb_imm(0, I64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_stackoff(16, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_callee_ref(ir, &callee),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  UT_ASSERT_EQ(tcc_ir_opt_dead_init_via_call(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  utb_free(ir);
  tcc_ir_func_write_summary_clear_all();
  return 0;
}

UT_TEST(test_dead_init_non_stack_param_kept)
{
  /* The argument passed is not Addr[StackLoc[]] (it's a vreg) -> the
   * `irop_get_tag(pop) != IROP_TAG_STACKOFF` guard rejects it. */
  tcc_ir_func_write_summary_clear_all();
  static Sym callee;
  TCCIRState *cir = build_callee_writes_p0(8);
  tcc_ir_compute_func_write_summary(cir, &callee);
  utb_free(cir);

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(16, 1, 0, 0, I64),
                       utb_imm(0, I64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32) /* not a stack addr */,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_callee_ref(ir, &callee),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  UT_ASSERT_EQ(tcc_ir_opt_dead_init_via_call(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  utb_free(ir);
  tcc_ir_func_write_summary_clear_all();
  return 0;
}

/* -------------------------------------------------- FWS analysis corners */

UT_TEST(test_fws_empty_ir_no_crash)
{
  tcc_ir_func_write_summary_clear_all();
  static Sym callee;
  TCCIRState *ir = utb_new(); /* n == 0 */
  tcc_ir_compute_func_write_summary(ir, &callee);
  /* No summary recorded for an empty body.  A caller init must survive. */
  TCCIRState *caller = utb_new();
  utb_pools_init(caller);
  int store = utb_emit(caller, TCCIR_OP_STORE, utb_stackoff(8, 1, 0, 0, I32),
                       utb_imm(0, I32), UTB_NONE);
  utb_emit(caller, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_stackoff(8, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(caller, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_callee_ref(caller, &callee),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));
  UT_ASSERT_EQ(tcc_ir_opt_dead_init_via_call(caller), 0);
  UT_ASSERT_EQ(utb_op(caller, store), TCCIR_OP_STORE);
  utb_free(ir);
  utb_free(caller);
  tcc_ir_func_write_summary_clear_all();
  return 0;
}

UT_TEST(test_fws_idempotent_second_call_noop)
{
  /* Computing the same function's summary twice must not duplicate or alter
   * it (fws_lookup short-circuits on the second call). */
  tcc_ir_func_write_summary_clear_all();
  static Sym callee;
  TCCIRState *cir = build_callee_writes_p0(8);
  tcc_ir_compute_func_write_summary(cir, &callee);
  /* Second compute on a different body for the same Sym must be ignored. */
  TCCIRState *cir2 = utb_new();
  tcc_ir_compute_func_write_summary(cir2, &callee); /* already known -> no-op */
  utb_free(cir2);

  /* Caller still sees the original 8-byte summary -> store killed. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(16, 1, 0, 0, I64),
                       utb_imm(0, I64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_stackoff(16, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_callee_ref(ir, &callee),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));
  UT_ASSERT_EQ(tcc_ir_opt_dead_init_via_call(ir), 1);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  utb_free(cir);
  utb_free(ir);
  tcc_ir_func_write_summary_clear_all();
  return 0;
}

/* -------------------------------------------------- dead_init guards */

UT_TEST(test_dead_init_empty_ir_no_crash)
{
  tcc_ir_func_write_summary_clear_all();
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_dead_init_via_call(ir), 0);
  UT_ASSERT_EQ(tcc_ir_opt_dead_init_via_call(NULL), 0);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_dead_init_call)
{
  UT_COVERS("compute_func_write_summary");
  UT_COVERS("dead_init_via_call");
  UT_RUN(test_dead_init_full_coverage_store_killed);
  UT_RUN(test_dead_init_partial_coverage_kept);
  UT_RUN(test_dead_init_read_between_store_and_call_kept);
  UT_RUN(test_dead_init_control_flow_boundary_kept);
  UT_RUN(test_dead_init_no_summary_kept);
  UT_RUN(test_dead_init_non_stack_param_kept);
  UT_RUN(test_fws_empty_ir_no_crash);
  UT_RUN(test_fws_idempotent_second_call_noop);
  UT_RUN(test_dead_init_empty_ir_no_crash);
}
