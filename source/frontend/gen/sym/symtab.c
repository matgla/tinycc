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

/* symtab.c -- Symbol allocator, symbol/label stacks and scope lookup.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */
/* symbol allocator */
static Sym *__sym_malloc(void)
{
  Sym *sym_pool, *sym, *last_sym;
  int i;

  sym_pool = tcc_malloc(SYM_POOL_NB * sizeof(Sym));
  dynarray_add(&sym_pools, &nb_sym_pools, sym_pool);

  last_sym = sym_free_first;
  sym = sym_pool;
  for (i = 0; i < SYM_POOL_NB; i++)
  {
    sym->next = last_sym;
    last_sym = sym;
    sym++;
  }
  sym_free_first = last_sym;
  return last_sym;
}

Sym *sym_malloc(void)
{
  Sym *sym;
#ifndef SYM_DEBUG
  sym = sym_free_first;
  if (!sym)
    sym = __sym_malloc();
  sym_free_first = sym->next;
  return sym;
#else
  sym = tcc_malloc(sizeof(Sym));
  return sym;
#endif
}

ST_INLN void sym_free(Sym *sym)
{
  if (sym->const_init_data)
  {
    tcc_free(sym->const_init_data);
    sym->const_init_data = NULL;
  }
#ifndef SYM_DEBUG
  /* Poison freed symbols to detect use-after-free */
  sym->v = 0xDEADBEEF;
  sym->next = sym_free_first;
  sym_free_first = sym;
#else
  tcc_free(sym);
#endif
}

/* push, without hashing */
ST_FUNC Sym *sym_push2(Sym **ps, int v, int t, int c)
{
  Sym *s;

  s = sym_malloc();
  memset(s, 0, sizeof *s);
  s->v = v;
  s->type.t = t;
  s->c = c;
  /* add in stack */
  s->prev = *ps;
  *ps = s;
  return s;
}

/* find a symbol and return its associated structure. 's' is the top
   of the symbol stack */
ST_FUNC Sym *sym_find2(Sym *s, int v)
{
  while (s)
  {
    if (s->v == v)
      return s;
    s = s->prev;
  }
  return NULL;
}

/* structure lookup */
ST_INLN Sym *struct_find(int v)
{
  TokenSym *ts;
  v -= TOK_IDENT;
  if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
    return NULL;
  ts = table_ident[v]; /* NULL = lazy builtin, never used as a struct tag */
  return ts ? ts->sym_struct : NULL;
}

/* find an identifier */
ST_INLN Sym *sym_find(int v)
{
  TokenSym *ts;
  v -= TOK_IDENT;
  if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
    return NULL;
  ts = table_ident[v]; /* NULL = lazy builtin, never declared as an identifier */
  return ts ? ts->sym_identifier : NULL;
}

int sym_scope(Sym *s)
{
  int scope;
  if (IS_ENUM_VAL(s->type.t))
    scope = s->type.ref->sym_scope;
  else
    scope = s->sym_scope;
  return scope;
}

int token_stream_references_local_object(const int *p)
{
  while (1)
  {
    int t;
    int bt;
    CValue cv;
    Sym *s;

    tok_get(&t, &p, &cv);
    if (t == TOK_EOF || t == 0)
      break;
    if (t < TOK_IDENT)
      continue;

    s = sym_find(t);
    if (!s || !sym_scope(s))
      continue;

    bt = s->type.t & VT_BTYPE;
    if ((s->type.t & VT_TYPEDEF) || IS_ENUM_VAL(s->type.t) || bt == VT_FUNC)
      continue;

    return 1;
  }

  return 0;
}

