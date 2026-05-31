/*
 *  TCC IR - Constant & Value Propagation
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
#include "opt_utils.h"
#include "opt_du.h"

/* When a soft-FP __aeabi_cfcmple/cdcmple is called with at least one NaN
 * operand, the (>)-(<) integer "cmp_result" we compute from the host's
 * IEEE comparison degenerates to 0 — indistinguishable from "equal" — so
 * evaluate_compare_condition() would mis-fold ordered predicates as true.
 * This helper returns the correct IEEE boolean for the JUMPIF/SETIF token
 * directly: ordered predicates are FALSE for NaN, NE is TRUE.
 *
 * Returns -1 to mean "don't fold" for tokens where the soft-FP runtime
 * (fcmp_core returns 2 for unordered, then `cmp r0, #0` makes flags say
 * "greater") would disagree with IEEE — GT/GE/UGT/UGE.  The IR generator
 * swaps operands so these tokens don't normally appear after cfcmple, but
 * if they ever do, folding to the IEEE answer would silently diverge from
 * runtime; leave the call so the (buggy-for-NaN) runtime answer stands. */
static int nan_compare_branch_result(int cond_token)
{
  switch (cond_token)
  {
  case TOK_EQ:
  case TOK_LT:
  case TOK_LE:
  case TOK_ULT:
  case TOK_ULE:
    return 0;
  case TOK_NE:
    return 1;
  default:
    return -1;
  }
}

/* Refresh stale `interval->addrtaken` flags.  The flag is set by the
 * frontend when source code takes a variable's address, but earlier
 * optimizer passes may have eliminated the producing LEA (e.g. a dead
 * `int **p = &v` whose `p` was never read).  Also recognize "effectively
 * dead" LEAs whose own destination is never read — DCE will reap those
 * shortly, but clearing addrtaken now lets const-prop fire in the same
 * round.
 *
 * Returns the number of intervals whose flag was cleared. */
static int refresh_stale_var_addrtaken(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Bail conservatively if the function has explicit static-chain plumbing:
   * captured locals can be read by a nested callee without a visible LEA. */
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

  /* Mark every VAR/TEMP that's read anywhere as a value.
   *
   * Sources of reads:
   *   - any src1/src2 (except a LEA's src1, which is an address-take, not a
   *     value load)
   *   - the dest of STORE/STORE_INDEXED/STORE_POSTINC when is_lval=1: the
   *     dest's base vreg is the pointer being written through, so the vreg
   *     IS read (we read the pointer to know where to store).  Without
   *     this, a `STORE *T0 <-- v` doesn't count as a use of T0 — and a
   *     downstream LEA-of-VAR feeding T0 (`T0 = &V`) gets mis-classified
   *     as dead, dropping V's addrtaken even though V is reachable through
   *     the STORE's pointer write. */
  uint8_t *var_read = tcc_mallocz((max_var + 8) / 8);
  uint8_t *tmp_read = (max_tmp >= 0) ? tcc_mallocz((max_tmp + 8) / 8) : NULL;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int lea = (q->op == TCCIR_OP_LEA);
    for (int slot = 0; slot < 3; slot++)
    {
      IROperand op;
      if (slot == 0)
      {
        /* Treat a lval dest as a read of its base vreg (pointer through
         * which we write).  Non-lval dest is a plain write and not a read. */
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

  /* Mark a VAR as having a "live" LEA only if some LEA's destination is
   * actually read downstream — otherwise the LEA is effectively dead. */
  uint8_t *has_live_lea = tcc_mallocz((max_var + 8) / 8);
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
        dest_read = (dp <= max_var) ? !!(var_read[dp / 8] & (1 << (dp % 8))) : 1;
      else if (dt == TCCIR_VREG_TYPE_TEMP)
        dest_read = (tmp_read && dp <= max_tmp) ? !!(tmp_read[dp / 8] & (1 << (dp % 8))) : 1;
    }
    if (dest_read)
      has_live_lea[sp / 8] |= (1 << (sp % 8));
  }

  int cleared = 0;
  uint8_t *seen = tcc_mallocz((max_var + 8) / 8);
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
  tcc_free(seen);
  tcc_free(has_live_lea);
  if (tmp_read) tcc_free(tmp_read);
  tcc_free(var_read);
  return cleared;
}

int tcc_ir_opt_const_var_prop(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int max_var_pos = 0;
  int i;

  if (n == 0)
    return 0;

  /* Clear `addrtaken` on VARs whose LEAs have all been DCE-ed since the
   * frontend set the flag.  Unblocks const-prop on locals like
   *   int *p = &g; int **dead = &p; ... use p ...
   * where `dead` got eliminated as unread. */
  refresh_stale_var_addrtaken(ir);

  /* Phase 1: Find constant VAR vregs (assigned exactly once with immediate
   * or symref).  For symrefs we also remember is_lval/is_local/is_const so
   * the rebuilt operand at the use site preserves the original semantics. */
  typedef struct
  {
    uint8_t is_constant : 1;
    uint8_t is_sym : 1;     /* value is a symref, not an immediate */
    uint8_t sym_is_lval : 1;
    uint8_t sym_is_local : 1;
    uint8_t sym_is_const : 1;
    uint8_t def_count;
    uint8_t use_count;
    int64_t value;          /* immediate value, or pool_idx for symrefs */
    int btype;
    int is_unsigned;
  } VarInfo;

  /* Combined pass: find max_var_pos and build var_info in one O(n) scan.
   * var_info grows dynamically as new VAR positions are discovered. */
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

    /* If the variable's address is taken, it can be modified through aliases
     * (e.g. passed as an out-parameter to a function).  Not safe for
     * constant propagation. */
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
    if (interval && interval->addrtaken)
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
      /* Symref values: `int *p = &g` or similar.  Safe to propagate the
       * symref through subsequent uses of the VAR — addrtaken has already
       * been refreshed, so writes through pointers can't have aliased it.
       * Only ASSIGN (not STORE) source is accepted; STORE-of-symref to a
       * stack VAR is the rarer form and would need extra reasoning about
       * the destination's storage. */
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

  /* Count uses per VAR (any src1/src2 reference) so we can throttle
   * symref propagation that bloats codegen when materialized at every
   * use site.  A multi-use symref VAR is usually cheaper held in a
   * callee-saved register across a call than re-loaded from the literal
   * pool at each use. */
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
  }

  /* Mark multiply-defined vars as non-constant */
  for (i = 0; i <= max_var_pos; i++)
  {
    if (var_info[i].def_count > 1)
      var_info[i].is_constant = 0;
  }

  /* Phase 2: Replace uses of constant VARs with immediates */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Check src1.
     * Don't propagate if src1 is a local without lval — that's an address-of
     * (LEA), not a value load.  Replacing it with the variable's value would
     * be incorrect. */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t src1_vr = irop_get_vreg(src1);
      if (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR && !(src1.is_local && !src1.is_lval))
      {
        int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        /* Symref propagation: only fold when the VAR has a single use, so
         * we don't materialize the same constant-pool address at multiple
         * use sites (regression: zerolen-1 spawned 2 PC-relative loads
         * where the baseline kept one address in callee-saved r4 across
         * the call).  Immediates are always folded — they don't need a
         * literal pool entry. */
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

          /* LOAD with constant src means the address was a local variable that
           * is now known to be a constant value — convert to ASSIGN.  Don't
           * do this for symref values: a LOAD of a symref means dereferencing
           * the symbol's storage, not folding the address itself. */
          if (q->op == TCCIR_OP_LOAD && !var_info[pos].is_sym)
            q->op = TCCIR_OP_ASSIGN;

          changes++;
        }
      }
    }

    /* Check src2 (same LEA guard as src1) */
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

  /* Phase 3: Eliminate dead VAR ASSIGNs whose uses were all replaced.
   * Scan for remaining uses of each constant VAR; if none found, NOP
   * the defining ASSIGN. */
  if (changes > 0)
  {
    /* Reset use counts for constant VARs */
    uint8_t *has_use = tcc_mallocz((max_var_pos + 8) / 8);

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
    }

    /* NOP dead ASSIGN instructions for constant VARs with no remaining uses */
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

    tcc_free(has_use);
  }

  tcc_free(var_info);

neg_vreg_phase:
  /* Phase 4: Propagate constants through negative-vreg temp locals.
   * These are temporary stack slots (vregs -2..-16) used by the frontend for
   * intermediate values in cast chains (e.g. int → long long).  They behave
   * like single-def VARs but use a different encoding. */
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
        /* For 64-bit negative vregs, subsequent LOADs may read individual
         * 32-bit halves (lo/hi) and we cannot tell which half a given LOAD
         * extracts.  Only propagate when the value is zero (both halves are
         * 0) or the type is 32-bit. */
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

/* ---------------------------------------------------------------------------
 * Global-initializer constant propagation
 *
 * Replace `LOAD dest <-- GlobalSym(X)+addend [deref]` with either:
 *   - `ASSIGN dest <-- #imm` when the initializer byte range is plain data,
 *   - `ASSIGN dest <-- &SymY+addend` when the byte range is covered by a
 *     single R_ARM_ABS32 relocation (e.g. a const pointer initialised to
 *     the address of another global, or a function-pointer entry inside a
 *     const struct).
 *
 * The pass handles primitive scalar globals, arrays, and aggregates: the
 * read width is taken from the operand's btype, not the symbol's full size,
 * so a 1-byte byte-access into a const struct at offset N folds to that
 * specific byte.  After this pass runs, the iterative const_prop +
 * branch_folding + DCE pipeline picks up the newly-visible constants /
 * symrefs and collapses downstream comparisons and dead arms.
 *
 * Safety gates (mirror the checks already used in try_inline_const_eval):
 *   - The sym must exist and carry a known type.
 *   - possibly_written == 0 (no stores / no non-const pointer escape).
 *   - Not volatile, not VLA.
 *   - Linkage: VT_STATIC, or VT_CONSTANT (the C language forbids writing
 *     to a const object even via another TU, so an extern-visible const
 *     global cannot be mutated legally).
 *   - Weak / dllimport / undefined symbols are skipped.
 *   - The initializer range must fit inside the section's emitted data.
 *   - If a relocation overlaps the read range, only a clean R_ARM_ABS32 at
 *     exactly `off` with a 4-byte read folds (to a symref); any partial
 *     overlap rejects the fold.
 */
int tcc_ir_opt_global_init_prop(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_state)
    return 0;

  const int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Consider both src1 and src2 operands.  For LOAD, src1's deref is the
     * load location and the whole op becomes ASSIGN.  For other ops (CMP,
     * JUMPIF, ASSIGN, arithmetic), an is_sym && is_lval operand is a
     * read-side deref that can be folded in place. */
    for (int slot = 0; slot < 2; slot++)
    {
      IROperand opnd = (slot == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      if (!opnd.is_sym || !opnd.is_lval)
        continue;
      /* STORE's address is in dest, not in src1/src2 — both srcs are values.
       * For LOAD, src1 carries the address; folding it converts to ASSIGN. */

      IRPoolSymref *ref = irop_get_symref_ex(ir, opnd);
      if (!ref || !ref->sym)
        continue;
      Sym *sym = ref->sym;

      /* Linkage / attribute gates. */
      if (sym->a.weak || sym->a.dllimport)
        continue;

      const int ttype = sym->type.t;
      if (ttype & VT_VLA)
        continue;
      if (ttype & VT_VOLATILE)
        continue;

      int is_const_q = (ttype & VT_CONSTANT) != 0;
      /* For array types, const qualifies the element type, not the
       * array itself.  Check the pointed-to type for VT_CONSTANT. */
      if (!is_const_q && (ttype & VT_ARRAY) && sym->type.ref)
        is_const_q = (sym->type.ref->type.t & VT_CONSTANT) != 0;

      /* For non-const globals, possibly_written means another function
       * may have stored to this symbol.  For const-qualified globals the
       * C standard forbids modification, so ignore the flag. */
      if (!is_const_q && sym->a.possibly_written)
        continue;

      if (!(ttype & VT_STATIC) && !is_const_q)
        continue;

      /* Pointer-typed globals are foldable only when const-qualified — the
       * non-const late_reopt path can't safely emit a symref fold (re-emit
       * shrinks the function and the literal-pool placement isn't kept
       * aligned).  Skip non-const pointer globals before we'd otherwise
       * flag the function for late_reopt and trigger that re-emit. */
      if ((ttype & VT_BTYPE) == VT_PTR && !is_const_q)
        continue;

      /* TCC is single-pass: when this function is optimized, stores in
       * later-declared functions have not yet been seen, so possibly_written
       * may be 0 for a global that is in fact written elsewhere in the TU
       * (see 20001111-1.c).  Restrict the fold to const-qualified globals,
       * which the language guarantees are not modified.
       *
       * BYPASS: during the end-of-TU late_reopt phase, possibly_written
       * reflects the entire TU, so non-const statics can also be folded —
       * but only if the symbol's address was never taken (otherwise an
       * alias write could have updated it without poisoning possibly_written;
       * static initializers that capture `&sym` don't go through the regular
       * store path that sets the flag).  See pr22237.c for the alias case. */
      if (!is_const_q)
      {
        if (sym->a.addrtaken)
          continue;
        if (!tcc_state->ir_late_reopt_phase)
        {
          /* Record the function for end-of-TU re-optimization: at that
           * point possibly_written will be final TU-wide and we can fold
           * safely. */
          if (tcc_state->cur_func_sym && tcc_state->cur_func_sym->type.ref)
            tcc_state->cur_func_sym->type.ref->f.func_late_reopt = 1;
          continue;
        }
        /* else: late phase — fall through, fold this non-const static. */
      }

      ElfSym *esym = elfsym(sym);
      if (!esym)
        continue;
      if (esym->st_shndx == SHN_UNDEF || esym->st_shndx == SHN_COMMON)
        continue;
      if (esym->st_shndx >= tcc_state->nb_sections)
        continue;

      Section *sec = tcc_state->sections[esym->st_shndx];
      if (!sec)
        continue;
      /* SHT_NOBITS (.bss): no data buffer, value is implicit zero. */
      int is_bss = (sec->sh_type == SHT_NOBITS);
      if (!is_bss && !sec->data)
        continue;

      /* Result btype: for LOAD, use dest btype; for read-side deref operands
       * on non-LOAD ops, use the operand's own btype so consumers keep their
       * expected operand width. */
      int result_btype;
      int result_is_unsigned;
      if (q->op == TCCIR_OP_LOAD && slot == 0)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        result_btype = irop_get_btype(dest);
        result_is_unsigned = dest.is_unsigned;
      }
      else
      {
        result_btype = irop_get_btype(opnd);
        result_is_unsigned = opnd.is_unsigned;
      }

      /* Map result btype to read size in bytes; reject types we can't decode. */
      int read_size;
      switch (result_btype)
      {
        case IROP_BTYPE_INT8:  read_size = 1; break;
        case IROP_BTYPE_INT16: read_size = 2; break;
        case IROP_BTYPE_INT32: read_size = 4; break;
        case IROP_BTYPE_INT64: read_size = 8; break;
        default:               read_size = 0; break;
      }
      if (read_size == 0)
        continue;

      unsigned long off = (unsigned long)(esym->st_value + (unsigned long long)ref->addend);
      if (!is_bss && off + (unsigned long)read_size > sec->data_offset)
        continue;

      /* Scan section relocations for any overlap with [off, off+read_size).
       * The only foldable overlap is a single R_ARM_ABS32 at exactly `off`
       * with a 4-byte read — that becomes a symref to the target.  Any
       * other overlap (partial reloc inside the read range, smaller read
       * over a 4-byte reloc, etc.) rejects the fold. */
      int reloc_at_off = 0;
      int reloc_overlap = 0;
      Sym *reloc_target_sym = NULL;
      int64_t reloc_data_addend = 0;

      if (sec->reloc && sec->reloc->data && sec->reloc->data_offset)
      {
        ElfW_Rel *rel;
        for_each_elem(sec->reloc, 0, rel, ElfW_Rel)
        {
          uint32_t r_off = (uint32_t)rel->r_offset;
          int r_type = ELFW(R_TYPE)(rel->r_info);
          /* Be conservative: anything other than ABS32 we treat as a
           * single-byte cover so we still reject partial overlaps. */
          uint32_t r_size = (r_type == R_ARM_ABS32) ? 4 : 1;

          if (r_off + r_size <= off)
            continue;
          if (r_off >= off + (unsigned long)read_size)
            continue;

          reloc_overlap = 1;
          /* Symref fold is only safe when the *source* global is
           * const-qualified.  The non-const late_reopt path can shrink the
           * caller (LOAD+DEREF → ASSIGN-sym is one fewer Thumb-2 instruction)
           * and the re-emit's literal-pool placement isn't kept aligned
           * across that size change (see pr22237).  Restricting to const
           * sources avoids that path entirely. */
          if (r_off == off && read_size == 4 && r_type == R_ARM_ABS32 && is_const_q)
          {
            int r_sym_idx = ELFW(R_SYM)(rel->r_info);
            if (r_sym_idx > 0 && symtab_section && symtab_section->link)
            {
              ElfW(Sym) *tgt_esym = &((ElfW(Sym) *)symtab_section->data)[r_sym_idx];
              const char *tname = (const char *)symtab_section->link->data + tgt_esym->st_name;
              if (tname && *tname)
              {
                int tok = tok_alloc_const(tname);
                Sym *tsym = sym_find(tok);
                if (tsym)
                {
                  /* REL format: addend lives in the data at the reloc offset. */
                  int32_t a32 = 0;
                  if (!is_bss)
                    memcpy(&a32, sec->data + off, 4);
                  reloc_target_sym = tsym;
                  reloc_data_addend = a32;
                  reloc_at_off = 1;
                }
              }
            }
          }
          break;
        }
      }

      if (reloc_overlap && !reloc_at_off)
        continue;

      IROperand new_opnd;
      if (reloc_at_off)
      {
        /* Fold to a symref-by-value (address constant: &TargetSym + addend). */
        uint32_t pool_idx = tcc_ir_pool_add_symref(ir, reloc_target_sym, (int32_t)reloc_data_addend, 0);
        new_opnd = irop_make_symref(-1, pool_idx, 0 /* not lval */, 0, 1 /* is_const */, result_btype);
        new_opnd.is_unsigned = result_is_unsigned;
      }
      else
      {
        int64_t val = 0;
        if (!is_bss)
        {
          const unsigned char *ptr = sec->data + off;
          memcpy(&val, ptr, read_size);
          if (!result_is_unsigned && read_size < 8)
          {
            int shift = (8 - read_size) * 8;
            val = (int64_t)(val << shift) >> shift;
          }
        }

        if (result_btype == IROP_BTYPE_INT64 || val != (int64_t)(int32_t)val)
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_opnd = irop_make_i64(-1, pool_idx, result_btype);
        }
        else
        {
          new_opnd = irop_make_imm32(-1, (int32_t)val, result_btype);
        }
        new_opnd.is_unsigned = result_is_unsigned;
      }

      if (q->op == TCCIR_OP_LOAD && slot == 0)
      {
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, new_opnd);
      }
      else if (slot == 0)
      {
        tcc_ir_set_src1(ir, i, new_opnd);
      }
      else
      {
        tcc_ir_set_src2(ir, i, new_opnd);
      }
      changes++;
    }
  }

  return changes;
}

/* ---------------------------------------------------------------------------
 * Symref-constant propagation
 *
 * Propagate `ASSIGN T <-- &S+addend` (a symref-by-value, not is_lval) into
 * subsequent uses of T.  Each use is replaced with a fresh symref operand
 * carrying the same sym + addend, preserving the use's is_lval / is_unsigned
 * flags so that `T***DEREF***` becomes `&S+addend***DEREF***` — a lval-symref
 * that downstream global-init-prop can then read out of the section data.
 *
 * Scope is per straight-line basic block: any jump / merge / function call
 * clears the tracked map.  Tmps must be single-defined within the block
 * (no later redef).  Restricted to TMP vregs (not VAR/PARAM) because VAR
 * lifetimes span blocks and PARAM values are owned by the caller.
 */
int tcc_ir_opt_symref_const_prop(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  const int n = ir->next_instruction_index;
  int changes = 0;

  int max_tmp_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    const int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos > max_tmp_pos)
      max_tmp_pos = pos;
  }
  if (max_tmp_pos == 0)
    return 0;

  /* Per-tmp tracked symref. gen 0 means invalid; bumps on block boundaries. */
  typedef struct
  {
    int gen;
    uint32_t pool_idx;
    int btype;
    uint8_t is_local;
    uint8_t is_const;
    uint8_t is_unsigned;
  } SymrefTmp;

  SymrefTmp *map = tcc_mallocz(sizeof(SymrefTmp) * (max_tmp_pos + 1));
  int current_gen = 1;
  int *block_start_seen = tcc_mallocz(sizeof(int) * n);
  int block_start_gen = 1;
  ir_opt_mark_block_starts(ir, block_start_seen, block_start_gen, n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (i != 0 && block_start_seen[i] == block_start_gen)
      current_gen++;

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Control-flow instructions clear the tracked map (lightweight tracker
     * doesn't model cross-block flow) and we do NOT substitute their
     * operands — JUMP/JUMPIF carry the branch target in `dest` and a
     * condition token in `src1`; neither should be rewritten. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      current_gen++;
      continue;
    }
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      current_gen++;
      /* fall through to substitute call argument operands */
    }

    /* Substitute symref into operand uses (src1, src2).  Skip the dest. */
    for (int slot = 0; slot < 2; slot++)
    {
      int has = (slot == 0) ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2;
      if (!has)
        continue;
      IROperand opnd = (slot == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      /* Operand must be a plain vreg (not already a sym/imm operand). */
      if (opnd.is_sym)
        continue;
      int32_t opnd_vr = irop_get_vreg(opnd);
      if (TCCIR_DECODE_VREG_TYPE(opnd_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(opnd_vr);
      if (pos > max_tmp_pos || map[pos].gen != current_gen)
        continue;

      /* Replace with a fresh symref operand carrying use-site flags. */
      IROperand new_opnd = irop_make_symref(-1, map[pos].pool_idx, opnd.is_lval, map[pos].is_local,
                                            map[pos].is_const, irop_get_btype(opnd));
      new_opnd.is_unsigned = opnd.is_unsigned;

      if (slot == 0)
        tcc_ir_set_src1(ir, i, new_opnd);
      else
        tcc_ir_set_src2(ir, i, new_opnd);
      changes++;
    }

    /* Record new ASSIGN(symref) definitions for downstream substitution. */
    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        if (src1.is_sym && !src1.is_lval)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(dvr);
          if (pos <= max_tmp_pos)
          {
            map[pos].gen = current_gen;
            map[pos].pool_idx = (uint32_t)src1.u.pool_idx;
            map[pos].btype = irop_get_btype(src1);
            map[pos].is_local = src1.is_local;
            map[pos].is_const = src1.is_const;
            map[pos].is_unsigned = src1.is_unsigned;
          }
        }
      }
    }
    /* Any other write that targets a tracked tmp invalidates it. */
    else if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos <= max_tmp_pos && map[pos].gen == current_gen)
          map[pos].gen = 0;
      }
    }
  }

  tcc_free(map);
  tcc_free(block_start_seen);
  return changes;
}

/* Complex Constant Param Folding — compile-time evaluation/folding for
 * _Complex float locals passed by value to a call.
 *
 * Pattern:
 *   StackLoc[-N]   <-- #C_real [STORE]         (4-byte float constant)
 *   StackLoc[-N+4] <-- #C_imag [STORE]         (4-byte float constant)
 *   FUNCPARAMVAL  src1 = StackLoc[-N]          (8-byte read, complex lval)
 *
 * When the 8-byte slot [-N, -N+8) is touched by exactly these three ops —
 * no other read, write, or address-of references it — pack {real,imag}
 * into a 64-bit complex-float immediate (real in low 32 bits, imag in
 * high 32 bits) and rewrite the PARAM source as that immediate.  The
 * two component stores become dead and are NOP'd; the codegen path for
 * complex constants then materializes the value directly into the
 * callee's argument registers, skipping the stack round-trip.
 */
int tcc_ir_opt_complex_const_param_fold(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  const int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src1) != IROP_TAG_STACKOFF)
      continue;
    if (!src1.is_lval || !src1.is_complex)
      continue;
    /* Only handle _Complex float (8 bytes packed: real_u32 | imag_u32 << 32) */
    if (src1.btype != IROP_BTYPE_FLOAT32)
      continue;
    /* Skip if this stack offset carries a vreg (spill slot for a vreg) — those
     * are not raw stack locals and need different treatment. */
    if (irop_get_vreg(src1) != -1)
      continue;
    /* Skip incoming-arg stack slots; they alias caller-allocated memory. */
    if (src1.is_param)
      continue;

    int real_off = (int)irop_get_stack_offset(src1);
    int imag_off = real_off + 4;

    int real_store_idx = -1;
    int imag_store_idx = -1;
    uint32_t real_bits = 0;
    uint32_t imag_bits = 0;
    int conflict = 0;

    for (int j = 0; j < n && !conflict; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *p = &ir->compact_instructions[j];
      if (p->op == TCCIR_OP_NOP)
        continue;

      /* dest: detect a 4-byte constant STORE/ASSIGN to either component slot. */
      if (irop_config[p->op].has_dest)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, p);
        if (irop_get_tag(dest) == IROP_TAG_STACKOFF && irop_get_vreg(dest) == -1 && dest.is_lval && !dest.is_param)
        {
          int doff = (int)irop_get_stack_offset(dest);
          if (doff == real_off || doff == imag_off)
          {
            if ((p->op != TCCIR_OP_STORE && p->op != TCCIR_OP_ASSIGN) || dest.is_complex ||
                dest.btype != IROP_BTYPE_FLOAT32)
            {
              conflict = 1;
              break;
            }
            IROperand sv = tcc_ir_op_get_src1(ir, p);
            int tag = irop_get_tag(sv);
            if (sv.is_sym || sv.is_lval || sv.is_complex)
            {
              conflict = 1;
              break;
            }
            if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_F32)
            {
              conflict = 1;
              break;
            }
            uint32_t bits = (uint32_t)irop_get_imm64_ex(ir, sv);
            if (doff == real_off)
            {
              if (real_store_idx != -1 || j >= i)
              {
                conflict = 1;
                break;
              }
              real_store_idx = j;
              real_bits = bits;
            }
            else
            {
              if (imag_store_idx != -1 || j >= i)
              {
                conflict = 1;
                break;
              }
              imag_store_idx = j;
              imag_bits = bits;
            }
            continue;
          }
        }
      }

      /* Any other reference to the 8-byte slot disqualifies the fold. */
      for (int s = 0; s < 2; s++)
      {
        if (s == 0 && !irop_config[p->op].has_src1)
          continue;
        if (s == 1 && !irop_config[p->op].has_src2)
          continue;
        IROperand src = s ? tcc_ir_op_get_src2(ir, p) : tcc_ir_op_get_src1(ir, p);
        if (irop_get_tag(src) != IROP_TAG_STACKOFF)
          continue;
        if (irop_get_vreg(src) != -1)
          continue;
        int soff = (int)irop_get_stack_offset(src);
        if (soff >= real_off && soff < imag_off + 4)
        {
          conflict = 1;
          break;
        }
      }
    }

    if (conflict || real_store_idx < 0 || imag_store_idx < 0)
      continue;

    /* Pack {real, imag} into a 64-bit complex-float immediate. */
    uint64_t packed = (uint64_t)real_bits | ((uint64_t)imag_bits << 32);
    uint32_t pool_idx = tcc_ir_pool_add_i64(ir, (int64_t)packed);
    IROperand new_src1 = irop_make_i64(-1, pool_idx, IROP_BTYPE_FLOAT32);
    new_src1.is_complex = 1;
    new_src1.is_lval = 0;

    tcc_ir_set_src1(ir, i, new_src1);
    ir->compact_instructions[real_store_idx].op = TCCIR_OP_NOP;
    ir->compact_instructions[imag_store_idx].op = TCCIR_OP_NOP;

    LOG_IR_GEN("=== COMPLEX CONST PARAM FOLD: stack[%d..%d] folded into PARAM at i=%d ===", real_off, real_off + 7, i);
    changes++;
  }

  return changes;
}

/* Dead Call Result Elimination — convert FUNCCALLVAL → FUNCCALLVOID when
 * the call's destination vreg has no remaining uses.  Without this, the
 * codegen emits dead `mov rN, r0` (and `mov rN+1, r1` for 8-byte returns)
 * to copy the AAPCS return registers into the destination's allocated
 * registers, even though nothing will read them.
 *
 * Typical trigger: `_Complex float z = pure_callee(...);` where z is then
 * unused — after constant folding eats the args, the call still happens
 * (we can't prove purity), but its result is dead.
 *
 * The dest of a complex-returning FUNCCALLVAL is typically encoded as a
 * "temp local" (negative vreg sentinel) rather than a regular TEMP, so we
 * compare full vreg values rather than restricting to the TEMP type.
 */

