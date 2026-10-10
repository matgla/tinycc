/*
 *  TCC IR - SSA-Aware Register Allocator
 *
 *  Operates directly on SSA-renamed IR with phi nodes.
 *  Replaces the tccls.c linear scan when -fssa-regalloc is enabled.
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* SSA register allocator: post-allocation passes -- redundant reload and
 * dead frame-store elimination, copy propagation and move coalescing. */

#include "regalloc_priv.h"

/* ============================================================================
 * Post-Allocation Redundant Reload Elimination
 *
 * Every soft-float routine opens by punning its arguments through a union:
 *
 *   StackLoc[-8] <-- R0(P0) [STORE]     ; ua.d = a
 *   R0(V0)      <-- StackLoc[-8] [LOAD] ; a_bits = ua.u
 *
 * which lowers to `strd r0,r1,[sp,#N]` immediately followed by
 * `ldrd r0,r1,[sp,#N]` -- a reload of registers that already hold the value.
 * sl_forward refuses to forward it because the two ends have different types
 * (FLOAT64 -> INT64), and forwarding it BEFORE allocation was measured and
 * reverted: keeping the punned value in a register stretched a 64-bit live
 * range and the linear-scan allocator cascaded into spills (dadd +13.4%).
 *
 * After allocation that trade no longer exists.  The registers are already
 * chosen, so dropping a load that rewrites a register with the value it
 * already holds cannot lengthen anything -- it only removes a memory access.
 *
 * The analysis is a straight-line available-value table: remember which
 * register pair was last stored to each frame slot, drop a later load of that
 * slot into exactly those registers, and invalidate on anything that could
 * disturb either side.
 * ==========================================================================*/

/* An allocation half carries its physical register in the low five bits and a
 * spill flag at 0x20, but an UNALLOCATED half is the all-ones sentinel -- and
 * that sentinel has the spill bit set.  So "is this half spilled" is only a
 * question once the half names a register; asked unconditionally it answers
 * yes for the absent high half of every 32-bit value.  Codegen already reads
 * it this way (ir/codegen.c takes pr1_reg and pr1_spilled apart separately).
 */
static int ra_alloc_half_unset(uint16_t half)
{
  return (half & PREG_REG_NONE) == PREG_REG_NONE;
}

static int ra_alloc_half_spilled(uint16_t half)
{
  return !ra_alloc_half_unset(half) && (half & PREG_SPILLED) != 0;
}

/* A direct frame slot: `StackLoc[off]` as a memory operand (an `Addr[...]`
 * form has is_lval clear and is an address, not a location). */
static int ra_rl_slot_offset(IROperand op, int *off)
{
  if (op.tag != IROP_TAG_STACKOFF || op.is_llocal || op.is_sym)
    return 0;
  if (irop_get_vreg(op) != -1 || !op.is_lval)
    return 0;
  *off = (int)irop_get_stack_offset(op);
  return 1;
}

/* Physical registers behind a plain register operand, as codegen resolves it. */
static int ra_rl_operand_regs(TCCIRState *ir, IROperand op, int *r0, int *r1)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || !tcc_ir_vreg_is_valid(ir, vr))
    return 0;
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, vr);
  if (!li || li->allocation.offset != 0)
    return 0;
  /* An s-register (hard-float) is no core register: masked to five bits, s0
   * would pass for r0 and a word copy of a float parameter's home would read
   * whatever r0 holds. */
  if (LS_IS_VFP_REG(li->allocation.r0) || LS_IS_VFP_REG(li->allocation.r1))
    return 0;
  if (ra_alloc_half_spilled(li->allocation.r0) || ra_alloc_half_spilled(li->allocation.r1))
    return 0;
  int a0 = li->allocation.r0 & PREG_REG_NONE;
  int a1 = li->allocation.r1 & PREG_REG_NONE;
  if (a0 == PREG_REG_NONE)
    return 0;
  *r0 = a0;
  *r1 = (a1 == PREG_REG_NONE) ? -1 : a1;
  return 1;
}

/* Bytes an access moves when the register ends up holding EXACTLY the slot's
 * contents.  A sub-word access does not: `strb r2,[sp,#N]` writes only r2's
 * low byte and `ldrb r2,[sp,#N]` brings it back zero-extended, so neither end
 * of such a pair may be matched against the other or against a word access.
 * FLOAT64/INT64 (and FLOAT32/INT32) share a width because a 64-bit soft-float
 * value lives in the same GPR pair as the integer it was punned from -- that
 * equivalence is the whole point of the pass. */
static int ra_rl_exact_width(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 0;
  }
}

/* Keep `regs` occupied over [lo,hi] so the encoder cannot hand them out.
 *
 * A register outside every live interval holds nothing the allocator is
 * accounting for, and the encoder helps itself to exactly those whenever it
 * has to materialise an operand: `R0 <- StackLoc[-12] ADD StackLoc[-36]` is
 * three instructions, two of them loads of the operands into whatever happens
 * to be free.  Neither load exists at IR level, so nothing in this pass sees
 * them -- it would go on believing a slot's value is in a register the
 * encoder had already reused (nested_struct_return printed dx + dy where it
 * meant p.y + dy).
 *
 * Refusing whenever the register is dead somewhere in between is correct but
 * throws away most of what this pass is for.  Reserving it instead makes the
 * assumption true: both scratch pickers -- tcc_ls_find_free_scratch_reg and
 * scratch_pushed_dead_reg -- union live_regs_by_instruction[] into what they
 * consider taken, and codegen has not run yet.  Nothing else owns the
 * register over this window, or it would have been live and there would be
 * nothing to reserve. */
static void ra_rl_reserve_regs(LSLiveIntervalState *ls, int r0, int r1, int lo, int hi)
{
  if (!ls->live_regs_by_instruction)
    return;
  for (int k = lo; k <= hi && k < ls->live_regs_by_instruction_size; ++k)
  {
    if (k < 0)
      continue;
    if (r0 >= 0)
      ls->live_regs_by_instruction[k] |= (1u << r0);
    if (r1 >= 0)
      ls->live_regs_by_instruction[k] |= (1u << r1);
  }
}

/* Does `q` leave every register and every frame slot alone except the ones its
 * IR destination names?  Only then may a slot->register fact outlive it.
 *
 * This is deny-by-default on purpose.  The pass used to list the ops that can
 * disturb a slot or a register and treat everything else as harmless, so each
 * op nobody thought of kept its facts: a BLOCK_COPY (its lowering runs on
 * r0-r3/r12/lr as cursors and data registers, and it writes the frame without
 * being a STORE), a __builtin_apply, a longjmp, a switch dispatch (r12), a
 * soft-float operation that still ends in an __aeabi_* helper (the helper
 * clobbers r0-r3 whatever register the result is moved to).  What is allowed
 * through is named instead: an op whose only hazards are reading memory,
 * touching the flags, or transferring control (the join that follows resets
 * the table), and whose destination is a register.  Anything else -- calls and
 * their setup, memory writes, asm, VLAs, non-local control, the static chain,
 * and any hazard class added to op_props.h later -- starts from nothing. */
