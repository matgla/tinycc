/*
 *  TCC IR - Value Materialization Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include <stdbool.h>

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/* Require non-null result carrier */
static void mat_require_result(void *ptr, const char *what)
{
  if (!ptr)
    tcc_error("compiler_error: %s requires a non-null result carrier", what);
}

/* Get stack slot for SValue materialization */
static const TCCStackSlot *mat_slot_sv(const TCCIRState *ir, const SValue *sv)
{
  if (!ir || !sv)
    return NULL;
  if (!tcc_ir_vreg_is_valid((TCCIRState *)ir, sv->vr))
    return NULL;
  return tcc_ir_stack_slot_by_vreg(ir, sv->vr);
}

/* Get frame offset for SValue materialization */
static int mat_offset_sv(const TCCIRState *ir, const SValue *sv)
{
  const TCCStackSlot *slot = mat_slot_sv(ir, sv);
  if (slot)
    return slot->offset;
  return sv ? sv->c.i : 0;
}

/* Get stack slot for IROperand materialization */
static const TCCStackSlot *mat_slot_op(const TCCIRState *ir, const IROperand *op)
{
  if (!ir || !op)
    return NULL;
  const int vreg = irop_get_vreg(*op);
  if (!tcc_ir_vreg_is_valid((TCCIRState *)ir, vreg))
    return NULL;
  return tcc_ir_stack_slot_by_vreg(ir, vreg);
}

/* Get frame offset for IROperand materialization */
static int mat_offset_op(const TCCIRState *ir, const IROperand *op)
{
  const TCCStackSlot *slot = mat_slot_op(ir, op);
  if (slot)
    return slot->offset;
  return op ? (int)irop_get_imm64_ex(ir, *op) : 0;
}

/* ============================================================================
 * SValue Materialization
 * ============================================================================ */

void tcc_ir_materialize_value(TCCIRState *ir, SValue *sv, TCCMaterializedValue *result)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !sv)
    return;

  if ((sv->r & VT_PARAM) && ((sv->r & VT_VALMASK) == VT_LOCAL))
  {
    /* Stack-passed parameters live in the caller frame. Leave them as VT_PARAM
     * lvalues so the backend can read directly from the caller stack. */
    sv->pr0_reg = PREG_REG_NONE;
    sv->pr0_spilled = 0;
    sv->pr1_reg = PREG_REG_NONE;
    sv->pr1_spilled = 0;
    return;
  }

  /* Register parameters (VT_PARAM with vreg, not on stack) have VT_LVAL set
   * to allow taking their address. But when materializing the VALUE, we need to
   * clear VT_LVAL since the register already holds the value, not a pointer. */
  if ((sv->r & VT_PARAM) && (sv->r & VT_LVAL))
  {
    const int val_kind = sv->r & VT_VALMASK;
    if (val_kind != VT_LOCAL && val_kind != VT_LLOCAL)
    {
      /* Register parameter - clear VT_LVAL since it's already a value */
      sv->r &= ~VT_LVAL;
    }
  }

  const int val_kind = sv->r & VT_VALMASK;
  const int is_64bit = tcc_ir_type_is_64bit(sv->type.t);
  const unsigned scratch_flags =
      (is_64bit ? TCC_MACHINE_SCRATCH_NEEDS_PAIR : 0) | (ir ? ir->codegen_materialize_scratch_flags : 0);

  /* Check for spilled values - this is the original materialization path */
  if (!sv->pr0_spilled)
  {
    return;
  }
  if (!tcc_ir_vreg_is_valid(ir, sv->vr))
  {
    return;
  }

  if (!(sv->r & VT_LVAL) && (val_kind == VT_LOCAL || val_kind == VT_LLOCAL))
  {
    /* VT_LOCAL without VT_LVAL represents "address of stack location".
     * This is an address computation (fp + offset), not a value to be loaded.
     * Skip materialization - the backend will compute the address directly. */
    return;
  }

  mat_require_result(result, "materialize_value(spill)");

  const int frame_offset = mat_offset_sv(ir, sv);
  unsigned short original_r = sv->r;

  result->original_pr0 = (sv->pr0_spilled ? PREG_SPILLED : 0) | sv->pr0_reg;
  result->original_pr1 = (sv->pr1_spilled ? PREG_SPILLED : 0) | sv->pr1_reg;
  result->original_c_i = sv->c.i;

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, scratch_flags);
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for spill load");

  tcc_machine_load_spill_slot(scratch.regs[0], frame_offset);
  if (is_64bit)
  {
    if (scratch.reg_count < 2)
      tcc_error("compiler_error: missing register pair for 64-bit spill load");
    tcc_machine_load_spill_slot(scratch.regs[1], frame_offset + 4);
  }

  int preserved_flags = sv->r & ~VT_VALMASK;
  /* The spill slot stores the vreg's VALUE.
   *
   * Important distinction:
   * - VT_LVAL on a normal (non-VT_LOCAL) operand means "load through pointer" and
   *   must be preserved.
   * - VT_LVAL on VT_LOCAL/VT_LLOCAL means "load from stack slot". Once we've
   *   loaded the spill slot into a register, that flag must be cleared, otherwise
   *   downstream code will incorrectly dereference the loaded value as an address
   *   (double-deref), e.g. treating an int loop index as int*.
   */
  {
    const int orig_kind = original_r & VT_VALMASK;
    if (orig_kind == VT_LOCAL || orig_kind == VT_LLOCAL)
      preserved_flags &= ~VT_LVAL;
  }

  sv->pr0_reg = scratch.regs[0];
  sv->pr0_spilled = 0;
  if (is_64bit)
  {
    sv->pr1_reg = scratch.regs[1];
    sv->pr1_spilled = 0;
  }
  else
  {
    sv->pr1_reg = PREG_REG_NONE;
    sv->pr1_spilled = 0;
  }
  /* sv->r should only contain the register number and semantic flags (VT_LVAL, VT_PARAM, etc.),
   * not PREG_SPILLED which is only for sv->pr0 */
  sv->r = (unsigned short)(scratch.regs[0] | preserved_flags);
  sv->c.i = 0;

  result->used_scratch = 1;
  result->is_64bit = is_64bit;
  result->original_r = original_r;
  result->scratch = scratch;
}

