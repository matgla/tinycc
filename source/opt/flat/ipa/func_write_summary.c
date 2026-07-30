/*
 *  TCC IR - per-function must-write byte ranges through pointer params
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"



#include "func_write_summary.h"

typedef struct FwsEntry
{
  Sym *sym;
  FuncWriteSummary summary;
  struct FwsEntry *next;
} FwsEntry;

static FwsEntry *fws_head = NULL;

FuncWriteSummary *fws_lookup(Sym *sym)
{
  for (FwsEntry *e = fws_head; e; e = e->next)
    if (e->sym == sym)
      return &e->summary;
  return NULL;
}

static FuncWriteSummary *fws_create(Sym *sym)
{
  FwsEntry *e = tcc_mallocz(sizeof(*e));
  e->sym = sym;
  e->next = fws_head;
  fws_head = e;
  return &e->summary;
}

void tcc_ir_func_write_summary_clear_all(void)
{
  while (fws_head)
  {
    FwsEntry *n = fws_head->next;
    tcc_free(fws_head);
    fws_head = n;
  }
}

static FwsParamSummary *fws_get_param(FuncWriteSummary *s, int param_idx)
{
  for (int i = 0; i < s->num_params; i++)
    if (s->params[i].param_idx == param_idx)
      return &s->params[i];
  if (s->num_params >= FWS_MAX_PARAMS)
    return NULL;
  FwsParamSummary *ps = &s->params[s->num_params++];
  ps->param_idx = param_idx;
  memset(ps->must_write, 0, sizeof(ps->must_write));
  return ps;
}

int fws_range_fully_set(const FwsParamSummary *ps, int offset, int size)
{
  if (offset < 0 || size <= 0 || offset + size > FWS_MAX_BYTES)
    return 0;
  for (int i = 0; i < size; i++)
  {
    int b = offset + i;
    if (!(ps->must_write[b >> 3] & (uint8_t)(1u << (b & 7))))
      return 0;
  }
  return 1;
}

int fws_btype_bytes(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 0;
  }
}

void tcc_ir_compute_func_write_summary(TCCIRState *ir, Sym *func_sym)
{
  if (!ir || !func_sym)
    return;
  if (fws_lookup(func_sym))
    return; /* Already computed (e.g. function compiled twice). */

  const int n = ir->next_instruction_index;
  if (n == 0)
    return;

  /* Pre-scan max vreg positions per type to size the maps below. */
  int max_tmp = 0, max_var = 0, max_par = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : (IROperand){0};
    ops[1] = irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : (IROperand){0};
    ops[2] = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : (IROperand){0};
    for (int k = 0; k < 3; k++)
    {
      int32_t vr = irop_get_vreg(ops[k]);
      if (vr < 0)
        continue;
      int t = TCCIR_DECODE_VREG_TYPE(vr);
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (t == TCCIR_VREG_TYPE_TEMP && p > max_tmp)
        max_tmp = p;
      else if (t == TCCIR_VREG_TYPE_VAR && p > max_var)
        max_var = p;
      else if (t == TCCIR_VREG_TYPE_PARAM && p > max_par)
        max_par = p;
    }
  }

  const int tmp_base = 0;
  const int var_base = max_tmp + 1;
  const int par_base = var_base + max_var + 1;
  const int total = par_base + max_par + 1;
  if (total <= 0)
    return;

  /* Per vreg: (param_idx, offset); param_idx = -1 means not tracked. */
  int8_t *vp = tcc_malloc((size_t)total);
  int32_t *vo = tcc_malloc((size_t)total * sizeof(int32_t));
  memset(vp, -1, (size_t)total);

