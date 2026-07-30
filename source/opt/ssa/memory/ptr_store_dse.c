/*
 *  TCC IR - SSA TEMP-deref store-store DSE
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"


/* Only single-def TEMP pointers qualify, so two derefs of the same vreg provably name the same address. */
typedef struct PSDPend { int32_t ptr_vr; int btype; int idx; } PSDPend;

int tcc_ir_ssa_opt_ptr_store_dse(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n <= 0)
    return 0;
  enum { PSD_CAP = 8 };
  PSDPend pend[PSD_CAP];
  int np = 0;
  int changes = 0;

  uint8_t *is_target = tcc_mallocz((size_t)(n + 7) / 8);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_INLINE_ASM) {
      tcc_free(is_target);
      return 0;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (t >= 0 && t < n)
        is_target[t / 8] |= (uint8_t)(1 << (t % 8));
    }
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (is_target[i / 8] & (1 << (i % 8)))
      np = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (src.is_lval) {
        np = 0;
        continue;
      }
      int32_t pvr = irop_get_vreg(dest);
      int btype = irop_get_btype(dest);
      if (q->op == TCCIR_OP_STORE && dest.is_lval && dest.tag == IROP_TAG_VREG &&
          !dest.is_local && !dest.is_llocal && !dest.is_sym && pvr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(pvr) == TCCIR_VREG_TYPE_TEMP &&
          (btype == IROP_BTYPE_INT32 || btype == IROP_BTYPE_FLOAT32)) {
        IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, pvr);
        if (pvi && pvi->def_count == 1) {
          for (int k = 0; k < np; k++) {
            if (pend[k].ptr_vr == pvr) {
              if (pend[k].btype == btype) {
                ssa_opt_nop_instr(ctx, pend[k].idx);
                changes++;
              }
              pend[k] = pend[--np];
              break;
            }
          }
          if (np < PSD_CAP) {
            pend[np].ptr_vr = pvr;
            pend[np].btype = btype;
            pend[np].idx = i;
            np++;
          }
        }
      }
      continue;
    }

    switch (q->op) {
    case TCCIR_OP_FUNCCALLVAL: case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_BLOCK_COPY: case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_LOAD_POSTINC: case TCCIR_OP_LOAD:
    case TCCIR_OP_LOAD_INDEXED: case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_JUMP: case TCCIR_OP_JUMPIF:
    case TCCIR_OP_RETURNVALUE: case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      np = 0;
      continue;
    default:
      break;
    }
    if ((irop_config[q->op].has_src1 && tcc_ir_op_get_src1(ir, q).is_lval) ||
        (irop_config[q->op].has_src2 && tcc_ir_op_get_src2(ir, q).is_lval) ||
        (q->op == TCCIR_OP_MLA && tcc_ir_op_get_accum(ir, q).is_lval))
      np = 0;
  }
  tcc_free(is_target);
  return changes;
}
