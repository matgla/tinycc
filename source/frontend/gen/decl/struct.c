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

  for (f = type->ref->next; f; f = f->next)
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
          int ofs = ((c * 8 + bit_pos) % a8 + bit_size + a8 - 1) / a8;
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
    Sym *group_start = NULL;
    int group_start_off = 0;
    int group_end_off = 0; /* exclusive: first byte outside the group */
    int group_unit_bits = 0;
    int group_base_type = VT_BYTE;

    for (f = type->ref->next; f; f = f->next)
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
          Sym *g;
          int ubytes = group_unit_bits / 8;
          for (g = group_start; g != f; g = g->next)
          {
            if (!(g->type.t & VT_BITFIELD) || BIT_SIZE(g->type.t) == 0)
              continue;
            int abs_bp = (g->c - group_start_off) * 8 + BIT_POS(g->type.t);
            int bs = BIT_SIZE(g->type.t);
            int be_bp = group_unit_bits - abs_bp - bs;
            g->c = group_start_off;
            g->type.t = (g->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | (be_bp << VT_STRUCT_SHIFT);
            g->type.ref = g;
            g->a.sso_be = 1;
            g->r = ubytes;
            if ((g->type.t & VT_BTYPE) != group_base_type)
              g->auxtype = group_base_type;
            else
              g->auxtype = -1;
          }
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
      Sym *g;
      int ubytes = group_unit_bits / 8;
      for (g = group_start; g; g = g->next)
      {
        if (!(g->type.t & VT_BITFIELD) || BIT_SIZE(g->type.t) == 0)
          continue;
        int abs_bp = (g->c - group_start_off) * 8 + BIT_POS(g->type.t);
        int bs = BIT_SIZE(g->type.t);
        int be_bp = group_unit_bits - abs_bp - bs;
        g->c = group_start_off;
        g->type.t = (g->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | (be_bp << VT_STRUCT_SHIFT);
        g->type.ref = g;
        g->a.sso_be = 1;
        g->r = ubytes;
        if ((g->type.t & VT_BTYPE) != group_base_type)
          g->auxtype = group_base_type;
        else
          g->auxtype = -1;
      }
    }
  }

  /* check whether we can access bitfields by their type */
  for (f = type->ref->next; f; f = f->next)
  {
    int s, px, cx, c0;
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
      px = f->c * 8 + bit_pos;
      cx = (px >> 3) & -align;
      px = px - (cx << 3);
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
      bit_pos = px;
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
void struct_decl(CType *type, int u)
{
  int v, c, size, align, flexible;
  int bit_size, bsize, bt, ut;
  Sym *s, *ss, **ps;
  AttributeDef ad, ad1;
  CType type1, btype;

  memset(&ad, 0, sizeof ad);
  next();
  parse_attribute(&ad);

  v = 0;
  if (tok >= TOK_IDENT) /* struct/enum tag */
    v = tok, next();

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
  merge_symattr(&s->a, &ad.a);

  if (tok == '{')
  {
    next();
    if (s->c != -1 && !(u == VT_ENUM && s->c == 0)) /* not yet defined typed enum */
      tcc_error("struct/union/enum already defined");
    s->c = -2;
    /* cannot be empty */
    /* non empty enums are not allowed */
    ps = &s->next;
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
        if (tok == '=')
        {
          next();
          ll = expr_const64();
        }
        ss = sym_push(v, &t, VT_CONST, 0);
        ss->enum_val = ll;
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
        ll = ss->enum_val;
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
      c = 0;
      flexible = 0;
      while (tok != '}')
      {
        if (!parse_btype(&btype, &ad1, 0))
        {
          if (tok == TOK_STATIC_ASSERT)
          {
            do_Static_assert();
            continue;
          }
          skip(';');
          continue;
        }
        while (1)
        {
          if (flexible)
            tcc_error("flexible array member '%s' not at the end of struct", get_tok_str(v, NULL));
          bit_size = -1;
          v = 0;
          type1 = btype;
          if (tok != ':')
          {
            if (tok != ';')
              type_decl(&type1, &ad1, &v, TYPE_DIRECT);
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
              if ((u == VT_STRUCT) && (type1.t & VT_ARRAY) && c)
                flexible = 1;
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
            else if (bit_size == bsize && !ad.a.packed && !ad1.a.packed)
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
            c = 1;
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
            *ps = ss;
            ps = &ss->next;
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
      if (debug_modes)
        tcc_debug_fix_anon(tcc_state, type);
    }
  }
}
