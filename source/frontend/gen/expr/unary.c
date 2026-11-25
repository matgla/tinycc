/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* unary.c -- Unary expressions, parenthesized/cast/compound-literal and _Generic.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* GNU cast to union: the type of the member whose unqualified type is
   compatible with the lvalue-converted type of vtop (arrays and functions
   decay to pointers), unqualified.  As in gcc, no such member is an error. */
static CType union_cast_member_type(CType *union_type)
{
  CType op = vtop->type;
  Sym *f;

  if (op.t & (VT_ARRAY | VT_VLA))
  {
    op = *pointed_type(&vtop->type);
    mk_pointer(&op);
  }
  else if ((op.t & VT_BTYPE) == VT_FUNC)
    mk_pointer(&op);

  for (f = sym_first(union_type->ref); f; f = f->next)
  {
    if (!(f->type.t & VT_BITFIELD) && compare_types(&f->type, &op, 1))
    {
      CType mtype = f->type;
      mtype.t &= ~(VT_CONSTANT | VT_VOLATILE);
      return mtype;
    }
  }
  tcc_error("cast to union type from type not present in union");
}

/* `&` applied to vtop, an lvalue (or a function or array designator). */
/* -Warray-bounds: base[index] (or index[base]) on the value stack, a
   constant index into an array of known size.  Returns the element count
   (and the index, the array's type for the message), or -1. */
int fe_subscript_bounds(int64_t *idx, char *tbuf, int tsize)
{
  SValue *base = vtop - 1, *ix = vtop;
  int big;
  if (!(base->type.t & VT_ARRAY) && (ix->type.t & VT_ARRAY))
    base = vtop, ix = vtop - 1;
  if (!(base->type.t & VT_ARRAY) && fe_int_const(ix))
  {
    /* `(a + 2)[3]`: a pointer into a known array */
    int n = fe_array_ptr_bounds(base, idx, tbuf, tsize);
    if (n >= 0)
    {
      *idx += fe_const_value(ix, &big);
      return n;
    }
    if (fe_int_const(base) || (n = fe_array_ptr_bounds(ix, idx, tbuf, tsize)) < 0)
      return -1;
    *idx += fe_const_value(base, &big);
    return n;
  }
  if (!(base->type.t & VT_ARRAY) || (base->type.t & VT_VLA) || !base->type.ref || base->type.ref->c < 0 ||
      base->union_member || !fe_int_const(ix))
    return -1;
  *idx = fe_const_value(ix, &big);
  if (big)
    *idx = INT64_MAX;
  CType ct = base->type;
  ct.t &= ~(VT_STORAGE | VT_CONSTANT | VT_VOLATILE);
  type_to_str(tbuf, tsize, &ct, NULL);
  /* gcc's 'int[4]' */
  for (char *p = tbuf; (p = strstr(p, " [")) != NULL;)
    memmove(p, p + 1, strlen(p));
  return base->type.ref->c;
}

/* -Warray-bounds=2: base[index] on the value stack, an array of known size
   and an index in a variable or a temp (not a constant: the frontend checks
   it): the subscript bounds.c checks on the raw IR, its id, or -1 */
static int fe_ab_site_begin(void)
{
  SValue *base = vtop - 1, *ix = vtop;
  char tbuf[64];
  int t, vt;
  if (!(base->type.t & VT_ARRAY) && (ix->type.t & VT_ARRAY))
    base = vtop, ix = vtop - 1;
  if (!(base->type.t & VT_ARRAY) || (base->type.t & VT_VLA) || !base->type.ref || base->type.ref->c < 0 ||
      base->union_member || fe_int_const(ix))
    return -1;
  t = ix->type.t & VT_BTYPE;
  if ((t != VT_INT && t != VT_SHORT && t != VT_BYTE && t != VT_BOOL) || (ix->type.t & (VT_BITFIELD | VT_VECTOR)) ||
      ix->vr < 0)
    return -1;
  vt = TCCIR_DECODE_VREG_TYPE(ix->vr);
  if ((ix->r & (VT_LVAL | VT_SYM)) == VT_LVAL &&
      ((ix->r & VT_VALMASK) == VT_LOCAL || ((ix->r & VT_PARAM) && (ix->r & VT_VALMASK) < VT_CONST)))
  {
    /* the variable itself, not a member of it */
    if ((vt != TCCIR_VREG_TYPE_VAR && vt != TCCIR_VREG_TYPE_PARAM) || !ix->sym || ix->sym->vreg != ix->vr ||
        (ix->sym->type.t & (VT_BTYPE | VT_ARRAY)) != t)
      return -1;
  }
  else if ((ix->r & VT_VALMASK) >= VT_CONST || (ix->r & VT_LVAL) || vt != TCCIR_VREG_TYPE_TEMP)
    return -1;
  CType ct = base->type;
  ct.t &= ~(VT_STORAGE | VT_CONSTANT | VT_VOLATILE);
  type_to_str(tbuf, sizeof tbuf, &ct, NULL);
  for (char *p = tbuf; (p = strstr(p, " [")) != NULL;)
    memmove(p, p + 1, strlen(p));
  return tcc_ir_ab_site(tcc_state->ir, ix->vr, !!(ix->type.t & VT_UNSIGNED), base->type.ref->c,
                        file ? file->line_num : 0, tbuf);
}

void unary_take_address(void)
{
  const int was_array = (vtop->type.t & VT_BTYPE) == VT_FUNC || (vtop->type.t & (VT_ARRAY | VT_VLA));
  const int os_rec = objsize_ctx && objsize_rec_is(vtop);
  /* &p: a followed pointer variable is no longer (builtin/objsize.c) */
  if (vtop->sym && (vtop->r & VT_LVAL) && (vtop->r & VT_VALMASK) == VT_LOCAL && vtop->type.ref == vtop->sym->type.ref &&
      (vtop->type.t & VT_BTYPE) == VT_PTR)
    objsize_addr_taken(vtop->sym);
  if ((vtop->type.t & VT_BTYPE) != VT_FUNC && !(vtop->type.t & (VT_ARRAY | VT_VLA)))
  {
    if (!(vtop->r & VT_LVAL) && (vtop->r & VT_VALMASK) == VT_CONST && vtop->sym != NULL)
    {
      vtop->r = VT_LVAL | VT_CONST | VT_SYM;
      vtop->c.i = 0;
      vtop->type = vtop->sym->type;
      vtop->vr = -1;
    }
    test_lvalue();
    /* (as gcc) -- except a constant address: offsetof's `&((T *)0)->m` */
    if (vtop->sso_reversed && (vtop->r & (VT_VALMASK | VT_SYM)) != VT_CONST)
      tcc_error("cannot take address of scalar with reverse storage order");
  }
  if (vtop->sym && ((vtop->r & VT_SYM) || (vtop->r & VT_LOCAL) || (vtop->r & VT_PARAM)))
  {
    if (tcc_state->stack_protector && !(vtop->r & VT_SYM))
      ssp_note_frame_use(0);
    if (tcc_state->defer_addrtaken && (vtop->r & (VT_VALMASK | VT_SYM | VT_PARAM)) == VT_LOCAL &&
        !vtop->sym->a.nested_func && tcc_state->nb_deferred_addrtaken < 4)
    {
      if (!tcc_state->nb_deferred_addrtaken)
        tcc_state->deferred_addr_lval = *vtop;
      tcc_state->deferred_addrtaken[tcc_state->nb_deferred_addrtaken++] = vtop->sym;
    }
    else
    {
      vtop->sym->a.addrtaken = 1;
      tcc_ir_set_addrtaken(tcc_state->ir, vtop->sym->vreg);
    }
    if (vtop->sym->a.nested_func)
      setup_nested_func_trampoline(vtop->sym);
  }
  {
    int is_vla_struct_local = struct_has_vla_member(&vtop->type) && (vtop->r & VT_VALMASK) == VT_LOCAL;
    mk_pointer(&vtop->type);
    if (!is_vla_struct_local)
    {
      gaddrof();
#ifdef CONFIG_TCC_BCHECK
      /* -b: a local scalar is registered where its address is taken */
      if (tcc_state->do_bounds_check && vtop->sym && !(vtop->r & VT_SYM) && !was_array)
        bcheck_addr_taken(vtop->sym);
#endif
    }
  }
  if (os_rec)
    objsize_after_addr(was_array);
}

