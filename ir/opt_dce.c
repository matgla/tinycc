/*
 *  TCC IR - Dead Code & Cleanup Passes
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
#include "opt_xform.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_loop_utils.h"

/* Dead Code Elimination pass
 * Removes unreachable instructions by following control flow from entry.
 * Returns 1 if any instructions were eliminated, 0 otherwise.
 */
int tcc_ir_opt_dce(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* If the function contains any IJUMP (computed goto / indirect jump),
   * skip DCE entirely.  The targets of an IJUMP are determined at runtime
   * (typically via labels-as-values stored in arrays), so we cannot
   * statically determine which basic blocks are reachable from them.
   * Attempting to do DCE would incorrectly eliminate label target blocks
   * that are only reachable through the computed goto. */
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  }

  uint8_t *reachable = tcc_mallocz((n + 7) / 8);
  int *worklist = tcc_malloc(n * sizeof(int));
  int worklist_head = 0, worklist_tail = 0;

/* Mark instruction as reachable if not already marked */
#define MARK_REACHABLE(idx)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((idx) >= 0 && (idx) < n && !(reachable[(idx) / 8] & (1 << ((idx) % 8))))                                       \
    {                                                                                                                  \
      reachable[(idx) / 8] |= (1 << ((idx) % 8));                                                                      \
      worklist[worklist_tail++] = (idx);                                                                               \
    }                                                                                                                  \
  } while (0)

  /* Start from instruction 0 */
  MARK_REACHABLE(0);

  while (worklist_head < worklist_tail)
  {
    int i = worklist[worklist_head++];
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    switch (q->op)
    {
    case TCCIR_OP_JUMP:
      /* Unconditional jump - only the target is reachable */
      MARK_REACHABLE((int)dest.u.imm32);
      break;
    case TCCIR_OP_JUMPIF:
      /* Conditional jump - both target and fall-through are reachable */
      MARK_REACHABLE((int)dest.u.imm32);
      MARK_REACHABLE(i + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE:
    {
      /* Switch table - all targets are reachable */
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
          MARK_REACHABLE(table->targets[j]);
        /* Also mark the default target */
        MARK_REACHABLE(table->default_target);
      }
      /* SWITCH_TABLE is a terminator - no fall-through */
      break;
    }
    case TCCIR_OP_IJUMP:
      /* Indirect jump (computed goto).
         The successor set is not statically known, but in typical patterns
         (like GCC's labels-as-values jump tables) targets are within the same
         function and code continues at/after those labels.
         Conservatively keep fall-through reachable to avoid deleting label
         blocks and subsequent code. */
      MARK_REACHABLE(i + 1);
      break;
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      /* Return/trap - no successor (epilogue is implicit, trap never returns) */
      break;
    default:
      /* All other instructions fall through to the next */
      MARK_REACHABLE(i + 1);
      break;
    }
  }

#undef MARK_REACHABLE

  /* Mark unreachable instructions as NOP (no array compaction needed) */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    if (!(reachable[i / 8] & (1 << (i % 8))))
    {
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
      changes++;
    }
  }

  tcc_free(reachable);
  tcc_free(worklist);

  return changes;
}

int tcc_ir_opt_dce_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dce(ctx->ir);
}

/* ============================================================================
 * NOP Compaction - remove NOP instructions from the instruction array
 * ============================================================================
 *
 * After multiple optimization passes, many instructions are marked as NOP.
 * Every subsequent pass still iterates over them.  This pass removes NOPs
 * in a single O(n) sweep, shrinks the array, and fixes all jump targets.
 *
 * Invariants preserved:
 *   - orig_index on each IRQuadCompact is NOT modified (codegen needs it)
 *   - operand_base indices into iroperand_pool are stable (pool is append-only)
 *   - switch_table targets are remapped
 *   - is_jump_target flags are re-derived from remapped jumps
 */
int tcc_ir_opt_compact_nops(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  IRQuadCompact *instr = ir->compact_instructions;

  /* Quick check: any NOPs at all? */
  int has_nops = 0;
  for (int i = 0; i < n; i++)
  {
    if (instr[i].op == TCCIR_OP_NOP)
    {
      has_nops = 1;
      break;
    }
  }
  if (!has_nops)
    return 0;

  /* Build old_to_new mapping and compact in one forward pass */
  int *old_to_new = tcc_malloc(n * sizeof(int));
  int write_pos = 0;

  for (int i = 0; i < n; i++)
  {
    if (instr[i].op == TCCIR_OP_NOP)
    {
      old_to_new[i] = -1;
    }
    else
    {
      old_to_new[i] = write_pos;
      if (write_pos != i)
        instr[write_pos] = instr[i];
      write_pos++;
    }
  }

  int removed = n - write_pos;
  if (removed == 0)
  {
    tcc_free(old_to_new);
    return 0;
  }

  /* Fix jump targets in JUMP / JUMPIF instructions.
   * Targets can be in [0, n] — target == n means "epilogue" (one past the
   * last instruction), set by tcc_ir_backpatch_to_here for return jumps. */
  for (int i = 0; i < write_pos; i++)
  {
    IRQuadCompact *q = &instr[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int old_target = (int)irop_get_imm64_ex(ir, dest);
      if (old_target < 0)
        continue;

      int new_target;
      if (old_target >= n)
      {
        /* Epilogue target (past-end): remap to new past-end */
        new_target = write_pos + (old_target - n);
      }
      else
      {
        new_target = old_to_new[old_target];
        if (new_target < 0)
        {
          /* Target was a NOP — find the next non-NOP instruction after it.
           * This shouldn't normally happen (DCE + jump threading should have
           * fixed dangling targets), but handle it defensively. */
          for (int j = old_target + 1; j < n; j++)
          {
            if (old_to_new[j] >= 0)
            {
              new_target = old_to_new[j];
              break;
            }
          }
          if (new_target < 0)
            new_target = write_pos; /* fall through to epilogue */
        }
      }
      if (new_target != old_target)
      {
        IROperand new_dest = irop_make_imm32(-1, new_target, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }

  /* Fix switch table targets — same epilogue-aware remapping as jumps */
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int j = 0; j < table->num_entries; j++)
    {
      int old_t = table->targets[j];
      if (old_t < 0)
        continue;
      if (old_t >= n)
      {
        table->targets[j] = write_pos + (old_t - n);
      }
      else
      {
        int new_t = old_to_new[old_t];
        if (new_t < 0)
        {
          for (int k = old_t + 1; k < n; k++)
          {
            if (old_to_new[k] >= 0)
            {
              new_t = old_to_new[k];
              break;
            }
          }
          if (new_t < 0)
            new_t = write_pos;
        }
        table->targets[j] = new_t;
      }
    }
    {
      int old_dt = table->default_target;
      if (old_dt >= 0)
      {
        if (old_dt >= n)
        {
          table->default_target = write_pos + (old_dt - n);
        }
        else
        {
          int new_dt = old_to_new[old_dt];
          if (new_dt < 0)
          {
            for (int k = old_dt + 1; k < n; k++)
            {
              if (old_to_new[k] >= 0)
              {
                new_dt = old_to_new[k];
                break;
              }
            }
            if (new_dt < 0)
              new_dt = write_pos;
          }
          table->default_target = new_dt;
        }
      }
    }
  }

  /* Re-derive is_jump_target flags from scratch */
  for (int i = 0; i < write_pos; i++)
    instr[i].is_jump_target = 0;

  for (int i = 0; i < write_pos; i++)
  {
    IRQuadCompact *q = &instr[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= 0 && target < write_pos)
        instr[target].is_jump_target = 1;
    }
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int j = 0; j < table->num_entries; j++)
    {
      if (table->targets[j] >= 0 && table->targets[j] < write_pos)
        instr[table->targets[j]].is_jump_target = 1;
    }
    if (table->default_target >= 0 && table->default_target < write_pos)
      instr[table->default_target].is_jump_target = 1;
  }

  ir->next_instruction_index = write_pos;

  tcc_free(old_to_new);

  return removed;
}

int tcc_ir_opt_compact_nops_ex(IROptCtx *ctx)
{
  int removed = tcc_ir_opt_compact_nops(ctx->ir);
  if (removed > 0)
    tcc_ir_opt_ctx_invalidate(ctx);
  return removed;
}

/* Constant VAR Propagation - propagate constant-assigned VAR vregs into uses.
 * Designed to run after store-load forwarding which may convert stack loads
 * into constant assignments (e.g. V0 <-- #34 [ASSIGN]) that the main
 * optimization loop's const_prop never saw.
 *
 * Unlike const_prop, this pass:
 *   - Does not check is_local/is_lval flags (safe since we verified single def)
 *   - Converts LOAD→ASSIGN when replacing a local-variable source with a constant
 *   - Handles both src1 and src2 operands
 *
 * This enables the register allocator to avoid callee-saved registers for
 * values that are cheap to rematerialize (small immediates).
 */

