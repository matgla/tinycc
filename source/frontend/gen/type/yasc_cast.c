/*
 *  yasc casts: cast<PERMISSION(TYPE), MODIFIER, ...>(EXPR) and unsafe_cast<...>
 *  (docs/yasc_casts.md).
 *
 *  A C cast does whatever it takes to turn a value into a type.  A yasc cast
 *  lists what may change -- one permission carries the target type, the
 *  modifiers (align, qual, const) add to a pointer cast -- and any other
 *  change is an error.  The value checks (int, enum, align) are a test and a
 *  trap where the types do not prove them (op/ubsan.c yasc_cast_check_*).
 *  The unsafe permissions (qual, const, fn, addr from an integer) need
 *  unsafe_cast.
 *
 *  In yasc a C cast is -Wold-style-cast (an error in the safe default),
 *  whose message names the cast to write: the same classification.
 *
 *  This file is part of TinyCC.  LGPL 2.1 or later (see COPYING).
 */

#include "gen_priv.h"

/* the permissions */
#define YC_AS 0x001
#define YC_INT 0x002
#define YC_TRUNC 0x004
#define YC_BITS 0x008
#define YC_FLOAT 0x010
#define YC_ENUM 0x020
#define YC_PTR 0x040
#define YC_ALIGN 0x080
#define YC_ADDR 0x100
#define YC_QUAL 0x200
#define YC_FN 0x400
#define YC_CONST 0x800 /* drops const from a const object: an interface's `char *` (dlerror) */
#define YC_VALUE (YC_AS | YC_INT | YC_TRUNC | YC_BITS | YC_FLOAT | YC_ENUM)
#define YC_POINTER (YC_PTR | YC_ALIGN | YC_ADDR | YC_QUAL | YC_FN | YC_CONST)

static const struct
{
  const char *name;
  int bit;
} yc_names[] = {{"as", YC_AS},       {"int", YC_INT},     {"trunc", YC_TRUNC}, {"bits", YC_BITS},
                {"float", YC_FLOAT}, {"enum", YC_ENUM},   {"ptr", YC_PTR},     {"align", YC_ALIGN},
                {"addr", YC_ADDR},   {"qual", YC_QUAL},   {"fn", YC_FN},       {"const", YC_CONST}};

static const char *yc_name(int bit)
{
  for (size_t i = 0; i < sizeof yc_names / sizeof yc_names[0]; i++)
    if (yc_names[i].bit == bit)
      return yc_names[i].name;
  return "?";
}

static int yc_bit_of(int t)
{
  const char *s = get_tok_str(t, NULL);
  for (size_t i = 0; i < sizeof yc_names / sizeof yc_names[0]; i++)
    if (!strcmp(yc_names[i].name, s))
      return yc_names[i].bit;
  return 0;
}

/* ---------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------- */

static int yc_bt(const CType *t)
{
  return t->t & VT_BTYPE;
}

static int yc_is_int(const CType *t)
{
  return is_integer_btype(yc_bt(t)) && !(t->t & (VT_VECTOR | VT_COMPLEX));
}

static int yc_is_flt(const CType *t)
{
  return is_float(t->t) && !(t->t & (VT_VECTOR | VT_COMPLEX));
}

static int yc_is_ptr(const CType *t)
{
  return yc_bt(t) == VT_PTR;
}

/* a pointer to a function */
static int yc_is_fnptr(CType *t)
{
  return yc_is_ptr(t) && yc_bt(pointed_type(t)) == VT_FUNC;
}

static int yc_is_voidptr(CType *t)
{
  return yc_is_ptr(t) && yc_bt(pointed_type(t)) == VT_VOID;
}

/* an integer type's width in bits and signedness (bool: 1 bit, unsigned) */
static int yc_int_bits(const CType *t, int *uns)
{
  int a;
  if (yc_bt(t) == VT_BOOL)
  {
    *uns = 1;
    return 1;
  }
  *uns = (t->t & VT_UNSIGNED) != 0;
  return type_size((CType *)t, &a) * 8;
}

/* T's range: [*lo, *hi] */
static void yc_int_range(const CType *t, long long *lo, unsigned long long *hi)
{
  int uns, n = yc_int_bits(t, &uns);
  if (uns)
  {
    *lo = 0;
    *hi = n >= 64 ? ~0ull : (1ull << n) - 1;
  }
  else
  {
    *lo = n >= 64 ? (long long)(1ull << 63) : -(1ll << (n - 1));
    *hi = (1ull << (n - 1)) - 1;
  }
}

