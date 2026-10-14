/*
 *  TCC Backend - gen_function() Code Generator
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

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
#include "tcc_scope.h"

#include "regalloc.h"
#include "../../opt/function_pipeline.h"

#include <string.h>

/* Forward declarations for helpers that live in tccgen.c but are called from
 * gen_function.  These are the only cross-TU references; the rest of the
 * symbols gen_function touches (globals, helpers) are in tcc.h / tccir.h. */
extern void block(int is_entry);
extern void tcc_bench_log_phase(TCCState *s1, const char *operation, const char *name,
                                unsigned *total_time, unsigned *count, unsigned elapsed);
extern void check_vstack(void);
extern void nested_names_reset(void); /* nested/nested.c */

/* Cross-TU globals referenced by gen_function.  Defined in tccgen.c.
 * Note: cur_scope/root_scope are declared in tcc_scope.h. */
extern Sym  *all_cleanups;
extern int   nocode_wanted;
extern int   ind;
extern const char *funcname;
extern int func_ind;
extern CType func_vt;
extern int func_var;
extern int func_has_label_addr;
extern int local_scope;
extern int loc;
extern int nb_temp_local_vars;
extern int rsym;
extern void vpush_type_size(CType *type, int *a);

#ifndef DATA_ONLY_WANTED
#define DATA_ONLY_WANTED 2
#endif

/* Helpers defined later in this TU — forward-declared so gen_function can
 * call them. */
static void gen_instrument_call(Sym *cur_func_sym, const char *hook_name);
static void func_vla_arg_code(Sym *arg);
static void func_vla_arg(Sym *sym);

/* A `V` operand (VAR vreg, is_local + is_lval) names a register-resident scalar;
 * every other lval — a stack slot, a symbol, a pointer deref — is real memory,
 * and `is_local && !is_lval` is the address-of form.  Mirrors the split
 * lcs_span_has_memory applies, so this predicts what it will accept. */
static int ir_operand_is_memory(IROperand op)
{
  if (op.is_sym || op.is_llocal)
    return op.is_lval || op.is_local;
  int32_t vr = irop_get_vreg(op);
  int is_var = (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR);
  if (op.is_lval)
    return !(op.is_local && is_var);
  return op.is_local;
}

/* Whether the optimized body works purely on registers/scalars, i.e. whether a
 * loop inside it is a candidate for constant-argument simulation. */
static int ir_body_is_register_only(TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC ||
        q->op == TCCIR_OP_BLOCK_COPY)
      return 0;
    if (irop_config[q->op].has_dest && ir_operand_is_memory(tcc_ir_op_get_dest(ir, q)))
      return 0;
    if (irop_config[q->op].has_src1 && ir_operand_is_memory(tcc_ir_op_get_src1(ir, q)))
      return 0;
    if (irop_config[q->op].has_src2 && ir_operand_is_memory(tcc_ir_op_get_src2(ir, q)))
      return 0;
  }
  return 1;
}

/* Count live operations; see docs/inline_control_flow.md for the cycle check. */
#define SMALL_LEAF_INLINE_OPS 24
#define SRET_LOOP_INLINE_OPS 32
#define SRET_LOOP_INLINE_TOKENS 2048

/* A static leaf whose only references are a few direct calls inlines at each
 * of them while (sites - 1) * its ops stays within this: the out-of-line copy
 * goes with the last call, so that is what the TU grows by.  96 takes the
 * kernel's bit_set setRangeValue (74 ops, 2 sites, `value` constant at both).
 * A TU may expand it twice per site: a caller is generated standalone and
 * expanded again where it is itself inlined. */
#define FEW_CALLERS_GROWTH_OPS 96
#define FEW_CALLERS_INLINE_TOKENS 512

/* Static acyclic wrappers use a local caller budget; see docs/inline_readonly_wrappers.md. */
#define WRAPPER_INLINE_OPS 12

/* The inline-atomic barrier is a FUNCCALLVOID to __tcc_dmb that the backend
 * emits as one DMB (tcc_gen_machine_func_call_mop): not a call for inlining.
 * Counting it made every acquire load a non-leaf (zig.c Entry.acquire, 1.7M
 * calls, kept its itemPtr/getItem callers out of line too).  The same holds
 * for an -minline-atomics read-modify-write, a call to __tcc_ax_* the backend
 * emits as an LDREX/STREX loop (thumb_inline_atomic_call): the kernel's
 * SpinLock.lock is that loop and nothing else; and for an asm statement of
 * system instructions, __tcc_mc_* (asm_machine_call_name): its interrupt
 * masking and event hints. */
static int ir_call_is_barrier(TCCIRState *ir, IRQuadCompact *q)
{
  if ((q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL) ||
      tcc_ir_opt_pass_disabled("inline:barrier_leaf"))
    return 0;
  Sym *cs = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!cs)
    return 0;
  const char *nm = get_tok_str(cs->v, NULL);
  return (q->op == TCCIR_OP_FUNCCALLVOID && !strcmp(nm, "__tcc_dmb")) || tcc_is_inline_machine_call(nm);
}

/* A wrapper has one to maxcalls real calls, with no cycle, asm, indirect jump, or VLA. */
static int ir_body_is_acyclic_wrapper(TCCIRState *ir, int maxcalls)
{
  int calls = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_INLINE_ASM || q->op == TCCIR_OP_BUILTIN_APPLY || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_VLA_ALLOC)
      return 0;
    if ((q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID) && !ir_call_is_barrier(ir, q) &&
        ++calls > maxcalls)
      return 0;
  }
  return calls > 0 && !tcc_ir_cfg_has_cycle(ir);
}

static int ir_body_is_leaf(TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (ir_call_is_barrier(ir, q))
      continue;
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_INLINE_ASM ||
        q->op == TCCIR_OP_BUILTIN_APPLY || q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_VLA_ALLOC)
      return 0;
  }
  return 1;
}

static int ir_body_is_acyclic_leaf(TCCIRState *ir)
{
  return ir_body_is_leaf(ir) && !tcc_ir_cfg_has_cycle(ir);
}

/* A register value: a vreg read as is, or a VAR read through its spill encoding. */
static int ir_operand_is_reg_value(IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_sym || op.is_llocal)
    return 0;
  if (irop_get_tag(op) == IROP_TAG_VREG)
    return !op.is_lval;
  return irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_lval && op.is_local &&
         TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR;
}

/* The vreg an lvalue operand reads or writes through, or -1. */
static int32_t ir_operand_deref_vreg(IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_sym || op.is_llocal || !op.is_lval || irop_get_tag(op) != IROP_TAG_VREG)
    return -1;
  return vr;
}

/* An incoming argument read from where the caller put it: a parameter's own
 * storage (stack-passed, or the home of an address-taken one). */
static int ir_operand_is_arg_read(IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  return irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_lval && op.is_local && !op.is_llocal &&
         (vr < 0 || op.vreg_type == 0 ? op.is_param : TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_PARAM);
}

/* The address of an incoming parameter's storage (`&P1`). */
static int ir_operand_is_arg_addr(IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  return irop_get_tag(op) == IROP_TAG_STACKOFF && !op.is_lval && op.is_local && !op.is_llocal &&
         (vr < 0 || op.vreg_type == 0 ? op.is_param : TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_PARAM);
}

static int ir_operand_is_frame_slot(IROperand op, int lval)
{
  return irop_get_tag(op) == IROP_TAG_STACKOFF && op.vreg_type == 0 && op.is_local && op.is_lval == lval &&
         !op.is_llocal;
}

/* Zig's `undefined`: the payload of a null optional or an error union, never read. */
static int ir_operand_is_undefined_fill(TCCIRState *ir, IROperand op)
{
  return irop_is_immediate(op) && !op.is_sym && op.btype == IROP_BTYPE_INT32 &&
         (uint32_t)irop_get_imm64_ex(ir, op) == 0xaaaaaaaau;
}

#define IR_MARSHAL_SRET 1 /* the hidden result pointer, or an offset from it */
#define IR_MARSHAL_ARG 2  /* the address of an incoming parameter's storage */

static int ir_marshal_index(TCCIRState *ir, int32_t vr, int n)
{
  int p = TCCIR_DECODE_VREG_POSITION(vr), b = -1;
  switch (TCCIR_DECODE_VREG_TYPE(vr))
  {
  case TCCIR_VREG_TYPE_TEMP:
    b = p;
    break;
  case TCCIR_VREG_TYPE_VAR:
    b = ir->next_temporary_variable + p;
    break;
  case TCCIR_VREG_TYPE_PARAM:
    b = ir->next_temporary_variable + ir->next_local_variable + p;
    break;
  }
  return b >= 0 && b < n ? b : -1;
}

/* The marshalling class a defining op gives its destination: a copy or a
 * constant offset of a marshalling pointer, the address of a parameter, or
 * the sret pointer reloaded from its home slot. */
