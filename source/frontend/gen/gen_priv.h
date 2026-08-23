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

/* gen_priv.h -- shared internal state for the source/frontend/gen/ split of
 * the former tccgen.c (parser + type checker + IR-emission frontend).  Every
 * TU under source/frontend/gen/ starts with `#include "gen_priv.h"` and
 * nothing before it: this header must be the one that pulls in tcc.h, so
 * that USING_GLOBALS (below) is defined before tcc.h is processed.
 *
 * See docs/plan_tccgen_split.md for the split this header supports: which
 * file owns which functions, and why each piece of state below had to
 * become an explicit cross-file declaration instead of a file-local static.
 */
#ifndef TCC_GEN_PRIV_H
#define TCC_GEN_PRIV_H

#define USING_GLOBALS
#include "tcc.h"

#include "source/ir/cfg.h"
#include "source/ir/codegen.h"
#include "source/ir/core.h"
#include "source/opt/include/licm.h"
#include "source/opt/include/opt.h"
#include "source/opt/include/opt_utils.h"
#include "source/opt/include/opt_engine.h"
#include "source/opt/include/opt_pipeline.h"
#include "source/opt/include/opt_gens_fusion.h"
#include "opt/flat/bool.h"
#include "opt/flat/call_result.h"
#include "source/ir/regalloc.h"
#include "source/ir/ssa.h"
#include "tccir.h"
#include "source/backend/arch/arm/arm_regalloc.h"
#include "source/opt/function_pipeline.h"
#include "tcc_scope.h"

#include <math.h>

/* ---------------------------------------------------------------------
 * gjmp override.  Every function in this split emits jumps through the
 * "already code sym" (acs) variants defined in core/suppress.c rather than
 * the generic gjmp()/gjmp_addr() declared in tcc.h -- gjmp_acs marks
 * CODE_OFF() after an unconditional forward jump, and gjmp_addr_acs marks
 * the backward-jump target as a jump target for the optimizer.  The
 * original monolithic tccgen.c scoped this override to itself with a
 * #define/#undef pair at the top/bottom of the file; here it is scoped to
 * every TU that includes this header instead, which reproduces the same
 * per-TU behaviour now that the frontend is split across many TUs (macros
 * do not cross translation units, so no #undef is needed at end-of-file).
 * --------------------------------------------------------------------- */
int gjmp_acs(int t);
int gjmp_addr_acs(int a);
#define gjmp_addr gjmp_addr_acs
#define gjmp gjmp_acs

/* ---------------------------------------------------------------------
 * Macros that were file-scope in tccgen.c and are relied on by name
 * throughout the split.
 * --------------------------------------------------------------------- */
#define vstack (_vstack + 1)

#define NODATA_WANTED (nocode_wanted > 0) /* no static data output wanted either */
#define DATA_ONLY_WANTED 0x80000000       /* ON outside of functions and for static initializers */

/* no code output after unconditional jumps such as with if (tcc_state->optimize > 0) ... */
#define CODE_OFF_BIT 0x20000000
#define CODE_OFF()                                                                                                   \
  do                                                                                                                 \
  {                                                                                                                  \
    if (!nocode_wanted)                                                                                             \
    {                                                                                                                \
      nocode_wanted |= CODE_OFF_BIT;                                                                                 \
    }                                                                                                                \
  } while (0)
#define CODE_ON() (nocode_wanted &= ~CODE_OFF_BIT)

/* no code output when parsing sizeof()/typeof() etc. (using nocode_wanted++/--) */
#define NOEVAL_MASK 0x0000FFFF
#define NOEVAL_WANTED (nocode_wanted & NOEVAL_MASK)

/* no code output when parsing constant expressions */
#define CONST_WANTED_BIT 0x00010000
#define CONST_WANTED_MASK 0x0FFF0000
#define CONST_WANTED (nocode_wanted & CONST_WANTED_MASK)

