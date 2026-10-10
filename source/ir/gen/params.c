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

/* The form the parameter being processed takes in the body, when it can be
 * described (see IRParamForm); NULL when not recording. */
static IRParamForm *pf_cur;

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
    if (func_vc != 0 && !ir->vfp_ret_words)
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

  const int vfp_words = ret_nregs == 0 ? gfunc_sret_vfp_words(&sym->type, sym->f.func_type == FUNC_ELLIPSIS) : 0;
  if (vfp_words)
  {
    /* Returned in s0..s(n-1): no hidden pointer arrives.  The returns write
     * a frame buffer, whose address func_vc's slot holds as it would hold
     * the caller's, and the exit loads the registers from it. */
    int size, align;
    size = type_size(&sym->type, &align);
    loc = tcc_ir_frame_alloc(loc, size, -8);
    tcc_ir_frame_note_type(loc, &sym->type);
    ir->vfp_ret_buf = loc;
    ir->vfp_ret_words = (uint8_t)vfp_words;
    loc = tcc_ir_frame_alloc(loc, PTR_SIZE, -PTR_SIZE);
    func_vc = loc;
    if (ir->naked) /* no frame: the asm body leaves the result in s0-s7 */
      return;

    SValue buf, addr, slot;
    svalue_init(&buf);
    buf.type.t = VT_PTR;
    buf.r = VT_LOCAL;
    buf.vr = -1;
    buf.c.i = ir->vfp_ret_buf;
    svalue_init(&addr);
    addr.type.t = VT_PTR;
    addr.r = 0;
    addr.vr = tcc_ir_get_vreg_temp(ir);
    tcc_ir_put(ir, TCCIR_OP_LEA, &buf, NULL, &addr);
    svalue_init(&slot);
    slot.type.t = VT_PTR;
    slot.r = VT_LOCAL | VT_LVAL;
    slot.vr = -1;
    slot.c.i = func_vc;
    tcc_ir_put(ir, TCCIR_OP_STORE, &addr, NULL, &slot);
  }
  else if (ret_nregs == 0)
  {
    /* Struct returned via hidden pointer in first parameter (r0) */
    SValue src, dst;

    /* A frame OBJECT, not a raw slot: SRA then treats the pointer like any
     * other word-sized local and promotes it, so the returns read a register
     * instead of reloading the slot (zig.c: every struct-returning function
     * reloaded it at each `return`). */
    if (getenv("TCC_SRET_HOME_SLOT")) /* the old raw slot, for experiments */
      loc = (loc - PTR_SIZE) & -PTR_SIZE;
    else
      loc = tcc_ir_frame_alloc(loc, PTR_SIZE, -PTR_SIZE);
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
    tcc_free(ir->param_forms);
    ir->param_forms = arg_count > 0 ? tcc_mallocz(sizeof(IRParamForm) * arg_count) : NULL;
  }

  /* Process each parameter */
  for (sym = param_list; sym; sym = sym->next, ++arg_index)
  {
    pf_cur = ir && ir->param_forms && arg_index < 127 ? &ir->param_forms[arg_index] : NULL;
    tcc_ir_params_process_single(ir, sym, arg_index, call_layout);
    pf_cur = NULL;
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
  if (!desc.is_float)
  {
    int hfa_base;
    desc.hfa_count = (uint8_t)gfunc_hfa(type, &hfa_base);
    desc.hfa_base = (uint8_t)hfa_base;
  }

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

/* A parameter the ABI placed wholly on the caller's stack.  Recorded on its
 * PARAM vreg because tcc_ir_register_allocation_params re-derives register
 * placement by counting, and cannot tell a register an 8-aligned struct
 * skipped (r3 before a stack-passed S24L) from one still free.  Under
 * hard-float, use_vfp on such a vreg marks a VFP candidate (a float, double,
 * _Complex float/double or HFA): it is on the stack because the VFP bank was
 * full, and later core-register arguments still take r0-r3. */
static void params_mark_stack(TCCIRState *ir, Sym *ps)
{
  if (!ir || !ps || ps->vreg < 0 || TCCIR_DECODE_VREG_TYPE(ps->vreg) != TCCIR_VREG_TYPE_PARAM)
    return;
  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, ps->vreg);
  if (iv)
    iv->incoming_stack = 1;
}

/* Offset of a stack-passed parameter as the IR names it: from the first stack
 * argument, or from the pushed r0 when the prologue pushes r0-r3 for an
 * in-place split struct (see tcc_ir_params_process_struct). */
