/*
 *  TCC IR - Block-local value tracking (constant + LEA lattice, flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* BB-local VAR-constant + LEA lattice feeding the fresh-IR const_prop cascade; Branch A (kept flat), see
 * docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include <math.h> /* isinf/isnan/isfinite/signbit — not pulled in transitively on YasOS libm */

#include "ir.h"
#include "opt.h"
#include "opt_du.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "memory/small_sequence.h"

/* Inline-first per-instruction scratch (heap only past the inline cap). */
TCC_SMALL_SEQUENCE_DEFINE(VtIntSeq, int, 256)

typedef struct
{
  int gen;
  int def_gen;
  int64_t value;
  int def_idx;
} VRegConstState;

typedef struct
{
  int gen;
  int var_pos;
} LeaMapGenEntry;

typedef struct
{
  int max_vreg;
  int max_tmp;
  int has_control_flow;
  int has_vla;
  int has_prefetch;
  int has_ijump;
  uint8_t *is_merge;
  uint8_t *var_def_count;
  uint8_t *is_addrtaken;
} ValueTrackingAnalysis;

#define VT_IS_CONST(st, pos) ((st)[pos].gen == vt_gen)
#define VT_HAS_DEF(st, pos) ((st)[pos].def_gen == vt_def_gen && (st)[pos].def_idx >= 0)
#define VT_SET_CONST(st, pos, val_)                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].gen = vt_gen;                                                                                            \
    (st)[pos].def_gen = vt_def_gen;                                                                                    \
    (st)[pos].value = (val_);                                                                                          \
    (st)[pos].def_idx = -1;                                                                                            \
  } while (0)
#define VT_SET_CONST_DEF(st, pos, val_, idx_)                                                                          \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].gen = vt_gen;                                                                                            \
    (st)[pos].def_gen = vt_def_gen;                                                                                    \
    (st)[pos].value = (val_);                                                                                          \
    (st)[pos].def_idx = (idx_);                                                                                        \
  } while (0)
#define VT_INVALIDATE(st, pos)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].gen = 0;                                                                                                 \
  } while (0)
#define VT_CLEAR_DEF(st, pos)                                                                                          \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].def_gen = 0;                                                                                             \
  } while (0)

#define VT_MAX_ADDRTAKEN 64

typedef struct
{
  TCCIRState *ir;
  int n;
  int max_vreg;
  int max_tmp;
  int has_control_flow;
  int has_vla;
  int has_prefetch;
  int has_ijump;
  uint8_t *is_merge;
  uint8_t *var_def_count;
  uint8_t *is_addrtaken;
  VRegConstState *state;
  LeaMapGenEntry *lea_map;
  LeaMapGenEntry *lea_var_map;
  int vt_gen;
  int vt_def_gen;
  int vt_lea_gen;
  int vt_in_dead_zone;
  int addrtaken_list[VT_MAX_ADDRTAKEN];
  int num_addrtaken;
  int addrtaken_overflow;
  int changes;
} VTCtx;

static int vt_fname_match(const char *fname, const char *name)
{
  return fname && strcmp(fname, name) == 0;
}

static void vt_analyze(TCCIRState *ir, int n, ValueTrackingAnalysis *a)
{
  small_sequence(VtIntSeq) pred_count_owner = {0};
  VtIntSeq_init(&pred_count_owner, (size_t)n);
  int *pred_count = VtIntSeq_data(&pred_count_owner);
  a->is_merge = tcc_mallocz((n + 7) / 8);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_NOP)
    {
      IROperand ops[3] = {tcc_ir_op_get_dest(ir, q), tcc_ir_op_get_src1(ir, q), tcc_ir_op_get_src2(ir, q)};
      for (int k = 0; k < 3; k++)
      {
        int32_t vr = irop_get_vreg(ops[k]);
        if (vr < 0)
          continue;
        int type = TCCIR_DECODE_VREG_TYPE(vr);
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (type == TCCIR_VREG_TYPE_VAR && pos > a->max_vreg)
          a->max_vreg = pos;
        else if (type == TCCIR_VREG_TYPE_TEMP && pos > a->max_tmp)
          a->max_tmp = pos;
      }
    }

    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      a->has_control_flow = 1;
      if (target >= 0 && target < n)
      {
        pred_count[target]++;
        if (i > target)
          a->is_merge[target / 8] |= (1 << (target % 8));
      }
    }
    if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      int table_id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      a->has_control_flow = 1;
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
        {
          int target = table->targets[j];
          if (target >= 0 && target < n)
            pred_count[target]++;
        }
        if (table->default_target >= 0 && table->default_target < n)
          pred_count[table->default_target]++;
      }
    }
    if (i + 1 < n && q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_RETURNVALUE && q->op != TCCIR_OP_RETURNVOID &&
        q->op != TCCIR_OP_SWITCH_TABLE)
      pred_count[i + 1]++;
    if (q->op == TCCIR_OP_IJUMP)
      a->has_control_flow = a->has_ijump = 1;
    if (q->op == TCCIR_OP_VLA_ALLOC || q->op == TCCIR_OP_VLA_SP_SAVE || q->op == TCCIR_OP_VLA_SP_RESTORE)
      a->has_vla = 1;
    if (q->op == TCCIR_OP_PREFETCH)
      a->has_prefetch = 1;
  }

  for (int i = 0; i < n; i++)
  {
    if (pred_count[i] > 1)
      a->is_merge[i / 8] |= (1 << (i % 8));
  }

  a->var_def_count = tcc_mallocz(a->max_vreg + 1);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos <= a->max_vreg && a->var_def_count[pos] < 2)
      a->var_def_count[pos]++;
  }

  a->is_addrtaken = tcc_mallocz((a->max_vreg + 8) / 8);
  for (int pos = 0; pos <= a->max_vreg; pos++)
  {
    int32_t vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, pos);
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
    if (interval && (interval->addrtaken || interval->is_volatile))
      a->is_addrtaken[pos / 8] |= (1 << (pos % 8));
  }
}

static int vt_var_pos(IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    return TCCIR_DECODE_VREG_POSITION(vr);
  return -1;
}

static void vt_mark_dest_const(VRegConstState *state, int max_vreg, int vt_gen, int vt_def_gen, IROperand call_dest,
                               int64_t value)
{
  int32_t dv = irop_get_vreg(call_dest);
  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
  {
    int dp = TCCIR_DECODE_VREG_POSITION(dv);
    if (dp >= 0 && dp <= max_vreg)
      VT_SET_CONST(state, dp, value);
  }
}

static IROperand vt_rewrite_call_as_assign(TCCIRState *ir, IRQuadCompact *q, int i, IROperand src)
{
  IROperand call_dest = tcc_ir_op_get_dest(ir, q);
  ir_opt_nop_call_params(ir, i);
  q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_dest(ir, i, call_dest);
  tcc_ir_set_src1(ir, i, src);
  tcc_ir_set_src2(ir, i, IROP_NONE);
  return call_dest;
}

static IROperand vt_make_int_imm(TCCIRState *ir, int64_t result, int imm32_btype)
{
  if (result == (int32_t)result)
    return irop_make_imm32(-1, (int32_t)result, imm32_btype);
  return irop_make_i64(-1, tcc_ir_pool_add_i64(ir, result), IROP_BTYPE_INT64);
}

