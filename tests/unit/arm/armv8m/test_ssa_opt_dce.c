/*
 *  test_ssa_opt_dce.c - dead code elimination pass
 *
 *  Phase 4: large, high-bug-density pass.
 *
 *  Covers:
 *    - dce_temp_worklist(): dead TEMP elimination including side-effect guards
 *    - dce_unreachable(): NOP'ing code after JUMP/RETURN until next target
 *    - dce_dead_var_stores(): eliminating stores to dead/address-taken VARs
 *    - dce_dead_stackloc_stores(): eliminating unread anonymous stack stores
 *    - dce_dead_overwrite_stores(): eliminating overwritten stack stores
 *    - dce_dead_phi_cycles(): breaking dead phi/ASSIGN cycles
 *    - ssa_opt_dce(): optimize-level gating
 *
 *  HARNESS NOTES:
 *    - Links the real passes in source/opt/ssa/dce/ via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "source/opt/ssa/include/ssa_opt.h"

#define I32 IROP_BTYPE_INT32

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

static int run_dce_with_opt_level(IRSSAOptCtx *ctx, int level)
{
  static TCCState tcc_state_storage;

  memset(&tcc_state_storage, 0, sizeof(tcc_state_storage));
  tcc_state_storage.optimize = level;
  tcc_enter_state(&tcc_state_storage);
  int changed = ssa_opt_dce(ctx);
  tcc_exit_state(&tcc_state_storage);
  return changed;
}

static int run_dce_with_opt(IRSSAOptCtx *ctx)
{
  return run_dce_with_opt_level(ctx, 1);
}

/* Anonymous stack-location operand (no associated vreg). */
static inline IROperand utb_anon_sl(int32_t off, int is_lval, int btype)
{
  return irop_make_stackoff(0, off, is_lval, 0, 0, btype);
}

/* Local VAR operand (stack slot, not a pointer value). */
static inline IROperand utb_lvar(int pos, int btype)
{
  IROperand op = utb_var(pos, btype);
  op.is_local = 1;
  return op;
}

/* Patch the is_local flag of an instruction operand. */
static inline void set_op_local(ssa_ctx *c, int instr, int slot)
{
  IROperand *pool = c->ir->iroperand_pool;
  int base = c->ir->compact_instructions[instr].operand_base;
  pool[base + slot].is_local = 1;
}

/* ========================================================================
 * Dead assignment: t1 = t0; (t1 unused) -> NOP t1
 * ======================================================================== */

UT_TEST(test_dce_dead_assign)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_ctx_init_manual(&c);
  /* t0 = #1; t1 = t0; (t1 has no uses) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  int t1_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                           utb_temp(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  /* The pass should NOP the dead assignment. */
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, t1_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Live assignment: t1 = t0; use(t1) -> keep t1
 * ======================================================================== */

UT_TEST(test_dce_live_assign)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_ctx_init_manual(&c);
  /* t0 = #1; t1 = t0; return t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  int t1_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                           utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  /* The pass should keep t1 (it has a use). */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, t1_i), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Dead value-def STORE: T = STORE val (non-lval dest) -> NOP
 * ======================================================================== */

UT_TEST(test_dce_dead_store_value_def)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  /* t0 = STORE #42 (non-lval dest is a value-def, not a memory write) */
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE, utb_temp(0, I32),
                              utb_imm(42, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Live side-effect temp: FUNCCALLVAL result with no uses is kept
 * ======================================================================== */

UT_TEST(test_dce_live_side_effect_temp_kept)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_ctx_init_manual(&c);
  /* t0 = FUNCCALLVAL t1, #0 (t0 unused, but call is a side effect) */
  int call_i = ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32),
                              utb_temp(1, I32), utb_imm(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, call_i), TCCIR_OP_FUNCCALLVAL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Dead MLA: T = MLA a, b, acc (T unused) -> NOP
 * ======================================================================== */

UT_TEST(test_dce_dead_mla)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_ctx_init_manual(&c);
  /* t0 = #1; t1 = #2; t2 = #3; t4 = MLA t0, t1, t2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(3, I32));
  int mla_i = ssa_add_instr4(&c, TCCIR_OP_MLA, utb_temp(4, I32),
                             utb_temp(0, I32), utb_temp(1, I32),
                             utb_temp(2, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, mla_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Cascading dead chain: t2 = t1 = t0 = #1 -> all NOP
 * ======================================================================== */