#if PTR_SIZE == 4
#define VT_SIZE_T (VT_INT | VT_UNSIGNED)
#define VT_PTRDIFF_T VT_INT
#elif LONG_SIZE == 4
#define VT_SIZE_T (VT_LLONG | VT_UNSIGNED)
#define VT_PTRDIFF_T VT_LLONG
#else
#define VT_SIZE_T (VT_LONG | VT_LLONG | VT_UNSIGNED)
#define VT_PTRDIFF_T (VT_LONG | VT_LLONG)
#endif

void block(int flags);
#define STMT_EXPR 1
#define STMT_COMPOUND 2

/* parse_init_elem() operand kinds. */
#define EXPR_CONST 1
#define EXPR_ANY 2

/* decl_initializer() flags. */
#define DIF_FIRST 1
#define DIF_SIZE_ONLY 2
#define DIF_HAVE_ELEM 4
#define DIF_CLEAR 8

/* Always defined in this fork (the #ifndef precedence_parser branch that
 * survives in expr/infix.c is dead code kept only because tccgen.c never
 * compiled it either -- see docs/plan_tccgen_split.md). */
#define precedence_parser

/* The operator-precedence parser's entry macros.  These lived in expr/infix.c
 * in the monolithic file, but expr/cond.c's expr_landor() expands
 * expr_landor_next() too, so they must be visible to every gen TU.  These are
 * the `#ifdef precedence_parser` variants, matching the #define above.
 *
 * expr/infix.c keeps its own `#define precedence(i)` direct table lookup for
 * its hot internal uses; expr_precedence() is the out-of-line accessor for
 * that table, since the `prec[]` array itself stays file-local. */
int expr_precedence(int tok);
void expr_infix(int p);
#define expr_landor_next(op) unary(), expr_infix(expr_precedence(op) + 1)
#define expr_lor() unary(), expr_infix(1)

#define MAX_TEMP_LOCAL_VARIABLE_NUMBER 8

/* ---------------------------------------------------------------------
 * Shared types.  Each of these was a file-scope typedef/struct in
 * tccgen.c; they are promoted here verbatim because more than one split
 * file needs the full field layout, not just an opaque pointer.
 * --------------------------------------------------------------------- */

/* current switch statement (stmt/block.c produces it, stmt/switch.c and
 * core/state.c's tccgen_finish() consume it). */
struct switch_t
{
  struct case_t
  {
    int64_t v1, v2;
    int ind, line;
  } **p;
  int n;       /* list of case ranges */
  int def_sym; /* default symbol */
  int nocode_wanted;
  int *bsym;
  struct scope *scope;
  struct switch_t *prev;
  SValue sv;
};

/* list of temporary local variables on the stack in the current function
 * (value/load.c produces/consumes; nested/nested.c and core/state.c save
 * and restore the array across nested-function compilation). */
struct temp_local_variable
{
  int location; // offset on stack. Svalue.c.i
  short size;
  short align;
};

/* init_params: threaded through the whole initializer-parsing family
 * (init/initializer.c, init/alloc.c) and read by value/load.c's gv() for
 * the vla-init fast path. */
typedef struct
{
  Section *sec;
  int local_offset;
  Sym *flex_array_ref;
  /* When non-NULL, init_putv captures pure-constant scalar values into
   * const_init_sym->const_init_data so the values can later be read at
   * compile time (e.g. for __builtin_shuffle masks). Any non-constant
   * element along the way clears const_init_valid on the sym. */
  Sym *const_init_sym;
  int const_init_base;
  /* Probe mode: when const_probe is set, init_putv captures each scalar into
   * const_probe_data (a host-side template buffer of const_probe_size bytes,
   * indexed by element offset minus const_probe_base) and emits nothing.  Any
   * element that is not a plain load-time-constant integer/pointer sets
   * const_probe_failed.  The whole probe runs under nocode_wanted so runtime
   * initializer expressions emit no code, letting the caller fall back to the
   * normal per-element path cleanly.  Used to lower a large constant local
   * array as a single memcpy from a .rodata template (matching GCC) instead of
   * memset + a store per non-zero element. */
  int const_probe;
  int const_probe_failed;
  unsigned char *const_probe_data;
  int const_probe_base;
  int const_probe_size;
} init_params;

