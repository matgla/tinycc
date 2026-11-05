/*
 *  TCC IR - Which frame object a TEMP or VAR may hold the address of
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The points-to facts the dead-store passes share (docs/plans/
 * opt_pass_merge_and_loop_reduction.md, A2).  Objects are the VARs whose
 * address a LEA takes and the anonymous StackLocs whose address an
 * instruction materialises into a TEMP; nodes are the TEMP and VAR vregs.
 * One fixpoint follows an address through register copies (ASSIGN, LOAD and
 * STORE into a register destination) and pointer arithmetic (ADD, SUB of the
 * pointer with anything); every other use of a node that may hold an address
 * is a read of the object, or a place the facts cannot follow, and marks it
 * read.  A node that also takes a value that is no tracked address is
 * inexact; one that may hold two objects' addresses is MANY, which flags
 * both objects, and a use of it reads every object.  Cached per flat-engine generation like
 * ir_opt_fn_features. */

#define USING_GLOBALS

#include "ir.h"
#include "opt_alias.h"
#include "opt_utils.h"

static int fp_node(const IRFramePtr *fp, int32_t vr)
{
  if (vr < 0)
    return -1;
  const int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos < fp->ntmp)
    return pos;
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos < fp->nvar)
    return fp->ntmp + pos;
  return -1;
}

int ir_opt_frame_ptr_node(const IRFramePtr *fp, int32_t vr)
{
  return fp_node(fp, vr);
}

/* A destination that (re)defines its vreg: a register, or a VAR written by
 * name.  STORE_INDEXED/POSTINC carry the base they store through there. */
static int fp_defines(const IRQuadCompact *q, IROperand d)
{
  return q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC && irop_dest_defines_vreg(d);
}

/* The LEA of a VAR, or the materialised address of an anonymous StackLoc
 * (a local, non-lvalue StackLoc operand with a frontend temporary's vreg) into
 * a register destination: the VAR position, the StackLoc marker (-2), or -1.
 * A memory read whose address operand is a StackLoc (LOAD_INDEXED over a
 * local array) loads the array's content, not its address. */
