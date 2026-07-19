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
 *    - Extended section (end of file): vslot VAR value forwarding, ALU-operand
 *      deref forwarding, STORE-src forwarding, deref-load CSE canonical keys,
 *      iload byte-range/VAR/PARAM precision, window limits, redefinition
 *      kills, and two pinned bugs (is_lval store src tracked as stored value)
 *
 *  HARNESS NOTES:
 *    - Links the real source/opt/ssa/memory/load_cse.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 *    - Some tests need a real tcc_state for USING_GLOBALS; see setup_tcc_state.
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"
#include "load_cse.h"

#include "ut.h"

#define USING_GLOBALS
#include <limits.h>
#include "tcc.h"
#include "source/opt/ssa/include/ssa_opt.h"

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
  /* t0 = &StackLoc[0]; *t0 = #7; StackLoc[0] <- #8; t1 = *t0
   * Behavior note: this test predates temp-indir sstore tracking. `*t0 = #7`
   * resolves via LEA to slot 0 and is tracked as an sstore entry (not a
   * tvstore entry), so the direct stack store precisely overwrites the same
   * slot and the load forwards the NEWEST write #8 — semantically correct.
   * (The old expectation "no fwd" described the conservative tvstore-kill
   * era; the unresolved-pointer form of that kill is covered by
   * test_direct_stack_store_drops_tvstore below.) */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(8, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_lval(utb_temp(0, I32)));

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
 * T_vreg-deref tracking: narrow stores are not tracked (width-safe guard)
 * ======================================================================== */

UT_TEST(test_tvstore_narrow_type_not_tracked)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 5);
  /* t0 = &StackLoc[0]; *t0 = #7 (I8); t1 = *t0 (I8)
   * Behavior note: this test predates temp-indir sstore tracking. A narrow
   * store through an UNRESOLVED pointer is still rejected (width guard at
   * load_cse.c:1088, covered by test_tvstore_narrow_unresolved_not_tracked),
   * but a LEA-resolved narrow store is tracked precisely at its slot and a
   * same-width load forwards it — a byte store of #7 read back as a byte is
   * exactly #7. (The old expectation "no fwd" described the tvstore era,
   * where narrow stores were dropped wholesale.) */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I8)), utb_imm(7, I8));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I8),
                             utb_lval(utb_temp(0, I8)));

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
   * B2: load (else)
   * Note: the JUMPIF targets instr 5 (start of B2, matching "JUMPIF -> B2").
   * It originally read 6 (RETURNVOID), which stranded the else load in an
   * unreachable block — that only passed when the dom walk visited
   * unreachable blocks; current CFG code does not, so the target was
   * corrected to the documented intent. */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_temp(0, I32), UTB_NONE);
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
 * Extended coverage
 *
 * Paths not exercised by the tests above:
 *   - vslot VAR value forwarding (var_fwd): `V <- value` tracked and forwarded
 *     into V reads (vslot_track_store / vslot_forward_reads)
 *   - deref forwarding into ALU operands: global-deref, direct StackLoc, and
 *     LEA-resolved TEMP-deref forms (the non-LOAD/STORE side loop)
 *   - STORE-src forwarding: tracked stack store forwarded into a
 *     memory-to-memory STORE source
 *   - TEMP-deref load CSE: zero-hop and canonical (base, offset) keys
 *   - iload invalidation precision: byte-range overlap, VAR/PARAM bases
 *   - window limits (SSTORE_MAX) and redefinition kills (*_remove_vr family)
 *   - two pinned bugs: is_lval store sources tracked as stored values
 * ======================================================================== */

/* ========================================================================
 * vslot forwarding: `V <- #imm` forwarded into a later ALU read (src1 side)
 * ======================================================================== */

UT_TEST(test_vslot_fwd_imm_alu_src1)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 6);
  /* V0 = #7; t0 = V0 + #3 -> t0 = #7 + #3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(7, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32),
                             utb_var(0, I32), utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ADD);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, add_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, add_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * vslot forwarding: `V <- TEMP` forwarded into a later ALU read (src2 side)
 * ======================================================================== */

UT_TEST(test_vslot_fwd_temp_alu_src2)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 6);
  /* V0 = t5; t0 = #3 + V0 -> t0 = #3 + t5 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_temp(5, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32),
                             utb_imm(3, I32), utb_var(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_vreg(utb_src2(c.ir, add_i)), VR_TMP(5));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * vslot guard: a LEA-source TEMP is not tracked (downstream stack DSE guard)
 * ======================================================================== */

