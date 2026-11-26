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
/* No static-storage bytes wanted: NODATA_WANTED, or a dropped static is being
 * parsed for its diagnostics only (check_only).  That cannot be a nocode_wanted
 * bit: its initializers need DATA_ONLY_WANTED (the sign bit) to fold as static
 * initializers do, and its function bodies generate code (into an IR that is
 * freed) -- so locals are laid out as ever, and only what would land in a
 * section is held back. */
#define NOSTATIC_WANTED (NODATA_WANTED || tcc_state->check_only)
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
int expr_infix(int p);
int expr_unary_infix(int p);
void check_parentheses(int t, int l, int r);
void fe_check_same_operands(int op);
/* a token span's hash (fe_tokfp_*, expr/infix.c): "the same expression" */
typedef struct FeSpan
{
  uint64_t h;
  unsigned n;
} FeSpan;
void fe_span_begin(FeSpan *s);
void fe_span_end(FeSpan *s);
int fe_span_same(const FeSpan *a, const FeSpan *b);
void fe_note_sizeof(const SValue *op, const CType *type, const FeSpan *span); /* builtin/call.c */
void fe_bitop_reset(void);
int fe_narrow_fits(const SValue *sv, int dtt);
void fe_narrow_pend_begin(const SValue *arm);
void fe_narrow_pend_end(void);
void fe_narrow_note_type(int ret);
void fe_note_flex_member(void);
int fe_array_ptr_bounds(SValue *sv, int64_t *k, char *tbuf, int tsize);
void fe_check_deref_bounds(int allow_end);
#define expr_landor_next(op) expr_unary_infix(expr_precedence(op) + 1)
#define expr_lor() expr_unary_infix(1)

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
  int body_depth; /* the compound statement depth of its body (stmt/block.c) */
  int ytu;        /* yasc: a switch on tagged union yasc_tagged[ytu - 1]'s tag, 0 none */
  int def_unreachable; /* yasc: `default: unreachable` -- the case checks run as with no default */
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
  /* flex_array_ref is a struct member's flexible array inside a union:
   * at most flex_array_max elements fit in the union's storage. */
  int flex_array_bounded;
  int flex_array_max;
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
  /* The element being initialized is a member of a big-endian
   * scalar_storage_order aggregate (or an element of such an array member):
   * init_putv writes its scalars byte-reversed.  Set per member by
   * decl_designator from the field's own mark. */
  int sso_be;
  /* -Woverride-init: the bits each designator has initialized so far
   * (decl_designator), allocated on first use; init_ovr_free releases it. */
  struct InitOvr *ovr;
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
extern Sym label_diff_marker;
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
void nested_track_captured_sym(Sym *s);
void pop_local_syms(Sym *b, int keep);

/* --- op/op.c: the frontend's constant and type checks --- */
void gen_op_checked(int op);
/* op/ubsan.c (-fsanitize=..., -ftrapv) */
int ubsan_gen_op(int op);
int ubsan_gen_neg(void);
void ubsan_deref(void);
/* the access through a checked lvalue, as clang's type_mismatch kinds */
#define UBSAN_STORE 1
#define UBSAN_MEMBER 3
void ubsan_access_kind(const SValue *lv, int kind);
void ubsan_subscript(int addr_ok);
void ubsan_vla_bound(void);
void gen_unreachable(int checked);
void ssp_note_local(const CType *type);
void ssp_note_frame_use(int alloca_);
void fe_check_neg(void);
int fe_eval_checks_wanted(void);
int fe_int_const(const SValue *sv);
int fe_int_bits(int t);
int fe_int_unsigned(int t);
int64_t fe_const_value(const SValue *sv, int *big);
/* type/assign_check.c: `x op= y` records y for the conversion check */
extern SValue fe_compound_rhs;
extern int fe_compound_pending;
extern int fe_conv_quiet;

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

/* --- builtin/fortify.c --- */
typedef struct FortifyCall FortifyCall;
extern FortifyCall *fortify_cur_call; /* unary_funcall -> unary_funcall_body */
FortifyCall *fortify_call_begin(Sym *fsym);
void fortify_arg_before(FortifyCall *fc, int i);
void fortify_arg(FortifyCall *fc, int i);
void fortify_arg_emit(FortifyCall *fc, int i);
void fortify_call_end(FortifyCall *fc);
void fortify_calls_unwind(void *mark);
void *fortify_calls_mark(void);

