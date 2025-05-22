/*
 *  TCC IR - Optimization Passes Implementation
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
#include "opt_alias.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "opt_utils.h"
#include "opt_xform.h"
#include "pool.h"
#include "vreg.h"

/* ============================================================================
 * FP Offset Cache Optimization - delegated to tccopt.c
 * ============================================================================ */

/* Forward declarations for functions remaining in opt.c */

extern void tcc_opt_fp_mat_cache_init(TCCIRState *ir);
extern void tcc_opt_fp_mat_cache_clear(TCCIRState *ir);
extern void tcc_opt_fp_mat_cache_free(TCCIRState *ir);
extern int tcc_opt_fp_mat_cache_lookup(TCCIRState *ir, int offset, int *phys_reg);
extern void tcc_opt_fp_mat_cache_record(TCCIRState *ir, int offset, int phys_reg);
extern void tcc_opt_fp_mat_cache_invalidate_reg(TCCIRState *ir, int phys_reg);

void tcc_ir_opt_fp_cache_init(TCCIRState *ir)
{
  tcc_opt_fp_mat_cache_init(ir);
}

void tcc_ir_opt_fp_cache_clear(TCCIRState *ir)
{
  tcc_opt_fp_mat_cache_clear(ir);
}

void tcc_ir_opt_fp_cache_free(TCCIRState *ir)
{
  tcc_opt_fp_mat_cache_free(ir);
}

int tcc_ir_opt_fp_cache_lookup(TCCIRState *ir, int offset, int *phys_reg)
{
  return tcc_opt_fp_mat_cache_lookup(ir, offset, phys_reg);
}

void tcc_ir_opt_fp_cache_record(TCCIRState *ir, int offset, int phys_reg)
{
  tcc_opt_fp_mat_cache_record(ir, offset, phys_reg);
}

void tcc_ir_opt_fp_cache_invalidate_reg(TCCIRState *ir, int phys_reg)
{
  tcc_opt_fp_mat_cache_invalidate_reg(ir, phys_reg);
}

/* External declarations for functions defined in tccir.c */
extern int tcc_ir_find_defining_instruction(TCCIRState *ir, int32_t vreg, int before_idx);
extern int tcc_ir_vreg_has_single_use(TCCIRState *ir, int32_t vreg, int exclude_idx);

#ifndef TCCIR_VREG_TYPE_NONE
#define TCCIR_VREG_TYPE_NONE 0
#endif

void tcc_ir_analyze_pure_via_sret(TCCIRState *ir, Sym *func_sym)
{
  if (!ir || !func_sym)
    return;
  if (func_sym->f.func_pure_via_sret)
    return;

  /* Only meaningful for functions that return via hidden sret pointer.
   * tcc_ir_params_add_hidden_sret sets func_vc to a non-zero stack offset
   * (the local slot holding the spilled sret pointer) when sret is used. */
  if (func_vc == 0)
    return;

  /* Seed the "sret-derived" set with the sret PARAM vreg (P0) and the spill
   * slot at func_vc.  Optimization may have eliminated the prolog STORE
   * (forwarding reads of the slot back to P0), so we both check for the
   * slot pattern and unconditionally include P0. */
  int32_t sret_param_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0);
  int32_t sret_slot = (int32_t)func_vc;

  const int n = ir->next_instruction_index;

  /* Build the set of vregs whose value is derived from the sret pointer.
   * Seed: sret_param_vr.  Forward propagation rules:
   *   - LOAD/ASSIGN T <-- StackLoc[sret_slot]: T joins set.
   *   - LOAD/ASSIGN T <-- vreg-in-set: T joins set.
   *   - ADD T <-- vreg-in-set + immediate: T joins set.
   * Iterate to fixpoint (linear scans, small functions). */
  int max_vreg = ir->next_temporary_variable + ir->next_local_variable + ir->next_parameter + 16;
  uint8_t *is_sret_derived = tcc_mallocz((size_t)((max_vreg + 7) / 8));
  if (!is_sret_derived)
    return;

#define SRET_VR_TO_BIT(vr)                                                                                             \
  ({                                                                                                                   \
    int _t = TCCIR_DECODE_VREG_TYPE(vr);                                                                               \
    int _p = TCCIR_DECODE_VREG_POSITION(vr);                                                                           \
    int _b = -1;                                                                                                       \
    if (_t == TCCIR_VREG_TYPE_TEMP)                                                                                    \
      _b = _p;                                                                                                         \
    else if (_t == TCCIR_VREG_TYPE_VAR)                                                                                \
      _b = ir->next_temporary_variable + _p;                                                                           \
    else if (_t == TCCIR_VREG_TYPE_PARAM)                                                                              \
      _b = ir->next_temporary_variable + ir->next_local_variable + _p;                                                 \
    _b;                                                                                                                \
  })
#define SRET_MARK(vr)                                                                                                  \
  do                                                                                                                   \
  {                                                                                                                    \
    int _b = SRET_VR_TO_BIT(vr);                                                                                       \
    if (_b >= 0 && _b < max_vreg)                                                                                      \
      is_sret_derived[_b / 8] |= (uint8_t)(1u << (_b % 8));                                                            \
  } while (0)
#define SRET_TEST(vr)                                                                                                  \
  ({                                                                                                                   \
    int _b = SRET_VR_TO_BIT(vr);                                                                                       \
    int _r = (_b >= 0 && _b < max_vreg) ? ((is_sret_derived[_b / 8] >> (_b % 8)) & 1u) : 0;                            \
    _r;                                                                                                                \
  })

  SRET_MARK(sret_param_vr);

  int changes = 1;
  while (changes)
  {
    changes = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[q->op].has_dest)
        continue;

      IROperand dst = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dst);
      if (dest_vr < 0)
        continue;
      if (SRET_TEST(dest_vr))
        continue;

      if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN)
      {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        /* LOAD/ASSIGN from sret_slot: dest holds the sret pointer value. */
        if (src.is_local && irop_get_tag(src) == IROP_TAG_STACKOFF && (int32_t)irop_get_stack_offset(src) == sret_slot)
        {
          SRET_MARK(dest_vr);
          changes++;
          continue;
        }
        /* Propagate via vreg. */
        int32_t src_vr = irop_get_vreg(src);
        if (src_vr >= 0 && SRET_TEST(src_vr))
        {
          SRET_MARK(dest_vr);
          changes++;
          continue;
        }
      }
      else if (q->op == TCCIR_OP_ADD)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        IROperand src2 = tcc_ir_op_get_src2(ir, q);
        int32_t src1_vr = irop_get_vreg(src1);
        if (src1_vr >= 0 && SRET_TEST(src1_vr) && irop_is_immediate(src2) && !src2.is_sym)
        {
          SRET_MARK(dest_vr);
          changes++;
        }
      }
    }
  }

  /* Scan all ops for disqualifying side effects. */
  int pure = 1;
  for (int i = 0; i < n && pure; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
    case TCCIR_OP_NOP:
    case TCCIR_OP_LOAD:
    case TCCIR_OP_LOAD_INDEXED:
    case TCCIR_OP_LOAD_POSTINC:
    case TCCIR_OP_LEA:
    case TCCIR_OP_ASSIGN:
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    case TCCIR_OP_MUL:
    case TCCIR_OP_DIV:
    case TCCIR_OP_IMOD:
    case TCCIR_OP_UMOD:
    case TCCIR_OP_AND:
    case TCCIR_OP_OR:
    case TCCIR_OP_XOR:
    case TCCIR_OP_SHL:
    case TCCIR_OP_SHR:
    case TCCIR_OP_SAR:
    case TCCIR_OP_CMP:
    case TCCIR_OP_FCMP:
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCPARAMVOID:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_SELECT:
      /* Pure-by-default ops. */
      break;

    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    {
      IROperand dst = tcc_ir_op_get_dest(ir, q);
      /* Local stack slot: not observable. */
      if (dst.is_local && irop_get_tag(dst) == IROP_TAG_STACKOFF)
        break;
      /* Through a vreg derived from sret pointer: allowed (the *sret write). */
      int32_t dest_vr = irop_get_vreg(dst);
      if (dest_vr >= 0 && SRET_TEST(dest_vr))
        break;
      /* Anything else (global, arbitrary pointer): not pure. */
      pure = 0;
      break;
    }

    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!callee)
      {
        pure = 0;
        break;
      }
      int callee_pure = callee->f.func_pure | callee->f.func_const | callee->f.func_pure_via_sret;
      if (callee->type.ref)
        callee_pure |=
            callee->type.ref->f.func_pure | callee->type.ref->f.func_const | callee->type.ref->f.func_pure_via_sret;
      if (callee_pure)
        break;
      const char *name = get_tok_str(callee->v, NULL);
      if (name && tcc_ir_is_pure_aeabi(name))
        break;
      pure = 0;
      break;
    }

    default:
      /* Unknown / side-effecting (INLINE_ASM, SETJMP, VLA_ALLOC, etc.): bail. */
      pure = 0;
      break;
    }
  }

  tcc_free(is_sret_derived);

  if (pure)
  {
    func_sym->f.func_pure_via_sret = 1;
    LOG_IR_GEN("=== PURE_VIA_SRET: function marked pure (sret_slot=%d) ===", (int)sret_slot);
  }

