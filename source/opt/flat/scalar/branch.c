/*
 *  TCC IR - Branch-folding generator table (pre-SSA engine)
 *
 *  Generators:
 *    branch_fold_test_zero — fold TEST_ZERO #const + JUMPIF to unconditional/NOP
 *    branch_fold_cmp       — fold CMP #const,#const + JUMPIF to unconditional/NOP
 *    setif_branch_fuse     — fuse CMP+SETIF+TEST_ZERO+JUMPIF → CMP+JUMPIF
 *    setif_branch_remat    — redo the comparison at a DISTANT branch on its
 *                            SETIF result, so the 0/1 never gets materialized
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
#include "opt_utils.h"
#include "opt_dsl.h"
#include "opt/flat/branch.h"

static int ir_branch_cmp_width(IROperand src1, IROperand src2)
{
  return (irop_get_btype(src1) == IROP_BTYPE_INT64 ||
          irop_get_btype(src2) == IROP_BTYPE_INT64)
             ? 64
             : 32;
}

static int ir_branch_eval_const_cmp(int64_t val1, int64_t val2, int cond,
                                    IROperand src1, IROperand src2)
{
  if (ir_branch_cmp_width(src1, src2) != 64)
  {
    uint32_t u1 = (uint32_t)val1;
    uint32_t u2 = (uint32_t)val2;
    int32_t s1 = (int32_t)u1;
    int32_t s2 = (int32_t)u2;
    switch (cond)
    {
    case TOK_EQ:
      return u1 == u2;
    case TOK_NE:
      return u1 != u2;
    case TOK_LT:
      return s1 < s2;
    case TOK_GE:
      return s1 >= s2;
    case TOK_LE:
      return s1 <= s2;
    case TOK_GT:
      return s1 > s2;
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
  return evaluate_compare_condition(val1, val2, cond);
}

OPT_GEN_FLAT(branch_fold_test_zero, TCCIR_OP_TEST_ZERO)
{
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_ANY });

  if (!irop_is_immediate(src1))
    return 0;

  int j = ir_skip_nops_forward(ir, i + 1, ir->next_instruction_index);
  if (j >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *jump_q = &ir->compact_instructions[j];
  if (jump_q->op != TCCIR_OP_JUMPIF)
    return 0;

  int64_t val = irop_get_imm64_ex(ir, src1);
  IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
  int tok = (int)irop_get_imm64_ex(ir, cond);

  int branch_taken;
  if (tok == 0x94)
    branch_taken = (val == 0);
  else if (tok == 0x95)
    branch_taken = (val != 0);
  else
    return 0;

  if (branch_taken) {
    IROperand jump_dest = tcc_ir_op_get_dest(ir, jump_q);
    q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, jump_dest);
  } else {
    q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_NOP;
    /* not-taken JUMPIF leaves stale flags: fold a dependent SETIF to its known constant */
    int k = ir_skip_nops_forward(ir, j + 1, ir->next_instruction_index);
    if (k < ir->next_instruction_index)
    {
      IRQuadCompact *setif_q = &ir->compact_instructions[k];
      if (setif_q->op == TCCIR_OP_SETIF && !setif_q->is_jump_target)
      {
        IROperand setif_cond = tcc_ir_op_get_src1(ir, setif_q);
        int setif_tok = (int)irop_get_imm64_ex(ir, setif_cond);
        int setif_result = -1;
        if (setif_tok == 0x95)
          setif_result = (val != 0) ? 1 : 0;
        else if (setif_tok == 0x94)
          setif_result = (val == 0) ? 1 : 0;
        if (setif_result >= 0)
        {
          IROperand setif_dest = tcc_ir_op_get_dest(ir, setif_q);
          IROperand imm = irop_make_imm32(-1, setif_result, irop_get_btype(setif_dest));
          setif_q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, k, imm);
          tcc_ir_set_src2(ir, k, IROP_NONE);
        }
      }
    }
  }

  return 1;
}