/* Locate the sret-pointer parameter spill at the prolog: the first non-NOP
 * instruction should be `STORE LocalSlot[X] <-- P0`.  Returns 1 and fills
 * *out_param_vr / *out_slot if found; 0 otherwise. */
/* Analyze whether the current function is "pure via sret": its only
 * observable side effects are writes through the sret pointer (the first
 * parameter, which holds the caller's destination for a struct/complex
 * return).  Sets func_sym->f.func_pure_via_sret on success.
 *
 * Allowed operations:
 *   - Reads (LOAD, ASSIGN reading params/locals)
 *   - Writes to local stack slots (.is_local + IROP_TAG_STACKOFF)
 *   - Writes through pointers derived from the sret pointer
 *   - Calls to functions marked pure / const / pure_via_sret, or to known-
 *     pure aeabi runtime helpers
 *
 * Disallowed:
 *   - Writes to globals, volatile, or arbitrary pointers
 *   - Calls to unknown functions (could have side effects)
 *   - Inline asm, setjmp/longjmp, VLA SP manipulation
 */

/* When CMP V_a,V_b is folded because pure_def_equal proved V_a==V_b via the
 * SETIF def-equality path, the SETIFs that produced V_a/V_b — and the CMPs
 * that produced flags for those SETIFs — become dead in the same step.  DCE
 * removes the SETIFs, but cannot reason about flag liveness across CMPs, so
 * the orphan CMPs survive unless we NOP them here. */
static int ir_opt_vreg_use_count(TCCIRState *ir, int32_t vreg)
{
  if (!ir || vreg < 0)
    return -1;
  int n = ir->next_instruction_index;
  int count = 0;
  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg ||
        irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == vreg)
      count++;
  }
  return count;
}

static void ir_opt_setif_chain_cleanup(TCCIRState *ir, int def1, int def2, int32_t vr1, int32_t vr2)
{
  if (def1 < 0 || def2 < 0)
    return;
  IRQuadCompact *dq1 = &ir->compact_instructions[def1];
  IRQuadCompact *dq2 = &ir->compact_instructions[def2];
  if (dq1->op != TCCIR_OP_SETIF || dq2->op != TCCIR_OP_SETIF)
    return;
  /* Caller has just NOPped the CMP that consumed V_a/V_b — if no other live
   * use remains, the SETIFs become dead and so do their flag-producing CMPs. */
  if (ir_opt_vreg_use_count(ir, vr1) != 0 || ir_opt_vreg_use_count(ir, vr2) != 0)
    return;

  int cmp_a_idx = def1 - 1;
  while (cmp_a_idx >= 0 && ir->compact_instructions[cmp_a_idx].op == TCCIR_OP_NOP)
    cmp_a_idx--;
  int cmp_b_idx = def2 - 1;
  while (cmp_b_idx >= 0 && ir->compact_instructions[cmp_b_idx].op == TCCIR_OP_NOP)
    cmp_b_idx--;

  dq1->op = TCCIR_OP_NOP;
  dq2->op = TCCIR_OP_NOP;
  if (cmp_a_idx >= 0 && ir->compact_instructions[cmp_a_idx].op == TCCIR_OP_CMP &&
      !ir->compact_instructions[cmp_a_idx].is_jump_target)
    ir->compact_instructions[cmp_a_idx].op = TCCIR_OP_NOP;
  if (cmp_b_idx >= 0 && ir->compact_instructions[cmp_b_idx].op == TCCIR_OP_CMP &&
      !ir->compact_instructions[cmp_b_idx].is_jump_target)
    ir->compact_instructions[cmp_b_idx].op = TCCIR_OP_NOP;
}

/* Evaluate a CMP operand to a compile-time constant.  Conservative
 * extension of ir_opt_eval_const_u64 for CMP+SETIF folding:
 *   1. Direct immediates pass straight through (matches original behavior).
 *   2. Vregs are accepted only when their defining ASSIGN of an immediate
 *      lies in the SAME basic block as the use (no jump-target between
 *      def_idx and use_idx) — guards against loop back-edges where the
 *      defining instruction in linear order is not the runtime def.
 *   3. STACKOFF lvals scan back within the same BB for a STORE of an
 *      immediate to the matching offset, bailing on any aliasing op.
 *
 * Both (2) and (3) catch the patterns left behind when sl_forward and the
 * arithmetic-folding passes simplify a comparison to "const cmp vreg" or
 * "const cmp stack[X]" but don't fully resolve the second operand. */
static int eval_cmp_operand_const(TCCIRState *ir, IROperand op, int use_idx, uint64_t *out)
{
  if (irop_is_immediate(op))
  {
    *out = (uint64_t)irop_get_imm64_ex(ir, op);
    return 1;
  }

  /* Same-BB single-def ASSIGN-of-immediate trace. */
  int32_t vr = irop_get_vreg(op);
  if (vr >= 0 && !op.is_lval)
  {
    int def_idx = tcc_ir_find_defining_instruction(ir, vr, use_idx);
    if (def_idx >= 0)
    {
      /* Same-BB check: no jump_target between def_idx (exclusive) and
       * use_idx (inclusive).  Including use_idx catches the case where
       * use_idx is a control-flow merge — an alternate predecessor could
       * have defined the vreg differently, and find_defining_instruction
       * sees only the linearly preceding def. */
      int same_bb = 1;
      for (int k = def_idx + 1; k <= use_idx; k++)
      {
        IRQuadCompact *kq = &ir->compact_instructions[k];
        if (kq->op == TCCIR_OP_NOP)
          continue;
        if (kq->is_jump_target)
        {
          same_bb = 0;
          break;
        }
      }
      /* Single-def check: no other instruction in the function defines
       * this vreg.  Multiple defs (e.g. in different branches of an
       * if/else that merge at the use) make linear-scan tracing unsafe. */
      if (same_bb)
      {
        int def_count = 0;
        int n_all = ir->next_instruction_index;
        for (int k = 0; k < n_all && def_count < 2; k++)
        {
          IRQuadCompact *kq = &ir->compact_instructions[k];
          if (kq->op == TCCIR_OP_NOP)
            continue;
          if (!irop_config[kq->op].has_dest)
            continue;
          IROperand kd = tcc_ir_op_get_dest(ir, kq);
          if (irop_get_vreg(kd) == vr && !kd.is_lval)
            def_count++;
        }
        if (def_count == 1)
        {
          /* Single-def + same-BB safe: delegate to the full evaluator,
           * which handles ASSIGN/LOAD chains and arithmetic. */
          if (ir_opt_eval_const_u64(ir, op, use_idx, out, 0))
            return 1;
        }
      }
    }
  }

  /* STACKOFF-lval case: scan backwards in the same basic block for a
   * STORE of an immediate to the matching slot. */
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_lval && op.is_local && !op.is_llocal)
  {
    int64_t target_off = irop_get_stack_offset(op);
    int op_btype = irop_get_btype(op);

    for (int j = use_idx - 1; j >= 0; j--)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->is_jump_target)
        return 0;
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP)
        return 0;
      if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
        return 0;
      if (q->op == TCCIR_OP_BLOCK_COPY || q->op == TCCIR_OP_STORE_INDEXED ||
          q->op == TCCIR_OP_STORE_POSTINC)
        return 0;
      if (q->op != TCCIR_OP_STORE)
        continue;

      IROperand sdest = tcc_ir_op_get_dest(ir, q);
      if (irop_get_tag(sdest) != IROP_TAG_STACKOFF || !sdest.is_lval || !sdest.is_local || sdest.is_llocal)
        return 0;
      int64_t soff = irop_get_stack_offset(sdest);
      if (soff != target_off)
        continue;
      if (irop_get_btype(sdest) != op_btype)
        return 0;

      IROperand sval = tcc_ir_op_get_src1(ir, q);
      if (!irop_is_immediate(sval))
        return 0;
      *out = (uint64_t)irop_get_imm64_ex(ir, sval);
      return 1;
    }
  }

  return 0;
}

