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
  if (!(tcc_state->sanitize && ubsan_gen_op('+')))
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

  /* the variable's own frame offset, for the reload below: vstore leaves the
     stored value's c.i on vtop, which is the variable's only by accident (a
     sanitized add's result carries another) */
  const SValue dest_lval = vtop[-1];

  fe_conv_quiet++; /* `c++` on a char: no -Wconversion, as gcc */
  vstore(); /* store value */
  fe_conv_quiet--;
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
      src.c.i = dest_lval.vr == sv->vr ? dest_lval.c.i : sv->c.i;

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

/* -Wignored-attributes: attributes that promise a check -- a reader takes
 * `counted_by(n)` for one -- which tcc does not make yet.  Reported once per
 * attribute in a unit, and not in system headers.  fallthrough marks a
 * statement (-Wimplicit-fallthrough), and maybe_unused only silences warnings
 * tcc does not give, so ignoring it loses nothing.  format, nonnull, returns_nonnull, malloc, alloc_size,
 * warn_unused_result, nodiscard, deprecated, access and
 * null_terminated_string_arg (-ffortify) are checked, stack_protect and
 * no_stack_protector applied (-fstack-protector*), in either
 * spelling (parse_checked_attribute). */
static const char *const unchecked_attributes[] = {"counted_by", NULL};

static void warn_unchecked_attribute(const char *name)
{
  char base[32];
  size_t n = strlen(name);
  BufferedFile *f = file;
  /* __format__ is format */
  if (n > 4 && name[0] == '_' && name[1] == '_' && name[n - 1] == '_' && name[n - 2] == '_')
    name += 2, n -= 4;
  if (n >= sizeof base)
    return;
  memcpy(base, name, n);
  base[n] = 0;
  while (f && f->filename[0] == ':' && f->prev)
    f = f->prev;
  for (int i = 0; unchecked_attributes[i]; i++)
    if (!strcmp(base, unchecked_attributes[i]))
    {
      if ((f && f->sys) || (tcc_state->warned_attributes & (1u << i)))
        return;
      tcc_state->warned_attributes |= 1u << i;
      tcc_warning_c(warn_ignored_attributes)("'%s' attribute is not checked by tcc: ignored", base);
      return;
    }
}

static int attribute_carries_check(const char *name)
{
  size_t n = strlen(name);
  if (n > 4 && name[0] == '_' && name[1] == '_' && name[n - 1] == '_' && name[n - 2] == '_')
    name += 2, n -= 4;
  for (int i = 0; unchecked_attributes[i]; i++)
    if (strlen(unchecked_attributes[i]) == n && !strncmp(name, unchecked_attributes[i], n))
      return 1;
  return 0;
}

/* Is attribute token T the name N, as `N` or `__N__`? */
static int attr_named(int t, const char *n)
{
  const char *s = get_tok_str(t, NULL);
  size_t len = strlen(n);
  if (!s)
    return 0;
  if (s[0] == '_' && s[1] == '_')
    return !strncmp(s + 2, n, len) && !strcmp(s + 2 + len, "__");
  return !strcmp(s, n);
}

/* `("text")` after deprecated or nodiscard: its token, 0 when there is none */
static int attr_message(void)
{
  int t;
  if (tok != '(')
    return 0;
  next();
  t = tok_alloc_const(parse_mult_str("string literal")->data);
  skip(')');
  return t;
}

/* The attributes the frontend applies or checks, after their name T, in
 * either spelling: `__attribute__((T...))` and C23 `[[gnu::T...]]`.  0 when
 * T is not one of them (nothing consumed). */