int tcc_ir_opt_dse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Orphaned PARAM elimination: NOP FUNCPARAMVAL/FUNCPARAMVOID instructions
   * whose call_id has no matching FUNCCALLVAL/FUNCCALLVOID.
   * This happens when a function is inlined: the CALL is replaced with inline
   * code but the PARAM (e.g. struct return buffer address) is left behind.
   * Eliminating them removes false address-takes that prevent dead store elim.
   *
   * Combined pass 1+2: single scan that tracks max_call_id AND builds has_call[]
   * (grows dynamically as new call_ids are observed). */
  {
    uint8_t *has_call = NULL;
    int has_call_bytes = 0;
    int max_call_id = 0;
    int saw_any = 0;

    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID && q->op != TCCIR_OP_FUNCCALLVAL &&
          q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      saw_any = 1;
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(ir, src2));
      if (cid > max_call_id)
        max_call_id = cid;

      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        int needed_bytes = (cid / 8) + 1;
        if (needed_bytes > has_call_bytes)
        {
          int new_bytes = has_call_bytes ? has_call_bytes * 2 : 32;
          while (new_bytes < needed_bytes)
            new_bytes *= 2;
          has_call = tcc_realloc(has_call, new_bytes);
          memset(has_call + has_call_bytes, 0, new_bytes - has_call_bytes);
          has_call_bytes = new_bytes;
        }
        has_call[cid / 8] |= (1 << (cid % 8));
      }
    }

    if (saw_any && max_call_id > 0)
    {
      /* NOP orphaned PARAMs */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
        {
          IROperand src2 = tcc_ir_op_get_src2(ir, q);
          int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(ir, src2));
          int byte_idx = cid / 8;
          int has = (byte_idx < has_call_bytes) && (has_call[byte_idx] & (1 << (cid % 8)));
          if (cid <= max_call_id && !has)
          {
            LOG_IR_GEN("OPTIMIZE: Orphaned PARAM at i=%d (call_id=%d has no CALL)", i, cid);
            q->op = TCCIR_OP_NOP;
          }
        }
      }
    }

    if (has_call)
      tcc_free(has_call);
  }

  /* Worklist-based cascading DSE.
   * Single O(n) combined pass: find max_tmp_pos, build use_count[] and
   * def_idx[] simultaneously, with dynamic growth of the per-TMP tables. */
  int max_tmp_pos = 0;
  int table_cap = 32;
  uint16_t *use_count = tcc_mallocz(table_cap * sizeof(uint16_t));
  int *def_idx = tcc_malloc(table_cap * sizeof(int));
  for (int i = 0; i < table_cap; i++)
    def_idx[i] = -1;

  /* Ensure use_count[] / def_idx[] can index position _p (grows on demand) */
#define DSE_ENSURE_CAP(_p)                                                                                             \
  do                                                                                                                   \
  {                                                                                                                    \
    int _pp = (_p);                                                                                                    \
    if (_pp >= table_cap)                                                                                              \
    {                                                                                                                  \
      int _new_cap = table_cap * 2;                                                                                    \
      while (_new_cap <= _pp)                                                                                          \
        _new_cap *= 2;                                                                                                 \
      use_count = tcc_realloc(use_count, _new_cap * sizeof(uint16_t));                                                 \
      memset(use_count + table_cap, 0, (_new_cap - table_cap) * sizeof(uint16_t));                                     \
      def_idx = tcc_realloc(def_idx, _new_cap * sizeof(int));                                                          \
      for (int _k = table_cap; _k < _new_cap; _k++)                                                                    \
        def_idx[_k] = -1;                                                                                              \
      table_cap = _new_cap;                                                                                            \
    }                                                                                                                  \
    if (_pp > max_tmp_pos)                                                                                             \
      max_tmp_pos = _pp;                                                                                               \
  } while (0)

#define DSE_INC_USE(_pos)                                                                                              \
  do                                                                                                                   \
  {                                                                                                                    \
    int _p = (_pos);                                                                                                   \
    if (_p >= 0)                                                                                                       \
    {                                                                                                                  \
      DSE_ENSURE_CAP(_p);                                                                                              \
      if (use_count[_p] < 0xFFFF)                                                                                      \
        use_count[_p]++;                                                                                               \
    }                                                                                                                  \
  } while (0)

  /* Single O(n) pass: build use_count[] and def_idx[] */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      const IROperand s = tcc_ir_op_get_src1(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s)));
    }
    if (irop_config[q->op].has_src2)
    {
      const IROperand s = tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s)));
    }

    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    /* STORE/STORE_INDEXED dest is a pointer use, not a def */
    if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
      DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)));
    /* FUNCPARAMVAL dest carries the parameter value — it's a use */
    if (q->op == TCCIR_OP_FUNCPARAMVAL && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
      DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)));
    /* MLA accumulator is a use */
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc)));
    }

    /* Record def site for TEMP-destination ops where dest is a real def */
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP &&
        q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_FUNCPARAMVAL)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
      if (pos >= 0)
      {
        DSE_ENSURE_CAP(pos);
        def_idx[pos] = i; /* last def wins; OK since we only eliminate when use_count hits 0 */
      }
    }
  }

  if (max_tmp_pos == 0)
  {
    tcc_free(use_count);
    tcc_free(def_idx);
    return 0;
  }

  int changes = 0;
  LOG_IR_GEN("=== DEAD STORE ELIMINATION START ===");

  /* Reusable dead-eligibility predicate */
