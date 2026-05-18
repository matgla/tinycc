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
#include "opt_engine.h"
#include "opt_hash.h"
#include "opt_du.h"
#include "opt_utils.h"
#include "licm.h"

int tcc_ir_opt_copy_prop(TCCIRState *ir)
{
  /* Track ASSIGN sources for TMP vregs.
   * A copy is: TMP:X <- VAR:Y or TMP:X <- PAR:Y (not TMP, not constant)
   * We can replace uses of TMP:X with the source, as long as the source
   * hasn't been redefined between the copy and the use.
   *
   * Uses generation counter: entry is valid only if entry.gen == current_gen.
   * Clears become O(1) by incrementing current_gen.
   */
  typedef struct
  {
    int gen;              /* Generation when this entry was recorded */
    int source_vr;        /* Source vreg */
    IROperand source;     /* Source of the ASSIGN */
    int next_same_source; /* Next TMP with same source_vr (per-generation list) */
  } CopyInfo;

  typedef struct
  {
    int head; /* Head of TMP list for this source */
    int gen;  /* Generation when head is valid */
  } SourceInfo;

  /* Stack buffers for small functions (covers most cases) */
#define COPY_PROP_STACK_TMP 64
#define COPY_PROP_STACK_VAR 32
#define COPY_PROP_STACK_PARAM 16
  CopyInfo copy_info_stack[COPY_PROP_STACK_TMP];
  SourceInfo var_sources_stack[COPY_PROP_STACK_VAR];
  SourceInfo param_sources_stack[COPY_PROP_STACK_PARAM];
  SourceInfo tmp_sources_stack[COPY_PROP_STACK_TMP];

  int n = ir->next_instruction_index;
  int changes = 0;
  int max_tmp_pos = 0;
  int max_var_pos = 0;
  int max_param_pos = 0;
  int any_tmp = 0;
  int current_gen = 1;   /* Generation counter, starts at 1 (0 means invalid) */
  int active_copies = 0; /* Number of active TMP copies in current_gen */
  int i;
  IRQuadCompact *q;
  CopyInfo *copy_info;
  SourceInfo *var_sources;
  SourceInfo *param_sources;
  SourceInfo *tmp_sources;
  void *heap_alloc = NULL; /* Single heap allocation if needed */
  int block_start_gen = 1; /* Generation for block start detection */
  int *block_start_seen;   /* Per-instruction: generation when marked as block start */
  int block_start_seen_stack[256];

  if (n == 0)
    return 0;

  /* Find max positions for TMP, VAR, and PARAM in a single pass */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      const int vr_type = TCCIR_DECODE_VREG_TYPE(dest_vr);
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (vr_type == TCCIR_VREG_TYPE_TEMP) {
        any_tmp = 1;
        if (pos > max_tmp_pos)
          max_tmp_pos = pos;
      }
      else if (vr_type == TCCIR_VREG_TYPE_VAR && pos > max_var_pos)
        max_var_pos = pos;
      else if (vr_type == TCCIR_VREG_TYPE_PARAM && pos > max_param_pos)
        max_param_pos = pos;
    }
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t src1_vr = irop_get_vreg(src1);
      const int vr_type = TCCIR_DECODE_VREG_TYPE(src1_vr);
      const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
      if (vr_type == TCCIR_VREG_TYPE_TEMP)
        any_tmp = 1;
      if (vr_type == TCCIR_VREG_TYPE_VAR && pos > max_var_pos)
        max_var_pos = pos;
      else if (vr_type == TCCIR_VREG_TYPE_PARAM && pos > max_param_pos)
        max_param_pos = pos;
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t src2_vr = irop_get_vreg(src2);
      const int vr_type = TCCIR_DECODE_VREG_TYPE(src2_vr);
      const int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
      if (vr_type == TCCIR_VREG_TYPE_TEMP)
        any_tmp = 1;
      if (vr_type == TCCIR_VREG_TYPE_VAR && pos > max_var_pos)
        max_var_pos = pos;
      else if (vr_type == TCCIR_VREG_TYPE_PARAM && pos > max_param_pos)
        max_param_pos = pos;
    }
  }

  if (!any_tmp)
    return 0;

  /* Use stack buffers if possible, otherwise single heap allocation */
  if (max_tmp_pos < COPY_PROP_STACK_TMP && max_var_pos < COPY_PROP_STACK_VAR && max_param_pos < COPY_PROP_STACK_PARAM &&
      n <= 256)
  {
    copy_info = copy_info_stack;
    var_sources = var_sources_stack;
    param_sources = param_sources_stack;
    tmp_sources = tmp_sources_stack;
    block_start_seen = block_start_seen_stack;
    /* Zero only what we need */
    memset(copy_info, 0, sizeof(CopyInfo) * (max_tmp_pos + 1));
    memset(var_sources, 0, sizeof(SourceInfo) * (max_var_pos + 1));
    memset(param_sources, 0, sizeof(SourceInfo) * (max_param_pos + 1));
    memset(tmp_sources, 0, sizeof(SourceInfo) * (max_tmp_pos + 1));
    memset(block_start_seen, 0, sizeof(int) * n);
  }
  else
  {
    /* Single allocation for all arrays */
    size_t copy_size = sizeof(CopyInfo) * (max_tmp_pos + 1);
    size_t var_size = sizeof(SourceInfo) * (max_var_pos + 1);
    size_t param_size = sizeof(SourceInfo) * (max_param_pos + 1);
    size_t tmp_src_size = sizeof(SourceInfo) * (max_tmp_pos + 1);
    size_t block_size = sizeof(int) * n;
    heap_alloc = tcc_mallocz(copy_size + var_size + param_size + tmp_src_size + block_size);
    copy_info = (CopyInfo *)heap_alloc;
    var_sources = (SourceInfo *)((char *)heap_alloc + copy_size);
    param_sources = (SourceInfo *)((char *)heap_alloc + copy_size + var_size);
    tmp_sources = (SourceInfo *)((char *)heap_alloc + copy_size + var_size + param_size);
    block_start_seen = (int *)((char *)heap_alloc + copy_size + var_size + param_size + tmp_src_size);
  }

  /* Mark block starts (shared helper) */
  ir_opt_mark_block_starts(ir, block_start_seen, block_start_gen, n);

  /* Single pass: process instructions in order, tracking and propagating copies */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];

    /* At block boundaries, invalidate all copies by incrementing generation */
    if (i != 0 && block_start_seen[i] == block_start_gen)
    {
      LOG_COPY_PROP("BB boundary at i=%d -> bump gen to %d", i, current_gen + 1);
      current_gen++;
      active_copies = 0;
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Propagate copies to uses in this instruction.
     * For non-lval uses: replace TMP:X with the copy source directly.
     * For lval uses (TMP:X***DEREF***): the copy records a register-to-register
     * copy of an address value (recording guards ensure source is NOT lval).
     * We can safely replace TMP:X***DEREF*** with TMP:Y***DEREF*** by preserving
     * the is_lval bit from the use site onto the copy source operand.
     * Also skip recording ASSIGN-with-lval as copies (those are LOADs).
     */

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int32_t src1_vr = irop_get_vreg(src1);
    if (active_copies > 0 && irop_config[q->op].has_src1 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
      if (pos <= max_tmp_pos && copy_info[pos].gen == current_gen)
      {
        /* For lval (DEREF) uses, only propagate TMP←TMP copies.
         * Propagating VAR/PAR into DEREF uses extends their live range past
         * function calls and other defs, potentially corrupting register allocation. */
        int src_type = TCCIR_DECODE_VREG_TYPE(copy_info[pos].source_vr);
        if (!src1.is_lval || src_type == TCCIR_VREG_TYPE_TEMP)
        {
          IROperand replacement = copy_info[pos].source;
          if (src1.is_lval)
          {
            replacement.is_lval = 1;                    /* Preserve DEREF semantics from use site */
            replacement.btype = src1.btype;             /* Preserve load width (e.g. INT16 for LDRH) */
            replacement.is_unsigned = src1.is_unsigned; /* Preserve signedness for load */
          }
          LOG_COPY_PROP("Propagate src1 TMP:%d -> vreg:%d (lval=%d) at i=%d", pos,
                        TCCIR_DECODE_VREG_POSITION(copy_info[pos].source_vr), src1.is_lval, i);
          tcc_ir_set_src1(ir, i, replacement);
          changes++;
        }
        else
        {
          LOG_COPY_PROP("Skip src1 TMP:%d (lval=%d src_type=%d) at i=%d op=%d", pos, src1.is_lval, src_type, i, q->op);
        }
      }
      else if (pos <= max_tmp_pos)
      {
        LOG_COPY_PROP("No copy for src1 TMP:%d (gen=%d cur=%d) at i=%d op=%d", pos, copy_info[pos].gen, current_gen, i,
                      q->op);
      }
    }

    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int32_t src2_vr = irop_get_vreg(src2);
    if (active_copies > 0 && irop_config[q->op].has_src2 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
      if (pos <= max_tmp_pos && copy_info[pos].gen == current_gen)
      {
        /* For lval (DEREF) uses, only propagate TMP←TMP copies.
         * Propagating VAR/PAR into DEREF uses extends their live range past
         * function calls and other defs, potentially corrupting register allocation. */
        int src_type = TCCIR_DECODE_VREG_TYPE(copy_info[pos].source_vr);
        if (!src2.is_lval || src_type == TCCIR_VREG_TYPE_TEMP)
        {
          IROperand replacement = copy_info[pos].source;
          if (src2.is_lval)
          {
            replacement.is_lval = 1;                    /* Preserve DEREF semantics from use site */
            replacement.btype = src2.btype;             /* Preserve load width (e.g. INT16 for LDRH) */
            replacement.is_unsigned = src2.is_unsigned; /* Preserve signedness for load */
          }
          LOG_COPY_PROP("Propagate src2 TMP:%d -> vreg:%d (lval=%d) at i=%d", pos,
                        TCCIR_DECODE_VREG_POSITION(copy_info[pos].source_vr), src2.is_lval, i);
          tcc_ir_set_src2(ir, i, replacement);
          changes++;
        }
      }
    }

    /* Propagate copies into STORE destinations.
     * For STORE: dest is TMP***DEREF*** (address to write to), src1 is the value.
     * If TMP was copied from another vreg, replace TMP***DEREF*** with src***DEREF***.
     *
     * Source kinds we accept:
     *   - TMP: standard TMP-to-TMP propagation.
     *   - PARAM/VAR: only if the source isn't is_local/is_llocal (i.e. the
     *     source holds a register-resident pointer, not a stack-relative
     *     address that would need an LEA at the use site).  copy_info
     *     already invalidates entries at FUNCCALL and BB boundaries, so
     *     the source value is guaranteed live with the same content here. */
    if (active_copies > 0 && q->op == TCCIR_OP_STORE && irop_config[q->op].has_dest)
    {
      IROperand store_dest = tcc_ir_op_get_dest(ir, q);
      int32_t store_dest_vr = irop_get_vreg(store_dest);
      if (store_dest.is_lval && TCCIR_DECODE_VREG_TYPE(store_dest_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        const int pos = TCCIR_DECODE_VREG_POSITION(store_dest_vr);
        if (pos <= max_tmp_pos && copy_info[pos].gen == current_gen)
        {
          int src_type = TCCIR_DECODE_VREG_TYPE(copy_info[pos].source_vr);
          IROperand src_op = copy_info[pos].source;
          int ok = (src_type == TCCIR_VREG_TYPE_TEMP);
          if (!ok && (src_type == TCCIR_VREG_TYPE_PARAM || src_type == TCCIR_VREG_TYPE_VAR) &&
              !src_op.is_local && !src_op.is_llocal)
            ok = 1;
          if (ok)
          {
            IROperand replacement = src_op;
            replacement.is_lval = 1;                          /* Preserve DEREF semantics */
            replacement.btype = store_dest.btype;             /* Preserve store width */
            replacement.is_unsigned = store_dest.is_unsigned; /* Preserve signedness */
            LOG_COPY_PROP("Propagate STORE dest TMP:%d -> vreg:%d at i=%d", pos,
                          TCCIR_DECODE_VREG_POSITION(copy_info[pos].source_vr), i);
            tcc_ir_set_dest(ir, i, replacement);
            changes++;
          }
        }
      }
    }

    /* If this instruction defines a VAR/PAR/TMP, invalidate any copies that use it as source.
     * Uses per-source reverse list to avoid scanning all TMPs.
     * Skip STORE dests: STORE writes THROUGH the pointer (dest is a USE, not a DEF).
     * The dest.is_lval flag distinguishes pointer dereferences from true definitions. */
    if (active_copies > 0 && irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      const int dest_type = TCCIR_DECODE_VREG_TYPE(dest_vr);
      if (dest.is_lval)
        goto skip_invalidation; /* STORE dest is a pointer use, not a redefinition */
      if (dest_type == TCCIR_VREG_TYPE_VAR || dest_type == TCCIR_VREG_TYPE_PARAM || dest_type == TCCIR_VREG_TYPE_TEMP)
      {
        int dest_pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        SourceInfo *src_info = NULL;
        if (dest_type == TCCIR_VREG_TYPE_VAR && dest_pos <= max_var_pos)
          src_info = &var_sources[dest_pos];
        else if (dest_type == TCCIR_VREG_TYPE_PARAM && dest_pos <= max_param_pos)
          src_info = &param_sources[dest_pos];
        else if (dest_type == TCCIR_VREG_TYPE_TEMP && dest_pos <= max_tmp_pos)
          src_info = &tmp_sources[dest_pos];

        if (src_info && src_info->gen == current_gen)
        {
          int tmp_pos = src_info->head;
          while (tmp_pos >= 0)
          {
            int next = copy_info[tmp_pos].next_same_source;
            if (copy_info[tmp_pos].gen == current_gen && copy_info[tmp_pos].source_vr == dest_vr)
            {
              LOG_COPY_PROP("Invalidate TMP:%d (source vreg:%d type=%d redefined) at i=%d", tmp_pos, dest_pos,
                            dest_type, i);
              copy_info[tmp_pos].gen = 0;
              if (active_copies > 0)
                active_copies--;
            }
            tmp_pos = next;
          }
          src_info->head = -1;
        }
      }
    }
  skip_invalidation:

    /* Clear all copies at basic block boundaries - O(1) operation */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL)
    {
      LOG_COPY_PROP("terminator op=%d at i=%d -> bump gen to %d", q->op, i, current_gen + 1);
      current_gen++;
      active_copies = 0;
    }

    /* If this is a copy (ASSIGN TMP <- VAR/PAR), record it */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_dest &&
        TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (pos <= max_tmp_pos)
      {
        int src_is_const = irop_is_immediate(src1);
        int src_vreg_type = TCCIR_DECODE_VREG_TYPE(src1_vr);

        /* Allow propagation if source is VAR, PAR, or TMP (not constant, not lval).
         * ASSIGN-with-lval is semantically a LOAD, not a copy - we must NOT
         * propagate lval sources as that would re-load from potentially stale memory.
         * Also require matching types: e.g. UMULL produces 64-bit T9, then
         * T10 <-- T9 [ASSIGN] truncates to 32-bit; that's NOT a copy. */
        /* Allow btype mismatch for register-width types (INT32, STRUCT pointer,
         * FUNC pointer are all 32-bit on ARM and interchangeable in registers). */
        {
          int db = irop_get_btype(dest), sb = irop_get_btype(src1);
          int btype_compat =
              (db == sb) ||
              (db != IROP_BTYPE_INT64 && db != IROP_BTYPE_FLOAT32 && db != IROP_BTYPE_FLOAT64 &&
               sb != IROP_BTYPE_INT64 && sb != IROP_BTYPE_FLOAT32 && sb != IROP_BTYPE_FLOAT64 &&
               db != IROP_BTYPE_INT8 && db != IROP_BTYPE_INT16 && sb != IROP_BTYPE_INT8 && sb != IROP_BTYPE_INT16);
          if (!src_is_const && src1_vr >= 0 && !src1.is_lval && btype_compat &&
              (src_vreg_type == TCCIR_VREG_TYPE_VAR || src_vreg_type == TCCIR_VREG_TYPE_PARAM ||
               src_vreg_type == TCCIR_VREG_TYPE_TEMP))
          {
            int src_pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
            SourceInfo *src_info = NULL;

            if (src_vreg_type == TCCIR_VREG_TYPE_VAR && src_pos <= max_var_pos)
              src_info = &var_sources[src_pos];
            else if (src_vreg_type == TCCIR_VREG_TYPE_PARAM && src_pos <= max_param_pos)
              src_info = &param_sources[src_pos];
            else if (src_vreg_type == TCCIR_VREG_TYPE_TEMP && src_pos <= max_tmp_pos)
              src_info = &tmp_sources[src_pos];

            if (src_info)
            {
              if (src_info->gen != current_gen)
              {
                src_info->head = -1;
                src_info->gen = current_gen;
              }
              copy_info[pos].next_same_source = src_info->head;
              src_info->head = pos;
            }

            if (copy_info[pos].gen != current_gen)
              active_copies++;
            copy_info[pos].gen = current_gen;
            copy_info[pos].source_vr = src1_vr;
            copy_info[pos].source = src1;
            LOG_COPY_PROP("Record TMP:%d <- vreg:%d (type=%d) at i=%d", pos, TCCIR_DECODE_VREG_POSITION(src1_vr),
                          src_vreg_type, i);
          }
          else
          {
            /* TMP is assigned something other than a simple VAR/PAR copy - invalidate */
            LOG_COPY_PROP("Reject record TMP:%d at i=%d is_const=%d src1_vr=%d lval=%d "
                          "dest_btype=%d src_btype=%d src_vreg_type=%d",
                          pos, i, src_is_const, src1_vr, src1.is_lval, irop_get_btype(dest), irop_get_btype(src1),
                          src_vreg_type);
            if (copy_info[pos].gen == current_gen && active_copies > 0)
              active_copies--;
            copy_info[pos].gen = 0;
            copy_info[pos].next_same_source = -1;
          }
        }
      }
    }
    else if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      /* TMP is defined by a non-ASSIGN instruction - invalidate any copy for it */
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (pos <= max_tmp_pos)
      {
        if (copy_info[pos].gen == current_gen && active_copies > 0)
          active_copies--;
        copy_info[pos].gen = 0;
        copy_info[pos].next_same_source = -1;
      }
    }
  }

  if (heap_alloc)
    tcc_free(heap_alloc);

