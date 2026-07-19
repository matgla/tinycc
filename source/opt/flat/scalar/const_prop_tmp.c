/*
 *  TCC IR - Block-local constant propagation (shared flat + SSA core)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Shared core for const_prop_tmp (flat + SSA wrappers); Branch A, see docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "memory/small_sequence.h"

typedef struct
{
  int gen;
  int64_t value;
} TmpConstInfo;

/* Inline-first scratch (caps match the old hand-rolled stack-then-heap split). */
TCC_SMALL_SEQUENCE_DEFINE(TmpInfoSeq, TmpConstInfo, 64)
TCC_SMALL_SEQUENCE_DEFINE(CptIntSeq, int, 256)
TCC_SMALL_SEQUENCE_DEFINE(CptByteSeq, uint8_t, 64)

typedef struct
{
  TCCIRState *ir;
  TmpConstInfo *tmp_info;
  TmpConstInfo *var_info;
  uint8_t *var_addrtaken;
  int max_tmp_pos;
  int max_var_pos;
  int current_gen;
  int n;
} CPTCtx;

static int64_t ir_opt_fit_const_to_operand(int64_t val, IROperand op)
{
  switch (irop_get_btype(op))
  {
  case IROP_BTYPE_INT8:
    return op.is_unsigned ? (int64_t)(uint8_t)val : (int64_t)(int8_t)val;
  case IROP_BTYPE_INT16:
    return op.is_unsigned ? (int64_t)(uint16_t)val : (int64_t)(int16_t)val;
  case IROP_BTYPE_INT32:
    return op.is_unsigned ? (int64_t)(uint32_t)val : (int64_t)(int32_t)val;
  default:
    return val;
  }
}

static IROperand cpt_make_const(TCCIRState *ir, int64_t val, int btype)
{
  if (val == (int32_t)val)
    return irop_make_imm32(-1, (int32_t)val, btype);
  uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
  return irop_make_i64(-1, pool_idx, btype);
}

static int cpt_operand_const(CPTCtx *c, IROperand op, int64_t *out)
{
  int32_t vr = irop_get_vreg(op);
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos <= c->max_tmp_pos && c->tmp_info[pos].gen == c->current_gen)
    {
      *out = c->tmp_info[pos].value;
      return 1;
    }
    return 0;
  }
  if (c->max_var_pos >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos <= c->max_var_pos && c->var_info[pos].gen == c->current_gen)
    {
      *out = c->var_info[pos].value;
      return 1;
    }
  }
  return 0;
}

static void cpt_scan_max_positions(TCCIRState *ir, int n, int *max_tmp, int *max_var)
{
  int mt = 0, mv = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!irop_config[q->op].has_dest)
      continue;
    int32_t dest_vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      if (pos > mt)
        mt = pos;
    }
    else if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
    {
      if (pos > mv)
        mv = pos;
    }
  }
  *max_tmp = mt;
  *max_var = mv;
}

static void cpt_mark_addrtaken(TCCIRState *ir, int n, int max_var, uint8_t *var_addrtaken)
{
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA)
      continue;
    int32_t sv = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
    if (sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_VAR)
    {
      int vp = TCCIR_DECODE_VREG_POSITION(sv);
      if (vp <= max_var)
        var_addrtaken[vp / 8] |= (1 << (vp % 8));
    }
  }
  /* Volatile VARs must never be tracked as holding a constant: exclude them
   * the same way as address-taken VARs so their loads stay real. */
  for (int vp = 0; vp <= max_var && vp < ir->variables_live_intervals_size; vp++)
    if (ir->variables_live_intervals[vp].is_volatile)
      var_addrtaken[vp / 8] |= (1 << (vp % 8));
}

static int cpt_is_block_boundary(int op)
{
  return op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF || op == TCCIR_OP_FUNCCALLVOID ||
         op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID;
}

