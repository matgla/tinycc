/*
 *  TCC Backend - Register-Allocation & Codegen-Prep Pipeline
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * Split out from source/opt/function_pipeline.c: the optimization phases
 * stay in source/opt; the target-dependent RA + codegen preparation lives
 * here next to gen_function (source/backend/generators/function.c).
 */

#include "tcc.h"
#include "ir/cfg.h"
#include "ir/codegen.h"
#include "ir/core.h"
#include "ir/licm.h"
#include "ir/opt.h"
#include "ir/opt_utils.h"
#include "ir/opt_engine.h"
#include "ir/opt_pipeline.h"
#include "ir/regalloc.h"
#include "ir/ssa.h"
#include "tccir.h"
#include "arch/arm/arm_regalloc.h"

#include "regalloc.h"

extern void compile_nested_functions(Sym *parent_sym);
extern void pop_local_syms(Sym *b, int keep);
extern void tcc_bench_log_phase(TCCState *s1, const char *operation, const char *name,
                                unsigned *total_time, unsigned *count, unsigned elapsed);
extern void dbg_scan_imm_dest(TCCIRState *ir, const char *pass);
extern void dbg_scan_overlap(TCCIRState *ir, const char *pass);

/* ================================================================== */
/*  Analyze whether the function is a leaf / tail-call-only           */
/* ================================================================== */
void tcc_ir_backend_analyze_leaf_and_tail_calls(TCCIRState *ir, int func_var)
{
  ir->leaffunc = 1;
  ir->tail_call_only = 0;
  int call_count = 0;
  int call_idx = -1;
  int has_complex_fp = 0;
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      ir->leaffunc = 0;
      call_count++;
      call_idx = i;
    }
    else if (q->op == TCCIR_OP_BUILTIN_APPLY)
    {
      ir->leaffunc = 0;
      call_count = 99;
    }
    if (q->op == TCCIR_OP_FADD || q->op == TCCIR_OP_FSUB || q->op == TCCIR_OP_FMUL || q->op == TCCIR_OP_FDIV)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (dest.is_complex)
      {
        ir->leaffunc = 0;
        has_complex_fp = 1;
      }
    }
  }

  if (call_count == 1 && !has_complex_fp && !func_var && !ir->has_static_chain && call_idx >= 0)
  {
    const IRQuadCompact *cq = &ir->compact_instructions[call_idx];
    int is_tail = 0;

    int j = call_idx + 1;
    while (j < ir->next_instruction_index && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;

    if (j < ir->next_instruction_index)
    {
      const IRQuadCompact *nq = &ir->compact_instructions[j];
      if (!nq->is_jump_target)
      {
        if (cq->op == TCCIR_OP_FUNCCALLVOID && nq->op == TCCIR_OP_RETURNVOID)
        {
          is_tail = 1;
        }
        else if (cq->op == TCCIR_OP_FUNCCALLVAL && nq->op == TCCIR_OP_RETURNVALUE)
        {
          IROperand call_dest = tcc_ir_op_get_dest(ir, cq);
          IROperand ret_src = tcc_ir_op_get_src1(ir, nq);
          int call_vr = irop_get_vreg(call_dest);
          int ret_vr = irop_get_vreg(ret_src);
          if (call_vr >= 0 && call_vr == ret_vr)
            is_tail = 1;
        }
        else if (cq->op == TCCIR_OP_FUNCCALLVOID && nq->op == TCCIR_OP_RETURNVALUE)
        {
          /* void call followed by value return — not a tail call */
        }
        else if (cq->op == TCCIR_OP_FUNCCALLVAL && nq->op == TCCIR_OP_RETURNVOID)
        {
          is_tail = 1;
        }
      }
    }

    if (is_tail)
    {
      for (int k = j + 1; k < ir->next_instruction_index; k++)
      {
        if (ir->compact_instructions[k].op != TCCIR_OP_NOP)
        {
          is_tail = 0;
          break;
        }
      }
    }

    if (is_tail)
    {
      ir->tail_call_only = 1;
      ir->leaffunc = 1;
    }
  }
}