#undef SRET_VR_TO_BIT
#undef SRET_MARK
#undef SRET_TEST
}

/* ============================================================================
 * Function Write Summary — per-function record of "must-write byte ranges"
 * through each pointer parameter.  Computed at the end of IR optimization
 * (just before codegen), consulted by tcc_ir_opt_dead_init_via_call to kill
 * stack-slot initializations that the callee fully overwrites.
 *
 * Conservative analysis:
 *   - Linearly scans IR from i=0 until the first branch / call / unknown op.
 *     Stores encountered along that prefix are unconditional must-writes.
 *   - Tracks (param_idx, offset) for each TEMP/VAR reached via ASSIGN, LOAD,
 *     or ADD #immediate from a PARAM source.
 *   - Records STORE writes whose dest vreg is a tracked alias.
 *
 * Storage: a flat per-TU linked list keyed by Sym*.  Linear lookup is fine
 * for typical translation-unit sizes (tens to low-hundreds of functions).
 * ============================================================================ */

#define FWS_MAX_PARAMS 8  /* up to 8 tracked pointer params per function */
#define FWS_MAX_BYTES 256 /* up to 256 bytes per param */

typedef struct FwsParamSummary
{
  int param_idx;                         /* IR param index this entry covers */
  uint8_t must_write[FWS_MAX_BYTES / 8]; /* bit i set = byte i is must-write */
} FwsParamSummary;

typedef struct FuncWriteSummary
{
  int num_params;
  FwsParamSummary params[FWS_MAX_PARAMS];
} FuncWriteSummary;

typedef struct FwsEntry
{
  Sym *sym;
  FuncWriteSummary summary;
  struct FwsEntry *next;
} FwsEntry;

static FwsEntry *fws_head = NULL;

static FuncWriteSummary *fws_lookup(Sym *sym)
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

static int fws_range_fully_set(const FwsParamSummary *ps, int offset, int size)
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

static int fws_btype_bytes(int btype)
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

  /* Pre-scan max vreg positions per type so we can size a bitmap. */
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

  /* For each vreg: (param_idx, offset).  param_idx = -1 means "not tracked". */
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

  /* Seed: every PARAM_N → (param_idx=N, offset=0). */
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
        continue; /* dst-deref means STORE-like; handled in scan, not propagation */
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

  /* Linear scan from entry, with per-byte first-event tracking per param.
   *
   * For each byte b of a tracked-param's pointee:
   *   - If a READ of b happens before any WRITE: byte is "read-first" and
   *     CANNOT be claimed as must-write (the prior caller value is
   *     observable through this read).
   *   - If a WRITE of b happens before any READ: byte is "must-write" and
   *     CAN be added to the summary (callers' prior stores are dead).
   *
   * "Read" = an lval (deref) reference in src1 or src2 of any op whose
   * underlying vreg is a tracked alias.
   * "Write" = STORE / STORE_INDEXED op with dst.is_lval=true through a
   * tracked-alias vreg.
   * Register-overwrite stores (`P0 <-- T5 [STORE]` for `a++`) have
   * is_lval=false and are NOT pointer-writes. */
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

    /* Detect READs (lval src refs through a tracked alias) BEFORE we
     * apply this op's WRITE (program-order).
     *
     * is_lval on an operand is overloaded:
     *   - ASSIGN/LOAD with src vreg = PARAM, lval=true: load the param's
     *     value from its home register/spill slot (NOT a deref of the
     *     pointer that the param holds).
     *   - Other ops (arithmetic, XOR, ADD-with-other-deref, etc.) with
     *     src vreg = PARAM-or-TEMP, lval=true: actual pointer deref.
     *
     * The first case is just a register/home load and doesn't read
     * through the pointer.  Skip it to avoid false positives. */
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
      /* PARAM-lval source in ASSIGN/LOAD is a home-load, not a pointer
       * deref.  Same for VAR-lval (load from var's stack home). */
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
        /* Only mark read_first if not already marked as must_write
         * (must_write means write happened earlier in program order). */
        if (!(must_write[pidx][b >> 3] & (uint8_t)(1u << (b & 7))))
          read_first[pidx][b >> 3] |= (uint8_t)(1u << (b & 7));
      }
    }

    /* Detect WRITE: only STORE / STORE_INDEXED with lval dest through a
     * tracked alias.  Register-overwrite stores (non-lval dest) are NOT
     * pointer writes; they overwrite the vreg itself (e.g. `a++` lowers
     * to `P0 <-- (P0 + sizeof)`). */
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
      /* Only mark must_write if not already marked as read_first. */
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

/* ============================================================================
 * TU-wide Read Summary — per-function record of:
 *   - reads of static globals (load or address-of)
 *   - writes to static globals
 *   - calls to other functions (callee Sym*)
 *
 * Collected from the optimized IR at end-of-IR-opts (same timing as
 * tcc_ir_compute_func_write_summary).  Consumed at end-of-TU by
 * tcc_ir_tu_analyze_dead_statics, which builds the call graph reachability
 * closure from non-static / addr-taken roots and marks each static global
 * with sym->a.tu_no_readers when no reachable function reads it.  Functions
 * that write to such statics are then marked func_late_reopt; the new DSE
 * pass eliminates those stores when the function is re-compiled.
 * ============================================================================ */

typedef struct TuSymSet
{
  Sym **items;
  int count;
  int capacity;
} TuSymSet;

static void tu_symset_add(TuSymSet *s, Sym *sym)
{
  if (!sym)
    return;
  for (int i = 0; i < s->count; i++)
    if (s->items[i] == sym)
      return;
  if (s->count >= s->capacity)
  {
    int new_cap = s->capacity ? s->capacity * 2 : 4;
    s->items = tcc_realloc(s->items, sizeof(Sym *) * new_cap);
    s->capacity = new_cap;
  }
  s->items[s->count++] = sym;
}

static void tu_symset_free(TuSymSet *s)
{
  if (s->items)
    tcc_free(s->items);
  s->items = NULL;
  s->count = s->capacity = 0;
}

typedef struct TuFuncSummary
{
  Sym *func_sym;
  TuSymSet calls;          /* static (intra-TU) functions called */
  TuSymSet static_reads;   /* static globals read or address-taken */
  TuSymSet static_writes;  /* static globals written */
  struct TuFuncSummary *next;
} TuFuncSummary;

static TuFuncSummary *tu_summary_head = NULL;

static TuFuncSummary *tu_summary_lookup(Sym *func_sym)
{
  for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
    if (e->func_sym == func_sym)
      return e;
  return NULL;
}

void tcc_ir_tu_func_summary_clear_all(void)
{
  while (tu_summary_head)
  {
    TuFuncSummary *n = tu_summary_head->next;
    tu_symset_free(&tu_summary_head->calls);
    tu_symset_free(&tu_summary_head->static_reads);
    tu_symset_free(&tu_summary_head->static_writes);
    tcc_free(tu_summary_head);
    tu_summary_head = n;
  }
}

/* Helper: extract Sym* from a SYMREF operand. */
static Sym *tu_extract_sym(TCCIRState *ir, IROperand op)
{
  if (!op.is_sym)
    return NULL;
  IRPoolSymref *ref = irop_get_symref_ex(ir, op);
  return ref ? ref->sym : NULL;
}

/* A static global candidate is a file-scope (or local) static *data* symbol
 * defined in this TU.  Excludes functions, externs, weaks, and dllimports. */
static int tu_is_static_global_candidate(const Sym *sym)
{
  if (!sym)
    return 0;
  if ((sym->type.t & VT_BTYPE) == VT_FUNC)
    return 0;
  if (sym->a.weak || sym->a.dllimport)
    return 0;
  /* VT_STATIC marks both file-scope and function-scope statics — both have
   * internal linkage and storage in this TU.  A symbol can still carry
   * VT_EXTERN at this point for tentative definitions or other internal
   * marker uses, but VT_STATIC implies the symbol's storage lives here. */
  if (!(sym->type.t & VT_STATIC))
    return 0;
  /* VT_CONSTANT (qualified const) globals are not writeable in well-formed
   * C, so they cannot be dead-store candidates anyway. */
  if (sym->type.t & VT_CONSTANT)
    return 0;
  /* Volatile statics must observe stores (hardware registers etc). */
  if (sym->type.t & VT_VOLATILE)
    return 0;
  return 1;
}

