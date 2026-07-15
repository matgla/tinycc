/*
 *  TCC IR - SSA Construction and Destruction
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa.h"
#include "memory/dynamic_bitset.h"
#include "memory/vector.h"

TCC_DYNAMIC_BITSET_DEFINE(SSABitset, 256)

static inline int raw_bitset_test(const uint8_t *bits, int pos)
{
  return bits[pos / 8] & (1 << (pos % 8));
}

static inline void raw_bitset_set(uint8_t *bits, int pos)
{
  bits[pos / 8] |= (1 << (pos % 8));
}

/* ============================================================================
 * SSA Construction
 * ============================================================================ */

static IRPhiNode *ssa_alloc_phi(int32_t orig_vreg, int32_t dest_vreg, int num_preds, int btype)
{
  IRPhiNode *phi = tcc_mallocz(sizeof(IRPhiNode));
  phi->orig_vreg = orig_vreg;
  phi->dest_vreg = dest_vreg;
  phi->num_operands = num_preds;
  phi->cap_operands = num_preds;
  phi->btype = btype;
  phi->operands = tcc_mallocz(num_preds * sizeof(IRPhiOperand));
  for (int i = 0; i < num_preds; i++) {
    phi->operands[i].vreg = -1;
    phi->operands[i].pred_block = -1;
  }
  return phi;
}

typedef struct {
  uint8_t *def_blocks;
  SSABitset *addrtaken;
  SSABitset *multi_block_def;
  int *var_btype;
  int block_bitset_bytes;
  int num_vars;
} SSAVarInfo;

/* LEA/ASM_INPUT/ASM_OUTPUT all prevent SSA promotion of the referenced VAR.
 * ASM: the codegen stores SValues with the original vreg at IR emission time;
 * SSA rename would split those into different temps, leaving stale SValues. */
static int ssa_mark_addrtaken(TCCIRState *ir, IRQuadCompact *q, SSABitset *addrtaken, int num_vars)
{
  int32_t vr = -1;
  if (q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASM_INPUT) {
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    vr = irop_get_vreg(src1);
  } else if (q->op == TCCIR_OP_ASM_OUTPUT) {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    vr = irop_get_vreg(dest);
  } else {
    return 0;
  }
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos < num_vars)
      SSABitset_set(addrtaken, pos);
  }
  return 1;
}

/* IJUMP: CFG cannot represent computed-goto edges, phi placement incomplete.
 * SETJMP: longjmp restores registers to setjmp-time values, losing
 * modifications made between setjmp and longjmp if locals are in regs. */
static int ssa_has_unsupported_ops(TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++) {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_NL_SETJMP)
      return 1;
  }
  return 0;
}

static int ssa_scan_var_defs(TCCIRState *ir, IRCFG *cfg, SSAVarInfo *info)
{
  int n = ir->next_instruction_index;
  int num_vars = info->num_vars;
  int bitset_bytes = info->block_bitset_bytes;
  dynamic_bitset(SSABitset) has_def = {0};

  if (SSABitset_init(&has_def, num_vars) != 0)
    return -1;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (ssa_mark_addrtaken(ir, q, info->addrtaken, num_vars))
      continue;

    if (!irop_config[q->op].has_dest)
      continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_FUNCPARAMVAL ||
        q->op == TCCIR_OP_FUNCPARAMVOID)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos >= num_vars)
      continue;

    int blk = cfg->instr_to_block[i];
    uint8_t *def_bits = &info->def_blocks[pos * bitset_bytes];
    if (SSABitset_test(&has_def, pos)) {
      if (!raw_bitset_test(def_bits, blk))
        SSABitset_set(info->multi_block_def, pos);
    }
    SSABitset_set(&has_def, pos);
    raw_bitset_set(def_bits, blk);
    if (dest.btype != IROP_BTYPE_INT32)
      info->var_btype[pos] = dest.btype;
  }

  for (int v = 0; v < num_vars; v++) {
    if (SSABitset_test(info->addrtaken, v))
      continue;
    if (v < ir->variables_live_intervals_size &&
        ir->variables_live_intervals[v].addrtaken)
      SSABitset_set(info->addrtaken, v);
  }

  return 0;
}

