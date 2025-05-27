/*
 *  TCC IR - Dead-VLA-Struct Elimination
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* GCC -O2 eliminates the entire dynamic-stack-alloc dance for a VLA struct
 * whose only write is into a never-read field — the address never escapes,
 * and the bytes are never read, so the alloc + store + restore are all dead.
 * Pattern from gcc.c-torture/execute/20040308-1.c:
 *
 *   void foo(int n) {
 *     struct S { int i[n]; unsigned int b:1; int i2; }
 *       __attribute__((packed)) __attribute__((aligned(4)));
 *     struct S s;
 *     s.i2 = 0;
 *   }
 *
 * TCC emits:
 *
 *   VLA_SP_SAVE     StackLoc[outer]           <- save SP for restore
 *   T_sz = ... size + slack ...
 *   VLA_ALLOC       T_sz, #align              <- SP -= aligned size
 *   VLA_SP_SAVE     StackLoc[base]            <- save VLA base
 *   ... compute offset T_off ...
 *   T_addr = StackLoc[base] ADD T_off
 *   T_addr***DEREF*** <-- val  [STORE]
 *   VLA_SP_RESTORE  StackLoc[outer]
 *
 * If the only readers of StackLoc[base] are address-arithmetic ops that end
 * in STORE destinations (no LOAD via the derived address, no escape via
 * CALL / STORE-as-value / RETURN / CMP), the VLA's contents are observably
 * dead.  We NOP the VLA_ALLOC, the inner VLA_SP_SAVE, every tainted
 * propagation, and every STORE through a tainted TEMP — leaving the outer
 * SAVE/RESTORE pair surrounding no SP-changing op, which the existing
 * `tcc_ir_opt_zero_vla_elim` cleans up in the same late-cleanup round.
 *
 * Conservative bails: function contains IJUMP / SETJMP / LONGJMP /
 * NL_SETJMP / NL_LONGJMP / INLINE_ASM, has captured locals or a nested-
 * function static chain, or the slot has a second writer.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"

static int op_is_address_propagator(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    return 1;
  default:
    return 0;
  }
}

/* True if `op` reads `slot` as a value (StackLoc[slot] used as a memory
 * load: lval=1, no vreg). */
static int operand_reads_slot(IROperand op, int32_t slot)
{
  if (!op.is_lval)
    return 0;
  if (irop_get_tag(op) != IROP_TAG_STACKOFF)
    return 0;
  if (!op.is_local)
    return 0;
  if (irop_get_vreg(op) != -1)
    return 0;
  return irop_get_stack_offset(op) == slot;
}

/* True if `op` is a non-lval TEMP at position `pos`. */
static int operand_is_temp(IROperand op, int *out_pos)
{
  if (op.is_lval)
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  *out_pos = TCCIR_DECODE_VREG_POSITION(vr);
  return 1;
}

/* True if `op` is an lval-deref of TEMP at `pos` (e.g. T***DEREF*** in a STORE
 * destination). */
static int operand_is_temp_lval(IROperand op, int *out_pos)
{
  if (!op.is_lval)
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  *out_pos = TCCIR_DECODE_VREG_POSITION(vr);
  return 1;
}

/* Walk forward from `vla_idx` to verify and gather elimination targets for
 * a single dead-VLA pattern rooted at the VLA_ALLOC at `vla_idx`.
 *
 * Returns 1 (and fills *out_save_idx, *out_kill_*) if the pattern is dead
 * and safe to eliminate.  Returns 0 otherwise.
 *
 * Caller is responsible for allocating tainted[max_tmp+1], kill_idx (capacity
 * at least n), and reading the resulting kill_count.
 */