/* --- builtin/chk.c --- */
void unary_builtin_chk(void);

/* --- builtin/objsize.c --- */
/* What the value being parsed points into, while objsize_ctx says a pointer's
   object is wanted (a __builtin_object_size argument, a checked call's
   destination): set by an identifier, a member access or an allocation's
   result, carried through [], &, *, casts and pointer +/- an integer. */
typedef struct ObjRec
{
  SValue snap;                 /* the value it describes (r, vr, c, sym) */
  long long off;               /* bytes from the start of a heap object / VLA */
  long long sub_size, sub_off; /* the closest member: its size, the offset in it */
  ObjHeap h;                   /* OR_HEAP: the allocation */
  Sym *base;                   /* the followed variable, or the VLA */
  unsigned char on, kind, form, sub, sub_flex, off_unknown, param;
} ObjRec;
#define OR_HEAP 1
#define OR_VLA 2
#define OR_DECL 3 /* base: a declared object */
#define OR_VAL 0  /* the value (an rvalue pointer, an array) points in */
#define OR_ADDR 1 /* the lvalue's address is in */
typedef struct ObjInfo
{
  unsigned char kind; /* OI_* */
  unsigned char known0, known1, off_unknown;
  unsigned long long max0, min0, max1, min1; /* bytes left: whole object, closest subobject */
  long long off;
  Sym *obj; /* OI_DECL: the object (anonymous for a literal) */
  Sym *vla;
  ObjHeap h; /* OI_HEAP */
} ObjInfo;
#define OI_UNKNOWN 0
#define OI_DECL 1
#define OI_HEAP 2
typedef struct ObjOpHook
{
  long long delta;
  unsigned char active, unknown, mul, risky;
} ObjOpHook;
typedef struct ObjCall ObjCall;
extern int objsize_ctx, objsize_loop_hdr;
extern ObjRec objsize_rec;
extern ObjCall *objsize_cur_call;
int objsize_rec_is(const SValue *sv);
void objsize_enter(ObjRec *saved);
void objsize_leave(const ObjRec *saved);
void objsize_store(SValue *dst, SValue *src);
void objsize_addr_taken(Sym *s);
void objsize_label(void);
void objsize_ident(Sym *s);
void objsize_param(void);
int objsize_member_begin(ObjRec *saved);
void objsize_member_end(const ObjRec *saved, int had, int arrow, Sym *field, int cumofs, int flex);
void objsize_after_indir(void);
void objsize_after_addr(int was_array);
int objsize_op_begin(ObjOpHook *h, int op);
void objsize_op_end(ObjOpHook *h);
void objsize_info(SValue *sv, ObjInfo *oi);
unsigned long long objsize_result(const ObjInfo *oi, int type);
int objsize_push_dynamic(const ObjInfo *oi, int type);
void objsize_apply_alloc_attrs(Sym *s, SymLocalFacts *f, AttributeDef *ad);
void fortify_apply_attrs(Sym *s, SymLocalFacts *f, AttributeDef *ad); /* builtin/fortify.c */
ObjCall *objsize_call_begin(Sym *fsym);
ObjCall *objsize_call_begin_named(const char *name);
void objsize_call_arg_before(ObjCall *oc, int i);
void objsize_call_arg(ObjCall *oc, int i);
void objsize_call_args_end(ObjCall *oc);
void *objsize_calls_mark(void);        /* error recovery: the calls begun */
void objsize_calls_unwind(void *mark); /* to MARK, NULL: all */
void objsize_call_end(ObjCall *oc, int discarded);
int objsize_value_heap(SValue *sv, ObjHeap *h);
void objsize_rhs_begin(ObjRec *saved);
void objsize_rhs_end(const ObjRec *saved);
void objsize_cond_result(const ObjHeap *t, const ObjHeap *f);
void objsize_alloca_result(SValue *size);
void objsize_chk_overflow(ObjCall *oc, const char *name, unsigned long long n, unsigned long long size);
int objsize_bound_fixit(ObjCall *oc, int line);

