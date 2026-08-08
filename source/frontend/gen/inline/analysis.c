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
    /* Only inline small plain structs (≤16 bytes).  Non-static functions with
     * struct params carrying pointer members can have complex aliasing (a
     * pointer member aliasing another param) that the optimizer mishandles
     * after inlining — so for non-static functions require the struct to be
     * pure scalar/bitfield data.  Vector types and large structs are always
     * rejected. */
    if ((p->type.t & VT_BTYPE) == VT_STRUCT)
    {
      if (p->type.t & VT_VECTOR)
        return 0;
      int sz, al;
      sz = type_size(&p->type, &al);
      if (sz > 16)
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
int inline_body_has_shadowed_ident(TokenString *func_str)
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
      if (ts && ts->sym_identifier)
      {
        Sym *s = ts->sym_identifier;
        /* If the identifier resolves to a local and there's also a global
         * with the same name, the local shadows the global. */
        if (sym_scope(s) > 0 && s->prev_tok)
          return 1;
      }
    }
  }
  return 0;
}

/* Smarter variant for nested inlining.  At the top level (not inside another
 * inline expansion), defer to the strict check — it catches the genuine bug
 * of a caller's local shadowing a global that the callee references.
 *
 * When called from inside a nested inline expansion, the outer-inlined
 * function's parameter/local symbols are already in scope and shadow their
 * file-scope counterparts.  The strict check would always trip on those,
 * blocking nested expansion for any non-trivial body — even when the inner
 * callee's identifiers are purely self-bound (its own params/locals, re-
 * resolved during its own replay).  In that case the shadowing is harmless,
 * so we relax the check to allow nested expansion.
 *
 * The residual risk is: callee references a free global identifier whose
 * name happens to match one of the outer-inlined function's locals.  This
 * is rare in practice and the gain (full collapse of helper-chain calls in
 * the c5p/CPOW/CCID style) is substantial.  If a real regression surfaces,
 * the check can be tightened by exempting only the outer expansion's
 * known-pushed symbols. */
int inline_body_has_unsafe_shadowed_ident(TokenString *func_str, Sym *call_func_sym)
{
  (void)call_func_sym;
  if (!func_str)
    return 0;
  /* In nested inline expansion: the outer expansion's locals are in scope
   * and would always shadow globals matching the callee's params/locals.
   * Skip the strict check in that case. */
  if (tcc_state->in_inline_expansion)
    return 0;
  return inline_body_has_shadowed_ident(func_str);
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
 * actually accessed through the static chain, not shadowed by parameters or
 * local declarations).  Returns 1 if any capture is genuine, 0 if all are
 * shadowed.  Safe for inlining only when this returns 0. */
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
    if (!shadowed && nf->func_str)
    {
      /* Check if the body declares a local with the same name (type keyword
       * immediately before the captured token).  This detects patterns like
       * "int x = 99;" where x shadows the parent's captured x. */
      const int *tp = tok_str_buf(nf->func_str);
      int prev = 0;
      while (*tp)
      {
        int tv;
        CValue tcv;
        tok_get(&tv, &tp, &tcv);
        if (tv == TOK_EOF || tv == 0)
          break;
        if (tv == ctok && (prev == TOK_INT || prev == TOK_CHAR || prev == TOK_SHORT ||
                           prev == TOK_LONG || prev == TOK_VOID || prev == TOK_FLOAT ||
                           prev == TOK_DOUBLE || prev == TOK_UNSIGNED || prev == TOK_SIGNED1 ||
                           prev == TOK_BOOL))
        {
          shadowed = 1;
          break;
        }
        prev = tv;
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
    if (s->nested_funcs[ni].sym == call_func_sym)
      return nested_has_genuine_capture(&s->nested_funcs[ni]);
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
    if (s->nested_funcs[ni].sym == call_func_sym) {
      callee_nf = &s->nested_funcs[ni];
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
    if (!shadowed && nf->func_str) {
      const int *tp = tok_str_buf(nf->func_str);
      int prev = 0;
      while (*tp) {
        int tv;
        CValue tcv;
        tok_get(&tv, &tp, &tcv);
        if (tv == TOK_EOF || tv == 0)
          break;
        if (tv == ctok && (prev == TOK_INT || prev == TOK_CHAR || prev == TOK_SHORT ||
                           prev == TOK_LONG || prev == TOK_VOID || prev == TOK_FLOAT ||
                           prev == TOK_DOUBLE || prev == TOK_UNSIGNED || prev == TOK_SIGNED1 ||
                           prev == TOK_BOOL)) {
          shadowed = 1;
          break;
        }
        prev = tv;
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
    int ident_idx = tokens[i] - TOK_IDENT;
    saved_labels[i] = table_ident[ident_idx]->sym_label;
    table_ident[ident_idx]->sym_label = NULL;
  }

  *tokens_out = tokens;
  *count_out = count;
  return saved_labels;
}

void inline_restore_label_bindings(int *tokens, Sym **saved_labels, int count)
{
  for (int i = 0; i < count; ++i)
  {
    int ident_idx = tokens[i] - TOK_IDENT;
    table_ident[ident_idx]->sym_label = saved_labels[i];
  }
  tcc_free(saved_labels);
  tcc_free(tokens);
}
