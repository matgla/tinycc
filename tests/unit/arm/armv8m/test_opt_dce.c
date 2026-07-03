/*
 *  test_opt_dce.c - suite for ir/opt_dce.c (legacy Dead Code Elimination)
 *
 *  DCE follows control-flow edges from instruction 0, marks reachable
 *  instructions, and NOPs the rest.  It returns the number of instructions
 *  it converted to NOP.
 */

#include "ir_build.h"
#include "ut.h"

/* Pass entry point (defined in ir/opt_dce.c). */
int tcc_ir_opt_dce(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Global pass-timing gate used by the timed wrapper in opt_dce.c. */
extern signed char tcc_pass_timing_on;

/* Token for naming a noreturn callee via the harness get_tok_str table. */
#define TOK_ABORT 300

/* Emit an unconditional JUMP to target index `tgt`. */
static int emit_jump(TCCIRState *ir, int tgt)
{
  return utb_emit(ir, TCCIR_OP_JUMP, utb_imm(tgt, I32), UTB_NONE, UTB_NONE);
}

/* Emit a JUMPIF with target index `tgt` and condition temp `cond`. */
static int emit_jumpif(TCCIRState *ir, int tgt, int cond)
{
  return utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(tgt, I32), utb_temp(cond, I32), UTB_NONE);
}

/* --------------------------------------------------------- positive test */

/* Unreachable fall-through after an unconditional JUMP is NOPed:
 *   0: ADD T0 <- #1, #2
 *   1: JUMP -> 3
 *   2: ADD T1 <- #4, #5   (unreachable)
 *   3: RETURNVALUE #0
 * Only instruction 2 should be eliminated. */