static int ir_marshal_def(TCCIRState *ir, IRQuadCompact *q, const uint8_t *m, int n, int sret)
{
  if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ADD)
    return 0;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  int32_t vr = irop_get_vreg(s);
  if (q->op == TCCIR_OP_ADD)
  {
    IROperand k = tcc_ir_op_get_src2(ir, q);
    int b = ir_operand_is_reg_value(s) ? ir_marshal_index(ir, vr, n) : -1;
    return b >= 0 && irop_is_immediate(k) && !k.is_sym ? m[b] : 0;
  }
  if (ir_operand_is_arg_addr(s))
    return IR_MARSHAL_ARG;
  if (sret && q->op == TCCIR_OP_LOAD && ir_operand_is_frame_slot(s, 1) && irop_get_stack_offset(s) == func_vc)
    return IR_MARSHAL_SRET;
  int b = ir_operand_is_reg_value(s) ? ir_marshal_index(ir, vr, n) : -1;
  return b >= 0 ? m[b] : 0;
}

/* Live prologue operations copying a struct parameter split between r0-r3
 * and the stack into its home (params.c): stores of its register words, and
 * loads of its stack words stored into the home.  An expanded call binds the
 * argument instead, so the inline size classes leave them out -- as they did
 * when such a parameter was used in place, without them.  *stack_stores gets
 * the stores of loaded stack words alone. */
static int ir_split_param_copy_ops(TCCIRState *ir, int *stack_stores)
{
  if (stack_stores)
    *stack_stores = 0;
  if (!ir || !ir->param_forms || ir->parameters_count <= 0)
    return 0;
  int32_t loaded[64];
  int nloaded = 0, n = 0;
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != (pass ? TCCIR_OP_STORE : TCCIR_OP_LOAD))
        continue;
      IROperand a = pass ? tcc_ir_op_get_dest(ir, q) : tcc_ir_op_get_src1(ir, q);
      if (irop_get_tag(a) != IROP_TAG_STACKOFF || irop_get_vreg(a) >= 0 || !a.is_lval || a.is_param != !pass)
        continue;
      const int32_t off = irop_get_stack_offset(a);
      IROperand v = pass ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_dest(ir, q);
      const int32_t vr = irop_get_tag(v) == IROP_TAG_VREG && !v.is_lval ? irop_get_vreg(v) : -1;
      for (int k = 0; k < ir->parameters_count; k++)
      {
        const IRParamForm *f = &ir->param_forms[k];
        if (f->kind != IR_PF_HOME || f->stack_words <= 0)
          continue;
        if (!pass)
        {
          if (off >= f->stack_off && off < f->stack_off + f->stack_words * 4 && vr >= 0 && nloaded < 64)
          {
            loaded[nloaded++] = vr;
            n++;
          }
          continue;
        }
        if (off < f->off || off >= f->off + (f->words + f->stack_words) * 4 || vr < 0)
          continue;
        int from_copy = TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_PARAM;
        for (int j = 0; j < nloaded && !from_copy; j++)
          from_copy = loaded[j] == vr;
        n += from_copy;
        if (stack_stores && from_copy && TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_PARAM)
          ++*stack_stores;
        break;
      }
    }
  return n;
}

/* An optimized body's size the way it survives expansion at a call site.  A
 * body written over by-value aggregates (Zig's `fn (self: Self) ?T`) is mostly
 * marshalling the expansion removes: register copies, reads of the incoming
 * aggregate through its address, stores through the hidden result pointer
 * (its undefined 0xaa fills too, also when they reach it as register
 * values), and the address arithmetic for both.  Those cost the caller as
 * much out of line, so only the remaining ops are counted.
 * -Os keeps the raw count: its classes are tuned to it. */
static int ir_body_inline_ops(TCCIRState *ir)
{
  const int nops = ir->next_instruction_index;
  /* A split struct parameter's home copy (ir_split_param_copy_ops): left out
   * of the raw count whole; the marshalling count below already drops its
   * register-word stores and stack-word loads, so only the stores of the
   * loaded stack words are left to discount there. */
  int pstack;
  const int pcopy = ir_split_param_copy_ops(ir, &pstack);
  int raw = -pcopy;
  for (int i = 0; i < nops; i++)
    raw += ir->compact_instructions[i].op != TCCIR_OP_NOP;
  if (TCC_OPT(tcc_state, optimize_size) || tcc_ir_opt_pass_disabled("inline:marshal_cost"))
    return raw;

  const int sret = func_vc != 0 && !ir->vfp_ret_words;
  const int n = ir->next_temporary_variable + ir->next_local_variable + ir->next_parameter + 1;
  uint8_t *m = tcc_mallocz(n);
  if (sret)
  {
    int b = ir_marshal_index(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0), n);
    if (b >= 0)
      m[b] = IR_MARSHAL_SRET;
  }
  for (int changed = 1; changed;)
  {
    changed = 0;
    for (int i = 0; i < nops; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
        continue;
      int cls = ir_marshal_def(ir, q, m, n, sret);
      int b = cls ? ir_marshal_index(ir, tcc_ir_op_dest_vreg(ir, q), n) : -1;
      if (b >= 0 && (m[b] | cls) != m[b])
      {
        m[b] |= cls;
        changed = 1;
      }
    }
  }

  int ops = 0;
  for (int i = 0; i < nops; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int free_op = 0;
    switch (q->op)
    {
    case TCCIR_OP_NOP:
      free_op = 1;
      break;
    case TCCIR_OP_ASSIGN:
    case TCCIR_OP_LOAD:
    case TCCIR_OP_LEA:
    case TCCIR_OP_ADD:
    case TCCIR_OP_LOAD_INDEXED:
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t through = q->op == TCCIR_OP_LOAD ? ir_operand_deref_vreg(s)
                        : q->op == TCCIR_OP_LOAD_INDEXED && ir_operand_is_reg_value(s) ? irop_get_vreg(s)
                                                                                        : -1;
      int b = through >= 0 ? ir_marshal_index(ir, through, n) : -1;
      free_op = ir_marshal_def(ir, q, m, n, sret) != 0 ||
                ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && ir_operand_is_reg_value(s)) ||
                (q->op == TCCIR_OP_ASSIGN && ir_operand_is_undefined_fill(ir, s)) ||
                (q->op == TCCIR_OP_LOAD && ir_operand_is_arg_read(s)) || (b >= 0 && (m[b] & IR_MARSHAL_ARG));
      break;
    }
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t through = q->op == TCCIR_OP_STORE ? ir_operand_deref_vreg(d)
                        : ir_operand_is_reg_value(d) ? irop_get_vreg(d)
                                                     : -1;
      int b = through >= 0 ? ir_marshal_index(ir, through, n) : -1;
      int32_t svr = irop_get_vreg(s);
      free_op = (b >= 0 && (m[b] & IR_MARSHAL_SRET)) ||
                (q->op == TCCIR_OP_STORE && ir_operand_is_frame_slot(d, 1) &&
                 ((sret && irop_get_stack_offset(d) == func_vc) ||
                  (ir_operand_is_reg_value(s) && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_PARAM)));
      break;
    }
    default:
      break;
    }
    ops += !free_op;
  }
  tcc_free(m);
  return ops - pstack;
}

int inline_body_has_unsafe_loops(TokenString *func_str); /* inline/analysis.c */
InlineFunc *inline_fn_lookup(TCCState *s, Sym *sym); /* inline/emit.c */
TokenString *inline_body_guard_head(TokenString *func_str, Sym *sym, int **free_names); /* inline/analysis.c */
void guard_head_register(TCCState *s, Sym *sym, TokenString *head, const int *pack, int *free_names); /* inline/emit.c */

/* A copy between two vregs the allocator gave one register emits nothing. */
static int ir_copy_is_free(TCCIRState *ir, IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_ASSIGN)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q), s = tcc_ir_op_get_src1(ir, q);
  int32_t dv = irop_get_vreg(d), sv = irop_get_vreg(s);
  if (dv < 0 || sv < 0 || irop_get_tag(d) != IROP_TAG_VREG || irop_get_tag(s) != IROP_TAG_VREG || d.is_lval ||
      s.is_lval || d.is_llocal || s.is_llocal || d.is_local || s.is_local ||
      irop_get_btype(d) != irop_get_btype(s) || d.is_unsigned != s.is_unsigned)
    return 0;
  IRLiveInterval *di = tcc_ir_get_live_interval(ir, dv), *si = tcc_ir_get_live_interval(ir, sv);
  if (!di || !si || (di->allocation.r0 & PREG_SPILLED) || (si->allocation.r0 & PREG_SPILLED) ||
      (di->allocation.r0 & PREG_REG_NONE) == PREG_REG_NONE)
    return 0;
  return di->allocation.r0 == si->allocation.r0 && (!irop_needs_pair(d) || di->allocation.r1 == si->allocation.r1);
}

/* Does TEMP vr hold 0 or 1 wherever it is read: every definition a copy of
 * either constant or of another such TEMP?  (A phi of `return true;` /
 * `return false;` edges, after phi resolution.) */