static int param_stack_off(const TCCIRState *ir, const TCCAbiArgLoc *loc_info)
{
  return loc_info->stack_off + (ir && ir->push_arg_regs ? 16 : 0);
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
    /* A stack slot is a whole number of words (a 17-byte struct takes 20). */
    int end = loc_info.stack_off + ((loc_info.size + 3) & ~3);
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
  int slot_align = align < 4 ? 4 : align;
  int flags = 0, addr = 0;

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
    if (pf_cur)
      *pf_cur = (IRParamForm){.kind = IR_PF_HOME, .reg_base = (int8_t)loc_info->reg_base, .off = struct_slot,
                              .words = word_count};
    return;
  }

  if (loc_info->kind == TCC_ABI_LOC_VFP_REG)
  {
    /* Hard-float HFA or _Complex float/double in s<reg_base>.. (a double is
     * the s-register pair of its d-register, low word first): one float PARAM
     * vreg per word, stored into a local home like a core-register struct.
     * Moving the raw words keeps a double's bits intact. */
    int slot_size = tcc_abi_align_up_int(size, 4);
    loc = (loc - slot_size) & -slot_align;
    const int struct_slot = loc;
    for (int w = 0; w < loc_info->reg_count; ++w)
    {
      const int word_param_vr = tcc_ir_get_vreg_param(ir);
      tcc_ir_set_float_type(ir, word_param_vr, 1, 0);
      IRLiveInterval *word_iv = tcc_ir_vreg_live_interval(ir, word_param_vr);
      if (word_iv)
      {
        word_iv->incoming_reg0 = LS_VFP_REG_BASE + loc_info->reg_base + w;
        word_iv->incoming_reg1 = -1;
      }

      SValue src, dst;
      memset(&src, 0, sizeof(src));
      memset(&dst, 0, sizeof(dst));
      src.type.t = VT_FLOAT;
      src.r = 0;
      src.vr = word_param_vr;
      dst.type.t = VT_FLOAT;
      dst.r = VT_LOCAL | VT_LVAL;
      dst.vr = -1;
      dst.c.i = struct_slot + w * 4;
      tcc_ir_put(ir, TCCIR_OP_STORE, &src, NULL, &dst);
    }
    int v = sym->v & ~SYM_FIELD;
    if (!v)
      v = anon_sym++;
    sym_push(v, type, VT_LVAL | VT_LOCAL, struct_slot);
    return;
  }

  if (loc_info->kind == TCC_ABI_LOC_REG_STACK && loc_info->stack_off == 0 && !call_layout->is_variadic)
  {
    /* Struct straddles r3 and the stack.  The prologue pushes r0-r3 right
     * below the stack arguments (push_arg_regs), which makes it contiguous in
     * memory: use it in place from its first register's slot instead of
     * copying it word by word into the frame.  Parameter offsets are then
     * measured from the pushed r0 (offset_to_args stops there), so they stay
     * non-negative -- disjoint from the frame's negative local offsets, which
     * passes comparing slot offsets rely on -- and every stack parameter
     * after this one sits 16 bytes further up (param_stack_off). */
    ir->push_arg_regs = 1;
    int v = sym->v & ~SYM_FIELD;
    if (!v)
      v = anon_sym++;
    Sym *ps = sym_push(v, type, VT_PARAM | VT_LVAL | VT_LOCAL, loc_info->reg_base * 4);
    params_mark_stack(ir, ps);
    if (pf_cur && ps->vreg >= 0)
      *pf_cur = (IRParamForm){.kind = IR_PF_MEM, .vreg = ps->vreg, .off = loc_info->reg_base * 4};
    return;
  }

  if (loc_info->kind == TCC_ABI_LOC_REG_STACK)
  {
    /* Struct straddles registers and stack, the stack part not first on the
     * stack (only under hard-float, after VFP arguments spilled): copy it. */
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
  addr = param_stack_off(ir, loc_info);
  {
    int v = sym->v & ~SYM_FIELD;
    if (!v)
      v = anon_sym++;
    Sym *ps = sym_push(v, type, flags, addr);
    params_mark_stack(ir, ps);
    /* A hard-float HFA here went to the stack because the VFP bank was full,
     * which leaves the core registers alone (see params_mark_stack). */
    int hfa_base;
    if (call_layout->hard_float && !call_layout->is_variadic && gfunc_hfa(type, &hfa_base) > 0 && ps->vreg >= 0)
    {
      IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, ps->vreg);
      if (iv)
        iv->use_vfp = 1;
    }
    if (pf_cur && ps->vreg >= 0)
      *pf_cur = (IRParamForm){.kind = IR_PF_MEM, .vreg = ps->vreg, .off = addr};
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
    addr = param_stack_off(ir, loc_info);
  }

  sym->r |= ~(VT_LVAL | VT_LLOCAL);
  /* For unnamed parameters (GNU C / C23), use anonymous symbol */
  int v = sym->v & ~SYM_FIELD;
  if (!v)
    v = anon_sym++;
  Sym *ps = sym_push(v, &pushed_type, flags, addr);
  if (loc_info->kind == TCC_ABI_LOC_STACK)
    params_mark_stack(ir, ps);
  if (pf_cur && ps->vreg >= 0 && !variadic)
    *pf_cur = (IRParamForm){.kind = loc_info->kind == TCC_ABI_LOC_STACK ? IR_PF_MEM : IR_PF_REG, .vreg = ps->vreg,
                            .off = addr};
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
