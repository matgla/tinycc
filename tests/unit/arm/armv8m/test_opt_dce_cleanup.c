/*
 *  test_opt_dce_cleanup.c - suite for the whole-function collapse / cleanup
 *  passes in ir/opt_dce.c that are NOT already covered by test_opt_dce.c or
 *  test_opt_dead_store.c:
 *
 *    tcc_ir_opt_useless_function_body()
 *    tcc_ir_opt_noreturn_collapse()
 *    tcc_ir_opt_trap_only_body_suppress()
 *    tcc_ir_opt_infinite_self_recursion()
 *    tcc_ir_opt_noreturn_call_epilogue_suppress()
 *    tcc_ir_opt_compact_nops()
 *    tcc_ir_opt_const_return_uninit_elide()
 *    tcc_ir_opt_ub_only_body_elide()
 *    tcc_ir_opt_null_store_dom_return()
 *
 *  These are whole-body "prove no observable effect / prove non-return"
 *  passes plus the mechanical NOP-compaction pass; none had any unit
 *  coverage before this file (grep across tests/unit/ found zero hits on
 *  any of these seven entry points). Each gets at least one positive
 *  (transform fires) and one negative/guard (transform must NOT fire) case,
 *  following the existing opt_dce/opt_dead_store suite conventions.
 */

#include "ir_build.h"

#include "opt_engine.h"
#include "ut.h"

/* Pass entry points (defined in ir/opt_dce.c; forward-declared here to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_useless_function_body(TCCIRState *ir);
int tcc_ir_opt_noreturn_collapse(TCCIRState *ir);
int tcc_ir_opt_trap_only_body_suppress(TCCIRState *ir);
int tcc_ir_opt_infinite_self_recursion(TCCIRState *ir, Sym *func_sym);
int tcc_ir_opt_noreturn_call_epilogue_suppress(TCCIRState *ir);
int tcc_ir_opt_compact_nops(TCCIRState *ir);

/* IROptCtx wrapper entry points and additional whole-function elision passes. */
int tcc_ir_opt_useless_function_body_ex(IROptCtx *ctx);
int tcc_ir_opt_noreturn_collapse_ex(IROptCtx *ctx);
int tcc_ir_opt_trap_only_body_suppress_ex(IROptCtx *ctx);
int tcc_ir_opt_compact_nops_ex(IROptCtx *ctx);
int tcc_ir_opt_ub_only_body_elide(TCCIRState *ir);
int tcc_ir_opt_ub_only_body_elide_ex(IROptCtx *ctx);
int tcc_ir_opt_local_only_body_elide(TCCIRState *ir);
int tcc_ir_opt_local_only_body_elide_ex(IROptCtx *ctx);
int tcc_ir_opt_const_return_uninit_elide(TCCIRState *ir);
int tcc_ir_opt_const_return_uninit_elide_ex(IROptCtx *ctx);
int tcc_ir_opt_null_store_dom_return(TCCIRState *ir);
int tcc_ir_opt_null_store_dom_return_ex(IROptCtx *ctx);

#define I32 IROP_BTYPE_INT32

/* ------------------------------------------------------------------ helpers */

static void set_optimize2(void) { tcc_state->optimize = 2; }

static void reset_state(void)
{
  tcc_state->optimize = 0;
  tcc_state->cur_func_sym = NULL;
  tcc_state->need_frame_pointer = 0;
  tcc_state->force_frame_pointer = 0;
  tcc_state->ir_late_reopt_phase = 0;
}

/* Emit an unconditional JUMP to target index `tgt`. */
static int emit_jump(TCCIRState *ir, int tgt)
{
  return utb_emit(ir, TCCIR_OP_JUMP, utb_imm(tgt, I32), UTB_NONE, UTB_NONE);
}

/* A SYMREF operand referencing `sym` as a callee (mirrors
 * test_opt_dead_init_call.c's utb_callee_ref()). */
