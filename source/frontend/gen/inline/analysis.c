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

/* analysis.c -- Auto-inline safety analysis of candidate function bodies.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"
#include "memory/unique_ptr.h"
#include "memory/vector.h"

/* Returns 1 if `type` is a struct/union with at least one pointer member
 * (searched recursively through nested aggregates).  Such structs, when passed
 * by value to a non-static function, are the aliasing hazard that makes
 * inlining unsafe (a pointer member may alias another parameter).  Pure scalar/
 * bitfield structs (the common bitfield-struct idiom) carry no such hazard. */
static int struct_has_pointer_member(const CType *type)
{
  Sym *f;
  if ((type->t & VT_BTYPE) != VT_STRUCT || !type->ref)
    return 0;
  for (f = type->ref->next; f; f = f->next)
  {
    int bt = f->type.t & VT_BTYPE;
    if (bt == VT_PTR)
      return 1;
    if (bt == VT_STRUCT && struct_has_pointer_member(&f->type))
      return 1;
  }
  return 0;
}

/* Returns 1 if the type is safe for auto-inline parameter passing (TCCIR_OP_STORE)
 * and return value storage.  Single-register scalars and VT_LLONG are accepted
 * (the IR STORE handles 64-bit integer values natively).  VT_DOUBLE / VT_LDOUBLE
 * and VT_STRUCT parameters still need a multi-register or memory ABI that
 * the inline expansion doesn't handle. */
static int auto_inline_type_ok(int type_t)
{
  switch (type_t & VT_BTYPE)
  {
  case VT_VOID:
  case VT_BYTE:
  case VT_SHORT:
  case VT_INT:
  case VT_LLONG:
  case VT_PTR:
  case VT_FLOAT:
  case VT_BOOL:
  case VT_STRUCT:
    return 1;
  default:
    return 0;
  }
}

/* Returns 1 if all types in the function signature (return + params) are safe
 * for auto-inlining. */
int auto_inline_sig_ok(Sym *func_sym)
{
  Sym *ref = func_sym->type.ref;
  Sym *p;
  if (!ref)
    return 0;
  /* A weak definition is a default the link may replace: the body here is not
   * necessarily the one that runs.  yaslibc's NOOS `sbrk` calls the weak
   * ENOSYS `_sbrk` beside it, and inlining that made every allocation fail in
   * a kernel that defines the real `_sbrk`. */
  if (func_sym->a.weak)
    return 0;
  /* Allow struct return types: the inline expansion handles them via vstore()
   * which generates a memcpy to the return slot.  Struct *parameters* are
   * still rejected (they need ABI-specific passing that STORE can't handle). */
  int ret_btype = ref->type.t & VT_BTYPE;
  if (ref->type.t & VT_COMPLEX)
    return 0;
  if (!auto_inline_type_ok(ref->type.t) && ret_btype != VT_STRUCT)
  {
    LOG_INLINE_STRUCT("[auto-inline-sig] REJECT ret_btype=%d for %s", ret_btype,
                      get_tok_str(func_sym->v & ~SYM_FIELD, NULL));
    return 0;
  }
  int has_llong_param = 0;
  for (p = ref->next; p; p = p->next)
  {
    /* (void) parameter list: single VT_VOID param with no next */
    if ((p->type.t & VT_BTYPE) == VT_VOID && !p->next)
      break;
    if (!auto_inline_type_ok(p->type.t))
      return 0;
    if (p->type.t & VT_COMPLEX)
      return 0;
    /* Struct parameters: a non-static function takes only small plain
     * structs (<=16 bytes, pure scalar/bitfield data -- a pointer member
     * aliasing another param is mishandled after inlining).  A static one
     * takes any size: the expansion binds the argument by the same copy the
     * call would make, and the copy is what inlining removes -- Zig passes
     * whole structs by value to accessors that read one field of them
     * (Sema.Block.src reads src_base_inst of a 100+ byte Block; the slice
     * accessors take a 20-byte MultiArrayList.Slice).  zig.c -O2 .text -47 KB;
     * 64 bytes took -38 KB of it.  Vector types are always rejected. */
    if ((p->type.t & VT_BTYPE) == VT_STRUCT)
    {
      if (p->type.t & VT_VECTOR)
        return 0;
      int sz, al;
      sz = type_size(&p->type, &al);
      static int small_only = -1; /* TCC_INLINE_STRUCT_SMALL: the old 16-byte limit for static ones too */
      if (small_only < 0)
        small_only = getenv("TCC_INLINE_STRUCT_SMALL") != NULL;
      if (sz > 16 && (small_only || !(func_sym->type.t & VT_STATIC)))
        return 0;
      if (!(func_sym->type.t & VT_STATIC) && struct_has_pointer_member(&p->type))
        return 0;
    }
    /* Unnamed parameters (v == 0) crash sym_push during inline expansion
     * because table_ident[0 - TOK_IDENT] is out of bounds. */
    if (p->v == 0)
      return 0;
    if ((p->type.t & VT_BTYPE) == VT_LLONG)
      has_llong_param = 1;
  }
  if (ret_btype == VT_VOID && has_llong_param)
    return 2;
  LOG_INLINE_STRUCT("[auto-inline-sig] ACCEPT %s (ret_btype=%d)", get_tok_str(func_sym->v & ~SYM_FIELD, NULL),
                    ret_btype);
  return 1;
}

/* Guard for the relaxed non-static struct-by-value inline path (see
 * auto_inline_sig_ok): inlining a non-static function that takes a struct by
 * value is only a win for trivial bodies — the by-value marshalling a normal
 * call performs is what the inline avoids, but a larger callee body re-expanded
 * at every site bloats past the call it replaced (e.g. gcc.c-torture structs.c).
 * Tiny identity/forwarding helpers (the `retme`-style bitfield idiom, 8 tokens)
 * are the safe, profitable case.  Static functions and non-struct-param
 * functions keep their existing (size-unrestricted) eligibility. */
#define AUTO_INLINE_NONSTATIC_STRUCT_MAX_TOKENS 12
int auto_inline_nonstatic_struct_body_ok(Sym *func_sym, TokenString *func_str)
{
  Sym *p;
  int has_struct_param = 0;
  if (func_sym->type.t & VT_STATIC)
    return 1;
  if (!func_sym->type.ref)
    return 1;
  for (p = func_sym->type.ref->next; p; p = p->next)
    if ((p->type.t & VT_BTYPE) == VT_STRUCT)
    {
      has_struct_param = 1;
      break;
    }
  if (!has_struct_param)
    return 1;
  return func_str && func_str->len <= AUTO_INLINE_NONSTATIC_STRUCT_MAX_TOKENS;
}

/* Count the number of non-void parameters in a function's type.
 * Returns -1 if any parameter has a VLA type (side effects in parameter
 * declarations that the inline expansion cannot replay). */
int auto_inline_param_count(Sym *func_sym)
{
  Sym *ref = func_sym->type.ref;
  Sym *p;
  int count = 0;
  if (!ref)
    return 0;
  for (p = ref->next; p; p = p->next)
  {
    if ((p->type.t & VT_BTYPE) == VT_VOID && !p->next)
      break;
    if (p->type.t & VT_VLA)
      return -1;
    count++;
  }
  return count;
}

int inline_body_has_return_stmt(TokenString *func_str)
{
  const int *tp;

  if (!func_str)
    return 0;

  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;

    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_RETURN)
      return 1;
    if (tv == TOK_EOF || tv == 0)
      break;
  }

  return 0;
}

/* Return 1 if the function body uses __builtin_apply_args().
 * That builtin captures the *calling* function's argument register block.
 * Inlining such a function changes whose frame is captured, producing wrong
 * results (pr47237). */
int inline_body_has_apply_args(TokenString *func_str)
{
  const int *tp;

  if (!func_str)
    return 0;

  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;

    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_builtin_apply_args || tv == TOK_builtin_longjmp || tv == TOK_builtin_setjmp)
      return 1;
    if (tv == TOK_EOF || tv == 0)
      break;
  }

  return 0;
}

/* Return 1 if the function body contains tokens that would produce observable
 * side effects when the body is speculatively evaluated with nocode_wanted.
 * We conservatively reject:
 *   - pre/post increment/decrement (they modify an lvalue)
 *   - compound assignment operators (+=, -=, *= …)
 *   - plain '=' (an assignment statement would be skipped silently — unsafe)
 * '=' also appears in initializers of local declarations, which we do need
 * to support. We walk the body and ignore '=' until the first ';' boundary
 * of a statement that looks like a declaration: this heuristic preserves
 * const-initialized locals while rejecting assignment statements.
 * Function calls are handled indirectly by rejecting any unknown identifier
 * sequence that looks like a call; if a body calls a function, we also drop
 * its side effects, so we conservatively reject token pair <ident> '('. */