static int ir_temp_is_bool_const(TCCIRState *ir, int32_t vr, int depth)
{
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || depth > 4)
    return 0;
  int defs = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || tcc_ir_op_dest_vreg(ir, q) != vr)
      continue;
    if (q->op != TCCIR_OP_ASSIGN || tcc_ir_op_dest_is_lval(ir, q))
      return 0;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (irop_is_plain_imm(s))
    {
      if (irop_get_imm64_ex(ir, s) != 0 && irop_get_imm64_ex(ir, s) != 1)
        return 0;
    }
    else if (s.is_lval || irop_get_tag(s) != IROP_TAG_VREG || irop_get_vreg(s) == vr ||
             !ir_temp_is_bool_const(ir, irop_get_vreg(s), depth + 1))
      return 0;
    defs++;
  }
  return defs > 0;
}

/* Zig C represents bool as an unsigned byte; require constant boolean
 * returns.  Counted are the ops that emit code: a predicate whose loop exits
 * through `return true;` / `return false;` keeps the copies phi resolution
 * left for them, which the allocator coalesced away. */
static int ir_body_is_small_predicate(TCCIRState *ir, Sym *sym)
{
  int type = sym->type.ref->type.t;
  int bt = type & VT_BTYPE;
  if (TCC_OPT(tcc_state, optimize) < 2 || TCC_OPT(tcc_state, optimize_size) ||
      tcc_ir_opt_pass_disabled("inline:small_predicate") || !(sym->type.t & VT_STATIC) ||
      (bt != VT_BOOL && (bt != VT_BYTE || !(type & VT_UNSIGNED))) ||
      tcc_ir_lookup_func_purity(tcc_state, func_cache_token(sym)) < TCC_FUNC_PURITY_PURE)
    return 0;
  int returns = 0, ops = 0;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    ops += q->op != TCCIR_OP_NOP && !ir_copy_is_free(ir, q);
    if (q->op != TCCIR_OP_RETURNVALUE)
      continue;
    IROperand value = tcc_ir_op_get_src1(ir, q);
    if (irop_is_plain_imm(value))
    {
      if (irop_get_imm64_ex(ir, value) != 0 && irop_get_imm64_ex(ir, value) != 1)
        return 0;
    }
    else if (value.is_lval || irop_get_tag(value) != IROP_TAG_VREG ||
             !ir_temp_is_bool_const(ir, irop_get_vreg(value), 0))
      return 0;
    returns++;
  }
  return returns > 0 && ops - ir_split_param_copy_ops(ir, NULL) <= 32;
}

static int ir_body_backedges(TCCIRState *ir)
{
  int n = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int target = (int)tcc_ir_op_dest_imm(ir, q);
      n += target >= 0 && target <= i;
    }
  }
  return n;
}

/* A wrapper measured after its one small-leaf callee expanded into it
 * (Zig's ComponentIterator.next over peekNext): before the expansion it was
 * a wrapper, expanded with the leaf nested in it, so the composite keeps that
 * class when its own ops (all but the leaf's cost) are wrapper-sized and it
 * adds no loop, and the leaf's per-caller bound (ir_body_is_small_loop_leaf).
 * The SMALL_LEAF + WRAPPER bound stops wrappers of wrappers. */
static int ir_body_wraps_small_leaf(TCCIRState *ir, int inline_ops)
{
  if (TCC_OPT(tcc_state, optimize_size) || tcc_ir_opt_pass_disabled("inline:wrappers") ||
      tcc_ir_opt_pass_disabled("inline:leaf_wrapper") || tcc_state->inline_leaf_sites != 1 ||
      inline_ops > SMALL_LEAF_INLINE_OPS + WRAPPER_INLINE_OPS ||
      inline_ops - tcc_state->inline_leaf_ops > WRAPPER_INLINE_OPS)
    return 0;
  return ir_body_backedges(ir) <= tcc_state->inline_leaf_backedges;
}

/* Token replay must still reject loops in expression context. */
static int ir_body_is_small_loop_leaf(TCCIRState *ir, Sym *sym, int real_ops)
{
  int wrapper = 0;
  if (TCC_OPT(tcc_state, optimize_size) || tcc_ir_opt_pass_disabled("inline:small_loop") ||
      !(sym->type.t & VT_STATIC) ||
      (real_ops > SMALL_LEAF_INLINE_OPS && !ir_body_is_small_predicate(ir, sym) &&
       !(wrapper = ir_body_wraps_small_leaf(ir, real_ops))) ||
      !ir_body_is_leaf(ir) || !tcc_ir_cfg_has_cycle(ir))
    return 0;
  for (int fi = 0; fi < tcc_state->nb_inline_fns; fi++)
    if (tcc_state->inline_fns[fi]->sym == sym)
    {
      if (!tcc_state->inline_fns[fi]->func_str || inline_body_has_unsafe_loops(tcc_state->inline_fns[fi]->func_str))
        return 0;
      if (wrapper && tcc_state->inline_leaf_bounded)
        sym->type.ref->f.func_inl_sret_loop = 1;
      return 1;
    }
  return 0;
}

TCC_DBG_ENV_STR(dbg_dump_func, "TCC_DUMP_FUNC")

/* An old-style definition receives each argument as its caller passed it:
 * promoted (int for a narrow integer or _Bool, double for a float), or as
 * the prototype in scope says.  Its body converts each one to the declared
 * parameter type on entry (C11 6.9.1p10).  The parameter the ABI delivers
 * binds under an anonymous name in its arrival type, and the declared name
 * binds a local initialised from it. */
typedef struct KnrConv
{
  Sym *param; /* in the definition's parameter list */
  int v;      /* its name, restored after tcc_ir_params_add */
  CType type; /* its declared type */
  int anon;   /* the arrival's name */
} KnrConv;

/* The type an old-style parameter of type DECL arrives in (PROTO_PARAM: the
 * prototype's, if one is in scope), into ARRIVAL; 0 when it is DECL. */
static int knr_arrival_type(const CType *decl, const Sym *proto_param, CType *arrival)
{
  const int arith = VT_BTYPE | VT_UNSIGNED;
  int bt = decl->t & VT_BTYPE;
  if (decl->t & (VT_ARRAY | VT_VLA | VT_VECTOR | VT_COMPLEX))
    return 0;
  if (proto_param)
  {
    int pbt = proto_param->type.t & VT_BTYPE;
    if (!(bt == VT_BOOL || bt == VT_BYTE || bt == VT_SHORT || bt == VT_INT || bt == VT_LLONG || is_float(bt)) ||
        !(pbt == VT_BOOL || pbt == VT_BYTE || pbt == VT_SHORT || pbt == VT_INT || pbt == VT_LLONG || is_float(pbt)) ||
        (proto_param->type.t & (VT_ARRAY | VT_VLA | VT_VECTOR | VT_COMPLEX)) ||
        (proto_param->type.t & arith) == (decl->t & arith))
      return 0;
    arrival->t = proto_param->type.t & arith;
    arrival->ref = NULL;
    return 1;
  }
  if (bt == VT_BOOL || bt == VT_BYTE || bt == VT_SHORT)
    arrival->t = VT_INT;
  else if (bt == VT_FLOAT)
    arrival->t = VT_DOUBLE;
  else
    return 0;
  arrival->ref = NULL;
  return 1;
}

/* Before tcc_ir_params_add: give each parameter of SYM's old-style definition
 * that arrives converted its arrival type and an anonymous name.  Returns
 * them (tcc_malloc'd, freed by knr_params_receive), their count in *N. */
static KnrConv *knr_params_arrive(Sym *sym, int *pn)
{
  Sym *ref = sym->type.ref;
  KnrConv *conv = NULL;
  int n = 0, nb_params = 0;
  *pn = 0;
  if (ref->f.func_type != FUNC_OLD || !ref->next)
    return NULL;
  const SymLocalFacts *facts = sym_facts_peek(sym);
  Sym *pp = facts && facts->knr_proto ? facts->knr_proto->next : NULL;
  for (Sym *p = ref->next; p; p = p->next)
    nb_params++;
  for (Sym *p = ref->next; p; p = p->next, pp = pp ? pp->next : NULL)
  {
    CType arrival;
    if (!knr_arrival_type(&p->type, pp, &arrival))
      continue;
    if (!conv)
      conv = tcc_malloc(sizeof *conv * nb_params);
    conv[n] = (KnrConv){.param = p, .v = p->v, .type = p->type, .anon = anon_sym++};
    p->type = arrival;
    p->v = conv[n].anon | SYM_FIELD;
    n++;
  }
  *pn = n;
  return conv;
}

/* After tcc_ir_params_add: restore the definition's parameters and bind each
 * declared name to its converted value. */
static void knr_params_receive(KnrConv *conv, int n)
{
  for (int i = 0; i < n; i++)
  {
    Sym *p = conv[i].param;
    Sym *arrived = sym_find2(local_stack, conv[i].anon);
    p->type = conv[i].type;
    p->v = conv[i].v;
    if (!arrived)
      continue;
    CType type = conv[i].type;
    int align, size = type_size(&type, &align);
    if (p->a.param_volatile)
      type.t |= VT_VOLATILE;
    loc = tcc_ir_frame_alloc(loc, size, -align);
    tcc_ir_frame_note_type(loc, &type);
    Sym *local = sym_push(conv[i].v & ~SYM_FIELD, &type, VT_LOCAL | VT_LVAL, loc);
    vset(&type, VT_LOCAL | VT_LVAL, loc);
    vtop->sym = local;
    vtop->vr = local->vreg;
    vset(&arrived->type, arrived->r, arrived->c);
    vtop->sym = arrived;
    vtop->vr = arrived->vreg;
    vstore();
    vpop();
  }
  tcc_free(conv);
}

