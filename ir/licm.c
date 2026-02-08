/*
 *  TCC IR - Loop-Invariant Code Motion (LICM) Optimization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "licm.h"
#include "core.h"
#include "pool.h"
#include "vreg.h"
#include <string.h>

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* Check if an opcode is a branch/jump */
/* Comment out unused function for now */
#if 0
static int is_branch(int op)
{
  return op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF || op == TCCIR_OP_IJUMP;
}
#endif

/* Check if an operand is a stack offset (Addr[StackLoc[...]]) */
static int is_stack_addr_operand(TCCIRState *ir, IROperand *op)
{
  if (!op)
    return 0;
  return irop_get_tag(*op) == IROP_TAG_STACKOFF && !op->is_lval;
}

/* Check if instruction produces side effects - kept for future use */
#if 0
static int has_side_effects(int op)
{
  switch (op) {
  case TCCIR_OP_STORE:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
    return 1;
  default:
    return 0;
  }
}
#endif

/* ============================================================================
 * Loop Detection - Improved Version with Forward Jump Analysis
 * ============================================================================
 *
 * This uses a pattern-based approach to find loops:
 * 1. Look for backward jumps (JUMP to lower instruction index)
 * 2. The target is the loop header, the jump source is the latch
 * This handles simple while/for loops but not complex control flow.
 */

IRLoops *tcc_ir_detect_loops(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return NULL;

  IRLoops *loops = tcc_mallocz(sizeof(IRLoops));
  if (!loops)
    return NULL;

  loops->capacity = LICM_MAX_LOOPS;
  loops->loops = tcc_mallocz(sizeof(IRLoop) * loops->capacity);
  if (!loops->loops)
  {
    tcc_free(loops);
    return NULL;
  }

  /* Scan for backward jumps */
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_JUMP)
    {
      /* Get jump target */
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);

      /* Check if this is a backward jump (loop back edge) */
      if (target < i)
      {
        /* Found a loop */
        if (loops->num_loops >= loops->capacity)
        {
          fprintf(stderr, "[LICM] Warning: too many loops, skipping rest\n");
          break;
        }

        IRLoop *loop = &loops->loops[loops->num_loops];
        loop->header_idx = target;
        loop->start_idx = target;
        loop->end_idx = i;

        /* Find a valid preheader - walk backward from header to find a non-jump instruction
         * that falls through to the header, or that dominates the loop */
        int preheader = target - 1;
        while (preheader >= 0)
        {
          IRQuadCompact *ph = &ir->compact_instructions[preheader];
          if (ph->op != TCCIR_OP_JUMP && ph->op != TCCIR_OP_JUMPIF)
          {
            /* Found a non-jump instruction - this could be the preheader */
            break;
          }
          preheader--;
        }
        loop->preheader_idx = preheader;
        loop->depth = 1;

        /* Allocate body instructions array */
        int body_size = i - target + 1;
        loop->body_instrs_capacity = body_size;
        loop->body_instrs = tcc_mallocz(sizeof(int) * body_size);

        if (loop->body_instrs)
        {
          /* Fill body instructions from target to i (the range containing the loop) */
          for (int j = target; j <= i; j++)
          {
            loop->body_instrs[loop->num_body_instrs++] = j;
          }

          /* Also find instructions that are part of the loop body via forward jumps.
           * For a typical for/while loop, the body is often reached by a forward jump
           * from the header. We need to scan for JUMP instructions that go to targets
           * within the loop range but outside [target, i].
           */
          int max_idx = i;
          for (int j = target; j <= max_idx; j++)
          {
            IRQuadCompact *jq = &ir->compact_instructions[j];
            if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
            {
              IROperand jdest = tcc_ir_op_get_dest(ir, jq);
              int jtarget = (int)irop_get_imm64_ex(ir, jdest);
              /* If this jump targets somewhere after i but within reasonable range,
               * and that target eventually leads back to the loop header,
               * it might be part of the loop body.
               */
              if (jtarget > max_idx && jtarget < ir->next_instruction_index)
              {
                /* Check if from jtarget there's a path back to the header */
                /* For now, simply extend the body range if it's close */
                if (jtarget < target + 50) /* reasonable limit */
                {
                  max_idx = jtarget;
                }
              }
            }
          }

          /* If we found an extended range, reallocate and refill */
          if (max_idx > i)
          {
            int new_body_size = max_idx - target + 1;
            tcc_free(loop->body_instrs);
            loop->body_instrs = tcc_mallocz(sizeof(int) * new_body_size);
            loop->body_instrs_capacity = new_body_size;
            loop->num_body_instrs = 0;
            for (int j = target; j <= max_idx; j++)
            {
              loop->body_instrs[loop->num_body_instrs++] = j;
            }
          }

          loops->num_loops++;
        }
      }
    }
  }

#ifdef DEBUG_IR_GEN
  if (loops->num_loops > 0)
  {
    printf("[LICM] Detected %d loop(s)\n", loops->num_loops);
    for (int i = 0; i < loops->num_loops; i++)
    {
      printf("[LICM]   Loop %d: header=%d, start=%d, end=%d, preheader=%d, body_instrs=%d\n", i,
             loops->loops[i].header_idx, loops->loops[i].start_idx, loops->loops[i].end_idx,
             loops->loops[i].preheader_idx, loops->loops[i].num_body_instrs);
    }
  }
#endif

  return loops;
}

void tcc_ir_free_loops(IRLoops *loops)
{
  if (!loops)
    return;

  if (loops->loops)
  {
    for (int i = 0; i < loops->num_loops; i++)
    {
      if (loops->loops[i].body_instrs)
        tcc_free(loops->loops[i].body_instrs);
    }
    tcc_free(loops->loops);
  }

  tcc_free(loops);
}

int tcc_ir_is_in_loop(IRLoop *loop, int instr_idx)
{
  if (!loop)
    return 0;

  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    if (loop->body_instrs[i] == instr_idx)
      return 1;
  }
  return 0;
}

/* ============================================================================
 * Loop-Invariant Identification and Hoisting
 * ============================================================================ */

/*
 * Check if an operand is loop-invariant - kept for future use
 * For now, we focus on:
 * 1. Constants (always invariant)
 * 2. Stack addresses (invariant - frame pointer doesn't change)
 * 3. Variables defined outside the loop
 */
#if 0
static int is_loop_invariant_operand(TCCIRState *ir, IROperand *op, IRLoop *loop)
{
  if (!op)
    return 1;  /* NULL is trivially invariant */

  int tag = irop_get_tag(*op);

  /* Constants are always invariant */
  if (tag == IROP_TAG_IMM32 || tag == IROP_TAG_I64 ||
      tag == IROP_TAG_F32 || tag == IROP_TAG_F64 ||
      tag == IROP_TAG_SYMREF)
    return 1;

  /* Stack addresses are invariant (frame pointer is constant) */
  if (tag == IROP_TAG_STACKOFF && !op->is_lval)
    return 1;

  /* For virtual registers, check if defined outside loop */
  if (tag == IROP_TAG_VREG || irop_get_vreg(*op) >= 0) {
    int32_t vreg = irop_get_vreg(*op);
    if (vreg < 0)
      return 1;  /* No vreg, treat as invariant */

    /* Find where this vreg is defined */
    /* For now, assume it's invariant if we can't prove otherwise */
    /* A full implementation would track definitions */
    return 1;
  }

  return 1;  /* Conservative: assume invariant */
}
#endif

/* Comment out unused function - kept for future use with more general LICM */
#if 0
/*
 * Check if an instruction is a candidate for hoisting
 * We focus on ADD instructions where one operand is a stack address
 */
static int is_hoistable_instr(TCCIRState *ir, int instr_idx, IRLoop *loop)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];

  /* Only hoist pure computations */
  if (has_side_effects(q->op))
    return 0;

  /* Skip if already in a preheader (would be outside loop) */
  if (instr_idx < loop->start_idx)
    return 0;

  /* Focus on ADD with stack address operand */
  if (q->op == TCCIR_OP_ADD) {
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Check if either operand is a stack address */
    if (is_stack_addr_operand(ir, &src1) || is_stack_addr_operand(ir, &src2)) {
      /* Check if operands are invariant */
      if (is_loop_invariant_operand(ir, &src1, loop) &&
          is_loop_invariant_operand(ir, &src2, loop)) {
        return 1;
      }
    }
  }

  /* Also hoist stack address loads themselves */
  if (q->op == TCCIR_OP_LOAD) {
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (is_stack_addr_operand(ir, &src1)) {
      return 1;
    }
  }

  return 0;
}
#endif

