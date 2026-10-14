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

/* struct_copy.c -- Struct copy shape analysis and unit-copy emission.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Return 1 if a struct/union type has any VLA (variable-length array)
   member field that requires dynamic stack allocation. */
int struct_has_vla_member(const CType *type)
{
  Sym *f;
  if ((type->t & VT_BTYPE) != VT_STRUCT)
    return 0;
  for (f = type->ref->next; f; f = f->next)
    if (f->type.t & VT_VLA)
      return 1;
  return 0;
}

/* True if the struct has at least one (top-level) bitfield member.  Such
 * structs are usually copied to a local only to poke one field and read it
 * back (the gcc.c-torture/execute/20040709-1.c idiom), so expanding the copy
 * to scalar LOAD/STOREs lets store-forwarding + the bitfield insert/extract
 * fold collapse it.  Plain (non-bitfield) struct copies are more often used
 * whole, where an inline expansion just bloats vs. a single memmove. */
int struct_has_bitfield_member(const CType *type)
{
  Sym *f;
  if ((type->t & VT_BTYPE) != VT_STRUCT || !type->ref)
    return 0;
  for (f = type->ref->next; f; f = f->next)
    if (f->type.t & VT_BITFIELD)
      return 1;
  return 0;
}

int struct_is_single_2byte_scalar_member(const CType *type)
{
  int align;
  Sym *f;
  if ((type->t & VT_BTYPE) != VT_STRUCT || !type->ref)
    return 0;
  f = type->ref->next;
  if (!f || f->next || f->c != 0)
    return 0;
  if (f->type.t & VT_BITFIELD)
    return 0;
  return type_size(&f->type, &align) == 2;
}

/* A small struct whose members are *all* bitfields (the packed
 * poke-one-field idiom, e.g. `struct { unsigned short i:1,j:3,k:11; }` or
 * `struct { unsigned int k:6,l:1,j:10,i:15; }`).  When the whole aggregate
 * fits in a single 1/2/4-byte storage unit it can be copied as one
 * byte/halfword/word LOAD/STORE whose access width matches how the bitfields
 * are later read — exposing the value to store-load forwarding (which only
 * narrows a wider store for *immediate* values, so a width-matched copy is
 * what lets the downstream bitfield insert/extract fold collapse the copy).
 * Packed bitfield structs have align 1, which keeps them out of the
 * word-aligned scalar-expansion path, so they would otherwise memmove. */
int struct_is_small_bitfield_word(const CType *type)
{
  int align, sz, saw = 0;
  Sym *f;
  if ((type->t & VT_BTYPE) != VT_STRUCT || !type->ref)
    return 0;
  sz = type_size(type, &align);
  if (sz != 1 && sz != 2 && sz != 4)
    return 0;
  for (f = type->ref->next; f; f = f->next)
  {
    if (!(f->type.t & VT_BITFIELD))
      return 0;
    saw = 1;
  }
  return saw;
}

/* Width (1/2/4/8) of the storage unit through which a bitfield field `f` is
 * accessed (mirrors adjust_bf): auxtype names the access type, -1 means the
 * field's own declared base type, VT_STRUCT means byte-wise (0 here). */
static int bitfield_unit_width(const Sym *f)
{
  int align, aux = f->type.ref ? f->type.ref->auxtype : -1;
  CType t;
  t.ref = NULL;
  if (aux == VT_STRUCT)
    return 0;
  if (aux != -1)
    t.t = aux;
  else
    t.t = f->type.t & ~VT_STRUCT_MASK; /* strip bitfield pos/size, keep base */
  return type_size(&t, &align);
}

/* True if a small struct is safe and worthwhile to copy member-wise via
 * ir_emit_struct_unit_copy: it has at least one bitfield (the poke-one-field
 * idiom that benefits from width-matched forwarding) AND *every* member is
 * accessed in a single 1/2/4-byte unit.
 *
 * The all-accesses-<=4 requirement is a correctness guard, not just a tuning
 * knob.  ir_emit_struct_unit_copy tiles the aggregate with <=4-byte chunks; if
 * any member is read with a WIDER access that overlaps several of those chunks
 * — a `long long`/`double` scalar, or a bitfield that straddles the 32-bit
 * boundary and is therefore read as a 64-bit unit (e.g. pr57344's `int b:22`
 * crossing bit 32) — store-load forwarding partial-forwards that wide load
 * from the narrow stores and corrupts the value.  Keeping every access <=4
 * bytes means each read width-matches (or is narrower than, hence reads memory
 * from) exactly one chunk, which is always sound. */