static int cpt_try_switch_table_fold(CPTCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  int32_t src1_vr = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
  if (TCCIR_DECODE_VREG_TYPE(src1_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
  if (pos > c->max_tmp_pos || c->tmp_info[pos].gen != c->current_gen)
    return 0;
  int64_t index_val = c->tmp_info[pos].value;
  int table_id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
  if (table_id < 0 || table_id >= ir->num_switch_tables)
    return 0;
  TCCIRSwitchTable *table = &ir->switch_tables[table_id];
  int target;
  if (index_val >= 0 && index_val < table->num_entries)
    target = table->targets[(int)index_val];
  else
    target = table->default_target;
  LOG_IR_GEN("OPTIMIZE: Constant SWITCH_TABLE index=%lld -> JUMP to %d", (long long)index_val, target);
  q->op = TCCIR_OP_JUMP;
  tcc_ir_set_dest(ir, i, irop_make_imm32(-1, target, 0));
  tcc_ir_set_src1(ir, i, IROP_NONE);
  tcc_ir_set_src2(ir, i, IROP_NONE);
  c->current_gen++;
  return 1;
}

static int cpt_propagate_src1(CPTCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (!irop_config[q->op].has_src1 || q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_IJUMP)
    return 0;
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src1)) == TCCIR_VREG_TYPE_VAR && src1.is_local && !src1.is_lval)
    return 0;
  int64_t prop_val;
  if (!cpt_operand_const(c, src1, &prop_val))
    return 0;
  /* CMP src1<-const inverts (const - src2); only fold when src2 is const too (order becomes irrelevant) */
  if (q->op == TCCIR_OP_CMP)
  {
    IROperand cmp_s2 = tcc_ir_op_get_src2(ir, q);
    int64_t s2v;
    if (!irop_is_immediate(cmp_s2))
    {
      int32_t s2_vr = irop_get_vreg(cmp_s2);
      if (s2_vr < 0 || !cpt_operand_const(c, cmp_s2, &s2v))
        return 0;
    }
  }
  int btype = irop_get_btype(src1);
  prop_val = ir_opt_fit_const_to_operand(prop_val, src1);
  IROperand new_src1 = cpt_make_const(ir, prop_val, btype);
  new_src1.is_unsigned = src1.is_unsigned;
  new_src1.is_static = src1.is_static;
  tcc_ir_set_src1(ir, i, new_src1);
  return 1;
}

static int cpt_propagate_src2(CPTCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (!irop_config[q->op].has_src2)
    return 0;
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  int64_t prop_val;
  if (!cpt_operand_const(c, src2, &prop_val))
    return 0;
  LOG_IR_GEN("OPTIMIZE: const propagate vreg %d = %lld to src2 at i=%d", irop_get_vreg(src2), (long long)prop_val, i);
  int btype = irop_get_btype(src2);
  int64_t val = ir_opt_fit_const_to_operand(prop_val, src2);
  /* widen narrow const to zero-extended INT64 for INT64 bitwise ops so codegen doesn't sign-extend into the high reg */
  if (irop_get_btype(tcc_ir_op_get_src1(ir, q)) == IROP_BTYPE_INT64 && btype != IROP_BTYPE_INT64 &&
      (q->op == TCCIR_OP_OR || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_XOR))
  {
    val = (int64_t)(uint32_t)val;
    btype = IROP_BTYPE_INT64;
  }
  IROperand new_src2 = cpt_make_const(ir, val, btype);
  new_src2.is_unsigned = src2.is_unsigned;
  new_src2.is_static = src2.is_static;
  tcc_ir_set_src2(ir, i, new_src2);
  return 1;
}