/* every value of integer type S is a T */
static int yc_int_fits(const CType *s, const CType *t)
{
  int us, ut, bs = yc_int_bits(s, &us), bt = yc_int_bits(t, &ut);
  if (us)
    return ut ? bs <= bt : bs < bt;
  return !ut && bs <= bt;
}

/* the mantissa of a floating type, in bits */
static int yc_mant(const CType *t)
{
  switch (yc_bt(t))
  {
  case VT_FLOAT:
    return 24;
  case VT_DOUBLE:
    return 53;
  default:
    return LDOUBLE_SIZE == 8 ? 53 : 64;
  }
}

/* the source as it is converted: an array is its first element's address,
   a function its address */
static CType yc_decayed(const CType *st)
{
  CType s = *st;
  if (s.t & VT_ARRAY)
  {
    s.t &= ~(VT_ARRAY | VT_VLA);
    s.t &= ~VT_STORAGE;
  }
  else if (yc_bt(&s) == VT_FUNC)
    mk_pointer(&s);
  s.t &= ~VT_STORAGE;
  return s;
}

/* the same type at every pointer level, qualifiers aside */
static int yc_same_unqualified(CType *a, CType *b)
{
  while (yc_is_ptr(a) && yc_is_ptr(b))
    a = pointed_type(a), b = pointed_type(b);
  if (yc_is_ptr(a) || yc_is_ptr(b))
    return 0;
  return compare_types(a, b, 1);
}

/* what T * from S * does to the qualifiers: 0 nothing unsafe, 1 drops
   const/volatile at some level, 2 adds const below a level that is not const
   (`char **` to `const char **`); *DROPPED: the qualifiers dropped */
static int yc_qual_change(CType *dt, CType *st, int *dropped)
{
  CType *t1 = pointed_type(dt), *t2 = pointed_type(st);
  int all_const = 1;
  *dropped = 0;
  for (int lvl = 0;; lvl++)
  {
    int drop = t2->t & ~t1->t & (VT_CONSTANT | VT_VOLATILE);
    if (drop)
    {
      *dropped = drop;
      return 1;
    }
    if (lvl && !all_const && (t1->t & ~t2->t & (VT_CONSTANT | VT_VOLATILE)))
      return 2;
    if (!(t1->t & VT_CONSTANT))
      all_const = 0;
    if (!yc_is_ptr(t1) || !yc_is_ptr(t2))
      return 0;
    t1 = pointed_type(t1);
    t2 = pointed_type(t2);
  }
}

/* the alignment the operand (of type ST before decay) is known to have */
static int yc_known_align(CType *st, CType *s)
{
  int al = 1, place;
  CType *pt = pointed_type(s);
  if (yc_bt(pt) != VT_VOID && yc_bt(pt) != VT_FUNC && type_size(pt, &al) < 0)
    al = 1;
  if (vtop->packed_addr)
    al = packed_member_addr.align; /* &packed->m: what the place guarantees */
  place = cast_source_place_align(st);
  if (place > al)
    al = place;
  return al;
}

/* the object the operand designates directly (an array, `&x`), or NULL;
   *OT its type (an array's element) */
static int yc_object_quals(CType *st)
{
  Sym *sym;
  if (st->t & VT_ARRAY)
    return pointed_type(st)->t & (VT_CONSTANT | VT_VOLATILE);
  if ((vtop->r & VT_LVAL) || (vtop->r & VT_VALMASK) == VT_CMP || !(sym = vtop->sym) || !yc_is_ptr(st))
    return 0;
  if ((vtop->r & VT_VALMASK) == VT_LOCAL ? vtop->c.i != sym->c
                                          : (vtop->r & (VT_VALMASK | VT_SYM)) != (VT_CONST | VT_SYM) || vtop->c.i)
    return 0;
  if (!compare_types(&sym->type, pointed_type(st), 1))
    return 0;
  return sym->type.t & (VT_CONSTANT | VT_VOLATILE);
}

