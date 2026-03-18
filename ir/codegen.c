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

/* Debug tracking variable (defined in arm-thumb-gen.c) */
extern int g_debug_current_op;

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
    int is_64bit = interval && (interval->is_double || interval->is_llong || interval->is_complex);

    /* If the ABI incoming registers were already set (e.g., by the
     * parameter handling in tcc_ir_add_function_parameters), respect them
     * and advance argno past the actual registers used. */
    if (interval && (interval->incoming_reg0 >= 0 || interval->incoming_reg1 >= 0))
    {
      /* Advance argno to the register AFTER the highest one used by this
       * parameter.  This correctly accounts for alignment-induced register
       * gaps (e.g. AAPCS 8-byte alignment skipping from r1 to r2). */
      int highest = interval->incoming_reg0;
      if (interval->incoming_reg1 > highest)
        highest = interval->incoming_reg1;
      int next = highest + 1;
      if (next > argno)
        argno = next;
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
    if (interval->is_llong || interval->is_double || interval->is_complex)
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
        /* Unconditional jump for a compile-time constant condition:
         * code after this point is unreachable.  Must mirror gjmp_acs()
         * which calls CODE_OFF() so that data/code suppression works
         * correctly for dead branches (e.g. if(0) { ... }).
         * CODE_OFF_BIT = 0x20000000 (defined in tccgen.c). */
        if (!nocode_wanted)
          nocode_wanted |= 0x20000000;
      }
    }
    else
    {
      /* If we're testing a memory lvalue (e.g. tabl[i]), load the value first.
       * Otherwise we end up testing the address, which is almost always non-zero
       * and can lead to invalid indirect calls.
       */
      /* Bit-fields must be extracted (shift/mask) before testing;
       * TEST_ZERO on the raw word would test all 32 bits, not just
       * the bit-field slice (e.g. a 1-bit field at position 0). */
      if (vtop->type.t & VT_BITFIELD)
        gv(RC_INT);
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
  if (!ir)
  {
    return;
  }

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

  /* Compute reserved_regs: physical registers of vregs that are live at this
   * INLINE_ASM instruction but are NOT asm operands.  The constraint solver
   * must avoid these registers when picking registers for "r" constraints,
   * otherwise the operand load will clobber the live value.
   *
   * Unlike clobber_regs, reserved_regs only affect constraint allocation —
   * they do NOT trigger save/restore in asm_gen_code prolog/epilog. */
  uint8_t reserved_regs[NB_ASM_REGS];
  memset(reserved_regs, 0, sizeof(reserved_regs));
  {
    int asm_instr_idx = ir->codegen_instruction_idx;
    struct
    {
      IRLiveInterval *intervals;
      int count;
    } groups[3] = {
        {ir->variables_live_intervals, ir->variables_live_intervals_size},
        {ir->temporary_variables_live_intervals, ir->temporary_variables_live_intervals_size},
        {ir->parameters_live_intervals, ir->parameters_live_intervals_size},
    };

    for (int g = 0; g < 3; g++)
    {
      for (int j = 0; j < groups[g].count; j++)
      {
        IRLiveInterval *interval = &groups[g].intervals[j];
        if (interval->start == INTERVAL_NOT_STARTED)
          continue;
        if ((int)interval->start > asm_instr_idx || (int)interval->end < asm_instr_idx)
          continue;

        int r0 = interval->allocation.r0;
        if (r0 & PREG_SPILLED)
          continue;
        int phys_reg = r0 & PREG_REG_NONE;
        if (phys_reg == PREG_REG_NONE)
          continue;
        if (phys_reg < NB_ASM_REGS)
          reserved_regs[phys_reg] = 1;

        int r1 = interval->allocation.r1;
        if (!(r1 & PREG_SPILLED))
        {
          int phys_reg1 = r1 & PREG_REG_NONE;
          if (phys_reg1 != PREG_REG_NONE && phys_reg1 < NB_ASM_REGS)
            reserved_regs[phys_reg1] = 1;
        }
      }
    }

    /* Asm operands themselves are allowed to reuse their currently assigned
     * physical registers.  Only non-operand live values need to remain
     * reserved from the constraint solver.  Without this, an inline asm that
     * already has several live register operands can spuriously run out of
     * allocatable "r" registers in IR mode. */
    for (int i = 0; i < nb_operands; ++i)
    {
      if (!vals[i].pr0_spilled && vals[i].pr0_reg != PREG_REG_NONE && vals[i].pr0_reg < NB_ASM_REGS)
        reserved_regs[vals[i].pr0_reg] = 0;
      if (!vals[i].pr1_spilled && vals[i].pr1_reg != PREG_REG_NONE && vals[i].pr1_reg < NB_ASM_REGS)
        reserved_regs[vals[i].pr1_reg] = 0;
    }
  }

  tcc_asm_emit_inline(ops, nb_operands, ia->nb_outputs, nb_labels, clobber_regs, reserved_regs, ia->asm_str,
                      ia->asm_len, ia->must_subst);
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

  /* Backpatch switch table entries.
   * Table entries are 32-bit signed PC-relative offsets with Thumb bit.
   * The reference point is table_start, which is the PC value when
   * the 16-bit ADD Rt, PC instruction at ind+10 reads PC (= ind+10+4 = ind+14 = table_start).
   * Formula: table[i] = (target_addr | 1) - table_start
   * This must happen after all code is generated so forward targets are mapped. */
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    int table_start = table->table_code_addr;
    if (table_start <= 0)
      continue;                  /* Table not emitted (e.g. dead code) */
    int ref_point = table_start; /* PC value at the 16-bit ADD Rt, PC instruction (at ind+10, PC=ind+14=table_start) */
    for (int j = 0; j < table->num_entries; j++)
    {
      int target_ir = table->targets[j];
      int entry_addr = table_start + j * 4; /* 4 bytes per entry */
      int target_addr;
      if (target_ir >= 0 && target_ir < (int)ir->ir_to_code_mapping_size)
        target_addr = ir_to_code_mapping[target_ir];
      else
        target_addr = ir_to_code_mapping[ir->next_instruction_index]; /* epilogue */
      int32_t offset = (int32_t)((target_addr | 1) - ref_point);
      write32le(cur_text_section->data + entry_addr, (uint32_t)offset);
    }
  }
}