typedef struct AbSub
{
  struct AbLoop *loop;
  long long idx;
  int n, line;
  char type[64];
} AbSub;
extern int objsize_ab_watch;
#define OS_FOR_COND 2
#define OS_FOR_STEP 3
#define OS_FOR_BODY 4
void objsize_for_begin(void);
void objsize_for_phase(int phase);
void objsize_for_end(void);
void objsize_branch(void);
void objsize_branch_stmt(int t);
void objsize_ab_op(int op, int after);
void objsize_ab_store(SValue *dst, SValue *src);
int objsize_ab_subscript_begin(AbSub *ix, int flex);
void objsize_ab_subscript_end(AbSub *ix, int one_past_ok);

/* --- stmt/block.c (objsize) --- */
int block_declared_in_loop(Sym *s);

/* --- builtin/fp.c --- */
void unary_builtin_alloca(void);
void unary_builtin_fp(void);
void unary_builtin_modf(void);

/* --- builtin/fp2.c --- */
void unary_builtin_fp2(void);
void unary_at_builtin(void);
void gen_bswap(int size);

/* --- builtin/misc.c --- */
int gcc_classify_type(CType *type);
void gen_bitop1(TccIrOp op);
void gen_builtin_libcall(int func_tok, int argc, int ret_type);
int gen_ir_call_args(SValue *args, int argc, int func_tok, CType *ret_ctype);

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

/* --- core/suppress.c --- */
int gind();

/* --- decl/attribute.c --- */
void parse_attribute(AttributeDef *ad);
int parse_c23_attribute(AttributeDef *ad);
void parse_decl_attributes(AttributeDef *ad);

/* --- decl/btype.c --- */
int parse_btype(CType *type, AttributeDef *ad, int ignore_label);
void parse_btype_qualify(CType *type, int qualifiers);

/* --- decl/decl.c --- */
int decl(int l);
void do_Static_assert(void);
int tok_is_static_assert(void);

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
void btype_no_auto(AttributeDef *ad);
void parse_type(CType *type);

/* --- expr/infix.c --- */
void expr_cond(void);
void expr_const1(void);
void expr_eq(void);
int nrvo_rhs_is_direct_call(void);
void init_prec(void);

/* --- expr/primary.c --- */
int unary_primary(void);

/* --- expr/unary.c --- */
extern int unary_addr_operand;
int fe_subscript_bounds(int64_t *idx, char *tbuf, int tsize);

extern int indir_keep_lvalue;
void unary_generic(void);
int unary_paren(void);

/* --- init/alloc.c --- */
void decl_initializer(init_params *p, CType *type, unsigned long c, int flags, int vreg);
void decl_initializer_alloc(CType *type, AttributeDef *ad, int r, int has_init, int v, int global);
ST_FUNC void finalize_tentative_definitions(TCCState *s1);
ST_FUNC void free_tentative_definitions(TCCState *s1);

/* --- init/initializer.c --- */
void decl_design_flex(init_params *p, Sym *ref, int index);
int decl_designator(init_params *p, CType *type, unsigned long c, Sym **cur_field, int flags, int al);
void init_assert(init_params *p, int offset);
void init_putv(init_params *p, CType *type, unsigned long c, int vreg);
void init_putz(init_params *p, unsigned long c, int size);
void parse_init_elem(int expr_type);
void init_ovr_free(init_params *p);
void init_ovr_quiet(init_params *p);
extern struct InitOvr *init_ovr_top;
void init_ovr_unwind(struct InitOvr *to);
/* -Wuninitialized: the local whose initializer is being parsed, [lo, hi) of
 * the frame or its vreg (init_self_on in tcc.h: ir/gen/put.c reports an operand read
 * from it while its initializer expression is parsed) */