#undef COPY_PROP_STACK_TMP
#undef COPY_PROP_STACK_VAR
#undef COPY_PROP_STACK_PARAM

  return changes;
}

/* Boolean CSE and Idempotent Optimization Pass
 *
 * This pass combines boolean CSE with idempotent boolean optimizations:
 * - CSE: (a && b) && c  ->  t = a && b;  t && c (reuses computed boolean)
 *        (a || b) || c  ->  t = a || b;  t || c
 * - Idempotent: a && a  ->  a
 *              a || a  ->  a
 *              a && 1  ->  a
 *              a || 0  ->  a
 *
 * The optimizations are applied iteratively until no more changes occur.
 * Benefits: Reduces redundant boolean evaluations and temporary allocations.
 */

/* Hash table for tracking boolean ops for CSE */
/* BoolCSE helpers using generic IROptHashTable.
 * extra[0] = op (BOOL_AND/BOOL_OR), extra[1] = left_vr, extra[2] = right_vr */
typedef struct
{
  int op;
  int left_vr;
  int right_vr;
} BoolCSEKey;

static uint32_t bool_cse_hash(int op, int left_vr, int right_vr)
{
  if (left_vr > right_vr)
  {
    int tmp = left_vr;
    left_vr = right_vr;
    right_vr = tmp;
  }
  return (uint32_t)op * 31 + (uint32_t)left_vr * 17 + (uint32_t)right_vr;
}

