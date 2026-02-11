/*
 *  TCC IR - Code Generation Helpers Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

/* Forward declarations for materialization functions (defined in ir/mat.c) */
extern void tcc_ir_release_materialized_value_ir(TCCMaterializedValue *mat);
extern void tcc_ir_release_materialized_addr_ir(TCCMaterializedAddr *mat);
extern void tcc_ir_storeback_materialized_dest_ir(IROperand *op, TCCMaterializedDest *mat);

/* ============================================================================
 * Register Fill (Apply Allocation to Operands)
 * ============================================================================ */

void tcc_ir_fill_registers(TCCIRState *ir, SValue *sv)
{
  int old_r = sv->r;
  int old_v = old_r & VT_VALMASK;

  /* VT_LOCAL/VT_LLOCAL operands can mean either:
   * - a concrete stack slot (vr == -1), e.g. VLA save slots, or
   * - a logical local tracked as a vreg by the IR (vr != -1).
   *
   * For concrete stack slots, do not rewrite them into registers here; doing
   * so can create uninitialized register reads at runtime.
   *
   * For locals that do carry a vreg, they must participate in register
   * allocation so that defs/uses stay consistent.
   */
  if ((old_v == VT_LOCAL || old_v == VT_LLOCAL) && sv->vr == -1)
  {
    sv->pr0_reg = PREG_REG_NONE;
    sv->pr0_spilled = 0;
    sv->pr1_reg = PREG_REG_NONE;
    sv->pr1_spilled = 0;
    return;
  }
  if (tcc_ir_vreg_is_valid(ir, sv->vr))
  {
    IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, sv->vr);

    /* Stack-passed parameters: if not allocated to a register, treat them as
     * residing in the incoming argument area (VT_PARAM) rather than forcing a
     * separate local spill slot.
     *
     * This is safe under AAPCS: the caller's argument stack area remains valid
     * for the duration of the call, and it also provides a correct addressable
     * home for '&param' semantics.
     */
    if (TCCIR_DECODE_VREG_TYPE(sv->vr) == TCCIR_VREG_TYPE_PARAM && interval && interval->incoming_reg0 < 0 &&
        interval->allocation.r0 == PREG_NONE && interval->allocation.offset == 0)
    {
      sv->pr0_reg = PREG_REG_NONE;
      sv->pr0_spilled = 0;
      sv->pr1_reg = PREG_REG_NONE;
      sv->pr1_spilled = 0;
      sv->c.i = interval->original_offset;

      int need_lval = (old_r & VT_LVAL);
      if (old_v < VT_CONST && old_v != VT_LOCAL && old_v != VT_LLOCAL && interval->is_lvalue)
        need_lval = VT_LVAL;

      sv->r = VT_LOCAL | need_lval | VT_PARAM;
      return;
    }

    /* Register-passed parameters: if allocated to a register (not spilled),
     * clear VT_LVAL. The value is already in the register, no dereference needed.
     * VT_LVAL is only used on parameters for address-of operations (&param) or
     * when they're on the stack (VT_LOCAL).
     */
    int is_register_param =
        (TCCIR_DECODE_VREG_TYPE(sv->vr) == TCCIR_VREG_TYPE_PARAM && interval && interval->incoming_reg0 >= 0);

    sv->pr0_reg = interval->allocation.r0 & PREG_REG_NONE;
    sv->pr0_spilled = (interval->allocation.r0 & PREG_SPILLED) != 0;
    sv->pr1_reg = interval->allocation.r1 & PREG_REG_NONE;
    sv->pr1_spilled = (interval->allocation.r1 & PREG_SPILLED) != 0;
    sv->c.i = interval->allocation.offset;

    /* Determine if we should preserve VT_LVAL:
     * - If old_r was VT_LOCAL|VT_LVAL (local variable on stack), and now
     *   it's allocated to a register, we should NOT preserve VT_LVAL because
     *   the value is already in the register, no load needed.
     * - If old_r has VT_LVAL but (old_r & VT_VALMASK) < VT_CONST, it means
     *   the vreg holds a pointer that needs dereferencing - preserve VT_LVAL.
     * - Register parameters: do NOT preserve VT_LVAL when allocated to a register.
     *   VT_LVAL on parameters is only needed for stack params (VT_LOCAL) or for
     *   address-of operations.
     * - If old_r does NOT have VT_LVAL, this is an address-of operation
     *   (we want the address, not the value). Do NOT add VT_LVAL. */
    int preserve_flags = old_r & VT_PARAM; /* Always preserve VT_PARAM */
    if ((old_r & VT_LVAL) && old_v < VT_CONST && old_v != VT_LOCAL && old_v != VT_LLOCAL && !is_register_param)
    {
      /* The vreg holds a pointer that needs dereferencing.
       * Note: VT_LOCAL/VT_LLOCAL use VT_LVAL to mean "load from stack slot".
       * When such a local/param is promoted to a register, we must NOT
       * preserve VT_LVAL, otherwise we turn a plain value into a pointer
       * dereference (double-indirection bugs).
       */
      preserve_flags |= VT_LVAL;
    }

    if ((interval->allocation.r0 & PREG_SPILLED) || interval->allocation.offset != 0)
    {
      /* Spilled to stack - treat as local.
       * For computed values (old_r was 0 or a register), add VT_LVAL to load the value.
       * For address-of expressions (old_r == VT_LOCAL without VT_LVAL), don't add VT_LVAL.
       * If original had VT_LVAL (pointer dereference), preserve it.
       *
       * DOUBLE INDIRECTION CASE: If old_r has VT_LVAL AND the original was NOT
       * already a local variable (VT_LOCAL), then the code wants to DEREFERENCE
       * the value held in this vreg. If that value is spilled:
       *   - Spill slot contains a POINTER value (e.g., result of ADD on address)
       *   - Need to: (1) load pointer from spill, (2) dereference it
       * Use VT_LLOCAL to encode this double-indirection requirement.
       *
       * But if old_v == VT_LOCAL, the VT_LVAL means "load/store from/to this stack slot"
       * which is standard local variable access - do NOT use VT_LLOCAL.
       *
       * ADDRESS-OF CASE: If old_v == VT_LOCAL and old_r does NOT have VT_LVAL,
       * this is an address-of operation (&var). We want the ADDRESS of the spill
       * slot, not its contents. Do NOT add VT_LVAL in this case.
       *
       * COMPUTED VALUE CASE: If old_v was a register (computed value that got
       * spilled), we ALWAYS need VT_LVAL to load the value from the spill slot. */
      int need_lval;
      if (old_v == VT_LOCAL || old_v == VT_LLOCAL)
      {
        /* Local variable: preserve VT_LVAL to distinguish load vs address-of */
        need_lval = (old_r & VT_LVAL);
      }
      else
      {
        /* Computed value (was in register): always need VT_LVAL to load from spill */
        need_lval = VT_LVAL;
      }
      int base_kind = VT_LOCAL;
      if ((old_r & VT_LVAL) && old_v != VT_LOCAL && old_v != VT_LLOCAL)
      {
        /* The original use wants to dereference the value in this vreg.
         * Since the value is spilled, we need double indirection:
         * load pointer from spill slot, then dereference it.
         * Note: We exclude VT_LOCAL/VT_LLOCAL because their VT_LVAL means
         * "access this stack slot" not "dereference pointer in vreg". */
        base_kind = VT_LLOCAL;
      }
      /* Only preserve VT_PARAM for stack-passed parameters (incoming_reg0 < 0).
       * Register-passed parameters that are spilled to local stack should NOT
       * have VT_PARAM set, because VT_PARAM causes load_to_dest to add
       * offset_to_args (for accessing caller's argument area), but spilled
       * register params live in the callee's local stack area (negative FP offset). */
      int spilled_param_flag = 0;
      if ((old_r & VT_PARAM) && interval->incoming_reg0 < 0)
      {
        spilled_param_flag = VT_PARAM;
      }
      sv->r = base_kind | need_lval | spilled_param_flag;
    }
    else if (interval->allocation.r0 != PREG_NONE)
    {
      /* In a register - set r to the register number, preserving VT_LVAL only for pointer derefs */
      sv->r = interval->allocation.r0 | preserve_flags;
    }
  }
  else if ((sv->vr == -1 || sv->vr == 0 || TCCIR_DECODE_VREG_TYPE(sv->vr) == 0) &&
           (sv->r == -1 || sv->r == PREG_REG_NONE || (old_v >= VT_CONST)))
  {
    /* No valid vreg and either invalid .r or a constant - preserve important flags.
     * This handles global symbol references (VT_CONST | VT_SYM) and plain constants. */
    int flags = sv->r & (VT_LVAL | VT_SYM);
    sv->r = VT_CONST | flags;
  }
  else if (sv->vr == -1 && old_r == 0 && sv->sym)
  {
    /* Special case: old_r=0 but has a symbol - this is a function symbol reference
     * that wasn't marked as VT_CONST. Preserve the symbol. */
    sv->r = VT_CONST | VT_SYM;
  }
}

