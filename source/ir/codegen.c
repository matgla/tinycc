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
#include "arm-thumb-callsite.h"
#include "opt/flat/if_convert.h"

/* Debug tracking variable (defined in arm-thumb-gen.c) */
extern int g_debug_current_op;

/* ============================================================================
 * Register Fill (Apply Allocation to Operands)
 * ============================================================================ */

void tcc_ir_fill_registers(TCCIRState *ir, SValue *sv)
{
  int old_r = sv->r;
  int old_v = old_r & VT_VALMASK;

  /* Concrete stack slots (vr == -1) must not be rewritten into registers;
   * that would create uninitialized reads.  Locals with a vreg must
   * participate in allocation so defs/uses stay consistent. */
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

    /* Stack-passed params (not allocated to a register) live in the caller's
     * argument area (VT_PARAM).  AAPCS guarantees that area is valid for the
     * call's duration and provides an addressable home for '&param'. */
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

    /* Register-passed params already in a register need no VT_LVAL — the
     * value is already there.  VT_LVAL is only needed for &param or stack
     * params (VT_LOCAL). */
    int is_register_param =
        (TCCIR_DECODE_VREG_TYPE(sv->vr) == TCCIR_VREG_TYPE_PARAM && interval && interval->incoming_reg0 >= 0);

    sv->pr0_reg = interval->allocation.r0 & PREG_REG_NONE;
    sv->pr0_spilled = (interval->allocation.r0 & PREG_SPILLED) != 0;
    sv->pr1_reg = interval->allocation.r1 & PREG_REG_NONE;
    sv->pr1_spilled = (interval->allocation.r1 & PREG_SPILLED) != 0;
    sv->c.i = interval->allocation.offset;

    /* Preserve VT_LVAL for pointer derefs and address-of ops; not for
     * register-resident params (value already there) or address-of
     * expressions (we want the address, not the value). */
    int preserve_flags = old_r & VT_PARAM; /* Always preserve VT_PARAM */
    if ((old_r & VT_LVAL) && old_v < VT_CONST && old_v != VT_LOCAL && old_v != VT_LLOCAL && !is_register_param)
    {
      /* The vreg holds a pointer that needs dereferencing.
       * Note: VT_LOCAL/VT_LLOCAL use VT_LVAL to mean "load from stack slot";
       * promoting such a local/param to a register must NOT preserve VT_LVAL
       * or we turn a plain value into a pointer dereference. */
      preserve_flags |= VT_LVAL;
    }

    if ((interval->allocation.r0 & PREG_SPILLED) || interval->allocation.offset != 0)
    {
      /* Spilled to stack — treat as local.
       * Computed values always need VT_LVAL to load from the spill slot.
       * Locals preserve their existing VT_LVAL to distinguish load vs address-of.
       *
       * DOUBLE INDIRECTION: If old_r has VT_LVAL but the original was NOT
       * VT_LOCAL/VT_LLOCAL, the code wants to DEREFERENCE the spilled value
       * (load pointer, then deref).  Use VT_LLOCAL to encode this.
       * VT_LOCAL/VT_LLOCAL's VT_LVAL means "access this stack slot", not
       * "dereference pointer in vreg" — do NOT use VT_LLOCAL there.
       *
       * ADDRESS-OF: If old_v == VT_LOCAL and old_r lacks VT_LVAL, this is
       * &var — we want the spill-slot address, not its contents. */
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
      /* Only preserve VT_PARAM for stack-passed params (incoming_reg0 < 0).
       * Spilled register params live in the callee's local stack area
       * (negative FP offset), not the caller's argument area, so VT_PARAM
       * would incorrectly add offset_to_args. */
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
    /* No valid vreg and either invalid .r or a constant — preserve flags.
     * Handles VT_CONST|VT_SYM (global symbols) and plain constants. */
    int flags = sv->r & (VT_LVAL | VT_SYM);
    sv->r = VT_CONST | flags;
  }
  else if (sv->vr == -1 && old_r == 0 && sv->sym)
  {
    /* Special case: old_r=0 with a symbol — function symbol ref not marked
     * VT_CONST. Preserve the symbol. */
    sv->r = VT_CONST | VT_SYM;
  }
}

/* True if any operand of `q` reads or writes a volatile-qualified local slot.
 * Used to defeat the store→reload spill-cache peephole for volatile accesses:
 * each volatile load must reach memory, never reuse a just-stored register. */
static int ir_codegen_op_touches_volatile(TCCIRState *ir, IRQuadCompact *q)
{
  IROperand ops[4];
  int no = 0;
  if (irop_config[q->op].has_dest) ops[no++] = tcc_ir_op_get_dest(ir, q);
  if (irop_config[q->op].has_src1) ops[no++] = tcc_ir_op_get_src1(ir, q);
  if (irop_config[q->op].has_src2) ops[no++] = tcc_ir_op_get_src2(ir, q);
  if (q->op == TCCIR_OP_MLA) ops[no++] = tcc_ir_op_get_accum(ir, q);
  for (int k = 0; k < no; k++)
  {
    int32_t vr = irop_get_vreg(ops[k]);
    if (vr < 0)
      continue;
    int t = TCCIR_DECODE_VREG_TYPE(vr);
    if (t != TCCIR_VREG_TYPE_VAR && t != TCCIR_VREG_TYPE_PARAM)
      continue;
    if (!tcc_ir_vreg_is_valid(ir, vr))
      continue;
    IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, vr);
    if (iv && iv->is_volatile)
      return 1;
  }
  return 0;
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
  int argno = 0;             // current GPR argument register (r0-r3)
  unsigned vfp_used = 0;     // bitmap of allocated VFP arg registers s0-s15
  const int hard_float = (tcc_state && tcc_state->float_abi == ARM_HARD_FLOAT && !ir->is_variadic);
  for (int vreg = 0; vreg < ir->next_parameter; ++vreg)
  {
    const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
    IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, encoded_vreg);

    /* Hard-float: scalar float/double parameters arrive in VFP registers
     * (s0..s15, viewed as d0..d7), consuming an independent bank — not GPR
     * slots.  Allocation back-fills exactly as tcc_abi_classify_argument does:
     * a float takes the lowest free s-register, a double the lowest free
     * even-aligned pair.  incoming_reg0 is marked with LS_VFP_REG_BASE so the
     * prolog homes it from the right register file. */
    if (hard_float && interval && interval->is_float && !interval->is_complex &&
        interval->incoming_reg0 < 0)
    {
      const int slots = interval->is_double ? 2 : 1;
      const int step = slots;
      int base = -1;
      for (int cand = 0; cand + slots <= 16; cand += step)
      {
        const unsigned mask = (unsigned)((1u << slots) - 1u) << cand;
        if (!(vfp_used & mask))
        {
          vfp_used |= mask;
          base = cand;
          break;
        }
      }
      if (base >= 0)
      {
        interval->incoming_reg0 = LS_VFP_REG_BASE + base;
        interval->incoming_reg1 = -1;
        continue;
      }
    }
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
        /* Don't overwrite interval->allocation here — it was set by the linear
         * scanner and argno (param index) is NOT the physical register number.
         * The prolog uses incoming_reg0/1 to know where the param arrives. */
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
        /* Don't overwrite allocator spill slots with caller-stack offsets.
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
        /* Don't overwrite allocator spill slots with caller-stack offsets.
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

  /* Find FUNCCALLVAL instructions that produce return values */
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    /* dest is the vreg that receives the return value */
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.vr < 0 || !tcc_ir_vreg_is_valid(ir, dest.vr))
      continue;

    /* Never on a parameter.  For a PARAM interval incoming_reg0 is not a
     * hint but ABI state: it records where the argument arrives, and < 0
     * means "in the caller's frame, at original_offset".  A call that
     * assigns to a parameter (`u64_9 %= x;` on the ninth argument) would
     * otherwise flip a stack-passed param's -1 to 0, after which
     * machine_op_from_ir stops recognising it as MACH_OP_PARAM_STACK and
     * lowers every read and write of it as a spill at frame offset 0 —
     * on top of the pushed registers.  See pr69447. */
    if (TCCIR_DECODE_VREG_TYPE(dest.vr) == TCCIR_VREG_TYPE_PARAM)
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

  /* Hint post-allocation swap pass: mark RETURNVALUE's source vreg with
   * incoming_reg0=0.  Only at -O1+ to avoid interfering with -O0 paths. */
  if (tcc_state->optimize < 1)
    return;

  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_RETURNVALUE)
      continue;

    const IROperand src = tcc_ir_op_get_src1(ir, q);
    if (!irop_has_vreg(src) || irop_is_immediate(src))
      continue;

    int32_t vr = irop_get_vreg(src);

    for (int depth = 0; depth < 5; depth++)
    {
      if (vr < 0 || !tcc_ir_vreg_is_valid(ir, vr))
        break;
      if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_PARAM)
        break;
      int found = 0;
      for (int j = i - 1; j >= 0; j--)
      {
        IRQuadCompact *dq = &ir->compact_instructions[j];
        if (dq->op == TCCIR_OP_NOP)
          continue;
        if (!irop_config[dq->op].has_dest)
          continue;
        IROperand dd = tcc_ir_op_get_dest(ir, dq);
        if (irop_get_vreg(dd) != vr)
          continue;
        if (dq->op == TCCIR_OP_LOAD || dq->op == TCCIR_OP_ASSIGN)
        {
          IROperand ds = tcc_ir_op_get_src1(ir, dq);
          if (irop_has_vreg(ds) &&
              TCCIR_DECODE_VREG_TYPE(irop_get_vreg(ds)) != TCCIR_VREG_TYPE_PARAM)
          {
            vr = irop_get_vreg(ds);
            found = 1;
          }
        }
        break;
      }
      if (!found)
        break;
    }

    if (vr >= 0 && tcc_ir_vreg_is_valid(ir, vr) &&
        TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_PARAM)
    {
      IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vr);
      if (interval && interval->incoming_reg0 < 0)
        interval->incoming_reg0 = 0; /* hint: prefer r0 */
    }
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
  unsigned vfp_used = 0;
  const int hard_float = (tcc_state && tcc_state->float_abi == ARM_HARD_FLOAT && !ir->is_variadic);
  for (int vreg = 0; vreg < param_count; ++vreg)
  {
    const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
    IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, encoded_vreg);
    if (!interval)
      continue;

    /* Hard-float: a scalar float/double param consumes VFP argument slots
     * (s0..s15), not GPR ones — it is only stack-passed once the VFP bank is
     * full.  Must mirror tcc_ir_register_allocation_params (same back-filling
     * rule) or in-register float params get their linear-scan allocation
     * wrongly reset here and collapse onto a shared stack slot. */
    if (hard_float && interval->is_float && !interval->is_complex)
    {
      const int slots = interval->is_double ? 2 : 1;
      int base = -1;
      for (int cand = 0; cand + slots <= 16; cand += slots)
      {
        const unsigned mask = (unsigned)((1u << slots) - 1u) << cand;
        if (!(vfp_used & mask))
        {
          vfp_used |= mask;
          base = cand;
          break;
        }
      }
      if (base < 0)
        is_stack_passed[vreg] = 1;
      continue;
    }

    const int is_64bit = interval->is_double || interval->is_llong;
    if (is_64bit && (argno & 1))
      argno++; /* align 64-bit to even reg pair */

    const int in_regs = is_64bit ? (argno <= 2) : (argno <= 3);
    if (!in_regs)
      is_stack_passed[vreg] = 1;

    argno += is_64bit ? 2 : 1;
  }

  /* Rewrite linear-scan results: stack-passed params already have a memory
   * home (caller arg area), so drop any local spill slot.  Also force
   * address-taken stack params to remain in memory. */
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

    /* Stack-passed params live in the caller's argument area.  If linear-scan
     * assigned them a register (without spilling), the prolog won't load
     * them — always reset r0/r1 to force use of the incoming stack location. */
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
  /* Guard against invalid vtop (empty structs) */
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
      /* Merge pending jump chains with the comparison */
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
    /* TCC comparison tokens: XORing with 1 inverts (e.g. TOK_EQ ^ 1 = TOK_NE) */
    int cond = vtop->cmp_op ^ invert;
    src.c.i = cond;
    dest.vr = -1;
    dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
    dest.c.i = test;
    test = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &src, NULL, &dest);

    /* Handle pending jump chains */
    if (invert)
    {
      /* inv=1: jump when condition is false — merge "jump-on-false" chain,
       * patch "jump-on-true" to fall through. */
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
      /* inv=0: jump when condition is true — merge "jump-on-true" chain,
       * patch "jump-on-false" to fall through. */
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

  /* Compute reserved_regs: physical registers of live vregs that are NOT
   * asm operands.  The constraint solver must avoid these when picking "r"
   * constraints, otherwise operand loads clobber live values.
   *
   * Unlike clobber_regs, reserved_regs only affect constraint allocation —
   * they don't trigger save/restore in asm prolog/epilog. */
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

    /* Asm operands can reuse their assigned physical registers.  Only non-
     * operand live values need to remain reserved — otherwise inline asm with
     * several live register operands spuriously runs out of allocatable "r"
     * registers. */
    for (int i = 0; i < nb_operands; ++i)
    {
      /* For lvalue operands like "+r"(*p), pr0_reg holds the ADDRESS (pointer),
       * not the value.  That pointer is read by both prolog load and epilog
       * store, so it must survive across the asm body.  Keep lvalue-operand
       * registers reserved so the value gets a distinct register. */
      if (vals[i].r & VT_LVAL)
        continue;
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
   * Reference point is table_start (PC at the 16-bit ADD Rt,PC instruction).
   * Formula: table[i] = (target_addr | 1) - table_start
   * Must happen after all code is generated so forward targets are mapped. */
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
 * After the dry run identifies instructions that would push a register
 * (no free scratch available), this function tries to move the vreg
 * currently occupying that register to a free callee-saved register.
 * This eliminates the push/pop overhead.
 *
 * Returns the new physical register on success, -1 if no reassignment
 * could be made (e.g. all callee-saved registers occupied over the vreg's
 * live range, or the interval is complex / 64-bit / float).
 */
static int try_reassign_scratch_conflict(TCCIRState *ir, int r, int insn_i)
{
  LSLiveIntervalState *ls = &ir->ls;

  /* Callee-saved registers R4-R11 (bits 4..11 = 0x0FF0), minus reserved
   * special-purpose registers:
   *   R7  = R_FP (= 7): always reserved as frame pointer.
   *   R10 = static_chain_reg (= 10): reserved when function uses a static chain.
   *   R9: reserved when text_and_data_separation (GOT base).
   */
  const uint32_t ALL_CALLEE_SAVED = 0x0FF0u;
  const uint32_t ARM_FP_REG = 7u;         /* R_FP = R7, defined in thumb.h */
  const uint32_t ARM_R9 = 9u;             /* R9 = GOT base pointer when text_and_data_separation */
  uint32_t reserved = (1u << ARM_FP_REG); /* always exclude frame pointer */
  if (tcc_state->text_and_data_separation)
    reserved |= (1u << ARM_R9); /* R9 holds GOT base — must not be clobbered */
  if (ir->has_static_chain || ir->emits_set_chain)
    reserved |= (1u << (uint32_t)architecture_config.static_chain_reg);
  /* The .rodata anchor holds a value for the whole body with no interval
   * behind it, so `blocked` below never reports it live. */
  if (tcc_gen_machine_rodata_anchor_get() >= 0)
    reserved |= 1u << (uint32_t)tcc_gen_machine_rodata_anchor_get();
  const uint32_t CALLEE_SAVED = ALL_CALLEE_SAVED & ~reserved;

  /* Find the LSLiveInterval holding r at instruction insn_i. */
  int holder = tcc_ls_find_int_reg_holder(ls, r, insn_i);
  if (holder < 0)
    return -1;
  LSLiveInterval *ls_iv = &ls->intervals[holder];

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
  /* Skip phi-pinned intervals: their register is relied upon by identity phi
   * resolution (no copy was emitted because src and dest share the same reg). */
  if (ir_iv->phi_pinned)
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
  if (tcc_state && tcc_state->verbose)
    fprintf(stderr, "[phase3-fixup] insn=%d vreg=%d R%d->R%d (blocked=0x%x avail=0x%x)\n", insn_i, (int)ls_iv->vreg, r,
            new_r, blocked, avail);

  /* 1. Update the IRLiveInterval (read by machine_op_from_ir). */
  ir_iv->allocation.r0 = (uint16_t)new_r;

  /* 2. Update the LSLiveInterval (read by tcc_ls_build_live_regs_by_instruction
   *    and tcc_ls_find_free_scratch_reg). */
  ls_iv->r0 = (int16_t)new_r;

  /* 3. Patch live_regs_by_instruction for the interval's full range.
   * r's bit may be shared with another interval that move-coalescing put on
   * the same register (in-place two-address ops) — only clear positions
   * where no other claimant is still live. */
  if (ls->live_regs_by_instruction)
  {
    for (int j = (int)ls_iv->start; j <= (int)ls_iv->end && j < ls->live_regs_by_instruction_size; j++)
    {
      if (!tcc_ls_reg_held_by_other(ls, r, j, ls_iv))
        ls->live_regs_by_instruction[j] &= ~(1u << r);
      ls->live_regs_by_instruction[j] |= (1u << new_r);
    }
  }

  /* 4. Mark new_r as dirty so the prologue will save/restore it. */
  ls->dirty_registers |= (1ull << new_r);

  return new_r;
}

/* How many allocatable registers the fixup has freed at one instruction, i.e.
 * how many extra scratch registers the real run will find there compared to
 * the dry run that recorded the save.  R7 (frame pointer), SP and PC are never
 * handed out as scratch, so they must not count. */
static int ir_codegen_regs_freed_at(uint32_t orig_live, uint32_t live_now)
{
  const uint32_t allocatable = 0x1F7Fu; /* R0-R6, R8-R12 */
  return __builtin_popcount(orig_live & ~live_now & allocatable);
}

/* ============================================================================
 * Phase-3 scratch conflict fixup, last resort: demote the blocker to memory
 * ============================================================================
 *
 * try_reassign_scratch_conflict() can only move the blocking vreg to another
 * register.  When every register is occupied across the blocker's whole live
 * range there is nowhere to move it, and the real run then pays a
 * save/use/restore round trip (STR + LDR, plus the reload of anything the
 * scratch clobbered) at EVERY instruction in that window.
 *
 * 20041011-1 is the pathological case: 30 volatile ints copied in and out
 * inside a loop.  The allocator hands out all 10 free registers, so the
 * remaining 20 copies each cost 7 instructions instead of 2 — the scratch
 * machinery spends 4 of them saving and restoring the two registers it
 * borrows, and the third is a literal-pool reload of the global's address
 * that only exists because the borrow invalidated the cached copy.
 *
 * Spilling ONE such blocker outright turns its register into a permanently
 * free scratch for the whole window.  The value then costs one memory access
 * per reference, which is cheaper than two per blocked instruction as soon as
 * the window is wider than the vreg's use count.
 *
 * Returns the demoted vreg on success, -1 when nothing could be demoted.  The
 * caller assigns the stack slot afterwards (the frame's low areas have not
 * been laid out yet at this point), so the interval is left marked with
 * DEMOTE_PENDING_SLOT until then.
 */
#define DEMOTE_PENDING_SLOT 1 /* non-zero placeholder; frame offsets are negative */

/* Decoded operand vregs, one [dest, src1, src2] triple per instruction.
 *
 * The reference count at the bottom of try_demote_scratch_conflict() is the
 * only part of it that has to walk the candidate's whole live range, and it
 * re-decodes three operands at every instruction on the way.  The caller runs
 * it once per (instruction, saved register) pair, so a function with wide
 * ranges and many predicted saves decodes the same operands over and over:
 * mibench_rijndael calls it 136 times and spends ~6% of its entire compile in
 * here, over half of that inside the operand accessors alone.
 *
 * ir->compact_instructions does not change during the phase-3 fixup — only
 * interval allocations and live_regs_by_instruction do — so the decode is done
 * once and read back afterwards.  Built lazily: nearly every function bails out
 * at one of the filters above the counting loop and must not pay for a table it
 * never reads. */
#define DEMOTE_REF_SLOTS 3
/* Absent operand.  Distinct both from irop_get_vreg()'s "not a vreg" (-1) and
 * from any encoded vreg, whose type nibble is 1..3, so it can never compare
 * equal to a candidate. */
#define DEMOTE_REF_NONE INT32_MIN

static int32_t *demote_ref_cache_build(TCCIRState *ir, int n_insns)
{
  int32_t *cache = tcc_malloc((size_t)n_insns * DEMOTE_REF_SLOTS * sizeof(int32_t));

  for (int j = 0; j < n_insns; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    int32_t *slot = &cache[(size_t)j * DEMOTE_REF_SLOTS];

    slot[0] = slot[1] = slot[2] = DEMOTE_REF_NONE;
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_dest)
      slot[0] = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (irop_config[q->op].has_src1)
      slot[1] = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
    if (irop_config[q->op].has_src2)
      slot[2] = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
  }
  return cache;
}

/* Prices the O(interval length) rescan this pass does per candidate. */
TCC_DBG_ENV_FLAG(cg_no_scratch_demote, "TCC_NO_SCRATCH_DEMOTE")
/* Dumps the post-opt IR that codegen is about to consume. */
TCC_DBG_ENV_FLAG(cg_dump_ir_cg, "DUMP_IR_CG")
/* A/B levers for the two codegen walks priced in docs/remote_smoke_speedup_plan.md. */
TCC_DBG_ENV_FLAG(cg_keep_fwd_dry, "TCC_KEEP_FWD_DRY")
TCC_DBG_ENV_FLAG(cg_no_rehearsal, "TCC_NO_REHEARSAL")

static int try_demote_scratch_conflict(TCCIRState *ir, int r, int insn_i, const uint16_t *dry_insn_saves,
                                       const int *dry_insn_scratch, int n_insns, int32_t **ref_cache)
{
  LSLiveIntervalState *ls = &ir->ls;

  if (cg_no_scratch_demote())
    return -1;

  int holder = tcc_ls_find_int_reg_holder(ls, r, insn_i);
  if (holder < 0)
    return -1;
  LSLiveInterval *ls_iv = &ls->intervals[holder];

  /* Already memory-backed, or part of a graph-coalesced class whose copies
   * were erased on the assumption that both ends share this register. */
  if (ls_iv->stack_location != 0 || ls_iv->addrtaken || ls_iv->co_member)
    return -1;
  if (ls_iv->reg_type != LS_REG_TYPE_INT || ls_iv->r1 >= 0)
    return -1;

  IRLiveInterval *ir_iv = tcc_ir_get_live_interval(ir, (int)ls_iv->vreg);
  if (!ir_iv)
    return -1;
  if (ir_iv->is_float || ir_iv->is_double || ir_iv->is_llong || ir_iv->is_complex || ir_iv->use_vfp)
    return -1;
  /* ABI-pinned (parameter / call result) and phi-pinned intervals must keep
   * the register the surrounding code already committed to. */
  if (ir_iv->incoming_reg0 >= 0 || ir_iv->phi_pinned || ir_iv->addrtaken)
    return -1;
  if (ir_iv->allocation.offset != 0)
    return -1;
  if (ir_iv->allocation.r0 != (uint16_t)r)
    return -1;
  /* A rematerializable constant is reloaded with a MOV, never from memory —
   * demoting it frees the register without any memory traffic, but it also
   * cannot be the thing that makes an instruction need scratch. Leave it. */
  if (ir_iv->remat_kind != 0)
    return -1;

  const int start = (int)ls_iv->start;
  const int end = (int)ls_iv->end;
  if (start < 0 || end < start || end >= n_insns)
    return -1;

  /* The register must be held by this interval ALONE over its whole range.
   * A second claimant means move coalescing put a copy's two ends on the same
   * register and erased the copy; demoting one end would drop the value. */
  for (int k = 0; k < ls->next_interval_index; k++)
  {
    const LSLiveInterval *other = &ls->intervals[k];
    if (other == ls_iv || other->stack_location != 0)
      continue;
    if (other->r0 != r && other->r1 != r)
      continue;
    if ((int)other->start <= end && (int)other->end >= start)
      return -1;
  }

  /* Benefit: every instruction inside the range that the dry run had to save
   * r at stops paying STR+LDR (2 instructions).  Cost: one memory access per
   * reference to the vreg.
   *
   * An instruction that references the vreg does NOT count as benefit: after
   * the demotion it still needs a register there, to hold the reloaded value,
   * so the borrow does not go away — it only changes what it is for.  Counting
   * those was what made 920928-2::g look like a 6-instruction win when it was
   * a 6-instruction loss (every blocked instruction was also a reference). */
  if (!*ref_cache)
    *ref_cache = demote_ref_cache_build(ir, n_insns);

  const int32_t vreg = (int32_t)ls_iv->vreg;
  int blocked = 0;
  int refs = 0;
  for (int j = start; j <= end; j++)
  {
    const int32_t *slot = &(*ref_cache)[(size_t)j * DEMOTE_REF_SLOTS];
    int here = (slot[0] == vreg) + (slot[1] == vreg) + (slot[2] == vreg);
    refs += here;
    if (here || !(dry_insn_saves[j] & (1u << (unsigned)r)))
      continue;
    /* dry_insn_saves is a pass-0 prediction made before the reassignment
     * fixups above (and any earlier demotion) ran.  Only count j when the real
     * run will still come up short of scratch registers there — otherwise the
     * predicted save never happens and the demotion buys nothing (920928-2::g:
     * six predicted saves, none real, six extra reloads of a pointer that had
     * been sitting in R0).  The comparison is against how many scratch
     * registers the instruction wants, not against zero: an instruction that
     * borrows two still pays for the second when only one is free. */
    if (ls->live_regs_by_instruction && j < ls->live_regs_by_instruction_size)
    {
      uint32_t free_now = ~ls->live_regs_by_instruction[j] & 0x1F7Fu & ~(1u << (unsigned)r);
      if (__builtin_popcount(free_now) >= dry_insn_scratch[j])
        continue;
    }
    blocked++;
  }
  /* Demanding a real window, not a one-off blip: freeing the register is not
   * guaranteed to remove the save (the instruction may want two scratches, or
   * the borrow may be excluded by the operands), while the added memory
   * traffic is certain.  Below this width the model is not reliable enough —
   * every corpus regression measured came from a 2-instruction window. */
  if (blocked < 4)
    return -1;
  /* The +4 margin covers the reload's own scratch needs and the saves that
   * freeing a single register does not remove. */
  if (2 * blocked <= refs + 4)
    return -1;

  if (tcc_state && tcc_state->verbose)
    fprintf(stderr, "[phase3-demote] insn=%d vreg=%d R%d -> memory (blocked=%d refs=%d range=%d..%d)\n", insn_i,
            (int)ls_iv->vreg, r, blocked, refs, start, end);

  /* --- Apply the demotion.  The real slot is assigned by the caller. --- */
  ls_iv->r0 = -1;
  ls_iv->stack_location = DEMOTE_PENDING_SLOT;
  ir_iv->allocation.r0 = PREG_SPILLED | PREG_REG_NONE;
  ir_iv->allocation.r1 = PREG_NONE;
  ir_iv->allocation.offset = DEMOTE_PENDING_SLOT;

  /* No other interval claims r over [start, end] (checked above), so the whole
   * window can be released unconditionally. */
  if (ls->live_regs_by_instruction)
  {
    for (int j = start; j <= end && j < ls->live_regs_by_instruction_size; j++)
      ls->live_regs_by_instruction[j] &= ~(1u << (unsigned)r);
  }

  return (int)ls_iv->vreg;
}

/* ============================================================================
 * Helper: sub-component fixup for register-pair operands used as LOAD/STORE
 * sources.  When a STACKOFF operand accesses a sub-component of a 64-bit
 * pair (e.g., __imag__ on _Complex float), the byte offset differs from the
 * interval's base offset.  Rewrite the MachineOperand to use r1 instead of
 * r0.
 *
 * MUST NOT be applied to DP/ASSIGN operands — a 64-bit pair allocated as a
 * register pair can also have a non-zero delta, but that is not a
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

/* Check whether any live interval (other than skip_vreg) is allocated to
 * physical register `reg` and overlaps [start, end].  Returns true if a
 * conflict exists, meaning we cannot reassign skip_vreg to `reg`. */
static bool ir_reg_conflict(const TCCIRState *ir, int reg, uint32_t start, uint32_t end, int skip_vreg)
{
  const struct
  {
    const IRLiveInterval *arr;
    int count;
  } pools[] = {
      {ir->variables_live_intervals, ir->next_local_variable},
      {ir->temporary_variables_live_intervals, ir->next_temporary_variable},
      {ir->parameters_live_intervals, ir->next_parameter},
  };
  for (int p = 0; p < 3; p++)
  {
    for (int k = 0; k < pools[p].count; k++)
    {
      const IRLiveInterval *other = &pools[p].arr[k];
      if (other->allocation.r0 != (uint16_t)reg)
        continue;
      /* Skip the vreg we're about to reassign */
      if (p == 0 && k == TCCIR_DECODE_VREG_POSITION(skip_vreg) &&
          TCCIR_DECODE_VREG_TYPE(skip_vreg) == TCCIR_VREG_TYPE_VAR)
        continue;
      if (p == 1 && k == TCCIR_DECODE_VREG_POSITION(skip_vreg) &&
          TCCIR_DECODE_VREG_TYPE(skip_vreg) == TCCIR_VREG_TYPE_TEMP)
        continue;
      if (p == 2 && k == TCCIR_DECODE_VREG_POSITION(skip_vreg) &&
          TCCIR_DECODE_VREG_TYPE(skip_vreg) == TCCIR_VREG_TYPE_PARAM)
        continue;
      /* Check overlap: intervals [other->start, other->end] ∩ [start, end] */
      if (other->start <= end && other->end >= start)
        return true;
    }
  }
  return false;
}

/* ============================================================================
 * Pre-prologue FUNCPARAMVAL allocation patching
 *
 * When a LOAD/LOAD_INDEXED/LOAD_POSTINC/ASSIGN produces a value immediately
 * consumed by FUNCPARAMVAL (param 0..3), the codegen peephole loads directly
 * into the ABI register (R0-R3) instead of the allocator-assigned register.
 * If the allocator assigned a callee-saved register, it becomes a ghost save.
 *
 * This pre-pass patches those allocations BEFORE prologue emission so that
 * dirty_registers can be recomputed accurately.  Mirrors the logic in
 * ir_codegen_before_ret_peephole's FUNCPARAMVAL branch, but only patches
 * allocations (no MachineOperand output needed).
 * ============================================================================ */
static void ir_codegen_pre_patch_funcparam_allocations(TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    /* Only instructions that produce a value and use peephole (.dest = 2) */
    if (op != TCCIR_OP_LOAD && op != TCCIR_OP_LOAD_INDEXED && op != TCCIR_OP_LOAD_POSTINC && op != TCCIR_OP_ASSIGN &&
        op != TCCIR_OP_SELECT && op != TCCIR_OP_FUNCCALLVAL)
      continue;

    IROperand dest_ir = tcc_ir_op_get_dest(ir, &ir->compact_instructions[i]);
    int dest_vr = irop_get_vreg(dest_ir);
    if (dest_vr < 0)
      continue;

    /* 64-bit values need register pairs — skip (peephole skips them too) */
    if (irop_needs_pair(dest_ir))
      continue;

    /* Find next non-NOP instruction */
    int j = i + 1;
    while (j < ir->next_instruction_index && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= ir->next_instruction_index)
      continue;

    /* Must also not be a jump target (another path could reach it without
     * executing instruction i, making the peephole unsafe). */
    if (j < ir->next_instruction_index && ir->compact_instructions[j].is_jump_target)
      continue;

    if (ir->compact_instructions[j].op != TCCIR_OP_FUNCPARAMVAL)
      continue;

    IROperand nq_src1 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[j]);
    int next_vr = irop_get_vreg(nq_src1);
    if (next_vr != dest_vr)
      continue;

    if (irop_op_is_lval(nq_src1))
      continue;

    IRLiveInterval *li = tcc_ir_get_live_interval(ir, dest_vr);
    if (!li || li->start != (uint32_t)i)
      continue;

    /* Find the CALL that consumes this parameter */
    int call_idx = -1;
    for (int k = j + 1; k < ir->next_instruction_index; k++)
    {
      TccIrOp kop = ir->compact_instructions[k].op;
      if (kop == TCCIR_OP_NOP || kop == TCCIR_OP_FUNCPARAMVAL)
        continue;
      if (kop == TCCIR_OP_FUNCCALLVAL || kop == TCCIR_OP_FUNCCALLVOID)
      {
        call_idx = k;
        break;
      }
      break;
    }
    if (call_idx < 0 || li->end != (uint32_t)call_idx)
      continue;

    /* Decode parameter index */
    IROperand nq_src2 = tcc_ir_op_get_src2(ir, &ir->compact_instructions[j]);
    uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, nq_src2);
    int param_idx = TCCIR_DECODE_PARAM_IDX(encoded);
    if (param_idx > 3)
      continue;

    int target_reg = param_idx;

    /* Check no register conflict */
    if (ir_reg_conflict(ir, target_reg, li->start, li->end > 0 ? li->end - 1 : 0, dest_vr))
      continue;

    /* Patch allocation to ABI register */
    li->allocation.r0 = (uint16_t)target_reg;
    li->allocation.offset = 0;
  }
}

