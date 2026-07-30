/*
 *  TCC IR - sret-slot purity analysis (flat, pre-SSA)
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
#include "opt_utils.h"



void tcc_ir_analyze_pure_via_sret(TCCIRState *ir, Sym *func_sym)
{
  if (!ir || !func_sym)
    return;
  if (func_sym->f.func_pure_via_sret)
    return;

  /* Non-zero func_vc is the local slot holding the spilled sret pointer. */
  if (func_vc == 0)
    return;

  /* P0 is seeded unconditionally: the prolog store to the slot may already be forwarded away. */
  int32_t sret_param_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0);
  int32_t sret_slot = (int32_t)func_vc;

  const int n = ir->next_instruction_index;

  int max_vreg = ir->next_temporary_variable + ir->next_local_variable + ir->next_parameter + 16;
  uint8_t *is_sret_derived = tcc_mallocz((size_t)((max_vreg + 7) / 8));
  if (!is_sret_derived)
    return;

#define SRET_VR_TO_BIT(vr)                                                                                             \
  ({                                                                                                                   \
    int _t = TCCIR_DECODE_VREG_TYPE(vr);                                                                               \
    int _p = TCCIR_DECODE_VREG_POSITION(vr);                                                                           \
    int _b = -1;                                                                                                       \
    if (_t == TCCIR_VREG_TYPE_TEMP)                                                                                    \
      _b = _p;                                                                                                         \
    else if (_t == TCCIR_VREG_TYPE_VAR)                                                                                \
      _b = ir->next_temporary_variable + _p;                                                                           \
    else if (_t == TCCIR_VREG_TYPE_PARAM)                                                                              \
      _b = ir->next_temporary_variable + ir->next_local_variable + _p;                                                 \
    _b;                                                                                                                \
  })
#define SRET_MARK(vr)                                                                                                  \
  do                                                                                                                   \
  {                                                                                                                    \
    int _b = SRET_VR_TO_BIT(vr);                                                                                       \
    if (_b >= 0 && _b < max_vreg)                                                                                      \
      is_sret_derived[_b / 8] |= (uint8_t)(1u << (_b % 8));                                                            \
  } while (0)
#define SRET_TEST(vr)                                                                                                  \
  ({                                                                                                                   \
    int _b = SRET_VR_TO_BIT(vr);                                                                                       \
    int _r = (_b >= 0 && _b < max_vreg) ? ((is_sret_derived[_b / 8] >> (_b % 8)) & 1u) : 0;                            \
    _r;                                                                                                                \
  })

  SRET_MARK(sret_param_vr);

  int changes = 1;
  while (changes)
  {
    changes = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[q->op].has_dest)
        continue;

      IROperand dst = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dst);
      if (dest_vr < 0)
        continue;
      if (SRET_TEST(dest_vr))
        continue;

      if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN)
      {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        if (src.is_local && irop_get_tag(src) == IROP_TAG_STACKOFF && (int32_t)irop_get_stack_offset(src) == sret_slot)
        {
          SRET_MARK(dest_vr);
          changes++;
          continue;
        }
        int32_t src_vr = irop_get_vreg(src);
        if (src_vr >= 0 && SRET_TEST(src_vr))
        {
          SRET_MARK(dest_vr);
          changes++;
          continue;
        }
      }
      else if (q->op == TCCIR_OP_ADD)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        IROperand src2 = tcc_ir_op_get_src2(ir, q);
        int32_t src1_vr = irop_get_vreg(src1);
        if (src1_vr >= 0 && SRET_TEST(src1_vr) && irop_is_immediate(src2) && !src2.is_sym)
        {
          SRET_MARK(dest_vr);
          changes++;
        }
      }
    }
  }

  int pure = 1;
  for (int i = 0; i < n && pure; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
    case TCCIR_OP_NOP:
    case TCCIR_OP_LOAD:
    case TCCIR_OP_LOAD_INDEXED:
    case TCCIR_OP_LOAD_POSTINC:
    case TCCIR_OP_LEA:
    case TCCIR_OP_ASSIGN:
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    case TCCIR_OP_MUL:
    case TCCIR_OP_DIV:
    case TCCIR_OP_IMOD:
    case TCCIR_OP_UMOD:
    case TCCIR_OP_AND:
    case TCCIR_OP_OR:
    case TCCIR_OP_XOR:
    case TCCIR_OP_SHL:
    case TCCIR_OP_SHR:
    case TCCIR_OP_SAR:
    case TCCIR_OP_CMP:
    case TCCIR_OP_FCMP:
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCPARAMVOID:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_SELECT:
      break;

    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    {
      IROperand dst = tcc_ir_op_get_dest(ir, q);
      /* Local stack slots are not observable to the caller. */
      if (dst.is_local && irop_get_tag(dst) == IROP_TAG_STACKOFF)
        break;
      /* A store through an sret-derived pointer is the return value, not a side effect. */
      int32_t dest_vr = irop_get_vreg(dst);
      if (dest_vr >= 0 && SRET_TEST(dest_vr))
        break;
      pure = 0;
      break;
    }

    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!callee)
      {
        pure = 0;
        break;
      }
      int callee_pure = callee->f.func_pure | callee->f.func_const | callee->f.func_pure_via_sret;
      if (callee->type.ref)
        callee_pure |=
            callee->type.ref->f.func_pure | callee->type.ref->f.func_const | callee->type.ref->f.func_pure_via_sret;
      if (callee_pure)
        break;
      const char *name = get_tok_str(callee->v, NULL);
      if (name && tcc_ir_is_pure_aeabi(name))
        break;
      pure = 0;
      break;
    }

    default:
      /* Unknown or side-effecting op (INLINE_ASM, SETJMP, VLA_ALLOC, ...): bail. */
      pure = 0;
      break;
    }
  }

  tcc_free(is_sret_derived);

  if (pure)
  {
    func_sym->f.func_pure_via_sret = 1;
    LOG_IR_GEN("=== PURE_VIA_SRET: function marked pure (sret_slot=%d) ===", (int)sret_slot);
  }

#undef SRET_VR_TO_BIT
#undef SRET_MARK
#undef SRET_TEST
}
