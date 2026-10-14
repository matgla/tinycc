/*
 *  TCC IR - Dead address-taken VAR elimination
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
#include "cfg.h"
#include "licm.h"

static void var_bit_set(uint8_t *bm, int pos) { bm[pos / 8] |= (1 << (pos % 8)); }
static int var_bit_test(const uint8_t *bm, int pos) { return bm[pos / 8] & (1 << (pos % 8)); }

/* Which local's address a TEMP/VAR may hold: nodes are TEMPs 0..ntmp-1, then
 * VARs.  tgt is the local (a VAR position), DAV_NONE before any address
 * reaches it, or DAV_MANY once it is untracked; other is set once it can
 * also hold a value that is no tracked address. */
#define DAV_NONE (-1)
#define DAV_MANY (-2)

typedef struct DavFlow
{
  int ntmp;
  int *tgt;
  uint8_t *other;
  uint8_t *var_read;
} DavFlow;

static void dav_mark_var_read(uint8_t *var_read, int32_t vr, int max_var)
{
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos <= max_var)
      var_bit_set(var_read, pos);
  }
}

static int dav_node(const DavFlow *f, int32_t vr, int max_var)
{
  if (vr < 0)
    return -1;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos < f->ntmp)
    return pos;
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos <= max_var)
    return f->ntmp + pos;
  return -1;
}

/* The node a plain (value-defining) destination names, or -1. */
static int dav_dest_node(TCCIRState *ir, IRQuadCompact *q, const DavFlow *f, int max_var)
{
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (q->op == TCCIR_OP_STORE_INDEXED || !irop_dest_defines_vreg(d))
    return -1;
  return dav_node(f, irop_get_vreg(d), max_var);
}

/* A LEA of a VAR into a TEMP: that VAR's position, or -1. */
static int dav_lea_var(TCCIRState *ir, IRQuadCompact *q, int max_var)
{
  if (q->op != TCCIR_OP_LEA)
    return -1;
  int32_t d_vr = tcc_ir_op_dest_vreg(ir, q);
  int32_t s_vr = tcc_ir_op_src1_vreg(ir, q);
  if (d_vr < 0 || TCCIR_DECODE_VREG_TYPE(d_vr) != TCCIR_VREG_TYPE_TEMP || s_vr < 0 ||
      TCCIR_DECODE_VREG_TYPE(s_vr) != TCCIR_VREG_TYPE_VAR || TCCIR_DECODE_VREG_POSITION(s_vr) > max_var)
    return -1;
  return TCCIR_DECODE_VREG_POSITION(s_vr);
}

/* A plain copy (ASSIGN/STORE/LOAD of a TEMP's or VAR's own value into a
 * plain destination): the source node, or -1. */
static int dav_copy_src(TCCIRState *ir, IRQuadCompact *q, const DavFlow *f, int max_var)
{
  if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_LOAD)
    return -1;
  if (!irop_dest_defines_vreg(tcc_ir_op_get_dest(ir, q)))
    return -1;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  if (!irop_is_vreg_value(s))
    return -1;
  return dav_node(f, irop_get_vreg(s), max_var);
}

static void dav_escape(DavFlow *f, int target)
{
  if (target >= 0)
    var_bit_set(f->var_read, target);
}

/* The local a tracked node points to escapes (a use the pass cannot delete). */
static void dav_escape_node(DavFlow *f, int node)
{
  if (node >= 0)
    dav_escape(f, f->tgt[node]);
}

static void dav_poison(DavFlow *f, int node)
{
  dav_escape(f, f->tgt[node]);
  f->tgt[node] = DAV_MANY;
}

/* Merge one def's value into node: returns 1 when the node changed. */
static int dav_join(DavFlow *f, int node, int tgt, int other)
{
  int changed = 0;
  if (other && !f->other[node])
  {
    f->other[node] = 1;
    changed = 1;
  }
  if (tgt == DAV_NONE || tgt == f->tgt[node])
    return changed;
  if (f->tgt[node] == DAV_NONE)
  {
    f->tgt[node] = tgt;
    return 1;
  }
  dav_escape(f, tgt);
  if (f->tgt[node] == DAV_MANY)
    return changed;
  dav_poison(f, node);
  return 1;
}

