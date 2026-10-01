/*
 *  TCC IR - SSA loop: bottom-test a counter-eliminated pointer walk
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ssa:loop_bottom_test — the second half of loop rotation, for the one shape
 * ssa:loop_rotate structurally cannot see.
 *
 * ssa:loop_rotate matches the frontend's canonical un-rotated header
 * `CMP; JUMPIF exit; JUMP body` and relocates the body/latch up into the
 * header's slots.  Two things leave loops outside that matcher:
 *
 *   - ssa:cfg_cleanup collapses the header's third slot (the `JUMP body`) into
 *     a fall-through whenever the body already follows the header, so the loop
 *     arrives at rotation as a plain `while` — `CMP; JUMPIF exit; <body>;
 *     JUMP header` — and is refused before any gate runs;
 *   - ssa:iv_strength_reduction runs AFTER rotation and produces exactly that
 *     shape when it eliminates a counter: the walk's `p != end` test sits at
 *     the top and an unconditional back-edge at the bottom.
 *
 * So tcc's pointer walks retire `cmp; b<cc> exit; <body>; b header` — four
 * instructions for a one-store loop — where gcc emits `<body>; cmp; b<cc>
 * body`.  The project's own per-element measurements on RP2350 silicon
 * (iv_analysis.c) put the bottom-tested post-indexed walk at 6.012 cycles
 * against 7.012 for the split-bump form and 8.012 for the scaled index, i.e.
 * the bottom test is worth one branch per iteration on the fastest shape we
 * can already produce.
 *
 * The rewrite here is deliberately NOT a rotation: the body already sits
 * between the header and the back-edge, so nothing moves.  A copy of the
 * header CMP is inserted in front of the back-edge and the back-edge JUMP
 * becomes a JUMPIF on the inverted condition targeting the body.  The header
 * CMP/JUMPIF stay put as the zero-trip guard.  Cost is +1 instruction of code;
 * the saving is one taken branch per iteration.
 *
 * Correctness argument (why this needs none of loop_rotate's machinery):
 *   before: ... body; JUMP hi; hi: CMP a,b; JUMPIF cond -> exit; fall -> body
 *   after:  ... body; CMP a,b; JUMPIF !cond -> body; fall -> exit
 * The two CMPs read the same operands and nothing runs between the end of the
 * body and the header, so the branch decision is identical.  Entry still goes
 * through the header guard.  No instruction changes index relative to another
 * (insert_instr_at renumbers every jump target and switch arm), so the
 * orig_index side tables that made rotation fragile are not disturbed —
 * the only synthesized instruction is the tail CMP, and it is created before
 * tcc_ir_barrel_shift_fusion (post-RA) has annotated anything.
 *
 * One instruction fewer per iteration is NOT automatically faster on this
 * silicon, and the reason is worth keeping: the first rig A/B had
 * bench_memcpy's checksum walk go from five instructions to four and get
 * 13.8% SLOWER (1,815,226 -> 2,066,226 cycles), while stringsearch's fill loop
 * -- the same transform, the same shape -- won 3.6%.  The difference was the
 * BRANCH TARGET's alignment: stringsearch's head landed on a word boundary,
 * bench_memcpy's on a halfword-odd one, so every refill after the back edge
 * paid an extra fetch.  Padding that one head to a word boundary handed back
 * all 251,000 cycles and then some (1,812,226).  Hence
 * IRQuadCompact.align_target, which this pass sets on every loop head it
 * bottom-tests; without it the pass is a net rig LOSS.  The effect is general
 * and has nothing to do with the head's width -- codegen now aligns every
 * backward-branch target, worth -0.944% on the rig set by itself.
 *
 * Every level requires the same structural core: a straight-line, call-free
 * body, at least one memory access in it, and exactly one constant
 * self-increment of the compared value.  The levels differ only in how close
 * the loop has to be to the measured-fastest shape — TCC_BOTTOM_TEST:
 *   0  off
 *   1  (default) the walk must end post-indexed (the 6.012 form) and the body
 *      must carry no register-scaled access (the 8.012 form)
 *   2  any constant-stride walk without a register-scaled access
 *   3  any constant-stride walk
 *
 * Levels 1 and 3 measured IDENTICAL on the RP2350 benchmark set (level 3 only
 * moves the QEMU corpus's own mibench_crc32, by -65,536 instructions); the
 * default stays at the tightest gate that produces the whole measured win.
 *
 * Disableable via TCC_DISABLE_PASS=ssa:loop_bottom_test.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_loop_utils.h"
#include "opt_utils.h"
#include "tccdbgenv.h"

TCC_DBG_ENV_INT(lbt_level_env, "TCC_BOTTOM_TEST", 1)

#define SSA_LBT_MAX_REWRITES 64

/* First non-NOP at or after `i`, or n. */
static int lbt_skip_nops(const TCCIRState *ir, int i)
{
  int n = ir->next_instruction_index;
  while (i < n && ir->compact_instructions[i].op == TCCIR_OP_NOP)
    i++;
  return i;
}