static int vt_operand_const(TCCIRState *ir, IROperand operand, int i, VRegConstState *state, int max_vreg, int vt_gen,
                            int use_eval, int64_t *value)
{
  if (irop_is_immediate(operand))
  {
    *value = irop_get_imm64_ex(ir, operand);
    return 1;
  }
  int32_t vr = irop_get_vreg(operand);
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos >= 0 && pos <= max_vreg && VT_IS_CONST(state, pos))
    {
      *value = state[pos].value;
      return 1;
    }
  }
  if (use_eval)
  {
    uint64_t uval;
    if (ir_opt_eval_const_u64(ir, operand, i, &uval, 0))
    {
      *value = (int64_t)uval;
      return 1;
    }
  }
  return 0;
}

static int32_t vt_resolve_copy_root(TCCIRState *ir, int32_t vr, int i)
{
  for (int depth = 0; depth < 8 && vr >= 0; depth++)
  {
    int def = tcc_ir_find_defining_instruction(ir, vr, i);
    if (def < 0)
      break;
    IRQuadCompact *dq = &ir->compact_instructions[def];
    if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_STORE)
      break;
    IROperand dsrc = tcc_ir_op_get_src1(ir, dq);
    int32_t svr = irop_get_vreg(dsrc);
    if (svr < 0 || dsrc.is_lval)
      break;
    vr = svr;
  }
  return vr;
}

static int vt_fold_call_to_const(TCCIRState *ir, IRQuadCompact *q, int i, VRegConstState *state, int max_vreg,
                                 int vt_gen, int vt_def_gen, int64_t result, int btype)
{
  IROperand call_dest = vt_rewrite_call_as_assign(ir, q, i, vt_make_int_imm(ir, result, btype));
  vt_mark_dest_const(state, max_vreg, vt_gen, vt_def_gen, call_dest, result);
  return 1;
}