static int parse_checked_attribute(AttributeDef *ad, int t)
{
  int n;
  if (attr_named(t, "malloc"))
  {
    /* malloc, or GCC 11's malloc(deallocator[, argument]) */
    ad->attr_malloc = 1;
    if (tok == '(')
    {
      next();
      if (tok < TOK_UIDENT)
        expect("deallocator");
      ad->attr_dealloc = tok;
      ad->attr_dealloc_arg = 1;
      next();
      if (tok == ',')
      {
        next();
        ad->attr_dealloc_arg = expr_const();
      }
      skip(')');
    }
    return 1;
  }
  if (attr_named(t, "nonnull"))
  {
    if (tok == '(')
    {
      next();
      for (;;)
      {
        n = expr_const();
        if (n >= 1 && n <= 32)
          ad->attr_nonnull |= 1u << (n - 1);
        if (tok != ',')
          break;
        next();
      }
      skip(')');
    }
    else
      ad->attr_nonnull_all = 1;
    return 1;
  }
  if (attr_named(t, "alloc_size"))
  {
    /* alloc_size(a[, b]): the result's size is argument a (times b) --
       __builtin_object_size and the checks of builtin/objsize.c */
    int a, b = 0;
    skip('(');
    a = expr_const();
    if (tok == ',')
    {
      next();
      b = expr_const();
    }
    skip(')');
    if (a >= 1 && a <= 255 && b >= 0 && b <= 255)
      ad->attr_alloc_a = a, ad->attr_alloc_b = b;
    return 1;
  }
  /* -ffortify's (builtin/fortify.c): the calls are checked against what the
     pointer arguments point to */
  if (attr_named(t, "null_terminated_string_arg"))
  {
    skip('(');
    n = expr_const();
    skip(')');
    if (n >= 1 && n <= 32)
      ad->attr_cstr_args |= 1u << (n - 1);
    else
      tcc_warning_c(warn_ignored_attributes)("null_terminated_string_arg: argument %d out of range: ignored", n);
    return 1;
  }
  if (attr_named(t, "access"))
  {
    /* access(read_only | write_only | read_write | none, ref[, size]) */
    static const char *const modes[] = {"none", "read_only", "write_only", "read_write"};
    int mode = -1, ref, size = 0;
    skip('(');
    for (int i = 0; i < 4; i++)
      if (tok >= TOK_UIDENT && attr_named(tok, modes[i]))
        mode = i;
    if (mode < 0)
      expect("read_only, write_only, read_write or none");
    next();
    skip(',');
    ref = expr_const();
    if (tok == ',')
    {
      next();
      size = expr_const();
    }
    skip(')');
    if (ref < 1 || ref > 255 || size < 0 || size > 255)
      tcc_warning_c(warn_ignored_attributes)("access: argument out of range: ignored");
    else if (ad->attr_access_n < FORTIFY_MAX_ACCESS)
    {
      unsigned char *a = ad->attr_access[ad->attr_access_n++];
      a[0] = mode, a[1] = ref, a[2] = size;
    }
    return 1;
  }
  if (attr_named(t, "returns_nonnull"))
  {
    ad->attr_returns_nonnull = 1;
    return 1;
  }
  /* a struct member's pointer from regular C is `view` / `view maybe`
     (attr_apply_member_facts): what sys/_attrs.h's __yl_view says */
  if (attr_named(t, "yasc_view"))
  {
    ad->attr_yasc_ptr |= YASC_VIEW;
    return 1;
  }
  if (attr_named(t, "yasc_maybe"))
  {
    ad->attr_yasc_ptr |= YASC_MAYBE;
    return 1;
  }
  /* yasc_c_api(n, T): the yasc declaration says T where this one says what
     its c_api(...) gave (decl/c_api.c, -Wyasc-c-api) */
  if (attr_named(t, "yasc_c_api"))
  {
    yasc_capi_attr_parse(ad);
    return 1;
  }
  /* -Wtainted-format's sources (ir/taint.c): the result, or what argument n
     points to after the call, is untrusted input */
  if (attr_named(t, "tainted_result"))
  {
    ad->attr_tainted_result = 1;
    return 1;
  }
  if (attr_named(t, "tainted_arg"))
  {
    int n;
    skip('(');
    for (;;)
    {
      n = expr_const();
      if (n >= 1 && n <= 32)
        ad->attr_tainted_args |= 1u << (n - 1);
      if (tok != ',')
        break;
      next();
    }
    skip(')');
    return 1;
  }
  if (attr_named(t, "format"))
  {
    /* format(printf|scanf|..., fmt, first): -Wformat at each call */
    int kind, idx, first;
    skip('(');
    if (tok < TOK_IDENT)
      expect("format archetype");
    kind = attr_named(tok, "printf") || attr_named(tok, "gnu_printf")       ? ATTR_FMT_PRINTF
           : attr_named(tok, "scanf") || attr_named(tok, "gnu_scanf")       ? ATTR_FMT_SCANF
           : attr_named(tok, "strftime") || attr_named(tok, "gnu_strftime") ? ATTR_FMT_STRFTIME
           : attr_named(tok, "strfmon")                                     ? ATTR_FMT_STRFMON
                                                                            : ATTR_FMT_OTHER;
    next();
    skip(',');
    idx = expr_const();
    skip(',');
    first = expr_const();
    skip(')');
    if (idx < 1 || idx > 255 || first < 0 || first > 255 || (first && first <= idx))
      tcc_warning_c(warn_format)("'format' attribute: argument numbers %d, %d are out of range: ignored", idx, first);
    else
    {
      ad->attr_fmt_kind = kind;
      ad->attr_fmt_idx = idx;
      ad->attr_fmt_first = first;
    }
    return 1;
  }
  if (attr_named(t, "format_arg"))
  {
    /* format_arg(n): the result is a format when argument n is (gettext) */
    int n;
    skip('(');
    n = expr_const();
    skip(')');
    if (n >= 1 && n <= 255)
      ad->attr_fmt_arg = n;
    return 1;
  }
  if (attr_named(t, "deprecated"))
  {
    int m = attr_message();
    ad->attr_deprecated = 1;
    if (m)
      ad->attr_deprecated_msg = m;
    return 1;
  }
  /* yasc_unsafe("what to use"): yasc code calls it inside unsafe only
     (-Wyasc-unsafe-call, expr/attr_checks.c) -- strcpy, sprintf: their
     destination has no size */
  if (attr_named(t, "yasc_unsafe"))
  {
    int m = attr_message();
    ad->attr_yasc_unsafe = 1;
    if (m)
      ad->attr_yasc_unsafe_msg = m;
    return 1;
  }
  /* -fstack-protector*'s choice for this function (op/ubsan.c
     ssp_begin_function).  Under -fstack-protector* not inlined, as gcc does
     not inline across a mismatch: an inlined body is protected or not with
     its caller.  Without one the attributes change nothing. */
  if (attr_named(t, "stack_protect") || attr_named(t, "no_stack_protector"))
  {
    ad->attr_ssp |= attr_named(t, "stack_protect") ? SYMF_STACK_PROTECT : SYMF_NO_STACK_PROTECT;
    if (tcc_state->stack_protector)
      ad->f.func_noinline = 1;
    return 1;
  }
  if (attr_named(t, "warn_unused_result"))
  {
    if (!ad->attr_unused_result)
      ad->attr_unused_result = ATTR_WUR;
    return 1;
  }
  return 0;
}