/* vtop is a load-time constant a static initializer accepts: a constant or
   symbol address, or an anonymous object (compound literal). */
static int is_init_constant(SValue *sv)
{
  if ((sv->r & (VT_VALMASK | VT_LVAL)) == VT_CONST)
    return 1;
  return (sv->r & VT_VALMASK) == VT_CONST && (sv->r & (VT_SYM | VT_LVAL)) == (VT_SYM | VT_LVAL) &&
         sv->sym->v >= SYM_FIRST_ANOM;
}

/* GNU cast to union, `(union U)x`, after unary() parsed x. */
static __attribute__((noinline)) void unary_union_cast(CType *type)
{
  if ((vtop->type.t & VT_BTYPE) == VT_STRUCT && compare_types(type, &vtop->type, 1))
  {
    /* the operand already has the union type */
    gen_cast(type);
  }
  else
  {
    CType mtype = union_cast_member_type(type);

    if (global_expr && is_init_constant(vtop))
    {
      /* static initializer: build the union in an anonymous object that
         init_putv copies, as for a compound literal */
      init_params ip = {data_section};
      int u_align;
      int u_size = type_size(type, &u_align);
      unsigned long offset;
      if (NOSTATIC_WANTED)
        u_size = 0, u_align = 1;
      offset = section_add(ip.sec, u_size, u_align);
      vpush_ref(type, ip.sec, offset, u_size);
      vswap();
      init_putv(&ip, &mtype, offset, -1);
      vtop->r |= VT_LVAL;
    }
    else if (nocode_wanted)
    {
      vtop->type = *type;
    }
    else
    {
      int u_align;
      int u_size = type_size(type, &u_align);
      int vr_tmp;
      int tmp_loc = get_temp_local_var(u_size, u_align, &vr_tmp);

      gen_cast(&mtype);

      SValue dst_sv;
      memset(&dst_sv, 0, sizeof(dst_sv));
      dst_sv.type = mtype;
      dst_sv.r = VT_LOCAL | VT_LVAL;
      dst_sv.vr = vr_tmp;
      dst_sv.c.i = tmp_loc;

      vpushv(&dst_sv);
      vswap();
      vstore();
      vtop--;

      dst_sv.type = *type;
      vpushv(&dst_sv);
    }
  }
}

/* What unary_paren_type() found after '(' */
enum
{
  PAREN_NOT_TYPE,  /* an expression: nothing read */
  PAREN_SIZEOF,    /* sizeof/alignof (type): the type is pushed */
  PAREN_LITERAL,   /* a compound literal: done */
  PAREN_CAST,      /* (type) -- the operand is next */
  PAREN_UNION_CAST /* (union type) -- a GNU cast to union */
};

/* The type name of a cast, compound literal or sizeof/alignof operand after
   '(' (T: the token before it), into *TYPE.  Out of line: AttributeDef and
   the compound literal's locals stay out of unary_paren(), which recursion
   through nested parentheses stacks once a level. */
static __attribute__((noinline)) int unary_paren_type(int t, CType *type)
{
  int n, r;
  AttributeDef ad;

  if (!parse_btype(type, &ad, 0))
    return PAREN_NOT_TYPE;
  btype_no_auto(&ad);
  type_decl(type, &ad, &n, TYPE_ABSTRACT);
  skip(')');
  /* check ISOC99 compound literal */
  if (tok == '{')
  {
    /* data is allocated locally by default */
    if (global_expr)
      r = VT_CONST;
    else
      r = VT_LOCAL;
    /* all except arrays are lvalues */
    if (!(type->t & VT_ARRAY))
      r |= VT_LVAL;
    memset(&ad, 0, sizeof(AttributeDef));
    int lit_ir_start = tcc_state->ir ? tcc_state->ir->next_instruction_index : 0;
    /* sizeof/typeof operand at file scope: DATA_ONLY_WANTED makes the sign
     * bit win over the no-eval count, so the literal would still be laid
     * out in .data.  Drop it so only the type is completed. */
    int saved_nocode = nocode_wanted;
    if (global_expr && unevaluated_operand)
      nocode_wanted &= ~DATA_ONLY_WANTED;
    decl_initializer_alloc(type, &ad, r, 1, 0, 0);
    nocode_wanted = saved_nocode;
    /* A fully-constant vector compound literal only ever writes its own
     * brand-new stack object, whose address nothing else can hold yet.  Mark
     * the materialising stores/copy so an in-flight vector recipe is not
     * invalidated by them (see gen_op_vector's element-major fusion). */
    if (tcc_state->ir && (type->t & VT_VECTOR) && find_sv_vec_literal_init(vtop, 1))
      vec_emit_range_record(lit_ir_start, tcc_state->ir->next_instruction_index);
    return PAREN_LITERAL;
  }
  if (t == TOK_SOTYPE)
  { /* from sizeof/alignof (...) */
    vpush(type);
    return PAREN_SIZEOF;
  }
  return IS_UNION(type->t) ? PAREN_UNION_CAST : PAREN_CAST;
}

/* Parenthesized expression, cast, compound literal, or statement expression.
   Extracted from unary_primary() to keep its locals out of the main frame.
   Returns 1 for sizeof/alignof type-only operand (early return), 0 otherwise. */
__attribute__((noinline)) int unary_paren(void)
{
  int t, k;
  CType type;

  type.ref = NULL;
  t = tok;
  next();
  /* cast ? */
  if ((k = unary_paren_type(t, &type)) != PAREN_NOT_TYPE)
  {
    if (k == PAREN_SIZEOF)
      return 1; /* early return - skip postfix ops */
    if (k == PAREN_UNION_CAST)
    {
      /* GCC extension: (union_type) expr */
      unary();
      unary_union_cast(&type);
    }
    else if (k == PAREN_CAST)
    {
      unary();
      if ((type.t & VT_BTYPE) == VT_PTR)
        explicit_cast_checks(&type);
      gen_cast(&type);
    }
    if ((type.t & VT_BTYPE) == VT_VOID)
      unused_void_cast();
    else
      uv_last = UV_VALUE;
  }
  else if (tok == '{')
  {
    int saved_nocode_wanted = nocode_wanted;
    if (CONST_WANTED && !NOEVAL_WANTED)
      expect("constant");
    if (0 == local_scope)
      tcc_error("statement expression outside of function");
    block(STMT_EXPR);
    if (saved_nocode_wanted)
      nocode_wanted = saved_nocode_wanted;
    skip(')');
    uv_effects++, uv_last = UV_EFFECT;
  }
  else
  {
    gexpr();
    skip(')');
  }
  return 0;
}

