/*
 *  test_ssa_opt_branch.c - branch optimization pass
 *
 *  Phase 3: Mid passes.
 *
 *  Covers:
 *    - ssa_opt_branch(): constant-folded CMP/TEST_ZERO branches
 *      * CMP #a, #b; JUMPIF cond  -> JUMP or NOP
 *      * CMP #a, #b; SETIF cond   -> ASSIGN #0/#1
 *      * TEST_ZERO #a; JUMPIF EQ/NE -> JUMP or NOP
 *      * TEST_ZERO #a; JUMPIF NE (not taken); SETIF -> ASSIGN
 *      * CMP of the same TEMP vreg (reflexive fold)
 *      * Backward VAR slot scan for immediate defs
 *      * ssa_drop_phi_edge when an outgoing CFG edge dies
 *      - ssa_branch_prune_unreachable_phis for reachable blocks with
 *        operands from unreachable predecessors
 *      - Reachability paths: JUMP, JUMPIF, RETURN, IJUMP, fall-through
 *
 *  HARNESS NOTES:
 *    - Links the real source/opt/ssa/cfg/branch.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 */

#include "ssa_build.h"
#include "opt/ssa/branch.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* ========================================================================
 * Fixture helpers
 * ======================================================================== */

static int emit_cmp_jumpif(ssa_ctx *c, IROperand src1, IROperand src2,
                           int tok, int target_idx)
{
  int cmp_i = ssa_add_instr3(c, TCCIR_OP_CMP, UTB_NONE, src1, src2);
  ssa_add_instr(c, TCCIR_OP_JUMPIF, utb_imm(target_idx, I32), utb_imm(tok, I32));
  return cmp_i;
}

static int emit_cmp_setif(ssa_ctx *c, IROperand src1, IROperand src2,
                          int tok, IROperand dest)
{
  int cmp_i = ssa_add_instr3(c, TCCIR_OP_CMP, UTB_NONE, src1, src2);
  ssa_add_instr(c, TCCIR_OP_SETIF, dest, utb_imm(tok, I32));
  return cmp_i;
}

static int emit_test_zero_jumpif(ssa_ctx *c, int32_t val, int tok,
                                 int target_idx)
{
  int tz_i = ssa_add_instr(c, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_imm(val, I32));
  ssa_add_instr(c, TCCIR_OP_JUMPIF, utb_imm(target_idx, I32), utb_imm(tok, I32));
  return tz_i;
}

static int emit_test_zero_setif(ssa_ctx *c, int32_t val, int jump_tok,
                                int target_idx, IROperand setif_dest,
                                int setif_tok)
{
  int tz_i = ssa_add_instr(c, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_imm(val, I32));
  ssa_add_instr(c, TCCIR_OP_JUMPIF, utb_imm(target_idx, I32),
                utb_imm(jump_tok, I32));
  ssa_add_instr(c, TCCIR_OP_SETIF, setif_dest, utb_imm(setif_tok, I32));
  return tz_i;
}

static void add_phi_with_preds(ssa_ctx *c, int block, int32_t dest_vreg,
                               const int32_t *ops, const int *preds, int n)
{
  IRPhiNode *phi = tcc_mallocz(sizeof(*phi));
  phi->dest_vreg = dest_vreg;
  phi->orig_vreg = dest_vreg;
  phi->num_operands = n;
  phi->cap_operands = n;
  phi->btype = I32;
  phi->operands = tcc_mallocz(n * sizeof(IRPhiOperand));
  for (int i = 0; i < n; i++) {
    phi->operands[i].vreg = ops[i];
    phi->operands[i].pred_block = preds[i];
  }
  phi->next = c->ssa->block_phis[block];
  c->ssa->block_phis[block] = phi;
}

/* ========================================================================
 * Immediate CMP + JUMPIF: table over every condition token
 * ======================================================================== */

typedef struct {
  int32_t a;
  int32_t b;
  int tok;
  int expect_taken;
} cmp_case_t;

