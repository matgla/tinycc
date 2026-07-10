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
#include "opt/ssa/branch.h"
#include "const_string_fold.h"
#include "bitop_const_fold.h"
#include "opt/ssa/strength.h"
#include "opt/ssa/fold.h"
#include <limits.h>

extern int tcc_ir_opt_pass_disabled(const char *name);

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

void ssa_opt_scan_instr_uses(IRSSAOptCtx *ctx, int i, IRQuadCompact *q)
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
    /* Memory-write STOREs: dest is an address being read.  STORE with a
     * non-lval VREG dest is the IR's value-def encoding (`T = expr`,
     * commonly address materialisation like `T = Addr[StackLoc[N]]`),
     * not a use — skip recording it. */
    int dest_is_use = 1;
    if (q->op == TCCIR_OP_STORE && !d.is_lval)
      dest_is_use = 0;
    if (dest_is_use)
      ssa_opt_record_use(ctx, irop_get_vreg(d), i);
  }
}

static int ssa_opt_is_def_op(int op)
{
  if (!irop_config[op].has_dest)
    return 0;
  if (op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_STORE_POSTINC ||
      op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID)
    return 0;
  /* TCCIR_OP_STORE with non-lval dest is a value def (see
   * ssa_opt_scan_instr_uses); lval-dest STORE is a memory write and is
   * NOT a vreg def.  Caller must additionally check dest.is_lval==0 for
   * STORE — this returns 1 here so the caller's decode path runs. */
  return 1;
}

/* Returns nonzero if `q` definitively defines a vreg via its dest operand. */
static int ssa_opt_quad_defines_value(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!ssa_opt_is_def_op(q->op))
    return 0;
  if (q->op == TCCIR_OP_STORE) {
    IROperand d = tcc_ir_op_get_dest((TCCIRState *)ir, (IRQuadCompact *)q);
    if (d.is_lval)
      return 0;
  }
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

    if (ssa_opt_quad_defines_value(ir, q)) {
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

static int ssa_opt_use_is_barrel_shift_src2(IRSSAOptCtx *ctx, IRSSAUse use,
                                            int32_t old_vr)
{
  if (use.kind != SSA_USE_INSTR)
    return 0;

  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[use.idx];
  if (tcc_ir_barrel_shift_at(ir, q) == 0 || !irop_config[q->op].has_src2)
    return 0;

  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  return irop_get_vreg(src2) == old_vr;
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

int ssa_opt_can_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, old_vr);
  if (!vi)
    return 0;

  for (int i = 0; i < vi->use_count; i++) {
    if (ssa_opt_use_is_barrel_shift_src2(ctx, vi->uses[i], old_vr))
      return 0;
  }
  return 1;
}

int ssa_opt_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr, int32_t new_vr)
{
  if (old_vr == new_vr)
    return 0;
  IRSSAVregInfo *old_vi = ssa_opt_vinfo(ctx, old_vr);
  IRSSAVregInfo *new_vi = ssa_opt_vinfo(ctx, new_vr);
  if (!old_vi)
    return 0;

  /* ARM barrel-shift fusion encodes a hidden shift on an instruction's src2
   * in ir->barrel_shifts[orig_index].  Replacing that src2 with another vreg
   * or an immediate drops the implicit "this operand must be shifted" value
   * identity from SSA's point of view.  Leave such defs in place so codegen
   * still materializes the shift source exactly as fusion recorded it. */
  if (!ssa_opt_can_replace_all_uses(ctx, old_vr))
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
  return ssa_opt_resolve_lea_stackloc_ex(ctx, vr, NULL);
}

/* The address-source operand at a resolution terminal carries the location's
 * identity in its vreg: irop_get_vreg(src) is -1 for a real direct stack slot
 * (vreg_type == 0, offset authoritative) and the VAR/PARAM vreg for a `&VAR`
 * spill-encoded address (offset is a shared placeholder).  Report it so callers
 * can tell distinct address-taken locals apart at SSA time. */
