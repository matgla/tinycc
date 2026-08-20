/*
 *  TCC IR - Loop-Invariant Code Motion (LICM) Optimization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "licm.h"
#include "opt.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "cfg.h"
#include "core.h"
#include "pool.h"
#include "vreg.h"
#include <string.h>



/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* ============================================================================
 * Loop-Invariant Identification and Hoisting
 * ============================================================================ */

/* Check if a loop contains VLA (Variable Length Array) allocations.
 * VLA allocations have special stack semantics - they dynamically adjust SP
 * each iteration based on runtime-computed sizes. Hoisting function calls
 * that compute VLA sizes out of the loop corrupts the VLA stack management.
 *
 * Example: char buf[strlen(str) + 10] inside a loop
 * The strlen() call computes the VLA size and must execute at the VLA_ALLOC point.
 */
static int loop_contains_vla(TCCIRState *ir, IRLoop *loop)
{
  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    int instr_idx = loop->body_instrs[i];
    IRQuadCompact *q = &ir->compact_instructions[instr_idx];

    if (q->op == TCCIR_OP_VLA_ALLOC)
    {
      return 1;
    }
  }
  return 0;
}

/* Check if an operand is loop-invariant
 * An operand is loop-invariant if:
 * 1. It's a constant (immediate)
 * 2. It's a vreg defined outside the loop
 * 3. It's a vreg defined by ASSIGN from another loop-invariant vreg (transitively invariant)
 *
 * The hoisted_vregs array contains vregs that were hoisted in previous iterations.
 * These are considered loop-invariant even if they have an ASSIGN in the loop body.
 */
/* True if the address of `vreg`'s stack slot is taken anywhere in the
 * function: a SOURCE operand carrying this vreg with STACKOFF tag and
 * is_lval == 0 (the IR's `&V` form — see the dump printer).  Once the
 * address escapes, any store through any pointer may mutate the variable,
 * so a "no direct def in the loop" scan is not sufficient for invariance
 * (docs/bugs.md #7: ptr fuzz seeds 500/517 — helper3(#imm, V7) hoisted out
 * of a loop that mutated V7 through the pointers p14 and p15). */
static int vreg_addr_taken_anywhere(TCCIRState *ir, int32_t vreg)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand srcs[3];
    int nsrcs = 0;
    if (irop_config[q->op].has_src1)
      srcs[nsrcs++] = tcc_ir_op_get_src1(ir, q);
    if (irop_config[q->op].has_src2)
      srcs[nsrcs++] = tcc_ir_op_get_src2(ir, q);
    if (q->op == TCCIR_OP_MLA)
      srcs[nsrcs++] = tcc_ir_op_get_accum(ir, q);
    for (int s = 0; s < nsrcs; s++)
    {
      if (irop_get_tag(srcs[s]) == IROP_TAG_STACKOFF && !srcs[s].is_lval &&
          irop_get_vreg(srcs[s]) == vreg)
        return 1;
    }
  }
  return 0;
}

static int loop_body_may_clobber_memory(TCCIRState *ir, IRLoop *loop);

static int is_operand_loop_invariant_ex(TCCIRState *ir, IROperand op, IRLoop *loop, int32_t *hoisted_vregs,
                                        int num_hoisted_vregs)
{
  /* Constants are always loop-invariant */
  if (irop_is_immediate(op))
    return 1;

  /* Check vreg - if defined inside loop, not invariant */
  int32_t vreg = irop_get_vreg(op);
  if (vreg < 0)
  {
    /* No vreg.  Only treat as invariant if it's a true constant (IMM32/I64).
     * Stack locals, symbols, and other non-constant operands without vregs
     * may be modified inside the loop and must be treated conservatively. */
    if (irop_is_immediate(op) && !op.is_sym && !op.is_lval && !op.is_local)
      return 1;
    return 0;
  }

  /* Check if this vreg was already hoisted AND has no other definitions
   * inside the loop.  A vreg defined by a hoisted call but ALSO redefined
   * by other instructions in the loop is NOT invariant. */
  {
    int is_hoisted = 0;
    for (int h = 0; h < num_hoisted_vregs; h++)
    {
      if (hoisted_vregs[h] == vreg)
      { is_hoisted = 1; break; }
    }
    if (is_hoisted)
    {
      int def_count = 0;
      for (int di = 0; di < loop->num_body_instrs; di++)
      {
        int didx = loop->body_instrs[di];
        IRQuadCompact *dq = &ir->compact_instructions[didx];
        if (dq->op == TCCIR_OP_NOP || !irop_config[dq->op].has_dest)
          continue;
        if (irop_get_vreg(tcc_ir_op_get_dest(ir, dq)) == vreg)
          def_count++;
      }
      if (def_count <= 1)
        return 1; /* Single def from hoisted call — invariant */
      /* Multiple defs — not invariant despite hoisted vreg */
    }
  }

  /* Find where this vreg is defined */
  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    int instr_idx = loop->body_instrs[i];
    IRQuadCompact *q = &ir->compact_instructions[instr_idx];

    if (!irop_config[q->op].has_dest)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) == vreg)
    {
      /* This vreg is defined inside the loop.
       * Check if it's an ASSIGN from a hoisted vreg (transitively invariant) */
      if (q->op == TCCIR_OP_ASSIGN)
      {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        int32_t src_vreg = irop_get_vreg(src);
        if (src_vreg >= 0)
        {
          for (int h = 0; h < num_hoisted_vregs; h++)
          {
            if (hoisted_vregs[h] == src_vreg)
              return 1; /* Assigned from hoisted vreg - loop invariant */
          }
        }
      }
      /* Otherwise not invariant */
      return 0;
    }
  }

  /* No direct def in the loop.  The value can STILL change across iterations
   * if the variable's address has been taken: any store through a pointer or
   * any non-CONST call inside the loop may then mutate its stack slot without
   * a visible def of the vreg (docs/bugs.md #7, ptr seeds 500/517).  Only
   * accept an address-taken variable when the loop provably cannot write
   * memory at all. */
  if (vreg_addr_taken_anywhere(ir, vreg) && loop_body_may_clobber_memory(ir, loop))
    return 0;

  /* Vreg not defined in loop - it's loop-invariant */
  return 1;
}

/* Does the loop body contain anything that could modify memory a PURE function
 * might read?  PR20100: a PURE function (as opposed to CONST) reads global/heap
 * memory, so its result is only loop-invariant if that memory is unchanged
 * across iterations.  Any store, or any call that is not itself CONST (an
 * IMPURE/UNKNOWN callee — or an indirect call — may write memory), can change
 * what a PURE callee observes, so hoisting it would be a miscompile.  CONST
 * callees read no memory and are unaffected by this. */
/* CFG-block flavour of loop_body_may_clobber_memory, for the dominance-based
 * LICM below (which works on an `in_loop[]` block mask, not an IRLoop). */
static int cfg_loop_may_clobber_memory(TCCIRState *ir, IRCFG *cfg, const uint8_t *in_loop)
{
  for (int bi = 0; bi < cfg->num_blocks; bi++)
  {
    if (!in_loop[bi])
      continue;
    for (int ii = cfg->blocks[bi].start_idx; ii < cfg->blocks[bi].end_idx; ii++)
    {
      IRQuadCompact *q = &ir->compact_instructions[ii];
      switch (q->op)
      {
      case TCCIR_OP_NOP:
        continue;
      case TCCIR_OP_STORE:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_BLOCK_COPY:
      case TCCIR_OP_INLINE_ASM:
      case TCCIR_OP_ASM_OUTPUT:
        return 1;
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      {
        Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
        if (!callee || tcc_ir_get_func_purity(ir, callee) < TCC_FUNC_PURITY_CONST)
          return 1;
        continue;
      }
      default:
        if (irop_config[q->op].has_dest && tcc_ir_op_get_dest(ir, q).is_lval)
          return 1;
        continue;
      }
    }
  }
  return 0;
}

static int loop_body_may_clobber_memory(TCCIRState *ir, IRLoop *loop)
{
  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[loop->body_instrs[i]];
    switch (q->op)
    {
    case TCCIR_OP_NOP:
      continue;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
      return 1;
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_OUTPUT:
      return 1; /* inline asm may write arbitrary memory */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!callee || tcc_ir_get_func_purity(ir, callee) < TCC_FUNC_PURITY_CONST)
        return 1; /* indirect / impure / merely-pure call may write memory */
      continue;
    }
    default:
      /* A memory store through a non-STORE op shows up as an lval destination. */
      if (irop_config[q->op].has_dest && tcc_ir_op_get_dest(ir, q).is_lval)
        return 1;
      continue;
    }
  }
  return 0;
}

/* Check if a function call instruction can be hoisted
 * Requirements:
 * 1. Function is pure or const
 * 2. All arguments are loop-invariant (considering already-hoisted vregs)
 * 3. If the function is PURE (reads memory) rather than CONST, the loop must
 *    not modify any memory it could read (PR20100)
 */
static int tcc_ir_is_hoistable_call_ex(TCCIRState *ir, int instr_idx, IRLoop *loop, int32_t *hoisted_vregs,
                                       int num_hoisted_vregs)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];

  if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
    return 0; /* Not a function call */

  /* For FUNCCALLVAL, the destination must be a vreg (not a memory location).
   * If the result is stored directly to a global/local variable, we can't
   * simply hoist it because we'd need to also handle the store operation.
   * This fixes a bug where hoisting a call with memory destination corrupted
   * the IR by treating the memory operand as a vreg. */
  if (q->op == TCCIR_OP_FUNCCALLVAL)
  {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(dest) != IROP_TAG_VREG)
    {
      LOG_LICM("Call at %d: destination is not a vreg, can't hoist", instr_idx);
      return 0;
    }
    /* A pair-returning call (soft-double / long long result in r0:r1):
     * hoisting stretches the pair's live range across every call in the loop
     * body, and regalloc only parks the LOW half in a callee-saved register —
     * the loop-tail phi copy then reads the high half from a clobbered r1
     * (fuzz seed float:118, test 387).  Bail until hoisted-pair live ranges
     * are modeled. */
    if (irop_is_64bit(dest))
    {
      LOG_LICM("Call at %d: pair-returning result, can't hoist", instr_idx);
      return 0;
    }
  }

  /* Get function symbol from src1 */
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  Sym *func_sym = irop_get_sym_ex(ir, src1);

  if (!func_sym)
  {
    /* Indirect call - can't determine purity */
    LOG_LICM("Call at %d: indirect call, can't hoist", instr_idx);
    return 0;
  }

  /* Check function purity */
  int purity = tcc_ir_get_func_purity(ir, func_sym);
  if (purity < TCC_FUNC_PURITY_PURE)
  {
    /* Function has side effects or is unknown - can't hoist */
    return 0;
  }

  /* A merely-PURE function reads memory; hoisting it is only safe if the loop
   * cannot change what it reads.  A CONST function reads nothing and is always
   * safe (PR20100 / docs/bugs.md #7). */
  if (purity < TCC_FUNC_PURITY_CONST && loop_body_may_clobber_memory(ir, loop))
  {
    LOG_LICM("Call at %d: PURE (not CONST) and loop clobbers memory — not hoistable", instr_idx);
    return 0;
  }

  LOG_LICM("Call at %d: function is pure (purity=%d), checking args...", instr_idx, purity);

  /* Find all FUNCPARAMVAL instructions for this call */
  IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
  int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, call_src2));

  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    int param_idx = loop->body_instrs[i];
    IRQuadCompact *param_q = &ir->compact_instructions[param_idx];

    if (param_q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;

    IROperand param_src2 = tcc_ir_op_get_src2(ir, param_q);
    int param_call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, param_src2));
    if (param_call_id != call_id)
      continue; /* Parameter for a different call */

    /* Check if the parameter value is loop-invariant (considering hoisted vregs) */
    IROperand param_src = tcc_ir_op_get_src1(ir, param_q);
    if (!is_operand_loop_invariant_ex(ir, param_src, loop, hoisted_vregs, num_hoisted_vregs))
    {
      return 0; /* Argument not loop-invariant */
    }
  }

  /* Function is pure and all arguments are loop-invariant - can hoist */
  return 1;
}

/* Maximum number of pure calls to hoist per loop */
#define MAX_HOISTABLE_CALLS 16

