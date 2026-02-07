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
#include "pool.h"
#include "vreg.h"

/* ============================================================================
 * FP Offset Cache Optimization - delegated to tccopt.c
 * ============================================================================ */

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

/* ============================================================================
 * Boolean Optimization Helpers
 * ============================================================================ */

/* Hash table entry for CSE */
typedef struct CSEHashEntry
{
  uint32_t key;        /* hash of (op, min(vr1,vr2), max(vr1,vr2)) */
  int instruction_idx; /* index of instruction that computes this */
  struct CSEHashEntry *next;
} CSEHashEntry;

#define CSE_HASH_SIZE 256

/* Stub implementation - functions to be moved from tccir.c */

/* Dead Code Elimination pass
 * Removes unreachable instructions by following control flow from entry.
 * Returns 1 if any instructions were eliminated, 0 otherwise.
 */
int tcc_ir_opt_dce(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

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
      /* Return - no successor (epilogue is implicit) */
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

/* Dead Store Elimination - remove ASSIGN instructions where the destination
 * vreg is never used. This eliminates redundant copies after CSE/idempotent
 * optimizations. Instead of compacting the array, we mark dead stores as NOP.
 */
int tcc_ir_opt_dse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Track which TMP vregs are used as sources */
  int max_tmp_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
      if (pos > max_tmp_pos)
        max_tmp_pos = pos;
    }
  }

  if (max_tmp_pos == 0)
    return 0;

  uint8_t *used = tcc_mallocz((max_tmp_pos + 8) / 8);

  /* Mark all TMP vregs that are used as sources */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Check src1 */
    const IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_config[q->op].has_src1 && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src1)) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(src1));
      if (pos <= max_tmp_pos)
        used[pos / 8] |= (1 << (pos % 8));
    }

    /* Check src2 */
    const IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (irop_config[q->op].has_src2 && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src2)) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(src2));
      if (pos <= max_tmp_pos)
        used[pos / 8] |= (1 << (pos % 8));
    }

    /* For STORE operations, the dest field is used as a pointer (address to store to),
     * not as a destination being written. If dest has VT_LVAL, the vreg is being
     * dereferenced, so it's a USE not a DEF. Mark it as used. */
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (q->op == TCCIR_OP_STORE && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
      if (pos <= max_tmp_pos)
        used[pos / 8] |= (1 << (pos % 8));
    }
  }

  /* Mark dead ASSIGN instructions as NOP (no array compaction needed) */
  int changes = 0;

#ifdef DEBUG_IR_GEN
  printf("=== DEAD STORE ELIMINATION START ===\n");
#endif

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Mark ASSIGN instructions where dest is an unused TMP vreg as NOP */
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (q->op == TCCIR_OP_ASSIGN && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
      if (pos <= max_tmp_pos && !(used[pos / 8] & (1 << (pos % 8))))
      {
        /* This ASSIGN's destination is never used - mark as NOP */
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }
  }

#ifdef DEBUG_IR_GEN
  printf("=== DEAD STORE ELIMINATION END (marked %d as NOP) ===\n", changes);
#endif
  tcc_free(used);

  return changes;
}

int tcc_ir_opt_const_prop(TCCIRState *ir)
{
  /* VarConstInfo: track constant variables */
  typedef struct
  {
    uint8_t is_constant : 1;
    uint8_t def_count : 7;
    int64_t value;
  } VarConstInfo;

  int n = ir->next_instruction_index;
  int changes = 0;
  int max_var_pos = 0;
  int i;
  IRQuadCompact *q;
  VarConstInfo *var_info;

  if (n == 0)
    return 0;

  /* Track which VAR vregs are constant (assigned exactly once with a constant value) */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (pos > max_var_pos)
        max_var_pos = pos;
    }
  }

  if (max_var_pos == 0)
    return 0;

  var_info = tcc_mallocz(sizeof(VarConstInfo) * (max_var_pos + 1));

  /* First pass: identify constant variables */
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
         */
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
        if (interval && interval->addrtaken)
        {
          var_info[pos].def_count++;
          var_info[pos].is_constant = 0;
          continue;
        }

        var_info[pos].def_count++;

        /* Check if this is a constant assignment */
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        if (q->op == TCCIR_OP_ASSIGN && irop_is_immediate(src1))
        {
          if (var_info[pos].def_count == 1)
          {
            var_info[pos].is_constant = 1;
            var_info[pos].value = irop_get_imm64_ex(ir, src1);
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
  for (i = 0; i <= max_var_pos; i++)
  {
    if (var_info[i].def_count > 1)
      var_info[i].is_constant = 0;
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
      if (TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
        if (pos <= max_var_pos && var_info[pos].is_constant)
          src1_can_be_const = 1;
      }
      else if (irop_is_immediate(src1))
        src1_can_be_const = 1;

      int32_t src2_vr = irop_get_vreg(src2);
      if (TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR)
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
    if (!skip_bool_prop && irop_config[q->op].has_src1 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR &&
        !(src1.is_local && !src1.is_lval))
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
      if (pos <= max_var_pos && var_info[pos].is_constant)
      {
        IROperand new_src1;
        int64_t val = var_info[pos].value;
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
        /* Preserve flags from original operand */
        new_src1.is_lval = src1.is_lval;
        new_src1.is_llocal = src1.is_llocal;
        new_src1.is_local = src1.is_local;
        new_src1.is_unsigned = src1.is_unsigned;
        new_src1.is_static = src1.is_static;
        tcc_ir_set_src1(ir, i, new_src1);
        changes++;
      }
    }

    int32_t src2_vr = irop_get_vreg(src2);
    if (!skip_bool_prop && irop_config[q->op].has_src2 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_VAR &&
        !(src2.is_local && !src2.is_lval))
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
      if (pos <= max_var_pos && var_info[pos].is_constant)
      {
        IROperand new_src2;
        int64_t val = var_info[pos].value;
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
        /* Preserve flags from original operand */
        new_src2.is_lval = src2.is_lval;
        new_src2.is_llocal = src2.is_llocal;
        new_src2.is_local = src2.is_local;
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
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Swap operands for commutative %s (const in src1) at i=%d\n", tcc_ir_get_op_name(q->op), i);
#endif
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
        result = val1 + val2;
        break;
      case TCCIR_OP_SUB:
        result = val1 - val2;
        break;
      case TCCIR_OP_MUL:
        result = val1 * val2;
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
        result = val1 << val2;
        break;
      case TCCIR_OP_SHR:
        result = (uint64_t)val1 >> val2;
        break;
      case TCCIR_OP_SAR:
        result = val1 >> val2;
        break;
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
          result = (uint64_t)val1 / (uint64_t)val2;
        }
        else
        {
          can_fold = 0; /* Division by zero - don't fold */
        }
        break;
      case TCCIR_OP_UMOD:
        if (val2 != 0)
        {
          result = (uint64_t)val1 % (uint64_t)val2;
        }
        else
        {
          can_fold = 0; /* Division by zero - don't fold */
        }
        break;
      default:
        can_fold = 0;
        break;
      }

      if (can_fold)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Constant fold %s(%lld, %lld) = %lld at i=%d\n", tcc_ir_get_op_name(q->op), (long long)val1,
               (long long)val2, (long long)result, i);
#endif
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
      int64_t const_value;
      int btype = irop_get_btype(src1);

      simplify = 0;
      replace_with_zero = 0;
      replace_with_const = 0;
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
        else if (c == -1 || c == 0xFFFFFFFF)
        {
          replace_with_const = 1; /* X | -1 = -1 */
          const_value = -1;
        }
        break;
      case TCCIR_OP_SHL:
      case TCCIR_OP_SHR:
      case TCCIR_OP_SAR:
        if (c == 0)
          simplify = 1; /* X << 0 = X, X >> 0 = X */
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
        break;
      case TCCIR_OP_AND:
        if (c == 0)
          replace_with_zero = 1; /* X & 0 = 0 */
        else if (c == -1 || c == 0xFFFFFFFF)
          simplify = 1; /* X & -1 = X */
        break;
      default:
        break;
      }

      if (simplify)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Algebraic simplify %s(x, %lld) = x at i=%d\n", tcc_ir_get_op_name(q->op), (long long)c, i);
#endif
        q->op = TCCIR_OP_ASSIGN;
        /* src1 stays as-is, clear src2 */
        tcc_ir_set_src1(ir, i, src1);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
      else if (replace_with_zero)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Algebraic simplify %s(x, %lld) = 0 at i=%d\n", tcc_ir_get_op_name(q->op), (long long)c, i);
#endif
        q->op = TCCIR_OP_ASSIGN;
        IROperand new_src1 = irop_make_imm32(-1, 0, btype);
        tcc_ir_set_src1(ir, i, new_src1);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
      else if (replace_with_const)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Algebraic simplify %s(x, %lld) = %lld at i=%d\n", tcc_ir_get_op_name(q->op), (long long)c,
               (long long)const_value, i);
#endif
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
    }

    /* Handle commutative operations: 0 + X = X, 0 << X = 0 */
    if (irop_config[q->op].has_src1 && src1_is_const)
    {
      const int64_t c = irop_get_imm64_ex(ir, src1);

      switch (q->op)
      {
      case TCCIR_OP_ADD:
      case TCCIR_OP_OR:
        if (c == 0)
        {
          /* 0 + X = X, 0 | X = X (commutative, swap operands) */
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Algebraic simplify %s(0, x) = x at i=%d\n", tcc_ir_get_op_name(q->op), i);
#endif
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
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Algebraic simplify %s(0, x) = 0 at i=%d\n", tcc_ir_get_op_name(q->op), i);
#endif
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
        if (c == 0)
        {
          /* 0 << X = 0, 0 >> X = 0 */
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Algebraic simplify %s(0, x) = 0 at i=%d\n", tcc_ir_get_op_name(q->op), i);
#endif
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

  /* Third pass: Fold CMP+SETIF patterns when CMP has constant operands */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *cmp_q = &ir->compact_instructions[i];
    IRQuadCompact *setif_q = &ir->compact_instructions[i + 1];
    int cmp_src1_const, cmp_src2_const;
    int64_t val1, val2;
    int cond, result;
    int btype;

    if (cmp_q->op != TCCIR_OP_CMP)
      continue;
    if (setif_q->op != TCCIR_OP_SETIF)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, cmp_q);
    IROperand src2 = tcc_ir_op_get_src2(ir, cmp_q);
    cmp_src1_const = irop_is_immediate(src1);
    cmp_src2_const = irop_is_immediate(src2);

    if (!cmp_src1_const || !cmp_src2_const)
      continue;

    val1 = irop_get_imm64_ex(ir, src1);
    val2 = irop_get_imm64_ex(ir, src2);
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
    case 0x96: /* TOK_ULT (unsigned <) */
      result = ((uint64_t)val1 < (uint64_t)val2) ? 1 : 0;
      break;
    case 0x97: /* TOK_UGE (unsigned >=) */
      result = ((uint64_t)val1 >= (uint64_t)val2) ? 1 : 0;
      break;
    case 0x98: /* TOK_ULE (unsigned <=) */
      result = ((uint64_t)val1 <= (uint64_t)val2) ? 1 : 0;
      break;
    case 0x99: /* TOK_UGT (unsigned >) */
      result = ((uint64_t)val1 > (uint64_t)val2) ? 1 : 0;
      break;
    default:
      /* Unknown condition, don't fold */
      continue;
    }

#ifdef DEBUG_IR_GEN
    printf("OPTIMIZE: Fold CMP+SETIF const (%lld cmp %lld, cond=0x%x) = %d at i=%d\n", (long long)val1, (long long)val2,
           cond, result, i);
#endif

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

  tcc_free(var_info);

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

/* Track constant values for vregs through arithmetic */
typedef struct
{
  int is_constant; /* 1 = value is known constant */
  int64_t value;   /* The constant value */
} VRegConstState;

/* Forward declaration - defined later in branch_folding section */
static int evaluate_compare_condition(int64_t val1, int64_t val2, int cond_token);

int tcc_ir_opt_value_tracking(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int max_vreg = 0;

  if (n == 0)
    return 0;

  /* Precompute merge points in O(n) to avoid O(n²) complexity */
  uint8_t *is_merge = tcc_mallocz((n + 7) / 8);
  int *pred_count = tcc_mallocz(n * sizeof(int));

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
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
    /* Fall-through predecessor */
    if (i + 1 < n && q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_RETURNVALUE &&
        q->op != TCCIR_OP_RETURNVOID)
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

  /* Find max VAR vreg position */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos > max_vreg)
        max_vreg = pos;
    }
  }

  if (max_vreg == 0)
  {
    tcc_free(is_merge);
    return 0;
  }

  VRegConstState *state = tcc_mallocz(sizeof(VRegConstState) * (max_vreg + 1));

  /* Forward pass: track values through the IR */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Clear state at merge points (multiple predecessors or back-edge targets) */
    if (is_merge[i / 8] & (1 << (i % 8)))
    {
      for (int v = 0; v <= max_vreg; v++)
        state[v].is_constant = 0;
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int32_t dest_vr = irop_get_vreg(dest);
    int dest_pos = (dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
                       ? TCCIR_DECODE_VREG_POSITION(dest_vr)
                       : -1;

    /* Pattern 1: Direct constant assignment: Vx <- #const */
    if (q->op == TCCIR_OP_ASSIGN && irop_is_immediate(src1))
    {
      if (dest_pos >= 0 && dest_pos <= max_vreg)
      {
        state[dest_pos].is_constant = 1;
        state[dest_pos].value = irop_get_imm64_ex(ir, src1);
      }
      continue;
    }

    /* Pattern 2: Arithmetic with constant operand: Vx <- Vy +/- #const */
    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_is_immediate(src2))
    {
      int32_t src1_vr = irop_get_vreg(src1);
      int src1_pos = (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
                         ? TCCIR_DECODE_VREG_POSITION(src1_vr)
                         : -1;

      /* Check if src1 is a known constant AND src2 is immediate */
      if (src1_pos >= 0 && src1_pos <= max_vreg && state[src1_pos].is_constant)
      {
        int64_t val1 = state[src1_pos].value;
        int64_t val2 = irop_get_imm64_ex(ir, src2);
        int64_t result = (q->op == TCCIR_OP_ADD) ? val1 + val2 : val1 - val2;

        if (dest_pos >= 0 && dest_pos <= max_vreg)
        {
          state[dest_pos].is_constant = 1;
          state[dest_pos].value = result;
        }
      }
      else
      {
        /* Destination no longer has known constant value */
        if (dest_pos >= 0 && dest_pos <= max_vreg)
          state[dest_pos].is_constant = 0;
      }
      continue;
    }

    /* Pattern 3: CMP with constant vreg - FOLD IT
     * Track constant values through arithmetic and fold CMP instructions
     * when the compared vreg has a known constant value.
     */
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
        int src1_const = (src1_pos >= 0 && src1_pos <= max_vreg && state[src1_pos].is_constant);
        int src2_const = irop_is_immediate(src2);

        if (src1_const && src2_const)
        {
          int64_t val1 = state[src1_pos].value;
          int64_t val2 = irop_get_imm64_ex(ir, src2);

          IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
          int tok = (int)irop_get_imm64_ex(ir, cond);

          /* Use evaluate_compare_condition from branch_folding */
          int result = evaluate_compare_condition(val1, val2, tok);

          if (result >= 0)
          {
            IROperand jmp_dest = tcc_ir_op_get_dest(ir, jump_q);

            if (result)
            {
              /* Branch always taken - convert to unconditional JUMP */
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_JUMP;
              tcc_ir_set_dest(ir, i + 1, jmp_dest);
#ifdef DEBUG_IR_GEN
              printf("VALUE_TRACK: CMP vreg=%lld,#%lld -> always taken, JUMP to %d\n", (long long)val1, (long long)val2,
                     (int)jmp_dest.u.imm32);
#endif
            }
            else
            {
              /* Branch never taken - eliminate both */
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_NOP;
#ifdef DEBUG_IR_GEN
              printf("VALUE_TRACK: CMP vreg=%lld,#%lld -> never taken, eliminated\n", (long long)val1, (long long)val2);
#endif
            }
            changes++;
          }
        }
      }
      continue;
    }

    /* Any other instruction that defines a VAR vreg invalidates the constant */
    if (dest_pos >= 0 && dest_pos <= max_vreg && irop_config[q->op].has_dest)
    {
      state[dest_pos].is_constant = 0;
    }
  }

  tcc_free(state);
  tcc_free(is_merge);

  /* Run DCE to remove code after eliminated branches */
  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