/*
 * Insert an instruction before a given position
 * Returns index of inserted instruction, or -1 on failure
 */
static int insert_instruction_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q)
{
  /* Ensure we have space BEFORE inserting */
  if (ir->next_instruction_index + 1 >= ir->compact_instructions_size)
  {
    int new_size = ir->compact_instructions_size << 1;
    ir->compact_instructions = (IRQuadCompact *)tcc_realloc(ir->compact_instructions, sizeof(IRQuadCompact) * new_size);
    if (!ir->compact_instructions)
      tcc_error("compiler_error: failed to resize compact_instructions");
    ir->compact_instructions_size = new_size;
  }

  /* Make room by shifting instructions from the end */
  for (int i = ir->next_instruction_index; i > before_idx; i--)
  {
    ir->compact_instructions[i] = ir->compact_instructions[i - 1];
  }

  /* Insert new instruction */
  ir->compact_instructions[before_idx] = *new_q;
  ir->next_instruction_index++;

  /* Update jump targets that point to or after before_idx
   * All jumps targeting >= before_idx need to be incremented by 1 */
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= before_idx)
      {
        /* Update jump target - create new operand with incremented target */
        IROperand new_dest = irop_make_imm32(-1, target + 1, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }

  return before_idx;
}

/*
 * Create an ASSIGN instruction to copy a value
 * This properly allocates space in the operand pool
 */
static IRQuadCompact create_assign_instr(TCCIRState *ir, int32_t dest_vreg, IROperand src)
{
  IRQuadCompact q = {0};
  q.op = TCCIR_OP_ASSIGN;

  /* ASSIGN has dest (slot 0) and src1 (slot 1) */
  /* Allocate operand pool space for both operands */
  IROperand dest_op = irop_make_vreg(dest_vreg, IROP_BTYPE_INT32);

  /* Add operands to pool and set operand_base */
  q.operand_base = tcc_ir_pool_add(ir, dest_op); /* dest at base + 0 */
  tcc_ir_pool_add(ir, src);                      /* src1 at base + 1 */

  return q;
}

/* Forward declaration for constant expression hoisting */
static int hoist_const_exprs_from_loop(TCCIRState *ir, IRLoop *loop);

/* Check if a loop contains any function calls
 * We skip LICM for such loops because inserting instructions
 * messes up the call_id tracking for function parameters */
static int loop_contains_calls(TCCIRState *ir, IRLoop *loop)
{
  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    int instr_idx = loop->body_instrs[i];
    IRQuadCompact *q = &ir->compact_instructions[instr_idx];

    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCPARAMVAL ||
        q->op == TCCIR_OP_FUNCPARAMVOID || q->op == TCCIR_OP_CALLSEQ_BEGIN || q->op == TCCIR_OP_CALLSEQ_END)
    {
      return 1;
    }
  }
  return 0;
}

/* Check if a loop contains VLA (Variable Length Array) allocations.
 * VLA allocations have special stack semantics - they dynamically adjust SP
 * each iteration based on runtime-computed sizes. Hoisting function calls
 * that compute VLA sizes out of the loop corrupts the VLA stack management.
 *
 * Example: char buf[strlen(str) + 10] inside a loop
 * The strlen() call computes the VLA size and must execute at the VLA_ALLOC point.
 */
static int loop_contains_vla(TCCIRState *ir, IRLoop *loop)
{
  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    int instr_idx = loop->body_instrs[i];
    IRQuadCompact *q = &ir->compact_instructions[instr_idx];

    if (q->op == TCCIR_OP_VLA_ALLOC)
    {
      return 1;
    }
  }
  return 0;
}

/*
 * Hoist invariant instructions from a single loop
 * Strategy:
 * 1. First pass: Find all unique stack offsets used in the loop
 * 2. For each unique offset, create ONE hoisted ASSIGN instruction
 * 3. Second pass: Replace ALL uses of that stack offset with the hoisted vreg
 * Returns number of instructions hoisted
 */

/* Maximum number of unique stack offsets to hoist per loop */
#define MAX_HOISTED_OFFSETS 16

typedef struct
{
  int offset;           /* The stack offset */
  int is_param;         /* Whether it's a parameter */
  int32_t hoisted_vreg; /* The vreg holding the hoisted value */
  int hoisted;          /* Whether we've created the ASSIGN yet */
} HoistedStackAddr;

static int hoist_from_loop(TCCIRState *ir, IRLoop *loop)
{
  if (!ir || !loop || loop->preheader_idx < 0)
    return 0;

  /* Skip LICM for loops containing function calls because inserting
   * instructions breaks call_id tracking. Note: Pure function call hoisting
   * is handled separately in tcc_ir_hoist_pure_calls() which is called
   * BEFORE this function in tcc_ir_opt_licm().
   */
  if (loop_contains_calls(ir, loop))
  {
#ifdef DEBUG_IR_GEN
    printf("[LICM] Skipping stack address LICM for loop with function calls (header=%d)\n", loop->header_idx);
#endif
    return 0;
  }

  /* Try to hoist constant expressions (Phase 3 enhancement) */
  int const_hoisted = hoist_const_exprs_from_loop(ir, loop);

#ifdef DEBUG_IR_GEN
  printf("[LICM] hoist_from_loop: const_hoisted=%d, header=%d\n", const_hoisted, loop->header_idx);
#endif

  /* Collect unique stack address offsets used in the loop */
  HoistedStackAddr hoisted_addrs[MAX_HOISTED_OFFSETS];
  int num_hoisted_addrs = 0;

  /* First pass: Find all unique stack offsets */
  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    int instr_idx = loop->body_instrs[i];
    IRQuadCompact *q = &ir->compact_instructions[instr_idx];

    /* Skip non-ADD instructions for now */
    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    IROperand *stack_ops[2] = {NULL, NULL};
    int num_stack_ops = 0;

    if (is_stack_addr_operand(ir, &src1))
      stack_ops[num_stack_ops++] = &src1;
    if (is_stack_addr_operand(ir, &src2))
      stack_ops[num_stack_ops++] = &src2;

    for (int j = 0; j < num_stack_ops; j++)
    {
      IROperand *op = stack_ops[j];
      int offset = irop_get_stack_offset(*op);
      int is_param = op->is_param;

      /* Check if we already have this offset */
      int found = 0;
      for (int k = 0; k < num_hoisted_addrs; k++)
      {
        if (hoisted_addrs[k].offset == offset && hoisted_addrs[k].is_param == is_param)
        {
          found = 1;
          break;
        }
      }

      if (!found && num_hoisted_addrs < MAX_HOISTED_OFFSETS)
      {
        hoisted_addrs[num_hoisted_addrs].offset = offset;
        hoisted_addrs[num_hoisted_addrs].is_param = is_param;
        hoisted_addrs[num_hoisted_addrs].hoisted_vreg = -1;
        hoisted_addrs[num_hoisted_addrs].hoisted = 0;
        num_hoisted_addrs++;
      }
    }
  }

  if (num_hoisted_addrs == 0)
    return const_hoisted;

#ifdef DEBUG_IR_GEN
  printf("[LICM] Found %d unique stack address(es) to hoist\n", num_hoisted_addrs);
