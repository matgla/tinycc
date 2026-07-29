/*
 *  TCC IR - double->float library-call narrowing (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

typedef struct
{
  const char *double_name;
  const char *float_name;
} FloatNarrowEntry;

static const FloatNarrowEntry float_narrow_table[] = {
    {"floor", "floorf"}, {"ceil", "ceilf"},           {"trunc", "truncf"}, {"round", "roundf"},
    {"fabs", "fabsf"},   {"nearbyint", "nearbyintf"}, {"rint", "rintf"},
};
#define NUM_FLOAT_NARROW (sizeof(float_narrow_table) / sizeof(float_narrow_table[0]))

typedef struct
{
  int param_idx;  /* instruction index of the FUNCPARAMVAL */
  int call_idx;   /* instruction index of the FUNCCALLVAL */
  int32_t src_vr; /* original source vreg (float for f2d, double for d2f) */
  int32_t dst_vr;
} ConvCallInfo;

#define MAX_CONV_CALLS 32

static void capture_pending_param(TCCIRState *ir, IRQuadCompact *q, int i, int *pending_param_idx,
                                  int32_t *pending_param_src_vr, int *pending_param_call_id)
{
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
  int param_idx_val = TCCIR_DECODE_PARAM_IDX(encoded);

  if (param_idx_val == 0)
  {
    *pending_param_idx = i;
    *pending_param_src_vr = irop_is_immediate(src1) ? -1 : irop_get_vreg(src1);
    *pending_param_call_id = TCCIR_DECODE_CALL_ID(encoded);
  }
}

static void record_conv_call(ConvCallInfo *arr, int *count, int param_idx, int call_idx, int32_t src_vr,
                             int32_t dst_vr)
{
  if (*count < MAX_CONV_CALLS)
  {
    arr[*count].param_idx = param_idx;
    arr[*count].call_idx = call_idx;
    arr[*count].src_vr = src_vr;
    arr[*count].dst_vr = dst_vr;
    (*count)++;
  }
}