int struct_member_copy_safe(const CType *type)
{
  Sym *f;
  int align, saw_bf = 0;
  if ((type->t & VT_BTYPE) != VT_STRUCT || !type->ref)
    return 0;
  for (f = type->ref->next; f; f = f->next)
  {
    if (f->type.t & VT_BITFIELD)
    {
      int w = bitfield_unit_width(f);
      if (w != 1 && w != 2 && w != 4)
        return 0; /* byte-wise (0) or 64-bit straddle: unsafe */
      saw_bf = 1;
    }
    else
    {
      int w = type_size(&f->type, &align);
      if (w != 1 && w != 2 && w != 4)
        return 0; /* ull/double/long double/nested aggregate: unsafe */
    }
  }
  return saw_bf;
}

/* Emit a byte-exact copy of a small struct as a sequence of width-aligned
 * LOAD/STORE pairs, choosing each chunk's width to match the access width of
 * the bitfield storage unit (or scalar member) starting there.  Width-matched
 * chunks let store-load forwarding feed a copied bitfield word straight into a
 * later read (the forwarder only narrows a *wider* store for immediates), so
 * the downstream bitfield insert/extract fold can collapse packed-struct
 * copies that would otherwise be an opaque memmove.  `src`/`dst` are LOCAL or
 * GLOBAL lvalues whose c.i is the base byte offset; caller has popped src. */
void ir_emit_struct_unit_copy(const SValue *src, const SValue *dst,
                                     const CType *stype, int size)
{
  unsigned char cut[16]; /* preferred chunk width starting at each byte */
  Sym *f;
  int p;

  if (size <= 0 || size > (int)sizeof(cut))
    return; /* caller gates size <= 16; defensive against overflow */
  memset(cut, 0, sizeof(cut));
  for (f = stype->ref->next; f; f = f->next)
  {
    int off = f->c, w, align;
    if (f->type.t & VT_BITFIELD)
      w = bitfield_unit_width(f);
    else
      w = type_size(&f->type, &align);
    if ((w != 1 && w != 2 && w != 4) || off < 0 || off + w > size || (off % w))
      continue; /* >4 (ull/long double) or misaligned: leave to greedy cover */
    if (cut[off] < w)
      cut[off] = (unsigned char)w;
  }

  for (p = 0; p < size;)
  {
    int w = 0, cand;
    if (cut[p] && (p % cut[p]) == 0 && p + cut[p] <= size)
      w = cut[p];
    else
      for (cand = 4; cand >= 1; cand >>= 1)
      {
        int k, crosses = 0;
        if ((p % cand) != 0 || p + cand > size)
          continue;
        for (k = p + 1; k < p + cand; k++)
          if (cut[k])
          {
            crosses = 1;
            break;
          }
        if (!crosses)
        {
          w = cand;
          break;
        }
      }
    if (w == 0)
      w = 1;

    {
      SValue s, d, tmp;
      CType ct;
      ct.ref = NULL;
      ct.t = (w == 1 ? (VT_BYTE | VT_UNSIGNED)
                     : w == 2 ? (VT_SHORT | VT_UNSIGNED) : VT_INT);

      svalue_init(&s);
      s.type = ct;
      s.r = src->r;
      s.vr = src->vr;
      s.sym = src->sym;
      s.c.i = src->c.i + p;
      s.volatile_access = src->volatile_access;
      /* The unit-chunk type hides the packed container the side lives in;
       * same sticky-mark rule as small_copy_lval. */
      s.underaligned = src->underaligned || (p & 3);

      svalue_init(&tmp);
      tmp.type = ct;
      tmp.r = 0;
      tmp.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &s, NULL, &tmp);

      svalue_init(&d);
      d.type = ct;
      d.r = dst->r;
      d.vr = dst->vr;
      d.sym = dst->sym;
      d.c.i = dst->c.i + p;
      d.volatile_access = dst->volatile_access;
      d.underaligned = dst->underaligned || (p & 3);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &tmp, NULL, &d);
    }
    p += w;
  }
}