void gen_function(Sym *sym)
{
  struct scope f = {0};
  TCCIRState *ir;
  Sym *global_label_stack_start; /* save global label stack at function start */
  unsigned phase_start = 0;
  cur_scope = root_scope = &f;
  nocode_wanted = 0;

  ind = cur_text_section->data_offset;
  tcc_state->force_frame_pointer = 0;
  tcc_state->need_frame_pointer = 0;
  tcc_state->func_dynamic_sp = 0;
  tcc_state->force_lr_save = 0;
  tcc_state->func_save_apply_args = 0;
  tcc_state->apply_args_offset = 0;
  tcc_state->ir_post_float_narrow = 0;

  global_label_stack_start = global_label_stack;

  if (sym->a.aligned)
  {
    size_t newoff = section_add(cur_text_section, 0, 1 << (sym->a.aligned - 1));
    gen_fill_nops(newoff - ind);
  }

  func_align_pad = 0;
  funcname = get_tok_str(sym->v, NULL);
  func_ind = ind;
#ifdef CONFIG_TCC_DEBUG
  /* TCC_DUMP_FUNC=<name>: -dump-ir / -dump-ir-passes print this function only,
   * so a whole-TU compile (whose inlining and summaries a cut-down TU does not
   * reproduce) can still be dumped for the one function that matters. */
  const int saved_dump_ir = tcc_state->dump_ir;
  char *const saved_dump_ir_passes = tcc_state->dump_ir_passes;
  if (dbg_dump_func() && strcmp(dbg_dump_func(), funcname))
  {
    tcc_state->dump_ir = 0;
    tcc_state->dump_ir_passes = NULL;
  }
#endif
  /* Where this body's relocations begin, for tcc_icf_try_fold. */
  const size_t func_reloc_mark =
      cur_text_section->reloc ? cur_text_section->reloc->data_offset : 0;
  func_vt = sym->type.ref->type;
  func_var = sym->type.ref->f.func_type == FUNC_ELLIPSIS;
  func_has_label_addr = 0;
  tcc_state->cur_func_sym = sym;
  if (!tcc_state->current_nested_func)
    nested_names_reset(); /* a file-scope function binds no nested function yet */
  /* Per-caller budget: see CONST_LOOP_INLINE_BUDGET in gen/builtin/call.c. */
  tcc_state->const_loop_inline_used = 0;
  tcc_state->nested_inline_budget = NESTED_INLINE_BUDGET;
  tcc_state->inline_leaf_sites = tcc_state->inline_leaf_ops = tcc_state->inline_leaf_backedges = 0;
  tcc_state->inline_leaf_bounded = 0;

  /* NOTE: we patch the symbol size later */
  put_extern_sym(sym, cur_text_section, ind + 1, 0);

  if (sym->type.ref->f.func_ctor)
    add_array(tcc_state, ".init_array", sym->c);
  if (sym->type.ref->f.func_dtor)
    add_array(tcc_state, ".fini_array", sym->c);

  tcc_debug_funcstart(tcc_state, sym);

  sym_push2(&local_stack, SYM_FIELD, 0, 0);
  LOG_IR_GEN("Generating IR for function %s", funcname);
  ir = tcc_ir_alloc();
  tcc_state->ir = ir;
  /* Vector-expression recipes index this function's IR; start clean. */
  gen_op_vector_reset();
  ir->naked = sym->a.naked;
  ir->is_variadic = func_var;

  /* Every nested function keeps its incoming chain (saved at frame top - 4,
   * reloaded after each call), captures or not: a call of a sibling or an
   * ancestor passes it on, and a descendant walks through it. */
  if (tcc_state->current_nested_func)
  {
    NestedFunc *nf = tcc_state->current_nested_func;
    ir->has_static_chain = 1;
    ir->captured_count = nf->nb_captured;
    for (int j = 0; j < nf->nb_captured && j < 32; j++)
    {
      ir->captured_offsets_list[j] = nf->captured_offsets[j];
      ir->captured_chain_depths[j] = nf->captured_chain_depth[j];
    }
    ir->static_chain_vreg = tcc_ir_get_vreg_static_chain(ir);
    ir->needs_chain_save = nf->needs_chain_save;
  }

  if (TCC_OPT(tcc_state, opt_fp_offset_cache))
    tcc_ir_opt_fp_cache_init(ir);

  local_scope = 1;
  int nb_knr_conv;
  KnrConv *knr_conv = knr_params_arrive(sym, &nb_knr_conv);
  tcc_ir_params_add(ir, &sym->type);

  /* Captures are addressed by offsets of their own: each gets a range at
   * the bottom of the frame offset space (TCC_CAPTURE_SPACE, where no local
   * of a frame under 32 KiB reaches, and which takes no room in the frame),
   * so an interior offset (a member, an element, one past the end) still
   * names the capture and its depth, and none of our own locals can be
   * mistaken for one.  The 8-byte gaps keep one-past-the-end addresses out of
   * the neighbouring range; vbase has the parent offset's alignment mod 8. */
  {
    int cursor = TCC_CAPTURE_SPACE;
    for (int j = 0; j < ir->captured_count && j < 32; j++)
    {
      int align;
      int size = type_size(&tcc_state->current_nested_func->captured_types[j], &align);
      if (size < 4)
        size = 4;
      cursor += (ir->captured_offsets_list[j] - cursor) & 7;
      ir->captured_vbase[j] = cursor;
      ir->captured_size[j] = size;
      cursor += size + 8;
    }
    ir->captured_space_end = cursor;
  }
  knr_params_receive(knr_conv, nb_knr_conv);
  nb_temp_local_vars = 0;

  local_scope = 0;
  rsym = -1;

  /* -finstrument-functions: emit entry hook call before function body */
  if (tcc_state->instrument_functions && !sym->type.ref->f.func_no_instrument)
  {
    gen_instrument_call(sym, "__cyg_profile_func_enter");
  }

  func_vla_arg(sym);
  if (tcc_state->do_bench)
    phase_start = tcc_getclock_us();
  block(0);
  tcc_ir_backpatch_to_here(ir, rsym);

  if (ir && tcc_state->nb_nested_funcs > 0) {
    int func_has_chain_op = 0;
    int any_trampoline_needed = 0;
    for (int i = 0; i < ir->next_instruction_index && !func_has_chain_op; i++) {
      int op = ir->compact_instructions[i].op;
      if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
        func_has_chain_op = 1;
    }
    for (int ni = 0; ni < tcc_state->nb_nested_funcs && !any_trampoline_needed; ni++) {
      if (tcc_state->nested_funcs[ni]->trampoline_needed)
        any_trampoline_needed = 1;
    }
    if (!func_has_chain_op && !any_trampoline_needed) {
      for (int ni = 0; ni < tcc_state->nb_nested_funcs; ni++) {
        NestedFunc *nf = tcc_state->nested_funcs[ni];
        for (int ci = 0; ci < nf->nb_captured; ci++) {
          int vreg = nf->captured_vregs[ci];
          if (vreg >= 0 && nested_capture_owned_by(nf, ci, tcc_state->current_nested_func)) {
            /* Owned entries only: an ancestor's vreg names its IR, and may
             * lie past this one's interval arrays as well. */
            IRLiveInterval *interval = tcc_ir_try_get_live_interval(ir, vreg);
            if (interval)
              interval->addrtaken = 0;
          }
        }
      }
    }
  }
  if (ir && tcc_state->nb_nested_funcs > 0) {
    for (int ni = 0; ni < tcc_state->nb_nested_funcs; ni++) {
      NestedFunc *nf = tcc_state->nested_funcs[ni];
      if (!nf->sym || !nf->sym->type.ref || !nf->sym->type.ref->f.func_auto_inline)
        continue;
      if (nf->trampoline_needed || nf->nb_captured == 0)
        continue;
      /* Marked auto_inline is a hope, not a fact: a call site can decline for
       * reasons of its own, and the surviving call still reads the captures
       * through the static chain.  nb_real_calls is the fact. */
      if (nf->nb_real_calls > 0)
        continue;
      int called_by_sibling = 0;
      int func_tok = nf->sym->v & ~SYM_FIELD;
      for (int si = 0; si < tcc_state->nb_nested_funcs && !called_by_sibling; si++) {
        NestedFunc *sib = tcc_state->nested_funcs[si];
        if (sib == nf || !sib->func_str)
          continue;
        const int *tp = tok_str_buf(sib->func_str);
        while (*tp) {
          int tv;
          CValue tcv;
          tok_get(&tv, &tp, &tcv);
          if (tv == TOK_EOF || tv == 0) break;
          if (tv == func_tok) { called_by_sibling = 1; break; }
        }
      }
      if (!called_by_sibling) {
        for (int ci = 0; ci < nf->nb_captured; ci++) {
          int vreg = nf->captured_vregs[ci];
          if (vreg >= 0 && nested_capture_owned_by(nf, ci, tcc_state->current_nested_func)) {
            int keep_addrtaken = 0;
            for (int oi = 0; oi < tcc_state->nb_nested_funcs && !keep_addrtaken; oi++) {
              NestedFunc *other = tcc_state->nested_funcs[oi];
              if (other == nf || other->nb_captured == 0)
                continue;

              int captures_vreg = 0;
              for (int oc = 0; oc < other->nb_captured; oc++) {
                if (other->captured_vregs[oc] == vreg &&
                    nested_capture_owned_by(other, oc, tcc_state->current_nested_func)) {
                  captures_vreg = 1;
                  break;
                }
              }
              if (!captures_vreg)
                continue;

              if (!other->sym || !other->sym->type.ref || other->trampoline_needed ||
                  !other->sym->type.ref->f.func_auto_inline) {
                keep_addrtaken = 1;
                break;
              }

              {
                int other_called_by_sibling = 0;
                int other_func_tok = other->sym->v & ~SYM_FIELD;
                for (int si = 0; si < tcc_state->nb_nested_funcs && !other_called_by_sibling; si++) {
                  NestedFunc *sib = tcc_state->nested_funcs[si];
                  if (sib == other || !sib->func_str)
                    continue;
                  const int *tp = tok_str_buf(sib->func_str);
                  while (*tp) {
                    int tv;
                    CValue tcv;
                    tok_get(&tv, &tp, &tcv);
                    if (tv == TOK_EOF || tv == 0)
                      break;
                    if (tv == other_func_tok) {
                      other_called_by_sibling = 1;
                      break;
                    }
                  }
                }
                if (other_called_by_sibling)
                  keep_addrtaken = 1;
              }
            }

            if (!keep_addrtaken) {
              IRLiveInterval *interval = tcc_ir_try_get_live_interval(ir, vreg); /* may be the parent's */
              if (interval)
                interval->addrtaken = 0;
            }
          }
        }
      }
    }
  }

  /* -finstrument-functions: emit exit hook call at the common return point */
  if (tcc_state->instrument_functions && !sym->type.ref->f.func_no_instrument)
  {
    gen_instrument_call(sym, "__cyg_profile_func_exit");
  }

  /* A result returned in s0-s7 is loaded from the buffer the returns wrote,
   * at the common exit after every cleanup: the last thing before the
   * epilogue, and never a tail call. */
  if (ir->vfp_ret_words && !ir->naked)
  {
    SValue buf;
    svalue_init(&buf);
    buf.type.t = VT_PTR;
    buf.r = VT_LOCAL;
    buf.vr = -1;
    buf.c.i = ir->vfp_ret_buf;
    gen_vfp_ret_transfer(&buf, ir->vfp_ret_words, 1);
  }

  if (tcc_state->do_bench)
  {
    unsigned now = tcc_getclock_us();
    tcc_bench_log_phase(tcc_state, "func-body", funcname, &tcc_state->bench_function_body_time,
                        &tcc_state->bench_function_body_count, now - phase_start);
    phase_start = now;
  }

  /* Target-independent optimization (source/opt/function_pipeline.c), then the
   * backend register-allocation + codegen-prep pipeline (regalloc.c).  The
   * two phases are synchronized here so the opt unit stays free of any
   * codegen/backend logic. */
  int nonstatic_global_copier = 0;
  /* Before anything else reads the IR: struct-return buffers a callee could
   * reach some other way become fresh temporaries (sret_nrvo.c). */
  if (tcc_ir_sret_dealias(ir))
    tcc_ir_dump_after_pass(ir, "sret_dealias");
  tcc_ir_opt_run_function_pipeline(ir, sym, func_var, &nonstatic_global_copier);

  tcc_ir_backend_fold_pure_forward(ir, sym);
  tcc_ir_backend_analyze_leaf_and_tail_calls(ir, func_var);

  if (tcc_state->do_bench)
  {
    unsigned now = tcc_getclock_us();
    tcc_bench_log_phase(tcc_state, "func-opt", funcname, &tcc_state->bench_function_opt_time,
                        &tcc_state->bench_function_opt_count, now - phase_start);
    phase_start = now;
  }

  tcc_ir_backend_regalloc_pipeline(ir, sym, func_var, &phase_start, funcname,
                                   global_label_stack_start);

  /* Stores writing back what was just loaded from the same frame bytes:
   * copies between objects frame relayout placed on the same slot. */
  if (TCC_OPT(tcc_state, optimize) > 0 && !tcc_ir_opt_pass_disabled("self_store") && tcc_ir_self_store(ir))
    tcc_ir_dump_after_pass(ir, "self_store");

  /* Repeated single-entry regions (switch case bodies) share one copy. */
  if (TCC_OPT(tcc_state, optimize) > 0 && !tcc_ir_opt_pass_disabled("region_merge") && tcc_ir_region_merge(ir))
    tcc_ir_dump_after_pass(ir, "region_merge");

  /* Identical block tails jumping to the same place share one copy. */
  if (TCC_OPT(tcc_state, optimize) > 0 && !tcc_ir_opt_pass_disabled("cross_jump") && tcc_ir_cross_jump(ir))
    tcc_ir_dump_after_pass(ir, "cross_jump");

  /* The merges leave branches to jumps, and jumps nothing reaches. */
  if (TCC_OPT(tcc_state, optimize) > 0 && !tcc_ir_opt_pass_disabled("branch_tidy") && tcc_ir_branch_tidy(ir))
    tcc_ir_dump_after_pass(ir, "branch_tidy");

  tcc_ir_codegen_generate(ir);

  /* The orig_index side tables (barrel_shifts, zero_half64, shift64_dead_half,
   * bfi_params) stay until tcc_ir_free: they are part of what the final IR
   * means, and the IPC snapshot below (tcc_ir_detect_switch_func) evaluates
   * that IR -- without barrel_shifts a fused `cmp i, n, lsr #4` reads as
   * `cmp i, n`. */

  if (!sym->a.naked)
  {
    tcc_debug_prolog_epilog(tcc_state, 1);
    // gfunc_epilog();
  }

  if (tcc_state->do_bench)
  {
    unsigned now = tcc_getclock_us();
    tcc_bench_log_phase(tcc_state, "func-codegen", funcname, &tcc_state->bench_function_codegen_time,
                        &tcc_state->bench_function_codegen_count, now - phase_start);
  }

#ifdef CONFIG_TCC_DEBUG
  if (tcc_state->dump_ir)
  {
    tcc_ir_dump_set_show_physical_regs(1); /* Show physical registers with virtual register info */
    printf("=== IR AFTER OPTIMIZATIONS ===\n");
    tcc_ir_show(ir);
    printf("=== END IR AFTER OPTIMIZATIONS ===\n");
  }
#endif

  /* Infer and cache function purity for LICM optimization
   * This allows LICM to hoist calls to pure functions defined in the same TU */
  /* Nothing learned from a weak definition's body may leave this function:
   * the link can replace it (a kernel's strong _sbrk over libc's ENOSYS
   * default), so callers must call through the symbol and assume nothing --
   * not its purity, not a constant result, not a trivial body to inline. */
  const int body_is_final = sym && !sym->a.weak;
  const int wrappers_on = !TCC_OPT(tcc_state, optimize_size) && !tcc_ir_opt_pass_disabled("inline:wrappers");
  /* Inline size classes count the body without a split struct's home copy. */
  const int pcopy = ir_split_param_copy_ops(ir, NULL);
  const int slots = ir ? ir->next_instruction_index - pcopy : 0;

  if (TCC_OPT(tcc_state, opt_licm) && ir && body_is_final)
  {
    /* Forward declare the inference function */
    extern TCCFuncPurity tcc_ir_infer_func_purity(TCCIRState * ir, Sym * func_sym);
    extern void tcc_ir_cache_func_purity(TCCState * s, int func_token, TCCFuncPurity purity);

    TCCFuncPurity purity = tcc_ir_infer_func_purity(ir, sym);
    tcc_ir_cache_func_purity(tcc_state, func_cache_token(sym), purity);
  }

  if (TCC_OPT(tcc_state, opt_ipc) && ir && body_is_final)
  {
    int64_t const_val;
    int const_btype;
    int const_cached = 0;
    if (tcc_ir_detect_const_result(ir, &const_val, &const_btype))
    {
      tcc_ir_cache_const_result(tcc_state, func_cache_token(sym), const_val, const_btype);
      const_cached = 1;
    }
    /* If the function isn't a plain const-returning function but is a pure
     * single-parameter dispatcher (switch / if-chain over the arg returning
     * constants), snapshot it so callers passing a constant can fold the call. */
    if (!const_cached)
    {
      TCCFuncSwitchSnapshot *snap = NULL;
      if (tcc_ir_detect_switch_func(ir, &snap))
        tcc_ir_cache_switch_func(tcc_state, func_cache_token(sym), snap);
    }
  }

  /* Read-only wrappers share the small-leaf limit; caller caps still apply. */
  const int wrapper_inline_ops =
      wrappers_on && !tcc_ir_opt_pass_disabled("inline:readonly_wrappers") &&
              tcc_ir_lookup_func_purity(tcc_state, func_cache_token(sym)) >= TCC_FUNC_PURITY_PURE
          ? SMALL_LEAF_INLINE_OPS
          : WRAPPER_INLINE_OPS;
  /* What the body costs a caller once expanded; the class limits below are in these ops. */
  const int inline_ops = ir ? ir_body_inline_ops(ir) : 0;

  /* Linkage does not gate the classes below: a non-static function's
   * standalone definition is always emitted (decl.c sets VT_INLINE — deferring
   * emission — only for static ones), so expanding its body at intra-TU call
   * sites is as safe as the static case; external callers keep the symbol.
   * The nonstatic_global_copier and nested-function guards still apply. */
  if (ir && body_is_final && sym->type.ref->f.func_auto_inline &&
      !sym->type.ref->f.func_inl_wrapper && wrappers_on && !sym->a.nested_func &&
      !nonstatic_global_copier)
  {
    int calls = 0;
    for (int ii = 0; ii < ir->next_instruction_index; ii++)
    {
      int op = ir->compact_instructions[ii].op;
      calls += (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID) &&
               !ir_call_is_barrier(ir, &ir->compact_instructions[ii]);
    }
    if (calls == 1 && inline_ops <= wrapper_inline_ops && ir_body_is_acyclic_wrapper(ir, 1))
    {
      sym->type.ref->f.func_small_leaf = 1;
      sym->type.ref->f.func_inl_wrapper = 1;
    }
  }

  /* Post-optimization re-inlining: if the optimized IR is trivial,
   * retroactively mark the function for auto-inlining so future callers
   * inline it via the existing token-replay mechanism.
   * Skip nested functions: marking them auto_inline causes the parent
   * to omit the frame pointer, breaking static chain access. */
  /* A body registration already made auto_inline (by token length) is
   * classified as well: as a small_leaf, the IR-size revoke below no longer
   * demotes it, while a body over the token limit gets the same class from
   * the block after this one.  Without it, raising -finline-limit UN-inlined
   * straight leaves such as math.rotr (+165M instructions on zig.c at 200),
   * and every registered leaf over 12 IR slots was judged as call-heavy. */
  if (ir && body_is_final && sym->type.ref->f.func_auto_inline && !sym->type.ref->f.func_small_leaf &&
      !sym->a.nested_func && !nonstatic_global_copier &&
      !tcc_ir_opt_pass_disabled("inline:leaf_rescue"))
  {
    if ((inline_ops <= SMALL_LEAF_INLINE_OPS && ir_body_is_acyclic_leaf(ir)) ||
        ir_body_is_small_loop_leaf(ir, sym, inline_ops))
      sym->type.ref->f.func_small_leaf = 1;
  }
  if (ir && body_is_final && !sym->type.ref->f.func_auto_inline &&
      !sym->a.nested_func &&
      !nonstatic_global_copier)
  {
    /* Count real instructions only for the fully-emptied case: late cleanup
     * can leave a NOP-only body uncompacted, which the slot count would miss.
     * Wider real-op counting newly inlines bodies the slot heuristic was
     * tuned to keep out of line (nestfunc-7). */
    int real_ops = -pcopy;
    for (int ii = 0; ii < ir->next_instruction_index; ii++)
      if (ir->compact_instructions[ii].op != TCCIR_OP_NOP)
        real_ops++;
    if (slots <= 8 || real_ops == 0)
    {
      sym->type.ref->f.func_auto_inline = 1;
      /* A trivial leaf is a small_leaf too: inlined at every site whatever
       * its token length, where decl.c's registration would have left it
       * eval-only (constant arguments). */
      if (ir_body_is_acyclic_leaf(ir) && !tcc_ir_opt_pass_disabled("inline:trivial_leaf"))
        sym->type.ref->f.func_small_leaf = 1;
    }
    else if (inline_ops <= SMALL_LEAF_INLINE_OPS && ir_body_is_acyclic_leaf(ir))
    {
      sym->type.ref->f.func_auto_inline = 1;
      sym->type.ref->f.func_small_leaf = 1;
    }
    else if (wrappers_on && inline_ops <= wrapper_inline_ops && ir_body_is_acyclic_wrapper(ir, 1))
    {
      sym->type.ref->f.func_auto_inline = 1;
      sym->type.ref->f.func_small_leaf = 1;
      sym->type.ref->f.func_inl_wrapper = 1;
    }
    else if (ir_body_is_small_loop_leaf(ir, sym, inline_ops))
    {
      sym->type.ref->f.func_auto_inline = 1;
      sym->type.ref->f.func_small_leaf = 1;
    }
  }

  /* Inlining aggregate-return scans exposes the result buffer to scalar replacement. */
  if (ir && body_is_final && TCC_OPT(tcc_state, optimize) >= 2 &&
      !TCC_OPT(tcc_state, optimize_size) && !sym->a.nested_func &&
      (sym->type.t & VT_STATIC) && sym->a.pure_via_sret &&
      !tcc_ir_opt_pass_disabled("inline:sret_loop") && ir_body_is_leaf(ir) && tcc_ir_cfg_has_cycle(ir))
  {
    for (int fi = 0; inline_ops <= SRET_LOOP_INLINE_OPS && fi < tcc_state->nb_inline_fns; fi++)
    {
      InlineFunc *fn = tcc_state->inline_fns[fi];
      if (fn->sym == sym && fn->func_str && fn->func_str->len <= SRET_LOOP_INLINE_TOKENS &&
          !inline_body_has_unsafe_loops(fn->func_str))
      {
        sym->type.ref->f.func_auto_inline = 1;
        sym->type.ref->f.func_small_leaf = 1;
        sym->type.ref->f.func_inl_sret_loop = 1;
        break;
      }
    }
  }

  /* Keep a non-static global-aggregate-copier with a parameter (the fn1/fn2
   * shape) out of line even after the late collapse shrank it below the
   * trivial-inline tier — the IR>12 demote below would miss the now-tiny body.
   * See nonstatic_global_copier above for why this set excludes retme/ini/960311. */
  if (ir && sym && sym->type.ref->f.func_auto_inline &&
      !sym->a.nested_func && !sym->type.ref->f.func_alwinl &&
      nonstatic_global_copier)
  {
    sym->type.ref->f.func_auto_inline = 0;
  }

  /* Post-optimization revoke: a function tagged auto_inline at registration
   * (based on token-stream length) may still produce a large IR if its body
   * is mostly calls to other helpers (cf. fail_u64 below: ~50 tokens but
   * ~20 IR ops dominated by FUNCCALL pairs).  Inlining such a function at N
   * call sites multiplies the call-heavy body by N for no real savings —
   * GCC keeps these helpers out-of-line.  We count "expensive" IR ops
   * (calls + control flow), and if the body looks call-heavy, demote it. */
  if (ir && sym && sym->type.ref->f.func_auto_inline &&
      !sym->a.nested_func &&
      !sym->type.ref->f.func_alwinl &&
      slots > 12)
  {
    int call_ops = 0;
    int has_aggr_copy = 0;
    int real_ops = -pcopy;
    for (int ii = 0; ii < ir->next_instruction_index; ii++)
    {
      int op = ir->compact_instructions[ii].op;
      if (op != TCCIR_OP_NOP)
        real_ops++;
      if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID)
      {
        call_ops++;
        Sym *cs = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, &ir->compact_instructions[ii]));
        const char *cn = cs ? get_tok_str(cs->v, NULL) : NULL;
        if (cn && (strstr(cn, "memmove") || strstr(cn, "memcpy")))
          has_aggr_copy = 1;
      }
    }
    /* Naturally-small functions (short token body) whose post-opt IR grew
     * mainly because their callees were inlined into them shouldn't be
     * demoted on IR size alone — the bloat is from in-body inlining, not
     * intrinsic complexity.  At a call site, the same inline expansion can
     * happen for the caller; const-prop + DCE will collapse it when args
     * are constant.  Look up the original token body length via inline_fns. */
    int natural_body_len = 0;
    for (int fi = 0; fi < tcc_state->nb_inline_fns; fi++)
    {
      if (tcc_state->inline_fns[fi]->sym == sym && tcc_state->inline_fns[fi]->func_str)
      {
        natural_body_len = tcc_state->inline_fns[fi]->func_str->len;
        break;
      }
    }
    /* Threshold: >=3 calls or IR larger than ~24 ops marks the body as
     * "too expensive to inline".  Naturally-small bodies (≤60 tokens) skip
     * the IR-size gate and only get demoted for call-heavy patterns. */
    int naturally_small = (natural_body_len > 0 && natural_body_len <= 60);
    /* A non-static function is always emitted standalone (its definition must
     * stay globally visible), so inlining a non-trivial body merely duplicates
     * it at every call site without dropping the out-of-line copy — pure bloat,
     * which GCC avoids by keeping such helpers out of line.  Only the ≤8-IR
     * "trivial" promote tier (handled above) is worth inlining for a non-static
     * function; demote anything larger here.  (Static functions can still be
     * inlined and have their standalone copy dropped by --gc-sections.) */
    int nonstatic_bloat =
        !(sym->type.t & VT_STATIC) && has_aggr_copy && slots > 8;
    /* Tail sharing can introduce backward jumps without introducing a loop. */
    int loop_body = !sym->type.ref->f.func_small_leaf && real_ops > 12 && tcc_ir_cfg_has_cycle(ir);
    if (!sym->type.ref->f.func_small_leaf &&
        (call_ops >= 3 || (!naturally_small && slots > 24) || nonstatic_bloat || loop_body))
    {
      /* Wrappers keep expansion with the per-caller budget. */
      if (wrappers_on && !nonstatic_global_copier && inline_ops <= wrapper_inline_ops &&
          ir_body_is_acyclic_wrapper(ir, 1))
      {
        sym->type.ref->f.func_small_leaf = 1;
        sym->type.ref->f.func_inl_wrapper = 1;
      }
      else
        sym->type.ref->f.func_auto_inline = 0;
    }
    /* A loop the body keeps is only worth duplicating at an all-constant call
     * site if ssa:loop_const_sim can then collapse it, which it only does for
     * register-only regions.  Record that here, where the optimized IR is in
     * hand, so tccgen keeps the token stream for exactly those helpers. */
    if (loop_body && ir_body_is_register_only(ir))
      sym->type.ref->f.func_const_arg_loop = 1;
  }

  /* Few call sites (FEW_CALLERS_GROWTH_OPS): what the expansion buys is the
   * call, an sret round trip, and whatever a constant argument folds there. */
  const int few_sites = sym && sym == tcc_state->deferred_sym ? tcc_state->deferred_call_sites : 0;
  if (ir && body_is_final && few_sites >= 2 && !sym->type.ref->f.func_auto_inline &&
      TCC_OPT(tcc_state, optimize) >= 2 && !TCC_OPT(tcc_state, optimize_size) && (sym->type.t & VT_STATIC) &&
      !sym->a.nested_func && !nonstatic_global_copier && ir_body_is_leaf(ir) &&
      (few_sites - 1) * (inline_ops > 0 ? inline_ops : 1) <= FEW_CALLERS_GROWTH_OPS &&
      !tcc_ir_opt_pass_disabled("inline:few_callers"))
  {
    InlineFunc *fn = inline_fn_lookup(tcc_state, sym);
    if (fn && fn->func_str && fn->func_str->len <= FEW_CALLERS_INLINE_TOKENS &&
        !inline_body_has_unsafe_loops(fn->func_str))
    {
      sym->type.ref->f.func_auto_inline = 1;
      sym->type.ref->f.func_inl_few_callers = 1;
      fn->few_callers_left = 2 * few_sites;
    }
  }

  /* Keep read-only loops visible to call-result reuse; see docs/inline_readonly_wrappers.md. */
  if (ir && body_is_final && sym->type.ref->f.func_auto_inline &&
      !sym->type.ref->f.func_alwinl && !sym->type.ref->f.func_inl_sret_loop &&
      !tcc_ir_opt_pass_disabled("inline:readonly_loops"))
  {
    TCCFuncPurity purity = tcc_ir_lookup_func_purity(tcc_state, func_cache_token(sym));
    if (purity == TCC_FUNC_PURITY_UNKNOWN)
      purity = tcc_ir_infer_func_purity(ir, sym);
    if (purity >= TCC_FUNC_PURITY_PURE && !ir_body_is_register_only(ir) && tcc_ir_cfg_has_cycle(ir) &&
        !ir_body_is_small_predicate(ir, sym))
    {
      sym->type.ref->f.func_auto_inline = 0;
      sym->type.ref->f.func_small_leaf = 0;
      sym->type.ref->f.func_inl_wrapper = 0;
      sym->type.ref->f.func_inl_few_callers = 0;
    }
  }

  /* Mark surviving auto-inline candidates whose body keeps a non-trivial,
   * non-foldable call (e.g. a printf wrapper) as "call-heavy".  Inlining
   * such a body at every call site duplicates the surviving call for no
   * savings; when a function like this is called dozens of times (macro-
   * generated check() in 55_lshift_type), unbounded expansion explodes the
   * compiler's memory.  The call-site logic budget-limits how many times a
   * call-heavy callee is expanded before falling back to a normal call. */
  if (ir && sym && sym->type.ref->f.func_auto_inline &&
      slots > 8)
  {
    for (int ii = 0; ii < ir->next_instruction_index; ii++)
    {
      int op = ir->compact_instructions[ii].op;
      if ((op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID) &&
          !ir_call_is_barrier(ir, &ir->compact_instructions[ii]))
      {
        sym->type.ref->f.func_inline_call_heavy = 1;
        break;
      }
    }
  }

  if (ir && body_is_final)
    for (int fi = 0; fi < tcc_state->nb_inline_fns; fi++)
    {
      InlineFunc *fn = tcc_state->inline_fns[fi];
      if (fn->sym != sym)
        continue;
      const int leaf = sym->type.ref->f.func_auto_inline && sym->type.ref->f.func_small_leaf &&
                       !sym->type.ref->f.func_inl_wrapper;
      fn->leaf_ops = leaf ? (inline_ops > 0 ? inline_ops : 1) : 0;
      fn->leaf_backedges = leaf ? ir_body_backedges(ir) : 0;
      fn->leaf_bounded = leaf && sym->type.ref->f.func_inl_sret_loop;
      break;
    }

  /* Guard head (docs/inline_guard_heads.md); the call repeats the test, so no volatile access.  A recompiled
   * body without one drops its earlier head. */
  TokenString *guard_head = NULL;
  int *guard_free = NULL;
  const int *guard_pack = NULL;
  if (ir && body_is_final && !sym->type.ref->f.func_auto_inline && !sym->type.ref->f.func_noinline &&
      (sym->type.t & VT_STATIC) && !sym->a.nested_func && !ir->func_has_volatile_access &&
      TCC_OPT(tcc_state, optimize) >= 2 && !TCC_OPT(tcc_state, optimize_size) &&
      !tcc_ir_opt_pass_disabled("inline:guard_head"))
  {
    InlineFunc *fn = inline_fn_lookup(tcc_state, sym);
    if (fn && fn->func_str)
    {
      guard_head = inline_body_guard_head(fn->func_str, sym, &guard_free);
      guard_pack = fn->pack;
    }
  }
  if (guard_head || sym->a.guard_head)
    guard_head_register(tcc_state, sym, guard_head, guard_pack, guard_free);

  /* TCC_INL_DBG: the post-optimization inline classification of every function (with [INLSITE] lines from
   * the call sites that expand one). */
  {
    static int dbg = -1;
    if (dbg < 0)
      dbg = getenv("TCC_INL_DBG") != NULL;
    if (dbg && ir && sym)
    {
      int ro = 0, calls = 0;
      for (int ii = 0; ii < ir->next_instruction_index; ii++)
      {
        int op = ir->compact_instructions[ii].op;
        ro += op != TCCIR_OP_NOP;
        calls += op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID;
      }
      fprintf(stderr, "[INL] %s slots=%d ops=%d cost=%d calls=%d auto=%d leaf=%d evalonly=%d heavy=%d guard=%d bytes=%d\n",
              get_tok_str(sym->v, NULL), ir->next_instruction_index, ro, inline_ops, calls, sym->type.ref->f.func_auto_inline,
              sym->type.ref->f.func_small_leaf, sym->type.ref->f.func_eval_only_inline,
              sym->type.ref->f.func_inline_call_heavy, sym->a.guard_head, ind - func_ind);
    }
  }
  tcc_debug_funcend(tcc_state, ind - func_ind);

  elfsym(sym)->st_size = ind - func_ind;

  cur_text_section->data_offset = ind;
  /* An identical body was generated already: drop this one and let the
   * symbol point at it (identical code folding). */
  if (tcc_icf_try_fold(tcc_state, sym, cur_text_section, func_ind, ind - func_ind, func_reloc_mark))
  {
    /* The word-alignment pad codegen put in front of the body goes with it. */
    ind = func_ind - func_align_pad;
    cur_text_section->data_offset = ind;
  }
  /* Publish what ra:known_ext proved r0 holds at every return, for the direct
   * calls compiled after this.  A folded body is another function's, which
   * may yet be recompiled under its own promise: publish nothing for it.  A
   * recompile only ever narrows the class -- gfunc_return masked every return
   * value to the one callers were already given. */
  else if (ir && sym->type.ref && ir->ret_zext_class &&
           (!sym->type.ref->f.func_ret_zext || ir->ret_zext_class < sym->type.ref->f.func_ret_zext))
    sym->type.ref->f.func_ret_zext = ir->ret_zext_class;
  local_scope = 0;
  label_pop(&global_label_stack, global_label_stack_start, 0);

  {
    LabelDiffFixup *f = tcc_state->label_diff_fixups;
    while (f)
    {
      LabelDiffFixup *next = f->next;
      ElfSym *esym_plus = elfsym(f->sym_plus);
      ElfSym *esym_minus = elfsym(f->sym_minus);
      if (esym_plus && esym_minus)
      {
        int32_t diff = (int32_t)esym_plus->st_value - (int32_t)esym_minus->st_value;
        unsigned char *where = f->sec->data + f->offset;
        if (f->size == 1)
          where[0] = (unsigned char)(where[0] + diff);
        else if (f->size == 2)
          write16le(where, (uint16_t)(read16le(where) + diff));
        else
          add32le(where, diff);
      }
      tcc_free(f);
      f = next;
    }
    tcc_state->label_diff_fixups = NULL;
  }

  if (ir && ir->ir_to_code_mapping)
  {
    tcc_free(ir->ir_to_code_mapping);
    ir->ir_to_code_mapping = NULL;
    ir->ir_to_code_mapping_size = 0;
  }
  sym_pop(&all_cleanups, NULL, 0);

  /* It's better to crash than to generate wrong code */
  cur_text_section = NULL;
