/*
 *  test_opt_self_store.c - suite for ir/self_store.c (self-store removal)
 *
 *  tcc_ir_self_store deletes `StackLoc[X] <-- T [STORE]` when T was loaded from
 *  StackLoc[X] earlier in the same straight run and nothing between can write
 *  those bytes; the load goes too when the store was T's only use.  The shape
 *  is what frame relayout leaves of a copy between two objects it placed on
 *  the same bytes.
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_self_store(TCCIRState *ir);

#define I8 IROP_BTYPE_INT8
#define I32 IROP_BTYPE_INT32

static IROperand slot(int off, int btype)
{
  return utb_stackoff(off, 1, 0, 0, btype);
}

static TCCIRState *new_ir(int ntemps)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = ntemps;
  return ir;
}

/* The two-word copy frame relayout leaves: all four instructions go. */
UT_TEST(test_self_store_two_words_removed)
{
  TCCIRState *ir = new_ir(2);
  int l0 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot(-16, I32), UTB_NONE);
  int l1 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), slot(-12, I32), UTB_NONE);
  int s0 = utb_emit(ir, TCCIR_OP_STORE, slot(-16, I32), utb_temp(0, I32), UTB_NONE);
  int s1 = utb_emit(ir, TCCIR_OP_STORE, slot(-12, I32), utb_temp(1, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_self_store(ir), 2);
  UT_ASSERT_EQ(utb_op(ir, l0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, l1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, s1), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

/* The loaded value is used again: the store goes, the load stays. */
UT_TEST(test_self_store_load_kept_for_other_use)
{
  TCCIRState *ir = new_ir(2);
  int l0 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot(-16, I32), UTB_NONE);
  int s0 = utb_emit(ir, TCCIR_OP_STORE, slot(-16, I32), utb_temp(0, I32), UTB_NONE);
  int a = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));

  UT_ASSERT_EQ(tcc_ir_self_store(ir), 1);
  UT_ASSERT_EQ(utb_op(ir, l0), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, a), TCCIR_OP_ADD);
  utb_free(ir);
  return 0;
}

/* A store overlapping the slot between load and store: the bytes changed. */
UT_TEST(test_self_store_overlapping_store_blocks)
{
  TCCIRState *ir = new_ir(1);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot(-16, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, slot(-14, I8), utb_imm(5, I8), UTB_NONE);
  int s0 = utb_emit(ir, TCCIR_OP_STORE, slot(-16, I32), utb_temp(0, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_self_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_STORE);
  utb_free(ir);
  return 0;
}

/* A store to disjoint bytes between them does not block. */
UT_TEST(test_self_store_disjoint_store_allowed)
{
  TCCIRState *ir = new_ir(1);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot(-16, I32), UTB_NONE);
  int o = utb_emit(ir, TCCIR_OP_STORE, slot(-12, I32), utb_imm(5, I32), UTB_NONE);
  int s0 = utb_emit(ir, TCCIR_OP_STORE, slot(-16, I32), utb_temp(0, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_self_store(ir), 1);
  UT_ASSERT_EQ(utb_op(ir, o), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_NOP);
  utb_free(ir);
  return 0;
}

/* A call between them may write the slot through an escaped address. */
UT_TEST(test_self_store_call_blocks)
{
  TCCIRState *ir = new_ir(1);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot(-16, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_imm(0, I32), utb_imm(0, I32));
  int s0 = utb_emit(ir, TCCIR_OP_STORE, slot(-16, I32), utb_temp(0, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_self_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_STORE);
  utb_free(ir);
  return 0;
}

/* A store through a pointer between them may alias the slot. */
UT_TEST(test_self_store_pointer_store_blocks)
{
  TCCIRState *ir = new_ir(2);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot(-16, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_imm(7, I32), UTB_NONE);
  int s0 = utb_emit(ir, TCCIR_OP_STORE, slot(-16, I32), utb_temp(0, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_self_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_STORE);
  utb_free(ir);
  return 0;
}

/* The store is a jump target: another path may arrive with other bytes. */
UT_TEST(test_self_store_jump_target_blocks)
{
  TCCIRState *ir = new_ir(1);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot(-16, I32), UTB_NONE);
  int n = utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  int s0 = utb_emit(ir, TCCIR_OP_STORE, slot(-16, I32), utb_temp(0, I32), UTB_NONE);
  ir->compact_instructions[n].is_jump_target = 1;

  UT_ASSERT_EQ(tcc_ir_self_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_STORE);
  utb_free(ir);
  return 0;
}

/* Another slot, or a store wider than the load: not a self-copy. */
UT_TEST(test_self_store_other_slot_or_wider_store_kept)
{
  TCCIRState *ir = new_ir(2);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot(-16, I32), UTB_NONE);
  int s0 = utb_emit(ir, TCCIR_OP_STORE, slot(-20, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I8), slot(-8, I8), UTB_NONE);
  int s1 = utb_emit(ir, TCCIR_OP_STORE, slot(-8, I32), utb_temp(1, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_self_store(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, s1), TCCIR_OP_STORE);
  utb_free(ir);
  return 0;
}