typedef struct
{
  int instr_idx;        /* Index of FUNCCALLVAL/CALLVOID instruction */
  int32_t hoisted_vreg; /* New vreg for hoisted result (if VAL) */
  int is_hoisted;
} HoistableCallInfo;

/* Collect all param markers belonging to a call.  BOTH FUNCPARAMVAL (a value
 * argument) and FUNCPARAMVOID (the marker a zero-argument or void call still
 * carries, and which the backend pairs with the CALL by call_id) must be
 * collected — otherwise hoisting the CALL but leaving its FUNCPARAMVOID behind
 * orphans the marker ("no call site found for call_id=N") and the hoisted call
 * loses its marker ("missing FUNCPARAMVAL").  See docs/bugs.md #7. */
static int collect_call_params(TCCIRState *ir, int call_idx, int *param_indices, int max_params)
{
  IRQuadCompact *call_q = &ir->compact_instructions[call_idx];
  IROperand call_src2 = tcc_ir_op_get_src2(ir, call_q);
  int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, call_src2));
  int num_params = 0;

  /* Scan all instructions for params/markers with matching call_id */
  for (int i = 0; i < ir->next_instruction_index && num_params < max_params; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int param_call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
      if (param_call_id == call_id)
      {
        param_indices[num_params++] = i;
      }
    }
  }

  return num_params;
}

/* Hoist pure function calls from loops
 * This is Phase 1 of FUNCTION_CALLS_OPTIMIZATION_PLAN
 */