#define DSE_IS_DEAD_ELIGIBLE(_q)                                                                                       \
  (((_q)->op != TCCIR_OP_STORE && (_q)->op != TCCIR_OP_STORE_INDEXED && (_q)->op != TCCIR_OP_STORE_POSTINC &&          \
    (_q)->op != TCCIR_OP_LOAD_POSTINC && (_q)->op != TCCIR_OP_LOAD && (_q)->op != TCCIR_OP_FUNCCALLVAL &&              \
    (_q)->op != TCCIR_OP_FUNCCALLVOID && (_q)->op != TCCIR_OP_FUNCPARAMVAL && (_q)->op != TCCIR_OP_FUNCPARAMVOID) ||   \
   ((_q)->op == TCCIR_OP_LOAD &&                                                                                       \
    (irop_is_immediate(tcc_ir_op_get_src1(ir, (_q))) || tcc_ir_op_get_src1(ir, (_q)).is_sym)))

  /* Worklist of TMP positions whose use_count just dropped to 0 */
  int *worklist = tcc_malloc((max_tmp_pos + 1) * sizeof(int));
  int wl_top = 0;

  /* Seed the worklist by eliminating all initially-dead instructions */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_config[q->op].has_dest)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
    if (pos > max_tmp_pos || use_count[pos] != 0)
      continue;
    if (!DSE_IS_DEAD_ELIGIBLE(q))
      continue;

    /* Decrement use_count for this instruction's sources before NOP'ing it */
    if (irop_config[q->op].has_src1)
    {
      const IROperand s = tcc_ir_op_get_src1(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (irop_config[q->op].has_src2)
    {
      const IROperand s = tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    q->op = TCCIR_OP_NOP;
    def_idx[pos] = -1;
    changes++;
  }

  /* Drain the worklist: cascade eliminations */
  while (wl_top > 0)
  {
    int pos = worklist[--wl_top];
    int di = def_idx[pos];
    if (di < 0 || di >= n)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[di];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (use_count[pos] != 0)
      continue; /* someone used it again via a different def */
    if (!DSE_IS_DEAD_ELIGIBLE(q))
      continue;
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_config[q->op].has_dest || TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) != TCCIR_VREG_TYPE_TEMP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      const IROperand s = tcc_ir_op_get_src1(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (irop_config[q->op].has_src2)
    {
      const IROperand s = tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    q->op = TCCIR_OP_NOP;
    def_idx[pos] = -1;
    changes++;
  }

#undef DSE_INC_USE
#undef DSE_ENSURE_CAP
#undef DSE_IS_DEAD_ELIGIBLE

  LOG_IR_GEN("=== DEAD STORE ELIMINATION END (marked %d as NOP) ===", changes);
  tcc_free(worklist);
  tcc_free(def_idx);
  tcc_free(use_count);

  /* Also eliminate dead VAR vreg definitions.
   * A VAR that is defined (ASSIGN) but never used as a source operand
   * anywhere in the function is dead — provided it's not address-taken. */
  {
    int max_var_pos = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      const IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (irop_config[q->op].has_dest && vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var_pos)
          max_var_pos = pos;
      }
    }

    if (max_var_pos > 0)
    {
      uint8_t *var_used = tcc_mallocz((max_var_pos + 8) / 8);

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;

        /* Check src1 */
        if (irop_config[q->op].has_src1)
        {
          const IROperand s = tcc_ir_op_get_src1(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_pos)
              var_used[pos / 8] |= (1 << (pos % 8));
          }
        }

        /* Check src2 */
        if (irop_config[q->op].has_src2)
        {
          const IROperand s = tcc_ir_op_get_src2(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_pos)
              var_used[pos / 8] |= (1 << (pos % 8));
          }
        }

        /* STORE/STORE_INDEXED dest: only a use when it's a pointer dereference
         * (non-local), not when it's a direct local store (which is a define). */
        if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          if (!d.is_local)
          {
            int32_t vr = irop_get_vreg(d);
            if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (pos <= max_var_pos)
                var_used[pos / 8] |= (1 << (pos % 8));
            }
          }
        }

        /* FUNCPARAMVAL dest carries the parameter value — it's a use, not a def */
        if (q->op == TCCIR_OP_FUNCPARAMVAL)
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t vr = irop_get_vreg(d);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_pos)
              var_used[pos / 8] |= (1 << (pos % 8));
          }
        }
      }

      /* NOP ASSIGN/STORE to unused VARs (skip address-taken) */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE)
          continue;
        const IROperand dest = tcc_ir_op_get_dest(ir, q);
        /* For STORE, only eliminate local stores (not pointer dereferences) */
        if (q->op == TCCIR_OP_STORE && !dest.is_local)
          continue;
        /* Skip stores to parent frame via static chain — externally visible */
        if (dest.is_llocal)
          continue;
        int32_t vr = irop_get_vreg(dest);
        if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
          continue;
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var_pos)
          continue;
        if (var_used[pos / 8] & (1 << (pos % 8)))
          continue; /* VAR is used */
        /* Skip address-taken variables */
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
        if (interval && interval->addrtaken)
          continue;
        q->op = TCCIR_OP_NOP;
        changes++;
      }

      tcc_free(var_used);
    }
  }

  /* Dead StackLoc store elimination.
   * STORE to anonymous StackLoc offsets (not VAR vregs) that are never read
   * and whose addresses are never taken can be safely eliminated.
   * Uses a hash set of (sym, offset) pairs to track read/address-taken locations.
   *
   * Skip if function uses static chain — two cases:
   * 1. Function is a nested function (has_static_chain=1): it may write to
   *    the parent's frame via the chain pointer, and those writes are
   *    externally visible even though they look dead within this function.
   * 2. Function contains SET_CHAIN instructions: it is a parent function
   *    that sets up the chain for nested function calls, meaning its own
   *    stack variables may be read by the nested functions. */
  {
    if (ir->has_static_chain)
      goto skip_dead_stackloc;

    for (int i = 0; i < n; i++)
    {
      if (ir->compact_instructions[i].op == TCCIR_OP_SET_CHAIN)
        goto skip_dead_stackloc;
    }
#define STACKLOC_HASH_SIZE 256
    uint8_t stackloc_read[STACKLOC_HASH_SIZE];
    memset(stackloc_read, 0, sizeof(stackloc_read));

    /* Pre-scan: find the maximum StackLoc offset used in any STORE.
     * This determines how far an address-of range needs to extend. */
    int64_t max_stackloc_off = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_STORE)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local || irop_get_vreg(dest) >= 0)
        continue;
      int64_t off;
      if (irop_get_tag(dest) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
        off = sr ? sr->addend : 0;
      }
      else
      {
        off = irop_get_stack_offset(dest);
      }
      if (off > max_stackloc_off)
        max_stackloc_off = off;
    }

    /* Helper: hash a (sym, offset) pair to a bit index */
#define STACKLOC_HASH(sym, off) (((uintptr_t)(sym) * 31 + (uint32_t)(off) * 17) % (STACKLOC_HASH_SIZE * 8))
#define STACKLOC_SET(sym, off)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    uint32_t _h = STACKLOC_HASH(sym, off);                                                                             \
    stackloc_read[_h / 8] |= (1 << (_h % 8));                                                                          \
  } while (0)
#define STACKLOC_TEST(sym, off) (stackloc_read[STACKLOC_HASH(sym, off) / 8] & (1 << (STACKLOC_HASH(sym, off) % 8)))

    /* Pre-scan: identify write-only address-of TEMPs.
     * An addr-TMP is "write-only" if the entire chain from the Addr[StackLoc]
     * through VAR intermediaries down to final uses consists only of:
     *   - STORE addr-prop-TMP → VAR  (address pipeline flow)
     *   - ASSIGN addr-prop-VAR → TMP (address pipeline flow)
     *   - ADD addr-prop-TMP, offset → TMP (pointer arithmetic)
     *   - STORE value → *addr-prop-TMP  (deref write — safe)
     * Any other use (LOAD, FUNCPARAM, TEST_ZERO, CMP, etc.) means the address
     * or pointed-to data is observable, so the addr-TMP is marked "read". */
    int max_tmp_stackloc = 0;
    int max_var_stackloc = -1;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      IROperand ops[3];
      int nops = 0;
      if (irop_config[q->op].has_dest)
        ops[nops++] = tcc_ir_op_get_dest(ir, q);
      if (irop_config[q->op].has_src1)
        ops[nops++] = tcc_ir_op_get_src1(ir, q);
      if (irop_config[q->op].has_src2)
        ops[nops++] = tcc_ir_op_get_src2(ir, q);
      for (int k = 0; k < nops; k++)
      {
        int32_t vr = irop_get_vreg(ops[k]);
        if (vr >= 0)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos > max_tmp_stackloc)
            max_tmp_stackloc = pos;
          else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos > max_var_stackloc)
            max_var_stackloc = pos;
        }
      }
    }

    /* addr_tmp[pos] = 1 if TMP pos was defined from an Addr[StackLoc] */
    /* addr_tmp_read[pos] = 1 if the addr or pointed-to data is observable */
    uint8_t *addr_tmp = NULL;
    uint8_t *addr_tmp_read = NULL;

    if (max_tmp_stackloc > 0)
    {
      addr_tmp = tcc_mallocz((max_tmp_stackloc + 8) / 8);
      addr_tmp_read = tcc_mallocz((max_tmp_stackloc + 8) / 8);

      /* Phase 1: Find TEMPs defined from Addr[StackLoc] (is_local=1, is_lval=0) */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_src1)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          if (s.is_local && !s.is_lval && irop_get_vreg(s) < 0)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t dvr = irop_get_vreg(d);
            if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
            {
              int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
              if (dpos <= max_tmp_stackloc)
                addr_tmp[dpos / 8] |= (1 << (dpos % 8));
            }
          }
        }
      }

      /* Phase 2: Propagate addr-prop through STORE→VAR, ASSIGN→TMP, ADD→TMP.
       * prop_tmp[pos] and prop_var[pos] record the origin addr-TMP position,
       * or -1 (not addr-prop) or -2 (ambiguous / multiple origins). */
      int *prop_tmp = tcc_mallocz((max_tmp_stackloc + 1) * sizeof(int));
      int *prop_var = max_var_stackloc >= 0 ? tcc_mallocz((max_var_stackloc + 1) * sizeof(int)) : NULL;
      memset(prop_tmp, 0xFF, (max_tmp_stackloc + 1) * sizeof(int)); /* -1 */
      if (prop_var)
        memset(prop_var, 0xFF, (max_var_stackloc + 1) * sizeof(int));

      /* Seed from addr-TMPs */
      for (int pos = 0; pos <= max_tmp_stackloc; pos++)
        if (addr_tmp[pos / 8] & (1 << (pos % 8)))
          prop_tmp[pos] = pos;

      /* Propagate through the chain */
      int prop_changed = 1;
      while (prop_changed)
      {
        prop_changed = 0;
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP)
            continue;

          if (!irop_config[q->op].has_src1 || !irop_config[q->op].has_dest)
            continue;
          IROperand src = tcc_ir_op_get_src1(ir, q);
          IROperand dest = tcc_ir_op_get_dest(ir, q);
          int32_t svr = irop_get_vreg(src);
          int32_t dvr = irop_get_vreg(dest);
          if (svr < 0 || dvr < 0)
            continue;

          int stype = TCCIR_DECODE_VREG_TYPE(svr);
          int dtype = TCCIR_DECODE_VREG_TYPE(dvr);
          int spos = TCCIR_DECODE_VREG_POSITION(svr);
          int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
          int origin = -1;

          /* STORE/ASSIGN TMP→VAR: addr flows from TMP to VAR.
           * ASSIGN case arises after copy propagation rewrites STORE to ASSIGN
           * or propagates an addr-TMP through a TMP→VAR assignment chain. */
          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN) && stype == TCCIR_VREG_TYPE_TEMP &&
              dtype == TCCIR_VREG_TYPE_VAR && spos <= max_tmp_stackloc && prop_var && dpos <= max_var_stackloc)
            origin = prop_tmp[spos];
          /* ASSIGN VAR→TMP: addr flows from VAR to TMP */
          else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && stype == TCCIR_VREG_TYPE_VAR &&
                   dtype == TCCIR_VREG_TYPE_TEMP && prop_var && spos <= max_var_stackloc && dpos <= max_tmp_stackloc)
            origin = prop_var[spos];
          /* ASSIGN/LOAD VAR→VAR: addr flows from one VAR to another */
          else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && stype == TCCIR_VREG_TYPE_VAR &&
                   dtype == TCCIR_VREG_TYPE_VAR && prop_var && spos <= max_var_stackloc && dpos <= max_var_stackloc)
            origin = prop_var[spos];
          /* ADD/SUB/ASSIGN TMP→TMP: pointer arithmetic and copies preserve addr-prop */
          else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_ASSIGN) &&
                   stype == TCCIR_VREG_TYPE_TEMP && dtype == TCCIR_VREG_TYPE_TEMP && spos <= max_tmp_stackloc &&
                   dpos <= max_tmp_stackloc)
            origin = prop_tmp[spos];

          if (origin < 0)
            continue;

          /* Propagate to dest */
          if (dtype == TCCIR_VREG_TYPE_TEMP && dpos <= max_tmp_stackloc)
          {
            if (prop_tmp[dpos] == -1)
            {
              prop_tmp[dpos] = origin;
              prop_changed = 1;
            }
            else if (prop_tmp[dpos] != origin && prop_tmp[dpos] != -2)
            {
              prop_tmp[dpos] = -2; /* ambiguous */
              prop_changed = 1;
            }
          }
          else if (dtype == TCCIR_VREG_TYPE_VAR && prop_var && dpos <= max_var_stackloc)
          {
            if (prop_var[dpos] == -1)
            {
              prop_var[dpos] = origin;
              prop_changed = 1;
            }
            else if (prop_var[dpos] != origin && prop_var[dpos] != -2)
            {
              prop_var[dpos] = -2;
              prop_changed = 1;
            }
          }
        }
      }

      /* Phase 3: Check uses of all addr-prop values. Mark origin addr-TMP
       * as "read" if any propagated value is used outside the write pipeline. */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;

        /* Helper: get origin of a vreg, or -1 if not addr-prop */
