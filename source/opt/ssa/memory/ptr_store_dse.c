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
#include "opt_utils.h"
#include "opt_alias.h"


/* A store through a pointer is named by the root vreg its address derives from
 * and a constant byte offset: the pointer itself, or single-definition hops of
 * ASSIGN and ADD-constant (`p`, `p + 4`, a copy of either) down to a root that
 * holds one value for the whole function -- a single-definition TEMP or a
 * PARAM nothing redefines.  Two stores with the same root and overlapping
 * offsets write the same bytes whatever vregs name the pointers.
 *
 * The sret pointer of Zig's `return .{ .error = X, .payload = undefined }`
 * is written twice: a zero fill of the whole result, then the real fields.
 * The fill's stores go through `p`, `p + 4`, ... one TEMP each, the fields
 * through `p` with 64-bit indexed stores, so equal-vreg matching sees nothing.
 *
 * TCC_DISABLE_PASS=ptr_store_dse_affine goes back to matching only 32-bit
 * stores through one and the same single-definition TEMP. */
typedef struct PSDPend { int32_t root; int off; int size; int idx; } PSDPend;

enum { PSD_CAP = 32, PSD_HOPS = 8 };

/* The root and offset a pointer vreg holds; 0 when it is not a stable one. */
static int psd_resolve(IRSSAOptCtx *ctx, const uint8_t *param_redef, int hops, int32_t vr, int32_t *root, int *off)
{
  TCCIRState *ir = ctx->ir;
  int acc = 0;
  for (int hop = 0;; hop++) {
    if (vr < 0)
      return 0;
    if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_PARAM) {
      const int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (!hops || pos > ir->next_parameter || param_redef[pos])
        return 0;
      *root = vr;
      *off = acc;
      return 1;
    }
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || ssa_opt_def_total(vi) != 1)
      return 0;
    IRQuadCompact *dq = vi->def_instr >= 0 ? &ir->compact_instructions[vi->def_instr] : NULL;
    int stop = !hops || hop >= PSD_HOPS || !dq || (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_ADD) ||
               tcc_ir_instr_access_is_volatile(ir, dq);
    IROperand s1 = IROP_NONE;
    if (!stop) {
      s1 = tcc_ir_op_get_src1(ir, dq);
      stop = irop_get_tag(s1) != IROP_TAG_VREG || s1.is_lval || s1.is_local || s1.is_llocal || s1.is_sym ||
             irop_get_vreg(s1) < 0 || tcc_ir_barrel_shift_at(ir, dq);
    }
    if (!stop && dq->op == TCCIR_OP_ADD)
      stop = !tcc_ir_op_src2_is_imm(ir, dq) || tcc_ir_op_src2_is_lval(ir, dq) || tcc_ir_op_src2_is_sym(ir, dq);
    if (stop) {
      *root = vr;
      *off = acc;
      return 1;
    }
    if (dq->op == TCCIR_OP_ADD)
      acc += (int)tcc_ir_op_src2_imm(ir, dq);
    vr = irop_get_vreg(s1);
  }
}

/* The root, offset and width of the bytes store `q` writes; 0 for a store this
 * does not place.  Without `affine`: a 32-bit deref store of a single-definition
 * TEMP only. */
