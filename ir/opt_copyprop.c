/*
 *  TCC IR - Copy Propagation & CSE
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
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_utils.h"
#include "licm.h"

/* Arithmetic Common Subexpression Elimination
 * Phase 3: Eliminate redundant arithmetic computations within basic blocks
 * Handles ADD, SUB, MUL, AND, OR, XOR, SHL, SHR, SAR operations
 */

/* ============================================================================
 * Global LOAD value CSE — deduplicate loads from the same global symbol
 * ============================================================================
 * Pattern: two LOAD instructions in the same basic block read from the same
 * GlobalSym(S)***DEREF*** with no intervening store that could alias it.
 *
 * Transform: replace the second LOAD with an ASSIGN from the first LOAD's
 * destination vreg.  This enables same-vreg comparison folds when both
 * operands of a compare were loaded from the same global.
 */
int tcc_ir_opt_cse_global_load(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  /* Function-wide CSE for loads from static globals.
   * Static globals can only be modified by code in the same TU.  If no
   * STORE instruction in the IR writes to the global, the value is stable
   * across the entire function — including across function calls and BB
   * boundaries — because external callees can't access a file-static symbol.
   *
   * Non-static (extern-visible) globals are only tracked within a BB
   * since any function call could modify them. */
#define GLOAD_CSE_MAX 16
  struct
  {
    Sym *sym;
    int64_t addend;
    int btype;
    int32_t result_vr;
    int is_static; /* 1 if sym has VT_STATIC — survives across calls/BBs */
  } tracked[GLOAD_CSE_MAX];
  int num_tracked = 0;

  /* Pre-scan: check if any instruction STOREs to a GlobalSym.
   * If we find a STORE to GlobalSym(X), we must not CSE loads of X
   * across that store. For simplicity, collect written globals and
   * exclude them from CSE entirely. */
  Sym *written_globals[16];
  int num_written = 0;
  for (int i = 0; i < n && num_written < 16; i++)
  {
    IRQuadCompact *sq = &ir->compact_instructions[i];
    if (sq->op != TCCIR_OP_STORE)
      continue;
    IROperand sdest = tcc_ir_op_get_dest(ir, sq);
    if (!sdest.is_sym || !sdest.is_lval)
      continue;
    IRPoolSymref *sref = irop_get_symref_ex(ir, sdest);
    if (sref && sref->sym)
    {
      int already = 0;
      for (int k = 0; k < num_written; k++)
        if (written_globals[k] == sref->sym)
        {
          already = 1;
          break;
        }
      if (!already)
        written_globals[num_written++] = sref->sym;
    }
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* At BB boundaries: clear non-static entries.
     * At function calls: clear ALL entries unless the call is known-pure.
     * Even static globals can be modified by other functions in the same TU
     * (e.g. frob() modifying static g through inc_g()). Only known-pure
     * calls (like __aeabi_lcmp) provably don't modify any memory. */
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      int is_pure = 0;
      Sym *call_sym = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (call_sym)
      {
        const char *cname = get_tok_str(call_sym->v, NULL);
        if (cname && cname[0] == '_' && cname[1] == '_')
          is_pure = tcc_ir_is_pure_aeabi(cname);
      }
      if (!is_pure)
        num_tracked = 0;
      continue;
    }
    if (q->is_jump_target || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID)
    {
      int dst = 0;
      for (int k = 0; k < num_tracked; k++)
      {
        if (tracked[k].is_static)
          tracked[dst++] = tracked[k];
      }
      num_tracked = dst;
      if (q->op != TCCIR_OP_FUNCCALLVAL)
        continue;
      /* For FUNCCALLVAL, fall through to check if dest overwrites a tracked vreg */
    }

    /* STORE through a pointer could alias any global — invalidate all.
     * Direct stores to known locals (is_local) or known globals (is_sym)
     * are safe: locals can't alias globals, and direct global stores are
     * handled by the written_globals exclusion list. */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
    {
      IROperand sdest = tcc_ir_op_get_dest(ir, q);
      if (!sdest.is_local && !sdest.is_sym)
      {
        num_tracked = 0;
      }
      continue;
    }

    /* If a tracked vreg is redefined, remove it */
    if (irop_config[q->op].has_dest)
    {
      IROperand qdest = tcc_ir_op_get_dest(ir, q);
      int32_t qdvr = irop_get_vreg(qdest);
      if (qdvr >= 0)
      {
        for (int k = 0; k < num_tracked; k++)
        {
          if (tracked[k].result_vr == qdvr)
          {
            tracked[k] = tracked[--num_tracked];
            break;
          }
        }
      }
    }

    if (q->op != TCCIR_OP_LOAD)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (!src1.is_sym || !src1.is_lval)
      continue;

    IRPoolSymref *ref = irop_get_symref_ex(ir, src1);
    if (!ref || !ref->sym)
      continue;

    /* Skip globals that are written to in this function */
    {
      int is_written = 0;
      for (int k = 0; k < num_written; k++)
        if (written_globals[k] == ref->sym)
        {
          is_written = 1;
          break;
        }
      if (is_written)
        continue;
    }

    /* Skip volatile */
    if (ref->sym->type.t & VT_VOLATILE)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0)
      continue;

    int dest_btype = irop_get_btype(dest);
    int sym_is_static = !!(ref->sym->type.t & VT_STATIC);

    int found = -1;
    for (int k = 0; k < num_tracked; k++)
    {
      if (tracked[k].sym == ref->sym && tracked[k].addend == ref->addend && tracked[k].btype == dest_btype)
      {
        found = k;
        break;
      }
    }

    if (found >= 0)
    {
      q->op = TCCIR_OP_ASSIGN;
      IROperand new_src = irop_make_vreg(tracked[found].result_vr, dest_btype);
      tcc_ir_set_src1(ir, i, new_src);
      LOG_IR_GEN("GLOAD_CSE@i=%d: replaced load of sym=%p with vreg %d (static=%d)", i, (void *)ref->sym,
                 tracked[found].result_vr, sym_is_static);
      changes++;
    }
    else if (num_tracked < GLOAD_CSE_MAX)
    {
      tracked[num_tracked].sym = ref->sym;
      tracked[num_tracked].addend = ref->addend;
      tracked[num_tracked].btype = dest_btype;
      tracked[num_tracked].result_vr = dest_vr;
      tracked[num_tracked].is_static = sym_is_static;
      num_tracked++;
    }
  }