#define GET_ORIGIN(vr)                                                                                                 \
  ({                                                                                                                   \
    int _o = -1;                                                                                                       \
    int _t = TCCIR_DECODE_VREG_TYPE(vr);                                                                               \
    int _p = TCCIR_DECODE_VREG_POSITION(vr);                                                                           \
    if (_t == TCCIR_VREG_TYPE_TEMP && _p <= max_tmp_stackloc)                                                          \
      _o = prop_tmp[_p];                                                                                               \
    else if (_t == TCCIR_VREG_TYPE_VAR && prop_var && _p <= max_var_stackloc)                                          \
      _o = prop_var[_p];                                                                                               \
    _o;                                                                                                                \
  })

#define MARK_ORIGIN_READ(origin)                                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((origin) == -2)                                                                                                \
      memset(addr_tmp_read, 0xFF, (max_tmp_stackloc + 8) / 8);                                                         \
    else if ((origin) >= 0 && (origin) <= max_tmp_stackloc)                                                            \
      addr_tmp_read[(origin) / 8] |= (1 << ((origin) % 8));                                                            \
  } while (0)

        /* Check src1 */
        if (irop_config[q->op].has_src1)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0)
          {
            int origin = GET_ORIGIN(vr);
            if (origin != -1)
            {
              /* Is this use safe (within the write pipeline)? */
              int safe = 0;
              if (q->op == TCCIR_OP_STORE && (!s.is_lval || TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR))
              {
                /* Storing addr value into a VAR = pipeline flow.
                 * For TMP src, is_lval=1 means deref read (*ptr) — NOT safe.
                 * For VAR src, is_lval=1 just means "load variable" — safe. */
                IROperand d = tcc_ir_op_get_dest(ir, q);
                int32_t dvr = irop_get_vreg(d);
                if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
                  safe = 1;
              }
              else if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
              {
                /* Pipeline flow or pointer arithmetic — but a deref read
                 * (is_lval=1 on a TMP) means memory at the address is being
                 * read, making the pointed-to data observable.
                 * For VAR sources, is_lval=1 just means "load the variable's
                 * value" (always set for VAR reads) — this is a pointer copy,
                 * NOT a dereference through the pointed-to data. */
                int stype = TCCIR_DECODE_VREG_TYPE(vr);
                if (!s.is_lval || stype == TCCIR_VREG_TYPE_VAR)
                  safe = 1;
              }
              if (!safe)
              {
                LOG_IR_GEN("DSE-SL: Phase3 MARK READ origin=%d at i=%d op=%d src1 is_lval=%d", origin, i, q->op,
                           s.is_lval);
                MARK_ORIGIN_READ(origin);
              }
            }
          }
        }

        /* Check src2: addr-prop as src2 is unusual; conservatively mark read */
        if (irop_config[q->op].has_src2)
        {
          IROperand s = tcc_ir_op_get_src2(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0)
          {
            int origin = GET_ORIGIN(vr);
            if (origin != -1)
            {
              LOG_IR_GEN("DSE-SL: Phase3 MARK READ origin=%d at i=%d op=%d src2", origin, i, q->op);
              MARK_ORIGIN_READ(origin);
            }
          }
        }

        /* STORE dest: if dest is an addr-prop TMP (deref write), that's safe.
         * No marking needed — this is a write through the pointer. */

#undef GET_ORIGIN
#undef MARK_ORIGIN_READ
      }

      tcc_free(prop_tmp);
      if (prop_var)
        tcc_free(prop_var);
    }

    /* Pass 1: Mark StackLoc offsets that are read or address-taken */
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Helper: mark a StackLoc operand as read or address-taken.
       * is_lval=true means direct memory read → mark exact offset.
       * is_lval=false means address-of → mark range up to max store offset.
       * Write-only address-of (pointer only used for writes) is skipped. */
