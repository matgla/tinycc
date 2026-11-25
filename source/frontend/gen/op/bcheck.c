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
 *     `__bound_new_region(&x, sizeof x)`, a VLA right after its allocation,
 *     a local scalar where its address is taken (`&x`), and each is dropped,
 *     `__bound_delete_region(&x)`, where its scope ends -- the block's end,
 *     break, continue, goto, return -- by a cleanup entry (CLEANUP_BCHECK,
 *     run by try_call_scope_cleanup like __attribute__((cleanup))), so a
 *     local declared after a return but registered by an earlier pass of a
 *     loop is gone when that return runs.  (Old tcc kept a frame-offset
 *     table, but frame offsets are only known after register allocation.)
 *     alloca's blocks and address-taken parameters live as long as the
 *     function: dropped at its returns and its end (bc_locals);
 *   - a struct copied whole checks both sides for the struct's size, and an
 *     access of a size no __bound_ptr_indirN covers is checked by
 *     `__bound_check(p, size)`;
 *   - calls of the C library's memcpy, memmove, memset, memcmp, strlen,
 *     strcpy, strncpy, strcmp, strncmp, strcat, strncat, strchr, strrchr,
 *     strdup (also through their __builtin_ spellings) and longjmp go to the
 *     runtime's `__bound_` wrappers, which check the regions they touch
 *     (longjmp: drops the regions of the frames it leaves);
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
  return 0; /* any other size: __bound_check (gbound) */
}

/* vtop: an lvalue marked VT_MUSTBOUND -> checked before it is read or
   written: the addition that computed its address checks the access too,
   or a fresh __bound_ptr_indirN(&lvalue, 0) does */
