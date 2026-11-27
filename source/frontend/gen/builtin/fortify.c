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

/* fortify.c -- -ffortify: a call checked against what its declaration says
 * about its pointer arguments, at the call, with no macro and no _chk entry.
 *
 * The attributes (gcc's spelling; gcc only warns with them):
 * - null_terminated_string_arg(n): argument n is a C string, read up to its
 *   NUL.  The NUL has to be inside the object the argument points into:
 *   __tcc_fortify_str(p, size) (libtcc1) ends the program (__chk_fail) when
 *   there is none in the first size bytes; a null pointer passes;
 * - access(mode, ref, size): the function reads and/or writes up to argument
 *   `size` elements of argument `ref`'s pointed-to type (bytes for void):
 *   `size > object / element` traps (udf, as -fsanitize=bounds).  access()
 *   without a size, or mode none, checks nothing.
 *
 * The object is __builtin_object_size's (builtin/objsize.c), asked where the
 * caller names the buffer: the closest member for a string (glibc's level 2),
 * the whole object for a size; at run time also a VLA, or malloc(n) through
 * a variable n not stored to since (__builtin_dynamic_object_size).  A buffer
 * the compiler cannot see -- a pointer parameter, a pointer loaded from
 * memory -- is not checked.  A constant size that does not fit is also
 * -Wstringop-overflow; ssa:const_string_fold removes a string check its
 * content proves (a literal, a known local copy).
 *
 * On by default, at every -O: the attributes are the declaration's contract,
 * and the compiler keeps it -- no macro is needed.  -ccompat (regular C)
 * turns it off for that compile, unless _FORTIFY_SOURCE is defined and not
 * 0; -fno-fortify turns it off (it costs a check per call), -ffortify on. */

#include "gen_priv.h"

struct FortifyCall
{
  const SymLocalFacts *f;
  Sym *fsym;
  int nargs;              /* the parameters the checks name: 1..32 */
  unsigned want;          /* pointer arguments whose object is wanted, bit i */
  unsigned seen;          /* arguments parsed */
  unsigned in_ctx;        /* objsize_enter'ed for the argument being parsed */
  ObjRec saved;
  ObjInfo obj[32];        /* the wanted arguments' objects */
  unsigned is_const;      /* constant arguments, bit i, their values: */
  unsigned long long cval[32];
  unsigned char warned;   /* a constant size over its buffer was reported */
  unsigned char str_kind; /* strcpy / stpcpy (FORT_STRCPY), strcat (FORT_STRCAT) */
  unsigned char kept_dst; /* strcat: its destination, in a register: */
  SValue kdst;
  unsigned kept;          /* sizes before their pointer, in a register: */
  SValue kval[32];
  struct FortifyCall *live_outer;
};

FortifyCall *fortify_cur_call;

/* begun and not ended, innermost first (objsize.c's objsize_live) */
static FortifyCall *fortify_live;

void *fortify_calls_mark(void)
{
  return fortify_live;
}

void fortify_calls_unwind(void *mark)
{
  while (fortify_live && fortify_live != mark)
  {
    FortifyCall *fc = fortify_live;
    fortify_live = fc->live_outer;
    if (fc->in_ctx)
      objsize_leave(&fc->saved);
    if (fortify_cur_call == fc)
      fortify_cur_call = NULL;
    tcc_free(fc);
  }
}

static int fortify_macro_on(void)
{
  Sym *d = define_find(tok_alloc_const("_FORTIFY_SOURCE"));
  const int *p;
  CValue cv;
  int t;
  if (!d)
    return 0;
  p = d->d;
  if (!p)
    return 1;
  tok_get(&t, &p, &cv);
  /* `#define _FORTIFY_SOURCE LEVEL`: what LEVEL says (a few levels deep) */
  for (int depth = 0; depth < 8 && t >= TOK_IDENT && (d = define_find(t)) && d->d; depth++)
  {
    p = d->d;
    tok_get(&t, &p, &cv);
  }
  if (t == TOK_PPNUM)
    return strcmp(cv.str.data, "0") != 0;
  if (t == TOK_CINT || t == TOK_CUINT)
    return cv.i != 0;
  return 1;
}

static int fortify_on(void)
{
  if (tcc_state->fortify != 2)
    return tcc_state->fortify;
  return !tcc_state->ccompat || fortify_macro_on();
}

