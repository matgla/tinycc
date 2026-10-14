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

/* nested.c -- Nested functions: trampolines, capture prescan and compilation.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"
#include "memory/vector.h"

/* gen_function lives in source/backend/generators/function.c */

/* Find NestedFunc by function symbol */
static NestedFunc *find_nested_func_by_sym(Sym *sym)
{
  for (int i = 0; i < tcc_state->nb_nested_funcs; i++)
  {
    if (tcc_state->nested_funcs[i]->sym == sym)
      return tcc_state->nested_funcs[i];
  }
  return NULL;
}

/* The static chain a reference from the function being compiled hands
 * `callee`: 0 = this frame's top (the callee is our child, or we are not a
 * nested function), k >= 1 = our incoming chain walked up k - 1 frames (1: a
 * sibling, 2: a sibling of our parent, ...). */
int nested_chain_level(const NestedFunc *callee)
{
  const NestedFunc *cur = tcc_state->current_nested_func;
  if (!cur || !callee || callee->parent_nf == cur)
    return 0;
  int callee_level = 0, cur_level = 0;
  for (const NestedFunc *p = callee; p; p = p->parent_nf)
    callee_level++;
  for (const NestedFunc *p = cur; p; p = p->parent_nf)
    cur_level++;
  return cur_level >= callee_level ? cur_level - callee_level + 1 : 0;
}

/* Set up trampoline for a nested function whose address is being taken.
 * Creates chain slot and trampoline symbols if not yet created,
 * emits INIT_CHAIN_SLOT IR to store the callee's chain into the chain slot,
 * and replaces vtop->sym with the trampoline symbol. */
void setup_nested_func_trampoline(Sym *s)
{
  NestedFunc *nf = find_nested_func_by_sym(s);
  /* A body parsed for its diagnostics only (check_only) has no code to need
   * a trampoline, and the chain slot and trampoline symbols would outlive it. */
  if (!nf || tcc_state->check_only)
    return;

  nf->trampoline_needed = 1;

  /* Get the nested function's ELF name for symbol naming */
  const char *func_name = get_tok_str(nf->sym->asm_label ? nf->sym->asm_label : nf->sym->v, NULL);

  /* Create chain slot TCC symbol + ELF symbol in .data (if not already created) */
  if (!nf->chain_slot_tcc_sym)
  {
    Section *data_sec = data_section;
    addr_t offset = section_add(data_sec, 4, 4);

    char chain_name[256];
    snprintf(chain_name, sizeof(chain_name), "__chain_%s", func_name);
    int elf_idx =
        put_elf_sym(symtab_section, offset, 4, ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT), 0, data_sec->sh_num, chain_name);

    /* Initialize to 0 */
    memset(data_sec->data + offset, 0, 4);

    /* Create a TCC Sym so greloc/load_full_const can work with it */
    Sym *cs_sym = sym_malloc();
    memset(cs_sym, 0, sizeof(*cs_sym));
    cs_sym->v = anon_sym++;
    cs_sym->type.t = VT_INT;
    cs_sym->r = VT_CONST | VT_SYM;
    cs_sym->c = elf_idx;
    nf->chain_slot_tcc_sym = cs_sym;
  }

  /* Create trampoline TCC symbol + ELF symbol in .text (if not already created) */
  if (!nf->trampoline_tcc_sym)
  {
    Section *text_sec = cur_text_section;
    char tramp_name[256];
    snprintf(tramp_name, sizeof(tramp_name), "__tramp_%s", func_name);

    /* Placeholder: offset and size will be updated when trampoline code is emitted */
    int elf_idx =
        put_elf_sym(symtab_section, 0, 0, ELFW(ST_INFO)(STB_LOCAL, STT_FUNC), 0, text_sec->sh_num, tramp_name);

    Sym *tr_sym = sym_malloc();
    memset(tr_sym, 0, sizeof(*tr_sym));
    tr_sym->v = anon_sym++;
    tr_sym->type.t = VT_FUNC;
    tr_sym->r = VT_CONST | VT_SYM;
    tr_sym->c = elf_idx;
    nf->trampoline_tcc_sym = tr_sym;
  }

  /* Emit INIT_CHAIN_SLOT IR: store parent FP to chain slot at runtime */
  if (tcc_state->ir && !NOEVAL_WANTED)
  {
    SValue src, dest;
    svalue_init(&src);
    svalue_init(&dest);
    /* src carries the chain slot symbol so the codegen can emit a
     * LDR + STR sequence with the correct relocation */
    src.type.t = VT_INT;
    src.r = VT_CONST | VT_SYM;
    src.sym = nf->chain_slot_tcc_sym;
    src.c.i = 0;
    src.vr = -1;
    dest.type.t = VT_INT;
    dest.r = 0;
    dest.vr = -1;
    SValue level;
    svalue_init(&level);
    level.type.t = VT_INT;
    level.r = VT_CONST;
    level.c.i = nested_chain_level(nf);
    level.vr = -1;
    tcc_ir_put(tcc_state->ir, TCCIR_OP_INIT_CHAIN_SLOT, &src, &level, &dest);
  }

  /* Replace the function symbol with the trampoline symbol */
  vtop->sym = nf->trampoline_tcc_sym;
}