/* push a given symbol on the symbol stack */
ST_FUNC Sym *sym_push(int v, CType *type, int r, int c)
{
  Sym *s, **ps;
  TokenSym *ts;
  int vreg = -1;
  /* register local variable at IR code generator, get Vreg number */
  int valmask = r & VT_VALMASK;

  if (r & VT_PARAM)
  {
    /* Create PARAM vreg for ALL parameters, including stack-passed ones */
    vreg = tcc_ir_get_vreg_param(tcc_state->ir);
    if (vreg >= 0)
    {
      IRLiveInterval *iv = tcc_ir_vreg_live_interval(tcc_state->ir, vreg);
      if (iv)
        iv->is_volatile = (type->t & VT_VOLATILE) != 0;
    }
    /* For stack-passed params (VT_LOCAL), c is the stack offset;
     * for register params, c is the parameter index */
    tcc_ir_assign_physical_register(tcc_state->ir, vreg, c, -1, -1);
    /* Store original parameter offset for prolog code generation */
    tcc_ir_set_original_offset(tcc_state->ir, vreg, c);
    /* Mark float/double parameters */
    if (is_float(type->t))
    {
      int is_double = (type->t & VT_BTYPE) == VT_DOUBLE || (type->t & VT_BTYPE) == VT_LDOUBLE;
      tcc_ir_set_float_type(tcc_state->ir, vreg, 1, is_double);
    }
    /* Mark complex parameters - needs register pairs */
    if (type->t & VT_COMPLEX)
      tcc_ir_vreg_type_set_complex(tcc_state->ir, vreg);
    /* Mark long long parameters */
    if ((type->t & VT_BTYPE) == VT_LLONG)
    {
      tcc_ir_set_llong_type(tcc_state->ir, vreg);
    }
  }
  else
  {
    if (((valmask == VT_LOCAL) || (valmask == VT_LLOCAL)) && (r & VT_LVAL) && ((type->t & VT_BTYPE) != VT_STRUCT) &&
        !(type->t & (VT_ARRAY | VT_VLA | VT_COMPLEX)))
    {
      vreg = tcc_ir_get_vreg_var(tcc_state->ir);
      /* Set the variable's stack offset so LEA operations can find it */
      if (vreg >= 0)
      {
        IRLiveInterval *iv = tcc_ir_vreg_live_interval(tcc_state->ir, vreg);
        tcc_ir_assign_physical_register(tcc_state->ir, vreg, c, -1, -1);
        tcc_ir_set_original_offset(tcc_state->ir, vreg, c);
        if (iv)
          iv->is_volatile = (type->t & VT_VOLATILE) != 0;
      }
      /* Mark float/double variables */
      if (is_float(type->t))
      {
        int is_double = (type->t & VT_BTYPE) == VT_DOUBLE || (type->t & VT_BTYPE) == VT_LDOUBLE;
        tcc_ir_set_float_type(tcc_state->ir, vreg, 1, is_double);
      }
      /* Mark complex variables - needs register pairs */
      if (type->t & VT_COMPLEX)
      {
        tcc_ir_vreg_type_set_complex(tcc_state->ir, vreg);
      }
      /* Mark long long variables */
      if ((type->t & VT_BTYPE) == VT_LLONG)
      {
        tcc_ir_set_llong_type(tcc_state->ir, vreg);
      }
    }
  }
  // }
  // r &= ~VT_PARAM;

  if (local_stack)
    ps = &local_stack;
  else
    ps = &global_stack;
  s = sym_push2(ps, v, type->t, c);
  s->type.ref = type->ref;
  s->r = r;
  s->vreg = vreg;
  /* don't record fields or anonymous symbols */
  /* XXX: simplify */
  if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
  {
    /* record symbol in token array (materialize a lazy builtin slot if the
       symbol's name is a builtin token referenced by fixed id) */
    ts = tok_ensure(v & ~SYM_STRUCT);
    if (v & SYM_STRUCT)
      ps = &ts->sym_struct;
    else
      ps = &ts->sym_identifier;
    s->prev_tok = *ps;
    *ps = s;
    s->sym_scope = local_scope;
    if (s->prev_tok && sym_scope(s->prev_tok) == s->sym_scope)
      tcc_error("redeclaration of '%s'", get_tok_str(v & ~SYM_STRUCT, NULL));
  }
  return s;
}