static int bool_cse_eq(const IROptHashEntry *e, const void *key)
{
  const BoolCSEKey *k = (const BoolCSEKey *)key;
  return e->extra[0] == k->op && e->extra[1] == k->left_vr && e->extra[2] == k->right_vr;
}



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

/* ============================================================================
 * GlobalSym CSE — hoist repeated global symbol addresses
 * ============================================================================
 * Pattern: multiple ADD instructions in the same basic block use the same
 * GlobalSym(S)+offset as src1 (e.g., AES table base addresses).  Each use
 * generates a separate literal pool load in the backend.
 *
 * Transform: find a NOP before the first use, convert it to
 *   T_base <-- GlobalSym(S)+offset [ASSIGN]
 * then replace subsequent GlobalSym src1 operands with T_base.
 */
#define GSYM_CSE_MAX 16

typedef struct
{
  Sym *sym;
  int64_t addend;
  int count;
  int has_lval;
} GSymEntry;

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

static void gsym_cse_count(TCCIRState *ir, IROperand op, GSymEntry *entries, int *num_entries)
{
  if (irop_get_tag(op) != IROP_TAG_SYMREF)
    return;
  IRPoolSymref *sr = irop_get_symref_ex(ir, op);
  if (!sr || !sr->sym)
    return;
  for (int e = 0; e < *num_entries; e++)
  {
    if (entries[e].sym == sr->sym && entries[e].addend == sr->addend)
    {
      if (op.is_lval)
        entries[e].has_lval = 1;
      else
        entries[e].count++;
      return;
    }
  }
  if (*num_entries < GSYM_CSE_MAX)
  {
    entries[*num_entries].sym = sr->sym;
    entries[*num_entries].addend = sr->addend;
    entries[*num_entries].has_lval = op.is_lval;
    entries[*num_entries].count = op.is_lval ? 0 : 1;
    (*num_entries)++;
  }
}