OPT_GEN_FLAT(branch_fold_cmp, TCCIR_OP_CMP)
{
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_ANY, .src2 = IR_CONSTRAINT_ANY });

  if (!irop_is_immediate(src1) || !irop_is_immediate(src2))
    return 0;

  int j = ir_skip_nops_forward(ir, i + 1, ir->next_instruction_index);
  if (j >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *jump_q = &ir->compact_instructions[j];
  if (jump_q->op != TCCIR_OP_JUMPIF)
    return 0;

  int64_t val1 = irop_get_imm64_ex(ir, src1);
  int64_t val2 = irop_get_imm64_ex(ir, src2);
  IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
  int tok = (int)irop_get_imm64_ex(ir, cond);

  int result = ir_branch_eval_const_cmp(val1, val2, tok, src1, src2);
  if (result < 0)
    return 0;

  if (result) {
    IROperand jump_dest = tcc_ir_op_get_dest(ir, jump_q);
    q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, jump_dest);
  } else {
    q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_NOP;
  }

  return 1;
}

OPT_GEN_FLAT(setif_branch_fuse, TCCIR_OP_CMP)
{
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });

  int n = ir->next_instruction_index;
  /* NOP-tolerant: passes leave NOP holes between the four ops (the inlined
   * bool-check chains of 20141107-1 carry one after the SETIF). */
  int si = ir_skip_nops_forward(ir, i + 1, n);
  if (si >= n)
    return 0;
  int ti = ir_skip_nops_forward(ir, si + 1, n);
  if (ti >= n)
    return 0;

  IRQuadCompact *setif_q = &ir->compact_instructions[si];
  if (setif_q->op != TCCIR_OP_SETIF)
    return 0;

  /* Look through one `U <- T` temp alias between SETIF and TEST_ZERO — the
   * residue of a const-folded `T XOR #0` that later cleanups would remove
   * anyway (20141107-1 site 1). */
  int ai = -1;
  int32_t alias_vr = -1;
  {
    IRQuadCompact *maybe_assign = &ir->compact_instructions[ti];
    if (maybe_assign->op == TCCIR_OP_ASSIGN && !maybe_assign->is_jump_target)
    {
      IROperand ad = tcc_ir_op_get_dest(ir, maybe_assign);
      IROperand as = tcc_ir_op_get_src1(ir, maybe_assign);
      IROperand sd = tcc_ir_op_get_dest(ir, setif_q);
      if (!ad.is_lval && !as.is_lval &&
          irop_get_vreg(as) >= 0 && irop_get_vreg(as) == irop_get_vreg(sd) &&
          irop_get_vreg(ad) >= 0 &&
          TCCIR_DECODE_VREG_TYPE(irop_get_vreg(ad)) == TCCIR_VREG_TYPE_TEMP &&
          irop_get_btype(ad) != IROP_BTYPE_INT64)
      {
        ai = ti;
        alias_vr = irop_get_vreg(ad);
        ti = ir_skip_nops_forward(ir, ti + 1, n);
        if (ti >= n)
          return 0;
      }
    }
  }
  int ji = ir_skip_nops_forward(ir, ti + 1, n);
  if (ji >= n)
    return 0;

  IRQuadCompact *test_q = &ir->compact_instructions[ti];
  IRQuadCompact *jump_q = &ir->compact_instructions[ji];
  /* 64-bit EQ/NE emit CMP T,#0 instead of TEST_ZERO T; both set Z from T==0 */
  if (test_q->op == TCCIR_OP_TEST_ZERO)
  {
  }
  else if (test_q->op == TCCIR_OP_CMP)
  {
    IROperand test_src2 = tcc_ir_op_get_src2(ir, test_q);
    if (!irop_is_immediate(test_src2) || irop_get_imm64_ex(ir, test_src2) != 0)
      return 0;
  }
  else
  {
    return 0;
  }
  if (jump_q->op != TCCIR_OP_JUMPIF)
    return 0;

  if (setif_q->is_jump_target || test_q->is_jump_target || jump_q->is_jump_target)
    return 0;

  IROperand setif_dest = tcc_ir_op_get_dest(ir, setif_q);
  IROperand test_src1 = tcc_ir_op_get_src1(ir, test_q);
  int32_t setif_vr = irop_get_vreg(setif_dest);
  int32_t test_vr = irop_get_vreg(test_src1);

  if (setif_vr < 0)
    return 0;
  if (ai >= 0)
  {
    if (test_vr == alias_vr)
    {
      /* live alias: SETIF -> assign -> TEST reads the alias */
      if (!tcc_ir_vreg_has_single_use(ir, setif_vr, -1) ||
          !tcc_ir_vreg_has_single_use(ir, alias_vr, -1))
        return 0;
    }
    else if (test_vr == setif_vr)
    {
      /* dead alias corpse: const-prop rewired TEST back to the SETIF result
       * (the folded `T XOR #0`), leaving the copy unread.  Require the alias
       * genuinely dead and the SETIF's only OTHER reader to be the TEST. */
      if (!tcc_ir_vreg_has_single_use(ir, setif_vr, ai))
        return 0;
      for (int u = 0; u < n; u++)
      {
        IRQuadCompact *uq = &ir->compact_instructions[u];
        if (u == ai || uq->op == TCCIR_OP_NOP)
          continue;
        for (int k = 1; k <= 3; k++)
        {
          IROperand op = (k == 1)   ? tcc_ir_op_get_src1(ir, uq)
                         : (k == 2) ? tcc_ir_op_get_src2(ir, uq)
                                    : tcc_ir_op_get_accum(ir, uq);
          if (irop_get_vreg(op) == alias_vr)
            return 0;
        }
        if (irop_config[uq->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, uq);
          if (d.is_lval && irop_get_vreg(d) == alias_vr)
            return 0;
        }
      }
    }
    else
      return 0;
  }
  else
  {
    if (test_vr != setif_vr)
      return 0;
    if (!tcc_ir_vreg_has_single_use(ir, setif_vr, -1))
      return 0;
  }

  IROperand setif_src1 = tcc_ir_op_get_src1(ir, setif_q);
  IROperand jump_src1 = tcc_ir_op_get_src1(ir, jump_q);
  int setif_tok = (int)irop_get_imm64_ex(ir, setif_src1);
  int jump_tok = (int)irop_get_imm64_ex(ir, jump_src1);

  int new_tok;
  if (jump_tok == 0x94)
    new_tok = invert_cond_token(setif_tok);
  else if (jump_tok == 0x95)
    new_tok = setif_tok;
  else
    return 0;

  if (new_tok < 0)
    return 0;

  int btype = irop_get_btype(jump_src1);
  IROperand new_cond = irop_make_imm32(-1, new_tok, btype);
  tcc_ir_set_src1(ir, ji, new_cond);

  setif_q->op = TCCIR_OP_NOP;
  test_q->op = TCCIR_OP_NOP;
  if (ai >= 0)
    ir->compact_instructions[ai].op = TCCIR_OP_NOP;

  return 1;
}

