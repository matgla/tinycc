/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

/* yasc_errors.c -- the yasc dialect's error sets and error unions.
 *
 * An error is a non-zero int: an errno value (`error.ENOENT` once errno.h
 * defines the macro) or 0x100 + fnv1a(name) % 0xff00, the same in every file
 * without coordination.  `error NAME { ... }` is a set of them, an int enum
 * whose members are not global names; `error` alone is any error.
 *
 * `!T` holds a T or an error: a 64-bit value, T's bits in the low word and
 * the error (0 = none) in the high word, so the AAPCS returns it in r0:r1
 * and plain C sees an unsigned long long.  Its type is unsigned long long
 * with type.ref an interned node (YASC_ERRUNION) whose type is T.  try,
 * catch and the returns of a `!T` function are lowered here into branches:
 * nothing after the frontend knows about them.  docs/plans/yasc_errors.md. */

#include "gen_priv.h"

/* Interned error-union nodes and the set of `error` alone.  They are Syms
 * from the pools tccgen_finish frees: yasc_errors_reset drops the pointers. */
static Sym *eu_nodes;
static Sym *anyerror_set;

/* The error names this file uses, value -> name token (0 for a number that
 * a saved body had already expanded). */
typedef struct
{
  int value;
  int name;
  int flags; /* ERRNAME_*, as in the yasc_errname records */
} YascErrName;
static YascErrName *err_names;
static int nb_err_names;
#define ERRNAME_EXPLICIT 0 /* a set gave it its value */
#define ERRNAME_HASHED 1   /* the value is the name's hash */
#define ERRNAME_ERRNO 2    /* an errno macro */

static int uses_error_name;  /* error_name() is called: record errno's names */
static CType errname_type;   /* const char *__yasc_error_name(int) */

void yasc_errors_reset(void)
{
  eu_nodes = anyerror_set = NULL;
  tcc_free(err_names);
  err_names = NULL;
  nb_err_names = 0;
  uses_error_name = 0;
  errname_type.ref = NULL;
}

static void note_error_name(int value, int name, int flags)
{
  for (int i = 0; i < nb_err_names; i++)
    if (err_names[i].value == value && err_names[i].name == name)
      return;
  err_names = tcc_realloc(err_names, (nb_err_names + 1) * sizeof *err_names);
  err_names[nb_err_names].value = value;
  err_names[nb_err_names].name = name;
  err_names[nb_err_names].flags = flags;
  nb_err_names++;
}

static int value_of_error_name(int name)
{
  for (int i = 0; i < nb_err_names; i++)
    if (err_names[i].name == name)
      return err_names[i].value;
  return -1;
}

/* For messages: `error.NAME`, or `error N` when no name is known. */
static const char *error_value_str(int value, int prefer)
{
  static char buf[80];
  int name = 0;
  for (int i = 0; i < nb_err_names && !name; i++)
    if (err_names[i].value == value && err_names[i].name == prefer)
      name = prefer;
  for (int i = 0; i < nb_err_names && !name; i++)
    if (err_names[i].value == value)
      name = err_names[i].name;
  if (name)
    snprintf(buf, sizeof buf, "error.%s", get_tok_str(name, NULL));
  else
    snprintf(buf, sizeof buf, "error %d", value);
  return buf;
}

ST_FUNC int type_is_errunion(CType *type)
{
  return (type->t & VT_BTYPE) == VT_LLONG && type->ref && (type->ref->yasc & YASC_ERRUNION);
}

int type_is_errset(CType *type)
{
  return IS_ENUM(type->t) && type->ref && (type->ref->yasc & YASC_ERRSET);
}

static int payload_is_void(CType *eu)
{
  return (eu->ref->type.t & VT_BTYPE) == VT_VOID;
}

static Sym *anyerror(void)
{
  if (!anyerror_set)
  {
    anyerror_set = sym_push2(&eu_nodes, SYM_FIELD, VT_INT | VT_ENUM, 1);
    anyerror_set->yasc = YASC_ERRSET;
  }
  return anyerror_set;
}

static void anyerror_type(CType *type)
{
  type->t = VT_INT | VT_ENUM;
  type->ref = anyerror();
}

