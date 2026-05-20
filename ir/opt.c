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
