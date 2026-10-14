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

  /* Top-level asm that named this symbol's asm label before the C
   * declaration made an asm symbol of that name: the C symbol takes over its
   * ELF entry, as a C declaration of the asm name itself does.  A mere
   * reference leaves what the asm defined alone. */
  if (sym->c <= 0 && sym->asm_label && !IS_ASM_SYM(sym) && sym->v < SYM_FIRST_ANOM)
  {
    Sym *a = sym_find(sym->asm_label);
    while (a && a->sym_scope)
      a = a->prev_tok;
    if (a && a != sym && IS_ASM_SYM(a) && a->c > 0)
    {
      sym->c = a->c;
      if (sh_num == SHN_UNDEF)
        return;
    }
  }

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
        snprintf(buf1, sizeof(buf1), "_%s@%d", name, ref->f.func_args * PTR_SIZE);
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
  /* -fvisibility= covers a C definition that names no visibility of its own.
     Only C definitions get here with a section: tccasm defines its labels
     by patching an SHN_UNDEF symbol, so asm-defined symbols keep theirs, as
     with gcc, whose -fvisibility never reaches the assembler. */
  if (tcc_state->default_visibility && sh_num != SHN_UNDEF && !sym->a.vis_explicit &&
      !(sym->type.t & (VT_STATIC | VT_INLINE)) && !IS_ASM_SYM(sym))
  {
    esym = elfsym(sym);
    esym->st_other = (esym->st_other & ~ELFW(ST_VISIBILITY)(-1)) | tcc_state->default_visibility;
  }
  update_storage(sym);
}

ST_FUNC void put_extern_sym(Sym *sym, Section *s, addr_t value, unsigned long size)
{
  if (tcc_state->check_only)
    return;
  if (nocode_wanted && (NODATA_WANTED || (s && s == cur_text_section)))
    return;
  put_extern_sym2(sym, s ? s->sh_num : SHN_UNDEF, value, size, 1);
}