static int cpt_try_fold_binop(CPTCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (!irop_config[q->op].has_src1 || !irop_config[q->op].has_src2)
    return 0;
  IROperand fs1 = tcc_ir_op_get_src1(ir, q);
  IROperand fs2 = tcc_ir_op_get_src2(ir, q);
  if (!irop_is_immediate(fs1) || !irop_is_immediate(fs2))
    return 0;
  int64_t v1 = irop_get_imm64_ex(ir, fs1);
  int64_t v2 = irop_get_imm64_ex(ir, fs2);
  int btype = irop_get_btype(fs1);
  int64_t res = 0;
  int ok = 1;
  switch (q->op)
  {
  case TCCIR_OP_ADD:
    res = (int64_t)((uint64_t)v1 + (uint64_t)v2);
    break;
  case TCCIR_OP_SUB:
    res = (int64_t)((uint64_t)v1 - (uint64_t)v2);
    break;
  case TCCIR_OP_AND:
    res = v1 & v2;
    break;
  case TCCIR_OP_OR:
    res = v1 | v2;
    break;
  case TCCIR_OP_XOR:
    res = v1 ^ v2;
    break;
  case TCCIR_OP_SHL:
    res = (int64_t)((uint64_t)v1 << v2);
    break;
  case TCCIR_OP_SHR:
    if (btype == IROP_BTYPE_INT64)
      res = (int64_t)((uint64_t)v1 >> v2);
    else
      res = (int64_t)((uint32_t)v1 >> v2);
    break;
  case TCCIR_OP_SAR:
    res = v1 >> v2;
    break;
  case TCCIR_OP_ROR:
  {
    uint32_t v = (uint32_t)v1;
    uint32_t sh = (uint32_t)v2 & 31;
    res = (int64_t)(int32_t)((v >> sh) | (v << (32 - sh)));
    break;
  }
  case TCCIR_OP_MUL:
    res = (int64_t)((uint64_t)v1 * (uint64_t)v2);
    break;
  case TCCIR_OP_UMULL:
    res = (int64_t)((uint64_t)(uint32_t)v1 * (uint64_t)(uint32_t)v2);
    btype = IROP_BTYPE_INT64;
    break;
  case TCCIR_OP_SMULL:
    res = (int64_t)(int32_t)v1 * (int64_t)(int32_t)v2;
    btype = IROP_BTYPE_INT64;
    break;
  case TCCIR_OP_UBFX:
  {
    int lsb = (int)v2 & 0x1F;
    int width = ((int)v2 >> 5) & 0x1F;
    if (width > 0 && width <= 32)
      res = ((uint32_t)v1 >> lsb) & ((1u << width) - 1);
    else
      ok = 0;
    break;
  }
  case TCCIR_OP_DIV:
  case TCCIR_OP_PDIV:
    /* INT_MIN / -1 traps; width-specific since a 32-bit v1 arrives sign-extended in this i64 slot */
    if (v2 == 0)
      ok = 0;
    else if (v2 == -1 && ((btype == IROP_BTYPE_INT64 && v1 == INT64_MIN) ||
                          (btype != IROP_BTYPE_INT64 && (int32_t)v1 == INT32_MIN)))
      ok = 0;
    else if (btype == IROP_BTYPE_INT64)
      res = v1 / v2;
    else
      res = (int64_t)((int32_t)v1 / (int32_t)v2);
    break;
  case TCCIR_OP_UDIV:
    if (v2 == 0)
      ok = 0;
    else if (btype == IROP_BTYPE_INT64)
      res = (int64_t)((uint64_t)v1 / (uint64_t)v2);
    else
      res = (int64_t)((uint32_t)v1 / (uint32_t)v2);
    break;
  case TCCIR_OP_IMOD:
    if (v2 == 0)
      ok = 0;
    else if (v2 == -1 && ((btype == IROP_BTYPE_INT64 && v1 == INT64_MIN) ||
                          (btype != IROP_BTYPE_INT64 && (int32_t)v1 == INT32_MIN)))
      ok = 0;
    else if (btype == IROP_BTYPE_INT64)
      res = v1 % v2;
    else
      res = (int64_t)((int32_t)v1 % (int32_t)v2);
    break;
  case TCCIR_OP_UMOD:
    if (v2 == 0)
      ok = 0;
    else if (btype == IROP_BTYPE_INT64)
      res = (int64_t)((uint64_t)v1 % (uint64_t)v2);
    else
      res = (int64_t)((uint32_t)v1 % (uint32_t)v2);
    break;
  default:
    ok = 0;
    break;
  }
  if (ok && btype != IROP_BTYPE_INT64 && btype != IROP_BTYPE_FLOAT64)
  {
    if (q->op == TCCIR_OP_SHL && v2 >= 32)
    {
      if (irop_get_btype(tcc_ir_op_get_dest(ir, q)) == IROP_BTYPE_INT64)
        btype = IROP_BTYPE_INT64;
      else
        ok = 0;
    }
    else
      res = (int64_t)(int32_t)(uint32_t)res;
  }
  if (!ok)
    return 0;
  q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(ir, i, cpt_make_const(ir, res, btype));
  tcc_ir_set_src2(ir, i, IROP_NONE);
  return 1;
}