static int analyze_dead_vla(TCCIRState *ir, int vla_idx, int max_tmp,
                            uint8_t *tainted, int *kill_idx, int *kill_count,
                            int *out_save_idx)
{
  int n = ir->next_instruction_index;
  *kill_count = 0;
  *out_save_idx = -1;
  memset(tainted, 0, max_tmp + 1);

  /* Find the inner VLA_SP_SAVE that immediately follows the VLA_ALLOC
   * (skipping NOPs).  The TCC frontend always emits this pair contiguously
   * for VLA-struct and `int a[n]` patterns. */
  int save_idx = -1;
  for (int j = vla_idx + 1; j < n; j++)
  {
    TccIrOp op = ir->compact_instructions[j].op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_VLA_SP_SAVE)
      save_idx = j;
    break;
  }
  if (save_idx < 0)
    return 0;

  IROperand save_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[save_idx]);
  if (irop_get_tag(save_dest) != IROP_TAG_STACKOFF || !save_dest.is_local ||
      irop_get_vreg(save_dest) != -1)
    return 0;
  int32_t slot = irop_get_stack_offset(save_dest);

  /* The slot must be written only by this VLA_SP_SAVE — any STORE / second
   * VLA_SP_SAVE to the same slot means the value can change later and our
   * single-source taint reasoning would be wrong. */
  for (int j = 0; j < n; j++)
  {
    if (j == save_idx)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval && irop_get_tag(d) == IROP_TAG_STACKOFF && d.is_local &&
        irop_get_vreg(d) == -1 && irop_get_stack_offset(d) == slot)
      return 0;
    if (q->op == TCCIR_OP_VLA_SP_SAVE && irop_get_tag(d) == IROP_TAG_STACKOFF &&
        d.is_local && irop_get_vreg(d) == -1 && irop_get_stack_offset(d) == slot)
      return 0;
  }

  /* Walk forward from save_idx+1 to the end.  For each op, classify any
   * read of `slot` or use of a tainted TEMP as either propagation
   * (produces a new tainted TEMP), STORE through tainted (kill candidate),
   * or untame escape (bail). */
  for (int j = save_idx + 1; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* A VLA_SP_RESTORE that reads our slot would mean someone is using
     * `slot` as a saved SP — we identified `slot` as the VLA-base capture,
     * not the outer save, so this shouldn't happen.  Bail defensively. */
    if (q->op == TCCIR_OP_VLA_SP_RESTORE)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (operand_reads_slot(s1, slot))
        return 0;
      continue;
    }

    int has_d = irop_config[q->op].has_dest;
    int has_s1 = irop_config[q->op].has_src1;
    int has_s2 = irop_config[q->op].has_src2;
    IROperand d = {0}, s1 = {0}, s2 = {0};
    if (has_d) d = tcc_ir_op_get_dest(ir, q);
    if (has_s1) s1 = tcc_ir_op_get_src1(ir, q);
    if (has_s2) s2 = tcc_ir_op_get_src2(ir, q);

    /* Does this op consume a tainted value (either by reading the slot
     * directly, or by reading a tainted TEMP)? */
    int reads_slot = 0;
    int reads_tainted = 0;
    int tpos;
    if (has_s1)
    {
      if (operand_reads_slot(s1, slot))
        reads_slot = 1;
      else if (operand_is_temp(s1, &tpos) && tpos <= max_tmp && tainted[tpos])
        reads_tainted = 1;
    }
    if (has_s2)
    {
      if (operand_reads_slot(s2, slot))
        reads_slot = 1;
      else if (operand_is_temp(s2, &tpos) && tpos <= max_tmp && tainted[tpos])
        reads_tainted = 1;
    }

    /* STORE: dest is the deref-target (an lval).  If the deref is a tainted
     * TEMP, this is a write through a derived VLA address — a kill candidate.
     * The stored value (src1) must NOT be a tainted address (escape). */
    if (q->op == TCCIR_OP_STORE)
    {
      int dpos;
      int dest_is_tainted = has_d && operand_is_temp_lval(d, &dpos) &&
                            dpos <= max_tmp && tainted[dpos];
      /* If src1 (value) is a tainted address or reads the slot, the VLA
       * pointer is being stored to memory — escape.  Bail. */
      if (has_s1)
      {
        if (operand_reads_slot(s1, slot))
          return 0;
        if (operand_is_temp(s1, &tpos) && tpos <= max_tmp && tainted[tpos])
          return 0;
      }
      if (dest_is_tainted)
      {
        kill_idx[(*kill_count)++] = j;
        continue;
      }
      /* STORE through some unrelated dest; harmless. */
      continue;
    }

    /* Memory READ through a tainted TEMP (e.g. LOAD T_addr***DEREF***): a
     * caller depends on the bytes we'd be eliminating — bail.  Also catches
     * CMP T_addr***DEREF***,imm (pr82210). */
    int deref_pos;
    if (has_s1 && operand_is_temp_lval(s1, &deref_pos) &&
        deref_pos <= max_tmp && tainted[deref_pos])
      return 0;
    if (has_s2 && operand_is_temp_lval(s2, &deref_pos) &&
        deref_pos <= max_tmp && tainted[deref_pos])
      return 0;

    if (!reads_slot && !reads_tainted)
      continue; /* This op doesn't touch the tracked value. */

    /* Reads the tracked value — must be a tame propagator with a TEMP dest
     * we can taint, otherwise the address escapes into an untracked op. */
    if (!op_is_address_propagator(q->op))
      return 0;
    if (!has_d)
      return 0;
    int dpos;
    if (!operand_is_temp(d, &dpos))
      return 0;
    if (dpos > max_tmp)
      return 0;

    /* Propagate taint and queue the def for elimination. */
    tainted[dpos] = 1;
    kill_idx[(*kill_count)++] = j;
  }

  *out_save_idx = save_idx;
  return 1;
}

