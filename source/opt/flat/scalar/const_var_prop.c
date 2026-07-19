/*
 *  TCC IR - Single-def constant VAR propagation (flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Whole-function single-def VAR const/symref forwarding feeding the fresh-IR const_prop cascade;
 * Branch A (kept flat), see docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "memory/small_sequence.h"

/* Inline-first per-VAR/TEMP bitset (64 B inline -> 512 slots, heap past that). */
TCC_SMALL_SEQUENCE_DEFINE(CvpBitset, uint8_t, 64)

/* Clears interval->addrtaken on VARs whose LEAs were all DCE-ed (or are effectively dead:
 * dest never read), so const-prop can fire in the same round; returns #cleared. */
static int refresh_stale_var_addrtaken(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Static-chain plumbing lets a nested callee read captured locals without a visible LEA. */
  int max_var = -1;
  int max_tmp = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_SET_CHAIN || q->op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 3; k++)
    {
      IROperand op = (k == 0)   ? tcc_ir_op_get_dest(ir, q)
                     : (k == 1) ? tcc_ir_op_get_src1(ir, q)
                                : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr < 0) continue;
      int t = TCCIR_DECODE_VREG_TYPE(vr);
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (t == TCCIR_VREG_TYPE_VAR && p > max_var)
        max_var = p;
      else if (t == TCCIR_VREG_TYPE_TEMP && p > max_tmp)
        max_tmp = p;
    }
  }
  if (max_var < 0)
    return 0;

  /* A lval dest counts as a read of its base vreg (the pointer written through); a LEA src1 is
   * an address-take, not a value read. */
  small_sequence(CvpBitset) var_read_owner = {0}, tmp_read_owner = {0}, var_addr_taken_owner = {0};
  if (CvpBitset_init(&var_read_owner, (size_t)((max_var + 8) / 8)) != 0 ||
      CvpBitset_init(&var_addr_taken_owner, (size_t)((max_var + 8) / 8)) != 0)
    return 0;
  uint8_t *var_read = CvpBitset_data(&var_read_owner);
  uint8_t *var_addr_taken = CvpBitset_data(&var_addr_taken_owner);
  uint8_t *tmp_read = NULL;
  if (max_tmp >= 0)
  {
    CvpBitset_init(&tmp_read_owner, (size_t)((max_tmp + 8) / 8));
    tmp_read = CvpBitset_data(&tmp_read_owner);
  }
  /* One walk: value reads (var_read/tmp_read) + LEA address-takes (var_addr_taken). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int lea = (q->op == TCCIR_OP_LEA);
    if (lea)
    {
      /* `int *p = &a; foo(&p);` — &a escapes through p even if p is never read as a value. */
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t svr = irop_get_vreg(s);
      if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
      {
        int sp = TCCIR_DECODE_VREG_POSITION(svr);
        if (sp <= max_var)
          var_addr_taken[sp / 8] |= (1 << (sp % 8));
      }
    }
    for (int slot = 0; slot < 3; slot++)
    {
      IROperand op;
      if (slot == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
        if (!op.is_lval)
          continue;
      }
      else
      {
        if (lea && slot == 1)
          continue;
        int has = (slot == 1) ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2;
        if (!has)
          continue;
        op = (slot == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      }
      int32_t vr = irop_get_vreg(op);
      if (vr < 0) continue;
      int t = TCCIR_DECODE_VREG_TYPE(vr);
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (t == TCCIR_VREG_TYPE_VAR && p <= max_var)
        var_read[p / 8] |= (1 << (p % 8));
      else if (t == TCCIR_VREG_TYPE_TEMP && tmp_read && p <= max_tmp)
        tmp_read[p / 8] |= (1 << (p % 8));
    }
  }

  /* A LEA is live only if its destination is read (or itself address-taken) downstream. */
  small_sequence(CvpBitset) has_live_lea_owner = {0};
  if (CvpBitset_init(&has_live_lea_owner, (size_t)((max_var + 8) / 8)) != 0)
    return 0;
  uint8_t *has_live_lea = CvpBitset_data(&has_live_lea_owner);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA)
      continue;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    int32_t svr = irop_get_vreg(s);
    if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int sp = TCCIR_DECODE_VREG_POSITION(svr);
    if (sp > max_var)
      continue;

    int dest_read = 1;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr >= 0)
    {
      int dt = TCCIR_DECODE_VREG_TYPE(dvr);
      int dp = TCCIR_DECODE_VREG_POSITION(dvr);
      if (dt == TCCIR_VREG_TYPE_VAR)
        dest_read = (dp <= max_var)
                        ? (!!(var_read[dp / 8] & (1 << (dp % 8))) ||
                           !!(var_addr_taken[dp / 8] & (1 << (dp % 8))))
                        : 1;
      else if (dt == TCCIR_VREG_TYPE_TEMP)
        dest_read = (tmp_read && dp <= max_tmp) ? !!(tmp_read[dp / 8] & (1 << (dp % 8))) : 1;
    }
    if (dest_read)
      has_live_lea[sp / 8] |= (1 << (sp % 8));
  }
  int cleared = 0;
  small_sequence(CvpBitset) seen_owner = {0};
  if (CvpBitset_init(&seen_owner, (size_t)((max_var + 8) / 8)) != 0)
    return 0;
  uint8_t *seen = CvpBitset_data(&seen_owner);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 3; k++)
    {
      IROperand op = (k == 0)   ? tcc_ir_op_get_dest(ir, q)
                     : (k == 1) ? tcc_ir_op_get_src1(ir, q)
                                : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p > max_var)
        continue;
      if (seen[p / 8] & (1 << (p % 8)))
        continue;
      seen[p / 8] |= (1 << (p % 8));
      if (has_live_lea[p / 8] & (1 << (p % 8)))
        continue;
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
      if (interval && interval->addrtaken)
      {
        interval->addrtaken = 0;
        cleared++;
      }
    }
  }
  return cleared;
}