/* Parse C23 [[ ... ]] standard attributes.  Returns 1 if at least one
   attribute was consumed, 0 if the current '[' is not part of a C23
   attribute (token stream is left unchanged in that case).  deprecated and
   nodiscard, and gnu:: the attributes parse_checked_attribute takes, have
   their meaning; the others are skipped (-Wignored-attributes for those that
   promise a check). */
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
    while (tok != ']' && tok != TOK_EOF)
    {
      int prefix = 0, name, done = 0;
      if (tok == ',')
      {
        next();
        continue;
      }
      if (tok < TOK_IDENT)
        expect("attribute name");
      name = tok;
      next();
      if (tok == ':')
      {
        /* prefix::name */
        next();
        skip(':');
        if (tok < TOK_IDENT)
          expect("attribute name");
        prefix = name;
        name = tok;
        next();
      }
      if ((!prefix || attr_named(prefix, "gnu")) && attr_named(name, "fallthrough"))
      {
        attr_fallthrough_seen = 1; /* a statement: -Wimplicit-fallthrough */
        done = 1;
      }
      else if (!prefix && attr_named(name, "nodiscard"))
      {
        int m = attr_message();
        ad->attr_unused_result = ATTR_NODISCARD;
        if (m)
          ad->attr_nodiscard_msg = m;
        done = 1;
      }
      else if (!prefix && attr_named(name, "deprecated"))
        done = parse_checked_attribute(ad, name);
      else if (prefix && attr_named(prefix, "gnu"))
        done = parse_checked_attribute(ad, name);
      /* [[maybe_unused]], [[gnu::unused]]: no -Wunused-* for it */
      if (!done && (prefix ? attr_named(prefix, "gnu") && attr_named(name, "unused") : attr_named(name, "maybe_unused")))
        ad->attr_unused = 1;
      if (!done)
      {
        int parens = 0;
        warn_unchecked_attribute(get_tok_str(name, NULL));
        /* skip its arguments, balanced */
        while (tok == '(' || parens > 0)
        {
          if (tok == TOK_EOF)
            break;
          if (tok == '(' || tok == '[')
            parens++;
          else if (tok == ')' || tok == ']')
            parens--;
          next();
        }
      }
    }
    skip(']');
    skip(']');
  }
  return found;
}

