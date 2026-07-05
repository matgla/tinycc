/*
 *  test_opt_dce.c - suite for ir/opt_dce.c (legacy Dead Code Elimination)
 *
 *  DCE follows control-flow edges from instruction 0, marks reachable
 *  instructions, and NOPs the rest.  It returns the number of instructions
 *  it converted to NOP.
 */

#include "ir_build.h"
#include "opt_engine.h"
#include "ut.h"

/* Pass entry points defined in ir/opt_dce.c. */
int tcc_ir_opt_dce(TCCIRState *ir);
int tcc_ir_opt_orphan_cmp_elim(TCCIRState *ir);
int tcc_ir_opt_dce_ex(IROptCtx *ctx);
int tcc_ir_opt_orphan_cmp_elim_ex(IROptCtx *ctx);
int tcc_ir_opt_useless_function_body(TCCIRState *ir);
int tcc_ir_opt_noreturn_collapse(TCCIRState *ir);
int tcc_ir_opt_trap_only_body_suppress(TCCIRState *ir);
int tcc_ir_opt_zero_vla_elim(TCCIRState *ir);
int tcc_ir_opt_compact_nops(TCCIRState *ir);
int tcc_ir_callee_is_noreturn(Sym *callee);
int tcc_ir_opt_infinite_self_recursion(TCCIRState *ir, Sym *func_sym);
int tcc_ir_opt_noreturn_call_epilogue_suppress(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Global pass-timing gate used by the timed wrapper in opt_dce.c. */
extern signed char tcc_pass_timing_on;

/* Tokens for naming callees via the harness get_tok_str table. */
#define TOK_ABORT     300
#define TOK_EXIT      301
#define TOK_CFCMPL    302
#define TOK_AEABI_I2F 303

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

/* Emit a CMP of `a` and `b` (no dest). */
static int emit_cmp(TCCIRState *ir, IROperand a, IROperand b)
{
  return utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, a, b);
}

/* Emit a SETIF writing `dest` with condition code `cond`. */
static int emit_setif(TCCIRState *ir, int dest, int cond)
{
  return utb_emit(ir, TCCIR_OP_SETIF, utb_temp(dest, I32), utb_imm(cond, I32), UTB_NONE);
}

/* Emit a void call to `callee` with call id `cid` and `argc`. */
static int emit_call_void(TCCIRState *ir, IROperand callee, int cid, int argc)
{
  return utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(cid, argc), I32));
}

/* Emit a void parameter for call `cid` at parameter index `pidx`. */
static int emit_param_void(TCCIRState *ir, int cid, int pidx)
{
  return utb_emit(ir, TCCIR_OP_FUNCPARAMVOID, UTB_NONE, UTB_NONE,
                  utb_imm((int32_t)TCCIR_ENCODE_PARAM(cid, pidx), I32));
}

/* Emit a STORE of `src` into `dest`. */
static int emit_store(TCCIRState *ir, IROperand dest, IROperand src)
{
  return utb_emit(ir, TCCIR_OP_STORE, dest, src, UTB_NONE);
}

/* Emit a VLA_ALLOC of `size` bytes aligned to `align`. */
static int emit_vla_alloc(TCCIRState *ir, IROperand size, IROperand align)
{
  return utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, size, align);
}

/* Emit a VLA_SP_SAVE into `slot`. */
static int emit_vla_sp_save(TCCIRState *ir, IROperand slot)
{
  return utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot, UTB_NONE, UTB_NONE);
}