void tcc_ir_fill_registers_ir(TCCIRState *ir, IROperand *op)
{
  const int old_is_local = op->is_local;
  const int old_is_llocal = op->is_llocal;
  const int old_is_const = op->is_const;
  const int old_is_lval = op->is_lval;
  const int old_is_param = op->is_param;

  const int vreg = irop_get_vreg(*op);

  /* VT_LOCAL/VT_LLOCAL operands can mean either:
   * - a concrete stack slot (vr == -1), e.g. VLA save slots, or
   * - a logical local tracked as a vreg by the IR (vr != -1).
   *
   * For concrete stack slots, do not rewrite them into registers here; doing
   * so can create uninitialized register reads at runtime. */
  if ((old_is_local || old_is_llocal) && vreg == -1)
  {
    op->pr0_reg = PREG_REG_NONE;
    op->pr0_spilled = 0;
    op->pr1_reg = PREG_REG_NONE;
    op->pr1_spilled = 0;
    return;
  }

  if (tcc_ir_vreg_is_valid(ir, vreg))
  {
    IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);

    /* Stack-passed parameters: if not allocated to a register, treat them as
     * residing in the incoming argument area (VT_PARAM) rather than forcing a
     * separate local spill slot. */
    if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM && interval && interval->incoming_reg0 < 0 &&
        interval->allocation.r0 == PREG_NONE && interval->allocation.offset == 0)
    {
      op->pr0_reg = PREG_REG_NONE;
      op->pr0_spilled = 0;
      op->pr1_reg = PREG_REG_NONE;
      op->pr1_spilled = 0;
      /* For STRUCT types, preserve ctype_idx in the split encoding */
      if (op->btype == IROP_BTYPE_STRUCT)
      {
        op->u.s.aux_data = interval->original_offset / 4;
      }
      else
      {
        op->u.imm32 = interval->original_offset;
      }
      op->tag = IROP_TAG_STACKOFF;

      int need_lval = old_is_lval;
      /* old_v < VT_CONST && old_v != VT_LOCAL && old_v != VT_LLOCAL → reg kind operand */
      if (!old_is_const && !old_is_local && !old_is_llocal && interval->is_lvalue)
        need_lval = 1;

      op->is_local = 1;
      op->is_llocal = 0;
      op->is_const = 0;
      op->is_lval = need_lval;
      op->is_param = 1;
      return;
    }

    /* Register-passed parameters: if allocated to a register (not spilled),
     * clear VT_LVAL. The value is already in the register, no dereference needed. */
    int is_register_param =
        (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM && interval && interval->incoming_reg0 >= 0);

    op->pr0_reg = interval->allocation.r0 & PREG_REG_NONE;
    op->pr0_spilled = (interval->allocation.r0 & PREG_SPILLED) != 0;
    op->pr1_reg = interval->allocation.r1 & PREG_REG_NONE;
    op->pr1_spilled = (interval->allocation.r1 & PREG_SPILLED) != 0;
    /* For STRUCT types, preserve ctype_idx in the split encoding */
    if (op->btype == IROP_BTYPE_STRUCT)
    {
      op->u.s.aux_data = interval->allocation.offset / 4;
    }
    else
    {
      op->u.imm32 = interval->allocation.offset;
    }

    /* Determine if we should preserve is_lval:
     * - If was local|lval and now in register, do NOT preserve is_lval
     * - If was lval with reg-kind operand (pointer deref), preserve is_lval
     * - Register parameters: do NOT preserve is_lval when in register */
    int preserve_param = old_is_param;
    int preserve_lval = 0;
    if (old_is_lval && !old_is_const && !old_is_local && !old_is_llocal && !is_register_param)
    {
      preserve_lval = 1;
    }

    if ((interval->allocation.r0 & PREG_SPILLED) || interval->allocation.offset != 0)
    {
      /* Spilled to stack */
      int need_lval;
      if (old_is_local || old_is_llocal)
      {
        need_lval = old_is_lval;
      }
      else
      {
        /* Computed value (was in register): always need lval to load from spill */
        need_lval = 1;
      }

      int use_llocal = 0;
      if (old_is_lval && !old_is_local && !old_is_llocal)
      {
        /* Double indirection: spilled pointer that needs dereferencing */
        use_llocal = 1;
      }

      /* Only preserve is_param for stack-passed parameters (incoming_reg0 < 0).
       * Register-passed parameters spilled to local stack should NOT have is_param. */
      int spilled_param = 0;
      if (old_is_param && interval->incoming_reg0 < 0)
      {
        spilled_param = 1;
      }

      op->is_local = 1;
      op->is_llocal = use_llocal;
      op->is_const = 0;
      op->is_lval = need_lval;
      op->is_param = spilled_param;
      op->tag = IROP_TAG_STACKOFF;
    }
    else if (interval->allocation.r0 != PREG_NONE)
    {
      /* In a register */
      op->is_local = 0;
      op->is_llocal = 0;
      op->is_const = 0;
      op->is_lval = preserve_lval;
      op->is_param = preserve_param;
      op->tag = IROP_TAG_VREG;
    }
  }
  /* No valid vreg: constants, symbols, etc. - IROperand already has the right encoding
   * from the pool. Nothing to do for register allocation. */
}

/* ============================================================================
 * Parameter Register Allocation
 * ============================================================================ */

void tcc_ir_register_allocation_params(TCCIRState *ir)
{
  /* For leaf functions: parameters can stay in registers r0-r3, UNLESS
   * the linear scan allocator already spilled them due to register pressure.
   * For non-leaf functions: parameters arrive in registers but must be
   * stored to stack since r0-r3 are caller-saved.
   * In both cases, we need to track which register each parameter arrives in.
   */
  int argno = 0; // current register number (r0-r3)
  for (int vreg = 0; vreg < ir->next_parameter; ++vreg)
  {
    const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
    IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, encoded_vreg);
    /* is_double for soft-float (LS_REG_TYPE_DOUBLE_SOFT) or is_llong for 64-bit
     */
    int is_64bit = interval && (interval->is_double || interval->is_llong);

    /* If the ABI incoming registers were already set (e.g., by the
     * parameter handling in tcc_ir_add_function_parameters), respect them
     * and only advance argno for subsequent parameters.
     */
    if (interval && (interval->incoming_reg0 >= 0 || interval->incoming_reg1 >= 0))
    {
      argno += is_64bit ? 2 : 1;
      continue;
    }

    /* AAPCS: 64-bit values must be aligned to even register pairs */
    if (is_64bit && (argno & 1))
    {
      argno++; /* skip odd register to align to even */
    }

    if (is_64bit)
    {
      /* 64-bit value (double or long long) takes r0+r1 or r2+r3 */
      if (argno <= 2)
      {
        /* Parameter arrives in registers */
        interval->incoming_reg0 = argno;
        interval->incoming_reg1 = argno + 1;
        /* NOTE: For leaf functions, the linear scanner has already assigned registers.
         * Don't overwrite interval->allocation here - it would clobber the correct allocation
         * with argno (parameter index), which is NOT the same as the physical register number.
         * The prolog will use incoming_reg0/1 to know which registers the parameter arrives in. */
      }
      else
      {
        /* Spilled to caller's stack frame - parameter passed on stack */
        interval->incoming_reg0 = -1;
        interval->incoming_reg1 = -1;
        /* Record where the parameter arrives on the caller's stack frame.
         * Use original_offset if already set by tcc_ir_set_original_offset
         * (from the ABI layout), otherwise compute from argno.
         * The ABI-derived offset is more accurate for complex cases like
         * split structs (REG_STACK) where argno doesn't account for
         * stack words that don't have PARAM vregs.
         */
        if (interval->original_offset == 0)
          interval->original_offset = (argno - 4) * 4;
        /* See 64-bit case above: do not overwrite allocator spill slots with
         * caller-stack offsets.
         */
        interval->allocation.r0 = PREG_NONE;
        interval->allocation.r1 = PREG_NONE;
        interval->allocation.offset = 0;
      }
      argno += 2;
    }
    else
    {
      if (argno <= 3)
      {
        interval->incoming_reg0 = argno;
        interval->incoming_reg1 = -1;
      }
      else
      {
        /* Spilled to caller's stack frame - parameter passed on stack */
        interval->incoming_reg0 = -1;
        interval->incoming_reg1 = -1;
        /* Record where the parameter arrives on the caller's stack frame.
         * Use original_offset if already set by tcc_ir_set_original_offset
         * (from the ABI layout), otherwise compute from argno.
         */
        if (interval->original_offset == 0)
          interval->original_offset = (argno - 4) * 4;
        /* See 64-bit case above: do not overwrite allocator spill slots with
         * caller-stack offsets.
         */
        interval->allocation.r0 = PREG_NONE;
        interval->allocation.r1 = PREG_NONE;
        interval->allocation.offset = 0;
      }
      argno++;
    }
  }
}

void tcc_ir_mark_return_value_incoming_regs(TCCIRState *ir)
{
  if (!ir)
    return;

  /* Scan all instructions to find FUNCCALLVAL that produce return values */
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    /* dest is the vreg that receives the return value */
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.vr < 0 || !tcc_ir_vreg_is_valid(ir, dest.vr))
      continue;

    IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, dest.vr);
    if (!interval)
      continue;

    /* Mark that this vreg arrives in r0 (or r0+r1 for 64-bit returns) */
    interval->incoming_reg0 = 0; /* r0 */
    if (interval->is_llong || interval->is_double)
      interval->incoming_reg1 = 1; /* r1 */
    else
      interval->incoming_reg1 = -1;
  }
}

