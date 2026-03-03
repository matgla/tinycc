/*
 *  TCC IR - Core Operations Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

#define IR_LIVE_INTERVAL_INIT_SIZE 64
#define QUADRUPLE_INIT_SIZE 128
/* Initialize all interval start fields to INTERVAL_NOT_STARTED and incoming_reg
 * to -1 */
static void tcc_ir_init_interval_starts(IRLiveInterval *intervals, int count)
{
  for (int i = 0; i < count; ++i)
  {
    intervals[i].start = INTERVAL_NOT_STARTED;
    intervals[i].incoming_reg0 = -1;
    intervals[i].incoming_reg1 = -1;
    intervals[i].stack_slot_index = -1;
    intervals[i].allocation.r0 = PREG_NONE;
    intervals[i].allocation.r1 = PREG_NONE;
    intervals[i].allocation.offset = 0;
  }
}

static void tcc_ir_clear_live_intervals(TCCIRState *ir)
{
  ir->variables_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  if (ir->variables_live_intervals != NULL)
  {
    tcc_free(ir->variables_live_intervals);
  }
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * IR_LIVE_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->variables_live_intervals, IR_LIVE_INTERVAL_INIT_SIZE);
  ir->next_local_variable = 0;

  ir->temporary_variables_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  if (ir->temporary_variables_live_intervals != NULL)
  {
    tcc_free(ir->temporary_variables_live_intervals);
  }
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * IR_LIVE_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->temporary_variables_live_intervals, IR_LIVE_INTERVAL_INIT_SIZE);
  ir->next_temporary_variable = 0;

  ir->parameters_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  if (ir->parameters_live_intervals != NULL)
  {
    tcc_free(ir->parameters_live_intervals);
  }

  ir->parameters_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * IR_LIVE_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->parameters_live_intervals, IR_LIVE_INTERVAL_INIT_SIZE);
  ir->next_parameter = 0;
}

TCCIRState *tcc_ir_alloc(void)
{
  TCCIRState *block = (TCCIRState *)tcc_mallocz(sizeof(TCCIRState));
  if (!block)
  {
    fprintf(stderr, "tcc_ir_allocate_block: out of memory\n");
    exit(1);
  }
  block->parameters_count = 0;
  block->named_arg_reg_bytes = 0;
  block->named_arg_stack_bytes = 0;
  block->active_set = (IRLiveInterval **)tcc_mallocz(sizeof(IRLiveInterval *) * tcc_gen_machine_number_of_registers());
  block->ir_to_code_mapping = NULL;
  block->ir_to_code_mapping_size = 0;
  block->orig_ir_to_code_mapping = NULL;
  block->orig_ir_to_code_mapping_size = 0;

  block->next_instruction_index = 0;
  /* call_id is 0-based and monotonically increasing per function. */
  block->next_call_id = 0;

  block->leaffunc = 1;
  block->processing_if = 0;
  block->basic_block_start = 1;
  block->prevent_coalescing = 0;

  /* Nested function / static chain fields */
  block->has_static_chain = 0;
  block->static_chain_vreg = 0;
  block->parent_loc = 0;

  /* Nested function tracking (for parent functions) */
  block->nested_funcs = NULL;
  block->nb_nested_funcs = 0;
  block->nested_funcs_capacity = 0;

  tcc_ir_clear_live_intervals(block);

  /* Initialize IROperand pools (i64, f64, symref) */
  tcc_ir_pools_init(block);

  /* Initialize compact instructions array */
  block->compact_instructions_size = QUADRUPLE_INIT_SIZE;
  block->compact_instructions = (IRQuadCompact *)tcc_mallocz(sizeof(IRQuadCompact) * QUADRUPLE_INIT_SIZE);
  if (!block->compact_instructions)
  {
    fprintf(stderr, "tcc_ir_allocate_block: out of memory (compact_instructions)\n");
    exit(1);
  }

  tcc_ls_initialize(&block->ls);
  block->stack_layout.slots = NULL;
  block->stack_layout.slot_capacity = 0;
  block->stack_layout.slot_count = 0;
  block->stack_layout.offset_hash_keys = NULL;
  block->stack_layout.offset_hash_values = NULL;
  block->stack_layout.offset_hash_size = 0;

#ifdef CONFIG_TCC_ASM
  block->inline_asms = NULL;
  block->inline_asm_count = 0;
  block->inline_asm_capacity = 0;
#endif

  /* Initialize optimization module data */
  block->opt_fp_mat_cache = NULL;

  return block;
}

void tcc_ir_free(TCCIRState *ir)
{
  if (!ir)
  {
    fprintf(stderr, "tcc_ir_release_block: NULL ir block\n");
    exit(1);
  }

  if (ir->active_set != NULL)
  {
    tcc_free(ir->active_set);
  }

  if (ir->ir_to_code_mapping)
  {
    tcc_free(ir->ir_to_code_mapping);
    ir->ir_to_code_mapping = NULL;
    ir->ir_to_code_mapping_size = 0;
  }

  if (ir->orig_ir_to_code_mapping)
  {
    tcc_free(ir->orig_ir_to_code_mapping);
    ir->orig_ir_to_code_mapping = NULL;
    ir->orig_ir_to_code_mapping_size = 0;
  }

  /* Free IROperand pools (i64, f64, symref, ctype) */

  /* Free IROperand pools */
  tcc_ir_pools_free(ir);

  /* Free compact instructions array */
  if (ir->compact_instructions)
  {
    tcc_free(ir->compact_instructions);
    ir->compact_instructions = NULL;
    ir->compact_instructions_size = 0;
  }

#ifdef CONFIG_TCC_ASM
  if (ir->inline_asms)
  {
    for (int i = 0; i < ir->inline_asm_count; ++i)
    {
      TCCIRInlineAsm *ia = &ir->inline_asms[i];
      if (ia->asm_str)
        tcc_free(ia->asm_str);
      ia->asm_str = NULL;
      if (ia->operands)
        tcc_free(ia->operands);
      ia->operands = NULL;
      if (ia->values)
        tcc_free(ia->values);
      ia->values = NULL;
    }
    tcc_free(ir->inline_asms);
  }
  ir->inline_asms = NULL;
  ir->inline_asm_count = 0;
  ir->inline_asm_capacity = 0;
#endif

  if (ir->variables_live_intervals != NULL)
  {
    tcc_free(ir->variables_live_intervals);
  }
  if (ir->temporary_variables_live_intervals != NULL)
  {
    tcc_free(ir->temporary_variables_live_intervals);
  }
  if (ir->parameters_live_intervals != NULL)
  {
    tcc_free(ir->parameters_live_intervals);
  }

  if (ir->stack_layout.slots != NULL)
  {
    tcc_free(ir->stack_layout.slots);
    ir->stack_layout.slots = NULL;
    ir->stack_layout.slot_capacity = 0;
    ir->stack_layout.slot_count = 0;
  }

  if (ir->stack_layout.offset_hash_keys)
  {
    tcc_free(ir->stack_layout.offset_hash_keys);
    ir->stack_layout.offset_hash_keys = NULL;
  }
  if (ir->stack_layout.offset_hash_values)
  {
    tcc_free(ir->stack_layout.offset_hash_values);
    ir->stack_layout.offset_hash_values = NULL;
  }
  ir->stack_layout.offset_hash_size = 0;

  tcc_ls_deinitialize(&ir->ls);

  /* Free optimization module data */
  tcc_ir_opt_fp_cache_free(ir);

  /* Free switch tables */
  if (ir->switch_tables)
  {
    for (int i = 0; i < ir->num_switch_tables; i++)
      tcc_free(ir->switch_tables[i].targets);
    tcc_free(ir->switch_tables);
    ir->switch_tables = NULL;
    ir->num_switch_tables = 0;
    ir->switch_tables_capacity = 0;
  }

  /* Free nested_funcs array (note: NestedFunc structs themselves are owned by TCCState) */
  if (ir->nested_funcs)
  {
    tcc_free(ir->nested_funcs);
    ir->nested_funcs = NULL;
    ir->nb_nested_funcs = 0;
    ir->nested_funcs_capacity = 0;
  }

  tcc_free(ir);
}