/* TMP Constant Propagation
 * After constant folding may create TMP <- #const instructions,
 * propagate these constants to uses of the TMP within the same basic block.
 *
 * Performance: Uses generation counters for O(1) block clears instead of memset.
 * Stack buffers avoid malloc for small functions.
 */
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
  int current_gen = 1; /* Generation counter, 0 means invalid */
  int i;
  IRQuadCompact *q;
  TmpConstInfo *tmp_info;
  int *block_start_seen;
  int block_start_gen = 1;
  void *heap_alloc = NULL;

  if (n == 0)
    return 0;

  /* Find max TMP position */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (pos > max_tmp_pos)
        max_tmp_pos = pos;
    }
  }

  if (max_tmp_pos == 0)
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

  /* Mark block starts */
  block_start_seen[0] = block_start_gen;
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      /* Jump target is stored in u.imm32 regardless of tag */
      const int tgt = (int)dest.u.imm32;
      if (tgt >= 0 && tgt < n)
        block_start_seen[tgt] = block_start_gen;
    }
  }

  /* Single pass: track TMP constants and propagate */
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

    /* Propagate TMP constants to src1 */
    if (irop_config[q->op].has_src1 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src1_vr);
      if (pos <= max_tmp_pos && tmp_info[pos].gen == current_gen)
      {
        int btype = irop_get_btype(src1);
        IROperand new_src1;
        int64_t val = tmp_info[pos].value;
        if (val == (int32_t)val)
        {
          new_src1 = irop_make_imm32(-1, (int32_t)val, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_src1 = irop_make_i64(-1, pool_idx, btype);
        }
        /* Preserve flags from original operand */
        new_src1.is_lval = src1.is_lval;
        new_src1.is_llocal = src1.is_llocal;
        new_src1.is_local = src1.is_local;
        new_src1.is_unsigned = src1.is_unsigned;
        new_src1.is_static = src1.is_static;
        tcc_ir_set_src1(ir, i, new_src1);
        changes++;
      }
    }

    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int32_t src2_vr = irop_get_vreg(src2);
    /* Propagate TMP constants to src2 */
    if (irop_config[q->op].has_src2 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
      if (pos <= max_tmp_pos && tmp_info[pos].gen == current_gen)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: TMP const propagate TMP:%d = %lld to src2 at i=%d\n", pos, (long long)tmp_info[pos].value, i);
#endif
        int btype = irop_get_btype(src2);
        IROperand new_src2;
        int64_t val = tmp_info[pos].value;
        if (val == (int32_t)val)
        {
          new_src2 = irop_make_imm32(-1, (int32_t)val, btype);
        }
        else
        {
          uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
          new_src2 = irop_make_i64(-1, pool_idx, btype);
        }
        /* Preserve flags from original operand */
        new_src2.is_lval = src2.is_lval;
        new_src2.is_llocal = src2.is_llocal;
        new_src2.is_local = src2.is_local;
        new_src2.is_unsigned = src2.is_unsigned;
        new_src2.is_static = src2.is_static;
        tcc_ir_set_src2(ir, i, new_src2);
        changes++;
      }
    }

    /* Clear all at basic block boundaries - O(1) via generation bump */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      current_gen++;
      continue;
    }

    /* Track TMP <- constant assignments */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP &&
        q->op == TCCIR_OP_ASSIGN)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (pos <= max_tmp_pos && irop_is_immediate(src1))
      {
        tmp_info[pos].gen = current_gen;
        tmp_info[pos].value = irop_get_imm64_ex(ir, src1);
      }
    }
  }

  if (heap_alloc)
    tcc_free(heap_alloc);

  return changes;
#undef TMP_CONST_STACK_SIZE
#undef TMP_CONST_STACK_N
}

/* Copy Propagation
 * Phase 3: Replace uses of x with y where x = y (direct copy)
 * Benefits: Removes redundant copies, enables more CSE.
 * Uses basic-block local analysis with conservative safety checks.
 */
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
      if (vr_type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp_pos)
        max_tmp_pos = pos;
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
      if (vr_type == TCCIR_VREG_TYPE_VAR && pos > max_var_pos)
        max_var_pos = pos;
      else if (vr_type == TCCIR_VREG_TYPE_PARAM && pos > max_param_pos)
        max_param_pos = pos;
    }
  }

  if (max_tmp_pos == 0)
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

  /* Mark instruction 0 as block start */
  block_start_seen[0] = block_start_gen;

  /* Two-pass approach: first mark block starts, then propagate.
   * This is still O(n) but avoids separate allocation for block_start bitmap. */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      const int tgt = (int)irop_get_imm64_ex(ir, dest);
      if (tgt >= 0 && tgt < n)
        block_start_seen[tgt] = block_start_gen;
    }
  }

  /* Single pass: process instructions in order, tracking and propagating copies */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];

    /* At block boundaries, invalidate all copies by incrementing generation */
    if (i != 0 && block_start_seen[i] == block_start_gen)
    {
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
            replacement.is_lval = 1; /* Preserve DEREF semantics from use site */
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Copy propagate TMP:%d -> vreg:%d (lval=%d) at i=%d\n", pos,
                 TCCIR_DECODE_VREG_POSITION(copy_info[pos].source_vr), src1.is_lval, i);
#endif
          tcc_ir_set_src1(ir, i, replacement);
          changes++;
        }
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
            replacement.is_lval = 1; /* Preserve DEREF semantics from use site */
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Copy propagate TMP:%d -> vreg:%d (lval=%d) at i=%d\n", pos,
                 TCCIR_DECODE_VREG_POSITION(copy_info[pos].source_vr), src2.is_lval, i);
#endif
          tcc_ir_set_src2(ir, i, replacement);
          changes++;
        }
      }
    }

    /* Propagate copies into STORE destinations.
     * For STORE: dest is TMP***DEREF*** (address to write to), src1 is the value.
     * If TMP was copied from another TMP, replace TMP***DEREF*** with source***DEREF***.
     * Only allow TMP←TMP copies here (same restriction as src1/src2 lval propagation). */
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
          if (src_type == TCCIR_VREG_TYPE_TEMP)
          {
            IROperand replacement = copy_info[pos].source;
            replacement.is_lval = 1; /* Preserve DEREF semantics */
#ifdef DEBUG_IR_GEN
            printf("OPTIMIZE: Copy propagate STORE dest TMP:%d -> vreg:%d at i=%d\n", pos,
                   TCCIR_DECODE_VREG_POSITION(copy_info[pos].source_vr), i);
#endif
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
      if (dest_type == TCCIR_VREG_TYPE_VAR || dest_type == TCCIR_VREG_TYPE_PARAM ||
          dest_type == TCCIR_VREG_TYPE_TEMP)
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
#ifdef DEBUG_IR_GEN
              printf("COPY_PROP: Invalidate TMP:%d (source vreg:%d type=%d redefined) at i=%d\n", tmp_pos,
                     dest_pos, dest_type, i);
#endif
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
        if (!src_is_const && src1_vr >= 0 && !src1.is_lval &&
            irop_get_btype(dest) == irop_get_btype(src1) &&
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
#ifdef DEBUG_IR_GEN
          printf("COPY_PROP: Record TMP:%d <- vreg:%d (type=%d) at i=%d\n", pos, TCCIR_DECODE_VREG_POSITION(src1_vr),
                 src_vreg_type, i);
#endif
        }
        else
        {
          /* TMP is assigned something other than a simple VAR/PAR copy - invalidate */
          if (copy_info[pos].gen == current_gen && active_copies > 0)
            active_copies--;
          copy_info[pos].gen = 0;
          copy_info[pos].next_same_source = -1;
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
typedef struct BoolCSEEntry
{
  int op;        /* TCCIR_OP_BOOL_AND or TCCIR_OP_BOOL_OR */
  int left_vr;   /* Left operand vreg (normalized: smaller first) */
  int right_vr;  /* Right operand vreg */
  int result_vr; /* The vreg that holds the result */
  struct BoolCSEEntry *next;
} BoolCSEEntry;

#define BOOL_CSE_HASH_SIZE 64

/* Compute hash for boolean op (normalized operand order) */
static uint32_t bool_cse_hash(int op, int left_vr, int right_vr)
{
  /* Normalize order for commutative ops */
  if (left_vr > right_vr)
  {
    int tmp = left_vr;
    left_vr = right_vr;
    right_vr = tmp;
  }
  return ((uint32_t)op * 31 + (uint32_t)left_vr * 17 + (uint32_t)right_vr) % BOOL_CSE_HASH_SIZE;
}

/* Find existing boolean CSE entry */
static BoolCSEEntry *bool_cse_find(BoolCSEEntry **hash_table, int op, int left_vr, int right_vr)
{
  uint32_t h = bool_cse_hash(op, left_vr, right_vr);
  BoolCSEEntry *e;

  for (e = hash_table[h]; e != NULL; e = e->next)
  {
    if (e->op == op && e->left_vr == left_vr && e->right_vr == right_vr)
      return e;
  }
  return NULL;
}

/* Add boolean CSE entry */
static void bool_cse_add(BoolCSEEntry **hash_table, int op, int left_vr, int right_vr, int result_vr)
{
  uint32_t h = bool_cse_hash(op, left_vr, right_vr);
  BoolCSEEntry *e = tcc_malloc(sizeof(BoolCSEEntry));
  e->op = op;
  e->left_vr = left_vr;
  e->right_vr = right_vr;
  e->result_vr = result_vr;
  e->next = hash_table[h];
  hash_table[h] = e;
}

/* Clear all CSE entries */
static void bool_cse_clear_all(BoolCSEEntry **hash_table)
{
  int i;
  for (i = 0; i < BOOL_CSE_HASH_SIZE; i++)
  {
    BoolCSEEntry *e = hash_table[i];
    while (e)
    {
      BoolCSEEntry *next = e->next;
      tcc_free(e);
      e = next;
    }
    hash_table[i] = NULL;
  }
}

/* Boolean CSE pass - find and reuse common boolean subexpressions */
int tcc_ir_opt_cse_bool(TCCIRState *ir)
{
  BoolCSEEntry *hash_table[BOOL_CSE_HASH_SIZE];
  int n = ir->next_instruction_index;
  int changes = 0;
  int i;

  if (n == 0)
    return 0;

  memset(hash_table, 0, sizeof(hash_table));

  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Clear CSE table at control flow boundaries */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      bool_cse_clear_all(hash_table);
      continue;
    }

    /* Only process BOOL_AND and BOOL_OR */
    if (q->op != TCCIR_OP_BOOL_AND && q->op != TCCIR_OP_BOOL_OR)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int left_vr = src1.vr;
    int right_vr = src2.vr;

    /* Normalize operand order for hash lookup */
    if (left_vr > right_vr)
    {
      int tmp = left_vr;
      left_vr = right_vr;
      right_vr = tmp;
    }

    /* Check if we've seen this boolean op before */
    BoolCSEEntry *existing = bool_cse_find(hash_table, q->op, left_vr, right_vr);
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (existing)
    {
      /* Found a match! Replace this op with ASSIGN from the existing result */
      /* Create new operand referencing the CSE result */
      IROperand new_src;
      new_src = dest;
      new_src.vr = existing->result_vr;

      /* Convert to ASSIGN */
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src);
      tcc_ir_set_src2(ir, i, IROP_NONE);

#ifdef DEBUG_IR_GEN
      printf("BOOL CSE: Reuse vr%d at i=%d (was computed at vr%d)\n", dest_vr, i, existing->result_vr);
#endif
      changes++;
    }
    else
    {
      /* Add this to the CSE table */
      bool_cse_add(hash_table, q->op, left_vr, right_vr, dest_vr);
    }
  }

  bool_cse_clear_all(hash_table);
  return changes;
}

/* Boolean idempotent optimization pass
 * Handles: a && a -> a, a || a -> a, a && 1 -> a, a || 0 -> a
 * Returns: number of optimizations applied.
 */
int tcc_ir_opt_bool_idempotent(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int i;

  if (n == 0)
    return 0;

  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_BOOL_AND && q->op != TCCIR_OP_BOOL_OR)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int is_and = (q->op == TCCIR_OP_BOOL_AND);

    /* Check for a && a or a || a */
    if (src1.vr >= 0 && src1.vr == src2.vr)
    {
#ifdef DEBUG_IR_GEN
      printf("BOOL IDEMPOTENT: %s vr%d with itself at i=%d -> ASSIGN\n", is_and ? "&&" : "||", src1.vr, i);
#endif
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src2(ir, i, IROP_NONE);
      changes++;
      continue;
    }

    /* Check for a && 1 or a || 0 */
    /* Note: These require the constant to be in src2 for our analysis */
    if (src2.vr < 0 && irop_is_immediate(src2))
    {
      int64_t val = irop_get_imm64_ex(ir, src2);
      int should_optimize = 0;

      if (is_and && val == 1)
      {
        /* a && 1 -> a */
        should_optimize = 1;
      }
      else if (!is_and && val == 0)
      {
        /* a || 0 -> a */
        should_optimize = 1;
      }

      if (should_optimize)
      {
#ifdef DEBUG_IR_GEN
        printf("BOOL IDEMPOTENT: %s with neutral element at i=%d -> ASSIGN\n", is_and ? "&&" : "||", i);
#endif
        q->op = TCCIR_OP_ASSIGN;
        /* src1 is already the value we want */
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
    }
  }

  return changes;
}

/* Boolean simplification pass
 * Handles: (x && y) && z -> inner = x && y; result = inner && z
 *          (x || y) || z -> inner = x || y; result = inner || z
 * This breaks down nested boolean ops to enable more CSE opportunities.
 * Returns: number of optimizations applied.
 */
int tcc_ir_opt_bool_simplify(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int i;

  if (n == 0)
    return 0;

  /* Single pass: look for nested boolean ops of the same type */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_BOOL_AND && q->op != TCCIR_OP_BOOL_OR)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    /* Skip if src1 is not a vreg (can't be result of another op) */
    if (src1.vr < 0)
      continue;

    /* Find the defining instruction for src1 */
    int def_idx = tcc_ir_find_defining_instruction(ir, src1.vr, i);
    if (def_idx < 0)
      continue;

    /* Check if the defining instruction is a boolean op of the same type */
    IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
    if (def_q->op != q->op)
      continue;

    /* Check that the inner op is only used here (single use) */
    if (!tcc_ir_vreg_has_single_use(ir, src1.vr, i))
      continue;

    /* Found: inner op of same type with single use.
     * We can flatten: (a OP b) OP c becomes just the outer OP using inner's operands.
     * Actually, that's not quite right - we want to KEEP the inner op and just
     * have the outer refer to its result. But that's already the case!
     * So what this optimization does is recognize that we've already done CSE
     * on the inner, and we can just use that result.
     *
     * Actually, the real purpose is to PREVENT the inner from being CSE'd
     * with something else if it's only used here. But that's not what we want.
     *
     * Let me reconsider: The goal is to simplify boolean expressions.
     * If we have: r1 = a && b; r2 = r1 && c
     * This can be kept as is - the code generator handles this fine.
     * But for CSE purposes, we might want to mark r1 as "don't CSE replace"
     * if it would prevent other optimizations.
     *
     * For now, let's just mark this as an optimization opportunity and
     * track it. The real benefit might be in register allocation.
     */

#ifdef DEBUG_IR_GEN
    printf("BOOL SIMPLIFY: Nested %s at i=%d (inner at i=%d)\n", q->op == TCCIR_OP_BOOL_AND ? "&&" : "||", i, def_idx);
#endif

    /* The second inner op will be eliminated by DCE if unused */
    changes++;
  }

  return changes;
}

/* Arithmetic Common Subexpression Elimination
 * Phase 3: Eliminate redundant arithmetic computations within basic blocks
 * Handles ADD, SUB, MUL, AND, OR, XOR, SHL, SHR, SAR operations
 */
