/*
 *  TCC IR - Dead temp-local elimination (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include <limits.h>

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "opt_loop_utils.h"


/* Eliminate non-call writes to anonymous TEMP_LOCAL slots (vreg [-9,-2]) with no later reader. */

static int dtl_read_operand(TCCIRState *ir, IRQuadCompact *p, int k, IROperand *out)
{
  if (k == 0) { if (!irop_config[p->op].has_dest) return 0; *out = tcc_ir_op_get_dest(ir, p); return 1; }
  if (k == 1) { if (!irop_config[p->op].has_src1) return 0; *out = tcc_ir_op_get_src1(ir, p); return 1; }
  if (!irop_config[p->op].has_src2) return 0;
  *out = tcc_ir_op_get_src2(ir, p);
  return 1;
}

int tcc_ir_opt_dead_temp_local_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  int changes = 0;
  /* Reverse order: NOP'ing the last write clears the forward window for earlier writes. */
  for (int i = n - 1; i >= 0; i--) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Calls (and their PARAMs) are eliminated by a separate pass; skip here. */
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(dest) != IROP_TAG_STACKOFF || !dest.is_local)
      continue;
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr > -2 || dest_vr < -9)
      continue;
    int my_off = irop_get_stack_offset(dest);
    int my_w = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
    if (my_w <= 0) my_w = irop_is_64bit(dest) ? 8 : 4;
    if (dest.is_complex) my_w *= 2;
    if (my_w <= 0 || my_w > 64) continue;
    /* Byte-precise forward scan: live_mask bit b = byte (my_off+b) still holds our value. */
    uint64_t live_mask = (my_w >= 64) ? ~(uint64_t)0 : ((uint64_t)1 << my_w) - 1;
    int alive = 0;
    int dead = 0;
    for (int j = i + 1; j < n && !alive && !dead; j++) {
      IRQuadCompact *p = &ir->compact_instructions[j];
      if (p->op == TCCIR_OP_NOP)
        continue;
      for (int k = 0; k < 3 && !alive && !dead; k++) {
        IROperand po;
        if (!dtl_read_operand(ir, p, k, &po)) continue;
        if (irop_get_vreg(po) != dest_vr) continue;
        int po_off = irop_get_stack_offset(po);
        int po_w = ir_opt_store_btype_size_bytes(irop_get_btype(po));
        if (po_w <= 0) po_w = irop_is_64bit(po) ? 8 : 4;
        if (po.is_complex) po_w *= 2;
        /* Clamp [po_off,po_off+po_w) to byte range [lo,hi) within our slot. */
        int lo = po_off - my_off;
        int hi = lo + po_w;
        if (lo < 0) lo = 0;
        if (hi > my_w) hi = my_w;

        if (k == 0 && irop_config[p->op].has_dest) {
          /* Subsequent write to our vreg — clobber overlapping bytes. */
          if (lo < hi) {
            uint64_t mask = (hi - lo >= 64) ? ~(uint64_t)0 : ((uint64_t)1 << (hi - lo)) - 1;
            live_mask &= ~(mask << lo);
            if (live_mask == 0) dead = 1;
          }
          continue;
        }
        if (po.is_lval) {
          /* LVAL source — direct memory read of our slot. */
          if (lo < hi) {
            uint64_t mask = (hi - lo >= 64) ? ~(uint64_t)0 : ((uint64_t)1 << (hi - lo)) - 1;
            if (live_mask & (mask << lo)) alive = 1;
          }
          continue;
        }
        /* Non-LVAL addr-of: resolve one LEA/ASSIGN hop to a PARAM whose call has a constant size. */
        int param_idx = -1;
        int pidx = -1;
        int sz = -1;
        if ((p->op == TCCIR_OP_FUNCPARAMVAL || p->op == TCCIR_OP_FUNCPARAMVOID) && k == 1) {
          param_idx = j;
        } else if ((p->op == TCCIR_OP_ASSIGN || p->op == TCCIR_OP_LEA) && k == 1) {
          IROperand pd = tcc_ir_op_get_dest(ir, p);
          int32_t pd_vr = irop_get_vreg(pd);
          if (pd_vr >= 0 && TCCIR_DECODE_VREG_TYPE(pd_vr) == TCCIR_VREG_TYPE_TEMP) {
            int hit = -1;
            int multi = 0;
            for (int m = j + 1; m < n && !multi; m++) {
              IRQuadCompact *mq = &ir->compact_instructions[m];
              if (mq->op == TCCIR_OP_NOP) continue;
              for (int mk = 0; mk < 3 && !multi; mk++) {
                IROperand mo;
                if (!dtl_read_operand(ir, mq, mk, &mo)) continue;
                if (irop_get_vreg(mo) != pd_vr) continue;
                if ((mq->op == TCCIR_OP_FUNCPARAMVAL || mq->op == TCCIR_OP_FUNCPARAMVOID) &&
                    mk == 1 && hit < 0) {
                  hit = m;
                } else {
                  multi = 1;
                }
              }
            }
            if (!multi && hit >= 0) param_idx = hit;
          }
        }
        if (param_idx >= 0) {
          IRQuadCompact *pp = &ir->compact_instructions[param_idx];
          uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, pp));
          int cid = TCCIR_DECODE_CALL_ID(enc);
          pidx = TCCIR_DECODE_PARAM_IDX(enc);
          for (int m = param_idx + 1; m < n; m++) {
            IRQuadCompact *cq = &ir->compact_instructions[m];
            if (cq->op != TCCIR_OP_FUNCCALLVOID && cq->op != TCCIR_OP_FUNCCALLVAL) continue;
            if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, cq))) != cid)
              continue;
            Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, cq));
            if (!callee) break;
            const char *nm = get_tok_str(callee->v, NULL);
            if (!nm) break;
            int sz_pidx = -1;
            if (!strcmp(nm, "__aeabi_memset") || !strcmp(nm, "memset"))
              sz_pidx = 2;
            else if (!strcmp(nm, "__aeabi_memmove") || !strcmp(nm, "__aeabi_memcpy") ||
                     !strcmp(nm, "memmove") || !strcmp(nm, "memcpy"))
              sz_pidx = 2;
            else if (!strcmp(nm, "__aeabi_memmove4") || !strcmp(nm, "__aeabi_memmove8") ||
                     !strcmp(nm, "__aeabi_memcpy4") || !strcmp(nm, "__aeabi_memcpy8"))
              sz_pidx = 1;
            if (sz_pidx < 0 || (pidx != 0 && pidx != 1)) break;
            IROperand sz_op;
            if (!ir_opt_get_call_param_operand(ir, m, sz_pidx, &sz_op)) break;
            if (irop_get_tag(sz_op) != IROP_TAG_IMM32) break;
            sz = (int)irop_get_imm64_ex(ir, sz_op);
            break;
          }
        }
        if (sz <= 0) {
          /* Couldn't bound the access — pessimistically alive. */
          alive = 1;
          continue;
        }
        int rlo = po_off - my_off;
        int rhi = rlo + sz;
        if (rlo < 0) rlo = 0;
        if (rhi > my_w) rhi = my_w;
        if (rlo >= rhi) continue; /* range disjoint from our live bytes */
        uint64_t mask = (rhi - rlo >= 64) ? ~(uint64_t)0 : ((uint64_t)1 << (rhi - rlo)) - 1;
        mask <<= rlo;
        if (pidx == 0) {
          /* Write through addr-of-our-slot. */
          live_mask &= ~mask;
          if (live_mask == 0) dead = 1;
        } else if (live_mask & mask) {
          alive = 1;
        }
      }
    }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD TEMP_LOCAL: nop op=%d at i=%d (vr=%d off=%d w=%d %s)",
               q->op, i, dest_vr, my_off, my_w, dead ? "overwritten" : "no-reader");
    q->op = TCCIR_OP_NOP;
    changes++;
  }
  return changes;
}
int tcc_ir_opt_dead_temp_local_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_temp_local_elim(ctx->ir); }