void tcc_ir_reset(TCCIRState *ir)
{
  /* TODO: Implement IR reset for reuse */
  (void)ir;
}

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

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

/* Forward declaration for soft call FPU check */
static int ir_put_soft_call_fpu_if_needed(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest);

/* ============================================================================
 * Main IR Instruction Insertion
 * ============================================================================ */

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

      if ((op == TCCIR_OP_SHL || op == TCCIR_OP_SHR || op == TCCIR_OP_SAR) && src1 &&
          tcc_ir_type_is_64bit(src1->type.t))
      {
        dest->type = src1->type;
      }

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

/* Forward declarations for internal helpers */
static void tcc_ir_params_add_hidden_sret(TCCIRState *ir, CType *func_type);
static void tcc_ir_params_process_arguments(TCCIRState *ir, Sym *param_list, TCCAbiCallLayout *call_layout);

void tcc_ir_params_add(TCCIRState *ir, CType *func_type)
{
  TCCAbiCallLayout call_layout;
  Sym *sym = func_type->ref;
  int variadic = (sym->f.func_type == FUNC_ELLIPSIS);

  /* Initialize layout for argument classification */
  memset(&call_layout, 0, sizeof(call_layout));

  /* Set up local variable area - variadic functions need extra space */
  loc = variadic ? -28 : 0;
  func_vc = 0;

  /* Handle hidden sret pointer for struct returns */
  if ((sym->type.t & VT_BTYPE) == VT_STRUCT)
  {
    tcc_ir_params_add_hidden_sret(ir, func_type);
    /* If sret was used (func_vc != 0), the hidden pointer consumed r0
     * per AAPCS. Advance the ABI layout so that explicit arguments
     * are classified starting from r1, not r0. Without this, all
     * parameters are off-by-one: the last register param is
     * misclassified as in-register when it is actually on the stack,
     * and the backend generates ADD (address) instead of LDR (value). */
    if (func_vc != 0)
      call_layout.next_reg = 1;
  }

  /* Process function parameters */
  tcc_ir_params_process_arguments(ir, sym->next, &call_layout);

  tcc_abi_call_layout_deinit(&call_layout);
}

static void tcc_ir_params_add_hidden_sret(TCCIRState *ir, CType *func_type)
{
  CType ret_type;
  int ret_align, regsize;
  Sym *sym = func_type->ref;

  int ret_nregs = gfunc_sret(&sym->type, (sym->f.func_type == FUNC_ELLIPSIS), &ret_type, &ret_align, &regsize);

  if (ret_nregs == 0)
  {
    /* Struct returned via hidden pointer in first parameter (r0) */
    SValue src, dst;

    loc = (loc - PTR_SIZE) & -PTR_SIZE;
    func_vc = loc;
    tcc_state->need_frame_pointer = 1;

    /* Consume a PARAM vreg for the hidden sret pointer */
    int sret_param_vr = tcc_ir_get_vreg_param(ir);

    /* Store the sret pointer to the local slot */
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.type.t = VT_PTR;
    src.r = 0;
    src.vr = sret_param_vr;
    dst.type.t = VT_PTR;
    dst.r = VT_LOCAL | VT_LVAL;
    dst.vr = -1;
    dst.c.i = func_vc;
    tcc_ir_put(ir, TCCIR_OP_STORE, &src, NULL, &dst);
  }
}

static void tcc_ir_params_process_arguments(TCCIRState *ir, Sym *param_list, TCCAbiCallLayout *call_layout)
{
  int arg_index = 0;
  int arg_count = 0;
  Sym *sym;

  /* Count arguments */
  for (sym = param_list; sym; sym = sym->next)
    arg_count++;

  if (arg_count > 0)
    tcc_abi_call_layout_ensure_capacity(call_layout, arg_count);

  if (ir)
  {
    ir->parameters_count = (int8_t)arg_count;
    ir->named_arg_reg_bytes = 0;
    ir->named_arg_stack_bytes = 0;
  }

  /* Process each parameter */
  for (sym = param_list; sym; sym = sym->next, ++arg_index)
  {
    tcc_ir_params_process_single(ir, sym, arg_index, call_layout);
  }
}

void tcc_ir_params_process_single(TCCIRState *ir, Sym *sym, int arg_index, TCCAbiCallLayout *call_layout)
{
  CType *type = &sym->type;
  int size = 0, align = 0;

  size = type_size(type, &align);
  if (align < 1)
    align = 1;

  TCCAbiArgDesc desc;
  memset(&desc, 0, sizeof(desc));

  if ((type->t & VT_BTYPE) == VT_STRUCT)
  {
    desc.kind = TCC_ABI_ARG_STRUCT_BYVAL;
    desc.size = (uint16_t)size;
    desc.alignment = (uint8_t)align;
  }
  else if (tcc_ir_type_is_64bit(type->t))
  {
    desc.kind = TCC_ABI_ARG_SCALAR64;
    desc.size = 8;
    desc.alignment = (uint8_t)align;
  }
  else
  {
    desc.kind = TCC_ABI_ARG_SCALAR32;
    desc.size = 4;
    desc.alignment = (uint8_t)align;
  }

  TCCAbiArgLoc loc_info = tcc_abi_classify_argument(call_layout, arg_index, &desc);
  tcc_ir_params_update_tracking(ir, loc_info, call_layout);

  if (loc_info.kind == TCC_ABI_LOC_STACK || loc_info.kind == TCC_ABI_LOC_REG_STACK)
    tcc_state->need_frame_pointer = 1;

  if ((type->t & VT_BTYPE) == VT_STRUCT)
  {
    tcc_ir_params_process_struct(ir, sym, type, size, align, &loc_info, call_layout, arg_index);
  }
  else
  {
    tcc_ir_params_process_scalar(ir, sym, type, &loc_info);
  }
}