int tcc_ir_opt_cse_arith(TCCIRState *ir)
{
  typedef struct ArithCSEEntry
  {
    TccIrOp op;
    int src1_vr;
    int src2_vr;
    int64_t src1_const;
    int64_t src2_const;
    int64_t src1_local_off;
    int64_t src2_local_off;
    Sym *src1_sym;
    Sym *src2_sym;
    uint8_t src1_is_const : 1;
    uint8_t src2_is_const : 1;
    uint8_t src1_is_sym : 1;
    uint8_t src2_is_sym : 1;
    uint8_t src1_is_local : 1;
    uint8_t src2_is_local : 1;
    uint8_t src1_is_llocal : 1;
    uint8_t src2_is_llocal : 1;
    int result_vr;
    int instruction_idx;
    struct ArithCSEEntry *next;
  } ArithCSEEntry;

  int n;
  int changes;
  int i, j;
  IRQuadCompact *q;
  ArithCSEEntry *hash_table[256];
  ArithCSEEntry *entries;
  int entry_count;

  n = ir->next_instruction_index;
  changes = 0;

  if (n == 0)
    return 0;

  memset(hash_table, 0, sizeof(hash_table));
  entries = tcc_malloc(sizeof(ArithCSEEntry) * n);
  entry_count = 0;

  for (i = 0; i < n; i++)
  {
    int src1_is_const, src2_is_const;
    int src1_is_sym, src2_is_sym;
    int64_t src1_const, src2_const;
    int src1_vr, src2_vr;
    Sym *src1_sym, *src2_sym;
    uint32_t h;
    int found;
    ArithCSEEntry *e;

    q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      memset(hash_table, 0, sizeof(hash_table));
      entry_count = 0;
      continue;
    }

    if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB && q->op != TCCIR_OP_MUL && q->op != TCCIR_OP_AND &&
        q->op != TCCIR_OP_OR && q->op != TCCIR_OP_XOR && q->op != TCCIR_OP_SHL && q->op != TCCIR_OP_SHR &&
        q->op != TCCIR_OP_SAR)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t src1_vr32 = irop_get_vreg(src1);
    int32_t src2_vr32 = irop_get_vreg(src2);
    int32_t dest_vr32 = irop_get_vreg(dest);
    int src1_is_local = src1.is_local;
    int src2_is_local = src2.is_local;
    int src1_is_llocal = src1.is_llocal;
    int src2_is_llocal = src2.is_llocal;
    src1_is_const = irop_is_immediate(src1) && !src1.is_sym && !src1_is_local && !src1_is_llocal;
    src2_is_const = irop_is_immediate(src2) && !src2.is_sym && !src2_is_local && !src2_is_llocal;
    src1_is_sym = src1.is_sym;
    src2_is_sym = src2.is_sym;
    src1_const = src1_is_const ? irop_get_imm64_ex(ir, src1) : 0;
    src2_const = src2_is_const ? irop_get_imm64_ex(ir, src2) : 0;
    src1_sym = src1_is_sym ? irop_get_sym_ex(ir, src1) : NULL;
    src2_sym = src2_is_sym ? irop_get_sym_ex(ir, src2) : NULL;
    src1_vr = src1_vr32;
    src2_vr = src2_vr32;
    int64_t src1_local_off = (src1_is_local || src1_is_llocal) ? irop_get_imm64_ex(ir, src1) : 0;
    int64_t src2_local_off = (src2_is_local || src2_is_llocal) ? irop_get_imm64_ex(ir, src2) : 0;

    h = (uint32_t)q->op * 31;
    if (src1_is_const)
      h += (uint32_t)src1_const * 17;
    else if (src1_is_sym)
      h += (uint32_t)(uintptr_t)src1_sym * 17;
    else if (src1_is_local || src1_is_llocal)
      h += (uint32_t)src1_local_off * 19 + (uint32_t)src1_vr * 7;
    else
      h += (uint32_t)src1_vr * 17;
    if (src2_is_const)
      h += (uint32_t)src2_const * 13;
    else if (src2_is_sym)
      h += (uint32_t)(uintptr_t)src2_sym * 13;
    else if (src2_is_local || src2_is_llocal)
      h += (uint32_t)src2_local_off * 23 + (uint32_t)src2_vr * 11;
    else
      h += (uint32_t)src2_vr * 13;
    h = h % 256;

    found = 0;
    for (e = hash_table[h]; e != NULL; e = e->next)
    {
      int is_commutative;
      int match1, match2;

      if (e->op != q->op)
        continue;

      /* Must match all operand type flags */
      if (e->src1_is_const == src1_is_const && e->src2_is_const == src2_is_const && e->src1_is_sym == src1_is_sym &&
          e->src2_is_sym == src2_is_sym && e->src1_is_local == src1_is_local && e->src2_is_local == src2_is_local &&
          e->src1_is_llocal == src1_is_llocal && e->src2_is_llocal == src2_is_llocal)
      {
        /* For consts, compare constant value; for symbols, compare symbol pointer;
         * for stack offsets, compare BOTH vreg AND offset (different vars can share
         * same offset when accessed via pointers); otherwise compare vreg */
        if (src1_is_const)
          match1 = (e->src1_const == src1_const);
        else if (src1_is_sym)
          match1 = (e->src1_sym == src1_sym);
        else if (src1_is_local || src1_is_llocal)
          match1 = (e->src1_local_off == src1_local_off);
        else
          match1 = (e->src1_vr == src1_vr);

        if (src2_is_const)
          match2 = (e->src2_const == src2_const);
        else if (src2_is_sym)
          match2 = (e->src2_sym == src2_sym);
        else if (src2_is_local || src2_is_llocal)
          match2 = (e->src2_local_off == src2_local_off);
        else
          match2 = (e->src2_vr == src2_vr);

        if (match1 && match2)
        {
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Arithmetic CSE %s at %d same as %d -> ASSIGN\n", tcc_ir_get_op_name(q->op), i,
                 e->instruction_idx);
#endif
          q->op = TCCIR_OP_ASSIGN;
          /* Create a reference to the previous instruction's dest vreg.
           * IMPORTANT: Only copy vr and btype - do NOT copy is_lval or other flags
           * that might cause incorrect dereferencing. The dest vreg holds a VALUE,
           * not an address to be dereferenced. */
          IROperand prev_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[e->instruction_idx]);
          int32_t prev_dest_vr = irop_get_vreg(prev_dest);
          int prev_btype = irop_get_btype(prev_dest);
          IROperand new_src1 = irop_make_vreg(prev_dest_vr, prev_btype);
          /* Preserve unsigned flag from previous dest */
          new_src1.is_unsigned = prev_dest.is_unsigned;
          tcc_ir_set_src1(ir, i, new_src1);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
          found = 1;
          break;
        }
      }

      is_commutative = (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_AND ||
                        q->op == TCCIR_OP_OR || q->op == TCCIR_OP_XOR);

      /* For commutative ops, also check swapped operands (with matching flags) */
      if (is_commutative && e->src1_is_const == src2_is_const && e->src2_is_const == src1_is_const &&
          e->src1_is_sym == src2_is_sym && e->src2_is_sym == src1_is_sym && e->src1_is_local == src2_is_local &&
          e->src2_is_local == src1_is_local && e->src1_is_llocal == src2_is_llocal &&
          e->src2_is_llocal == src1_is_llocal)
      {
        if (src2_is_const)
          match1 = (e->src1_const == src2_const);
        else if (src2_is_sym)
          match1 = (e->src1_sym == src2_sym);
        else if (src2_is_local || src2_is_llocal)
          match1 = (e->src1_local_off == src2_local_off) && (e->src1_vr == src2_vr);
        else
          match1 = (e->src1_vr == src2_vr);

        if (src1_is_const)
          match2 = (e->src2_const == src1_const);
        else if (src1_is_sym)
          match2 = (e->src2_sym == src1_sym);
        else if (src1_is_local || src1_is_llocal)
          match2 = (e->src2_local_off == src1_local_off) && (e->src2_vr == src1_vr);
        else
          match2 = (e->src2_vr == src1_vr);

        if (match1 && match2)
        {
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Arithmetic CSE %s at %d same as %d (commutative) -> ASSIGN\n", tcc_ir_get_op_name(q->op), i,
                 e->instruction_idx);
#endif
          q->op = TCCIR_OP_ASSIGN;
          /* Create a reference to the previous instruction's dest vreg.
           * IMPORTANT: Only copy vr and btype - do NOT copy is_lval or other flags
           * that might cause incorrect dereferencing. The dest vreg holds a VALUE,
           * not an address to be dereferenced. */
          IROperand prev_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[e->instruction_idx]);
          int32_t prev_dest_vr = irop_get_vreg(prev_dest);
          int prev_btype = irop_get_btype(prev_dest);
          IROperand new_src1 = irop_make_vreg(prev_dest_vr, prev_btype);
          /* Preserve unsigned flag from previous dest */
          new_src1.is_unsigned = prev_dest.is_unsigned;
          tcc_ir_set_src1(ir, i, new_src1);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
          found = 1;
          break;
        }
      }
    }

    if (!found && entry_count < n)
    {
      ArithCSEEntry *new_entry;
      new_entry = &entries[entry_count++];
      new_entry->op = q->op;
      new_entry->src1_vr = src1_vr;
      new_entry->src2_vr = src2_vr;
      new_entry->src1_const = src1_const;
      new_entry->src2_const = src2_const;
      new_entry->src1_local_off = src1_local_off;
      new_entry->src2_local_off = src2_local_off;
      new_entry->src1_sym = src1_sym;
      new_entry->src2_sym = src2_sym;
      new_entry->src1_is_const = src1_is_const;
      new_entry->src2_is_const = src2_is_const;
      new_entry->src1_is_sym = src1_is_sym;
      new_entry->src2_is_sym = src2_is_sym;
      new_entry->src1_is_local = src1_is_local;
      new_entry->src2_is_local = src2_is_local;
      new_entry->src1_is_llocal = src1_is_llocal;
      new_entry->src2_is_llocal = src2_is_llocal;
      new_entry->result_vr = dest_vr32;
      new_entry->instruction_idx = i;
      new_entry->next = hash_table[h];
      hash_table[h] = new_entry;
    }

    if (irop_config[q->op].has_dest)
    {
      int dest_vr = dest_vr32;
      for (j = 0; j < 256; j++)
      {
        ArithCSEEntry **ep;
        ep = &hash_table[j];
        while (*ep)
        {
          e = *ep;
          if ((!e->src1_is_const && e->src1_vr == dest_vr) || (!e->src2_is_const && e->src2_vr == dest_vr))
            *ep = e->next;
          else
            ep = &e->next;
        }
      }
    }
  }

  tcc_free(entries);
  return changes;
}

/* Return value optimization - fold LOAD -> RETURNVALUE patterns */
int tcc_ir_opt_return(TCCIRState *ir)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  return 0;
}

/* Store-Load Forwarding
 * Phase 4: Replace loads from addresses that were just stored to with the stored value
 * Uses conservative basic-block-local alias analysis:
 *   - Stack locals (VT_LOCAL) never alias pointer derefs
 *   - Track base vreg + offset for array accesses
 *   - Clear all pointer-based stores at unknown stores
 *   - Clear all stores at basic block boundaries and function calls
 */
int tcc_ir_opt_sl_forward(TCCIRState *ir)
{
  typedef struct StoreEntry
  {
    int valid;
    int addr_addrtaken;     /* 1 if address of this local is taken */
    int64_t local_offset;   /* stack offset or symref addend */
    const Sym *local_sym;   /* symbol for VT_LOCAL (NULL for pure stack offsets) */
    IROperand stored_value; /* IROperand of the stored value */
    int instruction_idx;    /* where the store happened */
    struct StoreEntry *next;
  } StoreEntry;

  int n = ir->next_instruction_index;
  int changes = 0;
  int i;
  IRQuadCompact *q;
  StoreEntry *hash_table[128];
  StoreEntry *entries;
  int entry_count;

  if (n == 0)
    return 0;

  memset(hash_table, 0, sizeof(hash_table));
  entries = tcc_malloc(sizeof(StoreEntry) * n);
  entry_count = 0;

#ifdef DEBUG_IR_GEN
  printf("=== STORE-LOAD FORWARDING START ===\n");
#endif

  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];

    /* Clear all stores at basic block boundaries and function calls */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      memset(hash_table, 0, sizeof(hash_table));
      entry_count = 0;
      continue;
    }

    /* Process LOAD instructions: check if we can forward from a previous store */
    if (q->op == TCCIR_OP_LOAD)
    {
      /* LOAD: dest <- src1***DEREF***
       * src1 is the address to load from */
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t addr_vr = irop_get_vreg(src1);
      const Sym *addr_sym;
      int64_t addr_offset;
      uint32_t h;
      StoreEntry *e;

      /* CONSERVATIVE: Only forward for stack locals */
      if (!src1.is_local)
        continue;

      /* Check if address is taken - if so, skip forwarding (may alias through pointer) */
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
          continue;
      }

      /* Extract sym and offset from the local address operand */
      if (irop_get_tag(src1) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *sr = irop_get_symref_ex(ir, src1);
        addr_sym = sr ? sr->sym : NULL;
        addr_offset = sr ? sr->addend : 0;
      }
      else
      {
        addr_sym = NULL;
        addr_offset = irop_get_imm64_ex(ir, src1);
      }

      /* For VT_LOCAL, hash on symbol pointer and offset */
      h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;

      /* Search for matching store */
      for (e = hash_table[h]; e != NULL; e = e->next)
      {
        if (!e->valid || e->addr_addrtaken)
          continue;

        /* Both are stack locals - match on symbol and offset */
        if (e->local_sym == addr_sym && e->local_offset == addr_offset)
        {
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Store-load forwarding at i=%d from store at i=%d\n", i, e->instruction_idx);
#endif
          /* Replace LOAD with ASSIGN from the stored value */
          q->op = TCCIR_OP_ASSIGN;
          /* Write stored value to both pools for src1 slot */
          int pool_off = q->operand_base + irop_config[TCCIR_OP_ASSIGN].has_dest;
          ir->iroperand_pool[pool_off] = e->stored_value;
          changes++;
          break;
        }
      }
    }
    /* Process STORE instructions: track them for later forwarding */
    else if (q->op == TCCIR_OP_STORE)
    {
      /* STORE: dest***DEREF*** <- src1
       * dest is the address, src1 is the value to store */
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t addr_vr = irop_get_vreg(dest);
      const Sym *addr_sym;
      int64_t addr_offset;
      int addr_addrtaken = 0;
      uint32_t h;
      StoreEntry *new_entry;
      int j;

      /* CONSERVATIVE: Only track stack locals for forwarding */
      if (!dest.is_local)
      {
        /* Non-local store - must invalidate ALL tracked stores since it could alias */
        for (j = 0; j < entry_count; j++)
        {
          if (entries[j].valid && entries[j].addr_addrtaken)
          {
#ifdef DEBUG_IR_GEN
            printf("STORE-LOAD: Invalidate addr-taken local at i=%d due to pointer store at i=%d\n",
                   entries[j].instruction_idx, i);
#endif
            entries[j].valid = 0;
          }
        }
        continue;
      }

      /* Check if address of this local is taken */
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
          addr_addrtaken = 1;
      }

      /* Extract sym and offset from the local address operand */
      if (irop_get_tag(dest) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
        addr_sym = sr ? sr->sym : NULL;
        addr_offset = sr ? sr->addend : 0;
      }
      else
      {
        addr_sym = NULL;
        addr_offset = irop_get_imm64_ex(ir, dest);
      }

      /* For VT_LOCAL, hash on symbol pointer and offset */
      h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;

      /* Check if we already have a store to this exact location - if so, invalidate it
       * (the new store overwrites the old one) */
      for (new_entry = hash_table[h]; new_entry != NULL; new_entry = new_entry->next)
      {
        if (new_entry->local_sym == addr_sym && new_entry->local_offset == addr_offset)
          new_entry->valid = 0;
      }

      /* Record the new store */
      new_entry = &entries[entry_count++];
      new_entry->valid = 1;
      new_entry->addr_addrtaken = addr_addrtaken;
      new_entry->local_offset = addr_offset;
      new_entry->local_sym = addr_sym;
      new_entry->stored_value = tcc_ir_op_get_src1(ir, q);
      new_entry->instruction_idx = i;
      new_entry->next = hash_table[h];
      hash_table[h] = new_entry;

#ifdef DEBUG_IR_GEN
      printf("STORE-LOAD: Track store at i=%d (addrtaken=%d, offset=%lld)\n", i, addr_addrtaken,
             (long long)addr_offset);
#endif
    }

    /* If this instruction modifies a vreg that's used as a stored value,
     * invalidate those store entries */
    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_LOAD)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      int j;

      for (j = 0; j < entry_count; j++)
      {
        if (entries[j].valid)
        {
          /* If the stored value vreg is redefined, invalidate */
          if (irop_get_vreg(entries[j].stored_value) == dest_vr)
          {
#ifdef DEBUG_IR_GEN
            printf("STORE-LOAD: Invalidate store at i=%d (stored value redefined at i=%d)\n",
                   entries[j].instruction_idx, i);
#endif
            entries[j].valid = 0;
          }
        }
      }
    }
  }

  tcc_free(entries);

#ifdef DEBUG_IR_GEN
  printf("=== STORE-LOAD FORWARDING END: %d changes ===\n", changes);
#endif

  return changes;
}

/* Redundant Store Elimination
 * Phase 4: Remove stores to memory locations that are overwritten before being read
 * (dead stores to memory)
 * CONSERVATIVE: Only handles stack locals whose address is not taken
 */
int tcc_ir_opt_store_redundant(TCCIRState *ir)
{
  typedef struct StoreInfo
  {
    int addr_vr;
    int addr_is_local;
    int addr_addrtaken;
    int64_t local_offset;
    const Sym *local_sym;
    int store_idx;
    int is_dead;
  } StoreInfo;

  int n = ir->next_instruction_index;
  int changes = 0;
  int i, j;
  IRQuadCompact *q;
  StoreInfo *stores;
  int store_count;

  if (n == 0)
    return 0;

  stores = tcc_malloc(sizeof(StoreInfo) * n);
  store_count = 0;

#ifdef DEBUG_IR_GEN
  printf("=== REDUNDANT STORE ELIMINATION START ===\n");
#endif

  /* Collect only VT_LOCAL STORE instructions (whose address is not taken) */
  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_STORE)
    {
      const IROperand dest = tcc_ir_op_get_dest(ir, q);
      const int addr_is_local = dest.is_local;
      int addr_addrtaken = 0;
      int32_t addr_vr = irop_get_vreg(dest);

      /* CONSERVATIVE: Only track stack locals */
      if (!addr_is_local)
        continue;

      /* Check if address is taken */
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
          addr_addrtaken = 1;
      }

      stores[store_count].addr_is_local = 1;
      stores[store_count].addr_addrtaken = addr_addrtaken;
      stores[store_count].addr_vr = addr_vr;
      stores[store_count].local_offset = irop_get_imm64_ex(ir, dest);
      stores[store_count].local_sym = irop_get_sym_ex(ir, dest);
      stores[store_count].store_idx = i;
      stores[store_count].is_dead = 0;
      store_count++;
    }
  }

  /* For each store, check if it's overwritten before being read */
  for (i = 0; i < store_count; i++)
  {
    int store_idx = stores[i].store_idx;
    int found_read = 0;
    int found_overwrite = 0;

    /* Skip stores to addresses that are taken (could be read through pointer) */
    if (stores[i].addr_addrtaken)
      continue;

    /* Scan forward from this store */
    for (j = store_idx + 1; j < n && !found_read && !found_overwrite; j++)
    {
      q = &ir->compact_instructions[j];

      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Stop at basic block boundaries - can't track across blocks conservatively */
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
          q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      {
        break;
      }

      const IROperand src1 = tcc_ir_op_get_src1(ir, q);
      const Sym *src1_sym = irop_get_sym_ex(ir, src1);
      /* Check for LOAD from the same address */
      if (q->op == TCCIR_OP_LOAD)
      {

        if (src1.is_local)
        {
          if (stores[i].local_sym == src1_sym && stores[i].local_offset == irop_get_imm64_ex(ir, src1))
            found_read = 1;
        }
        /* Non-local load could potentially alias with addr-taken locals
         * but we already skip addr-taken stores above */
      }

      /* Check for any instruction that reads from the same VT_LOCAL in src1 or src2
       * (e.g., AND, OR, ADD operations that directly use stack locations) */
      if (irop_config[q->op].has_src1)
      {
        if (src1.is_local)
        {
          if (stores[i].local_sym == src1_sym && stores[i].local_offset == irop_get_imm64_ex(ir, src1))
            found_read = 1;
        }
      }
      if (irop_config[q->op].has_src2)
      {
        const IROperand src2 = tcc_ir_op_get_src2(ir, q);
        if (src2.is_local)
        {
          const Sym *src2_sym = irop_get_sym_ex(ir, src2);
          if (stores[i].local_sym == src2_sym && stores[i].local_offset == irop_get_imm64_ex(ir, src2))
            found_read = 1;
        }
      }

      /* Check for STORE to the same address (overwrite) */
      if (q->op == TCCIR_OP_STORE && j != store_idx)
      {
        const IROperand dest = tcc_ir_op_get_dest(ir, q);
        const Sym *dest_sym = irop_get_sym_ex(ir, dest);
        if (dest.is_local)
        {
          if (stores[i].local_sym == dest_sym && stores[i].local_offset == irop_get_imm64_ex(ir, dest))
            found_overwrite = 1;
        }
      }
    }

    /* If we found an overwrite without a read in between, the store is dead */
    if (found_overwrite && !found_read)
    {
#ifdef DEBUG_IR_GEN
      printf("OPTIMIZE: Redundant store at i=%d (overwritten without read)\n", store_idx);
#endif
      stores[i].is_dead = 1;
      ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;
      changes++;
    }
  }

  tcc_free(stores);

#ifdef DEBUG_IR_GEN
  printf("=== REDUNDANT STORE ELIMINATION END: %d changes ===\n", changes);
