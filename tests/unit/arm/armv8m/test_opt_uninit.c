/*
 *  test_opt_uninit.c - suite for ir/opt_dce.c uninit-UB collapse passes
 *
 *  Covers two entry points from the same TU:
 *
 *  1. tcc_ir_opt_uninit_local_ub(): O2-only UB exploit that collapses a function
 *     body to a single self-jump when the entry basic block unconditionally
 *     reads a local VAR before any write to it.  The collapse is gated on the
 *     absence of observable side effects that can reach a return (or on the
 *     function being provably non-returning).
 *
 *  2. tcc_ir_opt_uninit_dominates_return(): generalises the above to any read
 *     of an uninitialised local VAR that dominates every RETURNVALUE/RETURNVOID
 *     and every implicit return jump.  Also O2-only and also collapses to a
 *     self-jump.
 *
 *  Both passes bail on inline asm / computed goto (IJUMP) and address-taken
 *  locals.  The passes are isolated here by driving the bare TCCIRState* entry
 *  points on hand-built IR.
 */

#include "ir_build.h"

#include "opt_engine.h"
#include "ut.h"

/* Pass entry points (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_uninit_local_ub(TCCIRState *ir);
int tcc_ir_opt_uninit_dominates_return(TCCIRState *ir);
int tcc_ir_opt_uninit_local_ub_ex(IROptCtx *ctx);
int tcc_ir_opt_uninit_dominates_return_ex(IROptCtx *ctx);

#define I32 IROP_BTYPE_INT32

/* ------------------------------------------------------------------ helpers */

static void setup_optimize(void)
{
  /* Both passes are gated on tcc_state->optimize >= 2. */
  tcc_state->optimize = 2;
}

static void reset_optimize(void)
{
  tcc_state->optimize = 0;
}

/* Build an address-of operand for a local VAR (is_local=1, is_lval=0), which
 * the passes treat as "address taken" for that VAR. */
static IROperand utb_var_addr(int pos, int btype)
{
  IROperand op = utb_var(pos, btype);
  op.is_local = 1;
  return op;
}

/* ========================================================== uninit_local_ub */

/* POSITIVE: entry block reads V0 before any write; no observable side effects;
 * no return.  The whole body collapses to a single self-jump.
 *   i0: T0 = V0 ADD #1   ->  JUMP -> 0
 */
UT_TEST(test_uninit_ub_entry_read_collapses)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_uninit_local_ub(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* POSITIVE: entry block reads uninit V0 and the function has no returning path
 * (infinite loop).  Collapse to self-jump.
 *   i0: T0 = V0 ADD #1
 *   i1: JUMP -> 1        ->  JUMP -> 0
 */
UT_TEST(test_uninit_ub_no_return_path_collapses)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_uninit_local_ub(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* GUARD: V0 is written before it is read, so the read is not uninitialised.
 *   i0: V0 = ASSIGN #5
 *   i1: T0 = V0 ADD #1
 *   i2: RETURNVALUE #0
 */
UT_TEST(test_uninit_ub_written_before_read_no_change)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_uninit_local_ub(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* GUARD: V0's address is taken, so a pointer write may initialise it.  No
 * collapse even though V0 is read before any direct write.
 *   i0: T0 = LEA &V0
 *   i1: T1 = V0 ADD #1
 *   i2: JUMP -> 2
 */
UT_TEST(test_uninit_ub_addrtaken_no_change)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var_addr(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_uninit_local_ub(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_LEA);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* GUARD: an IJUMP in the function makes control-flow targets unknown; the pass
 * bails conservatively.
 *   i0: T0 = V0 ADD #1
 *   i1: IJUMP T2
 *   i2: JUMP -> 2
 */
UT_TEST(test_uninit_ub_ijump_no_change)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_IJUMP, UTB_NONE, utb_temp(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_uninit_local_ub(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_IJUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* GUARD: an observable side effect before the uninit read ends the entry-block
 * scan before the read is seen, so no collapse occurs.
 *   i0: FUNCCALLVOID foo
 *   i1: T0 = V0 ADD #1
 *   i2: RETURNVALUE #0
 */
UT_TEST(test_uninit_ub_side_effect_before_read_no_change)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_imm(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_uninit_local_ub(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* GUARD: the pass is gated on optimize >= 2; at lower levels it must not fire. */
UT_TEST(test_uninit_ub_optimize_gate)
{
  TCCIRState *ir = utb_new();
  tcc_state->optimize = 0;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_uninit_local_ub(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* ===================================================== uninit_dominates_return */

/* POSITIVE: the first read of uninit V0 dominates the only RETURNVALUE and there
 * are no observable side effects.  The body collapses to a self-jump.
 *   i0: T0 = V0 ADD #1   ->  JUMP -> 0
 *   i1: RETURNVALUE #0
 */
UT_TEST(test_uninit_dom_ret_read_dominates_return_collapses)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_uninit_dominates_return(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* GUARD: a return block that is not dominated by the uninit read prevents
 * collapse.  The early return on the fall-through path is reachable without
 * executing the read.
 *   i0: JUMPIF -> 2, cond T0
 *   i1: RETURNVALUE #1
 *   i2: T0 = V0 ADD #1
 *   i3: RETURNVALUE #0
 */
UT_TEST(test_uninit_dom_ret_return_not_dominated_no_change)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_uninit_dominates_return(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* GUARD: observable side effects before the dominated return keep the body
 * intact.
 *   i0: T0 = V0 ADD #1
 *   i1: FUNCCALLVOID foo
 *   i2: RETURNVALUE #0
 */
UT_TEST(test_uninit_dom_ret_side_effects_no_change)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_imm(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_uninit_dominates_return(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* GUARD: if the function has no explicit or implicit return, the pass has
 * nothing to dominate and returns 0 (the case is handled by uninit_local_ub).
 *   i0: T0 = V0 ADD #1
 *   i1: JUMP -> 1
 */
UT_TEST(test_uninit_dom_ret_no_returns_no_change)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_uninit_dominates_return(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

/* WRAPPER: IROptCtx entry points forward to the bare TCCIRState* passes. */

static IROptCtx utb_ctx(TCCIRState *ir)
{
  IROptCtx ctx = {0};
  ctx.ir = ir;
  return ctx;
}

UT_TEST(test_uninit_local_ub_ex_forwards)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));

  IROptCtx ctx = utb_ctx(ir);
  int changes = tcc_ir_opt_uninit_local_ub_ex(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

UT_TEST(test_uninit_dominates_return_ex_forwards)
{
  TCCIRState *ir = utb_new();
  setup_optimize();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptCtx ctx = utb_ctx(ir);
  int changes = tcc_ir_opt_uninit_dominates_return_ex(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_optimize();
  return 0;
}

UT_COVERS("uninit_ub");
UT_COVERS("uninit_dom_ret");