void tcc_ir_avoid_spilling_stack_passed_params(TCCIRState *ir)
{
  if (!ir)
    return;

  /* Compute which PARAM vregs are stack-passed under AAPCS.
   * We intentionally do this before patching IRLiveInterval allocations,
   * operating on the linear-scan table so we can also shrink `loc`/frame size.
   */
  const int param_count = ir->next_parameter;
  if (param_count <= 0)
    return;

  uint8_t *is_stack_passed = tcc_mallocz((size_t)param_count);
  int argno = 0;
  for (int vreg = 0; vreg < param_count; ++vreg)
  {
    const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
    IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, encoded_vreg);
    if (!interval)
      continue;

    const int is_64bit = interval->is_double || interval->is_llong;
    if (is_64bit && (argno & 1))
      argno++; /* align 64-bit to even reg pair */

    const int in_regs = is_64bit ? (argno <= 2) : (argno <= 3);
    if (!in_regs)
      is_stack_passed[vreg] = 1;

    argno += is_64bit ? 2 : 1;
  }

  /* Rewrite linear-scan results: stack-passed params already have an incoming
   * memory home (caller arg area), so if the allocator spilled them, drop the
   * local spill slot. Also force address-taken stack params to remain in
   * memory (we can use the incoming slot as their addressable home).
   */
  for (int i = 0; i < ir->ls.next_interval_index; ++i)
  {
    LSLiveInterval *ls = &ir->ls.intervals[i];
    if (TCCIR_DECODE_VREG_TYPE((int)ls->vreg) != TCCIR_VREG_TYPE_PARAM)
      continue;
    const int pidx = TCCIR_DECODE_VREG_POSITION((int)ls->vreg);
    if (pidx < 0 || pidx >= param_count)
      continue;
    if (!is_stack_passed[pidx])
      continue;

    /* Stack-passed params live in the caller's argument area. If linear-scan
     * assigned them a register (without spilling), the prolog won't load them
     * into that register, causing incorrect code. Always reset r0/r1 to force
     * them to use the incoming stack location via VT_PARAM path. */
    ls->r0 = PREG_NONE;
    ls->r1 = PREG_NONE;
    ls->stack_location = 0;
  }

  tcc_free(is_stack_passed);
}

/* ============================================================================
 * Code Generation Helpers
 * ============================================================================ */

int tcc_ir_codegen_operand_get(TCCIRState *ir, const IRQuadCompact *q, int slot, SValue *out)
{
  int off;
  int has_operand;

  switch (slot)
  {
  case 0: /* dest */
    has_operand = irop_config[q->op].has_dest;
    off = 0;
    break;
  case 1: /* src1 */
    has_operand = irop_config[q->op].has_src1;
    off = irop_config[q->op].has_dest;
    break;
  case 2: /* src2 */
    has_operand = irop_config[q->op].has_src2;
    off = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
    break;
  default:
    return 0;
  }

  if (!has_operand)
  {
    svalue_init(out);
    return 0;
  }

  /* Read from iroperand_pool and expand to SValue */
  IROperand irop = ir->iroperand_pool[q->operand_base + off];
  iroperand_to_svalue(ir, irop, out);

  /* Apply register allocation */
  tcc_ir_fill_registers(ir, out);

  return 1;
}

IROperand tcc_ir_codegen_dest_get(TCCIRState *ir, const IRQuadCompact *q)
{
  if (!irop_config[q->op].has_dest)
  {
    IROperand empty = {0};
    return empty;
  }
  return ir->iroperand_pool[q->operand_base + 0];
}

IROperand tcc_ir_codegen_src1_get(TCCIRState *ir, const IRQuadCompact *q)
{
  int off = irop_config[q->op].has_dest;
  if (!irop_config[q->op].has_src1)
  {
    IROperand empty = {0};
    return empty;
  }
  return ir->iroperand_pool[q->operand_base + off];
}

IROperand tcc_ir_codegen_src2_get(TCCIRState *ir, const IRQuadCompact *q)
{
  int off = irop_config[q->op].has_dest + irop_config[q->op].has_src1;
  if (!irop_config[q->op].has_src2)
  {
    IROperand empty = {0};
    return empty;
  }
  return ir->iroperand_pool[q->operand_base + off];
}

void tcc_ir_codegen_dest_set(TCCIRState *ir, const IRQuadCompact *q, IROperand irop)
{
  if (!irop_config[q->op].has_dest)
    return;
  ir->iroperand_pool[q->operand_base + 0] = irop;
}

void tcc_ir_codegen_reg_fill(TCCIRState *ir, SValue *sv)
{
  tcc_ir_fill_registers(ir, sv);
}

void tcc_ir_codegen_reg_fill_op(TCCIRState *ir, IROperand *op)
{
  tcc_ir_fill_registers_ir(ir, op);
}

int tcc_ir_codegen_reg_get(TCCIRState *ir, int vreg)
{
  if (!ir || !tcc_ir_vreg_is_valid(ir, vreg))
    return PREG_NONE;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (!interval)
    return PREG_NONE;
  return interval->allocation.r0;
}

void tcc_ir_codegen_reg_set(TCCIRState *ir, int vreg, int preg)
{
  if (!ir || !tcc_ir_vreg_is_valid(ir, vreg))
    return;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
    interval->allocation.r0 = preg;
}

void tcc_ir_codegen_params_setup(TCCIRState *ir)
{
  tcc_ir_register_allocation_params(ir);
}

void tcc_ir_codegen_cmp_jmp_set(TCCIRState *ir)
{
  if (ir == NULL)
    return;
  /* Guard against invalid vtop - can happen with empty structs */
  extern SValue _vstack[];
  if (vtop < _vstack + 1) /* vstack is defined as (_vstack + 1) */
    return;
  int v = vtop->r & VT_VALMASK;
  if (v == VT_CMP)
  {
    SValue src, dest;
    int jtrue = vtop->jtrue;
    int jfalse = vtop->jfalse;
    svalue_init(&src);
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.type.t = VT_INT;
    dest.pr0_reg = PREG_REG_NONE;
    dest.pr0_spilled = 0;
    dest.pr1_reg = PREG_REG_NONE;
    dest.pr1_spilled = 0;

    if (jtrue >= 0 || jfalse >= 0)
    {
      /* We have pending jump chains - need to merge them with the comparison */
      SValue jump_dest;
      svalue_init(&jump_dest);
      jump_dest.vr = -1;
      jump_dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */

      /* Generate SETIF for the comparison part */
      src.vr = -1;
      src.r = VT_CONST;
      src.c.i = vtop->cmp_op;
      tcc_ir_put(ir, TCCIR_OP_SETIF, &src, NULL, &dest);

      /* Jump to end */
      jump_dest.c.i = -1; /* will be patched */
      int end_jump = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jump_dest);

      /* Patch jtrue chain to here - set dest = 1 */
      if (jtrue >= 0)
      {
        tcc_ir_backpatch_to_here(ir, jtrue);
        src.r = VT_CONST;
        src.c.i = 1;
        src.pr0_reg = PREG_REG_NONE;
        src.pr0_spilled = 0;
        src.pr1_reg = PREG_REG_NONE;
        src.pr1_spilled = 0;
        tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
        if (jfalse >= 0)
        {
          /* Jump over the jfalse handler */
          jump_dest.c.i = -1; /* will be patched */
          int skip_jump = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jump_dest);
          /* Patch jfalse chain to here - set dest = 0 */
          tcc_ir_backpatch_to_here(ir, jfalse);
          src.r = VT_CONST;
          src.c.i = 0;
          tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
          /* Patch skip_jump to end */
          tcc_ir_set_dest_jump_target(ir, skip_jump, ir->next_instruction_index);
        }
      }
      else if (jfalse >= 0)
      {
        tcc_ir_backpatch_to_here(ir, jfalse);
        src.r = VT_CONST;
        src.c.i = 0;
        tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
      }

      /* Patch end_jump to here */
      tcc_ir_set_dest_jump_target(ir, end_jump, ir->next_instruction_index);
      tcc_ir_codegen_bb_start(ir);
    }
    else
    {
      /* Simple case - just SETIF */
      src.vr = -1;
      src.r = VT_CONST;
      src.c.i = vtop->cmp_op;
      tcc_ir_put(ir, TCCIR_OP_SETIF, &src, NULL, &dest);
    }

    vtop->vr = dest.vr;
    vtop->r = 0;
  }
  else if ((v & ~1) == VT_JMP)
  {
    SValue dest, src1;
    SValue jump_dest;
    int t;
    svalue_init(&src1);
    svalue_init(&dest);
    svalue_init(&jump_dest);
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.type.t = VT_INT;
    src1.vr = -1;
    src1.r = VT_CONST;
    t = v & 1;
    src1.c.i = t;
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src1, NULL, &dest);

    /* Default path: result already set to `t`. Skip the alternate assignment.
       If the jump chain is taken, execution lands at the alternate assignment
       which flips the result to `t ^ 1`. */
    jump_dest.vr = -1;
    jump_dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    jump_dest.c.i = -1;     /* patched to end */
    int end_jump = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jump_dest);

    tcc_ir_backpatch_to_here(ir, vtop->c.i);
    src1.c.i = t ^ 1;
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src1, NULL, &dest);
    IROperand end_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[end_jump]);
    end_dest.u.imm32 = ir->next_instruction_index;
    tcc_ir_op_set_dest(ir, &ir->compact_instructions[end_jump], end_dest);
    vtop->vr = dest.vr;
    vtop->r = 0;
  }
}

void tcc_ir_codegen_backpatch(TCCIRState *ir, int jump_idx, int target_address)
{
  tcc_ir_backpatch(ir, jump_idx, target_address);
}

void tcc_ir_codegen_backpatch_here(TCCIRState *ir, int jump_idx)
{
  tcc_ir_backpatch_to_here(ir, jump_idx);
}

void tcc_ir_codegen_backpatch_first(TCCIRState *ir, int jump_idx, int target_address)
{
  tcc_ir_backpatch_first(ir, jump_idx, target_address);
}

int tcc_ir_codegen_jump_append(TCCIRState *ir, int chain, int jump)
{
  return tcc_ir_gjmp_append(ir, chain, jump);
}