static int vt_try_fold_long_compare(TCCIRState *ir, IRQuadCompact *q, int i, const char *fname, VRegConstState *state,
                                    int max_vreg, int vt_gen, int vt_def_gen, int dest_pos)
{
  (void)dest_pos;
  int is_lcmp = vt_fname_match(fname, "__aeabi_lcmp");
  int is_ulcmp = vt_fname_match(fname, "__aeabi_ulcmp");
  if (is_lcmp || is_ulcmp)
  {
    IROperand arg0, arg1;
    if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
    {
      int64_t val0 = 0, val1 = 0;
      int arg0_known = vt_operand_const(ir, arg0, i, state, max_vreg, vt_gen, 1, &val0);
      int arg1_known = vt_operand_const(ir, arg1, i, state, max_vreg, vt_gen, 1, &val1);

      if (arg0_known && arg1_known)
      {
        int result;
        if (is_ulcmp)
        {
          uint64_t u0 = (uint64_t)val0, u1 = (uint64_t)val1;
          result = (u0 > u1) - (u0 < u1);
        }
        else
        {
          result = (val0 > val1) - (val0 < val1);
        }

        vt_rewrite_call_as_assign(ir, q, i, irop_make_imm32(-1, result, IROP_BTYPE_INT32));
        LOG_IR_GEN("VALUE_TRACK: %s(%lld, %lld) = %d at i=%d -> folded", fname, (long long)val0, (long long)val1,
                   result, i);
        return 1;
      }

      {
        int32_t vr0 = vt_resolve_copy_root(ir, irop_get_vreg(arg0), i);
        int32_t vr1 = vt_resolve_copy_root(ir, irop_get_vreg(arg1), i);
        LOG_IR_GEN("VALUE_TRACK: %s resolved at i=%d: vr0=%d vr1=%d (orig %d %d)", fname, i, vr0, vr1,
                   irop_get_vreg(arg0), irop_get_vreg(arg1));
        if (vr0 >= 0 && vr0 == vr1 && !arg0.is_lval && !arg1.is_lval)
        {
          vt_rewrite_call_as_assign(ir, q, i, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
          LOG_IR_GEN("VALUE_TRACK: %s(vreg%d, vreg%d) = 0 at i=%d -> same-vreg fold", fname, vr0, vr1, i);
          return 1;
        }
      }
    }
  }
  return 0;
}

static int vt_try_fold_long_divmod(TCCIRState *ir, IRQuadCompact *q, int i, const char *fname, VRegConstState *state,
                                   int max_vreg, int vt_gen, int vt_def_gen, int dest_pos)
{
  int is_ldivmod = vt_fname_match(fname, "__aeabi_ldivmod");
  int is_uldivmod = vt_fname_match(fname, "__aeabi_uldivmod");
  int is_lmod = vt_fname_match(fname, "__aeabi_lmod");
  int is_ulmod = vt_fname_match(fname, "__aeabi_ulmod");
  if (!(is_ldivmod || is_uldivmod || is_lmod || is_ulmod))
    return 0;

  IROperand arg0, arg1;
  if (!ir_opt_get_call_param_operand(ir, i, 0, &arg0) || !ir_opt_get_call_param_operand(ir, i, 1, &arg1))
    return 0;
  if (!irop_is_immediate(arg0) || !irop_is_immediate(arg1))
    return 0;

  int64_t val0 = irop_get_imm64_ex(ir, arg0);
  int64_t val1 = irop_get_imm64_ex(ir, arg1);
  if (val1 == 0)
    return 0;

  int is_signed = is_ldivmod || is_lmod;
  int is_div = is_ldivmod || is_uldivmod;
  /* INT64_MIN /(or %) -1 is C UB and would trap the host divide; leave the runtime call */
  if (is_signed && val0 == INT64_MIN && val1 == -1)
    return 0;

  int64_t result;
  if (is_div)
    result = is_signed ? val0 / val1 : (int64_t)((uint64_t)val0 / (uint64_t)val1);
  else
    result = is_signed ? val0 % val1 : (int64_t)((uint64_t)val0 % (uint64_t)val1);

  vt_rewrite_call_as_assign(ir, q, i, vt_make_int_imm(ir, result, IROP_BTYPE_INT64));
  LOG_IR_GEN("VALUE_TRACK: %s(%lld, %lld) = %lld at i=%d -> folded", fname, (long long)val0, (long long)val1,
             (long long)result, i);
  /* CALL now defines dest VAR; invalidate so the stale pre-call constant isn't forwarded (the continue skips the tail) */
  if (dest_pos >= 0 && dest_pos <= max_vreg)
    VT_INVALIDATE(state, dest_pos);
  return 1;
}

static int vt_try_fold_float_compare(TCCIRState *ir, IRQuadCompact *q, int i, const char *fname, VRegConstState *state,
                                     int max_vreg, int vt_gen, int vt_def_gen, int dest_pos)
{
  (void)dest_pos;
  {
    int is_fcmp = vt_fname_match(fname, "__aeabi_cfcmple") || vt_fname_match(fname, "__aeabi_cfcmpeq");
    int is_dcmp = vt_fname_match(fname, "__aeabi_cdcmple") || vt_fname_match(fname, "__aeabi_cdcmpeq");
    if (is_fcmp || is_dcmp)
    {
      IROperand arg0, arg1;
      if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
      {
        int64_t a0 = 0, a1 = 0;
        int a0_ok = vt_operand_const(ir, arg0, i, state, max_vreg, vt_gen, 0, &a0);
        int a1_ok = vt_operand_const(ir, arg1, i, state, max_vreg, vt_gen, 0, &a1);
        if (a0_ok && a1_ok)
        {
          int is_nan;
          int result = ir_softfp_cmp3(is_dcmp, a0, a1, &is_nan);
          /* NaN -> IEEE unordered; the (>)-(<) collapse loses that, so leave the runtime call */
          if (is_nan)
            goto skip_fcmp_val_fold;
          vt_rewrite_call_as_assign(ir, q, i, irop_make_imm32(-1, result, IROP_BTYPE_INT32));
          LOG_IR_GEN("VALUE_TRACK: %s -> %d at i=%d (float cmp fold)", fname, result, i);
          return 1;
        skip_fcmp_val_fold:;
        }
      }
    }
  }
  return 0;
}

static int vt_try_fold_bswap(TCCIRState *ir, IRQuadCompact *q, int i, const char *fname, VRegConstState *state,
                             int max_vreg, int vt_gen, int vt_def_gen, int dest_pos)
{
  (void)dest_pos;
  {
    int is_bswap32 = vt_fname_match(fname, "__bswapsi2");
    int is_bswap64 = vt_fname_match(fname, "__bswapdi3");
    if (is_bswap32 || is_bswap64)
    {
      IROperand arg0;
      if (ir_opt_get_call_param_operand(ir, i, 0, &arg0))
      {
        int64_t val0 = 0;
        int arg0_known = vt_operand_const(ir, arg0, i, state, max_vreg, vt_gen, 0, &val0);
        if (arg0_known)
        {
          int64_t result;
          if (is_bswap32)
          {
            uint32_t x = (uint32_t)val0;
            result = (int64_t)(int32_t)(((x >> 24) & 0xFFU) | ((x >> 8) & 0xFF00U) | ((x << 8) & 0xFF0000U) |
                                        ((x << 24) & 0xFF000000U));
          }
          else
          {
            uint64_t x = (uint64_t)val0;
            result =
                (int64_t)(((x >> 56) & 0xFFULL) | ((x >> 40) & 0xFF00ULL) | ((x >> 24) & 0xFF0000ULL) |
                          ((x >> 8) & 0xFF000000ULL) | ((x << 8) & 0xFF00000000ULL) | ((x << 24) & 0xFF0000000000ULL) |
                          ((x << 40) & 0xFF000000000000ULL) | ((x << 56) & 0xFF00000000000000ULL));
          }
          vt_fold_call_to_const(ir, q, i, state, max_vreg, vt_gen, vt_def_gen, result, IROP_BTYPE_INT32);
          LOG_IR_GEN("VALUE_TRACK: %s(%lld) = %lld at i=%d -> folded", fname, (long long)val0, (long long)result, i);
          return 1;
        }
      }
    }
  }
  return 0;
}

static int vt_try_fold_float_classify(TCCIRState *ir, IRQuadCompact *q, int i, const char *fname, VRegConstState *state,
                                      int max_vreg, int vt_gen, int vt_def_gen, int dest_pos)
{
  (void)dest_pos;
  {
    int is_isinff = vt_fname_match(fname, "isinff") || vt_fname_match(fname, "__isinff");
    int is_isinfd = vt_fname_match(fname, "isinf") || vt_fname_match(fname, "__isinfd") || vt_fname_match(fname, "__isinf");
    int is_isnanf = vt_fname_match(fname, "isnanf") || vt_fname_match(fname, "__isnanf");
    int is_isnand = vt_fname_match(fname, "isnan") || vt_fname_match(fname, "__isnand") || vt_fname_match(fname, "__isnan");
    int is_finitef = vt_fname_match(fname, "finitef") || vt_fname_match(fname, "__finitef");
    int is_finited = vt_fname_match(fname, "finite") || vt_fname_match(fname, "__finite");
    int is_fp32 = is_isinff || is_isnanf || is_finitef;
    int is_fp64 = is_isinfd || is_isnand || is_finited;
    if (is_fp32 || is_fp64)
    {
      IROperand arg0;
      if (ir_opt_get_call_param_operand(ir, i, 0, &arg0))
      {
        int64_t bits = 0;
        int arg0_known = vt_operand_const(ir, arg0, i, state, max_vreg, vt_gen, 0, &bits);
        if (arg0_known)
        {
          double v = is_fp32 ? (double)ir_bits_to_f(bits) : ir_bits_to_d(bits);
          int result;
          if (is_isinff || is_isinfd)
            result = isinf(v) ? (signbit(v) ? -1 : 1) : 0;
          else if (is_isnanf || is_isnand)
            result = isnan(v) ? 1 : 0;
          else
            result = isfinite(v) ? 1 : 0;
          vt_fold_call_to_const(ir, q, i, state, max_vreg, vt_gen, vt_def_gen, result, IROP_BTYPE_INT32);
          LOG_IR_GEN("VALUE_TRACK: %s(0x%llx) = %d at i=%d -> folded", fname, (unsigned long long)bits, result, i);
          return 1;
        }
      }
    }
  }
  return 0;
}

static int vt_try_fold_long_arithmetic(TCCIRState *ir, IRQuadCompact *q, int i, const char *fname,
                                       VRegConstState *state, int max_vreg, int vt_gen, int vt_def_gen, int dest_pos)
{
  {
    int is_llsl = vt_fname_match(fname, "__aeabi_llsl");
    int is_llsr = vt_fname_match(fname, "__aeabi_llsr");
    int is_lasr = vt_fname_match(fname, "__aeabi_lasr");
    int is_lmul = vt_fname_match(fname, "__aeabi_lmul");
    if (is_llsl || is_llsr || is_lasr || is_lmul)
    {
      IROperand arg0, arg1;
      if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
      {
        int64_t val0 = 0, val1 = 0;
        int arg0_known = vt_operand_const(ir, arg0, i, state, max_vreg, vt_gen, 1, &val0);
        int arg1_known = vt_operand_const(ir, arg1, i, state, max_vreg, vt_gen, 0, &val1);

        if (arg0_known && arg1_known)
        {
          int64_t result;
          if (is_llsl)
            result = (int64_t)((uint64_t)val0 << (val1 & 63));
          else if (is_llsr)
            result = (int64_t)((uint64_t)val0 >> (val1 & 63));
          else if (is_lasr)
            result = val0 >> (val1 & 63);
          else /* is_lmul */
            result = val0 * val1;

          vt_fold_call_to_const(ir, q, i, state, max_vreg, vt_gen, vt_def_gen, result, IROP_BTYPE_INT64);
          LOG_IR_GEN("VALUE_TRACK: %s(%lld, %lld) = %lld at i=%d -> folded", fname, (long long)val0, (long long)val1,
                     (long long)result, i);
          return 1;
        }

        if (!is_lmul && arg1_known)
        {
          TccIrOp ir_op = is_llsl ? TCCIR_OP_SHL : is_llsr ? TCCIR_OP_SHR : TCCIR_OP_SAR;
          IROperand call_dest = tcc_ir_op_get_dest(ir, q);
          ir_opt_nop_call_params(ir, i);
          q->op = ir_op;
          tcc_ir_set_dest(ir, i, call_dest);
          arg0.btype = IROP_BTYPE_INT64;
          tcc_ir_set_src1(ir, i, arg0);
          tcc_ir_set_src2(ir, i, irop_make_imm32(-1, (int32_t)(val1 & 63), IROP_BTYPE_INT32));
          LOG_IR_GEN("VALUE_TRACK: %s(vreg, %lld) at i=%d -> lowered to IR shift", fname, (long long)val1, i);
          /* CALL redefines dest VAR with a runtime shift; invalidate or the stale pre-call constant forwards to a later
           * read (longlong seed) */
          if (dest_pos >= 0 && dest_pos <= max_vreg)
            VT_INVALIDATE(state, dest_pos);
          return 1;
        }
      }
    }
  }
  return 0;
}

typedef struct
{
  const char *name;
  int kind;
  int nargs;
} VTSoftFloatOp;

static int vt_soft_float_kind(const char *name, int *nargs)
{
  static const VTSoftFloatOp ops[] = {
      {"__aeabi_fadd", 1, 2},    {"__aeabi_fsub", 2, 2},    {"__aeabi_fmul", 3, 2},    {"__aeabi_fdiv", 4, 2},
      {"__aeabi_dadd", 0x81, 2}, {"__aeabi_dsub", 0x82, 2}, {"__aeabi_dmul", 0x83, 2}, {"__aeabi_ddiv", 0x84, 2},
      {"__aeabi_f2iz", 5, 1},    {"__aeabi_f2uiz", 6, 1},   {"__aeabi_i2f", 7, 1},     {"__aeabi_ui2f", 8, 1},
      {"__aeabi_f2d", 9, 1},     {"__aeabi_d2f", 10, 1},    {"__aeabi_d2iz", 11, 1},   {"__aeabi_d2uiz", 12, 1},
      {"__aeabi_i2d", 13, 1},    {"__aeabi_ui2d", 14, 1},   {"copysignf", 15, 2},      {"__copysignf", 15, 2},
      {"copysign", 16, 2},       {"__copysign", 16, 2},
  };

  for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
  {
    if (strcmp(name, ops[i].name) != 0)
      continue;
    if ((ops[i].kind == 9 || ops[i].kind == 10) && !tcc_state->ir_post_float_narrow)
      return 0;
    *nargs = ops[i].nargs;
    return ops[i].kind;
  }
  return 0;
}

static int vt_call_arg_const(TCCIRState *ir, int call_i, int arg_i, VRegConstState *state, int max_vreg, int vt_gen,
                             int64_t *value)
{
  IROperand operand;
  if (!ir_opt_get_call_param_operand(ir, call_i, arg_i, &operand))
    return 0;
  return vt_operand_const(ir, operand, call_i, state, max_vreg, vt_gen, 0, value);
}

#define VT_BINOP(op, a, b, r)                                                                                          \
  do                                                                                                                   \
  {                                                                                                                    \
    switch (op)                                                                                                        \
    {                                                                                                                  \
    case 1: (r) = (a) + (b); break;                                                                                    \
    case 2: (r) = (a) - (b); break;                                                                                    \
    case 3: (r) = (a) * (b); break;                                                                                    \
    default: (r) = (a) / (b); break;                                                                                   \
    }                                                                                                                  \
  } while (0)

static int vt_eval_soft_float(int kind, int64_t a0, int64_t a1, int64_t *value)
{
  int is_dbl = (kind & 0x80) != 0;
  int op = kind & 0x7F;

  if (op >= 1 && op <= 4)
  {
    if (is_dbl)
    {
      double a = ir_bits_to_d(a0), b = ir_bits_to_d(a1), r;
      if (op == 4 && (uint64_t)a1 == 0)
        return 0;
      VT_BINOP(op, a, b, r);
      *value = ir_d_to_bits(r);
    }
    else
    {
      float a = ir_bits_to_f(a0), b = ir_bits_to_f(a1), r;
      if (op == 4 && (uint32_t)a1 == 0)
        return 0;
      VT_BINOP(op, a, b, r);
      *value = ir_f_to_bits(r);
    }
    return 1;
  }

  switch (kind)
  {
  case 5: *value = (int32_t)ir_bits_to_f(a0); break;
  case 6: *value = (int64_t)(uint32_t)ir_bits_to_f(a0); break;
  case 7: *value = ir_f_to_bits((float)(int32_t)a0); break;
  case 8: *value = ir_f_to_bits((float)(uint32_t)a0); break;
  case 9: *value = ir_d_to_bits((double)ir_bits_to_f(a0)); break;
  case 10: *value = ir_f_to_bits((float)ir_bits_to_d(a0)); break;
  case 11: *value = (int32_t)ir_bits_to_d(a0); break;
  case 12: *value = (int64_t)(uint32_t)ir_bits_to_d(a0); break;
  case 13: *value = ir_d_to_bits((double)(int32_t)a0); break;
  case 14: *value = ir_d_to_bits((double)(uint32_t)a0); break;
  case 15: *value = ir_f_to_bits(copysignf(ir_bits_to_f(a0), ir_bits_to_f(a1))); break;
  case 16: *value = ir_d_to_bits(copysign(ir_bits_to_d(a0), ir_bits_to_d(a1))); break;
  default: return 0;
  }
  return 1;
}

#undef VT_BINOP

static int vt_try_fold_soft_float(TCCIRState *ir, IRQuadCompact *q, int i, VRegConstState *state, int max_vreg,
                                  int vt_gen, int vt_def_gen)
{
  if (q->op != TCCIR_OP_FUNCCALLVAL)
    return 0;
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
  if (!name)
    return 0;

  int nargs;
  int kind = vt_soft_float_kind(name, &nargs);
  int64_t arg0, arg1 = 0, result;
  if (!kind || !vt_call_arg_const(ir, i, 0, state, max_vreg, vt_gen, &arg0) ||
      (nargs == 2 && !vt_call_arg_const(ir, i, 1, state, max_vreg, vt_gen, &arg1)) ||
      !vt_eval_soft_float(kind, arg0, arg1, &result))
    return 0;

  IROperand call_dest = tcc_ir_op_get_dest(ir, q);
  int dest_is_64 = irop_is_64bit(call_dest);
  IROperand imm_src;
  if (dest_is_64)
  {
    uint32_t pool_idx = tcc_ir_pool_add_f64(ir, (uint64_t)result);
    imm_src = irop_make_f64(-1, pool_idx);
  }
  else
    imm_src = irop_make_imm32(-1, (int32_t)result, IROP_BTYPE_INT32);

  vt_rewrite_call_as_assign(ir, q, i, imm_src);
  vt_mark_dest_const(state, max_vreg, vt_gen, vt_def_gen, call_dest, dest_is_64 ? result : (int64_t)(int32_t)result);
  LOG_IR_GEN("VALUE_TRACK: %s -> %lld at i=%d (soft-float fold)", name, (long long)result, i);
  return 1;
}

static void vt_clear_def_if_var(VRegConstState *state, int32_t vr, int max_vreg)
{
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos >= 0 && pos <= max_vreg)
      VT_CLEAR_DEF(state, pos);
  }
}

