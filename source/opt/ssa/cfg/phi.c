/*
 *  TCC SSA opt - phi simplification (CFG structural pass)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt_dsl_phi.h"
#include "opt/ssa/phi.h"

/* Phi elimination replaces every use of the phi's dest NAME with the
 * replacement NAME.  That is only meaning-preserving when both names are
 * genuine SSA values: this IR allows in-place re-definition of a phi dest
 * (`T <-- x [STORE]`, an assignment to a register-promoted local), so a
 * dest with def_count > 0 has uses that refer to the STORE's value, not the
 * phi's — rewriting them forwards across the second def (see the
 * ssa_opt_def_total comment in ssa_opt.h).  Symmetrically the replacement
 * must be single-def, or a use placed after its in-place redef reads the
 * new value where the phi carried the old one.  Casualty: tccgen's
 * token_stream_references_local_object — the loop-head phi for `v` was also
 * assigned in-place by the inlined sym_find's `v = t`, and eliminating the
 * phi rewrote `v = t - 256` into `v = v - 256` (v uninitialized), so the
 * self-hosted tcc dropped locals referenced by __builtin_va_arg_pack call
 * args (gcc-torture pr37669: 'chmap' undeclared). */
static int phi_dest_is_sole_def(IRSSAOptCtx *ctx, const IRPhiNode *phi)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->dest_vreg);
  return vi && vi->def_phi_block >= 0 && vi->def_count == 0;
}

static int phi_replacement_value_stable(IRSSAOptCtx *ctx, int32_t vr)
{
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  /* def_total 0 = entry-undef placeholder; 1 = clean single def. */
  return ssa_opt_def_total(ssa_opt_vinfo(ctx, vr)) <= 1;
}

OPT_GEN_PHI(phi_trivial)
{
  PATTERN_PHI(.kind = IR_PHI_PATTERN_TRIVIAL);
  if (!phi_dest_is_sole_def(ctx, phi) ||
      !phi_replacement_value_stable(ctx, replacement_vreg))
    return 0;
  REWRITE_PHI(.replacement = replacement_vreg);
}

static const OptDslPhiRule phi_rules[] = {
  OPT_PHI_ENTRY(phi_trivial),
};

typedef struct PhiSCCNode {
  IRPhiNode *phi;
  int block;
  int index;
  int lowlink;
  int component;
  uint8_t on_stack;
} PhiSCCNode;

typedef struct PhiSCCState {
  PhiSCCNode *nodes;
  int node_count;
  int next_index;
  int component_count;
  int *stack;
  int stack_count;
  int *vreg_map;
  int vreg_map_cap;
} PhiSCCState;

static int phi_scc_find_node(PhiSCCState *state, int32_t vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (pos >= state->vreg_map_cap)
    return -1;
  return state->vreg_map[pos];
}

static void phi_scc_visit(PhiSCCState *state, int node_idx)
{
  PhiSCCNode *node = &state->nodes[node_idx];
  node->index = state->next_index;
  node->lowlink = state->next_index++;
  state->stack[state->stack_count++] = node_idx;
  node->on_stack = 1;

  for (int i = 0; i < node->phi->num_operands; i++) {
    int dep_idx = phi_scc_find_node(state, node->phi->operands[i].vreg);
    if (dep_idx < 0)
      continue;

    PhiSCCNode *dep = &state->nodes[dep_idx];
    if (dep->index < 0) {
      phi_scc_visit(state, dep_idx);
      if (dep->lowlink < node->lowlink)
        node->lowlink = dep->lowlink;
    } else if (dep->on_stack && dep->index < node->lowlink) {
      node->lowlink = dep->index;
    }
  }

  if (node->lowlink != node->index)
    return;

  for (;;) {
    int member_idx = state->stack[--state->stack_count];
    PhiSCCNode *member = &state->nodes[member_idx];
    member->on_stack = 0;
    member->component = state->component_count;
    if (member_idx == node_idx)
      break;
  }
  state->component_count++;
}

