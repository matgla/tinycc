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

/* size.c -- Type sizes, AAPCS natural alignment and pointer-target access.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

#ifdef TCC_TARGET_ARM
/* Compute AAPCS "natural alignment" for parameter passing.
 * For composites, this is the max alignment of fundamental data type
 * members.  Crucially, __attribute__((aligned)) on the struct does NOT
 * increase this, and __attribute__((packed)) DOES reduce member alignment
 * to 1.  This alignment determines whether register double-word alignment
 * (even-register rule) applies for function calls and va_arg. */
int compute_aapcs_natural_alignment(const CType *type)
{
  int bt = type->t & VT_BTYPE;
  if (bt != VT_STRUCT)
  {
    int align;
    type_size(type, &align);
    return align > 0 ? align : 1;
  }
  Sym *s = type->ref;
  if (!s)
    return 4;
  int max_align = 1;
  for (Sym *f = s->next; f; f = f->next)
  {
    int member_align;
    if ((f->type.t & VT_BTYPE) == VT_STRUCT)
      member_align = compute_aapcs_natural_alignment(&f->type);
    else if (f->type.t & VT_BITFIELD)
    {
      CType base_type = f->type;
      base_type.t &= ~VT_BITFIELD;
      type_size(&base_type, &member_align);
    }
    else
      type_size(&f->type, &member_align);
    if (f->a.packed || s->a.packed)
      member_align = 1;
    if (member_align > max_align)
      max_align = member_align;
  }
  return max_align;
}
#endif

/* return type size as known at compile time. Put alignment at 'a' */
ST_FUNC int type_size(const CType *type, int *a)
{
  Sym *s;
  int bt;

  bt = type->t & VT_BTYPE;

  /* DONE: Phase 1 - Handle complex types in type_size() */
  if (type->t & VT_COMPLEX)
  {
    if (bt == VT_FLOAT)
    {
      *a = 4;   /* Alignment of float */
      return 8; /* 2 x 4 bytes */
    }
    else if (bt == VT_DOUBLE || bt == VT_LDOUBLE)
    {
      *a = 8;    /* Alignment of double */
      return 16; /* 2 x 8 bytes */
    }
    else
    {
      /* Complex integer types (GCC extension): _Complex char/short/int/long long */
      int base_size, base_align;
      CType base_type;
      base_type.t = bt;
      base_type.ref = NULL;
      base_size = type_size(&base_type, &base_align);
      *a = base_align;
      return 2 * base_size;
    }
  }

  if (bt == VT_STRUCT)
  {
    /* struct/union */
    s = type->ref;
    *a = s->r;
    return s->c;
  }
  else if (bt == VT_PTR)
  {
    if (type->t & VT_ARRAY)
    {
      int ts;
      s = type->ref;
      ts = type_size(&s->type, a);
      if (ts < 0 && s->c < 0)
        ts = -ts;
      return ts * s->c;
    }
    else
    {
      *a = PTR_SIZE;
      return PTR_SIZE;
    }
  }
  else if (IS_ENUM(type->t) && type->ref->c < 0)
  {
    *a = 0;
    return -1; /* incomplete enum */
  }
  else if (bt == VT_LDOUBLE)
  {
    *a = LDOUBLE_ALIGN;
    return LDOUBLE_SIZE;
  }
  else if (bt == VT_DOUBLE || bt == VT_LLONG)
  {
#if (defined TCC_TARGET_I386 && !defined TCC_TARGET_PE) || (defined TCC_TARGET_ARM && !defined TCC_ARM_EABI)
    *a = 4;
#else
    *a = 8;
#endif
    return 8;
  }
  else if (bt == VT_INT || bt == VT_FLOAT)
  {
    *a = 4;
    return 4;
  }
  else if (bt == VT_SHORT)
  {
    *a = 2;
    return 2;
  }
  else if (bt == VT_QLONG || bt == VT_QFLOAT)
  {
    *a = 8;
    return 16;
  }
  else
  {
    /* char, void, function, _Bool */
    *a = 1;
    return 1;
  }
  /* unreachable - all branches above return, but TCC's flow analysis
     needs an explicit return to avoid 'function might return no value' */
  return 0;
}

/* push type size as known at runtime time on top of value stack. Put
   alignment at 'a' */
void vpush_type_size(CType *type, int *a)
{
  if (type->t & VT_VLA)
  {
    type_size(&type->ref->type, a);
    vset(&int_type, VT_LOCAL | VT_LVAL, type->ref->c);
  }
  else if (struct_has_vla_member(type))
  {
    /* Struct with inline VLA member(s): total size = fixed_component +
       sum of all VLA field runtime byte sizes.  The fixed_component
       (type->ref->c) already includes all non-VLA field sizes with
       correct alignment padding from struct_layout(). */
    Sym *f;
    int fixed = type_size(type, a);
    vpushs(fixed);
    for (f = type->ref->next; f; f = f->next)
    {
      if (f->type.t & VT_VLA)
      {
        vset(&int_type, VT_LOCAL | VT_LVAL, f->type.ref->c);
        gen_op('+');
      }
    }
  }
  else
  {
    int size = type_size(type, a);
    if (size < 0)
      tcc_error("unknown type size");
    vpushs(size);
  }
}

/* return the pointed type of t */
CType *pointed_type(CType *type)
{
  return &type->ref->type;
}