int tcc_ir_opt_globalsym_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  GSymEntry entries[GSYM_CSE_MAX];
  int num_entries = 0;

  /* Scan instructions that commonly carry GlobalSym operands.
   * Only ADD src1 carries non-lval SYMREFs (address + offset).
   * LOAD/STORE/FUNCPARAMVAL/ASSIGN carry lval SYMREFs — we track
   * those to avoid hoisting symbols that later passes might mishandle. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
    case TCCIR_OP_ADD:
    {
      int off = irop_config[TCCIR_OP_ADD].has_dest;
      if (q->operand_base + off < (uint32_t)ir->iroperand_pool_count)
        gsym_cse_count(ir, ir->iroperand_pool[q->operand_base + off],
                       entries, &num_entries);
      break;
    }
    case TCCIR_OP_LOAD:
    case TCCIR_OP_STORE:
    case TCCIR_OP_ASSIGN:
    case TCCIR_OP_FUNCPARAMVAL:
    {
      int nops = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
      for (int k = 0; k < nops; k++)
        if (q->operand_base + k < (uint32_t)ir->iroperand_pool_count)
        {
          IROperand op = ir->iroperand_pool[q->operand_base + k];
          if (op.tag == IROP_TAG_SYMREF)
            gsym_cse_count(ir, op, entries, &num_entries);
        }
      break;
    }
    default:
      break;
    }
  }

  /* Determine how many entries qualify for hoisting (count >= 3). */
  int max_gsym_hoist = tcc_ir_estimate_hoist_budget(ir, 0, n - 1, ir->parameters_count);
  if (max_gsym_hoist < 2)
    max_gsym_hoist = 2;

  /* Sort entries by use count descending so the most-used bases get priority */
  for (int i = 0; i < num_entries - 1; i++)
    for (int j = i + 1; j < num_entries; j++)
      if (entries[j].count > entries[i].count)
      {
        GSymEntry tmp = entries[i];
        entries[i] = entries[j];
        entries[j] = tmp;
      }

  /* Count how many will actually be hoisted and allocate their vregs. */
  int32_t hoist_vregs[GSYM_CSE_MAX];
  int num_hoist = 0;
  for (int e = 0; e < num_entries && num_hoist < max_gsym_hoist; e++)
  {
    if (entries[e].count < 3 || entries[e].has_lval)
      continue;
    hoist_vregs[num_hoist] = tcc_ir_vreg_alloc_temp(ir);
    entries[e].count = -(num_hoist + 1); /* tag: negative = hoist slot index */
    num_hoist++;
  }
  if (num_hoist == 0)
    return 0;

  /* Build all ASSIGN instructions and batch-insert at position 0.
   * Single array shift + single jump/switch-table patch pass. */
  {
    int new_n = n + num_hoist;
    while (new_n >= ir->compact_instructions_size)
    {
      int new_size = ir->compact_instructions_size << 1;
      ir->compact_instructions = tcc_realloc(ir->compact_instructions,
                                             sizeof(IRQuadCompact) * new_size);
      ir->compact_instructions_size = new_size;
    }
    /* Shift existing instructions right by num_hoist */
    for (int i = n - 1; i >= 0; i--)
      ir->compact_instructions[i + num_hoist] = ir->compact_instructions[i];
    ir->next_instruction_index = new_n;

    /* Fill the first num_hoist slots with ASSIGN instructions */
    for (int h = 0; h < num_hoist; h++)
    {
      /* Find the entry that maps to hoist slot h */
      GSymEntry *ge = NULL;
      for (int e = 0; e < num_entries; e++)
        if (entries[e].count == -(h + 1)) { ge = &entries[e]; break; }

      uint32_t pool_idx = tcc_ir_pool_add_symref(ir, ge->sym,
                                                  (int32_t)ge->addend, 0);
      IROperand sym_op = irop_make_symref(-1, pool_idx, 0, 0, 0, IROP_BTYPE_INT32);
      IROperand dest_op = irop_make_vreg(hoist_vregs[h], IROP_BTYPE_INT32);
      IRQuadCompact aq = {0};
      aq.op = TCCIR_OP_ASSIGN;
      aq.operand_base = tcc_ir_pool_add(ir, dest_op);
      tcc_ir_pool_add(ir, sym_op);
      ir->compact_instructions[h] = aq;
    }

    /* Patch jump targets: add num_hoist to all targets >= 0 */
    for (int i = 0; i < new_n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int target = (int)irop_get_imm64_ex(ir, dest);
        if (target >= 0)
          tcc_ir_op_set_dest(ir, q,
                             irop_make_imm32(-1, target + num_hoist, IROP_BTYPE_INT32));
      }
    }
    for (int t = 0; t < ir->num_switch_tables; t++)
    {
      TCCIRSwitchTable *table = &ir->switch_tables[t];
      if (table->default_target >= 0)
        table->default_target += num_hoist;
      if (table->targets)
        for (int j = 0; j < table->num_entries; j++)
          if (table->targets[j] >= 0)
            table->targets[j] += num_hoist;
    }
  }

  /* Replace matching non-lval GlobalSym ADD src1 operands with their TEMPs.
   * Only ADD instructions carry non-lval SYMREFs as src1 (address + offset). */
  int nn = ir->next_instruction_index;
  for (int j = num_hoist; j < nn; j++)
  {
    IRQuadCompact *rq = &ir->compact_instructions[j];
    if (rq->op != TCCIR_OP_ADD)
      continue;
    int off = irop_config[TCCIR_OP_ADD].has_dest;
    if (rq->operand_base + off >= (uint32_t)ir->iroperand_pool_count)
      continue;
    IROperand s1 = ir->iroperand_pool[rq->operand_base + off];
    if (s1.tag != IROP_TAG_SYMREF || s1.is_lval)
      continue;
    IRPoolSymref *sr = irop_get_symref_ex(ir, s1);
    if (!sr || !sr->sym)
      continue;
    for (int h = 0; h < num_entries; h++)
    {
      if (entries[h].count < 0 && entries[h].sym == sr->sym &&
          entries[h].addend == sr->addend)
      {
        int slot = -(entries[h].count + 1);
        tcc_ir_op_set_src1(ir, rq,
                           irop_make_vreg(hoist_vregs[slot], IROP_BTYPE_INT32));
        changes++;
        break;
      }
    }
  }

  return changes;
}