/* _Generic() expression parser - extracted to reduce unary_primary() frame. */
__attribute__((noinline)) void unary_generic(void)
{
  CType controlling_type;
  int has_default = 0;
  int has_match = 0;
  int learn = 0;
  TokenString *str = NULL;
  int saved_nocode_wanted = nocode_wanted;
  nocode_wanted &= ~CONST_WANTED_MASK;

  next();
  skip('(');
  expr_type(&controlling_type, expr_eq);
  convert_parameter_type(&controlling_type);

  nocode_wanted = saved_nocode_wanted;

  for (;;)
  {
    learn = 0;
    skip(',');
    if (tok == TOK_DEFAULT)
    {
      if (has_default)
        tcc_error("too many 'default'");
      has_default = 1;
      if (!has_match)
        learn = 1;
      next();
    }
    else
    {
      AttributeDef ad_tmp;
      int itmp;
      CType cur_type;

      parse_btype(&cur_type, &ad_tmp, 0);
      btype_no_auto(&ad_tmp);
      type_decl(&cur_type, &ad_tmp, &itmp, TYPE_ABSTRACT);
      if (compare_types(&controlling_type, &cur_type, 0))
      {
        if (has_match)
        {
          tcc_error("type match twice");
        }
        has_match = 1;
        learn = 1;
      }
    }
    skip(':');
    if (learn)
    {
      if (str)
        tok_str_free(str);
      skip_or_save_block(&str);
    }
    else if (tcc_state->unused_on)
    {
      /* -Wunused-*: an association not chosen still names what it names */
      TokenString *other = NULL;
      skip_or_save_block(&other);
      unused_scan_tokens(other, NULL, 1);
      tok_str_free(other);
    }
    else
    {
      skip_or_save_block(NULL);
    }
    if (tok == ')')
      break;
  }
  if (!str)
  {
    char buf[60];
    type_to_str(buf, sizeof buf, &controlling_type, NULL);
    tcc_error("type '%s' does not match any association", buf);
  }
  begin_macro(str, 1);
  next();
  expr_eq();
  if (tok != TOK_EOF)
    expect(",");
  end_macro();
  next();
}

PackedMember packed_member_last, packed_member_addr;

/* vtop is still the lvalue the last packed member access left */
int packed_member_is_vtop(void)
{
  const SValue *a = &packed_member_last.sv;
  return packed_member_last.set && a->r == vtop->r && a->vr == vtop->vr && a->type.t == vtop->type.t &&
         a->type.ref == vtop->type.ref && a->c.i == vtop->c.i && (!(a->r & VT_SYM) || a->sym == vtop->sym);
}

/* After a member access: whether the member's place guarantees less than its
   type's alignment -- a packed struct's, or a member of one (BASE_ALIGN: what
   the enclosing member guaranteed, 0 for a whole object). */
static void packed_member_note(CType *struct_type, Sym *field, int cumofs, int base_align, const CType *base_st)
{
  int align, malign;
  const CType *named = struct_type; /* the packed struct the message names */
  packed_member_last.set = 0;
  if (field->type.t & VT_BITFIELD)
    return;
  type_size(struct_type, &align);
  if (base_align && base_align < align)
    align = base_align, named = base_st;
  if (cumofs & (align - 1))
    align = cumofs & -cumofs;
  type_size(&field->type, &malign);
  if (field->a.aligned && (1 << (field->a.aligned - 1)) > malign)
    malign = 1 << (field->a.aligned - 1);
  if (align >= malign)
    return;
  packed_member_last.sv = *vtop;
  packed_member_last.st = *named;
  packed_member_last.align = align;
  packed_member_last.set = 1;
}

/* Set by '&' for the unary() that parses its operand (see unary()). */
int unary_addr_operand;
/* indir() must leave its result an lvalue: the operand of '&' is built. */
int indir_keep_lvalue;

/* The postfix '.' / '->' of unary(): a member access (or a yasc method
   call).  Out of line, as unary_subscript() below: unary() recurses once per
   nested parenthesis, and these locals in its frame cost the device's 32 KiB
   stack hundreds of bytes a level (tests/ir_tests/test_parse_stack_depth.py).
   Returns -1 after a method call, else whether the member is a flexible-like
   array (-Warray-bounds). */
/* yasc: the chain of member accesses through a tagged union (`v->u.p.b`):
   the value the last one left, and whether the chain crossed a union that
   is not tagged before (U2 applies only then).  The union_member mark,
   which the alias classes need, cannot tell the two kinds apart. */
static struct
{
  int set, vr, untagged;
  unsigned short r;
  int64_t c;
} yasc_tchain;

static int yasc_tchain_is(const SValue *sv)
{
  return yasc_tchain.set && sv->vr == yasc_tchain.vr && (unsigned short)sv->r == yasc_tchain.r &&
         sv->c.i == yasc_tchain.c;
}

static void yasc_tchain_set(int on, int untagged)
{
  yasc_tchain.set = on;
  if (on)
    yasc_tchain.vr = vtop->vr, yasc_tchain.r = (unsigned short)vtop->r, yasc_tchain.c = vtop->c.i,
    yasc_tchain.untagged = untagged;
}

/* the union chain before this access crossed a union that is not tagged */
static int yasc_chain_untagged(const SValue *sv)
{
  return yasc_tchain_is(sv) ? yasc_tchain.untagged : sv->union_member;
}

/* -Wlifetime member paths: the chain of member accesses from one object
   (`v->in.p`, `a[1].in.p`): the offset of the member the last one left from
   the object's start, and the object's size (an element of an array of
   them).  A member record holds both, so that `v->in.p` is the path in
   place, not element 1 of an array of `in`s. */
static struct
{
  int set, vr, cum, esize;
  unsigned short r;
  int64_t c;
} lt_chain;

/* yasc register fields: the member accesses of this statement that named a
   `w1c` field (whole: not a bit-field) -- vstore notes a store to one.  More
   than the last: the right side of `u->icr = u->ris & u->imsc;` names
   members after the destination. */
#define YASC_W1C_SLOTS 4
static struct
{
  int vr, nf;
  unsigned short r;
  int64_t c;
  uint32_t bits;
  uint32_t fbits[8];
  int ftok[8];
} yasc_w1c_member[YASC_W1C_SLOTS];
static int yasc_nw1c_member; /* accesses noted this statement (slot: % YASC_W1C_SLOTS) */

/* DST (a store's destination) is the member just named, with w1c bits --
   its own, or those of the object it is part of at its place (`r->raw` of a
   union whose `b.irq` is w1c): the bits from its address, and the fields */
uint32_t yasc_w1c_member_of(const SValue *dst, uint32_t *fbits, int *ftok, int *nf)
{
  const int n = yasc_nw1c_member < YASC_W1C_SLOTS ? yasc_nw1c_member : YASC_W1C_SLOTS;
  for (int i = 0; i < n; i++)
  {
    const int j = (yasc_nw1c_member - 1 - i) % YASC_W1C_SLOTS; /* the latest first */
    if (dst->vr != yasc_w1c_member[j].vr || (unsigned short)dst->r != yasc_w1c_member[j].r ||
        dst->c.i != yasc_w1c_member[j].c)
      continue;
    for (int k = 0; k < yasc_w1c_member[j].nf && *nf < 8; k++)
      fbits[*nf] = yasc_w1c_member[j].fbits[k], ftok[(*nf)++] = yasc_w1c_member[j].ftok[k];
    return yasc_w1c_member[j].bits;
  }
  return 0;
}

