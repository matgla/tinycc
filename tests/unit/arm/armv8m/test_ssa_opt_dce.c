/*
 *  test_ssa_opt_dce.c - dead code elimination pass
 *
 *  Phase 4: large, high-bug-density pass.
 *
 *  Covers:
 *    - dce_temp_worklist(): dead TEMP elimination including side-effect guards
 *    - dce_unreachable(): NOP'ing code after JUMP/JUMPIF/SWITCH_TABLE/RETURN
 *    - dce_dead_var_stores(): eliminating stores to dead/address-taken VARs
 *    - dce_dead_stackloc_stores(): eliminating unread anonymous stack stores
 *    - dce_dead_overwrite_stores(): eliminating overwritten stack stores
 *    - dce_dead_phi_cycles(): breaking dead phi/ASSIGN cycles
 *    - dce_ret_path_frame_store(): frame stores dead at RETURN
 *    - dce_orphan_params(): FUNCPARAMVAL without a matching call
 *    - dce_var_liveness(): cross-block VAR slot liveness
 *    - ssa_opt_dce() / ssa_opt_dce_light(): optimize-level gating
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
  ssa_ctx_alloc_var_intervals(&c, 1);
  /* Slot-def store: is_local && is_lval marks a write to the VAR's own slot
   * (plain lval VAR dest reads as a pointer and is never eliminable). */
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(0, I32)),
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
  ssa_ctx_alloc_var_intervals(&c, 1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(0, I32)),
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
 *
 * The LEA temp must genuinely escape: a write-only pointer chain (LEA temp
 * feeding only stores) is eliminated on purpose — see
 * test_dce_dead_var_write_through_pointer. Storing the pointer through an
 * unknown lval makes it live, marking V0 address-taken.
 * ======================================================================== */

UT_TEST(test_dce_var_addrtaken_lea)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_ctx_init_manual(&c);
  ssa_ctx_alloc_var_intervals(&c, 1);
  int lea_i = ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32),
                            utb_var(0, I32));
  /* Mark the LEA's VAR source as a local stack-slot reference. */
  set_op_local(&c, lea_i, 1);
  /* Escape: publish the pointer through an arbitrary memory location. */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)),
                utb_temp(0, I32));
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
 * Nested-function chain slot disables dead-VAR-store elimination
 * ======================================================================== */

UT_TEST(test_dce_dead_var_nested_bailout)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  ssa_ctx_alloc_var_intervals(&c, 1);
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
  ssa_ctx_alloc_var_intervals(&c, 1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(0, I32)),
                              utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr4(&c, TCCIR_OP_MLA, utb_temp(4, I32),
                 utb_temp(0, I32), utb_temp(1, I32),
                 utb_var(0, I32));
  /* Keep the MLA result live: an unused t4 would cascade-kill the whole
   * chain (including the now-unread V0 store) at the DCE fixpoint. */
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32));

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
  ssa_ctx_alloc_var_intervals(&c, 1);
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
  ssa_ctx_alloc_var_intervals(&c, 1);
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
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(7, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

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
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_anon_sl(0, 1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

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
  int lea_i = ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32),
                            utb_anon_sl(0, 0, I32));
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_temp(0, I32)),
                              utb_imm(7, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

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
  IROperand dest = utb_llocal(utb_anon_sl(0, 1, I32));
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE, dest,
                              utb_imm(7, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

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
  c.ir->has_static_chain = 1;
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(7, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

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
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(1, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(2, I32));
  /* Read the slot back so the surviving second store is not itself
   * eliminated as an unread anonymous stack store. */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_anon_sl(0, 1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 1);
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
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_anon_sl(0, 1, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(2, I32));
  /* Consume both loads so neither is eliminated as dead, and so the second
   * store is read. */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                utb_anon_sl(0, 1, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                 utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

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
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(1, I32));
  ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE,
                 utb_temp(0, I32), utb_imm(0, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(2, I32));
  /* Read the slot back so the second store is not dead. */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                utb_anon_sl(0, 1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

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
 * Unresolved pointer store does not alias a non-escaped local slot
 *
 * dce_dead_overwrite_stores deliberately treats a store through an
 * unresolvable pointer as never reaching a local stack slot ("never a local
 * slot", dead_overwrite_stores.c): reaching the slot would require the LEA
 * of the slot, which resolves. So the pending store is still overwritten.
 * ======================================================================== */

UT_TEST(test_dce_overwrite_unresolved_ptr_store)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(1, I32));
  /* Unresolved pointer store through an arbitrary temp. */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)),
                utb_imm(9, I32));
  int store3 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(2, I32));
  /* Read the slot back so the last store is not dead. */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                utb_anon_sl(0, 1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_NOP);
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

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

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
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);
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
  /* JUMP dest is an instruction index: 2 == start of block 1. */
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
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
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);
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
  /* JUMP dest is an instruction index: 2 == start of block 1. */
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
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
  ssa_ctx_alloc_var_intervals(&c, 1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(0, I32)),
                              utb_imm(7, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt_level(c.ctx, 0);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Volatile VAR slot store is never eliminated
 * ======================================================================== */

UT_TEST(test_dce_dead_var_volatile_store_kept)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_init_manual(&c);
  ssa_ctx_alloc_var_intervals(&c, 1);
  c.ir->variables_live_intervals[0].is_volatile = 1;
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(0, I32)),
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
 * VAR read passed as a call parameter keeps the store
 * ======================================================================== */