/* Emit trampoline code for a nested function that needs it */
static void emit_trampoline_for_nested_func(NestedFunc *nf)
{
  /* Arch-specific: emit trampoline machine code + relocations.
   * Returns the entry address (may include arch-specific bits, e.g. Thumb). */
  addr_t entry_addr = gen_nested_func_trampoline(nf->chain_slot_tcc_sym, nf->sym);

  /* Update the ELF symbol for the trampoline to point to actual code location */
  {
    ElfSym *esym = elfsym(nf->trampoline_tcc_sym);
    if (esym)
    {
      esym->st_value = entry_addr;
      esym->st_size = ind - (entry_addr & ~1u);
    }
  }
}

/* Emit all trampolines needed for nested functions in this parent */
static void emit_all_trampolines(void)
{
  for (int i = 0; i < tcc_state->nb_nested_funcs; i++)
  {
    NestedFunc *nf = tcc_state->nested_funcs[i];
    if (nf->trampoline_needed)
    {
      emit_trampoline_for_nested_func(nf);
    }
  }
}

/* Saved state for parent function when compiling nested functions */
typedef struct
{
  TCCIRState *ir;
  int loc;
  int ind;
  int rsym;
  int func_ind;
  int func_align_pad;
  const char *funcname;
  CType func_vt;
  int func_var;
  int func_has_label_addr;
  int cur_scope;
  int root_scope;
  int loop_scope;
  Sym *local_stack;
  Sym *local_label_stack;
  Sym *global_label_stack;
  unsigned nocode_wanted;
  int local_scope_level;
  int nb_temp_local_vars;
  /* Use mangled names to avoid macro conflicts */
  Section *sec_text;
  struct switch_t *sec_switch;
  /* Temp local vars array */
  struct temp_local_variable tmp_vars[MAX_TEMP_LOCAL_VARIABLE_NUMBER];
} ParentSavedState;

/* The stand-in Syms tok_identifier makes for each reference to a captured
 * variable sit on no scope stack, so no sym_pop frees the facts a store
 * records on them.  Chained through prev (unused off a stack) and dropped once
 * the body is compiled; the Syms stay, IR may still point at them. */
static Sym *captured_var_syms;

void nested_track_captured_sym(Sym *s)
{
  s->prev = captured_var_syms;
  captured_var_syms = s;
}

static void free_captured_sym_facts(void)
{
  for (Sym *s = captured_var_syms; s; s = s->prev)
    sym_free_facts(s);
  captured_var_syms = NULL;
}

/* ---- the names of GNU nested functions ----
 * A nested function's Sym lives on global_stack (it is a function of the TU)
 * but its name binds like a block-scope declaration: nested_define links it
 * at the head of its chain, and the end of the defining block unlinks it
 * (nested_scope_end).  While linked it carries a nonzero sym_scope, so
 * global_identifier_push and external_sym pass over it as over any other
 * block-scope binding.  A body compiled after its parent binds the nested
 * functions its definition saw (def_vis), linked again for that compile. */
typedef struct NestedLink
{
  Sym *s;
  int level; /* local_scope of the defining block; 0: linked for a compile */
} NestedLink;
static vector(NestedLink) nested_links;

/* A block-scope function prototype: a nested function of that name defined
 * later in the same block is its `auto` forward declaration (without `auto`
 * GNU C rejects the definition), and the nested functions defined since the
 * prototype bind to it. */
typedef struct NestedProto
{
  int v;
  int level;    /* local_scope of the block */
  int first_nf; /* tcc_state->nb_nested_funcs at the prototype */
} NestedProto;
static vector(NestedProto) nested_protos;

static void nested_link(Sym *s, int level)
{
  TokenSym *ts = tok_ensure(s->v);
  TCC_PREDEF_PROTOS_FLUSH();
  if (SYM_IS_PARKED(ts->sym_identifier))
    sym_unpark(ts);
  s->prev_tok = ts->sym_identifier;
  ts->sym_identifier = s;
  /* Bound for a compile: below every scope of the body (a parameter may
   * take the name), yet not file scope. */
  s->sym_scope = level > 0 ? level : -1;
  vector_push_back(&nested_links, ((NestedLink){s, level}));
}

static void nested_unlink(Sym *s)
{
  Sym **pp = &table_ident[s->v - TOK_IDENT]->sym_identifier;
  while (*pp && *pp != s && !SYM_IS_PARKED(*pp))
    pp = &(*pp)->prev_tok;
  if (*pp == s)
    *pp = s->prev_tok;
  s->prev_tok = NULL;
  s->sym_scope = 0;
}

static void nested_unlink_to(size_t mark)
{
  while (nested_links.size > mark)
    nested_unlink(nested_links.data[--nested_links.size].s);
}

static int nested_linked_since(size_t mark, Sym *s)
{
  for (size_t i = mark; i < nested_links.size; i++)
    if (nested_links.data[i].s == s)
      return 1;
  return 0;
}

static NestedFunc *nested_func_index_of(Sym *s, int from)
{
  for (int i = from; i < tcc_state->nb_nested_funcs; i++)
    if (tcc_state->nested_funcs[i]->sym == s)
      return tcc_state->nested_funcs[i];
  return NULL;
}