/* After the main analysis NOPs the VLA + STORE chain, walk the function and
 * repeatedly NOP any pure-arithmetic TEMP def whose result is no longer used.
 * This drains the offset-computation chain (T2 = ...; T3 = T2 & ~3; ... T7
 * = base + offset) once the consumer at the tail is NOPed.  Limited to ops
 * that are guaranteed side-effect-free so we don't accidentally drop e.g. a
 * LOAD from volatile memory. */
static int op_is_side_effect_free_tmp_def(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_MUL:
  case TCCIR_OP_ROR:
  case TCCIR_OP_ZEXT:
    return 1;
  default:
    return 0;
  }
}

static int sweep_orphan_tmp_defs(TCCIRState *ir, int max_tmp)
{
  int n = ir->next_instruction_index;
  int total = 0;

  /* use_count[pos] = number of live reads of TEMP at position pos. */
  int *use_count = tcc_mallocz(sizeof(int) * (max_tmp + 1));
  int changed = 1;
  while (changed)
  {
    changed = 0;
    memset(use_count, 0, sizeof(int) * (max_tmp + 1));

    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(s);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_tmp)
            use_count[p]++;
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        int32_t vr = irop_get_vreg(s);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_tmp)
            use_count[p]++;
        }
      }
      /* STORE deref-dest with TEMP vreg is a use of that TEMP (the address). */
      if (q->op == TCCIR_OP_STORE && irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (d.is_lval)
        {
          int32_t vr = irop_get_vreg(d);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
          {
            int p = TCCIR_DECODE_VREG_POSITION(vr);
            if (p <= max_tmp)
              use_count[p]++;
          }
        }
      }
    }

    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (!op_is_side_effect_free_tmp_def(q->op))
        continue;
      if (!irop_config[q->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (d.is_lval)
        continue;
      int32_t vr = irop_get_vreg(d);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p > max_tmp)
        continue;
      if (use_count[p] != 0)
        continue;
      q->op = TCCIR_OP_NOP;
      changed = 1;
      total++;
    }
  }

  tcc_free(use_count);
  return total;
}