#define VR_BIT(vr)                                                                                                     \
  ({                                                                                                                   \
    int _t = TCCIR_DECODE_VREG_TYPE(vr);                                                                               \
    int _p = TCCIR_DECODE_VREG_POSITION(vr);                                                                           \
    int _b = -1;                                                                                                       \
    if (_t == TCCIR_VREG_TYPE_TEMP)                                                                                    \
      _b = tmp_base + _p;                                                                                              \
    else if (_t == TCCIR_VREG_TYPE_VAR)                                                                                \
      _b = var_base + _p;                                                                                              \
    else if (_t == TCCIR_VREG_TYPE_PARAM)                                                                              \
      _b = par_base + _p;                                                                                              \
    _b;                                                                                                                \
  })

  /* Seed: every PARAM_N maps to (param_idx=N, offset=0). */
  for (int p = 0; p <= max_par; p++)
  {
    if (p >= FWS_MAX_PARAMS)
      break;
    int32_t vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, p);
    int bit = VR_BIT(vr);
    if (bit < 0 || bit >= total)
      continue;
    vp[bit] = (int8_t)p;
    vo[bit] = 0;
  }

  /* Propagate via LOAD / ASSIGN / ADD #imm to fixpoint. */
  int changed = 1;
  int guard = 0;
  while (changed && guard++ < 64)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[q->op].has_dest)
        continue;
      IROperand dst = tcc_ir_op_get_dest(ir, q);
      if (dst.is_lval)
        continue; /* dst-deref is STORE-like; handled by the scan, not propagation */
      int32_t dest_vr = irop_get_vreg(dst);
      if (dest_vr < 0)
        continue;
      int dbit = VR_BIT(dest_vr);
      if (dbit < 0 || dbit >= total || vp[dbit] >= 0)
        continue;

      if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
      {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        int32_t src_vr = irop_get_vreg(src);
        if (src_vr < 0)
          continue;
        int sbit = VR_BIT(src_vr);
        if (sbit < 0 || sbit >= total || vp[sbit] < 0)
          continue;
        vp[dbit] = vp[sbit];
        vo[dbit] = vo[sbit];
        changed++;
      }
      else if (q->op == TCCIR_OP_ADD)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        IROperand src2 = tcc_ir_op_get_src2(ir, q);
        int32_t s1_vr = irop_get_vreg(src1);
        if (s1_vr < 0)
          continue;
        int sbit = VR_BIT(s1_vr);
        if (sbit < 0 || sbit >= total || vp[sbit] < 0)
          continue;
        if (!irop_is_immediate(src2) || src2.is_sym)
          continue;
        int64_t imm = irop_get_imm64_ex(ir, src2);
        if (imm < -65535 || imm > 65535)
          continue;
        int32_t new_off = vo[sbit] + (int32_t)imm;
        if (new_off < 0 || new_off >= FWS_MAX_BYTES)
          continue;
        vp[dbit] = vp[sbit];
        vo[dbit] = new_off;
        changed++;
      }
    }
  }

  /* A byte read before it is written stays observable, so it can never be claimed must-write. */
  uint8_t read_first[FWS_MAX_PARAMS][FWS_MAX_BYTES / 8];
  uint8_t must_write[FWS_MAX_PARAMS][FWS_MAX_BYTES / 8];
  memset(read_first, 0, sizeof(read_first));
  memset(must_write, 0, sizeof(must_write));

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Hard stops: anything that splits control flow or may not return. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      break;
    if (q->is_jump_target)
      break;

    /* Reads must be applied before this op's write to preserve program order. */
    for (int k = 1; k <= 2; k++)
    {
      if (k == 1 && !irop_config[q->op].has_src1)
        continue;
      if (k == 2 && !irop_config[q->op].has_src2)
        continue;
      IROperand sop = (k == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      if (!sop.is_lval)
        continue;
      int32_t svr = irop_get_vreg(sop);
      if (svr < 0)
        continue;
      int svr_type = TCCIR_DECODE_VREG_TYPE(svr);
      /* PARAM/VAR lval source in ASSIGN/LOAD is a stack-home load, not a pointer deref. */
      if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) &&
          (svr_type == TCCIR_VREG_TYPE_PARAM || svr_type == TCCIR_VREG_TYPE_VAR))
        continue;
      int sbit = VR_BIT(svr);
      if (sbit < 0 || sbit >= total || vp[sbit] < 0)
        continue;
      int pidx = vp[sbit];
      int off = vo[sbit];
      int size = fws_btype_bytes(sop.btype);
      if (size <= 0)
        size = 4;
      if (off < 0 || off + size > FWS_MAX_BYTES || pidx < 0 || pidx >= FWS_MAX_PARAMS)
        continue;
      for (int b = off; b < off + size; b++)
      {
        /* An earlier must_write in program order wins over this read. */
        if (!(must_write[pidx][b >> 3] & (uint8_t)(1u << (b & 7))))
          read_first[pidx][b >> 3] |= (uint8_t)(1u << (b & 7));
      }
    }

    /* Only an lval dest is a pointer write; a non-lval dest overwrites the vreg itself. */
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED)
      continue;
    IROperand dst = tcc_ir_op_get_dest(ir, q);
    if (!dst.is_lval)
      continue;
    int32_t dest_vr = irop_get_vreg(dst);
    if (dest_vr < 0)
      continue;
    int dbit = VR_BIT(dest_vr);
    if (dbit < 0 || dbit >= total || vp[dbit] < 0)
      continue;

    int pidx = vp[dbit];
    int off = vo[dbit];
    int size = fws_btype_bytes(dst.btype);
    if (size <= 0)
      continue;
    if (off < 0 || off + size > FWS_MAX_BYTES)
      continue;
    if (pidx < 0 || pidx >= FWS_MAX_PARAMS)
      continue;

    for (int b = off; b < off + size; b++)
    {
      /* A byte already read-first can never become must-write. */
      if (!(read_first[pidx][b >> 3] & (uint8_t)(1u << (b & 7))))
        must_write[pidx][b >> 3] |= (uint8_t)(1u << (b & 7));
    }
    LOG_IR_GEN("FWS: %p param=%d offset=%d size=%d (at i=%d)", (void *)func_sym, pidx, off, size, i);
  }

  /* Commit non-empty per-param must_write bitmaps to the summary. */
  FuncWriteSummary *summary = NULL;
  for (int p = 0; p < FWS_MAX_PARAMS; p++)
  {
    int has_bits = 0;
    for (int j = 0; j < FWS_MAX_BYTES / 8; j++)
      if (must_write[p][j])
      {
        has_bits = 1;
        break;
      }
    if (!has_bits)
      continue;
    if (!summary)
      summary = fws_create(func_sym);
    FwsParamSummary *ps = fws_get_param(summary, p);
    if (!ps)
      continue;
    memcpy(ps->must_write, must_write[p], FWS_MAX_BYTES / 8);
  }

  tcc_free(vp);
  tcc_free(vo);

#undef VR_BIT
}