void tcc_ir_collect_tu_func_summary(TCCIRState *ir, Sym *func_sym)
{
  if (!ir || !func_sym)
    return;
  if (tu_summary_lookup(func_sym))
    return; /* Already collected for this Sym. */

  TuFuncSummary *s = tcc_mallocz(sizeof(*s));
  s->func_sym = func_sym;

  const int n = ir->next_instruction_index;
  int writes_any_static = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Direct calls: record callee Sym so the call graph is captured. */
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (callee)
        tu_symset_add(&s->calls, callee);
      /* fall through so the call's operands are still examined for sym refs
       * (e.g. struct arg passed by value) */
    }

    /* STORE: dest may be a SYMREF address (static global write).
     * For STORE_INDEXED / STORE_POSTINC, disp_fusion may clear is_lval on
     * the base operand even though the op semantically writes through that
     * base — treat the dest as a write for any SYMREF regardless of
     * is_lval for these indexed/postinc forms. */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int dest_is_write_target =
          dest.is_sym &&
          (dest.is_lval || q->op == TCCIR_OP_STORE_INDEXED ||
           q->op == TCCIR_OP_STORE_POSTINC);
      if (dest_is_write_target)
      {
        Sym *sym = tu_extract_sym(ir, dest);
        if (sym && tu_is_static_global_candidate(sym))
        {
          tu_symset_add(&s->static_writes, sym);
          writes_any_static = 1;
        }
      }
    }

    /* Read-side operand scan: any SYMREF appearing in src1/src2 counts as a
     * read.  Both lval-derefs (LOAD-like) and bare-symref address-of forms
     * are conservatively treated as reads — the address-of case may escape
     * via a call/struct and let the symbol be read indirectly. */
    if (irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      Sym *sym = tu_extract_sym(ir, s1);
      if (sym && tu_is_static_global_candidate(sym))
        tu_symset_add(&s->static_reads, sym);
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      Sym *sym = tu_extract_sym(ir, s2);
      if (sym && tu_is_static_global_candidate(sym))
        tu_symset_add(&s->static_reads, sym);
    }
    /* LEA: dest holds an address derived from src1.  If src1 was a SYMREF
     * we've already counted it as a read above.  STOREs that put the
     * SYMREF in src1 (the stored value) for "p = &g" are likewise covered. */

    /* Conservative escape: if any operand is an inline-asm or unknown op,
     * give up by marking every static this function touches as read so we
     * don't kill a store the asm reads.  Add a coarse switch list if more
     * unsafe ops show up. */
    if (q->op == TCCIR_OP_INLINE_ASM || q->op == TCCIR_OP_TRAP ||
        q->op == TCCIR_OP_SETJMP)
    {
      /* Promote all writes to also-read so DSE never fires for this func. */
      for (int k = 0; k < s->static_writes.count; k++)
        tu_symset_add(&s->static_reads, s->static_writes.items[k]);
    }
  }

  if (writes_any_static && func_sym->type.ref)
    func_sym->type.ref->f.tu_static_writer = 1;

  s->next = tu_summary_head;
  tu_summary_head = s;
}

/* End-of-TU noreturn propagation.
 *
 * The trigger in gen_function speculatively saved tokens (via
 * func_keep_tokens_for_noreturn) for any caller making a FUNCCALL to a
 * not-yet-compiled callee — at first-pass-compile time we can't tell
 * forward-decl-defined-later from extern-defined-in-another-TU apart.
 *
 * Now that every function in the TU has been compiled, the noreturn flag
 * on each callee is final.  Set func_late_reopt = 1 on any caller whose
 * callee is now func_noreturn — gen_late_reopt_functions will then re-
 * emit them so the DCE extension (FUNCCALL-to-noreturn as terminator)
 * eliminates the unreachable post-call body.  Callers whose callees did
 * NOT end up noreturn keep their tokens but are not re-emitted (the
 * existing token-cleanup paths cover this — they're harmless leakage). */
void tcc_ir_tu_propagate_noreturn_to_callers(void)
{
  for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
  {
    Sym *fs = e->func_sym;
    if (!fs || !fs->type.ref)
      continue;
    if (!fs->type.ref->f.func_keep_tokens_for_noreturn)
      continue;
    if (fs->type.ref->f.func_late_reopt)
      continue;
    for (int i = 0; i < e->calls.count; i++)
    {
      Sym *callee = e->calls.items[i];
      if (callee && callee->type.ref && callee->type.ref->f.func_noreturn)
      {
        fs->type.ref->f.func_late_reopt = 1;
        break;
      }
    }
  }
}

/* End-of-TU analysis: starting from non-static / addr-taken functions,
 * compute the transitive callee closure and the set of static globals read
 * by reachable functions.  Statics that are written but not read by any
 * reachable function, and whose address has not been taken, are marked
 * tu_no_readers.  Their writer functions are flagged func_late_reopt so the
 * end-of-TU re-compile pass can run the new DSE pass on them. */
void tcc_ir_tu_analyze_dead_statics(void)
{
  /* Phase 1: collect all function summaries and pick roots.
   * A "root" is a function whose body is reachable from outside this TU:
   *   - non-static / extern-visible functions
   *   - functions whose address has been taken (escape via function pointer) */
  int total = 0;
  for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
    total++;
  if (total == 0)
    return;

  /* Mark reachable: BFS over the call graph. */
  for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
  {
    Sym *fs = e->func_sym;
    int is_root = 0;
    if (!fs)
      continue;
    if (!(fs->type.t & VT_STATIC))
      is_root = 1;
    if (fs->a.addrtaken)
      is_root = 1;
    /* Constructors / destructors are entry points called by the runtime. */
    if (fs->type.ref && (fs->type.ref->f.func_ctor || fs->type.ref->f.func_dtor))
      is_root = 1;
    if (is_root && fs->type.ref)
      fs->type.ref->f.tu_reachable = 1;
  }

  /* Simple worklist BFS.  Bounded by total^2 in the pathological case;
   * fine for typical TUs (tens to hundreds of functions). */
  int changed = 1;
  while (changed)
  {
    changed = 0;
    for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
    {
      if (!e->func_sym || !e->func_sym->type.ref)
        continue;
      if (!e->func_sym->type.ref->f.tu_reachable)
        continue;
      for (int i = 0; i < e->calls.count; i++)
      {
        Sym *callee = e->calls.items[i];
        if (!callee || !callee->type.ref)
          continue;
        if (!callee->type.ref->f.tu_reachable)
        {
          callee->type.ref->f.tu_reachable = 1;
          changed = 1;
        }
      }
    }
  }

  /* Phase 2: for each static global written somewhere, decide if it has any
   * reachable readers.  Collect the union of static writes across the TU
   * first so we know which symbols to evaluate. */
  TuSymSet candidates = {0};
  for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
  {
    for (int i = 0; i < e->static_writes.count; i++)
      tu_symset_add(&candidates, e->static_writes.items[i]);
  }

  for (int c = 0; c < candidates.count; c++)
  {
    Sym *g = candidates.items[c];
    if (!g)
      continue;
    if (g->a.addrtaken)
      continue; /* address escaped; cannot prove no readers */
    int read_by_reachable = 0;
    for (TuFuncSummary *e = tu_summary_head; e && !read_by_reachable; e = e->next)
    {
      if (!e->func_sym || !e->func_sym->type.ref)
        continue;
      if (!e->func_sym->type.ref->f.tu_reachable)
        continue;
      for (int i = 0; i < e->static_reads.count; i++)
      {
        if (e->static_reads.items[i] == g)
        {
          read_by_reachable = 1;
          break;
        }
      }
    }
    if (!read_by_reachable)
    {
      g->a.tu_no_readers = 1;
      /* Mark reachable writer functions for late_reopt.  Unreachable writers
       * don't need re-compilation — their stores never execute.  Only those
       * already kept alive via the inline_fns token-preservation path can
       * actually be re-compiled, but setting the flag is harmless otherwise. */
      for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
      {
        if (!e->func_sym || !e->func_sym->type.ref)
          continue;
        if (!e->func_sym->type.ref->f.tu_reachable)
          continue;
        for (int i = 0; i < e->static_writes.count; i++)
        {
          if (e->static_writes.items[i] == g)
          {
            e->func_sym->type.ref->f.func_late_reopt = 1;
            break;
          }
        }
      }
    }
  }

  tu_symset_free(&candidates);
}

/* ============================================================================
 * Dead Init Via Call — kill stack-slot stores fully overwritten by a
 * subsequent CALL whose callee summary covers the stored bytes.
 * ============================================================================ */