/* Narrow CSE: deduplicate PARAM/VAR + #constant expressions.
 * Safe because params are immutable and the pattern is side-effect-free.
 * Handles repeated `P3 ADD #8` in set_key (12 occurrences → 1). */
int tcc_ir_opt_cse_param_add(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  /* Detect setjmp — STACKOFF CSE is unsafe across setjmp boundaries */
  int has_no_setjmp = 1;
  for (int si = 0; si < n && has_no_setjmp; si++)
  {
    TccIrOp sop = ir->compact_instructions[si].op;
    if (sop == TCCIR_OP_SETJMP || sop == TCCIR_OP_NL_SETJMP || sop == TCCIR_OP_VLA_ALLOC)
      has_no_setjmp = 0;
  }

#define PCSE_HASH_SIZE 64
#define PCSE_MAX_ENTRIES 128
  typedef struct
  {
    int32_t src_vr;
    int64_t imm_val;
    int32_t result_vr;
    int instr_idx;
    int valid;
  } PCSEEntry;

  PCSEEntry entries[PCSE_MAX_ENTRIES];
  int entry_count = 0;
  int current_gen = 1; (void)current_gen;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Invalidate at block boundaries */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVAL ||
        q->op == TCCIR_OP_FUNCCALLVOID)
    {
      current_gen++;
      entry_count = 0;
    }

    /* Reset at jump targets (new BB) */
    if (i > 0 && q->is_jump_target)
    {
      current_gen++;
      entry_count = 0;
    }

    /* Invalidate CSE entries when ANY instruction writes to a PARAM, VAR, or STACKOFF */
    if (entry_count > 0 && irop_config[q->op].has_dest)
    {
      IROperand wd = tcc_ir_op_get_dest(ir, q);
      if (!wd.is_lval)
      {
        int32_t wvr = irop_get_vreg(wd);
        if (tcc_ir_vreg_is_valid(ir, wvr))
        {
          int wt = TCCIR_DECODE_VREG_TYPE(wvr);
          if (wt == TCCIR_VREG_TYPE_VAR || wt == TCCIR_VREG_TYPE_PARAM)
          {
            for (int e = 0; e < entry_count; e++)
              if (entries[e].valid && entries[e].src_vr == wvr)
                entries[e].valid = 0;
          }
        }
      }
      else if (has_no_setjmp && wd.is_lval && irop_get_tag(wd) == IROP_TAG_STACKOFF && wd.is_local)
      {
        int32_t w_vr = irop_get_vreg(wd);
        if (tcc_ir_vreg_is_valid(ir, w_vr))
        {
          int32_t syn_key = (int32_t)(0x70000000 | ((uint32_t)w_vr & 0x0FFFFFFF));
          for (int e = 0; e < entry_count; e++)
            if (entries[e].valid && entries[e].src_vr == syn_key)
              entries[e].valid = 0;
        }
      }
    }

    if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);

    if (!irop_config[q->op].has_src1 || !irop_config[q->op].has_src2 || !irop_config[q->op].has_dest)
      continue;
    if (dest.is_lval)
      continue;

    int32_t src_vr = irop_get_vreg(src1);
    int src1_tag = irop_get_tag(src1);
    int is_stackoff_lval = (src1_tag == IROP_TAG_STACKOFF && src1.is_lval && src1.is_local);

    if (!is_stackoff_lval)
    {
      if (src1.is_lval)
        continue;
      if (!tcc_ir_vreg_is_valid(ir, src_vr))
        continue;
      int src_type = TCCIR_DECODE_VREG_TYPE(src_vr);
      if (src_type != TCCIR_VREG_TYPE_PARAM)
        continue;
    }
    else
    {
      if (!has_no_setjmp)
        continue;
      if (!tcc_ir_vreg_is_valid(ir, src_vr))
        continue;
      src_vr = (int32_t)(0x70000000 | ((uint32_t)src_vr & 0x0FFFFFFF));
    }

    if (!irop_is_immediate(src2))
      continue;
    int64_t imm = irop_get_imm64_ex(ir, src2);

    int32_t dest_vr = irop_get_vreg(dest);
    if (!tcc_ir_vreg_is_valid(ir, dest_vr))
      continue;
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Encode op into the key (ADD vs SUB) */
    int64_t key_imm = (q->op == TCCIR_OP_SUB) ? -imm : imm;

    /* Search for existing entry */
    int found = -1;
    for (int e = 0; e < entry_count; e++)
    {
      if (entries[e].valid && entries[e].src_vr == src_vr && entries[e].imm_val == key_imm)
      {
        found = e;
        break;
      }
    }

    if (found >= 0)
    {
      q->op = TCCIR_OP_ASSIGN;
      IROperand reuse = irop_make_vreg(entries[found].result_vr, dest.btype);
      int pool_off = q->operand_base + irop_config[TCCIR_OP_ASSIGN].has_dest;
      ir->iroperand_pool[pool_off] = reuse;
      changes++;
    }
    else if (entry_count < PCSE_MAX_ENTRIES)
    {
      entries[entry_count].src_vr = src_vr;
      entries[entry_count].imm_val = key_imm;
      entries[entry_count].result_vr = dest_vr;
      entries[entry_count].instr_idx = i;
      entries[entry_count].valid = 1;
      entry_count++;
    }

    /* Note: PARAM/VAR invalidation already handled at the top of the loop */
  }