int inline_body_has_side_effects(TokenString *func_str)
{
  const int *tp;
  int depth = 0;
  int brace_depth = 0; /* nesting of { } only — side-effect checks apply only at the body's top level */
  int at_stmt_start = 1;
  int in_decl = 0; /* set when current stmt starts with a type-like token */
  int in_for_header = 0; /* inside for(...) header — skip mutation checks (local vars) */
  int for_paren_depth = 0; /* depth at which the for-header '(' was seen */
  int prev_tv = 0;
  int dbg = TCC_LOG_INLINE_STRUCT;

  if (!func_str)
    return 0;

  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;
    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;
    /* TOK_LINENUM is a debug-info pseudo-token inserted between real tokens;
     * it must be invisible to the statement/declaration tracker. */
    if (tv == TOK_LINENUM)
      continue;

    if (tv == '{')
    {
      depth++;
      brace_depth++;
    }
    else if (tv == '(' || tv == '[')
    {
      depth++;
      if (prev_tv == TOK_FOR && tv == '(' && brace_depth == 1)
      {
        in_for_header = 1;
        for_paren_depth = depth;
      }
    }
    else if (tv == '}')
    {
      if (depth > 0)
        depth--;
      if (brace_depth > 0)
        brace_depth--;
      /* Closing brace of a nested statement-block (if/while/for/etc.) at
       * the function-body level means the NEXT token starts a fresh
       * top-level statement.  Without this the declaration-init heuristic
       * misclassifies `} unsigned int x = ...;` — it falls through to the
       * default `at_stmt_start=0` branch below, so the `=` fires the plain-
       * assignment check and rejects the whole body as having side effects. */
      if (brace_depth == 1 && depth == 1)
      {
        at_stmt_start = 1;
        in_decl = 0;
        prev_tv = tv;
        continue;
      }
    }
    else if (tv == ')' || tv == ']')
    {
      if (in_for_header && tv == ')' && depth == for_paren_depth)
        in_for_header = 0;
      if (depth > 0)
        depth--;
    }

    /* Side-effect checks apply only at the function body's top level
     * (brace_depth == 1). Nested subblocks — e.g. the bodies of `if`
     * statements — are left to the parser in try_inline_const_eval: it
     * either skips them (when the `if` condition is compile-time false)
     * or bails out, so side effects there cannot silently execute. */
    if (brace_depth == 1 && !in_for_header)
    {
      if (tv == TOK_INC || tv == TOK_DEC)
      {
        if (dbg)
          LOG_INLINE_STRUCT("[side_eff] fire INC/DEC tv=%d", tv);
        return 1;
      }
      if (TOK_ASSIGN(tv))
      {
        if (dbg)
          LOG_INLINE_STRUCT("[side_eff] fire TOK_ASSIGN tv=%d", tv);
        return 1;
      }
    }

    /* A '{' inside the body is either the body opener (transition to depth 1)
     * or a compound statement (e.g. block inside an if). Either way, the
     * NEXT token starts a new statement. */
    if (tv == '{')
    {
      at_stmt_start = 1;
      in_decl = 0;
      prev_tv = tv;
      continue;
    }

    /* Function call pattern: user identifier immediately followed by '('.
     * Keywords (TOK_IDENT..TOK_UIDENT-1) aren't callables — things like
     * `return(expr)` or `sizeof(x)` are structural, not function calls.
     * Known pure compile-time builtins (e.g. __builtin_constant_p, which
     * never evaluates its argument) are whitelisted: their "call" shape
     * has no runtime effect and their inner argument side effects, if any,
     * are flagged separately by the mutation checks above. */
    if (brace_depth == 1 && tv == '(' && prev_tv >= TOK_UIDENT)
    {
      switch (prev_tv)
      {
      case TOK_builtin_constant_p:
      case TOK_builtin_types_compatible_p:
      case TOK_builtin_choose_expr:
      case TOK_builtin_expect:
        break;
      default:
        if (dbg)
          LOG_INLINE_STRUCT("[side_eff] fire CALL prev_tv=%d(%s)", prev_tv, get_tok_str(prev_tv, NULL));
        return 1;
      }
    }

    /* Plain '=' handling at body top level:
     *   - inside a declaration statement at depth 1 (declarator init): OK
     *   - anywhere else: unsafe (nested assignment inside an init expr, or
     *     a plain assignment statement) */
    if (brace_depth == 1 && !in_for_header && tv == '=')
    {
      if (!(in_decl && depth == 1))
      {
        if (dbg)
          LOG_INLINE_STRUCT("[side_eff] fire = in_decl=%d depth=%d prev_tv=%d(%s)", in_decl, depth, prev_tv,
                            prev_tv >= TOK_IDENT ? get_tok_str(prev_tv, NULL) : "<op>");
        return 1;
      }
    }

    if (dbg && brace_depth >= 1)
      LOG_INLINE_STRUCT("[side_eff] tok tv=%d(%s) bd=%d d=%d at_stmt=%d in_decl=%d", tv,
                        tv >= TOK_IDENT ? get_tok_str(tv, NULL) : "<op>", brace_depth, depth, at_stmt_start, in_decl);
    if (at_stmt_start && depth == 1)
    {
      /* Heuristic: a statement starting with a type keyword is a declaration. */
      switch (tv)
      {
      case TOK_VOID:
      case TOK_CHAR:
      case TOK_SHORT:
      case TOK_INT:
      case TOK_LONG:
      case TOK_SIGNED1:
      case TOK_SIGNED2:
      case TOK_SIGNED3:
      case TOK_UNSIGNED:
      case TOK_FLOAT:
      case TOK_DOUBLE:
      case TOK_BOOL:
      case TOK_CONST1:
      case TOK_CONST2:
      case TOK_CONST3:
      case TOK_VOLATILE1:
      case TOK_VOLATILE2:
      case TOK_VOLATILE3:
      case TOK_STATIC:
      case TOK_EXTERN:
      case TOK_AUTO:
      case TOK_REGISTER:
      case TOK_TYPEDEF:
        in_decl = 1;
        break;
      default:
        in_decl = 0;
        break;
      }
      at_stmt_start = 0;
    }
    if (tv == ';' && depth == 1)
    {
      at_stmt_start = 1;
      in_decl = 0;
    }

    prev_tv = tv;
  }
  return 0;
}

/* Return 1 if the function body references any identifier that is shadowed
 * by a local variable in the caller's scope.  Token-replay inline expansion
 * resolves identifiers in the caller's scope, so a local `int i` in the
 * caller would shadow a global `int i` that the callee intended to read. */
static int inline_body_shadowed_ident(TokenString *func_str, int enum_only)
{
  const int *tp;
  if (!func_str)
    return 0;
  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;
    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;
    /* Check identifiers (not keywords, not constants) */
    if (tv >= TOK_IDENT)
    {
      TokenSym *ts = table_ident[tv - TOK_IDENT];
      if (ts && ts->sym_identifier && !SYM_IS_PARKED(ts->sym_identifier)) /* parked: a global */
      {
        Sym *s = ts->sym_identifier;
        /* If the identifier resolves to a local and there's also a global
         * with the same name, the local shadows the global. */
        /* Anonymous enums record no scope: a chained constant is a local one.
         * A GNU nested function's name is block-scoped (nested_define) but
         * is not a caller local this check is about. */
        if (enum_only ? IS_ENUM_VAL(s->type.t) && s->prev_tok
                      : (sym_scope(s) > 0 || IS_ENUM_VAL(s->type.t)) && s->prev_tok && !s->a.nested_func)
          return 1;
      }
    }
  }
  return 0;
}

int inline_body_has_shadowed_ident(TokenString *func_str)
{
  return inline_body_shadowed_ident(func_str, 0);
}

/* Return 1 if a struct/union/enum tag named by the body is re-declared in a
 * local scope of the caller: `struct S` in the caller would change sizeof,
 * member offsets and casts of the replayed callee. */
static int inline_body_has_shadowed_tag(TokenString *func_str)
{
  const int *tp;
  if (!func_str)
    return 0;
  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;
    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;
    if (tv >= TOK_IDENT)
    {
      Sym *s = sym_tag_head(tv);
      if (s && !SYM_IS_PARKED(s) && sym_scope(s) > 0 && s->prev_tok)
        return 1;
    }
  }
  return 0;
}

/* A name bound here to something the callee, defined at file scope, cannot
 * mean: a block-scope object, typedef or enum constant, a GNU nested
 * function, or a parent local or typedef an out-of-line nested function
 * captures (those sit at scope 0, or on no chain at all). */
static int inline_name_binds_local(int t)
{
  NestedFunc *cnf = tcc_state->current_nested_func;
  TokenSym *ts = table_ident[t - TOK_IDENT];
  Sym *s = ts ? ts->sym_identifier : NULL;
  if (s && !SYM_IS_PARKED(s))
  {
    if (IS_ENUM_VAL(s->type.t))
    {
      if (sym_scope(s) > 0 || s->prev_tok)
        return 1;
    }
    else if ((s->type.t & (VT_BTYPE | VT_TYPEDEF)) == VT_FUNC)
    {
      if (s->a.nested_func || (s->sym_scope > 0 && s->prev_tok))
        return 1;
    }
    else if (s->sym_scope > 0)
      return 1;
  }
  if (cnf)
  {
    for (int i = 0; i < cnf->nb_captured; i++)
      if (cnf->captured_tokens[i] == t)
        return 1;
    for (int i = 0; i < cnf->nb_parent_typedefs; i++)
      if (cnf->parent_typedef_tokens[i] == t)
        return 1;
  }
  return 0;
}

