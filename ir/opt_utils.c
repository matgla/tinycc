/*
 *  TCC IR - Shared optimization utilities (pre-SSA)
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

/* Forward declaration for mutual recursion */
static int ir_opt_pure_expr_equal_impl(TCCIRState *ir, IROperand a, int a_use_idx,
                                       IROperand b, int b_use_idx, int depth);

/* ============================================================================
 * Constant evaluators
 * ============================================================================ */

int is_power_of_2(int64_t n)
{
  if (n <= 0)
    return -1;
  if ((n & (n - 1)) != 0)
    return -1;
  int log = 0;
  while (n > 1)
  {
    n >>= 1;
    log++;
  }
  return log;
}

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
  case 0x92: /* TOK_ULT (unsigned <) */
    return (uint64_t)val1 < (uint64_t)val2;
  case 0x93: /* TOK_UGE (unsigned >=) */
    return (uint64_t)val1 >= (uint64_t)val2;
  case 0x96: /* TOK_ULE (unsigned <=) */
    return (uint64_t)val1 <= (uint64_t)val2;
  case 0x97: /* TOK_UGT (unsigned >) */
    return (uint64_t)val1 > (uint64_t)val2;
  default:
    return -1;
  }
}

int ir_opt_eval_const_u64(TCCIRState *ir, IROperand op, int use_idx, uint64_t *out, int depth)
{
  int32_t vr;
  int def_idx;
  IRQuadCompact *q;

  if (!ir || !out || depth > 12)
    return 0;

  if (irop_is_immediate(op))
  {
    *out = (uint64_t)irop_get_imm64_ex(ir, op);
    return 1;
  }

  vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  if (ir_opt_vreg_address_taken_between(ir, vr, 0, use_idx))
    return 0;

  /* Only trace vregs with exactly one definition.  tcc_ir_find_defining_instruction
   * returns the linearly-preceding def, but a multi-def vreg (e.g. a loop-carried
   * value `m = m << 1` whose other def `m = a - b` precedes the use) can be reached
   * at `use_idx` by a DIFFERENT definition via a control-flow/back-edge that the
   * linear scan never sees.  Evaluating the one preceding def as if it were the only
   * reaching value is unsound — it folded `(result_mant & (1ULL<<52))` in a soft-float
   * normalize loop to 0, deleting the loop's exit test.  Mirror the same single-def
   * guard ir_opt_eval_const_string already uses. */
  if (!tcc_ir_vreg_has_single_def(ir, vr))
    return 0;

  def_idx = tcc_ir_find_defining_instruction(ir, vr, use_idx);
  if (def_idx < 0)
    return 0;

  q = &ir->compact_instructions[def_idx];
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1);
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_ROR:
  {
    uint64_t v1, v2;
    if (!ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &v1, depth + 1))
      return 0;
    if (!ir_opt_eval_const_u64(ir, tcc_ir_op_get_src2(ir, q), def_idx, &v2, depth + 1))
      return 0;
    switch (q->op)
    {
    case TCCIR_OP_ADD:
      *out = v1 + v2;
      break;
    case TCCIR_OP_SUB:
      *out = v1 - v2;
      break;
    case TCCIR_OP_MUL:
      *out = v1 * v2;
      break;
    case TCCIR_OP_AND:
      *out = v1 & v2;
      break;
    case TCCIR_OP_OR:
      *out = v1 | v2;
      break;
    case TCCIR_OP_XOR:
      *out = v1 ^ v2;
      break;
    case TCCIR_OP_SHL:
      *out = v1 << v2;
      break;
    case TCCIR_OP_SHR:
      *out = v1 >> v2;
      break;
    case TCCIR_OP_SAR:
      *out = (uint64_t)((int64_t)v1 >> v2);
      break;
    case TCCIR_OP_ROR:
    {
      uint32_t v = (uint32_t)v1;
      uint32_t n = (uint32_t)v2 & 31;
      *out = (v >> n) | (v << (32 - n));
      break;
    }
    default:
      return 0;
    }
    return 1;
  }
  case TCCIR_OP_ZEXT:
  {
    /* Zero-extend src1 to the destination width.  Mask the recursively
     * evaluated source value by its declared narrower width.  Used by
     * the CMP+SETIF folder so trace chains that flow through a sign-/
     * zero-extension idiom (often produced by signed→unsigned casts)
     * remain evaluable instead of bailing at this opcode. */
    uint64_t v;
    if (!ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &v, depth + 1))
      return 0;
    IROperand sop = tcc_ir_op_get_src1(ir, q);
    int sb = irop_get_btype(sop);
    uint64_t mask;
    switch (sb)
    {
    case IROP_BTYPE_INT8:  mask = 0xFFULL; break;
    case IROP_BTYPE_INT16: mask = 0xFFFFULL; break;
    case IROP_BTYPE_INT32: mask = 0xFFFFFFFFULL; break;
    default:               mask = ~0ULL; break;
    }
    *out = v & mask;
    return 1;
  }
  default:
    return 0;
  }
}