static IROperand utb_callee_ref(TCCIRState *ir, Sym *sym)
{
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* redundant_init_elim calls tcc_ir_get_live_interval() unconditionally on
 * every candidate VAR init -- with variables_live_intervals_size left at
 * utb_new()'s default of 0, position 0 is "out of bounds" and the real
 * implementation calls exit(1). Every VAR-position-0 test needs a real
 * (zeroed - not addrtaken) backing array (see test_opt_dead_store.c's
 * identical helper). */
static void utb_alloc_var_intervals(TCCIRState *ir, int count)
{
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->variables_live_intervals_size = count;
}

/* ======================================================= useless_function_body
 *
 * Whole-body elision: fires only when every live instruction is classified
 * "non-essential" by ir_opt_op_is_essential() (RETURNVOID falls through the
 * switch to the volatile-sym-only default; RETURNVALUE, STORE, FUNCCALLVAL/
 * VOID to a non-elidable callee, forward JUMP-past-end, etc. are essential).
 */

/* POSITIVE: a body that is just an implicit-return RETURNVOID has no
 * essential op anywhere -> the whole body (1 instruction) collapses to NOP. */
UT_TEST(test_useless_body_returnvoid_only_collapses)
{
  TCCIRState *ir = utb_new();

  int ret = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_useless_function_body(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->leaffunc, 1);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE: RETURNVALUE is essential (can't drop the return value), so the
 * pass must return 0 and leave the body untouched. */
UT_TEST(test_useless_body_returnvalue_keeps_body)
{
  TCCIRState *ir = utb_new();

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_useless_function_body(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE: a call to an ordinary (non-elidable) function is essential -> body
 * kept even though it ends in a bare RETURNVOID. */
UT_TEST(test_useless_body_ordinary_call_keeps_body)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  memset(&callee, 0, sizeof(callee));
  IROperand fn = utb_callee_ref(ir, &callee);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int ret = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_useless_function_body(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVOID);

  utb_free(ir);
  reset_state();
  return 0;
}

/* ============================================================ noreturn_collapse
 *
 * O2-only: collapses a function with no RETURN/call/asm/volatile-access to a
 * bare self-jump when the last live op is a JUMP looping back into the body.
 */

/* POSITIVE: STORE to a non-volatile global inside a tight self-loop, no
 * RETURN anywhere -> collapses to a single self-JUMP; func_noreturn is
 * published on cur_func_sym because has_store is true. */
UT_TEST(test_noreturn_collapse_self_loop_with_store_collapses)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  set_optimize2();

  static Sym gsym;
  memset(&gsym, 0, sizeof(gsym));
  IROperand gx = utb_symref(ir, &gsym, /*is_lval*/ 1, /*is_local*/ 0, /*is_const*/ 0, I32);

  static Sym func_sym, func_ref;
  memset(&func_sym, 0, sizeof(func_sym));
  memset(&func_ref, 0, sizeof(func_ref));
  func_sym.type.ref = &func_ref;
  tcc_state->cur_func_sym = &func_sym;

  int store = utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(5, I32), UTB_NONE);
  int back = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(0, I32), UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_noreturn_collapse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_JUMP);
  /* The collapsed self-jump targets instruction 0 (itself), i.e. `b .`.
   * irop_make_imm32(vreg, val, btype) takes the "no vreg" sentinel (-1) as
   * its FIRST argument and the actual immediate as the SECOND; the source
   * builds it as irop_make_imm32(-1, 0, ...), so the jump-target immediate
   * is 0, not -1. */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, store)), 0);
  UT_ASSERT_EQ(utb_op(ir, back), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->noreturn, 1);
  UT_ASSERT_EQ((int)func_ref.f.func_noreturn, 1);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): a live RETURNVOID means the function CAN return; the
 * collapse would change semantics, so the pass must bail with 0 changes. */
