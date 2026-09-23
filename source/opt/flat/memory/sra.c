/*
 * TinyCC - Tiny C Compiler
 *
 * Scalar replacement of small frame objects.
 *
 * A struct local the frontend put in the frame stays there: every field read
 * is a load from the stack and every field write a store, and each call that
 * takes it by value copies it first.  The Zig C backend builds everything out
 * of such locals -- `Type`, `Value`, `InternPool.Index` are one-word structs
 * -- so most of the Zig compiler's stack traffic is to objects whose address
 * never leaves the function.
 *
 * An object qualifies when every reference to it is a LOAD or ASSIGN reading
 * an 8-, 16- or 32-bit field of it, a STORE writing one, or the whole of a
 * four-byte, float-free object passed to or returned from a call -- which the
 * AAPCS moves in a core register exactly like an int -- and the fields it is
 * accessed as do not overlap: each byte offset is always accessed with one
 * width, and a narrow one with one signedness.  A field may also be reached
 * through an address TEMP (`T <- &s; *(T + 4) = x`, the frontend's way to a
 * member) when that TEMP is used for nothing but such accesses.  Each field
 * then becomes a fresh VAR, which SSA renames like any other scalar (a narrow
 * one like a `short` local), and the address arithmetic goes.  Any other
 * reference -- an address that escapes, a 64-bit or overlapping access, a
 * block copy -- leaves the object in memory, untouched.
 *
 * The rule is kept narrow on purpose, and nothing is rewritten for an object
 * that stays in memory.  Accepting a field in any operand (an ALU op reading
 * it straight from the frame) promoted more objects but made the Zig compiler
 * larger, and so did folding `*(&obj + k)` into direct frame accesses for every
 * object: the extra promoted values mostly end up spilled, with the spill
 * slots nearest sp, and a direct access loses the short base-register form
 * and some later forwarding.
 *
 * Runs on the flat IR just before SSA construction.
 */

#include <stdint.h>

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"

#define SRA_MAX_BYTES 36 /* 32 bytes, plus the guard word frame_record adds */

typedef struct
{
  int lo, hi;  /* the object's extent, from tcc_ir_frame_object_at */
  int refs;    /* every operand a live instruction names it with */
  int ok_refs; /* those a field VAR can replace */
  int bad;     /* two accesses overlap, or one byte offset is read two ways */
  uint8_t width[SRA_MAX_BYTES]; /* per field start: bytes accessed, 0 = none */
  uint8_t sign[SRA_MAX_BYTES];  /* per narrow field: 1 signed, 2 unsigned */
  int32_t var[SRA_MAX_BYTES];
} SraUnit;

/* A frame slot the backend addresses directly, the same test frame.c uses. */
static int sra_concrete(IROperand op, int *off)
{
  if (irop_is_none(op) || irop_get_vreg(op) >= 0 || op.is_param)
    return 0;
  int tag = irop_get_tag(op);
  if (tag != IROP_TAG_STACKOFF && !(tag == IROP_TAG_VREG && (op.is_local || op.is_llocal)))
    return 0;
  *off = irop_get_stack_offset(op);
  return 1;
}

static int sra_type_has_float(CType *t, int depth)
{
  if (depth > 8)
    return 1;
  int bt = t->t & VT_BTYPE;
  if (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE)
    return 1;
  if ((t->t & VT_ARRAY) && t->ref)
    return sra_type_has_float(&t->ref->type, depth + 1);
  if (bt == VT_STRUCT && t->ref)
    for (Sym *f = t->ref->next; f; f = f->next)
      if (sra_type_has_float(&f->type, depth + 1))
        return 1;
  return 0;
}

/* A four-byte struct operand that travels in a core register like an int. */
static int sra_struct_is_word(IROperand op)
{
  if (irop_get_btype(op) != IROP_BTYPE_STRUCT || op.is_llocal || op.is_complex)
    return 0;
  CType *ct = irop_get_ctype(op);
  if (!ct)
    return 0;
  int align;
  return type_size(ct, &align) == 4 && align <= 4 && !sra_type_has_float(ct, 0);
}