/* ============================================================================
 * Phase-3 scratch conflict fixup
 * ============================================================================
 *
 * After the dry run has identified which instructions would push a register
 * to the stack (no free scratch register available), this function tries to
 * move the vreg currently occupying that register to a free callee-saved
 * register.  This eliminates the push/pop overhead for those instructions.
 *
 * Parameters:
 *   ir     - current function IR state
 *   r      - physical register that would be pushed at instruction insn_i
 *   insn_i - the instruction index where the push was noted
 *
 * Returns the new physical register on success, -1 if no reassignment could
 * be made (e.g. all callee-saved registers are already occupied over the
 * vreg's live range, or the interval is complex / 64-bit / float).
 */
static int try_reassign_scratch_conflict(TCCIRState *ir, int r, int insn_i)
{
  LSLiveIntervalState *ls = &ir->ls;

  /* Callee-saved registers R4-R11 (bits 4..11 = 0x0FF0), minus reserved
   * special-purpose registers:
   *   R7  = R_FP (= 7): always reserved as frame pointer by the ARM backend.
   *     arm-thumb-gen.c: "Always reserve R7 (FP) and never allocate it as a
   *     general register."  The linear-scan allocator never assigns vregs to R7,
   *     so it never appears in live_regs_by_instruction.  We must exclude it
   *     here as well, otherwise we would clobber the frame pointer.
   *   R10 = static_chain_reg (= 10): reserved when function uses a static chain.
   */
  const uint32_t ALL_CALLEE_SAVED = 0x0FF0u;
  const uint32_t ARM_FP_REG = 7u;         /* R_FP = R7, defined in arm-thumb-opcodes.h */
  uint32_t reserved = (1u << ARM_FP_REG); /* always exclude frame pointer */
  if (ir->has_static_chain)
    reserved |= (1u << (uint32_t)architecture_config.static_chain_reg);
  const uint32_t CALLEE_SAVED = ALL_CALLEE_SAVED & ~reserved;

  /* Find the LSLiveInterval holding r at instruction insn_i. */
  LSLiveInterval *ls_iv = NULL;
  for (int k = 0; k < ls->next_interval_index; k++)
  {
    LSLiveInterval *iv = &ls->intervals[k];
    /* Only handle plain integer register allocations. */
    if (iv->reg_type != LS_REG_TYPE_INT)
      continue;
    if (iv->addrtaken || iv->stack_location != 0)
      continue;
    /* Skip 64-bit pairs — they need two adjacent registers. */
    if (iv->r1 >= 0 && iv->r1 < 16)
      continue;
    if (iv->r0 != r)
      continue;
    if ((int)iv->start > insn_i || (int)iv->end < insn_i)
      continue;
    ls_iv = iv;
    break;
  }
  if (!ls_iv)
    return -1;

  /* Get the IRLiveInterval for the same vreg to check for float/double/llong. */
  IRLiveInterval *ir_iv = tcc_ir_get_live_interval(ir, (int)ls_iv->vreg);
  if (!ir_iv)
    return -1;
  /* Skip floating-point and 64-bit intervals. */
  if (ir_iv->is_float || ir_iv->is_double || ir_iv->is_llong || ir_iv->is_complex || ir_iv->use_vfp)
    return -1;
  /* Skip ABI-pinned intervals: function parameters and call return values have
   * incoming_reg0 >= 0, meaning the hardware places the value in a specific
   * register dictated by the calling convention.  Changing the allocation would
   * cause the codegen to look in the wrong register after a call/entry. */
  if (ir_iv->incoming_reg0 >= 0)
    return -1;

  /* Compute the union of live register masks across [ls_iv->start .. ls_iv->end].
   * Any register set in this union is occupied by some other live vreg and
   * cannot be used as the reassignment target. */
  uint32_t blocked = 0;
  if (ls->live_regs_by_instruction)
  {
    for (int j = (int)ls_iv->start; j <= (int)ls_iv->end && j < ls->live_regs_by_instruction_size; j++)
      blocked |= ls->live_regs_by_instruction[j];
  }
  blocked |= (1u << r); /* keep r itself blocked so we don't choose it */

  uint32_t avail = CALLEE_SAVED & ~blocked;
  if (!avail)
    return -1;

  int new_r = (int)__builtin_ctz(avail); /* lowest-numbered free callee-saved */

  /* --- Apply the reassignment --- */

  /* 1. Update the IRLiveInterval (read by machine_op_from_ir). */
  ir_iv->allocation.r0 = (uint16_t)new_r;

  /* 2. Update the LSLiveInterval (read by tcc_ls_build_live_regs_by_instruction
   *    and tcc_ls_find_free_scratch_reg). */
  ls_iv->r0 = (int16_t)new_r;

  /* 3. Patch live_regs_by_instruction for the interval's full range. */
  if (ls->live_regs_by_instruction)
  {
    for (int j = (int)ls_iv->start; j <= (int)ls_iv->end && j < ls->live_regs_by_instruction_size; j++)
    {
      ls->live_regs_by_instruction[j] &= ~(1u << r);
      ls->live_regs_by_instruction[j] |= (1u << new_r);
    }
  }

  /* 4. Mark new_r as dirty so the prologue will save/restore it. */
  ls->dirty_registers |= (1ull << new_r);

  return new_r;
}