/* Decide whether a local VAR should be promoted to SSA (and get phi nodes).
 *
 * Single-block CFG: no back-edges, so any non-addrtaken VAR is safely
 * promotable to a TEMP via straight-line renaming — no phi placement needed.
 * Enabling this lets GVN / cprop / DCE see local-variable defs in leaf
 * functions.
 *
 * Multi-block CFG: a VAR defined in >=2 blocks (multi_block_def) needs phis and
 * is promoted.  A VAR defined in only ONE block ALSO needs a phi when that def
 * does not dominate all later uses — i.e. its def-block has a non-empty
 * dominance frontier.  The classic case is a value defined only inside a loop
 * and read again on the next iteration through the back-edge (the loop header
 * is in the def-block's DF): without a phi it stays an unpromoted VAR with no
 * loop-header definition, and the register allocator can hand it a register
 * that is clobbered around the loop body (gcc-torture pr125291).  A value
 * defined on one arm of a branch and read after the merge is the same shape.
 * Promoting it is always safe: the phi resolver drops undef (vreg<0) operands,
 * so a path that leaves the var genuinely uninitialized is unchanged. */
static int ssa_var_promotable(const SSAVarInfo *info, IRCFG *cfg, int nb, int v,
                              int single_block)
{
  if (SSABitset_test(info->addrtaken, v))
    return 0;
  if (single_block || SSABitset_test(info->multi_block_def, v))
    return 1;
  /* Single-block-def: promote iff a phi would actually be placed, i.e. some
   * def-block has a non-empty dominance frontier. */
  const uint8_t *def_bits = &info->def_blocks[v * info->block_bitset_bytes];
  for (int b = 0; b < nb; b++) {
    if (raw_bitset_test(def_bits, b) && cfg->blocks[b].num_df > 0)
      return 1;
  }
  return 0;
}

static uint8_t *ssa_build_promotable(const SSAVarInfo *info, IRCFG *cfg, int nb,
                                     int *out_count)
{
  int num_vars = info->num_vars;
  int single_block = (nb <= 1);
  int count = 0;
  for (int v = 0; v < num_vars; v++) {
    if (ssa_var_promotable(info, cfg, nb, v, single_block))
      count++;
  }
  *out_count = count;
  if (count == 0)
    return NULL;

  uint8_t *is_promotable = tcc_mallocz((num_vars + 7) / 8);
  for (int v = 0; v < num_vars; v++) {
    if (ssa_var_promotable(info, cfg, nb, v, single_block))
      raw_bitset_set(is_promotable, v);
  }
  return is_promotable;
}

static int ssa_place_phis_for_var(IRSSAState *ssa, TCCIRState *ir, IRCFG *cfg, int v, int var_btype,
                                  uint8_t *def_bits, SSABitset *has_phi,
                                  SSABitset *in_worklist, int *worklist,
                                  int phi_counter)
{
  int nb = cfg->num_blocks;
  int wl_count = 0;
  SSABitset_clear(has_phi);
  SSABitset_clear(in_worklist);

  for (int b = 0; b < nb; b++) {
    if (raw_bitset_test(def_bits, b)) {
      worklist[wl_count++] = b;
      SSABitset_set(in_worklist, b);
    }
  }

  int32_t orig_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, v);

  for (int wi = 0; wi < wl_count; wi++) {
    int b = worklist[wi];
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int di = 0; di < bb->num_df; di++) {
      int df = bb->dom_frontier[di];
      if (SSABitset_test(has_phi, df))
        continue;
      SSABitset_set(has_phi, df);

      int num_preds = cfg->blocks[df].num_preds;
      int32_t phi_dest = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP,
                                            ir->next_temporary_variable + phi_counter);
      phi_counter++;

      IRPhiNode *phi = ssa_alloc_phi(orig_vreg, phi_dest, num_preds, var_btype);
      for (int pi = 0; pi < num_preds; pi++)
        phi->operands[pi].pred_block = cfg->blocks[df].preds[pi];
      phi->next = ssa->block_phis[df];
      ssa->block_phis[df] = phi;

      if (!SSABitset_test(in_worklist, df)) {
        SSABitset_set(in_worklist, df);
        worklist[wl_count++] = df;
      }
    }
  }

  return phi_counter;
}

