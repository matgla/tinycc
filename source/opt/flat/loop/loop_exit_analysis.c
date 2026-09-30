/*
 *  TCC IR - Loop exit condition analysis
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "licm.h"
#include "opt.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"


/* Convert signed comparison condition to unsigned equivalent; -1 if not a simple relational. */

int signed_to_unsigned_cond(int cond_token)
{
  switch (cond_token)
  {
  case 0x9c: /* TOK_LT  → TOK_ULT */
    return 0x92;
  case 0x9d: /* TOK_GE  → TOK_UGE */
    return 0x93;
  case 0x9e: /* TOK_LE  → TOK_ULE */
    return 0x96;
  case 0x9f: /* TOK_GT  → TOK_UGT */
    return 0x97;
  case 0x94: /* TOK_EQ  - same for signed/unsigned */
    return 0x94;
  case 0x95: /* TOK_NE  - same for signed/unsigned */
    return 0x95;
  /* Already unsigned conditions — pass through */
  case 0x92: /* TOK_ULT */
    return 0x92;
  case 0x93: /* TOK_UGE */
    return 0x93;
  case 0x96: /* TOK_ULE */
    return 0x96;
  case 0x97: /* TOK_UGT */
    return 0x97;
  default:
    return -1;
  }
}


int find_loop_exit_condition(TCCIRState *ir, IRLoop *loop, int iv_vreg, int *out_cmp_idx, int *out_jmpif_idx,
                                    int *out_limit, int *out_cond, int *out_exit_target)
{
  int ranges[2][2] = {
      {loop->header_idx, loop->header_idx + 3}, /* top-tested */
      {loop->end_idx - 3, loop->end_idx}        /* bottom-tested (rotated) */
  };

  for (int r = 0; r < 2; r++)
  {
    int scan_start = ranges[r][0];
    int scan_end = ranges[r][1];
    if (scan_start < loop->start_idx)
      scan_start = loop->start_idx;
    if (scan_end > loop->end_idx)
      scan_end = loop->end_idx;

    LOG_LOOP_OPT("find_loop_exit_condition: iv_vreg=VAR%d %s scan [%d..%d]", TCCIR_DECODE_VREG_POSITION(iv_vreg),
                 r == 0 ? "header" : "tail", scan_start, scan_end);

    for (int i = scan_start; i <= scan_end && i < ir->next_instruction_index - 1; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
      {
        LOG_LOOP_OPT("[%d] op=%d (not CMP), skipping", i, cq->op);
        continue;
      }

      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);

      /* Check: CMP Viv, #limit */
      int32_t vr1 = irop_get_vreg(cmp_src1);
      if (vr1 != iv_vreg || !irop_is_immediate(cmp_src2))
      {
        LOG_LOOP_OPT("[%d] CMP but vr1=%d (want %d) imm=%d, skipping", i,
                     vr1 >= 0 ? TCCIR_DECODE_VREG_POSITION(vr1) : -1, TCCIR_DECODE_VREG_POSITION(iv_vreg),
                     irop_is_immediate(cmp_src2));
        continue;
      }

      int limit = (int)irop_get_imm64_ex(ir, cmp_src2);

      /* Next instruction must be JUMPIF */
      IRQuadCompact *jq = &ir->compact_instructions[i + 1];
      if (jq->op != TCCIR_OP_JUMPIF)
      {
        LOG_LOOP_OPT("[%d] CMP ok but [%d] is op=%d (not JUMPIF)", i, i + 1, jq->op);
        continue;
      }

      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int cond = (int)irop_get_imm64_ex(ir, cond_op);

      IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
      int jmp_target = (int)irop_get_imm64_ex(ir, jmp_dest);

      /* Top-tested: exit target is outside the loop */
      if (jmp_target > loop->end_idx)
      {
        LOG_LOOP_OPT("[%d] FOUND top-tested exit: CMP VAR%d,#%d JUMPIF@%d exit=%d cond=%d", i,
                     TCCIR_DECODE_VREG_POSITION(iv_vreg), limit, i + 1, jmp_target, cond);
        *out_cmp_idx = i;
        *out_jmpif_idx = i + 1;
        *out_limit = limit;
        *out_cond = cond;
        *out_exit_target = jmp_target;
        return 1;
      }

      /* Bottom-tested (rotated): JUMPIF is the back-edge, exit is fall-through; invert cond. */
      if (jmp_target >= loop->start_idx && jmp_target <= loop->end_idx)
      {
        int exit_target = i + 2; /* fall-through past the JUMPIF */
        int inv_cond;
        switch (cond)
        {
        case TOK_GE:
          inv_cond = TOK_LT;
          break;
        case TOK_GT:
          inv_cond = TOK_LE;
          break;
        case TOK_LT:
          inv_cond = TOK_GE;
          break;
        case TOK_LE:
          inv_cond = TOK_GT;
          break;
        case TOK_UGE:
          inv_cond = TOK_ULT;
          break;
        case TOK_UGT:
          inv_cond = TOK_ULE;
          break;
        case TOK_ULT:
          inv_cond = TOK_UGE;
          break;
        case TOK_ULE:
          inv_cond = TOK_UGT;
          break;
        case TOK_EQ:
          inv_cond = TOK_NE;
          break;
        case TOK_NE:
          inv_cond = TOK_EQ;
          break;
        default:
          inv_cond = -1;
          break;
        }
        if (inv_cond < 0)
        {
          LOG_LOOP_OPT("[%d] bottom-tested but can't invert cond=%d", i, cond);
          continue;
        }
        LOG_LOOP_OPT("[%d] FOUND bottom-tested exit: CMP VAR%d,#%d JUMPIF@%d back=%d exit=%d cond=%d->%d", i,
                     TCCIR_DECODE_VREG_POSITION(iv_vreg), limit, i + 1, jmp_target, exit_target, cond, inv_cond);
        *out_cmp_idx = i;
        *out_jmpif_idx = i + 1;
        *out_limit = limit;
        *out_cond = inv_cond;
        *out_exit_target = exit_target;
        return 1;
      }

      LOG_LOOP_OPT("[%d] CMP+JUMPIF found but target=%d doesn't match any pattern", i, jmp_target);
    }
  }
  LOG_LOOP_OPT("-> exit condition NOT FOUND");
  return 0;
}

