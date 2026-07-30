/*
 *  TCC IR - Invariant global LOAD hoist (pre-SSA)
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


/* ============================================================================
 * Invariant Global LOAD Hoist (tcc_ir_opt_invariant_global_load_hoist)
 * ============================================================================
 *
 * Cross-BB CSE for `ASSIGN T <- GlobalSym(X)***DEREF***` and `LOAD` ops that
 * read from a non-static global X.  Targets the unrolled-check pattern from
 * gcc.c-torture/compile/961126-1.c where the same `*p` is reloaded across
 * every iteration of a goto-chain because the existing BB-local global LOAD
 * CSE clears its table at each jump target.
 *
 * Safety conditions (all required):
 *   - Function has only forward control flow:
 *     no IJUMP, no SWITCH_TABLE, no SETJMP/LONGJMP, no INLINE_ASM, and every
 *     JUMP/JUMPIF target is strictly greater than its source.
 *   - X is non-volatile and has no direct STORE to GlobalSym(X) in the function.
 *   - Between the anchor LOAD and the candidate reuse position, no instruction
 *     clobbers globals (CALL, STORE_INDEXED, STORE_POSTINC, STORE through a
 *     non-local non-direct-global destination, etc.).
 *   - The anchor LOAD dominates the reuse position: no JUMP/JUMPIF from a
 *     source outside [anchor, reuse] targets a position in (anchor, reuse].
 *
 * Replaces the reuse op with `ASSIGN T_reuse <- T_anchor`; copy_prop and DCE
 * collapse the chain.
 */
int tcc_ir_opt_invariant_global_load_hoist(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  /* Abort on any unusual control flow that we don't reason about. */
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
          return 0; /* backward jump - loop or weirdness */
        break;
      }
      default:
        break;
    }
  }

  /* Collect direct stores to globals: those globals are not eligible. */
#define IGLH_MAX_WRITTEN 16
  Sym *written_globals[IGLH_MAX_WRITTEN];
  int num_written = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand sdest = tcc_ir_op_get_dest(ir, q);
    if (!sdest.is_sym || !sdest.is_lval)
      continue;
    IRPoolSymref *sref = irop_get_symref_ex(ir, sdest);
    if (!sref || !sref->sym)
      continue;
    int already = 0;
    for (int k = 0; k < num_written; k++)
      if (written_globals[k] == sref->sym)
      {
        already = 1;
        break;
      }
    if (!already && num_written < IGLH_MAX_WRITTEN)
      written_globals[num_written++] = sref->sym;
  }

  /* Precompute per-instruction "clobbers any global" flag.  Calls and stores
   * through unknown addresses can write any global; direct stores to a known
   * GlobalSym do not alias other globals (that case is handled per-symbol via
   * the written_globals list) and stores to a local stack slot cannot alias
   * any global. */
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
        if (dest.is_sym && dest.is_lval)
        {
          IRPoolSymref *sref = irop_get_symref_ex(ir, dest);
          if (!sref || !sref->sym)
            clobber[i] = 1;
          /* else: direct global store - tracked via written_globals */
        }
        else if (!dest.is_local)
        {
          clobber[i] = 1;
        }
        break;
      }
      default:
        break;
    }
  }

#define IGLH_MAX_TRACKED 16
  struct
  {
    Sym *sym;
    int64_t addend;
    int btype;
    int32_t result_vr;
    int load_idx;
  } tracked[IGLH_MAX_TRACKED];
  int num_tracked = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (clobber[i])
    {
      num_tracked = 0;
      continue;
    }

    /* Identify a load of a global symbol: either an explicit LOAD op or an
     * ASSIGN whose src1 is a SYMREF lval (which the frontend emits for
     * `T = *g_ptr` reads of globals). */
    int is_load_like = 0;
    if ((q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) &&
        irop_config[q->op].has_dest && irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (src1.is_sym && src1.is_lval)
        is_load_like = 1;
    }

    if (!is_load_like)
    {
      /* If this op redefines a tracked vreg, drop that entry. */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t dvr = irop_get_vreg(d);
        if (dvr >= 0 && !d.is_lval)
        {
          for (int k = 0; k < num_tracked;)
          {
            if (tracked[k].result_vr == dvr)
              tracked[k] = tracked[--num_tracked];
            else
              k++;
          }
        }
      }
      continue;
    }

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || dest.is_lval)
      continue;

    IRPoolSymref *ref = irop_get_symref_ex(ir, src1);
    if (!ref || !ref->sym)
      continue;

    if (ref->sym->type.t & VT_VOLATILE)
      continue;

    int is_written = 0;
    for (int k = 0; k < num_written; k++)
      if (written_globals[k] == ref->sym)
      {
        is_written = 1;
        break;
      }
    if (is_written)
    {
      for (int k = 0; k < num_tracked;)
      {
        if (tracked[k].sym == ref->sym)
          tracked[k] = tracked[--num_tracked];
        else
          k++;
      }
      continue;
    }

    int dest_btype = irop_get_btype(dest);
    int found = -1;
    for (int k = 0; k < num_tracked; k++)
    {
      if (tracked[k].sym == ref->sym && tracked[k].addend == ref->addend &&
          tracked[k].btype == dest_btype)
      {
        found = k;
        break;
      }
    }

    if (found >= 0)
    {
      int anchor_idx = tracked[found].load_idx;
      /* Dominance check: no JUMP/JUMPIF from a source outside [anchor, i]
       * may target a position in (anchor, i].  A jump that skips over the
       * anchor would mean the value isn't available on all paths into i.
       * Since we already verified there are no backward jumps, source > i
       * with target in (anchor, i] is impossible. So we only need to check
       * jumps with source < anchor. */
      int safe = 1;
      for (int j = 0; j < anchor_idx; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int tgt = (int)irop_get_imm64_ex(ir, jdest);
        if (tgt > anchor_idx && tgt <= i)
        {
          safe = 0;
          break;
        }
      }
      if (safe)
      {
        q->op = TCCIR_OP_ASSIGN;
        IROperand new_src = irop_make_vreg(tracked[found].result_vr, dest_btype);
        new_src.is_unsigned = src1.is_unsigned;
        tcc_ir_set_src1(ir, i, new_src);
        LOG_IR_GEN("IGLH@i=%d: replaced load of sym=%p with vreg %d (anchor i=%d)",
                   i, (void *)ref->sym, tracked[found].result_vr, anchor_idx);
        changes++;
        continue;
      }
      /* not safe - fall through and add a new tracking entry from this load */
    }

    if (num_tracked < IGLH_MAX_TRACKED)
    {
      tracked[num_tracked].sym = ref->sym;
      tracked[num_tracked].addend = ref->addend;
      tracked[num_tracked].btype = dest_btype;
      tracked[num_tracked].result_vr = dest_vr;
      tracked[num_tracked].load_idx = i;
      num_tracked++;
    }
  }

  tcc_free(clobber);
#undef IGLH_MAX_TRACKED
#undef IGLH_MAX_WRITTEN
  return changes;
}

int tcc_ir_opt_invariant_global_load_hoist_ex(IROptCtx *ctx) { return tcc_ir_opt_invariant_global_load_hoist(ctx->ir); }