#ifdef CONFIG_TCC_DEBUG
  tcc_state->dump_ir = saved_dump_ir;
  tcc_state->dump_ir_passes = saved_dump_ir_passes;
#endif
  funcname = "";       /* for safety */
  func_vt.t = VT_VOID; /* for safety */
  func_var = 0;        /* for safety */
  ind = 0;             /* for safety */
  func_ind = -1;
  tcc_state->cur_func_sym = NULL;
  nocode_wanted = DATA_ONLY_WANTED;
  check_vstack();

  /* do this after funcend debug info */
  next();
  tcc_ir_free(ir);
  tcc_state->ir = NULL;

  /* Publish the fact that this function's body has been compiled in this
   * TU.  Read by gen_function's late_reopt trigger on later-compiled
   * callers (and by the inter-procedural noreturn propagation in general). */
  if (sym && sym->type.ref)
    sym->type.ref->f.func_compiled = 1;
}

/* Parse SYM's body, the current token stream from its '{', for the
 * diagnostics it raises and nothing else: prune_unused_statics dropped the
 * function, but an error in it is still an error (gcc reports it too).
 * The frontend runs as for gen_function -- parameters in scope, code on --
 * into an IR that is freed afterwards, with tcc_state->check_only keeping
 * every other product out of the object (see its comment in tcc.h).  No
 * optimization, no codegen.  Nested function bodies are saved by the parse
 * and generated only by codegen, so they are not checked. */
