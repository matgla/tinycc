/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
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

#include "tccdebug.h"

static const char *tcc_debug_vt_valmask_name(unsigned short r)
{
  const int v = r & VT_VALMASK;
  switch (v)
  {
  case VT_CONST:
    return "CONST";
  case VT_LLOCAL:
    return "LLOCAL";
  case VT_LOCAL:
    return "LOCAL";
  case VT_CMP:
    return "CMP";
  case VT_JMP:
    return "JMP";
  case VT_JMPI:
    return "JMPI";
  default:
    if (v < VT_CONST)
      return "REG";
    return "UNK";
  }
}

static void tcc_debug_print_r_mods(FILE *f, unsigned short r)
{
  if (!f)
    f = stderr;

  int first = 1;
  if (r & VT_LVAL)
  {
    fprintf(f, "%sLVAL", first ? "" : "|");
    first = 0;
  }
  if (r & VT_PARAM)
  {
    fprintf(f, "%sPARAM", first ? "" : "|");
    first = 0;
  }
  if (r & VT_SYM)
  {
    fprintf(f, "%sSYM", first ? "" : "|");
    first = 0;
  }
  if (r & VT_MUSTCAST)
  {
    fprintf(f, "%sMUSTCAST", first ? "" : "|");
    first = 0;
  }
  if (r & VT_NONCONST)
  {
    fprintf(f, "%sNONCONST", first ? "" : "|");
    first = 0;
  }
  if (r & VT_MUSTBOUND)
  {
    fprintf(f, "%sMUSTBOUND", first ? "" : "|");
    first = 0;
  }
  if (r & VT_BOUNDED)
  {
    fprintf(f, "%sBOUNDED", first ? "" : "|");
    first = 0;
  }
  if (first)
    fprintf(f, "-");
}

static void tcc_debug_print_r_info(FILE *f, unsigned short r)
{
  if (!f)
    f = stderr;

  const int valmask = r & VT_VALMASK;
  fprintf(f, "loc=%s", tcc_debug_vt_valmask_name(r));
  if (valmask < VT_CONST)
    fprintf(f, "(%d)", valmask);
  else
    fprintf(f, "(0x%02x)", valmask);
  fprintf(f, ", mods=");
  tcc_debug_print_r_mods(f, r);
}

static const char *tcc_debug_btype_name(int bt)
{
  switch (bt)
  {
  case VT_VOID:
    return "void";
  case VT_BYTE:
    return "byte";
  case VT_SHORT:
    return "short";
  case VT_INT:
    return "int";
  case VT_LLONG:
    return "long long";
  case VT_PTR:
    return "ptr";
  case VT_FUNC:
    return "func";
  case VT_STRUCT:
    return "struct";
  case VT_FLOAT:
    return "float";
  case VT_DOUBLE:
    return "double";
  case VT_LDOUBLE:
    return "long double";
  case VT_BOOL:
    return "bool";
  case VT_QLONG:
    return "qlong";
  case VT_QFLOAT:
    return "qfloat";
  default:
    return "unknown";
  }
}

static void tcc_debug_print_ctype(FILE *f, const CType *ct)
{
  if (!f)
    f = stderr;
  if (!ct)
  {
    fprintf(f, "<nulltype>");
    return;
  }

  const int vt = ct->t;
  const int bt = vt & VT_BTYPE;

  if (vt & VT_UNSIGNED)
    fprintf(f, "unsigned ");
  if (vt & VT_LONG)
    fprintf(f, "long ");
  fprintf(f, "%s", tcc_debug_btype_name(bt));
  if (vt & VT_ARRAY)
    fprintf(f, "[]");
  if (vt & VT_VLA)
    fprintf(f, "(vla)");
  if (vt & VT_BITFIELD)
    fprintf(f, "(bitfield)");
  if (vt & VT_CONSTANT)
    fprintf(f, " const");
  if (vt & VT_VOLATILE)
    fprintf(f, " volatile");
}