UT_TEST(test_noreturn_collapse_has_return_no_change)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  set_optimize2();

  static Sym gsym;
  memset(&gsym, 0, sizeof(gsym));
  IROperand gx = utb_symref(ir, &gsym, 1, 0, 0, I32);

  int store = utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(5, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_noreturn_collapse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVOID);
  UT_ASSERT_EQ(ir->noreturn, 0);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): the pass is O2-only; at a lower optimize level it must
 * not fire even on an otherwise-qualifying self-loop. */
UT_TEST(test_noreturn_collapse_optimize_gate)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  tcc_state->optimize = 1;

  static Sym gsym;
  memset(&gsym, 0, sizeof(gsym));
  IROperand gx = utb_symref(ir, &gsym, 1, 0, 0, I32);

  int store = utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(0, I32), UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_noreturn_collapse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  utb_free(ir);
  reset_state();
  return 0;
}

/* ======================================================= trap_only_body_suppress
 *
 * Suppresses prologue/epilogue when the whole body collapsed to one TRAP.
 */

/* POSITIVE: a single TRAP and nothing else -> leaf/noreturn flags set,
 * frame-pointer forcing dropped; returns 1 (no IR mutation, just state). */
UT_TEST(test_trap_only_body_single_trap_suppresses_frame)
{
  TCCIRState *ir = utb_new();
  set_optimize2();
  tcc_state->need_frame_pointer = 1;
  tcc_state->force_frame_pointer = 1;

  int trap = utb_emit(ir, TCCIR_OP_TRAP, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_trap_only_body_suppress(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, trap), TCCIR_OP_TRAP); /* IR itself is untouched */
  UT_ASSERT_EQ(ir->leaffunc, 1);
  UT_ASSERT_EQ(ir->noreturn, 1);
  UT_ASSERT_EQ(tcc_state->need_frame_pointer, 0);
  UT_ASSERT_EQ(tcc_state->force_frame_pointer, 0);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): a live op besides the TRAP means the body is not
 * "trap-only" -> no change. */
UT_TEST(test_trap_only_body_extra_op_no_change)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int trap = utb_emit(ir, TCCIR_OP_TRAP, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_trap_only_body_suppress(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, trap), TCCIR_OP_TRAP);
  UT_ASSERT_EQ(ir->leaffunc, 0);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): `ir->naked` functions are left alone (no prologue to
 * suppress in the first place). */
UT_TEST(test_trap_only_body_naked_function_no_change)
{
  TCCIRState *ir = utb_new();
  set_optimize2();
  ir->naked = 1;

  int trap = utb_emit(ir, TCCIR_OP_TRAP, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_trap_only_body_suppress(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, trap), TCCIR_OP_TRAP);
  UT_ASSERT_EQ(ir->leaffunc, 0);

  utb_free(ir);
  reset_state();
  return 0;
}

/* ===================================================== infinite_self_recursion
 *
 * Collapses `f() { ... ; f(); ... }` to `b .` when the self-call is
 * unconditionally reached with no observable side effects before it.
 */

/* POSITIVE: the function's very first instruction is an unconditional
 * self-call -> collapses to a bare self-JUMP; func_noreturn is published. */
UT_TEST(test_infinite_self_recursion_unconditional_call_collapses)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  set_optimize2();

  static Sym func_sym, func_ref;
  memset(&func_sym, 0, sizeof(func_sym));
  memset(&func_ref, 0, sizeof(func_ref));
  func_sym.type.ref = &func_ref;

  IROperand self_fn = utb_callee_ref(ir, &func_sym);
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, self_fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_infinite_self_recursion(ir, &func_sym);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_JUMP);
  /* Self-jump targets instruction 0 (itself) -- see the note in
   * test_noreturn_collapse_self_loop_with_store_collapses: irop_make_imm32's
   * FIRST arg is the "no vreg" sentinel (-1), not the immediate value, so
   * the source's irop_make_imm32(-1, 0, ...) encodes a jump target of 0. */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, call)), 0);
  UT_ASSERT_EQ(ir->noreturn, 1);
  UT_ASSERT_EQ((int)func_ref.f.func_noreturn, 1);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): an early RETURNVOID reached before the self-call breaks
 * the "unconditionally reached" guarantee -> no collapse. */