/* Stack of saved by-value call arguments for a funcall in progress
 * (core/state.c owns the free-list machinery, builtin/call.c's
 * unary_funcall() pushes/pops entries directly). */
typedef struct FuncallScratch
{
  SValue *saved_args;
  unsigned char **saved_args_cid;
  int *saved_args_cid_size;
  int saved_arg_count;
  struct FuncallScratch *next;
} FuncallScratch;

/* One queued `__attribute__((alias(...)))` target, resolved once the whole
 * TU has been parsed (core/state.c owns the array, sym/attr_merge.c queues
 * and resolves entries). */
typedef struct PendingAliasDef
{
  Sym *alias_sym;
  int target_tok;
} PendingAliasDef;

/* ---------------------------------------------------------------------
 * Cross-file state.  Each of these was `static` (or, for the four data
 * items already covered by tcc.h/tcc_scope.h, plain non-static without a
 * declaration anywhere) in tccgen.c.  Defined once, without `static`, in
 * core/state.c; every other file reaches them through this header.
 *
 * cur_scope/loop_scope/root_scope (defined in core/state.c) and
 * debug_modes are already declared extern by tcc_scope.h and tcc.h
 * respectively and are not repeated here.
 * --------------------------------------------------------------------- */

/* current switch statement -- see "struct switch_t" above.
 * Producer: stmt/block.c's block_1().  Consumers: stmt/switch.c,
 * core/state.c's tccgen_finish(). */
extern struct switch_t *cur_switch;

/* Producer/owner: value/load.c's get_temp_local_var().  Consumers:
 * nested/nested.c (saved/restored around nested-function compilation),
 * core/state.c's tccgen_finish(). */
extern struct temp_local_variable arr_temp_local_vars[MAX_TEMP_LOCAL_VARIABLE_NUMBER];
extern int nb_temp_local_vars;

/* Reusable stack slots for by-value struct arguments passed to variadic
 * functions.  Only the busy bitmask crosses a file boundary: stmt/block.c's
 * block() saves/restores it around every statement so two call expressions
 * that are lexically nested (e.g. inside a GNU statement expression used as
 * an argument) do not alias each other's reserved slots. */
extern uint64_t arg_struct_temp_busy;

/* sym_push()'s bump allocator.  Producer/owner: sym/symtab.c. */
extern Sym *sym_free_first;
extern void **sym_pools;
extern int nb_sym_pools;

/* Producer: builtin/call.c's unary_funcall(), which pushes/pops entries
 * directly (see FuncallScratch above); freed via core/state.c's
 * tccgen_finish(). */
extern FuncallScratch *funcall_scratch_stack;

/* AAPCS invisible-copy handshake: set while typing a call's formal
 * parameters (expr/indir.c's gfunc_param_typed()), consumed while lowering
 * the actual arguments (builtin/call.c's unary_funcall()). */
extern unsigned char *aapcs_last_const_init;
extern int aapcs_last_const_init_size;

/* Producer/consumer: decl/declarator.c's post_type() alone; kept here (not
 * file-local) only because promoting it cost nothing once str_lit_pool
 * needed the same treatment for a handful of others. Recursion-depth guard
 * against pathological parameter-list nesting. */
extern int func_param_decl_depth;

/* Queued `__attribute__((alias(...)))` targets -- see PendingAliasDef
 * above.  Producer: sym/attr_merge.c's queue_alias_symbol(); consumed by
 * the same file's resolve_pending_aliases(), called from
 * core/state.c's tccgen_compile()/tccgen_finish(). */
extern PendingAliasDef *pending_aliases;
extern int nb_pending_aliases;