static const cmp_case_t cmp_cases[] = {
  {1, 1, 0x94, 1},   /* EQ */
  {1, 2, 0x94, 0},
  {1, 2, 0x95, 1},   /* NE */
  {1, 1, 0x95, 0},
  {3, 5, 0x9c, 1},   /* LT */
  {5, 3, 0x9c, 0},
  {5, 5, 0x9d, 1},   /* GE */
  {3, 5, 0x9d, 0},
  {3, 5, 0x9e, 1},   /* LE */
  {5, 3, 0x9e, 0},
  {5, 3, 0x9f, 1},   /* GT */
  {3, 5, 0x9f, 0},
  {5, 10, 0x92, 1},  /* ULT */
  {10, 5, 0x92, 0},
  {10, 5, 0x93, 1},  /* UGE */
  {5, 10, 0x93, 0},
  {5, 10, 0x96, 1},  /* ULE */
  {10, 5, 0x96, 0},
  {10, 5, 0x97, 1},  /* UGT */
  {5, 10, 0x97, 0},
  {(int32_t)0xffffffffu, 5, 0x93, 1}, /* unsigned: -1 >= 5 */
  {5, (int32_t)0xffffffffu, 0x96, 1}, /* unsigned: 5 <= -1 */
};

UT_TEST(test_branch_cmp_imm_all_conds)
{
  unsigned n = sizeof(cmp_cases) / sizeof(cmp_cases[0]);
  for (unsigned ci = 0; ci < n; ci++) {
    const cmp_case_t *cc = &cmp_cases[ci];
    ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);

    /* 0: CMP; 1: JUMPIF target=3; 2: fallthrough; 3: target */
    emit_cmp_jumpif(&c, utb_imm(cc->a, I32), utb_imm(cc->b, I32), cc->tok,
                    /*target=*/3);
    ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32));
    ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(1, I32));

    ssa_ctx_build_cfg(&c);
    ssa_ctx_build_ssa_plain(&c);
    ssa_ctx_rebuild(&c);

    int changed = ssa_opt_branch(c.ctx);
    UT_ASSERT(changed >= 1);

    UT_ASSERT_EQ(utb_op(c.ir, 0), TCCIR_OP_NOP);
    if (cc->expect_taken) {
      UT_ASSERT_EQ(utb_op(c.ir, 1), TCCIR_OP_JUMP);
    } else {
      UT_ASSERT_EQ(utb_op(c.ir, 1), TCCIR_OP_NOP);
    }

    ssa_ctx_free(&c);
  }
  return 0;
}

/* ========================================================================
 * Reflexive CMP: CMP x, x
 * ======================================================================== */

UT_TEST(test_branch_cmp_same_vreg_eq)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(42, I32));
  emit_cmp_jumpif(&c, utb_temp(0, I32), utb_temp(0, I32), 0x94, 3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, 1), TCCIR_OP_NOP); /* CMP */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_JUMP); /* JUMPIF */

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_cmp_same_vreg_via_copy_chain)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/6);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32));
  /* CMP t2, t0 -> same root after chasing ASSIGN copies */
  emit_cmp_jumpif(&c, utb_temp(2, I32), utb_temp(0, I32), 0x95, 5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(4, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, 3), TCCIR_OP_NOP); /* CMP */
  UT_ASSERT_EQ(utb_op(c.ir, 4), TCCIR_OP_NOP); /* JUMPIF NE on equal values */

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * CMP followed by SETIF
 * ======================================================================== */