extern int init_self_lo, init_self_hi, init_self_tok, init_self_vreg, init_self_depth;
void init_self_push(int addr, int size, int tok, int vreg);
void init_self_pop(void);
void skip_or_save_block(TokenString **str);
extern int skip_save_stop_semicolon; /* skip_or_save_block: a ';' ends the block (a static's initializer) */

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
int inline_body_leaf_ops(TokenString *func_str, int depth);
int inline_body_has_large_local_array(TokenString *func_str, int limit);
int inline_body_has_unsafe_loops(TokenString *func_str);
int inline_body_has_unsafe_shadowed_ident(TokenString *func_str, Sym *call_func_sym);
void inline_record_def_bindings(InlineFunc *fn, NestedFunc *nf);
Sym **inline_hide_label_bindings(TokenString *func_str, int **tokens_out, int *count_out);
void inline_restore_label_bindings(int *tokens, Sym **saved_labels, int count);
void inline_release_hidden_label_bindings(void);
int inline_hidden_label_depth(void);
void inline_unwind_hidden_labels(int to);
/* error recovery (core/recover.c) */
extern int defer_body_level, struct_body_depth;
/* the declarator decl() is reading: an error in it still declares the name
   (`int y[ ;`), as gcc.  l is VT_CONST or VT_LOCAL while type_decl runs
   (0 otherwise); v, type, scope and func are set when the name is read. */
typedef struct RecoverDecl
{
  int l, v, scope, storage, func;
  CType type;
} RecoverDecl;
extern RecoverDecl recover_decl;
/* the declaration list decl() is reading (VT_CONST or VT_LOCAL): an error in
   one of its declarators goes on with the next one after the ',' (`int
   a[1] = {1, 2}, b;` still declares b), as gcc.  Pointers into decl()'s
   frame, read by the error while that frame is there. */
typedef struct RecoverDeclList
{
  struct RecoverDeclList *outer;
  CType *btype;
  AttributeDef *adbase;
  struct scope *scope; /* cur_scope when it began */
  int l, oldint, exported, struct_depth;
  int active;          /* in a declarator or its initializer */
  int init_base;       /* init_brace_depth when the declarator began */
} RecoverDeclList;
/* the initializer braces decl_initializer is in (an error leaves it up:
   the recovery reads how many it was in, then puts it back) */
extern int init_brace_depth;
extern RecoverDeclList *recover_decl_list;
void decl_list_unwind(RecoverDeclList *mark); /* decl.c: back to a recovery point's list */
void decl_list_reset(void);                   /* the end of the unit */
/* set by the recovery for the next decl(L): the list goes on with its next
   declarator, of this base type */