static int phi_scc_replacement_type_valid(IRSSAOptCtx *ctx,
                                          PhiSCCState *state,
                                          int32_t replacement, int btype)
{
  int node_idx = phi_scc_find_node(state, replacement);
  if (node_idx >= 0)
    return state->nodes[node_idx].phi->btype == btype;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, replacement);
  if (!vi || vi->def_instr < 0)
    return 1;

  IROperand dest = tcc_ir_op_get_dest(
      ctx->ir, &ctx->ir->compact_instructions[vi->def_instr]);
  return irop_get_vreg(dest) == replacement && irop_get_btype(dest) == btype;
}

static int phi_scc_remove_component(IRSSAOptCtx *ctx, PhiSCCState *state,
                                    int component)
{
  int member_count = 0;
  int btype = -1;
  int32_t replacement = -1;

  for (int i = 0; i < state->node_count; i++) {
    PhiSCCNode *node = &state->nodes[i];
    if (node->component != component)
      continue;
    member_count++;
    if (btype < 0)
      btype = node->phi->btype;
    else if (btype != node->phi->btype)
      return 0;

    for (int slot = 0; slot < node->phi->num_operands; slot++) {
      int32_t operand = node->phi->operands[slot].vreg;
      if (operand < 0)
        continue;
      int dep_idx = phi_scc_find_node(state, operand);
      if (dep_idx >= 0 && state->nodes[dep_idx].component == component)
        continue;
      if (replacement < 0)
        replacement = operand;
      else if (replacement != operand)
        return 0;
    }
  }

  if (member_count < 2 || replacement < 0 ||
      !phi_scc_replacement_type_valid(ctx, state, replacement, btype))
    return 0;

  /* The replacement must be a stable single-def value (a clean phi dest
   * passes too: its phi def is the single def). */
  if (!phi_replacement_value_stable(ctx, replacement))
    return 0;

  for (int i = 0; i < state->node_count; i++) {
    PhiSCCNode *node = &state->nodes[i];
    if (node->component != component)
      continue;
    if (!phi_dest_is_sole_def(ctx, node->phi) ||
        !opt_dsl_phi_metadata_valid(ctx, node->block, node->phi) ||
        !ssa_opt_can_replace_all_uses(ctx, node->phi->dest_vreg))
      return 0;
  }

  for (int i = 0; i < state->node_count; i++) {
    PhiSCCNode *node = &state->nodes[i];
    if (node->component == component)
      ssa_opt_replace_all_uses(ctx, node->phi->dest_vreg, replacement);
  }

  for (int i = 0; i < state->node_count; i++) {
    PhiSCCNode *node = &state->nodes[i];
    if (node->component != component)
      continue;
    IRPhiNode **link = &ctx->ssa->block_phis[node->block];
    while (*link && (*link)->dest_vreg != node->phi->dest_vreg)
      link = &(*link)->next;
    if (!opt_dsl_phi_remove(ctx, node->block, link))
      return 0;
  }
  return member_count;
}