/* push a global identifier */
ST_FUNC Sym *global_identifier_push(int v, int t, int c)
{
  Sym *s, **ps;
  s = sym_push2(&global_stack, v, t, c);
  s->r = VT_CONST | VT_SYM;
  /* don't record anonymous symbol */
  if (v < SYM_FIRST_ANOM)
  {
    ps = &tok_ensure(v)->sym_identifier;
    /* modify the top most local identifier, so that sym_identifier will
       point to 's' when popped; happens when called from inline asm */
    while (*ps != NULL && (*ps)->sym_scope)
      ps = &(*ps)->prev_tok;
    s->prev_tok = *ps;
    *ps = s;
  }
  return s;
}

/* pop symbols until top reaches 'b'.  If KEEP is non-zero don't really
   pop them yet from the list, but do remove them from the token array.  */
ST_FUNC void sym_pop(Sym **ptop, Sym *b, int keep)
{
  Sym *s, *ss, **ps;
  TokenSym *ts;
  int v;

  s = *ptop;
  while (s != b)
  {
    ss = s->prev;
    v = s->v;
    /* remove symbol in token array */
    /* XXX: simplify */
    if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
    {
      ts = table_ident[(v & ~SYM_STRUCT) - TOK_IDENT];
      if (v & SYM_STRUCT)
        ps = &ts->sym_struct;
      else
        ps = &ts->sym_identifier;
      *ps = s->prev_tok;
    }
    if (!keep && s->const_init_data)
    {
      tcc_free(s->const_init_data);
      s->const_init_data = NULL;
    }
    /* Don't free symbols that have been exported to ELF (sym->c != 0)
       as they may still be referenced by IR instructions */
    if (!keep && s->c == 0)
    {
      /* In IR mode the backend may still need Sym pointers (notably for
       * VT_SYM address materialization and relocations). Block-scope extern
       * declarations create temporary Sym copies that can be referenced by IR
       * after the scope ends; freeing them here can lead to missing relocations
       * and loads/stores from address 0 at runtime.
       */
      if (!(tcc_state->ir && (s->r & VT_SYM)))
        sym_free(s);
    }
    s = ss;
  }
  if (!keep)
    *ptop = b;
}

/* label lookup */
ST_FUNC Sym *label_find(int v)
{
  TokenSym *ts;
  v -= TOK_IDENT;
  if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
    return NULL;
  ts = table_ident[v]; /* NULL = lazy builtin, never used as a label */
  return ts ? ts->sym_label : NULL;
}

ST_FUNC Sym *label_push(Sym **ptop, int v, int flags)
{
  Sym *s, **ps;
  s = sym_push2(ptop, v, VT_STATIC, 0);
  s->r = flags;
  s->jnext = -1; /* Initialize to -1 so we know if there's an actual forward goto */
  ps = &table_ident[v - TOK_IDENT]->sym_label;
  if (ptop == &global_label_stack)
  {
    /* modify the top most local identifier, so that
       sym_identifier will point to 's' when popped */
    while (*ps != NULL)
      ps = &(*ps)->prev_tok;
  }
  s->prev_tok = *ps;
  *ps = s;
  return s;
}

/* pop labels until element last is reached. Look if any labels are
   undefined. Define symbols if '&&label' was used. */