UT_TEST(test_infinite_self_recursion_early_return_no_change)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  set_optimize2();

  static Sym func_sym, func_ref;
  memset(&func_sym, 0, sizeof(func_sym));
  memset(&func_ref, 0, sizeof(func_ref));
  func_sym.type.ref = &func_ref;

  int ret = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  IROperand self_fn = utb_callee_ref(ir, &func_sym);
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, self_fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_infinite_self_recursion(ir, &func_sym);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVOID);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(ir->noreturn, 0);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): a call to a DIFFERENT function before any self-call means
 * we can no longer prove non-return past that call -> bail. */
UT_TEST(test_infinite_self_recursion_other_call_first_no_change)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  set_optimize2();

  static Sym func_sym, func_ref, other_sym;
  memset(&func_sym, 0, sizeof(func_sym));
  memset(&func_ref, 0, sizeof(func_ref));
  memset(&other_sym, 0, sizeof(other_sym));
  func_sym.type.ref = &func_ref;

  IROperand other_fn = utb_callee_ref(ir, &other_sym);
  int call1 = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, other_fn,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_infinite_self_recursion(ir, &func_sym);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call1), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  reset_state();
  return 0;
}

/* =============================================== noreturn_call_epilogue_suppress
 *
 * Sets ir->noreturn (epilogue suppression only, no caller-visible publish)
 * when the surviving IR ends at a call to a provably-noreturn callee and no
 * live RETURN exists anywhere.
 */

/* POSITIVE: a live FUNCCALLVOID to a func_noreturn callee is the last live
 * op, no RETURN anywhere -> ir->noreturn set. */
UT_TEST(test_noreturn_call_epilogue_suppress_fires)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  set_optimize2();

  static Sym noret_sym, noret_ref;
  memset(&noret_sym, 0, sizeof(noret_sym));
  memset(&noret_ref, 0, sizeof(noret_ref));
  noret_ref.f.func_noreturn = 1;
  noret_sym.type.ref = &noret_ref;

  IROperand fn = utb_callee_ref(ir, &noret_sym);
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_noreturn_call_epilogue_suppress(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(ir->noreturn, 1);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID); /* IR itself untouched */

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): a live RETURNVOID anywhere means at least one path exits
 * cleanly -> no suppression. */
UT_TEST(test_noreturn_call_epilogue_suppress_live_return_no_change)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  set_optimize2();

  static Sym noret_sym, noret_ref;
  memset(&noret_sym, 0, sizeof(noret_sym));
  memset(&noret_ref, 0, sizeof(noret_ref));
  noret_ref.f.func_noreturn = 1;
  noret_sym.type.ref = &noret_ref;

  int ret = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  IROperand fn = utb_callee_ref(ir, &noret_sym);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_noreturn_call_epilogue_suppress(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(ir->noreturn, 0);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVOID);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): a JUMP can land after the final noreturn call (the
 * "if (bad) abort();" shape, non-abort path jumping straight to the
 * implicit-return epilogue) -> the epilogue is still a real target, so the
 * pass must not suppress it.
 *   0: JUMPIF -> 2, cond T0     (target == n: past-end / epilogue)
 *   1: FUNCCALLVOID <noret_sym> (last live op)
 * n == 2, so the JUMPIF's target (2) is >= n -> bail. */
UT_TEST(test_noreturn_call_epilogue_suppress_jump_past_call_no_change)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  set_optimize2();

  static Sym noret_sym, noret_ref;
  memset(&noret_sym, 0, sizeof(noret_sym));
  memset(&noret_ref, 0, sizeof(noret_ref));
  noret_ref.f.func_noreturn = 1;
  noret_sym.type.ref = &noret_ref;

  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_temp(0, I32), UTB_NONE);
  IROperand fn = utb_callee_ref(ir, &noret_sym);
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, fn,
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_noreturn_call_epilogue_suppress(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(ir->noreturn, 0);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  reset_state();
  return 0;
}

