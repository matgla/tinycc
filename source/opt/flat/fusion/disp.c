/*
 *  TCC IR - Fold a `base + #imm` address into a displacement load/store (pre-SSA engine)
 *
 *  A LOAD/STORE (or lval-source ASSIGN) whose address is a single-use `base ± #imm`
 *  ADD/SUB collapses to LOAD_INDEXED/STORE_INDEXED[base, #imm, scale=0], NOPing the
 *  ADD/SUB, provided the displacement stays inside the Thumb-2 immediate range and the
 *  base is a genuine register value (not a local/lval whose value would be corrupted).
 *  A single-use ASSIGN copy feeding the base is forwarded through and NOP'd as well.
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_dsl.h"
#include "opt/flat/disp.h"

OPT_GEN_FLAT(disp, TCCIR_OP_LOAD)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;

  if (!tcc_state->opt_disp_fusion)
    return 0;

  IRQuadCompact *q = &ir->compact_instructions[i];

  int is_store = 0;
  int is_load = 0;
  IROperand addr_op = IROP_NONE;

  if (q->op == TCCIR_OP_LOAD) {
    is_load = 1;
    addr_op = tcc_ir_op_get_src1(ir, q);
  } else if (q->op == TCCIR_OP_STORE) {
    is_store = 1;
    addr_op = tcc_ir_op_get_dest(ir, q);
  } else if (q->op == TCCIR_OP_ASSIGN) {
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (!src1.is_lval)
      return 0;
    is_load = 1;
    addr_op = src1;
  } else {
    return 0;
  }

  if (!irop_has_vreg(addr_op))
    return 0;

  int32_t addr_vr = irop_get_vreg(addr_op);

  if (is_load && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
    return 0;

  {
    int access_btype = addr_op.btype;
    if (access_btype == IROP_BTYPE_INT64 || access_btype == IROP_BTYPE_FLOAT64 ||
        access_btype == IROP_BTYPE_STRUCT)
      return 0;
  }

  if (is_load) {
    IROperand dest_op = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest_op);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
  }

  int add_idx = ir_opt_du_def(du, addr_vr, i);
  if (add_idx < 0)
    return 0;

  IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
  int is_sub = (add_q->op == TCCIR_OP_SUB);
  if (add_q->op != TCCIR_OP_ADD && !is_sub)
    return 0;

  if (ir_opt_du_uses(du, addr_vr) != 1)
    return 0;

  IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
  IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);

  IROperand base_op;
  int imm;
  if (irop_get_tag(add_src2) == IROP_TAG_IMM32 && irop_get_tag(add_src1) == IROP_TAG_VREG && irop_has_vreg(add_src1)) {
    base_op = add_src1;
    imm = is_sub ? -(int)add_src2.u.imm32 : (int)add_src2.u.imm32;
  } else if (!is_sub && irop_get_tag(add_src1) == IROP_TAG_IMM32 && irop_get_tag(add_src2) == IROP_TAG_VREG &&
             irop_has_vreg(add_src2)) {
    base_op = add_src2;
    imm = (int)add_src1.u.imm32;
  } else {
    return 0;
  }

  if (imm > 4095 || imm < -255)
    return 0;
  if (base_op.is_local || base_op.is_llocal || base_op.is_lval)
    return 0;
  if (!ir_xform_same_block(ir, add_idx, i))
    return 0;

  IROperand orig_dest = tcc_ir_op_get_dest(ir, q);
  IROperand orig_src1 = tcc_ir_op_get_src1(ir, q);

  {
    int32_t base_vr = irop_get_vreg(base_op);
    if (base_vr >= 0 && TCCIR_DECODE_VREG_TYPE(base_vr) == TCCIR_VREG_TYPE_TEMP &&
        ir_opt_du_uses(du, base_vr) == 1) {
      int copy_idx = ir_opt_du_def(du, base_vr, add_idx);
      if (copy_idx >= 0) {
        IRQuadCompact *copy_q = &ir->compact_instructions[copy_idx];
        if (copy_q->op == TCCIR_OP_ASSIGN) {
          IROperand copy_dest = tcc_ir_op_get_dest(ir, copy_q);
          IROperand copy_src = tcc_ir_op_get_src1(ir, copy_q);
          if (!copy_dest.is_lval && !copy_src.is_lval && irop_has_vreg(copy_src)) {
            base_op = copy_src;
            copy_q->op = TCCIR_OP_NOP;
          }
        }
      }
    }
  }

  tcc_ir_pool_ensure(ir, 4);
  int new_base_idx = ir->iroperand_pool_count;
  if (new_base_idx + 4 > ir->iroperand_pool_capacity)
    return 0;
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);
  tcc_ir_pool_add(ir, IROP_NONE);

  IROperand index_imm = irop_make_imm32(0, imm, IROP_BTYPE_INT32);
  IROperand scale_imm = irop_make_imm32(0, 0, IROP_BTYPE_INT32);

  if (is_store) {
    IROperand base_for_store = base_op;
    base_for_store.is_lval = 0;
    /* Carry the access marks from the replaced deref operand: the backend's
     * 64-bit indexed lowering assumes alignment (LDRD/STRD), and the store
     * DSE passes must still see whether the access is volatile. */
    irop_carry_access_marks(&base_for_store, orig_dest);
    ir->iroperand_pool[new_base_idx + 0] = base_for_store;
    ir->iroperand_pool[new_base_idx + 1] = orig_src1;
    ir->iroperand_pool[new_base_idx + 2] = index_imm;
    ir->iroperand_pool[new_base_idx + 3] = scale_imm;
    q->op = TCCIR_OP_STORE_INDEXED;
  } else {
    IROperand base_for_load = base_op;
    base_for_load.is_lval = 0;
    irop_carry_access_marks(&base_for_load, orig_src1);
    IROperand new_dest = orig_dest;
    if (q->op == TCCIR_OP_ASSIGN) {
      new_dest.btype = addr_op.btype;
      new_dest.is_unsigned = addr_op.is_unsigned;
    }
    ir->iroperand_pool[new_base_idx + 0] = new_dest;
    ir->iroperand_pool[new_base_idx + 1] = base_for_load;
    ir->iroperand_pool[new_base_idx + 2] = index_imm;
    ir->iroperand_pool[new_base_idx + 3] = scale_imm;
    q->op = TCCIR_OP_LOAD_INDEXED;
  }
  q->operand_base = new_base_idx;

  add_q->op = TCCIR_OP_NOP;
  return 1;
}

const IROptGen fusion_disp_gens[] = {
    {TCCIR_OP_LOAD, opt_dsl_dispatch_disp_flat, "disp_load_fusion", 1},
    {TCCIR_OP_STORE, opt_dsl_dispatch_disp_flat, "disp_store_fusion", 1},
    {TCCIR_OP_ASSIGN, opt_dsl_dispatch_disp_flat, "disp_assign_fusion", 1},
};

const int fusion_disp_gens_count = sizeof(fusion_disp_gens) / sizeof(fusion_disp_gens[0]);

int tcc_ir_opt_gens_disp_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, fusion_disp_gens, fusion_disp_gens_count);
}