UT_TEST(test_branch_cmp_setif_true)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  emit_cmp_setif(&c, utb_imm(2, I32), utb_imm(5, I32), 0x9c, utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, 0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, 1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, 1).tag, IROP_TAG_IMM32);
  UT_ASSERT_EQ(utb_src1(c.ir, 1).u.imm32, 1);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_cmp_setif_false)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  emit_cmp_setif(&c, utb_imm(5, I32), utb_imm(2, I32), 0x9c, utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_src1(c.ir, 1).u.imm32, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Boolean-normalisation: CMP X,#0; SETIF NE where X is provably {0,1}
 * collapses to ASSIGN V = X.
 * ======================================================================== */

UT_TEST(test_branch_bool_norm_setif_rewritten)
{
  /* T0 is single-def SETIF (provably {0,1}); CMP T0,#0; SETIF NE -> V = T0. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(0x95, I32));
  int cmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32),
                           utb_imm(0, I32));
  int setif2 = ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(1, I32),
                             utb_imm(0x95, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, setif2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, setif2)),
               utb_vreg(utb_temp(0, I32)));

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_bool_norm_bool_and_rewritten)
{
  /* BOOL_AND is also accepted as a provably-{0,1} def. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr3(&c, TCCIR_OP_BOOL_AND, utb_temp(0, I32), utb_imm(1, I32),
                 utb_imm(0, I32));
  int cmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32),
                           utb_imm(0, I32));
  int setif2 = ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(1, I32),
                             utb_imm(0x95, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, setif2), TCCIR_OP_ASSIGN);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_bool_norm_wrong_cond_kept)
{
  /* SETIF cond must be NE (0x95); EQ leaves the pair untouched. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(0x95, I32));
  int cmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32),
                           utb_imm(0, I32));
  int setif2 = ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(1, I32),
                             utb_imm(0x94, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_branch(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(c.ir, setif2), TCCIR_OP_SETIF);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_bool_norm_nonzero_cmp_kept)
{
  /* CMP T0,#5 -> the `!= 0` reduction does not apply. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(0x95, I32));
  int cmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32),
                           utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(0x95, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_branch(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp), TCCIR_OP_CMP);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_bool_norm_non_bool_def_kept)
{
  /* T0 defined by ADD -> not provably {0,1} -> no rewrite. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32),
                 utb_imm(2, I32));
  int cmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32),
                           utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(0x95, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_branch(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp), TCCIR_OP_CMP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * TEST_ZERO folds
 * ======================================================================== */

UT_TEST(test_branch_test_zero_eq_taken)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/3);
  emit_test_zero_jumpif(&c, 0, 0x94, 3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, 0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, 1), TCCIR_OP_JUMP);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_test_zero_ne_taken)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/3);
  emit_test_zero_jumpif(&c, 42, 0x95, 3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, 0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, 1), TCCIR_OP_JUMP);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_test_zero_fallthrough_setif)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);
  /* TEST_ZERO #0; JUMPIF NE (not taken); SETIF EQ -> true */
  emit_test_zero_setif(&c, 0, 0x95, 4, utb_temp(3, I32), 0x94);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, 0), TCCIR_OP_NOP); /* TEST_ZERO */
  UT_ASSERT_EQ(utb_op(c.ir, 1), TCCIR_OP_NOP); /* JUMPIF */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_ASSIGN); /* SETIF */
  UT_ASSERT_EQ(utb_src1(c.ir, 2).u.imm32, 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Negative / no-fold cases
 * ======================================================================== */

/* CMP of two single-def constant TEMPs: the folder chases ASSIGN #const,
 * so non-immediate operands that hold a known constant still fold. */
UT_TEST(test_branch_cmp_nonimm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp_jumpif(&c, utb_temp(0, I32), utb_temp(1, I32), 0x94, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_NOP); /* CMP folded */
  UT_ASSERT_EQ(utb_op(c.ir, 3), TCCIR_OP_NOP); /* JUMPIF: EQ(1,2) false */

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_cmp_unknown_token)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  emit_cmp_jumpif(&c, utb_imm(1, I32), utb_imm(1, I32), 0xAB, 3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_branch(c.ctx), 0);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_cmp_not_followed)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_imm(1, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_branch(c.ctx), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* TEST_ZERO of a single-def constant TEMP (ASSIGN #0): the folder resolves
 * the temp, so it folds just like a raw immediate. */
UT_TEST(test_branch_test_zero_nonimm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  int tz_i = ssa_add_instr(&c, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(0x94, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, tz_i), TCCIR_OP_NOP);     /* TEST_ZERO folded */
  UT_ASSERT_EQ(utb_op(c.ir, tz_i + 1), TCCIR_OP_JUMP); /* EQ: 0==0 taken */

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_test_zero_bad_cond)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  emit_test_zero_jumpif(&c, 0, 0x9c, 3); /* LT is not handled by TEST_ZERO */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_branch(c.ctx), 0);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_cmp_setif_unknown_token)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  emit_cmp_setif(&c, utb_imm(1, I32), utb_imm(1, I32), 0xAB, utb_temp(2, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_branch(c.ctx), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * VAR backward scan
 * ======================================================================== */

UT_TEST(test_branch_cmp_var_immediate_assign)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(7, I32));
  emit_cmp_jumpif(&c, utb_var(0, I32), utb_imm(7, I32), 0x94, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, 1), TCCIR_OP_NOP); /* CMP */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_JUMP); /* JUMPIF */

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_cmp_var_funccall_barrier)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE);
  emit_cmp_jumpif(&c, utb_var(0, I32), utb_imm(7, I32), 0x94, 5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_opt_branch(c.ctx), 0);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_cmp_var_store_immediate)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* STORE #7 -> V0 slot (is_local=1, is_lval=1) */
  IROperand vslot = utb_var(0, I32);
  vslot.is_lval = 1;
  vslot.is_local = 1;
  ssa_add_instr3(&c, TCCIR_OP_STORE, vslot, utb_imm(7, I32), UTB_NONE);
  emit_cmp_jumpif(&c, utb_var(0, I32), utb_imm(7, I32), 0x94, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, 1), TCCIR_OP_NOP);   /* CMP folded via STORE scan */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_JUMP);  /* JUMPIF: EQ(7,7) taken */

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_cmp_var_deref_store_skipped)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(7, I32));
  /* STORE through V0's pointer value: does not write the slot itself */
  ssa_add_instr3(&c, TCCIR_OP_STORE, utb_lval(utb_var(0, I32)),
                 utb_imm(99, I32), UTB_NONE);
  emit_cmp_jumpif(&c, utb_var(0, I32), utb_imm(7, I32), 0x94, 5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_NOP);  /* CMP folded (deref STORE skipped) */
  UT_ASSERT_EQ(utb_op(c.ir, 3), TCCIR_OP_JUMP); /* JUMPIF: EQ(7,7) taken */

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Phi-edge cleanup when a branch edge dies
 * ======================================================================== */