void tcc_ir_materialize_const_to_reg(TCCIRState *ir, SValue *sv, TCCMaterializedValue *result)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !sv)
    return;

  const int val_kind = sv->r & VT_VALMASK;

  /* Only handle values that aren't already in a register */
  if (sv->pr0_reg != PREG_REG_NONE && !sv->pr0_spilled)
    return;

  /* Only handle constants, comparisons, and jump conditions */
  if (val_kind != VT_CONST && val_kind != VT_CMP && val_kind != VT_JMP && val_kind != VT_JMPI)
    return;

  /* Skip VT_CONST with VT_SYM (symbol references) - those need special handling */
  if (val_kind == VT_CONST && (sv->r & VT_SYM))
    return;

  /* Skip VT_CONST with VT_LVAL (memory loads) - those need load_to_dest */
  if (val_kind == VT_CONST && (sv->r & VT_LVAL))
    return;

  mat_require_result(result, "materialize_const_to_reg");

  const int is_64bit = tcc_ir_type_is_64bit(sv->type.t);
  const unsigned scratch_flags =
      (is_64bit ? TCC_MACHINE_SCRATCH_NEEDS_PAIR : 0) | (ir ? ir->codegen_materialize_scratch_flags : 0);

  result->original_pr0 = (sv->pr0_spilled ? PREG_SPILLED : 0) | sv->pr0_reg;
  result->original_pr1 = (sv->pr1_spilled ? PREG_SPILLED : 0) | sv->pr1_reg;
  result->original_c_i = sv->c.i;
  result->original_r = sv->r;

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, scratch_flags);
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for const-to-reg");

  if (val_kind == VT_CONST)
  {
    tcc_machine_load_constant(scratch.regs[0], is_64bit ? scratch.regs[1] : PREG_NONE, sv->c.i, is_64bit, NULL);
  }
  else if (val_kind == VT_CMP)
  {
    tcc_machine_load_cmp_result(scratch.regs[0], sv->c.i);
  }
  else /* VT_JMP or VT_JMPI */
  {
    const int invert = (val_kind == VT_JMPI) ? 1 : 0;
    tcc_machine_load_jmp_result(scratch.regs[0], sv->c.i, invert);
  }

  sv->pr0_reg = scratch.regs[0];
  sv->pr0_spilled = 0;
  if (is_64bit)
  {
    sv->pr1_reg = scratch.regs[1];
    sv->pr1_spilled = 0;
  }
  else
  {
    sv->pr1_reg = PREG_REG_NONE;
    sv->pr1_spilled = 0;
  }
  sv->r = (unsigned short)(scratch.regs[0]);
  sv->c.i = 0;

  result->used_scratch = 1;
  result->is_64bit = is_64bit;
  result->scratch = scratch;
}