int ssa_opt_resolve_lea_stackloc_ex(IRSSAOptCtx *ctx, int32_t vr, int32_t *out_base_var)
{
  TCCIRState *ir = ctx->ir;
  int acc = 0;
  if (out_base_var)
    *out_base_var = -1;
  /* Bound on chain length; chains longer than this (e.g. degenerate va_arg
   * pointer arithmetic) bail to INT_MIN.  Without a cap the recursive form
   * blew the host stack on pathological inputs. */
  for (int hop = 0; hop < 64; hop++) {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return INT_MIN;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_instr < 0 || vi->def_count > 1)
      return INT_MIN;
    IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];

    if (dq->op == TCCIR_OP_LEA) {
      IROperand src = tcc_ir_op_get_src1(ir, dq);
      if (src.tag == IROP_TAG_STACKOFF || src.is_local) {
        if (out_base_var)
          *out_base_var = irop_get_vreg(src);
        return irop_get_stack_offset(src) + acc;
      }
      return INT_MIN;
    }

    if (dq->op == TCCIR_OP_ASSIGN) {
      IROperand src = tcc_ir_op_get_src1(ir, dq);
      if (src.tag == IROP_TAG_STACKOFF && !src.is_lval) {
        if (out_base_var)
          *out_base_var = irop_get_vreg(src);
        return irop_get_stack_offset(src) + acc;
      }
      int32_t sv = irop_get_vreg(src);
      if (sv >= 0 && !src.is_lval) {
        vr = sv;
        continue;
      }
      return INT_MIN;
    }

    /* `T <-- Addr[StackLoc[N]] [STORE]` is the frontend's encoding for
     * address materialisation into a TEMP (vstore through a non-lval dest).
     * Semantically identical to LEA / ASSIGN(stack-addr). */
    if (dq->op == TCCIR_OP_STORE) {
      IROperand dest = tcc_ir_op_get_dest(ir, dq);
      if (!dest.is_lval) {
        IROperand src = tcc_ir_op_get_src1(ir, dq);
        if (src.tag == IROP_TAG_STACKOFF && !src.is_lval) {
          if (out_base_var)
            *out_base_var = irop_get_vreg(src);
          return irop_get_stack_offset(src) + acc;
        }
        int32_t sv = irop_get_vreg(src);
        if (sv >= 0 && !src.is_lval) {
          vr = sv;
          continue;
        }
      }
      return INT_MIN;
    }

    /* T = base + imm where base resolves to LEA(StackLoc[N]).  Common pattern
     * for struct field address: T46 = T45 + 4 with T45 = &StackLoc[-196]. */
    if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB) {
      IROperand src1 = tcc_ir_op_get_src1(ir, dq);
      IROperand src2 = tcc_ir_op_get_src2(ir, dq);
      if (!src1.is_lval && irop_is_immediate(src2)) {
        int32_t s1vr = irop_get_vreg(src1);
        if (s1vr >= 0) {
          int delta = irop_get_imm32(src2);
          acc += (dq->op == TCCIR_OP_ADD) ? delta : -delta;
          vr = s1vr;
          continue;
        }
      }
      return INT_MIN;
    }

    return INT_MIN;
  }
  return INT_MIN;
}

/* Resolve `vr` backward to a canonical (base_vr, offset) form.  See
 * ssa_opt.h for the contract.
 *
 * Walks ASSIGN/ADD chains within the function, with a hop limit to prevent
 * pathological pointer-cycles from being expensive.  Stops as soon as the
 * current vreg's defining op is something we can't fold into an offset
 * (anything other than ASSIGN of another vreg or ADD with an immediate).
 *
 * VAR/PARAM vregs are terminals — they represent the "root" address whose
 * value is the canonical base.  Multi-def TEMPs and definitions outside
 * the function bail to prevent unsound forwarding.
 *
 * Accepts both VAR-read encodings on source operands: VREG-tagged (V's
 * register form, is_lval=0) and STACKOFF-tagged (V's slot form,
 * is_lval=1 + is_local=1).  Both produce the same address value. */
