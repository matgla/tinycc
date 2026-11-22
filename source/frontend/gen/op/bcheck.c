/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

/* bcheck.c -- `-b` bounds checking, emitted through the IR.
 *
 * The armv8m cross only (CONFIG_TCC_BCHECK_CROSS, tcc.h): the runtime,
 * lib/bcheck-bm.c, is for the QEMU newlib runs; the board has no memory for
 * it, and the device compiler is built without any of this.
 *
 * The scheme is old tcc's, made of ordinary calls:
 *
 *   - pointer arithmetic `p + n` becomes `__bound_ptr_add(p, n * size)`,
 *     which checks that the result stays inside (or one past) p's region;
 *   - a dereference of such a result turns that same call into
 *     `__bound_ptr_indirN(p, n * size)` (N the access size): the access must
 *     fit in p's region.  Old tcc patched the relocation of the call it had
 *     emitted; here the callee symbol of the FUNCCALLVAL is patched while the
 *     function is still raw IR (bc_patch maps the call's result vreg to its
 *     symref);
 *   - any other dereference (`*p`, `s.x`, `q->f`) is a fresh
 *     `__bound_ptr_indirN(&lvalue, 0)`;
 *   - a local array or struct is registered where it is declared,
 *     `__bound_new_region(&x, sizeof x)`, and every local this function
 *     registered is dropped at its common exit, `__bound_delete_region(&x)`
 *     (old tcc kept a frame-offset table, but frame offsets are only known
 *     after register allocation);
 *   - a global's (address, size) goes to the .bounds section (init/alloc.c),
 *     ended by a zero at link time (tcc_add_bcheck), whose start the linker
 *     names __bound_globals for the runtime's constructor.
 *
 * Off (do_bounds_check == 0) nothing here runs, and objects are those of a
 * compiler without -b. */

#include "gen_priv.h"

#ifdef CONFIG_TCC_BCHECK

static int bcheck_code(void)
{
  /* as tcc_ir_put: code after a return is still emitted (a label may follow) */
  return tcc_state->do_bounds_check && !(nocode_wanted & ~CODE_OFF_BIT) && tcc_state->ir;
}

static void bc_patch_note(TCCIRState *ir, int vr, uint32_t pool_idx)
{
  if (ir->bc_patch_n + 2 > ir->bc_patch_cap)
  {
    ir->bc_patch_cap = ir->bc_patch_cap ? ir->bc_patch_cap * 2 : 64;
    ir->bc_patch = tcc_realloc(ir->bc_patch, sizeof(int) * ir->bc_patch_cap);
  }
  ir->bc_patch[ir->bc_patch_n++] = vr;
  ir->bc_patch[ir->bc_patch_n++] = (int)pool_idx;
}

/* vtop[-1] (a pointer) + vtop (an offset in bytes) -> the checked sum */
void gen_bounded_ptr_add(void)
{
  TCCIRState *ir = tcc_state->ir;
  if (!bcheck_code())
  {
    gen_opic('+');
    return;
  }
  /* the operands go to the call as they are (an address stays an address:
     gv() would LOAD it); a pointer read through a pointer is checked by
     tcc_ir_put (bcheck_ir_operand) */
  /* the access goes through the call's result now: a store to a static
     seen through it must still stop the folds that assume the static keeps
     its initializer (vstore marks a direct one) */
  if ((vtop[-1].r & (VT_VALMASK | VT_SYM | VT_LVAL)) == (VT_CONST | VT_SYM) && vtop[-1].sym)
    vtop[-1].sym->a.possibly_written = 1;
  gen_builtin_libcall(tok_alloc_const("__bound_ptr_add"), 2, VT_PTR);
  const int pos = ir->next_instruction_index - 1;
  if (pos >= 0 && ir->compact_instructions[pos].op == TCCIR_OP_FUNCCALLVAL)
  {
    IROperand callee = tcc_ir_op_get_src1(ir, &ir->compact_instructions[pos]);
    if (irop_get_tag(callee) == IROP_TAG_SYMREF)
      bc_patch_note(ir, vtop->vr, irop_get_pool_idx(callee));
  }
}

/* vtop: an lvalue -> the index in bc_patch of the __bound_ptr_add call
   whose result is its address, or -1 */