static int ra_rl_op_keeps_facts(TCCIRState *ir, const IRQuadCompact *q)
{
  const TccIrOp op = (TccIrOp)q->op;

  if (ir_op_has(op, IR_HZ_FROM_OP & ~(IR_HZ_MEM_READ | IR_HZ_FLAGS_SET | IR_HZ_FLAGS_READ | IR_HZ_BRANCH |
                                      IR_HZ_RETURN | IR_HZ_TRAP | IR_HZ_HINT)))
    return 0;

  /* The dispatch and its value-table read use r12 as a scratch; the table
   * says "branch" and "memory read", which are otherwise harmless. */
  if (op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
    return 0;

  /* An FP op that is lowered to an __aeabi_* helper by the backend. */
  if (ir_op_is_implicit_call_ra(op))
    return 0;

  /* A store through the destination of an op that is not a STORE. */
  if (irop_config[op].has_dest && tcc_ir_op_get_dest(ir, (IRQuadCompact *)q).is_lval)
    return 0;

  return 1;
}

static int ra_redundant_reload_elim(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;
  const int n = ir->next_instruction_index;
  int removed = 0;

  if (TCC_OPT(tcc_state, optimize) < 1)
    return 0;
  if (tcc_ir_opt_pass_disabled("ra:reload_elim"))
    return 0;

  /* A slot whose address is taken anywhere can be written through that
   * pointer, so it is never tracked. */
  int escaped[RA_REL_MAX_TRACKED];
  int nescaped = 0;
  int escape_overflow = 0;
  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 4; ++k)
    {
      IROperand op = (k == 0)   ? (irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE)
                     : (k == 1) ? (irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE)
                     : (k == 2) ? (irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE)
                                : (tcc_ir_op_is_mac(q->op) ? tcc_ir_op_get_accum(ir, q) : IROP_NONE);
      if (op.tag != IROP_TAG_STACKOFF || op.is_llocal || op.is_lval)
        continue;
      if (irop_get_vreg(op) != -1)
        continue;
      int off = (int)irop_get_stack_offset(op); /* Addr[StackLoc[off]] */
      if (nescaped < RA_REL_MAX_TRACKED)
        escaped[nescaped++] = off;
      else
        escape_overflow = 1;
    }
  }

  RARelSlot tracked[RA_REL_MAX_TRACKED];
  int ntracked = 0;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Control can arrive here from anywhere else; nothing carries over. */
    if (q->is_jump_target)
      ntracked = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* STORE and LOAD are the two ops this table is about and are handled
     * below.  Every other op must prove it leaves the facts alone. */
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_LOAD && !ra_rl_op_keeps_facts(ir, q))
    {
      ntracked = 0;
      continue;
    }

    int slot_off = 0;

    /* Drop a load that rewrites the very registers already holding the slot. */
    if (q->op == TCCIR_OP_LOAD && ntracked &&
        ra_rl_slot_offset(tcc_ir_op_get_src1(ir, q), &slot_off) &&
        !tcc_ir_access_is_volatile(ir, tcc_ir_op_get_src1(ir, q)))
    {
      int d0 = 0, d1 = 0;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int width = ra_rl_exact_width(tcc_ir_op_src1_btype(ir, q));
      if (width && !dest.is_lval && ra_rl_operand_regs(ir, dest, &d0, &d1) &&
          ((d1 >= 0) == (width == 8)))
      {
        for (int t = 0; t < ntracked; ++t)
        {
          if (!tracked[t].valid || tracked[t].off != slot_off ||
              tracked[t].size != width)
            continue;
          if (tracked[t].r0 != d0 || tracked[t].r1 != d1)
            break;
          ra_rl_reserve_regs(ls, tracked[t].r0, tracked[t].r1, tracked[t].def_idx, i);
          /* With the load gone the destination has no definition of its own:
           * it holds the slot's value only because it shares a register with
           * the stored value.  Pin both, as ra_phi_copy_needed does for an
           * elided identity copy, or codegen's scratch-conflict fixup
           * (try_reassign_scratch_conflict) moves the destination to a
           * register nothing ever writes. */
          tcc_ir_vreg_live_interval(ir, irop_get_vreg(dest))->phi_pinned = 1;
          tcc_ir_vreg_live_interval(ir, tracked[t].vreg)->phi_pinned = 1;
          q->op = TCCIR_OP_NOP;
          removed++;
          RA_DBG("reload_elim @%d: slot %d already in R%d/R%d", i, slot_off, d0, d1);
          break;
        }
      }
      if (q->op == TCCIR_OP_NOP)
        continue;
    }

    /* Record a store, after invalidating whatever it overlaps. */
    if (q->op == TCCIR_OP_STORE &&
        ra_rl_slot_offset(tcc_ir_op_get_dest(ir, q), &slot_off))
    {
      IROperand val = tcc_ir_op_get_src1(ir, q);
      int s0 = 0, s1 = 0;
      int dst_bt = tcc_ir_op_dest_btype(ir, q);
      int size = ra_rl_exact_width(dst_bt);
      int inval_size = size;
      if (!size)
      {
        /* A sub-word store writes at most two bytes, so invalidating a whole
         * word over-covers it; it leaves no register holding the slot, so it
         * records nothing.  Anything else (a struct copy) has a width this
         * code cannot see -- assume it reached everything. */
        if (dst_bt != IROP_BTYPE_INT8 && dst_bt != IROP_BTYPE_INT16)
        {
          ntracked = 0;
          continue;
        }
        inval_size = 4;
      }

      for (int t = 0; t < ntracked; ++t)
        if (tracked[t].valid && slot_off < tracked[t].off + tracked[t].size &&
            tracked[t].off < slot_off + inval_size)
          tracked[t].valid = 0;

      int is_escaped = escape_overflow;
      for (int e = 0; e < nescaped && !is_escaped; ++e)
        if (escaped[e] == slot_off)
          is_escaped = 1;

      /* A volatile store is an access of its own, not a value to forward from. */
      if (size && !is_escaped && !tcc_ir_access_is_volatile(ir, tcc_ir_op_get_dest(ir, q)) && !val.is_lval &&
          ra_rl_operand_regs(ir, val, &s0, &s1) &&
          ((s1 >= 0) == (size == 8)) && ra_rl_exact_width(irop_get_btype(val)) == size &&
          ntracked < RA_REL_MAX_TRACKED)
      {
        tracked[ntracked].off = slot_off;
        tracked[ntracked].size = size;
        tracked[ntracked].r0 = s0;
        tracked[ntracked].r1 = s1;
        tracked[ntracked].vreg = irop_get_vreg(val);
        tracked[ntracked].def_idx = i;
        tracked[ntracked].valid = 1;
        ntracked++;
      }
      continue;
    }

    /* A store to a slot we could not decode, or through a vreg, may hit
     * anything we are holding. */
    if (q->op == TCCIR_OP_STORE)
    {
      ntracked = 0;
      continue;
    }

    /* Any other write to a register drops the entries that named it. */
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int d0 = 0, d1 = 0;
      if (!dest.is_lval && ra_rl_operand_regs(ir, dest, &d0, &d1))
      {
        for (int t = 0; t < ntracked; ++t)
          if (tracked[t].valid &&
              (tracked[t].r0 == d0 || tracked[t].r1 == d0 ||
               (d1 >= 0 && (tracked[t].r0 == d1 || tracked[t].r1 == d1))))
            tracked[t].valid = 0;
      }
      else if (!dest.is_lval && irop_get_vreg(dest) >= 0)
      {
        /* Destination register unknown -- assume it hit everything. */
        ntracked = 0;
      }
    }
  }

  return removed;
}

/* ============================================================================
 * Post-Allocation Dead Frame-Store Elimination
 *
 * The reload elimination above is what leaves these behind.  Every soft-float
 * routine opens by punning its arguments through a union:
 *
 *   StackLoc[-8] <-- R0(P0) [STORE]      ; ua.d = a
 *   R0(V0)       <-- StackLoc[-8] [LOAD] ; a_bits = ua.u
 *
 * Once the reload is gone nothing in the function reads that slot again, and
 * the store is `strd r0,r1,[sp,#N]` writing two words no one will ever look at.
 * __aeabi_dadd opens with two such slots, __aeabi_dmul with four.  The IR-level
 * dead-store pass (dce_dead_stackloc_stores) cannot reach them: it runs before
 * allocation, while the reload is still a reader.
 *
 * The rule is deliberately blunt -- a store dies only when NO instruction
 * anywhere in the function names an overlapping byte of its slot -- so it needs
 * no ordering or dominance reasoning and stays correct however control flows.
 * Anything that could reach a frame slot without naming it rules the whole
 * function out: an address taken of any slot, an indexed access based on one
 * (which reaches past the operand's own width), inline asm, setjmp, a VLA
 * moving sp, or a nested function's static chain.
 * ==========================================================================*/

/* Bytes an access to a frame slot covers, or 0 when the width is not
 * statically known -- a struct copy or a complex pair reaches further than its
 * component btype says. */
static int ra_dfs_width(IROperand op)
{
  if (op.is_complex)
    return 0;
  switch (irop_get_btype(op))
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 0;
  }
}

/* A plain frame slot operand: `StackLoc[off]` naming memory directly -- the
 * frontend's temporary slots included, which carry a negative vreg
 * (VR_TEMP_LOCAL).  Excluding those hid `Addr[temporary]` from the escape
 * check below, and a store to the same slot through a -1 operand (lea_fold
 * makes those) was deleted although a call read the slot. */
static int ra_dfs_is_slot(IROperand op)
{
  return op.tag == IROP_TAG_STACKOFF && !op.is_sym && irop_get_vreg(op) < 0;
}

/* True when the store at `at`, writing [off, off+w), is dead because control
 * runs straight from it to a RETURN with nothing reading the slot on the way.
 *
 * This is the case the whole-function scan above cannot take.  A soft-float
 * routine ends `ur.u = <value>; return ur.d;` -- and once post-allocation
 * reload elimination has dropped the matching load, because the value is
 * already in the return register, the store writes eight bytes that the
 * epilogue is about to abandon.  The blunt rule keeps it, because the SAME
 * slot is read on the cold NaN and zero paths that returned earlier.  Those
 * reads are unreachable from here, which is exactly what a straight-line walk
 * to the return can prove without any dataflow.
 *
 * Everything that could get control somewhere else -- a jump, a call, an
 * indirect branch -- ends the walk conservatively.  A jump TARGET does not:
 * it only adds a way in, and every way in still runs the same read-free tail
 * to the same return.  The caller has already ruled out the whole function on
 * an escaping address, inline asm, setjmp, a VLA or a static chain, so a slot
 * can only be reached here by being named. */
static int ra_dfs_dead_to_return(TCCIRState *ir, int at, int off, int w)
{
  const int n = ir->next_instruction_index;
  int steps = 0;

  for (int j = at + 1; j < n && steps < RA_DFS_TAIL_LIMIT; ++j)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    steps++;

    if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      return 1;

    /* Anything that can transfer control elsewhere, or that could read the
     * frame through something other than a named slot. */
    switch (q->op)
    {
    case TCCIR_OP_STORE:
    case TCCIR_OP_LOAD:
    case TCCIR_OP_ASSIGN:
      break;
    default:
      if (irop_config[q->op].has_dest && tcc_ir_op_dest_vreg(ir, q) >= 0)
        break; /* a plain computation into a register */
      return 0;
    }

    /* slot 3 too: an MLA/MLS accumulator reads its slot like any source */
    for (int k = 0; k < 4; ++k)
    {
      IROperand op = (k == 0)   ? (irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE)
                     : (k == 1) ? (irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE)
                     : (k == 2) ? (irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE)
                                : (tcc_ir_op_is_mac(q->op) ? tcc_ir_op_get_accum(ir, q) : IROP_NONE);
      if (!ra_dfs_is_slot(op))
        continue;
      /* A plain STORE's destination overwrites the slot, it does not read it;
       * anything else naming an overlapping byte keeps this store alive. */
      if (k == 0 && q->op == TCCIR_OP_STORE)
        continue;
      const int ow = ra_dfs_width(op);
      if (ow == 0)
        return 0;
      const int ooff = (int)irop_get_stack_offset(op);
      if (off < ooff + ow && ooff < off + w)
        return 0;
    }
  }
  return 0;
}

static int ra_dead_frame_store_elim(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  int removed = 0;

  if (TCC_OPT(tcc_state, optimize) < 1)
    return 0;
  if (tcc_ir_opt_pass_disabled("ra:dead_frame_store"))
    return 0;
  if (ir->has_static_chain)
    return 0;

  struct
  {
    int off;
    int size;
  } reads[RA_DFS_MAX_READS];
  int nreads = 0;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
      return 0;
    default:
      break;
    }

    /* An indexed or post-incrementing access based on a slot runs off the end
     * of the operand's own btype, so its width says nothing about what it
     * touched. */
    const int reaches_past_width =
        (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
         q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC);

    for (int k = 0; k < 4; ++k)
    {
      IROperand op = (k == 0)   ? (irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE)
                     : (k == 1) ? (irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE)
                     : (k == 2) ? (irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE)
                                : (tcc_ir_op_is_mac(q->op) ? tcc_ir_op_get_accum(ir, q) : IROP_NONE);
      if (!ra_dfs_is_slot(op))
        continue;
      /* `Addr[StackLoc[..]]` hands the address to something this scan cannot
       * follow; is_llocal reaches memory through a pointer held in the slot. */
      if (!op.is_lval || op.is_llocal || reaches_past_width)
        return 0;
      /* The destination of a plain STORE is the write under consideration, not
       * a read.  Every other appearance keeps the slot alive. */
      if (k == 0 && q->op == TCCIR_OP_STORE)
        continue;
      int w = ra_dfs_width(op);
      if (w == 0 || nreads >= RA_DFS_MAX_READS)
        return 0;
      reads[nreads].off = (int)irop_get_stack_offset(op);
      reads[nreads].size = w;
      nreads++;
    }
  }

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!ra_dfs_is_slot(dest) || !dest.is_lval || dest.is_llocal)
      continue;
    if (tcc_ir_access_is_volatile(ir, dest))
      continue;
    const int w = ra_dfs_width(dest);
    if (w == 0)
      continue;
    const int off = (int)irop_get_stack_offset(dest);
    int is_read = 0;
    for (int r = 0; r < nreads && !is_read; ++r)
      if (off < reads[r].off + reads[r].size && reads[r].off < off + w)
        is_read = 1;
    if (is_read && !ra_dfs_dead_to_return(ir, i, off, w))
      continue;
    q->op = TCCIR_OP_NOP;
    removed++;
    RA_DBG("dead_frame_store @%d: slot %d (%d bytes) is dead (%s)", i, off, w,
           is_read ? "straight line to the return" : "never read");
  }

  return removed;
}