int tcc_ir_opt_const_prop(TCCIRState *ir)
{
  /* VarConstInfo: track constant variables */
  typedef struct
  {
    uint8_t is_constant : 1;
    uint8_t def_count : 7;
    int64_t value;
    int def_idx;   /* instruction index of the defining STORE/ASSIGN */
    int use_count; /* count of source-operand uses (capped at 255) */
  } VarConstInfo;

  /* Returns 1 if materializing `val` into a register requires a multi-instruction
   * sequence (e.g. PC-relative pool load) on Thumb-2.  Small unsigned (≤0xFFFF
   * via MOVW) and small negative (≥-0xFFFF via MVN) fit in a single instruction;
   * other patterns generally don't.  Conservative — misses some "modified
   * immediate" encodings but that just means we propagate a few constants we
   * could have hoisted.  Used to suppress propagation of large constants that
   * would otherwise be loaded redundantly at each use site. */
  #define VAR_CONST_NEEDS_POOL_LOAD(val_)                                       \
    ({ uint32_t uv_ = (uint32_t)(val_); uv_ > 0xFFFFu && uv_ < 0xFFFF0001u; })

  int n = ir->next_instruction_index;
  int changes = 0;
  int max_var_pos = 0;
  int i;
  IRQuadCompact *q;
  VarConstInfo *var_info;

  if (n == 0)
    return 0;

  int dc_stride = 0;
  uint8_t *dc = (n <= 4000) ? ir_opt_build_def_count(ir, n, &dc_stride) : NULL;

  /* Combined pass: find max_var_pos AND fold identity comparisons in a single
   * scan.  The two concerns are orthogonal — one looks at VAR dests, the other
   * looks at CMP instructions followed by JUMPIF/SETIF.
   *
   * Identity comparison folding: fold CMP+JUMPIF and CMP+SETIF when both CMP
   * operands are the same vreg.  Comparing a value to itself always yields
   * equality, so == is true, != is false, <= and >= are true, etc.
   * Runs before the VAR-centric passes so it works even when there are no VAR
   * vregs (e.g. functions that only use parameters). */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Track max VAR position from destinations */
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
      {
        const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        if (pos > max_var_pos)
          max_var_pos = pos;
      }
    }

    /* Identity comparison folding — only for CMP followed by another instr */
    if (q->op != TCCIR_OP_CMP || i + 1 >= n)
      continue;

    IRQuadCompact *cmp_q = q;
    IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cmp_q);
    IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cmp_q);

    /* CMP commute peephole: CMP #imm, Vreg → CMP Vreg, #imm.
     * The backend can encode `cmp Rn, #imm8` as a 16-bit T1 instruction
     * (or `cmp.w Rn, #imm12` as T2). Keeping the immediate on the RHS
     * avoids materializing the constant into a register first.
     * When swapping operands, the consumer's comparison condition must be
     * swapped accordingly (LT<->GT, LE<->GE, EQ/NE unchanged).
     * Restricted to 32-bit integer comparisons against a register-resident
     * vreg: 64-bit CMPs decompose into hi/lo handled specially by codegen,
     * and DEREF/symbol/float operands have backend-specific encoding rules
     * that can be broken by a naive swap. */
    if (irop_is_immediate(cmp_src1) && !irop_is_immediate(cmp_src2)
        && irop_get_vreg(cmp_src2) >= 0
        && !cmp_src1.is_lval && !cmp_src2.is_lval
        && !cmp_src1.is_sym && !cmp_src2.is_sym
        && !cmp_src1.is_complex && !cmp_src2.is_complex)
    {
      int b1 = irop_get_btype(cmp_src1);
      int b2 = irop_get_btype(cmp_src2);
      int is_32bit_int = (b1 != IROP_BTYPE_INT64 && b1 != IROP_BTYPE_FLOAT32
                          && b1 != IROP_BTYPE_FLOAT64 && b2 != IROP_BTYPE_INT64
                          && b2 != IROP_BTYPE_FLOAT32 && b2 != IROP_BTYPE_FLOAT64);
      if (is_32bit_int)
      {
        IRQuadCompact *cons_q = &ir->compact_instructions[i + 1];
        int cond_pool_off = -1; /* offset into operand pool where cond is stored */
        if (cons_q->op == TCCIR_OP_JUMPIF || cons_q->op == TCCIR_OP_SETIF)
          cond_pool_off = irop_config[cons_q->op].has_dest; /* src1 slot */
        else if (cons_q->op == TCCIR_OP_SELECT)
          cond_pool_off = 3; /* dest, src1=then, src2=else, cond at +3 */

        if (cond_pool_off >= 0)
        {
          IROperand cur_cond = ir->iroperand_pool[cons_q->operand_base + cond_pool_off];
          int tok = (int)irop_get_imm64_ex(ir, cur_cond);
          int swapped = vrp_swap_cmp_tok(tok);
          if (swapped > 0)
          {
            tcc_ir_op_set_src1(ir, cmp_q, cmp_src2);
            tcc_ir_op_set_src2(ir, cmp_q, cmp_src1);
            int btype = irop_get_btype(cur_cond);
            ir->iroperand_pool[cons_q->operand_base + cond_pool_off] =
                irop_make_imm32(-1, swapped, btype);
            changes++;
            continue;
          }
        }
      }
    }

    /* Check if both operands are provably identical (identity comparison).
     * First check: same vreg with same is_lval flag.
     * Second check: different vregs but structurally equal expressions
     * (e.g. both compute "base + 5" via independent ADD instructions). */
    {
      int is_identity = 0;
      int32_t vr1 = irop_get_vreg(cmp_src1);
      int32_t vr2 = irop_get_vreg(cmp_src2);

      if (vr1 >= 0 && vr2 >= 0 && vr1 == vr2 && cmp_src1.is_lval == cmp_src2.is_lval)
      {
        /* Same vreg — check symbol refs for struct field disambiguation */
        if (cmp_src1.is_sym || cmp_src2.is_sym)
        {
          if (cmp_src1.is_sym == cmp_src2.is_sym)
          {
            IRPoolSymref *ref1 = irop_get_symref_ex(ir, cmp_src1);
            IRPoolSymref *ref2 = irop_get_symref_ex(ir, cmp_src2);
            if (ref1 && ref2 && ref1->sym == ref2->sym && ref1->addend == ref2->addend)
              is_identity = 1;
          }
        }
        else
          is_identity = 1;
      }

      /* Try definition-level equality for different vregs.
       * Compares the defining instructions directly (same op, same
       * operands), including cross-tag comparisons (VAR vs TEMP).
       * Guarded: find_defining_instruction is O(n) per CMP. */
      int identity_def1 = -1, identity_def2 = -1;
      int identity_via_setif = 0;
      if (!is_identity && n <= 4000 && vr1 >= 0 && vr2 >= 0 && vr1 != vr2 &&
          DC_IS_SINGLE_DEF(dc, dc_stride, vr1) && DC_IS_SINGLE_DEF(dc, dc_stride, vr2))
      {
        int def1 = tcc_ir_find_defining_instruction(ir, vr1, i);
        int def2 = tcc_ir_find_defining_instruction(ir, vr2, i);
        if (def1 >= 0 && def2 >= 0 && ir_opt_pure_def_equal(ir, def1, def2, 0))
        {
          is_identity = 1;
          identity_def1 = def1;
          identity_def2 = def2;
          if (ir->compact_instructions[def1].op == TCCIR_OP_SETIF &&
              ir->compact_instructions[def2].op == TCCIR_OP_SETIF)
            identity_via_setif = 1;
        }
      }

      /* Identity-via-copy: CMP A, B where one operand was assigned from the
       * other (e.g. `int *f = &r->f;` then `f == r` when f is at offset 0).
       *
       * Safe when:
       *   - one side (copy_vr) is VAR/TEMP with exactly one IR definition,
       *     and that definition is an ASSIGN from the other side (orig_vr),
       *   - the ASSIGN source is a plain register read (not lval / not symref),
       *   - orig_vr is a PARAM that no instruction in the function writes to
       *     (def count == 0), so its value is constant from entry,
       *   - no jump target appears between the ASSIGN and the CMP, so every
       *     control-flow path that reaches the CMP went through the copy. */
      if (!is_identity && n <= 4000 && dc && vr1 >= 0 && vr2 >= 0 && vr1 != vr2 &&
          !cmp_src1.is_sym && !cmp_src2.is_sym)
      {
        for (int dir = 0; dir < 2 && !is_identity; dir++)
        {
          int32_t copy_vr = (dir == 0) ? vr1 : vr2;
          int32_t orig_vr = (dir == 0) ? vr2 : vr1;
          int copy_type = TCCIR_DECODE_VREG_TYPE(copy_vr);
          int orig_type = TCCIR_DECODE_VREG_TYPE(orig_vr);
          if (copy_type != TCCIR_VREG_TYPE_VAR && copy_type != TCCIR_VREG_TYPE_TEMP)
            continue;
          if (orig_type != TCCIR_VREG_TYPE_PARAM)
            continue;
          if (!DC_IS_SINGLE_DEF(dc, dc_stride, copy_vr))
            continue;
          int orig_pos = TCCIR_DECODE_VREG_POSITION(orig_vr);
          if (dc[TCCIR_VREG_TYPE_PARAM * dc_stride + orig_pos] != 0)
            continue;
          int def_idx = tcc_ir_find_defining_instruction(ir, copy_vr, i);
          if (def_idx < 0)
            continue;
          IRQuadCompact *defq = &ir->compact_instructions[def_idx];
          if (defq->op != TCCIR_OP_ASSIGN)
            continue;
          IROperand asn_src = tcc_ir_op_get_src1(ir, defq);
          if (asn_src.is_lval || asn_src.is_sym)
            continue;
          if (irop_get_vreg(asn_src) != orig_vr)
            continue;
          int blocked = 0;
          for (int k = def_idx + 1; k < i; k++)
          {
            IRQuadCompact *kq = &ir->compact_instructions[k];
            if (kq->op == TCCIR_OP_NOP)
              continue;
            if (kq->is_jump_target)
            {
              blocked = 1;
              break;
            }
          }
          if (!blocked)
            is_identity = 1;
        }
      }

      if (!is_identity)
        continue;

      IRQuadCompact *next_q = &ir->compact_instructions[i + 1];

      if (next_q->op == TCCIR_OP_JUMPIF)
      {
        IROperand cond = tcc_ir_op_get_src1(ir, next_q);
        int tok = (int)irop_get_imm64_ex(ir, cond);
        /* evaluate_compare_condition(x, x, cond) — use 0,0 as representative */
        int result = evaluate_compare_condition(0, 0, tok);
        if (result < 0)
          continue;

        IROperand jmp_dest = tcc_ir_op_get_dest(ir, next_q);
        if (result)
        {
          /* Branch always taken — convert CMP to NOP, JUMPIF to unconditional JUMP */
          cmp_q->op = TCCIR_OP_NOP;
          next_q->op = TCCIR_OP_JUMP;
          tcc_ir_set_dest(ir, i + 1, jmp_dest);
        }
        else
        {
          /* Branch never taken — eliminate both */
          cmp_q->op = TCCIR_OP_NOP;
          next_q->op = TCCIR_OP_NOP;
        }
        if (identity_via_setif)
          ir_opt_setif_chain_cleanup(ir, identity_def1, identity_def2, vr1, vr2);
        changes++;
      }
      else if (next_q->op == TCCIR_OP_SETIF)
      {
        IROperand setif_src1 = tcc_ir_op_get_src1(ir, next_q);
        int tok = (int)irop_get_imm64_ex(ir, setif_src1);
        int result = evaluate_compare_condition(0, 0, tok);
        if (result < 0)
          continue;

        int btype = irop_get_btype(setif_src1);
        cmp_q->op = TCCIR_OP_NOP;
        next_q->op = TCCIR_OP_ASSIGN;
        IROperand new_src1 = irop_make_imm32(-1, result, btype);
        tcc_ir_set_src1(ir, i + 1, new_src1);
        tcc_ir_set_src2(ir, i + 1, IROP_NONE);
        if (identity_via_setif)
          ir_opt_setif_chain_cleanup(ir, identity_def1, identity_def2, vr1, vr2);
        changes++;
      }
    }
  }

  /* max_var_pos tracks the highest VAR position seen.  When no VAR dests
   * exist at all, the subsequent VAR-only passes have nothing to do, but
   * the two-const fold and algebraic simplifications at the end of the
   * function are still needed (they're op-level, not VAR-level).
   * Use `has_var_dests` to distinguish "no VARs" from "only V0@pos=0". */
  int has_var_dests = 0;
  for (i = 0; i < n && !has_var_dests; i++)
  {
    q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_dest)
    {
      int32_t dv = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
        has_var_dests = 1;
    }
  }

  var_info = has_var_dests ? tcc_mallocz(sizeof(VarConstInfo) * (max_var_pos + 1)) : NULL;

  /* First pass: identify constant variables (skip if no VAR dests) */
  if (has_var_dests)
    for (i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];

      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Track definitions of VAR vregs */
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
      {
        const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        if (pos <= max_var_pos)
        {
          /* If the address of a local is taken, it can be modified through aliases
           * (e.g. passed as an out-parameter). Such variables are not safe for
           * constant propagation even if they are only assigned once.
           *
           * Complex types (_Complex float/double) are stored as register pairs
           * (real, imag) but the constant tracker only records a single scalar
           * value. Propagating that scalar would replace both halves with the
           * same value, corrupting the imaginary part.
           */
          IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
          if (interval && (interval->addrtaken || interval->is_complex))
          {
            var_info[pos].def_count++;
            var_info[pos].is_constant = 0;
            continue;
          }

          var_info[pos].def_count++;

          /* Check if this is a constant assignment */
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) && irop_is_immediate(src1))
          {
            if (var_info[pos].def_count == 1)
            {
              var_info[pos].is_constant = 1;
              var_info[pos].value = irop_get_imm64_ex(ir, src1);
              var_info[pos].def_idx = i;
            }
          }
          else
          {
            /* Non-constant assignment - mark as non-constant */
            var_info[pos].is_constant = 0;
          }
        }
      }
    }

  /* Mark variables with multiple definitions as non-constant */
  if (var_info)
    for (i = 0; i <= max_var_pos; i++)
    {
      if (var_info[i].def_count > 1)
        var_info[i].is_constant = 0;
    }

  /* Count source-operand uses of each VAR.  Used below to suppress
   * propagation of large constants with multiple uses — propagating a
   * pool-loaded value into N uses creates N materializations that the
   * regalloc can't undo, whereas keeping the VAR alive lets a single
   * load satisfy all reads. */
  if (var_info)
    for (i = 0; i < n; i++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[i];
      if (uq->op == TCCIR_OP_NOP) continue;
      for (int oi = 0; oi < 2; oi++) {
        if (oi == 0 && !irop_config[uq->op].has_src1) continue;
        if (oi == 1 && !irop_config[uq->op].has_src2) continue;
        IROperand op = oi == 0 ? tcc_ir_op_get_src1(ir, uq) : tcc_ir_op_get_src2(ir, uq);
        int32_t vr = irop_get_vreg(op);
        if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR) continue;
        if (op.is_local && !op.is_lval) continue; /* address-of, not value */
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var_pos && var_info[pos].use_count < 255)
          var_info[pos].use_count++;
      }
    }

  /* Second pass: propagate constants and apply algebraic simplifications */
  for (i = 0; i < n; i++)
  {
    int src1_is_const, src2_is_const;
    int64_t result;
    int can_fold;
    int skip_bool_prop;

    q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* For BOOL_AND/BOOL_OR, don't propagate constants unless both become constants.
     * The code generator can't handle mixed const/reg operands for these ops. */
    skip_bool_prop = 0;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (q->op == TCCIR_OP_BOOL_AND || q->op == TCCIR_OP_BOOL_OR)
    {
      int src1_can_be_const = 0, src2_can_be_const = 0;
      /* Check if both would become constants */
      int32_t src1_vr = irop_get_vreg(src1);
      if (var_info && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (pos <= max_var_pos && var_info[pos].is_constant)
          src1_can_be_const = 1;
      }
      else if (irop_is_immediate(src1))
        src1_can_be_const = 1;

      int32_t src2_vr = irop_get_vreg(src2);
      if (var_info && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
        if (pos <= max_var_pos && var_info[pos].is_constant)
          src2_can_be_const = 1;
      }
      else if (irop_is_immediate(src2))
        src2_can_be_const = 1;

      /* Skip propagation if only ONE would become constant (can't generate code) */
      if (src1_can_be_const != src2_can_be_const)
        skip_bool_prop = 1;
    }

    /* Propagate constant VAR vregs to immediate values.
     * IMPORTANT: Don't propagate if src1 is local without lval - that means
     * "address of local variable", not its value. The address must be computed at runtime. */
    int32_t src1_vr = irop_get_vreg(src1);
    if (var_info && !skip_bool_prop && irop_config[q->op].has_src1 &&
        TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
      if (src1.is_local && !src1.is_lval)
        continue;
      if (pos <= max_var_pos && var_info[pos].is_constant)
      {
        int64_t val = var_info[pos].value;
        /* Suppress propagation of large (pool-loaded) constants with
         * multiple uses — keep the VAR alive so a single load suffices. */
        if (var_info[pos].use_count > 1 && VAR_CONST_NEEDS_POOL_LOAD(val))
          continue;
        IROperand new_src1;
        int btype = irop_get_btype(src1);
        if (val == (int32_t)val)
        {
          new_src1 = irop_make_imm32(-1, (int32_t)val, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_src1 = irop_make_i64(-1, pool_idx, btype);
        }
        /* Preserve type flags but NOT memory-access flags.
         * is_lval/is_llocal/is_local describe stack-slot semantics that
         * don't apply to an immediate constant value. */
        new_src1.is_unsigned = src1.is_unsigned;
        new_src1.is_static = src1.is_static;
        tcc_ir_set_src1(ir, i, new_src1);
        if (q->op == TCCIR_OP_LOAD)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          if (!d.is_lval && (btype == IROP_BTYPE_INT64 || val == (int32_t)val))
            q->op = TCCIR_OP_ASSIGN;
        }
        changes++;
      }
    }

    int32_t src2_vr = irop_get_vreg(src2);
    if (var_info && !skip_bool_prop && irop_config[q->op].has_src2 &&
        TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR && !(src2.is_local && !src2.is_lval))
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
      if (pos <= max_var_pos && var_info[pos].is_constant)
      {
        int64_t val = var_info[pos].value;
        /* Same suppression as for src1 above. */
        if (var_info[pos].use_count > 1 && VAR_CONST_NEEDS_POOL_LOAD(val))
          continue;
        IROperand new_src2;
        int btype = irop_get_btype(src2);
        if (val == (int32_t)val)
        {
          new_src2 = irop_make_imm32(-1, (int32_t)val, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_src2 = irop_make_i64(-1, pool_idx, btype);
        }
        /* Preserve type flags but NOT memory-access flags. */
        new_src2.is_unsigned = src2.is_unsigned;
        new_src2.is_static = src2.is_static;
        tcc_ir_set_src2(ir, i, new_src2);
        changes++;
      }
    }

    /* Re-read operands after propagation to get updated values */
    src1 = tcc_ir_op_get_src1(ir, q);
    src2 = tcc_ir_op_get_src2(ir, q);

    /* Algebraic simplifications */
    src1_is_const = irop_config[q->op].has_src1 ? irop_is_immediate(src1) : 0;
    src2_is_const = irop_config[q->op].has_src2 ? irop_is_immediate(src2) : 0;

    /* For commutative operations, if src1 is const and src2 is not, swap them.
     * This ensures constants end up in src2 where the code generator expects them.
     * Note: BOOL_AND/BOOL_OR are not included because the code generator doesn't
     * handle constants in either operand - they require both to be registers. */
    if (irop_config[q->op].has_src1 && irop_config[q->op].has_src2 && src1_is_const && !src2_is_const)
    {
      int is_commutative = 0;
      switch (q->op)
      {
      case TCCIR_OP_ADD:
      case TCCIR_OP_MUL:
      case TCCIR_OP_AND:
      case TCCIR_OP_OR:
      case TCCIR_OP_XOR:
        is_commutative = 1;
        break;
      default:
        break;
      }
      if (is_commutative)
      {
        IROperand tmp;
        LOG_IR_GEN("OPTIMIZE: Swap operands for commutative %s (const in src1) at i=%d", tcc_ir_get_op_name(q->op), i);
        tmp = src1;
        src1 = src2;
        src2 = tmp;
        tcc_ir_set_src1(ir, i, src1);
        tcc_ir_set_src2(ir, i, src2);
        /* Update flags after swap */
        src1_is_const = 0;
        src2_is_const = 1;
      }
    }

    /* Full constant folding: C1 OP C2 = result */
    result = 0;
    can_fold = 1;

    if (irop_config[q->op].has_src1 && irop_config[q->op].has_src2 && src1_is_const && src2_is_const)
    {
      int64_t val1 = irop_get_imm64_ex(ir, src1);
      int64_t val2 = irop_get_imm64_ex(ir, src2);
      int btype = irop_get_btype(src1);

      switch (q->op)
      {
      case TCCIR_OP_ADD:
        result = (int64_t)((uint64_t)val1 + (uint64_t)val2);
        break;
      case TCCIR_OP_SUB:
        result = (int64_t)((uint64_t)val1 - (uint64_t)val2);
        break;
      case TCCIR_OP_MUL:
        result = (int64_t)((uint64_t)val1 * (uint64_t)val2);
        break;
      case TCCIR_OP_AND:
        result = val1 & val2;
        break;
      case TCCIR_OP_OR:
        result = val1 | val2;
        break;
      case TCCIR_OP_XOR:
        result = val1 ^ val2;
        break;
      case TCCIR_OP_SHL:
        result = (int64_t)((uint64_t)val1 << val2);
        break;
      case TCCIR_OP_SHR:
        if (btype == IROP_BTYPE_INT64)
          result = (uint64_t)val1 >> val2;
        else
          result = (uint32_t)val1 >> val2;
        break;
      case TCCIR_OP_SAR:
        result = val1 >> val2;
        break;
      case TCCIR_OP_ROR:
      {
        uint32_t v = (uint32_t)val1;
        uint32_t n = (uint32_t)val2 & 31;
        result = (int64_t)(int32_t)((v >> n) | (v << (32 - n)));
        break;
      }
      case TCCIR_OP_BOOL_AND:
        result = (val1 != 0) && (val2 != 0) ? 1 : 0;
        break;
      case TCCIR_OP_BOOL_OR:
        result = (val1 != 0) || (val2 != 0) ? 1 : 0;
        break;
      case TCCIR_OP_IMOD:
        if (val2 != 0)
        {
          result = val1 % val2;
        }
        else
        {
          can_fold = 0; /* Division by zero - don't fold */
        }
        break;
      case TCCIR_OP_DIV:
        if (val2 != 0)
        {
          result = val1 / val2;
        }
        else
        {
          can_fold = 0; /* Division by zero - don't fold */
        }
        break;
      case TCCIR_OP_UDIV:
        if (val2 != 0)
        {
          if (btype == IROP_BTYPE_INT64)
            result = (uint64_t)val1 / (uint64_t)val2;
          else
            result = (uint32_t)val1 / (uint32_t)val2;
        }
        else
        {
          can_fold = 0; /* Division by zero - don't fold */
        }
        break;
      case TCCIR_OP_UMOD:
        if (val2 != 0)
        {
          if (btype == IROP_BTYPE_INT64)
            result = (uint64_t)val1 % (uint64_t)val2;
          else
            result = (uint32_t)val1 % (uint32_t)val2;
        }
        else
        {
          can_fold = 0; /* Division by zero - don't fold */
        }
        break;
      case TCCIR_OP_UMULL:
      {
        uint64_t uresult = (uint64_t)(uint32_t)val1 * (uint64_t)(uint32_t)val2;
        result = (int64_t)uresult;
        btype = IROP_BTYPE_INT64;
        break;
      }
      case TCCIR_OP_SMULL:
      {
        int64_t sresult = (int64_t)(int32_t)val1 * (int64_t)(int32_t)val2;
        result = sresult;
        btype = IROP_BTYPE_INT64;
        break;
      }
      case TCCIR_OP_UBFX:
      {
        int lsb = (int)val2 & 0x1F;
        int width = ((int)val2 >> 5) & 0x1F;
        if (width > 0 && width <= 32)
          result = ((uint32_t)val1 >> lsb) & ((1u << width) - 1);
        else
          can_fold = 0;
        break;
      }
      default:
        can_fold = 0;
        break;
      }

      /* Truncate to the operand's natural width so that 32-bit wrapping
       * arithmetic is modeled correctly (e.g. 0x80000000 + 0x80000000 wraps
       * to 0 in 32-bit).
       * Exception: SHL by >= 32 on a 32-bit type.  64-bit multiply chains
       * use 32-bit-typed temps with SHL #32 to position values in the upper
       * half of a register pair; truncating that to 0 is incorrect. */
      if (can_fold && btype != IROP_BTYPE_INT64 && btype != IROP_BTYPE_FLOAT64)
      {
        if (q->op == TCCIR_OP_SHL && val2 >= 32)
        {
          IROperand dest = tcc_ir_op_get_dest(ir, q);
          if (irop_get_btype(dest) == IROP_BTYPE_INT64)
            btype = IROP_BTYPE_INT64;
          else
            can_fold = 0;
        }
        else
          result = (int64_t)(int32_t)(uint32_t)result;
      }

      if (can_fold)
      {
        LOG_IR_GEN("OPTIMIZE: Constant fold %s(%lld, %lld) = %lld at i=%d", tcc_ir_get_op_name(q->op), (long long)val1,
                   (long long)val2, (long long)result, i);
        q->op = TCCIR_OP_ASSIGN;
        IROperand new_src1;
        if (result == (int32_t)result)
        {
          new_src1 = irop_make_imm32(-1, (int32_t)result, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
          new_src1 = irop_make_i64(-1, pool_idx, btype);
        }
        tcc_ir_set_src1(ir, i, new_src1);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
        continue;
      }
    }

    /* Algebraic simplifications with one constant operand */
    if (irop_config[q->op].has_src2 && src2_is_const)
    {
      int64_t c = irop_get_imm64_ex(ir, src2);
      int simplify;
      int replace_with_zero;
      int replace_with_const;
      int trap_on_div_zero;
      int64_t const_value;
      int btype = irop_get_btype(src1);

      simplify = 0;
      replace_with_zero = 0;
      replace_with_const = 0;
      trap_on_div_zero = 0;
      const_value = 0;

      switch (q->op)
      {
      case TCCIR_OP_ADD:
      case TCCIR_OP_SUB:
        if (c == 0)
          simplify = 1; /* X + 0 = X, X - 0 = X */
        break;
      case TCCIR_OP_OR:
        if (c == 0)
          simplify = 1; /* X | 0 = X */
        else if (c == -1 || (btype != IROP_BTYPE_INT64 && c == 0xFFFFFFFF))
        {
          replace_with_const = 1; /* X | -1 = -1 */
          const_value = -1;
        }
        break;
      case TCCIR_OP_SHL:
      case TCCIR_OP_SHR:
      case TCCIR_OP_SAR:
      case TCCIR_OP_ROR:
        if (c == 0)
          simplify = 1; /* X << 0 = X, X >> 0 = X, X ror 0 = X */
        break;
      case TCCIR_OP_MUL:
        if (c == 1)
          simplify = 1; /* X * 1 = X */
        else if (c == 0)
          replace_with_zero = 1; /* X * 0 = 0 */
        break;
      case TCCIR_OP_DIV:
      case TCCIR_OP_UDIV:
        if (c == 1)
          simplify = 1; /* X / 1 = X */
        else if (c == 0)
          trap_on_div_zero = 1; /* X / 0 is UB — emit trap */
        break;
      case TCCIR_OP_IMOD:
      case TCCIR_OP_UMOD:
        if (c == 0)
          trap_on_div_zero = 1; /* X % 0 is UB — emit trap */
        break;
      case TCCIR_OP_AND:
        if (c == 0)
          replace_with_zero = 1; /* X & 0 = 0 */
        else if (c == -1 || (btype != IROP_BTYPE_INT64 && c == 0xFFFFFFFF))
          simplify = 1; /* X & -1 = X */
        break;
      case TCCIR_OP_XOR:
        if (c == 0)
          simplify = 1; /* X ^ 0 = X */
        break;
      default:
        break;
      }

      if (simplify)
      {
        LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(x, %lld) = x at i=%d", tcc_ir_get_op_name(q->op), (long long)c, i);
        q->op = TCCIR_OP_ASSIGN;
        /* src1 stays as-is, clear src2 */
        tcc_ir_set_src1(ir, i, src1);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
      else if (replace_with_zero)
      {
        LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(x, %lld) = 0 at i=%d", tcc_ir_get_op_name(q->op), (long long)c, i);
        q->op = TCCIR_OP_ASSIGN;
        IROperand new_src1 = irop_make_imm32(-1, 0, btype);
        tcc_ir_set_src1(ir, i, new_src1);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
      else if (replace_with_const)
      {
        LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(x, %lld) = %lld at i=%d", tcc_ir_get_op_name(q->op), (long long)c,
                   (long long)const_value, i);
        q->op = TCCIR_OP_ASSIGN;
        IROperand new_src1;
        if (const_value == (int32_t)const_value)
        {
          new_src1 = irop_make_imm32(-1, (int32_t)const_value, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, const_value);
          new_src1 = irop_make_i64(-1, pool_idx, btype);
        }
        tcc_ir_set_src1(ir, i, new_src1);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
      else if (trap_on_div_zero)
      {
        /* Integer division/modulo by constant 0 is UB.  Replace with TRAP
         * so DCE can drop all subsequent code in the block (mirrors GCC -O2,
         * which emits a single UDF for `int b[1/0]` and similar). */
        LOG_IR_GEN("OPTIMIZE: %s by constant 0 -> trap at i=%d", tcc_ir_get_op_name(q->op), i);
        q->op = TCCIR_OP_TRAP;
        changes++;
      }
    }

    /* Handle commutative operations: 0 + X = X, 0 << X = 0 */
    if (irop_config[q->op].has_src1 && src1_is_const)
    {
      const int64_t c = irop_get_imm64_ex(ir, src1);

      switch (q->op)
      {
      case TCCIR_OP_ADD:
      case TCCIR_OP_OR:
      case TCCIR_OP_XOR:
        if (c == 0)
        {
          /* 0 + X = X, 0 | X = X, 0 ^ X = X (commutative, swap operands) */
          LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(0, x) = x at i=%d", tcc_ir_get_op_name(q->op), i);
          q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, i, src2);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
        }
        break;
      case TCCIR_OP_MUL:
        if (c == 0)
        {
          /* 0 * X = 0 */
          LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(0, x) = 0 at i=%d", tcc_ir_get_op_name(q->op), i);
          q->op = TCCIR_OP_ASSIGN;
          /* src1 is already 0 */
          tcc_ir_set_src1(ir, i, src1);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
        }
        break;
      case TCCIR_OP_SHL:
      case TCCIR_OP_SHR:
      case TCCIR_OP_SAR:
      case TCCIR_OP_ROR:
        if (c == 0)
        {
          /* 0 << X = 0, 0 >> X = 0 */
          LOG_IR_GEN("OPTIMIZE: Algebraic simplify %s(0, x) = 0 at i=%d", tcc_ir_get_op_name(q->op), i);
          q->op = TCCIR_OP_ASSIGN;
          /* src1 is already 0 */
          tcc_ir_set_src1(ir, i, src1);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
        }
        break;
      default:
        break;
      }
    }
  }

  /* Byte-cast folding: SHL #N → SHR #N → AND #mask.
   * TCC emits (byte)x as SHL #24, SHR #24 (shift up then unsigned shift down).
   * Fold to AND #0xFF which the backend can emit as UXTB or UBFX.
   * Also fold SHL #16, SHR #16 → AND #0xFFFF (halfword cast). */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *shl_q = &ir->compact_instructions[i];
    IRQuadCompact *shr_q = &ir->compact_instructions[i + 1];
    if (shl_q->op != TCCIR_OP_SHL || shr_q->op != TCCIR_OP_SHR)
      continue;
    IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
    IROperand shr_src2 = tcc_ir_op_get_src2(ir, shr_q);
    if (!irop_is_immediate(shl_src2) || !irop_is_immediate(shr_src2))
      continue;
    int64_t shl_amt = irop_get_imm64_ex(ir, shl_src2);
    int64_t shr_amt = irop_get_imm64_ex(ir, shr_src2);
    if (shl_amt != shr_amt || shl_amt <= 0 || shl_amt >= 32)
      continue;
    /* Verify the SHR reads the SHL's dest */
    IROperand shl_dest = tcc_ir_op_get_dest(ir, shl_q);
    IROperand shr_src1 = tcc_ir_op_get_src1(ir, shr_q);
    if (irop_get_vreg(shl_dest) != irop_get_vreg(shr_src1))
      continue;
    /* Skip 64-bit types: the mask computation assumes 32-bit width.
     * For INT64, SHL #16 → SHR #16 masks 48 bits, not 16.  Also check dest
     * btypes since src1 btype may have been weakened during forwarding. */
    IROperand shl_orig_src1_chk = tcc_ir_op_get_src1(ir, shl_q);
    IROperand shr_dest_chk = tcc_ir_op_get_dest(ir, shr_q);
    if (shl_orig_src1_chk.btype == IROP_BTYPE_INT64 || shl_orig_src1_chk.btype == IROP_BTYPE_FLOAT64 ||
        shl_dest.btype == IROP_BTYPE_INT64 || shl_dest.btype == IROP_BTYPE_FLOAT64 ||
        shr_dest_chk.btype == IROP_BTYPE_INT64 || shr_dest_chk.btype == IROP_BTYPE_FLOAT64)
      continue;
    /* SHL #N then SHR #N = AND with mask of (32-N) low bits */
    uint32_t mask = (shl_amt == 32) ? 0 : ((1u << (32 - shl_amt)) - 1);
    /* Replace SHL with AND, NOP the SHR */
    IROperand shl_orig_src1 = tcc_ir_op_get_src1(ir, shl_q);
    IROperand shr_dest = tcc_ir_op_get_dest(ir, shr_q);
    shr_q->op = TCCIR_OP_AND;
    tcc_ir_set_dest(ir, i + 1, shr_dest);
    tcc_ir_set_src1(ir, i + 1, shl_orig_src1);
    tcc_ir_set_src2(ir, i + 1, irop_make_imm32(-1, (int32_t)mask, IROP_BTYPE_INT32));
    shl_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* XOR cancellation: (x ^ C) ^ C = x.
   * Two consecutive XORs with the same constant cancel out. */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *xor1_q = &ir->compact_instructions[i];
    IRQuadCompact *xor2_q = &ir->compact_instructions[i + 1];
    if (xor1_q->op != TCCIR_OP_XOR || xor2_q->op != TCCIR_OP_XOR)
      continue;
    IROperand xor1_src2 = tcc_ir_op_get_src2(ir, xor1_q);
    IROperand xor2_src2 = tcc_ir_op_get_src2(ir, xor2_q);
    if (!irop_is_immediate(xor1_src2) || !irop_is_immediate(xor2_src2))
      continue;
    if (irop_get_imm64_ex(ir, xor1_src2) != irop_get_imm64_ex(ir, xor2_src2))
      continue;
    IROperand xor1_dest = tcc_ir_op_get_dest(ir, xor1_q);
    IROperand xor2_src1 = tcc_ir_op_get_src1(ir, xor2_q);
    if (irop_get_vreg(xor1_dest) != irop_get_vreg(xor2_src1))
      continue;
    LOG_IR_GEN("OPTIMIZE: XOR cancel (x ^ %lld) ^ %lld = x at i=%d,%d", (long long)irop_get_imm64_ex(ir, xor1_src2),
               (long long)irop_get_imm64_ex(ir, xor2_src2), i, i + 1);
    IROperand xor1_src1 = tcc_ir_op_get_src1(ir, xor1_q);
    IROperand xor2_dest = tcc_ir_op_get_dest(ir, xor2_q);
    if (irop_get_vreg(xor1_src1) == irop_get_vreg(xor2_dest))
    {
      xor2_q->op = TCCIR_OP_NOP;
    }
    else
    {
      xor2_q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_dest(ir, i + 1, xor2_dest);
      tcc_ir_set_src1(ir, i + 1, xor1_src1);
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
    }
    xor1_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* SHR+AND → UBFX fusion: SHR #N then AND #((1<<W)-1) → UBFX #N,#W.
   * This fuses two instructions into one ARM UBFX instruction. */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *shr_q = &ir->compact_instructions[i];
    IRQuadCompact *and_q = &ir->compact_instructions[i + 1];
    if (shr_q->op != TCCIR_OP_SHR || and_q->op != TCCIR_OP_AND)
      continue;
    IROperand shr_src2 = tcc_ir_op_get_src2(ir, shr_q);
    IROperand and_src2 = tcc_ir_op_get_src2(ir, and_q);
    if (!irop_is_immediate(shr_src2) || !irop_is_immediate(and_src2))
      continue;
    int64_t shift = irop_get_imm64_ex(ir, shr_src2);
    int64_t mask = irop_get_imm64_ex(ir, and_src2);
    if (shift <= 0 || shift >= 32)
      continue;
    /* Check mask is (1<<W)-1 for W in {8,16} */
    int width = 0;
    if (mask == 0xFF)
      width = 8;
    else if (mask == 0xFFFF)
      width = 16;
    else
      continue;
    if (shift + width > 32)
      continue;
    /* Verify AND reads SHR's dest */
    IROperand shr_dest = tcc_ir_op_get_dest(ir, shr_q);
    IROperand and_src1 = tcc_ir_op_get_src1(ir, and_q);
    if (irop_get_vreg(shr_dest) != irop_get_vreg(and_src1))
      continue;
    /* UBFX can handle lval sources — the backend loads to a scratch register
     * first, then applies UBFX. This saves 1 instruction vs SHR+AND. */
    /* Verify SHR dest is single-use (only the AND) */
    if (!tcc_ir_vreg_has_single_use(ir, irop_get_vreg(shr_dest), i))
      continue;
    /* Fuse: NOP the SHR, change AND to UBFX with src2 = lsb|(width<<5) */
    IROperand shr_orig_src1 = tcc_ir_op_get_src1(ir, shr_q);
    IROperand and_dest = tcc_ir_op_get_dest(ir, and_q);
    int32_t ubfx_param = (int32_t)shift | (width << 5);
    and_q->op = TCCIR_OP_UBFX;
    tcc_ir_set_dest(ir, i + 1, and_dest);
    tcc_ir_set_src1(ir, i + 1, shr_orig_src1);
    tcc_ir_set_src2(ir, i + 1, irop_make_imm32(-1, ubfx_param, IROP_BTYPE_INT32));
    shr_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* Redundant AND elimination: SHR #N (N>=24) + AND #255 → just SHR #N.
   * After shifting right by 24+ bits on a 32-bit value, the result is
   * already 0-255, making AND #255 redundant.  This catches cases the
   * UBFX fusion skips (DEREF sources). */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *shr_q = &ir->compact_instructions[i];
    IRQuadCompact *and_q = &ir->compact_instructions[i + 1];
    if (shr_q->op != TCCIR_OP_SHR || and_q->op != TCCIR_OP_AND)
      continue;
    IROperand shr_src2 = tcc_ir_op_get_src2(ir, shr_q);
    IROperand and_src2 = tcc_ir_op_get_src2(ir, and_q);
    if (!irop_is_immediate(shr_src2) || !irop_is_immediate(and_src2))
      continue;
    int64_t shift = irop_get_imm64_ex(ir, shr_src2);
    int64_t mask = irop_get_imm64_ex(ir, and_src2);
    if (shift < 24 || shift >= 32 || mask != 0xFF)
      continue;
    IROperand shr_dest = tcc_ir_op_get_dest(ir, shr_q);
    IROperand and_src1 = tcc_ir_op_get_src1(ir, and_q);
    if (irop_get_vreg(shr_dest) != irop_get_vreg(and_src1))
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, irop_get_vreg(shr_dest), i))
      continue;
    /* Redirect AND's dest to SHR's dest and NOP the AND */
    IROperand and_dest = tcc_ir_op_get_dest(ir, and_q);
    tcc_ir_set_dest(ir, i, and_dest);
    and_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* AND chain fold: AND #M1 + AND #M2 → AND #(M1 & M2) when first AND
   * result is single-use.  Handles bitfield chains where multiple AND
   * operations clear different bits of the same word.
   * Looks up the def chain: for each AND, if its src1 was defined by
   * another AND with an immediate mask, fold the masks together. */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *and2_q = &ir->compact_instructions[i];
    if (and2_q->op != TCCIR_OP_AND)
      continue;
    IROperand and2_src1 = tcc_ir_op_get_src1(ir, and2_q);
    IROperand and2_src2 = tcc_ir_op_get_src2(ir, and2_q);
    if (and2_src1.is_lval || !irop_is_immediate(and2_src2))
      continue;
    int32_t src1_vr = irop_get_vreg(and2_src1);
    if (src1_vr < 0)
      continue;
    int def_idx = tcc_ir_find_defining_instruction(ir, src1_vr, i);
    if (def_idx < 0 || def_idx >= i)
      continue;
    IRQuadCompact *and1_q = &ir->compact_instructions[def_idx];
    if (and1_q->op != TCCIR_OP_AND)
      continue;
    IROperand and1_src2 = tcc_ir_op_get_src2(ir, and1_q);
    if (!irop_is_immediate(and1_src2))
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, src1_vr, def_idx))
      continue;
    int64_t mask1 = irop_get_imm64_ex(ir, and1_src2);
    int64_t mask2 = irop_get_imm64_ex(ir, and2_src2);
    int64_t combined = mask1 & mask2;
    IROperand and1_src1 = tcc_ir_op_get_src1(ir, and1_q);
    tcc_ir_set_src1(ir, i, and1_src1);
    IROperand new_mask = irop_make_imm32(-1, (int32_t)combined, irop_get_btype(and2_src2));
    tcc_ir_op_set_src2(ir, and2_q, new_mask);
    and1_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* Redundant AND after UBFX: UBFX produces a value already within
   * the extracted range, so a following AND with a superset mask is
   * redundant.  E.g. UBFX #8,#8 (result 0-255) + AND #255 → UBFX. */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *ubfx_q = &ir->compact_instructions[i];
    IRQuadCompact *and_q = &ir->compact_instructions[i + 1];
    if (ubfx_q->op != TCCIR_OP_UBFX || and_q->op != TCCIR_OP_AND)
      continue;
    IROperand ubfx_src2 = tcc_ir_op_get_src2(ir, ubfx_q);
    IROperand and_src2 = tcc_ir_op_get_src2(ir, and_q);
    if (!irop_is_immediate(ubfx_src2) || !irop_is_immediate(and_src2))
      continue;
    int64_t ubfx_param = irop_get_imm64_ex(ir, ubfx_src2);
    int width = (ubfx_param >> 5) & 0x1F;
    if (width <= 0 || width > 31)
      continue;
    uint32_t ubfx_range = (1u << width) - 1;
    int64_t and_mask = irop_get_imm64_ex(ir, and_src2);
    if ((ubfx_range & and_mask) != ubfx_range)
      continue;
    IROperand ubfx_dest = tcc_ir_op_get_dest(ir, ubfx_q);
    IROperand and_src1 = tcc_ir_op_get_src1(ir, and_q);
    if (irop_get_vreg(ubfx_dest) != irop_get_vreg(and_src1))
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, irop_get_vreg(ubfx_dest), i))
      continue;
    IROperand and_dest = tcc_ir_op_get_dest(ir, and_q);
    tcc_ir_set_dest(ir, i, and_dest);
    and_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* Redundant AND elimination after LOAD u8/u16 unsigned:
   * LDRB/LDRH zero-extend on ARM, so the LOAD's result is already in
   * range and a following AND with a superset mask is dead.  The
   * backend selects LDRB/LDRH based on src.btype (which carries the
   * "load size" semantics for pointer-deref LOADs), so we must gate
   * on src.btype, not dest.btype — `T4 <- P1+T3` propagates pointer
   * width (INT32) into T4 even when P1 points to u16 data.
   *
   * Skip NOPs between LOAD and AND — earlier passes may leave compacted
   * gaps that hide otherwise-adjacent pairs from the fold. */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *load_q = &ir->compact_instructions[i];
    if (load_q->op != TCCIR_OP_LOAD)
      continue;
    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      continue;
    IRQuadCompact *and_q = &ir->compact_instructions[j];
    if (and_q->op != TCCIR_OP_AND)
      continue;
    IROperand load_src = tcc_ir_op_get_src1(ir, load_q);
    IROperand load_dest = tcc_ir_op_get_dest(ir, load_q);
    if (!load_src.is_unsigned)
      continue;
    int load_btype = irop_get_btype(load_src);
    uint32_t load_range;
    if (load_btype == IROP_BTYPE_INT8)
      load_range = 0xFFu;
    else if (load_btype == IROP_BTYPE_INT16)
      load_range = 0xFFFFu;
    else
      continue;
    IROperand and_src2 = tcc_ir_op_get_src2(ir, and_q);
    if (!irop_is_immediate(and_src2))
      continue;
    int64_t and_mask = irop_get_imm64_ex(ir, and_src2);
    if (((uint64_t)load_range & (uint64_t)and_mask) != (uint64_t)load_range)
      continue;
    IROperand and_src1 = tcc_ir_op_get_src1(ir, and_q);
    if (irop_get_vreg(load_dest) != irop_get_vreg(and_src1))
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, irop_get_vreg(load_dest), i))
      continue;
    /* Redirect the LOAD to write the AND's dest vreg, but preserve the
     * LOAD's dest btype so downstream passes (LOAD_INDEXED fusion) keep
     * the correct load width.  Copying the AND's operand verbatim would
     * widen the dest btype to INT32, causing fusion to emit LDR instead
     * of LDRB/LDRH. */
    IROperand and_dest = tcc_ir_op_get_dest(ir, and_q);
    IROperand new_dest = load_dest;
    irop_set_vreg(&new_dest, irop_get_vreg(and_dest));
    tcc_ir_set_dest(ir, i, new_dest);
    and_q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* Convert LOAD-no-deref to ASSIGN when the source vreg is already
   * provably in the destination type's range. The LOAD opcode forces
   * a UXTB/UXTH narrowing on REG sources to handle AAPCS-promoted
   * parameters; that narrowing is dead if the source value is already
   * narrow.  ASSIGN does not narrow, so coalescing/peepholing can
   * eliminate the move entirely. */
  {
    /* Find the maximum vreg position per type so we can size arrays. */
    int max_pos_var = 0, max_pos_tmp = 0;
    for (i = 0; i < n; i++)
    {
      IRQuadCompact *q2 = &ir->compact_instructions[i];
      if (q2->op == TCCIR_OP_NOP)
        continue;
      IROperand ops[3] = {tcc_ir_op_get_dest(ir, q2), tcc_ir_op_get_src1(ir, q2), tcc_ir_op_get_src2(ir, q2)};
      for (int k = 0; k < 3; k++)
      {
        int32_t vr = irop_get_vreg(ops[k]);
        if (vr < 0)
          continue;
        int type = TCCIR_DECODE_VREG_TYPE(vr);
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (type == TCCIR_VREG_TYPE_VAR && pos > max_pos_var)
          max_pos_var = pos;
        else if (type == TCCIR_VREG_TYPE_TEMP && pos > max_pos_tmp)
          max_pos_tmp = pos;
      }
    }

    /* narrow[type * stride + pos]:
     *   0 = uninitialised (no def seen yet)
     *   1 = always narrow_u8 so far
     *   2 = always narrow_u16 so far
     *   3 = mixed / not narrow (sticky)  */
    int stride = (max_pos_var > max_pos_tmp ? max_pos_var : max_pos_tmp) + 1;
    if (stride > 0)
    {
      uint8_t *narrow = tcc_mallocz((size_t)stride * 4); /* 4 type slots */

      /* Pass A: classify every vreg by all its definitions. */
      for (i = 0; i < n; i++)
      {
        IRQuadCompact *q2 = &ir->compact_instructions[i];
        if (q2->op == TCCIR_OP_NOP || !irop_config[q2->op].has_dest)
          continue;
        IROperand dest = tcc_ir_op_get_dest(ir, q2);
        int32_t dvr = irop_get_vreg(dest);
        if (dvr < 0)
          continue;
        int type = TCCIR_DECODE_VREG_TYPE(dvr);
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (type != TCCIR_VREG_TYPE_VAR && type != TCCIR_VREG_TYPE_TEMP)
          continue;
        uint8_t *slot = &narrow[type * stride + pos];
        if (*slot == 3)
          continue; /* already mixed */

        /* What does this definition produce? */
        uint8_t produced = 3; /* default: unknown / not narrow */
        if (q2->op == TCCIR_OP_LOAD)
        {
          /* The backend gates LDRB/LDRH on src.btype (see load_from_base
           * call in arm-thumb-gen.c).  Only narrow if src actually carries
           * sub-word type info — pointer LOADs propagate INT32 from the
           * ADD that built the address. */
          IROperand sq = tcc_ir_op_get_src1(ir, q2);
          if (sq.is_unsigned)
          {
            int b = irop_get_btype(sq);
            if (b == IROP_BTYPE_INT8)
              produced = 1;
            else if (b == IROP_BTYPE_INT16)
              produced = 2;
          }
        }
        else if (q2->op == TCCIR_OP_AND)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q2);
          if (irop_is_immediate(s2))
          {
            uint64_t m = (uint64_t)irop_get_imm64_ex(ir, s2);
            if ((m & ~(uint64_t)0xFFu) == 0)
              produced = 1;
            else if ((m & ~(uint64_t)0xFFFFu) == 0)
              produced = 2;
          }
        }
        else if (q2->op == TCCIR_OP_UBFX)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q2);
          if (irop_is_immediate(s2))
          {
            int64_t param = irop_get_imm64_ex(ir, s2);
            int width = (int)((param >> 5) & 0x1F);
            if (width > 0 && width <= 8)
              produced = 1;
            else if (width > 0 && width <= 16)
              produced = 2;
          }
        }
        else if (q2->op == TCCIR_OP_SHR)
        {
          /* Logical shift right by N on a 32-bit value leaves 32-N bits.
           * SHR #24 → result fits in 8 bits, SHR #16 → 16 bits.  Result
           * range is independent of source signedness (SHR zero-fills).
           * Guard against 64-bit src where the shift may not narrow. */
          IROperand sq = tcc_ir_op_get_src1(ir, q2);
          IROperand s2 = tcc_ir_op_get_src2(ir, q2);
          if (irop_is_immediate(s2) &&
              irop_get_btype(sq) != IROP_BTYPE_INT64)
          {
            int64_t shift = irop_get_imm64_ex(ir, s2);
            if (shift >= 24 && shift < 32)
              produced = 1;
            else if (shift >= 16 && shift < 32)
              produced = 2;
          }
        }
        else if (q2->op == TCCIR_OP_ASSIGN)
        {
          /* ASSIGN copies the source value unchanged — narrowness
           * propagates through.  In-order walk means earlier defs may
           * not be classified yet, so this only catches forward chains
           * (def of source appears textually before its use).  That's
           * sufficient for typical *p++ patterns where the LOAD's
           * narrow result feeds later byte-typed copies. */
          IROperand sq = tcc_ir_op_get_src1(ir, q2);
          int32_t svr = irop_get_vreg(sq);
          if (svr >= 0 && !sq.is_lval && !sq.is_local && !sq.is_llocal)
          {
            int stype = TCCIR_DECODE_VREG_TYPE(svr);
            int spos = TCCIR_DECODE_VREG_POSITION(svr);
            if ((stype == TCCIR_VREG_TYPE_VAR || stype == TCCIR_VREG_TYPE_TEMP) && spos < stride)
            {
              uint8_t src_cls = narrow[stype * stride + spos];
              if (src_cls == 1 || src_cls == 2)
                produced = src_cls;
            }
          }
        }

        if (*slot == 0)
          *slot = produced;
        else if (*slot != produced)
          *slot = 3;
      }

      /* Pass B: convert eligible LOAD to ASSIGN. */
      for (i = 0; i < n; i++)
      {
        IRQuadCompact *q2 = &ir->compact_instructions[i];
        if (q2->op != TCCIR_OP_LOAD)
          continue;
        IROperand dest = tcc_ir_op_get_dest(ir, q2);
        IROperand src1 = tcc_ir_op_get_src1(ir, q2);
        if (!dest.is_unsigned)
          continue;
        /* Memory deref iff src1.is_lval && not a register-promoted local/const.
         * Mirrors the backend's preserve_lval logic in machine_op.c. */
        int does_memory_deref = src1.is_lval && !src1.is_const && !src1.is_local && !src1.is_llocal;
        if (does_memory_deref)
          continue;
        int32_t svr = irop_get_vreg(src1);
        if (svr < 0)
          continue;
        int stype = TCCIR_DECODE_VREG_TYPE(svr);
        int spos = TCCIR_DECODE_VREG_POSITION(svr);
        if (stype != TCCIR_VREG_TYPE_VAR && stype != TCCIR_VREG_TYPE_TEMP)
          continue;
        uint8_t src_class = narrow[stype * stride + spos];
        int dbtype = irop_get_btype(dest);
        int need_class;
        if (dbtype == IROP_BTYPE_INT8)
          need_class = 1;
        else if (dbtype == IROP_BTYPE_INT16)
          need_class = 2;
        else
          continue;
        /* src_class==1 (u8) satisfies u16 dest too. */
        if (src_class == 0 || src_class == 3)
          continue;
        if (src_class > need_class)
          continue;
        q2->op = TCCIR_OP_ASSIGN;
        changes++;
      }

      tcc_free(narrow);
    }
  }

  /* Third pass: Fold CMP+SETIF patterns when both CMP operands evaluate
   * to compile-time constants — direct immediates, or vregs whose
   * defining ASSIGN of an immediate is in the same basic block, or
   * STACKOFF lvals whose most recent STORE in the BB wrote an immediate. */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *cmp_q = &ir->compact_instructions[i];
    IRQuadCompact *setif_q = &ir->compact_instructions[i + 1];
    int64_t val1, val2;
    int cond, result;
    int btype;

    if (cmp_q->op != TCCIR_OP_CMP)
      continue;
    if (setif_q->op != TCCIR_OP_SETIF)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, cmp_q);
    IROperand src2 = tcc_ir_op_get_src2(ir, cmp_q);

    uint64_t u1, u2;
    if (!eval_cmp_operand_const(ir, src1, i, &u1))
      continue;
    if (!eval_cmp_operand_const(ir, src2, i, &u2))
      continue;
    val1 = (int64_t)u1;
    val2 = (int64_t)u2;
    IROperand setif_src1 = tcc_ir_op_get_src1(ir, setif_q);
    cond = (int)irop_get_imm64_ex(ir, setif_src1); /* Condition code stored as immediate (TCC token) */

    /* Evaluate the comparison based on TCC token values */
    result = 0;
    switch (cond)
    {
    case 0x94: /* TOK_EQ */
      result = (val1 == val2) ? 1 : 0;
      break;
    case 0x95: /* TOK_NE */
      result = (val1 != val2) ? 1 : 0;
      break;
    case 0x9c: /* TOK_LT */
      result = (val1 < val2) ? 1 : 0;
      break;
    case 0x9d: /* TOK_GE */
      result = (val1 >= val2) ? 1 : 0;
      break;
    case 0x9e: /* TOK_LE */
      result = (val1 <= val2) ? 1 : 0;
      break;
    case 0x9f: /* TOK_GT */
      result = (val1 > val2) ? 1 : 0;
      break;
    case 0x92: /* TOK_ULT (unsigned <) */
      result = ((uint64_t)val1 < (uint64_t)val2) ? 1 : 0;
      break;
    case 0x93: /* TOK_UGE (unsigned >=) */
      result = ((uint64_t)val1 >= (uint64_t)val2) ? 1 : 0;
      break;
    case 0x96: /* TOK_ULE (unsigned <=) */
      result = ((uint64_t)val1 <= (uint64_t)val2) ? 1 : 0;
      break;
    case 0x97: /* TOK_UGT (unsigned >) */
      result = ((uint64_t)val1 > (uint64_t)val2) ? 1 : 0;
      break;
    default:
      /* Unknown condition, don't fold */
      continue;
    }

    LOG_IR_GEN("OPTIMIZE: Fold CMP+SETIF const (%lld cmp %lld, cond=0x%x) = %d at i=%d", (long long)val1,
               (long long)val2, cond, result, i);

    /* Convert CMP to NOP and SETIF to ASSIGN with constant result.
     * Dead store elimination will remove the NOP. */
    cmp_q->op = TCCIR_OP_NOP;
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    setif_q->op = TCCIR_OP_ASSIGN;
    ir->compact_instructions[i + 1].op = TCCIR_OP_ASSIGN;

    btype = irop_get_btype(setif_src1);
    IROperand new_setif_src1 = irop_make_imm32(-1, result, btype);
    tcc_ir_set_src1(ir, i + 1, new_setif_src1);
    tcc_ir_set_src2(ir, i + 1, IROP_NONE);
    changes++;
  }

  /* Fourth pass: eliminate dead STORE/ASSIGN to constant VARs whose values
   * were fully propagated (no remaining vreg references as sources).
   * Only safe when the variable's address is not taken (no aliased reads). */
  if (var_info)
    for (i = 0; i <= max_var_pos; i++)
    {
      if (!var_info[i].is_constant)
        continue;

      int32_t vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, i);
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
      if (!interval || interval->addrtaken || interval->is_complex)
        continue;

      /* Scan all instructions for any remaining use of this VAR as a source */
      int still_used = 0;
      for (int j = 0; j < n; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_NOP)
          continue;

        if (irop_config[jq->op].has_src1)
        {
          int32_t src_vr = irop_get_vreg(tcc_ir_op_get_src1(ir, jq));
          if (src_vr == vr)
          {
            still_used = 1;
            break;
          }
        }
        if (irop_config[jq->op].has_src2)
        {
          int32_t src_vr = irop_get_vreg(tcc_ir_op_get_src2(ir, jq));
          if (src_vr == vr)
          {
            still_used = 1;
            break;
          }
        }
      }

      if (!still_used)
      {
        int di = var_info[i].def_idx;
        if (di >= 0 && di < n && ir->compact_instructions[di].op != TCCIR_OP_NOP)
        {
          LOG_IR_GEN("OPTIMIZE: Dead constant VAR store at i=%d (V%d=#%lld, no remaining uses)", di, i,
                     (long long)var_info[i].value);
          ir->compact_instructions[di].op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }

  tcc_free(dc);
  tcc_free(var_info);

  if (getenv("DUMP_CP_OUT")) { fprintf(stderr, "=== CONST_PROP OUT (changes=%d) ===\n", changes); tcc_ir_show(ir); }
  return changes;
}

