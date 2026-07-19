/*
 *  TCC IR - Pure expression and definition equality
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

static int ir_opt_pure_expr_equal_impl(TCCIRState *ir, IROperand a, int a_use_idx,
                                       IROperand b, int b_use_idx, int depth);

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
    /* Same slot = same vreg identity (including both anonymous at -1) plus same offset/attrs. */
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

/* Caller has already proven memory stability between the two CMP sites, so identical STACKOFF loads count as equal. */
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

/* Collects vregs whose *current* value the reads observe: spill-encoded or direct, a redefinition changes what they return. */
static int ir_opt_collect_var_read_ids(TCCIRState *ir, IRQuadCompact *q, int32_t *ids, int count)
{
  IROperand srcs[3];
  int nsrc = 0;
  if (irop_config[q->op].has_src1)
    srcs[nsrc++] = tcc_ir_op_get_src1(ir, q);
  if (irop_config[q->op].has_src2)
    srcs[nsrc++] = tcc_ir_op_get_src2(ir, q);
  if (q->op == TCCIR_OP_MLA)
    srcs[nsrc++] = tcc_ir_op_get_accum(ir, q);
  for (int s = 0; s < nsrc; s++)
  {
    int32_t vr = irop_get_vreg(srcs[s]);
    int type;
    if (!srcs[s].is_lval || vr < 0)
      continue;
    type = TCCIR_DECODE_VREG_TYPE(vr);
    if (type == TCCIR_VREG_TYPE_VAR || type == TCCIR_VREG_TYPE_PARAM)
      ids[count++] = vr;
  }
  return count;
}

/* Pure ALU ops are skippable, but a memory-naming dest or a plain redef of a VAR/PARAM an endpoint reads is not (switch fuzz seed 8261). */
static int ir_opt_pure_def_memory_stable(TCCIRState *ir, int a_def_idx, int b_def_idx)
{
  int lo = a_def_idx < b_def_idx ? a_def_idx : b_def_idx;
  int hi = a_def_idx < b_def_idx ? b_def_idx : a_def_idx;
  int32_t read_ids[6];
  int nids = 0;
  nids = ir_opt_collect_var_read_ids(ir, &ir->compact_instructions[a_def_idx], read_ids, nids);
  nids = ir_opt_collect_var_read_ids(ir, &ir->compact_instructions[b_def_idx], read_ids, nids);
  for (int k = lo + 1; k < hi; k++)
  {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    int kop = kq->op;
    if (kop == TCCIR_OP_FUNCCALLVOID || kop == TCCIR_OP_FUNCCALLVAL)
    {
      /* Pure helpers (isnan, __aeabi_f2d, ...) touch no memory; compare-fp-3's isunordered fold depends on skipping them. */
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, kq));
      const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
      if (!ir_opt_is_pure_helper_name(name))
        return 0;
    }
    else if (kop == TCCIR_OP_STORE || kop == TCCIR_OP_STORE_INDEXED ||
             kop == TCCIR_OP_STORE_POSTINC || kop == TCCIR_OP_BLOCK_COPY ||
             kop == TCCIR_OP_INLINE_ASM || kop == TCCIR_OP_VLA_ALLOC)
      return 0;
    if (kq->is_jump_target)
      return 0;
    if (irop_config[kop].has_dest)
    {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      int32_t kd_vr = irop_get_vreg(kd);
      /* A destination that itself names memory mutates it like a STORE. */
      if (kd.is_lval || irop_get_tag(kd) == IROP_TAG_STACKOFF)
        return 0;
      for (int s = 0; s < nids; s++)
        if (kd_vr == read_ids[s])
          return 0;
    }
  }
  return 1;
}