typedef struct RecoverDeclResume
{
  int on, l, oldint, exported;
  CType btype;
  AttributeDef adbase;
} RecoverDeclResume;
extern RecoverDeclResume recover_decl_resume;
extern int shadow_name_sys; /* sym/symtab.c: -Wshadow, the name came from a system header's macro */
extern int shadow_param_sys; /* the same for the parameter type_decl read last */
void attr_checks_recover(void);
void *call_arg_checks_mark(void);        /* builtin/call.c */
void call_arg_checks_unwind(void *mark); /* to MARK, NULL: all */
int yasc_methods_depth(int set);
void decl_param_names(int set, int *nb, int *base);
void yasc_plain_reset(void);
/* decl/plain_param.c */
extern int yasc_pp_live;
int yasc_pp_hold(int line, CType *target);
/* "T *", the plain pointer to TARGET, as its error names it (decl/declarator.c) */
void yasc_plain_spell(char *buf, int size, CType *target);
extern int yasc_pp_local_decl, yasc_pp_binding;
int yasc_pp_hold_local(int line, CType *target);
int yasc_pp_local_begin(void);
void yasc_pp_local_end(int mark, int tok, int keep);
void yasc_pp_local_bind(Sym *sym);
void yasc_pp_redundant_view(CType *type, int keep);
extern int yasc_pp_view_line;
void yasc_pp_name(int mark, int tok, int idx);
int yasc_pp_mark(void);
void yasc_pp_declarator(Sym *def);
void yasc_pp_rekey(Sym *to);
int yasc_pp_waiting(Sym *def);
int yasc_pp_body_begin(Sym *sym);
void yasc_pp_body_end(int mark);
void yasc_pp_recover(int body);
void yasc_pp_finish(void);
void yasc_pp_arg(const SValue *sv, Sym *fsym, Sym *param, int idx);
void yasc_pp_store(const SValue *dst, const SValue *val);
void yasc_pp_return(const SValue *sv);
void yasc_pp_compare(void);
void block_recover_get(Sym **lt_loop, Sym **lt_switch, int *depth);
void block_recover_set(Sym *lt_loop, Sym *lt_switch, int depth);
void tccgen_decls_recovering(TCCState *s1);
void inline_scan_body_features(TokenString *func_str, int *has_addr_of_label, int *has_inline_asm);
int inline_body_asm_is_machine(TokenString *func_str);
int inline_body_writes_ident(TokenString *func_str, int ident_tok);
TokenString *inline_body_guard_head(TokenString *func_str, Sym *sym, int **free_names);
int guard_head_captures_local(const int *free_names);
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
void gen_owed_inline_functions(TCCState *s);
InlineFunc *inline_fn_lookup(TCCState *s, Sym *sym);
void guard_head_register(TCCState *s, Sym *sym, TokenString *head, const int *pack, int *free_names);
InlineFunc *guard_head_lookup(TCCState *s, Sym *sym);
void gen_deferred_function_bodies(TCCState *s);
void free_deferred_functions(TCCState *s);
void define_deferred_function(DeferredFunc *d);
void define_deferred_data(DeferredData *d);
void check_dropped_deferred_data(DeferredData *d, TokenString *init);
void check_dropped_function(DeferredFunc *d, TokenString *body);
void check_dropped_statics(TCCState *s);
void check_uncompiled_inline_functions(TCCState *s);
void gc_unreferenced_statics(TCCState *s);
void free_dropped_statics(TCCState *s);
void gen_function_check_only(Sym *sym);
void vla_param_exprs_free(TCCState *s1);
void tcc_check_scratch_begin(TCCState *s);
TokenString *tok_str_clone(TokenString *str);
void prune_unused_statics(TCCState *s);
int called_once_budget_begin(int own_len);
void called_once_budget_end(int saved);
void gen_late_reopt_functions(TCCState *s);
int sym_is_const_object(Sym *sym);

/* --- nested/nested.c --- */
void prescan_captured_vars(NestedFunc *nf, Sym *parent_local_stack, NestedFunc *explicit_parent_nf);
void prescan_vla_param_captured_vars(NestedFunc *nf, Sym *parent_local_stack);
void nested_define(NestedFunc *nf);
void nested_record_inherited(NestedFunc *nf);
void nested_note_block_proto(int v);
void nested_scope_end(int level);
void nested_names_reset(void);
void nested_func_release(NestedFunc *nf);
void nested_names_free(void);
void setup_nested_func_trampoline(Sym *s);
int nested_chain_level(const NestedFunc *callee);

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
void const_init_forget_all(void);
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

/* --- decl/attribute.c: pointer facts from GCC attributes --- */
void attr_apply_pointer_facts(CType *type, AttributeDef *ad, int own_functype);
void attr_apply_member_facts(CType *type, AttributeDef *ad, int v);
void attr_apply_decl_facts(Sym *s, AttributeDef *ad);
void attr_apply_decl_facts_at(Sym *s, AttributeDef *ad, int line);
void attr_decl_place(SymLocalFacts *f, int line);

/* --- expr/attr_checks.c --- */
/* A format(printf|scanf) call being parsed (builtin/call.c): the formats
   and the arguments after them are kept and checked at the call's end */
