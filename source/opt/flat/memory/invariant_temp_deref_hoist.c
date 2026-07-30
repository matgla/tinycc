/*
 *  TCC IR - Invariant TEMP-deref hoist (pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include <limits.h>

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "opt_loop_utils.h"


extern int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q);

/* ============================================================================
 * Invariant TEMP-deref Hoist (tcc_ir_opt_invariant_temp_deref_hoist)
 * ============================================================================
 *
 * Companion to tcc_ir_opt_invariant_global_load_hoist.  After that pass has
 * collapsed cross-BB reloads of a global pointer T into a single
 *   T = ASSIGN GlobalSym(P)***DEREF***
 * the body of the function may still contain many `op T***DEREF***` uses
 * (e.g. `CMP T***DEREF***, X` repeated per iteration of an unrolled chain).
 * Each one re-emits an LDR before the operation.
 *
 * For TEMPs that are defined once (load of a pointer from memory) and never
 * redefined, this pass inserts a single ASSIGN T_v <- T***DEREF*** right
 * after T's definition and rewrites every `T***DEREF***` use within the same
 * clobber-free single-entry region to T_v (non-lval).  Subsequent codegen
 * keeps T_v in a register, removing the per-use LDR.
 *
 * Safety conditions match the global LOAD hoist pass: forward control flow
 * only, no aliasing stores or calls between def and last use, and no jump
 * skipping over the def to land in the use range.
 */
static int optmem_temp_copy_btype_compat(int a, int b)
{
  int a_word = (a == IROP_BTYPE_INT32 || a == IROP_BTYPE_STRUCT || a == IROP_BTYPE_FUNC);
  int b_word = (b == IROP_BTYPE_INT32 || b == IROP_BTYPE_STRUCT || b == IROP_BTYPE_FUNC);
  return a == b || (a_word && b_word);
}

static int32_t optmem_resolve_temp_copy_root(int32_t vr, int use_idx,
                                             const int32_t *copy_src,
                                             const int *copy_def, int max_tmp)
{
  for (int depth = 0; depth < 16; depth++)
  {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return vr;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos < 0 || pos >= max_tmp)
      return vr;
    int32_t src = copy_src[pos];
    if (src < 0 || copy_def[pos] < 0 || use_idx <= copy_def[pos])
      return vr;
    vr = src;
  }
  return vr;
}

int tcc_ir_opt_invariant_temp_deref_hoist(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  /* Same control-flow preconditions as the global-load hoist. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
      case TCCIR_OP_IJUMP:
      case TCCIR_OP_SWITCH_TABLE:
      case TCCIR_OP_SETJMP:
      case TCCIR_OP_LONGJMP:
      case TCCIR_OP_NL_SETJMP:
      case TCCIR_OP_NL_LONGJMP:
      case TCCIR_OP_INLINE_ASM:
      case TCCIR_OP_ASM_INPUT:
      case TCCIR_OP_ASM_OUTPUT:
      case TCCIR_OP_BUILTIN_APPLY:
      case TCCIR_OP_BUILTIN_APPLY_ARGS:
      case TCCIR_OP_BUILTIN_RETURN:
        return 0;
      case TCCIR_OP_JUMP:
      case TCCIR_OP_JUMPIF:
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int target = (int)irop_get_imm64_ex(ir, dest);
        if (target <= i)
          return 0;
        break;
      }
      default:
        break;
    }
  }

  /* Clobber map (writes that could alias any pointer-derefed memory). */
  unsigned char *clobber = tcc_mallocz((size_t)n);
  if (!clobber)
    return 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_BLOCK_COPY:
      case TCCIR_OP_TRAP:
        clobber[i] = 1;
        break;
      case TCCIR_OP_STORE:
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        /* A direct store to any GlobalSym could alias an unknown pointer.
         * A store to a local stack slot cannot. */
        if (dest.is_sym && dest.is_lval)
          clobber[i] = 1;
        else if (!dest.is_local)
          clobber[i] = 1;
        break;
      }
      default:
        break;
    }
  }

  int max_tmp = ir->next_temporary_variable;
  int32_t *copy_src = NULL;
  int *copy_def = NULL;
  int *def_count = NULL;
  if (max_tmp > 0)
  {
    copy_src = tcc_mallocz((size_t)max_tmp * sizeof(int32_t));
    copy_def = tcc_mallocz((size_t)max_tmp * sizeof(int));
    def_count = tcc_mallocz((size_t)max_tmp * sizeof(int));
    for (int p = 0; p < max_tmp; p++)
    {
      copy_src[p] = -1;
      copy_def[p] = -1;
    }
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (dvr < 0 || dest.is_lval ||
          TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (dpos < 0 || dpos >= max_tmp)
        continue;
      def_count[dpos]++;
      copy_def[dpos] = i;
      copy_src[dpos] = -1;
      if (q->op != TCCIR_OP_ASSIGN || !irop_config[q->op].has_src1)
        continue;
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t svr = irop_get_vreg(src1);
      if (svr >= 0 && !src1.is_lval && !src1.is_local && !src1.is_llocal &&
          TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP &&
          optmem_temp_copy_btype_compat(irop_get_btype(dest), irop_get_btype(src1)))
        copy_src[dpos] = svr;
    }
    for (int p = 0; p < max_tmp; p++)
      if (def_count[p] != 1)
        copy_src[p] = -1;
  }

