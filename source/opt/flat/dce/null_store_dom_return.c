/*
 *  TCC IR - null-store-dominates-return dead code elimination
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

static int nsdr_operand_by_slot(TCCIRState *ir, IRQuadCompact *q, int k, IROperand *out)
{
  if (k == 0)
  {
    if (!irop_config[q->op].has_dest)
      return 0;
    *out = tcc_ir_op_get_dest(ir, q);
  }
  else if (k == 1)
  {
    if (!irop_config[q->op].has_src1)
      return 0;
    *out = tcc_ir_op_get_src1(ir, q);
  }
  else
  {
    if (!irop_config[q->op].has_src2)
      return 0;
    *out = tcc_ir_op_get_src2(ir, q);
  }
  return 1;
}

int tcc_ir_opt_null_store_dom_return(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  /* O2-only UB-exploit pass. */
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* Bail on unanalyzable ops that can hide pointer writes, and on non-void returns. */
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    switch (op)
    {
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_RETURNVALUE:
      return 0;
    default:
      break;
    }
    /* Volatile sym access on any operand is observable; can't elide. */
    IRQuadCompact *q = &ir->compact_instructions[i];
    for (int k = 0; k <= 2; k++)
    {
      IROperand op2;
      if (!nsdr_operand_by_slot(ir, q, k, &op2))
        continue;
      if (op2.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op2);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }
#define NSDR_MAX_TEMP 8192
#define NSDR_MAX_VAR 1024
  uint8_t temp_zero[(NSDR_MAX_TEMP + 7) / 8] = {0};
  uint8_t var_zero[(NSDR_MAX_VAR + 7) / 8] = {0};
  uint8_t var_addr_taken[(NSDR_MAX_VAR + 7) / 8] = {0};

  /* Pre-scan address-taken VARs (pointer writes may init them invisibly). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (!nsdr_operand_by_slot(ir, q, k, &op))
        continue;
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      if (op.is_local && !op.is_lval)
      {
        int p = TCCIR_DECODE_VREG_POSITION(vr);
        if (p >= 0 && p < NSDR_MAX_VAR)
          var_addr_taken[p >> 3] |= (uint8_t)(1u << (p & 7));
      }
    }
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand op = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(vr);
        if (p >= 0 && p < NSDR_MAX_VAR)
          var_addr_taken[p >> 3] |= (uint8_t)(1u << (p & 7));
      }
    }
  }

#define VREG_IS_KNOWN_ZERO(_op)                                                                                            \
  ({                                                                                                                       \
    int _r = 0;                                                                                                            \
    int32_t _vr = irop_get_vreg(_op);                                                                                      \
    if (_vr >= 0 && !(_op).is_lval)                                                                                        \
    {                                                                                                                      \
      int _t = TCCIR_DECODE_VREG_TYPE(_vr);                                                                                \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                            \
      if (_t == TCCIR_VREG_TYPE_TEMP && _p >= 0 && _p < NSDR_MAX_TEMP)                                                     \
      {                                                                                                                    \
        if (temp_zero[_p >> 3] & (uint8_t)(1u << (_p & 7)))                                                                \
          _r = 1;                                                                                                          \
      }                                                                                                                    \
      else if (_t == TCCIR_VREG_TYPE_VAR && _p >= 0 && _p < NSDR_MAX_VAR &&                                                \
               !(var_addr_taken[_p >> 3] & (uint8_t)(1u << (_p & 7))))                                                     \
      {                                                                                                                    \
        if (var_zero[_p >> 3] & (uint8_t)(1u << (_p & 7)))                                                                 \
          _r = 1;                                                                                                          \
      }                                                                                                                    \
    }                                                                                                                      \
    _r;                                                                                                                    \
  })

  int ub_store_idx = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* STORE through known-NULL pointer (dest is the address it reads). */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (dest.is_lval && !dest.is_local)
      {
        /* Direct immediate NULL address operand. */
        if (irop_is_immediate(dest) && irop_get_imm64_ex(ir, dest) == 0)
        {
          ub_store_idx = i;
          break;
        }
        /* Vreg currently known to be zero. */
        int32_t vr = irop_get_vreg(dest);
        if (vr >= 0)
        {
          int t = TCCIR_DECODE_VREG_TYPE(vr);
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (t == TCCIR_VREG_TYPE_TEMP && p >= 0 && p < NSDR_MAX_TEMP)
          {
            if (temp_zero[p >> 3] & (uint8_t)(1u << (p & 7)))
            {
              ub_store_idx = i;
              break;
            }
          }
          else if (t == TCCIR_VREG_TYPE_VAR && p >= 0 && p < NSDR_MAX_VAR &&
                   !(var_addr_taken[p >> 3] & (uint8_t)(1u << (p & 7))))
          {
            if (var_zero[p >> 3] & (uint8_t)(1u << (p & 7)))
            {
              ub_store_idx = i;
              break;
            }
          }
        }
      }
    }

    /* Apply the write effect of this instruction to known-zero tracking. */
    if (irop_config[q->op].has_dest)
    {
      IROperand dst = tcc_ir_op_get_dest(ir, q);
      /* Deref/stack-address destinations don't define a value vreg. */
      if (!dst.is_lval && !dst.is_local)
      {
        int32_t dvr = irop_get_vreg(dst);
        if (dvr >= 0)
        {
          int dtype = TCCIR_DECODE_VREG_TYPE(dvr);
          int dpos = TCCIR_DECODE_VREG_POSITION(dvr);

          int makes_zero = 0;
          if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_src1)
          {
            IROperand src1 = tcc_ir_op_get_src1(ir, q);
            if (irop_is_immediate(src1) && irop_get_imm64_ex(ir, src1) == 0)
              makes_zero = 1;
            else if (VREG_IS_KNOWN_ZERO(src1))
              makes_zero = 1;
          }

          if (dtype == TCCIR_VREG_TYPE_TEMP && dpos >= 0 && dpos < NSDR_MAX_TEMP)
          {
            if (makes_zero)
              temp_zero[dpos >> 3] |= (uint8_t)(1u << (dpos & 7));
            else
              temp_zero[dpos >> 3] &= (uint8_t)~(1u << (dpos & 7));
          }
          else if (dtype == TCCIR_VREG_TYPE_VAR && dpos >= 0 && dpos < NSDR_MAX_VAR)
          {
            if (makes_zero)
              var_zero[dpos >> 3] |= (uint8_t)(1u << (dpos & 7));
            else
              var_zero[dpos >> 3] &= (uint8_t)~(1u << (dpos & 7));
          }
        }
      }
    }

    /* Stop at any op that ends linear forward flow. */
    if (q->op == TCCIR_OP_JUMP)
      break;
    if (q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_RETURNVALUE)
      break;
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      break;
    if (q->op == TCCIR_OP_SWITCH_TABLE)
      break;
  }