/* __has_attribute(name): 1 when parse_attribute() gives `name` its meaning,
 * 0 when it would ignore it with a warning. Headers use this to choose between
 * an attribute and a fallback (zig.h poisons `naked` and `noinline` outright
 * when told they are missing), so the answer has to match the parser below --
 * keep the two lists in step. Calling conventions and the PE-only
 * dllexport/dllimport/nodecorate are parsed but mean nothing on this target,
 * so they report 0, as gcc does on ARM. */
ST_FUNC int tcc_attribute_supported(int t)
{
  switch (t)
  {
  case TOK_CLEANUP1:
  case TOK_CLEANUP2:
  case TOK_CONSTRUCTOR1:
  case TOK_CONSTRUCTOR2:
  case TOK_DESTRUCTOR1:
  case TOK_DESTRUCTOR2:
  case TOK_ALWAYS_INLINE1:
  case TOK_ALWAYS_INLINE2:
  case TOK_NOINLINE1:
  case TOK_NOINLINE2:
  case TOK_NOIPA1:
  case TOK_NOIPA2:
  case TOK_SECTION1:
  case TOK_SECTION2:
  case TOK_ALIAS1:
  case TOK_ALIAS2:
  case TOK_VISIBILITY1:
  case TOK_VISIBILITY2:
  case TOK_ALIGNED1:
  case TOK_ALIGNED2:
  case TOK_PACKED1:
  case TOK_PACKED2:
  case TOK_WEAK1:
  case TOK_WEAK2:
  case TOK_NAKED1:
  case TOK_NODEBUG1:
  case TOK_NODEBUG2:
  case TOK_USED1:
  case TOK_USED2:
  case TOK_UNUSED1:
  case TOK_UNUSED2:
  case TOK_NORETURN1:
  case TOK_NORETURN2:
  case TOK_NOINSTRUMENT1:
  case TOK_NOINSTRUMENT2:
  case TOK_PURE1:
  case TOK_PURE2:
  case TOK_CONST2:
  case TOK_CONST3:
  case TOK_VECTOR_SIZE1:
  case TOK_VECTOR_SIZE2:
  case TOK_MODE1:
  case TOK_MODE2:
  case TOK_SCALAR_STORAGE_ORDER1:
  case TOK_SCALAR_STORAGE_ORDER2:
    return 1;
  default:
    if (t >= TOK_IDENT)
      return attr_named(t, "transparent_union") || attr_named(t, "malloc") || attr_named(t, "nonnull") ||
             attr_named(t, "returns_nonnull") || attr_named(t, "format") || attr_named(t, "format_arg") ||
             attr_named(t, "deprecated") || attr_named(t, "tainted_result") || attr_named(t, "tainted_arg") ||
             attr_named(t, "warn_unused_result") || attr_named(t, "fallthrough") ||
             attr_named(t, "stack_protect") || attr_named(t, "no_stack_protector") || attr_named(t, "yasc_view") ||
             attr_named(t, "yasc_maybe") || attr_named(t, "yasc_c_api") || attr_named(t, "yasc_unsafe");
    return 0;
  }
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
        tcc_warning_once(warn_implicit_function_declaration, "implicit declaration of function '%s'",
                         get_tok_str(tok, &tokc));
        s = external_global_sym(tok, &func_old_type);
      }
      else if ((s->type.t & VT_BTYPE) != VT_FUNC)
        tcc_error("'%s' is not declared as function", get_tok_str(tok, &tokc));
      ad->cleanup_func = s;
      s->ymod |= SYM_UNUSED_REF; /* -Wunused-function: named */
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
      ad->a.vis_explicit = 1;
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
    case TOK_USED1:
    case TOK_USED2:
      ad->a.used = 1;
      break;
    case TOK_UNUSED1:
    case TOK_UNUSED2:
      ad->attr_unused = 1; /* no -Wunused-* for it */
      break;
    case TOK_NORETURN1:
    case TOK_NORETURN2:
      ad->f.func_noreturn = 1;
      break;
    case TOK_NOINSTRUMENT1:
    case TOK_NOINSTRUMENT2:
      ad->f.func_no_instrument = 1;
      break;
    case TOK_LEAF1:
    case TOK_LEAF2:
      ad->f.func_leaf = 1;
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
#ifdef TCC_TARGET_PE
      ad->a.nodecorate = 1;