/* The binding of a nested function's name compared across its definition and
 * a call: NULL for one at file scope, else the Sym itself. */
static Sym *inline_def_binding(Sym *s)
{
  if (!s || SYM_IS_PARKED(s))
    return NULL;
  if (IS_ENUM_VAL(s->type.t) || s->sym_scope > 0 ||
      ((s->type.t & (VT_BTYPE | VT_TYPEDEF)) == VT_FUNC && s->a.nested_func))
    return s;
  return NULL;
}

typedef struct InlineNameState
{
  int tok;
  int depth;  /* brace depth of the declaration while open */
  int for_bd; /* declared in a for header at this brace depth: ends with that statement; else -1 */
  uint8_t state;
} InlineNameState;

enum
{
  INL_NAME_UNSEEN,
  INL_NAME_OPEN,   /* the callee's own declaration binds it here */
  INL_NAME_CLOSED, /* its block ended; a later use is free */
  INL_NAME_FREE    /* some use binds outside the callee */
};

static InlineNameState *inline_name_find(InlineNameState *names, int n, int t)
{
  for (int i = 0; i < n; i++)
    if (names[i].tok == t)
      return &names[i];
  return NULL;
}

static int inline_tok_in(const int *v, size_t n, size_t stride, int t)
{
  for (size_t i = 0; i < n; i += stride)
    if (v[i] == t)
      return 1;
  return 0;
}

/* Keywords that begin or continue the specifiers of a declaration. */
static int inline_tok_is_decl_spec(int t)
{
  return (t >= TOK_EXTERN && t <= TOK_THREAD_LOCAL && t != TOK_EXTENSION) || (t >= TOK_VOID && t <= TOK_ENUM) ||
         t == TOK_ATTRIBUTE1 || t == TOK_ATTRIBUTE2 || t == TOK_ALIGNAS || t == TOK_TYPEOF1 || t == TOK_TYPEOF2 ||
         t == TOK_TYPEOF3;
}

static int inline_tok_is_declarator_qual(int t)
{
  return (t >= TOK_CONST1 && t <= TOK_VOLATILE3) || (t >= TOK_RESTRICT1 && t <= TOK_RODATA_REL) || t == TOK__Atomic ||
         t == TOK_ATTRIBUTE1 || t == TOK_ATTRIBUTE2;
}

/* Walks a callee body as its replay will parse it and settles, for each name
 * in NAMES, whether every use binds to a declaration of the callee itself
 * (its parameters, or a declarator before the use in an enclosing block).
 * Anything it cannot read as a declaration counts as a use, so it errs
 * towards FREE.  Returns 1 if the body defines a nested function. */
static int inline_spliced(const int *spliced, int ord)
{
  if (spliced)
    for (int i = 0; i < spliced[0]; i++)
      if (ord >= spliced[1 + 2 * i] && ord < spliced[2 + 2 * i])
        return 1;
  return 0;
}

static int inline_body_scan_names(TokenString *func_str, Sym *callee, const int *spliced, InlineNameState *names,
                                  int nnames)
{
  scoped_vector(int) toks = {0};
  scoped_vector(int) opener_prev = {0}; /* the token before each open '(' / '[' */
  scoped_vector(int) decl_names = {0};
  scoped_vector(int) decl_typedefs = {0}; /* pairs: the callee's own typedef name, its brace depth */
  const int *tp = tok_str_buf(func_str);
  enum { M_NONE, M_SPEC, M_DECLR, M_AFTER } mode = M_NONE;
  int bd = 0, pd = 0, stmt_start = 0, decl_bd = 0, decl_pd = 0, is_td = 0, type_seen = 0, in_init = 0;
  int noname_pd = -1; /* inside __attribute__/typeof/_Alignas parens: names are uses */
  int closed_prev = 0, ident_ord = 0, in_for = 0, defines_nested = 0;
  while (*tp)
  {
    int tv;
    CValue tcv;
    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;
    if (tv != TOK_LINENUM)
      vector_push_back(&toks, tv);
  }
  if (callee && callee->type.ref)
    for (Sym *p = callee->type.ref->next; p; p = p->next)
    {
      InlineNameState *ns = inline_name_find(names, nnames, p->v & ~SYM_FIELD);
      vector_push_back(&decl_names, p->v & ~SYM_FIELD); /* a parameter is no typedef name */
      if (ns)
        ns->state = INL_NAME_OPEN, ns->depth = 0, ns->for_bd = -1;
    }
  for (size_t i = 0; i < toks.size; i++)
  {
    int tv = toks.data[i];
    int prev = i ? toks.data[i - 1] : 0;
    int next = i + 1 < toks.size ? toks.data[i + 1] : 0;
    int at_decl_level = mode != M_NONE && bd == decl_bd && pd == decl_pd;
    int was_stmt_start = stmt_start;
    int ord = tv >= TOK_UIDENT ? ident_ord++ : -1;
    int for_header_start = mode == M_NONE && prev == '(' && i >= 2 && toks.data[i - 2] == TOK_FOR;
    stmt_start = 0;
    if (tv == '{')
    {
      /* `) {` after a call-shaped `name(...)`, or a declarator's body. */
      if ((mode == M_AFTER && at_decl_level && !in_init) ||
          (mode == M_NONE && prev == ')' && closed_prev >= TOK_UIDENT))
      {
        defines_nested = 1;
        mode = M_NONE; /* its body: statements; its parameters counted as uses */
      }
      bd++;
      if (mode == M_NONE && pd == 0)
        stmt_start = 1;
      continue;
    }
    if (tv == '}')
    {
      for (int k = 0; k < nnames; k++)
        if (names[k].state == INL_NAME_OPEN && names[k].depth >= bd)
          names[k].state = INL_NAME_CLOSED;
      while (decl_typedefs.size && decl_typedefs.data[decl_typedefs.size - 1] >= bd)
        decl_typedefs.size -= 2;
      bd--;
      if (mode != M_NONE && bd < decl_bd)
        mode = M_NONE;
      if (mode == M_NONE && pd == 0)
        stmt_start = 1;
      continue;
    }
    if (tv == '(' || tv == '[')
    {
      int spec_paren = prev == TOK_TYPEOF1 || prev == TOK_TYPEOF2 || prev == TOK_TYPEOF3 || prev == TOK_ATTRIBUTE1 ||
                       prev == TOK_ATTRIBUTE2 || prev == TOK_ALIGNAS || prev == TOK__Atomic;
      vector_push_back(&opener_prev, prev);
      if ((mode == M_SPEC || mode == M_DECLR) && spec_paren && noname_pd < 0)
        noname_pd = pd;
      else if (mode == M_SPEC && at_decl_level && tv == '(')
        mode = M_DECLR; /* int (*fp)(...) */
      pd++;
      continue;
    }
    if (tv == ')' || tv == ']')
    {
      if (opener_prev.size)
        closed_prev = opener_prev.data[--opener_prev.size];
      pd--;
      if (pd == noname_pd)
        noname_pd = -1;
      continue;
    }
    if (tv == ';')
    {
      /* a for statement without braces ends at its first ';' at its own level (or later: then this errs FREE) */
      if (pd == 0)
        for (int k = 0; k < nnames; k++)
          if (names[k].state == INL_NAME_OPEN && names[k].for_bd == bd)
            names[k].state = INL_NAME_CLOSED;
      if (mode == M_NONE ? pd == 0 : at_decl_level)
        mode = M_NONE, stmt_start = 1;
      continue;
    }
    if (at_decl_level && mode == M_AFTER && tv == ',')
    {
      mode = M_DECLR, in_init = 0;
      continue;
    }
    if (at_decl_level && mode == M_AFTER && tv == '=')
    {
      in_init = 1;
      continue;
    }
    if (tv < TOK_IDENT)
    {
      if (at_decl_level && mode == M_SPEC && tv == '*')
        mode = M_DECLR;
      else if (at_decl_level && (mode == M_SPEC || mode == M_DECLR) && tv != '*')
        mode = M_NONE; /* not a declaration this walk can read */
      continue;
    }
    if (tv < TOK_UIDENT)
    {
      if (((was_stmt_start && pd == 0) || for_header_start) && inline_tok_is_decl_spec(tv))
      {
        mode = M_SPEC, decl_bd = bd, decl_pd = pd, is_td = 0, type_seen = 0, in_init = 0, in_for = for_header_start;
        at_decl_level = 1;
      }
      if (!at_decl_level)
        continue;
      if (mode == M_SPEC && inline_tok_is_decl_spec(tv))
      {
        if (tv == TOK_TYPEDEF)
          is_td = 1;
        else if (!inline_tok_is_declarator_qual(tv) && tv != TOK_EXTERN && tv != TOK_STATIC && tv != TOK_AUTO &&
                 tv != TOK_REGISTER && !(tv >= TOK_INLINE1 && tv <= TOK_INLINE3) && tv != TOK_THREAD_LOCAL)
          type_seen = 1;
      }
      else if (!(mode == M_DECLR && inline_tok_is_declarator_qual(tv)) && mode != M_AFTER)
        mode = M_NONE;
      continue;
    }
    /* A user identifier: a caller's spliced argument, a member, a label or a
     * tag binds in no ordinary scope of the callee. */
    if (inline_spliced(spliced, ord))
      continue;
    if (prev == '.' || prev == TOK_ARROW || prev == TOK_GOTO || (was_stmt_start && next == ':'))
    {
      if (was_stmt_start)
        stmt_start = 1;
      continue;
    }
    if (mode == M_SPEC && at_decl_level && (prev == TOK_STRUCT || prev == TOK_UNION || prev == TOK_ENUM))
      continue; /* a tag */
    if ((was_stmt_start && pd == 0) || for_header_start)
    {
      int is_type = inline_tok_in(decl_typedefs.data, decl_typedefs.size, 2, tv);
      if (!is_type && !inline_tok_in(decl_names.data, decl_names.size, 1, tv))
      {
        TokenSym *ts = table_ident[tv - TOK_IDENT];
        Sym *s = ts ? ts->sym_identifier : NULL;
        is_type = s && !SYM_IS_PARKED(s) && (s->type.t & VT_TYPEDEF);
      }
      if (is_type)
      {
        mode = M_SPEC, decl_bd = bd, decl_pd = pd, is_td = 0, type_seen = 0, in_init = 0, in_for = for_header_start;
        at_decl_level = 1;
      }
    }
    if (mode == M_SPEC && at_decl_level && !type_seen && noname_pd < 0)
    {
      int is_type = inline_tok_in(decl_typedefs.data, decl_typedefs.size, 2, tv);
      if (!is_type && !inline_tok_in(decl_names.data, decl_names.size, 1, tv))
      {
        TokenSym *ts = table_ident[tv - TOK_IDENT];
        Sym *s = ts ? ts->sym_identifier : NULL;
        is_type = s && !SYM_IS_PARKED(s) && (s->type.t & VT_TYPEDEF);
      }
      if (is_type)
      {
        type_seen = 1;
        InlineNameState *ns = inline_name_find(names, nnames, tv);
        if (ns && ns->state != INL_NAME_OPEN)
          ns->state = INL_NAME_FREE;
        continue;
      }
    }
    if ((mode == M_SPEC || mode == M_DECLR) && bd == decl_bd && pd >= decl_pd && noname_pd < 0)
    {
      /* the declarator's name: in scope from here to the end of its block */
      InlineNameState *ns = inline_name_find(names, nnames, tv);
      vector_push_back(&decl_names, tv);
      if (is_td)
      {
        vector_push_back(&decl_typedefs, tv);
        vector_push_back(&decl_typedefs, bd);
      }
      if (ns && ns->state != INL_NAME_FREE)
        ns->state = INL_NAME_OPEN, ns->depth = in_for ? bd + 1 : bd, ns->for_bd = in_for ? bd : -1;
      mode = M_AFTER;
      continue;
    }
    {
      InlineNameState *ns = inline_name_find(names, nnames, tv);
      if (ns && ns->state != INL_NAME_OPEN)
        ns->state = INL_NAME_FREE;
    }
  }
  return defines_nested;
}

