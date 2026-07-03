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

#include "ut.h"

/* Pass entry points (defined in ir/opt_dce.c; forward-declared here to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_dse(TCCIRState *ir);
int tcc_ir_opt_dead_var_store_elim(TCCIRState *ir);
int tcc_ir_opt_dead_addrvar_elim(TCCIRState *ir);
int tcc_ir_opt_dead_trailing_addrvar_store_elim(TCCIRState *ir);
int tcc_ir_opt_zero_vla_elim(TCCIRState *ir);
int tcc_ir_opt_dead_before_infinite_loop(TCCIRState *ir);
int tcc_ir_opt_infinite_loop_simplify(TCCIRState *ir);

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

/* ================================================================== dse */

/* POSITIVE: a TEMP def with zero uses is NOPed. T1 is a dummy second, *used*
 * temp: max_tmp_pos tracks the *highest* TEMP position seen (via DSE_ENSURE_CAP,
 * called for both defs and uses), so a function referencing only T0 (position
 * 0) leaves max_tmp_pos == 0, which the pass treats as its "no TEMPs at all"
 * sentinel and bails out entirely before ever inspecting T0 (see the guard
 * test below). Adding a used T1 keeps max_tmp_pos > 0 so the pass actually
 * reaches T0. */
UT_TEST(test_dse_dead_temp_removed)
{
  TCCIRState *ir = utb_new();

  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int live = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(5, I32), utb_imm(6, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_dse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, live), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a TEMP def that is read by RETURNVALUE survives; a genuinely dead
 * peer at a different position is still eliminated in the same run. */
UT_TEST(test_dse_used_temp_kept)
{
  TCCIRState *ir = utb_new();

  int live = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(5, I32), utb_imm(6, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, live), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* GUARD (documented limitation, not fixed here): when T0 (position 0) is the
 * *only* TEMP referenced anywhere, max_tmp_pos stays 0 (position, not count),
 * which collides with the pass's "no TEMPs referenced" sentinel and makes it
 * bail out via `if (max_tmp_pos == 0) return pure_call_changes;` -- even
 * though T0 is trivially dead. The same position-0-as-sentinel pattern as
 * dead_var_store_elim's guard below, in the TEMP table instead of the VAR
 * table. Pinned per PASS_COVERAGE.md working rule: characterize, don't
 * silently fix production code in a coverage commit. */
UT_TEST(test_dse_solo_temp0_bails_out_suspected_bug)
{
  TCCIRState *ir = utb_new();

  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_ADD); /* NOT eliminated -- bug */

  utb_free(ir);
  return 0;
}

/* POSITIVE (cascade): T1's only use is dead T0's producer; killing T1 drops
 * T0's use_count to 0, cascading the elimination to T0 too. */
UT_TEST(test_dse_cascades_through_chain)
{
  TCCIRState *ir = utb_new();

  int t0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int t1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dse(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_op(ir, t0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, t1), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
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

/* ========================================================= dead_trailing_addrvar_store_elim */

/* POSITIVE: a write to V0 through its address AFTER the last read of V0 is
 * dead; an earlier write (before the read) survives.
 *   T0 = LEA &V0
 *   *T0 <- #1              (kept: read follows)
 *   T1 = *T0                (last read of V0)
 *   *T0 <- #2               (dead: no read follows)         -> NOP
 */
UT_TEST(test_dead_trailing_addrvar_write_after_last_read_removed)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE); /* dummy 2nd VAR */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int kept_store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_deref_temp(0, I32), UTB_NONE);
  int dead_store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(0, I32), utb_imm(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_trailing_addrvar_store_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, kept_store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, dead_store), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the only write to V0 is followed by a read -> nothing
 * is trailing-dead, the store survives. */
UT_TEST(test_dead_trailing_addrvar_write_before_read_kept)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(1, I32), utb_imm(0, I32), UTB_NONE); /* dummy 2nd VAR */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_deref_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_trailing_addrvar_store_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

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