#define ITDH_MAX_CANDS 16
  struct
  {
    int32_t temp_vr;
    int def_idx;
    int first_use;
    int last_use;
    int use_count;
    int btype;
    int is_unsigned;
    int32_t hoist_vr;
    int hoist_idx; /* position of the inserted ASSIGN — its own src must not be rewritten */
  } cands[ITDH_MAX_CANDS];
  int num_cands = 0;

  /* Pass 1: collect candidate TEMPs - defined by an ASSIGN/LOAD whose source
   * operand is an lval (so the TEMP holds a value loaded from memory).  These
   * are exactly the "loaded pointer" TEMPs whose subsequent T***DEREF*** uses
   * become per-iteration LDRs in the backend. */
  for (int i = 0; i < n && num_cands < ITDH_MAX_CANDS; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
      continue;
    if (!irop_config[q->op].has_dest || !irop_config[q->op].has_src1)
      continue;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!src1.is_lval || dest.is_lval)
      continue;
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    cands[num_cands].temp_vr = dest_vr;
    cands[num_cands].def_idx = i;
    cands[num_cands].first_use = -1;
    cands[num_cands].last_use = -1;
    cands[num_cands].use_count = 0;
    cands[num_cands].btype = -1;
    cands[num_cands].is_unsigned = 0;
    cands[num_cands].hoist_vr = -1;
    cands[num_cands].hoist_idx = -1;
    num_cands++;
  }

  if (num_cands == 0)
  {
    tcc_free(clobber);
    tcc_free(copy_src);
    tcc_free(copy_def);
    tcc_free(def_count);
    return 0;
  }

  /* Pass 2: scan IR.  For each candidate, check redefinitions and collect
   * lval-deref uses.  Mark candidates whose TEMP is redefined as invalid. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Redefinition check (skip the candidate's own def). */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr >= 0 && !d.is_lval)
      {
        for (int c = 0; c < num_cands; c++)
        {
          if (cands[c].use_count < 0)
            continue;
          if (cands[c].temp_vr == dvr && i != cands[c].def_idx)
            cands[c].use_count = -1; /* mark invalid */
        }
      }
    }

    /* Operand scan for lval uses of the candidate TEMP. */
    for (int s = 0; s < 3; s++)
    {
      int has;
      IROperand op;
      if (s == 0)
      {
        has = irop_config[q->op].has_dest;
        if (!has)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (s == 1)
      {
        has = irop_config[q->op].has_src1;
        if (!has)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        has = irop_config[q->op].has_src2;
        if (!has)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (!op.is_lval)
        continue;
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      vr = optmem_resolve_temp_copy_root(vr, i, copy_src, copy_def, max_tmp);
      for (int c = 0; c < num_cands; c++)
      {
        if (cands[c].use_count < 0)
          continue;
        if (cands[c].temp_vr != vr)
          continue;
        if (i <= cands[c].def_idx)
          continue; /* the def itself - ignore */
        int btype = irop_get_btype(op);
        if (cands[c].first_use < 0)
        {
          cands[c].first_use = i;
          cands[c].btype = btype;
          cands[c].is_unsigned = op.is_unsigned;
        }
        else if (btype != cands[c].btype)
        {
          cands[c].use_count = -1;
          continue;
        }
        cands[c].last_use = i;
        cands[c].use_count++;
      }
    }
  }

  /* Pass 3: safety filter — drop candidates whose use range has a clobber or
   * whose anchor doesn't dominate the uses (some external jump skips the def). */
  for (int c = 0; c < num_cands; c++)
  {
    if (cands[c].use_count < 2)
      continue;
    int anchor = cands[c].def_idx;
    int last = cands[c].last_use;
    int safe = 1;
    for (int j = anchor + 1; j <= last; j++)
    {
      if (clobber[j])
      {
        safe = 0;
        break;
      }
    }
    if (safe)
    {
      for (int j = 0; j < anchor; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int tgt = (int)irop_get_imm64_ex(ir, jdest);
        if (tgt > anchor && tgt <= last)
        {
          safe = 0;
          break;
        }
      }
    }
    if (!safe)
      cands[c].use_count = -1;
  }

  tcc_free(clobber);

  /* Pass 4: insert hoist ASSIGNs.  Process in REVERSE order of def_idx so
   * earlier-positioned candidates' indices are unaffected by later insertions
   * (each insertion shifts only positions at-or-after itself). */
  for (int pass = 0; pass < num_cands; pass++)
  {
    int best = -1;
    int best_idx = -1;
    for (int c = 0; c < num_cands; c++)
    {
      if (cands[c].use_count < 2 || cands[c].hoist_vr >= 0)
        continue;
      if (cands[c].def_idx > best_idx)
      {
        best_idx = cands[c].def_idx;
        best = c;
      }
    }
    if (best < 0)
      break;

    int c = best;
    int32_t t_new = tcc_ir_vreg_alloc_temp(ir);
    if (t_new < 0)
      continue;

    IROperand new_dest = irop_make_vreg(t_new, cands[c].btype);
    new_dest.is_unsigned = cands[c].is_unsigned;
    IROperand new_src = irop_make_vreg(cands[c].temp_vr, cands[c].btype);
    new_src.is_lval = 1;
    new_src.is_unsigned = cands[c].is_unsigned;

    if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
      tcc_ir_pool_ensure(ir, 2);

    IRQuadCompact new_q = {0};
    new_q.op = TCCIR_OP_ASSIGN;
    new_q.operand_base = tcc_ir_pool_add(ir, new_dest);
    tcc_ir_pool_add(ir, new_src);

    int insert_pos = cands[c].def_idx + 1;
    if (gsym_cse_insert_before(ir, insert_pos, &new_q) < 0)
      continue;

    cands[c].hoist_vr = t_new;
    cands[c].hoist_idx = insert_pos;
    /* Other candidates' positions at-or-after insert_pos shift by 1. */
    for (int c2 = 0; c2 < num_cands; c2++)
    {
      if (c2 == c)
        continue;
      if (cands[c2].def_idx >= insert_pos)
        cands[c2].def_idx++;
      if (cands[c2].first_use >= insert_pos)
        cands[c2].first_use++;
      if (cands[c2].last_use >= insert_pos)
        cands[c2].last_use++;
      if (cands[c2].hoist_idx >= 0 && cands[c2].hoist_idx >= insert_pos)
        cands[c2].hoist_idx++;
    }
    n++;
  }

  /* Pass 5: rewrite all `T***DEREF***` uses to the hoisted TEMP (non-lval).
   * Rewriting is by vreg identity so it's independent of position shifts.
   * Skip the inserted ASSIGN itself (whose src1 is the only legitimate
   * `T_old***DEREF***` use that must remain). */
  for (int c = 0; c < num_cands; c++)
  {
    if (cands[c].hoist_vr < 0)
      continue;
    for (int i = cands[c].def_idx + 1; i < n; i++)
    {
      if (i == cands[c].hoist_idx)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      for (int s = 0; s < 3; s++)
      {
        int has;
        IROperand op;
        if (s == 0)
        {
          has = irop_config[q->op].has_dest;
          if (!has)
            continue;
          op = tcc_ir_op_get_dest(ir, q);
        }
        else if (s == 1)
        {
          has = irop_config[q->op].has_src1;
          if (!has)
            continue;
          op = tcc_ir_op_get_src1(ir, q);
        }
        else
        {
          has = irop_config[q->op].has_src2;
          if (!has)
            continue;
          op = tcc_ir_op_get_src2(ir, q);
        }
        if (!op.is_lval)
          continue;
        int32_t use_vr = irop_get_vreg(op);
        use_vr = optmem_resolve_temp_copy_root(use_vr, i, copy_src, copy_def, max_tmp);
        if (use_vr != cands[c].temp_vr)
          continue;
        if (irop_get_btype(op) != cands[c].btype)
          continue;
        IROperand repl = irop_make_vreg(cands[c].hoist_vr, cands[c].btype);
        repl.is_unsigned = cands[c].is_unsigned;
        if (s == 0)
          tcc_ir_op_set_dest(ir, q, repl);
        else if (s == 1)
          tcc_ir_set_src1(ir, i, repl);
        else
          tcc_ir_set_src2(ir, i, repl);
        changes++;
      }
    }
  }

#undef ITDH_MAX_CANDS
  tcc_free(copy_src);
  tcc_free(copy_def);
  tcc_free(def_count);
  return changes;
}

int tcc_ir_opt_invariant_temp_deref_hoist_ex(IROptCtx *ctx) { return tcc_ir_opt_invariant_temp_deref_hoist(ctx->ir); }

