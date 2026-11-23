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

/* struct.c -- struct/union declaration parsing and field layout.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

Sym *find_field(CType *type, int v, int *cumofs)
{
  Sym *s = type->ref;
  int v1 = v | SYM_FIELD;
  sym_first(s); /* the loop steps from the struct Sym to its members */
  if (!(v & SYM_FIELD))
  { /* top-level call */
    if ((type->t & VT_BTYPE) != VT_STRUCT)
      expect("struct or union");
    if (v < TOK_UIDENT)
      expect("field name");
    if (s->c < 0)
      tcc_error("dereferencing incomplete type '%s'", get_tok_str(s->v & ~SYM_STRUCT, 0));
  }
  while ((s = s->next) != NULL)
  {
    if (s->v == v1)
    {
      *cumofs = s->c;
      return s;
    }
    if ((s->type.t & VT_BTYPE) == VT_STRUCT && s->v >= (SYM_FIRST_ANOM | SYM_FIELD))
    {
      /* try to find field in anonymous sub-struct/union */
      Sym *ret = find_field(&s->type, v1, cumofs);
      if (ret)
      {
        *cumofs += s->c;
        return ret;
      }
    }
  }
  if (!(v & SYM_FIELD))
    tcc_error("field not found: %s", get_tok_str(v, NULL));
  return s;
}

static void check_fields(CType *type, int check)
{
  Sym *s = type->ref;
  sym_first(s); /* the loop steps from the struct Sym to its members */

  while ((s = s->next) != NULL)
  {
    int v = s->v & ~SYM_FIELD;
    if (v < SYM_FIRST_ANOM)
    {
      TokenSym *ts = table_ident[v - TOK_IDENT];
      if (check && (ts->tok & SYM_FIELD))
        tcc_error("duplicate member '%s'", get_tok_str(v, NULL));
      ts->tok ^= SYM_FIELD;
    }
    else if ((s->type.t & VT_BTYPE) == VT_STRUCT)
      check_fields(&s->type, check);
  }
}

/* scalar_storage_order("big-endian"): convert the bitfields [g0, stop) of one
   storage-unit group from LE to BE bit positions.  PCC layout can place a
   field across its declared type's unit (a `long long :4` demoted to int that
   starts at bit 29), so the unit is widened to cover every field's extent;
   otherwise unit_bits - abs_bp - bs goes negative and corrupts type.t. */