/* CMP a,b; SETIF cond -> T, where every use of T is a branch test
 *      ->  the comparison redone at each branch, and the quartet deleted.
 *
 * setif_branch_fuse above needs the TEST_ZERO to be the very next instruction.
 * The predicates soft float is built from are not: `const int a_max_exp =
 * (a_exp == 0x7FF);` is computed at the top of __aeabi_dadd and branched on
 * forty instructions later -- sometimes twice -- so the boolean is materialized
 * with a `cmp`/`ite`/`moveq`/`movne` quartet and then tested against zero again
 * at every use.  Redoing the comparison at the branch costs the same two
 * instructions the TEST_ZERO pair already cost, and makes the quartet dead.
 *
 * Legality needs no dataflow walk.  The SETIF is T's only definition, so a path
 * reaching a use without executing it would be reading an uninitialised value;
 * and only NOPs separate the CMP from the SETIF, so whenever T is defined the
 * comparison ran.  What is left to establish is that the operands still HOLD
 * what was compared, which is why each non-immediate one must have exactly one
 * definition in the function -- and must be a value, not a memory read, whose
 * repetition could observe a different word.
 *
 * At most one non-immediate operand.  The transform trades the boolean's live
 * range for its operands', over the same span; with one register operand that
 * is a wash, with two it hands the allocator an extra simultaneous value -- the
 * trade that reverted five earlier attempts on this same library.
 *
 * The now-orphaned CMP is left for orphan_cmp_elim: whether its flags still
 * have a consumer is that pass's question, not this one's.
 */