int tcc_ir_codegen_test_gen(TCCIRState *ir, int invert, int test)
{
  int v;
  v = vtop->r & VT_VALMASK;
  if (v == VT_CMP)
  {
    SValue src, dest;
    int jtrue = vtop->jtrue;
    int jfalse = vtop->jfalse;

    svalue_init(&src);
    svalue_init(&dest);
    src.vr = -1;
    src.r = VT_CONST;
    /* Use cmp_op and invert if needed. In TCC, comparison tokens are designed
     * so that XORing with 1 inverts them (e.g., TOK_EQ ^ 1 = TOK_NE) */
    int cond = vtop->cmp_op ^ invert;
    /* Validate condition is a valid comparison token */
    src.c.i = cond;
    dest.vr = -1;
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = test;
    test = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &src, NULL, &dest);

    /* Handle pending jump chains - merge with the appropriate chain */
    if (invert)
    {
      /* inv=1: we want to jump when condition is false */
      /* Merge any existing "jump-on-false" chain with the new jump.
       * Patch the opposite chain (jump-on-true) to fall through here. */
      if (jfalse >= 0)
      {
        tcc_ir_backpatch_first(ir, jfalse, test);
        test = jfalse;
      }
      if (jtrue >= 0)
      {
        tcc_ir_backpatch_to_here(ir, jtrue);
      }
    }
    else
    {
      /* inv=0: we want to jump when condition is true */
      /* Merge any existing "jump-on-true" chain with the new jump.
       * Patch the opposite chain (jump-on-false) to fall through here. */
      if (jtrue >= 0)
      {
        tcc_ir_backpatch_first(ir, jtrue, test);
        test = jtrue;
      }
      if (jfalse >= 0)
      {
        tcc_ir_backpatch_to_here(ir, jfalse);
      }
    }
  }
  else if (v == VT_JMP || v == VT_JMPI)
  {
    if ((v & 1) == invert)
    {
      if (vtop->c.i == -1)
      {
        vtop->c.i = test;
      }
      else
      {
        if (test != -1)
        {
          tcc_ir_backpatch_first(ir, vtop->c.i, test);
        }
        test = vtop->c.i;
      }
    }
    else
    {
      SValue dest;
      svalue_init(&dest);
      dest.vr = -1;
      dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
      dest.c.i = test;
      test = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      tcc_ir_backpatch_to_here(ir, vtop->c.i);
    }
  }
  else
  {
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    {
      if ((vtop->c.i != 0) != invert)
      {
        SValue dest;
        svalue_init(&dest);
        dest.vr = -1;
        dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
        dest.c.i = test;
        test = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      }
    }
    else
    {
      /* If we're testing a memory lvalue (e.g. tabl[i]), load the value first.
       * Otherwise we end up testing the address, which is almost always non-zero
       * and can lead to invalid indirect calls.
       */
      tcc_ir_put(ir, TCCIR_OP_TEST_ZERO, &vtop[0], NULL, NULL);
      vtop->r = VT_CMP;
      vtop->cmp_op = TOK_NE;
      vtop->jtrue = -1;  /* -1 = no chain */
      vtop->jfalse = -1; /* -1 = no chain */
      return tcc_ir_codegen_test_gen(ir, invert, test);
    }
  }
  --vtop;
  return test;
}

void tcc_ir_codegen_bb_start(TCCIRState *ir)
{
  if (ir)
    ir->basic_block_start = 1;
}

/* ============================================================================
 * Return Value Handling
 * ============================================================================ */

void tcc_ir_codegen_drop_return(TCCIRState *ir)
{
  if (ir->next_instruction_index == 0)
  {
    return;
  }
  IRQuadCompact *last_instr = &ir->compact_instructions[ir->next_instruction_index - 1];

  if (last_instr->op == TCCIR_OP_FUNCCALLVAL)
  {
    /* Only drop return values that are assigned to temporaries.
     * If coalescing redirected the dest to a VAR, the value IS used
     * and should not be dropped. */
    IROperand dest = tcc_ir_op_get_dest(ir, last_instr);
    if (TCCIR_DECODE_VREG_TYPE(dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      if (tcc_ir_vreg_is_valid(ir, dest.vr))
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest.vr);
        interval->start = INTERVAL_NOT_STARTED;
        interval->end = 0;
      }
      irop_set_vreg(&dest, -1);
      dest.vr = -1;
      tcc_ir_op_set_dest(ir, last_instr, dest);
    }
  }
}

/* ============================================================================
 * Inline Assembly Code Generation
 * ============================================================================ */

#ifdef CONFIG_TCC_ASM
static void tcc_ir_codegen_inline_asm_by_id(TCCIRState *ir, int id)
{
  if (!ir)
    return;
  if (id < 0 || id >= ir->inline_asm_count)
    tcc_error("IR: invalid inline asm id");

  TCCIRInlineAsm *ia = &ir->inline_asms[id];
  if (!ia->asm_str)
    tcc_error("IR: inline asm payload missing");

  const int nb_operands = ia->nb_operands;
  const int nb_labels = ia->nb_labels;
  if (nb_operands < 0 || nb_operands > MAX_ASM_OPERANDS || nb_operands + nb_labels > MAX_ASM_OPERANDS)
    tcc_error("IR: invalid asm operand count");

  ASMOperand ops[MAX_ASM_OPERANDS];
  SValue vals[MAX_ASM_OPERANDS];
  memset(ops, 0, sizeof(ops));
  memset(vals, 0, sizeof(vals));

  memcpy(ops, ia->operands, sizeof(ASMOperand) * (nb_operands + nb_labels));
  for (int i = 0; i < nb_operands; ++i)
  {
    vals[i] = ia->values[i];
    tcc_ir_fill_registers(ir, &vals[i]);
    ops[i].vt = &vals[i];
  }
  for (int i = nb_operands; i < nb_operands + nb_labels; ++i)
    ops[i].vt = NULL;

  uint8_t clobber_regs[NB_ASM_REGS];
  memcpy(clobber_regs, ia->clobber_regs, sizeof(clobber_regs));

  tcc_asm_emit_inline(ops, nb_operands, ia->nb_outputs, nb_labels, clobber_regs, ia->asm_str, ia->asm_len,
                      ia->must_subst);
}

static void tcc_ir_codegen_inline_asm_ir(TCCIRState *ir, IROperand dest_irop)
{
  if (!ir)
    return;
  const int id = (int)irop_get_imm64_ex(ir, dest_irop);
  tcc_ir_codegen_inline_asm_by_id(ir, id);
}
#endif

/* ============================================================================
 * Jump Backpatching
 * ============================================================================ */

static void tcc_ir_codegen_backpatch_jumps(TCCIRState *ir, uint32_t *ir_to_code_mapping)
{
  IRQuadCompact *q;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target_ir = irop_is_none(dest) ? -1 : (int)dest.u.imm32;
      /* Skip unpatched jumps (target is -1 or truly out of range)
       * Note: target_ir == ir->next_instruction_index is valid (epilogue) */
      if (target_ir < 0 || target_ir > ir->next_instruction_index)
        continue;
      const int instruction_address = ir_to_code_mapping[i];
      const int target_address = ir_to_code_mapping[target_ir];
      tcc_gen_machine_backpatch_jump(instruction_address, target_address);
    }
  }
}

/* ============================================================================
 * Main Code Generation Loop
 * ============================================================================ */

void tcc_ir_codegen_generate(TCCIRState *ir)
{
  IRQuadCompact *cq;
  int drop_return_value = 0;

  /* Print vreg statistics for size optimization analysis */
  {
    int local_count = ir->next_local_variable;
    int temp_count = ir->next_temporary_variable;
    int param_count = ir->next_parameter;
    int total_vregs = local_count + temp_count + param_count;
    if (total_vregs > 1000) /* Only print for large functions */
      fprintf(stderr, "[VREG STATS] locals=%d temps=%d params=%d total=%d (max_encoded=%d)\n", local_count, temp_count,
              param_count, total_vregs,
              (local_count > temp_count ? local_count : temp_count) > param_count
                  ? (local_count > temp_count ? local_count : temp_count)
                  : param_count);
  }

  /* `&&label` stores label positions as IR indices BEFORE DCE/compaction.
   * Build a mapping for original indices, not just the compacted array indices.
   */
  int max_orig_index = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].orig_index > max_orig_index)
      max_orig_index = ir->compact_instructions[i].orig_index;
  }
  if (max_orig_index < 0)
    max_orig_index = 0;

  /* +1 to include epilogue when needed.
   * Keep this mapping available after codegen (e.g. for &&label). */
  if (ir->ir_to_code_mapping)
  {
    tcc_free(ir->ir_to_code_mapping);
    ir->ir_to_code_mapping = NULL;
    ir->ir_to_code_mapping_size = 0;
  }
  ir->ir_to_code_mapping_size = ir->next_instruction_index + 1;
  ir->ir_to_code_mapping = tcc_mallocz(sizeof(uint32_t) * ir->ir_to_code_mapping_size);
  uint32_t *ir_to_code_mapping = ir->ir_to_code_mapping;

  if (ir->orig_ir_to_code_mapping)
  {
    tcc_free(ir->orig_ir_to_code_mapping);
    ir->orig_ir_to_code_mapping = NULL;
    ir->orig_ir_to_code_mapping_size = 0;
  }
  /* +1 extra slot for a synthetic epilogue mapping.
   * Use 0xFFFFFFFF sentinel to distinguish "unmapped" from offset 0. */
  ir->orig_ir_to_code_mapping_size = max_orig_index + 2;
  ir->orig_ir_to_code_mapping = tcc_malloc(sizeof(uint32_t) * ir->orig_ir_to_code_mapping_size);
  uint32_t *orig_ir_to_code_mapping = ir->orig_ir_to_code_mapping;
  memset(orig_ir_to_code_mapping, 0xFF, sizeof(uint32_t) * ir->orig_ir_to_code_mapping_size);
  /* Track addresses of return jumps for later backpatching to epilogue */
  int *return_jump_addrs = tcc_malloc(sizeof(int) * ir->next_instruction_index);
  int num_return_jumps = 0;

  /* Clear spill cache at function start */
  tcc_ir_spill_cache_clear(&ir->spill_cache);

  /* Some peephole optimizations (LOAD/ASSIGN -> RETURNVALUE in R0, and skipping
   * RETURNVALUE moves) are only valid when RETURNVALUE is reached by straight-line
   * fallthrough from the immediately preceding instruction.
   *
   * If RETURNVALUE is a jump target (a control-flow merge), those peepholes can
   * become incorrect: the preceding instruction might not execute on all paths,
   * leaving the return value in a non-return register.
   *
   * Track which IR instruction indices are jump targets to guard these peepholes.
   */
  uint8_t *has_incoming_jump = tcc_mallocz(ir->next_instruction_index ? ir->next_instruction_index : 1);
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    IRQuadCompact *p = &ir->compact_instructions[i];
    if (p->op == TCCIR_OP_JUMP || p->op == TCCIR_OP_JUMPIF)
    {
      /* Read jump target from IROperand pool */
      IROperand dest_irop = tcc_ir_op_get_dest(ir, p);
      int target = (int)dest_irop.u.imm32;
      if (target >= 0 && target < ir->next_instruction_index)
        has_incoming_jump[target] = 1;
    }
  }

  /* Reserve outgoing call stack args area at the very bottom of the frame.
   * This ensures prepared-call stack args are at call-time SP.
   */
  if (ir->call_outgoing_size > 0)
  {
    loc -= ir->call_outgoing_size;
    ir->call_outgoing_base = loc;
  }

  int stack_size = (-loc + 7) & ~7; // align to 8 bytes

  /* ============================================================================
   * DRY RUN PASS: Analyze scratch register needs before emitting prologue
   * ============================================================================
   * This discovers what scratch registers will be needed during code generation,
   * allowing us to include them in the prologue (avoiding push/pop in loops).
   */
  int original_leaffunc = ir->leaffunc;
  uint32_t extra_prologue_regs = 0;

