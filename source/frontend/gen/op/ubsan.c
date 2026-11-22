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

/* ubsan.c -- -fsanitize=undefined (trap mode) and -ftrapv.
 *
 * A minimal UBSan with no runtime: before (or around) an operation whose
 * result would be undefined, the frontend emits the test as ordinary IR and
 * a TRAP (`udf #254`, the same as __builtin_trap) on the failing side.  The
 * checks are plain code, so every -O level keeps them -- a pass may drop one
 * only by proving its condition false -- and the rest of the expression is
 * optimised as ever.  ptrace reports the UDF as a stop (docs/debugging.md in
 * yasos); without a debugger the process dies of the fault.
 *
 * The arithmetic of a checked operation is done in the unsigned type of its
 * width, where wrapping is defined, so no fold that assumes "signed overflow
 * cannot happen" can see through to the test.
 *
 * Off (tcc_state->sanitize == 0) nothing here runs: the callers test the
 * mask first, and objects are those of a compiler without this file.
 *
 * Not checked: operations folded at compile time (both operands constant:
 * -Woverflow / -Wdiv-by-zero / -Wshift-count-* report those), pointer
 * arithmetic, the implementation-defined conversions (a too-big value into a
 * narrower signed type), floating point.
 *
 * Handler mode (-fno-sanitize-trap, -fsanitize-recover, -fno-sanitize-recover
 * for the checks they name, unless -fsanitize-trap names them too): the
 * failing side calls the runtime's __ubsan_handle_<check> (libc's ubsan.c)
 * with clang's ABI instead of trapping -- a static record per check in .data
 * (the source location {file, line, column 0}, then its type descriptors
 * {kind, info, name} from .rodata, shared like string literals) and the
 * operands as ValueHandles (a 32-bit value itself, a long long through a
 * pointer to a copy).  The plain handlers return and the program goes on
 * with the wrapped result; the _abort ones (-fno-sanitize-recover) are
 * followed by the same TRAP, so that side never rejoins.  -ftrapv's checks
 * always trap. */

#include "gen_priv.h"

