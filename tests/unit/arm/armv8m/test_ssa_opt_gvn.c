/*
 *  test_ssa_opt_gvn.c - global value numbering pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_gvn(): detecting redundant computations
 *      * x = a + b; y = a + b -> y = x (GVN)
 *      * x = a + b; y = b + a -> y = x (commutative GVN)
 *      * Different operands -> no redundancy
 *      * Different immediates -> no redundancy
 *      * MLA with accumulator redundancy
 *      * 64-bit destination decline
 *      * Barrel-shift annotation decline
 *      * Multi-definition source decline
 *      * Phi-defined source decline
 *      * lval/local/llocal source decline
 *      * Stable vs mutated PARAM handling
 *      * Dominator-tree scoped availability
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_gvn.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* ========================================================================
 * Redundant computation: x = a + b; y = a + b -> y = x
 * ======================================================================== */

UT_TEST(test_gvn_redundant_add)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = #2; t2 = t0 + t1; t3 = t0 + t1 -> t3 = t2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32),
                 utb_temp(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32),
                              utb_temp(0, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  /* The pass should detect the redundant ADD. */
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, add2_i)),
               IROP_VR(utb_temp(2, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Commutative: x = a + b; y = b + a -> y = x
 * ======================================================================== */

UT_TEST(test_gvn_commutative_add)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32),
                 utb_temp(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32),
                              utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, add2_i)),
               IROP_VR(utb_temp(2, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Non-redundant: different operands -> no optimization
 * ======================================================================== */

UT_TEST(test_gvn_non_redundant)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #1; t1 = #2; t2 = t0 + t1; t3 = t0 + #3 -> no redundancy */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32),
                 utb_temp(1, I32));
  int add_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32),
                             utb_temp(0, I32), utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  /* The pass should not optimize (operands differ). */
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Immediate-key distinction: same vreg, different immediates -> no merge
 * ======================================================================== */

UT_TEST(test_gvn_imm_key_distinct)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(5, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                              utb_temp(0, I32), utb_imm(6, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MLA redundancy: dest = a*b + c
 * ======================================================================== */

UT_TEST(test_gvn_mla_redundant)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(4, I32));
  ssa_add_instr4(&c, TCCIR_OP_MLA, utb_temp(3, I32), utb_temp(0, I32),
                 utb_temp(1, I32), utb_temp(2, I32));
  int mla2_i = ssa_add_instr4(&c, TCCIR_OP_MLA, utb_temp(4, I32),
                              utb_temp(0, I32), utb_temp(1, I32),
                              utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, mla2_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, mla2_i)),
               IROP_VR(utb_temp(3, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * MLA commutativity: a*b + c matches b*a + c
 * ======================================================================== */

UT_TEST(test_gvn_mla_commutative)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(4, I32));
  ssa_add_instr4(&c, TCCIR_OP_MLA, utb_temp(3, I32), utb_temp(1, I32),
                 utb_temp(0, I32), utb_temp(2, I32));
  int mla2_i = ssa_add_instr4(&c, TCCIR_OP_MLA, utb_temp(4, I32),
                              utb_temp(0, I32), utb_temp(1, I32),
                              utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, mla2_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, mla2_i)),
               IROP_VR(utb_temp(3, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * 64-bit destination: decline to avoid truncating copy
 * ======================================================================== */

UT_TEST(test_gvn_i64_dest_skip)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I64), utb_temp(0, I32),
                 utb_temp(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I64),
                              utb_temp(0, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Barrel-shift annotation: invisible to the GVN key -> decline
 * ======================================================================== */

UT_TEST(test_gvn_barrel_shift_skip)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  int add1_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                              utb_temp(0, I32), utb_temp(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32),
                              utb_temp(0, I32), utb_temp(1, I32));

  /* Annotate the first ADD with a barrel shift; it must not enter the table. */
  c.ir->barrel_shifts = tcc_mallocz(c.ir->max_orig_index + 1);
  c.ir->barrel_shifts[add1_i] = 1;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Multi-definition source: value is not unique -> decline
 * ======================================================================== */

UT_TEST(test_gvn_multi_def_src_skip)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32),
                 utb_temp(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32),
                              utb_temp(0, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phi-defined source: value merges across blocks -> decline
 * ======================================================================== */

UT_TEST(test_gvn_phi_src_skip)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  /* phi T0 = [T1, T1] in block 0. */
  ssa_add_phi(&c, /*block=*/0, utb_vreg(utb_temp(0, I32)),
              (int32_t[]){ utb_vreg(utb_temp(1, I32)),
                           utb_vreg(utb_temp(1, I32)) }, 2);

  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32),
                 utb_temp(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32),
                              utb_temp(0, I32), utb_temp(1, I32));

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * lval/local/llocal source flags: not a register value -> decline
 * ======================================================================== */