void nested_define(NestedFunc *nf)
{
  Sym *s = nf->sym;
  nested_link(s, local_scope);
  for (size_t i = 0; i < nested_protos.size; i++)
  {
    NestedProto p = nested_protos.data[i];
    if (p.v != s->v || p.level != local_scope)
      continue;
    for (int j = p.first_nf; j < tcc_state->nb_nested_funcs; j++)
    {
      NestedFunc *o = tcc_state->nested_funcs[j];
      int k;
      if (o == nf || o->compiled)
        continue;
      for (k = 0; k < o->nb_def_vis; k++)
        if (o->def_vis[k]->v == s->v)
          break;
      if (k == o->nb_def_vis)
        dynarray_add(&o->def_vis, &o->nb_def_vis, s);
      else if (!nested_func_index_of(o->def_vis[k], p.first_nf))
        o->def_vis[k] = s; /* the prototype hid an outer one */
    }
  }
  for (size_t i = 0; i < nested_links.size; i++)
    dynarray_add(&nf->def_vis, &nf->nb_def_vis, nested_links.data[i].s);
}

void nested_note_block_proto(int v)
{
  vector_push_back(&nested_protos, ((NestedProto){v, local_scope, tcc_state->nb_nested_funcs}));
}

void nested_scope_end(int level)
{
  while (nested_links.size && nested_links.data[nested_links.size - 1].level >= level &&
         nested_links.data[nested_links.size - 1].level > 0)
    nested_unlink(nested_links.data[--nested_links.size].s);
  while (nested_protos.size && nested_protos.data[nested_protos.size - 1].level >= level)
    nested_protos.size--;
}

/* A function at file scope starts with no nested function bound. */
void nested_names_reset(void)
{
  nested_unlink_to(0);
  nested_protos.size = 0;
}

void nested_names_free(void)
{
  vector_cleanup(&nested_links);
  vector_cleanup(&nested_protos);
}

void nested_func_release(NestedFunc *nf)
{
  tcc_free(nf->def_vis);
  nf->def_vis = NULL;
  nf->nb_def_vis = 0;
}

/* What compile_nested_functions binds for one body: the parent's labels,
 * typedefs, enumeration constants, statics and tags, and the nested
 * functions, as at the definition. */
typedef struct NestedEnv
{
  NestedFunc *nf;
  int live;
  size_t link_mark;
  Sym *storage; /* the typedef and enumeration constant copies */
  Sym *idents[2 * MAX_CAPTURED_VARS];
  int nb_idents;
  Sym *static_prev[MAX_CAPTURED_VARS];
  Sym *tag_prev[MAX_CAPTURED_VARS];
} NestedEnv;
static NestedEnv *nested_env; /* the body being compiled */

static void nested_ident_link(Sym *s)
{
  TokenSym *ts = table_ident[s->v - TOK_IDENT];
  if (SYM_IS_PARKED(ts->sym_identifier))
    sym_unpark(ts);
  s->prev_tok = ts->sym_identifier;
  ts->sym_identifier = s;
}

static void nested_env_enter(NestedEnv *e, NestedFunc *nf)
{
  memset(e, 0, sizeof(*e));
  e->nf = nf;
  e->live = 1;

  /* parent addr-taken labels, so &&label in the body finds them */
  for (int j = 0; j < nf->nb_addr_labels; j++)
  {
    Sym *lbl = nf->addr_label_syms[j];
    lbl->prev_tok = sym_label_head(lbl->v);
    sym_set_label_head(lbl->v, lbl);
  }

  /* the nested functions the definition saw, below the parent's names */
  e->link_mark = nested_links.size;
  for (int j = 0; j < nf->nb_def_vis; j++)
    if (!nested_linked_since(e->link_mark, nf->def_vis[j]))
      nested_link(nf->def_vis[j], 0);

  /* Typedefs and enumeration constants: copies on a stack of their own, so
   * gen_function's pop_local_syms does not free them. */
  for (int j = 0; j < nf->nb_parent_typedefs; j++)
  {
    int tv = nf->parent_typedef_tokens[j];
    Sym *ts_sym = sym_push2(&e->storage, tv, nf->parent_typedef_types[j].t, 0);
    ts_sym->type.ref = nf->parent_typedef_types[j].ref;
    ts_sym->sym_scope = 0;
    if ((unsigned)(tv - TOK_IDENT) < (unsigned)(tok_ident - TOK_IDENT))
    {
      nested_ident_link(ts_sym);
      e->idents[e->nb_idents++] = ts_sym;
    }
  }
  for (int j = 0; j < nf->nb_parent_enums; j++)
  {
    int ev = nf->parent_enum_tokens[j];
    Sym *es = sym_push2(&e->storage, ev, nf->parent_enum_types[j].t, 0);
    es->type.ref = nf->parent_enum_types[j].ref;
    es->r = VT_CONST;
    sym_set_enum_val(es, nf->parent_enum_vals[j]);
    if ((unsigned)(ev - TOK_IDENT) < (unsigned)(tok_ident - TOK_IDENT))
    {
      nested_ident_link(es);
      e->idents[e->nb_idents++] = es;
    }
  }

  /* Statics: the original Syms, which outlive their scope. */
  for (int j = 0; j < nf->nb_parent_statics; j++)
  {
    Sym *ss = nf->parent_static_syms[j];
    e->static_prev[j] = ss->prev_tok;
    nested_ident_link(ss);
  }

  /* Tags: the original Syms (completed tags have c != 0, so sym_pop does not
   * free them). */
  for (int j = 0; j < nf->nb_parent_struct_tags; j++)
  {
    Sym *tag = nf->parent_struct_tag_syms[j];
    int tv = tag->v & ~SYM_STRUCT;
    e->tag_prev[j] = NULL;
    if ((unsigned)(tv - TOK_IDENT) < (unsigned)(tok_ident - TOK_IDENT))
    {
      e->tag_prev[j] = sym_tag_head(tv);
      tag->prev_tok = e->tag_prev[j];
      sym_set_tag_head(tv, tag);
    }
  }
}