OPT_GEN_FLAT(setif_branch_remat, TCCIR_OP_CMP)
{
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_ANY, .src2 = IR_CONSTRAINT_ANY });

  const int n = ir->next_instruction_index;
  int si = ir_skip_nops_forward(ir, i + 1, n);
  if (si >= n)
    return 0;

  IRQuadCompact *setif_q = &ir->compact_instructions[si];
  if (setif_q->op != TCCIR_OP_SETIF)
    return 0;
  /* A label anywhere between means the SETIF is reachable without the CMP. */
  for (int j = i + 1; j <= si; j++)
    if (ir->compact_instructions[j].is_jump_target)
      return 0;

  IROperand setif_dest = tcc_ir_op_get_dest(ir, setif_q);
  int32_t sv = irop_get_vreg(setif_dest);
  if (sv < 0 || setif_dest.is_lval)
    return 0;
  /* A named local counts as well as a temp -- `const int a_max_exp = ...` is
   * exactly the shape this is here for.  Its address escaping is caught by the
   * use scan below, which admits nothing but a branch test. */
  if (TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_TEMP &&
      TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_VAR)
    return 0;
  if (irop_get_btype(setif_dest) == IROP_BTYPE_INT64)
    return 0;
  if (tcc_ir_access_is_volatile(ir, setif_dest))
    return 0;
  if (!tcc_ir_vreg_has_single_def(ir, sv))
    return 0;

  int nregs = 0;
  for (int k = 0; k < 2; k++)
  {
    IROperand o = k ? src2 : src1;
    if (irop_is_plain_imm(o))
      continue;
    int32_t vr = irop_get_vreg(o);
    if (vr < 0 || o.is_lval || o.is_local || o.is_sym)
      return 0;
    /* A TEMP and nothing else.  Its address is never taken, so no callee and
     * no store through a pointer can change it behind this scan -- and unlike
     * a parameter it carries no implicit definition at function entry, which
     * tcc_ir_vreg_has_single_def does not count.  (A parameter with one
     * explicit def therefore has TWO, and comparing it again at the branch
     * would compare the NEW value: the f2 case in tests2 pins that.) */
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    if (!tcc_ir_vreg_has_single_def(ir, vr))
      return 0;
    if (++nregs > 1)
      return 0;
  }
  if (nregs == 0)
    return 0; /* constant folding owns this shape */

  const int setif_tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, setif_q));
  const int inv_tok = invert_cond_token(setif_tok);

#define REMAT_MAX_USES 8
  int use_idx[REMAT_MAX_USES];
  int jmp_idx[REMAT_MAX_USES];
  int new_tok[REMAT_MAX_USES];
  int nuses = 0;

  for (int u = 0; u < n; u++)
  {
    IRQuadCompact *uq = &ir->compact_instructions[u];
    if (u == si || uq->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[uq->op].has_dest && irop_get_vreg(tcc_ir_op_get_dest(ir, uq)) == sv)
      return 0; /* redefined, or written through as an address */

    int reads = 0;
    for (int k = 1; k <= 3; k++)
    {
      IROperand o = (k == 1)   ? (irop_config[uq->op].has_src1 ? tcc_ir_op_get_src1(ir, uq) : IROP_NONE)
                    : (k == 2) ? (irop_config[uq->op].has_src2 ? tcc_ir_op_get_src2(ir, uq) : IROP_NONE)
                               : (uq->op == TCCIR_OP_MLA ? tcc_ir_op_get_accum(ir, uq) : IROP_NONE);
      if (irop_get_vreg(o) == sv)
        reads = 1;
    }
    if (!reads)
      continue;
    if (u < si || nuses >= REMAT_MAX_USES)
      return 0;

    /* 64-bit EQ/NE emits `CMP T,#0` where the 32-bit form emits TEST_ZERO;
     * both set Z from T == 0 and both occupy one slot, which is what lets the
     * comparison be written back over them without moving anything. */
    if (uq->op == TCCIR_OP_TEST_ZERO)
    {
      if (irop_get_vreg(tcc_ir_op_get_src1(ir, uq)) != sv)
        return 0;
    }
    else if (uq->op == TCCIR_OP_CMP)
    {
      IROperand a = tcc_ir_op_get_src1(ir, uq);
      IROperand b = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(a) != sv || !irop_is_plain_imm(b) || irop_get_imm64_ex(ir, b) != 0)
        return 0;
    }
    else
      return 0;

    int ji = ir_skip_nops_forward(ir, u + 1, n);
    if (ji >= n)
      return 0;
    IRQuadCompact *jq = &ir->compact_instructions[ji];
    if (jq->op != TCCIR_OP_JUMPIF)
      return 0;
    for (int j = u + 1; j <= ji; j++)
      if (ir->compact_instructions[j].is_jump_target)
        return 0;

    int jt = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jq));
    int nt;
    if (jt == 0x94)
      nt = inv_tok;
    else if (jt == 0x95)
      nt = setif_tok;
    else
      return 0;
    if (nt < 0)
      return 0;

    use_idx[nuses] = u;
    jmp_idx[nuses] = ji;
    new_tok[nuses] = nt;
    nuses++;
  }
  if (nuses == 0)
    return 0;

  /* TEST_ZERO owns ONE operand slot and CMP needs two, so the comparison
   * cannot be written over it in place -- the src2 write would land in the
   * next instruction's operands.  Take fresh slots at the pool tail and
   * re-point operand_base, and take them all BEFORE mutating anything so a
   * pool that will not grow leaves the IR untouched. */
  int new_base[REMAT_MAX_USES];
  for (int u = 0; u < nuses; u++)
  {
    tcc_ir_pool_ensure(ir, 2);
    if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
      return 0;
    new_base[u] = ir->iroperand_pool_count;
    tcc_ir_pool_add(ir, src1);
    tcc_ir_pool_add(ir, src2);
  }

  for (int u = 0; u < nuses; u++)
  {
    IRQuadCompact *uq = &ir->compact_instructions[use_idx[u]];
    IROperand jcond = tcc_ir_op_get_src1(ir, &ir->compact_instructions[jmp_idx[u]]);
    uq->op = TCCIR_OP_CMP;
    uq->operand_base = new_base[u];
    tcc_ir_set_src1(ir, jmp_idx[u], irop_make_imm32(-1, new_tok[u], irop_get_btype(jcond)));
  }
  setif_q->op = TCCIR_OP_NOP;
