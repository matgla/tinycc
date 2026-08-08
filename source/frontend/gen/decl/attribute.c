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

/* attribute.c -- __attribute__ and C23 [[...]] attribute parsing.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* post defines POST/PRE add. c is the token ++ or -- */
ST_FUNC void inc(int post, int c)
{
  test_lvalue();
  vdup(); /* save lvalue */
  if (post)
  {
    gv_dup(); /* duplicate value */
    vrotb(3);
    vrotb(3);
  }
  /* add constant */
  vpushi(c - TOK_MID);
  gen_op('+');

  /* For pre-increment on captured variables (nested functions): save the new
   * value before vstore(), because vstore() uses STORE (not ASSIGN) for
   * captured vars (vr == -1), leaving the destination lvalue on vtop instead
   * of the stored value.  We restore the saved value after the store. */
  SValue saved_new_value;
  int captured_preinc = 0;
  if (!post && tcc_state->ir && (vtop[-1].r & VT_VALMASK) == VT_LOCAL && vtop[-1].vr == -1 && (vtop[-1].r & VT_LVAL))
  {
    saved_new_value = *vtop; /* save computed new value (N+1 / N-1) */
    captured_preinc = 1;
  }

  vstore(); /* store value */
  if (post)
    vpop(); /* if post op, return saved value */
  else if (captured_preinc)
  {
    /* Replace the destination lvalue left by vstore() with the saved new
     * value so the expression evaluates to the incremented result. */
    *vtop = saved_new_value;
  }
  else if (tcc_state->ir)
  {
    /* Pre-increment/decrement: the result of vstore() is the destination vreg
     * with r=0.  If that vreg corresponds to a local variable (a stack slot),
     * later dereference via indir() will see {r=0, vr=local_vreg} and, after
     * the register allocator spills it, generate a single byte/word load
     * directly from the stack slot instead of the required two-step sequence
     * (load pointer from slot, then load through pointer).
     *
     * Fix: emit an explicit LOAD of the stored value into a fresh temp vreg.
     * This materializes the value so that subsequent indir() correctly treats
     * it as a pointer value to dereference, not a stack-slot reference.
     *
     * Only do this for VAR vregs (local variables with stack slots).
     * TEMP vregs already hold the computed value in a register and don't
     * need reloading — emitting a LOAD for them would incorrectly treat
     * the integer value as a memory address (crashes on global pre-dec). */
    SValue *sv = vtop;
    if (sv->vr >= 0 && (sv->r & VT_VALMASK) == 0 && TCCIR_DECODE_VREG_TYPE(sv->vr) == TCCIR_VREG_TYPE_VAR)
    {
      SValue src;
      memset(&src, 0, sizeof(src));
      src.type = sv->type;
      src.r = VT_LOCAL | VT_LVAL;
      src.vr = sv->vr;
      src.c.i = sv->c.i;

      SValue load_dest;
      memset(&load_dest, 0, sizeof(load_dest));
      load_dest.type = sv->type;
      load_dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &src, NULL, &load_dest);

      sv->vr = load_dest.vr;
      sv->r = 0;
    }
  }
}

ST_FUNC CString *parse_mult_str(const char *msg)
{
  /* read the string */
  if (tok != TOK_STR)
    expect(msg);
  cstr_reset(&initstr);
  while (tok == TOK_STR)
  {
    /* XXX: add \0 handling too ? */
    cstr_cat(&initstr, tokc.str.data, -1);
    next();
  }
  cstr_ccat(&initstr, '\0');
  return &initstr;
}

/* If I is >= 1 and a power of two, returns log2(i)+1.
   If I is 0 returns 0.  The parameter is unsigned so that a value whose
   highest set bit is the sign bit (e.g. 0x80000000) is shifted and
   compared without the signed loop condition terminating early. */
ST_FUNC int exact_log2p1(unsigned int i)
{
  int ret;
  if (!i)
    return 0;
  for (ret = 1; i >= 1 << 8; ret += 8)
    i >>= 8;
  if (i >= 1 << 4)
    ret += 4, i >>= 4;
  if (i >= 1 << 2)
    ret += 2, i >>= 2;
  if (i >= 1 << 1)
    ret++;
  return ret;
}

/* Parse C23 [[ ... ]] standard attribute syntax.
   Currently we skip/ignore these attributes since TCC does not
   perform interprocedural optimizations. Known attributes like
   [[noreturn]] are mapped to their equivalent effect. */
/* Parse C23 [[ ... ]] standard attributes.  Returns 1 if at least one
   attribute was consumed, 0 if the current '[' is not part of a C23
   attribute (token stream is left unchanged in that case). */