static int sra_btype_bytes(int bt)
{
  return bt == IROP_BTYPE_INT32 ? 4 : bt == IROP_BTYPE_INT16 ? 2 : bt == IROP_BTYPE_INT8 ? 1 : 0;
}

/* Record a `bytes`-wide access at `off` of the unit; mark the unit bad when it
 * does not fit the fields seen so far. */
static void sra_note_field(SraUnit *u, int off, int bytes, int is_unsigned)
{
  int at = off - u->lo;
  if (at < 0 || at + bytes > SRA_MAX_BYTES || at + bytes > u->hi - u->lo || (at & (bytes - 1)))
  {
    u->bad = 1;
    return;
  }
  if (u->width[at] && u->width[at] != bytes)
  {
    u->bad = 1;
    return;
  }
  if (bytes < 4)
  {
    int sg = is_unsigned ? 2 : 1;
    if (u->sign[at] && u->sign[at] != sg)
      u->bad = 1;
    u->sign[at] = sg;
  }
  u->width[at] = bytes;
}

/* A narrow field's write has to be a DEFINITION of its VAR, and of the whole
 * of it.
 *
 * SSA only takes a full-width slot STORE as a definition of a variable (ssa.c,
 * ssa_store_slot_def_pos); a narrower one updates whatever name is current, in
 * place.  A field written only by narrow stores would therefore get a VAR that
 * is never defined at all, while its reads -- which rename turns into
 * whole-register copies -- take that undefined name (struct_byval fuzz seed
 * 1962: a `struct { unsigned char a; }` built and passed by value read back 3
 * bytes of whatever was under it at -O2).  The frontend's own narrow locals do
 * not have this problem: it writes those with ASSIGN.
 *
 * So a narrow field's STORE is rewritten to `ASSIGN var <- src`, which is one,
 * and every access to the field gets a 32-bit operand.  That drops the
 * truncation the store's width used to do -- the frontend does NOT truncate
 * ahead of a narrow store, `s.a = x` stores x whole and lets the width cut it
 * -- so the object is promoted only when every stored value ALREADY fits the
 * field.  There is nowhere to put a mask otherwise: a STORE's operand block
 * holds two operands and an AND needs three, and widening it in place would
 * write over the next instruction's.
 *
 * A SIGNED narrow field would need its reads sign-extended, which a plain
 * register copy does not do, so those keep their memory too. */
static int sra_narrow_fields_unsigned(const SraUnit *u)
{
  for (int at = 0; at < SRA_MAX_BYTES; at++)
    if (u->width[at] && u->width[at] < 4 && u->sign[at] != 2)
      return 0;
  return 1;
}

static int sra_temp_pos(IROperand o, int ntemp)
{
  int32_t vr = irop_get_vreg(o);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || TCCIR_DECODE_VREG_POSITION(vr) >= ntemp)
    return -1;
  return TCCIR_DECODE_VREG_POSITION(vr);
}

/* The mask a `bytes`-wide unsigned field holds. */
static int32_t sra_field_mask(int bytes)
{
  return bytes == 1 ? 0xff : bytes == 2 ? 0xffff : -1;
}

/* Is every bit of `op` outside `mask` already zero, so that storing it whole
 * is what the narrow store would have stored?  `op` must also be a plain
 * value: SSA only makes a slot store a definition for one of those
 * (ssa_store_slot_def_pos).  Only the forms the frontend puts ahead of a
 * narrow store are recognised -- a constant, a mask by a subset of it, and a
 * value that is itself a narrow unsigned one.  `def` maps a TEMP to its only
 * defining instruction, or -1 when it has none or several. */