int ir_opt_eval_const_string(TCCIRState *ir, IROperand op, int use_idx, const char **out, int depth)
{
  const char *base;
  int32_t vr;
  int def_idx;
  IRQuadCompact *q;

  if (!ir || !out || depth > 16)
    return 0;

  if (op.is_lval && op.vreg_type == TCCIR_VREG_TYPE_TEMP)
    return 0;

  base = ir_opt_get_constant_string_from_symref(ir, op);
  if (base)
  {
    *out = base;
    return 1;
  }

  vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  if (ir_opt_vreg_address_taken_between(ir, vr, 0, use_idx))
    return 0;

  if (!tcc_ir_vreg_has_single_def(ir, vr))
    return 0;

  def_idx = tcc_ir_find_defining_instruction(ir, vr, use_idx);
  if (def_idx < 0)
    return 0;

  q = &ir->compact_instructions[def_idx];
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return ir_opt_eval_const_string(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1);
  case TCCIR_OP_ADD:
  {
    uint64_t addend;
    if (ir_opt_eval_const_string(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1) &&
        ir_opt_eval_const_u64(ir, tcc_ir_op_get_src2(ir, q), def_idx, &addend, depth + 1))
    {
      *out += addend;
      return 1;
    }
    if (ir_opt_eval_const_string(ir, tcc_ir_op_get_src2(ir, q), def_idx, out, depth + 1) &&
        ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &addend, depth + 1))
    {
      *out += addend;
      return 1;
    }
    return 0;
  }
  default:
    return 0;
  }
}

/* ============================================================================
 * Condition token helpers
 * ============================================================================ */

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

/* ============================================================================
 * BB / CFG helpers
 * ============================================================================ */

uint8_t *ir_opt_build_merge_bitmap(TCCIRState *ir, int n)
{
  uint8_t *is_merge = tcc_mallocz((n + 7) / 8);
  int *pred_count = tcc_mallocz(n * sizeof(int));

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)dest.u.imm32;
      if (target >= 0 && target < n)
      {
        pred_count[target]++;
        if (i > target)
          is_merge[target / 8] |= (1 << (target % 8));
      }
    }
    /* NOP is NOT a terminator — it falls through.  Counting its fall-through
     * edge is required so a merge whose preceding block ends in DCE-left NOP
     * padding is still detected (pred_count >= 2).  Omitting it leaves stale
     * per-block state alive across the merge in the passes that consume this
     * bitmap (matches the value_tracking fix in opt_constprop.c). */
    if (i + 1 < n && q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_RETURNVALUE &&
        q->op != TCCIR_OP_RETURNVOID && q->op != TCCIR_OP_SWITCH_TABLE)
    {
      pred_count[i + 1]++;
    }
  }

  for (int i = 0; i < n; i++)
  {
    if (pred_count[i] > 1)
      is_merge[i / 8] |= (1 << (i % 8));
  }

  tcc_free(pred_count);
  return is_merge;
}

void ir_opt_mark_block_starts(TCCIRState *ir, int *block_start_seen, int gen, int n)
{
  block_start_seen[0] = gen;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      const int tgt = (int)irop_get_imm64_ex(ir, dest);
      if (tgt >= 0 && tgt < n)
        block_start_seen[tgt] = gen;
    }
  }
}

uint8_t *ir_opt_build_block_starts_bitmap(TCCIRState *ir, int n)
{
  uint8_t *bs = tcc_mallocz((n + 7) / 8);
  bs[0] |= 1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      const int tgt = (int)irop_get_imm64_ex(ir, dest);
      if (tgt >= 0 && tgt < n)
        bs[tgt / 8] |= (1 << (tgt % 8));
      if (i + 1 < n)
        bs[(i + 1) / 8] |= (1 << ((i + 1) % 8));
    }
  }
  return bs;
}

int ir_opt_next_non_nop(TCCIRState *ir, int start)
{
  int n = ir->next_instruction_index;
  for (int i = start; i < n; ++i)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      return i;
  }
  return -1;
}

int ir_skip_nops_forward(TCCIRState *ir, int start, int n)
{
  for (int j = start; j < n; j++)
    if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
      return j;
  return n;
}

int ir_has_other_jump_to_fast(TCCIRState *ir, const int *jt_cnt,
                              int target, int exclude_idx)
{
  int n = ir->next_instruction_index;
  if (target < 0 || target >= n) return 0;
  int total = jt_cnt[target];
  if (total == 0) return 0;
  if (exclude_idx >= 0 && exclude_idx < n) {
    IRQuadCompact *q = &ir->compact_instructions[exclude_idx];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if ((int)irop_get_imm64_ex(ir, d) == target) total--;
    }
  }
  return total > 0;
}

/* ============================================================================
 * Purity tables
 * ============================================================================ */