int tcc_ir_opt_dead_init_via_call(TCCIRState *ir)
{
  if (!ir)
    return 0;
  const int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  int changes = 0;

  for (int call_idx = 0; call_idx < n; call_idx++)
  {
    IRQuadCompact *call_q = &ir->compact_instructions[call_idx];
    if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, call_q));
    if (!callee)
      continue;
    FuncWriteSummary *summary = fws_lookup(callee);
    if (!summary)
      continue;

    /* For each tracked param, find the corresponding FUNCPARAM at this
     * call site and check whether it passes the address of a local. */
    for (int pi = 0; pi < summary->num_params; pi++)
    {
      FwsParamSummary *ps = &summary->params[pi];
      IROperand pop;
      if (!ir_opt_get_call_param_operand(ir, call_idx, ps->param_idx, &pop))
        continue;
      /* Param must be Addr[StackLoc[X]] — local, not lval, STACKOFF-tagged. */
      if (!pop.is_local || pop.is_lval)
        continue;
      if (irop_get_tag(pop) != IROP_TAG_STACKOFF)
        continue;
      int32_t base_off = (int32_t)irop_get_stack_offset(pop);

      /* Scan backward from the call for STOREs to the covered bytes. */
      for (int s = call_idx - 1; s >= 0; s--)
      {
        IRQuadCompact *sq = &ir->compact_instructions[s];
        if (sq->op == TCCIR_OP_NOP)
          continue;
        /* Bail at any control-flow boundary or call (different basic block / unknown effects). */
        if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF || sq->op == TCCIR_OP_IJUMP ||
            sq->op == TCCIR_OP_SWITCH_TABLE || sq->op == TCCIR_OP_FUNCCALLVAL || sq->op == TCCIR_OP_FUNCCALLVOID)
          break;
        if (sq->is_jump_target)
          break;
        if (sq->op != TCCIR_OP_STORE)
          continue;
        IROperand sdst = tcc_ir_op_get_dest(ir, sq);
        /* STORE to a local stack slot. */
        if (!sdst.is_local || irop_get_tag(sdst) != IROP_TAG_STACKOFF)
          continue;
        int32_t s_off = (int32_t)irop_get_stack_offset(sdst);
        int s_size = fws_btype_bytes(sdst.btype);
        if (s_size <= 0)
          continue;

        /* Map this store's absolute offset back into "param-relative" space. */
        int32_t rel_off = s_off - base_off;
        if (!fws_range_fully_set(ps, rel_off, s_size))
          continue;

        /* Verify the slot isn't read between s and call_idx.  A "read"
         * here is an lval (deref) reference to a byte in [s_off, s_off+s_size).
         * An addr-of (is_lval=false) operand — e.g. the FUNCPARAM that
         * passes Addr[StackLoc[X]] to our call — is not a read; the actual
         * access happens inside the callee and is covered by its summary. */
        int slot_read = 0;
        for (int t = s + 1; t < call_idx && !slot_read; t++)
        {
          IRQuadCompact *tq = &ir->compact_instructions[t];
          if (tq->op == TCCIR_OP_NOP)
            continue;
          for (int k = 1; k < 3 && !slot_read; k++)
          {
            if (k == 1 && !irop_config[tq->op].has_src1)
              continue;
            if (k == 2 && !irop_config[tq->op].has_src2)
              continue;
            IROperand op = (k == 1) ? tcc_ir_op_get_src1(ir, tq) : tcc_ir_op_get_src2(ir, tq);
            if (!op.is_local || irop_get_tag(op) != IROP_TAG_STACKOFF)
              continue;
            if (!op.is_lval)
              continue; /* addr-of, not a read */
            int32_t op_off = (int32_t)irop_get_stack_offset(op);
            int op_sz = fws_btype_bytes(op.btype);
            if (op_sz <= 0)
              op_sz = 8; /* unknown size — assume worst case for overlap */
            if (op_off + op_sz > s_off && op_off < s_off + s_size)
              slot_read = 1;
          }
        }
        if (slot_read)
          continue;

        LOG_IR_GEN("DEAD INIT VIA CALL: nop STORE at i=%d (call i=%d, callee=%p, param=%d, rel_off=%d, size=%d)", s,
                   call_idx, (void *)callee, ps->param_idx, rel_off, s_size);
        sq->op = TCCIR_OP_NOP;
        changes++;
      }
    }
  }

  return changes;
}

/* Dead Store Elimination - remove ASSIGN instructions where the destination
 * vreg is never used. This eliminates redundant copies after CSE/idempotent
 * optimizations. Instead of compacting the array, we mark dead stores as NOP.
 */

#define STACK_CSE_MAX_ENTRIES 32

typedef struct StackAddrSeq
{
  int32_t stack_offset; /* StackLoc offset X */
  int64_t add_constant; /* Added constant C (0 if bare ASSIGN, no ADD) */
  int32_t result_vreg;  /* Vreg holding StackLoc[X]+C after the sequence */
  int assign_idx;       /* Index of the ASSIGN instruction */
  int add_idx;          /* Index of the ADD instruction (-1 if bare ASSIGN) */
  int eliminated;       /* Set to 1 when this entry was replaced by an earlier one */
} StackAddrSeq;

int tcc_ir_opt_stack_addr_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  StackAddrSeq seqs[STACK_CSE_MAX_ENTRIES];
  int seq_count = 0;
  int i, j, k;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== STACK ADDRESS CSE START (n=%d) ===", n);

  /* Pass 1: Collect all "ASSIGN Addr[StackLoc[X]]" sequences.
   * For each, check if the next non-NOP instruction is "ADD dest, dest, #C". */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src1) != IROP_TAG_STACKOFF || src1.is_lval)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vreg = irop_get_vreg(dest);
    int32_t stack_off = src1.u.imm32;
    int64_t add_const = 0;
    int add_idx = -1;
    int32_t final_vreg = vreg;

    /* Check if next instruction is ADD dest, dest, #imm */
    if (i + 1 < n)
    {
      IRQuadCompact *qnext = &ir->compact_instructions[i + 1];
      if (qnext->op == TCCIR_OP_ADD)
      {
        IROperand nd = tcc_ir_op_get_dest(ir, qnext);
        IROperand ns1 = tcc_ir_op_get_src1(ir, qnext);
        IROperand ns2 = tcc_ir_op_get_src2(ir, qnext);
        int32_t nd_vr = irop_get_vreg(nd);
        int32_t ns1_vr = irop_get_vreg(ns1);

        if (nd_vr == vreg && ns1_vr == vreg && irop_is_immediate(ns2))
        {
          add_const = irop_get_imm64_ex(ir, ns2);
          add_idx = i + 1;
          final_vreg = nd_vr;
        }
      }
    }

    /* Only track ASSIGN+ADD pairs (add_constant != 0).  Bare ASSIGN of a
     * stack address is used for many purposes (struct init, parameter passing,
     * store values) and merging them is fragile. */
    if (add_idx < 0)
      continue;

    if (seq_count < STACK_CSE_MAX_ENTRIES)
    {
      seqs[seq_count].stack_offset = stack_off;
      seqs[seq_count].add_constant = add_const;
      seqs[seq_count].result_vreg = final_vreg;
      seqs[seq_count].assign_idx = i;
      seqs[seq_count].add_idx = add_idx;
      seqs[seq_count].eliminated = 0;
      seq_count++;
    }
  }

  /* Pass 1b: Fold each ASSIGN+ADD pair into a single ASSIGN with combined
   * offset.  This lets the backend emit a single ADD Rd, SP, #combined
   * instead of MOV Rd, SP + ADD Rd, Rd, #K. */
  for (i = 0; i < seq_count; i++)
  {
    IRQuadCompact *q_assign = &ir->compact_instructions[seqs[i].assign_idx];
    IRQuadCompact *q_add = &ir->compact_instructions[seqs[i].add_idx];
    IROperand src1 = tcc_ir_op_get_src1(ir, q_assign);
    int32_t combined = seqs[i].stack_offset + (int32_t)seqs[i].add_constant;

    IROperand new_src = src1;
    new_src.u.imm32 = combined;
    tcc_ir_op_set_src1(ir, q_assign, new_src);

    q_add->op = TCCIR_OP_NOP;
    seqs[i].stack_offset = combined;
    seqs[i].add_constant = 0;
    seqs[i].add_idx = -1;
    changes++;

    LOG_IR_GEN("  FOLD: ASSIGN StackLoc[%d] + ADD #%lld → ASSIGN StackLoc[%d] at idx %d", (int)src1.u.imm32,
               (long long)seqs[i].add_constant, combined, seqs[i].assign_idx);
  }

  if (seq_count < 2)
  {
    LOG_IR_GEN("=== STACK ADDRESS CSE END: %d folds, fewer than 2 sequences ===", changes);
    return changes;
  }

  /* Pass 2: For each pair with the same (stack_offset, add_constant),
   * check if the first result vreg survives to the second sequence.
   * If so, NOP the second and replace its vreg everywhere. */
  for (i = 0; i < seq_count; i++)
  {
    if (seqs[i].eliminated)
      continue;

    for (j = i + 1; j < seq_count; j++)
    {
      if (seqs[j].eliminated)
        continue;
      if (seqs[i].stack_offset != seqs[j].stack_offset)
        continue;
      if (seqs[i].add_constant != seqs[j].add_constant)
        continue;

      /* Same (offset, constant) pair. Check that seqs[i].result_vreg is not
       * redefined between its last defining instruction and seqs[j].assign_idx. */
      int first_last_def = (seqs[i].add_idx >= 0) ? seqs[i].add_idx : seqs[i].assign_idx;
      int second_start = seqs[j].assign_idx;
      int redefined = 0;

      for (k = first_last_def + 1; k < second_start; k++)
      {
        IRQuadCompact *qk = &ir->compact_instructions[k];
        if (qk->op == TCCIR_OP_NOP)
          continue;
        if (!irop_config[qk->op].has_dest)
          continue;
        IROperand dk = tcc_ir_op_get_dest(ir, qk);
        if (irop_get_vreg(dk) == seqs[i].result_vreg)
        {
          redefined = 1;
          break;
        }
      }

      if (redefined)
        continue;

      LOG_IR_GEN("  CSE: seq[%d] (off=%d +%lld vreg=%d idx=%d/%d) duplicates seq[%d]", j, seqs[j].stack_offset,
                 (long long)seqs[j].add_constant, seqs[j].result_vreg, seqs[j].assign_idx, seqs[j].add_idx, i);

      /* NOP the duplicate sequence */
      ir->compact_instructions[seqs[j].assign_idx].op = TCCIR_OP_NOP;
      if (seqs[j].add_idx >= 0)
        ir->compact_instructions[seqs[j].add_idx].op = TCCIR_OP_NOP;

      /* Replace all SOURCE uses of seqs[j].result_vreg with seqs[i].result_vreg.
       * Only rewrite src1/src2 — never dest — to avoid redirecting writes. */
      int32_t old_vr = seqs[j].result_vreg;
      int32_t new_vr = seqs[i].result_vreg;

      for (k = 0; k < n; k++)
      {
        IRQuadCompact *qk = &ir->compact_instructions[k];
        if (qk->op == TCCIR_OP_NOP)
          continue;

        if (irop_config[qk->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, qk);
          if (irop_get_vreg(s1) == old_vr)
          {
            IROperand rep = s1;
            irop_set_vreg(&rep, new_vr);
            tcc_ir_op_set_src1(ir, qk, rep);
          }
        }
        if (irop_config[qk->op].has_src2)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, qk);
          if (irop_get_vreg(s2) == old_vr)
          {
            IROperand rep = s2;
            irop_set_vreg(&rep, new_vr);
            tcc_ir_op_set_src2(ir, qk, rep);
          }
        }
      }

      seqs[j].eliminated = 1;
      changes++;
    }
  }

  LOG_IR_GEN("=== STACK ADDRESS CSE END: %d replacements ===", changes);

  return changes;
}

