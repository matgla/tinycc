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

/* state.c -- Frontend global state definitions and the compile lifecycle
 * (tccgen_init / tccgen_compile / tccgen_finish).
 *
 * This is the only file in source/frontend/gen/ that *defines* the shared
 * frontend state; every other file reaches it through gen_priv.h.  Split out
 * of tccgen.c; see docs/plan_tccgen_split.md.
 */

#include "gen_priv.h"

/********************************************************/
/* global variables */

/* loc : local variable index
   ind : output code index
   rsym: return symbol
   anon_sym: anonymous symbol index
*/
ST_DATA int rsym, anon_sym, ind, loc;

ST_DATA Sym *global_stack;
ST_DATA Sym *local_stack;
ST_DATA Sym *define_stack;

unsigned char *aapcs_last_const_init;
int aapcs_last_const_init_size;
ST_DATA Sym *global_label_stack;
ST_DATA Sym *local_label_stack;

Sym *sym_free_first;
void **sym_pools;
int nb_sym_pools;

Sym *all_cleanups, *pending_gotos;
int local_scope;
int func_param_decl_depth;
ST_DATA char debug_modes;

FuncallScratch *funcall_scratch_stack;

static void funcall_scratch_free(FuncallScratch *fs)
{
  int i;

  if (!fs)
    return;
  tcc_free(fs->saved_args);
  for (i = 0; i < fs->saved_arg_count; i++)
    tcc_free(fs->saved_args_cid[i]);
  tcc_free(fs->saved_args_cid);
  tcc_free(fs->saved_args_cid_size);
  tcc_free(fs);
}

void funcall_scratch_pop_free(FuncallScratch *fs)
{
  FuncallScratch **p;

  for (p = &funcall_scratch_stack; *p; p = &(*p)->next)
  {
    if (*p == fs)
    {
      *p = fs->next;
      break;
    }
  }
  funcall_scratch_free(fs);
}

static void funcall_scratch_free_all(void)
{
  while (funcall_scratch_stack)
  {
    FuncallScratch *next = funcall_scratch_stack->next;
    funcall_scratch_free(funcall_scratch_stack);
    funcall_scratch_stack = next;
  }
}

PendingAliasDef *pending_aliases;
int nb_pending_aliases;

/* Pending label-difference symbols for &&lab1 - &&lab0 in static initializers.
   Set in gen_opic, consumed in init_putv. */
Sym *pending_label_diff_plus;
Sym *pending_label_diff_minus;

ST_DATA SValue *vtop;
ST_DATA SValue _vstack[1 + VSTACK_SIZE];

ST_DATA int nocode_wanted; /* no code generation wanted */

ST_DATA int global_expr; /* true if compound literals must be allocated globally
                            (used during initializers parsing */
ST_DATA CType func_vt;   /* current function return type (used by return instruction) */
ST_DATA int func_var;    /* true if current function is variadic (used by return
                            instruction) */
ST_DATA int func_vc;
ST_DATA int func_ind;
ST_DATA int func_has_label_addr;
ST_DATA const char *funcname;
ST_DATA CType int_type, func_old_type, func_old_void_type, func_old_char_pointer_type, func_old_void_pointer_type,
    func_old_size_t_type, char_type, char_pointer_type;

CString initstr;

struct switch_t *cur_switch; /* current switch */

/* list of temporary local variables on the stack in current function. */
struct temp_local_variable arr_temp_local_vars[MAX_TEMP_LOCAL_VARIABLE_NUMBER];
int nb_temp_local_vars;

/* Reusable stack slots for by-value struct arguments passed to variadic
 * functions (the invisible-copy the AAPCS requires for structs > 16 bytes).
 * Unlike get_temp_local_var()'s vstack-based tracking, these slots must stay
 * reserved until the whole call is emitted — each argument is lowered to a
 * FUNCPARAMVAL and popped off the vstack before the next argument is built,
 * so the vstack can no longer witness that the slot is in use.
 *
 * Instead a busy bitmask tracks live slots, and block() saves/restores it
 * around every statement.  Two struct-arg copies that are live at the same
 * time (e.g. f(a, b, a) — three copies, all read by the one call) therefore
 * get distinct slots, while copies from statements that have fully completed
 * are reclaimed.  GNU statement-expressions used as arguments enter a nested
 * block() whose save/restore leaves the enclosing call's reserved slots
 * untouched, so they cannot be aliased. */
