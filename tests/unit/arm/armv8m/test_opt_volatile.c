/*
 *  test_opt_volatile.c - volatility of a memory access, and who must respect it
 *
 *  Covers the two-layer model introduced with the volatile correctness sweep:
 *
 *    1. IROP_AUX_NONVOLATILE on the operand - "proven non-volatile", set by
 *       svalue_to_iroperand where the C type is still in hand.  It is the only
 *       signal that is right for `volatile int a[4]` (the qualifier sits on the
 *       ELEMENT type, so the array's Sym says nothing), for a volatile member
 *       of a plain struct, for a `(volatile T *)` cast, and for a bare
 *       `T***DEREF***`, which has no Sym at all.
 *    2. TCCIRState.func_has_volatile_access - a body that never touches
 *       volatile memory answers "not volatile" for EVERY operand, including the
 *       unmarked ones passes synthesize.  Without it the conservative default
 *       would switch off CSE/DSE compiler-wide.
 *
 *  Then the consumers: the instruction-level query, the fusion mark transfer,
 *  the lvalue-immediate encoding (`*(volatile T *)0x40000000`), and the two
 *  SSA passes that were deleting volatile accesses outright.
 *
 *  HARNESS NOTES:
 *    - Hand-built IR carries no marks, so a fixture that wants the per-operand
 *      checks live must BOTH set ir->func_has_volatile_access and mark the
 *      operands it intends to be non-volatile (utv_nonvol below).  That is the
 *      same asymmetry the production code has, and testing it is the point.
 *    - Links the real passes via UT11.
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"
#include "load_cse.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "source/opt/include/opt_utils.h"

#define I32 IROP_BTYPE_INT32

/* Mark an operand "proven non-volatile", the way svalue_to_iroperand does for
 * an lvalue built from a type without VT_VOLATILE. */
static inline IROperand utv_nonvol(IROperand op)
{
  op.aux |= IROP_AUX_NONVOLATILE;
  return op;
}

static void utv_enter_state(void)
{
  static TCCState tcc_state_storage;
  memset(&tcc_state_storage, 0, sizeof(tcc_state_storage));
  tcc_state_storage.optimize = 1;
  tcc_enter_state(&tcc_state_storage);
}

static void utv_exit_state(void)
{
  static TCCState tcc_state_storage;
  memset(&tcc_state_storage, 0, sizeof(tcc_state_storage));
  tcc_exit_state(&tcc_state_storage);
}

/* ========================================================================
 * Layer 2: with no volatile access in the function, nothing is volatile
 * ======================================================================== */

UT_TEST(test_volatile_func_flag_clear_means_never_volatile)
{
  TCCIRState *ir = utb_new();
  /* An unmarked lvalue deref - exactly what a pass synthesizes. */
  int ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);

  UT_ASSERT_EQ(ir->func_has_volatile_access, 0);
  UT_ASSERT_EQ(tcc_ir_access_is_volatile(ir, utb_src1(ir, ld)), 0);
  UT_ASSERT_EQ(tcc_ir_instr_access_is_volatile(ir, &ir->compact_instructions[ld]), 0);

  /* The bare operand predicate has no such escape hatch: unmarked is volatile. */
  UT_ASSERT_EQ(irop_access_is_volatile(utb_src1(ir, ld)), 1);

  utb_free(ir);
  return 0;
}

/* ========================================================================
 * Layer 1: once the function touches volatile, the per-operand mark decides
 * ======================================================================== */

UT_TEST(test_volatile_load_operand_mark_decides)
{
  TCCIRState *ir = utb_new();
  ir->func_has_volatile_access = 1;

  int vol = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);
  int nonvol = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32),
                        utv_nonvol(utb_lval(utb_temp(3, I32))), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_access_is_volatile(ir, utb_src1(ir, vol)), 1);
  UT_ASSERT_EQ(tcc_ir_access_is_volatile(ir, utb_src1(ir, nonvol)), 0);
  UT_ASSERT_EQ(tcc_ir_instr_access_is_volatile(ir, &ir->compact_instructions[vol]), 1);
  UT_ASSERT_EQ(tcc_ir_instr_access_is_volatile(ir, &ir->compact_instructions[nonvol]), 0);

  utb_free(ir);
  return 0;
}