int tcc_ir_opt_dead_vla_struct_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Nested-function entanglements break our escape reasoning:
   *  - captured_count > 0 / has_static_chain: this function reads parent
   *    locals through the static chain — the slot could mirror one of them.
   *  - nb_nested_funcs > 0: this function has child closures that capture
   *    OUR locals; a VLA's address can escape into a nested function
   *    invisibly (no FUNCCALL operand here). */
  if (ir->captured_count > 0 || ir->has_static_chain ||
      ir->nb_nested_funcs > 0)
    return 0;

  /* Bail on opcodes whose memory effects we don't model. */
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_NL_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM)
      return 0;
  }

  /* Find max temp position to size the taint bitmap. */
  int max_tmp = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_tmp)
      max_tmp = pos;
  }

  uint8_t *tainted = tcc_malloc(max_tmp + 1);
  int *kill_idx = tcc_malloc(sizeof(int) * n);

  int total_changes = 0;
  int any_dead_vla = 0;
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_VLA_ALLOC)
      continue;
    int save_idx = -1;
    int kill_count = 0;
    if (!analyze_dead_vla(ir, i, max_tmp, tainted, kill_idx, &kill_count,
                          &save_idx))
      continue;

    LOG_IR_GEN("DEAD-VLA-STRUCT: NOP VLA_ALLOC@%d + SP_SAVE@%d + %d "
               "dependent ops (slot=%d)",
               i, save_idx,
               kill_count,
               irop_get_stack_offset(tcc_ir_op_get_dest(
                   ir, &ir->compact_instructions[save_idx])));
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[save_idx].op = TCCIR_OP_NOP;
    for (int k = 0; k < kill_count; k++)
      ir->compact_instructions[kill_idx[k]].op = TCCIR_OP_NOP;
    total_changes += 2 + kill_count;
    any_dead_vla = 1;
  }

  tcc_free(kill_idx);
  tcc_free(tainted);

  /* Cascade: drain the upstream offset-computation chain whose tail
   * consumer we just NOPed. */
  if (any_dead_vla)
    total_changes += sweep_orphan_tmp_defs(ir, max_tmp);

  /* If every VLA_ALLOC and BUILTIN_APPLY in the function has been
   * eliminated, the `force_frame_pointer` flag set by the parser when it
   * saw the VLA / alloca / builtin_apply construct is now spurious — clear
   * it so the prologue doesn't emit a push/r7 / sub-sp dance for an
   * observationally-empty body.  Be defensive: only clear when we actually
   * made changes (avoid touching unrelated functions). */
  if (any_dead_vla)
  {
    int has_vla_or_apply = 0;
    for (int i = 0; i < n; i++)
    {
      int op = ir->compact_instructions[i].op;
      if (op == TCCIR_OP_VLA_ALLOC || op == TCCIR_OP_BUILTIN_APPLY_ARGS ||
          op == TCCIR_OP_BUILTIN_APPLY || op == TCCIR_OP_SET_CHAIN)
      {
        has_vla_or_apply = 1;
        break;
      }
    }
    if (!has_vla_or_apply && tcc_state)
    {
      tcc_state->force_frame_pointer = 0;
      tcc_state->need_frame_pointer = 0;
    }
  }

  return total_changes;
}

int tcc_ir_opt_dead_vla_struct_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_vla_struct_elim(ctx->ir);
}

/* alloca-load forwarding
 *
 * The TCC frontend lowers `__builtin_alloca(N)` (and `n = alloca(N)` patterns)
 * to a three-op sequence:
 *
 *   VLA_ALLOC #N, #align          ; adjusts SP
 *   VLA_SP_SAVE -> StackLoc[S]    ; spills the new SP to slot S
 *   LOAD vreg <- StackLoc[S]      ; reads the alloca pointer back
 *
 * which lowers to `mov scratch, sp; str scratch, [S]; ldr vreg, [S]` — 3
 * machine instructions even though `mov vreg, sp` is what we really want.
 *
 * When the slot is otherwise dead (no second writer, no other readers, no
 * VLA_SP_RESTORE) and the LOAD is the *immediately* next non-NOP op, we
 * retarget the VLA_SP_SAVE's destination to the LOAD's vreg and NOP the
 * LOAD.  The backend's VLA_SP_SAVE handler recognises the REG dest and
 * emits a single `mov dest_reg, sp`, collapsing the three-op dance to one
 * instruction. */