#define MAX_ARG_STRUCT_TEMPS 64
static struct arg_struct_temp
{
  int location;
  int size;
  int align;
} arg_struct_temps[MAX_ARG_STRUCT_TEMPS];
int nb_arg_struct_temps;
uint64_t arg_struct_temp_busy;

int get_arg_struct_temp(int size, int align)
{
  for (int i = 0; i < nb_arg_struct_temps; i++)
  {
    if (!(arg_struct_temp_busy & ((uint64_t)1 << i)) &&
        arg_struct_temps[i].size >= size && arg_struct_temps[i].align >= align)
    {
      arg_struct_temp_busy |= (uint64_t)1 << i;
      return arg_struct_temps[i].location;
    }
  }
  loc = (loc - size) & -align;
  if (nb_arg_struct_temps < MAX_ARG_STRUCT_TEMPS)
  {
    int i = nb_arg_struct_temps++;
    arg_struct_temps[i].location = loc;
    arg_struct_temps[i].size = size;
    arg_struct_temps[i].align = align;
    arg_struct_temp_busy |= (uint64_t)1 << i;
  }
  /* Pool exhausted: a fresh never-reused slot is always correct. */
  return loc;
}

struct scope *cur_scope, *loop_scope, *root_scope;

/* ------------------------------------------------------------------------- */
/* initialize vstack and types.  This must be done also for tcc -E */
ST_FUNC void tccgen_init(TCCState *s1)
{
  CType size_t_type, void_type, void_pointer_type;

  vtop = vstack - 1;
  memset(vtop, 0, sizeof *vtop);

  str_lit_pool_reset();

  /* define some often used types */
  int_type.t = VT_INT;

  char_type.t = VT_BYTE;
  if (s1->char_is_unsigned)
    char_type.t |= VT_UNSIGNED;
  char_pointer_type = char_type;
  mk_pointer(&char_pointer_type);

  size_t_type.t = VT_SIZE_T;
  size_t_type.ref = NULL;

  void_type.t = VT_VOID;
  void_type.ref = NULL;

  void_pointer_type = void_type;
  mk_pointer(&void_pointer_type);

  func_old_type.t = VT_FUNC;
  func_old_type.ref = sym_push(SYM_FIELD, &int_type, 0, 0);
  func_old_type.ref->f.func_call = FUNC_CDECL;
  func_old_type.ref->f.func_type = FUNC_OLD;

  func_old_void_type.t = VT_FUNC;
  func_old_void_type.ref = sym_push(SYM_FIELD, &void_type, 0, 0);
  func_old_void_type.ref->f.func_call = FUNC_CDECL;
  func_old_void_type.ref->f.func_type = FUNC_OLD;

  func_old_char_pointer_type.t = VT_FUNC;
  func_old_char_pointer_type.ref = sym_push(SYM_FIELD, &char_pointer_type, 0, 0);
  func_old_char_pointer_type.ref->f.func_call = FUNC_CDECL;
  func_old_char_pointer_type.ref->f.func_type = FUNC_OLD;

  func_old_void_pointer_type.t = VT_FUNC;
  func_old_void_pointer_type.ref = sym_push(SYM_FIELD, &void_pointer_type, 0, 0);
  func_old_void_pointer_type.ref->f.func_call = FUNC_CDECL;
  func_old_void_pointer_type.ref->f.func_type = FUNC_OLD;

  func_old_size_t_type.t = VT_FUNC;
  func_old_size_t_type.ref = sym_push(SYM_FIELD, &size_t_type, 0, 0);
  func_old_size_t_type.ref->f.func_call = FUNC_CDECL;
  func_old_size_t_type.ref->f.func_type = FUNC_OLD;
#ifdef precedence_parser
  init_prec();
#endif
  cstr_new(&initstr);
}