int ssa_opt_resolve_temp_to_base_off(IRSSAOptCtx *ctx, int32_t vr,
                                      int32_t *out_base, int32_t *out_off)
{
  *out_off = 0;
  for (int hop = 0; hop < 8; hop++) {
    if (vr < 0)
      return 0;
    int type = TCCIR_DECODE_VREG_TYPE(vr);
    if (type == TCCIR_VREG_TYPE_VAR || type == TCCIR_VREG_TYPE_PARAM) {
      *out_base = vr;
      return 1;
    }
    if (type != TCCIR_VREG_TYPE_TEMP)
      return 0;

    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_count > 1 || vi->def_instr < 0)
      return 0;
    IRQuadCompact *dq = &ctx->ir->compact_instructions[vi->def_instr];

    if (dq->op == TCCIR_OP_ASSIGN) {
      IROperand src = tcc_ir_op_get_src1(ctx->ir, dq);
      int32_t sv = irop_get_vreg(src);
      if (sv < 0)
        return 0;
      int svt = TCCIR_DECODE_VREG_TYPE(sv);
      /* TEMP-to-TEMP plain copy: keep chasing. */
      if (svt == TCCIR_VREG_TYPE_TEMP && !src.is_lval && !src.is_local &&
          !src.is_llocal && src.tag == IROP_TAG_VREG) {
        vr = sv;
        continue;
      }
      /* VAR/PARAM read, both encodings: register form (VREG/!lval) or
       * slot form (STACKOFF/lval+local).  Either way the value loaded
       * is V's current address-bearing content. */
      if (svt == TCCIR_VREG_TYPE_VAR || svt == TCCIR_VREG_TYPE_PARAM) {
        int reg_form = (src.tag == IROP_TAG_VREG && !src.is_lval &&
                        !src.is_local && !src.is_llocal);
        int slot_form = (src.tag == IROP_TAG_STACKOFF && src.is_lval &&
                          src.is_local && !src.is_llocal);
        if (reg_form || slot_form) {
          *out_base = sv;
          return 1;
        }
      }
      return 0;
    }

    if (dq->op == TCCIR_OP_ADD) {
      IROperand src1 = tcc_ir_op_get_src1(ctx->ir, dq);
      IROperand src2 = tcc_ir_op_get_src2(ctx->ir, dq);
      if (!irop_is_immediate(src2) || src2.is_lval)
        return 0;
      if (src1.is_lval)
        return 0;
      int32_t s1vr = irop_get_vreg(src1);
      if (s1vr < 0)
        return 0;
      *out_off += irop_get_imm32(src2);
      vr = s1vr;
      continue;
    }

    /* Other defining op (LOAD, MLA, CALL, ...): treat this TEMP as the
     * canonical root itself.  Two reads through it would still share if
     * the TEMP is the same vreg (existing TVStore path). */
    *out_base = vr;
    return 1;
  }
  return 0;
}

int ssa_opt_indirect_stack_offset(IRSSAOptCtx *ctx, const IRQuadCompact *q, int side)
{
  return ssa_opt_indirect_stack_offset_ex(ctx, q, side, NULL);
}