int tcc_ir_is_pure_aeabi(const char *name)
{
  if (!name || name[0] != '_' || name[1] != '_')
    return 0;
  /* 64-bit integer comparisons */
  if (strcmp(name, "__aeabi_lcmp") == 0 || strcmp(name, "__aeabi_ulcmp") == 0)
    return 1;
  /* 64-bit integer arithmetic */
  if (strcmp(name, "__aeabi_lmul") == 0 || strcmp(name, "__aeabi_ldivmod") == 0 ||
      strcmp(name, "__aeabi_uldivmod") == 0)
    return 1;
  /* 64-bit shifts */
  if (strcmp(name, "__aeabi_llsl") == 0 || strcmp(name, "__aeabi_llsr") == 0 || strcmp(name, "__aeabi_lasr") == 0)
    return 1;
  /* Soft-float arithmetic */
  if (strcmp(name, "__aeabi_dadd") == 0 || strcmp(name, "__aeabi_dsub") == 0 || strcmp(name, "__aeabi_dmul") == 0 ||
      strcmp(name, "__aeabi_ddiv") == 0 || strcmp(name, "__aeabi_fadd") == 0 || strcmp(name, "__aeabi_fsub") == 0 ||
      strcmp(name, "__aeabi_fmul") == 0 || strcmp(name, "__aeabi_fdiv") == 0)
    return 1;
  /* Soft-float comparisons */
  if (strcmp(name, "__aeabi_dcmpeq") == 0 || strcmp(name, "__aeabi_dcmplt") == 0 ||
      strcmp(name, "__aeabi_dcmple") == 0 || strcmp(name, "__aeabi_dcmpge") == 0 ||
      strcmp(name, "__aeabi_dcmpgt") == 0 || strcmp(name, "__aeabi_dcmpun") == 0 ||
      strcmp(name, "__aeabi_fcmpeq") == 0 || strcmp(name, "__aeabi_fcmplt") == 0 ||
      strcmp(name, "__aeabi_fcmple") == 0 || strcmp(name, "__aeabi_fcmpge") == 0 ||
      strcmp(name, "__aeabi_fcmpgt") == 0 || strcmp(name, "__aeabi_fcmpun") == 0)
    return 1;
  /* Soft-float conversions */
  if (strcmp(name, "__aeabi_f2d") == 0 || strcmp(name, "__aeabi_d2f") == 0 || strcmp(name, "__aeabi_i2d") == 0 ||
      strcmp(name, "__aeabi_i2f") == 0 || strcmp(name, "__aeabi_ui2d") == 0 || strcmp(name, "__aeabi_ui2f") == 0 ||
      strcmp(name, "__aeabi_d2iz") == 0 || strcmp(name, "__aeabi_d2uiz") == 0 || strcmp(name, "__aeabi_f2iz") == 0 ||
      strcmp(name, "__aeabi_f2uiz") == 0 || strcmp(name, "__aeabi_l2d") == 0 || strcmp(name, "__aeabi_l2f") == 0 ||
      strcmp(name, "__aeabi_ul2d") == 0 || strcmp(name, "__aeabi_ul2f") == 0 || strcmp(name, "__aeabi_d2lz") == 0 ||
      strcmp(name, "__aeabi_d2ulz") == 0 || strcmp(name, "__aeabi_f2lz") == 0 || strcmp(name, "__aeabi_f2ulz") == 0)
    return 1;
  /* Byte swap helpers */
  if (strcmp(name, "__bswapsi2") == 0 || strcmp(name, "__bswapdi3") == 0)
    return 1;
  return 0;
}

int ir_opt_is_pure_helper_name(const char *name)
{
  if (!name)
    return 0;

  return strcmp(name, "isnan") == 0 || strcmp(name, "__isnan") == 0 || strcmp(name, "__isnanf") == 0 ||
         strcmp(name, "__aeabi_f2d") == 0 || strcmp(name, "__aeabi_d2f") == 0;
}

/* Read-only libc string helpers emitted by the front end for __builtin_str*
 * calls (see redirect_call_to_tcc_helper in tccgen.c).  These return their
 * result by value in a register and only *read* memory through their pointer
 * arguments — they have no observable side effect — so a call whose result is
 * unused is dead and can be removed.
 *
 * Unlike ir_opt_is_pure_helper_name these are "pure" (read memory) rather than
 * "const" (touch no memory): two calls with identical pointer arguments are
 * NOT interchangeable if memory changed between them.  They must therefore
 * only be used to justify dead-result elimination, never value-numbering /
 * CSE of two separate calls. */
int ir_opt_is_readonly_str_helper_name(const char *name)
{
  if (!name)
    return 0;

  return strcmp(name, "__tcc_strcmp") == 0 || strcmp(name, "__tcc_strncmp") == 0 ||
         strcmp(name, "__tcc_strlen") == 0 || strcmp(name, "__tcc_strnlen") == 0 ||
         strcmp(name, "__tcc_strchr") == 0 || strcmp(name, "__tcc_strrchr") == 0 ||
         strcmp(name, "__tcc_strpbrk") == 0 || strcmp(name, "__tcc_strstr") == 0 ||
         strcmp(name, "__tcc_strcspn") == 0;
}

