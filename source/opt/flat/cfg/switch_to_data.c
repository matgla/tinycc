/*
 *  TCC IR - Switch-to-data-table transformation
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 *  This library is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

#define SWITCHDATA_MAX_BODY_OPS 1

/* Matches `[NOP..] ASSIGN dst<-const [NOP..] JMP merge` with no intervening jump_target; returns 0 on match. */
static int sd_check_case_body(TCCIRState *ir, int target,
                              IROperand *out_dest, IROperand *out_val, int *out_merge,
                              int *out_assign_idx, int *out_jump_idx)
{
  int n = ir->next_instruction_index;
  if (target < 0 || target >= n)
    return 1;

  IRQuadCompact *qi;
  int idx = target;
  while (idx < n) {
    qi = &ir->compact_instructions[idx];
    if (qi->op != TCCIR_OP_NOP)
      break;
    idx++;
    if (idx < n && ir->compact_instructions[idx].is_jump_target)
      return 1;
  }
  if (idx >= n)
    return 1;
  qi = &ir->compact_instructions[idx];
  if (qi->op != TCCIR_OP_ASSIGN && qi->op != TCCIR_OP_STORE)
    return 1;

  IROperand dest = tcc_ir_op_get_dest(ir, qi);
  IROperand src = tcc_ir_op_get_src1(ir, qi);
  /* The memory-resolved form of the same phi: each arm stores its constant
   * into the merge temp's home slot (`STORE #k -> V' + JMP merge) and the
   * join reads the slot once.  The caller rewrites that one read into the
   * SWITCH_LOAD dest, so the arms and the slot both go away.  Only the
   * not-is_lval ASSIGN dest and this STORE-to-a-vreg-home form are shapes
   * the rewrite understands. */
  if (qi->op == TCCIR_OP_ASSIGN) {
    if (dest.is_lval)
      return 1;
  } else {
    if (!dest.is_lval || irop_get_vreg(dest) < 0 || dest.is_llocal ||
        (irop_get_tag(dest) != IROP_TAG_STACKOFF && irop_get_tag(dest) != IROP_TAG_VREG))
      return 1;
  }
  int tag = src.tag;
  if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_SYMREF)
    return 1;
  if (src.is_lval || src.is_llocal)
    return 1;

  int dbt = irop_get_btype(dest);
  int sbt = irop_get_btype(src);
  /* The ASSIGN form's dest is the SWITCH_LOAD dest directly, so its type is
   * the table's.  A STORE arm's slot may be narrower than a word (a byte
   * enum, say); the value table still holds whole-word constants and the
   * join read keeps its own type. */
  if (qi->op == TCCIR_OP_ASSIGN) {
    if ((dbt != IROP_BTYPE_INT32 && dbt != IROP_BTYPE_FUNC) ||
        (sbt != IROP_BTYPE_INT32 && sbt != IROP_BTYPE_FUNC))
      return 1;
  }

  int jidx = idx + 1;
  while (jidx < n) {
    if (ir->compact_instructions[jidx].is_jump_target)
      return 1;
    IRQuadCompact *cq = &ir->compact_instructions[jidx];
    if (cq->op == TCCIR_OP_NOP) {
      jidx++;
      continue;
    }
    if (cq->op == TCCIR_OP_JUMP)
      break;
    return 1;
  }
  if (jidx >= n)
    return 1;
  IRQuadCompact *jq = &ir->compact_instructions[jidx];
  int merge = (int)tcc_ir_op_dest_imm(ir, jq);
  if (merge < 0 || merge >= n)
    return 1;

  *out_dest = dest;
  *out_val = src;
  *out_merge = merge;
  *out_assign_idx = idx;
  *out_jump_idx = jidx;
  return 0;
}

