/*
 *  TCC Optimizer Tests - Global symbol address CSE
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir_build.h"
#include "opt/flat/symaddr_cse.h"
#include "ut.h"

#define I32 IROP_BTYPE_INT32

static TCCIRState *symaddr_ir(void)
{
  static TCCState state;
  TCCIRState *ir = utb_new();
  state.registers_for_allocator = 11;
  tcc_state = &state;
  utb_pools_init(ir);
  ir->temporary_variables_live_intervals_size = 64;
  ir->temporary_variables_live_intervals = tcc_mallocz(sizeof(IRLiveInterval) * 64);
  ir->next_temporary_variable = 32;
  return ir;
}

static IROperand symaddr_ref(TCCIRState *ir, Sym *sym, int32_t addend, int is_lval)
{
  uint32_t idx = tcc_ir_pool_add_symref(ir, sym, addend, 0);
  return irop_make_symref(0, idx, is_lval, 0, 0, I32);
}

UT_TEST(test_symaddr_cse_hoists_repeated_base)
{
  TCCIRState *ir = symaddr_ir();
  static Sym sym;
  int add0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(0, I32));
  int add1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(4, I32));
  int add2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(8, I32));

  int changes = tcc_ir_opt_symaddr_cse(ir);

  UT_ASSERT_EQ(changes, 3);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_ASSIGN);
  int32_t base = utb_vreg(utb_dest(ir, 0));
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(base), TCCIR_VREG_TYPE_TEMP);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, add0 + 1)), base);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, add1 + 1)), base);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, add2 + 1)), base);

  utb_free(ir);
  return 0;
}

UT_TEST(test_symaddr_cse_below_threshold_is_unchanged)
{
  TCCIRState *ir = symaddr_ir();
  static Sym sym;
  int add0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(4, I32));

  UT_ASSERT_EQ(tcc_ir_opt_symaddr_cse(ir), 0);
  UT_ASSERT_EQ(ir->next_instruction_index, 2);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, add0)), IROP_TAG_SYMREF);

  utb_free(ir);
  return 0;
}

UT_TEST(test_symaddr_cse_lval_same_address_blocks_hoist)
{
  TCCIRState *ir = symaddr_ir();
  static Sym sym;
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(7, I32), symaddr_ref(ir, &sym, 0, 1), UTB_NONE);
  int add0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(8, I32));

  UT_ASSERT_EQ(tcc_ir_opt_symaddr_cse(ir), 0);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, add0)), IROP_TAG_SYMREF);

  utb_free(ir);
  return 0;
}

UT_TEST(test_symaddr_cse_reuses_base_for_entry_store)
{
  TCCIRState *ir = symaddr_ir();
  static Sym sym;
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), symaddr_ref(ir, &sym, 0, 0), utb_imm(8, I32));
  int store = utb_emit(ir, TCCIR_OP_STORE, symaddr_ref(ir, &sym, 4, 1), utb_temp(8, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_symaddr_cse(ir), 4);
  UT_ASSERT_EQ(utb_op(ir, store + 1), TCCIR_OP_STORE_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, store + 1)), utb_vreg(utb_dest(ir, 0)));
  UT_ASSERT_EQ(irop_get_imm32(utb_src2(ir, store + 1)), 4);

  utb_free(ir);
  return 0;
}

UT_TEST(test_symaddr_cse_hoists_reversed_add)
{
  TCCIRState *ir = symaddr_ir();
  static Sym sym;
  int add0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(0, I32), symaddr_ref(ir, &sym, 0, 0));
  int add1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_imm(4, I32), symaddr_ref(ir, &sym, 0, 0));
  int add2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_imm(8, I32), symaddr_ref(ir, &sym, 0, 0));

  UT_ASSERT_EQ(tcc_ir_opt_symaddr_cse(ir), 3);
  int32_t base = utb_vreg(utb_dest(ir, 0));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, add0 + 1)), base);
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, add1 + 1)), base);
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, add2 + 1)), base);

  utb_free(ir);
  return 0;
}

UT_TEST(test_symaddr_cse_selects_hot_symbol_after_sixteen_candidates)
{
  TCCIRState *ir = symaddr_ir();
  static Sym syms[17];
  tcc_ir_pool_ensure(ir, 128);

  for (int i = 0; i < 16; i++) {
    utb_emit(ir, TCCIR_OP_ADD, utb_temp(i * 2 + 1, I32), symaddr_ref(ir, &syms[i], 0, 0), utb_imm(0, I32));
    utb_emit(ir, TCCIR_OP_ADD, utb_temp(i * 2 + 2, I32), symaddr_ref(ir, &syms[i], 0, 0), utb_imm(4, I32));
  }
  int hot0 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(40, I32), symaddr_ref(ir, &syms[16], 0, 0), utb_imm(0, I32));
  int hot1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(41, I32), symaddr_ref(ir, &syms[16], 0, 0), utb_imm(4, I32));
  int hot2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(42, I32), symaddr_ref(ir, &syms[16], 0, 0), utb_imm(8, I32));

  UT_ASSERT_EQ(tcc_ir_opt_symaddr_cse(ir), 3);
  int32_t base = utb_vreg(utb_dest(ir, 0));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, hot0 + 1)), base);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, hot1 + 1)), base);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, hot2 + 1)), base);

  utb_free(ir);
  return 0;
}