/* ============================================================================
 * Phase 2: Value Tracking through Arithmetic
 * ============================================================================
 *
 * Track constant values through arithmetic operations (ADD, SUB) to enable
 * folding of comparisons where a vreg has a known constant value.
 *
 * Example:
 *   V0 <- #1234 [ASSIGN]           ; V0 = 1234
 *   V0 <- V0 SUB #42               ; V0 = 1192 (still constant!)
 *   CMP V0, #1000000               ; 1192 <= 1000000, always true
 *   JMP to X if "<=S"              ; Can fold to unconditional JUMP
 */

/* Track constant values for vregs through arithmetic.
 * Uses generation counters for O(1) bulk invalidation instead of O(max_vreg)
 * loops.  This makes the pass O(n) instead of O(n × max_vreg). */
typedef struct
{
  int gen;       /* entry valid when gen == current_gen */
  int def_gen;   /* def_idx valid when def_gen == current_def_gen */
  int64_t value; /* The constant value */
  int def_idx;   /* instruction index of last constant def (-1 = none/read) */
} VRegConstState;

/* LEA map entry with generation counter */
typedef struct
{
  int gen;     /* valid when gen == current_lea_gen */
  int var_pos; /* VAR position this TMP points to */
} LeaMapGenEntry;

/* Helper: check if state entry is a known constant in current generation */
#define VT_IS_CONST(st, pos) ((st)[pos].gen == vt_gen)
/* Helper: check if def_idx is valid in current def generation */
#define VT_HAS_DEF(st, pos) ((st)[pos].def_gen == vt_def_gen && (st)[pos].def_idx >= 0)
/* Helper: set state as constant */
#define VT_SET_CONST(st, pos, val_)                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].gen = vt_gen;                                                                                            \
    (st)[pos].def_gen = vt_def_gen;                                                                                    \
    (st)[pos].value = (val_);                                                                                          \
    (st)[pos].def_idx = -1;                                                                                            \
  } while (0)
/* Helper: set state as constant with def tracking */
#define VT_SET_CONST_DEF(st, pos, val_, idx_)                                                                          \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].gen = vt_gen;                                                                                            \
    (st)[pos].def_gen = vt_def_gen;                                                                                    \
    (st)[pos].value = (val_);                                                                                          \
    (st)[pos].def_idx = (idx_);                                                                                        \
  } while (0)
/* Helper: invalidate constant state for a position */
#define VT_INVALIDATE(st, pos)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].gen = 0;                                                                                                 \
  } while (0)
/* Helper: invalidate def_idx only (keep constant value) */
#define VT_CLEAR_DEF(st, pos)                                                                                          \
  do                                                                                                                   \
  {                                                                                                                    \
    (st)[pos].def_gen = 0;                                                                                             \
  } while (0)

/* Maximum number of addrtaken vregs to track for fast STORE/CALL invalidation.
 * Beyond this limit, falls back to full scan. */
#define VT_MAX_ADDRTAKEN 64

