/*
 *  TCC IR - useless-body / dead-before-infinite-loop / local-only-body elision
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_xform.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "cfg.h"
#include "licm.h"

static int ir_opt_pure_call_id_test(const uint8_t *pure_call_ids, int pure_call_id_bytes, int call_id)
{
  return call_id >= 0 && call_id / 8 < pure_call_id_bytes &&
         (pure_call_ids[call_id / 8] & (uint8_t)(1u << (call_id & 7)));
}

static void ir_opt_pure_call_id_mark(uint8_t **pure_call_ids, int *pure_call_id_bytes, int call_id)
{
  if (call_id < 0)
    return;

  int needed_bytes = call_id / 8 + 1;
  if (needed_bytes > *pure_call_id_bytes)
  {
    int old_bytes = *pure_call_id_bytes;
    int new_bytes = old_bytes ? old_bytes * 2 : 32;
    while (new_bytes < needed_bytes)
      new_bytes *= 2;
    *pure_call_ids = tcc_realloc(*pure_call_ids, new_bytes);
    memset(*pure_call_ids + old_bytes, 0, new_bytes - old_bytes);
    *pure_call_id_bytes = new_bytes;
  }

  (*pure_call_ids)[call_id / 8] |= (uint8_t)(1u << (call_id & 7));
}

static int ir_opt_callee_is_body_elidable(TCCIRState *ir, Sym *callee)
{
  if (!callee)
    return 0;

  const char *name = get_tok_str(callee->v, NULL);
  if (name && tcc_ir_is_pure_aeabi(name))
    return 1;

  /* Flag-cmp / fneg / dneg helpers have no side effects beyond their result — elidable. */
  if (name && ir_opt_is_flag_cmp_helper_name(name))
    return 1;
  if (name && (strcmp(name, "__aeabi_fneg") == 0 || strcmp(name, "__aeabi_dneg") == 0))
    return 1;

  return tcc_ir_get_func_purity(ir, callee) >= TCC_FUNC_PURITY_PURE;
}

static int ir_opt_param_vreg_is_volatile(int param_pos)
{
  int32_t param_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, param_pos);
  if (tcc_state && tcc_state->ir && tcc_ir_vreg_is_valid(tcc_state->ir, param_vreg))
  {
    IRLiveInterval *iv = tcc_ir_vreg_live_interval(tcc_state->ir, param_vreg);
    if (iv)
      return iv->is_volatile != 0;
  }

  for (Sym *sym = local_stack; sym; sym = sym->prev)
  {
    if (sym->vreg == param_vreg)
      return (sym->type.t & VT_VOLATILE) != 0;
  }

  if (!tcc_state || !tcc_state->cur_func_sym || !tcc_state->cur_func_sym->type.ref)
    return 1;

  Sym *param = tcc_state->cur_func_sym->type.ref->next;
  for (int i = 0; param && i < param_pos; i++)
    param = param->next;

  if (!param)
    return 1;
  return (param->type.t & VT_VOLATILE) != 0;
}

static int ir_opt_vreg_sym_is_volatile(int32_t vr)
{
  if (tcc_state && tcc_state->ir && tcc_ir_vreg_is_valid(tcc_state->ir, vr))
  {
    IRLiveInterval *iv = tcc_ir_vreg_live_interval(tcc_state->ir, vr);
    if (iv)
      return iv->is_volatile != 0;
  }

  for (Sym *sym = local_stack; sym; sym = sym->prev)
  {
    if (sym->vreg == vr)
      return (sym->type.t & VT_VOLATILE) != 0;
  }
  return 0;
}

static int ir_opt_direct_auto_vreg_store_is_local(IROperand op)
{
  int32_t vr;
  int vt;

  if (op.is_lval || op.is_sym || op.is_llocal)
    return 0;

  vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  vt = TCCIR_DECODE_VREG_TYPE(vr);
  if (vt == TCCIR_VREG_TYPE_VAR)
    return !ir_opt_vreg_sym_is_volatile(vr);
  if (vt == TCCIR_VREG_TYPE_PARAM)
    return !ir_opt_param_vreg_is_volatile(TCCIR_DECODE_VREG_POSITION(vr));
  return 0;
}

static int ir_opt_operand_is_volatile_sym(TCCIRState *ir, IROperand s)
{
  if (!s.is_sym)
    return 0;
  Sym *sym = irop_get_sym_ex(ir, s);
  return sym && (sym->type.t & VT_VOLATILE);
}

