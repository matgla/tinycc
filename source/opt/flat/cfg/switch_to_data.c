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
  if (qi->op != TCCIR_OP_ASSIGN)
    return 1;

  IROperand dest = tcc_ir_op_get_dest(ir, qi);
  IROperand src = tcc_ir_op_get_src1(ir, qi);
  if (dest.is_lval)
    return 1;
  int tag = src.tag;
  if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_SYMREF)
    return 1;
  if (src.is_lval || src.is_llocal)
    return 1;

  int dbt = irop_get_btype(dest);
  int sbt = irop_get_btype(src);
  if (dbt != IROP_BTYPE_INT32 && dbt != IROP_BTYPE_FUNC)
    return 1;
  if (sbt != IROP_BTYPE_INT32 && sbt != IROP_BTYPE_FUNC)
    return 1;

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
  IROperand jd = tcc_ir_op_get_dest(ir, jq);
  int merge = (int)irop_get_imm64_ex(ir, jd);
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

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_SWITCH_TABLE)
      continue;

    IROperand idx_op = tcc_ir_op_get_src1(ir, q);
    IROperand tid_op = tcc_ir_op_get_src2(ir, q);
    int table_id = (int)irop_get_imm64_ex(ir, tid_op);
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
    for (int k = 0; k < table->num_entries; k++) {
      IROperand d, v;
      int merge, aidx, jidx;
      if (sd_check_case_body(ir, table->targets[k], &d, &v, &merge, &aidx, &jidx)) {
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
      probe_assign[k] = aidx;
      probe_jump[k] = jidx;
      probe_val[k] = v;
    }
    if (!ok)
      continue;
    if (common_dest_vr < 0)
      continue;

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

    /* The old jump table's slot is kept (num_switch_tables intact) so existing indices stay valid. */

    changes++;
  }

  if (changes > 0)
    LOG_IR_GEN("switch_to_data: rewrote %d SWITCH_TABLE(s) into SWITCH_LOAD", changes);

  tcc_free(probe_assign);
  tcc_free(probe_jump);
  tcc_free(probe_val);
  return changes;
}

int tcc_ir_opt_switch_to_data_ex(IROptCtx *ctx) { return tcc_ir_opt_switch_to_data(ctx->ir); }