UT_TEST(test_vslot_lea_source_guard)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 6);
  /* t5 = &StackLoc[0]; V0 = t5; t0 = V0 + #3 -> no fwd */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(5, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_temp(5, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32),
                             utb_var(0, I32), utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, add_i)), VR_VAR(0));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * vslot ordering: a self-read sees the prior value; the non-ASSIGN write
 * then kills the slot, so later reads are not forwarded
 * ======================================================================== */

UT_TEST(test_vslot_self_read_then_killed)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 6);
  /* V0 = #7; V0 = V0 + #1; t0 = V0 + #3
   * -> first ADD reads the prior #7; its VAR dest write kills the slot,
   *    so the second ADD keeps the V0 read. */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(7, I32));
  int add1 = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_var(0, I32),
                            utb_var(0, I32), utb_imm(1, I32));
  int add2 = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32),
                            utb_var(0, I32), utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, add1)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, add1)), 7);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, add2)), VR_VAR(0));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * vslot tracking: STORE with a non-lval VAR dest is a register-form write
 * and seeds the same forwarding as ASSIGN
 * ======================================================================== */

UT_TEST(test_vslot_store_form_def)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 6);
  /* STORE V0 <- #7 (non-lval VAR dest = value copy); t0 = V0 + #3 -> #7 + #3 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_var(0, I32), utb_imm(7, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32),
                             utb_var(0, I32), utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, add_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, add_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * ALU-operand global deref: reuse an earlier load, then a tracked store
 * (the tracked store wins over the load entry)
 * ======================================================================== */

UT_TEST(test_alu_global_deref_fwd)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 6);
  static Sym g;
  ssa_init_sym(&g, 200, VT_STATIC);

  /* t1 = g; t2 = #3 + g -> t2 = #3 + t1 (gload reuse in an ALU operand) */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), ssa_symref_lval(&c, &g, 0, I32));
  int add1 = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                            utb_imm(3, I32), ssa_symref_lval(&c, &g, 0, I32));
  /* g = #7; t3 = #4 + g -> t3 = #4 + #7 (tracked store beats the load entry) */
  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 0, I32), utb_imm(7, I32));
  int add2 = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32),
                            utb_imm(4, I32), ssa_symref_lval(&c, &g, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 2);
  UT_ASSERT_EQ(utb_vreg(utb_src2(c.ir, add1)), VR_TMP(1));
  UT_ASSERT(irop_is_immediate(utb_src2(c.ir, add2)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src2(c.ir, add2)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * ALU-operand direct StackLoc deref: tracked stack store forwarded (I32)
 * ======================================================================== */

UT_TEST(test_alu_stack_deref_fwd_imm)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* StackLoc[0] <- #7; t0 = #3 + StackLoc[0] -> t0 = #3 + #7 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(7, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32),
                             utb_imm(3, I32), utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT(irop_is_immediate(utb_src2(c.ir, add_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src2(c.ir, add_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * ALU-operand TEMP deref resolving to a tracked stack slot: imm forwarded
 * ======================================================================== */

UT_TEST(test_alu_temp_deref_stack_imm_fwd)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 12);
  /* t9 = &StackLoc[0]; StackLoc[0] <- #7; t0 = #3 + *t9 -> t0 = #3 + #7 */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(9, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(7, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32),
                             utb_imm(3, I32), utb_lval(utb_temp(9, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT(irop_is_immediate(utb_src2(c.ir, add_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src2(c.ir, add_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * ALU-operand TEMP deref resolving to a tracked stack slot: a tracked TEMP
 * value is NOT forwarded here (imm only — a live TEMP would spill)
 * ======================================================================== */

UT_TEST(test_alu_temp_deref_stack_vr_not_fwd)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 12);
  /* t9 = &StackLoc[0]; StackLoc[0] <- t5; t0 = #3 + *t9 -> no fwd */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(9, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(5, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32),
                             utb_imm(3, I32), utb_lval(utb_temp(9, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src2(c.ir, add_i)), VR_TMP(9));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * STORE-src forwarding: a tracked stack store is forwarded into a
 * memory-to-memory STORE's source operand
 * ======================================================================== */

UT_TEST(test_store_src_stack_fwd)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* StackLoc[8] <- #7; StackLoc[0] <- StackLoc[8] -> StackLoc[0] <- #7 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(8, 1, 0, 0, I32), utb_imm(7, I32));
  int st_i = ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32),
                           utb_stackoff(8, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, st_i), TCCIR_OP_STORE);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, st_i)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, st_i)), 7);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * TEMP-deref load CSE: two loads through the same unresolved pointer
 * (zero-hop key) are deduplicated
 * ======================================================================== */

UT_TEST(test_deref_cse_same_ptr)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* t1 = *t0; t2 = *t0 -> t2 = t1 */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), utb_lval(utb_temp(0, I32)));
  int load2 = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                            utb_lval(utb_temp(0, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, load2)), VR_TMP(1));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * TEMP-deref load CSE: pointer canonicalized through an ADD chain to a
 * (base_vr, offset) key shared by both loads
 * ======================================================================== */

UT_TEST(test_deref_cse_canonical_add_offset)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 12);
  static Sym g;
  ssa_init_sym(&g, 201, VT_STATIC);

  /* t0 = g; t9 = t0 + #4; t1 = *t9; t2 = *t9 -> t2 = t1 (canon key (t0,+4)) */
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), ssa_symref_lval(&c, &g, 0, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(9, I32), utb_temp(0, I32), utb_imm(4, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), utb_lval(utb_temp(9, I32)));
  int load2 = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                            utb_lval(utb_temp(9, I32)));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, load2)), VR_TMP(1));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * iload precision: a stack-resolved store through the same base whose byte
 * range does NOT overlap the tracked indexed load leaves it CSE-able
 * ======================================================================== */