static int psd_store_range(IRSSAOptCtx *ctx, const uint8_t *param_redef, int affine, IRQuadCompact *q, int32_t *root,
                           int *off, int *size)
{
  TCCIRState *ir = ctx->ir;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (dest.is_local || dest.is_llocal || dest.is_sym)
    return 0;
  const int32_t pvr = irop_get_vreg(dest);
  if (pvr < 0 || irop_get_tag(dest) != IROP_TAG_VREG)
    return 0;
  int base_off = 0;
  int btype;
  if (q->op == TCCIR_OP_STORE) {
    if (!dest.is_lval)
      return 0;
    btype = irop_get_btype(dest);
  } else {
    if (!affine || dest.is_lval)
      return 0;
    IROperand sc = tcc_ir_op_get_scale(ir, q);
    if (irop_get_tag(sc) != IROP_TAG_IMM32 || !tcc_ir_op_src2_is_imm(ir, q) || tcc_ir_op_src2_is_lval(ir, q) ||
        tcc_ir_op_src2_is_sym(ir, q))
      return 0;
    const int shift = irop_get_imm32(sc);
    if (shift < 0 || shift > 3)
      return 0;
    base_off = (int)tcc_ir_op_src2_imm(ir, q) << shift;
    btype = irop_get_btype(tcc_ir_op_get_src1(ir, q));
  }
  if (!affine && btype != IROP_BTYPE_INT32 && btype != IROP_BTYPE_FLOAT32)
    return 0;
  const int w = ir_opt_store_btype_size_bytes(btype);
  if (w <= 0)
    return 0;
  int o;
  if (!psd_resolve(ctx, param_redef, affine, pvr, root, &o))
    return 0;
  *off = o + base_off;
  *size = w;
  return 1;
}

int tcc_ir_ssa_opt_ptr_store_dse(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n <= 0)
    return 0;
  PSDPend pend[PSD_CAP];
  int np = 0;
  int changes = 0;
  const int affine = !tcc_ir_opt_pass_disabled("ptr_store_dse_affine");

  uint8_t *is_target = tcc_mallocz((size_t)(n + 7) / 8);
  /* PARAM vregs some instruction writes: not a stable root. */
  uint8_t *param_redef = tcc_mallocz((size_t)ir->next_parameter + 1);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_INLINE_ASM) {
      tcc_free(param_redef);
      tcc_free(is_target);
      return 0;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t >= 0 && t < n)
        is_target[t / 8] |= (uint8_t)(1 << (t % 8));
    }
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && irop_config[q->op].has_dest) {
      const int32_t dv = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_PARAM) {
        const int pos = TCCIR_DECODE_VREG_POSITION(dv);
        if (pos <= ir->next_parameter)
          param_redef[pos] = 1;
      }
    }
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (is_target[i / 8] & (1 << (i % 8)))
      np = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      /* Every volatile store happens: pico-sdk's pio_sm_clear_fifos writes the
       * same bit twice to the XOR alias (toggle FJOIN_RX on, then off), and
       * dropping the first left the TX FIFO joined away -- the RP2350's SDIO
       * commands overflowed it and the card never answered. */
      if (src.is_lval || tcc_ir_instr_access_is_volatile(ir, q)) {
        np = 0;
        continue;
      }
      int32_t root;
      int off, size;
      if (psd_store_range(ctx, param_redef, affine, q, &root, &off, &size)) {
        /* Earlier stores this one covers entirely are dead. */
        for (int k = 0; k < np;) {
          if (pend[k].root == root && off <= pend[k].off && pend[k].off + pend[k].size <= off + size) {
            ssa_opt_nop_instr(ctx, pend[k].idx);
            changes++;
            pend[k] = pend[--np];
          } else
            k++;
        }
        if (np == PSD_CAP)
          pend[0] = pend[--np];
        pend[np].root = root;
        pend[np].off = off;
        pend[np].size = size;
        pend[np].idx = i;
        np++;
      }
      continue;
    }

    /* Deny by default: any op with a hazard of its own (a call, __builtin_apply,
     * setjmp/longjmp, a branch or return, a memory read, VLA, chain and call-sequence
     * ops, ...) may read *p.  Only flag traffic and prefetch hints are harmless. */
    if (ir_op_has(q->op, IR_HZ_FROM_OP & ~(IR_HZ_FLAGS_SET | IR_HZ_FLAGS_READ | IR_HZ_HINT))) {
      np = 0;
      continue;
    }
    if ((irop_config[q->op].has_src1 && tcc_ir_op_src1_is_lval(ir, q)) ||
        (irop_config[q->op].has_src2 && tcc_ir_op_src2_is_lval(ir, q)) ||
        (q->op == TCCIR_OP_MLA && tcc_ir_op_get_accum(ir, q).is_lval))
      np = 0;
  }
  tcc_free(param_redef);
  tcc_free(is_target);
  return changes;
}