/* Emit a VLA_SP_RESTORE from `slot`. */
static int emit_vla_sp_restore(TCCIRState *ir, IROperand slot)
{
  return utb_emit(ir, TCCIR_OP_VLA_SP_RESTORE, UTB_NONE, slot, UTB_NONE);
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

/* --------------------------------------------------------- callee noreturn */

UT_TEST(test_callee_is_noreturn_null)
{
  UT_ASSERT_EQ(tcc_ir_callee_is_noreturn(NULL), 0);
  return 0;
}

UT_TEST(test_callee_is_noreturn_attr)
{
  static Sym callee, ref;
  memset(&callee, 0, sizeof(callee));
  memset(&ref, 0, sizeof(ref));
  callee.c = 0;
  callee.type.ref = &ref;
  ref.f.func_noreturn = 1;

  UT_ASSERT_EQ(tcc_ir_callee_is_noreturn(&callee), 1);
  return 0;
}

UT_TEST(test_callee_is_noreturn_by_name_exit)
{
  static Sym callee;
  memset(&callee, 0, sizeof(callee));
  callee.c = 0;
  callee.v = TOK_EXIT;
  utb_set_tok_str(TOK_EXIT, "exit");

  UT_ASSERT_EQ(tcc_ir_callee_is_noreturn(&callee), 1);
  return 0;
}

/* --------------------------------------------------------- orphan cmp elim */

UT_TEST(test_orphan_cmp_elim_simple)
{
  TCCIRState *ir = utb_new();
  int cmp = emit_cmp(ir, utb_temp(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_orphan_cmp_elim_keeps_when_consumed)
{
  TCCIRState *ir = utb_new();
  int cmp = emit_cmp(ir, utb_temp(0, I32), utb_imm(0, I32));
  emit_setif(ir, 1, TOK_EQ);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_orphan_cmp_elim_flag_helper_call)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  memset(&callee, 0, sizeof(callee));
  IROperand fn = utb_named_callee(ir, &callee, TOK_CFCMPL, "__aeabi_cfcmple");
  int call = emit_call_void(ir, fn, 1, 0);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------------- useless function body */

UT_TEST(test_useless_body_elides_pure_call)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  memset(&callee, 0, sizeof(callee));
  IROperand fn = utb_named_callee(ir, &callee, TOK_AEABI_I2F, "__aeabi_i2f");
  emit_param_void(ir, 1, 0);
  int call = emit_call_void(ir, fn, 1, 1);

  int changes = tcc_ir_opt_useless_function_body(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_useless_body_keeps_essential_return)
{
  TCCIRState *ir = utb_new();
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_useless_function_body(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_useless_body_empty_elides_all)
{
  TCCIRState *ir = utb_new();
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));

  int changes = tcc_ir_opt_useless_function_body(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------------- noreturn collapse */

UT_TEST(test_noreturn_collapse_self_jump)
{
  TCCIRState *ir = utb_new();
  int saved_opt = tcc_state->optimize;
  tcc_state->optimize = 2;

  emit_jump(ir, 0);

  int changes = tcc_ir_opt_noreturn_collapse(ir);
  tcc_state->optimize = saved_opt;

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_noreturn_collapse_with_store)
{
  TCCIRState *ir = utb_new();
  int saved_opt = tcc_state->optimize;
  tcc_state->optimize = 2;

  static Sym cur, ref;
  memset(&cur, 0, sizeof(cur));
  memset(&ref, 0, sizeof(ref));
  cur.type.ref = &ref;
  Sym *saved_cur = tcc_state->cur_func_sym;
  tcc_state->cur_func_sym = &cur;

  emit_store(ir, utb_var(0, I32), utb_imm(0, I32));
  emit_jump(ir, 0);

  int changes = tcc_ir_opt_noreturn_collapse(ir);
  tcc_state->optimize = saved_opt;
  tcc_state->cur_func_sym = saved_cur;

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(ref.f.func_noreturn, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------------- trap-only body suppress */

UT_TEST(test_trap_only_body_suppress)
{
  TCCIRState *ir = utb_new();
  int saved_opt = tcc_state->optimize;
  tcc_state->optimize = 2;

  utb_emit(ir, TCCIR_OP_TRAP, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_trap_only_body_suppress(ir);
  tcc_state->optimize = saved_opt;

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(ir->noreturn, 1);
  UT_ASSERT_EQ(ir->leaffunc, 1);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------------- zero vla elim */

UT_TEST(test_zero_vla_elim_immediate_zero)
{
  TCCIRState *ir = utb_new();
  int alloc = emit_vla_alloc(ir, utb_imm(0, I32), utb_imm(4, I32));

  int changes = tcc_ir_opt_zero_vla_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

UT_TEST(test_zero_vla_elim_sp_save_restore_pair)
{
  TCCIRState *ir = utb_new();
  IROperand slot = utb_stackoff(0, 0, 0, 0, I32);
  int save = emit_vla_sp_save(ir, slot);
  int restore = emit_vla_sp_restore(ir, slot);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_zero_vla_elim(ir);

  /* The pass returns a boolean-style 0/1 change indicator, not a per-NOP
   * count, so a single SAVE/RESTORE pair reports 1 even though both
   * instructions become NOP. */
  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, restore), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------------- compact nops */

UT_TEST(test_compact_nops_removes_dead_and_fixes_targets)
{
  TCCIRState *ir = utb_new();
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int removed = tcc_ir_opt_compact_nops(ir);

  UT_ASSERT_EQ(removed, 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 3);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 0)), 2);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------ infinite self-recursion */

/* A function whose first live op is a self-call never returns; collapse the
 * whole body to a single self-jump. */
UT_TEST(test_infinite_self_recursion_collapse)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int saved_opt = tcc_state->optimize;
  tcc_state->optimize = 2;

  static Sym func;
  memset(&func, 0, sizeof(func));
  IROperand fn = utb_named_callee(ir, &func, 400, "self_recurse");

  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_infinite_self_recursion(ir, &func);
  tcc_state->optimize = saved_opt;

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(ir->noreturn, 1);
  UT_ASSERT_EQ(ir->leaffunc, 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* A RETURN before the self-call means the function can return; do not
 * collapse. */
UT_TEST(test_infinite_self_recursion_return_before_call)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int saved_opt = tcc_state->optimize;
  tcc_state->optimize = 2;

  static Sym func;
  memset(&func, 0, sizeof(func));
  IROperand fn = utb_named_callee(ir, &func, 401, "self_recurse2");

  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_infinite_self_recursion(ir, &func);
  tcc_state->optimize = saved_opt;

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_RETURNVOID);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------- noreturn-call epilogue suppress */

/* When the last live instruction is a call to a noreturn callee and there is
 * no surviving RETURN, codegen can omit the epilogue. */
UT_TEST(test_noreturn_call_epilogue_suppress)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int saved_opt = tcc_state->optimize;
  tcc_state->optimize = 2;

  static Sym callee;
  memset(&callee, 0, sizeof(callee));
  IROperand fn = utb_named_callee(ir, &callee, TOK_ABORT, "abort");

  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_noreturn_call_epilogue_suppress(ir);
  tcc_state->optimize = saved_opt;

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(ir->noreturn, 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* A surviving RETURNVOID means the function can return; keep the epilogue. */
UT_TEST(test_noreturn_call_epilogue_suppress_with_return)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  int saved_opt = tcc_state->optimize;
  tcc_state->optimize = 2;

  static Sym callee;
  memset(&callee, 0, sizeof(callee));
  IROperand fn = utb_named_callee(ir, &callee, TOK_ABORT, "abort");

  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_noreturn_call_epilogue_suppress(ir);
  tcc_state->optimize = saved_opt;

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(ir->noreturn, 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------- orphan cmp extra cases */

/* A CMP whose only consumer would be reached through a JUMP cycle is kept
 * conservatively: the scan hits a visited instruction and declares the CMP
 * live. */
UT_TEST(test_orphan_cmp_elim_jump_cycle_keeps_cmp)
{
  TCCIRState *ir = utb_new();
  int cmp = emit_cmp(ir, utb_temp(0, I32), utb_imm(0, I32));
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);
  emit_jump(ir, 1);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* A CMP followed immediately by a RETURN has no flag consumer before the end
 * of the function, so it is eliminated. */
UT_TEST(test_orphan_cmp_elim_end_of_function)
{
  TCCIRState *ir = utb_new();
  int cmp = emit_cmp(ir, utb_temp(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_orphan_cmp_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------------- noreturn collapse gates */

/* A function with no JUMP at all has an implicit return path; do not
 * collapse. */
UT_TEST(test_noreturn_collapse_no_jump_returns_zero)
{
  TCCIRState *ir = utb_new();
  int saved_opt = tcc_state->optimize;
  tcc_state->optimize = 2;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_noreturn_collapse(ir);
  tcc_state->optimize = saved_opt;

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* When the last live op is not an unconditional JUMP the function can return
 * implicitly; do not collapse. */
UT_TEST(test_noreturn_collapse_implicit_return_returns_zero)
{
  TCCIRState *ir = utb_new();
  int saved_opt = tcc_state->optimize;
  tcc_state->optimize = 2;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_noreturn_collapse(ir);
  tcc_state->optimize = saved_opt;

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* A conditional branch inside the loop can exit to the implicit epilogue,
 * so the function is not provably noreturn. */
UT_TEST(test_noreturn_collapse_conditional_exit_returns_zero)
{
  TCCIRState *ir = utb_new();
  int saved_opt = tcc_state->optimize;
  tcc_state->optimize = 2;

  emit_jump(ir, 4);
  emit_jumpif(ir, 5, 0); /* target 5 is NOP -> past last live op */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  emit_jump(ir, 0);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_noreturn_collapse(ir);
  tcc_state->optimize = saved_opt;

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* WRAPPER: IROptCtx entry points forward to the bare TCCIRState* passes. */

static IROptCtx utb_ctx(TCCIRState *ir)
{
  IROptCtx ctx = {0};
  ctx.ir = ir;
  return ctx;
}

UT_TEST(test_dce_ex_forwards)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  emit_jump(ir, 3);
  int dead = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(4, I32), utb_imm(5, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptCtx ctx = utb_ctx(ir);
  int changes = tcc_ir_opt_dce_ex(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

UT_TEST(test_orphan_cmp_elim_ex_forwards)
{
  TCCIRState *ir = utb_new();
  int cmp = emit_cmp(ir, utb_temp(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptCtx ctx = utb_ctx(ir);
  int changes = tcc_ir_opt_orphan_cmp_elim_ex(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_NOP);

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
  UT_RUN(test_callee_is_noreturn_null);
  UT_RUN(test_callee_is_noreturn_attr);
  UT_RUN(test_callee_is_noreturn_by_name_exit);
  UT_RUN(test_orphan_cmp_elim_simple);
  UT_RUN(test_orphan_cmp_elim_keeps_when_consumed);
  UT_RUN(test_orphan_cmp_elim_flag_helper_call);
  UT_RUN(test_useless_body_elides_pure_call);
  UT_RUN(test_useless_body_keeps_essential_return);
  UT_RUN(test_useless_body_empty_elides_all);
  UT_RUN(test_noreturn_collapse_self_jump);
  UT_RUN(test_noreturn_collapse_with_store);
  UT_RUN(test_trap_only_body_suppress);
  UT_RUN(test_zero_vla_elim_immediate_zero);
  UT_RUN(test_zero_vla_elim_sp_save_restore_pair);
  UT_RUN(test_compact_nops_removes_dead_and_fixes_targets);
  UT_RUN(test_infinite_self_recursion_collapse);
  UT_RUN(test_infinite_self_recursion_return_before_call);
  UT_RUN(test_noreturn_call_epilogue_suppress);
  UT_RUN(test_noreturn_call_epilogue_suppress_with_return);
  UT_RUN(test_orphan_cmp_elim_jump_cycle_keeps_cmp);
  UT_RUN(test_orphan_cmp_elim_end_of_function);
  UT_RUN(test_noreturn_collapse_no_jump_returns_zero);
  UT_RUN(test_noreturn_collapse_implicit_return_returns_zero);
  UT_RUN(test_noreturn_collapse_conditional_exit_returns_zero);
  UT_RUN(test_dce_ex_forwards);
  UT_RUN(test_orphan_cmp_elim_ex_forwards);
}