static int ubsan_is_const_sv(const SValue *sv)
{
  return (sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
}

static int ubsan_on(int bits)
{
  return (tcc_state->sanitize & bits) && !nocode_wanted && tcc_state->ir;
}

/* ---- handler mode ------------------------------------------------------ */

/* a check, for its handler: which one, its record's type descriptors and
   the operands it passes */
typedef struct UbsanSite
{
  const char *handler; /* __ubsan_handle_<this>[_abort] */
  int bit;             /* the SAN_* option the check belongs to */
  int nt, nargs;
  int mismatch; /* type_mismatch_v1: log2(alignment) + 1 */
  CType types[2];
  SValue args[2];
} UbsanSite;

static void ubsan_site(UbsanSite *u, const char *handler, int bit, int t)
{
  memset(u, 0, sizeof *u);
  u->handler = handler;
  u->bit = bit;
  if (t)
  {
    u->types[0].t = t;
    u->nt = 1;
  }
}

static void ubsan_site_type(UbsanSite *u, const CType *type)
{
  u->types[u->nt++] = *type;
}

static void ubsan_site_arg(UbsanSite *u, const SValue *sv)
{
  u->args[u->nargs++] = *sv;
}

static int ubsan_handler_on(int bit)
{
  return (tcc_state->sanitize_handler & ~tcc_state->sanitize_trap & bit) && !tcc_state->check_only;
}

/* LEN bytes in .rodata, shared with an identical string literal or
   descriptor (strlit_pool.c) */
static Sym *ubsan_rodata(const void *bytes, int len)
{
  addr_t pre = rodata_section->data_offset;
  size_t off = section_add(rodata_section, len, 4);
  Sym *sym;
  memcpy(rodata_section->data + off - rodata_section->data_base, bytes, len);
  vpushsym(&char_type, get_sym_ref(&char_type, rodata_section, off, len));
  str_lit_pool_merge(pre);
  sym = vtop->sym;
  vpop();
  return sym;
}

/* clang's TypeDescriptor: {u16 kind, u16 info, the name} -- kind 0 an
   integer (info = log2(bits) << 1 | signed), 1 a float (info = bits),
   0xffff anything else */
static Sym *ubsan_type_desc(CType *type)
{
  char name[128];
  unsigned char d[4 + sizeof name];
  int kind = 0xffff, info = 0, bt = type->t & VT_BTYPE, align, len;
  if (!(type->t & (VT_ARRAY | VT_VLA)) && is_integer_btype(bt) && bt != VT_PTR)
  {
    int bits = 8 * type_size(type, &align), lg = 0;
    while ((1 << lg) < bits)
      lg++;
    kind = 0;
    info = lg << 1 | !(type->t & VT_UNSIGNED);
  }
  else if (!(type->t & (VT_ARRAY | VT_VLA)) && is_float(type->t))
  {
    kind = 1;
    info = 8 * type_size(type, &align);
  }
  {
    CType named = *type; /* the type, not the storage class */
    named.t &= ~VT_STORAGE;
    type_to_str(name, sizeof name, &named, NULL);
  }
  len = strlen(name) + 1;
  d[0] = kind & 0xff;
  d[1] = kind >> 8;
  d[2] = info & 0xff;
  d[3] = info >> 8;
  memcpy(d + 4, name, len);
  return ubsan_rodata(d, 4 + len);
}

/* the check's record in .data (written by the handler: its column marks a
   place already reported): {file, line, column 0}, the type descriptors,
   type_mismatch_v1's {log2(alignment), 0 = load} */
static Sym *ubsan_record(UbsanSite *u)
{
  const char *fname = file ? file->filename : "<unknown>";
  Sym *fsym = ubsan_rodata(fname, strlen(fname) + 1), *ts[2];
  int size = 12 + 4 * u->nt + (u->mismatch ? 4 : 0), i;
  size_t off;
  unsigned char *p;
  for (i = 0; i < u->nt; i++)
    ts[i] = ubsan_type_desc(&u->types[i]);
  off = section_add(data_section, size, 4);
  p = data_section->data + off - data_section->data_base;
  memset(p, 0, size);
  write32le(p + 4, file ? file->line_num : 0);
  if (u->mismatch)
    p[12 + 4 * u->nt] = u->mismatch - 1;
  greloc(data_section, fsym, off, R_DATA_PTR);
  for (i = 0; i < u->nt; i++)
    greloc(data_section, ts[i], off + 12 + 4 * i, R_DATA_PTR);
  return get_sym_ref(&char_type, data_section, off, size);
}

/* an operand pushed as a ValueHandle: a 32-bit value in a register, a long
   long's copy in a frame slot of its own and its address */
static void ubsan_push_handle(const SValue *sv)
{
  vpushv((SValue *)sv);
  if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
  {
    TCCIRState *ir = tcc_state->ir;
    int vr = tcc_ir_get_vreg_var(ir);
    SValue lv;
    loc = tcc_ir_frame_alloc(loc, 8, -8);
    if (vr >= 0)
    {
      tcc_ir_assign_physical_register(ir, vr, loc, -1, -1);
      tcc_ir_set_original_offset(ir, vr, loc);
      tcc_ir_set_llong_type(ir, vr);
      tcc_ir_set_addrtaken(ir, vr);
    }
    svalue_init(&lv);
    lv.type.t = vtop->type.t;
    lv.r = VT_LOCAL | VT_LVAL;
    lv.vr = vr;
    lv.c.i = loc;
    vpushv(&lv);
    vswap();
    vstore();
    vpop();
    lv.r = VT_LOCAL;
    lv.type = char_pointer_type;
    vpushv(&lv);
  }
  if (!ubsan_is_const_sv(vtop))
    gv(RC_INT);
}

/* the failing side in handler mode: __ubsan_handle_<check>(&record, the
   operands), and after an _abort handler the TRAP */
static void ubsan_call(UbsanSite *u)
{
  const int abort_ = (tcc_state->sanitize_abort & u->bit) != 0;
  char name[64];
  SValue args[3];
  int i;
  snprintf(name, sizeof name, "__ubsan_handle_%s%s", u->handler, abort_ ? "_abort" : "");
  svalue_init(&args[0]);
  args[0].type = char_pointer_type;
  args[0].r = VT_CONST | VT_SYM;
  args[0].sym = ubsan_record(u);
  args[0].c.i = 0;
  for (i = 0; i < u->nargs; i++)
    ubsan_push_handle(&u->args[i]);
  for (i = 0; i < u->nargs; i++)
    args[1 + i] = vtop[i + 1 - u->nargs];
  gen_ir_void_call_args(args, u->nargs + 1, tok_alloc_const(name));
  for (i = 0; i < u->nargs; i++)
    vpop();
  if (abort_)
    tcc_ir_put(tcc_state->ir, TCCIR_OP_TRAP, NULL, NULL, NULL);
}

/* vtop: a condition (non-zero: undefined behaviour) -> popped, and on its
 * true side a TRAP, or the handler's call for the check U. */
static void ubsan_trap_if(UbsanSite *u)
{
  TCCIRState *ir = tcc_state->ir;
  if (u && !ubsan_handler_on(u->bit))
    u = NULL;
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    /* known at compile time: no test; a certain fault is a plain TRAP (the
       code after it is still generated, as after __builtin_trap) */
    int always = vtop->c.i != 0;
    vpop();
    if (always && u)
      ubsan_call(u);
    else if (always)
      tcc_ir_put(ir, TCCIR_OP_TRAP, NULL, NULL, NULL);
    return;
  }
  const int saved_nocode = nocode_wanted;
  int skip = tcc_ir_codegen_test_gen(ir, 1, -1);
  if (u)
    ubsan_call(u);
  else
    tcc_ir_put(ir, TCCIR_OP_TRAP, NULL, NULL, NULL);
  tcc_ir_backpatch_to_here(ir, skip);
  nocode_wanted = saved_nocode;
}