/* ============================================================================
 * Post-Increment Load/Store Fusion Optimization
 * ============================================================================
 *
 * Fuses LOAD/STORE followed by pointer increment into single post-increment op.
 * Pattern for load:  val = *ptr; ptr = ptr + #offset
 * Becomes:          val = LOAD_POSTINC(ptr, #offset)
 *
 * Pattern for store: *ptr = val; ptr = ptr + #offset
 * Becomes:          STORE_POSTINC(ptr, val, #offset)
 *
 * This is particularly effective for array iteration:
 *   for (i = 0; i < n; i++) sum += *p++;
 *
 * Requirements:
 * - The pointer must be the same in both LOAD/STORE and ADD
 * - The ADD must be: ptr = ptr + immediate (not register)
 * - The immediate offset must be small (1, 2, 4, 8 for valid ARM offsets)
 * - Both instructions must be in the same basic block
 * - LOAD/STORE result (for load) must not be the pointer being incremented
 */

int tcc_ir_find_defining_instruction(TCCIRState *ir, int32_t vreg, int before_idx)
{
  if (!ir || vreg < 0 || before_idx <= 0)
    return -1;

  for (int i = before_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) == vreg)
      return i;
  }
  return -1;
}

int tcc_ir_vreg_has_single_use(TCCIRState *ir, int32_t vreg, int exclude_idx)
{
  if (!ir || vreg < 0)
    return 0;

  int use_count = 0;
  int n = ir->next_instruction_index;

  for (int i = 0; i < n; ++i)
  {
    if (i == exclude_idx)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    if (irop_get_vreg(src1) == vreg || irop_get_vreg(src2) == vreg)
    {
      use_count++;
      if (use_count > 1)
        return 0;
    }
  }
  return use_count == 1;
}

int tcc_ir_opt_block_copy_init(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    /* Check if callee is __aeabi_memset / memset */
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;
    if (strcmp(name, "__aeabi_memset") != 0 && strcmp(name, "memset") != 0)
      continue;

    /* Get memset parameters:
     * __aeabi_memset(dest, size, fill_value)
     *   param0 = dest address (should be Addr[StackLoc[offset]])
     *   param1 = size (should be IMM32)
     *   param2 = fill value (should be IMM32 == 0)
     */
    IROperand param_dest, param_size, param_fill;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &param_dest))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 1, &param_size))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 2, &param_fill))
      continue;

    /* Fill value must be 0 */
    if (irop_get_tag(param_fill) != IROP_TAG_IMM32)
      continue;
    if ((int)irop_get_imm64_ex(ir, param_fill) != 0)
      continue;

    /* Size must be a positive multiple of 4 */
    if (irop_get_tag(param_size) != IROP_TAG_IMM32)
      continue;
    int total_size = (int)irop_get_imm64_ex(ir, param_size);
    if (total_size <= 0 || (total_size & 3) || total_size > 1024)
      continue;

    /* Dest must be a stack offset (address-of form: is_lval=0, tag=STACKOFF) */
    if (irop_get_tag(param_dest) != IROP_TAG_STACKOFF)
      continue;
    int base_offset = (int)irop_get_imm64_ex(ir, param_dest);

    /* Scan forward for consecutive STORE instructions into this stack region.
     * Each STORE writes 4 or 8 bytes at a known offset within [base_offset, base_offset + total_size).
     * The source must be a compile-time constant (SYMREF, IMM32, or I64).
     */
    int store_indices[256];
    int store_offsets[256]; /* byte offset relative to base */
    int store_sizes[256];   /* 4 or 8 bytes */
    IROperand store_values[256];
    int nstores = 0;

    for (int j = i + 1; j < n && nstores < 256; j++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[j];
      if (sq->op == TCCIR_OP_NOP)
        continue;
      if (sq->op != TCCIR_OP_STORE)
        break; /* non-store breaks the pattern */

      IROperand st_dest = tcc_ir_op_get_dest(ir, sq);
      IROperand st_src = tcc_ir_op_get_src1(ir, sq);

      /* Dest must be a local stack offset write */
      if (irop_get_tag(st_dest) != IROP_TAG_STACKOFF || !st_dest.is_local)
        break;

      int st_off = (int)irop_get_imm64_ex(ir, st_dest);
      int rel_off = st_off - base_offset;
      int is_wide = irop_is_64bit(st_dest);
      int st_size = is_wide ? 8 : 4;

      /* Must be within the memset'd region and properly aligned */
      if (rel_off < 0 || rel_off + st_size > total_size || (rel_off & 3))
        break;

      /* Source must be a compile-time constant */
      int src_tag = irop_get_tag(st_src);
      if (src_tag != IROP_TAG_SYMREF && src_tag != IROP_TAG_IMM32 && src_tag != IROP_TAG_I64 &&
          src_tag != IROP_TAG_F32 && src_tag != IROP_TAG_F64)
        break;

      store_indices[nstores] = j;
      store_offsets[nstores] = rel_off;
      store_sizes[nstores] = st_size;
      store_values[nstores] = st_src;
      nstores++;
    }

    /* Need at least 2 stores to be worth optimizing */
    if (nstores < 2)
      continue;

    /* Create the rodata block:
     * 1. Allocate space in rodata section
     * 2. Zero-fill (from the memset)
     * 3. Write constant values + relocations for symbol refs
     */
    size_t rodata_offset = section_add(rodata_section, total_size, 4);
    uint8_t *rodata_ptr = rodata_section->data + rodata_offset;
    memset(rodata_ptr, 0, total_size);

    for (int s = 0; s < nstores; s++)
    {
      int src_tag = irop_get_tag(store_values[s]);
      if (src_tag == IROP_TAG_SYMREF)
      {
        /* Symbol reference: write addend and create relocation */
        IRPoolSymref *symref = irop_get_symref_ex(ir, store_values[s]);
        if (symref && symref->sym)
        {
          write32le(rodata_ptr + store_offsets[s], symref->addend);
          greloc(rodata_section, symref->sym, rodata_offset + store_offsets[s], R_DATA_PTR);
        }
      }
      else if (store_sizes[s] == 8)
      {
        /* I64: write 8 bytes */
        int64_t val = irop_get_imm64_ex(ir, store_values[s]);
        write64le(rodata_ptr + store_offsets[s], (uint64_t)val);
      }
      else
      {
        /* IMM32: write 4 bytes */
        int32_t val = (int32_t)irop_get_imm64_ex(ir, store_values[s]);
        write32le(rodata_ptr + store_offsets[s], val);
      }
    }

    /* Create anonymous symbol pointing to the rodata block */
    CType ctype;
    ctype.t = VT_PTR | VT_CONST;
    ctype.ref = NULL;
    Sym *rodata_sym = get_sym_ref(&ctype, rodata_section, rodata_offset, total_size);

    /* Build BLOCK_COPY operands:
     * dest = STACKOFF(base_offset) with is_local=1
     * src1 = SYMREF pointing to rodata block
     * src2 = IMM32(total_size)
     */
    IROperand bc_dest = irop_make_stackoff(-1, base_offset, 1, 0, 0, IROP_BTYPE_INT32);
    uint32_t sym_pool_idx = tcc_ir_pool_add_symref(ir, rodata_sym, 0, 0);
    IROperand bc_src = irop_make_symref(-1, sym_pool_idx, 0, 0, 1, IROP_BTYPE_INT32);
    IROperand bc_size = irop_make_imm32(-1, total_size, VT_INT);

    /* Allocate 3 new pool entries for the BLOCK_COPY instruction */
    int pool_base = tcc_ir_iroperand_pool_add(ir, bc_dest);
    tcc_ir_iroperand_pool_add(ir, bc_src);
    tcc_ir_iroperand_pool_add(ir, bc_size);

    /* Rewrite first store as BLOCK_COPY */
    IRQuadCompact *first_store = &ir->compact_instructions[store_indices[0]];
    first_store->op = TCCIR_OP_BLOCK_COPY;
    first_store->operand_base = pool_base;

    /* NOP remaining stores */
    for (int s = 1; s < nstores; s++)
      ir->compact_instructions[store_indices[s]].op = TCCIR_OP_NOP;

    /* NOP the memset call and its params */
    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_NOP;

    changes++;
  }

  return changes;
}