/* 1 if a name of the body that the callee does not declare itself binds here
 * otherwise than the callee means it (DEF_BINDS: as recorded at a nested
 * function's definition; NULL: at file scope, so not to a caller local), or
 * if a file-scope callee's body defines a nested function. */
static int inline_body_free_names_unsafe(TokenString *func_str, Sym *callee, Sym **def_binds, const int *spliced)
{
  scoped_vector(InlineNameState) names = {0};
  const int *tp = tok_str_buf(func_str);
  int k = 0, unsafe, prev = 0, paren_brace = 0;
  while (*tp)
  {
    int tv;
    CValue tcv;
    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;
    if (tv == TOK_LINENUM)
      continue;
    paren_brace |= prev == ')' && tv == '{';
    prev = tv;
    if (tv < TOK_UIDENT || tv >= tok_ident)
      continue;
    int need;
    if (inline_spliced(spliced, k))
      need = 0;
    else if (def_binds)
    {
      TokenSym *ts = table_ident[tv - TOK_IDENT];
      need = inline_def_binding(ts ? ts->sym_identifier : NULL) != def_binds[k];
    }
    else
      need = inline_name_binds_local(tv);
    k++;
    if (need && !inline_name_find(names.data, names.size, tv))
      vector_push_back(&names, ((InlineNameState){tv, 0, -1, INL_NAME_UNSEEN}));
  }
  if (!names.size && !paren_brace)
    return 0; /* no shadowed name, and no `) {` a nested function definition needs */
  /* A file-scope callee's nested function would stay bound in the caller
   * past the replay (and compile there out of line); a nested callee's
   * children are its parent's family already. */
  unsafe = inline_body_scan_names(func_str, callee, spliced, names.data, names.size) &&
           !(callee && callee->a.nested_func);
  for (size_t i = 0; i < names.size && !unsafe; i++)
    unsafe = names.data[i].state == INL_NAME_FREE;
  return unsafe;
}

/* Records, for a nested function's inline copy, what each name of its body
 * binds to at the definition: its replay at a call must bind them alike. */
void inline_record_def_bindings(InlineFunc *fn, NestedFunc *nf)
{
  const int *tp = tok_str_buf(fn->func_str);
  int n = 0;
  while (*tp)
  {
    int tv;
    CValue tcv;
    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;
    if (tv >= TOK_UIDENT && tv < tok_ident)
      n++;
  }
  fn->def_binds = tcc_malloc((n ? n : 1) * sizeof(Sym *));
  tp = tok_str_buf(fn->func_str);
  n = 0;
  while (*tp)
  {
    int tv;
    CValue tcv;
    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;
    if (tv >= TOK_UIDENT && tv < tok_ident)
    {
      TokenSym *ts = table_ident[tv - TOK_IDENT];
      fn->def_binds[n++] = inline_def_binding(ts ? ts->sym_identifier : NULL);
    }
  }
  fn->def_func = tcc_state->cur_func_sym;
  fn->def_nf = tcc_state->current_nested_func;
  fn->def_scope_top = local_stack;
  fn->def_has_capture = nested_has_genuine_capture(nf);
}

/* Is the scope a nested function was defined in still the one being parsed? */
static int inline_def_scope_live(InlineFunc *fn)
{
  if (!fn->def_binds || fn->def_func != tcc_state->cur_func_sym || fn->def_nf != tcc_state->current_nested_func)
    return 0;
  if (!fn->def_scope_top)
    return 1;
  for (Sym *s = local_stack; s; s = s->prev)
    if (s == fn->def_scope_top)
      return 1;
  return 0;
}

/* Token replay parses the callee's body at the call site.  A file-scope
 * callee's free names must bind as at file scope; a nested function's as at
 * its definition.  The check of the callee's own names first: at the top
 * level any shadowing local refuses (an expansion's params and locals would
 * trip it below that, so nested expansions check enum constants only). */
int inline_body_has_unsafe_shadowed_ident(TokenString *func_str, Sym *call_func_sym)
{
  if (!func_str)
    return 0;
  if (inline_body_has_shadowed_tag(func_str))
    return 1;
  if (inline_body_shadowed_ident(func_str, tcc_state->in_inline_expansion != 0))
    return 1;
  InlineFunc *fn = call_func_sym ? inline_fn_lookup(tcc_state, call_func_sym) : NULL;
  const int *spliced = fn && fn->func_str == func_str ? fn->spliced : NULL;
  if (call_func_sym && call_func_sym->a.nested_func)
  {
    if (fn && inline_def_scope_live(fn))
      return inline_body_free_names_unsafe(func_str, call_func_sym, fn->def_binds, NULL);
    if (!fn || !fn->def_binds || fn->def_has_capture)
      return 1;
  }
  return inline_body_free_names_unsafe(func_str, call_func_sym, NULL, spliced);
}

/* Return 1 if the function body contains tokens that are unsafe for
 * token-replay inline expansion:
 * - TOK_STATIC: creates a new copy of each static variable per inline site
 * - __FUNCTION__/__func__: evaluates to the caller's name instead of the
 *   original function name when token-replayed in the caller's context.
 * We decline to auto-inline such functions. */
int inline_body_has_static_local(TokenString *func_str)
{
  const int *tp;

  if (!func_str)
    return 0;

  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;

    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_STATIC || tv == TOK___FUNCTION__ || tv == TOK___FUNC__)
      return 1;
    if (tv == TOK_EOF || tv == 0)
      break;
  }

  return 0;
}