static void ubsan_cast(int t)
{
  CType ct;
  ct.t = t;
  ct.ref = NULL;
  gen_cast(&ct);
}

/* an integer operand: its type after the integer promotions (VT_INT or
   VT_LLONG, VT_UNSIGNED or not), or 0 for anything else */
static int ubsan_promoted(const CType *type)
{
  int t = type->t, bt = t & VT_BTYPE;
  if (!is_integer_btype(bt) || (t & (VT_COMPLEX | VT_VECTOR)) || bt == VT_PTR)
    return 0;
  if (bt == VT_LLONG && (!(t & VT_BITFIELD) || BIT_SIZE(t) > 32))
    return VT_LLONG | (t & VT_UNSIGNED);
  if (t & VT_BITFIELD)
    return BIT_SIZE(t) < 32 || !(t & VT_UNSIGNED) ? VT_INT : VT_INT | VT_UNSIGNED;
  if (bt == VT_INT)
    return VT_INT | (t & VT_UNSIGNED);
  return VT_INT; /* char, short, _Bool: everything fits int */
}

/* the usual arithmetic conversions of two promoted integer types */
static int ubsan_common(int a, int b)
{
  if ((a & VT_BTYPE) == VT_LLONG || (b & VT_BTYPE) == VT_LLONG)
  {
    /* a signed long long holds every unsigned int */
    if (((a & VT_BTYPE) == VT_LLONG && (a & VT_UNSIGNED)) || ((b & VT_BTYPE) == VT_LLONG && (b & VT_UNSIGNED)))
      return VT_LLONG | VT_UNSIGNED;
    return VT_LLONG;
  }
  return VT_INT | ((a | b) & VT_UNSIGNED);
}

/* vtop, of type t: made safe to read more than once.  A 32-bit value goes
   to a register, whose copies on the value stack all read the same vreg.  A
   64-bit one goes to an anonymous local of its own (declared as a C local
   is, so SSA promotes it at -O1+), re-read at every use: copies of a lazily
   extended register pair reference halves defined for only one of them
   (builtin/overflow.c), and a reused temporary slot written from its own
   old contents (`slot = slot >> 2`) did not survive -O2.  (Every value in a
   local was tried too: it exposed docs/bugs/union-cast-struct-escaped-
   address-drops-copy.md and -licm-load-across-builtin-apply.md.) */