/* ================================================================== */
/*  Compute min stack offset from instruction operands                */
/* ================================================================== */
static void compute_min_stack_ref(TCCIRState *ir, int func_var)
{
  int min_stack_ref = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = tcc_ir_op_get_dest(ir, q);
    ops[1] = tcc_ir_get_src1(ir, i);
    ops[2] = tcc_ir_get_src2(ir, i);
    for (int j = 0; j < 3; j++)
    {
      if (ops[j].tag == IROP_TAG_STACKOFF)
      {
        int off = irop_get_stack_offset(ops[j]);
        if (off < min_stack_ref)
          min_stack_ref = off;
      }
    }
  }
  if (min_stack_ref > loc)
  {
    loc = min_stack_ref;
  }
  if (func_var && loc > -28)
    loc = -28;
}

/* ================================================================== */
/*  Setup register allocation budget: ijump cap, setjmp marking       */
/* ================================================================== */
static void setup_register_allocation(TCCIRState *ir, int func_var)
{
  compute_min_stack_ref(ir, func_var);

  {
    int has_ijmp = 0;
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      {
        has_ijmp = 1;
        break;
      }
    }
    if (has_ijmp && tcc_state->registers_for_allocator > 12)
      tcc_state->registers_for_allocator = 12;
  }
  if (tcc_state->optimize < 1 && tcc_state->registers_for_allocator > 12)
    tcc_state->registers_for_allocator = 12;

  {
    int has_setjmp = 0;
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      int op = ir->compact_instructions[i].op;
      if (op == TCCIR_OP_SETJMP || op == TCCIR_OP_NL_SETJMP)
      {
        has_setjmp = 1;
        break;
      }
    }
    if (has_setjmp)
    {
      for (int i = 0; i < ir->next_instruction_index; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        for (int k = 0; k < 3; k++)
        {
          IROperand op = (k == 0)   ? tcc_ir_op_get_dest(ir, q)
                         : (k == 1) ? tcc_ir_op_get_src1(ir, q)
                                    : tcc_ir_op_get_src2(ir, q);
          int32_t vr = irop_get_vreg(op);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
            tcc_ir_set_addrtaken(ir, vr);
        }
      }
    }
  }
}

/* ================================================================== */
/*  Post-RA micro-optimizations: bitfield, barrel shift, narrow, etc. */
/* ================================================================== */
static void run_post_ra_optimizations(TCCIRState *ir)
{
  if (tcc_state->optimize > 0)
    tcc_ir_opt_bitfield_insert_to_bfi(ir);

  if (tcc_state->optimize > 0)
    tcc_ir_barrel_shift_fusion(ir);

  if (tcc_state->optimize > 0)
    tcc_ir_opt_shift_pair_to_ubfx(ir);

  if (tcc_state->optimize > 0)
    tcc_ir_opt_shift64_dead_half(ir);

  if (tcc_state->optimize > 0)
    tcc_ir_opt_narrow_store_value_btype(ir);
}

/* ================================================================== */
/*  SSA register allocation + post-RA passes (phi hoist, diamond fwd, */
/*  abort tail merge)                                                 */
/* ================================================================== */
static void run_ssa_and_post_ra_passes(TCCIRState *ir)
{
  {
    const RegAllocTarget *ra_target = arm_get_regalloc_target();
    dbg_scan_imm_dest(ir, "before-ssa-regalloc");
    dbg_scan_overlap(ir, "before-ssa-regalloc");
    tcc_ir_ssa_regalloc(ir, ra_target, loc);
    dbg_scan_imm_dest(ir, "after-ssa-regalloc");
  }

  if (tcc_state->optimize > 0 && !tcc_ir_opt_pass_disabled("ra:backedge_phi_hoist"))
    tcc_ir_opt_backedge_phi_hoist(ir);
  tcc_ir_dump_after_pass(ir, "ra:backedge_phi_hoist");
  dbg_scan_imm_dest(ir, "after-backedge-phi-hoist");

  if (tcc_state->optimize > 0)
    tcc_ir_opt_post_ra_forward_diamond(ir);
  dbg_scan_imm_dest(ir, "after-post-ra-fwd-diamond");

  if (tcc_state->optimize > 0)
    tcc_ir_opt_abort_tail_merge(ir);
}