static int ir_opt_op_is_essential(TCCIRState *ir, IRQuadCompact *q, int idx,
                                  const uint8_t *pure_call_ids, int pure_call_id_bytes)
{
  switch (q->op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  {
    /* Backward/self jumps form observable loops; forward jumps route over NOPs and drop. */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)dest.u.imm32;
    if (target >= 0 && target <= idx)
      return 1;
    return 0;
  }
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_CALLSEQ_BEGIN:
  case TCCIR_OP_CALLARG_REG:
  case TCCIR_OP_CALLARG_STACK:
  case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_INIT_CHAIN_SLOT:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_BUILTIN_APPLY_ARGS:
  case TCCIR_OP_BUILTIN_APPLY:
  case TCCIR_OP_BUILTIN_RETURN:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SWITCH_LOAD:
    return 1;
  case TCCIR_OP_STORE:
  {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    int dest_vt = TCCIR_DECODE_VREG_TYPE(dest_vr);
    /* Direct write to a non-volatile param is unobservable once the body is side-effect-free. */
    if (tcc_state && tcc_state->ir_late_reopt_phase &&
        !dest.is_lval && !dest.is_sym && !dest.is_llocal &&
        dest_vt == TCCIR_VREG_TYPE_PARAM &&
        !ir_opt_param_vreg_is_volatile(TCCIR_DECODE_VREG_POSITION(dest_vr)))
      return 0;
    return 1;
  }
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
    return 1;
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  {
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    return !ir_opt_callee_is_body_elidable(ir, callee);
  }
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  {
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, src2));
    return !ir_opt_pure_call_id_test(pure_call_ids, pure_call_id_bytes, call_id);
  }
  default:
    break;
  }

  /* Volatile sym read on any source: keep the function alive. */
  if (irop_config[q->op].has_src1 && ir_opt_operand_is_volatile_sym(ir, tcc_ir_op_get_src1(ir, q)))
    return 1;
  if (irop_config[q->op].has_src2 && ir_opt_operand_is_volatile_sym(ir, tcc_ir_op_get_src2(ir, q)))
    return 1;
  return 0;
}

static int ir_opt_vreg_has_def_in_range(TCCIRState *ir, int32_t vreg, int start, int end)
{
  if (vreg < 0)
    return 0;
  for (int i = start; i <= end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!dest.is_lval && irop_get_vreg(dest) == vreg)
      return 1;
  }
  return 0;
}

static int ir_opt_vreg_has_iv_update_in_range(TCCIRState *ir, int32_t vreg, int start, int end, int depth)
{
  if (vreg < 0 || depth > 2)
    return 0;

  for (int i = start; i <= end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.is_lval || irop_get_vreg(dest) != vreg)
      continue;

    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) &&
        irop_config[q->op].has_src1 && irop_config[q->op].has_src2 &&
        irop_is_immediate(tcc_ir_op_get_src2(ir, q)))
    {
      int32_t s1 = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (ir_opt_vreg_has_def_in_range(ir, s1, start, end))
        return 1;
    }

    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_src1)
    {
      int32_t src = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (ir_opt_vreg_has_iv_update_in_range(ir, src, start, end, depth + 1))
        return 1;
    }
  }

  return 0;
}

static int ir_opt_jumpif_uses_iv_update(TCCIRState *ir, int jif_idx, int start, int end)
{
  int scan_floor = start < jif_idx ? start : 0;
  for (int i = jif_idx - 1; i >= scan_floor; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      break;
    if (q->op != TCCIR_OP_CMP && q->op != TCCIR_OP_TEST_ZERO)
      continue;

    if (irop_config[q->op].has_src1)
    {
      int32_t s1 = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (ir_opt_vreg_has_iv_update_in_range(ir, s1, start, end, 0))
        return 1;
    }
    if (irop_config[q->op].has_src2)
    {
      int32_t s2 = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
      if (ir_opt_vreg_has_iv_update_in_range(ir, s2, start, end, 0))
        return 1;
    }
    return 0;
  }

  return 0;
}

static int ir_opt_range_has_iv_update(TCCIRState *ir, int start, int end)
{
  for (int i = start; i <= end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      continue;
    if (!irop_config[q->op].has_src2 || !irop_is_immediate(tcc_ir_op_get_src2(ir, q)))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.is_lval)
      continue;

    int32_t dest_vr = irop_get_vreg(dest);
    int dest_vt = TCCIR_DECODE_VREG_TYPE(dest_vr);
    if (dest_vt != TCCIR_VREG_TYPE_TEMP && dest_vt != TCCIR_VREG_TYPE_VAR)
      continue;

    int32_t src_vr = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
    if (ir_opt_vreg_has_def_in_range(ir, src_vr, start, end))
      return 1;
  }
  return 0;
}

/* Region has both a JUMPIF and an out-of-region edge: a conditionally-terminating loop
 * (C11 6.8.5p6, with a monotonic IV) rather than a bare for(;;). */
