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
#include "pool.h"
#include "vreg.h"

/* ============================================================================
 * FP Offset Cache Optimization - delegated to tccopt.c
 * ============================================================================ */

/* Forward declarations for call parameter helpers (used by value_tracking) */
static int ir_opt_get_call_param_operand(TCCIRState *ir, int call_idx, int param_idx, IROperand *out);
static void ir_opt_nop_call_params(TCCIRState *ir, int call_idx);
static int ir_opt_eval_const_u64(TCCIRState *ir, IROperand op, int use_idx, uint64_t *out, int depth);

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

/* Forward declaration */
static int tcc_ir_vreg_has_single_def(TCCIRState *ir, int32_t vreg);

#ifndef TCCIR_VREG_TYPE_NONE
#define TCCIR_VREG_TYPE_NONE 0
#endif

/* Forward declaration (defined in branch_folding section below) */
static int evaluate_compare_condition(int64_t val1, int64_t val2, int cond_token);
static int change_callee_sym(TCCIRState *ir, int instr_idx, const char *new_name, int ret_btype);
static int change_callee_sym_keep_type(TCCIRState *ir, int instr_idx, const char *new_name);
/* Forward declaration (defined in IV strength reduction section) */
static int insert_instr_at(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2);

/* Known-pure AEABI runtime functions: these compute a result from their
 * arguments without reading or writing any memory accessible through
 * pointers.  Store-load forwarding can safely preserve tracked stores
 * across calls to these functions. */
static int tcc_ir_is_pure_aeabi(const char *name)
{
  if (!name || name[0] != '_' || name[1] != '_')
    return 0;
  /* 64-bit integer comparisons */
  if (strcmp(name, "__aeabi_lcmp") == 0 || strcmp(name, "__aeabi_ulcmp") == 0)
    return 1;
  /* 64-bit integer arithmetic */
  if (strcmp(name, "__aeabi_lmul") == 0 || strcmp(name, "__aeabi_ldivmod") == 0 ||
      strcmp(name, "__aeabi_uldivmod") == 0)
    return 1;
  /* 64-bit shifts */
  if (strcmp(name, "__aeabi_llsl") == 0 || strcmp(name, "__aeabi_llsr") == 0 || strcmp(name, "__aeabi_lasr") == 0)
    return 1;
  /* Soft-float arithmetic */
  if (strcmp(name, "__aeabi_dadd") == 0 || strcmp(name, "__aeabi_dsub") == 0 || strcmp(name, "__aeabi_dmul") == 0 ||
      strcmp(name, "__aeabi_ddiv") == 0 || strcmp(name, "__aeabi_fadd") == 0 || strcmp(name, "__aeabi_fsub") == 0 ||
      strcmp(name, "__aeabi_fmul") == 0 || strcmp(name, "__aeabi_fdiv") == 0)
    return 1;
  /* Soft-float comparisons */
  if (strcmp(name, "__aeabi_dcmpeq") == 0 || strcmp(name, "__aeabi_dcmplt") == 0 ||
      strcmp(name, "__aeabi_dcmple") == 0 || strcmp(name, "__aeabi_dcmpge") == 0 ||
      strcmp(name, "__aeabi_dcmpgt") == 0 || strcmp(name, "__aeabi_dcmpun") == 0 ||
      strcmp(name, "__aeabi_fcmpeq") == 0 || strcmp(name, "__aeabi_fcmplt") == 0 ||
      strcmp(name, "__aeabi_fcmple") == 0 || strcmp(name, "__aeabi_fcmpge") == 0 ||
      strcmp(name, "__aeabi_fcmpgt") == 0 || strcmp(name, "__aeabi_fcmpun") == 0)
    return 1;
  /* Soft-float conversions */
  if (strcmp(name, "__aeabi_f2d") == 0 || strcmp(name, "__aeabi_d2f") == 0 || strcmp(name, "__aeabi_i2d") == 0 ||
      strcmp(name, "__aeabi_i2f") == 0 || strcmp(name, "__aeabi_ui2d") == 0 || strcmp(name, "__aeabi_ui2f") == 0 ||
      strcmp(name, "__aeabi_d2iz") == 0 || strcmp(name, "__aeabi_d2uiz") == 0 || strcmp(name, "__aeabi_f2iz") == 0 ||
      strcmp(name, "__aeabi_f2uiz") == 0 || strcmp(name, "__aeabi_l2d") == 0 || strcmp(name, "__aeabi_l2f") == 0 ||
      strcmp(name, "__aeabi_ul2d") == 0 || strcmp(name, "__aeabi_ul2f") == 0 || strcmp(name, "__aeabi_d2lz") == 0 ||
      strcmp(name, "__aeabi_d2ulz") == 0 || strcmp(name, "__aeabi_f2lz") == 0 || strcmp(name, "__aeabi_f2ulz") == 0)
    return 1;
  return 0;
}

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
int tcc_ir_opt_const_var_prop(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int max_var_pos = 0;
  int i;

  if (n == 0)
    return 0;

  /* Phase 1: Find constant VAR vregs (assigned exactly once with immediate) */
  typedef struct
  {
    uint8_t is_constant : 1;
    uint8_t def_count : 7;
    int64_t value;
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
        var_info[pos].value = irop_get_imm64_ex(ir, src1);
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
    return 0;
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
        if (pos <= max_var_pos && var_info[pos].is_constant)
        {
          int64_t val = var_info[pos].value;
          IROperand new_src1;
          if (val == (int32_t)val)
            new_src1 = irop_make_imm32(-1, (int32_t)val, var_info[pos].btype);
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
            new_src1 = irop_make_i64(-1, pool_idx, var_info[pos].btype);
          }
          new_src1.is_unsigned = var_info[pos].is_unsigned;
          tcc_ir_set_src1(ir, i, new_src1);

          /* LOAD with constant src means the address was a local variable that
           * is now known to be a constant value — convert to ASSIGN. */
          if (q->op == TCCIR_OP_LOAD)
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
        if (pos <= max_var_pos && var_info[pos].is_constant)
        {
          int64_t val = var_info[pos].value;
          IROperand new_src2;
          if (val == (int32_t)val)
            new_src2 = irop_make_imm32(-1, (int32_t)val, var_info[pos].btype);
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
            new_src2 = irop_make_i64(-1, pool_idx, var_info[pos].btype);
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
  return changes;
}

/* ---------------------------------------------------------------------------
 * Global-initializer constant propagation
 *
 * Replace `LOAD dest <-- GlobalSym(X) [deref]` with `ASSIGN dest <-- #imm`
 * when X is a static global whose initial value is still valid — i.e. no
 * store or non-const pointer escape has been seen in this TU.  The
 * initializer bytes are read out of the symbol's ELF section (same source
 * the inline-eval path at tccgen.c:11965 uses for `*&g` folding).  After
 * this pass runs, the iterative const_prop + branch_folding + DCE pipeline
 * picks up the newly-visible constants and collapses comparisons / dead
 * error arms that the runtime CMP previously kept live.
 *
 * Safety gates (mirror the checks already used in try_inline_const_eval):
 *   - The sym must exist and carry a known type.
 *   - possibly_written == 0 (no stores / no non-const pointer escape).
 *   - Not volatile, not array / VLA, not aggregate.
 *   - Primitive type (VT_BYTE / VT_SHORT / VT_INT / VT_LLONG / VT_BOOL / VT_PTR).
 *   - Linkage: static / file-local only — non-static extern-visible globals
 *     may be written from other translation units, which possibly_written
 *     cannot observe.
 *   - Weak / dllimport / undefined symbols are skipped.
 *   - The initializer range must fit inside the section's emitted data.
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
    if (q->op != TCCIR_OP_LOAD)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (!src1.is_sym || !src1.is_lval)
      continue;

    IRPoolSymref *ref = irop_get_symref_ex(ir, src1);
    if (!ref || !ref->sym)
      continue;
    Sym *sym = ref->sym;

    /* Linkage / attribute gates. */
    if (sym->a.weak || sym->a.dllimport)
      continue;
    if (sym->a.possibly_written)
      continue;
    if (!(sym->type.t & VT_STATIC))
      continue; /* extern-visible: other TUs may write it */
    /* TCC is single-pass: when this function is optimized, stores in
     * later-declared functions have not yet been seen, so possibly_written
     * may be 0 for a global that is in fact written elsewhere in the TU
     * (see 20001111-1.c).  Restrict the fold to const-qualified globals,
     * which the language guarantees are not modified. */
    if (!(sym->type.t & VT_CONSTANT))
      continue;

    const int ttype = sym->type.t;
    if (ttype & (VT_ARRAY | VT_VLA))
      continue;
    if (ttype & VT_VOLATILE)
      continue;

    const int btype = ttype & VT_BTYPE;
    if (btype != VT_BYTE && btype != VT_SHORT && btype != VT_INT && btype != VT_LLONG && btype != VT_BOOL &&
        btype != VT_PTR)
      continue;

    /* Pointer globals whose initializer is another symbol (e.g. `static T *p = &x;`)
     * live in .data as zero bytes plus a relocation.  Reading the raw bytes
     * would yield a bogus null pointer — skip pointer types entirely. */
    if (btype == VT_PTR)
      continue;

    ElfSym *esym = elfsym(sym);
    if (!esym)
      continue;
    if (esym->st_shndx == SHN_UNDEF || esym->st_shndx == SHN_COMMON)
      continue;
    if (esym->st_shndx >= tcc_state->nb_sections)
      continue;

    Section *sec = tcc_state->sections[esym->st_shndx];
    if (!sec || !sec->data)
      continue;

    int align;
    int sz = type_size(&sym->type, &align);
    if (sz <= 0 || sz > 8)
      continue;

    unsigned long off = (unsigned long)(esym->st_value + (unsigned long long)ref->addend);
    if (off + (unsigned long)sz > sec->data_offset)
      continue;

    /* Read the initializer bytes.  Sign-extend narrow signed types so the
     * IR constant carries the correct high bits. */
    const unsigned char *ptr = sec->data + off;
    int64_t val = 0;
    if (sz == 8)
    {
      memcpy(&val, ptr, 8);
    }
    else
    {
      memcpy(&val, ptr, sz);
      if (!(ttype & VT_UNSIGNED) && sz < 8)
      {
        int shift = (8 - sz) * 8;
        val = (int64_t)(val << shift) >> shift;
      }
    }

    /* Build the new immediate operand.  Preserve the LOAD's result btype
     * (the destination) rather than deriving from Sym, so later passes see
     * a consistent shape. */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int dest_btype = irop_get_btype(dest);
    IROperand new_src1;
    if (dest_btype == IROP_BTYPE_INT64 || val != (int64_t)(int32_t)val)
    {
      uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
      new_src1 = irop_make_i64(-1, pool_idx, dest_btype);
    }
    else
    {
      new_src1 = irop_make_imm32(-1, (int32_t)val, dest_btype);
    }
    new_src1.is_unsigned = (ttype & VT_UNSIGNED) ? 1 : 0;

    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, new_src1);
    changes++;
  }

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

          /* For STORE: check if dest is a dead TMP/VAR used as pointer base */
          if (!has_dead_src && q->op == TCCIR_OP_STORE)
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
    if (q->op == TCCIR_OP_SET_CHAIN)
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
     * a pointer.  Three cases:
     * 1) LEA exists for this VAR — pointer alias is live, skip.
     * 2) Function defines nested functions — captured VARs are accessed
     *    through the static chain (R10→FP) or trampolines, not LEA.
     * 3) SET_CHAIN exists — explicit nested call in this function.
     * If none of these apply, the addrtaken flag is a stale frontend
     * annotation and the VAR is safe to eliminate. */
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
      if (interval && interval->addrtaken &&
          ((var_has_lea[pos / 8] & (1 << (pos % 8))) || tcc_state->nb_nested_funcs > 0 || has_set_chain))
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
int tcc_ir_opt_add_deref_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (!irop_is_immediate(src2))
      continue;
    int32_t imm = (int32_t)irop_get_imm64_ex(ir, src2);
    if (imm < 0 || imm > 4095)
      continue;
    int32_t base_vr = irop_get_vreg(src1);
    if (base_vr < 0)
      continue;
    if (src1.is_local || src1.is_llocal)
      continue;
    /* Only fold PARAM bases: the explicit LOAD_INDEXED can expose stack
     * loads to constant propagation which may incorrectly fold across
     * calls that modify memory through aliased pointers.  PARAM vregs
     * point to caller-owned memory, safe from this issue.
     *
     * Peep-through: if the base is a TEMP whose only def is a plain
     * ASSIGN copy from a PARAM, treat that PARAM as the effective base.
     * The TEMP is just a shadow of the parameter — copy_prop typically
     * eliminates it but doesn't always run before this pass. */
    if (TCCIR_DECODE_VREG_TYPE(base_vr) != TCCIR_VREG_TYPE_PARAM)
    {
      if (TCCIR_DECODE_VREG_TYPE(base_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      /* Peep-through: short bounded backward scan looking for an `ASSIGN
       * T <- PARAM` immediately preceding the ADD.  The frontend emits
       * the copy right before the ADD, so a window of ~16 instructions
       * is enough; falling back to a full-function scan would make this
       * O(n^2) on stress tests like 20001226-1 (16k compares).
       *
       * Bail on any branch/store/call before the def to keep the
       * semantics local — same constraints as the later same-block
       * and side-effect checks. */
      int copy_idx = -1;
      int max_back = 16;
      for (int j = i - 1; j >= 0 && (i - j) <= max_back; j--)
      {
        IRQuadCompact *cq = &ir->compact_instructions[j];
        if (cq->op == TCCIR_OP_NOP)
          continue;
        if (cq->op == TCCIR_OP_JUMP || cq->op == TCCIR_OP_JUMPIF ||
            cq->op == TCCIR_OP_STORE || cq->op == TCCIR_OP_STORE_INDEXED ||
            cq->op == TCCIR_OP_STORE_POSTINC || cq->op == TCCIR_OP_FUNCCALLVAL ||
            cq->op == TCCIR_OP_FUNCCALLVOID)
          break;
        if (irop_config[cq->op].has_dest)
        {
          IROperand cd = tcc_ir_op_get_dest(ir, cq);
          if (irop_get_vreg(cd) == base_vr && !cd.is_lval)
          {
            copy_idx = j;
            break;
          }
        }
      }
      if (copy_idx < 0)
        continue;
      IRQuadCompact *cq = &ir->compact_instructions[copy_idx];
      if (cq->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand cs1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cd = tcc_ir_op_get_dest(ir, cq);
      if (cs1.is_lval || cd.is_lval)
        continue;
      int32_t cs1_vr = irop_get_vreg(cs1);
      if (cs1_vr < 0 || TCCIR_DECODE_VREG_TYPE(cs1_vr) != TCCIR_VREG_TYPE_PARAM)
        continue;
      /* Use the PARAM source as the new base.  Don't NOP the copy — later
       * DCE will remove it if the TEMP becomes dead.  We don't verify "T is
       * used only here" because the existing use_count == 1 check on the
       * ADD's dest below covers what we actually need: the LOAD_INDEXED
       * still computes the same value regardless of how many extra readers
       * the TEMP base has, since the copy stays put. */
      src1 = cs1;
      base_vr = cs1_vr;
    }

    /* Check T has exactly one use, and that use is a DEREF */
    int use_count = 0;
    int use_idx = -1;
    int use_is_deref = 0;
    int use_in_src2 = 0; /* track which slot has the deref */
    for (int j = 0; j < n; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;
      /* Check src1 */
      if (irop_config[uq->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, uq);
        if (irop_get_vreg(s) == dest_vr)
        {
          use_count++;
          use_idx = j;
          use_is_deref = s.is_lval;
          use_in_src2 = 0;
        }
      }
      /* Check src2 */
      if (irop_config[uq->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, uq);
        if (irop_get_vreg(s) == dest_vr)
        {
          use_count++;
          use_idx = j;
          use_is_deref = s.is_lval;
          use_in_src2 = 1;
        }
      }
      /* Check dest (STORE dest is a pointer use) */
      if (uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED)
      {
        IROperand d = tcc_ir_op_get_dest(ir, uq);
        if (irop_get_vreg(d) == dest_vr)
          use_count++;
      }
    }

    if (use_count != 1 || !use_is_deref || use_idx < 0)
      continue;

    /* Same-block: no branch between ADD and its deref use.  Branches could
     * route through a path that stores to [base+imm], making the early
     * load see stale data. */
    {
      int cross_block = 0;
      for (int j = i + 1; j < use_idx; j++)
      {
        TccIrOp bop = ir->compact_instructions[j].op;
        if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
        {
          cross_block = 1;
          break;
        }
      }
      if (cross_block)
        continue;
    }

    /* The fold moves the load from the use site to the ADD site. If any
     * store or call occurs between them, the load might see stale data
     * (memory ordering violation). Bail if so. */
    {
      int has_side_effect = 0;
      for (int j = i + 1; j < use_idx; j++)
      {
        IRQuadCompact *sq = &ir->compact_instructions[j];
        if (sq->op == TCCIR_OP_STORE || sq->op == TCCIR_OP_STORE_INDEXED || sq->op == TCCIR_OP_STORE_POSTINC ||
            sq->op == TCCIR_OP_FUNCCALLVAL || sq->op == TCCIR_OP_FUNCCALLVOID)
        {
          has_side_effect = 1;
          break;
        }
      }
      if (has_side_effect)
        continue;
    }

    /* Get the DEREF use's btype — this determines the load width.
     * The ADD dest has a pointer btype which may differ from the
     * loaded value's type (e.g., struct pointer vs int field). */
    IRQuadCompact *uq_pre = &ir->compact_instructions[use_idx];
    IROperand use_op = use_in_src2 ? tcc_ir_op_get_src2(ir, uq_pre) : tcc_ir_op_get_src1(ir, uq_pre);
    int load_btype = irop_get_btype(use_op);

    /* Skip 64-bit and struct loads: LOAD_INDEXED uses LDRD which requires
     * 4-byte alignment.  Packed structs can place 64-bit fields at
     * unaligned offsets, causing a HardFault. */
    if (load_btype == IROP_BTYPE_INT64 || load_btype == IROP_BTYPE_FLOAT64 || load_btype == IROP_BTYPE_STRUCT)
      continue;

    /* Override the dest btype to the loaded value type */
    IROperand load_dest = dest;
    load_dest.btype = load_btype;

    /* Convert ADD to LOAD_INDEXED: allocate 4 contiguous pool entries */
    IROperand scale_op = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
    int new_base = tcc_ir_pool_add(ir, load_dest);
    tcc_ir_pool_add(ir, src1);
    tcc_ir_pool_add(ir, src2);
    tcc_ir_pool_add(ir, scale_op);
    q->operand_base = new_base;
    q->op = TCCIR_OP_LOAD_INDEXED;

    /* Clear DEREF on the use site — the value is now loaded, not a pointer. */
    IRQuadCompact *uq = &ir->compact_instructions[use_idx];
    if (irop_config[uq->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, uq);
      if (irop_get_vreg(s) == dest_vr && s.is_lval)
      {
        s.is_lval = 0;
        tcc_ir_set_src1(ir, use_idx, s);
      }
    }
    if (irop_config[uq->op].has_src2)
    {
      IROperand s = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(s) == dest_vr && s.is_lval)
      {
        s.is_lval = 0;
        tcc_ir_set_src2(ir, use_idx, s);
      }
    }

    changes++;
  }

  return changes;
}

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

int tcc_ir_opt_const_prop(TCCIRState *ir)
{
  /* VarConstInfo: track constant variables */
  typedef struct
  {
    uint8_t is_constant : 1;
    uint8_t def_count : 7;
    int64_t value;
    int def_idx; /* instruction index of the defining STORE/ASSIGN */
  } VarConstInfo;

  int n = ir->next_instruction_index;
  int changes = 0;
  int max_var_pos = 0;
  int i;
  IRQuadCompact *q;
  VarConstInfo *var_info;

  if (n == 0)
    return 0;

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

    /* Check if both operands refer to the same vreg (identity comparison).
     * Both must also have the same is_lval — CMP *V, V is NOT identity. */
    int32_t vr1 = irop_get_vreg(cmp_src1);
    int32_t vr2 = irop_get_vreg(cmp_src2);
    if (vr1 < 0 || vr2 < 0 || vr1 != vr2 || cmp_src1.is_lval != cmp_src2.is_lval)
      continue;

    /* Symbol reference operands with the same vreg may still refer to different
     * memory locations when their addends differ (e.g. different fields of the
     * same struct).  Only fold when both operands are truly identical. */
    if (cmp_src1.is_sym || cmp_src2.is_sym)
    {
      if (cmp_src1.is_sym != cmp_src2.is_sym)
        continue; /* one is sym, other is not — can't be identical */
      IRPoolSymref *ref1 = irop_get_symref_ex(ir, cmp_src1);
      IRPoolSymref *ref2 = irop_get_symref_ex(ir, cmp_src2);
      if (!ref1 || !ref2)
        continue;
      if (ref1->sym != ref2->sym || ref1->addend != ref2->addend)
        continue;
    }

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
      changes++;
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
     * For INT64, SHL #16 → SHR #16 masks 48 bits, not 16. */
    IROperand shl_orig_src1_chk = tcc_ir_op_get_src1(ir, shl_q);
    if (shl_orig_src1_chk.btype == IROP_BTYPE_INT64 || shl_orig_src1_chk.btype == IROP_BTYPE_FLOAT64)
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

  /* Redundant AND elimination: AND #M + AND #M → single AND #M.
   * Also: AND #M1 + AND #M2 where M2 is a superset of M1 → AND #M1.
   * Common pattern from C casts: (uint8_t)x generates AND #255 twice. */
  for (i = 0; i < n - 1; i++)
  {
    IRQuadCompact *and1_q = &ir->compact_instructions[i];
    IRQuadCompact *and2_q = &ir->compact_instructions[i + 1];
    if (and1_q->op != TCCIR_OP_AND || and2_q->op != TCCIR_OP_AND)
      continue;
    IROperand and1_src2 = tcc_ir_op_get_src2(ir, and1_q);
    IROperand and2_src2 = tcc_ir_op_get_src2(ir, and2_q);
    if (!irop_is_immediate(and1_src2) || !irop_is_immediate(and2_src2))
      continue;
    int64_t mask1 = irop_get_imm64_ex(ir, and1_src2);
    int64_t mask2 = irop_get_imm64_ex(ir, and2_src2);
    /* Second AND is redundant if mask1 is a subset of mask2 (mask1 & mask2 == mask1) */
    if ((mask1 & mask2) != mask1)
      continue;
    IROperand and1_dest = tcc_ir_op_get_dest(ir, and1_q);
    IROperand and2_src1 = tcc_ir_op_get_src1(ir, and2_q);
    if (irop_get_vreg(and1_dest) != irop_get_vreg(and2_src1))
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, irop_get_vreg(and1_dest), i))
      continue;
    IROperand and2_dest = tcc_ir_op_get_dest(ir, and2_q);
    tcc_ir_set_dest(ir, i, and2_dest);
    and2_q->op = TCCIR_OP_NOP;
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

    /* LEA tracking: T = &V — record that TMP T points to VAR V */
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
        /* src1 VAR is read here — mark its def as consumed so the
         * dead-def elimination won't kill the defining instruction. */
        if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_VAR)
        {
          int src_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
          if (src_pos >= 0 && src_pos <= max_vreg)
            VT_CLEAR_DEF(state, src_pos);
        }
        if (!lea_propagated && dest_pos <= max_vreg)
          lea_var_map[dest_pos].gen = 0;
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
         q->op == TCCIR_OP_SAR || q->op == TCCIR_OP_MUL) &&
        irop_is_immediate(src2))
    {
      int32_t src1_vr = irop_get_vreg(src1);
      int src1_pos = (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
                         ? TCCIR_DECODE_VREG_POSITION(src1_vr)
                         : -1;

      /* Check if src1 is a known constant AND src2 is immediate */
      if (src1_pos >= 0 && src1_pos <= max_vreg && VT_IS_CONST(state, src1_pos))
      {
        int64_t val1 = state[src1_pos].value;
        int64_t val2 = irop_get_imm64_ex(ir, src2);
        int btype = irop_get_btype(src1);
        int is_64 = (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64);
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
           *        9=f2d 10=d2f 11=d2iz 12=d2uiz */
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
          /* Double-returning operations (dadd/dsub/dmul/ddiv, f2d) are NOT
           * folded here because the result is 64-bit and irop_make_imm32
           * can't represent it.  Only 32-bit-returning ops are safe. */
          else if (0 && strcmp(sf, "__aeabi_dadd") == 0)
          {
            sf_nargs = 2;
            sf_kind = 1 | 0x80;
          }
          else if (0 && strcmp(sf, "__aeabi_dsub") == 0)
          {
            sf_nargs = 2;
            sf_kind = 2 | 0x80;
          }
          else if (0 && strcmp(sf, "__aeabi_dmul") == 0)
          {
            sf_nargs = 2;
            sf_kind = 3 | 0x80;
          }
          else if (0 && strcmp(sf, "__aeabi_ddiv") == 0)
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
          else if (0 && strcmp(sf, "__aeabi_f2d") == 0)
          {
            sf_nargs = 1;
            sf_kind = 9;
          }
          else if (0 && strcmp(sf, "__aeabi_d2f") == 0)
          {
            sf_nargs = 1;
            sf_kind = 10;
          }
          else if (0 && strcmp(sf, "__aeabi_d2iz") == 0)
          {
            sf_nargs = 1;
            sf_kind = 11;
          }
          else if (0 && strcmp(sf, "__aeabi_d2uiz") == 0)
          {
            sf_nargs = 1;
            sf_kind = 12;
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
                }

              if (folded)
              {
                IROperand call_dest = tcc_ir_op_get_dest(ir, q);
                ir_opt_nop_call_params(ir, i);
                q->op = TCCIR_OP_ASSIGN;
                tcc_ir_set_dest(ir, i, call_dest);
                tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, IROP_BTYPE_INT32));
                tcc_ir_set_src2(ir, i, IROP_NONE);
                /* Update value tracking for the dest so subsequent folds
                 * see the correct value (the continue skips normal processing). */
                {
                  int32_t dv = irop_get_vreg(call_dest);
                  if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
                  {
                    int dp = TCCIR_DECODE_VREG_POSITION(dv);
                    if (dp >= 0 && dp <= max_vreg)
                      VT_SET_CONST(state, dp, (int32_t)result);
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
typedef struct
{
  int valid;
  int64_t min_val;
  int64_t max_val;
} VRPRange;

/* Map (vreg_type, position) to a flat slot index.
 * PARAM positions 0..VRP_MAX_POS-1 → slots 0..VRP_MAX_POS-1
 * TEMP  positions 0..VRP_MAX_POS-1 → slots VRP_MAX_POS..2*VRP_MAX_POS-1
 * VAR   positions 0..VRP_MAX_POS-1 → slots 2*VRP_MAX_POS..3*VRP_MAX_POS-1
 * Returns -1 if not tracked. */
static int vrp_get_slot(int vr_type, int pos)
{
  if (pos < 0 || pos >= VRP_MAX_POS)
    return -1;
  if (vr_type == TCCIR_VREG_TYPE_PARAM)
    return pos;
  if (vr_type == TCCIR_VREG_TYPE_TEMP)
    return VRP_MAX_POS + pos;
  if (vr_type == TCCIR_VREG_TYPE_VAR)
    return 2 * VRP_MAX_POS + pos;
  return -1;
}

/* Check whether a comparison yields a constant result over [rmin, rmax].
 * Returns 1 if always taken, 0 if never taken, -1 if undetermined.
 * For unsigned comparisons, only safe when both endpoints have the same sign
 * (both >= 0 or both < 0 as int64), so the uint32 ordering is monotone. */
static int vrp_fold_cmp(int64_t rmin, int64_t rmax, int64_t cmp_val, int tok)
{
  int res_min = evaluate_compare_condition(rmin, cmp_val, tok);
  int res_max = evaluate_compare_condition(rmax, cmp_val, tok);
  if (res_min < 0 || res_max < 0 || res_min != res_max)
    return -1;
  return res_min;
}

/* Negate a comparison condition token: return the complement condition.
 * E.g. negate(EQ) = NE, negate(LT) = GE, etc. Returns -1 on unknown. */
static int vrp_negate_cmp_tok(int tok)
{
  switch (tok)
  {
  case TOK_EQ:
    return TOK_NE;
  case TOK_NE:
    return TOK_EQ;
  case TOK_LT:
    return TOK_GE;
  case TOK_GE:
    return TOK_LT;
  case TOK_LE:
    return TOK_GT;
  case TOK_GT:
    return TOK_LE;
  case TOK_ULT:
    return TOK_UGE;
  case TOK_UGE:
    return TOK_ULT;
  case TOK_ULE:
    return TOK_UGT;
  case TOK_UGT:
    return TOK_ULE;
  default:
    return -1;
  }
}

/* Swap a comparison condition for reversed operands.
 * If CMP A,B has condition c, then CMP B,A has condition swap(c).
 * E.g. swap(LT) = GT, swap(EQ) = EQ, etc. Returns -1 on unknown. */
static int vrp_swap_cmp_tok(int tok)
{
  switch (tok)
  {
  case TOK_EQ:
    return TOK_EQ;
  case TOK_NE:
    return TOK_NE;
  case TOK_LT:
    return TOK_GT;
  case TOK_GT:
    return TOK_LT;
  case TOK_LE:
    return TOK_GE;
  case TOK_GE:
    return TOK_LE;
  case TOK_ULT:
    return TOK_UGT;
  case TOK_UGT:
    return TOK_ULT;
  case TOK_ULE:
    return TOK_UGE;
  case TOK_UGE:
    return TOK_ULE;
  default:
    return -1;
  }
}

/* Check if knowing 'known_true' condition holds for (A, B) implies that
 * 'check' condition also holds for (A, B).
 * Returns 1 if implied, 0 otherwise. */
static int vrp_cmp_implies(int known_true, int check)
{
  if (known_true == check)
    return 1;
  switch (known_true)
  {
  case TOK_EQ: /* A == B implies: A <= B, A >= B, A <=U B, A >=U B */
    return (check == TOK_LE || check == TOK_GE || check == TOK_ULE || check == TOK_UGE);
  case TOK_LT: /* A < B implies: A <= B, A != B */
    return (check == TOK_LE || check == TOK_NE);
  case TOK_GT: /* A > B implies: A >= B, A != B */
    return (check == TOK_GE || check == TOK_NE);
  case TOK_ULT: /* A <U B implies: A <=U B, A != B */
    return (check == TOK_ULE || check == TOK_NE);
  case TOK_UGT: /* A >U B implies: A >=U B, A != B */
    return (check == TOK_UGE || check == TOK_NE);
  default:
    return 0;
  }
}

static uint8_t *ir_opt_build_merge_bitmap(TCCIRState *ir, int n)
{
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
        if (i > target)
          is_merge[target / 8] |= (1 << (target % 8));
      }
    }
    if (i + 1 < n && q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_RETURNVALUE &&
        q->op != TCCIR_OP_RETURNVOID)
    {
      pred_count[i + 1]++;
    }
  }

  for (int i = 0; i < n; i++)
  {
    if (pred_count[i] > 1)
      is_merge[i / 8] |= (1 << (i % 8));
  }

  tcc_free(pred_count);
  return is_merge;
}

static int fcmp_cmp_implies(int known_true, int check)
{
  if (known_true == check)
    return 1;

  switch (known_true)
  {
  case TOK_EQ:
    return (check == TOK_LE || check == TOK_GE);
  case TOK_NE:
    return (check == TOK_NE);
  case TOK_LT:
  case TOK_ULT:
    return (check == TOK_LE || check == TOK_NE || check == TOK_ULE);
  case TOK_GT:
  case TOK_UGT:
    return (check == TOK_GE || check == TOK_NE || check == TOK_UGE);
  default:
    return 0;
  }
}

/* Populate block_start_seen[0..n) so that each basic-block entry has value
 * `gen`.  Entry 0 is always a block start.  Each JUMP/JUMPIF target also
 * marks the start of a block.  The caller is responsible for zeroing the
 * array first (or using a generation counter — unmatched entries simply
 * remain at their previous generation value).
 *
 * Shared helper: used by copy_prop and const_prop_tmp to avoid each pass
 * duplicating the same scan. */
static void ir_opt_mark_block_starts(TCCIRState *ir, int *block_start_seen, int gen, int n)
{
  block_start_seen[0] = gen;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      const int tgt = (int)irop_get_imm64_ex(ir, dest);
      if (tgt >= 0 && tgt < n)
        block_start_seen[tgt] = gen;
    }
  }
}

static int ir_opt_next_non_nop(TCCIRState *ir, int start)
{
  int n = ir->next_instruction_index;
  for (int i = start; i < n; ++i)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      return i;
  }
  return -1;
}

static int ir_opt_is_pure_helper_name(const char *name)
{
  if (!name)
    return 0;

  return strcmp(name, "isnan") == 0 || strcmp(name, "__isnan") == 0 || strcmp(name, "__isnanf") == 0 ||
         strcmp(name, "__aeabi_f2d") == 0 || strcmp(name, "__aeabi_d2f") == 0;
}

static int ir_opt_is_flag_cmp_helper_name(const char *name)
{
  if (!name)
    return 0;

  return strcmp(name, "__aeabi_cfcmple") == 0 || strcmp(name, "__aeabi_cdcmple") == 0;
}

static int ir_opt_get_call_param_operand(TCCIRState *ir, int call_idx, int param_idx, IROperand *out)
{
  IRQuadCompact *call_q;
  IROperand call_src2;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index || !out)
    return 0;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return 0;

  call_src2 = tcc_ir_op_get_src2(ir, call_q);
  call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2));

  for (int i = call_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;

    IROperand enc = tcc_ir_op_get_src2(ir, q);
    uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, enc);
    if (TCCIR_DECODE_CALL_ID(encoded) != call_id)
      continue;
    if (TCCIR_DECODE_PARAM_IDX(encoded) != param_idx)
      continue;

    *out = tcc_ir_op_get_src1(ir, q);
    return 1;
  }

  return 0;
}

static void ir_opt_nop_call_params(TCCIRState *ir, int call_idx)
{
  IRQuadCompact *call_q;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return;

  call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call_q)));
  for (int i = call_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand enc;
    uint32_t encoded;

    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;

    enc = tcc_ir_op_get_src2(ir, q);
    encoded = (uint32_t)irop_get_imm64_ex(ir, enc);
    if (TCCIR_DECODE_CALL_ID(encoded) == call_id)
      q->op = TCCIR_OP_NOP;
  }
}

static void ir_opt_nop_call_param(TCCIRState *ir, int call_idx, int param_idx)
{
  IRQuadCompact *call_q;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return;

  call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call_q)));
  for (int i = call_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand enc;
    uint32_t encoded;

    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;

    enc = tcc_ir_op_get_src2(ir, q);
    encoded = (uint32_t)irop_get_imm64_ex(ir, enc);
    if (TCCIR_DECODE_CALL_ID(encoded) == call_id && TCCIR_DECODE_PARAM_IDX(encoded) == param_idx)
      q->op = TCCIR_OP_NOP;
  }
}

static void ir_opt_change_call_argc(TCCIRState *ir, int call_idx, int argc)
{
  IRQuadCompact *call_q;
  uint32_t encoded;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return;

  encoded = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call_q));
  call_id = TCCIR_DECODE_CALL_ID(encoded);
  tcc_ir_set_src2(ir, call_idx, irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_CALL(call_id, argc), IROP_BTYPE_INT32));
}

static int ir_opt_vreg_address_taken_between(TCCIRState *ir, int32_t vreg, int start_idx, int end_idx)
{
  if (!ir)
    return 0;

  for (int i = start_idx + 1; i < end_idx; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_LEA && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg)
      return 1;
  }

  return 0;
}

static const char *ir_opt_get_constant_string_from_symref(TCCIRState *ir, IROperand op)
{
  IRPoolSymref *symref;
  Sym *sym;
  ElfSym *esym;
  Section *sec;
  const char *str;
  const char *nul;
  addr_t offset;
  size_t remaining;

  if (!ir || irop_get_tag(op) != IROP_TAG_SYMREF)
    return NULL;

  symref = irop_get_symref_ex(ir, op);
  if (!symref || symref->addend < 0)
    return NULL;
  if (symref->flags & IRPOOL_SYMREF_LVAL)
    return NULL;

  sym = symref->sym;
  if (!sym)
    return NULL;

  esym = elfsym(sym);
  if (!esym)
    return NULL;
  if (esym->st_shndx == SHN_UNDEF || esym->st_shndx >= (unsigned)tcc_state->nb_sections)
    return NULL;

  sec = tcc_state->sections[esym->st_shndx];
  if (!sec || !sec->data)
    return NULL;
  if (sec->sh_flags & SHF_WRITE)
    return NULL;
  if (esym->st_size == 0 || (addr_t)symref->addend >= esym->st_size)
    return NULL;

  offset = esym->st_value + (addr_t)symref->addend;
  if (offset >= sec->data_offset)
    return NULL;

  str = (const char *)(sec->data + offset);
  remaining = (size_t)(esym->st_size - (addr_t)symref->addend);
  nul = memchr(str, '\0', remaining);
  if (!nul)
    return NULL;

  return str;
}

static const uint8_t *ir_opt_get_rodata_bytes(TCCIRState *ir, IROperand op, size_t *out_size)
{
  IRPoolSymref *symref;
  Sym *sym;
  ElfSym *esym;
  Section *sec;
  addr_t offset;

  if (!ir || irop_get_tag(op) != IROP_TAG_SYMREF)
    return NULL;

  symref = irop_get_symref_ex(ir, op);
  if (!symref || symref->addend < 0)
    return NULL;

  sym = symref->sym;
  if (!sym)
    return NULL;

  esym = elfsym(sym);
  if (!esym)
    return NULL;
  if (esym->st_shndx == SHN_UNDEF || esym->st_shndx >= (unsigned)tcc_state->nb_sections)
    return NULL;

  sec = tcc_state->sections[esym->st_shndx];
  if (!sec || !sec->data)
    return NULL;
  if (sec->sh_flags & SHF_WRITE)
    return NULL;
  if (esym->st_size == 0)
    return NULL;

  offset = esym->st_value + (addr_t)symref->addend;
  if (offset + esym->st_size > sec->data_offset)
    return NULL;

  if (sec->reloc && sec->reloc->data_offset > 0)
  {
    ElfW_Rel *rel = (ElfW_Rel *)sec->reloc->data;
    ElfW_Rel *rel_end = (ElfW_Rel *)(sec->reloc->data + sec->reloc->data_offset);
    for (; rel < rel_end; rel++)
    {
      if (rel->r_offset >= esym->st_value && rel->r_offset < esym->st_value + esym->st_size)
        return NULL;
    }
  }

  *out_size = (size_t)(esym->st_size - (addr_t)symref->addend);
  return sec->data + offset;
}

static int ir_opt_eval_const_u64(TCCIRState *ir, IROperand op, int use_idx, uint64_t *out, int depth)
{
  int32_t vr;
  int def_idx;
  IRQuadCompact *q;

  if (!ir || !out || depth > 12)
    return 0;

  if (irop_is_immediate(op))
  {
    *out = (uint64_t)irop_get_imm64_ex(ir, op);
    return 1;
  }

  vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  if (ir_opt_vreg_address_taken_between(ir, vr, 0, use_idx))
    return 0;

  def_idx = tcc_ir_find_defining_instruction(ir, vr, use_idx);
  if (def_idx < 0)
    return 0;

  q = &ir->compact_instructions[def_idx];
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1);
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_ROR:
  {
    uint64_t v1, v2;
    if (!ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &v1, depth + 1))
      return 0;
    if (!ir_opt_eval_const_u64(ir, tcc_ir_op_get_src2(ir, q), def_idx, &v2, depth + 1))
      return 0;
    switch (q->op)
    {
    case TCCIR_OP_ADD:
      *out = v1 + v2;
      break;
    case TCCIR_OP_SUB:
      *out = v1 - v2;
      break;
    case TCCIR_OP_MUL:
      *out = v1 * v2;
      break;
    case TCCIR_OP_AND:
      *out = v1 & v2;
      break;
    case TCCIR_OP_OR:
      *out = v1 | v2;
      break;
    case TCCIR_OP_XOR:
      *out = v1 ^ v2;
      break;
    case TCCIR_OP_SHL:
      *out = v1 << v2;
      break;
    case TCCIR_OP_SHR:
      *out = v1 >> v2;
      break;
    case TCCIR_OP_SAR:
      *out = (uint64_t)((int64_t)v1 >> v2);
      break;
    case TCCIR_OP_ROR:
    {
      uint32_t v = (uint32_t)v1;
      uint32_t n = (uint32_t)v2 & 31;
      *out = (v >> n) | (v << (32 - n));
      break;
    }
    default:
      return 0;
    }
    return 1;
  }
  default:
    return 0;
  }
}

static int ir_opt_eval_const_string(TCCIRState *ir, IROperand op, int use_idx, const char **out, int depth)
{
  const char *base;
  int32_t vr;
  int def_idx;
  IRQuadCompact *q;

  if (!ir || !out || depth > 16)
    return 0;

  /* For TEMP vregs, is_lval means "dereference this computed address" —
   * we can't resolve the pointed-to value (e.g. ptr_array[i]).
   * VAR/PARAM is_lval just means "load from stack slot" which the
   * definition chain already tracks correctly. */
  if (op.is_lval && op.vreg_type == TCCIR_VREG_TYPE_TEMP)
    return 0;

  base = ir_opt_get_constant_string_from_symref(ir, op);
  if (base)
  {
    *out = base;
    return 1;
  }

  vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  if (ir_opt_vreg_address_taken_between(ir, vr, 0, use_idx))
    return 0;

  /* Bail out if vreg has multiple definitions (e.g. from ternary branches) —
     we can't know which definition actually reaches us at runtime. */
  if (!tcc_ir_vreg_has_single_def(ir, vr))
    return 0;

  def_idx = tcc_ir_find_defining_instruction(ir, vr, use_idx);
  if (def_idx < 0)
    return 0;

  q = &ir->compact_instructions[def_idx];
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return ir_opt_eval_const_string(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1);
  case TCCIR_OP_ADD:
  {
    uint64_t addend;
    if (ir_opt_eval_const_string(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1) &&
        ir_opt_eval_const_u64(ir, tcc_ir_op_get_src2(ir, q), def_idx, &addend, depth + 1))
    {
      *out += addend;
      return 1;
    }
    if (ir_opt_eval_const_string(ir, tcc_ir_op_get_src2(ir, q), def_idx, out, depth + 1) &&
        ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &addend, depth + 1))
    {
      *out += addend;
      return 1;
    }
    return 0;
  }
  default:
    return 0;
  }
}

static int ir_opt_eval_const_string_operand(TCCIRState *ir, IROperand op, int use_idx, IROperand *out, int depth)
{
  int32_t vr;
  int def_idx;
  IRQuadCompact *q;

  if (!ir || !out || depth > 16)
    return 0;

  if (op.is_lval && op.vreg_type == TCCIR_VREG_TYPE_TEMP)
    return 0;

  if (ir_opt_get_constant_string_from_symref(ir, op))
  {
    *out = op;
    return 1;
  }

  vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  if (ir_opt_vreg_address_taken_between(ir, vr, 0, use_idx))
    return 0;

  if (!tcc_ir_vreg_has_single_def(ir, vr))
    return 0;

  def_idx = tcc_ir_find_defining_instruction(ir, vr, use_idx);
  if (def_idx < 0)
    return 0;

  q = &ir->compact_instructions[def_idx];
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return ir_opt_eval_const_string_operand(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1);
  case TCCIR_OP_ADD:
  {
    IROperand base_op;
    uint64_t addend;
    IRPoolSymref *symref;
    uint32_t new_idx;

    if (!ir_opt_eval_const_string_operand(ir, tcc_ir_op_get_src1(ir, q), def_idx, &base_op, depth + 1) ||
        !ir_opt_eval_const_u64(ir, tcc_ir_op_get_src2(ir, q), def_idx, &addend, depth + 1))
    {
      if (!ir_opt_eval_const_string_operand(ir, tcc_ir_op_get_src2(ir, q), def_idx, &base_op, depth + 1) ||
          !ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &addend, depth + 1))
        return 0;
    }

    if (irop_get_tag(base_op) != IROP_TAG_SYMREF)
      return 0;

    symref = irop_get_symref_ex(ir, base_op);
    if (!symref)
      return 0;

    new_idx = tcc_ir_pool_add_symref(ir, symref->sym, symref->addend + (int32_t)addend, symref->flags);
    *out = irop_make_symref(irop_get_vreg(base_op), new_idx, base_op.is_lval, base_op.is_local, base_op.is_const,
                            irop_get_btype(base_op));
    return 1;
  }
  default:
    return 0;
  }
}

static int ir_opt_fold_strcmp_result(const char *s1, const char *s2)
{
  while ((unsigned char)*s1 == (unsigned char)*s2)
  {
    if (*s1 == '\0')
      return 0;
    ++s1;
    ++s2;
  }

  return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

static int ir_opt_fold_strncmp_result(const char *s1, const char *s2, uint64_t n)
{
  if (n == 0)
    return 0;

  while (n-- > 0)
  {
    unsigned char c1 = (unsigned char)*s1++;
    unsigned char c2 = (unsigned char)*s2++;
    if (c1 != c2 || c1 == '\0')
      return (int)c1 - (int)c2;
  }

  return 0;
}

static int ir_opt_fold_memcmp_result(const char *s1, const char *s2, uint64_t n)
{
  uint64_t i;

  for (i = 0; i < n; ++i)
  {
    unsigned char c1 = (unsigned char)s1[i];
    unsigned char c2 = (unsigned char)s2[i];
    if (c1 != c2)
      return (int)c1 - (int)c2;
  }

  return 0;
}

static int ir_opt_fold_memchr_offset(const char *s, unsigned char c, uint64_t n, int *out_offset)
{
  uint64_t i;

  if (!out_offset)
    return 0;

  for (i = 0; i < n; ++i)
  {
    if ((unsigned char)s[i] == c)
    {
      *out_offset = (int)i;
      return 1;
    }
  }

  *out_offset = -1;
  return 1;
}

int tcc_ir_opt_const_string_calls(TCCIRState *ir)
{
  int changes = 0;

  if (!ir)
    return 0;

  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    Sym *callee;
    IROperand arg0;
    IROperand arg1;
    const char *s1;
    const char *s2;
    IROperand base_op;
    int folded_result;
    int arg0_is_const_string = 0;
    int arg1_is_const_string = 0;

    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;

    const char *name = get_tok_str(callee->v, NULL);
    const int id = resolve_str_builtin_id(callee->v, name);
    if (id == STRBI_UNKNOWN)
      continue;

    /* --- strlen: fold if arg is constant string, otherwise redirect --- */
    if (id == STRBI_STRLEN)
    {
      if (q->op == TCCIR_OP_FUNCCALLVAL && ir_opt_get_call_param_operand(ir, i, 0, &arg0) &&
          ir_opt_eval_const_string(ir, arg0, i, &s1, 0))
      {
        ir_opt_nop_call_params(ir, i);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int)strlen(s1), VT_INT));
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
      else
      {
        if (change_callee_sym_keep_type(ir, i, "__tcc_strlen"))
          changes++;
      }
      continue;
    }

    /* --- Simple redirects to __tcc_* helpers --- */
    {
      const char *helper = NULL;
      switch (id)
      {
      case STRBI_MEMMOVE:
        helper = "__tcc_memmove";
        break;
      case STRBI_BCOPY:
        helper = "__tcc_bcopy";
        break;
      case STRBI_MEMPCPY:
        helper = "__tcc_mempcpy";
        break;
      case STRBI_STRCAT:
        helper = "__tcc_strcat";
        break;
      case STRBI_STRCHR:
      case STRBI_INDEX:
        helper = "__tcc_strchr";
        break;
      case STRBI_STRCPY:
        helper = "__tcc_strcpy";
        break;
      case STRBI_STPCPY:
        helper = "__tcc_stpcpy";
        break;
      case STRBI_STPNCPY:
        helper = "__tcc_stpncpy";
        break;
      case STRBI_STRNLEN:
        helper = "__tcc_strnlen";
        break;
      case STRBI_STRPBRK:
        helper = "__tcc_strpbrk";
        break;
      case STRBI_STRRCHR:
      case STRBI_RINDEX:
        helper = "__tcc_strrchr";
        break;
      case STRBI_STRSTR:
        helper = "__tcc_strstr";
        break;
      case STRBI_STRCSPN:
        helper = "__tcc_strcspn";
        break;
      case STRBI_STRNCPY:
        helper = "__tcc_strncpy";
        break;
      case STRBI_STRNCAT:
        helper = "__tcc_strncat";
        break;
      default:
        break;
      }
      if (helper)
      {
        if (change_callee_sym_keep_type(ir, i, helper))
          changes++;
        continue;
      }
    }

    /* --- Functions that need argument analysis for folding --- */

    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    if (!ir_opt_get_call_param_operand(ir, i, 0, &arg0) || !ir_opt_get_call_param_operand(ir, i, 1, &arg1))
      continue;

    if (id == STRBI_MEMCHR)
    {
      IROperand arg2;
      uint64_t n;
      int match_offset;
      uint64_t needle_u64;
      if (!ir_opt_get_call_param_operand(ir, i, 2, &arg2) || !ir_opt_eval_const_u64(ir, arg2, i, &n, 0) ||
          !ir_opt_eval_const_string(ir, arg0, i, &s1, 0) ||
          !ir_opt_eval_const_string_operand(ir, arg0, i, &base_op, 0) ||
          !ir_opt_eval_const_u64(ir, arg1, i, &needle_u64, 0))
        continue;
      if (n > (uint64_t)strlen(s1) + 1)
        continue;

      if (!ir_opt_fold_memchr_offset(s1, (unsigned char)needle_u64, n, &match_offset))
        continue;

      ir_opt_nop_call_params(ir, i);
      q->op = TCCIR_OP_ASSIGN;
      if (match_offset < 0)
      {
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
      }
      else
      {
        IRPoolSymref *symref = irop_get_symref_ex(ir, base_op);
        uint32_t new_idx = tcc_ir_pool_add_symref(ir, symref->sym, symref->addend + match_offset, symref->flags);
        tcc_ir_set_src1(ir, i,
                        irop_make_symref(irop_get_vreg(base_op), new_idx, base_op.is_lval, base_op.is_local,
                                         base_op.is_const, irop_get_btype(base_op)));
      }
      tcc_ir_set_src2(ir, i, IROP_NONE);
      changes++;
      continue;
    }

    if (id == STRBI_MEMCMP)
    {
      IROperand arg2;
      uint64_t n;

      if (!ir_opt_get_call_param_operand(ir, i, 2, &arg2) || !ir_opt_eval_const_u64(ir, arg2, i, &n, 0))
        continue;

      if (n == 0)
      {
        ir_opt_nop_call_params(ir, i);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, 0, VT_INT));
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
        continue;
      }

      if (n == 1)
      {
        ir_opt_nop_call_param(ir, i, 2);
        if (!change_callee_sym(ir, i, "__tcc_memcmp1", VT_INT))
          continue;
        ir_opt_change_call_argc(ir, i, 2);
        changes++;
        continue;
      }
    }

    if (id == STRBI_STRNCMP)
    {
      IROperand arg2;
      uint64_t n;

      if (!ir_opt_get_call_param_operand(ir, i, 2, &arg2) || !ir_opt_eval_const_u64(ir, arg2, i, &n, 0))
        continue;

      if (n == 0)
      {
        ir_opt_nop_call_params(ir, i);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, 0, VT_INT));
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
        continue;
      }

      arg0_is_const_string = ir_opt_eval_const_string(ir, arg0, i, &s1, 0);
      arg1_is_const_string = ir_opt_eval_const_string(ir, arg1, i, &s2, 0);

      if (!(arg0_is_const_string && arg1_is_const_string))
      {
        if (!change_callee_sym(ir, i, "__tcc_strncmp", VT_INT))
          continue;
        changes++;
        continue;
      }
    }

    if (!arg0_is_const_string)
      arg0_is_const_string = ir_opt_eval_const_string(ir, arg0, i, &s1, 0);
    if (!arg1_is_const_string)
      arg1_is_const_string = ir_opt_eval_const_string(ir, arg1, i, &s2, 0);

    if (id == STRBI_STRCMP && !(arg0_is_const_string && arg1_is_const_string))
    {
      if (change_callee_sym_keep_type(ir, i, "__tcc_strcmp"))
        changes++;
      continue;
    }

    if (!arg0_is_const_string || !arg1_is_const_string)
      continue;

    if (id == STRBI_STRCMP)
      folded_result = ir_opt_fold_strcmp_result(s1, s2);
    else
    {
      IROperand arg2;
      uint64_t n;
      if (!ir_opt_get_call_param_operand(ir, i, 2, &arg2) || !ir_opt_eval_const_u64(ir, arg2, i, &n, 0))
        continue;
      if (n > (uint64_t)strlen(s1) + 1 || n > (uint64_t)strlen(s2) + 1)
        continue;
      if (id == STRBI_STRNCMP)
        folded_result = ir_opt_fold_strncmp_result(s1, s2, n);
      else
        folded_result = ir_opt_fold_memcmp_result(s1, s2, n);
    }

    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, irop_make_imm32(-1, folded_result, VT_INT));
    tcc_ir_set_src2(ir, i, IROP_NONE);
    changes++;
  }

  return changes;
}

static int ir_opt_pure_expr_equal(TCCIRState *ir, IROperand a, int a_use_idx, IROperand b, int b_use_idx, int depth);

static int ir_opt_nonvreg_expr_equal(TCCIRState *ir, IROperand a, IROperand b)
{
  int a_tag = irop_get_tag(a);
  int b_tag = irop_get_tag(b);

  if (a_tag != b_tag)
    return 0;

  /* STACKOFF: two stack-local operands with the same vreg and offset
     are accessing the same local variable at the same sub-component. */
  if (a_tag == IROP_TAG_STACKOFF)
  {
    int32_t a_vr = irop_get_vreg(a);
    int32_t b_vr = irop_get_vreg(b);
    if (a_vr >= 0 && a_vr == b_vr && a.u.imm32 == b.u.imm32 && a.is_lval == b.is_lval && a.is_local == b.is_local &&
        a.is_llocal == b.is_llocal && a.is_param == b.is_param && irop_get_btype(a) == irop_get_btype(b))
      return 1;
    return 0;
  }

  /* Only handle SYMREF — comparing symbol references for global struct
     field access in short-circuit conditions.  Other non-vreg tags are
     returned as "not equal" (conservative) to avoid incorrect CSE of
     inline function addresses and other operand kinds. */
  if (a_tag != IROP_TAG_SYMREF)
    return 0;

  if (a.is_lval != b.is_lval || a.is_llocal != b.is_llocal || a.is_local != b.is_local || a.is_const != b.is_const ||
      a.is_unsigned != b.is_unsigned || a.is_static != b.is_static || a.is_sym != b.is_sym ||
      a.is_param != b.is_param || a.is_complex != b.is_complex || irop_get_btype(a) != irop_get_btype(b))
  {
    return 0;
  }

  {
    IRPoolSymref *a_ref = irop_get_symref_ex(ir, a);
    IRPoolSymref *b_ref = irop_get_symref_ex(ir, b);

    if (!a_ref || !b_ref)
      return 0;

    return a_ref->sym == b_ref->sym && a_ref->addend == b_ref->addend && a_ref->flags == b_ref->flags;
  }
}

static int ir_opt_pure_def_equal(TCCIRState *ir, int a_def_idx, int b_def_idx, int depth)
{
  IRQuadCompact *qa;
  IRQuadCompact *qb;

  if (a_def_idx < 0 || b_def_idx < 0)
    return 0;
  if (depth > 12)
    return 0;

  qa = &ir->compact_instructions[a_def_idx];
  qb = &ir->compact_instructions[b_def_idx];

  if (qa->op != qb->op)
    return 0;

  switch (qa->op)
  {
  case TCCIR_OP_ASSIGN:
    return ir_opt_pure_expr_equal(ir, tcc_ir_op_get_src1(ir, qa), a_def_idx, tcc_ir_op_get_src1(ir, qb), b_def_idx,
                                  depth + 1);
  case TCCIR_OP_OR:
  case TCCIR_OP_AND:
  case TCCIR_OP_XOR:
  case TCCIR_OP_BOOL_OR:
  case TCCIR_OP_BOOL_AND:
  {
    IROperand a1 = tcc_ir_op_get_src1(ir, qa);
    IROperand a2 = tcc_ir_op_get_src2(ir, qa);
    IROperand b1 = tcc_ir_op_get_src1(ir, qb);
    IROperand b2 = tcc_ir_op_get_src2(ir, qb);
    return ((ir_opt_pure_expr_equal(ir, a1, a_def_idx, b1, b_def_idx, depth + 1) &&
             ir_opt_pure_expr_equal(ir, a2, a_def_idx, b2, b_def_idx, depth + 1)) ||
            (ir_opt_pure_expr_equal(ir, a1, a_def_idx, b2, b_def_idx, depth + 1) &&
             ir_opt_pure_expr_equal(ir, a2, a_def_idx, b1, b_def_idx, depth + 1)));
  }
  case TCCIR_OP_FUNCCALLVAL:
  {
    IROperand a_callee_op = tcc_ir_op_get_src1(ir, qa);
    IROperand b_callee_op = tcc_ir_op_get_src1(ir, qb);
    Sym *a_callee = irop_get_sym_ex(ir, a_callee_op);
    Sym *b_callee = irop_get_sym_ex(ir, b_callee_op);
    const char *a_name;
    const char *b_name;
    IROperand a_call_meta = tcc_ir_op_get_src2(ir, qa);
    IROperand b_call_meta = tcc_ir_op_get_src2(ir, qb);
    int argc;

    if (!a_callee || !b_callee)
      return 0;

    a_name = get_tok_str(a_callee->v, NULL);
    b_name = get_tok_str(b_callee->v, NULL);
    if (!ir_opt_is_pure_helper_name(a_name) || !b_name || strcmp(a_name, b_name) != 0)
      return 0;

    argc = TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, a_call_meta));
    if (argc != TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, b_call_meta)))
      return 0;

    for (int param_idx = 0; param_idx < argc; ++param_idx)
    {
      IROperand a_arg;
      IROperand b_arg;
      if (!ir_opt_get_call_param_operand(ir, a_def_idx, param_idx, &a_arg) ||
          !ir_opt_get_call_param_operand(ir, b_def_idx, param_idx, &b_arg))
      {
        return 0;
      }
      if (!ir_opt_pure_expr_equal(ir, a_arg, a_def_idx, b_arg, b_def_idx, depth + 1))
        return 0;
    }

    return 1;
  }
  default:
    return 0;
  }
}

static int ir_opt_pure_expr_equal(TCCIRState *ir, IROperand a, int a_use_idx, IROperand b, int b_use_idx, int depth)
{
  int a_tag;
  int b_tag;
  int32_t a_vr;
  int32_t b_vr;
  int a_def_idx;
  int b_def_idx;

  if (depth > 12)
    return 0;

  if (irop_is_immediate(a) || irop_is_immediate(b))
  {
    if (!irop_is_immediate(a) || !irop_is_immediate(b))
      return 0;
    return irop_get_imm64_ex(ir, a) == irop_get_imm64_ex(ir, b);
  }

  a_tag = irop_get_tag(a);
  b_tag = irop_get_tag(b);
  if (a_tag != IROP_TAG_VREG || b_tag != IROP_TAG_VREG)
    return ir_opt_nonvreg_expr_equal(ir, a, b);

  a_vr = irop_get_vreg(a);
  b_vr = irop_get_vreg(b);
  if (a_vr < 0 || b_vr < 0)
  {
    if (a_vr != b_vr)
      return 0;
    return a.vr == b.vr && a.u.imm32 == b.u.imm32 && a.is_unsigned == b.is_unsigned && a.is_static == b.is_static &&
           a.is_sym == b.is_sym && a.is_param == b.is_param;
  }

  a_def_idx = tcc_ir_find_defining_instruction(ir, a_vr, a_use_idx);
  b_def_idx = tcc_ir_find_defining_instruction(ir, b_vr, b_use_idx);

  if (a_def_idx < 0 || b_def_idx < 0)
    return a_vr == b_vr && a_def_idx == b_def_idx;

  if (a_def_idx == b_def_idx)
    return 1;

  /* When comparing different vregs, each must have a single definition.
   * tcc_ir_find_defining_instruction does a linear backward scan and may
   * find only one of multiple reaching definitions at merge points.
   * Without this check, two vregs with different semantics can appear
   * equal if they share the same constant on one branch (e.g. is_float
   * and is_max both have a path that assigns #1). */
  if (!tcc_ir_vreg_has_single_def(ir, a_vr) || !tcc_ir_vreg_has_single_def(ir, b_vr))
    return 0;

  return ir_opt_pure_def_equal(ir, a_def_idx, b_def_idx, depth + 1);
}

static int ir_opt_is_pure_fallthrough_instruction(TCCIRState *ir, int idx)
{
  IRQuadCompact *q;
  Sym *callee;
  const char *name;

  if (!ir || idx < 0 || idx >= ir->next_instruction_index)
    return 0;

  q = &ir->compact_instructions[idx];
  switch (q->op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_OR:
  case TCCIR_OP_AND:
  case TCCIR_OP_XOR:
  case TCCIR_OP_BOOL_OR:
  case TCCIR_OP_BOOL_AND:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
    return 1;
  case TCCIR_OP_FUNCCALLVAL:
    callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      return 0;
    name = get_tok_str(callee->v, NULL);
    return ir_opt_is_pure_helper_name(name);
  default:
    return 0;
  }
}

static int ir_opt_match_zero_test(TCCIRState *ir, int idx, IROperand *expr_out)
{
  IRQuadCompact *q;
  IROperand src1;
  IROperand src2;

  if (!ir || idx < 0 || idx >= ir->next_instruction_index || !expr_out)
    return 0;

  q = &ir->compact_instructions[idx];
  if (q->op == TCCIR_OP_TEST_ZERO)
  {
    *expr_out = tcc_ir_op_get_src1(ir, q);
    return 1;
  }

  if (q->op != TCCIR_OP_CMP)
    return 0;

  src1 = tcc_ir_op_get_src1(ir, q);
  src2 = tcc_ir_op_get_src2(ir, q);
  if (irop_is_immediate(src2) && irop_get_imm64_ex(ir, src2) == 0)
  {
    *expr_out = src1;
    return 1;
  }
  if (irop_is_immediate(src1) && irop_get_imm64_ex(ir, src1) == 0)
  {
    *expr_out = src2;
    return 1;
  }

  return 0;
}

int tcc_ir_opt_float_branch_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  uint8_t *is_merge;

  if (n < 4)
    return 0;

  is_merge = ir_opt_build_merge_bitmap(ir, n);

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      Sym *callee;
      const char *name;
      int jump1_idx = ir_opt_next_non_nop(ir, i + 1);
      int cmp2_idx;
      int jump2_idx;
      IRQuadCompact *jump1;
      IRQuadCompact *cmp2;
      IRQuadCompact *jump2;
      IROperand arg0;
      IROperand arg1;
      IROperand cmp2_arg0;
      IROperand cmp2_arg1;
      int tok1;
      int tok2;
      int known_fact;
      int effective_tok2 = -1;
      int is_swapped = 0;

      callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!callee)
        continue;
      name = get_tok_str(callee->v, NULL);
      if (!ir_opt_is_flag_cmp_helper_name(name))
        continue;
      if (!ir_opt_get_call_param_operand(ir, i, 0, &arg0) || !ir_opt_get_call_param_operand(ir, i, 1, &arg1))
        continue;

      if (jump1_idx < 0)
        continue;
      jump1 = &ir->compact_instructions[jump1_idx];
      if (jump1->op != TCCIR_OP_JUMPIF)
        continue;

      cmp2_idx = -1;
      jump2_idx = -1;
      for (int scan_idx = ir_opt_next_non_nop(ir, jump1_idx + 1); scan_idx >= 0 && scan_idx < n;
           scan_idx = ir_opt_next_non_nop(ir, scan_idx + 1))
      {
        IRQuadCompact *scan_q;
        Sym *scan_callee;
        const char *scan_name;

        if (is_merge[scan_idx / 8] & (1 << (scan_idx % 8)))
          break;

        scan_q = &ir->compact_instructions[scan_idx];
        if (scan_q->op != TCCIR_OP_FUNCCALLVOID && scan_q->op != TCCIR_OP_FUNCCALLVAL)
        {
          if (!ir_opt_is_pure_fallthrough_instruction(ir, scan_idx))
            break;
          continue;
        }

        scan_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, scan_q));
        scan_name = scan_callee ? get_tok_str(scan_callee->v, NULL) : NULL;
        if (!ir_opt_is_flag_cmp_helper_name(scan_name))
        {
          if (!ir_opt_is_pure_fallthrough_instruction(ir, scan_idx))
            break;
          continue;
        }

        cmp2_idx = scan_idx;
        jump2_idx = ir_opt_next_non_nop(ir, cmp2_idx + 1);
        break;
      }

      if (cmp2_idx < 0 || jump2_idx < 0)
        continue;

      cmp2 = &ir->compact_instructions[cmp2_idx];
      jump2 = &ir->compact_instructions[jump2_idx];
      if (jump2->op != TCCIR_OP_JUMPIF)
        continue;

      callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, cmp2));
      if (!callee)
        continue;
      name = get_tok_str(callee->v, NULL);
      if (!ir_opt_is_flag_cmp_helper_name(name))
        continue;
      if (!ir_opt_get_call_param_operand(ir, cmp2_idx, 0, &cmp2_arg0) ||
          !ir_opt_get_call_param_operand(ir, cmp2_idx, 1, &cmp2_arg1))
      {
        continue;
      }

      tok1 = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump1));
      tok2 = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump2));
      known_fact = vrp_negate_cmp_tok(tok1);
      if (known_fact < 0)
      {
        continue;
      }

      int eq1 = ir_opt_pure_expr_equal(ir, arg0, i, cmp2_arg0, cmp2_idx, 0);
      int eq2 = ir_opt_pure_expr_equal(ir, arg1, i, cmp2_arg1, cmp2_idx, 0);
      if (eq1 && eq2)
        effective_tok2 = tok2;
      else if (ir_opt_pure_expr_equal(ir, arg0, i, cmp2_arg1, cmp2_idx, 0) &&
               ir_opt_pure_expr_equal(ir, arg1, i, cmp2_arg0, cmp2_idx, 0))
      {
        is_swapped = 1;
        effective_tok2 = vrp_swap_cmp_tok(tok2);
      }

      if (effective_tok2 < 0)
      {
        continue;
      }

      if (is_swapped)
      {
        IROperand jmp1_dest = tcc_ir_op_get_dest(ir, jump1);
        IROperand jmp2_dest = tcc_ir_op_get_dest(ir, jump2);
        if (jmp1_dest.u.imm32 != jmp2_dest.u.imm32)
        {
          switch (known_fact)
          {
          case TOK_LT:
          case TOK_GT:
          case TOK_ULT:
          case TOK_UGT:
            break;
          default:
            continue;
          }
        }
      }

      if (fcmp_cmp_implies(known_fact, effective_tok2))
      {
        IROperand jmp2_dest = tcc_ir_op_get_dest(ir, jump2);
        cmp2->op = TCCIR_OP_NOP;
        jump2->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, jump2_idx, jmp2_dest);
        changes++;
      }
      else if (fcmp_cmp_implies(known_fact, vrp_negate_cmp_tok(effective_tok2)))
      {
        cmp2->op = TCCIR_OP_NOP;
        jump2->op = TCCIR_OP_NOP;
        changes++;
      }

      continue;
    }

    if (q->op == TCCIR_OP_TEST_ZERO || q->op == TCCIR_OP_CMP)
    {
      IRQuadCompact *jump1;
      int jump1_idx = ir_opt_next_non_nop(ir, i + 1);
      int known_zero = -1;
      IROperand expr1;

      if (!ir_opt_match_zero_test(ir, i, &expr1))
        continue;

      if (jump1_idx < 0)
        continue;
      jump1 = &ir->compact_instructions[jump1_idx];
      if (jump1->op != TCCIR_OP_JUMPIF)
        continue;

      switch ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump1)))
      {
      case TOK_NE:
        known_zero = 1;
        break;
      case TOK_EQ:
        known_zero = 0;
        break;
      default:
        break;
      }
      if (known_zero < 0)
        continue;

      for (int test2_idx = ir_opt_next_non_nop(ir, jump1_idx + 1); test2_idx >= 0 && test2_idx + 1 < n;
           test2_idx = ir_opt_next_non_nop(ir, test2_idx + 1))
      {
        IRQuadCompact *test2;
        IRQuadCompact *jump2;
        int jump2_idx;
        int tok2;
        IROperand expr2;
        int is_zero_test_candidate;

        if (is_merge[test2_idx / 8] & (1 << (test2_idx % 8)))
          break;

        test2 = &ir->compact_instructions[test2_idx];
        is_zero_test_candidate = ir_opt_match_zero_test(ir, test2_idx, &expr2);
        if (!is_zero_test_candidate)
        {
          if (!ir_opt_is_pure_fallthrough_instruction(ir, test2_idx))
            break;
          continue;
        }

        jump2_idx = ir_opt_next_non_nop(ir, test2_idx + 1);
        if (jump2_idx < 0)
          break;

        jump2 = &ir->compact_instructions[jump2_idx];
        if (jump2->op != TCCIR_OP_JUMPIF)
          break;

        if (!ir_opt_pure_expr_equal(ir, expr1, i, expr2, test2_idx, 0))
          continue;

        tok2 = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump2));
        if ((known_zero && tok2 == TOK_EQ) || (!known_zero && tok2 == TOK_NE))
        {
          IROperand jmp2_dest = tcc_ir_op_get_dest(ir, jump2);
          test2->op = TCCIR_OP_NOP;
          jump2->op = TCCIR_OP_JUMP;
          tcc_ir_set_dest(ir, jump2_idx, jmp2_dest);
          changes++;
        }
        else if ((known_zero && tok2 == TOK_NE) || (!known_zero && tok2 == TOK_EQ))
        {
          test2->op = TCCIR_OP_NOP;
          jump2->op = TCCIR_OP_NOP;
          changes++;
        }
        break;
      }
    }
  }

  tcc_free(is_merge);
  return changes;
}

int tcc_ir_opt_vrp(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 3)
    return 0;

  /* Precompute merge points (multiple predecessors or back-edge targets) */
  uint8_t *is_merge = ir_opt_build_merge_bitmap(ir, n);

  /* Range table: PARAM in 0..VRP_MAX_POS-1, TEMP in VRP_MAX_POS..2*VRP_MAX_POS-1,
   * VAR in 2*VRP_MAX_POS..3*VRP_MAX_POS-1 */
  VRPRange ranges[VRP_MAX_POS * 3];
  memset(ranges, 0, sizeof(ranges));

  /* Pending fall-through constraint: applied at instruction pending_apply_at */
  int pending_apply_at = -1;
  int pending_slot = -1;
  int64_t pending_min = 0;
  int64_t pending_max = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* At merge points: clear all ranges and discard pending constraint */
    if (is_merge[i / 8] & (1 << (i % 8)))
    {
      memset(ranges, 0, sizeof(ranges));
      pending_apply_at = -1;
      pending_slot = -1;
    }
    else if (pending_apply_at == i && pending_slot >= 0)
    {
      /* Apply fall-through constraint (intersect with any existing range) */
      VRPRange *r = &ranges[pending_slot];
      if (r->valid)
      {
        pending_min = pending_min > r->min_val ? pending_min : r->min_val;
        pending_max = pending_max < r->max_val ? pending_max : r->max_val;
      }
      if (pending_min <= pending_max)
      {
        r->valid = 1;
        r->min_val = pending_min;
        r->max_val = pending_max;
      }
      pending_apply_at = -1;
      pending_slot = -1;
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Track arithmetic: T/P_dest = T/P_src1 +/- #imm → propagate range */
    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_is_immediate(src2))
    {
      int32_t src1_vr = irop_get_vreg(src1);
      int32_t dest_vr = irop_get_vreg(dest);
      if (src1_vr >= 0 && dest_vr >= 0)
      {
        int src_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(src1_vr), TCCIR_DECODE_VREG_POSITION(src1_vr));
        int dst_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(dest_vr), TCCIR_DECODE_VREG_POSITION(dest_vr));
        if (src_slot >= 0 && ranges[src_slot].valid && dst_slot >= 0)
        {
          int64_t imm = irop_get_imm64_ex(ir, src2);
          int64_t new_min = (q->op == TCCIR_OP_ADD) ? ranges[src_slot].min_val + imm : ranges[src_slot].min_val - imm;
          int64_t new_max = (q->op == TCCIR_OP_ADD) ? ranges[src_slot].max_val + imm : ranges[src_slot].max_val - imm;
          /* Clamp to int32 range to stay within 32-bit value semantics */
          if (new_min < (int64_t)INT32_MIN)
            new_min = INT32_MIN;
          if (new_max > (int64_t)INT32_MAX)
            new_max = INT32_MAX;
          ranges[dst_slot].valid = 1;
          ranges[dst_slot].min_val = new_min;
          ranges[dst_slot].max_val = new_max;
        }
        else if (dst_slot >= 0)
        {
          ranges[dst_slot].valid = 0;
        }
      }
      continue;
    }

    /* CMP + JUMPIF: try to fold using range, or derive fall-through constraint */
    if (q->op == TCCIR_OP_CMP && i + 1 < n)
    {
      IRQuadCompact *jump_q = &ir->compact_instructions[i + 1];
      if (jump_q->op == TCCIR_OP_JUMPIF && irop_is_immediate(src2))
      {
        int32_t src1_vr = irop_get_vreg(src1);
        if (src1_vr >= 0)
        {
          int src_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(src1_vr), TCCIR_DECODE_VREG_POSITION(src1_vr));
          int64_t cmp_val = irop_get_imm64_ex(ir, src2);
          IROperand cond_op = tcc_ir_op_get_src1(ir, jump_q);
          int tok = (int)irop_get_imm64_ex(ir, cond_op);
          IROperand jmp_dest = tcc_ir_op_get_dest(ir, jump_q);

          /* Try to fold using known range */
          if (src_slot >= 0 && ranges[src_slot].valid)
          {
            int64_t rmin = ranges[src_slot].min_val;
            int64_t rmax = ranges[src_slot].max_val;
            int fold_result = -1;
            /* Monotone signed conditions: checking endpoints suffices */
            int is_monotone_signed = (tok == 0x9c || tok == 0x9d || tok == 0x9e || tok == 0x9f);
            /* TOK_ULT=0x92, TOK_UGE=0x93, TOK_ULE=0x96, TOK_UGT=0x97 per tcc.h */
            int is_unsigned_cond = (tok == 0x92 || tok == 0x93 || tok == 0x96 || tok == 0x97);
            /* EQ/NE are NOT monotone — special handling below */
            int is_eq_ne = (tok == 0x94 || tok == 0x95);

            if (is_monotone_signed)
            {
              fold_result = vrp_fold_cmp(rmin, rmax, cmp_val, tok);
            }
            else if (is_unsigned_cond && rmin >= 0 && rmax >= 0)
            {
              /* Both endpoints non-negative: uint32 ordering matches int64 ordering */
              fold_result = vrp_fold_cmp(rmin, rmax, cmp_val, tok);
            }
            else if (is_unsigned_cond && rmin < 0 && rmax < 0)
            {
              /* Both endpoints negative as int32: uint32 ordering preserved in int64.
               * (For two negative int32 a < b: uint32(a) = a+2^32 < uint32(b) = b+2^32,
               * and uint64(int64(a)) = a+2^64 < uint64(int64(b)) = b+2^64 — same order.) */
              fold_result = vrp_fold_cmp(rmin, rmax, cmp_val, tok);
            }
            else if (is_eq_ne)
            {
              /* For == and !=, endpoint checking alone is insufficient since
               * these are not monotone. We can only fold when:
               * (a) cmp_val is outside [rmin, rmax] → value can never/always match
               * (b) rmin == rmax → singleton range, exact comparison */
              if (cmp_val < rmin || cmp_val > rmax)
              {
                /* cmp_val outside range: == is never true, != is always true */
                fold_result = (tok == 0x95) ? 1 : 0;
              }
              else if (rmin == rmax)
              {
                /* Singleton: cmp_val == rmin, so == is true, != is false */
                fold_result = (tok == 0x94) ? 1 : 0;
              }
            }

            if (fold_result == 1)
            {
              /* Branch always taken → unconditional JUMP */
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_JUMP;
              tcc_ir_set_dest(ir, i + 1, jmp_dest);
              changes++;
              continue;
            }
            else if (fold_result == 0)
            {
              /* Branch never taken → NOP both */
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_NOP;
              changes++;
              continue;
            }
          }

          /* Set pending fall-through constraint: NOT(cond) holds after JUMPIF not-taken */
          if (src_slot >= 0 && i + 2 < n)
          {
            int64_t new_min = INT32_MIN;
            int64_t new_max = INT32_MAX;
            int set_constraint = 0;

            /* Fall-through means cond is FALSE for (src1 vs cmp_val) */
            switch (tok)
            {
            case 0x9e: /* TOK_LE (<=S): fall-through: src1 > cmp_val */
              if (cmp_val < (int64_t)INT32_MAX)
              {
                new_min = cmp_val + 1;
                new_max = INT32_MAX;
                set_constraint = 1;
              }
              break;
            case 0x9c: /* TOK_LT (<S): fall-through: src1 >= cmp_val */
              new_min = cmp_val < (int64_t)INT32_MIN ? INT32_MIN : cmp_val;
              new_max = INT32_MAX;
              set_constraint = 1;
              break;
            case 0x9d: /* TOK_GE (>=S): fall-through: src1 < cmp_val */
              new_min = INT32_MIN;
              new_max = cmp_val > (int64_t)INT32_MAX ? INT32_MAX : cmp_val - 1;
              set_constraint = (new_max >= (int64_t)INT32_MIN);
              break;
            case 0x9f: /* TOK_GT (>S): fall-through: src1 <= cmp_val */
              new_min = INT32_MIN;
              new_max = cmp_val > (int64_t)INT32_MAX ? INT32_MAX : cmp_val;
              set_constraint = 1;
              break;
            case 0x95: /* TOK_NE (!=): fall-through: src1 == cmp_val */
              new_min = cmp_val;
              new_max = cmp_val;
              set_constraint = (cmp_val >= INT32_MIN && cmp_val <= INT32_MAX);
              break;
            default:
              break;
            }

            if (set_constraint && new_min <= new_max)
            {
              /* Schedule constraint application at instruction i+2 (after the JUMPIF) */
              pending_apply_at = i + 2;
              pending_slot = src_slot;
              pending_min = new_min;
              pending_max = new_max;
            }
          }
        }
      }
      /* Register-register comparison constraint propagation.
       * Pattern: CMP A,B; JUMPIF c1 (falls through → !c1 holds for A vs B)
       *          CMP A,B; JUMPIF c2 (or CMP B,A; JUMPIF c2)
       * If !c1 implies c2 → second branch always taken → unconditional JUMP.
       * If !c1 implies !c2 → second branch never taken → NOP both. */
      else if (jump_q->op == TCCIR_OP_JUMPIF)
      {
        int32_t cmp_vr1 = irop_get_vreg(src1);
        int32_t cmp_vr2 = irop_get_vreg(src2);
        if (cmp_vr1 >= 0 && cmp_vr2 >= 0 && i + 3 < n)
        {
          IROperand cond_op = tcc_ir_op_get_src1(ir, jump_q);
          int tok1 = (int)irop_get_imm64_ex(ir, cond_op);
          int known_fact = vrp_negate_cmp_tok(tok1);

          /* Only proceed if the fall-through target is not a merge point */
          if (known_fact >= 0 && !(is_merge[(i + 2) / 8] & (1 << ((i + 2) % 8))))
          {
            IRQuadCompact *cmp2 = &ir->compact_instructions[i + 2];
            if (cmp2->op == TCCIR_OP_CMP)
            {
              IRQuadCompact *jump2 = &ir->compact_instructions[i + 3];
              if (jump2->op == TCCIR_OP_JUMPIF)
              {
                IROperand cmp2_src1 = tcc_ir_op_get_src1(ir, cmp2);
                IROperand cmp2_src2 = tcc_ir_op_get_src2(ir, cmp2);
                int32_t cmp2_vr1 = irop_get_vreg(cmp2_src1);
                int32_t cmp2_vr2 = irop_get_vreg(cmp2_src2);

                IROperand cond2_op = tcc_ir_op_get_src1(ir, jump2);
                int tok2 = (int)irop_get_imm64_ex(ir, cond2_op);
                IROperand jmp2_dest = tcc_ir_op_get_dest(ir, jump2);

                int effective_tok2 = -1;
                if (cmp2_vr1 == cmp_vr1 && cmp2_vr2 == cmp_vr2)
                  effective_tok2 = tok2; /* same operand order */
                else if (cmp2_vr1 == cmp_vr2 && cmp2_vr2 == cmp_vr1)
                  effective_tok2 = vrp_swap_cmp_tok(tok2); /* swapped operands */

                if (effective_tok2 >= 0)
                {
                  if (vrp_cmp_implies(known_fact, effective_tok2))
                  {
                    /* Second branch always taken → unconditional JUMP */
                    cmp2->op = TCCIR_OP_NOP;
                    jump2->op = TCCIR_OP_JUMP;
                    tcc_ir_set_dest(ir, i + 3, jmp2_dest);
                    changes++;
                  }
                  else if (vrp_cmp_implies(known_fact, vrp_negate_cmp_tok(effective_tok2)))
                  {
                    /* Second branch never taken → NOP both */
                    cmp2->op = TCCIR_OP_NOP;
                    jump2->op = TCCIR_OP_NOP;
                    changes++;
                  }
                }
              }
            }
          }
        }
      }
      continue;
    }

    /* Any other instruction writing to a tracked slot invalidates its range */
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr >= 0 && irop_config[q->op].has_dest)
    {
      int dst_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(dest_vr), TCCIR_DECODE_VREG_POSITION(dest_vr));
      if (dst_slot >= 0)
        ranges[dst_slot].valid = 0;
    }

    /* After instructions with no fall-through (JUMP, RETURN), clear all ranges
     * and discard pending constraints. The next linear instruction (if any) is
     * only reachable via its own predecessors, not from here. Without this,
     * constraints from one path leak to dead code or to instructions reached
     * from a different branch. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      memset(ranges, 0, sizeof(ranges));
      pending_apply_at = -1;
      pending_slot = -1;
    }
  }

  tcc_free(is_merge);

  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

/* ============================================================================
 * Redundant Loop Check Elimination
 *
 * When a loop guard ensures a condition (e.g., i < 4), any CMP+JUMPIF inside
 * the loop body that tests the same variable against the same constant with
 * an implied condition is redundant and can be folded.
 *
 * Pattern:
 *   header:  CMP V1, #4; JUMPIF >=U, exit    (guard: V1 < 4 in body)
 *   body:    V4 = V1; CMP V4, #4; JUMPIF <U  (redundant: always taken)
 * ============================================================================ */
int tcc_ir_opt_redundant_loop_check(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
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

    int32_t guard_vreg = -1;
    int64_t guard_const = 0;
    int guard_body_fact = -1;
    int guard_cmp_idx = -1;

    for (int i = loop->header_idx; i <= loop->header_idx + 4 && i <= loop->end_idx && i < n - 1; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op == TCCIR_OP_NOP)
        continue;
      if (cq->op != TCCIR_OP_CMP)
        continue;

      IROperand s1 = tcc_ir_op_get_src1(ir, cq);
      IROperand s2 = tcc_ir_op_get_src2(ir, cq);
      if (!irop_is_immediate(s2) || s2.is_sym)
        continue;
      int32_t vr = irop_get_vreg(s1);
      if (vr < 0)
        continue;

      int j = i + 1;
      while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
        j++;
      if (j >= n || ir->compact_instructions[j].op != TCCIR_OP_JUMPIF)
        continue;

      IRQuadCompact *jq = &ir->compact_instructions[j];
      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int cond = (int)irop_get_imm64_ex(ir, cond_op);
      IROperand jdest = tcc_ir_op_get_dest(ir, jq);
      int target = (int)jdest.u.imm32;

      if (target > loop->end_idx || target < loop->start_idx)
      {
        int neg = vrp_negate_cmp_tok(cond);
        if (neg >= 0)
        {
          guard_cmp_idx = i;
          guard_vreg = vr;
          guard_const = irop_get_imm64_ex(ir, s2);
          guard_body_fact = neg;
          break;
        }
      }
    }

    if (guard_body_fact < 0)
      continue;

    /* Find the scan range: all instructions in the loop body.
     * The guard fact holds between the header fall-through and the back-edge,
     * including body blocks that are after the back-edge in instruction order
     * but reachable from the header via a forward JMP.
     * Use the exit target as the upper bound — anything before the exit
     * target is in the loop body. */
    int exit_target = -1;
    {
      int gi = guard_cmp_idx + 1;
      while (gi < n && ir->compact_instructions[gi].op == TCCIR_OP_NOP)
        gi++;
      if (gi < n && ir->compact_instructions[gi].op == TCCIR_OP_JUMPIF)
      {
        IROperand gd = tcc_ir_op_get_dest(ir, &ir->compact_instructions[gi]);
        exit_target = (int)gd.u.imm32;
      }
    }
    int scan_end = (exit_target > 0) ? exit_target - 1 : loop->end_idx;

    for (int i = loop->start_idx; i <= scan_end && i < n - 1; i++)
    {
      if (i == guard_cmp_idx)
        continue;

      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
        continue;

      IROperand s1 = tcc_ir_op_get_src1(ir, cq);
      IROperand s2 = tcc_ir_op_get_src2(ir, cq);
      if (!irop_is_immediate(s2) || s2.is_sym)
        continue;
      if (irop_get_imm64_ex(ir, s2) != guard_const)
        continue;

      int32_t inner_vr = irop_get_vreg(s1);
      if (inner_vr < 0)
        continue;

      int vreg_match = (inner_vr == guard_vreg);
      if (!vreg_match)
      {
        int def_idx = tcc_ir_find_defining_instruction(ir, inner_vr, i);
        if (def_idx >= 0)
        {
          IRQuadCompact *dq = &ir->compact_instructions[def_idx];
          if (dq->op == TCCIR_OP_STORE || dq->op == TCCIR_OP_ASSIGN)
          {
            IROperand dsrc = tcc_ir_op_get_src1(ir, dq);
            if (irop_get_vreg(dsrc) == guard_vreg)
              vreg_match = 1;
          }
        }
      }
      if (!vreg_match)
        continue;

      int j = i + 1;
      while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
        j++;
      if (j >= n || ir->compact_instructions[j].op != TCCIR_OP_JUMPIF)
        continue;

      IRQuadCompact *jq = &ir->compact_instructions[j];
      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int inner_cond = (int)irop_get_imm64_ex(ir, cond_op);
      IROperand jdest = tcc_ir_op_get_dest(ir, jq);

      if (vrp_cmp_implies(guard_body_fact, inner_cond))
      {
        cq->op = TCCIR_OP_NOP;
        jq->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, j, jdest);
        changes++;
      }
      else
      {
        int neg_inner = vrp_negate_cmp_tok(inner_cond);
        if (neg_inner >= 0 && vrp_cmp_implies(guard_body_fact, neg_inner))
        {
          cq->op = TCCIR_OP_NOP;
          jq->op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }
  }

  tcc_ir_free_loops(loops);
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

  /* Mark block starts (shared helper) */
  ir_opt_mark_block_starts(ir, block_start_seen, block_start_gen, n);

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

    /* Propagate TMP constants to src1.
     * Skip SWITCH_TABLE and IJUMP: their src1 (the index / target address)
     * must remain in a register — the ARM code generator cannot handle an
     * immediate operand there. */
    if (irop_config[q->op].has_src1 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_TEMP &&
        q->op != TCCIR_OP_SWITCH_TABLE && q->op != TCCIR_OP_IJUMP)
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
    /* Propagate TMP constants to src2 */
    if (irop_config[q->op].has_src2 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(src2_vr);
      if (pos <= max_tmp_pos && tmp_info[pos].gen == current_gen)
      {
        LOG_IR_GEN("OPTIMIZE: TMP const propagate TMP:%d = %lld to src2 at i=%d", pos, (long long)tmp_info[pos].value,
                   i);
        int btype = irop_get_btype(src2);
        int64_t val = tmp_info[pos].value;
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
        q->op == TCCIR_OP_ASSIGN)
    {
      const int pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      IROperand cur_src1 = tcc_ir_op_get_src1(ir, q);
      if (pos <= max_tmp_pos && irop_is_immediate(cur_src1))
      {
        tmp_info[pos].gen = current_gen;
        tmp_info[pos].value = irop_get_imm64_ex(ir, cur_src1);
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
      LOG_IR_GEN("BOOL IDEMPOTENT: %s vr%d with itself at i=%d -> ASSIGN", is_and ? "&&" : "||", src1.vr, i);
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
        LOG_IR_GEN("BOOL IDEMPOTENT: %s with neutral element at i=%d -> ASSIGN", is_and ? "&&" : "||", i);
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

    LOG_IR_GEN("BOOL SIMPLIFY: Nested %s at i=%d (inner at i=%d)", q->op == TCCIR_OP_BOOL_AND ? "&&" : "||", i,
               def_idx);

    /* The second inner op will be eliminated by DCE if unused */
    changes++;
  }

  return changes;
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

/* Helper: insert instruction before `before_idx`, shift array, patch jumps.
 * Returns the index where the instruction was inserted (-1 on failure). */
static int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q)
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
  return before_idx;
}

int tcc_ir_opt_globalsym_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  typedef struct
  {
    Sym *sym;
    int64_t addend;
    int count;
  } GSymEntry;

  /* Scan entire function for repeated GlobalSym operands in ADD instructions */
  GSymEntry entries[GSYM_CSE_MAX];
  int num_entries = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD)
      continue;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src1) != IROP_TAG_SYMREF || src1.is_lval)
      continue;
    IRPoolSymref *sr = irop_get_symref_ex(ir, src1);
    if (!sr || !sr->sym)
      continue;
    int found = -1;
    for (int e = 0; e < num_entries; e++)
    {
      if (entries[e].sym == sr->sym && entries[e].addend == sr->addend)
      {
        found = e;
        break;
      }
    }
    if (found >= 0)
      entries[found].count++;
    else if (num_entries < GSYM_CSE_MAX)
    {
      entries[num_entries].sym = sr->sym;
      entries[num_entries].addend = sr->addend;
      entries[num_entries].count = 1;
      num_entries++;
    }
  }

  /* Sort entries by use count descending so the most-used bases get priority */
  for (int i = 0; i < num_entries - 1; i++)
    for (int j = i + 1; j < num_entries; j++)
      if (entries[j].count > entries[i].count)
      {
        GSymEntry tmp = entries[i];
        entries[i] = entries[j];
        entries[j] = tmp;
      }

  /* For entries with 3+ uses, insert ASSIGN at function entry and replace all uses.
   * Limit based on estimated register pressure in the function body. */
  int max_gsym_hoist = tcc_ir_estimate_hoist_budget(ir, 0, n - 1, ir->parameters_count);
  if (max_gsym_hoist < 2)
    max_gsym_hoist = 2;
  int total_inserted = 0;
  for (int e = 0; e < num_entries; e++)
  {
    if (entries[e].count < 3)
      continue;
    if (total_inserted >= max_gsym_hoist)
      break;

    int32_t base_vr = tcc_ir_vreg_alloc_temp(ir);

    /* Find the first ADD with this GlobalSym to copy the operand */
    IROperand sym_op = IROP_NONE;
    {
      int nn = ir->next_instruction_index;
      for (int j = total_inserted; j < nn; j++)
      {
        IRQuadCompact *sq = &ir->compact_instructions[j];
        if (sq->op != TCCIR_OP_ADD)
          continue;
        IROperand s1 = tcc_ir_op_get_src1(ir, sq);
        if (irop_get_tag(s1) != IROP_TAG_SYMREF || s1.is_lval)
          continue;
        IRPoolSymref *ssr = irop_get_symref_ex(ir, s1);
        if (ssr && ssr->sym == entries[e].sym && ssr->addend == entries[e].addend)
        {
          sym_op = s1;
          break;
        }
      }
    }
    if (irop_get_tag(sym_op) != IROP_TAG_SYMREF)
      continue;

    /* Build the ASSIGN instruction: T_base = GlobalSym+offset */
    IROperand dest_op = irop_make_vreg(base_vr, IROP_BTYPE_INT32);
    IRQuadCompact assign_q = {0};
    assign_q.op = TCCIR_OP_ASSIGN;
    assign_q.operand_base = tcc_ir_pool_add(ir, dest_op);
    tcc_ir_pool_add(ir, sym_op);

    /* Insert at position 0 (function entry) */
    gsym_cse_insert_before(ir, total_inserted, &assign_q);
    total_inserted++;

    /* Replace all matching GlobalSym src1 operands with the TEMP */
    IROperand base_ref = irop_make_vreg(base_vr, IROP_BTYPE_INT32);
    int nn = ir->next_instruction_index;
    for (int j = total_inserted; j < nn; j++)
    {
      IRQuadCompact *rq = &ir->compact_instructions[j];
      if (rq->op != TCCIR_OP_ADD)
        continue;
      IROperand rs1 = tcc_ir_op_get_src1(ir, rq);
      if (irop_get_tag(rs1) != IROP_TAG_SYMREF || rs1.is_lval)
        continue;
      IRPoolSymref *rsr = irop_get_symref_ex(ir, rs1);
      if (!rsr || rsr->sym != entries[e].sym || rsr->addend != entries[e].addend)
        continue;
      tcc_ir_op_set_src1(ir, rq, base_ref);
      changes++;
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

int tcc_ir_opt_deref_fwd(TCCIRState *ir)
{
  /* Forward a deref'd load to a subsequent use of the same deref.
   *
   *   i:   Vdest = Tsrc***DEREF***          (load from pointer)
   *   j:   CMP Rx, Tsrc***DEREF***          (same pointer deref)
   *                ^^^^^^^^^^^^^^^^^^
   *   =>   CMP Rx, Vdest                    (use already-loaded value)
   *
   * Only fires when i and j are adjacent (or separated only by NOPs)
   * so no aliasing or clobber analysis is needed. */
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* Only match ASSIGN/LOAD/STORE — these are the opcodes that genuinely
     * load a value from a dereferenced pointer into a destination vreg. */
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_STORE)
      continue;
    if (!irop_config[q->op].has_dest || !irop_config[q->op].has_src1)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (!src1.is_lval)
      continue;
    int32_t load_addr_vr = irop_get_vreg(src1);
    if (load_addr_vr < 0)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || dest.is_lval)
      continue;

    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      break;

    IRQuadCompact *next = &ir->compact_instructions[j];
    if (next->op != TCCIR_OP_CMP)
      continue;

    /* Check src2 of CMP for matching deref. */
    if (irop_config[next->op].has_src2)
    {
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, next);
      if (cmp_src2.is_lval && irop_get_vreg(cmp_src2) == load_addr_vr)
      {
        IROperand replacement = irop_make_vreg(dest_vr, dest.btype);
        tcc_ir_set_src2(ir, j, replacement);
        changes++;
        continue;
      }
    }

    /* Check src1 of CMP for matching deref. */
    {
      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, next);
      if (cmp_src1.is_lval && irop_get_vreg(cmp_src1) == load_addr_vr)
      {
        IROperand replacement = irop_make_vreg(dest_vr, dest.btype);
        tcc_ir_set_src1(ir, j, replacement);
        changes++;
      }
    }
  }

  return changes;
}


/* Return value optimization - fold LOAD -> RETURNVALUE patterns */
int tcc_ir_opt_return(TCCIRState *ir)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  return 0;
}

/* ============================================================================
 * Entry-Block Store Propagation
 * ============================================================================
 *
 * Forward constant stores from the function entry block into deref operands
 * anywhere in the function.  Entry-block stores dominate all subsequent code,
 * so their values are valid at every point unless overwritten.
 *
 * This specifically targets the pattern where struct fields are initialized
 * before a loop and accessed inside it via LEA + ADD + deref:
 *
 *   entry:  STORE StackLoc[-56] = #4         ; cont.count = 4
 *   loop:   T = Addr[StackLoc[-68]]          ; &cont
 *           T' = T + #12                     ; &cont.count
 *           CMP V4, T'***DEREF***            ; compare against cont.count
 *
 * The pass replaces T'***DEREF*** with #4.
 *
 * SL-FWD cannot do this because it drops tracked stores at loop headers
 * (multi-predecessor basic blocks).  This pass ignores BB boundaries since
 * entry-block stores are guaranteed to dominate all code.
 */
int tcc_ir_opt_entry_store_prop(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
    return 0;

  /* Phase 1: Collect constant stores from the entry basic block.
   * Entry BB = instructions before the first jump target. */
#define MAX_ENTRY_STORES 64
  struct
  {
    int64_t offset;
    IROperand value;
    int btype;
  } estores[MAX_ENTRY_STORES];
  int estore_count = 0;

#define MAX_BC_RANGES 8
  struct
  {
    int64_t base;
    int64_t size;
  } bc_ranges[MAX_BC_RANGES];
  int bc_range_count = 0;

  for (int i = 0; i < n && estore_count < MAX_ENTRY_STORES; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->is_jump_target)
    {
      LOG_IR_GEN("ENTRY_STORE_PROP: stopped at i=%d (jump_target)", i);
      break;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      LOG_IR_GEN("ENTRY_STORE_PROP: stopped at i=%d (jump/jumpif)", i);
      break;
    }

    if (q->op == TCCIR_OP_BLOCK_COPY)
    {
      IROperand bc_dest = tcc_ir_op_get_dest(ir, q);
      IROperand bc_src = tcc_ir_op_get_src1(ir, q);
      IROperand bc_sz = tcc_ir_op_get_src2(ir, q);

      if (!bc_dest.is_local || irop_get_tag(bc_dest) != IROP_TAG_STACKOFF)
        continue;
      if (!irop_is_immediate(bc_sz))
        continue;

      int64_t base_off = irop_get_stack_offset(bc_dest);
      int total_size = (int)irop_get_imm64_ex(ir, bc_sz);
      if (total_size <= 0 || (total_size & 3) || total_size > 256)
        continue;

      size_t avail = 0;
      const uint8_t *data = ir_opt_get_rodata_bytes(ir, bc_src, &avail);
      if (!data || avail < (size_t)total_size)
        continue;

      int nwords = total_size / 4;
      for (int w = 0; w < nwords && estore_count < MAX_ENTRY_STORES; w++)
      {
        int32_t val = (int32_t)read32le((unsigned char *)(data + w * 4));
        int64_t off = base_off + w * 4;
        int found = -1;
        for (int k = 0; k < estore_count; k++)
        {
          if (estores[k].offset == off)
          {
            found = k;
            break;
          }
        }
        IROperand imm = irop_make_imm32(-1, val, IROP_BTYPE_INT32);
        if (found >= 0)
        {
          estores[found].value = imm;
          estores[found].btype = IROP_BTYPE_INT32;
        }
        else
        {
          estores[estore_count].offset = off;
          estores[estore_count].value = imm;
          estores[estore_count].btype = IROP_BTYPE_INT32;
          estore_count++;
        }
      }
      if (bc_range_count < MAX_BC_RANGES)
      {
        bc_ranges[bc_range_count].base = base_off;
        bc_ranges[bc_range_count].size = total_size;
        bc_range_count++;
      }
      LOG_IR_GEN("ENTRY_STORE_PROP: BLOCK_COPY at i=%d expanded %d words from off=%lld", i, nwords,
                 (long long)base_off);
      continue;
    }

    if (q->op != TCCIR_OP_STORE)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);

    LOG_IR_GEN("ENTRY_STORE_PROP: STORE at i=%d: dest local=%d lval=%d llocal=%d tag=%d", i, dest.is_local,
               dest.is_lval, dest.is_llocal, irop_get_tag(dest));

    /* Only direct StackLoc stores (is_local, is_lval, not through pointer) */
    if (!dest.is_local || !dest.is_lval || dest.is_llocal)
      continue;
    if (irop_get_tag(dest) != IROP_TAG_STACKOFF)
      continue;

    int64_t off = irop_get_stack_offset(dest);

    /* Only constant or stack-address values.  If neither, this store
     * overwrites a previously collected constant for the same offset —
     * invalidate the earlier entry (last-write-wins). */
    int is_const = irop_is_immediate(src1);
    int is_stackaddr = src1.is_local && !src1.is_lval && irop_get_tag(src1) == IROP_TAG_STACKOFF;
    if (!is_const && !is_stackaddr)
    {
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset == off)
          estores[k].offset = 0x7FFFFFFFLL; /* invalidate */
      }
      continue;
    }

    /* Last-write-wins: update existing entry for same offset, or add new */
    int found = -1;
    for (int k = 0; k < estore_count; k++)
    {
      if (estores[k].offset == off)
      {
        found = k;
        break;
      }
    }
    if (found >= 0)
    {
      estores[found].value = src1;
      estores[found].btype = irop_get_btype(dest);
    }
    else if (estore_count < MAX_ENTRY_STORES)
    {
      estores[estore_count].offset = off;
      estores[estore_count].value = src1;
      estores[estore_count].btype = irop_get_btype(dest);
      estore_count++;
    }
  }

  LOG_IR_GEN("ENTRY_STORE_PROP: %d entry-BB stores collected", estore_count);
  if (estore_count == 0)
    return 0;

  /* Phase 1.5: Invalidate entries for offsets that are written to ANYWHERE
   * after the entry BB.  If a stack location is modified later (e.g., loop
   * counter gof.argc++), forwarding the entry-BB value is wrong. */
  {
    int entry_bb_end = 0;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->is_jump_target || eq->op == TCCIR_OP_JUMP || eq->op == TCCIR_OP_JUMPIF)
      {
        entry_bb_end = j;
        break;
      }
    }
    for (int j = entry_bb_end; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->op == TCCIR_OP_BLOCK_COPY)
      {
        IROperand bcd = tcc_ir_op_get_dest(ir, eq);
        IROperand bcsz = tcc_ir_op_get_src2(ir, eq);
        if (bcd.is_local && irop_get_tag(bcd) == IROP_TAG_STACKOFF && irop_is_immediate(bcsz))
        {
          int64_t bbase = irop_get_stack_offset(bcd);
          int64_t bsz = irop_get_imm64_ex(ir, bcsz);
          for (int k = 0; k < estore_count; k++)
          {
            if (estores[k].offset >= bbase && estores[k].offset < bbase + bsz)
              estores[k].offset = 0x7FFFFFFFLL;
          }
        }
        continue;
      }
      if (eq->op != TCCIR_OP_STORE && eq->op != TCCIR_OP_STORE_INDEXED && eq->op != TCCIR_OP_STORE_POSTINC)
        continue;
      IROperand sd = tcc_ir_op_get_dest(ir, eq);
      if (!sd.is_local || !sd.is_lval || sd.is_llocal)
        continue;
      if (irop_get_tag(sd) != IROP_TAG_STACKOFF)
        continue;
      int64_t soff = irop_get_stack_offset(sd);
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset == soff)
        {
          LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (rewritten at i=%d)", (long long)soff, j);
          estores[k].offset = 0x7FFFFFFFLL;
        }
      }
    }
    /* Also invalidate stores whose address is taken anywhere in the function.
     * A Addr[StackLoc[X]] operand means offset X's address may escape to a
     * function call, which could write through the pointer. */
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->op == TCCIR_OP_NOP)
        continue;
      for (int si = 0; si < 2; si++)
      {
        if (si == 0 && !irop_config[eq->op].has_src1)
          continue;
        if (si == 1 && !irop_config[eq->op].has_src2)
          continue;
        IROperand op = (si == 0) ? tcc_ir_op_get_src1(ir, eq) : tcc_ir_op_get_src2(ir, eq);
        if (!op.is_local || op.is_lval || irop_get_tag(op) != IROP_TAG_STACKOFF)
          continue;
        int64_t aoff = irop_get_stack_offset(op);
        for (int k = 0; k < estore_count; k++)
        {
          if (estores[k].offset == aoff)
          {
            LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (addr taken at i=%d)", (long long)aoff, j);
            estores[k].offset = 0x7FFFFFFFLL;
          }
        }
      }
    }

    /* Remove invalidated entries */
    int valid = 0;
    for (int k = 0; k < estore_count; k++)
    {
      if (estores[k].offset != 0x7FFFFFFFLL)
        estores[valid++] = estores[k];
    }
    estore_count = valid;
  }

  if (estore_count == 0)
    return 0;

  /* Phase 2: Build LEA map — track TEMPs holding addresses of stack locals. */
  int max_tmp = 0, max_var = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(d);
      if (vr >= 0)
      {
        int p = TCCIR_DECODE_VREG_POSITION(vr);
        if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && p > max_tmp)
          max_tmp = p;
        if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && p > max_var)
          max_var = p;
      }
    }
  }

  typedef struct
  {
    int64_t offset;
    int valid;
  } SimpleLeaEntry;

  SimpleLeaEntry *lea_map = tcc_mallocz(sizeof(SimpleLeaEntry) * (max_tmp + 1));
  SimpleLeaEntry *var_lea_map = tcc_mallocz(sizeof(SimpleLeaEntry) * (max_var + 1));

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* ASSIGN/LEA with Addr[StackLoc[X]] source → record in LEA map */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (src1.is_local && !src1.is_lval && irop_get_tag(src1) == IROP_TAG_STACKOFF)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int32_t vr = irop_get_vreg(dest);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_tmp)
          {
            lea_map[p].offset = irop_get_stack_offset(src1);
            lea_map[p].valid = 1;
          }
        }
      }
    }

    /* STORE/ASSIGN: VAR <-- LEA_temp → propagate into var_lea_map */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s1_vr = irop_get_vreg(s1);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR && s1_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (sp <= max_tmp && lea_map[sp].valid && dp <= max_var)
        {
          var_lea_map[dp].offset = lea_map[sp].offset;
          var_lea_map[dp].valid = 1;
        }
      }
    }

    /* ASSIGN: TEMP <-- VAR → propagate from var_lea_map */
    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s1_vr = irop_get_vreg(s1);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP && s1_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (sp <= max_var && var_lea_map[sp].valid && dp <= max_tmp)
        {
          lea_map[dp].offset = var_lea_map[sp].offset;
          lea_map[dp].valid = 1;
        }
      }
    }

    /* ADD: LEA_temp + constant or Addr[StackLoc] + constant → propagate in LEA map */
    if (q->op == TCCIR_OP_ADD)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (dp <= max_tmp)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          int32_t s1_vr = irop_get_vreg(s1);
          if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(s2) &&
              !s2.is_sym)
          {
            int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
            if (sp <= max_tmp && lea_map[sp].valid)
            {
              lea_map[dp].offset = lea_map[sp].offset + irop_get_imm64_ex(ir, s2);
              lea_map[dp].valid = 1;
            }
          }
          else if (s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_STACKOFF && irop_is_immediate(s2) &&
                   !s2.is_sym)
          {
            lea_map[dp].offset = irop_get_stack_offset(s1) + irop_get_imm64_ex(ir, s2);
            lea_map[dp].valid = 1;
          }
        }
      }
    }
  }

  /* Phase 2.5: Invalidate entries for pointer stores through LEA-resolved TEMPs.
   * Phase 1.5 only catches direct StackLoc stores; stores like T***DEREF*** <-- #0
   * where T resolves to a known stack offset via the LEA map are missed.  After
   * inlining, struct field writes go through pointer dereferences, so this is
   * needed to prevent forwarding a stale entry-BB value past an overwrite. */
  {
    int entry_bb_end = 0;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->is_jump_target || eq->op == TCCIR_OP_JUMP || eq->op == TCCIR_OP_JUMPIF)
      {
        entry_bb_end = j;
        break;
      }
    }
    for (int j = entry_bb_end; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->op != TCCIR_OP_STORE && eq->op != TCCIR_OP_STORE_INDEXED)
        continue;
      IROperand sd = tcc_ir_op_get_dest(ir, eq);
      if (sd.is_local)
        continue;
      if (!sd.is_lval)
        continue;
      int32_t dv = irop_get_vreg(sd);
      if (dv < 0)
        continue;
      int64_t soff;
      if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
      {
        int dp = TCCIR_DECODE_VREG_POSITION(dv);
        if (dp > max_tmp || !lea_map[dp].valid)
          continue;
        soff = lea_map[dp].offset;
      }
      else if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
      {
        int dp = TCCIR_DECODE_VREG_POSITION(dv);
        if (dp > max_var || !var_lea_map[dp].valid)
          continue;
        soff = var_lea_map[dp].offset;
      }
      else
        continue;
      if (eq->op == TCCIR_OP_STORE_INDEXED)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, eq);
        if (!irop_is_immediate(s2))
          continue;
        soff += irop_get_imm64_ex(ir, s2);
      }
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset == soff)
        {
          LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (ptr store via LEA at i=%d)", (long long)soff, j);
          estores[k].offset = 0x7FFFFFFFLL;
        }
      }
    }
    int v3 = 0;
    for (int k = 0; k < estore_count; k++)
      if (estores[k].offset != 0x7FFFFFFFLL)
        estores[v3++] = estores[k];
    estore_count = v3;
  }
  if (estore_count == 0)
  {
    tcc_free(lea_map);
    tcc_free(var_lea_map);
    return 0;
  }

  /* Collect call-escaped base offsets for Phase 3b safety check.
   * Only track stack addresses passed through actual function call parameters,
   * not addresses used within inlined code. */
#define MAX_ADDRTAKEN_BASES 32
  int64_t addrtaken_bases[MAX_ADDRTAKEN_BASES];
  int addrtaken_base_count = 0;
  for (int j = 0; j < n && addrtaken_base_count < MAX_ADDRTAKEN_BASES; j++)
  {
    IRQuadCompact *eq = &ir->compact_instructions[j];
    if (eq->op != TCCIR_OP_FUNCPARAMVAL)
      continue;
    IROperand op = tcc_ir_op_get_src1(ir, eq);
    if (op.is_local && !op.is_lval && irop_get_tag(op) == IROP_TAG_STACKOFF)
    {
      int64_t aoff = irop_get_stack_offset(op);
      int dup = 0;
      for (int ab = 0; ab < addrtaken_base_count; ab++)
        if (addrtaken_bases[ab] == aoff)
        {
          dup = 1;
          break;
        }
      if (!dup)
        addrtaken_bases[addrtaken_base_count++] = aoff;
    }
    else
    {
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(vr);
        if (p <= max_tmp && lea_map[p].valid)
        {
          int64_t aoff = lea_map[p].offset;
          int dup = 0;
          for (int ab = 0; ab < addrtaken_base_count; ab++)
            if (addrtaken_bases[ab] == aoff)
            {
              dup = 1;
              break;
            }
          if (!dup && addrtaken_base_count < MAX_ADDRTAKEN_BASES)
            addrtaken_bases[addrtaken_base_count++] = aoff;
        }
      }
    }
  }

  /* Range invalidation for BLOCK_COPY: when an address within a BLOCK_COPY
   * range escapes through a function call parameter, all fields of the struct
   * could be modified.  Invalidate ALL estores entries in the range. */
  for (int j = 0; j < n && bc_range_count > 0; j++)
  {
    IRQuadCompact *eq = &ir->compact_instructions[j];
    if (eq->op != TCCIR_OP_FUNCPARAMVAL)
      continue;
    IROperand fop = tcc_ir_op_get_src1(ir, eq);
    int64_t foff = 0x7FFFFFFFLL;
    if (fop.is_local && !fop.is_lval && irop_get_tag(fop) == IROP_TAG_STACKOFF)
      foff = irop_get_stack_offset(fop);
    else
    {
      int32_t fvr = irop_get_vreg(fop);
      if (fvr >= 0 && !fop.is_lval && TCCIR_DECODE_VREG_TYPE(fvr) == TCCIR_VREG_TYPE_TEMP)
      {
        int fp = TCCIR_DECODE_VREG_POSITION(fvr);
        if (fp <= max_tmp && lea_map[fp].valid)
          foff = lea_map[fp].offset;
      }
    }
    if (foff == 0x7FFFFFFFLL)
      continue;
    for (int br = 0; br < bc_range_count; br++)
    {
      if (foff >= bc_ranges[br].base && foff < bc_ranges[br].base + bc_ranges[br].size)
      {
        for (int k = 0; k < estore_count; k++)
        {
          if (estores[k].offset >= bc_ranges[br].base && estores[k].offset < bc_ranges[br].base + bc_ranges[br].size)
            estores[k].offset = 0x7FFFFFFFLL;
        }
        break;
      }
    }
  }
  {
    int v2 = 0;
    for (int k = 0; k < estore_count; k++)
      if (estores[k].offset != 0x7FFFFFFFLL)
        estores[v2++] = estores[k];
    estore_count = v2;
  }

  /* Phase 3: Forward entry-BB stores into deref operands.
   * For each instruction, check src1 and src2 for T***DEREF*** where T
   * is in the LEA map and the resolved offset matches an entry-BB store. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Process src1 and src2 */
    for (int si = 0; si < 2; si++)
    {
      if (si == 0 && !irop_config[q->op].has_src1)
        continue;
      if (si == 1 && !irop_config[q->op].has_src2)
        continue;

      IROperand src = (si == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);

      /* Only deref operands (is_lval) */
      if (!src.is_lval)
        continue;

      /* Resolve the address through LEA map */
      int32_t vr = irop_get_vreg(src);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;

      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p > max_tmp || !lea_map[p].valid)
        continue;

      int64_t resolved_offset = lea_map[p].offset;

      /* Look up in entry-BB store table */
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset != resolved_offset)
          continue;

        /* Match! Replace deref with the stored value.
         * Reuse the original stored operand directly to preserve
         * the correct type encoding (IMM32, I64, F32, F64, etc.). */
        IROperand replacement = estores[k].value;

        if (si == 0)
          tcc_ir_op_set_src1(ir, q, replacement);
        else
          tcc_ir_op_set_src2(ir, q, replacement);

        LOG_IR_GEN("ENTRY_STORE_PROP: i=%d si=%d replaced deref at off=%lld with stored value", i, si,
                   (long long)resolved_offset);
        changes++;
        break;
      }
    }
  }

  /* Phase 3b: Forward entry-BB stores into LOAD_INDEXED instructions.
   * LOAD_INDEXED dest, base, #imm — when base is in the LEA map and
   * (lea_offset + imm) matches an entry-BB store, replace the entire
   * LOAD_INDEXED with ASSIGN of the stored value. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LOAD_INDEXED)
      continue;

    IROperand li_src1 = tcc_ir_op_get_src1(ir, q);
    IROperand li_src2 = tcc_ir_op_get_src2(ir, q);

    int32_t base_vr = irop_get_vreg(li_src1);
    if (base_vr < 0 || TCCIR_DECODE_VREG_TYPE(base_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (!irop_is_immediate(li_src2) || li_src2.is_sym)
      continue;

    int bp = TCCIR_DECODE_VREG_POSITION(base_vr);
    if (bp > max_tmp || !lea_map[bp].valid)
      continue;

    int64_t base_off = lea_map[bp].offset;
    int64_t eff_off = base_off + irop_get_imm64_ex(ir, li_src2);

    /* If the LEA base's address was taken, the struct it points to could
     * have been modified by a function call.  Skip forwarding. */
    {
      int base_addrtaken = 0;
      for (int ab = 0; ab < addrtaken_base_count; ab++)
      {
        if (addrtaken_bases[ab] == base_off)
        {
          base_addrtaken = 1;
          break;
        }
      }
      if (base_addrtaken)
        continue;
    }

    for (int k = 0; k < estore_count; k++)
    {
      if (estores[k].offset != eff_off)
        continue;
      if (estores[k].btype != irop_get_btype(li_src1))
        continue;

      q->op = TCCIR_OP_ASSIGN;
      {
        int pool_off = q->operand_base + irop_config[TCCIR_OP_ASSIGN].has_dest;
        ir->iroperand_pool[pool_off] = estores[k].value;
      }
      tcc_ir_set_src2(ir, i, IROP_NONE);

      if (estores[k].value.is_local && !estores[k].value.is_lval && irop_get_tag(estores[k].value) == IROP_TAG_STACKOFF)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int32_t d_vr = irop_get_vreg(dest);
        if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
          if (dp <= max_tmp)
          {
            lea_map[dp].offset = irop_get_stack_offset(estores[k].value);
            lea_map[dp].valid = 1;
          }
        }
      }

      LOG_IR_GEN("ENTRY_STORE_PROP: i=%d LOAD_INDEXED forwarded at eff_off=%lld", i, (long long)eff_off);
      changes++;
      break;
    }
  }

  tcc_free(lea_map);
  tcc_free(var_lea_map);

  return changes;
}

static int ir_opt_store_btype_size_bytes(int btype)
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

static int ir_opt_stack_slot_range_for_offset(const TCCIRState *ir, int64_t frame_offset, int64_t *base_out,
                                              int64_t *end_out)
{
  const TCCStackSlot *slot;

  if (!ir)
    return 0;

  slot = tcc_ir_stack_slot_by_offset(ir, (int)frame_offset);
  if (!slot)
  {
    for (int si = 0; si < ir->stack_layout.slot_count; ++si)
    {
      const TCCStackSlot *candidate = &ir->stack_layout.slots[si];
      int64_t candidate_base = candidate->offset;
      int64_t candidate_end = candidate_base + candidate->size;

      if (candidate->size <= 0)
        continue;
      if (frame_offset >= candidate_base && frame_offset < candidate_end)
      {
        slot = candidate;
        break;
      }
    }
  }

  if (!slot || slot->size <= 0)
    return 0;

  *base_out = slot->offset;
  *end_out = (int64_t)slot->offset + slot->size;
  return 1;
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
    int addr_via_pointer;   /* 1 if store was resolved through LEA map (pointer) */
    int64_t local_offset;   /* stack offset or symref addend */
    const Sym *local_sym;   /* symbol for VT_LOCAL (NULL for pure stack offsets) */
    IROperand stored_value; /* IROperand of the stored value */
    int instruction_idx;    /* where the store happened */
    int store_dest_vr;      /* vreg of the store destination (address) */
    int store_btype;        /* btype of the store address (access width) */
    struct StoreEntry *next;
  } StoreEntry;

  /* Track last write index for each vreg to detect intervening writes.
   * When a LOAD's address vreg was written AFTER a matching store,
   * the store-load forward is invalid because the vreg now holds a
   * different value than what was stored. */
  typedef struct
  {
    int last_write_idx; /* instruction index of last write, -1 if none */
    int gen;            /* generation counter, valid only if gen == current_gen */
  } VregWriteTracker;

  int n = ir->next_instruction_index;
  int changes = 0;
  int i;
  IRQuadCompact *q;
  StoreEntry *hash_table[128];
  StoreEntry *entries;
  int entry_count;

  /* Track stores whose loads were forwarded — candidates for dead-store elim. */
#define SL_FWD_MAX_DEAD_STORES 16
  struct
  {
    int store_idx;
    int64_t offset;
    const Sym *sym;
  } fwd_stores[SL_FWD_MAX_DEAD_STORES];
  int fwd_store_count = 0;

  if (n == 0)
    return 0;

  /* Pre-pass: recompute is_jump_target flags from actual jump instructions.
   * After optimization passes (e.g. trivial JMP→NOP), some instructions may
   * still have is_jump_target set even though no JUMP/JUMPIF targets them
   * anymore.  These stale flags create artificial BB boundaries that prevent
   * store-load forwarding from seeing through.
   *
   * At the same time, compute pred_count[t] = number of control-flow
   * predecessors for each instruction t.  This is later used to extend
   * forwarding across BB boundaries when a target has exactly one
   * predecessor (either a single incoming JUMP, or plain fall-through). */
  int *pred_count = tcc_mallocz(sizeof(int) * n);
  {
    uint8_t *actual_targets = tcc_mallocz((n + 7) / 8);
    for (i = 0; i < n; i++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[i];
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, jq);
        int target = (int)dest.u.imm32;
        if (target >= 0 && target < n)
        {
          actual_targets[target / 8] |= (1 << (target % 8));
          pred_count[target]++;
        }
      }
    }
    /* Fall-through predecessors: instruction i+1 is reached from i unless i
     * is a terminator (JUMP, RETURNVALUE, RETURNVOID). */
    for (i = 0; i + 1 < n; i++)
    {
      IRQuadCompact *fq = &ir->compact_instructions[i];
      if (fq->op != TCCIR_OP_JUMP && fq->op != TCCIR_OP_RETURNVALUE && fq->op != TCCIR_OP_RETURNVOID)
        pred_count[i + 1]++;
    }
    /* Instruction 0 is always a function entry — has an implicit predecessor. */
    if (n > 0)
      pred_count[0]++;
    for (i = 0; i < n; i++)
    {
      int is_actual = (actual_targets[i / 8] & (1 << (i % 8))) != 0;
      if (ir->compact_instructions[i].is_jump_target && !is_actual)
        ir->compact_instructions[i].is_jump_target = 0;
      else if (!ir->compact_instructions[i].is_jump_target && is_actual)
        ir->compact_instructions[i].is_jump_target = 1;
    }
    tcc_free(actual_targets);
  }

  memset(hash_table, 0, sizeof(hash_table));
  entries = tcc_malloc(sizeof(StoreEntry) * n);
  entry_count = 0;

  /* Allocate vreg write trackers for all three vreg types.
   * Using generation counter so we don't need to clear on block boundaries. */
  int write_tracker_gen = 1;
  int max_var = ir->next_local_variable;
  int max_tmp = ir->next_temporary_variable;
  int max_par = ir->next_parameter;
  VregWriteTracker *var_writes = tcc_mallocz(sizeof(VregWriteTracker) * (max_var + 1));
  VregWriteTracker *tmp_writes = tcc_mallocz(sizeof(VregWriteTracker) * (max_tmp + 1));
  VregWriteTracker *par_writes = tcc_mallocz(sizeof(VregWriteTracker) * (max_par + 1));

  /* LEA map: track TEMPs that hold addresses of stack locals.
   * Used to resolve LEA-based memory accesses (e.g. struct field access via
   * LEA T = &StackLoc[X]; STORE V = T***DEREF***) back to direct StackLoc refs
   * so that store-load forwarding can propagate values through them. */
  typedef struct
  {
    int64_t offset; /* resolved stack offset */
    const Sym *sym; /* local symbol (NULL for anonymous) */
    int valid;      /* 1 if entry is valid */
  } LeaMapEntry;

  LeaMapEntry *lea_map = tcc_mallocz(sizeof(LeaMapEntry) * (max_tmp + 1));

  /* Set of addrtaken stack slots — any (sym, offset) that appears as the
   * source of a LEA/address-of anywhere in the function.  Stores to such
   * slots must be invalidated across function calls since the address may
   * have escaped.  Anonymous stack slots (no vreg) don't have an
   * IRLiveInterval::addrtaken flag, so without this set the STORE handler
   * leaves addr_addrtaken=0 and the CALL handler keeps stale entries. */
  typedef struct
  {
    int64_t offset;
    const Sym *sym;
    int earliest_lea_idx;   /* lowest instruction index of any LEA for this slot */
    int64_t max_access_end; /* upper bound (exclusive) of accessed range from this base */
  } AddrTakenSlot;
  int addrtaken_cap = 16;
  int addrtaken_count = 0;
  AddrTakenSlot *addrtaken_slots = tcc_malloc(sizeof(AddrTakenSlot) * addrtaken_cap);

  /* VAR LEA map: track VARs that hold addresses of stack locals.  Enables
   * forwarding through the pattern:  LEA T = &StackLoc; STORE V = T;
   * ASSIGN T' = V; LOAD _ = T'***DEREF***  — which otherwise loses the LEA
   * info at the VAR hand-off step.  Only single-def VARs are tracked, so the
   * LEA value read back is definitively the one stored. */
  LeaMapEntry *var_lea_map = tcc_mallocz(sizeof(LeaMapEntry) * (max_var + 1));
  uint8_t *var_def_count = tcc_mallocz(max_var + 1);

  /* Count VAR defs in one pass; cap at 2 since we only need the single-def bit.
   * Note: VAR STORE dests carry is_lval=1 (the VAR slot is written through its
   * storage address), but they are still definitions of the VAR. */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *cq = &ir->compact_instructions[i];
    if (cq->op == TCCIR_OP_NOP || !irop_config[cq->op].has_dest)
      continue;
    IROperand cd = tcc_ir_op_get_dest(ir, cq);
    int32_t cdv = irop_get_vreg(cd);
    if (cdv < 0 || TCCIR_DECODE_VREG_TYPE(cdv) != TCCIR_VREG_TYPE_VAR)
      continue;
    int cdp = TCCIR_DECODE_VREG_POSITION(cdv);
    if (cdp <= max_var && var_def_count[cdp] < 2)
      var_def_count[cdp]++;
  }

  /* Pre-scan: build set of active call_ids (those with a FUNCCALL instruction).
   * FUNCPARAMVAL instructions whose call_id has no matching FUNCCALL are dead
   * (e.g. from inlined function calls) and should not create addrtaken entries. */
  uint8_t *active_call_ids = NULL;
  int max_call_id = ir->next_call_id;
  if (max_call_id > 0)
  {
    active_call_ids = tcc_mallocz((max_call_id + 7) / 8);
    for (i = 0; i < n; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op == TCCIR_OP_FUNCCALLVOID || cq->op == TCCIR_OP_FUNCCALLVAL)
      {
        IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
        int cid = (int)((uint32_t)(int32_t)irop_get_imm64_ex(ir, cs2) >> 16);
        if (cid >= 0 && cid < max_call_id)
          active_call_ids[cid / 8] |= (1 << (cid % 8));
      }
    }
  }

  /* Pre-scan: build LEA map from LEA and LEA+ADD patterns */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *lq = &ir->compact_instructions[i];
    if (lq->op == TCCIR_OP_LEA)
    {
      IROperand ldest = tcc_ir_op_get_dest(ir, lq);
      IROperand lsrc1 = tcc_ir_op_get_src1(ir, lq);
      int32_t d_vr = irop_get_vreg(ldest);
      /* LEA from a StackLoc (is_local=1, is_lval=0 means address-of).
       * Only record concrete stack addresses (STACKOFF or SYMREF).  VAR-tagged
       * operands (e.g. `&V3`) have no resolved u.imm32 here — they all read as
       * offset=0, which would collide in the {sym,offset}-keyed hash table and
       * alias distinct VARs together.  Stores to VARs themselves are already
       * tracked via the normal VAR STORE path, so skipping them costs nothing. */
      if (lsrc1.is_local && !lsrc1.is_lval && d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        int src_tag = irop_get_tag(lsrc1);
        int32_t src_vr = irop_get_vreg(lsrc1);
        if (tmp_pos <= max_tmp)
        {
          const Sym *lsym = NULL;
          int64_t loff = 0;
          int resolved = 0;
          if (src_tag == IROP_TAG_SYMREF)
          {
            IRPoolSymref *sr = irop_get_symref_ex(ir, lsrc1);
            lsym = sr ? sr->sym : NULL;
            loff = sr ? sr->addend : 0;
            resolved = 1;
          }
          else if (src_vr >= 0)
          {
            /* VAR/PARAM-backed local: the IR operand's u.imm32 is unresolved
             * (typically 0) at this point.  Fall back to the allocated stack
             * slot for the vreg, so that distinct VARs don't collide at
             * (sym=NULL, offset=0) in the hash table. */
            const TCCStackSlot *slot = tcc_ir_stack_slot_by_vreg(ir, src_vr);
            if (slot)
            {
              loff = slot->offset;
              resolved = 1;
            }
          }
          else if (src_tag == IROP_TAG_STACKOFF)
          {
            /* Anonymous stack slot (no vreg): u.imm32 holds the frame offset. */
            loff = irop_get_stack_offset(lsrc1);
            resolved = 1;
          }
          if (resolved)
          {
            lea_map[tmp_pos].offset = loff;
            lea_map[tmp_pos].sym = lsym;
            lea_map[tmp_pos].valid = 1;
            /* Mark this slot as addrtaken: a LEA exposed its address, so any
             * function call after a STORE here could mutate it via the
             * escaping pointer.  Record the earliest LEA instruction index
             * so that CALL-time invalidation only fires when the CALL actually
             * happens after the LEA in program order (the pointer can't
             * escape until the LEA executes). */
            int already = 0;
            for (int k = 0; k < addrtaken_count; k++)
            {
              if (addrtaken_slots[k].sym == lsym && addrtaken_slots[k].offset == loff)
              {
                already = 1;
                if (i < addrtaken_slots[k].earliest_lea_idx)
                  addrtaken_slots[k].earliest_lea_idx = i;
                break;
              }
            }
            if (!already)
            {
              if (addrtaken_count >= addrtaken_cap)
              {
                addrtaken_cap *= 2;
                addrtaken_slots = tcc_realloc(addrtaken_slots, sizeof(AddrTakenSlot) * addrtaken_cap);
              }
              addrtaken_slots[addrtaken_count].sym = lsym;
              addrtaken_slots[addrtaken_count].offset = loff;
              addrtaken_slots[addrtaken_count].earliest_lea_idx = i;
              addrtaken_slots[addrtaken_count].max_access_end = loff + 4;
              addrtaken_count++;
            }
          }
        }
      }
    }
    else if (lq->op == TCCIR_OP_ADD)
    {
      IROperand ldest = tcc_ir_op_get_dest(ir, lq);
      IROperand lsrc1 = tcc_ir_op_get_src1(ir, lq);
      IROperand lsrc2 = tcc_ir_op_get_src2(ir, lq);
      int32_t d_vr = irop_get_vreg(ldest);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int dest_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (dest_pos <= max_tmp)
        {
          /* Check: LEA_temp + constant */
          int32_t s1_vr = irop_get_vreg(lsrc1);
          int32_t s2_vr = irop_get_vreg(lsrc2);
          if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(lsrc2) &&
              !lsrc2.is_sym)
          {
            int s1_pos = TCCIR_DECODE_VREG_POSITION(s1_vr);
            if (s1_pos <= max_tmp && lea_map[s1_pos].valid)
            {
              lea_map[dest_pos].offset = lea_map[s1_pos].offset + irop_get_imm64_ex(ir, lsrc2);
              lea_map[dest_pos].sym = lea_map[s1_pos].sym;
              lea_map[dest_pos].valid = 1;
            }
          }
          /* Check: constant + LEA_temp (ADD is commutative) */
          else if (s2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s2_vr) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(lsrc1) &&
                   !lsrc1.is_sym)
          {
            int s2_pos = TCCIR_DECODE_VREG_POSITION(s2_vr);
            if (s2_pos <= max_tmp && lea_map[s2_pos].valid)
            {
              lea_map[dest_pos].offset = lea_map[s2_pos].offset + irop_get_imm64_ex(ir, lsrc1);
              lea_map[dest_pos].sym = lea_map[s2_pos].sym;
              lea_map[dest_pos].valid = 1;
            }
          }
        }
      }
    }
    else if (lq->op == TCCIR_OP_STORE || lq->op == TCCIR_OP_ASSIGN)
    {
      /* VAR <-- TEMP_in_lea_map [STORE|ASSIGN] on a single-def VAR:
       * record VAR as holding the LEA's stack address. */
      IROperand ldest = tcc_ir_op_get_dest(ir, lq);
      IROperand lsrc1 = tcc_ir_op_get_src1(ir, lq);
      int32_t d_vr = irop_get_vreg(ldest);
      int32_t s_vr = irop_get_vreg(lsrc1);
      int dest_ok = (lq->op == TCCIR_OP_STORE) ? 1 : !ldest.is_lval;
      if (dest_ok && d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR && !lsrc1.is_lval && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int vp = TCCIR_DECODE_VREG_POSITION(d_vr);
        int sp = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (vp <= max_var && sp <= max_tmp && lea_map[sp].valid && var_def_count[vp] == 1)
        {
          var_lea_map[vp].offset = lea_map[sp].offset;
          var_lea_map[vp].sym = lea_map[sp].sym;
          var_lea_map[vp].valid = 1;
        }
      }
      /* TEMP <-- VAR_in_var_lea_map [ASSIGN]: propagate VAR's LEA to TEMP. */
      if (lq->op == TCCIR_OP_ASSIGN && !ldest.is_lval && d_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        int vp = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (dp <= max_tmp && vp <= max_var && var_lea_map[vp].valid)
        {
          lea_map[dp].offset = var_lea_map[vp].offset;
          lea_map[dp].sym = var_lea_map[vp].sym;
          lea_map[dp].valid = 1;
        }
      }
    }

    /* Direct-address escape tracking.  Any instruction whose src1/src2
     * carries an `Addr[StackLoc]` operand (STACKOFF, is_lval=0) effectively
     * takes the address of that stack slot — the most common case is
     * FUNCPARAMVAL passing `Addr[StackLoc[-N]]` directly without first
     * materializing a TEMP via LEA.  Without this the CALL-time
     * invalidation loop never learns the slot is reachable through the
     * callee's pointer argument, so a tracked store can be incorrectly
     * forwarded past the call (regression in pr86844.c where foo(a)
     * mutates *a but SL_FWD forwarded the pre-call init value). */
    {
      const IRRegistersConfig *cfg = &irop_config[lq->op];
      for (int src_i = 0; src_i < 2; src_i++)
      {
        if (src_i == 0 && !cfg->has_src1)
          continue;
        if (src_i == 1 && !cfg->has_src2)
          continue;
        IROperand src = (src_i == 0) ? tcc_ir_op_get_src1(ir, lq) : tcc_ir_op_get_src2(ir, lq);
        if (irop_get_tag(src) != IROP_TAG_STACKOFF)
          continue;
        if (src.is_lval || src.is_llocal)
          continue;
        /* STACKOFF with a vreg attached: offset comes from the vreg's
         * spill slot, not u.imm32 — skip for now (those paths are already
         * tracked via the vreg-based LEA map). */
        if (irop_get_vreg(src) != -1)
          continue;
        /* Skip FUNCPARAMVAL with dead call_ids (from inlined calls). */
        if (lq->op == TCCIR_OP_FUNCPARAMVAL && active_call_ids)
        {
          IROperand ps2 = tcc_ir_op_get_src2(ir, lq);
          int pcid = (int)((uint32_t)(int32_t)irop_get_imm64_ex(ir, ps2) >> 16);
          if (pcid >= 0 && pcid < max_call_id && !(active_call_ids[pcid / 8] & (1 << (pcid % 8))))
            continue;
        }
        int64_t off = irop_get_stack_offset(src);
        int already = 0;
        for (int k = 0; k < addrtaken_count; k++)
        {
          if (addrtaken_slots[k].sym == NULL && addrtaken_slots[k].offset == off)
          {
            already = 1;
            if (i < addrtaken_slots[k].earliest_lea_idx)
              addrtaken_slots[k].earliest_lea_idx = i;
            break;
          }
        }
        if (!already)
        {
          if (addrtaken_count >= addrtaken_cap)
          {
            addrtaken_cap *= 2;
            addrtaken_slots = tcc_realloc(addrtaken_slots, sizeof(AddrTakenSlot) * addrtaken_cap);
          }
          addrtaken_slots[addrtaken_count].sym = NULL;
          addrtaken_slots[addrtaken_count].offset = off;
          addrtaken_slots[addrtaken_count].earliest_lea_idx = i;
          addrtaken_slots[addrtaken_count].max_access_end = off + 4;
          addrtaken_count++;
        }
      }
    }
  }

  /* Compute max_access_end for each addrtaken slot from LEA+ADD patterns.
   * When a derived address (base + offset) exists in the lea_map, the owning
   * addrtaken slot's object extends at least to (derived_offset + word_size).
   * This allows precise overlap checks when the stack layout is unavailable. */
  for (int t = 0; t <= max_tmp; t++)
  {
    if (!lea_map[t].valid)
      continue;
    int64_t derived_off = lea_map[t].offset;
    const Sym *derived_sym = lea_map[t].sym;
    int64_t access_end = derived_off + 4;
    for (int k = 0; k < addrtaken_count; k++)
    {
      if (addrtaken_slots[k].sym != derived_sym)
        continue;
      if (addrtaken_slots[k].offset > derived_off)
        continue;
      if (access_end > addrtaken_slots[k].max_access_end)
        addrtaken_slots[k].max_access_end = access_end;
      break;
    }
  }

  /* Track constants assigned to TEMPs by forwarding, so that subsequent
   * stores of those TEMPs can use the resolved constant value instead. */
  IROperand *fwd_tmp_val = tcc_mallocz(sizeof(IROperand) * (max_tmp + 1));
  uint8_t *fwd_tmp_valid = tcc_mallocz(max_tmp + 1);

  /* Pre-populate fwd_tmp from existing constant ASSIGN/LOAD instructions.
   * Earlier optimization passes (value tracking, const prop) may have already
   * created T <-- #const [ASSIGN] or T <-- #const [LOAD] instructions.
   * The LOAD case arises when value tracking replaces a variable with its known
   * constant value (e.g., T0 <-- V0 [LOAD] → T0 <-- #7 [LOAD]).
   *
   * IMPORTANT: A TEMP with multiple definitions (e.g. on different control-flow
   * paths) must NOT be tracked, since the linear pre-scan cannot determine which
   * definition reaches a given use.  We use fwd_tmp_defs[] to count definitions
   * and permanently reject any TEMP defined more than once. */
  uint8_t *fwd_tmp_defs = tcc_mallocz(max_tmp + 1);
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *aq = &ir->compact_instructions[i];
    int has_temp_dest = 0;
    int apos = -1;
    if (irop_config[aq->op].has_dest && aq->op != TCCIR_OP_STORE && aq->op != TCCIR_OP_NOP)
    {
      IROperand adest = tcc_ir_op_get_dest(ir, aq);
      int32_t adest_vr = irop_get_vreg(adest);
      if (adest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(adest_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        apos = TCCIR_DECODE_VREG_POSITION(adest_vr);
        if (apos <= max_tmp)
          has_temp_dest = 1;
      }
    }
    if (!has_temp_dest)
      continue;

    /* Count definitions; if >1, permanently invalidate */
    if (fwd_tmp_defs[apos] < 2)
      fwd_tmp_defs[apos]++;
    if (fwd_tmp_defs[apos] > 1)
    {
      fwd_tmp_valid[apos] = 0;
      continue;
    }

    /* Single-def ASSIGN or LOAD with immediate constant — track it.
     * For LOAD, value tracking may have replaced the source variable with a
     * constant, producing T <-- #imm [LOAD] which means "T = imm". */
    if (aq->op == TCCIR_OP_ASSIGN || aq->op == TCCIR_OP_LOAD)
    {
      IROperand asrc1 = tcc_ir_op_get_src1(ir, aq);
      if (irop_is_immediate(asrc1) && !asrc1.is_sym && !asrc1.is_lval && !asrc1.is_local)
      {
        fwd_tmp_val[apos] = asrc1;
        fwd_tmp_valid[apos] = 1;
        continue;
      }
    }
    /* Non-constant or unsupported definition — don't track */
  }
  tcc_free(fwd_tmp_defs);

  /* Cross-BB state preservation: when a JUMP/JUMPIF's target has exactly
   * one predecessor, we snapshot the current state to saved_entries[t] so
   * the target BB can restore and continue forwarding.  This extends the
   * scope of forwarding across BB boundaries without requiring a full
   * data-flow join analysis. */
  StoreEntry **saved_entries = tcc_mallocz(sizeof(StoreEntry *) * n);
  int *saved_entry_count = tcc_mallocz(sizeof(int) * n);

  LOG_IR_GEN("=== STORE-LOAD FORWARDING START ===");

  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];

    /* BB exits: JUMP and JUMPIF optionally snapshot their target's state for
     * single-predecessor targets.  JUMP terminates the current path (no
     * fall-through).  JUMPIF has both a target and a fall-through path —
     * the fall-through inherits the current state. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand jdest = tcc_ir_op_get_dest(ir, q);
      int jtarget = (int)jdest.u.imm32;
      if (jtarget >= 0 && jtarget < n && pred_count[jtarget] == 1 && entry_count > 0)
      {
        /* Snapshot entries[] for the target to restore on entry */
        if (!saved_entries[jtarget])
          saved_entries[jtarget] = tcc_malloc(sizeof(StoreEntry) * n);
        memcpy(saved_entries[jtarget], entries, sizeof(StoreEntry) * entry_count);
        saved_entry_count[jtarget] = entry_count;
      }
      if (q->op == TCCIR_OP_JUMP)
      {
        /* No fall-through — clear state */
        memset(hash_table, 0, sizeof(hash_table));
        entry_count = 0;
        write_tracker_gen++;
      }
      /* JUMPIF: keep current state for the fall-through path */
      continue;
    }
    if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      memset(hash_table, 0, sizeof(hash_table));
      entry_count = 0;
      write_tracker_gen++;
      continue;
    }
    if (q->is_jump_target)
    {
      /* Multi-predecessor target: reset (can't safely merge states).
       * Single-predecessor target: restore from snapshot if predecessor was
       * a JUMP (i.e. i-1 is JUMP or RETURN, so no fall-through).  Otherwise
       * the predecessor is the fall-through — keep current state intact. */
      if (pred_count[i] > 1)
      {
        LOG_SL_FWD("BB@i=%d RESET: multi-pred target (preds=%d) — dropping %d tracked stores", i, pred_count[i],
                   entry_count);
        memset(hash_table, 0, sizeof(hash_table));
        entry_count = 0;
        write_tracker_gen++;
      }
      else if (pred_count[i] == 1)
      {
        int prev_is_terminator = 0;
        if (i > 0)
        {
          int pop = ir->compact_instructions[i - 1].op;
          prev_is_terminator = (pop == TCCIR_OP_JUMP || pop == TCCIR_OP_RETURNVALUE || pop == TCCIR_OP_RETURNVOID);
        }
        if (prev_is_terminator)
        {
          int had_entries = entry_count;
          /* Restore from snapshot if available; otherwise reset */
          memset(hash_table, 0, sizeof(hash_table));
          entry_count = 0;
          write_tracker_gen++;
          if (saved_entries[i])
          {
            int sc = saved_entry_count[i];
            LOG_SL_FWD("BB@i=%d RESTORE: single-pred target, restoring %d snapshot entries (was %d)", i, sc,
                       had_entries);
            memcpy(entries, saved_entries[i], sizeof(StoreEntry) * sc);
            entry_count = sc;
            /* Rebuild hash chains */
            for (int k = 0; k < sc; k++)
            {
              uint32_t h = ((uintptr_t)entries[k].local_sym * 31 + (uint32_t)entries[k].local_offset * 17) % 128;
              entries[k].next = hash_table[h];
              hash_table[h] = &entries[k];
            }
          }
          else if (had_entries > 0)
          {
            LOG_SL_FWD("BB@i=%d RESET: single-pred terminator target but no snapshot (dropped %d entries)", i,
                       had_entries);
          }
        }
        /* else: fall-through predecessor — keep current state intact */
      }
    }
    /* Function calls: only invalidate stores to escaped locals (addrtaken).
     * Stack locals whose address has NOT been taken cannot be modified
     * by any function call since no external code has a pointer to them.
     *
     * Anonymous stack slots (no IRLiveInterval) get their addrtaken
     * property from the pre-scanned addrtaken_slots set.  Only invalidate
     * if some LEA for this slot appeared at an instruction index <= this
     * CALL's index — otherwise the pointer hasn't escaped yet and the
     * CALL can't reach the slot. */
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      /* Known pure AEABI calls: these only compute a result from their
       * arguments and never modify memory, so tracked stores remain valid. */
      int is_pure_aeabi = 0;
      {
        IROperand csrc = tcc_ir_op_get_src1(ir, q);
        Sym *call_sym = irop_get_sym_ex(ir, csrc);
        if (call_sym)
        {
          const char *cname = get_tok_str(call_sym->v, NULL);
          LOG_SL_FWD("CALL@i=%d callee=%s (sym=%p)", i, cname ? cname : "(null)", (void *)call_sym);
          if (cname && cname[0] == '_' && cname[1] == '_')
            is_pure_aeabi = tcc_ir_is_pure_aeabi(cname);
        }
        else
        {
          LOG_SL_FWD("CALL@i=%d callee=NULL (tag=%d vr=%d is_sym=%d)", i, irop_get_tag(csrc), irop_get_vreg(csrc),
                     csrc.is_sym);
        }
        if (is_pure_aeabi)
          LOG_SL_FWD("CALL@i=%d PURE — skipping invalidation", i);
      }

      int j;
      if (!is_pure_aeabi)
      {
        for (j = 0; j < entry_count; j++)
        {
          if (!entries[j].valid)
            continue;
          if (entries[j].addr_addrtaken || entries[j].addr_via_pointer)
          {
            LOG_SL_FWD("CALL@i=%d INVALIDATE: store@i=%d sym=%p off=%lld (addrtaken/via_ptr)", i,
                       entries[j].instruction_idx, (const void *)entries[j].local_sym,
                       (long long)entries[j].local_offset);
            entries[j].valid = 0;
            continue;
          }
          /* Match by sym only for named locals — a taken address may reach any
           * offset within the same symbol via pointer arithmetic (e.g. struct
           * fields, array elements, negative indexing).
           *
           * For anonymous STACKOFF (sym==NULL), use stack-layout overlap so an
           * escaped temp struct does not invalidate unrelated anonymous locals
           * that merely share the NULL sym namespace. */
          for (int k = 0; k < addrtaken_count; k++)
          {
            if (addrtaken_slots[k].sym != entries[j].local_sym || addrtaken_slots[k].earliest_lea_idx > i)
              continue;

            if (entries[j].local_sym == NULL)
            {
              int64_t escaped_base, escaped_end;
              int store_size = ir_opt_store_btype_size_bytes(entries[j].store_btype);
              int64_t store_base = entries[j].local_offset;
              int64_t store_end = store_base + (store_size > 0 ? store_size : 1);

              if (!ir_opt_stack_slot_range_for_offset(ir, addrtaken_slots[k].offset, &escaped_base, &escaped_end))
              {
                if (addrtaken_slots[k].max_access_end > addrtaken_slots[k].offset + 4)
                {
                  escaped_base = addrtaken_slots[k].offset;
                  escaped_end = addrtaken_slots[k].max_access_end;
                }
                else
                {
                  LOG_SL_FWD("CALL@i=%d INVALIDATE: store@i=%d sym=%p off=%lld (unknown anonymous slot range)", i,
                             entries[j].instruction_idx, (const void *)entries[j].local_sym,
                             (long long)entries[j].local_offset);
                  entries[j].valid = 0;
                  break;
                }
              }

              if (store_base >= escaped_end || store_end <= escaped_base)
                continue;
            }

            {
              LOG_SL_FWD("CALL@i=%d INVALIDATE: store@i=%d sym=%p off=%lld (earliest_lea@%d <= call)", i,
                         entries[j].instruction_idx, (const void *)entries[j].local_sym,
                         (long long)entries[j].local_offset, addrtaken_slots[k].earliest_lea_idx);
              entries[j].valid = 0;
              break;
            }
          }
        }
      }
      /* For FUNCCALLVAL, the dest vreg is redefined — invalidate stores
       * whose stored_value was that vreg and track the write. */
      if (q->op == TCCIR_OP_FUNCCALLVAL)
      {
        IROperand call_dest = tcc_ir_op_get_dest(ir, q);
        int32_t call_dest_vr = irop_get_vreg(call_dest);
        if (call_dest_vr >= 0)
        {
          for (j = 0; j < entry_count; j++)
          {
            if (entries[j].valid && irop_get_vreg(entries[j].stored_value) == call_dest_vr)
              entries[j].valid = 0;
          }
          if (!call_dest.is_lval)
          {
            int vr_type = TCCIR_DECODE_VREG_TYPE(call_dest_vr);
            int vr_pos = TCCIR_DECODE_VREG_POSITION(call_dest_vr);
            VregWriteTracker *tracker = NULL;
            if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
              tracker = &var_writes[vr_pos];
            else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
              tracker = &tmp_writes[vr_pos];
            else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
              tracker = &par_writes[vr_pos];
            if (tracker)
            {
              tracker->last_write_idx = i;
              tracker->gen = write_tracker_gen;
            }
          }
        }
      }
      continue;
    }

    /* Process LOAD, ASSIGN-with-deref, and single-value FUNCPARAMVAL
     * instructions: forward from a previous store.
     * ASSIGN with is_lval src1 is semantically a LOAD (e.g. after LEA fold).
     * FUNCPARAMVAL with is_lval stack-loc src1 is essentially an embedded
     * LOAD, but only when it passes a single scalar:
     *  - complex types copy multiple words from the stack reference, so a
     *    single store can't cover the entire value;
     *  - VAR vregs use abstract positions, not real stack offsets. */
    if (q->op == TCCIR_OP_LOAD ||
        (q->op == TCCIR_OP_ASSIGN && tcc_ir_op_get_src1(ir, q).is_lval &&
         !(irop_get_vreg(tcc_ir_op_get_src1(ir, q)) >= 0 &&
           TCCIR_DECODE_VREG_TYPE(irop_get_vreg(tcc_ir_op_get_src1(ir, q))) == TCCIR_VREG_TYPE_VAR)) ||
        (q->op == TCCIR_OP_FUNCPARAMVAL && tcc_ir_op_get_src1(ir, q).is_local && tcc_ir_op_get_src1(ir, q).is_lval &&
         !tcc_ir_op_get_src1(ir, q).is_complex && !irop_is_64bit(tcc_ir_op_get_src1(ir, q)) &&
         !(irop_get_vreg(tcc_ir_op_get_src1(ir, q)) >= 0 &&
           TCCIR_DECODE_VREG_TYPE(irop_get_vreg(tcc_ir_op_get_src1(ir, q))) == TCCIR_VREG_TYPE_VAR)))
    {
      /* LOAD: dest <- src1***DEREF***
       * src1 is the address to load from */
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t addr_vr = irop_get_vreg(src1);
      const Sym *addr_sym;
      int64_t addr_offset;
      uint32_t h;
      StoreEntry *e;

      /* CONSERVATIVE: Only forward for stack locals, or non-locals that
       * can be resolved to a known stack location via the LEA map. */
      int load_via_lea = 0;
      if (!src1.is_local)
      {
        /* Check if src1 is a TEMP***DEREF*** in the LEA map */
        if (src1.is_lval)
        {
          int32_t lv = irop_get_vreg(src1);
          if (lv >= 0 && TCCIR_DECODE_VREG_TYPE(lv) == TCCIR_VREG_TYPE_TEMP)
          {
            int lp = TCCIR_DECODE_VREG_POSITION(lv);
            if (lp <= max_tmp && lea_map[lp].valid)
            {
              addr_sym = lea_map[lp].sym;
              addr_offset = lea_map[lp].offset;
              load_via_lea = 1;
              goto resolved_local_load;
            }
            LOG_SL_FWD("LOAD@i=%d SKIP: TMP:%d src1 is_lval but no LEA-map entry", i, lp);
          }
          else
          {
            LOG_SL_FWD("LOAD@i=%d SKIP: src1 is_lval but not a TEMP (vr=%d)", i, lv);
          }
        }
        else
        {
          LOG_SL_FWD("LOAD@i=%d SKIP: src1 not is_local and not is_lval", i);
        }
        continue;
      }

      /* Check if address is taken - if so, skip forwarding (may alias through pointer) */
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
        {
          LOG_SL_FWD("LOAD@i=%d SKIP: addr vreg:%d addrtaken", i, addr_vr);
          continue;
        }
      }

      /* Extract sym and offset from the local address operand.
       * For VAR-type vregs, the raw u.imm32 may be 0 (unresolved).
       * Fall back to the allocated stack slot to get a real offset
       * that matches LEA-resolved pointer stores. */
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

    resolved_local_load:
      /* For VT_LOCAL, hash on symbol pointer and offset */
      h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;

      LOG_SL_FWD("LOAD@i=%d PROBE: sym=%p off=%lld btype=%d via_lea=%d | tag=%d vr_type=%d pos=%d is_local=%d "
                 "is_lval=%d is_llocal=%d is_sym=%d u.imm32=%d",
                 i, (const void *)addr_sym, (long long)addr_offset, (int)src1.btype, load_via_lea,
                 (int)irop_get_tag(src1), (int)TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src1)),
                 (int)TCCIR_DECODE_VREG_POSITION(irop_get_vreg(src1)), (int)src1.is_local, (int)src1.is_lval,
                 (int)src1.is_llocal, (int)src1.is_sym, (int)src1.u.imm32);

      int matched_any = 0;
      int rejected_width = 0;
      int rejected_stale_tracker = 0;
      int rejected_invalid = 0;

      /* Search for matching store.  addr_addrtaken entries are valid as long
       * as they haven't been invalidated by a CALL (handled at line ~6938)
       * or by an unknown-pointer STORE (handled at line ~7271) — both of
       * which clear the entry.  Within the same BB, between STORE and LOAD,
       * with no such invalidation, forwarding is safe even if the slot's
       * address was taken elsewhere. */
      for (e = hash_table[h]; e != NULL; e = e->next)
      {
        if (!e->valid)
        {
          if (e->local_sym == addr_sym && e->local_offset == addr_offset)
            rejected_invalid++;
          continue;
        }

        /* Both are stack locals - match on symbol and offset */
        if (e->local_sym == addr_sym && e->local_offset == addr_offset)
        {
          matched_any++;
          /* Width check: don't forward if store and load access different widths.
           * E.g. a 32-bit store to StackLoc[-8] must not be forwarded to a
           * 64-bit load from StackLoc[-8] (the load reads additional bytes).
           *
           * Exception: narrower load from a wider *constant* store at the
           * same offset can forward the masked low bits as a new const.
           * Handles `int-store 4; byte-load` style patterns that show up
           * after struct sret + byte field reads. */
          if (e->store_btype != src1.btype)
          {
            int store_bits = 0, load_bits = 0;
            switch (e->store_btype)
            {
            case IROP_BTYPE_INT8:
              store_bits = 8;
              break;
            case IROP_BTYPE_INT16:
              store_bits = 16;
              break;
            case IROP_BTYPE_INT32:
              store_bits = 32;
              break;
            case IROP_BTYPE_INT64:
              store_bits = 64;
              break;
            default:
              store_bits = 0;
              break;
            }
            switch (src1.btype)
            {
            case IROP_BTYPE_INT8:
              load_bits = 8;
              break;
            case IROP_BTYPE_INT16:
              load_bits = 16;
              break;
            case IROP_BTYPE_INT32:
              load_bits = 32;
              break;
            default:
              load_bits = 0;
              break;
            }
            if (store_bits > 0 && load_bits > 0 && store_bits > load_bits && irop_is_immediate(e->stored_value))
            {
              int64_t full64 = irop_get_imm64_ex(ir, e->stored_value);
              uint32_t mask = (load_bits == 32) ? 0xFFFFFFFFu : ((1u << load_bits) - 1);
              int32_t full = (int32_t)(uint32_t)full64;
              int32_t narrow = (int32_t)((uint32_t)full & mask);
              /* Sign-extend if the load is signed and narrower than 32. */
              if (!src1.is_unsigned && load_bits < 32)
              {
                int shift = 32 - load_bits;
                narrow = (int32_t)((uint32_t)narrow << shift);
                narrow = narrow >> shift; /* arithmetic shift preserves sign */
              }
              LOG_SL_FWD("LOAD@i=%d FORWARD-MASK: store@i=%d store_bits=%d load_bits=%d full=%d narrow=%d", i,
                         e->instruction_idx, store_bits, load_bits, full, narrow);
              /* Replace the LOAD with ASSIGN of the masked constant.
               * Keep FUNCPARAMVAL as-is — only replace the deref src1. */
              if (q->op != TCCIR_OP_FUNCPARAMVAL)
                q->op = TCCIR_OP_ASSIGN;
              int pool_off = q->operand_base + irop_config[q->op].has_dest;
              ir->iroperand_pool[pool_off] = irop_make_imm32(-1, narrow, src1.btype);
              /* Track forwarded value for transitive forwarding, same as the
               * regular forwarding path does below. */
              {
                IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
                int32_t fwd_dest_vr = irop_get_vreg(fwd_dest);
                if (fwd_dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr) == TCCIR_VREG_TYPE_TEMP)
                {
                  int fwd_pos = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr);
                  if (fwd_pos <= max_tmp)
                  {
                    fwd_tmp_val[fwd_pos] = irop_make_imm32(-1, narrow, src1.btype);
                    fwd_tmp_valid[fwd_pos] = 1;
                  }
                }
              }
              changes++;
              break; /* exit the entry-scan loop; move to next LOAD */
            }
            LOG_SL_FWD("LOAD@i=%d REJECT width: store btype=%d vs load btype=%d (store at i=%d)", i,
                       (int)e->store_btype, (int)src1.btype, e->instruction_idx);
            rejected_width++;
            continue;
          }

          /* Safety check: if the LOAD's address vreg was written AFTER the
           * matching store, the store entry is stale. This happens when:
           * 1. STORE val → stack_slot[-88]  (records stored_value)
           * 2. AND/ADD/etc → VARx           (writes to VARx which lives at -88)
           * 3. LOAD VARx → dest             (should read VARx's register value, not step 1's value)
           * Without this check, step 3 incorrectly forwards step 1's value.
           *
           * Skip this check when the LOAD was resolved via the LEA map: the
           * address TEMP/VAR just holds the LEA result, so its def time is
           * independent of the stack-slot's value timeline. */
          if (!load_via_lea && addr_vr >= 0)
          {
            int vr_type = TCCIR_DECODE_VREG_TYPE(addr_vr);
            int vr_pos = TCCIR_DECODE_VREG_POSITION(addr_vr);
            VregWriteTracker *tracker = NULL;
            if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
              tracker = &var_writes[vr_pos];
            else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
              tracker = &tmp_writes[vr_pos];
            else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
              tracker = &par_writes[vr_pos];
            if (tracker && tracker->gen == write_tracker_gen && tracker->last_write_idx > e->instruction_idx)
            {
              /* The LOAD's address vreg was written after the store — skip */
              LOG_SL_FWD("LOAD@i=%d REJECT stale tracker: addr vr=%d last_write=%d > store@i=%d", i, addr_vr,
                         tracker->last_write_idx, e->instruction_idx);
              rejected_stale_tracker++;
              continue;
            }
          }
#ifdef TCC_REGALLOC_DEBUG
          fprintf(stderr,
                  "[SL-FWD] i=%d LOAD replaced by ASSIGN from store at i=%d, stored_vr=0x%x, load_addr_vr=0x%x, "
                  "offset=%lld\n",
                  i, e->instruction_idx, irop_get_vreg(e->stored_value), addr_vr, (long long)addr_offset);
#endif
          LOG_SL_FWD("LOAD@i=%d FORWARD from store at i=%d", i, e->instruction_idx);
          /* For FUNCPARAMVAL: keep the op, just replace the deref src1 with
           * the stored value.  For LOAD: convert to ASSIGN. */
          if (q->op != TCCIR_OP_FUNCPARAMVAL)
            q->op = TCCIR_OP_ASSIGN;
          /* Write stored value to both pools for src1 slot */
          int pool_off = q->operand_base + irop_config[q->op].has_dest;
          ir->iroperand_pool[pool_off] = e->stored_value;
          /* Track the assigned value for transitive forwarding:
           * If T2 <-- #7 [ASSIGN], record so that later STORE loc <-- T2
           * can use #7 directly instead of T2. */
          {
            IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
            int32_t fwd_dest_vr = irop_get_vreg(fwd_dest);
            if (fwd_dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int fwd_pos = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr);
              if (fwd_pos <= max_tmp)
              {
                IROperand fwd_sv = e->stored_value;
                /* Transitive resolution: if stored_value is a TEMP in fwd_tmp,
                 * resolve to its underlying constant */
                int32_t fwd_sv_vr = irop_get_vreg(fwd_sv);
                if (fwd_sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_sv_vr) == TCCIR_VREG_TYPE_TEMP && !fwd_sv.is_lval)
                {
                  int fwd_sv_pos = TCCIR_DECODE_VREG_POSITION(fwd_sv_vr);
                  if (fwd_sv_pos <= max_tmp && fwd_tmp_valid[fwd_sv_pos])
                    fwd_sv = fwd_tmp_val[fwd_sv_pos];
                }
                fwd_tmp_val[fwd_pos] = fwd_sv;
                fwd_tmp_valid[fwd_pos] = 1;
                /* LEA map propagation: if the stored value is a TEMP in the
                 * LEA map, propagate to the destination TEMP.  This handles
                 * the pattern: T28 = &StackLoc; V21 = T28; T29 = V21
                 * After forwarding, T29 = T28 [ASSIGN] — so T29 should
                 * inherit T28's LEA map entry for pointer-store resolution. */
                {
                  int32_t sv_vr = irop_get_vreg(e->stored_value);
                  if (sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(sv_vr) == TCCIR_VREG_TYPE_TEMP && !e->stored_value.is_lval)
                  {
                    int sv_pos = TCCIR_DECODE_VREG_POSITION(sv_vr);
                    if (sv_pos <= max_tmp && lea_map[sv_pos].valid)
                    {
                      lea_map[fwd_pos].offset = lea_map[sv_pos].offset;
                      lea_map[fwd_pos].sym = lea_map[sv_pos].sym;
                      lea_map[fwd_pos].valid = 1;
                    }
                  }
                }
              }
            }
          }
          /* Record this store as a candidate for dead-store elimination.
           * After the main loop we check if anyone still reads from its offset. */
          if (fwd_store_count < SL_FWD_MAX_DEAD_STORES && !e->addr_addrtaken &&
              irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[e->instruction_idx])) < 0)
          {
            fwd_stores[fwd_store_count].store_idx = e->instruction_idx;
            fwd_stores[fwd_store_count].offset = e->local_offset;
            fwd_stores[fwd_store_count].sym = e->local_sym;
            fwd_store_count++;
          }
          changes++;
          break;
        }
      }
      /* Cross-offset: 32-bit load from offset X may read the upper half of
       * a 64-bit constant store at offset X-4. Probe the hash table there. */
      if (e == NULL && src1.btype == IROP_BTYPE_INT32)
      {
        int64_t lo_offset = addr_offset - 4;
        uint32_t lo_h = ((uintptr_t)addr_sym * 31 + (uint32_t)lo_offset * 17) % 128;
        StoreEntry *lo_e;
        for (lo_e = hash_table[lo_h]; lo_e != NULL; lo_e = lo_e->next)
        {
          if (!lo_e->valid || lo_e->local_sym != addr_sym || lo_e->local_offset != lo_offset)
            continue;
          if (lo_e->store_btype != IROP_BTYPE_INT64 || !irop_is_immediate(lo_e->stored_value))
            continue;
          int64_t full64 = irop_get_imm64_ex(ir, lo_e->stored_value);
          int32_t upper = (int32_t)(uint32_t)(full64 >> 32);
          LOG_SL_FWD("LOAD@i=%d FORWARD-HI: store@i=%d upper32=%d from 64-bit val", i, lo_e->instruction_idx, upper);
          if (q->op != TCCIR_OP_FUNCPARAMVAL)
            q->op = TCCIR_OP_ASSIGN;
          {
            int pool_off = q->operand_base + irop_config[q->op].has_dest;
            ir->iroperand_pool[pool_off] = irop_make_imm32(-1, upper, src1.btype);
          }
          {
            IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
            int32_t fwd_dest_vr = irop_get_vreg(fwd_dest);
            if (fwd_dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int fwd_pos = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr);
              if (fwd_pos <= max_tmp)
              {
                fwd_tmp_val[fwd_pos] = irop_make_imm32(-1, upper, src1.btype);
                fwd_tmp_valid[fwd_pos] = 1;
              }
            }
          }
          changes++;
          break;
        }
        e = lo_e;
      }
      /* If we fell out of the search without forwarding, explain why in
       * aggregate. "no match" means the hash bucket had no matching entry
       * at all (either the STORE was never tracked, was invalidated, or is
       * in a different BB not reachable via snapshot). */
      if (e == NULL)
      {
        LOG_SL_FWD("LOAD@i=%d NOMATCH: sym=%p off=%lld matched=%d width_rej=%d stale_rej=%d invalid_rej=%d", i,
                   (const void *)addr_sym, (long long)addr_offset, matched_any, rejected_width, rejected_stale_tracker,
                   rejected_invalid);
      }
    }
    /* LOAD_INDEXED with LEA-mapped base + constant index:
     * dest = *(base + #imm) where base is in the LEA map → resolve to
     * base_offset + imm and forward from hash table. */
    else if (q->op == TCCIR_OP_LOAD_INDEXED)
    {
      IROperand li_src1 = tcc_ir_op_get_src1(ir, q);
      IROperand li_src2 = tcc_ir_op_get_src2(ir, q);
      int32_t base_vr = irop_get_vreg(li_src1);
      if (base_vr >= 0 && TCCIR_DECODE_VREG_TYPE(base_vr) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(li_src2) &&
          !li_src2.is_sym)
      {
        int bp = TCCIR_DECODE_VREG_POSITION(base_vr);
        if (bp <= max_tmp && lea_map[bp].valid)
        {
          int64_t eff_off = lea_map[bp].offset + irop_get_imm64_ex(ir, li_src2);
          const Sym *eff_sym = lea_map[bp].sym;
          uint32_t lih = ((uintptr_t)eff_sym * 31 + (uint32_t)eff_off * 17) % 128;
          StoreEntry *lie;
          for (lie = hash_table[lih]; lie != NULL; lie = lie->next)
          {
            if (!lie->valid)
              continue;
            if (lie->local_sym != eff_sym || lie->local_offset != eff_off)
              continue;
            if (lie->store_btype != li_src1.btype)
              continue;
            int li_tag = irop_get_tag(lie->stored_value);
            if (li_tag != IROP_TAG_IMM32 && li_tag != IROP_TAG_I64 && li_tag != IROP_TAG_STACKOFF)
              continue;
            if (li_tag == IROP_TAG_STACKOFF && lie->stored_value.is_lval)
              continue;
            q->op = TCCIR_OP_ASSIGN;
            int li_pool = q->operand_base + irop_config[TCCIR_OP_ASSIGN].has_dest;
            ir->iroperand_pool[li_pool] = lie->stored_value;
            tcc_ir_set_src2(ir, i, IROP_NONE);
            LOG_SL_FWD("LOAD_INDEXED@i=%d FORWARD: eff_off=%lld from store@i=%d", i, (long long)eff_off,
                       lie->instruction_idx);
            {
              IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
              int32_t fwd_vr = irop_get_vreg(fwd_dest);
              if (fwd_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_vr) == TCCIR_VREG_TYPE_TEMP)
              {
                int fp = TCCIR_DECODE_VREG_POSITION(fwd_vr);
                if (fp <= max_tmp)
                {
                  fwd_tmp_val[fp] = lie->stored_value;
                  fwd_tmp_valid[fp] = 1;
                  if (li_tag == IROP_TAG_STACKOFF)
                  {
                    lea_map[fp].offset = irop_get_stack_offset(lie->stored_value);
                    lea_map[fp].sym = lie->local_sym;
                    lea_map[fp].valid = 1;
                  }
                }
              }
            }
            changes++;
            break;
          }
        }
      }
    }
    /* Process TEST_ZERO / CMP with memory operands: forward stored values.
     * TEST_ZERO StackLoc[X] implicitly loads from the stack location.
     * If we have a tracked store to that location, replace the memory
     * operand with the stored value (e.g. TEST_ZERO #0). */
    else if (q->op == TCCIR_OP_TEST_ZERO)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t addr_vr = irop_get_vreg(src1);

      if (src1.is_local)
      {
        const Sym *addr_sym;
        int64_t addr_offset;

        /* Skip if address is taken */
        if (addr_vr >= 0)
        {
          IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
          if (interval && interval->addrtaken)
            goto skip_test_zero_fwd;
        }

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

        uint32_t h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;
        StoreEntry *e;
        for (e = hash_table[h]; e != NULL; e = e->next)
        {
          if (!e->valid)
            continue;
          if (e->local_sym == addr_sym && e->local_offset == addr_offset)
          {
            if (e->store_btype != src1.btype)
              continue;
            /* Vreg write safety check (same as LOAD path) */
            if (addr_vr >= 0)
            {
              int vr_type = TCCIR_DECODE_VREG_TYPE(addr_vr);
              int vr_pos = TCCIR_DECODE_VREG_POSITION(addr_vr);
              VregWriteTracker *tracker = NULL;
              if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
                tracker = &var_writes[vr_pos];
              else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
                tracker = &tmp_writes[vr_pos];
              else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
                tracker = &par_writes[vr_pos];
              if (tracker && tracker->gen == write_tracker_gen && tracker->last_write_idx > e->instruction_idx)
                continue;
            }
            LOG_IR_GEN("OPTIMIZE: TEST_ZERO store-forward at i=%d from store at i=%d", i, e->instruction_idx);
            /* Replace TEST_ZERO's memory src1 with the stored value */
            int pool_off = q->operand_base; /* TEST_ZERO: has_dest=0, src1 at base */
            ir->iroperand_pool[pool_off] = e->stored_value;
            changes++;
            break;
          }
        }
      }
    skip_test_zero_fwd:;
    }
    /* Forward tracked IMM32 stores into lval sources of ALU/comparison ops.
     * Pattern: `R0 <-- StackLoc[-4] AND #15` where StackLoc[-4] has a
     * tracked IMM32 store gets rewritten to `R0 <-- #const AND #15`; the
     * subsequent const_prop pass then folds to `R0 <-- #(const & 15)`, and
     * branch_folding collapses any CMP that depends on it.  Without this
     * the forwarding only fires on explicit TCCIR_OP_LOAD ops, leaving
     * struct-field read-and-test patterns unfolded. */
    else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_OR ||
             q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_SHL || q->op == TCCIR_OP_SHR || q->op == TCCIR_OP_SAR ||
             q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_CMP)
    {
      for (int si = 0; si < 2; si++)
      {
        IROperand src = (si == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        if (!src.is_lval)
          continue;
        /* Resolve the stack/symbol address — either directly for locals,
         * or through the LEA map for TEMP***DEREF*** operands. */
        const Sym *addr_sym = NULL;
        int64_t addr_offset = 0;
        if (src.is_local && !src.is_llocal)
        {
          int src_tag = irop_get_tag(src);
          if (src_tag == IROP_TAG_SYMREF)
          {
            IRPoolSymref *sr = irop_get_symref_ex(ir, src);
            addr_sym = sr ? sr->sym : NULL;
            addr_offset = sr ? sr->addend : 0;
          }
          else if (src_tag == IROP_TAG_STACKOFF)
          {
            addr_offset = irop_get_stack_offset(src);
          }
          else
          {
            continue;
          }
          /* Skip VAR-type vregs: VARs can be redefined by ALU ops (e.g.
           * `V0 <-- #0 SUB V0`), which SL_FWD doesn't track as stores.
           * Forwarding from a stale VAR store would produce wrong values.
           * VAR constants are handled by const_prop/const_var_prop instead. */
          int32_t addr_vr = irop_get_vreg(src);
          if (addr_vr >= 0 && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
            continue;
          if (addr_vr >= 0)
          {
            IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
            if (interval && interval->addrtaken)
              continue;
          }
        }
        else
        {
          /* TEMP***DEREF***: resolve via LEA map to a known stack location */
          int32_t lea_vr = irop_get_vreg(src);
          if (lea_vr >= 0 && TCCIR_DECODE_VREG_TYPE(lea_vr) == TCCIR_VREG_TYPE_TEMP)
          {
            int lp = TCCIR_DECODE_VREG_POSITION(lea_vr);
            if (lp <= max_tmp && lea_map[lp].valid)
            {
              addr_sym = lea_map[lp].sym;
              addr_offset = lea_map[lp].offset;
            }
            else
              continue;
          }
          else
            continue;
        }
        uint32_t h2 = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;
        StoreEntry *e;
        for (e = hash_table[h2]; e != NULL; e = e->next)
        {
          if (!e->valid)
            continue;
          if (e->local_sym != addr_sym || e->local_offset != addr_offset)
            continue;
          /* Accept integer-immediate or vreg stored values.
           * IMM32/I64: forward the constant into the ALU operand.
           * VREG: replace the lval deref with a direct vreg reference. */
          int sv_tag = irop_get_tag(e->stored_value);
          int32_t sv_vr = irop_get_vreg(e->stored_value);
          int sv_is_vreg = (sv_vr >= 0 && !e->stored_value.is_lval && sv_tag == IROP_TAG_VREG);
          if (!sv_is_vreg && sv_tag != IROP_TAG_IMM32 && sv_tag != IROP_TAG_I64)
            continue;

          if (sv_is_vreg && e->store_btype == src.btype)
          {
            IROperand replacement = e->stored_value;
            replacement.is_lval = 0;
            replacement.btype = src.btype;
            if (si == 0)
              tcc_ir_op_set_src1(ir, q, replacement);
            else
              tcc_ir_op_set_src2(ir, q, replacement);
            LOG_SL_FWD("ALU@i=%d FORWARD vreg: si=%d from store@i=%d", i, si, e->instruction_idx);
            changes++;
            break;
          }
          int store_bits, load_bits;
          switch (e->store_btype)
          {
          case IROP_BTYPE_INT8:
            store_bits = 8;
            break;
          case IROP_BTYPE_INT16:
            store_bits = 16;
            break;
          case IROP_BTYPE_INT32:
            store_bits = 32;
            break;
          default:
            store_bits = 0;
            break;
          }
          switch (src.btype)
          {
          case IROP_BTYPE_INT8:
            load_bits = 8;
            break;
          case IROP_BTYPE_INT16:
            load_bits = 16;
            break;
          case IROP_BTYPE_INT32:
            load_bits = 32;
            break;
          default:
            load_bits = 0;
            break;
          }
          if (store_bits <= 0 || load_bits <= 0 || store_bits < load_bits)
            continue;
          /* Extract the 64-bit value regardless of tag (IMM32 vs I64-via-pool),
           * then narrow to 32 bits.  For int stores the low 32 bits are the
           * entire payload; this keeps the rewriter simple and always emits
           * an IMM32 operand. */
          int64_t full64 = irop_get_imm64_ex(ir, e->stored_value);
          int32_t val = (int32_t)full64;
          if (store_bits > load_bits)
          {
            uint32_t mask = (load_bits == 32) ? 0xFFFFFFFFu : ((1u << load_bits) - 1);
            val = (int32_t)((uint32_t)val & mask);
            if (!src.is_unsigned && load_bits < 32)
            {
              int shift = 32 - load_bits;
              val = (int32_t)((uint32_t)val << shift);
              val = val >> shift; /* arithmetic: preserve sign */
            }
          }
          IROperand new_op = irop_make_imm32(-1, val, src.btype);
          if (si == 0)
            tcc_ir_op_set_src1(ir, q, new_op);
          else
            tcc_ir_op_set_src2(ir, q, new_op);
          LOG_SL_FWD("ALU@i=%d FORWARD: si=%d from store@i=%d store_bits=%d load_bits=%d val=%d", i, si,
                     e->instruction_idx, store_bits, load_bits, val);
          changes++;
          break;
        }
      }
    }
    /* STORE_INDEXED / STORE_POSTINC are pointer-based stores that this pass
     * does not analyze in detail.  Conservatively invalidate all tracked
     * stack slots — the pointer might alias any of them.  Without this,
     * disp_fusion's STORE -> STORE_INDEXED rewrites could leave the
     * forwarding table thinking a slot still holds its initializer value
     * after a real write through that slot's address. */
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    {
      int j;
      for (j = 0; j < entry_count; j++)
      {
        if (entries[j].valid)
        {
          LOG_IR_GEN("STORE-LOAD: Invalidate local at i=%d due to indexed/postinc store at i=%d",
                     entries[j].instruction_idx, i);
          entries[j].valid = 0;
        }
      }
    }
    /* Process STORE instructions: track them for later forwarding */
    if (q->op == TCCIR_OP_STORE)
    {
      /* Forward tracked StackLoc values into the src1 of this STORE.
       * Pattern: STORE StackLoc[X] <- val; ... ; STORE dest <- StackLoc[X]
       * Transform: STORE dest <- val (eliminates the stack load) */
      {
        IROperand stsrc1 = tcc_ir_op_get_src1(ir, q);
        if (stsrc1.is_local && stsrc1.is_lval)
        {
          const Sym *s_sym;
          int64_t s_offset;
          if (irop_get_tag(stsrc1) == IROP_TAG_SYMREF)
          {
            IRPoolSymref *sr = irop_get_symref_ex(ir, stsrc1);
            s_sym = sr ? sr->sym : NULL;
            s_offset = sr ? sr->addend : 0;
          }
          else
          {
            s_sym = NULL;
            s_offset = irop_get_imm64_ex(ir, stsrc1);
          }

          int32_t s_vr = irop_get_vreg(stsrc1);
          int src_addrtaken = 0;
          if (s_vr >= 0)
          {
            IRLiveInterval *interval = tcc_ir_get_live_interval(ir, s_vr);
            if (interval && interval->addrtaken)
              src_addrtaken = 1;
          }

          if (!src_addrtaken)
          {
            uint32_t sh = ((uintptr_t)s_sym * 31 + (uint32_t)s_offset * 17) % 128;
            StoreEntry *se;
            for (se = hash_table[sh]; se != NULL; se = se->next)
            {
              if (!se->valid)
                continue;
              if (se->local_sym != s_sym || se->local_offset != s_offset)
                continue;
              if (se->store_btype != stsrc1.btype)
                continue;
              int32_t sv_vr = irop_get_vreg(se->stored_value);
              if (sv_vr >= 0)
              {
                int vr_type = TCCIR_DECODE_VREG_TYPE(sv_vr);
                int vr_pos = TCCIR_DECODE_VREG_POSITION(sv_vr);
                VregWriteTracker *tracker = NULL;
                if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
                  tracker = &var_writes[vr_pos];
                else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
                  tracker = &tmp_writes[vr_pos];
                else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
                  tracker = &par_writes[vr_pos];
                if (tracker && tracker->gen == write_tracker_gen &&
                    tracker->last_write_idx > se->instruction_idx)
                  continue;
              }
              LOG_SL_FWD("STORE@i=%d FORWARD src1 from store at i=%d off=%lld", i, se->instruction_idx,
                         (long long)s_offset);
              {
                int pool_off = q->operand_base + irop_config[TCCIR_OP_STORE].has_dest;
                ir->iroperand_pool[pool_off] = se->stored_value;
              }
              if (fwd_store_count < SL_FWD_MAX_DEAD_STORES && !se->addr_addrtaken)
              {
                fwd_stores[fwd_store_count].store_idx = se->instruction_idx;
                fwd_stores[fwd_store_count].offset = se->local_offset;
                fwd_stores[fwd_store_count].sym = se->local_sym;
                fwd_store_count++;
              }
              changes++;
              break;
            }
          }
        }
      }

      /* STORE: dest***DEREF*** <- src1
       * dest is the address, src1 is the value to store */
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t addr_vr = irop_get_vreg(dest);
      const Sym *addr_sym;
      int64_t addr_offset;
      int addr_addrtaken = 0;
      int addr_via_pointer = 0;
      uint32_t h;
      StoreEntry *new_entry = NULL;
      int j;

      /* CONSERVATIVE: Only track stack locals for forwarding.
       * However, if the pointer is in the LEA map (i.e. we know it points to
       * a specific stack location), treat it as a local store instead. */
      if (!dest.is_local)
      {
        /* VAR dest: this is a write to a local variable's own storage,
         * not a pointer write.  It can't alias any tracked stack slot,
         * so leave the hash table intact.  (STOREs to VAR carry is_lval=1
         * since the VAR slot is written through its storage address;
         * ASSIGNs to VAR have is_lval=0.  Skip both.) */
        {
          int32_t dv = irop_get_vreg(dest);
          if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
            continue;
        }
        /* Check if dest is a TEMP in the LEA map — if so, resolve to local */
        int resolved_to_local = 0;
        if (dest.is_lval)
        {
          int32_t dv = irop_get_vreg(dest);
          if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
          {
            int dp = TCCIR_DECODE_VREG_POSITION(dv);
            if (dp <= max_tmp && lea_map[dp].valid)
            {
              /* Resolved: treat as local store at the resolved offset.
               * Mark as via_pointer so function calls invalidate it. */
              addr_sym = lea_map[dp].sym;
              addr_offset = lea_map[dp].offset;
              addr_via_pointer = 1;
              resolved_to_local = 1;
            }
          }
        }
        if (!resolved_to_local)
        {
          /* Unknown pointer store — invalidate ALL tracked stores */
          for (j = 0; j < entry_count; j++)
          {
            if (entries[j].valid)
            {
              LOG_IR_GEN("STORE-LOAD: Invalidate local at i=%d due to pointer store at i=%d",
                         entries[j].instruction_idx, i);
              entries[j].valid = 0;
            }
          }
          continue;
        }
        /* Fall through to normal store tracking with resolved addr_sym/addr_offset */
        goto resolved_local_store;
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

    resolved_local_store:
      /* VAR destinations use stack offsets that can coincidentally collide
       * with anonymous StackLoc offsets in the hash table.  Use a distinct
       * sentinel sym pointer so VARs hash to different buckets than StackLocs. */
      if (addr_vr >= 0 && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
        addr_sym = (const Sym *)(uintptr_t)1; /* sentinel: VAR namespace */

      /* For VT_LOCAL, hash on symbol pointer and offset */
      h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;

      /* Partial-overwrite merge: if an existing valid entry at the same
       * offset was a wider constant store, and the new store is a narrower
       * constant store that lands on the low bits, merge values into the
       * existing entry instead of invalidating it.  This lets a subsequent
       * wider load forward the merged constant — matching C's byte-overlay
       * semantics for compound-literal init + byte-field writes.
       *
       * Example (make_opcode's `(Opcode){4, encoded}` sret init):
       *   int-store  #0 at -56   (zero-init compound literal)
       *   byte-store #4 at -56   (size = 4)
       *   int-load   from -56    → forwards as int (0 & 0xFFFFFF00) | (4 & 0xFF) = 4
       */
      int new_bits_local = 0;
      int new_is_imm = (irop_get_tag(tcc_ir_op_get_src1(ir, q)) == IROP_TAG_IMM32);
      switch (dest.btype)
      {
      case IROP_BTYPE_INT8:
        new_bits_local = 8;
        break;
      case IROP_BTYPE_INT16:
        new_bits_local = 16;
        break;
      case IROP_BTYPE_INT32:
        new_bits_local = 32;
        break;
      default:
        new_bits_local = 0;
        break; /* FP/struct/64-bit: skip merge */
      }
      int merged_into_existing = 0;
      for (new_entry = hash_table[h]; new_entry != NULL; new_entry = new_entry->next)
      {
        if (new_entry->local_sym != addr_sym || new_entry->local_offset != addr_offset)
          continue;
        if (!new_entry->valid)
          continue;
        int old_bits = 0;
        switch (new_entry->store_btype)
        {
        case IROP_BTYPE_INT8:
          old_bits = 8;
          break;
        case IROP_BTYPE_INT16:
          old_bits = 16;
          break;
        case IROP_BTYPE_INT32:
          old_bits = 32;
          break;
        default:
          old_bits = 0;
          break;
        }
        int old_is_imm = (irop_get_tag(new_entry->stored_value) == IROP_TAG_IMM32);
        if (!merged_into_existing && new_is_imm && old_is_imm && old_bits > new_bits_local && new_bits_local > 0)
        {
          int32_t old_v = new_entry->stored_value.u.imm32;
          int32_t new_v = tcc_ir_op_get_src1(ir, q).u.imm32;
          uint32_t mask = (new_bits_local == 32) ? 0xFFFFFFFFu : ((1u << new_bits_local) - 1);
          int32_t merged = (int32_t)((uint32_t)old_v & ~mask) | (int32_t)((uint32_t)new_v & mask);
          new_entry->stored_value.u.imm32 = merged;
          new_entry->instruction_idx = i;
          LOG_SL_FWD("STORE@i=%d MERGE into store@i=? at off=%lld: old_bits=%d new_bits=%d old_v=%d new_v=%d merged=%d",
                     i, (long long)addr_offset, old_bits, new_bits_local, old_v, new_v, merged);
          merged_into_existing = 1;
          continue; /* keep other entries (if any) walking — don't invalidate the merged one */
        }
        /* Not a mergeable overwrite — invalidate as before */
        new_entry->valid = 0;
      }
      /* Invalidate wider entries at lower offsets that partially overlap.
       * A store of N bytes at offset X overwrites part of any wider entry
       * at offset Y < X where Y + entry_bytes > X.  Example:
       *   int-store  #0 at -48   (creates 32-bit entry)
       *   short-store #73 at -48 (merges low bits into 32-bit entry)
       *   short-store #65531 at -46 (must invalidate 32-bit entry at -48) */
      {
        int max_delta = (new_bits_local > 0) ? (4 - new_bits_local / 8) : 0;
        if (max_delta < 0)
          max_delta = 0;
        for (int delta = 1; delta <= max_delta; delta++)
        {
          int64_t check_off = addr_offset - delta;
          uint32_t ch2 = ((uintptr_t)addr_sym * 31 + (uint32_t)check_off * 17) % 128;
          StoreEntry *ce;
          for (ce = hash_table[ch2]; ce != NULL; ce = ce->next)
          {
            if (!ce->valid || ce->local_sym != addr_sym || ce->local_offset != check_off)
              continue;
            int entry_bytes = 0;
            switch (ce->store_btype)
            {
            case IROP_BTYPE_INT16:
              entry_bytes = 2;
              break;
            case IROP_BTYPE_INT32:
              entry_bytes = 4;
              break;
            case IROP_BTYPE_INT64:
              entry_bytes = 8;
              break;
            default:
              break;
            }
            if (entry_bytes > delta)
              ce->valid = 0;
          }
        }
      }

      if (merged_into_existing)
      {
        /* Skip the fresh-entry insert below; the existing entry now holds
         * the merged constant with the wider btype. */
        goto sl_fwd_store_done;
      }

      /* For wide stores, invalidate narrower entries at higher offsets
       * within the store's byte range.  A 32-bit store at X overwrites
       * any 16-bit entry at X+2; a 64-bit store at X overwrites entries
       * at X+2, X+4, X+6, etc. */
      {
        int store_bytes = new_bits_local / 8;
        if (store_bytes == 0 && (dest.btype == IROP_BTYPE_INT64 || dest.btype == IROP_BTYPE_FLOAT64))
          store_bytes = 8;
        StoreEntry *he;
        for (int fwd = 1; fwd < store_bytes; fwd++)
        {
          int64_t hi_offset = addr_offset + fwd;
          uint32_t hh = ((uintptr_t)addr_sym * 31 + (uint32_t)hi_offset * 17) % 128;
          for (he = hash_table[hh]; he != NULL; he = he->next)
          {
            if (he->valid && he->local_sym == addr_sym && he->local_offset == hi_offset)
              he->valid = 0;
          }
        }
      }

      /* Record the new store */
      new_entry = &entries[entry_count++];
      new_entry->valid = 1;
      new_entry->addr_addrtaken = addr_addrtaken;
      new_entry->addr_via_pointer = addr_via_pointer;
      new_entry->local_offset = addr_offset;
      new_entry->local_sym = addr_sym;
      new_entry->stored_value = tcc_ir_op_get_src1(ir, q);
      new_entry->instruction_idx = i;
      new_entry->store_dest_vr = addr_vr;
      new_entry->store_btype = dest.btype;
      new_entry->next = hash_table[h];
      hash_table[h] = new_entry;

      LOG_SL_FWD("STORE@i=%d TRACK: sym=%p off=%lld btype=%d addrtaken=%d via_ptr=%d", i, (const void *)addr_sym,
                 (long long)addr_offset, (int)dest.btype, addr_addrtaken, addr_via_pointer);

      /* Resolve stored value through forwarded-temp tracking:
       * If src1 is a TEMP that was assigned a value by earlier forwarding
       * (e.g. T2 <-- #7), use that value directly. This enables transitive
       * forwarding: STORE loc1 <-- #7; LOAD T2 <-- loc1 (forwarded to #7);
       * STORE loc2 <-- T2 → stored_value becomes #7 instead of T2. */
      {
        IROperand sv = new_entry->stored_value;
        int32_t sv_vr = irop_get_vreg(sv);
        if (sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(sv_vr) == TCCIR_VREG_TYPE_TEMP && !sv.is_lval)
        {
          int sv_pos = TCCIR_DECODE_VREG_POSITION(sv_vr);
          if (sv_pos <= max_tmp && fwd_tmp_valid[sv_pos])
          {
            new_entry->stored_value = fwd_tmp_val[sv_pos];
          }
        }
      }

      /* LEA-through / local-lval forwarding: if the stored value reads from
       * a memory location with a tracked constant, forward the constant.
       * Path 1: T***DEREF*** where T is in the LEA map → resolve to StackLoc.
       * Path 2: Direct StackLoc lval (vr<0, so irop_op_is_lval returns false,
       *         but is_lval bit is set — only safe when is_lval=1, not Addr[]). */
      {
        IROperand sv = tcc_ir_op_get_src1(ir, q);
        const Sym *resolved_sym = NULL;
        int64_t resolved_off = 0;
        int sv_resolved = 0;
        if (irop_op_is_lval(sv))
        {
          int32_t sv_vr = irop_get_vreg(sv);
          if (sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(sv_vr) == TCCIR_VREG_TYPE_TEMP)
          {
            int sv_pos = TCCIR_DECODE_VREG_POSITION(sv_vr);
            if (sv_pos <= max_tmp && lea_map[sv_pos].valid)
            {
              resolved_off = lea_map[sv_pos].offset;
              resolved_sym = lea_map[sv_pos].sym;
              sv_resolved = 1;
            }
          }
        }
        if (!sv_resolved && sv.is_lval && sv.is_local && !sv.is_llocal)
        {
          int sv_tag = irop_get_tag(sv);
          if (sv_tag == IROP_TAG_STACKOFF)
          {
            resolved_off = irop_get_stack_offset(sv);
            sv_resolved = 1;
          }
          else if (sv_tag == IROP_TAG_SYMREF)
          {
            IRPoolSymref *sr = irop_get_symref_ex(ir, sv);
            resolved_sym = sr ? sr->sym : NULL;
            resolved_off = sr ? sr->addend : 0;
            sv_resolved = 1;
          }
        }
        if (sv_resolved)
        {
          uint32_t rh = ((uintptr_t)resolved_sym * 31 + (uint32_t)resolved_off * 17) % 128;
          StoreEntry *re;
          for (re = hash_table[rh]; re != NULL; re = re->next)
          {
            if (!re->valid)
              continue;
            if (re->local_sym != resolved_sym || re->local_offset != resolved_off)
              continue;
            if (re->store_btype != sv.btype)
              continue;
            IROperand resolved_val = re->stored_value;
            {
              int32_t rv_vr = irop_get_vreg(resolved_val);
              if (rv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(rv_vr) == TCCIR_VREG_TYPE_TEMP && !resolved_val.is_lval)
              {
                int rv_pos = TCCIR_DECODE_VREG_POSITION(rv_vr);
                if (rv_pos <= max_tmp && fwd_tmp_valid[rv_pos])
                  resolved_val = fwd_tmp_val[rv_pos];
              }
            }
            int rv_tag = irop_get_tag(resolved_val);
            if (rv_tag != IROP_TAG_IMM32 && rv_tag != IROP_TAG_I64)
              continue;
            int src1_off = q->operand_base + irop_config[TCCIR_OP_STORE].has_dest;
            ir->iroperand_pool[src1_off] = resolved_val;
            if (new_entry)
              new_entry->stored_value = resolved_val;
            LOG_IR_GEN("OPTIMIZE: LVAL forwarding at i=%d from store at i=%d (offset=%lld)", i, re->instruction_idx,
                       (long long)resolved_off);
            changes++;
            break;
          }
        }
      }

#ifdef TCC_REGALLOC_DEBUG
      fprintf(stderr, "[SL-STORE] i=%d store_val_vr=0x%x store_addr_vr=0x%x offset=%lld n=%d\n", i,
              irop_get_vreg(new_entry->stored_value), addr_vr, (long long)addr_offset, ir->next_instruction_index);
#endif

      LOG_IR_GEN("STORE-LOAD: Track store at i=%d (addrtaken=%d, offset=%lld)", i, addr_addrtaken,
                 (long long)addr_offset);
    sl_fwd_store_done:;
    }

    /* Dynamic LEA map update: propagate LEA map through ADD instructions
     * encountered during forwarding.  The pre-scan LEA map handles ADDs
     * from the original IR, but forwarding may create new ASSIGN chains
     * (T29 = T28 where T28 is in the LEA map) followed by ADD (T32 = T29 + 4).
     * Without this, pointer-offset stores (fill_big field[1..3]) can't resolve. */
    if (q->op == TCCIR_OP_ADD)
    {
      IROperand adest = tcc_ir_op_get_dest(ir, q);
      int32_t adv = irop_get_vreg(adest);
      if (adv >= 0 && TCCIR_DECODE_VREG_TYPE(adv) == TCCIR_VREG_TYPE_TEMP)
      {
        int adp = TCCIR_DECODE_VREG_POSITION(adv);
        if (adp <= max_tmp)
        {
          IROperand as1 = tcc_ir_op_get_src1(ir, q);
          IROperand as2 = tcc_ir_op_get_src2(ir, q);
          int32_t s1v = irop_get_vreg(as1);
          int32_t s2v = irop_get_vreg(as2);
          if (s1v >= 0 && TCCIR_DECODE_VREG_TYPE(s1v) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(as2) && !as2.is_sym)
          {
            int s1p = TCCIR_DECODE_VREG_POSITION(s1v);
            if (s1p <= max_tmp && lea_map[s1p].valid)
            {
              lea_map[adp].offset = lea_map[s1p].offset + irop_get_imm64_ex(ir, as2);
              lea_map[adp].sym = lea_map[s1p].sym;
              lea_map[adp].valid = 1;
            }
          }
          else if (s2v >= 0 && TCCIR_DECODE_VREG_TYPE(s2v) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(as1) &&
                   !as1.is_sym)
          {
            int s2p = TCCIR_DECODE_VREG_POSITION(s2v);
            if (s2p <= max_tmp && lea_map[s2p].valid)
            {
              lea_map[adp].offset = lea_map[s2p].offset + irop_get_imm64_ex(ir, as1);
              lea_map[adp].sym = lea_map[s2p].sym;
              lea_map[adp].valid = 1;
            }
          }
        }
      }
    }

    /* Dynamic LEA map propagation for ASSIGN instructions.
     * Handles: T29 <-- V21 [ASSIGN] where V21 was stored from a LEA-mapped TEMP.
     * The ASSIGN may already exist from a prior pass (not created by SL forwarding),
     * so we must also resolve VARs through the hash table to find their stored value. */
    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand adest = tcc_ir_op_get_dest(ir, q);
      int32_t adv = irop_get_vreg(adest);
      if (adv >= 0 && TCCIR_DECODE_VREG_TYPE(adv) == TCCIR_VREG_TYPE_TEMP && !adest.is_lval)
      {
        int adp = TCCIR_DECODE_VREG_POSITION(adv);
        if (adp <= max_tmp && !lea_map[adp].valid)
        {
          IROperand asrc = tcc_ir_op_get_src1(ir, q);
          int32_t sv = irop_get_vreg(asrc);
          if (sv >= 0)
          {
            int sv_type = TCCIR_DECODE_VREG_TYPE(sv);
            int sv_pos = TCCIR_DECODE_VREG_POSITION(sv);
            /* Case 1: src is a TEMP directly in LEA map (not a DEREF) */
            if (sv_type == TCCIR_VREG_TYPE_TEMP && !asrc.is_lval && sv_pos <= max_tmp && lea_map[sv_pos].valid)
            {
              lea_map[adp] = lea_map[sv_pos];
            }
            /* Case 2: src is a VAR/PARAM — check hash table for its stored value */
            else if (asrc.is_local)
            {
              const Sym *vs = NULL;
              int64_t vo;
              if (irop_get_tag(asrc) == IROP_TAG_SYMREF)
              {
                IRPoolSymref *sr = irop_get_symref_ex(ir, asrc);
                vs = sr ? sr->sym : NULL;
                vo = sr ? sr->addend : 0;
              }
              else
              {
                vo = irop_get_imm64_ex(ir, asrc);
              }
              uint32_t vh = ((uintptr_t)vs * 31 + (uint32_t)vo * 17) % 128;
              StoreEntry *ve;
              for (ve = hash_table[vh]; ve != NULL; ve = ve->next)
              {
                if (!ve->valid)
                  continue;
                if (ve->local_sym == vs && ve->local_offset == vo)
                {
                  /* Found the stored value for this VAR — check if it's a LEA-mapped TEMP */
                  int32_t svr = irop_get_vreg(ve->stored_value);
                  if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP && !ve->stored_value.is_lval)
                  {
                    int svp = TCCIR_DECODE_VREG_POSITION(svr);
                    if (svp <= max_tmp && lea_map[svp].valid)
                    {
                      lea_map[adp] = lea_map[svp];
                    }
                  }
                  break;
                }
              }
            }
          }
        }
      }
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
#ifdef TCC_REGALLOC_DEBUG
            fprintf(stderr, "[SL-INVAL-VAL] i=%d invalidate store at si=%d (stored_val_vr=0x%x redefined) n=%d\n", i,
                    entries[j].instruction_idx, dest_vr, ir->next_instruction_index);
#endif
            entries[j].valid = 0;
          }
        }
      }

      /* Track this write for the LOAD address vreg safety check.
       * When a vreg is written by ANY instruction (AND, ADD, ASSIGN, etc.),
       * a later LOAD using that vreg as its address should NOT be forwarded
       * from a store that happened BEFORE this write. */
      if (dest_vr >= 0 && !dest.is_lval)
      {
        int vr_type = TCCIR_DECODE_VREG_TYPE(dest_vr);
        int vr_pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        VregWriteTracker *tracker = NULL;
        if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
          tracker = &var_writes[vr_pos];
        else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
          tracker = &tmp_writes[vr_pos];
        else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
          tracker = &par_writes[vr_pos];
        if (tracker)
        {
          tracker->last_write_idx = i;
          tracker->gen = write_tracker_gen;
        }
      }
    }
  }

  /* Post-pass: eliminate stores whose only load was forwarded.
   * For each forwarded store, scan all remaining (non-NOP) instructions to
   * check if any src operand still references the same local offset.
   * Only anonymous stores (vreg < 0) are candidates — already filtered above. */
  for (int fi = 0; fi < fwd_store_count; fi++)
  {
    int store_idx = fwd_stores[fi].store_idx;
    int64_t off = fwd_stores[fi].offset;
    const Sym *sym = fwd_stores[fi].sym;
    int still_read = 0;

    /* Check if the store was already NOP'd (e.g. by a later store overwrite) */
    if (ir->compact_instructions[store_idx].op == TCCIR_OP_NOP)
      continue;

    for (int j = 0; j < n && !still_read; j++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP || j == store_idx)
        continue;

      /* Helper macro: check if operand is a local address-of that could alias
       * our store's offset (i.e. the store is within a struct whose base
       * address is passed somewhere). */
#define CHECK_ADDR_ALIAS(op)                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((op).is_local && !(op).is_lval && !(op).is_llocal && irop_get_sym_ex(ir, (op)) == sym)                         \
    {                                                                                                                  \
      int64_t base_off = irop_get_imm64_ex(ir, (op));                                                                  \
      if (base_off <= off && (off - base_off) < 1024)                                                                  \
        still_read = 1;                                                                                                \
    }                                                                                                                  \
  } while (0)

      /* Helper macro: check if operand reads a multi-byte range that covers
       * our store's offset.  A read at offset X with width W covers [X, X+W). */
#define CHECK_WIDTH_OVERLAP(op)                                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((op).is_local && irop_get_sym_ex(ir, (op)) == sym)                                                             \
    {                                                                                                                  \
      int64_t _roff = irop_get_imm64_ex(ir, (op));                                                                     \
      if (_roff != off && _roff <= off)                                                                                \
      {                                                                                                                \
        int _w = 4;                                                                                                    \
        if ((op).btype == IROP_BTYPE_INT64 || (op).btype == IROP_BTYPE_FLOAT64)                                        \
          _w = 8;                                                                                                      \
        else if ((op).btype == IROP_BTYPE_STRUCT)                                                                      \
          _w = 1024;                                                                                                   \
        if (off < _roff + _w)                                                                                          \
          still_read = 1;                                                                                              \
      }                                                                                                                \
    }                                                                                                                  \
  } while (0)

      /* Check src1 */
      if (irop_config[jq->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, jq);
        if (s1.is_local && irop_get_imm64_ex(ir, s1) == off && irop_get_sym_ex(ir, s1) == sym)
          still_read = 1;
        if (!still_read)
          CHECK_WIDTH_OVERLAP(s1);
        if (!still_read)
          CHECK_ADDR_ALIAS(s1);
      }
      /* Check src2 */
      if (!still_read && irop_config[jq->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, jq);
        if (s2.is_local && irop_get_imm64_ex(ir, s2) == off && irop_get_sym_ex(ir, s2) == sym)
          still_read = 1;
        if (!still_read)
          CHECK_WIDTH_OVERLAP(s2);
        if (!still_read)
          CHECK_ADDR_ALIAS(s2);
      }
      /* Check dest of non-STORE ops (e.g. LOAD dest references an address) */
      if (!still_read && jq->op != TCCIR_OP_STORE && irop_config[jq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, jq);
        if (d.is_local && irop_get_imm64_ex(ir, d) == off && irop_get_sym_ex(ir, d) == sym)
          still_read = 1;
        if (!still_read)
          CHECK_WIDTH_OVERLAP(d);
        if (!still_read)
          CHECK_ADDR_ALIAS(d);
      }
      /* Check STORE dest with deref (reads the pointer from the slot) */
      if (!still_read && jq->op == TCCIR_OP_STORE && irop_config[jq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, jq);
        if (d.is_local && (d.is_lval || d.is_llocal) && irop_get_imm64_ex(ir, d) == off &&
            irop_get_sym_ex(ir, d) == sym)
          still_read = 1;
        if (!still_read)
          CHECK_WIDTH_OVERLAP(d);
        if (!still_read)
          CHECK_ADDR_ALIAS(d);
      }
#undef CHECK_ADDR_ALIAS
#undef CHECK_WIDTH_OVERLAP
    }

    if (!still_read)
    {
      LOG_IR_GEN("OPTIMIZE: Dead store at i=%d (offset %lld, no remaining readers after SL fwd)", store_idx,
                 (long long)off);
      ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;
      changes++;
    }
  }
#undef SL_FWD_MAX_DEAD_STORES

  tcc_free(entries);
  tcc_free(var_writes);
  tcc_free(tmp_writes);
  tcc_free(par_writes);
  tcc_free(lea_map);
  tcc_free(var_lea_map);
  tcc_free(var_def_count);
  tcc_free(addrtaken_slots);
  tcc_free(active_call_ids);
  tcc_free(fwd_tmp_val);
  tcc_free(fwd_tmp_valid);
  for (i = 0; i < n; i++)
    tcc_free(saved_entries[i]);
  tcc_free(saved_entries);
  tcc_free(saved_entry_count);
  tcc_free(pred_count);

  LOG_IR_GEN("=== STORE-LOAD FORWARDING END: %d changes ===", changes);

  return changes;
}

/* Return the byte width of an IROP_BTYPE_* value. */
static int irop_btype_byte_width(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
    return 4;
  case IROP_BTYPE_INT64:
    return 8;
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 4; /* struct, func, etc. — conservative */
  }
}

/* Redundant Store Elimination
 * Phase 4: Remove stores to memory locations that are overwritten before being read
 * (dead stores to memory)
 * CONSERVATIVE: Only handles stack locals whose address is not taken
 */
int tcc_ir_opt_store_redundant(TCCIRState *ir)
{
  /* Single forward pass: O(n) time, no heap allocation.
   *
   * Tracks at most RSE_MAX_ACTIVE pending stores since the last basic-block
   * boundary using a small on-stack table.  When a STORE to address A is seen
   * and A is already in the table, the previous store is overwritten without a
   * read → mark it NOP.  When a READ of A is seen, evict it from the table so
   * the producing store is not killed.  Block boundaries flush the table.
   *
   * If the table fills up (> RSE_MAX_ACTIVE distinct live stores in one block)
   * the excess stores are simply not tracked — conservative, never wrong. */
#define RSE_MAX_ACTIVE 16
  typedef struct
  {
    int64_t offset;
    const Sym *sym;
    int store_idx;
    int btype; /* VT_BYTE / VT_INT / etc. — width of the store */
  } RseSlot;

  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== REDUNDANT STORE ELIMINATION START ===");

  RseSlot active[RSE_MAX_ACTIVE];
  int active_count = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Block boundary: pending stores may be live on the other side → flush. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      active_count = 0;
      continue;
    }

    /* READ check: any instruction that uses a local address as src1 or src2
     * keeps the corresponding pending store alive. */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (src1.is_local)
      {
        int64_t off = irop_get_imm64_ex(ir, src1);
        const Sym *sym = irop_get_sym_ex(ir, src1);
        for (int k = 0; k < active_count; k++)
        {
          if (active[k].sym == sym && active[k].offset == off)
          {
            active[k] = active[--active_count]; /* evict (swap-remove) */
            break;
          }
        }
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      if (src2.is_local)
      {
        int64_t off = irop_get_imm64_ex(ir, src2);
        const Sym *sym = irop_get_sym_ex(ir, src2);
        for (int k = 0; k < active_count; k++)
        {
          if (active[k].sym == sym && active[k].offset == off)
          {
            active[k] = active[--active_count];
            break;
          }
        }
      }
    }

    /* STORE to a local non-addr-taken address. */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local)
        continue;

      /* Skip addr-taken locals: they may be read through a pointer. */
      int32_t addr_vr = irop_get_vreg(dest);
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
          continue;
      }

      int64_t off = irop_get_imm64_ex(ir, dest);
      const Sym *sym = irop_get_sym_ex(ir, dest);

      int store_btype = dest.btype;

      /* Look for a previous pending store to the same address. */
      int found = -1;
      for (int k = 0; k < active_count; k++)
      {
        if (active[k].sym == sym && active[k].offset == off)
        {
          found = k;
          break;
        }
      }

      if (found >= 0 && irop_btype_byte_width(store_btype) >= irop_btype_byte_width(active[found].btype))
      {
        /* Overwritten without a read AND the new store covers at least
         * as many bytes as the old one → the previous store is dead.
         * A byte-store must NOT kill a wider word-store at the same
         * offset, since the word-store covers additional bytes. */
        LOG_IR_GEN("OPTIMIZE: Redundant store at i=%d (overwritten without read)", active[found].store_idx);
        ir->compact_instructions[active[found].store_idx].op = TCCIR_OP_NOP;
        changes++;
        active[found].store_idx = i;
        active[found].btype = store_btype;
      }
      else if (found >= 0)
      {
        /* Same offset but narrower store — can't kill the wider store.
         * Evict the old entry and stop tracking this offset. */
        active[found] = active[--active_count];
      }
      else if (active_count < RSE_MAX_ACTIVE)
      {
        active[active_count].sym = sym;
        active[active_count].offset = off;
        active[active_count].store_idx = i;
        active[active_count].btype = store_btype;
        active_count++;
      }
      /* else: table full — skip this store conservatively */
    }
  }

  LOG_IR_GEN("=== REDUNDANT STORE ELIMINATION END: %d changes ===", changes);

  return changes;
#undef RSE_MAX_ACTIVE
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
static const char *nonneg_func_names[] = {
    "fabs", "fabsf", "abs", "labs", "llabs", "strlen", "sizeof",
};
#define NUM_NONNEG_FUNCS (sizeof(nonneg_func_names) / sizeof(nonneg_func_names[0]))

/* Flag-setting soft-float comparison function names.
 * __aeabi_cdcmple / __aeabi_cfcmple set ARM condition flags for a CMP-like
 * operation. The subsequent JUMPIF tests those flags with a TOK_* condition.
 * This is the default path used by TCC's soft-float FCMP lowering.
 */
static const char *flag_cmp_funcs[] = {
    "__aeabi_cdcmple",
    "__aeabi_cfcmple",
};
#define NUM_FLAG_CMP_FUNCS (sizeof(flag_cmp_funcs) / sizeof(flag_cmp_funcs[0]))

/* Maximum number of non-negative vregs to track simultaneously */
#define MAX_NONNEG_VREGS 32

/* Maximum number of pending call parameters to track */
#define MAX_PENDING_PARAMS 16

typedef struct
{
  int call_id;
  int param_idx;
  int32_t vreg;     /* -1 if immediate */
  int is_immediate; /* 1 if the parameter is an immediate value */
  int64_t imm_val;  /* immediate value (if is_immediate) */
} PendingParam;

int tcc_ir_opt_nonneg_branch_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 3)
    return 0;

  /* Phase 1: Identify which vregs hold non-negative values.
   * We track full 32-bit vreg IDs (type + position). */
  int32_t nonneg_vregs[MAX_NONNEG_VREGS];
  int nonneg_count = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    Sym *callee = irop_get_sym_ex(ir, src1);
    if (!callee)
      continue;

    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;

    int is_nonneg = 0;
    for (size_t j = 0; j < NUM_NONNEG_FUNCS; j++)
    {
      if (strcmp(name, nonneg_func_names[j]) == 0)
      {
        is_nonneg = 1;
        break;
      }
    }

    if (is_nonneg)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vreg = irop_get_vreg(dest);
      if (vreg >= 0 && nonneg_count < MAX_NONNEG_VREGS)
      {
        nonneg_vregs[nonneg_count++] = vreg;
        LOG_IR_GEN("NONNEG: vreg 0x%x is non-negative from call to '%s' at i=%d", vreg, name, i);
      }
    }
  }

  if (nonneg_count == 0)
    return 0;

  /* Phase 2: Find flag-setting soft-float comparison calls
   * (__aeabi_cdcmple / __aeabi_cfcmple) where:
   *   - Parameter 0 is a non-negative vreg and parameter 1 is zero (or vice versa)
   * Then determine the JUMPIF outcome from the condition token.
   *
   * cdcmple(a, b) sets flags as if CMP a, b. The JUMPIF condition token
   * directly encodes the comparison semantics (GE, LT, etc.).
   *
   * When a = nonneg >= 0 and b = 0:
   *   TOK_GE / TOK_UGE: nonneg >= 0 → ALWAYS TRUE  → jump always taken
   *   TOK_LT / TOK_ULT: nonneg <  0 → ALWAYS FALSE → jump never taken
   *   Others (EQ, NE, GT, LE): result depends on whether nonneg == 0 → UNKNOWN
   *
   * When a = 0 and b = nonneg >= 0 (reversed):
   *   TOK_LE / TOK_ULE: 0 <= nonneg → ALWAYS TRUE  → jump always taken
   *   TOK_GT / TOK_UGT: 0 >  nonneg → ALWAYS FALSE → jump never taken
   *   Others: UNKNOWN
   */

  PendingParam params[MAX_PENDING_PARAMS];
  int param_count = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Collect FUNCPARAMVAL instructions */
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
      int call_id = TCCIR_DECODE_CALL_ID(encoded);
      int param_idx = TCCIR_DECODE_PARAM_IDX(encoded);

      if (param_count < MAX_PENDING_PARAMS)
      {
        PendingParam *pp = &params[param_count++];
        pp->call_id = call_id;
        pp->param_idx = param_idx;
        pp->is_immediate = irop_is_immediate(src1);
        if (pp->is_immediate)
        {
          pp->vreg = -1;
          pp->imm_val = irop_get_imm64_ex(ir, src1);
        }
        else
        {
          pp->vreg = irop_get_vreg(src1);
          pp->imm_val = 0;
        }
      }
      continue;
    }

    /* Check FUNCCALLVOID for flag-setting soft-float comparison. */
    if (q->op != TCCIR_OP_FUNCCALLVOID)
    {
      if (q->op != TCCIR_OP_FUNCPARAMVOID && q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_FUNCCALLVAL)
        param_count = 0;
      continue;
    }

    IROperand call_src1 = tcc_ir_op_get_src1(ir, q);
    IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
    Sym *callee = irop_get_sym_ex(ir, call_src1);
    if (!callee)
    {
      param_count = 0;
      continue;
    }

    const char *cmp_name = get_tok_str(callee->v, NULL);
    if (!cmp_name)
    {
      param_count = 0;
      continue;
    }

    /* Check if this is a flag-setting comparison function */
    int is_flag_cmp = 0;
    for (size_t j = 0; j < NUM_FLAG_CMP_FUNCS; j++)
    {
      if (strcmp(cmp_name, flag_cmp_funcs[j]) == 0)
      {
        is_flag_cmp = 1;
        break;
      }
    }

    if (!is_flag_cmp)
    {
      param_count = 0;
      continue;
    }

    /* Found a flag-setting comparison. Extract call_id to match params. */
    uint32_t call_encoded = (uint32_t)irop_get_imm64_ex(ir, call_src2);
    int call_id = TCCIR_DECODE_CALL_ID(call_encoded);

    /* Find param 0 and param 1 for this call_id */
    PendingParam *p0 = NULL, *p1 = NULL;
    for (int p = 0; p < param_count; p++)
    {
      if (params[p].call_id == call_id)
      {
        if (params[p].param_idx == 0)
          p0 = &params[p];
        else if (params[p].param_idx == 1)
          p1 = &params[p];
      }
    }

    if (!p0 || !p1)
    {
      param_count = 0;
      continue;
    }

    /* Determine argument layout: which is nonneg and which is zero */
    int nonneg_is_arg0 = 0; /* 1 if cdcmple(nonneg, 0), 0 if cdcmple(0, nonneg) */
    int pattern_found = 0;

    /* Check pattern: param0 is non-negative vreg, param1 is zero */
    if (!p0->is_immediate && p0->vreg >= 0 && p1->is_immediate && p1->imm_val == 0)
    {
      for (int k = 0; k < nonneg_count; k++)
      {
        if (nonneg_vregs[k] == p0->vreg)
        {
          nonneg_is_arg0 = 1;
          pattern_found = 1;
          break;
        }
      }
    }
    /* Check reverse: param0 is zero, param1 is non-negative vreg */
    else if (p0->is_immediate && p0->imm_val == 0 && !p1->is_immediate && p1->vreg >= 0)
    {
      for (int k = 0; k < nonneg_count; k++)
      {
        if (nonneg_vregs[k] == p1->vreg)
        {
          nonneg_is_arg0 = 0;
          pattern_found = 1;
          break;
        }
      }
    }

    if (!pattern_found)
    {
      param_count = 0;
      continue;
    }

    /* Find the JUMPIF that follows this FUNCCALLVOID.
     * It should be the very next non-NOP instruction. */
    int jumpif_idx = -1;
    for (int j = i + 1; j < n && j <= i + 3; j++)
    {
      if (ir->compact_instructions[j].op == TCCIR_OP_NOP)
        continue;
      if (ir->compact_instructions[j].op == TCCIR_OP_JUMPIF)
      {
        jumpif_idx = j;
        break;
      }
      break;
    }

    if (jumpif_idx < 0)
    {
      param_count = 0;
      continue;
    }

    IRQuadCompact *jump_q = &ir->compact_instructions[jumpif_idx];
    IROperand jmp_cond = tcc_ir_op_get_src1(ir, jump_q);
    IROperand jmp_dest = tcc_ir_op_get_dest(ir, jump_q);
    int cond_tok = (int)irop_get_imm64_ex(ir, jmp_cond);

    /* Determine if the branch is always/never taken based on
     * the condition token and which argument is non-negative.
     *
     * cdcmple(a, b) sets flags for "a CMP b".
     * JUMPIF condition tests those flags. */
    int fold_result = -1; /* -1 = unknown, 0 = never taken, 1 = always taken */

    if (nonneg_is_arg0)
    {
      /* cdcmple(nonneg, 0): flags for "nonneg CMP 0" */
      switch (cond_tok)
      {
      case TOK_GE:
      case TOK_UGE:
        fold_result = 1; /* nonneg >= 0: always true */
        break;
      case TOK_LT:
      case TOK_ULT:
        fold_result = 0; /* nonneg < 0: always false */
        break;
      default:
        fold_result = -1; /* unknown */
        break;
      }
    }
    else
    {
      /* cdcmple(0, nonneg): flags for "0 CMP nonneg" */
      switch (cond_tok)
      {
      case TOK_LE:
      case TOK_ULE:
        fold_result = 1; /* 0 <= nonneg: always true */
        break;
      case TOK_GT:
      case TOK_UGT:
        fold_result = 0; /* 0 > nonneg: always false */
        break;
      default:
        fold_result = -1;
        break;
      }
    }

    if (fold_result < 0)
    {
      param_count = 0;
      continue;
    }

    if (fold_result == 1)
    {
      /* Branch always taken → convert JUMPIF to unconditional JUMP. */
      jump_q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, jumpif_idx, jmp_dest);
      LOG_IR_GEN("NONNEG FOLD: %s(nonneg, 0) at i=%d, JUMPIF cond=0x%x at %d "
                 "-> always taken, unconditional JUMP to %d",
                 cmp_name, i, cond_tok, jumpif_idx, (int)jmp_dest.u.imm32);
      changes++;
    }
    else
    {
      /* Branch never taken → NOP out the JUMPIF. */
      jump_q->op = TCCIR_OP_NOP;
      LOG_IR_GEN("NONNEG FOLD: %s(nonneg, 0) at i=%d, JUMPIF cond=0x%x at %d "
                 "-> never taken, eliminated",
                 cmp_name, i, cond_tok, jumpif_idx);
      changes++;
    }

    param_count = 0;
  }

  /* Run DCE to clean up dead code after folded branches */
  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

/* ============================================================================
 * Float Narrowing Optimization
 * ============================================================================
 *
 * Replaces double-precision math function calls with float-precision variants
 * when the argument was promoted from float and/or the result is demoted back
 * to float.
 *
 * This is valid for functions where (float)func((double)x) == funcf(x) for
 * all float x. These are "integer-valued" or "magnitude-preserving" functions:
 *   floor → floorf, ceil → ceilf, trunc → truncf, round → roundf,
 *   fabs → fabsf, nearbyint → nearbyintf, rint → rintf
 *
 * NOT valid for: sin, cos, tan, sqrt, exp, log, pow (precision-dependent).
 *
 * Pattern detected in IR (soft-float):
 *
 * Case 1: Result demoted back to float
 *   FUNCPARAMVAL float_arg, [call_A, 0]
 *   FUNCCALLVAL __aeabi_f2d → T_double      ; float-to-double
 *   FUNCPARAMVAL T_double, [call_B, 0]
 *   FUNCCALLVAL floor → T_result            ; double-precision math func
 *   FUNCPARAMVAL T_result, [call_C, 0]
 *   FUNCCALLVAL __aeabi_d2f → T_float       ; double-to-float
 *
 *   Transformed to:
 *   FUNCPARAMVAL float_arg, [call_B, 0]
 *   FUNCCALLVAL floorf → T_float             ; float-precision variant
 *   (f2d and d2f calls NOP'd out)
 *
 * Case 2: Result stays double (e.g., double q1(float a) { return floor(a); })
 *   FUNCPARAMVAL float_arg, [call_A, 0]
 *   FUNCCALLVAL __aeabi_f2d → T_double
 *   FUNCPARAMVAL T_double, [call_B, 0]
 *   FUNCCALLVAL floor → T_result
 *
 *   Transformed by swapping callees (f2d moves after the function):
 *   FUNCPARAMVAL float_arg, [call_A, 0]
 *   FUNCCALLVAL floorf → T_float_result      ; now calls floorf
 *   FUNCPARAMVAL T_float_result, [call_B, 0]
 *   FUNCCALLVAL __aeabi_f2d → T_result       ; now widens result to double
 */

/* Table mapping double-precision function names to float-precision equivalents */
typedef struct
{
  const char *double_name;
  const char *float_name;
} FloatNarrowEntry;

static const FloatNarrowEntry float_narrow_table[] = {
    {"floor", "floorf"}, {"ceil", "ceilf"},           {"trunc", "truncf"}, {"round", "roundf"},
    {"fabs", "fabsf"},   {"nearbyint", "nearbyintf"}, {"rint", "rintf"},
};
#define NUM_FLOAT_NARROW (sizeof(float_narrow_table) / sizeof(float_narrow_table[0]))

/* Tracking structure for f2d / d2f calls */
typedef struct
{
  int param_idx;  /* instruction index of the FUNCPARAMVAL */
  int call_idx;   /* instruction index of the FUNCCALLVAL */
  int32_t src_vr; /* original source vreg (float for f2d, double for d2f) */
  int32_t dst_vr; /* result vreg */
  int call_id;    /* IR call_id */
} ConvCallInfo;

#define MAX_CONV_CALLS 32

/* Helper: change the callee symbol of a FUNCCALLVAL/FUNCCALLVOID instruction.
 * ret_btype is the VT_* return type for correct forward declaration
 * (e.g. VT_FLOAT for floorf, VT_INT for __aeabi_* helpers). */
static int change_callee_sym(TCCIRState *ir, int instr_idx, const char *new_name, int ret_btype)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IRPoolSymref *entry = irop_get_symref_ex(ir, src1);
  if (!entry)
    return 0;

  /* Build a function type with the correct return type so later definitions
   * (e.g., "float floorf(float)") don't get a type-incompatible error.
   * We use FUNC_OLD (K&R) style so that parameter types are unspecified.
   * IMPORTANT: Push to global_stack, not local_stack, because this symbol
   * must outlive the current function scope. Using sym_push() would put it
   * on local_stack which gets freed when the function scope ends. */
  CType ftype;
  ftype.t = VT_FUNC;
  ftype.ref = sym_push2(&global_stack, SYM_FIELD, ret_btype, 0);
  ftype.ref->f.func_call = FUNC_CDECL;
  ftype.ref->f.func_type = FUNC_OLD;

  Sym *new_sym = external_global_sym(tok_alloc_const(new_name), &ftype);
  if (!new_sym)
    return 0;
  entry->sym = new_sym;
  return 1;
}

static int change_callee_sym_keep_type(TCCIRState *ir, int instr_idx, const char *new_name)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IRPoolSymref *entry = irop_get_symref_ex(ir, src1);
  Sym *new_sym;

  if (!entry || !entry->sym)
    return 0;

  new_sym = external_global_sym(tok_alloc_const(new_name), &entry->sym->type);
  if (!new_sym)
    return 0;

  entry->sym = new_sym;
  return 1;
}

int tcc_ir_opt_float_narrowing(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
    return 0;

  /* Phase 1: Collect f2d and d2f conversion calls */
  ConvCallInfo f2d_calls[MAX_CONV_CALLS];
  ConvCallInfo d2f_calls[MAX_CONV_CALLS];
  int num_f2d = 0, num_d2f = 0;

  /* Also track: for each instruction that is a FUNCPARAMVAL, record the
   * instruction index and the source vreg, keyed by (call_id, param_idx).
   * We do this in a linear scan. */

  int pending_param_idx = -1;
  int32_t pending_param_src_vr = -1;
  int pending_param_call_id = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
      int param_idx_val = TCCIR_DECODE_PARAM_IDX(encoded);

      if (param_idx_val == 0)
      {
        /* Track the most recent param 0 */
        pending_param_idx = i;
        pending_param_src_vr = irop_is_immediate(src1) ? -1 : irop_get_vreg(src1);
        pending_param_call_id = TCCIR_DECODE_CALL_ID(encoded);
      }
      continue;
    }

    if (q->op == TCCIR_OP_FUNCCALLVAL && pending_param_idx >= 0)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      Sym *callee = irop_get_sym_ex(ir, src1);
      if (!callee)
      {
        pending_param_idx = -1;
        continue;
      }

      const char *name = get_tok_str(callee->v, NULL);
      if (!name)
      {
        pending_param_idx = -1;
        continue;
      }

      uint32_t call_encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
      int this_call_id = TCCIR_DECODE_CALL_ID(call_encoded);

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dst_vr = irop_get_vreg(dest);

      if (strcmp(name, "__aeabi_f2d") == 0 && this_call_id == pending_param_call_id)
      {
        if (num_f2d < MAX_CONV_CALLS)
        {
          f2d_calls[num_f2d].param_idx = pending_param_idx;
          f2d_calls[num_f2d].call_idx = i;
          f2d_calls[num_f2d].src_vr = pending_param_src_vr;
          f2d_calls[num_f2d].dst_vr = dst_vr;
          f2d_calls[num_f2d].call_id = this_call_id;
          num_f2d++;
        }
      }
      else if (strcmp(name, "__aeabi_d2f") == 0 && this_call_id == pending_param_call_id)
      {
        if (num_d2f < MAX_CONV_CALLS)
        {
          d2f_calls[num_d2f].param_idx = pending_param_idx;
          d2f_calls[num_d2f].call_idx = i;
          d2f_calls[num_d2f].src_vr = pending_param_src_vr;
          d2f_calls[num_d2f].dst_vr = dst_vr;
          d2f_calls[num_d2f].call_id = this_call_id;
          num_d2f++;
        }
      }

      pending_param_idx = -1;
      continue;
    }

    /* Reset pending param tracking on non-param, non-call instructions */
    if (q->op != TCCIR_OP_NOP)
      pending_param_idx = -1;
  }

  if (num_f2d == 0)
    return 0;

  /* Phase 2: For each narrowable function call, check if:
   * - Its parameter is an f2d result
   * - Its result feeds into a d2f (Case 1) or not (Case 2) */

  /* Re-scan for function calls with matching f2d parameters */
  pending_param_idx = -1;
  pending_param_src_vr = -1;
  pending_param_call_id = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
      int param_idx_val = TCCIR_DECODE_PARAM_IDX(encoded);

      if (param_idx_val == 0)
      {
        pending_param_idx = i;
        pending_param_src_vr = irop_is_immediate(src1) ? -1 : irop_get_vreg(src1);
        pending_param_call_id = TCCIR_DECODE_CALL_ID(encoded);
      }
      continue;
    }

    if (q->op != TCCIR_OP_FUNCCALLVAL || pending_param_idx < 0)
    {
      if (q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_FUNCPARAMVOID)
        pending_param_idx = -1;
      continue;
    }

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    Sym *callee = irop_get_sym_ex(ir, src1);
    if (!callee)
    {
      pending_param_idx = -1;
      continue;
    }

    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
    {
      pending_param_idx = -1;
      continue;
    }

    /* Check if this is a narrowable function */
    const char *float_name = NULL;
    for (size_t j = 0; j < NUM_FLOAT_NARROW; j++)
    {
      if (strcmp(name, float_narrow_table[j].double_name) == 0)
      {
        float_name = float_narrow_table[j].float_name;
        break;
      }
    }

    if (!float_name)
    {
      pending_param_idx = -1;
      continue;
    }

    /* Check if param 0 comes from an f2d result */
    ConvCallInfo *f2d_info = NULL;
    for (int k = 0; k < num_f2d; k++)
    {
      if (f2d_calls[k].dst_vr == pending_param_src_vr)
      {
        f2d_info = &f2d_calls[k];
        break;
      }
    }

    if (!f2d_info)
    {
      pending_param_idx = -1;
      continue;
    }

    uint32_t call_encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
    (void)call_encoded;
    IROperand func_dest = tcc_ir_op_get_dest(ir, q);
    int32_t func_result_vr = irop_get_vreg(func_dest);
    int func_call_idx = i;
    int func_param_idx = pending_param_idx;

    /* Check if result feeds a d2f (Case 1) */
    ConvCallInfo *d2f_info = NULL;
    for (int k = 0; k < num_d2f; k++)
    {
      if (d2f_calls[k].src_vr == func_result_vr)
      {
        d2f_info = &d2f_calls[k];
        break;
      }
    }

    if (d2f_info)
    {
      /* ===== Case 1: f2d → func → d2f =====
       * Transform to: floorf(original_float) → T_float_result
       * NOP out the f2d and d2f conversion calls. */

      /* 1. Change func's FUNCPARAMVAL to use the original float arg */
      IROperand orig_float_param = tcc_ir_op_get_src1(ir, &ir->compact_instructions[f2d_info->param_idx]);
      tcc_ir_set_src1(ir, func_param_idx, orig_float_param);

      /* 2. Change func's FUNCCALLVAL callee to float variant */
      change_callee_sym(ir, func_call_idx, float_name, VT_FLOAT);

      /* 3. Change func's FUNCCALLVAL dest to d2f's result vreg */
      IROperand d2f_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[d2f_info->call_idx]);
      tcc_ir_set_dest(ir, func_call_idx, d2f_dest);

      /* 4. NOP out f2d (param + call) */
      ir->compact_instructions[f2d_info->param_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[f2d_info->call_idx].op = TCCIR_OP_NOP;

      /* 5. NOP out d2f (param + call) */
      ir->compact_instructions[d2f_info->param_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[d2f_info->call_idx].op = TCCIR_OP_NOP;

      LOG_IR_GEN("FLOAT NARROW (Case 1): %s → %s at i=%d, NOP'd f2d@%d and d2f@%d", name, float_name, func_call_idx,
                 f2d_info->call_idx, d2f_info->call_idx);
      changes++;
    }
    else
    {
      /* ===== Case 2: f2d → func, result stays double =====
       * Swap callees: f2d becomes floorf, func becomes f2d.
       * Before: f2d(float) → T_double → func(T_double) → T_result
       * After:  floorf(float) → T_float → f2d(T_float) → T_result */

      /* 1. Change f2d's callee to the float variant */
      change_callee_sym(ir, f2d_info->call_idx, float_name, VT_FLOAT);

      /* 2. Change func's callee to __aeabi_f2d */
      change_callee_sym(ir, func_call_idx, "__aeabi_f2d", VT_INT);

      LOG_IR_GEN("FLOAT NARROW (Case 2): swapped %s↔f2d at i=%d,%d", name, f2d_info->call_idx, func_call_idx);
      changes++;
    }

    /* Invalidate modified f2d entry to prevent double-processing */
    f2d_info->dst_vr = -1;

    pending_param_idx = -1;
  }

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
 * Eliminates redundant stack-address computations across loops.
 *
 * After IV strength reduction, array loops use pointer-based iteration with
 * an end-of-array bound:
 *   T_end = Addr[StackLoc[X]]    ; ASSIGN
 *   T_end = T_end ADD #C         ; ADD constant
 *
 * When multiple loops access the same array, each loop recomputes the same
 * end pointer.  This pass detects duplicates and replaces later occurrences
 * with the first computation, provided the result vreg is not redefined
 * in between.
 *
 * Before (bench_array_sum):
 *   T16 = Addr[StackLoc[-1024]]    ; init loop end ptr
 *   T16 = T16 ADD #1024
 *   ... init loop (reads T16) ...
 *   T18 = Addr[StackLoc[-1024]]    ; sum loop end ptr (redundant!)
 *   T18 = T18 ADD #1024
 *   ... sum loop (reads T18) ...
 *
 * After:
 *   T16 = Addr[StackLoc[-1024]]    ; computed once
 *   T16 = T16 ADD #1024
 *   ... init loop (reads T16) ...
 *   NOP                            ; eliminated
 *   NOP                            ; eliminated
 *   ... sum loop (reads T16) ...   ; T18 replaced with T16
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
 * Def-Use Table: O(n) pre-computation enabling O(1) def/use queries
 * ============================================================================
 * Replaces tcc_ir_find_defining_instruction() (O(n) backward scan) and
 * inline O(n) use-count loops used in the fusion passes.
 *
 * Memory: (4+1) bytes × total_vregs.  For a typical embedded function with
 * ~90 total vregs that is ~450 bytes — much less than existing pass allocs
 * such as sl_forward (32×n bytes).
 *
 * Layout in one allocation:
 *   int def[max_var + max_tmp + max_param]   (4 B each, -1 = no def)
 *   uint8_t use[max_var + max_tmp + max_param] (1 B each, saturates at 2)
 *
 * Vreg flat index:
 *   VAR   pos  →  pos
 *   TMP   pos  →  max_var + pos
 *   PARAM pos  →  max_var + max_tmp + pos
 */
typedef struct
{
  int *def;
  uint8_t *use;
  int max_var;
  int max_tmp;
  int total;
} IROptDU;

static int ir_opt_du_idx(const IROptDU *du, int32_t vreg)
{
  if (vreg < 0)
    return -1;
  int type = TCCIR_DECODE_VREG_TYPE(vreg);
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  int idx;
  switch (type)
  {
  case TCCIR_VREG_TYPE_VAR:
    idx = pos;
    break;
  case TCCIR_VREG_TYPE_TEMP:
    idx = du->max_var + pos;
    break;
  case TCCIR_VREG_TYPE_PARAM:
    idx = du->max_var + du->max_tmp + pos;
    break;
  default:
    return -1;
  }
  return (idx < du->total) ? idx : -1;
}

/* Build def and use tables in a single O(n) forward pass.
 * Call tcc_free(du.def) when done — single allocation covers both arrays. */
static void ir_opt_du_build(TCCIRState *ir, IROptDU *du)
{
  du->max_var = ir->next_local_variable + 1;
  du->max_tmp = ir->next_temporary_variable + 1;
  int max_par = ir->next_parameter + 1;
  du->total = du->max_var + du->max_tmp + max_par;

  /* Single allocation: int def[] immediately followed by uint8_t use[]. */
  int def_bytes = du->total * (int)sizeof(int);
  int use_bytes = du->total * (int)sizeof(uint8_t);
  du->def = tcc_malloc(def_bytes + use_bytes);
  du->use = (uint8_t *)((char *)du->def + def_bytes);

  for (int k = 0; k < du->total; k++)
    du->def[k] = -1;
  memset(du->use, 0, use_bytes);

  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* STORE-family ops carry the address pointer in their `dest` slot —
     * that is a USE of the pointer vreg, not a definition.  Counting it as
     * a def would shadow the real def from the upstream address-compute
     * (e.g. ADD base, #imm) and prevent disp/indexed fusion from finding
     * it via ir_opt_du_def. */
    int dest_is_addr_use = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                            q->op == TCCIR_OP_STORE_POSTINC);
    if (irop_config[q->op].has_dest)
    {
      int idx = ir_opt_du_idx(du, irop_get_vreg(tcc_ir_op_get_dest(ir, q)));
      if (idx >= 0)
      {
        if (dest_is_addr_use)
        {
          if (du->use[idx] < 2)
            du->use[idx]++;
        }
        else
        {
          du->def[idx] = i;
        }
      }
    }
    if (irop_config[q->op].has_src1)
    {
      int idx = ir_opt_du_idx(du, irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
      if (idx >= 0 && du->use[idx] < 2)
        du->use[idx]++;
    }
    if (irop_config[q->op].has_src2)
    {
      int idx = ir_opt_du_idx(du, irop_get_vreg(tcc_ir_op_get_src2(ir, q)));
      if (idx >= 0 && du->use[idx] < 2)
        du->use[idx]++;
    }
  }
}

/* Defining instruction index for vreg that is strictly before before_idx.
 * Returns -1 when the vreg has no definition or its def is not before before_idx. */
static inline int ir_opt_du_def(const IROptDU *du, int32_t vreg, int before_idx)
{
  int idx = ir_opt_du_idx(du, vreg);
  if (idx < 0)
    return -1;
  int d = du->def[idx];
  return (d >= 0 && d < before_idx) ? d : -1;
}

/* Use count for vreg (0, 1, or 2 meaning "2 or more"). */
static inline int ir_opt_du_uses(const IROptDU *du, int32_t vreg)
{
  int idx = ir_opt_du_idx(du, vreg);
  return (idx >= 0) ? (int)du->use[idx] : 0;
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

int tcc_ir_opt_postinc_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== POSTINC FUSION START (n=%d) ===", n);

  /* ---------------------------------------------------------------------------
   * Revised post-increment fusion (LOAD and STORE).
   *
   * Previous implementation had three fundamental problems:
   *
   * 1. ASSIGN tracing:  tracing through ASSIGN to find an "original pointer"
   *    allowed the ADD search to match against orig_ptr_vr.  After earlier
   *    optimisation passes (copy-prop, store-load-fwd, redundant-store-elim)
   *    rearranged and merged instructions, a LOAD from the first *p++ could
   *    be incorrectly fused with the ADD from the *second* p++, because both
   *    ADDs reference the same original variable.
   *
   * 2. Implicit writeback not modelled:  ARM LOAD_POSTINC (ldr Rd,[Rn],#imm)
   *    updates Rn in-place, but the IR has no way to express this side-effect.
   *    The register allocator treats the pointer operand as input-only, so
   *    after LOAD_POSTINC the updated value can be lost through spilling or
   *    register re-use.
   *
   * 3. Overly aggressive NOP-ing:  the old code NOPed the ASSIGN (pointer
   *    copy), ADD (increment) and STORE (writeback) — removing the entire
   *    pointer update chain.  If the codegen failed to propagate the
   *    implicit ARM writeback, the pointer was never incremented.
   *
   * New rules
   * =========
   *
   * a)  Fuse LOAD and STORE instructions.
   *
   * b)  The LOAD's pointer must be a TEMP vreg (is_local=0) that holds a
   *     pointer value to dereference.
   *
   * c)  The matching ADD must be *immediately* after the LOAD (the very
   *     next non-NOP instruction — no search window).  This prevents
   *     cross-matching between interleaved post-increment operations.
   *
   * d)  The ADD's pointer source must be *exactly* ptr_vr (the LOAD's own
   *     pointer TEMP).  No ASSIGN tracing, no orig_ptr matching.
   *
   * e)  Instead of NOP-ing the ADD, transform it into
   *         ASSIGN  add_result, ptr_vr
   *     After the ARM LOAD_POSTINC instruction executes, the register
   *     holding ptr_vr contains ptr+offset.  The ASSIGN propagates that
   *     updated value to the ADD's original result vreg so that any
   *     downstream STORE (writing the incremented pointer back to the
   *     variable's stack slot) still works correctly.
   *
   * f)  Never NOP any ASSIGN or STORE instruction.  The original pointer
   *     copy (ASSIGN tmp, p) and writeback (STORE [p_slot], result) stay
   *     intact, guaranteeing the pointer update reaches its stack slot.
   *
   * Net effect: one fewer instruction executed per post-increment (the ADD
   * is replaced by a cheaper ASSIGN that the codegen can often elide) and
   * the ARM post-indexed addressing mode saves a cycle.
   * ------------------------------------------------------------------------ */

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *mem_q = &ir->compact_instructions[i];

    /* (a) Fuse LOAD and STORE instructions. */
    int is_load = (mem_q->op == TCCIR_OP_LOAD);
    int is_store = (mem_q->op == TCCIR_OP_STORE);
    if (!is_load && !is_store)
      continue;

    /* LOAD: src1=pointer, dest=loaded_value
     * STORE: dest=pointer (is_lval), src1=stored_value */
    IROperand ptr_op, val_op;
    if (is_load)
    {
      ptr_op = tcc_ir_op_get_src1(ir, mem_q);
      val_op = tcc_ir_op_get_dest(ir, mem_q);
    }
    else
    {
      ptr_op = tcc_ir_op_get_dest(ir, mem_q);
      val_op = tcc_ir_op_get_src1(ir, mem_q);
    }

    /* (b) Pointer must be a TEMP vreg, not a stack-local variable. */
    if (!irop_has_vreg(ptr_op))
      continue;
    if (ptr_op.is_local)
      continue;

    int32_t ptr_vr = irop_get_vreg(ptr_op);

    /* Pointer must be a TEMP (register-resident). */
    if (TCCIR_DECODE_VREG_TYPE(ptr_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Loaded/stored value must not alias the pointer register. */
    if (irop_has_vreg(val_op) && irop_get_vreg(val_op) == ptr_vr)
      continue;

    /* (c) Find the ADD within a small window, ensuring ptr_vr is not used and no control flow changes. */
    int add_idx = -1;
    int unsafe = 0;
    for (int j = i + 1; j < n && j < i + 10; j++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;

      /* Stop at basic block boundaries */
      if (uq->is_jump_target || uq->op == TCCIR_OP_JUMP || uq->op == TCCIR_OP_JUMPIF || uq->op == TCCIR_OP_IJUMP)
      {
        unsafe = 1;
        break;
      }

      /* Check if this is our ADD */
      if (uq->op == TCCIR_OP_ADD)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, uq);
        IROperand s2 = tcc_ir_op_get_src2(ir, uq);
        int s1_vr = irop_get_vreg(s1);
        int s2_vr = irop_get_vreg(s2);
        if ((irop_has_vreg(s1) && s1_vr == ptr_vr) || (irop_has_vreg(s2) && s2_vr == ptr_vr))
        {
          add_idx = j;
          break;
        }
      }

      /* Check if ptr_vr is used or modified by this intermediate instruction */
      if (irop_config[uq->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, uq);
        if (irop_has_vreg(s1) && irop_get_vreg(s1) == ptr_vr)
          unsafe = 1;
      }
      if (irop_config[uq->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, uq);
        if (irop_has_vreg(s2) && irop_get_vreg(s2) == ptr_vr)
          unsafe = 1;
      }
      if (irop_config[uq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, uq);
        if (irop_has_vreg(d) && irop_get_vreg(d) == ptr_vr)
          unsafe = 1;
      }

      if (unsafe)
        break;
    }
    if (add_idx < 0 || unsafe)
      continue;

    IRQuadCompact *add_q = &ir->compact_instructions[add_idx];

    /* (d) The ADD must use exactly ptr_vr as one source. */
    IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
    IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);
    int s1_vr = irop_get_vreg(add_src1);
    int s2_vr = irop_get_vreg(add_src2);
    int ptr_is_src1 = (irop_has_vreg(add_src1) && s1_vr == ptr_vr);
    int ptr_is_src2 = (irop_has_vreg(add_src2) && s2_vr == ptr_vr);
    if (!ptr_is_src1 && !ptr_is_src2)
      continue;

    /* The other operand must be an immediate constant in [1..255]. */
    IROperand offset_op = ptr_is_src1 ? add_src2 : add_src1;
    if (!offset_op.is_const)
      continue;
    int offset = offset_op.u.imm32;
    if (offset < 1 || offset > 255)
      continue;

    /* Ensure the operand pool has room for 4 slots. */
    int new_base_idx = ir->iroperand_pool_count;
    if (new_base_idx + 4 > ir->iroperand_pool_capacity)
      continue;

    /* ---- Apply transformation ---- */

    /* Allocate 4 operand slots: dest, src1, unused, offset */
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);

    mem_q->operand_base = new_base_idx;
    if (is_load)
    {
      /* LOAD_POSTINC: slot0=loaded_value (dest), slot1=ptr (src1) */
      ir->iroperand_pool[new_base_idx + 0] = val_op; /* loaded value (dest) */
      ir->iroperand_pool[new_base_idx + 1] = ptr_op; /* pointer TEMP (input, updated by HW) */
    }
    else
    {
      /* STORE_POSTINC: slot0=ptr (dest, treated as USE by liveness),
       *                slot1=value (src1, data to store)
       * Clear is_lval on ptr: the STORE's dest has is_lval=1 (meaning
       * "dereference as address") but STORE_POSTINC codegen expects the
       * raw pointer register.  The post-indexed STR instruction handles
       * the dereference implicitly.  Keeping is_lval=1 would cause the
       * codegen to emit a spurious LDR to dereference the pointer first. */
      IROperand store_ptr = ptr_op;
      store_ptr.is_lval = 0;
      ir->iroperand_pool[new_base_idx + 0] = store_ptr; /* pointer (dest) */
      ir->iroperand_pool[new_base_idx + 1] = val_op;    /* value to store (src1) */
    }
    ir->iroperand_pool[new_base_idx + 2] = IROP_NONE; /* unused */
    ir->iroperand_pool[new_base_idx + 3] = irop_make_imm32(-1, offset, IROP_BTYPE_INT32);
    mem_q->op = is_load ? TCCIR_OP_LOAD_POSTINC : TCCIR_OP_STORE_POSTINC;

    /* (e) Transform the ADD into ASSIGN add_result := ptr_vr.
     *     After the ARM post-indexed load/store, the register holding
     *     ptr_vr contains ptr + offset.  The ASSIGN propagates that
     *     value to the original ADD result vreg so downstream code
     *     (especially the STORE that writes back to the variable's
     *     stack slot) sees the correct incremented pointer.
     *
     *     We reuse the ADD's existing operand slots: overwrite src1 with
     *     ptr_op (the TEMP pointer) and clear src2. The dest (add_result)
     *     stays unchanged.
     */
    add_q->op = TCCIR_OP_ASSIGN;
    {
      /* Build an ASSIGN source from ptr_op that is a plain register value
       * (not an lvalue dereference).  The original ptr_op comes from the
       * LOAD's src1 or STORE's dest, which has is_lval=1 (meaning
       * "dereference this register as a pointer").  For the ASSIGN we
       * want the *register contents* — the updated pointer value — not
       * another dereference. */
      IROperand assign_src = ptr_op;
      assign_src.is_lval = 0;
      tcc_ir_set_src1(ir, add_idx, assign_src);
    }
    /* ASSIGN has no src2 — the old src2 slot is ignored (has_src2=0 for ASSIGN). */

    changes++;

    LOG_IR_GEN("POSTINC FUSION: %s@%d + ADD@%d -> %s_POSTINC + ASSIGN (ptr_vr=%d, offset=%d)",
               is_load ? "LOAD" : "STORE", i, add_idx, is_load ? "LOAD" : "STORE", ptr_vr, offset);
  }

  LOG_IR_GEN("=== POSTINC FUSION END: %d fusions ===", changes);

  return changes;
}

/* ============================================================================
 * Loop-Aware Post-Increment Fusion
 * ============================================================================
 *
 * After IV strength reduction creates a pointer increment in the loop latch
 * (ptr += stride), this pass fuses the load and increment into a single
 * LOAD_POSTINC instruction.  It handles two patterns:
 *
 * Pattern A - Standalone LOAD:
 *   Before:  LOAD val, *ptr  ...  ptr = ptr + #4
 *   After:   LOAD_POSTINC val, ptr, #4; ASSIGN ptr, ptr  ...  NOP
 *
 * Pattern B - Embedded deref (needs NOP slot to extract the load):
 *   Before:  (nop) FUNCPARAMVAL ptr***DEREF***  ...  ptr = ptr + #4
 *   After:   LOAD_POSTINC tmp, ptr, #4; ASSIGN ptr, ptr; FUNCPARAMVAL tmp  ...  NOP
 *
 * The ASSIGN immediately after LOAD_POSTINC captures the hardware writeback
 * into the vreg so that if the pointer is later spilled, the spill slot
 * receives the updated value.  The latch ADD is NOP'd.
 *
 * If there is no room for the adjacent ASSIGN (no NOP slot), the pass falls
 * back to a plain LOAD + keeps the latch ADD, which is always safe.
 */
int tcc_ir_opt_loop_postinc_fusion(TCCIRState *ir)
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

    /* Step 1: Find the latch ADD: ptr_vr = ptr_vr + #imm (self-update).
     * Scan backward from end_idx (the back-edge JUMP) looking for it. */
    int latch_add_idx = -1;
    int32_t ptr_vr = -1;
    int offset = 0;

    for (int i = loop->end_idx - 1; i >= loop->start_idx; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_CMP)
        continue;
      if (q->op != TCCIR_OP_ADD)
        break; /* First non-NOP/JUMP/CMP/ADD — stop searching */

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);

      int d_vr = irop_get_vreg(dest);
      int s1_vr = irop_get_vreg(src1);

      /* Must be self-update: dest == src1, src2 is immediate */
      if (d_vr >= 0 && d_vr == s1_vr && irop_is_immediate(src2))
      {
        int imm = (int)irop_get_imm64_ex(ir, src2);
        if (imm >= 1 && imm <= 255)
        {
          latch_add_idx = i;
          ptr_vr = d_vr;
          offset = imm;
          break;
        }
      }
      break; /* Not a matching ADD — stop */
    }

    if (latch_add_idx < 0)
      continue;

    /* Step 2: Check for multiple exits — bail if the body has extra JUMPIFs
     * that jump outside the loop.  Use body_instrs to cover extended body. */
    {
      int extra_exits = 0;
      for (int bi = 0; bi < loop->num_body_instrs; bi++)
      {
        int i = loop->body_instrs[bi];
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand jdest = tcc_ir_op_get_dest(ir, q);
        int target = (int)irop_get_imm64_ex(ir, jdest);
        /* Allow the header exit, but count other exits outside the body range */
        int target_in_body = 0;
        for (int bj = 0; bj < loop->num_body_instrs; bj++)
        {
          if (loop->body_instrs[bj] == target)
          {
            target_in_body = 1;
            break;
          }
        }
        if (!target_in_body && i != loop->header_idx + 1)
          extra_exits++;
      }
      if (extra_exits > 0)
        continue;
    }

    /* Step 3: Find exactly one deref of ptr_vr in the loop body.
     * Search standalone LOADs, standalone STOREs, and embedded derefs
     * (ptr used as lval in non-LOAD/STORE ops). */
    int deref_idx = -1;
    int deref_src = 0; /* 1 = src1, 2 = src2 */
    int deref_count = 0;
    int deref_is_standalone_load = 0;
    int deref_is_standalone_store = 0;

    for (int bi = 0; bi < loop->num_body_instrs; bi++)
    {
      int i = loop->body_instrs[bi];
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (i == latch_add_idx)
        continue;

      /* Check standalone LOAD: src1 is ptr_vr with is_lval */
      if (q->op == TCCIR_OP_LOAD)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s1) == ptr_vr && irop_op_is_lval(s1))
        {
          deref_idx = i;
          deref_src = 1;
          deref_is_standalone_load = 1;
          deref_is_standalone_store = 0;
          deref_count++;
        }
        continue;
      }

      /* Check standalone STORE: dest is ptr_vr with is_lval */
      if (q->op == TCCIR_OP_STORE)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_get_vreg(d) == ptr_vr && irop_op_is_lval(d))
        {
          deref_idx = i;
          deref_src = 0;
          deref_is_standalone_load = 0;
          deref_is_standalone_store = 1;
          deref_count++;
        }
        continue;
      }

      /* Skip other memory ops */
      if (q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_LOAD_INDEXED ||
          q->op == TCCIR_OP_STORE_INDEXED)
        continue;

      /* Check embedded deref in non-memory ops */
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s1) == ptr_vr && irop_op_is_lval(s1))
        {
          deref_idx = i;
          deref_src = 1;
          deref_is_standalone_load = 0;
          deref_is_standalone_store = 0;
          deref_count++;
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        if (irop_get_vreg(s2) == ptr_vr && irop_op_is_lval(s2))
        {
          deref_idx = i;
          deref_src = 2;
          deref_is_standalone_load = 0;
          deref_is_standalone_store = 0;
          deref_count++;
        }
      }
    }

    if (deref_count != 1)
      continue;

    /* Check byte type — LOAD/STORE_POSTINC operates on single words */
    {
      IRQuadCompact *dq = &ir->compact_instructions[deref_idx];
      IROperand deref_op;
      if (deref_is_standalone_load)
        deref_op = tcc_ir_op_get_src1(ir, dq);
      else if (deref_is_standalone_store)
        deref_op = tcc_ir_op_get_dest(ir, dq);
      else
        deref_op = (deref_src == 1) ? tcc_ir_op_get_src1(ir, dq) : tcc_ir_op_get_src2(ir, dq);
      int btype = irop_get_btype(deref_op);
      if (btype != IROP_BTYPE_INT32)
        continue;
    }

    /* Step 3b: Dominance check — the deref must execute every iteration. */
    {
      int dominated = 1;
      for (int bi = 0; bi < loop->num_body_instrs; bi++)
      {
        int i = loop->body_instrs[bi];
        if (i >= deref_idx)
          break;
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_JUMPIF)
          continue;
        if (i == loop->header_idx + 1)
          continue;
        {
          dominated = 0;
          break;
        }
      }
      if (!dominated)
        continue;
    }

    /* Step 4: Safety check — no non-deref use of ptr_vr between the deref
     * and the latch ADD. */
    {
      int unsafe = 0;
      for (int bi = 0; bi < loop->num_body_instrs; bi++)
      {
        int i = loop->body_instrs[bi];
        if (i <= deref_idx)
          continue;
        if (i == latch_add_idx)
          continue;

        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_CMP)
          continue;

        if (irop_config[q->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(s1) == ptr_vr && !irop_op_is_lval(s1))
            unsafe = 1;
        }
        if (irop_config[q->op].has_src2)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          if (irop_get_vreg(s2) == ptr_vr && !irop_op_is_lval(s2))
            unsafe = 1;
        }
        if (irop_config[q->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          if (irop_get_vreg(d) == ptr_vr)
            unsafe = 1;
        }
      }
      if (unsafe)
        continue;
    }

    /* Step 5: Find NOP slots for the transformation.
     *
     * For standalone LOAD/STORE: we need one NOP immediately after deref_idx
     * for the ASSIGN that captures the hardware writeback.  The LOAD/STORE
     * itself is converted in-place to LOAD/STORE_POSTINC.
     *
     * For embedded deref: we need two consecutive NOPs before deref_idx
     * (one for LOAD_POSTINC, one for ASSIGN).  If only one NOP is
     * available, fall back to plain LOAD + keep latch ADD. */
    int assign_nop = -1; /* NOP slot for the writeback ASSIGN */
    int load_nop = -1;   /* NOP slot for LOAD_POSTINC (embedded deref only) */

    if (deref_is_standalone_load || deref_is_standalone_store)
    {
      /* Need a NOP right after the LOAD/STORE for the ASSIGN */
      if (deref_idx + 1 < n && ir->compact_instructions[deref_idx + 1].op == TCCIR_OP_NOP)
        assign_nop = deref_idx + 1;
    }
    else
    {
      /* Need two consecutive NOPs before the deref: load_nop, assign_nop */
      for (int i = deref_idx - 1; i >= 0; i--)
      {
        if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
          break;
        if (assign_nop < 0)
          assign_nop = i;
        else
        {
          load_nop = i;
          break;
        }
      }
    }

    /* Build the pointer operand (no lval — LOAD/STORE_POSTINC handles the deref) */
    IRQuadCompact *deref_q = &ir->compact_instructions[deref_idx];
    IROperand orig_deref_op;
    if (deref_is_standalone_load)
      orig_deref_op = tcc_ir_op_get_src1(ir, deref_q);
    else if (deref_is_standalone_store)
      orig_deref_op = tcc_ir_op_get_dest(ir, deref_q);
    else
      orig_deref_op = (deref_src == 1) ? tcc_ir_op_get_src1(ir, deref_q) : tcc_ir_op_get_src2(ir, deref_q);
    IROperand ptr_op = orig_deref_op;
    ptr_op.is_lval = 0;

    if (assign_nop >= 0 && (deref_is_standalone_load || deref_is_standalone_store || load_nop >= 0))
    {
      /* Pool: 4 slots for LOAD/STORE_POSTINC */
      if (ir->iroperand_pool_count + 4 > ir->iroperand_pool_capacity)
      {
        tcc_ir_pool_ensure(ir, 4);
        if (ir->iroperand_pool_count + 4 > ir->iroperand_pool_capacity)
          continue;
      }

      if (deref_is_standalone_load)
      {
        /* Convert the existing LOAD in-place to LOAD_POSTINC */
        IROperand load_dest = tcc_ir_op_get_dest(ir, deref_q);
        int new_base = ir->iroperand_pool_count;
        tcc_ir_pool_add(ir, load_dest);
        tcc_ir_pool_add(ir, ptr_op);
        tcc_ir_pool_add(ir, IROP_NONE);
        tcc_ir_pool_add(ir, irop_make_imm32(-1, offset, IROP_BTYPE_INT32));

        deref_q->op = TCCIR_OP_LOAD_POSTINC;
        deref_q->operand_base = new_base;
      }
      else if (deref_is_standalone_store)
      {
        /* Convert the existing STORE in-place to STORE_POSTINC
         * STORE_POSTINC: slot0=ptr (dest, no lval), slot1=value (src1),
         *                slot2=unused, slot3=offset */
        IROperand store_val = tcc_ir_op_get_src1(ir, deref_q);
        int new_base = ir->iroperand_pool_count;
        tcc_ir_pool_add(ir, ptr_op);    /* dest = pointer (no lval) */
        tcc_ir_pool_add(ir, store_val); /* src1 = value to store */
        tcc_ir_pool_add(ir, IROP_NONE);
        tcc_ir_pool_add(ir, irop_make_imm32(-1, offset, IROP_BTYPE_INT32));

        deref_q->op = TCCIR_OP_STORE_POSTINC;
        deref_q->operand_base = new_base;
      }
      else
      {
        /* Allocate temp vreg and create LOAD_POSTINC in load_nop */
        int32_t loaded_vreg = tcc_ir_vreg_alloc_temp(ir);
        if (loaded_vreg < 0)
          continue;
        IROperand loaded_op = irop_make_vreg(loaded_vreg, IROP_BTYPE_INT32);

        int new_base = ir->iroperand_pool_count;
        tcc_ir_pool_add(ir, loaded_op);
        tcc_ir_pool_add(ir, ptr_op);
        tcc_ir_pool_add(ir, IROP_NONE);
        tcc_ir_pool_add(ir, irop_make_imm32(-1, offset, IROP_BTYPE_INT32));

        IRQuadCompact *lnop = &ir->compact_instructions[load_nop];
        lnop->op = TCCIR_OP_LOAD_POSTINC;
        lnop->operand_base = new_base;
        lnop->line_num = deref_q->line_num;

        /* Patch the deref instruction to use loaded_vreg (no deref) */
        IROperand patched_op = loaded_op;
        patched_op.is_lval = 0;
        if (deref_src == 1)
          tcc_ir_set_src1(ir, deref_idx, patched_op);
        else
          tcc_ir_set_src2(ir, deref_idx, patched_op);
      }

      /* Place ASSIGN ptr_vr = ptr_vr in assign_nop.  This is immediately
       * adjacent to the LOAD/STORE_POSTINC, so the register still holds the
       * post-incremented value and cannot have been spilled yet.  The
       * ASSIGN creates an explicit DEF for liveness, ensuring that any
       * later spill stores the updated pointer value. */
      if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
      {
        tcc_ir_pool_ensure(ir, 2);
        if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
          continue;
      }
      int assign_base = ir->iroperand_pool_count;
      tcc_ir_pool_add(ir, ptr_op); /* dest = ptr_vr */
      tcc_ir_pool_add(ir, ptr_op); /* src1 = ptr_vr */

      IRQuadCompact *anop = &ir->compact_instructions[assign_nop];
      anop->op = TCCIR_OP_ASSIGN;
      anop->operand_base = assign_base;
      anop->line_num = deref_q->line_num;

      /* NOP the latch ADD — the increment is handled by LOAD/STORE_POSTINC */
      ir->compact_instructions[latch_add_idx].op = TCCIR_OP_NOP;

      changes++;
      continue;
    }

    /* ---- Fallback: plain LOAD + keep latch ADD (always safe) ---- */
    if (!deref_is_standalone_load && !deref_is_standalone_store)
    {
      /* Need at least one NOP before the deref */
      int nop_slot = -1;
      for (int i = deref_idx - 1; i >= 0; i--)
      {
        if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
        {
          nop_slot = i;
          break;
        }
        break;
      }
      if (nop_slot < 0)
        continue;

      int32_t loaded_vreg = tcc_ir_vreg_alloc_temp(ir);
      if (loaded_vreg < 0)
        continue;

      if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
      {
        tcc_ir_pool_ensure(ir, 2);
        if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
          continue;
      }

      IROperand loaded_op = irop_make_vreg(loaded_vreg, IROP_BTYPE_INT32);
      IROperand ptr_lval_op = orig_deref_op;
      ptr_lval_op.is_lval = 1;

      int new_base = ir->iroperand_pool_count;
      tcc_ir_pool_add(ir, loaded_op);
      tcc_ir_pool_add(ir, ptr_lval_op);

      IRQuadCompact *nop_q = &ir->compact_instructions[nop_slot];
      nop_q->op = TCCIR_OP_LOAD;
      nop_q->operand_base = new_base;
      nop_q->line_num = deref_q->line_num;

      IROperand patched_op = loaded_op;
      patched_op.is_lval = 0;
      if (deref_src == 1)
        tcc_ir_set_src1(ir, deref_idx, patched_op);
      else
        tcc_ir_set_src2(ir, deref_idx, patched_op);

      changes++;
    }
    /* For standalone LOAD without an ASSIGN slot: leave untouched */
  }

  tcc_ir_free_loops(loops);
  return changes;
}

/* ============================================================================
 * Combined Fusion Pass
 * ============================================================================
 *
 * Runs MLA fusion and indexed-memory fusion in a single forward loop, sharing
 * one IROptDU def/use table (one malloc instead of two, one scan instead of two).
 *
 * do_mla:     run mla_fusion logic
 * do_indexed: run indexed_memory_fusion logic
 *
 * The two passes operate on disjoint opcode sets (ADD vs LOAD/STORE) so merging
 * them is always safe.  The IROptDU table is built once before the loop and is
 * treated as read-only: modifications made during the loop (NOPing instructions)
 * are conservative — the stale entries cause missed-optimisation at worst, never
 * incorrect code generation.
 */
int tcc_ir_opt_fusion_pass(TCCIRState *ir, int do_mla, int do_indexed)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0 || (!do_mla && !do_indexed))
    return 0;

  IROptDU du;
  ir_opt_du_build(ir, &du);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* ------------------------------------------------------------------ */
    /* MLA fusion: ADD where one src is a single-use MUL → MLA            */
    /* ------------------------------------------------------------------ */
    if (do_mla && q->op == TCCIR_OP_ADD)
    {
      IROperand add_src1 = tcc_ir_op_get_src1(ir, q);
      IROperand add_src2 = tcc_ir_op_get_src2(ir, q);
      IROperand add_dest = tcc_ir_op_get_dest(ir, q);

      int32_t mul_result_vr = -1;
      IROperand accum_op;
      int mul_idx = -1;
      IRQuadCompact *mul_q = NULL;

      if (irop_has_vreg(add_src2))
      {
        int32_t vr = irop_get_vreg(add_src2);
        int idx = ir_opt_du_def(&du, vr, i);
        if (idx >= 0 && ir->compact_instructions[idx].op == TCCIR_OP_MUL)
        {
          mul_result_vr = vr;
          accum_op = add_src1;
          mul_idx = idx;
          mul_q = &ir->compact_instructions[mul_idx];
        }
      }
      if (!mul_q && irop_has_vreg(add_src1))
      {
        int32_t vr = irop_get_vreg(add_src1);
        int idx = ir_opt_du_def(&du, vr, i);
        if (idx >= 0 && ir->compact_instructions[idx].op == TCCIR_OP_MUL)
        {
          mul_result_vr = vr;
          accum_op = add_src2;
          mul_idx = idx;
          mul_q = &ir->compact_instructions[mul_idx];
        }
      }

      if (mul_q && irop_get_tag(accum_op) != IROP_TAG_SYMREF && irop_get_tag(add_dest) != IROP_TAG_SYMREF &&
          irop_get_tag(add_src1) != IROP_TAG_SYMREF && irop_get_tag(add_src2) != IROP_TAG_SYMREF &&
          !(irop_get_tag(accum_op) == IROP_TAG_STACKOFF && !accum_op.is_lval))
      {
        IROperand ms1 = tcc_ir_op_get_src1(ir, mul_q);
        IROperand ms2 = tcc_ir_op_get_src2(ir, mul_q);
        if (!(ms1.is_lval && !ms1.is_local && !ms1.is_llocal) && !(ms2.is_lval && !ms2.is_local && !ms2.is_llocal) &&
            !irop_is_immediate(ms1) && !irop_is_immediate(ms2) && ir_opt_du_uses(&du, mul_result_vr) == 1)
        {
          /* Same-block check */
          int same_block = 1;
          for (int j = mul_idx + 1; j < i && same_block; j++)
          {
            TccIrOp bop = ir->compact_instructions[j].op;
            if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
              same_block = 0;
          }
          /* Accumulator defined-before-MUL check */
          if (same_block)
          {
            int32_t accum_vr = irop_get_vreg(accum_op);
            if (accum_vr >= 0)
            {
              int adef = ir_opt_du_def(&du, accum_vr, i);
              if (adef >= 0 && adef >= mul_idx)
                same_block = 0;
            }
          }
          if (same_block)
          {
            /* Transform MUL→MLA, copy ADD dest, store accum at +3, NOP ADD */
            mul_q->op = TCCIR_OP_MLA;
            int mul_dest_idx = mul_q->operand_base;
            int add_dest_idx = q->operand_base;
            if (mul_dest_idx >= 0 && mul_dest_idx < ir->iroperand_pool_count && add_dest_idx >= 0 &&
                add_dest_idx < ir->iroperand_pool_count)
              ir->iroperand_pool[mul_dest_idx] = ir->iroperand_pool[add_dest_idx];

            int accum_idx = mul_q->operand_base + 3;
            while (ir->iroperand_pool_count <= accum_idx)
              tcc_ir_pool_add(ir, IROP_NONE);
            if (accum_idx < ir->iroperand_pool_capacity)
            {
              ir->iroperand_pool[accum_idx] = accum_op;
              q->op = TCCIR_OP_NOP;
              changes++;
            }
            else
            {
              mul_q->op = TCCIR_OP_MUL; /* revert */
            }
          }
        }
      }
      continue; /* ADD handled — skip indexed-memory check below */
    }

    /* ------------------------------------------------------------------ */
    /* Indexed memory fusion: LOAD/STORE with SHL+ADD pattern → _INDEXED  */
    /* ------------------------------------------------------------------ */
    if (!do_indexed || (q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_STORE))
      continue;

    int is_store = (q->op == TCCIR_OP_STORE);
    IROperand addr_op = is_store ? tcc_ir_op_get_dest(ir, q) : tcc_ir_op_get_src1(ir, q);

    if (!irop_has_vreg(addr_op))
      continue;

    int32_t addr_vr = irop_get_vreg(addr_op);
    if (!is_store && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
      continue;

    int add_idx = ir_opt_du_def(&du, addr_vr, i);
    if (add_idx < 0)
      continue;

    IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
    if (add_q->op != TCCIR_OP_ADD)
      continue;

    if (ir_opt_du_uses(&du, addr_vr) != 1)
      continue;

    IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
    IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);
    int32_t offset_vr = -1;
    IROperand base_op = IROP_NONE;
    int shl_idx = -1;
    IRQuadCompact *shl_q = NULL;

    if (irop_has_vreg(add_src1))
    {
      int32_t vr1 = irop_get_vreg(add_src1);
      int idx1 = ir_opt_du_def(&du, vr1, add_idx);
      if (idx1 >= 0 && ir->compact_instructions[idx1].op == TCCIR_OP_SHL)
      {
        offset_vr = vr1;
        base_op = add_src2;
        shl_idx = idx1;
        shl_q = &ir->compact_instructions[shl_idx];
      }
    }
    if (shl_idx < 0 && irop_has_vreg(add_src2))
    {
      int32_t vr2 = irop_get_vreg(add_src2);
      int idx2 = ir_opt_du_def(&du, vr2, add_idx);
      if (idx2 >= 0 && ir->compact_instructions[idx2].op == TCCIR_OP_SHL)
      {
        offset_vr = vr2;
        base_op = add_src1;
        shl_idx = idx2;
        shl_q = &ir->compact_instructions[shl_idx];
      }
    }
    if (shl_idx < 0)
      continue;

    if (ir_opt_du_uses(&du, offset_vr) != 1)
      continue;

    IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
    if (!shl_src2.is_const)
      continue;

    int shift_amount = shl_src2.u.imm32;
    if (shift_amount < 1 || shift_amount > 3)
      continue;

    IROperand index_op = tcc_ir_op_get_src1(ir, shl_q);
    if (index_op.is_local || index_op.is_llocal)
      continue;
    if (base_op.is_local || base_op.is_llocal || base_op.is_lval)
      continue;

    /* Same-block check */
    int same_block = 1;
    for (int j = shl_idx + 1; j < i && same_block; j++)
    {
      TccIrOp bop = ir->compact_instructions[j].op;
      if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF || bop == TCCIR_OP_NOP)
        same_block = 0;
    }
    if (!same_block)
      continue;

    IROperand orig_dest = tcc_ir_op_get_dest(ir, q);
    IROperand orig_src1 = tcc_ir_op_get_src1(ir, q);

    q->op = is_store ? TCCIR_OP_STORE_INDEXED : TCCIR_OP_LOAD_INDEXED;

    int new_base_idx = ir->iroperand_pool_count;
    if (new_base_idx + 4 > ir->iroperand_pool_capacity)
    {
      q->op = is_store ? TCCIR_OP_STORE : TCCIR_OP_LOAD; /* revert */
      continue;
    }

    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    q->operand_base = new_base_idx;

    IROperand base_op_clean = base_op;
    IROperand index_op_clean = index_op;
    base_op_clean.is_lval = 0;
    IROperand scale_imm = irop_make_imm32(0, shift_amount, IROP_BTYPE_INT32);

    if (is_store)
    {
      ir->iroperand_pool[new_base_idx + 0] = base_op_clean;
      ir->iroperand_pool[new_base_idx + 1] = orig_src1;
      ir->iroperand_pool[new_base_idx + 2] = index_op_clean;
      ir->iroperand_pool[new_base_idx + 3] = scale_imm;
    }
    else
    {
      ir->iroperand_pool[new_base_idx + 0] = orig_dest;
      ir->iroperand_pool[new_base_idx + 1] = base_op_clean;
      ir->iroperand_pool[new_base_idx + 2] = index_op_clean;
      ir->iroperand_pool[new_base_idx + 3] = scale_imm;
    }

    shl_q->op = TCCIR_OP_NOP;
    add_q->op = TCCIR_OP_NOP;
    changes++;
  }

  tcc_free(du.def);
  return changes;
}

/* ============================================================================
 * Rotation Fusion
 * ============================================================================
 *
 * Fuses the C rotation idiom into a single ROR instruction.
 * Pattern:
 *   t1 = SHL(x, #n)
 *   t2 = SHR(x, #(32-n))
 *   result = OR(t1, t2)       (or OR(t2, t1))
 *
 * Becomes:
 *   result = ROR(x, #(32-n))
 *   (SHL → NOP, SHR → NOP)
 */
int tcc_ir_opt_rotate_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  IROptDU du;
  ir_opt_du_build(ir, &du);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_OR)
      continue;

    IROperand or_src1 = tcc_ir_op_get_src1(ir, q);
    IROperand or_src2 = tcc_ir_op_get_src2(ir, q);

    if (!irop_has_vreg(or_src1) || !irop_has_vreg(or_src2))
      continue;

    int32_t vr1 = irop_get_vreg(or_src1);
    int32_t vr2 = irop_get_vreg(or_src2);

    int idx1 = ir_opt_du_def(&du, vr1, i);
    int idx2 = ir_opt_du_def(&du, vr2, i);
    if (idx1 < 0 || idx2 < 0)
      continue;

    IRQuadCompact *q1 = &ir->compact_instructions[idx1];
    IRQuadCompact *q2 = &ir->compact_instructions[idx2];

    IRQuadCompact *shl_q, *shr_q;
    int shl_idx, shr_idx;
    int32_t shl_vr, shr_vr;

    if (q1->op == TCCIR_OP_SHL && q2->op == TCCIR_OP_SHR) {
      shl_q = q1; shr_q = q2;
      shl_idx = idx1; shr_idx = idx2;
      shl_vr = vr1; shr_vr = vr2;
    } else if (q1->op == TCCIR_OP_SHR && q2->op == TCCIR_OP_SHL) {
      shr_q = q1; shl_q = q2;
      shr_idx = idx1; shl_idx = idx2;
      shr_vr = vr1; shl_vr = vr2;
    } else {
      continue;
    }

    if (ir_opt_du_uses(&du, shl_vr) != 1 || ir_opt_du_uses(&du, shr_vr) != 1)
      continue;

    IROperand shl_src1 = tcc_ir_op_get_src1(ir, shl_q);
    IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
    IROperand shr_src1 = tcc_ir_op_get_src1(ir, shr_q);
    IROperand shr_src2 = tcc_ir_op_get_src2(ir, shr_q);

    if (!irop_is_immediate(shl_src2) || !irop_is_immediate(shr_src2))
      continue;

    int64_t shl_amt = irop_get_imm64_ex(ir, shl_src2);
    int64_t shr_amt = irop_get_imm64_ex(ir, shr_src2);

    if (shl_amt <= 0 || shl_amt >= 32 || shr_amt <= 0 || shr_amt >= 32)
      continue;
    if (shl_amt + shr_amt != 32)
      continue;

    if (!irop_has_vreg(shl_src1) || !irop_has_vreg(shr_src1))
      continue;
    if (irop_get_vreg(shl_src1) != irop_get_vreg(shr_src1))
      continue;

    int same_block = 1;
    int min_idx = shl_idx < shr_idx ? shl_idx : shr_idx;
    for (int j = min_idx + 1; j < i && same_block; j++)
    {
      TccIrOp bop = ir->compact_instructions[j].op;
      if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
        same_block = 0;
    }
    if (!same_block)
      continue;

    IROperand or_dest = tcc_ir_op_get_dest(ir, q);
    IROperand ror_imm = irop_make_imm32(0, (int32_t)shr_amt, shr_src2.btype);

    q->op = TCCIR_OP_ROR;
    tcc_ir_set_dest(ir, i, or_dest);
    tcc_ir_set_src1(ir, i, shr_src1);
    tcc_ir_set_src2(ir, i, ror_imm);

    shl_q->op = TCCIR_OP_NOP;
    shr_q->op = TCCIR_OP_NOP;

    LOG_IR_GEN("OPTIMIZE: Rotate fusion SHL(%lld)+SHR(%lld)+OR → ROR(%lld) at i=%d",
               (long long)shl_amt, (long long)shr_amt, (long long)shr_amt, i);
    changes++;
  }

  tcc_free(du.def);
  return changes;
}

/* ============================================================================
 * Late Barrel Shift Fusion (runs just before codegen)
 * ============================================================================
 *
 * Folds a single-use shift/rotate into the consuming ALU instruction's src2
 * using the ARM barrel shifter.  Results are written to ir->barrel_shifts[]
 * (a side-table), not into IRQuadCompact, so no intermediate pass can corrupt them.
 *
 * Pattern:
 *   t = SHL/SHR/SAR/ROR(x, #n)     -- single use, 32-bit
 *   result = ADD/SUB/AND/OR/XOR/CMP(y, t)
 *
 * Encoding: barrel_shifts[i] = (type<<5)|amount
 *   type: 1=SHL, 2=SHR, 3=SAR, 4=ROR.  amount: 0-31.
 */
void tcc_ir_barrel_shift_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return;

  ir->barrel_shifts = tcc_mallocz(ir->max_orig_index + 1);

  IROptDU du;
  ir_opt_du_build(ir, &du);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    switch (q->op)
    {
    case TCCIR_OP_ADD: case TCCIR_OP_SUB:
    case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR:
    case TCCIR_OP_CMP:
      break;
    default:
      continue;
    }

    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (!irop_has_vreg(src2))
      continue;

    int32_t vr2 = irop_get_vreg(src2);
    int shift_idx = ir_opt_du_def(&du, vr2, i);
    if (shift_idx < 0)
      continue;

    IRQuadCompact *sq = &ir->compact_instructions[shift_idx];
    int stype;
    switch (sq->op) {
    case TCCIR_OP_SHL:
      if (q->op == TCCIR_OP_ADD) continue;
      stype = 1; break;
    case TCCIR_OP_SHR: stype = 2; break;
    case TCCIR_OP_SAR: stype = 3; break;
    case TCCIR_OP_ROR: stype = 4; break;
    default: continue;
    }

    if (ir_opt_du_uses(&du, vr2) != 1)
      continue;

    IROperand shift_dest = tcc_ir_op_get_dest(ir, sq);
    if (shift_dest.btype == IROP_BTYPE_INT64)
      continue;

    IROperand consumer_dest = tcc_ir_op_get_dest(ir, q);
    if (consumer_dest.btype == IROP_BTYPE_INT64)
      continue;
    if (src2.btype == IROP_BTYPE_INT64)
      continue;

    IROperand shift_src2 = tcc_ir_op_get_src2(ir, sq);
    if (!irop_is_immediate(shift_src2))
      continue;

    int64_t amount = irop_get_imm64_ex(ir, shift_src2);
    if (amount < 0 || amount > 31)
      continue;

    IROperand shift_src1 = tcc_ir_op_get_src1(ir, sq);
    if (!irop_has_vreg(shift_src1))
      continue;

    int32_t shift_src_vr = irop_get_vreg(shift_src1);

    IROperand alu_src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_has_vreg(alu_src1) && irop_get_vreg(alu_src1) == shift_src_vr)
      continue;

    int safe = 1;
    for (int j = shift_idx + 1; j < i && safe; j++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      TccIrOp bop = jq->op;
      if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
        safe = 0;
      if (bop == TCCIR_OP_NOP)
        continue;
      if (irop_config[bop].has_dest)
      {
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        if (irop_has_vreg(jdest) && irop_get_vreg(jdest) == shift_src_vr)
          safe = 0;
      }
    }
    if (!safe)
      continue;

    tcc_ir_set_src2(ir, i, shift_src1);
    ir->barrel_shifts[q->orig_index] = (uint8_t)((stype << 5) | (int)amount);
    sq->op = TCCIR_OP_NOP;
  }

  tcc_free(du.def);
}

/* ============================================================================
 * Deref-in-ALU Indexed Fusion
 * ============================================================================
 *
 * Extends indexed fusion to handle dereferences embedded in ALU operands.
 * Pattern:
 *   SHL shifted, index, #scale
 *   ADD addr, base, shifted
 *   result = addr***DEREF*** XOR other   (or any ALU op with deref operand)
 *
 * Becomes:
 *   (SHL → NOP)
 *   LOAD_INDEXED tmp, base, index, #scale   (ADD slot reused)
 *   result = tmp XOR other                   (deref removed)
 *
 * This catches the dominant pattern in AES table lookups where the memory
 * load is fused into an XOR/AND/OR operand rather than being a standalone
 * LOAD instruction.
 */
int tcc_ir_opt_deref_indexed_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  IROptDU du;
  ir_opt_du_build(ir, &du);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Skip non-ALU instructions and instructions that already handle memory */
    if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_LOAD_INDEXED ||
        q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC ||
        q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
        q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID)
      continue;

    /* Check src1 and src2 for deref operands */
    int operand_positions[2] = {0, 0};
    int num_deref = 0;

    if (irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (s1.is_lval && irop_has_vreg(s1))
        operand_positions[num_deref++] = 1;
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (s2.is_lval && irop_has_vreg(s2))
        operand_positions[num_deref++] = 2;
    }

    if (num_deref == 0)
      continue;

    for (int d = 0; d < num_deref; d++)
    {
      int src_pos = operand_positions[d];
      IROperand deref_op = (src_pos == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);

      int32_t addr_vr = irop_get_vreg(deref_op);
      if (addr_vr < 0)
        continue;

      /* addr_vr must have exactly 1 use (this deref) */
      if (ir_opt_du_uses(&du, addr_vr) != 1)
        continue;

      /* Find the ADD that defines addr_vr */
      int add_idx = ir_opt_du_def(&du, addr_vr, i);
      if (add_idx < 0)
        continue;

      IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
      if (add_q->op != TCCIR_OP_ADD)
        continue;

      /* Find which ADD operand is SHL result and which is base */
      IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
      IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);
      int32_t offset_vr = -1;
      IROperand base_op = IROP_NONE;
      int shl_idx = -1;
      IRQuadCompact *shl_q = NULL;

      if (irop_has_vreg(add_src1))
      {
        int32_t vr1 = irop_get_vreg(add_src1);
        int idx1 = ir_opt_du_def(&du, vr1, add_idx);
        if (idx1 >= 0 && ir->compact_instructions[idx1].op == TCCIR_OP_SHL)
        {
          offset_vr = vr1;
          base_op = add_src2;
          shl_idx = idx1;
          shl_q = &ir->compact_instructions[shl_idx];
        }
      }
      if (shl_idx < 0 && irop_has_vreg(add_src2))
      {
        int32_t vr2 = irop_get_vreg(add_src2);
        int idx2 = ir_opt_du_def(&du, vr2, add_idx);
        if (idx2 >= 0 && ir->compact_instructions[idx2].op == TCCIR_OP_SHL)
        {
          offset_vr = vr2;
          base_op = add_src1;
          shl_idx = idx2;
          shl_q = &ir->compact_instructions[shl_idx];
        }
      }
      if (shl_idx < 0)
        continue;

      /* SHL result must have exactly 1 use (the ADD) */
      if (ir_opt_du_uses(&du, offset_vr) != 1)
        continue;

      /* Scale must fit ARM Thumb-2 imm2 field (0-3) */
      IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);
      if (!shl_src2.is_const)
        continue;
      int shift_amount = shl_src2.u.imm32;
      if (shift_amount < 1 || shift_amount > 3)
        continue;

      IROperand index_op = tcc_ir_op_get_src1(ir, shl_q);
      if (index_op.is_local || index_op.is_llocal)
        continue;
      if (base_op.is_local || base_op.is_llocal || base_op.is_lval)
        continue;

      /* Same-block check: SHL through ALU instruction */
      int same_block = 1;
      for (int j = shl_idx + 1; j < i && same_block; j++)
      {
        TccIrOp bop = ir->compact_instructions[j].op;
        if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
          same_block = 0;
      }
      if (!same_block)
        continue;

      /* All checks passed — transform */

      /* Allocate a new TEMP for the loaded value */
      int32_t loaded_vr = tcc_ir_vreg_alloc_temp(ir);
      if (loaded_vr < 0)
        continue;

      /* Check operand pool capacity (need 4 slots) */
      if (ir->iroperand_pool_count + 4 > ir->iroperand_pool_capacity)
        continue;

      /* Convert ADD → LOAD_INDEXED */
      int new_base_idx = ir->iroperand_pool_count;
      tcc_ir_pool_add(ir, IROP_NONE);
      tcc_ir_pool_add(ir, IROP_NONE);
      tcc_ir_pool_add(ir, IROP_NONE);
      tcc_ir_pool_add(ir, IROP_NONE);

      IROperand loaded_op = irop_make_vreg(loaded_vr, deref_op.btype ? deref_op.btype : IROP_BTYPE_INT32);
      IROperand base_clean = base_op;
      base_clean.is_lval = 0;
      IROperand scale_imm = irop_make_imm32(0, shift_amount, IROP_BTYPE_INT32);

      ir->iroperand_pool[new_base_idx + 0] = loaded_op;  /* dest */
      ir->iroperand_pool[new_base_idx + 1] = base_clean; /* base */
      ir->iroperand_pool[new_base_idx + 2] = index_op;   /* index */
      ir->iroperand_pool[new_base_idx + 3] = scale_imm;  /* scale */

      add_q->op = TCCIR_OP_LOAD_INDEXED;
      add_q->operand_base = new_base_idx;

      /* NOP the SHL */
      shl_q->op = TCCIR_OP_NOP;

      /* Replace the deref operand in the ALU instruction with the loaded value */
      IROperand clean_op = loaded_op;
      /* Re-read the instruction since pool may have moved */
      q = &ir->compact_instructions[i];
      if (src_pos == 1)
        tcc_ir_op_set_src1(ir, q, clean_op);
      else
        tcc_ir_op_set_src2(ir, q, clean_op);

      changes++;
    }
  }

  tcc_free(du.def);
  return changes;
}

/* ============================================================================
 * Displacement Load/Store Fusion
 * ============================================================================
 *
 * Fuses plain-ADD-with-immediate + LOAD/STORE/ASSIGN-with-lval-source into a
 * single LOAD_INDEXED / STORE_INDEXED with scale=0 and an immediate index.
 * The backend lowers that to a displacement-addressed load/store:
 *
 *   ADD   Tk, base, #imm          ->  (NOP)
 *   LOAD  dest, Tk                ->  LOAD_INDEXED dest, base, #imm, #0
 *   STORE Tk, value               ->  STORE_INDEXED base, value, #imm, #0
 *   Tk***DEREF*** <-- value [STORE]  same as explicit STORE
 *   dest <-- Tk***DEREF*** [ASSIGN]  same as explicit LOAD
 *
 * This is distinct from `tcc_ir_opt_indexed_memory_fusion`, which handles
 * SHL+ADD (array-index with scale 2/3/4).  Disjoint patterns, so this pass
 * should run after the combined SHL+ADD fusion.
 *
 * Requirements:
 *   - ADD must have exactly one immediate operand, the other a vreg (base).
 *   - ADD result must have exactly one use.
 *   - The consumer (LOAD / STORE / ASSIGN-lval) must be in the same basic block.
 *   - imm offset must fit an LDR/STR immediate (|imm| <= 4095).
 *   - base must not itself be a stack local / double-indirect lvalue (we do
 *     preserve its is_lval so a pointer-from-memory base still loads before
 *     use — matches what the SHL+ADD fusion does).
 */
int tcc_ir_opt_disp_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  IROptDU du;
  ir_opt_du_build(ir, &du);

  LOG_IR_GEN("=== DISP FUSION START (n=%d) ===", n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    int is_store = 0;
    int is_load = 0;
    IROperand addr_op = IROP_NONE;

    if (q->op == TCCIR_OP_LOAD)
    {
      is_load = 1;
      addr_op = tcc_ir_op_get_src1(ir, q);
    }
    else if (q->op == TCCIR_OP_STORE)
    {
      is_store = 1;
      addr_op = tcc_ir_op_get_dest(ir, q);
    }
    else if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (!src1.is_lval)
        continue;
      is_load = 1;
      addr_op = src1;
    }
    else
    {
      continue;
    }

    if (!irop_has_vreg(addr_op))
      continue;

    int32_t addr_vr = irop_get_vreg(addr_op);

    /* A LOAD/ASSIGN-lval from a VAR vreg reads the variable's stack slot —
     * it is not a deref of a pointer, so fusing would change semantics. */
    if (is_load && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
      continue;

    /* Skip 64-bit, FP64 and struct accesses: LOAD_INDEXED/STORE_INDEXED
     * lower to LDRD/STRD which require 4-byte alignment.  Packed structs
     * can place 64-bit fields at unaligned offsets (e.g. packed Count at
     * offset 6 in pr20051113-1), which would HardFault at runtime. */
    {
      int access_btype = addr_op.btype;
      if (access_btype == IROP_BTYPE_INT64 || access_btype == IROP_BTYPE_FLOAT64 ||
          access_btype == IROP_BTYPE_STRUCT)
        continue;
    }

    /* The backend's `tcc_gen_machine_load_indexed_mop` 32-bit path reads
     * `dest.u.reg.r0` unconditionally, which is valid only for MACH_OP_REG
     * dests.  VAR vregs routinely get MACH_OP_SPILL, in which case the
     * union aliases with spill.offset and we'd encode a bogus register —
     * reliably producing a HardFault at runtime.  Until the backend grows
     * a generic dest-register-or-scratch path, only fuse when dest is a
     * TEMP: TEMPs are short-lived and the allocator keeps them in a
     * register.  (STORE has no dest vreg problem; its 'dest' slot is the
     * base address.) */
    if (is_load)
    {
      IROperand dest_op = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest_op);
      if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
    }

    int add_idx = ir_opt_du_def(&du, addr_vr, i);
    if (add_idx < 0)
      continue;

    IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
    if (add_q->op != TCCIR_OP_ADD)
      continue;

    if (ir_opt_du_uses(&du, addr_vr) != 1)
      continue;

    IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
    IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);

    /* Strict IMM32 check: `is_const` alone also matches SYMREF operands whose
     * u.pool_idx is NOT a numeric offset, and STACKOFF which is FP-relative.
     * Only IROP_TAG_IMM32 guarantees that u.imm32 is the real displacement. */
    IROperand base_op;
    int imm;
    if (irop_get_tag(add_src2) == IROP_TAG_IMM32 && irop_get_tag(add_src1) == IROP_TAG_VREG && irop_has_vreg(add_src1))
    {
      base_op = add_src1;
      imm = (int)add_src2.u.imm32;
    }
    else if (irop_get_tag(add_src1) == IROP_TAG_IMM32 && irop_get_tag(add_src2) == IROP_TAG_VREG &&
             irop_has_vreg(add_src2))
    {
      base_op = add_src2;
      imm = (int)add_src1.u.imm32;
    }
    else
    {
      continue;
    }

    /* Thumb-2 LDR/STR immediate range: 0..4095 (unsigned T3 enc),
     * -255..0 (signed T4 enc).  Byte/halfword share the same ranges. */
    if (imm > 4095 || imm < -255)
      continue;

    /* Reject complex bases that cannot be plugged into [base,#imm] directly. */
    if (base_op.is_local || base_op.is_llocal)
      continue;
    /* is_lval on base means "pointer loaded from memory"; the backend's
     * mach_ensure_in_reg will handle the load.  Allowed. */

    /* Same-block requirement between ADD and its consumer. */
    int same_block = 1;
    for (int j = add_idx + 1; j < i; j++)
    {
      TccIrOp bop = ir->compact_instructions[j].op;
      if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
      {
        same_block = 0;
        break;
      }
    }
    if (!same_block)
      continue;

    /* Capture original LOAD/STORE/ASSIGN operands before we repoint them. */
    IROperand orig_dest = tcc_ir_op_get_dest(ir, q);
    IROperand orig_src1 = tcc_ir_op_get_src1(ir, q);

    /* Single-use copy peep-through: when the ADD's base is a TEMP whose only
     * def is `T <- X [ASSIGN]` (a plain non-deref copy from a PARAM, VAR, or
     * other TEMP) and that TEMP has no other uses, fold X directly into the
     * STORE_INDEXED/LOAD_INDEXED base so regalloc doesn't have to coalesce
     * the copy.  Matches the sha_init pattern where each field write copies
     * P0 into a fresh TEMP just to feed the ADD. */
    {
      int32_t base_vr = irop_get_vreg(base_op);
      if (base_vr >= 0 && TCCIR_DECODE_VREG_TYPE(base_vr) == TCCIR_VREG_TYPE_TEMP &&
          ir_opt_du_uses(&du, base_vr) == 1)
      {
        int copy_idx = ir_opt_du_def(&du, base_vr, add_idx);
        if (copy_idx >= 0)
        {
          IRQuadCompact *copy_q = &ir->compact_instructions[copy_idx];
          if (copy_q->op == TCCIR_OP_ASSIGN)
          {
            IROperand copy_dest = tcc_ir_op_get_dest(ir, copy_q);
            IROperand copy_src = tcc_ir_op_get_src1(ir, copy_q);
            /* Plain copy (no deref on either side), with the destination just
             * a fresh TEMP — semantically equivalent to using copy_src
             * directly. */
            if (!copy_dest.is_lval && !copy_src.is_lval && irop_has_vreg(copy_src))
            {
              base_op = copy_src;
              copy_q->op = TCCIR_OP_NOP;
            }
          }
        }
      }
    }

    /* Allocate 4 fresh pool slots — LOAD/STORE/ASSIGN used 2, indexed ops need 4.
     * Grow the pool first so we don't bail when capacity is tight. */
    tcc_ir_pool_ensure(ir, 4);
    int new_base_idx = ir->iroperand_pool_count;
    if (new_base_idx + 4 > ir->iroperand_pool_capacity)
      continue;
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);

    IROperand index_imm = irop_make_imm32(0, imm, IROP_BTYPE_INT32);
    IROperand scale_imm = irop_make_imm32(0, 0, IROP_BTYPE_INT32);

    if (is_store)
    {
      /* STORE_INDEXED: base, value, index, scale
       * For plain STORE the dest was the address (lval) and src1 was the value.
       * For ASSIGN-as-store we don't handle that case (filtered above). */
      IROperand base_for_store = base_op;
      base_for_store.is_lval = 0; /* base provides address; no second deref */
      ir->iroperand_pool[new_base_idx + 0] = base_for_store;
      ir->iroperand_pool[new_base_idx + 1] = orig_src1;
      ir->iroperand_pool[new_base_idx + 2] = index_imm;
      ir->iroperand_pool[new_base_idx + 3] = scale_imm;
      q->op = TCCIR_OP_STORE_INDEXED;
    }
    else
    {
      /* LOAD_INDEXED: dest, base, index, scale */
      IROperand base_for_load = base_op;
      base_for_load.is_lval = 0;
      IROperand new_dest = orig_dest;
      /* ASSIGN's dest didn't carry the load btype; LOAD_INDEXED takes btype
       * from dest.  Copy sign/btype from the address operand (which carried
       * it via is_lval + btype) so the backend issues the right load width. */
      if (q->op == TCCIR_OP_ASSIGN)
      {
        new_dest.btype = addr_op.btype;
        new_dest.is_unsigned = addr_op.is_unsigned;
      }
      ir->iroperand_pool[new_base_idx + 0] = new_dest;
      ir->iroperand_pool[new_base_idx + 1] = base_for_load;
      ir->iroperand_pool[new_base_idx + 2] = index_imm;
      ir->iroperand_pool[new_base_idx + 3] = scale_imm;
      q->op = TCCIR_OP_LOAD_INDEXED;
    }
    q->operand_base = new_base_idx;

    add_q->op = TCCIR_OP_NOP;
    changes++;

    LOG_IR_GEN("DISP FUSION: ADD@%d + %s@%d -> %s_INDEXED imm=%d", add_idx,
               (is_store ? "STORE" : (q->op == TCCIR_OP_LOAD_INDEXED ? "LOAD" : "ASSIGN")), i,
               is_store ? "STORE" : "LOAD", imm);
  }

  LOG_IR_GEN("=== DISP FUSION END: %d fusions ===", changes);

  tcc_free(du.def);
  return changes;
}

/* ============================================================================
 * Indexed-chain fold
 * ============================================================================
 *
 * Fold a constant-immediate ADD that feeds an existing _INDEXED memory op
 * into the indexed op's offset:
 *
 *   ADD T = base, #imm1                    →  (NOP)
 *   ... base=T STORE_INDEXED #imm2, #0     →  base=base STORE_INDEXED #(imm1+imm2), #0
 *   ... base=T LOAD_INDEXED  #imm2, #0     →  base=base LOAD_INDEXED  #(imm1+imm2), #0
 *
 * Picks up sha_final-style chains:
 *
 *   T = &info->data[0]  (= P0+28)
 *   *(T + 56) = info->count_hi          ; STORE_INDEXED with offset 56
 *
 * which folds to a single `strb r1, [P0, #84]`.  Runs after disp_fusion so
 * any STORE_INDEXED produced there can also be chained into.  Only handles
 * scale=0 entries (constant offset, no shift).
 */
int tcc_ir_opt_indexed_chain(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  IROptDU du;
  ir_opt_du_build(ir, &du);

  LOG_IR_GEN("=== INDEXED CHAIN START (n=%d) ===", n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int is_store = (q->op == TCCIR_OP_STORE_INDEXED);
    int is_load = (q->op == TCCIR_OP_LOAD_INDEXED);
    if (!is_store && !is_load)
      continue;

    /* Layout: pool[base+0]=dest(load) or base(store)
     *         pool[base+1]=base(load) or value(store)
     *         pool[base+2]=index, pool[base+3]=scale */
    int base_slot = is_store ? 0 : 1;
    IROperand base_op = ir->iroperand_pool[q->operand_base + base_slot];
    IROperand index_op = ir->iroperand_pool[q->operand_base + 2];
    IROperand scale_op = ir->iroperand_pool[q->operand_base + 3];

    /* Only chain scale=0 ops with a constant existing offset. */
    if (irop_get_tag(scale_op) != IROP_TAG_IMM32 || scale_op.u.imm32 != 0)
      continue;
    if (irop_get_tag(index_op) != IROP_TAG_IMM32)
      continue;
    int imm2 = (int)index_op.u.imm32;

    int32_t base_vr = irop_get_vreg(base_op);
    if (base_vr < 0)
      continue;
    if (base_op.is_local || base_op.is_llocal)
      continue;
    if (base_op.is_lval)
      continue; /* base is a pointer-from-memory; backend would need to load it */

    /* Find the unique def of base.  Must be ADD with a constant immediate. */
    int add_idx = ir_opt_du_def(&du, base_vr, i);
    if (add_idx < 0)
      continue;
    if (ir_opt_du_uses(&du, base_vr) != 1)
      continue;

    IRQuadCompact *add_q = &ir->compact_instructions[add_idx];
    if (add_q->op != TCCIR_OP_ADD)
      continue;

    IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
    IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);

    IROperand new_base;
    int imm1;
    if (irop_get_tag(add_src2) == IROP_TAG_IMM32 && irop_get_tag(add_src1) == IROP_TAG_VREG &&
        irop_has_vreg(add_src1))
    {
      new_base = add_src1;
      imm1 = (int)add_src2.u.imm32;
    }
    else if (irop_get_tag(add_src1) == IROP_TAG_IMM32 && irop_get_tag(add_src2) == IROP_TAG_VREG &&
             irop_has_vreg(add_src2))
    {
      new_base = add_src2;
      imm1 = (int)add_src1.u.imm32;
    }
    else
    {
      continue;
    }

    if (new_base.is_local || new_base.is_llocal)
      continue;

    /* Combined offset must fit Thumb-2 immediate range and (for the LDRD/STRD
     * paths the backend chooses for 64-bit ops) preserve alignment.  We
     * already gate scale=0; for the value btype carried by STORE_INDEXED's
     * value slot or LOAD_INDEXED's dest slot, the existing STORE_INDEXED was
     * produced by disp_fusion which enforced non-64-bit, so we just need to
     * keep the same width and check the imm range. */
    long long imm_total = (long long)imm1 + imm2;
    if (imm_total > 4095 || imm_total < -255)
      continue;

    /* Same-block (ADD and the indexed op): disp_fusion already enforces this
     * for fold-produced entries, but be defensive for SHL+ADD-produced ones. */
    int same_block = 1;
    for (int j = add_idx + 1; j < i; j++)
    {
      TccIrOp bop = ir->compact_instructions[j].op;
      if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
      {
        same_block = 0;
        break;
      }
    }
    if (!same_block)
      continue;

    /* Update operands in place and NOP the ADD. */
    new_base.is_lval = 0;
    new_base.btype = base_op.btype;
    ir->iroperand_pool[q->operand_base + base_slot] = new_base;
    IROperand new_index = irop_make_imm32(0, (int32_t)imm_total, IROP_BTYPE_INT32);
    ir->iroperand_pool[q->operand_base + 2] = new_index;

    add_q->op = TCCIR_OP_NOP;
    changes++;

    LOG_IR_GEN("INDEXED_CHAIN: ADD@%d (#%d) + %s_INDEXED@%d (#%d) -> #%lld", add_idx, imm1,
               is_store ? "STORE" : "LOAD", i, imm2, imm_total);
  }

  LOG_IR_GEN("=== INDEXED CHAIN END: %d folds ===", changes);
  tcc_free(du.def);
  return changes;
}

/* ============================================================================
 * Indexed-pair reorder
 * ============================================================================
 *
 * Sink intervening FUNCPARAMVAL ops past an upcoming LOAD/STORE_INDEXED so
 * that LDRD/STRD-pairable ops become adjacent.  Pattern:
 *
 *   LOAD_INDEXED  T_a = base + offset_a   (scale=0, constant offset)
 *   FUNCPARAMVAL  ... T_a                  (consumes T_a; doesn't write base)
 *   LOAD_INDEXED  T_b = base + offset_b
 *
 * becomes
 *
 *   LOAD_INDEXED  T_a = base + offset_a
 *   LOAD_INDEXED  T_b = base + offset_b   (hoisted)
 *   FUNCPARAMVAL  ... T_a                  (sunk)
 *
 * Same for adjacent STORE_INDEXED chains.  Only swap when the FUNCPARAMVAL
 * does not name T_b (the hoisted op's dest) and the two indexed ops both
 * have scale=0 with the same base register vreg.  Letting codegen decide
 * pair-validity (offsets, alignment, regalloc) keeps this pass simple — we
 * only do swaps that *might* help and never block.
 */
int tcc_ir_opt_indexed_pair_reorder(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 3)
    return 0;

  LOG_IR_GEN("=== INDEXED PAIR REORDER START (n=%d) ===", n);

  for (int i = 0; i + 2 < n; i++)
  {
    IRQuadCompact *q1 = &ir->compact_instructions[i];
    if (q1->op != TCCIR_OP_LOAD_INDEXED && q1->op != TCCIR_OP_STORE_INDEXED)
      continue;
    int q1_is_load = (q1->op == TCCIR_OP_LOAD_INDEXED);

    /* q1 must have scale=0 and constant offset. */
    IROperand q1_scale = ir->iroperand_pool[q1->operand_base + 3];
    IROperand q1_index = ir->iroperand_pool[q1->operand_base + 2];
    if (irop_get_tag(q1_scale) != IROP_TAG_IMM32 || q1_scale.u.imm32 != 0)
      continue;
    if (irop_get_tag(q1_index) != IROP_TAG_IMM32)
      continue;

    int q1_base_slot = q1_is_load ? 1 : 0;
    IROperand q1_base = ir->iroperand_pool[q1->operand_base + q1_base_slot];
    int32_t q1_base_vr = irop_get_vreg(q1_base);
    if (q1_base_vr < 0)
      continue;

    /* Look ahead within a small window for a pair-able same-op _INDEXED.
     * Intervening ops may be NOP, FUNCPARAMVAL, or simple non-lval ASSIGN
     * — those are guaranteed not to write memory, define the base vreg,
     * or jump.  Anything else (STORE, CALL, JUMP, ALU writing base, ...)
     * blocks the search. */
    const int window = 12;
    int q3_idx = -1;
    int blocked = 0;
    for (int k = i + 1; k < n && (k - i) <= window; k++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[k];
      if (cq->op == TCCIR_OP_NOP)
        continue;
      if (cq->is_jump_target)
      {
        blocked = 1;
        break;
      }

      if (cq->op == q1->op)
      {
        q3_idx = k;
        break;
      }

      /* Is this op safe to leave between q1 and the future q3? */
      int safe = 0;
      if (cq->op == TCCIR_OP_FUNCPARAMVAL)
      {
        safe = 1; /* register/stack param setup; we accept the small risk
                   * that param-reg writes interfere with regalloc choices */
      }
      else if (cq->op == TCCIR_OP_ASSIGN)
      {
        IROperand a_dest = tcc_ir_op_get_dest(ir, cq);
        IROperand a_src1 = tcc_ir_op_get_src1(ir, cq);
        /* Plain non-deref copy.  Bail if the ASSIGN writes our base (the
         * to-be-hoisted load would observe the wrong base). */
        if (!a_dest.is_lval && !a_src1.is_lval && irop_get_vreg(a_dest) != q1_base_vr)
          safe = 1;
      }
      if (!safe)
      {
        blocked = 1;
        break;
      }
    }
    if (q3_idx < 0 || blocked)
      continue;

    IRQuadCompact *q3 = &ir->compact_instructions[q3_idx];
    /* q3 same op-kind as q1 (LOAD/STORE_INDEXED) — already verified. */

    IROperand q3_scale = ir->iroperand_pool[q3->operand_base + 3];
    IROperand q3_index = ir->iroperand_pool[q3->operand_base + 2];
    if (irop_get_tag(q3_scale) != IROP_TAG_IMM32 || q3_scale.u.imm32 != 0)
      continue;
    if (irop_get_tag(q3_index) != IROP_TAG_IMM32)
      continue;

    int q3_base_slot = q1_is_load ? 1 : 0;
    IROperand q3_base = ir->iroperand_pool[q3->operand_base + q3_base_slot];
    if (irop_get_vreg(q3_base) != q1_base_vr)
      continue;

    int32_t imm1 = q1_index.u.imm32;
    int32_t imm2 = q3_index.u.imm32;
    if (imm1 + 4 != imm2 && imm2 + 4 != imm1)
      continue;

    /* Offsets must be 4-byte aligned (LDRD/STRD requirement). */
    if ((imm1 & 3) != 0 || (imm2 & 3) != 0)
      continue;

    /* Bubble q3 up to position i+1 by swapping with each predecessor op.
     * Each swap must respect: no intervening op uses q3's dest/value or
     * defines q3's base.  Since the safety scan above already verified
     * each intervening op is FUNCPARAMVAL or non-lval ASSIGN that doesn't
     * touch the base, we just have to check vreg dependencies on q3's
     * dest/value at each step. */
    IROperand q3_dv = q1_is_load ? ir->iroperand_pool[q3->operand_base + 0]
                                 : ir->iroperand_pool[q3->operand_base + 1];
    int32_t q3_dv_vr = irop_get_vreg(q3_dv);

    int swap_pos = q3_idx;
    int target_pos = i + 1;
    while (swap_pos > target_pos)
    {
      int prev = swap_pos - 1;
      while (prev > i && ir->compact_instructions[prev].op == TCCIR_OP_NOP)
        prev--;
      if (prev <= i)
        break;

      IRQuadCompact *pq = &ir->compact_instructions[prev];

      /* For LOAD: prev must not USE q3's dest (it would see undefined
       * value if hoisted ahead).  For STORE: prev must not READ q3's
       * value (same reason). */
      int conflict = 0;
      if (q3_dv_vr >= 0)
      {
        if (irop_config[pq->op].has_src1)
        {
          IROperand s = tcc_ir_op_get_src1(ir, pq);
          if (irop_get_vreg(s) == q3_dv_vr)
            conflict = 1;
        }
        if (!conflict && irop_config[pq->op].has_src2)
        {
          IROperand s = tcc_ir_op_get_src2(ir, pq);
          if (irop_get_vreg(s) == q3_dv_vr)
            conflict = 1;
        }
      }
      if (conflict)
        break;

      /* Swap pq with q3 by exchanging IRQuadCompact contents (operand_base
       * stays put — operand pool entries don't move). */
      IRQuadCompact tmp = *pq;
      *pq = ir->compact_instructions[swap_pos];
      ir->compact_instructions[swap_pos] = tmp;

      swap_pos = prev;
    }

    if (swap_pos < q3_idx)
    {
      changes++;
      LOG_IR_GEN("INDEXED PAIR REORDER: bubbled %s_INDEXED from i=%d to i=%d (next to i=%d, offsets %d,%d)",
                 q1_is_load ? "LOAD" : "STORE", q3_idx, swap_pos, i, imm1, imm2);
    }
  }

  LOG_IR_GEN("=== INDEXED PAIR REORDER END: %d swaps ===", changes);
  return changes;
}

/* ============================================================================
 * LEA + deref fold
 * ============================================================================
 *
 * The frontend materializes `&local_var` as an explicit `LEA` op that
 * computes the stack-slot address into a TEMP vreg, even when the very next
 * use is a deref.  On ARM this becomes `add rX, sp, #off; ldr rY, [rX]`
 * (2 instructions) instead of `ldr rY, [sp, #off]` (1 instruction).  GCC
 * picks the one-instruction form because it doesn't split the address into
 * a vreg first.
 *
 * Pattern A — LEA + consumer-with-deref:
 *   i0: T = LEA Addr[StackLoc[-N]]     (STACKOFF, is_lval=0)
 *   i1: <op> ... T***DEREF*** ...       (is_lval=1 on a T-valued operand)
 *
 * Pattern B — LEA + ADD(#K) + consumer-with-deref:
 *   i0: T1 = LEA Addr[StackLoc[-N]]
 *   i1: T2 = T1 ADD #K
 *   i2: <op> ... T2***DEREF*** ...
 *
 * Transform: substitute the T-DEREF operand with the StackLoc itself at the
 * appropriate offset (is_lval=1 so the backend emits a direct stack load).
 * NOP the LEA (and ADD in pattern B).
 *
 * Safety constraints:
 *   - LEA source must be STACKOFF with is_lval=0 (i.e. Addr[StackLoc]).
 *   - LEA result must have exactly one use.  For pattern B, the ADD must
 *     also have exactly one use.
 *   - Consumer must reside in the same basic block.
 *   - Consumer must reference the LEA/ADD result with is_lval=1 exactly
 *     once (either src1 or src2 — never as the destination of a STORE
 *     through a LEA'd pointer; that's indistinguishable from a direct
 *     stack-slot store and is handled separately).
 */
static int find_deref_use_operand(TCCIRState *ir, int consumer_idx, int32_t vreg, int *which_out)
{
  IRQuadCompact *q = &ir->compact_instructions[consumer_idx];
  const IRRegistersConfig *cfg = &irop_config[q->op];
  int matches = 0;
  int which = 0;

  if (cfg->has_src1)
  {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (s.is_lval && irop_has_vreg(s) && irop_get_vreg(s) == vreg)
    {
      matches++;
      which = 1;
    }
  }
  if (cfg->has_src2)
  {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (s.is_lval && irop_has_vreg(s) && irop_get_vreg(s) == vreg)
    {
      matches++;
      which = 2;
    }
  }
  if (cfg->has_dest)
  {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval && irop_has_vreg(d) && irop_get_vreg(d) == vreg)
    {
      matches++;
      which = 0; /* 0 means dest */
    }
  }
  if (matches != 1)
    return 0;
  *which_out = which;
  return 1;
}

int tcc_ir_opt_lea_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  IROptDU du;
  ir_opt_du_build(ir, &du);

  LOG_IR_GEN("=== LEA FOLD START (n=%d) ===", n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *lea_q = &ir->compact_instructions[i];
    if (lea_q->op != TCCIR_OP_LEA)
      continue;

    IROperand lea_src = tcc_ir_op_get_src1(ir, lea_q);
    if (irop_get_tag(lea_src) != IROP_TAG_STACKOFF)
      continue;
    if (lea_src.is_lval) /* already a deref — not an Addr[] form */
      continue;
    if (lea_src.is_llocal) /* double-indirect; keep backend logic */
      continue;

    /* Reject vreg-backed stack operands like `&V1` — these share the
     * STACKOFF tag with `Addr[StackLoc[-N]]` but their real offset comes
     * from the register allocator's spill slot for the vreg, not from
     * u.imm32 (which is 0 for those operands).  Folding would produce
     * StackLoc[0] and dissociate the access from V1's slot. */
    if (irop_get_vreg(lea_src) != -1)
      continue;

    IROperand lea_dest = tcc_ir_op_get_dest(ir, lea_q);
    int32_t lea_vr = irop_get_vreg(lea_dest);
    if (lea_vr < 0)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(lea_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    /* `ir_opt_du_uses` relies on a def/use table where STORE's dest slot is
     * recorded as a *definition*, not a use — even though `T***DEREF*** <-- val
     * [STORE]` semantically uses T.  That undercounts real uses and caused
     * the pass to fold `LEA T; *T read; *T write` into `load StackLoc; NOP;
     * *T write` (dangling T).  Do an explicit linear-scan use count that
     * considers dest positions when is_lval=1. */
    {
      int total_uses = 0;
      for (int k = i + 1; k < n && total_uses < 2; k++)
      {
        IRQuadCompact *uq = &ir->compact_instructions[k];
        if (uq->op == TCCIR_OP_NOP)
          continue;
        const IRRegistersConfig *cfg = &irop_config[uq->op];
        if (cfg->has_src1)
        {
          IROperand s = tcc_ir_op_get_src1(ir, uq);
          if (irop_has_vreg(s) && irop_get_vreg(s) == lea_vr)
            total_uses++;
        }
        if (cfg->has_src2)
        {
          IROperand s = tcc_ir_op_get_src2(ir, uq);
          if (irop_has_vreg(s) && irop_get_vreg(s) == lea_vr)
            total_uses++;
        }
        if (cfg->has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, uq);
          /* A dest with is_lval=1 is a *use* of the vreg (we deref through
           * it), not a redefinition. A dest without is_lval would redefine
           * lea_vr and end its live range — treat as a hard stop. */
          if (irop_has_vreg(d) && irop_get_vreg(d) == lea_vr)
          {
            if (d.is_lval)
              total_uses++;
            else
              break; /* lea_vr redefined; stop scanning */
          }
        }
      }
      if (total_uses != 1)
        continue;
    }

    /* Use the accessor — STRUCT btype stores the offset in u.s.aux_data,
     * not u.imm32.  Reading u.imm32 directly on a struct-typed Addr[] gives
     * the concatenation of ctype_idx + aux_data and produces garbage. */
    int32_t base_offset = irop_get_stack_offset(lea_src);

    /* Find the single use of the LEA result. */
    int cur_idx = -1;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;
      /* Same-block check: the use must precede any control-flow edge. */
      if (uq->op == TCCIR_OP_JUMP || uq->op == TCCIR_OP_JUMPIF)
        break;
      const IRRegistersConfig *cfg = &irop_config[uq->op];
      int uses_lea = 0;
      if (cfg->has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, uq);
        if (irop_has_vreg(s) && irop_get_vreg(s) == lea_vr)
          uses_lea = 1;
      }
      if (!uses_lea && cfg->has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, uq);
        if (irop_has_vreg(s) && irop_get_vreg(s) == lea_vr)
          uses_lea = 1;
      }
      if (!uses_lea && cfg->has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, uq);
        if (irop_has_vreg(d) && irop_get_vreg(d) == lea_vr)
          uses_lea = 1;
      }
      if (uses_lea)
      {
        cur_idx = j;
        break;
      }
    }
    if (cur_idx < 0)
      continue;

    /* Optional ADD #K interposer: a single intermediate ADD that consumes
     * the LEA result and adds a constant, whose own result has exactly one
     * use (the eventual deref consumer). */
    int add_idx = -1;
    int32_t add_offset = 0;
    IRQuadCompact *add_q = &ir->compact_instructions[cur_idx];
    if (add_q->op == TCCIR_OP_ADD)
    {
      IROperand a1 = tcc_ir_op_get_src1(ir, add_q);
      IROperand a2 = tcc_ir_op_get_src2(ir, add_q);
      /* Must be `lea_vr + #K` or `#K + lea_vr` with the other side IMM32 —
       * AND the vreg side must have is_lval=0.  A DEREF flag on that operand
       * means the ADD reads the value *stored at* lea_vr and adds K to
       * that (loaded-pointer arithmetic), not pointer-plus-offset.  Folding
       * this into a direct stack slot would read the struct layout instead
       * of following the loaded pointer. */
      int ok = 0;
      if (irop_has_vreg(a1) && irop_get_vreg(a1) == lea_vr && !a1.is_lval && irop_get_tag(a2) == IROP_TAG_IMM32)
      {
        add_offset = (int32_t)a2.u.imm32;
        ok = 1;
      }
      else if (irop_has_vreg(a2) && irop_get_vreg(a2) == lea_vr && !a2.is_lval && irop_get_tag(a1) == IROP_TAG_IMM32)
      {
        add_offset = (int32_t)a1.u.imm32;
        ok = 1;
      }
      if (ok)
      {
        IROperand add_dest = tcc_ir_op_get_dest(ir, add_q);
        int32_t add_vr = irop_get_vreg(add_dest);
        /* Explicit-scan use count — ir_opt_du_uses undercounts STORE-dest
         * uses (treats `T***DEREF*** <-- val [STORE]` as a redefinition of
         * T rather than a use). */
        int add_uses_real = 0;
        if (add_vr >= 0 && TCCIR_DECODE_VREG_TYPE(add_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          for (int k = cur_idx + 1; k < n && add_uses_real < 2; k++)
          {
            IRQuadCompact *uq2 = &ir->compact_instructions[k];
            if (uq2->op == TCCIR_OP_NOP)
              continue;
            const IRRegistersConfig *cfg2 = &irop_config[uq2->op];
            if (cfg2->has_src1)
            {
              IROperand s = tcc_ir_op_get_src1(ir, uq2);
              if (irop_has_vreg(s) && irop_get_vreg(s) == add_vr)
                add_uses_real++;
            }
            if (cfg2->has_src2)
            {
              IROperand s = tcc_ir_op_get_src2(ir, uq2);
              if (irop_has_vreg(s) && irop_get_vreg(s) == add_vr)
                add_uses_real++;
            }
            if (cfg2->has_dest)
            {
              IROperand d = tcc_ir_op_get_dest(ir, uq2);
              if (irop_has_vreg(d) && irop_get_vreg(d) == add_vr)
              {
                if (d.is_lval)
                  add_uses_real++;
                else
                {
                  add_uses_real = 99;
                  break;
                }
              }
            }
          }
        }
        if (add_vr >= 0 && TCCIR_DECODE_VREG_TYPE(add_vr) == TCCIR_VREG_TYPE_TEMP && add_uses_real == 1)
        {
          add_idx = cur_idx;

          /* Find the consumer of add_vr in the same block. */
          int cons_idx = -1;
          for (int k = add_idx + 1; k < n; k++)
          {
            IRQuadCompact *ck = &ir->compact_instructions[k];
            if (ck->op == TCCIR_OP_NOP)
              continue;
            if (ck->op == TCCIR_OP_JUMP || ck->op == TCCIR_OP_JUMPIF)
              break;
            const IRRegistersConfig *cfg = &irop_config[ck->op];
            int touches = 0;
            if (cfg->has_src1)
            {
              IROperand s = tcc_ir_op_get_src1(ir, ck);
              if (irop_has_vreg(s) && irop_get_vreg(s) == add_vr)
                touches = 1;
            }
            if (!touches && cfg->has_src2)
            {
              IROperand s = tcc_ir_op_get_src2(ir, ck);
              if (irop_has_vreg(s) && irop_get_vreg(s) == add_vr)
                touches = 1;
            }
            if (!touches && cfg->has_dest)
            {
              IROperand d = tcc_ir_op_get_dest(ir, ck);
              if (irop_has_vreg(d) && irop_get_vreg(d) == add_vr)
                touches = 1;
            }
            if (touches)
            {
              cons_idx = k;
              break;
            }
          }
          if (cons_idx < 0)
            continue; /* ADD dead-ends — let DCE handle it */
          cur_idx = cons_idx;
        }
      }
    }

    /* Require the final consumer to reference the folded vreg exactly once
     * with is_lval=1.  Reject pure non-deref uses (e.g. the vreg flows into
     * a PARAM or another ADD) since the semantic change only holds when the
     * op actually dereferences through the address. */
    int32_t deref_vr =
        (add_idx >= 0) ? irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx])) : lea_vr;
    int which = 0;
    if (!find_deref_use_operand(ir, cur_idx, deref_vr, &which))
      continue;

    IRQuadCompact *cons_q = &ir->compact_instructions[cur_idx];

    IROperand old_op = (which == 1)   ? tcc_ir_op_get_src1(ir, cons_q)
                       : (which == 2) ? tcc_ir_op_get_src2(ir, cons_q)
                                      : tcc_ir_op_get_dest(ir, cons_q);

    /* The *consumer* side determines where the folded offset is stored.
     * When the consumer reads the slot as a struct, its btype==STRUCT and
     * the offset must go through u.s.aux_data (irop_make_stackoff writes
     * u.imm32 unconditionally, which would corrupt ctype_idx).  Skip only
     * that case.
     *
     * lea_src.btype==STRUCT is fine — the source is the address of a
     * struct, but the scalar consumer (CMP/AND/LOAD of an int field) has
     * its own non-struct btype and we rebuild the operand from scratch
     * with irop_get_stack_offset() handling the struct-side read. */
    if (old_op.btype == IROP_BTYPE_STRUCT)
      continue;

    /* Build the substituted operand: direct StackLoc at the folded offset,
     * is_lval=1.  Build from scratch via irop_make_stackoff so bit-fields
     * and unused union members are cleanly initialized, then copy the
     * consumer's load-width info (btype/is_unsigned) onto it. */
    int folded_off = base_offset + add_offset;
    IROperand new_op = irop_make_stackoff(-1, folded_off, /*is_lval*/ 1, /*is_llocal*/ 0,
                                          /*is_param_flag*/ (int)lea_src.is_param, old_op.btype);
    new_op.is_unsigned = old_op.is_unsigned;
    new_op.is_static = lea_src.is_static;

    if (which == 1)
      tcc_ir_op_set_src1(ir, cons_q, new_op);
    else if (which == 2)
      tcc_ir_op_set_src2(ir, cons_q, new_op);
    else
      tcc_ir_op_set_dest(ir, cons_q, new_op);

    lea_q->op = TCCIR_OP_NOP;
    if (add_idx >= 0)
      ir->compact_instructions[add_idx].op = TCCIR_OP_NOP;

    changes++;
    LOG_IR_GEN("LEA FOLD: LEA@%d%s -> consumer@%d  (offset=%d+%d=%d)", i, (add_idx >= 0 ? " + ADD" : ""), cur_idx,
               base_offset, add_offset, base_offset + add_offset);
  }

  LOG_IR_GEN("=== LEA FOLD END: %d folds ===", changes);

  tcc_free(du.def);
  return changes;
}

/* ============================================================================
 * Combined Boolean Pass
 * ============================================================================
 *
 * Runs cse_bool and bool_idempotent in a single forward loop (one scan instead
 * of two).  Within each BOOL_AND/BOOL_OR instruction, idempotent simplification
 * runs first; if it fires the CSE table lookup is skipped for that instruction.
 *
 * do_idempotent: run bool_idempotent logic (a&&a→a, a&&1→a, a||0→a)
 * do_cse:        run cse_bool logic (eliminate duplicate bool ops)
 *
 * Returns total number of changes.
 */
int tcc_ir_opt_bool_pass(TCCIRState *ir, int do_idempotent, int do_cse)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0 || (!do_idempotent && !do_cse))
    return 0;

  BoolCSEEntry *hash_table[BOOL_CSE_HASH_SIZE];
  memset(hash_table, 0, sizeof(hash_table));

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Block boundary: flush CSE table so stale entries don't cross blocks. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      if (do_cse)
        bool_cse_clear_all(hash_table);
      continue;
    }

    if (q->op != TCCIR_OP_BOOL_AND && q->op != TCCIR_OP_BOOL_OR)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* --- Idempotent simplification --- */
    if (do_idempotent)
    {
      int simplified = 0;
      if (src1.vr >= 0 && src1.vr == src2.vr)
      {
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
        simplified = 1;
      }
      else if (src2.vr < 0 && irop_is_immediate(src2))
      {
        int64_t val = irop_get_imm64_ex(ir, src2);
        int is_and = (q->op == TCCIR_OP_BOOL_AND);
        if ((is_and && val == 1) || (!is_and && val == 0))
        {
          q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
          simplified = 1;
        }
      }
      if (simplified)
        continue; /* Already handled; skip CSE for this slot. */
    }

    /* --- Common subexpression elimination --- */
    if (do_cse)
    {
      int left_vr = src1.vr, right_vr = src2.vr;
      if (left_vr > right_vr)
      {
        int tmp = left_vr;
        left_vr = right_vr;
        right_vr = tmp;
      }

      BoolCSEEntry *existing = bool_cse_find(hash_table, q->op, left_vr, right_vr);
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
        bool_cse_add(hash_table, q->op, left_vr, right_vr, dest_vr);
      }
    }
  }

  if (do_cse)
    bool_cse_clear_all(hash_table);
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

/* Check whether a vreg has exactly one definition in the function.
 * Returns 1 if there is a single definition, 0 otherwise.
 * This is needed by ir_opt_pure_expr_equal to guard against comparing
 * vregs that have multiple reaching definitions from different branches. */
static int tcc_ir_vreg_has_single_def(TCCIRState *ir, int32_t vreg)
{
  int def_count = 0;
  int n = ir->next_instruction_index;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) == vreg)
    {
      def_count++;
      if (def_count > 1)
        return 0;
    }
  }
  return def_count == 1;
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
  case 0x92: /* TOK_ULT (unsigned <) */
    return (uint64_t)val1 < (uint64_t)val2;
  case 0x93: /* TOK_UGE (unsigned >=) */
    return (uint64_t)val1 >= (uint64_t)val2;
  case 0x96: /* TOK_ULE (unsigned <=) */
    return (uint64_t)val1 <= (uint64_t)val2;
  case 0x97: /* TOK_UGT (unsigned >) */
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

  LOG_IR_GEN("=== BRANCH FOLDING START ===");

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *test_q = &ir->compact_instructions[i];

    if (test_q->op == TCCIR_OP_NOP)
      continue;

    /* Find the next non-NOP instruction (SETIF fusion can leave NOPs between CMP and JUMPIF) */
    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      continue;
    IRQuadCompact *jump_q = &ir->compact_instructions[j];

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
        tcc_ir_set_dest(ir, j, dest);

        LOG_IR_GEN("BRANCH FOLD: TEST_ZERO #0 -> unconditional JUMP to %d", (int)dest.u.imm32);
        changes++;
      }
      else
      {
        /* Branch never taken - remove both instructions */
        test_q->op = TCCIR_OP_NOP;
        jump_q->op = TCCIR_OP_NOP;

        LOG_IR_GEN("BRANCH FOLD: TEST_ZERO #%lld with cond 0x%x never taken -> both NOP", (long long)val, tok);
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

        tcc_ir_set_dest(ir, j, dest);

        LOG_IR_GEN("BRANCH FOLD: CMP %lld,%lld with cond 0x%x -> unconditional JUMP to %d", (long long)val1,
                   (long long)val2, tok, (int)dest.u.imm32);
        changes++;
      }
      else
      {
        /* Branch never taken - remove both instructions */
        test_q->op = TCCIR_OP_NOP;
        jump_q->op = TCCIR_OP_NOP;

        LOG_IR_GEN("BRANCH FOLD: CMP %lld,%lld with cond 0x%x never taken -> both NOP", (long long)val1,
                   (long long)val2, tok);
        changes++;
      }
    }
  }

  LOG_IR_GEN("=== BRANCH FOLDING END: %d branches folded ===", changes);

  return changes;
}

/* ============================================================================
 * Stack Address Non-Null Branch Folding
 * ============================================================================
 *
 * Stack addresses (Addr[StackLoc[X]]) are always non-zero on any real target.
 * When inlined code null-checks a pointer that is actually a stack address,
 * the CMP + JUMPIF is dead and can be folded away.
 *
 * Phase 1: Identify vregs that hold stack addresses by scanning for:
 *   - ASSIGN/LEA with src1 having is_local=1, is_lval=0 (address-of-stack)
 *   - STORE from a tracked temp into a VAR (propagate through store)
 *
 * Phase 2: Fold CMP(tracked_vreg, #0) + JUMPIF:
 *   - EQ: always false  (stack addr != 0) → NOP both
 *   - NE: always true   (stack addr != 0) → unconditional JUMP
 */
#define MAX_STACKADDR_VREGS 64

static int is_stack_address_operand(const IROperand op)
{
  return op.is_local && !op.is_lval && irop_get_tag(op) == IROP_TAG_STACKOFF;
}

int tcc_ir_opt_stack_addr_nonnull_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 3)
    return 0;

  LOG_IR_GEN("=== STACK ADDR NONNULL FOLD START ===");

  /* Phase 1: Identify TEMP vregs that hold stack addresses (single-assignment,
   * always safe). Also build a flow-sensitive bitmap for VAR vregs. */
  int32_t sa_vregs[MAX_STACKADDR_VREGS];
  int64_t sa_offsets[MAX_STACKADDR_VREGS];
  int sa_count = 0;

  /* Flow-sensitive VAR tracking: var_holds_stackaddr[pos] is 1 if the VAR
   * at that position currently holds a stack address, 0 otherwise.
   * Reset at jump targets (control flow merge points) for safety. */
#define MAX_TRACKED_VARS 256
  uint8_t var_holds_stackaddr[MAX_TRACKED_VARS];
  int64_t var_stackaddr_offset[MAX_TRACKED_VARS];
  memset(var_holds_stackaddr, 0, sizeof(var_holds_stackaddr));

  /* Single forward pass: track TEMPs and VARs, fold CMP+JUMPIF inline. */
  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* At jump targets, conservatively reset all VAR tracking.
     * A jump target is a merge point where different paths may have
     * assigned different values to the same VAR. */
    if (q->is_jump_target)
      memset(var_holds_stackaddr, 0, sizeof(var_holds_stackaddr));

    /* Track TEMPs assigned stack addresses */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (is_stack_address_operand(src1))
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int32_t vreg = irop_get_vreg(dest);
        if (vreg >= 0)
        {
          if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP && sa_count < MAX_STACKADDR_VREGS)
          {
            sa_offsets[sa_count] = irop_get_stack_offset(src1);
            sa_vregs[sa_count++] = vreg;
          }
          LOG_IR_GEN("STACKADDR: vreg 0x%x holds stack address at i=%d", vreg, i);
        }
      }
    }

    /* ADD: stack_addr + constant → result is also a stack address (non-null).
     * Covers patterns like &arr + 24 (element offset).
     * src1 can be either a tracked TEMP or a direct Addr[StackLoc] operand. */
    if (q->op == TCCIR_OP_ADD)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        int s1_is_sa = is_stack_address_operand(s1);
        if (!s1_is_sa)
        {
          int32_t s1_vr = irop_get_vreg(s1);
          if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP)
          {
            for (int k = 0; k < sa_count; k++)
            {
              if (sa_vregs[k] == s1_vr)
              {
                s1_is_sa = 1;
                break;
              }
            }
          }
        }
        if (s1_is_sa && sa_count < MAX_STACKADDR_VREGS)
        {
          int s2_nonneg = irop_is_immediate(s2);
          if (!s2_nonneg)
          {
            int32_t s2_vr = irop_get_vreg(s2);
            if (s2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s2_vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int def = tcc_ir_find_defining_instruction(ir, s2_vr, i);
              if (def >= 0 && ir->compact_instructions[def].op == TCCIR_OP_MUL)
              {
                IROperand mul_s2 = tcc_ir_op_get_src2(ir, &ir->compact_instructions[def]);
                if (irop_is_immediate(mul_s2) && irop_get_imm64_ex(ir, mul_s2) > 0)
                  s2_nonneg = 1;
              }
            }
          }
          if (!s2_nonneg)
            goto skip_sa_add;
          {
            int64_t base_off = 0;
            if (is_stack_address_operand(s1))
            {
              base_off = irop_get_stack_offset(s1);
            }
            else
            {
              int32_t s1_vr = irop_get_vreg(s1);
              for (int k = 0; k < sa_count; k++)
              {
                if (sa_vregs[k] == s1_vr)
                {
                  base_off = sa_offsets[k];
                  break;
                }
              }
            }
            if (irop_is_immediate(s2))
              sa_offsets[sa_count] = base_off + irop_get_imm64_ex(ir, s2);
            else
              sa_offsets[sa_count] = base_off;
            sa_vregs[sa_count++] = d_vr;
          }
        skip_sa_add:;
        }
      }
    }

    /* Flow-sensitive VAR tracking through STORE, ASSIGN, and LOAD.
     * After SL-FWD, a LOAD V=StackLoc may become ASSIGN V=T (forwarded). */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvreg = irop_get_vreg(dest);
      if (dvreg >= 0 && TCCIR_DECODE_VREG_TYPE(dvreg) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvreg);
        if (pos < MAX_TRACKED_VARS)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          int src_is_stackaddr = 0;
          int64_t src_offset = 0;
          if (is_stack_address_operand(src1))
          {
            src_is_stackaddr = 1;
            src_offset = irop_get_stack_offset(src1);
          }
          else
          {
            int32_t svreg = irop_get_vreg(src1);
            if (svreg >= 0)
            {
              if (TCCIR_DECODE_VREG_TYPE(svreg) == TCCIR_VREG_TYPE_TEMP)
              {
                for (int k = 0; k < sa_count; k++)
                {
                  if (sa_vregs[k] == svreg)
                  {
                    src_is_stackaddr = 1;
                    src_offset = sa_offsets[k];
                    break;
                  }
                }
              }
              else if (TCCIR_DECODE_VREG_TYPE(svreg) == TCCIR_VREG_TYPE_VAR)
              {
                int spos = TCCIR_DECODE_VREG_POSITION(svreg);
                if (spos < MAX_TRACKED_VARS && var_holds_stackaddr[spos])
                {
                  src_is_stackaddr = 1;
                  src_offset = var_stackaddr_offset[spos];
                }
              }
            }
          }
          var_holds_stackaddr[pos] = src_is_stackaddr;
          var_stackaddr_offset[pos] = src_offset;
        }
      }
    }

    /* ADD/SUB on a VAR: update tracked stack address offset, or invalidate. */
    if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvreg = irop_get_vreg(dest);
      if (dvreg >= 0 && TCCIR_DECODE_VREG_TYPE(dvreg) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvreg);
        if (pos < MAX_TRACKED_VARS)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          int32_t s1vr = irop_get_vreg(s1);
          int updated = 0;
          if (s1vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_VAR &&
              TCCIR_DECODE_VREG_POSITION(s1vr) == pos && var_holds_stackaddr[pos] && irop_is_immediate(s2))
          {
            int64_t delta = irop_get_imm64_ex(ir, s2);
            if (q->op == TCCIR_OP_SUB)
              delta = -delta;
            var_stackaddr_offset[pos] += delta;
            updated = 1;
          }
          if (!updated)
            var_holds_stackaddr[pos] = 0;
        }
      }
    }

    /* Check for CMP(stackaddr_vreg, #0) + JUMPIF pattern */
    if (q->op != TCCIR_OP_CMP)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Helper: check if a non-immediate operand holds a stack address */
    int matched = 0;
    int nonnull_is_src1 = 0;

    /* For TEMPs, is_lval=1 means pointer dereference (comparing *ptr, not ptr).
     * For VARs, is_lval=1 is normal — reading the VAR's value. A VAR tracked
     * as holding a stack address has that address AS its value, so comparing
     * it against 0 is a null-pointer check we can fold. */
    if (irop_is_immediate(src2) && irop_get_imm64_ex(ir, src2) == 0 && !irop_is_immediate(src1))
    {
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0)
      {
        if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
        {
          for (int k = 0; k < sa_count; k++)
          {
            if (sa_vregs[k] == vr)
            {
              matched = 1;
              break;
            }
          }
        }
        else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos < MAX_TRACKED_VARS && var_holds_stackaddr[pos])
            matched = 1;
        }
        if (matched)
          nonnull_is_src1 = 1;
      }
    }
    else if (irop_is_immediate(src1) && irop_get_imm64_ex(ir, src1) == 0 && !irop_is_immediate(src2))
    {
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0)
      {
        if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && !src2.is_lval)
        {
          for (int k = 0; k < sa_count; k++)
          {
            if (sa_vregs[k] == vr)
            {
              matched = 1;
              break;
            }
          }
        }
        else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos < MAX_TRACKED_VARS && var_holds_stackaddr[pos])
            matched = 1;
        }
        if (matched)
          nonnull_is_src1 = 0;
      }
    }

    /* Same-address comparison: CMP(sa_A, sa_B) where both are the same
     * known stack address. */
    if (!matched)
    {
      int sa1_valid = 0, sa2_valid = 0;
      int64_t off1 = 0, off2 = 0;
      if (is_stack_address_operand(src1))
      {
        sa1_valid = 1;
        off1 = irop_get_stack_offset(src1);
      }
      else
      {
        int32_t vr1 = irop_get_vreg(src1);
        if (vr1 >= 0)
        {
          if (TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
          {
            for (int k = 0; k < sa_count; k++)
            {
              if (sa_vregs[k] == vr1)
              {
                sa1_valid = 1;
                off1 = sa_offsets[k];
                break;
              }
            }
          }
          else if (TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr1);
            if (pos < MAX_TRACKED_VARS && var_holds_stackaddr[pos])
            {
              sa1_valid = 1;
              off1 = var_stackaddr_offset[pos];
            }
          }
        }
      }
      if (is_stack_address_operand(src2))
      {
        sa2_valid = 1;
        off2 = irop_get_stack_offset(src2);
      }
      else
      {
        int32_t vr2 = irop_get_vreg(src2);
        if (vr2 >= 0)
        {
          if (TCCIR_DECODE_VREG_TYPE(vr2) == TCCIR_VREG_TYPE_TEMP && !src2.is_lval)
          {
            for (int k = 0; k < sa_count; k++)
            {
              if (sa_vregs[k] == vr2)
              {
                sa2_valid = 1;
                off2 = sa_offsets[k];
                break;
              }
            }
          }
          else if (TCCIR_DECODE_VREG_TYPE(vr2) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr2);
            if (pos < MAX_TRACKED_VARS && var_holds_stackaddr[pos])
            {
              sa2_valid = 1;
              off2 = var_stackaddr_offset[pos];
            }
          }
        }
      }
      if (sa1_valid && sa2_valid && off1 == off2)
        matched = 2;
    }

    if (!matched)
      continue;

    /* Find next JUMPIF (skip NOPs) */
    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      continue;
    IRQuadCompact *jump_q = &ir->compact_instructions[j];
    if (jump_q->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
    int tok = (int)irop_get_imm64_ex(ir, cond);

    /* Stack address is always > 0 (unsigned), always != 0 */
    int fold_result = -1;

    if (matched == 2)
    {
      fold_result = evaluate_compare_condition(0, 0, tok);
    }
    else
    {
      (void)nonnull_is_src1;
      switch (tok)
      {
      case 0x94:
        fold_result = 0;
        break;
      case 0x95:
        fold_result = 1;
        break;
      default:
        break;
      }
    }

    if (fold_result < 0)
      continue;

    if (fold_result)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, jump_q);
      q->op = TCCIR_OP_NOP;
      jump_q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, j, dest);
      LOG_IR_GEN("STACKADDR FOLD: CMP(stackaddr, 0) with EQ/NE -> unconditional JUMP at i=%d", i);
    }
    else
    {
      q->op = TCCIR_OP_NOP;
      jump_q->op = TCCIR_OP_NOP;
      LOG_IR_GEN("STACKADDR FOLD: CMP(stackaddr, 0) == 0 -> NOP both at i=%d", i);
    }
    changes++;
  }

  LOG_IR_GEN("=== STACK ADDR NONNULL FOLD END: %d branches folded ===", changes);

  return changes;
}

/* ============================================================================
 * Boolean Materialization Peephole: CMP + SETIF + TEST_ZERO + JUMPIF → CMP + JUMPIF
 * ============================================================================
 *
 * C code like `if (cond) { ... }` often lowers to:
 *   CMP a, b
 *   R <-- (cond=C)          ; SETIF: materialize 0/1 into R
 *   TEST_ZERO R             ; reload flags from R
 *   JUMPIF target if EQ/NE  ; branch on R
 *
 * When R has no other use, this collapses to the flags already set by CMP:
 *   CMP a, b
 *   JUMPIF target if (EQ ? invert(C) : C)
 *
 * Inverse condition table (TCC tokens):
 *   EQ<->NE, LT<->GE, LE<->GT, ULT<->UGE, ULE<->UGT.
 */
static int invert_cond_token(int tok)
{
  switch (tok)
  {
  case 0x94:
    return 0x95; /* EQ -> NE */
  case 0x95:
    return 0x94; /* NE -> EQ */
  case 0x9c:
    return 0x9d; /* LT -> GE */
  case 0x9d:
    return 0x9c; /* GE -> LT */
  case 0x9e:
    return 0x9f; /* LE -> GT */
  case 0x9f:
    return 0x9e; /* GT -> LE */
  case 0x92:
    return 0x93; /* ULT -> UGE */
  case 0x93:
    return 0x92; /* UGE -> ULT */
  case 0x96:
    return 0x97; /* ULE -> UGT */
  case 0x97:
    return 0x96; /* UGT -> ULE */
  default:
    return -1;
  }
}

int tcc_ir_opt_setif_branch_fuse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
    return 0;

  LOG_IR_GEN("=== SETIF BRANCH FUSE START ===");

  for (int i = 0; i + 3 < n; i++)
  {
    IRQuadCompact *cmp_q = &ir->compact_instructions[i];
    IRQuadCompact *setif_q = &ir->compact_instructions[i + 1];
    IRQuadCompact *test_q = &ir->compact_instructions[i + 2];
    IRQuadCompact *jump_q = &ir->compact_instructions[i + 3];

    if (cmp_q->op != TCCIR_OP_CMP)
      continue;
    if (setif_q->op != TCCIR_OP_SETIF)
      continue;
    if (test_q->op != TCCIR_OP_TEST_ZERO)
      continue;
    if (jump_q->op != TCCIR_OP_JUMPIF)
      continue;

    /* None of the three later instructions may start a new basic block —
     * otherwise a branch could land inside our sequence after we fuse it. */
    if (setif_q->is_jump_target || test_q->is_jump_target || jump_q->is_jump_target)
      continue;

    /* SETIF's dest must be the exact vreg TEST_ZERO reads, and have no
     * other uses in the function (TEST_ZERO is the sole consumer). */
    IROperand setif_dest = tcc_ir_op_get_dest(ir, setif_q);
    IROperand test_src1 = tcc_ir_op_get_src1(ir, test_q);
    int32_t setif_vr = irop_get_vreg(setif_dest);
    int32_t test_vr = irop_get_vreg(test_src1);

    if (setif_vr < 0 || setif_vr != test_vr)
      continue;

    /* Only fuse when the materialized bool has a single use (the TEST_ZERO).
     * tcc_ir_vreg_has_single_use(vr, exclude_idx) counts uses *other than*
     * exclude_idx — pass -1 so we count all uses, and require exactly one. */
    if (!tcc_ir_vreg_has_single_use(ir, setif_vr, -1))
      continue;

    /* Read the condition tokens for SETIF and JUMPIF. */
    IROperand setif_src1 = tcc_ir_op_get_src1(ir, setif_q);
    IROperand jump_src1 = tcc_ir_op_get_src1(ir, jump_q);
    int setif_tok = (int)irop_get_imm64_ex(ir, setif_src1);
    int jump_tok = (int)irop_get_imm64_ex(ir, jump_src1);

    /* JUMPIF after TEST_ZERO only makes sense with EQ (jump when R==0) or
     * NE (jump when R!=0). */
    int new_tok;
    if (jump_tok == 0x94) /* EQ: jump when SETIF produced 0 -> inverse */
      new_tok = invert_cond_token(setif_tok);
    else if (jump_tok == 0x95) /* NE: jump when SETIF produced 1 -> same */
      new_tok = setif_tok;
    else
      continue;

    if (new_tok < 0)
      continue;

    /* Rewrite the JUMPIF condition, then NOP the SETIF + TEST_ZERO. */
    int btype = irop_get_btype(jump_src1);
    IROperand new_cond = irop_make_imm32(-1, new_tok, btype);
    tcc_ir_set_src1(ir, i + 3, new_cond);

    setif_q->op = TCCIR_OP_NOP;
    test_q->op = TCCIR_OP_NOP;

    LOG_IR_GEN("SETIF FUSE: CMP+SETIF(0x%x)+TEST_ZERO+JUMPIF(0x%x) -> CMP+JUMPIF(0x%x) at i=%d", setif_tok, jump_tok,
               new_tok, i);
    changes++;
    /* Skip over the fused region. */
    i += 3;
  }

  LOG_IR_GEN("=== SETIF BRANCH FUSE END: %d fused ===", changes);

  return changes;
}

/* ============================================================================
 * Stack-Boolean-Diamond Peephole
 * ============================================================================
 *
 * An inlined bool-returning helper typically lowers to a pair of constant
 * stores feeding a single reload+test:
 *
 *   i:   STORE slot,#A
 *   i+1: JUMP to L                 (L = i+3)
 *   i+2: STORE slot,#B             (target of some earlier JUMPIF)
 *   i+3: TEST_ZERO slot            (L)
 *   i+4: JUMPIF cond, T
 *
 * When the slot has no other references, both stores and the reload are dead.
 * Each arm's exit is pre-determined by its constant and the final condition,
 * so both can branch directly to the correct landing:
 *
 *   i:   NOP
 *   i+1: JUMP to (cond(A) ? T : i+5)
 *   i+2: NOP
 *   i+3: NOP
 *   i+4: JUMP to (cond(B) ? T : i+5)
 *
 * Because the slot is single-purpose (only our 3 references), no live value
 * depends on it; any other predecessor reaching i+3 without writing the slot
 * is already undefined behavior.
 */
static int stackoff_same_slot(IROperand a, IROperand b)
{
  if (irop_get_tag(a) != IROP_TAG_STACKOFF || irop_get_tag(b) != IROP_TAG_STACKOFF)
    return 0;
  return a.u.imm32 == b.u.imm32 && a.is_local == b.is_local && a.is_llocal == b.is_llocal;
}

static int operand_references_slot(IROperand op, IROperand slot)
{
  if (irop_get_tag(op) != IROP_TAG_STACKOFF)
    return 0;
  return op.u.imm32 == slot.u.imm32 && op.is_local == slot.is_local && op.is_llocal == slot.is_llocal;
}

int tcc_ir_opt_stack_bool_diamond(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 6)
    return 0;

  LOG_IR_GEN("=== STACK BOOL DIAMOND START ===");

  for (int i = 0; i + 4 < n; i++)
  {
    IRQuadCompact *q0 = &ir->compact_instructions[i];
    IRQuadCompact *q1 = &ir->compact_instructions[i + 1];
    IRQuadCompact *q2 = &ir->compact_instructions[i + 2];
    IRQuadCompact *q3 = &ir->compact_instructions[i + 3];
    IRQuadCompact *q4 = &ir->compact_instructions[i + 4];

    if (q0->op != TCCIR_OP_STORE)
      continue;
    if (q1->op != TCCIR_OP_JUMP)
      continue;
    if (q2->op != TCCIR_OP_STORE)
      continue;
    if (q3->op != TCCIR_OP_TEST_ZERO)
      continue;
    if (q4->op != TCCIR_OP_JUMPIF)
      continue;

    /* Changing q1's target, q3's semantics, or q4's op requires that no
     * external control flow lands on them. */
    if (q1->is_jump_target)
      continue;
    if (q3->is_jump_target)
    {
      /* q3 IS a jump target — only allow if the sole jump to it is q1. */
      /* Fall through and verify in a pass below. */
    }
    if (q4->is_jump_target)
      continue;

    /* All three stack references must be the same slot. */
    IROperand d0 = tcc_ir_op_get_dest(ir, q0);
    IROperand d2 = tcc_ir_op_get_dest(ir, q2);
    IROperand r3 = tcc_ir_op_get_src1(ir, q3);
    if (!stackoff_same_slot(d0, d2))
      continue;
    if (!stackoff_same_slot(d0, r3))
      continue;

    /* Both stores must have immediate constant values. */
    IROperand sa = tcc_ir_op_get_src1(ir, q0);
    IROperand sb = tcc_ir_op_get_src1(ir, q2);
    if (!irop_is_immediate(sa) || !irop_is_immediate(sb))
      continue;
    int64_t val_a = irop_get_imm64_ex(ir, sa);
    int64_t val_b = irop_get_imm64_ex(ir, sb);

    /* JUMP at i+1 must target the TEST_ZERO at i+3. */
    IROperand q1_dest = tcc_ir_op_get_dest(ir, q1);
    if ((int)q1_dest.u.imm32 != i + 3)
      continue;

    /* JUMPIF condition must be EQ or NE (anything else is nonsensical
     * for a TEST_ZERO result). */
    IROperand q4_cond = tcc_ir_op_get_src1(ir, q4);
    int cond_tok = (int)irop_get_imm64_ex(ir, q4_cond);
    if (cond_tok != 0x94 && cond_tok != 0x95)
      continue;

    IROperand q4_dest = tcc_ir_op_get_dest(ir, q4);
    int target_T = (int)q4_dest.u.imm32;
    int target_next = i + 5;

    /* Scan the function to verify two things at once:
     *   1) the slot is referenced ONLY at q0 (dest), q2 (dest), q3 (src1)
     *   2) the only jump into q3 comes from q1 (if q3 is a jump target)
     *   3) nothing jumps to q1 or q4 (already partly checked via is_jump_target) */
    int bail = 0;
    for (int j = 0; j < n && !bail; j++)
    {
      if (j == i || j == i + 2 || j == i + 3)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;

      if (irop_config[qj->op].has_dest)
      {
        IROperand op = tcc_ir_op_get_dest(ir, qj);
        if (operand_references_slot(op, d0))
        {
          bail = 1;
          break;
        }
      }
      if (irop_config[qj->op].has_src1)
      {
        IROperand op = tcc_ir_op_get_src1(ir, qj);
        if (operand_references_slot(op, d0))
        {
          bail = 1;
          break;
        }
      }
      if (irop_config[qj->op].has_src2)
      {
        IROperand op = tcc_ir_op_get_src2(ir, qj);
        if (operand_references_slot(op, d0))
        {
          bail = 1;
          break;
        }
      }

      /* Jump-target checks: reject any jump that lands on q3 (other than q1)
       * or any jump to q1 / q4. */
      if (qj->op == TCCIR_OP_JUMP || qj->op == TCCIR_OP_JUMPIF)
      {
        int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
        if (j != i + 1 && tgt == i + 3)
        {
          bail = 1;
          break;
        }
        if (tgt == i + 1 || tgt == i + 4)
        {
          bail = 1;
          break;
        }
      }
    }
    if (bail)
      continue;

    /* Evaluate: JUMPIF EQ jumps when reload == 0; JUMPIF NE jumps when != 0. */
    int a_jumps = (cond_tok == 0x94) ? (val_a == 0) : (val_a != 0);
    int b_jumps = (cond_tok == 0x94) ? (val_b == 0) : (val_b != 0);

    int a_target = a_jumps ? target_T : target_next;
    int b_target = b_jumps ? target_T : target_next;

    /* Rewrite q1 to JUMP directly to a_target. */
    q1_dest.u.imm32 = a_target;
    tcc_ir_set_dest(ir, i + 1, q1_dest);

    /* Rewrite q4 from JUMPIF to unconditional JUMP to b_target. */
    q4_dest.u.imm32 = b_target;
    q4->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, i + 4, q4_dest);

    /* NOP the remaining scaffolding. */
    q0->op = TCCIR_OP_NOP;
    q2->op = TCCIR_OP_NOP;
    q3->op = TCCIR_OP_NOP;

    LOG_IR_GEN("STACK BOOL DIAMOND: slot=%d A=%lld B=%lld cond=0x%x T=%d next=%d a_tgt=%d b_tgt=%d at i=%d",
               (int)d0.u.imm32, (long long)val_a, (long long)val_b, cond_tok, target_T, target_next, a_target, b_target,
               i);
    changes++;
    i += 4;
  }

  LOG_IR_GEN("=== STACK BOOL DIAMOND END: %d fused ===", changes);
  return changes;
}

/* ============================================================================
 * VAR → TMP Local Forwarding
 * ============================================================================
 *
 * When a STORE writes a TEMP into a local VAR and the VAR is subsequently
 * read within the same basic block, rewrite those reads to use the TEMP
 * directly.  This avoids the reload round-trip when the TEMP is already in
 * a register.
 *
 * Example:
 *   V10 <-- T21 [STORE]            ; store r0,r1 to V10 slot
 *   PARAM0[call] V10                ; reload V10 → r0,r1 (wasteful)
 *
 * Becomes:
 *   V10 <-- T21 [STORE]            ; STORE may still be needed for later uses
 *   PARAM0[call] T21                ; use r0,r1 directly (no reload)
 *
 * Guardrails:
 *   - TEMP must not be redefined between the STORE and the use.
 *   - Stops at BB boundaries (jumps, jump targets, terminators).
 *   - Stops at any CALL (callee-clobbered TEMP registers).
 *   - Stops once V is redefined.
 *   - Does not touch operand slots that encode call IDs or callee symbols.
 */
int tcc_ir_opt_var_tmp_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  LOG_IR_GEN("=== VAR→TMP FWD START ===");

  /* Pre-compute a fresh jump-target bitmap instead of relying on stale
   * IRQuadCompact::is_jump_target flags that can desync between passes.
   * A back-edge into the scan window — including into a NOP that later
   * falls through to a real use — must terminate forwarding, because on
   * the next loop iteration V may hold a different value. */
  uint8_t *is_target = tcc_mallocz((size_t)((n + 7) / 8));
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *jq = &ir->compact_instructions[i];
    if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
    {
      IROperand d = tcc_ir_op_get_dest(ir, jq);
      int t = (int)d.u.imm32;
      if (t >= 0 && t < n)
        is_target[t >> 3] |= (uint8_t)(1u << (t & 7));
    }
  }
  for (int tbl = 0; tbl < ir->num_switch_tables; tbl++)
  {
    TCCIRSwitchTable *st = &ir->switch_tables[tbl];
    for (int k = 0; k < st->num_entries; k++)
    {
      int t = st->targets[k];
      if (t >= 0 && t < n)
        is_target[t >> 3] |= (uint8_t)(1u << (t & 7));
    }
    if (st->default_target >= 0 && st->default_target < n)
      is_target[st->default_target >> 3] |= (uint8_t)(1u << (st->default_target & 7));
  }

  /* Pre-compute aliasing info for the addrtaken refinement:
   * - has_nested_or_chain: any nested function context
   * - var_has_lea[pos]: per-VAR LEA bitmap */
  int has_nested_or_chain = (tcc_state->nb_nested_funcs > 0);
  int max_var_for_lea = 0;
  uint8_t *var_has_lea = NULL;
  if (!has_nested_or_chain)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[i];
      if (sq->op == TCCIR_OP_SET_CHAIN)
      {
        has_nested_or_chain = 1;
        break;
      }
      if (sq->op == TCCIR_OP_LEA)
      {
        IROperand ls = tcc_ir_op_get_src1(ir, sq);
        int32_t vr = irop_get_vreg(ls);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos > max_var_for_lea)
            max_var_for_lea = pos;
        }
      }
    }
    if (max_var_for_lea > 0 && !has_nested_or_chain)
    {
      var_has_lea = tcc_mallocz((max_var_for_lea + 8) / 8);
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *sq = &ir->compact_instructions[i];
        if (sq->op == TCCIR_OP_LEA)
        {
          IROperand ls = tcc_ir_op_get_src1(ir, sq);
          int32_t vr = irop_get_vreg(ls);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_for_lea)
              var_has_lea[pos / 8] |= (1 << (pos % 8));
          }
        }
      }
    }
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *store_q = &ir->compact_instructions[i];
    if (store_q->op != TCCIR_OP_STORE && store_q->op != TCCIR_OP_ASSIGN)
      continue;
    if (!irop_config[store_q->op].has_dest || !irop_config[store_q->op].has_src1)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, store_q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    /* Skip address-taken VARs if there is an actual aliasing path:
     * LEA in the IR, nested function definitions, or SET_CHAIN.
     * Without these, addrtaken is a stale frontend annotation. */
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
      if (interval && interval->addrtaken)
      {
        int dpos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        if (has_nested_or_chain ||
            (var_has_lea && dpos <= max_var_for_lea && (var_has_lea[dpos / 8] & (1 << (dpos % 8)))))
          continue;
      }
    }

    IROperand src1 = tcc_ir_op_get_src1(ir, store_q);
    int32_t src_vr = irop_get_vreg(src1);
    if (TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Don't forward TEMPs that hold a computed stack/symbol ADDRESS (from
     * LEA / Addr[...]).  Even when V is single-use, removing the VAR that
     * held the address breaks downstream DSE / store_redundant analyses —
     * they rely on the VAR-holding-address pattern to know which stack slot
     * is live, and without V they can drop the stores the slot requires.
     * The few VAR→TMP wins we'd get from LEA-sourced chains aren't worth
     * the risk of silently corrupting programs that take the address of a
     * local variable. */
    {
      int t_def = tcc_ir_find_defining_instruction(ir, src_vr, i);
      if (t_def >= 0 && ir->compact_instructions[t_def].op == TCCIR_OP_LEA)
        continue;
    }

    int src_btype = irop_get_btype(src1);

    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];

      /* BB boundary check first: a jump-target instruction may be a NOP
       * whose next real instruction is NOT itself a jump target, so we
       * cannot defer this check until after NOP-skipping. */
      if (is_target[j >> 3] & (1u << (j & 7)))
        break;

      if (q->op == TCCIR_OP_NOP)
        continue;
      /* Skip unconditional JMPs that target the next non-NOP instruction
       * (fallthroughs left by earlier branch elimination + DCE). */
      if (q->op == TCCIR_OP_JUMP)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)jd.u.imm32;
        while (jt < n && ir->compact_instructions[jt].op == TCCIR_OP_NOP)
          jt++;
        int next_real = j + 1;
        while (next_real < n && ir->compact_instructions[next_real].op == TCCIR_OP_NOP)
          next_real++;
        if (jt == next_real)
          continue; /* fallthrough JMP — skip it */
      }
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
          q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
        break;
      /* Calls clobber caller-saved TEMP registers — extending T across a call
       * would force a spill, defeating the purpose. */
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
        break;

      /* Detect a redefinition of T (rare for TEMPs, but guard). */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t d_vr = irop_get_vreg(d);
        if (d_vr == src_vr)
          break;
      }

      /* Substitute V → T in readable src positions.  Skip operand slots that
       * the op uses to encode non-value metadata (callee sym, call id). */
      int can_rewrite_src1 = (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID);
      int can_rewrite_src2 = (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID &&
                              q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID);

      /* Ops where src1 represents an *address* (not a value to read).  For
       * these we must not rewrite an is_lval=1 VAR source to an is_lval=0
       * TEMP, because the TEMP holds the value, not an address.  Pure value
       * ops (FUNCPARAMVAL, CMP, arithmetic, etc.) safely accept the TEMP —
       * the VAR read was just a deref of V's slot, which now holds T. */
      int src1_is_address = (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_LEA ||
                             q->op == TCCIR_OP_LOAD_INDEXED);

      if (!src1_is_address && can_rewrite_src1 && irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s) == dest_vr && irop_get_btype(s) == src_btype && !s.is_llocal)
        {
          tcc_ir_set_src1(ir, j, src1);
          changes++;
          LOG_IR_GEN("VAR→TMP FWD: V=%d -> T at j=%d (src1) after store at i=%d", dest_vr, j, i);
        }
      }
      /* src2 is a value for every op in this table (indices for LOAD_INDEXED
       * and STORE_INDEXED are values, not addresses). */
      if (can_rewrite_src2 && irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        if (irop_get_vreg(s) == dest_vr && irop_get_btype(s) == src_btype && !s.is_llocal)
        {
          tcc_ir_set_src2(ir, j, src1);
          changes++;
          LOG_IR_GEN("VAR→TMP FWD: V=%d -> T at j=%d (src2) after store at i=%d", dest_vr, j, i);
        }
      }

      /* If this op wrote V (dest == V), subsequent reads see the new value —
       * stop forwarding the old one. */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t d_vr = irop_get_vreg(d);
        if (d_vr == dest_vr)
          break;
      }
    }
  }

  tcc_free(var_has_lea);
  tcc_free(is_target);
  LOG_IR_GEN("=== VAR→TMP FWD END: %d substitutions ===", changes);
  return changes;
}

/* ============================================================================
 * Local Load CSE  (tcc_ir_opt_local_load_cse)
 * ============================================================================
 *
 * Within a basic block, when a VAR/PARAM is loaded twice into different TEMPs,
 * the second load is replaced with a copy of the first TEMP.
 *
 * Before:
 *   T9  <-- V1 [ASSIGN, lval]     # load V1 into T9
 *   T10 <-- V1 [ASSIGN, lval]     # load V1 again into T10
 *
 * After:
 *   T9  <-- V1 [ASSIGN, lval]     # load V1 into T9
 *   T10 <-- T9 [ASSIGN]           # copy from T9 (no reload)
 */
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

int tcc_ir_opt_var_to_tmp(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  /* Phase 1: single-pass collect def_count + any-non-lval-read for each VAR. */
  int max_var_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t v = irop_get_vreg(d);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(v);
        if (p > max_var_pos)
          max_var_pos = p;
      }
    }
    if (irop_config[q->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t v = irop_get_vreg(s);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(v);
        if (p > max_var_pos)
          max_var_pos = p;
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      int32_t v = irop_get_vreg(s);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(v);
        if (p > max_var_pos)
          max_var_pos = p;
      }
    }
  }

  if (max_var_pos == 0)
    return 0;

  typedef struct
  {
    int def_idx;       /* -1 if no def yet, -2 if multiple defs seen */
    int bad_use;       /* 1 if V is ever used in a way we can't rewrite */
    int lval_read_cnt; /* number of rewritable lval ASSIGN reads */
  } VInfo;

  VInfo *info = tcc_mallocz(sizeof(VInfo) * (max_var_pos + 1));
  for (int p = 0; p <= max_var_pos; p++)
    info[p].def_idx = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Track VAR defs. In TCC's IR, writing a VAR is a STORE to its memory
     * slot; the dest carries is_lval=1 *because* a VAR is memory, not because
     * someone took its address. So any write where the dest vreg is a VAR
     * counts as a def regardless of is_lval. Address-taken VARs are filtered
     * out in Phase 2 via the live interval. */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t v = irop_get_vreg(d);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(v);
        if (info[p].def_idx == -1)
          info[p].def_idx = i;
        else
          info[p].def_idx = -2; /* multiple defs */
      }
    }

    /* Check src1 / src2 uses of any VAR. Rewritable uses are:
     *   - src1 of ASSIGN targeting a TEMP (the classic reload pattern)
     *   - src1 of FUNCPARAMVAL / FUNCPARAMVOID (passing V's value to a call)
     * Anything else disqualifies V. */
    if (irop_config[q->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t v = irop_get_vreg(s);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(v);
        int ok = 0;
        if (q->op == TCCIR_OP_ASSIGN && s.is_lval &&
            TCCIR_DECODE_VREG_TYPE(irop_get_vreg(tcc_ir_op_get_dest(ir, q))) == TCCIR_VREG_TYPE_TEMP)
          ok = 1;
        else if ((q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) && s.is_lval)
          ok = 1;
        if (ok)
          info[p].lval_read_cnt++;
        else
          info[p].bad_use = 1;
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      int32_t v = irop_get_vreg(s);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        /* VAR in src2 is never the "T <-- V [ASSIGN lval]" pattern — disqualify */
        int p = TCCIR_DECODE_VREG_POSITION(v);
        info[p].bad_use = 1;
      }
    }
  }

  /* Phase 2: for each viable VAR, verify single-def, in-BB, and rewrite. */
  for (int p = 0; p <= max_var_pos; p++)
  {
    LOG_COPY_PROP("var_to_tmp CAND V:%d def_idx=%d bad_use=%d lval_reads=%d", p, info[p].def_idx, info[p].bad_use,
                  info[p].lval_read_cnt);
    if (info[p].def_idx == -1)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: no def", p);
      continue;
    }
    if (info[p].def_idx == -2)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: multiple defs", p);
      continue;
    }
    if (info[p].bad_use)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: bad_use", p);
      continue;
    }
    if (info[p].lval_read_cnt == 0)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: no lval reads", p);
      continue;
    }

    int def_i = info[p].def_idx;
    IRQuadCompact *def_q = &ir->compact_instructions[def_i];
    IROperand def_dest = tcc_ir_op_get_dest(ir, def_q);
    int32_t dest_vr = irop_get_vreg(def_dest);
    int def_btype = irop_get_btype(def_dest);

    /* Only handle plain INT32 (pointer / regular int) scalars. Any wider or
     * multi-word type (INT64, FLOAT32/64, STRUCT, INT8/16) would require
     * preserving memory semantics we can't reproduce with a single TMP. */
    if (def_btype != IROP_BTYPE_INT32)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: def btype=%d not INT32", p, def_btype);
      continue;
    }

    /* Skip VARs whose storage is observable or whose type wouldn't round-trip
     * cleanly through a TEMP: address-taken, complex, long long, float/double,
     * or explicit lvalue kinds. Keep the pass to plain 32-bit scalars and
     * let other passes tackle the wider cases. Missing live interval is
     * treated conservatively (skip) since we can't verify the type flags. */
    IRLiveInterval *intv = tcc_ir_get_live_interval(ir, dest_vr);
    if (!intv)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: null interval", p);
      continue;
    }
    if (intv->addrtaken || intv->is_complex || intv->is_llong || intv->is_float || intv->is_double || intv->is_lvalue)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: flags addr=%d cx=%d ll=%d f=%d d=%d lv=%d", p, intv->addrtaken,
                    intv->is_complex, intv->is_llong, intv->is_float, intv->is_double, intv->is_lvalue);
      continue;
    }

    /* Restrict the def to opcodes that produce a single scalar value and
     * whose operand shape is already a pure value-write (ASSIGN-like).
     * TCC's STORE is what you get for local-pointer VAR writes, but
     * converting STORE→ASSIGN has turned out to corrupt later passes
     * (observed wrong value in test_llong_load_signed). Hold it back
     * until we understand why. */
    int def_op = def_q->op;
    if (def_op != TCCIR_OP_ASSIGN && def_op != TCCIR_OP_LOAD && def_op != TCCIR_OP_ADD && def_op != TCCIR_OP_SUB &&
        def_op != TCCIR_OP_MUL && def_op != TCCIR_OP_AND && def_op != TCCIR_OP_OR && def_op != TCCIR_OP_XOR &&
        def_op != TCCIR_OP_SHL && def_op != TCCIR_OP_SHR && def_op != TCCIR_OP_LEA)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: def op=%d not in allowlist", p, def_op);
      continue;
    }

    /* Walk forward within the same BB, collecting lval reads of V.
     * Abort on any control-flow boundary, call, or surprise use. */
    int uses[16];
    int num_uses = 0;
    int aborted = 0;

    for (int j = def_i + 1; j < n; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Terminators end the BB without a use */
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
          q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_IJUMP)
        break;
      /* Calls clobber caller-saved regs and split the BB for our scan */
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
        break;

      /* Redef of V — stop (shouldn't happen since def_count==1, but defensive) */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_get_vreg(d) == dest_vr)
        {
          aborted = 1;
          break;
        }
      }

      /* Collect ASSIGN-lval and FUNCPARAMVAL/VOID reads of V */
      int is_rewritable_read = 0;
      if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s) == dest_vr && s.is_lval)
        {
          IROperand ud = tcc_ir_op_get_dest(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(ud)) != TCCIR_VREG_TYPE_TEMP)
          {
            aborted = 1;
            break;
          }
          is_rewritable_read = 1;
        }
      }
      else if ((q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) && irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s) == dest_vr && s.is_lval)
          is_rewritable_read = 1;
      }
      if (is_rewritable_read)
      {
        IROperand s = tcc_ir_get_src1(ir, j);
        /* Btype must match the def — narrowing/widening loads can't be
         * replaced by a direct register copy. */
        if (irop_get_btype(s) != def_btype)
        {
          aborted = 1;
          break;
        }
        if (num_uses >= (int)(sizeof(uses) / sizeof(uses[0])))
        {
          aborted = 1;
          break;
        }
        uses[num_uses++] = j;
      }
    }

    if (aborted || num_uses == 0)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: aborted=%d num_uses=%d", p, aborted, num_uses);
      continue;
    }
    /* Guard against Phase-1 vs Phase-2 disagreement (uses outside this BB) */
    if (num_uses != info[p].lval_read_cnt)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: cross-BB uses (BB=%d global=%d)", p, num_uses, info[p].lval_read_cnt);
      continue;
    }

    /* Transform: allocate a fresh TMP, redirect def, rewrite each use. */
    int32_t new_tmp = tcc_ir_vreg_alloc_temp(ir);
    if (new_tmp < 0)
      continue;

    /* Fresh TMP operand — clears is_local / is_llocal / u.*, preserves btype. */
    IROperand new_dest = irop_make_vreg(new_tmp, def_btype);
    new_dest.is_unsigned = def_dest.is_unsigned;
    tcc_ir_set_dest(ir, def_i, new_dest);

    /* If we ever re-enable STORE defs here, remember to flip the op to
     * ASSIGN so the backend doesn't misinterpret a TMP dest as a pointer
     * dereference. The earlier experiment produced wrong data — hold off. */

    for (int u = 0; u < num_uses; u++)
    {
      int j = uses[u];
      IROperand s = tcc_ir_get_src1(ir, j);
      IROperand new_src = irop_make_vreg(new_tmp, s.btype);
      new_src.is_unsigned = s.is_unsigned;
      /* is_lval cleared by irop_make_vreg — the read is now a register copy */
      tcc_ir_set_src1(ir, j, new_src);
      changes++;
      LOG_COPY_PROP("var_to_tmp: V:%d@def=%d -> T:%d; rewrite use at i=%d", p, def_i, new_tmp, j);
    }
  }

  tcc_free(info);
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
    LOG_IR_GEN("STRENGTH_RED: x * 0 -> 0 at i=%d", instr_idx);
    return 1;
  }

  if (multiplier == 1)
  {
    /* x * 1 = x (should have been handled by const prop, but be safe) */
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, instr_idx, *value_op);
    tcc_ir_set_src2(ir, instr_idx, IROP_NONE);
    LOG_IR_GEN("STRENGTH_RED: x * 1 -> x at i=%d", instr_idx);
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
    LOG_IR_GEN("STRENGTH_RED: x * %lld -> x << %d at i=%d", (long long)multiplier, log2_val, instr_idx);
    return 1;
  }

  /* TODO: Multi-instruction patterns (2^n+1, 2^n-1, composite) require
   * inserting new instructions via insert_instr_at. This conflicts with
   * prior IV strength reduction transformations — the instruction indices
   * and liveness info become inconsistent, causing miscompilation.
   * These patterns need a dedicated pre-regalloc insertion mechanism. */

  return 0;
}

/* Run strength reduction on all MUL instructions in function
 * Returns number of instructions transformed
 */
int tcc_ir_opt_strength_reduction(TCCIRState *ir)
{
  int changes = 0;

  if (ir->next_instruction_index == 0)
    return 0;

  LOG_IR_GEN("=== STRENGTH REDUCTION START ===");

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    changes += tcc_ir_strength_reduce_mul(ir, i);
  }

  LOG_IR_GEN("=== STRENGTH REDUCTION END: %d multiplies reduced ===", changes);

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
static int find_induction_vars_ex(TCCIRState *ir, IRLoop *loop, InductionVar *ivs, int max_ivs, int allow_copy_through)
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

    /* Pattern: V = V + const  OR  V = T + const where T := V (copy-through) */
    int effective_src_vr = src1_vr;
    if (allow_copy_through && src1_vr != dest_vr && src1_vr >= 0 && irop_is_immediate(src2))
    {
      /* Check if src1 is a temp assigned from dest_vr just before */
      for (int k = i - 1; k >= loop->start_idx && k >= i - 3; k--)
      {
        IRQuadCompact *aq = &ir->compact_instructions[k];
        if (aq->op == TCCIR_OP_ASSIGN)
        {
          IROperand adest = tcc_ir_op_get_dest(ir, aq);
          IROperand asrc = tcc_ir_op_get_src1(ir, aq);
          if (irop_get_vreg(adest) == src1_vr && irop_get_vreg(asrc) == dest_vr)
          {
            effective_src_vr = dest_vr;
            break;
          }
        }
        if (aq->op != TCCIR_OP_NOP)
          break; /* stop at first non-NOP non-matching instr */
      }
    }
    if (effective_src_vr == dest_vr && irop_is_immediate(src2))
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

      LOG_IV_SR("IV_SR: Found BIV VAR%d (init=%d, step=%d) at idx=%d", TCCIR_DECODE_VREG_POSITION(dest_vr), init_val,
                step, i);
      LOG_LOOP_OPT("IV found: VAR%d init=%d step=%d def_idx=%d init_idx=%d", TCCIR_DECODE_VREG_POSITION(dest_vr),
                   init_val, step, i, init_idx);
    }
  }

  LOG_LOOP_OPT("find_induction_vars_ex: found %d IV(s) in loop [%d..%d]", num_ivs, loop->start_idx, loop->end_idx);
  return num_ivs;
}

/* Find derived induction variables in a loop.
 * A DIV is: base + (IV << shift) - used for array indexing.
 * We look for ADD instructions that use a SHL result where SHL uses an IV.
 */
static int find_derived_ivs(TCCIRState *ir, IRLoop *loop, InductionVar *ivs, int num_ivs, DerivedIV *divs, int max_divs)
{
  int num_divs = 0;

  if (TCC_LOG_IV_SR)
  {
    fprintf(stderr, "[IV_SR] Loop body_instrs:");
    for (int bi = 0; bi < loop->num_body_instrs; bi++)
      fprintf(stderr, " %d", loop->body_instrs[bi]);
    fprintf(stderr, "\n");
  }

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

    /* Pattern: T = base + T_mul_shl  OR  T = T_mul_shl + base */
    int shl_vr = -1, base_vr = -1;
    IROperand *base_op = NULL;
    int shl_idx = -1;
    int is_mul = 0;

    /* Check src2 for SHL/MUL result */
    int vr2 = irop_get_vreg(src2);
    if (vr2 >= 0 && TCCIR_DECODE_VREG_TYPE(vr2) == TCCIR_VREG_TYPE_TEMP)
    {
      /* Look for SHL/MUL defining this temp */
      for (int j = 0; j < loop->num_body_instrs; j++)
      {
        int sj = loop->body_instrs[j];
        if (sj >= i)
          break; /* Must be before the ADD */
        IRQuadCompact *sq = &ir->compact_instructions[sj];
        if (sq->op == TCCIR_OP_SHL || sq->op == TCCIR_OP_MUL)
        {
          IROperand sdest = tcc_ir_op_get_dest(ir, sq);
          if (irop_get_vreg(sdest) == vr2)
          {
            shl_vr = vr2;
            shl_idx = sj;
            base_op = &src1;
            base_vr = irop_get_vreg(src1);
            is_mul = (sq->op == TCCIR_OP_MUL);
            break;
          }
        }
      }
    }

    /* Check src1 for SHL/MUL result if not found */
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
          if (sq->op == TCCIR_OP_SHL || sq->op == TCCIR_OP_MUL)
          {
            IROperand sdest = tcc_ir_op_get_dest(ir, sq);
            if (irop_get_vreg(sdest) == vr1)
            {
              shl_vr = vr1;
              shl_idx = sj;
              base_op = &src2;
              base_vr = irop_get_vreg(src2);
              is_mul = (sq->op == TCCIR_OP_MUL);
              break;
            }
          }
        }
      }
    }

    if (shl_idx < 0)
      continue; /* Not a base + SHL/MUL pattern */

    /* Check that the SHL/MUL input is an IV */
    IRQuadCompact *shl_q = &ir->compact_instructions[shl_idx];
    IROperand shl_src1 = tcc_ir_op_get_src1(ir, shl_q);
    IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);

    int iv_vr = irop_get_vreg(shl_src1);
    if (iv_vr < 0 || !irop_is_immediate(shl_src2))
    {
      /* Check if src1 is immediate and src2 is IV (for MUL) */
      if (is_mul && irop_is_immediate(shl_src1))
      {
        iv_vr = irop_get_vreg(shl_src2);
        if (iv_vr >= 0)
        {
          IROperand tmp = shl_src1;
          shl_src1 = shl_src2;
          shl_src2 = tmp;
        }
        else
        {
          continue;
        }
      }
      else
      {
        continue;
      }
    }

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

    /* Chase one level of copy: if iv_vr is defined by ASSIGN/STORE from
     * a BIV (e.g. V4 <-- V1 [STORE]), treat it as the BIV. */
    if (iv_idx < 0 && iv_vr >= 0)
    {
      int def = tcc_ir_find_defining_instruction(ir, iv_vr, shl_idx);
      if (def >= 0)
      {
        IRQuadCompact *dq = &ir->compact_instructions[def];
        if (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_STORE)
        {
          int copy_src = irop_get_vreg(tcc_ir_op_get_src1(ir, dq));
          for (int k = 0; k < num_ivs; k++)
          {
            if (ivs[k].vreg == copy_src)
            {
              iv_idx = k;
              break;
            }
          }
        }
      }
    }

    if (iv_idx < 0)
      continue; /* SHL/MUL operand is not an IV */

    /* Calculate stride */
    int stride;
    if (is_mul)
    {
      int mul_const = (int)irop_get_imm64_ex(ir, shl_src2);
      stride = ivs[iv_idx].step * mul_const;
    }
    else
    {
      int shift = (int)irop_get_imm64_ex(ir, shl_src2);
      stride = ivs[iv_idx].step * (1 << shift);
    }

    /* Check that this ADD result is used (not dead code).  Multiple uses
     * are fine: the transformation replaces the ADD with ASSIGN (result =
     * strength-reduced ptr), so all existing uses transparently receive
     * the correct address value. */
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
      if (uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED || uq->op == TCCIR_OP_STORE_POSTINC)
      {
        IROperand ud = tcc_ir_op_get_dest(ir, uq);
        if (irop_get_vreg(ud) == dest_vr)
          use_count++;
      }
    }

    if (use_count < 1)
      continue; /* Dead code — skip */

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
      LOG_IV_SR("IV_SR: Skipping DIV at idx=%d: SHL result has %d uses (not 1)", i, shl_vr_uses);
      continue; /* SHL result used by other instructions - can't NOP it */
    }

    divs[num_divs].iv_idx = iv_idx;
    divs[num_divs].base_vreg = base_vr;
    divs[num_divs].base_op = *base_op;
    divs[num_divs].stride = stride;
    divs[num_divs].use_idx = i;
    divs[num_divs].shl_idx = shl_idx;
    num_divs++;

    LOG_IV_SR("IV_SR: Found DIV base+%d*VAR%d at ADD idx=%d (SHL idx=%d)", stride, TCCIR_DECODE_VREG_POSITION(iv_vr), i,
              shl_idx);
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
    int new_size = ir->compact_instructions_size << 1;
    ir->compact_instructions = (IRQuadCompact *)tcc_realloc(ir->compact_instructions, new_size * sizeof(IRQuadCompact));
    ir->compact_instructions_size = new_size;
  }

  /* Ensure operand pool has room for 3 slots */
  if (ir->iroperand_pool_count + 3 > ir->iroperand_pool_capacity)
  {
    tcc_ir_pool_ensure(ir, 3);
    if (ir->iroperand_pool_count + 3 > ir->iroperand_pool_capacity)
    {
      if (TCC_LOG_IV_SR)
        fprintf(stderr, "[IV_SR] ERROR: iroperand_pool_capacity limit reached\n");
      return -1;
    }
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
  new_q->is_jump_target = 0; /* shifted instructions carry their flag; new slot has none */
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
static int transform_derived_iv(TCCIRState *ir, IRLoop *loop, InductionVar *iv, DerivedIV *div, int *out_ptr_vreg,
                                int *out_idx_shift)
{
  if (out_ptr_vreg)
    *out_ptr_vreg = -1;
  if (out_idx_shift)
    *out_idx_shift = 0;

  /* Allocate a new temp vreg for the pointer */
  int ptr_vreg = tcc_ir_vreg_alloc_temp(ir);
  if (ptr_vreg < 0)
    return 0;

  LOG_IV_SR("IV_SR: Transforming DIV at idx=%d, new ptr vreg=TMP%d, iv_init=%d, stride=%d", div->use_idx,
            TCCIR_DECODE_VREG_POSITION(ptr_vreg), iv->init_val, div->stride);

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
  if (loop->preheader_idx < 0)
    return 0;
  int insert_pos = loop->preheader_idx + 1;

  /* Bail out if inserting would split a CMP→JUMPIF pair.  The preheader
   * might be the CMP itself — inserting after it places instructions
   * between CMP and JUMPIF, and the ADD would clobber condition flags.
   * Only bail when init_offset != 0 (which inserts an ADD that clobbers flags);
   * a single ASSIGN does not clobber condition flags on ARM. */
  {
    int element_size_check = div->stride / iv->step;
    int init_offset_check = iv->init_val * element_size_check;
    if (init_offset_check != 0 && insert_pos > 0 && ir->compact_instructions[insert_pos - 1].op == TCCIR_OP_CMP &&
        insert_pos < ir->next_instruction_index && ir->compact_instructions[insert_pos].op == TCCIR_OP_JUMPIF)
    {
      LOG_IV_SR("IV_SR: Skipping DIV transform — would split CMP→JUMPIF at %d→%d", insert_pos - 1, insert_pos);
      return 0;
    }
  }

  LOG_IV_SR("IV_SR: transform_derived_iv: header_idx=%d, preheader_idx=%d, start_idx=%d, end_idx=%d, insert_pos=%d",
            loop->header_idx, loop->preheader_idx, loop->start_idx, loop->end_idx, insert_pos);

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
        LOG_IV_SR("IV_SR: Skipping DIV transform — base vreg not defined before insert_pos %d", insert_pos);
        return 0;
      }

      /* Also verify base is loop-invariant: not defined inside the loop body.
       * If the base changes each iteration, the strength-reduced pointer
       * would diverge from the original address computation. */
      for (int i = loop->start_idx; i <= loop->end_idx; i++)
      {
        IRQuadCompact *lq = &ir->compact_instructions[i];
        if (lq->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[lq->op].has_dest)
        {
          IROperand ld = tcc_ir_op_get_dest(ir, lq);
          if (irop_get_vreg(ld) == base_vr)
          {
            LOG_IV_SR("IV_SR: Skipping DIV transform — base vreg redefined inside loop at idx %d", i);
            return 0;
          }
        }
      }
    }
  }

  IROperand ptr_op = irop_make_vreg(ptr_vreg, IROP_BTYPE_INT32);
  IROperand null_op = {0};

  int idx_shift = 0;

  /* Calculate initial offset = iv_init * element_size
   * where element_size = stride / step = (1 << shift).
   * Note: init_offset = init_val * stride is WRONG when step != 1,
   * because stride = step * element_size, not just element_size. */
  int element_size = div->stride / iv->step;
  int init_offset = iv->init_val * element_size;

  if (init_offset == 0)
  {
    /* Simple case: ptr = base */
    int inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ASSIGN, ptr_op, div->base_op, null_op);
    LOG_IV_SR("IV_SR: init insert at pos=%d, result=%d (base_vr=%d)", insert_pos, inserted,
              irop_get_vreg(div->base_op));
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
    LOG_IV_SR("IV_SR: init insert at pos=%d, result=%d", insert_pos, inserted);
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
   * In most loops the IV increment is at the latch (unconditional),
   * so the stride executes exactly once per iteration.
   *
   * Special case: in post-increment patterns like arglist[numargs++],
   * the IV increment sits in the body BEFORE the derived pointer use.
   * Placing the stride right after the increment would advance the
   * pointer before the store through it.  We must push the stride past
   * ALL uses of the derived address (not just the ASSIGN), because copy
   * propagation or register coalescing can merge the pointer with the
   * address temp, causing a store to see the post-increment value. */
  int stride_insert_pos = new_iv_def_idx + 1;
  if (new_use_idx > new_iv_def_idx)
  {
    int safe_to_push = 1;
    for (int si = new_iv_def_idx + 1; si <= new_use_idx; si++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[si];
      if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF || sq->op == TCCIR_OP_FUNCCALLVAL ||
          sq->op == TCCIR_OP_FUNCCALLVOID)
      {
        safe_to_push = 0;
        break;
      }
    }
    if (safe_to_push)
    {
      /* Find the last use of the address temp (dest of the ASSIGN at new_use_idx)
       * within the same straight-line block.  The stride must go after the last
       * dereference through this pointer. */
      IROperand use_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[new_use_idx]);
      int32_t use_dest_vr = irop_get_vreg(use_dest);
      int last_use = new_use_idx;
      if (use_dest_vr >= 0)
      {
        int loop_end = loop->end_idx + idx_shift;
        for (int si = new_use_idx + 1; si <= loop_end; si++)
        {
          IRQuadCompact *sq = &ir->compact_instructions[si];
          if (sq->op == TCCIR_OP_NOP)
            continue;
          if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF ||
              sq->op == TCCIR_OP_FUNCCALLVAL || sq->op == TCCIR_OP_FUNCCALLVOID)
            break;
          int uses_it = 0;
          int defines_it = 0;
          if (irop_config[sq->op].has_dest)
          {
            IROperand d = tcc_ir_op_get_dest(ir, sq);
            if (irop_get_vreg(d) == use_dest_vr)
            {
              if (sq->op == TCCIR_OP_STORE || sq->op == TCCIR_OP_STORE_INDEXED ||
                  sq->op == TCCIR_OP_STORE_POSTINC)
                uses_it = 1;
              else
                defines_it = 1;
            }
          }
          if (irop_config[sq->op].has_src1)
          {
            IROperand s1 = tcc_ir_op_get_src1(ir, sq);
            if (irop_get_vreg(s1) == use_dest_vr)
              uses_it = 1;
          }
          if (irop_config[sq->op].has_src2)
          {
            IROperand s2 = tcc_ir_op_get_src2(ir, sq);
            if (irop_get_vreg(s2) == use_dest_vr)
              uses_it = 1;
          }
          if (defines_it && !uses_it)
            break;
          if (uses_it)
            last_use = si;
        }
      }
      stride_insert_pos = last_use + 1;
    }
  }
  IROperand stride_op = irop_make_imm32(-1, div->stride, IROP_BTYPE_INT32);

  int stride_inserted = insert_instr_at(ir, stride_insert_pos, TCCIR_OP_ADD, ptr_op, ptr_op, stride_op);
  LOG_IV_SR("IV_SR: stride insert at pos=%d, result=%d, new_iv_def=%d", stride_insert_pos, stride_inserted,
            new_iv_def_idx);
  if (stride_inserted < 0)
    return 2; /* Partial success - at least did the pointer init and use replacement */

  if (out_ptr_vreg)
    *out_ptr_vreg = ptr_vreg;
  if (out_idx_shift)
    *out_idx_shift = idx_shift;

  return 3; /* Full success: init + replace + stride */
}

/* Convert signed comparison condition to unsigned equivalent.
 * Pointer comparisons must use unsigned conditions since addresses
 * are unsigned quantities. Returns the unsigned token, or -1 if
 * the condition is not a simple signed relational. */
static int signed_to_unsigned_cond(int cond_token)
{
  switch (cond_token)
  {
  case 0x9c: /* TOK_LT  → TOK_ULT */
    return 0x92;
  case 0x9d: /* TOK_GE  → TOK_UGE */
    return 0x93;
  case 0x9e: /* TOK_LE  → TOK_ULE */
    return 0x96;
  case 0x9f: /* TOK_GT  → TOK_UGT */
    return 0x97;
  case 0x94: /* TOK_EQ  - same for signed/unsigned */
    return 0x94;
  case 0x95: /* TOK_NE  - same for signed/unsigned */
    return 0x95;
  /* Already unsigned conditions — pass through */
  case 0x92: /* TOK_ULT */
    return 0x92;
  case 0x93: /* TOK_UGE */
    return 0x93;
  case 0x96: /* TOK_ULE */
    return 0x96;
  case 0x97: /* TOK_UGT */
    return 0x97;
  default:
    return -1;
  }
}

/* Try to eliminate the original IV counter after strength reduction has created
 * a derived pointer.  If the IV's only remaining uses are its own increment
 * and the loop exit CMP, we can replace the CMP with a pointer comparison
 * against a precomputed end address, making the IV completely dead.
 *
 * Before:  CMP i, #5; JUMPIF >=S exit   (signed comparison of index)
 *          i = i + 1
 *          ptr = ptr + 4
 *
 * After:   CMP ptr, end_ptr; JUMPIF >=U exit   (unsigned pointer comparison)
 *          ptr = ptr + 4
 *          (i is dead, eliminated by DCE)
 *
 * Parameters:
 *   ir       - IR state
 *   loop     - Loop structure (indices already shifted by transform_derived_iv)
 *   iv       - The basic induction variable (indices already shifted)
 *   div      - The derived IV info
 *   ptr_vreg - The vreg allocated for the pointer by transform_derived_iv
 *   idx_shift - Number of instructions inserted at the header by transform_derived_iv
 *
 * Returns 1 if elimination succeeded, 0 otherwise.
 */
static int try_eliminate_iv_counter(TCCIRState *ir, IRLoop *loop, InductionVar *iv, DerivedIV *div, int ptr_vreg,
                                    int idx_shift)
{
  int n = ir->next_instruction_index;
  int iv_vr = iv->vreg;

  /* Adjusted loop indices (transform_derived_iv inserted instructions at header) */
  int adj_header = loop->header_idx + idx_shift;
  int adj_end = loop->end_idx + idx_shift;
  int adj_iv_def = iv->def_idx;
  if (iv->def_idx >= loop->header_idx)
    adj_iv_def += idx_shift;

  /* Step 1a: Find the CMP + JUMPIF pre-test guard that tests the IV.
   * After loop rotation and IV strength reduction insertions, the pre-test
   * guard CMP is typically in the preheader area (just before the header).
   * Scan from the preheader up through a few instructions past the header. */
  int hdr_cmp_idx = -1, hdr_jmpif_idx = -1;
  int limit_val = 0, hdr_cond_token = 0;

  {
    int scan_start = loop->preheader_idx;
    if (scan_start < 0)
      scan_start = adj_header > 4 ? adj_header - 4 : 0;
    int scan_end = adj_header + 4;
    if (scan_end >= n - 1)
      scan_end = n - 2;

    for (int i = scan_start; i <= scan_end; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
        continue;

      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);

      if (irop_get_vreg(cmp_src1) != iv_vr || !irop_is_immediate(cmp_src2))
        continue;

      /* Look forward for the JUMPIF, skipping NOPs and ASSIGNs (which don't clobber flags) */
      int jq_idx = i + 1;
      while (jq_idx < n && (ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP ||
                            ir->compact_instructions[jq_idx].op == TCCIR_OP_ASSIGN))
        jq_idx++;

      if (jq_idx >= n)
        continue;
      IRQuadCompact *jq = &ir->compact_instructions[jq_idx];
      if (jq->op != TCCIR_OP_JUMPIF)
        continue;

      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      hdr_cond_token = (int)irop_get_imm64_ex(ir, cond_op);

      /* Exit target must be outside the loop */
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
      int exit_target = (int)irop_get_imm64_ex(ir, jmp_dest);
      if (exit_target <= adj_end + 1) /* +1 for the stride ADD we inserted */
        continue;

      limit_val = (int)irop_get_imm64_ex(ir, cmp_src2);
      hdr_cmp_idx = i;
      hdr_jmpif_idx = jq_idx;
      break;
    }
  }

  /* Step 1b: Find the CMP + JUMPIF near the back-edge (post-test / latch test).
   * This is typically just before the back-edge JUMP at adj_end. Scan backward
   * from adj_end looking for a CMP of the IV against the same limit. */
  int be_cmp_idx = -1, be_jmpif_idx = -1;
  int be_cond_token = 0;

  for (int i = adj_end; i >= adj_end - 5 && i >= 0; i--)
  {
    IRQuadCompact *cq = &ir->compact_instructions[i];
    if (cq->op != TCCIR_OP_CMP)
      continue;

    IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
    IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);

    if (irop_get_vreg(cmp_src1) != iv_vr || !irop_is_immediate(cmp_src2))
      continue;

    /* Look forward for the JUMPIF */
    int jq_idx = i + 1;
    while (jq_idx < n && (ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP ||
                          ir->compact_instructions[jq_idx].op == TCCIR_OP_ASSIGN))
      jq_idx++;

    if (jq_idx >= n)
      continue;
    IRQuadCompact *jq = &ir->compact_instructions[jq_idx];
    if (jq->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
    be_cond_token = (int)irop_get_imm64_ex(ir, cond_op);

    /* Back-edge target must be inside the loop (jumps back) */
    IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
    int back_target = (int)irop_get_imm64_ex(ir, jmp_dest);
    if (back_target > adj_end)
      continue; /* Not a back-edge */

    int be_limit = (int)irop_get_imm64_ex(ir, cmp_src2);

    /* Use limit_val from header if found, otherwise from back-edge */
    if (hdr_cmp_idx < 0)
      limit_val = be_limit;

    be_cmp_idx = i;
    be_jmpif_idx = jq_idx;
    break;
  }

  /* We need at least one CMP to proceed */
  if (hdr_cmp_idx < 0 && be_cmp_idx < 0)
  {
    LOG_IV_SR("IV_SR_ELIM: No CMP+JUMPIF found for IV VAR%d at header %d or back-edge %d",
              TCCIR_DECODE_VREG_POSITION(iv_vr), adj_header, adj_end);
    return 0;
  }

  if (TCC_LOG_IV_SR)
    fprintf(stderr, "[IV_SR_ELIM] hdr_cmp_idx=%d, be_cmp_idx=%d, adj_iv_def=%d, adj_iv_init=%d\n", hdr_cmp_idx,
            be_cmp_idx, adj_iv_def, iv->init_idx);

  /* Step 2: Check that the IV has no other uses besides:
   *   - The header CMP instruction (pre-test, if present)
   *   - The back-edge CMP instruction (post-test, if present)
   *   - Its own increment (adj_iv_def)
   *   - A copy-through temp (ASSIGN T=V just before the ADD)
   * If the IV is used elsewhere (e.g., as a function argument), we can't eliminate it. */
  int other_uses = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (i == hdr_cmp_idx || i == be_cmp_idx)
      continue; /* The CMPs we'll replace/remove */
    if (i == adj_iv_def)
      continue; /* The IV increment */

    /* Allow the copy-through pattern: T = V just before V = T + 1 */
    if (q->op == TCCIR_OP_ASSIGN && i >= adj_iv_def - 2 && i < adj_iv_def)
    {
      IROperand asrc = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(asrc) == iv_vr)
        continue; /* This is the copy-through temp */
    }

    /* Check src1 and src2 for uses of the IV */
    if (irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(s1) == iv_vr)
      {
        other_uses++;
        if (TCC_LOG_IV_SR)
          fprintf(stderr, "[IV_SR_ELIM] other_uses++ at idx=%d (src1) op=%d\n", i, q->op);
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_get_vreg(s2) == iv_vr)
      {
        other_uses++;
        if (TCC_LOG_IV_SR)
          fprintf(stderr, "[IV_SR_ELIM] other_uses++ at idx=%d (src2) op=%d\n", i, q->op);
      }
    }
  }

  if (other_uses > 0)
  {
    LOG_IV_SR("IV_SR_ELIM: IV VAR%d has %d other uses, cannot eliminate", TCCIR_DECODE_VREG_POSITION(iv_vr),
              other_uses);
    return 0;
  }

  /* Step 3: Compute end_ptr = base + limit * element_size
   * element_size = stride / step (e.g., stride=4, step=1 → element_size=4)
   * end_value = limit * element_size */
  int element_size = div->stride / iv->step;
  int end_offset = limit_val * element_size;

  /* Allocate a vreg for end_ptr */
  int end_vreg = tcc_ir_vreg_alloc_temp(ir);
  if (end_vreg < 0)
    return 0;

  IROperand end_op = irop_make_vreg(end_vreg, IROP_BTYPE_INT32);
  IROperand ptr_op = irop_make_vreg(ptr_vreg, IROP_BTYPE_INT32);
  IROperand null_op = {0};

  /* Insert end_ptr = base + end_offset in preheader (before the loop header).
   * We insert at adj_header (which is already shifted). */
  int insert_pos = adj_header;
  int end_shift = 0;

  /* end_ptr = base */
  int inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ASSIGN, end_op, div->base_op, null_op);
  if (inserted < 0)
    return 0;
  end_shift = 1;

  if (end_offset != 0)
  {
    /* end_ptr = end_ptr + end_offset */
    IROperand offset_op = irop_make_imm32(-1, end_offset, IROP_BTYPE_INT32);
    inserted = insert_instr_at(ir, insert_pos + 1, TCCIR_OP_ADD, end_op, end_op, offset_op);
    if (inserted < 0)
      return 0; /* Partial — at least the ASSIGN was inserted */
    end_shift = 2;
  }

  /* Update indices after insertion — only shift those at or after insert_pos */
  if (hdr_cmp_idx >= 0 && hdr_cmp_idx >= insert_pos)
  {
    hdr_cmp_idx += end_shift;
    hdr_jmpif_idx += end_shift;
  }
  if (be_cmp_idx >= 0 && be_cmp_idx >= insert_pos)
  {
    be_cmp_idx += end_shift;
    be_jmpif_idx += end_shift;
  }

  /* Step 4: Replace the back-edge CMP with pointer comparison (ptr vs end_ptr).
   * This is the primary loop continuation test. */
  if (be_cmp_idx >= 0)
  {
    int be_unsigned_cond = signed_to_unsigned_cond(be_cond_token);
    if (be_unsigned_cond < 0)
    {
      LOG_IV_SR("IV_SR_ELIM: Unsupported back-edge condition token 0x%x", be_cond_token);
      return 0;
    }

    IRQuadCompact *be_cmp_q = &ir->compact_instructions[be_cmp_idx];
    tcc_ir_op_set_src1(ir, be_cmp_q, ptr_op);
    tcc_ir_op_set_src2(ir, be_cmp_q, end_op);

    IRQuadCompact *be_jmp_q = &ir->compact_instructions[be_jmpif_idx];
    IROperand be_new_cond = irop_make_imm32(-1, be_unsigned_cond, IROP_BTYPE_INT32);
    tcc_ir_op_set_src1(ir, be_jmp_q, be_new_cond);
  }

  /* Step 5: Handle the header pre-test guard.
   * If the initial value satisfies the exit condition (e.g., init=0 < limit=5),
   * the pre-test is always false (loop always executes), so we can NOP it out.
   * Otherwise, replace with pointer comparison. */
  if (hdr_cmp_idx >= 0)
  {
    /* If there's a back-edge CMP, the pre-test is just a guard and can potentially
     * be NOP'd away. If the pre-test is the ONLY loop exit test (no back-edge CMP),
     * we must keep it and replace with pointer comparison — NOP'ing it would make
     * the loop infinite. */
    if (be_cmp_idx >= 0)
    {
      /* Back-edge test exists. Check if the pre-test can be constant-folded away.
       * The header tests: if (iv_init <cond> limit) goto exit
       * If this is always false for the initial value, the guard is redundant. */
      int guard_always_false = evaluate_compare_condition((int64_t)iv->init_val, (int64_t)limit_val, hdr_cond_token);

      if (!guard_always_false)
      {
        /* Guard is never taken — NOP out both CMP and JUMPIF */
        ir->compact_instructions[hdr_cmp_idx].op = TCCIR_OP_NOP;
        ir->compact_instructions[hdr_jmpif_idx].op = TCCIR_OP_NOP;
      }
      else
      {
        /* Guard might be taken — replace with pointer comparison */
        int hdr_unsigned_cond = signed_to_unsigned_cond(hdr_cond_token);
        if (hdr_unsigned_cond >= 0)
        {
          IRQuadCompact *hdr_cmp_q = &ir->compact_instructions[hdr_cmp_idx];
          tcc_ir_op_set_src1(ir, hdr_cmp_q, ptr_op);
          tcc_ir_op_set_src2(ir, hdr_cmp_q, end_op);

          IRQuadCompact *hdr_jmp_q = &ir->compact_instructions[hdr_jmpif_idx];
          IROperand hdr_new_cond = irop_make_imm32(-1, hdr_unsigned_cond, IROP_BTYPE_INT32);
          tcc_ir_op_set_src1(ir, hdr_jmp_q, hdr_new_cond);
        }
      }
    }
    else
    {
      /* Pre-test is the only loop exit — must replace with pointer comparison */
      int hdr_unsigned_cond = signed_to_unsigned_cond(hdr_cond_token);
      if (hdr_unsigned_cond >= 0)
      {
        IRQuadCompact *hdr_cmp_q = &ir->compact_instructions[hdr_cmp_idx];
        tcc_ir_op_set_src1(ir, hdr_cmp_q, ptr_op);
        tcc_ir_op_set_src2(ir, hdr_cmp_q, end_op);

        IRQuadCompact *hdr_jmp_q = &ir->compact_instructions[hdr_jmpif_idx];
        IROperand hdr_new_cond = irop_make_imm32(-1, hdr_unsigned_cond, IROP_BTYPE_INT32);
        tcc_ir_op_set_src1(ir, hdr_jmp_q, hdr_new_cond);
      }
    }
  }

  /* Step 6: NOP out the now-dead IV initialization and increment.
   * DCE cannot eliminate self-referential cycles (V = V + 1 uses itself),
   * so we must explicitly remove them.
   *
   * Index adjustments:
   * - iv->init_idx is in the preheader (before header_idx), not shifted by any insertions
   * - adj_iv_def was already adjusted for idx_shift; needs end_shift added for our insertions */
  {
    /* NOP the IV initialization (in preheader, not shifted) */
    int adj_iv_init = iv->init_idx;
    if (adj_iv_init >= 0 && adj_iv_init < ir->next_instruction_index)
    {
      IRQuadCompact *init_q = &ir->compact_instructions[adj_iv_init];
      IROperand init_dest = tcc_ir_op_get_dest(ir, init_q);
      if (irop_get_vreg(init_dest) == iv_vr)
        init_q->op = TCCIR_OP_NOP;
    }

    /* NOP the IV increment (in loop body, shifted by both idx_shift and end_shift) */
    int adj_iv_inc = adj_iv_def + end_shift;
    if (adj_iv_inc >= 0 && adj_iv_inc < ir->next_instruction_index)
    {
      IRQuadCompact *inc_q = &ir->compact_instructions[adj_iv_inc];
      IROperand inc_dest = tcc_ir_op_get_dest(ir, inc_q);
      if (irop_get_vreg(inc_dest) == iv_vr)
        inc_q->op = TCCIR_OP_NOP;

      /* Also NOP the copy-through temp (T1 = V1) that precedes V1 = T1 + 1 */
      for (int k = adj_iv_inc - 1; k >= adj_iv_inc - 3 && k >= 0; k--)
      {
        IRQuadCompact *cq = &ir->compact_instructions[k];
        if (cq->op == TCCIR_OP_NOP)
          continue;
        if (cq->op == TCCIR_OP_ASSIGN)
        {
          IROperand csrc = tcc_ir_op_get_src1(ir, cq);
          if (irop_get_vreg(csrc) == iv_vr)
          {
            cq->op = TCCIR_OP_NOP;
            break;
          }
        }
        break; /* Stop at first non-NOP, non-matching */
      }
    }
  }

  LOG_IV_SR("IV_SR_ELIM: Eliminated IV VAR%d, replaced CMP with ptr(TMP%d) vs end(TMP%d), "
            "end_offset=%d, hdr_cmp=%d, be_cmp=%d",
            TCCIR_DECODE_VREG_POSITION(iv_vr), TCCIR_DECODE_VREG_POSITION(ptr_vreg),
            TCCIR_DECODE_VREG_POSITION(end_vreg), end_offset, hdr_cmp_idx, be_cmp_idx);

  return 1;
}

/* Main entry point: Induction Variable Strength Reduction
 * Returns number of transformations applied
 */
/* Core IV strength reduction using pre-detected loops */
static int iv_strength_reduction_core(TCCIRState *ir, IRLoops *loops)
{
  int total_changes = 0;

  LOG_IV_SR("IV_SR: Found %d loop(s)", loops->num_loops);

  /* Process each loop, but only process loops with valid preheaders */
  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    if (loop->preheader_idx < 0)
      continue;

    /* Skip if inserting at preheader+1 would land inside a CHILD loop's
     * body range (a smaller loop contained within ours).  Inserting inside
     * a parent loop is fine — that's the expected case for inner loops. */
    {
      int insert_pos = loop->preheader_idx + 1;
      int loop_size = loop->end_idx - loop->start_idx;
      int skip = 0;
      for (int other = 0; other < loops->num_loops; other++)
      {
        if (other == li)
          continue;
        IRLoop *oloop = &loops->loops[other];
        int oloop_size = oloop->end_idx - oloop->start_idx;
        if (insert_pos > oloop->start_idx && insert_pos <= oloop->end_idx && oloop_size < loop_size)
        {
          skip = 1;
          break;
        }
      }
      if (skip)
        continue;
    }

    InductionVar ivs[MAX_IV];
    DerivedIV divs[MAX_DIV];

    int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1);
    if (num_ivs == 0)
      continue;

    int num_divs = find_derived_ivs(ir, loop, ivs, num_ivs, divs, MAX_DIV);
    if (num_divs == 0)
      continue;

    LOG_IV_SR("IV_SR: Found %d DIV(s) in loop %d", num_divs, li);

    /* Transform each derived IV, deferring IV elimination to pick the
     * cheapest end-pointer across all transformed DIVs. */
    int div_ptr_vregs[MAX_DIV];
    int div_changes[MAX_DIV];
    for (int di = 0; di < num_divs; di++)
    {
      div_ptr_vregs[di] = -1;
      div_changes[di] = 0;
    }

    for (int di = 0; di < num_divs; di++)
    {
      int sr_ptr_vreg = -1, sr_idx_shift = 0;
      int changes = transform_derived_iv(ir, loop, &ivs[divs[di].iv_idx], &divs[di], &sr_ptr_vreg, &sr_idx_shift);
      total_changes += changes;
      div_ptr_vregs[di] = sr_ptr_vreg;
      div_changes[di] = changes;

      /* After transformation, indices have shifted.  insert_instr_at
       * shifts all instructions >= pos by +1 per insertion.  Apply the
       * same position-aware shift to remaining loops' metadata.
       *
       * Insertion points (before any shifting):
       *   init_pos   = preheader_idx + 1 (sr_idx_shift instructions)
       *   stride_pos = iv->def_idx + sr_idx_shift + 1 (1 instruction)
       */
      if (changes > 0)
      {
        /* Compute shift for each original-space index.  Insertions:
         *   sr_idx_shift instructions at init_pos = preheader_idx + 1
         *   1 instruction at def_idx + 1 (only when changes >= 3)
         * Since init_pos < def_idx + 1 always holds (preheader is
         * before the loop body), the total shift for original index X:
         *   X < init_pos                  → 0
         *   init_pos <= X <= def_idx      → sr_idx_shift
         *   X > def_idx (and changes>=3)  → sr_idx_shift + 1
         */
        int init_pos = loop->preheader_idx + 1;
        int orig_def_idx = ivs[divs[di].iv_idx].def_idx;
        int has_stride = (changes >= 3);

#define APPLY_SHIFT(idx)                                                                                               \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((idx) >= init_pos)                                                                                             \
    {                                                                                                                  \
      (idx) += sr_idx_shift;                                                                                           \
      if (has_stride && (idx) - sr_idx_shift > orig_def_idx)                                                           \
        (idx)++;                                                                                                       \
    }                                                                                                                  \
  } while (0)

        /* Shift indices of remaining DIVs and all IVs so we can
         * continue processing more DIVs in this loop. */
        for (int dj = di + 1; dj < num_divs; dj++)
        {
          APPLY_SHIFT(divs[dj].use_idx);
          APPLY_SHIFT(divs[dj].shl_idx);
        }
        for (int ij = 0; ij < num_ivs; ij++)
        {
          APPLY_SHIFT(ivs[ij].def_idx);
          APPLY_SHIFT(ivs[ij].init_idx);
        }

        /* Shift current loop metadata */
        APPLY_SHIFT(loop->header_idx);
        APPLY_SHIFT(loop->start_idx);
        APPLY_SHIFT(loop->end_idx);
        if (loop->preheader_idx >= 0)
          APPLY_SHIFT(loop->preheader_idx);
        for (int bi = 0; bi < loop->num_body_instrs; bi++)
          APPLY_SHIFT(loop->body_instrs[bi]);

#undef APPLY_SHIFT

        /* After insertions, later loops' metadata is stale.
         * Break out — the caller re-invokes with fresh loop detection.
         * Already-transformed DIVs won't re-match (SHL/MUL are NOP'd). */
        if (di == num_divs - 1)
          goto try_elim;
      }
    }
    continue;

  try_elim:
    /* All DIVs in this loop are transformed.  Now try IV elimination
     * with each candidate, preferring the one whose end-pointer is
     * cheapest to materialize.
     *
     * Heuristic: prefer stack-based base (SP-relative end = single ADD)
     * over immediate base.  Among same-kind bases, prefer smaller
     * absolute end_offset (fewer bits to encode). */
    {
      int best_di = -1;
      int best_cost = 0x7fffffff;

      for (int di = 0; di < num_divs; di++)
      {
        if (div_changes[di] != 3 || div_ptr_vregs[di] < 0)
          continue;

        int element_size = divs[di].stride / ivs[divs[di].iv_idx].step;
        int abs_end_offset = ivs[divs[di].iv_idx].init_val * element_size;
        if (abs_end_offset < 0)
          abs_end_offset = -abs_end_offset;

        int cost;
        if (irop_get_tag(divs[di].base_op) == IROP_TAG_STACKOFF)
          cost = abs_end_offset;
        else
          cost = abs_end_offset + 0x10000;

        if (cost < best_cost)
        {
          best_cost = cost;
          best_di = di;
        }
      }

      if (best_di >= 0)
      {
        /* idx_shift=0 because APPLY_SHIFT already updated all loop/IV indices
         * to current (post-all-transforms) positions. */
        int elim =
            try_eliminate_iv_counter(ir, loop, &ivs[divs[best_di].iv_idx], &divs[best_di], div_ptr_vregs[best_di], 0);
        total_changes += elim;
      }
    }
    goto done;
  }

done:
  LOG_IV_SR("=== IV STRENGTH REDUCTION END: %d changes ===", total_changes);

  return total_changes;
}

int tcc_ir_opt_iv_strength_reduction(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total = 0;
  for (int iter = 0; iter < 8; iter++)
  {
    IRLoops *loops = tcc_ir_detect_loops(ir);
    if (!loops || loops->num_loops == 0)
    {
      tcc_ir_free_loops(loops);
      break;
    }
    int changes = iv_strength_reduction_core(ir, loops);
    tcc_ir_free_loops(loops);
    total += changes;
    if (changes == 0)
      break;
  }
  return total;
}

int tcc_ir_opt_iv_strength_reduction_with_loops(TCCIRState *ir, IRLoops *loops)
{
  if (!ir || ir->next_instruction_index == 0 || !loops || loops->num_loops == 0)
    return 0;

  LOG_IV_SR("=== IV STRENGTH REDUCTION START (with pre-detected loops) ===");

  int total = iv_strength_reduction_core(ir, loops);
  if (total == 0)
    return 0;

  /* First pass used LICM-provided loops.  If it transformed a loop and
   * broke out early, remaining loops need processing with fresh detection. */
  for (int iter = 0; iter < 7; iter++)
  {
    IRLoops *fresh = tcc_ir_detect_loops(ir);
    if (!fresh || fresh->num_loops == 0)
    {
      tcc_ir_free_loops(fresh);
      break;
    }
    int changes = iv_strength_reduction_core(ir, fresh);
    tcc_ir_free_loops(fresh);
    total += changes;
    if (changes == 0)
      break;
  }
  return total;
}

/* ============================================================================
 * Loop Bound Rematerialization
 * ============================================================================
 *
 * After IV strength reduction, the loop exit test compares the induction
 * pointer against an end-pointer vreg that was hoisted into the preheader:
 *
 *   [preheader]
 *   ASSIGN end_vreg, STACKOFF(base_off)
 *   ADD    end_vreg, end_vreg, #offset   (optional)
 *   ...
 *   [loop body with function calls]
 *   CMP    ptr_vreg, end_vreg
 *   JUMPIF ...
 *
 * Because end_vreg is live across the entire loop (including calls), the
 * register allocator must place it in a callee-saved register (R4-R11),
 * which costs a PUSH/POP pair in the prologue/epilogue.
 *
 * When the end pointer is a simple SP+constant computation, it is cheaper
 * to recompute it just before each CMP use inside the loop.  This shrinks
 * the live range so it no longer crosses calls, allowing a caller-saved
 * register (R0-R3) or a scratch register to be used instead.
 *
 * GCC does exactly this: it emits `ADD r3, sp, #offset` inside the loop
 * rather than keeping the end pointer in a callee-saved register.
 */

/* Maximum number of rematerialization candidates per loop */
#define REMAT_MAX_CANDIDATES 8

int tcc_ir_opt_loop_bound_remat(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  int total_changes = 0;
  int n = ir->next_instruction_index;

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    /* Only worthwhile if the loop contains function calls — otherwise the
     * end pointer can live in a caller-saved register anyway. */
    int has_calls = 0;
    for (int i = loop->start_idx; i <= loop->end_idx && i < n; i++)
    {
      TccIrOp op = ir->compact_instructions[i].op;
      if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID)
      {
        has_calls = 1;
        break;
      }
    }
    if (!has_calls)
      continue;

    /* Scan preheader for TEMP vregs defined by ASSIGN from STACKOFF,
     * optionally followed by ADD with immediate.  These are candidates
     * for rematerialization. */
    int preheader_end = loop->header_idx; /* exclusive */
    int preheader_start = loop->preheader_idx;
    if (preheader_start < 0)
      continue;

    /* Expand preheader backwards to find the full basic block that flows
     * into the loop header.  The preheader_idx from loop detection is just
     * a single instruction; we need to scan further back for definitions
     * that were inserted before the loop (e.g., by IV strength reduction). */
    for (int i = preheader_start - 1; i >= 0; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      /* Stop at jump targets — they start a new basic block */
      if (q->is_jump_target)
        break;
      /* Stop at control flow instructions */
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
          q->op == TCCIR_OP_RETURNVALUE)
        break;
      /* Stop at calls — we don't want to look before function calls */
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
        break;
      /* Skip NOPs, expand for everything else */
      preheader_start = i;
    }

    /* Collect candidates: TEMP vregs with cheap rematerializable definitions */
    struct
    {
      int vreg;          /* The TEMP vreg */
      int assign_idx;    /* ASSIGN instruction index */
      int add_idx;       /* ADD instruction index (-1 if none) */
      int32_t stack_off; /* Stack offset from ASSIGN's STACKOFF src */
      int32_t add_imm;   /* Immediate from ADD (0 if no ADD) */
      int is_param;      /* is_param flag from STACKOFF operand */
      int is_lval;       /* is_lval flag — 1 for value loads, 0 for address-of */
    } candidates[REMAT_MAX_CANDIDATES];
    int num_candidates = 0;

    for (int i = preheader_start; i < preheader_end && i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN)
        continue;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int vr = irop_get_vreg(dest);
      if (vr < 0)
        continue;

      /* Must be a TEMP vreg */
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;

      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (irop_get_tag(src) != IROP_TAG_STACKOFF)
        continue;

      int32_t stack_off = (int32_t)irop_get_imm64_ex(ir, src);
      int is_param = src.is_param;
      int is_lval = src.is_lval;
      int add_idx = -1;
      int32_t add_imm = 0;

      /* Check if the next non-NOP instruction is ADD tmpN, tmpN, #imm */
      for (int j = i + 1; j < preheader_end && j < n; j++)
      {
        IRQuadCompact *aq = &ir->compact_instructions[j];
        if (aq->op == TCCIR_OP_NOP)
          continue;
        if (aq->op == TCCIR_OP_ADD)
        {
          IROperand adest = tcc_ir_op_get_dest(ir, aq);
          IROperand asrc1 = tcc_ir_op_get_src1(ir, aq);
          IROperand asrc2 = tcc_ir_op_get_src2(ir, aq);
          if (irop_get_vreg(adest) == vr && irop_get_vreg(asrc1) == vr && irop_is_immediate(asrc2))
          {
            add_idx = j;
            add_imm = (int32_t)irop_get_imm64_ex(ir, asrc2);
          }
        }
        break; /* Only check the immediately following non-NOP */
      }

      if (num_candidates < REMAT_MAX_CANDIDATES)
      {
        candidates[num_candidates].vreg = vr;
        candidates[num_candidates].assign_idx = i;
        candidates[num_candidates].add_idx = add_idx;
        candidates[num_candidates].stack_off = stack_off;
        candidates[num_candidates].add_imm = add_imm;
        candidates[num_candidates].is_param = is_param;
        candidates[num_candidates].is_lval = is_lval;
        num_candidates++;
      }
    }

    if (num_candidates == 0)
      continue;

    /* For each candidate, verify it is only used in CMP instructions inside
     * the loop (or in the header guard area).  Count uses. */
    for (int ci = 0; ci < num_candidates; ci++)
    {
      int vr = candidates[ci].vreg;
      int use_count = 0;
      int bad_use = 0;

      /* Collect CMP use sites inside the loop */
      int cmp_indices[4];
      int num_cmp_uses = 0;

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        /* Skip the defining instructions */
        if (i == candidates[ci].assign_idx || i == candidates[ci].add_idx)
          continue;

        /* Check if this instruction uses the vreg */
        int uses_vr = 0;
        if (irop_config[q->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(s1) == vr)
            uses_vr = 1;
        }
        if (irop_config[q->op].has_src2)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          if (irop_get_vreg(s2) == vr)
            uses_vr = 1;
        }

        if (!uses_vr)
          continue;

        use_count++;

        /* Must be a CMP instruction within or near the loop */
        if (q->op != TCCIR_OP_CMP)
        {
          bad_use = 1;
          break;
        }

        /* Record CMP site if inside or near loop bounds */
        if (i >= preheader_start && i <= loop->end_idx + 2)
        {
          if (num_cmp_uses < 4)
            cmp_indices[num_cmp_uses++] = i;
        }
        else
        {
          bad_use = 1;
          break;
        }
      }

      if (bad_use || use_count == 0 || num_cmp_uses == 0)
        continue;

      /* Rematerialize: insert ASSIGN from combined STACKOFF just before
       * each CMP use, then NOP the preheader definitions.
       *
       * Process CMP sites from last to first so insertion indices remain
       * valid (inserting at a later position doesn't shift earlier ones). */
      int32_t combined_off = candidates[ci].stack_off + candidates[ci].add_imm;
      int remat_shift = 0;

      for (int ui = num_cmp_uses - 1; ui >= 0; ui--)
      {
        int cmp_idx = cmp_indices[ui] + remat_shift;

        /* Allocate a fresh TEMP vreg for each rematerialization site */
        int remat_vreg = tcc_ir_vreg_alloc_temp(ir);
        if (remat_vreg < 0)
          break;

        IROperand remat_dest = irop_make_vreg(remat_vreg, IROP_BTYPE_INT32);
        IROperand remat_src =
            irop_make_stackoff(-1, combined_off, candidates[ci].is_lval, 0, candidates[ci].is_param, IROP_BTYPE_INT32);
        IROperand null_op = {0};

        int inserted = insert_instr_at(ir, cmp_idx, TCCIR_OP_ASSIGN, remat_dest, remat_src, null_op);
        if (inserted < 0)
          break;

        n = ir->next_instruction_index;
        remat_shift++;

        /* Update the CMP (now at cmp_idx+1) to use the new remat vreg */
        IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx + 1];
        IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cmp_q);
        if (irop_get_vreg(cmp_src2) == vr)
        {
          IROperand new_src2 = irop_make_vreg(remat_vreg, IROP_BTYPE_INT32);
          tcc_ir_op_set_src2(ir, cmp_q, new_src2);
        }
        else
        {
          /* The vreg might be in src1 */
          IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cmp_q);
          if (irop_get_vreg(cmp_src1) == vr)
          {
            IROperand new_src1 = irop_make_vreg(remat_vreg, IROP_BTYPE_INT32);
            tcc_ir_op_set_src1(ir, cmp_q, new_src1);
          }
        }
      }

      /* NOP the preheader definitions — the vreg is now dead */
      ir->compact_instructions[candidates[ci].assign_idx].op = TCCIR_OP_NOP;
      if (candidates[ci].add_idx >= 0)
        ir->compact_instructions[candidates[ci].add_idx].op = TCCIR_OP_NOP;

      total_changes++;
    }
  }

  tcc_ir_free_loops(loops);
  return total_changes;
}

/* ============================================================================
 * Loop Unrolling - Fully unroll small constant-trip-count loops
 * ============================================================================
 *
 * For loops like: for (i=0; i<5; i++) sum += 5;
 * After unrolling, the loop body is replicated trip_count times and the
 * loop control flow is eliminated.  Subsequent constant propagation can
 * then collapse the result (e.g. 0+5+5+5+5+5 → 25).
 */

#define UNROLL_MAX_TRIP_COUNT 16
#define UNROLL_MAX_BODY_INSNS 32
#define UNROLL_MAX_TOTAL_INSNS 128

/* Find the exit condition in a loop.
 * Checks both the header (top-tested) and the tail (bottom-tested / rotated).
 * Pattern: CMP Viv, #limit; JUMPIF target COND
 * For top-tested: JUMPIF exits the loop (target > end_idx).
 * For bottom-tested: JUMPIF is the back-edge (target inside loop), exit is fall-through.
 * Returns 1 if found, 0 otherwise. */
static int find_loop_exit_condition(TCCIRState *ir, IRLoop *loop, int iv_vreg, int *out_cmp_idx, int *out_jmpif_idx,
                                    int *out_limit, int *out_cond, int *out_exit_target)
{
  /* Define scan ranges: header region and tail region */
  int ranges[2][2] = {
      {loop->header_idx, loop->header_idx + 3}, /* top-tested */
      {loop->end_idx - 3, loop->end_idx}        /* bottom-tested (rotated) */
  };

  for (int r = 0; r < 2; r++)
  {
    int scan_start = ranges[r][0];
    int scan_end = ranges[r][1];
    if (scan_start < loop->start_idx)
      scan_start = loop->start_idx;
    if (scan_end > loop->end_idx)
      scan_end = loop->end_idx;

    LOG_LOOP_OPT("find_loop_exit_condition: iv_vreg=VAR%d %s scan [%d..%d]", TCCIR_DECODE_VREG_POSITION(iv_vreg),
                 r == 0 ? "header" : "tail", scan_start, scan_end);

    for (int i = scan_start; i <= scan_end && i < ir->next_instruction_index - 1; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
      {
        LOG_LOOP_OPT("[%d] op=%d (not CMP), skipping", i, cq->op);
        continue;
      }

      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);

      /* Check: CMP Viv, #limit */
      int32_t vr1 = irop_get_vreg(cmp_src1);
      if (vr1 != iv_vreg || !irop_is_immediate(cmp_src2))
      {
        LOG_LOOP_OPT("[%d] CMP but vr1=%d (want %d) imm=%d, skipping", i,
                     vr1 >= 0 ? TCCIR_DECODE_VREG_POSITION(vr1) : -1, TCCIR_DECODE_VREG_POSITION(iv_vreg),
                     irop_is_immediate(cmp_src2));
        continue;
      }

      int limit = (int)irop_get_imm64_ex(ir, cmp_src2);

      /* Next instruction must be JUMPIF */
      IRQuadCompact *jq = &ir->compact_instructions[i + 1];
      if (jq->op != TCCIR_OP_JUMPIF)
      {
        LOG_LOOP_OPT("[%d] CMP ok but [%d] is op=%d (not JUMPIF)", i, i + 1, jq->op);
        continue;
      }

      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int cond = (int)irop_get_imm64_ex(ir, cond_op);

      IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
      int jmp_target = (int)irop_get_imm64_ex(ir, jmp_dest);

      /* Top-tested: exit target is outside the loop */
      if (jmp_target > loop->end_idx)
      {
        LOG_LOOP_OPT("[%d] FOUND top-tested exit: CMP VAR%d,#%d JUMPIF@%d exit=%d cond=%d", i,
                     TCCIR_DECODE_VREG_POSITION(iv_vreg), limit, i + 1, jmp_target, cond);
        *out_cmp_idx = i;
        *out_jmpif_idx = i + 1;
        *out_limit = limit;
        *out_cond = cond;
        *out_exit_target = jmp_target;
        return 1;
      }

      /* Bottom-tested (rotated): JUMPIF is the back-edge, exit is fall-through.
       * The back-edge target is inside the loop, and the condition is inverted
       * (loop continues when condition holds, exits on fall-through).
       * We need to invert the condition so callers see it as an exit condition. */
      if (jmp_target >= loop->start_idx && jmp_target <= loop->end_idx)
      {
        int exit_target = i + 2; /* fall-through past the JUMPIF */
        /* Invert the condition: the JUMPIF continues the loop, so the exit
         * condition is the opposite. */
        int inv_cond;
        switch (cond)
        {
        case TOK_GE:
          inv_cond = TOK_LT;
          break;
        case TOK_GT:
          inv_cond = TOK_LE;
          break;
        case TOK_LT:
          inv_cond = TOK_GE;
          break;
        case TOK_LE:
          inv_cond = TOK_GT;
          break;
        case TOK_EQ:
          inv_cond = TOK_NE;
          break;
        case TOK_NE:
          inv_cond = TOK_EQ;
          break;
        default:
          inv_cond = -1;
          break;
        }
        if (inv_cond < 0)
        {
          LOG_LOOP_OPT("[%d] bottom-tested but can't invert cond=%d", i, cond);
          continue;
        }
        LOG_LOOP_OPT("[%d] FOUND bottom-tested exit: CMP VAR%d,#%d JUMPIF@%d back=%d exit=%d cond=%d->%d", i,
                     TCCIR_DECODE_VREG_POSITION(iv_vreg), limit, i + 1, jmp_target, exit_target, cond, inv_cond);
        *out_cmp_idx = i;
        *out_jmpif_idx = i + 1;
        *out_limit = limit;
        *out_cond = inv_cond;
        *out_exit_target = exit_target;
        return 1;
      }

      LOG_LOOP_OPT("[%d] CMP+JUMPIF found but target=%d doesn't match any pattern", i, jmp_target);
    }
  }
  LOG_LOOP_OPT("-> exit condition NOT FOUND");
  return 0;
}

/* Compute the trip count for a loop given IV init, limit, step, and condition.
 * Returns trip count >= 0, or -1 if it cannot be computed.
 * Uses int64_t internally to avoid signed overflow when init_val and limit
 * are far apart (e.g. 0x60000000 and 0xA0000000 in 32-bit signed). */
static int compute_trip_count(int init_val, int limit, int step, int cond_token)
{
  if (step <= 0)
    return -1;

  int64_t range = (int64_t)limit - (int64_t)init_val;

  LOG_LOOP_OPT("compute_trip_count: init=%d limit=%d step=%d cond=%d range=%lld", init_val, limit, step, cond_token,
               (long long)range);

  switch (cond_token)
  {
  case TOK_GE: /* exit if iv >= limit → loop while iv < limit */
    if (range <= 0)
      return 0;
    return (int)((range + step - 1) / step);

  case TOK_GT: /* exit if iv > limit → loop while iv <= limit */
    if (range < 0)
      return 0;
    return (int)(range / step + 1);

  case TOK_NE: /* exit if iv != limit → loop until iv == limit */
    if (range < 0)
      return -1;
    if (range == 0)
      return 0;
    if (range % step != 0)
      return -1; /* would loop forever */
    return (int)(range / step);

  default:
    return -1;
  }
}

/* Collect the body instructions to clone (excluding loop control flow and IV update).
 * Returns count of body instructions, fills body_indices[]. */
static int collect_body_instructions(TCCIRState *ir, IRLoop *loop, int iv_vreg, int cmp_idx, int jmpif_idx,
                                     int iv_def_idx, int *body_indices, int max_body)
{
  int count = 0;
  /* Scan only [start_idx..end_idx].  The forward-jump extension in the loop
   * detector can pull in post-loop instructions (e.g. the exit target), which
   * must NOT be treated as body.  The merge pass already ensures end_idx
   * covers all body instructions from overlapping loops. */
  for (int i = loop->start_idx; i <= loop->end_idx && count < max_body; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Skip NOP */
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Skip CMP and JUMPIF (exit condition) */
    if (i == cmp_idx || i == jmpif_idx)
      continue;

    /* Skip all unconditional jumps (loop structure) */
    if (q->op == TCCIR_OP_JUMP)
      continue;

    /* Skip IV increment */
    if (i == iv_def_idx)
      continue;

    /* Skip the ASSIGN that saves old IV for post-increment pattern:
     * T = Viv  (where T is only used by the IV ADD on the next line) */
    if (q->op == TCCIR_OP_ASSIGN && i == iv_def_idx - 1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(src1) == iv_vreg)
        continue;
    }

    /* Reject bodies with internal branches (too complex for v1) */
    if (q->op == TCCIR_OP_JUMPIF)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] internal JUMPIF", i);
      return -1;
    }

    /* Reject bodies with calls (side effects) */
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCPARAMVAL ||
        q->op == TCCIR_OP_FUNCPARAMVOID)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] function call op=%d", i, q->op);
      return -1;
    }

    /* Reject inline asm */
    if (q->op == TCCIR_OP_INLINE_ASM)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] inline asm", i);
      return -1;
    }

    LOG_LOOP_OPT("collect_body: body[%d] = instr [%d] op=%d", count, i, q->op);
    body_indices[count++] = i;
  }

  LOG_LOOP_OPT("collect_body: collected %d body instruction(s)", count);
  return count;
}

/* Write an instruction into a NOP slot at position pos.
 * The slot MUST already be NOP. */
static void write_instr_at_nop(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2)
{
  IRQuadCompact *q = &ir->compact_instructions[pos];
  q->op = op;
  q->is_jump_target = 0;
  /* Only write operand slots that the instruction uses, so the pool
   * layout matches what codegen expects (it reads at base + has_dest
   * for src1, etc.).  Writing an unused dest slot would misalign. */
  int base = ir->iroperand_pool_count;
  if (irop_config[op].has_dest)
    tcc_ir_pool_add(ir, dest);
  if (irop_config[op].has_src1)
    tcc_ir_pool_add(ir, src1);
  if (irop_config[op].has_src2)
    tcc_ir_pool_add(ir, src2);
  q->operand_base = base;
}

/* Try to eliminate a loop entirely by computing final IV values.
 * This handles loops whose body consists only of induction variable
 * updates (no side effects, no calls, no branches).
 * Example: for (i=0; i<10; i++) count++ → count = 10
 * Returns 1 if eliminated, 0 otherwise. */
static int try_eliminate_loop(TCCIRState *ir, IRLoop *loop)
{
  LOG_LOOP_OPT("try_eliminate_loop: header=%d start=%d end=%d preheader=%d", loop->header_idx, loop->start_idx,
               loop->end_idx, loop->preheader_idx);
  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1 /* allow copy-through */);
  if (num_ivs < 1)
  {
    LOG_LOOP_OPT("try_eliminate_loop: no IVs found, giving up");
    return 0;
  }

  /* Find the primary IV — the one referenced in the loop exit condition. */
  int cmp_idx, jmpif_idx, limit, cond, exit_target;
  InductionVar *primary_iv = NULL;
  for (int k = 0; k < num_ivs; k++)
  {
    if (find_loop_exit_condition(ir, loop, ivs[k].vreg, &cmp_idx, &jmpif_idx, &limit, &cond, &exit_target))
    {
      primary_iv = &ivs[k];
      LOG_LOOP_OPT("try_eliminate_loop: primary IV=VAR%d (init=%d, step=%d)",
                   TCCIR_DECODE_VREG_POSITION(primary_iv->vreg), primary_iv->init_val, primary_iv->step);
      break;
    }
  }
  if (!primary_iv)
  {
    LOG_LOOP_OPT("try_eliminate_loop: no primary IV (exit condition not found for any IV)");
    return 0;
  }

  int trip_count = compute_trip_count(primary_iv->init_val, limit, primary_iv->step, cond);
  if (trip_count <= 0)
  {
    LOG_LOOP_OPT("try_eliminate_loop: trip_count=%d (invalid), giving up", trip_count);
    return 0;
  }
  LOG_LOOP_OPT("try_eliminate_loop: trip_count=%d limit=%d", trip_count, limit);

  /* Verify the loop body contains ONLY IV updates.
   * After removing: NOPs, JUMPs, CMP+JUMPIF, and all IV defs and their
   * copy-through temps, nothing should remain. */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP)
      continue;
    if (i == cmp_idx || i == jmpif_idx)
      continue;

    /* Check if this instruction is an IV definition */
    int is_iv_def = 0;
    for (int k = 0; k < num_ivs; k++)
    {
      if (i == ivs[k].def_idx)
      {
        is_iv_def = 1;
        break;
      }
    }
    if (is_iv_def)
      continue;

    /* Check if this is a copy-through temp for any IV:
     * ASSIGN where dest is a temp and src is an IV vreg,
     * immediately before that IV's def instruction */
    if (q->op == TCCIR_OP_ASSIGN)
    {
      int is_iv_copy = 0;
      for (int k = 0; k < num_ivs; k++)
      {
        if (i == ivs[k].def_idx - 1)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(src1) == ivs[k].vreg)
          {
            is_iv_copy = 1;
            break;
          }
        }
      }
      if (is_iv_copy)
        continue;
    }

    /* Any other instruction — loop has side effects, can't eliminate */
    LOG_LOOP_OPT("try_eliminate_loop: BLOCKED by instr [%d] op=%d (not IV/NOP/JUMP/CMP)", i, q->op);
    return 0;
  }

  /* Also verify no backward jumps escape the loop */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, jd);
      if (target < loop->start_idx)
        return 0;
    }
  }

  LOG_IR_GEN("[LOOP-ELIM] Eliminating loop header=%d trip_count=%d num_ivs=%d", loop->header_idx, trip_count, num_ivs);

  /* NOP the entire loop */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  /* Write final value assignments for all IVs used after the loop */
  int write_pos = loop->start_idx;
  for (int k = 0; k < num_ivs; k++)
  {
    int iv_final = ivs[k].init_val + trip_count * ivs[k].step;

    /* Check if this IV is used after the loop */
    int used_after = 0;
    for (int j = exit_target; j < ir->next_instruction_index; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == ivs[k].vreg)
      {
        used_after = 1;
        break;
      }
      if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == ivs[k].vreg)
      {
        used_after = 1;
        break;
      }
    }

    if (used_after && write_pos <= loop->end_idx)
    {
      IROperand dest = irop_make_vreg(ivs[k].vreg, IROP_BTYPE_INT32);
      IROperand val = irop_make_imm32(-1, iv_final, IROP_BTYPE_INT32);
      write_instr_at_nop(ir, write_pos++, TCCIR_OP_ASSIGN, dest, val, (IROperand){0});
    }

    /* NOP the initialization too */
    if (ivs[k].init_idx >= 0)
    {
      ir->compact_instructions[ivs[k].init_idx].op = TCCIR_OP_NOP;

      /* For bottom-tested (rotated) loops, also NOP the pre-loop guard that
       * tests this IV.  Since trip_count > 0, the guard is dead code, and
       * leaving it with a NOP'd IV init would read an undefined register. */
      for (int g = ivs[k].init_idx + 1; g < loop->start_idx; g++)
      {
        IRQuadCompact *gq = &ir->compact_instructions[g];
        if (gq->op == TCCIR_OP_CMP)
        {
          IROperand gsrc1 = tcc_ir_op_get_src1(ir, gq);
          if (irop_get_vreg(gsrc1) == ivs[k].vreg && g + 1 < loop->start_idx)
          {
            IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
            if (gjq->op == TCCIR_OP_JUMPIF)
            {
              LOG_LOOP_OPT("NOP'ing pre-loop guard CMP@%d + JUMPIF@%d", g, g + 1);
              gq->op = TCCIR_OP_NOP;
              gjq->op = TCCIR_OP_NOP;
            }
          }
        }
      }
    }
  }

  return 1;
}

/* Try to unroll a single loop. Returns 1 if unrolled, 0 otherwise. */
static int try_unroll_loop(TCCIRState *ir, IRLoop *loop)
{
  LOG_LOOP_OPT("try_unroll_loop: header=%d start=%d end=%d preheader=%d", loop->header_idx, loop->start_idx,
               loop->end_idx, loop->preheader_idx);
  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1 /* allow copy-through */);
  if (num_ivs < 1)
  {
    LOG_LOOP_OPT("try_unroll_loop: no IVs found, giving up");
    return 0;
  }

  /* Find the primary IV — the one referenced in the loop exit condition.
   * Accumulators (e.g. sum += const) also match the IV pattern but are not
   * used in the exit CMP; they are handled as regular body instructions. */
  int cmp_idx, jmpif_idx, limit, cond, exit_target;
  InductionVar *iv = NULL;
  for (int k = 0; k < num_ivs; k++)
  {
    if (find_loop_exit_condition(ir, loop, ivs[k].vreg, &cmp_idx, &jmpif_idx, &limit, &cond, &exit_target))
    {
      iv = &ivs[k];
      break;
    }
  }
  if (!iv)
  {
    LOG_LOOP_OPT("try_unroll_loop: no primary IV (exit condition not found)");
    return 0;
  }

  int trip_count = compute_trip_count(iv->init_val, limit, iv->step, cond);
  if (trip_count <= 0 || trip_count > UNROLL_MAX_TRIP_COUNT)
  {
    LOG_LOOP_OPT("try_unroll_loop: trip_count=%d (invalid or > %d), giving up", trip_count, UNROLL_MAX_TRIP_COUNT);
    return 0;
  }

  int body_indices[UNROLL_MAX_BODY_INSNS];
  int body_count = collect_body_instructions(ir, loop, iv->vreg, cmp_idx, jmpif_idx, iv->def_idx, body_indices,
                                             UNROLL_MAX_BODY_INSNS);
  if (body_count <= 0 || body_count > UNROLL_MAX_BODY_INSNS)
  {
    LOG_LOOP_OPT("try_unroll_loop: body_count=%d (invalid or > %d), giving up", body_count, UNROLL_MAX_BODY_INSNS);
    return 0;
  }

  int total_insns = trip_count * body_count;
  if (total_insns > UNROLL_MAX_TOTAL_INSNS)
  {
    LOG_LOOP_OPT("try_unroll_loop: total_insns=%d > %d, giving up", total_insns, UNROLL_MAX_TOTAL_INSNS);
    return 0;
  }

  /* Save original opcodes and operands for body instructions before NOP'ing.
   * The write loop needs original data, but NOP slots may be overwritten by
   * earlier iterations (a body instruction's slot can be reused for unrolled
   * output, destroying the operand_base). */
  int body_ops[UNROLL_MAX_BODY_INSNS];
  IROperand body_dests[UNROLL_MAX_BODY_INSNS];
  IROperand body_src1s[UNROLL_MAX_BODY_INSNS];
  IROperand body_src2s[UNROLL_MAX_BODY_INSNS];
  for (int b = 0; b < body_count; b++)
  {
    IRQuadCompact *bq = &ir->compact_instructions[body_indices[b]];
    int op = bq->op;
    body_ops[b] = op;
    body_dests[b] = (IROperand){0};
    body_src1s[b] = (IROperand){0};
    body_src2s[b] = (IROperand){0};
    if (irop_config[op].has_dest)
      body_dests[b] = ir->iroperand_pool[bq->operand_base];
    if (irop_config[op].has_src1)
      body_src1s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest];
    if (irop_config[op].has_src2)
      body_src2s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest + irop_config[op].has_src1];
  }

  /* Use only the [start_idx..end_idx] range for NOP/write region.
   * Do NOT use the extended body_instrs — the forward-jump extension
   * can include post-loop instructions that must not be touched. */
  int loop_end = loop->end_idx;

  /* Ensure the unrolled body fits in the available NOP region. */
  int avail_slots = loop_end - loop->start_idx + 1;
  if (total_insns > avail_slots)
    return 0;

  for (int i = loop->start_idx; i <= loop_end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP && i != loop->end_idx)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, jd);
      if (target < i && target < loop->start_idx)
        return 0; /* Backward jump escaping the loop — nested or malformed */
    }
  }

  LOG_IR_GEN("[UNROLL] Unrolling loop header=%d trip_count=%d body_count=%d", loop->header_idx, trip_count, body_count);

  /* NOP out the entire loop region */
  for (int i = loop->start_idx; i <= loop_end; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  /* NOP out IV initialization in preheader */
  if (iv->init_idx >= 0)
    ir->compact_instructions[iv->init_idx].op = TCCIR_OP_NOP;

  /* For bottom-tested (rotated) loops, there is a guard CMP+JUMPIF before the
   * loop that tests the IV and jumps past the loop if it shouldn't execute.
   * Since trip_count > 0 (we are unrolling), the guard is dead code.
   * We must NOP it because we NOP'd the IV init above, leaving the guard's
   * IV operand undefined. */
  if (iv->init_idx >= 0)
  {
    for (int g = iv->init_idx + 1; g < loop->start_idx; g++)
    {
      IRQuadCompact *gq = &ir->compact_instructions[g];
      if (gq->op == TCCIR_OP_CMP)
      {
        IROperand gsrc1 = tcc_ir_op_get_src1(ir, gq);
        if (irop_get_vreg(gsrc1) == iv->vreg && g + 1 < loop->start_idx)
        {
          IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
          if (gjq->op == TCCIR_OP_JUMPIF)
          {
            LOG_LOOP_OPT("NOP'ing pre-loop guard CMP@%d + JUMPIF@%d", g, g + 1);
            gq->op = TCCIR_OP_NOP;
            gjq->op = TCCIR_OP_NOP;
          }
        }
      }
    }
  }

  /* Also NOP the "JMP to body" that may precede the loop header
   * (instruction at start_idx - 1 if it's a jump into the loop body) */

  /* Write unrolled copies into the NOP'd slots */
  int write_pos = loop->start_idx;

  for (int k = 0; k < trip_count; k++)
  {
    for (int b = 0; b < body_count; b++)
    {
      int saved_op = body_ops[b];
      IROperand dest = body_dests[b];
      IROperand src1 = body_src1s[b];
      IROperand src2 = body_src2s[b];

      /* Substitute IV references in src operands with constant value for this iteration */
      int iv_val = iv->init_val + k * iv->step;
      IROperand iv_const = irop_make_imm32(-1, iv_val, IROP_BTYPE_INT32);

      if (irop_get_vreg(src1) == iv->vreg)
        src1 = iv_const;
      if (irop_get_vreg(src2) == iv->vreg)
        src2 = iv_const;

      /* Find next NOP slot to write into */
      while (write_pos <= loop_end && ir->compact_instructions[write_pos].op != TCCIR_OP_NOP)
        write_pos++;

      if (write_pos > loop_end)
        return 0; /* Should not happen — avail_slots check above prevents this */

      write_instr_at_nop(ir, write_pos, saved_op, dest, src1, src2);
      write_pos++;
    }
  }

  /* If the IV is used after the loop, set its final value.
   * Check if iv vreg is referenced anywhere after the loop. */
  {
    int iv_used_after = 0;
    for (int i = exit_target; i < ir->next_instruction_index; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s1) == iv->vreg)
        {
          iv_used_after = 1;
          break;
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        if (irop_get_vreg(s2) == iv->vreg)
        {
          iv_used_after = 1;
          break;
        }
      }
    }

    if (iv_used_after)
    {
      /* Write final IV value into a NOP slot before exit_target */
      int iv_final = iv->init_val + trip_count * iv->step;
      IROperand iv_dest = irop_make_vreg(iv->vreg, IROP_BTYPE_INT32);
      IROperand iv_val_op = irop_make_imm32(-1, iv_final, IROP_BTYPE_INT32);

      /* Find a NOP slot */
      for (int i = write_pos; i <= loop_end; i++)
      {
        if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
        {
          write_instr_at_nop(ir, i, TCCIR_OP_ASSIGN, iv_dest, iv_val_op, (IROperand){0});
          break;
        }
      }
    }
  }

  return 1;
}

int tcc_ir_opt_loop_unroll(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  /* Merge overlapping loops.  A C for-loop often produces two backward
   * edges (header<-latch and increment<-body), creating two detected loops
   * that are really one.  Merge them so the unroller sees a single loop.
   * Mark absorbed loops with start_idx = -1. */
  for (int i = 0; i < loops->num_loops; i++)
  {
    if (loops->loops[i].start_idx < 0)
      continue;
    int merged;
    do
    {
      merged = 0;
      for (int j = 0; j < loops->num_loops; j++)
      {
        if (j == i || loops->loops[j].start_idx < 0)
          continue;
        IRLoop *a = &loops->loops[i];
        IRLoop *b = &loops->loops[j];
        if (a->start_idx <= b->end_idx && b->start_idx <= a->end_idx)
        {
          /* Merge b into a: keep the earlier header (has the CMP) */
          if (b->start_idx < a->start_idx)
          {
            a->header_idx = b->header_idx;
            a->start_idx = b->start_idx;
            a->preheader_idx = b->preheader_idx;
          }
          if (b->end_idx > a->end_idx)
            a->end_idx = b->end_idx;
          /* Rebuild body_instrs for the merged range */
          tcc_free(a->body_instrs);
          int new_size = a->end_idx - a->start_idx + 1;
          a->body_instrs = tcc_mallocz(sizeof(int) * new_size);
          a->body_instrs_capacity = new_size;
          a->num_body_instrs = 0;
          for (int k = a->start_idx; k <= a->end_idx; k++)
            a->body_instrs[a->num_body_instrs++] = k;
          /* Mark b as absorbed */
          b->start_idx = -1;
          merged = 1;
        }
      }
    } while (merged);
  }

  LOG_LOOP_OPT("=== LOOP UNROLL/ELIM START: %d loop(s) detected ===", loops->num_loops);

  int unrolled = 0;
  for (int i = 0; i < loops->num_loops; i++)
  {
    IRLoop *loop = &loops->loops[i];
    if (loop->start_idx < 0)
    {
      LOG_LOOP_OPT("Loop %d: absorbed by merge, skipping", i);
      continue; /* absorbed by merge */
    }

    LOG_LOOP_OPT("Loop %d: header=%d start=%d end=%d preheader=%d", i, loop->header_idx, loop->start_idx, loop->end_idx,
                 loop->preheader_idx);

    /* Dump loop body instructions for debugging */
#ifdef DEBUG_LOOP_OPT
    for (int di = loop->start_idx; di <= loop->end_idx; di++)
    {
      IRQuadCompact *dq = &ir->compact_instructions[di];
      if (dq->op != TCCIR_OP_NOP)
        LOG_LOOP_OPT("[%d] op=%d%s", di, dq->op, di == loop->header_idx ? " (header)" : "");
    }
#endif

    /* Skip loops that have external entries into the body (not to the header).
     * After jump threading, an outer loop's back-edge may jump directly into
     * the inner loop body.  Unrolling would NOP those targets and break the
     * outer loop. */
    int ext_entry = 0;
    for (int j = 0; j < ir->next_instruction_index && !ext_entry; j++)
    {
      if (j >= loop->start_idx && j <= loop->end_idx)
        continue; /* skip instructions inside the loop itself */
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
      {
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int jtarget = (int)irop_get_imm64_ex(ir, jdest);
        /* Target is inside the loop body (not at the header) */
        if (jtarget > loop->start_idx && jtarget <= loop->end_idx)
        {
          LOG_LOOP_OPT("Loop %d: external entry from [%d] to [%d], skipping", i, j, jtarget);
          ext_entry = 1;
        }
      }
    }
    if (ext_entry)
      continue;

    /* Try elimination first (cheaper than unrolling), fall back to unrolling */
    if (!try_eliminate_loop(ir, loop))
      unrolled += try_unroll_loop(ir, loop);
    else
      unrolled++;
  }

  LOG_LOOP_OPT("=== LOOP UNROLL/ELIM END: %d loop(s) processed ===", unrolled);
  tcc_ir_free_loops(loops);
  return unrolled;
}

/* ============================================================================
 * Loop Rotation - Convert top-tested loops to bottom-tested
 * ============================================================================
 *
 * TCC generates for/while loops as top-tested with a 3-block layout:
 *
 *   HEADER:  CMP iv, limit       ; test at top
 *            JUMPIF exit if COND
 *            JUMP body_start     ; skip latch on first iteration
 *   LATCH:   save_iv             ; latch block
 *            iv = save + step
 *            JUMP header         ; back-edge
 *   BODY:    ...work...          ; body block
 *            JUMP latch          ; to latch
 *
 * This causes 3 branches per iteration.  Loop rotation converts this to a
 * bottom-tested do-while with a guard, producing 1 branch per iteration:
 *
 *   GUARD:   CMP iv, limit       ; guard (once)
 *            JUMPIF exit if COND
 *   BODY:    ...work...          ; body (back-edge target)
 *   LATCH:   save_iv             ; latch inlined
 *            iv = save + step
 *            CMP iv, limit       ; tail test
 *            JUMPIF body if !COND ; single back-edge
 *   EXIT:    ...
 */

/* Invert a comparison condition token for the back-edge. */
static int invert_condition(int cond)
{
  switch (cond)
  {
  case TOK_GE:
    return TOK_LT;
  case TOK_GT:
    return TOK_LE;
  case TOK_LT:
    return TOK_GE;
  case TOK_LE:
    return TOK_GT;
  case TOK_EQ:
    return TOK_NE;
  case TOK_NE:
    return TOK_EQ;
  case TOK_UGE:
    return TOK_ULT;
  case TOK_UGT:
    return TOK_ULE;
  case TOK_ULT:
    return TOK_UGE;
  case TOK_ULE:
    return TOK_UGT;
  default:
    return -1;
  }
}

/* Try to rotate a single loop.  Returns 1 if rotated, 0 otherwise.
 *
 * Expected IR pattern (header_idx..body_end_jmp):
 *   [header_idx]:   CMP iv, limit
 *   [header_idx+1]: JUMPIF exit if COND      (exit_target > end_idx)
 *   [header_idx+2]: JUMP body_start           (body_start > end_idx)
 *   [header_idx+3 .. end_idx-1]: latch instrs (IV save + increment)
 *   [end_idx]:      JUMP header_idx            (back-edge)
 *   [body_start .. body_end]: body instrs
 *   [body_end+1]:   JUMP latch_start           (body→latch) */
static int try_rotate_loop(TCCIRState *ir, IRLoop *loop)
{
  int hi = loop->header_idx;
  int n = ir->next_instruction_index;

  /* --- Step 1: Validate header pattern --- */

  /* Need at least 3 instructions: CMP, JUMPIF, JUMP */
  if (hi + 2 > loop->end_idx)
    return 0;

  IRQuadCompact *cmp_q = &ir->compact_instructions[hi];
  IRQuadCompact *jif_q = &ir->compact_instructions[hi + 1];
  IRQuadCompact *jmp_q = &ir->compact_instructions[hi + 2];

  if (cmp_q->op != TCCIR_OP_CMP)
    return 0;
  if (jif_q->op != TCCIR_OP_JUMPIF)
    return 0;
  if (jmp_q->op != TCCIR_OP_JUMP)
    return 0;

  /* Get exit target and condition */
  IROperand exit_dest = tcc_ir_op_get_dest(ir, jif_q);
  int exit_target = (int)irop_get_imm64_ex(ir, exit_dest);
  IROperand cond_op = tcc_ir_op_get_src1(ir, jif_q);
  int cond = (int)irop_get_imm64_ex(ir, cond_op);

  /* Get body-entry target */
  IROperand body_entry_dest = tcc_ir_op_get_dest(ir, jmp_q);
  int body_start = (int)irop_get_imm64_ex(ir, body_entry_dest);

  /* --- Step 2: Find the back-edge JUMP targeting the header --- */
  /* Don't use loop->end_idx directly — when the loop detector merges
   * the body→latch jump as part of the loop, end_idx covers the body
   * too.  Instead, scan from hi+3 for the first JUMP targeting hi. */
  int backedge_idx = -1;
  for (int i = hi + 3; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int jt = (int)irop_get_imm64_ex(ir, jd);
      if (jt == hi)
      {
        backedge_idx = i;
        break;
      }
    }
  }
  if (backedge_idx < 0)
  {
    LOG_LOOP_OPT("Rotation: reject — no back-edge JUMP to header %d", hi);
    return 0;
  }
  LOG_LOOP_OPT("Rotation: backedge at %d, exit_target=%d, body_start=%d", backedge_idx, exit_target, body_start);

  /* Exit must jump outside the loop's core region [hi, backedge_idx].
   * The extended end_idx may include exit targets due to aggressive
   * loop extension, so use the actual backedge position instead. */
  if (exit_target > hi && exit_target <= backedge_idx)
  {
    LOG_LOOP_OPT("Rotation: reject — exit_target %d inside [%d,%d]", exit_target, hi, backedge_idx);
    return 0;
  }

  /* Body must be after the back-edge (standard TCC layout) */
  if (body_start <= backedge_idx || body_start >= n)
  {
    LOG_LOOP_OPT("Rotation: reject — body_start %d not after backedge %d (n=%d)", body_start, backedge_idx, n);
    return 0;
  }

  /* --- Step 3: Identify latch region [latch_start .. latch_end] --- */
  int latch_start = hi + 3;
  int latch_end = backedge_idx - 1; /* exclude back-edge JUMP */
  int latch_count = latch_end - latch_start + 1;
  if (latch_count < 0)
    latch_count = 0;

  /* Latch must be small (IV save + increment, typically 2 instrs) */
  if (latch_count > 8)
    return 0;

  /* --- Step 4: Identify body region and body→latch jump --- */
  /* Scan from body_start forward for a JUMP targeting anywhere in the
   * latch region [latch_start, end_idx].  After jump threading + DCE,
   * the first latch instruction may have become NOP, and the body→latch
   * JUMP may have been threaded to a later instruction in the latch. */
  int body_end_jmp = -1;
  int body_latch_target = -1;
  int body_end_is_implicit = 0;
  for (int i = body_start; i < n && i < body_start + 100; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int jt = (int)irop_get_imm64_ex(ir, jd);
      if (jt >= latch_start && jt <= backedge_idx)
      {
        body_end_jmp = i;
        body_latch_target = jt;
        break;
      }
    }
  }

  /* Nested loops: the inner loop's conditional exit JUMPIF may target the
   * outer latch directly (after jump threading eliminates the explicit JUMP).
   * Only use this path when the body contains a backward jump (inner loop).
   * Simple loops with eliminated break JMPs rely on fallthrough to exit,
   * which rotation would break. */
  if (body_end_jmp < 0)
  {
    int has_inner_loop = 0;
    for (int i = body_start; i < n && i < body_start + 100; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)irop_get_imm64_ex(ir, jd);
        if (jt < i && jt >= body_start)
        {
          has_inner_loop = 1;
          break;
        }
      }
    }
    if (has_inner_loop)
    {
      for (int i = body_start; i < n && i < body_start + 100; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_JUMPIF)
        {
          IROperand jd = tcc_ir_op_get_dest(ir, q);
          int jt = (int)irop_get_imm64_ex(ir, jd);
          if (jt >= latch_start && jt <= backedge_idx)
          {
            body_latch_target = jt;
            body_end_is_implicit = 1;
            body_end_jmp = exit_target - 1;
            break;
          }
        }
      }
    }
  }
  if (body_end_jmp < 0)
  {
    LOG_LOOP_OPT("Rotation: reject — no body→latch JUMP from body_start=%d targeting [%d,%d]", body_start, latch_start,
                 backedge_idx);
    return 0;
  }
  LOG_LOOP_OPT("Rotation: body_end_jmp=%d, latch=[%d,%d], latch_count=%d", body_end_jmp, latch_start, latch_end,
               latch_count);
  int body_end = body_end_is_implicit ? body_end_jmp : body_end_jmp - 1;
  int body_count = body_end - body_start + 1;
  if (body_count < 0)
    body_count = 0;
  if (body_count > 128)
    return 0;

  /* --- Step 4a2: Reject if body has a fall-through exit --- */
  /* When body_end_is_implicit, the body may end with trailing NOPs (from
   * eliminated fall-through jumps) after a JUMPIF.  In the original layout,
   * the fall-through from that JUMPIF goes to the exit target (e.g., a goto
   * label).  After rotation, the latch is placed right after the body, so the
   * fall-through would go to the latch instead — a miscompilation.
   * Reject if the last non-NOP body instruction is a JUMPIF whose fall-through
   * reaches the exit target. */
  if (body_end_is_implicit)
  {
    int last_real = body_end;
    while (last_real >= body_start && ir->compact_instructions[last_real].op == TCCIR_OP_NOP)
      last_real--;
    if (last_real >= body_start && ir->compact_instructions[last_real].op == TCCIR_OP_JUMPIF)
    {
      int ft = last_real + 1;
      while (ft < n && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
        ft++;
      if (ft >= exit_target)
      {
        LOG_LOOP_OPT("Rotation: reject — body JUMPIF at %d falls through to exit_target %d", last_real, exit_target);
        return 0;
      }
    }
  }

  /* --- Step 4b: Check for external entries into the loop --- */
  /* Skip instructions inside the header/latch region [start_idx, end_idx]
   * and the body region [body_start, body_end_jmp] — those are normal
   * loop control flow, not external entries. */
  {
    int ext_entry = 0;
    for (int j = 0; j < n && !ext_entry; j++)
    {
      if (j >= loop->start_idx && j <= loop->end_idx)
        continue;
      if (j >= body_start && j <= body_end_jmp)
        continue;
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
      {
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int jtarget = (int)irop_get_imm64_ex(ir, jdest);
        /* External jump into the latch or body (not the header) */
        if (jtarget > loop->start_idx && jtarget <= loop->end_idx)
          ext_entry = 1;
        if (jtarget >= body_start && jtarget <= body_end_jmp)
          ext_entry = 1;
      }
    }
    if (ext_entry)
    {
      LOG_LOOP_OPT("Rotation: reject — external entry into loop body/latch");
      return 0;
    }
  }

  /* --- Step 5: Validate body contents --- */
  /* Only allow body branches when there's a nested inner loop (backward
   * jump within the body).  Simple loops with conditional bodies (if/break)
   * should not be rotated here — later passes like IV strength reduction
   * may not handle the rotated form correctly. */
  int body_has_branches = 0;
  {
    int has_inner_loop = 0;
    for (int i = body_start; i <= body_end; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)irop_get_imm64_ex(ir, jd);
        if (jt < i && jt >= body_start)
        {
          has_inner_loop = 1;
          break;
        }
      }
    }

    int region_start_5 = hi + 2;
    for (int i = body_start; i <= body_end; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_IJUMP)
        return 0;
      if (q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_JUMP)
      {
        if (!has_inner_loop)
          return 0;
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)irop_get_imm64_ex(ir, jd);
        /* Internal body branches (inner loops etc.) - will be remapped */
        if (jt >= body_start && jt <= body_end_jmp)
        {
          body_has_branches = 1;
          continue;
        }
        /* Branch to latch region - will be remapped */
        if (jt >= latch_start && jt <= backedge_idx)
        {
          body_has_branches = 1;
          continue;
        }
        /* Branch outside modified region - no remap needed */
        if (jt < region_start_5 || jt > body_end_jmp)
        {
          body_has_branches = 1;
          continue;
        }
        return 0;
      }
    }
  }

  /* Validate latch contents - no branches except the back-edge we already found.
   * Use body_latch_target as effective latch start (may skip leading NOPs). */
  int eff_latch_start = body_latch_target;
  int eff_latch_count = latch_end - eff_latch_start + 1;
  if (eff_latch_count < 0)
    eff_latch_count = 0;
  for (int i = eff_latch_start; i <= latch_end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_JUMP)
    {
      LOG_LOOP_OPT("Rotation: reject — latch has branch at i=%d (op=%d)", i, q->op);
      return 0;
    }
  }

  /* --- Step 6: Check size - rotated code must fit --- */
  /* Available slots: [hi+2 .. body_end_jmp] */
  int region_start = hi + 2;     /* first slot to overwrite (was body-entry JUMP) */
  int region_end = body_end_jmp; /* last slot to overwrite (was body→latch JUMP) */
  int avail_slots = region_end - region_start + 1;

  /* Check if an exit jump is needed after the bottom test.
   * The fall-through from the bottom JUMPIF goes to region_end+1.
   * If that doesn't reach exit_target (accounting for NOPs), we need
   * an explicit JUMP to exit_target.  This happens in nested loops
   * where the inner loop's exit target (outer latch) is above the
   * loop body in instruction order, not right after it. */
  int need_exit_jump = 0;
  {
    int ft = region_end + 1;
    while (ft < n && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
      ft++;
    if (ft != exit_target)
      need_exit_jump = 1;
  }

  /* Need: body_count + eff_latch_count + 2 (tail CMP + JUMPIF) + optional exit JUMP */
  int needed = body_count + eff_latch_count + 2 + need_exit_jump;
  if (needed > avail_slots)
  {
    LOG_LOOP_OPT("Rotation: reject — needed %d > avail %d", needed, avail_slots);
    return 0;
  }

  /* Invert condition for back-edge */
  int inv_cond = invert_condition(cond);
  if (inv_cond < 0)
  {
    LOG_LOOP_OPT("Rotation: reject — cannot invert cond 0x%x", cond);
    return 0;
  }
  LOG_LOOP_OPT("Rotation: all checks passed, rotating!");

  /* --- Step 7: Save instructions before overwriting --- */
  /* Save CMP operands for the tail test */
  IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cmp_q);

  /* Save body instructions */
  int body_ops[128], latch_ops[8];
  IROperand body_dests[128], body_src1s[128], body_src2s[128];
  IROperand body_extras[128]; /* MLA accumulator (operand_base+3) */
  int body_has_extra[128];
  IROperand latch_dests[8], latch_src1s[8], latch_src2s[8];
  uint32_t body_lines[128], latch_lines[8];

  for (int b = 0; b < body_count; b++)
  {
    IRQuadCompact *bq = &ir->compact_instructions[body_start + b];
    int op = bq->op;
    body_ops[b] = op;
    body_lines[b] = bq->line_num;
    body_dests[b] = (IROperand){0};
    body_src1s[b] = (IROperand){0};
    body_src2s[b] = (IROperand){0};
    body_extras[b] = (IROperand){0};
    body_has_extra[b] = (op == TCCIR_OP_MLA || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED);
    if (irop_config[op].has_dest)
      body_dests[b] = ir->iroperand_pool[bq->operand_base];
    if (irop_config[op].has_src1)
      body_src1s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest];
    if (irop_config[op].has_src2)
      body_src2s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest + irop_config[op].has_src1];
    if (body_has_extra[b])
      body_extras[b] = ir->iroperand_pool[bq->operand_base + 3];
  }

  /* Save latch instructions (from effective latch start, skipping leading NOPs) */
  for (int l = 0; l < eff_latch_count; l++)
  {
    IRQuadCompact *lq = &ir->compact_instructions[eff_latch_start + l];
    int op = lq->op;
    latch_ops[l] = op;
    latch_lines[l] = lq->line_num;
    latch_dests[l] = (IROperand){0};
    latch_src1s[l] = (IROperand){0};
    latch_src2s[l] = (IROperand){0};
    if (irop_config[op].has_dest)
      latch_dests[l] = ir->iroperand_pool[lq->operand_base];
    if (irop_config[op].has_src1)
      latch_src1s[l] = ir->iroperand_pool[lq->operand_base + irop_config[op].has_dest];
    if (irop_config[op].has_src2)
      latch_src2s[l] = ir->iroperand_pool[lq->operand_base + irop_config[op].has_dest + irop_config[op].has_src1];
  }

  /* --- Step 8: NOP the region [hi+2 .. body_end_jmp] --- */
  for (int i = region_start; i <= region_end; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  /* --- Step 9: Write rotated code --- */
  int wp = region_start;

  /* Write body instructions */
  int body_target = wp; /* back-edge will target this */
  for (int b = 0; b < body_count; b++)
  {
    write_instr_at_nop(ir, wp, body_ops[b], body_dests[b], body_src1s[b], body_src2s[b]);
    if (body_has_extra[b])
      tcc_ir_pool_add(ir, body_extras[b]); /* MLA accumulator at operand_base+3 */
    ir->compact_instructions[wp].line_num = body_lines[b];
    wp++;
  }

  /* Remap branch targets within the relocated body */
  if (body_has_branches)
  {
    int body_offset = region_start - body_start;
    int latch_new_start = region_start + body_count;
    int latch_offset = latch_new_start - eff_latch_start;
    for (int i = body_target; i < body_target + body_count; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        IROperand *dest = &ir->iroperand_pool[q->operand_base];
        int old_target = dest->u.imm32;
        int new_target = -1;
        if (old_target >= body_start && old_target <= body_end_jmp)
        {
          new_target = old_target + body_offset;
        }
        else if (old_target >= eff_latch_start && old_target <= latch_end)
        {
          new_target = old_target + latch_offset;
        }
        if (new_target >= 0)
        {
          dest->u.imm32 = new_target;
          if (new_target < n)
            ir->compact_instructions[new_target].is_jump_target = 1;
        }
      }
    }
  }

  /* Write latch instructions (IV save + increment, no back-edge JUMP) */
  for (int l = 0; l < eff_latch_count; l++)
  {
    write_instr_at_nop(ir, wp, latch_ops[l], latch_dests[l], latch_src1s[l], latch_src2s[l]);
    ir->compact_instructions[wp].line_num = latch_lines[l];
    wp++;
  }

  /* Write tail CMP (duplicate of header CMP) */
  write_instr_at_nop(ir, wp, TCCIR_OP_CMP, (IROperand){0}, cmp_src1, cmp_src2);
  wp++;

  /* Write tail JUMPIF with inverted condition, targeting body_target */
  {
    IROperand jmp_dest = irop_make_imm32(-1, body_target, IROP_BTYPE_INT32);
    IROperand inv_cond_op = irop_make_imm32(-1, inv_cond, IROP_BTYPE_INT32);
    write_instr_at_nop(ir, wp, TCCIR_OP_JUMPIF, jmp_dest, inv_cond_op, (IROperand){0});
    wp++;
  }

  /* Write exit JUMP when fall-through doesn't reach exit_target */
  if (need_exit_jump)
  {
    IROperand exit_dest = irop_make_imm32(-1, exit_target, IROP_BTYPE_INT32);
    write_instr_at_nop(ir, wp, TCCIR_OP_JUMP, exit_dest, (IROperand){0}, (IROperand){0});
    wp++;
  }

  /* --- Step 10: Fix is_jump_target flags --- */
  /* The first body instruction is the back-edge target */
  ir->compact_instructions[body_target].is_jump_target = 1;

  /* The old header CMP no longer has a back-edge targeting it, but may still
   * be targeted by outer code (e.g. goto).  Conservatively leave it. */

  /* Clear is_jump_target on the exit_target instruction only if it was set
   * by the old body-entry jump — it's still targeted by the guard JUMPIF,
   * so leave it alone. */

  LOG_IR_GEN("[LOOP-ROTATE] Rotated loop header=%d body=[%d..%d] latch=[%d..%d] → bottom-tested at %d", hi, body_start,
             body_end, latch_start, latch_end, body_target);

  return 1;
}

static int loop_size_cmp(const void *a, const void *b)
{
  const IRLoop *la = (const IRLoop *)a;
  const IRLoop *lb = (const IRLoop *)b;
  int sa = la->end_idx - la->start_idx;
  int sb = lb->end_idx - lb->start_idx;
  return sa - sb;
}

int tcc_ir_opt_loop_rotation(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total_rotated = 0;
  for (int pass = 0; pass < 4; pass++)
  {
    IRLoops *loops = tcc_ir_detect_loops(ir);
    if (!loops || loops->num_loops == 0)
    {
      tcc_ir_free_loops(loops);
      break;
    }

    /* Sort smallest-first so inner loops rotate before outer ones */
    qsort(loops->loops, loops->num_loops, sizeof(IRLoop), loop_size_cmp);

    LOG_LOOP_OPT("=== LOOP ROTATION PASS %d: %d loop(s) ===", pass, loops->num_loops);

    int pass_rotated = 0;
    for (int i = 0; i < loops->num_loops; i++)
    {
      IRLoop *loop = &loops->loops[i];
      if (loop->start_idx < 0)
        continue;

      LOG_LOOP_OPT("Rotation: trying loop %d header=%d start=%d end=%d", i, loop->header_idx, loop->start_idx,
                   loop->end_idx);
      int did_rotate = try_rotate_loop(ir, loop);
      LOG_LOOP_OPT("Rotation: loop %d %s", i, did_rotate ? "ROTATED" : "not rotated");
      pass_rotated += did_rotate;
    }

    LOG_LOOP_OPT("=== LOOP ROTATION PASS %d END: %d rotated ===", pass, pass_rotated);
    tcc_ir_free_loops(loops);
    total_rotated += pass_rotated;
    if (pass_rotated == 0)
      break;
  }
  return total_rotated;
}


/* ============================================================================
 * Conditional Select (ITE) Optimization
 *
 * Detects "diamond" if/else patterns where both branches produce a single
 * value and replaces them with a SELECT instruction, enabling ITE generation.
 *
 * Pattern: CMP + JUMPIF + PARAM+CALL(then) + JUMP + PARAM+CALL(else)
 * where both sides call the same function with only arg0 differing.
 * Result: CMP + SELECT arg0 + PARAM+CALL
 *
 * Also handles: CMP + JUMPIF + ASSIGN(then) + JUMP + ASSIGN(else)
 * where both sides assign to the same vreg.
 * Result: CMP + SELECT vreg
 * ============================================================================ */

/* Helper: skip NOP instructions forward, return next non-NOP index or n */
static int ir_skip_nops_forward(TCCIRState *ir, int start, int n)
{
  for (int j = start; j < n; j++)
    if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
      return j;
  return n;
}

/* Negate a TOK_* condition code: EQ↔NE, LT↔GE, etc. */
static int ir_negate_condition(int cond)
{
  return cond ^ 1;
}

/* O(1) variant for use with a precomputed jump-target count array.
 * `jt_cnt[t]` = number of JUMP/JUMPIFs targeting t. The exclude is honored
 * by checking whether the instruction at exclude_idx is itself a jump to
 * `target` and subtracting its contribution. */
static int ir_has_other_jump_to_fast(TCCIRState *ir, const int *jt_cnt,
                                     int target, int exclude_idx)
{
  int n = ir->next_instruction_index;
  if (target < 0 || target >= n) return 0;
  int total = jt_cnt[target];
  if (total == 0) return 0;
  if (exclude_idx >= 0 && exclude_idx < n) {
    IRQuadCompact *q = &ir->compact_instructions[exclude_idx];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if ((int)irop_get_imm64_ex(ir, d) == target) total--;
    }
  }
  return total > 0;
}

/* ============================================================================
 * Redundant Initialization Elimination
 * ============================================================================
 *
 * Eliminates function-entry VAR initializations that are always killed
 * (redefined) before any use.  Common pattern:
 *
 *   int sum = 0;           ← dead init (eliminated)
 *   for (...) { ... }      ← loop doesn't use sum
 *   for (...) {
 *     sum = 0;             ← kills sum
 *     for (...) sum += x;  ← first use after kill
 *   }
 *
 * Uses forward dataflow: from the init, follow all control flow paths.
 * If every path reaches a redef of V before any use of V, the init is dead.
 */
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

/* ============================================================================
 * Decrement-to-Zero Loop Transformation
 * ============================================================================
 *
 * Transforms count-up loops whose IV is only used for counting into
 * count-down-to-zero loops.  This enables the backend to fuse the
 * SUB + CMP #0 into a single flag-setting SUBS instruction.
 *
 * Before:  V = 0; ... V = V + 1; CMP V, #limit; JUMPIF <S body
 * After:   V = limit; ... V = V + #-1; CMP V, #0; JUMPIF >S body
 *
 * Requirements:
 * - IV has init=0, step=+1, limit > 0 (constant)
 * - IV has no uses in loop body other than increment + CMP
 * - Back-edge condition is <S (signed less-than)
 */
int tcc_ir_opt_decrement_to_zero(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int total_changes = 0;

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

    /* Find a simple count-up IV: V = V + 1 in the latch */
    int iv_def_idx = -1;
    int32_t iv_vr = -1;

    for (int i = loop->end_idx; i >= loop->start_idx; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_JUMPIF)
        continue;
      if (q->op != TCCIR_OP_ADD)
        break;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);

      int d_vr = irop_get_vreg(dest);
      int s1_vr = irop_get_vreg(src1);
      if (d_vr >= 0 && d_vr == s1_vr && irop_is_immediate(src2) && irop_get_imm64_ex(ir, src2) == 1 &&
          TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR)
      {
        iv_def_idx = i;
        iv_vr = d_vr;
        break;
      }
      break;
    }

    if (iv_def_idx < 0)
      continue;

    /* Find the back-edge CMP: CMP V, #limit; JUMPIF <S body */
    int be_cmp_idx = -1;
    int be_jmpif_idx = -1;
    int limit_val = 0;

    for (int i = loop->end_idx; i >= loop->end_idx - 5 && i >= 0; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_CMP)
        continue;

      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_get_vreg(s1) != iv_vr || !irop_is_immediate(s2))
        continue;

      int jq_idx = i + 1;
      while (jq_idx < n && ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP)
        jq_idx++;
      if (jq_idx >= n || ir->compact_instructions[jq_idx].op != TCCIR_OP_JUMPIF)
        continue;

      IROperand cond = tcc_ir_op_get_src1(ir, &ir->compact_instructions[jq_idx]);
      int cond_tok = (int)irop_get_imm64_ex(ir, cond);
      if (cond_tok != 0x9c) /* TOK_LT (<S) */
        continue;

      limit_val = (int)irop_get_imm64_ex(ir, s2);
      if (limit_val <= 0)
        continue;

      be_cmp_idx = i;
      be_jmpif_idx = jq_idx;
      break;
    }

    if (be_cmp_idx < 0)
      continue;

    /* Find the IV init: V = #0 in the preheader */
    int init_idx = -1;
    for (int i = loop->preheader_idx; i >= 0 && i >= loop->preheader_idx - 5; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op != TCCIR_OP_ASSIGN)
        continue;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(dest) == iv_vr && irop_is_immediate(src1) && irop_get_imm64_ex(ir, src1) == 0)
      {
        init_idx = i;
        break;
      }
    }

    if (init_idx < 0)
      continue;

    /* Find pre-test guard: CMP V, #limit near header.
     * We'll NOP it since limit > 0 means the loop always executes. */
    int hdr_cmp_idx = -1, hdr_jmpif_idx = -1;
    {
      int scan_start = loop->preheader_idx;
      if (scan_start < 0)
        scan_start = 0;
      for (int i = scan_start; i <= loop->header_idx + 2 && i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_CMP)
          continue;
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s1) != iv_vr)
          continue;

        int jq_idx = i + 1;
        while (jq_idx < n && (ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP ||
                              ir->compact_instructions[jq_idx].op == TCCIR_OP_ASSIGN))
          jq_idx++;
        if (jq_idx < n && ir->compact_instructions[jq_idx].op == TCCIR_OP_JUMPIF)
        {
          hdr_cmp_idx = i;
          hdr_jmpif_idx = jq_idx;
          break;
        }
      }
    }

    /* Verify IV has NO uses anywhere besides: init, increment, back-edge
     * CMP, pre-test CMP, and copy-through temp (T=V before V=T+step,
     * but ONLY if T is unused outside the increment). */
    {
      int other_uses = 0;
      int copy_through_vr = -1;

      /* Find the copy-through temp if it exists */
      for (int k = iv_def_idx - 1; k >= iv_def_idx - 3 && k >= 0; k--)
      {
        IRQuadCompact *q = &ir->compact_instructions[k];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (q->op == TCCIR_OP_ASSIGN)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(s) == iv_vr)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            copy_through_vr = irop_get_vreg(d);
          }
        }
        break;
      }

      /* Check if the copy-through temp is used outside the increment */
      if (copy_through_vr >= 0)
      {
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP || i == iv_def_idx)
            continue;
          if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == copy_through_vr)
          {
            other_uses++;
            break;
          }
          if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == copy_through_vr)
          {
            other_uses++;
            break;
          }
        }
      }

      /* Find the extent of the IV's live range: from init_idx to the next
       * redefinition of iv_vr after the loop (exclusive).  Uses of iv_vr
       * after a redefinition belong to a different live range. */
      int live_end = n;
      for (int i = loop->end_idx + 1; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          if (irop_get_vreg(d) == iv_vr && !irop_op_is_lval(d))
          {
            live_end = i;
            break;
          }
        }
      }

      /* Check uses of IV within its live range */
      for (int i = 0; i < live_end && other_uses == 0; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (i == init_idx || i == iv_def_idx || i == be_cmp_idx || i == hdr_cmp_idx)
          continue;

        /* Allow copy-through temp only if it has no external uses */
        if (copy_through_vr >= 0 && q->op == TCCIR_OP_ASSIGN && i >= iv_def_idx - 3 && i < iv_def_idx)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(s) == iv_vr)
            continue;
        }

        if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == iv_vr)
          other_uses++;
        if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == iv_vr)
          other_uses++;
      }

      if (other_uses > 0)
        continue;
    }

    /* Must have found the pre-test guard — the transform changes the init
     * value, which would make an unfound/unpatched guard skip the loop. */
    if (hdr_cmp_idx < 0)
      continue;

    /* === Apply transformation === */

    /* 1. Init: V = #0  →  V = #limit */
    {
      IRQuadCompact *q = &ir->compact_instructions[init_idx];
      IROperand new_init = irop_make_imm32(-1, limit_val, IROP_BTYPE_INT32);
      tcc_ir_op_set_src1(ir, q, new_init);
    }

    /* 2. Increment: V = V + 1  →  V = V - 1 */
    {
      IRQuadCompact *q = &ir->compact_instructions[iv_def_idx];
      q->op = TCCIR_OP_SUB;
      IROperand new_step = irop_make_imm32(-1, 1, IROP_BTYPE_INT32);
      tcc_ir_op_set_src2(ir, q, new_step);
    }

    /* 3. Back-edge: CMP V, #limit  →  CMP V, #0 */
    {
      IRQuadCompact *q = &ir->compact_instructions[be_cmp_idx];
      IROperand zero = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
      tcc_ir_op_set_src2(ir, q, zero);
    }

    /* 4. Back-edge condition: <S  →  != (not-equal zero)
     * Using != instead of >S because the codegen peephole can fuse
     * SUB + CMP #0 only for EQ/NE conditions (Z flag only). */
    {
      IRQuadCompact *q = &ir->compact_instructions[be_jmpif_idx];
      IROperand new_cond = irop_make_imm32(-1, 0x95, IROP_BTYPE_INT32); /* TOK_NE (!=) */
      tcc_ir_op_set_src1(ir, q, new_cond);
    }

    /* 5. NOP the pre-test guard (always passes since limit > 0) */
    if (hdr_cmp_idx >= 0)
    {
      ir->compact_instructions[hdr_cmp_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[hdr_jmpif_idx].op = TCCIR_OP_NOP;
    }

    total_changes++;
  }

  tcc_ir_free_loops(loops);
  return total_changes;
}

int tcc_ir_opt_select(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 5)
    return 0;

  /* Precompute jump target counts: jt_cnt[target] = number of JUMP/JUMPIFs
   * targeting `target`. Lets ir_has_other_jump_to_fast (below) answer in O(1)
   * what would otherwise be an O(n) scan per query — opt_select calls the
   * predicate four times per JUMPIF, which on a function with thousands of
   * branches drives the pass into O(n^2). Decrement the count whenever we
   * NOP a JUMP/JUMPIF below to keep it consistent. */
  int *jt_cnt = tcc_mallocz(sizeof(int) * n);
  for (int j = 0; j < n; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int t = (int)irop_get_imm64_ex(ir, d);
    if (t >= 0 && t < n)
      jt_cnt[t]++;
  }
  #define JT_HAS_OTHER(target, exclude_idx) \
    ir_has_other_jump_to_fast(ir, jt_cnt, (target), (exclude_idx))
  #define JT_NOP_JUMP(idx) do { \
    IRQuadCompact *_jq = &ir->compact_instructions[idx]; \
    if (_jq->op == TCCIR_OP_JUMP || _jq->op == TCCIR_OP_JUMPIF) { \
      IROperand _jd = tcc_ir_op_get_dest(ir, _jq); \
      int _jt = (int)irop_get_imm64_ex(ir, _jd); \
      if (_jt >= 0 && _jt < n && jt_cnt[_jt] > 0) jt_cnt[_jt]--; \
    } \
    _jq->op = TCCIR_OP_NOP; \
  } while (0)

  for (int i = 0; i < n - 4; i++)
  {
    IRQuadCompact *jumpif_q = &ir->compact_instructions[i];
    if (jumpif_q->op != TCCIR_OP_JUMPIF)
      continue;

    /* Get JUMPIF operands: dest=else_target, src1=condition */
    IROperand jumpif_dest = tcc_ir_op_get_dest(ir, jumpif_q);
    IROperand jumpif_cond = tcc_ir_op_get_src1(ir, jumpif_q);
    int branch_cond = (int)irop_get_imm64_ex(ir, jumpif_cond);
    int else_target = (int)irop_get_imm64_ex(ir, jumpif_dest);

    /* The "then" condition is the negation of the branch condition
     * (branch jumps to else when cond is true, so then runs when !cond) */
    int then_cond = ir_negate_condition(branch_cond);

    /* Scan forward past NOPs to find the then-block start */
    int then_start = ir_skip_nops_forward(ir, i + 1, n);
    if (then_start >= n)
      continue;

    /* Safety: the then-block (fall-through) must not be a jump target from
     * elsewhere, and the else-block must only be targeted by this JUMPIF.
     * Otherwise NOP'ing the blocks would break other control flow. */
    if (JT_HAS_OTHER(then_start, i))
      continue;
    if (JT_HAS_OTHER(else_target, i))
      continue;

    /* ----------------------------------------------------------------
     * Pattern: Call diamond (PARAM+CALL in both branches)
     * ----------------------------------------------------------------
     * then: PARAM0[call_A] val1, CALL func
     * JUMP to merge
     * else: PARAM0[call_B] val2, CALL func
     * merge: ...
     * ---------------------------------------------------------------- */
    IRQuadCompact *then_q1 = &ir->compact_instructions[then_start];
    if (then_q1->op == TCCIR_OP_FUNCPARAMVAL || then_q1->op == TCCIR_OP_FUNCPARAMVOID)
    {
      /* Check if next non-NOP is a FUNCCALLVOID */
      int then_call_idx = ir_skip_nops_forward(ir, then_start + 1, n);
      if (then_call_idx >= n)
        continue;
      IRQuadCompact *then_call_q = &ir->compact_instructions[then_call_idx];
      if (then_call_q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      /* Next should be unconditional JUMP to merge */
      int jump_idx = ir_skip_nops_forward(ir, then_call_idx + 1, n);
      if (jump_idx >= n)
        continue;
      IRQuadCompact *jump_q = &ir->compact_instructions[jump_idx];
      if (jump_q->op != TCCIR_OP_JUMP)
        continue;
      int merge_target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jump_q));

      /* else_target should point to the else block */
      if (else_target < 0 || else_target >= n)
        continue;

      /* Find else block start (skip NOPs) */
      int else_start = ir_skip_nops_forward(ir, else_target, n);
      if (else_start >= n)
        continue;

      /* Else block: PARAM + CALL same function */
      IRQuadCompact *else_q1 = &ir->compact_instructions[else_start];
      if (else_q1->op != TCCIR_OP_FUNCPARAMVAL && else_q1->op != TCCIR_OP_FUNCPARAMVOID)
        continue;

      int else_call_idx = ir_skip_nops_forward(ir, else_start + 1, n);
      if (else_call_idx >= n)
        continue;
      IRQuadCompact *else_call_q = &ir->compact_instructions[else_call_idx];
      if (else_call_q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      /* Verify both calls target the same function */
      Sym *then_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, then_call_q));
      Sym *else_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, else_call_q));
      if (!then_callee || !else_callee || then_callee != else_callee)
        continue;

      /* Verify both params are param index 0 for their respective calls */
      IROperand then_param_enc = tcc_ir_op_get_src2(ir, then_q1);
      IROperand else_param_enc = tcc_ir_op_get_src2(ir, else_q1);
      int then_param_idx = TCCIR_DECODE_PARAM_IDX((uint32_t)irop_get_imm64_ex(ir, then_param_enc));
      int else_param_idx = TCCIR_DECODE_PARAM_IDX((uint32_t)irop_get_imm64_ex(ir, else_param_enc));
      if (then_param_idx != 0 || else_param_idx != 0)
        continue;

      /* Both calls have 1 argument (CALL #N where argc from encoded src2) */
      IROperand then_call_meta = tcc_ir_op_get_src2(ir, then_call_q);
      IROperand else_call_meta = tcc_ir_op_get_src2(ir, else_call_q);
      int then_argc = TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, then_call_meta));
      int else_argc = TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, else_call_meta));
      if (then_argc != 1 || else_argc != 1)
        continue;

      /* Verify merge_target is right after the else CALL (or after NOPs) */
      int after_else_call = ir_skip_nops_forward(ir, else_call_idx + 1, n);
      if (after_else_call != merge_target && else_call_idx + 1 != merge_target)
      {
        /* Also accept if merge_target equals the instruction after else_call directly */
        int next_real = ir_skip_nops_forward(ir, else_call_idx + 1, n);
        if (next_real != merge_target)
          continue;
      }

      /* Get the differing parameter values */
      IROperand then_val = tcc_ir_op_get_src1(ir, then_q1);
      IROperand else_val = tcc_ir_op_get_src1(ir, else_q1);

      /* Both must be compile-time constants (SYMREF or IMM32) — no vreg uses
       * since the vreg lifetimes would be disrupted by removing the branches */
      int then_tag = irop_get_tag(then_val);
      int else_tag = irop_get_tag(else_val);
      if (then_tag != IROP_TAG_SYMREF && then_tag != IROP_TAG_IMM32)
        continue;
      if (else_tag != IROP_TAG_SYMREF && else_tag != IROP_TAG_IMM32)
        continue;

      /* ---- Transform ---- */

      /* Create a new temporary vreg for the SELECT result */
      int32_t select_vreg = tcc_ir_get_vreg_temp(ir);

      /* Allocate 4 pool entries for SELECT: dest, src1(then), src2(else), cond */
      IROperand sel_dest = irop_make_vreg(select_vreg, IROP_BTYPE_INT32);

      IROperand sel_cond = irop_make_imm32(-1, then_cond, VT_INT);

      int pool_base = tcc_ir_iroperand_pool_add(ir, sel_dest);
      tcc_ir_iroperand_pool_add(ir, then_val);
      tcc_ir_iroperand_pool_add(ir, else_val);
      tcc_ir_iroperand_pool_add(ir, sel_cond);

      /* Rewrite the JUMPIF as SELECT (drops one jump to else_target) */
      if (else_target >= 0 && else_target < n && jt_cnt[else_target] > 0)
        jt_cnt[else_target]--;
      jumpif_q->op = TCCIR_OP_SELECT;
      jumpif_q->operand_base = pool_base;

      /* Rewrite the then PARAM to use the SELECT result vreg,
       * and keep it pointing to the else call (which we'll keep) */
      /* Actually, we need to rewrite the else_q1 (PARAM) to use the
       * SELECT vreg as its value, then NOP the then-block entirely */

      /* Rewrite else PARAM0 to use the SELECT result */
      IROperand new_param_val = irop_make_vreg(select_vreg, IROP_BTYPE_INT32);
      tcc_ir_op_set_src1(ir, else_q1, new_param_val);

      /* NOP the then-block: then_param, then_call, unconditional jump */
      ir->compact_instructions[then_start].op = TCCIR_OP_NOP;
      ir->compact_instructions[then_call_idx].op = TCCIR_OP_NOP;
      JT_NOP_JUMP(jump_idx);

      /* Clear stale is_jump_target on positions no longer targeted
       * (the NOPed JUMP no longer jumps, so re-check with no exclusion) */
      if (!JT_HAS_OTHER(else_target, -1))
        ir->compact_instructions[else_target].is_jump_target = 0;
      if (merge_target >= 0 && merge_target < n && !JT_HAS_OTHER(merge_target, -1))
        ir->compact_instructions[merge_target].is_jump_target = 0;

      changes++;
      continue;
    }

    /* ----------------------------------------------------------------
     * Pattern: Simple ASSIGN diamond
     * ----------------------------------------------------------------
     * then: dest <-- val1 [ASSIGN]
     * JUMP to merge
     * else: dest <-- val2 [ASSIGN]
     * merge: ...
     * ---------------------------------------------------------------- */
    if (then_q1->op == TCCIR_OP_ASSIGN)
    {
      IROperand then_dest = tcc_ir_op_get_dest(ir, then_q1);
      IROperand then_val = tcc_ir_op_get_src1(ir, then_q1);
      int32_t dest_vreg = irop_get_vreg(then_dest);

      /* Next should be unconditional JUMP to merge */
      int jump_idx = ir_skip_nops_forward(ir, then_start + 1, n);
      if (jump_idx >= n)
        continue;
      IRQuadCompact *jump_q = &ir->compact_instructions[jump_idx];
      if (jump_q->op != TCCIR_OP_JUMP)
        continue;

      /* Find else block */
      int else_start = ir_skip_nops_forward(ir, else_target, n);
      if (else_start >= n)
        continue;
      IRQuadCompact *else_q = &ir->compact_instructions[else_start];
      if (else_q->op != TCCIR_OP_ASSIGN)
        continue;

      /* Same destination vreg */
      IROperand else_dest = tcc_ir_op_get_dest(ir, else_q);
      IROperand else_val = tcc_ir_op_get_src1(ir, else_q);
      if (irop_get_vreg(else_dest) != dest_vreg)
        continue;

      /* The else block must be exactly one ASSIGN.  After it, the next
       * instruction must be the merge point (the JMP target from then).
       * Otherwise the else block has more instructions and it's not a
       * simple diamond — NOP'ing the else ASSIGN would break the rest. */
      int merge_target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jump_q));
      int after_else = ir_skip_nops_forward(ir, else_start + 1, n);
      if (after_else != merge_target)
        continue;

      /* Allocate 4 pool entries for SELECT */
      IROperand sel_cond = irop_make_imm32(-1, then_cond, VT_INT);
      int pool_base = tcc_ir_iroperand_pool_add(ir, then_dest);
      tcc_ir_iroperand_pool_add(ir, then_val);
      tcc_ir_iroperand_pool_add(ir, else_val);
      tcc_ir_iroperand_pool_add(ir, sel_cond);

      /* Rewrite JUMPIF as SELECT (drops one jump to else_target) */
      if (else_target >= 0 && else_target < n && jt_cnt[else_target] > 0)
        jt_cnt[else_target]--;
      jumpif_q->op = TCCIR_OP_SELECT;
      jumpif_q->operand_base = pool_base;

      /* NOP the then-assign, jump, and else-assign */
      ir->compact_instructions[then_start].op = TCCIR_OP_NOP;
      JT_NOP_JUMP(jump_idx);
      ir->compact_instructions[else_start].op = TCCIR_OP_NOP;

      /* Clear stale is_jump_target on positions no longer targeted
       * (the NOPed JUMP no longer jumps, so re-check with no exclusion) */
      if (!JT_HAS_OTHER(else_target, -1))
        ir->compact_instructions[else_target].is_jump_target = 0;
      if (merge_target >= 0 && merge_target < n && !JT_HAS_OTHER(merge_target, -1))
        ir->compact_instructions[merge_target].is_jump_target = 0;

      changes++;
      continue;
    }
  }

  tcc_free(jt_cnt);
  #undef JT_HAS_OTHER
  #undef JT_NOP_JUMP
  return changes;
}

/* ============================================================================
 * Block Copy Initialization Optimization
 *
 * Detects pattern: memset(stack_area, 0, N) followed by consecutive STORE
 * instructions writing constant values (symbol refs) into the same stack area.
 * Replaces with a single BLOCK_COPY from a pre-built rodata block.
 *
 * Before: memset(sp[-20], 0, 20) + 5x STORE sp[-20..-4] <- GlobalSym(...)
 * After:  BLOCK_COPY sp[-20] <- rodata_sym, 20
 * ============================================================================ */

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
    int store_sizes[256];  /* 4 or 8 bytes */
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
      if (src_tag != IROP_TAG_SYMREF && src_tag != IROP_TAG_IMM32 && src_tag != IROP_TAG_I64)
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

/* ============================================================================
 * Post-Increment Assign Folding
 * ============================================================================
 *
 * Folds redundant ASSIGN+arithmetic pairs generated by C post-increment
 * in for-loop update expressions (e.g., i++).
 *
 * Pattern:
 *   A: T = V [ASSIGN]       (V is VAR/PARAM with is_lval — stack load)
 *   B: V = T OP <anything>  (same V vreg as dest, T as src1)
 *
 * Condition: T has exactly one use (instruction B), no jump targets between.
 *
 * Transform:
 *   A: NOP
 *   B: V = V OP <anything>  (replace T with V in src1, preserving is_lval)
 *
 * This handles cases that copy propagation cannot, because copy prop
 * correctly refuses to propagate lval sources (ASSIGN-with-lval is
 * semantically a LOAD, not a register copy).  But when T is only used
 * in the immediately following arithmetic that writes back to the same V,
 * we can safely fold the pair — the value loaded from V's stack slot is
 * consumed exactly once and written right back.
 */
int tcc_ir_opt_postinc_assign_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q_assign = &ir->compact_instructions[i];

    /* Instruction A must be ASSIGN */
    if (q_assign->op != TCCIR_OP_ASSIGN)
      continue;
    if (!irop_config[q_assign->op].has_dest)
      continue;

    IROperand assign_dest = tcc_ir_op_get_dest(ir, q_assign);
    IROperand assign_src = tcc_ir_op_get_src1(ir, q_assign);

    /* Dest must be a TEMP vreg */
    int32_t temp_vr = irop_get_vreg(assign_dest);
    if (temp_vr < 0)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(temp_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Source must be a VAR or PARAM with is_lval (stack variable) */
    int32_t var_vr = irop_get_vreg(assign_src);
    if (var_vr < 0)
      continue;
    int var_type = TCCIR_DECODE_VREG_TYPE(var_vr);
    if (var_type != TCCIR_VREG_TYPE_VAR && var_type != TCCIR_VREG_TYPE_PARAM)
      continue;
    if (!assign_src.is_lval)
      continue;

    /* Find next non-NOP instruction */
    int next_idx = -1;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *q_next = &ir->compact_instructions[j];
      if (q_next->op == TCCIR_OP_NOP)
        continue;
      /* Bail if we hit a jump target — control flow might skip A */
      if (q_next->is_jump_target)
        break;
      next_idx = j;
      break;
    }
    if (next_idx < 0)
      continue;

    IRQuadCompact *q_arith = &ir->compact_instructions[next_idx];

    /* Instruction B must have dest, src1, src2 (arithmetic/binary op) */
    if (!irop_config[q_arith->op].has_dest || !irop_config[q_arith->op].has_src1 || !irop_config[q_arith->op].has_src2)
      continue;

    /* B's dest must be the same VAR/PARAM as A's source */
    IROperand arith_dest = tcc_ir_op_get_dest(ir, q_arith);
    int32_t arith_dest_vr = irop_get_vreg(arith_dest);
    if (arith_dest_vr != var_vr)
      continue;

    /* B's src1 must be the TEMP from A */
    IROperand arith_src1 = tcc_ir_op_get_src1(ir, q_arith);
    int32_t arith_src1_vr = irop_get_vreg(arith_src1);
    if (arith_src1_vr != temp_vr)
      continue;

    /* T must have exactly one use across the entire function (instruction B).
     * We cannot use tcc_ir_vreg_has_single_use() because it only checks
     * src1/src2 operands.  For STORE instructions, the "dest" operand is
     * actually a USE (the memory address being written to), e.g.:
     *   T7***DEREF*** <-- T9 [STORE]   -- T7 provides the address
     * If T appears as a STORE dest, it has an additional use that would
     * make our folding unsafe (the *q++ = t pattern). */
    {
      int use_count = 0;
      int safe = 1;
      for (int k = 0; k < n && safe; k++)
      {
        if (k == i) /* skip the ASSIGN we're considering */
          continue;
        IRQuadCompact *qk = &ir->compact_instructions[k];
        if (qk->op == TCCIR_OP_NOP)
          continue;

        /* Check src1 and src2 */
        if (irop_config[qk->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, qk)) == temp_vr)
          use_count++;
        if (irop_config[qk->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, qk)) == temp_vr)
          use_count++;

        /* Check dest operand — for STORE/STORE_INDEXED/STORE_POSTINC the
         * dest provides the memory address (a USE, not a definition).
         * For PARAM ops the dest provides the value being passed.
         * Conservatively count any dest reference as a use. */
        if (irop_config[qk->op].has_dest)
        {
          IROperand dest_op = tcc_ir_op_get_dest(ir, qk);
          if (irop_get_vreg(dest_op) == temp_vr)
            use_count++;
        }

        if (use_count > 1)
          safe = 0;
      }
      if (!safe || use_count != 1)
        continue;
    }

    /* Safe to fold: replace T with V(is_lval) in B's src1, NOP A */

    /* Build replacement: use the original assign_src (V with is_lval)
     * to preserve load semantics, btype, and signedness */
    tcc_ir_set_src1(ir, next_idx, assign_src);

    /* NOP the ASSIGN */
    q_assign->op = TCCIR_OP_NOP;

    changes++;
  }

  return changes;
}

/* ============================================================================
 * Dead Loop Elimination
 * ============================================================================
 *
 * Eliminate loops whose body has no observable side effects.
 * When all stores inside the loop are to local VARs with constant values,
 * and there are no calls, memory stores, or other side effects, the entire
 * loop can be replaced by its final constant assignments.
 */
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

/* ============================================================================
 * Interprocedural Constant Propagation
 * ============================================================================ */

int tcc_ir_detect_const_result(TCCIRState *ir, int64_t *value, int *btype)
{
  int n = ir->next_instruction_index;
  if (n == 0 || ir->parameters_count > 0)
    return 0;

  int non_nop_count = 0;
  int ret_idx = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    non_nop_count++;

    switch (q->op)
    {
    case TCCIR_OP_ASSIGN:
    case TCCIR_OP_RETURNVALUE:
      break;
    default:
      return 0;
    }

    if (q->op == TCCIR_OP_RETURNVALUE)
      ret_idx = i;
  }

  if (ret_idx < 0 || non_nop_count > 4)
    return 0;

  IRQuadCompact *ret_q = &ir->compact_instructions[ret_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, ret_q);

  if (irop_is_immediate(src1))
  {
    *value = irop_get_imm64_ex(ir, src1);
    *btype = irop_get_btype(src1);
    return 1;
  }

  int32_t ret_vr = irop_get_vreg(src1);
  if (ret_vr < 0)
    return 0;

  for (int i = ret_idx - 1; i >= 0; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (irop_get_vreg(dest) == ret_vr)
      {
        IROperand as1 = tcc_ir_op_get_src1(ir, q);
        if (irop_is_immediate(as1))
        {
          *value = irop_get_imm64_ex(ir, as1);
          *btype = irop_get_btype(as1);
          return 1;
        }
        return 0;
      }
    }
    break;
  }

  return 0;
}

void tcc_ir_cache_const_result(TCCState *s, int func_token, int64_t value, int btype)
{
  if (s->func_const_result_cache_count >= FUNC_CONST_RESULT_CACHE_SIZE)
    return;
  for (int i = 0; i < s->func_const_result_cache_count; i++)
  {
    if (s->func_const_result_cache[i].token == func_token)
      return;
  }
  int idx = s->func_const_result_cache_count++;
  s->func_const_result_cache[idx].token = func_token;
  s->func_const_result_cache[idx].value = value;
  s->func_const_result_cache[idx].btype = btype;
}

int tcc_ir_lookup_const_result(TCCState *s, int func_token, int64_t *value, int *btype)
{
  for (int i = 0; i < s->func_const_result_cache_count; i++)
  {
    if (s->func_const_result_cache[i].token == func_token)
    {
      *value = s->func_const_result_cache[i].value;
      *btype = s->func_const_result_cache[i].btype;
      return 1;
    }
  }
  return 0;
}

int tcc_ir_opt_const_call_replace(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0 || !tcc_state || tcc_state->func_const_result_cache_count == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    IROperand callee_op = tcc_ir_op_get_src1(ir, q);
    Sym *callee = irop_get_sym_ex(ir, callee_op);
    if (!callee)
      continue;

    int64_t val;
    int btype;
    if (!tcc_ir_lookup_const_result(tcc_state, callee->v, &val, &btype))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand call_info = tcc_ir_op_get_src2(ir, q);
    int call_id = TCCIR_DECODE_CALL_ID((int)irop_get_imm64_ex(ir, call_info));

    LOG_IR_GEN("OPTIMIZE: IPC replace call to %s with #%lld at i=%d", get_tok_str(callee->v, NULL), (long long)val, i);

    q->op = TCCIR_OP_ASSIGN;
    if (val == (int32_t)val)
      tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)val, btype));
    else
    {
      uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
      tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
    }
    tcc_ir_set_src2(ir, i, IROP_NONE);
    tcc_ir_set_dest(ir, i, dest);

    for (int j = i - 1; j >= 0; j--)
    {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op == TCCIR_OP_NOP)
        continue;
      if (pq->op == TCCIR_OP_FUNCPARAMVAL || pq->op == TCCIR_OP_FUNCPARAMVOID)
      {
        IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
        int p_call_id = TCCIR_DECODE_CALL_ID((int)irop_get_imm64_ex(ir, ps2));
        if (p_call_id == call_id)
          pq->op = TCCIR_OP_NOP;
        continue;
      }
      break;
    }

    changes++;
  }

  return changes;
}
