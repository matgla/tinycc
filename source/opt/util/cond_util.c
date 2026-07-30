/*
 *  TCC IR - Compare-condition token helpers
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_utils.h"

int evaluate_compare_condition(int64_t val1, int64_t val2, int cond_token)
{
  switch (cond_token)
  {
  case 0x94: /* TOK_EQ */
    return val1 == val2;
  case 0x95: /* TOK_NE */
    return val1 != val2;
  case 0x9c: /* TOK_LT */
    return val1 < val2;
  case 0x9d: /* TOK_GE */
    return val1 >= val2;
  case 0x9e: /* TOK_LE */
    return val1 <= val2;
  case 0x9f: /* TOK_GT */
    return val1 > val2;
  case 0x92: /* TOK_ULT */
    return (uint64_t)val1 < (uint64_t)val2;
  case 0x93: /* TOK_UGE */
    return (uint64_t)val1 >= (uint64_t)val2;
  case 0x96: /* TOK_ULE */
    return (uint64_t)val1 <= (uint64_t)val2;
  case 0x97: /* TOK_UGT */
    return (uint64_t)val1 > (uint64_t)val2;
  default:
    return -1;
  }
}

int vrp_negate_cmp_tok(int tok)
{
  switch (tok)
  {
  case TOK_EQ:
    return TOK_NE;
  case TOK_NE:
    return TOK_EQ;
  case TOK_LT:
    return TOK_GE;
  case TOK_GE:
    return TOK_LT;
  case TOK_LE:
    return TOK_GT;
  case TOK_GT:
    return TOK_LE;
  case TOK_ULT:
    return TOK_UGE;
  case TOK_UGE:
    return TOK_ULT;
  case TOK_ULE:
    return TOK_UGT;
  case TOK_UGT:
    return TOK_ULE;
  default:
    return -1;
  }
}

int vrp_swap_cmp_tok(int tok)
{
  switch (tok)
  {
  case TOK_EQ:
    return TOK_EQ;
  case TOK_NE:
    return TOK_NE;
  case TOK_LT:
    return TOK_GT;
  case TOK_GT:
    return TOK_LT;
  case TOK_LE:
    return TOK_GE;
  case TOK_GE:
    return TOK_LE;
  case TOK_ULT:
    return TOK_UGT;
  case TOK_UGT:
    return TOK_ULT;
  case TOK_ULE:
    return TOK_UGE;
  case TOK_UGE:
    return TOK_ULE;
  default:
    return -1;
  }
}

int vrp_cmp_implies(int known_true, int check)
{
  if (known_true == check)
    return 1;
  switch (known_true)
  {
  case TOK_EQ:
    return (check == TOK_LE || check == TOK_GE || check == TOK_ULE || check == TOK_UGE);
  case TOK_LT:
    return (check == TOK_LE || check == TOK_NE);
  case TOK_GT:
    return (check == TOK_GE || check == TOK_NE);
  case TOK_ULT:
    return (check == TOK_ULE || check == TOK_NE);
  case TOK_UGT:
    return (check == TOK_UGE || check == TOK_NE);
  default:
    return 0;
  }
}

int fcmp_cmp_implies(int known_true, int check)
{
  if (known_true == check)
    return 1;

  switch (known_true)
  {
  case TOK_EQ:
    return (check == TOK_LE || check == TOK_GE);
  case TOK_NE:
    return (check == TOK_NE);
  case TOK_LT:
  case TOK_ULT:
    return (check == TOK_LE || check == TOK_NE || check == TOK_ULE);
  case TOK_GT:
  case TOK_UGT:
    return (check == TOK_GE || check == TOK_NE || check == TOK_UGE);
  default:
    return 0;
  }
}

int invert_cond_token(int tok)
{
  switch (tok)
  {
  case 0x94:
    return 0x95; /* EQ -> NE */
  case 0x95:
    return 0x94; /* NE -> EQ */
  case 0x9c:
    return 0x9d; /* LT -> GE */
  case 0x9d:
    return 0x9c; /* GE -> LT */
  case 0x9e:
    return 0x9f; /* LE -> GT */
  case 0x9f:
    return 0x9e; /* GT -> LE */
  case 0x92:
    return 0x93; /* ULT -> UGE */
  case 0x93:
    return 0x92; /* UGE -> ULT */
  case 0x96:
    return 0x97; /* ULE -> UGT */
  case 0x97:
    return 0x96; /* UGT -> ULE */
  default:
    return -1;
  }
}

int invert_condition(int cond)
{
  switch (cond)
  {
  case TOK_GE:
    return TOK_LT;
  case TOK_GT:
    return TOK_LE;
  case TOK_LT:
    return TOK_GE;
  case TOK_LE:
    return TOK_GT;
  case TOK_EQ:
    return TOK_NE;
  case TOK_NE:
    return TOK_EQ;
  case TOK_UGE:
    return TOK_ULT;
  case TOK_UGT:
    return TOK_ULE;
  case TOK_ULT:
    return TOK_UGE;
  case TOK_ULE:
    return TOK_UGT;
  default:
    return -1;
  }
}

int ir_negate_condition(int cond)
{
  return cond ^ 1;
}