int tcc_ir_opt_alloca_load_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *save = &ir->compact_instructions[i];
    if (save->op != TCCIR_OP_VLA_SP_SAVE)
      continue;

    IROperand save_dest = tcc_ir_op_get_dest(ir, save);
    if (irop_get_tag(save_dest) != IROP_TAG_STACKOFF || !save_dest.is_local ||
        irop_get_vreg(save_dest) != -1)
      continue;
    int32_t slot = irop_get_stack_offset(save_dest);

    /* Find immediately-next non-NOP instruction. */
    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      continue;

    IRQuadCompact *ld = &ir->compact_instructions[j];
    if (ld->op != TCCIR_OP_LOAD)
      continue;
    if (ld->is_jump_target)
      continue;

    IROperand ld_src = tcc_ir_op_get_src1(ir, ld);
    IROperand ld_dest = tcc_ir_op_get_dest(ir, ld);

    /* LOAD must read exactly the slot we just wrote. */
    if (irop_get_tag(ld_src) != IROP_TAG_STACKOFF || !ld_src.is_local ||
        irop_get_vreg(ld_src) != -1 || irop_get_stack_offset(ld_src) != slot)
      continue;
    /* LOAD must not dereference through an intermediate pointer (llocal). */
    if (ld_src.is_llocal)
      continue;
    /* LOAD's btype must match a 32-bit pointer-sized value — VLA_SP_SAVE
     * stores SP, which is always 32 bits on this target.  Skip 64-bit pairs
     * and sub-word loads which would require sign/zero-extension. */
    if (irop_needs_pair(ld_dest))
      continue;
    if (ld_dest.btype != IROP_BTYPE_INT32 && ld_dest.btype != 0)
      continue;

    /* LOAD's dest must be a plain vreg (TEMP or VAR) — not a deref/spill
     * target that the backend would still spill to memory. */
    if (irop_get_tag(ld_dest) != IROP_TAG_VREG || ld_dest.is_lval)
      continue;
    int32_t ld_dest_vr = irop_get_vreg(ld_dest);
    if (ld_dest_vr < 0)
      continue;

    /* Verify the slot has no other writers and no other readers anywhere in
     * the function.  Any STORE / second VLA_SP_SAVE / VLA_SP_RESTORE / LOAD
     * touching the slot disqualifies the rewrite — the slot's value would
     * then need to remain readable from memory. */
    int slot_is_isolated = 1;
    for (int k = 0; k < n && slot_is_isolated; k++)
    {
      if (k == i || k == j)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Check destination: any write to the same slot disqualifies. */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_get_tag(d) == IROP_TAG_STACKOFF && d.is_local &&
            irop_get_vreg(d) == -1 && irop_get_stack_offset(d) == slot)
        {
          slot_is_isolated = 0;
          break;
        }
      }
      /* Check sources: any read from the same slot disqualifies. */
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (operand_reads_slot(s, slot))
        {
          slot_is_isolated = 0;
          break;
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        if (operand_reads_slot(s, slot))
        {
          slot_is_isolated = 0;
          break;
        }
      }
    }
    if (!slot_is_isolated)
      continue;

    /* Rewrite VLA_SP_SAVE's dest from STACKOFF to the LOAD's vreg, and NOP
     * the LOAD.  Preserve dest btype as INT32 (pointer-sized SP). */
    IROperand new_dest = irop_make_vreg(ld_dest_vr, IROP_BTYPE_INT32);
    tcc_ir_set_dest(ir, i, new_dest);
    ld->op = TCCIR_OP_NOP;

    LOG_IR_GEN("ALLOCA-FWD: VLA_SP_SAVE@%d slot=%d redirected to vreg=%d "
               "(LOAD@%d folded)",
               i, slot, ld_dest_vr, j);
    changes++;
  }

  return changes;
}

int tcc_ir_opt_alloca_load_fwd_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_alloca_load_fwd(ctx->ir);
}

/* Dead-alloca elimination for VREG-target VLA_SP_SAVE.
 *
 * Companion to `dead_vla_struct_elim`, which only handles VLA_SP_SAVE writing
 * to a STACK SLOT (the original pre-`alloca_load_fwd` shape).  After
 * `alloca_load_fwd` rewrites the SP_SAVE's dest to a VREG, the resulting
 * pattern slips past `dead_vla_struct_elim`'s slot-based analysis.  This pass
 * handles the VREG-dest case directly.
 *
 * Pattern:
 *
 *   VLA_ALLOC #N, #align
 *   VLA_SP_SAVE -> V_seed (TEMP or VAR vreg)
 *   ... uses of V_seed (and transitively-propagated copies) only as STORE
 *       destinations, with no LOAD of memory through any tainted vreg, and
 *       no escape of V_seed's value to memory / calls / returns / globals.
 *
 * Bails (function-wide): same as `dead_vla_struct_elim`.  No CALL with
 * tainted-arg checks here because we already bail on CALL via the
 * dead_vla_struct_elim path; the late_cleanup loop ensures both passes see
 * the same IR snapshot.  We re-check the bail set defensively. */