/* ============================================================================
 * Post-Allocation Copy Propagation
 *
 * Move coalescing (below) removes a register copy by REASSIGNING one of its
 * endpoints so the move degenerates to `mov rX,rX`.  Both of its directions
 * need one endpoint's register to be free across the other's live range, so
 * neither can fire when the source OUTLIVES the copy: the source is still
 * sitting in its own register, which is precisely why that register is not
 * free.
 *
 * That is the dominant shape in soft float, where one union-punned 64-bit
 * value feeds a dozen inlined accessors:
 *
 *   R4(T8)  <-- ...                  ; a_bits, live to the end of the function
 *   R8(T18) <-- &R4(T8) [ASSIGN]     ; mov r8,r4 / mov r9,r5
 *   R8(T19) <-- &R8(T18) SHR #52     ; reads only the high half
 *
 * The complementary transform needs no free register at all: rewrite T18's
 * reads back to T8 and drop the copy.  That is legal exactly when T8's
 * register still holds T8 everywhere T18 is live, and the allocator's own
 * invariant -- one live interval per physical register at a time -- makes
 * that checkable by scanning the interval list for another claimant.
 *
 * Nothing here changes an allocation, so unlike every hint- or
 * forwarding-based attempt on these same copies, it cannot stretch a live
 * range and trade a move for a spill.  Each removed quad is a removed
 * instruction.
 * ==========================================================================*/

/* Ops whose operands are pinned to particular physical registers (ABI slots,
 * asm constraints, the static chain) or whose destination must not alias a
 * source: a read inside one of these cannot be redirected to another
 * register. */
static int ra_cp_use_is_redirectable(int op)
{
  switch (op)
  {
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_SET_CHAIN:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SWITCH_LOAD:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_UMULL:
  case TCCIR_OP_MLA:
  case TCCIR_OP_UMAAL:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
    return 0;
  default:
    return 1;
  }
}

/* Does any interval other than `keep` occupy one of `keep`'s registers at some
 * point in [lo,hi]?  If not, `keep`'s registers still hold `keep`'s value
 * across the whole window and reads of a copy of it can be redirected there. */
static int ra_cp_src_regs_clobbered(const LSLiveIntervalState *ls,
                                    const LSLiveInterval *keep,
                                    uint32_t lo, uint32_t hi)
{
  int regs[2];
  int nregs = 0;
  regs[nregs++] = keep->r0;
  if (keep->r1 >= 0 && keep->r1 < PREG_NONE && keep->r1 != keep->r0)
    regs[nregs++] = keep->r1;

  for (int j = 0; j < ls->next_interval_index; ++j)
  {
    const LSLiveInterval *x = &ls->intervals[j];
    if (x == keep || x->stack_location != 0)
      continue;
    if (x->start > hi || x->end < lo)
      continue;
    for (int r = 0; r < nregs; ++r)
      if (x->r0 == regs[r] || x->r1 == regs[r])
      {
        return 1;
      }
  }
  return 0;
}

/* Rewrite the reads of a copy's destination back to its source and drop the
 * copy.  Returns the number of copies removed. */
static int ra_copy_propagate(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;
  const int n = ir->next_instruction_index;
  int removed = 0;

  if (TCC_OPT(tcc_state, optimize) < 1)
    return 0;
  if (!ls->live_regs_by_instruction || ls->live_regs_by_instruction_size <= 0)
    return 0;
  if (tcc_ir_opt_pass_disabled("ra:copy_prop"))
    return 0;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src = tcc_ir_op_get_src1(ir, q);
    /* A LOAD off a vreg whose interval ended up in a register performs no
     * memory access -- it is a register copy at codegen, and the IR generator
     * emits the inlined `temp = var` binding in exactly that form.  Skip
     * is_llocal (a real dereference through a pointer) and is_sym (a global).
     * Sub-word LOADs emit a UXTB/SXTH alongside the move to narrow the value,
     * so only full-width copies qualify. */
    int is_copy_load = 0;
    if (q->op == TCCIR_OP_LOAD)
    {
      int dbt = irop_get_btype(dest), sbt = irop_get_btype(src);
      /* is_lval on a STACKOFF source is the VAR's own slot being read, which
       * for a register-resident VAR is a register copy.  On a VREG source it
       * is a DEREFERENCE OF THAT REGISTER -- `R4 <- R3***DEREF***` reads the
       * memory R3 points at, and rewriting its readers to R3 hands them the
       * address instead of the value (pr90311: `b -= c < (unsigned char) a`
       * subtracted from &b).  is_llocal is not the flag that separates them;
       * a plain pointer dereference does not set it. */
      int deref_through_reg = (src.tag == IROP_TAG_VREG && src.is_lval);
      if ((src.tag == IROP_TAG_VREG || (src.tag == IROP_TAG_STACKOFF && src.is_local)) &&
          !deref_through_reg && !src.is_llocal && !src.is_sym && dbt == sbt &&
          (dbt == IROP_BTYPE_INT32 || dbt == IROP_BTYPE_INT64 || dbt == IROP_BTYPE_FUNC))
        is_copy_load = 1;
      if (!is_copy_load)
        continue;
    }
    /* Plain register-to-register only; an lval on either side is memory --
     * except a copy-LOAD, whose source carries is_lval for the VAR read. */
    if (dest.is_lval || (src.is_lval && !is_copy_load))
      continue;
    if (irop_get_btype(dest) != irop_get_btype(src))
      continue;

    int32_t dv = irop_get_vreg(dest);
    int32_t sv = irop_get_vreg(src);
    if (dv < 0 || sv < 0 || dv == sv)
      continue;
    if (!tcc_ir_vreg_is_valid(ir, dv) || !tcc_ir_vreg_is_valid(ir, sv))
      continue;
    /* The destination must be a temp: a VAR has a home beyond its register.
     * The source may be a VAR -- a register-resident one is just a register,
     * and it is the punned `a_bits` that every inlined accessor re-reads --
     * but then every write to it counts as a redefinition (below). */
    int src_is_var = (TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_VAR);
    if (TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_TEMP && !src_is_var)
      continue;

    /* One interval per endpoint.  A vreg split across several intervals can
     * hold a DIFFERENT register in each, and the range reasoning below reads
     * a single interval's bounds -- picking one of several would let a read
     * be redirected into the register the other half was living in. */
    LSLiveInterval *src_iv = NULL, *dst_iv = NULL;
    int src_ivs = 0, dst_ivs = 0;
    for (int j = 0; j < ls->next_interval_index; ++j)
    {
      if (ls->intervals[j].vreg == (uint32_t)sv) { src_iv = &ls->intervals[j]; src_ivs++; }
      if (ls->intervals[j].vreg == (uint32_t)dv) { dst_iv = &ls->intervals[j]; dst_ivs++; }
    }
    if (!src_iv || !dst_iv || src_ivs != 1 || dst_ivs != 1)
      continue;
    /* PREG_NONE/PREG_SPILLED are >= 0 but are not registers. */
    if (src_iv->r0 < 0 || src_iv->r0 >= PREG_NONE)
      continue;
    if (src_iv->stack_location != 0)
      continue;
    /* A SPILLED destination is the better case, not a disqualifying one: the
     * copy the allocator could not keep in a register costs a store and a
     * reload at every read, and redirecting those reads to the source removes
     * all of it.  __aeabi_dadd's `b_bits` is exactly this -- one 64-bit ASSIGN
     * off the parameter, spilled, then loaded back four bytes at a time.  The
     * legality argument below never mentions the destination's register, only
     * that the SOURCE's registers still hold the source across the reads, so
     * it carries over unchanged. */
    const int dst_spilled = (dst_iv->r0 < 0 || dst_iv->r0 >= PREG_NONE || dst_iv->stack_location != 0);
    /* Both ends must sit in the same register class, and a 64-bit class means
     * a register PAIR: redirecting a paired read to an interval that only owns
     * r0 hands the backend a half-formed operand (it asserts on r1 == 31).  A
     * register-resident VAR can be exactly that. */
    if (src_iv->reg_type != dst_iv->reg_type)
      continue;
    if (!dst_spilled)
    {
      int src_pair = (src_iv->r1 >= 0 && src_iv->r1 < PREG_NONE);
      int dst_pair = (dst_iv->r1 >= 0 && dst_iv->r1 < PREG_NONE);
      if (src_pair != dst_pair)
        continue;
    }
    if (src_iv->addrtaken || dst_iv->addrtaken)
      continue;
    /* dst's register is shared with a graph-coalesced class that expects this
     * write to land; src is only read, so its class membership is harmless. */
    if (dst_iv->co_member)
      continue;
    /* The LS intervals above drive the allocator; codegen resolves an operand
     * through the IR-level interval's `allocation` instead, and the two are
     * not interchangeable -- a VAR can be register-resident there with no high
     * half, which the backend rejects the moment a 64-bit read names it.
     * Validate against the structure codegen will actually consult. */
    {
      IRLiveInterval *src_li = tcc_ir_vreg_live_interval(ir, sv);
      IRLiveInterval *dst_li = tcc_ir_vreg_live_interval(ir, dv);
      if (!src_li || !dst_li)
        continue;
      /* A phi-pinned dest shares its register with an identity-elided phi
       * partner (post_ra_forward_diamond / ra_phi_copy_needed): the partner's
       * reads continue PAST dst's own interval, and this copy is the only
       * write that puts the value there.  Removing it leaves the shared
       * register stale on the path the elided copy used to cover — the
       * ra_refine_live_regs_accurate clamp-diamond self-host break. */
      if (dst_li->phi_pinned)
        continue;
      if (src_li->allocation.offset != 0)
        continue;
      if (ra_alloc_half_spilled(src_li->allocation.r0) ||
          ra_alloc_half_spilled(src_li->allocation.r1))
        continue;
      if (ra_alloc_half_unset(src_li->allocation.r0))
        continue;
      /* The source must carry the full width the reads expect.  With a
       * register-resident destination that is "both are pairs or neither is";
       * with a spilled one there is no destination pair to compare against, so
       * the operand btype -- already required equal above -- decides. */
      const int src_paired = ((src_li->allocation.r1 & PREG_REG_NONE) != PREG_REG_NONE);
      if (!dst_spilled)
      {
        if (dst_li->allocation.offset != 0)
          continue;
        if (ra_alloc_half_spilled(dst_li->allocation.r0) ||
            ra_alloc_half_spilled(dst_li->allocation.r1))
          continue;
        if (ra_alloc_half_unset(dst_li->allocation.r0))
          continue;
        if (src_paired != ((dst_li->allocation.r1 & PREG_REG_NONE) != PREG_REG_NONE))
          continue;
      }
      else if (src_paired != (irop_get_btype(dest) == IROP_BTYPE_INT64 ||
                              irop_get_btype(dest) == IROP_BTYPE_FLOAT64))
        continue;
    }

    /* Already the same register: codegen elides it, nothing to remove. */
    if (src_iv->r0 == dst_iv->r0 && src_iv->r1 == dst_iv->r1)
      continue;
    /* src must already be live at the copy. */
    if (src_iv->start > (uint32_t)i)
      continue;

    /* Collect dst's reads.  Require exactly one definition (this copy), every
     * read after it, and every read in an op whose operands are not pinned. */
    int ok = 1;
    int nuses = 0;
    int last_use = i;
    for (int k = 0; k < n; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (k == i || qk->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[qk->op].has_dest &&
          tcc_ir_op_dest_vreg(ir, qk) == dv)
      { ok = 0; break; } /* redefined, or written through as an address */

      int reads = 0;
      if (irop_config[qk->op].has_src1 &&
          tcc_ir_op_src1_vreg(ir, qk) == dv)
        reads = 1;
      if (!reads && irop_config[qk->op].has_src2 &&
          tcc_ir_op_src2_vreg(ir, qk) == dv)
        reads = 1;
      if (!reads && tcc_ir_op_is_mac(qk->op) &&
          tcc_ir_op_accum_vreg(ir, qk) == dv)
        reads = 1;
      if (!reads)
        continue;

      if (k < i || !ra_cp_use_is_redirectable(qk->op))
      { ok = 0; break; }
      nuses++;
      if (k > last_use)
        last_use = k;
    }
    /* nuses == 0 is a dead copy; ra_eliminate_dead_reg_copies owns that. */
    if (!ok || nuses == 0)
      continue;

    /* src's value must survive to the last redirected read. */
    if (src_iv->end < (uint32_t)last_use)
      continue;
    /* ...and no other interval may claim src's registers over that window. */
    if (ra_cp_src_regs_clobbered(ls, src_iv, (uint32_t)i, (uint32_t)last_use))
      continue;
    /* ...and src itself must not be redefined under the reads. */
    for (int k = i + 1; k <= last_use; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP || !irop_config[qk->op].has_dest)
        continue;
      IROperand dk = tcc_ir_op_get_dest(ir, qk);
      /* For a temp, an lval destination is a store THROUGH it (an address
       * use), not a new value.  For a VAR it is a write to the variable, so
       * there is no exemption. */
      if (dk.is_lval && !src_is_var)
        continue;
      if (irop_get_vreg(dk) == sv)
      { ok = 0; break; }
    }
    if (!ok)
      continue;

    for (int k = i + 1; k <= last_use; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[qk->op].has_src1)
      {
        IROperand o = tcc_ir_op_get_src1(ir, qk);
        if (irop_get_vreg(o) == dv)
        { irop_set_vreg(&o, sv); tcc_ir_set_src1(ir, k, o); }
      }
      if (irop_config[qk->op].has_src2)
      {
        IROperand o = tcc_ir_op_get_src2(ir, qk);
        if (irop_get_vreg(o) == dv)
        { irop_set_vreg(&o, sv); tcc_ir_set_src2(ir, k, o); }
      }
    }
    q->op = TCCIR_OP_NOP;
    removed++;
    RA_DBG("copy_prop @%d: T%d -> T%d (R%d), %d use(s) to %d", i,
           (int)(dv & 0xffffff), (int)(sv & 0xffffff), src_iv->r0, nuses, last_use);
  }

  return removed;
}