#endif

  /* Allocate vregs for all hoisted values */
  for (int i = 0; i < num_hoisted_addrs; i++)
  {
    hoisted_addrs[i].hoisted_vreg = tcc_ir_vreg_alloc_temp(ir);
    if (hoisted_addrs[i].hoisted_vreg < 0)
    {
      fprintf(stderr, "[LICM] Warning: failed to allocate vreg for offset %d\n", hoisted_addrs[i].offset);
      return 0;
    }
  }

  /* Insert ASSIGN instructions after the preheader (before the loop body)
   * We insert after the preheader instruction, so the hoisted code executes
   * before the loop starts. Insert from back to front to avoid index updates. */
  int insert_pos = loop->preheader_idx + 1; /* Insert AFTER preheader */
  int total_inserted = 0;

  for (int i = num_hoisted_addrs - 1; i >= 0; i--)
  {
    /* Create the source operand for the stack address
     * Args: vreg, offset, is_lval, is_llocal, is_param, btype */
    IROperand src_op =
        irop_make_stackoff(-1, hoisted_addrs[i].offset, 0, 0, hoisted_addrs[i].is_param, IROP_BTYPE_INT32);

    IRQuadCompact hoist_q = create_assign_instr(ir, hoisted_addrs[i].hoisted_vreg, src_op);

    int inserted_idx = insert_instruction_before(ir, insert_pos, &hoist_q);
    if (inserted_idx < 0)
    {
      fprintf(stderr, "[LICM] Warning: failed to insert instruction\n");
      continue;
    }

    hoisted_addrs[i].hoisted = 1;
    total_inserted++;

#ifdef DEBUG_IR_GEN
    printf("[LICM] Inserted hoist for offset %d at position %d (vreg %d)\n", hoisted_addrs[i].offset, inserted_idx,
           TCCIR_DECODE_VREG_POSITION(hoisted_addrs[i].hoisted_vreg));
#endif
  }

  /* Update loop body indices to account for inserted instructions */
  loop->header_idx += total_inserted;
  loop->start_idx += total_inserted;
  loop->end_idx += total_inserted;
  loop->preheader_idx += total_inserted;
  for (int j = 0; j < loop->num_body_instrs; j++)
  {
    loop->body_instrs[j] += total_inserted;
  }

  /* Second pass: Replace ALL uses of stack addresses with hoisted vregs */
  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    int instr_idx = loop->body_instrs[i];
    IRQuadCompact *q = &ir->compact_instructions[instr_idx];

    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Check and replace src1 */
    if (is_stack_addr_operand(ir, &src1))
    {
      int offset = irop_get_stack_offset(src1);
      int is_param = src1.is_param;

      for (int k = 0; k < num_hoisted_addrs; k++)
      {
        if (hoisted_addrs[k].offset == offset && hoisted_addrs[k].is_param == is_param && hoisted_addrs[k].hoisted)
        {
          IROperand new_op = irop_make_vreg(hoisted_addrs[k].hoisted_vreg, IROP_BTYPE_INT32);
          tcc_ir_op_set_src1(ir, q, new_op);
          break;
        }
      }
    }

    /* Check and replace src2 */
    if (is_stack_addr_operand(ir, &src2))
    {
      int offset = irop_get_stack_offset(src2);
      int is_param = src2.is_param;

      for (int k = 0; k < num_hoisted_addrs; k++)
      {
        if (hoisted_addrs[k].offset == offset && hoisted_addrs[k].is_param == is_param && hoisted_addrs[k].hoisted)
        {
          IROperand new_op = irop_make_vreg(hoisted_addrs[k].hoisted_vreg, IROP_BTYPE_INT32);
          tcc_ir_op_set_src2(ir, q, new_op);
          break;
        }
      }
    }
  }

#ifdef DEBUG_IR_GEN
  printf("[LICM] Replaced stack address operand(s) in loop body\n");
  printf("[LICM] hoist_from_loop returning: total_inserted=%d, const_hoisted=%d, sum=%d\n", total_inserted,
         const_hoisted, total_inserted + const_hoisted);
#endif

  return total_inserted + const_hoisted;
}

/* ============================================================================
 * Constant Expression Hoisting (Phase 3 Enhancement)
 * ============================================================================
 *
 * Hoist loop-invariant constant computations out of loops.
 * This handles patterns like:
 *   V0 <- #1234 [ASSIGN]       ; Constant assignment
 *   V0 <- V0 SUB #42           ; Constant arithmetic (result always 1192)
 *
 * These are identified as loop-invariant and moved to the pre-header.
 */

/* Forward declaration for loop-invariant operand check */
static int is_operand_loop_invariant_ex(TCCIRState *ir, IROperand op, IRLoop *loop, int32_t *hoisted_vregs,
                                        int num_hoisted_vregs);

/* Compare two IR operands for semantic equality (same vreg or same immediate value) */
static int operands_equal(TCCIRState *ir, IROperand a, IROperand b)
{
  int tag_a = irop_get_tag(a);
  int tag_b = irop_get_tag(b);
  if (tag_a != tag_b)
    return 0;
  if (tag_a == IROP_TAG_IMM32)
    return a.u.imm32 == b.u.imm32;
  /* For vreg operands, compare the encoded vreg value */
  int32_t vr_a = irop_get_vreg(a);
  int32_t vr_b = irop_get_vreg(b);
  if (vr_a >= 0 && vr_a == vr_b)
    return 1;
  return 0;
}

/* Search instructions [0..before_idx) for one with matching op and operands.
 * Returns the dest vreg of the matching instruction, or -1 if not found. */
static int32_t find_existing_expr(TCCIRState *ir, int before_idx, int op, IROperand src1, IROperand src2)
{
  for (int i = 0; i < before_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != op)
      continue;
    IROperand q_src1 = tcc_ir_op_get_src1(ir, q);
    IROperand q_src2 = tcc_ir_op_get_src2(ir, q);
    if (operands_equal(ir, src1, q_src1) && operands_equal(ir, src2, q_src2))
    {
      IROperand q_dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(q_dest);
      if (vr >= 0)
        return vr;
    }
  }
  return -1;
}

/* Maximum number of constant expressions to hoist per loop */
#define MAX_HOISTED_CONSTS 16

typedef struct
{
  int instr_idx;        /* Original instruction index in loop */
  int32_t dest_vreg;    /* Destination vreg */
  int is_hoisted;       /* Whether we've created the hoist yet */
  int32_t hoisted_vreg; /* The new vreg holding hoisted value */
} HoistedConstExpr;

/* Hoist constant expressions from a loop
 *
 * CONSERVATIVE IMPLEMENTATION:
 * Only hoist arithmetic operations with ALL constant operands (not just ASSIGN).
 * This avoids issues with stack-allocated local variables that get redefined each iteration.
 */
