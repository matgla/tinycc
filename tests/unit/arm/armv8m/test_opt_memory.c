/*
 *  test_opt_memory.c - suite for ir/opt_memory.c
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_opt_sl_forward(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define VR_VAR(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, (p))

static IROperand slot_lval(int32_t off)
{
  return irop_make_stackoff(0, off, 1, 0, 0, I32);
}

static IROperand slot_addr(int32_t off)
{
  return irop_make_stackoff(0, off, 0, 0, 0, I32);
}

static IROperand var_slot_lval(int pos, int32_t off)
{
  return irop_make_stackoff(VR_VAR(pos), off, 1, 0, 0, I32);
}

static void utb_alloc_var_intervals(TCCIRState *ir, int count)
{
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->variables_live_intervals_size = count;
}

static void utb_alloc_tmp_intervals(TCCIRState *ir, int count)
{
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->temporary_variables_live_intervals_size = count;
  ir->next_temporary_variable = count - 1;
}

/* GUARD (STORE_LVAL VAR namespace): anonymous StackLoc offsets and VAR-backed
 * stack slots can have the same raw offset.  STORE_LVAL forwarding must not
 * replace a read from VAR0's slot with an unrelated anonymous StackLoc value. */
UT_TEST(test_sl_forward_store_lval_var_slot_does_not_alias_stackloc)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 1);

  utb_emit(ir, TCCIR_OP_STORE, slot_lval(-88), utb_imm(123, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, slot_lval(-200), var_slot_lval(0, -88), UTB_NONE);

  (void)tcc_ir_opt_sl_forward(ir);

  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, store)), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, store)), VR_VAR(0));
  UT_ASSERT(utb_src1(ir, store).is_lval);

  utb_free(ir);
  return 0;
}

/* GUARD (FORWARD-HI): a STORE whose source TEMP has stale 64-bit metadata but
 * resolves to a forwarded 32-bit constant must not become an 8-byte store. */
UT_TEST(test_sl_forward_resolves_temp_before_store_width)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 2);
  ir->temporary_variables_live_intervals[0].is_llong = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(-2385, I32), UTB_NONE);
  IROperand noisy_dest = slot_lval(-56);
  noisy_dest.btype = IROP_BTYPE_INT64;
  utb_emit(ir, TCCIR_OP_STORE, noisy_dest, utb_temp(0, I32), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), slot_lval(-52), UTB_NONE);

  (void)tcc_ir_opt_sl_forward(ir);

  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, load)), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_stack_offset(utb_src1(ir, load)), -52);

  utb_free(ir);
  return 0;
}

/* GUARD (LEA-deref store width): a pointer-derived 32-bit store must not be
 * widened by stale/wider source metadata and then forwarded as the low half of
 * an adjacent 64-bit store. */
UT_TEST(test_sl_forward_pointer_store_keeps_deref_width_for_high_half)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 3);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), slot_addr(-56), UTB_NONE);
  uint32_t wide_idx = tcc_ir_pool_add_i64(ir, 0);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)),
           irop_make_i64(-1, wide_idx, IROP_BTYPE_INT64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);

  (void)tcc_ir_opt_sl_forward(ir);

  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load)), 1);

  utb_free(ir);
  return 0;
}

UT_SUITE(opt_memory)
{
  UT_RUN(test_sl_forward_store_lval_var_slot_does_not_alias_stackloc);
  UT_RUN(test_sl_forward_resolves_temp_before_store_width);
  UT_RUN(test_sl_forward_pointer_store_keeps_deref_width_for_high_half);
  UT_COVERS("sl_forward");
}