/* Replace `memset(&stack[off], N, 0)` with one or two direct STORE #0
 * instructions when N is small (<= 8 bytes).  Covers the case
 * tcc_ir_opt_block_copy_init misses: a trivial zero-initialized local
 * (`unsigned char x1[1] = {0}`) where no follow-up stores feed the rodata
 * materialization heuristic.  Without this, a 1-byte zero-init becomes a
 * full __aeabi_memset call. */
int tcc_ir_opt_small_memset_to_store(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;
    if (strcmp(name, "__aeabi_memset") != 0 && strcmp(name, "memset") != 0)
      continue;

    IROperand p_dst, p_size, p_fill;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &p_dst))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 1, &p_size))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 2, &p_fill))
      continue;

    /* Fill must be 0 */
    if (irop_get_tag(p_fill) != IROP_TAG_IMM32)
      continue;
    if ((int)irop_get_imm64_ex(ir, p_fill) != 0)
      continue;

    /* Size must be a known small positive constant */
    if (irop_get_tag(p_size) != IROP_TAG_IMM32)
      continue;
    int total_size = (int)irop_get_imm64_ex(ir, p_size);
    if (total_size <= 0 || total_size > 8)
      continue;

    /* Dest must be a stack address (LEA form: is_lval=0, is_local=1) */
    if (irop_get_tag(p_dst) != IROP_TAG_STACKOFF || !p_dst.is_local || p_dst.is_lval)
      continue;
    int base_offset = (int)irop_get_imm64_ex(ir, p_dst);

    /* Decompose total_size into 1-2 power-of-2 stores: 8,4,2,1.  Sizes 3/5/6/7
     * use two stores (4+2, 4+1, 4+2 etc); size 7 would need three so we skip. */
    int chunk_btype[2] = {0, 0};
    int chunk_off[2] = {0, 0};
    int nchunks = 0;
    int remaining = total_size;
    int cur_off = 0;
    while (remaining > 0 && nchunks < 2)
    {
      int sz;
      int bt;
      if (remaining >= 8)
      {
        sz = 8;
        bt = IROP_BTYPE_INT64;
      }
      else if (remaining >= 4)
      {
        sz = 4;
        bt = IROP_BTYPE_INT32;
      }
      else if (remaining >= 2)
      {
        sz = 2;
        bt = IROP_BTYPE_INT16;
      }
      else
      {
        sz = 1;
        bt = IROP_BTYPE_INT8;
      }
      chunk_btype[nchunks] = bt;
      chunk_off[nchunks] = base_offset + cur_off;
      nchunks++;
      cur_off += sz;
      remaining -= sz;
    }
    if (remaining != 0)
      continue; /* would need >2 stores, skip */

    /* For 2-chunk case we also need to allocate a NOP slot for the second
     * store.  Look for the closest preceding NOP slot we can repurpose: a
     * PARAM* belonging to this call. */
    int extra_store_idx = -1;
    if (nchunks == 2)
    {
      int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
      for (int j = i - 1; j >= 0; j--)
      {
        IRQuadCompact *pq = &ir->compact_instructions[j];
        if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
          continue;
        IROperand enc = tcc_ir_op_get_src2(ir, pq);
        if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, enc)) != call_id)
          continue;
        extra_store_idx = j;
        break;
      }
      if (extra_store_idx < 0)
        continue;
    }

    /* NOP all params BEFORE rewriting the call slot — ir_opt_nop_call_params
     * needs q to still be a CALL to read its call_id from src2.  If the
     * second-chunk case repurposes one of those slots, we'll un-NOP it
     * by writing the new STORE on top. */
    ir_opt_nop_call_params(ir, i);

    /* Rewrite the call slot as the first STORE.  STORE pool layout is
     * [dest, src1] (has_dest=1, has_src1=1, has_src2=0). */
    IROperand st_dest0 = irop_make_stackoff(-1, chunk_off[0], /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0,
                                            chunk_btype[0]);
    IROperand st_src0 = irop_make_imm32(-1, 0, chunk_btype[0]);
    int pool_base0 = tcc_ir_iroperand_pool_add(ir, st_dest0);
    tcc_ir_iroperand_pool_add(ir, st_src0);
    q->op = TCCIR_OP_STORE;
    q->operand_base = pool_base0;

    if (nchunks == 2)
    {
      IROperand st_dest1 = irop_make_stackoff(-1, chunk_off[1], /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0,
                                              chunk_btype[1]);
      IROperand st_src1 = irop_make_imm32(-1, 0, chunk_btype[1]);
      int pool_base1 = tcc_ir_iroperand_pool_add(ir, st_dest1);
      tcc_ir_iroperand_pool_add(ir, st_src1);
      IRQuadCompact *eq = &ir->compact_instructions[extra_store_idx];
      eq->op = TCCIR_OP_STORE;
      eq->operand_base = pool_base1;
    }

    changes++;
  }

  return changes;
}

/* Returns 1 if an instruction may clobber a value used by a CMP/SETIF
 * we want to CSE across.  Used by tcc_ir_opt_cmp_setif_cse to bail when
 * any intervening op could change the comparison's result. */
static int cse_cmp_op_may_clobber(IRQuadCompact *q)
{
  switch (q->op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_RETURNVALUE: /* terminates BB but doesn't reach CMP@j */
    return 0;
  /* Anything that writes memory or branches is a hard stop.  Calls
   * may write through pointers; jumps cross basic-block boundaries. */
  case TCCIR_OP_STORE:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SET_CHAIN:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
  case TCCIR_OP_INLINE_ASM:
    return 1;
  default:
    return 0;
  }
}

/* Returns 1 if a CMP operand depends on a memory location that any
 * STORE could alias.  STACKOFF lvals do; everything else is safe under
 * the caller's existing "no STOREs between" guard. */
static int cse_cmp_operand_reads_memory(IROperand op)
{
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_lval)
    return 1;
  if (op.is_lval && irop_get_tag(op) == IROP_TAG_VREG)
    return 1;
  if (op.is_lval && irop_get_tag(op) == IROP_TAG_SYMREF)
    return 1;
  return 0;
}

/* CMP+SETIF CSE pass.  Detects pattern:
 *   i:   CMP A, B
 *   i+1: V1 <-- (cond=C)             [SETIF]
 *   ...intervening ops with no clobber...
 *   j:   CMP A', B'   (structurally equal to A, B)
 *   j+1: V2 <-- (cond=C)             [SETIF]
 * And rewrites the second pair to:
 *   j:   NOP
 *   j+1: V2 <-- V1                   [ASSIGN]
 *
 * Subsequent copy-propagation eliminates V2 entirely.  Scoped to a single
 * basic block (no jump-target or terminator between i and j) and bails on
 * any intervening op that could clobber memory or vregs read by the CMPs. */