/* IEEE NaN compare: ordered=FALSE, NE=TRUE; -1 = don't fold (GT/GE/UGT/UGE disagree with runtime). */
int nan_compare_branch_result(int cond_token)
{
  switch (cond_token)
  {
  case TOK_EQ:
  case TOK_LT:
  case TOK_LE:
  case TOK_ULT:
  case TOK_ULE:
    return 0;
  case TOK_NE:
    return 1;
  default:
    return -1;
  }
}

static int cmp_operand_is_unsigned_int(IROperand op)
{
  int btype = irop_get_btype(op);
  return op.is_unsigned &&
         (irop_get_tag(op) == IROP_TAG_I64 ||
          btype == IROP_BTYPE_INT8 || btype == IROP_BTYPE_INT16 ||
          btype == IROP_BTYPE_INT32 || btype == IROP_BTYPE_INT64);
}

static int cmp_operands_unsigned_width(IROperand src1, IROperand src2)
{
  /* Width is semantic (btype), never storage: IROP_TAG_I64 on a non-INT64
   * operand is just a pooled 32-bit constant that doesn't fit a signed imm32
   * (same convention as fold_read_imm32), and its i64 slot may be sign- OR
   * zero-extended depending on the producing pass. */
  return (irop_get_btype(src1) == IROP_BTYPE_INT64 ||
          irop_get_btype(src2) == IROP_BTYPE_INT64)
             ? 64
             : 32;
}

static int unsigned_cond_for_cmp_operands(int cond, IROperand src1, IROperand src2)
{
  if (!cmp_operand_is_unsigned_int(src1) && !cmp_operand_is_unsigned_int(src2))
    return cond;

  switch (cond)
  {
  case TOK_LT:
    return TOK_ULT;
  case TOK_GE:
    return TOK_UGE;
  case TOK_LE:
    return TOK_ULE;
  case TOK_GT:
    return TOK_UGT;
  default:
    return cond;
  }
}

/* Like evaluate_compare_condition_cmp_operands, but honours a barrel-shift
 * annotation on the CMP: the real comparison is src1 vs (src2 SHIFT #n), so
 * the shift must be applied to val2 before evaluating (encoding: stype=bs>>5,
 * 1=LSL 2=LSR 3=ASR 4=ROR, amount=bs&31 — same as fold_apply_barrel).  Every
 * CMP-fold site must use THIS entry point when it has the CMP quad in hand:
 * evaluating the raw operand values mis-folds (int)(short) narrowing compares
 * whose SHL16/ASR16 got fused into the CMP (fuzz seeds signed:840 in
 * branch.c, signed:8890 in const_prop_tmp/value_tracking; tests 386/393). */
int evaluate_compare_condition_cmp_annotated(const TCCIRState *ir, const IRQuadCompact *q,
                                             int64_t val1, int64_t val2, int cond,
                                             IROperand src1, IROperand src2)
{
  uint8_t bs = tcc_ir_barrel_shift_at(ir, q);
  if (bs) {
    if (irop_is_64bit(src1) || irop_is_64bit(src2))
      return -1;
    uint32_t m = (uint32_t)val2;
    int amount = bs & 0x1F;
    switch (bs >> 5) {
    case 1: m = m << amount; break;
    case 2: m = m >> amount; break;
    case 3: m = (m >> amount) |
                (((m & 0x80000000u) && amount) ? ~(0xFFFFFFFFu >> amount) : 0u);
      break;
    case 4: m = amount ? ((m >> amount) | (m << (32 - amount))) : m; break;
    default: return -1;
    }
    val2 = (int64_t)(int32_t)m;
  }
  return evaluate_compare_condition_cmp_operands(val1, val2, cond, src1, src2);
}

int evaluate_compare_condition_cmp_operands(int64_t val1, int64_t val2, int cond,
                                            IROperand src1, IROperand src2)
{
  cond = unsigned_cond_for_cmp_operands(cond, src1, src2);
  if (cmp_operands_unsigned_width(src1, src2) != 64)
  {
    int32_t s1 = (int32_t)(uint32_t)val1;
    int32_t s2 = (int32_t)(uint32_t)val2;
    switch (cond)
    {
    case TOK_EQ:
      return (uint32_t)val1 == (uint32_t)val2;
    case TOK_NE:
      return (uint32_t)val1 != (uint32_t)val2;
    case TOK_LT:
      return s1 < s2;
    case TOK_GE:
      return s1 >= s2;
    case TOK_LE:
      return s1 <= s2;
    case TOK_GT:
      return s1 > s2;
    default:
      break;
    }
  }
  switch (cond)
  {
  case TOK_ULT:
  case TOK_UGE:
  case TOK_ULE:
  case TOK_UGT:
  {
    if (cmp_operands_unsigned_width(src1, src2) == 64)
      return evaluate_compare_condition(val1, val2, cond);
    uint32_t u1 = (uint32_t)val1;
    uint32_t u2 = (uint32_t)val2;
    switch (cond)
    {
    case TOK_ULT:
      return u1 < u2;
    case TOK_UGE:
      return u1 >= u2;
    case TOK_ULE:
      return u1 <= u2;
    case TOK_UGT:
      return u1 > u2;
    default:
      break;
    }
  }
  default:
    return evaluate_compare_condition(val1, val2, cond);
  }
}