ST_FUNC int tccgen_compile(TCCState *s1)
{
  funcname = "";
  func_ind = -1;
  anon_sym = SYM_FIRST_ANOM;
  pending_aliases = NULL;
  nb_pending_aliases = 0;
  nocode_wanted = DATA_ONLY_WANTED; /* no code outside of functions */
  debug_modes = (s1->do_debug ? 1 : 0) | s1->test_coverage << 1;

  tcc_debug_start(s1);
  tcc_tcov_start(s1);
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  arm_init(s1);
#endif
  /* The `predef macros` stamp is taken when the <command line> buffer closes,
     which is well after this point -- so everything here used to be billed to
     it, and the label reads as if it were all macro parsing.  It is not: the
     58 builtin prototypes are built here, and a ladder of extra -D flags
     prices a warm macro definition at ~15 us, i.e. ~2.6 ms for the ~170
     predefines against a 17 ms window.  Split the window so the two are
     attributable separately. */
  tcc_init_stamp("gen prologue");
  if (s1->predef_protos_pending)
  {
    s1->predef_protos_pending = 0;
    tccgen_predef_protos(s1);
  }
  tcc_init_stamp("predef protos");
#ifdef INC_DEBUG
  printf("%s: **** new file\n", file->filename);
#endif
  parse_flags = PARSE_FLAG_PREPROCESS | PARSE_FLAG_TOK_NUM | PARSE_FLAG_TOK_STR;
  next();
  decl(VT_CONST);
  /* End-of-TU analysis: compute call-graph reachability and the set of
   * static globals with no reachable readers.  Must run before
   * gen_late_reopt_functions so newly-flagged writer functions get picked
   * up by the existing late_reopt loop. */
  if (s1->opt_dead_store)
    tcc_ir_tu_analyze_dead_statics();
  /* Propagate noreturn from callees to callers: for any function whose
   * tokens were preserved by the gen_function noreturn-trigger AND whose
   * call graph now includes a func_noreturn callee, set func_late_reopt
   * so gen_late_reopt_functions re-emits it with noreturn-call DCE.
   *
   * Static inline callees are emitted by gen_inline_functions(), after the
   * first end-of-TU propagation point.  Speculatively kept callers are
   * therefore cleaned up after a second propagation/reopt pass below. */
  if (s1->opt_dce && s1->optimize >= 2)
    tcc_ir_tu_propagate_noreturn_to_callers();
  gen_late_reopt_functions(s1);
  /* tu_static_writer functions were kept in inline_fns (gen_function's auto-
   * inline path) so the end-of-TU dead-static analysis could re-compile them
   * via late_reopt to drop dead static stores.  Those whose static turned out
   * to be live were never flagged func_late_reopt, so they need no re-compile —
   * but they were already emitted standalone at definition time.  Free their
   * tokens and orphan the sym, otherwise gen_inline_functions re-emits the body
   * a second time (the symbol just bumps forward, orphaning the first copy and
   * doubling the function's .text footprint — e.g. a memset(static) inlined to a
   * direct store would emit twice).  Skip functions still flagged for re-emit,
   * kept for noreturn propagation (cleaned up below), or legitimately inlinable
   * (gen_inline_functions already skips those, and their tokens are needed for
   * call-site inlining). */
  for (int fi = 0; fi < s1->nb_inline_fns; fi++)
  {
    struct InlineFunc *ifn = s1->inline_fns[fi];
    if (!ifn || !ifn->sym || !ifn->sym->type.ref)
      continue;
    if (!ifn->sym->type.ref->f.tu_static_writer)
      continue;
    if (ifn->sym->type.ref->f.func_late_reopt || ifn->sym->type.ref->f.func_keep_tokens_for_noreturn ||
        ifn->sym->type.ref->f.func_auto_inline || ifn->sym->type.ref->f.func_eval_only_inline)
      continue;
    if (ifn->func_str)
    {
      tok_str_free(ifn->func_str);
      ifn->func_str = NULL;
    }
    ifn->sym = NULL;
  }
  gen_inline_functions(s1);
  if (s1->opt_dce && s1->optimize >= 2)
  {
    tcc_ir_tu_propagate_noreturn_to_callers();
    gen_late_reopt_functions(s1);
    for (int fi = 0; fi < s1->nb_inline_fns; fi++)
    {
      struct InlineFunc *ifn = s1->inline_fns[fi];
      if (!ifn || !ifn->sym || !ifn->sym->type.ref)
        continue;
      if (!ifn->sym->type.ref->f.func_keep_tokens_for_noreturn)
        continue;
      if (ifn->sym->type.ref->f.func_late_reopt)
        continue; /* re-emit pending — leave alone */
      /* Speculative token-keep is no longer needed.  Free tokens and
       * orphan sym so gen_inline_functions doesn't re-emit. */
      ifn->sym->type.ref->f.func_keep_tokens_for_noreturn = 0;
      if (ifn->func_str)
      {
        tok_str_free(ifn->func_str);
        ifn->func_str = NULL;
      }
      ifn->sym = NULL;
    }
  }
  resolve_pending_aliases();
  check_vstack();
  /* end of translation unit info */
#if TCC_EH_FRAME
  tcc_eh_frame_end(s1);
#endif
  tcc_debug_end(s1);
  tcc_tcov_end(s1);
  return 0;
}