/* ================================================================== */
/*  Iterative jump threading loop                                     */
/* ================================================================== */
static void run_jump_threading_loop(TCCIRState *ir)
{
  if (tcc_state->opt_jump_threading) {
    for (int outer = 0; outer < 3; outer++) {
      int jt_changes;
      do {
        jt_changes = tcc_ir_opt_jump_threading(ir);
        jt_changes += tcc_ir_opt_eliminate_fallthrough(ir);
        jt_changes += tcc_ir_opt_jumpif_invert(ir, 1);
        jt_changes += tcc_ir_opt_orphan_cmp_elim(ir);
      } while (jt_changes > 0);
      if (!tcc_state->opt_dce || tcc_ir_opt_dce(ir) == 0)
         break;
    }
  }
}

/* ================================================================== */
/*  Register coalescing: live-range swap optimization                 */
/* ================================================================== */
static void run_register_coalescing(TCCIRState *ir)
{
  for (int hi = 0; hi < ir->ls.next_interval_index; hi++)
  {
    LSLiveInterval *hint_li = &ir->ls.intervals[hi];
    if (hint_li->r0 < 0 || hint_li->r1 >= 0 || hint_li->crosses_call)
      continue;

    if (TCCIR_DECODE_VREG_TYPE(hint_li->vreg) != TCCIR_VREG_TYPE_VAR)
      continue;

    IRLiveInterval *hint_iri = tcc_ir_vreg_live_interval(ir, hint_li->vreg);
    if (!hint_iri || hint_iri->incoming_reg0 < 0)
      continue;

    int wanted_reg = hint_iri->incoming_reg0;
    if (hint_li->r0 == wanted_reg)
      continue;

    int have_reg = hint_li->r0;

    LSLiveInterval *blocker = NULL;
    for (int bi = 0; bi < ir->ls.next_interval_index; bi++)
    {
      LSLiveInterval *b = &ir->ls.intervals[bi];
      if (b->r0 != wanted_reg || b->r1 >= 0 || b->crosses_call)
        continue;
      if (b->start > hint_li->end || b->end < hint_li->start)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(b->vreg) == TCCIR_VREG_TYPE_PARAM)
      {
        blocker = NULL;
        break;
      }
      blocker = b;
      break;
    }
    if (!blocker)
      continue;

    int safe = 1;
    for (int ci = 0; ci < ir->ls.next_interval_index; ci++)
    {
      LSLiveInterval *c = &ir->ls.intervals[ci];
      if (c == hint_li || c == blocker)
        continue;
      if (c->r0 == have_reg &&
          c->start < blocker->end && c->end > blocker->start)
      {
        safe = 0;
        break;
      }
      if (c->r0 == wanted_reg &&
          c->start < hint_li->end && c->end > hint_li->start)
      {
        safe = 0;
        break;
      }
    }
    if (!safe)
      continue;

    hint_li->r0 = wanted_reg;
    blocker->r0 = have_reg;
    ir->ls.dirty_registers |= (1ull << wanted_reg) | (1ull << have_reg);

    if (ir->ls.live_regs_by_instruction)
    {
      int lim = ir->ls.live_regs_by_instruction_size;
      uint32_t have_mask = (1u << have_reg);
      uint32_t want_mask = (1u << wanted_reg);
      int lo = (int)(hint_li->start < blocker->start ? hint_li->start : blocker->start);
      int hi = (int)(hint_li->end > blocker->end ? hint_li->end : blocker->end);
      for (int k = lo; k <= hi && k < lim; k++)
      {
        int in_hint = (k >= (int)hint_li->start && k <= (int)hint_li->end);
        int in_blocker = (k >= (int)blocker->start && k <= (int)blocker->end);
        ir->ls.live_regs_by_instruction[k] &= ~(have_mask | want_mask);
        if (in_hint) ir->ls.live_regs_by_instruction[k] |= want_mask;
        if (in_blocker) ir->ls.live_regs_by_instruction[k] |= have_mask;
      }
    }
    break;
  }
}