/* Return 1 if the body declares a local array of at least `limit` bytes, as
 * far as its tokens tell: `T name[N` with N a constant.  T is a basic type
 * keyword (its size counts) or a typedef name (counted as 1 byte, so an
 * unknown element size never inflates the estimate); a pointer element is 4.
 * gcc's large-stack-frame rule for the same reason: inlined, the array is in
 * the caller's frame on every path through it, including the one that
 * recurses or calls on down while the callee's path is never taken (tcc's
 * own next() carried parse_string's 1000-byte buffer, main()
 * default_outputfile's 1 KB, into every compile). */
int inline_body_has_large_local_array(TokenString *func_str, int limit)
{
  const int *tp;
  int prev2 = 0, prev = 0, elem = 0;

  if (!func_str)
    return 0;
  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;

    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;
    if (tv == '[' && prev >= TOK_UIDENT)
    {
      /* prev is the declarator name: what came before it is the type. */
      elem = 0;
      switch (prev2)
      {
      case TOK_CHAR:
        elem = 1;
        break;
      case TOK_SHORT:
        elem = 2;
        break;
      case TOK_INT:
      case TOK_FLOAT:
      case TOK_UNSIGNED:
      case TOK_SIGNED1:
      case TOK_SIGNED2:
      case TOK_SIGNED3:
      case '*':
        elem = 4;
        break;
      case TOK_LONG:
        elem = 4; /* long long counts 4 too: an underestimate */
        break;
      case TOK_DOUBLE:
        elem = 8;
        break;
      default:
        if (prev2 >= TOK_UIDENT)
          elem = 1; /* typedef name */
        break;
      }
      if (elem)
      {
        int nv;
        CValue ncv;
        tok_get(&nv, &tp, &ncv);
        if ((nv == TOK_CINT || nv == TOK_CUINT || nv == TOK_CLONG || nv == TOK_CULONG) &&
            (unsigned long long)ncv.i * elem >= (unsigned)limit)
          return 1;
        if ((nv == TOK_CLLONG || nv == TOK_CULLONG) && ncv.i >= limit)
          return 1;
        prev2 = prev, prev = nv;
        continue;
      }
    }
    if (tv != TOK_LINENUM)
      prev2 = prev, prev = tv;
  }
  return 0;
}

/* Return 1 if the function body contains any loop statement (for/while/do).
 * Token-replay inline expansion does not correctly handle backward jumps in
 * some expression contexts (e.g. for-loop condition), so we decline to
 * always_inline such functions.  Strict: kept conservative for the
 * always_inline call-site check (line ~15200), which can't see the caller's
 * expression-context state. */
int inline_body_has_loops(TokenString *func_str)
{
  const int *tp;

  if (!func_str)
    return 0;

  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;

    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_WHILE || tv == TOK_FOR || tv == TOK_DO)
      return 1;
    if (tv == TOK_EOF || tv == 0)
      break;
  }

  return 0;
}

/* Looser variant for auto-inline candidate registration: accept bodies that
 * contain only `while` / `do` loops at statement level.  The conservative
 * `inline_body_has_loops` rejects every loop because token-replay into an
 * expression-context call site (e.g. a for-loop condition) misbinds the
 * inlined loop's break/continue.  At registration time we don't know the
 * caller's context, but auto-inlined helpers are typically tiny and called
 * from statement-level call sites.  CPOW-style `while(--y > 0)` is the
 * motivating case: once the helper inlines, the IR loop unroller can fold
 * the loop when the trip-count parameter is a compile-time constant.
 *
 * Rejects:
 *  - any `for` loop (the three-part header interacts badly with token replay)
 *  - any loop appearing inside `(`/`[` (expression context — the original
 *    correctness concern, which `({...})` statement-expressions do NOT
 *    trigger because the inner `{` re-enters statement context). */
int inline_body_has_unsafe_loops(TokenString *func_str)
{
  const int *tp;
  /* Stack of open bracket contexts: 0='{' (statement), 1='(', 2='['. */
  unsigned char stack[64];
  int sp = 0;

  if (!func_str)
    return 0;

  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;

    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;
    if (tv == TOK_LINENUM)
      continue;

    switch (tv)
    {
    case '(':
      if (sp < (int)sizeof(stack)) stack[sp] = 1;
      sp++;
      break;
    case '[':
      if (sp < (int)sizeof(stack)) stack[sp] = 2;
      sp++;
      break;
    case '{':
      if (sp < (int)sizeof(stack)) stack[sp] = 0;
      sp++;
      break;
    case ')':
    case ']':
    case '}':
      if (sp > 0) sp--;
      break;
    case TOK_FOR:
    case TOK_WHILE:
    case TOK_DO:
    {
      int innermost = (sp == 0) ? 0
                    : (sp <= (int)sizeof(stack)) ? stack[sp - 1]
                    : 1;
      if (innermost != 0)
        return 1;
    }
    break;
    }
  }

  return 0;
}

/* Check if a nested function has genuine captures (parent variables that are
 * actually accessed through the static chain, not shadowed by parameters).
 * Returns 1 if any capture is genuine, 0 if all are shadowed.  Safe for inlining only when this returns 0. */
int nested_has_genuine_capture(NestedFunc *nf)
{
  if (nf->nb_captured == 0)
    return 0;
  for (int ci = 0; ci < nf->nb_captured; ci++)
  {
    int ctok = nf->captured_tokens[ci];
    int shadowed = 0;
    /* Check if this captured token is a function parameter */
    Sym *ref = nf->sym->type.ref;
    if (ref)
    {
      for (Sym *p = ref->next; p; p = p->next)
      {
        if ((p->v & ~SYM_FIELD) == ctok)
        {
          shadowed = 1;
          break;
        }
      }
    }
    if (!shadowed)
      return 1;
  }
  return 0;
}

/* Look up a callee in the nested function table and return whether it has
 * genuine captures from the parent scope. */
int nested_callee_has_genuine_capture(TCCState *s, Sym *call_func_sym)
{
  for (int ni = 0; ni < s->nb_nested_funcs; ni++) {
    if (s->nested_funcs[ni]->sym == call_func_sym)
      return nested_has_genuine_capture(s->nested_funcs[ni]);
  }
  return 0;
}

/* Returns 1 when inlining `call_func_sym` (a nested function) into the
 * currently-compiling function is safe with respect to capture scope:
 * the callee's lexical parent is either (a) the current function itself
 * (callee is our direct child) or (b) an ancestor of the current function
 * (callee is an enclosing function we still see through our own static
 * chain).  In both cases any reference to a captured variable inside the
 * inlined body still resolves to a slot reachable from the current frame
 * pointer or from R10's existing chain target.
 *
 * Returns 0 for siblings or otherwise-unreachable callees — those would
 * need a chain pointer that the current function does not hold.
 *
 * When `current_nf` is NULL (top-level caller, not inside a nested func),
 * any nested callee's parent is reachable (the inline replay happens in
 * the outer scope, which is the callee's lexical parent or an ancestor). */
int nested_callee_captures_reachable(TCCState *s, Sym *call_func_sym, NestedFunc *current_nf)
{
  NestedFunc *callee_nf = NULL;
  for (int ni = 0; ni < s->nb_nested_funcs; ni++) {
    if (s->nested_funcs[ni]->sym == call_func_sym) {
      callee_nf = s->nested_funcs[ni];
      break;
    }
  }
  if (!callee_nf)
    return 0;
  if (!current_nf)
    return 1;
  /* Walk up from current_nf checking if callee's parent appears on the way.
   * If callee->parent_nf == current_nf  -> direct child (safe).
   * If callee->parent_nf is one of current_nf's ancestors -> safe.
   * Otherwise (sibling, cousin, unrelated) -> not safe. */
  for (NestedFunc *p = current_nf; p; p = p->parent_nf) {
    if (callee_nf->parent_nf == p)
      return 1;
  }
  return 0;
}

/* Check if a nested function with genuine captures only reads them (never
 * writes or takes their address).  When true, token-replay inlining is safe:
 * the inlined body will reference the parent's locals directly, and since it
 * only reads them the "VAR-to-VAR IR pattern" concern does not apply. */
