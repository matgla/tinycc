/*
 *  TCC IR - Main Instruction Insertion
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

/* Ensure anonymous symbols in operands are registered for ELF output */
static void ir_ensure_sym_registered(SValue *sv)
{
  if (sv && (sv->r & VT_SYM) && sv->sym)
  {
    Sym *sym = sv->sym;
    /* Check if this is an anonymous symbol that hasn't been registered yet */
    if ((sym->v & ~0x0FFFFFFF) == SYM_FIRST_ANOM && sym->c == 0)
    {
      /* Use put_extern_sym2 directly to bypass nocode_wanted check.
       * We need the symbol registered in ELF even if we're in a "nocode" section
       * because the IR instruction we're about to create will reference it later. */
      put_extern_sym2(sym, SHN_UNDEF, 0, 0, 1);
    }
  }
}

/* Check if operand is a stack address (not a value loaded from stack) */
static int ir_operand_is_stack_addr(const SValue *sv)
{
  if (!sv)
    return 0;
  int val_kind = sv->r & VT_VALMASK;
  if ((val_kind == VT_LOCAL || val_kind == VT_LLOCAL) && !(sv->r & VT_LVAL) && sv->vr == -1)
    return 1;
  return 0;
}

/* Defined in ir/gen/softfloat.c */
int ir_put_soft_call_fpu_if_needed(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest);