#define MARK_STACKLOC_OP(op)                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((op).is_local && irop_get_vreg(op) < 0)                                                                        \
    {                                                                                                                  \
      const Sym *_sym = NULL;                                                                                          \
      int64_t _off;                                                                                                    \
      if (irop_get_tag(op) == IROP_TAG_SYMREF)                                                                         \
      {                                                                                                                \
        IRPoolSymref *_sr = irop_get_symref_ex(ir, op);                                                                \
        _sym = _sr ? _sr->sym : NULL;                                                                                  \
        _off = _sr ? _sr->addend : 0;                                                                                  \
      }                                                                                                                \
      else                                                                                                             \
      {                                                                                                                \
        _off = irop_get_stack_offset(op);                                                                              \
      }                                                                                                                \
      if ((op).is_lval)                                                                                                \
      {                                                                                                                \
        /* Mark all byte offsets within the access width so that stores                                                \
         * at sub-offsets (e.g. _Complex short imag part at _off+2)                                                    \
         * are not incorrectly eliminated as dead. */                                                                  \
        int _width;                                                                                                    \
        switch ((op).btype)                                                                                            \
        {                                                                                                              \
        case IROP_BTYPE_INT8:                                                                                          \
          _width = 1;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_INT16:                                                                                         \
          _width = 2;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_FLOAT32:                                                                                       \
          _width = 4;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_INT64:                                                                                         \
        case IROP_BTYPE_FLOAT64:                                                                                       \
          _width = 8;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_STRUCT:                                                                                        \
        {                                                                                                              \
          /* Struct access — conservatively mark range up to max store offset */                                       \
          int64_t _send = max_stackloc_off + 4;                                                                        \
          for (int64_t _s = _off; _s <= _send; _s++)                                                                   \
            STACKLOC_SET(_sym, _s);                                                                                    \
          _width = 0; /* already handled */                                                                            \
          break;                                                                                                       \
        }                                                                                                              \
        default:                                                                                                       \
          _width = 4;                                                                                                  \
          break;                                                                                                       \
        }                                                                                                              \
        for (int _b = 0; _b < _width; _b++)                                                                            \
          STACKLOC_SET(_sym, _off + _b);                                                                               \
      }                                                                                                                \
      else                                                                                                             \
      {                                                                                                                \
        /* Address-of: mark from base offset to max store offset (+ margin for field access).                          \
         * Cap range to avoid excessive iteration; if too large, mark all bits. */                                     \
        int64_t _range_end = max_stackloc_off + 4;                                                                     \
        int64_t _range_len = _range_end - _off + 1;                                                                    \
        if (_range_len > STACKLOC_HASH_SIZE * 8)                                                                       \
        {                                                                                                              \
          memset(stackloc_read, 0xFF, sizeof(stackloc_read));                                                          \
        }                                                                                                              \
        else if (_range_len > 0)                                                                                       \
        {                                                                                                              \
          for (int64_t _k = _off; _k <= _range_end; _k++)                                                              \
            STACKLOC_SET(_sym, _k);                                                                                    \
        }                                                                                                              \
      }                                                                                                                \
    }                                                                                                                  \
  } while (0)

      /* Check all operands for StackLoc reads / address-taken */
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        /* Skip range marking for address-of StackLoc that feeds a write-only TMP.
         * If the TMP is only used for STORE destinations (pointer writes), the
         * address-of doesn't constitute a "read" of the StackLoc range. */
        if (s.is_local && !s.is_lval && irop_get_vreg(s) < 0 && addr_tmp != NULL)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (TCC_LOG_IR_GEN)
          {
            fprintf(stderr, "[IR_GEN] DSE-SL: addr-of check i=%d dvr=0x%x type=%d pos=%d max=%d", i, (unsigned)dvr,
                    dvr >= 0 ? TCCIR_DECODE_VREG_TYPE(dvr) : -1, dvr >= 0 ? TCCIR_DECODE_VREG_POSITION(dvr) : -1,
                    max_tmp_stackloc);
            if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
            {
              int _dp = TCCIR_DECODE_VREG_POSITION(dvr);
              fprintf(stderr, " addr_tmp=%d addr_tmp_read=%d",
                      !!(_dp <= max_tmp_stackloc && (addr_tmp[_dp / 8] & (1 << (_dp % 8)))),
                      !!(_dp <= max_tmp_stackloc && (addr_tmp_read[_dp / 8] & (1 << (_dp % 8)))));
            }
            fprintf(stderr, "\n");
          }
          if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
          {
            int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
            if (dpos <= max_tmp_stackloc && (addr_tmp[dpos / 8] & (1 << (dpos % 8))) &&
                !(addr_tmp_read[dpos / 8] & (1 << (dpos % 8))))
            {
              LOG_IR_GEN("DSE-SL: SKIP addr-of at i=%d (write-only T%d)", i, dpos);
              goto after_src1_mark;
            }
          }
        }
        LOG_IR_GEN("DSE-SL: MARK src1 at i=%d op=%d is_lval=%d is_local=%d", i, q->op, s.is_lval, s.is_local);
        /* FUNCPARAMVAL/FUNCPARAMVOID src1 may pass a struct spanning multiple
         * consecutive words from a single StackLoc base — force range marking. */
        if ((q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) && s.is_local && irop_get_vreg(s) < 0)
        {
          IROperand s_range = s;
          s_range.is_lval = 0;
          MARK_STACKLOC_OP(s_range);
        }
        else
        {
          MARK_STACKLOC_OP(s);
        }
      after_src1_mark:;
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        LOG_IR_GEN("DSE-SL: MARK src2 at i=%d op=%d is_lval=%d is_local=%d", i, q->op, s.is_lval, s.is_local);
        MARK_STACKLOC_OP(s);
      }
      /* dest operands are not uses for most instructions (they are defines).
       * Special cases (FUNCPARAMVAL src1) are handled above. */
      /* MLA accumulator (4th operand) may reference a StackLoc */
      if (q->op == TCCIR_OP_MLA)
      {
        IROperand acc = tcc_ir_op_get_accum(ir, q);
        MARK_STACKLOC_OP(acc);
      }
#undef MARK_STACKLOC_OP
    }

    /* Pass 2: Eliminate STORE to unread StackLoc offsets */
#if TCC_LOG_IR_GEN
    {
      int any_set = 0;
      for (int bi = 0; bi <= max_stackloc_off / 8; bi++)
        if (stackloc_read[bi])
        {
          any_set = 1;
          break;
        }
      LOG_IR_GEN("DSE-SL: stackloc_read has %s bits set, max_stackloc_off=%lld", any_set ? "some" : "NO",
                 (long long)max_stackloc_off);
    }
#endif
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_STORE)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local || irop_get_vreg(dest) >= 0)
        continue; /* Not an anonymous StackLoc */
      if (dest.is_llocal)
        continue; /* Store to parent frame via static chain — externally visible */

      const Sym *sym = NULL;
      int64_t off;
      if (irop_get_tag(dest) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
        sym = sr ? sr->sym : NULL;
        off = sr ? sr->addend : 0;
      }
      else
      {
        off = irop_get_stack_offset(dest);
      }

      LOG_IR_GEN("DSE-SL: i=%d off=%lld sym=%p test=%d", i, (long long)off, (void *)sym, !!STACKLOC_TEST(sym, off));
      if (!STACKLOC_TEST(sym, off))
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }

    /* Pass 3: Eliminate dead pointer chains from write-only addr-of.
     * If a write-only addr-TMP produced by Addr[StackLoc] feeds only
     * pointer writes, and the StackLoc range has no surviving reads,
     * NOP the LEA and forward-propagate: NOP instructions that use
     * only dead TMPs/VARs as sources or pointer bases (deref stores). */
    if (addr_tmp != NULL)
    {
      /* Find max VAR position for dead_var tracking */
      int max_var_pos = -1;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t vr = irop_get_vreg(d);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos > max_var_pos)
              max_var_pos = pos;
          }
        }
      }

      uint8_t *dead_tmp = tcc_mallocz((max_tmp_stackloc + 8) / 8);
      uint8_t *dead_var = max_var_pos >= 0 ? tcc_mallocz((max_var_pos + 8) / 8) : NULL;

      /* Step 1: NOP LEA/Addr instructions for write-only addr-TMPs whose
       * StackLoc range is actually dead (no surviving reads). */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_src1)
          continue;
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (!s.is_local || s.is_lval || irop_get_vreg(s) >= 0)
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t dvr = irop_get_vreg(d);
        if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
          continue;
        int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (dpos > max_tmp_stackloc)
          continue;
        if (!(addr_tmp[dpos / 8] & (1 << (dpos % 8))))
          continue;
        if (addr_tmp_read[dpos / 8] & (1 << (dpos % 8)))
          continue;

        /* Verify the pointed-to StackLoc range is dead: check if any offset
         * in [base_off, max_stackloc_off+4] is still marked as read.
         * This catches cases where different Addr instructions to the same
         * StackLoc produce both read and write-only TMPs. */
        const Sym *addr_sym = NULL;
        int64_t addr_off;
        if (irop_get_tag(s) == IROP_TAG_SYMREF)
        {
          IRPoolSymref *sr = irop_get_symref_ex(ir, s);
          addr_sym = sr ? sr->sym : NULL;
          addr_off = sr ? sr->addend : 0;
        }
        else
        {
          addr_off = irop_get_stack_offset(s);
        }
        int range_is_read = 0;
        int64_t range_end = max_stackloc_off + 4;
        int64_t range_len = range_end - addr_off + 1;
        if (range_len > STACKLOC_HASH_SIZE * 8)
        {
          range_is_read = 1; /* Too large — conservatively assume read */
        }
        else
        {
          for (int64_t k = addr_off; k <= range_end; k++)
          {
            if (STACKLOC_TEST(addr_sym, k))
            {
              range_is_read = 1;
              break;
            }
          }
        }
        if (range_is_read)
          continue;

        q->op = TCCIR_OP_NOP;
        changes++;
        dead_tmp[dpos / 8] |= (1 << (dpos % 8));
      }

      /* Step 2: Forward-propagate dead values through the pointer chain.
       * NOP instructions whose source values are all dead (defined only
       * by NOP'd instructions), then mark their dests as dead too. */
      int prop_changed = 1;
      while (prop_changed)
      {
        prop_changed = 0;
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP)
            continue;
          /* Only propagate through data-flow instructions */
          if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
              q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
              q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID || q->op == TCCIR_OP_SWITCH_TABLE)
            continue;

          int has_dead_src = 0;

          if (irop_config[q->op].has_src1)
          {
            IROperand s = tcc_ir_op_get_src1(ir, q);
            int32_t vr = irop_get_vreg(s);
            if (vr >= 0)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos <= max_tmp_stackloc &&
                  (dead_tmp[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
              else if (dead_var && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos <= max_var_pos &&
                       (dead_var[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
            }
          }

          /* For STORE / STORE_INDEXED: check if dest is a dead TMP/VAR
           * used as pointer base.  STORE_INDEXED has the same dest-as-base
           * semantics as STORE — the dest carries the address, src1 the
           * value — so the same dead-base check kills the store. */
          if (!has_dead_src && (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED))
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t vr = irop_get_vreg(d);
            if (vr >= 0)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos <= max_tmp_stackloc &&
                  (dead_tmp[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
            }
          }

          if (has_dead_src)
          {
            /* Mark dest as dead */
            if (irop_config[q->op].has_dest)
            {
              IROperand d = tcc_ir_op_get_dest(ir, q);
              int32_t dvr = irop_get_vreg(d);
              if (dvr >= 0)
              {
                int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
                if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && dpos <= max_tmp_stackloc)
                  dead_tmp[dpos / 8] |= (1 << (dpos % 8));
                else if (dead_var && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && dpos <= max_var_pos)
                  dead_var[dpos / 8] |= (1 << (dpos % 8));
              }
            }
            q->op = TCCIR_OP_NOP;
            changes++;
            prop_changed = 1;
          }
        }
      }

      tcc_free(dead_tmp);
      if (dead_var)
        tcc_free(dead_var);
    }

    if (addr_tmp)
      tcc_free(addr_tmp);
    if (addr_tmp_read)
      tcc_free(addr_tmp_read);