void tcc_ir_materialize_addr(TCCIRState *ir, SValue *sv, TCCMaterializedAddr *result, int dest_reg)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !sv)
    return;

  const int val_kind = sv->r & VT_VALMASK;
  const int wants_stack_address = (val_kind == VT_LOCAL || val_kind == VT_LLOCAL) && !(sv->r & VT_LVAL);
  /* Check for spilled pointer: pr0 must be PREG_SPILLED (0x80), NOT PREG_NONE (0xFF).
   * PREG_NONE has the PREG_SPILLED bit set, so we must explicitly exclude it.
   * IMPORTANT: This is for cases where a POINTER value (result of address arithmetic)
   * was spilled to stack and needs to be reloaded to dereference through it.
   * This is NOT for regular local variables that happen to be spilled - those are
   * handled by VT_LOCAL|VT_LVAL path in the backend.
   * Exclude VT_LOCAL/VT_LLOCAL from being treated as spilled pointers. */
  const int is_local_access = (val_kind == VT_LOCAL || val_kind == VT_LLOCAL);
  const int spilled_pointer = !is_local_access && (sv->pr0_reg != PREG_REG_NONE) && sv->pr0_spilled;

  if (!wants_stack_address && !spilled_pointer)
    return;

  /* Optimization: For VT_LOCAL with encodable offsets, skip materialization.
   * Let the backend handle it directly with [base, #offset] addressing mode
   * instead of wasting a scratch register to compute the address. */
  if (wants_stack_address)
  {
    const int frame_offset = mat_offset_sv(ir, sv);
    /* VT_PARAM with positive offset = stack parameter in caller frame, needs offset_to_args.
     * VT_PARAM with negative offset = variadic register param saved in our frame, no adjustment. */
    const int is_param = ((sv->r & VT_PARAM) && frame_offset >= 0) ? 1 : 0;
    /* Use the actual destination register for the encoding test.
     * If dest_reg is invalid (PREG_NONE), fall back to r12 (typical scratch). */
    const int test_reg = (dest_reg != PREG_NONE && dest_reg < 16) ? dest_reg : 12;
    if (tcc_machine_can_encode_stack_offset_with_param_adj(frame_offset, is_param, test_reg))
      return; /* Backend can encode this offset directly, no scratch needed */
  }

  mat_require_result(result, "materialize_addr");

  result->original_r = sv->r;
  result->original_pr0 = (sv->pr0_spilled ? PREG_SPILLED : 0) | sv->pr0_reg;
  result->original_pr1 = (sv->pr1_spilled ? PREG_SPILLED : 0) | sv->pr1_reg;
  result->original_c_i = sv->c.i;

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, (ir ? ir->codegen_materialize_scratch_flags : 0));
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for address materialization");

  const int target_reg = scratch.regs[0];
  const int frame_offset = mat_offset_sv(ir, sv);
  /* VT_PARAM with positive offset = stack parameter in caller frame, needs offset_to_args.
   * VT_PARAM with negative offset = variadic register param saved in our frame, no adjustment. */
  const int is_param = ((sv->r & VT_PARAM) && frame_offset >= 0) ? 1 : 0;

  if (wants_stack_address)
  {
    tcc_machine_addr_of_stack_slot(target_reg, frame_offset, is_param);
    int flags = (sv->r & ~VT_VALMASK) | VT_LVAL;
    sv->pr0_reg = target_reg;
    sv->pr0_spilled = 0;
    sv->pr1_reg = PREG_REG_NONE;
    sv->pr1_spilled = 0;
    sv->r = (unsigned short)(target_reg | flags);
    sv->c.i = 0;
  }
  else if (spilled_pointer)
  {
    tcc_machine_load_spill_slot(target_reg, frame_offset);
    sv->pr0_reg = target_reg;
    sv->pr0_spilled = 0;
    sv->pr1_reg = PREG_REG_NONE;
    sv->pr1_spilled = 0;
    sv->r = (unsigned short)((sv->r & ~VT_VALMASK) | target_reg);
    sv->c.i = 0;
  }

  result->used_scratch = 1;
  result->scratch = scratch;
}

void tcc_ir_materialize_dest(TCCIRState *ir, SValue *dest, TCCMaterializedDest *result)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !dest)
    return;
  if (!dest->pr0_spilled)
    return;
  if (!tcc_ir_vreg_is_valid(ir, dest->vr))
    return;

  mat_require_result(result, "materialize_dest");

  const int frame_offset = mat_offset_sv(ir, dest);
  const int is_64bit = tcc_ir_type_is_64bit(dest->type.t);
  const unsigned scratch_flags =
      (is_64bit ? TCC_MACHINE_SCRATCH_NEEDS_PAIR : 0) | (ir ? ir->codegen_materialize_scratch_flags : 0);
  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, scratch_flags);
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for spill destination");
  if (is_64bit && scratch.reg_count < 2)
    tcc_error("compiler_error: missing register pair for 64-bit spill destination");

  result->needs_storeback = 1;
  result->is_64bit = is_64bit;
  result->frame_offset = frame_offset;
  result->original_pr0 = (dest->pr0_spilled ? PREG_SPILLED : 0) | dest->pr0_reg;
  result->original_pr1 = (dest->pr1_spilled ? PREG_SPILLED : 0) | dest->pr1_reg;
  result->original_r = dest->r;
  result->scratch = scratch;

  dest->pr0_reg = scratch.regs[0];
  dest->pr0_spilled = 0;
  if (is_64bit)
  {
    dest->pr1_reg = scratch.regs[1];
    dest->pr1_spilled = 0;
  }
  else
  {
    dest->pr1_reg = PREG_REG_NONE;
    dest->pr1_spilled = 0;
  }
  int flags = dest->r & ~VT_VALMASK;
  flags &= ~VT_LVAL;
  dest->r = (unsigned short)(dest->pr0_reg | flags);
  dest->c.i = 0;
}