/* `error` or `error NAME`, for type_to_str */
const char *yasc_errset_name(CType *type)
{
  static char buf[128];
  Sym *s = type->ref;
  if (s == anyerror_set || (s->v & ~SYM_STRUCT) >= SYM_FIRST_ANOM)
    return "error";
  snprintf(buf, sizeof buf, "error %s", get_tok_str(s->v & ~SYM_STRUCT, NULL));
  return buf;
}

static int set_has(Sym *set, int value)
{
  if (set == anyerror_set)
    return 1;
  for (Sym *m = set->next; m; m = m->next)
    if (sym_enum_val(m) == value)
      return 1;
  return 0;
}

static int set_within(Sym *a, Sym *b)
{
  if (b == anyerror_set)
    return 1;
  if (a == anyerror_set)
    return 0;
  for (Sym *m = a->next; m; m = m->next)
    if (!set_has(b, (int)sym_enum_val(m)))
      return 0;
  return 1;
}

/* ------------------------------------------------------------------------- */
/* error values */

static int looks_like_errno_name(const char *s)
{
  if (s[0] != 'E' || !s[1])
    return 0;
  for (s++; *s; s++)
    if (!((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')))
      return 0;
  return 1;
}

/* The value of error name NAME that no macro defines; *FLAGS says where it
 * came from. */
static int hashed_error(int name, int *flags)
{
  const char *s = get_tok_str(name, NULL);
  unsigned h = 0x811c9dc5u;
  int known = value_of_error_name(name);

  *flags = ERRNAME_EXPLICIT;
  if (known > 0)
    return known; /* a set of this file gave it a value */
  *flags = ERRNAME_HASHED;
  if (looks_like_errno_name(s))
    tcc_error("error.%s needs <errno.h>", s);
  for (; *s; s++)
    h = (h ^ (unsigned char)*s) * 0x01000193u;
  return 0x100 + (int)(h % 0xff00u);
}

static void check_error_range(long long v, int name)
{
  if (v < 1 || v > 0xffff)
    tcc_error("error.%s: %lld is outside 1..65535", name ? get_tok_str(name, NULL) : "?", v);
}

/* `error` was read and tok is '.': push the error constant. */
static int last_error_name; /* for messages about the constant just pushed */
static void error_value(void)
{
  long long v;
  int name = 0, flags = ERRNAME_EXPLICIT;
  CType type;

  if (tok != '.')
    expect("'.'");
  v = pp_error_name();
  if (tok >= TOK_IDENT)
  {
    name = tok;
    if (v < 0)
      v = hashed_error(name, &flags);
    else if (looks_like_errno_name(get_tok_str(name, NULL)))
      flags = ERRNAME_ERRNO;
  }
  else if (v < 0)
    expect("error name");
  check_error_range(v, name);
  if (name)
    note_error_name((int)v, name, flags);
  last_error_name = name;
  next();
  vpushi((int)v);
  anyerror_type(&type);
  vtop->type = type;
}

/* error_from(x): an int as an error */
static void error_from(void)
{
  CType type;
  next();
  skip('(');
  expr_eq();
  skip(')');
  if (!is_integer_btype(vtop->type.t & VT_BTYPE) || type_is_errunion(&vtop->type))
    tcc_error("error_from takes an int");
  gen_cast_s(VT_INT);
  if (vtop->r & VT_LVAL)
    gv(RC_INT);
  anyerror_type(&type);
  vtop->type = type;
}

/* ------------------------------------------------------------------------- */
/* types */

/* tok follows `error`: an error set's definition, a named set, or `error`
 * alone (any error, whatever identifier follows is the declarator). */
void yasc_error_type(CType *type)
{
  Sym *s = NULL, *pool = NULL, **ps;
  int v = 0;

  if (tok >= TOK_UIDENT)
  {
    v = tok;
    s = struct_find(v);
    next();
    if (tok != '{')
    {
      if (s && (s->yasc & YASC_ERRSET))
      {
        type->t = s->type.t;
        type->ref = s;
        return;
      }
      unget_tok(v); /* the declarator's name */
      anyerror_type(type);
      return;
    }
    if (s && (s->yasc & YASC_ERRSET) && s->sym_scope == local_scope)
      tcc_error("redefinition of error %s", get_tok_str(v, NULL));
  }
  else if (tok != '{')
  {
    anyerror_type(type);
    return;
  }
  else
    v = anon_sym++;

  {
    CType t1;
    t1.t = VT_INT | VT_ENUM;
    t1.ref = NULL;
    s = sym_push(v | SYM_STRUCT, &t1, 0, 1);
    s->yasc = YASC_ERRSET;
  }
  ps = &s->next;
  /* tok is '{': each member is read unexpanded, so an errno name keeps its
   * name (pp_error_name) */
  for (;;)
  {
    long long mv = pp_error_name();
    int name = 0, flags = ERRNAME_EXPLICIT;
    Sym *m;
    CType mt;

    if (tok == '}')
      break;
    if (tok >= TOK_UIDENT)
    {
      name = tok;
      if (mv >= 0 && looks_like_errno_name(get_tok_str(name, NULL)))
        flags = ERRNAME_ERRNO;
    }
    else if (mv < 0)
      expect("error name");
    next();
    if (tok == '=')
    {
      next();
      mv = expr_const64();
      flags = ERRNAME_EXPLICIT;
    }
    else if (mv < 0)
      mv = hashed_error(name, &flags);
    check_error_range(mv, name);
    if (name)
      note_error_name((int)mv, name, flags);
    mt.t = VT_INT | VT_STATIC | VT_ENUM_VAL;
    mt.ref = s;
    m = sym_push2(&pool, name ? name : SYM_FIELD, mt.t, 0);
    m->type.ref = s;
    sym_set_enum_val(m, mv);
    *ps = m;
    ps = &m->next;
    if (tok != ',')
      break;
  }
  skip('}');
  type->t = s->type.t;
  type->ref = s;
}

/* `!` before the declaration's type: wrap what the declarator built before
 * its first function or array derivation (type_decl). */
void yasc_errunion_wrap(CType *type)
{
  const int bt = type->t & VT_BTYPE;
  Sym *n;
  CType pt;

  if ((type->t & (VT_ARRAY | VT_VLA | VT_COMPLEX | VT_VECTOR | VT_BITFIELD)) ||
      !(bt == VT_VOID || bt == VT_BOOL || bt == VT_BYTE || bt == VT_SHORT || bt == VT_INT || bt == VT_PTR))
  {
    char buf[256];
    type_to_str(buf, sizeof buf, type, NULL);
    tcc_error("!%s: an error union holds void, an integer of at most 32 bits, a pointer, an enum or an error set",
              buf);
  }
  pt.t = type->t & ~VT_STORAGE;
  pt.ref = type->ref;
  for (n = eu_nodes; n; n = n->prev)
    if ((n->yasc & YASC_ERRUNION) && n->type.t == pt.t && n->type.ref == pt.ref)
      break;
  if (!n)
  {
    n = sym_push2(&eu_nodes, SYM_FIELD, pt.t, 0);
    n->type.ref = pt.ref;
    n->yasc = YASC_ERRUNION;
  }
  type->t = VT_LLONG | VT_UNSIGNED | (type->t & VT_STORAGE);
  type->ref = n;
}

/* ------------------------------------------------------------------------- */
/* the representation */

/* An lvalue is read once, so both halves come from the same value. */
static void eu_stabilize(void)
{
  if ((vtop->r & VT_LVAL) && tcc_state->ir)
  {
    SValue dst;
    svalue_init(&dst);
    dst.type.t = VT_LLONG | VT_UNSIGNED;
    dst.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, vtop, NULL, &dst);
    vtop->vr = dst.vr;
    vtop->r = 0;
  }
}

static void eu_as_u64(void)
{
  vtop->type.t = VT_LLONG | VT_UNSIGNED;
  vtop->type.ref = NULL;
}

/* vtop: an error union -> its error, an int (0: none) */
static void eu_error_of(void)
{
  CType type;
  eu_as_u64();
  vpushi(32);
  gen_op(TOK_SHR);
  gen_cast_s(VT_INT);
  anyerror_type(&type);
  vtop->type = type;
}

/* vtop: an error union -> its value, of type PT (void: no value) */
static void eu_value_of(CType *pt)
{
  if ((pt->t & VT_BTYPE) == VT_VOID)
  {
    vpop();
    vpushi(0);
    vtop->type.t = VT_VOID;
    return;
  }
  eu_as_u64();
  gen_cast_s(VT_INT | VT_UNSIGNED);
  gen_cast(pt);
}

/* vtop: a value already of EU's payload type -> EU holding it */
static void eu_from_value(CType *eu)
{
  if (payload_is_void(eu))
  {
    vpop();
    vpush64(VT_LLONG | VT_UNSIGNED, 0);
  }
  else
  {
    gen_cast_s(VT_INT | VT_UNSIGNED);
    gen_cast_s(VT_LLONG | VT_UNSIGNED);
  }
  vtop->type.t = VT_LLONG | VT_UNSIGNED;
  vtop->type.ref = eu->ref;
}

/* vtop: an error -> EU holding it */
static void eu_from_error(CType *eu)
{
  gen_cast_s(VT_INT | VT_UNSIGNED);
  gen_cast_s(VT_LLONG | VT_UNSIGNED);
  vpushi(32);
  gen_op(TOK_SHL);
  vtop->type.t = VT_LLONG | VT_UNSIGNED;
  vtop->type.ref = eu->ref;
}

/* ------------------------------------------------------------------------- */
/* checks */

#define MISUSE "an error union needs try or catch here"
#define DISCARD "an error union is discarded: use try or catch"

ST_FUNC void yasc_errunion_misuse(void)
{
  tcc_error(MISUSE);
}

void yasc_errunion_discarded(void)
{
  tcc_error(DISCARD);
}

/* Storing vtop in DT (assignment, initializer, argument, return): an error
 * union in something else needs try or catch; anything into an error union
 * becomes one (a value, or an error); errors keep to their sets. */
void yasc_verify_errors_assign(CType *dt)
{
  if (type_is_errunion(dt))
  {
    if (type_is_errunion(&vtop->type))
    {
      if (vtop->type.ref != dt->ref)
        cast_error(&vtop->type, dt);
      return;
    }
    if (type_is_errset(&vtop->type))
    {
      eu_from_error(dt);
      return;
    }
    if (payload_is_void(dt))
      tcc_error("a !void error union holds no value");
    gen_assign_cast(&dt->ref->type);
    eu_from_value(dt);
    return;
  }
  if (type_is_errunion(&vtop->type))
    yasc_errunion_misuse();
  if (type_is_errset(dt))
  {
    Sym *ds = dt->ref;
    if (type_is_errset(&vtop->type))
    {
      Sym *ss = vtop->type.ref;
      if (ss == ds || ds == anyerror_set)
        return;
      if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
      {
        if (!set_has(ds, (int)vtop->c.i))
          tcc_error("%s is not a member of %s", error_value_str((int)vtop->c.i, last_error_name),
                    yasc_errset_name(dt));
        return;
      }
      if (!set_within(ss, ds))
      {
        char from[128];
        pstrcpy(from, sizeof from, yasc_errset_name(&vtop->type));
        tcc_error("%s does not convert to %s", from, yasc_errset_name(dt));
      }
      return;
    }
    if (is_integer_btype(vtop->type.t & VT_BTYPE))
      tcc_error("an int is not an error: use error_from()");
  }
}

/* ------------------------------------------------------------------------- */
/* try, catch, return */

/* `try` was read and the operand parsed. */
void yasc_try(void)
{
  CType eu;
  int a, saved_nocode_wanted;

  if (!type_is_errunion(&vtop->type))
    tcc_error("try needs an error union");
  if (defer_body_active())
    tcc_error("try in a defer body");
  if (!type_is_errunion(&func_vt))
    tcc_error("try in a function that does not return an error union");
  eu = vtop->type;
  eu_stabilize();
  vdup();
  eu_error_of();
  a = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
  saved_nocode_wanted = nocode_wanted;
  /* the error: return it, the same bits as the function's own union */
  vdup();
  vtop->type.ref = func_vt.ref;
  gen_return_emit(1, 1);
  gen_return_jump();
  tcc_ir_backpatch_to_here(tcc_state->ir, a);
  nocode_wanted = saved_nocode_wanted;
  eu_value_of(&eu.ref->type);
}

/* Copy vtop (of type T) into RES_VR, the value both arms of a catch leave. */
static void catch_store(int res_vr, CType *type)
{
  SValue src, dst;
  gv(RC_TYPE(type->t));
  svalue_init(&src);
  svalue_init(&dst);
  src.vr = vtop->vr;
  src.type = *type;
  dst.vr = res_vr;
  dst.type = *type;
  tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &src, NULL, &dst);
  vpop();
}

/* tok is `catch` after an expression. */
void yasc_catch(void)
{
  CType eu, pt;
  int has_value, completes = 1, value_result, res_vr = -1, bind = 0, a, d = -1, saved_nocode_wanted;
  int block_form;
  int saved_local_scope = local_scope;
  Sym *saved_local = local_stack;

  if (!type_is_errunion(&vtop->type))
    tcc_error("catch needs an error union");
  next();
  eu = vtop->type;
  pt = eu.ref->type;
  has_value = (pt.t & VT_BTYPE) != VT_VOID;
  if (tok == '|')
  {
    next();
    if (tok < TOK_UIDENT)
      expect("identifier");
    bind = tok;
    next();
    skip('|');
  }
  block_form = 0;
  eu_stabilize();
  if (has_value && tcc_state->ir)
    res_vr = tcc_ir_get_vreg_temp(tcc_state->ir);

  vdup();
  eu_error_of();
  if (bind)
  {
    /* `|e|`: a local of type `error` holding the error */
    CType et;
    Sym *es;
    anyerror_type(&et);
    ++local_scope;
    loc = tcc_ir_frame_alloc(loc, 4, -4);
    es = sym_push(bind, &et, VT_LOCAL | VT_LVAL, loc);
    vdup();
    vset(&et, VT_LOCAL | VT_LVAL, loc);
    vtop->sym = es;
    vtop->vr = es->vreg;
    vswap();
    vstore();
    vpop();
  }
  a = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
  saved_nocode_wanted = nocode_wanted;

  /* the error; the union stays on the stack below what the arm pushes */
  if (tok == '{')
  {
    block_form = 1;
    block(0);
    completes = !stmt_diverges && !(nocode_wanted & CODE_OFF_BIT);
    if (completes)
      d = gjmp(-1);
  }
  else
  {
    expr_cond();
    if ((vtop->type.t & VT_BTYPE) == VT_VOID && !(saved_nocode_wanted & CODE_OFF_BIT) &&
        (nocode_wanted & CODE_OFF_BIT))
    {
      /* `catch unreachable`, `catch abort()`: the arm does not complete, so
       * it owes the expression no value */
      vpop();
    }
    else
    {
      if (has_value)
      {
        gen_assign_cast(&pt);
        catch_store(res_vr, &pt);
      }
      else
        vpop();
      d = gjmp(-1);
    }
  }
  if (bind)
  {
    sym_pop(&local_stack, saved_local, 0);
    local_scope = saved_local_scope;
  }

  /* no error: the value.  A block that completes gives none: the whole
   * expression is void then. */
  tcc_ir_backpatch_to_here(tcc_state->ir, a);
  nocode_wanted = saved_nocode_wanted;
  value_result = has_value && !(block_form && completes);
  if (value_result)
  {
    eu_value_of(&pt);
    catch_store(res_vr, &pt);
  }
  else
    vpop();
  tcc_ir_backpatch_to_here(tcc_state->ir, d);
  nocode_wanted = saved_nocode_wanted;
  if (value_result)
  {
    vset(&pt, 0, 0);
    vtop->vr = res_vr;
  }
  else
  {
    vpushi(0);
    vtop->type.t = VT_VOID;
  }
}

/* `return` in a function returning `!T`; tok follows `return`, and the ';'
 * is left to the caller.  An error, or a union holding one, is an error exit:
 * the errdefers run too. */
void yasc_return_stmt(void)
{
  CType eu = func_vt;

  if (tok == ';')
  {
    if (!payload_is_void(&eu))
      tcc_error("'return' with no value");
    vpush64(VT_LLONG | VT_UNSIGNED, 0);
    vtop->type.ref = eu.ref;
    gen_return_emit(1, 0);
    return;
  }
  gexpr();
  if (type_is_errunion(&vtop->type))
  {
    int a, saved_nocode_wanted;
    if (vtop->type.ref != eu.ref)
      cast_error(&vtop->type, &eu);
    eu_stabilize();
    vdup();
    eu_error_of();
    a = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
    saved_nocode_wanted = nocode_wanted;
    vdup();
    gen_return_emit(1, 1);
    gen_return_jump();
    tcc_ir_backpatch_to_here(tcc_state->ir, a);
    nocode_wanted = saved_nocode_wanted;
    gen_return_emit(1, 0);
    return;
  }
  if (type_is_errset(&vtop->type))
  {
    eu_from_error(&eu);
    gen_return_emit(1, 1);
    return;
  }
  if (payload_is_void(&eu))
    tcc_error("a !void function returns a value");
  yasc_returning = 1;
  gen_assign_cast(&eu.ref->type);
  yasc_returning = 0;
  eu_from_value(&eu);
  gen_return_emit(1, 0);
}

/* The end of a `!void` function's body is reached: `return;` */
int yasc_void_union_falls_off(void)
{
  if (!type_is_errunion(&func_vt) || !payload_is_void(&func_vt))
    return 0;
  vpush64(VT_LLONG | VT_UNSIGNED, 0);
  vtop->type.ref = func_vt.ref;
  gen_return_emit(1, 0);
  return 1;
}

/* unary(): `try X`, `error.NAME`, `error_from(x)`.  Returns 0 when tok is
 * none of them here (a variable named error, say). */
int yasc_unary(void)
{
  if (tok == TOK_try)
  {
    next();
    unary();
    yasc_try();
    return 1;
  }
  if (tok == TOK_ERROR && !sym_find(TOK_ERROR))
  {
    next();
    error_value();
    return 1;
  }
  if (tok == TOK_error_from && !sym_find(TOK_error_from))
  {
    error_from();
    return 1;
  }
  if (tok == TOK_unreachable && !sym_find(TOK_unreachable))
  {
    /* `unreachable`: a void expression that traps when reached */
    CType vt;
    next();
    gen_unreachable(1);
    vt.t = VT_VOID;
    vt.ref = NULL;
    vpush(&vt);
    return 1;
  }
  if (tok == TOK_error_name && !sym_find(TOK_error_name))
  {
    /* a call of __yasc_error_name (libtcc1), which scans the yasc_errname
     * records; the postfix '(' parses the argument */
    Sym *f;
    next();
    if (tok != '(')
      expect("'('");
    if (!errname_type.ref)
    {
      Sym *p = sym_push2(&global_stack, SYM_FIELD, int_type.t, 0);
      Sym *s = sym_push2(&global_stack, SYM_FIELD, char_pointer_type.t, 0);
      s->type.ref = char_pointer_type.ref;
      s->f.func_call = FUNC_CDECL;
      s->f.func_type = FUNC_NEW;
      s->f.func_args = 1;
      s->next = p;
      errname_type.t = VT_FUNC;
      errname_type.ref = s;
    }
    uses_error_name = 1;
    f = external_global_sym(TOK___yasc_error_name, &errname_type);
    vpushsym(&f->type, f);
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* the yasc_errname records */

static void note_errno_macro(int tok, int value)
{
  note_error_name(value, tok, ERRNAME_ERRNO);
}

/* The end of the file: a record for every error name it used (all of
 * errno's when it calls error_name), {u16 value, u8 flags, u8 len, name,
 * NUL}, padded to 4.  libtcc1's __yasc_error_name reads them, and the
 * linker checks them (tcc_yasc_check_error_names). */
void yasc_errors_emit(TCCState *s1)
{
  Section *sec = NULL;

  if (uses_error_name)
    pp_errno_macros(note_errno_macro);
  for (int i = 0; i < nb_err_names; i++)
  {
    const char *n;
    int len, size;
    unsigned char *p;
    if (!err_names[i].name)
      continue;
    if (!sec)
    {
      sec = find_section(s1, YASC_ERRNAME_SECTION);
      sec->sh_addralign = 4;
    }
    n = get_tok_str(err_names[i].name, NULL);
    len = (int)strlen(n);
    if (len > 255)
      len = 255;
    size = (4 + len + 1 + 3) & ~3;
    p = section_ptr_add(sec, size);
    memset(p, 0, size);
    p[0] = err_names[i].value & 0xff;
    p[1] = (err_names[i].value >> 8) & 0xff;
    p[2] = (unsigned char)err_names[i].flags;
    p[3] = (unsigned char)len;
    memcpy(p + 4, n, len);
  }
}