#undef STACKLOC_HASH_SIZE
#undef STACKLOC_HASH
#undef STACKLOC_SET
#undef STACKLOC_TEST
  skip_dead_stackloc:;
  }

  /* Second pass: dead TMP elimination after VAR + StackLoc store elimination.
   * The first TMP pass (above) cannot eliminate e.g. T0 in:
   *   T0 <-- #7 [LOAD]          (defines T0)
   *   StackLoc[-32] <-- T0      (only use of T0)
   * because the STORE still uses T0.  After StackLoc store elimination removes
   * the STORE, T0 is dead but the first TMP pass already finished.
   * Re-run the same iterative elimination to catch these cascading deaths. */
  {
    uint8_t *used2 = tcc_mallocz((max_tmp_pos + 8) / 8);
    int iter2_changes;
    do
    {
      iter2_changes = 0;
      memset(used2, 0, (max_tmp_pos + 8) / 8);
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_src1)
        {
          const IROperand s = tcc_ir_op_get_src1(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        if (irop_config[q->op].has_src2)
        {
          const IROperand s = tcc_ir_op_get_src2(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        /* STORE/STORE_INDEXED dest is a use (pointer deref) */
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
              TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(d));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
          if (q->op == TCCIR_OP_FUNCPARAMVAL && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(d));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        if (q->op == TCCIR_OP_MLA)
        {
          const IROperand acc = tcc_ir_op_get_accum(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
      }

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        const IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
        {
          int is_dead_eligible =
              (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC &&
               q->op != TCCIR_OP_LOAD_POSTINC && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_FUNCCALLVAL &&
               q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID);
          if (!is_dead_eligible && q->op == TCCIR_OP_LOAD)
          {
            const IROperand src1 = tcc_ir_op_get_src1(ir, q);
            if (irop_is_immediate(src1))
              is_dead_eligible = 1;
          }
          if (is_dead_eligible)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
            if (pos <= max_tmp_pos && !(used2[pos / 8] & (1 << (pos % 8))))
            {
              q->op = TCCIR_OP_NOP;
              iter2_changes++;
            }
          }
        }
      }
      changes += iter2_changes;
    } while (iter2_changes > 0);
    tcc_free(used2);
  }

  return changes;
}

int tcc_ir_opt_dse_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dse(ctx->ir);
}

/* Dead address-taken VAR elimination.
 * After value tracking folds branches that read address-taken VARs (e.g.,
 * overflow builtin results), the ASSIGN + LEA + STORE sequences writing to
 * those VARs become dead. The regular DSE skips address-taken VARs, but this
 * pass can safely eliminate them by proving no live reads remain.
 *
 * A VAR is "dead" if:
 * 1. It's never read directly (not src1/src2 of any non-NOP, non-LEA instruction)
 * 2. All LEA pointers to it are only used as STORE destinations (write-only)
 */
int tcc_ir_opt_dead_var_store_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;

  int max_var = 0;
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
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var)
          max_var = pos;
      }
    }
  }
  if (max_var == 0)
    return 0;

  uint8_t *var_read = tcc_mallocz((max_var + 8) / 8);
  uint8_t *var_has_lea = tcc_mallocz((max_var + 8) / 8);
  int has_set_chain = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_SET_CHAIN || q->op == TCCIR_OP_INIT_CHAIN_SLOT)
      has_set_chain = 1;
    /* Track LEA instructions that take the address of a VAR */
    if (q->op == TCCIR_OP_LEA)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_has_lea[pos / 8] |= (1 << (pos % 8));
      }
    }
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_ASSIGN)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_var)
      continue;
    if (var_read[pos / 8] & (1 << (pos % 8)))
      continue;
    /* If addrtaken, check whether the VAR could actually be read through
     * a pointer.  Two cases:
     * 1) LEA exists for this VAR — pointer alias is live, skip.
     * 2) SET_CHAIN exists — explicit nested call in this function makes
     *    captured VARs reachable via the static chain.
     * If neither applies, the addrtaken flag is a stale frontend
     * annotation (e.g. capture by a now-fully-inlined nested function)
     * and the VAR is safe to eliminate. */
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
      if (interval && interval->addrtaken &&
          ((var_has_lea[pos / 8] & (1 << (pos % 8))) || has_set_chain))
        continue;
    }
    LOG_IR_GEN("=== DEAD VAR STORE: eliminating V%d at i=%d ===", pos, i);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  LOG_IR_GEN("=== DEAD VAR STORE ELIM: eliminated %d dead VAR stores ===", changes);
  tcc_free(var_has_lea);
  tcc_free(var_read);
  return changes;
}

/* Fold ADD-immediate + DEREF into LOAD_INDEXED with offset.
 *
 * Pattern:  T = base ADD #imm          (T is single-use TEMP)
 *           ... T***DEREF***            (only use, as lval/deref)
 *
 * Becomes:  T = LOAD_INDEXED base, #imm, scale=0   (T = *(base + imm))
 *           ... T                       (plain value, deref cleared)
 *
 * This lets the codegen emit `ldr Rd, [Rbase, #imm]` instead of
 * `add Rt, Rbase, #imm; ldr Rd, [Rt, #0]`, saving one instruction
 * and one register. */

