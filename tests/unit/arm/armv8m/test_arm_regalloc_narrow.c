/*
 *  test_arm_regalloc_narrow.c - Tests for arm_regalloc.c narrow encoding capability
 *
 *  Exercises arm_op_narrow_capable() directly through the RegAllocTarget callback,
 *  covering all switch cases and edge cases for T16 narrow encoding support.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ir/regalloc.h"
#include "source/backend/arch/arm/arm_regalloc.h"
#include "ut.h"

/* Test arm_op_narrow_capable through the public API */
static int test_narrow_capable(TccIrOp op, int src2_is_imm, int scale)
{
  const RegAllocTarget *target = arm_get_regalloc_target();
  UT_ASSERT(target != NULL);
  UT_ASSERT(target->op_narrow_capable != NULL);
  return target->op_narrow_capable(op, src2_is_imm, scale);
}

/* Test all arithmetic ops that support narrow encoding */
UT_TEST(test_narrow_capable_arithmetic)
{
  /* ADD, SUB, MUL, AND, OR, XOR, SHL, SHR, SAR all support narrow */
  static const TccIrOp ops[] = {
    TCCIR_OP_ADD, TCCIR_OP_SUB, TCCIR_OP_MUL,
    TCCIR_OP_AND, TCCIR_OP_OR, TCCIR_OP_XOR,
    TCCIR_OP_SHL, TCCIR_OP_SHR, TCCIR_OP_SAR
  };

  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    /* All arithmetic ops should be narrow-capable with any src2/scale combo */
    UT_ASSERT_EQ(test_narrow_capable(ops[i], 0, 0), 1);
    UT_ASSERT_EQ(test_narrow_capable(ops[i], 1, 0), 1);
    UT_ASSERT_EQ(test_narrow_capable(ops[i], 0, 1), 1);
  }

  return 0;
}

/* Test memory ops that support narrow encoding */
UT_TEST(test_narrow_capable_memory)
{
  /* LOAD, STORE, LEA support narrow encoding */
  static const TccIrOp ops[] = {
    TCCIR_OP_LOAD, TCCIR_OP_STORE, TCCIR_OP_LEA
  };

  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    UT_ASSERT_EQ(test_narrow_capable(ops[i], 0, 0), 1);
    UT_ASSERT_EQ(test_narrow_capable(ops[i], 1, 0), 1);
  }

  return 0;
}

/* Test ZEXT and TEST_ZERO ops */
UT_TEST(test_narrow_capable_zext_testzero)
{
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_ZEXT, 0, 0), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_TEST_ZERO, 0, 0), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_ZEXT, 1, 0), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_TEST_ZERO, 1, 0), 1);

  return 0;
}

/* Test indexed memory ops with scale=0 (narrow-capable) */
UT_TEST(test_narrow_capable_indexed_no_scale)
{
  /* LOAD_INDEXED and STORE_INDEXED are narrow-capable when scale==0 */
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_LOAD_INDEXED, 0, 0), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_STORE_INDEXED, 0, 0), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_LOAD_INDEXED, 1, 0), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_STORE_INDEXED, 1, 0), 1);

  return 0;
}

/* Test indexed memory ops with scale!=0 (not narrow-capable) */
UT_TEST(test_narrow_capable_indexed_with_scale)
{
  /* LOAD_INDEXED and STORE_INDEXED are NOT narrow-capable when scale!=0 */
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_LOAD_INDEXED, 0, 1), 0);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_STORE_INDEXED, 0, 1), 0);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_LOAD_INDEXED, 1, 1), 0);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_STORE_INDEXED, 1, 1), 0);

  return 0;
}

/* Test CMP op - narrow-capable only when src2 is immediate */
UT_TEST(test_narrow_capable_cmp)
{
  /* CMP is narrow-capable when src2_is_imm==1 */
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_CMP, 1, 0), 1);
  /* CMP is NOT narrow-capable when src2_is_imm==0 */
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_CMP, 0, 0), 0);

  return 0;
}