/* Trip count for a loop; -1 if not computable. int64_t range avoids signed overflow. */
int compute_trip_count(int init_val, int limit, int step, int cond_token)
{
  if (step <= 0)
    return -1;

  int64_t range = (int64_t)limit - (int64_t)init_val;

  LOG_LOOP_OPT("compute_trip_count: init=%d limit=%d step=%d cond=%d range=%lld", init_val, limit, step, cond_token,
               (long long)range);

  switch (cond_token)
  {
  case TOK_UGE:
  case TOK_GE: /* exit if iv >= limit → loop while iv < limit */
    if (range <= 0)
      return 0;
    return (int)((range + step - 1) / step);

  case TOK_UGT:
  case TOK_GT: /* exit if iv > limit → loop while iv <= limit */
    if (range < 0)
      return 0;
    return (int)(range / step + 1);

  case TOK_NE: /* exit if iv != limit → loop until iv == limit */
    if (range < 0)
      return -1;
    if (range == 0)
      return 0;
    if (range % step != 0)
      return -1; /* would loop forever */
    return (int)(range / step);

  default:
    return -1;
  }
}

/* Collect body instructions to clone (excluding loop control flow and IV update). */
int collect_body_instructions(TCCIRState *ir, IRLoop *loop, int iv_vreg, int cmp_idx, int jmpif_idx,
                                     int iv_def_idx, int *body_indices, int max_body, int *out_has_memory)
{
  int count = 0;
  if (out_has_memory)
    *out_has_memory = 0;
  /* Scan the full [start..end]; truncating the body would unroll wrong code. */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Skip CMP and JUMPIF (exit condition) */
    if (i == cmp_idx || i == jmpif_idx)
      continue;

    /* Skip all unconditional jumps (loop structure) */
    if (q->op == TCCIR_OP_JUMP)
      continue;

    /* Skip IV increment */
    if (i == iv_def_idx)
      continue;

    /* The ASSIGN saving the old IV for a post-increment (T = Viv) stays in
     * the body: every copy makes it `T = #k`, and a body that reads T -- the
     * `buf[i++] = arg` store of 990525-1 -- needs it (dead copies go to DCE). */

    /* Reject bodies with internal branches */
    if (q->op == TCCIR_OP_JUMPIF)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] internal JUMPIF", i);
      return -1;
    }

    /* Reject bodies with calls (side effects) */
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCPARAMVAL ||
        q->op == TCCIR_OP_FUNCPARAMVOID)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] function call op=%d", i, q->op);
      return -1;
    }

    if (q->op == TCCIR_OP_INLINE_ASM)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] inline asm", i);
      return -1;
    }

    /* Plain loads and stores (and their indexed forms) clone in order, so the
     * unrolled body touches memory exactly as the loop did; the caller counts
     * them against a smaller size cap.  Post-increment forms update their
     * pointer as a side effect and a BLOCK_COPY's size may be a whole object:
     * those stay loops. */
    if (q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_BLOCK_COPY)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] memory access op=%d", i, q->op);
      return -1;
    }
    if (out_has_memory &&
        (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_LOAD_INDEXED ||
         q->op == TCCIR_OP_STORE_INDEXED ||
         (irop_config[q->op].has_src1 && ir_xform_operand_reads_memory(tcc_ir_op_get_src1(ir, q))) ||
         (irop_config[q->op].has_src2 && ir_xform_operand_reads_memory(tcc_ir_op_get_src2(ir, q))) ||
         (tcc_ir_op_is_mac(q->op) && ir_xform_operand_reads_memory(tcc_ir_op_get_accum(ir, q)))))
      *out_has_memory = 1;

    /* `V <- mem [STORE]` (an inlined parameter bound to a load) stays one
     * in-place-renamed multi-def TEMP in SSA (ssa_store_slot_def_pos), which
     * is sound for the single copy a loop body has, not for N copies of it in
     * straight-line code: later SSA folds took every copy's value from the
     * first (fuzz 308/191/212 family). */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && !d.is_llocal &&
          ir_xform_operand_reads_memory(tcc_ir_op_get_src1(ir, q)))
      {
        LOG_LOOP_OPT("collect_body: REJECTED at [%d] memory load bound to a VAR by STORE", i);
        return -1;
      }
    }

    /* A SELECT's 4th slot is its condition token; the others the caller copies. */
    if (q->op == TCCIR_OP_SELECT)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] 4-operand op=%d", i, q->op);
      return -1;
    }

    /* Reject over-cap body: truncating would unroll wrong code. */
    if (count >= max_body)
    {
      LOG_LOOP_OPT("collect_body: REJECTED body exceeds max_body=%d", max_body);
      return -1;
    }

    LOG_LOOP_OPT("collect_body: body[%d] = instr [%d] op=%d", count, i, q->op);
    body_indices[count++] = i;
  }

  LOG_LOOP_OPT("collect_body: collected %d body instruction(s)", count);
  return count;
}