void tcc_bench_log_phase(TCCState *s1, const char *operation, const char *name, unsigned *total_time,
                                unsigned *count, unsigned elapsed)
{
  if (!s1 || !s1->do_bench)
    return;
  *total_time += elapsed;
  (*count)++;
  tcc_bench_log(s1, operation, name, elapsed);
}

ST_FUNC void tccgen_finish(TCCState *s1)
{
  tcc_debug_end(s1); /* just in case of errors: free memory */

  str_lit_pool_free();

  /* Release per-TU function write summaries (Sym* keys are about to become
   * invalid as global_stack is popped). */
  tcc_ir_func_write_summary_clear_all();
  /* Same for the TU-wide read/call summary used by dead-static-store elim. */
  tcc_ir_tu_func_summary_clear_all();
  funcall_scratch_free_all();

  tcc_free(pending_aliases);
  pending_aliases = NULL;
  nb_pending_aliases = 0;

  /* Reclaim inner VLA dimension token streams that were never materialized
     (abstract / function-pointer declarators, e.g. `typedef void(*)(int[][n()])`).
     Consumed ones were already freed and NULLed in func_vla_arg_code. */
  if (s1->vla_inner_exprs)
  {
    for (int i = 0; i < s1->nb_vla_inner_exprs; i++)
      tcc_free(s1->vla_inner_exprs[i]);
    tcc_free(s1->vla_inner_exprs);
    s1->vla_inner_exprs = NULL;
    s1->nb_vla_inner_exprs = 0;
  }

  /* Free any label-difference fixups left over from a symbol/label diff
     (e.g. `int z = &"s"[1] - &"s"[0];`) that appeared in a GLOBAL initializer
     with no enclosing function: gen_function's resolver only runs per function
     body, so a global-only translation unit would leak the fixup node.  We only
     RECLAIM them here, deliberately not re-applying the st_value-difference
     patch: the slot already holds the addend difference written by init_putv,
     and re-resolving at global scope changes that emitted value (the existing
     resolver is meant for in-function computed-goto label diffs).  Leaving the
     value untouched keeps codegen identical to before — this is purely a leak
     fix. */
  {
    LabelDiffFixup *f = s1->label_diff_fixups;
    while (f)
    {
      LabelDiffFixup *next = f->next;
      tcc_free(f);
      f = next;
    }
    s1->label_diff_fixups = NULL;
  }

  /* If compilation aborted while generating a function, the per-function IR
     block allocated in gen_function() may not have been released (because we
     unwind via longjmp). Free it here to avoid leaks on compile errors. */
  if (s1->ir)
  {
    tcc_ir_free(s1->ir);
    s1->ir = NULL;
  }

  free_inline_functions(s1);
  /* Flush stashed static-function IR before sym_pop drops the Sym* keys. */
  ir_inline_stash_flush(s1);
  sym_pop(&global_stack, NULL, 0);
  sym_pop(&local_stack, NULL, 0);
  /* free nested functions array */
  tcc_free(s1->nested_funcs);
  s1->nested_funcs = NULL;
  s1->nb_nested_funcs = 0;
  s1->nested_funcs_capacity = 0;
  /* free preprocessor macros */
  free_defines(NULL);
  /* free sym_pools */
  dynarray_reset(&sym_pools, &nb_sym_pools);
  cstr_free(&initstr);
  dynarray_reset(&stk_data, &nb_stk_data);
  while (cur_switch)
    end_switch();
  local_scope = 0;
  loop_scope = NULL;
  all_cleanups = NULL;
  pending_gotos = NULL;
  nb_temp_local_vars = 0;
  nb_arg_struct_temps = 0;
  arg_struct_temp_busy = 0;
  global_label_stack = NULL;
  local_label_stack = NULL;
  cur_text_section = NULL;
  sym_free_first = NULL;
}