static int fp_address_def(TCCIRState *ir, const IRQuadCompact *q, IROperand d)
{
  if (!irop_config[q->op].has_src1 || !fp_defines(q, d))
    return -1;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  int32_t svr = irop_get_vreg(s);
  if (q->op == TCCIR_OP_LEA && !s.is_lval && svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
    return TCCIR_DECODE_VREG_POSITION(svr);
  if (q->op == TCCIR_OP_LEA && s.is_lval && svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
    return TCCIR_DECODE_VREG_POSITION(svr);
  if (irop_is_local(s) && !s.is_lval && svr < 0 && !ir_op_has(q->op, IR_HZ_MEM_READ))
    return -2;
  return -1;
}

int ir_opt_frame_ptr_copy_src(TCCIRState *ir, const IRFramePtr *fp, const struct IRQuadCompact *q)
{
  if (!irop_config[q->op].has_dest || !irop_config[q->op].has_src1)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (!fp_defines(q, d) || fp_address_def(ir, q, d) != -1)
    return -1;
  if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_ADD &&
      q->op != TCCIR_OP_SUB)
    return -1;
  /* Only a copy INTO a node the facts follow is a copy; into anything else
   * (a parameter variable, say) the address leaves the facts, and the source
   * use counts as a read.  decl_designator stored &local into a parameter it
   * then passed on: the local's initialiser store was deleted under it. */
  if (fp_node(fp, irop_get_vreg(d)) < 0)
    return -1;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  if (!irop_is_vreg_value(s))
    return -1;
  return fp_node(fp, irop_get_vreg(s));
}

static int fp_new_obj(IRFramePtr *fp, int var, int tmp, int def_idx)
{
  if (fp->nobjs == fp->objs_cap)
  {
    fp->objs_cap = fp->objs_cap ? fp->objs_cap * 2 : 8;
    fp->objs = tcc_realloc(fp->objs, fp->objs_cap * sizeof(IRFrameObj));
  }
  IRFrameObj *o = &fp->objs[fp->nobjs];
  memset(o, 0, sizeof *o);
  o->var = var;
  o->tmp = tmp;
  o->def_idx = def_idx;
  return fp->nobjs++;
}

static void fp_obj_read(IRFramePtr *fp, int obj)
{
  if (obj == IR_FP_MANY)
    fp->read_all = 1;
  else if (obj >= 0)
    fp->objs[obj].read = 1;
}

/* Merge one definition's value into node: 1 when the node changed. */
static int fp_join(IRFramePtr *fp, int node, int obj, int other)
{
  int changed = 0;
  if (other && !fp->node_inexact[node])
  {
    fp->node_inexact[node] = 1;
    changed = 1;
  }
  if (obj == IR_FP_NONE || obj == fp->node_obj[node])
    return changed;
  if (fp->node_obj[node] == IR_FP_NONE)
  {
    fp->node_obj[node] = obj;
    return 1;
  }
  /* Two addresses, or an address and MANY: the node is MANY, and the objects
   * are flagged; a use of the node is what reads them (fp_obj_read). */
  if (obj >= 0)
    fp->objs[obj].many = 1;
  if (fp->node_obj[node] == IR_FP_MANY)
    return changed;
  fp->objs[fp->node_obj[node]].many = 1;
  fp->node_obj[node] = IR_FP_MANY;
  return 1;
}

static void fp_build(TCCIRState *ir, IRFramePtr *fp)
{
  const int n = ir->next_instruction_index;
  int max_tmp = -1, max_var = -1;
  fp->n = n;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 4; k++)
    {
      IROperand op;
      if (k == 0) { if (!irop_config[q->op].has_dest) continue; op = tcc_ir_op_get_dest(ir, q); }
      else if (k == 1) { if (!irop_config[q->op].has_src1) continue; op = tcc_ir_op_get_src1(ir, q); }
      else if (k == 2) { if (!irop_config[q->op].has_src2) continue; op = tcc_ir_op_get_src2(ir, q); }
      else { if (!ir_op_has(q->op, IROP_A_SLOT3)) continue; op = tcc_ir_op_get_accum(ir, q); }
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos > max_tmp)
        max_tmp = pos;
      else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos > max_var)
        max_var = pos;
    }
  }
  fp->ntmp = max_tmp + 1;
  fp->nvar = max_var + 1;
  const int nnodes = fp->ntmp + fp->nvar;
  fp->node_obj = tcc_malloc(sizeof(int) * (nnodes + 1));
  fp->node_inexact = tcc_mallocz(nnodes + 1);
  fp->node_defs = tcc_mallocz(nnodes + 1);
  fp->var_obj = tcc_malloc(sizeof(int) * (fp->nvar + 1));
  fp->tmp_anon_obj = tcc_malloc(sizeof(int) * (fp->ntmp + 1));
  for (int i = 0; i <= nnodes; i++)
    fp->node_obj[i] = IR_FP_NONE;
  for (int i = 0; i <= fp->nvar; i++)
    fp->var_obj[i] = -1;
  for (int i = 0; i <= fp->ntmp; i++)
    fp->tmp_anon_obj[i] = -1;
  fp->var_direct_read = tcc_mallocz(fp->nvar + 1);

  /* 1. The address definitions seed their nodes and create the objects. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    int dn = fp_node(fp, dvr);
    if (dn < 0)
      continue;
    if (fp_defines(q, d) && fp->node_defs[dn] < 2)
      fp->node_defs[dn]++;
    int what = fp_address_def(ir, q, d);
    if (what == -1)
      continue;
    int obj;
    if (what >= 0)
    {
      if (fp->var_obj[what] < 0)
        fp->var_obj[what] = fp_new_obj(fp, what, -1, i);
      obj = fp->var_obj[what];
      if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
        fp->objs[obj].has_lea = 1;
      else
        fp->objs[obj].lea_into_var = 1;
    }
    else
    {
      if (TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int tpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (fp->tmp_anon_obj[tpos] < 0)
        fp->tmp_anon_obj[tpos] = fp_new_obj(fp, -1, tpos, i);
      obj = fp->tmp_anon_obj[tpos];
    }
    fp_join(fp, dn, obj, 0);
  }
  if (fp->nobjs == 0)
    return;

  /* 2. Copies and pointer arithmetic carry the facts; any other definition
   *    makes the node inexact.  To a fixpoint: a VAR may be copied before
   *    the LEA that reaches it. */
  for (int changed = 1; changed;)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!fp_defines(q, d) || fp_address_def(ir, q, d) != -1)
        continue;
      int dn = fp_node(fp, irop_get_vreg(d));
      if (dn < 0)
        continue;
      int sn = ir_opt_frame_ptr_copy_src(ir, fp, q);
      if (sn >= 0)
        changed |= fp_join(fp, dn, fp->node_obj[sn], fp->node_inexact[sn]);
      else
        changed |= fp_join(fp, dn, IR_FP_NONE, 1);
    }
  }
  for (int nd = 0; nd < nnodes; nd++)
    if (fp->node_obj[nd] >= 0 && fp->node_inexact[nd])
      fp->objs[fp->node_obj[nd]].inexact = 1;

  /* 3. Uses.  A node that may hold an address is used as the source of a
   *    tracked copy, as the base of a plain store (which a consumer may
   *    delete with the address), or in some other way -- a deref read, a
   *    value stored or passed on, an index -- which reads the object or
   *    hands its address where the facts cannot follow.  A VAR read by name
   *    (any use but as the LEA's operand) is a direct read. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    const int copy_src = ir_opt_frame_ptr_copy_src(ir, fp, q);
    for (int k = 1; k < 4; k++)
    {
      IROperand s;
      if (k == 1) { if (!irop_config[q->op].has_src1) continue; s = tcc_ir_op_get_src1(ir, q); }
      else if (k == 2) { if (!irop_config[q->op].has_src2) continue; s = tcc_ir_op_get_src2(ir, q); }
      else { if (!ir_op_has(q->op, IROP_A_SLOT3)) continue; s = tcc_ir_op_get_accum(ir, q); }
      int32_t vr = irop_get_vreg(s);
      if (vr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && !(k == 1 && q->op == TCCIR_OP_LEA))
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < fp->nvar)
          fp->var_direct_read[pos] = 1;
      }
      int nd = fp_node(fp, vr);
      if (nd < 0 || fp->node_obj[nd] == IR_FP_NONE)
        continue;
      if (k == 1 && copy_src == nd)
        continue;
      fp_obj_read(fp, fp->node_obj[nd]);
    }
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int nd = fp_node(fp, irop_get_vreg(d));
      if (nd < 0 || fp->node_obj[nd] == IR_FP_NONE || fp_defines(q, d))
        continue;
      /* The base a store writes through: deletable with the address unless it
       * is volatile (observable by itself) or advances the base. */
      if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) && !tcc_ir_instr_access_is_volatile(ir, q))
        continue;
      fp_obj_read(fp, fp->node_obj[nd]);
    }
  }
}

void ir_opt_frame_ptr_free(TCCIRState *ir)
{
  IRFramePtr *fp = ir->frame_ptr;
  if (!fp)
    return;
  tcc_free(fp->node_obj);
  tcc_free(fp->node_inexact);
  tcc_free(fp->node_defs);
  tcc_free(fp->objs);
  tcc_free(fp->var_obj);
  tcc_free(fp->tmp_anon_obj);
  tcc_free(fp->var_direct_read);
  tcc_free(fp);
  ir->frame_ptr = NULL;
  ir->frame_ptr_gen = 0;
}

const IRFramePtr *ir_opt_frame_ptr(TCCIRState *ir)
{
  if (ir->frame_ptr && ir->opt_generation && ir->frame_ptr_gen == ir->opt_generation &&
      ((IRFramePtr *)ir->frame_ptr)->n == ir->next_instruction_index)
    return ir->frame_ptr;
  ir_opt_frame_ptr_free(ir);
  IRFramePtr *fp = tcc_mallocz(sizeof *fp);
  fp_build(ir, fp);
  ir->frame_ptr = fp;
  ir->frame_ptr_gen = ir->opt_generation;
  return fp;
}