/* ============================================================================
 * Post-Allocation Move Coalescing
 *
 * Eliminates register-to-register copies (ASSIGN dest = src) by making both
 * sides share the same physical register.  The source must die at the ASSIGN
 * instruction, the new register must be free for the destination's entire
 * live range, and call-crossing safety must be preserved.
 *
 * The code generator already elides identity moves (mov rX, rX), so a
 * successful coalescing eliminates the copy without modifying the IR.
 * ============================================================================ */

static int ra_count_copies_reaching_codegen(TCCIRState *ir)
{
  int copies = 0;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);
    int32_t dest_vreg = tcc_ir_op_dest_vreg(ir, q);
    int32_t src_vreg = irop_get_vreg(src);
    if (tcc_ir_op_dest_is_lval(ir, q) || src.is_lval || dest_vreg < 0 || src_vreg < 0)
      continue;
    if (tcc_ir_op_dest_btype(ir, q) != irop_get_btype(src)) {
      copies++;
      continue;
    }

    IRLiveInterval *dest_li = tcc_ir_vreg_live_interval(ir, dest_vreg);
    IRLiveInterval *src_li = tcc_ir_vreg_live_interval(ir, src_vreg);
    if (!ra_phi_copy_is_identity(dest_li, src_li))
      copies++;
  }

  return copies;
}

/* Is `vr` mentioned by any instruction at all? */
static int ra_rp_vreg_mentioned(TCCIRState *ir, uint32_t vr)
{
  const int n = ir->next_instruction_index;
  for (int k = 0; k < n; ++k)
  {
    IRQuadCompact *qk = &ir->compact_instructions[k];
    if (qk->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[qk->op].has_dest &&
        (uint32_t)tcc_ir_op_dest_vreg(ir, qk) == vr)
      return 1;
    if (irop_config[qk->op].has_src1 &&
        (uint32_t)tcc_ir_op_src1_vreg(ir, qk) == vr)
      return 1;
    if (irop_config[qk->op].has_src2 &&
        (uint32_t)tcc_ir_op_src2_vreg(ir, qk) == vr)
      return 1;
    if (tcc_ir_op_is_mac(qk->op) &&
        (uint32_t)tcc_ir_op_accum_vreg(ir, qk) == vr)
      return 1;
  }
  return 0;
}

/* Is any of `keep`'s registers claimed by another interval over [lo,hi]?
 *
 * ra_cp_src_regs_clobbered answers the same question, but every pass that
 * deletes an instruction leaves the interval of the value it computed behind:
 * the vreg is then mentioned nowhere, yet its interval still claims its
 * registers over the range it used to occupy.  Such a phantom holds no value
 * and cannot clobber anything -- and because it sits exactly where the deleted
 * instruction was, it lands on the one-instruction window this pass asks
 * about far more often than its share.  So a claim is only believed once its
 * vreg is shown to still exist in the instruction stream.
 */
static int ra_rp_dst_regs_busy(TCCIRState *ir, const LSLiveIntervalState *ls,
                               const LSLiveInterval *keep, uint32_t lo, uint32_t hi)
{
  int regs[2];
  int nregs = 0;
  regs[nregs++] = keep->r0;
  if (keep->r1 >= 0 && keep->r1 < PREG_NONE && keep->r1 != keep->r0)
    regs[nregs++] = keep->r1;

  for (int j = 0; j < ls->next_interval_index; ++j)
  {
    const LSLiveInterval *x = &ls->intervals[j];
    if (x == keep || x->stack_location != 0)
      continue;
    if (x->start > hi || x->end < lo)
      continue;
    int hits = 0;
    for (int r = 0; r < nregs; ++r)
      if (x->r0 == regs[r] || x->r1 == regs[r])
        hits = 1;
    if (!hits)
      continue;
    if (ra_rp_vreg_mentioned(ir, x->vreg))
      return 1;
  }
  return 0;
}

/* Which producers may have their destination register changed.  This is an
 * ALLOW list, not a deny list: an op earns a place only when its destination
 * is a plain register write the encoder is free to direct anywhere.  A fixed
 * destination (a call's r0, UMULL's pair, the division helpers), a
 * two-address one (BFI presets its dest to the host word) and one the encoder
 * also reads (the postinc forms write back through their base, LDRD against
 * its own base is unpredictable) all stay off it. */
static int ra_rp_def_is_retargetable(int op)
{
  switch (op)
  {
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
  case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_ZEXT:
  case TCCIR_OP_SETIF:
  case TCCIR_OP_ASSIGN:
    return 1;
  default:
    return 0;
  }
}

/* Retarget the producer of a dying copy source, and drop the copy.
 *
 *     ubfx  r9, r1, #20, #11        ubfx  sl, r1, #20, #11
 *     mov   sl, r9              ->
 *
 * This is the backward sibling of ra_copy_propagate.  Copy propagation
 * rewrites the reads of a copy's DESTINATION back to its source, so it needs
 * the source to still be the occupant of its register across every one of
 * them -- a long-lived destination therefore rules it out.  Retargeting goes
 * the other way: the producer writes the destination's register directly, the
 * copy disappears, and whatever reads of the source remain are rewritten to
 * the destination, which holds the identical value from the producer onwards.
 *
 * Nothing here depends on how long the destination lives.  What it costs is
 * that the destination is born one instruction earlier, so its register has
 * to be free over that one instruction -- and, when the source outlives the
 * copy, that the source's last read still falls inside the destination's own
 * live range, so no range grows at the far end either.
 *
 * The move coalescer's forward direction covers the same shape by giving the
 * destination the SOURCE's register, but only when that register is free
 * across the destination's whole live range, which for a value computed early
 * and used late it usually is not.  Retargeting asks for far less: the
 * destination's register must be free over the single instruction the value's
 * definition moves back by.
 *
 * Every candidate is checked against the whole function rather than against
 * the live intervals alone, so an interval that over-approximates cannot make
 * a read of the source disappear.
 */