int tcc_ir_hoist_pure_calls(TCCIRState *ir, IRLoops *loops)
{
  if (!ir || !loops)
    return 0;

  /* Re-enabled 2026-07-02 after the ninth (and final) defect fix: the
   * combo-profile residue (seeds 52/80/187/311/333/392/460) was
   * tcc_ir_insert_instruction_before desynchronizing SWITCH_TABLE side-table
   * targets — not the linear-index call bookkeeping suspected earlier.
   * Full history in docs/bugs.md #7 (resolved). */

  /* Kill-switch for bisection: TCC_DISABLE_PASS=pure_call_hoist. */
  if (tcc_ir_opt_pass_disabled("pure_call_hoist"))
    return 0;

  int total_hoisted = 0;

  for (int loop_idx = 0; loop_idx < loops->num_loops; loop_idx++)
  {
    IRLoop *loop = &loops->loops[loop_idx];

    /* total_hoisted accumulates across ALL loops; the post-loop index fix-up
     * below must shift by only THIS loop's insertions, otherwise a later loop
     * (already shifted by an earlier loop's insertions) is over-shifted.
     * Snapshot the running total to recover the per-loop delta. */
    int total_hoisted_at_loop_start = total_hoisted;

    if (loop->preheader_idx < 0)
      continue; /* No preheader - can't hoist */

    /* Skip loops whose preheader is inside another loop's body.
     * This prevents hoisting INTO an enclosing loop instead of BEFORE it.
     *
     * Example of problematic pattern (for loop with body after increment):
     *   3: CMP i,5         <- outer loop header
     *   4: JMPIF exit
     *   5: JMP body (9)
     *   6: NOP             <- "inner loop" header (fake)
     *   7: i++
     *   8: JMP 3           <- outer loop back edge
     *   9: strlen()        <- loop body (hoistable)
     *  12: JMP 6           <- back edge detected as "inner loop"
     *
     * The "inner loop" (6-12) has preheader=3, but 3 is the OUTER loop's header!
     * Hoisting to preheader+1=4 places code INSIDE the outer loop.
     */
    int preheader_in_other_loop = 0;
    for (int other_idx = 0; other_idx < loops->num_loops; other_idx++)
    {
      if (other_idx == loop_idx)
        continue;
      IRLoop *other = &loops->loops[other_idx];
      /* Check if this loop's preheader is inside another loop's range */
      if (loop->preheader_idx >= other->start_idx && loop->preheader_idx <= other->end_idx)
      {
        preheader_in_other_loop = 1;
        break;
      }
    }
    if (preheader_in_other_loop)
      continue;

    /* The hoist inserts at preheader_idx+1 and relies on control FALLING
     * THROUGH from the preheader into the loop header.  tcc_ir_detect_loops'
     * preheader walk skips backward over JUMP/JUMPIF, so preheader_idx may
     * belong to a block that never reaches this loop.  docs/bugs.md #7,
     * combo fuzz seed 18: the hoisted call landed on a bypass path just
     * ahead of an unconditional JMP while the loop itself was entered by a
     * jump straight to the header — the loop then read the hoisted result
     * vreg UNDEFINED (wrong checksum; an undefined loop bound turns into an
     * infinite loop).  Two requirements make the insertion point sound:
     *   1. the preheader is the header's immediate predecessor (nothing was
     *      skipped — control genuinely falls from it into the header), and
     *   2. no jump from OUTSIDE the loop targets the header (such an entry
     *      edge would bypass the inserted preheader code).  Back-edges and
     *      `continue`-style jumps from inside are fine: on any path that
     *      reaches them, the hoisted call has already executed. */
    if (loop->preheader_idx != loop->header_idx - 1)
    {
      LOG_LICM("Skipping loop %d: preheader %d is not the header %d's immediate predecessor", loop_idx,
               loop->preheader_idx, loop->header_idx);
      continue;
    }
    {
      /* Reject any entry edge from outside the loop's linear range into ANY
       * instruction of [header, end] — not just the header.  An edge into
       * the header bypasses the inserted preheader code (docs/bugs.md #7,
       * combo seed 18); an edge into the middle of the range would break the
       * header's dominance over the call sites we rewrite (the hoisted
       * result vreg could be read on a path that never ran the preheader).
       * Note this also skips increment-trampoline rotated loops, whose
       * physical body jumps back into [header, end] from linearly outside —
       * their linear range holds only guard/increment code, so nothing
       * hoistable is lost. */
      int external_entry = 0;
      for (int j = 0; j < ir->next_instruction_index && !external_entry; j++)
      {
        if (j >= loop->start_idx && j <= loop->end_idx)
          continue; /* jumps from inside the loop are fine */
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
        {
          int jt = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jq));
          if (jt >= loop->header_idx && jt <= loop->end_idx)
            external_entry = 1;
        }
        else if (jq->op == TCCIR_OP_SWITCH_TABLE)
        {
          /* A switch outside the loop with a case/default target in the
           * range is an entry edge, same as a plain JUMP. */
          int table_id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, jq));
          if (table_id >= 0 && table_id < ir->num_switch_tables)
          {
            TCCIRSwitchTable *table = &ir->switch_tables[table_id];
            if (table->default_target >= loop->header_idx && table->default_target <= loop->end_idx)
              external_entry = 1;
            for (int ti = 0; table->targets && ti < table->num_entries && !external_entry; ti++)
            {
              if (table->targets[ti] >= loop->header_idx && table->targets[ti] <= loop->end_idx)
                external_entry = 1;
            }
          }
        }
        else if (jq->op == TCCIR_OP_IJUMP)
        {
          /* Indirect jump: target unknowable — conservatively treat it as a
           * possible entry edge into the loop. */
          external_entry = 1;
        }
      }
      if (external_entry)
      {
        LOG_LICM("Skipping loop %d: header %d is entered by a jump from outside the loop", loop_idx,
                 loop->header_idx);
        continue;
      }
    }

    /* Skip loops containing VLA allocations.
     * VLAs have special stack semantics - the size is computed at runtime
     * and SP is adjusted dynamically. Hoisting a pure function call that
     * computes the VLA size (e.g., strlen() in "char buf[strlen(s)+1]")
     * breaks the VLA stack management because the call must execute at
     * the point of VLA_ALLOC, not in the preheader.
     *
     * Test case: 123_vla_bug.c - has VLA inside switch/case in a for loop.
     */
    if (loop_contains_vla(ir, loop))
    {
      LOG_LICM("Skipping loop %d with VLA allocations", loop_idx);
      continue;
    }

    /* Iterative pure call hoisting:
     * Some calls may become hoistable after we hoist earlier calls in the chain.
     * For example: result = func_a(100); result = func_b(result);
     * Initially only func_a(100) is hoistable. After hoisting it, func_b(result)
     * becomes hoistable because 'result' is now defined outside the loop.
     *
     * We iterate until no more calls can be hoisted.
     */

    /* Collect ALL pure function calls in this loop (for iterative checking) */
    int all_call_indices[MAX_HOISTABLE_CALLS];
    int num_all_calls = 0;

    LOG_LICM("Scanning loop %d with %d body instructions for pure calls", loop_idx, loop->num_body_instrs);

    for (int i = 0; i < loop->num_body_instrs && num_all_calls < MAX_HOISTABLE_CALLS; i++)
    {
      int instr_idx = loop->body_instrs[i];

      /* body_instrs is an OVER-approximation: tcc_ir_detect_loops' forward-
       * jump "extension" (any jump out of [start,end] whose target is within
       * +50 of the header extends the body, with no path-back-to-header
       * check) can swallow post-loop code.  volatile fuzz seeds 3583/6116: a
       * rotated for-loop's exit jump pulled the else arm of the enclosing
       * if/else into body_instrs, and the two else-arm calls were "hoisted"
       * above the loop — onto the then-path — while the else path entered at
       * its own label and read both result vregs UNDEFINED.  The over-
       * approximation is conservative (correct) for the clobber/invariance
       * scans, but calls may only be hoisted from the certain linear range:
       * with the preheader fall-through + no-external-entry guards above,
       * every instruction in [start,end] is dominated by the header, so the
       * preheader insertion point dominates the rewritten call site. */
      if (instr_idx < loop->start_idx || instr_idx > loop->end_idx)
        continue;

      IRQuadCompact *q = &ir->compact_instructions[instr_idx];

      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        /* Check basic requirements (pure function, vreg dest) but NOT argument invariance yet */
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        Sym *func_sym = irop_get_sym_ex(ir, src1);
        if (func_sym && tcc_ir_get_func_purity(ir, func_sym) >= TCC_FUNC_PURITY_PURE)
        {
          if (q->op == TCCIR_OP_FUNCCALLVOID ||
              (q->op == TCCIR_OP_FUNCCALLVAL && irop_get_tag(tcc_ir_op_get_dest(ir, q)) == IROP_TAG_VREG))
          {
            all_call_indices[num_all_calls++] = instr_idx;
          }
        }
      }
    }

    if (num_all_calls == 0)
    {
      LOG_LICM("No pure calls found in loop %d", loop_idx);
      continue;
    }

    /* Track hoisted vregs for iterative detection.
     * This maps hoisted_vreg -> original_vreg so we can detect transitively invariant operands.
     * We store both the hoisted vreg (defined in preheader) and the original vreg (now assigned
     * from hoisted vreg in loop body). */
    int32_t hoisted_vregs[MAX_HOISTABLE_CALLS];
    int num_hoisted_vregs = 0;
    int hoisted_call_flags[MAX_HOISTABLE_CALLS] = {0}; /* Track which calls have been hoisted */

    /* Iterative hoisting loop */
    int hoisted_this_iteration;
    do
    {
      hoisted_this_iteration = 0;

      LOG_LICM("Iteration: checking %d pure calls", num_all_calls);

      /* Find hoistable function calls in this loop */
      HoistableCallInfo hoistable[MAX_HOISTABLE_CALLS];
      int num_hoistable = 0;

      for (int i = 0; i < num_all_calls && num_hoistable < MAX_HOISTABLE_CALLS; i++)
      {
        if (hoisted_call_flags[i])
          continue; /* Already hoisted */

        int instr_idx = all_call_indices[i];
        IRQuadCompact *q = &ir->compact_instructions[instr_idx];

        /* Skip if already NOP'd (from previous hoisting) */
        if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_ASSIGN)
          continue;

        LOG_LICM("Found call at instruction %d, checking hoistability...", instr_idx);
        if (tcc_ir_is_hoistable_call_ex(ir, instr_idx, loop, hoisted_vregs, num_hoisted_vregs))
        {
          hoistable[num_hoistable].instr_idx = instr_idx;
          hoistable[num_hoistable].hoisted_vreg = -1;
          hoistable[num_hoistable].is_hoisted = 0;
          num_hoistable++;
          hoisted_call_flags[i] = 1; /* Mark as will-be-hoisted */
        }
      }

      if (num_hoistable == 0)
      {
        LOG_LICM("No more hoistable pure calls found in loop %d", loop_idx);
        break;
      }

      LOG_LICM("Found %d hoistable pure call(s) in loop %d", num_hoistable, loop_idx);

      /* For each hoistable call, we need to:
       * 1. Allocate a NEW call_id for the hoisted call (critical!)
       * 2. Create new vregs for results (if FUNCCALLVAL)
       * 3. Copy PARAM instructions to preheader with NEW call_id
       * 4. Copy CALL instruction to preheader with NEW call_id
       * 5. Replace original call with ASSIGN from hoisted vreg (or NOP for void)
       * 6. NOP out original parameters
       */

      /* Allocate vregs for results */
      for (int i = 0; i < num_hoistable; i++)
      {
        IRQuadCompact *call_q = &ir->compact_instructions[hoistable[i].instr_idx];
        if (call_q->op == TCCIR_OP_FUNCCALLVAL)
        {
          hoistable[i].hoisted_vreg = tcc_ir_vreg_alloc_temp(ir);
        }
      }

      /* Process each hoistable call */
      for (int i = 0; i < num_hoistable; i++)
      {
        int call_idx = hoistable[i].instr_idx;

        /* Get old call_id and argc from the original call */
        IRQuadCompact *orig_call_q = &ir->compact_instructions[call_idx];
        IROperand orig_call_src2 = tcc_ir_op_get_src2(ir, orig_call_q);
        int64_t orig_encoded = irop_get_imm64_ex(ir, orig_call_src2);
        int argc = TCCIR_DECODE_CALL_ARGC(orig_encoded);

        /* Allocate a NEW call_id for the hoisted call */
        int new_call_id = ir->next_call_id++;

        /* Collect all parameters for this call BEFORE any insertions */
        int param_indices[16];
        int num_params = collect_call_params(ir, call_idx, param_indices, 16);

        /* Count insertions for this call to track index shifts */
        int insertions_this_call = 0;

        /* Copy and modify the call instruction */
        IRQuadCompact call_copy = *orig_call_q;

        /* Copy parameter instructions */
        IRQuadCompact param_copies[16];
        for (int p = 0; p < num_params; p++)
        {
          param_copies[p] = ir->compact_instructions[param_indices[p]];
        }

        /* We insert instructions at preheader+1, and each insertion shifts
         * subsequent instructions. To get the correct order (PARAM, PARAM, ..., CALL),
         * we insert in REVERSE order: first CALL, then params from last to first.
         * This way:
         *   Insert CALL at preheader+1 -> [CALL]
         *   Insert PARAM[n-1] at preheader+1 -> [PARAM[n-1], CALL]
         *   Insert PARAM[0] at preheader+1 -> [PARAM[0], ..., PARAM[n-1], CALL]
         */

        /* First, insert the CALL instruction (it will end up LAST) */
        int64_t new_call_encoded = TCCIR_ENCODE_CALL(new_call_id, argc);
        IROperand new_call_src2 = irop_make_imm32(-1, (int32_t)new_call_encoded, IROP_BTYPE_INT32);

        /* Reallocate operand pool for the call copy with the updated call_id.
         * The operand layout is [dest?, src1, src2] where the dest slot exists
         * ONLY when irop_config[op].has_dest is set (FUNCCALLVAL).  A
         * FUNCCALLVOID has no dest, so it must be laid out as [src1, src2];
         * emitting a spurious dest operand for it shifts src1/src2 down by one
         * and makes the accessors read the call_id/argc encoding out of the
         * callee-symref slot instead (decoding to a bogus argc -> the backend
         * then reports "missing FUNCPARAMVAL for call_id=N").  See bugs.md #7. */
        IROperand call_src1 = tcc_ir_op_get_src1(ir, &call_copy);
        int call_has_dest = irop_config[call_copy.op].has_dest;

        if (call_has_dest)
        {
          IROperand call_dest = tcc_ir_op_get_dest(ir, &call_copy);
          if (hoistable[i].hoisted_vreg >= 0)
            call_dest = irop_make_vreg(hoistable[i].hoisted_vreg, IROP_BTYPE_INT32);
          call_copy.operand_base = tcc_ir_pool_add(ir, call_dest);
          tcc_ir_pool_add(ir, call_src1);
        }
        else
        {
          call_copy.operand_base = tcc_ir_pool_add(ir, call_src1);
        }
        tcc_ir_pool_add(ir, new_call_src2);

        tcc_ir_insert_instruction_before(ir, loop->preheader_idx + 1, &call_copy);
        insertions_this_call++;
        total_hoisted++;

        /* Now insert parameters from LAST to FIRST (they will end up in correct order) */
        for (int p = num_params - 1; p >= 0; p--)
        {
          /* Get param_idx from the original encoding */
          IROperand orig_param_src2 = tcc_ir_op_get_src2(ir, &param_copies[p]);
          int64_t orig_param_encoded = irop_get_imm64_ex(ir, orig_param_src2);
          int param_idx = TCCIR_DECODE_PARAM_IDX(orig_param_encoded);

          /* Create new encoding with new_call_id but same param_idx */
          int64_t new_param_encoded = TCCIR_ENCODE_PARAM(new_call_id, param_idx);
          IROperand new_param_src2 = irop_make_imm32(-1, (int32_t)new_param_encoded, IROP_BTYPE_INT32);

          /* Allocate operands per the marker's own irop_config (both have
           * has_dest=0, has_src2=1).  FUNCPARAMVAL has has_src1=1 (the value):
           * layout [src1, src2].  FUNCPARAMVOID has has_src1=0: layout [src2]
           * only — writing a spurious src1 for it would push src2 down a slot
           * and misdecode the call_id (docs/bugs.md #7). */
          int new_operand_base;
          if (irop_config[param_copies[p].op].has_src1)
          {
            new_operand_base = tcc_ir_pool_add(ir, tcc_ir_op_get_src1(ir, &param_copies[p]));
            tcc_ir_pool_add(ir, new_param_src2);
          }
          else
          {
            new_operand_base = tcc_ir_pool_add(ir, new_param_src2);
          }
          param_copies[p].operand_base = new_operand_base;

          tcc_ir_insert_instruction_before(ir, loop->preheader_idx + 1, &param_copies[p]);
          insertions_this_call++;
          total_hoisted++;
        }

        /* Update indices to account for inserted instructions */
        int adjusted_call_idx = call_idx + insertions_this_call;

        /* Replace original call with ASSIGN from hoisted vreg or NOP */
        IRQuadCompact *call_q = &ir->compact_instructions[adjusted_call_idx];
        if (hoistable[i].hoisted_vreg >= 0)
        {
          /* Get original destination from the adjusted position */
          IROperand orig_dest = tcc_ir_op_get_dest(ir, call_q);
          int32_t orig_vreg = irop_get_vreg(orig_dest);

          call_q->op = TCCIR_OP_ASSIGN;
          IROperand hoisted_src = irop_make_vreg(hoistable[i].hoisted_vreg, IROP_BTYPE_INT32);
          tcc_ir_set_src1(ir, adjusted_call_idx, hoisted_src);
          tcc_ir_set_src2(ir, adjusted_call_idx, IROP_NONE);
          /* Keep original destination */
          tcc_ir_op_set_dest(ir, call_q, irop_make_vreg(orig_vreg, IROP_BTYPE_INT32));

          /* Track the hoisted vreg for iterative detection.
           * Store the hoisted vreg so that subsequent calls using it as argument
           * can see that it's loop-invariant. */
          if (num_hoisted_vregs < MAX_HOISTABLE_CALLS)
          {
            hoisted_vregs[num_hoisted_vregs++] = hoistable[i].hoisted_vreg;
          }
          hoisted_this_iteration++;
        }
        else
        {
          /* VOID call - just mark as NOP */
          call_q->op = TCCIR_OP_NOP;
          hoisted_this_iteration++;
        }

        /* Mark original parameters as NOP (indices are shifted by insertions_this_call) */
        for (int p = 0; p < num_params; p++)
        {
          int adjusted_param_idx = param_indices[p] + insertions_this_call;
          ir->compact_instructions[adjusted_param_idx].op = TCCIR_OP_NOP;
        }

        /* Update hoistable indices for remaining calls in this loop */
        for (int j = i + 1; j < num_hoistable; j++)
        {
          if (hoistable[j].instr_idx >= loop->preheader_idx + 1)
          {
            hoistable[j].instr_idx += insertions_this_call;
          }
        }

        hoistable[i].is_hoisted = 1;

        LOG_LICM("Hoisted pure call at instruction %d (new call_id=%d)", call_idx, new_call_id);

        /* Update all_call_indices for remaining calls - they shifted by insertions_this_call */
        for (int j = 0; j < num_all_calls; j++)
        {
          if (!hoisted_call_flags[j] && all_call_indices[j] > call_idx)
          {
            all_call_indices[j] += insertions_this_call;
          }
        }
      }

      /* Single pass only: transitive-invariance chaining (hoisting a call
       * whose argument is the RESULT of another call just hoisted in this same
       * loop) is deliberately NOT performed.  That path required rewriting the
       * copied FUNCPARAMVAL's operand from the loop-body result vreg to the
       * hoisted temp AND inserting the dependent call after its producer;
       * the copy-verbatim / insert-at-preheader+1 logic below does neither, so
       * a second iteration produced a preheader call reading an undefined vreg
       * (or one defined by a later-in-preheader call), corrupting argument
       * linkage (docs/bugs.md #7).  A single pass with num_hoisted_vregs left
       * at 0 during the hoistability checks only hoists calls whose arguments
       * are loop-invariant in the strict sense, which is correct by
       * construction.  (void)hoisted_this_iteration keeps the counter live for
       * the trace logging above without re-looping.) */
    } while (0);
    (void)hoisted_this_iteration;

    /* Update loop indices for subsequent loops, shifting by ONLY the number of
     * instructions inserted while processing THIS loop (see snapshot above). */
    int hoisted_this_loop = total_hoisted - total_hoisted_at_loop_start;
    if (hoisted_this_loop > 0)
    {
      for (int j = loop_idx + 1; j < loops->num_loops; j++)
      {
        IRLoop *later_loop = &loops->loops[j];
        if (later_loop->start_idx >= loop->preheader_idx)
          later_loop->start_idx += hoisted_this_loop;
        if (later_loop->end_idx >= loop->preheader_idx)
          later_loop->end_idx += hoisted_this_loop;
        if (later_loop->preheader_idx >= loop->preheader_idx)
          later_loop->preheader_idx += hoisted_this_loop;
        for (int k = 0; k < later_loop->num_body_instrs; k++)
        {
          if (later_loop->body_instrs[k] >= loop->preheader_idx)
            later_loop->body_instrs[k] += hoisted_this_loop;
        }
      }

      /* Update this loop's indices too */
      loop->header_idx += hoisted_this_loop;
      loop->start_idx += hoisted_this_loop;
      for (int k = 0; k < loop->num_body_instrs; k++)
      {
        loop->body_instrs[k] += hoisted_this_loop;
      }
    }
  }

  return total_hoisted;
}

/* Loop-invariant global-load hoisting.  A non-volatile global value read (a
 * SYMREF-deref operand consumed by an ALU op) is loop-invariant when the loop
 * writes no memory, so materialize it once in the preheader and rewrite the
 * in-loop reads to the temp.  A global address is always valid to load, so
 * speculating it onto the zero-trip path cannot fault (unlike a pointer deref),
 * which is why no dominance-over-uses guard is needed.  Gated on
 * loop_body_may_clobber_memory==0 so the loaded value cannot change. */
#define GLH_MAX_PER_LOOP 2

static int glh_is_value_consumer(int op)
{
  switch (op) {
  case TCCIR_OP_LOAD_INDEXED: case TCCIR_OP_LOAD_POSTINC:
  case TCCIR_OP_STORE: case TCCIR_OP_STORE_INDEXED: case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_LEA: case TCCIR_OP_VLA_ALLOC: case TCCIR_OP_BLOCK_COPY:
    return 0; /* here a SYMREF operand names an address, not a value */
  case TCCIR_OP_LOAD:
    /* A plain LOAD off a SYMREF-DEREF reads the global's value, exactly as the
     * ASSIGN form does -- there is no index to make the location vary.  It was
     * lumped in with the indexed forms above, which is why
     * mibench_stringsearch's `tbl[i] = len` loop reloaded `len` every
     * iteration: the frontend emits that read as a LOAD, not an ASSIGN. */
    return 1;
  default:
    return 1;
  }
}