/* ================================================================== */
/*  Compute final stack layout: min offsets, compact, move coalescing */
/* ================================================================== */
static void compute_stack_layout(TCCIRState *ir, int func_var)
{
  tcc_ls_reset_scratch_cache(&ir->ls);
  tcc_ir_avoid_spilling_stack_passed_params(ir);

  /* Initial min-local-offset scan */
  {
    int min_local_offset = 0;
    (void)0; /* stackoff_count removed — was diagnostic only */
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      const IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      IROperand ops[3];
      ops[0] = tcc_ir_op_get_dest(ir, q);
      ops[1] = tcc_ir_op_get_src1(ir, q);
      ops[2] = tcc_ir_op_get_src2(ir, q);
      for (int j = 0; j < 3; j++)
      {
        if (irop_is_none(ops[j]))
          continue;
        if (irop_get_tag(ops[j]) == IROP_TAG_STACKOFF)
        {
          int32_t off = irop_get_stack_offset(ops[j]);
          if (off < min_local_offset)
            min_local_offset = off;
        }
      }
    }
    if (min_local_offset > loc)
    {
      loc = min_local_offset;
    }
    if (func_var && loc > -28)
      loc = -28;
  }

  tcc_ls_compact_stack_locations(&ir->ls, loc);

  /* Nested-chain detection + live vreg bitmap + min_stack_loc */
  {
    int has_nested_chain = ir->has_static_chain;
    if (!has_nested_chain) {
      for (int j = 0; j < ir->next_instruction_index; j++) {
        int op = ir->compact_instructions[j].op;
        if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT) {
          has_nested_chain = 1;
          break;
        }
      }
    }
    int max_vreg_pos = 0;
    for (int i = 0; i < ir->ls.next_interval_index; ++i) {
      int p = TCCIR_DECODE_VREG_POSITION(ir->ls.intervals[i].vreg);
      if (p > max_vreg_pos) max_vreg_pos = p;
    }
    uint8_t *live_vregs = tcc_mallocz((max_vreg_pos + 8) / 8);
    for (int j = 0; j < ir->next_instruction_index; j++) {
      const IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;
      int32_t vrs[3] = { -1, -1, -1 };
      if (irop_config[q->op].has_dest)
        vrs[0] = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (irop_config[q->op].has_src1)
        vrs[1] = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (irop_config[q->op].has_src2)
        vrs[2] = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
      for (int k = 0; k < 3; k++) {
        if (vrs[k] < 0)
          continue;
        if (TCCIR_DECODE_VREG_TYPE(vrs[k]) == 0)
          continue;
        int p = TCCIR_DECODE_VREG_POSITION(vrs[k]);
        if (p <= max_vreg_pos)
          live_vregs[p / 8] |= (1 << (p % 8));
      }
      if (q->op == TCCIR_OP_MLA || q->op == TCCIR_OP_LOAD_INDEXED ||
          q->op == TCCIR_OP_STORE_INDEXED) {
        int32_t av = irop_get_vreg(tcc_ir_op_get_accum(ir, q));
        if (av >= 0 && TCCIR_DECODE_VREG_TYPE(av) != 0) {
          int p = TCCIR_DECODE_VREG_POSITION(av);
          if (p <= max_vreg_pos)
            live_vregs[p / 8] |= (1 << (p % 8));
        }
      }
    }

    int min_stack_loc = 0;
    for (int i = 0; i < ir->ls.next_interval_index; ++i)
    {
      int sl = ir->ls.intervals[i].stack_location;
      if (sl >= min_stack_loc)
        continue;
      if (ir->ls.intervals[i].r0 < 0 && !has_nested_chain) {
        if (!(live_vregs[TCCIR_DECODE_VREG_POSITION(ir->ls.intervals[i].vreg) / 8] &
              (1 << (TCCIR_DECODE_VREG_POSITION(ir->ls.intervals[i].vreg) % 8)))) {
          ir->ls.intervals[i].stack_location = 0;
          continue;
        }
      }
      min_stack_loc = sl;
    }
    tcc_free(live_vregs);

    /* min_op_offset scan (pre-move-coalescing) */
    int min_op_offset = 0;
    if (!has_nested_chain) {
      for (int j = 0; j < ir->next_instruction_index; j++) {
        const IRQuadCompact *q = &ir->compact_instructions[j];
        if (q->op == TCCIR_OP_NOP)
          continue;
        IROperand ops[3];
        int nops = 0;
        if (irop_config[q->op].has_dest)
          ops[nops++] = tcc_ir_op_get_dest(ir, q);
        if (irop_config[q->op].has_src1)
          ops[nops++] = tcc_ir_op_get_src1(ir, q);
        if (irop_config[q->op].has_src2)
          ops[nops++] = tcc_ir_op_get_src2(ir, q);
        for (int k = 0; k < nops; k++) {
          IROperand *o = &ops[k];
          int has_stackoff = (o->tag == IROP_TAG_STACKOFF) ||
                             (o->is_local || o->is_llocal);
          if (!has_stackoff)
            continue;
          int vr = irop_get_vreg(*o);
          if (vr >= 0) {
            IRLiveInterval *li = tcc_ir_get_live_interval(ir, vr);
            if (li) {
              if (li->allocation.r0 != PREG_NONE &&
                  !(li->allocation.r0 & PREG_SPILLED) &&
                  li->allocation.offset == 0)
                continue; /* vreg is register-only; stack slot unused */
              if (li->allocation.offset != 0) {
                int off = li->allocation.offset + ((int)o->u.imm32 - li->original_offset);
                if (off < min_op_offset)
                  min_op_offset = off;
                continue;
              }
            }
          }
          int off = (int)irop_get_stack_offset(*o);
          if (off < min_op_offset)
            min_op_offset = off;
        }
      }
    } else {
      min_op_offset = loc;
    }

    if (min_op_offset < min_stack_loc)
      min_stack_loc = min_op_offset;

    if (min_stack_loc < loc)
      loc = min_stack_loc;
    else if (!has_nested_chain && min_stack_loc > loc)
      loc = min_stack_loc;
    if (func_var && loc > -28)
      loc = -28;
  }

  tcc_ir_move_coalescing(ir);

  /* Post-move-coalescing min offset scan */
  {
    int post_min_op_offset = 0;
    int post_has_nested_chain = ir->has_static_chain;
    if (!post_has_nested_chain) {
      for (int j = 0; j < ir->next_instruction_index; j++) {
        int op = ir->compact_instructions[j].op;
        if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT) {
          post_has_nested_chain = 1;
          break;
        }
      }
    }
    if (!post_has_nested_chain) {
      for (int j = 0; j < ir->next_instruction_index; j++) {
        const IRQuadCompact *q = &ir->compact_instructions[j];
        if (q->op == TCCIR_OP_NOP)
          continue;
        IROperand ops[3];
        int nops = 0;
        if (irop_config[q->op].has_dest)
          ops[nops++] = tcc_ir_op_get_dest(ir, q);
        if (irop_config[q->op].has_src1)
          ops[nops++] = tcc_ir_op_get_src1(ir, q);
        if (irop_config[q->op].has_src2)
          ops[nops++] = tcc_ir_op_get_src2(ir, q);
        for (int k = 0; k < nops; k++) {
          IROperand *o = &ops[k];
          int has_stackoff = (o->tag == IROP_TAG_STACKOFF) ||
                             (o->is_local || o->is_llocal);
          if (!has_stackoff)
            continue;
          int vr = irop_get_vreg(*o);
          if (vr >= 0) {
            IRLiveInterval *li = tcc_ir_get_live_interval(ir, vr);
            if (li) {
              if (li->allocation.r0 != PREG_NONE &&
                  !(li->allocation.r0 & PREG_SPILLED) &&
                  li->allocation.offset == 0)
                continue; /* register-only vreg; stack slot unused */
              if (li->allocation.offset != 0) {
                int off = li->allocation.offset + ((int)o->u.imm32 - li->original_offset);
                if (off < post_min_op_offset)
                  post_min_op_offset = off;
                continue;
              }
            }
          }
          int off = (int)irop_get_stack_offset(*o);
          if (off < post_min_op_offset)
            post_min_op_offset = off;
        }
      }
      for (int i = 0; i < ir->ls.next_interval_index; ++i) {
        int sl = ir->ls.intervals[i].stack_location;
        if (sl < post_min_op_offset)
          post_min_op_offset = sl;
      }
      if (post_min_op_offset > loc)
        loc = post_min_op_offset;
      if (func_var && loc > -28)
        loc = -28;
    }
  }

  /* Assign stack registers and build final layout */
  for (int i = 0; i < ir->ls.next_interval_index; ++i)
  {
    LSLiveInterval *lsi = &ir->ls.intervals[i];
    tcc_ir_stack_reg_assign(ir, lsi->vreg, lsi->stack_location, lsi->r0, lsi->r1);
    IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, lsi->vreg);
    if (li)
      li->crosses_call = lsi->crosses_call;
  }

  tcc_ir_register_allocation_params(ir);
  tcc_ir_build_stack_layout(ir);
}