UT_TEST(test_iload_nonoverlap_store_same_base_survives)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 12);
  /* t9 = &StackLoc[0]; t0 = *(t9 + #4<<0); *t9 = #7; t1 = *(t9 + #4<<0)
   * -> store range [0,4) vs load range [4,8): no overlap, t1 = t0 */
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(9, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32),
                 utb_temp(9, I32), utb_imm(4, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(9, I32)), utb_imm(7, I32));
  int load_i = ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32),
                              utb_temp(9, I32), utb_imm(4, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, load_i)), VR_TMP(0));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * iload invalidation: a direct stack store kills entries whose VAR base may
 * point into the frame
 * ======================================================================== */

UT_TEST(test_iload_var_base_killed_by_direct_stack_store)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  /* t0 = *(V0 + #0<<0); StackLoc[0] <- #7; t1 = *(V0 + #0<<0) -> no CSE */
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32),
                 utb_var(0, I32), utb_imm(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(7, I32));
  int load_i = ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32),
                              utb_var(0, I32), utb_imm(0, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_LOAD_INDEXED);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * iload precision: a PARAM base is a caller pointer — a local stack store
 * cannot reach it, so the tracked load survives
 * ======================================================================== */

UT_TEST(test_iload_param_base_survives_direct_stack_store)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  c.ir->next_parameter = 1; /* P0 exists: lcse_base_stable admits a 0-def PARAM */
  /* t0 = *(P0 + #0<<0); StackLoc[0] <- #7; t1 = *(P0 + #0<<0) -> t1 = t0 */
  ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(0, I32),
                 utb_param(0, I32), utb_imm(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(7, I32));
  int load_i = ssa_add_instr4(&c, TCCIR_OP_LOAD_INDEXED, utb_temp(1, I32),
                              utb_param(0, I32), utb_imm(0, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, load_i)), VR_TMP(0));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Redefinition kill: reassigning the tracked TEMP drops the sstore entry
 * (forwarding the stale vreg would be wrong at regalloc time)
 * ======================================================================== */

UT_TEST(test_temp_redef_kills_sstore_entry)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* StackLoc[0] <- t0; t0 = #5; t1 = LOAD(StackLoc[0]) -> no fwd */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
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
 * Invalidation: an indexed store to a global drops that sym's tracked
 * store entry
 * ======================================================================== */

UT_TEST(test_store_indexed_sym_drops_gstore)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 4);
  static Sym g;
  ssa_init_sym(&g, 202, VT_STATIC);

  /* g = #7; *(g + #0<<0) = #8; t0 = g -> no fwd */
  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 0, I32), utb_imm(7, I32));
  ssa_add_instr4(&c, TCCIR_OP_STORE_INDEXED, ssa_symref_lval(&c, &g, 0, I32),
                 utb_imm(8, I32), utb_imm(0, I32), utb_imm(0, I32));
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
 * Invalidation: a direct store to a global drops tvstore entries — an
 * unresolved tracked pointer may name that very global
 * ======================================================================== */

UT_TEST(test_sym_store_drops_tvstore)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 12);
  static Sym g;
  ssa_init_sym(&g, 203, VT_STATIC);

  /* t9 = t8 + #0 (unresolved ptr); *t9 = #7; g = #8; t1 = *t9 -> no fwd */
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(9, I32), utb_temp(8, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(9, I32)), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 0, I32), utb_imm(8, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_lval(utb_temp(9, I32)));

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
 * Invalidation: a direct stack store drops tvstore entries — an unresolved
 * tracked pointer may alias the frame (load_cse.c:1013)
 * ======================================================================== */