/* Find loop exit condition with a possibly-symbolic limit operand (vreg or immediate). */
int find_loop_exit_condition_op(TCCIRState *ir, IRLoop *loop, int iv_vreg, int *out_cmp_idx,
                                       int *out_jmpif_idx, IROperand *out_limit_op, int *out_cond,
                                       int *out_exit_target)
{
  int ranges[2][2] = {
      {loop->header_idx, loop->header_idx + 3},
      {loop->end_idx - 3, loop->end_idx},
  };
  for (int r = 0; r < 2; r++)
  {
    int scan_start = ranges[r][0];
    int scan_end = ranges[r][1];
    if (scan_start < loop->start_idx)
      scan_start = loop->start_idx;
    if (scan_end > loop->end_idx)
      scan_end = loop->end_idx;
    for (int i = scan_start; i <= scan_end && i < ir->next_instruction_index - 1; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
        continue;
      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);
      int32_t vr1 = irop_get_vreg(cmp_src1);
      if (vr1 != iv_vreg)
        continue;
      /* Accept either immediate OR a plain vreg (no DEREF/sym/complex). */
      int src2_is_imm = irop_is_immediate(cmp_src2);
      int src2_is_simple_vreg =
          (!cmp_src2.is_lval && !cmp_src2.is_sym && !cmp_src2.is_complex && irop_get_vreg(cmp_src2) >= 0);
      if (!src2_is_imm && !src2_is_simple_vreg)
        continue;
      IRQuadCompact *jq = &ir->compact_instructions[i + 1];
      if (jq->op != TCCIR_OP_JUMPIF)
        continue;
      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int cond = (int)irop_get_imm64_ex(ir, cond_op);
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
      int jmp_target = (int)irop_get_imm64_ex(ir, jmp_dest);
      if (jmp_target > loop->end_idx)
      {
        *out_cmp_idx = i;
        *out_jmpif_idx = i + 1;
        *out_limit_op = cmp_src2;
        *out_cond = cond;
        *out_exit_target = jmp_target;
        return 1;
      }
      if (jmp_target >= loop->start_idx && jmp_target <= loop->end_idx)
      {
        int exit_target = i + 2;
        int inv_cond;
        switch (cond)
        {
        case TOK_GE: inv_cond = TOK_LT; break;
        case TOK_GT: inv_cond = TOK_LE; break;
        case TOK_LT: inv_cond = TOK_GE; break;
        case TOK_LE: inv_cond = TOK_GT; break;
        case TOK_EQ: inv_cond = TOK_NE; break;
        case TOK_NE: inv_cond = TOK_EQ; break;
        default: inv_cond = -1; break;
        }
        if (inv_cond < 0)
          continue;
        *out_cmp_idx = i;
        *out_jmpif_idx = i + 1;
        *out_limit_op = cmp_src2;
        *out_cond = inv_cond;
        *out_exit_target = exit_target;
        return 1;
      }
    }
  }
  return 0;
}