/* ================================================================== */
/*  Compile nested functions and update symbol info                   */
/* ================================================================== */
static void finalize_nested_functions(TCCIRState *ir, Sym *sym)
{
  if (tcc_state->nb_nested_funcs > 0)
  {
    for (int i = 0; i < tcc_state->nb_nested_funcs; i++)
    {
      NestedFunc *nf = &tcc_state->nested_funcs[i];
      for (int j = 0; j < nf->nb_captured; j++)
      {
        int vreg = nf->captured_vregs[j];
        if (vreg >= 0)
        {
          IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
          if (interval && interval->allocation.offset != 0)
            nf->captured_offsets[j] = interval->allocation.offset;
        }
      }
    }
    uint8_t saved_need_fp = tcc_state->need_frame_pointer;
    uint8_t saved_force_fp = tcc_state->force_frame_pointer;
    uint8_t saved_force_lr = tcc_state->force_lr_save;
    int can_omit_fp = (tcc_state->nb_nested_funcs > 0);
    for (int i = 0; i < tcc_state->nb_nested_funcs && can_omit_fp; i++) {
      NestedFunc *nf = &tcc_state->nested_funcs[i];
      if (nf->trampoline_needed ||
          !nf->sym || !nf->sym->type.ref ||
          !nf->sym->type.ref->f.func_auto_inline)
        can_omit_fp = 0;
    }
    int needs_fp_for_nested = !can_omit_fp;
    compile_nested_functions(sym);
    tcc_state->force_frame_pointer = saved_force_fp;
    tcc_state->force_lr_save = saved_force_lr;
    tcc_state->need_frame_pointer = needs_fp_for_nested ? tcc_state->need_frame_pointer : saved_need_fp;

    func_ind = ind;
    put_extern_sym(sym, cur_text_section, ind + 1, 0);
  }
}