static int hoist_const_exprs_from_loop(TCCIRState *ir, IRLoop *loop)
{
  if (!ir || !loop || loop->preheader_idx < 0)
    return 0;

  /* Skip loops containing function calls */
  if (loop_contains_calls(ir, loop))
    return 0;

  /* Find loop-invariant constant expressions
   * Look for: Vx <- #const1 OP #const2 (arithmetic with constant operands)
   * NOT: Vx <- #const (simple assign) - this causes issues with stack locals
   */
  HoistedConstExpr hoisted_exprs[MAX_HOISTED_CONSTS];
  int num_hoisted = 0;

  for (int i = 0; i < loop->num_body_instrs && num_hoisted < MAX_HOISTED_CONSTS; i++)
  {
    int instr_idx = loop->body_instrs[i];
    IRQuadCompact *q = &ir->compact_instructions[instr_idx];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Only consider arithmetic operations (not ASSIGN) */
    switch (q->op)
    {
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    case TCCIR_OP_MUL:
    case TCCIR_OP_AND:
    case TCCIR_OP_OR:
    case TCCIR_OP_XOR:
    case TCCIR_OP_SHL:
    case TCCIR_OP_SHR:
    case TCCIR_OP_SAR:
      break;
    default:
      continue; /* Skip non-arithmetic operations */
    }

    /* Check if destination is a vreg (VAR or TEMP) */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    int dest_type = TCCIR_DECODE_VREG_TYPE(dest_vr);
    if (dest_type != TCCIR_VREG_TYPE_VAR && dest_type != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Check if ALL operands are loop-invariant (constants, parameters,
     * or vregs defined outside the loop). This is more general than
     * the previous irop_is_immediate() check, which missed cases like
     * P1 SUB #1 where P1 is a function parameter (loop-invariant). */
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    if (!is_operand_loop_invariant_ex(ir, src1, loop, NULL, 0) ||
        !is_operand_loop_invariant_ex(ir, src2, loop, NULL, 0))
      continue;

    /* Check that the destination is not redefined later in the loop */
    int can_hoist = 1;
    for (int j = i + 1; j < loop->num_body_instrs; j++)
    {
      int later_idx = loop->body_instrs[j];
      IRQuadCompact *later_q = &ir->compact_instructions[later_idx];
      if (later_q->op == TCCIR_OP_NOP)
        continue;
      IROperand later_dest = tcc_ir_op_get_dest(ir, later_q);
      int32_t later_dest_vr = irop_get_vreg(later_dest);
      if (irop_config[later_q->op].has_dest && later_dest_vr == dest_vr)
      {
        can_hoist = 0;
        break;
      }
    }

    if (can_hoist)
    {
      hoisted_exprs[num_hoisted].instr_idx = instr_idx;
      hoisted_exprs[num_hoisted].dest_vreg = dest_vr;
      hoisted_exprs[num_hoisted].is_hoisted = 0;
      hoisted_exprs[num_hoisted].hoisted_vreg = -1;
      num_hoisted++;
    }
  }

  if (num_hoisted == 0)
    return 0;

#ifdef DEBUG_IR_GEN
  printf("[LICM] Found %d constant expression(s) to hoist\n", num_hoisted);
#endif

  /* For each candidate, check if the same expression already exists before the loop
   * (e.g., hoisted by an outer loop). If so, reuse that vreg instead of hoisting again. */
  for (int i = 0; i < num_hoisted; i++)
  {
    int orig_idx = hoisted_exprs[i].instr_idx;
    IRQuadCompact *orig_q = &ir->compact_instructions[orig_idx];
    IROperand src1 = tcc_ir_op_get_src1(ir, orig_q);
    IROperand src2 = tcc_ir_op_get_src2(ir, orig_q);
    int32_t existing = find_existing_expr(ir, loop->preheader_idx + 1, orig_q->op, src1, src2);
    if (existing >= 0)
    {
      /* Reuse existing computation — mark with the existing vreg and flag as already hoisted
       * so we skip insertion but still replace the in-loop instruction with ASSIGN */
      hoisted_exprs[i].hoisted_vreg = existing;
      hoisted_exprs[i].is_hoisted = 1; /* skip insertion, but do replacement */
    }
  }

  /* Allocate new vregs for expressions that truly need hoisting */
  for (int i = 0; i < num_hoisted; i++)
  {
    if (hoisted_exprs[i].is_hoisted)
      continue; /* already has a reused vreg */
    hoisted_exprs[i].hoisted_vreg = tcc_ir_vreg_alloc_temp(ir);
    if (hoisted_exprs[i].hoisted_vreg < 0)
    {
      fprintf(stderr, "[LICM] Warning: failed to allocate vreg for hoisted expr\n");
      return 0;
    }
  }

  /* Insert hoisted instructions at preheader */
  int insert_pos = loop->preheader_idx + 1;
  int total_inserted = 0;

#ifdef DEBUG_IR_GEN
  printf("[LICM] hoist_const_exprs: loop preheader=%d, insert_pos=%d, header=%d, start=%d, end=%d\n",
         loop->preheader_idx, insert_pos, loop->header_idx, loop->start_idx, loop->end_idx);
#endif

  for (int i = num_hoisted - 1; i >= 0; i--)
  {
    /* Skip expressions that are reusing an already-existing computation */
    if (hoisted_exprs[i].is_hoisted)
      continue;

    /* Adjust index: previous insertions in this loop shifted all
     * instructions after insert_pos forward by total_inserted. */
    int orig_idx = hoisted_exprs[i].instr_idx + total_inserted;
    IRQuadCompact *orig_q = &ir->compact_instructions[orig_idx];

    /* Create a copy of the original instruction with NEW pool entries.
     * We must NOT share operand_base with the original, because
     * tcc_ir_op_set_dest modifies the pool directly, which would
     * corrupt the original instruction's operands. */
    IRQuadCompact hoist_q = {0};
    hoist_q.op = orig_q->op;

    /* Read original operands */
    IROperand orig_src1 = tcc_ir_op_get_src1(ir, orig_q);
    IROperand orig_src2 = tcc_ir_op_get_src2(ir, orig_q);

    /* Allocate new pool entries: dest, src1, src2 */
    IROperand new_dest = irop_make_vreg(hoisted_exprs[i].hoisted_vreg, IROP_BTYPE_INT32);
    hoist_q.operand_base = tcc_ir_pool_add(ir, new_dest);
    tcc_ir_pool_add(ir, orig_src1);
    tcc_ir_pool_add(ir, orig_src2);

    int inserted_idx = insert_instruction_before(ir, insert_pos, &hoist_q);
    if (inserted_idx < 0)
    {
      fprintf(stderr, "[LICM] Warning: failed to insert hoisted instruction\n");
      continue;
    }

    hoisted_exprs[i].is_hoisted = 1;
    total_inserted++;

#ifdef DEBUG_IR_GEN
    printf("[LICM] Hoisted instruction %d to position %d (vreg %d)\n", orig_idx, inserted_idx,
           TCCIR_DECODE_VREG_POSITION(hoisted_exprs[i].hoisted_vreg));
#endif
  }

  /* Update loop indices */
  loop->header_idx += total_inserted;
  loop->start_idx += total_inserted;
  loop->end_idx += total_inserted;
  loop->preheader_idx += total_inserted;
  for (int j = 0; j < loop->num_body_instrs; j++)
  {
    loop->body_instrs[j] += total_inserted;
  }

  /* Replace original instructions with ASSIGN from hoisted vreg */
  for (int i = 0; i < num_hoisted; i++)
  {
    if (!hoisted_exprs[i].is_hoisted)
      continue;

    int orig_idx = hoisted_exprs[i].instr_idx + total_inserted;
    if (orig_idx >= ir->next_instruction_index)
      continue;

    IRQuadCompact *orig_q = &ir->compact_instructions[orig_idx];

    /* Convert original to ASSIGN from hoisted vreg */
    orig_q->op = TCCIR_OP_ASSIGN;
    IROperand hoisted_src = irop_make_vreg(hoisted_exprs[i].hoisted_vreg, IROP_BTYPE_INT32);
    tcc_ir_set_src1(ir, orig_idx, hoisted_src);
    tcc_ir_set_src2(ir, orig_idx, IROP_NONE);

    /* Keep the original destination */
    IROperand orig_dest = irop_make_vreg(hoisted_exprs[i].dest_vreg, IROP_BTYPE_INT32);
    tcc_ir_op_set_dest(ir, orig_q, orig_dest);
  }

#ifdef DEBUG_IR_GEN
  printf("[LICM] Replaced original instruction(s) with ASSIGN\n");
#endif

  return total_inserted;
}

int tcc_ir_hoist_loop_invariants(TCCIRState *ir, IRLoops *loops)
{
  if (!ir || !loops)
    return 0;

  int total_hoisted = 0;

  for (int i = 0; i < loops->num_loops; i++)
  {
    IRLoop *loop = &loops->loops[i];
    int hoisted = hoist_from_loop(ir, loop);
    total_hoisted += hoisted;

    /* If we hoisted any instructions, update indices for all subsequent loops */
    if (hoisted > 0)
    {
#ifdef DEBUG_IR_GEN
      printf("[LICM] Loop %d hoisted %d instrs, loop[%d].preheader=%d, updating later loops\n", i, hoisted, i,
             loop->preheader_idx);
#endif
      /* Indices of subsequent loops need to be shifted by number of inserted instructions */
      for (int j = i + 1; j < loops->num_loops; j++)
      {
        IRLoop *later_loop = &loops->loops[j];

        /* Update loop boundary indices if they are after the insertion point */
        if (later_loop->header_idx >= loop->preheader_idx)
          later_loop->header_idx += hoisted;
        if (later_loop->start_idx >= loop->preheader_idx)
          later_loop->start_idx += hoisted;
        if (later_loop->end_idx >= loop->preheader_idx)
          later_loop->end_idx += hoisted;
        if (later_loop->preheader_idx >= loop->preheader_idx)
          later_loop->preheader_idx += hoisted;

        /* Update body instruction indices */
        for (int k = 0; k < later_loop->num_body_instrs; k++)
        {
          if (later_loop->body_instrs[k] >= loop->preheader_idx)
            later_loop->body_instrs[k] += hoisted;
        }
      }
    }
  }

  return total_hoisted;
}

/* ============================================================================
 * Pure Function Detection and LICM for Function Calls (Phase 1)
 * ============================================================================ */

/* Forward declarations from tccgen.c for token string lookup
 * get_tok_str is already declared in tcc.h */

/* Table of well-known pure functions (C standard library)
 * These are functions that have no side effects and depend only on arguments.
 * Format: { "func_name", purity_level }
 * purity_level: 2 = PURE, 3 = CONST
 */
static struct
{
  const char *name;
  int purity;
} pure_func_table[] = {
    /* String functions - PURE (read memory) */
    {"strlen", 2},
    {"strcmp", 2},
    {"strncmp", 2},
    {"strchr", 2},
    {"strrchr", 2},
    {"strstr", 2},
    {"strpbrk", 2},
    {"strcspn", 2},
    {"strspn", 2},

    /* Memory functions - PURE */
    {"memcmp", 2},
    {"memchr", 2},

    /* Math functions - CONST (no memory reads, pure computation) */
    {"abs", 3},
    {"labs", 3},
    {"llabs", 3},
    {"fabs", 3},
    {"fabsf", 3},
    {"sqrt", 3},
    {"sqrtf", 3},
    {"sin", 3},
    {"sinf", 3},
    {"cos", 3},
    {"cosf", 3},
    {"tan", 3},
    {"tanf", 3},
    {"atan", 3},
    {"atanf", 3},
    {"atan2", 3},
    {"atan2f", 3},
    {"exp", 3},
    {"expf", 3},
    {"log", 3},
    {"logf", 3},
    {"log10", 3},
    {"log10f", 3},
    {"pow", 3},
    {"powf", 3},
    {"ceil", 3},
    {"ceilf", 3},
    {"floor", 3},
    {"floorf", 3},
    {"round", 3},
    {"roundf", 3},
    {"fmod", 3},
    {"fmodf", 3},
    {"modf", 3},
    {"modff", 3},

    /* Character classification - CONST */
    {"isalpha", 3},
    {"isdigit", 3},
    {"isalnum", 3},
    {"isspace", 3},
    {"isupper", 3},
    {"islower", 3},
    {"isprint", 3},
    {"isgraph", 3},
    {"ispunct", 3},
    {"iscntrl", 3},
    {"isxdigit", 3},
    {"tolower", 3},
    {"toupper", 3},
};

#define NUM_PURE_FUNCS (sizeof(pure_func_table) / sizeof(pure_func_table[0]))
/* ============================================================================
 * Function Purity Cache and Inference (Automatic Purity Detection)
 * ============================================================================
 * This allows LICM to optimize calls to functions defined in the same TU
 * without requiring explicit __attribute__((pure)) annotations.
 */

/* Add function purity to cache */
void tcc_ir_cache_func_purity(TCCState *s, int func_token, TCCFuncPurity purity)
{
  if (!s || func_token < TOK_IDENT)
    return;

  /* Check if already in cache */
  for (int i = 0; i < s->func_purity_cache_count; i++)
  {
    if (s->func_purity_cache[i].token == func_token)
      return; /* Already cached */
  }

  if (s->func_purity_cache_count >= FUNC_PURITY_CACHE_SIZE)
    return; /* Cache full */

  s->func_purity_cache[s->func_purity_cache_count].token = func_token;
  s->func_purity_cache[s->func_purity_cache_count].purity = purity;
  s->func_purity_cache_count++;

#ifdef DEBUG_IR_GEN
  printf("[PURITY] Cached '%s' as %s\n", get_tok_str(func_token, NULL),
         purity == TCC_FUNC_PURITY_CONST  ? "CONST"
         : purity == TCC_FUNC_PURITY_PURE ? "PURE"
                                          : "IMPURE");
#endif
}

/* Lookup function purity from cache */
int tcc_ir_lookup_func_purity(TCCState *s, int func_token)
{
  if (!s || func_token < TOK_IDENT)
    return -1;

  for (int i = 0; i < s->func_purity_cache_count; i++)
  {
    if (s->func_purity_cache[i].token == func_token)
      return s->func_purity_cache[i].purity;
  }
  return -1; /* Not found */
}

/* Check if an operand refers to stack or parameter memory
 * Conservative: returns 0 for any address we can't prove is stack/param
 */
static int is_stack_or_param_addr(TCCIRState *ir, IROperand op)
{
  int tag = irop_get_tag(op);

  /* Stack offsets are always local */
  if (tag == IROP_TAG_STACKOFF)
    return 1;

  /* Immediate addresses (absolute) - conservative: not stack */
  if (tag == IROP_TAG_IMM32 || tag == IROP_TAG_I64)
    return 0;

  /* VREGs that hold stack addresses - conservative: return 0 */
  if (tag == IROP_TAG_VREG)
    return 0;

  /* Symbol references - could be global or static, not stack */
  if (tag == IROP_TAG_SYMREF)
    return 0;

  return 0; /* Conservative default */
}

/* Infer function purity by analyzing its IR
 * Called after IR generation for each function
 * Returns: TCC_FUNC_PURITY_CONST, TCC_FUNC_PURITY_PURE, or TCC_FUNC_PURITY_IMPURE
 */
TCCFuncPurity tcc_ir_infer_func_purity(TCCIRState *ir, Sym *func_sym)
{
  if (!ir || !func_sym)
    return TCC_FUNC_PURITY_IMPURE;

#ifdef DEBUG_IR_GEN
  /* Get function name for debugging */
  const char *func_name = get_tok_str(func_sym->v, NULL);
#endif

  int is_const = 1; /* Assume const until proven otherwise */

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    switch (q->op)
    {
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
      /* Store to non-stack memory → IMPURE */
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (!is_stack_or_param_addr(ir, dest))
        {
#ifdef DEBUG_IR_GEN
          printf("[PURITY] Function '%s' is IMPURE: stores to non-stack memory\n", func_name);
#endif
          return TCC_FUNC_PURITY_IMPURE;
        }
      }
      break;

    case TCCIR_OP_LOAD:
    case TCCIR_OP_LOAD_INDEXED:
      /* Load from non-stack/param → not CONST (could still be PURE) */
      {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        if (!is_stack_or_param_addr(ir, src))
        {
          is_const = 0;
        }
      }
      break;

    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
      /* Call to impure function → IMPURE */
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        Sym *callee = irop_get_sym_ex(ir, src1);
        if (callee)
        {
          /* Check callee purity - use attributes/table only, not cache
           * to avoid infinite recursion */
          int callee_purity = TCC_FUNC_PURITY_UNKNOWN;

          /* Check well-known pure functions table first */
          const char *callee_name = get_tok_str(callee->v, NULL);
          for (size_t j = 0; j < sizeof(pure_func_table) / sizeof(pure_func_table[0]); j++)
          {
            if (strcmp(callee_name, pure_func_table[j].name) == 0)
            {
              callee_purity = pure_func_table[j].purity;
              break;
            }
          }

          /* Check explicit attributes */
          if (callee_purity == TCC_FUNC_PURITY_UNKNOWN)
          {
            int func_pure = callee->f.func_pure;
            int func_const = callee->f.func_const;
            if (callee->type.ref)
            {
              func_pure |= callee->type.ref->f.func_pure;
              func_const |= callee->type.ref->f.func_const;
            }
            if (func_const)
              callee_purity = TCC_FUNC_PURITY_CONST;
            else if (func_pure)
              callee_purity = TCC_FUNC_PURITY_PURE;
          }

          if (callee_purity == TCC_FUNC_PURITY_IMPURE || callee_purity == TCC_FUNC_PURITY_UNKNOWN)
          {
#ifdef DEBUG_IR_GEN
            printf("[PURITY] Function '%s' is IMPURE: calls impure function '%s'\n", func_name, callee_name);
#endif
            return TCC_FUNC_PURITY_IMPURE;
          }
          if (callee_purity == TCC_FUNC_PURITY_PURE)
            is_const = 0;
        }
        else
        {
          /* Indirect call - can't determine purity, conservative: IMPURE */
#ifdef DEBUG_IR_GEN
          printf("[PURITY] Function '%s' is IMPURE: indirect call\n", func_name);
#endif
          return TCC_FUNC_PURITY_IMPURE;
        }
      }
      break;

    case TCCIR_OP_VLA_ALLOC:
      /* VLA allocation modifies stack in non-trivial way */
#ifdef DEBUG_IR_GEN
      printf("[PURITY] Function '%s' is IMPURE: VLA allocation\n", func_name);
#endif
      return TCC_FUNC_PURITY_IMPURE;

    default:
      break;
    }
  }

  TCCFuncPurity result = is_const ? TCC_FUNC_PURITY_CONST : TCC_FUNC_PURITY_PURE;