/* A CMP operand may be duplicated into the tail test only when re-evaluating it
 * is free and side-effect-free: a register value or a literal.  An lval operand
 * dereferences memory, and a second read of a volatile location would be a
 * visible extra access. */
static int lbt_operand_reusable(const TCCIRState *ir, IROperand op)
{
  if (irop_is_immediate(op))
    return 1;
  if (!irop_has_vreg(op))
    return 0;
  if (!op.is_lval)
    return 1;
  /* A direct local VAR is a register or a frame slot, not a pointer
   * dereference: re-reading it in the tail test is what the header test would
   * have read, at worst one more frame load.  Anything else -- a deref through
   * a pointer, a global, a `*p` -- must not be evaluated a second time.  Same
   * convention as loop_rotate's ROT_VAR_DIRECT. */
  if (!(TCCIR_DECODE_VREG_TYPE(irop_get_vreg(op)) == TCCIR_VREG_TYPE_VAR && op.is_local))
    return 0;
  /* ... and not when the function has volatile memory and this access is not
   * proven clean: a duplicated read of `volatile int i` is a visible extra
   * access. */
  return !tcc_ir_access_is_volatile(ir, op);
}

/* Does `q` write `v`?  A non-lval dest is an ordinary definition; a direct local
 * VAR lval is one too (loop_rotate's ROT_VAR_DIRECT convention).  A dest that
 * dereferences a pointer stores THROUGH v and leaves v alone.  The post-indexed
 * forms also write their base, but they are created after this pass runs. */
static int lbt_defines_vreg(const TCCIRState *ir, const IRQuadCompact *q, int32_t v)
{
  int op = q->op;
  if (v < 0)
    return 0;
  if (op == TCCIR_OP_LOAD_POSTINC || op == TCCIR_OP_STORE_POSTINC)
  {
    IROperand b = (op == TCCIR_OP_LOAD_POSTINC) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(b) == v)
      return 1;
  }
  if (!irop_config[op].has_dest)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (irop_get_vreg(d) != v)
    return 0;
  if (!d.is_lval)
    return 1;
  return TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR && d.is_local;
}

static int lbt_is_memory_op(int op)
{
  return op == TCCIR_OP_LOAD || op == TCCIR_OP_STORE || op == TCCIR_OP_LOAD_INDEXED ||
         op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_LOAD_POSTINC || op == TCCIR_OP_STORE_POSTINC ||
         op == TCCIR_OP_BLOCK_COPY;
}

/* Straight-line-ness: the body must be one basic block with no way out but the
 * back-edge, so the tail test is reached exactly when the back-edge was. */