/* ============================================================================
 * IROperand Materialization
 * ============================================================================ */

void tcc_ir_materialize_value_ir(TCCIRState *ir, IROperand *op, TCCMaterializedValue *result)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !op)
    return;

  const int vreg = irop_get_vreg(*op);

  if (op->is_param && op->is_local)
  {
    /* Stack-passed parameters live in the caller frame. Leave them as
     * param lvalues so the backend can read directly from the caller stack. */
    op->pr0_reg = PREG_REG_NONE;
    op->pr0_spilled = 0;
    op->pr1_reg = PREG_REG_NONE;
    op->pr1_spilled = 0;
    return;
  }

  /* Register parameters with is_lval: clear is_lval since the register
   * already holds the value, not a pointer. */
  if (op->is_param && op->is_lval)
  {
    if (!op->is_local && !op->is_llocal)
    {
      op->is_lval = 0;
    }
  }

  const int is_64bit = irop_is_64bit(*op);
  const unsigned scratch_flags =
      (is_64bit ? TCC_MACHINE_SCRATCH_NEEDS_PAIR : 0) | (ir ? ir->codegen_materialize_scratch_flags : 0);

  if (!op->pr0_spilled)
  {
    return;
  }
  if (!tcc_ir_vreg_is_valid(ir, vreg))
  {
    return;
  }

  if (!op->is_lval && op->is_local)
  {
    /* VT_LOCAL without VT_LVAL represents "address of stack location".
     * Skip materialization - the backend will compute the address directly. */
    return;
  }

  mat_require_result(result, "materialize_value_ir(spill)");

  const int frame_offset = mat_offset_op(ir, op);

  result->original_pr0 = (op->pr0_spilled ? PREG_SPILLED : 0) | op->pr0_reg;
  result->original_pr1 = (op->pr1_spilled ? PREG_SPILLED : 0) | op->pr1_reg;

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, scratch_flags);
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for spill load");

  tcc_machine_load_spill_slot(scratch.regs[0], frame_offset);
  if (is_64bit)
  {
    if (scratch.reg_count < 2)
      tcc_error("compiler_error: missing register pair for 64-bit spill load");
    tcc_machine_load_spill_slot(scratch.regs[1], frame_offset + 4);
  }

  /* Once loaded from spill slot, clear local/llocal flags for stack-origin values.
   * The value is now in a register, not on the stack. */
  const int was_local = op->is_local;
  const int was_llocal = op->is_llocal;
  if (was_local || was_llocal)
    op->is_lval = 0;

  op->pr0_reg = scratch.regs[0];
  op->pr0_spilled = 0;
  if (is_64bit)
  {
    op->pr1_reg = scratch.regs[1];
    op->pr1_spilled = 0;
  }
  else
  {
    op->pr1_reg = PREG_REG_NONE;
    op->pr1_spilled = 0;
  }
  op->tag = IROP_TAG_VREG;
  op->is_local = 0;
  op->is_llocal = 0;
  op->is_const = 0;
  op->u.imm32 = 0;

  result->used_scratch = 1;
  result->is_64bit = is_64bit;
  result->scratch = scratch;
}

void tcc_ir_materialize_const_to_reg_ir(TCCIRState *ir, IROperand *op, TCCMaterializedValue *result)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !op)
    return;

  /* Only handle values that aren't already in a register */
  if (op->pr0_reg != PREG_REG_NONE && !op->pr0_spilled)
    return;

  const int tag = irop_get_tag(*op);

  /* Only handle constants (IMM32, I64, F32, F64) - not VREG or STACKOFF */
  if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64 && tag != IROP_TAG_F32 && tag != IROP_TAG_F64)
    return;

  /* Skip constants with symbols (SYMREF) - those need special handling */
  if (op->is_sym)
    return;

  /* Skip constants with lval (memory loads) - those need load_to_dest */
  if (op->is_lval)
    return;

  mat_require_result(result, "materialize_const_to_reg_ir");

  const int is_64bit = irop_is_64bit(*op);
  const unsigned scratch_flags =
      (is_64bit ? TCC_MACHINE_SCRATCH_NEEDS_PAIR : 0) | (ir ? ir->codegen_materialize_scratch_flags : 0);

  result->original_pr0 = (op->pr0_spilled ? PREG_SPILLED : 0) | op->pr0_reg;
  result->original_pr1 = (op->pr1_spilled ? PREG_SPILLED : 0) | op->pr1_reg;

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, scratch_flags);
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for const-to-reg");

  int64_t val = irop_get_imm64_ex(ir, *op);
  tcc_machine_load_constant(scratch.regs[0], is_64bit ? scratch.regs[1] : PREG_NONE, val, is_64bit, NULL);

  op->pr0_reg = scratch.regs[0];
  op->pr0_spilled = 0;
  if (is_64bit)
  {
    op->pr1_reg = scratch.regs[1];
    op->pr1_spilled = 0;
  }
  else
  {
    op->pr1_reg = PREG_REG_NONE;
    op->pr1_spilled = 0;
  }
  op->tag = IROP_TAG_VREG;
  op->is_const = 0;
  op->u.imm32 = 0;

  result->used_scratch = 1;
  result->is_64bit = is_64bit;
  result->scratch = scratch;
}