int tcc_ir_opt_float_narrowing(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
    return 0;

  ConvCallInfo f2d_calls[MAX_CONV_CALLS];
  ConvCallInfo d2f_calls[MAX_CONV_CALLS];
  int num_f2d = 0, num_d2f = 0;


  int pending_param_idx = -1;
  int32_t pending_param_src_vr = -1;
  int pending_param_call_id = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      capture_pending_param(ir, q, i, &pending_param_idx, &pending_param_src_vr, &pending_param_call_id);
      continue;
    }

    if (q->op == TCCIR_OP_FUNCCALLVAL && pending_param_idx >= 0)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      Sym *callee = irop_get_sym_ex(ir, src1);
      if (!callee)
      {
        pending_param_idx = -1;
        continue;
      }

      const char *name = get_tok_str(callee->v, NULL);
      if (!name)
      {
        pending_param_idx = -1;
        continue;
      }

      uint32_t call_encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
      int this_call_id = TCCIR_DECODE_CALL_ID(call_encoded);

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dst_vr = irop_get_vreg(dest);

      if (strcmp(name, "__aeabi_f2d") == 0 && this_call_id == pending_param_call_id)
      {
        record_conv_call(f2d_calls, &num_f2d, pending_param_idx, i, pending_param_src_vr, dst_vr);
      }
      else if (strcmp(name, "__aeabi_d2f") == 0 && this_call_id == pending_param_call_id)
      {
        record_conv_call(d2f_calls, &num_d2f, pending_param_idx, i, pending_param_src_vr, dst_vr);
      }

      pending_param_idx = -1;
      continue;
    }

    if (q->op != TCCIR_OP_NOP)
      pending_param_idx = -1;
  }

  if (num_f2d == 0)
    return 0;


  pending_param_idx = -1;
  pending_param_src_vr = -1;
  pending_param_call_id = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      capture_pending_param(ir, q, i, &pending_param_idx, &pending_param_src_vr, &pending_param_call_id);
      continue;
    }

    if (q->op != TCCIR_OP_FUNCCALLVAL || pending_param_idx < 0)
    {
      if (q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_FUNCPARAMVOID)
        pending_param_idx = -1;
      continue;
    }

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    Sym *callee = irop_get_sym_ex(ir, src1);
    if (!callee)
    {
      pending_param_idx = -1;
      continue;
    }

    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
    {
      pending_param_idx = -1;
      continue;
    }

    const char *float_name = NULL;
    for (size_t j = 0; j < NUM_FLOAT_NARROW; j++)
    {
      if (strcmp(name, float_narrow_table[j].double_name) == 0)
      {
        float_name = float_narrow_table[j].float_name;
        break;
      }
    }

    if (!float_name)
    {
      pending_param_idx = -1;
      continue;
    }

    /* Never narrow into a self-call.  Inside float_name's OWN definition —
     * libm's `float ceilf(float x) { return (float)ceil((double)x); }` — the
     * rewrite turns the wrapper into `return ceilf(x);`, which
     * infinite_self_recursion then (rightly, by then) collapses into a `b .`
     * self-loop: every ceilf/floorf/roundf/truncf call on the device hung. */
    if (funcname && strcmp(funcname, float_name) == 0)
    {
      pending_param_idx = -1;
      continue;
    }

    ConvCallInfo *f2d_info = NULL;
    for (int k = 0; k < num_f2d; k++)
    {
      if (f2d_calls[k].dst_vr == pending_param_src_vr)
      {
        f2d_info = &f2d_calls[k];
        break;
      }
    }

    if (!f2d_info)
    {
      pending_param_idx = -1;
      continue;
    }

    IROperand func_dest = tcc_ir_op_get_dest(ir, q);
    int32_t func_result_vr = irop_get_vreg(func_dest);
    int func_call_idx = i;
    int func_param_idx = pending_param_idx;

    ConvCallInfo *d2f_info = NULL;
    for (int k = 0; k < num_d2f; k++)
    {
      if (d2f_calls[k].src_vr == func_result_vr)
      {
        d2f_info = &d2f_calls[k];
        break;
      }
    }

    if (d2f_info)
    {

      IROperand orig_float_param = tcc_ir_op_get_src1(ir, &ir->compact_instructions[f2d_info->param_idx]);
      tcc_ir_set_src1(ir, func_param_idx, orig_float_param);

      change_callee_sym(ir, func_call_idx, float_name, VT_FLOAT);

      IROperand d2f_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[d2f_info->call_idx]);
      tcc_ir_set_dest(ir, func_call_idx, d2f_dest);

      ir->compact_instructions[f2d_info->param_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[f2d_info->call_idx].op = TCCIR_OP_NOP;

      ir->compact_instructions[d2f_info->param_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[d2f_info->call_idx].op = TCCIR_OP_NOP;

      LOG_IR_GEN("FLOAT NARROW (Case 1): %s → %s at i=%d, NOP'd f2d@%d and d2f@%d", name, float_name, func_call_idx,
                 f2d_info->call_idx, d2f_info->call_idx);
      changes++;
    }
    else
    {

      change_callee_sym(ir, f2d_info->call_idx, float_name, VT_FLOAT);

      change_callee_sym(ir, func_call_idx, "__aeabi_f2d", VT_INT);

      LOG_IR_GEN("FLOAT NARROW (Case 2): swapped %s↔f2d at i=%d,%d", name, f2d_info->call_idx, func_call_idx);
      changes++;
    }

    /* Invalidate modified f2d entry to prevent double-processing */
    f2d_info->dst_vr = -1;

    pending_param_idx = -1;
  }

  return changes;
}
int tcc_ir_opt_float_narrowing_ex(IROptCtx *ctx) { return tcc_ir_opt_float_narrowing(ctx->ir); }