UT_TEST(test_dce_var_store_kept_by_funcparam)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  ssa_ctx_alloc_var_intervals(&c, 1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(0, I32)),
                              utb_imm(7, I32));
  int param_i = ssa_add_instr3(&c, TCCIR_OP_FUNCPARAMVAL, UTB_NONE,
                              utb_var(0, I32),
                              utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE,
                 utb_temp(0, I32),
                 utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, param_i), TCCIR_OP_FUNCPARAMVAL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * VAR def in one block, use in a successor: store stays live
 * ======================================================================== */

UT_TEST(test_dce_var_liveness_cross_block_use)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_alloc_var_intervals(&c, 1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(0, I32)),
                              utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_lval(utb_var(0, I32)));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * VAR stored in one block and never read anywhere: killed across blocks
 * ======================================================================== */

UT_TEST(test_dce_var_liveness_dead_store_multiblock)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_alloc_var_intervals(&c, 1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(0, I32)),
                              utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(5, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Frame store with only a pure temp read before RETURNVALUE is dead
 * ======================================================================== */

UT_TEST(test_dce_ret_frame_store_returnvalue_pure)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_anon_sl(0, 1, I32));
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Frame store followed by RETURNVOID is dead (frame dies at return)
 * ======================================================================== */

UT_TEST(test_dce_ret_frame_store_returnvoid)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_anon_sl(0, 1, I32));
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(1, I32));
  /* Keep t0 live without reading the slot after the target store; the
   * sl[4] store is itself dead (unread) and vanishes first. */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_anon_sl(4, 1, I32),
                utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Frame store with a LOAD after it is kept
 * ======================================================================== */

UT_TEST(test_dce_ret_frame_store_load_after_kept)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_anon_sl(0, 1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Frame store followed by an lval-sourced ASSIGN (memory read) is kept
 * ======================================================================== */

UT_TEST(test_dce_ret_frame_store_impure_src_kept)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_anon_sl(0, 1, I32));
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(1, I32));
  /* t1 = *t0 -- an unresolvable memory read between the store and return. */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                utb_lval(utb_temp(0, I32)));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Orphaned FUNCPARAMVAL (no matching call id) is removed
 * ======================================================================== */

UT_TEST(test_dce_orphan_param_removed)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_init_manual(&c);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  int param_i = ssa_add_instr3(&c, TCCIR_OP_FUNCPARAMVAL, UTB_NONE,
                              utb_temp(0, I32),
                              utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, param_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * FUNCPARAMVAL with a matching call id is kept
 * ======================================================================== */

UT_TEST(test_dce_param_with_matching_call_kept)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  int param_i = ssa_add_instr3(&c, TCCIR_OP_FUNCPARAMVAL, UTB_NONE,
                              utb_temp(0, I32),
                              utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE,
                 utb_temp(1, I32),
                 utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, param_i), TCCIR_OP_FUNCPARAMVAL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Unreachable code past a JUMPIF+JUMP pair is NOP'd (JUMPIF both-edges)
 * ======================================================================== */

UT_TEST(test_dce_unreachable_jumpif_region)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(5, I32), UTB_NONE);
  /* Side-effecting calls: only dce_unreachable can remove these. */
  int call1 = ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE,
                             utb_temp(1, I32),
                             utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int call2 = ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE,
                             utb_temp(1, I32),
                             utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 0), I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 2);
  UT_ASSERT_EQ(utb_op(c.ir, call1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, call2), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Unreachable code outside all switch-table targets is NOP'd
 * ======================================================================== */

UT_TEST(test_dce_unreachable_switch_table)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_init_manual(&c);
  c.ir->num_switch_tables = 1;
  c.ir->switch_tables = tcc_mallocz(sizeof(TCCIRSwitchTable));
  c.ir->switch_tables[0].num_entries = 1;
  c.ir->switch_tables[0].targets = tcc_malloc(sizeof(int));
  c.ir->switch_tables[0].targets[0] = 3;
  c.ir->switch_tables[0].default_target = 3;

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr3(&c, TCCIR_OP_SWITCH_TABLE, UTB_NONE,
                 utb_temp(0, I32), utb_imm(0, I32));
  int call_i = ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE,
                              utb_temp(1, I32),
                              utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, call_i), TCCIR_OP_NOP);

  tcc_free(c.ir->switch_tables[0].targets);
  tcc_free(c.ir->switch_tables);
  c.ir->switch_tables = NULL;
  c.ir->num_switch_tables = 0;
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_dce_light: dead temp worklist + unreachable only
 * ======================================================================== */