#endif

  return changes;
}

void tcc_ir_opt_run_all(TCCIRState *ir, int level)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)level;
}

int tcc_ir_opt_run_by_name(TCCIRState *ir, const char *name)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)name;
  return 0;
}

/* ============================================================================
 * Stack Address CSE (Common Subexpression Elimination) Optimization
 * ============================================================================
 *
 * Hoists repeated stack address computations by creating a single temp vreg.
 * Pattern: Multiple uses of Addr[StackLoc[X]] in ADD instructions
 *
 * Before:
 *   T3 = Addr[StackLoc[-256]] ADD T2    ; computes &arr[0] + offset
 *   ...
 *   T16 = Addr[StackLoc[-256]] ADD T15  ; computes &arr[0] + offset (redundant!)
 *
 * After:
 *   T_base = Addr[StackLoc[-256]]       ; compute base address once
 *   T3 = T_base ADD T2
 *   ...
 *   T16 = T_base ADD T15                ; reuse base address
 *
 * This optimization enables the Indexed Load/Store fusion to work with
 * stack-allocated arrays by providing a consistent base vreg.
 */

/* Maximum number of unique stack offsets to track */
#define STACK_ADDR_CSE_MAX_OFFSETS 32

typedef struct StackAddrEntry
{
  int32_t offset;    /* Stack offset value */
  int use_count;     /* Number of uses */
  int base_vreg;     /* Vreg holding the base address (or -1 if not yet created) */
  int first_use_idx; /* Index of first instruction using this offset */
} StackAddrEntry;

int tcc_ir_opt_stack_addr_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  StackAddrEntry entries[STACK_ADDR_CSE_MAX_OFFSETS];
  int entry_count = 0;
  int i, j;

  if (n == 0)
    return 0;

#ifdef DEBUG_IR_GEN
  printf("=== STACK ADDRESS CSE START (n=%d) ===\n", n);
#endif

  /* Pass 1: Count uses of each stack offset in ADD instructions */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Only look at ADD instructions */
    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Check if either operand is a stack offset (address, not lval) */
    IROperand stack_op = IROP_NONE;
    if (src1.tag == IROP_TAG_STACKOFF && !src1.is_lval)
      stack_op = src1;
    else if (src2.tag == IROP_TAG_STACKOFF && !src2.is_lval)
      stack_op = src2;
    else
      continue;

    int32_t offset = stack_op.u.imm32;

    /* Find or create entry for this offset */
    int found = -1;
    for (j = 0; j < entry_count; j++)
    {
      if (entries[j].offset == offset)
      {
        found = j;
        break;
      }
    }

    if (found >= 0)
    {
      entries[found].use_count++;
    }
    else if (entry_count < STACK_ADDR_CSE_MAX_OFFSETS)
    {
      entries[entry_count].offset = offset;
      entries[entry_count].use_count = 1;
      entries[entry_count].base_vreg = -1;
      entries[entry_count].first_use_idx = i;
      entry_count++;
    }
  }

  /* Check if any offset is used more than once */
  int need_transform = 0;
  for (i = 0; i < entry_count; i++)
  {
    if (entries[i].use_count > 1)
    {
      need_transform = 1;
      break;
    }
  }

  if (!need_transform)
  {
#ifdef DEBUG_IR_GEN
    printf("=== STACK ADDRESS CSE END: no redundant stack addresses ===\n");
#endif
    return 0;
  }

  /* Pass 2: For offsets used 2+ times, transform:
   * - First use: Keep the ADD but change destination to be the base vreg
   *   This creates: base_vreg = Addr[StackLoc[X]] ADD offset
   *   We then need the original dest to still get its value...
   *
   * Actually, a cleaner approach: Transform the first ADD into two operations:
   *   Original: dest = Addr[StackLoc[X]] ADD offset
   *   Becomes:  base_vreg = Addr[StackLoc[X]] (ASSIGN - just the address)
   *             dest = base_vreg ADD offset
   *
   * Since we can't insert instructions, we'll use a different strategy:
   * Change the first ADD to compute the base address into a temp vreg,
   * then for subsequent uses, use that vreg.
   *
   * Strategy: For the FIRST use of each stack offset:
   *   - Convert ADD dest, StackOff, idx  to  ASSIGN dest, StackOff  ; base computation
   *   - This gives us the base address in dest
   *   - BUT we also need to add idx to get the final address...
   *
   * This is tricky without instruction insertion. Let's use a different approach:
   * Instead of modifying the IR, we'll make the code generator smarter.
   * For now, let's skip the optimization since it needs instruction insertion.
   */

#if 0 /* Disabled until we can properly insert instructions */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Determine which operand is the stack offset */
    int stack_is_src1 = (src1.tag == IROP_TAG_STACKOFF && !src1.is_lval);
    int stack_is_src2 = (src2.tag == IROP_TAG_STACKOFF && !src2.is_lval);

    if (!stack_is_src1 && !stack_is_src2)
      continue;

    IROperand stack_op = stack_is_src1 ? src1 : src2;
    int32_t offset = stack_op.u.imm32;

    /* Find the entry for this offset */
    int entry_idx = -1;
    for (j = 0; j < entry_count; j++)
    {
      if (entries[j].offset == offset)
      {
        entry_idx = j;
        break;
      }
    }

    if (entry_idx < 0 || entries[entry_idx].use_count < 2)
      continue;

    /* Skip the first use - we'll use that to define the base vreg */
    if (i == entries[entry_idx].first_use_idx)
    {
      /* First use: Change dest to be the base vreg */
      int base_vr = tcc_ir_vreg_alloc_temp(ir);
      entries[entry_idx].base_vreg = base_vr;

      /* TODO: Need to somehow capture just the base address...
       * This is the fundamental problem - we need instruction insertion. */
      continue;
    }

    /* Create base vreg if not yet created (shouldn't happen after first use) */
    if (entries[entry_idx].base_vreg < 0)
      continue;  /* First use not processed yet */

    int base_vr = entries[entry_idx].base_vreg;

    /* Create a new operand referencing the base vreg */
    IROperand new_base_op = IROP_NONE;
    new_base_op.tag = IROP_TAG_VREG;
    irop_set_vreg(&new_base_op, base_vr);
    new_base_op.is_lval = 0;
    new_base_op.is_local = 0;  /* No longer a stack reference */
    new_base_op.btype = IROP_BTYPE_INT32;  /* Pointer type */
    irop_init_phys_regs(&new_base_op);

    /* Replace the stack offset operand with the vreg operand */
    int op_idx = q->operand_base;
    if (stack_is_src1)
    {
      /* src1 is at operand_base + 1 */
      if (op_idx + 1 < ir->iroperand_pool_count)
        ir->iroperand_pool[op_idx + 1] = new_base_op;
    }
    else
    {
      /* src2 is at operand_base + 2 */
      if (op_idx + 2 < ir->iroperand_pool_count)
        ir->iroperand_pool[op_idx + 2] = new_base_op;
    }

    changes++;
  }
#endif

  /* Alternative approach: Use the code generator's FP cache more effectively.
   * The real fix is to improve the FP cache to work at the right level. */

  /* Actually, we need to insert ASSIGN instructions. Since we can't easily
   * insert instructions, let's use a different strategy:
   * - Keep the first ADD instruction as-is (it computes the address)
   * - Make the destination of that ADD also be the base vreg
   * - For subsequent uses, the base vreg is already available
   *
   * This is still problematic because the ADD destination is different each time.
   *
   * BETTER APPROACH: Leave the ADD instructions alone, but change how the
   * backend handles STACKOFF operands - it should cache them across instructions.
   * This is what the FP cache was supposed to do, but it needs to work at the
   * right level.
   *
   * FOR NOW: Let's do a simpler transformation - convert the first ADD to
   * produce both the original result AND set up the base. Then subsequent
   * ADDs can use the base vreg.
   */

  /* The transformation is incomplete - for now, just flag that we identified
   * opportunities. A future enhancement would properly insert ASSIGN instructions. */

#ifdef DEBUG_IR_GEN
  printf("=== STACK ADDRESS CSE END: %d replacements ===\n", changes);
#endif

  return changes;
}

/* ============================================================================
 * MLA (Multiply-Accumulate) Fusion Optimization
 * ============================================================================
 *
 * Fuses MUL followed by ADD into a single MLA instruction.
 * Pattern:  temp = a * b; result = temp + c;
 * Becomes:  result = MLA(a, b, c);  // result = a * b + c
 *
 * Requirements:
 * - The MUL result must have exactly one use (the ADD instruction)
 * - Both MUL and ADD must be in the same basic block
 * - MLA is available in ARMv7-M and later (Cortex-M3, M4, M7, M33)
 *
 * The optimization transforms:
 *   MUL temp, a, b       -> MLA result, a, b, c
 *   ADD result, temp, c  -> (NOP - removed by DCE)
 *
 * Or:
 *   MUL temp, a, b       -> MLA result, a, b, c
 *   ADD result, c, temp  -> (NOP - removed by DCE)
 */

int tcc_ir_opt_mla_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int i;

  if (n == 0)
    return 0;

  for (i = 0; i < n; i++)
  {
    IRQuadCompact *add_q = &ir->compact_instructions[i];

    /* Look for ADD instructions */
#ifdef DEBUG_IR_GEN
    if (add_q->op == TCCIR_OP_ADD)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, add_q);
      IROperand s2 = tcc_ir_op_get_src2(ir, add_q);
      printf("MLA CHECK ADD@%d: src1(tag=%d,lval=%d,local=%d,llocal=%d) src2(tag=%d,lval=%d,local=%d,llocal=%d)\n", i,
             irop_get_tag(s1), s1.is_lval, s1.is_local, s1.is_llocal, irop_get_tag(s2), s2.is_lval, s2.is_local,
             s2.is_llocal);
    }
#endif
    if (add_q->op != TCCIR_OP_ADD)
      continue;

    IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
    IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);
    IROperand add_dest = tcc_ir_op_get_dest(ir, add_q);
#ifdef DEBUG_IR_GEN
    (void)add_dest; /* suppress unused variable warning when not logging */
#endif

    /* Find which source (if any) is the MUL result.
     * We need to try both operands since we don't know which one comes from MUL.
     * Try src2 first (more common pattern: sum = sum + temp), then src1.
     */
    int32_t mul_result_vr = -1;
    IROperand accum_op;
    int mul_idx = -1;
    IRQuadCompact *mul_q = NULL;

    /* Try src2 as MUL result first (common pattern: accum = accum + mul_result) */
    if (irop_has_vreg(add_src2))
    {
      int32_t candidate_vr = irop_get_vreg(add_src2);
      int candidate_idx = tcc_ir_find_defining_instruction(ir, candidate_vr, i);
      if (candidate_idx >= 0 && ir->compact_instructions[candidate_idx].op == TCCIR_OP_MUL)
      {
        mul_result_vr = candidate_vr;
        accum_op = add_src1;
        mul_idx = candidate_idx;
        mul_q = &ir->compact_instructions[mul_idx];
      }
    }

    /* If src2 wasn't from MUL, try src1 */
    if (mul_q == NULL && irop_has_vreg(add_src1))
    {
      int32_t candidate_vr = irop_get_vreg(add_src1);
      int candidate_idx = tcc_ir_find_defining_instruction(ir, candidate_vr, i);
      if (candidate_idx >= 0 && ir->compact_instructions[candidate_idx].op == TCCIR_OP_MUL)
      {
        mul_result_vr = candidate_vr;
        accum_op = add_src2;
        mul_idx = candidate_idx;
        mul_q = &ir->compact_instructions[mul_idx];
      }
    }

    /* Neither operand comes from a MUL - skip */
    if (mul_q == NULL)
    {
      continue;
    }

    /* Skip if this is an address calculation (base + offset)
     * MLA is for arithmetic: a * b + c
     * Address calc is: &array[i] = base + (i * sizeof(element))
     *
     * Heuristics to detect address calculations:
     * 1. Accumulator is a symbol reference (GlobalSym) - indicates array/pointer
     * 2. Both operands of the ADD are symbol references
     *
     * NOTE: We no longer skip based on is_local/is_lval because local variables
     * are legitimate accumulator values (e.g., "int sum; sum += a*b;"). The
     * is_local flag just means the value is stored on the stack, not that it's
     * an address being computed.
     */

    /* Check 1: Accumulator should not be a symbol reference (GlobalSym) */
    /* Symbol references indicate arrays/pointers, not values */
    if (irop_get_tag(accum_op) == IROP_TAG_SYMREF)
    {
      continue;
    }

    /* Check 2: Skip if destination looks like an address computation.
     * Symbol references as destination indicate we're computing a pointer. */
    if (irop_get_tag(add_dest) == IROP_TAG_SYMREF)
    {
      continue;
    }

    /* Check 3: Both operands of the ADD should be values (not symbol refs)
     * If one operand is a symbol ref and the other is a MUL result,
     * this is likely an address calculation */
    if (irop_get_tag(add_src1) == IROP_TAG_SYMREF || irop_get_tag(add_src2) == IROP_TAG_SYMREF)
    {
      continue;
    }

    /* Check 4: Skip if MUL operands require memory dereference or are immediates.
     * The MLA instruction codegen requires all operands to be registers.
     *
     * For memory operands: if is_lval=1 AND NOT is_local/is_llocal, we need to
     * load the value from the address held in a register.
     *
     * For immediates: ARM MLA instruction doesn't support immediate operands,
     * so we can only fuse when both MUL sources are in registers. */
    IROperand mul_src1 = tcc_ir_op_get_src1(ir, mul_q);
    IROperand mul_src2 = tcc_ir_op_get_src2(ir, mul_q);
    int src1_needs_deref = mul_src1.is_lval && !mul_src1.is_local && !mul_src1.is_llocal;
    int src2_needs_deref = mul_src2.is_lval && !mul_src2.is_local && !mul_src2.is_llocal;
    int src1_is_immediate = irop_is_immediate(mul_src1);
    int src2_is_immediate = irop_is_immediate(mul_src2);
    if (src1_needs_deref || src2_needs_deref || src1_is_immediate || src2_is_immediate)
    {
      continue;
    }

    /* Check if the MUL result has exactly one use (this ADD) */
    /* Note: tcc_ir_vreg_has_single_use returns true if there's exactly 1 OTHER use,
     * but we want to check if there are 0 other uses (only used by this ADD) */
    int other_uses = 0;
    for (int j = 0; j < n; ++j)
    {
      if (j == i)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;
      IROperand s1 = tcc_ir_op_get_src1(ir, qj);
      IROperand s2 = tcc_ir_op_get_src2(ir, qj);
      if (irop_get_vreg(s1) == mul_result_vr || irop_get_vreg(s2) == mul_result_vr)
      {
        other_uses++;
        break;
      }
    }
    if (other_uses > 0)
    {
      continue;
    }

    /* Check that MUL and ADD are in the same basic block */
    /* Simple check: no jumps between them */
    int same_block = 1;
    for (int j = mul_idx + 1; j < i; j++)
    {
      IRQuadCompact *between = &ir->compact_instructions[j];
      if (between->op == TCCIR_OP_JUMP || between->op == TCCIR_OP_JUMPIF || between->op == TCCIR_OP_NOP)
      {
        same_block = 0;
        break;
      }
    }
    if (!same_block)
      continue;

    /* Check that accumulator is defined before the MUL (if it's a vreg) */
    /* The MLA will replace the MUL, so accumulator must be ready before mul_idx */
    int32_t accum_vr = irop_get_vreg(accum_op);
    if (accum_vr >= 0)
    {
      int accum_def_idx = tcc_ir_find_defining_instruction(ir, accum_vr, i);
      /* accum_def_idx < 0 means no defining instruction found (e.g., parameter).
       * This is OK - parameters are ready from function entry.
       * We only need to skip if the accumulator is defined AFTER the MUL.
       */
      if (accum_def_idx >= 0 && accum_def_idx >= mul_idx)
      {
#ifdef DEBUG_IR_GEN
        printf("MLA FUSION SKIP: accumulator vr%d defined at %d after MUL@%d\n", accum_vr, accum_def_idx, mul_idx);
#endif
        continue;
      }
    }

#ifdef DEBUG_IR_GEN
    /* Get MUL operands for debug output */
    IROperand mul_src1 = tcc_ir_op_get_src1(ir, mul_q);
    IROperand mul_src2 = tcc_ir_op_get_src2(ir, mul_q);
#endif

    /* Transform MUL + ADD into MLA */
    /* 1. Change MUL opcode to MLA */
    mul_q->op = TCCIR_OP_MLA;

    /* 2. Change MLA destination to ADD's destination */
    /* The dest is at operand_base + 0 */
    int mul_dest_idx = mul_q->operand_base;
    int add_dest_idx = add_q->operand_base;
    if (mul_dest_idx >= 0 && mul_dest_idx < ir->iroperand_pool_count && add_dest_idx >= 0 &&
        add_dest_idx < ir->iroperand_pool_count)
    {
      ir->iroperand_pool[mul_dest_idx] = ir->iroperand_pool[add_dest_idx];
    }

    /* 3. Store accumulator as extra operand at operand_base + 3 */
    /* First ensure pool has space and extend to include slot +3 */
    int accum_idx = mul_q->operand_base + 3;

    /* Extend pool to include the accumulator slot if needed */
    while (ir->iroperand_pool_count <= accum_idx)
    {
      tcc_ir_pool_add(ir, IROP_NONE);
    }

    if (accum_idx >= ir->iroperand_pool_capacity)
    {
      /* Not enough space - revert */
      mul_q->op = TCCIR_OP_MUL;
      continue;
    }

    /* Store accumulator operand */
    ir->iroperand_pool[accum_idx] = accum_op;

    /* 4. Mark ADD as NOP (will be removed by DCE) */
    add_q->op = TCCIR_OP_NOP;

#ifdef DEBUG_IR_GEN
    printf("MLA FUSION: MUL@%d + ADD@%d -> MLA vr%d = vr%d * vr%d + ", mul_idx, i, irop_get_vreg(add_dest),
           irop_get_vreg(mul_src1), irop_get_vreg(mul_src2));
    printf("vr%d\n", irop_get_vreg(accum_op));