/* ============================================================================
 * Recompute dirty_registers from actual IRLiveInterval allocations
 *
 * After peephole/pre-patch optimizations change IRLiveInterval.allocation,
 * the allocator's dirty_registers bitmap may contain callee-saved registers
 * no longer referenced by any interval.  Rebuild from ground truth.
 * ============================================================================ */
static void ir_codegen_recompute_dirty_from_allocations(TCCIRState *ir)
{
  uint64_t callee_mask = 0;
  for (int r = 4; r <= 11; ++r)
    callee_mask |= (1ULL << r);

  /* Collect registers actually referenced by any interval allocation. */
  uint64_t used = 0;

#define SCAN_INTERVALS(arr, count)                                                                                     \
  for (int _i = 0; _i < (count); ++_i)                                                                                 \
  {                                                                                                                    \
    const IRLiveInterval *_li = &(arr)[_i];                                                                            \
    uint16_t _r0 = _li->allocation.r0;                                                                                 \
    uint16_t _r1 = _li->allocation.r1;                                                                                 \
    if (!(_r0 & PREG_SPILLED) && _r0 != PREG_NONE && _r0 < 16)                                                         \
      used |= (1ULL << _r0);                                                                                           \
    if (!(_r1 & PREG_SPILLED) && _r1 != PREG_NONE && _r1 < 16)                                                         \
      used |= (1ULL << _r1);                                                                                           \
  }

  SCAN_INTERVALS(ir->parameters_live_intervals, ir->next_parameter);
  SCAN_INTERVALS(ir->variables_live_intervals, ir->next_local_variable);
  SCAN_INTERVALS(ir->temporary_variables_live_intervals, ir->next_temporary_variable);
#undef SCAN_INTERVALS

  uint64_t old_dirty = ir->ls.dirty_registers;
  uint64_t non_callee = old_dirty & ~callee_mask;
  uint64_t callee_dirty = old_dirty & callee_mask;
  ir->ls.dirty_registers = non_callee | (callee_dirty & used);
}

/* ============================================================================
 * Before-Return Peephole
 *
 * When a LOAD, LOAD_INDEXED, or ASSIGN is immediately followed by a
 * RETURNVALUE on the same vreg (no intervening jump target), patch the
 * dest vreg's allocation to R0 (R0+R1 for 64-bit) and construct a synthetic
 * MACH_OP_REG MachineOperand.  This eliminates the extra move that
 * RETURNVALUE would otherwise emit.
 *
 * Called from both dry-run and real-run dispatch loops so that scratch
 * accounting stays consistent.
 * ============================================================================ */
static bool ir_codegen_before_ret_peephole(TCCIRState *ir, int i, const IROperand *dest_ir,
                                           MachineOperand *out_mop_dest)
{
  int dest_vr = irop_get_vreg(*dest_ir);
  if (dest_vr < 0)
    return false;

  /* Find the next non-NOP instruction.  NOPs are skipped regardless of
   * is_jump_target — a branch landing on a NOP falls through without
   * affecting register state, so the peephole assumption ("instruction i's
   * result flows to j") remains valid.  Only check is_jump_target on the
   * actual consumer (non-NOP) — if a branch can reach it without executing
   * instruction i, the peephole would produce wrong code. */
  int j = i + 1;
  while (j < ir->next_instruction_index)
  {
    if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
      break;
    j++;
  }
  if (j >= ir->next_instruction_index)
    return false;

  if (ir->compact_instructions[j].is_jump_target)
  {
    return false;
  }

  const IRQuadCompact *nq = &ir->compact_instructions[j];
  const int needs_pair = irop_needs_pair(*dest_ir);

  if (nq->op == TCCIR_OP_RETURNVALUE)
  {
    IROperand nq_src1 = tcc_ir_op_get_src1(ir, nq);
    int next_vr = irop_get_vreg(nq_src1);
    if (next_vr != dest_vr)
      return false;

    IRLiveInterval *li = tcc_ir_get_live_interval(ir, dest_vr);
    if (!li || li->start != (uint32_t)i)
      return false;

    li->allocation.r0 = REG_IRET;
    li->allocation.offset = 0;
    if (needs_pair)
      li->allocation.r1 = REG_IRE2;

    *out_mop_dest = (MachineOperand){.kind = MACH_OP_REG,
                                     .btype = irop_get_btype(*dest_ir),
                                     .vreg = dest_vr,
                                     .is_64bit = needs_pair,
                                     .is_unsigned = dest_ir->is_unsigned,
                                     .needs_deref = false,
                                     .u.reg = {.r0 = REG_IRET, .r1 = needs_pair ? (int)REG_IRE2 : -1}};
    return true;
  }

  /* Peephole: when the next instruction is an ASSIGN copying our dest
   * vreg into another register, load directly into the ASSIGN's destination.
   * Eliminates "ldr rT, [pc,#imm]; mov rD, rT" sequences.
   *
   * Safety: the dest vreg must die at the ASSIGN (end == i+1), ensuring no
   * other instruction reads the old allocation. */
  if (nq->op == TCCIR_OP_ASSIGN)
  {
    IROperand nq_src1 = tcc_ir_op_get_src1(ir, nq);
    int next_vr = irop_get_vreg(nq_src1);
    if (next_vr != dest_vr)
      return false;

    /* dest vreg must be defined here and die at the ASSIGN — no earlier
     * definitions and no later uses.  If li->start < i, another instruction
     * (e.g. conditional set on an alternate path) also defines the vreg;
     * patching the global allocation would break that earlier instruction. */
    IRLiveInterval *li = tcc_ir_get_live_interval(ir, dest_vr);
    if (!li || li->start != (uint32_t)i || li->end != (uint32_t)j)
      return false;

    /* Get the ASSIGN's destination vreg and its register allocation */
    IROperand nq_dest = tcc_ir_op_get_dest(ir, nq);
    int assign_dest_vr = irop_get_vreg(nq_dest);
    if (assign_dest_vr < 0)
      return false;

    /* Both source and destination must have matching pair requirements */
    int dest_needs_pair = irop_needs_pair(nq_dest);
    if (dest_needs_pair != needs_pair)
      return false;

    IRLiveInterval *dest_li = tcc_ir_get_live_interval(ir, assign_dest_vr);
    if (!dest_li)
      return false;

    int target_r0 = (int)dest_li->allocation.r0;
    int target_r1 = needs_pair ? (int)dest_li->allocation.r1 : -1;

    /* Target must be a valid physical register, not spilled (r0 >= NB_REGS
     * means spilled to stack — e.g. r0=63 is a spill sentinel). */
    if (target_r0 >= NB_REGS)
      return false;

    /* For 64-bit pairs, both halves must have valid registers */
    if (needs_pair && (target_r1 < 0 || target_r1 >= NB_REGS))
      return false;

    /* Patch the source vreg's allocation to the ASSIGN's destination register */
    li->allocation.r0 = (uint16_t)target_r0;
    li->allocation.offset = 0;
    if (needs_pair)
      li->allocation.r1 = (uint16_t)target_r1;

    *out_mop_dest = (MachineOperand){.kind = MACH_OP_REG,
                                     .btype = irop_get_btype(*dest_ir),
                                     .vreg = dest_vr,
                                     .is_64bit = needs_pair,
                                     .is_unsigned = dest_ir->is_unsigned,
                                     .needs_deref = false,
                                     .u.reg = {.r0 = target_r0, .r1 = target_r1}};
    return true;
  }

  /* Peephole: when the next non-NOP instruction is FUNCPARAMVAL using our dest
   * vreg as a 32-bit scalar argument, load directly into the parameter register
   * (R0+param_index).  Eliminates "ldr rT, ...; mov r0, rT" sequences
   * generated when a SELECT/LOAD result feeds a function call parameter.
   *
   * Only applies to simple 32-bit scalar arguments (param_index 0..3). */
  if (nq->op == TCCIR_OP_FUNCPARAMVAL && !needs_pair)
  {
    IROperand nq_src1 = tcc_ir_op_get_src1(ir, nq);
    int next_vr = irop_get_vreg(nq_src1);
    if (next_vr != dest_vr)
    {
      return false;
    }

    if (irop_op_is_lval(nq_src1))
    {
      return false;
    }

    IRLiveInterval *li = tcc_ir_get_live_interval(ir, dest_vr);
    if (!li)
    {
      return false;
    }

    if (li->start != (uint32_t)i)
    {
      return false;
    }

    /* Scan forward from j+1 to find the FUNCCALLVAL that consumes
     * this parameter.  The vreg must end at that CALL instruction. */
    int call_idx = -1;
    for (int k = j + 1; k < ir->next_instruction_index; k++)
    {
      TccIrOp kop = ir->compact_instructions[k].op;
      if (kop == TCCIR_OP_NOP || kop == TCCIR_OP_FUNCPARAMVAL)
        continue;
      if (kop == TCCIR_OP_FUNCCALLVAL || kop == TCCIR_OP_FUNCCALLVOID)
      {
        call_idx = k;
        break;
      }
      break; /* unexpected instruction — bail */
    }
    if (call_idx < 0 || li->end != (uint32_t)call_idx)
    {
      return false;
    }

    /* Decode parameter index from src2 (packed call_id | param_idx) */
    IROperand nq_src2 = tcc_ir_op_get_src2(ir, nq);
    uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, nq_src2);
    int param_idx = TCCIR_DECODE_PARAM_IDX(encoded);

    /* Only handle scalar register parameters (param 0..3 → R0..R3) */
    if (param_idx > 3)
      return false;

    int target_reg = param_idx; /* R0=0, R1=1, R2=2, R3=3 */

    /* Bail if another vreg already occupies target_reg during our live range.
     * Use end-1: the value must be in the target reg up to (but not including)
     * the call instruction where it is consumed; the call's return value (a
     * different vreg) may start at exactly li->end in the same register. */
    if (ir_reg_conflict(ir, target_reg, li->start, li->end > 0 ? li->end - 1 : 0, dest_vr))
      return false;

    /* Patch allocation */
    li->allocation.r0 = (uint16_t)target_reg;
    li->allocation.offset = 0;

    *out_mop_dest = (MachineOperand){.kind = MACH_OP_REG,
                                     .btype = irop_get_btype(*dest_ir),
                                     .vreg = dest_vr,
                                     .is_64bit = false,
                                     .is_unsigned = dest_ir->is_unsigned,
                                     .needs_deref = false,
                                     .u.reg = {.r0 = target_reg, .r1 = -1}};
    return true;
  }

  return false;
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