/* ================================================================ compact_nops
 *
 * Removes NOP instructions in one O(n) sweep and remaps every JUMP/JUMPIF
 * target (including past-end "epilogue" targets) to the compacted indices.
 */

/* POSITIVE: a NOP in the middle is removed, and a JUMP whose target lands
 * after it is remapped down by one.
 *   0: ADD T0 <- #1,#2
 *   1: NOP                      (removed)
 *   2: JUMP -> 3
 *   3: RETURNVALUE T0
 * -> 0: ADD, 1: JUMP -> 2, 2: RETURNVALUE ; next_instruction_index == 3 */
UT_TEST(test_compact_nops_removes_nop_and_remaps_jump_target)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int removed = tcc_ir_opt_compact_nops(ir);

  UT_ASSERT_EQ(removed, 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 3);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 1)), 2);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: an epilogue JUMP whose target is exactly `n` (one past the last
 * instruction, tcc_ir_backpatch_to_here's convention for "fall off the end")
 * is remapped to the new past-end index after compaction, not treated as an
 * in-range NOP target.
 *   0: NOP                    (removed)
 *   1: JUMP -> 2  (== n, epilogue target)
 * -> 0: JUMP -> 1 (== new write_pos, still past-end) */
UT_TEST(test_compact_nops_remaps_epilogue_target)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);

  int removed = tcc_ir_opt_compact_nops(ir);

  UT_ASSERT_EQ(removed, 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 0)), 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): no NOPs present at all -> quick-check bails with 0. */
UT_TEST(test_compact_nops_no_nops_no_change)
{
  TCCIRState *ir = utb_new();

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int removed = tcc_ir_opt_compact_nops(ir);

  UT_ASSERT_EQ(removed, 0);
  UT_ASSERT_EQ(ir->next_instruction_index, 2);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  return 0;
}

/* POSITIVE: is_jump_target flags are re-derived from scratch after
 * compaction -- a target that shifts down must have is_jump_target==1 at its
 * NEW index, and the vacated old index must not spuriously carry the flag. */
UT_TEST(test_compact_nops_rederives_jump_target_flags)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  int target = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  ir->compact_instructions[target].is_jump_target = 1; /* stale pre-compaction flag */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE, UTB_NONE);

  int removed = tcc_ir_opt_compact_nops(ir);

  UT_ASSERT_EQ(removed, 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 2);
  /* RETURNVOID moved from index 1 to index 0; JUMP moved from 2 to 1 and now
   * targets the new index 0. */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_RETURNVOID);
  UT_ASSERT_EQ(ir->compact_instructions[0].is_jump_target, 1);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 1)), 0);

  utb_free(ir);
  return 0;
}


/* ================================================================== IROptCtx wrappers and additional whole-body elision passes */

static IROptCtx utb_ctx(TCCIRState *ir)
{
  IROptCtx ctx = {0};
  ctx.ir = ir;
  return ctx;
}

UT_TEST(test_useless_function_body_ex_forwards)
{
  TCCIRState *ir = utb_new();
  int ret = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  IROptCtx ctx = utb_ctx(ir);
  int changes = tcc_ir_opt_useless_function_body_ex(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->leaffunc, 1);

  utb_free(ir);
  reset_state();
  return 0;
}

UT_TEST(test_noreturn_collapse_ex_forwards)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  emit_jump(ir, 0);

  IROptCtx ctx = utb_ctx(ir);
  int changes = tcc_ir_opt_noreturn_collapse_ex(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);

  utb_free(ir);
  reset_state();
  return 0;
}

