/*
 *  test_ssa_opt_gvn.c - CMP+SETIF CSE folded into ssa_opt_gvn
 *
 *  Ports the former test_opt_cmp_cse.c (which drove the retired flat
 *  tcc_ir_opt_cmp_setif_cse) onto the GVN hook gvn_try_cmp_setif:
 *
 *      CMP A, B            CMP A, B   (equal operands + cond + btypes)
 *      V1 <-- (cond=C)  => NOP
 *      ...                 V2 <-- V1  [ASSIGN]
 *      CMP A, B
 *      V2 <-- (cond=C)
 *
 *  GVN routes each pair by operand class: both operands GVN-stable
 *  (immediates / single-def const vregs) -> dominator-scoped table, giving
 *  cross-block reuse the flat pass never had; a memory/multi-def operand ->
 *  block-local cache, inheriting gvn_local_invalidate's store/redef bailouts.
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_gvn.c via UT11.
 *    - Uses ssa_build.h for hand-built CFG + SSA + vinfo.
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"
#include "opt/ssa/gvn.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "source/opt/ssa/include/ssa_opt.h"

#define I32 IROP_BTYPE_INT32
#define I16 IROP_BTYPE_INT16

#define UT_TOK_NE 0x95
#define UT_TOK_EQ 0x94

#define VR_TEMP(n) irop_get_vreg(utb_temp(n, I32))

#define gvn_cmp(c, s1, s2) ssa_add_instr3((c), TCCIR_OP_CMP, UTB_NONE, (s1), (s2))
#define gvn_setif(c, d, cond) \
  ssa_add_instr3((c), TCCIR_OP_SETIF, (d), utb_imm((cond), I32), UTB_NONE)

static void gvn_build_rebuild(ssa_ctx *c)
{
  ssa_ctx_build_cfg(c);
  /* GVN walks the dominator tree via dom_children, populated here (the real
   * pipeline gets this from SSA construction; the plain harness does not). */
  if (c->cfg)
    tcc_ir_cfg_compute_dom_frontiers(c->cfg);
  ssa_ctx_build_ssa_plain(c);
  ssa_ctx_rebuild(c);
}

/* -------------------------------------------------- positive paths */

UT_TEST(test_gvn_cmp_setif_two_imm_pairs_fold)
{
  /* Two CMP #5,#7 ; SETIF NE pairs (immediate operands -> dominator table).
   * The second CMP is NOPed and its SETIF becomes ASSIGN of the first result. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/10);
  gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));                 /* 0 */
  gvn_setif(&c, utb_temp(1, I32), UT_TOK_NE);                    /* 1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(9, I32), utb_imm(1, I32)); /* 2 benign */
  int cmp2 = gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));      /* 3 */
  int setif2 = gvn_setif(&c, utb_temp(2, I32), UT_TOK_NE);       /* 4 */

  gvn_build_rebuild(&c);
  int changes = ssa_opt_gvn(c.ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, setif2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, setif2)), VR_TEMP(1));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_gvn_cmp_setif_vreg_const_pairs_fold)
{
  /* CMP operands are the same single-def const vreg (T0 = #5) -> both pairs key
   * identically in the dominator table. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/10);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32)); /* 0 */
  gvn_cmp(&c, utb_temp(0, I32), utb_imm(7, I32));                /* 1 */
  gvn_setif(&c, utb_temp(1, I32), UT_TOK_NE);                    /* 2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(9, I32), utb_imm(1, I32)); /* 3 benign */
  int cmp2 = gvn_cmp(&c, utb_temp(0, I32), utb_imm(7, I32));     /* 4 */
  int setif2 = gvn_setif(&c, utb_temp(2, I32), UT_TOK_NE);       /* 5 */

  gvn_build_rebuild(&c);
  int changes = ssa_opt_gvn(c.ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, setif2), TCCIR_OP_ASSIGN);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_gvn_cmp_setif_cross_block_fold)
{
  /* Dominator-scoped reuse: pair in block 0 dominates a duplicate pair in
   * block 1 (reached by an unconditional JUMP).  The flat pass, scoped to one
   * basic block, could never fold this. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/10);
  gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));                 /* 0 */
  gvn_setif(&c, utb_temp(1, I32), UT_TOK_NE);                    /* 1 */
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE);   /* 2 -> idx 3 */
  int cmp2 = gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));      /* 3 (block 1) */
  int setif2 = gvn_setif(&c, utb_temp(2, I32), UT_TOK_NE);       /* 4 */

  gvn_build_rebuild(&c);
  UT_ASSERT(c.cfg->num_blocks >= 2);
  int changes = ssa_opt_gvn(c.ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, setif2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, setif2)), VR_TEMP(1));
  ssa_ctx_free(&c);
  return 0;
}