void tcc_ir_params_update_tracking(TCCIRState *ir, TCCAbiArgLoc loc_info, TCCAbiCallLayout *layout)
{
  if (!ir)
    return;

  if (loc_info.kind == TCC_ABI_LOC_REG)
  {
    int bytes = (loc_info.reg_base + loc_info.reg_count) * 4;
    if (bytes > ir->named_arg_reg_bytes)
      ir->named_arg_reg_bytes = bytes;
  }
  else if (loc_info.kind == TCC_ABI_LOC_REG_STACK)
  {
    int reg_bytes = (loc_info.reg_base + loc_info.reg_count) * 4;
    if (reg_bytes > ir->named_arg_reg_bytes)
      ir->named_arg_reg_bytes = reg_bytes;
    int stack_end = loc_info.stack_off + loc_info.stack_size;
    if (stack_end > ir->named_arg_stack_bytes)
      ir->named_arg_stack_bytes = stack_end;
  }
  else
  {
    int end = loc_info.stack_off + loc_info.size;
    if (end > ir->named_arg_stack_bytes)
      ir->named_arg_stack_bytes = end;
  }

  /* Also account for registers consumed (or skipped) by alignment.
   * When e.g. a long long causes r3 to be skipped (AAPCS 8-byte alignment),
   * the argument goes to stack but next_reg advances to 4.  Without this,
   * named_arg_reg_bytes would be too low and va_start would incorrectly
   * try to read the skipped register slot as a variadic argument. */
  if (layout)
  {
    int consumed = layout->next_reg * 4;
    if (consumed > ir->named_arg_reg_bytes)
      ir->named_arg_reg_bytes = consumed;
  }
}

void tcc_ir_params_process_struct(TCCIRState *ir, Sym *sym, CType *type, int size, int align, TCCAbiArgLoc *loc_info,
                                  TCCAbiCallLayout *call_layout, int arg_index)
{
  const int invisible_ref =
      (call_layout->arg_flags && (call_layout->arg_flags[arg_index] & TCC_ABI_ARG_FLAG_INVISIBLE_REF));
  int slot_align = align < 4 ? 4 : align;
  int flags = 0, addr = 0;

  if (invisible_ref)
  {
    /* Large struct passed as hidden pointer */
    loc = (loc - PTR_SIZE) & -PTR_SIZE;
    const int ptr_slot = loc;
    const int ptr_param_vr = tcc_ir_get_vreg_param(ir);

    SValue src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.type.t = VT_PTR;
    src.r = 0;
    src.vr = ptr_param_vr;
    dst.type.t = VT_PTR;
    dst.r = VT_LOCAL | VT_LVAL;
    dst.vr = -1;
    dst.c.i = ptr_slot;
    tcc_ir_put(ir, TCCIR_OP_STORE, &src, NULL, &dst);

    flags = VT_LVAL | VT_LLOCAL;
    addr = ptr_slot;
    sym_push(sym->v & ~SYM_FIELD, type, flags, addr);
    return;
  }

  if (loc_info->kind == TCC_ABI_LOC_REG)
  {
    /* Struct passed in registers - spill to local home */
    int slot_size = tcc_abi_align_up_int(size, 4);
    loc = (loc - slot_size) & -slot_align;
    const int struct_slot = loc;
    const int word_count = (slot_size + 3) / 4;

    for (int w = 0; w < word_count; ++w)
    {
      const int word_param_vr = tcc_ir_get_vreg_param(ir);
      SValue src, dst;
      memset(&src, 0, sizeof(src));
      memset(&dst, 0, sizeof(dst));
      src.type.t = VT_INT;
      src.r = 0;
      src.vr = word_param_vr;
      dst.type.t = VT_INT;
      dst.r = VT_LOCAL | VT_LVAL;
      dst.vr = -1;
      dst.c.i = struct_slot + w * 4;
      tcc_ir_put(ir, TCCIR_OP_STORE, &src, NULL, &dst);
    }

    flags = VT_LVAL | VT_LOCAL;
    addr = struct_slot;
    sym_push(sym->v & ~SYM_FIELD, type, flags, addr);
    return;
  }

  if (loc_info->kind == TCC_ABI_LOC_REG_STACK)
  {
    /* Struct straddles registers and stack */
    int slot_size = tcc_abi_align_up_int(size, 4);
    loc = (loc - slot_size) & -slot_align;
    const int struct_slot = loc;
    const int total_words = (slot_size + 3) / 4;
    const int reg_words = loc_info->reg_count;
    const int stack_words = total_words - reg_words;

    /* Spill register words from PARAM vregs */
    for (int w = 0; w < reg_words; ++w)
    {
      const int word_param_vr = tcc_ir_get_vreg_param(ir);
      SValue src, dst;
      memset(&src, 0, sizeof(src));
      memset(&dst, 0, sizeof(dst));
      src.type.t = VT_INT;
      src.r = 0;
      src.vr = word_param_vr;
      dst.type.t = VT_INT;
      dst.r = VT_LOCAL | VT_LVAL;
      dst.vr = -1;
      dst.c.i = struct_slot + w * 4;
      tcc_ir_put(ir, TCCIR_OP_STORE, &src, NULL, &dst);
    }

    /* Copy stack words from caller argument area */
    for (int w = 0; w < stack_words; ++w)
    {
      SValue src, dst, tmp;
      memset(&src, 0, sizeof(src));
      memset(&dst, 0, sizeof(dst));
      memset(&tmp, 0, sizeof(tmp));

      int temp_vr = tcc_ir_get_vreg_temp(ir);

      src.type.t = VT_INT;
      src.r = VT_PARAM | VT_LVAL | VT_LOCAL;
      src.vr = -1;
      src.c.i = loc_info->stack_off + w * 4;

      tmp.type.t = VT_INT;
      tmp.r = 0;
      tmp.vr = temp_vr;

      tcc_ir_put(ir, TCCIR_OP_LOAD, &src, NULL, &tmp);

      dst.type.t = VT_INT;
      dst.r = VT_LOCAL | VT_LVAL;
      dst.vr = -1;
      dst.c.i = struct_slot + (reg_words + w) * 4;

      tmp.r = 0;
      tcc_ir_put(ir, TCCIR_OP_STORE, &tmp, NULL, &dst);
    }

    flags = VT_LVAL | VT_LOCAL;
    addr = struct_slot;
    sym_push(sym->v & ~SYM_FIELD, type, flags, addr);
    return;
  }

  /* Struct passed on stack */
  flags = VT_PARAM | VT_LVAL | VT_LOCAL;
  addr = loc_info->stack_off;
  sym_push(sym->v & ~SYM_FIELD, type, flags, addr);
}