int tcc_ir_opt_value_tracking(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int max_vreg = 0;
  int max_tmp = 0;

  if (n == 0)
    return 0;

  /* Single pre-scan: build merge-point bitmap AND find max vreg/tmp positions.
   * Merges 3 separate O(n) scans into 1. */
  uint8_t *is_merge = tcc_mallocz((n + 7) / 8);
  int *pred_count = tcc_mallocz(n * sizeof(int));

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Track max vreg/tmp positions while scanning */
    if (q->op != TCCIR_OP_NOP)
    {
      IROperand ops[3];
      ops[0] = tcc_ir_op_get_dest(ir, q);
      ops[1] = tcc_ir_op_get_src1(ir, q);
      ops[2] = tcc_ir_op_get_src2(ir, q);
      for (int k = 0; k < 3; k++)
      {
        int32_t vr = irop_get_vreg(ops[k]);
        if (vr >= 0)
        {
          int type = TCCIR_DECODE_VREG_TYPE(vr);
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (type == TCCIR_VREG_TYPE_VAR && pos > max_vreg)
            max_vreg = pos;
          else if (type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp)
            max_tmp = pos;
        }
      }
    }

    /* Build pred_count and is_merge */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)dest.u.imm32;
      if (target >= 0 && target < n)
      {
        pred_count[target]++;
        /* Back-edge: jump from later instruction to earlier one - always a merge point */
        if (i > target)
          is_merge[target / 8] |= (1 << (target % 8));
      }
    }
    /* SWITCH_TABLE: all case targets are merge points */
    if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
        {
          int t = table->targets[j];
          if (t >= 0 && t < n)
            pred_count[t]++;
        }
        if (table->default_target >= 0 && table->default_target < n)
          pred_count[table->default_target]++;
      }
    }
    /* Fall-through predecessor (SWITCH_TABLE is a terminator — no fall-through) */
    if (i + 1 < n && q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_RETURNVALUE &&
        q->op != TCCIR_OP_RETURNVOID && q->op != TCCIR_OP_SWITCH_TABLE)
    {
      pred_count[i + 1]++;
    }
  }
  /* Mark instructions with multiple predecessors as merge points */
  for (int i = 0; i < n; i++)
  {
    if (pred_count[i] > 1)
      is_merge[i / 8] |= (1 << (i % 8));
  }
  tcc_free(pred_count);

  /* Detect VLA — SHL folding is unsafe in functions with VLA because
   * it can disrupt VLA stack save/restore patterns in nested scopes. */
  int has_vla = 0;
  for (int vi = 0; vi < n && !has_vla; vi++)
  {
    TccIrOp vop = ir->compact_instructions[vi].op;
    if (vop == TCCIR_OP_VLA_ALLOC || vop == TCCIR_OP_VLA_SP_SAVE || vop == TCCIR_OP_VLA_SP_RESTORE)
      has_vla = 1;
  }

  int has_prefetch = 0;
  for (int vi = 0; vi < n && !has_prefetch; vi++)
  {
    if (ir->compact_instructions[vi].op == TCCIR_OP_PREFETCH)
      has_prefetch = 1;
  }

  /* Detect IJUMP — `&&label` targets aren't marked as merge points
   * (the predecessor scan only records JUMP/JUMPIF/SWITCH_TABLE edges),
   * so VAR const-tracking can carry a stale value through what is
   * really a back-edge target.  The new T<-V_const fold below
   * (Pattern 2b') is particularly load-bearing for this — it can
   * promote a stale V to immediate, after which TMP propagation +
   * arithmetic folding cascade into eliminating a real branch.
   * Skip that fold when IJUMP is present. */
  int has_ijump = 0;
  for (int vi = 0; vi < n && !has_ijump; vi++)
  {
    if (ir->compact_instructions[vi].op == TCCIR_OP_IJUMP)
      has_ijump = 1;
  }

  /* Note: do NOT return early when max_vreg == 0.  The loop also
   * constant-folds __aeabi_lcmp/ulcmp calls with immediate args,
   * which doesn't require any tracked VARs. */

  VRegConstState *state = tcc_mallocz(sizeof(VRegConstState) * (max_vreg + 1));

  /* LEA tracking with generation counters */
  LeaMapGenEntry *lea_map = tcc_mallocz(sizeof(LeaMapGenEntry) * (max_tmp + 1));
  LeaMapGenEntry *lea_var_map = tcc_mallocz(sizeof(LeaMapGenEntry) * (max_vreg + 1));

  /* Generation counters — bumping invalidates all entries in O(1) */
  int vt_gen = 1;     /* state[].gen must match for is_constant to be valid */
  int vt_def_gen = 1; /* state[].def_gen must match for def_idx to be valid */
  int vt_lea_gen = 1; /* lea_map[].gen must match for entry to be valid */
  int vt_in_dead_zone = 0;

  /* Track addrtaken constant vregs for fast STORE/CALL invalidation.
   * Instead of scanning all max_vreg entries, we only iterate this small list. */
  int addrtaken_list[VT_MAX_ADDRTAKEN];
  int num_addrtaken = 0;
  int addrtaken_overflow = 0; /* 1 = list full, must fall back to full scan */

  /* Pre-build addrtaken bitmap for quick lookup during constant tracking */
  uint8_t *is_addrtaken = tcc_mallocz((max_vreg + 8) / 8);
  for (int v = 0; v <= max_vreg; v++)
  {
    int32_t vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, v);
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
    if (interval && interval->addrtaken)
      is_addrtaken[v / 8] |= (1 << (v % 8));
  }

  /* Forward pass: track values through the IR */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Clear state at merge points — O(1) via generation bump */
    if (is_merge[i / 8] & (1 << (i % 8)))
    {
      vt_gen++;
      vt_def_gen++;
      vt_lea_gen++;
      num_addrtaken = 0;
      addrtaken_overflow = 0;
    }

    /* After a terminator, the next instruction is NOT a fall-through successor.
     * Clear state — O(1) via generation bump.
     * Exception: after RETURNVALUE/RETURNVOID, if the next instruction is NOT a
     * merge point it has exactly one predecessor (a JUMPIF).  The JUMPIF preserves
     * constant state, so we keep propagating.  A dead unconditional JUMP between
     * RETURNVALUE and the target (emitted but unreachable) is harmless — it
     * doesn't modify any VARs, so we stay in the "post-return" zone until we hit
     * a merge point or the dead code ends. */
    if (i > 0)
    {
      IRQuadCompact *prev = &ir->compact_instructions[i - 1];
      if (prev->op == TCCIR_OP_JUMP || prev->op == TCCIR_OP_RETURNVALUE || prev->op == TCCIR_OP_RETURNVOID ||
          prev->op == TCCIR_OP_SWITCH_TABLE)
      {
        int skip_clear = 0;
        if (prev->op == TCCIR_OP_RETURNVALUE || prev->op == TCCIR_OP_RETURNVOID)
          vt_in_dead_zone = 1;
        if (vt_in_dead_zone && !(is_merge[i / 8] & (1 << (i % 8))))
          skip_clear = 1;
        else
          vt_in_dead_zone = 0;
        /* JMP-over-NOPs to the next real instruction (residue from dead-code
         * elimination of an empty branch): the JMP skips only NOPs, so its
         * effective destination is whichever real instruction follows i.
         * Preserve const-state so subsequent reads see prior LEA+STORE tracking.
         * Without this, a JMP target=i+N (where N..target-1 are NOPs from DCE)
         * clears state at i and blocks __builtin_modf+copysign constant folding
         * into the local (pr48641-style: 1st `if` folds to bl link_error, the
         * resulting JMP-over-dead-code becomes JMP-over-NOPs, and value_tracking
         * loses the LEA-tracked V0 const across the residual JMP).
         *
         * Safety: i must not be a merge point — multi-pred targets need a real
         * state clear since other paths may bring different state.  When
         * jtarget is a merge point, the merge check at line 2206 clears state
         * there anyway, so over-preserving through intermediate NOPs is fine. */
        if (prev->op == TCCIR_OP_JUMP && !skip_clear && !(is_merge[i / 8] & (1 << (i % 8))))
        {
          IROperand jdest = tcc_ir_op_get_dest(ir, prev);
          int jtarget = (int)irop_get_imm64_ex(ir, jdest);
          if (jtarget >= i && jtarget < n)
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
          vt_gen++;
          vt_def_gen++;
          vt_lea_gen++;
          num_addrtaken = 0;
          addrtaken_overflow = 0;
        }
      }
      else
        vt_in_dead_zone = 0;
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* A conditional branch creates an alternative path where current defs
     * may still be live.  Clear def_idx only — O(1) via def generation bump.
     * Constant values remain valid (is_constant preserved). */
    if (q->op == TCCIR_OP_JUMPIF)
    {
      vt_def_gen++;
      continue;
    }

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int32_t dest_vr = irop_get_vreg(dest);
    int dest_pos = (dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
                       ? TCCIR_DECODE_VREG_POSITION(dest_vr)
                       : -1;

    /* LEA tracking: T = &V+offset — record that TMP T points to VAR V at given offset */
    if (q->op == TCCIR_OP_LEA)
    {
      int32_t src1_vr = irop_get_vreg(src1);
      if (dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP && src1_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        int var_pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (tmp_pos <= max_tmp && var_pos <= max_vreg)
        {
          lea_map[tmp_pos].gen = vt_lea_gen;
          lea_map[tmp_pos].var_pos = var_pos;
          LOG_IR_GEN("VALUE_TRACK LEA: i=%d T%d -> V%d", i, tmp_pos, var_pos);
        }
      }
      LOG_IR_GEN("VALUE_TRACK LEA SKIP: i=%d dest_vr=0x%x dest_type=%d src1_vr=0x%x src1_type=%d", i, dest_vr,
                 dest_vr >= 0 ? TCCIR_DECODE_VREG_TYPE(dest_vr) : -1, irop_get_vreg(src1),
                 irop_get_vreg(src1) >= 0 ? TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src1)) : -1);
      continue;
    }

    /* STORE through LEA: *T = value — if T = &V, propagate value to V. */
    if (q->op == TCCIR_OP_STORE)
    {
      int32_t addr_vr = irop_get_vreg(dest);
      if (addr_vr >= 0 && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(addr_vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos].gen == vt_lea_gen)
        {
          int var_pos = lea_map[tmp_pos].var_pos;
          if (var_pos <= max_vreg)
          {
            if (irop_is_immediate(src1))
            {
              VT_SET_CONST(state, var_pos, irop_get_imm64_ex(ir, src1));
              /* Track addrtaken for fast invalidation */
              if (is_addrtaken[var_pos / 8] & (1 << (var_pos % 8)))
              {
                if (!addrtaken_overflow && num_addrtaken < VT_MAX_ADDRTAKEN)
                  addrtaken_list[num_addrtaken++] = var_pos;
                else
                  addrtaken_overflow = 1;
              }
              LOG_IR_GEN("VALUE_TRACK STORE: i=%d V%d = %lld (via T%d)", i, var_pos, (long long)state[var_pos].value,
                         tmp_pos);
            }
            else
            {
              /* Non-constant store → invalidate tracked value */
              VT_INVALIDATE(state, var_pos);
            }
          }
        }
      }
      /* Direct VAR store: V = T — propagate LEA if src is a LEA result */
      else if (dest_pos >= 0)
      {
        int lea_propagated = 0;
        int32_t src_vr = irop_get_vreg(src1);
        if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int src_tmp = TCCIR_DECODE_VREG_POSITION(src_vr);
          if (src_tmp <= max_tmp && lea_map[src_tmp].gen == vt_lea_gen)
          {
            lea_var_map[dest_pos].gen = vt_lea_gen;
            lea_var_map[dest_pos].var_pos = lea_map[src_tmp].var_pos;
            lea_propagated = 1;
            LOG_IR_GEN("VALUE_TRACK LEA-VAR: i=%d V%d -> V%d (via T%d)", i, dest_pos, lea_map[src_tmp].var_pos,
                       src_tmp);
          }
        }
        if (!lea_propagated && dest_pos <= max_vreg)
        {
          lea_var_map[dest_pos].gen = 0;
          if (has_prefetch)
          {
            VT_INVALIDATE(state, dest_pos);
          }
          else if (irop_is_immediate(src1))
          {
            if (is_addrtaken[dest_pos / 8] & (1 << (dest_pos % 8)))
            {
              VT_INVALIDATE(state, dest_pos);
            }
            else
            {
              if (VT_IS_CONST(state, dest_pos) && VT_HAS_DEF(state, dest_pos))
              {
                ir->compact_instructions[state[dest_pos].def_idx].op = TCCIR_OP_NOP;
                changes++;
              }
              VT_SET_CONST_DEF(state, dest_pos, irop_get_imm64_ex(ir, src1), i);
              LOG_IR_GEN("VALUE_TRACK DIRECT STORE: i=%d V%d = %lld", i, dest_pos, (long long)state[dest_pos].value);
            }
          }
          else
          {
            VT_INVALIDATE(state, dest_pos);
          }
        }
        /* src1 VAR is read here — mark its def as consumed so the
         * dead-def elimination won't kill the defining instruction. */
        if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_VAR)
        {
          int src_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
          if (src_pos >= 0 && src_pos <= max_vreg)
            VT_CLEAR_DEF(state, src_pos);
        }
      }
      /* Any STORE through an unknown pointer could alias any address-taken var.
       * Iterate only the tracked addrtaken list — O(k) instead of O(max_vreg). */
      else
      {
        if (!addrtaken_overflow)
        {
          for (int a = 0; a < num_addrtaken; a++)
          {
            int v = addrtaken_list[a];
            if (VT_IS_CONST(state, v))
              VT_INVALIDATE(state, v);
          }
        }
        else
        {
          /* Overflow fallback: scan all vregs (rare) */
          for (int v = 0; v <= max_vreg; v++)
          {
            if (VT_IS_CONST(state, v) && (is_addrtaken[v / 8] & (1 << (v % 8))))
              VT_INVALIDATE(state, v);
          }
        }
      }
      continue;
    }

    /* LEA propagation through VAR: T = V where V holds a LEA result */
    if (q->op == TCCIR_OP_ASSIGN && dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int32_t src_vr = irop_get_vreg(src1);
      if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int src_var = TCCIR_DECODE_VREG_POSITION(src_vr);
        if (src_var <= max_vreg && lea_var_map[src_var].gen == vt_lea_gen)
        {
          int dest_tmp = TCCIR_DECODE_VREG_POSITION(dest_vr);
          if (dest_tmp <= max_tmp)
          {
            lea_map[dest_tmp].gen = vt_lea_gen;
            lea_map[dest_tmp].var_pos = lea_var_map[src_var].var_pos;
            LOG_IR_GEN("VALUE_TRACK LEA-TMP: i=%d T%d -> V%d (via V%d)", i, dest_tmp, lea_var_map[src_var].var_pos,
                       src_var);
          }
        }
      }
    }

    /* Pattern 1: Direct constant assignment: Vx <- #const */
    if (q->op == TCCIR_OP_ASSIGN && irop_is_immediate(src1))
    {
      if (dest_pos >= 0 && dest_pos <= max_vreg)
      {
        /* If the address of this variable is taken, it can be modified
         * through aliases.  Do not track it as constant. */
        if (is_addrtaken[dest_pos / 8] & (1 << (dest_pos % 8)))
        {
          VT_INVALIDATE(state, dest_pos);
        }
        else
        {
          /* Previous unread constant def is dead — NOP it */
          if (VT_IS_CONST(state, dest_pos) && VT_HAS_DEF(state, dest_pos))
          {
            ir->compact_instructions[state[dest_pos].def_idx].op = TCCIR_OP_NOP;
            changes++;
          }
          VT_SET_CONST_DEF(state, dest_pos, irop_get_imm64_ex(ir, src1), i);
        }
      }
      continue;
    }

    /* Pattern 2: Arithmetic/bitwise with constant operand: Vx <- Vy op #const
     * SHL/SHR/SAR/MUL included: merge-point invalidation at loop headers
     * prevents constant folding of live IVs inside loops, so straight-line
     * folds are safe. */
    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_AND ||
         q->op == TCCIR_OP_OR || (!has_vla && q->op == TCCIR_OP_SHL) || q->op == TCCIR_OP_SHR ||
         q->op == TCCIR_OP_SAR || q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_MLA) &&
        irop_is_immediate(src2))
    {
      int32_t src1_vr = irop_get_vreg(src1);
      int src1_pos = (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
                         ? TCCIR_DECODE_VREG_POSITION(src1_vr)
                         : -1;
      IROperand accum = (q->op == TCCIR_OP_MLA) ? tcc_ir_op_get_accum(ir, q) : IROP_NONE;

      /* Check if src1 is a known constant AND src2 is immediate */
      if (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos))
      {
        int64_t val1 = state[src1_pos].value;
        int64_t val2 = irop_get_imm64_ex(ir, src2);
        int btype = (q->op == TCCIR_OP_MLA) ? irop_get_btype(dest) : irop_get_btype(src1);
        int is_64 = (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64);
        int64_t result;
        int fold_ok = 1;
        int shift_mask = is_64 ? 63 : 31;
        switch (q->op)
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
        case TCCIR_OP_MLA:
        {
          int64_t acc_val = 0;
          int acc_ok = 0;
          int32_t acc_vr = irop_get_vreg(accum);
          int acc_pos = (acc_vr >= 0 && TCCIR_DECODE_VREG_TYPE(acc_vr) == TCCIR_VREG_TYPE_VAR)
                            ? TCCIR_DECODE_VREG_POSITION(acc_vr)
                            : -1;
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
          {
            fold_ok = 0;
            break;
          }
          if (dest.is_unsigned)
            result = (int64_t)((uint64_t)(uint32_t)val1 * (uint64_t)(uint32_t)val2 + (uint64_t)acc_val);
          else
            result = (int64_t)((int64_t)(int32_t)val1 * (int64_t)(int32_t)val2 + acc_val);
          is_64 = 1;
          btype = IROP_BTYPE_INT64;
          break;
        }
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
        if (!fold_ok)
        {
          if (src1_pos >= 0 && src1_pos <= max_vreg)
            VT_CLEAR_DEF(state, src1_pos);
          if (dest_pos >= 0 && dest_pos <= max_vreg)
            VT_INVALIDATE(state, dest_pos);
          continue;
        }
        if (!is_64 && q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_SAR)
          result = (int64_t)(int32_t)(uint32_t)result;

        LOG_IR_GEN("OPTIMIZE: Constant fold %s(%lld, %lld) = %lld at i=%d", tcc_ir_get_op_name(q->op), (long long)val1,
                   (long long)val2, (long long)result, i);

        /* Fold: replace op with constant ASSIGN */
        q->op = TCCIR_OP_ASSIGN;
        if (result == (int32_t)result)
          tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, btype));
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
          tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
        }
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;

        if (dest_pos >= 0 && dest_pos <= max_vreg)
        {
          /* Do not propagate constant through address-taken variables */
          if (is_addrtaken[dest_pos / 8] & (1 << (dest_pos % 8)))
          {
            VT_INVALIDATE(state, dest_pos);
          }
          else
          {
            /* Previous unread constant def is dead — NOP it */
            if (VT_IS_CONST(state, dest_pos) && VT_HAS_DEF(state, dest_pos))
            {
              ir->compact_instructions[state[dest_pos].def_idx].op = TCCIR_OP_NOP;
              changes++;
            }
            VT_SET_CONST_DEF(state, dest_pos, result, i);
          }
        }
      }
      else
      {
        /* src1 is read but not folded — mark its def as live */
        if (src1_pos >= 0 && src1_pos <= max_vreg)
          VT_CLEAR_DEF(state, src1_pos);
        /* Destination no longer has known constant value */
        if (dest_pos >= 0 && dest_pos <= max_vreg)
          VT_INVALIDATE(state, dest_pos);
      }
      continue;
    }

    /* Pattern 2a: Arithmetic where src2 is a known-constant VAR.
     * Handles `T ADD V0` where V0 is tracked as constant — substitute src2
     * with the immediate value.  If src1 is also immediate, fold entirely. */
    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_AND ||
         q->op == TCCIR_OP_OR || (!has_vla && q->op == TCCIR_OP_SHL) || q->op == TCCIR_OP_SHR ||
         q->op == TCCIR_OP_SAR || q->op == TCCIR_OP_MUL) &&
        !irop_is_immediate(src2))
    {
      int32_t src2_vr = irop_get_vreg(src2);
      int src2_pos = (src2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR)
                         ? TCCIR_DECODE_VREG_POSITION(src2_vr)
                         : -1;

      if (src2_pos >= 0 && src2_pos <= max_vreg && VT_IS_CONST(state, src2_pos))
      {
        int64_t val2 = state[src2_pos].value;
        int btype = irop_get_btype(src2);
        int is_64 = (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64);

        /* Check if src1 is also a known constant (immediate or tracked VAR) */
        int src1_const = 0;
        int64_t val1 = 0;
        if (irop_is_immediate(src1))
        {
          src1_const = 1;
          val1 = irop_get_imm64_ex(ir, src1);
        }
        else
        {
          int32_t src1_vr2 = irop_get_vreg(src1);
          int s1_pos = (src1_vr2 >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr2) == TCCIR_VREG_TYPE_VAR)
                           ? TCCIR_DECODE_VREG_POSITION(src1_vr2)
                           : -1;
          if (s1_pos >= 0 && s1_pos <= max_vreg && VT_IS_CONST(state, s1_pos))
          {
            src1_const = 1;
            val1 = state[s1_pos].value;
          }
        }

        if (src1_const)
        {
          /* Both operands are constants — fold entirely */
          int64_t result;
          int shift_mask = is_64 ? 63 : 31;
          switch (q->op)
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
          if (!is_64 && q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_SAR)
            result = (int64_t)(int32_t)(uint32_t)result;

          LOG_IR_GEN("VALUE_TRACK 2a FOLD: i=%d %s(%lld, %lld) = %lld", i, tcc_ir_get_op_name(q->op), (long long)val1,
                     (long long)val2, (long long)result);

          q->op = TCCIR_OP_ASSIGN;
          if (result == (int32_t)result)
            tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, btype));
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
            tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
          }
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;

          if (dest_pos >= 0 && dest_pos <= max_vreg)
          {
            if (is_addrtaken[dest_pos / 8] & (1 << (dest_pos % 8)))
              VT_INVALIDATE(state, dest_pos);
            else
            {
              if (VT_IS_CONST(state, dest_pos) && VT_HAS_DEF(state, dest_pos))
              {
                ir->compact_instructions[state[dest_pos].def_idx].op = TCCIR_OP_NOP;
                changes++;
              }
              VT_SET_CONST_DEF(state, dest_pos, result, i);
            }
          }
        }
        else
        {
          /* Only src2 is constant — substitute it with immediate */
          LOG_IR_GEN("VALUE_TRACK 2a SUBST: i=%d src2 V%d -> #%lld", i, src2_pos, (long long)val2);
          if (val2 == (int32_t)val2)
            tcc_ir_set_src2(ir, i, irop_make_imm32(-1, (int32_t)val2, btype));
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val2);
            tcc_ir_set_src2(ir, i, irop_make_i64(-1, pool_idx, btype));
          }
          changes++;

          if (dest_pos >= 0 && dest_pos <= max_vreg)
            VT_INVALIDATE(state, dest_pos);
        }
        /* Mark src2 def as consumed */
        VT_CLEAR_DEF(state, src2_pos);
        continue;
      }
    }

    /* Pattern 2b: LOAD of known-constant VAR → ASSIGN #const.
     * Propagates constants tracked through LEA+STORE into TMPs. */
    if (q->op == TCCIR_OP_LOAD && !dest.is_lval)
    {
      int32_t src1_vr = irop_get_vreg(src1);
      if (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int src1_pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos))
        {
          int64_t val = state[src1_pos].value;
          int btype = irop_get_btype(src1);
          q->op = TCCIR_OP_ASSIGN;
          if (val == (int32_t)val)
            tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)val, btype));
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
            tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
          }
          tcc_ir_set_src2(ir, i, IROP_NONE);
          LOG_IR_GEN("VALUE_TRACK LOAD-FOLD: i=%d V%d -> #%lld", i, src1_pos, (long long)val);
          changes++;
        }
      }
    }

    /* Pattern 2b': T <- V where V is tracked constant — fold src to imm.
     * Covers two ops:
     *   - ASSIGN T <- V  (plain copy of a VAR's value)
     *   - CVT_FTOF T <- V when src/dst have the same float btype (e.g.
     *     long double → double on ARM where both are FLOAT64).  This is the
     *     IR that long-double FUNCPARAM marshaling produces; without the
     *     fold, the cdcmple+SETIF chain downstream never sees both args as
     *     immediates even when V is constant-tracked via a prior LEA+STORE
     *     (e.g. a folded __builtin_modfl). */
    if (!has_ijump &&
        (q->op == TCCIR_OP_ASSIGN ||
         (q->op == TCCIR_OP_CVT_FTOF && irop_get_btype(src1) == irop_get_btype(dest))) &&
        dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP &&
        !dest.is_lval)
    {
      int32_t src1_vr = irop_get_vreg(src1);
      if (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int src1_pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos))
        {
          int64_t val = state[src1_pos].value;
          int btype = irop_get_btype(src1);
          q->op = TCCIR_OP_ASSIGN;
          if (val == (int32_t)val)
            tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)val, btype));
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
            tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
          }
          tcc_ir_set_src2(ir, i, IROP_NONE);
          LOG_IR_GEN("VALUE_TRACK ASSIGN-FOLD: i=%d T<-V%d -> T<-#%lld", i, src1_pos, (long long)val);
          changes++;
        }
      }
    }

    /* Pattern 2c: FUNCPARAMVAL with known-constant VAR src → replace with immediate.
     * When modf/copysign folding stores a compile-time constant to a local
     * via LEA+STORE, the subsequent FUNCPARAMVAL that passes that local by
     * value can substitute the tracked constant directly. */
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      int32_t src1_vr = irop_get_vreg(src1);
      if (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int src1_pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos))
        {
          int64_t val = state[src1_pos].value;
          int btype = irop_get_btype(src1);
          if (val == (int32_t)val)
            tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)val, btype));
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
            tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
          }
          LOG_IR_GEN("VALUE_TRACK PARAM-FOLD: i=%d V%d -> #%lld", i, src1_pos, (long long)val);
          changes++;
        }
      }
    }

    /* Pattern 3: CMP with constant vreg - FOLD IT */
    if (q->op == TCCIR_OP_CMP && i + 1 < n)
    {
      IRQuadCompact *jump_q = &ir->compact_instructions[i + 1];
      if (jump_q->op == TCCIR_OP_JUMPIF)
      {
        int32_t src1_vr = irop_get_vreg(src1);
        int src1_pos = (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
                           ? TCCIR_DECODE_VREG_POSITION(src1_vr)
                           : -1;

        /* Check if src1 is known constant AND src2 is immediate */
        int src1_const = (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos));
        int src2_const = irop_is_immediate(src2);

        if (src1_const && src2_const)
        {
          int64_t val1 = state[src1_pos].value;
          int64_t val2 = irop_get_imm64_ex(ir, src2);

          IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
          int tok = (int)irop_get_imm64_ex(ir, cond);

          int result = evaluate_compare_condition(val1, val2, tok);

          if (result >= 0)
          {
            IROperand jmp_dest = tcc_ir_op_get_dest(ir, jump_q);

            if (result)
            {
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_JUMP;
              tcc_ir_set_dest(ir, i + 1, jmp_dest);
              LOG_IR_GEN("VALUE_TRACK: CMP vreg=%lld,#%lld -> always taken, JUMP to %d", (long long)val1,
                         (long long)val2, (int)jmp_dest.u.imm32);
            }
            else
            {
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_NOP;
              LOG_IR_GEN("VALUE_TRACK: CMP vreg=%lld,#%lld -> never taken, eliminated", (long long)val1,
                         (long long)val2);
            }
            changes++;
          }
        }
      }
      else if (jump_q->op == TCCIR_OP_SETIF)
      {
        int32_t src1_vr = irop_get_vreg(src1);
        int src1_pos = (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
                           ? TCCIR_DECODE_VREG_POSITION(src1_vr)
                           : -1;

        int src1_const = (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos));
        int src2_const = irop_is_immediate(src2);

        if (src1_const && src2_const)
        {
          int64_t val1 = state[src1_pos].value;
          int64_t val2 = irop_get_imm64_ex(ir, src2);

          IROperand setif_src1 = tcc_ir_op_get_src1(ir, jump_q);
          int cond = (int)irop_get_imm64_ex(ir, setif_src1);
          int result = evaluate_compare_condition(val1, val2, cond);

          if (result >= 0)
          {
            int btype = irop_get_btype(setif_src1);
            q->op = TCCIR_OP_NOP;
            jump_q->op = TCCIR_OP_ASSIGN;
            tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, btype));
            tcc_ir_set_src2(ir, i + 1, IROP_NONE);
            LOG_IR_GEN("VALUE_TRACK: CMP+SETIF vreg=%lld,#%lld cond=0x%x -> %d at i=%d", (long long)val1,
                       (long long)val2, cond, result, i);
            changes++;
          }
        }
      }
      /* CMP reads src1 — mark its def as live */
      {
        int32_t s1_vr = irop_get_vreg(src1);
        if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_VAR)
        {
          int s1_pos = TCCIR_DECODE_VREG_POSITION(s1_vr);
          if (s1_pos >= 0 && s1_pos <= max_vreg)
            VT_CLEAR_DEF(state, s1_pos);
        }
      }
      continue;
    }

    /* Mark source operand reads — preserve their defining instructions */
    {
      int32_t s1_vr = irop_get_vreg(src1);
      if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int s1_pos = TCCIR_DECODE_VREG_POSITION(s1_vr);
        if (s1_pos >= 0 && s1_pos <= max_vreg)
          VT_CLEAR_DEF(state, s1_pos);
      }
      int32_t s2_vr = irop_get_vreg(src2);
      if (s2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s2_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int s2_pos = TCCIR_DECODE_VREG_POSITION(s2_vr);
        if (s2_pos >= 0 && s2_pos <= max_vreg)
          VT_CLEAR_DEF(state, s2_pos);
      }
    }

    /* Constant-fold __aeabi_lcmp/__aeabi_ulcmp calls when both arguments are
     * known constants (tracked through LEA+STORE). */
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (callee)
      {
        const char *fname = get_tok_str(callee->v, NULL);
        LOG_IR_GEN("VALUE_TRACK CALL: i=%d fname=%s", i, fname ? fname : "(null)");
        int is_lcmp = (fname && strcmp(fname, "__aeabi_lcmp") == 0);
        int is_ulcmp = (fname && strcmp(fname, "__aeabi_ulcmp") == 0);
        if (is_lcmp || is_ulcmp)
        {
          IROperand arg0, arg1;
          if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
          {
            int arg0_known = 0, arg1_known = 0;
            int64_t val0 = 0, val1 = 0;
            uint64_t uval;

            if (irop_is_immediate(arg0))
            {
              val0 = irop_get_imm64_ex(ir, arg0);
              arg0_known = 1;
            }
            else
            {
              int32_t vr0 = irop_get_vreg(arg0);
              if (vr0 >= 0 && TCCIR_DECODE_VREG_TYPE(vr0) == TCCIR_VREG_TYPE_VAR)
              {
                int pos0 = TCCIR_DECODE_VREG_POSITION(vr0);
                if (pos0 >= 0 && pos0 <= max_vreg && VT_IS_CONST(state, pos0))
                {
                  val0 = state[pos0].value;
                  arg0_known = 1;
                }
              }
              if (!arg0_known && ir_opt_eval_const_u64(ir, arg0, i, &uval, 0))
              {
                val0 = (int64_t)uval;
                arg0_known = 1;
              }
            }

            if (irop_is_immediate(arg1))
            {
              val1 = irop_get_imm64_ex(ir, arg1);
              arg1_known = 1;
            }
            else
            {
              int32_t vr1 = irop_get_vreg(arg1);
              if (vr1 >= 0 && TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_VAR)
              {
                int pos1 = TCCIR_DECODE_VREG_POSITION(vr1);
                if (pos1 >= 0 && pos1 <= max_vreg && VT_IS_CONST(state, pos1))
                {
                  val1 = state[pos1].value;
                  arg1_known = 1;
                }
              }
              if (!arg1_known && ir_opt_eval_const_u64(ir, arg1, i, &uval, 0))
              {
                val1 = (int64_t)uval;
                arg1_known = 1;
              }
            }

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

              IROperand call_dest = tcc_ir_op_get_dest(ir, q);
              ir_opt_nop_call_params(ir, i);
              q->op = TCCIR_OP_ASSIGN;
              tcc_ir_set_dest(ir, i, call_dest);
              tcc_ir_set_src1(ir, i, irop_make_imm32(-1, result, IROP_BTYPE_INT32));
              tcc_ir_set_src2(ir, i, IROP_NONE);
              LOG_IR_GEN("VALUE_TRACK: %s(%lld, %lld) = %d at i=%d -> folded", fname, (long long)val0, (long long)val1,
                         result, i);
              changes++;
              continue; /* Skip call invalidation — call was eliminated */
            }

            /* Same-vreg fold: lcmp(x, x) == 0 regardless of the value.
             * Catches cases where global LOAD CSE or copy propagation
             * made both arguments refer to the same virtual register.
             * Traces through ASSIGN chains (T5←T4←T0) to find the root. */
            {
              int32_t vr0 = irop_get_vreg(arg0);
              int32_t vr1 = irop_get_vreg(arg1);
              /* Resolve copy chains: follow ASSIGN and single-def STORE
               * to find root vreg.  Covers patterns like:
               *   T4 <-- T0 [ASSIGN]  (from SL_FWD)
               *   V3 <-- T5 [STORE]   (inlined parameter)
               *   T5 <-- T4 [ASSIGN]  (from SL_FWD) */
              for (int depth = 0; depth < 8 && vr0 >= 0; depth++)
              {
                int def = tcc_ir_find_defining_instruction(ir, vr0, i);
                if (def < 0)
                  break;
                IRQuadCompact *dq = &ir->compact_instructions[def];
                if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_STORE)
                  break;
                IROperand dsrc = tcc_ir_op_get_src1(ir, dq);
                int32_t svr = irop_get_vreg(dsrc);
                if (svr < 0 || dsrc.is_lval)
                  break;
                vr0 = svr;
              }
              for (int depth = 0; depth < 8 && vr1 >= 0; depth++)
              {
                int def = tcc_ir_find_defining_instruction(ir, vr1, i);
                if (def < 0)
                  break;
                IRQuadCompact *dq = &ir->compact_instructions[def];
                if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_STORE)
                  break;
                IROperand dsrc = tcc_ir_op_get_src1(ir, dq);
                int32_t svr = irop_get_vreg(dsrc);
                if (svr < 0 || dsrc.is_lval)
                  break;
                vr1 = svr;
              }
              LOG_IR_GEN("VALUE_TRACK: %s resolved at i=%d: vr0=%d vr1=%d (orig %d %d)", fname, i, vr0, vr1,
                         irop_get_vreg(arg0), irop_get_vreg(arg1));
              if (vr0 >= 0 && vr0 == vr1 && !arg0.is_lval && !arg1.is_lval)
              {
                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                tcc_ir_set_src1(ir, i, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
                tcc_ir_set_src2(ir, i, IROP_NONE);
                LOG_IR_GEN("VALUE_TRACK: %s(vreg%d, vreg%d) = 0 at i=%d -> same-vreg fold", fname, vr0, vr1, i);
                changes++;
                continue;
              }
            }
          }
        }
        /* Constant-fold __aeabi_ldivmod/__aeabi_uldivmod with constant args. */
        {
          int is_ldivmod = (fname && strcmp(fname, "__aeabi_ldivmod") == 0);
          int is_uldivmod = (fname && strcmp(fname, "__aeabi_uldivmod") == 0);
          if (is_ldivmod || is_uldivmod)
          {
            IROperand arg0, arg1;
            if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
            {
              int arg0_known = irop_is_immediate(arg0);
              int arg1_known = irop_is_immediate(arg1);
              int64_t val0 = arg0_known ? irop_get_imm64_ex(ir, arg0) : 0;
              int64_t val1 = arg1_known ? irop_get_imm64_ex(ir, arg1) : 0;

              if (arg0_known && arg1_known && val1 != 0)
              {
                int64_t result;
                if (is_uldivmod)
                  result = (int64_t)((uint64_t)val0 / (uint64_t)val1);
                else
                  result = val0 / val1;

                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                if (result == (int32_t)result)
                  tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, IROP_BTYPE_INT64));
                else
                {
                  uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
                  tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, IROP_BTYPE_INT64));
                }
                tcc_ir_set_src2(ir, i, IROP_NONE);
                LOG_IR_GEN("VALUE_TRACK: %s(%lld, %lld) = %lld at i=%d -> folded", fname, (long long)val0,
                           (long long)val1, (long long)result, i);
                changes++;
                continue;
              }
            }
          }
        }
        /* Constant-fold __aeabi_cfcmple/cdcmple with known constant args (FUNCCALLVAL variant). */
        {
          int is_fcmp = (fname && (strcmp(fname, "__aeabi_cfcmple") == 0 || strcmp(fname, "__aeabi_cfcmpeq") == 0));
          int is_dcmp = (fname && (strcmp(fname, "__aeabi_cdcmple") == 0 || strcmp(fname, "__aeabi_cdcmpeq") == 0));
          if (is_fcmp || is_dcmp)
          {
            IROperand arg0, arg1;
            if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
            {
              int64_t a0 = 0, a1 = 0;
              int a0_ok = irop_is_immediate(arg0), a1_ok = irop_is_immediate(arg1);
              if (a0_ok) a0 = irop_get_imm64_ex(ir, arg0);
              if (a1_ok) a1 = irop_get_imm64_ex(ir, arg1);
              if (!a0_ok)
              {
                int32_t vr = irop_get_vreg(arg0);
                if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
                {
                  int p = TCCIR_DECODE_VREG_POSITION(vr);
                  if (p >= 0 && p <= max_vreg && VT_IS_CONST(state, p))
                  { a0 = state[p].value; a0_ok = 1; }
                }
              }
              if (!a1_ok)
              {
                int32_t vr = irop_get_vreg(arg1);
                if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
                {
                  int p = TCCIR_DECODE_VREG_POSITION(vr);
                  if (p >= 0 && p <= max_vreg && VT_IS_CONST(state, p))
                  { a1 = state[p].value; a1_ok = 1; }
                }
              }
              if (a0_ok && a1_ok)
              {
                int result, is_nan;
                if (is_fcmp)
                {
                  union { float f; uint32_t u; } fa, fb;
                  fa.u = (uint32_t)a0;
                  fb.u = (uint32_t)a1;
                  is_nan = (fa.f != fa.f) || (fb.f != fb.f);
                  result = (fa.f > fb.f) - (fa.f < fb.f);
                }
                else
                {
                  union { double d; uint64_t u; } da, db;
                  da.u = (uint64_t)a0;
                  db.u = (uint64_t)a1;
                  is_nan = (da.d != da.d) || (db.d != db.d);
                  result = (da.d > db.d) - (da.d < db.d);
                }
                /* NaN involved → IEEE unordered.  The (>)-(<) collapse
                 * loses that signal (returns 0 same as "equal"), so
                 * downstream uses would mis-fold.  Leave runtime call. */
                if (is_nan)
                  goto skip_fcmp_val_fold;
                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                tcc_ir_set_src1(ir, i, irop_make_imm32(-1, result, IROP_BTYPE_INT32));
                tcc_ir_set_src2(ir, i, IROP_NONE);
                LOG_IR_GEN("VALUE_TRACK: %s -> %d at i=%d (float cmp fold)", fname, result, i);
                changes++;
                continue;
                skip_fcmp_val_fold:;
              }
            }
          }
        }
        /* Constant-fold __bswapsi2/__bswapdi3 calls with known constant arg. */
        {
          int is_bswap32 = (fname && strcmp(fname, "__bswapsi2") == 0);
          int is_bswap64 = (fname && strcmp(fname, "__bswapdi3") == 0);
          if (is_bswap32 || is_bswap64)
          {
            IROperand arg0;
            if (ir_opt_get_call_param_operand(ir, i, 0, &arg0))
            {
              int arg0_known = 0;
              int64_t val0 = 0;
              if (irop_is_immediate(arg0))
              {
                val0 = irop_get_imm64_ex(ir, arg0);
                arg0_known = 1;
              }
              else
              {
                int32_t vr0 = irop_get_vreg(arg0);
                if (vr0 >= 0 && TCCIR_DECODE_VREG_TYPE(vr0) == TCCIR_VREG_TYPE_VAR)
                {
                  int pos0 = TCCIR_DECODE_VREG_POSITION(vr0);
                  if (pos0 >= 0 && pos0 <= max_vreg && VT_IS_CONST(state, pos0))
                  {
                    val0 = state[pos0].value;
                    arg0_known = 1;
                  }
                }
              }
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
                  result = (int64_t)(((x >> 56) & 0xFFULL) | ((x >> 40) & 0xFF00ULL) | ((x >> 24) & 0xFF0000ULL) |
                                     ((x >> 8) & 0xFF000000ULL) | ((x << 8) & 0xFF00000000ULL) |
                                     ((x << 24) & 0xFF0000000000ULL) | ((x << 40) & 0xFF000000000000ULL) |
                                     ((x << 56) & 0xFF00000000000000ULL));
                }
                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                if (result == (int32_t)result)
                  tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, IROP_BTYPE_INT32));
                else
                {
                  uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
                  tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, IROP_BTYPE_INT64));
                }
                tcc_ir_set_src2(ir, i, IROP_NONE);
                {
                  int32_t dv = irop_get_vreg(call_dest);
                  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
                  {
                    int dp = TCCIR_DECODE_VREG_POSITION(dv);
                    if (dp >= 0 && dp <= max_vreg)
                      VT_SET_CONST(state, dp, result);
                  }
                }
                LOG_IR_GEN("VALUE_TRACK: %s(%lld) = %lld at i=%d -> folded", fname, (long long)val0, (long long)result,
                           i);
                changes++;
                continue;
              }
            }
          }
        }
        /* Constant-fold libm classification calls (isinff/isinf/isnanf/isnan
         * and their newlib internal __ variants, plus finitef/__finite) when
         * the FP argument is a known constant (passed as the IEEE 754 bit
         * pattern in an integer register under soft-float). Lets inline
         * expansions that thread compile-time constants through parameter
         * locals get folded down to integer constants here in the IR pass. */
        {
          int is_isinff = (fname && (strcmp(fname, "isinff") == 0 || strcmp(fname, "__isinff") == 0));
          int is_isinfd = (fname && (strcmp(fname, "isinf") == 0 || strcmp(fname, "__isinfd") == 0 ||
                                     strcmp(fname, "__isinf") == 0));
          int is_isnanf = (fname && (strcmp(fname, "isnanf") == 0 || strcmp(fname, "__isnanf") == 0));
          int is_isnand = (fname && (strcmp(fname, "isnan") == 0 || strcmp(fname, "__isnand") == 0 ||
                                     strcmp(fname, "__isnan") == 0));
          int is_finitef = (fname && (strcmp(fname, "finitef") == 0 || strcmp(fname, "__finitef") == 0));
          int is_finited = (fname && (strcmp(fname, "finite") == 0 || strcmp(fname, "__finite") == 0));
          int is_fp32 = is_isinff || is_isnanf || is_finitef;
          int is_fp64 = is_isinfd || is_isnand || is_finited;
          if (is_fp32 || is_fp64)
          {
            IROperand arg0;
            if (ir_opt_get_call_param_operand(ir, i, 0, &arg0))
            {
              int arg0_known = 0;
              int64_t bits = 0;
              if (irop_is_immediate(arg0))
              {
                bits = irop_get_imm64_ex(ir, arg0);
                arg0_known = 1;
              }
              else
              {
                int32_t vr0 = irop_get_vreg(arg0);
                if (vr0 >= 0 && TCCIR_DECODE_VREG_TYPE(vr0) == TCCIR_VREG_TYPE_VAR)
                {
                  int pos0 = TCCIR_DECODE_VREG_POSITION(vr0);
                  if (pos0 >= 0 && pos0 <= max_vreg && VT_IS_CONST(state, pos0))
                  {
                    bits = state[pos0].value;
                    arg0_known = 1;
                  }
                }
              }
              if (arg0_known)
              {
                int result = 0;
                if (is_fp32)
                {
                  uint32_t u = (uint32_t)bits;
                  uint32_t exp = (u >> 23) & 0xFFU;
                  uint32_t man = u & 0x7FFFFFU;
                  int is_inf = (exp == 0xFFU && man == 0);
                  int is_nan = (exp == 0xFFU && man != 0);
                  int sign_neg = (u >> 31) & 1U;
                  if (is_isinff)
                    result = is_inf ? (sign_neg ? -1 : 1) : 0;
                  else if (is_isnanf)
                    result = is_nan ? 1 : 0;
                  else /* finitef */
                    result = (!is_inf && !is_nan) ? 1 : 0;
                }
                else
                {
                  uint64_t u = (uint64_t)bits;
                  uint64_t exp = (u >> 52) & 0x7FFULL;
                  uint64_t man = u & 0xFFFFFFFFFFFFFULL;
                  int is_inf = (exp == 0x7FFULL && man == 0);
                  int is_nan = (exp == 0x7FFULL && man != 0);
                  int sign_neg = (u >> 63) & 1ULL;
                  if (is_isinfd)
                    result = is_inf ? (sign_neg ? -1 : 1) : 0;
                  else if (is_isnand)
                    result = is_nan ? 1 : 0;
                  else /* finite */
                    result = (!is_inf && !is_nan) ? 1 : 0;
                }
                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                tcc_ir_set_src1(ir, i, irop_make_imm32(-1, result, IROP_BTYPE_INT32));
                tcc_ir_set_src2(ir, i, IROP_NONE);
                {
                  int32_t dv = irop_get_vreg(call_dest);
                  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
                  {
                    int dp = TCCIR_DECODE_VREG_POSITION(dv);
                    if (dp >= 0 && dp <= max_vreg)
                      VT_SET_CONST(state, dp, result);
                  }
                }
                LOG_IR_GEN("VALUE_TRACK: %s(0x%llx) = %d at i=%d -> folded", fname, (unsigned long long)bits, result, i);
                changes++;
                continue;
              }
            }
          }
        }
        /* Constant-fold __aeabi_llsl/__aeabi_llsr/__aeabi_lasr/__aeabi_lmul
         * calls when both arguments are compile-time constants. */
        {
          int is_llsl = (fname && strcmp(fname, "__aeabi_llsl") == 0);
          int is_llsr = (fname && strcmp(fname, "__aeabi_llsr") == 0);
          int is_lasr = (fname && strcmp(fname, "__aeabi_lasr") == 0);
          int is_lmul = (fname && strcmp(fname, "__aeabi_lmul") == 0);
          if (is_llsl || is_llsr || is_lasr || is_lmul)
          {
            IROperand arg0, arg1;
            if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
            {
              int arg0_known = irop_is_immediate(arg0);
              int arg1_known = irop_is_immediate(arg1);
              int64_t val0 = arg0_known ? irop_get_imm64_ex(ir, arg0) : 0;
              int64_t val1 = arg1_known ? irop_get_imm64_ex(ir, arg1) : 0;

              if (!arg0_known)
              {
                int32_t vr0 = irop_get_vreg(arg0);
                if (vr0 >= 0 && TCCIR_DECODE_VREG_TYPE(vr0) == TCCIR_VREG_TYPE_VAR)
                {
                  int pos0 = TCCIR_DECODE_VREG_POSITION(vr0);
                  if (pos0 >= 0 && pos0 <= max_vreg && VT_IS_CONST(state, pos0))
                  {
                    val0 = state[pos0].value;
                    arg0_known = 1;
                  }
                }
                if (!arg0_known)
                {
                  uint64_t uval;
                  if (ir_opt_eval_const_u64(ir, arg0, i, &uval, 0))
                  {
                    val0 = (int64_t)uval;
                    arg0_known = 1;
                  }
                }
              }
              if (!arg1_known)
              {
                int32_t vr1 = irop_get_vreg(arg1);
                if (vr1 >= 0 && TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_VAR)
                {
                  int pos1 = TCCIR_DECODE_VREG_POSITION(vr1);
                  if (pos1 >= 0 && pos1 <= max_vreg && VT_IS_CONST(state, pos1))
                  {
                    val1 = state[pos1].value;
                    arg1_known = 1;
                  }
                }
              }

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

                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                if (result == (int32_t)result)
                  tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, IROP_BTYPE_INT64));
                else
                {
                  uint32_t pool_idx = tcc_ir_pool_add_i64(ir, result);
                  tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, IROP_BTYPE_INT64));
                }
                tcc_ir_set_src2(ir, i, IROP_NONE);
                {
                  int32_t dv = irop_get_vreg(call_dest);
                  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
                  {
                    int dp = TCCIR_DECODE_VREG_POSITION(dv);
                    if (dp >= 0 && dp <= max_vreg)
                      VT_SET_CONST(state, dp, result);
                  }
                }
                LOG_IR_GEN("VALUE_TRACK: %s(%lld, %lld) = %lld at i=%d -> folded", fname, (long long)val0,
                           (long long)val1, (long long)result, i);
                changes++;
                continue;
              }

              /* Lower shift calls with immediate shift amount to IR
               * instructions so subsequent passes can optimize them. */
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
                changes++;
                continue;
              }
            }
          }
        }
      }
    }

    /* Constant-fold soft-float arithmetic calls (__aeabi_fadd/fsub/fmul/fdiv,
     * __aeabi_f2iz, __aeabi_dadd/dsub/dmul/ddiv, __aeabi_d2iz, conversions)
     * when all arguments are compile-time constants.  Uses host FPU. */
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      Sym *sf_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (sf_callee)
      {
        const char *sf = get_tok_str(sf_callee->v, NULL);
        if (sf)
        {
          /* Classify: nargs=1 or 2, float or double */
          int sf_nargs = 0, sf_kind = 0;
          /* kinds: 1=add 2=sub 3=mul 4=div 5=f2iz 6=f2uiz 7=i2f 8=ui2f
           *        9=f2d 10=d2f 11=d2iz 12=d2uiz 13=i2d 14=ui2d */
          if (strcmp(sf, "__aeabi_fadd") == 0)
          {
            sf_nargs = 2;
            sf_kind = 1;
          }
          else if (strcmp(sf, "__aeabi_fsub") == 0)
          {
            sf_nargs = 2;
            sf_kind = 2;
          }
          else if (strcmp(sf, "__aeabi_fmul") == 0)
          {
            sf_nargs = 2;
            sf_kind = 3;
          }
          else if (strcmp(sf, "__aeabi_fdiv") == 0)
          {
            sf_nargs = 2;
            sf_kind = 4;
          }
          /* Double-returning operations (dadd/dsub/dmul/ddiv, f2d): result is
           * 64-bit and is materialized via the F64 immediate pool below. */
          else if (strcmp(sf, "__aeabi_dadd") == 0)
          {
            sf_nargs = 2;
            sf_kind = 1 | 0x80;
          }
          else if (strcmp(sf, "__aeabi_dsub") == 0)
          {
            sf_nargs = 2;
            sf_kind = 2 | 0x80;
          }
          else if (strcmp(sf, "__aeabi_dmul") == 0)
          {
            sf_nargs = 2;
            sf_kind = 3 | 0x80;
          }
          else if (strcmp(sf, "__aeabi_ddiv") == 0)
          {
            sf_nargs = 2;
            sf_kind = 4 | 0x80;
          }
          else if (strcmp(sf, "__aeabi_f2iz") == 0)
          {
            sf_nargs = 1;
            sf_kind = 5;
          }
          else if (strcmp(sf, "__aeabi_f2uiz") == 0)
          {
            sf_nargs = 1;
            sf_kind = 6;
          }
          else if (strcmp(sf, "__aeabi_i2f") == 0)
          {
            sf_nargs = 1;
            sf_kind = 7;
          }
          else if (strcmp(sf, "__aeabi_ui2f") == 0)
          {
            sf_nargs = 1;
            sf_kind = 8;
          }
          /* __aeabi_f2d and __aeabi_d2f are normally NOT folded: the
           * float_narrowing pass pattern-matches f2d → double-math → d2f
           * and rewrites it to a single float-precision call.  Folding
           * f2d/d2f to constants prevents narrowing from firing.
           * After float_narrowing has run (ir_post_float_narrow), it's
           * safe to fold — DSF can create new constant f2d arguments
           * that need folding to enable downstream comparison folding. */
          else if (tcc_state->ir_post_float_narrow && strcmp(sf, "__aeabi_f2d") == 0)
          {
            sf_nargs = 1;
            sf_kind = 9;
          }
          else if (tcc_state->ir_post_float_narrow && strcmp(sf, "__aeabi_d2f") == 0)
          {
            sf_nargs = 1;
            sf_kind = 10;
          }
          else if (strcmp(sf, "__aeabi_i2d") == 0)
          {
            sf_nargs = 1;
            sf_kind = 13;
          }
          else if (strcmp(sf, "__aeabi_ui2d") == 0)
          {
            sf_nargs = 1;
            sf_kind = 14;
          }
          else if (strcmp(sf, "__aeabi_d2iz") == 0)
          {
            sf_nargs = 1;
            sf_kind = 11;
          }
          else if (strcmp(sf, "__aeabi_d2uiz") == 0)
          {
            sf_nargs = 1;
            sf_kind = 12;
          }
          else if (strcmp(sf, "copysignf") == 0 || strcmp(sf, "__copysignf") == 0)
          {
            sf_nargs = 2;
            sf_kind = 15;
          }
          else if (strcmp(sf, "copysign") == 0 || strcmp(sf, "__copysign") == 0)
          {
            sf_nargs = 2;
            sf_kind = 16;
          }

          if (sf_kind)
          {
            /* Resolve arguments */
            int64_t a0 = 0, a1 = 0;
            int a0_ok = 0, a1_ok = 0;
            IROperand op0;
            if (ir_opt_get_call_param_operand(ir, i, 0, &op0))
            {
              if (irop_is_immediate(op0))
              {
                a0 = irop_get_imm64_ex(ir, op0);
                a0_ok = 1;
              }
              else
              {
                int32_t vr = irop_get_vreg(op0);
                if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
                {
                  int p = TCCIR_DECODE_VREG_POSITION(vr);
                  if (p >= 0 && p <= max_vreg && VT_IS_CONST(state, p))
                  {
                    a0 = state[p].value;
                    a0_ok = 1;
                  }
                }
              }
            }
            if (sf_nargs >= 2)
            {
              IROperand op1;
              if (ir_opt_get_call_param_operand(ir, i, 1, &op1))
              {
                if (irop_is_immediate(op1))
                {
                  a1 = irop_get_imm64_ex(ir, op1);
                  a1_ok = 1;
                }
                else
                {
                  int32_t vr = irop_get_vreg(op1);
                  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
                  {
                    int p = TCCIR_DECODE_VREG_POSITION(vr);
                    if (p >= 0 && p <= max_vreg && VT_IS_CONST(state, p))
                    {
                      a1 = state[p].value;
                      a1_ok = 1;
                    }
                  }
                }
              }
            }
            else
              a1_ok = 1;

            if (a0_ok && a1_ok)
            {
              int64_t result = 0;
              int folded = 0;
              int is_dbl = (sf_kind & 0x80) != 0;
              int op = sf_kind & 0x7F;

              if (!is_dbl && op >= 1 && op <= 4)
              {
                /* Float binary: fadd/fsub/fmul/fdiv */
                union
                {
                  float f;
                  uint32_t u;
                } fa, fb, fr;
                fa.u = (uint32_t)a0;
                fb.u = (uint32_t)a1;
                switch (op)
                {
                case 1:
                  fr.f = fa.f + fb.f;
                  folded = 1;
                  break;
                case 2:
                  fr.f = fa.f - fb.f;
                  folded = 1;
                  break;
                case 3:
                  fr.f = fa.f * fb.f;
                  folded = 1;
                  break;
                case 4:
                  if (fb.u != 0)
                  {
                    fr.f = fa.f / fb.f;
                    folded = 1;
                  }
                  break;
                }
                if (folded)
                  result = (int64_t)(int32_t)fr.u;
              }
              else if (is_dbl && op >= 1 && op <= 4)
              {
                /* Double binary: dadd/dsub/dmul/ddiv */
                union
                {
                  double d;
                  uint64_t u;
                } da, db, dr;
                da.u = (uint64_t)a0;
                db.u = (uint64_t)a1;
                switch (op)
                {
                case 1:
                  dr.d = da.d + db.d;
                  folded = 1;
                  break;
                case 2:
                  dr.d = da.d - db.d;
                  folded = 1;
                  break;
                case 3:
                  dr.d = da.d * db.d;
                  folded = 1;
                  break;
                case 4:
                  if (db.u != 0)
                  {
                    dr.d = da.d / db.d;
                    folded = 1;
                  }
                  break;
                }
                if (folded)
                  result = (int64_t)dr.u;
              }
              else
                switch (sf_kind)
                {
                case 5:
                { /* f2iz */
                  union
                  {
                    float f;
                    uint32_t u;
                  } fa;
                  fa.u = (uint32_t)a0;
                  result = (int32_t)fa.f;
                  folded = 1;
                }
                break;
                case 6:
                { /* f2uiz */
                  union
                  {
                    float f;
                    uint32_t u;
                  } fa;
                  fa.u = (uint32_t)a0;
                  result = (int64_t)(uint32_t)fa.f;
                  folded = 1;
                }
                break;
                case 7:
                { /* i2f */
                  union
                  {
                    float f;
                    uint32_t u;
                  } fr;
                  fr.f = (float)(int32_t)a0;
                  result = (int64_t)(int32_t)fr.u;
                  folded = 1;
                }
                break;
                case 8:
                { /* ui2f */
                  union
                  {
                    float f;
                    uint32_t u;
                  } fr;
                  fr.f = (float)(uint32_t)a0;
                  result = (int64_t)(int32_t)fr.u;
                  folded = 1;
                }
                break;
                case 9:
                { /* f2d */
                  union
                  {
                    float f;
                    uint32_t u;
                  } fa;
                  fa.u = (uint32_t)a0;
                  union
                  {
                    double d;
                    uint64_t u;
                  } dr;
                  dr.d = (double)fa.f;
                  result = (int64_t)dr.u;
                  folded = 1;
                }
                break;
                case 10:
                { /* d2f */
                  union
                  {
                    double d;
                    uint64_t u;
                  } da;
                  da.u = (uint64_t)a0;
                  union
                  {
                    float f;
                    uint32_t u;
                  } fr;
                  fr.f = (float)da.d;
                  result = (int64_t)(int32_t)fr.u;
                  folded = 1;
                }
                break;
                case 11:
                { /* d2iz */
                  union
                  {
                    double d;
                    uint64_t u;
                  } da;
                  da.u = (uint64_t)a0;
                  result = (int32_t)da.d;
                  folded = 1;
                }
                break;
                case 12:
                { /* d2uiz */
                  union
                  {
                    double d;
                    uint64_t u;
                  } da;
                  da.u = (uint64_t)a0;
                  result = (int64_t)(uint32_t)da.d;
                  folded = 1;
                }
                break;
                case 13:
                { /* i2d */
                  union
                  {
                    double d;
                    uint64_t u;
                  } dr;
                  dr.d = (double)(int32_t)a0;
                  result = (int64_t)dr.u;
                  folded = 1;
                }
                break;
                case 14:
                { /* ui2d */
                  union
                  {
                    double d;
                    uint64_t u;
                  } dr;
                  dr.d = (double)(uint32_t)a0;
                  result = (int64_t)dr.u;
                  folded = 1;
                }
                break;
                case 15:
                { /* copysignf */
                  union { float f; uint32_t u; } fa, fb, fr;
                  fa.u = (uint32_t)a0;
                  fb.u = (uint32_t)a1;
                  fr.f = copysignf(fa.f, fb.f);
                  result = (int64_t)(int32_t)fr.u;
                  folded = 1;
                }
                break;
                case 16:
                { /* copysign (double) */
                  union { double d; uint64_t u; } da, db, dr;
                  da.u = (uint64_t)a0;
                  db.u = (uint64_t)a1;
                  dr.d = copysign(da.d, db.d);
                  result = (int64_t)dr.u;
                  folded = 1;
                }
                break;
                }

              if (folded)
              {
                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                /* 64-bit results (double-returning ops, f2d) need to be
                 * materialized via the F64 immediate pool because imm32 only
                 * holds 32 bits. */
                int dest_is_64 = irop_is_64bit(call_dest);
                IROperand imm_src;
                if (dest_is_64)
                {
                  uint32_t pool_idx = tcc_ir_pool_add_f64(ir, (uint64_t)result);
                  imm_src = irop_make_f64(-1, pool_idx);
                }
                else
                {
                  imm_src = irop_make_imm32(-1, (int32_t)result, IROP_BTYPE_INT32);
                }
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                tcc_ir_set_src1(ir, i, imm_src);
                tcc_ir_set_src2(ir, i, IROP_NONE);
                /* Update value tracking for the dest so subsequent folds
                 * see the correct value (the continue skips normal processing).
                 * state[].value is int64_t so it can carry the full result. */
                {
                  int32_t dv = irop_get_vreg(call_dest);
                  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
                  {
                    int dp = TCCIR_DECODE_VREG_POSITION(dv);
                    if (dp >= 0 && dp <= max_vreg)
                      VT_SET_CONST(state, dp, dest_is_64 ? result : (int64_t)(int32_t)result);
                  }
                }
                LOG_IR_GEN("VALUE_TRACK: %s -> %lld at i=%d (soft-float fold)", sf, (long long)result, i);
                changes++;
                continue;
              }
            }
          }
        }
      }
    }

    /* Constant-fold __aeabi_cfcmple/cdcmple VOID calls + subsequent JUMPIF or
     * SETIF.  These set CPU flags; when both args are known constants, replace
     * the call+consumer pair with an unconditional jump/nop (JUMPIF) or with
     * an ASSIGN of the boolean result (SETIF).
     *
     * SETIF folding is essential to unblock chains like
     *   cdcmple → SETIF (int 0/1) → __aeabi_i2d → cdcmple → JUMPIF
     * (e.g. `(a != b) != 1.0`): without it, the int-bool intermediate stays
     * runtime-only and downstream i2d / second cdcmple can't be folded. */
    if (q->op == TCCIR_OP_FUNCCALLVOID && i + 1 < n)
    {
      IRQuadCompact *next_q = &ir->compact_instructions[i + 1];
      if (next_q->op == TCCIR_OP_JUMPIF || next_q->op == TCCIR_OP_SETIF)
      {
        Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
        if (callee)
        {
          const char *fname = get_tok_str(callee->v, NULL);
          int is_fcmp = (fname && (strcmp(fname, "__aeabi_cfcmple") == 0 || strcmp(fname, "__aeabi_cfcmpeq") == 0));
          int is_dcmp = (fname && (strcmp(fname, "__aeabi_cdcmple") == 0 || strcmp(fname, "__aeabi_cdcmpeq") == 0));
          if (is_fcmp || is_dcmp)
          {
            IROperand arg0, arg1;
            if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
            {
              int64_t a0 = 0, a1 = 0;
              int a0_ok = irop_is_immediate(arg0), a1_ok = irop_is_immediate(arg1);
              if (a0_ok) a0 = irop_get_imm64_ex(ir, arg0);
              if (a1_ok) a1 = irop_get_imm64_ex(ir, arg1);
              if (!a0_ok)
              {
                int32_t vr = irop_get_vreg(arg0);
                if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
                {
                  int p = TCCIR_DECODE_VREG_POSITION(vr);
                  if (p >= 0 && p <= max_vreg && VT_IS_CONST(state, p))
                  { a0 = state[p].value; a0_ok = 1; }
                }
              }
              if (!a1_ok)
              {
                int32_t vr = irop_get_vreg(arg1);
                if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
                {
                  int p = TCCIR_DECODE_VREG_POSITION(vr);
                  if (p >= 0 && p <= max_vreg && VT_IS_CONST(state, p))
                  { a1 = state[p].value; a1_ok = 1; }
                }
              }
              if (a0_ok && a1_ok)
              {
                int cmp_result, is_nan;
                if (is_fcmp)
                {
                  union { float f; uint32_t u; } fa, fb;
                  fa.u = (uint32_t)a0;
                  fb.u = (uint32_t)a1;
                  is_nan = (fa.f != fa.f) || (fb.f != fb.f);
                  cmp_result = (fa.f > fb.f) - (fa.f < fb.f);
                }
                else
                {
                  union { double d; uint64_t u; } da, db;
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
                  /* IEEE: ordered predicates are FALSE for NaN, NE is TRUE.
                   * Helper returns -1 for tokens where the soft-FP runtime
                   * disagrees with IEEE (GT/GE family), so we leave those
                   * as runtime calls. */
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
                  /* Track the SETIF dest as a known constant so downstream
                   * folds (i2d, second cdcmple, etc.) can cascade in this
                   * same pass. */
                  IROperand setif_dest = tcc_ir_op_get_dest(ir, next_q);
                  int32_t dv = irop_get_vreg(setif_dest);
                  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
                  {
                    int dp = TCCIR_DECODE_VREG_POSITION(dv);
                    if (dp >= 0 && dp <= max_vreg)
                      VT_SET_CONST(state, dp, result);
                  }
                  LOG_IR_GEN("VALUE_TRACK: %s+SETIF fold -> cmp=%d result=%d at i=%d", fname, cmp_result, result, i);
                }
                changes++;
                continue;
              }
            }
          }
        }
        cdcmple_void_fold_skip:;
      }
    }

    /* Function calls can modify any address-taken variable through pointers.
     * Invalidate only tracked addrtaken constants — O(k) instead of O(max_vreg). */
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      if (!addrtaken_overflow)
      {
        for (int a = 0; a < num_addrtaken; a++)
        {
          int v = addrtaken_list[a];
          if (VT_IS_CONST(state, v))
            VT_INVALIDATE(state, v);
        }
      }
      else
      {
        /* Overflow fallback: scan all vregs (rare) */
        for (int v = 0; v <= max_vreg; v++)
        {
          if (VT_IS_CONST(state, v) && (is_addrtaken[v / 8] & (1 << (v % 8))))
            VT_INVALIDATE(state, v);
        }
      }
    }

    /* Any other instruction that defines a VAR vreg invalidates the constant */
    if (dest_pos >= 0 && dest_pos <= max_vreg && irop_config[q->op].has_dest)
      VT_INVALIDATE(state, dest_pos);
  }

  tcc_free(is_addrtaken);
  tcc_free(lea_var_map);
  tcc_free(lea_map);
  tcc_free(state);
  tcc_free(is_merge);

  /* Run DCE to remove code after eliminated branches */
  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