int tcc_ir_opt_cmp_setif_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 4)
    return 0;

  for (int i = 0; i + 2 < n; i++)
  {
    IRQuadCompact *cmp1 = &ir->compact_instructions[i];
    if (cmp1->op != TCCIR_OP_CMP)
      continue;
    IRQuadCompact *setif1 = &ir->compact_instructions[i + 1];
    if (setif1->op != TCCIR_OP_SETIF)
      continue;

    IROperand setif1_dest = tcc_ir_op_get_dest(ir, setif1);
    int32_t setif1_dest_vr = irop_get_vreg(setif1_dest);
    if (setif1_dest_vr < 0 || setif1_dest.is_lval)
      continue;
    /* Require the SETIF result vreg to be single-def — otherwise later
     * redefinitions could carry the wrong value past our CSE point. */
    if (!tcc_ir_vreg_has_single_def(ir, setif1_dest_vr))
      continue;

    IROperand cmp1_s1 = tcc_ir_op_get_src1(ir, cmp1);
    IROperand cmp1_s2 = tcc_ir_op_get_src2(ir, cmp1);
    IROperand cond1_op = tcc_ir_op_get_src1(ir, setif1);
    int cond1 = (int)irop_get_imm64_ex(ir, cond1_op);
    int s1_btype = irop_get_btype(cmp1_s1);
    int s2_btype = irop_get_btype(cmp1_s2);
    int setif1_btype = irop_get_btype(setif1_dest);

    /* Whether either CMP operand could be invalidated by an intervening
     * STORE — if yes, we'd need stricter aliasing checks beyond the
     * cse_cmp_op_may_clobber guard (which already bails on any STORE). */
    (void)cse_cmp_operand_reads_memory;

    /* Forward scan for a duplicate CMP+SETIF pair. */
    for (int j = i + 2; j + 1 < n; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->is_jump_target)
        break; /* BB boundary */
      if (cse_cmp_op_may_clobber(q))
        break;

      /* If this op writes a vreg used by cmp1, or overwrites setif1's
       * result, bail. */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (!d.is_lval)
        {
          int32_t dvr = irop_get_vreg(d);
          if (dvr >= 0)
          {
            if (dvr == setif1_dest_vr)
              break;
            if (irop_get_tag(cmp1_s1) == IROP_TAG_VREG && !cmp1_s1.is_lval &&
                irop_get_vreg(cmp1_s1) == dvr)
              break;
            if (irop_get_tag(cmp1_s2) == IROP_TAG_VREG && !cmp1_s2.is_lval &&
                irop_get_vreg(cmp1_s2) == dvr)
              break;
          }
        }
      }

      if (q->op != TCCIR_OP_CMP)
        continue;
      IRQuadCompact *setif2 = &ir->compact_instructions[j + 1];
      if (setif2->op != TCCIR_OP_SETIF)
        continue;

      IROperand cmp2_s1 = tcc_ir_op_get_src1(ir, q);
      IROperand cmp2_s2 = tcc_ir_op_get_src2(ir, q);
      IROperand cond2_op = tcc_ir_op_get_src1(ir, setif2);
      int cond2 = (int)irop_get_imm64_ex(ir, cond2_op);

      if (cond1 != cond2)
        continue;
      if (irop_get_btype(cmp2_s1) != s1_btype || irop_get_btype(cmp2_s2) != s2_btype)
        continue;

      /* Structural equality of operands.  Use the public helper that
       * handles vregs, immediates, stack offsets, and symrefs. */
      if (!ir_opt_pure_expr_equal(ir, cmp1_s1, i, cmp2_s1, j, 0))
        continue;
      if (!ir_opt_pure_expr_equal(ir, cmp1_s2, i, cmp2_s2, j, 0))
        continue;

      /* Rewrite: CMP@j becomes NOP, SETIF@j+1 becomes ASSIGN of setif1's
       * vreg.  The destination vreg of SETIF@j+1 is preserved. */
      q->op = TCCIR_OP_NOP;
      setif2->op = TCCIR_OP_ASSIGN;
      IROperand src_vreg = irop_make_vreg(setif1_dest_vr, setif1_btype);
      tcc_ir_set_src1(ir, j + 1, src_vreg);
      tcc_ir_set_src2(ir, j + 1, IROP_NONE);
      changes++;
      break;
    }
  }

  return changes;
}

/* Eliminate `memmove/memcpy(dst_ptr, &stack_tmp, N)` when the only writes to
 * stack_tmp[0..N) are local STOREs that precede the call in the same basic
 * block.  Each contributing STORE is rewritten to a STORE_INDEXED targeting
 * the destination pointer at its original offset, and the call + params + the
 * `LEA &stack_tmp` are NOPed.  Skipping the memmove call removes a function
 * call from each hot iteration of a `*c = *d * s`-style complex-assignment
 * loop and frees the stack temp for DCE. */