#undef GLOAD_CSE_MAX
  return changes;
}

/* Helper: insert instruction before `before_idx`, shift array, patch jumps.
 * Returns the index where the instruction was inserted (-1 on failure). */
int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q)
{
  if (ir->next_instruction_index + 1 >= ir->compact_instructions_size)
  {
    int new_size = ir->compact_instructions_size << 1;
    ir->compact_instructions = tcc_realloc(ir->compact_instructions, sizeof(IRQuadCompact) * new_size);
    ir->compact_instructions_size = new_size;
  }
  for (int i = ir->next_instruction_index; i > before_idx; i--)
    ir->compact_instructions[i] = ir->compact_instructions[i - 1];
  ir->compact_instructions[before_idx] = *new_q;
  ir->next_instruction_index++;
  /* Patch jump targets */
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= before_idx)
        tcc_ir_op_set_dest(ir, q, irop_make_imm32(-1, target + 1, IROP_BTYPE_INT32));
    }
  }
  /* Patch switch-table targets too — SWITCH_TABLE op stores its case targets
   * in a separate side table that is independent of the IR array, so a plain
   * shift+jump-patch pass would silently desynchronize them. */
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    if (table->default_target >= before_idx)
      table->default_target += 1;
    if (table->targets)
    {
      for (int j = 0; j < table->num_entries; j++)
      {
        if (table->targets[j] >= before_idx)
          table->targets[j] += 1;
      }
    }
  }
  return before_idx;
}

/* ============================================================================
 * Non-Negative Value Tracking & Branch Folding
 * ============================================================================
 *
 * Recognizes that return values of functions like fabs/fabsf/abs/labs are
 * always >= 0, and uses this to fold soft-float comparisons against zero.
 *
 * Pattern (soft-float):
 *   FUNCPARAMVAL  P0, call_A:0          ; pass argument to fabs
 *   FUNCCALLVAL   fabs --> V_result     ; V_result is always >= 0
 *   ...
 *   FUNCPARAMVAL  V_result, call_B:0    ; first arg to compare
 *   FUNCPARAMVAL  #0, call_B:1          ; second arg is 0.0
 *   FUNCCALLVAL   __aeabi_dcmpge        ; compares V_result >= 0.0
 *   JUMPIF cond, target                 ; can be folded
 *
 * The key insight: if one argument to a float comparison is known non-negative
 * and the other is zero (or negative), certain comparisons have known results:
 *   fabs(x) >= 0.0  => always true
 *   fabs(x) <  0.0  => always false
 *   fabs(x) <= 0.0  => unknown (could be == 0)
 *   fabs(x) >  0.0  => unknown (could be == 0)
 *   fabs(x) == 0.0  => unknown
 *   fabs(x) != 0.0  => unknown
 */

/* Table of functions known to return non-negative values */

int tcc_ir_opt_local_load_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

