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

/* An address-taken VAR that nothing ever reads -- by name, or through any
 * pointer that may hold its address -- is dead with every write to it: the
 * LEAs that take its address into a TEMP, the stores through those pointers,
 * the copies and pointer arithmetic that carry the address, and the writes to
 * it by name.  The facts come from ir_opt_frame_ptr (opt_alias.h), shared
 * with dse; a VAR whose address lands in a VAR, or reaches a pointer that may
 * hold something else, or escapes, stays. */
static int dav_pos_dead(const IRFramePtr *fp, int pos)
{
  if (pos < 0 || pos >= fp->nvar || fp->var_obj[pos] < 0)
    return 0;
  const IRFrameObj *o = &fp->objs[fp->var_obj[pos]];
  /* A MANY pointer may carry this address into a store this pass cannot
   * delete (the other object may be live), so the LEA must stay too. */
  return o->has_lea && !o->lea_into_var && !o->read && !o->inexact && !o->many && !fp->var_direct_read[pos] &&
         !fp->read_all;
}

static int dav_var_dead(const IRFramePtr *fp, int32_t vr)
{
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  return dav_pos_dead(fp, TCCIR_DECODE_VREG_POSITION(vr));
}

/* The dead VAR a node points to, if any (exact pointers only). */
static int dav_node_dead(const IRFramePtr *fp, int nd)
{
  if (nd < 0 || fp->node_obj[nd] < 0 || fp->node_inexact[nd])
    return 0;
  const IRFrameObj *o = &fp->objs[fp->node_obj[nd]];
  return o->var >= 0 && dav_pos_dead(fp, o->var);
}

int tcc_ir_opt_dead_addrvar_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  const IRFramePtr *fp = ir_opt_frame_ptr(ir);
  /* Position 0 alone was never a candidate here (the VAR-cascading passes'
   * max_var == 0 sentinel); kept so the pass changes no more than it must. */
  if (fp->nvar <= 1 || fp->nobjs == 0)
    return 0;

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);

    /* A write to the dead VAR by name: only the side-effect-free shapes. */
    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) && irop_dest_defines_vreg(d) && dav_var_dead(fp, dvr))
    {
      q->op = TCCIR_OP_NOP;
      changes++;
      continue;
    }
    /* LEA of the dead VAR into a TEMP. */
    if (q->op == TCCIR_OP_LEA && dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP &&
        dav_var_dead(fp, tcc_ir_op_src1_vreg(ir, q)))
    {
      q->op = TCCIR_OP_NOP;
      changes++;
      continue;
    }
    /* A store through a pointer to it.  STORE_INDEXED too: it carries the
     * base in a plain dest, and left behind it would store through the TEMP
     * whose LEA was just deleted -- through whatever register the allocator
     * gave it (Zig's meta.eql zeroed a u64 local with a memset lowered to two
     * STORE_INDEXEDs, and wrote the zeros through its caller's r4 and r5). */
    if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) && dvr >= 0 &&
        TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && !irop_dest_defines_vreg(d) &&
        dav_node_dead(fp, ir_opt_frame_ptr_node(fp, dvr)))
    {
      q->op = TCCIR_OP_NOP;
      changes++;
      continue;
    }
    /* A copy of the dead address, or arithmetic on it: every use of its
     * result was a copy or a store deleted here, and left behind it would read
     * the deleted LEA's TEMP. */
    if (dav_node_dead(fp, ir_opt_frame_ptr_node(fp, dvr)) && ir_opt_frame_ptr_copy_src(ir, fp, q) >= 0)
    {
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }

  LOG_IR_GEN("=== DEAD ADDRVAR ELIM: eliminated %d dead writes ===", changes);
  return changes;
}
