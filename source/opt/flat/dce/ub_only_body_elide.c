/*
 *  TCC IR - UB-only body elide pass
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

static int ub_elide_get_operand(TCCIRState *ir, IRQuadCompact *q, int k, IROperand *out)
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

int tcc_ir_opt_ub_only_body_elide(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* First pass: inventory STOREs and bail on ops that block whole-function elision. */
  int has_store = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    /* Externally observable / unmodellable — can't elide. */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCPARAMVOID:
    case TCCIR_OP_CALLSEQ_BEGIN:
    case TCCIR_OP_CALLARG_REG:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_CALLSEQ_END:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_RETURNVALUE: /* non-void return: can't drop the return value */
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
    case TCCIR_OP_PREFETCH:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      return 0;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
      has_store = 1;
      break;
    default:
      break;
    }

    /* Volatile sym access on any operand keeps the body alive. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (!ub_elide_get_operand(ir, q, k, &op))
        continue;
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }

  /* No STOREs → nothing to elide. */
  if (!has_store)
    return 0;

#define UB_ELIDE_MAX_VAR_POS 1024
#define UB_ELIDE_MAX_TEMPS 8192
#define UB_ELIDE_MAX_STACK_OFFS 256
  uint8_t var_written[(UB_ELIDE_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t var_addr_taken[(UB_ELIDE_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t temp_tainted[(UB_ELIDE_MAX_TEMPS + 7) / 8] = {0};
  /* Stack slots directly written or address-taken; reads of slots not here are uninit. */
  int32_t stack_blocked_offs[UB_ELIDE_MAX_STACK_OFFS];
  int stack_blocked_count = 0;
  int stack_blocked_overflow = 0;

  /* Inventory VAR writes and address-takes. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (!ub_elide_get_operand(ir, q, k, &op))
        continue;
      /* Bare STACKOFF operand touched other than by a pure read: treat slot as initialised. */
      if (!stack_blocked_overflow && irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && irop_get_vreg(op) == -1)
      {
        int block = 0;
        if (!op.is_lval)
          block = 1; /* Addr[StackLoc[X]] — pointer could be used to write */
        else if (k == 0)
          block = 1; /* dest with is_lval=1 — direct write to slot */
        if (block)
        {
          int32_t off = irop_get_stack_offset(op);
          int found = 0;
          for (int s = 0; s < stack_blocked_count; s++)
            if (stack_blocked_offs[s] == off)
            {
              found = 1;
              break;
            }
          if (!found)
          {
            if (stack_blocked_count >= UB_ELIDE_MAX_STACK_OFFS)
              stack_blocked_overflow = 1;
            else
              stack_blocked_offs[stack_blocked_count++] = off;
          }
        }
      }
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      if (op.is_local && !op.is_lval)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UB_ELIDE_MAX_VAR_POS)
          var_addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand op = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UB_ELIDE_MAX_VAR_POS)
          var_addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
    /* Any VAR appearing as dest is "written" at some point. */
    if (irop_config[q->op].has_dest)
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        /* Out-of-range position: skip; its reads won't be classed uninit either. Safe. */
        if (pos >= 0 && pos < UB_ELIDE_MAX_VAR_POS)
          var_written[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
  }

  /* Classify a source operand as tainted (reads an uninit VAR or uninit stack slot). */
#define SRC_IS_UNINIT_READ(sop) ({                                                                                     \
    int _t = 0;                                                                                                        \
    int32_t _vr = irop_get_vreg(sop);                                                                                  \
    if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_VAR && !((sop).is_local && !(sop).is_lval))         \
    {                                                                                                                  \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                        \
      if (_p >= 0 && _p < UB_ELIDE_MAX_VAR_POS &&                                                                      \
          !(var_written[_p >> 3] & (uint8_t)(1u << (_p & 7))) &&                                                       \
          !(var_addr_taken[_p >> 3] & (uint8_t)(1u << (_p & 7))))                                                      \
        _t = 1;                                                                                                        \
    }                                                                                                                  \
    /* Bare stack-slot read (StackLoc[X] as a value source, vreg=-1). */                                               \
    if (!_t && !stack_blocked_overflow && irop_get_tag(sop) == IROP_TAG_STACKOFF &&                                    \
        (sop).is_local && (sop).is_lval && irop_get_vreg(sop) == -1)                                                   \
    {                                                                                                                  \
      int32_t _off = irop_get_stack_offset(sop);                                                                       \
      int _blocked = 0;                                                                                                \
      for (int _s = 0; _s < stack_blocked_count; _s++)                                                                 \
        if (stack_blocked_offs[_s] == _off)                                                                            \
        {                                                                                                              \
          _blocked = 1;                                                                                                \
          break;                                                                                                       \
        }                                                                                                              \
      if (!_blocked)                                                                                                   \
        _t = 1;                                                                                                        \
    }                                                                                                                  \
    _t;                                                                                                                \
  })