int nested_capture_is_read_only(NestedFunc *nf)
{
  if (nf->nb_captured == 0)
    return 1;
  if (!nf->func_str)
    return 0;

  /* Reject if any parameter has VLA dimensions — VLA expressions can contain
   * side effects on captured variables (e.g. N++) that are not visible in the
   * function body token stream. */
  Sym *fref = nf->sym->type.ref;
  if (fref) {
    for (Sym *p = fref->next; p; p = p->next) {
      if ((p->type.t & VT_VLA) || ((p->type.t & VT_BTYPE) == VT_PTR && p->type.ref &&
                                    (p->type.ref->type.t & VT_VLA)))
        return 0;
    }
  }

  /* Collect the set of genuinely captured tokens (same logic as
   * nested_has_genuine_capture, but we store the tokens). */
  int genuine[MAX_CAPTURED_VARS];
  int ng = 0;
  for (int ci = 0; ci < nf->nb_captured; ci++) {
    int ctok = nf->captured_tokens[ci];
    int shadowed = 0;
    Sym *ref = nf->sym->type.ref;
    if (ref) {
      for (Sym *p = ref->next; p; p = p->next) {
        if ((p->v & ~SYM_FIELD) == ctok) {
          shadowed = 1;
          break;
        }
      }
    }
    if (!shadowed) {
      if (ng >= MAX_CAPTURED_VARS)
        return 0;
      genuine[ng++] = ctok;
    }
  }
  if (ng == 0)
    return 1;

  /* Scan the token stream looking for writes to any genuine capture:
   *   capture = ...       capture += ...  (and other compound assigns)
   *   capture++  capture-- ++capture  --capture
   *   &capture            (address taken — could be used for indirect write) */
  const int *tp = tok_str_buf(nf->func_str);
  int prev = 0;
  while (*tp) {
    int tv;
    CValue tcv;
    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;

    for (int g = 0; g < ng; g++) {
      if (tv != genuine[g])
        continue;
      /* This token is a genuine capture.  Peek at the next token. */
      const int *peek = tp;
      int next_tv = 0;
      if (*peek) {
        CValue dummy;
        tok_get(&next_tv, &peek, &dummy);
      }
      /* Write: capture = expr, capture += expr, ... */
      if (next_tv == '=' || TOK_ASSIGN(next_tv))
        return 0;
      /* Post-increment/decrement: capture++, capture-- */
      if (next_tv == TOK_INC || next_tv == TOK_DEC)
        return 0;
      /* Pre-increment/decrement: ++capture, --capture */
      if (prev == TOK_INC || prev == TOK_DEC)
        return 0;
      /* Address-of: &capture */
      if (prev == '&')
        return 0;
      break;
    }
    prev = tv;
  }
  return 1;
}

/* Does the body write to (or take the address of) the parameter whose
 * identifier token is ident_tok?  An always_inline call with a constant
 * argument must NOT record that parameter in tcc_state->inline_const_args when
 * the body assigns to it: by the time an inline-asm operand names the
 * parameter, its slot holds the new value, not the argument, so substituting
 * the constant would read the original argument instead.  Mirrors the write
 * detection in nested_capture_is_read_only. */
int inline_body_writes_ident(TokenString *func_str, int ident_tok)
{
  if (!func_str || ident_tok < TOK_UIDENT)
    return 0;
  const int *tp = tok_str_buf(func_str);
  int prev = 0;
  while (*tp)
  {
    int tv;
    CValue tcv;
    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      break;
    if (tv == ident_tok)
    {
      const int *peek = tp;
      int next_tv = 0;
      if (*peek)
      {
        CValue dummy;
        tok_get(&next_tv, &peek, &dummy);
      }
      /* capture = expr, capture += expr, ... */
      if (next_tv == '=' || TOK_ASSIGN(next_tv))
        return 1;
      /* capture++ capture-- */
      if (next_tv == TOK_INC || next_tv == TOK_DEC)
        return 1;
      /* ++capture --capture */
      if (prev == TOK_INC || prev == TOK_DEC)
        return 1;
      /* &capture (address taken -- could be written indirectly) */
      if (prev == '&')
        return 1;
    }
    prev = tv;
  }
  return 0;
}

void inline_scan_body_features(TokenString *func_str, int *has_addr_of_label, int *has_inline_asm)
{
  const int *tp;
  int prev_tok_val = 0;
  int prev2_tok_val = 0;

  *has_addr_of_label = 0;
  *has_inline_asm = 0;
  if (!func_str)
    return;

  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;

    tok_get(&tv, &tp, &tcv);
    /* Detect &&label (address-of-label, GNU extension).
     * &&ident is address-of-label only when && appears where a primary
     * expression is expected --- i.e. the token before && is NOT the end
     * of an expression (identifier, number, ')', etc.).
     * When the token before && IS an expression-ender, && is the logical
     * AND binary operator, not address-of-label. */
    if (prev_tok_val == TOK_LAND && tv >= TOK_UIDENT)
    {
      /* prev2 is the token before '&&'.  If it could end an expression
       * (identifier, constant, closing paren/bracket), this is logical AND. */
      int is_logical_and =
          (prev2_tok_val >= TOK_UIDENT || prev2_tok_val == TOK_PPNUM || prev2_tok_val == TOK_CINT ||
           prev2_tok_val == TOK_CUINT || prev2_tok_val == TOK_CCHAR || prev2_tok_val == TOK_LCHAR ||
           prev2_tok_val == TOK_CFLOAT || prev2_tok_val == TOK_CDOUBLE || prev2_tok_val == TOK_CLDOUBLE ||
           prev2_tok_val == TOK_CLLONG || prev2_tok_val == TOK_CULLONG || prev2_tok_val == ')' || prev2_tok_val == ']');
      if (!is_logical_and)
        *has_addr_of_label = 1;
    }
    if (tv == TOK_ASM1 || tv == TOK_ASM2 || tv == TOK_ASM3)
      *has_inline_asm = 1;
    if (*has_addr_of_label && *has_inline_asm)
      break;
    if (tv == TOK_EOF || tv == 0)
      break;
    prev2_tok_val = prev_tok_val;
    prev_tok_val = tv;
  }
}

/* Whether every asm statement in FUNC_STR is one asm_machine_call_name turns
 * into a call -- system instructions, at most one "=r" or "r" operand, no
 * register clobber, no asm goto -- so that an expansion of the body holds no
 * asm statement at all.  Such a body may be auto-inlined: none of the reasons
 * an asm statement keeps a body out of line apply (an "i" operand bound to an
 * argument, a label defined twice), and none of the passes that give up on a
 * function holding asm ever see one.  The kernel's interrupt masking and
 * spin-lock hints are all of this kind.  Operand types are not known here;
 * an operand the parse then rejects (a 64-bit one) only keeps the asm. */
int inline_body_asm_is_machine(TokenString *func_str)
{
#if defined TCC_TARGET_ARM && defined CONFIG_TCC_ASM
  const int *tp = tok_str_buf(func_str);
  int tv;
  CValue tcv;
  for (;;)
  {
    tok_get(&tv, &tp, &tcv);
    if (tv == TOK_EOF || tv == 0)
      return 1;
    if (tv != TOK_ASM1 && tv != TOK_ASM2 && tv != TOK_ASM3)
      continue;
    do
      tok_get(&tv, &tp, &tcv);
    while (tv == TOK_VOLATILE1 || tv == TOK_VOLATILE2 || tv == TOK_VOLATILE3 || tv == TOK_INLINE1 ||
           tv == TOK_INLINE2 || tv == TOK_INLINE3);
    if (tv != '(')
      return 0;
    char tmpl[256];
    int len = 0;
    for (tok_get(&tv, &tp, &tcv); tv == TOK_STR; tok_get(&tv, &tp, &tcv))
    {
      const int n = tcv.str.size - 1;
      if (n < 0 || len + n >= (int)sizeof(tmpl))
        return 0;
      memcpy(tmpl + len, tcv.str.data, n);
      len += n;
    }
    tmpl[len] = 0;
    ASMOperand ops[2];
    uint8_t clobbers[NB_ASM_REGS] = {0};
    int nops = 0, nouts = 0;
    memset(ops, 0, sizeof(ops));
    for (int section = 1; tv == ':'; section++)
    {
      tok_get(&tv, &tp, &tcv);
      if (section > 3)
        return 0; /* asm goto */
      while (tv != ':' && tv != ')')
      {
        if (section == 3)
        {
          if (tv != TOK_STR || (strcmp(tcv.str.data, "memory") && strcmp(tcv.str.data, "cc") &&
                                strcmp(tcv.str.data, "flags")))
            return 0;
          tok_get(&tv, &tp, &tcv);
        }
        else
        {
          if (nops == 2)
            return 0;
          ASMOperand *op = &ops[nops++];
          nouts += section == 1;
          if (tv == '[')
          {
            tok_get(&tv, &tp, &tcv);
            op->id = tv;
            tok_get(&tv, &tp, &tcv);
            if (tv != ']')
              return 0;
            tok_get(&tv, &tp, &tcv);
          }
          if (tv != TOK_STR || tcv.str.size > (int)sizeof(op->constraint))
            return 0;
          memcpy(op->constraint, tcv.str.data, tcv.str.size);
          tok_get(&tv, &tp, &tcv);
          if (tv != '(')
            return 0;
          for (int depth = 1; depth;)
          {
            tok_get(&tv, &tp, &tcv);
            if (tv == TOK_EOF || tv == 0)
              return 0;
            depth += (tv == '(') - (tv == ')');
          }
          tok_get(&tv, &tp, &tcv);
        }
        if (tv == ',')
          tok_get(&tv, &tp, &tcv);
      }
    }
    char name[64];
    if (tv != ')' || !asm_machine_call_name(tmpl, ops, nops, nouts, clobbers, name, sizeof(name)))
      return 0;
  }
#else
  (void)func_str;
  return 0;
#endif
}