#define LLCSE_MAX 32
  struct
  {
    int32_t var_vr;  /* VAR/PARAM vreg */
    int32_t temp_vr; /* TEMP vreg holding the loaded value */
    int btype;       /* btype of the load */
    int def_idx;     /* instruction that defined the TEMP */
  } cache[LLCSE_MAX];
  int cache_count = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Reset at basic-block boundaries. */
    if (q->is_jump_target)
      cache_count = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      cache_count = 0;
      continue;
    }

    /* Look for ASSIGN: TEMP <-- VAR/PARAM (lval read = load from stack). */
    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src = tcc_ir_op_get_src1(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      int32_t src_vr = irop_get_vreg(src);
      int src_type = (src_vr >= 0) ? TCCIR_DECODE_VREG_TYPE(src_vr) : -1;
      int dest_type = (dest_vr >= 0) ? TCCIR_DECODE_VREG_TYPE(dest_vr) : -1;

      if (dest_type == TCCIR_VREG_TYPE_TEMP && (src_type == TCCIR_VREG_TYPE_VAR || src_type == TCCIR_VREG_TYPE_PARAM) &&
          irop_op_is_lval(src) && !irop_is_64bit(src))
      {
        int src_btype = irop_get_btype(src);

        /* Check if we already have a cached load from this VAR. */
        for (int c = 0; c < cache_count; c++)
        {
          if (cache[c].var_vr == src_vr && cache[c].btype == src_btype)
          {
            /* Direct use substitution: replace all downstream uses of
             * dest_vr with cache[c].temp_vr, preserving use-site flags
             * (especially is_lval for DEREF semantics).  Then NOP the
             * redundant definition.  This avoids creating a TMP←TMP copy
             * that downstream passes could mishandle. */
            {
              int cached_vr = cache[c].temp_vr;
              for (int k = i + 1; k < n; k++)
              {
                IRQuadCompact *kq = &ir->compact_instructions[k];
                if (kq->op == TCCIR_OP_NOP)
                  continue;
                if (kq->is_jump_target)
                  break;
                if (kq->op == TCCIR_OP_JUMP || kq->op == TCCIR_OP_JUMPIF ||
                    kq->op == TCCIR_OP_RETURNVALUE || kq->op == TCCIR_OP_RETURNVOID)
                  break;
                if (irop_config[kq->op].has_src1)
                {
                  IROperand ks = tcc_ir_op_get_src1(ir, kq);
                  if (irop_get_vreg(ks) == dest_vr)
                  {
                    irop_set_vreg(&ks, cached_vr);
                    tcc_ir_set_src1(ir, k, ks);
                  }
                }
                if (irop_config[kq->op].has_src2)
                {
                  IROperand ks = tcc_ir_op_get_src2(ir, kq);
                  if (irop_get_vreg(ks) == dest_vr)
                  {
                    irop_set_vreg(&ks, cached_vr);
                    tcc_ir_set_src2(ir, k, ks);
                  }
                }
                if (irop_config[kq->op].has_dest)
                {
                  IROperand kd = tcc_ir_op_get_dest(ir, kq);
                  if (irop_get_vreg(kd) == dest_vr)
                  {
                    irop_set_vreg(&kd, cached_vr);
                    tcc_ir_set_dest(ir, k, kd);
                  }
                }
              }
              q->op = TCCIR_OP_NOP;
              cache[c].def_idx = i;
              changes++;
            }
            goto next_instr;
          }
        }
        /* No cached load — record this one. */
        if (cache_count < LLCSE_MAX)
        {
          cache[cache_count].var_vr = src_vr;
          cache[cache_count].temp_vr = dest_vr;
          cache[cache_count].btype = src_btype;
          cache[cache_count].def_idx = i;
          cache_count++;
        }
        goto next_instr;
      }
    }

    /* Invalidate cache entries when a VAR/PARAM is written. */
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      if (dest_vr >= 0)
      {
        int dest_type = TCCIR_DECODE_VREG_TYPE(dest_vr);
        if (dest_type == TCCIR_VREG_TYPE_VAR || dest_type == TCCIR_VREG_TYPE_PARAM)
        {
          for (int c = 0; c < cache_count; c++)
          {
            if (cache[c].var_vr == dest_vr)
            {
              cache[c] = cache[--cache_count];
              break;
            }
          }
        }
        /* Also invalidate if a TEMP in the cache is redefined. */
        if (dest_type == TCCIR_VREG_TYPE_TEMP)
        {
          for (int c = 0; c < cache_count; c++)
          {
            if (cache[c].temp_vr == dest_vr)
            {
              cache[c] = cache[--cache_count];
              break;
            }
          }
        }
      }
    }

    /* STORE through pointer could alias any addrtaken VAR — flush all. */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    {
      IROperand st_dest = tcc_ir_op_get_dest(ir, q);
      int32_t st_vr = irop_get_vreg(st_dest);
      if (st_vr >= 0 && TCCIR_DECODE_VREG_TYPE(st_vr) == TCCIR_VREG_TYPE_TEMP)
        cache_count = 0; /* indirect store — conservative flush */
    }

  next_instr:;
  }
#undef LLCSE_MAX
  return changes;
}