int tcc_ir_opt_dead_addrvar_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Find max VAR and TMP positions */
  int max_var = 0, max_tmp = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
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
        if (type == TCCIR_VREG_TYPE_VAR && pos > max_var)
          max_var = pos;
        else if (type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp)
          max_tmp = pos;
      }
    }
  }

  if (max_var == 0)
    return 0;

  uint8_t *var_read = tcc_mallocz((max_var + 8) / 8);
  uint8_t *var_has_lea = tcc_mallocz((max_var + 8) / 8);
  int *lea_map = tcc_mallocz(sizeof(int) * (max_tmp + 1));
  int *var_lea = tcc_mallocz(sizeof(int) * (max_var + 1));
  for (int i = 0; i <= max_tmp; i++)
    lea_map[i] = -1;
  for (int i = 0; i <= max_var; i++)
    var_lea[i] = -1;

  /* Pass 1: Build LEA map and mark directly-read VARs.
   * LEA src1 (address-take) is NOT a value read.
   * STORE dest (pointer) is NOT a value read of the pointed-to VAR.
   * Everything else that references a VAR as src1/src2 is a value read. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_LEA)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s_vr = irop_get_vreg(src1);
      if (s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int var_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          /* LEA with TMP dest: trackable — record in lea_map */
          int tmp_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
          if (tmp_pos <= max_tmp && var_pos <= max_var)
          {
            lea_map[tmp_pos] = var_pos;
            var_has_lea[var_pos / 8] |= (1 << (var_pos % 8));
          }
        }
        else if (var_pos <= max_var)
        {
          /* LEA with VAR dest: pointer escapes into a VAR, conservatively mark as read */
          var_read[var_pos / 8] |= (1 << (var_pos % 8));
        }
      }
      continue;
    }

    /* LEA propagation through VARs: STORE V = T where T is LEA result */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest_op = tcc_ir_op_get_dest(ir, q);
      IROperand src1_op = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest_op);
      int32_t s_vr = irop_get_vreg(src1_op);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int d_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_tmp = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_pos <= max_var && s_tmp <= max_tmp && lea_map[s_tmp] >= 0)
          var_lea[d_pos] = lea_map[s_tmp];
      }
    }

    /* LEA propagation: ASSIGN T = V where V holds a LEA result */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
    {
      IROperand dest_op = tcc_ir_op_get_dest(ir, q);
      IROperand src1_op = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest_op);
      int32_t s_vr = irop_get_vreg(src1_op);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int d_tmp = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_tmp <= max_tmp && s_pos <= max_var && var_lea[s_pos] >= 0)
          lea_map[d_tmp] = var_lea[s_pos];
      }
    }

    /* Mark VARs read as src1 (for all instructions including STORE) */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }

    /* Mark VARs read as src2 */
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }
  }

  /* Pass 2: Mark VARs whose LEA pointers escape (used outside STORE dest).
   * If a LEA TMP appears as src1/src2 of any instruction, the VAR's address
   * may be used to read the VAR elsewhere — mark it as read.
   * Exception: STORE V = T (pointer copy to VAR) is tracked in Pass 1 and
   * does not constitute a read of the pointed-to VAR. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int is_ptr_copy = 0;
          if (q->op == TCCIR_OP_STORE)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t d_vr = irop_get_vreg(d);
            if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR)
              is_ptr_copy = 1;
          }
          if (!is_ptr_copy)
          {
            int var_pos = lea_map[tmp_pos];
            if (var_pos <= max_var)
              var_read[var_pos / 8] |= (1 << (var_pos % 8));
          }
        }
      }
    }

    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int var_pos = lea_map[tmp_pos];
          if (var_pos <= max_var)
            var_read[var_pos / 8] |= (1 << (var_pos % 8));
        }
      }
    }
  }

  /* Pass 2b: Mark VARs as read when their pointer escapes via FUNCPARAMVAL.
   * If a VAR in var_lea is passed as a function argument, the pointer
   * escapes to the callee which may read through it. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;
    IROperand param_val = tcc_ir_op_get_src1(ir, q);
    int32_t vr = irop_get_vreg(param_val);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos <= max_var && var_lea[pos] >= 0)
      {
        int target = var_lea[pos];
        if (target <= max_var)
          var_read[target / 8] |= (1 << (target % 8));
      }
    }
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
      {
        int target = lea_map[tmp_pos];
        if (target <= max_var)
          var_read[target / 8] |= (1 << (target % 8));
      }
    }
  }

  /* Pass 3: Eliminate dead writes to unread VARs */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* ASSIGN to dead VAR -> NOP (only if VAR has LEA, proving we track all accesses) */
    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var && (var_has_lea[pos / 8] & (1 << (pos % 8))) && !(var_read[pos / 8] & (1 << (pos % 8))))
        {
          q->op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }

    /* LEA from dead VAR -> NOP (only for TMP destinations; VAR dests handled by regular DSE) */
    if (q->op == TCCIR_OP_LEA)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var && (var_has_lea[pos / 8] & (1 << (pos % 8))) && !(var_read[pos / 8] & (1 << (pos % 8))))
          {
            q->op = TCCIR_OP_NOP;
            changes++;
          }
        }
      }
    }

    /* STORE through LEA to dead VAR -> NOP */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int var_pos = lea_map[tmp_pos];
          if (var_pos <= max_var && !(var_read[var_pos / 8] & (1 << (var_pos % 8))))
          {
            q->op = TCCIR_OP_NOP;
            changes++;
          }
        }
      }
    }
  }

  /* STORE to dead VAR (non-deref): V = val where V is unread */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos <= max_var && (var_has_lea[pos / 8] & (1 << (pos % 8))) && !(var_read[pos / 8] & (1 << (pos % 8))))
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }
  }

  LOG_IR_GEN("=== DEAD ADDRVAR ELIM: eliminated %d dead writes ===", changes);

  tcc_free(var_lea);
  tcc_free(lea_map);
  tcc_free(var_has_lea);
  tcc_free(var_read);
  return changes;
}

/* Redundant VAR ASSIGN elimination.
 * Forward scan within basic blocks: if a VAR is assigned and then assigned
 * again without being read in between, the first assign is dead → NOP it.
 * This catches patterns like repeated overflow flag stores where earlier
 * values are overwritten before use.
 */
int tcc_ir_opt_redundant_var_assign(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Find max VAR position */
  int max_var = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = tcc_ir_op_get_dest(ir, q);
    ops[1] = tcc_ir_op_get_src1(ir, q);
    ops[2] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < 3; k++)
    {
      int32_t vr = irop_get_vreg(ops[k]);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var)
          max_var = pos;
      }
    }
  }

  if (max_var == 0)
    return 0;

  /* Mark jump targets as merge points — must flush pending at these */
  uint8_t *is_target = tcc_mallocz((n + 7) / 8);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)dest.u.imm32;
      if (target >= 0 && target < n)
        is_target[target / 8] |= (1 << (target % 8));
    }
  }

  /* pending[v] = instruction index of last unread ASSIGN to VAR v, or -1 */
  int *pending = tcc_malloc(sizeof(int) * (max_var + 1));
  for (int v = 0; v <= max_var; v++)
    pending[v] = -1;

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Flush at merge points (jump targets) */
    if (is_target[i / 8] & (1 << (i % 8)))
    {
      for (int v = 0; v <= max_var; v++)
        pending[v] = -1;
    }

    /* Block boundary: flush all pending */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
        q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_SWITCH_TABLE)
    {
      /* First process reads in this instruction (src1/src2) */
      if (irop_config[q->op].has_src1)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var)
            pending[pos] = -1;
        }
      }
      /* Flush all */
      for (int v = 0; v <= max_var; v++)
        pending[v] = -1;
      continue;
    }

    /* Process reads: src1 and src2 clear pending for read VARs */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          pending[pos] = -1;
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          pending[pos] = -1;
      }
    }

    /* STORE dest is a pointer USE — if it's a VAR, count as read */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          pending[pos] = -1;
      }
      continue;
    }

    /* Process write: if dest is VAR, check for redundant prior assign */
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
        {
          if (pending[pos] >= 0)
          {
            /* Previous assign to this VAR is dead — overwritten before read */
            ir->compact_instructions[pending[pos]].op = TCCIR_OP_NOP;
            changes++;
          }
          pending[pos] = i;
        }
      }
    }
  }

  LOG_IR_GEN("=== REDUNDANT VAR ASSIGN: eliminated %d dead assigns ===", changes);

  tcc_free(pending);
  tcc_free(is_target);
  return changes;
}

/* vrp_swap_cmp_tok now in opt_utils.h */