typedef struct {
  Sym *sym;
  int64_t addend;
  int btype;
  IROperand src_op;   /* the exact global-deref operand, for the preheader LOAD */
  int32_t tmp;
} GLHCand;

/* A write destination that provably cannot alias a global: a direct named-local
 * (VAR vreg) or a direct stack slot.  A global (is_sym) or a pointer deref can. */
static int glh_dest_is_local_slot(IROperand d)
{
  if (d.is_sym)
    return 0;
  int32_t vr = irop_get_vreg(d);
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    return 1;
  if (irop_get_tag(d) == IROP_TAG_STACKOFF && d.is_local)
    return 1;
  return 0;
}

/* The single definition of a TEMP vreg, or -1. */
static int glh_single_def_of(TCCIRState *ir, int32_t vr)
{
  int def = -1;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval || irop_get_vreg(d) != vr)
      continue;
    if (def >= 0)
      return -1;
    def = i;
  }
  return def;
}

/* The global object a written ADDRESS is confined to, or NULL when unknown.
 *
 * `GlobalSym(X) ADD/SUB v` keeps the address inside X: reaching another object
 * through pointer arithmetic on X is undefined, so the write cannot touch one.
 * That is the whole point -- it lets `for (i..) tbl[i] = len;` hoist the load
 * of `len`, which mibench_stringsearch's init_search reloaded on all 256
 * iterations because the store through `&tbl[i]` counted as a write to "some
 * global".
 *
 * `saw_var` is what keeps global_base_share out of it.  That pass re-bases a
 * direct store to one global onto a NEIGHBOUR's address with a CONSTANT delta,
 * so after it `(sym=a, off=4)` and `(sym=b, off=0)` are one location under two
 * names and Sym*-keyed reasoning is unsound (see
 * tests/ir_tests/459_global_base_share_alias.c).  It only ever rewrites a
 * direct SYMREF-deref STORE and only with a constant offset, so requiring a
 * NON-IMMEDIATE component somewhere in the chain excludes everything it can
 * produce.  A weak symbol is refused for the same reason that pass refuses
 * one: it can be interposed onto another definition. */
static Sym *glh_addr_confined_sym(TCCIRState *ir, IROperand addr, int saw_var, int depth)
{
  if (irop_get_tag(addr) == IROP_TAG_SYMREF && !addr.is_lval && !addr.is_local) {
    if (!saw_var)
      return NULL;
    IRPoolSymref *sr = irop_get_symref_ex(ir, addr);
    if (!sr || !sr->sym || sr->sym->a.weak || (sr->sym->type.t & VT_VOLATILE))
      return NULL;
    return sr->sym;
  }
  if (depth <= 0)
    return NULL;
  int32_t vr = irop_get_vreg(addr);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return NULL;
  int def = glh_single_def_of(ir, vr);
  if (def < 0)
    return NULL;
  IRQuadCompact *dq = &ir->compact_instructions[def];
  if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB)
    return NULL;
  IROperand s1 = tcc_ir_op_get_src1(ir, dq);
  if (dq->op == TCCIR_OP_ASSIGN)
    return glh_addr_confined_sym(ir, s1, saw_var, depth - 1);
  IROperand s2 = tcc_ir_op_get_src2(ir, dq);
  /* One side carries the object, the other the displacement. */
  if (irop_get_tag(s1) == IROP_TAG_SYMREF && !s1.is_lval)
    return glh_addr_confined_sym(ir, s1, saw_var || !irop_is_immediate(s2), depth - 1);
  if (dq->op == TCCIR_OP_ADD && irop_get_tag(s2) == IROP_TAG_SYMREF && !s2.is_lval)
    return glh_addr_confined_sym(ir, s2, saw_var || !irop_is_immediate(s1), depth - 1);
  return glh_addr_confined_sym(ir, s1, saw_var || !irop_is_immediate(s2), depth - 1);
}

#define GLH_MAX_WRITTEN 8

typedef struct GLHWrites
{
  Sym *syms[GLH_MAX_WRITTEN];
  int n;
  int unknown; /* a write this cannot place: refuse every candidate */
} GLHWrites;

static void glh_note_write(GLHWrites *w, Sym *sym)
{
  if (!sym) {
    w->unknown = 1;
    return;
  }
  for (int i = 0; i < w->n; i++)
    if (w->syms[i] == sym)
      return;
  if (w->n >= GLH_MAX_WRITTEN)
    w->unknown = 1;
  else
    w->syms[w->n++] = sym;
}

/* Which globals does the loop write?  Unlike loop_body_may_clobber_memory this
 * exempts direct local-slot writes, which cannot alias a global — the case that
 * makes `s += G` reductions hoistable — and now also places a write that is
 * confined to one named global, so a candidate in a DIFFERENT global stays
 * hoistable.  Anything unplaceable sets `unknown`. */
static void glh_collect_global_writes(TCCIRState *ir, IRLoop *loop, GLHWrites *w)
{
  w->n = 0;
  w->unknown = 0;
  for (int i = 0; i < loop->num_body_instrs && !w->unknown; i++) {
    int idx = loop->body_instrs[i];
    if (idx < loop->start_idx || idx > loop->end_idx)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[idx];
    switch (q->op) {
    case TCCIR_OP_NOP:
      continue;
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_OUTPUT:
      w->unknown = 1;
      continue;
    case TCCIR_OP_STORE_INDEXED: {
      /* A constant index is exactly what global_base_share emits, so only a
       * variable one identifies the object. */
      IROperand ix = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE;
      IROperand base = tcc_ir_op_get_dest(ir, q);
      glh_note_write(w, irop_is_immediate(ix)
                            ? NULL
                            : glh_addr_confined_sym(ir, base, 1, 4));
      continue;
    }
    case TCCIR_OP_STORE: {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (glh_dest_is_local_slot(d))
        continue;
      if (irop_get_tag(d) == IROP_TAG_SYMREF && d.is_lval) {
        /* A direct named store: confined to that symbol whatever the addend. */
        IRPoolSymref *sr = irop_get_symref_ex(ir, d);
        glh_note_write(w, (sr && sr->sym && !sr->sym->a.weak) ? sr->sym : NULL);
        continue;
      }
      glh_note_write(w, glh_addr_confined_sym(ir, d, 0, 4));
      continue;
    }
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID: {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!callee || tcc_ir_get_func_purity(ir, callee) < TCC_FUNC_PURITY_CONST)
        w->unknown = 1;
      continue;
    }
    default:
      if (irop_config[q->op].has_dest) {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (d.is_lval && !glh_dest_is_local_slot(d))
          w->unknown = 1;
      }
      continue;
    }
  }
}

static int glh_writes_sym(const GLHWrites *w, const Sym *sym)
{
  if (w->unknown)
    return 1;
  for (int i = 0; i < w->n; i++)
    if (w->syms[i] == sym)
      return 1;
  return 0;
}

/* A SYMREF value read (global lvalue used as a value); volatility is checked
 * separately via the resolved symbol's type. */
static int glh_operand_is_global_value(IROperand op)
{
  return op.is_lval && op.is_sym && !op.is_llocal && !op.is_local;
}

/* Shift the still-unprocessed loops' indices by `delta` for entries >= pos,
 * after `after_li`'s insertions moved everything at/after pos forward. */
static void glh_shift_after(IRLoops *loops, int after_li, int pos, int delta)
{
  for (int li = after_li + 1; li < loops->num_loops; li++) {
    IRLoop *o = &loops->loops[li];
    if (o->header_idx >= pos) o->header_idx += delta;
    if (o->start_idx >= pos) o->start_idx += delta;
    if (o->end_idx >= pos) o->end_idx += delta;
    if (o->preheader_idx >= pos) o->preheader_idx += delta;
    for (int b = 0; b < o->num_body_instrs; b++)
      if (o->body_instrs[b] >= pos) o->body_instrs[b] += delta;
  }
}

static int tcc_ir_hoist_invariant_global_loads(TCCIRState *ir, IRLoops *loops)
{
  if (!ir || !loops)
    return 0;
  if (tcc_ir_opt_pass_disabled("licm_global_load"))
    return 0;

  int total = 0;
  for (int li = 0; li < loops->num_loops; li++) {
    IRLoop *loop = &loops->loops[li];

    /* Preheader-insertion safety: identical requirements to pure-call hoisting
     * (docs/bugs.md #7) — a real fall-through preheader that is the header's
     * immediate predecessor, not nested in another loop, with no entry edge
     * from outside bypassing it, and no VLA. */
    if (loop->preheader_idx < 0 || loop->preheader_idx != loop->header_idx - 1)
      continue;
    int bad = 0;
    for (int oi = 0; oi < loops->num_loops && !bad; oi++) {
      if (oi == li) continue;
      IRLoop *o = &loops->loops[oi];
      if (loop->preheader_idx >= o->start_idx && loop->preheader_idx <= o->end_idx)
          bad = 1;
    }
    for (int j = 0; j < ir->next_instruction_index && !bad; j++) {
      if (j >= loop->start_idx && j <= loop->end_idx)
        continue;
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF) {
        int jt = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jq));
        if (jt >= loop->header_idx && jt <= loop->end_idx)
          bad = 1;
      } else if (jq->op == TCCIR_OP_SWITCH_TABLE || jq->op == TCCIR_OP_IJUMP) {
        bad = 1;
      }
    }
    if (bad || loop_contains_vla(ir, loop))
      continue;
    /* Value-invariance gate: the loop must not write the global this candidate
     * reads.  Direct local-slot writes are exempt (they cannot alias static
     * memory), and a write confined to a DIFFERENT named global is exempt too;
     * anything unplaceable refuses every candidate in this loop. */
    GLHWrites writes;
    glh_collect_global_writes(ir, loop, &writes);
    if (writes.unknown)
      continue;

    GLHCand cand[GLH_MAX_PER_LOOP];
    int ncand = 0;
    for (int bi = 0; bi < loop->num_body_instrs; bi++) {
      int idx = loop->body_instrs[bi];
      if (idx < loop->start_idx || idx > loop->end_idx)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[idx];
      if (q->op == TCCIR_OP_NOP || !glh_is_value_consumer(q->op))
        continue;
      for (int side = 0; side < 2; side++) {
        if (side == 0 && !irop_config[q->op].has_src1) continue;
        if (side == 1 && !irop_config[q->op].has_src2) continue;
        IROperand op = side == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        if (!glh_operand_is_global_value(op))
          continue;
        IRPoolSymref *ref = irop_get_symref_ex(ir, op);
        if (!ref || !ref->sym || (ref->sym->type.t & VT_VOLATILE))
          continue;
        if (ref->sym->a.weak || glh_writes_sym(&writes, ref->sym))
          continue;
        int bt = irop_get_btype(op);
        int seen = 0;
        for (int c = 0; c < ncand; c++)
          if (cand[c].sym == ref->sym && cand[c].addend == ref->addend && cand[c].btype == bt) {
            seen = 1; break;
          }
        if (!seen && ncand < GLH_MAX_PER_LOOP) {
          cand[ncand].sym = ref->sym;
          cand[ncand].addend = ref->addend;
          cand[ncand].btype = bt;
          cand[ncand].src_op = op;
          cand[ncand].tmp = -1;
          ncand++;
        }
      }
    }
    if (ncand == 0)
      continue;

    for (int c = 0; c < ncand; c++)
      cand[c].tmp = tcc_ir_vreg_alloc_temp(ir);

    /* Rewrite the in-loop reads while indices are still stable. */
    int rewrote = 0;
    for (int bi = 0; bi < loop->num_body_instrs; bi++) {
      int idx = loop->body_instrs[bi];
      if (idx < loop->start_idx || idx > loop->end_idx)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[idx];
      if (q->op == TCCIR_OP_NOP || !glh_is_value_consumer(q->op))
        continue;
      for (int side = 0; side < 2; side++) {
        if (side == 0 && !irop_config[q->op].has_src1) continue;
        if (side == 1 && !irop_config[q->op].has_src2) continue;
        IROperand op = side == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        if (!glh_operand_is_global_value(op))
          continue;
        IRPoolSymref *ref = irop_get_symref_ex(ir, op);
        if (!ref || !ref->sym)
          continue;
        int bt = irop_get_btype(op);
        for (int c = 0; c < ncand; c++) {
          if (cand[c].sym == ref->sym && cand[c].addend == ref->addend && cand[c].btype == bt) {
            IROperand nv = irop_make_vreg(cand[c].tmp, bt);
            nv.is_unsigned = op.is_unsigned;
            if (side == 0) tcc_ir_set_src1(ir, idx, nv);
            else tcc_ir_set_src2(ir, idx, nv);
            rewrote++;
            break;
          }
        }
      }
    }
    if (rewrote == 0)
      continue;

    /* Materialize `tmp <- *global [LOAD]` in the preheader (fall-through into
     * the header).  Each insert shifts later indices forward by one. */
    int insert_at = loop->preheader_idx + 1;
    for (int c = 0; c < ncand; c++) {
      IROperand dest = irop_make_vreg(cand[c].tmp, cand[c].btype);
      dest.is_unsigned = cand[c].src_op.is_unsigned;
      IRQuadCompact ld = {0};
      ld.op = TCCIR_OP_LOAD;
      ld.operand_base = tcc_ir_pool_add(ir, dest);
      tcc_ir_pool_add(ir, cand[c].src_op);
      tcc_ir_pool_add(ir, IROP_NONE);
      tcc_ir_insert_instruction_before(ir, insert_at + c, &ld);
    }
    glh_shift_after(loops, li, insert_at, ncand);
    total += rewrote;
  }
  return total;
}

