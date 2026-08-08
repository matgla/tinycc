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

/* assign_check.c -- Assignment-cast checking and pointer-compatibility diagnostics.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Recursively mark value (non-padding) bytes of TYPE at BASE offset into MAP.
 * MAP is a byte array of MAP_SIZE bytes; map[i]=1 means "value byte", 0 means
 * "padding".  Caller pre-zeroes MAP.  Returns 0 on success, -1 if the type
 * contains a VLA member or other structure we can't statically analyze (in
 * which case the caller should not emit any clear-padding stores). */
int mark_value_bytes(CType *type, int base, unsigned char *map, int map_size)
{
  int align, size;
  int bt = type->t & VT_BTYPE;

  /* VLA member inside a struct: layout can't be statically determined.
   * The top-level VLA pointer case is handled by the caller (size<=0). */
  if (type->t & VT_VLA)
    return -1;

  /* Bitfield: mark the storage-unit bytes the field touches as value bytes.
   * Bits not used by this bitfield but inside the same byte may be used by
   * an adjacent bitfield, so we conservatively never clear those bytes. */
  if (type->t & VT_BITFIELD)
  {
    int bpos = BIT_POS(type->t);
    int bsize_bits = BIT_SIZE(type->t);
    int span = (bpos + bsize_bits + 7) / 8;
    for (int i = 0; i < span; i++)
    {
      int idx = base + i;
      if (idx >= 0 && idx < map_size)
        map[idx] = 1;
    }
    return 0;
  }

  size = type_size(type, &align);
  if (size <= 0)
    return 0;

  if (bt == VT_STRUCT)
  {
    Sym *sref = type->ref;
    if (!sref)
      return -1;
    for (Sym *f = sref->next; f; f = f->next)
    {
      int rc = mark_value_bytes(&f->type, base + f->c, map, map_size);
      if (rc < 0)
        return rc;
    }
    return 0;
  }

  if ((type->t & VT_ARRAY) && bt == VT_PTR)
  {
    Sym *sref = type->ref;
    int nelem = sref->c;
    if (nelem <= 0)
      return 0;
    int eal;
    int esize = type_size(&sref->type, &eal);
    if (esize <= 0)
      return 0;
    for (int i = 0; i < nelem; i++)
    {
      int rc = mark_value_bytes(&sref->type, base + i * esize, map, map_size);
      if (rc < 0)
        return rc;
    }
    return 0;
  }

  /* Scalar, pointer, function pointer, etc.: every byte is a value byte. */
  for (int i = 0; i < size; i++)
  {
    int idx = base + i;
    if (idx >= 0 && idx < map_size)
      map[idx] = 1;
  }
  return 0;
}

/* modify type so that its it is a pointer to type. */
ST_FUNC void mk_pointer(CType *type)
{
  Sym *s;
  s = sym_push(SYM_FIELD, type, 0, -1);
  type->t = VT_PTR | (type->t & VT_STORAGE);
  type->ref = s;
}

/* return true if type1 and type2 are exactly the same (including
   qualifiers).
*/
int is_compatible_types(CType *type1, CType *type2)
{
  return compare_types(type1, type2, 0);
}

/* return true if type1 and type2 are the same (ignoring qualifiers).
 */
int is_compatible_unqualified_types(CType *type1, CType *type2)
{
  return compare_types(type1, type2, 1);
}

void cast_error(CType *st, CType *dt)
{
  type_incompatibility_error(st, dt, "cannot convert '%s' to '%s'");
}

/* verify type compatibility to store vtop in 'dt' type */
void verify_assign_cast(CType *dt)
{
  CType *st, *type1, *type2;
  int dbt, sbt, qualwarn, lvl;

  st = &vtop->type; /* source type */
  dbt = dt->t & VT_BTYPE;
  sbt = st->t & VT_BTYPE;
  if (dt->t & VT_CONSTANT)
    tcc_warning("assignment of read-only location");
  switch (dbt)
  {
  case VT_VOID:
    if (sbt != dbt)
      tcc_error("assignment to void expression");
    break;
  case VT_PTR:
    /* special cases for pointers */
    /* '0' can also be a pointer */
    if (is_null_pointer(vtop))
      break;
    /* accept implicit pointer to integer cast with warning */
    if (is_integer_btype(sbt))
    {
      tcc_warning("assignment makes pointer from integer without a cast");
      break;
    }
    type1 = pointed_type(dt);
    if (sbt == VT_PTR)
      type2 = pointed_type(st);
    else if (sbt == VT_FUNC)
      type2 = st; /* a function is implicitly a function pointer */
    else
      goto error;
    if (is_compatible_types(type1, type2))
      break;
    for (qualwarn = lvl = 0;; ++lvl)
    {
      if (((type2->t & VT_CONSTANT) && !(type1->t & VT_CONSTANT)) ||
          ((type2->t & VT_VOLATILE) && !(type1->t & VT_VOLATILE)))
        qualwarn = 1;
      dbt = type1->t & (VT_BTYPE | VT_LONG);
      sbt = type2->t & (VT_BTYPE | VT_LONG);
      if (dbt != VT_PTR || sbt != VT_PTR)
        break;
      type1 = pointed_type(type1);
      type2 = pointed_type(type2);
    }
    if (!is_compatible_unqualified_types(type1, type2))
    {
      if ((dbt == VT_VOID || sbt == VT_VOID) && lvl == 0)
      {
        /* void * can match anything */
      }
      else if (dbt == sbt && is_integer_btype(sbt & VT_BTYPE) &&
               IS_ENUM(type1->t) + IS_ENUM(type2->t) + !!((type1->t ^ type2->t) & VT_UNSIGNED) < 2)
      {
        /* Like GCC don't warn by default for merely changes
           in pointer target signedness.  Do warn for different
           base types, though, in particular for unsigned enums
           and signed int targets.  */
      }
      else
      {
        tcc_warning("assignment from incompatible pointer type");
        break;
      }
    }
    if (qualwarn)
      tcc_warning_c(warn_discarded_qualifiers)("assignment discards qualifiers from pointer target type");
    break;
  case VT_BYTE:
  case VT_SHORT:
  case VT_INT:
  case VT_LLONG:
    if (sbt == VT_PTR || sbt == VT_FUNC)
    {
      tcc_warning("assignment makes integer from pointer without a cast");
    }
    else if (sbt == VT_STRUCT)
    {
      goto case_VT_STRUCT;
    }
    /* XXX: more tests */
    break;
  case VT_STRUCT:
  case_VT_STRUCT:
    if (is_transparent_union_type(dt) && find_assignable_transparent_union_member(dt))
      break;
    /* Allow reinterpret assignment/cast between GCC vector types of the
     * same total byte size (e.g. v4si <-> v4ui, v8hi <-> v4si). */
    if ((dt->t & VT_VECTOR) && (st->t & VT_BTYPE) == VT_STRUCT && (st->t & VT_VECTOR) && dt->ref->c == st->ref->c)
      break;
    if (!is_compatible_unqualified_types(dt, st))
    {
    error:
      cast_error(st, dt);
    }
    break;
  }
}

void gen_assign_cast(CType *dt)
{
  verify_assign_cast(dt);
  gen_cast(dt);
}