static int sd_alloc_value_table(TCCIRState *ir, int num_entries)
{
  if (ir->num_switch_value_tables >= ir->switch_value_tables_capacity) {
    int new_cap = ir->switch_value_tables_capacity * 2 + 4;
    ir->switch_value_tables = tcc_realloc(ir->switch_value_tables,
                                          new_cap * sizeof(TCCIRSwitchValueTable));
    /* Zero the grown tail: rodata_sym/default_val must be initialised. */
    for (int k = ir->switch_value_tables_capacity; k < new_cap; k++) {
      ir->switch_value_tables[k].values = NULL;
      ir->switch_value_tables[k].num_entries = 0;
      ir->switch_value_tables[k].rodata_sym = NULL;
      memset(&ir->switch_value_tables[k].default_val, 0, sizeof(IROperand));
    }
    ir->switch_value_tables_capacity = new_cap;
  }
  int id = ir->num_switch_value_tables++;
  TCCIRSwitchValueTable *t = &ir->switch_value_tables[id];
  t->num_entries = num_entries;
  t->values = tcc_mallocz(num_entries * sizeof(IROperand));
  t->rodata_sym = NULL;
  memset(&t->default_val, 0, sizeof(IROperand));
  return id;
}

/* Does the rewrite NOP case k's body?  Not when it is shared with the
 * default: the out-of-range JUMPIF still branches there. */
static int sd_body_removed(const TCCIRSwitchTable *table, const int *probe_assign, const int *probe_jump, int k)
{
  for (int m = 0; m < table->num_entries; m++)
    if (table->targets[m] == table->default_target &&
        (probe_assign[m] == probe_assign[k] || probe_jump[m] == probe_jump[k]))
      return 0;
  return 1;
}

/* How many JUMP/JUMPIF and switch table edges enter each instruction, the
 * entries of every SWITCH_TABLE included.  Returns 0 when an IJUMP makes every
 * label a possible target. */
static int sd_count_refs(TCCIRState *ir, int *refs)
{
  int n = ir->next_instruction_index;
  memset(refs, 0, n * sizeof(int));
  for (int j = 0; j < n; j++) {
    IRQuadCompact *jq = &ir->compact_instructions[j];
    if (jq->op == TCCIR_OP_IJUMP)
      return 0;
    if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF) {
      int64_t t = irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jq));
      if (t >= 0 && t < n)
        refs[t]++;
    } else if (jq->op == TCCIR_OP_SWITCH_TABLE) {
      int id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, jq));
      if (id < 0 || id >= ir->num_switch_tables)
        continue;
      const TCCIRSwitchTable *o = &ir->switch_tables[id];
      for (int m = 0; m < o->num_entries; m++)
        if (o->targets[m] >= 0 && o->targets[m] < n)
          refs[o->targets[m]]++;
      if (o->default_target >= 0 && o->default_target < n)
        refs[o->default_target]++;
    }
  }
  return 1;
}

/* Add `by` times the edges of table t's dispatch, once per SWITCH_TABLE using
 * it: with by = -1 the counts leave out the table being rewritten. */
static void sd_adjust_refs(TCCIRState *ir, int *refs, int t, int by)
{
  int n = ir->next_instruction_index, uses = 0;
  for (int j = 0; j < n; j++) {
    IRQuadCompact *jq = &ir->compact_instructions[j];
    if (jq->op == TCCIR_OP_SWITCH_TABLE && (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, jq)) == t)
      uses++;
  }
  const TCCIRSwitchTable *o = &ir->switch_tables[t];
  for (int m = 0; m < o->num_entries; m++)
    if (o->targets[m] >= 0 && o->targets[m] < n)
      refs[o->targets[m]] += by * uses;
  if (o->default_target >= 0 && o->default_target < n)
    refs[o->default_target] += by * uses;
}

/* Does anything other than this table's own dispatch reach the case body
 * [target..jidx]?  A JUMP/JUMPIF (a goto to the case label), another switch
 * table, or a fall-through from the preceding instruction would land on the
 * NOPs left by the rewrite.  `refs` counts the edges into each instruction
 * from everything but this table (sd_count_refs, sd_adjust_refs). */