static int cpt_try_cmp_setif_fold(CPTCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (q->op != TCCIR_OP_CMP || i + 1 >= c->n)
    return 0;
  IRQuadCompact *next_q = &ir->compact_instructions[i + 1];
  if (next_q->op != TCCIR_OP_SETIF)
    return 0;
  IROperand cs1 = tcc_ir_op_get_src1(ir, q);
  IROperand cs2 = tcc_ir_op_get_src2(ir, q);
  if (!irop_is_immediate(cs1) || !irop_is_immediate(cs2))
    return 0;
  int64_t cv1 = irop_get_imm64_ex(ir, cs1);
  int64_t cv2 = irop_get_imm64_ex(ir, cs2);
  IROperand setif_src1 = tcc_ir_op_get_src1(ir, next_q);
  int cond = (int)irop_get_imm64_ex(ir, setif_src1);
  int result = evaluate_compare_condition_cmp_operands(cv1, cv2, cond, cs1, cs2);
  if (result < 0)
    return 0;
  q->op = TCCIR_OP_NOP;
  next_q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, irop_get_btype(setif_src1)));
  tcc_ir_set_src2(ir, i + 1, IROP_NONE);
  return 1;
}

/* Fold __aeabi_c[df]cmp[le|eq] VOID call + following JUMPIF/SETIF when both PARAM args are immediate. */
static int cpt_try_softfp_cmp_fold(CPTCtx *c, int i, int *out_continue)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  *out_continue = 0;
  if (q->op != TCCIR_OP_FUNCCALLVOID || i + 1 >= c->n)
    return 0;
  IRQuadCompact *next_q = &ir->compact_instructions[i + 1];
  if (next_q->op != TCCIR_OP_JUMPIF && next_q->op != TCCIR_OP_SETIF)
    return 0;
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!callee)
    return 0;
  const char *fn = get_tok_str(callee->v, NULL);
  int is_fcmp = fn && (strcmp(fn, "__aeabi_cfcmple") == 0 || strcmp(fn, "__aeabi_cfcmpeq") == 0);
  int is_dcmp = fn && (strcmp(fn, "__aeabi_cdcmple") == 0 || strcmp(fn, "__aeabi_cdcmpeq") == 0);
  if (!is_fcmp && !is_dcmp)
    return 0;
  IROperand arg0, arg1;
  if (!ir_opt_get_call_param_operand(ir, i, 0, &arg0) || !ir_opt_get_call_param_operand(ir, i, 1, &arg1))
    return 0;
  if (!irop_is_immediate(arg0) || !irop_is_immediate(arg1))
    return 0;
  int64_t a0 = irop_get_imm64_ex(ir, arg0);
  int64_t a1 = irop_get_imm64_ex(ir, arg1);
  int is_nan;
  int cmp_result = ir_softfp_cmp3(is_dcmp, a0, a1, &is_nan);
  IROperand cond = tcc_ir_op_get_src1(ir, next_q);
  int tok = (int)irop_get_imm64_ex(ir, cond);
  int result = is_nan ? nan_compare_branch_result(tok) : evaluate_compare_condition(cmp_result, 0, tok);
  if (result < 0)
    return 0;
  ir_opt_nop_call_params(ir, i);
  q->op = TCCIR_OP_NOP;
  *out_continue = 1;
  if (next_q->op == TCCIR_OP_JUMPIF)
  {
    if (result)
    {
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, next_q);
      next_q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, i + 1, jmp_dest);
    }
    else
      next_q->op = TCCIR_OP_NOP;
    c->current_gen++;
    return 1;
  }
  next_q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, irop_get_btype(cond)));
  tcc_ir_set_src2(ir, i + 1, IROP_NONE);
  /* bump gen before recording so the new const survives this call's block boundary */
  c->current_gen++;
  int32_t dv = irop_get_vreg(tcc_ir_op_get_dest(ir, next_q));
  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
  {
    int dp = TCCIR_DECODE_VREG_POSITION(dv);
    if (dp <= c->max_tmp_pos)
    {
      c->tmp_info[dp].gen = c->current_gen;
      c->tmp_info[dp].value = result;
    }
  }
  return 1;
}

/* A redefined TEMP drops its const: loop-unrolled body-local temps are multi-def (volatile fuzz seed 8310).
 * STORE/STORE_INDEXED lvalue dests and FUNCPARAM dests are uses, not defs, and leave the entry alone. */
static void cpt_track_tmp_def(CPTCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t dest_vr = irop_get_vreg(dest);
  if (!irop_config[q->op].has_dest || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP ||
      q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID ||
      ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) && dest.is_lval))
    return;
  int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
  if (pos > c->max_tmp_pos)
    return;
  IROperand cur_src1 = tcc_ir_op_get_src1(ir, q);
  if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_CVT_FTOF) && irop_is_immediate(cur_src1))
  {
    c->tmp_info[pos].gen = c->current_gen;
    c->tmp_info[pos].value = ir_opt_fit_const_to_operand(irop_get_imm64_ex(ir, cur_src1), dest);
  }
  else
    c->tmp_info[pos].gen = 0;
}