UT_TEST(test_dce_dead_temp_chain)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_ctx_init_manual(&c);
  int i0 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                         utb_imm(1, I32));
  int i1 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                         utb_temp(0, I32));
  int i2 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32),
                         utb_temp(1, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 3);
  UT_ASSERT_EQ(utb_op(c.ir, i0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, i1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, i2), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Unreachable code after RETURNVALUE is NOP'd
 * ======================================================================== */

UT_TEST(test_dce_unreachable_after_return)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_ctx_init_manual(&c);
  int i0 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                         utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));
  int i2 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                         utb_imm(2, I32));
  (void)i0;

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, i2), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Unreachable code between JUMP and its target is NOP'd
 * ======================================================================== */

UT_TEST(test_dce_unreachable_jump_target)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_ctx_init_manual(&c);
  int i0 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                         utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE);
  int i2 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                         utb_imm(2, I32));
  int i3 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32),
                         utb_imm(3, I32));
  int i4 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32),
                         utb_imm(4, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32));
  (void)i0;

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 2);
  UT_ASSERT_EQ(utb_op(c.ir, i2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, i3), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, i4), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Indirect jump disables the unreachable-code pass
 * ======================================================================== */

UT_TEST(test_dce_unreachable_indirect_jump)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_ctx_init_manual(&c);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_IJUMP, UTB_NONE, utb_temp(0, I32));
  int i2 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                         utb_imm(2, I32));
  /* Keep t1 live so the unreachable-code sub-pass is the one being tested. */
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, i2), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Dead VAR store: V0 unused -> NOP the STORE
 * ======================================================================== */

UT_TEST(test_dce_dead_var_store)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_init_manual(&c);
  c.ir->next_local_variable = 1;
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_var(0, I32)),
                              utb_imm(7, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Live VAR store: V0 is read later -> keep the STORE
 * ======================================================================== */

UT_TEST(test_dce_live_var_store_load)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  c.ir->next_local_variable = 1;
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_var(0, I32)),
                              utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_lval(utb_var(0, I32)));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Address-taken VAR via LEA keeps the STORE
 * ======================================================================== */

UT_TEST(test_dce_var_addrtaken_lea)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  c.ir->next_local_variable = 1;
  int lea_i = ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32),
                            utb_var(0, I32));
  /* Mark the LEA's VAR source as a local stack-slot reference. */
  set_op_local(&c, lea_i, 1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_temp(0, I32)),
                              utb_imm(7, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Nested-function chain slot disables dead-VAR-store elimination
 * ======================================================================== */

UT_TEST(test_dce_dead_var_nested_bailout)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  c.ir->next_local_variable = 1;
  ssa_add_instr(&c, TCCIR_OP_INIT_CHAIN_SLOT, UTB_NONE,
                utb_imm(0, I32));
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_var(0, I32)),
                              utb_imm(7, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MLA accumulator that reads a VAR keeps a later store to that VAR
 * ======================================================================== */

UT_TEST(test_dce_mla_var_accumulator_keeps_store)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_ctx_init_manual(&c);
  c.ir->next_local_variable = 1;
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_var(0, I32)),
                              utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(3, I32));
  ssa_add_instr4(&c, TCCIR_OP_MLA, utb_temp(4, I32),
                 utb_temp(0, I32), utb_temp(1, I32),
                 utb_var(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * STORE_INDEXED through a VAR pointer marks the VAR as used
 * ======================================================================== */

UT_TEST(test_dce_store_indexed_var_pointer_kept)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  c.ir->next_local_variable = 1;
  int store_i = ssa_add_instr3(&c, TCCIR_OP_STORE_INDEXED,
                               utb_lval(utb_var(0, I32)),
                               utb_imm(7, I32),
                               utb_imm(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE_INDEXED);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Write-through-pointer chain to a dead VAR is eliminated
 * ======================================================================== */

UT_TEST(test_dce_dead_var_write_through_pointer)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  c.ir->next_local_variable = 1;
  int lea_i = ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32),
                            utb_var(0, I32));
  set_op_local(&c, lea_i, 1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_temp(0, I32)),
                              utb_imm(7, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 2);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, lea_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Dead anonymous StackLoc store -> NOP
 * ======================================================================== */

UT_TEST(test_dce_dead_stackloc_store)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(7, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Read StackLoc store via LOAD keeps the STORE
 * ======================================================================== */

UT_TEST(test_dce_stackloc_store_read_by_load)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_anon_sl(0, 1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Escaped StackLoc address keeps the STORE
 * ======================================================================== */

UT_TEST(test_dce_stackloc_store_address_escape)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  int lea_i = ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32),
                            utb_anon_sl(0, 0, I32));
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_temp(0, I32)),
                              utb_imm(7, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, lea_i), TCCIR_OP_LEA);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * is_llocal StackLoc store is never eliminated
 * ======================================================================== */