static int lbt_body_is_straight_line(const TCCIRState *ir, int from, int to)
{
  for (int i = from; i <= to; i++)
  {
    int op = ir->compact_instructions[i].op;
    switch (op)
    {
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

/* The profitability signature: `X <- X ADD #k` for one of the compared values,
 * plus real memory traffic in the body.  Without the self-increment this is not
 * a walk at all (and may not even terminate through this test); without the
 * memory access there is nothing for the saved branch to be measured against. */
static int lbt_body_profitable(const TCCIRState *ir, int from, int to, int32_t vr1, int32_t vr2, int level,
                               int32_t *out_walk_vreg, int *out_bump_idx)
{
  int bumps = 0, mem = 0;
  for (int i = from; i <= to; i++)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (lbt_is_memory_op(op))
      mem = 1;
    if (op != TCCIR_OP_ADD && op != TCCIR_OP_SUB)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    if (d.is_lval || s1.is_lval || !irop_has_vreg(d) || !irop_has_vreg(s1))
      continue;
    if (!irop_is_immediate(s2))
      continue;
    int32_t dv = irop_get_vreg(d);
    if (dv != irop_get_vreg(s1))
      continue;
    if (dv == vr1 || dv == vr2)
    {
      bumps++;
      *out_walk_vreg = dv;
      *out_bump_idx = i;
    }
  }
  if (bumps != 1 || !mem)
    return 0;
  /* The bump must be the walked value's ONLY definition in the body.  A second
   * one means the value is not a constant-stride walk at all -- 20000801-1's
   * `*bp++ = c; ...; bp += 2` writes it three times -- and the copy forwarding
   * below reasons from `P is written once, at bump_idx`. */
  for (int i = from; i <= to; i++)
    if (i != *out_bump_idx && lbt_defines_vreg(ir, &ir->compact_instructions[i], *out_walk_vreg))
      return 0;
  return 1;
}

/* A register-scaled access -- `ldr rd,[rn,rm,lsl #k]`, whether already fused
 * into an indexed op or still open as `t <- i SHL #k; a <- base ADD t; *a`.
 * The M33 charges an extra cycle for it that QEMU's icount does not
 * (tcc-m33-addressing-mode-cycle-costs), and it is the form every previous
 * rotation experiment lost on.  An indexed op with an IMMEDIATE index is just
 * `[rn,#imm]` and carries no such penalty. */
static int lbt_body_has_scaled_index(const TCCIRState *ir, int from, int to)
{
  for (int i = from; i <= to; i++)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED)
    {
      if (!irop_is_immediate(tcc_ir_op_get_src2(ir, q)))
        return 1;
      continue;
    }
    if (op == TCCIR_OP_SHL || op == TCCIR_OP_MUL)
      return 1;
  }
  return 0;
}

/* Will the walk end post-indexed?  That is the 6.012 cycles/element form in
 * iv_analysis.c's table, and the only one the rig data actually endorses; the
 * split-bump form is 7.012 and the scaled index 8.012.  ra:load_postinc /
 * ra:store_postinc fuse `*P; P <- P + #k`, so the body must dereference the
 * walked register itself -- or a copy of it that lbt_forward_body_copies is
 * about to fold away. */
static int lbt_walk_is_postinc_shaped(const TCCIRState *ir, int from, int bump_idx, int32_t walk_vreg)
{
  int32_t alias[8];
  int nalias = 0;
  for (int i = from; i < bump_idx; i++)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_ASSIGN)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (!d.is_lval && !s1.is_lval && irop_has_vreg(d) && irop_get_vreg(s1) == walk_vreg &&
          nalias < (int)(sizeof(alias) / sizeof(alias[0])))
        alias[nalias++] = irop_get_vreg(d);
      continue;
    }
    if (op != TCCIR_OP_LOAD && op != TCCIR_OP_STORE)
      continue;
    IROperand ptr = (op == TCCIR_OP_LOAD) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_dest(ir, q);
    if (!ptr.is_lval || ptr.is_local || ptr.is_llocal || ptr.is_sym || !irop_has_vreg(ptr))
      continue;
    int32_t pv = irop_get_vreg(ptr);
    if (pv == walk_vreg)
      return 1;
    for (int a = 0; a < nalias; a++)
      if (alias[a] == pv)
        return 1;
  }
  return 0;
}

