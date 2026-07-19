/*
 *  test_ssa_opt_dce_global.c - dead redundant GLOBAL store elimination
 *
 *  Covers dce_dead_global_stores() in source/opt/ssa/dce/dead_global_stores.c: block-local
 *  overwrite elimination for stores to global symbols (`STORE g <- a;
 *  STORE g <- b` with no intervening read of `g`).  This is the SSA-side
 *  companion to the legacy flat store_redundant global path.
 *
 *  Kept in a separate suite from test_ssa_opt_dce.c: these cases use no VARs
 *  so they exercise the global path without depending on live-interval setup.
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

#define I32 IROP_BTYPE_INT32
#define I8 IROP_BTYPE_INT8

/* Anonymous stack-location operand (vreg -1, is_local=1). */
static inline IROperand gsl_anon(int32_t off, int is_lval, int btype)
{
  return irop_make_stackoff(-1, off, is_lval, 0, 0, btype);
}

static int run_dce(IRSSAOptCtx *ctx)
{
  static TCCState tcc_state_storage;

  memset(&tcc_state_storage, 0, sizeof(tcc_state_storage));
  tcc_state_storage.optimize = 1;
  tcc_enter_state(&tcc_state_storage);
  int changed = ssa_opt_dce(ctx);
  tcc_exit_state(&tcc_state_storage);
  return changed;
}

/* ========================================================================
 * Dead overwritten GLOBAL store: second write to the same symbol kills first
 * ======================================================================== */

UT_TEST(test_global_overwrite_same_sym)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  static Sym g;
  g.v = 100;
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(1, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Intervening read of the SAME global keeps the older global store
 * ======================================================================== */

UT_TEST(test_global_overwrite_intervening_load_same)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  static Sym g;
  g.v = 100;
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_symref(c.ir, &g, 1, 0, 0, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  run_dce(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Intervening read of a DIFFERENT global does not block the elimination
 * ======================================================================== */

UT_TEST(test_global_overwrite_read_other_sym)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  static Sym g, g2;
  g.v = 100;
  g2.v = 101;
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32),
                utb_symref(c.ir, &g2, 1, 0, 0, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * A call between two global stores keeps the first (callee may read it)
 * ======================================================================== */

UT_TEST(test_global_overwrite_call_clears)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  static Sym g;
  g.v = 100;
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(1, I32));
  ssa_add_instr3(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE,
                 utb_temp(0, I32), utb_imm(0, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  run_dce(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * A store through an unresolved pointer may alias a global; keep the first
 * ======================================================================== */

UT_TEST(test_global_overwrite_unknown_ptr_alias)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  static Sym g;
  g.v = 100;
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)),
                utb_imm(9, I32));
  int store3 = ssa_add_instr(&c, TCCIR_OP_STORE,
                             utb_symref(c.ir, &g, 1, 0, 0, I32),
                             utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  run_dce(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, store3), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Plain-STORE dead-overwrite to an anon stack slot: second write kills first.
 *
 * Regression guard for the source-sweep bug where dce_dead_overwrite_stores
 * read src2 without checking has_src2 — for a plain STORE (has_src2=0) that
 * stale operand looked like an unresolved deref, flushed pending, and skipped
 * every plain-STORE overwrite (leaving redundant zero-init stores live).
 * ======================================================================== */

UT_TEST(test_stack_plain_store_overwrite_same_offset)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE, gsl_anon(0, 1, I32),
                             utb_imm(1, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE, gsl_anon(0, 1, I32),
                             utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), gsl_anon(0, 1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* Byte-wide zero-init overwritten by a byte const store (the char-array init
 * shape from ir/95_ternary_array): both must resolve at width 1. */
UT_TEST(test_stack_plain_store_overwrite_byte)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/1);
  int z0 = ssa_add_instr(&c, TCCIR_OP_STORE, gsl_anon(-6, 1, I8),
                         utb_imm(0, I8));
  int v0 = ssa_add_instr(&c, TCCIR_OP_STORE, gsl_anon(-6, 1, I8),
                         utb_imm(3, I8));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), gsl_anon(-6, 1, I8));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  int changed = run_dce(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, z0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, v0), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* An intervening LOAD of the same slot keeps the older plain STORE. */
UT_TEST(test_stack_plain_store_overwrite_intervening_load)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  int store1 = ssa_add_instr(&c, TCCIR_OP_STORE, gsl_anon(0, 1, I32),
                             utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(0, I32), gsl_anon(0, 1, I32));
  int store2 = ssa_add_instr(&c, TCCIR_OP_STORE, gsl_anon(0, 1, I32),
                             utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_LOAD, utb_temp(1, I32), gsl_anon(0, 1, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32),
                 utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);

  run_dce(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, store1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(c.ir, store2), TCCIR_OP_STORE);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:dce:global_store");