#undef VREG_IS_KNOWN_ZERO
#undef NSDR_MAX_TEMP
#undef NSDR_MAX_VAR

  if (ub_store_idx < 0)
    return 0;

  /* Verify the UB STORE's block dominates every exit (RETURNVOID or CFG-leaf). */
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg || cfg->num_blocks == 0)
  {
    if (cfg)
      tcc_ir_cfg_free(cfg);
    return 0;
  }
  tcc_ir_cfg_compute_dominators(cfg);

  int store_block = cfg->instr_to_block[ub_store_idx];
  int ok = 1;
  /* Check explicit RETURNVOIDs. */
  for (int i = 0; i < n && ok; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op != TCCIR_OP_RETURNVOID)
      continue;
    int ret_block = cfg->instr_to_block[i];
    if (store_block == ret_block)
    {
      if (ub_store_idx >= i)
        ok = 0;
    }
    else if (!tcc_ir_cfg_dominates(cfg, store_block, ret_block))
    {
      ok = 0;
    }
  }
  /* Check CFG-leaf blocks (implicit fall-off exits). */
  for (int b = 0; b < cfg->num_blocks && ok; b++)
  {
    if (cfg->blocks[b].num_succs != 0)
      continue;
    if (b == store_block)
      continue; /* STORE precedes the implicit exit by construction */
    if (!tcc_ir_cfg_dominates(cfg, store_block, b))
      ok = 0;
  }
  tcc_ir_cfg_free(cfg);

  if (!ok)
    return 0;

  LOG_IR_GEN("NULL-STORE-DOM-RETURN: collapsing function body to bx lr "
             "(STORE at i=%d through compile-time NULL dominates all RETURNVOIDs)", ub_store_idx);

  /* NOP everything — codegen emits bare prologue + bx lr. */
  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;

  return 1;
}

int tcc_ir_opt_null_store_dom_return_ex(IROptCtx *ctx) { return tcc_ir_opt_null_store_dom_return(ctx->ir); }