#undef PCSE_HASH_SIZE
#undef PCSE_MAX_ENTRIES
  return changes;
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

/* ============================================================================
 * Local ALU CSE  (tcc_ir_opt_local_alu_cse)
 * ============================================================================
 *
 * Within a basic block, when the same arithmetic op produces equal values
 * (same opcode + same operands, including the optional MLA accumulator),
 * the second occurrence is replaced with an ASSIGN copy of the first.
 *
 * This complements ssa_opt_gvn for two cases that GVN cannot handle:
 *   1. VAR-typed sources (multiple defs across the function) that happen
 *      to be unchanged within a single BB — e.g. the loop induction
 *      variable V used as `&arr[V]` (== V * stride + base) at every
 *      array access in the loop body.
 *   2. MLA: GVN runs before MLA fusion, so MLAs created later are never
 *      seen by GVN.
 *
 * Cache is reset on:
 *   - basic-block boundary (jump target)
 *   - any control-flow / call instruction
 *   - definition of any vreg currently used as a key in the cache
 * ============================================================================ */
int tcc_ir_opt_local_alu_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;
  int dbg_alu = (getenv("TCC_DBG_CSE") != NULL);
  if (dbg_alu)
    fprintf(stderr, "[local_alu_cse] entering, n=%d\n", n);

