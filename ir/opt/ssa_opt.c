/*
 *  TCC IR - SSA Optimization Engine: Driver + Use-Def Chains
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include <limits.h>

/* ============================================================================
 * Target-Specific Generator Registration
 * ============================================================================ */

static const IRSSAOptGen *target_gens;
static int target_gen_count;

void tcc_ir_ssa_opt_register_target(const IRSSAOptGen *gens, int count)
{
  target_gens = gens;
  target_gen_count = count;
}

/* ============================================================================
 * Use-Def Chain Internals
 * ============================================================================ */

IRSSAVregInfo *ssa_opt_vinfo(IRSSAOptCtx *ctx, int32_t vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP)
    return NULL;
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (pos >= ctx->vinfo_cap)
    return NULL;
  return &ctx->vinfo[pos];
}

void ssa_opt_add_use_instr(IRSSAVregInfo *vi, int instr_idx)
{
  if (vi->use_count >= vi->use_cap) {
    int nc = vi->use_cap ? vi->use_cap * 2 : 4;
    vi->uses = tcc_realloc(vi->uses, nc * sizeof(IRSSAUse));
    vi->use_cap = nc;
  }
  vi->uses[vi->use_count++] = (IRSSAUse){ .idx = instr_idx, .kind = SSA_USE_INSTR };
}

void ssa_opt_add_use_phi(IRSSAVregInfo *vi, int block, int slot)
{
  if (vi->use_count >= vi->use_cap) {
    int nc = vi->use_cap ? vi->use_cap * 2 : 4;
    vi->uses = tcc_realloc(vi->uses, nc * sizeof(IRSSAUse));
    vi->use_cap = nc;
  }
  vi->uses[vi->use_count++] = (IRSSAUse){ .idx = block, .slot = slot, .kind = SSA_USE_PHI };
}

void ssa_opt_remove_use_instr(IRSSAVregInfo *vi, int instr_idx)
{
  for (int i = 0; i < vi->use_count; i++) {
    if (vi->uses[i].kind == SSA_USE_INSTR && vi->uses[i].idx == instr_idx) {
      vi->uses[i] = vi->uses[--vi->use_count];
      return;
    }
  }
}

static void ssa_opt_record_use(IRSSAOptCtx *ctx, int32_t vreg, int instr_idx)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vreg);
  if (vi)
    ssa_opt_add_use_instr(vi, instr_idx);
}

static void ssa_opt_scan_instr_uses(IRSSAOptCtx *ctx, int i, IRQuadCompact *q)
{
  TCCIRState *ir = ctx->ir;

  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    ssa_opt_record_use(ctx, irop_get_vreg(s), i);
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    ssa_opt_record_use(ctx, irop_get_vreg(s), i);
  }
  if (q->op == TCCIR_OP_MLA) {
    IROperand a = tcc_ir_op_get_accum(ir, q);
    ssa_opt_record_use(ctx, irop_get_vreg(a), i);
  }
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_STORE_POSTINC) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    ssa_opt_record_use(ctx, irop_get_vreg(d), i);
  }
}

static int ssa_opt_is_def_op(int op)
{
  if (!irop_config[op].has_dest)
    return 0;
  if (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED ||
      op == TCCIR_OP_STORE_POSTINC || op == TCCIR_OP_FUNCPARAMVAL ||
      op == TCCIR_OP_FUNCPARAMVOID)
    return 0;
  return 1;
}

/* ============================================================================
 * Init / Rebuild / Free
 * ============================================================================ */

