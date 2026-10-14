/*
 *  test_opt_pure_via_sret.c - what tcc_ir_analyze_pure_via_sret
 *  (opt/flat/ipa/pure_via_sret.c) accepts as a pure aggregate-return body.
 *
 *  The op hazards come from op_props; two exclusions are not op hazards and
 *  are pinned here: __builtin_return_address (each call site sees its own
 *  return address, so the call must not be merged, removed or classed as an
 *  inlinable pure loop) and an op whose destination stores through a pointer
 *  that is neither the sret buffer nor a frame slot.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License.
 */

#include "ir_build.h"

#include "ut.h"

void tcc_ir_analyze_pure_via_sret(TCCIRState *ir, Sym *func_sym);

#define I32 IROP_BTYPE_INT32

/* A body returning through P0 (the sret pointer) with P1 a plain pointer. */
static TCCIRState *pvs_new(void)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->next_temporary_variable = 8;
  ir->next_local_variable = 2;
  ir->next_parameter = 2;
  func_vc = -8;
  return ir;
}

static int pvs_run(TCCIRState *ir)
{
  static Sym fn;
  memset(&fn, 0, sizeof(fn));
  tcc_ir_analyze_pure_via_sret(ir, &fn);
  utb_free(ir);
  func_vc = 0;
  return fn.a.pure_via_sret;
}

/* [P0] = T0 and a frame store: the only effects are the result. */
static void pvs_result_stores(TCCIRState *ir)
{
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_param(1, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(-16, 1, 0, 0, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_param(0, I32)), utb_temp(0, I32), UTB_NONE);
}

UT_TEST(test_pure_via_sret_result_only_is_pure)
{
  TCCIRState *ir = pvs_new();
  pvs_result_stores(ir);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(pvs_run(ir), 1);
  return 0;
}

UT_TEST(test_pure_via_sret_return_address_is_not_pure)
{
  TCCIRState *ir = pvs_new();
  pvs_result_stores(ir);
  utb_emit(ir, TCCIR_OP_RETURN_ADDRESS, utb_temp(1, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_param(0, I32)), utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(pvs_run(ir), 0);
  return 0;
}

/* An ALU op whose destination is *P1: a store the caller can see. */
UT_TEST(test_pure_via_sret_lval_dest_through_pointer_is_not_pure)
{
  TCCIRState *ir = pvs_new();
  pvs_result_stores(ir);
  utb_emit(ir, TCCIR_OP_ADD, utb_lval(utb_param(1, I32)), utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(pvs_run(ir), 0);
  return 0;
}

/* The same op writing a frame slot or the sret buffer is still pure. */
UT_TEST(test_pure_via_sret_lval_dest_frame_or_sret_is_pure)
{
  TCCIRState *ir = pvs_new();
  pvs_result_stores(ir);
  utb_emit(ir, TCCIR_OP_ADD, utb_stackoff(-12, 1, 0, 0, I32), utb_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_lval(utb_param(0, I32)), utb_temp(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  UT_ASSERT_EQ(pvs_run(ir), 1);
  return 0;
}