static int bc_patch_find(void)
{
  TCCIRState *ir = tcc_state->ir;
  const int v = vtop->r & VT_VALMASK;
  if (vtop->vr < 0 || v >= VT_CONST || v == VT_LOCAL || v == VT_LLOCAL || (vtop->r & VT_SYM) || vtop->c.i)
    return -1;
  for (int i = ir->bc_patch_n - 2; i >= 0; i -= 2)
    if (ir->bc_patch[i] == vtop->vr)
      return i;
  return -1;
}

/* the __bound_ptr_indirN that checks an access of this size, or 0 */
static int bc_indir_func(int size)
{
  switch (size)
  {
  case 1:
    return tok_alloc_const("__bound_ptr_indir1");
  case 2:
    return tok_alloc_const("__bound_ptr_indir2");
  case 4:
    return tok_alloc_const("__bound_ptr_indir4");
  case 8:
    return tok_alloc_const("__bound_ptr_indir8");
  case 12:
    return tok_alloc_const("__bound_ptr_indir12");
  case 16:
    return tok_alloc_const("__bound_ptr_indir16");
  }
  return 0; /* a struct copied whole: checked as pointer arithmetic only */
}

/* vtop: an lvalue marked VT_MUSTBOUND -> checked before it is read or
   written: the addition that computed its address checks the access too,
   or a fresh __bound_ptr_indirN(&lvalue, 0) does */
void gbound(void)
{
  TCCIRState *ir = tcc_state->ir;
  CType type1;
  int align, func, i;

  vtop->r &= ~VT_MUSTBOUND;
  if (!(vtop->r & VT_LVAL) || !bcheck_code())
    return;
  i = bc_patch_find();
  if (i < 0)
  {
    /* the address, as a pointer, plus 0 */
    type1 = vtop->type;
    vtop->type.t = VT_PTR;
    gaddrof();
    vpushi(0);
    gen_bounded_ptr_add();
    vtop->r |= VT_LVAL;
    vtop->type = type1;
    i = bc_patch_find();
    if (i < 0)
      return;
  }
  /* a bitfield's unit may reach past a packed struct (adjust_bf narrows the
     access later): its first byte is what is known to be inside */
  func = bc_indir_func((vtop->type.t & VT_BITFIELD) ? 1 : type_size(&vtop->type, &align));
  if (func)
  {
    IRPoolSymref *ref = tcc_ir_pool_get_symref_ptr(ir, (uint32_t)ir->bc_patch[i + 1]);
    if (ref)
      ref->sym = external_helper_sym(func);
  }
}

/* an operand of tcc_ir_put marked VT_MUSTBOUND (loads folded into an
   operation, stores) -> made the checked lvalue */
void bcheck_ir_operand(SValue *sv)
{
  vpushv(sv);
  gbound();
  *sv = *vtop;
  vtop--;
}

/* the local just declared (sym, at frame offset addr): registered, and
   dropped at the function's exit */
void bcheck_local_decl(Sym *sym)
{
  TCCIRState *ir = tcc_state->ir;
  int align, size;
  if (!bcheck_code() || (sym->type.t & VT_VLA))
    return;
  if (!(sym->type.t & VT_ARRAY) && (sym->type.t & VT_BTYPE) != VT_STRUCT)
    return;
  size = type_size(&sym->type, &align);
  if (size <= 0)
    return;
  vset(&sym->type, (sym->r & ~VT_VALMASK) | VT_LOCAL, sym->c);
  vtop->sym = sym;
  vtop->vr = sym->vreg;
  if (vtop->r & VT_LVAL)
    gaddrof();
  vtop->type = char_pointer_type;
  if (ir->bc_locals_n == ir->bc_locals_cap)
  {
    ir->bc_locals_cap = ir->bc_locals_cap ? ir->bc_locals_cap * 2 : 16;
    ir->bc_locals = tcc_realloc(ir->bc_locals, sizeof(SValue) * ir->bc_locals_cap);
  }
  ir->bc_locals[ir->bc_locals_n++] = *vtop;
  vpushi(size);
  gen_builtin_libcall(tok_alloc_const("__bound_new_region"), 2, VT_INT);
  vpop();
}

/* the common exit: the locals this function registered are dead */
void bcheck_func_exit(void)
{
  TCCIRState *ir = tcc_state->ir;
  for (int i = 0; ir && i < ir->bc_locals_n; i++)
  {
    vpushv(&ir->bc_locals[i]);
    gen_builtin_libcall(tok_alloc_const("__bound_delete_region"), 1, VT_INT);
    vpop();
  }
}

#endif /* CONFIG_TCC_BCHECK */