/* Pending label-difference symbols for &&lab1 - &&lab0 in static
 * initializers.  Producer: op/int.c's gen_opic(); consumer:
 * init/initializer.c's init_putv(). */
extern Sym *pending_label_diff_plus;
extern Sym *pending_label_diff_minus;

/* String-merge scratch buffer for adjacent string-literal initializers.
 * Producer: decl/attribute.c's parse_mult_str(); consumer:
 * init/alloc.c's decl_initializer(); freed in core/state.c's
 * tccgen_finish()/tccgen_init(). */
extern CString initstr;

/* Cleanup-attribute and pending-goto chains, and the current lexical-scope
 * nesting depth.  Non-static in tccgen.c already (no linkage change), but
 * still needed an extern declaration visible outside core/state.c. */
extern Sym *all_cleanups, *pending_gotos;
extern int local_scope;

/* The value stack itself.  tcc.h declares vtop but not the backing array,
 * which the vstack macro above indexes. */
extern SValue _vstack[1 + VSTACK_SIZE];

/* Shared CType singletons built once by tccgen_init().  tcc.h already
 * declares int_type, func_old_type and char_pointer_type; these were
 * tccgen.c-local and are now referenced across the split. */
extern CType char_type;
extern CType func_old_void_type;
extern CType func_old_char_pointer_type;
extern CType func_old_void_pointer_type;
extern CType func_old_size_t_type;

/* Non-static in tccgen.c but never declared in a header, because every
 * caller lived in the same TU. */
void vpush_type_size(CType *type, int *a);
void compile_nested_functions(Sym *parent_sym);
void pop_local_syms(Sym *b, int keep);

/* combine_types() operator classes, shared with op/op.c's gen_op_impl(). */
#define CMP_OP 'C'
#define SHIFT_OP 'S'

/* Defined in source/backend/generators/function.c -- the IR pipeline driver
 * the frontend hands each completed function body to. */
extern void gen_function(Sym *sym);

/* ---------------------------------------------------------------------
 * Cross-file function prototypes.  Each of these was `static` in
 * tccgen.c; the definition (now in the file named in the comment below)
 * has had `static` removed to match.  Grouped by home file, functions
 * alphabetical within each group.
 * --------------------------------------------------------------------- */

/* --- builtin/call.c --- */
void gen_ir_void_call_args(SValue *args, int argc, int func_tok);
void nop_or_rollback_call_params(int call_id, int ir_idx_before_first_param, int ir_idx_before_args);
int redirect_call_to_tcc_helper(SValue *saved_args, int nargs, const char *helper_name, CType *result_type, int call_id, int ir_idx_before_first_param, int ir_idx_before_args);
void unary_funcall(void);

/* --- builtin/chk.c --- */
void unary_builtin_chk(void);

/* --- builtin/fp.c --- */
void unary_builtin_alloca(void);
void unary_builtin_fp(void);
void unary_builtin_modf(void);

/* --- builtin/fp2.c --- */
void unary_builtin_fp2(void);

/* --- builtin/misc.c --- */
int gcc_classify_type(CType *type);
void gen_bitop1(TccIrOp op);
void gen_builtin_libcall(int func_tok, int argc, int ret_type);
void gen_ir_call_args(SValue *args, int argc, int func_tok, CType *ret_ctype);

/* --- builtin/overflow.c --- */
void unary_builtin_overflow(void);

/* --- builtin/simd.c --- */
void unary_builtin_convertvector(void);
void unary_builtin_shuffle(void);

/* --- builtin/string.c --- */
int builtin_abs_decl_matches(Sym *func_sym, const char *func_name);
void gen_inline_abs_from_vtop(int shift_amount, int is_unsigned);
int get_builtin_abs_info(const char *func_name, int *is_unsigned);
const char *try_get_constant_string(SValue *sv, int *out_len);
int try_inline_builtin_call(const char *func_name, SValue *args, int nb_args);
int unary_funcall_opt_string_builtins(int func_tok, const char *func_name, SValue *saved_args, int nb_real_args, int call_id, int ir_idx_before_first_param, int ir_idx_before_args, const CType *ret_type);