#ifdef DEBUG_IR_GEN
  printf("[PURITY] Function '%s' inferred as %s\n", func_name, result == TCC_FUNC_PURITY_CONST ? "CONST" : "PURE");
#endif
  return result;
}

/* Get function purity for a symbol
 * Returns TCC_FUNC_PURITY_UNKNOWN, TCC_FUNC_PURITY_IMPURE, TCC_FUNC_PURITY_PURE, or TCC_FUNC_PURITY_CONST
 */
int tcc_ir_get_func_purity(TCCIRState *ir, Sym *sym)
{
  if (!sym)
    return TCC_FUNC_PURITY_UNKNOWN;

  /* Check if this is a function */
  if (!(sym->type.t & VT_FUNC))
    return TCC_FUNC_PURITY_IMPURE; /* Not a function = not pure */

  /* Get function name from symbol */
  const char *func_name = get_tok_str(sym->v, NULL);
  if (!func_name)
    return TCC_FUNC_PURITY_UNKNOWN;

  /* Check both sym->f and sym->type.ref->f for attributes.
   * For function declarations, pure/const attributes are stored in
   * sym->type.ref->f (the function type symbol), not in sym->f.
   */
  int func_pure = sym->f.func_pure;
  int func_const = sym->f.func_const;
  int func_noreturn = sym->f.func_noreturn;

  /* Also check the function type symbol if available */
  if (sym->type.ref)
  {
    func_pure |= sym->type.ref->f.func_pure;
    func_const |= sym->type.ref->f.func_const;
    func_noreturn |= sym->type.ref->f.func_noreturn;
  }

#ifdef DEBUG_IR_GEN
  printf("[LICM] Checking purity for function '%s': func_pure=%d, func_const=%d\n", func_name, func_pure, func_const);
#endif

  /* Check well-known pure functions */
  for (size_t i = 0; i < NUM_PURE_FUNCS; i++)
  {
    if (strcmp(func_name, pure_func_table[i].name) == 0)
    {
#ifdef DEBUG_IR_GEN
      printf("[LICM] Found '%s' in pure function table with purity=%d\n", func_name, pure_func_table[i].purity);
#endif
      return pure_func_table[i].purity;
    }
  }

  /* Check function attributes from parsing */
  if (func_noreturn)
  {
    /* noreturn functions typically exit or loop forever - not pure */
    return TCC_FUNC_PURITY_IMPURE;
  }

  /* Check for explicit __attribute__((const)) - highest purity level */
  if (func_const)
  {
#ifdef DEBUG_IR_GEN
    printf("[LICM] Function '%s' has func_const attribute\n", func_name);
#endif
    return TCC_FUNC_PURITY_CONST;
  }

  /* Check for explicit __attribute__((pure)) */
  if (func_pure)
  {
#ifdef DEBUG_IR_GEN
    printf("[LICM] Function '%s' has func_pure attribute\n", func_name);
#endif
    return TCC_FUNC_PURITY_PURE;
  }

  /* Check purity cache for inferred purity from same-TU functions */
  /* Use tcc_state which is available through extern in tcc.h */
  extern TCCState *tcc_state;
  if (tcc_state)
  {
    int cached = tcc_ir_lookup_func_purity(tcc_state, sym->v);
    if (cached >= 0)
    {
#ifdef DEBUG_IR_GEN
      printf("[LICM] Found cached purity for '%s': %d\n", func_name, cached);
#endif
      return cached;
    }
  }

  /* Conservative default: unknown = IMPURE (can't hoist) */
#ifdef DEBUG_IR_GEN
  printf("[LICM] Function '%s' is unknown, marking as IMPURE\n", func_name);
#endif
  return TCC_FUNC_PURITY_IMPURE;
}

