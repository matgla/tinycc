/*
 *  TCC IR - Global Symbol Address CSE
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "licm.h"
#include "memory/vector.h"
#include "opt_engine.h"
#include "opt_dsl.h"
#include "opt/flat/symaddr_cse.h"

#define SYMADDR_CSE_MAX_HOISTS 16

typedef struct {
  Sym *sym;
  int64_t addend;
  int count;
  int has_lval;
  int selected;
  int32_t hoist_vreg;
} SymaddrEntry;

typedef struct {
  SymaddrEntry *entries;
  int num_entries;
  int num_hoist;
  int entry_end;
} SymaddrState;

TCC_VECTOR_DEFINE(SymaddrEntryVector, SymaddrEntry)

extern int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q);

static int symaddr_count(TCCIRState *ir, IROperand op, SymaddrEntryVector *entries)
{
  if (irop_get_tag(op) != IROP_TAG_SYMREF)
    return 0;
  IRPoolSymref *sr = irop_get_symref_ex(ir, op);
  if (!sr || !sr->sym)
    return 0;
  for (size_t i = 0; i < entries->size; i++) {
    SymaddrEntry *entry = &entries->data[i];
    if (entry->sym != sr->sym || entry->addend != sr->addend)
      continue;
    if (op.is_lval)
      entry->has_lval = 1;
    else
      entry->count++;
    return 0;
  }
  SymaddrEntry entry = {0};
  entry.sym = sr->sym;
  entry.addend = sr->addend;
  entry.has_lval = op.is_lval;
  entry.count = op.is_lval ? 0 : 1;
  return SymaddrEntryVector_push_back(entries, entry);
}

static int symaddr_collect(TCCIRState *ir, SymaddrEntryVector *entries)
{
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_ADD) {
      if (symaddr_count(ir, tcc_ir_op_get_src1(ir, q), entries) < 0 ||
          symaddr_count(ir, tcc_ir_op_get_src2(ir, q), entries) < 0)
        return -1;
      continue;
    }
    if (q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_STORE &&
        q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;
    if (irop_config[q->op].has_dest &&
        symaddr_count(ir, tcc_ir_op_get_dest(ir, q), entries) < 0)
      return -1;
    if (irop_config[q->op].has_src1 &&
        symaddr_count(ir, tcc_ir_op_get_src1(ir, q), entries) < 0)
      return -1;
  }
  return 0;
}

static void symaddr_sort(SymaddrEntry *entries, int num_entries)
{
  for (int i = 0; i < num_entries - 1; i++) {
    for (int j = i + 1; j < num_entries; j++) {
      if (entries[j].count <= entries[i].count)
        continue;
      SymaddrEntry tmp = entries[i];
      entries[i] = entries[j];
      entries[j] = tmp;
    }
  }
}

static int symaddr_select(TCCIRState *ir, SymaddrEntry *entries, int num_entries)
{
  int limit = tcc_ir_estimate_hoist_budget(ir, 0, ir->next_instruction_index - 1,
                                           ir->parameters_count);
  int num_hoist = 0;
  if (limit < 2)
    limit = 2;
  if (limit > SYMADDR_CSE_MAX_HOISTS)
    limit = SYMADDR_CSE_MAX_HOISTS;
  symaddr_sort(entries, num_entries);
  for (int i = 0; i < num_entries && num_hoist < limit; i++) {
    if (entries[i].count < 3 || entries[i].has_lval)
      continue;
    int32_t vreg = tcc_ir_vreg_alloc_temp(ir);
    if (vreg < 0)
      continue;
    entries[i].selected = 1;
    entries[i].hoist_vreg = vreg;
    num_hoist++;
  }
  return num_hoist;
}

static int symaddr_insert_defs(TCCIRState *ir, SymaddrEntry *entries, int num_entries)
{
  int slot = 0;
  for (int i = 0; i < num_entries; i++) {
    SymaddrEntry *entry = &entries[i];
    if (!entry->selected)
      continue;
    uint32_t pool_idx = tcc_ir_pool_add_symref(ir, entry->sym, (int32_t)entry->addend, 0);
    IROperand src = irop_make_symref(-1, pool_idx, 0, 0, 0, IROP_BTYPE_INT32);
    IROperand dest = irop_make_vreg(entry->hoist_vreg, IROP_BTYPE_INT32);
    IRQuadCompact assign = {0};
    assign.op = TCCIR_OP_ASSIGN;
    assign.operand_base = tcc_ir_pool_add(ir, dest);
    tcc_ir_pool_add(ir, src);
    if (gsym_cse_insert_before(ir, slot, &assign) < 0)
      return -1;
    slot++;
  }
  return 0;
}

static SymaddrEntry *symaddr_find_exact(SymaddrState *st, TCCIRState *ir,
                                        IROperand op)
{
  if (irop_get_tag(op) != IROP_TAG_SYMREF || op.is_lval)
    return NULL;
  IRPoolSymref *sr = irop_get_symref_ex(ir, op);
  if (!sr || !sr->sym)
    return NULL;
  for (int i = 0; i < st->num_entries; i++) {
    SymaddrEntry *entry = &st->entries[i];
    if (entry->selected && entry->sym == sr->sym && entry->addend == sr->addend)
      return entry;
  }
  return NULL;
}

static int symaddr_entry_end(TCCIRState *ir, int num_hoist)
{
  for (int i = num_hoist; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (i > num_hoist && q->is_jump_target)
      return i;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
        q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      return i;
  }
  return ir->next_instruction_index;
}

OPT_GEN_FLAT(symaddr_add, TCCIR_OP_ADD)
{
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_ANY,
                           .src2 = IR_CONSTRAINT_ANY });
  SymaddrState *st = ctx->pass_state;
  SymaddrEntry *entry = symaddr_find_exact(st, ir, src1);
  if (entry)
    REWRITE(.src1 = irop_make_vreg(entry->hoist_vreg, IROP_BTYPE_INT32));
  entry = symaddr_find_exact(st, ir, src2);
  GUARD(when(entry != NULL));
  REWRITE(.src2 = irop_make_vreg(entry->hoist_vreg, IROP_BTYPE_INT32));
}

OPT_GEN_FLAT(symaddr_store, TCCIR_OP_STORE)
{
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY,
                           .src1 = IR_CONSTRAINT_ANY });
  SymaddrState *st = ctx->pass_state;
  if (i < st->num_hoist || i >= st->entry_end ||
      irop_get_tag(dest) != IROP_TAG_SYMREF || !dest.is_lval || dest.is_local ||
      dest.btype == IROP_BTYPE_INT64 || dest.btype == IROP_BTYPE_FLOAT64 ||
      dest.btype == IROP_BTYPE_STRUCT)
    return 0;
  IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
  if (!sr || !sr->sym)
    return 0;
  SymaddrEntry *base = NULL;
  int64_t delta = 0;
  for (int j = 0; j < st->num_entries; j++) {
    SymaddrEntry *entry = &st->entries[j];
    if (!entry->selected || entry->sym != sr->sym)
      continue;
    delta = (int64_t)sr->addend - entry->addend;
    if (delta < -255 || delta > 4095)
      continue;
    base = entry;
    break;
  }
  GUARD(when(base != NULL));
  tcc_ir_pool_ensure(ir, 4);
  int operand_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(base->hoist_vreg, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, src1);
  tcc_ir_pool_add(ir, mk_imm(delta));
  tcc_ir_pool_add(ir, mk_imm(0));
  q->operand_base = operand_base;
  REWRITE(.new_op = TCCIR_OP_STORE_INDEXED);
}

const IROptGen symaddr_cse_gens[] = {
    OPT_GEN_ENTRY_FLAT(symaddr_add, TCCIR_OP_ADD),
    OPT_GEN_ENTRY_FLAT(symaddr_store, TCCIR_OP_STORE),
};

const int symaddr_cse_gens_count = OPT_DSL_TABLE_COUNT(symaddr_cse_gens);

int tcc_ir_opt_symaddr_cse_ex(IROptCtx *ctx)
{
  if (!ctx || !ctx->ir || ctx->ir->next_instruction_index == 0)
    return 0;
  TCCIRState *ir = ctx->ir;
  scoped_named_vector(SymaddrEntryVector) entries = {0};
  if (symaddr_collect(ir, &entries) < 0)
    return 0;
  int num_hoist = symaddr_select(ir, entries.data, (int)entries.size);
  if (num_hoist == 0 || symaddr_insert_defs(ir, entries.data, (int)entries.size) < 0)
    return 0;
  SymaddrState state = {
      entries.data,
      (int)entries.size,
      num_hoist,
      symaddr_entry_end(ir, num_hoist),
  };
  void *old_state = ctx->pass_state;
  ctx->pass_state = &state;
  int changes = tcc_ir_opt_run_gens(ctx, symaddr_cse_gens,
                                    symaddr_cse_gens_count);
  ctx->pass_state = old_state;
  return changes;
}

int tcc_ir_opt_symaddr_cse(TCCIRState *ir)
{
  if (!ir)
    return 0;
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_symaddr_cse_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}