/* ============================================================================
 * Invariant reads of a local's own stack slot
 *
 * dom-LICM declines every instruction with an lval source, because an lval is
 * normally a pointer dereference whose target can change under the loop.  A
 * bare `StackLoc[off]` source is different: it names a local's own frame slot
 * directly, so it IS provably invariant whenever nothing in the loop can write
 * those bytes.  That is the difference between
 *
 *     ldr r1,[sp,#4] / mov r2,#400 / ldr r3,=adj / mla sl,r1,r2,r3
 *
 * being recomputed on every inner iteration and being hoisted once
 * (mibench_dijkstra's `adj_matrix[current.node][node]`, where `current` is a
 * 12-byte struct copy the frontend lowers to an __aeabi_memmove4 call).
 * ==========================================================================*/

/* A direct read of a named local variable: the IR's `V<n>` lvalue form, where
 * the vreg IS the variable rather than a pointer to be dereferenced.  Such a
 * read is a load from the variable's own slot, so it is only as invariant as
 * the slot — but when the address is never taken anywhere in the function, the
 * only writes are the explicit defs the def_count scan below already sees.
 * Without this, EVERY instruction reading a named local (`t = 63 - i`) counted
 * as a memory dereference and LICM hoisted nothing out of an ordinary
 * `for (j...) if (a[j] > a[j+1])` loop. */
/* Move a jump target off a NOP.
 *
 * A hoist inserts the clone at the loop header's index and NOPs the original
 * one slot later — which is the header itself, so the loop now BEGINS with a
 * NOP that still carries is_jump_target.  The CFG splits there, and
 * ssa:loop_rotate matches a header as exactly CMP/JUMPIF/JUMP at hi..hi+2 and
 * silently declines anything else, so every loop LICM helped lost rotation
 * (bubble sort's inner loop, dijkstra's, ...).
 *
 * Retargeting rather than compacting is deliberate: compacting changes the
 * instruction COUNT, and the frontend's auto-inline revoke is a threshold on
 * exactly that count, so it silently re-decides inlining for unrelated
 * functions (gcc-torture pr47428 started re-compiling a body at end-of-TU and
 * hard-erroring on an implicit declaration that had since acquired a real
 * prototype).  Retargeting leaves every index alone. */
static void licm_move_jump_target_off_nop(TCCIRState *ir, int nop_pos)
{
  int n = ir->next_instruction_index;
  if (nop_pos < 0 || nop_pos >= n)
    return;
  if (!ir->compact_instructions[nop_pos].is_jump_target)
    return;
  /* A computed jump can reach any label; its targets are not in the operands,
   * so the flag cannot be proved unused. */
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return;
  int t = nop_pos + 1;
  while (t < n && ir->compact_instructions[t].op == TCCIR_OP_NOP &&
         !ir->compact_instructions[t].is_jump_target)
    t++;
  if (t >= n)
    return;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    if ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q)) != nop_pos)
      continue;
    tcc_ir_op_set_dest(ir, q, irop_make_imm32(-1, t, IROP_BTYPE_INT32));
  }
  for (int k = 0; k < ir->num_switch_tables; k++)
  {
    TCCIRSwitchTable *tbl = &ir->switch_tables[k];
    if (tbl->default_target == nop_pos)
      tbl->default_target = t;
    if (!tbl->targets)
      continue;
    for (int j = 0; j < tbl->num_entries; j++)
      if (tbl->targets[j] == nop_pos)
        tbl->targets[j] = t;
  }
  ir->compact_instructions[t].is_jump_target = 1;
  ir->compact_instructions[nop_pos].is_jump_target = 0;
}

static int licm_is_direct_var_read(TCCIRState *ir, IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || !op.is_local || op.is_llocal)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  if (irop_access_is_volatile(op))
    return 0;
  return !vreg_addr_taken_anywhere(ir, vr);
}

/* A direct read of a named local's slot.  `Addr[StackLoc[off]]` (is_lval == 0)
 * is the slot's ADDRESS and is already accepted as invariant; the vreg-carrying
 * forms are VARs, which the ordinary def-count machinery handles. */
static int licm_is_direct_slot_read(IROperand op)
{
  return irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_lval && op.is_local &&
         !op.is_llocal && irop_get_vreg(op) == -1;
}

/* Callees that touch only the bytes their pointer arguments name and neither
 * store nor publish the pointer itself (LLVM's `nocapture`): handing one
 * `&local` does not let anything else reach the local.  This is exactly the
 * shape a small struct assignment lowers to — `T = &current;
 * __aeabi_memmove4(T, &queue[h], 12)`. */
static int licm_name_is_noncapturing_mem(const char *nm)
{
  if (!nm)
    return 0;
  if (ir_opt_is_memcpy_or_memmove_name(nm))
    return 1;
  return !strcmp(nm, "memset") || !strcmp(nm, "__aeabi_memset") ||
         !strcmp(nm, "__aeabi_memset4") || !strcmp(nm, "__aeabi_memset8") ||
         !strcmp(nm, "__aeabi_memclr") || !strcmp(nm, "__aeabi_memclr4") ||
         !strcmp(nm, "__aeabi_memclr8");
}

static int licm_instr_in_loop(IRCFG *cfg, const uint8_t *in_loop, int idx)
{
  for (int bi = 0; bi < cfg->num_blocks; bi++)
    if (in_loop[bi] && idx >= cfg->blocks[bi].start_idx && idx < cfg->blocks[bi].end_idx)
      return 1;
  return 0;
}

/* Every operand of a quad: dest, src1, src2 and MLA's accumulator. */
static int licm_quad_operands(TCCIRState *ir, int idx, IROperand *out)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  int n = 0;
  if (irop_config[q->op].has_dest)
    out[n++] = tcc_ir_op_get_dest(ir, q);
  if (irop_config[q->op].has_src1)
    out[n++] = tcc_ir_op_get_src1(ir, q);
  if (irop_config[q->op].has_src2)
    out[n++] = tcc_ir_op_get_src2(ir, q);
  if (q->op == TCCIR_OP_MLA)
    out[n++] = tcc_ir_op_get_accum(ir, q);
  return n;
}

/* Is `vr` mentioned by any instruction other than `def_idx`?  Dest mentions
 * count too — the callers want "nobody else touches this", not a use count. */
static int licm_vreg_touched_elsewhere(TCCIRState *ir, int32_t vr, int def_idx)
{
  if (vr < 0)
    return 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand ops[4];
    int n;
    if (i == def_idx || q->op == TCCIR_OP_NOP)
      continue;
    n = licm_quad_operands(ir, i, ops);
    for (int k = 0; k < n; k++)
      if (irop_get_vreg(ops[k]) == vr)
        return 1;
  }
  return 0;
}

#define LICM_MAX_ADDR_TEMPS 4
#define LICM_MAX_MEM_PARAMS 16

/* Can the loop reach `slot` through a pointer?  Sound over-approximation: the
 * slot's ADDRESS must be materialized only by `LEA T <- &slot` quads outside
 * the loop, and every use of such a T must be an argument of a non-capturing
 * mem* helper that is itself outside the loop.  Anything else — an address
 * materialized inside the loop, pointer arithmetic on it, a store of it, an
 * unknown callee, or a memcpy whose returned dst pointer somebody keeps —
 * counts as escaped, and then a store or call in the loop may write the slot. */
static int licm_slot_addr_escapes(TCCIRState *ir, IRCFG *cfg, const uint8_t *in_loop, IROperand slot)
{
  int32_t addr_tmp[LICM_MAX_ADDR_TEMPS];
  int n_addr = 0;
  int ok_param[LICM_MAX_MEM_PARAMS];
  int n_ok = 0;
  int64_t slot_base = irop_get_stack_offset(slot);
  int slot_size = ir_opt_store_btype_size_bytes(irop_get_btype(slot));
  int64_t slot_end = slot_base + (slot_size > 0 ? slot_size : 1);

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand ops[4], d;
    int n, refs = 0;
    int32_t dv;
    if (q->op == TCCIR_OP_NOP)
      continue;
    n = licm_quad_operands(ir, i, ops);
    for (int k = 0; k < n && !refs; k++)
    {
      int64_t ab, ae;
      if (ops[k].is_lval || irop_get_tag(ops[k]) != IROP_TAG_STACKOFF)
        continue;
      /* A frame ADDRESS.  It can reach our slot unless it provably names a
       * different frame object.  Compare RESOLVED slot ranges, never raw
       * operand fields: on a STRUCT-typed operand the offset lives in
       * u.s.aux_data and the imm32 is a pool index, so `&gof` and `gof.argc`
       * name the same slot yet share no field (this is why
       * operand_references_slot cannot be used here).  A VT_LLOCAL slot holds
       * a pointer to somewhere else entirely — always assume it aliases. */
      if (!ops[k].is_llocal &&
          ir_opt_stack_slot_range_for_offset(ir, irop_get_stack_offset(ops[k]), &ab, &ae) &&
          (slot_end <= ab || slot_base >= ae))
        continue;
      refs = 1;
    }
    if (!refs)
      continue;
    if (q->op != TCCIR_OP_LEA || licm_instr_in_loop(cfg, in_loop, i))
      return 1;
    d = tcc_ir_op_get_dest(ir, q);
    dv = irop_get_vreg(d);
    if (d.is_lval || dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
      return 1;
    if (n_addr >= LICM_MAX_ADDR_TEMPS)
      return 1;
    addr_tmp[n_addr++] = dv;
  }
  if (n_addr == 0)
    return 0; /* address never taken — no pointer can name the slot */

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    Sym *callee;
    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;
    callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee || !licm_name_is_noncapturing_mem(get_tok_str(callee->v, NULL)))
      continue;
    if (licm_instr_in_loop(cfg, in_loop, i))
      continue;
    if (q->op == TCCIR_OP_FUNCCALLVAL &&
        licm_vreg_touched_elsewhere(ir, irop_get_vreg(tcc_ir_op_get_dest(ir, q)), i))
      continue; /* the returned dst pointer is kept — its args are not safe */
    for (int k = 0; k < 4; k++)
    {
      int pi = ir_opt_get_call_param_index(ir, i, k);
      if (pi < 0)
        continue;
      if (n_ok >= LICM_MAX_MEM_PARAMS)
        return 1;
      ok_param[n_ok++] = pi;
    }
  }

  /* Every READ of an address temp must be one of those arguments.  Only source
   * positions count as reads; a plain dest is the temp's own definition (an
   * lval dest is a store THROUGH the pointer, i.e. a read of it, and is caught
   * because a store quad is never an argument quad). */
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand ops[4];
    int n, reads = 0, allowed = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;
    n = licm_quad_operands(ir, i, ops);
    for (int k = 0; k < n && !reads; k++)
    {
      /* ops[0] is the dest when the op has one; skip it unless it is an lval. */
      int is_dest = irop_config[q->op].has_dest && k == 0;
      if (is_dest && !ops[k].is_lval)
        continue;
      for (int a = 0; a < n_addr; a++)
        if (irop_get_vreg(ops[k]) == addr_tmp[a])
        {
          reads = 1;
          break;
        }
    }
    if (!reads)
      continue;
    for (int k = 0; k < n_ok && !allowed; k++)
      allowed = (ok_param[k] == i);
    if (!allowed)
      return 1;
  }
  return 0;
}