typedef struct FmtArgRec
{
  CType type; /* before the conversion to the parameter */
  int line;
} FmtArgRec;
typedef struct FmtCheck
{
  unsigned char kind, idx, first; /* ATTR_FMT_*, the format's argument, the first formatted (from 1) */
  unsigned char state;            /* 0 off, 1 before the format, 2 checking arguments, 3 a va_list */
  unsigned char saved_armed, saved_seen, nonliteral, nfmts;
  char **fmts;     /* the literal, or each arm of a conditional (`c ? "%d" : _("%s")`) */
  FmtArgRec *args; /* the arguments from `first` on */
  int nargs, fmt_line;
  /* format_arg(n) (gettext) inside another call's format argument: the
     literal argument n is, for the result */
  unsigned char fa_idx, fa_saved_seen;
  Sym *fa_saved_sym;
  char *fa_str;
} FmtCheck;
extern int attr_deprecated_suppress;
void attr_deprecated_use(Sym *s);
void attr_dep_call_flush(void);
int attr_dep_call_take(Sym *fsym);
void attr_dep_call_report(Sym *fsym, int line, ObjCall *oc);
int attr_btype_begin(void);
void attr_btype_end(int mark, AttributeDef *ad);
void attr_call_result(Sym *fsym);
void attr_check_discard(void);
void attr_format_capture(Sym *lit, int wide);
int attr_format_capturing(void);
FmtCheck *attr_format_begin(Sym *fsym);
void attr_format_before(FmtCheck *fc, int argno);
void attr_format_arg(FmtCheck *fc, int argno);
void attr_format_end(FmtCheck *fc, int nb_args);
void *attr_format_cond_begin(void);
void attr_format_cond_arm(void *arms, int live);
void attr_format_cond_end(void *arms);
void attr_checks_reset(void);

/* --- decl/module.c --- */
extern int yasc_exporting;
int yasc_in_main_file(void);
int yasc_file_scope_decl(void);
int yasc_module_linkage(CType *type, int v, int exported);
void yasc_module_sym(Sym *s, int exported);
void yasc_export_tag(Sym *s);
extern int yasc_export_object;
void yasc_ymi_compiler_id(char *buf, int size);
void yasc_module_finish(TCCState *s1);
void yasc_module_reset(void);
void yasc_check_export_members(Sym *s);

/* --- decl/method.c --- */
extern int yasc_method_cur_kind, yasc_method_cur_tag, yasc_method_cur_name;
extern int funcall_self_pending;
extern SValue funcall_self_arg;
void yasc_methods_reset(void);
void yasc_struct_body_begin(void);
void yasc_struct_body_end(void);
int yasc_method_member(Sym *tag);
void yasc_methods_struct_done(Sym *tag);
int yasc_methods_pending(void);
void yasc_methods_flush(void);
void yasc_method_marker(void);
Sym *yasc_method_find(Sym *tag, int name);
int yasc_method_name_of(int fn);
int yasc_method_name_known(int name);
int yasc_method_postfix(Sym *tag, int arrow);
Sym *yasc_method_static(int t, Sym *s);
TokenString *yasc_methods_strip(TokenString *str);
void unary_take_address(void);
/* -Waddress-of-packed-member (expr/unary.c): a member placed below its
   type's alignment -- the last one accessed (`sv`: the value it left, to
   recognise it), and the one the last '&' took (SValue.packed_addr) */
typedef struct PackedMember
{
  SValue sv;     /* the lvalue the access left (type, place) */
  CType st;      /* the struct (or union) type */
  int align;     /* what the member's place guarantees */
  int set;
} PackedMember;
extern PackedMember packed_member_last, packed_member_addr;
int packed_member_is_vtop(void);
/* -Wcast-align (expr/unary.c): the array member accessed last and what its
   place guarantees (`align`), for `(u32 *)s->buf` */
extern PackedMember array_member_last;
void packed_conversion_check(CType *dt);
void explicit_cast_checks(CType *dt);

/* --- stmt/defer.c --- */
/* A cleanup entry's type.t: 0 is __attribute__((cleanup)) (cleanup_func,
 * prev_tok = the variable); a yasc defer keeps its body's tokens in
 * defer_str, the local_stack top where it was written in prev_tok and the
 * number of times it was replayed in vreg. */
#define CLEANUP_DEFER 1
#define CLEANUP_ERRDEFER 2 /* runs only when the function returns an error */
#define CLEANUP_BCHECK 3   /* -b: drops a local's region (op/bcheck.c, bcheck_cleanup) */
int defer_body_active(void);
void defer_stmt(int kind);
void defer_replay(Sym *cls);
void defer_free_entries(Sym *from, Sym *to);
void defer_settle_return_value(void);
extern int cleanup_error_exit;