/* ================================================================== */
/*  Post-allocation: summaries, DCE passes, returnvalue merge, noreturn*/
/* ================================================================== */
static void run_post_alloc_passes(TCCIRState *ir, Sym *sym,
                                   Sym *global_label_stack_start)
{
  if (tcc_state->opt_dead_store)
    tcc_ir_analyze_pure_via_sret(ir, sym);

  if (tcc_state->opt_dead_store)
    tcc_ir_compute_func_write_summary(ir, sym);

  if (tcc_state->opt_dead_store && !tcc_state->ir_late_reopt_phase)
    tcc_ir_collect_tu_func_summary(ir, sym);

  /* Reset label markers */
  {
    Sym *lbl;
    for (lbl = global_label_stack; lbl && lbl != global_label_stack_start; lbl = lbl->prev)
    {
      if (lbl->c == -3)
      {
        lbl->c = 0; /* Reset marker so put_extern_sym2 creates new symbol */
        put_extern_sym2(lbl, cur_text_section->sh_num, 0, 1, 1);
      }
    }
  }

  /* Dead-code elimination passes */
  if (tcc_state->opt_dce)
  {
    if (tcc_ir_opt_noreturn_collapse(ir))
      loc = 0;
    else if (tcc_ir_opt_infinite_self_recursion(ir, sym))
      loc = 0;
    else
      tcc_ir_opt_noreturn_call_epilogue_suppress(ir);
  }

  if (tcc_state->opt_dce) {
    if (tcc_ir_opt_ub_only_body_elide(ir))
      loc = 0;
  }

  if (tcc_state->opt_dce) {
    if (tcc_ir_opt_null_store_dom_return(ir))
      loc = 0;
  }

  if (tcc_state->opt_dce) {
    if (tcc_ir_opt_trap_only_body_suppress(ir))
      loc = 0;
  }

  if (tcc_state->opt_dce) {
    if (tcc_ir_opt_local_only_body_elide(ir))
      loc = 0;
  }

  if (tcc_state->opt_dce) {
    if (tcc_ir_opt_const_return_uninit_elide(ir))
      loc = 0;
  }

  if (tcc_state->opt_dce) {
    if (tcc_ir_opt_useless_function_body(ir))
      loc = 0;
  }

  dbg_scan_imm_dest(ir, "before-returnvalue-merge");
  tcc_ir_opt_returnvalue_merge(ir);
  dbg_scan_imm_dest(ir, "after-returnvalue-merge");

  if (tcc_state->opt_dce && tcc_state->optimize >= 2 && !tcc_state->ir_late_reopt_phase &&
      sym && sym->type.ref && !sym->type.ref->f.func_keep_tokens_for_noreturn)
  {
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
        continue;
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!callee || !callee->type.ref || callee == sym)
        continue;
      if (callee->type.ref->f.func_compiled && !(callee->type.t & VT_INLINE))
        continue;
      sym->type.ref->f.func_keep_tokens_for_noreturn = 1;
      break;
    }
  }

  /* Re-check leaf status after all optimizations */
  if (!ir->leaffunc)
  {
    int still_has_call = 0;
    for (int i = 0; i < ir->next_instruction_index; ++i)
    {
      int op = ir->compact_instructions[i].op;
      if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_BUILTIN_APPLY)
      {
        still_has_call = 1;
        break;
      }
    }
    if (!still_has_call)
      ir->leaffunc = 1;
  }
}