ST_FUNC void label_pop(Sym **ptop, Sym *slast, int keep)
{
  Sym *s, *s1;
  for (s = *ptop; s != slast; s = s1)
  {
    s1 = s->prev;
    int addr_taken =
        (s->c == -3 || s->c > 0 || s->a.addrtaken); /* Remember if address was taken before modifying s->c */
    if (s->r == LABEL_DECLARED)
    {
      tcc_warning_c(warn_all)("label '%s' declared but not used", get_tok_str(s->v, NULL));
    }
    else if (s->r == LABEL_FORWARD)
    {
      tcc_error("label '%s' used but not defined", get_tok_str(s->v, NULL));
    }
    else
    {
      if (s->c)
      {
        /* In IR mode, label_pop for local labels runs at scope exit BEFORE
           codegen, so orig_ir_to_code_mapping is NULL.  Defer resolution of
           addr-taken labels by moving them to global_label_stack, which is
           popped AFTER codegen when the mapping is available. */
        if (addr_taken && tcc_state->ir && !tcc_state->ir->orig_ir_to_code_mapping && ptop != &global_label_stack)
        {
          /* Unlink from table_ident now (function scope is ending) */
          if (s->r != LABEL_GONE)
            table_ident[s->v - TOK_IDENT]->sym_label = s->prev_tok;
          s->r = LABEL_GONE;
          /* Create ELF symbol NOW with placeholder value (0) so that
             relocations emitted during codegen reference a valid symbol.
             Use put_extern_sym2 directly to bypass nocode_wanted check.
             After codegen the global label_pop will UPDATE this symbol
             with the correct code offset via orig_ir_to_code_mapping. */
          if (s->c == -3)
            s->c = 0; /* Reset marker so put_extern_sym2 creates new symbol */
          put_extern_sym2(s, cur_text_section->sh_num, 0, 1, 1);
          /* Push onto global_label_stack for deferred value update.
             s->c is now a valid ELF symbol index (> 0). */
          s->prev = global_label_stack;
          global_label_stack = s;
          continue;
        }

        /* Define corresponding symbol for &&label.
           In IR mode, the label position is recorded as an IR instruction index
           (s->jind) BEFORE DCE/IR compaction, so we must translate it using the
           original-index mapping.
           Also set Thumb bit (+1) so computed goto uses correct state.

           Note: s->c can be:
           - -3: LABEL_ADDR_TAKEN marker, need to reset to 0 for put_extern_sym to create symbol
           - > 0: valid ELF symbol index, put_extern_sym will UPDATE the existing symbol */
        if (s->c == -3)
          s->c = 0; /* Reset marker so put_extern_sym creates new symbol */

        if (tcc_state->ir && tcc_state->ir->orig_ir_to_code_mapping && s->jind >= 0 &&
            s->jind < tcc_state->ir->orig_ir_to_code_mapping_size)
        {
          uint32_t off = tcc_state->ir->orig_ir_to_code_mapping[s->jind];
          /* If the instruction at jind was deleted by DSE/optimization, find the next
             valid mapping. The sentinel value 0xFFFFFFFF indicates no instruction. */
          if (off == 0xFFFFFFFF)
          {
            for (int idx = s->jind + 1; idx < tcc_state->ir->orig_ir_to_code_mapping_size; idx++)
            {
              if (tcc_state->ir->orig_ir_to_code_mapping[idx] != 0xFFFFFFFF)
              {
                off = tcc_state->ir->orig_ir_to_code_mapping[idx];
                break;
              }
            }
          }
          put_extern_sym(s, cur_text_section, off + 1, 1);
        }
        else if (tcc_state->ir && tcc_state->ir->ir_to_code_mapping && s->jind >= 0 &&
                 s->jind < tcc_state->ir->ir_to_code_mapping_size)
        {
          /* Backward-compatible fallback for older IR mapping */
          uint32_t off = tcc_state->ir->ir_to_code_mapping[s->jind];
          put_extern_sym(s, cur_text_section, off + 1, 1);
        }
        else
        {
          /* Fallback for non-IR codegen */
          put_extern_sym(s, cur_text_section, s->jnext, 1);
        }
      }
    }
    /* remove label */
    if (s->r != LABEL_GONE)
      table_ident[s->v - TOK_IDENT]->sym_label = s->prev_tok;
    /* Don't free local label symbols whose address was taken (&&label) until
       after IR codegen, as the IR instructions still reference them. The symbol
       will be freed later with global labels after code generation. */
    if (!keep && !addr_taken)
      sym_free(s);
    else
      s->r = LABEL_GONE;
  }
  if (!keep)
    *ptop = slast;
}