static int sra_narrow_store_fits(TCCIRState *ir, IRQuadCompact *q, int bytes,
                                 const int32_t *def, int ntemp)
{
  const int32_t mask = sra_field_mask(bytes);
  IROperand op = tcc_ir_op_get_src1(ir, q);
  if (irop_get_tag(op) == IROP_TAG_IMM32 && !op.is_lval)
    return (irop_get_imm32(op) & ~mask) == 0;
  if (irop_get_tag(op) != IROP_TAG_VREG || op.is_lval || op.is_llocal)
    return 0;
  int ob = sra_btype_bytes(irop_get_btype(op));
  if (ob && ob <= bytes && op.is_unsigned)
    return 1;
  int t = sra_temp_pos(op, ntemp);
  if (t < 0 || !def || def[t] < 0)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[def[t]];
  if (dq->op == TCCIR_OP_AND)
  {
    IROperand a = tcc_ir_op_get_src1(ir, dq), b = tcc_ir_op_get_src2(ir, dq);
    return (irop_get_tag(b) == IROP_TAG_IMM32 && !b.is_lval && (irop_get_imm32(b) & ~mask) == 0) ||
           (irop_get_tag(a) == IROP_TAG_IMM32 && !a.is_lval && (irop_get_imm32(a) & ~mask) == 0);
  }
  if (dq->op == TCCIR_OP_LOAD || dq->op == TCCIR_OP_ASSIGN)
  {
    IROperand a = tcc_ir_op_get_src1(ir, dq);
    int ab = sra_btype_bytes(irop_get_btype(a));
    return ab && ab <= bytes && a.is_unsigned;
  }
  return 0;
}

/* A narrow field's STORE becomes an ASSIGN -- a definition of the VAR -- with
 * the same two operands and the VAR widened to 32 bits. */
static void sra_store_as_def(TCCIRState *ir, int i, IROperand rep)
{
  rep.is_lval = 0; /* a value definition of the VAR, not a write through it */
  ir->compact_instructions[i].op = TCCIR_OP_ASSIGN;
  tcc_ir_set_dest(ir, i, rep);
}

/* No two fields share a byte. */
static int sra_fields_disjoint(const SraUnit *u)
{
  int end = 0;
  for (int at = 0; at < SRA_MAX_BYTES; at++)
  {
    if (!u->width[at])
      continue;
    if (at < end)
      return 0;
    end = at + u->width[at];
  }
  return 1;
}

static int sra_find_unit(SraUnit *units, int nu, int off)
{
  int a = 0, b = nu - 1;
  while (a <= b)
  {
    int m = (a + b) / 2;
    if (off < units[m].lo)
      b = m - 1;
    else if (off >= units[m].hi)
      a = m + 1;
    else
      return m;
  }
  return -1;
}

/* Slot s (0 dest, 1 src1, 2 src2) of an instruction: the width in bytes of the
 * field a VAR can stand in for there, or 0.  A whole four-byte struct moved
 * like an int counts as its 32-bit field at offset 0. */
static int sra_slot_bytes(TCCIRState *ir, TccIrOp op, int s, IROperand o)
{
  if (!o.is_lval || o.is_llocal || o.is_complex || tcc_ir_access_is_volatile(ir, o))
    return 0;
  int bt = irop_get_btype(o);
  if (sra_btype_bytes(bt) &&
      ((s == 1 && (op == TCCIR_OP_LOAD || op == TCCIR_OP_ASSIGN)) || (s == 0 && op == TCCIR_OP_STORE)))
    return sra_btype_bytes(bt);
  if (bt == IROP_BTYPE_STRUCT && sra_struct_is_word(o) &&
      ((s == 1 && (op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_RETURNVALUE)) ||
       (s == 0 && op == TCCIR_OP_FUNCCALLVAL)))
    return 4;
  return 0;
}

/* The ops that keep a fourth operand at operand_base + 3 (frame.c's list). */
static int sra_op_has_slot3(int op)
{
  return op == TCCIR_OP_MLA || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_SELECT;
}