void tcc_ir_materialize_addr_ir(TCCIRState *ir, IROperand *op, TCCMaterializedAddr *result, int dest_reg)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !op)
    return;

  const int wants_stack_address = op->is_local && !op->is_lval;
  /* Spilled pointer: pr0 must be PREG_SPILLED, NOT PREG_NONE.
   * Exclude local/llocal from being treated as spilled pointers. */
  const int is_local_access = op->is_local;
  const int spilled_pointer = !is_local_access && (op->pr0_reg != PREG_REG_NONE) && op->pr0_spilled;

  if (!wants_stack_address && !spilled_pointer)
    return;

  /* Optimization: For locals with encodable offsets, skip materialization. */
  if (wants_stack_address)
  {
    const int frame_offset = mat_offset_op(ir, op);
    const int is_param = (op->is_param && frame_offset >= 0) ? 1 : 0;
    const int test_reg = (dest_reg != PREG_NONE && dest_reg < 16) ? dest_reg : 12;
    if (tcc_machine_can_encode_stack_offset_with_param_adj(frame_offset, is_param, test_reg))
      return;
  }

  mat_require_result(result, "materialize_addr_ir");

  result->original_pr0 = (op->pr0_spilled ? PREG_SPILLED : 0) | op->pr0_reg;
  result->original_pr1 = (op->pr1_spilled ? PREG_SPILLED : 0) | op->pr1_reg;

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, (ir ? ir->codegen_materialize_scratch_flags : 0));
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for address materialization");

  const int target_reg = scratch.regs[0];
  const int frame_offset = mat_offset_op(ir, op);
  const int is_param = (op->is_param && frame_offset >= 0) ? 1 : 0;

  if (wants_stack_address)
  {
    tcc_machine_addr_of_stack_slot(target_reg, frame_offset, is_param);
    op->pr0_reg = target_reg;
    op->pr0_spilled = 0;
    op->pr1_reg = PREG_REG_NONE;
    op->pr1_spilled = 0;
    op->is_lval = 1;
    op->tag = IROP_TAG_VREG;
    op->is_local = 0;
    op->is_llocal = 0;
    op->is_const = 0;
    op->u.imm32 = 0;
  }
  else if (spilled_pointer)
  {
    tcc_machine_load_spill_slot(target_reg, frame_offset);
    op->pr0_reg = target_reg;
    op->pr0_spilled = 0;
    op->pr1_reg = PREG_REG_NONE;
    op->pr1_spilled = 0;
    op->tag = IROP_TAG_VREG;
    op->is_local = 0;
    op->is_llocal = 0;
    op->is_const = 0;
    op->u.imm32 = 0;
  }

  result->used_scratch = 1;
  result->scratch = scratch;
}