int ir_opt_is_flag_cmp_helper_name(const char *name)
{
  if (!name)
    return 0;

  return strcmp(name, "__aeabi_cfcmple") == 0 || strcmp(name, "__aeabi_cdcmple") == 0;
}

int ir_opt_is_pure_fallthrough_instruction(TCCIRState *ir, int idx)
{
  IRQuadCompact *q;
  Sym *callee;
  const char *name;

  if (!ir || idx < 0 || idx >= ir->next_instruction_index)
    return 0;

  q = &ir->compact_instructions[idx];
  switch (q->op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_OR:
  case TCCIR_OP_AND:
  case TCCIR_OP_XOR:
  case TCCIR_OP_BOOL_OR:
  case TCCIR_OP_BOOL_AND:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
    return 1;
  case TCCIR_OP_FUNCCALLVAL:
    callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      return 0;
    name = get_tok_str(callee->v, NULL);
    return ir_opt_is_pure_helper_name(name);
  default:
    return 0;
  }
}

/* ============================================================================
 * Expression equality
 * ============================================================================ */

int ir_opt_nonvreg_expr_equal(TCCIRState *ir, IROperand a, IROperand b)
{
  int a_tag = irop_get_tag(a);
  int b_tag = irop_get_tag(b);

  if (a_tag != b_tag)
    return 0;

  if (a_tag == IROP_TAG_STACKOFF)
  {
    int32_t a_vr = irop_get_vreg(a);
    int32_t b_vr = irop_get_vreg(b);
    /* Two STACKOFF operands refer to the same slot when they share the same
     * vreg identity (a_vr == b_vr, including both being anonymous at -1) AND
     * the same offset/access attributes. */
    if (a_vr == b_vr && a.u.imm32 == b.u.imm32 && a.is_lval == b.is_lval && a.is_local == b.is_local &&
        a.is_llocal == b.is_llocal && a.is_param == b.is_param && irop_get_btype(a) == irop_get_btype(b))
      return 1;
    return 0;
  }

  if (a_tag != IROP_TAG_SYMREF)
    return 0;

  if (a.is_lval != b.is_lval || a.is_llocal != b.is_llocal || a.is_local != b.is_local || a.is_const != b.is_const ||
      a.is_unsigned != b.is_unsigned || a.is_static != b.is_static || a.is_sym != b.is_sym ||
      a.is_param != b.is_param || a.is_complex != b.is_complex || irop_get_btype(a) != irop_get_btype(b))
  {
    return 0;
  }

  {
    IRPoolSymref *a_ref = irop_get_symref_ex(ir, a);
    IRPoolSymref *b_ref = irop_get_symref_ex(ir, b);

    if (!a_ref || !b_ref)
      return 0;

    return a_ref->sym == b_ref->sym && a_ref->addend == b_ref->addend && a_ref->flags == b_ref->flags;
  }
}

/* Helper for the SETIF case of ir_opt_pure_def_equal: decide whether two CMP
 * operands evaluate to the same value at their respective CMP sites.  The
 * caller has already verified there are no memory-changing or jump-target
 * instructions between the two CMPs, so we can treat structurally-identical
 * STACKOFF loads as equal.  Falls back to constant-value evaluation when
 * pure_expr_equal_impl bails out due to asymmetric folding (e.g. one side
 * was inlined to an immediate while the other still references a VAR). */
static int ir_opt_setif_cmp_operand_equal(TCCIRState *ir, IROperand a, IROperand b,
                                          int a_use_idx, int b_use_idx, int depth)
{
  if (ir_opt_pure_expr_equal_impl(ir, a, a_use_idx, b, b_use_idx, depth + 1))
    return 1;

  int a_tag = irop_get_tag(a);
  int b_tag = irop_get_tag(b);
  if (a_tag == IROP_TAG_STACKOFF && b_tag == IROP_TAG_STACKOFF)
  {
    int32_t a_vr = irop_get_vreg(a);
    int32_t b_vr = irop_get_vreg(b);
    if (a_vr == b_vr && a.u.imm32 == b.u.imm32 && a.is_lval == b.is_lval && a.is_local == b.is_local &&
        a.is_llocal == b.is_llocal && a.is_param == b.is_param && irop_get_btype(a) == irop_get_btype(b))
      return 1;
  }

  {
    uint64_t va, vb;
    if (ir_opt_eval_const_u64(ir, a, a_use_idx, &va, 0) &&
        ir_opt_eval_const_u64(ir, b, b_use_idx, &vb, 0) && va == vb)
      return 1;
  }

  return 0;
}

/* When a def reads memory (`Sym***DEREF***` or `T_vreg***DEREF***` source), the
 * value at that address must be the same at both `a_def_idx` and `b_def_idx`
 * for the defs to be value-equivalent.  Conservatively require no aliasing
 * store, call, inline-asm, or branch target between the two defs.  Pure ALU
 * ops (and loads — they only read) are safe to skip. */
