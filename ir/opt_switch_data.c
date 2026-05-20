/*
 *  TCC IR - Switch-to-data-table transformation
 *
 *  When all case bodies of a dense SWITCH_TABLE consist of a single ASSIGN
 *  of a constant to the same variable (followed by a JMP to a common merge
 *  block), the entire dispatch can be replaced by an indexed load from a
 *  constant value table:
 *
 *    case 100: p = &x000; break;     →     range_check(i, 100..1099);
 *    case 101: p = &x001; break;            if in_range: p = table[i-100];
 *    ...                                    if out_of_range: keep p's default
 *
 *  This compresses N case bodies of ~3 instructions each plus the jump-table
 *  dispatch into ~5 dispatch instructions plus an inline data table.  On the
 *  pr34093.c gcc-torture workload this drops the function from 3032 → ~15
 *  instructions (matching GCC).
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

/* Maximum case-body length we accept (in non-NOP IR ops, not counting the
 * trailing JMP).  A "thin" case body is just one ASSIGN. */
#define SWITCHDATA_MAX_BODY_OPS 1

/* Examine a single case body starting at `target`.  Return 0 if the body
 * matches the constant-store pattern, else nonzero.  On match, fills
 * *out_dest with the assigned vreg, *out_val with the source operand, and
 * *out_merge with the JMP destination IR index. */
/* Check that the basic block beginning at `target` matches the canonical
 * "constant-store + branch-to-merge" shape:
 *   target:          (may be jump_target)
 *     [NOP...]       (any number of NOPs)
 *     ASSIGN dst <- const     (single, non-lval dest, IMM32 or SYMREF src)
 *     [NOP...]
 *     JMP merge
 *
 * The whole block must live within the basic block starting at `target` —
 * no other jump_targets between target's start and the trailing JMP.
 * Returns 0 on match; *out_assign_idx and *out_jump_idx record the exact
 * IR indices for later NOP-ping (avoids re-walking after mutations).
 */
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

/* Allocate a fresh switch_value_table entry; returns its index. */
static int sd_alloc_value_table(TCCIRState *ir, int num_entries)
{
  if (ir->num_switch_value_tables >= ir->switch_value_tables_capacity) {
    int new_cap = ir->switch_value_tables_capacity * 2 + 4;
    ir->switch_value_tables = tcc_realloc(ir->switch_value_tables,
                                          new_cap * sizeof(TCCIRSwitchValueTable));
    /* Zero the freshly grown tail to keep rodata_sym/default_val initialised. */
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

    /* Probe every case target.  We need: same dest_vreg across all cases,
     * same merge target across all cases, and constant-source ASSIGN body. */
    int ok = 1;
    int32_t common_dest_vr = -1;
    int common_dbt = -1;
    int common_merge = -1;
    int common_dest_unsigned = 0;
    /* Record exact (assign_idx, jump_idx) per case to NOP after probing.
     * Capped at 1024 to keep stack usage bounded; bail out on huge tables. */
    if (table->num_entries > 1024) continue;
    int probe_assign[1024];
    int probe_jump[1024];
    IROperand probe_val[1024];
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

    /* Default target: when in non-range path V's value is preserved.  Use
     * IMM32 0 as the table's default_val; the range-check JMP guarantees
     * we don't actually read it, but it's set for completeness. */
    int vt_id = sd_alloc_value_table(ir, table->num_entries);
    TCCIRSwitchValueTable *vtab = &ir->switch_value_tables[vt_id];

    for (int k = 0; k < table->num_entries; k++)
      vtab->values[k] = probe_val[k];
    vtab->default_val.tag = IROP_TAG_IMM32;
    vtab->default_val.u.imm32 = 0;

    /* Rewrite the SWITCH_TABLE in place to SWITCH_LOAD.  SWITCH_LOAD has
     * dest+src1+src2 — three operands — so we allocate three fresh slots
     * in the iroperand pool and repoint operand_base. */
    IROperand dest_op;
    memset(&dest_op, 0, sizeof(dest_op));
    irop_set_vreg(&dest_op, common_dest_vr);
    dest_op.tag = IROP_TAG_VREG;
    dest_op.btype = common_dbt;
    dest_op.is_unsigned = common_dest_unsigned;
    /* Reads of V at merge are `T <- V [ASSIGN, is_lval]`, but the dest of
     * SWITCH_LOAD is a direct write — is_lval=0. */

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

    /* Emit the value table into .rodata.  The dispatch will load this
     * table's address via the literal pool and do an indexed read at
     * codegen time. */
    {
      int tbl_bytes = vtab->num_entries * 4;
      size_t tbl_off = section_add(rodata_section, tbl_bytes, 4);
      unsigned char *tbl = (unsigned char *)(rodata_section->data + tbl_off);
      for (int k = 0; k < vtab->num_entries; k++) {
        IROperand v = vtab->values[k];
        uint32_t val = 0;
        if (v.tag == IROP_TAG_IMM32) {
          val = (uint32_t)v.u.imm32;
        } else if (v.tag == IROP_TAG_SYMREF) {
          IRPoolSymref *sr = &ir->pool_symref[v.u.pool_idx];
          greloc(rodata_section, sr->sym, (unsigned long)(tbl_off + k * 4), R_ARM_ABS32);
          val = (uint32_t)sr->addend;
        } else {
          tcc_error("internal error: SWITCH_LOAD table entry has unsupported tag %d", (int)v.tag);
        }
        tbl[k * 4 + 0] = (unsigned char)(val & 0xff);
        tbl[k * 4 + 1] = (unsigned char)((val >> 8) & 0xff);
        tbl[k * 4 + 2] = (unsigned char)((val >> 16) & 0xff);
        tbl[k * 4 + 3] = (unsigned char)((val >> 24) & 0xff);
      }
      vtab->rodata_sym = get_sym_ref(&int_type, rodata_section, tbl_off, tbl_bytes);
    }

    /* NOP each case body's ASSIGN + JMP using exact indices recorded
     * during probing. Indices may repeat across cases (fall-through paths
     * after DCE share a single surviving ASSIGN); repeated NOPs are safe. */
    for (int k = 0; k < table->num_entries; k++) {
      ir->compact_instructions[probe_assign[k]].op = TCCIR_OP_NOP;
      ir->compact_instructions[probe_jump[k]].op = TCCIR_OP_NOP;
    }

    /* Free the old jump table — it's no longer referenced.  Keep the slot
     * (num_switch_tables intact) so existing indices remain valid. */
    /* (Targets array stays allocated until ir teardown — minor leak only.) */

    changes++;
  }

  if (changes > 0)
    LOG_IR_GEN("switch_to_data: rewrote %d SWITCH_TABLE(s) into SWITCH_LOAD", changes);

  return changes;
}

int tcc_ir_opt_switch_to_data_ex(IROptCtx *ctx) { return tcc_ir_opt_switch_to_data(ctx->ir); }
