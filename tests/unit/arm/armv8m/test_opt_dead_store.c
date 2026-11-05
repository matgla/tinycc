/*
 *  test_opt_dead_store.c - suite for the dead-store / dead-code passes in
 *  ir/opt_dce.c: dse, dead_var_store_elim, dead_addrvar_elim,
 *  dead_trailing_addrvar_store_elim, zero_vla_elim, dead_before_infinite_loop,
 *  infinite_loop_simplify.
 *
 *  These passes are the store-forwarding/dead-store seam the differential
 *  fuzzer flags as the dominant optimizer bug-density cluster (see
 *  docs/plan_fuzz_coverage_master.md and docs/plan_ut_next_steps.md P1a).
 *  Each pass gets a positive case (the transform fires) and a negative/guard
 *  case (a legitimate reason the transform must NOT fire).
 */

#include "ir_build.h"

#include "opt_engine.h"
#include "ut.h"

/* Pass entry points (defined in ir/opt_dce.c; forward-declared here to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_dead_var_store_elim(TCCIRState *ir);
int tcc_ir_opt_dse(TCCIRState *ir);
int tcc_ir_opt_dead_addrvar_elim(TCCIRState *ir);
int tcc_ir_opt_zero_vla_elim(TCCIRState *ir);
int tcc_ir_opt_dead_before_infinite_loop(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* ------------------------------------------------------------------ helpers */

static int emit_jump(TCCIRState *ir, int tgt)
{
  return utb_emit(ir, TCCIR_OP_JUMP, utb_imm(tgt, I32), UTB_NONE, UTB_NONE);
}

static int emit_jumpif(TCCIRState *ir, int tgt, int cond)
{
  return utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(tgt, I32), utb_temp(cond, I32), UTB_NONE);
}

/* A LEA-deref lvalue: the TEMP holding an address, used as a memory operand. */
static IROperand utb_deref_temp(int pos, int btype)
{
  return utb_lval(utb_temp(pos, btype));
}

static void utb_alloc_var_intervals(TCCIRState *ir, int count)
{
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->variables_live_intervals_size = count;
}

/* CFG-building passes (infinite_loop_simplify -> tcc_ir_detect_loops ->
 * tcc_ir_cfg_build) grow via pool_add()/insert_instruction_before(), which
 * need the *_capacity/_size bookkeeping fields set to the real allocated
 * sizes (utb_new() only pre-fills the buffers). See test_opt_licm.c. */
static TCCIRState *utb_loop_new(void)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  ir->compact_instructions_size = UTB_MAX_INSTR;
  return ir;
}

/* ================================================================== dead_var_store_elim */

/* POSITIVE: V0 is written and never read anywhere -> the write is dead.
 * V1 is a dummy second VAR: max_var tracks the *highest* VAR position seen,
 * so a function referencing only V0 (position 0) leaves max_var == 0, which
 * the pass treats as its "no VARs at all" sentinel and bails out entirely
 * (see the guard test below). Adding V1 keeps max_var > 0 so the pass
 * actually reaches V0. */
UT_TEST(test_dead_var_store_unread_var_removed)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 2);

  int dead = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_var(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_var_store_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: V0 is written then read -> the write survives. V1 is also read
 * (not just written), so it doesn't confound the assertion by being
 * legitimately eliminated as its own dead store. */
UT_TEST(test_dead_var_store_read_var_kept)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 2);

  int store = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  int store1 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);
  int read = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int read1 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_var_store_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, store1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, read), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, read1), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* GUARD (documented limitation, not fixed here): when V0 (position 0) is the
 * *only* VAR referenced anywhere, max_var stays 0 (position, not count), which
 * collides with the pass's "no VARs referenced" sentinel and makes it bail
 * out via `if (max_var == 0) return 0;` -- even though V0 is trivially dead.
 * Pinned per PASS_COVERAGE.md working rule: characterize, don't silently fix
 * production code in a coverage commit. */
UT_TEST(test_dead_var_store_solo_var0_bails_out_suspected_bug)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 1);

  int dead = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_var_store_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_ASSIGN); /* NOT eliminated -- bug */

  utb_free(ir);
  return 0;
}