static int phi_scc_eliminate_once(IRSSAOptCtx *ctx)
{
  PhiSCCState state = {0};
  int node_count = opt_dsl_phi_count(ctx, NULL);
  if (node_count < 2)
    return 0;

  state.nodes = tcc_mallocz((size_t)node_count * sizeof(*state.nodes));
  state.stack = tcc_malloc((size_t)node_count * sizeof(*state.stack));
  state.vreg_map = tcc_malloc((size_t)ctx->vinfo_cap * sizeof(*state.vreg_map));
  for (int i = 0; i < ctx->vinfo_cap; i++)
    state.vreg_map[i] = -1;
  state.node_count = node_count;
  state.vreg_map_cap = ctx->vinfo_cap;

  int node_idx = 0;
  for (int b = 0; b < ctx->cfg->num_blocks; b++) {
    for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next) {
      state.nodes[node_idx].phi = phi;
      state.nodes[node_idx].block = b;
      state.nodes[node_idx].index = -1;
      state.nodes[node_idx].component = -1;
      int pos = TCCIR_DECODE_VREG_POSITION(phi->dest_vreg);
      if (pos < state.vreg_map_cap)
        state.vreg_map[pos] = node_idx;
      node_idx++;
    }
  }

  for (int i = 0; i < state.node_count; i++) {
    if (state.nodes[i].index < 0)
      phi_scc_visit(&state, i);
  }

  int changes = 0;
  for (int component = 0; component < state.component_count; component++) {
    changes = phi_scc_remove_component(ctx, &state, component);
    if (changes > 0)
      break;
  }

  tcc_free(state.stack);
  tcc_free(state.vreg_map);
  tcc_free(state.nodes);
  return changes;
}

static int phi_congruent(const IRPhiNode *a, const IRPhiNode *b)
{
  if (a->btype != b->btype || a->num_operands != b->num_operands)
    return 0;

  for (int i = 0; i < a->num_operands; i++) {
    int pred = a->operands[i].pred_block;
    int32_t v = a->operands[i].vreg;
    int found = 0;
    for (int j = 0; j < b->num_operands; j++) {
      if (b->operands[j].pred_block != pred)
        continue;
      if (b->operands[j].vreg != v)
        return 0;
      found = 1;
      break;
    }
    if (!found)
      return 0;
  }
  return 1;
}

static int phi_congruent_eliminate_once(IRSSAOptCtx *ctx)
{
  int changes = 0;

  for (int b = 0; b < ctx->cfg->num_blocks; b++) {
    for (IRPhiNode *rep = ctx->ssa->block_phis[b]; rep; rep = rep->next) {
      IRPhiNode **link = &rep->next;
      while (*link) {
        IRPhiNode *dup = *link;
        if (!phi_congruent(rep, dup) ||
            !phi_dest_is_sole_def(ctx, dup) ||
            !phi_dest_is_sole_def(ctx, rep) ||
            !opt_dsl_phi_metadata_valid(ctx, b, dup) ||
            !ssa_opt_can_replace_all_uses(ctx, dup->dest_vreg)) {
          link = &(*link)->next;
          continue;
        }

        ssa_opt_replace_all_uses(ctx, dup->dest_vreg, rep->dest_vreg);
        if (!opt_dsl_phi_remove(ctx, b, link)) {
          link = &(*link)->next;
          continue;
        }
        changes++;
      }
    }
  }
  return changes;
}

int ssa_opt_phi_simplify(IRSSAOptCtx *ctx)
{
  int phis_before = 0;
  int phis_after = 0;
  int operands_before = 0;

  if (TCC_LOG_IR_GEN)
    phis_before = opt_dsl_phi_count(ctx, &operands_before);

  int trivial_changes = 0;
  int scc_changes = 0;
  int congruent_changes = 0;
  for (;;) {
    int trivial = opt_dsl_run_phi_rules(
        ctx, phi_rules, OPT_DSL_TABLE_COUNT(phi_rules));
    int scc = phi_scc_eliminate_once(ctx);
    int congruent = phi_congruent_eliminate_once(ctx);
    trivial_changes += trivial;
    scc_changes += scc;
    congruent_changes += congruent;
    if (trivial + scc + congruent == 0)
      break;
  }
  int changes = trivial_changes + scc_changes + congruent_changes;

  if (TCC_LOG_IR_GEN) {
    phis_after = opt_dsl_phi_count(ctx, NULL);
    LOG_IR_GEN("ssa:phi_simplify phis_before=%d phis_after=%d operands_before=%d trivial_removed=%d scc_removed=%d congruent_removed=%d",
               phis_before, phis_after, operands_before, trivial_changes,
               scc_changes, congruent_changes);
  }

  return changes;
}