void tcc_ir_params_process_scalar(TCCIRState *ir, Sym *sym, CType *type, TCCAbiArgLoc *loc_info)
{
  int flags = 0, addr = 0;
  int variadic = (sym->f.func_type == FUNC_ELLIPSIS);

  if (loc_info->kind == TCC_ABI_LOC_REG)
  {
    flags = VT_PARAM | VT_LVAL;
    if (variadic)
    {
      addr = -16 + (loc_info->reg_base * 4);
      flags |= VT_LOCAL;
    }
    else
    {
      addr = 0;
    }
  }
  else
  {
    flags = VT_PARAM | VT_LVAL | VT_LOCAL;
    addr = loc_info->stack_off;
  }

  sym->r |= ~(VT_LVAL | VT_LLOCAL);
  sym_push(sym->v & ~SYM_FIELD, type, flags, addr);
}

int tcc_ir_local_add(TCCIRState *ir, Sym *sym, int stack_offset)
{
  int align, size, addr;
  CType *type = &sym->type;

  (void)ir;
  (void)stack_offset;

  size = type_size(type, &align);
  if (align < 1)
    align = 1;

  /* Align stack location */
  loc = (loc - size) & -align;
  addr = loc;

  /* Push symbol with computed location */
  sym_push(sym->v & ~SYM_FIELD, type, VT_LOCAL | VT_LVAL, addr);

  return addr;
}

/* Arithmetic operations */
void tcc_ir_gen_add(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, '+');
}
void tcc_ir_gen_sub(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, '-');
}
void tcc_ir_gen_mul(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, '*');
}
void tcc_ir_gen_div(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, '/');
}
void tcc_ir_gen_mod(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, '%');
}
void tcc_ir_gen_and(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, '&');
}
void tcc_ir_gen_or(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, '|');
}
void tcc_ir_gen_xor(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, '^');
}
void tcc_ir_gen_shl(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, TOK_SHL);
}
void tcc_ir_gen_shr(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, TOK_SHR);
}
void tcc_ir_gen_sar(TCCIRState *ir)
{
  tcc_ir_gen_i(ir, TOK_SAR);
}

/* FP operations */
void tcc_ir_gen_fadd(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, '+');
}
void tcc_ir_gen_fsub(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, '-');
}
void tcc_ir_gen_fmul(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, '*');
}

/* ============================================================================
 * Token to IR Operation Mapping
 * ============================================================================ */

TccIrOp tcc_irop_from_token(int token)
{
  switch (token)
  {
  case '+':
    return TCCIR_OP_ADD;
  case TOK_ADDC1:
    return TCCIR_OP_ADC_GEN;
  case TOK_ADDC2:
    return TCCIR_OP_ADC_USE;
  case '-':
    return TCCIR_OP_SUB;
  case TOK_SUBC1:
    return TCCIR_OP_SUBC_GEN;
  case TOK_SUBC2:
    return TCCIR_OP_SUBC_USE;
  case '&':
    return TCCIR_OP_AND;
  case '^':
    return TCCIR_OP_XOR;
  case '|':
    return TCCIR_OP_OR;
  case '*':
    return TCCIR_OP_MUL;
  case TOK_UMULL:
    return TCCIR_OP_UMULL;
  case TOK_SHL:
    return TCCIR_OP_SHL;
  case TOK_SAR:
    return TCCIR_OP_SAR;
  case TOK_SHR:
    return TCCIR_OP_SHR;
  case '/':
    return TCCIR_OP_DIV;
  case TOK_PDIV:
    return TCCIR_OP_DIV;
  case TOK_UDIV:
    return TCCIR_OP_UDIV;
  case '%':
    return TCCIR_OP_IMOD;
  case TOK_UMOD:
    return TCCIR_OP_UMOD;
  case TOK_EQ:
  case TOK_NE:
  case TOK_LT:
  case TOK_GT:
  case TOK_LE:
  case TOK_GE:
  case TOK_ULT:
  case TOK_UGT:
  case TOK_ULE:
  case TOK_UGE:
    return TCCIR_OP_CMP;
  default:
    fprintf(stderr, "tcc_irop_from_token: unknown token %d(0x%x)\n", token, token);
    exit(1);
    return TCCIR_OP_NOP; /* unreachable, silences warning */
  };
}

/* ============================================================================
 * Core IR Generation Functions
 * ============================================================================ */

void tcc_ir_gen_i(TCCIRState *ir, int op)
{
  const TccIrOp ir_op = tcc_irop_from_token(op);
  SValue dest;

  if (ir_op == TCCIR_OP_CMP)
  {
    tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], NULL);
    --vtop;
    vtop->r = VT_CMP;
    vtop->cmp_op = op;
    vtop->jfalse = -1; /* -1 = no chain */
    vtop->jtrue = -1;  /* -1 = no chain */
    return;
  }

  svalue_init(&dest);
  dest.vr = tcc_ir_get_vreg_temp(ir);
  dest.r = 0;
  /* Most integer ops preserve the operand type, but UMULL produces a 64-bit result. */
  if (ir_op == TCCIR_OP_UMULL)
  {
    dest.type.t = VT_LLONG | VT_UNSIGNED;
    tcc_ir_set_llong_type(ir, dest.vr);
  }
  else
  {
    dest.type.t = vtop[-1].type.t;
  }
  tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], &dest);
  vtop[-1].vr = dest.vr;
  vtop[-1].r = 0;
  vtop[-1].type = dest.type; /* Update type - critical for UMULL which produces 64-bit from 32-bit inputs */
  --vtop;
}