static int ir_opt_pure_def_has_memory_read(TCCIRState *ir, IRQuadCompact *q)
{
  if (irop_config[q->op].has_src1 && tcc_ir_op_get_src1(ir, q).is_lval)
    return 1;
  if (irop_config[q->op].has_src2 && tcc_ir_op_get_src2(ir, q).is_lval)
    return 1;
  if (q->op == TCCIR_OP_MLA && tcc_ir_op_get_accum(ir, q).is_lval)
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

  /* Without this gate a STORE or call between two structurally-identical `*p` loads would fold a stale read. */
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
    /* Same address + width; memory stability is already guaranteed by the gate above (LOAD has an lval src1). */
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
    /* MLA = src1 * src2 + accum: src1*src2 is commutative, accum is fixed. */
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
    /* The flag-producing CMP must sit at def_idx - 1 modulo NOPs. */
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

    /* Operand comparison below treats identical slot reads as equal, which only holds if nothing changed between the CMPs. */
    if (!ir_opt_pure_def_memory_stable(ir, cmp_a_idx, cmp_b_idx))
      return 0;

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

/* Recognize `T` where T's only def is `T <- [A] LOAD`, yielding the address
 * operand A and the index at which the read actually happens.  Used to compare a
 * value read through an explicit LOAD temp against the same memory named
 * directly as an operand. */
static int ir_opt_load_temp_addr(TCCIRState *ir, IROperand op, int use_idx,
                                 IROperand *out_addr, int *out_read_idx)
{
  if (op.is_lval || irop_get_tag(op) != IROP_TAG_VREG)
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || !tcc_ir_vreg_has_single_def(ir, vr))
    return 0;
  int d = tcc_ir_find_defining_instruction(ir, vr, use_idx);
  if (d < 0)
    return 0;
  IRQuadCompact *q = &ir->compact_instructions[d];
  if (q->op != TCCIR_OP_LOAD)
    return 0;
  IROperand src = tcc_ir_op_get_src1(ir, q);
  if (!src.is_lval)
    return 0;
  *out_addr = src;
  *out_read_idx = d;
  return 1;
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

  /* One side reads memory through an explicit LOAD temp while the other names
   * the same memory directly as an operand:
   *     T7 <- [G+8] LOAD ;  T3 <- T7            SHL #20      (side A)
   *                        T5 <- [G+8]*DEREF*   SHL #20      (side B)
   * Same value, different tree shape, so the tag comparison below rejects it.
   * copy_source_load_fwd produces exactly this mismatch — it rewrites a load's
   * ADDRESS and keeps the LOAD, while the expression it must compare equal to
   * reads the global inline.  Equal iff the addresses match and memory is stable
   * between the two reads (the LOAD's own index is where its read happens). */
  if (a_tag != b_tag)
  {
    IROperand laddr;
    int lidx;
    if (a_tag == IROP_TAG_VREG && b.is_lval && b_use_idx >= 0 &&
        ir_opt_load_temp_addr(ir, a, a_use_idx, &laddr, &lidx))

      return ir_opt_nonvreg_expr_equal(ir, laddr, b) &&
             ir_opt_pure_def_memory_stable(ir, lidx, b_use_idx);
    if (b_tag == IROP_TAG_VREG && a.is_lval && a_use_idx >= 0 &&
        ir_opt_load_temp_addr(ir, b, b_use_idx, &laddr, &lidx))
      return ir_opt_nonvreg_expr_equal(ir, a, laddr) &&
             ir_opt_pure_def_memory_stable(ir, a_use_idx, lidx);
  }

  if (a_tag != IROP_TAG_VREG || b_tag != IROP_TAG_VREG)
  {
    if (!ir_opt_nonvreg_expr_equal(ir, a, b))
      return 0;
    /* Identical memory reads yield the same value only if neither memory nor the named variable changed between the uses. */
    if (a.is_lval && a_use_idx >= 0 && b_use_idx >= 0 && a_use_idx != b_use_idx &&
        !ir_opt_pure_def_memory_stable(ir, a_use_idx, b_use_idx))
      return 0;
    return 1;
  }

  /* `*(V)` loads, `V` is the address: without this guard `c->size + K` folds against `&c->field0 + K` when K is the field gap. */
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