static void ssa_opt_build_chains(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRSSAState *ssa = ctx->ssa;

  for (int i = 0; i < ctx->vinfo_cap; i++) {
    ctx->vinfo[i].def_instr = -1;
    ctx->vinfo[i].def_phi_block = -1;
    ctx->vinfo[i].def_count = 0;
    ctx->vinfo[i].use_count = 0;
  }

  /* phi definitions */
  for (int b = 0; b < cfg->num_blocks; b++) {
    for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->dest_vreg);
      if (vi)
        vi->def_phi_block = b;
    }
  }

  /* instruction definitions + uses */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (ssa_opt_is_def_op(q->op)) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(d));
      if (vi) {
        vi->def_instr = i;
        vi->def_count++;
      }
    }

    ssa_opt_scan_instr_uses(ctx, i, q);
  }

  /* phi operand uses */
  for (int b = 0; b < cfg->num_blocks; b++) {
    for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
      for (int pi = 0; pi < phi->num_operands; pi++) {
        IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->operands[pi].vreg);
        if (vi)
          ssa_opt_add_use_phi(vi, b, pi);
      }
    }
  }
}

void tcc_ir_ssa_opt_init(IRSSAOptCtx *ctx, TCCIRState *ir,
                         IRSSAState *ssa, IRCFG *cfg)
{
  memset(ctx, 0, sizeof(*ctx));
  ctx->ir = ir;
  ctx->ssa = ssa;
  ctx->cfg = cfg;
  ctx->vinfo_cap = ir->next_temporary_variable;
  if (ctx->vinfo_cap <= 0)
    ctx->vinfo_cap = 1;
  ctx->vinfo = tcc_mallocz(ctx->vinfo_cap * sizeof(IRSSAVregInfo));
  ssa_opt_build_chains(ctx);
}

void tcc_ir_ssa_opt_rebuild(IRSSAOptCtx *ctx)
{
  for (int i = 0; i < ctx->vinfo_cap; i++) {
    tcc_free(ctx->vinfo[i].uses);
    ctx->vinfo[i].uses = NULL;
    ctx->vinfo[i].use_count = 0;
    ctx->vinfo[i].use_cap = 0;
  }

  int new_cap = ctx->ir->next_temporary_variable;
  if (new_cap > ctx->vinfo_cap) {
    ctx->vinfo = tcc_realloc(ctx->vinfo, new_cap * sizeof(IRSSAVregInfo));
    memset(&ctx->vinfo[ctx->vinfo_cap], 0,
           (new_cap - ctx->vinfo_cap) * sizeof(IRSSAVregInfo));
    ctx->vinfo_cap = new_cap;
  }

  ssa_opt_build_chains(ctx);
}

void tcc_ir_ssa_opt_free(IRSSAOptCtx *ctx)
{
  if (!ctx->vinfo)
    return;
  for (int i = 0; i < ctx->vinfo_cap; i++)
    tcc_free(ctx->vinfo[i].uses);
  tcc_free(ctx->vinfo);
  ctx->vinfo = NULL;
}

/* ============================================================================
 * Helpers
 * ============================================================================ */

int ssa_opt_has_side_effects(int op)
{
  switch (op) {
  case TCCIR_OP_STORE:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_CALLSEQ_BEGIN:
  case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_CALLARG_REG:
  case TCCIR_OP_CALLARG_STACK:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_SET_CHAIN:
  case TCCIR_OP_INIT_CHAIN_SLOT:
  case TCCIR_OP_BUILTIN_APPLY_ARGS:
  case TCCIR_OP_BUILTIN_APPLY:
  case TCCIR_OP_BUILTIN_RETURN:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_PREFETCH:
    return 1;
  default:
    return 0;
  }
}

void ssa_opt_nop_instr(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op == TCCIR_OP_NOP)
    return;

  /* Decrement use counts for operands */
  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(s));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(s));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (q->op == TCCIR_OP_MLA) {
    IROperand a = tcc_ir_op_get_accum(ir, q);
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(a));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_STORE_POSTINC) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(d));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }

  q->op = TCCIR_OP_NOP;
}