void gen_function_check_only(Sym *sym)
{
  struct scope f = {0};
  Sym *global_label_stack_start = global_label_stack;
  LabelDiffFixup *fixups_start = tcc_state->label_diff_fixups;
  const int nested_start = tcc_state->nb_nested_funcs;
  Section *saved_text = cur_text_section;

  tcc_state->check_only++;
  nested_names_reset();
  cur_scope = root_scope = &f;
  nocode_wanted = 0;
  cur_text_section = tcc_state->check_scratch;
  ind = 0;
  funcname = get_tok_str(sym->v, NULL);
  func_ind = 0;
  func_vt = sym->type.ref->type;
  func_var = sym->type.ref->f.func_type == FUNC_ELLIPSIS;
  func_has_label_addr = 0;
  tcc_state->cur_func_sym = sym;
  tcc_state->const_loop_inline_used = 0;

  sym_push2(&local_stack, SYM_FIELD, 0, 0);
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->ir = ir;
  gen_op_vector_reset();
  ir->is_variadic = func_var;
  local_scope = 1;
  int nb_knr_conv;
  KnrConv *knr_conv = knr_params_arrive(sym, &nb_knr_conv);
  tcc_ir_params_add(ir, &sym->type);
  knr_params_receive(knr_conv, nb_knr_conv);
  nb_temp_local_vars = 0;
  local_scope = 0;
  rsym = -1;
  block(0);

  sym_pop(&local_stack, NULL, 0);
  sym_pop(&all_cleanups, NULL, 0);
  label_pop(&global_label_stack, global_label_stack_start, 0);
  while (tcc_state->label_diff_fixups && tcc_state->label_diff_fixups != fixups_start)
  {
    LabelDiffFixup *next = tcc_state->label_diff_fixups->next;
    tcc_free(tcc_state->label_diff_fixups);
    tcc_state->label_diff_fixups = next;
  }
  for (int i = nested_start; i < tcc_state->nb_nested_funcs; i++)
    if (tcc_state->nested_funcs[i]->func_str)
    {
      tok_str_free(tcc_state->nested_funcs[i]->func_str);
      tcc_state->nested_funcs[i]->func_str = NULL;
    }
  tcc_state->nb_nested_funcs = nested_start;

  cur_text_section = saved_text;
  funcname = "";
  func_vt.t = VT_VOID;
  func_var = 0;
  ind = 0;
  func_ind = -1;
  tcc_state->cur_func_sym = NULL;
  nocode_wanted = DATA_ONLY_WANTED;
  check_vstack();
  next();
  tcc_ir_free(ir);
  tcc_state->ir = NULL;
  tcc_state->check_only--;
}

