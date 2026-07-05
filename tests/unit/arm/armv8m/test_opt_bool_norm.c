/*
 *  test_opt_bool_norm.c - suite for ir/opt.c :: tcc_ir_opt_bool_norm_elim
 *
 *  Drops the redundant `!!bool` idiom the frontend emits when a comparison
 *  result is stored into a _Bool:
 *
 *      CMP X, #0        X is a vreg already in {0,1}
 *      V <-- (cond=NE)  [SETIF]      ==>    V <-- X   [ASSIGN]
 *
 *  Guarded rewrite: cond must be exactly TOK_NE, the second CMP operand must be
 *  immediate 0, the first must be a plain (non-lval, non-sym) vreg whose single
 *  defining instruction is SETIF / BOOL_AND / BOOL_OR (so it is provably 0/1).
 *  Each guard branch gets a dedicated negative test.
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_opt_bool_norm_elim(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* TOK_NE / TOK_EQ condition-code immediates carried in the SETIF src1. */
#define UT_TOK_NE 0x95
#define UT_TOK_EQ 0x94

/* -------------------------------------------------- positive path */

UT_TEST(test_bool_norm_ne_of_bool_setif_rewritten)
{
  /* T0 is single-def SETIF (so provably {0,1}); `CMP T0,#0; SETIF NE` becomes
   * `ASSIGN V = T0`. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE); /* 0: def T0 */
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int setif2 = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  int changes = tcc_ir_opt_bool_norm_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, setif2), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, setif2)),
               irop_get_vreg(utb_temp(0, I32)));
  utb_free(ir);
  return 0;
}

/* -------------------------------------------------- guard branches */

UT_TEST(test_bool_norm_too_short)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  UT_ASSERT_EQ(tcc_ir_opt_bool_norm_elim(ir), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_bool_norm_wrong_cond_eq_kept)
{
  /* cond must be NE (0x95); EQ leaves the pair untouched. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int setif2 = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_EQ, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_bool_norm_elim(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, setif2), TCCIR_OP_SETIF);
  utb_free(ir);
  return 0;
}

UT_TEST(test_bool_norm_nonzero_cmp_operand_kept)
{
  /* CMP T0, #5 -> the `!= 0` reduction does not apply. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(5, I32));
  int setif2 = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_bool_norm_elim(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, setif2), TCCIR_OP_SETIF);
  utb_free(ir);
  return 0;
}

UT_TEST(test_bool_norm_non_immediate_cmp_s2_kept)
{
  /* cmp_s2 must be immediate; a vreg operand blocks the rewrite. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(2, I32));
  int setif2 = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_bool_norm_elim(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, setif2), TCCIR_OP_SETIF);
  utb_free(ir);
  return 0;
}

UT_TEST(test_bool_norm_lval_first_operand_kept)
{
  /* cmp_s1.is_lval (a memory deref) is not a plain vreg -> reject. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE,
                     utb_lval(utb_temp(0, I32)), utb_imm(0, I32));
  int setif2 = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_bool_norm_elim(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, setif2), TCCIR_OP_SETIF);
  utb_free(ir);
  return 0;
}

UT_TEST(test_bool_norm_non_bool_def_kept)
{
  /* T0 is defined by a plain ASSIGN (not SETIF/BOOL_*) -> not provably {0,1}
   * -> ir_vreg_is_bool01 returns false -> no rewrite. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE);
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int setif2 = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_bool_norm_elim(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(ir, setif2), TCCIR_OP_SETIF);
  utb_free(ir);
  return 0;
}

UT_TEST(test_bool_norm_multi_def_bool_kept)
{
  /* T0 is SETIF-defined but also redefined later -> not single-def -> the bool
   * predicate bails (a later redefinition could carry a non-{0,1} value). */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE); /* def 1 */
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int setif2 = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  (void)setif2;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(1, I32), UTB_NONE); /* def 2 of T0 */

  UT_ASSERT_EQ(tcc_ir_opt_bool_norm_elim(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

UT_TEST(test_bool_norm_bool_and_def_rewritten)
{
  /* BOOL_AND is also accepted by ir_vreg_is_bool01 (idempotent boolean op). */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_BOOL_AND, utb_temp(0, I32), utb_param(0, I32), utb_param(1, I32));
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  int setif2 = utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_bool_norm_elim(ir), 1);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, setif2), TCCIR_OP_ASSIGN);
  utb_free(ir);
  return 0;
}

UT_TEST(test_bool_norm_adjacency_required)
{
  /* The pair must be strictly adjacent (CMP@i, SETIF@i+1).  An intervening
   * instr breaks adjacency; the outer loop only checks i+1. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(0, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_param(2, I32), UTB_NONE); /* spacer */
  utb_emit(ir, TCCIR_OP_SETIF, utb_temp(1, I32), utb_imm(UT_TOK_NE, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_bool_norm_elim(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_CMP);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_bool_norm)
{
  UT_COVERS("bool_norm_elim");
  UT_RUN(test_bool_norm_ne_of_bool_setif_rewritten);
  UT_RUN(test_bool_norm_too_short);
  UT_RUN(test_bool_norm_wrong_cond_eq_kept);
  UT_RUN(test_bool_norm_nonzero_cmp_operand_kept);
  UT_RUN(test_bool_norm_non_immediate_cmp_s2_kept);
  UT_RUN(test_bool_norm_lval_first_operand_kept);
  UT_RUN(test_bool_norm_non_bool_def_kept);
  UT_RUN(test_bool_norm_multi_def_bool_kept);
  UT_RUN(test_bool_norm_bool_and_def_rewritten);
  UT_RUN(test_bool_norm_adjacency_required);
}