int tcc_ir_opt_dead_addrvar_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Find max VAR and TMP positions */
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
      if (vr >= 0)
      {
        int type = TCCIR_DECODE_VREG_TYPE(vr);
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (type == TCCIR_VREG_TYPE_VAR && pos > max_var)
          max_var = pos;
        else if (type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp)
          max_tmp = pos;
      }
    }
  }

  if (max_var == 0)
    return 0;

  uint8_t *var_read = tcc_mallocz((max_var + 8) / 8);
  uint8_t *var_has_lea = tcc_mallocz((max_var + 8) / 8);

  /* Pass 1: mark directly-read VARs (LEA src1 / STORE dest are not reads)
   * and the VARs whose address a LEA takes. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_LEA)
    {
      int32_t d_vr = tcc_ir_op_dest_vreg(ir, q);
      int32_t s_vr = tcc_ir_op_src1_vreg(ir, q);
      if (s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int var_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
          var_bit_set(var_has_lea, var_pos); /* trackable: Pass 2 follows it */
        else
          var_bit_set(var_read, var_pos); /* LEA into a VAR: escapes */
      }
      continue;
    }

    if (irop_config[q->op].has_src1)
      dav_mark_var_read(var_read, tcc_ir_op_src1_vreg(ir, q), max_var);
    if (irop_config[q->op].has_src2)
      dav_mark_var_read(var_read, tcc_ir_op_src2_vreg(ir, q), max_var);
    /* An MLA/UMAAL accumulator or a SELECT condition is a read the src1/src2
     * helpers above don't cover. */
    if (ir_op_has(q->op, IROP_A_SLOT3))
      dav_mark_var_read(var_read, tcc_ir_op_accum_vreg(ir, q), max_var);
  }

  /* Pass 2: which local's address each TEMP/VAR may hold, followed through
   * LEA and plain copies (TEMP or VAR into TEMP or VAR, in any order, to a
   * fixpoint).  A value that can also hold anything else, or two locals'
   * addresses, is untracked and the locals flowing into it escape. */
  DavFlow f;
  f.ntmp = max_tmp + 1;
  f.var_read = var_read;
  f.tgt = tcc_malloc(sizeof(int) * (f.ntmp + max_var + 1));
  f.other = tcc_mallocz(f.ntmp + max_var + 1);
  for (int i = 0; i < f.ntmp + max_var + 1; i++)
    f.tgt[i] = DAV_NONE;
  /* An address-taken VAR's value can change, or be read, through memory. */
  for (int v = 0; v <= max_var; v++)
    if (var_bit_test(var_has_lea, v))
      f.other[f.ntmp + v] = 1;

  for (int changed = 1; changed;)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      /* STORE_INDEXED's plain dest is the base it stores through, not a def. */
      if (q->op == TCCIR_OP_STORE_INDEXED || !irop_dest_defines_vreg(d))
        continue;
      int dn = dav_node(&f, irop_get_vreg(d), max_var);
      if (dn < 0)
        continue;
      int lea_var = dav_lea_var(ir, q, max_var);
      if (lea_var >= 0)
        changed |= dav_join(&f, dn, lea_var, 0);
      else
      {
        int sn = dav_copy_src(ir, q, &f, max_var);
        if (sn >= 0)
          changed |= dav_join(&f, dn, f.tgt[sn], f.other[sn]);
        else
          changed |= dav_join(&f, dn, DAV_NONE, 1);
      }
    }
  }
  for (int i = 0; i < f.ntmp + max_var + 1; i++)
    if (f.tgt[i] >= 0 && f.other[i])
      dav_poison(&f, i);

  /* Pass 3: a tracked pointer used other than as a copy (into a tracked
   * value) or as the base of a STORE/STORE_INDEXED (which Pass 4 deletes with
   * its LEA) lets its local escape. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int copy_src = -1;
    if (irop_config[q->op].has_dest && dav_dest_node(ir, q, &f, max_var) >= 0)
      copy_src = dav_copy_src(ir, q, &f, max_var);

    if (irop_config[q->op].has_src1)
    {
      int sn = dav_node(&f, tcc_ir_op_src1_vreg(ir, q), max_var);
      if (sn >= 0 && sn != copy_src)
        dav_escape_node(&f, sn);
    }
    if (irop_config[q->op].has_src2)
      dav_escape_node(&f, dav_node(&f, tcc_ir_op_src2_vreg(ir, q), max_var));
    if (ir_op_has(q->op, IROP_A_SLOT3))
      dav_escape_node(&f, dav_node(&f, tcc_ir_op_accum_vreg(ir, q), max_var));

    /* A destination that is not a def reads the pointer it names: only a
     * STORE/STORE_INDEXED through a TEMP dies with the LEA. */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(d);
      int is_def = q->op != TCCIR_OP_STORE_INDEXED && irop_dest_defines_vreg(d);
      int is_tracked_store = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) && vr >= 0 &&
                             TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP;
      /* STORE_POSTINC both reads and advances its base. */
      if (q->op == TCCIR_OP_STORE_POSTINC || (!is_def && !is_tracked_store))
        dav_escape_node(&f, dav_node(&f, vr, max_var));
    }
  }

  /* Pass 4: eliminate dead writes to unread VARs */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* ASSIGN to dead VAR -> NOP (only if VAR has LEA, proving all accesses are tracked) */
    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_dest)
    {
      int32_t vr = tcc_ir_op_dest_vreg(ir, q);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var && var_bit_test(var_has_lea, pos) && !var_bit_test(var_read, pos))
        {
          q->op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }

    /* LEA from dead VAR -> NOP (TMP dests only; VAR dests handled by regular DSE) */
    if (q->op == TCCIR_OP_LEA)
    {
      int32_t d_vr = tcc_ir_op_dest_vreg(ir, q);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int32_t vr = tcc_ir_op_src1_vreg(ir, q);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var && var_bit_test(var_has_lea, pos) && !var_bit_test(var_read, pos))
          {
            q->op = TCCIR_OP_NOP;
            changes++;
          }
        }
      }
    }

    /* STORE through LEA to dead VAR -> NOP.  STORE_INDEXED too: it carries the
     * base in a plain dest, and left behind it would store through the TEMP
     * whose LEA was just deleted -- through whatever register the allocator
     * gave it (Zig's meta.eql zeroed a u64 local with a memset lowered to two
     * STORE_INDEXEDs, and wrote the zeros through its caller's r4 and r5). */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
    {
      int32_t vr = tcc_ir_op_dest_vreg(ir, q);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tn = dav_node(&f, vr, max_var);
        if (tn >= 0 && f.tgt[tn] >= 0 && !var_bit_test(var_read, f.tgt[tn]))
        {
          q->op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }
  }

  /* STORE to dead VAR (non-deref): V = val where V is unread.  A copy of a
   * dead local's address goes too: every use of it was a copy or a store
   * deleted above, and left behind it would read the deleted LEA's TEMP. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    int32_t vr = tcc_ir_op_dest_vreg(ir, q);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos <= max_var && var_bit_test(var_has_lea, pos) && !var_bit_test(var_read, pos))
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }
  }
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int dn = dav_dest_node(ir, q, &f, max_var);
    if (dn < 0 || f.tgt[dn] < 0 || var_bit_test(var_read, f.tgt[dn]))
      continue;
    if (dav_copy_src(ir, q, &f, max_var) >= 0)
    {
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }

  LOG_IR_GEN("=== DEAD ADDRVAR ELIM: eliminated %d dead writes ===", changes);

  tcc_free(f.other);
  tcc_free(f.tgt);
  tcc_free(var_has_lea);
  tcc_free(var_read);
  return changes;
}
