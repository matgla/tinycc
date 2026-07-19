/*
 *  TCC IR - Global base-address sharing (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include <limits.h>

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "opt_loop_utils.h"


/* Cluster consecutive STOREs to same-section globals into one LEA base plus
 * STORE_INDEXED offset stores, removing per-store symbol-address loads. */

#define GBS_MAX_CLUSTER 16
#define GBS_DELTA_MIN  (-1020)
#define GBS_DELTA_MAX  (1020)

/* Symref for a STORE with SYMREF-deref dest, NULL on mismatch. STORE width comes
 * from dest.btype but STORE_INDEXED from value.btype, so reject mismatched btypes. */
static IRPoolSymref *gbs_get_store_symref(TCCIRState *ir, IRQuadCompact *q, ElfSym **out_esym)
{
  if (q->op != TCCIR_OP_STORE)
    return NULL;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (irop_get_tag(dest) != IROP_TAG_SYMREF || !dest.is_lval)
    return NULL;
  /* Only INT32/INT64/float; dest and src btype must match to avoid narrowing within the STORE. */
  IROperand src = tcc_ir_op_get_src1(ir, q);
  int dest_bt = irop_get_btype(dest);
  int src_bt = irop_get_btype(src);
  if (dest_bt != src_bt)
    return NULL;
  if (dest_bt != IROP_BTYPE_INT32 && dest_bt != IROP_BTYPE_INT64 &&
      dest_bt != IROP_BTYPE_FLOAT32 && dest_bt != IROP_BTYPE_FLOAT64)
    return NULL;
  IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
  if (!sr || !sr->sym)
    return NULL;
  if (sr->addend != 0)
    return NULL;
  Sym *sym = sr->sym;
  if (sym->type.t & VT_VOLATILE)
    return NULL;
  /* Struct globals mix access widths (bitfield RMW); skip. */
  if ((sym->type.t & VT_BTYPE) == VT_STRUCT)
    return NULL;
  ElfSym *esym = elfsym(sym);
  if (!esym)
    return NULL;
  if (esym->st_shndx == SHN_UNDEF || esym->st_shndx >= (unsigned)tcc_state->nb_sections)
    return NULL;
  /* SHN_COMMON / SHN_ABS aren't real section indices */
  if (esym->st_shndx == SHN_COMMON || esym->st_shndx == SHN_ABS)
    return NULL;
  unsigned char bind = ELFW(ST_BIND)(esym->st_info);
  if (bind == STB_WEAK)
    return NULL;
  Section *sec = tcc_state->sections[esym->st_shndx];
  if (!sec || !(sec->sh_flags & SHF_ALLOC) || !(sec->sh_flags & SHF_WRITE))
    return NULL;
  *out_esym = esym;
  return sr;
}