/* Find the next non-NOP instruction after `i`, for peepholes that fuse `i` with
 * a later partner (STRD/LDRD spill pairs, MLAL, spill block copy) and advance
 * the loop counter past the gap.  Returns -1 when there is none, OR when a
 * skipped NOP between `i` and the partner is a branch target: fusing `i` and
 * the partner into a single instruction is illegal if a branch can land on that
 * NOP, because the partner would then be reachable without executing `i` (and
 * vice-versa).  branch_target_reset[] catches targets that is_jump_target
 * misses at -O0. */
static int ir_codegen_next_nonnop_no_label(TCCIRState *ir, const uint8_t *branch_target_reset, int i)
{
  for (int j = i + 1; j < ir->next_instruction_index; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
    {
      if (q->is_jump_target || (branch_target_reset && branch_target_reset[j]))
        return -1;
      continue;
    }
    return j;
  }
  return -1;
}

/* True if idx is an ASSIGN/LOAD copy that materialises to a physical self-move
 * (src and dst the same hw register), which the assign/load codegen elides to
 * no code — so it can be scanned over between a flag-setting op and its
 * consuming branch without disturbing the flags.  Same identity-move idiom the
 * STRD-fusion scan uses (ir/codegen.c). */
static int ir_codegen_is_identity_move(TCCIRState *ir, int idx)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
    return 0;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (d.tag != IROP_TAG_VREG || d.is_lval)
    return 0;
  if (!(s.tag == IROP_TAG_VREG || (s.tag == IROP_TAG_STACKOFF && s.is_local)))
    return 0;
  if (s.is_llocal || s.is_sym)
    return 0;
  MachineOperand sm = machine_op_from_ir(ir, &s);
  MachineOperand dm = machine_op_from_ir(ir, &d);
  return sm.kind == MACH_OP_REG && dm.kind == MACH_OP_REG &&
         !sm.needs_deref && !dm.needs_deref &&
         sm.u.reg.r0 == dm.u.reg.r0 && sm.u.reg.r0 >= 0;
}

static int ir_codegen_count_vreg_uses(TCCIRState *ir, int32_t vreg)
{
  if (vreg < 0)
    return 0;

  int uses = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg)
      uses++;
    if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == vreg)
      uses++;
    if (q->op == TCCIR_OP_MLA && q->operand_base + 3 < ir->iroperand_pool_count &&
        irop_get_vreg(ir->iroperand_pool[q->operand_base + 3]) == vreg)
      uses++;
  }
  return uses;
}

/* True if `vreg` is referenced by any instruction OTHER than its def at
 * `def_idx` and a single consumer at `use_idx`.  Gates the MUL-const+ADD
 * fusion, which leaves the MUL result holding only the PARTIAL product (the
 * trailing <<b is folded into the ADD dest, not the MUL dest), so the fusion
 * is correct ONLY when the ADD at `use_idx` is the sole consumer — any other
 * reader would pick up the unscaled value.  We scan the IR directly, and over
 * ALL operand slots including the (lval) dest of a STORE, because the
 * live-interval `end` can under-approximate cross-block / loop-back-edge uses
 * and let the fusion fire on a strided struct store (base+idx*odd instead of
 * base+idx*C) — a misaligned store that smashes the heap. */
static int ir_codegen_vreg_used_elsewhere(TCCIRState *ir, int32_t vreg, int def_idx, int use_idx)
{
  if (vreg < 0)
    return 0;

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (i == def_idx || i == use_idx)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_dest && irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == vreg)
      return 1;
    if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg)
      return 1;
    if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == vreg)
      return 1;
    if (q->op == TCCIR_OP_MLA && q->operand_base + 3 < ir->iroperand_pool_count &&
        irop_get_vreg(ir->iroperand_pool[q->operand_base + 3]) == vreg)
      return 1;
  }
  return 0;
}

#ifdef TCC_REGALLOC_DEBUG
static void tcc_ir_debug_codegen_generate_entry(TCCIRState *ir)
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
#else
#define tcc_ir_debug_codegen_generate_entry(ir) ((void)0)
#endif

/* ============================================================================
 * Operand decode helper
 *
 * MopSpec encodes which MachineOperands to extract for a given IR instruction.
 * decode_mop_args() performs all machine_op_from_ir / peephole / fixup calls
 * once, returning a MopArgs struct.  Switch cases then forward to the
 * appropriate backend function with the pre-decoded args.
 *
 * dest modes: 0 = none, 1 = normal, 2 = with before-return peephole
 * src1 modes: 0 = none, 1 = normal, 2 = normal + subcomponent fixup
 * src2/scale/accum: 0 = none, 1 = extract
 * ============================================================================ */

typedef struct
{
  uint8_t dest;  /* 0/1/2: none / normal / peephole */
  uint8_t src1;  /* 0/1/2: none / normal / +subcomp fixup */
  uint8_t src2;  /* 0/1:   none / normal */
  uint8_t scale; /* 0/1:   none / from scale slot */
  uint8_t accum; /* 0/1:   none / from operand slot 3 (MLA) */
} MopSpec;

typedef struct
{
  MachineOperand dest, src1, src2, scale, accum;
} MopArgs;

static MopArgs decode_mop_args(TCCIRState *ir, IRQuadCompact *cq, const IROperand *src1_ir, const IROperand *src2_ir,
                               const IROperand *dest_ir, int i, MopSpec spec)
{
  MopArgs a;
  if (spec.dest)
  {
    if (spec.dest == 2 && ir_codegen_before_ret_peephole(ir, i, dest_ir, &a.dest))
      ; /* peephole patched the allocation — use synthesised MachineOperand */
    else
      a.dest = machine_op_from_ir(ir, dest_ir);
  }

  if (spec.src1 >= 1)
  {
    a.src1 = machine_op_from_ir(ir, src1_ir);
    if (spec.src1 == 2)
      mop_fixup_subcomponent(&a.src1, src1_ir, ir);
  }

  if (spec.src2)
    a.src2 = machine_op_from_ir(ir, src2_ir);
  if (spec.scale)
  {
    IROperand scale_ir = tcc_ir_op_get_scale(ir, cq);
    a.scale = machine_op_from_ir(ir, &scale_ir);
  }
  if (spec.accum)
  {
    IROperand accum_ir = ir->iroperand_pool[cq->operand_base + 3];
    a.accum = machine_op_from_ir(ir, &accum_ir);
  }
  return a;
}

/* ============================================================================
 * OPTION B: MopArgs cache helper
 * ============================================================================
 * During the dry-run, decoded dest/src1/src2 operands are stored in mop_cache.
 * During the real-run (when the cache is valid), they are read back directly,
 * skipping the interval-table lookups in decode_mop_args.
 *
 * Instructions that use scale or accum (indexed loads/stores, MLA) are rare;
 * those slots are not cached — they fall through to a full decode in both passes.
 * ============================================================================ */
static inline MopArgs ir_decode_cached(int is_dry_run, int use_mop_cache, MopArgs *mop_cache, int i, TCCIRState *ir,
                                       IRQuadCompact *cq, const IROperand *src1_ir, const IROperand *src2_ir,
                                       const IROperand *dest_ir, MopSpec spec)
{
  /* Real-run cache hit: replay the dry-run decode.  This must cover ALL
   * specs, including scale/accum (LOAD_INDEXED/STORE_INDEXED/MLA): the
   * decode-time peepholes (ir_codegen_before_ret_peephole) PATCH interval
   * allocations, and those patches persist from the dry-run into the
   * real-run.  A fresh real-run decode can therefore make a peephole
   * decision the dry-run did not — e.g. a LOAD_INDEXED whose following
   * ASSIGN's dest was only retargeted to a register later in the dry-run
   * fires the coalesce peephole in the real-run only, retargeting the load
   * while every cached consumer still reads the source's pre-patch register
   * (ptr fuzz seed 30436: `ldr r8, [...]` clobbered by stale-cache `mov r8, ip`). */
  if (!is_dry_run && use_mop_cache)
  {
    MopArgs cached = mop_cache[i];
    /* A peephole that skips an instruction (i = next_i; break) can fire in the
     * dry-run but not the real-run when its decision depends on pass-varying
     * state.  The STRD-spill fusion is one such case: it keys on the
     * SP-relative offset via fp_adjust_local_offset(), whose allocated_stack_size
     * term is 0 during the dry-run (the prologue that sets it runs only before
     * the real pass) but final during the real-run.  A large frame can therefore
     * make the dry-run fuse-and-skip instruction i while the real-run does not,
     * leaving mop_cache[i] never written (zero-initialised → all MACH_OP_NONE).
     * A genuinely decoded store/load always materialises dest or src1, so an
     * all-NONE pair marks an unpopulated slot: re-decode instead of returning
     * the stale sentinel (which would trip the MACH_OP_NONE codegen assert). */
    if (cached.dest.kind != MACH_OP_NONE || cached.src1.kind != MACH_OP_NONE)
      return cached;
  }

  MopArgs a = decode_mop_args(ir, cq, src1_ir, src2_ir, dest_ir, i, spec);

  /* Dry-run: store the decoded operands for real-run replay. */
  if (is_dry_run && mop_cache)
    mop_cache[i] = a;

  return a;
}

