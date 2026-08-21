/*
 *  TCC IR - Compare operand canonicalization (constant to the right)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"

/* `cmp` takes its immediate on the right only, so a compare whose *first*
 * operand is the constant costs an extra `mov` to park it in a register:
 *
 *     movs r1, #0        cmp  r0, #0
 *     cmp  r1, r0   vs   ite  gt
 *     ite  lt            movgt.w r0, #1192
 *
 * The frontend already puts the constant second where it can; the shape here
 * is what the *optimizer* leaves behind, when a pass folds the first operand
 * to a constant after the compare was emitted.  The loop guard `n < iterations`
 * of a counted loop whose body turns out to be invariant becomes `0 < P0` in
 * exactly this way (sccp folds the counter to its initial value), which is why
 * every `for (n = 0; n < iterations; n++)` benchmark head paid for it.
 *
 * Exchanging the two operands and mirroring the condition on every flag reader
 * (`0 < x` -> `x > 0`) is value-preserving for signed and unsigned compares
 * alike -- ARM's condition codes implement the mathematical predicate, overflow
 * included -- so the only work is proving the reader set is complete.
 *
 * Placement: after register allocation (the SSA engine, sccp included, runs
 * inside it) but before codegen, which is what materializes a compare's
 * immediate operand into a scratch register.
 *
 * 64-bit compares are out of scope: their CMP/SBCS lowering has no GT/LE form,
 * so the frontend reaches those by exchanging the operands itself and undoing
 * that here would ask the backend for codes it does not have.  cmp_narrow_64
 * already performs this swap for the 64-bit compares it can narrow to 32.  */

/* Indices control can enter from somewhere other than the preceding
 * instruction.  The IRQuadCompact `is_jump_target` bit is not usable on its
 * own: it survives the removal of the branch that set it, and an eliminated
 * loop leaves its former header marked with no predecessor left -- which is
 * exactly the compare this pass exists to fix.  Recomputing the set from the
 * branches actually present is precise.  A computed jump has no enumerable
 * target list, so a function holding one falls back to the stale-but-safe bit,
 * which is a superset of where such a jump could land. */
static uint8_t *cis_build_entry_map(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  uint8_t *entry = tcc_mallocz((n + 7) / 8);
  int indirect = 0;

  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
    {
      indirect = 1;
      continue;
    }
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_is_none(d))
      continue;
    int t = (int)d.u.imm32;
    if (t >= 0 && t < n)
      entry[t / 8] |= (uint8_t)(1 << (t % 8));
  }

  if (indirect)
    for (int j = 0; j < n; j++)
      if (ir->compact_instructions[j].is_jump_target)
        entry[j / 8] |= (uint8_t)(1 << (j % 8));

  return entry;
}

/* Readers of one CMP's flags, or -1 if the set cannot be determined exactly.
 * Walks forward from the compare with a strict rule: only NOP and ASSIGN may
 * be passed through (ASSIGN lowers to `mov`, which preserves the flags -- the
 * same invariant the backend's relational lowering relies on to keep a CMP's
 * flags live across the phi-resolution copies scheduled before the branch).
 * Anything not known to either read the flags or end their life bails out. */
static int cis_collect_readers(TCCIRState *ir, int cmp_idx, const uint8_t *entry,
                               int *readers, int max_readers)
{
  int n = ir->next_instruction_index;
  int count = 0;

  for (int j = cmp_idx + 1; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];

    /* Control can enter here carrying somebody else's flags, so a reader at or
     * below this point is not ours alone to rewrite.  Checked ahead of the NOP
     * skip: a NOP can be a branch target too, and stepping over one would walk
     * into a reader this compare does not own. */
    if (entry[j / 8] & (1 << (j % 8)))
      return -1;

    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    case TCCIR_OP_ASSIGN:
      continue;

    case TCCIR_OP_SETIF:
    case TCCIR_OP_SELECT:
      if (count >= max_readers)
        return -1;
      readers[count++] = j;
      continue;

    case TCCIR_OP_JUMPIF:
      /* Consumes the flags and diverges; nothing downstream reads them. */
      if (count >= max_readers)
        return -1;
      readers[count++] = j;
      return count;

    /* The flags die here: either overwritten, or control leaves for good.
     * A call kills them too -- AAPCS lets a callee clobber the APSR flags. */
    case TCCIR_OP_CMP:
    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      return count;

    default:
      /* Everything else refuses rather than rewrites a partial reader set:
       * an unmodelled op that preserves the flags would hide a reader past
       * this point.  That deliberately includes the control transfers whose
       * successors this linear walk does not visit -- JUMP, IJUMP and
       * SWITCH_TABLE -- since flags do survive a branch. */
      return -1;
    }
  }
  return count;
}

/* The condition of a flag reader lives in src1 for SETIF/JUMPIF and in the
 * cond slot (operand_base + 3, shared with MLA's accumulator) for SELECT. */
static IROperand cis_reader_cond(TCCIRState *ir, IRQuadCompact *q)
{
  if (q->op == TCCIR_OP_SELECT)
    return tcc_ir_op_get_cond(ir, q);
  return tcc_ir_op_get_src1(ir, q);
}

static void cis_set_reader_cond(TCCIRState *ir, int idx, IROperand cond)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op == TCCIR_OP_SELECT)
    tcc_ir_op_set_accum(ir, q, cond);
  else
    tcc_ir_set_src1(ir, idx, cond);
}

int tcc_ir_opt_cmp_imm_swap(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  uint8_t *entry = cis_build_entry_map(ir);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Only the constant-first shape, and only when the exchange actually moves
     * the constant into `cmp`'s immediate slot (two constants are const-folding's
     * business, not ours). */
    if (!irop_is_immediate(src1) || irop_is_immediate(src2))
      continue;
    if (irop_is_64bit(src1) || irop_is_64bit(src2))
      continue;

    /* A barrel-shift annotation binds to the src2 SLOT of its instruction:
     * the compare really reads (src2 SHIFT #n).  The exchange would re-anchor
     * the shift onto the former constant, and there is no src1-shifted form
     * to move it to, so such a compare cannot be swapped at all. */
    if (tcc_ir_barrel_shift_at(ir, q))
      continue;

    enum { CIS_MAX_READERS = 8 };
    int readers[CIS_MAX_READERS];
    int nreaders = cis_collect_readers(ir, i, entry, readers, CIS_MAX_READERS);
    if (nreaders <= 0)
      continue;

    int mirrored[CIS_MAX_READERS];
    int ok = 1;
    for (int r = 0; r < nreaders; r++)
    {
      IROperand c = cis_reader_cond(ir, &ir->compact_instructions[readers[r]]);
      if (!irop_is_immediate(c))
      {
        ok = 0;
        break;
      }
      mirrored[r] = swap_cond_token((int)irop_get_imm64_ex(ir, c));
      if (mirrored[r] < 0)
      {
        ok = 0;
        break;
      }
    }
    if (!ok)
      continue;

    LOG_IR_GEN("OPTIMIZE: cmp_imm_swap at i=%d (%d reader(s))", i, nreaders);
    for (int r = 0; r < nreaders; r++)
    {
      IROperand c = cis_reader_cond(ir, &ir->compact_instructions[readers[r]]);
      cis_set_reader_cond(ir, readers[r], irop_make_imm32(-1, mirrored[r], irop_get_btype(c)));
    }
    tcc_ir_set_src1(ir, i, src2);
    tcc_ir_set_src2(ir, i, src1);
    changes++;
  }

  tcc_free(entry);
  return changes;
}

int tcc_ir_opt_cmp_imm_swap_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_imm_swap(ctx->ir); }