int tcc_ir_opt_dead_alloca_vreg_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  if (ir->captured_count > 0 || ir->has_static_chain || ir->nb_nested_funcs > 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_NL_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_SET_CHAIN ||
        op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  int max_tmp = 0, max_var = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = tcc_ir_op_get_dest(ir, q);
    ops[1] = tcc_ir_op_get_src1(ir, q);
    ops[2] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < 3; k++)
    {
      int32_t vr = irop_get_vreg(ops[k]);
      if (vr < 0)
        continue;
      int t = TCCIR_DECODE_VREG_TYPE(vr);
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (t == TCCIR_VREG_TYPE_TEMP && p > max_tmp) max_tmp = p;
      else if (t == TCCIR_VREG_TYPE_VAR && p > max_var) max_var = p;
    }
  }

  uint8_t *tainted_tmp = tcc_malloc((max_tmp + 1));
  uint8_t *tainted_var = (max_var > 0) ? tcc_malloc((max_var + 1)) : NULL;
  int *kill_idx = tcc_malloc(sizeof(int) * n);

  int total_changes = 0;
  int any_dead = 0;

  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_VLA_ALLOC)
      continue;

    int save_idx = -1;
    for (int j = i + 1; j < n; j++)
    {
      TccIrOp op = ir->compact_instructions[j].op;
      if (op == TCCIR_OP_NOP)
        continue;
      if (op == TCCIR_OP_VLA_SP_SAVE)
        save_idx = j;
      break;
    }
    if (save_idx < 0)
      continue;

    IROperand save_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[save_idx]);
    int32_t seed_vr = irop_get_vreg(save_dest);
    if (seed_vr < 0)
      continue; /* slot-dest case → handled by dead_vla_struct_elim */
    int seed_type = TCCIR_DECODE_VREG_TYPE(seed_vr);
    int seed_pos = TCCIR_DECODE_VREG_POSITION(seed_vr);

    memset(tainted_tmp, 0, max_tmp + 1);
    if (tainted_var)
      memset(tainted_var, 0, max_var + 1);

    if (seed_type == TCCIR_VREG_TYPE_TEMP && seed_pos <= max_tmp)
      tainted_tmp[seed_pos] = 1;
    else if (seed_type == TCCIR_VREG_TYPE_VAR && tainted_var && seed_pos <= max_var)
      tainted_var[seed_pos] = 1;
    else
      continue;

    int kill_count = 0;
    int bail = 0;

    for (int j = save_idx + 1; j < n && !bail; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;

      if (q->op == TCCIR_OP_VLA_SP_RESTORE)
        continue;
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
          q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
      {
        bail = 1;
        break;
      }

      int has_d = irop_config[q->op].has_dest;
      int has_s1 = irop_config[q->op].has_src1;
      int has_s2 = irop_config[q->op].has_src2;
      IROperand d = {0}, s1 = {0}, s2 = {0};
      if (has_d) d = tcc_ir_op_get_dest(ir, q);
      if (has_s1) s1 = tcc_ir_op_get_src1(ir, q);
      if (has_s2) s2 = tcc_ir_op_get_src2(ir, q);

      /* Classify each source operand wrt taint.
       *   tainted_val   = operand yields a tainted VALUE (the alloca pointer or
       *                   something derived from it).  Propagation candidate.
       *   tainted_deref = operand is a memory READ through a tainted TEMP
       *                   pointer (lval-deref).  Bail — observer of alloca mem.
       *
       * VAR-src semantics: is_lval=1 is the normal "fetch from slot" form
       * (the VAR itself holds the alloca ptr), counts as tainted_val.
       * TEMP-src with is_lval=1 IS a deref of a pointer-typed TEMP, counts as
       * tainted_deref.  TEMP-src with is_lval=0 is value-use → tainted_val. */
#define CLASSIFY(_op, _val_out, _deref_out)                                      \
  do                                                                             \
  {                                                                              \
    int32_t _vr = irop_get_vreg(_op);                                            \
    if (_vr >= 0)                                                                \
    {                                                                            \
      int _vt = TCCIR_DECODE_VREG_TYPE(_vr);                                     \
      int _vp = TCCIR_DECODE_VREG_POSITION(_vr);                                 \
      if (_vt == TCCIR_VREG_TYPE_TEMP && _vp <= max_tmp && tainted_tmp[_vp])     \
      {                                                                          \
        if ((_op).is_lval) _deref_out = 1;                                       \
        else _val_out = 1;                                                       \
      }                                                                          \
      else if (_vt == TCCIR_VREG_TYPE_VAR && tainted_var && _vp <= max_var &&    \
               tainted_var[_vp])                                                 \
      {                                                                          \
        _val_out = 1;                                                            \
      }                                                                          \
    }                                                                            \
  } while (0)

      int s1_val = 0, s1_deref = 0, s2_val = 0, s2_deref = 0;
      if (has_s1) CLASSIFY(s1, s1_val, s1_deref);
      if (has_s2) CLASSIFY(s2, s2_val, s2_deref);

      if (s1_deref || s2_deref)
      {
        bail = 1;
        break;
      }

      /* STORE family: dest as tainted-TEMP pointer → kill candidate.
       * src1 (stored value) being tainted_val and dest NOT in tainted region
       * → escape (alloca ptr leaks into non-alloca memory). */
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
          q->op == TCCIR_OP_STORE_POSTINC)
      {
        int dest_is_tainted_addr = 0;
        if (has_d)
        {
          int32_t _vr = irop_get_vreg(d);
          if (_vr >= 0)
          {
            int _vt = TCCIR_DECODE_VREG_TYPE(_vr);
            int _vp = TCCIR_DECODE_VREG_POSITION(_vr);
            if (_vt == TCCIR_VREG_TYPE_TEMP && _vp <= max_tmp && tainted_tmp[_vp])
              dest_is_tainted_addr = 1;
          }
        }
        if (s1_val && !dest_is_tainted_addr)
        {
          bail = 1;
          break;
        }
        if (dest_is_tainted_addr)
          kill_idx[kill_count++] = j;
        continue;
      }

      /* No tainted input — instruction doesn't propagate or kill anything.
       * Special case: if dest is a tainted VAR being overwritten with a
       * non-tainted value, the VAR loses its taint. */
      if (!s1_val && !s2_val)
      {
        if (has_d)
        {
          int32_t _vr = irop_get_vreg(d);
          if (_vr >= 0)
          {
            int _vt = TCCIR_DECODE_VREG_TYPE(_vr);
            int _vp = TCCIR_DECODE_VREG_POSITION(_vr);
            if (_vt == TCCIR_VREG_TYPE_VAR && tainted_var && _vp <= max_var &&
                tainted_var[_vp])
              tainted_var[_vp] = 0;
          }
        }
        continue;
      }

      /* Tainted input: must be a propagator op.  Include LOAD because the
       * frontend sometimes emits `T = V [LOAD]` for a VAR fetch where ASSIGN
       * would have done equally — once we've ruled out TEMP-deref above
       * (tainted_deref bail), LOAD here is just a slot read. */
      int is_prop = (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA ||
                     q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ADD ||
                     q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_AND ||
                     q->op == TCCIR_OP_OR || q->op == TCCIR_OP_XOR);
      if (!is_prop || !has_d)
      {
        bail = 1;
        break;
      }
      int32_t d_vr = irop_get_vreg(d);
      if (d_vr < 0)
      {
        bail = 1;
        break;
      }
      int d_vt = TCCIR_DECODE_VREG_TYPE(d_vr);
      int d_vp = TCCIR_DECODE_VREG_POSITION(d_vr);
      if (d_vt == TCCIR_VREG_TYPE_TEMP && d_vp <= max_tmp)
      {
        tainted_tmp[d_vp] = 1;
        kill_idx[kill_count++] = j;
      }
      else if (d_vt == TCCIR_VREG_TYPE_VAR && tainted_var && d_vp <= max_var)
      {
        tainted_var[d_vp] = 1;
        kill_idx[kill_count++] = j;
      }
      else
      {
        bail = 1;
        break;
      }