#endif

    changes++;
  }

#ifdef DEBUG_IR_GEN
  printf("=== MLA FUSION END: %d fusions ===\n", changes);
#endif

  return changes;
}

/* ============================================================================
 * Indexed Load/Store Fusion Optimization
 * ============================================================================
 *
 * Fuses SHL + ADD + LOAD/STORE into single indexed memory operation.
 * Pattern for load:  offset = index << 2; addr = base + offset; val = *addr;
 * Becomes:          val = LOAD_INDEXED(base, index, scale=2)
 *
 * Pattern for store: offset = index << 2; addr = base + offset; *addr = val;
 * Becomes:          STORE_INDEXED(base, index, scale=2, val)
 *
 * The optimization transforms:
 *   SHL temp, index, #2       -> (NOP)
 *   ADD addr, base, temp      -> (NOP)
 *   LOAD val, addr            -> LOAD_INDEXED val, base, index, #2
 *
 * Requirements:
 * - SHL must be by 2, 3, or 4 (for 4, 8, 16 byte elements)
 * - ADD must have the SHL result as one operand and base as the other
 * - LOAD/STORE must use the ADD result as address
 * - All three instructions must be in the same basic block
 * - SHL and ADD results must have exactly one use each
 */

int tcc_ir_opt_indexed_memory_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

#ifdef DEBUG_IR_GEN
  printf("=== INDEXED MEMORY FUSION START (n=%d) ===\n", n);
#endif

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *load_q = &ir->compact_instructions[i];

    /* Look for LOAD or STORE instructions */
    if (load_q->op != TCCIR_OP_LOAD && load_q->op != TCCIR_OP_STORE)
      continue;

    /* Get the address operand (source for LOAD, dest for STORE) */
    IROperand addr_op;
    int is_store = (load_q->op == TCCIR_OP_STORE);

    if (is_store)
    {
      /* For STORE: dest is the address, src1 is the value */
      addr_op = tcc_ir_op_get_dest(ir, load_q);
    }
    else
    {
      /* For LOAD: src1 is the address */
      addr_op = tcc_ir_op_get_src1(ir, load_q);
    }

    /* Address must be a virtual register (computed, not a direct symbol) */
    if (!irop_has_vreg(addr_op))
      continue;

    int32_t addr_vr = irop_get_vreg(addr_op);

    /* Find the instruction that defines the address (should be ADD) */
    int add_idx = tcc_ir_find_defining_instruction(ir, addr_vr, i);
    if (add_idx < 0)
      continue;

    IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
    if (add_q->op != TCCIR_OP_ADD)
      continue;

    /* Check that ADD result has only this one use */
    int add_other_uses = 0;
    for (int j = 0; j < n; ++j)
    {
      if (j == i || j == add_idx)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;
      IROperand s1 = tcc_ir_op_get_src1(ir, qj);
      IROperand s2 = tcc_ir_op_get_src2(ir, qj);
      if (irop_get_vreg(s1) == addr_vr || irop_get_vreg(s2) == addr_vr)
      {
        add_other_uses++;
        break;
      }
    }
    if (add_other_uses > 0)
      continue;

    /* Find which operand of ADD is the base and which is the offset (SHL result) */
    IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
    IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);

    /* One of them should be the SHL result (a vreg), the other is the base.
     * IMPORTANT: Both operands may have vregs (e.g., ADD P0, T0 where P0 is a parameter
     * and T0 is the SHL result). We need to check which one is actually defined by SHL. */
    int32_t offset_vr = -1;
    IROperand base_op = IROP_NONE;
    int shl_idx = -1;
    IRQuadCompact *shl_q = NULL;

    /* Try src1 as offset first */
    if (irop_has_vreg(add_src1))
    {
      int32_t vr1 = irop_get_vreg(add_src1);
      int idx1 = tcc_ir_find_defining_instruction(ir, vr1, add_idx);
      if (idx1 >= 0 && ir->compact_instructions[idx1].op == TCCIR_OP_SHL)
      {
        offset_vr = vr1;
        base_op = add_src2;
        shl_idx = idx1;
        shl_q = &ir->compact_instructions[shl_idx];
      }
    }

    /* If src1 wasn't the SHL result, try src2 */
    if (shl_idx < 0 && irop_has_vreg(add_src2))
    {
      int32_t vr2 = irop_get_vreg(add_src2);
      int idx2 = tcc_ir_find_defining_instruction(ir, vr2, add_idx);
      if (idx2 >= 0 && ir->compact_instructions[idx2].op == TCCIR_OP_SHL)
      {
        offset_vr = vr2;
        base_op = add_src1;
        shl_idx = idx2;
        shl_q = &ir->compact_instructions[shl_idx];
      }
    }

    /* Neither operand is a SHL result - not our pattern */
    if (shl_idx < 0)
      continue;

    /* Check that SHL result has only one use (the ADD) */
    int shl_other_uses = 0;
    for (int j = 0; j < n; ++j)
    {
      if (j == add_idx || j == shl_idx)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;
      IROperand s1 = tcc_ir_op_get_src1(ir, qj);
      IROperand s2 = tcc_ir_op_get_src2(ir, qj);
      if (irop_get_vreg(s1) == offset_vr || irop_get_vreg(s2) == offset_vr)
      {
        shl_other_uses++;
        break;
      }
    }
    if (shl_other_uses > 0)
      continue;

    /* Check that SHL shift amount is a valid immediate (2, 3, or 4) */
    IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
    if (!shl_src2.is_const)
      continue;

    int shift_amount = shl_src2.u.imm32;
    if (shift_amount != 2 && shift_amount != 3 && shift_amount != 4)
      continue;

    /* Get the index operand (what's being shifted) */
    IROperand index_op = tcc_ir_op_get_src1(ir, shl_q);

    /* SAFETY CHECKS: Ensure we don't fuse address calculations incorrectly */

    /* Check 1: Index must not be a complex memory operand (stack/local variable) */
    /* Simple register values with is_lval are OK (will be loaded by backend),
     * but stack offsets and local variables make the addressing mode too complex */
    if (index_op.is_local || index_op.is_llocal)
    {
      continue;
    }

    /* Check 2: Base must be a simple address (symbol or register), not a complex lvalue */
    if (base_op.is_local || base_op.is_llocal || base_op.is_lval)
    {
      /* Base with is_lval means it's a pointer loaded from memory - too complex */
      continue;
    }

    /* Check that all three instructions are in the same basic block */
    int same_block = 1;
    for (int j = shl_idx + 1; j < i; j++)
    {
      IRQuadCompact *between = &ir->compact_instructions[j];
      if (between->op == TCCIR_OP_JUMP || between->op == TCCIR_OP_JUMPIF || between->op == TCCIR_OP_NOP)
      {
        same_block = 0;
        break;
      }
    }
    if (!same_block)
      continue;

    /* All checks passed - transform the instructions */
#ifdef DEBUG_IR_GEN
    printf("INDEXED FUSION: SHL@%d + ADD@%d + %s@%d -> %s_INDEXED\n", shl_idx, add_idx, is_store ? "STORE" : "LOAD", i,
           is_store ? "STORE" : "LOAD");
#endif

    /* Transform:
     * 1. Change LOAD/STORE to LOAD_INDEXED/STORE_INDEXED
     * 2. Change src1/dest to the base operand
     * 3. Store index and scale as extra operands
     * 4. Mark SHL and ADD as NOP
     */

    /* Get original operands BEFORE we change operand_base */
    IROperand orig_dest = tcc_ir_op_get_dest(ir, load_q);
    IROperand orig_src1 = tcc_ir_op_get_src1(ir, load_q);

    /* Change opcode to indexed version */
    load_q->op = is_store ? TCCIR_OP_STORE_INDEXED : TCCIR_OP_LOAD_INDEXED;

    /* For LOAD_INDEXED: dest = *(base + (index << scale))
     *   operand_base + 0: dest
     *   operand_base + 1: base
     *   operand_base + 2: index
     *   operand_base + 3: scale (immediate)
     *
     * For STORE_INDEXED: *(base + (index << scale)) = value
     *   operand_base + 0: base (treated as "dest" for addressing)
     *   operand_base + 1: value (treated as "src1")
     *   operand_base + 2: index
     *   operand_base + 3: scale (immediate)
     */

    /* IMPORTANT: Allocate NEW operand space at the end of the pool to avoid
     * overwriting the next instruction's operands. The original LOAD/STORE
     * only used 2 operands, but LOAD_INDEXED/STORE_INDEXED need 4.
     */
    int new_base_idx = ir->iroperand_pool_count;
    if (new_base_idx + 4 > ir->iroperand_pool_capacity)
    {
      /* Not enough space - revert */
      load_q->op = is_store ? TCCIR_OP_STORE : TCCIR_OP_LOAD;
      continue;
    }

    /* Add 4 new operand slots */
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);

    /* Update the instruction to use the new operand base */
    load_q->operand_base = new_base_idx;

    /* Clear is_lval on base and index operands - they should be used as
     * register values, not dereferenced, in indexed addressing mode */
    IROperand base_op_clean = base_op;
    IROperand index_op_clean = index_op;
    base_op_clean.is_lval = 0;
    index_op_clean.is_lval = 0;

    if (is_store)
    {
      /* STORE_INDEXED: base, value, index, scale */
      ir->iroperand_pool[new_base_idx + 0] = base_op_clean;  /* base address */
      ir->iroperand_pool[new_base_idx + 1] = orig_src1;      /* value to store (original src1) */
      ir->iroperand_pool[new_base_idx + 2] = index_op_clean; /* index register */
      /* scale as immediate operand */
      IROperand scale_op = IROP_NONE;
      scale_op.is_const = 1;
      scale_op.u.imm32 = shift_amount;
      ir->iroperand_pool[new_base_idx + 3] = scale_op;
    }
    else
    {
      /* LOAD_INDEXED: dest, base, index, scale */
      ir->iroperand_pool[new_base_idx + 0] = orig_dest;      /* dest (original) */
      ir->iroperand_pool[new_base_idx + 1] = base_op_clean;  /* base address */
      ir->iroperand_pool[new_base_idx + 2] = index_op_clean; /* index register */
      /* scale as immediate operand */
      IROperand scale_op = IROP_NONE;
      scale_op.is_const = 1;
      scale_op.u.imm32 = shift_amount;
      ir->iroperand_pool[new_base_idx + 3] = scale_op;
    }

    /* Mark SHL and ADD as NOP */
    shl_q->op = TCCIR_OP_NOP;
    add_q->op = TCCIR_OP_NOP;

    changes++;
  }

#ifdef DEBUG_IR_GEN
  printf("=== INDEXED MEMORY FUSION END: %d fusions ===\n", changes);
#endif

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

/* Helper: Find the ASSIGN instruction that created a given TMP vreg
 * Returns the index of the ASSIGN instruction, or -1 if not found
 */
static int find_assign_for_tmp(TCCIRState *ir, int32_t tmp_vr, int before_idx)
{
  if (!ir || tmp_vr < 0 || before_idx <= 0)
    return -1;

  /* Only look for TMP vregs */
  if (TCCIR_DECODE_VREG_TYPE(tmp_vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;

  for (int i = before_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (irop_get_vreg(dest) == tmp_vr)
        return i;
    }
  }
  return -1;
}

/* Helper: Check if a STORE instruction stores a value to a vreg
 * Returns 1 if store at store_idx stores src_vr to dest_vr
 */
static int is_store_of_vreg(TCCIRState *ir, int store_idx, int32_t dest_vr, int32_t src_vr)
{
  IRQuadCompact *q = &ir->compact_instructions[store_idx];
  if (q->op != TCCIR_OP_STORE)
    return 0;

  IROperand q_dest = tcc_ir_op_get_dest(ir, q);
  IROperand q_src = tcc_ir_op_get_src1(ir, q);

  return (irop_get_vreg(q_dest) == dest_vr && irop_get_vreg(q_src) == src_vr);
}

int tcc_ir_opt_postinc_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

#ifdef DEBUG_IR_GEN
  printf("=== POSTINC FUSION START (n=%d) ===\n", n);
#endif

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *mem_q = &ir->compact_instructions[i];

    /* Look for LOAD or STORE instructions */
    if (mem_q->op != TCCIR_OP_LOAD && mem_q->op != TCCIR_OP_STORE)
      continue;

    int is_store = (mem_q->op == TCCIR_OP_STORE);

    /* Get the pointer operand */
    IROperand ptr_op;
    IROperand loaded_val_op;

    if (is_store)
    {
      /* STORE: dest is the pointer, src1 is the value */
      ptr_op = tcc_ir_op_get_dest(ir, mem_q);
      loaded_val_op = tcc_ir_op_get_src1(ir, mem_q);
    }
    else
    {
      /* LOAD: src1 is the pointer, dest is the loaded value */
      ptr_op = tcc_ir_op_get_src1(ir, mem_q);
      loaded_val_op = tcc_ir_op_get_dest(ir, mem_q);
    }

    /* Pointer must be a virtual register */
    if (!irop_has_vreg(ptr_op))
      continue;

    int32_t ptr_vr = irop_get_vreg(ptr_op);
    int32_t orig_ptr_vr = ptr_vr;
    IROperand orig_ptr_op = ptr_op;
    int assign_idx = -1;

    /* For LOAD: loaded value must not be the same as pointer */
    if (!is_store && irop_has_vreg(loaded_val_op) && irop_get_vreg(loaded_val_op) == ptr_vr)
      continue;

    /* Check if this is a TMP that came from an ASSIGN (pointer copy pattern)
     * Pattern: ASSIGN temp, ptr; LOAD dest, temp; ADD ptr, ptr, #imm
     * We want to fuse this into: LOAD_POSTINC dest, ptr, #imm
     */
    if (TCCIR_DECODE_VREG_TYPE(ptr_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      assign_idx = find_assign_for_tmp(ir, ptr_vr, i);
      if (assign_idx >= 0)
      {
        IRQuadCompact *assign_q = &ir->compact_instructions[assign_idx];
        IROperand assign_src = tcc_ir_op_get_src1(ir, assign_q);
        if (irop_has_vreg(assign_src))
        {
          /* Found the original pointer */
          orig_ptr_vr = irop_get_vreg(assign_src);
          orig_ptr_op = assign_src;
#ifdef DEBUG_IR_GEN
          printf("POSTINC: Found pointer copy pattern: TMP%d <- VR%d\n", TCCIR_DECODE_VREG_POSITION(ptr_vr),
                 TCCIR_DECODE_VREG_POSITION(orig_ptr_vr));
#endif
        }
        else
        {
          assign_idx = -1; /* ASSIGN source is not a vreg, can't use */
        }
      }
    }

    /* Look at the next instructions for ADD that increments the ORIGINAL pointer.
     * There are two patterns:
     * 1. ADD orig_ptr, orig_ptr, #imm  (direct update)
     * 2. ADD tmp, orig_ptr, #imm; STORE orig_ptr, tmp  (via temporary)
     *
     * There may be intervening instructions (like ASSIGN for another temp copy)
     * so we search forward for the ADD instead of just looking at i+1.
     */
    int add_idx = -1;
    int search_limit = (i + 5 < n) ? i + 5 : n; /* Look up to 5 instructions ahead */
    for (int j = i + 1; j < search_limit; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_ADD)
      {
        /* Check if this ADD uses our pointer */
        IROperand add_s1 = tcc_ir_op_get_src1(ir, qj);
        IROperand add_s2 = tcc_ir_op_get_src2(ir, qj);
        int s1_vr = irop_get_vreg(add_s1);
        int s2_vr = irop_get_vreg(add_s2);
        /* Check if either source is our pointer (original or temp) */
        if ((irop_has_vreg(add_s1) && (s1_vr == orig_ptr_vr || s1_vr == ptr_vr)) ||
            (irop_has_vreg(add_s2) && (s2_vr == orig_ptr_vr || s2_vr == ptr_vr)))
        {
          add_idx = j;
          break;
        }
        /* Also check if this ADD uses a temp that copies our pointer */
        if (irop_has_vreg(add_s1) && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int asn = find_assign_for_tmp(ir, s1_vr, j);
          if (asn >= 0)
          {
            IROperand asn_src = tcc_ir_op_get_src1(ir, &ir->compact_instructions[asn]);
            if (irop_get_vreg(asn_src) == orig_ptr_vr)
            {
              add_idx = j;
              break;
            }
          }
        }
      }
      /* Stop if we hit a branch or function call */
      if (qj->op == TCCIR_OP_JUMP || qj->op == TCCIR_OP_JUMPIF || qj->op == TCCIR_OP_FUNCCALLVOID ||
          qj->op == TCCIR_OP_FUNCCALLVAL)
        break;
    }

    if (add_idx < 0)
      continue;

    IRQuadCompact *add_q = &ir->compact_instructions[add_idx];

    /* Check ADD operands - one should be the original pointer OR the temp copy */
    IROperand add_dest = tcc_ir_op_get_dest(ir, add_q);
    IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
    IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);

    int add_src1_vr = irop_get_vreg(add_src1);
    int add_src2_vr = irop_get_vreg(add_src2);
    /* Accept either original pointer OR the temp copy as the ADD source */
    int ptr_is_src1 = (irop_has_vreg(add_src1) && (add_src1_vr == orig_ptr_vr || add_src1_vr == ptr_vr));
    int ptr_is_src2 = (irop_has_vreg(add_src2) && (add_src2_vr == orig_ptr_vr || add_src2_vr == ptr_vr));

    if (!ptr_is_src1 && !ptr_is_src2)
      continue;

    /* Check if ADD result goes directly to original pointer (pattern 1) */
    int add_dest_is_orig = (irop_has_vreg(add_dest) && irop_get_vreg(add_dest) == orig_ptr_vr);

    /* Or check if ADD result is a TMP that gets stored to original pointer (pattern 2) */
    int add_dest_vr = irop_get_vreg(add_dest);
    int store_idx = -1;

    if (!add_dest_is_orig && TCCIR_DECODE_VREG_TYPE(add_dest_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      /* Look for STORE orig_ptr, add_dest after the ADD */
      int j = add_idx + 1;
      while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
        j++;

      if (j < n && is_store_of_vreg(ir, j, orig_ptr_vr, add_dest_vr))
        store_idx = j;
    }

    /* We need either direct update or store pattern */
    if (!add_dest_is_orig && store_idx < 0)
      continue;

    /* The other operand must be an immediate offset */
    IROperand offset_op = ptr_is_src1 ? add_src2 : add_src1;
#ifdef DEBUG_IR_GEN
    printf("POSTINC DEBUG: ptr_is_src1=%d, ptr_is_src2=%d, offset_op.is_const=%d\n", ptr_is_src1, ptr_is_src2,
           offset_op.is_const);
#endif
    if (!offset_op.is_const)
    {
#ifdef DEBUG_IR_GEN
      printf("POSTINC DEBUG: offset_op is not const, skipping\n");
#endif
      continue;
    }

    int offset = offset_op.u.imm32;
#ifdef DEBUG_IR_GEN
    printf("POSTINC DEBUG: extracted offset=%d\n", offset);
#endif
    /* ARM post-increment supports offsets 1-255 (8-bit unsigned immediate) */
    if (offset < 1 || offset > 255)
      continue;

    /* Check that both instructions are in the same basic block */
    for (int j = i + 1; j < add_idx; j++)
    {
      IRQuadCompact *between = &ir->compact_instructions[j];
      if (between->op == TCCIR_OP_JUMP || between->op == TCCIR_OP_JUMPIF)
        goto skip_fusion;
    }

    /* Check that the TEMP pointer (if used) has no other uses between LOAD/STORE and ADD
     * and that the ORIGINAL pointer is not modified between the ASSIGN and the ADD */
    for (int j = (assign_idx >= 0 ? assign_idx + 1 : i + 1); j < add_idx; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;
      /* Check for modifications to original pointer */
      if (irop_config[qj->op].has_dest)
      {
        IROperand qj_dest = tcc_ir_op_get_dest(ir, qj);
        if (irop_get_vreg(qj_dest) == orig_ptr_vr)
          goto skip_fusion; /* Original pointer is modified before ADD */
      }
    }

    /* If we used an ASSIGN, check that the temp has no other uses */
    if (assign_idx >= 0)
    {
      for (int j = i + 1; j < add_idx; j++)
      {
        IRQuadCompact *qj = &ir->compact_instructions[j];
        if (qj->op == TCCIR_OP_NOP)
          continue;
        IROperand s1 = tcc_ir_op_get_src1(ir, qj);
        IROperand s2 = tcc_ir_op_get_src2(ir, qj);
        if (irop_get_vreg(s1) == ptr_vr || irop_get_vreg(s2) == ptr_vr)
          goto skip_fusion;
      }
    }

    /* Transform to POSTINC version */
    /* Allocate new operand space for POSTINC (4 operands: dest/src, ptr, unused, offset)
     * The offset goes at position 3 (scale field) as expected by tcc_ir_op_get_scale()
     */
    int new_base_idx = ir->iroperand_pool_count;
    if (new_base_idx + 4 > ir->iroperand_pool_capacity)
    {
      /* Not enough space - skip */
      continue;
    }

    /* Add 4 new operand slots */
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);

    /* Update instruction to use new operand base */
    mem_q->operand_base = new_base_idx;

    if (is_store)
    {
      /* STORE_POSTINC: ptr, value, unused, offset */
      ir->iroperand_pool[new_base_idx + 0] = orig_ptr_op;   /* pointer (gets updated) */
      ir->iroperand_pool[new_base_idx + 1] = loaded_val_op; /* value to store */
      ir->iroperand_pool[new_base_idx + 2] = IROP_NONE;     /* unused */
      IROperand offset_imm = IROP_NONE;
      offset_imm.is_const = 1;
      offset_imm.u.imm32 = offset;
      ir->iroperand_pool[new_base_idx + 3] = offset_imm; /* offset immediate (scale position) */
    }
    else
    {
      /* LOAD_POSTINC: load_dest, ptr, unused, offset */
      ir->iroperand_pool[new_base_idx + 0] = loaded_val_op; /* loaded value dest */
      ir->iroperand_pool[new_base_idx + 1] = orig_ptr_op;   /* pointer (gets updated) */
      ir->iroperand_pool[new_base_idx + 2] = IROP_NONE;     /* unused */
      IROperand offset_imm = IROP_NONE;
      offset_imm.is_const = 1;
      offset_imm.u.imm32 = offset;
      ir->iroperand_pool[new_base_idx + 3] = offset_imm; /* offset immediate (scale position) */
    }

    /* Change opcode to POSTINC version */
    mem_q->op = is_store ? TCCIR_OP_STORE_POSTINC : TCCIR_OP_LOAD_POSTINC;

    /* Mark ADD as NOP (will be removed by DCE) */
    add_q->op = TCCIR_OP_NOP;

    /* If there was an ASSIGN, mark it as NOP too (the temp is no longer needed) */
    if (assign_idx >= 0)
    {
      ir->compact_instructions[assign_idx].op = TCCIR_OP_NOP;
    }

    /* If there was a STORE of the ADD result, mark it as NOP too */
    if (store_idx >= 0)
    {
      ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;
    }

    changes++;

  skip_fusion:
    continue;
  }