/* The body is parsed: what enter bound stops binding (in reverse). */
static void nested_env_unlink(NestedEnv *e)
{
  NestedFunc *nf = e->nf;
  if (!e->live)
    return;
  e->live = 0;
  for (int j = nf->nb_parent_struct_tags - 1; j >= 0; j--)
  {
    Sym *tag = nf->parent_struct_tag_syms[j];
    int tv = tag->v & ~SYM_STRUCT;
    if ((unsigned)(tv - TOK_IDENT) < (unsigned)(tok_ident - TOK_IDENT) && sym_tag_head(tv) == tag)
      sym_set_tag_head(tv, e->tag_prev[j]);
  }
  for (int j = nf->nb_parent_statics - 1; j >= 0; j--)
  {
    Sym *ss = nf->parent_static_syms[j];
    TokenSym *tsi = table_ident[ss->v - TOK_IDENT];
    if (tsi->sym_identifier == ss)
      tsi->sym_identifier = ss->prev_tok;
    ss->prev_tok = e->static_prev[j];
  }
  for (int j = e->nb_idents - 1; j >= 0; j--)
  {
    Sym *ts_sym = e->idents[j];
    TokenSym *tsi = table_ident[ts_sym->v - TOK_IDENT];
    if (tsi->sym_identifier == ts_sym)
      tsi->sym_identifier = ts_sym->prev_tok;
  }
  nested_unlink_to(e->link_mark);
  for (int j = nf->nb_addr_labels - 1; j >= 0; j--)
  {
    Sym *lbl = nf->addr_label_syms[j];
    if (sym_label_head(lbl->v) == lbl)
      sym_set_label_head(lbl->v, lbl->prev_tok);
  }
}

static void nested_env_leave(NestedEnv *e)
{
  nested_env_unlink(e);
  for (Sym *s = e->storage; s;)
  {
    Sym *next = s->prev;
    sym_free(s);
    s = next;
  }
  e->storage = NULL;
}

/* A nested function defined in the body being compiled sees, besides the
 * body's own scope (the local_stack walk in decl), what that body sees of its
 * parent: the visible ones among the names nested_env_enter bound. */
void nested_record_inherited(NestedFunc *nf)
{
  NestedEnv *e = nested_env;
  if (!e || !e->live)
    return;
  for (int j = 0; j < e->nb_idents; j++)
  {
    Sym *s = e->idents[j];
    if (sym_find(s->v) != s)
      continue;
    if (IS_ENUM_VAL(s->type.t))
    {
      if (nf->nb_parent_enums < MAX_CAPTURED_VARS)
      {
        nf->parent_enum_tokens[nf->nb_parent_enums] = s->v;
        nf->parent_enum_types[nf->nb_parent_enums] = s->type;
        nf->parent_enum_vals[nf->nb_parent_enums] = sym_enum_val(s);
        nf->nb_parent_enums++;
      }
    }
    else if (nf->nb_parent_typedefs < MAX_CAPTURED_VARS)
    {
      nf->parent_typedef_tokens[nf->nb_parent_typedefs] = s->v;
      nf->parent_typedef_types[nf->nb_parent_typedefs] = s->type;
      nf->nb_parent_typedefs++;
    }
  }
  for (int j = 0; j < e->nf->nb_parent_statics; j++)
  {
    Sym *s = e->nf->parent_static_syms[j];
    if (sym_find(s->v) == s && nf->nb_parent_statics < MAX_CAPTURED_VARS)
      nf->parent_static_syms[nf->nb_parent_statics++] = s;
  }
  for (int j = 0; j < e->nf->nb_parent_struct_tags; j++)
  {
    Sym *tag = e->nf->parent_struct_tag_syms[j];
    int tv = tag->v & ~SYM_STRUCT;
    if ((unsigned)(tv - TOK_IDENT) < (unsigned)(tok_ident - TOK_IDENT) && sym_tag_head(tv) == tag &&
        nf->nb_parent_struct_tags < MAX_CAPTURED_VARS)
      nf->parent_struct_tag_syms[nf->nb_parent_struct_tags++] = tag;
  }
}