UT_TEST(test_dce_light)
{
  static TCCState tcc_state_storage;
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_ctx_init_manual(&c);
  int dead_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(5, I32));
  int unreach_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32),
                                utb_imm(2, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  memset(&tcc_state_storage, 0, sizeof(tcc_state_storage));
  tcc_state_storage.optimize = 1;
  tcc_enter_state(&tcc_state_storage);
  int changed = ssa_opt_dce_light(c.ctx);
  tcc_exit_state(&tcc_state_storage);

  UT_ASSERT_EQ(changed, 2);
  UT_ASSERT_EQ(utb_op(c.ir, dead_i), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, unreach_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Overwrite tracking honors INT16 width: same offset kills the first store
 * ======================================================================== */

UT_TEST(test_dce_dead_overwrite_int16)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, IROP_BTYPE_INT16),
                             utb_imm(1, IROP_BTYPE_INT16));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, IROP_BTYPE_INT16),
                             utb_imm(2, IROP_BTYPE_INT16));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, IROP_BTYPE_INT16),
                utb_anon_sl(0, 1, IROP_BTYPE_INT16));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Overwrite tracking honors INT64 width: same offset kills the first store
 * ======================================================================== */

UT_TEST(test_dce_dead_overwrite_int64)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, IROP_BTYPE_INT64),
                             utb_imm(1, IROP_BTYPE_INT64));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, IROP_BTYPE_INT64),
                             utb_imm(2, IROP_BTYPE_INT64));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, IROP_BTYPE_INT64),
                utb_anon_sl(0, 1, IROP_BTYPE_INT64));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * STRUCT-typed store has no scalar width: never overwrite-eliminated
 * ======================================================================== */

UT_TEST(test_dce_overwrite_struct_width_unknown)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, IROP_BTYPE_STRUCT),
                             utb_imm(1, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, IROP_BTYPE_STRUCT),
                             utb_imm(2, I32));
  /* Read the slot back so the stackloc pass keeps both stores; the
   * overwrite pass alone must refuse to track the zero-width stores. */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_anon_sl(0, 1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

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
 * LOAD_INDEXED of the same slot evicts the pending overwritten store
 * ======================================================================== */

UT_TEST(test_dce_overwrite_load_indexed_evicts)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32),
                utb_anon_sl(8, 0, I32));
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(8, 1, I32),
                             utb_imm(1, I32));
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32),
                 utb_temp(0, I32), utb_imm(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_anon_sl(8, 1, I32),
                utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                utb_anon_sl(8, 1, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32),
                 utb_temp(1, I32), utb_temp(2, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * STORE_POSTINC clears pending overwrite state (base register changes)
 * ======================================================================== */

UT_TEST(test_dce_overwrite_store_postinc_clears)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32),
                utb_anon_sl(16, 0, I32));
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(1, I32));
  ssa_add_instr3(&c, TCCIR_OP_STORE_POSTINC, utb_temp(0, I32),
                 utb_imm(9, I32), utb_imm(4, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_anon_sl(0, 1, I32),
                utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                utb_anon_sl(0, 1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Passing a slot pointer as a value conservatively blocks the overwrite
 * ======================================================================== */

UT_TEST(test_dce_overwrite_lea_value_use_evicts)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32),
                utb_anon_sl(0, 0, I32));
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(1, I32));
  /* The slot address is consumed as a value: pending store must survive. */
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                 utb_temp(0, I32), utb_temp(0, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_anon_sl(0, 1, I32),
                             utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);
  /* The second store is unread, so the stackloc pass removes it. */
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * StackLoc store read only by a dead-temp LOAD is eliminated
 * ======================================================================== */

