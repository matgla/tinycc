/*
 *  TCC IR - Phi-copy / constant-staging chain fold
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define USING_GLOBALS
#include "ir.h"
#include "regalloc.h"

/* Folds the post-ra_resolve_phis chain `T_case <- const_N` + `T_phi <- T_case` (switch-case body plus its phi copy) into a single `T_phi <- const_N`. */
/* Precondition: T_case has exactly one use and the source is freely duplicable (IMM/SYMREF/STACKOFF/F32 inline payload, F64/I64/SYMREF via pool_idx). */
/* Rewrites the original def's dest rather than the copy: that keeps the slot carrying the basic-block label and leaves the source operand's in-pool payload intact. */
int ra_fold_phi_const_chain(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  int max_tmp = -1;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    if (!irop_config[q->op].has_dest) continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval) continue;
    int32_t v = irop_get_vreg(d);
    if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP) continue;
    int pos = TCCIR_DECODE_VREG_POSITION(v);
    if (pos > max_tmp) max_tmp = pos;
  }
  if (max_tmp < 0)
    return 0;

  /* def_idx[t]: -1 no def, -2 multi-def, else the single def's index; def_count saturates. */
  int *def_idx = tcc_malloc(sizeof(int) * (max_tmp + 1));
  int *use_count = tcc_mallocz(sizeof(int) * (max_tmp + 1));
  int *def_count = tcc_mallocz(sizeof(int) * (max_tmp + 1));
  for (int p = 0; p <= max_tmp; p++) def_idx[p] = -1;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    int op = q->op;

    if (irop_config[op].has_dest) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!d.is_lval) {
        int32_t v = irop_get_vreg(d);
        if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP) {
          int pos = TCCIR_DECODE_VREG_POSITION(v);
          if (pos <= max_tmp) {
            if (def_idx[pos] == -1) def_idx[pos] = i;
            else def_idx[pos] = -2;
            if (def_count[pos] < 3) def_count[pos]++;
          }
        }
      } else if (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED ||
                 op == TCCIR_OP_STORE_POSTINC) {
        int32_t v = irop_get_vreg(d);
        if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP) {
          int pos = TCCIR_DECODE_VREG_POSITION(v);
          if (pos <= max_tmp) use_count[pos]++;
        }
      }
    }
    if (irop_config[op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t v = irop_get_vreg(s);
      if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP) {
        int pos = TCCIR_DECODE_VREG_POSITION(v);
        if (pos <= max_tmp) use_count[pos]++;
      }
    }
    if (irop_config[op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      int32_t v = irop_get_vreg(s);
      if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP) {
        int pos = TCCIR_DECODE_VREG_POSITION(v);
        if (pos <= max_tmp) use_count[pos]++;
      }
    }
    if (op == TCCIR_OP_MLA) {
      IROperand s = tcc_ir_op_get_accum(ir, q);
      int32_t v = irop_get_vreg(s);
      if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP) {
        int pos = TCCIR_DECODE_VREG_POSITION(v);
        if (pos <= max_tmp) use_count[pos]++;
      }
    }
  }

  int folded = 0;
  for (int i = 1; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN) continue;

    IROperand u_dest = tcc_ir_op_get_dest(ir, q);
    if (u_dest.is_lval) continue;
    int32_t u_dest_vr = irop_get_vreg(u_dest);
    /* Any non-lval dest is fine: the def inherits it. */
    if (u_dest_vr < 0) continue;

    IROperand u_src = tcc_ir_op_get_src1(ir, q);
    if (u_src.is_lval || u_src.is_llocal) continue;
    int32_t src_vr = irop_get_vreg(u_src);
    if (src_vr < 0 || TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_TEMP) continue;
    int src_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
    if (src_pos > max_tmp) continue;

    if (use_count[src_pos] != 1) continue;
    int j = def_idx[src_pos];
    if (j < 0 || j >= i) continue;

    /* Require a phi-style multi-def dest: single-def TMPs gain nothing and regress test_mul32wide_outparams. */
    int u_dest_type = TCCIR_DECODE_VREG_TYPE(u_dest_vr);
    if (u_dest_type != TCCIR_VREG_TYPE_TEMP) continue;
    int u_dest_pos = TCCIR_DECODE_VREG_POSITION(u_dest_vr);
    if (u_dest_pos > max_tmp) continue;
    if (def_count[u_dest_pos] < 2) continue;

    IRQuadCompact *def_q = &ir->compact_instructions[j];
    if (def_q->op != TCCIR_OP_ASSIGN) continue;

    IROperand def_src = tcc_ir_op_get_src1(ir, def_q);
    /* Constant-like sources only: an is_lval memory read would need its load width preserved when the use's dest btype differs. */
    if (def_src.is_lval || def_src.is_llocal) continue;
    int dtag = def_src.tag;
    int safe_const = (dtag == IROP_TAG_IMM32 || dtag == IROP_TAG_F32 ||
                      dtag == IROP_TAG_I64 || dtag == IROP_TAG_F64 ||
                      dtag == IROP_TAG_SYMREF || dtag == IROP_TAG_STACKOFF);
    if (!safe_const) continue;

    /* btype must match across the whole chain: a width difference changes ASSIGN's implicit widen/narrow (e.g. ZEXT of a 32-bit const into a 64-bit register pair). */
    int u_dest_bt = irop_get_btype(u_dest);
    int def_dest_bt = irop_get_btype(tcc_ir_op_get_dest(ir, def_q));
    int def_src_bt = irop_get_btype(def_src);
    if (u_dest_bt != def_dest_bt || u_dest_bt != def_src_bt)
      continue;
    /* Guards collapsing a narrowing read of a wider T_src. */
    if (irop_get_btype(u_src) != u_dest_bt)
      continue;

    /* The def may itself be a jump target (case body start); only intervening landing zones break the chain. */
    int same_bb = 1;
    for (int k = j + 1; k <= i; k++) {
      if (ir->compact_instructions[k].is_jump_target) { same_bb = 0; break; }
    }
    if (!same_bb) continue;

    /* u_dest must not be redefined or read in (j, i): rewriting j's dest would change semantics. */
    int conflict = 0;
    for (int k = j + 1; k < i && !conflict; k++) {
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op == TCCIR_OP_NOP) continue;
      if (irop_config[kq->op].has_dest) {
        IROperand kd = tcc_ir_op_get_dest(ir, kq);
        if (!kd.is_lval && irop_get_vreg(kd) == u_dest_vr) { conflict = 1; break; }
      }
      if (irop_config[kq->op].has_src1) {
        IROperand ks = tcc_ir_op_get_src1(ir, kq);
        if (irop_get_vreg(ks) == u_dest_vr) { conflict = 1; break; }
      }
      if (irop_config[kq->op].has_src2) {
        IROperand ks = tcc_ir_op_get_src2(ir, kq);
        if (irop_get_vreg(ks) == u_dest_vr) { conflict = 1; break; }
      }
    }
    if (conflict) continue;

    tcc_ir_set_dest(ir, j, u_dest);
    q->op = TCCIR_OP_NOP;
    folded++;
  }

  tcc_free(def_idx);
  tcc_free(use_count);
  tcc_free(def_count);
  return folded;
}
