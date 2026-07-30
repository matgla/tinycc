/*
 *  TCC IR - SSA DCE: dead TEMP worklist
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "dce_passes.h"


int dce_temp_worklist(IRSSAOptCtx *ctx)
{
  int cap = ctx->vinfo_cap;
  int *worklist = tcc_mallocz(cap * sizeof(int));
  int wl_count = 0;
  int changes = 0;

  for (int pos = 0; pos < cap; pos++) {
    IRSSAVregInfo *vi = &ctx->vinfo[pos];
    if (vi->use_count == 0 && vi->def_instr >= 0)
      worklist[wl_count++] = pos;
  }

  while (wl_count > 0) {
    int pos = worklist[--wl_count];
    IRSSAVregInfo *vi = &ctx->vinfo[pos];

    if (vi->use_count > 0 || vi->def_instr < 0)
      continue;

    int def = vi->def_instr;
    IRQuadCompact *q = &ctx->ir->compact_instructions[def];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* A write to a volatile slot is a mandated side effect even if never read. */
    if (irop_config[q->op].has_dest) {
      int32_t dvr = irop_get_vreg(tcc_ir_op_get_dest(ctx->ir, q));
      int dt = (dvr >= 0) ? TCCIR_DECODE_VREG_TYPE(dvr) : -1;
      if (dt == TCCIR_VREG_TYPE_VAR || dt == TCCIR_VREG_TYPE_PARAM) {
        IRLiveInterval *div = tcc_ir_get_live_interval(ctx->ir, dvr);
        if (div && div->is_volatile)
          continue;
      }
    }
    if (ssa_opt_has_side_effects(q->op)) {
      /* A non-lval STORE dest is the value-def encoding `T = expr`: no memory write. */
      int killable = 0;
      if (q->op == TCCIR_OP_STORE) {
        IROperand d = tcc_ir_op_get_dest(ctx->ir, q);
        if (!d.is_lval)
          killable = 1;
      }
      if (q->op == TCCIR_OP_FUNCCALLVAL) {
        Sym *callee = irop_get_sym_ex(ctx->ir, tcc_ir_op_get_src1(ctx->ir, q));
        const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
        if (name && (tcc_ir_is_pure_aeabi(name) || ir_opt_is_pure_helper_name(name) ||
                     ir_opt_is_readonly_str_helper_name(name)))
          killable = 1;
      }
      if (!killable)
        continue;
    }

    int32_t op_vregs[4] = { -1, -1, -1, -1 };
    int nops = 0;
    TCCIRState *ir = ctx->ir;

    if (irop_config[q->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      op_vregs[nops++] = irop_get_vreg(s);
    }
    if (irop_config[q->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      op_vregs[nops++] = irop_get_vreg(s);
    }
    if (q->op == TCCIR_OP_MLA) {
      IROperand a = tcc_ir_op_get_accum(ir, q);
      op_vregs[nops++] = irop_get_vreg(a);
    }

    ssa_opt_nop_instr(ctx, def);
    vi->def_instr = -1;
    changes++;

    for (int k = 0; k < nops; k++) {
      IRSSAVregInfo *ovi = ssa_opt_vinfo(ctx, op_vregs[k]);
      if (ovi && ovi->use_count == 0 && ovi->def_instr >= 0) {
        if (wl_count < cap)
          worklist[wl_count++] = TCCIR_DECODE_VREG_POSITION(op_vregs[k]);
      }
    }
  }

  tcc_free(worklist);
  return changes;
}