#if 1 /* DRY_RUN_ENABLED */
  /* Initialize dry-run state and branch optimization */
  tcc_gen_machine_dry_run_init();
  tcc_gen_machine_branch_opt_init();
  tcc_gen_machine_dry_run_start();

  /* Reset scratch state for clean dry-run */
  tcc_gen_machine_reset_scratch_state();
  tcc_ir_spill_cache_clear(&ir->spill_cache);

  /* Save state that will be modified during dry run */
  int saved_ind = ind;
  int saved_codegen_idx = ir->codegen_instruction_idx;
  int saved_loc = loc;
  int saved_call_outgoing_base = ir->call_outgoing_base;

  /* Run through all instructions without emitting.
   * We call the actual codegen functions, but ot() is a no-op during dry-run.
   * This ensures we exercise the exact same code paths for scratch allocation. */
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    ir->codegen_instruction_idx = i;
    cq = &ir->compact_instructions[i];

    /* Record address mapping for branch optimizer analysis */
    ir_to_code_mapping[i] = ind;

    /* Skip marker ops */
    if (cq->op == TCCIR_OP_ASM_INPUT || cq->op == TCCIR_OP_ASM_OUTPUT || cq->op == TCCIR_OP_NOP ||
        cq->op == TCCIR_OP_INLINE_ASM)
      continue;

    /* Determine materialization needs (same logic as real pass) */
    bool need_src1_value = false;
    bool need_src2_value = false;
    bool need_dest_value = false;
    bool need_src1_addr = false;
    bool need_src2_addr = false;
    bool need_dest_addr = false;
    bool need_src1_in_reg = false;
    bool need_src2_in_reg = false;

    switch (cq->op)
    {
    case TCCIR_OP_LOAD:
      need_src1_addr = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_STORE:
      need_src1_value = true;
      need_dest_addr = true;
      break;
    case TCCIR_OP_LOAD_INDEXED:
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_STORE_INDEXED:
      need_src1_value = true;
      need_dest_addr = true;
      need_src2_value = true;
      break;
    case TCCIR_OP_LOAD_POSTINC:
      need_src1_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_STORE_POSTINC:
      need_src1_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_ASSIGN:
      need_src1_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_MUL:
    case TCCIR_OP_DIV:
    case TCCIR_OP_UDIV:
    case TCCIR_OP_IMOD:
    case TCCIR_OP_UMOD:
    case TCCIR_OP_UMULL:
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      need_src1_in_reg = true;
      need_src2_in_reg = true;
      break;
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    case TCCIR_OP_SHL:
    case TCCIR_OP_SHR:
    case TCCIR_OP_SAR:
    case TCCIR_OP_AND:
    case TCCIR_OP_OR:
    case TCCIR_OP_XOR:
    case TCCIR_OP_CMP:
    case TCCIR_OP_MLA:
    case TCCIR_OP_ADC_GEN:
    case TCCIR_OP_ADC_USE:
    case TCCIR_OP_TEST_ZERO:
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_RETURNVALUE:
      need_src1_value = true;
      break;
    case TCCIR_OP_LEA:
      need_src1_addr = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_SETIF:
      need_dest_value = true;
      break;
    case TCCIR_OP_FUNCCALLVAL:
      need_dest_value = true;
      /* fall through */
    case TCCIR_OP_FUNCCALLVOID:
      need_src1_value = true;
      break;
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCPARAMVOID:
      need_src1_value = true;
      break;
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      need_src1_value = true;
      break;
    case TCCIR_OP_IJUMP:
      need_src1_value = true;
      break;
    case TCCIR_OP_BOOL_OR:
    case TCCIR_OP_BOOL_AND:
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_FADD:
    case TCCIR_OP_FSUB:
    case TCCIR_OP_FMUL:
    case TCCIR_OP_FDIV:
    case TCCIR_OP_FNEG:
    case TCCIR_OP_FCMP:
    case TCCIR_OP_CVT_FTOF:
    case TCCIR_OP_CVT_ITOF:
    case TCCIR_OP_CVT_FTOI:
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_SWITCH_TABLE:
      need_src1_value = true; /* Index vreg needs materialization */
      /* src2 contains table_id which is an immediate, not a vreg */
      break;
    default:
      break;
    }

    /* Get operand copies from iroperand_pool */
    IROperand src1_ir = tcc_ir_op_get_src1(ir, cq);
    IROperand src2_ir = tcc_ir_op_get_src2(ir, cq);
    IROperand dest_ir = tcc_ir_op_get_dest(ir, cq);

    /* Apply register allocation to operands */
    if (irop_get_tag(src1_ir) != IROP_TAG_NONE)
      tcc_ir_fill_registers_ir(ir, &src1_ir);
    if (irop_get_tag(src2_ir) != IROP_TAG_NONE)
      tcc_ir_fill_registers_ir(ir, &src2_ir);
    if (irop_get_tag(dest_ir) != IROP_TAG_NONE)
      tcc_ir_fill_registers_ir(ir, &dest_ir);

    TCCMaterializedValue mat_src1 = {0};
    TCCMaterializedValue mat_src2 = {0};
    TCCMaterializedAddr mat_src1_addr = {0};
    TCCMaterializedAddr mat_src2_addr = {0};
    TCCMaterializedAddr mat_dest_addr = {0};
    TCCMaterializedDest mat_dest = {0};

    if (need_src1_value)
      tcc_ir_materialize_value_ir(ir, &src1_ir, &mat_src1);
    else if (need_src1_addr)
      tcc_ir_materialize_addr_ir(ir, &src1_ir, &mat_src1_addr, dest_ir.pr0_reg);

    if (need_src2_value)
      tcc_ir_materialize_value_ir(ir, &src2_ir, &mat_src2);
    else if (need_src2_addr)
      tcc_ir_materialize_addr_ir(ir, &src2_ir, &mat_src2_addr, dest_ir.pr0_reg);

    if (need_dest_value)
      tcc_ir_materialize_dest_ir(ir, &dest_ir, &mat_dest);
    else if (need_dest_addr)
      tcc_ir_materialize_addr_ir(ir, &dest_ir, &mat_dest_addr, PREG_NONE);

    /* For operations that require register-only operands, materialize constants to registers */
    TCCMaterializedValue mat_src1_reg = {0};
    TCCMaterializedValue mat_src2_reg = {0};
    if (need_src1_in_reg && !mat_src1.used_scratch)
      tcc_ir_materialize_const_to_reg_ir(ir, &src1_ir, &mat_src1_reg);
    if (need_src2_in_reg && !mat_src2.used_scratch)
      tcc_ir_materialize_const_to_reg_ir(ir, &src2_ir, &mat_src2_reg);

    /* Call the actual codegen function - ot() will be a no-op in dry-run mode,
     * but scratch allocation inside these functions will still be recorded */
    switch (cq->op)
    {
    case TCCIR_OP_LOAD:
      tcc_gen_machine_load_op(dest_ir, src1_ir);
      break;
    case TCCIR_OP_STORE:
      tcc_gen_machine_store_op(dest_ir, src1_ir, cq->op);
      break;
    case TCCIR_OP_LOAD_INDEXED:
    {
      IROperand base_op = src1_ir;
      IROperand index_op = src2_ir;
      IROperand scale_op = tcc_ir_op_get_scale(ir, cq);
      tcc_gen_machine_load_indexed_op(dest_ir, base_op, index_op, scale_op);
      break;
    }
    case TCCIR_OP_STORE_INDEXED:
    {
      IROperand base_op = dest_ir;
      IROperand index_op = src2_ir;
      IROperand scale_op = tcc_ir_op_get_scale(ir, cq);
      tcc_gen_machine_store_indexed_op(base_op, index_op, scale_op, src1_ir);
      break;
    }
    case TCCIR_OP_LOAD_POSTINC:
    {
      IROperand ptr_op = src1_ir;
      IROperand offset_op = tcc_ir_op_get_scale(ir, cq);
      tcc_gen_machine_load_postinc_op(dest_ir, ptr_op, offset_op);
      break;
    }
    case TCCIR_OP_STORE_POSTINC:
    {
      IROperand ptr_op = dest_ir;
      IROperand value_op = src1_ir;
      IROperand offset_op = tcc_ir_op_get_scale(ir, cq);
      tcc_gen_machine_store_postinc_op(ptr_op, value_op, offset_op);
      break;
    }
    case TCCIR_OP_LEA:
      tcc_gen_machine_lea_op(dest_ir, src1_ir, cq->op);
      break;
    case TCCIR_OP_ASSIGN:
      tcc_gen_machine_assign_op(dest_ir, src1_ir, cq->op);
      break;
    case TCCIR_OP_RETURNVALUE:
      tcc_gen_machine_return_value_op(src1_ir, cq->op);
      break;
    case TCCIR_OP_RETURNVOID:
      /* No scratch allocation needed */
      break;
    case TCCIR_OP_JUMP:
      /* Record branch for optimization analysis (ot() is no-op during dry-run) */
      tcc_gen_machine_jump_op(cq->op, dest_ir, i);
      break;
    case TCCIR_OP_JUMPIF:
      /* Record branch for optimization analysis (ot() is no-op during dry-run) */
      tcc_gen_machine_conditional_jump_op(src1_ir, cq->op, dest_ir, i);
      break;
    case TCCIR_OP_MUL:
    case TCCIR_OP_MLA:
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    case TCCIR_OP_CMP:
    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_SHL:
    case TCCIR_OP_SHR:
    case TCCIR_OP_OR:
    case TCCIR_OP_AND:
    case TCCIR_OP_XOR:
    case TCCIR_OP_DIV:
    case TCCIR_OP_UDIV:
    case TCCIR_OP_IMOD:
    case TCCIR_OP_UMOD:
    case TCCIR_OP_SAR:
    case TCCIR_OP_UMULL:
    case TCCIR_OP_ADC_GEN:
    case TCCIR_OP_ADC_USE:
      tcc_gen_machine_data_processing_op(src1_ir, src2_ir, dest_ir, cq->op);
      break;
    case TCCIR_OP_IJUMP:
      tcc_gen_machine_indirect_jump_op(src1_ir);
      break;
    case TCCIR_OP_SWITCH_TABLE:
    {
      /* Dry-run: compute exact table size so branch offsets are accurate.
       * The real pass emits TBB/TBH (4 bytes) + 1 or 2 bytes per entry + alignment. */
      int table_id = (int)irop_get_imm64_ex(ir, src2_ir);
      TCCIRSwitchTable *table = &ir->switch_tables[table_id];
      int use_tbh = (table->num_entries > 255);
      int table_data_size = use_tbh ? table->num_entries * 2 : table->num_entries + (table->num_entries & 1);
      ind += 4;               /* TBB/TBH instruction */
      ind += table_data_size; /* Jump table entries + alignment */
      break;
    }
    case TCCIR_OP_SETIF:
      tcc_gen_machine_setif_op(dest_ir, src1_ir, cq->op);
      break;
    case TCCIR_OP_BOOL_OR:
    case TCCIR_OP_BOOL_AND:
      tcc_gen_machine_bool_op(dest_ir, src1_ir, src2_ir, cq->op);
      break;
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_FUNCCALLVAL:
      tcc_gen_machine_func_call_op(src1_ir, src2_ir, dest_ir, 0, ir, i);
      break;
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCPARAMVOID:
      tcc_gen_machine_func_parameter_op(src1_ir, src2_ir, cq->op);
      break;
    case TCCIR_OP_FADD:
    case TCCIR_OP_FSUB:
    case TCCIR_OP_FMUL:
    case TCCIR_OP_FDIV:
    case TCCIR_OP_FNEG:
    case TCCIR_OP_FCMP:
    case TCCIR_OP_CVT_FTOF:
    case TCCIR_OP_CVT_ITOF:
    case TCCIR_OP_CVT_FTOI:
      tcc_gen_machine_fp_op(dest_ir, src1_ir, src2_ir, cq->op);
      break;
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      tcc_gen_machine_vla_op(dest_ir, src1_ir, src2_ir, cq->op);
      break;
    default:
      /* Unknown op - skip */
      break;
    }

    /* Release any scratch registers allocated during materialization */
    if (mat_src1.used_scratch)
      tcc_machine_release_scratch(&mat_src1.scratch);
    if (mat_src2.used_scratch)
      tcc_machine_release_scratch(&mat_src2.scratch);
    if (mat_src1_addr.used_scratch)
      tcc_machine_release_scratch(&mat_src1_addr.scratch);
    if (mat_src2_addr.used_scratch)
      tcc_machine_release_scratch(&mat_src2_addr.scratch);
    if (mat_dest_addr.used_scratch)
      tcc_machine_release_scratch(&mat_dest_addr.scratch);
    if (mat_src1_reg.used_scratch)
      tcc_machine_release_scratch(&mat_src1_reg.scratch);
    if (mat_src2_reg.used_scratch)
      tcc_machine_release_scratch(&mat_src2_reg.scratch);

    /* Clean up scratch register state */
    tcc_gen_machine_end_instruction();
  }

  /* End dry-run and analyze results */
  tcc_gen_machine_dry_run_end();

  /* Analyze branch offsets and select optimal encodings */
  tcc_gen_machine_branch_opt_analyze(ir_to_code_mapping, ir->next_instruction_index);

  /* Check if LR was pushed during dry run in a leaf function */
  if (original_leaffunc && tcc_gen_machine_dry_run_get_lr_push_count() > 0)
  {
    /* LR was pushed in loop - save at prologue instead */
    extra_prologue_regs |= (1 << 14); /* R_LR */
    /* NOTE: We don't modify ir->leaffunc here because optimizations may depend on it.
     * The extra_prologue_regs will ensure LR is pushed in the prologue, making it
     * available as scratch without push/pop in loops, which is the main goal. */
  }

  /* Restore state for real code generation */
  ind = saved_ind;
  loc = saved_loc;
  ir->call_outgoing_base = saved_call_outgoing_base;
  ir->codegen_instruction_idx = saved_codegen_idx;

  /* Reset scratch state for real pass */
  tcc_gen_machine_reset_scratch_state();

  /* Clear caches for fresh start - dry-run may have recorded entries
   * but the actual instructions were never emitted */
  tcc_ir_spill_cache_clear(&ir->spill_cache);
  tcc_ir_opt_fp_cache_clear(ir);