int ssa_opt_indirect_stack_offset_ex(IRSSAOptCtx *ctx, const IRQuadCompact *q, int side,
                                     int32_t *out_base_var)
{
  TCCIRState *ir = ctx->ir;
  IROperand base;
  int has_index = 0;
  int require_lval = 0;
  IROperand idx = IROP_NONE, scale = IROP_NONE;

  if (out_base_var)
    *out_base_var = -1;

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
  int base_off = ssa_opt_resolve_lea_stackloc_ex(ctx, bvr, out_base_var);
  if (base_off == INT_MIN) {
    if (out_base_var)
      *out_base_var = -1;
    return INT_MIN;
  }
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

void dbg_scan_imm_dest(TCCIRState *ir, const char *pass);
int tcc_ir_ssa_opt_run(IRSSAOptCtx *ctx)
{
  int total = 0;
  int iteration = 0;
  const int max_iterations = 5;
  int changes;

  /* Run one SSA pass, accumulate its change count, then make it observable:
   * dbg_scan_imm_dest() for the SCAN_IMM_DEST bug hunt and
   * tcc_ir_dump_after_pass() for -dump-ir-passes=<name> golden snapshots
   * (mirrors the legacy RUN_PASS macro in tccgen.c). */
#define SSA_RUN(name, call)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!tcc_ir_opt_pass_disabled(name))                                                                               \
      changes += (call);                                                                                               \
    dbg_scan_imm_dest(ctx->ir, name);                                                                                  \
    tcc_ir_dump_after_pass(ctx->ir, name);                                                                             \
  } while (0)

  do {
    changes = 0;
    iteration++;

    /* target-independent passes */
    SSA_RUN("ssa:var_const_fold", ssa_opt_var_const_fold(ctx));
    SSA_RUN("ssa:sccp", ssa_opt_sccp(ctx));
    SSA_RUN("ssa:cprop", ssa_opt_cprop(ctx));
    /* Collapse `V <- val [STORE]; ... PARAM V` into `... PARAM val` when V
     * has a single def and that lone PARAM as its only use.  Catches the
     * inlined-check1 pattern that spills printf args into VARs ahead of
     * the conditional branch even when only the FAIL path reads them. */
    SSA_RUN("ssa:var_to_param_forward", ssa_opt_var_to_param_forward(ctx));
    SSA_RUN("ssa:fold", ssa_opt_fold(ctx));
    SSA_RUN("ssa:cprop", ssa_opt_cprop(ctx));
    SSA_RUN("ssa:var_imm_prop", ssa_opt_var_imm_prop(ctx));
    SSA_RUN("ssa:const_prop_tmp", ssa_opt_const_prop_tmp(ctx));
    SSA_RUN("ssa:load_cse", ssa_opt_load_cse(ctx));
    SSA_RUN("ssa:const_string_fold", tcc_ir_ssa_opt_const_string_fold(ctx));
    SSA_RUN("ssa:bitop_const_fold", tcc_ir_ssa_opt_bitop_const_fold(ctx));
    SSA_RUN("ssa:ptr_store_dse", tcc_ir_ssa_opt_ptr_store_dse(ctx));
    SSA_RUN("ssa:branch", ssa_opt_branch(ctx));
    SSA_RUN("ssa:cmp_eq_prop", ssa_opt_cmp_eq_prop(ctx));
    SSA_RUN("ssa:reassoc", ssa_opt_reassoc(ctx));
    SSA_RUN("ssa:strength", ssa_opt_strength(ctx));
    SSA_RUN("ssa:narrow", ssa_opt_narrow(ctx));
    SSA_RUN("ssa:gvn", ssa_opt_gvn(ctx));
    SSA_RUN("ssa:phi_simplify", ssa_opt_phi_simplify(ctx));
    /* Dead-loop post-phi rewrite: collapse a side-effect-free counting loop
     * whose result is a loop-invariant constant into a guarded constant
     * (`(trip>0) ? body_const : init`).  This is an -O2 optimization — GCC
     * likewise keeps the empty loop at -O1 and only elides it at -O2 — so
     * gate it to keep -O1 a lighter tier. */
    SSA_RUN("ssa:dead_loop",
            (tcc_state && tcc_state->optimize >= 2) ? ssa_opt_dead_loop(ctx) : 0);
    SSA_RUN("ssa:dce", ssa_opt_dce(ctx));

    /* target-specific generators (registered by backend) */
    if (target_gens && target_gen_count > 0)
      changes += ssa_opt_run_gens(ctx, target_gens, target_gen_count);

    total += changes;
  } while (changes > 0 && iteration < max_iterations);
#undef SSA_RUN

  total += tcc_ir_ssa_opt_guard_collapse(ctx);

  return total;
}

/* Straight-line overwritten-global-store elimination: a STORE to (sym,addend)
 * with no possible read, call, control transfer, or live jump target between
 * it and a later same-slot STORE is dead.  Jump targets are recomputed from
 * live JUMP/JUMPIFs — the is_jump_target flags persist on folded guards'
 * merge points and would reset tracking at every former section boundary. */
typedef struct GSDPend { Sym *sym; int64_t addend; int idx; } GSDPend;

/* Memory read via an lval operand: symref reads invalidate that symbol's
 * pending stores only, stack reads alias no global, anything else (pointer
 * deref) invalidates everything. */