static int ir_has_variadic_stack_arg_call(TCCIRState *ir)
{
  int n = ir ? ir->next_instruction_index : 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee || !callee->type.ref || callee->type.ref->f.func_type != FUNC_ELLIPSIS)
      continue;

    IROperand meta = tcc_ir_op_get_src2(ir, q);
    int argc = TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, meta));
    if (argc > 4)
      return 1;
  }
  return 0;
}

static int tcc_ir_opt_const_var_prop__timed(TCCIRState *ir);
int tcc_ir_opt_const_var_prop(TCCIRState *ir)
{
  if (tcc_ir_opt_pass_disabled("const_var_prop")) return 0;
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_const_var_prop__timed(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_const_var_prop__timed(ir);
  tcc_pass_timing_add("const_var_prop", tcc_pass_clk_us() - _t);
  return _r;
}

static int tcc_ir_opt_const_var_prop__timed(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int max_var_pos = 0;
  int i;

  if (n == 0)
    return 0;

  refresh_stale_var_addrtaken(ir);

  /* Variadic calls with anonymous args beyond r0-r3 use the caller's outgoing stack area;
   * simplifying such call regions exposes a lower-level ABI miscompile (varargs seed 31282). */
  if (ir_has_variadic_stack_arg_call(ir))
    return 0;

  /* Phase 1: find VAR vregs assigned exactly once with an immediate or symref. */
  typedef struct
  {
    uint8_t is_constant : 1;
    uint8_t is_sym : 1;
    uint8_t sym_is_lval : 1;
    uint8_t sym_is_local : 1;
    uint8_t sym_is_const : 1;
    uint8_t def_count;
    uint8_t use_count;
    int64_t value; /* immediate value, or pool_idx for symrefs */
    int btype;
    int is_unsigned;
  } VarInfo;

  VarInfo *var_info = NULL;
  int var_info_cap = 0;
  int has_var = 0;

  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
    has_var = 1;
    if (pos > max_var_pos)
      max_var_pos = pos;

    if (pos >= var_info_cap)
    {
      int new_cap = var_info_cap ? var_info_cap * 2 : 16;
      while (new_cap <= pos)
        new_cap *= 2;
      var_info = tcc_realloc(var_info, sizeof(VarInfo) * new_cap);
      memset(var_info + var_info_cap, 0, sizeof(VarInfo) * (new_cap - var_info_cap));
      var_info_cap = new_cap;
    }

    /* Address-taken VARs can be modified through aliases; volatile VARs must
     * never have their loads folded to the stored value. */
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
    if (interval && (interval->addrtaken || interval->is_volatile))
    {
      var_info[pos].def_count++;
      var_info[pos].is_constant = 0;
      continue;
    }

    var_info[pos].def_count++;

    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (irop_is_immediate(src1) && !src1.is_sym && !src1.is_lval && !src1.is_local && var_info[pos].def_count == 1)
      {
        var_info[pos].is_constant = 1;
        var_info[pos].is_sym = 0;
        var_info[pos].value = irop_get_imm64_ex(ir, src1);
        var_info[pos].btype = irop_get_btype(src1);
        var_info[pos].is_unsigned = src1.is_unsigned;
      }
      /* Symref values (`int *p = &g`): ASSIGN source only — STORE-of-symref to a stack VAR
       * would need extra reasoning about the destination's storage. */
      else if (q->op == TCCIR_OP_ASSIGN && src1.is_sym && !src1.is_lval &&
               var_info[pos].def_count == 1)
      {
        var_info[pos].is_constant = 1;
        var_info[pos].is_sym = 1;
        var_info[pos].sym_is_lval = src1.is_lval;
        var_info[pos].sym_is_local = src1.is_local;
        var_info[pos].sym_is_const = src1.is_const;
        var_info[pos].value = (int64_t)src1.u.pool_idx;
        var_info[pos].btype = irop_get_btype(src1);
        var_info[pos].is_unsigned = src1.is_unsigned;
      }
    }
    else
    {
      var_info[pos].is_constant = 0;
    }
  }

  if (!has_var)
  {
    if (var_info)
      tcc_free(var_info);
    goto neg_vreg_phase;
  }

  /* Use counts throttle symref propagation: a multi-use symref VAR is cheaper held in a
   * callee-saved register than re-materialized from the literal pool at each use. */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int slot = 0; slot < 2; slot++)
    {
      int has = (slot == 0) ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2;
      if (!has)
        continue;
      IROperand op = (slot == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos > max_var_pos)
        continue;
      if (var_info[pos].use_count < 255)
        var_info[pos].use_count++;
    }
    int32_t acc_vr = ir_opt_mla_accum_vreg(ir, q);
    if (acc_vr >= 0 && TCCIR_DECODE_VREG_TYPE(acc_vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(acc_vr);
      if (pos <= max_var_pos && var_info[pos].use_count < 255)
        var_info[pos].use_count++;
    }
  }

  for (i = 0; i <= max_var_pos; i++)
  {
    if (var_info[i].def_count > 1)
      var_info[i].is_constant = 0;
  }

  /* Phase 2: replace uses.  A local without lval is an address-of (LEA), not a value load —
   * never substitute those. */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t src1_vr = irop_get_vreg(src1);
      if (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR && !(src1.is_local && !src1.is_lval))
      {
        int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        /* Symrefs fold only at a single use so the pool address isn't materialized at every
         * site (zerolen-1); immediates always fold. */
        if (pos <= max_var_pos && var_info[pos].is_constant &&
            (!var_info[pos].is_sym || var_info[pos].use_count <= 1))
        {
          IROperand new_src1;
          if (var_info[pos].is_sym)
          {
            new_src1 = irop_make_symref(-1, (uint32_t)var_info[pos].value, var_info[pos].sym_is_lval,
                                        var_info[pos].sym_is_local, var_info[pos].sym_is_const,
                                        var_info[pos].btype);
          }
          else
          {
            int64_t val = var_info[pos].value;
            if (val == (int32_t)val)
              new_src1 = irop_make_imm32(-1, (int32_t)val, var_info[pos].btype);
            else
            {
              uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
              new_src1 = irop_make_i64(-1, pool_idx, var_info[pos].btype);
            }
          }
          new_src1.is_unsigned = var_info[pos].is_unsigned;
          tcc_ir_set_src1(ir, i, new_src1);

          /* LOAD of a now-constant value becomes ASSIGN; not for symrefs, where LOAD means
           * dereferencing the symbol's storage. */
          if (q->op == TCCIR_OP_LOAD && !var_info[pos].is_sym)
            q->op = TCCIR_OP_ASSIGN;

          changes++;
        }
      }
    }

    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t src2_vr = irop_get_vreg(src2);
      if (src2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR && !(src2.is_local && !src2.is_lval))
      {
        int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
        if (pos <= max_var_pos && var_info[pos].is_constant &&
            (!var_info[pos].is_sym || var_info[pos].use_count <= 1))
        {
          IROperand new_src2;
          if (var_info[pos].is_sym)
          {
            new_src2 = irop_make_symref(-1, (uint32_t)var_info[pos].value, var_info[pos].sym_is_lval,
                                        var_info[pos].sym_is_local, var_info[pos].sym_is_const,
                                        var_info[pos].btype);
          }
          else
          {
            int64_t val = var_info[pos].value;
            if (val == (int32_t)val)
              new_src2 = irop_make_imm32(-1, (int32_t)val, var_info[pos].btype);
            else
            {
              uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
              new_src2 = irop_make_i64(-1, pool_idx, var_info[pos].btype);
            }
          }
          new_src2.is_unsigned = var_info[pos].is_unsigned;
          tcc_ir_set_src2(ir, i, new_src2);
          changes++;
        }
      }
    }
  }

  /* Phase 3: NOP defining ASSIGNs of constant VARs with no remaining uses. */
  if (changes > 0)
  {
    small_sequence(CvpBitset) has_use_owner = {0};
    CvpBitset_init(&has_use_owner, (size_t)((max_var_pos + 8) / 8));
    uint8_t *has_use = CvpBitset_data(&has_use_owner);

    for (i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var_pos)
            has_use[pos / 8] |= (1 << (pos % 8));
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand src2 = tcc_ir_op_get_src2(ir, q);
        int32_t vr = irop_get_vreg(src2);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var_pos)
            has_use[pos / 8] |= (1 << (pos % 8));
        }
      }
      int32_t acc_vr = ir_opt_mla_accum_vreg(ir, q);
      if (acc_vr >= 0 && TCCIR_DECODE_VREG_TYPE(acc_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(acc_vr);
        if (pos <= max_var_pos)
          has_use[pos / 8] |= (1 << (pos % 8));
      }
    }

    for (i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos > max_var_pos)
        continue;
      if (var_info[pos].is_constant && !(has_use[pos / 8] & (1 << (pos % 8))))
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }
  }

  tcc_free(var_info);