/* A write inside the loop that could land on an arbitrary frame slot: a direct
 * anonymous-slot destination (matched offset-blind — accesses of different
 * widths overlap without sharing an offset), a block copy, inline asm, or a VLA
 * that moves sp.  VAR destinations are exempt: a named local has its own slot. */
static int licm_loop_writes_frame(TCCIRState *ir, IRCFG *cfg, const uint8_t *in_loop)
{
  for (int bi = 0; bi < cfg->num_blocks; bi++)
  {
    if (!in_loop[bi])
      continue;
    for (int ii = cfg->blocks[bi].start_idx; ii < cfg->blocks[bi].end_idx; ii++)
    {
      IRQuadCompact *q = &ir->compact_instructions[ii];
      switch (q->op)
      {
      case TCCIR_OP_NOP:
        continue;
      case TCCIR_OP_BLOCK_COPY:
      case TCCIR_OP_INLINE_ASM:
      case TCCIR_OP_ASM_OUTPUT:
      case TCCIR_OP_VLA_ALLOC:
        return 1;
      default:
        break;
      }
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_get_tag(d) == IROP_TAG_STACKOFF && irop_get_vreg(d) < 0)
          return 1;
      }
    }
  }
  return 0;
}

/* Per-loop memo for the two scans above (both are O(function) and the fixed
 * point below asks the same question many times). */
typedef struct
{
  int writes_frame; /* -1 until computed */
  int n;
  int32_t key_off[8];
  signed char key_bt[8];
  signed char verdict[8];
} LicmSlotCtx;

static int licm_slot_read_is_invariant(TCCIRState *ir, IRCFG *cfg, const uint8_t *in_loop,
                                       IROperand slot, LicmSlotCtx *ctx)
{
  int32_t off = irop_get_stack_offset(slot);
  int bt = irop_get_btype(slot);
  int v;
  if (ctx->writes_frame < 0)
    ctx->writes_frame = licm_loop_writes_frame(ir, cfg, in_loop);
  if (ctx->writes_frame)
    return 0;
  /* Keyed on (offset, btype): the access WIDTH is part of the alias question. */
  for (int i = 0; i < ctx->n; i++)
    if (ctx->key_off[i] == off && ctx->key_bt[i] == (signed char)bt)
      return ctx->verdict[i];
  v = !licm_slot_addr_escapes(ir, cfg, in_loop, slot);
  if (ctx->n < (int)(sizeof(ctx->key_off) / sizeof(ctx->key_off[0])))
  {
    ctx->key_off[ctx->n] = off;
    ctx->key_bt[ctx->n] = (signed char)bt;
    ctx->verdict[ctx->n] = (signed char)v;
    ctx->n++;
  }
  return v;
}

/* Second chance for the invariant-global-load hoist, run AFTER loop rotation.
 *
 * ssa:licm runs BEFORE ssa:loop_rotate, so it sees the frontend's un-rotated
 * header/latch/body shape whose preheader is not the header's immediate
 * predecessor -- and the hoist's preheader-insertion safety demands exactly
 * that, so it declines every ordinary `for` loop.  mibench_stringsearch's
 * `for (i = 0; i <= UCHAR_MAX; i++) tbl[i] = len;` is one: it reloaded `len`
 * from memory on all 256 iterations.  Rotation produces the shape the hoist
 * wants, so run it once more on the rotated loops. */
int ssa_opt_licm_global_load(TCCIRState *ir)
{
  if (!ir || tcc_ir_opt_pass_disabled("licm_global_load_post"))
    return 0;
  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops)
    return 0;
  int changed = 0;
  if (loops->num_loops > 0)
    changed = tcc_ir_hoist_invariant_global_loads(ir, loops);
  tcc_ir_free_loops(loops);
  return changed;
}

/* ── Speculative-hoist profitability ──
 * A hoist that does not dominate the loop's exits trades ONE loop-long live
 * value for the ops it removes, so a single-use invariant is normally declined.
 * A CHAIN of invariant ops is the exception: `t = slot * 400; u = &g + t` still
 * leaves exactly one value live in the loop while removing both ops.  The two
 * predicates below recognise a chain from either end — the producer (all its
 * in-loop uses leave with it, so it costs nothing) and the consumer (it retires
 * a single-use invariant producer, so two ops go for one register). */
static int licm_all_uses_invariant(TCCIRState *ir, IRCFG *cfg, const uint8_t *in_loop,
                                   const uint8_t *is_invariant, int idx, int32_t vr)
{
  int seen = 0;
  if (vr < 0)
    return 0;
  for (int bi = 0; bi < cfg->num_blocks; bi++)
  {
    if (!in_loop[bi])
      continue;
    for (int ii = cfg->blocks[bi].start_idx; ii < cfg->blocks[bi].end_idx; ii++)
    {
      IRQuadCompact *q = &ir->compact_instructions[ii];
      IROperand ops[4];
      int n, uses = 0;
      if (ii == idx || q->op == TCCIR_OP_NOP)
        continue;
      n = licm_quad_operands(ir, ii, ops);
      for (int k = 0; k < n; k++)
        if (irop_get_vreg(ops[k]) == vr)
          uses = 1;
      if (!uses)
        continue;
      if (!is_invariant[ii])
        return 0;
      seen = 1;
    }
  }
  return seen;
}

static int licm_feeds_from_invariant_chain(TCCIRState *ir, IRCFG *cfg, const uint8_t *in_loop,
                                           const uint8_t *is_invariant, const int *def_count,
                                           const int *use_count, int dc_stride, int max_vr, int idx)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  for (int oi = 0; oi < 2; oi++)
  {
    IROperand s;
    int32_t v;
    int p, t;
    if (oi == 0 && !irop_config[q->op].has_src1)
      continue;
    if (oi == 1 && !irop_config[q->op].has_src2)
      continue;
    s = (oi == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
    v = irop_get_vreg(s);
    if (v < 0)
      continue;
    p = TCCIR_DECODE_VREG_POSITION(v);
    t = TCCIR_DECODE_VREG_TYPE(v);
    if (p > max_vr)
      continue;
    if (def_count[t * dc_stride + p] != 1 || use_count[t * dc_stride + p] != 1)
      continue;
    for (int bi = 0; bi < cfg->num_blocks; bi++)
    {
      if (!in_loop[bi])
        continue;
      for (int ii = cfg->blocks[bi].start_idx; ii < cfg->blocks[bi].end_idx; ii++)
      {
        IRQuadCompact *dq = &ir->compact_instructions[ii];
        if (dq->op == TCCIR_OP_NOP || !irop_config[dq->op].has_dest)
          continue;
        if (irop_get_vreg(tcc_ir_op_get_dest(ir, dq)) != v)
          continue;
        if (is_invariant[ii])
          return 1;
      }
    }
  }
  return 0;
}

/* Pure, fault-free, side-effect-free ops (dom-LICM's whole hoist whitelist).
 * Executing one on the zero-trip path is harmless, so they may be hoisted to a
 * dominating preheader without dominating the loop's exits — the requirement
 * that only matters for trapping (DIV, pointer LOAD) or side-effecting ops.
 * LOAD is deliberately NOT here: admitting a proven-invariant slot read was
 * measured over the ir_tests corpus at 1 file changed, +8 bytes, and no cycle
 * change on mibench_dijkstra — the reloads it targets are separate single-use
 * temps, so each is still rejected by the use_count gate.
 *
 * The follow-up this used to propose — slot-load CSE, one preheader load
 * feeding every in-loop read — was built and MEASURED on the rig, and it is a
 * LOSS.  Do not re-propose it.  It does what it says: dijkstra's inner loop
 * loses all four `current.dist` / `current.node` reloads (and its two stores
 * fuse into STRD).  But each merged read leaves a value live across the whole
 * loop, that pressure evicts `queue_tail`, and phi resolution answers with
 * `mov fp,r3` / `mov r3,fp` on the HOT path — the `edge == NONE` arm, ~70% of
 * iterations, which grows 8 -> 11 instructions to save 4 loads on the ~30% arm:
 *
 *     mibench_dijkstra   off 26,522,784   <=2 slots 27,350,520 (+3.12%)
 *                                         <=1 slot  28,785,239 (+8.53%)
 *
 * (every other benchmark within +-0.5%; the reverted tree reproduces 26,522,784
 * to the cycle).  Capping at one slot keeps the hot path at 8 instructions and
 * still measures worse, so the damage is not just the copies one can see.  The
 * un-rotated loop shape is what makes any extra loop-long value this expensive;
 * the way out is SROA on the 12-byte struct copy — which REMOVES memory traffic
 * instead of trading it for pressure — not a smarter hoist. */
