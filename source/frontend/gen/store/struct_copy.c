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
