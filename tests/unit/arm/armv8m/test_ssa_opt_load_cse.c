/*
 *  test_ssa_opt_load_cse.c - load common subexpression elimination
 *
 *  Phase 4: Large, high-bug-density pass.
 *
 *  Covers:
 *    - Stack Store-Load Forwarding: forward tracked stack stores into loads
 *    - T_vreg-deref forwarding: forward stores through TEMP pointer derefs
 *    - LOAD_INDEXED CSE: dedup indexed loads with constant idx/scale
 *    - Invalidation: calls, stores to overlapping addresses
 *
 *  HARNESS NOTES:
 *    - Links the real source/opt/ssa/memory/load_cse.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 *    - Some tests need a real tcc_state for USING_GLOBALS; see setup_tcc_state.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include <limits.h>
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64
#define I8  IROP_BTYPE_INT8

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* Helper to set up tcc_state for tests that need USING_GLOBALS. */
static void setup_tcc_state(void)
{
  static TCCState tcc_state_storage;
  memset(&tcc_state_storage, 0, sizeof(tcc_state_storage));
  tcc_state_storage.optimize = 1;
  tcc_enter_state(&tcc_state_storage);
}

static void teardown_tcc_state(void)
{
  static TCCState tcc_state_storage;
  memset(&tcc_state_storage, 0, sizeof(tcc_state_storage));
  tcc_exit_state(&tcc_state_storage);
}

/* ------------------------------------------------------------------ helpers */

#define VR_TMP(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, (p))
#define VR_VAR(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, (p))

/* A ctx with symref pools + live-interval arrays ready for global/VAR fixtures. */
static inline ssa_ctx ssa_ctx_new_full(int blocks, int temps)
{
  ssa_ctx c = ssa_ctx_new(blocks, temps);
  utb_pools_init(c.ir);
  c.ir->compact_instructions_size = UTB_MAX_INSTR;
  c.ir->temporary_variables_live_intervals_size = 64;
  c.ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 64);
  c.ir->variables_live_intervals_size = 64;
  c.ir->variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 64);
  c.ir->parameters_live_intervals_size = 64;
  c.ir->parameters_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 64);
  return c;
}

static inline void ssa_init_sym(Sym *s, int tok, int vt_flags)
{
  memset(s, 0, sizeof(*s));
  s->v = tok;
  s->type.t = VT_INT | vt_flags;
}

static inline IROperand ssa_symref_lval(ssa_ctx *c, Sym *sym, int32_t addend, int btype)
{
  uint32_t sidx = tcc_ir_pool_add_symref(c->ir, sym, addend, 0);
  return irop_make_symref(0, sidx, /*is_lval*/ 1, /*is_local*/ 0, /*is_const*/ 0, btype);
}

/* Allocate a VAR vreg and mark it address-taken. */
static inline int32_t ssa_alloc_addrtaken_var(ssa_ctx *c)
{
  int32_t vr = tcc_ir_vreg_alloc_var(c->ir);
  tcc_ir_vreg_flag_addrtaken_set(c->ir, vr);
  return vr;
}

/* ========================================================================
 * Stack Store-Load Forwarding: tracked stack store forwarded into load
 * ======================================================================== */

UT_TEST(test_stack_fwd_basic)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* StackLoc[0] <- t0; t1 = LOAD(StackLoc[0]) → t1 = t0 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Store-Load Forwarding: no tracked store → no forwarding
 * ======================================================================== */

UT_TEST(test_stack_fwd_no_store)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 3);
  /* t0 = LOAD(StackLoc[0]) — no prior store → no forwarding */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Store-Load Forwarding: overlapping store invalidates earlier tracking
 * ======================================================================== */

UT_TEST(test_stack_fwd_overlap_invalidates)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 6);
  /* StackLoc[0] <- t0; StackLoc[0] <- t1; t2 = LOAD(StackLoc[0]) → t2 = t1 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(1, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  /* Should forward from t1 (the later store), not t0 */
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * T_vreg-deref forwarding: store through TEMP pointer forwarded into load
 * ======================================================================== */

UT_TEST(test_tvstore_fwd_basic)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = &StackLoc[0]; *t0 = t2; t1 = *t0 → t1 = t2 */
  /* t0 = LEA(StackLoc[0]) */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 1, 0, 0, I32));
  /* *t0 = t2 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_temp(2, I32));
  /* t1 = *t0 */
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_lval(utb_temp(0, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  /* Load should be rewritten to ASSIGN t2 */
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * LOAD_INDEXED CSE: same indexed load CSE'd
 * ======================================================================== */

UT_TEST(test_iload_cse_duplicate)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = *(t1 + #0<<2); t2 = *(t1 + #0<<2) → t2 = t0 */
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32),
                 utb_temp(1, I32), utb_imm(0, I32), utb_imm(2, I32));
  int t2_i = ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32),
                            utb_temp(1, I32), utb_imm(0, I32), utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  /* Second load should be rewritten to ASSIGN t0 */
  UT_ASSERT_EQ(utb_op(c.ir, t2_i), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * LOAD_INDEXED CSE: different indices → no CSE
 * ======================================================================== */

UT_TEST(test_iload_cse_different_idx)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = *(t1 + #0<<2); t2 = *(t1 + #1<<2) → no CSE */
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32),
                 utb_temp(1, I32), utb_imm(0, I32), utb_imm(2, I32));
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(2, I32),
                 utb_temp(1, I32), utb_imm(1, I32), utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Invalidation: function call clears all forwarding state
 * ======================================================================== */

