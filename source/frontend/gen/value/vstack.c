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

/* vstack.c -- Value-stack primitives: push/pop/rotate and CPU-flag value handling.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */
static void vcheck_cmp(void)
{
  /* cannot let cpu flags if other instruction are generated. Also
     avoid leaving VT_JMP anywhere except on the top of the stack
     because it would complicate the code generator.

     Don't do this when nocode_wanted.  vtop might come from
     !nocode_wanted regions (see 88_codeopt.c) and transforming
     it to a register without actually generating code is wrong
     as their value might still be used for real.  All values
     we push under nocode_wanted will eventually be popped
     again, so that the VT_CMP/VT_JMP value will be in vtop
     when code is unsuppressed again. */

  /* However if it's just automatic suppression via CODE_OFF/ON()
     then it seems that we better let things work undisturbed.
     How can it work at all under nocode_wanted?  Well, gv() will
     actually clear it at the gsym() in load()/VT_JMP in the
     generator backends */

  // if (vtop->r == VT_CMP && 0 == (nocode_wanted & ~CODE_OFF_BIT))
  // gv(RC_INT);
  if (vtop >= vstack && (0 == (nocode_wanted & ~CODE_OFF_BIT)))
  {
    // if (vtop->r == VT_CMP) {
    // vset_VT_JMP();
    // }
    tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
  }
}

void vsetc(CType *type, int r, CValue *vc)
{
  if (vtop >= vstack + (VSTACK_SIZE - 1))
    tcc_error("memory full (vstack)");

  vcheck_cmp();
  vtop++;
  print_vstack("vsetc");
  vtop->type = *type;
  vtop->r = r;
  vtop->c = *vc;
  vtop->vr = -1;
  vtop->pr0_reg = PREG_REG_NONE;
  vtop->pr0_spilled = 0;
  vtop->underaligned = 0; /* fresh value: alignment derives from its type;
                           * a stale 1 from a reused vstack slot would
                           * needlessly block LDRD/STRD pairing */
  vtop->volatile_access = 0; /* likewise: volatility derives from the fresh type */
  vtop->pr1_reg = PREG_REG_NONE;
  vtop->pr1_spilled = 0;
  vtop->sym = NULL;
  /* Note: jtrue/jfalse are in a union with c, so we DON'T initialize them here.
     They should only be used when r == VT_CMP, and c is used otherwise. */
}
ST_FUNC void vswap(void)
{
  SValue tmp;

  vcheck_cmp();
  tmp = vtop[0];
  vtop[0] = vtop[-1];
  vtop[-1] = tmp;
}

/* pop stack value */
ST_FUNC void vpop(void)
{
  int v;
  v = vtop->r & VT_VALMASK;
#if defined(TCC_TARGET_I386) || defined(TCC_TARGET_X86_64)
  /* for x86, we need to pop the FP stack */
  if (v == TREG_ST0)
  {
    o(0xd8dd); /* fstp %st(0) */
  }
  else
#endif
      if (v == VT_CMP)
  {
    /* need to put correct jump if && or || without test */
    /* Use IR backpatching - jtrue/jfalse use -1 as "no chain" sentinel */
    if (vtop->jtrue >= 0)
      tcc_ir_backpatch_to_here(tcc_state->ir, vtop->jtrue);
    if (vtop->jfalse >= 0)
      tcc_ir_backpatch_to_here(tcc_state->ir, vtop->jfalse);
  }
  vtop--;
  print_vstack("vpop");
}

/* An expression whose value is discarded still has to perform its volatile
 * read: `*p;` and a volatile operand of a comma expression or a for-loop
 * increment are mandated accesses (the read-to-clear MMIO idiom is exactly
 * this shape), but vpop drops the lvalue without ever loading it.  Structs are
 * left alone — there is no register to load one into.  The `(void)x` spelling
 * is handled where the cast erases the type, in gen_cast. */
ST_FUNC void gv_discarded_volatile(void)
{
  if (nocode_wanted)
    return;
  if (!(vtop->r & VT_LVAL))
    return;
  int bt = vtop->type.t & VT_BTYPE;
  if (bt == VT_STRUCT || bt == VT_VOID || (vtop->type.t & (VT_ARRAY | VT_VLA)))
    return;
  if (!((vtop->type.t & VT_VOLATILE) || vtop->volatile_access ||
        (vtop->sym && (vtop->sym->type.t & VT_VOLATILE))))
    return;
  gv(RC_TYPE(vtop->type.t));
}

/* push constant of type "type" with useless value */
void vpush(CType *type)
{
  vset(type, VT_CONST, 0);
}

/* push arbitrary 64bit constant */
void vpush64(int ty, unsigned long long v)
{
  CValue cval;
  CType ctype;
  ctype.t = ty;
  ctype.ref = NULL;
  cval.i = v;
  vsetc(&ctype, VT_CONST, &cval);
}

/* push integer constant */
ST_FUNC void vpushi(int v)
{
  vpush64(VT_INT, v);
}

/* push a pointer sized constant */
void vpushs(addr_t v)
{
  vpush64(VT_SIZE_T, v);
}

/* push long long constant */
void vpushll(long long v)
{
  vpush64(VT_LLONG, v);
}

ST_FUNC void vset(CType *type, int r, int v)
{
  CValue cval;
  cval.i = v;
  vsetc(type, r, &cval);
}

void vseti(int r, int v)
{
  CType type;
  type.t = VT_INT;
  type.ref = NULL;
  vset(&type, r, v);
}