UT_TEST(test_direct_stack_store_drops_tvstore)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 12);
  /* t9 = t8 + #0 (unresolved ptr); *t9 = #7; StackLoc[0] <- #8; t1 = *t9 -> no fwd */
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(9, I32), utb_temp(8, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(9, I32)), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_imm(8, I32));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_lval(utb_temp(9, I32)));

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
 * tvstore width guard: a narrow (sub-word) store through an UNRESOLVED
 * pointer is not tracked (load_cse.c:1088 width_safe_tv)
 * ======================================================================== */

UT_TEST(test_tvstore_narrow_unresolved_not_tracked)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 12);
  /* t9 = t8 + #0 (unresolved ptr); *t9 = #7 (I8); t1 = *t9 (I8) -> no fwd */
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(9, I32), utb_temp(8, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(9, I8)), utb_imm(7, I8));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I8),
                             utb_lval(utb_temp(9, I8)));

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
 * Global store tracking: a stored TEMP defined by `ASSIGN #imm` is peeled
 * to the immediate so downstream folding can fire
 * ======================================================================== */

UT_TEST(test_gstore_assign_imm_peel)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new_full(1, 6);
  static Sym g;
  ssa_init_sym(&g, 204, VT_STATIC);

  /* t5 = #7; g = t5; t0 = g -> t0 = #7 (peeled imm, not t5) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, ssa_symref_lval(&c, &g, 0, I32), utb_temp(5, I32));
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
 * Window limit: only SSTORE_MAX (16) distinct stack slots are tracked;
 * the 17th store is silently dropped
 * ======================================================================== */

UT_TEST(test_sstore_window_limit)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* Slots 0..15 (offsets 0..60) fill the window; slot 16 (offset 64) is
   * dropped. A reload of slot 0 still forwards; a reload of slot 16 cannot. */
  for (int k = 0; k < 17; k++)
    ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(k * 4, 1, 0, 0, I32),
                  utb_imm(100 + k, I32));
  int load_first = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                                 utb_stackoff(0, 1, 0, 0, I32));
  int load_last = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                                utb_stackoff(64, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT_EQ(changed, 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_first), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, load_first)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, load_first)), 100);
  UT_ASSERT_EQ(utb_op(c.ir, load_last), TCCIR_OP_LOAD);

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Overlap precision: an adjacent-but-not-overlapping narrow store does not
 * invalidate the wider tracked slot
 * ======================================================================== */

UT_TEST(test_sstore_adjacent_narrow_store_survives)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* StackLoc[0] (I32) <- t0; StackLoc[4] (I8) <- #7; t1 = LOAD(StackLoc[0])
   * -> ranges [0,4) and [4,5) do not overlap: forward t0 */
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(4, 1, 0, 0, I8), utb_imm(7, I8));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, load_i)), VR_TMP(0));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Pinned bugs
 * ======================================================================== */

/* Regression lock for docs/bugs.md "load_cse: stack store of a pointer-deref
 * source tracks the pointer as the stored value".
 * `StackLoc[0] <- *t1` records t1 (the ADDRESS) in the sstore slot because
 * the tracker at load_cse.c:1031 lacks the !src.is_lval guard its siblings
 * have (load_cse.c:302, :1104, :1181). The later LOAD is then rewritten to
 * `ASSIGN t1` — the pointer, not the pointee. Correct behavior: do not track
 * (load stays a load). This pins the CURRENT (buggy) result; flip the
 * assertions once fixed. */
UT_TEST(test_stack_store_lval_vreg_src_tracked_bug)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* t1 = t0 + #0 (unresolvable pointer); StackLoc[0] <- *t1;
   * t2 = LOAD(StackLoc[0]) -> currently becomes t2 = t1 (the pointer) */
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_stackoff(0, 1, 0, 0, I32),
                utb_lval(utb_temp(1, I32)));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

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

/* Regression lock for the same root cause at the twin call site
 * load_cse.c:1076 (TEMP-indir stack store `*T <- val` resolving to
 * LEA(StackLoc[N])): an is_lval source is recorded as the stored value.
 * `t1 = &StackLoc[0]; *t1 = *t0; t2 = LOAD(StackLoc[0])` currently becomes
 * `t2 = t0` (the pointer). Correct behavior: do not track. Pins the CURRENT
 * (buggy) result; flip the assertions once fixed. */
UT_TEST(test_temp_indir_store_lval_src_tracked_bug)
{
  setup_tcc_state();
  ssa_ctx c = ssa_ctx_new(1, 4);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(1, I32), utb_stackoff(0, 0, 0, 0, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)),
                utb_lval(utb_temp(0, I32)));
  int load_i = ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(2, I32),
                             utb_stackoff(0, 1, 0, 0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_load_cse(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, load_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, load_i)), VR_TMP(0));

  ssa_ctx_free(&c);
  teardown_tcc_state();
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:load_cse");