/* Live hidden-label frames, innermost last.
 *
 * A compile error inside a replayed inline body longjmps straight out of
 * unary_funcall to tcc_compile's setjmp, so the inline_restore_label_bindings()
 * that owns the two allocations below never runs -- ASan reported them as a
 * 72-byte leak on every -Werror=implicit-function-declaration hit in the toybox
 * build, which buried the real diagnostic.  Recording the frames lets the TU
 * teardown reclaim whatever the unwind skipped.  The array itself is reachable
 * from a static and reused across TUs, so it is not itself a leak. */
typedef struct HiddenLabelFrame
{
  int *tokens;
  Sym **saved_labels;
} HiddenLabelFrame;

static HiddenLabelFrame *hidden_label_frames;
static int nb_hidden_label_frames;
static int hidden_label_frames_capacity;

static void inline_push_hidden_label_frame(int *tokens, Sym **saved_labels)
{
  if (nb_hidden_label_frames >= hidden_label_frames_capacity)
  {
    hidden_label_frames_capacity = hidden_label_frames_capacity ? hidden_label_frames_capacity * 2 : 8;
    hidden_label_frames =
        tcc_realloc(hidden_label_frames, hidden_label_frames_capacity * sizeof(*hidden_label_frames));
  }
  hidden_label_frames[nb_hidden_label_frames].tokens = tokens;
  hidden_label_frames[nb_hidden_label_frames].saved_labels = saved_labels;
  nb_hidden_label_frames++;
}

/* Expansions nest strictly, so the matching frame is normally the top one; scan
 * anyway rather than trust that and drop somebody else's. */
static void inline_pop_hidden_label_frame(int *tokens)
{
  for (int i = nb_hidden_label_frames - 1; i >= 0; --i)
  {
    if (hidden_label_frames[i].tokens != tokens)
      continue;
    for (int j = i + 1; j < nb_hidden_label_frames; ++j)
      hidden_label_frames[j - 1] = hidden_label_frames[j];
    nb_hidden_label_frames--;
    return;
  }
}

/* Called from tccgen_finish() on both the normal and the aborted path.  The
 * saved sym_label pointers are deliberately NOT written back: the TU teardown
 * pops label_stack right after this, which restores those bindings itself, and
 * the values we hold would be stale by then. */
void inline_release_hidden_label_bindings(void)
{
  while (nb_hidden_label_frames > 0)
  {
    HiddenLabelFrame *f = &hidden_label_frames[--nb_hidden_label_frames];
    tcc_free(f->saved_labels);
    tcc_free(f->tokens);
    f->saved_labels = NULL;
    f->tokens = NULL;
  }
}

static int inline_collect_ident_tokens(TokenString *func_str, int **tokens_out)
{
  const int *tp;
  int *tokens = NULL;
  int count = 0;
  int capacity = 0;

  *tokens_out = NULL;
  if (!func_str)
    return 0;

  tp = tok_str_buf(func_str);
  while (*tp)
  {
    int tv;
    CValue tcv;

    tok_get(&tv, &tp, &tcv);
    if (tv >= TOK_UIDENT)
    {
      int i;
      for (i = 0; i < count; ++i)
        if (tokens[i] == tv)
          break;
      if (i == count)
      {
        if (count >= capacity)
        {
          capacity = capacity ? capacity * 2 : 16;
          tokens = tcc_realloc(tokens, capacity * sizeof(*tokens));
        }
        tokens[count++] = tv;
      }
    }
    if (tv == TOK_EOF || tv == 0)
      break;
  }

  *tokens_out = tokens;
  return count;
}

Sym **inline_hide_label_bindings(TokenString *func_str, int **tokens_out, int *count_out)
{
  int *tokens;
  int count;
  Sym **saved_labels;

  *tokens_out = NULL;
  *count_out = 0;

  count = inline_collect_ident_tokens(func_str, &tokens);
  if (count <= 0)
    return NULL;

  saved_labels = tcc_malloc(count * sizeof(*saved_labels));
  for (int i = 0; i < count; ++i)
  {
    saved_labels[i] = sym_label_head(tokens[i]);
    sym_set_label_head(tokens[i], NULL);
  }

  *tokens_out = tokens;
  *count_out = count;
  inline_push_hidden_label_frame(tokens, saved_labels);
  return saved_labels;
}

void inline_restore_label_bindings(int *tokens, Sym **saved_labels, int count)
{
  inline_pop_hidden_label_frame(tokens);
  for (int i = 0; i < count; ++i)
  {
    sym_set_label_head(tokens[i], saved_labels[i]);
  }
  tcc_free(saved_labels);
  tcc_free(tokens);
}

/* Guard heads (partial inlining of a void body's test): see docs/inline_guard_heads.md. */

#define GUARD_HEAD_MAX_TOKENS 512
#define GUARD_HEAD_MAX_STMTS 8
#define GUARD_HEAD_MAX_LOCALS 32
#define GUARD_HEAD_MAX_LABELS 8
#define GUARD_HEAD_MAX_FREE 48

typedef struct GuardTok
{
  int t;
  const int *at;
} GuardTok;

static int guard_type_keyword(int t)
{
  switch (t)
  {
  case TOK_STRUCT:
  case TOK_UNION:
  case TOK_ENUM:
  case TOK_VOID:
  case TOK_CHAR:
  case TOK_INT:
  case TOK_SHORT:
  case TOK_LONG:
  case TOK_SIGNED1:
  case TOK_SIGNED2:
  case TOK_SIGNED3:
  case TOK_UNSIGNED:
  case TOK_BOOL:
  case TOK_FLOAT:
  case TOK_DOUBLE:
  case TOK_CONST1:
  case TOK_CONST2:
  case TOK_CONST3:
  case TOK_RESTRICT1:
  case TOK_RESTRICT2:
  case TOK_RESTRICT3:
    return 1;
  default:
    return 0;
  }
}

static int guard_is_name(int t)
{
  return t >= TOK_UIDENT;
}

static int guard_find(const int *names, int n, int t)
{
  for (int i = 0; i < n; i++)
    if (names[i] == t)
      return 1;
  return 0;
}

/* The names the head binds itself (parameters, its locals declared so far) and
 * the free ones it reads: those are resolved in the caller when it is replayed
 * and must still mean the file-scope ones there (guard_head_captures_local). */
typedef struct GuardNames
{
  const int *params;
  int nparams;
  const int *locals;
  const int *nlocals;
  int free[GUARD_HEAD_MAX_FREE];
  int nfree;
} GuardNames;

/* Name tk[k] as the head reads it; 0 when it cannot be replayed. */
static int guard_note_name(GuardNames *gn, const GuardTok *tk, int k, int b)
{
  int t = tk[k].t;
  int prev = k > b ? tk[k - 1].t : 0;
  /* __func__ names the function it appears in: the caller, once replayed. */
  if (t == TOK___FUNC__ || t == TOK___FUNCTION__)
    return 0;
  /* a member, or a struct/union/enum tag (tags: inline_body_has_shadowed_tag) */
  if (prev == '.' || prev == TOK_ARROW || prev == TOK_STRUCT || prev == TOK_UNION || prev == TOK_ENUM)
    return 1;
  if (guard_find(gn->params, gn->nparams, t) || guard_find(gn->locals, *gn->nlocals, t) ||
      guard_find(gn->free, gn->nfree, t))
    return 1;
  if (gn->nfree >= GUARD_HEAD_MAX_FREE)
    return 0;
  gn->free[gn->nfree++] = t;
  return 1;
}

/* Tokens [b, e) read but write and call nothing. */
static int guard_expr_ok(const GuardTok *tk, int b, int e, GuardNames *gn)
{
  if (b >= e)
    return 0;
  for (int k = b; k < e; k++)
  {
    int t = tk[k].t;
    int prev = k > b ? tk[k - 1].t : 0;
    if (guard_is_name(t))
    {
      if (!guard_note_name(gn, tk, k, b))
        return 0;
      continue;
    }
    if (guard_type_keyword(t) || (t >= TOK_CCHAR && t <= TOK_CULONG))
      continue;
    if (TOK_ISCOND(t))
      continue;
    switch (t)
    {
    case '(':
      /* a call */
      if (guard_is_name(prev) || prev == ')' || prev == ']')
        return 0;
      continue;
    case ')':
    case '[':
    case ']':
    case '.':
    case TOK_ARROW:
    case '*':
    case '&':
    case '+':
    case '-':
    case '~':
    case '!':
    case '/':
    case '%':
    case '<':
    case '>':
    case TOK_SHR:
    case '|':
    case '^':
    case '?':
    case ':':
      continue;
    default:
      return 0;
    }
  }
  return 1;
}

static int guard_match(const GuardTok *tk, int n, int open)
{
  int depth = 0;
  for (int k = open; k < n; k++)
  {
    int t = tk[k].t;
    if (t == '(' || t == '[' || t == '{')
      depth++;
    else if ((t == ')' || t == ']' || t == '}') && --depth == 0)
      return k;
  }
  return -1;
}