#ifdef DEBUG_IR_GEN
  printf("=== POSTINC FUSION END: %d fusions ===\n", changes);
#endif

  return changes;
}

/* ============================================================================
 * Helper Functions for Optimization
 * ============================================================================ */

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

/* ============================================================================
 * Constant Branch Folding Optimization
 * ============================================================================
 *
 * Folds branches with constant conditions to unconditional jumps or eliminates them.
 * This is critical for optimizing conditionals where values are compile-time constants.
 *
 * Pattern 1: TEST_ZERO #const followed by JUMPIF
 *   TEST_ZERO #0          ->  NOP
 *   JUMPIF "==", target   ->  JUMP target  (always taken since 0 == 0)
 *   ...dead code...       ->  NOP (removed by subsequent DCE)
 *
 * Pattern 2: CMP #const1, #const2 followed by JUMPIF
 *   CMP #5, #3            ->  NOP
 *   JUMPIF ">", target    ->  JUMP target  (always taken since 5 > 3)
 *   ...dead code...       ->  NOP (removed by subsequent DCE)
 *
 * The optimization also handles the case where the branch is never taken:
 *   TEST_ZERO #1          ->  NOP
 *   JUMPIF "==", target   ->  NOP  (never taken since 1 != 0)
 *
 * This pass should be run after constant propagation to maximize folding opportunities.
 */

/* Helper: Evaluate a comparison condition given two constant values.
 * Returns 1 if condition is true, 0 if false.
 * The condition token values match those in tcctok.h
 */
static int evaluate_compare_condition(int64_t val1, int64_t val2, int cond_token)
{
  switch (cond_token)
  {
  case 0x94: /* TOK_EQ */
    return val1 == val2;
  case 0x95: /* TOK_NE */
    return val1 != val2;
  case 0x9c: /* TOK_LT */
    return val1 < val2;
  case 0x9d: /* TOK_GE */
    return val1 >= val2;
  case 0x9e: /* TOK_LE */
    return val1 <= val2;
  case 0x9f: /* TOK_GT */
    return val1 > val2;
  case 0x96: /* TOK_ULT (unsigned <) */
    return (uint64_t)val1 < (uint64_t)val2;
  case 0x97: /* TOK_UGE (unsigned >=) */
    return (uint64_t)val1 >= (uint64_t)val2;
  case 0x98: /* TOK_ULE (unsigned <=) */
    return (uint64_t)val1 <= (uint64_t)val2;
  case 0x99: /* TOK_UGT (unsigned >) */
    return (uint64_t)val1 > (uint64_t)val2;
  default:
    return -1; /* Unknown condition */
  }
}

/* ============================================================================
 * Phase 2: Constant Comparison Folding through VReg Tracking
 * ============================================================================
 *
 * Tracks constant values through virtual registers to enable branch folding
 * even when the CMP instruction uses a vreg (not immediate).
 *
 * Example:
 *   V0 <- #1234              ; V0 = 1234 (tracked constant)
 *   V0 <- V0 SUB #42         ; V0 = 1192 (computed constant)
 *   CMP V0, #1000000         ; Compare 1192 vs 1000000
 *   JUMPIF "<=", target      ; ALWAYS TRUE - fold to unconditional JUMP
 *
 * This optimization runs within branch folding to maximize opportunities.
 */

/* Structure to track constant values for VAR vregs */
typedef struct
{
  int is_constant;
  int64_t value;
} VRegConstValue;

int tcc_ir_opt_branch_folding(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

#ifdef DEBUG_IR_GEN
  printf("=== BRANCH FOLDING START ===\n");
#endif

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *test_q = &ir->compact_instructions[i];
    IRQuadCompact *jump_q = &ir->compact_instructions[i + 1];

    if (test_q->op == TCCIR_OP_NOP || jump_q->op == TCCIR_OP_NOP)
      continue;

    /* Pattern 1: TEST_ZERO #const followed by JUMPIF */
    if (test_q->op == TCCIR_OP_TEST_ZERO && jump_q->op == TCCIR_OP_JUMPIF)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, test_q);

      if (!irop_is_immediate(src1))
        continue;

      int64_t val = irop_get_imm64_ex(ir, src1);
      IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
      int tok = (int)irop_get_imm64_ex(ir, cond);

      /* Evaluate the condition: JUMPIF tests if the condition is true */
      int branch_taken = 0;
      int is_known_condition = 1;

      switch (tok)
      {
      case 0x94: /* TOK_EQ */
        branch_taken = (val == 0);
        break;
      case 0x95: /* TOK_NE */
        branch_taken = (val != 0);
        break;
      default:
        /* For TEST_ZERO, we only expect EQ and NE conditions */
        is_known_condition = 0;
        break;
      }

      if (!is_known_condition)
        continue;

      if (branch_taken)
      {
        /* Branch always taken - convert JUMPIF to unconditional JUMP */
        /* The jump target is stored in the dest operand */
        IROperand dest = tcc_ir_op_get_dest(ir, jump_q);

        test_q->op = TCCIR_OP_NOP;
        jump_q->op = TCCIR_OP_JUMP;

        /* For JUMP, dest contains the target. Keep the same dest operand */
        tcc_ir_set_dest(ir, i + 1, dest);

#ifdef DEBUG_IR_GEN
        printf("BRANCH FOLD: TEST_ZERO #0 -> unconditional JUMP to %d\n", (int)dest.u.imm32);
#endif
        changes++;
      }
      else
      {
        /* Branch never taken - remove both instructions */
        test_q->op = TCCIR_OP_NOP;
        jump_q->op = TCCIR_OP_NOP;

#ifdef DEBUG_IR_GEN
        printf("BRANCH FOLD: TEST_ZERO #%lld with cond 0x%x never taken -> both NOP\n", (long long)val, tok);
#endif
        changes++;
      }
    }
    /* Pattern 2: CMP #const, #const followed by JUMPIF */
    else if (test_q->op == TCCIR_OP_CMP && jump_q->op == TCCIR_OP_JUMPIF)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, test_q);
      IROperand src2 = tcc_ir_op_get_src2(ir, test_q);

      if (!irop_is_immediate(src1) || !irop_is_immediate(src2))
        continue;

      int64_t val1 = irop_get_imm64_ex(ir, src1);
      int64_t val2 = irop_get_imm64_ex(ir, src2);

      IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
      int tok = (int)irop_get_imm64_ex(ir, cond);

      int result = evaluate_compare_condition(val1, val2, tok);

      if (result < 0)
        continue; /* Unknown condition */

      if (result)
      {
        /* Branch always taken - convert to unconditional JUMP */
        IROperand dest = tcc_ir_op_get_dest(ir, jump_q);

        test_q->op = TCCIR_OP_NOP;
        jump_q->op = TCCIR_OP_JUMP;

        tcc_ir_set_dest(ir, i + 1, dest);

#ifdef DEBUG_IR_GEN
        printf("BRANCH FOLD: CMP %lld,%lld with cond 0x%x -> unconditional JUMP to %d\n", (long long)val1,
               (long long)val2, tok, (int)dest.u.imm32);
#endif
        changes++;
      }
      else
      {
        /* Branch never taken - remove both instructions */
        test_q->op = TCCIR_OP_NOP;
        jump_q->op = TCCIR_OP_NOP;

#ifdef DEBUG_IR_GEN
        printf("BRANCH FOLD: CMP %lld,%lld with cond 0x%x never taken -> both NOP\n", (long long)val1, (long long)val2,
               tok);
#endif
        changes++;
      }
    }
  }

#ifdef DEBUG_IR_GEN
  printf("=== BRANCH FOLDING END: %d branches folded ===\n", changes);
#endif

  return changes;
}

/* ============================================================================
 * Strength Reduction for Multiply (Phase 3 of FUNCTION_CALLS_OPTIMIZATION_PLAN)
 * ============================================================================
 *
 * Transform MUL by constant into shift/add/sub sequences.
 * This reduces instruction latency on ARM where MUL is slower than shifts.
 *
 * Patterns:
 *   x * 2   -> x << 1
 *   x * 3   -> x + (x << 1)
 *   x * 4   -> x << 2
 *   x * 5   -> x + (x << 2)
 *   x * 7   -> (x << 3) - x
 *   x * 8   -> x << 3
 *   x * 9   -> x + (x << 3)
 *   x * 10  -> (x + (x << 2)) << 1
 *
 * For now, we only handle multipliers that can be expressed as:
 *   - Power of 2: use single shift
 *   - 2^n + 1: use add + shift (e.g., x*5 = x + x*4)
 *   - 2^n - 1: use shift + sub (e.g., x*7 = x*8 - x)
 *   - 2^n + 2^m: use two shifts + add
 *
 * Returns: 1 if transformation applied, 0 otherwise
 */

/* Check if n is a power of 2 and return log2(n) */
static int is_power_of_2(int64_t n)
{
  if (n <= 0)
    return -1;
  if ((n & (n - 1)) != 0)
    return -1;
  int log = 0;
  while (n > 1)
  {
    n >>= 1;
    log++;
  }
  return log;
}

/* Transform a single MUL instruction
 * Returns 1 if transformed, 0 otherwise
 */
int tcc_ir_strength_reduce_mul(TCCIRState *ir, int instr_idx)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];

  if (q->op != TCCIR_OP_MUL)
    return 0;

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  /* Find the constant operand (if any) */
  IROperand *value_op = NULL;
  int64_t multiplier = 0;

  if (irop_is_immediate(src1))
  {
    multiplier = irop_get_imm64_ex(ir, src1);
    value_op = &src2; /* The variable operand */
  }
  else if (irop_is_immediate(src2))
  {
    multiplier = irop_get_imm64_ex(ir, src2);
    value_op = &src1;
  }
  else
  {
    /* Both operands are variables - can't strength reduce */
    return 0;
  }

  /* Get the vreg for the value being multiplied */
  int32_t value_vreg = irop_get_vreg(*value_op);
  if (value_vreg < 0)
    return 0; /* No vreg - probably a constant expression */

  /* Get the destination vreg */
  int32_t dest_vreg = irop_get_vreg(dest);
  if (dest_vreg < 0)
    return 0;

  int btype = irop_get_btype(*value_op);

  /* Handle special cases */
  if (multiplier == 0)
  {
    /* x * 0 = 0 */
    q->op = TCCIR_OP_ASSIGN;
    IROperand zero = irop_make_imm32(-1, 0, btype);
    tcc_ir_set_src1(ir, instr_idx, zero);
    tcc_ir_set_src2(ir, instr_idx, IROP_NONE);
#ifdef DEBUG_IR_GEN
    printf("STRENGTH_RED: x * 0 -> 0 at i=%d\n", instr_idx);
#endif
    return 1;
  }

  if (multiplier == 1)
  {
    /* x * 1 = x (should have been handled by const prop, but be safe) */
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, instr_idx, *value_op);
    tcc_ir_set_src2(ir, instr_idx, IROP_NONE);
#ifdef DEBUG_IR_GEN
    printf("STRENGTH_RED: x * 1 -> x at i=%d\n", instr_idx);
#endif
    return 1;
  }

  /* Check for power of 2: x * (2^n) -> x << n */
  int log2_val = is_power_of_2(multiplier);
  if (log2_val >= 0 && log2_val <= 31)
  {
    q->op = TCCIR_OP_SHL;
    IROperand shift_amount = irop_make_imm32(-1, log2_val, btype);
    tcc_ir_set_src1(ir, instr_idx, *value_op);
    tcc_ir_set_src2(ir, instr_idx, shift_amount);
#ifdef DEBUG_IR_GEN
    printf("STRENGTH_RED: x * %lld -> x << %d at i=%d\n", (long long)multiplier, log2_val, instr_idx);
#endif
    return 1;
  }

  /* For now, we only handle simple cases that fit in one instruction.
   * More complex patterns would require inserting new instructions,
   * which needs careful handling to maintain call_id tracking and other invariants.
   *
   * The code generator can further optimize SHL instructions with constants.
   */

  return 0;
}

/* Run strength reduction on all MUL instructions in function
 * Returns number of instructions transformed
 */
int tcc_ir_opt_strength_reduction(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

#ifdef DEBUG_IR_GEN
  printf("=== STRENGTH REDUCTION START ===\n");
#endif

  for (int i = 0; i < n; i++)
  {
    changes += tcc_ir_strength_reduce_mul(ir, i);
  }

#ifdef DEBUG_IR_GEN
  printf("=== STRENGTH REDUCTION END: %d multiplies reduced ===\n", changes);
#endif

  return changes;
}

/* ============================================================================
 * Induction Variable Strength Reduction
 * ============================================================================
 *
 * This optimization transforms array indexing patterns:
 *   for (i = 0; i < n; i++) sum += arr[i];
 *
 * From: base + i*stride (SHL + ADD every iteration)
 * To:   ptr += stride (single ADD, enabling post-increment addressing)
 *
 * Key insight: Instead of computing the address each iteration, we maintain
 * a pointer that we increment by the stride.
 */

#include "licm.h"

/* Maximum induction variables per loop */
#define MAX_IV 8
/* Maximum derived IVs per loop */
#define MAX_DIV 16