/* --- core/predicates.c --- */
void PUT_R_RET(SValue *sv, int t);
int RC_RET(int t);
int RC_TYPE(int t);
int btype_size(int bt);
int is_integer_btype(int bt);

/* --- core/state.c --- */
Sym *find_local_scalar_sym_by_offset(int offset);
void funcall_scratch_pop_free(FuncallScratch *fs);
int get_arg_struct_temp(int size, int align);

/* --- core/suppress.c --- */
int gind();

/* --- decl/attribute.c --- */
void parse_attribute(AttributeDef *ad);
int parse_c23_attribute(AttributeDef *ad);
void parse_decl_attributes(AttributeDef *ad);

/* --- decl/btype.c --- */
int parse_btype(CType *type, AttributeDef *ad, int ignore_label);

/* --- decl/decl.c --- */
int decl(int l);
void do_Static_assert(void);

/* --- decl/declarator.c --- */
int asm_label_instr(void);
void convert_parameter_type(CType *pt);
int post_type(CType *type, AttributeDef *ad, int storage, int td);
CType *type_decl(CType *type, AttributeDef *ad, int *v, int td);

/* --- decl/struct.c --- */
Sym *find_field(CType *type, int v, int *cumofs);
void struct_decl(CType *type, int u);
void struct_layout(CType *type, AttributeDef *ad);

/* --- expr/atomic.c --- */
void parse_atomic(int atok);

/* --- expr/cond.c --- */
void expr_cond_ternary(void);
void expr_landor(int op);

/* --- expr/indir.c --- */
void expr_type(CType *type, void (*expr_fn)(void));
void gfunc_param_typed(Sym *func, Sym *arg);
void parse_builtin_params(int nc, const char *args);
void parse_expr_type(CType *type);
void parse_type(CType *type);

/* --- expr/infix.c --- */
void expr_cond(void);
void expr_const1(void);
void expr_eq(void);
void init_prec(void);

/* --- expr/primary.c --- */
int unary_primary(void);

/* --- expr/unary.c --- */
void unary_generic(void);
int unary_paren(void);

/* --- init/alloc.c --- */
void decl_initializer(init_params *p, CType *type, unsigned long c, int flags, int vreg);
void decl_initializer_alloc(CType *type, AttributeDef *ad, int r, int has_init, int v, int global);

/* --- init/initializer.c --- */
void decl_design_flex(init_params *p, Sym *ref, int index);
int decl_designator(init_params *p, CType *type, unsigned long c, Sym **cur_field, int flags, int al);
void init_assert(init_params *p, int offset);
void init_putv(init_params *p, CType *type, unsigned long c, int vreg);
void init_putz(init_params *p, unsigned long c, int size);
void parse_init_elem(int expr_type);
void skip_or_save_block(TokenString **str);
void sso_swap_struct_init(init_params *p, CType *type, unsigned long c);

/* --- inline/analysis.c --- */
int auto_inline_nonstatic_struct_body_ok(Sym *func_sym, TokenString *func_str);
int auto_inline_param_count(Sym *func_sym);
int auto_inline_sig_ok(Sym *func_sym);
int inline_body_has_apply_args(TokenString *func_str);
int inline_body_has_loops(TokenString *func_str);
int inline_body_has_return_stmt(TokenString *func_str);
int inline_body_has_shadowed_ident(TokenString *func_str);
int inline_body_has_side_effects(TokenString *func_str);
int inline_body_has_static_local(TokenString *func_str);
int inline_body_has_unsafe_loops(TokenString *func_str);
int inline_body_has_unsafe_shadowed_ident(TokenString *func_str, Sym *call_func_sym);
Sym **inline_hide_label_bindings(TokenString *func_str, int **tokens_out, int *count_out);
void inline_restore_label_bindings(int *tokens, Sym **saved_labels, int count);
void inline_release_hidden_label_bindings(void);
void inline_scan_body_features(TokenString *func_str, int *has_addr_of_label, int *has_inline_asm);
int nested_callee_captures_reachable(TCCState *s, Sym *call_func_sym, NestedFunc *current_nf);
int nested_callee_has_genuine_capture(TCCState *s, Sym *call_func_sym);
int nested_capture_is_read_only(NestedFunc *nf);
int nested_has_genuine_capture(NestedFunc *nf);