/* -------------------------------------------------- guard branches */

UT_TEST(test_gvn_cmp_setif_cond_mismatch_no_fold)
{
  /* First pair NE, second pair EQ -> cond differs (imm3 key) -> no fold. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/10);
  gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));
  gvn_setif(&c, utb_temp(1, I32), UT_TOK_NE);
  int cmp2 = gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));
  gvn_setif(&c, utb_temp(2, I32), UT_TOK_EQ);

  gvn_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_gvn(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp2), TCCIR_OP_CMP);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_gvn_cmp_setif_btype_mismatch_no_fold)
{
  /* Operand btype differs between the two CMPs (I32 vs I16) -> btkey differs. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/10);
  gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));
  gvn_setif(&c, utb_temp(1, I32), UT_TOK_NE);
  int cmp2 = gvn_cmp(&c, utb_imm(5, I16), utb_imm(7, I16));
  gvn_setif(&c, utb_temp(2, I32), UT_TOK_NE);

  gvn_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_gvn(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp2), TCCIR_OP_CMP);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_gvn_cmp_setif_operand_mismatch_no_fold)
{
  /* Same cond/btypes but different compared value (7 vs 9) -> operands unequal. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/10);
  gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));
  gvn_setif(&c, utb_temp(1, I32), UT_TOK_NE);
  int cmp2 = gvn_cmp(&c, utb_imm(5, I32), utb_imm(9, I32));
  gvn_setif(&c, utb_temp(2, I32), UT_TOK_NE);

  gvn_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_gvn(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp2), TCCIR_OP_CMP);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_gvn_cmp_setif_operand_redef_no_fold)
{
  /* A multi-def CMP operand (T0) routes the pair to the block-local cache; the
   * intervening redefinition of T0 invalidates the cached entry -> no fold. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/10);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32)); /* 0 */
  gvn_cmp(&c, utb_temp(0, I32), utb_imm(7, I32));               /* 1 */
  gvn_setif(&c, utb_temp(1, I32), UT_TOK_NE);                   /* 2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(9, I32)); /* 3 redef */
  int cmp2 = gvn_cmp(&c, utb_temp(0, I32), utb_imm(7, I32));    /* 4 */
  gvn_setif(&c, utb_temp(2, I32), UT_TOK_NE);                   /* 5 */

  gvn_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_gvn(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp2), TCCIR_OP_CMP);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_gvn_cmp_setif_lval_dest_skipped)
{
  /* A SETIF whose dest is an lvalue is not a normal value result -> not
   * recorded as a CSE anchor. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/10);
  gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));
  gvn_setif(&c, utb_lval(utb_temp(1, I32)), UT_TOK_NE);
  int cmp2 = gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));
  gvn_setif(&c, utb_temp(2, I32), UT_TOK_NE);

  gvn_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_gvn(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp2), TCCIR_OP_CMP);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_gvn_cmp_setif_single_pair_no_fold)
{
  /* A lone CMP+SETIF with no duplicate records but folds nothing (no crash). */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/10);
  int cmp = gvn_cmp(&c, utb_imm(5, I32), utb_imm(7, I32));
  gvn_setif(&c, utb_temp(1, I32), UT_TOK_NE);

  gvn_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_gvn(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp), TCCIR_OP_CMP);
  ssa_ctx_free(&c);
  return 0;
}

UT_COVERS("gvn");