static int ir_opt_pure_def_memory_stable(TCCIRState *ir, int a_def_idx, int b_def_idx)
{
  int lo = a_def_idx < b_def_idx ? a_def_idx : b_def_idx;
  int hi = a_def_idx < b_def_idx ? b_def_idx : a_def_idx;
  for (int k = lo + 1; k < hi; k++)
  {
    int kop = ir->compact_instructions[k].op;
    if (kop == TCCIR_OP_STORE || kop == TCCIR_OP_STORE_INDEXED ||
        kop == TCCIR_OP_STORE_POSTINC || kop == TCCIR_OP_BLOCK_COPY ||
        kop == TCCIR_OP_FUNCCALLVOID || kop == TCCIR_OP_FUNCCALLVAL ||
        kop == TCCIR_OP_INLINE_ASM || kop == TCCIR_OP_VLA_ALLOC)
      return 0;
    if (ir->compact_instructions[k].is_jump_target)
      return 0;
  }
  return 1;
}

/* True if `q` has any source operand that reads memory (lval-flagged operand
 * — `Sym***DEREF***`, `StackLoc***DEREF***`, or `T_vreg***DEREF***`). */
static int ir_opt_pure_def_has_memory_read(TCCIRState *ir, IRQuadCompact *q)
{
  if (irop_config[q->op].has_src1 && tcc_ir_op_get_src1(ir, q).is_lval)
    return 1;
  if (irop_config[q->op].has_src2 && tcc_ir_op_get_src2(ir, q).is_lval)
    return 1;
  return 0;
}