static int ra_retarget_producer(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;
  const int n = ir->next_instruction_index;
  int retargeted = 0;

  if (TCC_OPT(tcc_state, optimize) < 1)
    return 0;
  if (!ls->live_regs_by_instruction || ls->live_regs_by_instruction_size <= 0)
    return 0;
  if (tcc_ir_opt_pass_disabled("ra:retarget_producer"))
    return 0;
  const int tbl_size = ls->live_regs_by_instruction_size;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;
    /* Control may reach the copy without passing the producer, in which case
     * the copy is the only thing establishing the destination on that path. */
    if (q->is_jump_target)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (dest.is_lval || src.is_lval)
      continue;
    if (irop_get_btype(dest) != irop_get_btype(src))
      continue;

    int32_t dv = irop_get_vreg(dest);
    int32_t sv = irop_get_vreg(src);
    if (dv < 0 || sv < 0 || dv == sv)
      continue;
    if (!tcc_ir_vreg_is_valid(ir, dv) || !tcc_ir_vreg_is_valid(ir, sv))
      continue;
    /* Both ends must be temps.  A VAR has a home beyond its register, and the
     * source's disappears entirely here. */
    if (TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP ||
        TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* The producer is the instruction before the copy; NOPs left by earlier
     * passes do not count as instructions, but a NOP that is a jump target
     * still lets control in between the two. */
    int d = -1;
    for (int k = i - 1; k >= 0; --k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op != TCCIR_OP_NOP) { d = k; break; }
      if (qk->is_jump_target) break;
    }
    if (d < 0)
      continue;
    IRQuadCompact *pq = &ir->compact_instructions[d];
    if (!ra_rp_def_is_retargetable(pq->op) || !irop_config[pq->op].has_dest)
      continue;
    IROperand pd = tcc_ir_op_get_dest(ir, pq);
    if (pd.is_lval || irop_get_vreg(pd) != sv)
      continue;
    /* The width the encoder lowers by is the DESTINATION's, and passes that
     * narrow a value retype operands in place -- so the producer's own view of
     * its destination must be the width the copy moves. */
    if (irop_get_btype(pd) != irop_get_btype(dest))
      continue;

    /* Exactly one interval per endpoint: a split vreg holds a different
     * register in each half, and the reasoning below reads one pair of
     * bounds. */
    LSLiveInterval *src_iv = NULL, *dst_iv = NULL;
    int src_ivs = 0, dst_ivs = 0;
    for (int j = 0; j < ls->next_interval_index; ++j)
    {
      if (ls->intervals[j].vreg == (uint32_t)sv) { src_iv = &ls->intervals[j]; src_ivs++; }
      if (ls->intervals[j].vreg == (uint32_t)dv) { dst_iv = &ls->intervals[j]; dst_ivs++; }
    }
    if (!src_iv || !dst_iv || src_ivs != 1 || dst_ivs != 1)
      continue;
    /* PREG_NONE/PREG_SPILLED are >= 0 but are not registers.  Both ends must
     * be register-resident: a spilled destination would turn the producer's
     * write into a spill store, which is not what this is measuring. */
    if (src_iv->r0 < 0 || src_iv->r0 >= PREG_NONE || src_iv->stack_location != 0)
      continue;
    if (dst_iv->r0 < 0 || dst_iv->r0 >= PREG_NONE || dst_iv->stack_location != 0)
      continue;
    if (src_iv->reg_type != dst_iv->reg_type)
      continue;
    /* A 64-bit class means a register PAIR; handing the encoder a destination
     * that owns only its low half aborts it (r1 == 31). */
    {
      int src_pair = (src_iv->r1 >= 0 && src_iv->r1 < PREG_NONE);
      int dst_pair = (dst_iv->r1 >= 0 && dst_iv->r1 < PREG_NONE);
      if (src_pair != dst_pair)
        continue;
    }
    if (src_iv->addrtaken || dst_iv->addrtaken)
      continue;
    /* A graph-coalesced interval shares one register with its whole class. */
    if (src_iv->co_member || dst_iv->co_member)
      continue;
    /* Already the same register: codegen elides the move, nothing to remove. */
    if (src_iv->r0 == dst_iv->r0 && src_iv->r1 == dst_iv->r1)
      continue;

    /* Codegen resolves an operand through the IR-level interval's allocation,
     * not through the LS one, and the two are not interchangeable. */
    {
      IRLiveInterval *src_li = tcc_ir_vreg_live_interval(ir, sv);
      IRLiveInterval *dst_li = tcc_ir_vreg_live_interval(ir, dv);
      if (!src_li || !dst_li)
        continue;
      /* Retargeting moves the producer's write off src's register.  A
       * phi-pinned src shares that register with an identity-elided phi
       * partner whose reads outlive src's interval — the write must stay.
       * (Mirror of the ra_copy_propagate guard.) */
      if (src_li->phi_pinned)
        continue;
      if (dst_li->allocation.offset != 0)
        continue;
      if (ra_alloc_half_spilled(dst_li->allocation.r0) ||
          ra_alloc_half_spilled(dst_li->allocation.r1))
        continue;
      if (ra_alloc_half_unset(dst_li->allocation.r0))
        continue;
      if (ra_alloc_half_unset(src_li->allocation.r1) !=
          ra_alloc_half_unset(dst_li->allocation.r1))
        continue;
    }

    /* The source is born at the producer, the destination at the copy, and
     * the source does not outlive the destination -- the value ends up in the
     * destination's register, which is guaranteed to be the destination's own
     * only up to where the allocator said it ends. */
    if (src_iv->start != (uint32_t)d)
      continue;
    /* ...or, extended back over a loop it is read after, it already starts
     * at or before the producer: its register is its own there too, any read
     * of an older value at or above the producer happens before the write,
     * and the destination is still written only at the copy (checked below).
     * A rotated loop's induction step `t2 = t1 + 1; t3 = t2` is that shape
     * whenever t3 is also read after the loop. */
    if (dst_iv->start != (uint32_t)i)
    {
      if (dst_iv->start > (uint32_t)d)
        continue;
      int other_def = 0;
      for (int k = 0; k < n && !other_def; ++k)
        other_def = k != i && ir->compact_instructions[k].op != TCCIR_OP_NOP &&
                    irop_config[ir->compact_instructions[k].op].has_dest &&
                    tcc_ir_op_dest_vreg(ir, &ir->compact_instructions[k]) == dv;
      if (other_def)
        continue;
    }
    if (src_iv->end > dst_iv->end)
      continue;

    /* The destination's registers must be free from the producer to the last
     * read that will be redirected onto them.  Below the copy that window is
     * inside the destination's own range and the check is a formality; above
     * it, it is what makes moving the definition back safe -- and it is also
     * what keeps the producer's operands out of the way, since a source of
     * the producer is live at the producer, and a multi-instruction lowering
     * need not read all of its operands before writing its first half. */
    {
      uint32_t hi = src_iv->end > (uint32_t)i ? src_iv->end : (uint32_t)i;
      if (ra_rp_dst_regs_busy(ir, ls, dst_iv, (uint32_t)d, hi))
        continue;
    }

    /* Now against the instruction stream rather than the intervals, so that
     * an interval which over-approximates cannot hide a mention.  Outside the
     * producer and the copy, the source may only be READ, only below the copy
     * and inside the destination's range, and only by an op whose operands are
     * not pinned.  A mention in a DEST operand is refused whatever it means --
     * a redefinition invalidates the rewrite, and a store THROUGH the source
     * is a read this pass does not rewrite.  The destination, in turn, must
     * not be written anywhere but the copy. */
    int ok = 1;
    int last_use = i;
    for (int k = 0; k < n; ++k)
    {
      if (k == d || k == i)
        continue;
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP)
        continue;
      /* Control must not be able to enter at the copy: on such an edge the
       * copy is the only thing that establishes the destination, and the
       * producer that would replace it never ran.  `is_jump_target` says this
       * already and is checked above, but it is a bit many passes maintain by
       * hand, so the branches are also read directly.  A computed target is
       * refused outright because it names no index to compare against. */
      if (qk->op == TCCIR_OP_IJUMP || qk->op == TCCIR_OP_SWITCH_TABLE ||
          qk->op == TCCIR_OP_SWITCH_LOAD || qk->op == TCCIR_OP_SETJMP ||
          qk->op == TCCIR_OP_NL_SETJMP)
      { ok = 0; break; }
      if ((qk->op == TCCIR_OP_JUMP || qk->op == TCCIR_OP_JUMPIF) &&
          (int)tcc_ir_op_dest_imm(ir, qk) == i)
      { ok = 0; break; }
      if (irop_config[qk->op].has_dest &&
          tcc_ir_op_dest_vreg(ir, qk) == sv)
      { ok = 0; break; }
      int reads = 0;
      if (irop_config[qk->op].has_src1 &&
          tcc_ir_op_src1_vreg(ir, qk) == sv)
        reads = 1;
      if (!reads && irop_config[qk->op].has_src2 &&
          tcc_ir_op_src2_vreg(ir, qk) == sv)
        reads = 1;
      if (!reads && tcc_ir_op_is_mac(qk->op) &&
          tcc_ir_op_accum_vreg(ir, qk) == sv)
        reads = 1;
      if (!reads)
        continue;
      if (k < i || (uint32_t)k > dst_iv->end || !ra_cp_use_is_redirectable(qk->op))
      { ok = 0; break; }
      if (k > last_use)
        last_use = k;
    }
    if (!ok)
      continue;
    /* The redirected reads must all see the value the producer wrote, so the
     * destination may not be given a new one underneath them.  Only the span
     * that actually carries a redirected read matters: a temp reassigned
     * beyond the last of them is none of this transform's business. */
    for (int k = i + 1; k <= last_use; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[qk->op].has_dest &&
          tcc_ir_op_dest_vreg(ir, qk) == dv)
      { ok = 0; break; }
    }
    if (!ok)
      continue;

    irop_set_vreg(&pd, dv);
    tcc_ir_set_dest(ir, d, pd);
    q->op = TCCIR_OP_NOP;
    for (int k = i + 1; k <= last_use; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[qk->op].has_src1)
      {
        IROperand o = tcc_ir_op_get_src1(ir, qk);
        if (irop_get_vreg(o) == sv)
        { irop_set_vreg(&o, dv); tcc_ir_set_src1(ir, k, o); }
      }
      if (irop_config[qk->op].has_src2)
      {
        IROperand o = tcc_ir_op_get_src2(ir, qk);
        if (irop_get_vreg(o) == sv)
        { irop_set_vreg(&o, dv); tcc_ir_set_src2(ir, k, o); }
      }
    }

    /* The destination is now born at the producer.  The source's interval is
     * left claiming its own registers over [d,i]; nothing writes them any
     * more, but pretending they are still busy only costs later passes an
     * opportunity, where clearing them wrongly would cost correctness. */
    if (dst_iv->start > (uint32_t)d)
      dst_iv->start = (uint32_t)d;
    {
      IRLiveInterval *dst_li = tcc_ir_vreg_live_interval(ir, dv);
      if (dst_li && dst_li->start > (uint32_t)d)
        dst_li->start = (uint32_t)d;
    }
    for (int k = d; k <= i && k < tbl_size; ++k)
    {
      ls->live_regs_by_instruction[k] |= (1u << dst_iv->r0);
      if (dst_iv->r1 >= 0 && dst_iv->r1 < PREG_NONE)
        ls->live_regs_by_instruction[k] |= (1u << dst_iv->r1);
    }
    retargeted++;
    RA_DBG("retarget_producer @%d: %s dest T%d -> T%d (R%d), reads to %d", d,
           tcc_ir_get_op_name((TccIrOp)pq->op), (int)(sv & 0xffffff),
           (int)(dv & 0xffffff), dst_iv->r0, last_use);
  }

  return retargeted;
}