/* Declaration [b, e] of plain locals with effect-free initializers; adds them to locals[]. */
static int guard_decl_ok(const GuardTok *tk, int b, int e, int *locals, int *nlocals, const int *params, int nparams,
                         int self, int *inits, GuardNames *gn)
{
  int last = 0, last_k = 0;
  for (int k = b; k <= e; k++)
  {
    int t = tk[k].t;
    if (t == ',' || t == ';' || t == '=')
    {
      if (!last || last == self || guard_find(params, nparams, last) || *nlocals >= GUARD_HEAD_MAX_LOCALS)
        return 0;
      locals[(*nlocals)++] = last;
      last = 0;
      if (t == '=')
      {
        int s = k + 1, end = s;
        while (end < e && tk[end].t != ',')
        {
          if (tk[end].t == '(' || tk[end].t == '[')
          {
            end = guard_match(tk, e, end);
            if (end < 0)
              return 0;
          }
          end++;
        }
        if (!guard_expr_ok(tk, s, end, gn))
          return 0;
        ++*inits;
        k = end;
      }
      continue;
    }
    if (guard_is_name(t))
    {
      /* a name followed by another one is a typedef name (or a tag) */
      if (last && !guard_note_name(gn, tk, last_k, b))
        return 0;
      last = t;
      last_k = k;
    }
    else if (t != '*' && !guard_type_keyword(t))
      return 0;
  }
  return 1;
}

/* Does a statement starting with t declare?  The body's own scope is gone
 * when this runs, so a name is a type only when the head does not bind it
 * itself (a parameter or one of its locals named like a file-scope typedef
 * makes `T * x;` a multiplication) and its binding here is a file-scope
 * typedef. */
static int guard_decl_start(int t, const GuardNames *gn)
{
  if (guard_type_keyword(t))
    return 1;
  if (!guard_is_name(t) || guard_find(gn->params, gn->nparams, t) || guard_find(gn->locals, *gn->nlocals, t))
    return 0;
  Sym *s = sym_find(t);
  return s && (s->type.t & VT_TYPEDEF) && sym_scope(s) == 0;
}

static TokenString *guard_head_scan(TokenString *func_str, Sym *sym, GuardTok *tk, int **free_names)
{
  Sym *ref = sym->type.ref;
  const int self = sym->v & ~SYM_FIELD;
  int params[16], nparams = 0;
  for (Sym *p = ref->next; p; p = p->next)
  {
    int bt = p->type.t & VT_BTYPE;
    if (nparams >= 16 || !p->v || (p->v & ~SYM_FIELD) == self || (p->type.t & (VT_VLA | VT_ARRAY | VT_VOLATILE)) ||
        !(bt == VT_PTR || bt == VT_INT || bt == VT_SHORT || bt == VT_BYTE || bt == VT_BOOL))
      return NULL;
    params[nparams++] = p->v & ~SYM_FIELD;
  }

  int n = 0;
  const int *p = tok_str_buf(func_str), *end = p + func_str->len;
  while (p < end)
  {
    const int *at = p;
    int t;
    CValue cv;
    tok_get(&t, &p, &cv);
    if (t == TOK_EOF || t == 0)
      break;
    if (t == TOK_LINENUM)
      continue;
    if (t == TOK_PACK_REPLAY || t == self || n >= GUARD_HEAD_MAX_TOKENS)
      return NULL;
    tk[n].t = t;
    tk[n].at = at;
    n++;
  }
  if (n < 2 || tk[0].t != '{' || guard_match(tk, n, 0) != n - 1)
    return NULL;

  int locals[GUARD_HEAD_MAX_LOCALS], nlocals = 0, stmts = 0;
  GuardNames gn = {params, nparams, locals, &nlocals};
  gn.nfree = 0;
  int i = 1;
  while (i < n - 1 && tk[i].t != TOK_IF)
  {
    int semi = i;
    while (semi < n - 1 && tk[semi].t != ';')
    {
      if (tk[semi].t == '{' || tk[semi].t == '}')
        return NULL;
      semi++;
    }
    if (semi >= n - 1)
      return NULL;
    if (semi == i)
      ;
    else if (guard_is_name(tk[i].t) && tk[i + 1].t == '=')
    {
      if (!guard_find(locals, nlocals, tk[i].t) || !guard_expr_ok(tk, i + 2, semi, &gn))
        return NULL;
      stmts++;
    }
    else if (guard_decl_start(tk[i].t, &gn))
    {
      if (!guard_decl_ok(tk, i, semi, locals, &nlocals, params, nparams, self, &stmts, &gn))
        return NULL;
    }
    else
      return NULL;
    if (stmts > GUARD_HEAD_MAX_STMTS)
      return NULL;
    i = semi + 1;
  }

  if (i >= n - 1 || tk[i + 1].t != '(')
    return NULL;
  const int cond_close = guard_match(tk, n, i + 1);
  if (cond_close < 0 || !guard_expr_ok(tk, i + 2, cond_close, &gn) || tk[cond_close + 1].t != '{')
    return NULL;
  const int work_close = guard_match(tk, n, cond_close + 1);
  if (work_close < 0 || work_close >= n - 1 || tk[work_close + 1].t == TOK_ELSE)
    return NULL;

  int pending[GUARD_HEAD_MAX_LABELS], npending = 0, seen[GUARD_HEAD_MAX_LABELS], nseen = 0;
  for (int k = work_close + 1; k < n - 1;)
  {
    int t = tk[k].t;
    if (t == ';')
      k++;
    else if (t == TOK_RETURN && tk[k + 1].t == ';')
      k += 2;
    else if (t == TOK_GOTO && k + 2 < n && guard_is_name(tk[k + 1].t) && tk[k + 2].t == ';')
    {
      if (guard_find(seen, nseen, tk[k + 1].t) || npending >= GUARD_HEAD_MAX_LABELS)
        return NULL;
      pending[npending++] = tk[k + 1].t;
      k += 3;
    }
    else if (guard_is_name(t) && tk[k + 1].t == ':')
    {
      if (nseen >= GUARD_HEAD_MAX_LABELS)
        return NULL;
      seen[nseen++] = t;
      for (int j = 0; j < npending; j++)
        if (pending[j] == t)
          pending[j--] = pending[--npending];
      k += 2;
    }
    else
      return NULL;
  }
  if (npending)
    return NULL;

  /* The call it ends with is one more free name: a caller's local of that
   * name must not take it. */
  int *names = tcc_malloc(sizeof(int) * (gn.nfree + 2));
  names[0] = gn.nfree + 1;
  memcpy(names + 1, gn.free, sizeof(int) * gn.nfree);
  names[gn.nfree + 1] = self;
  *free_names = names;

  TokenString *head = tok_str_alloc();
  const int *q = tok_str_buf(func_str);
  const int *stop = tk[cond_close + 1].at;
  while (q < stop)
  {
    int t;
    CValue cv;
    tok_get(&t, &q, &cv);
    tok_str_add2(head, t, &cv);
  }
  tok_str_add(head, self);
  tok_str_add(head, '(');
  for (int k = 0; k < nparams; k++)
  {
    if (k)
      tok_str_add(head, ',');
    tok_str_add(head, params[k]);
  }
  tok_str_add(head, ')');
  tok_str_add(head, ';');
  tok_str_add(head, '}');
  tok_str_add(head, TOK_EOF);
  return head;
}

/* The head, and in *free_names ([0] = count) the names it reads that it
 * does not bind itself. */
TokenString *inline_body_guard_head(TokenString *func_str, Sym *sym, int **free_names)
{
  Sym *ref = sym->type.ref;
  if (!func_str || !ref || (ref->type.t & VT_BTYPE) != VT_VOID || ref->f.func_type == FUNC_ELLIPSIS ||
      func_str->len > GUARD_HEAD_MAX_TOKENS)
    return NULL;
  /* the native compiler runs on a small stack */
  unique_ptr(GuardTok) tk = tcc_malloc(sizeof(GuardTok) * GUARD_HEAD_MAX_TOKENS);
  *free_names = NULL;
  return guard_head_scan(func_str, sym, tk, free_names);
}

/* Replayed here, would a free name of the head bind to something other than
 * what it meant at file scope?  The general check (inline_body_has_unsafe_shadowed_ident)
 * lets the locals of an outer expansion shadow globals, trusting the replayed
 * body to rebind its own names; a guard head's free names are known exactly,
 * so any local binding of one -- the caller's or an outer expansion's, a
 * variable, a typedef or an enum constant -- keeps the plain call. */
int guard_head_captures_local(const int *free_names)
{
  if (!free_names)
    return 1;
  for (int i = 1; i <= free_names[0]; i++)
  {
    int t = free_names[i];
    TokenSym *ts = table_ident[t - TOK_IDENT];
    Sym *s = ts ? ts->sym_identifier : NULL;
    if (s && !SYM_IS_PARKED(s) && (sym_scope(s) > 0 || (IS_ENUM_VAL(s->type.t) && s->prev_tok)))
      return 1;
  }
  return 0;
}