/* Every vreg slot of an instruction that a rewrite has to reach.  The 4th pool
 * slot is an immediate scale on the indexed/postinc forms and a condition on
 * SELECT; only MLA carries a vreg there. */
static IROperand *lbt_operand_slot(TCCIRState *ir, IRQuadCompact *q, int which)
{
  int op = q->op;
  int base = (int)q->operand_base;
  switch (which)
  {
  case 0:
    return irop_config[op].has_dest ? &ir->iroperand_pool[base] : NULL;
  case 1:
    return irop_config[op].has_src1 ? &ir->iroperand_pool[base + irop_config[op].has_dest] : NULL;
  case 2:
    return irop_config[op].has_src2
               ? &ir->iroperand_pool[base + irop_config[op].has_dest + irop_config[op].has_src1]
               : NULL;
  default:
    return op == TCCIR_OP_MLA ? &ir->iroperand_pool[base + 3] : NULL;
  }
}

/* Forward `T <- P` copies inside the body onto P itself.
 *
 * ssa:iv_strength_reduction leaves the walked pointer's every access reading a
 * COPY of the walk register (`T <- P; *T <- v; P <- P + k`).  In the un-rotated
 * shape SSA renaming versions P at the loop header, and ssa:cprop then folds
 * the copy away.  Bottom-testing makes the body its own predecessor, and
 * ra_promote_multidef_temps_to_vars deliberately declines a TEMP whose every
 * use sits in one of its own def-blocks -- which is now true of P -- so no phi
 * is built, P keeps two defs, and the copy survives.  It costs no instruction
 * (the coalescer gives T and P one register and the move disappears), but it
 * hides the walk from ra:store_postinc/ra:load_postinc, whose matcher wants the
 * dereferenced vreg and the bumped vreg to be the SAME -- and the post-indexed
 * access is the entire point of bottom-testing this shape.
 *
 * Safe here for reasons the body checks already established: the body is one
 * straight-line block, P is written exactly once in it (the bump), and a use of
 * T before the bump therefore sees exactly P.  What is left to prove per copy is
 * that T is confined to the body and not redefined inside it. */
static int lbt_forward_body_copies(TCCIRState *ir, int body_start, int body_end, int32_t walk_vreg, int bump_idx)
{
  int n = ir->next_instruction_index;
  int forwarded = 0;
  for (int c = body_start; c <= body_end; c++)
  {
    IRQuadCompact *cq = &ir->compact_instructions[c];
    if (cq->op != TCCIR_OP_ASSIGN)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, cq);
    IROperand s = tcc_ir_op_get_src1(ir, cq);
    if (d.is_lval || s.is_lval || !irop_has_vreg(d) || !irop_has_vreg(s))
      continue;
    if (irop_get_vreg(s) != walk_vreg)
      continue;
    int32_t tv = irop_get_vreg(d);
    if (tv == walk_vreg)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(tv) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (irop_get_btype(d) != irop_get_btype(s) || d.is_unsigned != s.is_unsigned)
      continue;
    /* The copy must dominate the uses it feeds, and P must still hold the
     * copied value there: only uses strictly before the bump qualify.  P having
     * no other definition in the body is what lbt_body_profitable proved; assert
     * it again rather than inherit it, because getting this wrong is silent
     * wrong code (20000801-1 stored through the already-incremented pointer). */
    if (c >= bump_idx)
      continue;
    {
      int other_def = 0;
      for (int i = body_start; i <= body_end && !other_def; i++)
        if (i != bump_idx && lbt_defines_vreg(ir, &ir->compact_instructions[i], walk_vreg))
          other_def = 1;
      if (other_def)
        continue;
    }

    /* T must live only inside this body, and only from this one def. */
    int confined = 1;
    for (int i = 0; i < n && confined; i++)
    {
      if (i >= body_start && i <= body_end)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[i];
      for (int w = 0; w < 4; w++)
      {
        IROperand *o = lbt_operand_slot(ir, q, w);
        if (o && irop_get_vreg(*o) == tv)
        {
          confined = 0;
          break;
        }
      }
    }
    if (!confined)
      continue;
    int redefined = 0, used_after_bump = 0;
    for (int i = c + 1; i <= body_end && !redefined; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      for (int w = 0; w < 4; w++)
      {
        IROperand *o = lbt_operand_slot(ir, q, w);
        if (!o || irop_get_vreg(*o) != tv)
          continue;
        if (w == 0 && irop_config[q->op].has_dest && !o->is_lval)
        {
          redefined = 1;
          break;
        }
        if (i > bump_idx)
          used_after_bump = 1;
      }
    }
    if (redefined || used_after_bump)
      continue;

    for (int i = c + 1; i <= body_end; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      for (int w = 0; w < 4; w++)
      {
        IROperand *o = lbt_operand_slot(ir, q, w);
        if (o && irop_get_vreg(*o) == tv)
          irop_set_vreg(o, walk_vreg);
      }
    }
    cq->op = TCCIR_OP_NOP;
    forwarded++;
  }
  return forwarded;
}

