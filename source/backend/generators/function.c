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
extern int nb_arg_struct_temps;
extern uint64_t arg_struct_temp_busy;
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
static int  ir_inline_stash_eligible(Sym *sym, TCCIRState *ir);
static void ir_inline_stash_add(TCCState *s1, Sym *sym, TCCIRState *ir);

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

  funcname = get_tok_str(sym->v, NULL);
  func_ind = ind;
  func_vt = sym->type.ref->type;
  func_var = sym->type.ref->f.func_type == FUNC_ELLIPSIS;
  func_has_label_addr = 0;
  tcc_state->cur_func_sym = sym;

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

  if (tcc_state->current_nested_func && tcc_state->current_nested_func->nb_captured > 0)
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

  if (tcc_state->opt_fp_offset_cache)
    tcc_ir_opt_fp_cache_init(ir);

  local_scope = 1;
  tcc_ir_params_add(ir, &sym->type);

  if (ir->has_static_chain)
    loc -= 4;
  nb_temp_local_vars = 0;
  nb_arg_struct_temps = 0;
  arg_struct_temp_busy = 0;

  local_scope = 0;
  rsym = -1;

  /* -finstrument-functions: emit entry hook call before function body */
  if (tcc_state->instrument_functions && !sym->type.ref->f.func_no_instrument)
  {
    tcc_state->force_frame_pointer = 1;
    tcc_state->force_lr_save = 1;
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
      if (tcc_state->nested_funcs[ni].trampoline_needed)
        any_trampoline_needed = 1;
    }
    if (!func_has_chain_op && !any_trampoline_needed) {
      for (int ni = 0; ni < tcc_state->nb_nested_funcs; ni++) {
        NestedFunc *nf = &tcc_state->nested_funcs[ni];
        for (int ci = 0; ci < nf->nb_captured; ci++) {
          int vreg = nf->captured_vregs[ci];
          if (vreg >= 0) {
            IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
            if (interval)
              interval->addrtaken = 0;
          }
        }
      }
    }
  }
  if (ir && tcc_state->nb_nested_funcs > 0) {
    for (int ni = 0; ni < tcc_state->nb_nested_funcs; ni++) {
      NestedFunc *nf = &tcc_state->nested_funcs[ni];
      if (!nf->sym || !nf->sym->type.ref || !nf->sym->type.ref->f.func_auto_inline)
        continue;
      if (nf->trampoline_needed || nf->nb_captured == 0)
        continue;
      int called_by_sibling = 0;
      int func_tok = nf->sym->v & ~SYM_FIELD;
      for (int si = 0; si < tcc_state->nb_nested_funcs && !called_by_sibling; si++) {
        NestedFunc *sib = &tcc_state->nested_funcs[si];
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
          if (vreg >= 0) {
            int keep_addrtaken = 0;
            for (int oi = 0; oi < tcc_state->nb_nested_funcs && !keep_addrtaken; oi++) {
              NestedFunc *other = &tcc_state->nested_funcs[oi];
              if (other == nf || other->nb_captured == 0)
                continue;

              int captures_vreg = 0;
              for (int oc = 0; oc < other->nb_captured; oc++) {
                if (other->captured_vregs[oc] == vreg) {
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
                  NestedFunc *sib = &tcc_state->nested_funcs[si];
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
              IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
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
  tcc_ir_opt_run_function_pipeline(ir, sym, func_var, &nonstatic_global_copier);

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

  tcc_ir_codegen_generate(ir);

  if (ir->barrel_shifts) {
    tcc_free(ir->barrel_shifts);
    ir->barrel_shifts = NULL;
    ir->barrel_shifts_len = 0;
  }
  if (ir->zero_half64) {
    tcc_free(ir->zero_half64);
    ir->zero_half64 = NULL;
    ir->zero_half64_len = 0;
  }
  if (ir->shift64_dead_half) {
    tcc_free(ir->shift64_dead_half);
    ir->shift64_dead_half = NULL;
    ir->shift64_dead_half_len = 0;
  }
  if (ir->bfi_params) {
    tcc_free(ir->bfi_params);
    ir->bfi_params = NULL;
    ir->bfi_params_len = 0;
  }

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
  if (tcc_state->opt_licm && ir && sym)
  {
    /* Forward declare the inference function */
    extern TCCFuncPurity tcc_ir_infer_func_purity(TCCIRState * ir, Sym * func_sym);
    extern void tcc_ir_cache_func_purity(TCCState * s, int func_token, TCCFuncPurity purity);

    TCCFuncPurity purity = tcc_ir_infer_func_purity(ir, sym);
    tcc_ir_cache_func_purity(tcc_state, sym->v, purity);
  }

  if (tcc_state->opt_ipc && ir && sym)
  {
    int64_t const_val;
    int const_btype;
    int const_cached = 0;
    if (tcc_ir_detect_const_result(ir, &const_val, &const_btype))
    {
      tcc_ir_cache_const_result(tcc_state, sym->v, const_val, const_btype);
      const_cached = 1;
    }
    /* If the function isn't a plain const-returning function but is a pure
     * single-parameter dispatcher (switch / if-chain over the arg returning
     * constants), snapshot it so callers passing a constant can fold the call. */
    if (!const_cached)
    {
      TCCFuncSwitchSnapshot *snap = NULL;
      if (tcc_ir_detect_switch_func(ir, &snap))
        tcc_ir_cache_switch_func(tcc_state, sym->v, snap);
    }
  }

  /* Post-optimization re-inlining: if the optimized IR is trivial,
   * retroactively mark the function for auto-inlining so future callers
   * inline it via the existing token-replay mechanism.
   * Skip nested functions: marking them auto_inline causes the parent
   * to omit the frame pointer, breaking static chain access. */
  if (ir && sym && !sym->type.ref->f.func_auto_inline &&
      !sym->a.nested_func &&
      !nonstatic_global_copier)
  {
    /* Count real instructions only for the fully-emptied case: late cleanup
     * can leave a NOP-only body uncompacted, which the slot count would miss.
     * Wider real-op counting newly inlines bodies the slot heuristic was
     * tuned to keep out of line (nestfunc-7). */
    int real_ops = 0;
    for (int ii = 0; ii < ir->next_instruction_index; ii++)
      if (ir->compact_instructions[ii].op != TCCIR_OP_NOP)
        real_ops++;
    if (ir->next_instruction_index <= 8 || real_ops == 0)
      sym->type.ref->f.func_auto_inline = 1;
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
      ir->next_instruction_index > 12)
  {
    int call_ops = 0;
    int has_aggr_copy = 0;
    int has_loop = 0;
    int real_ops = 0;
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
      if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, &ir->compact_instructions[ii]);
        if ((int)irop_get_imm64_ex(ir, jd) <= ii)
          has_loop = 1;
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
        !(sym->type.t & VT_STATIC) && has_aggr_copy && ir->next_instruction_index > 8;
    /* A body that keeps a loop after optimization is dominated by its loop at
     * runtime, so inlining buys only the call overhead while duplicating the
     * loop at every site — GCC keeps such helpers out of line.  The slot-count
     * gate below used to catch these by accident (unfused 64-bit and indexed
     * code was fat); ldrd/strd pairing shrank pr38048-2's foo from 27 slots to
     * 24 with identical real ops and flipped it straight through the `> 24`
     * boundary, inlining loop bodies into main in pr38048-2 (+23), pr53163
     * (+47), 258_derived_iv_strength_reduction (+32) and builtin-bitops-1
     * (+183).  Count real ops, not slots, so residual NOPs cannot decide.
     * The ≤12 floor keeps tiny loop helpers inlinable for the constant-arg
     * path, where loop_const_sim can still fold the whole thing. */
    int loop_body = has_loop && real_ops > 12;
    if (call_ops >= 3 || (!naturally_small && ir->next_instruction_index > 24) ||
        nonstatic_bloat || loop_body)
    {
      sym->type.ref->f.func_auto_inline = 0;
    }
    /* A loop the body keeps is only worth duplicating at an all-constant call
     * site if ssa:loop_const_sim can then collapse it, which it only does for
     * register-only regions.  Record that here, where the optimized IR is in
     * hand, so tccgen keeps the token stream for exactly those helpers. */
    if (loop_body && ir_body_is_register_only(ir))
      sym->type.ref->f.func_const_arg_loop = 1;
  }

  /* Mark surviving auto-inline candidates whose body keeps a non-trivial,
   * non-foldable call (e.g. a printf wrapper) as "call-heavy".  Inlining
   * such a body at every call site duplicates the surviving call for no
   * savings; when a function like this is called dozens of times (macro-
   * generated check() in 55_lshift_type), unbounded expansion explodes the
   * compiler's memory.  The call-site logic budget-limits how many times a
   * call-heavy callee is expanded before falling back to a normal call. */
  if (ir && sym && sym->type.ref->f.func_auto_inline &&
      ir->next_instruction_index > 8)
  {
    for (int ii = 0; ii < ir->next_instruction_index; ii++)
    {
      int op = ir->compact_instructions[ii].op;
      if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID)
      {
        sym->type.ref->f.func_inline_call_heavy = 1;
        break;
      }
    }
  }

  tcc_debug_funcend(tcc_state, ind - func_ind);

  elfsym(sym)->st_size = ind - func_ind;

  cur_text_section->data_offset = ind;
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
        add32le(f->sec->data + f->offset, diff);
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
  if (ir_inline_stash_eligible(sym, ir))
  {
    ir_inline_stash_add(tcc_state, sym, ir);
  }
  else
  {
    tcc_ir_free(ir);
  }
  tcc_state->ir = NULL;

  /* Publish the fact that this function's body has been compiled in this
   * TU.  Read by gen_function's late_reopt trigger on later-compiled
   * callers (and by the inter-procedural noreturn propagation in general). */
  if (sym && sym->type.ref)
    sym->type.ref->f.func_compiled = 1;
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

  /* arg1: return address (call site) = __builtin_return_address(0)
   * LR is saved at [FP + PTR_SIZE] in the standard frame record */
  CType ptr_type;
  ptr_type.t = VT_VOID;
  ptr_type.ref = NULL;
  mk_pointer(&ptr_type);
  vset(&ptr_type, VT_LOCAL, 0); /* FP value */
  vpushi(PTR_SIZE);
  gen_op('+');
  mk_pointer(&vtop->type);
  indir();

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

/* Phase 0 inliner stash: keep optimized IR of eligible `static` functions
 * alive past gen_function() so a future inliner pass can splice it into
 * callers. Today there are no consumers — this exists to validate the
 * lifecycle change (no leaks, no regressions) before the splice logic lands. */
#ifndef IR_INLINE_STASH_SIZE_BUDGET
#define IR_INLINE_STASH_SIZE_BUDGET 200
#endif

static int ir_inline_stash_eligible(Sym *sym, TCCIRState *ir)
{
  if (!sym || !ir)
    return 0;
  if (!(sym->type.t & VT_STATIC))
    return 0;
  if (sym->a.addrtaken)
    return 0;
  if (!sym->type.ref || sym->type.ref->f.func_type == FUNC_ELLIPSIS)
    return 0;
  if (ir->has_static_chain)
    return 0;
  if (ir->nb_nested_funcs > 0)
    return 0;
  if (ir->naked)
    return 0;
#ifdef CONFIG_TCC_ASM
  if (ir->inline_asm_count > 0)
    return 0;
#endif
  if (ir->next_instruction_index > IR_INLINE_STASH_SIZE_BUDGET)
    return 0;
  return 1;
}

static void ir_inline_stash_add(TCCState *s1, Sym *sym, TCCIRState *ir)
{
  if (s1->nb_stashed_func_irs >= s1->stashed_func_irs_capacity)
  {
    s1->stashed_func_irs_capacity = s1->stashed_func_irs_capacity ? s1->stashed_func_irs_capacity * 2 : 4;
    s1->stashed_func_irs =
        tcc_realloc(s1->stashed_func_irs, s1->stashed_func_irs_capacity * sizeof(StashedFuncIR));
  }
  s1->stashed_func_irs[s1->nb_stashed_func_irs].sym = sym;
  s1->stashed_func_irs[s1->nb_stashed_func_irs].ir = ir;
  s1->nb_stashed_func_irs++;
}