#undef VT_IS_CONST
#undef VT_HAS_DEF
#undef VT_SET_CONST
#undef VT_SET_CONST_DEF
#undef VT_INVALIDATE
#undef VT_CLEAR_DEF

/* ============================================================================
 * VRP (Value Range Propagation)
 * ============================================================================
 *
 * Tracks integer value ranges for PARAM and TEMP vregs through the IR.
 * Derives range constraints from conditional branch fall-through paths,
 * propagates constraints through arithmetic, and folds subsequent comparisons
 * when the range fully determines the outcome.
 *
 * Example:
 *   CMP P0, #0
 *   JMP to X if "<=S"     ; fall-through: P0 > 0, i.e. P0 in [1, INT32_MAX]
 *   T0 = P0 - #1          ; T0 in [0, INT32_MAX-1]
 *   CMP T0, #-1           ; -1 == UINT32_MAX as unsigned
 *   JMP to X if "<U"      ; T0 <U UINT32_MAX always true → fold to unconditional JUMP
 *
 * The second branch is always taken (T0 >= 0 implies T0 <U UINT32_MAX),
 * enabling dead code elimination of the otherwise-unreachable block.
 */

/* Maximum vreg positions tracked per type */
#define VRP_MAX_POS 256

/* Range state for a single vreg slot */