/* Match and rewrite the loop whose back-edge JUMP sits at `be`.  Returns 1 on a
 * rewrite (indices after `be` have moved), 0 otherwise. */
static int lbt_try_backedge(TCCIRState *ir, int be, int level)
{
  int n = ir->next_instruction_index;
  IRQuadCompact *jmp_q = &ir->compact_instructions[be];
  if (jmp_q->op != TCCIR_OP_JUMP)
    return 0;
  /* A jump landing on the back-edge itself would skip the tail CMP once the
   * slot in front of it is claimed. */
  if (jmp_q->is_jump_target)
    return 0;

  int hi = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jmp_q));
  if (hi < 0 || hi >= be)
    return 0;

  IRQuadCompact *cmp_q = &ir->compact_instructions[hi];
  if (cmp_q->op != TCCIR_OP_CMP)
    return 0;
  int jif = lbt_skip_nops(ir, hi + 1);
  if (jif >= be)
    return 0;
  IRQuadCompact *jif_q = &ir->compact_instructions[jif];
  if (jif_q->op != TCCIR_OP_JUMPIF)
    return 0;
  /* Nothing may reach the exit test without the CMP that feeds it. */
  if (jif_q->is_jump_target)
    return 0;

  int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jif_q));
  int inv_cond = invert_condition(cond);
  if (inv_cond < 0)
    return 0;
  int exit_target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jif_q));
  /* The guard's target must leave the loop forwards; a target inside would make
   * the fall-through after the tail test wrong. */
  if (exit_target <= be || exit_target > n)
    return 0;

  int body_start = lbt_skip_nops(ir, jif + 1);
  if (body_start >= be)
    return 0; /* empty body: nothing to bottom-test */

  IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cmp_q);
  if (!lbt_operand_reusable(ir, cmp_src1) || !lbt_operand_reusable(ir, cmp_src2))
    return 0;
  /* A CMP fused with a barrel shift or any other orig_index-keyed annotation
   * cannot be duplicated: the copy gets a fresh index and would compare the
   * unshifted value (the pr71083 family).  Nothing sets these before regalloc
   * today, but the check costs nothing and pins the assumption. */
  if (tcc_ir_barrel_shift_at(ir, cmp_q))
    return 0;

  if (!lbt_body_is_straight_line(ir, body_start, be - 1))
    return 0;

  int32_t vr1 = irop_has_vreg(cmp_src1) ? irop_get_vreg(cmp_src1) : -1;
  int32_t vr2 = irop_has_vreg(cmp_src2) ? irop_get_vreg(cmp_src2) : -1;
  int32_t walk_vreg = -1;
  int bump_idx = -1;
  if (!lbt_body_profitable(ir, body_start, be - 1, vr1, vr2, level, &walk_vreg, &bump_idx))
    return 0;
  if (level < 3 && lbt_body_has_scaled_index(ir, body_start, be - 1))
    return 0;
  if (level < 2 && !lbt_walk_is_postinc_shaped(ir, body_start, bump_idx, walk_vreg))
    return 0;

  lbt_forward_body_copies(ir, body_start, be - 1, walk_vreg, bump_idx);

  /* Does the tail test fall straight into the exit, or does it need a jump? */
  int need_exit_jump = lbt_skip_nops(ir, be + 1) != exit_target;

  /* Claim the slots BEFORE writing anything, so a failed allocation leaves the
   * loop exactly as it was (a bare NOP at worst) rather than a half-built tail
   * test in front of a still-unconditional back-edge.  insert_instr_at
   * renumbers every jump target and switch arm past the insertion point, and
   * exit_target is past it by the check above. */
  IROperand none_op = irop_make_none();
  if (insert_instr_at(ir, be, TCCIR_OP_NOP, none_op, none_op, none_op) < 0)
    return 0;
  exit_target++;
  int cmp_pos = be;
  int jmp_pos = be + 1;
  if (need_exit_jump)
  {
    if (insert_instr_at(ir, jmp_pos + 1, TCCIR_OP_NOP, none_op, none_op, none_op) < 0)
      return 0;
    exit_target++;
  }

  write_instr_at_nop(ir, cmp_pos, TCCIR_OP_CMP, none_op, cmp_src1, cmp_src2);
  ir->compact_instructions[cmp_pos].line_num = ir->compact_instructions[hi].line_num;

  if (need_exit_jump)
  {
    write_instr_at_nop(ir, jmp_pos + 1, TCCIR_OP_JUMP,
                       irop_make_imm32(-1, exit_target, IROP_BTYPE_INT32), none_op, none_op);
    if (exit_target < ir->next_instruction_index)
      ir->compact_instructions[exit_target].is_jump_target = 1;
  }

  ir->compact_instructions[jmp_pos].op = TCCIR_OP_NOP;
  write_instr_at_nop(ir, jmp_pos, TCCIR_OP_JUMPIF, irop_make_imm32(-1, body_start, IROP_BTYPE_INT32),
                     irop_make_imm32(-1, inv_cond, IROP_BTYPE_INT32), none_op);
  ir->compact_instructions[jmp_pos].line_num = ir->compact_instructions[hi].line_num;
  ir->compact_instructions[body_start].is_jump_target = 1;
  /* Ask codegen to word-align the head.  The flag goes on the first REAL
   * instruction, not on body_start: the copy forwarding above may have left a
   * NOP there, and tcc_ir_opt_compact_nops (which runs a few lines later in
   * the pipeline) would drop it and the flag with it.  A back-edge branch onto
   * a halfword-odd 32-bit instruction costs about a cycle per iteration on the
   * RP2350 -- see IRQuadCompact.align_target. */
  ir->compact_instructions[lbt_skip_nops(ir, body_start)].align_target = 1;

  LOG_IR_GEN("[LOOP-BOTTOM-TEST] header=%d body=[%d..%d] -> tail CMP@%d JUMPIF@%d exit=%d", hi, body_start,
             cmp_pos - 1, cmp_pos, jmp_pos, exit_target);
  return 1;
}

int ssa_opt_loop_bottom_test(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  int level = lbt_level_env();
  if (level <= 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;

  int total = 0;
  int scan = 0;
  while (total < SSA_LBT_MAX_REWRITES)
  {
    int hit = 0;
    for (int i = scan; i < ir->next_instruction_index; i++)
    {
      if (lbt_try_backedge(ir, i, level))
      {
        total++;
        /* the rewritten back-edge is now a JUMPIF; resume past it */
        scan = i + 2;
        hit = 1;
        break;
      }
    }
    if (!hit)
      break;
  }
  return total;
}