void gbound(void)
{
  TCCIRState *ir = tcc_state->ir;
  CType type1;
  int align, func, i, size;

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
  size = (vtop->type.t & VT_BITFIELD) ? 1 : type_size(&vtop->type, &align);
  func = bc_indir_func(size);
  if (func)
  {
    IRPoolSymref *ref = tcc_ir_pool_get_symref_ptr(ir, (uint32_t)ir->bc_patch[i + 1]);
    if (ref)
      ref->sym = external_helper_sym(func);
  }
  else if (size > 0)
  {
    /* a struct copied whole (or any size no __bound_ptr_indirN has): the
       address checked above, then all of its bytes */
    vpushv(vtop);
    vtop->r &= ~VT_LVAL;
    vtop->type = char_pointer_type;
    vpushi(size);
    gen_builtin_libcall(tok_alloc_const("__bound_check"), 2, VT_INT);
    vpop();
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

/* sym, a local or a parameter: its address, as a char * (as an identifier
   pushes it, then &) */
static void bc_push_local_addr(Sym *sym)
{
  int r = sym->r;
  if ((r & VT_VALMASK) < VT_CONST && !(r & VT_PARAM))
    r = (r & ~VT_VALMASK) | VT_LOCAL;
  vset(&sym->type, r, sym->c);
  vtop->sym = sym;
  vtop->vr = sym->vreg;
  if (vtop->r & VT_LVAL)
    gaddrof();
  vtop->type = char_pointer_type;
}

/* the pointer in frame slot SLOT (a VLA's or alloca's block), as an lvalue */
static void bc_push_slot(int slot)
{
  vset(&char_pointer_type, VT_LOCAL | VT_LVAL, slot);
  vtop->vr = -1;
}

/* vtop[-1] an address, vtop a size: __bound_new_region(address, size), or
   for a scalar __bound_new_scalar (whose one-past-the-end address is taken
   for the next object's: scalars' homes are packed) */
static void bc_new_region(int scalar)
{
  gen_builtin_libcall(tok_alloc_const(scalar ? "__bound_new_scalar" : "__bound_new_region"), 2, VT_INT);
  vpop();
}

/* vtop an address: __bound_delete_region(address), popped */
static void bc_delete_region(void)
{
  if (vtop->r & VT_LVAL)
    gv(RC_INT);
  gen_builtin_libcall(tok_alloc_const("__bound_delete_region"), 1, VT_INT);
  vpop();
}

/* the scope's cleanup entry that drops a local: KIND (in r) says how its
   address is found (BC_CL_*), SLOT (in vreg) is a VLA's pointer slot */
enum
{
  BC_CL_LOCAL,  /* &sym */
  BC_CL_SLOT,   /* the pointer in frame slot cls->vreg */
  BC_CL_SCALAR, /* &sym, once its address was taken (SYMF_BC_ADDR) */
};

static void bc_push_cleanup(Sym *sym, int kind, int slot)
{
  Sym *cls = sym_push2(&all_cleanups, SYM_FIELD | ++cur_scope->cl.n, CLEANUP_BCHECK, 0);
  cls->defer_str = NULL; /* c shares it: not a defer's body (defer_free_entries) */
  cls->prev_tok = sym;
  cls->r = kind;
  cls->vreg = slot;
  cls->next = cur_scope->cl.s;
  cur_scope->cl.s = cls;
}

/* try_call_scope_cleanup: a CLEANUP_BCHECK entry, its scope left */
void bcheck_cleanup(Sym *cls)
{
  if (!bcheck_code())
    return;
  switch (cls->r)
  {
  case BC_CL_SLOT:
    bc_push_slot(cls->vreg);
    break;
  case BC_CL_SCALAR:
    if (!sym_flag(cls->prev_tok, SYMF_BC_ADDR))
      return; /* no `&x` up to here: never registered on this path */
    /* fall through */
  default:
    bc_push_local_addr(cls->prev_tok);
    break;
  }
  bc_delete_region();
}

/* a region that lives as long as the function: dropped at its exits */
static void bc_note_frame_region(SValue *addr)
{
  TCCIRState *ir = tcc_state->ir;
  if (ir->bc_locals_n == ir->bc_locals_cap)
  {
    ir->bc_locals_cap = ir->bc_locals_cap ? ir->bc_locals_cap * 2 : 16;
    ir->bc_locals = tcc_realloc(ir->bc_locals, sizeof(SValue) * ir->bc_locals_cap);
  }
  ir->bc_locals[ir->bc_locals_n++] = *addr;
}

/* the local just declared: an array or struct is registered and dropped at
   the end of its scope; a scalar gets the entry that drops it should its
   address be taken (bcheck_addr_taken); a VLA is registered once allocated
   (bcheck_vla) */
void bcheck_local_decl(Sym *sym)
{
  int align, size;
  if (!bcheck_code() || (sym->type.t & VT_VLA) || (sym->r & VT_VALMASK) != VT_LOCAL)
    return;
  if (!(sym->type.t & VT_ARRAY) && (sym->type.t & VT_BTYPE) != VT_STRUCT)
  {
    if ((sym->type.t & VT_BTYPE) != VT_FUNC)
    {
      sym_flag_set(sym, SYMF_BC_SCOPED);
      bc_push_cleanup(sym, BC_CL_SCALAR, 0);
    }
    return;
  }
  if (struct_has_vla_member(&sym->type))
    return;
  size = type_size(&sym->type, &align);
  if (size <= 0)
    return;
  bc_push_local_addr(sym);
  vpushi(size);
  bc_new_region(0);
  bc_push_cleanup(sym, BC_CL_LOCAL, 0);
}

/* a VLA (or a struct with VLA members) of TYPE, its block's address just
   saved in frame slot SLOT: registered, dropped at the end of its scope */
void bcheck_vla(Sym *sym, CType *type, int slot)
{
  int align;
  if (!bcheck_code())
    return;
  bc_push_slot(slot);
  gv(RC_INT);
  vpush_type_size(type, &align);
  bc_new_region(0);
  bc_push_cleanup(sym, BC_CL_SLOT, slot);
}

/* alloca: its block (the pointer in frame slot SLOT) of SIZE bytes, until the
   function returns */
void bcheck_alloca(int slot, SValue *size)
{
  if (!bcheck_code())
    return;
  bc_push_slot(slot);
  bc_note_frame_region(vtop);
  gv(RC_INT);
  vpushv(size);
  bc_new_region(0);
}

/* vtop: &x just taken.  A local scalar is registered here (every time: the
   path may not have passed an earlier `&x`); a block-scope one is dropped by
   its scope's entry from now on, a parameter at the function's exits.
   Arrays and structs were registered where they were declared. */
void bcheck_addr_taken(Sym *sym)
{
  int align, size;
  if (!bcheck_code() || !sym || ((sym->r & VT_VALMASK) != VT_LOCAL && !(sym->r & VT_PARAM)) ||
      (sym->type.t & (VT_ARRAY | VT_VLA)) ||
      (sym->type.t & VT_BTYPE) == VT_STRUCT || (sym->type.t & VT_BTYPE) == VT_FUNC || sym->a.nested_func)
    return;
  size = type_size(&sym->type, &align);
  if (size <= 0)
    return;
  vpushv(vtop);
  vtop->type = char_pointer_type;
  vpushi(size);
  bc_new_region(1);
  if (!sym_flag(sym, SYMF_BC_ADDR))
  {
    sym_flag_set(sym, SYMF_BC_ADDR);
    if (!sym_flag(sym, SYMF_BC_SCOPED))
    {
      /* a parameter: its address, for the function's exits */
      bc_push_local_addr(sym);
      bc_note_frame_region(vtop);
      vpop();
    }
  }
}

/* the function's exits (each return, and the common one): the regions that
   live as long as it are dead */
void bcheck_func_exit(void)
{
  TCCIRState *ir = tcc_state->ir;
  for (int i = 0; ir && i < ir->bc_locals_n; i++)
  {
    vpushv(&ir->bc_locals[i]);
    bc_delete_region();
  }
}

/* main's entry: `__bound_main_arg(argc, argv)`, which makes argv's
   strings and the array itself, its NULL included, regions (old tcc's
   runtime did the same).  Without it a pointer into them is outside every
   region, and nothing read or written through it (argv[argc + 1],
   argv[1][n] past the string's end) is checked.  Not envp: newlib's crt0
   calls main(argc, argv) and leaves r2 as it was.  Not a static or nested
   main, nor one whose argc or argv is unnamed. */
void bcheck_main_entry(Sym *func)
{
  Sym *params[2], *arg;
  int n = 0;
  if (!bcheck_code() || (func->type.t & VT_STATIC) || func->a.nested_func || func->v >= SYM_FIRST_ANOM ||
      strcmp(tok_name_tmp(func->v), "main"))
    return;
  for (arg = sym_first(func->type.ref); arg && n < 2; arg = arg->next)
  {
    const int v = arg->v & ~SYM_FIELD;
    Sym *s = v < SYM_FIRST_ANOM ? sym_find(v) : NULL;
    if (!s || !(s->r & VT_PARAM))
      return;
    params[n++] = s;
  }
  if (n < 2 || !is_integer_btype(params[0]->type.t & VT_BTYPE) || (params[1]->type.t & VT_BTYPE) != VT_PTR)
    return;
  for (int i = 0; i < 2; i++)
  {
    Sym *s = params[i];
    vset(&s->type, s->r, s->c);
    vtop->sym = s;
    vtop->vr = s->vreg;
    gv(RC_INT);
  }
  vtop->type = char_pointer_type;
  gen_builtin_libcall(tok_alloc_const("__bound_main_arg"), 2, VT_INT);
  vpop();
}

/* the C library functions whose reads and writes the runtime checks */
static const char bc_lib_names[] = "memcpy\0memmove\0memset\0memcmp\0strlen\0strcpy\0strncpy\0strcmp\0"
                                   "strncmp\0strcat\0strncat\0strchr\0strrchr\0strdup\0longjmp\0";

/* vtop: the function a call is about to call.  A direct call of one of the
   library functions above calls its __bound_ wrapper instead, with the same
   prototype.  Not a static function of this file reusing the name. */
void bcheck_redirect_call(void)
{
  char buf[64];
  const char *name;
  Sym *s = vtop->sym;
  if (!bcheck_code() || (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != (VT_CONST | VT_SYM) || vtop->c.i || !s ||
      (s->type.t & (VT_BTYPE | VT_STATIC)) != VT_FUNC || s->v >= SYM_FIRST_ANOM)
    return;
  name = tok_name_tmp(s->asm_label ? s->asm_label : s->v);
  if (!strncmp(name, "__builtin_", 10))
    name += 10;
  for (const char *p = bc_lib_names; *p; p += strlen(p) + 1)
    if (!strcmp(p, name))
    {
      snprintf(buf, sizeof buf, "__bound_%s", name);
      vtop->sym = external_global_sym(tok_alloc_const(buf), &s->type);
      return;
    }
}

#endif /* CONFIG_TCC_BCHECK */