/* -ffortify is on for this compile (chk.c's implicit sprintf calls too) */
int fortify_wanted(void)
{
  return fortify_on();
}

/* the merged attributes of every declaration (decl/attribute.c) */
void fortify_apply_attrs(Sym *s, SymLocalFacts *f, AttributeDef *ad)
{
  f->fort_cstr |= ad->attr_cstr_args;
  for (int i = 0; i < ad->attr_access_n; i++)
  {
    const unsigned char *a = ad->attr_access[i];
    int j;
    for (j = 0; j < f->fort_n; j++)
      if (f->fort_access[j][1] == a[1] && f->fort_access[j][2] == a[2])
        break;
    if (j < f->fort_n)
      f->fort_access[j][0] = a[0];
    else if (f->fort_n < FORTIFY_MAX_ACCESS)
      memcpy(f->fort_access[f->fort_n++], a, 3);
  }
  (void)s;
}

/* parameter I's (from 0) type of the function FSYM, NULL past the list */
static CType *param_type(Sym *fsym, int i)
{
  Sym *p = sym_first(fsym->type.ref);
  while (p && i--)
    p = p->next;
  return p ? &p->type : NULL;
}

static int ptr_param(Sym *fsym, int i)
{
  CType *t = param_type(fsym, i);
  return t && (t->t & VT_BTYPE) == VT_PTR;
}

enum
{
  FORT_STRCPY = 1,
  FORT_STRCAT
};

/* FSYM is the C library's strcpy / stpcpy (FORT_STRCPY) or strcat
   (FORT_STRCAT), checked by name as gcc knows them: the destination has to
   hold the source's string (and, for strcat, what it holds already) -- no
   size argument says so.  0 for anything else. */
static int fortify_str_kind(Sym *fsym)
{
  const char *name;
  ElfSym *es;
  int k;
  if ((fsym->type.t & (VT_BTYPE | VT_STATIC)) != VT_FUNC || !fsym->type.ref ||
      fsym->type.ref->f.func_type != FUNC_NEW || (tcc_state->no_builtin_funcs & NO_BUILTIN_ALL) || !ptr_param(fsym, 0) ||
      !ptr_param(fsym, 1))
    return 0;
  name = get_tok_str(fsym->v, NULL);
  k = !strcmp(name, "strcpy") || !strcmp(name, "stpcpy") ? FORT_STRCPY : !strcmp(name, "strcat") ? FORT_STRCAT : 0;
  /* the program's own, defined here: not the library's */
  if (k && (fsym->r & VT_SYM) && (es = elfsym(fsym)) && es->st_shndx != SHN_UNDEF)
    return 0;
  return k;
}

FortifyCall *fortify_call_begin(Sym *fsym)
{
  static const SymLocalFacts no_facts;
  const SymLocalFacts *f = NULL;
  FortifyCall *fc;
  unsigned want = 0;
  int nargs = 0, str_kind;

  if (!fsym || (fsym->type.t & VT_BTYPE) != VT_FUNC || tcc_state->reverse_funcargs || !tcc_state->ir || nocode_wanted)
    return NULL;
  if (fsym->a.attr_facts)
    f = sym_facts_peek(fsym);
  if (f && !(f->fort_cstr || f->fort_n))
    f = NULL;
  str_kind = fortify_str_kind(fsym);
  if ((!f && !str_kind) || !fortify_on())
    return NULL;
  if (!f)
    f = &no_facts;
  if (str_kind)
    want |= 1u, nargs = 2; /* the destination's object */
  for (int i = 0; i < 32; i++)
    if ((f->fort_cstr & (1u << i)) && ptr_param(fsym, i))
      want |= 1u << i, nargs = i + 1;
  for (int j = 0; j < f->fort_n; j++)
  {
    const unsigned char *a = f->fort_access[j];
    if (a[0] == ATTR_ACCESS_NONE || !a[2] || a[1] > 32 || a[2] > 32 || !ptr_param(fsym, a[1] - 1))
      continue;
    want |= 1u << (a[1] - 1);
    if (a[1] > nargs)
      nargs = a[1];
    if (a[2] > nargs)
      nargs = a[2];
  }
  if (!want)
    return NULL;
  fc = tcc_mallocz(sizeof *fc);
  fc->live_outer = fortify_live;
  fortify_live = fc;
  fc->f = f;
  fc->fsym = fsym;
  fc->nargs = nargs;
  fc->want = want;
  fc->str_kind = str_kind;
  return fc;
}