#endif
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
      if (attr_named(t, "transparent_union"))
      {
        ad->a.transparent_union = 1;
        break;
      }
      if (parse_checked_attribute(ad, t))
        break;
      if (attr_named(t, "fallthrough"))
      {
        /* `__attribute__((fallthrough));`: block() reads it as a statement
           (-Wimplicit-fallthrough) */
        attr_fallthrough_seen = 1;
        break;
      }
    }
      if (attribute_carries_check(get_tok_str(t, NULL)))
        warn_unchecked_attribute(get_tok_str(t, NULL));
      else
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

/* A pointer node with these yasc qualifiers instead of "unchecked" */
static void attr_requalify(CType *ptr, int add)
{
  ptr->ref = sym_pointer_node_q(&ptr->ref->type, (type_yasc(ptr) & ~YASC_UNCHECKED) | add);
}

/* A struct member V of TYPE was declared with AD: yasc_view makes its pointer
 * `view`, with yasc_maybe `view maybe` -- in a unit with yasc in it, and only
 * a pointer written in regular C, which says nothing itself: a C header that
 * says what its struct holds is believed, as malloc is
 * (attr_apply_pointer_facts).  The outer pointer only: `T **p` is a view of
 * unchecked pointers. */
void attr_apply_member_facts(CType *type, AttributeDef *ad, int v)
{
  const int q = ad->attr_yasc_ptr;
  const char *name = v ? get_tok_str(v, NULL) : "";
  if (!q)
    return;
  if ((type->t & (VT_BTYPE | VT_ARRAY)) != VT_PTR)
  {
    tcc_warning_c(warn_ignored_attributes)("yasc_view on '%s', which is not a pointer: ignored", name);
    return;
  }
  if (!(q & YASC_VIEW))
  {
    tcc_warning_c(warn_ignored_attributes)("yasc_maybe on '%s' without yasc_view: ignored", name);
    return;
  }
  if (tcc_yasc_unit(tcc_state) && (type_yasc(type) & YASC_UNCHECKED))
    attr_requalify(type, q);
}

/* A function declarator TYPE was parsed with AD: what its GCC attributes say
 * about pointers goes into the types.
 * - nonnull marks the parameters, for -Wnonnull at calls;
 * - in a yasc unit, malloc makes the result `owned maybe` (`owned` with
 *   returns_nonnull), and malloc(dealloc, i) makes the deallocator's i-th
 *   parameter `owned maybe drop` -- on a declaration from regular C too, over
 *   "unchecked": a C header that says what it means is believed.
 * Only a function type the declarator built itself: a typedef's is shared. */