static void ssa_opt_rewrite_operand(IRSSAOptCtx *ctx, int instr_idx,
                                    int32_t old_vr, int32_t new_vr)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];

  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (irop_get_vreg(s) == old_vr) {
      irop_set_vreg(&s, new_vr);
      tcc_ir_op_set_src1(ir, q, s);
    }
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (irop_get_vreg(s) == old_vr) {
      irop_set_vreg(&s, new_vr);
      tcc_ir_op_set_src2(ir, q, s);
    }
  }
  if (q->op == TCCIR_OP_MLA) {
    IROperand a = tcc_ir_op_get_accum(ir, q);
    if (irop_get_vreg(a) == old_vr) {
      irop_set_vreg(&a, new_vr);
      tcc_ir_op_set_accum(ir, q, a);
    }
  }
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_STORE_POSTINC) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(d) == old_vr) {
      irop_set_vreg(&d, new_vr);
      tcc_ir_op_set_dest(ir, q, d);
    }
  }
}

static void ssa_opt_rewrite_phi_operand(IRSSAOptCtx *ctx, int block,
                                        int slot, int32_t old_vr,
                                        int32_t new_vr)
{
  IRSSAState *ssa = ctx->ssa;
  for (IRPhiNode *phi = ssa->block_phis[block]; phi; phi = phi->next) {
    if (slot < phi->num_operands && phi->operands[slot].vreg == old_vr) {
      phi->operands[slot].vreg = new_vr;
      return;
    }
  }
}

int ssa_opt_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr, int32_t new_vr)
{
  if (old_vr == new_vr)
    return 0;
  IRSSAVregInfo *old_vi = ssa_opt_vinfo(ctx, old_vr);
  IRSSAVregInfo *new_vi = ssa_opt_vinfo(ctx, new_vr);
  if (!old_vi)
    return 0;

  int count = 0;
  while (old_vi->use_count > 0) {
    IRSSAUse use = old_vi->uses[--old_vi->use_count];

    if (use.kind == SSA_USE_INSTR)
      ssa_opt_rewrite_operand(ctx, use.idx, old_vr, new_vr);
    else
      ssa_opt_rewrite_phi_operand(ctx, use.idx, use.slot, old_vr, new_vr);

    if (new_vi) {
      if (use.kind == SSA_USE_INSTR)
        ssa_opt_add_use_instr(new_vi, use.idx);
      else
        ssa_opt_add_use_phi(new_vi, use.idx, use.slot);
    }
    count++;
  }

  return count;
}

/* ============================================================================
 * LEA Resolution Helpers (shared by load_cse + sccp)
 * ============================================================================ */

int ssa_opt_resolve_lea_stackloc(IRSSAOptCtx *ctx, int32_t vr)
{
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return INT_MIN;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_instr < 0 || vi->def_count > 1)
    return INT_MIN;
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  if (dq->op == TCCIR_OP_LEA) {
    IROperand src = tcc_ir_op_get_src1(ir, dq);
    if (src.tag == IROP_TAG_STACKOFF || src.is_local)
      return irop_get_stack_offset(src);
  }
  if (dq->op == TCCIR_OP_ASSIGN) {
    IROperand src = tcc_ir_op_get_src1(ir, dq);
    if (src.tag == IROP_TAG_STACKOFF && !src.is_lval)
      return irop_get_stack_offset(src);
    int32_t sv = irop_get_vreg(src);
    if (sv >= 0 && !src.is_lval)
      return ssa_opt_resolve_lea_stackloc(ctx, sv);
  }
  /* T = base + imm where base resolves to LEA(StackLoc[N]).  Common pattern
   * for struct field address: T46 = T45 + 4 with T45 = &StackLoc[-196]. */
  if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB) {
    IROperand src1 = tcc_ir_op_get_src1(ir, dq);
    IROperand src2 = tcc_ir_op_get_src2(ir, dq);
    if (!src1.is_lval && irop_is_immediate(src2)) {
      int32_t s1vr = irop_get_vreg(src1);
      if (s1vr >= 0) {
        int base_off = ssa_opt_resolve_lea_stackloc(ctx, s1vr);
        if (base_off != INT_MIN) {
          int delta = irop_get_imm32(src2);
          return dq->op == TCCIR_OP_ADD ? base_off + delta : base_off - delta;
        }
      }
    }
  }
  return INT_MIN;
}