UT_TEST(test_dce_stackloc_store_is_llocal_kept)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  IROperand dest = utb_llocal(utb_anon_sl(0, 1, I32));
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE, dest,
                              utb_imm(7, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Static chain disables StackLoc-store elimination
 * ======================================================================== */

UT_TEST(test_dce_stackloc_static_chain_bailout)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ir->has_static_chain = 1;
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(7, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Dead overwritten StackLoc store: second write kills first
 * ======================================================================== */

UT_TEST(test_dce_dead_overwrite_same_offset)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(1, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(2, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Intervening LOAD keeps the older StackLoc store
 * ======================================================================== */

UT_TEST(test_dce_overwrite_intervening_load)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_anon_sl(0, 1, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(2, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Function call clears pending overwrite state
 * ======================================================================== */

UT_TEST(test_dce_overwrite_call_clears_pending)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(1, I32));
  ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE,
                 utb_temp(0, I32), utb_imm(0, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(2, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Unresolved alias clears pending overwrite state
 * ======================================================================== */

UT_TEST(test_dce_overwrite_unknown_alias)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(1, I32));
  /* Unresolved pointer store through an arbitrary temp. */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)),
                utb_imm(9, I32));
  int store3 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(2, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, store3), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Dead overwritten STORE_INDEXED resolved to the same offset
 * ======================================================================== */

UT_TEST(test_dce_dead_overwrite_indexed)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  int lea_i = ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32),
                            utb_anon_sl(8, 0, I32));
  int store1 = ssa_add_instr4(&c, TCCIR_OP_STORE_INDEXED,
                              utb_temp(0, I32),
                              utb_imm(1, I32),
                              utb_imm(0, I32),
                              utb_imm(0, I32));
  int store2 = ssa_add_instr4(&c, TCCIR_OP_STORE_INDEXED,
                              utb_temp(0, I32),
                              utb_imm(2, I32),
                              utb_imm(0, I32),
                              utb_imm(0, I32));
  (void)lea_i;

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE_INDEXED);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Dead phi cycle is removed when no essential use exists
 * ======================================================================== */

UT_TEST(test_dce_dead_phi_cycle_removed)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_ctx_init_manual(&c);
  c.cfg->num_blocks = 2;
  c.cfg->blocks[0].start_idx = 0;
  c.cfg->blocks[0].end_idx = 2;
  c.cfg->blocks[1].start_idx = 2;
  c.cfg->blocks[1].end_idx = 4;
  c.cfg->blocks[0].idom = -1;
  c.cfg->blocks[1].idom = 0;
  for (int i = 0; i < 4; i++)
    c.cfg->instr_to_block[i] = (i < 2) ? 0 : 1;

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE);
  ssa_add_phi(&c, /*block=*/1, utb_vreg(utb_temp(2, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);
  int copy_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32),
                             utb_temp(2, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 0);
  UT_ASSERT_EQ(utb_op(c.ir, copy_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phi in a natural back-edge region is kept even if apparently dead
 * ======================================================================== */

UT_TEST(test_dce_dead_phi_cycle_backedge_kept)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_ctx_init_manual(&c);
  c.cfg->num_blocks = 2;
  c.cfg->blocks[0].start_idx = 0;
  c.cfg->blocks[0].end_idx = 2;
  c.cfg->blocks[1].start_idx = 2;
  c.cfg->blocks[1].end_idx = 4;
  c.cfg->blocks[0].idom = -1;
  c.cfg->blocks[1].idom = 0;
  c.cfg->blocks[1].num_preds = 2;
  c.cfg->blocks[1].preds = tcc_malloc(2 * sizeof(int));
  c.cfg->blocks[1].preds[0] = 0;
  c.cfg->blocks[1].preds[1] = 1;
  for (int i = 0; i < 4; i++)
    c.cfg->instr_to_block[i] = (i < 2) ? 0 : 1;

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(1, I32), UTB_NONE);
  ssa_add_phi(&c, /*block=*/1, utb_vreg(utb_temp(2, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_temp(2, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * optimize=0 skips advanced DCE sub-passes
 * ======================================================================== */

UT_TEST(test_dce_optimize_zero_skips_advanced)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ir->next_local_variable = 1;
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_var(0, I32)),
                              utb_imm(7, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt_level(c.ctx, 0);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:dce");