ST_FUNC void vpushv(SValue *v)
{
  if (vtop >= vstack + (VSTACK_SIZE - 1))
    tcc_error("memory full (vstack)");
  vtop++;
  print_vstack("vpushv");
  *vtop = *v;
}

void vdup(void)
{
  vpushv(vtop);
}

/* rotate the stack element at position n-1 to the top */
ST_FUNC void vrotb(int n)
{
  SValue tmp;
  if (--n < 1)
    return;
  vcheck_cmp();
  tmp = vtop[-n];
  memmove(vtop - n, vtop - n + 1, sizeof *vtop * n);
  vtop[0] = tmp;
}

/* rotate the top stack element into position n-1 */
ST_FUNC void vrott(int n)
{
  SValue tmp;
  if (--n < 1)
    return;
  vcheck_cmp();
  tmp = vtop[0];
  memmove(vtop - n + 1, vtop - n, sizeof *vtop * n);
  vtop[-n] = tmp;
}

/* reverse order of the the first n stack elements */
ST_FUNC void vrev(int n)
{
  int i;
  SValue tmp;
  vcheck_cmp();
  for (i = 0, n = -n; i > ++n; --i)
    tmp = vtop[i], vtop[i] = vtop[n], vtop[n] = tmp;
}

/* ------------------------------------------------------------------------- */
/* vtop->r = VT_CMP means CPU-flags have been set from comparison or test. */

/* called from generators to set the result from relational ops  */
ST_FUNC void vset_VT_CMP(int op)
{
  vtop->r = VT_CMP;
  vtop->cmp_op = op;
  vtop->jfalse = -1; /* -1 = no chain */
  vtop->jtrue = -1;  /* -1 = no chain */
}

/* called once before asking generators to load VT_CMP to a register */
void vset_VT_JMP(void)
{
  if (vtop->r != VT_CMP)
    return;

  int op = vtop->cmp_op;

  // if (vtop->jtrue || vtop->jfalse) {
  int origt = vtop->type.t;
  /* we need to jump to 'mov $0,%R' or 'mov $1,%R' */
  int inv = op & (op < 2); /* small optimization */
  /* -1, not 0: 0 is a valid IR instruction index, so passing it as the
   * incoming chain links the emitted JUMPIF to instruction 0 and a later
   * backpatch of this chain walks into (and retargets) that instruction. */
  int test = tcc_ir_codegen_test_gen(tcc_state->ir, inv, -1);
  vseti(VT_JMP + inv, test);
  vtop->type.t |= origt & (VT_UNSIGNED | VT_DEFSIGN);
  // } else {
  /* otherwise convert flags (rsp. 0/1) to register */
  // vtop->c.i = op;
  // if (op < 2) /* doesn't seem to happen */
  // vtop->r = VT_CONST;
  // }
}

/* Set CPU Flags, doesn't yet jump */
void gvtst_set(int inv, int t)
{
  int *p;
  // SValue dest;

  if (vtop->r != VT_CMP)
  {
    vpushi(0);
    gen_op(TOK_NE);
    if (vtop->r != VT_CMP) /* must be VT_CONST then */
      vset_VT_CMP(vtop->c.i != 0 ? TOK_NE : TOK_EQ);
  }

  p = inv ? &vtop->jfalse : &vtop->jtrue;
  *p = tcc_ir_gjmp_append(tcc_state->ir, *p, t);
  // tcc_ir_codegen_test_gen(tcc_state->ir, inv, t);
  // if (vtop->)
  // *p = tcc_ir_gjmp_append(tcc_state->ir, *p, t);
  // tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
}

/* generate a zero or nozero test */
void gen_test_zero(int op)
{
  if (vtop->r == VT_CMP)
  {
    int j;
    if (op == TOK_EQ)
    {
      j = vtop->jfalse;
      vtop->jfalse = vtop->jtrue;
      vtop->jtrue = j;
      vtop->cmp_op ^= 1;
    }
  }
  else
  {
    vpushi(0);
    gen_op(op);
  }
}

void check_nonvoid_value(void)
{
  if ((vtop->type.t & VT_BTYPE) == VT_VOID)
    tcc_error("void value not ignored as it ought to be");
}

/* ------------------------------------------------------------------------- */
/* push a symbol value of TYPE */
ST_FUNC void vpushsym(CType *type, Sym *sym)
{
  CValue cval;
  cval.i = 0;
  vsetc(type, VT_CONST | VT_SYM, &cval);
  vtop->sym = sym;
}

/* Return a static symbol pointing to a section */
ST_FUNC Sym *get_sym_ref(CType *type, Section *sec, unsigned long offset, unsigned long size)
{
  int v;
  Sym *sym;

  v = anon_sym++;
  sym = sym_push(v, type, VT_CONST | VT_SYM, 0);
  sym->type.t |= VT_STATIC;
  /* Use put_extern_sym2 directly to bypass the nocode_wanted guard in
   * put_extern_sym.  Anonymous data symbols (string literals, float
   * constants) must always have a valid ELF entry because the IR
   * backend emits instructions under CODE_OFF_BIT that may reference
   * them.  Those IR instructions are later removed by DCE, but the
   * symbol must exist during compilation.  Under NODATA_WANTED the
   * caller passes size=0, so no section data is wasted. */
  put_extern_sym2(sym, sec ? sec->sh_num : SHN_UNDEF, offset, size, 1);
  return sym;
}

/* push a reference to a section offset by adding a dummy symbol */
void vpush_ref(CType *type, Section *sec, unsigned long offset, unsigned long size)
{
  vpushsym(type, get_sym_ref(type, sec, offset, size));
}