int parse_c23_attribute(AttributeDef *ad)
{
  int found = 0;
  while (tok == '[')
  {
    next();
    if (tok != '[')
    {
      /* Not a C23 attribute — put '[' back */
      unget_tok('[');
      break;
    }
    /* skip the second '[' */
    next();
    found = 1;
    /* parse the attribute contents: handle balanced brackets */
    int brackets = 2;
    while (brackets > 0 && tok != TOK_EOF)
    {
      if (tok == '[')
        brackets++;
      else if (tok == ']')
        brackets--;
      next();
    }
  }
  return found;
}

/* Parse __attribute__((...)) GNUC extension. */
void parse_attribute(AttributeDef *ad)
{
  int t, n;
  char *astr;

redo:
  if (tok != TOK_ATTRIBUTE1 && tok != TOK_ATTRIBUTE2)
    return;
  next();
  skip('(');
  skip('(');
  while (tok != ')')
  {
    if (tok < TOK_IDENT)
      expect("attribute name");
    t = tok;
    next();
    switch (t)
    {
    case TOK_CLEANUP1:
    case TOK_CLEANUP2:
    {
      Sym *s;

      skip('(');
      s = sym_find(tok);
      if (!s)
      {
        tcc_warning_c(warn_implicit_function_declaration)("implicit declaration of function '%s'",
                                                          get_tok_str(tok, &tokc));
        s = external_global_sym(tok, &func_old_type);
      }
      else if ((s->type.t & VT_BTYPE) != VT_FUNC)
        tcc_error("'%s' is not declared as function", get_tok_str(tok, &tokc));
      ad->cleanup_func = s;
      next();
      skip(')');
      break;
    }
    case TOK_CONSTRUCTOR1:
    case TOK_CONSTRUCTOR2:
      ad->f.func_ctor = 1;
      break;
    case TOK_DESTRUCTOR1:
    case TOK_DESTRUCTOR2:
      ad->f.func_dtor = 1;
      break;
    case TOK_ALWAYS_INLINE1:
    case TOK_ALWAYS_INLINE2:
      ad->f.func_alwinl = 1;
      break;
    case TOK_NOINLINE1:
    case TOK_NOINLINE2:
    case TOK_NOIPA1:
    case TOK_NOIPA2:
      ad->f.func_noinline = 1;
      break;
    case TOK_SECTION1:
    case TOK_SECTION2:
      skip('(');
      astr = parse_mult_str("section name")->data;
      ad->section = find_section(tcc_state, astr);
      skip(')');
      break;
    case TOK_ALIAS1:
    case TOK_ALIAS2:
      skip('(');
      astr = parse_mult_str("alias(\"target\")")->data;
      /* save string as token, for later */
      ad->alias_target = tok_alloc_const(astr);
      skip(')');
      break;
    case TOK_VISIBILITY1:
    case TOK_VISIBILITY2:
      skip('(');
      astr = parse_mult_str("visibility(\"default|hidden|internal|protected\")")->data;
      if (!strcmp(astr, "default"))
        ad->a.visibility = STV_DEFAULT;
      else if (!strcmp(astr, "hidden"))
        ad->a.visibility = STV_HIDDEN;
      else if (!strcmp(astr, "internal"))
        ad->a.visibility = STV_INTERNAL;
      else if (!strcmp(astr, "protected"))
        ad->a.visibility = STV_PROTECTED;
      else
        expect("visibility(\"default|hidden|internal|protected\")");
      skip(')');
      break;
    case TOK_ALIGNED1:
    case TOK_ALIGNED2:
      if (tok == '(')
      {
        next();
        n = expr_const();
        if (n <= 0 || (n & (n - 1)) != 0)
          tcc_error("alignment must be a positive power of two");
        skip(')');
      }
      else
      {
        n = MAX_ALIGN;
      }
      ad->a.aligned = exact_log2p1(n);
      if (n != 1 << (ad->a.aligned - 1))
        tcc_error("alignment of %d is larger than implemented", n);
      break;
    case TOK_PACKED1:
    case TOK_PACKED2:
      ad->a.packed = 1;
      break;
    case TOK_WEAK1:
    case TOK_WEAK2:
      ad->a.weak = 1;
      break;
    case TOK_NAKED1:
      ad->a.naked = 1;
      break;
    case TOK_NODEBUG1:
    case TOK_NODEBUG2:
      ad->a.nodebug = 1;
      break;
    case TOK_UNUSED1:
    case TOK_UNUSED2:
      /* currently, no need to handle it because tcc does not
         track unused objects */
      break;
    case TOK_NORETURN1:
    case TOK_NORETURN2:
      ad->f.func_noreturn = 1;
      break;
    case TOK_NOINSTRUMENT1:
    case TOK_NOINSTRUMENT2:
      ad->f.func_no_instrument = 1;
      break;
    case TOK_PURE1:
    case TOK_PURE2:
      ad->f.func_pure = 1;
      break;
    case TOK_CONST2:
    case TOK_CONST3:
      ad->f.func_const = 1;
      break;
    case TOK_CDECL1:
    case TOK_CDECL2:
    case TOK_CDECL3:
      ad->f.func_call = FUNC_CDECL;
      break;
    case TOK_STDCALL1:
    case TOK_STDCALL2:
    case TOK_STDCALL3:
      ad->f.func_call = FUNC_STDCALL;
      break;
#ifdef TCC_TARGET_I386
    case TOK_REGPARM1:
    case TOK_REGPARM2:
      skip('(');
      n = expr_const();
      if (n > 3)
        n = 3;
      else if (n < 0)
        n = 0;
      if (n > 0)
        ad->f.func_call = FUNC_FASTCALL1 + n - 1;
      skip(')');
      break;
    case TOK_FASTCALL1:
    case TOK_FASTCALL2:
    case TOK_FASTCALL3:
      ad->f.func_call = FUNC_FASTCALLW;
      break;
    case TOK_THISCALL1:
    case TOK_THISCALL2:
    case TOK_THISCALL3:
      ad->f.func_call = FUNC_THISCALL;
      break;
#endif
    case TOK_VECTOR_SIZE1:
    case TOK_VECTOR_SIZE2:
      skip('(');
      n = expr_const();
      if (n < 1 || (n & (n - 1)) != 0)
        tcc_error("vector_size must be a positive power of 2");
      ad->vector_size = n;
      skip(')');
      break;
    case TOK_MODE1:
    case TOK_MODE2:
      skip('(');
      switch (tok)
      {
      case TOK_MODE_DI1:
      case TOK_MODE_DI2:
        ad->attr_mode = VT_LLONG + 1;
        break;
      case TOK_MODE_QI1:
      case TOK_MODE_QI2:
        ad->attr_mode = VT_BYTE + 1;
        break;
      case TOK_MODE_HI1:
      case TOK_MODE_HI2:
        ad->attr_mode = VT_SHORT + 1;
        break;
      case TOK_MODE_SI1:
      case TOK_MODE_SI2:
      case TOK_MODE_word1:
      case TOK_MODE_word2:
        ad->attr_mode = VT_INT + 1;
        break;
      default:
        tcc_warning("__mode__(%s) not supported\n", get_tok_str(tok, NULL));
        break;
      }
      next();
      skip(')');
      break;
    case TOK_DLLEXPORT:
      ad->a.dllexport = 1;
      break;
    case TOK_NODECORATE:
      ad->a.nodecorate = 1;
      break;
    case TOK_DLLIMPORT:
      ad->a.dllimport = 1;
      break;
    case TOK_SCALAR_STORAGE_ORDER1:
    case TOK_SCALAR_STORAGE_ORDER2:
      skip('(');
      astr = parse_mult_str("scalar_storage_order(\"big-endian|little-endian\")")->data;
      if (!strcmp(astr, "big-endian"))
        ad->a.sso_be = 1;
      else if (!strcmp(astr, "little-endian"))
        ad->a.sso_be = 0;
      else
        tcc_error("scalar_storage_order must be one of \"big-endian\" or \"little-endian\"");
      skip(')');
      break;
    default:
    {
      const char *attr = get_tok_str(t, NULL);
      if (attr && (!strcmp(attr, "transparent_union") || !strcmp(attr, "__transparent_union__")))
      {
        ad->a.transparent_union = 1;
        break;
      }
    }
      tcc_warning_c(warn_unsupported)("'%s' attribute ignored", get_tok_str(t, NULL));
      /* skip parameters */
      if (tok == '(')
      {
        int parenthesis = 0;
        do
        {
          if (tok == '(')
            parenthesis++;
          else if (tok == ')')
            parenthesis--;
          next();
        } while (parenthesis && tok != -1);
      }
      break;
    }
    if (tok != ',')
      break;
    next();
  }
  skip(')');
  skip(')');
  goto redo;
}

void parse_decl_attributes(AttributeDef *ad)
{
  while (1)
  {
    if (tok == TOK_ATTRIBUTE1 || tok == TOK_ATTRIBUTE2)
    {
      parse_attribute(ad);
      continue;
    }
    if (tok == '[' && parse_c23_attribute(ad))
      continue;
    break;
  }
}