/* Compile all nested functions defined inside a parent function */
void compile_nested_functions(Sym *parent_sym)
{
  int nb_nested;
  ParentSavedState saved;

  (void)parent_sym; /* currently unused */

  /* Called at the end of a nested function's gen_function: that body is
   * parsed, and each body compiled from here binds its own names. */
  NestedEnv *saved_env = nested_env;
  if (nested_env)
    nested_env_unlink(nested_env);

  nb_nested = tcc_state->nb_nested_funcs;
  if (nb_nested == 0)
    return;

  /* Save debug state before nested function compilation */
  void *saved_debug_info, *saved_debug_root;
  tcc_debug_save_state(tcc_state, &saved_debug_info, &saved_debug_root);

  /* Save ALL parent global state */
  saved.ir = tcc_state->ir;
  NestedFunc *saved_current_nf = tcc_state->current_nested_func;
  saved.loc = loc;
  saved.ind = ind;
  saved.rsym = rsym;
  saved.func_ind = func_ind;
  saved.func_align_pad = func_align_pad;
  saved.funcname = funcname;
  saved.func_vt = func_vt;
  saved.func_var = func_var;
  saved.func_has_label_addr = func_has_label_addr;
  saved.cur_scope = (int)(intptr_t)cur_scope;
  saved.root_scope = (int)(intptr_t)root_scope;
  saved.loop_scope = (int)(intptr_t)loop_scope;
  saved.local_stack = local_stack;
  saved.local_label_stack = local_label_stack;
  saved.global_label_stack = global_label_stack;
  saved.nocode_wanted = nocode_wanted;
  saved.local_scope_level = local_scope;
  saved.nb_temp_local_vars = nb_temp_local_vars;
  saved.sec_text = cur_text_section;
  saved.sec_switch = cur_switch;
  memcpy(saved.tmp_vars, arr_temp_local_vars, sizeof(arr_temp_local_vars));

  /* Compile each nested function.
   * Reset compile_idx each time; the 'compiled' flag on each NestedFunc
   * prevents re-compilation when gen_function calls compile_nested_functions
   * recursively (for multi-level nesting). */
  int compile_idx = 0;
  while (compile_idx < tcc_state->nb_nested_funcs)
  {
    NestedFunc *nf = tcc_state->nested_funcs[compile_idx];

    /* Skip already-compiled functions (safety check) */
    if (nf->compiled)
    {
      compile_idx++;
      continue;
    }

    /* For nested function compilation, start with a fresh local_stack.
     * Captured variable resolution is handled in the identifier lookup code
     * (see tok_identifier in tccgen.c), which checks current_nested_func. */
    local_stack = NULL; /* Start fresh - captured vars handled specially */
    local_scope = 0;

    /* Track current nested function for static chain setup */
    tcc_state->current_nested_func = nf;

    /* Replay saved token stream (same as inline function expansion) */
    tccpp_setfile(nf->filename);
    begin_macro(nf->func_str, 1);
    next(); /* prime the first token - should be '{' */

    /* Set up text section for nested function (same as regular functions) */
    if (!cur_text_section)
      cur_text_section = text_section;

    /* Use the symbol that was already pushed during parsing */
    /* The symbol was pushed with VT_CONST to mark it as a function */

    /* Mark as compiled BEFORE gen_function to prevent recursive recompilation.
     * gen_function may discover inner nested functions (e.g., level2 inside level1)
     * and call compile_nested_functions recursively. If this function isn't marked,
     * the recursive call would try to compile it again (compile_idx is static). */
    nf->compiled = 1;

    /* Bind the parent's names the body sees, as at its definition. */
    NestedEnv env;
    nested_env_enter(&env, nf);
    nested_env = &env;

    gen_function(nf->sym);
    free_captured_sym_facts();

    nested_env_leave(&env);
    nested_env = saved_env;

    /* gen_function() resets cur_text_section=NULL and ind=0 for safety.
     * Restore them so the next nested function starts at the right offset
     * and compile_nested_functions can report the correct ind to the parent. */
    cur_text_section = saved.sec_text;
    ind = cur_text_section->data_offset;

    /* Clear current nested function */
    tcc_state->current_nested_func = NULL;

    end_macro();

    /* Continue to next nested function. If new ones were discovered during
     * compilation, they'll have indices > compile_idx, and we'll get to them
     * because compile_idx < nb_nested_funcs will still be true. */
    compile_idx++;
  }

  /* Restore ALL parent state */
  tcc_state->ir = saved.ir;
  tcc_state->current_nested_func = saved_current_nf;
  loc = saved.loc;
  /* NOTE: do NOT restore ind - nested func code is in .text and
     the parent's codegen will emit at the CURRENT ind (after nested funcs) */
  rsym = saved.rsym;
  func_ind = saved.func_ind;
  func_align_pad = saved.func_align_pad;
  funcname = saved.funcname;
  func_vt = saved.func_vt;
  func_var = saved.func_var;
  func_has_label_addr = saved.func_has_label_addr;
  cur_scope = (struct scope *)(intptr_t)saved.cur_scope;
  root_scope = (struct scope *)(intptr_t)saved.root_scope;
  loop_scope = (struct scope *)(intptr_t)saved.loop_scope;
  local_stack = saved.local_stack;
  local_label_stack = saved.local_label_stack;
  global_label_stack = saved.global_label_stack;
  nocode_wanted = saved.nocode_wanted;
  local_scope = saved.local_scope_level;
  nb_temp_local_vars = saved.nb_temp_local_vars;
  cur_text_section = saved.sec_text;
  cur_switch = saved.sec_switch;
  memcpy(arr_temp_local_vars, saved.tmp_vars, sizeof(arr_temp_local_vars));

  /* Restore debug state for parent function */
  tcc_debug_restore_state(tcc_state, saved_debug_info, saved_debug_root);

  /* Emit trampolines for nested functions whose address was taken.
   * Must be done before clearing the nested funcs list. */
  emit_all_trampolines();

  /* Clear nested funcs list after compiling */
  tcc_state->nb_nested_funcs = 0;
}

