/*
 *  TCC IR - Dead Trailing Addr-Var Store Elimination
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_xform.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "opt_range.h"
#include "cfg.h"
#include "licm.h"

static void mark_var_escaped(uint8_t *m, int v)
{
  m[v / 8] |= (1 << (v % 8));
}

static int var_is_escaped(const uint8_t *m, int v)
{
  return m[v / 8] & (1 << (v % 8));
}

/* Which local's address a TEMP or VAR vreg may hold (pt, -1 = none), and
 * whether every def of it gives exactly that address (inexact = 0). */
typedef struct DtaPtrs
{
  int *tmp_pt, *var_pt;
  uint8_t *tmp_inexact, *var_inexact;
  int max_tmp, max_var;
} DtaPtrs;

/* Slot index for vreg vr, or NULL when it is not a TEMP/VAR in range. */
static int *dta_pt(DtaPtrs *P, int32_t vr, uint8_t **inexact)
{
  if (vr < 0)
    return NULL;
  int t = TCCIR_DECODE_VREG_TYPE(vr);
  int p = TCCIR_DECODE_VREG_POSITION(vr);
  if (t == TCCIR_VREG_TYPE_TEMP && p <= P->max_tmp)
  {
    *inexact = &P->tmp_inexact[p];
    return &P->tmp_pt[p];
  }
  if (t == TCCIR_VREG_TYPE_VAR && p <= P->max_var)
  {
    *inexact = &P->var_inexact[p];
    return &P->var_pt[p];
  }
  return NULL;
}

static int dta_target(DtaPtrs *P, int32_t vr)
{
  uint8_t *ix;
  int *pt = dta_pt(P, vr, &ix);
  return pt ? *pt : -1;
}

static int dta_exact(DtaPtrs *P, int32_t vr)
{
  uint8_t *ix;
  int *pt = dta_pt(P, vr, &ix);
  return pt && *pt >= 0 && !*ix;
}

/* A STORE-family DEST names memory (a TEMP base, or any lval); a VAR DEST
 * of a plain STORE without lval is that VAR's own def. */
static int dta_dest_is_mem(const TCCIRState *ir, const IRQuadCompact *q)
{
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (d.is_lval)
    return 1;
  if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    return 1;
  int32_t vr = irop_get_vreg(d);
  return q->op == TCCIR_OP_STORE && vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP;
}

/* `D <- S` that copies a pointer value from vreg S into vreg D. */
static int dta_is_copy(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_STORE)
    return 0;
  if (dta_dest_is_mem(ir, q) || tcc_ir_op_src1_is_lval(ir, q))
    return 0;
  int32_t d = tcc_ir_op_dest_vreg(ir, q);
  if (d < 0)
    return 0;
  int t = TCCIR_DECODE_VREG_TYPE(d);
  return t == TCCIR_VREG_TYPE_TEMP || t == TCCIR_VREG_TYPE_VAR;
}

/* One sweep of the points-to facts; returns whether anything changed. */
static int dta_propagate(TCCIRState *ir, int n, DtaPtrs *P, uint8_t *var_escaped)
{
  int changed = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (ir_op_has(q->op, IR_HZ_UPDATES_SRC))
    {
      /* POSTINC advances its pointer operand: one more def of it. */
      IROperand ops[3] = {tcc_ir_op_get_dest(ir, q), tcc_ir_op_get_src1(ir, q), tcc_ir_op_get_src2(ir, q)};
      for (int k = 0; k < 3; k++)
      {
        uint8_t *ix;
        if (dta_pt(P, irop_get_vreg(ops[k]), &ix) && !*ix)
          changed = *ix = 1;
      }
    }
    if (!irop_config[q->op].has_dest || dta_dest_is_mem(ir, q))
      continue;
    uint8_t *d_ix;
    int *d_pt = dta_pt(P, tcc_ir_op_dest_vreg(ir, q), &d_ix);
    if (!d_pt)
      continue;
    int target = -1, exact = 0;
    if (q->op == TCCIR_OP_LEA)
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t s_vr = irop_get_vreg(s);
      if (!s.is_lval && s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(s_vr) <= P->max_var)
      {
        target = TCCIR_DECODE_VREG_POSITION(s_vr);
        exact = 1;
      }
    }
    else if (dta_is_copy(ir, q))
    {
      int32_t s_vr = tcc_ir_op_src1_vreg(ir, q);
      target = dta_target(P, s_vr);
      exact = dta_exact(P, s_vr);
    }
    if (target < 0)
    {
      /* Some other def: whatever address it may also hold, it is not only that. */
      if (!*d_ix)
        changed = *d_ix = 1;
      continue;
    }
    if (*d_pt < 0)
    {
      *d_pt = target;
      changed = 1;
    }
    else if (*d_pt != target)
    {
      mark_var_escaped(var_escaped, *d_pt);
      mark_var_escaped(var_escaped, target);
    }
    if (!exact && !*d_ix)
      changed = *d_ix = 1;
  }
  return changed;
}