UT_TEST(test_branch_drop_phi_edge_taken)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/8);
  ssa_ctx_init_manual(&c);

  /* block 0: CMP #1,#1; JUMPIF EQ target=4 */
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_imm(1, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(0x94, I32));
  /* block 1: fallthrough [2,4) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(4, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(11, I32));
  /* block 2: target [4,6) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(6, I32), utb_imm(20, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32), utb_imm(21, I32));

  ssa_ctx_manual_block_range(&c, 0, 0, 2);
  ssa_ctx_manual_block_range(&c, 1, 2, 4);
  ssa_ctx_manual_block_range(&c, 2, 4, 6);

  int32_t ops[] = { utb_vreg(utb_temp(0, I32)) };
  int preds[] = { 0 };
  add_phi_with_preds(&c, 1, utb_vreg(utb_temp(1, I32)), ops, preds, 1);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);

  IRPhiNode *phi = c.ssa->block_phis[1];
  UT_ASSERT(phi != NULL);
  UT_ASSERT_EQ(phi->num_operands, 0);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_drop_phi_edge_not_taken)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/8);
  ssa_ctx_init_manual(&c);

  /* block 0: CMP #1,#2; JUMPIF EQ target=4 (false) */
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_imm(1, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(0x94, I32));
  /* block 1: fallthrough [2,4) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(4, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(11, I32));
  /* block 2: target [4,6) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(6, I32), utb_imm(20, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32), utb_imm(21, I32));

  ssa_ctx_manual_block_range(&c, 0, 0, 2);
  ssa_ctx_manual_block_range(&c, 1, 2, 4);
  ssa_ctx_manual_block_range(&c, 2, 4, 6);

  int32_t ops[] = { utb_vreg(utb_temp(0, I32)) };
  int preds[] = { 0 };
  add_phi_with_preds(&c, 2, utb_vreg(utb_temp(1, I32)), ops, preds, 1);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);

  IRPhiNode *phi = c.ssa->block_phis[2];
  UT_ASSERT(phi != NULL);
  UT_ASSERT_EQ(phi->num_operands, 0);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_drop_phi_edge_multi_operand)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/8);
  ssa_ctx_init_manual(&c);

  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_imm(1, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_imm(0x94, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(4, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(11, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(6, I32), utb_imm(20, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32), utb_imm(21, I32));

  ssa_ctx_manual_block_range(&c, 0, 0, 2);
  ssa_ctx_manual_block_range(&c, 1, 2, 4);
  ssa_ctx_manual_block_range(&c, 2, 4, 6);

  int32_t ops[] = {
    utb_vreg(utb_temp(0, I32)),
    utb_vreg(utb_temp(1, I32)),
    utb_vreg(utb_temp(2, I32))
  };
  int preds[] = { 0, 2, 0 };
  add_phi_with_preds(&c, 1, utb_vreg(utb_temp(3, I32)), ops, preds, 3);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);

  IRPhiNode *phi = c.ssa->block_phis[1];
  UT_ASSERT(phi != NULL);
  UT_ASSERT_EQ(phi->num_operands, 1);
  UT_ASSERT_EQ(phi->operands[0].vreg, utb_vreg(utb_temp(1, I32)));
  UT_ASSERT_EQ(phi->operands[0].pred_block, 2);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Prune unreachable phi operands across transitively-dead predecessors
 * ======================================================================== */