static void ubsan_fix(int t)
{
  int vr;
  SValue v;
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    return;
  if ((t & VT_BTYPE) != VT_LLONG || (vr = tcc_ir_get_vreg_var(tcc_state->ir)) < 0)
  {
    gv(RC_TYPE(t));
    return;
  }
  loc = tcc_ir_frame_alloc(loc, 8, -8);
  tcc_ir_assign_physical_register(tcc_state->ir, vr, loc, -1, -1);
  tcc_ir_set_original_offset(tcc_state->ir, vr, loc);
  tcc_ir_set_llong_type(tcc_state->ir, vr);
  /* an ASSIGN that tcc_ir_put may not fold into the instruction defining
     the value: a copy of that TEMP elsewhere on the stack (inc()'s gv_dup)
     would then name a vreg nothing defines */
  {
    SValue src, dst;
    TCCIRState *ir = tcc_state->ir;
    uint8_t saved_prevent = ir->prevent_coalescing;
    gv(RC_TYPE(t));
    svalue_init(&src);
    svalue_init(&dst);
    src.vr = vtop->vr;
    src.type.t = t;
    dst.vr = vr;
    dst.type.t = t;
    ir->prevent_coalescing = 1;
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dst);
    ir->prevent_coalescing = saved_prevent;
    vpop();
  }
  svalue_init(&v);
  v.type.t = t;
  v.type.ref = NULL;
  v.r = VT_LOCAL | VT_LVAL;
  v.vr = vr;
  v.c.i = loc;
  vpushv(&v);
}

/* vtop[-1], vtop: converted to type t and each made safe to copy */
static void ubsan_operands(int t)
{
  vswap();
  ubsan_cast(t);
  ubsan_fix(t);
  vswap();
  ubsan_cast(t);
  ubsan_fix(t);
}

/* stack: a b r -> r, an rvalue */
static void ubsan_result(int t)
{
  vrott(3);
  vpop();
  vpop();
  ubsan_cast(t);
  if (vtop->r & VT_LVAL)
    gv(RC_TYPE(t));
}

/* a comparison whose result is combined with others: forced into a
   register, or the flags it left would be clobbered by the next one */
static void ubsan_cmp(int op)
{
  gen_op(op);
  gv(RC_INT);
}