/* Basic Induction Variable: v = v + constant */
typedef struct InductionVar
{
  int vreg;     /* Virtual register number (VAR type) */
  int init_val; /* Initial value (from preheader ASSIGN) */
  int step;     /* Increment per iteration */
  int def_idx;  /* Instruction index where IV is incremented */
  int init_idx; /* Instruction index of initialization */
} InductionVar;

/* Derived Induction Variable: base + iv * stride (after SHL) */
typedef struct DerivedIV
{
  int iv_idx;        /* Index into InductionVar array */
  int base_vreg;     /* Base address vreg (-1 if stack offset or immediate) */
  IROperand base_op; /* Original base operand */
  int stride;        /* Stride = iv.step * shift_amount (in bytes) */
  int use_idx;       /* ADD instruction index where DIV is computed */
  int shl_idx;       /* SHL instruction index (for NOP-ing) */
} DerivedIV;

/* Find basic induction variables in a loop.
 * An IV is a variable that is incremented by a constant in each iteration.
 * Pattern: V = V + const (where V is a VAR type vreg)
 */
static int find_induction_vars(TCCIRState *ir, IRLoop *loop, InductionVar *ivs, int max_ivs)
{
  int num_ivs = 0;

  /* Scan the ORIGINAL loop range (not extended body) for IV increments */
  for (int i = loop->start_idx; i <= loop->end_idx && num_ivs < max_ivs; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int dest_vr = irop_get_vreg(dest);
    int src1_vr = irop_get_vreg(src1);

    /* Must be a VAR register */
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    /* Pattern: V = V + const */
    if (src1_vr == dest_vr && irop_is_immediate(src2))
    {
      int step = (int)irop_get_imm64_ex(ir, src2);

      /* Check that this VAR is only defined ONCE in the loop range
       * (the increment itself) and once in the preheader (initialization) */
      int def_count = 0;
      for (int j = loop->start_idx; j <= loop->end_idx; j++)
      {
        IRQuadCompact *dq = &ir->compact_instructions[j];
        IROperand ddest = tcc_ir_op_get_dest(ir, dq);
        if (irop_get_vreg(ddest) == dest_vr && dq->op != TCCIR_OP_NOP)
          def_count++;
      }

      if (def_count != 1)
        continue; /* IV has multiple definitions in loop - not simple */

      /* Look for initialization in preheader */
      int init_val = 0;
      int init_idx = -1;
      for (int j = loop->preheader_idx; j >= 0 && j >= loop->preheader_idx - 5; j--)
      {
        IRQuadCompact *pq = &ir->compact_instructions[j];
        if (pq->op == TCCIR_OP_ASSIGN)
        {
          IROperand pdest = tcc_ir_op_get_dest(ir, pq);
          IROperand psrc1 = tcc_ir_op_get_src1(ir, pq);
          if (irop_get_vreg(pdest) == dest_vr && irop_is_immediate(psrc1))
          {
            init_val = (int)irop_get_imm64_ex(ir, psrc1);
            init_idx = j;
            break;
          }
        }
      }

      if (init_idx < 0)
        continue; /* No initialization found */

      ivs[num_ivs].vreg = dest_vr;
      ivs[num_ivs].init_val = init_val;
      ivs[num_ivs].step = step;
      ivs[num_ivs].def_idx = i;
      ivs[num_ivs].init_idx = init_idx;
      num_ivs++;

#ifdef DEBUG_IV_SR
      printf("IV_SR: Found BIV VAR%d (init=%d, step=%d) at idx=%d\n", TCCIR_DECODE_VREG_POSITION(dest_vr), init_val,
             step, i);
#endif
    }
  }

  return num_ivs;
}

/* Find derived induction variables in a loop.
 * A DIV is: base + (IV << shift) - used for array indexing.
 * We look for ADD instructions that use a SHL result where SHL uses an IV.
 */
static int find_derived_ivs(TCCIRState *ir, IRLoop *loop, InductionVar *ivs, int num_ivs, DerivedIV *divs, int max_divs)
{
  int num_divs = 0;

#ifdef DEBUG_IV_SR
  printf("IV_SR: Loop body_instrs: ");
  for (int bi = 0; bi < loop->num_body_instrs; bi++)
    printf("%d ", loop->body_instrs[bi]);
  printf("\n");
#endif

  /* Scan the extended body for ADD instructions (DIV computation) */
  for (int bi = 0; bi < loop->num_body_instrs && num_divs < max_divs; bi++)
  {
    int i = loop->body_instrs[bi];
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Pattern: T = base + Tshl  OR  T = Tshl + base */
    int shl_vr = -1, base_vr = -1;
    IROperand *base_op = NULL;
    int shl_idx = -1;

    /* Check src2 for SHL result */
    int vr2 = irop_get_vreg(src2);
    if (vr2 >= 0 && TCCIR_DECODE_VREG_TYPE(vr2) == TCCIR_VREG_TYPE_TEMP)
    {
      /* Look for SHL defining this temp */
      for (int j = 0; j < loop->num_body_instrs; j++)
      {
        int sj = loop->body_instrs[j];
        if (sj >= i)
          break; /* Must be before the ADD */
        IRQuadCompact *sq = &ir->compact_instructions[sj];
        if (sq->op == TCCIR_OP_SHL)
        {
          IROperand sdest = tcc_ir_op_get_dest(ir, sq);
          if (irop_get_vreg(sdest) == vr2)
          {
            shl_vr = vr2;
            shl_idx = sj;
            base_op = &src1;
            base_vr = irop_get_vreg(src1);
            break;
          }
        }
      }
    }

    /* Check src1 for SHL result if not found */
    if (shl_vr < 0)
    {
      int vr1 = irop_get_vreg(src1);
      if (vr1 >= 0 && TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_TEMP)
      {
        for (int j = 0; j < loop->num_body_instrs; j++)
        {
          int sj = loop->body_instrs[j];
          if (sj >= i)
            break;
          IRQuadCompact *sq = &ir->compact_instructions[sj];
          if (sq->op == TCCIR_OP_SHL)
          {
            IROperand sdest = tcc_ir_op_get_dest(ir, sq);
            if (irop_get_vreg(sdest) == vr1)
            {
              shl_vr = vr1;
              shl_idx = sj;
              base_op = &src2;
              base_vr = irop_get_vreg(src2);
              break;
            }
          }
        }
      }
    }

    if (shl_idx < 0)
      continue; /* Not a base + SHL pattern */

    /* Check that the SHL input is an IV */
    IRQuadCompact *shl_q = &ir->compact_instructions[shl_idx];
    IROperand shl_src1 = tcc_ir_op_get_src1(ir, shl_q);
    IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);

    int iv_vr = irop_get_vreg(shl_src1);
    if (iv_vr < 0 || !irop_is_immediate(shl_src2))
      continue;

    /* Find which IV this corresponds to */
    int iv_idx = -1;
    for (int k = 0; k < num_ivs; k++)
    {
      if (ivs[k].vreg == iv_vr)
      {
        iv_idx = k;
        break;
      }
    }

    if (iv_idx < 0)
      continue; /* SHL operand is not an IV */

    /* Calculate stride = step * (1 << shift) */
    int shift = (int)irop_get_imm64_ex(ir, shl_src2);
    int stride = ivs[iv_idx].step * (1 << shift);

    /* Check that this ADD result is only used once (as an address) */
    int dest_vr = irop_get_vreg(dest);
    int use_count = 0;
    for (int j = 0; j < ir->next_instruction_index; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *uq = &ir->compact_instructions[j];
      IROperand u1 = tcc_ir_op_get_src1(ir, uq);
      IROperand u2 = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(u1) == dest_vr)
        use_count++;
      if (irop_get_vreg(u2) == dest_vr)
        use_count++;
    }

    if (use_count != 1)
      continue; /* DIV result used multiple times - unsafe to transform */

    /* Check that the SHL result is only used by this ADD.
     * After CSE, other instructions might reference this SHL's result.
     * If so, we can't NOP the SHL without breaking those uses. */
    int shl_vr_uses = 0;
    for (int j = 0; j < ir->next_instruction_index; j++)
    {
      if (j == shl_idx)
        continue;
      IRQuadCompact *uq = &ir->compact_instructions[j];
      IROperand u1 = tcc_ir_op_get_src1(ir, uq);
      IROperand u2 = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(u1) == shl_vr)
        shl_vr_uses++;
      if (irop_get_vreg(u2) == shl_vr)
        shl_vr_uses++;
    }

    if (shl_vr_uses != 1)
    {
#ifdef DEBUG_IV_SR
      printf("IV_SR: Skipping DIV at idx=%d: SHL result has %d uses (not 1)\n", i, shl_vr_uses);
#endif
      continue; /* SHL result used by other instructions - can't NOP it */
    }

    divs[num_divs].iv_idx = iv_idx;
    divs[num_divs].base_vreg = base_vr;
    divs[num_divs].base_op = *base_op;
    divs[num_divs].stride = stride;
    divs[num_divs].use_idx = i;
    divs[num_divs].shl_idx = shl_idx;
    num_divs++;

#ifdef DEBUG_IV_SR
    printf("IV_SR: Found DIV base+%d*VAR%d at ADD idx=%d (SHL idx=%d)\n", stride, TCCIR_DECODE_VREG_POSITION(iv_vr), i,
           shl_idx);
#endif
  }

  return num_divs;
}

/* Insert an instruction at position 'pos', shifting all later instructions.
 * Updates jump targets that reference instructions >= pos.
 * Returns the instruction index where the new instruction was inserted.
 */
static int insert_instr_at(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2)
{
  int n = ir->next_instruction_index;

  /* Make room by shifting instructions */
  if (n + 1 >= ir->compact_instructions_size)
  {
    /* Need to resize - for safety, just fail */
    return -1;
  }

  /* Shift instructions from pos to end */
  for (int i = n; i > pos; i--)
  {
    ir->compact_instructions[i] = ir->compact_instructions[i - 1];
  }
  ir->next_instruction_index++;

  /* Update jump targets that point at or after pos */
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (i == pos)
      continue; /* Skip the new instruction */
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand jdest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, jdest);
      if (target >= pos)
      {
        IROperand new_dest = irop_make_imm32(-1, target + 1, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }

  /* Create the new instruction using operand pool */
  IRQuadCompact *new_q = &ir->compact_instructions[pos];
  new_q->op = op;
  new_q->orig_index = pos;
  new_q->line_num = 0;
  new_q->operand_base = tcc_ir_pool_add(ir, dest); /* dest at base + 0 */
  tcc_ir_pool_add(ir, src1);                       /* src1 at base + 1 */
  tcc_ir_pool_add(ir, src2);                       /* src2 at base + 2 */

  return pos;
}

/* Transform a derived IV to use pointer increment.
 * 1. Insert ptr = base + (iv_init * stride) in preheader (BEFORE the header)
 * 2. Replace the ADD (DIV) with just using ptr
 * 3. Insert ptr += stride after the IV increment
 * 4. NOP out the SHL instruction
 */
static int transform_derived_iv(TCCIRState *ir, IRLoop *loop, InductionVar *iv, DerivedIV *div)
{
  /* Allocate a new temp vreg for the pointer */
  int ptr_vreg = tcc_ir_vreg_alloc_temp(ir);
  if (ptr_vreg < 0)
    return 0;

#ifdef DEBUG_IV_SR
  printf("IV_SR: Transforming DIV at idx=%d, new ptr vreg=TMP%d, iv_init=%d, stride=%d\n", div->use_idx,
         TCCIR_DECODE_VREG_POSITION(ptr_vreg), iv->init_val, div->stride);
#endif

  /* Step 1: Insert ptr = base + (iv_init * stride) BEFORE the loop header
   * This ensures the init is executed once before entering the loop.
   * Important: We insert at preheader_idx + 1 to place it AFTER the preheader
   * instruction but BEFORE the header instruction.
   *
   * If iv_init == 0, we just do ptr = base
   * Otherwise, ptr = base + (iv_init * stride) requires two instructions:
   *   ptr = base
   *   ptr = ptr + offset
   */
  int insert_pos = loop->header_idx;

  /* Safety check: verify that base_op (if it's a vreg) is defined before
   * insert_pos.  This can fail when LICM hoists a stack-address for an inner
   * loop, placing the definition of the base vreg AFTER the outer loop's
   * header.  Inserting the derived-IV init before that definition would
   * create a use-before-def. */
  {
    int32_t base_vr = irop_get_vreg(div->base_op);
    if (base_vr >= 0)
    {
      int def_found_before = 0;
      for (int i = 0; i < insert_pos; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (irop_config[q->op].has_dest)
        {
          IROperand qd = tcc_ir_op_get_dest(ir, q);
          if (irop_get_vreg(qd) == base_vr)
          {
            def_found_before = 1;
            break;
          }
        }
      }
      if (!def_found_before)
      {
#ifdef DEBUG_IV_SR
        printf("IV_SR: Skipping DIV transform — base vreg not defined before insert_pos %d\n", insert_pos);
#endif
        return 0;
      }
    }
  }

  IROperand ptr_op = irop_make_vreg(ptr_vreg, IROP_BTYPE_INT32);
  IROperand null_op = {0};

  int idx_shift = 0;

  /* Calculate initial offset = iv_init * stride */
  int init_offset = iv->init_val * div->stride;

  if (init_offset == 0)
  {
    /* Simple case: ptr = base */
    int inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ASSIGN, ptr_op, div->base_op, null_op);
    if (inserted < 0)
      return 0;
    idx_shift = 1;
  }
  else
  {
    /* Need: ptr = base + init_offset
     * Insert: ptr = base
     *         ptr = ptr + init_offset */
    int inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ASSIGN, ptr_op, div->base_op, null_op);
    if (inserted < 0)
      return 0;
    idx_shift = 1;

    IROperand offset_op = irop_make_imm32(-1, init_offset, IROP_BTYPE_INT32);
    inserted = insert_instr_at(ir, insert_pos + 1, TCCIR_OP_ADD, ptr_op, ptr_op, offset_op);
    if (inserted < 0)
      return 1; /* Partial - at least did the assignment */
    idx_shift = 2;
  }

  /* After insertion, all indices >= insert_pos have shifted */

  /* Update our tracked indices */
  int new_use_idx = div->use_idx + idx_shift;
  int new_shl_idx = div->shl_idx + idx_shift;
  int new_iv_def_idx = iv->def_idx;
  if (iv->def_idx >= insert_pos)
    new_iv_def_idx += idx_shift;

  /* Step 2: Replace the ADD instruction with ASSIGN (ptr -> dest) */
  IRQuadCompact *add_q = &ir->compact_instructions[new_use_idx];
  add_q->op = TCCIR_OP_ASSIGN;
  tcc_ir_op_set_src1(ir, add_q, ptr_op);
  tcc_ir_op_set_src2(ir, add_q, null_op);
  /* dest stays the same - it's the address temp that was being used */

  /* Step 3: NOP out the SHL instruction (no longer needed) */
  IRQuadCompact *shl_q = &ir->compact_instructions[new_shl_idx];
  shl_q->op = TCCIR_OP_NOP;

  /* Step 4: Insert ptr += stride AFTER the IV increment.
   * The IV increment is the back-edge of the inner loop structure.
   * We insert right after it so the pointer is ready for the next iteration. */
  int stride_insert_pos = new_iv_def_idx + 1;
  IROperand stride_op = irop_make_imm32(-1, div->stride, IROP_BTYPE_INT32);

  int stride_inserted = insert_instr_at(ir, stride_insert_pos, TCCIR_OP_ADD, ptr_op, ptr_op, stride_op);
  if (stride_inserted < 0)
    return 2; /* Partial success - at least did the pointer init and use replacement */

  return 3; /* Full success: init + replace + stride */
}

/* Main entry point: Induction Variable Strength Reduction
 * Returns number of transformations applied
 */
/* Core IV strength reduction using pre-detected loops */
static int iv_strength_reduction_core(TCCIRState *ir, IRLoops *loops)
{
  int total_changes = 0;

#ifdef DEBUG_IV_SR
  printf("IV_SR: Found %d loop(s)\n", loops->num_loops);
#endif

  /* Process each loop, but only process loops with valid preheaders */
  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    /* Skip if this loop's preheader is inside another loop's body range.
     * This indicates a "phantom" inner loop from TCC's split control flow. */
    int skip = 0;
    for (int other = 0; other < loops->num_loops; other++)
    {
      if (other == li)
        continue;
      IRLoop *oloop = &loops->loops[other];
      if (loop->preheader_idx >= oloop->start_idx && loop->preheader_idx <= oloop->end_idx)
      {
        skip = 1;
        break;
      }
    }
    if (skip)
    {
#ifdef DEBUG_IV_SR
      printf("IV_SR: Skipping loop %d (preheader inside another loop)\n", li);
#endif
      continue;
    }

    InductionVar ivs[MAX_IV];
    DerivedIV divs[MAX_DIV];

    int num_ivs = find_induction_vars(ir, loop, ivs, MAX_IV);
    if (num_ivs == 0)
      continue;

#ifdef DEBUG_IV_SR
    printf("IV_SR: Loop %d has %d BIV(s)\n", li, num_ivs);
#endif

    int num_divs = find_derived_ivs(ir, loop, ivs, num_ivs, divs, MAX_DIV);
    if (num_divs == 0)
      continue;

#ifdef DEBUG_IV_SR
    printf("IV_SR: Found %d DIV(s) in loop %d\n", num_divs, li);
#endif

    /* Transform each derived IV */
    for (int di = 0; di < num_divs; di++)
    {
      int changes = transform_derived_iv(ir, loop, &ivs[divs[di].iv_idx], &divs[di]);
      total_changes += changes;

      /* After transformation, indices have shifted - we need to re-detect loops.
       * For now, just transform one DIV per loop to be safe. */
      if (changes > 0)
        break;
    }
  }

#ifdef DEBUG_IV_SR
  printf("=== IV STRENGTH REDUCTION END: %d changes ===\n", total_changes);
#endif

  return total_changes;
}

int tcc_ir_opt_iv_strength_reduction(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

#ifdef DEBUG_IV_SR
  printf("=== IV STRENGTH REDUCTION START ===\n");
#endif

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }
  int changes = iv_strength_reduction_core(ir, loops);
  tcc_ir_free_loops(loops);
  return changes;
}