/* the member on vtop, at CUMOFS of STRUCT_TYPE, is just named: noted when
   its object has w1c bits at its place (its own, if it is a w1c field, among
   them).  Out of line: the member access is on the parser's recursive path. */
static __attribute__((noinline)) void yasc_w1c_member_note(CType *struct_type, int cumofs)
{
  uint32_t ob[8], bits;
  int ot[8], onf = 0, a, sz = type_size(&vtop->type, &a);
  bits = !(vtop->type.t & VT_BITFIELD) && sz > 0 ? yasc_reg_w1c_mask(struct_type, cumofs, ob, ot, &onf) : 0;
  if (!bits)
    return;
  const uint32_t keep = sz >= 4 ? ~0u : (1u << (8 * sz)) - 1;
  bits &= keep;
  if (!bits)
    return;
  const int j = yasc_nw1c_member++ % YASC_W1C_SLOTS;
  yasc_w1c_member[j].nf = 0;
  for (int k = 0; k < onf; k++)
    if (ob[k] & keep)
      yasc_w1c_member[j].fbits[yasc_w1c_member[j].nf] = ob[k] & keep, yasc_w1c_member[j].ftok[yasc_w1c_member[j].nf++] = ot[k];
  yasc_w1c_member[j].vr = vtop->vr, yasc_w1c_member[j].r = (unsigned short)vtop->r;
  yasc_w1c_member[j].c = vtop->c.i, yasc_w1c_member[j].bits = bits;
}

/* ro / wo: what the access after member S (volatile, tok after it) does */
static void yasc_reg_member_check(Sym *st, Sym *s, int addr_operand)
{
  const int access = yasc_reg_access(st, s->v & ~SYM_FIELD);
  const int writes = tok == '=' || TOK_ASSIGN(tok) || tok == TOK_INC || tok == TOK_DEC;
  const int reads = tok != '=' && tok != '.' && tok != '[';
  if (addr_operand || nocode_wanted || !tcc_warn_wanted(warn_register_access))
    return;
  if (access == YASC_REG_RO && writes)
    tcc_warning_once(warn_register_access, "'%s' is a read-only register field: writing it does nothing, or worse",
                     get_tok_str(s->v & ~SYM_FIELD, NULL));
  else if (access == YASC_REG_WO && reads)
    tcc_warning_once(warn_register_access,
                     "'%s' is a write-only register field: reading it does not give what was written",
                     get_tok_str(s->v & ~SYM_FIELD, NULL));
}

/* yasc: the last member access that read a tagged union's tag (`v->k`):
   a switch on it must handle every value the union's members name */
static struct
{
  int set, vr, tu;
  unsigned short r;
  int64_t c;
} yasc_last_tag;

/* locals a tag was stored into (`enum vkind k = v->k;`), in the function
   being compiled: a switch on one handles every variant too */
static struct
{
  int var; /* its vreg */
  const char *func;
  int tu;
} yasc_tag_locals[16];
static int yasc_ntag_locals;

static int yasc_last_tag_is(const SValue *sv)
{
  return yasc_last_tag.set && sv->vr == yasc_last_tag.vr && (unsigned short)sv->r == yasc_last_tag.r &&
         sv->c.i == yasc_last_tag.c;
}

/* the local integer variable a value names (its vreg), -1 */
static int yasc_local_of(const SValue *sv)
{
  if (sv->vr < 0 || TCCIR_DECODE_VREG_TYPE(sv->vr) != TCCIR_VREG_TYPE_VAR || !is_integer_btype(sv->type.t & VT_BTYPE))
    return -1;
  return sv->vr;
}

/* a statement or declaration begins: the tag read before it is no switch's
   value, the w1c members named before it are no store's destination (vregs
   start again in the next function) */
void yasc_stmt_forget(void)
{
  yasc_last_tag.set = 0;
  yasc_nw1c_member = 0;
}

/* vstore: SRC stored into DST (a local): a tag just read makes it a copy of
   the tag, anything else makes it no longer one */
void yasc_tag_store_note(const SValue *dst, const SValue *src)
{
  const int var = yasc_local_of(dst);
  if (var < 0)
    return;
  int k;
  if (!yasc_last_tag_is(src))
  {
    for (k = 0; k < yasc_ntag_locals; k++)
      if (yasc_tag_locals[k].var == var && yasc_tag_locals[k].func == funcname)
        yasc_tag_locals[k].var = -1;
    return;
  }
  for (k = 0; k < yasc_ntag_locals; k++)
    if (yasc_tag_locals[k].var == var || yasc_tag_locals[k].func != funcname)
      break;
  if (k == yasc_ntag_locals)
  {
    if (k == 16)
      k = 0; /* forget the oldest: the check is a help, not a proof */
    else
      yasc_ntag_locals++;
  }
  yasc_tag_locals[k].var = var, yasc_tag_locals[k].func = funcname, yasc_tag_locals[k].tu = yasc_last_tag.tu;
}

/* SV (a switch's value) is a tagged union's tag, or a local one was stored
   into: the union's index + 1, else 0 */
int yasc_tag_switch_of(const SValue *sv)
{
  int var;
  if (yasc_last_tag_is(sv))
    return yasc_last_tag.tu + 1;
  if ((var = yasc_local_of(sv)) >= 0)
    for (int k = 0; k < yasc_ntag_locals; k++)
      if (yasc_tag_locals[k].var == var && yasc_tag_locals[k].func == funcname)
        return yasc_tag_locals[k].tu + 1;
  return 0;
}

/* the member (M >= 0) of tagged union TU whose address is on vtop is used
   here; M -1: its tag is (only a key for tcc_ir_tag_check) */
static void yasc_note_tagged(int tu, int m)
{
  const int line = file ? file->line_num : 0;
  if ((vtop->r & VT_VALMASK) < VT_CONST && TCCIR_DECODE_VREG_TYPE(vtop->vr) == TCCIR_VREG_TYPE_TEMP)
    tcc_ir_note_tagged(tcc_state->ir, 0, vtop->vr, 0, NULL, tu, m, line);
  else if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_LOCAL)
    tcc_ir_note_tagged(tcc_state->ir, 1, -1, (int32_t)vtop->c.i, NULL, tu, m, line);
  else if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM) && vtop->sym)
    tcc_ir_note_tagged(tcc_state->ir, 2, -1, (int32_t)vtop->c.i, vtop->sym, tu, m, line);
  else if (m >= 0)
    tcc_ir_note_tagged(tcc_state->ir, 0, -1, 0, NULL, tu, m, line); /* not followed: said */
}