static int ir_opt_region_has_conditional_exit(TCCIRState *ir, int start, int end)
{
  int has_cond = 0;
  int has_exit = 0;
  for (int i = start; i <= end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    if (q->op == TCCIR_OP_JUMPIF)
      has_cond = 1;
    if (t < start || t > end)
      has_exit = 1;
    if (has_cond && has_exit)
      return 1;
  }
  return 0;
}

static int ir_opt_successor_enters_range(TCCIRState *ir, int succ, int start, int end)
{
  int n = ir->next_instruction_index;
  if (succ >= start && succ <= end)
    return 1;
  while (succ >= 0 && succ < n && ir->compact_instructions[succ].op == TCCIR_OP_NOP)
    succ++;
  if (succ >= start && succ <= end)
    return 1;
  if (succ >= 0 && succ < n && ir->compact_instructions[succ].op == TCCIR_OP_JUMP)
  {
    IROperand dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[succ]);
    int target = (int)irop_get_imm64_ex(ir, dest);
    return target >= start && target <= end;
  }
  return 0;
}

static int ir_opt_backward_jump_has_cond_exit(TCCIRState *ir, int idx)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int target = (int)irop_get_imm64_ex(ir, dest);

  if (target < 0 || target > idx)
    return 0;

  /* Conditional back-edge driven by an in-loop IV update is a finite loop; bare
   * infinite loops and exits on an unchanged param/load stay essential. */
  if (q->op == TCCIR_OP_JUMPIF)
  {
    if (ir_opt_jumpif_uses_iv_update(ir, idx, target, idx))
      return 1;
    /* Secondary conditional back-edges inside a finite IV loop don't keep the body alive. */
    return ir_opt_range_has_iv_update(ir, target, idx);
  }

  if (q->op != TCCIR_OP_JUMP)
    return 0;

  /* C11 6.8.5p6 forward-progress: a monotonic-IV loop with a genuine conditional exit
   * may be assumed to terminate, including exits indirected through extra blocks. */
  if (ir_opt_range_has_iv_update(ir, target, idx) &&
      ir_opt_region_has_conditional_exit(ir, target, idx))
    return 1;

  for (int i = 0; i <= idx; i++)
  {
    IRQuadCompact *iq = &ir->compact_instructions[i];
    if (iq->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand idest = tcc_ir_op_get_dest(ir, iq);
    int itarget = (int)irop_get_imm64_ex(ir, idest);
    int target_enters = ir_opt_successor_enters_range(ir, itarget, target, idx);
    int fallthrough_enters = ir_opt_successor_enters_range(ir, i + 1, target, idx);
    if (!target_enters && fallthrough_enters &&
        ir_opt_jumpif_uses_iv_update(ir, i, target, idx))
      return 1;
    if (target_enters && !fallthrough_enters &&
        ir_opt_jumpif_uses_iv_update(ir, i, target, idx))
      return 1;
  }

  return 0;
}

/* All ops now NOP: clear the post-regalloc dirty bitmap / live-reg map, mark the
 * function a leaf, and drop parked parameter allocations so the prologue emits
 * no spurious push/pop or mov-param setup. */
static void ir_opt_reset_elided_body_codegen_state(TCCIRState *ir)
{
  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;
  for (int p = 0; p < ir->next_parameter; p++)
  {
    IRLiveInterval *iv = &ir->parameters_live_intervals[p];
    iv->allocation.r0 = PREG_NONE;
    iv->allocation.r1 = PREG_NONE;
    iv->allocation.offset = 0;
  }
}