static void tcc_debug_print_symattr(FILE *f, const struct SymAttr *a)
{
  if (!f)
    f = stderr;
  if (!a)
  {
    fprintf(f, "<nullattr>");
    return;
  }

  int first = 1;
  if (a->aligned)
  {
    fprintf(f, "%saligned=%u", first ? "" : "|", (unsigned)a->aligned);
    first = 0;
  }
  if (a->packed)
  {
    fprintf(f, "%spacked", first ? "" : "|");
    first = 0;
  }
  if (a->weak)
  {
    fprintf(f, "%sweak", first ? "" : "|");
    first = 0;
  }
  if (a->visibility)
  {
    fprintf(f, "%svis=%u", first ? "" : "|", (unsigned)a->visibility);
    first = 0;
  }
  if (a->dllexport)
  {
    fprintf(f, "%sdllexport", first ? "" : "|");
    first = 0;
  }
  if (a->dllimport)
  {
    fprintf(f, "%sdllimport", first ? "" : "|");
    first = 0;
  }
  if (a->nodecorate)
  {
    fprintf(f, "%snodecorate", first ? "" : "|");
    first = 0;
  }
  if (a->addrtaken)
  {
    fprintf(f, "%saddrtaken", first ? "" : "|");
    first = 0;
  }
  if (a->nodebug)
  {
    fprintf(f, "%snodebug", first ? "" : "|");
    first = 0;
  }
  if (a->naked)
  {
    fprintf(f, "%snaked", first ? "" : "|");
    first = 0;
  }

  if (first)
    fprintf(f, "-");
}

void tcc_debug_print_svalue(const SValue *sv)
{
  if (!sv)
  {
    fprintf(stderr, "SValue(NULL)\n");
    return;
  }

  const unsigned short r = sv->r;
  const int vt = (int)sv->type.t;
  const int bt = vt & VT_BTYPE;
  const int valmask = r & VT_VALMASK;

  fprintf(stderr, "SValue{ ");
  tcc_debug_print_r_info(stderr, r);

  /* Location payload. */
  if (valmask == VT_CONST)
    fprintf(stderr, ", c=%lld", (long long)sv->c.i);
  else if (valmask == VT_LOCAL || valmask == VT_LLOCAL)
    fprintf(stderr, ", off=%d", (int)sv->c.i);

  fprintf(stderr, ", type=");
  if (vt & VT_UNSIGNED)
    fprintf(stderr, "unsigned ");
  if (vt & VT_LONG)
    fprintf(stderr, "long ");
  fprintf(stderr, "%s", tcc_debug_btype_name(bt));
  if (vt & VT_PTR)
    fprintf(stderr, "*");
  if (vt & VT_ARRAY)
    fprintf(stderr, "[]");
  if (vt & VT_VLA)
    fprintf(stderr, "(vla)");
  if (vt & VT_BITFIELD)
    fprintf(stderr, "(bitfield)");
  if (vt & VT_CONSTANT)
    fprintf(stderr, " const");
  if (vt & VT_VOLATILE)
    fprintf(stderr, " volatile");

  fprintf(stderr, ", vr=%d, pr0=%u, pr1=%u", sv->vr, (unsigned)sv->pr0, (unsigned)sv->pr1);
  fprintf(stderr, " }\n");
}

void tcc_debug_print_sym(const Sym *s)
{
  if (!s)
  {
    fprintf(stderr, "Sym(NULL)\n");
    return;
  }

  const char *name = NULL;
  /* get_tok_str is safe for debug printing; pass NULL for non-constant tokens. */
  name = get_tok_str(s->v & ~SYM_FIELD, NULL);

  fprintf(stderr, "Sym{ v=%d", s->v);
  if (name)
    fprintf(stderr, "('%s')", name);
  fprintf(stderr, ", r={");
  tcc_debug_print_r_info(stderr, (unsigned short)s->r);
  fprintf(stderr, "} (0x%04x)", (unsigned)s->r);
  fprintf(stderr, ", vreg=%d", s->vreg);

  fprintf(stderr, ", type=");
  tcc_debug_print_ctype(stderr, &s->type);

  fprintf(stderr, ", attr=");
  tcc_debug_print_symattr(stderr, &s->a);

  /* Useful linkage pointers when debugging scopes/fields. */
  fprintf(stderr, ", next=%p, prev=%p, prev_tok=%p", (void *)s->next, (void *)s->prev, (void *)s->prev_tok);
  fprintf(stderr, " }\n");
}