static int vt_try_fold_void_float_compare(TCCIRState *ir, IRQuadCompact *q, int i, int n, VRegConstState *state,
                                              int max_vreg, int vt_gen, int vt_def_gen)
{
  if (q->op == TCCIR_OP_FUNCCALLVOID && i + 1 < n)
  {
    IRQuadCompact *next_q = &ir->compact_instructions[i + 1];
    if (next_q->op == TCCIR_OP_JUMPIF || next_q->op == TCCIR_OP_SETIF)
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (callee)
      {
        const char *fname = get_tok_str(callee->v, NULL);
        int is_fcmp = vt_fname_match(fname, "__aeabi_cfcmple") || vt_fname_match(fname, "__aeabi_cfcmpeq");
        int is_dcmp = vt_fname_match(fname, "__aeabi_cdcmple") || vt_fname_match(fname, "__aeabi_cdcmpeq");
        if (is_fcmp || is_dcmp)
        {
          IROperand arg0, arg1;
          if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
          {
            int64_t a0 = 0, a1 = 0;
            int a0_ok = vt_operand_const(ir, arg0, i, state, max_vreg, vt_gen, 0, &a0);
            int a1_ok = vt_operand_const(ir, arg1, i, state, max_vreg, vt_gen, 0, &a1);
            if (a0_ok && a1_ok)
            {
              int cmp_result, is_nan;
              if (is_fcmp)
              {
                union
                {
                  float f;
                  uint32_t u;
                } fa, fb;
                fa.u = (uint32_t)a0;
                fb.u = (uint32_t)a1;
                is_nan = (fa.f != fa.f) || (fb.f != fb.f);
                cmp_result = (fa.f > fb.f) - (fa.f < fb.f);
              }
              else
              {
                union
                {
                  double d;
                  uint64_t u;
                } da, db;
                da.u = (uint64_t)a0;
                db.u = (uint64_t)a1;
                is_nan = (da.d != da.d) || (db.d != db.d);
                cmp_result = (da.d > db.d) - (da.d < db.d);
              }
              IROperand cond = tcc_ir_op_get_src1(ir, next_q);
              int tok = (int)irop_get_imm64_ex(ir, cond);
              int result;
              if (is_nan)
              {
                /* ordered predicates are FALSE for NaN, NE TRUE; helper returns -1 where soft-FP disagrees with IEEE
                 * (GT/GE) so leave those runtime */
                result = nan_compare_branch_result(tok);
                if (result < 0)
                  goto cdcmple_void_fold_skip;
              }
              else
              {
                result = evaluate_compare_condition(cmp_result, 0, tok);
                if (result < 0)
                  goto cdcmple_void_fold_skip;
              }

              ir_opt_nop_call_params(ir, i);
              q->op = TCCIR_OP_NOP;
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
                LOG_IR_GEN("VALUE_TRACK: %s+JUMPIF fold -> cmp=%d taken=%d at i=%d", fname, cmp_result, result, i);
              }
              else /* SETIF */
              {
                int btype = irop_get_btype(cond);
                next_q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, btype));
                tcc_ir_set_src2(ir, i + 1, IROP_NONE);
                IROperand setif_dest = tcc_ir_op_get_dest(ir, next_q);
                vt_mark_dest_const(state, max_vreg, vt_gen, vt_def_gen, setif_dest, result);
                LOG_IR_GEN("VALUE_TRACK: %s+SETIF fold -> cmp=%d result=%d at i=%d", fname, cmp_result, result, i);
              }
              return 1;
            }
          }
        }
      }
    cdcmple_void_fold_skip:;
    }
  }
  return 0;
}