IRSSAState *tcc_ir_ssa_construct(TCCIRState *ir, IRCFG *cfg)
{
  if (!ir || !cfg || cfg->num_blocks == 0)
    return NULL;

  int nb = cfg->num_blocks;
  int num_vars = ir->next_local_variable;

  if (num_vars == 0 || nb == 0)
    return NULL;

  if (ssa_has_unsupported_ops(ir))
    return NULL;

  int bitset_bytes = (nb + 7) / 8;
  scoped_vector(uint8_t) def_blocks_owner = {0};
  scoped_vector(int) var_btype_owner = {0};
  dynamic_bitset(SSABitset) addrtaken = {0};
  dynamic_bitset(SSABitset) multi_block_def = {0};

  if (vector_resize(&def_blocks_owner, (size_t)num_vars * bitset_bytes) != 0 ||
      vector_resize(&var_btype_owner, num_vars) != 0 ||
      SSABitset_init(&addrtaken, num_vars) != 0 ||
      SSABitset_init(&multi_block_def, num_vars) != 0)
    return NULL;

  SSAVarInfo info = {
    .def_blocks = vector_data(&def_blocks_owner),
    .addrtaken = &addrtaken,
    .multi_block_def = &multi_block_def,
    .var_btype = vector_data(&var_btype_owner),
    .block_bitset_bytes = bitset_bytes,
    .num_vars = num_vars,
  };
  if (ssa_scan_var_defs(ir, cfg, &info) != 0)
    return NULL;

  int promotable_count;
  uint8_t *is_promotable = ssa_build_promotable(&info, cfg, nb, &promotable_count);
  if (!is_promotable)
    return NULL;

  IRSSAState *ssa = tcc_mallocz(sizeof(IRSSAState));
  ssa->cfg = cfg;
  ssa->block_phis = tcc_mallocz(nb * sizeof(IRPhiNode *));
  ssa->next_ssa_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, ir->next_temporary_variable);
  ssa->is_promotable = is_promotable;
  ssa->num_vars = num_vars;

  dynamic_bitset(SSABitset) has_phi = {0};
  dynamic_bitset(SSABitset) in_worklist = {0};
  scoped_vector(int) worklist_owner = {0};
  if (SSABitset_init(&has_phi, nb) != 0 ||
      SSABitset_init(&in_worklist, nb) != 0 ||
      vector_resize(&worklist_owner, nb) != 0) {
    tcc_ir_ssa_free(ssa);
    return NULL;
  }
  int *worklist = vector_data(&worklist_owner);
  int phi_counter = 0;

  for (int v = 0; v < num_vars; v++) {
    /* Place phis for every promoted var (is_promotable already excludes
     * addrtaken). For single-block-def vars this now also covers the ones kept
     * as VARs before — loop-carried / branch-merge-live values that need a phi.
     * In a single-block CFG the def-block has an empty DF, so this places none. */
    if (!raw_bitset_test(is_promotable, v))
      continue;
    uint8_t *def_bits = &info.def_blocks[v * bitset_bytes];
    phi_counter = ssa_place_phis_for_var(ssa, ir, cfg, v, info.var_btype[v], def_bits,
                                         &has_phi, &in_worklist, worklist,
                                         phi_counter);
  }

  ssa->next_ssa_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP,
                                          ir->next_temporary_variable + phi_counter);

  return ssa;
}

/* ============================================================================
 * SSA Renaming
 * ============================================================================ */

typedef vector(int32_t) VRegStack;

static void vstack_push(VRegStack *s, int32_t v)
{
  vector_push_back(s, v);
}

static int32_t vstack_top(VRegStack *s)
{
  return s->size > 0 ? s->data[s->size - 1] : -1;
}