int ir_opt_pure_def_equal(TCCIRState *ir, int a_def_idx, int b_def_idx, int depth)
{
  IRQuadCompact *qa;
  IRQuadCompact *qb;

  if (a_def_idx < 0 || b_def_idx < 0)
    return 0;
  if (depth > 12)
    return 0;

  qa = &ir->compact_instructions[a_def_idx];
  qb = &ir->compact_instructions[b_def_idx];

  if (qa->op != qb->op)
    return 0;

  /* Memory-stability gate: if either def reads memory through a lval source,
   * we can only call the two defs value-equivalent when the underlying
   * memory hasn't been mutated between them.  Without this check a STORE
   * (or call) between two structurally-identical `*p` loads would silently
   * fold a stale read.  Cheap to check (single forward scan) and a no-op for
   * the existing ALU-only cases that never had lval sources. */
  if (a_def_idx != b_def_idx &&
      (ir_opt_pure_def_has_memory_read(ir, qa) ||
       ir_opt_pure_def_has_memory_read(ir, qb)) &&
      !ir_opt_pure_def_memory_stable(ir, a_def_idx, b_def_idx))
    return 0;

  switch (qa->op)
  {
  case TCCIR_OP_ASSIGN:
    return ir_opt_pure_expr_equal_impl(ir, tcc_ir_op_get_src1(ir, qa), a_def_idx, tcc_ir_op_get_src1(ir, qb), b_def_idx,
                                  depth + 1);
  case TCCIR_OP_LOAD:
    /* Two LOADs are value-equal when they read the same address with the
     * same access width.  Memory stability between the two defs has already
     * been verified above (LOAD has lval src1 -> has_memory_read is true),
     * so no intervening store/call could have changed the value. */
    return ir_opt_pure_expr_equal_impl(ir, tcc_ir_op_get_src1(ir, qa), a_def_idx, tcc_ir_op_get_src1(ir, qb), b_def_idx,
                                  depth + 1);
  case TCCIR_OP_ADD:
  case TCCIR_OP_OR:
  case TCCIR_OP_AND:
  case TCCIR_OP_XOR:
  case TCCIR_OP_MUL:
  case TCCIR_OP_BOOL_OR:
  case TCCIR_OP_BOOL_AND:
  {
    IROperand a1 = tcc_ir_op_get_src1(ir, qa);
    IROperand a2 = tcc_ir_op_get_src2(ir, qa);
    IROperand b1 = tcc_ir_op_get_src1(ir, qb);
    IROperand b2 = tcc_ir_op_get_src2(ir, qb);
    return ((ir_opt_pure_expr_equal_impl(ir, a1, a_def_idx, b1, b_def_idx, depth + 1) &&
             ir_opt_pure_expr_equal_impl(ir, a2, a_def_idx, b2, b_def_idx, depth + 1)) ||
            (ir_opt_pure_expr_equal_impl(ir, a1, a_def_idx, b2, b_def_idx, depth + 1) &&
             ir_opt_pure_expr_equal_impl(ir, a2, a_def_idx, b1, b_def_idx, depth + 1)));
  }
  case TCCIR_OP_SUB:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_ROR:
  case TCCIR_OP_UMOD:
  case TCCIR_OP_IMOD:
  case TCCIR_OP_UDIV:
  case TCCIR_OP_DIV:
  case TCCIR_OP_PDIV:
  {
    IROperand a1 = tcc_ir_op_get_src1(ir, qa);
    IROperand a2 = tcc_ir_op_get_src2(ir, qa);
    IROperand b1 = tcc_ir_op_get_src1(ir, qb);
    IROperand b2 = tcc_ir_op_get_src2(ir, qb);
    return (ir_opt_pure_expr_equal_impl(ir, a1, a_def_idx, b1, b_def_idx, depth + 1) &&
            ir_opt_pure_expr_equal_impl(ir, a2, a_def_idx, b2, b_def_idx, depth + 1));
  }
  case TCCIR_OP_MLA:
  {
    IROperand a1 = tcc_ir_op_get_src1(ir, qa);
    IROperand a2 = tcc_ir_op_get_src2(ir, qa);
    IROperand b1 = tcc_ir_op_get_src1(ir, qb);
    IROperand b2 = tcc_ir_op_get_src2(ir, qb);
    IROperand a3 = tcc_ir_op_get_accum(ir, qa);
    IROperand b3 = tcc_ir_op_get_accum(ir, qb);
    /* MLA = src1 * src2 + accum.  src1*src2 is commutative; accum is fixed. */
    if (!ir_opt_pure_expr_equal_impl(ir, a3, a_def_idx, b3, b_def_idx, depth + 1))
      return 0;
    return ((ir_opt_pure_expr_equal_impl(ir, a1, a_def_idx, b1, b_def_idx, depth + 1) &&
             ir_opt_pure_expr_equal_impl(ir, a2, a_def_idx, b2, b_def_idx, depth + 1)) ||
            (ir_opt_pure_expr_equal_impl(ir, a1, a_def_idx, b2, b_def_idx, depth + 1) &&
             ir_opt_pure_expr_equal_impl(ir, a2, a_def_idx, b1, b_def_idx, depth + 1)));
  }
  case TCCIR_OP_FUNCCALLVAL:
  {
    IROperand a_callee_op = tcc_ir_op_get_src1(ir, qa);
    IROperand b_callee_op = tcc_ir_op_get_src1(ir, qb);
    Sym *a_callee = irop_get_sym_ex(ir, a_callee_op);
    Sym *b_callee = irop_get_sym_ex(ir, b_callee_op);
    const char *a_name;
    const char *b_name;
    IROperand a_call_meta = tcc_ir_op_get_src2(ir, qa);
    IROperand b_call_meta = tcc_ir_op_get_src2(ir, qb);
    int argc;

    if (!a_callee || !b_callee)
      return 0;

    a_name = get_tok_str(a_callee->v, NULL);
    b_name = get_tok_str(b_callee->v, NULL);
    if (!ir_opt_is_pure_helper_name(a_name) || !b_name || strcmp(a_name, b_name) != 0)
      return 0;

    argc = TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, a_call_meta));
    if (argc != TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, b_call_meta)))
      return 0;

    for (int param_idx = 0; param_idx < argc; ++param_idx)
    {
      IROperand a_arg;
      IROperand b_arg;
      if (!ir_opt_get_call_param_operand(ir, a_def_idx, param_idx, &a_arg) ||
          !ir_opt_get_call_param_operand(ir, b_def_idx, param_idx, &b_arg))
      {
        return 0;
      }
      if (!ir_opt_pure_expr_equal_impl(ir, a_arg, a_def_idx, b_arg, b_def_idx, depth + 1))
        return 0;
    }

    return 1;
  }
  case TCCIR_OP_SETIF:
  {
    /* Two SETIFs are equal when:
     *   - Their condition codes match
     *   - The immediately-preceding CMPs have equal operands (in order)
     *   - No memory-changing op appears between the two CMPs (otherwise a
     *     memory operand might read different values).
     * The flag-producing CMP must sit at (def_idx - 1) modulo NOPs since
     * SETIF reads flags right after the CMP that set them. */
    IROperand cond_a = tcc_ir_op_get_src1(ir, qa);
    IROperand cond_b = tcc_ir_op_get_src1(ir, qb);
    if (!irop_is_immediate(cond_a) || !irop_is_immediate(cond_b))
      return 0;
    if (irop_get_imm64_ex(ir, cond_a) != irop_get_imm64_ex(ir, cond_b))
      return 0;

    int cmp_a_idx = a_def_idx - 1;
    while (cmp_a_idx >= 0 && ir->compact_instructions[cmp_a_idx].op == TCCIR_OP_NOP)
      cmp_a_idx--;
    int cmp_b_idx = b_def_idx - 1;
    while (cmp_b_idx >= 0 && ir->compact_instructions[cmp_b_idx].op == TCCIR_OP_NOP)
      cmp_b_idx--;
    if (cmp_a_idx < 0 || cmp_b_idx < 0)
      return 0;
    if (cmp_a_idx == cmp_b_idx)
      return 1;

    IRQuadCompact *cmp_a = &ir->compact_instructions[cmp_a_idx];
    IRQuadCompact *cmp_b = &ir->compact_instructions[cmp_b_idx];
    if (cmp_a->op != TCCIR_OP_CMP || cmp_b->op != TCCIR_OP_CMP)
      return 0;

    int lo = cmp_a_idx < cmp_b_idx ? cmp_a_idx : cmp_b_idx;
    int hi = cmp_a_idx < cmp_b_idx ? cmp_b_idx : cmp_a_idx;
    for (int k = lo + 1; k < hi; k++)
    {
      int kop = ir->compact_instructions[k].op;
      if (kop == TCCIR_OP_STORE || kop == TCCIR_OP_STORE_INDEXED ||
          kop == TCCIR_OP_BLOCK_COPY || kop == TCCIR_OP_FUNCCALLVOID ||
          kop == TCCIR_OP_FUNCCALLVAL || kop == TCCIR_OP_INLINE_ASM ||
          kop == TCCIR_OP_VLA_ALLOC)
        return 0;
      if (ir->compact_instructions[k].is_jump_target)
        return 0;
    }

    IROperand a1 = tcc_ir_op_get_src1(ir, cmp_a);
    IROperand a2 = tcc_ir_op_get_src2(ir, cmp_a);
    IROperand b1 = tcc_ir_op_get_src1(ir, cmp_b);
    IROperand b2 = tcc_ir_op_get_src2(ir, cmp_b);
    return ir_opt_setif_cmp_operand_equal(ir, a1, b1, cmp_a_idx, cmp_b_idx, depth) &&
           ir_opt_setif_cmp_operand_equal(ir, a2, b2, cmp_a_idx, cmp_b_idx, depth);
  }
  default:
    return 0;
  }
}