int tcc_ir_opt_useless_function_body(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  uint8_t *pure_call_ids = NULL;
  int pure_call_id_bytes = 0;

  /* Uniform constant return: every RETURNVALUE returns the same immediate → collapse to one. */
  IROperand rv_src = IROP_NONE;
  int rv_count = 0;
  int rv_ok = 1;
  int64_t rv_val = 0;
  int rv_tag = 0, rv_btype = 0;
  for (int i = 0; i < n && rv_ok; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_RETURNVOID)
    {
      rv_ok = 0;
      break;
    }
    if (q->op != TCCIR_OP_RETURNVALUE)
      continue;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    int tag = irop_get_tag(s);
    if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64)
    {
      rv_ok = 0;
      break;
    }
    int64_t v = irop_get_imm64_ex(ir, s);
    if (rv_count == 0)
    {
      rv_val = v;
      rv_tag = tag;
      rv_btype = s.btype;
      rv_src = s;
    }
    else if (v != rv_val || tag != rv_tag || s.btype != rv_btype)
      rv_ok = 0;
    rv_count++;
  }
  if (rv_count == 0)
    rv_ok = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!ir_opt_callee_is_body_elidable(ir, callee))
      continue;

    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, src2));
    ir_opt_pure_call_id_mark(&pure_call_ids, &pure_call_id_bytes, call_id);
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (rv_ok && q->op == TCCIR_OP_RETURNVALUE)
      continue;
    if (ir_opt_op_is_essential(ir, q, i, pure_call_ids, pure_call_id_bytes))
    {
      if ((q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) &&
          ir_opt_backward_jump_has_cond_exit(ir, i))
        continue;
      if (pure_call_ids)
        tcc_free(pure_call_ids);
      return 0;
    }
  }

  if (pure_call_ids)
    tcc_free(pure_call_ids);

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
    {
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
      changes++;
    }
  }

  if (rv_ok)
  {
    for (int i = 0; i < n; i++)
      ir->compact_instructions[i].is_jump_target = 0;
    ir->compact_instructions[0].op = TCCIR_OP_RETURNVALUE;
    tcc_ir_set_dest(ir, 0, IROP_NONE);
    tcc_ir_set_src1(ir, 0, rv_src);
    tcc_ir_set_src2(ir, 0, IROP_NONE);
  }

  ir_opt_reset_elided_body_codegen_state(ir);
  /* Drop frame-pointer forcing so the prologue collapses to a bare bx lr. */
  tcc_state->need_frame_pointer = 0;
  tcc_state->force_frame_pointer = 0;

  LOG_IR_GEN("USELESS-BODY: NOPed %d instructions (no observable side effects)", changes);
  /* Return 1 even when changes==0 so the caller still resets loc for now-dead locals. */
  return changes > 0 ? changes : 1;
}

int tcc_ir_opt_useless_function_body_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_useless_function_body(ctx->ir);
}

/* Anchor test for dead_before_infinite_loop: a function that never returns and has
 * no externally-observable effect is indistinguishable from a self-jump, so its
 * STOREs (which useless_function_body kept) become unobservable and non-anchoring. */
static int ir_opt_op_is_inf_dead_anchor(TCCIRState *ir, IRQuadCompact *q, int idx)
{
  switch (q->op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
    return 0; /* pure control flow */
  case TCCIR_OP_STORE:
  {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    /* Store to a global (sym lval): observable-without-return only if volatile. */
    if (dest.is_sym && dest.is_lval)
    {
      Sym *s = irop_get_sym_ex(ir, dest);
      return (s && (s->type.t & VT_VOLATILE)) ? 1 : 0;
    }
    /* Direct store to a local/param vreg: dead unless volatile. */
    if (!dest.is_lval && !dest.is_sym && !dest.is_llocal)
    {
      int32_t dvr = irop_get_vreg(dest);
      int dvt = TCCIR_DECODE_VREG_TYPE(dvr);
      if (dvt == TCCIR_VREG_TYPE_VAR)
        return ir_opt_vreg_sym_is_volatile(dvr);
      if (dvt == TCCIR_VREG_TYPE_PARAM)
        return ir_opt_param_vreg_is_volatile(TCCIR_DECODE_VREG_POSITION(dvr));
    }
    /* Pointer / unknown store target: keep conservatively. */
    return 1;
  }
  default:
    break;
  }
  /* Everything else keeps its ir_opt_op_is_essential classification. */
  return ir_opt_op_is_essential(ir, q, idx, NULL, 0);
}

/* Forward-walk CFG successors from `start` within the dead/sink set to an
 * empty-infinite-loop sink; returns its index, or -1 if none is reachable. */
static int ir_inf_dead_find_sink(TCCIRState *ir, int start, const uint8_t *dead,
                                 const uint8_t *is_sink, int n)
{
  uint8_t *vis = tcc_mallocz((n + 7) / 8);
  int *stk = tcc_malloc(n * sizeof(int));
  int sp = 0, found = -1;
  stk[sp++] = start;
  vis[start / 8] |= (1 << (start % 8));
  while (sp > 0)
  {
    int i = stk[--sp];
    if (is_sink[i / 8] & (1 << (i % 8)))
    {
      found = i;
      break;
    }
    IRQuadCompact *q = &ir->compact_instructions[i];
    int succ[2], ns = 0;
    switch (q->op)
    {
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      break;
    case TCCIR_OP_JUMP:
      succ[ns++] = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      break;
    case TCCIR_OP_JUMPIF:
      succ[ns++] = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      succ[ns++] = i + 1;
      break;
    default:
      succ[ns++] = i + 1;
      break;
    }
    for (int k = 0; k < ns; k++)
    {
      int s = succ[k];
      if (s < 0 || s >= n)
        continue;
      int sdead = dead[s / 8] & (1 << (s % 8));
      int ssink = is_sink[s / 8] & (1 << (s % 8));
      if (!sdead && !ssink)
        continue; /* escapes the dead region — cannot happen for a dead node */
      if (!(vis[s / 8] & (1 << (s % 8))))
      {
        vis[s / 8] |= (1 << (s % 8));
        stk[sp++] = s;
      }
    }
  }
  tcc_free(vis);
  tcc_free(stk);
  return found;
}

