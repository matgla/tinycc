/*
 *  TCC IR - Dead static-store elimination (flat, pre-SSA)
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


/* Eliminate stores to file-scope statics with no TU readers (late_reopt only). */

static Sym *dss_symref_sym(TCCIRState *ir, IROperand op)
{
  IRPoolSymref *ref = irop_get_symref_ex(ir, op);
  return ref ? ref->sym : NULL;
}

/* vreg->sym forward-trace map: resolves STORE dests reached through temps. */
#define DSS_VREG_MAP_MAX 128
typedef struct
{
  int32_t vreg;
  Sym *sym;
} DssVregEntry;

static Sym *dss_vreg_map_lookup(const DssVregEntry *map, int count, int32_t vr)
{
  for (int i = 0; i < count; i++)
    if (map[i].vreg == vr)
      return map[i].sym;
  return NULL;
}

static void dss_vreg_map_set(DssVregEntry *map, int *count, int32_t vr, Sym *sym)
{
  for (int i = 0; i < *count; i++)
  {
    if (map[i].vreg == vr)
    {
      map[i].sym = sym;
      return;
    }
  }
  if (*count < DSS_VREG_MAP_MAX)
  {
    map[*count].vreg = vr;
    map[*count].sym = sym;
    (*count)++;
  }
}

static void dss_vreg_map_clear(DssVregEntry *map, int *count, int32_t vr)
{
  for (int i = 0; i < *count; i++)
  {
    if (map[i].vreg == vr)
    {
      map[i].sym = NULL;
      return;
    }
  }
}

static void dss_build_vreg_map(TCCIRState *ir, DssVregEntry *map, int *count)
{
  const int n = ir->next_instruction_index;
  *count = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (dvr < 0 || dest.is_lval)
      continue;

    Sym *derived_sym = NULL;
    if (irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (s1.is_sym && !s1.is_lval)
        derived_sym = dss_symref_sym(ir, s1);
    }
    if (!derived_sym && (q->op == TCCIR_OP_MLA))
    {
      IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (acc.is_sym && !acc.is_lval)
        derived_sym = dss_symref_sym(ir, acc);
    }
    if (!derived_sym &&
        (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD ||
         q->op == TCCIR_OP_SUB))
    {
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        int32_t svr = irop_get_vreg(s1);
        if (svr >= 0 && !s1.is_sym)
          derived_sym = dss_vreg_map_lookup(map, *count, svr);
      }
    }
    if (!derived_sym && (q->op == TCCIR_OP_MLA))
    {
      IROperand acc = tcc_ir_op_get_accum(ir, q);
      int32_t avr = irop_get_vreg(acc);
      if (avr >= 0 && !acc.is_sym)
        derived_sym = dss_vreg_map_lookup(map, *count, avr);
    }

    if (derived_sym)
      dss_vreg_map_set(map, count, dvr, derived_sym);
    else
      dss_vreg_map_clear(map, count, dvr);
  }
}

static Sym *dss_resolve_store_dest_sym(TCCIRState *ir, IRQuadCompact *q,
                                       int store_idx,
                                       const DssVregEntry *vreg_map,
                                       int vreg_map_count)
{
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  if (dest.is_sym)
  {
    /* STORE requires an lval dest; INDEXED/POSTINC may have lval cleared. */
    if (!dest.is_lval && q->op == TCCIR_OP_STORE)
      return NULL;
    return dss_symref_sym(ir, dest);
  }

  /* Indirect TEMP-DEREF form for plain STORE. */
  if (q->op == TCCIR_OP_STORE && dest.is_lval)
  {
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return NULL;

    /* Bail if the TEMP is multiply defined. */
    int def_idx = -1;
    int def_count = 0;
    for (int j = 0; j < ir->next_instruction_index; j++)
    {
      IRQuadCompact *dq = &ir->compact_instructions[j];
      if (dq->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[dq->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, dq);
      if (d.is_lval)
        continue;
      if (irop_get_vreg(d) == vr &&
          TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP)
      {
        def_idx = j;
        def_count++;
        if (def_count > 1)
          return NULL;
      }
    }
    if (def_idx < 0 || def_count != 1)
      return NULL;

    IRQuadCompact *dq = &ir->compact_instructions[def_idx];
    if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_LEA &&
        dq->op != TCCIR_OP_ASSIGN)
      return NULL;
    IROperand s1 = tcc_ir_op_get_src1(ir, dq);
    if (!s1.is_sym || s1.is_lval)
      return NULL;
    return dss_symref_sym(ir, s1);
  }

  /* STORE_INDEXED / STORE_POSTINC with temp dest: resolve via the vreg map. */
  if (vreg_map)
  {
    int32_t dvr = irop_get_vreg(dest);
    if (dvr >= 0)
      return dss_vreg_map_lookup(vreg_map, vreg_map_count, dvr);
  }

  return NULL;
}

int tcc_ir_opt_dead_static_store_elim(TCCIRState *ir)
{
  if (!ir || !tcc_state)
    return 0;
  /* tu_no_readers is only set at end-of-TU, so this is a no-op earlier. */
  if (!tcc_state->ir_late_reopt_phase)
    return 0;

  const int n = ir->next_instruction_index;
  int changes = 0;

  DssVregEntry vreg_map[DSS_VREG_MAP_MAX];
  int vreg_map_count = 0;
  dss_build_vreg_map(ir, vreg_map, &vreg_map_count);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
        q->op != TCCIR_OP_STORE_POSTINC)
      continue;

    Sym *sym = dss_resolve_store_dest_sym(ir, q, i, vreg_map, vreg_map_count);
    if (!sym)
      continue;
    if (!sym->a.tu_no_readers)
      continue;
    if (sym->a.addrtaken)
      continue;
    /* Volatile stores stay observable even with no C-level reader. */
    if (sym->type.t & VT_VOLATILE)
      continue;

    LOG_IR_GEN("DEAD_STATIC_STORE: NOPed STORE at i=%d -> %s", i,
               get_tok_str(sym->v & ~SYM_FIELD, NULL));

    q->op = TCCIR_OP_NOP;
    changes++;
  }

  return changes;
}

int tcc_ir_opt_dead_static_store_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_static_store_elim(ctx->ir);
}