/* ============================================================================
 * Helper: sub-component fixup for register-pair operands used as LOAD/STORE
 * sources.  When a local STACKOFF operand accesses a sub-component of a 64-bit
 * pair (e.g., __imag__ on _Complex float), the original operand's byte offset
 * differs from the interval's base offset.  In that case, rewrite the
 * MachineOperand to use r1 (second register of the pair) instead of r0.
 *
 * This MUST NOT be applied to DP/ASSIGN operands — a 64-bit pair allocated as
 * a register pair can also have a non-zero delta, but that is not a
 * sub-component access.
 * ============================================================================ */
static void mop_fixup_subcomponent(MachineOperand *mop, const IROperand *op, TCCIRState *ir)
{
  if (mop->kind != MACH_OP_REG || mop->needs_deref || mop->u.reg.r1 < 0)
    return;
  int vreg = irop_get_vreg(*op);
  if (vreg <= 0 || irop_get_tag(*op) != IROP_TAG_STACKOFF || op->btype == IROP_BTYPE_STRUCT)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (!interval)
    return;
  int32_t delta = op->u.imm32 - interval->original_offset;
  if (delta != 0)
  {
    mop->u.reg.r0 = mop->u.reg.r1;
    mop->u.reg.r1 = -1;
    mop->needs_deref = false;
  }
}

/* ============================================================================
 * Before-Return Peephole
 *
 * When a LOAD, LOAD_INDEXED, or ASSIGN is immediately followed by a
 * RETURNVALUE on the same vreg (with no intervening jump target), patch the
 * dest vreg's allocation to R0 (R0+R1 for 64-bit) and construct a synthetic
 * MACH_OP_REG MachineOperand.  This eliminates the extra move that
 * RETURNVALUE would otherwise emit.
 *
 * Called from both dry-run and real-run dispatch loops so that scratch
 * accounting stays consistent.
 * ============================================================================ */