UT_TEST(test_dce_stackloc_store_dead_load_temp)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(7, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                             utb_anon_sl(0, 1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 2);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * A LOAD whose dest temp is beyond vinfo capacity counts as a live read
 * ======================================================================== */

UT_TEST(test_dce_stackloc_load_beyond_vinfo_cap)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_anon_sl(0, 1, I32),
                              utb_imm(7, I32));
  /* t5 has no vinfo slot (cap is 1): treated as a live read. */
  ssa_add_instr(&c, TCCIR_OP_LOAD,
                utb_temp(5, I32), utb_anon_sl(0, 1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phi kept alive through an ASSIGN chain (liveness fixpoint propagation)
 * ======================================================================== */

UT_TEST(test_dce_phi_kept_via_assign_chain)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/7);
  ssa_ctx_init_manual(&c);
  c.cfg->num_blocks = 2;
  c.cfg->blocks[0].start_idx = 0;
  c.cfg->blocks[0].end_idx = 2;
  c.cfg->blocks[1].start_idx = 2;
  c.cfg->blocks[1].end_idx = 5;
  c.cfg->blocks[0].idom = -1;
  c.cfg->blocks[1].idom = 0;
  c.cfg->blocks[1].num_preds = 2;
  c.cfg->blocks[1].preds = tcc_malloc(2 * sizeof(int));
  c.cfg->blocks[1].preds[0] = 0;
  c.cfg->blocks[1].preds[1] = 1;
  for (int i = 0; i < 5; i++)
    c.cfg->instr_to_block[i] = (i < 2) ? 0 : 1;

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  ssa_add_phi(&c, /*block=*/1, utb_vreg(utb_temp(2, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_temp(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(6, I32), utb_temp(3, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(6, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Backedge phis referencing each other are kept (phi-operand use scan)
 * ======================================================================== */

UT_TEST(test_dce_phi_backedge_mutual_refs_kept)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/5);
  ssa_ctx_init_manual(&c);
  c.cfg->num_blocks = 2;
  c.cfg->blocks[0].start_idx = 0;
  c.cfg->blocks[0].end_idx = 2;
  c.cfg->blocks[1].start_idx = 2;
  c.cfg->blocks[1].end_idx = 3;
  c.cfg->blocks[0].idom = -1;
  c.cfg->blocks[1].idom = 0;
  c.cfg->blocks[1].num_preds = 2;
  c.cfg->blocks[1].preds = tcc_malloc(2 * sizeof(int));
  c.cfg->blocks[1].preds[0] = 0;
  c.cfg->blocks[1].preds[1] = 1;
  for (int i = 0; i < 3; i++)
    c.cfg->instr_to_block[i] = (i < 2) ? 0 : 1;

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  /* t2 and t3 are only referenced by each other's phi: no IR use at all. */
  ssa_add_phi(&c, /*block=*/1, utb_vreg(utb_temp(2, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);
  ssa_add_phi(&c, /*block=*/1, utb_vreg(utb_temp(3, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(2, I32)) }, 2);
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 2);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Backedge phi with unread dest and unread operands is removed
 * ======================================================================== */

UT_TEST(test_dce_phi_backedge_unread_removed)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/3);
  ssa_ctx_init_manual(&c);
  c.cfg->num_blocks = 2;
  c.cfg->blocks[0].start_idx = 0;
  c.cfg->blocks[0].end_idx = 2;
  c.cfg->blocks[1].start_idx = 2;
  c.cfg->blocks[1].end_idx = 3;
  c.cfg->blocks[0].idom = -1;
  c.cfg->blocks[1].idom = 0;
  c.cfg->blocks[1].num_preds = 2;
  c.cfg->blocks[1].preds = tcc_malloc(2 * sizeof(int));
  c.cfg->blocks[1].preds[0] = 0;
  c.cfg->blocks[1].preds[1] = 1;
  for (int i = 0; i < 3; i++)
    c.cfg->instr_to_block[i] = (i < 2) ? 0 : 1;

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  /* Duplicate operands: nothing reads t2, and t0 is read only by this phi. */
  ssa_add_phi(&c, /*block=*/1, utb_vreg(utb_temp(2, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(0, I32)) }, 2);
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(5, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Removing one phi rebuilds uses for the surviving ones
 *
 * Also covers the liveness-marking sweep for non-ASSIGN sources (ADD/MLA),
 * STORE dest marking, and ASSIGN with a non-TEMP destination.
 * ======================================================================== */

UT_TEST(test_dce_phi_removed_rebuild_keeps_live_phi)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/10);
  ssa_ctx_init_manual(&c);
  c.cfg->num_blocks = 2;
  c.cfg->blocks[0].start_idx = 0;
  c.cfg->blocks[0].end_idx = 2;
  c.cfg->blocks[1].start_idx = 2;
  c.cfg->blocks[1].end_idx = 9;
  c.cfg->blocks[0].idom = -1;
  c.cfg->blocks[1].idom = 0;
  c.cfg->blocks[1].num_preds = 2;
  c.cfg->blocks[1].preds = tcc_malloc(2 * sizeof(int));
  c.cfg->blocks[1].preds[0] = 0;
  c.cfg->blocks[1].preds[1] = 1;
  for (int i = 0; i < 9; i++)
    c.cfg->instr_to_block[i] = (i < 2) ? 0 : 1;

  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE);
  /* Kept: operand t0 is genuinely read below. */
  ssa_add_phi(&c, /*block=*/1, utb_vreg(utb_temp(2, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)),
                           utb_vreg(utb_temp(3, I32)) }, 2);
  /* Removed: t7 and its operand t5 are read by nothing. */
  ssa_add_phi(&c, /*block=*/1, utb_vreg(utb_temp(7, I32)),
              (int32_t[]){ utb_vreg(utb_temp(5, I32)),
                           utb_vreg(utb_temp(5, I32)) }, 2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_temp(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(8, I32),
                 utb_temp(0, I32), utb_temp(0, I32));
  ssa_add_instr4(&c, TCCIR_OP_MLA, utb_temp(9, I32),
                 utb_temp(0, I32), utb_temp(0, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(4, I32)),
                utb_imm(9, I32));
  static Sym g4;
  g4.v = 104;
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_symref(c.ir, &g4, 0, 0, 0, I32),
                utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(3, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 1), 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Trailing store after the last VAR read is dead (var_liveness removal)
 * ======================================================================== */

UT_TEST(test_dce_var_liveness_trailing_dead_store)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_alloc_var_intervals(&c, 1);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_lval(utb_lvar(0, I32)),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_lval(utb_var(0, I32)));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_lval(utb_lvar(0, I32)),
                             utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * VAR read as src2 of an arithmetic op keeps the store
 * ======================================================================== */

UT_TEST(test_dce_var_liveness_var_src2_use)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_ctx_alloc_var_intervals(&c, 1);
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(0, I32)),
                              utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32),
                 utb_temp(1, I32), utb_var(0, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Global (is_sym) store is not a VAR slot def; left untouched
 * ======================================================================== */

UT_TEST(test_dce_var_liveness_sym_dest_store)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_alloc_var_intervals(&c, 1);
  static Sym g;
  g.v = 105;
  int gstore = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_lvar(0, I32)),
                utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_lval(utb_var(0, I32)));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, gstore), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * STRUCT-typed and is_llong interval defs are not treated as VAR slot defs
 * ======================================================================== */