int tcc_ir_opt_dead_trailing_addrvar_store_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID ||
        op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID ||
        op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_NL_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_SET_CHAIN ||
        op == TCCIR_OP_INIT_CHAIN_SLOT || op == TCCIR_OP_SWITCH_TABLE)
      return 0;
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int target = (int)tcc_ir_op_dest_imm(ir, q);
    if (target <= i)
      return 0;
  }

  int max_var = 0, max_tmp = 0;
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
      if (t == TCCIR_VREG_TYPE_VAR && p > max_var) max_var = p;
      else if (t == TCCIR_VREG_TYPE_TEMP && p > max_tmp) max_tmp = p;
    }
  }
  if (max_var == 0)
    return 0;

  DtaPtrs P;
  P.max_tmp = max_tmp;
  P.max_var = max_var;
  P.tmp_pt = tcc_malloc(sizeof(int) * (max_tmp + 1));
  for (int i = 0; i <= max_tmp; i++) P.tmp_pt[i] = -1;
  P.var_pt = tcc_malloc(sizeof(int) * (max_var + 1));
  for (int i = 0; i <= max_var; i++) P.var_pt[i] = -1;
  P.tmp_inexact = tcc_mallocz(max_tmp + 1);
  P.var_inexact = tcc_mallocz(max_var + 1);
  uint8_t *var_escaped = tcc_mallocz((max_var + 8) / 8);
  int *var_last_read = tcc_malloc(sizeof(int) * (max_var + 1));
  for (int i = 0; i <= max_var; i++) var_last_read[i] = -1;

  /* Pass 1: which vregs may hold &V -- from a LEA, through any chain of
   * register copies -- and which hold nothing else.  Facts only grow. */
  while (dta_propagate(ir, n, &P, var_escaped))
    ;

  /* Pass 2: deny by default.  A vreg that may hold &V is used only as a
   * deref base (a read or a write of V) or as the source of a pass-1 copy;
   * any other use (arithmetic, select, an indexed/postinc base, a stored or
   * returned value) lets &V go where this pass cannot follow, so V escapes,
   * and so does a volatile V: every write to it is an access that stays.
   * Pass 3: var_last_read[V] from direct reads of V and derefs of &V. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int plain_mem = q->op != TCCIR_OP_LOAD_INDEXED && q->op != TCCIR_OP_STORE_INDEXED &&
                    !ir_op_has(q->op, IR_HZ_UPDATES_SRC);
    int copy = dta_is_copy(ir, q);

    for (int k = 0; k < 4; k++)
    {
      IROperand s;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        s = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1 || k == 2)
      {
        if (!(k == 1 ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2))
          continue;
        s = tcc_ir_op_get_src1_or_2(ir, q, k == 2);
      }
      else
      {
        if (!ir_op_has(q->op, IROP_A_SLOT3))
          continue;
        s = ir->iroperand_pool[q->operand_base + 3];
      }
      int32_t vr = irop_get_vreg(s);
      if (vr < 0)
        continue;
      int vt = TCCIR_DECODE_VREG_TYPE(vr);
      int vp = TCCIR_DECODE_VREG_POSITION(vr);

      if (vt == TCCIR_VREG_TYPE_VAR && vp <= max_var && tcc_ir_operand_names_volatile_var(ir, s))
        mark_var_escaped(var_escaped, vp);

      /* Direct VAR read: even STORE src1 of `STORE dest <- V` reads V, and
       * a VAR the DEST stores through is read for its address. */
      if (vt == TCCIR_VREG_TYPE_VAR && vp <= max_var && (k != 0 || dta_dest_is_mem(ir, q)) &&
          i > var_last_read[vp])
        var_last_read[vp] = i;

      int v = dta_target(&P, vr);
      if (v < 0)
        continue;
      if (k == 0)
      {
        if (!dta_dest_is_mem(ir, q))
          continue; /* a def of the pointer vreg itself */
        if (!plain_mem)
          mark_var_escaped(var_escaped, v);
        continue; /* a write through it: pass 4 decides */
      }
      if (s.is_lval && plain_mem)
      {
        if (i > var_last_read[v])
          var_last_read[v] = i;
        continue;
      }
      if (k == 1 && copy)
        continue;
      mark_var_escaped(var_escaped, v);
    }
  }

  /* Pass 4: NOP writes to V after last_read[V] (direct, or via exactly &V). */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* The write is dead, not what it reads or writes: `x = *r` with r
     * volatile keeps its load, `*(volatile int *)&x = v` its store. */
    if (ir_q_hazards(ir, q, IR_HZ_VOLATILE))
      continue;

    int written_var = -1;
    if (irop_config[q->op].has_dest)
    {
      int32_t vr = tcc_ir_op_dest_vreg(ir, q);
      if (!dta_dest_is_mem(ir, q))
      {
        /* Direct write: conservative, only side-effect-free ASSIGN/STORE shapes. */
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR &&
            (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_STORE ||
             q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ADD ||
             q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_OR ||
             q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_SHL ||
             q->op == TCCIR_OP_SHR || q->op == TCCIR_OP_SAR || q->op == TCCIR_OP_ZEXT))
          written_var = TCCIR_DECODE_VREG_POSITION(vr);
      }
      else if (q->op == TCCIR_OP_STORE && dta_exact(&P, vr))
      {
        /* A plain store through a vreg that holds exactly &V. */
        written_var = dta_target(&P, vr);
      }
    }

    if (written_var < 0 || written_var > max_var)
      continue;
    if (var_is_escaped(var_escaped, written_var))
      continue;
    int last_read = var_last_read[written_var];
    if (last_read < 0)
      continue; /* never read -- dead_addrvar handles full elimination */
    if (i <= last_read)
      continue;

    LOG_IR_GEN("=== DEAD TRAILING ADDRVAR STORE: NOP i=%d (V=%d, last_read=%d) ===", i,
               written_var, last_read);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  tcc_free(var_last_read);
  tcc_free(var_escaped);
  tcc_free(P.var_inexact);
  tcc_free(P.tmp_inexact);
  tcc_free(P.var_pt);
  tcc_free(P.tmp_pt);
  return changes;
}