static bool ir_codegen_before_ret_peephole(TCCIRState *ir, int i, const IROperand *dest_ir,
                                           const uint8_t *has_incoming_jump, MachineOperand *out_mop_dest)
{
  if (i + 1 >= ir->next_instruction_index)
    return false;

  const IRQuadCompact *nq = &ir->compact_instructions[i + 1];
  if (nq->op != TCCIR_OP_RETURNVALUE || has_incoming_jump[i + 1])
    return false;

  IROperand nq_src1 = tcc_ir_op_get_src1(ir, nq);
  int next_vr = irop_get_vreg(nq_src1);
  int dest_vr = irop_get_vreg(*dest_ir);
  if (next_vr != dest_vr || dest_vr < 0)
    return false;

  IRLiveInterval *li = tcc_ir_get_live_interval(ir, dest_vr);
  const int needs_pair = irop_needs_pair(*dest_ir);
  if (li)
  {
    li->allocation.r0 = REG_IRET;
    li->allocation.offset = 0;
    if (needs_pair)
      li->allocation.r1 = REG_IRE2;
  }

  *out_mop_dest = (MachineOperand){.kind = MACH_OP_REG,
                                   .btype = irop_get_btype(*dest_ir),
                                   .vreg = dest_vr,
                                   .is_64bit = needs_pair,
                                   .is_unsigned = dest_ir->is_unsigned,
                                   .needs_deref = false,
                                   .u.reg = {.r0 = REG_IRET, .r1 = needs_pair ? (int)REG_IRE2 : -1}};
  return true;
}

/* ============================================================================
 * Scratch Recording / Checking
 *
 * During dry-run: record how many scratch registers each instruction used.
 * During real-run: verify the count matches (under TCC_LS_DEBUG).
 *
 * Consolidates 16 dry-run recording sites and 16 real-run checking sites
 * into a single inline helper.
 * ============================================================================ */
static inline void ir_codegen_record_scratch(int i, int *dry_insn_scratch, uint16_t *dry_insn_saves)
{
  dry_insn_scratch[i] = tcc_gen_machine_insn_scratch_count();
  dry_insn_saves[i] = tcc_gen_machine_insn_scratch_saves_mask();
}

static inline void ir_codegen_check_scratch(int i, TccIrOp op, const int *dry_insn_scratch,
                                            const uint16_t *dry_insn_saves)
{
#ifdef TCC_LS_DEBUG
  int real_scratch = tcc_gen_machine_insn_scratch_count();
  if (real_scratch != dry_insn_scratch[i] && dry_insn_saves[i] == 0)
    fprintf(stderr, "[insn-scratch] i=%d op=%d dry=%d real=%d MISMATCH\n", i, (int)op, dry_insn_scratch[i],
            real_scratch);
#else
  (void)i;
  (void)op;
  (void)dry_insn_scratch;
  (void)dry_insn_saves;
#endif
}

/* Unified scratch tracking: records during dry-run, checks during real-run. */
static inline void ir_codegen_track_scratch(int is_dry_run, int i, TccIrOp op, int *dry_insn_scratch,
                                            uint16_t *dry_insn_saves)
{
  if (is_dry_run)
    ir_codegen_record_scratch(i, dry_insn_scratch, dry_insn_saves);
  else
    ir_codegen_check_scratch(i, op, dry_insn_scratch, dry_insn_saves);
}

/* ============================================================================
 * Main Code Generation Loop
 * ============================================================================ */