int tcc_ir_opt_const_prop_tmp(TCCIRState *ir)
{
  typedef struct
  {
    int gen; /* Generation when this entry is valid */
    int64_t value;
  } TmpConstInfo;

  /* Stack buffers for common case */
#define TMP_CONST_STACK_SIZE 64
#define TMP_CONST_STACK_N 256
  TmpConstInfo tmp_info_stack[TMP_CONST_STACK_SIZE];
  int block_start_seen_stack[TMP_CONST_STACK_N];

  int n = ir->next_instruction_index;
  int changes = 0;
  int max_tmp_pos = 0;
  int max_var_pos = -1;
  int current_gen = 1; /* Generation counter, 0 means invalid */
  int i;
  IRQuadCompact *q;
  TmpConstInfo *tmp_info;
  TmpConstInfo *var_info = NULL;
  uint8_t *var_addrtaken = NULL;
  int *block_start_seen;
  int block_start_gen = 1;
  void *heap_alloc = NULL;

  if (n == 0)
    return 0;

  /* Find max TMP and VAR positions */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (!irop_config[q->op].has_dest)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (pos > max_tmp_pos)
        max_tmp_pos = pos;
    }
    else if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (pos > max_var_pos)
        max_var_pos = pos;
    }
  }

  if (max_tmp_pos == 0 && max_var_pos < 0)
    return 0;

  /* Use stack buffers if possible */
  if (max_tmp_pos < TMP_CONST_STACK_SIZE && n <= TMP_CONST_STACK_N)
  {
    tmp_info = tmp_info_stack;
    block_start_seen = block_start_seen_stack;
    memset(tmp_info, 0, sizeof(TmpConstInfo) * (max_tmp_pos + 1));
    memset(block_start_seen, 0, sizeof(int) * n);
  }
  else
  {
    size_t tmp_size = sizeof(TmpConstInfo) * (max_tmp_pos + 1);
    size_t block_size = sizeof(int) * n;
    heap_alloc = tcc_mallocz(tmp_size + block_size);
    tmp_info = (TmpConstInfo *)heap_alloc;
    block_start_seen = (int *)((char *)heap_alloc + tmp_size);
  }

  /* Allocate VAR tracking arrays */
  if (max_var_pos >= 0)
  {
    var_info = tcc_mallocz(sizeof(TmpConstInfo) * (max_var_pos + 1));
    var_addrtaken = tcc_mallocz((max_var_pos + 8) / 8);
    for (i = 0; i < n; i++)
    {
      q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_LEA)
      {
        IROperand lsrc = tcc_ir_op_get_src1(ir, q);
        int32_t sv = irop_get_vreg(lsrc);
        if (sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_VAR)
        {
          int vp = TCCIR_DECODE_VREG_POSITION(sv);
          if (vp <= max_var_pos)
            var_addrtaken[vp / 8] |= (1 << (vp % 8));
        }
      }
    }
  }

  /* Mark block starts (shared helper) */
  ir_opt_mark_block_starts(ir, block_start_seen, block_start_gen, n);

  /* Single pass: track TMP/VAR constants and propagate */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];

    /* Clear at basic block entry (jump targets) - O(1) via generation bump */
    if (i != 0 && block_start_seen[i] == block_start_gen)
    {
      current_gen++;
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int32_t src1_vr = irop_get_vreg(src1);

    /* Resolve SWITCH_TABLE when the index TMP is a known constant:
     * replace with a direct JUMP to the appropriate case target. */
    if (q->op == TCCIR_OP_SWITCH_TABLE && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
      if (pos <= max_tmp_pos && tmp_info[pos].gen == current_gen)
      {
        int64_t index_val = tmp_info[pos].value;
        IROperand src2 = tcc_ir_op_get_src2(ir, q);
        int table_id = (int)irop_get_imm64_ex(ir, src2);
        if (table_id >= 0 && table_id < ir->num_switch_tables)
        {
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
          changes++;
          current_gen++;
          continue;
        }
      }
    }

    /* Propagate TMP/VAR constants to src1.
     * Skip SWITCH_TABLE and IJUMP: their src1 (the index / target address)
     * must remain in a register — the ARM code generator cannot handle an
     * immediate operand there. */
    if (irop_config[q->op].has_src1 && q->op != TCCIR_OP_SWITCH_TABLE && q->op != TCCIR_OP_IJUMP)
    {
      int do_prop = 0;
      int64_t prop_val = 0;
      if (TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (pos <= max_tmp_pos && tmp_info[pos].gen == current_gen)
        {
          do_prop = 1;
          prop_val = tmp_info[pos].value;
        }
      }
      else if (max_var_pos >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR &&
               !(src1.is_local && !src1.is_lval))
      {
        const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (pos <= max_var_pos && var_info[pos].gen == current_gen)
        {
          do_prop = 1;
          prop_val = var_info[pos].value;
        }
      }
      if (do_prop)
      {
        int btype = irop_get_btype(src1);
        IROperand new_src1;
        if (prop_val == (int32_t)prop_val)
        {
          new_src1 = irop_make_imm32(-1, (int32_t)prop_val, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, prop_val);
          new_src1 = irop_make_i64(-1, pool_idx, btype);
        }
        /* Preserve type flags but NOT memory-access flags.
         * is_lval/is_llocal/is_local describe stack-slot semantics that
         * don't apply to an immediate constant value. */
        new_src1.is_unsigned = src1.is_unsigned;
        new_src1.is_static = src1.is_static;
        tcc_ir_set_src1(ir, i, new_src1);
        changes++;
      }
    }

    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int32_t src2_vr = irop_get_vreg(src2);
    /* Propagate TMP/VAR constants to src2 */
    if (irop_config[q->op].has_src2)
    {
      int do_prop = 0;
      int64_t prop_val = 0;
      if (TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        const int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
        if (pos <= max_tmp_pos && tmp_info[pos].gen == current_gen)
        {
          do_prop = 1;
          prop_val = tmp_info[pos].value;
        }
      }
      else if (max_var_pos >= 0 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR)
      {
        const int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
        if (pos <= max_var_pos && var_info[pos].gen == current_gen)
        {
          do_prop = 1;
          prop_val = var_info[pos].value;
        }
      }
      if (do_prop)
      {
        LOG_IR_GEN("OPTIMIZE: const propagate vreg %d = %lld to src2 at i=%d", src2_vr, (long long)prop_val, i);
        int btype = irop_get_btype(src2);
        int64_t val = prop_val;
        /* When propagating a narrow constant into a wider bitwise op,
         * widen it to INT64 with zero-extension so the code generator
         * doesn't sign-extend the immediate into the upper register. */
        int src1_bt = irop_get_btype(src1);
        if (src1_bt == IROP_BTYPE_INT64 && btype != IROP_BTYPE_INT64 &&
            (q->op == TCCIR_OP_OR || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_XOR))
        {
          val = (int64_t)(uint32_t)val;
          btype = IROP_BTYPE_INT64;
        }
        IROperand new_src2;
        if (val == (int32_t)val)
        {
          new_src2 = irop_make_imm32(-1, (int32_t)val, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_src2 = irop_make_i64(-1, pool_idx, btype);
        }
        /* Preserve type flags but NOT memory-access flags. */
        new_src2.is_unsigned = src2.is_unsigned;
        new_src2.is_static = src2.is_static;
        tcc_ir_set_src2(ir, i, new_src2);
        changes++;
      }
    }

    /* After propagation, fold if both operands are now immediate.
     * This cascades within a single pass: the result is tracked and
     * feeds the next instruction, avoiding multi-iteration ping-pong. */
    if (irop_config[q->op].has_src1 && irop_config[q->op].has_src2)
    {
      IROperand fs1 = tcc_ir_op_get_src1(ir, q);
      IROperand fs2 = tcc_ir_op_get_src2(ir, q);
      if (irop_is_immediate(fs1) && irop_is_immediate(fs2))
      {
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
          uint32_t n = (uint32_t)v2 & 31;
          res = (int64_t)(int32_t)((v >> n) | (v << (32 - n)));
          break;
        }
        case TCCIR_OP_MUL:
          res = (int64_t)((uint64_t)v1 * (uint64_t)v2);
          break;
        case TCCIR_OP_UMULL:
        {
          uint64_t uresult = (uint64_t)(uint32_t)v1 * (uint64_t)(uint32_t)v2;
          res = (int64_t)uresult;
          btype = IROP_BTYPE_INT64;
          break;
        }
        case TCCIR_OP_SMULL:
        {
          int64_t sresult = (int64_t)(int32_t)v1 * (int64_t)(int32_t)v2;
          res = sresult;
          btype = IROP_BTYPE_INT64;
          break;
        }
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
          /* INT_MIN / -1 overflows and traps on hardware divide.  The
           * width-specific check matters: a 32-bit DIV with v1=INT32_MIN
           * passes a sign-extended v1 in this 64-bit slot, so the int64 check
           * misses it. */
          if (v2 == 0)
            ok = 0;
          else if (v2 == -1 &&
                   ((btype == IROP_BTYPE_INT64 && v1 == INT64_MIN) ||
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
          else if (v2 == -1 &&
                   ((btype == IROP_BTYPE_INT64 && v1 == INT64_MIN) ||
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
        if (ok)
        {
          if (btype != IROP_BTYPE_INT64 && btype != IROP_BTYPE_FLOAT64)
          {
            if (q->op == TCCIR_OP_SHL && v2 >= 32)
            {
              IROperand dest = tcc_ir_op_get_dest(ir, q);
              if (irop_get_btype(dest) == IROP_BTYPE_INT64)
                btype = IROP_BTYPE_INT64;
              else
                ok = 0;
            }
            else
              res = (int64_t)(int32_t)(uint32_t)res;
          }
        }
        if (ok)
        {
          q->op = TCCIR_OP_ASSIGN;
          IROperand nr;
          if (res == (int32_t)res)
            nr = irop_make_imm32(-1, (int32_t)res, btype);
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, res);
            nr = irop_make_i64(-1, pool_idx, btype);
          }
          tcc_ir_set_src1(ir, i, nr);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
        }
      }
    }

    /* CMP+SETIF fold: when TMP propagation makes both CMP operands immediate,
     * fold the CMP+SETIF pair in-place so TEST_ZERO+JUMPIF can be folded
     * within the same pass rather than waiting for the next const_prop round. */
    if (q->op == TCCIR_OP_CMP && i + 1 < n)
    {
      IRQuadCompact *next_q = &ir->compact_instructions[i + 1];
      if (next_q->op == TCCIR_OP_SETIF)
      {
        IROperand cs1 = tcc_ir_op_get_src1(ir, q);
        IROperand cs2 = tcc_ir_op_get_src2(ir, q);
        if (irop_is_immediate(cs1) && irop_is_immediate(cs2))
        {
          int64_t cv1 = irop_get_imm64_ex(ir, cs1);
          int64_t cv2 = irop_get_imm64_ex(ir, cs2);
          IROperand setif_src1 = tcc_ir_op_get_src1(ir, next_q);
          int cond = (int)irop_get_imm64_ex(ir, setif_src1);
          int result = evaluate_compare_condition(cv1, cv2, cond);
          if (result >= 0)
          {
            q->op = TCCIR_OP_NOP;
            next_q->op = TCCIR_OP_ASSIGN;
            int btype = irop_get_btype(setif_src1);
            tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, btype));
            tcc_ir_set_src2(ir, i + 1, IROP_NONE);
            changes++;
          }
        }
      }
    }

    /* Fold __aeabi_cfcmple/cdcmple VOID calls + JUMPIF or SETIF when TMP
     * propagation has made both PARAM args immediate.  SETIF folding lets
     * cdcmple → SETIF → i2d → cdcmple chains collapse end-to-end. */
    if (q->op == TCCIR_OP_FUNCCALLVOID && i + 1 < n)
    {
      IRQuadCompact *next_q = &ir->compact_instructions[i + 1];
      if (next_q->op == TCCIR_OP_JUMPIF || next_q->op == TCCIR_OP_SETIF)
      {
        Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
        if (callee)
        {
          const char *fn = get_tok_str(callee->v, NULL);
          int is_fcmp = (fn && (strcmp(fn, "__aeabi_cfcmple") == 0 || strcmp(fn, "__aeabi_cfcmpeq") == 0));
          int is_dcmp = (fn && (strcmp(fn, "__aeabi_cdcmple") == 0 || strcmp(fn, "__aeabi_cdcmpeq") == 0));
          if (is_fcmp || is_dcmp)
          {
            IROperand arg0, arg1;
            if (ir_opt_get_call_param_operand(ir, i, 0, &arg0) && ir_opt_get_call_param_operand(ir, i, 1, &arg1))
            {
              if (irop_is_immediate(arg0) && irop_is_immediate(arg1))
              {
                int64_t a0 = irop_get_imm64_ex(ir, arg0);
                int64_t a1 = irop_get_imm64_ex(ir, arg1);
                int cmp_result, is_nan;
                if (is_fcmp)
                {
                  union { float f; uint32_t u; } fa, fb;
                  fa.u = (uint32_t)a0;
                  fb.u = (uint32_t)a1;
                  is_nan = (fa.f != fa.f) || (fb.f != fb.f);
                  cmp_result = (fa.f > fb.f) - (fa.f < fb.f);
                }
                else
                {
                  union { double d; uint64_t u; } da, db;
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
                  /* See nan_compare_branch_result — IEEE NaN semantics for
                   * tokens the IR generator emits after cfcmple. */
                  result = nan_compare_branch_result(tok);
                  if (result < 0)
                    goto cdcmple_tmp_fold_skip;
                }
                else
                {
                  result = evaluate_compare_condition(cmp_result, 0, tok);
                  if (result < 0)
                    goto cdcmple_tmp_fold_skip;
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
                }
                else /* SETIF */
                {
                  int btype = irop_get_btype(cond);
                  next_q->op = TCCIR_OP_ASSIGN;
                  tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, btype));
                  tcc_ir_set_src2(ir, i + 1, IROP_NONE);
                  /* Record the new TMP constant so the rest of this pass
                   * (e.g. propagation into the following FUNCPARAM/i2d) sees
                   * it as known.  Bump current_gen *first* so this entry
                   * isn't wiped by the FUNCCALLVOID block-boundary bump. */
                  current_gen++;
                  IROperand setif_dest = tcc_ir_op_get_dest(ir, next_q);
                  int32_t dv = irop_get_vreg(setif_dest);
                  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
                  {
                    int dp = TCCIR_DECODE_VREG_POSITION(dv);
                    if (dp <= max_tmp_pos)
                    {
                      tmp_info[dp].gen = current_gen;
                      tmp_info[dp].value = result;
                    }
                  }
                  changes++;
                  continue;
                }
                changes++;
                current_gen++;
                continue;
              }
            }
          }
        }
        cdcmple_tmp_fold_skip:;
      }
    }

    /* Clear all at basic block boundaries - O(1) via generation bump */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      current_gen++;
      continue;
    }

    /* Track TMP <- constant assignments (re-fetch src1 since fold may have changed it) */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP &&
        (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_CVT_FTOF))
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      IROperand cur_src1 = tcc_ir_op_get_src1(ir, q);
      if (pos <= max_tmp_pos && irop_is_immediate(cur_src1))
      {
        tmp_info[pos].gen = current_gen;
        tmp_info[pos].value = irop_get_imm64_ex(ir, cur_src1);
      }
    }

    /* Track VAR <- constant assignments within basic blocks.
     * Unlike single-def const_var_prop, this handles multi-def VARs by
     * tracking the most recent constant value and invalidating on
     * non-constant redefinitions. */
    if (max_var_pos >= 0 && irop_config[q->op].has_dest &&
        TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (pos <= max_var_pos && !(var_addrtaken[pos / 8] & (1 << (pos % 8))))
      {
        if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) && !dest.is_lval)
        {
          IROperand cur_src1 = tcc_ir_op_get_src1(ir, q);
          if (irop_is_immediate(cur_src1) && !cur_src1.is_sym)
          {
            var_info[pos].gen = current_gen;
            var_info[pos].value = irop_get_imm64_ex(ir, cur_src1);
          }
          else
          {
            var_info[pos].gen = 0;
          }
        }
        else
        {
          var_info[pos].gen = 0;
        }
      }
    }

  }

  if (var_info)
    tcc_free(var_info);
  if (var_addrtaken)
    tcc_free(var_addrtaken);
  if (heap_alloc)
    tcc_free(heap_alloc);

  return changes;
#undef TMP_CONST_STACK_SIZE
#undef TMP_CONST_STACK_N
}

/* ADD/SUB Constant Reassociation
 *
 * Normalizes ADD/SUB chains with constant operands so that cascaded
 * pointer arithmetic collapses into a single ADD from the original base:
 *
 *   ADD(ADD(base, c1), c2)  →  ADD(base, c1+c2)
 *
 * This enables downstream CMP identity folding to recognize that two
 * independently computed "base + N" values are identical.
 */

int tcc_ir_opt_add_reassoc(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2 || n > 4000)
    return 0;

  uint8_t *is_merge = ir_opt_build_merge_bitmap(ir, n);
  int dc_stride = 0;
  uint8_t *dc = ir_opt_build_def_count(ir, n, &dc_stride);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(src2))
      continue;

    /* Bail on real memory dereferences only. Register-promoted locals
     * (is_lval && is_local) and llocals carry is_lval as a tag but
     * read from a register, so substituting their def-value is sound.
     * Matches the does_memory_deref predicate elsewhere in this file. */
    if (src1.is_lval && !src1.is_const && !src1.is_local && !src1.is_llocal)
      continue;

    /* Direct case: src1 is itself a symref-by-value (the prior symref-prop
     * pass folded the vreg use into a sym operand).  Combine directly. */
    if (src1.is_sym && !src1.is_lval)
    {
      IRPoolSymref *sref = irop_get_symref_ex(ir, src1);
      if (!sref || !sref->sym)
        continue;
      int64_t c2_d = irop_get_imm64_ex(ir, src2);
      int64_t eff_c2_d = (q->op == TCCIR_OP_SUB) ? -c2_d : c2_d;
      int64_t new_addend_d = (int64_t)sref->addend + eff_c2_d;
      if (new_addend_d != (int32_t)new_addend_d)
        continue;
      Sym *target_sym = sref->sym;
      uint32_t sref_flags = sref->flags;
      int btype_d = irop_get_btype(src1);
      uint8_t local_d = src1.is_local;
      uint8_t const_d = src1.is_const;
      uint8_t uns_d = src1.is_unsigned;
      uint32_t pool_idx_d = tcc_ir_pool_add_symref(ir, target_sym, (int32_t)new_addend_d, sref_flags);
      IROperand new_src_d = irop_make_symref(-1, pool_idx_d, 0, local_d, const_d, btype_d);
      new_src_d.is_unsigned = uns_d;
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src_d);
      changes++;
      continue;
    }

    int32_t src1_vr = irop_get_vreg(src1);
    if (src1_vr < 0)
      continue;

    int def_idx = tcc_ir_find_defining_instruction(ir, src1_vr, i);
    if (def_idx < 0)
      continue;

    /* Verify def_idx and i are in the same straight-line basic block.
     * The merge bitmap only flags multi-predecessor instructions; a
     * forward-jump target with a single (non-fall-through) predecessor
     * is NOT a merge but IS a block boundary, and the linear-scan def
     * lookup will incorrectly find a def that doesn't actually reach i.
     * Bail on any merge OR any prior JUMP/RETURN that breaks linearity. */
    {
      int safe = 1;
      for (int j = def_idx + 1; j <= i; j++)
      {
        if (is_merge[j / 8] & (1 << (j % 8)))
        {
          safe = 0;
          break;
        }
        if (j > 0)
        {
          int prev_op = ir->compact_instructions[j - 1].op;
          if (prev_op == TCCIR_OP_JUMP || prev_op == TCCIR_OP_RETURNVALUE ||
              prev_op == TCCIR_OP_RETURNVOID)
          {
            safe = 0;
            break;
          }
        }
      }
      if (!safe)
        continue;
    }

    IRQuadCompact *def_q = &ir->compact_instructions[def_idx];

    /* Accept either ADD/SUB-with-immediate (the chain case) or an ASSIGN
     * whose source is an address-constant (a symref-by-value).  The latter
     * lets `ADD(T, imm)` collapse into a single `ASSIGN T2 = &S+(addend+imm)`
     * when an earlier pass (e.g. global-init-prop) replaced a const pointer
     * load with a symref. */
    int def_is_assign_symref = 0;
    IROperand def_src1;
    int64_t eff_c1 = 0;

    if (def_q->op == TCCIR_OP_ADD || def_q->op == TCCIR_OP_SUB)
    {
      IROperand def_src2 = tcc_ir_op_get_src2(ir, def_q);
      if (!irop_is_immediate(def_src2))
        continue;
      def_src1 = tcc_ir_op_get_src1(ir, def_q);
      int64_t c1 = irop_get_imm64_ex(ir, def_src2);
      eff_c1 = (def_q->op == TCCIR_OP_SUB) ? -c1 : c1;
    }
    else if (def_q->op == TCCIR_OP_ASSIGN)
    {
      def_src1 = tcc_ir_op_get_src1(ir, def_q);
      if (!def_src1.is_sym || def_src1.is_lval)
        continue;
      def_is_assign_symref = 1;
      eff_c1 = 0;
    }
    else
    {
      continue;
    }

    /* The reassociation replaces src1_vr with def_src1 at the use point.
     * If def_src1 is a vreg, it must not be redefined between def_idx and i
     * (inclusive of def_idx, since def_q itself may write inner_vr, e.g.
     * self-update chains like V0 = V0 + 200 where def_src1 and def_dst are
     * both V0 — substituting reads V0 at the wrong point). */
    int32_t inner_vr = irop_get_vreg(def_src1);
    if (inner_vr >= 0 && !DC_IS_SINGLE_DEF(dc, dc_stride, inner_vr))
    {
      int redefined = 0;
      for (int j = def_idx; j < i; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_NOP)
          continue;
        IROperand jdst = tcc_ir_op_get_dest(ir, jq);
        if (irop_get_vreg(jdst) == inner_vr)
        {
          redefined = 1;
          break;
        }
      }
      if (redefined)
        continue;
    }

    int64_t c2 = irop_get_imm64_ex(ir, src2);
    int64_t eff_c2 = (q->op == TCCIR_OP_SUB) ? -c2 : c2;
    int64_t combined = eff_c1 + eff_c2;

    if (combined != (int32_t)combined)
      continue;

    int btype = irop_get_btype(src2);
    LOG_IR_GEN("OPTIMIZE: ADD reassoc at i=%d: (%lld) + (%lld) = %lld",
               i, (long long)eff_c1, (long long)eff_c2, (long long)combined);

    if (def_is_assign_symref)
    {
      /* Fold `T <- symref(S,+A); T2 <- T ± imm` into `T2 <- symref(S,+A±imm)`.
       * Builds a new symref pool entry with the combined addend. */
      IRPoolSymref *sref = irop_get_symref_ex(ir, def_src1);
      if (!sref || !sref->sym)
        continue;
      int64_t new_addend = (int64_t)sref->addend + combined;
      if (new_addend != (int32_t)new_addend)
        continue;
      uint32_t pool_idx = tcc_ir_pool_add_symref(ir, sref->sym, (int32_t)new_addend, sref->flags);
      IROperand new_src = irop_make_symref(-1, pool_idx, 0, def_src1.is_local, def_src1.is_const,
                                           irop_get_btype(def_src1));
      new_src.is_unsigned = def_src1.is_unsigned;
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src);
      tcc_ir_set_src2(ir, i, IROP_NONE);
    }
    else if (combined == 0)
    {
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, def_src1);
      tcc_ir_set_src2(ir, i, IROP_NONE);
    }
    else
    {
      q->op = TCCIR_OP_ADD;
      tcc_ir_set_src1(ir, i, def_src1);
      tcc_ir_set_src2(ir, i, irop_make_imm32(-1, (int32_t)combined, btype));
    }
    changes++;
  }

  tcc_free(dc);
  tcc_free(is_merge);
  return changes;
}

/* CMP Expression-Equality Fold
 *
 * Fold CMP+JUMPIF/SELECT when both CMP operands compute the same
 * expression (e.g. both are ADD(GlobalSym, 5) via different vregs).
 * Handles cross-type comparisons (VAR vs TEMP) by comparing at the
 * definition level, bypassing the STACKOFF/VREG tag mismatch that
 * ir_opt_pure_expr_equal cannot handle.
 */
int tcc_ir_opt_cmp_expr_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2 || n > 4000)
    return 0;

  int dc_stride = 0;
  uint8_t *dc = ir_opt_build_def_count(ir, n, &dc_stride);

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int32_t vr1 = irop_get_vreg(src1);
    int32_t vr2 = irop_get_vreg(src2);

    int def1 = -1, def2 = -1;
    int is_equal = 0;
    int both_nonvreg = (vr1 < 0 && vr2 < 0);

    if (both_nonvreg)
    {
      /* Both operands are immediates or symrefs.  Compare structurally;
       * const-var-prop may leave behind `CMP symref(X), symref(X)` that the
       * vreg-based path below would skip because vr1 == vr2 == -1. */
      is_equal = ir_opt_nonvreg_expr_equal(ir, src1, src2);
      /* Fallback for symref-vs-symref: the strict check requires every flag
       * to match, but the two operands at a CMP can carry different
       * unsigned/is_lval encodings from how the frontend lowered each side
       * even though both resolve to the same sym+addend.  For an equality
       * comparison the value-identity is enough — comparison of the same
       * symbol's address to itself is always equal. */
      if (!is_equal && src1.is_sym && src2.is_sym && !src1.is_lval && !src2.is_lval)
      {
        IRPoolSymref *a_ref = irop_get_symref_ex(ir, src1);
        IRPoolSymref *b_ref = irop_get_symref_ex(ir, src2);
        if (a_ref && b_ref && a_ref->sym == b_ref->sym && a_ref->addend == b_ref->addend)
          is_equal = 1;
      }
      /* Same-symbol deref read: CMP *sym, *sym where both operands load
       * from the same non-volatile global.  The reads see the same value,
       * so the comparison result is known (x==x, x>=x, etc.).  Safe for
       * integer types; skip floats (NaN != NaN). */
      if (!is_equal && src1.is_sym && src2.is_sym && src1.is_lval && src2.is_lval)
      {
        IRPoolSymref *a_ref = irop_get_symref_ex(ir, src1);
        IRPoolSymref *b_ref = irop_get_symref_ex(ir, src2);
        if (a_ref && b_ref && a_ref->sym == b_ref->sym &&
            a_ref->addend == b_ref->addend)
        {
          Sym *sym = a_ref->sym;
          int ttype = sym->type.t;
          int btype = ttype & VT_BTYPE;
          if (!(ttype & VT_VOLATILE) &&
              btype != VT_FLOAT && btype != VT_DOUBLE && btype != VT_LDOUBLE)
            is_equal = 1;
        }
      }
      if (!is_equal)
        continue;
    }
    else if ((vr1 >= 0) != (vr2 >= 0))
    {
      /* Asymmetric: one side is a vreg, the other is a non-vreg literal.
       * Resolve the vreg by chasing its single defining ASSIGN to its
       * literal value, then compare against the other side.  Without this,
       * const-var-prop's symref propagation produces e.g. `CMP V0, &f+5`
       * which the strict both-vregs path below would reject, even though
       * V0's only def is `ASSIGN V0 <-- &f+5`.
       *
       * Skip address-taken VARs: the value at the CMP may differ from the
       * defining ASSIGN's source if a store-through-pointer happened in
       * between. */
      int32_t v_vr = (vr1 >= 0) ? vr1 : vr2;
      IROperand other = (vr1 >= 0) ? src2 : src1;
      if (DC_IS_SINGLE_DEF(dc, dc_stride, v_vr))
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, v_vr);
        if (!interval || !interval->addrtaken)
        {
          int vdef = tcc_ir_find_defining_instruction(ir, v_vr, i);
          if (vdef >= 0)
          {
            IRQuadCompact *vdq = &ir->compact_instructions[vdef];
            if (vdq->op == TCCIR_OP_ASSIGN)
            {
              IROperand vs = tcc_ir_op_get_src1(ir, vdq);
              if (irop_get_vreg(vs) < 0)
              {
                /* Both literals now: compare exactly the same way the
                 * both_nonvreg branch above does. */
                is_equal = ir_opt_nonvreg_expr_equal(ir, vs, other);
                if (!is_equal && vs.is_sym && other.is_sym &&
                    !vs.is_lval && !other.is_lval)
                {
                  IRPoolSymref *a_ref = irop_get_symref_ex(ir, vs);
                  IRPoolSymref *b_ref = irop_get_symref_ex(ir, other);
                  if (a_ref && b_ref && a_ref->sym == b_ref->sym &&
                      a_ref->addend == b_ref->addend)
                    is_equal = 1;
                }
                if (!is_equal && irop_is_immediate(vs) && irop_is_immediate(other) &&
                    !vs.is_sym && !other.is_sym)
                {
                  is_equal = irop_get_imm64_ex(ir, vs) == irop_get_imm64_ex(ir, other);
                }
              }
            }
          }
        }
      }
      if (!is_equal)
        continue;
    }
    else
    {
      if (vr1 < 0 || vr2 < 0 || vr1 == vr2)
        continue;

      /* Both operands must have a single reaching definition */
      def1 = tcc_ir_find_defining_instruction(ir, vr1, i);
      def2 = tcc_ir_find_defining_instruction(ir, vr2, i);
      if (def1 < 0 || def2 < 0 || def1 == def2)
        continue;

      /* Try standard def equality (works for single-def vregs) */
      if (DC_IS_SINGLE_DEF(dc, dc_stride, vr1) && DC_IS_SINGLE_DEF(dc, dc_stride, vr2))
        is_equal = ir_opt_pure_def_equal(ir, def1, def2, 0);
    }

    /* Pattern match: both defs are ADD/SUB with the same immediate, and
     * their base operands resolve to the same value (e.g. both are
     * ASSIGN(GlobalSym) or LOAD of the same source). */
    if (!is_equal)
    {
      IRQuadCompact *dq1 = &ir->compact_instructions[def1];
      IRQuadCompact *dq2 = &ir->compact_instructions[def2];
      if (dq1->op == dq2->op && (dq1->op == TCCIR_OP_ADD || dq1->op == TCCIR_OP_SUB))
      {
        IROperand ds2_1 = tcc_ir_op_get_src2(ir, dq1);
        IROperand ds2_2 = tcc_ir_op_get_src2(ir, dq2);
        if (irop_is_immediate(ds2_1) && irop_is_immediate(ds2_2) &&
            irop_get_imm64_ex(ir, ds2_1) == irop_get_imm64_ex(ir, ds2_2))
        {
          IROperand base1 = tcc_ir_op_get_src1(ir, dq1);
          IROperand base2 = tcc_ir_op_get_src1(ir, dq2);
          int32_t bvr1 = irop_get_vreg(base1);
          int32_t bvr2 = irop_get_vreg(base2);

          if (bvr1 >= 0 && bvr2 >= 0)
          {
            /* Same base vreg → equal */
            if (bvr1 == bvr2)
              is_equal = 1;
            /* Different base vregs: check if they resolve to the same value */
            if (!is_equal)
            {
              int bd1 = tcc_ir_find_defining_instruction(ir, bvr1, def1);
              int bd2 = tcc_ir_find_defining_instruction(ir, bvr2, def2);
              if (bd1 >= 0 && bd2 >= 0)
              {
                IRQuadCompact *bdq1 = &ir->compact_instructions[bd1];
                IRQuadCompact *bdq2 = &ir->compact_instructions[bd2];
                /* Both ASSIGN/LOAD of the same source operand */
                if ((bdq1->op == TCCIR_OP_ASSIGN || bdq1->op == TCCIR_OP_LOAD) &&
                    (bdq2->op == TCCIR_OP_ASSIGN || bdq2->op == TCCIR_OP_LOAD))
                {
                  IROperand bs1 = tcc_ir_op_get_src1(ir, bdq1);
                  IROperand bs2 = tcc_ir_op_get_src1(ir, bdq2);
                  int32_t bsvr1 = irop_get_vreg(bs1);
                  int32_t bsvr2 = irop_get_vreg(bs2);
                  /* Same vreg source (e.g. both LOAD from V0) */
                  if (bsvr1 >= 0 && bsvr1 == bsvr2)
                    is_equal = 1;
                  /* Both non-vreg: compare structurally (e.g. same GlobalSym) */
                  if (!is_equal && bsvr1 < 0 && bsvr2 < 0)
                    is_equal = ir_opt_nonvreg_expr_equal(ir, bs1, bs2);
                  /* One is vreg (LOAD(V0)), other is constant (ASSIGN(GlobalSym)):
                   * resolve the vreg's value and compare with the constant. */
                  if (!is_equal && ((bsvr1 >= 0) != (bsvr2 >= 0)))
                  {
                    int vreg_side = (bsvr1 >= 0) ? bsvr1 : bsvr2;
                    IROperand const_side = (bsvr1 >= 0) ? bs2 : bs1;
                    int vreg_def_at = (bsvr1 >= 0) ? bd1 : bd2;
                    int vdef = tcc_ir_find_defining_instruction(ir, vreg_side, vreg_def_at);
                    if (vdef >= 0)
                    {
                      IRQuadCompact *vdq = &ir->compact_instructions[vdef];
                      if (vdq->op == TCCIR_OP_ASSIGN)
                      {
                        IROperand vs = tcc_ir_op_get_src1(ir, vdq);
                        if (irop_get_vreg(vs) < 0)
                          is_equal = ir_opt_nonvreg_expr_equal(ir, vs, const_side);
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    if (!is_equal)
      continue;

    /* Both operands compute the same expression — fold the CMP */
    IRQuadCompact *next = &ir->compact_instructions[i + 1];
    int folded = 0;
    if (next->op == TCCIR_OP_JUMPIF)
    {
      IROperand cond = tcc_ir_op_get_src1(ir, next);
      int tok = (int)irop_get_imm64_ex(ir, cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, next);
      if (result)
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, i + 1, jmp_dest);
      }
      else
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_NOP;
      }
      changes++;
      folded = 1;
    }
    else if (next->op == TCCIR_OP_SELECT)
    {
      IROperand select_cond = ir->iroperand_pool[next->operand_base + 3];
      int tok = (int)irop_get_imm64_ex(ir, select_cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand then_val = tcc_ir_op_get_src1(ir, next);
      IROperand else_val = tcc_ir_op_get_src2(ir, next);
      IROperand chosen = result ? then_val : else_val;
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i + 1, chosen);
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
      changes++;
      folded = 1;
    }
    else if (next->op == TCCIR_OP_SETIF)
    {
      IROperand cond = tcc_ir_op_get_src1(ir, next);
      int tok = (int)irop_get_imm64_ex(ir, cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand setif_dest = tcc_ir_op_get_dest(ir, next);
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_dest(ir, i + 1, setif_dest);
      tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, result, irop_get_btype(setif_dest)));
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
      changes++;
      folded = 1;
    }

    if (folded)
      ir_opt_setif_chain_cleanup(ir, def1, def2, vr1, vr2);
  }

  tcc_free(dc);

  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

/* Self-expression arithmetic identity fold: x/x→1, x%x→0 for integer
 * expressions where both operands provably read the same non-volatile
 * global value.  Safe because x/x is UB when x==0, so the compiler may
 * assume x!=0. */
int tcc_ir_opt_self_arith_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_DIV && q->op != TCCIR_OP_UDIV &&
        q->op != TCCIR_OP_IMOD && q->op != TCCIR_OP_UMOD)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    if (!src1.is_sym || !src1.is_lval || !src2.is_sym || !src2.is_lval)
      continue;

    IRPoolSymref *a_ref = irop_get_symref_ex(ir, src1);
    IRPoolSymref *b_ref = irop_get_symref_ex(ir, src2);
    if (!a_ref || !b_ref || a_ref->sym != b_ref->sym ||
        a_ref->addend != b_ref->addend)
      continue;

    Sym *sym = a_ref->sym;
    int ttype = sym->type.t;
    int btype = ttype & VT_BTYPE;
    if ((ttype & VT_VOLATILE) ||
        btype == VT_FLOAT || btype == VT_DOUBLE || btype == VT_LDOUBLE)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int is_div = (q->op == TCCIR_OP_DIV || q->op == TCCIR_OP_UDIV);
    int fold_val = is_div ? 1 : 0;
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_dest(ir, i, dest);
    tcc_ir_set_src1(ir, i, irop_make_imm32(-1, fold_val, irop_get_btype(dest)));
    tcc_ir_set_src2(ir, i, IROP_NONE);
    changes++;
  }

  return changes;
}

/* CMP Constant-Offset Fold
 *
 * Fold CMP+JUMPIF/SELECT when one operand is provably equal to the other
 * plus a known integer constant.  Pattern:
 *
 *   A <- B ADD #K       (or B SUB #K, symmetrical for swapped operand)
 *   CMP A, B
 *   JUMPIF cond ...     (or SELECT)
 *
 * Substituting A = B + K reduces the comparison to "K cond 0", which folds
 * unconditionally.  Catches the `for (i = opnum+1; i < opnum; ...)` shape
 * (gcc.c-torture/compile/pr31703.c) where GCC collapses the entire loop
 * body to a single `bx lr` via UB-exploit of signed overflow.
 *
 * Safety:
 *   - Only fires for signed conditions (<S, <=S, >=S, >S) and EQ/NE.  The
 *     signed-overflow-is-UB rule lets the optimizer assume `B + K` does not
 *     wrap, so the algebraic identity holds.  Unsigned conditions would
 *     require an overflow proof and are skipped.
 *   - Requires B's value at the CMP to match its value at the def of A:
 *     same defining instruction (or both PARAM / undefined).
 *   - Requires K to fit in int32 — keeps EQ/NE sound regardless of the
 *     ADD's bit width.  A 64-bit ADD with K = 2^32 would mod-wrap on a
 *     32-bit-truncated CMP, so we reject that case.
 */
int tcc_ir_opt_cmp_const_offset_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2 || n > 4000)
    return 0;

  for (int i = 0; i + 1 < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;

    IRQuadCompact *next = &ir->compact_instructions[i + 1];
    int is_jumpif = (next->op == TCCIR_OP_JUMPIF);
    int is_select = (next->op == TCCIR_OP_SELECT);
    if (!is_jumpif && !is_select)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int32_t vr1 = irop_get_vreg(src1);
    int32_t vr2 = irop_get_vreg(src2);
    if (vr1 < 0 || vr2 < 0 || vr1 == vr2)
      continue;

    /* Search both orientations: vr1 = vr2 ± K, then vr2 = vr1 ± K. */
    int64_t delta = 0;
    int found = 0;
    for (int swap = 0; swap < 2 && !found; swap++)
    {
      int32_t a = swap ? vr2 : vr1;
      int32_t b = swap ? vr1 : vr2;

      int def_a = tcc_ir_find_defining_instruction(ir, a, i);
      if (def_a < 0)
        continue;
      IRQuadCompact *dq = &ir->compact_instructions[def_a];
      if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB)
        continue;

      IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
      IROperand ds2 = tcc_ir_op_get_src2(ir, dq);

      /* Match `a = b + K` (or `a = K + b`, commutative ADD). */
      int64_t k = 0;
      if (irop_get_vreg(ds1) == b && irop_is_immediate(ds2))
        k = irop_get_imm64_ex(ir, ds2);
      else if (dq->op == TCCIR_OP_ADD && irop_get_vreg(ds2) == b && irop_is_immediate(ds1))
        k = irop_get_imm64_ex(ir, ds1);
      else
        continue;

      if (dq->op == TCCIR_OP_SUB)
        k = -k;
      if (k == 0)
        continue;
      /* Restrict to int32-fitting immediates: avoids unsoundness on a
       * 32-bit-truncated ADD/SUB whose 64-bit immediate has zero low half. */
      if (k > (int64_t)INT32_MAX || k < (int64_t)INT32_MIN)
        continue;

      /* B must hold the same value at the CMP as at def_a. */
      int b_def_at_use = tcc_ir_find_defining_instruction(ir, b, i);
      int b_def_at_def = tcc_ir_find_defining_instruction(ir, b, def_a);
      if (b_def_at_use != b_def_at_def)
        continue;
      /* Address-taken B can be mutated through aliasing stores / calls. */
      if (ir_opt_vreg_address_taken_between(ir, b, def_a, i))
        continue;

      /* delta = vr1 - vr2.  swap=0 → vr1 = vr2 + k → delta = k.
       *                     swap=1 → vr2 = vr1 + k → delta = -k. */
      delta = swap ? -k : k;
      found = 1;
    }

    if (!found)
      continue;

    IROperand cond_op = is_jumpif
      ? tcc_ir_op_get_src1(ir, next)
      : ir->iroperand_pool[next->operand_base + 3];
    int tok = (int)irop_get_imm64_ex(ir, cond_op);

    /* Signed and EQ/NE only.  Unsigned needs an overflow proof. */
    int is_signed_cmp = (tok == 0x9c || tok == 0x9d || tok == 0x9e || tok == 0x9f);
    int is_eq_ne = (tok == 0x94 || tok == 0x95);
    if (!is_signed_cmp && !is_eq_ne)
      continue;

    int result = evaluate_compare_condition(delta, 0, tok);
    if (result < 0)
      continue;

    if (is_jumpif)
    {
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, next);
      if (result)
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, i + 1, jmp_dest);
      }
      else
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_NOP;
      }
      changes++;
    }
    else /* SELECT */
    {
      IROperand then_val = tcc_ir_op_get_src1(ir, next);
      IROperand else_val = tcc_ir_op_get_src2(ir, next);
      IROperand chosen = result ? then_val : else_val;
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i + 1, chosen);
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
      changes++;
    }
  }

  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