/* An INDEXED op carries the mark on its BASE operand, which is NOT an lvalue -
 * the fusion passes move it there when they absorb the deref. */
UT_TEST(test_volatile_indexed_keys_off_the_base_operand)
{
  TCCIRState *ir = utb_new();
  ir->func_has_volatile_access = 1;

  int ldx = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32), utb_temp(1, I32),
                      utb_imm(4, I32), utb_imm(0, I32));
  int stx = utb_emit4(ir, TCCIR_OP_STORE_INDEXED, utb_temp(2, I32), utb_imm(7, I32),
                      utb_imm(4, I32), utb_imm(0, I32));

  UT_ASSERT_EQ(tcc_ir_instr_access_is_volatile(ir, &ir->compact_instructions[ldx]), 1);
  UT_ASSERT_EQ(tcc_ir_instr_access_is_volatile(ir, &ir->compact_instructions[stx]), 1);

  /* Mark the bases and both become ordinary accesses again. */
  tcc_ir_set_src1(ir, ldx, utv_nonvol(utb_temp(1, I32)));
  tcc_ir_set_dest(ir, stx, utv_nonvol(utb_temp(2, I32)));
  UT_ASSERT_EQ(tcc_ir_instr_access_is_volatile(ir, &ir->compact_instructions[ldx]), 0);
  UT_ASSERT_EQ(tcc_ir_instr_access_is_volatile(ir, &ir->compact_instructions[stx]), 0);

  utb_free(ir);
  return 0;
}

/* A plain ALU op over values is not a memory access, so it must not read as
 * volatile just because the function contains one somewhere. */
UT_TEST(test_volatile_instr_query_ignores_value_operands)
{
  TCCIRState *ir = utb_new();
  ir->func_has_volatile_access = 1;

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(1, I32), utb_imm(1, I32));
  UT_ASSERT_EQ(tcc_ir_instr_access_is_volatile(ir, &ir->compact_instructions[add]), 0);

  /* ...but an embedded deref operand IS one. */
  tcc_ir_set_src1(ir, add, utb_lval(utb_temp(1, I32)));
  UT_ASSERT_EQ(tcc_ir_instr_access_is_volatile(ir, &ir->compact_instructions[add]), 1);

  utb_free(ir);
  return 0;
}

/* ========================================================================
 * irop_carry_access_marks: UNDERALIGN accumulates, NONVOLATILE is COPIED
 * ======================================================================== */

UT_TEST(test_carry_access_marks_copies_nonvolatile)
{
  IROperand deref = utb_lval(utb_temp(1, I32));
  IROperand base = utb_temp(2, I32);

  /* deref proven non-volatile -> base ends up proven too */
  deref.aux |= IROP_AUX_NONVOLATILE;
  irop_carry_access_marks(&base, deref);
  UT_ASSERT((base.aux & IROP_AUX_NONVOLATILE) != 0);

  /* A volatile deref must CLEAR a stale mark on the base, not OR into it:
   * the bit means "proven", and inheriting it would license CSE. */
  deref.aux &= (uint8_t)~IROP_AUX_NONVOLATILE;
  irop_carry_access_marks(&base, deref);
  UT_ASSERT_EQ((int)(base.aux & IROP_AUX_NONVOLATILE), 0);

  /* UNDERALIGN keeps its OR semantics - either operand may know about it. */
  base.aux |= IROP_AUX_UNDERALIGN;
  irop_carry_access_marks(&base, deref);
  UT_ASSERT((base.aux & IROP_AUX_UNDERALIGN) != 0);
  deref.aux |= IROP_AUX_UNDERALIGN;
  base = utb_temp(2, I32);
  irop_carry_access_marks(&base, deref);
  UT_ASSERT((base.aux & IROP_AUX_UNDERALIGN) != 0);

  return 0;
}