#define LACSE_MAX 32
  struct LACSEEntry
  {
    int op;
    uint8_t s1_tag, s2_tag, s3_tag;
    uint8_t s1_lval, s2_lval, s3_lval; /* lval-flag for each src — used for STORE invalidation */
    int32_t s1_vr, s2_vr, s3_vr;
    int32_t s1_imm, s2_imm, s3_imm;
    int32_t dest_vr;
  };
  struct LACSEEntry cache[LACSE_MAX];
  int cache_count = 0;

  /* Operand key extractor: returns (tag, vreg, imm) so two operands compare
   * equal iff they refer to the same value. */
  #define EXTRACT_KEY(op_, tag_, vr_, imm_)                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
      (tag_) = (op_).tag;                                                                                              \
      (vr_) = irop_get_vreg(op_);                                                                                      \
      if ((op_).tag == IROP_TAG_IMM32 || (op_).tag == IROP_TAG_F32 || (op_).tag == IROP_TAG_STACKOFF)                  \
        (imm_) = (op_).u.imm32;                                                                                        \
      else if ((op_).tag == IROP_TAG_SYMREF || (op_).tag == IROP_TAG_I64 || (op_).tag == IROP_TAG_F64)                 \
        (imm_) = (int32_t)(op_).u.pool_idx;                                                                            \
      else                                                                                                             \
        (imm_) = 0;                                                                                                    \
    } while (0)

  /* Returns 1 if the op is a pure arithmetic op safe to CSE.
   * Excludes ops with side effects (CMP sets flags, STORE writes memory) and
   * ops whose result depends on more than just the operand values. */
  #define IS_CSE_PURE(op)                                                                                              \
    ((op) == TCCIR_OP_ADD || (op) == TCCIR_OP_SUB || (op) == TCCIR_OP_MUL || (op) == TCCIR_OP_MLA ||                   \
     (op) == TCCIR_OP_AND || (op) == TCCIR_OP_OR || (op) == TCCIR_OP_XOR || (op) == TCCIR_OP_SHL ||                    \
     (op) == TCCIR_OP_SHR || (op) == TCCIR_OP_SAR || (op) == TCCIR_OP_ROR)

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Reset at basic-block boundaries. */
    if (q->is_jump_target)
      cache_count = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Control flow / calls flush the cache: callees can mutate any
     * addrtaken VAR, so cached entries depending on VARs become stale. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      cache_count = 0;
      continue;
    }

    /* Invalidate cache entries based on this instruction's effects. Two cases:
     *   1. STORE / STORE_INDEXED / STORE_POSTINC: writes memory — kill any
     *      entry whose src is an lval (memory read). Conservative on aliasing.
     *      Also: STORE with is_lval=0 dest is a direct write to the dest vreg
     *      (e.g. `P0 = T4` updates P0), so we must also kill entries reading
     *      that vreg directly.
     *   2. Any op with has_dest writing to a vreg V: kill entries whose src
     *      uses V (V's value just changed). Particularly important for VAR
     *      redefinition like the loop induction variable increment. */
    int is_store_like = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                         q->op == TCCIR_OP_STORE_POSTINC);
    int32_t dest_vr_kill = -1;
    if (irop_config[q->op].has_dest)
    {
      IROperand dest_op = tcc_ir_op_get_dest(ir, q);
      dest_vr_kill = irop_get_vreg(dest_op);
    }
    if (is_store_like || dest_vr_kill >= 0)
    {
      int w = 0;
      for (int c = 0; c < cache_count; c++)
      {
        int kills = 0;
        if (is_store_like && (cache[c].s1_lval || cache[c].s2_lval || cache[c].s3_lval))
          kills = 1;
        if (dest_vr_kill >= 0)
        {
          if (cache[c].s1_tag == IROP_TAG_VREG && cache[c].s1_vr == dest_vr_kill)
            kills = 1;
          else if (cache[c].s2_tag == IROP_TAG_VREG && cache[c].s2_vr == dest_vr_kill)
            kills = 1;
          else if (cache[c].s3_tag == IROP_TAG_VREG && cache[c].s3_vr == dest_vr_kill)
            kills = 1;
          else if (cache[c].dest_vr == dest_vr_kill)
            kills = 1; /* this op redefines a previously-cached dest — drop entry */
        }
        if (!kills)
          cache[w++] = cache[c];
      }
      cache_count = w;
    }
    if (is_store_like)
      continue; /* STORE itself isn't an ALU op — don't try to cache it */

    if (!IS_CSE_PURE(q->op))
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);

    /* Skip llocal (indirect-via-pointer) — too aliasing-sensitive. */
    if (src1.is_llocal || src2.is_llocal)
      continue;

    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0)
      continue;

    /* Don't cache when dest itself is an lval (means STORE, not arithmetic). */
    if (dest.is_lval)
      continue;
    /* Don't replace VAR/PARAM defs — only TEMP defs.  Replacing a VAR def
     * with ASSIGN is unsafe because:
     *   - VAR has multiple defs across the function (it's a stack slot)
     *   - The cached_dest may be a VAR/TEMP whose value differs at the next
     *     def site if there's any path where its value isn't computed.
     *   - cprop on the resulting `VAR <-- TEMP [ASSIGN]` may not propagate
     *     the way we expect, leaving stale uses. */
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int is_mla = (q->op == TCCIR_OP_MLA);
    IROperand accum = IROP_NONE;
    if (is_mla)
    {
      accum = tcc_ir_op_get_accum(ir, q);
      if (accum.is_llocal)
        continue;
    }

    uint8_t s1_tag, s2_tag, s3_tag = 0;
    int32_t s1_vr, s2_vr, s3_vr = 0;
    int32_t s1_imm, s2_imm, s3_imm = 0;
    EXTRACT_KEY(src1, s1_tag, s1_vr, s1_imm);
    EXTRACT_KEY(src2, s2_tag, s2_vr, s2_imm);
    if (is_mla)
      EXTRACT_KEY(accum, s3_tag, s3_vr, s3_imm);

    uint8_t s1_lval_q = src1.is_lval;
    uint8_t s2_lval_q = src2.is_lval;
    uint8_t s3_lval_q = is_mla ? accum.is_lval : 0;

    /* Look up in cache. */
    int found = -1;
    for (int c = 0; c < cache_count; c++)
    {
      if (cache[c].op != q->op)
        continue;
      if (cache[c].s1_tag == s1_tag && cache[c].s1_lval == s1_lval_q && cache[c].s1_vr == s1_vr &&
          cache[c].s1_imm == s1_imm && cache[c].s2_tag == s2_tag && cache[c].s2_lval == s2_lval_q &&
          cache[c].s2_vr == s2_vr && cache[c].s2_imm == s2_imm && cache[c].s3_tag == s3_tag &&
          cache[c].s3_lval == s3_lval_q && cache[c].s3_vr == s3_vr && cache[c].s3_imm == s3_imm)
      {
        found = c;
        break;
      }
      /* Commutative: ADD/MUL/AND/OR/XOR/MLA's mul pair commute on src1<->src2. */
      if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_OR ||
          q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_MLA)
      {
        if (cache[c].s1_tag == s2_tag && cache[c].s1_lval == s2_lval_q && cache[c].s1_vr == s2_vr &&
            cache[c].s1_imm == s2_imm && cache[c].s2_tag == s1_tag && cache[c].s2_lval == s1_lval_q &&
            cache[c].s2_vr == s1_vr && cache[c].s2_imm == s1_imm && cache[c].s3_tag == s3_tag &&
            cache[c].s3_lval == s3_lval_q && cache[c].s3_vr == s3_vr && cache[c].s3_imm == s3_imm)
        {
          found = c;
          break;
        }
      }
    }

    if (found >= 0)
    {
      /* Replace with ASSIGN dest = cached_dest. */
      IROperand new_src = irop_make_vreg(cache[found].dest_vr, dest.btype);
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_op_set_src1(ir, q, new_src);
      tcc_ir_op_set_src2(ir, q, IROP_NONE);
      if (is_mla)
        tcc_ir_op_set_accum(ir, q, IROP_NONE);
      changes++;
      continue;
    }

    /* Cache this op's result. */
    if (cache_count < LACSE_MAX)
    {
      struct LACSEEntry *e = &cache[cache_count++];
      e->op = q->op;
      e->s1_tag = s1_tag;
      e->s1_lval = (uint8_t)src1.is_lval;
      e->s1_vr = s1_vr;
      e->s1_imm = s1_imm;
      e->s2_tag = s2_tag;
      e->s2_lval = (uint8_t)src2.is_lval;
      e->s2_vr = s2_vr;
      e->s2_imm = s2_imm;
      e->s3_tag = s3_tag;
      e->s3_lval = is_mla ? (uint8_t)accum.is_lval : 0;
      e->s3_vr = s3_vr;
      e->s3_imm = s3_imm;
      e->dest_vr = dest_vr;
    }
  }