static void gsd_read_reset(TCCIRState *ir, IROperand op, GSDPend *pend, int *np)
{
  if (!op.is_lval)
    return;
  if (op.is_sym) {
    IRPoolSymref *ref = irop_get_symref_ex(ir, op);
    if (ref && ref->sym) {
      for (int k = 0; k < *np;) {
        if (pend[k].sym == ref->sym)
          pend[k] = pend[--(*np)];
        else
          k++;
      }
      return;
    }
    *np = 0;
    return;
  }
  if (op.tag == IROP_TAG_STACKOFF || op.is_local || op.is_llocal)
    return;
  *np = 0;
}

static int ssa_opt_global_store_dse(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n <= 0)
    return 0;
  enum { GSD_CAP = 16 };
  GSDPend pend[GSD_CAP];
  int np = 0;
  int changes = 0;

  uint8_t *is_target = tcc_mallocz((size_t)(n + 7) / 8);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_INLINE_ASM) {
      /* Targets we can't enumerate — a kill across one could be observed. */
      tcc_free(is_target);
      return 0;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (t >= 0 && t < n)
        is_target[t / 8] |= (uint8_t)(1 << (t % 8));
    }
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (is_target[i / 8] & (1 << (i % 8)))
      np = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_STORE) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (dest.is_sym && dest.is_lval && !src.is_lval &&
          irop_get_btype(dest) == IROP_BTYPE_INT32) {
        IRPoolSymref *ref = irop_get_symref_ex(ir, dest);
        if (ref && ref->sym && !(ref->sym->type.t & VT_VOLATILE)) {
          for (int k = 0; k < np; k++) {
            if (pend[k].sym == ref->sym && pend[k].addend == ref->addend) {
              ssa_opt_nop_instr(ctx, pend[k].idx);
              changes++;
              pend[k] = pend[--np];
              break;
            }
          }
          if (np < GSD_CAP) {
            pend[np].sym = ref->sym;
            pend[np].addend = ref->addend;
            pend[np].idx = i;
            np++;
          }
          continue;
        }
      }
      np = 0;
      continue;
    }

    switch (q->op) {
    case TCCIR_OP_FUNCCALLVAL: case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_IJUMP: case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_BLOCK_COPY: case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_STORE_INDEXED: case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_LOAD_POSTINC:
    case TCCIR_OP_JUMP: case TCCIR_OP_JUMPIF:
    case TCCIR_OP_RETURNVALUE: case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      np = 0;
      continue;
    default:
      break;
    }
    if (irop_config[q->op].has_src1)
      gsd_read_reset(ir, tcc_ir_op_get_src1(ir, q), pend, &np);
    if (irop_config[q->op].has_src2)
      gsd_read_reset(ir, tcc_ir_op_get_src2(ir, q), pend, &np);
    if (q->op == TCCIR_OP_MLA)
      gsd_read_reset(ir, tcc_ir_op_get_accum(ir, q), pend, &np);
  }
  tcc_free(is_target);
  return changes;
}

/* TEMP-deref store-store DSE: a full-width `*T <- a [STORE]` followed by
 * another same-width `*T <- b [STORE]` with no possible memory read, call,
 * opaque op, control transfer, or live jump target between them is dead.
 * T is a single-def TEMP, so both derefs name the same address; intervening
 * pure writes don't observe the pending value and don't reset tracking.
 * Same model as the legacy flat ptr_store_load_fwd DSE it replaces. */
typedef struct PSDPend { int32_t ptr_vr; int btype; int idx; } PSDPend;

