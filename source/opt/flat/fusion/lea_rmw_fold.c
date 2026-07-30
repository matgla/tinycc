/*
 *  TCC IR - Fusion & Addressing Mode Optimization
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
#include "opt_loop_utils.h"
#include "opt_alias.h"
#include "opt_utils.h"

extern int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q);



/*
 * LEA read-modify-write fold
 *
 * Generalizes lea_fold's single-use case to an `Addr[StackLoc[X]]` LEA whose
 * *every* use is a same-block stack-slot deref (the `u.field++` idiom:
 * materialize the field address once, load+store it).  Each deref is rewritten
 * to a direct StackLoc[X] access and the LEA dropped.  An optional single
 * `T2 = T ADD #K` interposer folds a non-zero struct offset K into the offset.
 *
 * Safety: every use of the LEA result (and of any interposer result) must be a
 * same-block deref (is_lval load operand or plain STORE base).  Any non-deref
 * use (address escape into PARAM/call/non-lval op, a STORE/LOAD_INDEXED base, a
 * struct read, or a use past a control-flow edge) disables the fold.  No
 * instruction is moved; only operand forms change, so order and aliasing are
 * preserved.  Two further width/bitfield restrictions (below) keep the
 * direct-StackLoc form from exposing partial-overwrite hazards to the DSE chain.
 */

#define LEA_RMW_MAX_SITES 32