static int sd_body_has_outside_pred(TCCIRState *ir, const int *refs, int target, int jidx)
{
  for (int j = target; j <= jidx; j++)
    if (refs[j])
      return 1;
  /* fall-through: the previous real instruction must not continue here */
  int p = target - 1;
  while (p >= 0 && ir->compact_instructions[p].op == TCCIR_OP_NOP)
    p--;
  if (p >= 0 && !ir_op_has(ir->compact_instructions[p].op, IROP_A_NO_FALLTHROUGH))
    return 1;
  return 0;
}

int tcc_ir_opt_switch_to_data(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 4 || ir->num_switch_tables == 0)
    return 0;

  int changes = 0;

  /* Probe scratch must be heap-allocated: fixed [1024] arrays cost ~24 KiB of frame in every function, overflowing the 32 KiB target stack. */
  int max_entries = 0;
  for (int t = 0; t < ir->num_switch_tables; t++) {
    int ne = ir->switch_tables[t].num_entries;
    if (ne > max_entries)
      max_entries = ne;
  }
  if (max_entries > 1024)
    max_entries = 1024;
  if (max_entries <= 0)
    return 0;
  int *probe_assign = tcc_malloc(max_entries * sizeof(int));
  int *probe_jump = tcc_malloc(max_entries * sizeof(int));
  IROperand *probe_val = tcc_malloc(max_entries * sizeof(IROperand));
  /* Edge counts for sd_body_has_outside_pred, counted when first needed and
   * again after a rewrite changes the jumps; refs_ok 0 means an IJUMP. */
  int *refs = tcc_malloc(n * sizeof(int));
  int refs_valid = 0, refs_ok = 0;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_SWITCH_TABLE)
      continue;

    IROperand idx_op = tcc_ir_op_get_src1(ir, q);
    int table_id = (int)tcc_ir_op_src2_imm(ir, q);
    if (table_id < 0 || table_id >= ir->num_switch_tables)
      continue;
    TCCIRSwitchTable *table = &ir->switch_tables[table_id];
    if (!table || table->num_entries <= 0 || !table->targets)
      continue;

    /* Requires: same dest_vreg, same merge target, constant-source ASSIGN body across all cases. */
    int ok = 1;
    int32_t common_dest_vr = -1;
    int common_dbt = -1;
    int common_merge = -1;
    int common_dest_unsigned = 0;
    /* Probe buffers are sized to min(max_entries, 1024). */
    if (table->num_entries > 1024) continue;
    if (!refs_valid) {
      refs_ok = sd_count_refs(ir, refs);
      refs_valid = 1;
    }
    if (!refs_ok)
      continue;
    sd_adjust_refs(ir, refs, table_id, -1);
    int arm_store_form = -1; /* all arms ASSIGN (0) / all STORE (1); -1 unset */
    for (int k = 0; k < table->num_entries; k++) {
      IROperand d, v;
      int merge, aidx, jidx;
      if (sd_check_case_body(ir, table->targets[k], &d, &v, &merge, &aidx, &jidx)) {
        ok = 0;
        break;
      }
      int arm_is_store = ir->compact_instructions[aidx].op == TCCIR_OP_STORE;
      if (arm_store_form < 0)
        arm_store_form = arm_is_store;
      else if (arm_store_form != arm_is_store) {
        ok = 0;
        break;
      }
      int32_t dvr = irop_get_vreg(d);
      if (k == 0) {
        common_dest_vr = dvr;
        common_dbt = irop_get_btype(d);
        common_dest_unsigned = d.is_unsigned;
        common_merge = merge;
      } else {
        if (dvr != common_dest_vr || irop_get_btype(d) != common_dbt || merge != common_merge) {
          ok = 0;
          break;
        }
      }
      if (sd_body_has_outside_pred(ir, refs, table->targets[k], jidx)) {
        ok = 0;
        break;
      }
      probe_assign[k] = aidx;
      probe_jump[k] = jidx;
      probe_val[k] = v;
    }
    /* While this table's own edges are still excluded: does anything still
     * jump to the default body?  Zero means the dispatch had no range check
     * (the index is masked in-range), so the default arm is unreachable. */
    int default_live_refs = -1;
    if (table->default_target >= 0 && table->default_target < n)
      default_live_refs = refs[table->default_target];
    sd_adjust_refs(ir, refs, table_id, 1);
    if (!ok)
      continue;
    if (common_dest_vr < 0)
      continue;

    /* STORE-form arms park their constant in the merge temp's home slot; the
     * join's read of that slot becomes the SWITCH_LOAD dest.  The rewrite is
     * only sound when the slot is this switch's alone: one reader, no other
     * writer, address never taken.  A default arm that still reaches the
     * merge would store the slot whose only reader is about to disappear, so
     * a live default keeps the dispatch as it was; a dead one (no range
     * check, refs 0 above) is dropped along with the arms. */
    int join_load_idx = -1;
    int default_store_idx = -1;
    if (arm_store_form == 1)
    {
      int slot_vr = common_dest_vr;
      if (table->default_target >= 0)
      {
        if (default_live_refs != 0)
          continue;
        /* The default body is exactly one matching STORE, then NOPs into the
         * merge (fall-through), so dropping it changes nothing else.  A body
         * that cannot fall through (UNREACHABLE / TRAP / RETURN) needs no
         * dropping: nothing reaches it once the dispatch is gone. */
        int p = table->default_target;
        while (p < common_merge && ir->compact_instructions[p].op == TCCIR_OP_NOP)
          p++;
        if (p >= common_merge)
          continue;
        if (ir_op_has(ir->compact_instructions[p].op, IROP_A_NO_FALLTHROUGH))
          goto default_handled; /* UNREACHABLE / TRAP: nothing reaches it */
        if (p > common_merge)
          continue;
        IROperand dd = tcc_ir_op_get_dest(ir, &ir->compact_instructions[p]);
        if (ir->compact_instructions[p].op != TCCIR_OP_STORE || dd.is_lval == 0 ||
            irop_get_vreg(dd) != slot_vr)
          continue;
        int p2 = p + 1;
        while (p2 < common_merge && ir->compact_instructions[p2].op == TCCIR_OP_NOP)
          p2++;
        if (p2 != common_merge)
          continue;
        default_store_idx = p;
      }
    default_handled:;
      int nload = 0;
      for (int j = 0; j < n; j++) {
        IRQuadCompact *jq2 = &ir->compact_instructions[j];
        if (jq2->op == TCCIR_OP_NOP)
          continue;
        int nops = irop_config[jq2->op].has_dest + irop_config[jq2->op].has_src1 +
                   irop_config[jq2->op].has_src2;
        if (ir_op_has(jq2->op, IROP_A_SLOT3))
          nops++;
        for (int s2 = 0; s2 < nops; s2++) {
          IROperand op2 = ir->iroperand_pool[jq2->operand_base + s2];
          if (irop_get_vreg(op2) != slot_vr || !irop_has_vreg(op2))
            continue;
          if (irop_get_tag(op2) != IROP_TAG_STACKOFF && irop_get_tag(op2) != IROP_TAG_VREG)
            continue;
          if (!op2.is_lval)
            ok = 0; /* its address escapes */
          else if (jq2->op == TCCIR_OP_LOAD && s2 >= 1 && !op2.is_llocal) {
            nload++;
            join_load_idx = j;
          } else if (jq2->op == TCCIR_OP_STORE && s2 == 0) {
            /* only the case arms and a dead default write the slot */
            int is_arm = default_store_idx == j;
            for (int k = 0; k < table->num_entries && !is_arm; k++)
              is_arm = probe_assign[k] == j;
            if (!is_arm)
              ok = 0;
          } else {
            /* a partial/narrow access, or any other touch of the slot */
            ok = 0;
          }
        }
        if (!ok)
          break;
      }
      if (!ok || nload != 1 || join_load_idx < 0)
        continue;
      if (!tcc_ir_op_dest_has_vreg(ir, &ir->compact_instructions[join_load_idx]) ||
          tcc_ir_op_dest_vreg(ir, &ir->compact_instructions[join_load_idx]) < 0)
        continue;
      common_dest_vr = tcc_ir_op_dest_vreg(ir, &ir->compact_instructions[join_load_idx]);
      common_dbt = irop_get_btype(tcc_ir_op_get_dest(ir, &ir->compact_instructions[join_load_idx]));
      common_dest_unsigned = tcc_ir_op_get_dest(ir, &ir->compact_instructions[join_load_idx]).is_unsigned;
    }

    /* SWITCH_LOAD falls through to what follows the dispatch.  With the
     * dispatch ahead of the bodies (switch_head.c) that is a case body, not
     * the merge: a jump to the merge takes the slot after the dispatch, which
     * must be a NOP or a case ASSIGN this rewrite removes, entered by nothing
     * but the table. */
    int merge_first = common_merge;
    while (merge_first < n && ir->compact_instructions[merge_first].op == TCCIR_OP_NOP)
      merge_first++;
    /* what follows once the case bodies below are gone */
    int after = i + 1;
    for (; after < n; after++)
    {
      if (ir->compact_instructions[after].op == TCCIR_OP_NOP)
        continue;
      int gone = 0;
      for (int k = 0; k < table->num_entries && !gone; k++)
        gone = (probe_assign[k] == after || probe_jump[k] == after) &&
               sd_body_removed(table, probe_assign, probe_jump, k);
      if (!gone)
        break;
    }
    int jump_slot = -1;
    if (after != merge_first)
    {
      int s = i + 1;
      if (s >= n)
        continue;
      int removable = ir->compact_instructions[s].op == TCCIR_OP_NOP;
      for (int k = 0; k < table->num_entries && !removable; k++)
        removable = probe_assign[k] == s && sd_body_removed(table, probe_assign, probe_jump, k);
      if (!removable || s == table->default_target)
        continue;
      for (int j = 0; j < n && removable; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if ((jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF) &&
            (int)tcc_ir_op_dest_imm(ir, jq) == s)
          removable = 0;
      }
      if (!removable)
        continue;
      jump_slot = s;
    }

    /* default_val is never read: the range-check JMP covers the out-of-range path. */
    int vt_id = sd_alloc_value_table(ir, table->num_entries);
    TCCIRSwitchValueTable *vtab = &ir->switch_value_tables[vt_id];

    for (int k = 0; k < table->num_entries; k++)
      vtab->values[k] = probe_val[k];
    vtab->default_val.tag = IROP_TAG_IMM32;
    vtab->default_val.u.imm32 = 0;

    /* SWITCH_LOAD takes three operands, so fresh pool slots are needed before repointing operand_base. */
    IROperand dest_op;
    memset(&dest_op, 0, sizeof(dest_op));
    irop_set_vreg(&dest_op, common_dest_vr);
    dest_op.tag = IROP_TAG_VREG;
    dest_op.btype = common_dbt;
    dest_op.is_unsigned = common_dest_unsigned;
    /* is_lval stays 0: SWITCH_LOAD's dest is a direct write, unlike the merge-block reads of V. */

    IROperand new_tid_op;
    memset(&new_tid_op, 0, sizeof(new_tid_op));
    new_tid_op.tag = IROP_TAG_IMM32;
    new_tid_op.u.imm32 = vt_id;
    new_tid_op.btype = IROP_BTYPE_INT32;
    irop_set_vreg(&new_tid_op, -1);

    int new_base = tcc_ir_iroperand_pool_add(ir, dest_op);
    tcc_ir_iroperand_pool_add(ir, idx_op);
    tcc_ir_iroperand_pool_add(ir, new_tid_op);

    q->op = TCCIR_OP_SWITCH_LOAD;
    q->operand_base = new_base;

    {
      int tbl_bytes = vtab->num_entries * 4;
      /* A table with a SYMREF entry needs relocations, so it cannot live in shared .rodata. */
      int tbl_has_symref = 0;
      for (int k = 0; k < vtab->num_entries; k++) {
        if (vtab->values[k].tag == IROP_TAG_SYMREF) {
          tbl_has_symref = 1;
          break;
        }
      }
      Section *tbl_sec =
          (tbl_has_symref && tcc_state->share_rodata) ? data_section : rodata_section;
      size_t tbl_off = section_add(tbl_sec, tbl_bytes, 4);
      unsigned char *tbl = (unsigned char *)(tbl_sec->data + tbl_off);
      for (int k = 0; k < vtab->num_entries; k++) {
        IROperand v = vtab->values[k];
        uint32_t val = 0;
        if (v.tag == IROP_TAG_IMM32) {
          val = (uint32_t)v.u.imm32;
        } else if (v.tag == IROP_TAG_SYMREF) {
          IRPoolSymref *sr = &ir->pool_symref[v.u.pool_idx];
          greloc(tbl_sec, sr->sym, (unsigned long)(tbl_off + k * 4), R_ARM_ABS32);
          val = (uint32_t)sr->addend;
        } else {
          tcc_error("internal error: SWITCH_LOAD table entry has unsupported tag %d", (int)v.tag);
        }
        tbl[k * 4 + 0] = (unsigned char)(val & 0xff);
        tbl[k * 4 + 1] = (unsigned char)((val >> 8) & 0xff);
        tbl[k * 4 + 2] = (unsigned char)((val >> 16) & 0xff);
        tbl[k * 4 + 3] = (unsigned char)((val >> 24) & 0xff);
      }
      vtab->rodata_sym = get_sym_ref(&int_type, tbl_sec, tbl_off, tbl_bytes);
    }

    /* Indices may repeat across cases (post-DCE fall-through paths share one ASSIGN); re-NOPing is safe. */
    /* A body shared with default_target must survive: the out-of-range JUMPIF still branches to it. */
    for (int k = 0; k < table->num_entries; k++) {
      int preserved = 0;
      for (int m = 0; m < table->num_entries; m++) {
        if (table->targets[m] == table->default_target &&
            (probe_assign[m] == probe_assign[k] || probe_jump[m] == probe_jump[k])) {
          preserved = 1;
          break;
        }
      }
      if (preserved)
        continue;
      ir->compact_instructions[probe_assign[k]].op = TCCIR_OP_NOP;
      ir->compact_instructions[probe_jump[k]].op = TCCIR_OP_NOP;
    }

    /* STORE form: the join's read of the merge slot is the SWITCH_LOAD dest now. */
    if (join_load_idx >= 0)
      ir->compact_instructions[join_load_idx].op = TCCIR_OP_NOP;
    if (default_store_idx >= 0)
      ir->compact_instructions[default_store_idx].op = TCCIR_OP_NOP;

    if (jump_slot >= 0)
    {
      ir->compact_instructions[jump_slot].op = TCCIR_OP_NOP;
      write_instr_at_nop(ir, jump_slot, TCCIR_OP_JUMP, irop_make_imm32(-1, common_merge, IROP_BTYPE_INT32),
                         IROP_NONE, IROP_NONE);
    }

    /* The old jump table's slot is kept (num_switch_tables intact) so existing indices stay valid. */

    changes++;
    refs_valid = 0;
  }

  if (changes > 0)
    LOG_IR_GEN("switch_to_data: rewrote %d SWITCH_TABLE(s) into SWITCH_LOAD", changes);

  tcc_free(refs);
  tcc_free(probe_assign);
  tcc_free(probe_jump);
  tcc_free(probe_val);
  return changes;
}