/* ── ra:load_postinc / ra:store_postinc ──
 *
 * `D <- P***DEREF***` immediately followed (in the same straight-line region)
 * by `P <- P + #k` is ARM's post-indexed load, `ldr D,[P],#k`; the mirrored
 * `P***DEREF*** <- V; P <- P + #k` is the post-indexed store, `str V,[P],#k`.
 * The backend has emitted both forms since forever
 * (tcc_gen_machine_load/store_postinc_mop), but until now NOTHING in the
 * pipeline ever produced the ops, so the whole addressing mode was dead code.
 * Every IV-strength-reduced pointer walk paid a separate `adds P,#k`; on the
 * M33 the post-increment is also a cycle cheaper than the separate add (docs,
 * and tcc-m33-addressing-mode-cycle-costs).
 *
 * A source-level `*p++ = v` lowers the increment BEFORE the access
 * (`T <- P + k; P***DEREF*** <- V; P <- T`, or with a copy of P first);
 * ra:bump_sink (regalloc_entry.c) moves the increment below the access before
 * allocation, so it reaches here in one of the forms matched below.  The
 * third form is an increment into a different vreg that the allocator gave
 * P's register.
 *
 * This runs POST-allocation deliberately.  Pre-RA the fused op writes P without
 * the allocator being told (irop_config says the POSTINC ops read P as a USE
 * only), and P is exactly the loop-carried value the allocator most needs to
 * model.  After allocation both registers are already fixed and folding the
 * add changes no allocation at all.
 *
 * The operand layout is the indexed-op one: two value slots, unused, increment
 * at operand_base+3 (codegen reads it via tcc_ir_op_get_scale), so the fused
 * instruction takes four fresh pool slots rather than reusing the original
 * op's two.
 */
static int ra_load_postinc_fuse(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  int fused = 0;

  if (TCC_OPT(tcc_state, optimize) < 1)
    return 0;
  int load_disabled = tcc_ir_opt_pass_disabled("ra:load_postinc");
  int store_disabled = tcc_ir_opt_pass_disabled("ra:store_postinc");
  if (load_disabled && store_disabled)
    return 0;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int is_load = (q->op == TCCIR_OP_LOAD);
    int is_store = (q->op == TCCIR_OP_STORE);
    if (!is_load && !is_store)
      continue;
    if (is_load ? load_disabled : store_disabled)
      continue;

    /* LOAD: `value <- ptr***DEREF***` — ptr is src1, an lvalue.
     * STORE: `ptr***DEREF*** <- value` — ptr is the dest, an lvalue. */
    IROperand ptr = tcc_ir_op_get_dest_or_src1(ir, q, is_load);
    IROperand value = tcc_ir_op_get_dest_or_src1(ir, q, !is_load);

    /* A dereference THROUGH a register: `R4 <- R3***DEREF***`.  is_local /
     * is_llocal / is_sym are the frame-slot and global forms, which name memory
     * some other way and have no pointer register to write back. */
    if (irop_get_tag(ptr) != IROP_TAG_VREG || !ptr.is_lval)
      continue;
    if (ptr.is_local || ptr.is_llocal || ptr.is_sym || ptr.is_complex)
      continue;
    /* No volatility guard is needed: the fusion neither removes, duplicates nor
     * reorders the access.  `ldr D,[P],#k` performs exactly the access the
     * original did, in the same place; only the address arithmetic moves. */
    if (value.is_lval)
      continue;
    /* load_postinc_mop takes the access width from the DEST operand (and
     * store_postinc_mop from the VALUE operand), while a plain LOAD/STORE
     * takes it from the lvalue.  Requiring the two to agree makes the forms
     * equivalent; a widening load (`int c = *bytep`, dest INT32 over an INT8
     * access) would otherwise silently become LDR.  Signedness only matters
     * for loads — a store of width N writes the low N bytes either way.
     * 64-bit is LDRD/STRD-with-writeback and stays out until it has its own
     * test. */
    {
      int vbt = irop_get_btype(value), pbt = irop_get_btype(ptr);
      if (vbt != pbt || (is_load && value.is_unsigned != ptr.is_unsigned))
        continue;
      if (vbt != IROP_BTYPE_INT32 && vbt != IROP_BTYPE_INT16 && vbt != IROP_BTYPE_INT8)
        continue;
    }

    int32_t pv = irop_get_vreg(ptr);
    int32_t dv = irop_get_vreg(value);
    if (pv < 0 || dv < 0 || pv == dv)
      continue;

    /* The pointer must live in a real register: load_postinc_mop writes the
     * incremented address back to the register it found the base in, and never
     * to a spill slot, so a spilled P would silently lose the increment. */
    {
      IRLiveInterval *pli = tcc_ir_vreg_live_interval(ir, pv);
      if (!pli || pli->allocation.offset != 0)
        continue;
      if (ra_alloc_half_unset(pli->allocation.r0) ||
          ra_alloc_half_spilled(pli->allocation.r0))
        continue;
    }

    /* Scan forward for `P <- P + #k`, bailing on anything that would change
     * what the fusion means.  The jump-target check comes BEFORE the NOP
     * skip: a NOPed quad that is still a jump target means a path enters the
     * window that never executed the access, and once the add is folded away
     * that path would leave P un-incremented. */
    int add_idx = -1;
    int assign_idx = -1; /* the `P <- T` half of a renamed split, if any */
    for (int j = i + 1; j < n; ++j)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      /* Leaving the region: an instruction reached from elsewhere may arrive
       * with P un-incremented, and a branch may skip the add entirely. */
      if (jq->is_jump_target)
        break;
      if (jq->op == TCCIR_OP_NOP)
        continue;
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF ||
          jq->op == TCCIR_OP_IJUMP || jq->op == TCCIR_OP_SWITCH_TABLE ||
          jq->op == TCCIR_OP_RETURNVALUE || jq->op == TCCIR_OP_RETURNVOID)
        break;

      if (jq->op == TCCIR_OP_ADD)
      {
        IROperand ad = tcc_ir_op_get_dest(ir, jq);
        if (!ad.is_lval && tcc_ir_op_src1_vreg(ir, jq) == pv && tcc_ir_op_src2_is_imm(ir, jq))
        {
          int64_t k = tcc_ir_op_src2_imm(ir, jq);
          int32_t av = irop_get_vreg(ad);
          if (k >= 1 && k <= 255 && av == pv)
          {
            add_idx = j; /* in-place: P <- P + #k */
          }
          else if (k >= 1 && k <= 255 && av >= 0)
          {
            /* SSA rename splits the in-place bump into `T <- P + #k` followed
             * by `P <- T [ASSIGN]`.  That is still the same increment when T
             * is born at the ADD and dies at the copy — prove it with the
             * allocator's own intervals, then fold and NOP both halves.
             * Between the two halves only non-jump-target NOPs may sit. */
            for (int m = j + 1; m < n; ++m)
            {
              IRQuadCompact *mq = &ir->compact_instructions[m];
              if (mq->is_jump_target)
                break;
              if (mq->op == TCCIR_OP_NOP)
                continue;
              if (mq->op == TCCIR_OP_ASSIGN)
              {
                IROperand cs = tcc_ir_op_get_src1(ir, mq);
                if (!tcc_ir_op_dest_is_lval(ir, mq) && !cs.is_lval && tcc_ir_op_dest_vreg(ir, mq) == pv &&
                    irop_get_vreg(cs) == av)
                {
                  IRLiveInterval *tli = tcc_ir_vreg_live_interval(ir, av);
                  if (tli && tli->start == (uint32_t)j && tli->end == (uint32_t)m)
                  {
                    add_idx = j;
                    assign_idx = m;
                  }
                }
              }
              break; /* first non-NOP decides either way */
            }
            /* The allocator already gave T P's register (the copy back, if
             * any, sits beyond a loop's exit test): T <- P + #k then
             * overwrites P in place, which is the in-place bump.  Two values
             * that differ by k share a register only when P is dead from the
             * ADD on, so nothing later reads the un-incremented P. */
            if (add_idx < 0)
            {
              IRLiveInterval *tli = tcc_ir_vreg_live_interval(ir, av);
              IRLiveInterval *pli = tcc_ir_vreg_live_interval(ir, pv);
              if (tli && pli && tli->allocation.offset == 0 && !ra_alloc_half_unset(tli->allocation.r0) &&
                  !ra_alloc_half_spilled(tli->allocation.r0) && tli->allocation.r0 == pli->allocation.r0 &&
                  irop_get_btype(ad) == IROP_BTYPE_INT32)
                add_idx = j;
            }
          }
        }
        if (add_idx >= 0)
          break;
      }

      /* Any other read or write of P in between would see the wrong value
       * once the increment moves up to the load. */
      {
        int touches = 0;
        for (int k = 0; k < 3 && !touches; ++k)
        {
          IROperand o;
          if (k == 0)
          {
            if (!irop_config[jq->op].has_dest)
              continue;
            o = tcc_ir_op_get_dest(ir, jq);
          }
          else if (k == 1)
          {
            if (!irop_config[jq->op].has_src1)
              continue;
            o = tcc_ir_op_get_src1(ir, jq);
          }
          else
          {
            if (!irop_config[jq->op].has_src2)
              continue;
            o = tcc_ir_op_get_src2(ir, jq);
          }
          if (irop_get_vreg(o) == pv)
            touches = 1;
        }
        if (tcc_ir_op_is_mac(jq->op) && tcc_ir_op_accum_vreg(ir, jq) == pv)
          touches = 1;
        /* A call reads its arguments where it is made, not where their
         * FUNCPARAMVALs sit, and those can lie ABOVE the access: in
         * `printf("%c%d", *p, ext(*p))` the `*p` for printf is a param quad
         * before the load feeding ext, yet it is evaluated at the printf
         * call, below the load.  Walk back to the call's own params (an
         * earlier call with the same id ends them). */
        if (!touches && (jq->op == TCCIR_OP_FUNCCALLVOID || jq->op == TCCIR_OP_FUNCCALLVAL))
        {
          int cid = TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, jq));
          for (int m = j - 1; m >= 0 && !touches; --m)
          {
            IRQuadCompact *mq = &ir->compact_instructions[m];
            if (mq->op == TCCIR_OP_FUNCCALLVOID || mq->op == TCCIR_OP_FUNCCALLVAL)
            {
              if (TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, mq)) == cid)
                break;
              continue;
            }
            if (mq->op != TCCIR_OP_FUNCPARAMVAL ||
                TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, mq)) != cid)
              continue;
            if (tcc_ir_op_src1_vreg(ir, mq) == pv)
              touches = 1;
          }
        }
        if (touches)
          break;
      }
    }
    if (add_idx < 0)
      continue;

    IRQuadCompact *aq = &ir->compact_instructions[add_idx];
    IROperand incr = tcc_ir_op_get_src2(ir, aq);

    /* Four fresh pool slots: dest, ptr, unused, increment (see the header). */
    int nb = ir->iroperand_pool_count;
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    if (nb + 3 >= ir->iroperand_pool_capacity)
      continue;

    q->op = is_load ? TCCIR_OP_LOAD_POSTINC : TCCIR_OP_STORE_POSTINC;
    q->operand_base = (uint32_t)nb;
    /* The backend wants the POINTER VALUE here, not the lvalue it dereferences:
     * mach_ensure_in_reg on an lval operand emits a load OF the pointer from
     * the memory it names, and the writeback then lands in a scratch. */
    IROperand base = ptr;
    base.is_lval = 0;
    /* LOAD_POSTINC decodes {dest=value, src1=base}; STORE_POSTINC decodes
     * {dest=base, src1=value} — the same slots as the plain ops they replace. */
    ir->iroperand_pool[nb + 0] = is_load ? value : base;
    ir->iroperand_pool[nb + 1] = is_load ? base : value;
    ir->iroperand_pool[nb + 2] = IROP_NONE;
    ir->iroperand_pool[nb + 3] = incr;
    aq->op = TCCIR_OP_NOP;
    if (assign_idx >= 0)
      ir->compact_instructions[assign_idx].op = TCCIR_OP_NOP; /* the `P <- T` half */
    fused++;
    RA_DBG("%s_postinc @%d: folded +%d from @%d", is_load ? "load" : "store", i,
           (int)irop_get_imm64_ex(ir, incr), add_idx);
  }

  return fused;
}

