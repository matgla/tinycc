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



/* Opcode hazards a pure body may have: control flow, memory reads, flags and
 * the parameters of a call the CALL case vets.  The saved-LR read of
 * __builtin_return_address is not one: inlined, it would name the caller's. */
#define PURE_VIA_SRET_OP_HZ                                                                                            \
  (IR_HZ_BRANCH | IR_HZ_RETURN | IR_HZ_MEM_READ | IR_HZ_UPDATES_SRC | IR_HZ_FLAGS_SET | IR_HZ_FLAGS_READ |            \
   IR_HZ_CALL_PARAM)

void tcc_ir_analyze_pure_via_sret(TCCIRState *ir, Sym *func_sym)
{
  if (!ir || !func_sym)
    return;
  if (func_sym->a.pure_via_sret)
    return;
  /* A weak body may be replaced by one with side effects. */
  if (func_sym->a.weak)
    return;

  /* Non-zero func_vc is the local slot holding the spilled sret pointer --
   * not a parameter when the result is returned in s0-s7.  An aggregate
   * returned in r0/r1 (gfunc_regpair_ret) has no buffer: only frame stores
   * are then allowed, and the class is the same. */
  const int has_sret = func_vc != 0 && !ir->vfp_ret_words;
  if (!has_sret && !(func_vc == 0 && gfunc_regpair_ret(func_sym)))
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

  if (has_sret)
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

      int32_t dest_vr = tcc_ir_op_dest_vreg(ir, q);
      if (dest_vr < 0)
        continue;
      if (SRET_TEST(dest_vr))
        continue;

      if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN)
      {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        if (has_sret && src.is_local && irop_get_tag(src) == IROP_TAG_STACKOFF &&
            (int32_t)irop_get_stack_offset(src) == sret_slot)
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
        IROperand src2 = tcc_ir_op_get_src2(ir, q);
        int32_t src1_vr = tcc_ir_op_src1_vreg(ir, q);
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
    if (tcc_ir_instr_access_is_volatile(ir, q))
    {
      pure = 0;
      break;
    }
    switch (q->op)
    {
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
      Sym *callee = tcc_ir_op_src1_sym(ir, q);
      if (!callee)
      {
        pure = 0;
        break;
      }
      int callee_pure = callee->f.func_pure | callee->f.func_const | callee->a.pure_via_sret;
      if (callee->type.ref)
        callee_pure |= callee->type.ref->f.func_pure | callee->type.ref->f.func_const;
      if (callee_pure)
        break;
      const char *name = get_tok_str(callee->v, NULL);
      if (name && tcc_ir_is_pure_aeabi(name))
        break;
      pure = 0;
      break;
    }

    default:
    {
      /* Any other op computes, reads memory or branches -- or it is a side
       * effect (asm, setjmp, a VLA, a trap, ...).  Asking op_props keeps the
       * bit and count ops (clz/rbit of a ctz, ubfx, setif) that a list of
       * names missed: the kernel's next_bit scans were never pure. */
      if (ir_op_has(q->op, IR_HZ_FROM_OP & ~PURE_VIA_SRET_OP_HZ) || q->op == TCCIR_OP_RETURN_ADDRESS)
      {
        pure = 0;
        break;
      }
      /* An lvalue destination stores through it, as a STORE would. */
      if (irop_config[q->op].has_dest)
      {
        IROperand dst = tcc_ir_op_get_dest(ir, q);
        int32_t dest_vr = irop_get_vreg(dst);
        if (dst.is_lval && !(dst.is_local && irop_get_tag(dst) == IROP_TAG_STACKOFF) &&
            !(dest_vr >= 0 && SRET_TEST(dest_vr)))
          pure = 0;
      }
      break;
    }
    }
  }

  tcc_free(is_sret_derived);

  if (pure)
  {
    func_sym->a.pure_via_sret = 1;
    LOG_IR_GEN("=== PURE_VIA_SRET: function marked pure (sret_slot=%d) ===", (int)sret_slot);
  }

#undef SRET_VR_TO_BIT
#undef SRET_MARK
#undef SRET_TEST
}