static __attribute__((noinline)) int unary_member(int addr_operand)
{
  Sym *s;
  int qualifiers, cumofs, arrow = tok == TOK_ARROW, name_read = 0;
  int member_flex = 0; /* -Warray-bounds: it named a flexible-like array */
  /* yasc: `obj.method(...)` or `p->method(...)` (decl/method.c) */
  {
    const CType *st = &vtop->type;
    if (arrow)
      st = (st->t & VT_BTYPE) == VT_PTR ? pointed_type(&vtop->type) : NULL;
    if (st && (st->t & VT_BTYPE) == VT_STRUCT && (st->ref->ymod & YASC_HAS_METHODS))
    {
      next();
      if (yasc_method_postfix(st->ref, arrow))
      {
        uv_effects++, uv_last = UV_EFFECT;
        return -1;
      }
      name_read = 1;
    }
  }
  /* -Wunused-but-set-variable: `s.f = 1` with the address not folded */
  unsigned char uu_nr = 0;
  Sym *uu_base = !arrow && tcc_state->unused_on ? unused_part_begin(&uu_nr, 0) : NULL;
  /* __builtin_object_size: the member is the closest subobject */
  ObjRec os_saved;
  const int os_had = objsize_ctx ? objsize_member_begin(&os_saved) : 0;
  /* -Waddress-of-packed-member: `a.b.c` -- what `a.b` guaranteed */
  int pm_base_align = 0;
  CType pm_base_st = {0, NULL};
  if (!arrow && packed_member_last.set && tcc_warn_wanted(warn_address_of_packed_member) &&
      packed_member_is_vtop())
    pm_base_align = packed_member_last.align, pm_base_st = packed_member_last.st;
  /* field */
  if (arrow && tcc_state->warn_array_bounds)
    fe_check_deref_bounds(0); /* `(a + 4)->m` */
  if (arrow)
    indir();
  qualifiers = vtop->type.t & (VT_CONSTANT | VT_VOLATILE);
  test_lvalue();
  /* -fsanitize=null,alignment handlers: `p->m`, `(*p).m` */
  if (tcc_state->sanitize & (SAN_NULL | SAN_ALIGN))
    ubsan_access_kind(vtop, UBSAN_MEMBER);
  /* expect pointer on structure */
  if (!name_read)
    next();
  CType struct_type = vtop->type; /* save before find_field/type changes */
  s = find_field(&vtop->type, tok, &cumofs);
  if (s && s->a.attr_facts)
    attr_deprecated_use(s); /* a deprecated member */
  /* 64-bit deref alignment tracking: the member access is possibly
   * under-aligned (< 4) when the base lvalue already was (chained packed
   * access), when the struct type itself has alignment < 4 (packed /
   * #pragma pack), or when the member offset is not word-aligned.  The
   * mark is consumed by svalue_to_iroperand to keep 64-bit accesses off
   * the LDRD/STRD path, which faults on unaligned addresses. */
  int member_underaligned;
  {
    int salign;
    type_size(&struct_type, &salign);
    member_underaligned = vtop->underaligned || salign < 4 || (cumofs & 3) != 0;
  }
  /* Union-member accesses keep the alias-class marks off their IR loads
   * and stores: C permits reading a union member other than the one last
   * written, so pointer/non-pointer type-based disambiguation (C11 6.5p7)
   * must not apply across them.  Sticky in the lvalue like `underaligned`
   * above — the array-member offset arithmetic below mutates this SValue
     in place but never rebuilds it. */
  /* the access chain crossed a union: this member, or one it is part of
     (`x.u.st.p`); `->` starts a new chain (indir() left no mark) */
  const int via_union = IS_UNION(struct_type.t) || vtop->union_member;
  /* where this member's struct starts in the object the chain began at */
  const int lt_in_chain = !arrow && lt_chain.set && vtop->vr == lt_chain.vr &&
                          (unsigned short)vtop->r == lt_chain.r && vtop->c.i == lt_chain.c;
  const int lt_base_cum = lt_in_chain ? lt_chain.cum : 0, lt_base_esize = lt_in_chain ? lt_chain.esize : -1;
  /* yasc tagged union: a member of one is used where its tag selects it
     (lifetime.c tcc_ir_tag_check); no U2 for it or below it */
  YascTagged *ytu = NULL;
  int ytm = -1;
  if (yasc_ntagged && tcc_state->yasc && s && IS_UNION(struct_type.t) && (ytu = yasc_tagged_of(struct_type.ref)) &&
      ytu->resolved)
    ytm = yasc_tagged_member(ytu, s->v & ~SYM_FIELD);
  const int chain_untagged =
      tcc_state->yasc && ((!arrow && yasc_chain_untagged(vtop)) || (IS_UNION(struct_type.t) && ytm < 0));
  const int chain_tagged = tcc_state->yasc && (ytm >= 0 || (!arrow && yasc_tchain_is(vtop)));
  if (IS_UNION(struct_type.t))
    vtop->union_member = 1;
  /* -Warray-bounds: a trailing array member is taken as flexible (as gcc
     does), unless it has two or more elements and the struct is a named
     object, not reached through a pointer */
  if (s && (s->type.t & VT_ARRAY) && !s->next)
    member_flex = arrow || (s->type.t & VT_VLA) || s->type.ref->c <= 1 ||
                  !((vtop->r & VT_LVAL) && ((vtop->r & VT_VALMASK) == VT_LOCAL ||
                                            (vtop->r & (VT_VALMASK | VT_SYM)) == (VT_CONST | VT_SYM)));
  /* add field offset to pointer */
  if (struct_has_vla_member(&vtop->type) && (vtop->r & VT_VALMASK) == VT_LOCAL)
  {
    /* VLA struct stored via pointer indirection: load the data pointer
       from the pointer slot instead of computing its address.
       Works whether VT_LVAL is already set (normal variable reference)
       or not (e.g. from declaration context). */
    vtop->type = char_pointer_type;
    vtop->r |= VT_LVAL;
  }
  else
  {
    gaddrof();
    vtop->type = char_pointer_type; /* change type to 'char *' */
  }
  /* Check if any VLA field precedes the target field.  If so, the
     compile-time cumofs does not account for VLA field sizes and we
     must compute the full offset dynamically at runtime. */
  {
    int has_preceding_vla = 0;
    if ((struct_type.t & VT_BTYPE) == VT_STRUCT && struct_has_vla_member(&struct_type) &&
        struct_type.ref->type.t != VT_UNION)
    {
      Sym *f;
      for (f = sym_first(struct_type.ref); f && f != s; f = f->next)
      {
        if (f->type.t & VT_VLA)
        {
          has_preceding_vla = 1;
          break;
        }
      }
    }
    if (has_preceding_vla)
    {
      /* Walk all fields in order, computing the cumulative byte
         offset at runtime.  For each field we align to its
         required alignment, then (unless it is the target) add
         its runtime or compile-time size. */
      Sym *f;
      vpushi(0); /* running integer offset */
      for (f = sym_first(struct_type.ref); f; f = f->next)
      {
        int f_align, f_size;
        if (f->type.t & VT_VLA)
        {
          f_size = 0; /* determined at runtime */
          type_size(&f->type.ref->type, &f_align);
        }
        else
        {
          f_size = type_size(&f->type, &f_align);
          if (f_size < 0)
            f_size = 0;
        }
        /* honour explicit alignment attribute on the field */
        if (f->a.aligned)
        {
          int ea = 1 << (f->a.aligned - 1);
          if (ea > f_align)
            f_align = ea;
        }
        /* runtime: offset = (offset + align-1) & -align */
        if (f_align > 1)
        {
          vpushi(f_align - 1);
          gen_op('+');
          vpushi(~(f_align - 1));
          gen_op('&');
        }
        if (f == s)
          break; /* aligned to target field — done */
        /* add this field's size to the running offset */
        if (f->type.t & VT_VLA)
        {
          vset(&int_type, VT_LOCAL | VT_LVAL, f->type.ref->c);
        }
        else
        {
          vpushi(f_size);
        }
        gen_op('+');
      }
      gen_op('+'); /* pointer + computed_offset */
    }
    else
    {
      vpushi(cumofs);
      gen_op('+');
    }
  }
  /* change type to field type, and set to lvalue */
  vtop->type = s->type;
  vtop->type.t |= qualifiers;
  /* yasc: a member through a union holds what another member wrote, as far
     as the checks know (yasc_union_member_end after the member's name).
     The mark is kept for the members below it: the offset arithmetic above
     may have rebuilt the value (yasc only, so regular C compiles as before). */
  if (via_union && tcc_state->yasc)
    vtop->union_member = 1;
  const int yasc_union = chain_untagged && yasc_type_holds_annotated(&s->type, 0, 0);
  if (ytm >= 0 && tcc_state->ir && tcc_state->ir->lt_on && !nocode_wanted)
    yasc_note_tagged((int)(ytu - yasc_tagged), ytm);
  /* An array member of a qualified struct has qualified ELEMENTS: once
   * `r->ch` decays, `r->ch[1]` takes its type from the element, so a
   * qualifier left on the array type alone is lost there and two volatile
   * accesses to `r->ch[1]` fold into one. */
  if ((vtop->type.t & VT_ARRAY) && qualifiers)
    parse_btype_qualify(&vtop->type, qualifiers);
  /* Set even for array-typed members (no VT_LVAL): the mark rides the
   * SValue into pointer decay and gen_op('+') propagates it, so
   * packed->arr[i] still reaches the deref marked. */
  vtop->underaligned = member_underaligned;
  /* A scalar member of a big-endian scalar_storage_order aggregate, or an
   * array of them, is stored byte-reversed.  Set (or cleared) on every
   * access, like the alignment mark: the base may itself have been one. */
  vtop->sso_reversed = s->a.sso_be && sso_scalar_size(&vtop->type) != 0;
  /* -Wlifetime: a `* maybe` member is followed for null like a variable
     (lifetime.c), which needs the member's name and offset: the IR has
     lost member types.  What is recorded is the temp that holds the
     member's address; the checker finds the base pointer from the IR. */
  int lt_align, lt_esize = 0;
  /* ... and so is an `int owned(close) maybe fd` handle member: maybe
     there is "may be < 0", refined by the handle tests (LT_MEMBER_HANDLE) */
  /* ... and an owned member that is never null, for ownership only
     (LT_MEMBER_NOTMAYBE: no null checks) */
  const int lt_q = type_yasc(&s->type);
  const int lt_followed = !yasc_union &&
                          ((s->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR || type_is_handle(&s->type)) &&
                          !(lt_q & YASC_UNCHECKED) && (lt_q & (YASC_MAYBE | YASC_OWNED));
  const int lt_kind = ((lt_q & YASC_OWNED) ? LT_MEMBER_OWNED : 0) | (type_is_handle(&s->type) ? LT_MEMBER_HANDLE : 0) |
                      (!(lt_q & YASC_MAYBE) ? LT_MEMBER_NOTMAYBE : 0);
  if (tcc_state->ir && tcc_state->ir->lt_on && (struct_type.t & (VT_BTYPE | VT_ARRAY)) == VT_STRUCT)
    lt_esize = type_size(&struct_type, &lt_align);
  if (lt_base_esize >= 0)
    lt_esize = lt_base_esize; /* the outermost object's */
  const int lt_cum = lt_base_cum + cumofs;
  if (tcc_state->ir && tcc_state->ir->lt_on && !nocode_wanted && lt_followed &&
      (vtop->r & VT_VALMASK) < VT_CONST && TCCIR_DECODE_VREG_TYPE(vtop->vr) == TCCIR_VREG_TYPE_TEMP)
    tcc_ir_lt_note_member(tcc_state->ir, vtop->vr, lt_cum, s->v & ~SYM_FIELD, lt_kind, lt_esize);
  /* ... of a global struct (`g.next`): the address is a symbol plus an
     offset, used in place */
  else if (tcc_state->ir && tcc_state->ir->lt_on && !nocode_wanted && lt_followed &&
           (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM) && vtop->sym)
    tcc_ir_lt_note_gmember(tcc_state->ir, vtop->sym, (int)vtop->c.i, s->v & ~SYM_FIELD, lt_kind, lt_cum, lt_esize);
  /* ... and a borrows(member) member: reads of it are checked against
     its source member being freed or changed */
  if (tcc_state->ir && tcc_state->ir->lt_on && !nocode_wanted && !yasc_union && type_borrows_field(&s->type) >= 0 &&
      (vtop->r & VT_VALMASK) < VT_CONST && TCCIR_DECODE_VREG_TYPE(vtop->vr) == TCCIR_VREG_TYPE_TEMP)
    tcc_ir_lt_note_field_load(tcc_state->ir, vtop->vr, type_borrows_field(&s->type));
  uv_last = UV_VALUE;
  /* an array (or VLA) is never an lvalue */
  if (!(vtop->type.t & (VT_ARRAY | VT_VLA)))
  {
    vtop->r |= VT_LVAL;
#ifdef CONFIG_TCC_BCHECK
    /* if bound checking, the referenced pointer must be checked */
    if (tcc_state->do_bounds_check)
      vtop->r |= VT_MUSTBOUND;
#endif
  }
  if (tcc_state->yasc)
    yasc_tchain_set(chain_tagged, chain_untagged);
  if (tcc_state->ir && tcc_state->ir->lt_on)
  {
    lt_chain.set = 1, lt_chain.vr = vtop->vr, lt_chain.r = (unsigned short)vtop->r, lt_chain.c = vtop->c.i;
    lt_chain.cum = lt_cum, lt_chain.esize = lt_esize;
  }
  /* ... and the tag of one: `switch (v->k)` */
  yasc_last_tag.set = 0;
  if (yasc_ntagged && tcc_state->yasc && !IS_UNION(struct_type.t) && !(vtop->type.t & VT_ARRAY))
    for (int i = 0; i < yasc_ntagged; i++)
      if (yasc_tagged[i].outer == struct_type.ref && yasc_tagged[i].tag_tok == (s->v & ~SYM_FIELD) &&
          yasc_tagged[i].resolved)
      {
        if (yasc_tagged[i].nown && tcc_state->ir && tcc_state->ir->lt_on && !nocode_wanted)
        {
          /* a function that only changes the tag is checked too */
          const unsigned short r_saved = vtop->r;
          vtop->r &= ~VT_LVAL;
          yasc_note_tagged(i, -1);
          vtop->r = r_saved;
        }
        yasc_last_tag.set = 1, yasc_last_tag.tu = i;
        yasc_last_tag.vr = vtop->vr, yasc_last_tag.r = (unsigned short)vtop->r, yasc_last_tag.c = vtop->c.i;
        break;
      }
  if (member_underaligned && tcc_warn_wanted(warn_address_of_packed_member))
    packed_member_note(&struct_type, s, cumofs, pm_base_align, &pm_base_st);
  if (member_flex && tcc_state->warn_array_bounds)
    fe_note_flex_member(); /* `s.tail + 4` is not checked either */
  if (uu_base)
    unused_part_end(uu_base, uu_nr, 0);
  if (objsize_ctx)
    objsize_member_end(&os_saved, os_had, arrow, s, cumofs, member_flex);
  /* register fields: a w1c one (whole) for vstore; ro / wo by what comes next */
  if (yasc_nreg && tcc_state->yasc)
    yasc_w1c_member_note(&struct_type, cumofs);
  const int reg_check = yasc_nreg && tcc_state->yasc && (vtop->type.t & VT_VOLATILE) &&
                        yasc_reg_access(struct_type.ref, s->v & ~SYM_FIELD);
  next();
  if (reg_check)
    yasc_reg_member_check(struct_type.ref, s, addr_operand);
  if (yasc_union)
    yasc_union_member_end(addr_operand);
  return member_flex;
}

/* What unary_subscript() keeps across the index expression, for
   unary_subscript_end(). */
typedef struct SubscriptState
{
  Sym *uu_base;      /* -Wunused-but-set-variable: `a[i] = 0` */
  CType pm_st;       /* -Waddress-of-packed-member: the packed array member's */
  int pm_align;      /* struct and what its place guarantees, when pm_set */
  unsigned char flex, addr_operand, uu_nr, pm_set;
  unsigned char via_union; /* yasc: an array member reached through a union */
} SubscriptState;

/* The index of base[index] while __builtin_object_size follows a pointer:
   the index is not the pointer whose object is wanted. */
static __attribute__((noinline)) void subscript_index_objsize(void)
{
  ObjRec os_saved = objsize_rec;
  const int os_ctx = objsize_ctx;
  objsize_ctx = 0;
  gexpr();
  objsize_ctx = os_ctx;
  objsize_rec = os_saved;
}

/* unary_subscript() after the index: base and index on the value stack. */
static __attribute__((noinline)) void unary_subscript_end(const SubscriptState *ss)
{
  const int flex = ss->flex, addr_operand = ss->addr_operand;
  int64_t bounds_idx = 0;
  int bounds_n = -1, ab_site = -1;
  char bounds_type[64];
  int pm_k = ss->pm_set && fe_int_const(vtop), big;
  int64_t pm_idx = pm_k ? fe_const_value(vtop, &big) : 0;
  if (tcc_state->warn_array_bounds && !flex && fe_eval_checks_wanted() && !(nocode_wanted & CODE_OFF_BIT))
    bounds_n = fe_subscript_bounds(&bounds_idx, bounds_type, sizeof bounds_type);
  /* ... and a loop's last trip: a[i] for (i = 0; i <= N; i++) (objsize.c) */
  AbSub ab_sub;
  const int ab_hit = objsize_ab_watch && fe_eval_checks_wanted() && objsize_ab_subscript_begin(&ab_sub, flex);
  if (is_vector_type(&vtop[-1].type))
  {
    /* GCC vector subscript: vec[i] -> element of the vector. */
    gen_vec_subscript();
  }
  else
  {
    /* A subscript `base[idx]` on a VT_SYM global (array or pointer) is
     * treated as a potential write path: gen_op('+') below will often
     * materialize the base into a register, stripping VT_SYM from the
     * resulting lvalue, so vstore's sym-based poisoning can't catch a
     * later store through this lvalue.  Poison here if the pointee is
     * non-const.  Cost: blocks `*&g` scalar folds for syms that are
     * also subscripted — but scalar syms aren't subscripted. */
    if (!nocode_wanted && (vtop[-1].r & (VT_VALMASK | VT_SYM)) == (VT_CONST | VT_SYM) && vtop[-1].sym)
    {
      CType *pointed = NULL;
      if (vtop[-1].type.t & VT_ARRAY)
        pointed = &vtop[-1].type; /* array element type */
      else if ((vtop[-1].type.t & VT_BTYPE) == VT_PTR)
        pointed = pointed_type(&vtop[-1].type);
      if (pointed && !(pointed->t & VT_CONSTANT))
        vtop[-1].sym->a.possibly_written = 1;
    }
    if ((tcc_state->sanitize & SAN_BOUNDS) && !flex)
      ubsan_subscript(addr_operand);
    if (tcc_state->ir && tcc_state->ir->ab_on && !flex && !nocode_wanted && !tcc_state->in_inline_expansion &&
        fe_eval_checks_wanted())
      ab_site = fe_ab_site_begin();
    gen_op('+');
    if (ab_site >= 0)
      tcc_ir_ab_site_mid(tcc_state->ir, ab_site);
    indir();
  }
  if (ss->uu_base)
    unused_part_end(ss->uu_base, ss->uu_nr, 1);
  if (ss->pm_set)
  {
    int ealign, esize = type_size(&vtop->type, &ealign);
    int64_t off = pm_k ? pm_idx * esize : esize;
    int align = ss->pm_align;
    if (off && (int)(off & -off) < align)
      align = (int)(off & -off);
    if (esize > 0 && align < ealign)
    {
      packed_member_last.sv = *vtop;
      packed_member_last.st = ss->pm_st;
      packed_member_last.align = align;
      packed_member_last.set = 1;
    }
  }
  skip(']');
  if (ab_hit)
    objsize_ab_subscript_end(&ab_sub, addr_operand && !(tok == '[' || tok == '.' || tok == TOK_ARROW || tok == '(' ||
                                                        tok == TOK_INC || tok == TOK_DEC));
  if (ab_site >= 0)
    tcc_ir_ab_site_end(tcc_state->ir, ab_site,
                       addr_operand && !(tok == '[' || tok == '.' || tok == TOK_ARROW || tok == '(' ||
                                         tok == TOK_INC || tok == TOK_DEC));
  /* one past the end is a valid address, &a[n], not an element */
  if (bounds_n >= 0 &&
      (bounds_idx < 0 || bounds_idx > bounds_n ||
       (bounds_idx == bounds_n && (!addr_operand || tok == '[' || tok == '.' || tok == TOK_ARROW || tok == '(' ||
                                   tok == TOK_INC || tok == TOK_DEC))))
    tcc_fe_warning_c(warn_array_bounds, "array subscript %lld is %s array bounds of '%s'", (long long)bounds_idx,
                     bounds_idx < 0 ? "below" : "above", bounds_type);
  uv_last = UV_VALUE;
}

/* The postfix '[' of unary(): base[index].  FLEX: the base is a
   flexible-like array member; ADDR_OPERAND: the operand of '&'.  Its frame
   is on the recursive path of an index expression: what the rest needs
   waits in a SubscriptState, the rest is unary_subscript_end(). */
static __attribute__((noinline)) void unary_subscript(int flex, int addr_operand)
{
  SubscriptState ss;
  /* yasc: p[i] with p a regular-C pointer (indir() sees p + i, a new type) */
  if (tcc_state->yasc && (vtop->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR &&
      (type_yasc(&vtop->type) & YASC_UNCHECKED) && tcc_warn_wanted(warn_unchecked))
    yasc_unchecked_deref();
  ss.flex = flex;
  ss.addr_operand = addr_operand;
  ss.via_union = tcc_state->yasc && (vtop->type.t & VT_ARRAY) && yasc_chain_untagged(vtop) &&
                 yasc_type_holds_annotated(&vtop->type, 0, 0);
  const int sub_tagged = tcc_state->yasc && (vtop->type.t & VT_ARRAY) && yasc_tchain_is(vtop);
  const int sub_untagged = tcc_state->yasc && yasc_chain_untagged(vtop);
  /* -Waddress-of-packed-member: an element of a packed array member
     (`&p->arr[i]`, `&p->items[i].x`) is no better aligned than it */
  ss.pm_set = packed_member_last.set && (vtop->type.t & VT_ARRAY) &&
              tcc_warn_wanted(warn_address_of_packed_member) && packed_member_is_vtop();
  ss.pm_align = 0;
  ss.pm_st.t = 0, ss.pm_st.ref = NULL;
  if (ss.pm_set)
    ss.pm_st = packed_member_last.st, ss.pm_align = packed_member_last.align;
  ss.uu_nr = 0;
  ss.uu_base = tcc_state->unused_on ? unused_part_begin(&ss.uu_nr, 1) : NULL; /* `a[i] = 0` */
  next();
  /* yasc: s[i] of a slice, and slicing x[a:b]; the index is not the
     pointer __builtin_object_size follows (subscript_index_objsize) */
  if (tcc_state->yasc)
  {
    ObjRec os_saved = objsize_rec;
    const int os_ctx = objsize_ctx;
    objsize_ctx = 0;
    const int done = yasc_subscript();
    objsize_ctx = os_ctx;
    objsize_rec = os_saved;
    if (done)
    {
      if (ss.uu_base)
        unused_part_end(ss.uu_base, ss.uu_nr, 1);
      return;
    }
  }
  else if (objsize_ctx)
    subscript_index_objsize();
  else
    gexpr();
  unary_subscript_end(&ss);
  /* `v->u.arr[i].p`: the chain goes on through the element */
  if (tcc_state->yasc)
    yasc_tchain_set(sub_tagged, sub_untagged);
  if (ss.via_union)
  {
    vtop->union_member = 1;
    yasc_union_member_end(addr_operand);
  }
}

ST_FUNC HOT void unary(void)
{
  /* This is the operand of '&': it stays the lvalue it names -- indir()
     folds no load into a constant, a __rodata_relative pointer stays a word.
     The flag is the caller's: the expressions nested in this one get 0. */
  const int addr_operand = unary_addr_operand;
  const int saved_keep_lvalue = indir_keep_lvalue;
  unary_addr_operand = 0;
  indir_keep_lvalue = addr_operand;
  /* a new operand: the last packed member access is not what it will leave */
  packed_member_last.set = 0;

  /* generate line number info */
  if (debug_modes)
    tcc_debug_line(tcc_state), tcc_tcov_check_line(tcc_state, 1);

  /* yasc: try X, error.NAME, error_from(x) */
  if (tcc_state->yasc &&
      (tok == TOK_try || tok == TOK_ERROR || tok == TOK_error_from || tok == TOK_error_name || tok == TOK_unreachable) &&
      yasc_unary())
  {
    uv_effects++, uv_last = UV_EFFECT;
    goto postfix;
  }
  /* yasc: move(p), borrow(p) */
  if (tcc_state->yasc && (tok == TOK_move || tok == TOK_borrow || tok == TOK_unsafe) &&
      (tok == TOK_unsafe ? yasc_unsafe_expr() : yasc_move_borrow()))
  {
    uv_effects++, uv_last = UV_EFFECT;
    goto postfix;
  }

  /* Handle simple prefix operators directly to avoid entering
     unary_primary()'s large stack frame on the recursive path. */
  switch (tok)
  {
  case '*':
    next();
    unary();
    if (tcc_state->warn_array_bounds)
      fe_check_deref_bounds(addr_operand); /* `*(a + 4)` */
    indir();
    uv_last = UV_VALUE;
    goto postfix;
  case '!':
    next();
    unary();
    gen_test_zero(TOK_EQ);
    uv_last = UV_VALUE;
    goto postfix;
  case TOK_INC:
  case TOK_DEC:
  {
    int t = tok;
    Sym *lhs;
    next();
    unary();
    lhs = unused_lhs();
    inc(0, t);
    unused_assigned_to(lhs);
    uv_last = UV_INCDEC;
  }
    goto postfix;
  case '-':
    next();
    unary();
    if (is_float(vtop->type.t))
    {
      gen_opif(TOK_NEG);
    }
    else
    {
      fe_check_neg();
      fe_narrow_pend_begin(NULL); /* -Wconversion: `uc = -uc` is quiet, as gcc */
      vpushi(0);
      vswap();
      if (!(tcc_state->sanitize && ubsan_gen_neg()))
        gen_op('-');
      fe_narrow_pend_end();
    }
    uv_last = UV_VALUE;
    goto postfix;
  case '~':
    next();
    unary();
    if (vtop->type.t & VT_COMPLEX)
    {
      gen_complex_conjugate();
    }
    else
    {
      vpushi(-1);
      gen_op('^');
    }
    uv_last = UV_VALUE;
    goto postfix;
  case '+':
    next();
    unary();
    if ((vtop->type.t & VT_BTYPE) == VT_PTR)
      tcc_error("pointer not accepted for unary plus");
    if (!is_float(vtop->type.t))
    {
      vpushi(0);
      gen_op('+');
    }
    uv_last = UV_VALUE;
    goto postfix;
  case '&':
  {
    int packed;
    next();
    unary_addr_operand = 1;
    unary();
    packed = packed_member_last.set && tcc_warn_wanted(warn_address_of_packed_member) && packed_member_is_vtop();
    unary_take_address();
    vtop->packed_addr = packed;
    if (packed)
      packed_member_addr = packed_member_last;
    uv_last = UV_VALUE;
  }
    goto postfix;
  case TOK_SOTYPE:
  case '(':
    if (unary_paren())
      goto done;
    goto postfix;
  default:
    break;
  }

  {
    /* -Wunused-value: a builtin (__sync_*, __builtin_*, va_arg, ...) may
       have effects; sizeof, _Alignof, _Generic and constants do not */
    const int t0 = tok;
    uv_ident = 0;
    if (unary_primary())
      goto done;
    if (t0 < TOK_UIDENT ? !TOK_HAS_VALUE(t0) : uv_ident != t0)
    {
      if (t0 == TOK_SIZEOF || t0 == TOK_ALIGNOF1 || t0 == TOK_ALIGNOF2 || t0 == TOK_ALIGNOF3 || t0 == TOK_GENERIC ||
          t0 == TOK_LAND || t0 == TOK_builtin_constant_p || t0 == TOK_builtin_types_compatible_p)
        uv_last = UV_VALUE;
      else
        uv_effects++, uv_last = UV_EFFECT;
    }
  }

postfix:
  /* post operations */
  int member_flex = 0; /* -Warray-bounds: the last '.'/'->' named a flexible-like array */
  while (1)
  {
    const int flex = member_flex;
    member_flex = 0;
    /* Every postfix operator reads the pointer, but ++/-- would write it. */
    if ((vtop->type.t & VT_RODATA_REL) && (vtop->r & VT_LVAL))
    {
      if (tok == TOK_INC || tok == TOK_DEC)
        rodata_rel_store_error();
      if (tok == TOK_ARROW || tok == '[' || tok == '(')
        rodata_rel_rvalue();
    }
    if (tok == TOK_INC || tok == TOK_DEC)
    {
      Sym *lhs = unused_lhs();
      inc(1, tok);
      unused_assigned_to(lhs);
      uv_last = UV_INCDEC;
      next();
    }
    else if (tok == '.' && MAYBE_ERRUNION(&vtop->type) && type_is_slice(&vtop->type))
    {
      next();
      yasc_slice_member(); /* s.ptr, s.len */
    }
    else if (tok == '.' || tok == TOK_ARROW)
    {
      const int mf = unary_member(addr_operand);
      if (mf < 0)
        continue;
      member_flex = mf;
    }
    else if (tok == '[')
    {
      unary_subscript(flex, addr_operand);
    }
    else if (tok == '(')
    {
      unary_funcall();
      uv_effects++, uv_last = UV_EFFECT;
    }
    else
    {
      break;
    }
  }
done:
  indir_keep_lvalue = saved_keep_lvalue;
  if (!addr_operand && (vtop->type.t & VT_RODATA_REL) && (vtop->r & VT_LVAL))
  {
    /* The left side of an assignment: say why it cannot be one. */
    if (tok == '=' || TOK_ASSIGN(tok))
      rodata_rel_store_error();
    rodata_rel_rvalue();
  }
}