void tcc_ir_gen_f(TCCIRState *ir, int op)
{
  TccIrOp ir_op;
  SValue dest;
  int is_double;

  /* Determine the IR operation based on token */
  switch (op)
  {
  case '+':
    ir_op = TCCIR_OP_FADD;
    break;
  case '-':
    ir_op = TCCIR_OP_FSUB;
    break;
  case '*':
    ir_op = TCCIR_OP_FMUL;
    break;
  case '/':
    ir_op = TCCIR_OP_FDIV;
    break;
  case 'n': /* negation */
    ir_op = TCCIR_OP_FNEG;
    break;
  case 'c': /* compare */
    ir_op = TCCIR_OP_FCMP;
    tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], NULL);
    --vtop;
    vtop->r = VT_CMP;
    vtop->cmp_op = TOK_LT; /* default, will be fixed up later */
    vtop->jfalse = -1;     /* -1 = no chain */
    vtop->jtrue = -1;      /* -1 = no chain */
    return;
  case 't': /* float-to-float conversion */
    ir_op = TCCIR_OP_CVT_FTOF;
    break;
  case 'i': /* int-to-float conversion */
    ir_op = TCCIR_OP_CVT_ITOF;
    break;
  case 'f': /* float-to-int conversion */
    ir_op = TCCIR_OP_CVT_FTOI;
    break;
  default:
    /* Comparison operations */
    if (op >= TOK_ULT && op <= TOK_GT)
    {
      ir_op = TCCIR_OP_FCMP;
      tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], NULL);
      --vtop;
      vtop->r = VT_CMP;
      vtop->cmp_op = op;
      vtop->jfalse = -1; /* -1 = no chain */
      vtop->jtrue = -1;  /* -1 = no chain */
      return;
    }
    tcc_error("tcc_ir_gen_f: unknown floating point operation: 0x%x", op);
    return;
  }

  /* Handle negation (unary) */
  if (ir_op == TCCIR_OP_FNEG)
  {
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.r = 0;
    dest.type = vtop->type;
    /* Mark temp as float/double */
    is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
    tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
    tcc_ir_put(ir, ir_op, &vtop[0], NULL, &dest);
    vtop->vr = dest.vr;
    vtop->r = 0;
    return;
  }

  /* Check if this is a complex addition/subtraction operation */
  int is_complex_op = ((vtop[-1].type.t & VT_COMPLEX) || (vtop[0].type.t & VT_COMPLEX));

  if (is_complex_op &&
      (ir_op == TCCIR_OP_FADD || ir_op == TCCIR_OP_FSUB || ir_op == TCCIR_OP_FMUL || ir_op == TCCIR_OP_FDIV))
  {
    /* Phase 3: Complex addition/subtraction
     * For complex: (a+bi) + (c+di) = (a+c) + (b+d)i
     * We generate two FP operations and use a single vr to track the result.
     * The code generator (arm-thumb-gen.c) will recognize complex operands
     * and emit two soft-float library calls.
     */
    int base_type = vtop[-1].type.t & VT_BTYPE;

    /* Create destination SValue with complex type */
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.r = 0;
    dest.type.t = (base_type | VT_COMPLEX);

    /* Mark as float type (not double) for register allocation */
    is_double = (base_type == VT_DOUBLE || base_type == VT_LDOUBLE);
    tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
    /* Phase 3: Mark as complex type so register allocator allocates pairs */
    tcc_ir_vreg_type_set_complex(ir, dest.vr);

    /* Generate a single complex operation - the code generator will
     * recognize the complex type and emit two soft-float calls */
    tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], &dest);

    vtop[-1].vr = dest.vr;
    vtop[-1].r = 0;
    vtop[-1].type.t = dest.type.t;
    --vtop;
    return;
  }

  /* Binary FP operations and conversions */
  svalue_init(&dest);
  dest.vr = tcc_ir_get_vreg_temp(ir);
  dest.r = 0;
  if (ir_op == TCCIR_OP_CVT_ITOF || ir_op == TCCIR_OP_CVT_FTOI || ir_op == TCCIR_OP_CVT_FTOF)
  {
    /* For conversions, dest type depends on the operation */
    if (ir_op == TCCIR_OP_CVT_ITOF)
    {
      /* int to float: result is float type of destination */
      dest.type = vtop->type;
      is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
      tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
    }
    else if (ir_op == TCCIR_OP_CVT_FTOI)
    {
      /* float to int: result is int type */
      dest.type.t = VT_INT;
    }
    else /* TCCIR_OP_CVT_FTOF */
    {
      /* float-to-float: result is destination type */
      dest.type = vtop->type;
      is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
      tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
    }
  }
  else
  {
    dest.type = vtop[-1].type;
    /* Mark temp as float/double */
    is_double = (vtop[-1].type.t & VT_BTYPE) == VT_DOUBLE || (vtop[-1].type.t & VT_BTYPE) == VT_LDOUBLE;
    tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
  }
  tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], &dest);
  if (ir_op == TCCIR_OP_CVT_ITOF || ir_op == TCCIR_OP_CVT_FTOI || ir_op == TCCIR_OP_CVT_FTOF)
  {
    vtop->vr = dest.vr;
    vtop->r = 0;
    vtop->type = dest.type;
  }
  else
  {
    vtop[-1].vr = dest.vr;
    vtop[-1].r = 0;
    --vtop;
  }
}

void tcc_ir_gen_fdiv(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, '/');
}
void tcc_ir_gen_fneg(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, 'n');
}
void tcc_ir_gen_fcmp(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, 'c');
}

/* Conversions */
void tcc_ir_gen_cvt_ftof(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, 't');
}
void tcc_ir_gen_cvt_itof(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, 'i');
}
void tcc_ir_gen_cvt_ftoi(TCCIRState *ir)
{
  tcc_ir_gen_f(ir, 'f');
}

/* Control flow */
int tcc_ir_gen_test(TCCIRState *ir, int invert, int t)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)invert;
  (void)t;
  return 0;
}

int tcc_ir_gen_jmp(TCCIRState *ir)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  return 0;
}

void tcc_ir_gen_ijmp(TCCIRState *ir, SValue *target)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)target;
}

void tcc_ir_gen_return_void(TCCIRState *ir)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
}

void tcc_ir_gen_return_value(TCCIRState *ir, SValue *val)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)val;
}

/* Comparison */
void tcc_ir_gen_cmp(TCCIRState *ir, int op)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)op;
}

void tcc_ir_gen_setif(TCCIRState *ir, int condition)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)condition;
}

/* Memory operations */
void tcc_ir_gen_load(TCCIRState *ir, CType *type)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)type;
}

void tcc_ir_gen_store(TCCIRState *ir, CType *type)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)type;
}

void tcc_ir_gen_lea(TCCIRState *ir)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
}

/* Function calls */
void tcc_ir_gen_call_void(TCCIRState *ir, SValue *func)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)func;
}

void tcc_ir_gen_call_value(TCCIRState *ir, SValue *func, CType *ret_type)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)func;
  (void)ret_type;
}

void tcc_ir_gen_param_void(TCCIRState *ir, SValue *val)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)val;
}

void tcc_ir_gen_param_value(TCCIRState *ir, SValue *val)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)val;
}

int tcc_ir_gen_soft_call_fpu(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)op;
  (void)src1;
  (void)src2;
  (void)dest;
  return 0;
}

void tcc_ir_gen_soft_call(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)op;
  (void)src1;
  (void)src2;
  (void)dest;
}

void tcc_ir_return_drop(TCCIRState *ir)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
}

/* Boolean operations */
void tcc_ir_gen_bool_or(TCCIRState *ir)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
}

void tcc_ir_gen_bool_and(TCCIRState *ir)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
}

void tcc_ir_gen_test_zero(TCCIRState *ir, SValue *val)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)val;
}

/* VLA support */
void tcc_ir_gen_vla_alloc(TCCIRState *ir, SValue *size)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)size;
}

void tcc_ir_gen_vla_sp_save(TCCIRState *ir, int slot)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)slot;
}

void tcc_ir_gen_vla_sp_restore(TCCIRState *ir, int slot)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)slot;
}