#undef REMAT_MAX_USES
  return 1;
}

/* (SETIF cond -> T); U <- T XOR #1  ->  U <- SETIF !cond, when T is single-use
 * and only NOPs separate the two (the CMP's flags are still current at the XOR
 * position).  A SETIF result is 0/1, so XOR #1 is boolean NOT.  Feeds the
 * `((x != k) ^ const_b)` chains of inlined bool checks into setif_branch_fuse. */
OPT_GEN_FLAT(setif_xor_invert, TCCIR_OP_XOR)
{
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_ANY, .src2 = IR_CONSTRAINT_IMM });

  int64_t mask;
  if (!irop_is_plain_imm(src2))
    return 0;
  mask = irop_get_imm64_ex(ir, src2);
  if (mask != 1 && mask != 0) /* 1 = boolean NOT of a 0/1 SETIF; 0 = identity */
    return 0;
  if (irop_get_btype(dest) == IROP_BTYPE_INT64)
    return 0;

  int32_t xvr = irop_get_vreg(src1);
  if (xvr < 0 || src1.is_lval)
    return 0;

  /* previous non-NOP must be the SETIF defining src1 */
  int si = i - 1;
  while (si >= 0 && ir->compact_instructions[si].op == TCCIR_OP_NOP)
    si--;
  if (si < 0)
    return 0;
  IRQuadCompact *setif_q = &ir->compact_instructions[si];
  if (setif_q->op != TCCIR_OP_SETIF || q->is_jump_target)
    return 0;
  IROperand setif_dest = tcc_ir_op_get_dest(ir, setif_q);
  if (irop_get_vreg(setif_dest) != xvr)
    return 0;
  if (!tcc_ir_vreg_has_single_use(ir, xvr, -1))
    return 0;

  /* The XOR must be reached ONLY by falling through from the SETIF.  The
   * instructions between them are NOPs, but a NOP can still carry a jump label
   * after its instruction was folded away (e.g. a diamond merge whose UDIV/AND
   * collapsed to constants).  Such a label means another path — the else arm of
   * a `cond ? setif_expr : other` — reaches the XOR without executing the
   * CMP/SETIF: its flags are stale AND src1 (the SETIF dest) is a phi temp
   * holding the else value there.  Rewriting the XOR to an unconditional SETIF
   * would drop that arm (fuzz seed struct_byval:6988).  Bail on any join. */
  for (int j = si + 1; j <= i; j++)
    if (ir->compact_instructions[j].is_jump_target)
      return 0;

  IROperand setif_cond = tcc_ir_op_get_src1(ir, setif_q);
  int tok = (int)irop_get_imm64_ex(ir, setif_cond);
  if (mask == 1)
  {
    tok = invert_cond_token(tok);
    if (tok < 0)
      return 0;
  }

  {
    IROperand new_cond = irop_make_imm32(-1, tok, irop_get_btype(setif_cond));
    IROperand new_dest = dest;
    q->op = TCCIR_OP_SETIF;
    tcc_ir_set_dest(ir, i, new_dest);
    tcc_ir_set_src1(ir, i, new_cond);
    tcc_ir_set_src2(ir, i, IROP_NONE);
    setif_q->op = TCCIR_OP_NOP;
  }
  return 1;
}