UT_TEST(test_trap_only_body_suppress_ex_forwards)
{
  TCCIRState *ir = utb_new();
  set_optimize2();
  tcc_state->need_frame_pointer = 1;
  tcc_state->force_frame_pointer = 1;

  int trap = utb_emit(ir, TCCIR_OP_TRAP, UTB_NONE, UTB_NONE, UTB_NONE);

  IROptCtx ctx = utb_ctx(ir);
  int changes = tcc_ir_opt_trap_only_body_suppress_ex(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, trap), TCCIR_OP_TRAP);
  UT_ASSERT_EQ(ir->leaffunc, 1);
  UT_ASSERT_EQ(ir->noreturn, 1);
  UT_ASSERT_EQ(tcc_state->need_frame_pointer, 0);
  UT_ASSERT_EQ(tcc_state->force_frame_pointer, 0);

  utb_free(ir);
  reset_state();
  return 0;
}

UT_TEST(test_compact_nops_ex_forwards)
{
  TCCIRState *ir = utb_new();
  emit_jump(ir, 3);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptCtx ctx = utb_ctx(ir);
  int removed = tcc_ir_opt_compact_nops_ex(&ctx);

  UT_ASSERT_EQ(removed, 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 3);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, 0)), 2);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ================================================== const_return_uninit_elide
 *
 * O2-only: when every RETURNVALUE returns the same constant AND the entry
 * block reads an uninitialized stack slot / VAR vreg before any observable
 * side effect, the whole body collapses to a single RETURNVALUE-const.
 */

/* POSITIVE: entry-block ADD reads StackLoc[8] (raw frontend STACKOFF, never
 * stored) before any side effect; the single RETURNVALUE is a constant.
 *   0: ADD T0 <- StackLoc[8], #1   (uninit read)
 *   1: RETURNVALUE #42
 * -> 0: RETURNVALUE #42, 1: NOP ; leaffunc set. */
UT_TEST(test_crue_uninit_stack_read_collapses_to_const_return)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_stackoff(8, /*is_lval*/ 1, 0, 0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(42, I32), UTB_NONE);

  int changes = tcc_ir_opt_const_return_uninit_elide(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 0)), 42);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->leaffunc, 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  reset_state();
  return 0;
}

/* POSITIVE: same collapse triggered by a read of a never-written,
 * never-address-taken VAR vreg instead of a raw stack slot. */
UT_TEST(test_crue_uninit_var_read_collapses_to_const_return)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_var(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(7, I32), UTB_NONE);

  int changes = tcc_ir_opt_const_return_uninit_elide(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, 0)), 7);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->leaffunc, 1);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): the slot is initialised by an earlier (non-observable)
 * ASSIGN to the same StackLoc[8] -- the read is then well-defined, no UB to
 * exploit. */
UT_TEST(test_crue_initialised_slot_no_change)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  int init = utb_emit(ir, TCCIR_OP_ASSIGN, utb_stackoff(8, 1, 0, 0, I32), utb_imm(5, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_stackoff(8, 1, 0, 0, I32), utb_imm(1, I32));
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(42, I32), UTB_NONE);

  int changes = tcc_ir_opt_const_return_uninit_elide(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, init), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): a STORE (observable side effect) precedes the uninit
 * read -- collapsing would drop the store, so the pass must bail. */
UT_TEST(test_crue_store_before_uninit_read_no_change)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  set_optimize2();

  static Sym gsym;
  memset(&gsym, 0, sizeof(gsym));
  IROperand gx = utb_symref(ir, &gsym, /*is_lval*/ 1, /*is_local*/ 0, /*is_const*/ 0, I32);

  int store = utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(5, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_stackoff(8, 1, 0, 0, I32), utb_imm(1, I32));
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(42, I32), UTB_NONE);

  int changes = tcc_ir_opt_const_return_uninit_elide(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): two RETURNVALUEs with DIFFERENT constants -- there is no
 * single constant to collapse to.
 *   0: JUMPIF -> 2, T0
 *   1: RETURNVALUE #1
 *   2: RETURNVALUE #2 */
UT_TEST(test_crue_differing_return_constants_no_change)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_temp(0, I32), UTB_NONE);
  int ret1 = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(1, I32), UTB_NONE);
  int ret2 = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_const_return_uninit_elide(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, ret1), TCCIR_OP_RETURNVALUE);
  UT_ASSERT_EQ(utb_op(ir, ret2), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): the pass is O2-only; at -O1 it must not fire even on an
 * otherwise-qualifying body. */
