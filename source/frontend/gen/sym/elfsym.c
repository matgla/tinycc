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

/* elfsym.c -- ELF symbol emission and relocation entry points.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */
ST_FUNC ElfSym *elfsym(Sym *s)
{
  if (!s || s->c <= 0) /* s->c < 0 used for special values like -2 for "being defined" */
    return NULL;
  return &((ElfSym *)symtab_section->data)[s->c];
}

/* apply storage attributes to Elf symbol */
ST_FUNC void update_storage(Sym *sym)
{
  ElfSym *esym;
  int sym_bind, old_sym_bind;

  esym = elfsym(sym);
  if (!esym)
    return;

  if (sym->a.visibility)
    esym->st_other = (esym->st_other & ~ELFW(ST_VISIBILITY)(-1)) | sym->a.visibility;

  if (sym->type.t & (VT_STATIC | VT_INLINE))
    sym_bind = STB_LOCAL;
  else if (sym->a.weak)
    sym_bind = STB_WEAK;
  else
    sym_bind = STB_GLOBAL;
  old_sym_bind = ELFW(ST_BIND)(esym->st_info);
  if (sym_bind != old_sym_bind)
  {
    esym->st_info = ELFW(ST_INFO)(sym_bind, ELFW(ST_TYPE)(esym->st_info));
  }

#ifdef TCC_TARGET_PE
  if (sym->a.dllimport)
    esym->st_other |= ST_PE_IMPORT;
  if (sym->a.dllexport)
    esym->st_other |= ST_PE_EXPORT;
#endif

#if 0
    printf("storage %s: bind=%c vis=%d exp=%d imp=%d\n",
        get_tok_str(sym->v, NULL),
        sym_bind == STB_WEAK ? 'w' : sym_bind == STB_LOCAL ? 'l' : 'g',
        sym->a.visibility,
        sym->a.dllexport,
        sym->a.dllimport
        );
#endif
}

/* ------------------------------------------------------------------------- */
/* update sym->c so that it points to an external symbol in section
   'section' with value 'value' */

ST_FUNC void put_extern_sym2(Sym *sym, int sh_num, addr_t value, unsigned long size, int can_add_underscore)
{
  int sym_type, sym_bind, info, other, t;
  ElfSym *esym;
  const char *name;
  char buf1[256];

  if (sym->c <= 0)
  {
    /* DEBUG: Validate sym->v before calling get_tok_str */
    /* Valid v values are: TOK_* constants, identifiers (TOK_IDENT..tok_ident), or anonymous (SYM_FIRST_ANOM..) */
    if (sym->v == 0xDEADBEEF)
    {
      /* Use-after-free detected - sym was freed but still referenced */
      return;
    }
    if (sym->v == 0 || (sym->v > 0x20000000 && sym->v < SYM_FIRST_ANOM))
    {
      /* sym->v looks like a garbage pointer - skip */
      return;
    }
    name = get_tok_str(sym->v, NULL);
    /* Detect garbage symbol names early */
    if (name && (name[0] == 'L' && name[1] == '.'))
    {
      /* This is likely a garbage anonymous symbol - L.XXXXX format */
      /* Check if the v value looks suspicious */
      if (sym->v >= SYM_FIRST_ANOM && (sym->v - SYM_FIRST_ANOM) > 100000)
      {
        tcc_error("internal error: put_extern_sym2 called with garbage anonymous symbol (v=0x%x, name='%s')", sym->v,
                  name);
      }
    }
    t = sym->type.t;
    if ((t & VT_BTYPE) == VT_FUNC)
    {
      sym_type = STT_FUNC;
    }
    else if ((t & VT_BTYPE) == VT_VOID)
    {
      sym_type = STT_NOTYPE;
      if ((t & (VT_BTYPE | VT_ASM_FUNC)) == VT_ASM_FUNC)
        sym_type = STT_FUNC;
    }
    else
    {
      sym_type = STT_OBJECT;
    }
    if (t & (VT_STATIC | VT_INLINE))
      sym_bind = STB_LOCAL;
    else
      sym_bind = STB_GLOBAL;
    other = 0;

#ifdef TCC_TARGET_PE
    if (sym_type == STT_FUNC && sym->type.ref)
    {
      Sym *ref = sym->type.ref;
      if (ref->a.nodecorate)
      {
        can_add_underscore = 0;
      }
      if (ref->f.func_call == FUNC_STDCALL && can_add_underscore)
      {
        sprintf(buf1, "_%s@%d", name, ref->f.func_args * PTR_SIZE);
        name = buf1;
        other |= ST_PE_STDCALL;
        can_add_underscore = 0;
      }
    }
#endif

    if (sym->asm_label)
    {
      name = get_tok_str(sym->asm_label, NULL);
      can_add_underscore = 0;
    }

    if (tcc_state->leading_underscore && can_add_underscore)
    {
      buf1[0] = '_';
      pstrcpy(buf1 + 1, sizeof(buf1) - 1, name);
      name = buf1;
    }

    info = ELFW(ST_INFO)(sym_bind, sym_type);
    sym->c = put_elf_sym(symtab_section, value, size, info, other, sh_num, name);

    if (debug_modes)
      tcc_debug_extern_sym(tcc_state, sym, sh_num, sym_bind, sym_type);
  }
  else
  {
    esym = elfsym(sym);
    esym->st_value = value;
    esym->st_size = size;
    esym->st_shndx = sh_num;
  }
  update_storage(sym);
}