void tcc_ir_materialize_dest_ir(TCCIRState *ir, IROperand *op, TCCMaterializedDest *result)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !op)
    return;

  const int is_64bit = irop_is_64bit(*op);

  /* Stack-passed parameters (is_param && is_local) have pr0_reg == PREG_REG_NONE
   * without being "spilled" in the traditional sense — they were never in a register.
   * When used as a destination, we need a scratch register for the computation
   * and must store the result back to the caller's argument area. */
  if (op->is_param && op->is_local && !op->pr0_spilled && op->pr0_reg == PREG_REG_NONE)
  {
    const int vreg = irop_get_vreg(*op);
    if (!tcc_ir_vreg_is_valid(ir, vreg))
      return;

    mat_require_result(result, "materialize_dest_ir(param)");

    const int frame_offset = mat_offset_op(ir, op);
    unsigned scratch_flags = (ir ? ir->codegen_materialize_scratch_flags : 0);
    if (is_64bit)
      scratch_flags |= TCC_MACHINE_SCRATCH_NEEDS_PAIR;

    TCCMachineScratchRegs scratch = {0};
    tcc_machine_acquire_scratch(&scratch, scratch_flags);
    if (scratch.reg_count == 0)
      tcc_error("compiler_error: unable to allocate scratch register for param destination");
    if (is_64bit && scratch.reg_count < 2)
      tcc_error("compiler_error: missing register pair for 64-bit param destination");

    result->needs_storeback = 1;
    result->is_64bit = is_64bit;
    result->is_param = 1;
    result->frame_offset = frame_offset;
    result->original_pr0 = PREG_SPILLED | PREG_REG_NONE;
    result->original_pr1 = is_64bit ? (PREG_SPILLED | PREG_REG_NONE) : PREG_REG_NONE;
    result->scratch = scratch;

    op->pr0_reg = scratch.regs[0];
    op->pr0_spilled = 0;
    if (is_64bit && scratch.reg_count >= 2)
    {
      op->pr1_reg = scratch.regs[1];
      op->pr1_spilled = 0;
    }
    op->is_lval = 0;
    op->tag = IROP_TAG_VREG;
    op->is_local = 0;
    op->is_llocal = 0;
    op->is_const = 0;
    op->is_param = 0;
    op->u.imm32 = 0;
    return;
  }

  /* Handle destinations with no physical register allocated. This covers:
   * - Concrete stack slot destinations (vreg == -1, is_local) where
   *   tcc_ir_fill_registers_ir() leaves them unallocated.
   * - Vregs that ended up with r0 == PREG_NONE and offset == 0 after
   *   register allocation (neither spilled nor in-register).
   * In both cases we need a scratch register for the computation
   * and must store the result back. */
  if (!op->is_param && op->pr0_reg == PREG_REG_NONE && !op->pr0_spilled)
  {
    mat_require_result(result, "materialize_dest_ir(stack_slot)");

    const int frame_offset = mat_offset_op(ir, op);
    unsigned scratch_flags = (ir ? ir->codegen_materialize_scratch_flags : 0);
    if (is_64bit)
      scratch_flags |= TCC_MACHINE_SCRATCH_NEEDS_PAIR;

    TCCMachineScratchRegs scratch = {0};
    tcc_machine_acquire_scratch(&scratch, scratch_flags);
    if (scratch.reg_count == 0)
      tcc_error("compiler_error: unable to allocate scratch register for stack slot destination");
    if (is_64bit && scratch.reg_count < 2)
      tcc_error("compiler_error: missing register pair for 64-bit stack slot destination");

    result->needs_storeback = 1;
    result->is_64bit = is_64bit;
    result->is_param = 0;
    result->frame_offset = frame_offset;
    result->original_pr0 = PREG_SPILLED | PREG_REG_NONE;
    result->original_pr1 = is_64bit ? (PREG_SPILLED | PREG_REG_NONE) : PREG_REG_NONE;
    result->scratch = scratch;

    op->pr0_reg = scratch.regs[0];
    op->pr0_spilled = 0;
    if (is_64bit && scratch.reg_count >= 2)
    {
      op->pr1_reg = scratch.regs[1];
      op->pr1_spilled = 0;
    }
    op->is_lval = 0;
    op->tag = IROP_TAG_VREG;
    op->is_local = 0;
    op->is_llocal = 0;
    op->is_const = 0;
    op->is_param = 0;
    op->u.imm32 = 0;
    return;
  }

  /* Handle case when pr0 is spilled, or when pr1 is spilled for 64-bit values */
  const int needs_materialize = op->pr0_spilled || (is_64bit && op->pr1_spilled);
  if (!needs_materialize)
    return;

  const int vreg = irop_get_vreg(*op);
  if (!tcc_ir_vreg_is_valid(ir, vreg))
    return;

  mat_require_result(result, "materialize_dest_ir");

  const int frame_offset = mat_offset_op(ir, op);
  const int pr0_was_spilled = op->pr0_spilled;
  const int pr1_was_spilled = op->pr1_spilled;

  /*
   * For 64-bit values, we need to handle several cases:
   * 1. Both pr0 and pr1 spilled: need 2 scratch registers
   * 2. Only pr0 spilled: need 1 scratch register for pr0
   * 3. Only pr1 spilled: need 1 scratch register for pr1
   */
  unsigned scratch_flags = (ir ? ir->codegen_materialize_scratch_flags : 0);
  if (is_64bit && (pr0_was_spilled || pr1_was_spilled))
    scratch_flags |= TCC_MACHINE_SCRATCH_NEEDS_PAIR;

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, scratch_flags);
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for spill destination");
  if (is_64bit && scratch.reg_count < 2)
    tcc_error("compiler_error: missing register pair for 64-bit spill destination");

  result->needs_storeback = 1;
  result->is_64bit = is_64bit;
  result->frame_offset = frame_offset;
  result->original_pr0 = (pr0_was_spilled ? PREG_SPILLED : 0) | op->pr0_reg;
  result->original_pr1 = (pr1_was_spilled ? PREG_SPILLED : 0) | op->pr1_reg;
  result->scratch = scratch;

  /* Replace spilled registers with scratch registers */
  if (pr0_was_spilled)
  {
    op->pr0_reg = scratch.regs[0];
    op->pr0_spilled = 0;
    if (is_64bit && pr1_was_spilled)
    {
      op->pr1_reg = scratch.regs[1];
      op->pr1_spilled = 0;
    }
    else if (is_64bit)
    {
      /* pr0 was spilled but pr1 was not - pr1 stays in its register */
      op->pr1_spilled = 0;
    }
  }
  else if (is_64bit && pr1_was_spilled)
  {
    /* Only pr1 was spilled, pr0 stays in its register */
    op->pr1_reg = scratch.regs[0];
    op->pr1_spilled = 0;
  }
  else
  {
    op->pr1_reg = PREG_REG_NONE;
    op->pr1_spilled = 0;
  }
  op->is_lval = 0;
  op->tag = IROP_TAG_VREG;
  op->is_local = 0;
  op->is_llocal = 0;
  op->is_const = 0;
  op->u.imm32 = 0;
}