static int ssa_rename_use(IROperand *op, int num_vars, const uint8_t *is_promotable,
                          VRegStack *stacks)
{
  int32_t vr = irop_get_vreg(*op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (pos >= num_vars || !raw_bitset_test(is_promotable, pos))
    return 0;
  int32_t cur = vstack_top(&stacks[pos]);
  if (cur < 0)
    return 0;
  /* A use that dereferences the variable's *value* (is_lval set, is_local
   * clear) is a pointer dereference — e.g. `*vv` / `vv->m` after the address
   * fold collapsed `&vv->m` (offset 0) to vv itself, leaving the var operand
   * as the pointer being stored/loaded through.  Promoting vv to an SSA
   * register must KEEP the dereference: the pointer now lives in `cur`, so
   * `*cur` still loads/stores through it.  Only a var-SLOT access (is_local)
   * collapses to a plain register value.  Without this, `(vv=call())->m0=c`
   * lowered `*vv=c` to `vv=c`, dropping the store and clobbering the pointer. */
  int deref_through_value = op->is_lval && !op->is_local;
  irop_set_vreg(op, cur);
  op->tag = IROP_TAG_VREG;
  if (!deref_through_value) {
    op->is_lval = 0;
    op->is_local = 0;
  }
  op->u.imm32 = 0;
  return deref_through_value ? 2 : 1;
}

static void ssa_rename_phi_defs(IRSSAState *ssa, int b, VRegStack *stacks, int num_vars)
{
  for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
    int32_t orig = phi->orig_vreg;
    if (TCCIR_DECODE_VREG_TYPE(orig) != TCCIR_VREG_TYPE_VAR)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(orig);
    if (pos < num_vars && raw_bitset_test(ssa->is_promotable, pos))
      vstack_push(&stacks[pos], phi->dest_vreg);
  }
}

static void ssa_rename_block_instrs(TCCIRState *ir, IRSSAState *ssa, IRBasicBlock *bb,
                                    VRegStack *stacks, int num_vars, int *next_temp_pos)
{
  for (int i = bb->start_idx; i < bb->end_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int r = ssa_rename_use(&s, num_vars, ssa->is_promotable, stacks);
      if (r) {
        tcc_ir_op_set_src1(ir, q, s);
        /* A var-slot LOAD becomes a register copy (ASSIGN); a LOAD that
         * dereferences the var's pointer value (r==2) stays a real LOAD. */
        if (r == 1 && q->op == TCCIR_OP_LOAD)
          q->op = TCCIR_OP_ASSIGN;
      }
    }

    if (irop_config[q->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      if (ssa_rename_use(&s, num_vars, ssa->is_promotable, stacks))
        tcc_ir_op_set_src2(ir, q, s);
    }

    if (q->op == TCCIR_OP_MLA) {
      IROperand s = tcc_ir_op_get_accum(ir, q);
      if (ssa_rename_use(&s, num_vars, ssa->is_promotable, stacks))
        tcc_ir_op_set_accum(ir, q, s);
    }

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (ssa_rename_use(&d, num_vars, ssa->is_promotable, stacks))
        tcc_ir_op_set_dest(ir, q, d);
      continue;
    }

    if (irop_config[q->op].has_dest &&
        q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(d);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars && raw_bitset_test(ssa->is_promotable, pos)) {
          int32_t new_name = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, (*next_temp_pos)++);
          vstack_push(&stacks[pos], new_name);
          irop_set_vreg(&d, new_name);
          d.tag = IROP_TAG_VREG;
          d.is_lval = 0;
          d.is_local = 0;
          d.u.imm32 = 0;
          tcc_ir_op_set_dest(ir, q, d);
        }
      }
    }
  }
}