static void vt_eval_int_binop(TccIrOp op, int64_t val1, int64_t val2, int is_64, int64_t *out)
{
  int64_t result;
  int shift_mask = is_64 ? 63 : 31;
  switch (op)
  {
  case TCCIR_OP_ADD:
    result = val1 + val2;
    break;
  case TCCIR_OP_SUB:
    result = val1 - val2;
    break;
  case TCCIR_OP_XOR:
    result = val1 ^ val2;
    break;
  case TCCIR_OP_AND:
    result = val1 & val2;
    break;
  case TCCIR_OP_OR:
    result = val1 | val2;
    break;
  case TCCIR_OP_MUL:
    result = val1 * val2;
    break;
  case TCCIR_OP_SHL:
    result = (int64_t)((uint64_t)val1 << (val2 & shift_mask));
    break;
  case TCCIR_OP_SHR:
    if (is_64)
      result = (int64_t)((uint64_t)val1 >> (val2 & 63));
    else
      result = (int64_t)((uint32_t)val1 >> (val2 & 31));
    break;
  case TCCIR_OP_SAR:
    if (is_64)
      result = val1 >> (val2 & 63);
    else
      result = (int64_t)((int32_t)val1 >> (val2 & 31));
    break;
  case TCCIR_OP_ROR:
  {
    uint32_t v = (uint32_t)val1;
    uint32_t n = (uint32_t)val2 & 31;
    result = (int64_t)(int32_t)((v >> n) | (v << (32 - n)));
    break;
  }
  default:
    result = 0;
    break;
  }
  *out = result;
}

static void vt_commit_binop_fold(TCCIRState *ir, IRQuadCompact *q, int i, VRegConstState *state, int max_vreg,
                                 int vt_gen, int vt_def_gen, const uint8_t *is_addrtaken, int dest_pos, int64_t result,
                                 int btype, int *changes)
{
  q->op = TCCIR_OP_ASSIGN;
  if (result == (int32_t)result)
    tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, btype));
  else
  {
    uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
    tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
  }
  tcc_ir_set_src2(ir, i, IROP_NONE);
  (*changes)++;

  if (dest_pos >= 0 && dest_pos <= max_vreg)
  {
    if (is_addrtaken[dest_pos / 8] & (1 << (dest_pos % 8)))
      VT_INVALIDATE(state, dest_pos);
    else
    {
      if (VT_IS_CONST(state, dest_pos) && VT_HAS_DEF(state, dest_pos))
      {
        ir->compact_instructions[state[dest_pos].def_idx].op = TCCIR_OP_NOP;
        (*changes)++;
      }
      VT_SET_CONST_DEF(state, dest_pos, result, i);
    }
  }
}

static void vt_clear_binop_operands(VRegConstState *state, int max_vreg, int src1_pos, IROperand accum, int dest_pos)
{
  if (src1_pos >= 0 && src1_pos <= max_vreg)
    VT_CLEAR_DEF(state, src1_pos);
  /* surviving MLA still reads its accumulator: mark that def live or a later VAR redef NOPs it */
  int live_acc_pos = vt_var_pos(accum);
  if (live_acc_pos >= 0 && live_acc_pos <= max_vreg)
    VT_CLEAR_DEF(state, live_acc_pos);
  if (dest_pos >= 0 && dest_pos <= max_vreg)
    VT_INVALIDATE(state, dest_pos);
}