/* Test JUMPIF op - narrow-capable only when src2 is immediate */
UT_TEST(test_narrow_capable_jumpif)
{
  /* JUMPIF is narrow-capable when src2_is_imm==1 */
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_JUMPIF, 1, 0), 1);
  /* JUMPIF is NOT narrow-capable when src2_is_imm==0 */
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_JUMPIF, 0, 0), 0);

  return 0;
}

/* Test unknown ops - should not be narrow-capable */
UT_TEST(test_narrow_capable_unknown_ops)
{
  /* These ops should not support narrow encoding */
  static const TccIrOp unknown_ops[] = {
    TCCIR_OP_JUMP, TCCIR_OP_IJUMP, TCCIR_OP_SWITCH_TABLE,
    TCCIR_OP_FUNCPARAMVAL, TCCIR_OP_FUNCCALLVAL, TCCIR_OP_RETURNVALUE,
    TCCIR_OP_FADD, TCCIR_OP_FSUB, TCCIR_OP_FMUL, TCCIR_OP_FDIV,
    TCCIR_OP_CVT_ITOF, TCCIR_OP_CVT_FTOI, TCCIR_OP_CVT_ITOF,
    TCCIR_OP_BLOCK_COPY, TCCIR_OP_LOAD_POSTINC, TCCIR_OP_STORE_POSTINC
  };

  for (size_t i = 0; i < sizeof(unknown_ops) / sizeof(unknown_ops[0]); i++) {
    UT_ASSERT_EQ(test_narrow_capable(unknown_ops[i], 0, 0), 0);
    UT_ASSERT_EQ(test_narrow_capable(unknown_ops[i], 1, 0), 0);
  }

  return 0;
}

/* Test edge cases with various scale values */
UT_TEST(test_narrow_capable_scale_edge_cases)
{
  /* LOAD_INDEXED/STORE_INDEXED with various scale values */
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_LOAD_INDEXED, 0, 0), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_LOAD_INDEXED, 0, 1), 0);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_LOAD_INDEXED, 0, 2), 0);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_LOAD_INDEXED, 0, 4), 0);

  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_STORE_INDEXED, 0, 0), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_STORE_INDEXED, 0, 1), 0);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_STORE_INDEXED, 0, 2), 0);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_STORE_INDEXED, 0, 4), 0);

  return 0;
}

/* Test CMP/JUMPIF with various scale values (scale doesn't matter for these) */
UT_TEST(test_narrow_capable_cmp_jumpif_scale_insensitive)
{
  /* For CMP and JUMPIF, scale parameter doesn't affect narrow capability */
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_CMP, 1, 0), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_CMP, 1, 1), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_CMP, 1, 2), 1);

  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_JUMPIF, 1, 0), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_JUMPIF, 1, 1), 1);
  UT_ASSERT_EQ(test_narrow_capable(TCCIR_OP_JUMPIF, 1, 2), 1);

  return 0;
}

/* Verify the callback is properly set in the target */
UT_TEST(test_target_callback_valid)
{
  const RegAllocTarget *target = arm_get_regalloc_target();
  UT_ASSERT(target != NULL);
  UT_ASSERT(target->op_narrow_capable != NULL);

  /* Should be able to call it without crashing */
  int result = target->op_narrow_capable(TCCIR_OP_ADD, 0, 0);
  UT_ASSERT_EQ(result, 1);

  return 0;
}

/* Test that the target descriptor is properly initialized */
UT_TEST(test_target_descriptor_complete)
{
  const RegAllocTarget *target = arm_get_regalloc_target();
  UT_ASSERT(target != NULL);

  /* Verify all required fields are set */
  UT_ASSERT(target->int_class.num_regs > 0);
  UT_ASSERT(target->int_class.caller_saved != NULL);
  UT_ASSERT(target->int_class.callee_saved != NULL);
  UT_ASSERT(target->fp_class.num_regs > 0);
  UT_ASSERT(target->fp_class.caller_saved != NULL);
  UT_ASSERT(target->fp_class.callee_saved != NULL);
  UT_ASSERT(target->param_regs > 0);
  UT_ASSERT(target->static_chain_reg >= 0);
  UT_ASSERT(target->op_narrow_capable != NULL);

  return 0;
}