UT_TEST(test_dce_unreachable_after_jump)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 3);
  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(4, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* A conditional JUMPIF keeps both its taken edge and its fall-through alive:
 *   0: JUMPIF -> 2, cond T0
 *   1: ADD T1 <- #1, #2
 *   2: RETURNVALUE #0
 * Nothing is dead. */
UT_TEST(test_dce_jumpif_keeps_both_targets)
{
  TCCIRState *ir = utb_new();

  emit_jumpif(ir, 2, 0);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* The presence of an IJUMP makes static reachability unknowable, so the pass
 * bails out and returns 0 without mutating anything. */
UT_TEST(test_dce_ijump_skips_pass)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_IJUMP, UTB_NONE, utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_IJUMP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------------- negative test */

/* Straight-line code with no branches is already fully reachable. */
UT_TEST(test_dce_straight_line_unchanged)
{
  TCCIRState *ir = utb_new();

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------ idempotence test */

/* A second DCE run should ideally report 0 changes because the first run
 * already NOPed everything unreachable.  The current implementation
 * recomputes reachability from scratch and counts every unreachable
 * instruction, including ones that are already NOP, so the second run
 * returns the same non-zero count as the first.
 *
 * SUSPECTED BUG: DCE does not skip already-NOP instructions when counting
 * changes, so it is not idempotent in its return value.  This may cause
 * the pass manager to schedule extra fixpoint iterations even though no
 * real transformation happens after the first run. */
UT_TEST(test_dce_second_run_reports_same_count)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 3);
  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(4, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int first = tcc_ir_opt_dce(ir);
  TccIrOp dead_op_after_first = utb_op(ir, dead);
  int second = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(first, 1);
  /* Current (possibly buggy) behavior: second pass re-counts the already-NOP
   * unreachable instruction.  Do not change production code; pin behavior. */
  UT_ASSERT_EQ(second, first);
  UT_ASSERT_EQ(dead_op_after_first, TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP); /* no new non-NOP -> NOP changes */
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ helpers */

/* Build a SYMREF callee whose Sym has the func_noreturn attribute set. */
static IROperand utb_noreturn_attr_callee(TCCIRState *ir)
{
  static Sym callee, ref;
  memset(&callee, 0, sizeof(callee));
  memset(&ref, 0, sizeof(ref));
  callee.c = 0; /* keep elfsym() NULL so the name/attribute path is used */
  callee.type.ref = &ref;
  ref.f.func_noreturn = 1;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &callee, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* Build a SYMREF callee whose name is looked up via get_tok_str(). */
static IROperand utb_named_callee(TCCIRState *ir, Sym *sym, int tok, const char *name)
{
  sym->v = tok;
  utb_set_tok_str(tok, name);
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* Attach a single switch table to the IR.  Caller must have allocated `targets`. */
static void utb_setup_switch_table(TCCIRState *ir, int tid, int *targets, int n, int def)
{
  ir->switch_tables = (TCCIRSwitchTable *)tcc_mallocz(sizeof(TCCIRSwitchTable) * (tid + 1));
  ir->num_switch_tables = tid + 1;
  TCCIRSwitchTable *tbl = &ir->switch_tables[tid];
  tbl->min_val = 0;
  tbl->max_val = n - 1;
  tbl->targets = targets;
  tbl->num_entries = n;
  tbl->default_target = def;
}

/* ------------------------------------------------------- new DCE coverage tests */

/* Empty function body: the pass must return 0 without crashing. */
UT_TEST(test_dce_empty_ir_returns_zero)
{
  TCCIRState *ir = utb_new();

  UT_ASSERT_EQ(tcc_ir_opt_dce(ir), 0);

  utb_free(ir);
  return 0;
}

/* The timed wrapper path (tcc_pass_timing_on != 0) must still perform the
 * transformation and report the same number of NOPs. */
UT_TEST(test_dce_timing_path)
{
  TCCIRState *ir = utb_new();
  tcc_pass_timing_on = 1;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 3);
  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(4, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);
  tcc_pass_timing_on = 0; /* reset before any assertion can early-return */

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* SWITCH_TABLE is a terminator whose targets are all reachable.  The single
 * instruction that falls through after it is dead. */
UT_TEST(test_dce_switch_table_marks_targets)
{
  TCCIRState *ir = utb_new();
  static int targets[2];
  targets[0] = 2;
  targets[1] = 3;
  utb_setup_switch_table(ir, 0, targets, 2, 4);

  utb_emit(ir, TCCIR_OP_SWITCH_TABLE, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(10, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(20, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_SWITCH_TABLE);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_RETURNVOID);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  tcc_free(ir->switch_tables);
  utb_free(ir);
  return 0;
}

/* A FUNCCALLVOID whose callee is not a symbol is conservatively treated as
 * returning, so the fall-through instruction stays alive. */
UT_TEST(test_dce_funccall_null_callee_falls_through)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_imm(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* A FUNCCALL to a callee with func_noreturn set is a terminator: code after it
 * is unreachable and must be NOPed. */
UT_TEST(test_dce_noreturn_attr_call_elides_fallthrough)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  IROperand callee = utb_noreturn_attr_callee(ir);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int dead1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int dead2 = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_op(ir, dead1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, dead2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* The "abort" builtin is recognised as noreturn by name, so code after the
 * call is eliminated even without a func_noreturn attribute. */
UT_TEST(test_dce_named_noreturn_call_elides_fallthrough)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  memset(&callee, 0, sizeof(callee));
  callee.c = 0;
  IROperand fn = utb_named_callee(ir, &callee, TOK_ABORT, "abort");

  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int dead = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dce(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_dce)
{
  UT_COVERS("dce");

  UT_RUN(test_dce_unreachable_after_jump);
  UT_RUN(test_dce_jumpif_keeps_both_targets);
  UT_RUN(test_dce_ijump_skips_pass);
  UT_RUN(test_dce_straight_line_unchanged);
  UT_RUN(test_dce_second_run_reports_same_count);
  UT_RUN(test_dce_empty_ir_returns_zero);
  UT_RUN(test_dce_timing_path);
  UT_RUN(test_dce_switch_table_marks_targets);
  UT_RUN(test_dce_funccall_null_callee_falls_through);
  UT_RUN(test_dce_noreturn_attr_call_elides_fallthrough);
  UT_RUN(test_dce_named_noreturn_call_elides_fallthrough);
}