UT_TEST(test_gvn_lval_src_skip)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  IROperand t0_lval = utb_temp(0, I32);
  t0_lval.is_lval = 1;
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), t0_lval,
                 utb_temp(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32), t0_lval,
                              utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_gvn_local_src_skip)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  IROperand t0_local = utb_temp(0, I32);
  t0_local.is_local = 1;
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), t0_local,
                 utb_temp(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32), t0_local,
                              utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_gvn_llocal_src_skip)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  IROperand t0_llocal = utb_temp(0, I32);
  t0_llocal.is_llocal = 1;
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), t0_llocal,
                 utb_temp(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32), t0_llocal,
                              utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Stable PARAM source: can hash on a parameter that is never written
 * ======================================================================== */

UT_TEST(test_gvn_param_stable)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  c.ir->next_parameter = 1;
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_param(0, I32),
                 utb_imm(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                              utb_param(0, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, add2_i)),
               IROP_VR(utb_temp(1, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Mutated PARAM source: mid-function write makes it unstable -> decline
 * ======================================================================== */

UT_TEST(test_gvn_param_mutated_skip)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  c.ir->next_parameter = 1;
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_param(0, I32), utb_imm(5, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_param(0, I32),
                 utb_imm(1, I32));
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                              utb_param(0, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Dominator-tree scope: hash entries flow to dominated blocks and are
 * popped after the subtree.
 * ======================================================================== */

UT_TEST(test_gvn_domtree_scope)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32),
                 utb_temp(1, I32));
  /* Jump to the instruction immediately following it -> block 1 starts here. */
  utb_emit(c.ir, TCCIR_OP_JUMP, utb_imm(4, I32), UTB_NONE, UTB_NONE);
  int add2_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(3, I32),
                              utb_temp(0, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  /* GVN needs the dominator-tree children, not just idom. */
  tcc_ir_cfg_compute_dom_frontiers(c.cfg);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_gvn(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, add2_i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, add2_i)),
               IROP_VR(utb_temp(2, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_gvn)
{
  UT_COVERS("ssa:gvn");
  UT_RUN(test_gvn_redundant_add);
  UT_RUN(test_gvn_commutative_add);
  UT_RUN(test_gvn_non_redundant);
  UT_RUN(test_gvn_imm_key_distinct);
  UT_RUN(test_gvn_mla_redundant);
  UT_RUN(test_gvn_mla_commutative);
  UT_RUN(test_gvn_i64_dest_skip);
  UT_RUN(test_gvn_barrel_shift_skip);
  UT_RUN(test_gvn_multi_def_src_skip);
  UT_RUN(test_gvn_phi_src_skip);
  UT_RUN(test_gvn_lval_src_skip);
  UT_RUN(test_gvn_local_src_skip);
  UT_RUN(test_gvn_llocal_src_skip);
  UT_RUN(test_gvn_param_stable);
  UT_RUN(test_gvn_param_mutated_skip);
  UT_RUN(test_gvn_domtree_scope);
}