int tcc_ir_opt_dead_before_infinite_loop(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* Indirect jumps have statically-unknown successors — bail. */
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;

#define GETBIT(arr, k) ((arr)[(k) / 8] & (1 << ((k) % 8)))
#define SETBIT(arr, k) ((arr)[(k) / 8] |= (1 << ((k) % 8)))

  /* Empty infinite-loop sinks: a JUMP whose target is itself. */
  uint8_t *is_sink = tcc_mallocz((n + 7) / 8);
  int have_sink = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP)
      continue;
    if ((int)tcc_ir_op_get_dest(ir, q).u.imm32 == i)
    {
      SETBIT(is_sink, i);
      have_sink = 1;
    }
  }
  if (!have_sink)
  {
    tcc_free(is_sink);
    return 0;
  }

  /* anchor[i]: instruction has an effect observable without returning. */
  uint8_t *anchor = tcc_mallocz((n + 7) / 8);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (ir_opt_op_is_inf_dead_anchor(ir, q, i))
      SETBIT(anchor, i);
  }

  /* can_reach[i]: from i, control can reach an anchor (backward fixpoint). */
  uint8_t *can_reach = tcc_mallocz((n + 7) / 8);
  int changed = 1;
  while (changed)
  {
    changed = 0;
    for (int i = n - 1; i >= 0; i--)
    {
      if (GETBIT(can_reach, i))
        continue;
      IRQuadCompact *q = &ir->compact_instructions[i];
      int reach = GETBIT(anchor, i) ? 1 : 0;
      if (!reach)
      {
        switch (q->op)
        {
        case TCCIR_OP_RETURNVALUE:
        case TCCIR_OP_RETURNVOID:
        case TCCIR_OP_TRAP:
        case TCCIR_OP_SWITCH_TABLE:
          break; /* anchors / no fall-through */
        case TCCIR_OP_JUMP:
        {
          int t = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
          if (t >= 0 && t < n && GETBIT(can_reach, t))
            reach = 1;
          break;
        }
        case TCCIR_OP_JUMPIF:
        {
          int t = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
          if (t >= 0 && t < n && GETBIT(can_reach, t))
            reach = 1;
          if (i + 1 < n && GETBIT(can_reach, i + 1))
            reach = 1;
          break;
        }
        default:
          if (i + 1 < n && GETBIT(can_reach, i + 1))
            reach = 1;
          break;
        }
      }
      if (reach)
      {
        SETBIT(can_reach, i);
        changed = 1;
      }
    }
  }

  /* reach[i]: executed on some path from entry (forward BFS). */
  uint8_t *reach = tcc_mallocz((n + 7) / 8);
  int *wl = tcc_malloc(n * sizeof(int));
  int wh = 0, wt = 0;
  SETBIT(reach, 0);
  wl[wt++] = 0;
  while (wh < wt)
  {
    int i = wl[wh++];
    IRQuadCompact *q = &ir->compact_instructions[i];
#define PUSH(k)                                                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    int _k = (k);                                                                                                      \
    if (_k >= 0 && _k < n && !GETBIT(reach, _k))                                                                       \
    {                                                                                                                  \
      SETBIT(reach, _k);                                                                                               \
      wl[wt++] = _k;                                                                                                   \
    }                                                                                                                  \
  } while (0)
    switch (q->op)
    {
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      break;
    case TCCIR_OP_JUMP:
      PUSH((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      break;
    case TCCIR_OP_JUMPIF:
      PUSH((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      PUSH(i + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE:
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
          PUSH(table->targets[j]);
        PUSH(table->default_target);
      }
      break;
    }
    default:
      PUSH(i + 1);
      break;
    }
#undef PUSH
  }

  /* dead[i]: reachable, cannot reach an anchor, and not itself a sink. */
  uint8_t *dead = tcc_mallocz((n + 7) / 8);
  int any_dead = 0;
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    if (GETBIT(is_sink, i))
      continue;
    if (GETBIT(reach, i) && !GETBIT(can_reach, i))
    {
      SETBIT(dead, i);
      any_dead = 1;
    }
  }

  int changes = 0;
  if (!any_dead)
    goto done;

  /* entry[d]: dead instr reached from a kept instruction; such edges reroute to the sink. */
  uint8_t *entry = tcc_mallocz((n + 7) / 8);
  for (int p = 0; p < n; p++)
  {
    if (!GETBIT(reach, p) || GETBIT(dead, p))
      continue;
    IRQuadCompact *q = &ir->compact_instructions[p];
    if (q->op == TCCIR_OP_NOP)
      continue;
#define MARKENTRY(s)                                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    int _s = (s);                                                                                                      \
    if (_s >= 0 && _s < n && GETBIT(dead, _s))                                                                         \
      SETBIT(entry, _s);                                                                                               \
  } while (0)
    switch (q->op)
    {
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      break;
    case TCCIR_OP_JUMP:
      MARKENTRY((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      break;
    case TCCIR_OP_JUMPIF:
      MARKENTRY((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      MARKENTRY(p + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE:
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
          MARKENTRY(table->targets[j]);
        MARKENTRY(table->default_target);
      }
      break;
    }
    default:
      MARKENTRY(p + 1);
      break;
    }
#undef MARKENTRY
  }

  /* Resolve a sink for every entry up front; abort untouched if any can't reach one. */
  int *entry_sink = tcc_malloc(n * sizeof(int));
  int abort_pass = 0;
  for (int d = 0; d < n; d++)
  {
    entry_sink[d] = -1;
    if (!GETBIT(entry, d))
      continue;
    int sink = ir_inf_dead_find_sink(ir, d, dead, is_sink, n);
    if (sink < 0)
    {
      abort_pass = 1;
      break;
    }
    entry_sink[d] = sink;
  }
  if (abort_pass)
  {
    tcc_free(entry);
    tcc_free(entry_sink);
    goto done;
  }

  /* Reroute entries to their sink, NOP the rest; track removed LEAs to clear spill flags. */
  int32_t *cleared_vr = tcc_malloc(n * sizeof(int32_t));
  int ncleared = 0;
  for (int d = 0; d < n; d++)
  {
    if (!GETBIT(dead, d))
      continue;
    IRQuadCompact *q = &ir->compact_instructions[d];
    if (q->op == TCCIR_OP_LEA)
    {
      int32_t lea_src = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (lea_src >= 0)
        cleared_vr[ncleared++] = lea_src;
    }
    if (GETBIT(entry, d))
    {
      q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, d, irop_make_imm32(-1, entry_sink[d], IROP_BTYPE_INT32));
      tcc_ir_set_src1(ir, d, IROP_NONE);
      tcc_ir_set_src2(ir, d, IROP_NONE);
    }
    else
    {
      q->op = TCCIR_OP_NOP;
    }
    changes++;
  }

  /* Clear addrtaken on any vreg whose last surviving LEA we removed — drops the stack spill. */
  for (int c = 0; c < ncleared; c++)
  {
    int32_t vr = cleared_vr[c];
    int still = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_LEA)
        continue;
      if (irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vr)
      {
        still = 1;
        break;
      }
    }
    if (!still && tcc_ir_vreg_is_valid(ir, vr))
    {
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, vr);
      if (iv)
        iv->addrtaken = 0;
    }
  }

  LOG_IR_GEN("DEAD-BEFORE-INF-LOOP: rerouted/NOPed %d instructions", changes);

  tcc_free(cleared_vr);
  tcc_free(entry);
  tcc_free(entry_sink);

