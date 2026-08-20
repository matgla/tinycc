/*
 *  TCC IR - SSA constant-selector switch folding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `switch (7)` on a compile-time constant was dispatched at run time.
 *
 * Nothing in the pipeline looked at a SWITCH_TABLE's selector.  SCCP happily
 * propagated the constant into it and stopped there, so the switch survived to
 * ir/regalloc.c, where switch_to_data turned it into a `.rodata` value table
 * plus an indexed load -- a table lookup with a constant index -- and inside a
 * loop, where the case bodies are not the uniform shape switch_to_data wants,
 * it stayed a full PC-relative indirect jump executed on every iteration.
 * bench_switch pays ~30 instructions a trip for a `switch (i)` on `int i = 7`;
 * gcc -O2 emits five instructions for the whole function.
 *
 * Picking the arm is the entire transform: the selector indexes the table the
 * frontend already built, the SWITCH_TABLE becomes an unconditional JUMP, and
 * the dying edges' phi operands go with it.  ssa:dce then drops the arms the
 * jump no longer reaches, which is what lets ssa:dead_loop see a pure body and
 * delete the loop around it.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt/ssa/ssa_opt_helpers.h"

/* Successor blocks whose edge from `from_block` dies, minus the one we keep. */
static void sf_drop_dead_edges(IRSSAOptCtx *ctx, const TCCIRSwitchTable *table,
                               int from_block, int keep_block)
{
  IRCFG *cfg = ctx->cfg;

  for (int k = 0; k <= table->num_entries; k++) {
    int target = (k == table->num_entries) ? table->default_target : table->targets[k];
    if (target < 0 || target >= cfg->num_instrs)
      continue;
    int sb = cfg->instr_to_block[target];
    if (sb < 0 || sb == keep_block)
      continue;
    /* Idempotent: a block reached by several cases is dropped once. */
    ssa_drop_phi_edge(ctx, from_block, sb);
  }
}

int ssa_opt_switch_fold(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg || !ctx->ssa || ir->num_switch_tables == 0)
    return 0;

  int changes = 0;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_SWITCH_TABLE)
      continue;

    IROperand sel = tcc_ir_op_get_src1(ir, q);
    if (sel.is_lval || !irop_is_immediate(sel))
      continue;

    int table_id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
    if (table_id < 0 || table_id >= ir->num_switch_tables)
      continue;
    TCCIRSwitchTable *table = &ir->switch_tables[table_id];
    if (!table->targets || table->num_entries <= 0)
      continue;
    /* min_val + num_entries - 1 == max_val is the frontend's invariant; a
     * table that lost it would index off the end. */
    if (table->max_val - table->min_val + 1 != table->num_entries)
      continue;

    /* The selector is the index the frontend already normalized, not the raw
     * case value: gen/stmt/switch.c emits `value - min_val` (and the unsigned
     * range check against it) ahead of the SWITCH_TABLE, and the backend's
     * table lookup indexes with it directly.  Subtracting min_val a second
     * time here picked the arm min_val slots too far along -- invisible for
     * the common `case 0:` table, wrong for every other one:
     *   switch (-1) over cases -2..4 ran `case 1`.
     * Out of range cannot happen (the range check already branched to
     * default), but fold to default rather than index off the end. */
    int64_t idx = irop_get_imm64_ex(ir, sel);
    int target = (idx >= 0 && idx < table->num_entries)
                     ? table->targets[(int)idx]
                     : table->default_target;
    if (target < 0 || target >= ir->next_instruction_index || target >= cfg->num_instrs)
      continue;

    int from_block = (i < cfg->num_instrs) ? cfg->instr_to_block[i] : -1;
    int keep_block = cfg->instr_to_block[target];
    if (from_block < 0 || keep_block < 0)
      continue;

    sf_drop_dead_edges(ctx, table, from_block, keep_block);

    /* SWITCH_TABLE is {no dest, src1, src2} and JUMP is {dest, -, -}, so the
     * target needs a fresh pool slot rather than an operand rewrite. */
    IROperand jdest = irop_make_imm32(-1, target, IROP_BTYPE_INT32);
    int pool_base = tcc_ir_iroperand_pool_add(ir, jdest);
    q->op = TCCIR_OP_JUMP;
    q->operand_base = pool_base;
    changes++;
  }

  if (changes > 0)
    changes += ssa_opt_prune_unreachable_phis(ctx);
  return changes;
}