/* ------------------------------------------------------------------ */
/* Helpers that were extracted from tccgen.c because they are only
 * called from gen_function().  Keeping them here avoids orphaned-static
 * warnings when tccgen.c is compiled without gen_function inline.      */

/* Emit a call to __cyg_profile_func_enter or __cyg_profile_func_exit.
 * Used for -finstrument-functions support.
 * Arguments: (void *this_fn, void *call_site) */
static void gen_instrument_call(Sym *cur_func_sym, const char *hook_name)
{
  CType void_ptr_type;
  void_ptr_type.t = VT_VOID;
  void_ptr_type.ref = NULL;
  mk_pointer(&void_ptr_type);

  /* arg0: address of current function */
  vpushsym(&void_ptr_type, cur_func_sym);

  /* arg1: return address (call site) = __builtin_return_address(0) */
  vpush_return_address();

  /* Push the hook function */
  vpush_helper_func(tok_alloc_const(hook_name));

  /* Emit IR for 2-arg void call: hook(this_fn, call_site) */
  const int call_id = tcc_state->ir->next_call_id++;
  SValue param_num;
  svalue_init(&param_num);
  param_num.vr = -1;
  param_num.r = VT_CONST;

  param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
  tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-2], &param_num, NULL);
  param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
  tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &param_num, NULL);

  SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 2);
  tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[0], &call_id_sv, NULL);
  vtop -= 3; /* pop 2 args + func */
}