#endif /* DRY_RUN_DISABLED */

  /* ============================================================================
   * REAL CODE GENERATION PASS
   * ============================================================================
   */

  // generate prolog (with extra registers if needed)
  (void)original_leaffunc; /* May be unused when dry-run is disabled */
  if (!ir->naked)
    tcc_gen_machine_prolog(ir->leaffunc, ir->ls.dirty_registers, stack_size, extra_prologue_regs);

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    drop_return_value = 0;
    cq = &ir->compact_instructions[i];

    /* Default: no extra scratch constraints for this instruction. */
    ir->codegen_materialize_scratch_flags = 0;

    /* Track current instruction for scratch register allocation */
    ir->codegen_instruction_idx = i;

    ir_to_code_mapping[i] = ind;

    if (cq->orig_index >= 0 && cq->orig_index < ir->orig_ir_to_code_mapping_size)
      orig_ir_to_code_mapping[cq->orig_index] = ind;

    // emit debug line info for this IR instruction AFTER recording ind
    tcc_debug_line_num(tcc_state, cq->line_num);

    /* Get operand copies from iroperand_pool (compact representation) */
    IROperand src1_ir = tcc_ir_op_get_src1(ir, cq);
    IROperand src2_ir = tcc_ir_op_get_src2(ir, cq);
    IROperand dest_ir = tcc_ir_op_get_dest(ir, cq);

    /* Peephole for LOAD/ASSIGN/LOAD_INDEXED followed by RETURNVALUE:
     * Update the live interval to use R0 BEFORE register allocation.
     * This ensures the load result goes directly to the return register.
     */
    if (cq->op == TCCIR_OP_LOAD || cq->op == TCCIR_OP_ASSIGN || cq->op == TCCIR_OP_LOAD_INDEXED)
    {
      const IRQuadCompact *ir_next = (i + 1 < ir->next_instruction_index) ? &ir->compact_instructions[i + 1] : NULL;
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE && !has_incoming_jump[i + 1])
      {
        IROperand next_src1 = tcc_ir_op_get_src1(ir, ir_next);
        int next_vr = irop_get_vreg(next_src1);
        int dest_vr = irop_get_vreg(dest_ir);
        if (next_vr == dest_vr && next_vr >= 0)
        {
          IRLiveInterval *li = tcc_ir_get_live_interval(ir, dest_vr);
          if (li && li->allocation.r0 != REG_IRET)
          {
            li->allocation.r0 = REG_IRET;
            li->allocation.offset = 0;
            if (li->is_llong || li->is_double)
              li->allocation.r1 = REG_IRE2;
          }
        }
      }
    }

    /* Apply register allocation to operands */
    if (irop_get_tag(src1_ir) != IROP_TAG_NONE)
      tcc_ir_fill_registers_ir(ir, &src1_ir);
    if (irop_get_tag(src2_ir) != IROP_TAG_NONE)
      tcc_ir_fill_registers_ir(ir, &src2_ir);
    if (irop_get_tag(dest_ir) != IROP_TAG_NONE)
      tcc_ir_fill_registers_ir(ir, &dest_ir);

    bool need_src1_value = false;
    bool need_src2_value = false;
    bool need_dest_value = false;
    bool need_src1_addr = false;
    bool need_src2_addr = false;
    bool need_dest_addr = false;
    bool need_src1_in_reg = false; /* Operand must be in register, not immediate */
    bool need_src2_in_reg = false;

    switch (cq->op)
    {
    case TCCIR_OP_MUL:
    case TCCIR_OP_DIV:
    case TCCIR_OP_UDIV:
    case TCCIR_OP_IMOD:
    case TCCIR_OP_UMOD:
    case TCCIR_OP_UMULL:
      /* These operations require register-only operands (no immediate forms) */
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      need_src1_in_reg = true;
      need_src2_in_reg = true;
      break;
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    case TCCIR_OP_AND:
    case TCCIR_OP_OR:
    case TCCIR_OP_XOR:
    case TCCIR_OP_SHL:
    case TCCIR_OP_SHR:
    case TCCIR_OP_SAR:
    case TCCIR_OP_ADC_GEN:
    case TCCIR_OP_ADC_USE:
    case TCCIR_OP_BOOL_OR:
    case TCCIR_OP_BOOL_AND:
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_CMP:
      need_src1_value = true;
      need_src2_value = true;
      break;
    case TCCIR_OP_TEST_ZERO:
      need_src1_value = true;
      break;
    case TCCIR_OP_FADD:
    case TCCIR_OP_FSUB:
    case TCCIR_OP_FMUL:
    case TCCIR_OP_FDIV:
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_FNEG:
    case TCCIR_OP_CVT_FTOF:
    case TCCIR_OP_CVT_ITOF:
    case TCCIR_OP_CVT_FTOI:
      need_src1_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_LOAD:
      need_src1_addr = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_STORE:
      need_src1_value = true;
      need_dest_addr = true;
      break;
    case TCCIR_OP_ASSIGN:
      need_src1_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_LEA:
      need_src1_addr = true; /* We need the address of src1, not its value */
      need_dest_value = true;
      break;
    case TCCIR_OP_IJUMP:
      need_src1_value = true;
      break;
    case TCCIR_OP_SETIF:
      need_dest_value = true;
      break;
    case TCCIR_OP_RETURNVALUE:
      need_src1_value = true;
      break;
    case TCCIR_OP_FUNCPARAMVAL:
      /* FUNCPARAMVAL is a marker op only.
       * Argument placement is handled when we reach the owning FUNCCALL*,
       * so do not materialize anything here (would just emit dead loads).
       */
      break;
    case TCCIR_OP_FUNCCALLVAL:
      need_dest_value = true;
      /* fall through */
    case TCCIR_OP_FUNCCALLVOID:
    {
      need_src1_value = true;
      break;
    }
    case TCCIR_OP_VLA_ALLOC:
      need_src1_value = true;
      break;
    default:
      break;
    }

    TCCMaterializedValue mat_src1 = {0};
    TCCMaterializedValue mat_src2 = {0};
    TCCMaterializedAddr mat_src1_addr = {0};
    TCCMaterializedAddr mat_src2_addr = {0};
    TCCMaterializedAddr mat_dest_addr = {0};
    TCCMaterializedDest mat_dest = {0};

    if (need_src1_value)
    {
      tcc_ir_materialize_value_ir(ir, &src1_ir, &mat_src1);
    }
    else if (need_src1_addr)
    {
      tcc_ir_materialize_addr_ir(ir, &src1_ir, &mat_src1_addr, dest_ir.pr0_reg);
    }

    if (need_src2_value)
    {
      tcc_ir_materialize_value_ir(ir, &src2_ir, &mat_src2);
    }
    else if (need_src2_addr)
    {
      tcc_ir_materialize_addr_ir(ir, &src2_ir, &mat_src2_addr, dest_ir.pr0_reg);
    }

    if (need_dest_value)
    {
      tcc_ir_materialize_dest_ir(ir, &dest_ir, &mat_dest);
    }
    else if (need_dest_addr)
    {
      tcc_ir_materialize_addr_ir(ir, &dest_ir, &mat_dest_addr, PREG_NONE);
    }

    /* For operations that require register-only operands (MUL, DIV, MOD),
     * ensure constants/comparisons are loaded into registers. */
    TCCMaterializedValue mat_src1_reg = {0};
    TCCMaterializedValue mat_src2_reg = {0};
    if (need_src1_in_reg)
    {
      tcc_ir_materialize_const_to_reg_ir(ir, &src1_ir, &mat_src1_reg);
    }
    if (need_src2_in_reg)
    {
      tcc_ir_materialize_const_to_reg_ir(ir, &src2_ir, &mat_src2_reg);
    }

    /* Debug: trace all operations in parse_line ternary area */
    switch (cq->op)
    {
    case TCCIR_OP_MUL:
    case TCCIR_OP_MLA:
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    case TCCIR_OP_CMP:
    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_SHL:
    case TCCIR_OP_SHR:
    case TCCIR_OP_OR:
    case TCCIR_OP_AND:
    case TCCIR_OP_XOR:
    case TCCIR_OP_DIV:
    case TCCIR_OP_UDIV:
    case TCCIR_OP_IMOD:
    case TCCIR_OP_UMOD:
    case TCCIR_OP_SAR:
    case TCCIR_OP_UMULL:
    case TCCIR_OP_ADC_GEN:
    case TCCIR_OP_ADC_USE:
      tcc_gen_machine_data_processing_op(src1_ir, src2_ir, dest_ir, cq->op);
      break;
    case TCCIR_OP_FADD:
    case TCCIR_OP_FSUB:
    case TCCIR_OP_FMUL:
    case TCCIR_OP_FDIV:
    case TCCIR_OP_FNEG:
    case TCCIR_OP_FCMP:
    case TCCIR_OP_CVT_FTOF:
    case TCCIR_OP_CVT_ITOF:
    case TCCIR_OP_CVT_FTOI:
      tcc_gen_machine_fp_op(dest_ir, src1_ir, src2_ir, cq->op);
      break;
    case TCCIR_OP_LOAD:
    {
      /* Peephole: if next instruction is RETURNVALUE using this LOAD's result,
       * load directly to R0 instead of the allocated register */
      const IRQuadCompact *ir_next = (i + 1 < ir->next_instruction_index) ? &ir->compact_instructions[i + 1] : NULL;
      int ir_next_src1_vr = -1;
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE)
      {
        IROperand next_src1_irop = tcc_ir_op_get_src1(ir, ir_next);
        ir_next_src1_vr = irop_get_vreg(next_src1_irop);
      }
      const int dest_vreg = irop_get_vreg(dest_ir);
      int is_64bit_load = irop_is_64bit(dest_ir);
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE && ir_next_src1_vr == dest_vreg && !has_incoming_jump[i + 1])
      {
        dest_ir.pr0_reg = REG_IRET; /* R0 */
        dest_ir.pr0_spilled = 0;
        if (is_64bit_load)
        {
          dest_ir.pr1_reg = REG_IRE2; /* R1 */
          dest_ir.pr1_spilled = 0;
        }
        /* Also update the interval allocation so that RETURNVALUE's src1 gets the same registers */
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vreg);
        if (interval)
        {
          interval->allocation.r0 = REG_IRET;
          if (is_64bit_load)
            interval->allocation.r1 = REG_IRE2;
        }
      }
      tcc_gen_machine_load_op(dest_ir, src1_ir);
      break;
    }
    case TCCIR_OP_STORE:
      tcc_gen_machine_store_op(dest_ir, src1_ir, cq->op);
      break;
    case TCCIR_OP_LOAD_INDEXED:
    {
      /* LOAD_INDEXED: dest = *(base + (index << scale))
       * IR operands: dest, base, index, scale
       * Use src1_ir and src2_ir which already have register allocation applied
       */
      IROperand base_op = src1_ir;  /* base was src1 */
      IROperand index_op = src2_ir; /* index was src2 */
      IROperand scale_op = tcc_ir_op_get_scale(ir, cq);

      /* Peephole: if next instruction is RETURNVALUE using this LOAD_INDEXED's result,
       * load directly to R0 instead of the allocated register */
      const IRQuadCompact *ir_next = (i + 1 < ir->next_instruction_index) ? &ir->compact_instructions[i + 1] : NULL;
      int ir_next_src1_vr = -1;
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE)
      {
        IROperand next_src1_irop = tcc_ir_op_get_src1(ir, ir_next);
        ir_next_src1_vr = irop_get_vreg(next_src1_irop);
      }
      const int dest_vreg = irop_get_vreg(dest_ir);
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE && ir_next_src1_vr == dest_vreg && !has_incoming_jump[i + 1])
      {
        dest_ir.pr0_reg = REG_IRET; /* R0 */
        dest_ir.pr0_spilled = 0;
        /* Also update the interval allocation so that RETURNVALUE's src1 gets the same registers */
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vreg);
        if (interval)
        {
          interval->allocation.r0 = REG_IRET;
        }
      }

      tcc_gen_machine_load_indexed_op(dest_ir, base_op, index_op, scale_op);
      break;
    }
    case TCCIR_OP_STORE_INDEXED:
    {
      /* STORE_INDEXED: *(base + (index << scale)) = value
       * IR operands: base, value, index, scale
       * Use dest_ir, src1_ir, src2_ir which already have register allocation applied
       */
      IROperand base_op = dest_ir;  /* base is in "dest" position */
      IROperand value_op = src1_ir; /* value is in "src1" position */
      IROperand index_op = src2_ir; /* index is in "src2" position */
      IROperand scale_op = tcc_ir_op_get_scale(ir, cq);
      tcc_gen_machine_store_indexed_op(base_op, index_op, scale_op, value_op);
      break;
    }
    case TCCIR_OP_LOAD_POSTINC:
    {
      /* LOAD_POSTINC: dest = *ptr; ptr += offset
       * IR operands: dest, ptr, offset
       * Use dest_ir, src1_ir (ptr), and scale field for offset
       */
      IROperand ptr_op = src1_ir;                        /* pointer register */
      IROperand offset_op = tcc_ir_op_get_scale(ir, cq); /* offset is in scale position */
      tcc_gen_machine_load_postinc_op(dest_ir, ptr_op, offset_op);
      break;
    }
    case TCCIR_OP_STORE_POSTINC:
    {
      /* STORE_POSTINC: *ptr = src; ptr += offset
       * IR operands: ptr, src, offset
       * Use dest_ir (ptr), src1_ir (value), and scale field for offset
       */
      IROperand ptr_op = dest_ir;                        /* pointer register */
      IROperand value_op = src1_ir;                      /* value to store */
      IROperand offset_op = tcc_ir_op_get_scale(ir, cq); /* offset is in scale position */
      tcc_gen_machine_store_postinc_op(ptr_op, value_op, offset_op);
      break;
    }
    case TCCIR_OP_RETURNVALUE:
    {
      /* Peephole: if previous instruction was LOAD/ASSIGN that already loaded to R0,
       * skip the return value copy.
       * Check the interval allocation (updated by LOAD/ASSIGN peepholes) instead of
       * pool entries, since we work with local IROperand copies. */
      const IRQuadCompact *ir_prev = (i > 0) ? &ir->compact_instructions[i - 1] : NULL;
      int skip_copy = 0;
      if (!has_incoming_jump[i] && ir_prev && (ir_prev->op == TCCIR_OP_LOAD || ir_prev->op == TCCIR_OP_ASSIGN))
      {
        IROperand prev_dest_irop = tcc_ir_op_get_dest(ir, ir_prev);
        const int prev_dest_vreg = irop_get_vreg(prev_dest_irop);
        const int src1_vreg = irop_get_vreg(src1_ir);
        if (prev_dest_vreg == src1_vreg)
        {
          /* Check if the LOAD/ASSIGN peephole updated the interval to R0 */
          IRLiveInterval *prev_interval = tcc_ir_get_live_interval(ir, prev_dest_vreg);
          if (prev_interval && prev_interval->allocation.r0 == REG_IRET)
            skip_copy = 1;
        }
      }
      if (!skip_copy)
      {
        tcc_gen_machine_return_value_op(src1_ir, cq->op);
      }
    }
    case TCCIR_OP_RETURNVOID:
      /* Emit jump to epilogue (will be backpatched later) */
      /* if return is last instruction, then jump is not needed */
      if (i != ir->next_instruction_index - 1)
      {
        return_jump_addrs[num_return_jumps++] = ind;
        /* Return jumps target the epilogue (-1 indicates no IR target) */
        tcc_gen_machine_jump_op(cq->op, dest_ir, i);
      }
      break;
    case TCCIR_OP_ASSIGN:
    {
      /* Peephole: if next instruction is RETURNVALUE using this ASSIGN's dest,
       * assign directly to R0 to avoid an extra move */
      const IRQuadCompact *ir_next = (i + 1 < ir->next_instruction_index) ? &ir->compact_instructions[i + 1] : NULL;
      int ir_next_src1_vr = -1;
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE)
      {
        IROperand next_src1_irop = tcc_ir_op_get_src1(ir, ir_next);
        ir_next_src1_vr = irop_get_vreg(next_src1_irop);
      }
      const int assign_dest_vreg = irop_get_vreg(dest_ir);
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE && ir_next_src1_vr == assign_dest_vreg &&
          !has_incoming_jump[i + 1])
      {
        dest_ir.pr0_reg = REG_IRET; /* R0 */
        dest_ir.pr0_spilled = 0;
        if (irop_is_64bit(dest_ir))
        {
          dest_ir.pr1_reg = REG_IRE2; /* R1 */
          dest_ir.pr1_spilled = 0;
        }
        /* Update the interval allocation so RETURNVALUE sees the change */
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, assign_dest_vreg);
        if (interval)
        {
          interval->allocation.r0 = REG_IRET;
          if (irop_is_64bit(dest_ir))
            interval->allocation.r1 = REG_IRE2;
        }
      }
      tcc_gen_machine_assign_op(dest_ir, src1_ir, cq->op);
      break;
    }
    case TCCIR_OP_LEA:
      /* Load Effective Address: compute address of src1 into dest */
      tcc_gen_machine_lea_op(dest_ir, src1_ir, cq->op);
      break;
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCPARAMVOID:
    {
      tcc_gen_machine_func_parameter_op(src1_ir, src2_ir, cq->op);
      break;
    }
    case TCCIR_OP_JUMP:
      tcc_gen_machine_jump_op(cq->op, dest_ir, i);
      /* Update mapping to actual instruction address (may have shifted due to literal pool) */
      ir_to_code_mapping[i] = ind - (tcc_gen_machine_branch_opt_get_encoding(i) == 16 ? 2 : 4);
      /* Clear spill cache at branch - value may come from different path */
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      break;
    case TCCIR_OP_JUMPIF:
      tcc_gen_machine_conditional_jump_op(src1_ir, cq->op, dest_ir, i);
      /* Update mapping to actual instruction address (may have shifted due to literal pool) */
      ir_to_code_mapping[i] = ind - (tcc_gen_machine_branch_opt_get_encoding(i) == 16 ? 2 : 4);
      /* Clear spill cache at conditional branch - target may have different values */
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      break;
    case TCCIR_OP_IJUMP:
      tcc_gen_machine_indirect_jump_op(src1_ir);
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      break;
    case TCCIR_OP_SWITCH_TABLE:
    {
      int table_id = (int)irop_get_imm64_ex(ir, src2_ir);
      TCCIRSwitchTable *table = &ir->switch_tables[table_id];
      tcc_gen_machine_switch_table_op(src1_ir, table, ir, i);
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      break;
    }
    case TCCIR_OP_SETIF:
      tcc_gen_machine_setif_op(dest_ir, src1_ir, cq->op);
      break;
    case TCCIR_OP_BOOL_OR:
    case TCCIR_OP_BOOL_AND:
      tcc_gen_machine_bool_op(dest_ir, src1_ir, src2_ir, cq->op);
      break;

    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      tcc_gen_machine_vla_op(dest_ir, src1_ir, src2_ir, cq->op);
      break;
    case TCCIR_OP_FUNCCALLVOID:
      drop_return_value = 1;
      /* fall through */
    case TCCIR_OP_FUNCCALLVAL:
    {
      tcc_gen_machine_func_call_op(src1_ir, src2_ir, dest_ir, drop_return_value, ir, i);
      /* Clear spill cache after function call - callee may have modified memory */
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      break;
    }
    case TCCIR_OP_NOP:
      /* No operation - skip silently */
      break;
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
      /* Marker ops only: regalloc/liveness uses them, codegen emits nothing. */
      break;
    case TCCIR_OP_INLINE_ASM:
    {
#ifdef CONFIG_TCC_ASM
      tcc_ir_codegen_inline_asm_ir(ir, src1_ir);
      /* Inline asm may clobber registers/memory: treat as a full barrier. */
      tcc_ir_spill_cache_clear(&ir->spill_cache);
#else
      tcc_error("inline asm not supported");
#endif
      break;
    }
    default:
    {
      printf("Unsupported operation in tcc_generate_code: %s\n", tcc_ir_get_op_name(cq->op));
      if (ir->ir_to_code_mapping)
      {
        tcc_free(ir->ir_to_code_mapping);
        ir->ir_to_code_mapping = NULL;
        ir->ir_to_code_mapping_size = 0;
      }
      tcc_free(return_jump_addrs);
      exit(1);
    }
    };

    tcc_ir_release_materialized_addr_ir(&mat_dest_addr);
    tcc_ir_storeback_materialized_dest_ir(&dest_ir, &mat_dest);
    tcc_ir_release_materialized_addr_ir(&mat_src2_addr);
    tcc_ir_release_materialized_value_ir(&mat_src2_reg);
    tcc_ir_release_materialized_value_ir(&mat_src2);
    tcc_ir_release_materialized_value_ir(&mat_src1_reg);
    tcc_ir_release_materialized_addr_ir(&mat_src1_addr);
    tcc_ir_release_materialized_value_ir(&mat_src1);

    /* Clean up scratch register state at end of each IR instruction.
     * This restores any pushed scratch registers and resets the global exclude mask. */
    tcc_gen_machine_end_instruction();
  }

  ir_to_code_mapping[ir->next_instruction_index] = ind;
  orig_ir_to_code_mapping[ir->orig_ir_to_code_mapping_size - 1] = ind;

  /* Fill gaps for removed original indices: map them to the next reachable
   * emitted code address (or epilogue). This keeps &&label stable even if the
   * instruction at the exact original index was optimized away. */
  {
    uint32_t last = orig_ir_to_code_mapping[ir->orig_ir_to_code_mapping_size - 1];
    for (int k = ir->orig_ir_to_code_mapping_size - 2; k >= 0; --k)
    {
      if (orig_ir_to_code_mapping[k] == 0xFFFFFFFFu)
        orig_ir_to_code_mapping[k] = last;
      else
        last = orig_ir_to_code_mapping[k];
    }
  }

  if (!ir->naked)
    tcc_gen_machine_epilog(ir->leaffunc);
  tcc_ir_codegen_backpatch_jumps(ir, ir_to_code_mapping);

  /* Backpatch return jumps to point to epilogue */
  int epilogue_addr = ir_to_code_mapping[ir->next_instruction_index];
  for (int i = 0; i < num_return_jumps; i++)
  {
    tcc_gen_machine_backpatch_jump(return_jump_addrs[i], epilogue_addr);
  }

  tcc_free(return_jump_addrs);
  tcc_free(has_incoming_jump);
}

/* ============================================================================
 * Legacy API Wrappers
 * ============================================================================ */

/* Legacy wrapper for tcc_ir_fill_registers */
void tcc_ir_fill_registers_ir_legacy(TCCIRState *ir, IROperand *op)
{
  tcc_ir_fill_registers_ir(ir, op);
}

/* Note: tcc_ir_generate_code legacy wrapper remains in tccir.c */
