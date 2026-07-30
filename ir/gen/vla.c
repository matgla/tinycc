/*
 *  TCC IR - Variable Length Array (VLA) Support
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

void tcc_ir_gen_vla_alloc(TCCIRState *ir, SValue *size, int align)
{
  SValue align_sv;
  memset(&align_sv, 0, sizeof(align_sv));
  align_sv.type.t = VT_INT;
  align_sv.r = VT_CONST;
  align_sv.c.i = align;
  align_sv.vr = -1;
  tcc_ir_put(ir, TCCIR_OP_VLA_ALLOC, size, &align_sv, NULL);
}

void tcc_ir_gen_vla_sp_save(TCCIRState *ir, int slot)
{
  SValue dst;
  memset(&dst, 0, sizeof(dst));
  dst.type.t = VT_PTR;
  dst.r = VT_LOCAL | VT_LVAL;
  dst.c.i = slot;
  dst.vr = -1;
  tcc_ir_put(ir, TCCIR_OP_VLA_SP_SAVE, NULL, NULL, &dst);
}

void tcc_ir_gen_vla_sp_restore(TCCIRState *ir, int slot)
{
  SValue src;
  memset(&src, 0, sizeof(src));
  src.type.t = VT_PTR;
  src.r = VT_LOCAL | VT_LVAL;
  src.c.i = slot;
  src.vr = -1;
  tcc_ir_put(ir, TCCIR_OP_VLA_SP_RESTORE, &src, NULL, NULL);
}