/* argument I is about to be parsed */
void fortify_arg_before(FortifyCall *fc, int i)
{
  if (i < 32 && (fc->want & (1u << i)))
  {
    objsize_enter(&fc->saved);
    fc->in_ctx = 1;
  }
}

/* argument I is on vtop, as written (before objsize_call_arg, whose
   objsize_enter came first) */
void fortify_arg(FortifyCall *fc, int i)
{
  if (fc->in_ctx)
  {
    objsize_info(vtop, &fc->obj[i]);
    objsize_leave(&fc->saved);
    fc->in_ctx = 0;
  }
}

/* push argument I's object size (closest member: TYPE 1, whole: 0) and
   return 1, or return 0 when nothing is known */
static int push_objsize(FortifyCall *fc, int i, int type)
{
  const ObjInfo *oi = &fc->obj[i];
  unsigned long long sz = objsize_result(oi, type);
  if (sz != (unsigned long long)(addr_t)-1)
  {
    vpushs((addr_t)sz);
    return 1;
  }
  return objsize_push_dynamic(oi, type);
}

/* a string in read-only data -- a literal, a const array -- whose NUL is in
   its N bytes from where the argument points: nothing can change that */
static int rodata_terminated(const ObjInfo *oi, unsigned long long n)
{
  ElfSym *es;
  Section *sec;
  unsigned long long at;
  if (oi->kind != OI_DECL || !oi->obj || oi->off_unknown || oi->off < 0 || !(oi->obj->r & VT_SYM) || !n)
    return 0;
  es = elfsym(oi->obj);
  if (!es || es->st_shndx == SHN_UNDEF || es->st_shndx >= SHN_LORESERVE || es->st_shndx >= tcc_state->nb_sections)
    return 0;
  sec = tcc_state->sections[es->st_shndx];
  if (!sec || (sec->sh_flags & SHF_WRITE) || sec->sh_type != SHT_PROGBITS)
    return 0;
  at = (unsigned long long)es->st_value + (unsigned long long)oi->off;
  if (at + n > sec->data_offset)
    return 0;
  return memchr(sec->data + at, 0, (size_t)n) != NULL;
}

/* vtop: argument I, converted to its parameter's type -- the string check */
static void check_cstr(FortifyCall *fc, int i)
{
  SValue args[2];
  unsigned long long sz = objsize_result(&fc->obj[i], 1);
  if (sz != (unsigned long long)(addr_t)-1 && rodata_terminated(&fc->obj[i], sz))
    return;
  if (!push_objsize(fc, i, 1))
    return;
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
    gv(RC_INT);
  /* the pointer goes to the check and to the call: once in a register */
  if (vtop[-1].r & VT_LVAL)
  {
    vswap();
    gv(RC_INT);
    vswap();
  }
  args[1] = *vtop;
  args[0] = vtop[-1];
  gen_ir_void_call_args(args, 2, tok_alloc_const("__tcc_fortify_str"));
  vpop();
}

/* strcpy / stpcpy / strcat: vtop is the source, converted; the destination's
   object was taken at argument 0.  libtcc1 ends the program when the string
   (strcat: and the one already there) does not fit; a source whose length
   is known to fit, or an empty one appended, costs nothing. */
static void check_str(FortifyCall *fc)
{
  SValue args[3];
  unsigned long long sz = objsize_result(&fc->obj[0], 1), sl;
  int n = 0;
  const int known = svalue_get_conservative_string_bytes_u64(vtop, &sl);
  if (fc->str_kind == FORT_STRCPY && known && sz != (unsigned long long)(addr_t)-1 && sl <= sz)
    return;
  if (fc->str_kind == FORT_STRCAT && ((known && sl == 1) || !fc->kept_dst))
    return;
  if (!push_objsize(fc, 0, 1))
    return;
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
    gv(RC_INT);
  /* the source goes to the check and to the call: once in a register */
  if (vtop[-1].r & VT_LVAL)
  {
    vswap();
    gv(RC_INT);
    vswap();
  }
  if (fc->str_kind == FORT_STRCAT)
    args[n++] = fc->kdst;
  args[n++] = vtop[-1];
  args[n++] = *vtop;
  gen_ir_void_call_args(args, n,
                        tok_alloc_const(fc->str_kind == FORT_STRCPY ? "__tcc_fortify_strcpy" : "__tcc_fortify_strcat"));
  vpop();
}