void attr_apply_pointer_facts(CType *type, AttributeDef *ad, int own_functype)
{
  Sym *f = type->ref, *p;
  CType *ret;
  int i;

  if ((type->t & VT_BTYPE) != VT_FUNC || !own_functype)
    return;
  if (ad->attr_nonnull || ad->attr_nonnull_all)
    for (i = 1, p = sym_first(f); p; p = p->next, i++)
      if ((p->type.t & VT_BTYPE) == VT_PTR &&
          (ad->attr_nonnull_all || (i <= 32 && (ad->attr_nonnull & (1u << (i - 1))))))
        p->a.nonnull = 1;
  /* a C header's declarations are read as C, but in a unit with yasc in it */
  if (!tcc_yasc_unit(tcc_state) || !ad->attr_malloc)
    return;
  ret = &f->type;
  if ((ret->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR)
  {
    attr_requalify(ret, YASC_OWNED | (ad->attr_returns_nonnull ? 0 : YASC_MAYBE));
    /* malloc(fclose, 1): released by fclose, as `owned(fclose)` says */
    if (ad->attr_dealloc)
      ret->ref = sym_pointer_node_b(&ret->ref->type, ret->ref->yasc, ad->attr_dealloc);
  }
  if (ad->attr_dealloc)
  {
    Sym *d = sym_find(ad->attr_dealloc);
    if (!d || (d->type.t & VT_BTYPE) != VT_FUNC)
    {
      tcc_warning("malloc: deallocator '%s' is not a function declared before", get_tok_str(ad->attr_dealloc, NULL));
      return;
    }
    for (i = 1, p = sym_first(d->type.ref); p && i < ad->attr_dealloc_arg; p = p->next)
      i++;
    if (p && (p->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR)
    {
      attr_requalify(&p->type, YASC_OWNED | YASC_MAYBE | YASC_DROP);
      p->type.ref = sym_pointer_node_b(&p->type.ref->type, p->type.ref->yasc, ad->attr_dealloc);
    }
  }
}

/* The checked attributes a declaration AD wrote (deprecated, nodiscard /
 * warn_unused_result, format) go to its Sym S, merged over the earlier
 * declarations' (SymAttr.attr_facts marks it; expr/attr_checks.c reads them
 * at each use). */
void attr_apply_decl_facts(Sym *s, AttributeDef *ad)
{
  attr_apply_decl_facts_at(s, ad, 0);
}

/* The same, the declaration at LINE of this file (0: the current line) */
void attr_apply_decl_facts_at(Sym *s, AttributeDef *ad, int line)
{
  SymLocalFacts *f;
  if (s)
    yasc_capi_attr_apply(&s->type, ad);
  if (!s || !(ad->attr_deprecated | ad->attr_yasc_unsafe | ad->attr_unused_result | ad->attr_fmt_kind | ad->attr_fmt_arg | ad->attr_alloc_a |
               ad->attr_malloc | ad->attr_tainted_result | ad->attr_tainted_args | ad->attr_ssp | ad->attr_cstr_args |
               ad->attr_access_n))
    return;
  f = sym_facts(s);
  s->a.attr_facts = 1;
  /* stack_protect / no_stack_protector: no_stack_protector wins (gcc) */
  if (ad->attr_ssp && (s->type.t & VT_BTYPE) == VT_FUNC)
  {
    f->flags |= ad->attr_ssp;
    if ((f->flags & (SYMF_STACK_PROTECT | SYMF_NO_STACK_PROTECT)) == (SYMF_STACK_PROTECT | SYMF_NO_STACK_PROTECT))
    {
      f->flags &= ~SYMF_STACK_PROTECT;
      tcc_warning_once(warn_ignored_attributes, "'stack_protect' attribute conflicts with 'no_stack_protector': ignored");
    }
  }
  /* alloc_size, malloc, malloc(dealloc, i): builtin/objsize.c */
  if ((ad->attr_alloc_a || ad->attr_malloc) && (s->type.t & VT_BTYPE) == VT_FUNC)
    objsize_apply_alloc_attrs(s, f, ad);
  if (ad->attr_deprecated)
  {
    if (!f->deprecated)
      attr_decl_place(f, line);
    f->deprecated = 1;
    if (ad->attr_deprecated_msg)
      f->deprecated_msg = ad->attr_deprecated_msg;
  }
  if (ad->attr_yasc_unsafe)
  {
    if (!f->deprecated && !f->yasc_unsafe)
      attr_decl_place(f, line);
    f->yasc_unsafe = 1;
    if (ad->attr_yasc_unsafe_msg)
      f->yasc_unsafe_msg = ad->attr_yasc_unsafe_msg;
  }
  if (ad->attr_unused_result > f->unused_result)
    f->unused_result = ad->attr_unused_result;
  if (ad->attr_nodiscard_msg)
    f->nodiscard_msg = ad->attr_nodiscard_msg;
  if (ad->attr_fmt_arg)
    f->fmt_arg = ad->attr_fmt_arg;
  f->tainted_result |= ad->attr_tainted_result;
  f->tainted_args |= ad->attr_tainted_args;
  if ((ad->attr_cstr_args || ad->attr_access_n) && (s->type.t & VT_BTYPE) == VT_FUNC)
    fortify_apply_attrs(s, f, ad);
  if (ad->attr_fmt_kind)
  {
    f->fmt_kind = ad->attr_fmt_kind;
    f->fmt_idx = ad->attr_fmt_idx;
    f->fmt_first = ad->attr_fmt_first;
  }
}