/* Utility functions */
int tcc_ir_count(TCCIRState *ir)
{
  return ir ? ir->next_instruction_index : 0;
}

int tcc_ir_current_idx(TCCIRState *ir)
{
  return ir ? ir->next_instruction_index - 1 : -1;
}

int tcc_ir_is_leaf(TCCIRState *ir)
{
  return ir ? ir->leaffunc : 0;
}

void tcc_ir_nonleaf_mark(TCCIRState *ir)
{
  if (ir)
    ir->leaffunc = 0;
}

int tcc_ir_call_id_next(TCCIRState *ir)
{
  if (!ir)
    return 0;
  return ir->next_call_id++;
}

/* ============================================================================
 * Jump Chain Management
 * ============================================================================ */

void tcc_ir_backpatch(TCCIRState *ir, int t, int target_address)
{
  IROperand cur;
  int next;
  if (t < 0)
    return; /* -1 means no chain */

  while (t >= 0 && t < ir->next_instruction_index)
  {
    TccIrOp op = ir->compact_instructions[t].op;

    /* Check if this instruction is actually a jump */
    if (op != TCCIR_OP_JUMP && op != TCCIR_OP_JUMPIF)
    {
      break; /* Don't corrupt non-jump instructions */
    }

    cur = tcc_ir_op_get_dest(ir, &ir->compact_instructions[t]);
    next = cur.u.imm32;
    cur.u.imm32 = target_address;

    /* Sync to iroperand_pool as well to keep both pools in sync */
    const int pool_off = ir->compact_instructions[t].operand_base;
    ir->iroperand_pool[pool_off] = cur;

    /* Chain ends when next is -1 (sentinel), out of range, or already patched */
    if (next < 0 || next >= ir->next_instruction_index || next == target_address)
      break;
    t = next;
  }
}

void tcc_ir_backpatch_to_here(TCCIRState *ir, int t)
{
  if (!ir)
    return;
  tcc_ir_backpatch(ir, t, ir->next_instruction_index);
  /* A backpatch target is a new basic block boundary — multiple control flow
   * paths converge here. Mark it so that tcc_ir_put() does NOT coalesce the
   * next ASSIGN with the previous instruction, which may belong to a
   * different branch. Without this, ternary operators like
   *   var = cond ? true_expr : false_expr
   * can have their merge-point ASSIGN coalesced into the true-path LOAD,
   * leaving the false-path result disconnected from the variable. */
  if (t >= 0)
  {
    ir->basic_block_start = 1;
    /* Must match CODE_ON() in tccgen.c: clear the CODE_OFF_BIT so that
     * subsequent code is not treated as unreachable.  Without this, a
     * ternary inside a while loop (which uses gjmp → CODE_OFF) leaves the
     * bit set and causes a following switch statement to skip its entire
     * dispatch because sw->nocode_wanted is captured as non-zero.  This
     * mirrors the gsym() function which calls CODE_ON() after patching. */
    nocode_wanted &= ~0x20000000; /* CODE_OFF_BIT */
  }
}

void tcc_ir_backpatch_first(TCCIRState *ir, int t, int target_address)
{
  int lp, next;
  if (t < 0)
    return; /* -1 means no chain */
  do
  {
    lp = t;
    next = tcc_ir_op_get_dest(ir, &ir->compact_instructions[t]).u.imm32;
    /* Stop if we hit end of chain or go out of bounds */
    if (next < 0 || next >= ir->next_instruction_index)
      break;
    t = next;
  } while (1);
  tcc_ir_pool_jump_target_set(ir, lp, target_address);
}

int tcc_ir_gjmp_append(TCCIRState *ir, int n, int t)
{
  if (n >= 0 && n < ir->next_instruction_index)
  {
    tcc_ir_backpatch_first(ir, n, t);
    return n;
  }
  return t;
}

/* ============================================================================
 * Inline Assembly
 * ============================================================================ */

#ifdef CONFIG_TCC_ASM

/* Ensure inline asm array has capacity for needed elements */
static void tcc_ir_inline_asms_ensure_capacity(TCCIRState *ir, int needed)
{
  if (!ir)
    return;
  if (ir->inline_asm_capacity >= needed)
    return;
  int new_cap = ir->inline_asm_capacity ? ir->inline_asm_capacity : 8;
  while (new_cap < needed)
    new_cap <<= 1;
  ir->inline_asms = tcc_realloc(ir->inline_asms, sizeof(TCCIRInlineAsm) * new_cap);
  memset(ir->inline_asms + ir->inline_asm_capacity, 0, sizeof(TCCIRInlineAsm) * (new_cap - ir->inline_asm_capacity));
  ir->inline_asm_capacity = new_cap;
}

/* Add inline assembly block, return ID */
int tcc_ir_asm_add(TCCIRState *ir, const char *asm_str, int asm_len, int must_subst, ASMOperand *operands,
                   int nb_operands, int nb_outputs, int nb_labels, const uint8_t *clobber_regs)
{
  if (!ir)
    return -1;
  if (!asm_str || asm_len < 0)
    tcc_error("IR: invalid inline asm string");
  if (nb_operands < 0 || nb_operands > MAX_ASM_OPERANDS)
    tcc_error("IR: invalid asm operand count");
  if (nb_labels < 0 || nb_operands + nb_labels > MAX_ASM_OPERANDS)
    tcc_error("IR: invalid asm label count");
  if (nb_outputs < 0 || nb_outputs > nb_operands)
    tcc_error("IR: invalid asm output count");

  tcc_ir_inline_asms_ensure_capacity(ir, ir->inline_asm_count + 1);
  const int id = ir->inline_asm_count++;
  TCCIRInlineAsm *ia = &ir->inline_asms[id];

  ia->asm_len = asm_len;
  ia->asm_str = tcc_mallocz((size_t)asm_len + 1);
  memcpy(ia->asm_str, asm_str, (size_t)asm_len);
  ia->must_subst = must_subst;
  ia->nb_operands = nb_operands;
  ia->nb_outputs = nb_outputs;
  ia->nb_labels = nb_labels;
  if (clobber_regs)
    memcpy(ia->clobber_regs, clobber_regs, NB_ASM_REGS);
  else
    memset(ia->clobber_regs, 0, NB_ASM_REGS);

  ia->operands = tcc_mallocz(sizeof(ASMOperand) * (nb_operands + nb_labels));
  memcpy(ia->operands, operands, sizeof(ASMOperand) * (nb_operands + nb_labels));

  ia->values = tcc_mallocz(sizeof(SValue) * nb_operands);
  for (int i = 0; i < nb_operands; ++i)
  {
    if (!operands[i].vt)
      tcc_error("IR: asm operand missing value");
    ia->values[i] = *operands[i].vt;
    ia->operands[i].vt = &ia->values[i];
  }
  for (int i = nb_operands; i < nb_operands + nb_labels; ++i)
  {
    ia->operands[i].vt = NULL;
  }

  /* Conservative: inline asm is call-like for leaf analysis. */
  ir->leaffunc = 0;

  return id;
}