int struct_is_single_1byte_scalar_member(const CType *type)
{
  int align;
  Sym *f;
  if ((type->t & VT_BTYPE) != VT_STRUCT || !type->ref)
    return 0;
  f = type->ref->next;
  if (!f || f->next || f->c != 0)
    return 0;
  if (f->type.t & VT_BITFIELD)
    return 0;
  return type_size(&f->type, &align) == 1;
}

/* Mark the bytes of `t` placed at `base` as covered, rejecting anything the
 * chunked copy below cannot move without splitting an access: a scalar or
 * bitfield unit wider than the chunk width `w` (a later read of it would be
 * wider than the stores that wrote it, which store-load forwarding must not
 * partial-forward), or one not naturally aligned inside the aggregate. */
static int small_copy_cover(const CType *t, int base, int w, int size, unsigned char *covered)
{
  int align, n;

  if (t->t & VT_ARRAY)
  {
    const CType *et = pointed_type((CType *)t);
    int esize = type_size((CType *)et, &align);
    if (!t->ref || t->ref->c < 0 || esize <= 0)
      return 0; /* flexible/incomplete array member */
    for (int i = 0; i < t->ref->c; i++)
      if (!small_copy_cover(et, base + i * esize, w, size, covered))
        return 0;
    return 1;
  }
  if ((t->t & VT_BTYPE) == VT_STRUCT) /* struct or union */
  {
    if (!t->ref)
      return 0;
    for (Sym *f = t->ref->next; f; f = f->next)
    {
      if (f->type.t & VT_BITFIELD)
      {
        int bw = bitfield_unit_width(f);
        int off = base + (f->c & ~(bw - 1));
        if (bw != 1 && bw != 2 && bw != 4)
          return 0;
        if (bw > w || off < 0 || off + bw > size || (off % bw))
          return 0;
        for (int k = off; k < off + bw; k++)
          covered[k] = 1;
      }
      else if (!small_copy_cover(&f->type, base + f->c, w, size, covered))
        return 0;
    }
    return 1;
  }
  if (t->t & (VT_VECTOR | VT_COMPLEX | VT_VLA))
    return 0;
  n = type_size((CType *)t, &align);
  if (n == 0)
    return 1; /* zero-sized member */
  if ((n != 1 && n != 2 && n != 4) || n > w || base < 0 || base + n > size || (base % n))
    return 0; /* 8-byte scalars keep memmove: a split one would be read wider */
  for (int k = base; k < base + n; k++)
    covered[k] = 1;
  return 1;
}

/* Plan the copy of a small aggregate for ir_emit_small_aggregate_copy: every
 * chunk has one width w = min(4, the type's alignment, the alignment of each
 * frame/global base offset), so each field sits inside a single chunk and no
 * chunk is narrower than a field it holds.  Returns the number of chunks that
 * carry data (pure-padding chunks are not copied), or 0 if the type cannot be
 * tiled that way.  `src_off`/`dst_off` are the c.i of a LOCAL/GLOBAL side, or
 * -1 for a register-deref side, which is trusted to the type's alignment. */
int small_aggregate_copy_plan(const CType *stype, int size, int align, int src_off, int dst_off, int *w_out,
                              unsigned char *covered)
{
  int w = align < 4 ? align : 4, chunks = 0;
  if (size <= 0 || size > SMALL_AGGREGATE_COPY_MAX || w <= 0)
    return 0;
  while (w > 1 && ((src_off >= 0 && (src_off % w)) || (dst_off >= 0 && (dst_off % w))))
    w >>= 1;
  if (size % w)
    return 0;
  memset(covered, 0, SMALL_AGGREGATE_COPY_MAX);
  if (!small_copy_cover(stype, 0, w, size, covered))
    return 0;
  for (int p = 0; p < size; p += w)
  {
    int any = 0;
    for (int k = p; k < p + w; k++)
      any |= covered[k];
    chunks += any;
  }
  *w_out = w;
  return chunks;
}

/* Address `side` + off as an lvalue of type `ct`.  A LOCAL/GLOBAL side takes the
 * offset in c.i; a register-deref side (whose c.i the codegen does not honour)
 * gets an explicit ADD, which displacement fusion later folds into the access. */
