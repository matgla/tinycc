/*
 *  TCC IR - SSA straight-line global store DSE
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
#include "global_store_dse.h"


/* Jump targets are recomputed from live JUMP/JUMPIFs: the is_jump_target flags persist on folded guards' merge points. */
typedef struct GSDPend { Sym *sym; int64_t addend; int idx; } GSDPend;

/* A symref read kills only that symbol's pending stores, a stack read kills none, any other deref kills all. */
static void gsd_read_reset(TCCIRState *ir, IROperand op, GSDPend *pend, int *np)
{
  if (!op.is_lval)
    return;
  if (op.is_sym) {
    IRPoolSymref *ref = irop_get_symref_ex(ir, op);
    if (ref && ref->sym) {
      for (int k = 0; k < *np;) {
        if (pend[k].sym == ref->sym)
          pend[k] = pend[--(*np)];
        else
          k++;
      }
      return;
    }
    *np = 0;
    return;
  }
  if (op.tag == IROP_TAG_STACKOFF || op.is_local || op.is_llocal)
    return;
  *np = 0;
}

int ssa_opt_global_store_dse(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n <= 0)
    return 0;
  enum { GSD_CAP = 16 };
  GSDPend pend[GSD_CAP];
  int np = 0;
  int changes = 0;

  uint8_t *is_target = tcc_mallocz((size_t)(n + 7) / 8);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_INLINE_ASM) {
      /* Targets we can't enumerate — a kill across one could be observed. */
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

    if (q->op == TCCIR_OP_STORE) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (dest.is_sym && dest.is_lval && !src.is_lval &&
          irop_get_btype(dest) == IROP_BTYPE_INT32) {
        IRPoolSymref *ref = irop_get_symref_ex(ir, dest);
        if (ref && ref->sym && !(ref->sym->type.t & VT_VOLATILE)) {
          for (int k = 0; k < np; k++) {
            if (pend[k].sym == ref->sym && pend[k].addend == ref->addend) {
              ssa_opt_nop_instr(ctx, pend[k].idx);
              changes++;
              pend[k] = pend[--np];
              break;
            }
          }
          if (np < GSD_CAP) {
            pend[np].sym = ref->sym;
            pend[np].addend = ref->addend;
            pend[np].idx = i;
            np++;
          }
          continue;
        }
      }
      np = 0;
      continue;
    }

    switch (q->op) {
    case TCCIR_OP_FUNCCALLVAL: case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_IJUMP: case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_BLOCK_COPY: case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_STORE_INDEXED: case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_LOAD_POSTINC:
    case TCCIR_OP_JUMP: case TCCIR_OP_JUMPIF:
    case TCCIR_OP_RETURNVALUE: case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      np = 0;
      continue;
    default:
      break;
    }
    if (irop_config[q->op].has_src1)
      gsd_read_reset(ir, tcc_ir_op_get_src1(ir, q), pend, &np);
    if (irop_config[q->op].has_src2)
      gsd_read_reset(ir, tcc_ir_op_get_src2(ir, q), pend, &np);
    if (q->op == TCCIR_OP_MLA)
      gsd_read_reset(ir, tcc_ir_op_get_accum(ir, q), pend, &np);
  }
  tcc_free(is_target);
  return changes;
}