/* ================================================================== */
/*  Backend entry — register allocation + codegen prep                */
/* ================================================================== */
void tcc_ir_backend_regalloc_pipeline(TCCIRState *ir, Sym *sym, int func_var,
                                      unsigned *phase_start, const char *funcname,
                                      Sym *global_label_stack_start)
{
  pop_local_syms(NULL, 0);
  tcc_ir_mark_return_value_incoming_regs(ir);

  int saved_regs_for_alloc = tcc_state->registers_for_allocator;
  setup_register_allocation(ir, func_var);

  run_post_ra_optimizations(ir);
  run_ssa_and_post_ra_passes(ir);
  run_jump_threading_loop(ir);
  compute_min_stack_ref(ir, func_var);
  tcc_state->registers_for_allocator = saved_regs_for_alloc;
  run_register_coalescing(ir);
  compute_stack_layout(ir, func_var);
  finalize_nested_functions(ir, sym);

  if (tcc_state->do_bench)
  {
    unsigned now = tcc_getclock_ms();
    tcc_bench_log_phase(tcc_state, "func-alloc", funcname, &tcc_state->bench_function_alloc_time,
                        &tcc_state->bench_function_alloc_count, now - *phase_start);
    *phase_start = now;
  }

  run_post_alloc_passes(ir, sym, global_label_stack_start);
}