/* Put inline assembly instruction */
void tcc_ir_asm_put(TCCIRState *ir, int asm_id)
{
  if (!ir)
    return;
  SValue id_sv = tcc_svalue_const_i64(asm_id);
  (void)tcc_ir_put(ir, TCCIR_OP_INLINE_ASM, &id_sv, NULL, NULL);
  ir->leaffunc = 0;
}

#endif /* CONFIG_TCC_ASM */

/* ============================================================================
 * Legacy API Wrappers
 * ============================================================================ */

#ifdef CONFIG_TCC_ASM

/* Legacy wrapper for tcc_ir_asm_add */
int tcc_ir_add_inline_asm(TCCIRState *ir, const char *asm_str, int asm_len, int must_subst, ASMOperand *operands,
                          int nb_operands, int nb_outputs, int nb_labels, const uint8_t *clobber_regs)
{
  return tcc_ir_asm_add(ir, asm_str, asm_len, must_subst, operands, nb_operands, nb_outputs, nb_labels, clobber_regs);
}

/* Legacy wrapper for tcc_ir_asm_put */
void tcc_ir_put_inline_asm(TCCIRState *ir, int inline_asm_id)
{
  tcc_ir_asm_put(ir, inline_asm_id);
}

#endif /* CONFIG_TCC_ASM */

/* ============================================================================
 * Soft-Float Call Support
 * ============================================================================ */

/* Forward declaration for ABI soft-call name lookup */
extern const char *tcc_get_abi_softcall_name(SValue *src1, SValue *src2, SValue *dest, TccIrOp op);

/* Put a soft-float library call for FPU operations not supported by hardware */
void tcc_ir_put_soft_call(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  SValue param;
  Sym *sym;
  const int call_id = ir ? ir->next_call_id++ : 0;
  const char *func_name = NULL;

  func_name = tcc_get_abi_softcall_name(src1, src2, dest, op);
  if (func_name == NULL)
  {
    tcc_error("No soft-float ABI function for operation %s\n", tcc_ir_dump_op_name(op));
    return;
  }
  svalue_init(&param);
  param.r = VT_CONST;
  int argc = 0;
  if (irop_config[op].has_src1)
  {
    param.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
    tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, src1, &param, NULL);
    argc++;
  }
  if (irop_config[op].has_src2)
  {
    param.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
    tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, src2, &param, NULL);
    argc++;
  }
  sym = external_global_sym(tok_alloc_const(func_name), &func_old_type);
  param.r = VT_CONST | VT_SYM;
  param.sym = sym;
  param.c.i = 0;

  if (irop_config[op].has_dest)
  {
    SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, argc);
    tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &param, &call_id_sv, dest);
  }
  else
  {
    SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, argc);
    tcc_ir_put(ir, TCCIR_OP_FUNCCALLVOID, &param, &call_id_sv, NULL);
  }
}

/* Check if FPU operation needs soft-float call and emit it if needed.
 * Returns 1 if soft call was emitted, 0 if hardware FPU can be used.
 */
static int ir_put_soft_call_fpu_if_needed(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  const int is64bit = tcc_is_64bit_operand(src1) || tcc_is_64bit_operand(src2) || tcc_is_64bit_operand(dest);
  const FloatingPointConfig *fpu = architecture_config.fpu;

  switch (op)
  {
  case TCCIR_OP_FADD:
    if (is64bit && fpu->has_dadd)
      return 0;
    else if (!is64bit && fpu->has_fadd)
      return 0;
    break;
  case TCCIR_OP_FSUB:
    if (is64bit && fpu->has_dsub)
      return 0;
    else if (!is64bit && fpu->has_fsub)
      return 0;
    break;
  case TCCIR_OP_FMUL:
    if (is64bit && fpu->has_dmul)
      return 0;
    else if (!is64bit && fpu->has_fmul)
      return 0;
    break;
  case TCCIR_OP_FDIV:
    if (is64bit && fpu->has_ddiv)
      return 0;
    else if (!is64bit && fpu->has_fdiv)
      return 0;
    break;
  case TCCIR_OP_FNEG:
    if (is64bit && fpu->has_dneg)
      return 0;
    else if (!is64bit && fpu->has_fneg)
      return 0;
    break;
  case TCCIR_OP_FCMP:
    if (is64bit && fpu->has_dcmp)
      return 0;
    else if (!is64bit && fpu->has_fcmp)
      return 0;
    break;
  case TCCIR_OP_CVT_ITOF:
    if (is64bit && fpu->has_itod)
      return 0;
    else if (!is64bit && fpu->has_itof)
      return 0;
    break;
  case TCCIR_OP_CVT_FTOI:
    if (is64bit && fpu->has_dtoi)
      return 0;
    else if (!is64bit && fpu->has_ftoi)
      return 0;
    break;
  case TCCIR_OP_CVT_FTOF:
  {
    /* Same-size conversion (e.g., double <-> long double on ARM where both are 8 bytes)
     * is a no-op - emit ASSIGN instead of a soft-float call. */
    int src_align, dst_align;
    int src_size = src1 ? type_size(&src1->type, &src_align) : 0;
    int dst_size = dest ? type_size(&dest->type, &dst_align) : 0;
    if (src_size == dst_size)
      return 0; /* Codegen handles same-size as copy */
    if (is64bit && fpu->has_dtof && fpu->has_ftod)
      return 0;
    break;
  }
  default:
    return 0;
  }

  /* No hardware support, emit soft-float call */
  tcc_ir_put_soft_call(ir, op, src1, src2, dest);
  return 1;
}

/* ============================================================================
 * IR Operation Configuration
 * ============================================================================ */