void tcc_ir_codegen_generate(TCCIRState *ir)
{
  IRQuadCompact *cq;

  tcc_ir_debug_codegen_generate_entry(ir);

  /* No rodata anchor until this function's discovery run asks for one; the
   * previous function's register must not leak into the scratch exclusions. */
  tcc_gen_machine_rodata_anchor_reset();

  TCC_DBG_BLOCK(cg_dump_ir_cg) { printf("==== POST-OPT IR AT CODEGEN ====\n"); tcc_ir_show(ir); fflush(stdout); }

  /* `&&label` stores label positions as IR indices BEFORE DCE/compaction.
   * max_orig_index and is_jump_target flags are maintained incrementally
   * during IR construction (tcc_ir_put / tcc_ir_backpatch), so no pre-pass
   * is needed here. */
  int max_orig_index = ir->max_orig_index;

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
  ir->codegen_return_jump_addrs = return_jump_addrs;
  int num_return_jumps = 0;

  /* Clear spill cache at function start */
  tcc_ir_spill_cache_clear(&ir->spill_cache);

  /* ============================================================================
   * PRE-SCAN: Compute maximum outgoing call stack argument size
   * ============================================================================
   * Scan all FUNCCALLVAL/FUNCCALLVOID instructions to find the maximum stack
   * argument area needed across all calls.  Pre-reserve the area in the
   * frame to avoid dynamic SP adjustments at each call site.
   */
  {
    int max_outgoing = 0;
    int call_count = 0;
    int has_softfloat_ops = 0;
    int max_nested_save_regs = 0;
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      const IRQuadCompact *q = &ir->compact_instructions[i];

      /* Detect soft-float operations that temporarily adjust SP.
       * These require FP to keep frame-relative offsets stable. */
      if (q->op >= TCCIR_OP_FADD && q->op <= TCCIR_OP_CVT_FTOI)
        has_softfloat_ops = 1;

      /* VLA functions dynamically move SP (sub sp, vla_size).  Without FP,
       * saved-SP references and local variable offsets break.  Force FP. */
      if (q->op == TCCIR_OP_VLA_ALLOC)
      {
        tcc_state->need_frame_pointer = 1;
        tcc_state->func_dynamic_sp = 1;
      }

      if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      call_count++;
      const IROperand call_id_op = tcc_ir_get_src2(ir, i);
      if (irop_is_none(call_id_op))
        continue;

      const int call_id = TCCIR_DECODE_CALL_ID((uint32_t)call_id_op.u.imm32);
      const int argc_hint = TCCIR_DECODE_CALL_ARGC((uint32_t)call_id_op.u.imm32);

      /* Compute ABI layout to determine stack arg size (no MOP allocation). */
      TCCAbiCallLayout layout;
      memset(&layout, 0, sizeof(layout));
      TCCAbiArgLoc inline_locs[16];
      layout.locs = inline_locs;
      layout.capacity = 16;

      TCCAbiArgLoc *heap_locs = NULL;
      if (argc_hint > 16)
      {
        heap_locs = tcc_mallocz(sizeof(TCCAbiArgLoc) * argc_hint);
        layout.locs = heap_locs;
        layout.capacity = argc_hint;
      }

      int argc = thumb_build_call_layout_from_ir(ir, i, call_id, argc_hint, &layout, NULL, NULL);
      int stack = (argc > 0) ? (int)layout.stack_size : 0;
      stack = (stack + 7) & ~7; /* 8-byte align (AAPCS) */
      if (stack > max_outgoing)
        max_outgoing = stack;

      /* Compute actual nested-call save needs using liveness data.
       * Only R0-R3 that hold values live BEFORE argument setup AND those
       * values survive past the call actually need saving.
       *
       * Check liveness at the first FUNCPARAM for this call (before arg
       * setup clobbers R0-R3).  If R0-R3 aren't live there, the call
       * doesn't need nested register saves.
       *
       * Checking at i+1 (after call) is wrong: a new definition at i+1
       * (e.g., R0 <-- #34) makes R0 appear "live" even though it's a fresh
       * value, not one that needs preserving across the call. */
      {
        uint32_t reg_arg_mask = 0;
        for (int a = 0; a < argc; a++)
        {
          const TCCAbiArgLoc *al = &layout.locs[a];
          if (al->kind == TCC_ABI_LOC_REG || al->kind == TCC_ABI_LOC_REG_STACK)
          {
            for (int w = 0; w < al->reg_count; w++)
              reg_arg_mask |= (1u << (al->reg_base + w));
          }
        }
        /* Find the first FUNCPARAM for this call by scanning backward.
         * Liveness at that point reflects the pre-arg-setup state. */
        uint32_t need_save = reg_arg_mask & 0x0F;
        if (!ir->ls.live_regs_by_instruction)
        {
          /* No liveness table means the register allocator assigned no physical
           * registers — all values are materialized on-the-fly.  Nothing can be
           * live across calls, so no saves are needed. */
          need_save = 0;
        }
        else
        {
          int first_param = i; /* fallback to call instruction */
          /* Scan backward for the earliest FUNCPARAM of this call.
           * Param-value computation may sit between FUNCPARAMs (e.g. PARAM0,
           * then some ASSIGNs setting up R1, then PARAM1), so do NOT stop at
           * non-PARAM instructions — only stop at the previous CALL or at
           * function entry. */
          for (int k = i - 1; k >= 0; k--)
          {
            const IRQuadCompact *pk = &ir->compact_instructions[k];
            if (pk->op == TCCIR_OP_NOP)
              continue;
            if (pk->op == TCCIR_OP_FUNCCALLVAL || pk->op == TCCIR_OP_FUNCCALLVOID)
              break; /* hit the previous call — done */
            if (pk->op == TCCIR_OP_FUNCPARAMVAL)
            {
              const IROperand param_src2 = tcc_ir_get_src2(ir, k);
              if (!irop_is_none(param_src2))
              {
                int param_call_id = TCCIR_DECODE_CALL_ID((uint32_t)param_src2.u.imm32);
                if (param_call_id == call_id)
                  first_param = k;
              }
            }
            /* Non-PARAM, non-CALL: keep scanning past arg-value computation. */
          }
          /* A register needs saving across this call iff some interval holds
           * a value in it BEFORE the call setup begins AND that value is still
           * needed AFTER the call returns.  Checking live_before alone falsely
           * counts arg-passing intervals of the *previous* call (their ranges
           * end exactly at that CALL, which is often first_param - 1).
           * Intersecting with live_after_call removes them: their intervals
           * do not extend past the previous call.  Cross-call live values, by
           * definition, are live at both points. */
          if (first_param == 0)
          {
            need_save = 0;
          }
          else if (first_param - 1 < ir->ls.live_regs_by_instruction_size)
          {
            uint32_t live_before = ir->ls.live_regs_by_instruction[first_param - 1];
            uint32_t live_after_call = (i + 1 < ir->ls.live_regs_by_instruction_size)
                                           ? ir->ls.live_regs_by_instruction[i + 1]
                                           : 0;
            need_save &= live_before & live_after_call;
          }
          else
          {
            need_save = 0;
          }
        }
        const int save_count = __builtin_popcount(need_save);
        if (save_count > max_nested_save_regs)
          max_nested_save_regs = save_count;
      }

      if (heap_locs)
        tcc_free(heap_locs);
      if (layout.locs != inline_locs && layout.locs)
        tcc_free(layout.locs);
    }
    ir->call_outgoing_size = max_outgoing;

    /* Disable tail-call optimization if the call needs stack arguments or if
     * text_and_data_separation requires R9 save/restore around the call.
     * With stack args, the pre-reserved outgoing area would need to be set up
     * before the branch, complicating frame teardown. */
    if (ir->tail_call_only && (max_outgoing > 0 || tcc_state->text_and_data_separation))
    {
      ir->tail_call_only = 0;
      ir->leaffunc = 0;
    }

    /* Reserve nested-call register save area for functions with multiple calls.
     * Size based on actual max R0-R3 usage across calls (+ R9 if needed). */
    if (call_count > 1 && max_nested_save_regs > 0)
    {
      int save_regs = max_nested_save_regs;
      if (tcc_state->text_and_data_separation)
        save_regs++; /* R9 */
      ir->call_nested_save_size = save_regs * 4;
    }
    else if (call_count >= 1 && tcc_state->text_and_data_separation)
    {
      ir->call_nested_save_size = 4; /* R9 only */
    }
    else
    {
      ir->call_nested_save_size = 0;
    }

    /* Soft-float helpers temporarily lower SP (sub sp, #N ... add sp, #N)
     * to save intermediate values.  This breaks SP-relative local offsets
     * when FP is omitted.  Force FP for functions with soft-float ops. */
    if (has_softfloat_ops)
      tcc_state->need_frame_pointer = 1;

    if (ir->has_static_chain)
      tcc_state->need_frame_pointer = 1;

    /* A stack-passed parameter used to force FP here whenever the function
     * also made a call.  It does not need to: the MACH_OP_PARAM_STACK path
     * adds offset_to_args, and without FP the SP-relative form folds in the
     * local frame too, so an incoming argument is addressable either way.
     * That rule was the source of essentially every frame pointer we emitted
     * -- instrumenting each need_frame_pointer site over the compiler's own
     * TUs put it at 19/4/10/9/1 against 1 for soft-float and 0 for VLA and
     * static-chain -- and it cost twice: locals below FP need the wide
     * negative-offset encodings (a positive [sp,#imm] with a low register is
     * 16-bit), and R7 stayed out of the allocator, forcing extra spills.
     * Dropping it took the compiler's .text from 1,559,816 to 1,485,760. */
  }

  /* Reserve nested-call save area above the outgoing area. */
  if (ir->call_nested_save_size > 0)
  {
    loc -= ir->call_nested_save_size;
    ir->call_nested_save_base = loc;
  }

  /* Reserve outgoing call stack args area at the very bottom of the frame.
   * This ensures prepared-call stack args are at call-time SP.
   */
  if (ir->call_outgoing_size > 0)
  {
    loc -= ir->call_outgoing_size;
    ir->call_outgoing_base = loc;
  }

  ir->scratch_save_size = 0;
  ir->scratch_save_base = 0;

  int stack_size = (-loc + 7) & ~7; // align to 8 bytes

  /* Disable tail-call if the function needs any stack frame or frame pointer.
   * Tail-call tears down the frame before branching, but arguments to the tail
   * call may reference stack-relative addresses (struct copies, spilled values)
   * that would become invalid after the teardown. */
  if (ir->tail_call_only &&
      (stack_size > 0 || tcc_state->need_frame_pointer || tcc_state->force_frame_pointer))
  {
    ir->tail_call_only = 0;
    ir->leaffunc = 0;
  }

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
  else
  {
    /* A PARENT that calls its nested functions writes R10 at the call site
     * (SET_CHAIN / INIT_CHAIN_SLOT pass the chain = its own FP).  R10 is
     * AAPCS callee-saved, so the parent's own caller may keep a live value
     * there — tccasm's asm_instr held nb_labels in R10 while
     * parse_asm_operands called ITS nested helper, and every asm inside a
     * nested function died with "invalid asm label count".  Put R10 in the
     * prologue save mask whenever the body emits a chain write. */
    for (int ci = 0; ci < ir->next_instruction_index; ci++)
    {
      int cop = ir->compact_instructions[ci].op;
      if (cop == TCCIR_OP_SET_CHAIN || cop == TCCIR_OP_INIT_CHAIN_SLOT)
      {
        ir->emits_set_chain = 1;
        extra_prologue_regs |= (1 << architecture_config.static_chain_reg);
        break;
      }
    }
  }

  /* Phase-3 per-instruction scratch constraint recording.
   * Allocated once per function; indexed by instruction index.
   * dry_insn_scratch[i] = number of mach_alloc_scratch() calls at instruction i.
   * dry_insn_saves[i]   = bitmask of registers that would be PUSH'd at instruction i.
   * Both arrays are declared before #if so they are visible in both passes. */
  int *dry_insn_scratch = tcc_mallocz(ir->next_instruction_index * sizeof(int));
  uint16_t *dry_insn_saves = tcc_mallocz(ir->next_instruction_index * sizeof(uint16_t));
  ir->codegen_dry_insn_scratch = dry_insn_scratch;
  ir->codegen_dry_insn_saves = dry_insn_saves;

  /* Vregs the phase-3 fixup demoted from a register to memory (see
   * try_demote_scratch_conflict).  Their stack slots are carved out of the
   * frame together with the scratch save area, after the dry run settles. */
  int demoted_vregs[4];
  int demoted_count = 0;

  /* ============================================================================
   * OPTION A: Skip dry-run for scratch-conflict-free functions
   * ============================================================================
   * ARM has 13 allocatable integer registers (r0-r12) and 16 single-precision
   * VFP registers (s0-s15). Scratch needs at most 2 of each simultaneously.
   * If enough registers are provably free at every program point, no scratch
   * push/pop can occur, so the dry-run produces no useful information.
   *
   * When skipping:
   *   - dry_insn_scratch[] / dry_insn_saves[] stay zero (tcc_mallocz) — correct.
   *   - Phase-3 fixup is a no-op (all-zero dry_insn_saves).
   *   - LR: no scratch push means no surprise LR push; leaffunc already correct.
   *   - Branch optimizer falls back to 32-bit encodings for all branches
   *     (2 bytes wasted per branch; acceptable tradeoff).
   * ============================================================================ */
  /* The dry run used to have one consumer — scratch accounting — so it was
   * skipped whenever enough registers were provably free that no scratch push
   * could occur.  Branch-encoding selection is a second consumer: without the
   * dry pass's address map, branch_opt_analyze never runs and every branch
   * keeps its 32-bit encoding, costing 2 bytes each.  So any function with a
   * forward branch used to run the dry pass too.
   *
   * That gate was written against a host measurement — "no measurable
   * compile-time saving (the dry passes emit no bytes)" — and the device says
   * otherwise, loudly.  On the RP2350 a compile is bound by instruction
   * fetches through a 16 KiB XIP cache, not by the bytes it writes, so a walk
   * that emits nothing still pays the whole miss stream: PASS_TIME over the
   * suite puts cg:dry at ~39% of all pass time, and *more* per call than the
   * real emit walk, because the forward-branch gate leaves it running on
   * exactly the complex functions.  Nearly every real function has a forward
   * branch (any if/while/for/&&/||), so this one term is what keeps the second
   * walk alive.
   *
   * At -O0 that is the same trade already taken for the rehearsal walk (−20%
   * codegen-heavy compile against +2.6% object size): -O0 has chosen compile
   * speed over code quality, and its objects are throwaway — the rootfs and
   * the self-host build are -O2, which keeps both walks and both peepholes.
   * TCC_KEEP_FWD_DRY=1 forces the old behaviour, so one firmware carries both
   * arms of the A/B. */
  /* Only the branch-width term is dropped. Every scratch-related demotion
     below still routes its function through the dry pass, so the invariants
     the skip path relies on — no surprise LR push, an exactly-sized scratch
     area — are untouched.  When the term cannot matter, the scan that feeds it
     is dead work too: it decodes an operand per branch over the whole IR. */
  const int fwd_branch_may_force_dry = (tcc_state->optimize != 0 || cg_keep_fwd_dry());
  int has_forward_branch = 0;
  for (int bi = 0; fwd_branch_may_force_dry && bi < ir->next_instruction_index && !has_forward_branch; bi++)
  {
    IRQuadCompact *bq = &ir->compact_instructions[bi];
    if (bq->op != TCCIR_OP_JUMP && bq->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand bdest = tcc_ir_op_get_dest(ir, bq);
    if (irop_is_none(bdest))
      continue;
    if ((int)bdest.u.imm32 > bi)
      has_forward_branch = 1;
  }

  int can_skip_dry_run =
      !has_forward_branch &&
      __builtin_popcountll(ir->ls.dirty_registers) <= (unsigned)(tcc_state->registers_for_allocator - 2) &&
      __builtin_popcountll(ir->ls.dirty_float_registers) <= (unsigned)(tcc_state->float_registers_for_allocator - 2);

  if (can_skip_dry_run)
  {
    /* When FP is omitted and the dry run is skipped, get_scratch_reg_with_save()
     * has no reserved save area, and a PUSH fallback would move SP under
     * SP-relative addressing.  The old answer was a conservative 16-byte
     * reservation whenever a scratch-capable op or a stack param was present —
     * measured at 367 corpus functions carrying a fully-dead frame (the saves
     * almost never happen).  New answer: those functions run the dry-run
     * passes instead, which discover the ACTUAL max scratch depth and size the
     * area exactly (usually zero).  The dry passes emit no bytes and measured
     * no compile-time cost when forward-branch functions were routed through
     * them; straight-line scratch-free functions still take the fast path. */
    int has_stack_params = 0;
    for (int p = 0; p < ir->next_parameter; p++)
    {
      if (ir->parameters_live_intervals[p].incoming_reg0 < 0)
      {
        has_stack_params = 1;
        break;
      }
    }
    /* A nested-call save reservation on this path is sized purely from the
     * over-approximated static liveness; the dry run sizes it exactly and
     * usually to zero.  Demote those functions too. */
    if (!tcc_state->need_frame_pointer && !tcc_state->func_dynamic_sp &&
        ir->call_nested_save_size > 0)
      can_skip_dry_run = 0;

    if (can_skip_dry_run &&
        !tcc_state->need_frame_pointer && !tcc_state->force_frame_pointer && (stack_size > 0 || has_stack_params))
    {
      /* Scratch save area is a safety net for get_scratch_reg_with_save()
       * paths that PUSH/STR into the area when no free register is found.
       * The skip-dry-run path doesn't know max_scratch_depth, so it reserves
       * conservatively.  But a pre-scan over the IR can rule out the
       * scratch-requiring ops entirely for simple functions (e.g. integer
       * code with no FP / 64-bit / div / inline-asm), avoiding the dead
       * reservation. */
      int might_need_scratch = 0;
      int has_any_op = 0;
      for (int i = 0; i < ir->next_instruction_index; i++)
      {
        int op = ir->compact_instructions[i].op;
        if (op == TCCIR_OP_NOP)
          continue;
        has_any_op = 1;
        /* FP/double ops invoke soft-float helpers (or VFP) with multi-reg
         * scratch needs; 64-bit ints are emulated as pairs and may need
         * scratch for the high half; div/mod call helpers; block-copy and
         * VLA touch SP/memcpy; inline asm and indexed memory ops are
         * unconstrained.  Any of these forces the safety net. */
        if (op >= TCCIR_OP_FADD && op <= TCCIR_OP_CVT_FTOI) { might_need_scratch = 1; break; }
        switch (op)
        {
        case TCCIR_OP_DIV:
        case TCCIR_OP_UDIV:
        case TCCIR_OP_PDIV:
        case TCCIR_OP_UMOD:
        case TCCIR_OP_IMOD:
        case TCCIR_OP_UMULL:
        case TCCIR_OP_SMULL:
        case TCCIR_OP_BLOCK_COPY:
        case TCCIR_OP_VLA_ALLOC:
        case TCCIR_OP_VLA_SP_SAVE:
        case TCCIR_OP_VLA_SP_RESTORE:
        case TCCIR_OP_INLINE_ASM:
        case TCCIR_OP_ASM_INPUT:
        case TCCIR_OP_ASM_OUTPUT:
        case TCCIR_OP_SET_CHAIN:
        case TCCIR_OP_LOAD_INDEXED:
        case TCCIR_OP_STORE_INDEXED:
        case TCCIR_OP_IJUMP:
        case TCCIR_OP_SWITCH_TABLE:
        case TCCIR_OP_SWITCH_LOAD:
          might_need_scratch = 1;
          break;
        default:
          break;
        }
        if (might_need_scratch)
          break;
        /* 64-bit operand on any op: emulated as a pair, may need scratch. */
        IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[i]);
        IROperand s1 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[i]);
        IROperand s2 = tcc_ir_op_get_src2(ir, &ir->compact_instructions[i]);
        if (irop_is_64bit(d) || irop_is_64bit(s1) || irop_is_64bit(s2))
        {
          might_need_scratch = 1;
          break;
        }
      }
      /* Calls with stack-passed args: the call-site setup may need scratch
       * to materialise the argument values into SP-relative slots. */
      if (!might_need_scratch && ir->call_outgoing_size > 0)
        might_need_scratch = 1;
      /* Incoming stack params: reads from [sp + offset_to_args] may collide
       * with live argument registers, forcing get_scratch_reg_with_save to
       * STR the register into the reserved area before the load.
       * Only relevant if some non-NOP op actually runs — a fully-NOP'd body
       * (useless_function_body) never loads those params. */
      if (!might_need_scratch && has_stack_params && has_any_op)
        might_need_scratch = 1;
      /* Large frames need scratch to materialise SP-relative offsets that
       * exceed the immediate-encoding range of Thumb-2 LDR/STR.  A simple
       * 124-byte threshold matches the LDR rt,[sp,#imm5*4] limit; above
       * that, individual access sites may need an extra register. */
      if (!might_need_scratch && stack_size > 124)
        might_need_scratch = 1;

      if (might_need_scratch)
        can_skip_dry_run = 0; /* dry run sizes the scratch area exactly */
    }

    if (can_skip_dry_run)
    {
      /* Mirror the dry-run finalisation: init branch opt (sets 32-bit fallback),
       * reset scratch/spill/fp state, then emit prologue immediately. */
      tcc_gen_machine_branch_opt_init();
      tcc_gen_machine_reset_scratch_state();
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      tcc_ir_opt_fp_cache_clear(ir);
      /* Pre-patch allocations for FUNCPARAMVAL fusion, then trim ghost
       * callee-saved registers from dirty_registers before prologue. */
      ir_codegen_pre_patch_funcparam_allocations(ir);
      ir_codegen_recompute_dirty_from_allocations(ir);
      if (!ir->naked)
        tcc_gen_machine_prolog(ir->leaffunc, ir->ls.dirty_registers, stack_size, extra_prologue_regs);
      if (!ir->naked)
        tcc_debug_prolog_epilog(tcc_state, 0);
    }
  }

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
   * When can_skip_dry_run: pass 0 is skipped entirely, prologue already emitted.
   * ============================================================================ */
  /* Option B: allocate per-instruction MopArgs cache for the dry-run.
   * Not used when the dry-run is skipped (can_skip_dry_run). */
  /* Zero-initialised: an unwritten slot reads back as all-MACH_OP_NONE, which
   * ir_decode_cached() treats as "not populated in the dry-run" and re-decodes
   * (see the cache-hit path there). */
  MopArgs *mop_cache = (!can_skip_dry_run && ir->next_instruction_index > 0)
                           ? tcc_mallocz(ir->next_instruction_index * sizeof(MopArgs))
                           : NULL;
  ir->codegen_mop_cache = mop_cache;
  int use_mop_cache = 0;

  const int pass_start = can_skip_dry_run ? 2 : 0;
  uint32_t *cbz_dry_mapping = NULL;
  uint16_t *dry_pool_entries = NULL;
  if (ir->next_instruction_index > 0)
  {
    dry_pool_entries = tcc_mallocz(ir->next_instruction_index * sizeof(uint16_t));
    ir->codegen_dry_pool_entries = dry_pool_entries;
  }

  /* Branch-target reset map for the materialisation cache (imm_cache).
   *
   * imm_cache persists a register's cached constant / symbol address across
   * straight-line IR boundaries (dead registers keep their value).  This is
   * only sound when control reaches the instruction linearly: at a control-flow
   * merge an alternate predecessor may have clobbered the register.  The shared
   * `is_jump_target` flag covers most merges, but at -O0 backward (loop) branch
   * targets are not always flagged, so cache a complete target set here and
   * reset at those points too.  Kept local to codegen so the wider
   * `is_jump_target` semantics (and the peephole fusions keyed on it) are
   * untouched.  Mirrors the target enumeration in tcc_ir_codegen_backpatch_jumps. */
  uint8_t *branch_target_reset = NULL;
  if (ir->next_instruction_index > 0)
  {
    branch_target_reset = tcc_mallocz((size_t)ir->next_instruction_index);
    ir->codegen_branch_target_reset = branch_target_reset;
    int has_indirect_jump = 0;
    for (int bi = 0; bi < ir->next_instruction_index; bi++)
    {
      IRQuadCompact *bq = &ir->compact_instructions[bi];
      if (bq->op == TCCIR_OP_JUMP || bq->op == TCCIR_OP_JUMPIF)
      {
        IROperand bdest = tcc_ir_op_get_dest(ir, bq);
        int btgt = irop_is_none(bdest) ? -1 : (int)bdest.u.imm32;
        if (btgt >= 0 && btgt < ir->next_instruction_index)
          branch_target_reset[btgt] = 1;
      }
      else if (bq->op == TCCIR_OP_IJUMP)
      {
        /* Computed goto: lands on an address-taken label that is not a static
         * JUMP target and cannot be cheaply enumerated from the register-
         * indirect jump.  Conservatively disable cross-boundary cache
         * persistence for the whole function (computed goto is rare). */
        has_indirect_jump = 1;
      }
    }
    /* Switch-table targets (data-driven jumps). */
    for (int st = 0; st < ir->num_switch_tables; st++)
    {
      TCCIRSwitchTable *tbl = &ir->switch_tables[st];
      for (int je = 0; je < tbl->num_entries; je++)
      {
        int btgt = tbl->targets[je];
        if (btgt >= 0 && btgt < ir->next_instruction_index)
          branch_target_reset[btgt] = 1;
      }
    }
    if (has_indirect_jump)
      memset(branch_target_reset, 1, (size_t)ir->next_instruction_index);
  }

  /* Pass 0 discovery (dry), pass 1 rehearsal (dry), pass 2 real.
   *
   * Pass 0 is what it always was: it discovers scratch pushes, LR usage and the
   * scratch save area, and its finalisation reassigns registers and resizes the
   * frame — so the code pass 0 lays out is NOT the code the real pass emits.
   *
   * Pass 1 reruns the same loop AFTER all of that has settled, from the same
   * `ind` the real pass starts at and with the same register assignments, so
   * its address map is a faithful model of the real layout. */
  /* -O0 skips the rehearsal walk. It exists to model the final layout for
     CBZ and forward-branch narrowing — both size peepholes — and measured on
     the RP2350 the third walk costs ~20% of codegen-heavy compile time
     against +2.6% object size: the wrong trade at the optimization level
     that already chose compile speed over code quality. -O1 and above keep
     all three walks and both peepholes. Skipping nulls the layout models the
     rehearsal alone can produce soundly (pass 0's layout precedes register
     reassignment and frame resize), so the peepholes fall back to their
     conservative encodings via their existing NULL guards.
     TCC_NO_REHEARSAL=1 forces the skip at any level (the measurement knob
     this decision was priced with). */
  int cg_skip_rehearsal = tcc_state->optimize == 0 || cg_no_rehearsal();
  TCCPassTimer cg_pt = {0};
  for (int pass = pass_start; pass < 3; pass++)
  {
    if (pass == 1 && cg_skip_rehearsal)
      continue;
    if (pass == 2 && cg_skip_rehearsal)
    {
      if (cbz_dry_mapping)
      {
        tcc_free(cbz_dry_mapping);
        cbz_dry_mapping = NULL;
      }
      ir->codegen_cbz_dry_mapping = NULL;
      ir->codegen_dry_pool_entries = NULL; /* buffer itself freed at exit */
    }
    /* PASS_TIME split of the codegen passes; begin closes the previous
       iteration's frame, the loop exit below closes the last one */
    if (cg_pt.active)
      tcc_pass_timing_end(&cg_pt, -1);
    tcc_pass_timing_begin(&cg_pt, pass == 0 ? "cg:dry" : pass == 1 ? "cg:rehearsal" : "cg:emit");
    const int is_dry_run = (pass < 2);
    const int is_rehearsal = (pass == 1);
    int codegen_skip_cmp = -1;
    int codegen_skip_select = -1; /* SUBS+IT peephole: skip this SELECT (CMP already emitted SUBS+IT+MOVNE in its slot). */
    int codegen_cbz_reg = -1;    /* pending CBZ: physical register for compare */
    int codegen_cbz_nonzero = 0; /* pending CBZ: 0=CBZ (EQ), 1=CBNZ (NE) */
    /* CBZ/CBNZ peephole: fuse `CMP rN,#0; JUMPIF EQ/NE` into a single 16-bit
     * CBZ/CBNZ.
     *
     * This was disabled for a long time because both distance estimators it
     * had were unsound.  CBZ is forward-only with a 0..126 byte range and the
     * peephole commits the 2-byte encoding irrevocably, while the target is not
     * emitted yet; when the real offset did not fit, th_patch_call() had no way
     * to widen it in place and aborted.  The old estimators were an
     * instructions-times-ten guess, and distances from the *discovery* dry run,
     * which diverge from the real layout (that pass reassigns registers and
     * resizes the frame afterwards).
     *
     * The rehearsal pass fixes exactly that: it lays the function out the way
     * the real pass will, so tcc_gen_machine_cbz_forward_ok() can bound the real
     * offset from both sides and reject anything a pool flush could disturb.
     * That is the "proper branch relaxation" the old comment asked for. */
    const int cbz_enabled = !is_dry_run && cbz_dry_mapping != NULL;

    /* ---- Pass-specific initialisation ---- */
    if (is_dry_run)
    {
      tcc_gen_machine_dry_run_init();
      tcc_gen_machine_branch_opt_init();
      tcc_gen_machine_dry_run_start();
      tcc_gen_machine_dry_run_set_rehearsal(is_rehearsal);
      tcc_gen_machine_reset_scratch_state();
      tcc_ir_spill_cache_clear(&ir->spill_cache);
    }

    /* Save state before dry-run so we can restore for real-run. */
    int saved_ind = ind;
    int saved_codegen_idx = ir->codegen_instruction_idx;
    int saved_loc = loc;
    int saved_call_outgoing_base = ir->call_outgoing_base;
    int saved_call_nested_save_base = ir->call_nested_save_base;

    /* ---- Instruction loop ---- */
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      cq = &ir->compact_instructions[i];

      /* Default: no extra scratch constraints for this instruction. */
      ir->codegen_materialize_scratch_flags = 0;

      /* At jump targets, flags from a prior CMP are not guaranteed live.
       * The STR→LDR peephole tracker is also invalidated, since a branch can
       * reach this IR op from a path where the prior STR did not execute. */
      if (cq->is_jump_target)
      {
        ir->codegen_flags_live = 0;
        ir->spill_cache.last_emit_kind = 0;
      }

      /* Track current instruction for scratch register allocation */
      ir->codegen_instruction_idx = i;

      /* Debug tracking: update current op for ot_check failure reporting */
      g_debug_current_op = (int)cq->op;

      ir_to_code_mapping[i] = ind;

      /* Snapshot pool pressure for can_narrow_forward_branch. */
      if (is_rehearsal && dry_pool_entries)
      {
        int en = tcc_gen_machine_pool_entries_total();
        dry_pool_entries[i] = (uint16_t)(en > 0xFFFF ? 0xFFFF : en);
      }

      /* Reset the STR→LDR memory-reload cache at every IR instruction
       * boundary (it tracks memory state, which an aliasing store on a
       * jumped-from path could invalidate without an emit the tracker sees).
       *
       * The MOV-equivalence (GPR value) cache, by contrast, stays sound
       * across straight-line IR-op boundaries: every emitted instruction
       * updates it (invalidating its dest reg, with calls/unknown opcodes
       * forcing a full reset), so register equivalences only become invalid
       * at a real control-flow merge.  Reset it only at jump targets; this
       * lets cross-IR `mov` chains — e.g. a soft-float double result copied
       * to its callee-saved home pair and then back to the next call's
       * argument pair — coalesce away. */
      tcc_gen_machine_strldr_cache_reset();
      /* Volatile slot access: drop the store→reload spill-cache peephole so
       * each volatile load reaches memory instead of reusing a just-stored
       * register (the peephole is otherwise only cleared at jump targets). */
      if (ir_codegen_op_touches_volatile(ir, cq))
        tcc_ir_spill_cache_clear(&ir->spill_cache);
      /* Like imm_cache below, the GPR-equivalence cache must also drop at
       * backward (loop) branch targets that is_jump_target misses at -O0:
       * an equivalence recorded before the loop (e.g. the prologue's
       * `mov r4, r0` param save) is not re-established on the back edge,
       * and eliding a call-argument `mov r0, r4` on that basis passes
       * garbage from the previous iteration (gcc_execute/990128-1 stored
       * through such a garbage pointer into the kernel vector table). */
      if (cq->is_jump_target || (branch_target_reset && branch_target_reset[i]))
        tcc_gen_machine_mov_equiv_reset();

      /* Invalidate imm_cache for registers assigned to live vregs.
       * Free (dead) registers retain cached constants across IR boundaries.
       * Full reset at jump targets / calls where control flow is non-linear. */
      if (cq->is_jump_target || (branch_target_reset && branch_target_reset[i]) ||
          cq->op == TCCIR_OP_FUNCCALLVAL || cq->op == TCCIR_OP_FUNCCALLVOID)
        tcc_gen_machine_imm_cache_reset();
      else if (ir->ls.live_regs_by_instruction &&
               i < ir->ls.live_regs_by_instruction_size)
        tcc_gen_machine_imm_cache_invalidate_live(ir->ls.live_regs_by_instruction[i]);

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

#define DECODE(...)                                                                                                    \
  ir_decode_cached(is_dry_run, use_mop_cache, mop_cache, i, ir, cq, &src1_ir, &src2_ir, &dest_ir,                      \
                   (MopSpec){__VA_ARGS__})
#define SCRATCH_WRAP(call)                                                                                             \
  do                                                                                                                   \
  {                                                                                                                    \
    tcc_gen_machine_insn_scratch_reset();                                                                              \
    call;                                                                                                              \
    ir_codegen_track_scratch(is_dry_run && !is_rehearsal, i, cq->op, dry_insn_scratch, dry_insn_saves);                                 \
  } while (0)

      switch (cq->op)
      {
      case TCCIR_OP_MUL:
      case TCCIR_OP_DIV:
      case TCCIR_OP_UDIV:
      case TCCIR_OP_IMOD:
      case TCCIR_OP_UMOD:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);

        /* Peephole: MUL-by-const + ADD → fused shifted-add.
         * When MUL result feeds directly into ADD, fuse the trailing
         * shift into the ADD using ARM's flexible second operand. */
        if (cq->op == TCCIR_OP_MUL && !a.src1.is_64bit && !a.dest.is_64bit)
        {
          const MachineOperand *imm_op = NULL, *var_op = NULL;
          if (a.src2.kind == MACH_OP_IMM)
          {
            imm_op = &a.src2;
            var_op = &a.src1;
          }
          else if (a.src1.kind == MACH_OP_IMM)
          {
            imm_op = &a.src1;
            var_op = &a.src2;
          }
          if (imm_op)
          {
            int next_j = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
            /* The ADD may carry a barrel-shift side-table annotation
             * (ir->barrel_shifts[orig_index], set by tcc_ir_barrel_shift_fusion):
             * its src2 is then a value that must still be shifted when the ADD
             * executes.  The fused emission below bypasses the annotated path
             * entirely, silently dropping that shift, so skip the fusion. */
            if (next_j >= 0 && ir->compact_instructions[next_j].op == TCCIR_OP_ADD &&
                !ir->compact_instructions[next_j].is_jump_target &&
                !tcc_ir_barrel_shift_at(ir, &ir->compact_instructions[next_j]))
            {
              IRQuadCompact *nq = &ir->compact_instructions[next_j];
              IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
              IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
              IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
              MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_j, ir, nq, &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                           (MopSpec){.dest = 1, .src1 = 1, .src2 = 1});

              /* Identify which ADD operand is the MUL result and which is the base */
              MachineOperand *add_base = NULL;
              int mul_dest_vreg = a.dest.vreg;
              if (!b.src1.is_64bit && !b.src2.is_64bit && mul_dest_vreg >= 0)
              {
                if (b.src2.vreg == mul_dest_vreg && b.src2.kind == MACH_OP_REG && !b.src2.needs_deref)
                  add_base = &b.src1;
                else if (b.src1.vreg == mul_dest_vreg && b.src1.kind == MACH_OP_REG && !b.src1.needs_deref)
                  add_base = &b.src2;
              }

              /* Only safe when the ADD at next_j is the SOLE consumer of the MUL
               * result: the fused helper leaves mul_dest holding the PARTIAL
               * product (var*odd for a (2^a+1)*2^b or (2^a-1)*2^b constant), not
               * the full var*C — the trailing <<b is folded only into add_dest.
               * Any other use of the MUL result would then read an unscaled
               * value.  (This miscompiled tcc_pch_auto_add_entry:
               * auto_pch_entries[idx].pch_name/.disabled addresses came out as
               * base+idx*3 instead of base+idx*12, a wild misaligned store that
               * corrupted the heap -> deferred free() HardFault; the same shape
               * smashed cfg->blocks in the 02-08 self-host crashes.)  Scan the IR
               * directly rather than trust the live-interval `end`, which can
               * under-approximate cross-block / loop-back-edge uses and let the
               * fusion fire when mul_dest is in fact still live. */
              if (add_base && mul_dest_vreg >= 0)
              {
                if (ir_codegen_vreg_used_elsewhere(ir, mul_dest_vreg, i, next_j))
                  add_base = NULL;
              }

              if (add_base)
              {
                tcc_gen_machine_insn_scratch_reset();
                int fused = tcc_gen_machine_mul_const_add_fused_mop(*var_op, imm_op->u.imm.val, a.dest, *add_base,
                                                                    b.dest);
                if (fused)
                {
                  ir_codegen_track_scratch(is_dry_run && !is_rehearsal, i, cq->op, dry_insn_scratch, dry_insn_saves);
                  i = next_j;
                  break;
                }
              }
            }
          }
        }

        SCRATCH_WRAP(tcc_gen_machine_muldiv_mop(a.src1, a.src2, a.dest, cq->op));
        break;
      }
      case TCCIR_OP_TEST_ZERO:
      {
        /* CBZ/CBNZ peephole for TEST_ZERO: same as CMP #0 pattern */
        if (cbz_enabled)
        {
          MopArgs cbz_a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
          if (!cbz_a.src1.is_64bit && cbz_a.src1.kind == MACH_OP_REG && !cbz_a.src1.needs_deref &&
              cbz_a.src1.u.reg.r0 >= 0 && cbz_a.src1.u.reg.r0 <= 7)
          {
            int next_j = i + 1;
            while (next_j < ir->next_instruction_index && ir->compact_instructions[next_j].op == TCCIR_OP_NOP)
              next_j++;
            if (next_j < ir->next_instruction_index && ir->compact_instructions[next_j].op == TCCIR_OP_JUMPIF)
            {
              IROperand jc = tcc_ir_op_get_src1(ir, &ir->compact_instructions[next_j]);
              int ct = (int)irop_get_imm64_ex(ir, jc);
              if (ct == 0x94 || ct == 0x95)
              {
                IROperand jdest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[next_j]);
                int target_ir = irop_is_none(jdest) ? -1 : (int)jdest.u.imm32;
                if (target_ir > i && target_ir < (int)ir->ir_to_code_mapping_size)
                {
                  if (tcc_gen_machine_cbz_forward_ok(target_ir, i))
                  {
                    codegen_cbz_reg = cbz_a.src1.u.reg.r0;
                    codegen_cbz_nonzero = (ct == 0x95);
                    break;
                  }
                }
              }
            }
          }
        }
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        SCRATCH_WRAP(tcc_gen_machine_muldiv_mop(a.src1, a.src2, a.dest, cq->op));
        break;
      }
      case TCCIR_OP_MLA:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1, .accum = 1);
        if (TCC_LOG_LS) {
          IROperand accum_ir_dbg = ir->iroperand_pool[cq->operand_base + 3];
          int vr_dbg = irop_get_vreg(accum_ir_dbg);
          IRLiveInterval *li_dbg = (vr_dbg > 0 && tcc_ir_vreg_is_valid(ir, vr_dbg)) ? tcc_ir_vreg_live_interval(ir, vr_dbg) : (IRLiveInterval*)0;
          LOG_LS("MLA accum: vreg=0x%x type=%d pos=%d tag=%d alloc.r0=%d alloc.off=%d mop.kind=%d mop.off=%d",
                 vr_dbg, TCCIR_DECODE_VREG_TYPE(vr_dbg), TCCIR_DECODE_VREG_POSITION(vr_dbg),
                 irop_get_tag(accum_ir_dbg),
                 li_dbg ? li_dbg->allocation.r0 : -99,
                 li_dbg ? li_dbg->allocation.offset : -99,
                 a.accum.kind, a.accum.kind == MACH_OP_SPILL ? a.accum.u.spill.offset : -99);
        }
        if (a.dest.is_64bit)
        {
          SCRATCH_WRAP({
            int fused = tcc_gen_machine_mlal_accum_mop(a.src1, a.src2, a.accum, a.dest, !a.dest.is_unsigned);
            if (!fused)
              tcc_error("compiler_error: unable to lower 64-bit MLA");
          });
        }
        else
        {
          SCRATCH_WRAP(tcc_gen_machine_mla_mop(a.src1, a.src2, a.dest, a.accum));
        }
        break;
      }
      case TCCIR_OP_UMULL:
      case TCCIR_OP_SMULL:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);

        /* Peephole: (S/U)MULL feeding a single 64-bit ADD into the same
         * accumulator pair maps directly to (S/U)MLAL. */
        if (a.dest.vreg >= 0 && ir_codegen_count_vreg_uses(ir, a.dest.vreg) == 1)
        {
          int next_j = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          if (next_j >= 0 && ir->compact_instructions[next_j].op == TCCIR_OP_ADD &&
              !ir->compact_instructions[next_j].is_jump_target)
          {
            IRQuadCompact *nq = &ir->compact_instructions[next_j];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_j, ir, nq, &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 1, .src1 = 1, .src2 = 1});

            MachineOperand *accum = NULL;
            if (b.src1.vreg == a.dest.vreg)
              accum = &b.src2;
            else if (b.src2.vreg == a.dest.vreg)
              accum = &b.src1;

            if (accum && b.dest.is_64bit && accum->is_64bit)
            {
              tcc_gen_machine_insn_scratch_reset();
              int fused = tcc_gen_machine_mlal_accum_mop(a.src1, a.src2, *accum, b.dest, cq->op == TCCIR_OP_SMULL);
              if (fused)
              {
                ir_codegen_track_scratch(is_dry_run && !is_rehearsal, i, cq->op, dry_insn_scratch, dry_insn_saves);
                i = next_j;
                break;
              }
            }

            if (accum && accum->is_64bit && irop_get_vreg(n_dest_ir) >= 0)
            {
              int store_j = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, next_j);
              if (store_j >= 0 && ir->compact_instructions[store_j].op == TCCIR_OP_STORE &&
                  !ir->compact_instructions[store_j].is_jump_target)
              {
                IRQuadCompact *sq = &ir->compact_instructions[store_j];
                IROperand st_src_ir = tcc_ir_op_get_src1(ir, sq);
                IROperand st_dest_ir = tcc_ir_op_get_dest(ir, sq);
                if (irop_get_vreg(st_src_ir) == irop_get_vreg(n_dest_ir) &&
                    irop_get_vreg(st_dest_ir) == accum->vreg &&
                    ir_codegen_count_vreg_uses(ir, irop_get_vreg(n_dest_ir)) == 1)
                {
                  tcc_gen_machine_insn_scratch_reset();
                  int fused =
                      tcc_gen_machine_mlal_accum_mop(a.src1, a.src2, *accum, *accum, cq->op == TCCIR_OP_SMULL);
                  if (fused)
                  {
                    ir_codegen_track_scratch(is_dry_run && !is_rehearsal, i, cq->op, dry_insn_scratch, dry_insn_saves);
                    i = store_j;
                    break;
                  }
                }
              }
            }
          }
        }

        if (cq->op == TCCIR_OP_UMULL)
          SCRATCH_WRAP(tcc_gen_machine_umull_mop(a.src1, a.src2, a.dest));
        else
          SCRATCH_WRAP(tcc_gen_machine_smull_mop(a.src1, a.src2, a.dest));
        break;
      }
      case TCCIR_OP_ADD:
      case TCCIR_OP_SUB:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        /* Peephole: if next instruction is CMP #0 of the same dest vreg,
         * force flag-setting encoding for this ADD/SUB and skip the CMP.
         * ARM Thumb SUBS/ADDS sets Z flag which replaces CMP Rd, #0.
         * Don't NOP the CMP — use skip index so both dry/real runs agree. */
        if (i + 1 < ir->next_instruction_index)
        {
          IRQuadCompact *nq = &ir->compact_instructions[i + 1];
          if (nq->op == TCCIR_OP_CMP)
          {
            IROperand cmp_s1 = tcc_ir_op_get_src1(ir, nq);
            IROperand cmp_s2 = tcc_ir_op_get_src2(ir, nq);
            /* Only safe for EQ/NE conditions (Z flag only).  Scan over an
             * out-of-SSA phi copy that materialised to a physical self-move
             * (emits no code, preserves flags) so a rotated count-down latch
             * `subs; cmp #0; <coalesced copy>; bne` still fuses. */
            int next_jmpif_idx = i + 2;
            while (next_jmpif_idx < ir->next_instruction_index &&
                   (ir->compact_instructions[next_jmpif_idx].op == TCCIR_OP_NOP ||
                    ir_codegen_is_identity_move(ir, next_jmpif_idx)))
              next_jmpif_idx++;
            int cond_safe = 0;
            if (next_jmpif_idx < ir->next_instruction_index &&
                ir->compact_instructions[next_jmpif_idx].op == TCCIR_OP_JUMPIF)
            {
              IROperand jc = tcc_ir_op_get_src1(ir, &ir->compact_instructions[next_jmpif_idx]);
              int ct = (int)irop_get_imm64_ex(ir, jc);
              cond_safe = (ct == 0x94 || ct == 0x95); /* TOK_EQ or TOK_NE */
            }
            /* Block when CMP src1 is a TEMP used as a pointer dereference
             * (is_lval + TEMP type = *ptr, tests memory not the pointer).
             * Allow VAR operands with is_lval (load from stack = same value). */
            int cmp_is_ptr_deref = irop_op_is_lval(cmp_s1) &&
                                   TCCIR_DECODE_VREG_TYPE(irop_get_vreg(cmp_s1)) == TCCIR_VREG_TYPE_TEMP;
            /* 64-bit only: a flag-setting 64-bit SUB/ADD lowers to
             * `subs lo; sbc hi` (or adds/adc) where only the low-word op sets
             * flags — `sbc`/`adc` do not.  So Z reflects only the low word and
             * cannot replace a full-width `CMP Rd,#0` for an EQ/NE branch
             * (miscompile: 920501-6's `for(b=0,s=t; b++,(s>>=1)!=0;)` exited
             * after one iteration).  Keep the CMP, which the 64-bit EQ/NE
             * peephole below lowers correctly via cmp_eq64. */
            if (cond_safe && !cmp_is_ptr_deref &&
                !a.src1.is_64bit && !a.dest.is_64bit &&
                irop_is_immediate(cmp_s2) && irop_get_imm64_ex(ir, cmp_s2) == 0 &&
                irop_has_vreg(cmp_s1) &&
                irop_get_vreg(cmp_s1) == irop_get_vreg(dest_ir))
            {
              SCRATCH_WRAP(tcc_gen_machine_data_processing_mop_flags(a.src1, a.src2, a.dest, cq->op));
              codegen_skip_cmp = i + 1;
              ir->codegen_flags_live = 1;
              break;
            }
          }
        }
        /* Predicate-into-SELECT (if-conversion at codegen time): when this ALU
         * op feeds an else-identity SELECT with live CMP flags, emit it
         * predicated into the SELECT dest — `cmp; it <cond>; rsb.w` (gcc-parity
         * abs) — and skip the SELECT.  Pattern detection lives in
         * source/opt/flat/cfg/if_convert.c; the backend gates the actual
         * encoding via tcc_gen_machine_can_predicate_alu. */
        if (ir->codegen_flags_live && !a.dest.is_64bit)
        {
          int sj, scc;
          if (tcc_ir_ifconv_match_predicated_select(ir, i, &sj, &scc))
          {
            IROperand s_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[sj]);
            MachineOperand sdst = machine_op_from_ir(ir, &s_dest);
            if (tcc_gen_machine_can_predicate_alu(a.src1, a.src2, sdst, cq->op))
            {
              SCRATCH_WRAP(tcc_gen_machine_predicated_alu_mop(a.src1, a.src2, sdst, cq->op, scc));
              codegen_skip_select = sj;
              ir->codegen_flags_live = 0;
              break;
            }
          }
        }
        {
          uint32_t bs = tcc_ir_barrel_shift_at(ir, cq);
          SCRATCH_WRAP(tcc_gen_machine_data_processing_mop(a.src1, a.src2, a.dest, cq->op, bs));
        }
        break;
      }
      case TCCIR_OP_CMP:
        if (i == codegen_skip_cmp)
        {
          codegen_skip_cmp = -1;
          break;
        }
        /* CBZ/CBNZ peephole: CMP rN, #0 followed by JUMPIF EQ/NE.
         * Only in real pass with valid dry-run distance estimates. */
        if (cbz_enabled)
        {
          MopArgs cbz_a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
          if (cbz_a.src2.kind == MACH_OP_IMM && cbz_a.src2.u.imm.val == 0 && !cbz_a.src1.is_64bit &&
              cbz_a.src1.kind == MACH_OP_REG && !cbz_a.src1.needs_deref && cbz_a.src1.u.reg.r0 >= 0 &&
              cbz_a.src1.u.reg.r0 <= 7)
          {
            int next_j = i + 1;
            while (next_j < ir->next_instruction_index && ir->compact_instructions[next_j].op == TCCIR_OP_NOP)
              next_j++;
            if (next_j < ir->next_instruction_index && ir->compact_instructions[next_j].op == TCCIR_OP_JUMPIF)
            {
              IROperand jc = tcc_ir_op_get_src1(ir, &ir->compact_instructions[next_j]);
              int ct = (int)irop_get_imm64_ex(ir, jc);
              if (ct == 0x94 || ct == 0x95) /* TOK_EQ or TOK_NE */
              {
                IROperand jdest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[next_j]);
                int target_ir = irop_is_none(jdest) ? -1 : (int)jdest.u.imm32;
                if (target_ir > i && target_ir < (int)ir->ir_to_code_mapping_size)
                {
                  if (tcc_gen_machine_cbz_forward_ok(target_ir, i))
                  {
                    codegen_cbz_reg = cbz_a.src1.u.reg.r0;
                    codegen_cbz_nonzero = (ct == 0x95); /* NE → CBNZ */
                    break;                               /* skip emitting CMP */
                  }
                }
              }
            }
          }
        }
        /* 64-bit EQ/NE peephole: CMP pair followed by SETIF/JUMPIF/SELECT EQ/NE.
         * Use CMP+IT+CMPEQ instead of CMP+SBCS for correct Z flag. */
        {
          MopArgs eq_a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
          if (eq_a.src1.is_64bit)
          {
            /* Skip NOPs and flag-neutral register copies (ASSIGN lowers to
             * `mov`, which preserves flags) when searching for the condition
             * consumer.  After const-prop folds `CMP; SETIF; TEST_ZERO; JUMPIF`
             * into `CMP; JUMPIF`, phi-resolution ASSIGNs for loop-carried
             * variables get scheduled between the CMP and the JUMPIF; without
             * skipping them this peephole would miss the EQ/NE consumer and
             * fall back to the relational SBCS lowering, whose Z flag reflects
             * only the high word — wrong for a 64-bit equality test
             * (920501-6: `for(b=0,s=t; b++,(s>>=1)!=0;)` exited after one
             * iteration).  The relational path already relies on these ASSIGNs
             * preserving the CMP's flags up to the branch, so skipping them
             * here is consistent. */
            int next_j = i + 1;
            while (next_j < ir->next_instruction_index &&
                   (ir->compact_instructions[next_j].op == TCCIR_OP_NOP ||
                    ir->compact_instructions[next_j].op == TCCIR_OP_ASSIGN))
              next_j++;
            if (next_j < ir->next_instruction_index)
            {
              TccIrOp next_op = ir->compact_instructions[next_j].op;
              IROperand nc;
              int has_cond = 0;
              if (next_op == TCCIR_OP_SETIF || next_op == TCCIR_OP_JUMPIF)
              {
                nc = tcc_ir_op_get_src1(ir, &ir->compact_instructions[next_j]);
                has_cond = 1;
              }
              else if (next_op == TCCIR_OP_SELECT)
              {
                nc = tcc_ir_op_get_cond(ir, &ir->compact_instructions[next_j]);
                has_cond = 1;
              }
              if (has_cond)
              {
                int next_cond = (int)irop_get_imm64_ex(ir, nc);
                if (next_cond == TOK_EQ || next_cond == TOK_NE)
                {
                  SCRATCH_WRAP(tcc_gen_machine_cmp_eq64_mop(eq_a.src1, eq_a.src2));
                  break;
                }
              }
            }
          }
        }
        /* SUBS+IT peephole: CMP x, #K immediately followed by
         * `T <-- #1 SELECT #0` (cond=NE) or `T <-- #0 SELECT #1` (cond=EQ)
         * collapses cmp+ite+movne+moveq (4 instr) into subs+it+movne (3 instr).
         * The SUBS sets flags AND result in one shot: on EQ the result is 0
         * (matches the "else" arm), on NE the IT-MOVNE overwrites with 1. */
        {
          MopArgs subs_a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
          if (!subs_a.src1.is_64bit && subs_a.src2.kind == MACH_OP_IMM &&
              subs_a.src1.kind == MACH_OP_REG && !subs_a.src1.needs_deref) {
            int next_j = i + 1;
            while (next_j < ir->next_instruction_index &&
                   ir->compact_instructions[next_j].op == TCCIR_OP_NOP)
              next_j++;
            if (next_j < ir->next_instruction_index &&
                ir->compact_instructions[next_j].op == TCCIR_OP_SELECT) {
              IRQuadCompact *sq = &ir->compact_instructions[next_j];
              IROperand sel_s1 = tcc_ir_op_get_src1(ir, sq);
              IROperand sel_s2 = tcc_ir_op_get_src2(ir, sq);
              IROperand sel_cond = tcc_ir_op_get_cond(ir, sq);
              int sel_cc = (int)irop_get_imm64_ex(ir, sel_cond);
              int v1 = irop_is_immediate(sel_s1) ? (int)irop_get_imm64_ex(ir, sel_s1) : -1;
              int v2 = irop_is_immediate(sel_s2) ? (int)irop_get_imm64_ex(ir, sel_s2) : -1;
              int matches = (v1 == 1 && v2 == 0 && sel_cc == TOK_NE) ||
                            (v1 == 0 && v2 == 1 && sel_cc == TOK_EQ);
              if (matches) {
                IROperand sel_dest = tcc_ir_op_get_dest(ir, sq);
                MachineOperand sd = machine_op_from_ir(ir, &sel_dest);
                if (sd.kind == MACH_OP_REG && !sd.needs_deref && sd.u.reg.r0 >= 0) {
                  if (tcc_gen_machine_subs_eq_select_01(subs_a.src1, subs_a.src2, sd)) {
                    ir->codegen_flags_live = 0;
                    codegen_skip_select = next_j;
                    break; /* skip normal CMP emission */
                  }
                }
              }
            }
          }
        }
        /* fall through */
      case TCCIR_OP_SHL:
      case TCCIR_OP_SHR:
      case TCCIR_OP_SAR:
      case TCCIR_OP_ROR:
      case TCCIR_OP_OR:
      case TCCIR_OP_AND:
      case TCCIR_OP_XOR:
      case TCCIR_OP_ADC_GEN:
      case TCCIR_OP_ADC_USE:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        {
          uint32_t bs = tcc_ir_barrel_shift_at(ir, cq);
          /* For 64-bit shifts, pass dead-half annotations in bits 16-17 so the
           * emitter can skip the dead low/high word write. */
          if (cq->op == TCCIR_OP_SHL || cq->op == TCCIR_OP_SHR || cq->op == TCCIR_OP_SAR)
            bs |= (uint32_t)tcc_ir_shift64_dead_half_at(ir, cq) << 16;
          SCRATCH_WRAP(tcc_gen_machine_data_processing_mop(a.src1, a.src2, a.dest, cq->op, bs));
        }
        break;
      }
      case TCCIR_OP_UBFX:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        SCRATCH_WRAP(tcc_gen_machine_ubfx_mop(a.src1, a.src2, a.dest));
        break;
      }
      case TCCIR_OP_SBFX:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        SCRATCH_WRAP(tcc_gen_machine_sbfx_mop(a.src1, a.src2, a.dest));
        break;
      }
      case TCCIR_OP_BFI:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        uint32_t params = tcc_ir_bfi_params_at(ir, cq);
        SCRATCH_WRAP(tcc_gen_machine_bfi_mop(a.src1, a.src2, a.dest, params));
        break;
      }
      case TCCIR_OP_CLZ:
      case TCCIR_OP_RBIT:
      case TCCIR_OP_REV:
      case TCCIR_OP_REV16:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1);
        SCRATCH_WRAP(tcc_gen_machine_bitop1_mop(a.src1, a.dest, cq->op));
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
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        tcc_gen_machine_fp_mop(a.src1, a.src2, a.dest, cq->op, src1_ir.is_complex || dest_ir.is_complex);
        break;
      }
      case TCCIR_OP_LOAD:
      {
        MopArgs a = DECODE(.dest = 2, .src1 = 2);
        if (a.dest.kind == MACH_OP_NONE || a.src1.kind == MACH_OP_NONE)
          tcc_error("compiler_error: LOAD operand produced MACH_OP_NONE (i=%d dest_kind=%d src_kind=%d)", i,
                    a.dest.kind, a.src1.kind);

        /* Block copy peephole: consecutive LOAD-from-spill + STORE-to-spill pairs
         * with sequential offsets → single LDM/STM block copy.
         * Safety: all loads must use the same destination register, proving each
         * loaded value is dead after the store (just a temporary for the copy).
         * If different registers are used, the values are live past the copy. */
        if (a.dest.kind == MACH_OP_REG && !a.dest.needs_deref &&
            a.src1.kind == MACH_OP_SPILL && !a.src1.needs_deref && !a.src1.is_64bit &&
            (a.src1.btype == IROP_BTYPE_INT32 || a.src1.btype == IROP_BTYPE_FLOAT32) &&
            (a.src1.u.spill.offset & 3) == 0)
        {
          int first_load_reg = a.dest.u.reg.r0;
          int store_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          if (store_i >= 0 && ir->compact_instructions[store_i].op == TCCIR_OP_STORE &&
              !ir->compact_instructions[store_i].is_jump_target)
          {
            IRQuadCompact *sq = &ir->compact_instructions[store_i];
            IROperand s_src1 = tcc_ir_op_get_src1(ir, sq);
            IROperand s_src2 = tcc_ir_op_get_src2(ir, sq);
            IROperand s_dest = tcc_ir_op_get_dest(ir, sq);
            MopArgs sa = ir_decode_cached(is_dry_run, 0, NULL, store_i, ir, sq,
                                          &s_src1, &s_src2, &s_dest,
                                          (MopSpec){.dest = 1, .src1 = 2});

            if (sa.dest.kind == MACH_OP_SPILL && !sa.dest.needs_deref && !sa.src1.is_64bit &&
                sa.src1.kind == MACH_OP_REG && sa.src1.u.reg.r0 == first_load_reg &&
                (sa.dest.btype == IROP_BTYPE_INT32 || sa.dest.btype == IROP_BTYPE_FLOAT32) &&
                (sa.dest.u.spill.offset & 3) == 0)
            {
              int32_t src_base = a.src1.u.spill.offset;
              int32_t dst_base = sa.dest.u.spill.offset;
              int count = 1;
              int last_i = store_i;

              while (count < 32)
              {
                int next_load_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, last_i);
                if (next_load_i < 0 || ir->compact_instructions[next_load_i].op != TCCIR_OP_LOAD ||
                    ir->compact_instructions[next_load_i].is_jump_target)
                  break;

                IRQuadCompact *lq = &ir->compact_instructions[next_load_i];
                IROperand l_src1 = tcc_ir_op_get_src1(ir, lq);
                IROperand l_src2 = tcc_ir_op_get_src2(ir, lq);
                IROperand l_dest = tcc_ir_op_get_dest(ir, lq);
                MopArgs la = ir_decode_cached(is_dry_run, 0, NULL, next_load_i, ir, lq,
                                              &l_src1, &l_src2, &l_dest,
                                              (MopSpec){.dest = 1, .src1 = 2});

                if (la.src1.kind != MACH_OP_SPILL || la.src1.needs_deref || la.src1.is_64bit ||
                    la.src1.u.spill.offset != src_base + count * 4 ||
                    (la.src1.btype != IROP_BTYPE_INT32 && la.src1.btype != IROP_BTYPE_FLOAT32))
                  break;

                if (la.dest.kind != MACH_OP_REG || la.dest.u.reg.r0 != first_load_reg)
                  break;

                int next_store_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, next_load_i);
                if (next_store_i < 0 || ir->compact_instructions[next_store_i].op != TCCIR_OP_STORE ||
                    ir->compact_instructions[next_store_i].is_jump_target)
                  break;

                IRQuadCompact *sq2 = &ir->compact_instructions[next_store_i];
                IROperand s2_src1 = tcc_ir_op_get_src1(ir, sq2);
                IROperand s2_src2 = tcc_ir_op_get_src2(ir, sq2);
                IROperand s2_dest = tcc_ir_op_get_dest(ir, sq2);
                MopArgs sa2 = ir_decode_cached(is_dry_run, 0, NULL, next_store_i, ir, sq2,
                                               &s2_src1, &s2_src2, &s2_dest,
                                               (MopSpec){.dest = 1, .src1 = 2});

                if (sa2.dest.kind != MACH_OP_SPILL || sa2.dest.needs_deref || sa2.src1.is_64bit ||
                    sa2.dest.u.spill.offset != dst_base + count * 4 ||
                    sa2.src1.kind != MACH_OP_REG || sa2.src1.u.reg.r0 != first_load_reg ||
                    (sa2.dest.btype != IROP_BTYPE_INT32 && sa2.dest.btype != IROP_BTYPE_FLOAT32))
                  break;

                count++;
                last_i = next_store_i;
              }

              if (count >= 8)
              {
                SCRATCH_WRAP(tcc_gen_machine_spill_block_copy(src_base, dst_base, count));
                i = last_i;
                break;
              }
            }
          }
        }

        SCRATCH_WRAP(tcc_gen_machine_load_mop(a.src1, a.dest, cq->op));
        break;
      }
      case TCCIR_OP_STORE:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 2);
        if (a.dest.kind == MACH_OP_NONE || a.src1.kind == MACH_OP_NONE)
          tcc_error("compiler_error: STORE operand produced MACH_OP_NONE (i=%d dest_kind=%d src_kind=%d)", i,
                    a.dest.kind, a.src1.kind);

        /* STRD peephole: if this is a 32-bit store to a spill slot and the
         * very next non-NOP instruction is also a 32-bit store to an adjacent
         * (+4) spill slot, emit STRD for both and skip the second.
         *
         * The value operand (src1) must NOT be a deref: STORE's src1 can carry
         * needs_deref, meaning "dereference this pointer register to obtain the
         * value to store" (slot = *ptr).  Pairing such a store into STRD would
         * feed the address register straight to try_strd_spill as if it were
         * the value, silently dropping the required load. */
        if (a.dest.kind == MACH_OP_SPILL && !a.dest.needs_deref &&
            a.src1.kind == MACH_OP_REG && !a.src1.is_64bit && !a.src1.needs_deref &&
            (a.dest.btype == IROP_BTYPE_INT32 || a.dest.btype == IROP_BTYPE_FLOAT32) &&
            (a.dest.u.spill.offset & 3) == 0)
        {
          /* Find next non-NOP instruction */
          int next_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          /* is_jump_target misses some branch targets (see branch_target_reset);
           * consuming a branch-target store removes the label's only emission
           * point, so branches to it backpatch against code address 0. */
          if (next_i >= 0 && ir->compact_instructions[next_i].op == TCCIR_OP_STORE &&
              !ir->compact_instructions[next_i].is_jump_target &&
              !(branch_target_reset && branch_target_reset[next_i]))
          {
            /* Decode the next store's operands */
            IRQuadCompact *nq = &ir->compact_instructions[next_i];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, nq,
                                         &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 1, .src1 = 2});

            if (b.dest.kind == MACH_OP_SPILL && !b.dest.needs_deref &&
                b.src1.kind == MACH_OP_REG && !b.src1.is_64bit && !b.src1.needs_deref &&
                (b.dest.btype == IROP_BTYPE_INT32 || b.dest.btype == IROP_BTYPE_FLOAT32) &&
                (b.dest.u.spill.offset & 3) == 0)
            {
              int32_t off1 = a.dest.u.spill.offset;
              int32_t off2 = b.dest.u.spill.offset;
              int reg1 = a.src1.u.reg.r0;
              int reg2 = b.src1.u.reg.r0;

              if (reg1 != reg2 && off1 + 4 == off2)
              {
                if (tcc_gen_machine_try_strd_spill(reg1, off1, reg2, off2))
                {
                  /* Skip the next store — advance i past NOPs and the paired store */
                  i = next_i;
                  break;
                }
              }
              else if (reg1 != reg2 && off2 + 4 == off1)
              {
                if (tcc_gen_machine_try_strd_spill(reg2, off2, reg1, off1))
                {
                  i = next_i;
                  break;
                }
              }
            }
          }
        }

        /* STRD peephole (immediate-to-spill form): two consecutive stores of
         * immediate constants to adjacent spill slots → single STRD.
         * The helper materializes the constants into scratch registers. */
        if (a.dest.kind == MACH_OP_SPILL && !a.dest.needs_deref &&
            a.src1.kind == MACH_OP_IMM && !a.src1.is_64bit &&
            (a.dest.btype == IROP_BTYPE_INT32 || a.dest.btype == IROP_BTYPE_FLOAT32) &&
            (a.dest.u.spill.offset & 3) == 0)
        {
          int next_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          if (next_i >= 0 && ir->compact_instructions[next_i].op == TCCIR_OP_STORE &&
              !ir->compact_instructions[next_i].is_jump_target)
          {
            IRQuadCompact *nq = &ir->compact_instructions[next_i];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, nq,
                                         &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 1, .src1 = 2});

            if (b.dest.kind == MACH_OP_SPILL && !b.dest.needs_deref &&
                b.src1.kind == MACH_OP_IMM && !b.src1.is_64bit &&
                (b.dest.btype == IROP_BTYPE_INT32 || b.dest.btype == IROP_BTYPE_FLOAT32) &&
                (b.dest.u.spill.offset & 3) == 0)
            {
              int32_t off1 = a.dest.u.spill.offset;
              int32_t off2 = b.dest.u.spill.offset;
              int64_t val1 = a.src1.u.imm.val;
              int64_t val2 = b.src1.u.imm.val;
              int strd_ok = 0;

              if (off1 + 4 == off2)
              {
                SCRATCH_WRAP(strd_ok = tcc_gen_machine_try_strd_imm_spill(val1, val2, off1, off2));
              }
              else if (off2 + 4 == off1)
              {
                SCRATCH_WRAP(strd_ok = tcc_gen_machine_try_strd_imm_spill(val2, val1, off2, off1));
              }
              if (strd_ok)
              {
                i = next_i;
                break;
              }
            }
          }
        }

        /* STRD peephole (deref-through-vreg form): pair a plain STORE
         * through a register-deref destination (offset 0 implicit) with an
         * immediately-following STORE_INDEXED through the same base vreg
         * at offset +4.  Mirrors the spill-slot STRD peephole above; the
         * disp-fusion turns "ADD base+N; STORE *(...) <- v" into
         * STORE_INDEXED, but the off=0 store stays plain STORE — so the
         * existing STORE_INDEXED-only peephole misses the pair. */
        if (a.dest.kind == MACH_OP_REG && a.dest.needs_deref &&
            a.src1.kind == MACH_OP_REG && !a.src1.is_64bit && !a.src1.needs_deref &&
            !a.dest.underalign_hint && !a.src1.underalign_hint &&
            (a.dest.btype == IROP_BTYPE_INT32 || a.dest.btype == IROP_BTYPE_FLOAT32))
        {
          int next_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          /* is_jump_target misses some branch targets (see branch_target_reset);
           * consuming a branch-target store removes the label's only emission
           * point, so branches to it backpatch against code address 0. */
          if (next_i >= 0 && ir->compact_instructions[next_i].op == TCCIR_OP_STORE_INDEXED &&
              !ir->compact_instructions[next_i].is_jump_target &&
              !(branch_target_reset && branch_target_reset[next_i]))
          {
            IRQuadCompact *nq = &ir->compact_instructions[next_i];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, nq, &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 1, .src1 = 1, .src2 = 1, .scale = 1});

            /* src1 is the value being stored; a deref there (value = *ptr) would
             * feed the pointer register to try_strd_base as the value. */
            if (!b.src1.is_64bit && b.src1.kind == MACH_OP_REG && !b.src1.needs_deref &&
                b.scale.kind == MACH_OP_IMM && b.scale.u.imm.val == 0 &&
                b.src2.kind == MACH_OP_IMM &&
                b.dest.kind == MACH_OP_REG && !b.dest.needs_deref &&
                !b.dest.underalign_hint && !b.src1.underalign_hint &&
                (b.src1.btype == IROP_BTYPE_INT32 || b.src1.btype == IROP_BTYPE_FLOAT32) &&
                a.dest.u.reg.r0 == b.dest.u.reg.r0)
            {
              int reg1 = a.src1.u.reg.r0;
              int reg2 = b.src1.u.reg.r0;
              int base_reg = a.dest.u.reg.r0;
              int32_t off2 = (int32_t)b.src2.u.imm.val;

              if (off2 == 4)
              {
                if (tcc_gen_machine_try_strd_base(reg1, reg2, base_reg, 0))
                {
                  i = next_i;
                  break;
                }
              }
            }
          }
        }

        /* STRD peephole (deref-through-vreg form, IMM sources): pair a plain
         * STORE of an immediate through a register-deref destination (offset 0)
         * with an immediately-following STORE_INDEXED of an immediate through
         * the same base vreg at offset +4.  Mirrors the REG-source variant
         * above; here both values are constants materialised into scratch regs
         * before the paired store. */
        if (a.dest.kind == MACH_OP_REG && a.dest.needs_deref &&
            a.src1.kind == MACH_OP_IMM && !a.src1.is_64bit &&
            !a.dest.underalign_hint && !a.src1.underalign_hint &&
            (a.dest.btype == IROP_BTYPE_INT32 || a.dest.btype == IROP_BTYPE_FLOAT32))
        {
          int next_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          /* is_jump_target misses some branch targets (see branch_target_reset);
           * consuming a branch-target store removes the label's only emission
           * point, so branches to it backpatch against code address 0. */
          if (next_i >= 0 && ir->compact_instructions[next_i].op == TCCIR_OP_STORE_INDEXED &&
              !ir->compact_instructions[next_i].is_jump_target &&
              !(branch_target_reset && branch_target_reset[next_i]))
          {
            IRQuadCompact *nq = &ir->compact_instructions[next_i];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, nq, &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 1, .src1 = 1, .src2 = 1, .scale = 1});

            if (b.src1.kind == MACH_OP_IMM && !b.src1.is_64bit &&
                b.scale.kind == MACH_OP_IMM && b.scale.u.imm.val == 0 &&
                b.src2.kind == MACH_OP_IMM &&
                b.dest.kind == MACH_OP_REG && !b.dest.needs_deref &&
                !b.dest.underalign_hint && !b.src1.underalign_hint &&
                (b.src1.btype == IROP_BTYPE_INT32 || b.src1.btype == IROP_BTYPE_FLOAT32) &&
                a.dest.u.reg.r0 == b.dest.u.reg.r0)
            {
              int32_t off2 = (int32_t)b.src2.u.imm.val;
              if (off2 == 4)
              {
                if (tcc_gen_machine_try_strd_imm_base(a.src1.u.imm.val, b.src1.u.imm.val,
                                                       a.dest.u.reg.r0, 0))
                {
                  i = next_i;
                  break;
                }
              }
            }
          }
        }

        /* Store-load forwarding: STORE reg → spill followed immediately by
         * LOAD from the same spill → same reg.  The value is still in the
         * register, so emit the store but skip the redundant load. */
        if (a.dest.kind == MACH_OP_SPILL && !a.dest.needs_deref &&
            a.src1.kind == MACH_OP_REG && !a.src1.is_64bit &&
            (a.dest.u.spill.offset & 3) == 0)
        {
          int next_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          if (next_i >= 0 && ir->compact_instructions[next_i].op == TCCIR_OP_LOAD &&
              !ir->compact_instructions[next_i].is_jump_target)
          {
            IRQuadCompact *lq = &ir->compact_instructions[next_i];
            IROperand l_src1 = tcc_ir_op_get_src1(ir, lq);
            IROperand l_src2 = tcc_ir_op_get_src2(ir, lq);
            IROperand l_dest = tcc_ir_op_get_dest(ir, lq);
            MopArgs la = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, lq,
                                          &l_src1, &l_src2, &l_dest,
                                          (MopSpec){.dest = 1, .src1 = 1});

            if (la.src1.kind == MACH_OP_SPILL && !la.src1.needs_deref &&
                la.src1.u.spill.offset == a.dest.u.spill.offset &&
                la.dest.kind == MACH_OP_REG && !la.dest.is_64bit &&
                la.dest.u.reg.r0 == a.src1.u.reg.r0 &&
                la.src1.btype == a.dest.btype)
            {
              SCRATCH_WRAP(tcc_gen_machine_store_mop(a.dest, a.src1, cq->op));
              i = next_i;
              break;
            }
          }
        }

        SCRATCH_WRAP(tcc_gen_machine_store_mop(a.dest, a.src1, cq->op));
        break;
      }
      case TCCIR_OP_LOAD_INDEXED:
      {
        MopArgs a = DECODE(.dest = 2, .src1 = 1, .src2 = 1, .scale = 1);

        /* LDRD pairing: two adjacent 32-bit LOAD_INDEXED ops with the same
         * base register, scale=0, and constant offsets differing by 4 can
         * fold to a single LDRD.  Mirrors the SPILL-slot LDRD peephole
         * above; the offset is a generic [base, #imm] so we use the
         * non-spill `try_ldrd_base` wrapper. */
        if (!a.dest.is_64bit && a.dest.kind == MACH_OP_REG &&
            a.scale.kind == MACH_OP_IMM && a.scale.u.imm.val == 0 &&
            a.src2.kind == MACH_OP_IMM &&
            a.src1.kind == MACH_OP_REG && !a.src1.needs_deref &&
            !a.src1.underalign_hint && !a.dest.underalign_hint &&
            (a.dest.btype == IROP_BTYPE_INT32 || a.dest.btype == IROP_BTYPE_FLOAT32))
        {
          int next_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          if (next_i >= 0 && ir->compact_instructions[next_i].op == TCCIR_OP_LOAD_INDEXED &&
              !ir->compact_instructions[next_i].is_jump_target)
          {
            IRQuadCompact *nq = &ir->compact_instructions[next_i];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, nq, &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 2, .src1 = 1, .src2 = 1, .scale = 1});

            if (!b.dest.is_64bit && b.dest.kind == MACH_OP_REG &&
                b.scale.kind == MACH_OP_IMM && b.scale.u.imm.val == 0 &&
                b.src2.kind == MACH_OP_IMM &&
                b.src1.kind == MACH_OP_REG && !b.src1.needs_deref &&
                !b.src1.underalign_hint && !b.dest.underalign_hint &&
                (b.dest.btype == IROP_BTYPE_INT32 || b.dest.btype == IROP_BTYPE_FLOAT32) &&
                a.src1.u.reg.r0 == b.src1.u.reg.r0)
            {
              int32_t off1 = (int32_t)a.src2.u.imm.val;
              int32_t off2 = (int32_t)b.src2.u.imm.val;
              int reg1 = a.dest.u.reg.r0;
              int reg2 = b.dest.u.reg.r0;
              int base_reg = a.src1.u.reg.r0;

              /* LDRD writes Rt before Rt2; if Rt overlaps the base reg the
               * second load reads from a clobbered base.  Punt those cases. */
              if (reg1 != reg2 && reg1 != base_reg && reg2 != base_reg)
              {
                if ((off1 & 3) == 0 && off1 + 4 == off2)
                {
                  if (tcc_gen_machine_try_ldrd_base(reg1, reg2, base_reg, off1))
                  {
                    i = next_i;
                    break;
                  }
                }
                else if ((off2 & 3) == 0 && off2 + 4 == off1)
                {
                  if (tcc_gen_machine_try_ldrd_base(reg2, reg1, base_reg, off2))
                  {
                    i = next_i;
                    break;
                  }
                }
              }
            }
          }
        }

        SCRATCH_WRAP(tcc_gen_machine_load_indexed_mop(a.dest, a.src1, a.src2, a.scale, cq->op));
        break;
      }
      case TCCIR_OP_STORE_INDEXED:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1, .scale = 1);

        /* STRD pairing peephole: two adjacent 32-bit STORE_INDEXED ops with
         * same base, scale=0, offsets differing by 4 → single STRD.
         * Only for REG sources — IMM STRD through generic base registers is
         * unsafe because STRD requires 4-byte aligned addresses while
         * individual STR tolerates unaligned access on ARMv8-M. */
        if (!a.src1.is_64bit && a.src1.kind == MACH_OP_REG && !a.src1.needs_deref &&
            a.scale.kind == MACH_OP_IMM && a.scale.u.imm.val == 0 &&
            a.src2.kind == MACH_OP_IMM &&
            a.dest.kind == MACH_OP_REG && !a.dest.needs_deref &&
            !a.dest.underalign_hint && !a.src1.underalign_hint &&
            (a.src1.btype == IROP_BTYPE_INT32 || a.src1.btype == IROP_BTYPE_FLOAT32))
        {
          int next_i = -1;
          for (int j = i + 1; j < ir->next_instruction_index; j++)
          {
            int jop = ir->compact_instructions[j].op;
            /* A branch target between the two STORE_INDEXEDs — even a code-less
             * NOP or identity move — means a jump can land between them, so they
             * cannot share one STRD.  Bail rather than fuse across the label. */
            if (ir->compact_instructions[j].is_jump_target ||
                (branch_target_reset && branch_target_reset[j]))
              break;
            if (jop == TCCIR_OP_NOP)
              continue;
            /* An ASSIGN or pure-vreg LOAD whose src and dst materialise to
             * the same physical register emits no code (mov elision in
             * load/assign codegen).  Skip these so adjacent STORE_INDEXEDs
             * can still pair as STRD even with a no-op copy between them
             * (move-coalesced inlined swap_adjacent / similar patterns). */
            if (jop == TCCIR_OP_ASSIGN || jop == TCCIR_OP_LOAD) {
              IRQuadCompact *jq = &ir->compact_instructions[j];
              IROperand jds = tcc_ir_op_get_src1(ir, jq);
              IROperand jdd = tcc_ir_op_get_dest(ir, jq);
              /* Identity check: src is a vreg / stack-local-as-mov, dst is
               * a vreg, both end up in the same hw register. */
              if (jdd.tag == IROP_TAG_VREG && !jdd.is_lval &&
                  (jds.tag == IROP_TAG_VREG ||
                   (jds.tag == IROP_TAG_STACKOFF && jds.is_local)) &&
                  !jds.is_llocal && !jds.is_sym) {
                MachineOperand sm = machine_op_from_ir(ir, &jds);
                MachineOperand dm = machine_op_from_ir(ir, &jdd);
                if (sm.kind == MACH_OP_REG && dm.kind == MACH_OP_REG &&
                    !sm.needs_deref && !dm.needs_deref &&
                    sm.u.reg.r0 == dm.u.reg.r0 && sm.u.reg.r0 >= 0)
                  continue; /* identity move — skip */
              }
            }
            next_i = j;
            break;
          }
          /* is_jump_target misses some branch targets (see branch_target_reset);
           * consuming a branch-target store removes the label's only emission
           * point, so branches to it backpatch against code address 0. */
          if (next_i >= 0 && ir->compact_instructions[next_i].op == TCCIR_OP_STORE_INDEXED &&
              !ir->compact_instructions[next_i].is_jump_target &&
              !(branch_target_reset && branch_target_reset[next_i]))
          {
            IRQuadCompact *nq = &ir->compact_instructions[next_i];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, nq, &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 1, .src1 = 1, .src2 = 1, .scale = 1});

            if (!b.src1.is_64bit && b.src1.kind == MACH_OP_REG && !b.src1.needs_deref &&
                b.scale.kind == MACH_OP_IMM && b.scale.u.imm.val == 0 &&
                b.src2.kind == MACH_OP_IMM &&
                b.dest.kind == MACH_OP_REG && !b.dest.needs_deref &&
                !b.dest.underalign_hint && !b.src1.underalign_hint &&
                (b.src1.btype == IROP_BTYPE_INT32 || b.src1.btype == IROP_BTYPE_FLOAT32) &&
                a.dest.u.reg.r0 == b.dest.u.reg.r0)
            {
              int32_t off1 = (int32_t)a.src2.u.imm.val;
              int32_t off2 = (int32_t)b.src2.u.imm.val;
              int reg1 = a.src1.u.reg.r0;
              int reg2 = b.src1.u.reg.r0;
              int base_reg = a.dest.u.reg.r0;

              if ((off1 & 3) == 0 && off1 + 4 == off2)
              {
                if (tcc_gen_machine_try_strd_base(reg1, reg2, base_reg, off1))
                {
                  i = next_i;
                  break;
                }
              }
              else if ((off2 & 3) == 0 && off2 + 4 == off1)
              {
                if (tcc_gen_machine_try_strd_base(reg2, reg1, base_reg, off2))
                {
                  i = next_i;
                  break;
                }
              }
            }
          }
        }

        /* STRD pairing peephole for IMM-source STORE_INDEXED ops: two adjacent
         * stores of immediate values to consecutive word-aligned offsets from
         * the same base register → materialise constants into scratch regs,
         * emit a single STRD.  Mirrors the REG-source peephole above. */
        if (a.src1.kind == MACH_OP_IMM && !a.src1.is_64bit &&
            a.scale.kind == MACH_OP_IMM && a.scale.u.imm.val == 0 &&
            a.src2.kind == MACH_OP_IMM &&
            a.dest.kind == MACH_OP_REG && !a.dest.needs_deref &&
            !a.dest.underalign_hint && !a.src1.underalign_hint &&
            (a.src1.btype == IROP_BTYPE_INT32 || a.src1.btype == IROP_BTYPE_FLOAT32))
        {
          int next_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          /* is_jump_target misses some branch targets (see branch_target_reset);
           * consuming a branch-target store removes the label's only emission
           * point, so branches to it backpatch against code address 0. */
          if (next_i >= 0 && ir->compact_instructions[next_i].op == TCCIR_OP_STORE_INDEXED &&
              !ir->compact_instructions[next_i].is_jump_target &&
              !(branch_target_reset && branch_target_reset[next_i]))
          {
            IRQuadCompact *nq = &ir->compact_instructions[next_i];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, nq, &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 1, .src1 = 1, .src2 = 1, .scale = 1});

            if (b.src1.kind == MACH_OP_IMM && !b.src1.is_64bit &&
                b.scale.kind == MACH_OP_IMM && b.scale.u.imm.val == 0 &&
                b.src2.kind == MACH_OP_IMM &&
                b.dest.kind == MACH_OP_REG && !b.dest.needs_deref &&
                !b.dest.underalign_hint && !b.src1.underalign_hint &&
                (b.src1.btype == IROP_BTYPE_INT32 || b.src1.btype == IROP_BTYPE_FLOAT32) &&
                a.dest.u.reg.r0 == b.dest.u.reg.r0)
            {
              int32_t off1 = (int32_t)a.src2.u.imm.val;
              int32_t off2 = (int32_t)b.src2.u.imm.val;
              int base_reg = a.dest.u.reg.r0;

              if ((off1 & 3) == 0 && off1 + 4 == off2)
              {
                if (tcc_gen_machine_try_strd_imm_base(a.src1.u.imm.val, b.src1.u.imm.val,
                                                       base_reg, off1))
                {
                  i = next_i;
                  break;
                }
              }
              else if ((off2 & 3) == 0 && off2 + 4 == off1)
              {
                if (tcc_gen_machine_try_strd_imm_base(b.src1.u.imm.val, a.src1.u.imm.val,
                                                       base_reg, off2))
                {
                  i = next_i;
                  break;
                }
              }
            }
          }
        }

        /* Byte-to-word coalescing peephole: four consecutive byte
         * STORE_INDEXEDs with immediate sources to word-aligned consecutive
         * offsets on the same base → single word store of the packed constant.
         * Saves 3 constant loads + 3 strb → 1 movs + 1 str. */
        if (a.src1.kind == MACH_OP_IMM && !a.src1.is_64bit &&
            a.src1.btype == IROP_BTYPE_INT8 &&
            a.scale.kind == MACH_OP_IMM && a.scale.u.imm.val == 0 &&
            a.src2.kind == MACH_OP_IMM &&
            ((int32_t)a.src2.u.imm.val & 3) == 0)
        {
          int32_t base_off = (int32_t)a.src2.u.imm.val;
          uint32_t combined = (uint32_t)(a.src1.u.imm.val & 0xFF);
          int last_i = i;
          int found = 0;

          for (int k = 1; k <= 3; k++)
          {
            int next_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, last_i);
            if (next_i < 0 ||
                ir->compact_instructions[next_i].op != TCCIR_OP_STORE_INDEXED ||
                ir->compact_instructions[next_i].is_jump_target)
              break;

            IRQuadCompact *nq = &ir->compact_instructions[next_i];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, nq,
                                         &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 1, .src1 = 1, .src2 = 1, .scale = 1});

            if (b.src1.kind != MACH_OP_IMM || b.src1.is_64bit ||
                b.src1.btype != IROP_BTYPE_INT8 ||
                b.scale.kind != MACH_OP_IMM || b.scale.u.imm.val != 0 ||
                b.src2.kind != MACH_OP_IMM ||
                (int32_t)b.src2.u.imm.val != base_off + k)
              break;

            if (b.dest.kind != a.dest.kind)
              break;
            if (a.dest.kind == MACH_OP_REG &&
                (b.dest.u.reg.r0 != a.dest.u.reg.r0 || b.dest.needs_deref != a.dest.needs_deref))
              break;
            if (a.dest.kind == MACH_OP_FRAME_ADDR &&
                b.dest.u.frame.offset != a.dest.u.frame.offset)
              break;

            combined |= (uint32_t)(b.src1.u.imm.val & 0xFF) << (k * 8);
            last_i = next_i;
            found++;
          }

          if (found == 3)
          {
            uint32_t combined2 = 0;
            int last_i2 = last_i;
            int found2 = 0;
            for (int k = 0; k <= 3; k++)
            {
              int next_i2 = -1;
              for (int j = last_i2 + 1; j < ir->next_instruction_index; j++)
              {
                if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
                {
                  next_i2 = j;
                  break;
                }
              }
              if (next_i2 < 0 ||
                  ir->compact_instructions[next_i2].op != TCCIR_OP_STORE_INDEXED ||
                  ir->compact_instructions[next_i2].is_jump_target)
                break;

              IRQuadCompact *nq2 = &ir->compact_instructions[next_i2];
              IROperand ns1 = tcc_ir_op_get_src1(ir, nq2);
              IROperand ns2 = tcc_ir_op_get_src2(ir, nq2);
              IROperand nd = tcc_ir_op_get_dest(ir, nq2);
              MopArgs c = ir_decode_cached(is_dry_run, 0, NULL, next_i2, ir, nq2,
                                           &ns1, &ns2, &nd,
                                           (MopSpec){.dest = 1, .src1 = 1, .src2 = 1, .scale = 1});

              if (c.src1.kind != MACH_OP_IMM || c.src1.is_64bit ||
                  c.src1.btype != IROP_BTYPE_INT8 ||
                  c.scale.kind != MACH_OP_IMM || c.scale.u.imm.val != 0 ||
                  c.src2.kind != MACH_OP_IMM ||
                  (int32_t)c.src2.u.imm.val != base_off + 4 + k)
                break;
              if (c.dest.kind != a.dest.kind)
                break;
              if (a.dest.kind == MACH_OP_REG &&
                  (c.dest.u.reg.r0 != a.dest.u.reg.r0 || c.dest.needs_deref != a.dest.needs_deref))
                break;
              if (a.dest.kind == MACH_OP_FRAME_ADDR &&
                  c.dest.u.frame.offset != a.dest.u.frame.offset)
                break;

              combined2 |= (uint32_t)(c.src1.u.imm.val & 0xFF) << (k * 8);
              last_i2 = next_i2;
              found2++;
            }

            if (found2 == 4)
            {
              /* All 8 bytes coalesced.  Emit TWO 32-bit STRs, NOT an STRD:
               * these stores originate from INT8 writes, so the destination
               * has byte (1) alignment — e.g. zero-initialising an element of
               * an array of 9-byte structs, where the base register holds
               * `arr + i*9` and is unaligned for odd i.  On ARMv7-M/v8-M a
               * single STR tolerates an unaligned address (CCR.UNALIGN_TRP=0
               * by default) but STRD/LDRD ALWAYS fault when unaligned, so
               * pairing into STRD off a register base (try_unroll_loop_ex's
               * struct-array zero-init miscompiled this way) is unsafe. */
              MachineOperand wv1 = a.src1;
              wv1.btype = IROP_BTYPE_INT32;
              wv1.u.imm.val = (int64_t)(int32_t)combined;
              SCRATCH_WRAP(tcc_gen_machine_store_indexed_mop(a.dest, a.src2, a.scale, wv1, cq->op));

              MachineOperand off2 = a.src2;
              off2.u.imm.val = base_off + 4;
              MachineOperand wv2 = a.src1;
              wv2.btype = IROP_BTYPE_INT32;
              wv2.u.imm.val = (int64_t)(int32_t)combined2;
              SCRATCH_WRAP(tcc_gen_machine_store_indexed_mop(a.dest, off2, a.scale, wv2, cq->op));
              i = last_i2;
              break;
            }

            MachineOperand word_val = a.src1;
            word_val.btype = IROP_BTYPE_INT32;
            word_val.u.imm.val = (int64_t)(int32_t)combined;
            SCRATCH_WRAP(tcc_gen_machine_store_indexed_mop(a.dest, a.src2, a.scale, word_val, cq->op));
            i = last_i;
            break;
          }
        }

        SCRATCH_WRAP(tcc_gen_machine_store_indexed_mop(a.dest, a.src2, a.scale, a.src1, cq->op));
        break;
      }
      case TCCIR_OP_LOAD_POSTINC:
      {
        MopArgs a = DECODE(.dest = 2, .src1 = 1, .scale = 1);
        SCRATCH_WRAP(tcc_gen_machine_load_postinc_mop(a.dest, a.src1, a.scale, cq->op));
        break;
      }
      case TCCIR_OP_STORE_POSTINC:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .scale = 1);
        SCRATCH_WRAP(tcc_gen_machine_store_postinc_mop(a.dest, a.src1, a.scale, cq->op));
        break;
      }
      case TCCIR_OP_RETURNVALUE:
      {
        MopArgs a = DECODE(.src1 = 1);
        SCRATCH_WRAP(tcc_gen_machine_return_value_mop(a.src1, cq->op));
      }
      /* fall through to RETURNVOID */
      case TCCIR_OP_RETURNVOID:
        /* Real-run: emit jump to epilogue (backpatched later).
         * Dry-run: no-op (we don't track return_jump_addrs).
         * Skip the jump if all remaining instructions are NOPs —
         * the epilogue immediately follows, so the branch is a no-op. */
        {
          int has_trailing_code = 0;
          for (int j = i + 1; j < ir->next_instruction_index; j++)
          {
            if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
            {
              has_trailing_code = 1;
              break;
            }
          }
          if (!is_dry_run && has_trailing_code)
          {
            /* Target -1 means "the epilogue", which is not an IR index.  The
             * emitter sizes it from the rehearsal's end-of-body address. */
            int ret_branch_size = tcc_gen_machine_jump_mop(cq->op, -1, i);
            return_jump_addrs[num_return_jumps++] = ind - ret_branch_size;
          }
        }
        break;
      case TCCIR_OP_ASSIGN:
      {
        MopArgs a = DECODE(.dest = 2, .src1 = 1);

        /* A bare immediate destination (no deref) is malformed IR: you cannot
         * assign into a literal, so the instruction is a dead no-op.  It can
         * survive when const-prop folds a value to a constant and a fusion then
         * consumes the real consumer, leaving a stranded `#K <- #M` assign
         * (seed 2966: the UDIV accumulator folds to 9 and is fused into the
         * MLA, but the now-dead `#9 <- #-9733` def keeps an immediate dest).
         * Drop it rather than aborting in mach_get_dest_reg ("unexpected kind
         * 3"); the live value already resides in the fused op, so the computed
         * result is unchanged. */
        if (a.dest.kind == MACH_OP_IMM && !a.dest.needs_deref)
          break;

        /* LDRD peephole: two adjacent 32-bit assigns loading from adjacent
         * spill slots into registers → single LDRD instruction. */
        if (a.src1.kind == MACH_OP_SPILL && !a.src1.needs_deref &&
            a.dest.kind == MACH_OP_REG && !a.dest.is_64bit &&
            (a.src1.btype == IROP_BTYPE_INT32 || a.src1.btype == IROP_BTYPE_FLOAT32) &&
            (a.src1.u.spill.offset & 3) == 0)
        {
          int next_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          if (next_i >= 0 && ir->compact_instructions[next_i].op == TCCIR_OP_ASSIGN &&
              !ir->compact_instructions[next_i].is_jump_target)
          {
            IRQuadCompact *nq = &ir->compact_instructions[next_i];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, nq,
                                         &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 2, .src1 = 1});

            if (b.src1.kind == MACH_OP_SPILL && !b.src1.needs_deref &&
                b.dest.kind == MACH_OP_REG && !b.dest.is_64bit &&
                (b.src1.btype == IROP_BTYPE_INT32 || b.src1.btype == IROP_BTYPE_FLOAT32) &&
                (b.src1.u.spill.offset & 3) == 0)
            {
              int32_t off1 = a.src1.u.spill.offset;
              int32_t off2 = b.src1.u.spill.offset;
              int reg1 = a.dest.u.reg.r0;
              int reg2 = b.dest.u.reg.r0;

              if (reg1 != reg2 && off1 + 4 == off2)
              {
                if (tcc_gen_machine_try_ldrd_spill(reg1, off1, reg2, off2))
                {
                  i = next_i;
                  break;
                }
              }
              else if (reg1 != reg2 && off2 + 4 == off1)
              {
                if (tcc_gen_machine_try_ldrd_spill(reg2, off2, reg1, off1))
                {
                  i = next_i;
                  break;
                }
              }
            }
          }
        }

        /* STRD peephole: two adjacent 32-bit assigns storing registers to
         * adjacent spill slots → single STRD instruction.
         *
         * needs_deref on a REG src means the assign is really a LOAD
         * (`T <- *reg`), not a plain reg→spill store: fusing it into an STRD
         * would spill the *pointer* raw instead of the dereferenced value,
         * dropping a level of indirection (agg_deep seeds 52367/53515: a
         * `T46 <- *ppa` def stored ppa itself, so a later `*T46 = x` corrupted
         * the pointer and the next `**ppa` read faulted). Require plain-value
         * sources on both halves — the STORE_INDEXED STRD peephole above guards
         * this the same way. */
        if (a.dest.kind == MACH_OP_SPILL && !a.dest.needs_deref &&
            a.src1.kind == MACH_OP_REG && !a.src1.is_64bit && !a.src1.needs_deref &&
            (a.dest.btype == IROP_BTYPE_INT32 || a.dest.btype == IROP_BTYPE_FLOAT32) &&
            (a.dest.u.spill.offset & 3) == 0)
        {
          int next_i = ir_codegen_next_nonnop_no_label(ir, branch_target_reset, i);
          if (next_i >= 0 && ir->compact_instructions[next_i].op == TCCIR_OP_ASSIGN &&
              !ir->compact_instructions[next_i].is_jump_target)
          {
            IRQuadCompact *nq = &ir->compact_instructions[next_i];
            IROperand n_src1_ir = tcc_ir_op_get_src1(ir, nq);
            IROperand n_src2_ir = tcc_ir_op_get_src2(ir, nq);
            IROperand n_dest_ir = tcc_ir_op_get_dest(ir, nq);
            MopArgs b = ir_decode_cached(is_dry_run, 0, NULL, next_i, ir, nq,
                                         &n_src1_ir, &n_src2_ir, &n_dest_ir,
                                         (MopSpec){.dest = 2, .src1 = 1});

            if (b.dest.kind == MACH_OP_SPILL && !b.dest.needs_deref &&
                b.src1.kind == MACH_OP_REG && !b.src1.is_64bit && !b.src1.needs_deref &&
                (b.dest.btype == IROP_BTYPE_INT32 || b.dest.btype == IROP_BTYPE_FLOAT32) &&
                (b.dest.u.spill.offset & 3) == 0)
            {
              int32_t off1 = a.dest.u.spill.offset;
              int32_t off2 = b.dest.u.spill.offset;
              int reg1 = a.src1.u.reg.r0;
              int reg2 = b.src1.u.reg.r0;

              if (reg1 != reg2 && off1 + 4 == off2)
              {
                if (tcc_gen_machine_try_strd_spill(reg1, off1, reg2, off2))
                {
                  i = next_i;
                  break;
                }
              }
              else if (reg1 != reg2 && off2 + 4 == off1)
              {
                if (tcc_gen_machine_try_strd_spill(reg2, off2, reg1, off1))
                {
                  i = next_i;
                  break;
                }
              }
            }
          }
        }

        SCRATCH_WRAP(tcc_gen_machine_assign_mop(a.src1, a.dest, cq->op));
        break;
      }
      case TCCIR_OP_ZEXT:
      {
        /* Zero-extension: lower like ASSIGN — assign_mop already emits the
         * u32-src→u64-dest widening (low = src, high = 0).  The point of
         * ZEXT as a distinct opcode is to be opaque to the IR optimizer's
         * value-tracking, which would otherwise sign-extend the source. */
        MopArgs a = DECODE(.dest = 2, .src1 = 1);
        SCRATCH_WRAP(tcc_gen_machine_assign_mop(a.src1, a.dest, TCCIR_OP_ASSIGN));
        break;
      }
      case TCCIR_OP_PACK64:
      {
        /* Pack two u32s into a u64: dest_lo = src1, dest_hi = src2.  Lower
         * to two 32-bit assigns to the dest's halves. */
        MopArgs a = DECODE(.dest = 2, .src1 = 1, .src2 = 1);
        SCRATCH_WRAP(tcc_gen_machine_pack64_mop(a.src1, a.src2, a.dest));
        break;
      }
      case TCCIR_OP_LEA:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1);
        SCRATCH_WRAP(tcc_gen_machine_lea_mop(a.dest, a.src1));
        break;
      }
      case TCCIR_OP_FUNCPARAMVAL:
      case TCCIR_OP_FUNCPARAMVOID:
      {
        MopArgs a = DECODE(.src1 = 1, .src2 = 1);
        tcc_gen_machine_func_parameter_mop(a.src1, a.src2, cq->op);
        break;
      }
      case TCCIR_OP_JUMP:
      {
        int branch_size = tcc_gen_machine_jump_mop(cq->op, irop_get_imm32(dest_ir), i);
        if (!is_dry_run)
          ir_to_code_mapping[i] = ind - branch_size;
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        break;
      }
      case TCCIR_OP_JUMPIF:
      {
        int branch_size;
        if (codegen_cbz_reg >= 0)
        {
          branch_size = tcc_gen_machine_cbz_jump_mop(codegen_cbz_reg, codegen_cbz_nonzero, irop_get_imm32(dest_ir), i);
          codegen_cbz_reg = -1;
        }
        else
        {
          branch_size = tcc_gen_machine_conditional_jump_mop(src1_ir.u.imm32, cq->op, irop_get_imm32(dest_ir), i);
        }
        if (!is_dry_run)
          ir_to_code_mapping[i] = ind - branch_size;
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        break;
      }
      case TCCIR_OP_IJUMP:
      {
        MopArgs a = DECODE(.src1 = 1);
        SCRATCH_WRAP(tcc_gen_machine_indirect_jump_mop(a.src1, cq->op));
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        break;
      }
      case TCCIR_OP_SWITCH_TABLE:
      {
        int table_id = (int)irop_get_imm64_ex(ir, src2_ir);
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        MopArgs a = DECODE(.src1 = 1);
        /* Flush any pending literal pool before the dispatch+table block so it
         * cannot be flushed in the middle of the preamble (which would relocate
         * the terminal `ADD Rt,PC; BX Rt` past the pool and break the switch-
         * table offset backpatch).  Done in both passes — with the same byte
         * count — so dry-run size estimates and real-run addresses agree. */
        tcc_gen_machine_reserve_pool_bytes(tcc_gen_machine_switch_table_dry_run_size(table->num_entries));
        if (is_dry_run)
        {
          ind += tcc_gen_machine_switch_table_dry_run_size(table->num_entries);
        }
        else
        {
          tcc_gen_machine_insn_scratch_reset();
          tcc_gen_machine_switch_table_mop(a.src1, table, ir, i);
        }
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        break;
      }
      case TCCIR_OP_SWITCH_LOAD:
      {
        int vt_id = (int)irop_get_imm64_ex(ir, src2_ir);
        TCCIRSwitchValueTable *vtab = &ir->switch_value_tables[vt_id];
        MopArgs a = DECODE(.dest = 1, .src1 = 1);
        if (is_dry_run)
        {
          ind += tcc_gen_machine_switch_load_dry_run_size(vtab->num_entries);
        }
        else
        {
          tcc_gen_machine_insn_scratch_reset();
          tcc_gen_machine_switch_load_mop(a.src1, a.dest, vtab, ir, i);
        }
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        break;
      }
      case TCCIR_OP_SETIF:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1);
        SCRATCH_WRAP(tcc_gen_machine_setif_mop(a.src1, a.dest, cq->op));
        break;
      }
      case TCCIR_OP_BOOL_OR:
      case TCCIR_OP_BOOL_AND:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        SCRATCH_WRAP(tcc_gen_machine_bool_mop(a.src1, a.src2, a.dest, cq->op));
        break;
      }
      case TCCIR_OP_VLA_ALLOC:
      case TCCIR_OP_VLA_SP_SAVE:
      case TCCIR_OP_VLA_SP_RESTORE:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        tcc_gen_machine_vla_mop(a.dest, a.src1, a.src2, cq->op);
        break;
      }
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_FUNCCALLVAL:
      {
        int drop_return_value = (cq->op == TCCIR_OP_FUNCCALLVOID);
        MopArgs a = DECODE(.dest = 2, .src1 = 1);
        tcc_gen_machine_insn_scratch_reset();
        tcc_gen_machine_func_call_mop(a.src1, src2_ir, a.dest, drop_return_value, ir, i);
        ir_codegen_track_scratch(is_dry_run && !is_rehearsal, i, cq->op, dry_insn_scratch, dry_insn_saves);
        tcc_ir_spill_cache_clear(&ir->spill_cache);
        if (ir->has_static_chain)
          tcc_gen_machine_restore_chain();
        break;
      }
      case TCCIR_OP_NOP:
        break;
      case TCCIR_OP_PREFETCH:
      {
        MopArgs a = DECODE(.src1 = 1);
        /* src2 holds the rw hint: 0 = read (PLD), 1 = write (PLDW) */
        SCRATCH_WRAP(tcc_gen_machine_prefetch_mop(a.src1, (int)irop_get_imm64_ex(ir, src2_ir)));
        break;
      }
      case TCCIR_OP_TRAP:
        tcc_gen_machine_trap_mop();
        break;
      case TCCIR_OP_SETJMP:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        SCRATCH_WRAP(tcc_gen_machine_setjmp_mop(a.src1, a.src2, a.dest));
        break;
      }
      case TCCIR_OP_LONGJMP:
      {
        MopArgs a = DECODE(.src1 = 1);
        SCRATCH_WRAP(tcc_gen_machine_longjmp_mop(a.src1));
        break;
      }
      case TCCIR_OP_NL_SETJMP:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1);
        SCRATCH_WRAP(tcc_gen_machine_nl_setjmp_mop(a.src1, a.dest));
        break;
      }
      case TCCIR_OP_NL_LONGJMP:
      {
        MopArgs a = DECODE(.src1 = 1);
        SCRATCH_WRAP(tcc_gen_machine_nl_longjmp_mop(a.src1));
        break;
      }
      case TCCIR_OP_BUILTIN_APPLY_ARGS:
      {
        MopArgs a = DECODE(.dest = 1);
        SCRATCH_WRAP(tcc_gen_machine_builtin_apply_args_mop(a.dest));
        break;
      }
      case TCCIR_OP_BUILTIN_APPLY:
      {
        MopArgs a = DECODE(.dest = 1, .src1 = 1, .src2 = 1);
        SCRATCH_WRAP(tcc_gen_machine_builtin_apply_mop(a.src1, a.src2, a.dest));
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
      case TCCIR_OP_BLOCK_COPY:
      {
        /* dest=stack offset, src1=symbol ref, src2=size immediate.
         * No vregs involved - pass raw IROperands to the backend. */
        IROperand bc_dest = tcc_ir_op_get_dest(ir, cq);
        IROperand bc_src = tcc_ir_op_get_src1(ir, cq);
        int bc_size = (int)irop_get_imm64_ex(ir, src2_ir);
        tcc_gen_machine_block_copy_mop(ir, bc_dest, bc_src, bc_size);
        break;
      }
      case TCCIR_OP_SELECT:
      {
        /* Skip if the preceding CMP's SUBS+IT peephole already emitted the
         * full sequence (subs + it ne + movne #1) in this slot. */
        if (i == codegen_skip_select) {
          codegen_skip_select = -1;
          break;
        }
        /* Conditional select: dest = (cond) ? src1 : src2
         * Condition code stored in 4th pool entry as IMM32. */
        MopArgs a = DECODE(.dest = 2, .src1 = 1, .src2 = 1);
        IROperand cond_op = tcc_ir_op_get_cond(ir, cq);
        int cond_code = (int)irop_get_imm64_ex(ir, cond_op);
        SCRATCH_WRAP(tcc_gen_machine_select_mop(a.src1, a.src2, a.dest, cond_code));
        break;
      }
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

#undef DECODE
#undef SCRATCH_WRAP

      /* Track condition-flag liveness for the backend: after a CMP the flags
       * are live until consumed by a JUMPIF.  Any control-flow instruction
       * (JUMP, IJUMP, SWITCH_TABLE) also kills the pending flags. */
      if (cq->op == TCCIR_OP_CMP || cq->op == TCCIR_OP_TEST_ZERO)
        ir->codegen_flags_live = 1;
      else if (cq->op == TCCIR_OP_JUMPIF || cq->op == TCCIR_OP_JUMP ||
               cq->op == TCCIR_OP_IJUMP || cq->op == TCCIR_OP_SWITCH_TABLE)
        ir->codegen_flags_live = 0;

      /* Clean up scratch register state at end of each IR instruction.
       * This restores any pushed scratch registers and resets the global exclude mask. */
      tcc_gen_machine_end_instruction();
    }

    /* ---- Pass-specific finalisation ---- */
    if (is_rehearsal)
    {
      /* The rehearsal exists only to produce an address map the real pass will
       * match.  Capture it, then rewind everything the real pass redoes. */
      tcc_gen_machine_dry_run_end();
      tcc_gen_machine_dry_run_set_rehearsal(0);

      if (cbz_dry_mapping)
        tcc_free(cbz_dry_mapping);
      cbz_dry_mapping = tcc_malloc(ir->ir_to_code_mapping_size * sizeof(uint32_t));
      ir->codegen_cbz_dry_mapping = cbz_dry_mapping;
      memcpy(cbz_dry_mapping, ir_to_code_mapping, ir->ir_to_code_mapping_size * sizeof(uint32_t));
      ir->codegen_rehearsal_end = (uint32_t)ind;

      ind = saved_ind;
      loc = saved_loc;
      ir->call_outgoing_base = saved_call_outgoing_base;
      ir->call_nested_save_base = saved_call_nested_save_base;
      ir->codegen_instruction_idx = saved_codegen_idx;

      tcc_gen_machine_reset_scratch_state();
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      tcc_ir_opt_fp_cache_clear(ir);
      /* Both passes must START from the same backend cache state or the
       * rehearsal is not a model of the real pass.  dry_run_init/start reset
       * the MOV-equivalence and STR->LDR caches before each dry pass; nothing
       * reset them before the real pass, so it used to inherit the dry pass's
       * equivalences and could elide a `mov` that is actually needed
       * (tests2/25_quicksort::partition dropped `mov r4, r0` at -O2). */
      tcc_gen_machine_mov_coalesce_reset();
    }
    else if (is_dry_run)
    {
      /* End dry-run and analyze results */
      tcc_gen_machine_dry_run_end();

      /* Analyze branch offsets and select optimal encodings */
      tcc_gen_machine_branch_opt_analyze(ir_to_code_mapping, ir->next_instruction_index);

      /* Save dry-run mapping for CBZ distance estimation in the real pass.
       * The real pass overwrites ir_to_code_mapping as it goes, but CBZ needs
       * the dry-run distance (source→target within the same run) to avoid
       * literal-pool-timing divergence between runs. */
      if (cbz_dry_mapping)
        tcc_free(cbz_dry_mapping);
      cbz_dry_mapping = tcc_malloc(ir->ir_to_code_mapping_size * sizeof(uint32_t));
      ir->codegen_cbz_dry_mapping = cbz_dry_mapping;
      memcpy(cbz_dry_mapping, ir_to_code_mapping, ir->ir_to_code_mapping_size * sizeof(uint32_t));

      /* Check if LR was pushed during dry run in a leaf function */
      if (original_leaffunc && tcc_gen_machine_dry_run_get_lr_push_count() > 0)
      {
        extra_prologue_regs |= (1 << 14); /* R_LR */
      }

      /* Hoist the shared-.rodata base into a register the allocation left
       * free, so every reference in the body becomes a bare ADD.  Claimed
       * here, ahead of the phase-3 scratch fixup and the demotion pass
       * below: those hand out free callee-saved registers too, and the
       * allocator deliberately left this one for us. */
      extra_prologue_regs |= tcc_gen_machine_rodata_anchor_claim(ir->ls.dirty_registers);

      /* Restore state for real code generation */
      ind = saved_ind;
      loc = saved_loc;
      ir->call_outgoing_base = saved_call_outgoing_base;
      ir->call_nested_save_base = saved_call_nested_save_base;
      ir->codegen_instruction_idx = saved_codegen_idx;

      /* Phase-3 scratch conflict fixup.
       * For each instruction where the dry run needed to PUSH a register,
       * try to move the blocking vreg to a free callee-saved register.
       *
       * If the specific register from dry_insn_saves can't be freed (e.g. it
       * holds a function parameter pinned by the ABI), try freeing any other
       * R0-R3 register that is occupied at this instruction.  Low registers
       * use 16-bit Thumb encoding for PUSH/POP and most ALU ops, so freeing
       * one avoids the push/pop entirely AND keeps instructions compact. */
      {
        int any_fixup = 0;
        /* Snapshot the pre-fixup live map: an instruction is only "fixed" once
         * we have actually freed as many registers at it as the dry run had to
         * save.  Counting "some R0-R3 is free" instead under-counts whenever
         * one instruction borrows two scratch registers (a global's address
         * plus the value) and leaves the second borrow paying save/restore. */
        uint32_t *orig_live = NULL;
        int orig_live_n = 0;
        /* Built by the first demote attempt that gets as far as counting, and
         * shared by every later one in this function. */
        int32_t *demote_ref_cache = NULL;
        if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
        {
          orig_live_n = ir->ls.live_regs_by_instruction_size;
          orig_live = tcc_malloc((size_t)orig_live_n * sizeof(uint32_t));
          memcpy(orig_live, ir->ls.live_regs_by_instruction, (size_t)orig_live_n * sizeof(uint32_t));
        }
        for (int i = 0; i < ir->next_instruction_index; i++)
        {
          uint16_t saves = dry_insn_saves[i];
          if (!saves)
            continue;
          const int need = __builtin_popcount(saves);
          int all_fixed = 1;
          while (saves)
          {
            int r = (int)__builtin_ctz(saves);
            saves = (uint16_t)(saves & (saves - 1u));
            int new_r = try_reassign_scratch_conflict(ir, r, i);
            if (new_r >= 0)
            {
              any_fixup = 1;
            }
            else
            {
              /* The recorded register couldn't be freed.  Try to free any
               * other R0-R3 at this instruction — if one is already free or
               * can be freed, tcc_ls_find_free_scratch_reg will find it during
               * the real run and no push/pop will be needed. */
              int alt_fixed = 0;
              int enough_freed = 0;
              if (ir->ls.live_regs_by_instruction && i < ir->ls.live_regs_by_instruction_size)
              {
                uint32_t live = ir->ls.live_regs_by_instruction[i];
                /* Has the fixup already handed this instruction as many extra
                 * registers as the dry run had to save?  "Some R0-R3 is free"
                 * is not the same question: an instruction that borrows two
                 * scratch registers (a global's address plus the value) still
                 * pays save/restore for the second one. */
                enough_freed = (orig_live && i < orig_live_n &&
                                ir_codegen_regs_freed_at(orig_live[i], live) >= need);
                /* If any R0-R3 is already free, the real run will use it. */
                if ((~live & 0xFu) & ~(1u << r))
                {
                  alt_fixed = 1;
                }
                else
                {
                  /* All R0-R3 occupied — try to reassign one to callee-saved. */
                  for (int ar = 0; ar <= 3; ar++)
                  {
                    if (ar == r)
                      continue;
                    if (try_reassign_scratch_conflict(ir, ar, i) >= 0)
                    {
                      any_fixup = 1;
                      alt_fixed = 1;
                      break;
                    }
                  }
                }
              }
              /* Nothing could be moved (or not enough of it): spill the
               * blocker so its register becomes a permanently free scratch
               * over the blocked window. */
              if (!enough_freed && demoted_count < (int)(sizeof(demoted_vregs) / sizeof(demoted_vregs[0])))
              {
                int dv = try_demote_scratch_conflict(ir, r, i, dry_insn_saves, dry_insn_scratch,
                                                     ir->next_instruction_index, &demote_ref_cache);
                if (dv >= 0)
                {
                  demoted_vregs[demoted_count++] = dv;
                  any_fixup = 1;
                  alt_fixed = 1;
                }
              }
              if (!alt_fixed)
                all_fixed = 0;
            }
          }
          if (all_fixed)
            dry_insn_scratch[i] = 0;
        }
        if (orig_live)
          tcc_free(orig_live);
        if (demote_ref_cache)
          tcc_free(demote_ref_cache);
        if (any_fixup)
        {
          tcc_ls_reset_scratch_cache(&ir->ls);
          /* Interval table was mutated: cached MopArgs are stale, discard. */
          tcc_free(mop_cache);
          mop_cache = NULL;
          ir->codegen_mop_cache = NULL;
        }
        use_mop_cache = (mop_cache != NULL);
      }

      /* Shrink the nested-call save area to the dry run's ACTUAL maximum.
       * The static max_nested_save_regs sizing over-approximates from blob
       * liveness bitmaps (311 corpus functions carried a reservation with
       * zero stores into it).  The call-site save logic is deterministic
       * across passes, so the dry run's per-site maximum is exact.  All
       * frame areas below the locals shift up by the returned slack; the
       * call sites address these areas by SIZE ([SP + outgoing], slots
       * upward), so only loc and the two base fields need adjusting.
       * Skipped under FP/dynamic-SP frames (FP-relative bias math). */
      if (!tcc_state->need_frame_pointer && !tcc_state->func_dynamic_sp &&
          ir->call_nested_save_size > 0)
      {
        int actual = tcc_gen_machine_dry_run_get_max_nested_saves() * 4;
        if (actual < ir->call_nested_save_size)
        {
          int delta = ir->call_nested_save_size - actual;
          ir->call_nested_save_size = actual;
          loc += delta;
          ir->call_nested_save_base += delta;
          ir->call_outgoing_base += delta;
          stack_size = (-loc + 7) & ~7;
        }
      }

      /* Allocate scratch save area if the dry run detected scratch pushes.
       * When FP is omitted, scratch PUSH/POP would move SP and break
       * SP-relative addressing.  Instead, reserve stack slots so that
       * get_scratch_reg_with_save() can use STR/LDR to fixed offsets.
       * Only allocate when actually needed (detected by dry run). */
      if (!tcc_state->need_frame_pointer)
      {
        int max_scratch_depth = 0;
        /* Check per-instruction saves from dry run */
        for (int i = 0; i < ir->next_instruction_index; i++)
        {
          if (dry_insn_saves[i])
          {
            int depth = __builtin_popcount(dry_insn_saves[i]);
            if (depth > max_scratch_depth)
              max_scratch_depth = depth;
          }
        }
        /* Also check the global bitmap as safety net for dry/real divergence */
        {
          uint32_t global_bitmap = tcc_gen_machine_dry_run_get_scratch_regs_pushed();
          if (global_bitmap)
          {
            int depth = __builtin_popcount(global_bitmap);
            if (depth > max_scratch_depth)
              max_scratch_depth = depth;
          }
        }
        if (max_scratch_depth > 0)
        {
          /* Round up to 8 so the frame's alignment padding (and with it the
           * SP-literal addressing of the outgoing/nested areas) is unchanged
           * relative to the no-scratch layout. */
          ir->scratch_save_size = (max_scratch_depth * 4 + 7) & ~7;
          loc -= ir->scratch_save_size;
          /* Keep the outgoing call-arg area at the very bottom of the frame
           * and the nested-call save area directly above it (both are
           * addressed with literal SP offsets); see the matching re-slot in
           * the might_need_scratch reservation above. */
          if (ir->call_outgoing_size > 0 || ir->call_nested_save_size > 0)
          {
            ir->call_outgoing_base = loc;
            ir->call_nested_save_base = loc + ir->call_outgoing_size;
            ir->scratch_save_base = loc + ir->call_outgoing_size + ir->call_nested_save_size;
          }
          else
          {
            ir->scratch_save_base = loc;
          }
          /* Recompute stack_size with scratch area included */
          stack_size = (-loc + 7) & ~7;
        }
      }

      /* Home slots for the vregs the phase-3 fixup demoted to memory.  Carved
       * out the same way as the scratch save area: the frame grows downward
       * and the areas addressed by literal SP offsets slide with it. */
      if (demoted_count > 0)
      {
        int demote_size = (demoted_count * 4 + 7) & ~7;
        loc -= demote_size;
        int slot_base;
        if (ir->call_outgoing_size > 0 || ir->call_nested_save_size > 0 || ir->scratch_save_size > 0)
        {
          ir->call_outgoing_base = loc;
          ir->call_nested_save_base = loc + ir->call_outgoing_size;
          ir->scratch_save_base = loc + ir->call_outgoing_size + ir->call_nested_save_size;
          slot_base = ir->scratch_save_base + ir->scratch_save_size;
        }
        else
        {
          slot_base = loc;
        }
        for (int k = 0; k < demoted_count; k++)
        {
          int off = slot_base + k * 4;
          IRLiveInterval *dli = tcc_ir_get_live_interval(ir, demoted_vregs[k]);
          if (dli)
          {
            dli->allocation.r0 = PREG_SPILLED | PREG_REG_NONE;
            dli->allocation.r1 = PREG_NONE;
            dli->allocation.offset = off;
          }
          for (int j = 0; j < ir->ls.next_interval_index; j++)
          {
            if (ir->ls.intervals[j].vreg == (uint32_t)demoted_vregs[k])
              ir->ls.intervals[j].stack_location = (uint32_t)off;
          }
        }
        stack_size = (-loc + 7) & ~7;
      }

      /* Reset scratch state for real pass */
      tcc_gen_machine_reset_scratch_state();
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      tcc_ir_opt_fp_cache_clear(ir);

      /* Emit prologue before real pass.
       * The dry-run peephole already patched some allocations, but re-run
       * the pre-patch for any cases the peephole missed (e.g. if the
       * dispatch didn't trigger for certain ops). */
      (void)original_leaffunc;
      ir_codegen_pre_patch_funcparam_allocations(ir);
      ir_codegen_recompute_dirty_from_allocations(ir);
      if (!ir->naked)
        tcc_gen_machine_prolog(ir->leaffunc, ir->ls.dirty_registers, stack_size, extra_prologue_regs);
      if (!ir->naked)
        tcc_debug_prolog_epilog(tcc_state, 0);
    }
  }

  if (cg_pt.active)
    tcc_pass_timing_end(&cg_pt, -1);

  tcc_free(mop_cache);
  ir->codegen_mop_cache = NULL;
  if (cbz_dry_mapping)
    tcc_free(cbz_dry_mapping);
  ir->codegen_cbz_dry_mapping = NULL;
  if (dry_pool_entries)
    tcc_free(dry_pool_entries);
  ir->codegen_dry_pool_entries = NULL;
  if (branch_target_reset)
    tcc_free(branch_target_reset);
  ir->codegen_branch_target_reset = NULL;

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
  {
    if (!ir->noreturn && !ir->tail_call_only)
      tcc_gen_machine_epilog(ir->leaffunc);
    else
      tcc_gen_machine_finish_noreturn();
  }
  tcc_ir_codegen_backpatch_jumps(ir, ir_to_code_mapping);

  /* Backpatch return jumps to point to epilogue */
  int epilogue_addr = ir_to_code_mapping[ir->next_instruction_index];
  for (int i = 0; i < num_return_jumps; i++)
  {
    tcc_gen_machine_backpatch_jump(return_jump_addrs[i], epilogue_addr);
  }

  tcc_free(return_jump_addrs);
  ir->codegen_return_jump_addrs = NULL;
  tcc_free(dry_insn_saves);
  ir->codegen_dry_insn_saves = NULL;
  tcc_free(dry_insn_scratch);
  ir->codegen_dry_insn_scratch = NULL;
}

/* ============================================================================
 * Legacy API Wrappers
 * ============================================================================ */

/* Note: tcc_ir_generate_code legacy wrapper remains in tccir.c */