/* access entry A: both arguments are in.  vtop is argument CUR (A's pointer
   or its size), converted. */
static void check_access(FortifyCall *fc, const unsigned char *a, int cur)
{
  int pi = a[1] - 1, si = a[2] - 1, align;
  CType *pt = param_type(fc->fsym, pi);
  int el = pt ? type_size(pointed_type(pt), &align) : 1;
  if (el <= 0)
    el = 1; /* void, an incomplete type: bytes */
  if (cur == si)
  {
    vdup();
    gen_cast_s(VT_SIZE_T);
  }
  else if (fc->is_const & (1u << si))
    vpushs((addr_t)fc->cval[si]);
  else if (fc->kept & (1u << si))
  {
    vpushv(&fc->kval[si]);
    gen_cast_s(VT_SIZE_T);
  }
  else
    return;
  if (!push_objsize(fc, pi, 0))
  {
    vpop();
    return;
  }
  if ((vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST &&
      (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST &&
      (unsigned long long)vtop[-1].c.i > (unsigned long long)vtop->c.i / (unsigned)el)
  {
    fc->warned = 1;
    tcc_warning_c(warn_stringop_overflow)("'%s' accesses %llu bytes of an object of %llu: the call ends the program",
                                          get_tok_str(fc->fsym->v, NULL),
                                          (unsigned long long)vtop[-1].c.i * (unsigned)el,
                                          (unsigned long long)vtop->c.i);
  }
  if (el > 1)
  {
    vpushs(el);
    gen_op(TOK_UDIV);
  }
  gen_op(TOK_UGT);
  fe_trap_if();
}

/* argument I is on vtop, converted to its parameter's type; its FUNCPARAMVAL
   comes next */
void fortify_arg_emit(FortifyCall *fc, int i)
{
  if (i >= fc->nargs || NOEVAL_WANTED)
    return;
  fc->seen |= 1u << i;
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && is_integer_btype(vtop->type.t & VT_BTYPE))
  {
    fc->is_const |= 1u << i;
    fc->cval[i] = vtop->c.i;
  }
  else if (is_integer_btype(vtop->type.t & VT_BTYPE))
  {
    /* a size whose pointer comes after it: its value, kept for that check */
    for (int j = 0; j < fc->f->fort_n; j++)
    {
      const unsigned char *a = fc->f->fort_access[j];
      if (a[0] != ATTR_ACCESS_NONE && a[2] == i + 1 && a[1] > i + 1 && a[1] <= 32 && (fc->want & (1u << (a[1] - 1))))
      {
        gv(RC_INT);
        fc->kval[i] = *vtop;
        fc->kept |= 1u << i;
        break;
      }
    }
  }
  if (fc->str_kind == FORT_STRCAT && i == 0)
  {
    /* strcat's destination goes to its check too (check_str) */
    gv(RC_INT);
    fc->kdst = *vtop;
    fc->kept_dst = 1;
  }
  if (fc->str_kind && i == 1)
    check_str(fc);
  if (fc->f->fort_cstr & (1u << i) & fc->want)
    check_cstr(fc, i);
  for (int j = 0; j < fc->f->fort_n; j++)
  {
    const unsigned char *a = fc->f->fort_access[j];
    int pi = a[1] - 1, si = a[2] - 1;
    if (a[0] == ATTR_ACCESS_NONE || !a[2] || pi >= 32 || si >= 32 || !(fc->want & (1u << pi)))
      continue;
    if ((i == pi || i == si) && (fc->seen & (1u << pi)) && (fc->seen & (1u << si)))
      check_access(fc, a, i);
  }
}

/* FC reported a sure overflow: the plain function's check (objsize.c) need
   not say it again */
int fortify_call_warned(const FortifyCall *fc)
{
  return fc->warned;
}

void fortify_call_end(FortifyCall *fc)
{
  FortifyCall **pp = &fortify_live;
  while (*pp && *pp != fc)
    pp = &(*pp)->live_outer;
  if (*pp)
    *pp = fc->live_outer;
  if (fc->in_ctx)
    objsize_leave(&fc->saved);
  tcc_free(fc);
}