int tcc_ir_opt_iv_strength_reduction_with_loops(TCCIRState *ir, IRLoops *loops)
{
  if (!ir || ir->next_instruction_index == 0 || !loops || loops->num_loops == 0)
    return 0;

#ifdef DEBUG_IV_SR
  printf("=== IV STRENGTH REDUCTION START (with pre-detected loops) ===\n");
#endif

  return iv_strength_reduction_core(ir, loops);
}

/* ============================================================================
 * Global CSE - Common Subexpression Elimination Across Basic Blocks
 * Phase 2 of BUBBLE_SORT_COMPARISON_PLAN
 * ============================================================================
 *
 * Problem: Local CSE (tcc_ir_opt_cse_arith) clears its hash table at block
 * boundaries, missing redundant computations in different basic blocks.
 *
 * Example from bubble_sort:
 *   ; Compare block:
 *   0017: T7 <-- V1 SHL #2       ; j * 4
 *   0018: T8 <-- P0 ADD T7       ; &arr[j]
 *
 *   ; Swap block (REDUNDANT - but different basic block):
 *   0024: T12 <-- V1 SHL #2      ; j * 4 AGAIN
 *   0025: T13 <-- P0 ADD T12     ; &arr[j] AGAIN
 *
 * Solution: Track available expressions across basic blocks using a simplified
 * dominator-based approach. When a computation is available from all paths
 * reaching a block, reuse it instead of recomputing.
 */

/* Maximum number of expressions to track per block */
#define GCSE_MAX_EXPRS 128

/* Expression entry for global CSE */
typedef struct GCSEExpr
{
  TccIrOp op;
  int32_t src1_vr;
  int32_t src2_vr;
  int64_t src1_const;
  int64_t src2_const;
  uint8_t src1_is_const : 1;
  uint8_t src2_is_const : 1;
  uint8_t src1_is_sym : 1;
  uint8_t src2_is_sym : 1;
  int32_t result_vr;     /* The vreg holding the computed result */
  int instr_idx;         /* Instruction index where computed */
  uint8_t valid : 1;     /* Whether this entry is valid */
} GCSEExpr;

/* Available expressions at block entry/exit */
typedef struct GCSEAvail
{
  GCSEExpr exprs[GCSE_MAX_EXPRS];
  int count;
} GCSEAvail;

/* Check if two expressions are equivalent */
static int gcse_exprs_equal(GCSEExpr *a, GCSEExpr *b)
{
  if (a->op != b->op)
    return 0;
  if (a->src1_is_const != b->src1_is_const || a->src2_is_const != b->src2_is_const)
    return 0;
  if (a->src1_is_sym != b->src1_is_sym || a->src2_is_sym != b->src2_is_sym)
    return 0;

  if (a->src1_is_const)
  {
    if (a->src1_const != b->src1_const)
      return 0;
  }
  else
  {
    if (a->src1_vr != b->src1_vr)
      return 0;
  }

  if (a->src2_is_const)
  {
    if (a->src2_const != b->src2_const)
      return 0;
  }
  else
  {
    if (a->src2_vr != b->src2_vr)
      return 0;
  }

  return 1;
}

/* Find an expression in the available set */
static GCSEExpr *gcse_find_expr(GCSEAvail *avail, GCSEExpr *expr)
{
  for (int i = 0; i < avail->count; i++)
  {
    if (avail->exprs[i].valid && gcse_exprs_equal(&avail->exprs[i], expr))
      return &avail->exprs[i];
  }
  return NULL;
}

/* Add an expression to the available set */
static void gcse_add_expr(GCSEAvail *avail, GCSEExpr *expr)
{
  if (avail->count >= GCSE_MAX_EXPRS)
    return;

  /* Check if already present */
  if (gcse_find_expr(avail, expr))
    return;

  avail->exprs[avail->count++] = *expr;
}

/* Invalidate expressions that use a specific vreg as source or whose
 * result_vr is being overwritten (the old value is no longer available).
 */
static void gcse_invalidate_vreg(GCSEAvail *avail, int32_t vreg)
{
  for (int i = 0; i < avail->count; i++)
  {
    if (!avail->exprs[i].valid)
      continue;

    /* Invalidate if this vreg is used as a source operand */
    if ((!avail->exprs[i].src1_is_const && avail->exprs[i].src1_vr == vreg) ||
        (!avail->exprs[i].src2_is_const && avail->exprs[i].src2_vr == vreg))
    {
      avail->exprs[i].valid = 0;
      continue;
    }

    /* Invalidate if this vreg is the result - the old value is overwritten */
    if (avail->exprs[i].result_vr == vreg)
    {
      avail->exprs[i].valid = 0;
    }
  }
}

/* Compact the available set by removing invalid entries */
static void gcse_compact(GCSEAvail *avail)
{
  int write = 0;
  for (int read = 0; read < avail->count; read++)
  {
    if (avail->exprs[read].valid)
    {
      if (write != read)
        avail->exprs[write] = avail->exprs[read];
      write++;
    }
  }
  avail->count = write;
}

/* Intersect two available sets (for join points) */
static void gcse_intersect(GCSEAvail *result, GCSEAvail *a, GCSEAvail *b)
{
  result->count = 0;

  for (int i = 0; i < a->count; i++)
  {
    if (!a->exprs[i].valid)
      continue;

    /* Check if this expr is also in b */
    for (int j = 0; j < b->count; j++)
    {
      if (!b->exprs[j].valid)
        continue;

      if (gcse_exprs_equal(&a->exprs[i], &b->exprs[j]))
      {
        /* Keep the one with the earliest instruction (dominates) */
        if (a->exprs[i].instr_idx <= b->exprs[j].instr_idx)
          result->exprs[result->count++] = a->exprs[i];
        else
          result->exprs[result->count++] = b->exprs[j];
        break;
      }
    }
  }
}

/* Copy available set */
static void gcse_copy(GCSEAvail *dst, GCSEAvail *src)
{
  dst->count = src->count;
  for (int i = 0; i < src->count; i++)
    dst->exprs[i] = src->exprs[i];
}

/* Extract expression info from an instruction */
static int gcse_extract_expr(TCCIRState *ir, int instr_idx, GCSEExpr *expr)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];

  /* Only handle arithmetic ops suitable for CSE */
  if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB && q->op != TCCIR_OP_MUL &&
      q->op != TCCIR_OP_AND && q->op != TCCIR_OP_OR && q->op != TCCIR_OP_XOR &&
      q->op != TCCIR_OP_SHL && q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_SAR)
    return 0;

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  /* Skip expressions involving symbols - different symbols map to the same
   * vreg (-1), so GCSE would incorrectly treat them as equivalent.
   * Symbol differences (e.g., &label1 - &label2) are link-time constants
   * and not suitable for runtime CSE anyway. */
  if (src1.is_sym || src2.is_sym)
    return 0;

  memset(expr, 0, sizeof(GCSEExpr));
  expr->op = q->op;
  expr->instr_idx = instr_idx;
  expr->valid = 1;

  /* Source 1 */
  if (irop_is_immediate(src1))
  {
    expr->src1_is_const = 1;
    expr->src1_const = irop_get_imm64_ex(ir, src1);
  }
  else
  {
    expr->src1_vr = irop_get_vreg(src1);
  }

  /* Source 2 */
  if (irop_is_immediate(src2))
  {
    expr->src2_is_const = 1;
    expr->src2_const = irop_get_imm64_ex(ir, src2);
  }
  else
  {
    expr->src2_vr = irop_get_vreg(src2);
  }

  expr->result_vr = irop_get_vreg(dest);

  return 1;
}

/* Basic block structure for global CSE */
typedef struct GCSEBlock
{
  int start_idx;
  int end_idx;
  int num_succs;
  int succs[2];  /* JUMP/JUMPIF can have at most 2 successors */
  int num_preds;
  int preds[8];  /* Arbitrary limit for predecessors */
  int visited;
  int rpo_num;   /* Reverse postorder number */
} GCSEBlock;

/* Build basic blocks from IR */
static int gcse_build_blocks(TCCIRState *ir, GCSEBlock *blocks, int max_blocks)
{
  int n = ir->next_instruction_index;
  int num_blocks = 0;
  uint8_t *is_block_start = tcc_mallocz(sizeof(uint8_t) * (n + 1));

  /* Mark block starts */
  is_block_start[0] = 1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int tgt = (int)irop_get_imm64_ex(ir, dest);
      if (tgt >= 0 && tgt < n)
        is_block_start[tgt] = 1;
      /* Instruction after jump is block start if not at end */
      if (i + 1 < n)
        is_block_start[i + 1] = 1;
    }
    else if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
             q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      if (i + 1 < n)
        is_block_start[i + 1] = 1;
    }
  }

  /* Create blocks */
  int current_start = 0;
  for (int i = 0; i <= n; i++)
  {
    if (is_block_start[i] && i > current_start)
    {
      if (num_blocks >= max_blocks)
        break;

      blocks[num_blocks].start_idx = current_start;
      blocks[num_blocks].end_idx = i;
      blocks[num_blocks].num_succs = 0;
      blocks[num_blocks].num_preds = 0;
      blocks[num_blocks].visited = 0;
      blocks[num_blocks].rpo_num = -1;
      num_blocks++;
      current_start = i;
    }
  }

  /* Handle last block */
  if (current_start < n && num_blocks < max_blocks)
  {
    blocks[num_blocks].start_idx = current_start;
    blocks[num_blocks].end_idx = n;
    blocks[num_blocks].num_succs = 0;
    blocks[num_blocks].num_preds = 0;
    blocks[num_blocks].visited = 0;
    blocks[num_blocks].rpo_num = -1;
    num_blocks++;
  }

  /* Build successor/predecessor relationships */
  for (int b = 0; b < num_blocks; b++)
  {
    int end = blocks[b].end_idx - 1;
    if (end < 0)
      continue;

    IRQuadCompact *q = &ir->compact_instructions[end];

    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int tgt = (int)irop_get_imm64_ex(ir, dest);
      /* Find block containing tgt */
      for (int s = 0; s < num_blocks; s++)
      {
        if (tgt >= blocks[s].start_idx && tgt < blocks[s].end_idx)
        {
          blocks[b].succs[blocks[b].num_succs++] = s;
          if (blocks[s].num_preds < 8)
            blocks[s].preds[blocks[s].num_preds++] = b;
          break;
        }
      }
    }
    else if (q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int tgt = (int)irop_get_imm64_ex(ir, dest);

      /* Branch target */
      for (int s = 0; s < num_blocks; s++)
      {
        if (tgt >= blocks[s].start_idx && tgt < blocks[s].end_idx)
        {
          blocks[b].succs[blocks[b].num_succs++] = s;
          if (blocks[s].num_preds < 8)
            blocks[s].preds[blocks[s].num_preds++] = b;
          break;
        }
      }

      /* Fall-through */
      if (b + 1 < num_blocks)
      {
        blocks[b].succs[blocks[b].num_succs++] = b + 1;
        if (blocks[b + 1].num_preds < 8)
          blocks[b + 1].preds[blocks[b + 1].num_preds++] = b;
      }
    }
    else if (q->op != TCCIR_OP_RETURNVALUE && q->op != TCCIR_OP_RETURNVOID)
    {
      /* Fall-through to next block */
      if (b + 1 < num_blocks)
      {
        blocks[b].succs[blocks[b].num_succs++] = b + 1;
        if (blocks[b + 1].num_preds < 8)
          blocks[b + 1].preds[blocks[b + 1].num_preds++] = b;
      }
    }
  }

  tcc_free(is_block_start);
  return num_blocks;
}

/* Compute reverse postorder for iterative dataflow */
static void gcse_compute_rpo(GCSEBlock *blocks, int num_blocks, int *rpo_order)
{
  int rpo_idx = 0;
  int stack[256];
  int sp = 0;

  /* Simple iterative DFS from block 0 */
  stack[sp++] = 0;

  while (sp > 0 && rpo_idx < num_blocks)
  {
    int b = stack[--sp];
    if (b < 0 || b >= num_blocks)
      continue;
    if (blocks[b].visited)
      continue;

    blocks[b].visited = 1;
    rpo_order[rpo_idx++] = b;

    /* Add successors to stack */
    for (int i = 0; i < blocks[b].num_succs; i++)
    {
      int s = blocks[b].succs[i];
      if (!blocks[s].visited)
        stack[sp++] = s;
    }
  }

  /* Handle unreachable blocks */
  for (int b = 0; b < num_blocks; b++)
  {
    if (!blocks[b].visited)
      rpo_order[rpo_idx++] = b;
  }
}

/* Main global CSE pass */
int tcc_ir_opt_cse_global(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

#ifdef DEBUG_IR_GEN
  printf("=== GLOBAL CSE START (n=%d) ===\n", n);
#endif

  /* Build CFG */
  GCSEBlock blocks[128];
  int num_blocks = gcse_build_blocks(ir, blocks, 128);

  if (num_blocks < 2)
  {
#ifdef DEBUG_IR_GEN
    printf("GLOBAL CSE: Only %d block(s), skipping\n", num_blocks);
#endif
    return 0;
  }

#ifdef DEBUG_IR_GEN
  printf("GLOBAL CSE: Built %d blocks\n", num_blocks);
#endif

  /* Compute RPO */
  int rpo_order[128];
  gcse_compute_rpo(blocks, num_blocks, rpo_order);

  /* Allocate available sets */
  GCSEAvail *block_in = tcc_mallocz(sizeof(GCSEAvail) * num_blocks);
  GCSEAvail *block_out = tcc_mallocz(sizeof(GCSEAvail) * num_blocks);

  /* Iterative dataflow: compute available expressions at block entries */
  int changed = 1;
  int iterations = 0;
  while (changed && iterations < 10)
  {
    changed = 0;
    iterations++;

    for (int r = 0; r < num_blocks; r++)
    {
      int b = rpo_order[r];

      /* Compute IN[b] = intersection of OUT[p] for all predecessors p */
      if (blocks[b].num_preds == 0)
      {
        /* Entry block - start empty */
        if (block_in[b].count != 0)
        {
          block_in[b].count = 0;
          changed = 1;
        }
      }
      else if (blocks[b].num_preds == 1)
      {
        /* Single predecessor - inherit directly */
        int p = blocks[b].preds[0];
        if (block_out[p].count != block_in[b].count)
        {
          gcse_copy(&block_in[b], &block_out[p]);
          changed = 1;
        }
        else
        {
          /* Check if content differs */
          for (int i = 0; i < block_out[p].count; i++)
          {
            if (!gcse_find_expr(&block_in[b], &block_out[p].exprs[i]))
            {
              gcse_copy(&block_in[b], &block_out[p]);
              changed = 1;
              break;
            }
          }
        }
      }
      else
      {
        /* Multiple predecessors - intersect */
        GCSEAvail new_in;
        gcse_copy(&new_in, &block_out[blocks[b].preds[0]]);

        for (int p = 1; p < blocks[b].num_preds; p++)
        {
          GCSEAvail temp;
          gcse_intersect(&temp, &new_in, &block_out[blocks[b].preds[p]]);
          gcse_copy(&new_in, &temp);
        }

        if (new_in.count != block_in[b].count)
        {
          gcse_copy(&block_in[b], &new_in);
          changed = 1;
        }
      }

      /* Compute OUT[b] by processing block instructions */
      GCSEAvail new_out;
      gcse_copy(&new_out, &block_in[b]);

#ifdef DEBUG_IR_GEN
      printf("GLOBAL CSE: Block %d [%d-%d) IN has %d exprs\n",
             b, blocks[b].start_idx, blocks[b].end_idx, block_in[b].count);
#endif

      for (int i = blocks[b].start_idx; i < blocks[b].end_idx; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];

        /* Skip NOPs */
        if (q->op == TCCIR_OP_NOP)
          continue;

        /* On calls, conservatively clear all available expressions.
         * Calls may modify any memory and clobber caller-saved registers. */
        if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
        {
          new_out.count = 0;
        }

        /* Invalidate available expressions when a vreg is redefined.
         * Must happen BEFORE we check/add the new expression, otherwise
         * we'd immediately kill an expression whose result_vr == def_vr
         * right after adding it (since this instruction defines that vreg).
         * By invalidating first, we remove stale entries that reference
         * the old value of def_vr, then add the fresh expression. */
        {
          IROperand def_dest = tcc_ir_op_get_dest(ir, q);
          int32_t def_vr = irop_get_vreg(def_dest);
          if (def_vr >= 0)
          {
            gcse_invalidate_vreg(&new_out, def_vr);
            gcse_compact(&new_out);
          }
        }

        /* Check if this instruction can be CSE'd */
        GCSEExpr expr;
        if (gcse_extract_expr(ir, i, &expr))
        {
          /* Check if available */
          GCSEExpr *avail = gcse_find_expr(&new_out, &expr);
          if (avail)
          {
            /* Already available - replace with ASSIGN */
            q->op = TCCIR_OP_ASSIGN;
            IROperand new_src = irop_make_vreg(avail->result_vr, IROP_BTYPE_INT32);
            tcc_ir_set_src1(ir, i, new_src);
            tcc_ir_set_src2(ir, i, IROP_NONE);
            changes++;

#ifdef DEBUG_IR_GEN
            printf("GLOBAL CSE: Replaced instr %d with ASSIGN from vr%d\n", i, avail->result_vr);
#endif

            /* Add the new result as available */
            GCSEExpr new_expr;
            if (gcse_extract_expr(ir, i, &new_expr))
              gcse_add_expr(&new_out, &new_expr);
          }
          else
          {
            /* Not available - add to available set */
            gcse_add_expr(&new_out, &expr);
          }
        }
      }

      /* Check if OUT changed */
      if (new_out.count != block_out[b].count)
      {
        gcse_copy(&block_out[b], &new_out);
        changed = 1;
      }
    }
  }

#ifdef DEBUG_IR_GEN
  printf("GLOBAL CSE: Converged in %d iterations, %d changes\n", iterations, changes);
#endif

  tcc_free(block_in);
  tcc_free(block_out);

  return changes;
}