/* Check if an operand is loop-invariant
 * An operand is loop-invariant if:
 * 1. It's a constant (immediate)
 * 2. It's a vreg defined outside the loop
 * 3. It's a vreg defined by ASSIGN from another loop-invariant vreg (transitively invariant)
 *
 * The hoisted_vregs array contains vregs that were hoisted in previous iterations.
 * These are considered loop-invariant even if they have an ASSIGN in the loop body.
 */
static int is_operand_loop_invariant_ex(TCCIRState *ir, IROperand op, IRLoop *loop, int32_t *hoisted_vregs,
                                        int num_hoisted_vregs)
{
  /* Constants are always loop-invariant */
  if (irop_is_immediate(op))
    return 1;

  /* Check vreg - if defined inside loop, not invariant */
  int32_t vreg = irop_get_vreg(op);
  if (vreg < 0)
    return 1; /* No vreg = treat as invariant */

  /* Check if this vreg was already hoisted */
  for (int h = 0; h < num_hoisted_vregs; h++)
  {
    if (hoisted_vregs[h] == vreg)
      return 1; /* Already hoisted - loop invariant */
  }

  /* Find where this vreg is defined */
  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    int instr_idx = loop->body_instrs[i];
    IRQuadCompact *q = &ir->compact_instructions[instr_idx];

    if (!irop_config[q->op].has_dest)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) == vreg)
    {
      /* This vreg is defined inside the loop.
       * Check if it's an ASSIGN from a hoisted vreg (transitively invariant) */
      if (q->op == TCCIR_OP_ASSIGN)
      {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        int32_t src_vreg = irop_get_vreg(src);
        if (src_vreg >= 0)
        {
          for (int h = 0; h < num_hoisted_vregs; h++)
          {
            if (hoisted_vregs[h] == src_vreg)
              return 1; /* Assigned from hoisted vreg - loop invariant */
          }
        }
      }
      /* Otherwise not invariant */
      return 0;
    }
  }

  /* Vreg not defined in loop - it's loop-invariant */
  return 1;
}

/* Backward-compatible wrapper for is_operand_loop_invariant */
__attribute__((unused)) static int is_operand_loop_invariant(TCCIRState *ir, IROperand op, IRLoop *loop)
{
  return is_operand_loop_invariant_ex(ir, op, loop, NULL, 0);
}

/* Check if a function call instruction can be hoisted
 * Requirements:
 * 1. Function is pure or const
 * 2. All arguments are loop-invariant (considering already-hoisted vregs)
 */
static int tcc_ir_is_hoistable_call_ex(TCCIRState *ir, int instr_idx, IRLoop *loop, int32_t *hoisted_vregs,
                                       int num_hoisted_vregs)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];

  if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
    return 0; /* Not a function call */

  /* For FUNCCALLVAL, the destination must be a vreg (not a memory location).
   * If the result is stored directly to a global/local variable, we can't
   * simply hoist it because we'd need to also handle the store operation.
   * This fixes a bug where hoisting a call with memory destination corrupted
   * the IR by treating the memory operand as a vreg. */
  if (q->op == TCCIR_OP_FUNCCALLVAL)
  {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(dest) != IROP_TAG_VREG)
    {
#ifdef DEBUG_IR_GEN
      printf("[LICM] Call at %d: destination is not a vreg, can't hoist\n", instr_idx);
#endif
      return 0;
    }
  }

  /* Get function symbol from src1 */
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  Sym *func_sym = irop_get_sym_ex(ir, src1);

  if (!func_sym)
  {
    /* Indirect call - can't determine purity */
#ifdef DEBUG_IR_GEN
    printf("[LICM] Call at %d: indirect call, can't hoist\n", instr_idx);
#endif
    return 0;
  }

  /* Check function purity */
  int purity = tcc_ir_get_func_purity(ir, func_sym);
  if (purity < TCC_FUNC_PURITY_PURE)
  {
    /* Function has side effects or is unknown - can't hoist */
    return 0;
  }

#ifdef DEBUG_IR_GEN
  printf("[LICM] Call at %d: function is pure (purity=%d), checking args...\n", instr_idx, purity);
#endif

  /* Find all FUNCPARAMVAL instructions for this call */
  IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
  int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, call_src2));

  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    int param_idx = loop->body_instrs[i];
    IRQuadCompact *param_q = &ir->compact_instructions[param_idx];

    if (param_q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;

    IROperand param_src2 = tcc_ir_op_get_src2(ir, param_q);
    int param_call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, param_src2));
    if (param_call_id != call_id)
      continue; /* Parameter for a different call */

    /* Check if the parameter value is loop-invariant (considering hoisted vregs) */
    IROperand param_src = tcc_ir_op_get_src1(ir, param_q);
    if (!is_operand_loop_invariant_ex(ir, param_src, loop, hoisted_vregs, num_hoisted_vregs))
    {
      return 0; /* Argument not loop-invariant */
    }
  }

  /* Function is pure and all arguments are loop-invariant - can hoist */
  return 1;
}

/* Backward-compatible wrapper */
__attribute__((unused)) int tcc_ir_is_hoistable_call(TCCIRState *ir, int instr_idx, IRLoop *loop)
{
  return tcc_ir_is_hoistable_call_ex(ir, instr_idx, loop, NULL, 0);
}

/* Maximum number of pure calls to hoist per loop */
#define MAX_HOISTABLE_CALLS 16

typedef struct
{
  int instr_idx;        /* Index of FUNCCALLVAL/CALLVOID instruction */
  int32_t hoisted_vreg; /* New vreg for hoisted result (if VAL) */
  int is_hoisted;
} HoistableCallInfo;

/* Collect all FUNCPARAMVAL instructions belonging to a call */
static int collect_call_params(TCCIRState *ir, int call_idx, int *param_indices, int max_params)
{
  IRQuadCompact *call_q = &ir->compact_instructions[call_idx];
  IROperand call_src2 = tcc_ir_op_get_src2(ir, call_q);
  int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, call_src2));
  int num_params = 0;

  /* Scan all instructions for params with matching call_id */
  for (int i = 0; i < ir->next_instruction_index && num_params < max_params; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int param_call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
      if (param_call_id == call_id)
      {
        param_indices[num_params++] = i;
      }
    }
  }

  return num_params;
}