static int vt_handle_arithmetic(TCCIRState *ir, IRQuadCompact *q, int i, VRegConstState *state, int max_vreg,
                                int vt_gen, int vt_def_gen, const uint8_t *is_addrtaken, int dest_pos, int has_vla,
                                int *changes)
{
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  /* merge-point invalidation at loop headers stops live-IV folding, so straight-line SHL/SHR/SAR/MUL folds are safe */
  if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_AND ||
       q->op == TCCIR_OP_OR || (!has_vla && q->op == TCCIR_OP_SHL) || q->op == TCCIR_OP_SHR || q->op == TCCIR_OP_SAR ||
       q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_MLA) &&
      irop_is_immediate(src2))
  {
    int src1_pos = vt_var_pos(src1);
    IROperand accum = (q->op == TCCIR_OP_MLA) ? tcc_ir_op_get_accum(ir, q) : IROP_NONE;

    if (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos))
    {
      int64_t val1 = state[src1_pos].value;
      int64_t val2 = irop_get_imm64_ex(ir, src2);
      int btype = (q->op == TCCIR_OP_MLA) ? irop_get_btype(dest) : irop_get_btype(src1);
      int is_64 = (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64);
      int64_t result;
      int fold_ok = 1;
      if (q->op == TCCIR_OP_MLA)
      {
        int64_t acc_val = 0;
        int acc_ok = 0;
        int acc_pos = vt_var_pos(accum);
        if (irop_is_immediate(accum))
        {
          acc_val = irop_get_imm64_ex(ir, accum);
          acc_ok = 1;
        }
        else if (acc_pos >= 0 && acc_pos <= max_vreg && VT_IS_CONST(state, acc_pos))
        {
          acc_val = state[acc_pos].value;
          acc_ok = 1;
        }
        if (!acc_ok)
          fold_ok = 0;
        else
        {
          if (dest.is_unsigned)
            result = (int64_t)((uint64_t)(uint32_t)val1 * (uint64_t)(uint32_t)val2 + (uint64_t)acc_val);
          else
            result = (int64_t)((int64_t)(int32_t)val1 * (int64_t)(int32_t)val2 + acc_val);
          is_64 = 1;
          btype = IROP_BTYPE_INT64;
        }
      }
      else
        vt_eval_int_binop(q->op, val1, val2, is_64, &result);
      if (!fold_ok)
      {
        vt_clear_binop_operands(state, max_vreg, src1_pos, accum, dest_pos);
        return 1;
      }
      if (!is_64 && q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_SAR)
        result = (int64_t)(int32_t)(uint32_t)result;

      LOG_IR_GEN("OPTIMIZE: Constant fold %s(%lld, %lld) = %lld at i=%d", tcc_ir_get_op_name(q->op), (long long)val1,
                 (long long)val2, (long long)result, i);

      vt_commit_binop_fold(ir, q, i, state, max_vreg, vt_gen, vt_def_gen, is_addrtaken, dest_pos, result, btype,
                           changes);
    }
    else
      vt_clear_binop_operands(state, max_vreg, src1_pos, accum, dest_pos);
    return 1;
  }

  if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_AND ||
       q->op == TCCIR_OP_OR || (!has_vla && q->op == TCCIR_OP_SHL) || q->op == TCCIR_OP_SHR || q->op == TCCIR_OP_SAR ||
       q->op == TCCIR_OP_MUL) &&
      !irop_is_immediate(src2))
  {
    int src2_pos = vt_var_pos(src2);

    if (src2_pos >= 0 && src2_pos <= max_vreg && VT_IS_CONST(state, src2_pos))
    {
      int64_t val2 = state[src2_pos].value;
      int btype = irop_get_btype(src2);
      int is_64 = (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64);

      int src1_const = 0;
      int64_t val1 = 0;
      if (irop_is_immediate(src1))
      {
        src1_const = 1;
        val1 = irop_get_imm64_ex(ir, src1);
      }
      else
      {
        int s1_pos = vt_var_pos(src1);
        if (s1_pos >= 0 && s1_pos <= max_vreg && VT_IS_CONST(state, s1_pos))
        {
          src1_const = 1;
          val1 = state[s1_pos].value;
        }
      }

      if (src1_const)
      {
        int64_t result;
        vt_eval_int_binop(q->op, val1, val2, is_64, &result);
        if (!is_64 && q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_SAR)
          result = (int64_t)(int32_t)(uint32_t)result;

        LOG_IR_GEN("VALUE_TRACK 2a FOLD: i=%d %s(%lld, %lld) = %lld", i, tcc_ir_get_op_name(q->op), (long long)val1,
                   (long long)val2, (long long)result);

        vt_commit_binop_fold(ir, q, i, state, max_vreg, vt_gen, vt_def_gen, is_addrtaken, dest_pos, result, btype,
                             changes);
      }
      else
      {
        LOG_IR_GEN("VALUE_TRACK 2a SUBST: i=%d src2 V%d -> #%lld", i, src2_pos, (long long)val2);
        if (val2 == (int32_t)val2)
          tcc_ir_set_src2(ir, i, irop_make_imm32(-1, (int32_t)val2, btype));
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val2);
          tcc_ir_set_src2(ir, i, irop_make_i64(-1, pool_idx, btype));
        }
        (*changes)++;

        if (dest_pos >= 0 && dest_pos <= max_vreg)
          VT_INVALIDATE(state, dest_pos);
      }
      VT_CLEAR_DEF(state, src2_pos);
      return 1;
    }
  }
  return 0;
}

static int vt_handle_compare(TCCIRState *ir, IRQuadCompact *q, int i, int n, VRegConstState *state, int max_vreg,
                             int vt_gen, int *changes)
{
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  if (q->op == TCCIR_OP_CMP && i + 1 < n)
  {
    IRQuadCompact *jump_q = &ir->compact_instructions[i + 1];
    if (jump_q->op == TCCIR_OP_JUMPIF)
    {
      int src1_pos = src1.is_lval ? -1 : vt_var_pos(src1);

      int src1_const = (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos));
      int src2_const = irop_is_immediate(src2);

      if (src1_const && src2_const)
      {
        int64_t val1 = state[src1_pos].value;
        int64_t val2 = irop_get_imm64_ex(ir, src2);

        IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
        int tok = (int)irop_get_imm64_ex(ir, cond);

        int result = evaluate_compare_condition_cmp_annotated(ir, q, val1, val2, tok, src1, src2);

        if (result >= 0)
        {
          IROperand jmp_dest = tcc_ir_op_get_dest(ir, jump_q);

          if (result)
          {
            q->op = TCCIR_OP_NOP;
            jump_q->op = TCCIR_OP_JUMP;
            tcc_ir_set_dest(ir, i + 1, jmp_dest);
            LOG_IR_GEN("VALUE_TRACK: CMP vreg=%lld,#%lld -> always taken, JUMP to %d", (long long)val1, (long long)val2,
                       (int)jmp_dest.u.imm32);
          }
          else
          {
            q->op = TCCIR_OP_NOP;
            jump_q->op = TCCIR_OP_NOP;
            LOG_IR_GEN("VALUE_TRACK: CMP vreg=%lld,#%lld -> never taken, eliminated", (long long)val1, (long long)val2);
          }
          (*changes)++;
        }
      }
    }
    else if (jump_q->op == TCCIR_OP_SETIF)
    {
      int src1_pos = src1.is_lval ? -1 : vt_var_pos(src1);

      int src1_const = (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos));
      int src2_const = irop_is_immediate(src2);

      if (src1_const && src2_const)
      {
        int64_t val1 = state[src1_pos].value;
        int64_t val2 = irop_get_imm64_ex(ir, src2);

        IROperand setif_src1 = tcc_ir_op_get_src1(ir, jump_q);
        int cond = (int)irop_get_imm64_ex(ir, setif_src1);
        int result = evaluate_compare_condition_cmp_annotated(ir, q, val1, val2, cond, src1, src2);

        if (result >= 0)
        {
          int btype = irop_get_btype(setif_src1);
          q->op = TCCIR_OP_NOP;
          jump_q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, btype));
          tcc_ir_set_src2(ir, i + 1, IROP_NONE);
          LOG_IR_GEN("VALUE_TRACK: CMP+SETIF vreg=%lld,#%lld cond=0x%x -> %d at i=%d", (long long)val1, (long long)val2,
                     cond, result, i);
          (*changes)++;
        }
      }
    }
    {
      int s1_pos = vt_var_pos(src1);
      if (s1_pos >= 0 && s1_pos <= max_vreg)
        VT_CLEAR_DEF(state, s1_pos);
    }
    return 1;
  }
  return 0;
}

static void vt_set_src1_const(TCCIRState *ir, int i, int64_t val, int btype)
{
  if (val == (int32_t)val)
    tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)val, btype));
  else
  {
    uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
    tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
  }
}

static void vt_invalidate_addrtaken_consts(VTCtx *c)
{
  int vt_gen = c->vt_gen;
  if (!c->addrtaken_overflow)
  {
    for (int a = 0; a < c->num_addrtaken; a++)
    {
      int v = c->addrtaken_list[a];
      if (VT_IS_CONST(c->state, v))
        VT_INVALIDATE(c->state, v);
    }
  }
  else
  {
    for (int v = 0; v <= c->max_vreg; v++)
    {
      if (VT_IS_CONST(c->state, v) && (c->is_addrtaken[v / 8] & (1 << (v % 8))))
        VT_INVALIDATE(c->state, v);
    }
  }
}

static void vt_set_const_def_killing_prev(VTCtx *c, int dest_pos, int64_t val, int i)
{
  int vt_gen = c->vt_gen, vt_def_gen = c->vt_def_gen;
  if (VT_IS_CONST(c->state, dest_pos) && VT_HAS_DEF(c->state, dest_pos))
  {
    c->ir->compact_instructions[c->state[dest_pos].def_idx].op = TCCIR_OP_NOP;
    c->changes++;
  }
  VT_SET_CONST_DEF(c->state, dest_pos, val, i);
}