static int yc_const_operand(void)
{
  return (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
}

/* ---------------------------------------------------------------------
 * What a conversion needs
 * ------------------------------------------------------------------- */

typedef struct YcNeed
{
  int perms;     /* the permissions it needs (YC_AS: none beyond lossless) */
  int alt;       /* another permission that would do (trunc beside int) */
  int unsafe;    /* it needs unsafe_cast */
  int from_al;   /* align: what the source is known to have */
  int to_al;     /* ... and what the target needs */
  int dropped;   /* qual: the qualifiers dropped (0: a nested const added) */
  const char *bad; /* no cast does it: why */
} YcNeed;

/* the conversion of the operand (vtop, of type ST) to DT */
static void yc_need(CType *st, CType *dt, YcNeed *n)
{
  CType s = yc_decayed(st);
  memset(n, 0, sizeof *n);
  if (yc_bt(dt) == VT_VOID)
  {
    n->bad = "a cast to void: write (void)x";
    return;
  }
  if (dt->t & VT_ARRAY || yc_bt(dt) == VT_FUNC)
  {
    n->bad = "no value has an array or a function type";
    return;
  }
  /* pointers */
  if (yc_is_ptr(&s) || yc_is_ptr(dt))
  {
    if (yc_is_ptr(&s) && yc_is_ptr(dt))
    {
      int q;
      if (yc_is_fnptr(&s) || yc_is_fnptr(dt))
      {
        if (!compare_types(&s, dt, 1))
          n->perms |= YC_FN, n->unsafe = 1;
        return;
      }
      if (!yc_is_voidptr(dt) && !yc_same_unqualified(pointed_type(&s), pointed_type(dt)))
      {
        int al = 1;
        n->perms |= YC_PTR;
        if (!yc_is_voidptr(&s) && yc_bt(pointed_type(dt)) != VT_VOID && type_size(pointed_type(dt), &al) >= 0 &&
            al > (n->from_al = yc_known_align(st, &s)))
          n->perms |= YC_ALIGN, n->to_al = al;
      }
      else if (yc_is_voidptr(dt) || yc_same_unqualified(pointed_type(&s), pointed_type(dt)))
      {
        /* the same pointee: only an alignment the place cannot vouch for
           (`(u32 *)&packed->u`) */
        int al = 1;
        if (vtop->packed_addr && yc_bt(pointed_type(dt)) != VT_VOID && type_size(pointed_type(dt), &al) >= 0 &&
            al > packed_member_addr.align)
          n->perms |= YC_ALIGN, n->to_al = al, n->from_al = packed_member_addr.align;
      }
      if ((q = yc_qual_change(dt, &s, &n->dropped)))
        n->perms |= YC_QUAL, n->unsafe = 1;
      if (!n->perms)
        n->perms = YC_AS;
      return;
    }
    if (yc_is_ptr(&s))
    {
      int al;
      if (!yc_is_int(dt) || yc_bt(dt) == VT_BOOL)
      {
        n->bad = yc_bt(dt) == VT_BOOL ? "a pointer is not a bool: compare it with nullptr" : "a pointer is only an address: addr(uintptr_t)";
        return;
      }
      if (type_size(dt, &al) < PTR_SIZE)
      {
        n->bad = "an address does not fit that integer: addr(uintptr_t)";
        return;
      }
      n->perms = YC_ADDR;
      return;
    }
    if (yc_is_int(&s) && yc_bt(&s) != VT_BOOL)
    {
      n->perms = YC_ADDR, n->unsafe = 1;
      return;
    }
    n->bad = "only an integer becomes a pointer: addr(T *)";
    return;
  }
  /* values */
  if (yc_bt(&s) == VT_STRUCT || yc_bt(dt) == VT_STRUCT || (s.t | dt->t) & (VT_VECTOR | VT_COMPLEX))
  {
    if (compare_types(&s, dt, 1))
      n->perms = YC_AS;
    else
      n->perms = YC_BITS;
    return;
  }
  if (IS_ENUM(dt->t))
  {
    if (IS_ENUM(s.t) && compare_types(&s, dt, 1))
      n->perms = YC_AS;
    else if (yc_is_int(&s))
      n->perms = YC_ENUM;
    else
      n->bad = "only an integer becomes an enum: enum(E)";
    return;
  }
  if (yc_bt(dt) == VT_BOOL && yc_bt(&s) != VT_BOOL)
  {
    n->bad = "a value is not a bool: compare it (x != 0)";
    return;
  }
  if (yc_is_int(dt))
  {
    if (yc_is_int(&s))
    {
      if (yc_int_fits(&s, dt))
        n->perms = YC_AS;
      else
        n->perms = YC_INT, n->alt = YC_TRUNC;
    }
    else if (yc_is_flt(&s))
      n->perms = YC_INT;
    else
      n->bad = "no such conversion";
    return;
  }
  if (yc_is_flt(dt))
  {
    if (yc_is_int(&s))
    {
      int us, bs = yc_int_bits(&s, &us);
      n->perms = bs - !us <= yc_mant(dt) ? YC_AS : YC_FLOAT;
    }
    else if (yc_is_flt(&s))
    {
      int as, ad;
      n->perms = type_size(&s, &as) <= type_size(dt, &ad) ? YC_AS : YC_FLOAT;
    }
    else
      n->bad = "no such conversion";
    return;
  }
  n->bad = "no such conversion";
}

/* the cast that does it, for a message: `cast<int(unsigned short)>` */
static void yc_spell(char *buf, int size, CType *dt, const YcNeed *n)
{
  char tn[160];
  int main_bit = n->perms & (YC_VALUE | YC_PTR | YC_ADDR | YC_FN);
  CType t = *dt;
  t.t &= ~VT_STORAGE;
  type_to_str(tn, sizeof tn, &t, NULL);
  if (!main_bit && (n->perms & YC_QUAL))
    snprintf(buf, size, "unsafe_cast<qual(%s)%s>", tn, (n->perms & YC_ALIGN) ? ", align" : "");
  else if (!main_bit && (n->perms & YC_ALIGN))
    snprintf(buf, size, "cast<align(%d)>", n->to_al);
  else
    snprintf(buf, size, "%s<%s(%s)%s%s>", n->unsafe ? "unsafe_cast" : "cast", yc_name(main_bit ? main_bit : YC_AS), tn,
             (n->perms & YC_QUAL) && main_bit ? ", qual" : "", (n->perms & YC_ALIGN) ? ", align" : "");
}

/* ---------------------------------------------------------------------
 * The C cast in yasc
 * ------------------------------------------------------------------- */

/* `(T)x` in yasc (unary_paren, vtop the operand): the cast to write */
void yasc_old_style_cast(CType *dt)
{
  YcNeed n;
  char spell[240];
  if (is_null_pointer(vtop) && yc_is_ptr(dt))
  {
    tcc_warning_once(warn_old_style_cast, "C cast: write nullptr");
    return;
  }
  yc_need(&vtop->type, dt, &n);
  if (n.bad)
  {
    tcc_warning_once(warn_old_style_cast, "C cast: %s", n.bad);
    return;
  }
  yc_spell(spell, sizeof spell, dt, &n);
  if (n.alt)
    tcc_warning_once(warn_old_style_cast, "C cast: write %s(...), or cast<%s(...)>(...) to keep the low bits", spell,
                     yc_name(n.alt));
  else
    tcc_warning_once(warn_old_style_cast, "C cast: write %s(...)", spell);
}

/* ---------------------------------------------------------------------
 * cast<...>(x)
 * ------------------------------------------------------------------- */

/* T spells no owned/view/maybe/borrows: it takes the source's */
static void yc_inherit_yasc(CType *t, CType *s)
{
  int q;
  if (!yc_is_ptr(t) || !t->ref)
    return;
  q = type_yasc(t);
  if ((q & (YASC_OWNED | YASC_VIEW | YASC_MAYBE)) || t->ref->vreg >= 0)
    return;
  if (!yc_is_ptr(s) || !s->ref || (!(type_yasc(s) & ~YASC_RESTRICT) && s->ref->vreg < 0))
    return;
  t->ref = sym_pointer_node_b(&t->ref->type, type_yasc(s) | (q & YASC_RESTRICT), s->ref->vreg);
}

/* T spells owned/view/maybe/borrows of its own, different from S's: what
   the types cannot check, as for an explicit C cast (assign_check.c
   yasc_cast_check) -- the qualifiers aside, which qual/addr say */
static void yc_yasc_quals(CType *s, CType *t)
{
  int dq = type_yasc(t), sq = type_yasc(s);
  const char *what;
  if (!(dq & (YASC_OWNED | YASC_VIEW)) || (dq & YASC_UNCHECKED) || type_is_handle(t) || is_null_pointer(vtop))
    return;
  what = (dq & YASC_OWNED) ? "owned" : type_borrows(t) ? "a borrow" : "a view";
  if (sq & YASC_UNCHECKED)
    tcc_warning_once(warn_unchecked, "a pointer from regular C becomes %s here: write unsafe(...)", what);
  else if ((sq & YASC_MAYBE) && !(dq & YASC_MAYBE))
    tcc_warning_once(warn_unchecked, "a cast drops maybe: test the pointer, or write unsafe(...)");
  else if ((dq & YASC_OWNED) && !(sq & YASC_OWNED))
    tcc_warning_once(warn_unchecked, "a cast makes a pointer that owns nothing owned: write unsafe(...)");
  else if (!(dq & YASC_OWNED) && (sq & YASC_OWNED))
    tcc_warning_once(warn_unchecked, "a cast makes an owned pointer a view: write borrow(...)");
}

static void yc_value_checks(int perm, CType *dt)
{
  CType s = vtop->type;
  const int checks = !tcc_state->cast_checks_off;
  long long lo;
  unsigned long long hi;
  char sn[128], tn[128];

  if (perm == YC_INT || perm == YC_ENUM)
    yc_int_range(perm == YC_ENUM ? dt : dt, &lo, &hi);
  type_to_str(tn, sizeof tn, dt, NULL);
  if (perm == YC_INT && yc_is_int(&s) && !yc_int_fits(&s, dt))
  {
    if (yc_const_operand())
    {
      int us;
      long long v = vtop->c.i;
      int bits = yc_int_bits(&s, &us);
      int fits;
      if (bits < 64)
        v = us ? (long long)((unsigned long long)v & ((1ull << bits) - 1)) : v;
      fits = us ? (unsigned long long)v <= hi : v >= lo && (v < 0 || (unsigned long long)v <= hi);
      if (!fits)
      {
        if (us)
          tcc_error_noabort("cast<int(%s)>: %llu is not a '%s'", tn, (unsigned long long)v, tn);
        else
          tcc_error_noabort("cast<int(%s)>: %lld is not a '%s'", tn, v, tn);
      }
    }
    else if (checks)
      yasc_cast_check_int(lo, hi);
  }
  else if (perm == YC_INT && yc_is_flt(&s))
  {
    long double flo = (long double)lo - 1, fhi = (long double)hi + 1;
    int strict = 1;
    /* lo - 1 not a value of the source type: no value lies between it and
       lo either, so v >= lo is the same test */
    if (yc_bt(&s) == VT_FLOAT ? (long double)(float)flo != flo : (long double)(double)flo != flo)
      flo = lo, strict = 0;
    if (yc_const_operand())
    {
      long double v = yc_bt(&s) == VT_FLOAT ? vtop->c.f : yc_bt(&s) == VT_DOUBLE ? vtop->c.d : vtop->c.ld;
      if (!(strict ? v > flo : v >= flo) || !(v < fhi))
        tcc_error_noabort("cast<int(%s)>: the value is not a '%s'", tn, tn);
    }
    else if (checks)
      yasc_cast_check_float(flo, strict, fhi);
  }
  else if (perm == YC_ENUM)
  {
    Sym *e;
    int n = 0;
    long long *vals;
    for (e = dt->ref ? dt->ref->next : NULL; e; e = e->next)
      n++;
    if (!n)
      return;
    vals = tcc_malloc(n * sizeof *vals);
    n = 0;
    for (e = dt->ref->next; e; e = e->next)
      vals[n++] = sym_enum_val(e);
    if (yc_const_operand())
    {
      int found = 0;
      for (int i = 0; i < n && !found; i++)
        found = vals[i] == vtop->c.i;
      if (!found)
      {
        type_to_str(sn, sizeof sn, dt, NULL);
        tcc_error_noabort("cast<enum(%s)>: %lld is not one of its enumerators", sn, (long long)vtop->c.i);
      }
    }
    else if (checks)
      yasc_cast_check_enum(vals, n);
    tcc_free(vals);
  }
}

/* bits(T): the bytes of the operand read as a T, through memory (a union's
   member, as far as the alias analysis knows) */
static void yc_bit_cast(CType *dt)
{
  CType s = vtop->type;
  int as, at, ss = type_size(&s, &as), ts = type_size(dt, &at), al = as > at ? as : at;
  SValue slot;
  if (yc_bt(&s) == VT_STRUCT && (vtop->r & VT_LVAL))
  {
    vtop->type = *dt;
    vtop->union_member = 1;
    return;
  }
  loc = tcc_ir_frame_alloc(loc, ss > ts ? ss : ts, -al);
  svalue_init(&slot);
  slot.type = s;
  slot.type.t &= ~VT_STORAGE;
  slot.r = VT_LOCAL | VT_LVAL;
  slot.c.i = loc;
  slot.union_member = 1;
  vpushv(&slot);
  vswap();
  fe_conv_quiet++;
  vstore();
  fe_conv_quiet--;
  vpop();
  slot.type = *dt;
  vpushv(&slot);
}

static void yc_apply(int is_unsafe, int perms, int typed, CType *type, int align_n, int align_bare)
{
  CType st = vtop->type, dt, s;
  YcNeed need;
  char sn[160], tn[160];
  const int value_perms = perms & YC_VALUE;

  if (value_perms && (perms & YC_POINTER))
  {
    tcc_error_noabort("cast<>: a value permission (%s) and a pointer one do not mix: write two casts",
                      yc_name(value_perms));
    return;
  }
  s = yc_decayed(&st);
  dt = typed ? *type : s;
  dt.t &= ~VT_STORAGE;
  yc_inherit_yasc(&dt, &s);
  type_to_str(sn, sizeof sn, &s, NULL);
  type_to_str(tn, sizeof tn, &dt, NULL);

  if (align_bare)
  {
    if (!(perms & YC_PTR) || !yc_is_ptr(&dt) || type_size(pointed_type(&dt), &align_n) < 0)
    {
      tcc_error_noabort("cast<>: align without a number needs ptr(T *) to take it from: write align(N)");
      align_n = 1;
    }
  }
  if ((perms & YC_ALIGN) && (align_n <= 0 || (align_n & (align_n - 1))))
  {
    tcc_error_noabort("cast<>: align(%d) is not a power of two", align_n);
    return;
  }

  /* align(N) alone, or the align the cast asks for beyond what it needs */
  yc_need(&st, &dt, &need);
  if (need.bad)
  {
    tcc_error_noabort("cast<>: '%s' to '%s': %s", sn, tn, need.bad);
    return;
  }
  if (need.perms & YC_ALIGN && (perms & YC_ALIGN) && align_n < need.to_al)
    tcc_error_noabort("cast<>: '%s' needs %d-byte alignment, align(%d) is less", tn, need.to_al, align_n);
  {
    int missing = need.perms & ~YC_AS & ~perms;
    /* const: drops const and nothing else (volatile, a nested const added
       are qual's) */
    if ((perms & YC_CONST) && (perms & YC_QUAL))
      tcc_error_noabort("cast<>: const and qual: write one (const for a const object, qual for the rest)");
    else if (perms & YC_CONST)
    {
      if (!(need.perms & YC_QUAL) || !(need.dropped & VT_CONSTANT))
        tcc_error_noabort("cast<>: const is not needed: no const is dropped");
      else if (need.dropped == VT_CONSTANT)
        missing &= ~YC_QUAL;
    }
    /* a value permission is the conversion itself: int stands for as, and
       for trunc (the low bits of what fits are the value) */
    if (value_perms && need.perms & YC_VALUE)
    {
      int ok = value_perms == need.perms || value_perms == YC_BITS ||
               (need.perms == YC_AS && value_perms != YC_ENUM) || (need.perms == YC_INT && value_perms == YC_TRUNC &&
                                                                   yc_is_int(&s)) ||
               (need.perms == YC_FLOAT && value_perms == YC_FLOAT);
      if (value_perms == YC_BITS)
      {
        int a, b;
        if (yc_is_ptr(&s) || yc_is_ptr(&dt) || type_size(&s, &a) != type_size(&dt, &b) || type_size(&s, &a) <= 0)
        {
          tcc_error_noabort("cast<bits(%s)>: '%s' is not the same size%s", tn, sn,
                            yc_is_ptr(&s) || yc_is_ptr(&dt) ? " (a pointer: addr or ptr)" : "");
          return;
        }
      }
      else if (value_perms == YC_TRUNC && !(yc_is_int(&s) && yc_is_int(&dt) && yc_bt(&dt) != VT_BOOL))
      {
        tcc_error_noabort("cast<trunc(%s)>: trunc is integer to integer%s", tn,
                          yc_is_flt(&s) ? ": int(T) for a float" : "");
        return;
      }
      else if (value_perms == YC_FLOAT && !yc_is_flt(&dt))
      {
        tcc_error_noabort("cast<float(%s)>: float makes a floating type", tn);
        return;
      }
      else if (value_perms == YC_INT && (!yc_is_int(&dt) || IS_ENUM(dt.t)))
      {
        tcc_error_noabort("cast<int(%s)>: int makes an integer type", tn);
        return;
      }
      else if (!ok)
      {
        char spell[240];
        yc_spell(spell, sizeof spell, &dt, &need);
        tcc_error_noabort("cast<%s(%s)>: '%s' to '%s' needs %s", yc_name(value_perms), tn, sn, tn, spell);
        return;
      }
      missing = 0;
    }
    else if (value_perms && !(need.perms & YC_VALUE))
    {
      char spell[240];
      yc_spell(spell, sizeof spell, &dt, &need);
      tcc_error_noabort("cast<%s(%s)>: '%s' to '%s' is a pointer conversion: %s", yc_name(value_perms), tn, sn, tn,
                        spell);
      return;
    }
    else if (!value_perms && (need.perms & YC_VALUE) && need.perms != YC_AS)
    {
      char spell[240];
      yc_spell(spell, sizeof spell, &dt, &need);
      tcc_error_noabort("cast<>: '%s' to '%s' is a value conversion: %s", sn, tn, spell);
      return;
    }
    if (missing & YC_PTR)
      tcc_error_noabort("cast<>: the pointee changes ('%s' to '%s'): this needs ptr", sn, tn);
    else if (missing & YC_FN)
      tcc_error_noabort("cast<>: a function pointer changes ('%s' to '%s'): this needs unsafe_cast<fn(...)>", sn, tn);
    else if (missing & YC_ADDR)
      tcc_error_noabort("cast<>: '%s' to '%s' is an address: this needs addr", sn, tn);
    if ((missing & YC_ALIGN) && !(perms & YC_ALIGN))
      tcc_error_noabort("cast<>: the alignment rises from %d to %d: add align, or declare the object _Alignas(%d)",
                        need.from_al, need.to_al, need.to_al);
    if (missing & YC_QUAL)
    {
      if (need.dropped)
        tcc_error_noabort("cast<>: '%s' to '%s' drops '%s': this needs qual (unsafe_cast)%s", sn, tn,
                          (need.dropped & VT_CONSTANT) && (need.dropped & VT_VOLATILE) ? "const volatile"
                          : (need.dropped & VT_CONSTANT)                              ? "const"
                                                                                      : "volatile",
                          (perms & YC_CONST) ? "; const drops only const" : "");
      else
        tcc_error_noabort("cast<>: '%s' to '%s' adds const below a level that is not const: this needs qual "
                          "(unsafe_cast)",
                          sn, tn);
    }
    /* a modifier that allows nothing */
    if ((perms & YC_QUAL) && !(need.perms & YC_QUAL))
      tcc_error_noabort("cast<>: qual is not needed: no qualifier is dropped");
    if ((perms & YC_ALIGN) && yc_is_ptr(&s) && !yc_is_voidptr(&s) && !(need.perms & YC_ALIGN))
    {
      int k = yc_known_align(&st, &s);
      if (k >= align_n)
        tcc_error_noabort("cast<>: align is not needed: the operand is already %d-aligned", k);
    }
    if ((perms & YC_ALIGN) && !yc_is_ptr(&s))
      tcc_error_noabort("cast<>: align takes a pointer");
    if ((perms & YC_ADDR) && !(need.perms & YC_ADDR))
      tcc_error_noabort("cast<>: addr is pointer to integer or integer to pointer: '%s' to '%s' is neither", sn, tn);
    if ((perms & YC_FN) && !(need.perms & YC_FN) && !(yc_is_fnptr(&s) || yc_is_fnptr(&dt)))
      tcc_error_noabort("cast<>: fn takes a function pointer");
  }
  /* qual: what the operand is */
  if (perms & YC_QUAL)
  {
    int oq = yc_object_quals(&st);
    if ((oq & VT_CONSTANT) && (need.dropped & VT_CONSTANT))
      tcc_error_noabort("cast<>: '%s' points at a const object: qual does not make it writable (const says the "
                        "object stays const: unsafe_cast<..., const>)",
                        sn);
    if ((oq & VT_VOLATILE) && (need.dropped & VT_VOLATILE))
      tcc_error_noabort("cast<>: '%s' points at a volatile object: it stays volatile", sn);
  }
  /* unsafe_cast exactly for the unsafe permissions */
  {
    int unsafe_used = (perms & (YC_QUAL | YC_CONST | YC_FN)) || ((perms & YC_ADDR) && yc_is_ptr(&dt));
    if (unsafe_used && !is_unsafe)
      tcc_error_noabort("cast<>: %s is unsafe: write unsafe_cast",
                        (perms & YC_QUAL)    ? "qual"
                        : (perms & YC_CONST) ? "const"
                        : (perms & YC_FN)    ? "fn"
                                             : "an integer made a pointer (addr)");
    else if (!unsafe_used && is_unsafe)
      tcc_error_noabort("unsafe_cast<>: nothing unsafe here: write cast");
  }
  /* the yasc qualifiers: a cast that spells different ones is checked as
     an explicit cast was (unchecked to view, dropping maybe, ...) */
  if (yc_is_ptr(&dt) && yc_is_ptr(&s) && tcc_state->yasc && tcc_warn_wanted(warn_unchecked))
    yc_yasc_quals(&s, &dt);

  /* the conversion, with its checks */
  if (value_perms == YC_BITS)
  {
    yc_bit_cast(&dt);
    return;
  }
  if (value_perms == YC_INT || value_perms == YC_ENUM)
  {
    if (vtop->r & VT_LVAL)
      gv(RC_TYPE(vtop->type.t));
    yc_value_checks(value_perms, &dt);
  }
  if ((perms & YC_ALIGN) && yc_is_ptr(&s))
  {
    int known = yc_is_voidptr(&s) ? 1 : yc_known_align(&st, &s);
    gen_cast(&s);
    if (known < align_n && !tcc_state->cast_checks_off)
      yasc_cast_check_align(align_n);
  }
  gen_cast(&dt);
  vtop->packed_addr = 0; /* &packed->m: the cast said what it needs (align) */
}

/* cast<...>(x) / unsafe_cast<...>(x), tok the keyword: 0 when it is a name
   (one in scope, or not followed by '<') */
int yasc_cast_expr(void)
{
  int kw = tok, perms = 0, typed = 0, align_n = 0, align_bare = 0;
  CType type;

  if (sym_find(kw))
    return 0;
  next();
  if (tok != TOK_LT)
  {
    unget_tok(kw);
    return 0;
  }
  next();
  type.t = VT_INT;
  type.ref = NULL;
  for (;;)
  {
    int bit = yc_bit_of(tok);
    if (!bit)
      tcc_error("'%s' is not a cast permission: as, int, trunc, bits, float, enum, ptr, align, addr, qual, const "
                "or fn",
                get_tok_str(tok, NULL));
    if (perms & bit)
      tcc_error("cast<>: '%s' twice", yc_name(bit));
    next();
    if (bit == YC_ALIGN)
    {
      if (tok == '(')
      {
        CType at;
        AttributeDef ad;
        next();
        if (parse_btype(&at, &ad, 0))
        {
          int n;
          btype_no_auto(&ad);
          type_decl(&at, &ad, &n, TYPE_ABSTRACT);
          if (type_size(&at, &align_n) < 0)
            tcc_error("cast<>: align(T) of an incomplete type");
        }
        else
          align_n = expr_const();
        skip(')');
      }
      else
        align_bare = 1;
    }
    else if (tok == '(')
    {
      if (typed)
        tcc_error("cast<>: one permission carries the type ('%s' and '%s' both do)", yc_name(typed), yc_name(bit));
      next();
      {
        const int saved = yasc_cast_type_plain;
        int top_plain;
        yasc_cast_type_plain = 0;
        parse_type(&type);
        top_plain = yc_is_ptr(&type) && !yc_is_fnptr(&type) && type.ref &&
                    !(type_yasc(&type) & (YASC_OWNED | YASC_VIEW | YASC_UNCHECKED));
        if (yasc_cast_type_plain > top_plain)
          tcc_error_noabort("plain pointer in yasc: say owned or view (only the outermost '*' of a cast's type may "
                            "say nothing: it takes the operand's)");
        yasc_cast_type_plain = saved;
      }
      skip(')');
      typed = bit;
    }
    else if (bit != YC_QUAL && bit != YC_CONST)
      tcc_error("cast<>: '%s' needs its type: %s(T)", yc_name(bit), yc_name(bit));
    perms |= bit;
    if (tok == TOK_GT)
      break;
    if (tok != ',')
      expect("',' or '>'");
    next();
  }
  next();
  if ((perms == YC_QUAL || perms == YC_CONST) && !typed)
    tcc_error("cast<>: %s alone needs its type: %s(T *)", yc_name(perms), yc_name(perms));
  skip('(');
  expr_eq();
  skip(')');
  yc_apply(kw == TOK_unsafe_cast, perms, typed != 0, &type, align_n, align_bare);
  return 1;
}