/* Hoist pure function calls from loops
 * This is Phase 1 of FUNCTION_CALLS_OPTIMIZATION_PLAN
 */
int tcc_ir_hoist_pure_calls(TCCIRState *ir, IRLoops *loops)
{
  if (!ir || !loops)
    return 0;

  int total_hoisted = 0;

  for (int loop_idx = 0; loop_idx < loops->num_loops; loop_idx++)
  {
    IRLoop *loop = &loops->loops[loop_idx];

    if (loop->preheader_idx < 0)
      continue; /* No preheader - can't hoist */

    /* Skip loops whose preheader is inside another loop's body.
     * This prevents hoisting INTO an enclosing loop instead of BEFORE it.
     *
     * Example of problematic pattern (for loop with body after increment):
     *   3: CMP i,5         <- outer loop header
     *   4: JMPIF exit
     *   5: JMP body (9)
     *   6: NOP             <- "inner loop" header (fake)
     *   7: i++
     *   8: JMP 3           <- outer loop back edge
     *   9: strlen()        <- loop body (hoistable)
     *  12: JMP 6           <- back edge detected as "inner loop"
     *
     * The "inner loop" (6-12) has preheader=3, but 3 is the OUTER loop's header!
     * Hoisting to preheader+1=4 places code INSIDE the outer loop.
     */
    int preheader_in_other_loop = 0;
    for (int other_idx = 0; other_idx < loops->num_loops; other_idx++)
    {
      if (other_idx == loop_idx)
        continue;
      IRLoop *other = &loops->loops[other_idx];
      /* Check if this loop's preheader is inside another loop's range */
      if (loop->preheader_idx >= other->start_idx && loop->preheader_idx <= other->end_idx)
      {
        preheader_in_other_loop = 1;
        break;
      }
    }
    if (preheader_in_other_loop)
      continue;

    /* Skip loops containing VLA allocations.
     * VLAs have special stack semantics - the size is computed at runtime
     * and SP is adjusted dynamically. Hoisting a pure function call that
     * computes the VLA size (e.g., strlen() in "char buf[strlen(s)+1]")
     * breaks the VLA stack management because the call must execute at
     * the point of VLA_ALLOC, not in the preheader.
     *
     * Test case: 123_vla_bug.c - has VLA inside switch/case in a for loop.
     */
    if (loop_contains_vla(ir, loop))
    {
#ifdef DEBUG_IR_GEN
      printf("[LICM] Skipping loop %d with VLA allocations\n", loop_idx);
#endif
      continue;
    }

    /* Iterative pure call hoisting:
     * Some calls may become hoistable after we hoist earlier calls in the chain.
     * For example: result = func_a(100); result = func_b(result);
     * Initially only func_a(100) is hoistable. After hoisting it, func_b(result)
     * becomes hoistable because 'result' is now defined outside the loop.
     *
     * We iterate until no more calls can be hoisted.
     */

    /* Collect ALL pure function calls in this loop (for iterative checking) */
    int all_call_indices[MAX_HOISTABLE_CALLS];
    int num_all_calls = 0;

#ifdef DEBUG_IR_GEN
    printf("[LICM] Scanning loop %d with %d body instructions for pure calls\n", loop_idx, loop->num_body_instrs);
#endif

    for (int i = 0; i < loop->num_body_instrs && num_all_calls < MAX_HOISTABLE_CALLS; i++)
    {
      int instr_idx = loop->body_instrs[i];
      IRQuadCompact *q = &ir->compact_instructions[instr_idx];

      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        /* Check basic requirements (pure function, vreg dest) but NOT argument invariance yet */
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        Sym *func_sym = irop_get_sym_ex(ir, src1);
        if (func_sym && tcc_ir_get_func_purity(ir, func_sym) >= TCC_FUNC_PURITY_PURE)
        {
          if (q->op == TCCIR_OP_FUNCCALLVOID ||
              (q->op == TCCIR_OP_FUNCCALLVAL && irop_get_tag(tcc_ir_op_get_dest(ir, q)) == IROP_TAG_VREG))
          {
            all_call_indices[num_all_calls++] = instr_idx;
          }
        }
      }
    }

    if (num_all_calls == 0)
    {
#ifdef DEBUG_IR_GEN
      printf("[LICM] No pure calls found in loop %d\n", loop_idx);
#endif
      continue;
    }

    /* Track hoisted vregs for iterative detection.
     * This maps hoisted_vreg -> original_vreg so we can detect transitively invariant operands.
     * We store both the hoisted vreg (defined in preheader) and the original vreg (now assigned
     * from hoisted vreg in loop body). */
    int32_t hoisted_vregs[MAX_HOISTABLE_CALLS];
    int num_hoisted_vregs = 0;
    int hoisted_call_flags[MAX_HOISTABLE_CALLS] = {0}; /* Track which calls have been hoisted */

    /* Iterative hoisting loop */
    int hoisted_this_iteration;
    do
    {
      hoisted_this_iteration = 0;

#ifdef DEBUG_IR_GEN
      printf("[LICM] Iteration: checking %d pure calls\n", num_all_calls);
#endif

      /* Find hoistable function calls in this loop */
      HoistableCallInfo hoistable[MAX_HOISTABLE_CALLS];
      int num_hoistable = 0;

      for (int i = 0; i < num_all_calls && num_hoistable < MAX_HOISTABLE_CALLS; i++)
      {
        if (hoisted_call_flags[i])
          continue; /* Already hoisted */

        int instr_idx = all_call_indices[i];
        IRQuadCompact *q = &ir->compact_instructions[instr_idx];

        /* Skip if already NOP'd (from previous hoisting) */
        if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_ASSIGN)
          continue;

#ifdef DEBUG_IR_GEN
        printf("[LICM] Found call at instruction %d, checking hoistability...\n", instr_idx);
#endif
        if (tcc_ir_is_hoistable_call_ex(ir, instr_idx, loop, hoisted_vregs, num_hoisted_vregs))
        {
          hoistable[num_hoistable].instr_idx = instr_idx;
          hoistable[num_hoistable].hoisted_vreg = -1;
          hoistable[num_hoistable].is_hoisted = 0;
          num_hoistable++;
          hoisted_call_flags[i] = 1; /* Mark as will-be-hoisted */
        }
      }

      if (num_hoistable == 0)
      {
#ifdef DEBUG_IR_GEN
        printf("[LICM] No more hoistable pure calls found in loop %d\n", loop_idx);
#endif
        break;
      }

#ifdef DEBUG_IR_GEN
      printf("[LICM] Found %d hoistable pure call(s) in loop %d\n", num_hoistable, loop_idx);
#endif

      /* For each hoistable call, we need to:
       * 1. Allocate a NEW call_id for the hoisted call (critical!)
       * 2. Create new vregs for results (if FUNCCALLVAL)
       * 3. Copy PARAM instructions to preheader with NEW call_id
       * 4. Copy CALL instruction to preheader with NEW call_id
       * 5. Replace original call with ASSIGN from hoisted vreg (or NOP for void)
       * 6. NOP out original parameters
       */

      /* Allocate vregs for results */
      for (int i = 0; i < num_hoistable; i++)
      {
        IRQuadCompact *call_q = &ir->compact_instructions[hoistable[i].instr_idx];
        if (call_q->op == TCCIR_OP_FUNCCALLVAL)
        {
          hoistable[i].hoisted_vreg = tcc_ir_vreg_alloc_temp(ir);
        }
      }

      /* Process each hoistable call */
      for (int i = 0; i < num_hoistable; i++)
      {
        int call_idx = hoistable[i].instr_idx;

        /* Get old call_id and argc from the original call */
        IRQuadCompact *orig_call_q = &ir->compact_instructions[call_idx];
        IROperand orig_call_src2 = tcc_ir_op_get_src2(ir, orig_call_q);
        int64_t orig_encoded = irop_get_imm64_ex(ir, orig_call_src2);
        int argc = TCCIR_DECODE_CALL_ARGC(orig_encoded);

        /* Allocate a NEW call_id for the hoisted call */
        int new_call_id = ir->next_call_id++;

        /* Collect all parameters for this call BEFORE any insertions */
        int param_indices[16];
        int num_params = collect_call_params(ir, call_idx, param_indices, 16);

        /* Count insertions for this call to track index shifts */
        int insertions_this_call = 0;

        /* Copy and modify the call instruction */
        IRQuadCompact call_copy = *orig_call_q;

        /* Copy parameter instructions */
        IRQuadCompact param_copies[16];
        for (int p = 0; p < num_params; p++)
        {
          param_copies[p] = ir->compact_instructions[param_indices[p]];
        }

        /* We insert instructions at preheader+1, and each insertion shifts
         * subsequent instructions. To get the correct order (PARAM, PARAM, ..., CALL),
         * we insert in REVERSE order: first CALL, then params from last to first.
         * This way:
         *   Insert CALL at preheader+1 -> [CALL]
         *   Insert PARAM[n-1] at preheader+1 -> [PARAM[n-1], CALL]
         *   Insert PARAM[0] at preheader+1 -> [PARAM[0], ..., PARAM[n-1], CALL]
         */

        /* First, insert the CALL instruction (it will end up LAST) */
        int64_t new_call_encoded = TCCIR_ENCODE_CALL(new_call_id, argc);
        IROperand new_call_src2 = irop_make_imm32(-1, (int32_t)new_call_encoded, IROP_BTYPE_INT32);

        /* Reallocate operand pool for call copy with updated call_id */
        IROperand call_dest = tcc_ir_op_get_dest(ir, &call_copy);
        IROperand call_src1 = tcc_ir_op_get_src1(ir, &call_copy);

        if (hoistable[i].hoisted_vreg >= 0)
        {
          /* Update destination to use hoisted vreg */
          call_dest = irop_make_vreg(hoistable[i].hoisted_vreg, IROP_BTYPE_INT32);
        }

        call_copy.operand_base = tcc_ir_pool_add(ir, call_dest);
        tcc_ir_pool_add(ir, call_src1);
        tcc_ir_pool_add(ir, new_call_src2);

        insert_instruction_before(ir, loop->preheader_idx + 1, &call_copy);
        insertions_this_call++;
        total_hoisted++;

        /* Now insert parameters from LAST to FIRST (they will end up in correct order) */
        for (int p = num_params - 1; p >= 0; p--)
        {
          /* Get param_idx from the original encoding */
          IROperand orig_param_src2 = tcc_ir_op_get_src2(ir, &param_copies[p]);
          int64_t orig_param_encoded = irop_get_imm64_ex(ir, orig_param_src2);
          int param_idx = TCCIR_DECODE_PARAM_IDX(orig_param_encoded);

          /* Create new encoding with new_call_id but same param_idx */
          int64_t new_param_encoded = TCCIR_ENCODE_PARAM(new_call_id, param_idx);
          IROperand new_param_src2 = irop_make_imm32(-1, (int32_t)new_param_encoded, IROP_BTYPE_INT32);

          /* Allocate operands in pool according to irop_config for FUNCPARAMVAL:
           * has_dest=0, has_src1=1, has_src2=1
           * So operands are: src1 at base+0, src2 at base+1 (NO dest!) */
          int new_operand_base = tcc_ir_pool_add(ir, tcc_ir_op_get_src1(ir, &param_copies[p]));
          tcc_ir_pool_add(ir, new_param_src2);
          param_copies[p].operand_base = new_operand_base;

          insert_instruction_before(ir, loop->preheader_idx + 1, &param_copies[p]);
          insertions_this_call++;
          total_hoisted++;
        }

        /* Update indices to account for inserted instructions */
        int adjusted_call_idx = call_idx + insertions_this_call;

        /* Replace original call with ASSIGN from hoisted vreg or NOP */
        IRQuadCompact *call_q = &ir->compact_instructions[adjusted_call_idx];
        if (hoistable[i].hoisted_vreg >= 0)
        {
          /* Get original destination from the adjusted position */
          IROperand orig_dest = tcc_ir_op_get_dest(ir, call_q);
          int32_t orig_vreg = irop_get_vreg(orig_dest);

          call_q->op = TCCIR_OP_ASSIGN;
          IROperand hoisted_src = irop_make_vreg(hoistable[i].hoisted_vreg, IROP_BTYPE_INT32);
          tcc_ir_set_src1(ir, adjusted_call_idx, hoisted_src);
          tcc_ir_set_src2(ir, adjusted_call_idx, IROP_NONE);
          /* Keep original destination */
          tcc_ir_op_set_dest(ir, call_q, irop_make_vreg(orig_vreg, IROP_BTYPE_INT32));

          /* Track the hoisted vreg for iterative detection.
           * Store the hoisted vreg so that subsequent calls using it as argument
           * can see that it's loop-invariant. */
          if (num_hoisted_vregs < MAX_HOISTABLE_CALLS)
          {
            hoisted_vregs[num_hoisted_vregs++] = hoistable[i].hoisted_vreg;
          }
          hoisted_this_iteration++;
        }
        else
        {
          /* VOID call - just mark as NOP */
          call_q->op = TCCIR_OP_NOP;
          hoisted_this_iteration++;
        }

        /* Mark original parameters as NOP (indices are shifted by insertions_this_call) */
        for (int p = 0; p < num_params; p++)
        {
          int adjusted_param_idx = param_indices[p] + insertions_this_call;
          ir->compact_instructions[adjusted_param_idx].op = TCCIR_OP_NOP;
        }

        /* Update hoistable indices for remaining calls in this loop */
        for (int j = i + 1; j < num_hoistable; j++)
        {
          if (hoistable[j].instr_idx >= loop->preheader_idx + 1)
          {
            hoistable[j].instr_idx += insertions_this_call;
          }
        }

        hoistable[i].is_hoisted = 1;

#ifdef DEBUG_IR_GEN
        printf("[LICM] Hoisted pure call at instruction %d (new call_id=%d)\n", call_idx, new_call_id);
#endif

        /* Update all_call_indices for remaining calls - they shifted by insertions_this_call */
        for (int j = 0; j < num_all_calls; j++)
        {
          if (!hoisted_call_flags[j] && all_call_indices[j] > call_idx)
          {
            all_call_indices[j] += insertions_this_call;
          }
        }
      }

    } while (hoisted_this_iteration > 0);

    /* Update loop indices for subsequent loops */
    if (total_hoisted > 0)
    {
      for (int j = loop_idx + 1; j < loops->num_loops; j++)
      {
        IRLoop *later_loop = &loops->loops[j];
        if (later_loop->start_idx >= loop->preheader_idx)
          later_loop->start_idx += total_hoisted;
        if (later_loop->end_idx >= loop->preheader_idx)
          later_loop->end_idx += total_hoisted;
        if (later_loop->preheader_idx >= loop->preheader_idx)
          later_loop->preheader_idx += total_hoisted;
        for (int k = 0; k < later_loop->num_body_instrs; k++)
        {
          if (later_loop->body_instrs[k] >= loop->preheader_idx)
            later_loop->body_instrs[k] += total_hoisted;
        }
      }

      /* Update this loop's indices too */
      loop->header_idx += total_hoisted;
      loop->start_idx += total_hoisted;
      for (int k = 0; k < loop->num_body_instrs; k++)
      {
        loop->body_instrs[k] += total_hoisted;
      }
    }
  }

  return total_hoisted;
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================ */