static void sso_flush_group(Sym *g0, Sym *stop, int start_off, int unit_bits, int base_type, int content_end)
{
  Sym *g;
  int need = 0;
  for (g = g0; g != stop; g = g->next)
  {
    if (!(g->type.t & VT_BITFIELD) || BIT_SIZE(g->type.t) == 0)
      continue;
    int end = (g->c - start_off) * 8 + BIT_POS(g->type.t) + BIT_SIZE(g->type.t);
    if (end > need)
      need = end;
  }
  if (need > unit_bits)
  {
    while (unit_bits < need && unit_bits < 64)
      unit_bits *= 2;
    base_type = unit_bits == 64 ? VT_LLONG : unit_bits == 32 ? VT_INT : VT_SHORT;
  }
  const int ubytes = unit_bits / 8;
  /* The unit must not run past the end of the struct (packed: no tail padding):
     anchor it earlier, so the group sits in its low-order bytes. */
  if (start_off + ubytes > content_end)
  {
    if (content_end - ubytes >= 0)
    {
      int anchor = content_end - ubytes;
      for (g = g0; g != stop; g = g->next)
        if ((g->type.t & VT_BITFIELD) && BIT_SIZE(g->type.t) != 0)
        {
          int bp = (g->c - anchor) * 8 + BIT_POS(g->type.t);
          g->c = anchor;
          g->type.t = (g->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | ((unsigned)bp << VT_STRUCT_SHIFT);
        }
      start_off = anchor;
    }
    else
    {
      /* The struct is smaller than any unit covering the group (a packed 3-byte
         group): give each field the smallest unit covering its own bytes. */
      for (g = g0; g != stop; g = g->next)
      {
        if (!(g->type.t & VT_BITFIELD) || BIT_SIZE(g->type.t) == 0)
          continue;
        int bs = BIT_SIZE(g->type.t);
        int abs_bp = (g->c - start_off) * 8 + BIT_POS(g->type.t);
        int first = start_off + abs_bp / 8;
        int span = (abs_bp + bs - 1) / 8 - abs_bp / 8 + 1;
        int fb = span <= 1 ? 1 : span <= 2 ? 2 : span <= 4 ? 4 : 8;
        int anchor = first + fb > content_end ? content_end - fb : first;
        if (anchor < 0)
          anchor = first; /* no room: leave the overflow to be diagnosed */
        int be_bp = fb * 8 - ((first - anchor) * 8 + abs_bp % 8) - bs;
        int fbase = fb == 8 ? VT_LLONG : fb == 4 ? VT_INT : fb == 2 ? VT_SHORT : VT_BYTE;
        if (be_bp < 0)
          be_bp = 0;
        g->c = anchor;
        g->type.t = (g->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | ((unsigned)be_bp << VT_STRUCT_SHIFT);
        g->type.ref = g;
        g->a.sso_be = 1;
        g->r = fb;
        g->auxtype = (g->type.t & VT_BTYPE) != fbase ? fbase : -1;
      }
      return;
    }
  }
  for (g = g0; g != stop; g = g->next)
  {
    if (!(g->type.t & VT_BITFIELD) || BIT_SIZE(g->type.t) == 0)
      continue;
    int abs_bp = (g->c - start_off) * 8 + BIT_POS(g->type.t);
    int bs = BIT_SIZE(g->type.t);
    int be_bp = unit_bits - abs_bp - bs;
    if (be_bp < 0)
      be_bp = 0;
    g->c = start_off;
    g->type.t = (g->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | ((unsigned)be_bp << VT_STRUCT_SHIFT);
    g->type.ref = g;
    g->a.sso_be = 1;
    g->r = ubytes;
    if ((g->type.t & VT_BTYPE) != base_type)
      g->auxtype = base_type;
    else
      g->auxtype = -1;
  }
}

void struct_layout(CType *type, AttributeDef *ad)
{
  int size, align, maxalign, offset, c, bit_pos, bit_size;
  int packed, a, bt, prevbt, prev_bit_size;
  int pcc = !tcc_state->ms_bitfields;
  int pragma_pack = *tcc_state->pack_stack_ptr;
  Sym *f;

  maxalign = 1;
  offset = 0;
  c = 0;
  bit_pos = 0;
  prevbt = VT_STRUCT; /* make it never match */
  prev_bit_size = 0;

  // #define BF_DEBUG

  for (f = sym_first(type->ref); f; f = f->next)
  {
    /* VLA fields in structs: data is stored inline, so the field has
       zero bytes in the fixed (compile-time) size component.  Its runtime
       size will be added by vpush_type_size at access/sizeof time. */
    if ((f->type.t & VT_VLA) && type->ref->type.t != VT_UNION)
    {
      /* Get element type alignment for the VLA data */
      int vla_align;
      type_size(&f->type.ref->type, &vla_align);
      if (pcc)
        c += (bit_pos + 7) >> 3;
      c = (c + vla_align - 1) & -vla_align;
      offset = c;
      /* Do NOT add size to c — VLA size is runtime-dependent */
      bit_pos = 0;
      prevbt = VT_STRUCT;
      prev_bit_size = 0;
      if (vla_align > maxalign)
        maxalign = vla_align;

      f->c = offset;
      f->r = 0;
      continue;
    }

    if (f->type.t & VT_BITFIELD)
      bit_size = BIT_SIZE(f->type.t);
    else
      bit_size = -1;
    size = type_size(&f->type, &align);
    a = f->a.aligned ? 1 << (f->a.aligned - 1) : 0;
    packed = 0;

    if (pcc && bit_size == 0)
    {
      /* in pcc mode, packing does not affect zero-width bitfields */
    }
    else
    {
      /* in pcc mode, attribute packed overrides if set. */
      if (pcc && (f->a.packed || ad->a.packed))
        align = packed = 1;

      /* pragma pack overrides align if lesser and packs bitfields always */
      if (pragma_pack)
      {
        packed = 1;
        if (pragma_pack < align)
          align = pragma_pack;
        /* in pcc mode pragma pack also overrides individual align */
        if (pcc && pragma_pack < a)
          a = 0;
      }
    }
    /* some individual align was specified */
    if (a)
      align = a;

    if (type->ref->type.t == VT_UNION)
    {
      if (pcc && bit_size >= 0)
        size = (bit_size + 7) >> 3;
      offset = 0;
      if (size > c)
        c = size;
    }
    else if (bit_size < 0)
    {
      if (pcc)
        c += (bit_pos + 7) >> 3;
      c = (c + align - 1) & -align;
      offset = c;
      if (size > 0)
        c += size;
      bit_pos = 0;
      prevbt = VT_STRUCT;
      prev_bit_size = 0;
    }
    else
    {
      /* A bit-field.  Layout is more complicated.  There are two
         options: PCC (GCC) compatible and MS compatible */
      if (pcc)
      {
        /* In PCC layout a bit-field is placed adjacent to the
           preceding bit-fields, except if:
           - it has zero-width
           - an individual alignment was given
           - it would overflow its base type container and
             there is no packing */
        if (bit_size == 0)
        {
        new_field:
          c = (c + ((bit_pos + 7) >> 3) + align - 1) & -align;
          bit_pos = 0;
        }
        else if (f->a.aligned)
        {
          goto new_field;
        }
        else if (!packed)
        {
          int a8 = align * 8;
          int ofs = (int)(((int64_t)c * 8 + bit_pos) % a8 + bit_size + a8 - 1) / a8;
          if (ofs > size / align)
            goto new_field;
        }

        /* in pcc mode, long long bitfields have type int if they fit */
        if (size == 8 && bit_size <= 32)
          f->type.t = (f->type.t & ~VT_BTYPE) | VT_INT, size = 4;

        while (bit_pos >= align * 8)
          c += align, bit_pos -= align * 8;
        offset = c;

        /* In PCC layout named bit-fields influence the alignment
           of the containing struct using the base types alignment,
           except for packed fields (which here have correct align).  */
        if (f->v & SYM_FIRST_ANOM
            // && bit_size // ??? gcc on ARM/rpi does that
        )
          align = 1;
      }
      else
      {
        bt = f->type.t & VT_BTYPE;
        if ((bit_pos + bit_size > size * 8) || (bit_size > 0) == (bt != prevbt))
        {
          c = (c + align - 1) & -align;
          offset = c;
          bit_pos = 0;
          /* In MS bitfield mode a bit-field run always uses
             at least as many bits as the underlying type.
             To start a new run it's also required that this
             or the last bit-field had non-zero width.  */
          if (bit_size || prev_bit_size)
            c += size;
        }
        /* In MS layout the records alignment is normally
           influenced by the field, except for a zero-width
           field at the start of a run (but by further zero-width
           fields it is again).  */
        if (bit_size == 0 && prevbt != bt)
          align = 1;
        prevbt = bt;
        prev_bit_size = bit_size;
      }

      f->type.t = (f->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | (bit_pos << VT_STRUCT_SHIFT);
      bit_pos += bit_size;
    }
    if (align > maxalign)
      maxalign = align;

#ifdef BF_DEBUG
    printf("set field %s offset %-2d size %-2d align %-2d", get_tok_str(f->v & ~SYM_FIELD, NULL), offset, size, align);
    if (f->type.t & VT_BITFIELD)
    {
      printf(" pos %-2d bits %-2d", BIT_POS(f->type.t), BIT_SIZE(f->type.t));
    }
    printf("\n");
#endif

    f->c = offset;
    f->r = 0;
  }

  if (pcc)
    c += (bit_pos + 7) >> 3;

  /* store size and alignment */
  a = bt = ad->a.aligned ? 1 << (ad->a.aligned - 1) : 1;
  if (a < maxalign)
    a = maxalign;
  type->ref->r = a;
  if (pragma_pack && pragma_pack < maxalign && 0 == pcc)
  {
    /* can happen if individual align for some member was given.  In
       this case MSVC ignores maxalign when aligning the size */
    a = pragma_pack;
    if (a < bt)
      a = bt;
  }
  c = (c + a - 1) & -a;
  type->ref->c = c;

#ifdef BF_DEBUG
  printf("struct size %-2d align %-2d\n\n", c, a), fflush(stdout);
#endif

  /* For big-endian scalar_storage_order: convert LE bit positions to BE.
     Must run BEFORE the bitfield fixup loop so that field offsets are still
     in their original (pre-fixup) positions. All fields in a storage unit
     share the same base offset and use the widest type for access.
     Note: PCC layout may split fields across byte boundaries (e.g. char
     fields at offset 1 within a 2-byte short-based unit), so we group by
     overlapping byte ranges, not by exact offset. */
  if (ad->a.sso_be)
  {
    type->ref->a.sso_be = 1;
    /* every member is marked: member access swaps the scalar ones (and
       arrays of them), sso_flush_group below lays out the bitfields */
    for (f = sym_first(type->ref); f; f = f->next)
      f->a.sso_be = 1;
    /* end of the member data: a packed struct has no padding beyond it */
    int content_end = 0;
    for (f = sym_first(type->ref); f; f = f->next)
    {
      int fend;
      if (f->type.t & VT_BITFIELD)
        fend = f->c + (BIT_POS(f->type.t) + BIT_SIZE(f->type.t) + 7) / 8;
      else
      {
        int fa;
        fend = f->c + type_size(&f->type, &fa);
      }
      if (fend > content_end)
        content_end = fend;
    }
    Sym *group_start = NULL;
    int group_start_off = 0;
    int group_end_off = 0; /* exclusive: first byte outside the group */
    int group_unit_bits = 0;
    int group_base_type = VT_BYTE;

    for (f = sym_first(type->ref); f; f = f->next)
    {
      if (!(f->type.t & VT_BITFIELD) || BIT_SIZE(f->type.t) == 0)
      {
        if (group_start)
          goto sso_flush;
        continue;
      }
      int fsize, falign;
      fsize = type_size(&f->type, &falign);
      int field_end = f->c + fsize;

      if (!group_start || f->c >= group_end_off)
      {
        if (group_start)
        {
        sso_flush:;
          /* Flush current group: convert each field's LE position to BE.
             Compute absolute bit offset from the group's start, then flip. */
          sso_flush_group(group_start, f, group_start_off, group_unit_bits, group_base_type, content_end);
          group_start = NULL;
          if (!(f->type.t & VT_BITFIELD) || BIT_SIZE(f->type.t) == 0)
            continue;
        }
        /* Start new group */
        group_start = f;
        group_start_off = f->c;
        group_end_off = field_end;
        group_unit_bits = fsize * 8;
        group_base_type = f->type.t & VT_BTYPE;
      }
      else
      {
        /* Extend group */
        if (field_end > group_end_off)
          group_end_off = field_end;
        if (fsize * 8 > group_unit_bits)
        {
          group_unit_bits = fsize * 8;
          group_base_type = f->type.t & VT_BTYPE;
        }
      }
    }
    /* Flush last group */
    if (group_start)
    {
      sso_flush_group(group_start, NULL, group_start_off, group_unit_bits, group_base_type, content_end);
    }
  }

  /* check whether we can access bitfields by their type */
  for (f = sym_first(type->ref); f; f = f->next)
  {
    int s, cx, c0;
    int64_t px; /* bit position: c * 8 overflows int past 256 MB */
    CType t;

    if (0 == (f->type.t & VT_BITFIELD))
      continue;
    /* Skip SSO bitfields — they use full storage unit access with byte-swap */
    if (f->a.sso_be)
    {
      if (!f->type.ref)
        f->type.ref = f;
      if (f->auxtype == 0)
        f->auxtype = -1;
      continue;
    }
    f->type.ref = f;
    f->auxtype = -1;
    bit_size = BIT_SIZE(f->type.t);
    if (bit_size == 0)
      continue;
    bit_pos = BIT_POS(f->type.t);
    size = type_size(&f->type, &align);

    if (bit_pos + bit_size <= size * 8 && f->c + size <= c
#ifdef TCC_TARGET_ARM
        && !(f->c & (align - 1))
#endif
    )
      continue;

    /* try to access the field using a different type */
    c0 = -1, s = align = 1;
    t.t = VT_BYTE;
    for (;;)
    {
      px = (int64_t)f->c * 8 + bit_pos;
      cx = (int)((px >> 3) & -align);
      px = px - ((int64_t)cx << 3);
      if (c0 == cx)
        break;
      s = (px + bit_size + 7) >> 3;
      if (s > 4)
      {
        t.t = VT_LLONG;
      }
      else if (s > 2)
      {
        t.t = VT_INT;
      }
      else if (s > 1)
      {
        t.t = VT_SHORT;
      }
      else
      {
        t.t = VT_BYTE;
      }
      s = type_size(&t, &align);
      c0 = cx;
    }

    if (px + bit_size <= s * 8 && cx + s <= c
#ifdef TCC_TARGET_ARM
        && !(cx & (align - 1))
#endif
    )
    {
      /* update offset and bit position */
      f->c = cx;
      bit_pos = (int)px;
      f->type.t = (f->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | (bit_pos << VT_STRUCT_SHIFT);
      if (s != size)
        f->auxtype = t.t;
#ifdef BF_DEBUG
      printf("FIX field %s offset %-2d size %-2d align %-2d "
             "pos %-2d bits %-2d\n",
             get_tok_str(f->v & ~SYM_FIELD, NULL), cx, s, align, px, bit_size);
#endif
    }
    else
    {
      /* fall back to load/store single-byte wise */
      f->auxtype = VT_STRUCT;
#ifdef BF_DEBUG
      printf("FIX field %s : load byte-wise\n", get_tok_str(f->v & ~SYM_FIELD, NULL));
#endif
    }
  }
}

/* enum/struct/union declaration. u is VT_ENUM/VT_STRUCT/VT_UNION */
/* yasc borrows(...) on members: what the table says of each such member */
static YascFieldBorrows *field_borrows;
static int nb_field_borrows;

ST_FUNC YascFieldBorrows *yasc_field_borrows(int i)
{
  return i >= 0 && i < nb_field_borrows ? &field_borrows[i] : NULL;
}

/* borrowing structs: the tag and its borrows(...) names */
static struct
{
  Sym *s;
  int nb, names[4];
} *struct_params;
static int nb_struct_params;
/* the struct, union and enum bodies being parsed (error recovery: a ';' in
   one ends a member, not the declaration) */
int struct_body_depth;
int yasc_sbind_pending, yasc_sbind_static, yasc_sbind_nb, yasc_sbind_names[4];
Sym yasc_static_bound;

ST_FUNC int yasc_struct_params(Sym *s)
{
  for (int i = 0; i < nb_struct_params; i++)
    if (struct_params[i].s == s)
      return struct_params[i].nb;
  return 0;
}

ST_FUNC int yasc_struct_param(Sym *s, int tok)
{
  for (int i = 0; i < nb_struct_params; i++)
    if (struct_params[i].s == s)
      for (int k = 0; k < struct_params[i].nb; k++)
        if (struct_params[i].names[k] == tok)
          return k;
  return -1;
}

ST_FUNC int yasc_borrowing_struct(CType *type)
{
  return (type->t & (VT_BTYPE | VT_ARRAY)) == VT_STRUCT && type->ref && yasc_struct_params(type->ref) > 0;
}

/* functions (by name: a parked prototype's type Sym is made again when it
   is named) whose result is a borrowing struct: what it borrows */
static struct
{
  int name, code;
} *result_binds;
static int nb_result_binds;

ST_FUNC void yasc_struct_result_bind(int name, int code)
{
  for (int i = 0; i < nb_result_binds; i++)
    if (result_binds[i].name == name)
    {
      result_binds[i].code = code;
      return;
    }
  result_binds = tcc_realloc(result_binds, (nb_result_binds + 1) * sizeof *result_binds);
  result_binds[nb_result_binds].name = name;
  result_binds[nb_result_binds++].code = code;
}

ST_FUNC int yasc_struct_result_code(int name)
{
  for (int i = 0; i < nb_result_binds; i++)
    if (result_binds[i].name == name)
      return result_binds[i].code;
  return -1;
}

typedef struct PendingFieldBorrows
{
  Sym *member;
  int nb, st;
  int names[28];
} PendingFieldBorrows;

/* the members of struct REF that borrow from other members: their names
   resolved to offsets, the member's pointer node pointing at its entry */
static void yasc_resolve_field_borrows(Sym *ref, PendingFieldBorrows *fb, int nfb)
{
  for (int i = 0; i < nfb; i++)
  {
    Sym *m = fb[i].member, *f;
    YascFieldBorrows e;
    memset(&e, 0, sizeof e);
    e.self_tok = m->v & ~SYM_FIELD;
    e.self_off = m->c;
    e.is_static = fb[i].st != 0;
    for (int k = 0; k < fb[i].nb; k++)
    {
      int name = fb[i].names[k];
      for (f = sym_first(ref); f && (f->v & ~SYM_FIELD) != name; f = f->next)
        ;
      /* one of the struct's own borrows(...) names: a view of what the
         value borrows, not of a member */
      if (!f && yasc_struct_param(ref, name) >= 0)
        continue;
      if (!f)
        tcc_error("borrows(%s): no member is called '%s'", get_tok_str(name, NULL), get_tok_str(name, NULL));
      if (f == m)
        tcc_error("borrows(%s): a member cannot borrow from itself", get_tok_str(name, NULL));
      if ((f->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR && !(f->type.t & VT_ARRAY))
        tcc_error("borrows(%s): '%s' is neither a pointer nor an array", get_tok_str(name, NULL),
                  get_tok_str(name, NULL));
      if (e.nsrc == YASC_FIELD_SRC_MAX)
        tcc_error("borrows(...) on a member names at most %d members", YASC_FIELD_SRC_MAX);
      e.src_tok[e.nsrc] = name;
      {
        int al;
        e.src_size[e.nsrc] = type_size(&f->type, &al);
      }
      /* an array member is the memory itself; a pointer member's target is
         what was loaded from it */
      e.src_off[e.nsrc++] = (f->type.t & VT_ARRAY) ? ~f->c : f->c;
    }
    if (!e.nsrc && !e.is_static)
      continue; /* only the struct's own names: a plain view member */
    field_borrows = tcc_realloc(field_borrows, (nb_field_borrows + 1) * sizeof *field_borrows);
    field_borrows[nb_field_borrows] = e;
    m->type.ref = sym_pointer_node_b(&m->type.ref->type, m->type.ref->yasc, -2 - nb_field_borrows);
    nb_field_borrows++;
  }
}

/* A struct or union body being parsed: what its members add to. */
typedef struct StructBody
{
  Sym *s, **ps;
  AttributeDef *ad;
  PendingFieldBorrows *fb;
  int u, c, nfb, with_methods;
  int flexible; /* the flexible array member's name: no member after it */
} StructBody;

/* One member declaration of a struct or union body (error recovery goes on
   with the next one: core/recover.c struct_body_recovering). */
static void struct_member(void *arg)
{
  StructBody *sb = arg;
  int v, size, align, bit_size, bsize, bt;
  Sym *ss;
  AttributeDef ad1;
  CType type1, btype;

  /* yasc: a function member is a method (decl/method.c) */
  if (sb->with_methods && yasc_method_member(sb->s))
    return;
  if (!parse_btype(&btype, &ad1, 0))
  {
    if (tok_is_static_assert())
    {
      do_Static_assert();
      return;
    }
    skip(';');
    return;
  }
  btype_no_auto(&ad1);
  /* `deprecated` in the specifiers is every declarator's, after a
     declarator only its own */
  const unsigned char dep_spec = ad1.attr_deprecated;
  const int dep_spec_msg = ad1.attr_deprecated_msg;
  while (1)
  {
    ad1.attr_deprecated = dep_spec;
    ad1.attr_deprecated_msg = dep_spec_msg;
    if (sb->flexible)
      tcc_error("flexible array member '%s' not at the end of struct", get_tok_str(sb->flexible, NULL));
    bit_size = -1;
    v = 0;
    type1 = btype;
    if (tok != ':')
    {
      if (tok != ';')
      {
        int saved_ok = yasc_borrows_field_ok;
        yasc_borrows_field_ok = 1;
        yasc_field_bnb = 0;
        type_decl(&type1, &ad1, &v, TYPE_DIRECT);
        yasc_borrows_field_ok = saved_ok;
      }
      if (v == 0)
      {
        if ((type1.t & VT_BTYPE) != VT_STRUCT)
          expect("identifier");
        else
        {
          int v = btype.ref->v;
          if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
          {
            if (tcc_state->ms_extensions == 0)
              expect("identifier");
          }
        }
      }
      if (type_size(&type1, &align) < 0)
      {
        if ((sb->u == VT_STRUCT) && (type1.t & VT_ARRAY) && sb->c)
          sb->flexible = v ? v : 1;
        else
          tcc_error("field '%s' has incomplete type", get_tok_str(v, NULL));
      }
      if ((type1.t & VT_BTYPE) == VT_FUNC || (type1.t & VT_BTYPE) == VT_VOID || (type1.t & VT_STORAGE))
        tcc_error("invalid type for '%s'", get_tok_str(v, NULL));
    }
    if (tok == ':')
    {
      next();
      bit_size = expr_const();
      /* XXX: handle v = 0 case for messages */
      if (bit_size < 0)
        tcc_error("negative width in bit-field '%s'", get_tok_str(v, NULL));
      if (v && bit_size == 0)
        tcc_error("zero width for bit-field '%s'", get_tok_str(v, NULL));
      parse_attribute(&ad1);
    }
    size = type_size(&type1, &align);
    if (bit_size >= 0)
    {
      bt = type1.t & VT_BTYPE;
      if (bt != VT_INT && bt != VT_BYTE && bt != VT_SHORT && bt != VT_BOOL && bt != VT_LLONG)
        tcc_error("bitfields must have scalar type");
      bsize = size * 8;
      if (bit_size > bsize)
      {
        tcc_error("width of '%s' exceeds its type", get_tok_str(v, NULL));
      }
      else if (bit_size == bsize && !sb->ad->a.packed && !ad1.a.packed)
      {
        /* no need for bit fields */
        ;
      }
      else if (bit_size == 64)
      {
        tcc_error("field width 64 not implemented");
      }
      else
      {
        type1.t = (type1.t & ~VT_STRUCT_MASK) | VT_BITFIELD | ((unsigned)bit_size << (VT_STRUCT_SHIFT + 6));
      }
    }
    if (v != 0 || (type1.t & VT_BTYPE) == VT_STRUCT)
    {
      /* Remember we've seen a real field to check
         for placement of flexible array member. */
      sb->c = 1;
    }
    /* If member is a struct or bit-field, enforce
       placing into the struct (as anonymous).  */
    if (v == 0 && ((type1.t & VT_BTYPE) == VT_STRUCT || bit_size >= 0))
    {
      v = anon_sym++;
    }
    if (v)
    {
      ss = sym_push(v | SYM_FIELD, &type1, 0, 0);
      ss->a = ad1.a;
      if (ad1.attr_deprecated) /* -Wdeprecated-declarations at each use */
        attr_apply_decl_facts(ss, &ad1);
      *sb->ps = ss;
      sb->ps = &ss->next;
      if (yasc_field_bnb > 0)
      {
        /* resolved after the layout: the names may come later */
        sb->fb = tcc_realloc(sb->fb, (sb->nfb + 1) * sizeof *sb->fb);
        sb->fb[sb->nfb].member = ss;
        sb->fb[sb->nfb].nb = yasc_field_bnb;
        sb->fb[sb->nfb].st = yasc_field_bstatic;
        memcpy(sb->fb[sb->nfb].names, yasc_field_bnames, yasc_field_bnb * sizeof(int));
        sb->nfb++;
        yasc_field_bnb = 0;
      }
    }
    if (tok == ';' || tok == '}' || tok == TOK_EOF)
      break;
    skip(',');
  }
  if (tok == ';')
    next();
  else if (tok != '}')
    skip(';');
}

void struct_decl(CType *type, int u)
{
  int v, bt, ut;
  Sym *s, *ss, **ps;
  AttributeDef ad, ad1;
  CType type1, btype;
  PendingFieldBorrows *fb = NULL;
  int nfb = 0;

  memset(&ad, 0, sizeof ad);
  next();
  parse_decl_attributes(&ad);
  const int tag_line = file ? file->line_num : 0; /* "declared here" for a deprecated tag */

  v = 0;
  if (tok >= TOK_IDENT) /* struct/enum tag */
    v = tok, next();
  /* yasc `struct T borrows(text) { ... }` (the names it borrows from) or,
     where the type is used, `struct T borrows(src)` (what this one does) */
  int bnames[4], bnb = 0, bstatic = 0, bwritten = 0;
  if (tcc_state->yasc && u != VT_ENUM && tok == TOK_borrows)
  {
    bwritten = 1;
    next();
    skip('(');
    for (;;)
    {
      if (tok == TOK_STATIC)
        bstatic = 1;
      else if (tok >= TOK_UIDENT && bnb < 4)
        bnames[bnb++] = tok;
      else
        expect("a name or 'static'");
      next();
      if (tok != ',')
        break;
      next();
    }
    skip(')');
    if (tok != '{')
    {
      yasc_sbind_pending = 1;
      yasc_sbind_static = bstatic;
      yasc_sbind_nb = bnb;
      memcpy(yasc_sbind_names, bnames, bnb * sizeof(int));
      bwritten = 0;
    }
    else if (bstatic || !bnb)
      tcc_error("a struct borrows from names it is given: borrows(text)");
  }

  bt = ut = 0;
  if (u == VT_ENUM)
  {
    ut = VT_INT;
    if (tok == ':')
    { /* C2x enum : <type> ... */
      next();
      if (!parse_btype(&btype, &ad1, 0) || !is_integer_btype(btype.t & VT_BTYPE))
        expect("enum type");
      bt = ut = btype.t & (VT_BTYPE | VT_LONG | VT_UNSIGNED | VT_DEFSIGN);
    }
  }

  if (v)
  {
    /* struct already defined ? return it */
    s = struct_find(v);
    if (s && (s->sym_scope == local_scope || (tok != '{' && tok != ';')))
    {
      if (u == s->type.t)
        goto do_decl;
      if (u == VT_ENUM && IS_ENUM(s->type.t)) /* XXX: check integral types */
        goto do_decl;
      tcc_error("redeclaration of '%s'", get_tok_str(v, NULL));
    }
  }
  else
  {
    if (tok != '{')
      expect("struct/union/enum name");
    v = anon_sym++;
  }
  /* Record the original enum/struct/union token.  */
  type1.t = u | ut;
  type1.ref = NULL;
  /* we put an undefined size for struct/union */
  s = sym_push(v | SYM_STRUCT, &type1, 0, bt ? 0 : -1);
  s->r = 0; /* default alignment is zero as gcc */
do_decl:
  type->t = s->type.t;
  type->ref = s;
  if (s->a.attr_facts && tok != '{' && tok != ';')
    attr_deprecated_use(s); /* `struct S` names a deprecated tag */
  merge_symattr(&s->a, &ad.a);
  if (bwritten && tok == '{')
  {
    struct_params = tcc_realloc(struct_params, (nb_struct_params + 1) * sizeof *struct_params);
    struct_params[nb_struct_params].s = s;
    struct_params[nb_struct_params].nb = bnb;
    memcpy(struct_params[nb_struct_params].names, bnames, bnb * sizeof(int));
    nb_struct_params++;
  }

  if (tok == '{')
  {
    if ((tcc_state->warn_cross_tu & WARN_ON) && !local_stack && u != VT_ENUM)
      tcc_typesig_note(s->v, tag_line, 1); /* -Wcross-tu: where its layout is */
    next();
    struct_body_depth++;
    if (s->c != -1 && !(u == VT_ENUM && s->c == 0)) /* not yet defined typed enum */
      tcc_error("struct/union/enum already defined");
    s->c = -2;
    if (tcc_state->yasc_module && yasc_in_main_file())
    {
      s->ymod |= YASC_MODULE_OWN;
      if (yasc_exporting)
        yasc_export_tag(s);
    }
    /* cannot be empty */
    /* non empty enums are not allowed */
    ps = &s->next;
    Sym *const members_after = global_stack; /* the members are pushed above it */
    if (u == VT_ENUM)
    {
      long long ll = 0, pl = 0, nl = 0;
      CType t;
      t.ref = s;
      /* enum symbols have static storage */
      t.t = VT_INT | VT_STATIC | VT_ENUM_VAL;
      if (bt)
        t.t = bt | VT_STATIC | VT_ENUM_VAL;
      for (;;)
      {
        v = tok;
        if (v < TOK_UIDENT)
          expect("identifier");
        ss = sym_find(v);
        if (ss && !local_stack)
          tcc_error("redefinition of enumerator '%s'", get_tok_str(v, NULL));
        next();
        /* `A __attribute__((deprecated)) = 1`, `A [[deprecated]]` */
        AttributeDef ead;
        int ead_set = tok == TOK_ATTRIBUTE1 || tok == TOK_ATTRIBUTE2 || tok == '[';
        if (ead_set)
        {
          memset(&ead, 0, sizeof ead);
          parse_decl_attributes(&ead);
        }
        if (tok == '=')
        {
          next();
          ll = expr_const64();
        }
        ss = sym_push(v, &t, VT_CONST, 0);
        if (ead_set)
          attr_apply_decl_facts(ss, &ead);
        sym_set_enum_val(ss, ll);
        *ps = ss, ps = &ss->next;
        if (ll < nl)
          nl = ll;
        if (ll > pl)
          pl = ll;
        if (tok != ',')
          break;
        next();
        ll++;
        /* NOTE: we accept a trailing comma */
        if (tok == '}')
          break;
      }
      struct_body_depth--;
      skip('}');

      if (bt)
      {
        t.t = bt;
        s->c = 2;
        goto enum_done;
      }

      /* set integral type of the enum */
      t.t = VT_INT;
      if (nl >= 0)
      {
        if (pl != (unsigned)pl)
          t.t = (LONG_SIZE == 8 ? VT_LLONG | VT_LONG : VT_LLONG);
        t.t |= VT_UNSIGNED;
      }
      else if (pl != (int)pl || nl != (int)nl)
        t.t = (LONG_SIZE == 8 ? VT_LLONG | VT_LONG : VT_LLONG);

      /* set type for enum members */
      for (ss = s->next; ss; ss = ss->next)
      {
        ll = sym_enum_val(ss);
        if (ll == (int)ll) /* default is int if it fits */
          continue;
        if (t.t & VT_UNSIGNED)
        {
          ss->type.t |= VT_UNSIGNED;
          if (ll == (unsigned)ll)
            continue;
        }
        ss->type.t = (ss->type.t & ~VT_BTYPE) | (LONG_SIZE == 8 ? VT_LLONG | VT_LONG : VT_LLONG);
      }
      s->c = 1;
    enum_done:
      s->type.t = type->t = t.t | VT_ENUM;
    }
    else
    {
      const int with_methods = tcc_state->yasc;
      if (with_methods)
        yasc_struct_body_begin();
      StructBody sb;
      sb.s = s;
      sb.ps = ps;
      sb.ad = &ad;
      sb.fb = NULL;
      sb.u = u;
      sb.c = 0;
      sb.flexible = 0;
      sb.nfb = 0;
      sb.with_methods = with_methods;
      struct_body_recovering(struct_member, &sb);
      fb = sb.fb;
      nfb = sb.nfb;
      if (with_methods)
        yasc_struct_body_end();
      struct_body_depth--;
      skip('}');
      parse_attribute(&ad);
      if (ad.cleanup_func)
      {
        tcc_warning("attribute '__cleanup__' ignored on type");
      }
      check_fields(type, 1);
      check_fields(type, 0);
      merge_symattr(&type->ref->a, &ad.a);
      struct_layout(type, &ad);
      if (nfb)
        yasc_resolve_field_borrows(type->ref, fb, nfb);
      tcc_free(fb);
      fb = NULL;
      nfb = 0;
      yasc_methods_struct_done(s);
      if (s->ymod & YASC_EXPORT)
        yasc_check_export_members(s);
      if (debug_modes)
        tcc_debug_fix_anon(tcc_state, type);
      else
        sym_params_track(s, members_after); /* a file-scope struct's members spill like parameters */
    }
  }
  /* deprecated on the tag: after its body, which may name it */
  attr_apply_decl_facts_at(s, &ad, tag_line);
}