/* generate vla code saved in post_type() */
static void func_vla_arg_code(Sym *arg)
{
  int align;
  TokenString *vla_array_tok = NULL;

  if (arg->type.ref)
    func_vla_arg_code(arg->type.ref);

  if ((arg->type.t & VT_VLA) && arg->type.ref->vla_array_str)
  {
    loc -= type_size(&int_type, &align);
    loc &= -align;
    arg->type.ref->c = loc;

    unget_tok(0);
    vla_array_tok = tok_str_alloc();
    vla_array_tok->data.str = arg->type.ref->vla_array_str;
    vla_array_tok->allocated_len = 1;
    begin_macro(vla_array_tok, 2); /* alloc=2: don't free borrowed buffer */
    next();
    gexpr();
    end_macro();
    next();
    vpush_type_size(&arg->type.ref->type, &align);
    gen_op('*');
    vset(&int_type, VT_LOCAL | VT_LVAL, arg->type.ref->c);
    vswap();
    vstore();
    vpop();
    for (int i = 0; i < tcc_state->nb_vla_inner_exprs; i++)
      if (tcc_state->vla_inner_exprs[i] == arg->type.ref->vla_array_str)
      {
        tcc_state->vla_inner_exprs[i] = NULL;
        break;
      }
    tcc_free(arg->type.ref->vla_array_str);
    arg->type.ref->vla_array_str = NULL;
  }
}

static void func_vla_arg(Sym *sym)
{
  Sym *arg;

  for (arg = sym->type.ref->next; arg; arg = arg->next)
  {
    if ((arg->type.t & VT_BTYPE) != VT_PTR)
      continue;
    if (arg->type.ref->type.t & VT_VLA)
      func_vla_arg_code(arg->type.ref);
    for (int i = 0; i < tcc_state->nb_vla_param_exprs; i++)
    {
      if (tcc_state->vla_param_exprs[i].param == arg->type.ref)
      {
        TokenString *vla_array_tok = tok_str_alloc();
        vla_array_tok->data.str = tcc_state->vla_param_exprs[i].tokens;
        vla_array_tok->allocated_len = 1;
        unget_tok(0);
        begin_macro(vla_array_tok, 2); /* alloc=2: don't free borrowed buffer */
        next();
        gexpr();
        end_macro();
        next();
        vpop(); /* discard result, only side effects matter */
        break;
      }
    }
  }
  if (tcc_state->nb_vla_param_exprs)
  {
    for (int i = 0; i < tcc_state->nb_vla_param_exprs; i++)
      tcc_free(tcc_state->vla_param_exprs[i].tokens);
    tcc_free(tcc_state->vla_param_exprs);
    tcc_state->vla_param_exprs = NULL;
    tcc_state->nb_vla_param_exprs = 0;
  }
}