static int ubsan_is_const(const SValue *sv)
{
  return (sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
}

static void ubsan_push_min(int t)
{
  if ((t & VT_BTYPE) == VT_LLONG)
    vpush64(VT_LLONG, (unsigned long long)1 << 63);
  else
    vpushi(INT32_MIN);
}

/* 1 while unary minus's `0 - x` is generated (ubsan_gen_neg) */
static int ubsan_negating;

/* signed a op b, op one of + - *, both of signed type t on the stack:
   replaced by the result, with the overflow test before it is used */
static void ubsan_signed_arith(int op, int t)
{
  const int ut = t | VT_UNSIGNED, wide = (t & VT_BTYPE) == VT_LLONG;
  UbsanSite site;
  ubsan_operands(t);
  if (ubsan_negating)
  {
    ubsan_site(&site, "negate_overflow", SAN_SIGNED_OVERFLOW, t);
    ubsan_site_arg(&site, &vtop[0]);
  }
  else
  {
    ubsan_site(&site, op == '+' ? "add_overflow" : op == '-' ? "sub_overflow" : "mul_overflow", SAN_SIGNED_OVERFLOW,
               t);
    ubsan_site_arg(&site, &vtop[-1]);
    ubsan_site_arg(&site, &vtop[0]);
  }
  /* -ftrapv's own checks always trap */
  if (!ubsan_on(SAN_SIGNED_OVERFLOW))
    site.bit = 0;

  if (op == '*' && !wide)
  {
    /* the exact product in 64 bits: an overflow is a high word that is not
       the sign of the low one */
    vswap();
    ubsan_cast(VT_LLONG);
    vswap();
    ubsan_cast(VT_LLONG);
    gen_op('*');
    ubsan_fix(VT_LLONG);
    vdup();
    vdup();
    ubsan_cast(VT_INT);
    ubsan_cast(VT_LLONG);
    gen_op(TOK_NE);
    ubsan_trap_if(&site);
    ubsan_cast(VT_INT);
    if (vtop->r & VT_LVAL)
      gv(RC_INT);
    return;
  }

  if (op == '*')
  {
    /* 64 x 64: r = a * b wraps; it overflowed when a != 0 and r / a != b,
       or for -1 * LLONG_MIN, whose r / a itself overflows.  The quotient is
       taken with a divisor that is never 0 or -1:
         d = a + (a == 0) + 2 * (a == -1)          (0 and -1 become 1)
         ovf = ((a != 0) & (a != -1) & (r / d != b))
             | ((a == -1) & (b == MIN)) | ((b == -1) & (a == MIN))  */
    SValue a = vtop[-1], b = vtop[0];
    vpushv(&a);
    ubsan_cast(ut);
    vpushv(&b);
    ubsan_cast(ut);
    gen_op('*');
    ubsan_cast(t);
    ubsan_fix(t); /* stack: a b r */
    SValue r = *vtop;

    vpushv(&a);
    vpushi(0);
    ubsan_cast(t);
    ubsan_cmp(TOK_EQ);
    vpushv(&a);
    vpushll(-1);
    ubsan_cmp(TOK_EQ);
    vpushi(2);
    gen_op('*');
    gen_op('+');
    ubsan_cast(t);
    vpushv(&a);
    gen_op('+'); /* d */
    vpushv(&r);
    vswap();
    gen_op('/');
    vpushv(&b);
    ubsan_cmp(TOK_NE);
    vpushv(&a);
    vpushll(0);
    ubsan_cmp(TOK_NE);
    gen_op('&');
    vpushv(&a);
    vpushll(-1);
    ubsan_cmp(TOK_NE);
    gen_op('&');

    vpushv(&a);
    vpushll(-1);
    ubsan_cmp(TOK_EQ);
    vpushv(&b);
    ubsan_push_min(t);
    ubsan_cmp(TOK_EQ);
    gen_op('&');
    gen_op('|');

    vpushv(&b);
    vpushll(-1);
    ubsan_cmp(TOK_EQ);
    vpushv(&a);
    ubsan_push_min(t);
    ubsan_cmp(TOK_EQ);
    gen_op('&');
    gen_op('|');
    ubsan_trap_if(&site); /* stack: a b r */
    ubsan_result(t);
    return;
  }

  /* + and -: in the unsigned type, where they wrap */
  SValue a = vtop[-1], b = vtop[0];
  vpushv(&a);
  ubsan_cast(ut);
  vpushv(&b);
  ubsan_cast(ut);
  gen_op(op);
  ubsan_fix(ut);
  SValue r = *vtop; /* stack: a b r */

  if (ubsan_is_const(&b) || (op == '+' && ubsan_is_const(&a)))
  {
    /* x + c, x - c, c + x: one comparison of the result with x */
    int b_const = ubsan_is_const(&b);
    SValue *x = b_const ? &a : &b;
    int64_t c = (b_const ? b : a).c.i;
    if (!wide)
      c = (int32_t)c;
    if (c == 0)
    {
      ubsan_result(t);
      return;
    }
    /* + c > 0 and - c < 0 overflow upwards: the result is below x */
    int up = (op == '+') == (c > 0);
    vpushv(&r);
    ubsan_cast(t);
    vpushv(x);
    ubsan_cast(t);
    gen_op(up ? TOK_LT : TOK_GT);
  }
  else
  {
    /* +: (a ^ r) & (b ^ r) has the sign bit set; -: (a ^ b) & (a ^ r) */
    vpushv(&a);
    ubsan_cast(ut);
    vpushv(op == '+' ? &r : &b);
    ubsan_cast(ut);
    gen_op('^');
    vpushv(op == '+' ? &b : &a);
    ubsan_cast(ut);
    vpushv(&r);
    gen_op('^');
    gen_op('&');
    ubsan_cast(t);
    vpushi(0);
    ubsan_cast(t);
    gen_op(TOK_LT);
  }
  ubsan_trap_if(&site);
  ubsan_result(t);
}

/* a / b, a % b of integer type t, both on the stack: the tests, then the
   operation itself */
static void ubsan_division(int op, int t)
{
  ubsan_operands(t);
  SValue a = vtop[-1], b = vtop[0];
  UbsanSite site;
  ubsan_site(&site, "divrem_overflow", SAN_DIV_ZERO, t);
  ubsan_site_arg(&site, &a);
  ubsan_site_arg(&site, &b);
  if (ubsan_on(SAN_DIV_ZERO))
  {
    vpushv(&b);
    vpushi(0);
    ubsan_cast(t);
    gen_op(TOK_EQ);
    ubsan_trap_if(&site);
  }
  if (!(t & VT_UNSIGNED) && ubsan_on(SAN_SIGNED_OVERFLOW))
  {
    /* MIN / -1 (and MIN % -1, whose quotient is the same) */
    vpushv(&a);
    ubsan_push_min(t);
    ubsan_cmp(TOK_EQ);
    vpushv(&b);
    vpushll(-1);
    ubsan_cast(t);
    ubsan_cmp(TOK_EQ);
    gen_op('&');
    site.bit = SAN_SIGNED_OVERFLOW;
    ubsan_trap_if(&site);
  }
  gen_op(op);
}

/* a << b, a >> b; the left operand of promoted type t */
static void ubsan_shift(int op, int t)
{
  const int w = (t & VT_BTYPE) == VT_LLONG ? 64 : 32;
  int bt = ubsan_promoted(&vtop->type);
  vswap();
  ubsan_cast(t);
  ubsan_fix(t);
  vswap();
  ubsan_cast(bt);
  ubsan_fix(bt);
  SValue a = vtop[-1], b = vtop[0];
  UbsanSite site;
  CType btype = {bt, NULL};
  ubsan_site(&site, "shift_out_of_bounds", SAN_SHIFT_EXP, t);
  ubsan_site_type(&site, &btype);
  ubsan_site_arg(&site, &a);
  ubsan_site_arg(&site, &b);
  if (ubsan_on(SAN_SHIFT_EXP))
  {
    /* negative or at least the width: one unsigned comparison */
    vpushv(&b);
    ubsan_cast(bt | VT_UNSIGNED);
    vpushi(w);
    ubsan_cast(bt | VT_UNSIGNED);
    gen_op(TOK_UGE);
    ubsan_trap_if(&site);
  }
  if (op == TOK_SHL && !(t & VT_UNSIGNED) && ubsan_on(SAN_SHIFT_BASE))
  {
    /* a negative a, or a bit shifted into or past the sign:
       (unsigned)a >> (w - 1 - b) is not 0 (b taken modulo w, the exponent
       test above has had its say) */
    vpushv(&a);
    ubsan_cast(t | VT_UNSIGNED);
    vpushi(w - 1);
    vpushv(&b);
    ubsan_cast(VT_INT);
    gen_op('-');
    vpushi(w - 1);
    gen_op('&');
    gen_op(TOK_SHR);
    vpushi(0);
    ubsan_cast(t | VT_UNSIGNED);
    gen_op(TOK_NE);
    site.bit = SAN_SHIFT_BASE;
    ubsan_trap_if(&site);
  }
  gen_op(op);
}

/* gen_op for an operator of the source, sanitized: returns 1 when it did the
 * operation (with its tests), 0 when the caller is to do it unchecked. */
int ubsan_gen_op(int op)
{
  int ta, tb, t;
  if (!ubsan_on(SAN_ARITH_ALL | SAN_TRAPV) || (ubsan_is_const(&vtop[-1]) && ubsan_is_const(vtop)))
    return 0;
  ta = ubsan_promoted(&vtop[-1].type);
  tb = ubsan_promoted(&vtop->type);
  if (!ta || !tb)
    return 0;
  if (op == TOK_SHL || op == TOK_SAR)
  {
    if (!ubsan_on(SAN_SHIFT_EXP) && !(op == TOK_SHL && !(ta & VT_UNSIGNED) && ubsan_on(SAN_SHIFT_BASE)))
      return 0;
    ubsan_shift(op, ta);
    return 1;
  }
  t = ubsan_common(ta, tb);
  if (op == '+' || op == '-' || op == '*')
  {
    if ((t & VT_UNSIGNED) || !ubsan_on(SAN_SIGNED_OVERFLOW | SAN_TRAPV))
      return 0;
    ubsan_signed_arith(op, t);
    return 1;
  }
  if (op == '/' || op == '%')
  {
    if (!ubsan_on(SAN_DIV_ZERO) && ((t & VT_UNSIGNED) || !ubsan_on(SAN_SIGNED_OVERFLOW)))
      return 0;
    ubsan_division(op, t);
    return 1;
  }
  return 0;
}

/* unary minus, `0 - x` on the stack: as ubsan_gen_op('-'), its overflow
   reported as a negation */
int ubsan_gen_neg(void)
{
  int r;
  ubsan_negating = 1;
  r = ubsan_gen_op('-');
  ubsan_negating = 0;
  return r;
}

/* indir(): vtop, a pointer value about to name the object it points to.
 * -fsanitize=null: not a null pointer; =alignment: aligned for that type.
 * A constant address, a symbol's and a frame address are known good (or
 * deliberate, `*(volatile int *)0`); only a computed pointer is tested. */
void ubsan_deref(void)
{
  CType *pt;
  int align, size;
  /* a value in a vreg only: not a VT_PARAM address (a by-value parameter's
     home, which only a LEA turns into a pointer) */
  if (!ubsan_on(SAN_NULL | SAN_ALIGN) || (vtop->type.t & VT_BTYPE) != VT_PTR ||
      (vtop->r & (VT_VALMASK | VT_LVAL | VT_PARAM)) != 0 || vtop->vr < 0 || vtop->c.i != 0)
    return;
  pt = pointed_type(&vtop->type);
  if ((pt->t & (VT_ARRAY | VT_VLA)) || (pt->t & VT_BTYPE) == VT_FUNC || (pt->t & VT_BTYPE) == VT_VOID)
    return;
  size = type_size(pt, &align);
  if (size < 0)
    return; /* incomplete: nothing is accessed through it */
  /* The pointer is read by each test and by the access itself straight from
     its vreg: parked in a local (ubsan_fix), a frame address hidden behind
     it let the passes drop the stores the access reads (a union cast's
     copy, bug_union_cast_member_type.c). */
  int under = vtop->underaligned, packed = vtop->packed_addr, lg = 0;
  SValue p = *vtop;
  UbsanSite site;
  /* at most 4: a word is what LDRD / LDM need on ARMv8-M, and what tcc's
     frames promise a local of an 8-aligned type (the AAPCS's 8 is a
     stack-pointer rule at calls) */
  if (align > 4)
    align = 4;
  while ((1 << lg) < align)
    lg++;
  ubsan_site(&site, "type_mismatch_v1", SAN_NULL, 0);
  ubsan_site_type(&site, pt);
  site.mismatch = lg + 1;
  /* the handle: the pointer as a 32-bit integer */
  vpushv(&p);
  ubsan_cast(VT_INT | VT_UNSIGNED);
  ubsan_site_arg(&site, vtop);
  vpop();
  if (ubsan_on(SAN_NULL))
  {
    vpushv(&p);
    ubsan_cast(VT_INT | VT_UNSIGNED);
    vpushi(0);
    gen_op(TOK_EQ);
    ubsan_trap_if(&site);
  }
  if (ubsan_on(SAN_ALIGN) && align > 1 && !under && !packed)
  {
    vpushv(&p);
    ubsan_cast(VT_INT | VT_UNSIGNED);
    vpushi(align - 1);
    gen_op('&');
    vpushi(0);
    gen_op(TOK_NE);
    site.bit = SAN_ALIGN;
    ubsan_trap_if(&site);
  }
}

/* base[index] (or index[base]) on the stack, before the addition:
 * -fsanitize=bounds for an array of known size.  ADDR_OK: the element is
 * only addressed (`&a[i]`), so one past the end is still in bounds. */
void ubsan_subscript(int addr_ok)
{
  SValue *base = vtop - 1, *ix = vtop;
  int n, it;
  if (!ubsan_on(SAN_BOUNDS))
    return;
  if (!(base->type.t & VT_ARRAY) && (ix->type.t & VT_ARRAY))
    base = vtop, ix = vtop - 1;
  if (!(base->type.t & VT_ARRAY) || (base->type.t & VT_VLA) || !base->type.ref || base->type.ref->c < 0 ||
      base->union_member)
    return;
  n = base->type.ref->c;
  /* a trailing [0] / [1] stands for a flexible array (as gcc's default) */
  if (n <= 1)
    return;
  it = ubsan_promoted(&ix->type);
  if (!it)
    return;
  UbsanSite site;
  ubsan_site(&site, "out_of_bounds", SAN_BOUNDS, 0);
  ubsan_site_type(&site, &base->type);
  site.types[1].t = it;
  site.nt = 2;
  if (ix == vtop)
  {
    ubsan_cast(it);
    ubsan_fix(it);
  }
  else
  {
    vswap();
    ubsan_cast(it);
    ubsan_fix(it);
    vswap();
    ix = vtop - 1;
  }
  ubsan_site_arg(&site, ix);
  vpushv(ix);
  ubsan_cast(it | VT_UNSIGNED);
  vpushi(n + (addr_ok != 0));
  ubsan_cast(it | VT_UNSIGNED);
  gen_op(TOK_UGE);
  ubsan_trap_if(&site);
}

/* ---- -fstack-protector: which functions get a canary ------------------
 * gcc's rules, decided while the body is parsed (so the same at every -O; an
 * inlined body's locals count for the function they are inlined into):
 *   -fstack-protector-all     every function;
 *   -fstack-protector-strong  a local array of any type (or a struct holding
 *                             one), a local whose address is taken, alloca or
 *                             a VLA;
 *   -fstack-protector         a char array of 8 bytes or more (also inside a
 *                             struct), alloca or a VLA.
 * The canary itself is the backend's (arm-thumb-frame.c, ir->ssp). */
static int ssp_char_buffer(const CType *type)
{
  if (type->t & VT_ARRAY)
  {
    CType *et = &type->ref->type;
    int bt = et->t & VT_BTYPE, align;
    if ((bt == VT_BYTE || bt == VT_BOOL) && !(et->t & VT_ARRAY))
      return type_size((CType *)type, &align) >= 8;
    return ssp_char_buffer(et);
  }
  if ((type->t & VT_BTYPE) == VT_STRUCT)
    for (Sym *f = type->ref->next; f; f = f->next)
      if (ssp_char_buffer(&f->type))
        return 1;
  return 0;
}

static int ssp_has_array(const CType *type)
{
  if (type->t & (VT_ARRAY | VT_VLA))
    return 1;
  if ((type->t & VT_BTYPE) == VT_STRUCT)
    for (Sym *f = type->ref->next; f; f = f->next)
      if (ssp_has_array(&f->type))
        return 1;
  return 0;
}

/* The choice a function's start makes: -fstack-protector-all protects it;
 * the attributes stack_protect (a canary under any -fstack-protector*, and
 * the only functions -explicit protects) and no_stack_protector (none, even
 * under -all) decide before the body does (gcc's rules). */
void ssp_begin_function(TCCIRState *ir, Sym *sym)
{
  int mode = tcc_state->stack_protector;
  ir->ssp = 0;
  ir->ssp_off = 0;
  if (!mode)
    return;
  if (sym_flag(sym, SYMF_NO_STACK_PROTECT))
    ir->ssp_off = 1;
  else if (mode == SSP_ALL || sym_flag(sym, SYMF_STACK_PROTECT))
    ir->ssp = 1;
  else if (mode == SSP_EXPLICIT)
    ir->ssp_off = 1;
}

/* a local of this type was given a frame slot */
void ssp_note_local(const CType *type)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir || ir->ssp || ir->ssp_off)
    return;
  if (type->t & VT_VLA)
    ir->ssp = 1;
  else if (tcc_state->stack_protector == SSP_STRONG)
    ir->ssp = ssp_has_array(type);
  else
    ir->ssp = ssp_char_buffer(type);
}

/* alloca, or a local's address taken (only -strong counts that) */
void ssp_note_frame_use(int alloca_)
{
  if (tcc_state->ir && !tcc_state->ir->ssp_off && (alloca_ || tcc_state->stack_protector == SSP_STRONG))
    tcc_state->ir->ssp = 1;
}

/* a VLA bound on the stack, just evaluated: -fsanitize=vla-bound traps on a
   value <= 0 */
void ubsan_vla_bound(void)
{
  int t;
  if (!(tcc_state->sanitize & SAN_VLA) || !tcc_state->ir || ubsan_is_const(vtop))
    return;
  t = ubsan_promoted(&vtop->type);
  if (!t)
    return;
  ubsan_cast(t);
  ubsan_fix(t);
  UbsanSite site;
  ubsan_site(&site, "vla_bound_not_positive", SAN_VLA, t);
  ubsan_site_arg(&site, vtop);
  vdup();
  vpushi(0);
  ubsan_cast(t);
  gen_op((t & VT_UNSIGNED) ? TOK_EQ : TOK_LE);
  ubsan_trap_if(&site);
}