UT_TEST(test_crue_optimize_gate)
{
  TCCIRState *ir = utb_new();
  tcc_state->optimize = 1;

  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_stackoff(8, 1, 0, 0, I32), utb_imm(1, I32));
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(42, I32), UTB_NONE);

  int changes = tcc_ir_opt_const_return_uninit_elide(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  reset_state();
  return 0;
}

/* ========================================================= ub_only_body_elide
 *
 * O2-only: when every STORE in a void function goes through an address that
 * is tainted by an uninitialized VAR/stack read (whole-function UB), the
 * entire body is NOPed out.
 */

/* POSITIVE: the only STORE's address is a TEMP defined by an uninit VAR
 * read -> whole body collapses to NOPs.
 *   0: ASSIGN T0 <- V0   (V0 never written/address-taken -> taints T0)
 *   1: STORE T0, #7      (store through garbage pointer)
 *   2: RETURNVOID */
UT_TEST(test_ub_elide_store_through_uninit_pointer_collapses)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_ub_only_body_elide(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->leaffunc, 1);

  utb_free(ir);
  reset_state();
  return 0;
}

/* POSITIVE: taint propagates through a chain of TEMP defs -- the STORE
 * address is two defs away from the uninit read.
 *   0: ASSIGN T0 <- V0
 *   1: ADD T1 <- T0, #4
 *   2: STORE T1, #7
 *   3: RETURNVOID */
UT_TEST(test_ub_elide_taint_propagates_through_temps)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_ub_only_body_elide(ir);

  UT_ASSERT_EQ(changes, 1);
  for (int i = 0; i < 4; i++)
    UT_ASSERT_EQ(utb_op(ir, i), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->leaffunc, 1);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): a STORE through a well-defined global address is a real,
 * observable write -> no elision even though an uninit read exists nearby. */
UT_TEST(test_ub_elide_global_store_keeps_body)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  set_optimize2();

  static Sym gsym;
  memset(&gsym, 0, sizeof(gsym));
  IROperand gx = utb_symref(ir, &gsym, /*is_lval*/ 1, /*is_local*/ 0, /*is_const*/ 0, I32);

  int asn = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(7, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_ub_only_body_elide(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, asn), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVOID);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): the VAR is written before being read, so the TEMP
 * feeding the STORE address is not tainted. */
UT_TEST(test_ub_elide_written_var_not_tainted_no_change)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_ub_only_body_elide(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): a RETURNVALUE anywhere makes the function non-void; the
 * pass can't drop the return value and must bail in its first scan. */
UT_TEST(test_ub_elide_returnvalue_no_change)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(7, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_ub_only_body_elide(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): the pass is O2-only; at -O1 it must not fire even on an
 * otherwise-qualifying body. */
UT_TEST(test_ub_elide_optimize_gate)
{
  TCCIRState *ir = utb_new();
  tcc_state->optimize = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_ub_only_body_elide(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  utb_free(ir);
  reset_state();
  return 0;
}

/* ======================================================== null_store_dom_return
 *
 * O2-only: a STORE through a compile-time NULL pointer that dominates every
 * function exit (RETURNVOID or CFG leaf) makes the whole body UB -> NOP all.
 */

/* POSITIVE: direct STORE through an immediate NULL address followed by
 * RETURNVOID in the same (only) block -> whole body collapses.
 *   0: STORE [NULL], #1
 *   1: RETURNVOID */
UT_TEST(test_nsdr_immediate_null_store_collapses)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_imm(0, I32)), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_null_store_dom_return(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->leaffunc, 1);

  utb_free(ir);
  reset_state();
  return 0;
}

/* POSITIVE: the STORE address is a TEMP assigned constant 0 earlier in
 * linear flow (known-zero tracking through ASSIGN).
 *   0: ASSIGN T0 <- #0
 *   1: STORE T0, #1
 *   2: RETURNVOID */