int tcc_ir_opt_lea_rmw_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *lea_q = &ir->compact_instructions[i];

    /* Entry shape: plain LEA / ASSIGN of Addr[StackLoc[X]] (no vreg base,
     * no double-indirect) into a TEMP. */
    if (lea_q->op == TCCIR_OP_ASSIGN)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, lea_q);
      if (!irop_is_none(s2))
        continue;
    }
    else if (lea_q->op != TCCIR_OP_LEA)
      continue;

    IROperand lea_src = tcc_ir_op_get_src1(ir, lea_q);
    if (irop_get_tag(lea_src) != IROP_TAG_STACKOFF)
      continue;
    if (lea_src.is_lval || lea_src.is_llocal)
      continue;
    if (irop_get_vreg(lea_src) != -1) /* vreg-backed spill slot — see lea_fold */
      continue;

    IROperand lea_dest = tcc_ir_op_get_dest(ir, lea_q);
    int32_t lea_vr = irop_get_vreg(lea_dest);
    if (lea_vr < 0 || TCCIR_DECODE_VREG_TYPE(lea_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int32_t base_offset = irop_get_stack_offset(lea_src);

    /* Block end: first control-flow edge after the LEA.  Any use of the LEA
     * result at or beyond this point crosses a basic-block boundary. */
    int bb_end = n;
    for (int k = i + 1; k < n; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        bb_end = k;
        break;
      }
    }

    /* Worklist of address vregs derived from the LEA, tagged with their
     * offset from the slot base and their defining index (skipped on scan).
     * Entry 0 is the LEA result itself at offset 0. */
    int32_t wl_vr[LEA_RMW_MAX_SITES];
    int32_t wl_off[LEA_RMW_MAX_SITES];
    int wl_def[LEA_RMW_MAX_SITES];
    int wl_n = 1;
    wl_vr[0] = lea_vr;
    wl_off[0] = 0;
    wl_def[0] = i;

    /* Deref sites to redirect at a direct StackLoc. */
    int rw_idx[LEA_RMW_MAX_SITES];
    int rw_which[LEA_RMW_MAX_SITES];
    int32_t rw_off[LEA_RMW_MAX_SITES];
    int rw_n = 0;

    int ok = 1;
    for (int w = 0; w < wl_n && ok; w++)
    {
      int32_t av = wl_vr[w];
      int32_t aoff = wl_off[w];
      int adef = wl_def[w];

      for (int k = i + 1; k < n && ok; k++)
      {
        if (k == adef)
          continue;
        IRQuadCompact *q = &ir->compact_instructions[k];
        if (q->op == TCCIR_OP_NOP)
          continue;
        const IRRegistersConfig *cfg = &irop_config[q->op];

        IROperand s1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE;
        IROperand s2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE;
        IROperand d = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE;

        int ref_lval = 0, ref_nonlval = 0, which_lval = -1;
        if (cfg->has_src1 && irop_has_vreg(s1) && irop_get_vreg(s1) == av)
        {
          if (s1.is_lval) { ref_lval++; which_lval = 1; }
          else ref_nonlval++;
        }
        if (cfg->has_src2 && irop_has_vreg(s2) && irop_get_vreg(s2) == av)
        {
          if (s2.is_lval) { ref_lval++; which_lval = 2; }
          else ref_nonlval++;
        }
        if (cfg->has_dest && irop_has_vreg(d) && irop_get_vreg(d) == av)
        {
          if (d.is_lval) { ref_lval++; which_lval = 0; }
          else ref_nonlval++;
        }

        if (ref_lval == 0 && ref_nonlval == 0)
          continue;

        if (k >= bb_end)
        {
          ok = 0;
          break;
        }

        /* Interposer `new = av + #K` (av as a plain pointer value). */
        if (q->op == TCCIR_OP_ADD && ref_lval == 0 && ref_nonlval == 1)
        {
          int32_t kk;
          if (irop_has_vreg(s1) && irop_get_vreg(s1) == av && !s1.is_lval &&
              irop_get_tag(s2) == IROP_TAG_IMM32)
            kk = (int32_t)s2.u.imm32;
          else if (irop_has_vreg(s2) && irop_get_vreg(s2) == av && !s2.is_lval &&
                   irop_get_tag(s1) == IROP_TAG_IMM32)
            kk = (int32_t)s1.u.imm32;
          else
          {
            ok = 0;
            break;
          }
          int32_t nvr = irop_get_vreg(d);
          if (nvr < 0 || TCCIR_DECODE_VREG_TYPE(nvr) != TCCIR_VREG_TYPE_TEMP || d.is_lval ||
              wl_n >= LEA_RMW_MAX_SITES)
          {
            ok = 0;
            break;
          }
          wl_vr[wl_n] = nvr;
          wl_off[wl_n] = aoff + kk;
          wl_def[wl_n] = k;
          wl_n++;
          continue;
        }

        /* Otherwise must be a single clean deref (load operand or STORE base)
         * of a non-struct width. */
        if (ref_lval != 1 || ref_nonlval != 0 || rw_n >= LEA_RMW_MAX_SITES)
        {
          ok = 0;
          break;
        }
        IROperand dref = (which_lval == 1) ? s1 : (which_lval == 2) ? s2 : d;
        /* Restrict to 8-byte (long long / double) accesses.  A narrower folded
         * store can land at a sub-offset of a wider store to the same slot;
         * DSE then drops the wider store once the now-dead narrow RMW load is
         * DCE'd.  8-byte RMW is aligned and never a strict sub-range, so the
         * hazard cannot arise; narrower accesses stay as opaque LEA-derefs. */
        if (dref.btype != IROP_BTYPE_INT64 && dref.btype != IROP_BTYPE_FLOAT64)
        {
          ok = 0;
          break;
        }
        /* Reject bitfield write-backs even at 8-byte width: a bitfield store's
         * value is a masked merge of the slot's prior content
         * (`(load & ~mask) | bits`), so the initializing store stays live.  As
         * a direct StackLoc store the DSE/loop passes treat it as a clean
         * full-word overwrite and drop the write-back or the init.  The merge
         * always tops out in an OR/AND consuming a load of this same slot, so a
         * STORE whose value is defined by OR/AND is left as an LEA-deref; plain
         * arithmetic RMW (ADD/SUB/FADD/FSUB) is unaffected. */
        if (which_lval == 0 && q->op == TCCIR_OP_STORE && irop_has_vreg(s1) && !s1.is_lval)
        {
          int32_t vvr = irop_get_vreg(s1);
          for (int d2 = k - 1; d2 >= 0; d2--)
          {
            IRQuadCompact *dq = &ir->compact_instructions[d2];
            if (dq->op == TCCIR_OP_NOP)
              continue;
            if (!irop_config[dq->op].has_dest)
              continue;
            if (irop_get_vreg(tcc_ir_op_get_dest(ir, dq)) != vvr)
              continue;
            if (dq->op == TCCIR_OP_OR || dq->op == TCCIR_OP_AND)
              ok = 0;
            break; /* found the def */
          }
          if (!ok)
            break;
        }
        rw_idx[rw_n] = k;
        rw_which[rw_n] = which_lval;
        rw_off[rw_n] = aoff;
        rw_n++;
      }
    }

    if (!ok || rw_n == 0)
      continue;

    /* Apply: redirect every deref operand at a direct StackLoc, then NOP the
     * LEA and every interposer ADD. */
    for (int r = 0; r < rw_n; r++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[rw_idx[r]];
      int which = rw_which[r];
      IROperand old_op = (which == 1)   ? tcc_ir_op_get_src1(ir, cq)
                         : (which == 2) ? tcc_ir_op_get_src2(ir, cq)
                                        : tcc_ir_op_get_dest(ir, cq);
      int32_t folded_off = base_offset + rw_off[r];
      IROperand new_op = irop_make_stackoff(-1, folded_off, /*is_lval*/ 1, /*is_llocal*/ 0,
                                            /*is_param_flag*/ (int)lea_src.is_param, old_op.btype);
      new_op.is_unsigned = old_op.is_unsigned;
      new_op.is_static = lea_src.is_static;
      if (which == 1)
        tcc_ir_op_set_src1(ir, cq, new_op);
      else if (which == 2)
        tcc_ir_op_set_src2(ir, cq, new_op);
      else
        tcc_ir_op_set_dest(ir, cq, new_op);
    }

    lea_q->op = TCCIR_OP_NOP;
    for (int w = 1; w < wl_n; w++)
      ir->compact_instructions[wl_def[w]].op = TCCIR_OP_NOP;

    changes++;
    LOG_IR_GEN("LEA RMW FOLD: LEA@%d base=%d -> %d deref sites, %d interposers", i, base_offset, rw_n,
               wl_n - 1);
  }

  return changes;
}