#undef LACSE_MAX
#undef EXTRACT_KEY
#undef IS_CSE_PURE
  return changes;
}

/* ============================================================================
 * Single-BB VAR → TMP Promotion  (tcc_ir_opt_var_to_tmp)
 * ============================================================================
 *
 * Converts a local VAR that is defined once and only read back via lval
 * ASSIGNs into a TEMP, eliminating the redundant memory slot traffic.
 *
 * Before:
 *   V0 <-- X***DEREF*** [LOAD]        # write into V0's stack slot
 *   T1 <-- V0 [ASSIGN, lval src]      # reload V0's slot into T1
 *   T2 <-- T1 ADD #c
 *
 * After:
 *   T_new <-- X***DEREF*** [LOAD]     # load directly into a TEMP
 *   T1 <-- T_new [ASSIGN]             # pure copy (eaten by copy_prop)
 *   T2 <-- T1 ADD #c
 *
 * After subsequent copy_prop + DCE the chain collapses to:
 *   T2 <-- X***DEREF*** ADD #c   (or similar, depending on backend fusion)
 *
 * Preconditions per candidate V:
 *   - not address-taken, not is_complex
 *   - exactly one def in the function (checked globally first)
 *   - every use is src1 of an ASSIGN with is_lval=1 and matching btype
 *   - def and all uses live in the same straight-line segment (no BB
 *     boundary, no call, no redefinition between def and last use)
 * ============================================================================ */

int tcc_ir_opt_bool_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  IROptHashTable ht;
  ir_opt_hash_init(&ht, 64, n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      ir_opt_hash_clear(&ht);
      continue;
    }

    if (q->op != TCCIR_OP_BOOL_AND && q->op != TCCIR_OP_BOOL_OR)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int left_vr = src1.vr, right_vr = src2.vr;
    if (left_vr > right_vr)
    {
      int tmp = left_vr;
      left_vr = right_vr;
      right_vr = tmp;
    }

    uint32_t h = bool_cse_hash(q->op, left_vr, right_vr);
    BoolCSEKey key = {q->op, left_vr, right_vr};
    IROptHashEntry *existing = ir_opt_hash_lookup(&ht, h, bool_cse_eq, &key);
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (existing)
    {
      IROperand new_src = dest;
      new_src.vr = existing->result_vr;
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src);
      tcc_ir_set_src2(ir, i, IROP_NONE);
      changes++;
    }
    else
    {
      IROptHashEntry *e = ir_opt_hash_insert(&ht, h);
      if (e)
      {
        e->instruction_idx = i;
        e->result_vr = dest_vr;
        e->extra[0] = q->op;
        e->extra[1] = left_vr;
        e->extra[2] = right_vr;
      }
    }
  }

  ir_opt_hash_free(&ht);
  return changes;
}

int tcc_ir_opt_copy_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_copy_prop(ctx->ir); }

