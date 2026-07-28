/*
 *  TCC IR - Function Parameter / ABI Setup
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

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

  /* Hard-float: a non-variadic function receives its float parameters in VFP
   * registers (s0..s15), matching the caller's placement.  A variadic function
   * uses the base (GPR) standard for every parameter. */
  call_layout.hard_float = (tcc_state && tcc_state->float_abi == ARM_HARD_FLOAT);
  call_layout.is_variadic = variadic;

  /* Set up local variable area - variadic functions need extra space */
  loc = variadic ? -28 : 0;
  func_vc = 0;

  /* Handle hidden sret pointer for struct/complex returns */
  if ((sym->type.t & VT_BTYPE) == VT_STRUCT || (sym->type.t & VT_COMPLEX))
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
    desc.size = (uint32_t)size;
    /* Use AAPCS natural alignment (based on member types) for register
     * double-word alignment rule (even-register requirement). */
    int aapcs_align = ctype_aapcs_alignment(type);
    desc.alignment = (uint8_t)(aapcs_align < align ? aapcs_align : align);
  }
  else if (type->t & VT_COMPLEX)
  {
    /* Complex types are passed like composites (AAPCS treats them as
     * arrays of two elements): complex float = 8 bytes, complex double = 16 bytes. */
    desc.kind = TCC_ABI_ARG_STRUCT_BYVAL;
    desc.size = (uint32_t)size;
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

  /* Scalar float/double only: complex is passed as a composite (see above), so
   * it must not be diverted into the VFP argument bank. */
  desc.is_float = (is_float(type->t) && !(type->t & VT_COMPLEX) && (type->t & VT_BTYPE) != VT_STRUCT) ? 1 : 0;

  TCCAbiArgLoc loc_info = tcc_abi_classify_argument(call_layout, arg_index, &desc);
  tcc_ir_params_update_tracking(ir, loc_info, call_layout);

  /* With the pre-reserved outgoing call area, stack args no longer require
   * a frame pointer — SP stays fixed across calls. */

  if ((type->t & VT_BTYPE) == VT_STRUCT || (type->t & VT_COMPLEX))
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

    IRLiveInterval *ptr_iv = tcc_ir_vreg_live_interval(ir, ptr_param_vr);
    if (ptr_iv)
    {
      if (loc_info->kind == TCC_ABI_LOC_REG)
      {
        /* Invisible-ref pointer passed in a register.
         * Set incoming register so tcc_ir_mark_param_incoming_regs skips
         * this vreg and doesn't re-assign it based on sequential argno. */
        ptr_iv->incoming_reg0 = loc_info->reg_base;
        ptr_iv->incoming_reg1 = -1;
      }
      else
      {
        /* Invisible-ref pointer passed on the stack (all argument registers
         * exhausted).  Mark as stack-passed and record the caller-frame
         * offset so PARAM_STACK materialisation picks it up correctly. */
        ptr_iv->incoming_reg0 = -1;
        ptr_iv->incoming_reg1 = -1;
        tcc_ir_set_original_offset(ir, ptr_param_vr, loc_info->stack_off);
      }
    }

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
    {
      int v = sym->v & ~SYM_FIELD;
      if (!v)
        v = anon_sym++;
      sym_push(v, type, flags, addr);
    }
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

      /* Set incoming register so tcc_ir_mark_param_incoming_regs skips
       * this vreg.  The AAPCS even-register rule may have skipped a
       * register, so reg_base may not match the sequential argno. */
      IRLiveInterval *word_iv = tcc_ir_vreg_live_interval(ir, word_param_vr);
      if (word_iv)
      {
        word_iv->incoming_reg0 = loc_info->reg_base + w;
        word_iv->incoming_reg1 = -1;
      }

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
    {
      int v = sym->v & ~SYM_FIELD;
      if (!v)
        v = anon_sym++;
      sym_push(v, type, flags, addr);
    }
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

      /* Set incoming register — see REG case above. */
      IRLiveInterval *word_iv = tcc_ir_vreg_live_interval(ir, word_param_vr);
      if (word_iv)
      {
        word_iv->incoming_reg0 = loc_info->reg_base + w;
        word_iv->incoming_reg1 = -1;
      }

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
    {
      int v = sym->v & ~SYM_FIELD;
      if (!v)
        v = anon_sym++;
      sym_push(v, type, flags, addr);
    }
    return;
  }

  /* Struct passed on stack */
  flags = VT_PARAM | VT_LVAL | VT_LOCAL;
  addr = loc_info->stack_off;
  {
    int v = sym->v & ~SYM_FIELD;
    if (!v)
      v = anon_sym++;
    sym_push(v, type, flags, addr);
  }
}

void tcc_ir_params_process_scalar(TCCIRState *ir, Sym *sym, CType *type, TCCAbiArgLoc *loc_info)
{
  int flags = 0, addr = 0;
  int variadic = (sym->f.func_type == FUNC_ELLIPSIS);
  CType pushed_type = *type;

  if (sym->a.param_volatile)
    pushed_type.t |= VT_VOLATILE;

  if (loc_info->kind == TCC_ABI_LOC_REG || loc_info->kind == TCC_ABI_LOC_VFP_REG)
  {
    /* Register-resident parameter (GPR or, for hard-float floats, a VFP
     * register).  VFP passing only happens for non-variadic functions, so the
     * variadic stack-slot handling never applies to TCC_ABI_LOC_VFP_REG. */
    flags = VT_PARAM | VT_LVAL;
    if (variadic && loc_info->kind == TCC_ABI_LOC_REG)
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
  /* For unnamed parameters (GNU C / C23), use anonymous symbol */
  int v = sym->v & ~SYM_FIELD;
  if (!v)
    v = anon_sym++;
  sym_push(v, &pushed_type, flags, addr);
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