/* Track which nested function is currently being prescanned.
 * This is needed for multi-level nesting to establish parent-child links. */
static NestedFunc *prescan_current_nf = NULL;

/* Pre-scan a nested function's token stream to identify captured parent variables.
 * This is called during parsing of the parent function, before the parent's block
 * generates IR, so that captured variables can be marked address-taken early.
 * If explicit_parent_nf is non-NULL, it is used as the parent (for nested funcs
 * discovered during gen_function). Otherwise, prescan_current_nf is used. */

/* Pre-scan a nested function's token stream to identify captured parent variables.
 * This is called during parsing of the parent function, before the parent's block
 * generates IR, so that captured variables can be marked address-taken early. */
void prescan_captured_vars(NestedFunc *nf, Sym *parent_local_stack, NestedFunc *explicit_parent_nf)
{
  /* If we're already inside a prescan (prescan_current_nf != NULL), this means
   * we discovered a nested function during another nested function's prescan.
   * Skip the prescan of this inner function - it will be handled later when
   * the outer function is compiled and its tokens are replayed. */
  if (prescan_current_nf != NULL)
  {
    /* Just set the parent link so we know the hierarchy */
    nf->parent_nf = prescan_current_nf;
    return;
  }

  /* Set parent_nf for multi-level nesting support.
   * If explicit_parent_nf is provided, use it (for nested funcs discovered
   * during gen_function). Otherwise, use prescan_current_nf (for nested funcs
   * discovered during prescan). */
  nf->parent_nf = explicit_parent_nf;

  /* Save and set current */
  NestedFunc *saved_current = prescan_current_nf;
  prescan_current_nf = nf;
  TokenString *tok_str = nf->func_str;

  if (!tok_str)
    return;

  /* Parameters shadow the parent's variables for the whole body.  Locals do
   * not: they are scoped, so a token is captured whenever it resolves in the
   * parent and the nested function's own scope rules pick its local where one
   * is visible (unary() tries sym_find before captured_tokens). */
  int shadowed_toks[MAX_CAPTURED_VARS];
  int nb_shadowed = 0;
  /* Parameter names shadow parent variables of the same name */
  {
    Sym *ref = nf->sym ? nf->sym->type.ref : NULL;
    if (ref)
    {
      for (Sym *param = ref->next; param; param = param->next)
      {
        int pv = param->v & ~SYM_FIELD;
        if (pv >= TOK_IDENT && nb_shadowed < MAX_CAPTURED_VARS)
          shadowed_toks[nb_shadowed++] = pv;
      }
    }
  }
  const int *p = tok_str_buf(tok_str);
  int prev_tok = 0; /* track previous token for goto detection */

  while (*p != TOK_EOF && *p != 0)
  {
    int t = *p++;

    /* Skip past token payload for multi-int tokens */
    switch (t)
    {
    case TOK_CINT:
    case TOK_CCHAR:
    case TOK_LCHAR:
    case TOK_LINENUM:
    case TOK_PACK_REPLAY:
    case TOK_CUINT:
    case TOK_CFLOAT:
    case TOK_CFLOAT_I:
    case TOK_CINT_I:
#if LONG_SIZE == 4
    case TOK_CLONG:
    case TOK_CULONG:
#endif
      p++; /* 1 extra int */
      break;
    case TOK_CDOUBLE:
    case TOK_CDOUBLE_I:
    case TOK_CLLONG:
    case TOK_CULLONG:
#if LONG_SIZE == 8
    case TOK_CLONG:
    case TOK_CULONG:
#endif
      p += 2; /* 2 extra ints */
      break;
    case TOK_CLDOUBLE:
    case TOK_CLDOUBLE_I:
#if LDOUBLE_SIZE == 8 || defined TCC_USING_DOUBLE_FOR_LDOUBLE
      p += 2;
#elif LDOUBLE_SIZE == 12
      p += 3;
#elif LDOUBLE_SIZE == 16
      p += 4;
#endif
      break;
    case TOK_STR:
    case TOK_LSTR:
    case TOK_PPNUM:
    case TOK_PPSTR:
    {
      int sz = *p++;
      p += (sz + sizeof(int) - 1) / sizeof(int);
      break;
    }
    default:
      break;
    }

    if (t >= TOK_IDENT)
    {
      /* Skip tokens that are shadowed by parameters or local declarations —
       * these are NOT genuine captures of parent variables. */
      int is_shadowed = 0;
      for (int si = 0; si < nb_shadowed; si++)
        if (shadowed_toks[si] == t) { is_shadowed = 1; break; }

      /* Look up this identifier in parent's local stack */
      Sym *s = !is_shadowed ? sym_find2(parent_local_stack, t) : NULL;
      if (s && ((s->r & VT_VALMASK) == VT_LOCAL || (s->r & VT_PARAM)))
      {
        /* Mark as address-taken to force stack allocation */
        s->a.addrtaken = 1;
        /* Also mark in IR so register allocator knows to spill to stack */
        if (tcc_state->ir && s->vreg >= 0)
          tcc_ir_set_addrtaken(tcc_state->ir, s->vreg);

        /* Record the variable if we haven't already */
        int i;
        int already_captured = 0;
        for (i = 0; i < nf->nb_captured; i++)
        {
          if (nf->captured_tokens[i] == t)
          {
            already_captured = 1;
            break;
          }
        }
        if (!already_captured && nf->nb_captured < MAX_CAPTURED_VARS)
        {
          nf->captured_vregs[nf->nb_captured] = s->vreg;
          nf->captured_offsets[nf->nb_captured] = s->c;
          nf->captured_tokens[nf->nb_captured] = t;
          nf->captured_types[nf->nb_captured] = s->type;
          nf->captured_chain_depth[nf->nb_captured] = 1; /* direct parent */
          nf->nb_captured++;
        }
      }
      /* Not found in parent locals — search parent's own captured vars.
       * level1 captured 'a' from main with depth 1, so level2 inherits
       * it with depth 2. */
      else if (nf->parent_nf)
      {
        NestedFunc *parent_nf = nf->parent_nf;
        for (int j = 0; j < parent_nf->nb_captured; j++)
        {
          if (parent_nf->captured_tokens[j] == t)
          {
            /* Guard: check not already captured (e.g. token appears twice) */
            int dup = 0;
            for (int k = 0; k < nf->nb_captured; k++)
              if (nf->captured_tokens[k] == t)
              {
                dup = 1;
                break;
              }
            if (dup)
              break;

            nf->captured_offsets[nf->nb_captured] = parent_nf->captured_offsets[j];
            nf->captured_tokens[nf->nb_captured] = t;
            nf->captured_types[nf->nb_captured] = parent_nf->captured_types[j];
            nf->captured_vregs[nf->nb_captured] = parent_nf->captured_vregs[j];
            nf->captured_chain_depth[nf->nb_captured] = parent_nf->captured_chain_depth[j] + 1;
            /* Child needs multi-hop → parent must save chain at FP-4 */
            if (nf->captured_chain_depth[nf->nb_captured] > 1)
            {
              parent_nf->needs_chain_save = 1;
              /* Also update parent's IR if it's currently being compiled */
              if (tcc_state->ir && tcc_state->ir->has_static_chain)
                tcc_state->ir->needs_chain_save = 1;
            }
            nf->nb_captured++;
            break;
          }
        }
      }

      /* Non-local goto detection: if previous token was TOK_GOTO
       * and this identifier matches a __label__ in the parent scope,
       * record it as a non-local goto target. */
      if (prev_tok == TOK_GOTO)
      {
        /* Search parent's local_label_stack for this token */
        Sym *lbl;
        for (lbl = local_label_stack; lbl; lbl = lbl->prev)
        {
          if (lbl->v == t && (lbl->r == LABEL_DECLARED || lbl->r == LABEL_FORWARD || lbl->r == LABEL_DEFINED))
          {
            /* Found a matching __label__ in parent - record as non-local goto target */
            if (nf->nb_nlgotos < MAX_NONLOCAL_GOTOS)
            {
              /* Check for duplicate */
              int dup = 0;
              for (int k = 0; k < nf->nb_nlgotos; k++)
              {
                if (nf->nlgoto_label_tokens[k] == t)
                {
                  dup = 1;
                  break;
                }
              }
              if (!dup)
              {
                nf->nlgoto_label_tokens[nf->nb_nlgotos] = t;
                nf->nlgoto_buf_offsets[nf->nb_nlgotos] = lbl->c; /* jmp_buf FP offset from __label__ alloc */
                nf->nb_nlgotos++;
                /* Also add the jmp_buf as a captured variable so the nested function
                 * can access it via the static chain. Use a synthetic token that won't
                 * collide with real variables. We use negative token values. */
                if (nf->nb_captured < MAX_CAPTURED_VARS)
                {
                  nf->captured_vregs[nf->nb_captured] = -1;
                  nf->captured_offsets[nf->nb_captured] = lbl->c;
                  /* Use the label token itself as captured token - it won't collide with
                   * variables because labels and variables are in different namespaces */
                  nf->captured_tokens[nf->nb_captured] = -t; /* negative = non-local goto buf */
                  CType buf_type;
                  buf_type.t = VT_INT; /* placeholder type for the buffer */
                  buf_type.ref = NULL;
                  nf->captured_types[nf->nb_captured] = buf_type;
                  nf->captured_chain_depth[nf->nb_captured] = 1; /* direct parent */
                  nf->nb_captured++;
                }
              }
            }
            break;
          }
        }
      }

      /* Address-of-label detection: if previous token was TOK_LAND (&&)
       * and this identifier matches a __label__ in the parent scope,
       * mark the label as addr-taken so it persists through label_pop
       * and record it so compile_nested_functions can make it visible. */
      if (prev_tok == TOK_LAND)
      {
        Sym *lbl;
        for (lbl = local_label_stack; lbl; lbl = lbl->prev)
        {
          if (lbl->v == t && (lbl->r == LABEL_DECLARED || lbl->r == LABEL_FORWARD || lbl->r == LABEL_DEFINED))
          {
            lbl->a.addrtaken = 1;
            if (nf->nb_addr_labels < MAX_NONLOCAL_GOTOS)
            {
              /* Check for duplicate */
              int dup = 0;
              for (int k = 0; k < nf->nb_addr_labels; k++)
              {
                if (nf->addr_label_syms[k] == lbl)
                {
                  dup = 1;
                  break;
                }
              }
              if (!dup)
                nf->addr_label_syms[nf->nb_addr_labels++] = lbl;
            }
            break;
          }
        }
      }
    }
    prev_tok = t;
  }

  /* Restore previous prescan current */
  prescan_current_nf = saved_current;
}