int tcc_ir_opt_memmove_to_indexed_stores(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    /* Callee must be memmove/memcpy family (returns first arg). */
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;
    int is_memmove_like = strcmp(name, "__aeabi_memmove") == 0 ||
                          strcmp(name, "__aeabi_memmove4") == 0 ||
                          strcmp(name, "__aeabi_memmove8") == 0 ||
                          strcmp(name, "__aeabi_memcpy") == 0 ||
                          strcmp(name, "__aeabi_memcpy4") == 0 ||
                          strcmp(name, "__aeabi_memcpy8") == 0 ||
                          strcmp(name, "memmove") == 0 ||
                          strcmp(name, "memcpy") == 0;
    if (!is_memmove_like)
      continue;

    IROperand p_dst, p_src, p_size;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &p_dst))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 1, &p_src))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 2, &p_size))
      continue;

    /* Size must be a small positive constant divisible by 4. */
    if (irop_get_tag(p_size) != IROP_TAG_IMM32)
      continue;
    int total_size = (int)irop_get_imm64_ex(ir, p_size);
    if (total_size <= 0 || (total_size & 3) || total_size > 64)
      continue;

    /* Source must be an address of a local stack offset.  May arrive in one
     * of two forms:
     *   (a) Direct LEA-form operand: STACKOFF with is_local=1, is_lval=0.
     *   (b) A vreg that was set by `LEA/ASSIGN vr <- Addr[StackLoc[X]]`
     *       earlier in the same basic block. */
    int tmp_base;
    int lea_idx = -1; /* instruction index of the LEA that produced p_src, if any */
    if (irop_get_tag(p_src) == IROP_TAG_STACKOFF && p_src.is_local && !p_src.is_lval)
    {
      tmp_base = (int)irop_get_imm64_ex(ir, p_src);
    }
    else if (irop_get_tag(p_src) == IROP_TAG_VREG && irop_has_vreg(p_src) && !p_src.is_lval)
    {
      int32_t src_vr = irop_get_vreg(p_src);
      if (src_vr < 0)
        continue;
      /* Scan backwards for the most recent `<vr> <- Addr[StackLoc[X]]`. */
      int found_lea = 0;
      tmp_base = 0;
      for (int j = i - 1; j >= 0; j--)
      {
        IRQuadCompact *lq = &ir->compact_instructions[j];
        if (lq->op == TCCIR_OP_NOP)
          continue;
        if (lq->is_jump_target)
          break;
        if (lq->op == TCCIR_OP_JUMP || lq->op == TCCIR_OP_JUMPIF || lq->op == TCCIR_OP_IJUMP)
          break;
        if (!irop_config[lq->op].has_dest)
          continue;
        IROperand ld = tcc_ir_op_get_dest(ir, lq);
        if (!irop_has_vreg(ld) || irop_get_vreg(ld) != src_vr || ld.is_lval)
          continue;
        /* This op writes our vreg.  Must be a LEA/ASSIGN whose src1 is an
         * Addr[StackLoc[X]] (is_local=1, is_lval=0). */
        if (lq->op != TCCIR_OP_LEA && lq->op != TCCIR_OP_ASSIGN)
          break;
        IROperand ls = tcc_ir_op_get_src1(ir, lq);
        if (irop_get_tag(ls) != IROP_TAG_STACKOFF || !ls.is_local || ls.is_lval)
          break;
        tmp_base = (int)irop_get_imm64_ex(ir, ls);
        lea_idx = j;
        found_lea = 1;
        break;
      }
      if (!found_lea)
        continue;
    }
    else
    {
      continue;
    }

    /* Destination must be a vreg pointer (value, not lvalue, not stack-local). */
    if (irop_get_tag(p_dst) != IROP_TAG_VREG || p_dst.is_lval || p_dst.is_local || p_dst.is_const)
      continue;
    if (!irop_has_vreg(p_dst))
      continue;
    int32_t dst_vr = irop_get_vreg(p_dst);
    if (dst_vr < 0)
      continue;
    (void)dst_vr;

    /* Walk backwards through the same basic block looking for STOREs that
     * fully cover the temp range.  Stop on any other op (other call, jump,
     * non-STORE write) or a previously NOPed slot. */
    int store_indices[16];
    int store_offsets[16];
    int nstores = 0;
    int total_covered = 0;
    int aborted = 0;

    for (int j = i - 1; j >= 0 && nstores < 16; j--)
    {
      IRQuadCompact *sq = &ir->compact_instructions[j];
      if (sq->op == TCCIR_OP_NOP)
        continue;
      if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
        continue; /* memmove's own params */
      if (sq->is_jump_target)
        break;
      if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF || sq->op == TCCIR_OP_IJUMP)
        break;

      if (sq->op != TCCIR_OP_STORE)
      {
        /* Allow benign in-between ops (LOAD, ASSIGN, ADD, arithmetic into
         * vregs, FUNCCALLs).  Aliasing safety is enforced by the global
         * scan below — which verifies no operand anywhere references the
         * temp range.  Here we only bail if this very op writes into the
         * temp range, which the global scan would also catch but is cheap
         * to detect now. */
        if (irop_config[sq->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, sq);
          if (irop_get_tag(d) == IROP_TAG_STACKOFF && d.is_local && d.is_lval)
          {
            int doff = (int)irop_get_imm64_ex(ir, d);
            if (doff >= tmp_base && doff < tmp_base + total_size)
            {
              aborted = 1;
              break;
            }
          }
        }
        continue;
      }

      /* This is a STORE.  Check if it targets our temp range. */
      IROperand st_dest = tcc_ir_op_get_dest(ir, sq);
      if (irop_get_tag(st_dest) != IROP_TAG_STACKOFF || !st_dest.is_local || !st_dest.is_lval)
        continue;
      int st_off = (int)irop_get_imm64_ex(ir, st_dest);
      if (st_off < tmp_base || st_off >= tmp_base + total_size)
        continue;

      int is_wide = irop_is_64bit(st_dest);
      int st_size = is_wide ? 8 : 4;
      if (st_off + st_size > tmp_base + total_size)
      {
        aborted = 1;
        break;
      }

      /* Source value must be a vreg or immediate (rewriting requires the
       * value to be straightforwardly usable in a STORE_INDEXED). */
      IROperand st_src = tcc_ir_op_get_src1(ir, sq);
      int src_tag = irop_get_tag(st_src);
      if (src_tag != IROP_TAG_VREG && src_tag != IROP_TAG_IMM32 &&
          src_tag != IROP_TAG_I64 && src_tag != IROP_TAG_F32 && src_tag != IROP_TAG_F64)
      {
        aborted = 1;
        break;
      }

      store_indices[nstores] = j;
      store_offsets[nstores] = st_off - tmp_base;
      nstores++;
      total_covered += st_size;

      if (total_covered >= total_size)
        break;
    }

    if (aborted || nstores == 0 || total_covered != total_size)
      continue;

    /* Verify the stack temp is not used anywhere else as a load source or
     * have its address taken elsewhere.  Conservative: scan all instructions
     * (except the memmove call's own params + the contributing stores). */
    int safe = 1;
    for (int j = 0; j < n; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *sq = &ir->compact_instructions[j];
      if (sq->op == TCCIR_OP_NOP)
        continue;
      /* Skip the contributing stores. */
      int is_store_of_ours = 0;
      for (int s = 0; s < nstores; s++)
      {
        if (store_indices[s] == j)
        {
          is_store_of_ours = 1;
          break;
        }
      }
      if (is_store_of_ours)
        continue;
      /* Skip the LEA that produced p_src (the temp's address) — it's about
       * to be NOPed alongside the memmove call. */
      if (j == lea_idx)
        continue;
      /* Skip params of the memmove call. */
      if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
      {
        IROperand pop_enc = tcc_ir_op_get_src2(ir, sq);
        IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
        if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, pop_enc)) ==
            TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2)))
          continue;
      }

      /* Check operands for tmp_base address use or stack offset use. */
      for (int si = 0; si < 3; si++)
      {
        IROperand op;
        if (si == 0 && irop_config[sq->op].has_dest)
          op = tcc_ir_op_get_dest(ir, sq);
        else if (si == 1 && irop_config[sq->op].has_src1)
          op = tcc_ir_op_get_src1(ir, sq);
        else if (si == 2 && irop_config[sq->op].has_src2)
          op = tcc_ir_op_get_src2(ir, sq);
        else
          continue;
        if (irop_get_tag(op) != IROP_TAG_STACKOFF)
          continue;
        if (!op.is_local)
          continue;
        int off = (int)irop_get_imm64_ex(ir, op);
        if (off < tmp_base || off >= tmp_base + total_size)
          continue;
        /* The stack temp is referenced elsewhere — bail. */
        safe = 0;
        break;
      }
      if (!safe)
        break;
    }
    if (!safe)
      continue;

    /* The destination vreg must be defined before the earliest store we
     * plan to rewrite — otherwise the rewritten STORE_INDEXEDs would
     * reference an uninitialized vreg.  Scan backwards from the memmove
     * for the most recent definition of dst_vr; require it to be at an
     * index strictly less than every store_indices[] entry. */
    {
      int earliest_store_idx = i;
      for (int s = 0; s < nstores; s++)
      {
        if (store_indices[s] < earliest_store_idx)
          earliest_store_idx = store_indices[s];
      }
      int dst_def_idx = -1;
      for (int j = i - 1; j >= 0; j--)
      {
        IRQuadCompact *sq = &ir->compact_instructions[j];
        if (sq->op == TCCIR_OP_NOP)
          continue;
        if (!irop_config[sq->op].has_dest)
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, sq);
        if (irop_has_vreg(d) && irop_get_vreg(d) == dst_vr && !d.is_lval)
        {
          dst_def_idx = j;
          break;
        }
      }
      if (dst_def_idx < 0 || dst_def_idx >= earliest_store_idx)
        continue;
    }

    /* If we tracked an LEA that produced p_src, verify its result vreg is
     * referenced exactly once outside the LEA itself: by the memmove's
     * PARAM1.  Otherwise some other instruction reads the temp's address
     * (and may, through that pointer, observe writes we're about to
     * relocate). */
    if (lea_idx >= 0)
    {
      int32_t lea_vr = irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[lea_idx]));
      int lea_other_uses = 0;
      for (int j = 0; j < n && !lea_other_uses; j++)
      {
        if (j == lea_idx)
          continue;
        IRQuadCompact *sq = &ir->compact_instructions[j];
        if (sq->op == TCCIR_OP_NOP)
          continue;
        /* The memmove PARAM1 is an expected use — skip. */
        if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
        {
          IROperand pop_enc = tcc_ir_op_get_src2(ir, sq);
          IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
          if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, pop_enc)) ==
                  TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2)) &&
              TCCIR_DECODE_PARAM_IDX((uint32_t)irop_get_imm64_ex(ir, pop_enc)) == 1)
            continue;
        }
        for (int si = 0; si < 3; si++)
        {
          IROperand op2;
          if (si == 0 && irop_config[sq->op].has_dest)
            op2 = tcc_ir_op_get_dest(ir, sq);
          else if (si == 1 && irop_config[sq->op].has_src1)
            op2 = tcc_ir_op_get_src1(ir, sq);
          else if (si == 2 && irop_config[sq->op].has_src2)
            op2 = tcc_ir_op_get_src2(ir, sq);
          else
            continue;
          if (irop_has_vreg(op2) && irop_get_vreg(op2) == lea_vr)
          {
            lea_other_uses = 1;
            break;
          }
        }
      }
      if (lea_other_uses)
        continue;
    }

    /* Rewrite each contributing STORE to STORE_INDEXED targeting the
     * destination pointer with the appropriate immediate offset. */
    for (int s = 0; s < nstores; s++)
    {
      int sidx = store_indices[s];
      IRQuadCompact *sq = &ir->compact_instructions[sidx];
      IROperand st_src = tcc_ir_op_get_src1(ir, sq);
      IROperand st_dest_old = tcc_ir_op_get_dest(ir, sq);
      int offset = store_offsets[s];

      /* Build the STORE_INDEXED operand block:
       *   slot 0: dest = dst_vr (pointer, no lval)
       *   slot 1: src1 = stored value
       *   slot 2: src2 = immediate index (byte offset)
       *   slot 3: scale = 0 (byte-offset mode)
       */
      tcc_ir_pool_ensure(ir, 4);
      int new_base = ir->iroperand_pool_count;

      IROperand base = p_dst;
      base.is_lval = 0;
      /* Preserve the destination's natural width as the base; the index field
       * carries the byte offset.  The store width comes from src1's btype. */
      tcc_ir_pool_add(ir, base);
      tcc_ir_pool_add(ir, st_src);
      IROperand index_op = irop_make_imm32(-1, offset, IROP_BTYPE_INT32);
      tcc_ir_pool_add(ir, index_op);
      IROperand scale_op = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
      tcc_ir_pool_add(ir, scale_op);

      sq->op = TCCIR_OP_STORE_INDEXED;
      sq->operand_base = new_base;
      (void)st_dest_old;
    }

    /* NOP the memmove call and all its FUNCPARAMVAL/FUNCPARAMVOID params. */
    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_NOP;

    /* NOP the LEA that took the address of the temp — its only user (the
     * memmove's PARAM1) is gone, and the stack temp is now dead. */
    if (lea_idx >= 0)
      ir->compact_instructions[lea_idx].op = TCCIR_OP_NOP;

    changes++;
  }

  return changes;
}