int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  {
    /* Suppress IR emission when nocode_wanted is set, but NOT
     * when only CODE_OFF_BIT is set.  CODE_OFF_BIT indicates dead code
     * after unconditional jumps (return/break/goto).  Unlike if(0) dead
     * branches, these paths may still contain case labels and other
     * jump targets that need IR instructions to exist for backpatching.
     * The data/code suppression for if(0) style dead branches is handled
     * by the string-literal DATA_ONLY_WANTED guard in tccgen.c and
     * the CODE_OFF in tcc_ir_codegen_test_gen. */
    if (nocode_wanted & ~0x20000000)
      return -1;
  }

  /* Ensure any anonymous symbols in the operands are registered before
   * storing them in the IR instruction. This prevents use-after-free when
   * local scopes are popped before the IR is processed. */
  ir_ensure_sym_registered(src1);
  ir_ensure_sym_registered(src2);
  ir_ensure_sym_registered(dest);

  /* Check if we need to use soft-float call instead of native FPU instruction.
   * Skip this for complex operations - they need special handling in the code generator. */
  if (tcc_ir_type_op_needs_fpu(op) && !((dest && (dest->type.t & VT_COMPLEX)) ||
                                        (src1 && (src1->type.t & VT_COMPLEX)) || (src2 && (src2->type.t & VT_COMPLEX))))
  {
    if (ir_put_soft_call_fpu_if_needed(ir, op, src1, src2, dest))
    {
      return ir->next_instruction_index;
    }
  }

  /* Resize array if needed */
  const int pos = ir->next_instruction_index;
  if (ir->next_instruction_index >= ir->compact_instructions_size)
  {
    ir->compact_instructions_size <<= 1;
    ir->compact_instructions =
        (IRQuadCompact *)tcc_realloc(ir->compact_instructions, sizeof(IRQuadCompact) * ir->compact_instructions_size);
    if (!ir->compact_instructions)
    {
      fprintf(stderr, "tcc_ir_put: out of memory (compact)\n");
      exit(1);
    }
  }

  IRQuadCompact *cq = &ir->compact_instructions[pos];
  memset(cq, 0, sizeof(IRQuadCompact));
  cq->op = (uint8_t)op;
  cq->orig_index = pos;
  if (pos > ir->max_orig_index)
    ir->max_orig_index = pos;
  if (ir->next_insn_is_jump_target)
  {
    cq->is_jump_target = 1;
    ir->next_insn_is_jump_target = 0;
  }
  cq->operand_base = ir->iroperand_pool_count;

  /* Handle destination operand */
  if (irop_config[op].has_dest == 1)
  {
    IRLiveInterval *dest_interval = NULL;
    if (dest == NULL)
    {
      fprintf(stderr, "tcc_ir_put: dest is NULL for op %s\n", tcc_ir_dump_op_name(op));
      exit(1);
    }

    if (tcc_ir_vreg_is_valid(ir, dest->vr))
    {
      if (dest->type.t == 0)
      {
        if (src1 && tcc_ir_type_is_float(src1->type.t))
        {
          dest->type = src1->type;
        }
        else if (src2 && tcc_ir_type_is_float(src2->type.t))
        {
          dest->type = src2->type;
        }
        else if (src1 && tcc_ir_type_is_64bit(src1->type.t))
        {
          dest->type = src1->type;
        }
        else if (src2 && tcc_ir_type_is_64bit(src2->type.t))
        {
          dest->type = src2->type;
        }
      }

      /* For ASSIGN (simple copy), the destination must match the source's
       * float nature.  When the caller provides a non-float dest type
       * (e.g. VT_INT for a double value), inherit the source type so that
       * the backend generates a correctly-sized load/move.
       * Note: we do NOT do this for 64-bit integer (LLONG) sources because
       * TCC can emit ASSIGN for intentional LLONG-to-INT truncation. */
      if (op == TCCIR_OP_ASSIGN && src1)
      {
        if (tcc_ir_type_is_float(src1->type.t) && !tcc_ir_type_is_float(dest->type.t))
          dest->type = src1->type;
      }

      if ((op == TCCIR_OP_SHL || op == TCCIR_OP_SHR || op == TCCIR_OP_SAR) && src1 &&
          tcc_ir_type_is_64bit(src1->type.t))
      {
        dest->type = src1->type;
      }

      /* For STORE ops the dest vreg holds a 32-bit address; dest->type
       * describes the stored value, not the pointer.  Don't promote the
       * address vreg to float/64-bit/complex. */
      int dest_is_store = (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED ||
                           op == TCCIR_OP_STORE_POSTINC);
      if (!dest_is_store) {
        if (tcc_ir_type_is_float(dest->type.t))
        {
          tcc_ir_vreg_type_set_fp(ir, dest->vr, 1, tcc_ir_type_is_double(dest->type.t));
        }
        else if ((dest->type.t & VT_BTYPE) == VT_LLONG)
        {
          tcc_ir_vreg_type_set_64bit(ir, dest->vr);
        }
        /* Phase 3: Set complex flag for complex types */
        if (dest->type.t & VT_COMPLEX)
        {
          tcc_ir_vreg_type_set_complex(ir, dest->vr);
        }
      }
      dest_interval = tcc_ir_vreg_live_interval(ir, dest->vr);
      int new_is_lvalue;
      int src_is_stack_addr = ir_operand_is_stack_addr(src1);
      if (op == TCCIR_OP_ASSIGN && src1 && !(src1->r & VT_LVAL) && !src_is_stack_addr)
      {
        new_is_lvalue = 1;
      }
      else
      {
        new_is_lvalue = 0;
      }
      dest_interval->is_lvalue = new_is_lvalue;
    }

    IROperand dest_irop = svalue_to_iroperand(ir, dest);
    tcc_ir_pool_add(ir, dest_irop);
  }

  /* Handle source 1 operand */
  if (irop_config[op].has_src1 == 1)
  {
    if (src1 == NULL)
    {
      fprintf(stderr, "tcc_ir_put: src1 is NULL for op %s\n", tcc_ir_dump_op_name(op));
      exit(1);
    }
    IROperand src1_irop = svalue_to_iroperand(ir, src1);
    tcc_ir_pool_add(ir, src1_irop);
  }

  /* Handle source 2 operand */
  if (irop_config[op].has_src2 == 1)
  {
    if (src2 == NULL)
    {
      fprintf(stderr, "tcc_ir_put: src2 is NULL for op %s\n", tcc_ir_dump_op_name(op));
      exit(1);
    }
    IROperand src2_irop = svalue_to_iroperand(ir, src2);
    tcc_ir_pool_add(ir, src2_irop);
  }

  /* Mark function as non-leaf if it makes a call */
  if ((op == TCCIR_OP_FUNCCALLVOID) || (op == TCCIR_OP_FUNCCALLVAL))
  {
    ir->leaffunc = 0;
  }

  /* LEA takes the address of src1, so mark it as address-taken */
  if (op == TCCIR_OP_LEA && src1 && tcc_ir_vreg_is_valid(ir, src1->vr))
  {
    tcc_ir_vreg_flag_addrtaken_set(ir, src1->vr);
  }

  /* Store current source line number for debug info */
  cq->line_num = file ? file->line_num : 0;

  if (ir->basic_block_start)
  {
    ir->basic_block_start = 0;
  }
  else if (op == TCCIR_OP_ASSIGN && pos > 0)
  {
    /* Try to coalesce: if assigning from a TEMP that was the dest of the previous instruction,
     * redirect that instruction's dest to our dest and skip this ASSIGN. */
    IROperand prev_dest_irop = tcc_ir_op_get_dest(ir, &ir->compact_instructions[pos - 1]);
    IROperand src1_irop = tcc_ir_op_get_src1(ir, &ir->compact_instructions[pos]);
    IROperand dest_irop = tcc_ir_op_get_dest(ir, &ir->compact_instructions[pos]);

    const int prev_dest_vr = irop_get_vreg(prev_dest_irop);
    const int prev_is_64bit = irop_is_64bit(prev_dest_irop);
    const int new_is_64bit = irop_is_64bit(dest_irop);
    const int width_match = (prev_is_64bit == new_is_64bit);
    const int can_coalesce = (!ir->prevent_coalescing) && width_match &&
                             (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src1_irop)) == TCCIR_VREG_TYPE_TEMP) &&
                             !src1_irop.is_lval && (irop_get_vreg(src1_irop) == prev_dest_vr);
    if (can_coalesce)
    {
      /* When coalescing, copy type information from old dest to new dest */
      const int new_dest_vr = irop_get_vreg(dest_irop);

      if (tcc_ir_vreg_is_valid(ir, prev_dest_vr) && tcc_ir_vreg_is_valid(ir, new_dest_vr))
      {
        IRLiveInterval *old_interval = tcc_ir_vreg_live_interval(ir, prev_dest_vr);
        IRLiveInterval *new_interval = tcc_ir_vreg_live_interval(ir, new_dest_vr);
        /* Only propagate is_llong if BOTH source and dest are 64-bit */
        if (old_interval && new_interval && old_interval->is_llong && new_is_64bit)
          new_interval->is_llong = 1;
      }

      /* Build the new previous destination IROperand */
      IROperand new_prev_dest;
      if (width_match)
      {
        new_prev_dest = prev_dest_irop;
        irop_set_vreg(&new_prev_dest, new_dest_vr);
        /* Temp locals and concrete stack slots (negative vregs) are not
         * tracked by the register allocator.  Their destinations need
         * the STACKOFF tag and frame offset from the ASSIGN's dest so
         * that fill_registers_ir recognises them as stack-relative and
         * materialize_dest_ir can compute the correct storeback offset.
         * Without this the coalesced dest keeps VREG / is_local=0 and
         * the storeback writes to frame offset 0 instead of the real
         * stack location. */
        if (new_dest_vr < 0 && irop_get_tag(dest_irop) == IROP_TAG_STACKOFF)
        {
          new_prev_dest.tag = dest_irop.tag;
          new_prev_dest.is_local = dest_irop.is_local;
          new_prev_dest.is_llocal = dest_irop.is_llocal;
          new_prev_dest.is_lval = dest_irop.is_lval;
          new_prev_dest.u = dest_irop.u;
        }
      }
      else
      {
        int prev_btype = irop_get_btype(dest_irop);
        new_prev_dest = irop_make_vreg(new_dest_vr, prev_btype);
        new_prev_dest.is_lval = prev_dest_irop.is_lval;
        new_prev_dest.is_llocal = prev_dest_irop.is_llocal;
        new_prev_dest.is_local = prev_dest_irop.is_local;
        new_prev_dest.is_const = prev_dest_irop.is_const;
        new_prev_dest.is_unsigned = prev_dest_irop.is_unsigned;
        new_prev_dest.is_static = prev_dest_irop.is_static;
        new_prev_dest.is_sym = prev_dest_irop.is_sym;
        new_prev_dest.is_param = prev_dest_irop.is_param;
        new_prev_dest.is_complex = prev_dest_irop.is_complex; /* Phase 3: preserve complex flag */
        new_prev_dest.u = prev_dest_irop.u;
      }

      /* Update the pool entry for the coalesced instruction's dest */
      IRQuadCompact *prev_cq = &ir->compact_instructions[pos - 1];
      if (irop_config[prev_cq->op].has_dest)
      {
        tcc_ir_set_dest(ir, pos - 1, new_prev_dest);
      }

      /* Don't increment - the ASSIGN at pos should be overwritten */
      return pos - 1;
    }
  }

  ir->next_instruction_index++;
  return pos;
}

int tcc_ir_put_op(TCCIRState *ir, TccIrOp op, IROperand src1, IROperand src2, IROperand dest)
{
  /* TODO: Full implementation using IROperand directly */
  (void)ir;
  (void)op;
  (void)src1;
  (void)src2;
  (void)dest;
  return 0;
}

int tcc_ir_put_no_op(TCCIRState *ir, TccIrOp op)
{
  return tcc_ir_put(ir, op, NULL, NULL, NULL);
}
