/*
 *  TCC IR - Soft-Float Call Support
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

/* Forward declaration for ABI soft-call name lookup */
extern const char *tcc_get_abi_softcall_name(SValue *src1, SValue *src2, SValue *dest, TccIrOp op);

/* Put a soft-float library call for FPU operations not supported by hardware */
void tcc_ir_put_soft_call(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  SValue param;
  Sym *sym;
  const int call_id = ir ? ir->next_call_id++ : 0;
  const char *func_name = NULL;

  func_name = tcc_get_abi_softcall_name(src1, src2, dest, op);
  if (func_name == NULL)
  {
    tcc_error("No soft-float ABI function for operation %s\n", tcc_ir_dump_op_name(op));
    return;
  }
  svalue_init(&param);
  param.r = VT_CONST;
  int argc = 0;
  if (irop_config[op].has_src1)
  {
    param.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
    tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, src1, &param, NULL);
    argc++;
  }
  if (irop_config[op].has_src2)
  {
    param.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
    tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, src2, &param, NULL);
    argc++;
  }
  sym = external_global_sym(tok_alloc_const(func_name), &func_old_type);
  param.r = VT_CONST | VT_SYM;
  param.sym = sym;
  param.c.i = 0;

  if (irop_config[op].has_dest)
  {
    SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, argc);
    tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &param, &call_id_sv, dest);
  }
  else
  {
    SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, argc);
    tcc_ir_put(ir, TCCIR_OP_FUNCCALLVOID, &param, &call_id_sv, NULL);
  }
}

/* Check if FPU operation needs soft-float call and emit it if needed.
 * Returns 1 if soft call was emitted, 0 if hardware FPU can be used.
 * Called from tcc_ir_put() in ir/gen/put.c. */
int ir_put_soft_call_fpu_if_needed(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  const int is64bit = tcc_is_64bit_operand(src1) || tcc_is_64bit_operand(src2) || tcc_is_64bit_operand(dest);
  const FloatingPointConfig *fpu = architecture_config.fpu;

  switch (op)
  {
  case TCCIR_OP_FADD:
    if (is64bit && fpu->has_dadd)
      return 0;
    else if (!is64bit && fpu->has_fadd)
      return 0;
    break;
  case TCCIR_OP_FSUB:
    if (is64bit && fpu->has_dsub)
      return 0;
    else if (!is64bit && fpu->has_fsub)
      return 0;
    break;
  case TCCIR_OP_FMUL:
    if (is64bit && fpu->has_dmul)
      return 0;
    else if (!is64bit && fpu->has_fmul)
      return 0;
    break;
  case TCCIR_OP_FDIV:
    if (is64bit && fpu->has_ddiv)
      return 0;
    else if (!is64bit && fpu->has_fdiv)
      return 0;
    break;
  case TCCIR_OP_FNEG:
    if (is64bit && fpu->has_dneg)
      return 0;
    else if (!is64bit && fpu->has_fneg)
      return 0;
    break;
  case TCCIR_OP_FCMP:
    if (is64bit && fpu->has_dcmp)
      return 0;
    else if (!is64bit && fpu->has_fcmp)
      return 0;
    break;
  case TCCIR_OP_CVT_ITOF:
    if (is64bit && fpu->has_itod)
      return 0;
    else if (!is64bit && fpu->has_itof)
      return 0;
    break;
  case TCCIR_OP_CVT_FTOI:
    if (is64bit && fpu->has_dtoi)
      return 0;
    else if (!is64bit && fpu->has_ftoi)
      return 0;
    break;
  case TCCIR_OP_CVT_FTOF:
  {
    /* Same-size conversion (e.g., double <-> long double on ARM where both are 8 bytes)
     * is a no-op - emit ASSIGN instead of a soft-float call. */
    int src_align, dst_align;
    int src_size = src1 ? type_size(&src1->type, &src_align) : 0;
    int dst_size = dest ? type_size(&dest->type, &dst_align) : 0;
    if (src_size == dst_size)
      return 0; /* Codegen handles same-size as copy */
    if (is64bit && fpu->has_dtof && fpu->has_ftod)
      return 0;
    break;
  }
  default:
    return 0;
  }

  /* No hardware support, emit soft-float call */
  tcc_ir_put_soft_call(ir, op, src1, src2, dest);
  return 1;
}