/* Scan a raw token buffer (int*) for identifiers that reference captured parent
 * variables. Used to capture variables referenced in VLA expression tokens
 * (vla_array_str) which are not part of the nested function body token stream. */
static void prescan_token_buf_for_captures(NestedFunc *nf, const int *p, Sym *parent_local_stack)
{
  while (*p != TOK_EOF && *p != 0)
  {
    int t = *p++;
    switch (t)
    {
    case TOK_CINT:
    case TOK_CCHAR:
    case TOK_LCHAR:
    case TOK_LINENUM:
    case TOK_PACK_REPLAY:
    case TOK_CUINT:
    case TOK_CFLOAT:
    case TOK_CFLOAT_I:
    case TOK_CINT_I:
#if LONG_SIZE == 4
    case TOK_CLONG:
    case TOK_CULONG:
#endif
      p++;
      break;
    case TOK_CDOUBLE:
    case TOK_CDOUBLE_I:
    case TOK_CLLONG:
    case TOK_CULLONG:
#if LONG_SIZE == 8
    case TOK_CLONG:
    case TOK_CULONG:
#endif
      p += 2;
      break;
    case TOK_CLDOUBLE:
    case TOK_CLDOUBLE_I:
#if LDOUBLE_SIZE == 8 || defined TCC_USING_DOUBLE_FOR_LDOUBLE
      p += 2;
#elif LDOUBLE_SIZE == 12
      p += 3;
#elif LDOUBLE_SIZE == 16
      p += 4;
#endif
      break;
    case TOK_STR:
    case TOK_LSTR:
    case TOK_PPNUM:
    case TOK_PPSTR:
    {
      int sz = *p++;
      p += (sz + sizeof(int) - 1) / sizeof(int);
      break;
    }
    default:
      break;
    }
    if (t >= TOK_IDENT)
    {
      Sym *s = sym_find2(parent_local_stack, t);
      if (s && ((s->r & VT_VALMASK) == VT_LOCAL || (s->r & VT_PARAM)))
      {
        s->a.addrtaken = 1;
        if (tcc_state->ir && s->vreg >= 0)
          tcc_ir_set_addrtaken(tcc_state->ir, s->vreg);
        int already = 0;
        for (int i = 0; i < nf->nb_captured; i++)
          if (nf->captured_tokens[i] == t)
          {
            already = 1;
            break;
          }
        if (!already && nf->nb_captured < MAX_CAPTURED_VARS)
        {
          nf->captured_vregs[nf->nb_captured] = s->vreg;
          nf->captured_offsets[nf->nb_captured] = s->c;
          nf->captured_tokens[nf->nb_captured] = t;
          nf->captured_types[nf->nb_captured] = s->type;
          nf->captured_chain_depth[nf->nb_captured] = 1;
          nf->nb_captured++;
        }
      }
    }
  }
}