/* ============================================================================
 * Materialization Cleanup
 * ============================================================================ */

void tcc_ir_storeback_materialized_dest_ir(IROperand *op, TCCMaterializedDest *mat)
{
  if (!mat || !mat->needs_storeback)
    return;

  /* Store back only the registers that were originally spilled */
  const int pr0_was_spilled = (mat->original_pr0 & PREG_SPILLED) != 0;
  const int pr1_was_spilled = (mat->original_pr1 & PREG_SPILLED) != 0;

  if (mat->is_param)
  {
    /* Stack-passed parameters need offset_to_args adjustment in the backend */
    if (pr0_was_spilled)
      tcc_machine_store_param_slot(op->pr0_reg, mat->frame_offset);
    if (mat->is_64bit && pr1_was_spilled)
      tcc_machine_store_param_slot(op->pr1_reg, mat->frame_offset + 4);
  }
  else
  {
    if (pr0_was_spilled)
      tcc_machine_store_spill_slot(op->pr0_reg, mat->frame_offset);
    if (mat->is_64bit && pr1_was_spilled)
      tcc_machine_store_spill_slot(op->pr1_reg, mat->frame_offset + 4);
  }

  tcc_machine_release_scratch(&mat->scratch);
}

void tcc_ir_release_materialized_value_ir(TCCMaterializedValue *mat)
{
  if (!mat || !mat->used_scratch)
    return;
  tcc_machine_release_scratch(&mat->scratch);
}

void tcc_ir_release_materialized_addr_ir(TCCMaterializedAddr *mat)
{
  if (!mat || !mat->used_scratch)
    return;
  tcc_machine_release_scratch(&mat->scratch);
}

/* ============================================================================
 * Spill Detection
 * ============================================================================ */

int tcc_ir_mat_spilled(SValue *sv)
{
  return (sv->pr0_reg == PREG_REG_NONE) || sv->pr0_spilled;
}

int tcc_ir_mat_spilled_op(const IROperand *op)
{
  return op->pr0_spilled;
}

/* Legacy wrapper for spilled check */
int tcc_ir_is_spilled_ir(const IROperand *op)
{
  return tcc_ir_mat_spilled_op(op);
}

/* ============================================================================
 * New API Wrappers (TCCMatValue, TCCMatAddr, TCCMatDest)
 * ============================================================================
 * These wrap the legacy TCCMaterialized* structures for new code.
 */

void tcc_ir_mat_value(TCCIRState *ir, SValue *sv, TCCMatValue *result)
{
  TCCMaterializedValue legacy = {0};
  tcc_ir_materialize_value(ir, sv, &legacy);
  if (result)
  {
    result->used_scratch = legacy.used_scratch;
    result->scratch = legacy.scratch;
    result->original_pr0 = legacy.original_pr0;
    result->original_pr1 = legacy.original_pr1;
  }
}

void tcc_ir_mat_const(TCCIRState *ir, SValue *sv, TCCMatValue *result)
{
  TCCMaterializedValue legacy = {0};
  tcc_ir_materialize_const_to_reg(ir, sv, &legacy);
  if (result)
  {
    result->used_scratch = legacy.used_scratch;
    result->scratch = legacy.scratch;
    result->original_pr0 = legacy.original_pr0;
    result->original_pr1 = legacy.original_pr1;
  }
}

void tcc_ir_mat_addr(TCCIRState *ir, SValue *sv, TCCMatAddr *result, int dest_reg)
{
  TCCMaterializedAddr legacy = {0};
  tcc_ir_materialize_addr(ir, sv, &legacy, dest_reg);
  if (result)
  {
    result->used_scratch = legacy.used_scratch;
    result->scratch = legacy.scratch;
    result->base_reg = legacy.used_scratch ? legacy.scratch.regs[0] : 0;
    result->needs_deref = 0;
  }
}