static int vt_fold_const_var_src1(VTCtx *c, int i, IROperand src1, int to_assign, const char *tag)
{
  int vt_gen = c->vt_gen;
  int32_t src1_vr = irop_get_vreg(src1);
  if (src1_vr < 0 || TCCIR_DECODE_VREG_TYPE(src1_vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  int src1_pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
  if (src1_pos < 0 || src1_pos > c->max_vreg || !VT_IS_CONST(c->state, src1_pos))
    return 0;
  int64_t val = c->state[src1_pos].value;
  int btype = irop_get_btype(src1);
  if (to_assign)
    c->ir->compact_instructions[i].op = TCCIR_OP_ASSIGN;
  vt_set_src1_const(c->ir, i, val, btype);
  if (to_assign)
    tcc_ir_set_src2(c->ir, i, IROP_NONE);
  LOG_IR_GEN("VALUE_TRACK %s: i=%d V%d -> #%lld", tag, i, src1_pos, (long long)val);
  c->changes++;
  return 1;
}

static void vt_step_gen_boundary(VTCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  if (c->is_merge[i / 8] & (1 << (i % 8)))
  {
    c->vt_gen++;
    c->vt_def_gen++;
    c->vt_lea_gen++;
    c->num_addrtaken = 0;
    c->addrtaken_overflow = 0;
  }

  if (i > 0)
  {
    IRQuadCompact *prev = &ir->compact_instructions[i - 1];
    if (prev->op == TCCIR_OP_JUMP || prev->op == TCCIR_OP_RETURNVALUE || prev->op == TCCIR_OP_RETURNVOID ||
        prev->op == TCCIR_OP_SWITCH_TABLE)
    {
      int skip_clear = 0;
      if (prev->op == TCCIR_OP_RETURNVALUE || prev->op == TCCIR_OP_RETURNVOID)
        c->vt_in_dead_zone = 1;
      if (c->vt_in_dead_zone && !(c->is_merge[i / 8] & (1 << (i % 8))))
        skip_clear = 1;
      else
        c->vt_in_dead_zone = 0;
      /* preserve const-state across a JMP skipping only DCE NOPs to the next real instr; safe only when i is not merge */
      if (prev->op == TCCIR_OP_JUMP && !skip_clear && !(c->is_merge[i / 8] & (1 << (i % 8))))
      {
        IROperand jdest = tcc_ir_op_get_dest(ir, prev);
        int jtarget = (int)irop_get_imm64_ex(ir, jdest);
        if (jtarget >= i && jtarget < c->n)
        {
          int all_nops = 1;
          for (int k = i; k < jtarget; k++)
          {
            if (ir->compact_instructions[k].op != TCCIR_OP_NOP)
            {
              all_nops = 0;
              break;
            }
          }
          if (all_nops)
            skip_clear = 1;
        }
      }
      if (!skip_clear)
      {
        c->vt_gen++;
        c->vt_def_gen++;
        c->vt_lea_gen++;
        c->num_addrtaken = 0;
        c->addrtaken_overflow = 0;
      }
    }
    else
      c->vt_in_dead_zone = 0;
  }
}

static void vt_step_lea(VTCtx *c, int i, IROperand src1, int32_t dest_vr)
{
  int32_t src1_vr = irop_get_vreg(src1);
  if (dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP && src1_vr >= 0 &&
      TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
  {
    int tmp_pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
    int var_pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
    if (tmp_pos <= c->max_tmp && var_pos <= c->max_vreg)
    {
      c->lea_map[tmp_pos].gen = c->vt_lea_gen;
      c->lea_map[tmp_pos].var_pos = var_pos;
      LOG_IR_GEN("VALUE_TRACK LEA: i=%d T%d -> V%d", i, tmp_pos, var_pos);
    }
  }
  LOG_IR_GEN("VALUE_TRACK LEA SKIP: i=%d dest_vr=0x%x dest_type=%d src1_vr=0x%x src1_type=%d", i, dest_vr,
             dest_vr >= 0 ? TCCIR_DECODE_VREG_TYPE(dest_vr) : -1, irop_get_vreg(src1),
             irop_get_vreg(src1) >= 0 ? TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src1)) : -1);
}

static void vt_step_store(VTCtx *c, int i, IROperand dest, IROperand src1, int dest_pos)
{
  TCCIRState *ir = c->ir;
  int vt_gen = c->vt_gen, vt_def_gen = c->vt_def_gen;
  int32_t addr_vr = irop_get_vreg(dest);
  if (addr_vr >= 0 && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_TEMP)
  {
    int tmp_pos = TCCIR_DECODE_VREG_POSITION(addr_vr);
    if (tmp_pos <= c->max_tmp && c->lea_map[tmp_pos].gen == c->vt_lea_gen)
    {
      int var_pos = c->lea_map[tmp_pos].var_pos;
      if (var_pos <= c->max_vreg)
      {
        if (irop_is_immediate(src1))
        {
          VT_SET_CONST(c->state, var_pos, irop_get_imm64_ex(ir, src1));
          if (c->is_addrtaken[var_pos / 8] & (1 << (var_pos % 8)))
          {
            if (!c->addrtaken_overflow && c->num_addrtaken < VT_MAX_ADDRTAKEN)
              c->addrtaken_list[c->num_addrtaken++] = var_pos;
            else
              c->addrtaken_overflow = 1;
          }
          LOG_IR_GEN("VALUE_TRACK STORE: i=%d V%d = %lld (via T%d)", i, var_pos, (long long)c->state[var_pos].value,
                     tmp_pos);
        }
        else
        {
          VT_INVALIDATE(c->state, var_pos);
        }
      }
    }
  }
  /* propagate LEA only for the var's own storage, not a *Vptr deref store */
  else if (dest_pos >= 0 && !(dest.is_lval && !dest.is_local && !dest.is_llocal))
  {
    int lea_propagated = 0;
    int32_t src_vr = irop_get_vreg(src1);
    if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int src_tmp = TCCIR_DECODE_VREG_POSITION(src_vr);
      if (src_tmp <= c->max_tmp && c->lea_map[src_tmp].gen == c->vt_lea_gen)
      {
        c->lea_var_map[dest_pos].gen = c->vt_lea_gen;
        c->lea_var_map[dest_pos].var_pos = c->lea_map[src_tmp].var_pos;
        lea_propagated = 1;
        LOG_IR_GEN("VALUE_TRACK LEA-VAR: i=%d V%d -> V%d (via T%d)", i, dest_pos, c->lea_map[src_tmp].var_pos, src_tmp);
      }
    }
    if (!lea_propagated && dest_pos <= c->max_vreg)
    {
      c->lea_var_map[dest_pos].gen = 0;
      if (c->has_prefetch)
      {
        VT_INVALIDATE(c->state, dest_pos);
      }
      else if (irop_is_immediate(src1))
      {
        if (c->is_addrtaken[dest_pos / 8] & (1 << (dest_pos % 8)))
        {
          VT_INVALIDATE(c->state, dest_pos);
        }
        else
        {
          vt_set_const_def_killing_prev(c, dest_pos, irop_get_imm64_ex(ir, src1), i);
          LOG_IR_GEN("VALUE_TRACK DIRECT STORE: i=%d V%d = %lld", i, dest_pos, (long long)c->state[dest_pos].value);
        }
      }
      else
      {
        VT_INVALIDATE(c->state, dest_pos);
      }
    }
    if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_VAR)
    {
      int src_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
      if (src_pos >= 0 && src_pos <= c->max_vreg)
        VT_CLEAR_DEF(c->state, src_pos);
    }
  }
  else
  {
    vt_invalidate_addrtaken_consts(c);
  }
}