/* Walk a nested function's parameter types to find VLA expression token streams
 * and scan them for captured parent variables. VLA expressions are stored in
 * vla_array_str (inner dimensions) and vla_param_exprs (outermost dimension),
 * which are NOT part of the function body token stream scanned by prescan_captured_vars. */
void prescan_vla_param_captured_vars(NestedFunc *nf, Sym *parent_local_stack)
{
  Sym *func_type = nf->sym->type.ref;
  if (!func_type)
    return;
  for (Sym *arg = func_type->next; arg; arg = arg->next)
  {
    if ((arg->type.t & VT_BTYPE) != VT_PTR)
      continue;
    /* Walk the array dimension chain looking for VLA expressions */
    for (Sym *field = arg->type.ref; field;)
    {
      if ((field->type.t & VT_VLA) && field->type.ref)
      {
        /* The inner field may have vla_array_str set (TYPE_NEST dimensions) */
        Sym *inner = field->type.ref;
        if (inner->vla_array_str)
          prescan_token_buf_for_captures(nf, inner->vla_array_str, parent_local_stack);
        /* Continue to inner dimensions */
        field = inner;
      }
      else
        break;
    }
    /* Also check vla_param_exprs for outermost dimension tokens */
    for (int i = 0; i < tcc_state->nb_vla_param_exprs; i++)
    {
      if (tcc_state->vla_param_exprs[i].param == arg->type.ref)
        prescan_token_buf_for_captures(nf, tcc_state->vla_param_exprs[i].tokens, parent_local_stack);
    }
  }
}