/* VAR self-update chain fold: combine consecutive `V <- V ± Ck` updates of
 * the same VAR/PARAM into a single `V <- V ± sum`.  Produced by loop
 * unrolling of `for (i=0; i<N; i++) p++` patterns, where each iteration
 * becomes a self-update ADD.  add_reassoc deliberately bails on self-update
 * chains because rewriting `V = V + C1; V = V + C2` to use def_src1 reads
 * V at the wrong point (after def_q's write).  This pass handles the
 * self-update case by NOPping the intermediate defs, which is sound when
 * V's intermediate values have no observers between the chain steps. */
int tcc_ir_opt_var_self_add_chain_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  /* Bail when the function contains IJUMP — address-taken labels (`&&label`)
   * are NOT marked `is_jump_target`, so the scan would happily walk past
   * them and incorrectly fold across computed-goto landing points (e.g.
   * `goto *p; l_a: c++; l_b: c++;` → wrong: c always += 2). */
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(src2))
      continue;

    int32_t v = irop_get_vreg(dest);
    if (v < 0)
      continue;
    int vtype = TCCIR_DECODE_VREG_TYPE(v);
    if (vtype != TCCIR_VREG_TYPE_VAR && vtype != TCCIR_VREG_TYPE_PARAM)
      continue;
    if (irop_get_vreg(src1) != v)
      continue;
    /* dest is unconditionally a VAR/PARAM def; src1 must read the same VAR
     * (no STACKOFF/Addr deref form — that would be a memory access). */
    /* src1 reads V (lvalue, often tag=STACKOFF carrying V's spill home);
     * dest writes V (tag=VREG).  Same vreg already verified above; no
     * further tag/offset constraint — they refer to the same VAR. */

    int btype = irop_get_btype(src2);
    int64_t sum = (q->op == TCCIR_OP_SUB) ? -irop_get_imm64_ex(ir, src2)
                                          :  irop_get_imm64_ex(ir, src2);
    int last_idx = i;
    int last_btype = btype;
#define VSA_MAX_CHAIN 64
    int chain_idx[VSA_MAX_CHAIN];
    int chain_count = 1;
    chain_idx[0] = i;

    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;
      if (qj->is_jump_target)
        break;
      /* Control flow / calls / returns: stop the chain — V's value escapes. */
      if (qj->op == TCCIR_OP_JUMP || qj->op == TCCIR_OP_JUMPIF ||
          qj->op == TCCIR_OP_IJUMP || qj->op == TCCIR_OP_SWITCH_TABLE ||
          qj->op == TCCIR_OP_RETURNVALUE || qj->op == TCCIR_OP_RETURNVOID ||
          qj->op == TCCIR_OP_FUNCCALLVAL || qj->op == TCCIR_OP_FUNCCALLVOID ||
          qj->op == TCCIR_OP_FUNCPARAMVAL || qj->op == TCCIR_OP_FUNCPARAMVOID)
        break;

      /* Detect any read or write of V in qj. */
      int touches_v = 0;
      int writes_v = 0;
      if (irop_config[qj->op].has_dest)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, qj);
        if (irop_get_vreg(jd) == v)
        {
          touches_v = 1;
          /* For STORE/STORE_INDEXED dest is the address (a use, not a def).
           * Treat that as a read, not a write. */
          if (qj->op != TCCIR_OP_STORE && qj->op != TCCIR_OP_STORE_INDEXED &&
              qj->op != TCCIR_OP_STORE_POSTINC)
            writes_v = 1;
        }
      }
      if (!touches_v && irop_config[qj->op].has_src1 &&
          irop_get_vreg(tcc_ir_op_get_src1(ir, qj)) == v)
        touches_v = 1;
      if (!touches_v && irop_config[qj->op].has_src2 &&
          irop_get_vreg(tcc_ir_op_get_src2(ir, qj)) == v)
        touches_v = 1;

      if (!touches_v)
        continue; /* unrelated instruction, skip past */

      /* qj touches V. To extend the chain, qj must be `V <- V ± #C`. */
      if (!writes_v)
        break; /* read of V's intermediate value — chain stops here */
      if (qj->op != TCCIR_OP_ADD && qj->op != TCCIR_OP_SUB)
        break;
      IROperand jdest = tcc_ir_op_get_dest(ir, qj);
      IROperand jsrc1 = tcc_ir_op_get_src1(ir, qj);
      IROperand jsrc2 = tcc_ir_op_get_src2(ir, qj);
      if (!irop_is_immediate(jsrc2))
        break;
      if (irop_get_vreg(jdest) != v || irop_get_vreg(jsrc1) != v)
        break;
      /* Same vreg confirmed above; no further tag check needed. */

      int64_t c = (qj->op == TCCIR_OP_SUB) ? -irop_get_imm64_ex(ir, jsrc2)
                                           :  irop_get_imm64_ex(ir, jsrc2);
      sum += c;
      last_idx = j;
      last_btype = irop_get_btype(jsrc2);
      if (chain_count >= VSA_MAX_CHAIN)
        break;
      chain_idx[chain_count++] = j;
    }

    if (last_idx == i)
      continue;
    if (sum != (int32_t)sum)
      continue;

    LOG_IR_GEN("OPTIMIZE: VAR self-add chain fold [%d..%d] sum=%lld",
               i, last_idx, (long long)sum);

    IRQuadCompact *qlast = &ir->compact_instructions[last_idx];
    if (sum == 0)
    {
      /* Replace with ASSIGN V = V (effectively a NOP, will get cleaned). */
      qlast->op = TCCIR_OP_ASSIGN;
      IROperand v_lval = src1;
      tcc_ir_set_src1(ir, last_idx, v_lval);
      tcc_ir_set_src2(ir, last_idx, IROP_NONE);
    }
    else
    {
      qlast->op = (sum < 0) ? TCCIR_OP_SUB : TCCIR_OP_ADD;
      int64_t abs_sum = (sum < 0) ? -sum : sum;
      tcc_ir_set_src2(ir, last_idx, irop_make_imm32(-1, (int32_t)abs_sum, last_btype));
      /* dest and src1 already V */
    }

    /* NOP only the matched chain entries (not unrelated instructions between
     * them).  Skip the LAST entry, which we rewrote in place above. */
    for (int k = 0; k < chain_count - 1; k++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[chain_idx[k]];
      if (qj->op != TCCIR_OP_NOP)
        qj->op = TCCIR_OP_NOP;
    }
    changes++;
    /* Do NOT skip to last_idx — there may be other chains (different V)
     * interleaved between this chain's elements that still need folding. */
  }
#undef VSA_MAX_CHAIN

  return changes;
}

typedef struct StackAddrValue
{
  int off;
  int is_param;
} StackAddrValue;

static int ir_resolve_stack_addr_value_ex(TCCIRState *ir, IROperand op, int at_idx,
                                          StackAddrValue *out, int depth);

static int ir_has_backward_control_flow(TCCIRState *ir)
{
  int n = ir ? ir->next_instruction_index : 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (target >= 0 && target <= i)
        return 1;
    }
    else if (q->op == TCCIR_OP_IJUMP)
    {
      return 1;
    }
  }

  return 0;
}

/* Resolve an operand at instruction `at_idx` to a stack-frame offset, if
 * provably constant.  Recognized shapes:
 *   - direct address operand:  `Addr[StackLoc[X]]` → X
 *   - vreg V with same-BB defs of the form `V = Addr[StackLoc[X]]` followed
 *     by zero or more `V = V ± const` self-updates → X + sum(const).
 * Returns 1 and writes *out_off on success, 0 otherwise.
 *
 * Conservative: stops at any other def of V or at any jump_target between
 * the def and `at_idx` (don't cross BB boundaries / merge points). */
static int ir_resolve_stack_addr_value(TCCIRState *ir, IROperand op, int at_idx, int *out_off)
{
  StackAddrValue value;
  if (!ir_resolve_stack_addr_value_ex(ir, op, at_idx, &value, 0))
    return 0;
  *out_off = value.off;
  return 1;
}

/* Presence map of vregs that have at least one "def" as seen by the backward
 * walk in ir_resolve_stack_addr_value_ex (any has_dest op other than
 * STORE/STORE_INDEXED/STORE_POSTINC/FUNCPARAMVAL writing that vreg).  Built
 * once per driving pass so the resolver can answer "this vreg has no def — the
 * walk would scan to the start and return 0" in O(1).  Indexed by
 * pos*3 + (type-1); a vreg outside the map's range is treated as absent.
 * Returning early only for the no-def case keeps the walk's merge-crossing
 * semantics intact (a no-def walk can only ever return 0). */
static uint8_t *sav_def_present;
static int sav_def_present_maxpos = -1;

static int sav_is_def_op(int op)
{
  return op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED &&
         op != TCCIR_OP_STORE_POSTINC && op != TCCIR_OP_FUNCPARAMVAL;
}

static void sav_build_def_map(TCCIRState *ir)
{
  int n = ir ? ir->next_instruction_index : 0;
  sav_def_present = NULL;
  sav_def_present_maxpos = -1;
  int maxpos = -1;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || !sav_is_def_op(q->op))
      continue;
    int32_t dvr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (dvr < 0)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos > maxpos)
      maxpos = pos;
  }
  if (maxpos < 0)
    return;
  sav_def_present = (uint8_t *)tcc_mallocz((size_t)(maxpos + 1) * 3);
  sav_def_present_maxpos = maxpos;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || !sav_is_def_op(q->op))
      continue;
    int32_t dvr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (dvr < 0)
      continue;
    int type = TCCIR_DECODE_VREG_TYPE(dvr);
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (type >= 1 && type <= 3)
      sav_def_present[pos * 3 + (type - 1)] = 1;
  }
}

static void sav_free_def_map(void)
{
  tcc_free(sav_def_present);
  sav_def_present = NULL;
  sav_def_present_maxpos = -1;
}

/* Returns 1 if vr definitely has no qualifying def (walk would return 0). */
static int sav_vreg_has_no_def(int32_t vr)
{
  if (!sav_def_present)
    return 0; /* map not built — be safe, let the walk run */
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (type < 1 || type > 3 || pos > sav_def_present_maxpos)
    return 1; /* outside any recorded def */
  return sav_def_present[pos * 3 + (type - 1)] == 0;
}

static int ir_resolve_stack_addr_value_ex(TCCIRState *ir, IROperand op, int at_idx,
                                          StackAddrValue *out, int depth)
{
  if (!ir || !out || depth > 12)
    return 0;

  /* Direct stack address (Addr[StackLoc[X]], i.e. STACKOFF tag, no vreg, not lval). */
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_get_vreg(op) == -1 && !op.is_lval)
  {
    out->off = (int)irop_get_imm64_ex(ir, op);
    out->is_param = op.is_param;
    return 1;
  }

  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  /* Fast reject: a vreg with no def anywhere can only make the walk below
   * scan to the start and return 0 — skip the scan. */
  if (sav_vreg_has_no_def(vr))
    return 0;

  int saw_merge_at = at_idx;
  for (int j = at_idx - 1; j >= 0; j--)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Conservative: stop crossing merges (instruction with is_jump_target set).
     * We allow the very first step (j == at_idx-1) to look back across our
     * own CMP/BB head, but no further. */
    if (q->is_jump_target && j != saw_merge_at - 1)
      return 0;
    saw_merge_at = j;

    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) != vr)
      continue;
    /* STORE-style ops carry an address-of-write in dest, not a def. */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
      continue;
    /* FUNCPARAMVAL dest carries the param value (a use, not a def). */
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
      continue;

    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      return ir_resolve_stack_addr_value_ex(ir, src, j, out, depth + 1);
    }
    if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      StackAddrValue base;
      int64_t c;
      if (!ir_resolve_stack_addr_value_ex(ir, s1, j, &base, depth + 1))
        return 0;
      if (!irop_is_immediate(s2))
        return 0;
      c = irop_get_imm64_ex(ir, s2);
      if (q->op == TCCIR_OP_SUB)
        c = -c;
      c += base.off;
      if (c != (int32_t)c)
        return 0;
      out->off = (int)c;
      out->is_param = base.is_param;
      return 1;
    }
    /* Some other op writes vr — give up. */
    return 0;
  }
  return 0;
}

/* Canonicalize dereferences through pointers whose value is a known stack
 * address, and fold simple stack-address arithmetic.  This exposes ordinary
 * StackLoc STORE/LOAD/CMP patterns to the existing store-load forwarding and
 * branch folders.
 *
 * Example:
 *   T0 = Addr[StackLoc[-16]]
 *   T0***DEREF*** = #10       -> StackLoc[-16] = #10
 *   T1 = (T0 + 1) - T0        -> T1 = #1
 */
int tcc_ir_opt_stack_addr_simplify(TCCIRState *ir)
{
  int n = ir ? ir->next_instruction_index : 0;
  int changes = 0;

  if (ir_has_backward_control_flow(ir))
    return 0;

  sav_build_def_map(ir);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (dest.is_lval &&
          !(irop_get_tag(dest) == IROP_TAG_STACKOFF && dest.is_local && !dest.is_llocal))
      {
        StackAddrValue addr;
        if (ir_resolve_stack_addr_value_ex(ir, dest, i, &addr, 0))
        {
          IROperand direct = irop_make_stackoff(-1, addr.off, 1, 0, addr.is_param, irop_get_btype(dest));
          direct.is_unsigned = dest.is_unsigned;
          tcc_ir_set_dest(ir, i, direct);
          changes++;
        }
      }
      continue;
    }

    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && irop_config[q->op].has_src1)
    {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (src.is_lval &&
          !(irop_get_tag(src) == IROP_TAG_STACKOFF && src.is_local && !src.is_llocal))
      {
        StackAddrValue addr;
        if (ir_resolve_stack_addr_value_ex(ir, src, i, &addr, 0))
        {
          IROperand direct = irop_make_stackoff(-1, addr.off, 1, 0, addr.is_param, irop_get_btype(src));
          direct.is_unsigned = src.is_unsigned;
          tcc_ir_set_src1(ir, i, direct);
          changes++;
        }
      }
    }

    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_config[q->op].has_dest)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      StackAddrValue a1, a2;
      int folded = 0;
      int64_t result = 0;

      if (q->op == TCCIR_OP_SUB &&
          ir_resolve_stack_addr_value_ex(ir, src1, i, &a1, 0) &&
          ir_resolve_stack_addr_value_ex(ir, src2, i, &a2, 0) &&
          a1.is_param == a2.is_param)
      {
        result = (int64_t)a1.off - (int64_t)a2.off;
        folded = 1;
      }

      if (folded && result == (int32_t)result)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, irop_get_btype(dest)));
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
    }
  }

  sav_free_def_map();
  return changes;
}

/* Fold CMP whose two operands provably resolve to the same stack-frame
 * offset (one side a vreg holding `Addr[StackLoc[X]] + N`, the other side
 * a literal `Addr[StackLoc[X+N]]`).  Rewrites the following JUMPIF/SELECT
 * by precomputing the comparison result, matching `cmp_expr_fold`'s
 * downstream logic. */
int tcc_ir_opt_cmp_stack_addr_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  /* IJUMP safety: address-taken labels (`&&label`) aren't marked
   * is_jump_target, so the backward def-walk could cross a target
   * unaware. See [[project_global_sl_fwd_ijump_safety]]. */
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  }

  sav_build_def_map(ir);

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;

    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);

    int off1, off2;
    if (!ir_resolve_stack_addr_value(ir, s1, i, &off1))
      continue;
    if (!ir_resolve_stack_addr_value(ir, s2, i, &off2))
      continue;
    if (off1 != off2)
      continue; /* could fold to !equal too, but be conservative */

    IRQuadCompact *next = &ir->compact_instructions[i + 1];
    if (next->op == TCCIR_OP_JUMPIF)
    {
      IROperand cond = tcc_ir_op_get_src1(ir, next);
      int tok = (int)irop_get_imm64_ex(ir, cond);
      int result = evaluate_compare_condition(0, 0, tok); /* equal-equal */
      if (result < 0)
        continue;
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, next);
      LOG_IR_GEN("OPTIMIZE: CMP stack-addr fold at %d (off=%d, %s)",
                 i, off1, result ? "taken" : "not taken");
      if (result)
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, i + 1, jmp_dest);
      }
      else
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_NOP;
      }
      changes++;
    }
    else if (next->op == TCCIR_OP_SELECT)
    {
      IROperand select_cond = ir->iroperand_pool[next->operand_base + 3];
      int tok = (int)irop_get_imm64_ex(ir, select_cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand then_val = tcc_ir_op_get_src1(ir, next);
      IROperand else_val = tcc_ir_op_get_src2(ir, next);
      IROperand chosen = result ? then_val : else_val;
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i + 1, chosen);
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
      changes++;
    }
  }
  sav_free_def_map();
  return changes;
}

int tcc_ir_opt_cmp_stack_addr_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_stack_addr_fold(ctx->ir); }

int tcc_ir_opt_var_self_add_chain_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_var_self_add_chain_fold(ctx->ir); }

/* Single-Value Temp Propagation
 *
 * If ALL definitions of a TEMP are ASSIGN/LOAD of the same immediate
 * constant, replace every use of that TEMP with the constant.  This
 * handles phi-like merges where both arms assign the same value
 * (e.g. after VRP folds a conditional set). */
int tcc_ir_opt_single_value_tmp(TCCIRState *ir)
{
#define SVT_MAX_TEMPS 128
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  int max_tmp = -1;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP) {
      int pos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (pos > max_tmp)
        max_tmp = pos;
    }
  }
  if (max_tmp < 0 || max_tmp >= SVT_MAX_TEMPS)
    return 0;

  int count = max_tmp + 1;
  uint8_t state[SVT_MAX_TEMPS];
  int32_t vals[SVT_MAX_TEMPS];
  memset(state, 0, count);

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos >= count) continue;

    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) &&
        state[pos] != 2) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_is_immediate(s) && !s.is_lval &&
          irop_get_btype(s) == IROP_BTYPE_INT32) {
        int32_t v = (int32_t)irop_get_imm64_ex(ir, s);
        if (state[pos] == 0) {
          state[pos] = 1;
          vals[pos] = v;
        } else if (vals[pos] != v) {
          state[pos] = 2;
        }
        continue;
      }
    }
    state[pos] = 2;
  }

  int changes = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_RETURNVALUE)
      continue;
    for (int k = 0; k < 2; k++) {
      IROperand op = k == 0 ? tcc_ir_op_get_src1(ir, q)
                            : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr < 0 || op.is_lval)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos >= count || state[pos] != 1)
        continue;
      IROperand imm = irop_make_imm32(-1, vals[pos], IROP_BTYPE_INT32);
      if (k == 0)
        tcc_ir_set_src1(ir, i, imm);
      else
        tcc_ir_set_src2(ir, i, imm);
      changes++;
    }
  }

  if (changes) {
    for (int i = 0; i < n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
        continue;
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (pos < count && state[pos] == 1) {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }
    changes += tcc_ir_opt_dce(ir);
  }

  if (changes) {
    n = ir->next_instruction_index;
    int64_t ret_val = 0;
    int ret_btype = 0;
    int ret_idx = -1;
    int all_same_ret = 1;
    int has_side_effect = 0;
    for (int i = 0; i < n && all_same_ret && !has_side_effect; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      switch (q->op) {
      case TCCIR_OP_RETURNVALUE: {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (!irop_is_immediate(s)) { all_same_ret = 0; break; }
        int64_t v = irop_get_imm64_ex(ir, s);
        if (ret_idx < 0) {
          ret_val = v; ret_btype = irop_get_btype(s); ret_idx = i;
        } else if (v != ret_val) { all_same_ret = 0; }
        break;
      }
      case TCCIR_OP_STORE: case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_FUNCCALLVAL: case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_VLA_ALLOC: case TCCIR_OP_VLA_SP_SAVE:
      case TCCIR_OP_VLA_SP_RESTORE: case TCCIR_OP_TRAP:
      case TCCIR_OP_RETURNVOID:
        has_side_effect = 1; break;
      default: break;
      }
    }
    if (all_same_ret && !has_side_effect && ret_idx >= 0) {
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_RETURNVALUE)
          continue;
        q->op = TCCIR_OP_NOP;
        changes++;
      }
      if (ret_idx > 0) {
        ir->compact_instructions[0].op = TCCIR_OP_RETURNVALUE;
        IROperand rv = irop_make_imm32(-1, (int32_t)ret_val, ret_btype);
        tcc_ir_set_src1(ir, 0, rv);
        ir->compact_instructions[ret_idx].op = TCCIR_OP_NOP;
        changes++;
      }
      changes += tcc_ir_opt_dce(ir);
    }
  }
  return changes;
#undef SVT_MAX_TEMPS
}

int tcc_ir_opt_single_value_tmp_ex(IROptCtx *ctx) { return tcc_ir_opt_single_value_tmp(ctx->ir); }

int tcc_ir_opt_const_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_const_prop(ctx->ir); }
int tcc_ir_opt_const_prop_tmp_ex(IROptCtx *ctx) { return tcc_ir_opt_const_prop_tmp(ctx->ir); }
int tcc_ir_opt_const_var_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_const_var_prop(ctx->ir); }
int tcc_ir_opt_global_init_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_global_init_prop(ctx->ir); }
int tcc_ir_opt_symref_const_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_symref_const_prop(ctx->ir); }
int tcc_ir_opt_value_tracking_ex(IROptCtx *ctx) { return tcc_ir_opt_value_tracking(ctx->ir); }
int tcc_ir_opt_add_reassoc_ex(IROptCtx *ctx) { return tcc_ir_opt_add_reassoc(ctx->ir); }
int tcc_ir_opt_cmp_expr_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_expr_fold(ctx->ir); }
int tcc_ir_opt_self_arith_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_self_arith_fold(ctx->ir); }
int tcc_ir_opt_cmp_const_offset_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_const_offset_fold(ctx->ir); }
int tcc_ir_opt_const_string_calls_ex(IROptCtx *ctx) { return tcc_ir_opt_const_string_calls(ctx->ir); }
int tcc_ir_opt_self_copy_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_self_copy_elim(ctx->ir); }