int ssa_opt_indirect_stack_offset(IRSSAOptCtx *ctx, const IRQuadCompact *q, int side)
{
  TCCIRState *ir = ctx->ir;
  IROperand base;
  int has_index = 0;
  int require_lval = 0;
  IROperand idx = IROP_NONE, scale = IROP_NONE;

  if (side == SSA_OPT_INDIRECT_DEST) {
    base = tcc_ir_op_get_dest(ir, q);
    if (q->op == TCCIR_OP_STORE_INDEXED) {
      has_index = 1;
      idx = tcc_ir_op_get_src2(ir, q);
      scale = tcc_ir_op_get_scale(ir, q);
    } else if (q->op == TCCIR_OP_STORE) {
      require_lval = 1; /* plain *T = val: T must be deref'd */
    } else {
      return INT_MIN;
    }
  } else {
    base = tcc_ir_op_get_src1(ir, q);
    if (q->op == TCCIR_OP_LOAD_INDEXED) {
      has_index = 1;
      idx = tcc_ir_op_get_src2(ir, q);
      scale = tcc_ir_op_get_scale(ir, q);
    } else if (q->op == TCCIR_OP_LOAD) {
      require_lval = 1;
    } else {
      return INT_MIN;
    }
  }

  if (base.tag != IROP_TAG_VREG || base.is_local)
    return INT_MIN;
  if (require_lval && !base.is_lval)
    return INT_MIN;
  int32_t bvr = irop_get_vreg(base);
  if (bvr < 0 || TCCIR_DECODE_VREG_TYPE(bvr) != TCCIR_VREG_TYPE_TEMP)
    return INT_MIN;
  int base_off = ssa_opt_resolve_lea_stackloc(ctx, bvr);
  if (base_off == INT_MIN)
    return INT_MIN;
  if (!has_index)
    return base_off;
  if (!irop_is_immediate(idx) || !irop_is_immediate(scale))
    return INT_MIN;
  if (irop_get_imm32(scale) != 0)
    return INT_MIN;
  return base_off + irop_get_imm32(idx);
}

/* ============================================================================
 * Generator Driver
 * ============================================================================ */

int ssa_opt_run_gens(IRSSAOptCtx *ctx, const IRSSAOptGen *gens, int count)
{
  TCCIRState *ir = ctx->ir;
  int changes = 0;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_NOP)
      continue;
    for (int g = 0; g < count; g++) {
      if (gens[g].op == op) {
        changes += gens[g].fn(ctx, i);
        break;
      }
    }
  }

  return changes;
}

/* ============================================================================
 * Main Driver
 * ============================================================================ */

int tcc_ir_ssa_opt_run(IRSSAOptCtx *ctx)
{
  int total = 0;
  int iteration = 0;
  const int max_iterations = 5;
  int changes;

  do {
    changes = 0;
    iteration++;

    /* target-independent passes */
    changes += ssa_opt_var_const_fold(ctx);
    changes += ssa_opt_sccp(ctx);
    changes += ssa_opt_cprop(ctx);
    changes += ssa_opt_fold(ctx);
    changes += ssa_opt_load_cse(ctx);
    changes += ssa_opt_branch(ctx);
    changes += ssa_opt_reassoc(ctx);
    changes += ssa_opt_strength(ctx);
    changes += ssa_opt_narrow(ctx);
    changes += ssa_opt_gvn(ctx);
    changes += ssa_opt_phi_simplify(ctx);
    changes += ssa_opt_dead_loop(ctx);
    changes += ssa_opt_dce(ctx);

    /* target-specific generators (registered by backend) */
    if (target_gens && target_gen_count > 0)
      changes += ssa_opt_run_gens(ctx, target_gens, target_gen_count);

    total += changes;
  } while (changes > 0 && iteration < max_iterations);

  return total;
}