static int licm_op_is_speculatable(int op)
{
  switch (op) {
  case TCCIR_OP_ADD: case TCCIR_OP_SUB: case TCCIR_OP_MUL:
  case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR:
  case TCCIR_OP_SHL: case TCCIR_OP_SHR: case TCCIR_OP_SAR: case TCCIR_OP_ROR:
  case TCCIR_OP_ASSIGN: case TCCIR_OP_LEA:
    return 1;
  default:
    return 0;
  }
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================ */

int tcc_ir_opt_licm(TCCIRState *ir)
{
  IRLoops *loops = tcc_ir_opt_licm_ex(ir);
  int hoisted = loops ? loops->num_loops : 0; /* non-zero if loops exist */
  tcc_ir_free_loops(loops);
  return hoisted;
}

/* ssa:licm regalloc-time driver: runs the proven LICM engine on flat IR in the
 * ssa: region, before ssa:iv_strength_reduction (docs/plan_legacy_loop_licm_ssa.md). */
int ssa_opt_licm(TCCIRState *ir)
{
  IRLoops *loops = tcc_ir_opt_licm_ex(ir);
  int changed = loops != NULL;
  tcc_ir_free_loops(loops);
  return changed;
}

/* Timed at its production call site (ssa:licm in tcc_ir_ssa_regalloc). */
IRLoops *tcc_ir_opt_licm_ex(TCCIRState *ir)
{
  if (!ir)
    return NULL;

  LOG_LICM("Starting loop-invariant code motion");

  /* Step 1: Detect loops */
  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    LOG_LICM("No loops found");
    tcc_ir_free_loops(loops);
    return NULL;
  }

  /* Step 2: Hoist pure function calls FIRST (FUNCTION_CALLS_OPTIMIZATION_PLAN Phase 1)
   *
   * Pure function hoisting is done before general LICM so that pure calls
   * inside loops can be hoisted even if the loop contains other non-pure calls
   * that would normally block LICM.
   *
   * Note: Loops containing VLA_ALLOC instructions are automatically skipped
   * because VLAs have special stack semantics - the size computation must
   * happen at the VLA allocation point, not in the preheader.
   */
  /* Pure/const call hoisting — RE-ENABLED (docs/bugs.md #7, fixed 2026-07-02).
   * Four defects were fixed to make this safe:
   *   1. Multi-loop index fix-up now shifts later loops by per-loop insertion
   *      counts, not the cumulative total (which over-shifted a 3rd+ loop).
   *   2. Transitive-invariance CHAINING (a call whose arg is another just-
   *      hoisted call's result) is not attempted — a single non-chaining pass;
   *      that path copied a FUNCPARAMVAL referencing a body-only vreg and
   *      ordered the dependent call before its producer.
   *   3. The copied CALL / param markers are laid out per each op's irop_config
   *      (FUNCCALLVOID has no dest; FUNCPARAMVOID has no src1) — the old code
   *      always emitted a dest+src1, misdecoding a void call's call_id.
   *   4. collect_call_params gathers FUNCPARAMVOID markers too, so a call's
   *      end-of-args marker travels with it.
   *   5. A merely-PURE (memory-reading) call is hoisted only when the loop
   *      cannot modify the memory it reads (PR20100); CONST calls always may.
   * Change is signalled to the pipeline via num_loops > 0 (see tcc_ir_opt_licm),
   * same as the dominance-based LICM below. */
  int hoisted_calls = tcc_ir_hoist_pure_calls(ir, loops);
  int hoisted = 0;
  (void)hoisted_calls;

  /* Loop-invariant global-load hoisting runs on the fresh loop structure (same
   * preheader shape the pure-call hoist relies on); re-detect if it changed the
   * IR so the dom-LICM phase below sees valid indices. */
  if (tcc_ir_hoist_invariant_global_loads(ir, loops) > 0) {
    tcc_ir_free_loops(loops);
    loops = tcc_ir_detect_loops(ir);
    if (!loops || loops->num_loops == 0) {
      tcc_ir_free_loops(loops);
      return NULL;
    }
  }

  /* ── Dominance-based LICM ──
   * Uses proper CFG + dominator tree to detect natural loops and
   * verify invariant safety.  Replaces the buggy pattern-based approach. */
  {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (cfg && cfg->num_blocks > 1) {
      tcc_ir_cfg_compute_dominators(cfg);

      /* Detect natural loops via dominance-verified back-edges */
      for (int b = 0; b < cfg->num_blocks; b++) {
        IRBasicBlock *bb = &cfg->blocks[b];
        for (int si = 0; si < bb->num_succs; si++) {
          int h = bb->succs[si];
          if (h < 0 || h >= cfg->num_blocks)
            continue;
          if (!tcc_ir_cfg_dominates(cfg, h, b))
            continue;

          /* Natural loop: header=h, latch=b.  Collect body via flood-fill. */
          uint8_t *in_loop = tcc_mallocz(cfg->num_blocks);
          in_loop[h] = 1;
          int *worklist = tcc_mallocz(cfg->num_blocks * sizeof(int));
          int wl_count = 0;
          if (b != h) {
            in_loop[b] = 1;
            worklist[wl_count++] = b;
          }
          while (wl_count > 0) {
            int node = worklist[--wl_count];
            IRBasicBlock *nb = &cfg->blocks[node];
            for (int pi = 0; pi < nb->num_preds; pi++) {
              int p = nb->preds[pi];
              if (p >= 0 && p < cfg->num_blocks && !in_loop[p]) {
                in_loop[p] = 1;
                worklist[wl_count++] = p;
              }
            }
          }

          /* Find preheader: unique predecessor of header not in loop */
          int preheader = -1;
          {
            IRBasicBlock *hb = &cfg->blocks[h];
            for (int pi = 0; pi < hb->num_preds; pi++) {
              int p = hb->preds[pi];
              if (p >= 0 && !in_loop[p]) {
                if (preheader == -1)
                  preheader = p;
                else {
                  preheader = -1; /* multiple outside preds — can't use simple preheader */
                  break;
                }
              }
            }
          }
          /* A valid preheader must DOMINATE the header: every path from
           * function entry to the header must pass through it, so code
           * hoisted there runs before each header execution.  A unique
           * out-of-loop predecessor is not sufficient — when the header is
           * itself the function entry block (or otherwise reachable from
           * entry without passing the predecessor), that predecessor is a
           * back-edge source of an enclosing loop, and hoisting an invariant
           * into it skips it on the entry path (miscompile: the hoisted
           * value is undefined on the first iteration). */
          if (preheader >= 0 && !tcc_ir_cfg_dominates(cfg, preheader, h))
            preheader = -1;
          if (preheader < 0) {
            tcc_free(in_loop);
            tcc_free(worklist);
            continue;
          }

          /* Collect loop defs: vreg → def count.
           * Index by type*stride+position so that V2 (VAR,pos=2) and
           * P2 (PARAM,pos=2) don't collide. */
          int max_vr = 0;
          for (int bi = 0; bi < cfg->num_blocks; bi++) {
            if (!in_loop[bi])
              continue;
            for (int ii = cfg->blocks[bi].start_idx; ii < cfg->blocks[bi].end_idx; ii++) {
              IRQuadCompact *q = &ir->compact_instructions[ii];
              if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
                continue;
              int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
              if (vr >= 0) {
                int pos = TCCIR_DECODE_VREG_POSITION(vr);
                if (pos > max_vr)
                  max_vr = pos;
              }
            }
          }
          int dc_stride = max_vr + 1;
          int *def_count = tcc_mallocz(4 * dc_stride * sizeof(int));
          /* Per-vreg use count within the loop body — a profitability signal for
           * hoists that don't dominate the loop's exits (see the safety check). */
          int *use_count = tcc_mallocz(4 * dc_stride * sizeof(int));
          for (int bi = 0; bi < cfg->num_blocks; bi++) {
            if (!in_loop[bi])
              continue;
            for (int ii = cfg->blocks[bi].start_idx; ii < cfg->blocks[bi].end_idx; ii++) {
              IRQuadCompact *q = &ir->compact_instructions[ii];
              if (q->op == TCCIR_OP_NOP)
                continue;
              if (irop_config[q->op].has_dest) {
                int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
                if (vr >= 0) {
                  int pos = TCCIR_DECODE_VREG_POSITION(vr);
                  int typ = TCCIR_DECODE_VREG_TYPE(vr);
                  if (pos <= max_vr)
                    def_count[typ * dc_stride + pos]++;
                }
              }
              for (int side = 0; side < 2; side++) {
                if (side == 0 && !irop_config[q->op].has_src1) continue;
                if (side == 1 && !irop_config[q->op].has_src2) continue;
                IROperand s = side == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
                int32_t vr = irop_get_vreg(s);
                if (vr >= 0) {
                  int pos = TCCIR_DECODE_VREG_POSITION(vr);
                  int typ = TCCIR_DECODE_VREG_TYPE(vr);
                  if (pos <= max_vr)
                    use_count[typ * dc_stride + pos]++;
                }
              }
            }
          }

          /* Fixed-point invariant detection */
          int total_loop_instrs = 0; (void)total_loop_instrs;
          for (int bi = 0; bi < cfg->num_blocks; bi++)
            if (in_loop[bi])
              total_loop_instrs += cfg->blocks[bi].end_idx - cfg->blocks[bi].start_idx;

          uint8_t *is_invariant = tcc_mallocz(ir->next_instruction_index);
          LicmSlotCtx slot_ctx;
          int inv_changed = 1;
          slot_ctx.writes_frame = -1;
          slot_ctx.n = 0;
          while (inv_changed) {
            inv_changed = 0;
            for (int bi = 0; bi < cfg->num_blocks; bi++) {
              if (!in_loop[bi])
                continue;
              for (int ii = cfg->blocks[bi].start_idx; ii < cfg->blocks[bi].end_idx; ii++) {
                if (is_invariant[ii])
                  continue;
                IRQuadCompact *q = &ir->compact_instructions[ii];
                if (q->op == TCCIR_OP_NOP)
                  continue;
                /* Only hoist side-effect-free arithmetic/assign */
                switch (q->op) {
                case TCCIR_OP_ADD: case TCCIR_OP_SUB: case TCCIR_OP_MUL:
                case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR:
                case TCCIR_OP_SHL: case TCCIR_OP_SHR: case TCCIR_OP_SAR: case TCCIR_OP_ROR:
                case TCCIR_OP_ASSIGN: case TCCIR_OP_LEA:
                  break;
                default:
                  continue;
                }
                /* Dest must have single def in loop AND must not be
                 * defined outside the loop.  If the vreg carries a value
                 * INTO the loop (live at entry), hoisting clobbers it. */
                if (irop_config[q->op].has_dest) {
                  int32_t dvr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
                  if (dvr >= 0) {
                    int dp = TCCIR_DECODE_VREG_POSITION(dvr);
                    int dt = TCCIR_DECODE_VREG_TYPE(dvr);
                    if (dp <= max_vr && def_count[dt * dc_stride + dp] > 1)
                      continue;
                    /* Check if vreg is also defined outside the loop */
                    int outside_def = 0;
                    for (int obi = 0; obi < cfg->num_blocks && !outside_def; obi++) {
                      if (in_loop[obi])
                        continue;
                      for (int oi2 = cfg->blocks[obi].start_idx; oi2 < cfg->blocks[obi].end_idx; oi2++) {
                        IRQuadCompact *oq = &ir->compact_instructions[oi2];
                        if (oq->op == TCCIR_OP_NOP || !irop_config[oq->op].has_dest)
                          continue;
                        if (irop_get_vreg(tcc_ir_op_get_dest(ir, oq)) == dvr) {
                          outside_def = 1;
                          break;
                        }
                      }
                    }
                    if (outside_def)
                      continue;
                    /* The dest's single visible def is not the whole story: if
                     * the variable's address escapes, a store through a pointer
                     * or any non-CONST call inside the loop can redefine it
                     * without a def this scan sees.  Hoisting then makes the
                     * assignment happen once instead of once per iteration —
                     * `while (n--) { v = 5; g(&v); use(v); }` accumulated g's
                     * side effect across iterations.  This mirrors the guard
                     * is_operand_loop_invariant_ex already applies on the READ
                     * side (docs/bugs.md #7). */
                    if (vreg_addr_taken_anywhere(ir, dvr) &&
                        cfg_loop_may_clobber_memory(ir, cfg, in_loop))
                      continue;
                  }
                }
                /* Skip instructions with memory dereference sources — these are
                 * loads that may read volatile/changing memory.  A bare
                 * `StackLoc[off]` source is not a dereference but a named
                 * local's own slot, so it is allowed once proved unwritable by
                 * the loop (licm_slot_read_is_invariant). */
                {
                  int has_deref = 0;
                  for (int oi = 0; oi < 2 && !has_deref; oi++) {
                    IROperand s;
                    if (oi == 0 && !irop_config[q->op].has_src1) continue;
                    if (oi == 1 && !irop_config[q->op].has_src2) continue;
                    s = (oi == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
                    if (!(s.is_lval || irop_op_is_lval(s))) continue;
                    if (licm_is_direct_slot_read(s) &&
                        licm_slot_read_is_invariant(ir, cfg, in_loop, s, &slot_ctx))
                      continue;
                    if (licm_is_direct_var_read(ir, s))
                      continue; /* named local, address never taken — def_count covers it */
                    has_deref = 1;
                  }
                  if (has_deref) continue;
                }
                /* Check all source operands */
                int all_inv = 1;
                for (int oi = 0; oi < 2 && all_inv; oi++) {
                  if (oi == 0 && !irop_config[q->op].has_src1)
                    continue;
                  if (oi == 1 && !irop_config[q->op].has_src2)
                    continue;
                  IROperand op = (oi == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
                  int tag = irop_get_tag(op);
                  if (tag == IROP_TAG_IMM32 || tag == IROP_TAG_I64 ||
                      tag == IROP_TAG_F32 || tag == IROP_TAG_F64 ||
                      tag == IROP_TAG_SYMREF)
                    continue; /* constant/symbol — invariant */
                  if (tag == IROP_TAG_STACKOFF && !op.is_lval)
                    continue; /* stack address — invariant */
                  if (licm_is_direct_slot_read(op))
                    continue; /* slot value — invariance proved by the gate above */
                  int32_t vr = irop_get_vreg(op);
                  if (vr < 0) {
                    all_inv = 0;
                    continue;
                  }
                  int vp = TCCIR_DECODE_VREG_POSITION(vr);
                  int vt = TCCIR_DECODE_VREG_TYPE(vr);
                  if (vp <= max_vr && def_count[vt * dc_stride + vp] > 0) {
                    /* Defined in loop — only invariant if single-def and that def is invariant */
                    if (def_count[vt * dc_stride + vp] == 1) {
                      /* Find the def instruction */
                      int found_inv = 0;
                      for (int bi2 = 0; bi2 < cfg->num_blocks && !found_inv; bi2++) {
                        if (!in_loop[bi2])
                          continue;
                        for (int jj = cfg->blocks[bi2].start_idx; jj < cfg->blocks[bi2].end_idx; jj++) {
                          IRQuadCompact *dq = &ir->compact_instructions[jj];
                          if (!irop_config[dq->op].has_dest)
                            continue;
                          if (irop_get_vreg(tcc_ir_op_get_dest(ir, dq)) == vr) {
                            found_inv = is_invariant[jj];
                            break;
                          }
                        }
                      }
                      if (!found_inv)
                        all_inv = 0;
                    }
                    else {
                      all_inv = 0;
                    }
                  }
                  /* else: not defined in loop → invariant (defined outside) */
                }
                if (all_inv) {
                  is_invariant[ii] = 1;
                  inv_changed = 1;
                  LOG_LICM("  dom-LICM: marked invariant insn %d op=%d", ii, q->op);
                }
              }
            }
          }

          /* Find exit blocks */
          uint8_t *is_exit = tcc_mallocz(cfg->num_blocks);
          for (int bi = 0; bi < cfg->num_blocks; bi++) {
            if (!in_loop[bi])
              continue;
            IRBasicBlock *lb = &cfg->blocks[bi];
            for (int si2 = 0; si2 < lb->num_succs; si2++) {
              int s = lb->succs[si2];
              if (s >= 0 && s < cfg->num_blocks && !in_loop[s]) {
                is_exit[bi] = 1;
                break;
              }
            }
          }

          LOG_LICM("dom-LICM: natural loop header=blk%d latch=blk%d preheader=blk%d", h, b, preheader);

          /* Hoist invariant instructions to preheader */
          int insert_pos = cfg->blocks[preheader].end_idx;
          /* If preheader ends with a jump, insert before it */
          if (insert_pos > cfg->blocks[preheader].start_idx) {
            int lop = ir->compact_instructions[insert_pos - 1].op;
            if (lop == TCCIR_OP_JUMP || lop == TCCIR_OP_JUMPIF)
              insert_pos--;
          }

          /* Skip functions containing SWITCH_TABLE: tcc_ir_insert_instruction_before
           * doesn't update switch table target indices, so hoisting corrupts
           * the dispatch. */
          {
            int has_switch = 0;
            for (int si3 = 0; si3 < ir->next_instruction_index; si3++) {
              if (ir->compact_instructions[si3].op == TCCIR_OP_SWITCH_TABLE) {
                has_switch = 1;
                break;
              }
            }
            if (has_switch) {
              LOG_LICM("dom-LICM: skipping — function has SWITCH_TABLE");
              tcc_free(is_invariant);
              tcc_free(is_exit);
              tcc_free(def_count);
              tcc_free(use_count);
              tcc_free(in_loop);
              tcc_free(worklist);
              continue;
            }
          }

          /* When the preheader ends in a jump, insert_pos lands strictly BEFORE
           * the header (in the preheader body).  A branch that targets that
           * position bypasses the hoist: tcc_ir_insert_instruction_before renumbers a
           * jump whose target == insert_pos to insert_pos+1, so the edge skips
           * the inserted instruction and enters the loop with the hoisted value
           * undefined.  This arises when the preheader is a bare jump block a
           * sibling branch jumps straight into (e.g. a switch-dispatch-at-bottom
           * whose backward arm makes a spurious loop; jump_threading normally
           * re-canonicalizes the CFG and hides it).  When insert_pos == header
           * start there is no such hazard: the header's own back-edge targets it
           * and is renumbered consistently, so the hoist still dominates the
           * (shifted) header via preheader fall-through. */
          if (insert_pos < cfg->blocks[h].start_idx) {
            int insert_targeted = 0;
            for (int j = 0; j < ir->next_instruction_index && !insert_targeted; j++) {
              IRQuadCompact *jq = &ir->compact_instructions[j];
              if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
                continue;
              if ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jq)) == insert_pos)
                insert_targeted = 1;
            }
            if (insert_targeted) {
              LOG_LICM("dom-LICM: skipping — insert_pos %d is a branch target", insert_pos);
              tcc_free(is_invariant);
              tcc_free(is_exit);
              tcc_free(def_count);
              tcc_free(use_count);
              tcc_free(in_loop);
              tcc_free(worklist);
              continue;
            }
          }

          /* Chain profitability for the speculative-hoist gate below.  Computed
           * HERE, before the first insertion: is_invariant and the CFG both
           * index pre-insertion positions, while the hoist loop below reads the
           * IR at shifted ones. */
          uint8_t *chain_ok = tcc_mallocz(ir->next_instruction_index);
          for (int bi = 0; bi < cfg->num_blocks; bi++) {
            if (!in_loop[bi])
              continue;
            for (int ii = cfg->blocks[bi].start_idx; ii < cfg->blocks[bi].end_idx; ii++) {
              IRQuadCompact *cq = &ir->compact_instructions[ii];
              int32_t cdv;
              if (!is_invariant[ii] || cq->op == TCCIR_OP_NOP)
                continue;
              cdv = irop_config[cq->op].has_dest ? irop_get_vreg(tcc_ir_op_get_dest(ir, cq)) : -1;
              if (licm_all_uses_invariant(ir, cfg, in_loop, is_invariant, ii, cdv) ||
                  licm_feeds_from_invariant_chain(ir, cfg, in_loop, is_invariant, def_count,
                                                  use_count, dc_stride, max_vr, ii))
                chain_ok[ii] = 1;
            }
          }

          /* Estimate how many values we can hoist without starving the loop body */
          int loop_start_idx = cfg->blocks[h].start_idx;
          int loop_end_idx = cfg->blocks[b].end_idx;
          int max_hoist = tcc_ir_estimate_hoist_budget(ir, loop_start_idx, loop_end_idx, ir->parameters_count);

          int total_hoisted_here = 0;
          int body_shift = 0;
          /* Dests actually emitted to the preheader.  is_invariant marks a
           * whole chain, but the gates below (speculation profitability, the
           * max_hoist budget) can skip a member — a later member that USES the
           * skipped dest must then stay in the loop too, or the preheader
           * computes the use before its def ever ran (fuzz seed
           * struct_byval:76, test 388). */
          uint8_t *hoisted_dest = tcc_mallocz((size_t)4 * dc_stride);
          for (int bi = 0; bi < cfg->num_blocks && total_hoisted_here < max_hoist; bi++) {
            if (!in_loop[bi])
              continue;
            for (int ii = cfg->blocks[bi].start_idx; ii < cfg->blocks[bi].end_idx; ii++) {
              int adj_ii = ii + body_shift;
              if (adj_ii >= ir->next_instruction_index)
                break;
              if (!is_invariant[ii])
                continue;
              IRQuadCompact *q = &ir->compact_instructions[adj_ii];
              if (q->op == TCCIR_OP_NOP)
                continue;

              if (total_hoisted_here >= max_hoist)
                break;

              /* Safety: a trapping / side-effecting instruction may only be
               * hoisted if its block dominates all loop exits (so it certainly
               * ran in the original). */
              int instr_block = bi;
              int dominates_exits = 1;
              for (int ei = 0; ei < cfg->num_blocks && dominates_exits; ei++) {
                if (!is_exit[ei])
                  continue;
                if (!tcc_ir_cfg_dominates(cfg, instr_block, ei))
                  dominates_exits = 0;
              }
              if (!dominates_exits) {
                /* This hoist speculates onto the zero-trip path (e.g. a rotated
                 * for-loop's body never dominates its guard exit).  Only sound
                 * for a pure fault-free op, and only PROFITABLE when the value
                 * is used more than once in the loop — a single-use invariant
                 * hoisted into a pressured loop just spills, costing more than
                 * the one op it saved — unless it is part of an invariant CHAIN
                 * that leaves as a unit for that same one register (chain_ok). */
                if (!licm_op_is_speculatable(q->op))
                  continue;
                IROperand hd = tcc_ir_op_get_dest(ir, q);
                int32_t hvr = irop_get_vreg(hd);
                int hu = 0;
                if (hvr >= 0) {
                  int hp = TCCIR_DECODE_VREG_POSITION(hvr);
                  int ht = TCCIR_DECODE_VREG_TYPE(hvr);
                  if (hp <= max_vr)
                    hu = use_count[ht * dc_stride + hp];
                }
                if (hu < 2 && !chain_ok[ii])
                  continue;
              }

              /* Every in-loop-defined source must itself have been hoisted
               * already — see hoisted_dest above. */
              {
                int src_unhoisted = 0;
                for (int oi = 0; oi < 2 && !src_unhoisted; oi++) {
                  if (oi == 0 && !irop_config[q->op].has_src1)
                    continue;
                  if (oi == 1 && !irop_config[q->op].has_src2)
                    continue;
                  IROperand sop = (oi == 0) ? tcc_ir_op_get_src1(ir, q)
                                            : tcc_ir_op_get_src2(ir, q);
                  int32_t svr = irop_get_vreg(sop);
                  if (svr < 0)
                    continue;
                  int sp2 = TCCIR_DECODE_VREG_POSITION(svr);
                  int st2 = TCCIR_DECODE_VREG_TYPE(svr);
                  if (sp2 <= max_vr && def_count[st2 * dc_stride + sp2] > 0 &&
                      !hoisted_dest[st2 * dc_stride + sp2])
                    src_unhoisted = 1;
                }
                if (src_unhoisted)
                  continue;
              }

              /* Clone and insert at preheader */
              IRQuadCompact hoist_q = {0};
              hoist_q.op = q->op;
              /* Carry orig_index across the move.  The side tables the backend
               * reads just before codegen (barrel_shifts, shift64_dead_half,
               * zero_half64, bfi_params) are keyed by it, so a clone left at
               * the zeroed default both loses its own annotation and aliases
               * instruction 0's slot — a hoisted `add rd,rn,rm lsl #4` came
               * back as a plain `add` (244_fuzz_entry_store_rt_base_plus_imm).
               * The original is NOP'd below, so the index is not duplicated. */
              hoist_q.orig_index = q->orig_index;
              hoist_q.line_num = q->line_num;
              IROperand orig_dest = tcc_ir_op_get_dest(ir, q);
              IROperand orig_src1 = tcc_ir_op_get_src1(ir, q);
              IROperand orig_src2 = tcc_ir_op_get_src2(ir, q);
              hoist_q.operand_base = tcc_ir_pool_add(ir, orig_dest);
              tcc_ir_pool_add(ir, orig_src1);
              tcc_ir_pool_add(ir, orig_src2);

              int adj_insert = insert_pos + total_hoisted_here;
              tcc_ir_insert_instruction_before(ir, adj_insert, &hoist_q);
              total_hoisted_here++;

              /* NOP out the original.  tcc_ir_insert_instruction_before shifts
               * all instructions at indices >= adj_insert.  If adj_insert
               * was before or at adj_ii, the original moved to adj_ii+1;
               * otherwise it stayed at adj_ii. */
              int nop_pos;
              if (adj_insert <= adj_ii) {
                nop_pos = adj_ii + 1;
                body_shift++;
              } else {
                nop_pos = adj_ii;
              }
              ir->compact_instructions[nop_pos].op = TCCIR_OP_NOP;
              licm_move_jump_target_off_nop(ir, nop_pos);
              hoisted++;
              {
                int32_t hdvr = irop_get_vreg(orig_dest);
                if (hdvr >= 0) {
                  int hdp = TCCIR_DECODE_VREG_POSITION(hdvr);
                  int hdt = TCCIR_DECODE_VREG_TYPE(hdvr);
                  if (hdp <= max_vr)
                    hoisted_dest[hdt * dc_stride + hdp] = 1;
                }
              }
              LOG_LICM("dom-LICM: hoisted insn %d (adj %d) op=%d to preheader pos %d, NOP'd %d",
                       ii, adj_ii, hoist_q.op, adj_insert, nop_pos);
            }
          }
          tcc_free(hoisted_dest);
          tcc_free(chain_ok);

          /* Update CFG block indices to account for inserted instructions.
           * Each tcc_ir_insert_instruction_before shifts all instructions >= insert_pos.
           * After total_hoisted_here insertions at insert_pos, all blocks
           * with indices >= insert_pos are shifted forward. */
          if (total_hoisted_here > 0) {
            for (int ui = 0; ui < cfg->num_blocks; ui++) {
              if (cfg->blocks[ui].start_idx >= insert_pos)
                cfg->blocks[ui].start_idx += total_hoisted_here;
              if (cfg->blocks[ui].end_idx >= insert_pos)
                cfg->blocks[ui].end_idx += total_hoisted_here;
            }
          }

          tcc_free(is_invariant);
          tcc_free(is_exit);
          tcc_free(def_count);
          tcc_free(use_count);
          tcc_free(in_loop);
          tcc_free(worklist);
        }
      }
    }
    tcc_ir_cfg_free(cfg);
  }

  /* Dom-LICM may have inserted instructions — re-detect loops so the
   * caller (IV strength reduction) gets valid indices. */
  if (hoisted > 0) {
    tcc_ir_free_loops(loops);
    loops = tcc_ir_detect_loops(ir);
  }

  return loops;
}