int tcc_ir_move_coalescing(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;

  /* Copies whose source outlives them cannot be coalesced by reassignment;
   * propagate those away first so the loop below only sees the rest. */
  int propagated = ra_copy_propagate(ir);
  /* Then the copies whose source is born one instruction earlier, which have a
   * producer to retarget instead of reads to redirect. */
  int retargeted = ra_retarget_producer(ir);
  /* Drop reloads of a slot into the registers that already hold it -- the
   * union punning every soft-float routine opens with. */
  int reloads = ra_redundant_reload_elim(ir);
  /* And then the stores those reloads were the only reader of. */
  reloads += ra_dead_frame_store_elim(ir);

  if (!ls->live_regs_by_instruction || ls->live_regs_by_instruction_size <= 0) {
    if (TCC_LOG_LS)
      LOG_LS("copy_stats coalesced=0 reaching_codegen=%d",
             ra_count_copies_reaching_codegen(ir));
    return 0;
  }

  int coalesced = 0;
  const int n = ir->next_instruction_index;
  const int tbl_size = ls->live_regs_by_instruction_size;

  /* Track vregs already reverse-coalesced to prevent chains where a src
   * gets moved to register A, then a later ASSIGN moves it back to B. */
  uint32_t *rev_done = NULL;
  int rev_done_size = 0;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* LOAD with a VREG source where the underlying interval ended up in a
     * register (no spill) is a register copy at codegen — treat like ASSIGN
     * for coalescing.  Catches inlined "temp = var" patterns the IR
     * generator emits as LOAD even when no memory access is involved.
     * Accept is_lval=1 for local VAR reads as long as the VAR is reg-only;
     * skip is_llocal (true memory load via pointer) and is_sym (global). */
    /* LOAD from a vreg whose interval ended up in a register (no spill)
     * is a register copy at codegen — treat like ASSIGN for coalescing.
     * Catches inlined "temp = var" patterns where the IR generator emits
     * LOAD with VREG/STACKOFF source even though no memory access happens.
     * Skip is_llocal (double-indirection via pointer) and is_sym (global).
     * Skip sub-word btypes: those LOADs emit UXTB/SXTB/UXTH/SXTH alongside
     * the mov to truncate; coalescing them away would skip the narrowing
     * and yield wrong values (see pr69447, fp-cmp-8 sub-word args). */
    int is_copy_load = 0;
    if (q->op == TCCIR_OP_LOAD) {
      IROperand ts = tcc_ir_op_get_src1(ir, q);
      int valid_tag = (ts.tag == IROP_TAG_VREG ||
                       (ts.tag == IROP_TAG_STACKOFF && ts.is_local));
      int dest_bt = tcc_ir_op_dest_btype(ir, q);
      int src_bt = irop_get_btype(ts);
      int width_safe = (dest_bt == src_bt) &&
                       (dest_bt == IROP_BTYPE_INT32 ||
                        dest_bt == IROP_BTYPE_INT64 ||
                        dest_bt == IROP_BTYPE_FUNC);
      if (width_safe && valid_tag && !ts.is_llocal && !ts.is_sym) {
        int32_t tsv = irop_get_vreg(ts);
        if (tsv >= 0 && tcc_ir_vreg_is_valid(ir, tsv)) {
          for (int j = 0; j < ls->next_interval_index; ++j) {
            if (ls->intervals[j].vreg == (uint32_t)tsv) {
              /* Only a source in a REAL register is a register copy.  PREG_NONE
               * (0x1F) is >= 0 but means "not allocated" — e.g. a stack-passed
               * parameter that lives in the caller's frame, not a register.
               * Coalescing the LOAD result into such a source would make it
               * inherit PREG_NONE and be mis-lowered as a spill at frame offset
               * 0 (clobbering the saved frame pointer). */
              if (ls->intervals[j].r0 >= 0 && ls->intervals[j].r0 < PREG_NONE &&
                  ls->intervals[j].stack_location == 0)
                is_copy_load = 1;
              break;
            }
          }
        }
      }
    }
    if (q->op != TCCIR_OP_ASSIGN && !is_copy_load)
      continue;

    const IROperand src1 = tcc_ir_op_get_src1(ir, q);
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    /* For is_copy_load (LOAD from in-register VAR) the src has is_lval=1
     * but it behaves as a register copy — don't skip it on that basis. */
    if ((src1.is_lval && !is_copy_load) || dest.is_lval) continue;
    int32_t sv = irop_get_vreg(src1);
    if (sv < 0 || !tcc_ir_vreg_is_valid(ir, sv))
      continue;
    int32_t dv = irop_get_vreg(dest);
    if (dv < 0 || !tcc_ir_vreg_is_valid(ir, dv))
      continue;

    LSLiveInterval *src_iv = NULL, *dst_iv = NULL;
    for (int j = 0; j < ls->next_interval_index; ++j)
    {
      if (ls->intervals[j].vreg == (uint32_t)sv) src_iv = &ls->intervals[j];
      if (ls->intervals[j].vreg == (uint32_t)dv) dst_iv = &ls->intervals[j];
      if (src_iv && dst_iv) break;
    }
    if (!src_iv || !dst_iv) continue;
    /* Both endpoints must live in a REAL register.  PREG_NONE (0x1F) and
     * PREG_SPILLED (0x20) are >= 0 but are NOT registers (e.g. a stack-passed
     * parameter resident in the caller's frame).  Coalescing onto such an
     * endpoint propagates PREG_NONE into a live value, which is then mis-lowered
     * as a spill at frame offset 0 — clobbering a saved register at [FP,#0]. */
    if (src_iv->r0 < 0 || src_iv->r0 >= PREG_NONE ||
        dst_iv->r0 < 0 || dst_iv->r0 >= PREG_NONE) continue;
    if (src_iv->stack_location != 0 || dst_iv->stack_location != 0) continue;
    if (src_iv->r0 == dst_iv->r0) continue;
    /* Never reassign a graph-coalesced interval: it shares one register with
     * its whole class, and reassigning one member here would split the class
     * (the other members keep the class register), corrupting the value. */
    if (src_iv->co_member || dst_iv->co_member) continue;

    /* Nor a phi-pinned one.  post_ra_forward_diamond / ra_phi_copy_needed
     * delete an identity phi copy precisely BECAUSE its two vregs already sit
     * in one register, and set phi_pinned to say the pairing is now load
     * bearing: no instruction is left that would re-establish it.  Moving
     * either endpoint to a different register here silently un-pairs them, and
     * the merge block reads a register the elided-copy path never wrote.
     * (Same guard as ra_copy_propagate / ra_retarget_producer above; this loop
     * reassigns registers rather than rewriting operands, so it needs it too.) */
    {
      IRLiveInterval *src_ir_iv = tcc_ir_vreg_live_interval(ir, sv);
      IRLiveInterval *dst_ir_iv = tcc_ir_vreg_live_interval(ir, dv);
      if ((src_ir_iv && src_ir_iv->phi_pinned) || (dst_ir_iv && dst_ir_iv->phi_pinned))
        continue;
    }

    /* Forward direction: reassign dest to use src's register.
     * Requires src to die at this ASSIGN. */
    if (src_iv->end == (uint32_t)i) {
      int src_reg = src_iv->r0;
      if (dst_iv->crosses_call && !(src_reg >= 4 && src_reg <= 11))
        goto try_reverse;

      int conflict = 0;
      for (int k = i + 1; k <= (int)dst_iv->end && k < tbl_size; ++k)
      {
        if (ls->live_regs_by_instruction[k] & (1u << src_reg)) {
          /* Two-address relaxation: at k == dst.end the dst's last use is
           * an instruction that consumes dst and writes a fresh result.
           * If that result lands in src_reg (some interval starts at k in
           * src_reg) AND the same instruction reads dst, sharing src_reg
           * is safe — ARM ops read all sources before writing dest, so
           * `OP src_reg, ..., src_reg` is a valid two-operand form. */
          int safe = 0;
          if (k == (int)dst_iv->end) {
            IRQuadCompact *kq = &ir->compact_instructions[k];
            int reads_dst = 0;
            if (irop_config[kq->op].has_src1 &&
                tcc_ir_op_src1_vreg(ir, kq) == dv)
              reads_dst = 1;
            if (!reads_dst && irop_config[kq->op].has_src2 &&
                tcc_ir_op_src2_vreg(ir, kq) == dv)
              reads_dst = 1;
            if (reads_dst) {
              for (int j2 = 0; j2 < ls->next_interval_index; j2++) {
                LSLiveInterval *xi = &ls->intervals[j2];
                if (xi->r0 == src_reg && xi->stack_location == 0 &&
                    xi->start == (uint32_t)k) {
                  safe = 1;
                  break;
                }
              }
            }
          }
          if (!safe) { conflict = 1; break; }
        }
      }
      if (!conflict) {
        for (int k = (int)dst_iv->start; k < i && k < tbl_size; ++k)
        {
          if (ls->live_regs_by_instruction[k] & (1u << src_reg))
          { conflict = 1; break; }
        }
      }
      if (!conflict) {
        int old_reg = dst_iv->r0;
        dst_iv->r0 = src_reg;
        ls->live_sweep_valid = 0;
        uint8_t *held = tcc_ls_reg_held_by_other_range(ls, old_reg, (int)dst_iv->start,
                                                    (int)dst_iv->end < tbl_size ? (int)dst_iv->end : tbl_size - 1, dst_iv);
        for (int k = (int)dst_iv->start; k <= (int)dst_iv->end && k < tbl_size; ++k)
        {
          /* old_reg's bit may be shared with another interval that coalesced
           * onto it earlier (in-place two-address ops overlap on purpose) —
           * only clear positions where no other claimant is still live. */
          if (!held[k - (int)dst_iv->start])
            ls->live_regs_by_instruction[k] &= ~(1u << old_reg);
          ls->live_regs_by_instruction[k] |= (1u << src_reg);
        }
        tcc_free(held);
        RA_DBG("move_coalesce fwd @%d: T%d R%d->R%d [%u,%u]", i,
               (int)(dv & 0xffffff), old_reg, src_reg, dst_iv->start, dst_iv->end);
        coalesced++;
        continue;
      }
    }

    /* Reverse direction: reassign src to use dest's register.
     * Works for loop-carried phi copies where src = f(dest, ...) and
     * dest's register is only occupied by dest during src's range.
     * Safety: src must not be redefined between the ASSIGN and dest's
     * last use, otherwise the shared register would get clobbered. */
try_reverse:;
    /* Skip if this src vreg was already reverse-coalesced */
    {
      int already = 0;
      for (int ri = 0; ri < rev_done_size; ri++) {
        if (rev_done[ri] == (uint32_t)sv) { already = 1; break; }
      }
      if (already) continue;
    }
    int dest_reg = dst_iv->r0;
    if (src_iv->crosses_call && !(dest_reg >= 4 && dest_reg <= 11))
      continue;

    int conflict = 0;

    /* Conservative: src must be defined directly FROM dest (reads dest
     * as src1), like `src = dest + 1` or `src = dest * x + acc`.
     * This guarantees ARM's read-before-write makes the in-place
     * operation correct. */
    {
      int def_idx = (int)src_iv->start;
      if (def_idx < 0 || def_idx >= n) { conflict = 1; goto rev_check_done; }
      IRQuadCompact *qdef = &ir->compact_instructions[def_idx];
      if (!irop_config[qdef->op].has_src1) { conflict = 1; goto rev_check_done; }
      if (tcc_ir_op_src1_vreg(ir, qdef) != dv) { conflict = 1; goto rev_check_done; }
    }

    /* Check src is not redefined while dest is still live */
    for (int k = i + 1; k <= (int)dst_iv->end && k < n; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP) continue;
      if (irop_config[qk->op].has_dest) {
        IROperand dk = tcc_ir_op_get_dest(ir, qk);
        int is_mem_store = (qk->op == TCCIR_OP_STORE || qk->op == TCCIR_OP_STORE_INDEXED ||
                            qk->op == TCCIR_OP_STORE_POSTINC) && dk.is_lval;
        if (!is_mem_store) {
          int32_t dkvr = irop_get_vreg(dk);
          if (dkvr == sv) { conflict = 1; break; }
        }
      }
    }
    if (conflict) goto rev_check_done;

    /* Symmetric guard (dest side): after this copy src and dest share
     * dest_reg holding the same value.  If dest is given a NEW, independent
     * value while src is still live, that write clobbers dest_reg and src's
     * remaining uses read the wrong value.  The loop-carried phi copy this
     * pass targets has src dying at the copy (src_iv->end == i), so the range
     * below is empty and legitimate coalescing is unaffected; the guard only
     * fires when src OUTLIVES the copy and dest is re-defined underneath it
     * (bitfield 40979: `u4 = u3` copy, then `u4 = const` clobbers the shared
     * register while `u3` is still read).  A redefinition at exactly src's
     * last use that also reads src is the two-address read-before-write case
     * and stays safe. */
    for (int k = i + 1; k <= (int)src_iv->end && k < n; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP) continue;
      if (!irop_config[qk->op].has_dest) continue;
      IROperand dk = tcc_ir_op_get_dest(ir, qk);
      int is_mem_store = (qk->op == TCCIR_OP_STORE || qk->op == TCCIR_OP_STORE_INDEXED ||
                          qk->op == TCCIR_OP_STORE_POSTINC) && dk.is_lval;
      if (is_mem_store) continue;
      if (irop_get_vreg(dk) != dv) continue;
      if (k == (int)src_iv->end) {
        int reads_src = 0;
        if (irop_config[qk->op].has_src1 &&
            tcc_ir_op_src1_vreg(ir, qk) == sv) reads_src = 1;
        if (!reads_src && irop_config[qk->op].has_src2 &&
            tcc_ir_op_src2_vreg(ir, qk) == sv) reads_src = 1;
        if (!reads_src && tcc_ir_op_is_mac(qk->op) &&
            tcc_ir_op_accum_vreg(ir, qk) == sv) reads_src = 1;
        if (reads_src) continue;
      }
      conflict = 1;
      break;
    }
    if (conflict) goto rev_check_done;

    /* Check dest not used between src's def and the ASSIGN.
     * src's def overwrites dest_reg; any intervening use of dest
     * would read the wrong value. */
    for (int k = (int)src_iv->start + 1; k < i && k < n; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP) continue;
      if (irop_config[qk->op].has_src1) {
        if (tcc_ir_op_src1_vreg(ir, qk) == dv) { conflict = 1; break; }
      }
      if (!conflict && irop_config[qk->op].has_src2) {
        if (tcc_ir_op_src2_vreg(ir, qk) == dv) { conflict = 1; break; }
      }
      if (!conflict && irop_config[qk->op].has_dest) {
        if (tcc_ir_op_dest_is_lval(ir, qk) && tcc_ir_op_dest_vreg(ir, qk) == dv) { conflict = 1; break; }
      }
      if (!conflict && tcc_ir_op_is_mac(qk->op)) {
        if (tcc_ir_op_accum_vreg(ir, qk) == dv) { conflict = 1; break; }
      }
    }
    if (conflict) goto rev_check_done;

    /* Check no control-flow escape between src's def and the ASSIGN.
     * src's def overwrites dest_reg; the ASSIGN re-establishes dest's value
     * only on the path that reaches it.  A JUMP/JUMPIF in (def, ASSIGN) that
     * targets outside [def, ASSIGN] lets control reach later uses of dest
     * with dest_reg clobbered and the restoring copy skipped — e.g. a
     * top-tested pointer-chase loop (`while (p->next) p = p->next;`) whose
     * exit edge branches past the back-edge copy while `p` is still live. */
    for (int k = (int)src_iv->start; k < i && k < n; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP) continue;
      if (qk->op == TCCIR_OP_IJUMP || qk->op == TCCIR_OP_SWITCH_TABLE ||
          qk->op == TCCIR_OP_SWITCH_LOAD) { conflict = 1; break; }
      if (qk->op == TCCIR_OP_JUMP || qk->op == TCCIR_OP_JUMPIF) {
        int jt = (int)tcc_ir_op_dest_imm(ir, qk);
        if (jt < (int)src_iv->start || jt > i) { conflict = 1; break; }
      }
    }