/* --- stmt/block.c: returns --- */
extern int stmt_diverges;
/* -Wimplicit-fallthrough: how the statement block() last parsed ends */
enum
{
  FT_LABEL,    /* a label, or nothing yet: a case right after it is an empty case */
  FT_FALLS,    /* may complete: the next case is reached by falling through */
  FT_DIVERGES, /* cannot complete (return, break, goto, noreturn call; all arms) */
  FT_ATTR      /* [[fallthrough]]; / __attribute__((fallthrough)); */
};
extern int stmt_ft, stmt_ft_line;
extern int attr_fallthrough_seen;
extern int expr_eq_assigned;
int fe_check_on(unsigned char opt);
int fe_checks_wanted(void);
void gen_return_emit(int b, int error_exit);
void gen_return_jump(void);

/* --- expr/yasc_errors.c --- */
void yasc_errors_reset(void);
int type_is_errset(CType *type);
const char *yasc_errset_name(CType *type);
void yasc_error_type(CType *type);
void yasc_errunion_wrap(CType *type);
/* --- expr/yasc_slices.c --- */
void yasc_slice_wrap(CType *type, int sentinel);
int yasc_slice_to_str(char *buf, int buf_size, CType *type, const char *varstr);
void yasc_slice_assign(CType *dt);
void yasc_slice_member(void);
int yasc_subscript(void);
extern int yasc_sentinel_live;  /* a [:0] slice's element is noted: vstore checks a store into it */
void yasc_sentinel_store(void); /* vstore: no non-zero over the sentinel */
void yasc_sentinel_reset(void); /* a function's start */
void fe_trap_if(void); /* op/ubsan.c: vtop a condition -> popped, a trap on its true side */
/* yasc cast<...> runtime checks (op/ubsan.c) */
void yasc_cast_check_int(long long lo, unsigned long long hi);
void yasc_cast_check_float(long double lo, int lo_strict, long double hi);
void yasc_cast_check_enum(const long long *vals, int n);
void yasc_cast_check_align(int n);
/* type/yasc_cast.c */
int yasc_cast_expr(void);
extern int yasc_cast_type_plain; /* decl/declarator.c */
void yasc_old_style_cast(CType *dt);
/* type/assign_check.c */
int cast_source_place_align(CType *st);
void yasc_errunion_discarded(void);
void yasc_verify_errors_assign(CType *dt);
void yasc_try(void);
void yasc_catch(void);
void yasc_return_stmt(void);
int yasc_void_union_falls_off(void);
int yasc_unary(void);
void yasc_errors_emit(TCCState *s1);

/* --- stmt/ret.c --- */
void check_func_return(void);
void gfunc_return(CType *func_type);
void gen_return_value_now(void);
void gen_instrument_call(Sym *cur_func_sym, const char *hook_name);

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
#define SMALL_AGGREGATE_COPY_MAX 16
int small_unaligned_copy_plan(const CType *stype, int size, int align, int *w_out, unsigned char *covered);
void ir_emit_small_unaligned_copy(const SValue *src, int src_deref, const SValue *dst, int dst_deref, int size,
                                  const unsigned char *covered);
int small_aggregate_copy_plan(const CType *stype, int size, int align, int src_off, int dst_off, int *w_out,
                              unsigned char *covered);
void ir_emit_small_aggregate_copy(const SValue *src, int src_deref, const SValue *dst, int dst_deref, int size,
                                  int w, const unsigned char *covered);
int ir_emit_small_padded_local_copy(const SValue *src, const SValue *dst, const CType *stype, int size, int align);

/* --- sym/attr_merge.c --- */
void apply_alias_attribute(Sym *alias_sym, int target_tok);
Sym *external_sym(int v, CType *type, int r, AttributeDef *ad);
void merge_attr(AttributeDef *ad, AttributeDef *ad1);
void merge_funcattr(struct FuncAttr *fa, struct FuncAttr *fa1);
void merge_symattr(struct SymAttr *sa, struct SymAttr *sa1);
void patch_storage(Sym *sym, AttributeDef *ad, CType *type);
void resolve_pending_aliases(void);
void mark_pending_alias_targets_used(void);
int is_pending_alias_target(Sym *sym);
void regpair_adapt_pending_aliases(void);
void regpair_store_call_result(CType *type, int addr);
Sym *canonical_global_sym(Sym *sym);
void sym_copy_ref(Sym *s, Sym **ps);