/* ================================================================== dead_addrvar_elim */

/* POSITIVE: V0's address is taken and stored through, but V0 is never read
 * anywhere -> both the LEA and the pointer STORE are dead. */
UT_TEST(test_dead_addrvar_unread_lea_and_store_removed)
{
  TCCIRState *ir = utb_new();

  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(0, I32), utb_imm(42, I32), UTB_NONE);
  /* Dummy second VAR read so max_var > 0 (see the max_var==0 sentinel note
   * on the dead_var_store_elim guard test above -- the same limitation
   * applies to every VAR-cascading pass in this file). */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_var(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_addrvar_elim(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: same shape, but V0 is also read directly elsewhere -> the LEA and
 * the pointer STORE must survive. */
UT_TEST(test_dead_addrvar_read_var_keeps_lea_and_store)
{
  TCCIRState *ir = utb_new();

  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(0, I32), utb_imm(42, I32), UTB_NONE);
  int read = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_addrvar_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_LEA);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, read), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* POSITIVE: V0's address reaches the store through a VAR-to-VAR copy
 * (V1 <-- T0; V2 <-- V1; T1 <-- V2; *T1 = 42) and V0 is never read: the LEA,
 * the store and every copy of the address go together. */
UT_TEST(test_dead_addrvar_var_copy_chain_removed)
{
  TCCIRState *ir = utb_new();

  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int c1 = utb_emit(ir, TCCIR_OP_STORE, utb_var(1, I32), utb_temp(0, I32), UTB_NONE);
  int c2 = utb_emit(ir, TCCIR_OP_STORE, utb_var(2, I32), utb_var(1, I32), UTB_NONE);
  int c3 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_var(2, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(1, I32), utb_imm(42, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_addrvar_elim(ir);

  UT_ASSERT_EQ(changes, 5);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c3), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the copied address also escapes (returned), so V0 is live through
 * it: nothing may go.  The copy chain once hid this and the LEA was deleted
 * under its remaining users (gcc 990128-1). */
UT_TEST(test_dead_addrvar_var_copy_chain_escape_keeps_lea)
{
  TCCIRState *ir = utb_new();

  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int c1 = utb_emit(ir, TCCIR_OP_STORE, utb_var(1, I32), utb_temp(0, I32), UTB_NONE);
  int c2 = utb_emit(ir, TCCIR_OP_STORE, utb_var(2, I32), utb_var(1, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(0, I32), utb_imm(42, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_var(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_addrvar_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_LEA);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: V0's address is copied into a PARAM vreg, which the frame
 * points-to facts do not follow, so V0 escapes there: nothing may go.  (The
 * facts once took that copy for a tracked one and dse deleted a local's
 * initialiser under a call in tinycc's decl_designator.) */
UT_TEST(test_dead_addrvar_copy_into_param_keeps_all)
{
  TCCIRState *ir = utb_new();
  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int init = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(7, I32), UTB_NONE);
  int copy = utb_emit(ir, TCCIR_OP_STORE, utb_param(1, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_param(1, I32), UTB_NONE);
  int changes = tcc_ir_opt_dead_addrvar_elim(ir);
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_LEA);
  UT_ASSERT_EQ(utb_op(ir, init), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, copy), TCCIR_OP_STORE);
  utb_free(ir);
  return 0;
}

/* POSITIVE: the address only travels through pointer arithmetic into stores,
 * and V0 is never read: LEA, ADD, both stores go (the facts follow ADD/SUB). */
UT_TEST(test_dead_addrvar_arith_derived_pointer_removed)
{
  TCCIRState *ir = utb_new();
  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int st0 = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  int st1 = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(1, I32), utb_imm(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_var(1, I32), UTB_NONE);
  int changes = tcc_ir_opt_dead_addrvar_elim(ir);
  UT_ASSERT_EQ(changes, 4);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, st0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, st1), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

/* ================================================================== zero_vla_elim */

/* POSITIVE: a VLA_ALLOC whose size resolves to the compile-time constant 0
 * doesn't change SP -> NOPed. */
UT_TEST(test_zero_vla_zero_size_alloc_removed)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(0, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_zero_vla_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a non-zero-size VLA_ALLOC is a real SP-changing allocation and
 * must survive. */
UT_TEST(test_zero_vla_nonzero_size_alloc_kept)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(16, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_zero_vla_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_VLA_ALLOC);

  utb_free(ir);
  return 0;
}

/* ================================================================== dead_before_infinite_loop */

/* POSITIVE: an ADD that feeds nothing, followed immediately by an empty
 * infinite loop (JUMP to self) -- the ADD can never be observed since the
 * function never returns, so it is NOPed; the self-jump sink survives. Gated
 * on tcc_state->optimize >= 2. */
UT_TEST(test_dead_before_inf_loop_dead_prologue_removed)
{
  TCCIRState *ir = utb_new();
  tcc_state->optimize = 2;

  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int sink = emit_jump(ir, 1); /* self-jump: index 1 -> 1 */

  int changes = tcc_ir_opt_dead_before_infinite_loop(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, sink), TCCIR_OP_JUMP);

  tcc_state->optimize = 0;
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the ADD's value can still reach a live RETURNVALUE via the
 * JUMPIF fallthrough edge, so nothing before the loop is actually dead.
 *   0: T0 = ADD #1,#2
 *   1: JUMPIF -> 3, T0        (taken: enter the infinite loop)
 *   2: RETURNVALUE T0         (fallthrough: T0 observed -> anchor)
 *   3: JUMP -> 3              (self-jump sink)
 */
UT_TEST(test_dead_before_inf_loop_reachable_anchor_keeps_prologue)
{
  TCCIRState *ir = utb_new();
  tcc_state->optimize = 2;

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int jumpif = emit_jumpif(ir, 3, 0);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  int sink = emit_jump(ir, 3);

  int changes = tcc_ir_opt_dead_before_infinite_loop(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_op(ir, sink), TCCIR_OP_JUMP);

  tcc_state->optimize = 0;
  utb_free(ir);
  return 0;
}


/* Dead-loop elimination is owned by the SSA pass ssa:dead_loop
 * (ir/opt/ssa_opt_dead_loop.c, exercised by test_ssa_opt_dead_loop.c and IR
 * pins 327/328).  The legacy pre-SSA tcc_ir_opt_dead_loop_elim was retired
 * 2026-07-07 (proven inert at its tccgen site — see
 * docs/plan_legacy_loop_dead_loop_elim_ssa.md), so its two unit tests
 * (const-VAR hoist positive, _ex-empty) were removed with the symbol. */

/* ================================================================== direct entry points */

UT_TEST(test_dead_var_store_elim_direct_call)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 2);
  int dead = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(9, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_var(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_var_store_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

UT_TEST(test_dead_addrvar_elim_direct_call)
{
  TCCIRState *ir = utb_new();
  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(0, I32), utb_imm(42, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_var(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_addrvar_elim(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_op(ir, lea), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

UT_TEST(test_zero_vla_elim_direct_call)
{
  TCCIRState *ir = utb_new();
  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(0, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_zero_vla_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

UT_TEST(test_dead_before_infinite_loop_direct_call)
{
  TCCIRState *ir = utb_new();
  tcc_state->optimize = 2;

  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 1);

  int changes = tcc_ir_opt_dead_before_infinite_loop(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);

  tcc_state->optimize = 0;
  utb_free(ir);
  return 0;
}

enum DseWawGuard
{
  DSE_WAW_NONE,
  DSE_WAW_READ,
  DSE_WAW_ALIAS_READ,
  DSE_WAW_CALL,
  DSE_WAW_TARGET,
  DSE_WAW_PARTIAL,
  DSE_WAW_SELF_READ,
  DSE_WAW_VOLATILE,
  DSE_WAW_SCALED,
  DSE_WAW_REDEFINE
};

static TCCIRState *dse_waw_fixture(enum DseWawGuard guard, int *lo, int *hi)
{
  TCCIRState *ir = utb_new();
  utb_alloc_var_intervals(ir, 2);
  ir->variables_live_intervals[1].addrtaken = 1;
  IROperand var = utb_var(1, IROP_BTYPE_INT64);
  IROperand ptr = utb_temp(1, I32);
  utb_emit(ir, TCCIR_OP_LEA, ptr, var, UTB_NONE);
  *lo = utb_emit4(ir, TCCIR_OP_STORE_INDEXED, ptr, utb_imm(0, I32), utb_imm(0, I32),
                  utb_imm(guard == DSE_WAW_SCALED ? 1 : 0, I32));
  *hi = utb_emit4(ir, TCCIR_OP_STORE_INDEXED, ptr, utb_imm(0, I32), utb_imm(4, I32), utb_imm(0, I32));
  if (guard == DSE_WAW_READ)
    utb_emit(ir, TCCIR_OP_LOAD, utb_temp(4, I32), utb_lval(ptr), UTB_NONE);
  if (guard == DSE_WAW_ALIAS_READ)
    utb_emit(ir, TCCIR_OP_LOAD, utb_temp(4, I32), utb_lval(utb_param(0, I32)), UTB_NONE);
  if (guard == DSE_WAW_CALL)
    utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  if (guard == DSE_WAW_REDEFINE)
    utb_emit(ir, TCCIR_OP_ASSIGN, ptr, utb_param(0, I32), UTB_NONE);
  IROperand dest = guard == DSE_WAW_PARTIAL ? utb_var(1, I32) : var;
  IROperand value = guard == DSE_WAW_SELF_READ ? var : utb_param(1, IROP_BTYPE_INT64);
  int overwrite = utb_emit(ir, TCCIR_OP_ASSIGN, dest, value, UTB_NONE);
  if (guard == DSE_WAW_TARGET)
    ir->compact_instructions[overwrite].is_jump_target = 1;
  if (guard == DSE_WAW_VOLATILE)
  {
    ir->func_has_volatile_access = 1;
    ir->variables_live_intervals[1].is_volatile = 1;
  }
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(3, IROP_BTYPE_INT64), utb_lval(ptr), UTB_NONE);
  if (guard == DSE_WAW_READ || guard == DSE_WAW_ALIAS_READ)
  {
    utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, IROP_BTYPE_INT64), utb_temp(3, IROP_BTYPE_INT64), utb_temp(4, I32));
    utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(5, IROP_BTYPE_INT64), UTB_NONE);
  }
  else
    utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, IROP_BTYPE_INT64), UTB_NONE);
  return ir;
}

UT_TEST(test_dse_addrvar_indexed_zeroing_fully_overwritten)
{
  int ops[] = {TCCIR_OP_ASSIGN, TCCIR_OP_LOAD, TCCIR_OP_STORE};
  for (unsigned k = 0; k < sizeof(ops) / sizeof(ops[0]); k++)
  {
    int lo, hi;
    TCCIRState *ir = dse_waw_fixture(DSE_WAW_NONE, &lo, &hi);
    ir->compact_instructions[hi + 1].op = ops[k];
    UT_ASSERT(tcc_ir_opt_dse(ir) >= 2);
    UT_ASSERT_EQ(utb_op(ir, lo), TCCIR_OP_NOP);
    UT_ASSERT_EQ(utb_op(ir, hi), TCCIR_OP_NOP);
    utb_free(ir);
  }
  return 0;
}

UT_TEST(test_dse_addrvar_indexed_zeroing_guards)
{
  for (int guard = DSE_WAW_READ; guard <= DSE_WAW_REDEFINE; guard++)
  {
    int lo, hi;
    TCCIRState *ir = dse_waw_fixture(guard, &lo, &hi);
    tcc_ir_opt_dse(ir);
    UT_ASSERT_EQ(utb_op(ir, lo), TCCIR_OP_STORE_INDEXED);
    UT_ASSERT_EQ(utb_op(ir, hi), TCCIR_OP_STORE_INDEXED);
    utb_free(ir);
  }
  return 0;
}

UT_COVERS("dead_var_store");   /* alias tracked by check_pass_coverage.py normalization */
UT_COVERS("dead_addrvar");
UT_COVERS("zero_vla");
UT_COVERS("dead_pre_inf");