UT_TEST(test_branch_prune_unreachable_phi)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/8);
  ssa_ctx_init_manual(&c);

  /* block 0: JUMPIF to block 3, fall-through block 1 becomes unreachable */
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_imm(1, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(0x94, I32));
  /* block 1: unreachable [2,4) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(4, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(5, I32), utb_imm(11, I32));
  /* block 2: also unreachable [4,6) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(6, I32), utb_imm(20, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32), utb_imm(21, I32));
  /* block 3: reachable target [6,8) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(30, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(31, I32));

  ssa_ctx_manual_block_range(&c, 0, 0, 2);
  ssa_ctx_manual_block_range(&c, 1, 2, 4);
  ssa_ctx_manual_block_range(&c, 2, 4, 6);
  ssa_ctx_manual_block_range(&c, 3, 6, 8);

  int32_t ops[] = {
    utb_vreg(utb_temp(2, I32)),
    utb_vreg(utb_temp(3, I32))
  };
  int preds[] = { 0, 1 };
  add_phi_with_preds(&c, 3, utb_vreg(utb_temp(2, I32)), ops, preds, 2);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 1);

  IRPhiNode *phi = c.ssa->block_phis[3];
  UT_ASSERT(phi != NULL);
  UT_ASSERT_EQ(phi->num_operands, 1);
  UT_ASSERT_EQ(phi->operands[0].pred_block, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reachability paths exercised through ssa_branch_prune_unreachable_phis
 * ======================================================================== */

UT_TEST(test_branch_reachable_return_terminator)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);
  ssa_ctx_init_manual(&c);

  /* block 0: RETURNVOID */
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  /* block 1: unreachable, phi from block 0 (will be skipped because block1 is unreachable) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(1, I32));

  ssa_ctx_manual_block_range(&c, 0, 0, 1);
  ssa_ctx_manual_block_range(&c, 1, 1, 3);

  int32_t ops[] = { utb_vreg(utb_temp(1, I32)) };
  int preds[] = { 0 };
  add_phi_with_preds(&c, 1, utb_vreg(utb_temp(2, I32)), ops, preds, 1);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_branch_reachable_ijump)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);
  ssa_ctx_init_manual(&c);

  /* block 0: IJUMP - conservative reachability uses CFG succs (none here) */
  ssa_add_instr(&c, TCCIR_OP_IJUMP, UTB_NONE, utb_temp(0, I32));
  /* block 1: unreachable */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(1, I32));

  ssa_ctx_manual_block_range(&c, 0, 0, 1);
  ssa_ctx_manual_block_range(&c, 1, 1, 3);

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_branch(c.ctx);
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Idempotence
 * ======================================================================== */

UT_TEST(test_branch_idempotent)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);
  emit_cmp_jumpif(&c, utb_imm(1, I32), utb_imm(1, I32), 0x94, 3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int c1 = ssa_opt_branch(c.ctx);
  UT_ASSERT(c1 >= 1);
  int c2 = ssa_opt_branch(c.ctx);
  UT_ASSERT_EQ(c2, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_branch)
{
  UT_COVERS("ssa:branch");
  UT_RUN(test_branch_cmp_imm_all_conds);
  UT_RUN(test_branch_cmp_same_vreg_eq);
  UT_RUN(test_branch_cmp_same_vreg_via_copy_chain);
  UT_RUN(test_branch_cmp_setif_true);
  UT_RUN(test_branch_cmp_setif_false);
  UT_RUN(test_branch_bool_norm_setif_rewritten);
  UT_RUN(test_branch_bool_norm_bool_and_rewritten);
  UT_RUN(test_branch_bool_norm_wrong_cond_kept);
  UT_RUN(test_branch_bool_norm_nonzero_cmp_kept);
  UT_RUN(test_branch_bool_norm_non_bool_def_kept);
  UT_RUN(test_branch_test_zero_eq_taken);
  UT_RUN(test_branch_test_zero_ne_taken);
  UT_RUN(test_branch_test_zero_fallthrough_setif);
  UT_RUN(test_branch_cmp_nonimm);
  UT_RUN(test_branch_cmp_unknown_token);
  UT_RUN(test_branch_cmp_not_followed);
  UT_RUN(test_branch_test_zero_nonimm);
  UT_RUN(test_branch_test_zero_bad_cond);
  UT_RUN(test_branch_cmp_setif_unknown_token);
  UT_RUN(test_branch_cmp_var_immediate_assign);
  UT_RUN(test_branch_cmp_var_funccall_barrier);
  UT_RUN(test_branch_cmp_var_store_immediate);
  UT_RUN(test_branch_cmp_var_deref_store_skipped);
  UT_RUN(test_branch_drop_phi_edge_taken);
  UT_RUN(test_branch_drop_phi_edge_not_taken);
  UT_RUN(test_branch_drop_phi_edge_multi_operand);
  UT_RUN(test_branch_prune_unreachable_phi);
  UT_RUN(test_branch_reachable_return_terminator);
  UT_RUN(test_branch_reachable_ijump);
  UT_RUN(test_branch_idempotent);
}