done:
  tcc_free(is_sink);
  tcc_free(anchor);
  tcc_free(can_reach);
  tcc_free(reach);
  tcc_free(wl);
  tcc_free(dead);
#undef GETBIT
#undef SETBIT
  return changes;
}

int tcc_ir_opt_dead_before_infinite_loop_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_before_infinite_loop(ctx->ir);
}

/* Collapse a function body whose every side effect is confined to its own stack
 * frame (no caller-visible state): NOP the body and emit a single constant/void
 * return.  Handles direct local stores, stores through proven local-frame
 * pointers, and memmove-like calls whose first arg points into the local frame. */
int tcc_ir_opt_local_only_body_elide(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;


  /* Static-chain functions off-limits: a nested function's StackLoc writes hit the
   * parent frame, and a parent exposing its frame to a callee makes even its own
   * local writes observable. */
  if (ir->has_static_chain)
    return 0;

#define LOCAL_ONLY_MAX_MEMMOVE_CALLS 256
#define LOCAL_ONLY_MAX_TEMPS 8192

  /* Pass 0: scan for hard bails; record memmove-like calls for later first-arg check. */
  int memmove_call_ids[LOCAL_ONLY_MAX_MEMMOVE_CALLS];
  int n_memmove_calls = 0;
  int has_observable_op = 0;
  IROperand return_src = IROP_NONE;
  int return_count = 0;
  int return_void_count = 0;
  int first_return_idx = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    /* Hard bails: ops that publish state or do non-local control flow. */
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_PREFETCH:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      return 0;
    case TCCIR_OP_RETURNVALUE:
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int tag = irop_get_tag(s);
      if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64)
        return 0;
      if (return_count == 0)
      {
        return_src = s;
        first_return_idx = i;
      }
      else if (tag != irop_get_tag(return_src) || s.btype != return_src.btype ||
               irop_get_imm64_ex(ir, s) != irop_get_imm64_ex(ir, return_src))
      {
        return 0;
      }
      return_count++;
      break;
    }
    case TCCIR_OP_RETURNVOID:
      return_void_count++;
      break;
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!callee)
        return 0;
      const char *name = get_tok_str(callee->v, NULL);
      if (!name)
        return 0;
      has_observable_op = 1;
      if (tcc_ir_is_pure_aeabi(name))
        break;
      /* Flag-cmp helpers are functionally pure — CPSR flags are caller-invisible. */
      if (ir_opt_is_flag_cmp_helper_name(name))
        break;
      /* memmove/memcpy/memset and va_list helpers write through their first arg only;
       * verify that arg is a local-frame pointer later. */
      int is_memlike = strcmp(name, "__aeabi_memmove4") == 0 || strcmp(name, "__aeabi_memmove8") == 0 ||
                       strcmp(name, "__aeabi_memmove") == 0 || strcmp(name, "__aeabi_memcpy4") == 0 ||
                       strcmp(name, "__aeabi_memcpy8") == 0 || strcmp(name, "__aeabi_memcpy") == 0 ||
                       strcmp(name, "__aeabi_memset") == 0 || strcmp(name, "__aeabi_memset4") == 0 ||
                       strcmp(name, "__aeabi_memset8") == 0 || strcmp(name, "__aeabi_memclr") == 0 ||
                       strcmp(name, "__aeabi_memclr4") == 0 || strcmp(name, "__aeabi_memclr8") == 0 ||
                       strcmp(name, "memmove") == 0 || strcmp(name, "memcpy") == 0 ||
                       strcmp(name, "memset") == 0 || strcmp(name, "__tcc_va_arg") == 0 ||
                       strcmp(name, "__tcc_va_start") == 0;
      if (!is_memlike)
        return 0;
      if (n_memmove_calls >= LOCAL_ONLY_MAX_MEMMOVE_CALLS)
        return 0;
      IROperand call_id_op = tcc_ir_op_get_src2(ir, q);
      int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, call_id_op));
      memmove_call_ids[n_memmove_calls++] = call_id;
      break;
    }
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
      has_observable_op = 1;
      break;
    default:
      break;
    }

    /* Volatile sym access on any operand keeps the body alive. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (ir_opt_operand_is_volatile_sym(ir, op))
        return 0;
    }
  }

  /* useless_function_body covers the no-call-no-store case — leave it to avoid double-counting. */
  if (!has_observable_op)
    return 0;
  if (return_count > 0 && return_void_count > 0)
    return 0;

  /* Pass 1: forward fixpoint marking TEMPs that hold a local-frame pointer;
   * seed from LEA of a stack-local, propagate through ASSIGN/ADD/SUB. */
  uint8_t local_ptr[(LOCAL_ONLY_MAX_TEMPS + 7) / 8] = {0};