/* --- sym/symtab.c --- */
Sym *sym_malloc(void);
int sym_scope(Sym *s);
void check_shadow(Sym *s);
void check_shadow_params(Sym *func);
void shadow_free(void);
int token_stream_references_local_object(const int *p);

/* --- type/assign_check.c --- */
void cast_error(CType *st, CType *dt);
void gen_assign_cast(CType *dt);
int is_compatible_types(CType *type1, CType *type2);
int is_compatible_unqualified_types(CType *type1, CType *type2);
int mark_value_bytes(CType *type, int base, unsigned char *map, int map_size);
void verify_assign_cast(CType *dt);
extern int yasc_returning, yasc_argument;
extern int yasc_argument_c;
extern int yasc_stmt_bol, yasc_unsafe_expr_line; /* the statement read now begins its line; unsafe(expr)'s line */
extern int yasc_rbrace_line;                     /* the line of the last compound statement's '}' */
void yasc_unsafe_line_check(int line);
extern SValue yasc_unsafe_value;           /* what the last unsafe(expr) left on the stack ... */
extern const SValue *yasc_unsafe_value_at; /* ... at this slot, NULL: none since the statement began */
Sym *yasc_vtop_var(void);
int yasc_move_borrow(void);
int yasc_unsafe_expr(void);
void yasc_unchecked_deref(void);
int yasc_type_holds_annotated(CType *t, int owned_only, int depth);
void yasc_union_member_end(int addr_operand);
int yasc_tag_switch_of(const SValue *sv);
void yasc_tag_store_note(const SValue *dst, const SValue *src);
void yasc_stmt_forget(void);
uint32_t yasc_w1c_member_of(const SValue *dst, uint32_t *fbits, int *ftok, int *nf);
void yasc_unsafe_block(void);

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
void yasc_null_spelling(SValue *p);
void yasc_null_pair(SValue *a, SValue *b);
int is_transparent_union_type(CType *type);
int pointed_size(CType *type);
void type_incompatibility_error(CType *st, CType *dt, const char *fmt);
void type_to_str(char *buf, int buf_size, CType *type, const char *varstr);

/* --- type/size.c --- */
int compute_aapcs_natural_alignment(const CType *type);
CType *pointed_type(CType *type);

/* --- value/load.c --- */
int adjust_bf(SValue *sv, int bit_pos, int bit_size);
/* -b (op/bcheck.c, CONFIG_TCC_BCHECK) */
void gbound(void);
void gen_bounded_ptr_add(void);
void bcheck_local_decl(Sym *sym);
void bcheck_vla(Sym *sym, CType *type, int slot);
void bcheck_alloca(int slot, SValue *size);
void bcheck_addr_taken(Sym *sym);
void bcheck_cleanup(Sym *cls);
void bcheck_redirect_call(void);
int get_temp_local_var(int size, int align, int *vr_out);
extern unsigned pending_call_temp_slots;
void pin_call_arg_temp_slot(const SValue *sv);
void gv_dup(void);
void incr_bf_adr(int o);
void incr_offset(int offset);
void move_reg(int r, int s, int t);
int sso_scalar_size(CType *type);
void sso_store_reversed(void);
void store_packed_bf(int bit_pos, int bit_size);

/* --- value/longlong.c --- */
void gen_opl(int op);
void lbuild(int t);

/* --- value/rodata_rel.c --- */
int rodata_rel_qualifier(void);
void rodata_rel_store_error(void);
void rodata_rel_check_targets(CType *dt, CType *st, CType *src, CType *dst);
void rodata_rel_rvalue(void);
void rodata_rel_init(Section *sec, unsigned long c);

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

/* -Wvla / -Walloca: the stack a body takes at run time.  Reported with the
 * line ("%i:") so that the check-only parse of an inlined static reports it,
 * and never from an inline expansion, which replays a body reported once
 * already; quiet in a system header, as gcc is. */
static inline int diag_stack_site(void)
{
  BufferedFile *f = file;
  if (tcc_state->in_inline_expansion)
    return 0;
  while (f && f->filename[0] == ':' && f->prev)
    f = f->prev;
  return !f || !f->sys;
}

#endif /* TCC_GEN_PRIV_H */