static int ir_opt_pure_expr_equal_impl(TCCIRState *ir, IROperand a, int a_use_idx,
                                       IROperand b, int b_use_idx, int depth)
{
  int a_tag;
  int b_tag;
  int32_t a_vr;
  int32_t b_vr;
  int a_def_idx;
  int b_def_idx;

  if (depth > 12)
    return 0;

  if (irop_is_immediate(a) || irop_is_immediate(b))
  {
    if (!irop_is_immediate(a) || !irop_is_immediate(b))
      return 0;
    return irop_get_imm64_ex(ir, a) == irop_get_imm64_ex(ir, b);
  }

  a_tag = irop_get_tag(a);
  b_tag = irop_get_tag(b);
  if (a_tag != IROP_TAG_VREG || b_tag != IROP_TAG_VREG)
    return ir_opt_nonvreg_expr_equal(ir, a, b);

  /* A dereferenced operand `*(V)` (is_lval) and a plain address operand `V`
   * (not is_lval) are different values — one loads from memory, the other is
   * the address itself — even when V resolves to the same definition.  Without
   * this guard, `c->field0 + K` (value-of-load + K) is treated as equal to
   * `&c->field0 + K` (== &c->fieldK, an address), which mis-folds comparisons
   * like `(c->size + K) > c->size_allocated` to a constant when K is the
   * byte offset between the two fields. */
  if (a.is_lval != b.is_lval)
    return 0;

  a_vr = irop_get_vreg(a);
  b_vr = irop_get_vreg(b);
  if (a_vr < 0 || b_vr < 0)
  {
    if (a_vr != b_vr)
      return 0;
    return a.vr == b.vr && a.u.imm32 == b.u.imm32 && a.is_unsigned == b.is_unsigned && a.is_static == b.is_static &&
           a.is_sym == b.is_sym && a.is_param == b.is_param;
  }

  a_def_idx = tcc_ir_find_defining_instruction(ir, a_vr, a_use_idx);
  b_def_idx = tcc_ir_find_defining_instruction(ir, b_vr, b_use_idx);

  if (a_def_idx < 0 || b_def_idx < 0)
    return a_vr == b_vr && a_def_idx == b_def_idx;

  if (a_def_idx == b_def_idx)
    return 1;

  if (!tcc_ir_vreg_has_single_def(ir, a_vr) || !tcc_ir_vreg_has_single_def(ir, b_vr))
    return 0;

  return ir_opt_pure_def_equal(ir, a_def_idx, b_def_idx, depth + 1);
}

int ir_opt_pure_expr_equal(TCCIRState *ir, IROperand a, int a_use_idx,
                           IROperand b, int b_use_idx, int depth)
{
  return ir_opt_pure_expr_equal_impl(ir, a, a_use_idx, b, b_use_idx, depth);
}

/* ============================================================================
 * Call-param helpers
 * ============================================================================ */

int ir_opt_get_call_param_operand(TCCIRState *ir, int call_idx, int param_idx, IROperand *out)
{
  IRQuadCompact *call_q;
  IROperand call_src2;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index || !out)
    return 0;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return 0;

  call_src2 = tcc_ir_op_get_src2(ir, call_q);
  call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2));

  for (int i = call_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;

    IROperand enc = tcc_ir_op_get_src2(ir, q);
    uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, enc);
    if (TCCIR_DECODE_CALL_ID(encoded) != call_id)
      continue;
    if (TCCIR_DECODE_PARAM_IDX(encoded) != param_idx)
      continue;

    *out = tcc_ir_op_get_src1(ir, q);
    return 1;
  }

  return 0;
}