static SValue small_copy_lval(const SValue *side, int is_deref, int off, CType ct)
{
  SValue v;
  svalue_init(&v);
  v.type = ct;
  v.volatile_access = side->volatile_access;
  int side_align;
  type_size(&side->type, &side_align);
  v.underaligned = side->underaligned || side_align < 4 || (off & 3);
  if (is_deref && off != 0)
  {
    SValue base, imm, ptr;
    svalue_init(&base);
    base.type.t = VT_PTR;
    base.vr = side->vr;
    base.r = 0;
    svalue_init(&imm);
    imm.type.t = VT_INT;
    imm.r = VT_CONST;
    imm.vr = -1;
    imm.c.i = off;
    svalue_init(&ptr);
    ptr.type.t = VT_PTR;
    ptr.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    ptr.r = 0;
    tcc_ir_put(tcc_state->ir, TCCIR_OP_ADD, &base, &imm, &ptr);
    v.r = VT_LVAL;
    v.vr = ptr.vr;
    return v;
  }
  v.r = side->r;
  v.vr = side->vr;
  v.sym = side->sym;
  v.c.i = side->c.i + off;
  return v;
}

static int small_copy_chunk_width(const unsigned char *covered, int p, int w)
{
  int end = p + w;
  while (end > p && !covered[end - 1])
    end--;
  if (end == p)
    return 0;
  int bytes = 1;
  while (bytes < end - p)
    bytes <<= 1;
  return bytes;
}

/* Load every chunk before any store; omit padding beyond its last covered byte. */
static void ir_emit_small_aggregate_chunks(const SValue *src, int src_deref, const SValue *dst, int dst_deref,
                                           int size, int w, const unsigned char *covered, int narrow_padding)
{
  int tmp_vr[SMALL_AGGREGATE_COPY_MAX];
  CType ct;
  ct.ref = NULL;
  for (int p = 0; p < size; p += w)
  {
    int bytes = small_copy_chunk_width(covered, p, w);
    tmp_vr[p / w] = -1;
    if (!bytes)
      continue; /* padding only */
    if (!narrow_padding || src->volatile_access || dst->volatile_access)
      bytes = w;
    ct.t = (bytes == 1 ? (VT_BYTE | VT_UNSIGNED) : bytes == 2 ? (VT_SHORT | VT_UNSIGNED) : VT_INT);
    SValue s = small_copy_lval(src, src_deref, p, ct), tmp;
    svalue_init(&tmp);
    tmp.type = ct;
    tmp.r = 0;
    tmp.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_LOAD, &s, NULL, &tmp);
    tmp_vr[p / w] = tmp.vr;
  }
  for (int p = 0; p < size; p += w)
  {
    if (tmp_vr[p / w] < 0)
      continue;
    int bytes = small_copy_chunk_width(covered, p, w);
    if (!narrow_padding || src->volatile_access || dst->volatile_access)
      bytes = w;
    ct.t = (bytes == 1 ? (VT_BYTE | VT_UNSIGNED) : bytes == 2 ? (VT_SHORT | VT_UNSIGNED) : VT_INT);
    SValue tmp, d = small_copy_lval(dst, dst_deref, p, ct);
    svalue_init(&tmp);
    tmp.type = ct;
    tmp.r = 0;
    tmp.vr = tmp_vr[p / w];
    tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &tmp, NULL, &d);
  }
}

void ir_emit_small_aggregate_copy(const SValue *src, int src_deref, const SValue *dst, int dst_deref, int size,
                                  int w, const unsigned char *covered)
{
  ir_emit_small_aggregate_chunks(src, src_deref, dst, dst_deref, size, w, covered, 0);
}

int ir_emit_small_padded_local_copy(const SValue *src, const SValue *dst, const CType *stype, int size, int align)
{
  if (size < 4 || size > SMALL_AGGREGATE_COPY_MAX || (size & 3) || (align & 3) ||
      src->volatile_access || dst->volatile_access ||
      (src->r & (VT_VALMASK | VT_LVAL)) != (VT_LOCAL | VT_LVAL) ||
      (dst->r & (VT_VALMASK | VT_LVAL)) != (VT_LOCAL | VT_LVAL))
    return 0;
  unsigned char covered[SMALL_AGGREGATE_COPY_MAX];
  int w;
  if (!small_aggregate_copy_plan(stype, size, align, (int)src->c.i, (int)dst->c.i, &w, covered) ||
      w != 4 || covered[size - 2] || covered[size - 1])
    return 0;
  if (src->c.i != dst->c.i || src->vr != dst->vr)
    ir_emit_small_aggregate_chunks(src, 0, dst, 0, size, w, covered, 1);
  return 1;
}