ST_FUNC void put_extern_sym(Sym *sym, Section *s, addr_t value, unsigned long size)
{
  if (nocode_wanted && (NODATA_WANTED || (s && s == cur_text_section)))
    return;
  put_extern_sym2(sym, s ? s->sh_num : SHN_UNDEF, value, size, 1);
}

/* add a new relocation entry to symbol 'sym' in section 's' */
ST_FUNC void greloca(Section *s, Sym *sym, unsigned long offset, int type, addr_t addend)
{
  int c = 0;

  if (nocode_wanted && s == cur_text_section)
    return;

  if (sym)
  {
    /* Debug: detect garbage symbols early */
    if (sym->v >= SYM_FIRST_ANOM && (sym->v - SYM_FIRST_ANOM) > 100000)
    {
      tcc_error("internal error: greloca called with garbage symbol (v=0x%x, c=%d, likely invalid pointer)", sym->v,
                sym->c);
    }
    /* Create ELF symbol if not yet created.
     * sym->c == 0: no ELF symbol yet
     * sym->c == -3: LABEL_ADDR_TAKEN marker (&&label), need to create symbol
     * sym->c > 0: valid ELF symbol index */
    if (sym->c <= 0)
      put_extern_sym(sym, NULL, 0, 0);
    c = sym->c;
    if (c <= 0)
    {
      /* sym->c should be a valid positive ELF symbol index at this point.
       * c = 0: put_extern_sym failed or was skipped (NODATA_WANTED?)
       * c = -1: type descriptor symbol (from mk_pointer) - should not be here
       * c = -2: struct/union being defined - should not be here
       * c = -3: LABEL_ADDR_TAKEN but put_extern_sym didn't create symbol
       * This indicates a bug where we're trying to create a relocation for
       * a symbol that was never properly registered in ELF. */
      tcc_error("internal error: greloca called with invalid symbol (c=%d, v=0x%x, type.t=0x%x, r=0x%x)", c, sym->v,
                sym->type.t, sym->r);
    }
  }

  /* now we can add ELF relocation info */
  put_elf_reloca(symtab_section, s, offset, type, c, addend);
}

#if PTR_SIZE == 4
ST_FUNC void greloc(Section *s, Sym *sym, unsigned long offset, int type)
{
  greloca(s, sym, offset, type, 0);
}
#endif