/* Multi-def VAR tracking: keep the most recent constant, invalidate on non-const redefinition. */
static void cpt_track_var_def(CPTCtx *c, int i)
{
  if (c->max_var_pos < 0)
    return;
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t dest_vr = irop_get_vreg(dest);
  if (!irop_config[q->op].has_dest || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_VAR)
    return;
  int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
  if (pos > c->max_var_pos || (c->var_addrtaken[pos / 8] & (1 << (pos % 8))))
    return;
  if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) && !dest.is_lval)
  {
    IROperand cur_src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_is_immediate(cur_src1) && !cur_src1.is_sym)
    {
      c->var_info[pos].gen = c->current_gen;
      c->var_info[pos].value = ir_opt_fit_const_to_operand(irop_get_imm64_ex(ir, cur_src1), dest);
    }
    else
      c->var_info[pos].gen = 0;
  }
  else
    c->var_info[pos].gen = 0;
}

int tcc_ir_opt_const_prop_tmp_core(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  int max_tmp_pos, max_var_pos;
  cpt_scan_max_positions(ir, n, &max_tmp_pos, &max_var_pos);
  if (max_tmp_pos == 0 && max_var_pos < 0)
    return 0;

  small_sequence(TmpInfoSeq) tmp_info_owner = {0};
  small_sequence(CptIntSeq) block_start_owner = {0};
  if (TmpInfoSeq_init(&tmp_info_owner, (size_t)(max_tmp_pos + 1)) != 0 ||
      CptIntSeq_init(&block_start_owner, (size_t)n) != 0)
    return 0;
  TmpConstInfo *tmp_info = TmpInfoSeq_data(&tmp_info_owner);
  int *block_start_seen = CptIntSeq_data(&block_start_owner);

  small_sequence(TmpInfoSeq) var_info_owner = {0};
  small_sequence(CptByteSeq) var_addrtaken_owner = {0};
  TmpConstInfo *var_info = NULL;
  uint8_t *var_addrtaken = NULL;
  if (max_var_pos >= 0)
  {
    TmpInfoSeq_init(&var_info_owner, (size_t)(max_var_pos + 1));
    CptByteSeq_init(&var_addrtaken_owner, (size_t)((max_var_pos + 8) / 8));
    var_info = TmpInfoSeq_data(&var_info_owner);
    var_addrtaken = CptByteSeq_data(&var_addrtaken_owner);
    cpt_mark_addrtaken(ir, n, max_var_pos, var_addrtaken);
  }

  int block_start_gen = 1;
  ir_opt_mark_block_starts(ir, block_start_seen, block_start_gen, n);

  CPTCtx ctx;
  ctx.ir = ir;
  ctx.tmp_info = tmp_info;
  ctx.var_info = var_info;
  ctx.var_addrtaken = var_addrtaken;
  ctx.max_tmp_pos = max_tmp_pos;
  ctx.max_var_pos = max_var_pos;
  ctx.current_gen = 1;
  ctx.n = n;

  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (i != 0 && block_start_seen[i] == block_start_gen)
      ctx.current_gen++;

    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_SWITCH_TABLE && cpt_try_switch_table_fold(&ctx, i))
    {
      changes++;
      continue;
    }

    changes += cpt_propagate_src1(&ctx, i);
    changes += cpt_propagate_src2(&ctx, i);
    changes += cpt_try_fold_binop(&ctx, i);
    changes += cpt_try_cmp_setif_fold(&ctx, i);

    int did_continue = 0;
    changes += cpt_try_softfp_cmp_fold(&ctx, i, &did_continue);
    if (did_continue)
      continue;

    if (cpt_is_block_boundary(q->op))
    {
      ctx.current_gen++;
      continue;
    }

    cpt_track_tmp_def(&ctx, i);
    cpt_track_var_def(&ctx, i);
  }

  return changes;
}

int tcc_ir_opt_const_prop_tmp(TCCIRState *ir)
{
  if (tcc_ir_opt_pass_disabled("const_prop_tmp")) return 0;
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_const_prop_tmp_core(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_const_prop_tmp_core(ir);
  tcc_pass_timing_add("const_prop_tmp", tcc_pass_clk_us() - _t);
  return _r;
}