rev_check_done:
    if (conflict) continue;

    /* Check dest_reg not occupied by other intervals during src's range.
     * Identity-based: earlier coalesces may have moved a third interval onto
     * dest_reg inside dst_iv's range, so "position within dst_iv's range" is
     * not proof the claim is dst_iv's own. */
    for (int k = (int)src_iv->start; k <= (int)src_iv->end && k < tbl_size; ++k)
    {
      if (ls->live_regs_by_instruction[k] & (1u << dest_reg))
      {
        if (tcc_ls_reg_held_by_other(ls, dest_reg, k, dst_iv))
        { conflict = 1; break; }
        /* dest_reg is live here — only OK if it's from dest_iv itself */
        if (k < (int)dst_iv->start || k > (int)dst_iv->end)
        { conflict = 1; break; }
      }
    }
    if (conflict) continue;

    int old_reg = src_iv->r0;
    src_iv->r0 = dest_reg;
    ls->live_sweep_valid = 0;
    uint8_t *held = tcc_ls_reg_held_by_other_range(ls, old_reg, (int)src_iv->start,
                                                    (int)src_iv->end < tbl_size ? (int)src_iv->end : tbl_size - 1, src_iv);
    for (int k = (int)src_iv->start; k <= (int)src_iv->end && k < tbl_size; ++k)
    {
      /* old_reg's bit may be shared with another interval that coalesced
       * onto it earlier — only clear positions with no other live claimant
       * (volatile 36818: T175 leaving R5 wiped T212's in-place-XOR claim,
       * and the phase-3 scratch fixup then put the outer loop counter there). */
      if (!held[k - (int)src_iv->start])
        ls->live_regs_by_instruction[k] &= ~(1u << old_reg);
      ls->live_regs_by_instruction[k] |= (1u << dest_reg);
    }
    tcc_free(held);
    RA_DBG("move_coalesce rev @%d: T%d R%d->R%d [%u,%u]", i,
           (int)(sv & 0xffffff), old_reg, dest_reg, src_iv->start, src_iv->end);
    /* Record this src vreg as reverse-coalesced */
    rev_done = tcc_realloc(rev_done, sizeof(uint32_t) * (rev_done_size + 1));
    rev_done[rev_done_size++] = (uint32_t)sv;
    coalesced++;
  }

  if (rev_done)
    tcc_free(rev_done);

  if (coalesced > 0)
    tcc_ls_recompute_dirty_registers(ls);

  /* Last: the IR is in its final shape, so an increment folded into a load
   * cannot be moved apart again. */
  int postinc = ra_load_postinc_fuse(ir);

  if (TCC_LOG_LS)
    LOG_LS("copy_stats propagated=%d reloads=%d coalesced=%d retargeted=%d reaching_codegen=%d",
           propagated, reloads, coalesced, retargeted,
           ra_count_copies_reaching_codegen(ir));

  return coalesced + propagated + reloads + retargeted + postinc;
}