UT_TEST(test_dce_var_liveness_struct_and_llong_defs_kept)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_ctx_alloc_var_intervals(&c, 2);
  c.ir->variables_live_intervals[1].is_llong = 1;
  /* Read both VARs first so dead_var_stores keeps the later stores. */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_lval(utb_var(0, I32)));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                utb_lval(utb_var(1, I32)));
  int s_struct = ssa_add_instr(&c, TCCIR_OP_STORE,
                               utb_lval(utb_lvar(0, IROP_BTYPE_STRUCT)),
                               utb_imm(1, I32));
  int s_llong = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(1, I32)),
                              utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                 utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, s_struct), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, s_llong), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * BUG PIN: store to a dead VAR slot with a volatile lval source
 *
 * The fused mem-to-mem copy `STORE lval(V0slot) <- lval(V1)` embodies a
 * READ of V1. When V1 is volatile, that read is a mandated side effect, so
 * the store must survive even though V0 is never read. var_liveness has the
 * guard (vl_removable rejects volatile sources); dce_dead_var_stores does
 * not, and runs first. This test pins the CURRENT (buggy) outcome; flip the
 * assertions to "store kept" once fixed. See docs/bugs/.
 * ======================================================================== */

UT_TEST(test_dce_dead_var_store_volatile_lval_src_bug)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  ssa_ctx_init_manual(&c);
  ssa_ctx_alloc_var_intervals(&c, 2);
  c.ir->variables_live_intervals[1].is_volatile = 1;
  int store_i = ssa_add_instr(&c, TCCIR_OP_STORE,
                              utb_lval(utb_lvar(0, I32)),
                              utb_lval(utb_var(1, I32)));

  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce_with_opt(c.ctx);
  /* Buggy behavior: the store (and with it the volatile read of V1) is
   * eliminated. Correct behavior: changed == 0, store kept. */
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:dce");
