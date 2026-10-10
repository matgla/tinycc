/*
 *  TCC Unit Tests - Control-flow cycle classification
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ut.h"

static void cfg_test_put(TCCIRState *ir, int op, int target)
{
  SValue dest;
  svalue_init(&dest);
  dest.r = VT_CONST;
  dest.type.t = VT_INT;
  dest.c.i = target;
  tcc_ir_put(ir, op, irop_config[op].has_src1 ? &dest : NULL,
             irop_config[op].has_src2 ? &dest : NULL,
             irop_config[op].has_dest ? &dest : NULL);
}

UT_TEST(test_cfg_cycle_empty)
{
  UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(NULL), 0);
  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_cfg_cycle_backward_return_tail)
{
  TCCIRState *ir = tcc_ir_alloc();
  cfg_test_put(ir, TCCIR_OP_JUMPIF, 3);
  cfg_test_put(ir, TCCIR_OP_NOP, 0);
  cfg_test_put(ir, TCCIR_OP_RETURNVOID, 0);
  cfg_test_put(ir, TCCIR_OP_JUMP, 1);
  UT_ASSERT_EQ(tcc_ir_cfg_flat_has_backedge(ir), 1);
  UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_cfg_cycle_conditional_backward_return_tail)
{
  TCCIRState *ir = tcc_ir_alloc();
  cfg_test_put(ir, TCCIR_OP_JUMP, 2);
  cfg_test_put(ir, TCCIR_OP_RETURNVOID, 0);
  cfg_test_put(ir, TCCIR_OP_JUMPIF, 1);
  cfg_test_put(ir, TCCIR_OP_RETURNVOID, 0);
  UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_cfg_cycle_real_loop)
{
  TCCIRState *ir = tcc_ir_alloc();
  cfg_test_put(ir, TCCIR_OP_NOP, 0);
  cfg_test_put(ir, TCCIR_OP_JUMPIF, 0);
  cfg_test_put(ir, TCCIR_OP_RETURNVOID, 0);
  UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(ir), 1);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_cfg_cycle_self_loop)
{
  TCCIRState *ir = tcc_ir_alloc();
  cfg_test_put(ir, TCCIR_OP_JUMP, 0);
  UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(ir), 1);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_cfg_cycle_duplicate_successor_and_epilogue)
{
  TCCIRState *ir = tcc_ir_alloc();
  cfg_test_put(ir, TCCIR_OP_JUMPIF, 1);
  cfg_test_put(ir, TCCIR_OP_RETURNVOID, 0);
  cfg_test_put(ir, TCCIR_OP_JUMP, 0);
  UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(ir), 0);
  cfg_test_put(ir, TCCIR_OP_JUMP, 4);
  UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(ir), 0);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_cfg_cycle_heap_worklist_and_unreachable_loop)
{
  TCCIRState *ir = tcc_ir_alloc();
  cfg_test_put(ir, TCCIR_OP_RETURNVOID, 0);
  for (int i = 1; i < 80; i++)
    cfg_test_put(ir, TCCIR_OP_JUMP, i - 1);
  UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(ir), 0);
  cfg_test_put(ir, TCCIR_OP_JUMP, 80);
  UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(ir), 1);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_cfg_cycle_unknown_successors_are_conservative)
{
  int ops[] = {TCCIR_OP_IJUMP, TCCIR_OP_SWITCH_TABLE, TCCIR_OP_SWITCH_LOAD};
  for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    TCCIRState *ir = tcc_ir_alloc();
    cfg_test_put(ir, ops[i], 0);
    UT_ASSERT_EQ(tcc_ir_cfg_has_cycle(ir), 1);
    tcc_ir_free(ir);
  }
  return 0;
}