UT_TEST(test_invalidated_by_call)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* StackLoc[0] <- t0; call foo(); t1 = LOAD(StackLoc[0]) → no fwd (call invalidates) */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE);
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Stack Store-Load Forwarding: immediate store value is forwarded
 * ======================================================================== */

UT_TEST(test_stack_fwd_imm)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* StackLoc[0] <- #7; t0 = LOAD(StackLoc[0]) -> t0 = ASSIGN #7 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(7, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, load_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, load_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Stack Store-Load Forwarding: no_stack_fwd gate disables forwarding
 * ======================================================================== */

UT_TEST(test_stack_fwd_no_stack_fwd_gate)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(7, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  c.ctx->no_stack_fwd = 1;

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Stack Store-Load Forwarding: overlapping narrower store invalidates
 * ======================================================================== */

UT_TEST(test_stack_fwd_overlap_narrow_invalidates)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* StackLoc[0] (I32) <- t0; StackLoc[1] (I8) <- #7; t1 = LOAD(StackLoc[0]) -> no fwd */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(1, 1, 0, 0, I8), utb_imm(7, I8));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * T_vreg-deref forwarding: immediate store forwarded into load and ALU use
 * ======================================================================== */

UT_TEST(test_tvstore_fwd_imm)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = &StackLoc[0]; *t0 = #7; t1 = *t0 -> t1 = #7 */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(7, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_lval(utb_temp(0, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, load_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, load_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

UT_TEST(test_tvstore_fwd_into_alu_operand)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = &StackLoc[0]; *t0 = #7; t1 = #3 ADD *t0 -> t1 = #3 ADD #7 */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(7, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                             utb_imm(3, I32), utb_lval(utb_temp(0, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ADD);
  UT_ASSERT(irop_is_immediate(utb_src2(c.ir, add_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src2(c.ir, add_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * T_vreg-deref invalidation: direct stack store aliases the tracked pointer
 * ======================================================================== */

UT_TEST(test_tvstore_invalidated_by_direct_stack_store)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = &StackLoc[0]; *t0 = #7; StackLoc[0] <- #8; t1 = *t0 -> no fwd */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(8, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_lval(utb_temp(0, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * T_vreg-deref tracking: narrow stores are not tracked (width-safe guard)
 * ======================================================================== */

UT_TEST(test_tvstore_narrow_type_not_tracked)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = &StackLoc[0]; *t0 = #7 (I8); t1 = *t0 (I8) -> no fwd */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I8)), utb_imm(7, I8));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I8),
                             utb_lval(utb_temp(0, I8)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * LOAD_INDEXED forwarding: tracked stack store forwarded into indexed load
 * ======================================================================== */

UT_TEST(test_iload_fwd_from_stack_store)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* StackLoc[4] <- #7; t0 = &StackLoc[0]; t1 = *(t0 + #4<<0) -> #7 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(4, 1, 0, 0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 0, 0, 0, I32));
  int load_i = ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32),
                              utb_temp(0, I32), utb_imm(4, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, load_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, load_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * STORE_INDEXED: runtime index forces conservative invalidation
 * ======================================================================== */

UT_TEST(test_store_indexed_runtime_idx_clears_stack_fwd)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 6);
  /* StackLoc[0] <- #7; *(base + runtime_idx) = #9; LOAD StackLoc[0] -> no fwd */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr4(&c, TCCIR_OP_STORE_INDEXED, utb_lval(utb_temp(0, I32)),
                 utb_imm(9, I32), utb_temp(1, I32), utb_imm(0, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * STORE_INDEXED constant index: updates the tracked slot and forwards
 * ======================================================================== */

UT_TEST(test_store_indexed_const_idx_updates_tracked_slot)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* StackLoc[4] <- #7; *(t0 + #4<<0) = #8; *(t0 + #4<<0) -> #8 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(4, 1, 0, 0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr4(&c, TCCIR_OP_STORE_INDEXED, utb_lval(utb_temp(0, I32)),
                 utb_imm(8, I32), utb_imm(4, I32), utb_imm(0, I32));
  int load_i = ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32),
                              utb_temp(0, I32), utb_imm(4, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, load_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, load_i)), 8);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Address-taken VAR invalidation: direct VAR def kills ptr-based trackers
 * ======================================================================== */

UT_TEST(test_addrtaken_var_kills_ptr_state)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 6);
  int32_t v0 = ssa_alloc_addrtaken_var(&c);
  (void)v0;

  /* t0 = V0; t1 = *t0; V0 = t2; t3 = *t0 -> second load must not CSE */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32));
  int load1 = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                            utb_lval(utb_temp(0, I32)));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_temp(2, I32));
  int load2 = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(3, I32),
                            utb_lval(utb_temp(0, I32)));
  (void)load1;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  (void)changed;
  UT_ASSERT_EQ(utb_op(c.ir, load2), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Multi-block dominator: state forks correctly to multiple children
 * ======================================================================== */

UT_TEST(test_multi_child_dom_tree_forwards)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 6);
  /* B0: store; JUMPIF -> B2
   * B1: load (then)
   * B2: load (else) */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_temp(0, I32), UTB_NONE);
  /* B1: instr 3-4 */
  int load_then = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                                utb_stackoff(0, 1, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(7, I32), UTB_NONE);
  /* B2: instr 5-6 */
  int load_else = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                                utb_stackoff(0, 1, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 2);
  UT_ASSERT_EQ(utb_op(c.ir, load_then), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(c.ir, load_else), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Multi-block dominator: non-idom predecessor clears state
 * ======================================================================== */

UT_TEST(test_non_idom_pred_clears_state)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 6);
  /* B0: store; JUMPIF -> B2
   * B1: NOP; JUMP -> B2
   * B2: load (preds: B0 idom, B1 non-idom) */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_temp(0, I32), UTB_NONE);
  /* B1: instr 3-4 */
  ssa_add_instr(&c, TCCIR_OP_NOP, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(5, I32), UTB_NONE);
  /* B2: instr 5 */
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Global Load CSE: same Sym+addend+btype deduplicated
 * ======================================================================== */