void tcc_ir_codegen_generate(TCCIRState *ir)
{
  IRQuadCompact *cq;
  int drop_return_value = 0;

#ifdef TCC_REGALLOC_DEBUG
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
#endif

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

  /* If this function has a static chain (nested function), reserve R10
   * as callee-saved so the parent's static chain is preserved.
   * R10 is the static chain register per architecture_config.static_chain_reg. */
  if (ir->has_static_chain)
  {
    extra_prologue_regs |= (1 << architecture_config.static_chain_reg);
  }

  /* Phase-3 per-instruction scratch constraint recording.
   * Allocated once per function; indexed by instruction index.
   * dry_insn_scratch[i] = number of mach_alloc_scratch() calls at instruction i.
   * dry_insn_saves[i]   = bitmask of registers that would be PUSH'd at instruction i.
   * Both arrays are declared before #if so they are visible in both passes. */
  int *dry_insn_scratch = tcc_mallocz(ir->next_instruction_index * sizeof(int));
  uint16_t *dry_insn_saves = tcc_mallocz(ir->next_instruction_index * sizeof(uint16_t));

  /* ============================================================================
   * TWO-PASS CODE GENERATION
   * ============================================================================
   * Pass 0 (dry-run): Discover scratch register needs without emitting code.
   *   - ot() is a no-op; ind advances but no bytes are written.
   *   - Records per-instruction scratch counts in dry_insn_scratch[].
   *   - Branch optimizer collects offset data.
   * Pass 1 (real-run): Emit actual Thumb-2 machine code.
   *   - Uses dry-run data for scratch consistency checks.
   *   - Emits debug info, epilogue jumps, inline asm.
   * ============================================================================ */
  for (int pass = 0; pass < 2; pass++)
  {
    const int is_dry_run = (pass == 0);

    /* ---- Pass-specific initialisation ---- */
    if (is_dry_run)
    {
      tcc_gen_machine_dry_run_init();
      tcc_gen_machine_branch_opt_init();
      tcc_gen_machine_dry_run_start();
      tcc_gen_machine_reset_scratch_state();
      tcc_ir_spill_cache_clear(&ir->spill_cache);
    }

    /* Save state before dry-run so we can restore for real-run. */
    int saved_ind = ind;
    int saved_codegen_idx = ir->codegen_instruction_idx;
    int saved_loc = loc;
    int saved_call_outgoing_base = ir->call_outgoing_base;

    /* ---- Instruction loop ---- */
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      drop_return_value = 0;
      cq = &ir->compact_instructions[i];

      /* Default: no extra scratch constraints for this instruction. */
      ir->codegen_materialize_scratch_flags = 0;

      /* Track current instruction for scratch register allocation */
      ir->codegen_instruction_idx = i;

      /* Debug tracking: update current op for ot_check failure reporting */
      g_debug_current_op = (int)cq->op;

      ir_to_code_mapping[i] = ind;

      /* Real-run only: record original-index mapping and emit debug line info */
      if (!is_dry_run)
      {
        if (cq->orig_index >= 0 && cq->orig_index < ir->orig_ir_to_code_mapping_size)
          orig_ir_to_code_mapping[cq->orig_index] = ind;
        tcc_debug_line_num(tcc_state, cq->line_num);
      }

      /* Get operand copies from iroperand_pool (compact representation) */
      IROperand src1_ir = tcc_ir_op_get_src1(ir, cq);
      IROperand src2_ir = tcc_ir_op_get_src2(ir, cq);
      IROperand dest_ir = tcc_ir_op_get_dest(ir, cq);

      /* Operands are NOT filled here. machine_op_from_ir reads the interval
       * table directly from the raw operand.  All dispatch sites now use
       * MachineOperand-based (_mop) handlers unconditionally. */

      switch (cq->op)
      {
      case TCCIR_OP_MUL:
      case TCCIR_OP_DIV:
      case TCCIR_OP_UDIV:
      case TCCIR_OP_IMOD:
      case TCCIR_OP_UMOD:
      case TCCIR_OP_TEST_ZERO:
      {
        MachineOperand mop_src1 = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_src2 = machine_op_from_ir(ir, &src2_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_muldiv_mop(mop_src1, mop_src2, mop_dest, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_MLA:
      {
        IROperand accum_ir = ir->iroperand_pool[cq->operand_base + 3];
        MachineOperand mop_src1 = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_src2 = machine_op_from_ir(ir, &src2_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        MachineOperand mop_accum = machine_op_from_ir(ir, &accum_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_mla_mop(mop_src1, mop_src2, mop_dest, mop_accum);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_UMULL:
      {
        MachineOperand mop_src1 = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_src2 = machine_op_from_ir(ir, &src2_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_umull_mop(mop_src1, mop_src2, mop_dest);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_ADD:
      case TCCIR_OP_SUB:
      case TCCIR_OP_CMP:
      case TCCIR_OP_SHL:
      case TCCIR_OP_SHR:
      case TCCIR_OP_SAR:
      case TCCIR_OP_OR:
      case TCCIR_OP_AND:
      case TCCIR_OP_XOR:
      case TCCIR_OP_ADC_GEN:
      case TCCIR_OP_ADC_USE:
      {
        MachineOperand mop_src1 = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_src2 = machine_op_from_ir(ir, &src2_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_data_processing_mop(mop_src1, mop_src2, mop_dest, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_FADD:
      case TCCIR_OP_FSUB:
      case TCCIR_OP_FMUL:
      case TCCIR_OP_FDIV:
      case TCCIR_OP_FNEG:
      case TCCIR_OP_FCMP:
      case TCCIR_OP_CVT_FTOF:
      case TCCIR_OP_CVT_ITOF:
      case TCCIR_OP_CVT_FTOI:
      {
        MachineOperand mop_src1 = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_src2 = machine_op_from_ir(ir, &src2_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_fp_mop(mop_src1, mop_src2, mop_dest, cq->op, src1_ir.is_complex || dest_ir.is_complex);
        break;
      }
      case TCCIR_OP_LOAD:
      {
        MachineOperand mop_dest;
        if (!ir_codegen_before_ret_peephole(ir, i, &dest_ir, has_incoming_jump, &mop_dest))
          mop_dest = machine_op_from_ir(ir, &dest_ir);
        MachineOperand mop_src = machine_op_from_ir(ir, &src1_ir);
        mop_fixup_subcomponent(&mop_src, &src1_ir, ir);
        if (mop_dest.kind == MACH_OP_NONE || mop_src.kind == MACH_OP_NONE)
          tcc_error("compiler_error: LOAD operand produced MACH_OP_NONE (i=%d dest_kind=%d src_kind=%d)", i,
                    mop_dest.kind, mop_src.kind);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_load_mop(mop_src, mop_dest, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_STORE:
      {
        MachineOperand mop_dest_s = machine_op_from_ir(ir, &dest_ir);
        MachineOperand mop_src_s = machine_op_from_ir(ir, &src1_ir);
        mop_fixup_subcomponent(&mop_src_s, &src1_ir, ir);
        if (mop_dest_s.kind == MACH_OP_NONE || mop_src_s.kind == MACH_OP_NONE)
          tcc_error("compiler_error: STORE operand produced MACH_OP_NONE (i=%d dest_kind=%d src_kind=%d)", i,
                    mop_dest_s.kind, mop_src_s.kind);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_store_mop(mop_dest_s, mop_src_s, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_LOAD_INDEXED:
      {
        MachineOperand mop_dest;
        if (!ir_codegen_before_ret_peephole(ir, i, &dest_ir, has_incoming_jump, &mop_dest))
          mop_dest = machine_op_from_ir(ir, &dest_ir);
        IROperand scale_raw = tcc_ir_op_get_scale(ir, cq);
        MachineOperand mop_base = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_index = machine_op_from_ir(ir, &src2_ir);
        MachineOperand mop_scale = machine_op_from_ir(ir, &scale_raw);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_load_indexed_mop(mop_dest, mop_base, mop_index, mop_scale, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_STORE_INDEXED:
      {
        IROperand scale_raw = tcc_ir_op_get_scale(ir, cq);
        MachineOperand mop_base = machine_op_from_ir(ir, &dest_ir);
        MachineOperand mop_index = machine_op_from_ir(ir, &src2_ir);
        MachineOperand mop_scale = machine_op_from_ir(ir, &scale_raw);
        MachineOperand mop_value = machine_op_from_ir(ir, &src1_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_store_indexed_mop(mop_base, mop_index, mop_scale, mop_value, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_LOAD_POSTINC:
      {
        IROperand offset_raw = tcc_ir_op_get_scale(ir, cq);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        MachineOperand mop_ptr = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_offset = machine_op_from_ir(ir, &offset_raw);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_load_postinc_mop(mop_dest, mop_ptr, mop_offset, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_STORE_POSTINC:
      {
        IROperand offset_raw = tcc_ir_op_get_scale(ir, cq);
        MachineOperand mop_ptr = machine_op_from_ir(ir, &dest_ir);
        MachineOperand mop_value = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_offset = machine_op_from_ir(ir, &offset_raw);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_store_postinc_mop(mop_ptr, mop_value, mop_offset, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_RETURNVALUE:
      {
        MachineOperand mop_src = machine_op_from_ir(ir, &src1_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_return_value_mop(mop_src, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
      }
      /* fall through to RETURNVOID */
      case TCCIR_OP_RETURNVOID:
        /* Real-run: emit jump to epilogue (backpatched later).
         * Dry-run: no-op (we don't track return_jump_addrs). */
        if (!is_dry_run && i != ir->next_instruction_index - 1)
        {
          return_jump_addrs[num_return_jumps++] = ind;
          tcc_gen_machine_jump_mop(cq->op, irop_get_imm32(dest_ir), i);
        }
        break;
      case TCCIR_OP_ASSIGN:
      {
        MachineOperand mop_dest;
        if (!ir_codegen_before_ret_peephole(ir, i, &dest_ir, has_incoming_jump, &mop_dest))
          mop_dest = machine_op_from_ir(ir, &dest_ir);
        MachineOperand mop_src = machine_op_from_ir(ir, &src1_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_assign_mop(mop_src, mop_dest, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_LEA:
      {
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        MachineOperand mop_src = machine_op_from_ir(ir, &src1_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_lea_mop(mop_dest, mop_src);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_FUNCPARAMVAL:
      case TCCIR_OP_FUNCPARAMVOID:
      {
        MachineOperand mop_src1 = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_src2 = machine_op_from_ir(ir, &src2_ir);
        tcc_gen_machine_func_parameter_mop(mop_src1, mop_src2, cq->op);
        break;
      }
      case TCCIR_OP_JUMP:
        tcc_gen_machine_jump_mop(cq->op, irop_get_imm32(dest_ir), i);
        if (!is_dry_run)
          ir_to_code_mapping[i] = ind - (tcc_gen_machine_branch_opt_get_encoding(i) == 16 ? 2 : 4);
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        break;
      case TCCIR_OP_JUMPIF:
        tcc_gen_machine_conditional_jump_mop(src1_ir.u.imm32, cq->op, irop_get_imm32(dest_ir), i);
        if (!is_dry_run)
          ir_to_code_mapping[i] = ind - (tcc_gen_machine_branch_opt_get_encoding(i) == 16 ? 2 : 4);
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        break;
      case TCCIR_OP_IJUMP:
      {
        MachineOperand mop_src = machine_op_from_ir(ir, &src1_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_indirect_jump_mop(mop_src, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        break;
      }
      case TCCIR_OP_SWITCH_TABLE:
      {
        int table_id = (int)irop_get_imm64_ex(ir, src2_ir);
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        if (is_dry_run)
        {
          /* Compute exact table size so branch offsets are accurate.
           * Layout: ADD.W(4) + LDR.W(4) + ADD.W(4) + BX(2) = 14 bytes preamble
           * + 4 bytes per table entry (32-bit signed PC-relative offsets). */
          int table_data_size = table->num_entries * 4;
          ind += 14;
          ind += table_data_size;
        }
        else
        {
          MachineOperand mop_idx = machine_op_from_ir(ir, &src1_ir);
          tcc_gen_machine_insn_scratch_reset();
          tcc_gen_machine_switch_table_mop(mop_idx, table, ir, i);
        }
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        break;
      }
      case TCCIR_OP_SETIF:
      {
        MachineOperand mop_src = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_setif_mop(mop_src, mop_dest, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_BOOL_OR:
      case TCCIR_OP_BOOL_AND:
      {
        MachineOperand mop_src1 = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_src2 = machine_op_from_ir(ir, &src2_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_bool_mop(mop_src1, mop_src2, mop_dest, cq->op);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_VLA_ALLOC:
      case TCCIR_OP_VLA_SP_SAVE:
      case TCCIR_OP_VLA_SP_RESTORE:
      {
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        MachineOperand mop_src1 = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_src2 = machine_op_from_ir(ir, &src2_ir);
        tcc_gen_machine_vla_mop(mop_dest, mop_src1, mop_src2, cq->op);
        break;
      }
      case TCCIR_OP_FUNCCALLVOID:
        drop_return_value = 1;
        /* fall through */
      case TCCIR_OP_FUNCCALLVAL:
      {
        MachineOperand func_mop = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_func_call_mop(func_mop, src2_ir, mop_dest, drop_return_value, ir, i);
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        if (ir->has_static_chain)
          tcc_gen_machine_restore_chain();
        break;
      }
      case TCCIR_OP_NOP:
        break;
      case TCCIR_OP_PREFETCH:
      {
        MachineOperand mop_addr = machine_op_from_ir(ir, &src1_ir);
        /* src2 holds the rw hint: 0 = read (PLD), 1 = write (PLDW) */
        int rw = (int)irop_get_imm64_ex(ir, src2_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_prefetch_mop(mop_addr, rw);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_TRAP:
        tcc_gen_machine_trap_mop();
        break;
      case TCCIR_OP_SETJMP:
      {
        MachineOperand mop_buf = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_setjmp_mop(mop_buf, mop_dest);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_LONGJMP:
      {
        MachineOperand mop_buf = machine_op_from_ir(ir, &src1_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_longjmp_mop(mop_buf);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_NL_SETJMP:
      {
        MachineOperand mop_buf = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_nl_setjmp_mop(mop_buf, mop_dest);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_NL_LONGJMP:
      {
        MachineOperand mop_buf = machine_op_from_ir(ir, &src1_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_nl_longjmp_mop(mop_buf);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_BUILTIN_APPLY_ARGS:
      {
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_builtin_apply_args_mop(mop_dest);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        break;
      }
      case TCCIR_OP_BUILTIN_APPLY:
      {
        MachineOperand mop_fn = machine_op_from_ir(ir, &src1_ir);
        MachineOperand mop_args = machine_op_from_ir(ir, &src2_ir);
        MachineOperand mop_dest = machine_op_from_ir(ir, &dest_ir);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_builtin_apply_mop(mop_fn, mop_args, mop_dest);
        ir_codegen_track_scratch(is_dry_run, i, cq->op, dry_insn_scratch, dry_insn_saves);
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        break;
      }
      case TCCIR_OP_BUILTIN_RETURN:
        /* Handled as RETURNVALUE by the parser; should not reach here */
        break;
      case TCCIR_OP_SET_CHAIN:
        tcc_gen_machine_set_chain();
        break;
      case TCCIR_OP_INIT_CHAIN_SLOT:
        tcc_gen_machine_init_chain_slot(src1_ir);
        break;
      case TCCIR_OP_ASM_INPUT:
      case TCCIR_OP_ASM_OUTPUT:
        break;
      case TCCIR_OP_INLINE_ASM:
        if (!is_dry_run)
        {
#ifdef CONFIG_TCC_ASM
          tcc_ir_codegen_inline_asm_ir(ir, src1_ir);
          tcc_ir_spill_cache_clear(&ir->spill_cache);
#else
          tcc_error("inline asm not supported");
#endif
        }
        break;
      default:
        if (!is_dry_run)
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
        break;
      };

      /* Clean up scratch register state at end of each IR instruction.
       * This restores any pushed scratch registers and resets the global exclude mask. */
      tcc_gen_machine_end_instruction();
    }

    /* ---- Pass-specific finalisation ---- */
    if (is_dry_run)
    {
      /* End dry-run and analyze results */
      tcc_gen_machine_dry_run_end();

      /* Analyze branch offsets and select optimal encodings */
      tcc_gen_machine_branch_opt_analyze(ir_to_code_mapping, ir->next_instruction_index);

      /* Check if LR was pushed during dry run in a leaf function */
      if (original_leaffunc && tcc_gen_machine_dry_run_get_lr_push_count() > 0)
      {
        extra_prologue_regs |= (1 << 14); /* R_LR */
      }

      /* Restore state for real code generation */
      ind = saved_ind;
      loc = saved_loc;
      ir->call_outgoing_base = saved_call_outgoing_base;
      ir->codegen_instruction_idx = saved_codegen_idx;

      /* Phase-3 scratch conflict fixup.
       * For each instruction where the dry run needed to PUSH a register,
       * try to move the blocking vreg to a free callee-saved register. */
      {
        int any_fixup = 0;
        for (int i = 0; i < ir->next_instruction_index; i++)
        {
          uint16_t saves = dry_insn_saves[i];
          if (!saves)
            continue;
          while (saves)
          {
            int r = (int)__builtin_ctz(saves);
            saves = (uint16_t)(saves & (saves - 1u));
            int new_r = try_reassign_scratch_conflict(ir, r, i);
            if (new_r >= 0)
            {
              dry_insn_scratch[i] = 0;
              any_fixup = 1;
            }
          }
        }
        if (any_fixup)
          tcc_ls_reset_scratch_cache(&ir->ls);
      }

      /* Reset scratch state for real pass */
      tcc_gen_machine_reset_scratch_state();
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      tcc_ir_opt_fp_cache_clear(ir);

      /* Emit prologue before real pass */
      (void)original_leaffunc;
      if (!ir->naked)
        tcc_gen_machine_prolog(ir->leaffunc, ir->ls.dirty_registers, stack_size, extra_prologue_regs);
      if (!ir->naked)
        tcc_debug_prolog_epilog(tcc_state, 0);
    }
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
  tcc_free(dry_insn_saves);
  tcc_free(dry_insn_scratch);
  tcc_free(has_incoming_jump);
}

/* ============================================================================
 * Legacy API Wrappers
 * ============================================================================ */

/* Note: tcc_ir_generate_code legacy wrapper remains in tccir.c */