/* add a new relocation entry to symbol 'sym' in section 's' */
ST_FUNC void greloca(Section *s, Sym *sym, unsigned long offset, int type, addr_t addend)
{
  int c = 0;

  if (tcc_state->check_only || (nocode_wanted && s == cur_text_section))
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

/* ---- r0/r1 aggregate returns: the AAPCS adapters (gfunc_regpair_ret) ---- */

/* The symbol an address of SYM names: SYM itself, or -- for an internal
 * function whose direct calls take its 8-byte aggregate in r0/r1 -- its AAPCS
 * adapter `SYM.aapcs`, made the first time something other than a direct call
 * reaches the function (a function pointer, a table, an alias).  An address
 * taken before anything decided the convention keeps the function AAPCS: no
 * adapter then sits on its indirect calls (Zig's syscall table and vtables
 * come before the bodies). */
ST_FUNC Sym *gfunc_regpair_address_sym(Sym *sym)
{
  if (!sym || (sym->type.t & VT_BTYPE) != VT_FUNC)
    return sym;
  if (sym->a.regpair_ret == REGPAIR_RET_UNKNOWN)
    sym->a.regpair_ret = REGPAIR_RET_NO;
  if (!gfunc_regpair_ret(sym))
    return sym;
  TCCState *s1 = tcc_state;
  for (int i = 0; i < s1->nb_regpair_fns; i++)
    if (s1->regpair_fns[i] == sym)
      return s1->regpair_adapters[i];
  /* The adapter calls the body: it must be emitted, even when every direct
   * call was inlined (a static inline body is generated only when referenced),
   * and the function escapes like any other whose address is taken. */
  sym->a.addrtaken = 1;
  if (sym->c <= 0)
    put_extern_sym(sym, NULL, 0, 0);
  char buf[300];
  snprintf(buf, sizeof(buf), "%s.aapcs", get_tok_str(sym->v, NULL));
  AttributeDef ad;
  memset(&ad, 0, sizeof(ad));
  ad.a.regpair_ret = REGPAIR_RET_NO;
  CType type = sym->type;
  Sym *adapter = external_sym(tok_alloc(buf, strlen(buf))->tok, &type, 0, &ad);
  dynarray_add(&s1->regpair_fns, &s1->nb_regpair_fns, sym);
  dynarray_add(&s1->regpair_adapters, &s1->nb_regpair_adapters, adapter);
  return adapter;
}

/* SYM, a C function, named in asm text (HOW: REGPAIR_ASM_*).  An r0/r1
 * function is reached as from a pointer -- through its adapter -- where the
 * name is an address.  A function not decided yet stays AAPCS for good: asm
 * run while parsing (top-level asm, the bodies generated then) decides first,
 * and the names in the asm of the bodies generated later were recorded when
 * they were saved (gfunc_regpair_note_asm_names).  So asm that defines the
 * name of an r0/r1 function or changes its binding -- which would need the
 * AAPCS body its direct calls do not expect -- is left to asm built some
 * other way, and is an error. */
ST_FUNC Sym *gfunc_regpair_asm_sym(Sym *sym, int how)
{
  if (!sym || (sym->type.t & VT_BTYPE) != VT_FUNC || IS_ASM_SYM(sym))
    return sym;
  if (sym->a.regpair_ret == REGPAIR_RET_UNKNOWN)
    sym->a.regpair_ret = REGPAIR_RET_NO;
  if (sym->a.regpair_ret != REGPAIR_RET_YES || how == REGPAIR_ASM_MENTION)
    return sym;
  if (how == REGPAIR_ASM_DEFINE)
    tcc_error("'%s' returns its aggregate in r0/r1 to its direct calls: asm cannot define it or change its binding",
              get_tok_str(sym->v, NULL));
  return gfunc_regpair_address_sym(sym);
}

/* A redeclaration of FN, already decided r0/r1, merged attributes AD into
 * it: those that need the AAPCS body the earlier calls do not expect are an
 * error.  `used` is fine -- asm reaches the function through its adapter. */
ST_FUNC void gfunc_regpair_check_redecl(Sym *fn)
{
  if ((fn->type.t & VT_BTYPE) == VT_FUNC && fn->a.regpair_ret == REGPAIR_RET_YES &&
      (fn->a.naked || fn->a.weak || !(fn->type.t & VT_STATIC)))
    tcc_error("'%s' redeclared %s after a call that takes its aggregate in r0/r1", get_tok_str(fn->v, NULL),
              fn->a.naked ? "naked" : fn->a.weak ? "weak" : "with external linkage");
}

/* Emit the adapters something refers to (an ELF symbol, or an alias marked
 * it used), once every body is generated and before the end-of-TU garbage
 * collection.  No adapter exists for a function prune_unused_statics dropped:
 * that runs before tu_parsed, and so before any function is r0/r1. */
ST_FUNC void gfunc_regpair_emit_adapters(TCCState *s1)
{
  for (int i = 0; i < s1->nb_regpair_fns; i++)
  {
    Sym *fn = s1->regpair_fns[i], *adapter = s1->regpair_adapters[i];
    ElfSym *esym = adapter->c > 0 ? elfsym(adapter) : NULL;
    if (esym && esym->st_shndx != SHN_UNDEF)
      continue;
    if (adapter->c <= 0 && !adapter->a.used)
      continue;
    /* The adapter's call must reach this TU's body, never bind elsewhere. */
    ElfSym *fesym = fn->c > 0 ? elfsym(fn) : NULL;
    if (!fesym || fesym->st_shndx == SHN_UNDEF)
      tcc_error("internal error: '%s' has an AAPCS adapter but no body", get_tok_str(fn->v, NULL));
    gfunc_regpair_emit_adapter(s1, fn, adapter, function_text_section(s1, adapter));
  }
}

/* Record the names the asm statements of BODY, a function body saved to be
 * generated after the TU is parsed, spell out.  Asm runs only when its body
 * is generated, and a deferred body's runs after functions are decided r0/r1;
 * its `.global f` or `f:` would then need an AAPCS body f no longer has.  So
 * every identifier in such an asm statement's strings keeps a function of
 * that name AAPCS (gfunc_regpair_asm_named) -- the template, operand
 * constraints and asm labels alike, which only ever costs a function the
 * r0/r1 return. */
ST_FUNC void gfunc_regpair_note_asm_names(TokenString *body)
{
  TCCState *s1 = tcc_state;
  const int *p = tok_str_buf(body), *end = p + body->len;
  int depth = -1; /* inside an asm statement's parentheses: their depth */
  CValue cv;
  while (p < end)
  {
    int t;
    tok_get(&t, &p, &cv);
    if (t == TOK_ASM1 || t == TOK_ASM2 || t == TOK_ASM3)
    {
      if (depth < 0)
        depth = 0;
      continue;
    }
    if (depth < 0)
      continue;
    if (t == '(')
      depth++;
    else if (t == ')' && --depth <= 0)
      depth = -1;
    else if (t == TOK_STR && depth > 0)
    {
      const char *str = cv.str.data;
      for (int i = 0, n = cv.str.size; i < n;)
      {
        int c = (unsigned char)str[i];
        if (!(isid(c) || isnum(c) || c == '$'))
        {
          i++;
          continue;
        }
        int j = i;
        while (j < n && (isid((unsigned char)str[j]) || isnum((unsigned char)str[j]) || str[j] == '$'))
          j++;
        if (!isnum(c))
        {
          int k = 0;
          while (k < s1->nb_regpair_asm_names &&
                 (strncmp(s1->regpair_asm_names[k], str + i, j - i) || s1->regpair_asm_names[k][j - i]))
            k++;
          if (k == s1->nb_regpair_asm_names)
          {
            char *name = tcc_malloc(j - i + 1);
            memcpy(name, str + i, j - i);
            name[j - i] = 0;
            dynarray_add(&s1->regpair_asm_names, &s1->nb_regpair_asm_names, name);
          }
        }
        i = j;
      }
    }
  }
}

/* Whether a saved body's asm names FN (gfunc_regpair_note_asm_names). */
ST_FUNC int gfunc_regpair_asm_named(Sym *fn)
{
  TCCState *s1 = tcc_state;
  const char *name = get_tok_str(fn->v, NULL), *label = fn->asm_label ? get_tok_str(fn->asm_label, NULL) : NULL;
  for (int k = 0; k < s1->nb_regpair_asm_names; k++)
    if (!strcmp(s1->regpair_asm_names[k], name) || (label && !strcmp(s1->regpair_asm_names[k], label)))
      return 1;
  return 0;
}

ST_FUNC void gfunc_regpair_reset(TCCState *s1)
{
  dynarray_reset(&s1->regpair_asm_names, &s1->nb_regpair_asm_names);
  tcc_free(s1->regpair_fns);
  tcc_free(s1->regpair_adapters);
  s1->regpair_fns = s1->regpair_adapters = NULL;
  s1->nb_regpair_fns = s1->nb_regpair_adapters = 0;
  s1->regpair_direct_call = NULL;
}