#define TEMP_IS_TAINTED(sop) ({                                                                                        \
    int _t = 0;                                                                                                        \
    int32_t _vr = irop_get_vreg(sop);                                                                                  \
    if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_TEMP)                                               \
    {                                                                                                                  \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                        \
      if (_p >= 0 && _p < UB_ELIDE_MAX_TEMPS &&                                                                        \
          (temp_tainted[_p >> 3] & (uint8_t)(1u << (_p & 7))))                                                         \
        _t = 1;                                                                                                        \
    }                                                                                                                  \
    _t;                                                                                                                \
  })

  /* Forward fixpoint: propagate taint through TEMP defs. */
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
      if (dvr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      /* dest with is_lval is an address, not a new TEMP value — skip. */
      if (dop.is_lval)
        continue;
      int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (dpos < 0 || dpos >= UB_ELIDE_MAX_TEMPS)
        continue;
      if (temp_tainted[dpos >> 3] & (uint8_t)(1u << (dpos & 7)))
        continue;

      int taint = 0;
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (SRC_IS_UNINIT_READ(s) || TEMP_IS_TAINTED(s))
          taint = 1;
      }
      if (!taint && irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        if (SRC_IS_UNINIT_READ(s) || TEMP_IS_TAINTED(s))
          taint = 1;
      }

      if (taint)
      {
        temp_tainted[dpos >> 3] |= (uint8_t)(1u << (dpos & 7));
        changed = 1;
      }
    }
    if (!changed)
      break;
  }

  /* Verify every STORE has a tainted address. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
      continue;

    /* Address operand: dest for all three STORE forms (the base pointer). */
    IROperand dop = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dop);
    int tainted = 0;

    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      if (TEMP_IS_TAINTED(dop))
        tainted = 1;
    }
    else if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && dop.is_lval && !dop.is_local)
    {
      /* Store-through a non-local VAR slot: uninit VAR means garbage address. */
      int p = TCCIR_DECODE_VREG_POSITION(dvr);
      if (p >= 0 && p < UB_ELIDE_MAX_VAR_POS &&
          !(var_written[p >> 3] & (uint8_t)(1u << (p & 7))) &&
          !(var_addr_taken[p >> 3] & (uint8_t)(1u << (p & 7))))
        tainted = 1;
    }

    if (!tainted)
      return 0; /* a STORE has a real, well-defined address — can't elide */
  }

#undef SRC_IS_UNINIT_READ
#undef TEMP_IS_TAINTED
#undef UB_ELIDE_MAX_VAR_POS
#undef UB_ELIDE_MAX_TEMPS
#undef UB_ELIDE_MAX_STACK_OFFS

  LOG_IR_GEN("UB-ELIDE: collapsing function body to empty "
             "(every STORE goes through uninit-pointer address — whole-function UB)");

  /* NOP everything — codegen emits a bare prologue + bx lr. */
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

int tcc_ir_opt_ub_only_body_elide_ex(IROptCtx *ctx) { return tcc_ir_opt_ub_only_body_elide(ctx->ir); }