#define LP_GET(p) ((local_ptr[(p) >> 3] & (uint8_t)(1u << ((p) & 7))) != 0)
#define LP_SET(p)                                                                                                      \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((p) >= 0 && (p) < LOCAL_ONLY_MAX_TEMPS)                                                                        \
      local_ptr[(p) >> 3] |= (uint8_t)(1u << ((p) & 7));                                                               \
  } while (0)

#define IS_LOCAL_PTR_OP(sop)                                                                                           \
  ({                                                                                                                   \
    int _r = 0;                                                                                                        \
    int32_t _vr = irop_get_vreg(sop);                                                                                  \
    int _tag = irop_get_tag(sop);                                                                                      \
    if (_tag == IROP_TAG_STACKOFF && (sop).is_local && !(sop).is_lval && !(sop).is_llocal && !(sop).is_param)          \
      _r = 1;                                                                                                          \
    if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_TEMP)                                               \
    {                                                                                                                  \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                        \
      if (_p >= 0 && _p < LOCAL_ONLY_MAX_TEMPS && LP_GET(_p))                                                          \
        _r = 1;                                                                                                        \
    }                                                                                                                  \
    _r;                                                                                                                \
  })

  for (int iter = 0; iter < 16; iter++)
  {
    int changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[q->op].has_dest)
        continue;
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (dop.is_lval) /* STORE-through-TEMP form is not a def of this TEMP */
        continue;
      int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (dpos < 0 || dpos >= LOCAL_ONLY_MAX_TEMPS)
        continue;
      if (LP_GET(dpos))
        continue;

      int set = 0;
      switch (q->op)
      {
      case TCCIR_OP_LEA:
      {
        /* Address of a stack-local or non-volatile param: a local-only pointer. */
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (s.is_llocal)
          break;
        int tag = irop_get_tag(s);
        if (tag == IROP_TAG_STACKOFF && s.is_local)
          set = 1;
        else
        {
          int32_t svr = irop_get_vreg(s);
          if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR && s.is_local)
            set = 1;
          else if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_PARAM &&
                   !ir_opt_param_vreg_is_volatile(TCCIR_DECODE_VREG_POSITION(svr)))
            set = 1;
        }
        break;
      }
      case TCCIR_OP_ASSIGN:
      case TCCIR_OP_STORE:
      {
        /* ASSIGN, or a STORE with non-lval TEMP dest (a TEMP def after var-to-tmp
         * promotion): propagate local-pointer status from the source. */
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (IS_LOCAL_PTR_OP(s))
          set = 1;
        break;
      }
      case TCCIR_OP_ADD:
      case TCCIR_OP_SUB:
      {
        /* local-pointer ± offset stays local-pointer; one local-pointer operand suffices. */
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        if (IS_LOCAL_PTR_OP(s1) || IS_LOCAL_PTR_OP(s2))
          set = 1;
        break;
      }
      case TCCIR_OP_MLA:
      {
        /* MLA with a local-pointer accum: result is local-pointer + scaled offset. */
        IROperand accum = tcc_ir_op_get_accum(ir, q);
        if (IS_LOCAL_PTR_OP(accum))
          set = 1;
        break;
      }
      default:
        break;
      }

      if (set)
      {
        LP_SET(dpos);
        changed = 1;
      }
    }
    if (!changed)
      break;
  }

  /* Pass 2a: every STORE* must write through a local-pointer address. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
      continue;
    IROperand dop = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dop);
    int ok = 0;
    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      /* STORE with non-lval TEMP dest is a TEMP def (no memory write) — local-only. */
      if (q->op == TCCIR_OP_STORE && !dop.is_lval)
        ok = 1;
      else if (IS_LOCAL_PTR_OP(dop))
        ok = 1;
    }
    else if (q->op == TCCIR_OP_STORE)
    {
      /* Direct automatic-object stores and STACKOFF/local dests are local-only. */
      int tag = irop_get_tag(dop);
      if (ir_opt_direct_auto_vreg_store_is_local(dop))
        ok = 1;
      else if (tag == IROP_TAG_STACKOFF && dop.is_local && !dop.is_param)
        ok = 1;
    }
    else if (q->op == TCCIR_OP_STORE_INDEXED)
    {
      /* Direct indexed write into a local stack object stays in this frame; require
       * the address-of form (an lvalue slot would load a pointer then store through it). */
      int tag = irop_get_tag(dop);
      if (tag == IROP_TAG_STACKOFF && dop.is_local && !dop.is_lval && !dop.is_llocal && !dop.is_param)
        ok = 1;
    }
    else
    {
      /* STORE_INDEXED/STORE_POSTINC through a pointer: only proven local-pointer TEMPs accepted. */
    }
    if (!ok)
      return 0;
  }

  /* Pass 2b: each memmove-like call's first-arg PARAM must be a local pointer. */
  for (int j = 0; j < n_memmove_calls; j++)
  {
    int target_call_id = memmove_call_ids[j];
    int verified = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
        continue;
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int64_t encoded = irop_get_imm64_ex(ir, src2);
      if (TCCIR_DECODE_CALL_ID(encoded) != target_call_id)
        continue;
      if (TCCIR_DECODE_PARAM_IDX(encoded) != 0)
        continue;
      IROperand val = tcc_ir_op_get_src1(ir, q);
      if (IS_LOCAL_PTR_OP(val))
        verified = 1;
      break;
    }
    if (!verified)
      return 0;
  }

#undef LP_GET
#undef LP_SET
#undef IS_LOCAL_PTR_OP
#undef LOCAL_ONLY_MAX_MEMMOVE_CALLS
#undef LOCAL_ONLY_MAX_TEMPS

  LOG_IR_GEN("LOCAL-ONLY-ELIDE: collapsing function body — every side effect "
             "is confined to the local stack frame (no caller-visible state)");

  for (int i = 0; i < n; i++)
  {
    if (return_count > 0 && i == first_return_idx)
    {
      ir->compact_instructions[i].op = TCCIR_OP_RETURNVALUE;
      tcc_ir_set_src1(ir, i, return_src);
    }
    else
    {
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
    }
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir_opt_reset_elided_body_codegen_state(ir);

  return 1;
}

int tcc_ir_opt_local_only_body_elide_ex(IROptCtx *ctx) { return tcc_ir_opt_local_only_body_elide(ctx->ir); }