/* --- inline/const_eval.c --- */
int inline_arg_is_constant_like(const SValue *sv);
void inline_eval_cast_arg_to_param(SValue *sv, const CType *param_type);
int try_inline_const_eval(Sym *func_sym, SValue *args, int nb_args);

/* --- inline/emit.c --- */
void free_inline_functions(TCCState *s);
Section *function_text_section(TCCState *s1, Sym *sym);
void gen_inline_functions(TCCState *s);
void gen_late_reopt_functions(TCCState *s);
void ir_inline_stash_flush(TCCState *s1);

/* --- nested/nested.c --- */
void prescan_captured_vars(NestedFunc *nf, Sym *parent_local_stack, NestedFunc *explicit_parent_nf);
void prescan_vla_param_captured_vars(NestedFunc *nf, Sym *parent_local_stack);
void setup_nested_func_trampoline(Sym *s);

/* --- op/complex.c --- */
void gen_complex_conjugate(void);
void gen_complex_float_arith(int op);
void gen_complex_float_cmp(int op);
void gen_complex_float_mul(int op);
void gen_complex_int_arith(int op);
void gen_complex_int_cmp(int op);

/* --- op/float.c --- */
void gen_opif(int op);

/* --- op/fold_math.c --- */
int chk_get_conservative_sprintf_bytes(int tok, int fmt_idx, SValue *all_args, int total_args, unsigned long long *out_bytes);
double get_const_double(SValue *sv);
float get_const_float(SValue *sv);
void inline_subst_const_arg(SValue *sv);
int is_const_for_folding(SValue *sv);
void objsize_vreg_fact_record(TCCIRState *ir, int vreg, int max_valid, unsigned long long max_value, int strlen_valid, unsigned long long strlen_value);
int svalue_get_conservative_max_u64(SValue *sv, unsigned long long *out_max);
int svalue_get_conservative_string_bytes_u64(SValue *sv, unsigned long long *out_max);
int try_fold_complex_call(const char *func_name, SValue *args, int nb_args);
int try_fold_math_call(const char *func_name, SValue *args, int nb_args);
void update_local_scalar_max_bound(SValue *dst, SValue *src);

/* --- op/int.c --- */
void gen_opic(int op);
uint64_t value64(uint64_t l1, int t);

/* --- op/vector.c --- */
void attach_const_init_to_temp(int frame_offset, int size, const unsigned char *data);
unsigned char *find_sv_const_init(const SValue *sv, int min_size);
unsigned char *find_sv_vec_literal_init(const SValue *sv, int min_size);
void gen_op_vector(int op);
void gen_vec_subscript(void);
int is_vector_type(const CType *type);
void make_vector_type(CType *out, const CType *elem_type, int vector_bytes);
int64_t read_vec_const_elem(const unsigned char *data, int elem_size, int idx, int is_unsigned);
void vec_emit_range_record(int start, int end);
void vec_recipe_kill_slot(int vr);
int vector_elem_count(const CType *vec);

/* --- stmt/cleanup.c --- */
void block_cleanup(struct scope *o);
void try_call_cleanup_goto(Sym *cleanupstate);
void try_call_scope_cleanup(Sym *stop);

/* --- stmt/ret.c --- */
void check_func_return(void);
void gfunc_return(CType *func_type);