/* ================================================================== infinite_loop_simplify */

/* POSITIVE: a trivial infinite loop (header IS the entry, no preheader) whose
 * only body op is a constant store to a non-volatile global collapses to a
 * bare self-jump; the store can never be hoisted (no preheader exists) or
 * observed (the function never returns), so it is dropped outright.
 *   0: GlobalSym(X) <- #5   [STORE; header]
 *   1: JUMP -> 0             [back-edge]
 *   2: RETURNVOID            [unreachable filler so n >= 3]
 * -> 0: JUMP -> 0 (self-jump), 1: NOP */
UT_TEST(test_infinite_loop_simplify_dead_global_store_collapses_to_selfjump)
{
  TCCIRState *ir = utb_loop_new();
  tcc_state->optimize = 2;
  utb_pools_init(ir);

  Sym sym_x;
  memset(&sym_x, 0, sizeof(sym_x));
  IROperand gx = utb_symref(ir, &sym_x, /*is_lval*/ 1, /*is_local*/ 0, /*is_const*/ 0, I32);

  int header = utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(5, I32), UTB_NONE);
  int back_edge = emit_jump(ir, 0);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_infinite_loop_simplify(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, header), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, header)), 0);
  UT_ASSERT_EQ(utb_op(ir, back_edge), TCCIR_OP_NOP);

  tcc_state->optimize = 0;
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the loop body contains a pointer store (STORE_INDEXED),
 * which the pass cannot prove dead/hoistable -- it must leave the loop
 * untouched. */
UT_TEST(test_infinite_loop_simplify_indexed_store_blocks_collapse)
{
  TCCIRState *ir = utb_loop_new();
  tcc_state->optimize = 2;

  int header = utb_emit4(ir, TCCIR_OP_STORE_INDEXED, utb_temp(0, I32), utb_imm(7, I32), utb_imm(0, I32),
                          utb_imm(1, I32));
  int back_edge = emit_jump(ir, 0);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_infinite_loop_simplify(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, header), TCCIR_OP_STORE_INDEXED);
  UT_ASSERT_EQ(utb_op(ir, back_edge), TCCIR_OP_JUMP);

  tcc_state->optimize = 0;
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_dead_store)
{
  UT_COVERS("dse");
  UT_COVERS("dead_var_store");   /* alias tracked by check_pass_coverage.py normalization */
  UT_COVERS("dead_addrvar");
  UT_COVERS("dead_trail_addrvar");
  UT_COVERS("zero_vla");
  UT_COVERS("dead_pre_inf");
  UT_COVERS("inf_loop_simpl");

  UT_RUN(test_dse_dead_temp_removed);
  UT_RUN(test_dse_used_temp_kept);
  UT_RUN(test_dse_solo_temp0_bails_out_suspected_bug);
  UT_RUN(test_dse_cascades_through_chain);

  UT_RUN(test_dead_var_store_unread_var_removed);
  UT_RUN(test_dead_var_store_read_var_kept);
  UT_RUN(test_dead_var_store_solo_var0_bails_out_suspected_bug);

  UT_RUN(test_dead_addrvar_unread_lea_and_store_removed);
  UT_RUN(test_dead_addrvar_read_var_keeps_lea_and_store);

  UT_RUN(test_dead_trailing_addrvar_write_after_last_read_removed);
  UT_RUN(test_dead_trailing_addrvar_write_before_read_kept);

  UT_RUN(test_zero_vla_zero_size_alloc_removed);
  UT_RUN(test_zero_vla_nonzero_size_alloc_kept);

  UT_RUN(test_dead_before_inf_loop_dead_prologue_removed);
  UT_RUN(test_dead_before_inf_loop_reachable_anchor_keeps_prologue);

  UT_RUN(test_infinite_loop_simplify_dead_global_store_collapses_to_selfjump);
  UT_RUN(test_infinite_loop_simplify_indexed_store_blocks_collapse);
}