int tcc_ir_ssa_opt_ptr_store_dse(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n <= 0)
    return 0;
  enum { PSD_CAP = 8 };
  PSDPend pend[PSD_CAP];
  int np = 0;
  int changes = 0;

  uint8_t *is_target = tcc_mallocz((size_t)(n + 7) / 8);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_INLINE_ASM) {
      tcc_free(is_target);
      return 0;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (t >= 0 && t < n)
        is_target[t / 8] |= (uint8_t)(1 << (t % 8));
    }
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (is_target[i / 8] & (1 << (i % 8)))
      np = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (src.is_lval) {
        np = 0;
        continue;
      }
      int32_t pvr = irop_get_vreg(dest);
      int btype = irop_get_btype(dest);
      if (q->op == TCCIR_OP_STORE && dest.is_lval && dest.tag == IROP_TAG_VREG &&
          !dest.is_local && !dest.is_llocal && !dest.is_sym && pvr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(pvr) == TCCIR_VREG_TYPE_TEMP &&
          (btype == IROP_BTYPE_INT32 || btype == IROP_BTYPE_FLOAT32)) {
        IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, pvr);
        if (pvi && pvi->def_count == 1) {
          for (int k = 0; k < np; k++) {
            if (pend[k].ptr_vr == pvr) {
              if (pend[k].btype == btype) {
                ssa_opt_nop_instr(ctx, pend[k].idx);
                changes++;
              }
              pend[k] = pend[--np];
              break;
            }
          }
          if (np < PSD_CAP) {
            pend[np].ptr_vr = pvr;
            pend[np].btype = btype;
            pend[np].idx = i;
            np++;
          }
        }
      }
      continue;
    }

    switch (q->op) {
    case TCCIR_OP_FUNCCALLVAL: case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_BLOCK_COPY: case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_LOAD_POSTINC: case TCCIR_OP_LOAD:
    case TCCIR_OP_LOAD_INDEXED: case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_JUMP: case TCCIR_OP_JUMPIF:
    case TCCIR_OP_RETURNVALUE: case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      np = 0;
      continue;
    default:
      break;
    }
    if ((irop_config[q->op].has_src1 && tcc_ir_op_get_src1(ir, q).is_lval) ||
        (irop_config[q->op].has_src2 && tcc_ir_op_get_src2(ir, q).is_lval) ||
        (q->op == TCCIR_OP_MLA && tcc_ir_op_get_accum(ir, q).is_lval))
      np = 0;
  }
  tcc_free(is_target);
  return changes;
}

/* Sequential const-guard chains (store; cmp; jumpif; call abort; load ...)
 * fold at most one guard per pipeline round: the dead call must be removed
 * before the next store→load forwards.  Loop the cheap linear subset until
 * branch folding runs dry — bounded by the JUMPIF count since folded
 * branches are never recreated.  Restores the legacy flat branch_fold
 * fixpoint (gcc-torture 20040629-1 family). */
int tcc_ir_ssa_opt_guard_collapse(IRSSAOptCtx *ctx)
{
  if (tcc_ir_opt_pass_disabled("ssa:guard_collapse"))
    return 0;
  int total = 0;
  for (int guard = 0; guard < 4096; guard++) {
    total += ssa_opt_load_cse(ctx);
    total += ssa_opt_cprop(ctx);
    total += ssa_opt_fold(ctx);
    int br = ssa_opt_branch(ctx);
    if (!br)
      break;
    total += br + ssa_opt_dce_light(ctx);
  }
  if (total) {
    TCCIRState *ir = ctx->ir;
    for (int round = 0; round < 8; round++) {
      int c = 0;
      /* Retarget jumps pointing at NOP'd instructions to the next live one —
       * eliminate_fallthrough compares raw indices and would keep them. */
      int n = ir->next_instruction_index;
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int t = (int)irop_get_imm64_ex(ir, d);
        int nt = t;
        while (nt >= 0 && nt < n && ir->compact_instructions[nt].op == TCCIR_OP_NOP)
          nt++;
        if (nt != t && nt >= 0) {
          tcc_ir_set_dest(ir, i, irop_make_imm32(0, nt, irop_get_btype(d)));
          c++;
        }
      }
      c += tcc_ir_opt_eliminate_fallthrough(ir);
      c += ssa_opt_global_store_dse(ctx);
      total += c;
      if (!c)
        break;
    }
    total += ssa_opt_dce(ctx);
  }
  tcc_ir_dump_after_pass(ctx->ir, "ssa:guard_collapse");
  return total;
}

int tcc_ir_ssa_opt_run_target(IRSSAOptCtx *ctx)
{
  if (!target_gens || target_gen_count <= 0)
    return 0;
  int total = 0;
  for (int iter = 0; iter < 3; iter++) {
    int changes = ssa_opt_run_gens(ctx, target_gens, target_gen_count);
    if (changes == 0)
      break;
    total += changes;
    /* DCE removes instructions we NOP'd; rerun cprop to clean up new copies. */
    ssa_opt_cprop(ctx);
    ssa_opt_dce(ctx);
  }
  return total;
}