static int tcc_ir_opt_value_tracking__timed(TCCIRState *ir);

int tcc_ir_opt_value_tracking(TCCIRState *ir)
{
  if (tcc_ir_opt_pass_disabled("value_tracking"))
    return 0;
  int r;
  TCC_PASS_TIMED(r, "value_tracking", tcc_ir_opt_value_tracking__timed(ir));
  return r;
}

static int tcc_ir_opt_value_tracking__timed(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  ValueTrackingAnalysis analysis = {0};
  vt_analyze(ir, n, &analysis);

  VTCtx ctx = {0};
  VTCtx *c = &ctx;
  c->ir = ir;
  c->n = n;
  c->max_vreg = analysis.max_vreg;
  c->max_tmp = analysis.max_tmp;
  c->has_control_flow = analysis.has_control_flow;
  c->has_vla = analysis.has_vla;
  c->has_prefetch = analysis.has_prefetch;
  c->has_ijump = analysis.has_ijump;
  c->is_merge = analysis.is_merge;
  c->var_def_count = analysis.var_def_count;
  c->is_addrtaken = analysis.is_addrtaken;
  c->state = tcc_mallocz(sizeof(VRegConstState) * (c->max_vreg + 1));
  c->lea_map = tcc_mallocz(sizeof(LeaMapGenEntry) * (c->max_tmp + 1));
  c->lea_var_map = tcc_mallocz(sizeof(LeaMapGenEntry) * (c->max_vreg + 1));
  c->vt_gen = 1;
  c->vt_def_gen = 1;
  c->vt_lea_gen = 1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    vt_step_gen_boundary(c, i);

    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_JUMPIF)
    {
      c->vt_def_gen++;
      continue;
    }

    int vt_gen = c->vt_gen, vt_def_gen = c->vt_def_gen;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int32_t dest_vr = irop_get_vreg(dest);
    int dest_pos = vt_var_pos(dest);

    if (q->op == TCCIR_OP_LEA)
    {
      vt_step_lea(c, i, src1, dest_vr);
      continue;
    }

    if (q->op == TCCIR_OP_STORE)
    {
      vt_step_store(c, i, dest, src1, dest_pos);
      continue;
    }

    if (q->op == TCCIR_OP_ASSIGN && dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int32_t src_vr = irop_get_vreg(src1);
      if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int src_var = TCCIR_DECODE_VREG_POSITION(src_vr);
        if (src_var <= c->max_vreg && c->lea_var_map[src_var].gen == c->vt_lea_gen)
        {
          int dest_tmp = TCCIR_DECODE_VREG_POSITION(dest_vr);
          if (dest_tmp <= c->max_tmp)
          {
            c->lea_map[dest_tmp].gen = c->vt_lea_gen;
            c->lea_map[dest_tmp].var_pos = c->lea_var_map[src_var].var_pos;
            LOG_IR_GEN("VALUE_TRACK LEA-TMP: i=%d T%d -> V%d (via V%d)", i, dest_tmp, c->lea_var_map[src_var].var_pos,
                       src_var);
          }
        }
      }
    }

    if (q->op == TCCIR_OP_ASSIGN && irop_is_immediate(src1))
    {
      if (dest_pos >= 0 && dest_pos <= c->max_vreg)
      {
        /* address-taken var can be mutated via an alias: do not track as constant */
        if (c->is_addrtaken[dest_pos / 8] & (1 << (dest_pos % 8)))
        {
          VT_INVALIDATE(c->state, dest_pos);
        }
        else if (c->has_control_flow && c->var_def_count[dest_pos] > 1)
        {
          VT_INVALIDATE(c->state, dest_pos);
        }
        else
          vt_set_const_def_killing_prev(c, dest_pos, irop_get_imm64_ex(ir, src1), i);
      }
      continue;
    }

    if (vt_handle_arithmetic(ir, q, i, c->state, c->max_vreg, vt_gen, vt_def_gen, c->is_addrtaken, dest_pos, c->has_vla,
                             &c->changes))
      continue;

    if (q->op == TCCIR_OP_LOAD && !dest.is_lval)
      vt_fold_const_var_src1(c, i, src1, 1, "LOAD-FOLD");

    if (!c->has_ijump &&
        (q->op == TCCIR_OP_ASSIGN || (q->op == TCCIR_OP_CVT_FTOF && irop_get_btype(src1) == irop_get_btype(dest))) &&
        dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP && !dest.is_lval)
      vt_fold_const_var_src1(c, i, src1, 1, "ASSIGN-FOLD");

    if (q->op == TCCIR_OP_FUNCPARAMVAL)
      vt_fold_const_var_src1(c, i, src1, 0, "PARAM-FOLD");

    if (vt_handle_compare(ir, q, i, n, c->state, c->max_vreg, vt_gen, &c->changes))
      continue;

    vt_clear_def_if_var(c->state, irop_get_vreg(src1), c->max_vreg);
    vt_clear_def_if_var(c->state, irop_get_vreg(src2), c->max_vreg);
    vt_clear_def_if_var(c->state, ir_opt_mla_accum_vreg(ir, q), c->max_vreg);

    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (callee)
      {
        const char *fname = get_tok_str(callee->v, NULL);
        LOG_IR_GEN("VALUE_TRACK CALL: i=%d fname=%s", i, fname ? fname : "(null)");
        if (vt_try_fold_long_compare(ir, q, i, fname, c->state, c->max_vreg, vt_gen, vt_def_gen, dest_pos) ||
            vt_try_fold_long_divmod(ir, q, i, fname, c->state, c->max_vreg, vt_gen, vt_def_gen, dest_pos) ||
            vt_try_fold_float_compare(ir, q, i, fname, c->state, c->max_vreg, vt_gen, vt_def_gen, dest_pos) ||
            vt_try_fold_bswap(ir, q, i, fname, c->state, c->max_vreg, vt_gen, vt_def_gen, dest_pos) ||
            vt_try_fold_float_classify(ir, q, i, fname, c->state, c->max_vreg, vt_gen, vt_def_gen, dest_pos) ||
            vt_try_fold_long_arithmetic(ir, q, i, fname, c->state, c->max_vreg, vt_gen, vt_def_gen, dest_pos))
        {
          c->changes++;
          continue;
        }
      }
      if (vt_try_fold_soft_float(ir, q, i, c->state, c->max_vreg, vt_gen, vt_def_gen))
      {
        c->changes++;
        continue;
      }
    }

    if (vt_try_fold_void_float_compare(ir, q, i, n, c->state, c->max_vreg, vt_gen, vt_def_gen))
    {
      c->changes++;
      continue;
    }
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
      vt_invalidate_addrtaken_consts(c);

    if (dest_pos >= 0 && dest_pos <= c->max_vreg && irop_config[q->op].has_dest)
      VT_INVALIDATE(c->state, dest_pos);
  }

  int changes = c->changes;
  tcc_free(c->is_addrtaken);
  tcc_free(c->lea_var_map);
  tcc_free(c->lea_map);
  tcc_free(c->state);
  tcc_free(c->var_def_count);
  tcc_free(c->is_merge);

  return changes;
}

#undef VT_IS_CONST
#undef VT_HAS_DEF
#undef VT_SET_CONST
#undef VT_SET_CONST_DEF
#undef VT_INVALIDATE
#undef VT_CLEAR_DEF