static void ssa_fill_successor_phis(IRSSAState *ssa, IRCFG *cfg, int b,
                                    VRegStack *stacks, int num_vars)
{
  IRBasicBlock *bb = &cfg->blocks[b];
  for (int si = 0; si < bb->num_succs; si++) {
    int succ = bb->succs[si];
    if (succ < 0)
      continue;
    IRBasicBlock *sbb = &cfg->blocks[succ];
    int pred_idx = -1;
    for (int pi = 0; pi < sbb->num_preds; pi++) {
      if (sbb->preds[pi] == b) { pred_idx = pi; break; }
    }
    if (pred_idx < 0)
      continue;
    for (IRPhiNode *phi = ssa->block_phis[succ]; phi; phi = phi->next) {
      int32_t orig = phi->orig_vreg;
      if (TCCIR_DECODE_VREG_TYPE(orig) != TCCIR_VREG_TYPE_VAR)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(orig);
      if (pos < num_vars && raw_bitset_test(ssa->is_promotable, pos)) {
        if (pred_idx < phi->num_operands)
          phi->operands[pred_idx].vreg = vstack_top(&stacks[pos]);
      }
    }
  }
}

void tcc_ir_ssa_rename(TCCIRState *ir, IRSSAState *ssa)
{
  if (!ir || !ssa || !ssa->cfg || !ssa->is_promotable)
    return;

  IRCFG *cfg = ssa->cfg;
  int nb = cfg->num_blocks;
  int num_vars = ssa->num_vars;
  int next_temp_pos = TCCIR_DECODE_VREG_POSITION(ssa->next_ssa_vreg);

  scoped_vector(VRegStack) stacks_owner = {0};
  if (vector_resize(&stacks_owner, num_vars) != 0)
    return;
  VRegStack *stacks = vector_data(&stacks_owner);

  for (int v = 0; v < num_vars; v++) {
    if (raw_bitset_test(ssa->is_promotable, v)) {
      int32_t init_name = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, next_temp_pos++);
      vstack_push(&stacks[v], init_name);
    }
  }

  typedef struct { int block; int child_idx; } DomFrame;
  scoped_vector(DomFrame) dom_stack_owner = {0};
  scoped_vector(int) saved_depths_owner = {0};
  if (vector_resize(&dom_stack_owner, nb) != 0 ||
      vector_resize(&saved_depths_owner, (size_t)nb * num_vars) != 0) {
    for (int v = 0; v < num_vars; v++)
      vector_cleanup(&stacks[v]);
    return;
  }
  DomFrame *dom_stack = vector_data(&dom_stack_owner);
  int *saved_depths = vector_data(&saved_depths_owner);
  int dsp = 0;

  dom_stack[dsp++] = (DomFrame){0, 0};

  while (dsp > 0) {
    DomFrame *top = &dom_stack[dsp - 1];
    int b = top->block;

    if (top->child_idx == 0) {
      int *frame_depths = &saved_depths[(dsp - 1) * num_vars];
      for (int v = 0; v < num_vars; v++)
        frame_depths[v] = stacks[v].size;

      ssa_rename_phi_defs(ssa, b, stacks, num_vars);
      ssa_rename_block_instrs(ir, ssa, &cfg->blocks[b], stacks, num_vars, &next_temp_pos);
      ssa_fill_successor_phis(ssa, cfg, b, stacks, num_vars);
    }

    IRBasicBlock *bb = &cfg->blocks[b];
    if (top->child_idx < bb->num_dom_children) {
      int child = bb->dom_children[top->child_idx];
      top->child_idx++;
      dom_stack[dsp++] = (DomFrame){child, 0};
    }
    else {
      int *frame_depths = &saved_depths[(dsp - 1) * num_vars];
      for (int v = 0; v < num_vars; v++)
        stacks[v].size = frame_depths[v];
      dsp--;
    }
  }

  tcc_ir_vreg_ensure_temp_capacity(ir, next_temp_pos);
  ir->next_temporary_variable = next_temp_pos;

  for (int v = 0; v < num_vars; v++)
    vector_cleanup(&stacks[v]);
}

void tcc_ir_ssa_free(IRSSAState *ssa)
{
  if (!ssa)
    return;
  if (ssa->block_phis && ssa->cfg) {
    for (int b = 0; b < ssa->cfg->num_blocks; b++) {
      IRPhiNode *phi = ssa->block_phis[b];
      while (phi) {
        IRPhiNode *next = phi->next;
        tcc_free(phi->operands);
        tcc_free(phi);
        phi = next;
      }
    }
    tcc_free(ssa->block_phis);
  }
  tcc_free(ssa->is_promotable);
  tcc_free(ssa);
}