/* ========================================================================
 * An lvalue immediate is an ADDRESS (`*(volatile T *)0x40000000`), not a value
 * ======================================================================== */

UT_TEST(test_lvalue_immediate_is_an_address_not_a_constant)
{
  IROperand value = utb_imm(0x40000000, I32);
  IROperand addr = utb_lval(utb_imm(0x40000000, I32));

  UT_ASSERT(irop_is_immediate(value));
  UT_ASSERT_EQ(irop_is_lval_imm_addr(value), 0);

  /* The whole point: no folder may treat this as the constant 0x40000000. */
  UT_ASSERT_EQ(irop_is_immediate(addr), 0);
  UT_ASSERT_EQ(irop_is_plain_imm(addr), 0);
  UT_ASSERT(irop_is_lval_imm_addr(addr));
  UT_ASSERT_EQ((int32_t)irop_get_imm64_ex(NULL, addr), 0x40000000);

  return 0;
}

/* ========================================================================
 * ssa:load_cse - a volatile indexed load is neither reused nor reusable
 * ======================================================================== */

UT_TEST(test_load_cse_keeps_volatile_indexed_load)
{
  utv_enter_state();
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  c.ir->func_has_volatile_access = 1;

  /* t0 = *(t1 + #4); t2 = *(t1 + #4) - same address, unmarked base. */
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32),
                 utb_temp(1, I32), utb_imm(4, I32), utb_imm(0, I32));
  int second = ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32),
                              utb_temp(1, I32), utb_imm(4, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, second), TCCIR_OP_LOAD_INDEXED);

  ssa_ctx_free(&c);
  utv_exit_state();
  return 0;
}

/* The control: the same shape with the base marked non-volatile still CSEs,
 * so the guard above is blocking volatility and not the whole rule. */
UT_TEST(test_load_cse_still_cses_marked_indexed_load)
{
  utv_enter_state();
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  c.ir->func_has_volatile_access = 1;

  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32),
                 utv_nonvol(utb_temp(1, I32)), utb_imm(4, I32), utb_imm(0, I32));
  int second = ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32),
                              utv_nonvol(utb_temp(1, I32)), utb_imm(4, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, second), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  utv_exit_state();
  return 0;
}

/* ========================================================================
 * ssa:dce - a volatile store is not dead, a volatile read is not dead
 * ======================================================================== */

static int utv_run_dce(IRSSAOptCtx *ctx)
{
  utv_enter_state();
  int changed = ssa_opt_dce(ctx);
  utv_exit_state();
  return changed;
}

UT_TEST(test_dce_keeps_overwritten_volatile_global_store)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  c.ir->func_has_volatile_access = 1;
  static Sym gv;
  gv.v = 100;

  /* Both writes must survive: the first is "overwritten" but still observable.
   * The Sym is deliberately NOT volatile-typed - this is the `volatile int
   * a[4]` / volatile-member shape, where only the operand mark knows. */
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE, utb_symref(c.ir, &gv, 1, 0, 0, I32),
                             utb_imm(1, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE, utb_symref(c.ir, &gv, 1, 0, 0, I32),
                             utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  utv_run_dce(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* The control: unmarked-but-non-volatile function, the first store still dies. */
UT_TEST(test_dce_still_kills_overwritten_plain_global_store)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  static Sym gp;
  gp.v = 101;

  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE, utb_symref(c.ir, &gp, 1, 0, 0, I32),
                             utb_imm(1, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE, utb_symref(c.ir, &gp, 1, 0, 0, I32),
                             utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  utv_run_dce(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* `*p;` / `(void)REG;`: the loaded value is used by nothing, but the read is
 * the whole statement. */
UT_TEST(test_dce_keeps_volatile_read_with_unused_result)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  c.ir->func_has_volatile_access = 1;

  int vol = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), utb_lval(utb_temp(1, I32)));
  int plain = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                            utv_nonvol(utb_lval(utb_temp(3, I32))));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  utv_run_dce(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, vol), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_op(c.ir, plain), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}