neg_vreg_phase:
  /* Phase 4: propagate through negative-vreg temp locals (-2..-16), the frontend's
   * intermediate slots for cast chains — single-def VARs under a different encoding. */
  {
#define NEG_VREG_MAX 16
    typedef struct
    {
      uint8_t is_constant;
      uint8_t def_count;
      int64_t value;
      int btype;
      int is_unsigned;
    } NegVregInfo;
    NegVregInfo nvi[NEG_VREG_MAX];
    memset(nvi, 0, sizeof(nvi));
    int has_neg = 0;

    for (i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      if (dest_vr >= -1 || dest_vr < -(int)NEG_VREG_MAX)
        continue;
      int idx = (int)(-dest_vr - 1);
      has_neg = 1;
      nvi[idx].def_count++;
      if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        /* LOADs may read individual 32-bit halves of a 64-bit neg vreg; only propagate when
         * the value is zero (both halves 0) or the type is 32-bit. */
        if (irop_is_immediate(src1) && !src1.is_sym && nvi[idx].def_count == 1 &&
            (irop_get_btype(src1) != IROP_BTYPE_INT64 || irop_get_imm64_ex(ir, src1) == 0))
        {
          nvi[idx].is_constant = 1;
          nvi[idx].value = irop_get_imm64_ex(ir, src1);
          nvi[idx].btype = irop_get_btype(src1);
          nvi[idx].is_unsigned = src1.is_unsigned;
        }
      }
      else
      {
        nvi[idx].is_constant = 0;
      }
    }

    for (i = 0; i < NEG_VREG_MAX; i++)
    {
      if (nvi[i].def_count > 1)
        nvi[i].is_constant = 0;
    }

    if (has_neg)
    {
      for (i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_src1)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          int32_t vr = irop_get_vreg(src1);
          if (vr < -1 && vr >= -(int)NEG_VREG_MAX)
          {
            int idx = (int)(-vr - 1);
            if (nvi[idx].is_constant)
            {
              int64_t val = nvi[idx].value;
              int btype = nvi[idx].btype;
              IROperand new_src1;
              if (val == (int32_t)val)
                new_src1 = irop_make_imm32(-1, (int32_t)val, btype);
              else
              {
                uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
                new_src1 = irop_make_i64(-1, pool_idx, btype);
              }
              new_src1.is_unsigned = nvi[idx].is_unsigned;
              tcc_ir_set_src1(ir, i, new_src1);
              if (q->op == TCCIR_OP_LOAD)
                q->op = TCCIR_OP_ASSIGN;
              changes++;
            }
          }
        }
        if (irop_config[q->op].has_src2)
        {
          IROperand src2 = tcc_ir_op_get_src2(ir, q);
          int32_t vr = irop_get_vreg(src2);
          if (vr < -1 && vr >= -(int)NEG_VREG_MAX)
          {
            int idx = (int)(-vr - 1);
            if (nvi[idx].is_constant)
            {
              int64_t val = nvi[idx].value;
              int btype = nvi[idx].btype;
              IROperand new_src2;
              if (val == (int32_t)val)
                new_src2 = irop_make_imm32(-1, (int32_t)val, btype);
              else
              {
                uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
                new_src2 = irop_make_i64(-1, pool_idx, btype);
              }
              new_src2.is_unsigned = nvi[idx].is_unsigned;
              tcc_ir_set_src2(ir, i, new_src2);
              changes++;
            }
          }
        }
      }
    }
#undef NEG_VREG_MAX
  }

  return changes;
}