int tcc_ir_opt_licm(TCCIRState *ir)
{
  IRLoops *loops = tcc_ir_opt_licm_ex(ir);
  int hoisted = loops ? loops->num_loops : 0; /* non-zero if loops exist */
  tcc_ir_free_loops(loops);
  return hoisted;
}

IRLoops *tcc_ir_opt_licm_ex(TCCIRState *ir)
{
  if (!ir)
    return NULL;

#ifdef DEBUG_IR_GEN
  printf("[LICM] Starting loop-invariant code motion\n");
#endif

  /* Step 1: Detect loops */
  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
#ifdef DEBUG_IR_GEN
    printf("[LICM] No loops found\n");
#endif
    tcc_ir_free_loops(loops);
    return NULL;
  }

  /* Step 2: Hoist pure function calls FIRST (FUNCTION_CALLS_OPTIMIZATION_PLAN Phase 1)
   *
   * Pure function hoisting is done before general LICM so that pure calls
   * inside loops can be hoisted even if the loop contains other non-pure calls
   * that would normally block LICM.
   *
   * Note: Loops containing VLA_ALLOC instructions are automatically skipped
   * because VLAs have special stack semantics - the size computation must
   * happen at the VLA allocation point, not in the preheader.
   */
  int hoisted_calls = tcc_ir_hoist_pure_calls(ir, loops);
  (void)hoisted_calls;
  /* Step 3: Hoist other invariant instructions (stack addresses, constants) */
  int hoisted = tcc_ir_hoist_loop_invariants(ir, loops);
  (void)hoisted;
#ifdef DEBUG_IR_GEN
  hoisted += hoisted_calls;
  printf("[LICM] Hoisted %d instruction(s) and %d pure call(s)\n", hoisted - hoisted_calls, hoisted_calls);
#endif

  return loops;
}