UT_TEST(test_nsdr_known_zero_temp_store_collapses)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_null_store_dom_return(ir);

  UT_ASSERT_EQ(changes, 1);
  for (int i = 0; i < 3; i++)
    UT_ASSERT_EQ(utb_op(ir, i), TCCIR_OP_NOP);
  UT_ASSERT_EQ(ir->leaffunc, 1);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): the STORE address is a TEMP assigned a NON-zero constant
 * -- no UB, nothing to elide. */
UT_TEST(test_nsdr_nonzero_temp_store_no_change)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  int asn = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(4, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(1, I32), UTB_NONE);
  int ret = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_null_store_dom_return(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, asn), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, ret), TCCIR_OP_RETURNVOID);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): the NULL store sits on only one branch -- the other
 * branch's RETURNVOID is NOT dominated by the store block, so the function
 * can exit cleanly and must be kept.
 *   0: JUMPIF -> 3, T0
 *   1: STORE [NULL], #1
 *   2: RETURNVOID
 *   3: RETURNVOID  (reachable without passing the store) */
UT_TEST(test_nsdr_non_dominated_return_no_change)
{
  TCCIRState *ir = utb_new();
  set_optimize2();

  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_temp(0, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_imm(0, I32)), utb_imm(1, I32), UTB_NONE);
  int ret1 = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int ret2 = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_null_store_dom_return(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, ret1), TCCIR_OP_RETURNVOID);
  UT_ASSERT_EQ(utb_op(ir, ret2), TCCIR_OP_RETURNVOID);

  utb_free(ir);
  reset_state();
  return 0;
}

/* NEGATIVE (guard): the pass is O2-only; at -O1 it must not fire even on an
 * otherwise-qualifying body. */
UT_TEST(test_nsdr_optimize_gate)
{
  TCCIRState *ir = utb_new();
  tcc_state->optimize = 1;

  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_imm(0, I32)), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_null_store_dom_return(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  utb_free(ir);
  reset_state();
  return 0;
}

/* Empty-IR guards for the whole-function elision passes: they exercise the
 * wrapper line and the n==0 / optimize-gate early returns, complementing the
 * dedicated positive/negative sections above. */

UT_TEST(test_ub_only_body_elide_empty)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_ub_only_body_elide(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_ub_only_body_elide_ex_empty)
{
  TCCIRState *ir = utb_new();
  IROptCtx ctx = utb_ctx(ir);
  UT_ASSERT_EQ(tcc_ir_opt_ub_only_body_elide_ex(&ctx), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_local_only_body_elide_empty)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_local_only_body_elide(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_local_only_body_elide_ex_empty)
{
  TCCIRState *ir = utb_new();
  IROptCtx ctx = utb_ctx(ir);
  UT_ASSERT_EQ(tcc_ir_opt_local_only_body_elide_ex(&ctx), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_const_return_uninit_elide_empty)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_const_return_uninit_elide(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_const_return_uninit_elide_ex_empty)
{
  TCCIRState *ir = utb_new();
  IROptCtx ctx = utb_ctx(ir);
  UT_ASSERT_EQ(tcc_ir_opt_const_return_uninit_elide_ex(&ctx), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_null_store_dom_return_empty)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_null_store_dom_return(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_null_store_dom_return_ex_empty)
{
  TCCIRState *ir = utb_new();
  IROptCtx ctx = utb_ctx(ir);
  UT_ASSERT_EQ(tcc_ir_opt_null_store_dom_return_ex(&ctx), 0);
  utb_free(ir);
  return 0;
}

UT_COVERS("useless_function_body");
UT_COVERS("noreturn_collapse");
UT_COVERS("trap_only_body_suppress");
UT_COVERS("infinite_self_recursion");
UT_COVERS("noreturn_call_epilogue_suppress");
UT_COVERS("compact_nops");
UT_COVERS("ub_only_body_elide");
UT_COVERS("local_only_body_elide");
UT_COVERS("const_return_uninit_elide");
UT_COVERS("null_store_dom_return");