UT_TEST(test_gload_cse_basic)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  static Sym g;
  ssa_init_sym(&g, 100, VT_STATIC);

  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), ssa_symref_lval(&c, &g, 0, I32));
  int load2 = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                            ssa_symref_lval(&c, &g, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, load2)), VR_TMP(0));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Global Load CSE: addend mismatch prevents CSE
 * ======================================================================== */

UT_TEST(test_gload_cse_addend_mismatch)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  static Sym g;
  ssa_init_sym(&g, 101, VT_STATIC);

  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), ssa_symref_lval(&c, &g, 0, I32));
  int load2 = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                            ssa_symref_lval(&c, &g, 4, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load2), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Global Store-to-Load Forwarding: immediate stored value forwarded
 * ======================================================================== */

UT_TEST(test_gstore_fwd_imm)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  static Sym g;
  ssa_init_sym(&g, 102, VT_STATIC);

  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 0, I32), utb_imm(7, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                             ssa_symref_lval(&c, &g, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, load_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, load_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Global Store-to-Load Forwarding: TEMP stored value forwarded
 * ======================================================================== */

UT_TEST(test_gstore_fwd_temp)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  static Sym g;
  ssa_init_sym(&g, 103, VT_STATIC);

  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 0, I32), utb_temp(1, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                             ssa_symref_lval(&c, &g, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, load_i)), VR_TMP(1));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Global Store-to-Load Forwarding: intervening global store to same sym
 * ======================================================================== */

UT_TEST(test_gstore_overwrites_gload_cse)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  static Sym g;
  ssa_init_sym(&g, 104, VT_STATIC);

  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), ssa_symref_lval(&c, &g, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 0, I32), utb_imm(7, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             ssa_symref_lval(&c, &g, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  /* Should forward from the store, not CSE to the first load. */
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, load_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, load_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Global Store-to-Load Forwarding: overlapping sub-word store invalidates
 * ======================================================================== */

UT_TEST(test_gstore_overlap_invalidates)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  static Sym g;
  ssa_init_sym(&g, 105, VT_STATIC);

  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 1, I8), utb_imm(7, I8));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                             ssa_symref_lval(&c, &g, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Global Store-to-Load Forwarding: sub-word store is not forwarded
 * ======================================================================== */

UT_TEST(test_subword_global_store_not_forwarded)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  static Sym g;
  ssa_init_sym(&g, 106, VT_STATIC);

  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 0, I8), utb_imm(7, I8));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                             ssa_symref_lval(&c, &g, 0, I8));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Global Load CSE: volatile global guard
 * ======================================================================== */

UT_TEST(test_gload_volatile_guard)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  static Sym g;
  ssa_init_sym(&g, 107, VT_STATIC | VT_VOLATILE);

  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), ssa_symref_lval(&c, &g, 0, I32));
  int load2 = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                            ssa_symref_lval(&c, &g, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load2), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Direct VAR assignment: must not poison unrelated global forwarding state
 * ======================================================================== */

UT_TEST(test_direct_var_assign_preserves_global_fwd)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 5);
  static Sym g;
  ssa_init_sym(&g, 108, VT_STATIC);

  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_temp(1, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                             ssa_symref_lval(&c, &g, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, load_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, load_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Unresolved pointer store: conservative kill-all for global/iload state
 * ======================================================================== */

UT_TEST(test_unresolved_store_kills_global_state)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 5);
  static Sym g;
  ssa_init_sym(&g, 109, VT_STATIC);

  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), ssa_symref_lval(&c, &g, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_imm(7, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                             ssa_symref_lval(&c, &g, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:load_cse");