void tcc_ir_mat_dest(TCCIRState *ir, SValue *dest, TCCMatDest *result)
{
  TCCMaterializedDest legacy = {0};
  tcc_ir_materialize_dest(ir, dest, &legacy);
  if (result)
  {
    result->used_scratch = legacy.needs_storeback;
    result->scratch = legacy.scratch;
    result->frame_offset = legacy.frame_offset;
    result->is_64bit = legacy.is_64bit;
  }
}

void tcc_ir_mat_value_op(TCCIRState *ir, IROperand *op, TCCMatValue *result)
{
  TCCMaterializedValue legacy = {0};
  tcc_ir_materialize_value_ir(ir, op, &legacy);
  if (result)
  {
    result->used_scratch = legacy.used_scratch;
    result->scratch = legacy.scratch;
    result->original_pr0 = legacy.original_pr0;
    result->original_pr1 = legacy.original_pr1;
  }
}

void tcc_ir_mat_const_op(TCCIRState *ir, IROperand *op, TCCMatValue *result)
{
  TCCMaterializedValue legacy = {0};
  tcc_ir_materialize_const_to_reg_ir(ir, op, &legacy);
  if (result)
  {
    result->used_scratch = legacy.used_scratch;
    result->scratch = legacy.scratch;
    result->original_pr0 = legacy.original_pr0;
    result->original_pr1 = legacy.original_pr1;
  }
}

void tcc_ir_mat_addr_op(TCCIRState *ir, IROperand *op, TCCMatAddr *result, int dest_reg)
{
  TCCMaterializedAddr legacy = {0};
  tcc_ir_materialize_addr_ir(ir, op, &legacy, dest_reg);
  if (result)
  {
    result->used_scratch = legacy.used_scratch;
    result->scratch = legacy.scratch;
    result->base_reg = legacy.used_scratch ? legacy.scratch.regs[0] : 0;
    result->needs_deref = 0;
  }
}

void tcc_ir_mat_dest_op(TCCIRState *ir, IROperand *op, TCCMatDest *result)
{
  TCCMaterializedDest legacy = {0};
  tcc_ir_materialize_dest_ir(ir, op, &legacy);
  if (result)
  {
    result->used_scratch = legacy.needs_storeback;
    result->scratch = legacy.scratch;
    result->frame_offset = legacy.frame_offset;
    result->is_64bit = legacy.is_64bit;
  }
}

void tcc_ir_mat_dest_storeback(TCCIRState *ir, IROperand *op, TCCMatDest *mat)
{
  (void)ir;
  if (!mat)
    return;
  TCCMaterializedDest legacy = {0};
  legacy.needs_storeback = mat->used_scratch;
  legacy.is_64bit = mat->is_64bit;
  legacy.frame_offset = mat->frame_offset;
  legacy.original_pr0 = mat->used_scratch ? (PREG_SPILLED | mat->scratch.regs[0]) : 0;
  legacy.original_pr1 = (mat->is_64bit && mat->used_scratch) ? (PREG_SPILLED | mat->scratch.regs[1]) : 0;
  legacy.scratch = mat->scratch;
  tcc_ir_storeback_materialized_dest_ir(op, &legacy);
}

void tcc_ir_mat_value_release(TCCIRState *ir, TCCMatValue *mat)
{
  (void)ir;
  if (!mat || !mat->used_scratch)
    return;
  tcc_machine_release_scratch(&mat->scratch);
}

void tcc_ir_mat_addr_release(TCCIRState *ir, TCCMatAddr *mat)
{
  (void)ir;
  if (!mat || !mat->used_scratch)
    return;
  tcc_machine_release_scratch(&mat->scratch);
}

void tcc_ir_mat_dest_release(TCCIRState *ir, TCCMatDest *mat)
{
  (void)ir;
  if (!mat || !mat->used_scratch)
    return;
  tcc_machine_release_scratch(&mat->scratch);
}

/* ============================================================================
 * Operand Property Helpers
 * ============================================================================ */

bool tcc_ir_operand_needs_dereference(SValue *sv)
{
  const int val_loc = sv->r & VT_VALMASK;
  switch (val_loc)
  {
  case VT_CONST:
  case VT_LOCAL:
    /* VT_CONST with VT_LVAL means we're loading through a global symbol address.
     * For example: a.x where 'a' is a static struct - the address is a constant
     * (global symbol) but we need to dereference it to get the value. */
    return (sv->r & VT_LVAL) != 0;
  case VT_LLOCAL:
  case VT_CMP:
  case VT_JMP:
  case VT_JMPI:
    return false;
  default: /* must be temporary vreg */
    /* Register parameters (VT_PARAM without VT_LOCAL) have VT_LVAL set to allow
     * taking their address (&param), but the register holds the VALUE directly,
     * not a pointer. So VT_LVAL does NOT mean dereference for these. */
    if ((sv->r & VT_PARAM) && !(sv->r & VT_LOCAL))
      return false;
    return (sv->r & VT_LVAL) != 0;
  }
}