int tcc_ir_opt_redundant_init_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n <= 1)
    return 0;

  /* Bail if function has indirect jumps (setjmp/longjmp, computed goto).
   * These create hidden control flow that our BFS doesn't follow. */
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  }

  /* Find function-entry VAR inits: instructions before any jump target
   * that assign a constant to a VAR. */
  for (int init_idx = 0; init_idx < n; init_idx++)
  {
    IRQuadCompact *q = &ir->compact_instructions[init_idx];
    if (q->is_jump_target)
      break;
    if (q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (!irop_is_immediate(src))
      continue;

    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
    if (interval && interval->addrtaken)
      continue;

    /* Forward reachability: check if V is killed on all paths before use.
     * State per instruction: 0=unvisited, 1=V-alive (init not yet killed),
     * 2=V-killed (redef seen on this path). */
    uint8_t *state = tcc_mallocz(n);
    int found_use_before_kill = 0;

    /* Worklist-based BFS from init_idx+1 */
    int *worklist = tcc_malloc(n * sizeof(int));
    int wl_head = 0, wl_tail = 0;
    worklist[wl_tail++] = init_idx + 1;
    state[init_idx] = 1;

    while (wl_head < wl_tail && !found_use_before_kill)
    {
      int idx = worklist[wl_head++];
      if (idx < 0 || idx >= n)
        continue;
      if (state[idx] == 2)
        continue; /* already killed on this path */
      if (state[idx] == 1)
        continue;     /* already queued as alive */
      state[idx] = 1; /* mark as V-alive */

      IRQuadCompact *iq = &ir->compact_instructions[idx];
      if (iq->op == TCCIR_OP_NOP)
      {
        if (idx + 1 < n)
          worklist[wl_tail++] = idx + 1;
        continue;
      }

      /* Check if this instruction USES V */
      if (irop_config[iq->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, iq)) == vr)
      {
        found_use_before_kill = 1;
        break;
      }
      if (irop_config[iq->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, iq)) == vr)
      {
        found_use_before_kill = 1;
        break;
      }
      /* STORE/FUNCPARAMVAL dest is a use */
      if ((iq->op == TCCIR_OP_STORE || iq->op == TCCIR_OP_STORE_INDEXED || iq->op == TCCIR_OP_FUNCPARAMVAL) &&
          irop_get_vreg(tcc_ir_op_get_dest(ir, iq)) == vr)
      {
        found_use_before_kill = 1;
        break;
      }

      /* Check if this instruction DEFINES (kills) V.
       * Only treat as kill if the source is explicit (immediate or vreg).
       * Bare defs like ASM outputs (V <-- with no visible source) may
       * implicitly depend on V's prior value via register constraints. */
      if (irop_config[iq->op].has_dest && iq->op != TCCIR_OP_STORE && iq->op != TCCIR_OP_STORE_INDEXED &&
          iq->op != TCCIR_OP_FUNCPARAMVAL)
      {
        IROperand d = tcc_ir_op_get_dest(ir, iq);
        if (irop_get_vreg(d) == vr)
        {
          int has_explicit_src = 0;
          if (irop_config[iq->op].has_src1)
          {
            IROperand s = tcc_ir_op_get_src1(ir, iq);
            if (irop_is_immediate(s) || irop_has_vreg(s))
              has_explicit_src = 1;
          }
          if (has_explicit_src)
          {
            state[idx] = 2; /* killed */
            continue;       /* don't follow successors — V is dead on this path */
          }
        }
      }

      /* Follow successors */
      if (iq->op == TCCIR_OP_JUMP)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, iq);
        int target = (int)irop_get_imm64_ex(ir, jd);
        if (target >= 0 && target < n && state[target] == 0)
          worklist[wl_tail++] = target;
      }
      else if (iq->op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, iq);
        int target = (int)irop_get_imm64_ex(ir, jd);
        if (target >= 0 && target < n && state[target] == 0)
          worklist[wl_tail++] = target;
        if (idx + 1 < n && state[idx + 1] == 0)
          worklist[wl_tail++] = idx + 1;
      }
      else if (iq->op == TCCIR_OP_RETURNVALUE)
      {
        /* Return with V alive but unused = V is dead (return doesn't use V
         * as an argument here since we checked src1 above) */
      }
      else
      {
        if (idx + 1 < n && state[idx + 1] == 0)
          worklist[wl_tail++] = idx + 1;
      }
    }

    tcc_free(worklist);
    tcc_free(state);

    if (!found_use_before_kill)
    {
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }

  return changes;
}


int tcc_ir_opt_dead_loop_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];
    if (loop->num_body_instrs == 0)
      continue;

    int has_side_effects = 0;
    int num_const_assigns = 0; (void)num_const_assigns;
    int has_loop_counter = 0;

    /* Track which VARs get constant assignments inside the loop body */
    typedef struct
    {
      int var_pos;
      int64_t value;
      int btype;
    } ConstVar;
    ConstVar const_vars[8];
    int num_const_vars = 0;

    for (int idx = loop->start_idx; idx <= loop->end_idx && idx < n; idx++)
    {
      IRQuadCompact *q = &ir->compact_instructions[idx];

      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
        continue;
      if (q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_TEST_ZERO)
        continue;

      /* Calls are side effects */
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        has_side_effects = 1;
        break;
      }

      /* Stores to memory (non-local) are side effects */
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
      {
        has_side_effects = 1;
        break;
      }

      /* PARAM instructions (function call setup) */
      if (q->op == TCCIR_OP_FUNCPARAMVOID || q->op == TCCIR_OP_FUNCPARAMVAL)
        continue;

      /* VAR <- immediate constant assignment: safe, track it */
      if (q->op == TCCIR_OP_ASSIGN)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t dest_vr = irop_get_vreg(dest);

        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR && irop_is_immediate(src1))
        {
          int var_pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
          int64_t val = irop_get_imm64_ex(ir, src1);
          int btype = irop_get_btype(src1);

          /* Check if we already track this VAR */
          int found = 0;
          for (int vi = 0; vi < num_const_vars; vi++)
          {
            if (const_vars[vi].var_pos == var_pos)
            {
              if (const_vars[vi].value != val)
              {
                has_side_effects = 1; /* Different values on different paths */
              }
              found = 1;
              break;
            }
          }
          if (has_side_effects)
            break;
          if (!found && num_const_vars < 8)
          {
            const_vars[num_const_vars].var_pos = var_pos;
            const_vars[num_const_vars].value = val;
            const_vars[num_const_vars].btype = btype;
            num_const_vars++;
          }
          num_const_assigns++;
          continue;
        }

        /* TMP <- anything (loop counter, etc): OK, no side effect */
        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
          continue;
      }

      /* ADD/SUB on TMPs or dead VARs (loop counters): safe.
       * A VAR modified by ADD/SUB is safe only if not used after the loop
       * (just a dead counter). If used after the loop, it's meaningful
       * accumulation (like sum += 1) and the loop is NOT dead. */
      if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_is_immediate(tcc_ir_op_get_src2(ir, q)))
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int32_t dest_vr = irop_get_vreg(dest);
        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          has_loop_counter = 1;
          continue;
        }
        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
        {
          int var_used_after = 0;
          for (int post = loop->end_idx + 1; post < n; post++)
          {
            IRQuadCompact *pq = &ir->compact_instructions[post];
            if (pq->op == TCCIR_OP_NOP)
              continue;
            if (irop_config[pq->op].has_src1)
            {
              int32_t s1 = irop_get_vreg(tcc_ir_op_get_src1(ir, pq));
              if (s1 == dest_vr)
              {
                var_used_after = 1;
                break;
              }
            }
            if (irop_config[pq->op].has_src2)
            {
              int32_t s2 = irop_get_vreg(tcc_ir_op_get_src2(ir, pq));
              if (s2 == dest_vr)
              {
                var_used_after = 1;
                break;
              }
            }
          }
          if (var_used_after)
          {
            has_side_effects = 1;
            break;
          }
          has_loop_counter = 1;
          continue;
        }
      }

      /* LOAD of a VAR (reading the result): safe */
      if (q->op == TCCIR_OP_LOAD)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (!dest.is_lval)
          continue;
      }

      /* Anything else is a potential side effect */
      has_side_effects = 1;
      break;
    }

    if (has_side_effects || num_const_vars == 0 || !has_loop_counter)
      continue;

    /* The loop body only contains constant VAR assignments and counter updates.
     * NOP all body instructions and place constant assignments in the preheader. */
    LOG_IR_GEN("OPTIMIZE: Dead loop elimination at header=%d (%d const vars)", loop->header_idx, num_const_vars);

    /* NOP loop body instructions within [start_idx, end_idx] only.
     * Instructions outside this range (exit targets, returns) must not be touched. */
    for (int idx = loop->start_idx; idx <= loop->end_idx && idx < n; idx++)
    {
      ir->compact_instructions[idx].op = TCCIR_OP_NOP;
    }

    /* Place constant assignments in the preheader (or at loop header).
     * Use the first available NOP slot at or before the header. */
    int insert_at = loop->preheader_idx >= 0 ? loop->preheader_idx : loop->header_idx;
    for (int vi = 0; vi < num_const_vars; vi++)
    {
      /* Find a NOP slot at or after insert_at */
      int slot = -1;
      for (int j = insert_at; j < n; j++)
      {
        if (ir->compact_instructions[j].op == TCCIR_OP_NOP)
        {
          slot = j;
          break;
        }
      }
      if (slot < 0)
        continue;

      int32_t dest_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, const_vars[vi].var_pos);
      ir->compact_instructions[slot].op = TCCIR_OP_ASSIGN;
      IROperand dest_op = irop_make_vreg(dest_vr, const_vars[vi].btype);
      tcc_ir_set_dest(ir, slot, dest_op);
      if (const_vars[vi].value == (int32_t)const_vars[vi].value)
        tcc_ir_set_src1(ir, slot, irop_make_imm32(-1, (int32_t)const_vars[vi].value, const_vars[vi].btype));
      else
      {
        uint32_t pool_idx = tcc_ir_pool_add_i64(ir, const_vars[vi].value);
        tcc_ir_set_src1(ir, slot, irop_make_i64(-1, pool_idx, const_vars[vi].btype));
      }
      tcc_ir_set_src2(ir, slot, IROP_NONE);
    }

    changes++;
  }

  tcc_ir_free_loops(loops);
  return changes;
}

int tcc_ir_opt_dead_loop_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_loop_elim(ctx->ir); }
int tcc_ir_opt_redundant_var_assign_ex(IROptCtx *ctx) { return tcc_ir_opt_redundant_var_assign(ctx->ir); }
int tcc_ir_opt_dead_var_store_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_var_store_elim(ctx->ir); }