// clang-format off
const IRRegistersConfig irop_config[] = {
    [TCCIR_OP_ADD] = {1, 1, 1},
    [TCCIR_OP_ADC_USE] = {1, 1, 1},
    [TCCIR_OP_ADC_GEN] = {1, 1, 1},
    [TCCIR_OP_SUB] = {1, 1, 1},
    [TCCIR_OP_SUBC_GEN] = {1, 1, 1},
    [TCCIR_OP_SUBC_USE] = {1, 1, 1},
    [TCCIR_OP_MUL] = {1, 1, 1},
    [TCCIR_OP_MLA] = {1, 1, 1},  /* MLA has accumulator as extra operand at pool[operand_base+3] */
    [TCCIR_OP_UMULL] = {1, 1, 1},
    [TCCIR_OP_DIV] = {1, 1, 1},
    [TCCIR_OP_UMOD] = {1, 1, 1},
    [TCCIR_OP_IMOD] = {1, 1, 1},
    [TCCIR_OP_AND] = {1, 1, 1},
    [TCCIR_OP_OR] = {1, 1, 1},
    [TCCIR_OP_XOR] = {1, 1, 1},
    [TCCIR_OP_SHL] = {1, 1, 1},
    [TCCIR_OP_SAR] = {1, 1, 1},
    [TCCIR_OP_SHR] = {1, 1, 1},
    [TCCIR_OP_PDIV] = {1, 1, 1},
    [TCCIR_OP_UDIV] = {1, 1, 1},
    [TCCIR_OP_CMP] = {0, 1, 1},
    [TCCIR_OP_RETURNVOID] = {0, 0, 0},
    [TCCIR_OP_RETURNVALUE] = {0, 1, 0},
    [TCCIR_OP_JUMP] = {1, 0, 0},
    [TCCIR_OP_JUMPIF] = {1, 1, 0},
    [TCCIR_OP_IJUMP] = {0, 1, 0},
    [TCCIR_OP_SETIF] = {1, 1, 0},
    /* FUNCPARAMVOID carries call_id in src2.c.i (encoded like FUNCPARAMVAL). */
    [TCCIR_OP_FUNCPARAMVOID] = {0, 0, 1},
    [TCCIR_OP_FUNCPARAMVAL] = {0, 1, 1},
    /* FUNCCALL* carries call_id in src2.c.i so backends can match parameters. */
    [TCCIR_OP_FUNCCALLVOID] = {0, 1, 1},
    [TCCIR_OP_FUNCCALLVAL] = {1, 1, 1},
    [TCCIR_OP_LOAD] = {1, 1, 0},
    [TCCIR_OP_STORE] = {1, 1, 0},
    [TCCIR_OP_ASSIGN] = {1, 1, 0},
    [TCCIR_OP_LEA] = {1, 1, 0},    /* dest = &src1 */
    [TCCIR_OP_LOAD_INDEXED] = {1, 1, 1},   /* dest = *(base + (index << scale)) */
    [TCCIR_OP_STORE_INDEXED] = {1, 1, 1},  /* *(base + (index << scale)) = src */
    [TCCIR_OP_LOAD_POSTINC] = {1, 1, 0},   /* dest = *ptr; ptr += offset */
    [TCCIR_OP_STORE_POSTINC] = {1, 1, 0},  /* *ptr = src; ptr += offset */
    [TCCIR_OP_TEST_ZERO] = {0, 1, 0},
    /* Floating point operations */
    [TCCIR_OP_FADD] = {1, 1, 1}, [TCCIR_OP_FSUB] = {1, 1, 1}, [TCCIR_OP_FMUL] = {1, 1, 1}, [TCCIR_OP_FDIV] = {1, 1, 1},
    [TCCIR_OP_FNEG] = {1, 1, 0}, /* unary: src1=input, dest */
    [TCCIR_OP_FCMP] = {0, 1, 1},
    /* Floating point conversion operations */
    [TCCIR_OP_CVT_FTOF] = {1, 1, 0}, /* dest=result, src1=input */
    [TCCIR_OP_CVT_ITOF] = {1, 1, 0}, /* dest=result, src1=input */
    [TCCIR_OP_CVT_FTOI] = {1, 1, 0}, /* dest=result, src1=input */
    /* Logical boolean operations */
    [TCCIR_OP_BOOL_OR] = {1, 1, 1},  /* dest = (src1 || src2) */
    [TCCIR_OP_BOOL_AND] = {1, 1, 1}, /* dest = (src1 && src2) */

    /* VLA / dynamic stack ops */
    [TCCIR_OP_VLA_ALLOC] = {0, 1, 1},      /* src1=size(bytes), src2=align(bytes) */
    [TCCIR_OP_VLA_SP_SAVE] = {1, 0, 0},    /* dest=stack slot to store SP */
    [TCCIR_OP_VLA_SP_RESTORE] = {0, 1, 0}, /* src1=stack slot holding saved SP */

    /* Inline asm markers/barrier.
     * INLINE_ASM carries inline_asm_id in src1.c.i. */
    [TCCIR_OP_ASM_INPUT] = {0, 1, 0}, [TCCIR_OP_INLINE_ASM] = {0, 1, 0}, [TCCIR_OP_ASM_OUTPUT] = {1, 0, 0},
    /* Explicit call sequence ops (Option A scaffold)
     * - CALLSEQ_BEGIN: src1=stack_size (bytes), src2=pad (bytes)
     * - CALLARG_REG: src1=value, src2=reg_index (immediate)
     * - CALLARG_STACK: src1=value, src2=stack_off (immediate)
     * - CALLSEQ_END: src1=stack_size (bytes), src2=pad (bytes)
     */
    [TCCIR_OP_CALLSEQ_BEGIN] = {0, 1, 1}, [TCCIR_OP_CALLARG_REG] = {0, 1, 1}, [TCCIR_OP_CALLARG_STACK] = {0, 1, 1},
    [TCCIR_OP_CALLSEQ_END] = {0, 1, 1},

    /* Init chain slot: src1 carries the chain slot symbol (SYMREF), no vreg */
    [TCCIR_OP_INIT_CHAIN_SLOT] = {0, 1, 0},
    /* No-operation */
    [TCCIR_OP_NOP] = {0, 0, 0},
    /* Trap instruction: no operands, no dest */
    [TCCIR_OP_TRAP] = {0, 0, 0},
    /* Jump table switch: src1=index vreg, src2=table_id, no dest */
    [TCCIR_OP_SWITCH_TABLE] = {0, 1, 1},
}
;
// clang-format on

/* ============================================================================
 * Live Interval Access
 * ============================================================================ */

IRLiveInterval *tcc_ir_get_live_interval(TCCIRState *ir, int vreg)
{
  if (vreg < 0)
  {
    fprintf(stderr, "tcc_ir_get_live_interval: invalid vreg: %d\n", vreg);
    exit(1);
  }
  int decoded_vreg_position = TCCIR_DECODE_VREG_POSITION(vreg);
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
  case TCCIR_VREG_TYPE_VAR:
  {
    if (decoded_vreg_position >= ir->variables_live_intervals_size)
    {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
      exit(1);
    }
    return &ir->variables_live_intervals[decoded_vreg_position];
  }
  case TCCIR_VREG_TYPE_TEMP:
  {
    if (decoded_vreg_position >= ir->temporary_variables_live_intervals_size)
    {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
      exit(1);
    }
    return &ir->temporary_variables_live_intervals[decoded_vreg_position];
  }
  case TCCIR_VREG_TYPE_PARAM:
  {
    if (decoded_vreg_position >= ir->parameters_live_intervals_size)
    {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
      exit(1);
    }
    return &ir->parameters_live_intervals[decoded_vreg_position];
  }
  default:
    fprintf(stderr, "Unknown vreg type %d for vreg %d\n", TCCIR_DECODE_VREG_TYPE(vreg), vreg);
    exit(1);
  }
  return NULL; /* unreachable, silences -Werror with old compiler */
}