/* CALL f -> T (f returns _Bool per its declared type); CMP T,#0;
 * X <- SETIF(!=)  ->  X <- T.  The AAPCS bool return is already 0/1 (gcc
 * makes the same assumption), so the defensive re-normalization the frontend
 * emits on `bool c = f(...)` is an identity.  The CMP stays; orphan-cmp
 * elimination removes it once no flag consumer is left. */
OPT_GEN_FLAT(bool_call_norm, TCCIR_OP_CMP)
{
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_ANY, .src2 = IR_CONSTRAINT_IMM });

  if (!irop_is_plain_imm(src2) || irop_get_imm64_ex(ir, src2) != 0)
    return 0;
  int32_t tvr = irop_get_vreg(src1);
  if (tvr < 0 || src1.is_lval || irop_get_btype(src1) == IROP_BTYPE_INT64)
    return 0;

  /* previous non-NOP defines src1 via a FUNCCALLVAL of a _Bool function */
  int ci = i - 1;
  while (ci >= 0 && ir->compact_instructions[ci].op == TCCIR_OP_NOP)
    ci--;
  if (ci < 0)
    return 0;
  {
    IRQuadCompact *call_q = &ir->compact_instructions[ci];
    Sym *callee;
    IROperand call_dest;
    if (call_q->op != TCCIR_OP_FUNCCALLVAL)
      return 0;
    call_dest = tcc_ir_op_get_dest(ir, call_q);
    if (irop_get_vreg(call_dest) != tvr)
      return 0;
    callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, call_q));
    if (!callee || (callee->type.t & VT_BTYPE) != VT_FUNC || !callee->type.ref)
      return 0;
    if ((callee->type.ref->type.t & VT_BTYPE) != VT_BOOL)
      return 0;
  }

  /* next non-NOP must be the normalizing SETIF(!=) */
  {
    int si = ir_skip_nops_forward(ir, i + 1, ir->next_instruction_index);
    IRQuadCompact *setif_q;
    if (si >= ir->next_instruction_index)
      return 0;
    setif_q = &ir->compact_instructions[si];
    if (setif_q->op != TCCIR_OP_SETIF || setif_q->is_jump_target)
      return 0;
    if ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, setif_q)) != TOK_NE)
      return 0;
    setif_q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, si, src1);
    tcc_ir_set_src2(ir, si, IROP_NONE);
  }
  return 1;
}

const IROptGen branch_gens[] = {
    {TCCIR_OP_CMP, opt_dsl_dispatch_bool_call_norm_flat, "bool_call_norm", 0},
    {TCCIR_OP_XOR, opt_dsl_dispatch_setif_xor_invert_flat, "setif_xor_invert", 0},
    {TCCIR_OP_CMP, opt_dsl_dispatch_setif_branch_fuse_flat, "setif_branch_fuse", 0},
    {TCCIR_OP_CMP, opt_dsl_dispatch_setif_branch_remat_flat, "setif_branch_remat", 0},
    {TCCIR_OP_CMP, opt_dsl_dispatch_branch_fold_cmp_flat, "branch_fold_cmp", 0},
    {TCCIR_OP_TEST_ZERO, opt_dsl_dispatch_branch_fold_test_zero_flat, "branch_fold_test_zero", 0},
};

const int branch_gens_count = sizeof(branch_gens) / sizeof(branch_gens[0]);

int tcc_ir_opt_setif_branch_fuse(TCCIRState *ir)
{
  if (ir->next_instruction_index < 4)
    return 0;
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_run_gens(&ctx, branch_gens, branch_gens_count);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}

int tcc_ir_opt_setif_branch_fuse_ex(IROptCtx *ctx) { return tcc_ir_opt_setif_branch_fuse(ctx->ir); }