#undef CLASSIFY
    }

    if (bail)
      continue;

    LOG_IR_GEN("DEAD-ALLOCA-VREG: NOP VLA_ALLOC@%d + VLA_SP_SAVE@%d + %d "
               "dependent ops",
               i, save_idx, kill_count);
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[save_idx].op = TCCIR_OP_NOP;
    for (int k = 0; k < kill_count; k++)
      ir->compact_instructions[kill_idx[k]].op = TCCIR_OP_NOP;
    total_changes += 2 + kill_count;
    any_dead = 1;
  }

  tcc_free(kill_idx);
  if (tainted_var)
    tcc_free(tainted_var);
  tcc_free(tainted_tmp);

  if (any_dead)
    total_changes += sweep_orphan_tmp_defs(ir, max_tmp);

  if (any_dead)
  {
    int has_vla_or_apply = 0;
    for (int i = 0; i < n; i++)
    {
      int op = ir->compact_instructions[i].op;
      if (op == TCCIR_OP_VLA_ALLOC || op == TCCIR_OP_BUILTIN_APPLY_ARGS ||
          op == TCCIR_OP_BUILTIN_APPLY || op == TCCIR_OP_SET_CHAIN)
      {
        has_vla_or_apply = 1;
        break;
      }
    }
    if (!has_vla_or_apply && tcc_state)
    {
      tcc_state->force_frame_pointer = 0;
      tcc_state->need_frame_pointer = 0;
    }
  }

  return total_changes;
}

int tcc_ir_opt_dead_alloca_vreg_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_alloca_vreg_elim(ctx->ir);
}