/* Operand slot s (0-3) of q, or IROP_NONE. */
static IROperand sra_operand(TCCIRState *ir, IRQuadCompact *q, int s)
{
  if (s == 0 && irop_config[q->op].has_dest)
    return tcc_ir_op_get_dest(ir, q);
  if (s == 1 && irop_config[q->op].has_src1)
    return tcc_ir_op_get_src1(ir, q);
  if (s == 2 && irop_config[q->op].has_src2)
    return tcc_ir_op_get_src2(ir, q);
  if (s == 3 && sra_op_has_slot3(q->op) && q->operand_base + 3 < (uint32_t)ir->iroperand_pool_count)
    return ir->iroperand_pool[q->operand_base + 3];
  return IROP_NONE;
}

static int sra_function_eligible(TCCIRState *ir)
{
  if (tcc_state->do_debug || tcc_bounds_checking(tcc_state))
    return 0;
  if (ir->has_static_chain || ir->captured_count > 0 || tcc_state->nb_nested_funcs > 0)
    return 0;
  if (tcc_ir_calls_returns_twice(ir))
    return 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    switch (ir->compact_instructions[i].op)
    {
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

#define SRA_NO_ADDR INT32_MIN

/* A LOAD or STORE through a TEMP base, plain or indexed by a constant: the
 * base's TEMP, with the displacement and the operand that carries the access's
 * width, sign and volatility (ssa:stack_deref_fold reads it the same way).
 * -1 for anything else. */
static int sra_access_through(TCCIRState *ir, IRQuadCompact *q, int ntemp, int *disp, IROperand *acc)
{
  int op = q->op;
  if (op != TCCIR_OP_STORE && op != TCCIR_OP_LOAD && op != TCCIR_OP_STORE_INDEXED && op != TCCIR_OP_LOAD_INDEXED)
    return -1;
  int is_store = op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED;
  int indexed = op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_LOAD_INDEXED;
  IROperand base = is_store ? tcc_ir_op_get_dest(ir, q) : tcc_ir_op_get_src1(ir, q);
  int t = sra_temp_pos(base, ntemp);
  if (t < 0 || irop_get_tag(base) != IROP_TAG_VREG || base.is_local || base.is_llocal || base.is_lval == indexed)
    return -1;
  *disp = 0;
  *acc = base;
  if (indexed)
  {
    IROperand idx = tcc_ir_op_get_src2(ir, q);
    IROperand scale = tcc_ir_op_get_scale(ir, q);
    if (irop_get_tag(idx) != IROP_TAG_IMM32 || idx.is_lval ||
        (!irop_is_none(scale) && (irop_get_tag(scale) != IROP_TAG_IMM32 || irop_get_imm32(scale) != 0)))
      return -1;
    *disp = irop_get_imm32(idx);
    *acc = is_store ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_dest(ir, q);
    acc->aux = base.aux;
  }
  if (irop_is_64bit(*acc) || acc->is_complex || !sra_btype_bytes(irop_get_btype(*acc)) ||
      tcc_ir_access_is_volatile(ir, *acc))
    return -1;
  return t;
}

/* Is q an address computation of a TEMP -- `T <- Addr[slot]`, a copy, or a
 * constant offset of another TEMP?  Returns T's position, or -1. */
static int sra_addr_def(TCCIRState *ir, IRQuadCompact *q, int ntemp)
{
  if (q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  return d.is_lval ? -1 : sra_temp_pos(d, ntemp);
}

int tcc_ir_opt_sra(TCCIRState *ir)
{
  if (!ir || !tcc_state || tcc_state->optimize <= 0 || ir->frame_relaid || ir->frame_obj_count == 0)
    return 0;
  if (!sra_function_eligible(ir))
    return 0;

  /* Units: the frame objects' merged extents, sorted by start. */
  int lo, hi;
  tcc_ir_frame_object_at(ir, ir->frame_objs[0], &lo, &hi); /* builds ir->frame_index */
  int nu = ir->frame_index_n;
  if (nu <= 0)
    return 0;
  SraUnit *u = tcc_mallocz(sizeof(SraUnit) * nu);
  for (int k = 0; k < nu; k++)
  {
    u[k].lo = ir->frame_index[2 * k];
    u[k].hi = ir->frame_index[2 * k + 1];
    for (int b = 0; b < SRA_MAX_BYTES; b++)
      u[k].var[b] = -1;
  }
  const int n = ir->next_instruction_index;

  /* Address TEMPs.  The frontend reaches a field through one
   * (`T <- &s; *(T + 4) = x`).  A TEMP defined once, from `Addr[slot]` or a
   * copy / constant offset of such a TEMP, holds that frame address wherever it
   * is read; when every use is the base of a load or store at a constant
   * displacement, or feeds another such TEMP, the accesses are the object's
   * fields.  Nothing is rewritten unless the object is replaced. */
  const int ntemp = ir->next_temporary_variable;
  uint8_t *defs = ntemp > 0 ? tcc_mallocz(ntemp) : NULL;
  /* Each TEMP's only defining instruction, or -1: what a narrow store's value
   * has to be looked up through (sra_narrow_store_fits). */
  int32_t *def_idx = ntemp > 0 ? tcc_malloc(sizeof(int32_t) * ntemp) : NULL;
  for (int t = 0; t < ntemp; t++)
    def_idx[t] = -1;
  int32_t *toff = ntemp > 0 ? tcc_malloc(sizeof(int32_t) * ntemp) : NULL;
  int32_t *tfrom = ntemp > 0 ? tcc_malloc(sizeof(int32_t) * ntemp) : NULL; /* TEMP it was derived from, or -1 */
  int32_t *troot = ntemp > 0 ? tcc_malloc(sizeof(int32_t) * ntemp) : NULL; /* unit of the root Addr */
  uint8_t *tbad = ntemp > 0 ? tcc_mallocz(ntemp) : NULL;
  for (int t = 0; t < ntemp; t++)
    toff[t] = SRA_NO_ADDR, tfrom[t] = -1, troot[t] = -1;
  for (int i = 0; i < n && ntemp > 0; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int t;
    /* Definitions: a value dest, or a pointer a post-increment writes back.  A
     * STORE_INDEXED dest is the base pointer, read and not written. */
    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE_INDEXED)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if ((!d.is_lval || q->op == TCCIR_OP_STORE_POSTINC) && (t = sra_temp_pos(d, ntemp)) >= 0 && defs[t] < 2)
      {
        def_idx[t] = defs[t] == 0 ? i : -1;
        defs[t]++;
      }
    }
    if (q->op == TCCIR_OP_LOAD_POSTINC && (t = sra_temp_pos(tcc_ir_op_get_src1(ir, q), ntemp)) >= 0)
      defs[t] = 2, def_idx[t] = -1;
  }
  for (int round = 0, changed = 1; round < 8 && changed && ntemp > 0; round++)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      int t = sra_addr_def(ir, q, ntemp);
      if (t < 0 || defs[t] != 1 || toff[t] != SRA_NO_ADDR)
        continue;
      IROperand a = tcc_ir_op_get_src1(ir, q);
      int off, at, k;
      if (q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASSIGN)
      {
        if (!a.is_lval && !a.is_llocal && sra_concrete(a, &off) && (k = sra_find_unit(u, nu, off)) >= 0)
          toff[t] = off, troot[t] = k;
        else if (q->op == TCCIR_OP_ASSIGN && !a.is_lval && (at = sra_temp_pos(a, ntemp)) >= 0 &&
                 toff[at] != SRA_NO_ADDR)
          toff[t] = toff[at], troot[t] = troot[at], tfrom[t] = at;
      }
      else
      {
        IROperand c = tcc_ir_op_get_src2(ir, q);
        if (!a.is_lval && (at = sra_temp_pos(a, ntemp)) >= 0 && toff[at] != SRA_NO_ADDR &&
            irop_get_tag(c) == IROP_TAG_IMM32 && !c.is_lval)
        {
          int64_t v = (int64_t)toff[at] + (q->op == TCCIR_OP_ADD ? 1 : -1) * (int64_t)irop_get_imm32(c);
          if (v > -(1 << 24) && v < (1 << 24))
            toff[t] = (int32_t)v, troot[t] = troot[at], tfrom[t] = at;
        }
      }
      changed |= toff[t] != SRA_NO_ADDR;
    }
  }
  /* A use other than as an access base or the source of a derived address
   * lets the address escape. */
  for (int i = 0; i < n && ntemp > 0; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int disp, dt = sra_addr_def(ir, q, ntemp);
    IROperand acc;
    int bt = sra_access_through(ir, q, ntemp, &disp, &acc);
    int base_slot = q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ? 0 : 1;
    for (int s = 0; s < 4; s++)
    {
      int t = sra_temp_pos(sra_operand(ir, q, s), ntemp);
      if (t < 0 || toff[t] == SRA_NO_ADDR)
        continue;
      if (s == 0 && dt == t)
        continue; /* its own definition */
      if (s == base_slot && bt == t)
        continue;
      if (s == 1 && dt >= 0 && tfrom[dt] == t)
        continue;
      tbad[t] = 1;
    }
  }
  for (int changed = 1; changed && ntemp > 0;)
  {
    changed = 0;
    for (int t = 0; t < ntemp; t++)
      if (tbad[t] && tfrom[t] >= 0 && !tbad[tfrom[t]])
        tbad[tfrom[t]] = 1, changed = 1;
  }

  /* Every reference a live instruction makes, the fourth operand slot of
   * MLA / indexed accesses / SELECT included: an object keeps its memory
   * unless all of them can be replaced. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int dt = ntemp > 0 ? sra_addr_def(ir, q, ntemp) : -1;
    for (int s = 0; s < 4; s++)
    {
      IROperand o = sra_operand(ir, q, s);
      int off, k;
      if (!sra_concrete(o, &off) || (k = sra_find_unit(u, nu, off)) < 0)
        continue;
      u[k].refs++;
      if (s == 1 && dt >= 0 && toff[dt] != SRA_NO_ADDR && tfrom[dt] < 0)
      {
        if (!tbad[dt])
          u[k].ok_refs++; /* the root of an address used only for field accesses */
        continue;
      }
      int bytes = s < 3 ? sra_slot_bytes(ir, q->op, s, o) : 0;
      if (bytes && (irop_get_btype(o) != IROP_BTYPE_STRUCT || off == u[k].lo))
      {
        if (bytes < 4 && s == 0 && q->op == TCCIR_OP_STORE &&
            !sra_narrow_store_fits(ir, q, bytes, def_idx, ntemp))
          u[k].bad = 1;
        sra_note_field(&u[k], off, bytes, o.is_unsigned);
        u[k].ok_refs++;
      }
    }
    int disp;
    IROperand acc;
    int t = ntemp > 0 ? sra_access_through(ir, q, ntemp, &disp, &acc) : -1;
    if (t >= 0 && toff[t] != SRA_NO_ADDR && !tbad[t] && troot[t] >= 0)
    {
      int off = toff[t] + disp, k = sra_find_unit(u, nu, off);
      if (k != troot[t])
        u[troot[t]].bad = 1; /* strays outside the object */
      else
      {
        u[k].refs++;
        u[k].ok_refs++;
        int abytes = sra_btype_bytes(irop_get_btype(acc));
        if (abytes < 4 && (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
            (q->op == TCCIR_OP_STORE_INDEXED ||
             !sra_narrow_store_fits(ir, q, abytes, def_idx, ntemp)))
          u[k].bad = 1;
        sra_note_field(&u[k], off, abytes, acc.is_unsigned);
      }
    }
  }
  for (int k = 0; k < nu; k++)
    if (u[k].ok_refs != u[k].refs || !sra_fields_disjoint(&u[k]) || !sra_narrow_fields_unsigned(&u[k]))
      u[k].bad = 1;

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Address arithmetic of a replaced object: nothing reads it any more. */
    int dt = ntemp > 0 ? sra_addr_def(ir, q, ntemp) : -1;
    if (dt >= 0 && toff[dt] != SRA_NO_ADDR && troot[dt] >= 0 && !tbad[dt] && !u[troot[dt]].bad)
    {
      q->op = TCCIR_OP_NOP;
      changes++;
      continue;
    }
    int disp;
    IROperand acc;
    int t = ntemp > 0 ? sra_access_through(ir, q, ntemp, &disp, &acc) : -1;
    if (t >= 0 && toff[t] != SRA_NO_ADDR && !tbad[t] && troot[t] >= 0 && !u[troot[t]].bad)
    {
      int k = troot[t], at = toff[t] + disp - u[k].lo;
      if (u[k].var[at] < 0)
        u[k].var[at] = tcc_ir_vreg_alloc_var(ir);
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, u[k].var[at]);
      const int narrow = u[k].width[at] && u[k].width[at] < 4;
      const int rbt = narrow ? IROP_BTYPE_INT32 : irop_get_btype(acc);
      IROperand rep = irop_make_stackoff(u[k].var[at], iv ? iv->original_offset : 0, 1, 0, 0, rbt);
      rep.is_unsigned = narrow ? 1 : acc.is_unsigned;
      rep.aux = IROP_AUX_NONVOLATILE;
      int is_store = q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED;
      if (is_store && narrow)
      {
        sra_store_as_def(ir, i, rep);
        changes++;
        continue;
      }
      q->op = is_store ? TCCIR_OP_STORE : TCCIR_OP_LOAD;
      if (is_store)
        tcc_ir_set_dest(ir, i, rep);
      else
        tcc_ir_set_src1(ir, i, rep);
      changes++;
      continue;
    }
    for (int s = 0; s < 3; s++)
    {
      IROperand o = sra_operand(ir, q, s);
      int off, k;
      if (!sra_concrete(o, &off) || (k = sra_find_unit(u, nu, off)) < 0 || u[k].bad)
        continue;
      int at = off - u[k].lo;
      if (u[k].var[at] < 0)
        u[k].var[at] = tcc_ir_vreg_alloc_var(ir);
      int32_t v = u[k].var[at];
      const int narrow = u[k].width[at] && u[k].width[at] < 4;
      int bt = narrow || irop_get_btype(o) == IROP_BTYPE_STRUCT ? IROP_BTYPE_INT32 : irop_get_btype(o);
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, v);
      IROperand rep = irop_make_stackoff(v, iv ? iv->original_offset : 0, 1, 0, 0, bt);
      rep.is_unsigned = narrow || irop_get_btype(o) == IROP_BTYPE_STRUCT ? 1 : o.is_unsigned;
      rep.aux = IROP_AUX_NONVOLATILE;
      if (s == 0 && narrow && q->op == TCCIR_OP_STORE)
      {
        sra_store_as_def(ir, i, rep);
        changes++;
        continue;
      }
      if (s == 0)
        tcc_ir_set_dest(ir, i, rep);
      else if (s == 1)
        tcc_ir_set_src1(ir, i, rep);
      else
        tcc_ir_set_src2(ir, i, rep);
      changes++;
    }
  }

  tcc_free(def_idx);
  tcc_free(tbad);
  tcc_free(troot);
  tcc_free(tfrom);
  tcc_free(toff);
  tcc_free(defs);
  tcc_free(u);
  return changes;
}