/* --- stmt/switch.c --- */
int case_cmp(uint64_t a, uint64_t b);
void case_sort(struct switch_t *sw);
void end_switch(void);
int gcase(struct case_t **base, int len, int dsym);
int gcase_jump_table(struct switch_t *sw, int dsym);
int switch_can_use_jump_table(struct switch_t *sw);

/* --- store/struct_copy.c --- */
void ir_emit_struct_unit_copy(const SValue *src, const SValue *dst, const CType *stype, int size);
int struct_has_bitfield_member(const CType *type);
int struct_has_vla_member(const CType *type);
int struct_is_single_1byte_scalar_member(const CType *type);
int struct_is_single_2byte_scalar_member(const CType *type);
int struct_is_small_bitfield_word(const CType *type);
int struct_member_copy_safe(const CType *type);

/* --- sym/attr_merge.c --- */
void apply_alias_attribute(Sym *alias_sym, int target_tok);
Sym *external_sym(int v, CType *type, int r, AttributeDef *ad);
void merge_attr(AttributeDef *ad, AttributeDef *ad1);
void merge_funcattr(struct FuncAttr *fa, struct FuncAttr *fa1);
void merge_symattr(struct SymAttr *sa, struct SymAttr *sa1);
void patch_storage(Sym *sym, AttributeDef *ad, CType *type);
void resolve_pending_aliases(void);
void sym_copy_ref(Sym *s, Sym **ps);

/* --- sym/symtab.c --- */
Sym *sym_malloc(void);
int sym_scope(Sym *s);
int token_stream_references_local_object(const int *p);

/* --- type/assign_check.c --- */
void cast_error(CType *st, CType *dt);
void gen_assign_cast(CType *dt);
int is_compatible_types(CType *type1, CType *type2);
int is_compatible_unqualified_types(CType *type1, CType *type2);
int mark_value_bytes(CType *type, int base, unsigned char *map, int map_size);
void verify_assign_cast(CType *dt);

/* --- type/cast.c --- */
void force_charshort_cast(void);
void gen_cast(CType *type);
void gen_cast_s(int t);

/* --- type/compare.c --- */
int combine_types(CType *dest, SValue *op1, SValue *op2, int op);
int compare_types(CType *type1, CType *type2, int unqualified);
int compare_types_structural(CType *type1, CType *type2);
CType *find_assignable_transparent_union_member(CType *type);
int is_null_pointer(SValue *p);
int is_transparent_union_type(CType *type);
int pointed_size(CType *type);
void type_incompatibility_error(CType *st, CType *dt, const char *fmt);
void type_to_str(char *buf, int buf_size, CType *type, const char *varstr);

/* --- type/size.c --- */
int compute_aapcs_natural_alignment(const CType *type);
CType *pointed_type(CType *type);

/* --- value/load.c --- */
int adjust_bf(SValue *sv, int bit_pos, int bit_size);
void gbound(void);
void gen_bounded_ptr_add(void);
int get_temp_local_var(int size, int align, int *vr_out);
void gv_dup(void);
void incr_bf_adr(int o);
void incr_offset(int offset);
void move_reg(int r, int s, int t);
void store_packed_bf(int bit_pos, int bit_size);

/* --- value/longlong.c --- */
void gen_opl(int op);
void lbuild(int t);

/* --- value/strlit_pool.c --- */
void str_lit_pool_free(void);
void str_lit_pool_merge(addr_t pre_off);
void str_lit_pool_reset(void);

/* --- value/vstack.c --- */
void check_nonvoid_value(void);
void gen_test_zero(int op);
void gvtst_set(int inv, int t);
void vdup(void);
void vpush(CType *type);
void vpush64(int ty, unsigned long long v);
void vpush_ref(CType *type, Section *sec, unsigned long offset, unsigned long size);
void vpushll(long long v);
void vpushs(addr_t v);
void vset_VT_JMP(void);
void vsetc(CType *type, int r, CValue *vc);
void vseti(int r, int v);

#endif /* TCC_GEN_PRIV_H */