void ir_opt_nop_call_params(TCCIRState *ir, int call_idx)
{
  IRQuadCompact *call_q;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return;

  call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call_q)));
  for (int i = call_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand enc;
    uint32_t encoded;

    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;

    enc = tcc_ir_op_get_src2(ir, q);
    encoded = (uint32_t)irop_get_imm64_ex(ir, enc);
    if (TCCIR_DECODE_CALL_ID(encoded) == call_id)
      q->op = TCCIR_OP_NOP;
  }
}

void ir_opt_nop_call_param(TCCIRState *ir, int call_idx, int param_idx)
{
  IRQuadCompact *call_q;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return;

  call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call_q)));
  for (int i = call_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand enc;
    uint32_t encoded;

    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;

    enc = tcc_ir_op_get_src2(ir, q);
    encoded = (uint32_t)irop_get_imm64_ex(ir, enc);
    if (TCCIR_DECODE_CALL_ID(encoded) == call_id && TCCIR_DECODE_PARAM_IDX(encoded) == param_idx)
      q->op = TCCIR_OP_NOP;
  }
}

void ir_opt_change_call_argc(TCCIRState *ir, int call_idx, int argc)
{
  IRQuadCompact *call_q;
  uint32_t encoded;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return;

  encoded = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call_q));
  call_id = TCCIR_DECODE_CALL_ID(encoded);
  tcc_ir_set_src2(ir, call_idx, irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_CALL(call_id, argc), IROP_BTYPE_INT32));
}

/* ============================================================================
 * Misc helpers
 * ============================================================================ */

int ir_opt_vreg_address_taken_between(TCCIRState *ir, int32_t vreg, int start_idx, int end_idx)
{
  if (!ir)
    return 0;

  for (int i = start_idx + 1; i < end_idx; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_LEA && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg)
      return 1;
  }

  return 0;
}

const char *ir_opt_get_constant_string_from_symref(TCCIRState *ir, IROperand op)
{
  IRPoolSymref *symref;
  Sym *sym;
  ElfSym *esym;
  Section *sec;
  const char *str;
  const char *nul;
  addr_t offset;
  size_t remaining;

  if (!ir || irop_get_tag(op) != IROP_TAG_SYMREF)
    return NULL;

  symref = irop_get_symref_ex(ir, op);
  if (!symref || symref->addend < 0)
    return NULL;
  if (symref->flags & IRPOOL_SYMREF_LVAL)
    return NULL;

  sym = symref->sym;
  if (!sym)
    return NULL;

  esym = elfsym(sym);
  if (!esym)
    return NULL;
  if (esym->st_shndx == SHN_UNDEF || esym->st_shndx >= (unsigned)tcc_state->nb_sections)
    return NULL;

  sec = tcc_state->sections[esym->st_shndx];
  if (!sec || !sec->data)
    return NULL;
  if (sec->sh_flags & SHF_WRITE)
    return NULL;
  if (esym->st_size == 0 || (addr_t)symref->addend >= esym->st_size)
    return NULL;

  offset = esym->st_value + (addr_t)symref->addend;
  if (offset >= sec->data_offset)
    return NULL;

  str = (const char *)(sec->data + offset);
  remaining = (size_t)(esym->st_size - (addr_t)symref->addend);
  nul = memchr(str, '\0', remaining);
  if (!nul)
    return NULL;

  return str;
}

/* ============================================================================
 * Callee symbol replacement helpers
 * ============================================================================ */

int change_callee_sym(TCCIRState *ir, int instr_idx, const char *new_name, int ret_btype)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IRPoolSymref *entry = irop_get_symref_ex(ir, src1);
  if (!entry)
    return 0;

  CType ftype;
  ftype.t = VT_FUNC;
  ftype.ref = sym_push2(&global_stack, SYM_FIELD, ret_btype, 0);
  ftype.ref->f.func_call = FUNC_CDECL;
  ftype.ref->f.func_type = FUNC_OLD;

  Sym *new_sym = external_global_sym(tok_alloc_const(new_name), &ftype);
  if (!new_sym)
    return 0;
  if (entry->sym == new_sym)
    return 0; /* already this callee: report no change so the optimizer converges */
  entry->sym = new_sym;
  return 1;
}

int change_callee_sym_keep_type(TCCIRState *ir, int instr_idx, const char *new_name)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IRPoolSymref *entry = irop_get_symref_ex(ir, src1);
  Sym *new_sym;

  if (!entry || !entry->sym)
    return 0;

  new_sym = external_global_sym(tok_alloc_const(new_name), &entry->sym->type);
  if (!new_sym)
    return 0;
  if (entry->sym == new_sym)
    return 0; /* already this callee: report no change so the optimizer converges */

  entry->sym = new_sym;
  return 1;
}

int tcc_ir_vreg_has_single_def(TCCIRState *ir, int32_t vreg)
{
  int def_count = 0;
  int n = ir->next_instruction_index;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) == vreg)
    {
      def_count++;
      if (def_count > 1)
        return 0;
    }
  }
  return def_count == 1;
}
