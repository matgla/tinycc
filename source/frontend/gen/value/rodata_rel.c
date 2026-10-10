/*
 *  TCC - Tiny C Compiler
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

/* rodata_rel.c -- __rodata_relative pointers.
 *
 *   static const struct { const char *__rodata_relative name; int v; } tbl[] = ...;
 *
 * A pointer so qualified does not hold an address but the offset of its target
 * from the start of the module's .rodata, plus one (0 stays NULL).  The linker
 * fills it in (R_ARM_RODATA_OFF), so the word needs no load-time relocation:
 * a const table of them is pure .rodata, shared from flash under
 * -share-rodata, instead of a per-process copy in .data.  Reading one adds
 * the start of .rodata back (__tcc_rodata_base, which the backend serves from
 * the rodata anchor).  The offset is from one module-wide base, not from the
 * word itself, so copying the struct that holds one keeps it valid.
 *
 * The type carries it (VT_RODATA_REL on the pointer), so every read knows:
 * a qualified lvalue becomes the pointer it names when unary() hands it to
 * anything but '&'.  It is set only by a static initializer, to NULL or the
 * address of read-only data in .rodata -- not to a function (on YasOS that is
 * a per-process thunk) nor to writable data.  docs/rodata_relative.md. */

#include "gen_priv.h"

int rodata_rel_qualifier(void)
{
#ifdef TCC_TARGET_ARM
  return VT_RODATA_REL;
#else
  /* No R_ARM_RODATA_OFF: an ordinary pointer. */
  return 0;
#endif
}

void rodata_rel_store_error(void)
{
  tcc_error("a __rodata_relative pointer is set only by the initializer of a static object");
}

/* An address conversion must not lose or invent the qualifier at any level
   below the top: through a 'T *__rodata_relative *' the word is an offset,
   through a 'T **' an address.  void * is opaque, as for any other type. */
void rodata_rel_check_targets(CType *dt, CType *st, CType *src, CType *dst)
{
  for (;;)
  {
    if ((dt->t & VT_BTYPE) == VT_VOID || (st->t & VT_BTYPE) == VT_VOID)
      return;
    if ((dt->t ^ st->t) & VT_RODATA_REL)
      type_incompatibility_error(src, dst, "cannot convert '%s' to '%s' (__rodata_relative differs)");
    if ((dt->t & VT_BTYPE) != VT_PTR || (st->t & VT_BTYPE) != VT_PTR)
      return;
    dt = pointed_type(dt);
    st = pointed_type(st);
  }
}

/* vtop, a __rodata_relative lvalue, becomes the pointer it names:
 *   w = word;  p = w ? __tcc_rodata_base - 1 + w : 0
 * computed as (base - 1 + w) & -(w != 0). */
void rodata_rel_rvalue(void)
{
  CType ptype = vtop->type;
  CType word;
  Sym *base;

  ptype.t &= ~VT_RODATA_REL;
  word.t = VT_INT | VT_UNSIGNED | (vtop->type.t & VT_VOLATILE);
  word.ref = NULL;
  vtop->type = word;
  gv(RC_INT);

  vdup();
  vpushi(0);
  gen_op(TOK_NE);
  vpushi(0);
  vswap();
  gen_op('-');
  vswap();
  {
    CType ct;
    ct.t = VT_BYTE | VT_CONSTANT;
    ct.ref = NULL;
    base = external_global_sym(TOK___tcc_rodata_base, &ct);
  }
  word.t = VT_INT | VT_UNSIGNED;
  vpushsym(&word, base);
  vtop->c.i = -1;
  gen_op('+');
  gen_op('&');
  vtop->type = ptype;
}

/* The static initializer of a __rodata_relative word at SEC+C from vtop. */
void rodata_rel_init(Section *sec, unsigned long c)
{
  unsigned char *ptr = sec->data + c;
  Sym *sym;
  ElfSym *esym;

  if (!(vtop->r & VT_SYM))
  {
    if (vtop->c.i != 0)
      tcc_error("a __rodata_relative pointer is NULL or the address of read-only data");
    write32le(ptr, 0);
    return;
  }
  sym = vtop->sym;
  if ((sym->type.t & VT_BTYPE) == VT_FUNC)
    tcc_error("a __rodata_relative pointer cannot point to a function ('%s')", get_tok_str(sym->v, NULL));
  /* Not yet placed (extern, or a tentative one in COMMON): the linker checks.
     A tentative one ends in .rodata only if it is const. */
  esym = elfsym(sym);
  if (esym && esym->st_shndx == SHN_COMMON)
  {
    CType *tp = &sym->type;
    while ((tp->t & (VT_BTYPE | VT_ARRAY)) == (VT_PTR | VT_ARRAY))
      tp = &tp->ref->type;
    if (!(tp->t & VT_CONSTANT))
      tcc_error("a __rodata_relative pointer must point to read-only data; '%s' is writable", get_tok_str(sym->v, NULL));
  }
  if (esym && esym->st_shndx != SHN_UNDEF && esym->st_shndx != SHN_COMMON && esym->st_shndx != rodata_section->sh_num)
    tcc_error("a __rodata_relative pointer must point into .rodata; '%s' is in %s", get_tok_str(sym->v, NULL),
              esym->st_shndx < tcc_state->nb_sections ? tcc_state->sections[esym->st_shndx]->name : "no section");
  greloc(sec, sym, c, R_ARM_RODATA_OFF);
  write32le(ptr, (uint32_t)vtop->c.i + 1);
}