int tcc_ir_opt_global_base_share(TCCIRState *ir)
{
  if (!ir || !tcc_state)
    return 0;
  if (!tcc_state->opt_indexed_memory)
    return 0;

  int n = ir->next_instruction_index;
  int changes = 0;

  /* Indirect jumps can target any IR position; inserting a LEA before a labeled
   * STORE would break orig_ir_to_code_mapping. Disable for such functions. */
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
      return 0;
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    /* Skip jump-target STOREs — base wouldn't be live across the merge. */
    if (q->is_jump_target)
      continue;

    ElfSym *anchor_esym = NULL;
    IRPoolSymref *anchor_sr = gbs_get_store_symref(ir, q, &anchor_esym);
    if (!anchor_sr)
      continue;

    int64_t anchor_base = (int64_t)anchor_esym->st_value + anchor_sr->addend;

    int cluster_stores[GBS_MAX_CLUSTER];
    int64_t cluster_deltas[GBS_MAX_CLUSTER];
    int cluster_size = 0;
    cluster_stores[cluster_size] = i;
    cluster_deltas[cluster_size] = 0;
    cluster_size++;

    int last_in_cluster = i;
    for (int j = i + 1; j < n && cluster_size < GBS_MAX_CLUSTER; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;

      /* Control flow or call breaks the cluster — base reg may not survive. */
      if (qj->op == TCCIR_OP_JUMP || qj->op == TCCIR_OP_JUMPIF ||
          qj->op == TCCIR_OP_IJUMP || qj->op == TCCIR_OP_SWITCH_TABLE ||
          qj->op == TCCIR_OP_SWITCH_LOAD ||
          qj->op == TCCIR_OP_FUNCCALLVOID || qj->op == TCCIR_OP_FUNCCALLVAL ||
          qj->op == TCCIR_OP_FUNCPARAMVAL || qj->op == TCCIR_OP_FUNCPARAMVOID ||
          qj->op == TCCIR_OP_RETURNVALUE || qj->op == TCCIR_OP_RETURNVOID ||
          qj->op == TCCIR_OP_INLINE_ASM)
        break;
      if (qj->is_jump_target)
        break;

      /* Non-STORE ops between are fine; RA manages base-reg liveness. */
      if (qj->op != TCCIR_OP_STORE)
        continue;

      ElfSym *ej = NULL;
      IRPoolSymref *sj = gbs_get_store_symref(ir, qj, &ej);
      if (!sj)
        continue;
      if (ej->st_shndx != anchor_esym->st_shndx)
        continue;
      int64_t addr = (int64_t)ej->st_value + sj->addend;
      int64_t delta = addr - anchor_base;
      if (delta < GBS_DELTA_MIN || delta > GBS_DELTA_MAX)
        continue;
      if (delta & 3)
        continue; /* require 4-byte alignment for STRD compatibility */

      cluster_stores[cluster_size] = j;
      cluster_deltas[cluster_size] = delta;
      cluster_size++;
      last_in_cluster = j;
    }

    if (cluster_size < 2)
      continue;

    int32_t base_vreg = tcc_ir_vreg_alloc_temp(ir);
    if (base_vreg < 0)
      continue;

    uint32_t sym_pool = tcc_ir_pool_add_symref(ir, anchor_sr->sym,
                                               (int32_t)anchor_sr->addend, anchor_sr->flags);
    IROperand lea_src = irop_make_symref(-1, sym_pool, 0 /* is_lval */, 0 /* is_local */,
                                          0 /* is_const */, IROP_BTYPE_INT32);
    IROperand lea_dest = irop_make_vreg(base_vreg, IROP_BTYPE_INT32);
    IROperand null_op = {0};

    int inserted = insert_instr_at(ir, i, TCCIR_OP_LEA, lea_dest, lea_src, null_op);
    if (inserted < 0)
      continue;
    /* All cluster indices shifted +1 after insertion. */
    for (int k = 0; k < cluster_size; k++)
      cluster_stores[k] += 1;
    n = ir->next_instruction_index;
    last_in_cluster += 1;

    for (int k = 0; k < cluster_size; k++)
    {
      int sidx = cluster_stores[k];
      IRQuadCompact *sq = &ir->compact_instructions[sidx];
      IROperand st_src = tcc_ir_op_get_src1(ir, sq);
      int64_t delta = cluster_deltas[k];

      tcc_ir_pool_ensure(ir, 4);
      int new_base = ir->iroperand_pool_count;

      IROperand st_base = irop_make_vreg(base_vreg, IROP_BTYPE_INT32);
      st_base.is_lval = 0;
      tcc_ir_pool_add(ir, st_base);
      tcc_ir_pool_add(ir, st_src);
      tcc_ir_pool_add(ir, irop_make_imm32(-1, (int32_t)delta, IROP_BTYPE_INT32));
      tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));

      sq->op = TCCIR_OP_STORE_INDEXED;
      sq->operand_base = new_base;
    }

    LOG_IR_GEN("GLOBAL_BASE_SHARE: cluster of %d stores starting at i=%d (after LEA insertion at i=%d)",
               cluster_size, i + 1, i);

    changes++;
    /* Skip past the cluster. */
    i = last_in_cluster;
  }

  return changes;
}

int tcc_ir_opt_global_base_share_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_global_base_share(ctx->ir);
}
