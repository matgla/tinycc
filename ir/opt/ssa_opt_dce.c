/*
 *  TCC IR - SSA Dead Code Elimination
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
#include <limits.h>

static int dce_temp_worklist(IRSSAOptCtx *ctx)
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
    if (ssa_opt_has_side_effects(q->op)) {
      /* STORE with non-lval VREG dest is the IR's value-def encoding
       * (`T = expr`, e.g. address materialisation `T = Addr[StackLoc[N]]`).
       * It writes only to the dest vreg, no memory or other side effect,
       * so a dead TEMP defined this way is safe to NOP. */
      int killable = 0;
      if (q->op == TCCIR_OP_STORE) {
        IROperand d = tcc_ir_op_get_dest(ctx->ir, q);
        if (!d.is_lval)
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

static int dce_unreachable(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int changes = 0;
  int has_indirect = 0;

  uint8_t *is_target = tcc_mallocz((n + 7) / 8);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE) {
      has_indirect = 1;
      break;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = dest.u.imm32;
      if (target >= 0 && target < n)
        is_target[target / 8] |= (1 << (target % 8));
    }
  }
  if (!has_indirect) {
    int dead = 0;
    for (int i = 0; i < n; i++) {
      if (is_target[i / 8] & (1 << (i % 8)))
        dead = 0;
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (dead && q->op != TCCIR_OP_NOP) {
        ssa_opt_nop_instr(ctx, i);
        changes++;
        continue;
      }
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_RETURNVALUE ||
          q->op == TCCIR_OP_RETURNVOID)
        dead = 1;
    }
  }
  tcc_free(is_target);
  return changes;
}

static int sl_temp_has_live_uses(IRSSAOptCtx *ctx, int32_t vreg);

/* Check if a TEMP produced by LEA/address-of is used only for writing
 * through the pointer (STORE dest), never for reading, calling, or
 * escaping.  Returns 1 if the address is write-only (safe to ignore
 * for address-taken analysis). */
static int var_addr_is_write_only(IRSSAOptCtx *ctx, int32_t vreg)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vreg);
  if (!vi)
    return 0;
  TCCIRState *ir = ctx->ir;
  for (int u = 0; u < vi->use_count; u++) {
    if (vi->uses[u].kind == SSA_USE_PHI)
      return 0;
    int idx = vi->uses[u].idx;
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (irop_get_vreg(d) == vreg)
        continue;
    }
    return 0;
  }
  return 1;
}

static int dce_dead_var_stores(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int num_vars = ir->next_local_variable;
  int changes = 0;

  if (num_vars <= 0)
    return 0;

  /* Nested functions access parent VARs through the frame pointer, which
   * doesn't appear as explicit VAR reads in the parent's IR. Bail out
   * if the function sets up a static chain or trampoline for nested calls. */
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  uint8_t *var_used = tcc_mallocz((num_vars + 7) / 8);
  uint8_t *var_addrtaken = tcc_mallocz((num_vars + 7) / 8);

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    for (int oi = 0; oi < 2; oi++) {
      IROperand s;
      if (oi == 0 && irop_config[q->op].has_src1)
        s = tcc_ir_op_get_src1(ir, q);
      else if (oi == 1 && irop_config[q->op].has_src2)
        s = tcc_ir_op_get_src2(ir, q);
      else
        continue;
      int32_t vr = irop_get_vreg(s);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos >= num_vars)
        continue;
      if (s.is_local && !s.is_lval) {
        int safe = 0;
        if (irop_config[q->op].has_dest) {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (dvr >= 0 &&
              TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP &&
              (!sl_temp_has_live_uses(ctx, dvr) ||
               var_addr_is_write_only(ctx, dvr)))
            safe = 1;
        }
        if (!safe)
          var_addrtaken[pos / 8] |= (1 << (pos % 8));
      } else {
        var_used[pos / 8] |= (1 << (pos % 8));
      }
    }

    /* MLA accumulator: third operand not covered by src1/src2 */
    if (q->op == TCCIR_OP_MLA) {
      IROperand a = tcc_ir_op_get_accum(ir, q);
      int32_t vr = irop_get_vreg(a);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars)
          var_used[pos / 8] |= (1 << (pos % 8));
      }
    }

    /* STORE dest: for ptr stores, the dest TEMP is a use (address).
     * When the dest is a VAR vreg with is_lval=1 AND is_local=0, V's *value*
     * (a pointer) is read as the destination address — that's a value use of
     * V, not a write to V's slot.  Without marking V as used, pass 2 below
     * would NOP this STORE, dropping the write to the pointee memory.
     *
     * The is_local=0 check excludes plain VAR-slot stores `V <-- val [STORE]`
     * where the operand encodes V's stack slot (is_local=1, is_lval=1).  In
     * that pattern V is the storage, not a pointer, so the STORE writes
     * directly to V's slot — V is *not* used as a value here, and a dead V
     * is safe to eliminate. */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR &&
          d.is_lval && !d.is_local) {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos < num_vars)
          var_used[pos / 8] |= (1 << (pos % 8));
      }
    }

    if (q->op == TCCIR_OP_LEA) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars) {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (dvr < 0 ||
              TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP ||
              (sl_temp_has_live_uses(ctx, dvr) &&
               !var_addr_is_write_only(ctx, dvr)))
            var_addrtaken[pos / 8] |= (1 << (pos % 8));
        }
      }
    }

    if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars)
          var_used[pos / 8] |= (1 << (pos % 8));
      }
    }

    if (q->op == TCCIR_OP_FUNCPARAMVAL) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars)
          var_used[pos / 8] |= (1 << (pos % 8));
      }
    }
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_VAR)
      continue;
    /* STORE to a dead VAR is safe to eliminate; other side-effect ops are not */
    if (q->op != TCCIR_OP_STORE && ssa_opt_has_side_effects(q->op))
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos >= num_vars)
      continue;
    if (var_addrtaken[pos / 8] & (1 << (pos % 8)))
      continue;
    if (var_used[pos / 8] & (1 << (pos % 8)))
      continue;

    ssa_opt_nop_instr(ctx, i);
    changes++;
  }

  /* Pass 3: eliminate write-through-pointer chains targeting dead VARs.
   * When a VAR is dead (not used, not address-taken) and a LEA produced
   * a write-only pointer to it, NOP the STORE instructions that write
   * through that pointer. */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ASSIGN)
      continue;
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_src1)
      continue;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    int32_t svr = irop_get_vreg(s);
    if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
      continue;
    if (!(s.is_local && !s.is_lval))
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(svr);
    if (pos >= num_vars)
      continue;
    if (var_addrtaken[pos / 8] & (1 << (pos % 8)))
      continue;
    if (var_used[pos / 8] & (1 << (pos % 8)))
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (!var_addr_is_write_only(ctx, dvr))
      continue;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dvr);
    if (!vi)
      continue;
    for (int u = vi->use_count - 1; u >= 0; u--) {
      if (vi->uses[u].kind != SSA_USE_INSTR)
        continue;
      int si = vi->uses[u].idx;
      if (ir->compact_instructions[si].op == TCCIR_OP_NOP)
        continue;
      ssa_opt_nop_instr(ctx, si);
      changes++;
    }
    ssa_opt_nop_instr(ctx, i);
    changes++;
  }

  tcc_free(var_used);
  tcc_free(var_addrtaken);
  return changes;
}

/* ============================================================================
 * Dead StackLoc Store Elimination
 *
 * Eliminates STORE instructions to anonymous StackLoc offsets (not VAR vregs)
 * that are never read.  Uses a hash-based bitmap to track read offsets.
 *
 * When checking whether a TEMP is "effectively dead", we walk its use list
 * and verify that all remaining uses point to NOP'd instructions.  This is
 * more robust than relying on use_count alone, which can become stale when
 * earlier passes NOP instructions without fully updating the use chains.
 * ============================================================================ */

#define SL_HASH_SIZE 256
#define SL_HASH(sym, off) \
  (((uintptr_t)(sym) * 31 + (uint32_t)(off) * 17) % (SL_HASH_SIZE * 8))
#define SL_SET(bm, sym, off)                               \
  do {                                                     \
    uint32_t _h = SL_HASH(sym, off);                       \
    (bm)[_h / 8] |= (1 << (_h % 8));                      \
  } while (0)
#define SL_TEST(bm, sym, off) \
  ((bm)[SL_HASH(sym, off) / 8] & (1 << (SL_HASH(sym, off) % 8)))

static void sl_get_offset(TCCIRState *ir, IROperand op,
                          const Sym **sym_out, int64_t *off_out)
{
  *sym_out = NULL;
  if (irop_get_tag(op) == IROP_TAG_SYMREF) {
    IRPoolSymref *sr = irop_get_symref_ex(ir, op);
    *sym_out = sr ? sr->sym : NULL;
    *off_out = sr ? sr->addend : 0;
  } else {
    *off_out = irop_get_stack_offset(op);
  }
}

static int sl_access_width(IROperand op)
{
  switch (irop_get_btype(op)) {
  case IROP_BTYPE_INT8: return 1;
  case IROP_BTYPE_INT16: return 2;
  case IROP_BTYPE_FLOAT32: return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64: return 8;
  default: return 4;
  }
}

static void sl_mark_read(uint8_t *bm, const Sym *sym, int64_t off, int width)
{
  for (int b = 0; b < width; b++)
    SL_SET(bm, sym, off + b);
}

static void sl_mark_range(uint8_t *bm, const Sym *sym,
                          int64_t min_off, int64_t max_off)
{
  int64_t start = min_off;
  int64_t end = max_off + 8;
  int64_t len = end - start + 1;
  if (len > SL_HASH_SIZE * 8) {
    memset(bm, 0xFF, SL_HASH_SIZE);
    return;
  }
  for (int64_t k = start; k <= end; k++)
    SL_SET(bm, sym, k);
}

static int sl_is_anon_stackloc(IROperand op)
{
  return op.is_local && irop_get_vreg(op) < 0;
}

static void sl_mark_op(TCCIRState *ir, uint8_t *bm, IROperand op,
                       int64_t min_off, int64_t max_off)
{
  if (!sl_is_anon_stackloc(op))
    return;
  const Sym *sym;
  int64_t off;
  sl_get_offset(ir, op, &sym, &off);
  if (op.is_lval) {
    int w = (irop_get_btype(op) == IROP_BTYPE_STRUCT) ? 0 : sl_access_width(op);
    if (w > 0)
      sl_mark_read(bm, sym, off, w);
    else
      sl_mark_range(bm, sym, min_off, max_off);
  } else {
    sl_mark_range(bm, sym, min_off, max_off);
  }
}

static int sl_temp_has_live_uses(IRSSAOptCtx *ctx, int32_t vreg)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vreg);
  if (!vi)
    return 1;
  TCCIRState *ir = ctx->ir;
  for (int u = 0; u < vi->use_count; u++) {
    if (vi->uses[u].kind == SSA_USE_PHI)
      return 1;
    if (ir->compact_instructions[vi->uses[u].idx].op != TCCIR_OP_NOP)
      return 1;
  }
  return 0;
}

static int dce_dead_stackloc_stores(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int changes = 0;

  if (ir->has_static_chain)
    return 0;
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  int64_t min_off = 0, max_off = 0;
  int have_off = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!sl_is_anon_stackloc(dest))
      continue;
    const Sym *sym;
    int64_t off;
    sl_get_offset(ir, dest, &sym, &off);
    if (!have_off) {
      min_off = max_off = off;
      have_off = 1;
    } else {
      if (off < min_off) min_off = off;
      if (off > max_off) max_off = off;
    }
  }

  uint8_t sl_read[SL_HASH_SIZE];
  memset(sl_read, 0, sizeof(sl_read));

  /* Pass 1: mark StackLoc offsets that are read or whose address escapes.
   * Skip instructions whose dest TEMP has no live uses — they are
   * effectively dead and their operand reads should not prevent
   * StackLoc elimination. */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!ssa_opt_has_side_effects(q->op) && irop_config[q->op].has_dest) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP &&
          !sl_temp_has_live_uses(ctx, dvr))
        continue;
    }

    if (irop_config[q->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (sl_is_anon_stackloc(s) && !s.is_lval) {
        /* Addr[StackLoc]: skip range marking if the dest TEMP has no
         * live uses — the address never escapes to observable code. */
        if (irop_config[q->op].has_dest) {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (dvr >= 0 &&
              TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP &&
              !sl_temp_has_live_uses(ctx, dvr))
            goto skip_src1;
        }
      }
      if (q->op == TCCIR_OP_FUNCPARAMVAL ||
          q->op == TCCIR_OP_FUNCPARAMVOID) {
        if (sl_is_anon_stackloc(s)) {
          IROperand sr = s;
          sr.is_lval = 0;
          sl_mark_op(ir, sl_read, sr, min_off, max_off);
        }
      }
      sl_mark_op(ir, sl_read, s, min_off, max_off);
    skip_src1:;
    }
    if (irop_config[q->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      sl_mark_op(ir, sl_read, s, min_off, max_off);
    }
    if (q->op == TCCIR_OP_MLA) {
      IROperand a = tcc_ir_op_get_accum(ir, q);
      sl_mark_op(ir, sl_read, a, min_off, max_off);
    }
  }

  /* Pass 2: eliminate STORE to unread StackLoc offsets */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!sl_is_anon_stackloc(dest))
      continue;
    if (dest.is_llocal)
      continue;

    const Sym *sym;
    int64_t off;
    sl_get_offset(ir, dest, &sym, &off);
    int width = (irop_get_btype(dest) == IROP_BTYPE_STRUCT)
                    ? 1
                    : sl_access_width(dest);
    int any_read = 0;
    for (int b = 0; b < width; b++) {
      if (SL_TEST(sl_read, sym, off + b)) {
        any_read = 1;
        break;
      }
    }
    if (!any_read) {
      ssa_opt_nop_instr(ctx, i);
      changes++;
    }
  }

  return changes;
}

#undef SL_HASH_SIZE
#undef SL_HASH
#undef SL_SET
#undef SL_TEST

/* Aggressive dead phi cycle elimination.
 *
 * Standard DCE cannot break cycles of phi nodes and ASSIGN copies where each
 * value is "used" only by another value in the cycle.  This pass computes
 * backward liveness from essential operations (anything other than ASSIGN/NOP)
 * and removes phi nodes whose dests never reach an essential use. */
static int dce_dead_phi_cycles(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRSSAState *ssa = ctx->ssa;
  int cap = ctx->vinfo_cap;

  if (!ssa || !ssa->block_phis || !cfg || cap <= 0)
    return 0;

  int has_phi = 0;
  for (int b = 0; b < cfg->num_blocks && !has_phi; b++)
    if (ssa->block_phis[b])
      has_phi = 1;
  if (!has_phi)
    return 0;

  int bm_size = (cap + 7) / 8;
  uint8_t *live = tcc_mallocz(bm_size);

#define BM_SET(pos)  (live[(pos) / 8] |= (1u << ((pos) % 8)))
#define BM_TEST(pos) (live[(pos) / 8] &  (1u << ((pos) % 8)))

#define MARK_TEMP_LIVE(vr) do { \
    int32_t _v = (vr); \
    if (_v >= 0 && TCCIR_DECODE_VREG_TYPE(_v) == TCCIR_VREG_TYPE_TEMP) { \
      int _p = TCCIR_DECODE_VREG_POSITION(_v); \
      if (_p < cap && !BM_TEST(_p)) { BM_SET(_p); } \
    } \
  } while (0)

  /* Phase 1: seed liveness from essential instructions.
   * An "essential use" is any operand read by a non-ASSIGN instruction,
   * plus ASSIGN sources when the dest is a non-TEMP (value escapes SSA). */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_ASSIGN) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(d);
      if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP) {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        MARK_TEMP_LIVE(irop_get_vreg(s));
      }
      continue;
    }
    if (irop_config[q->op].has_src1)
      MARK_TEMP_LIVE(irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
    if (irop_config[q->op].has_src2)
      MARK_TEMP_LIVE(irop_get_vreg(tcc_ir_op_get_src2(ir, q)));
    if (q->op == TCCIR_OP_MLA)
      MARK_TEMP_LIVE(irop_get_vreg(tcc_ir_op_get_accum(ir, q)));
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
      MARK_TEMP_LIVE(irop_get_vreg(tcc_ir_op_get_dest(ir, q)));
  }

  /* Phase 2: propagate liveness backward through ASSIGN chains and phi edges. */
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int i = 0; i < ir->next_instruction_index; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN)
        continue;
      int32_t dv = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int dp = TCCIR_DECODE_VREG_POSITION(dv);
      if (dp >= cap || !BM_TEST(dp))
        continue;
      int32_t sv = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (sv < 0 || TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int sp = TCCIR_DECODE_VREG_POSITION(sv);
      if (sp >= cap || BM_TEST(sp))
        continue;
      BM_SET(sp);
      changed = 1;
    }
    for (int b = 0; b < cfg->num_blocks; b++) {
      for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
        int32_t dv = phi->dest_vreg;
        if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
          continue;
        int dp = TCCIR_DECODE_VREG_POSITION(dv);
        if (dp >= cap || !BM_TEST(dp))
          continue;
        for (int pi = 0; pi < phi->num_operands; pi++) {
          int32_t ov = phi->operands[pi].vreg;
          if (ov < 0 || TCCIR_DECODE_VREG_TYPE(ov) != TCCIR_VREG_TYPE_TEMP)
            continue;
          int op = TCCIR_DECODE_VREG_POSITION(ov);
          if (op >= cap || BM_TEST(op))
            continue;
          BM_SET(op);
          changed = 1;
        }
      }
    }
  }

  /* Phase 3: remove phi nodes whose dest TEMP is not live. */
  int changes = 0;
  for (int b = 0; b < cfg->num_blocks; b++) {
    IRPhiNode **pp = &ssa->block_phis[b];
    while (*pp) {
      IRPhiNode *phi = *pp;
      int32_t dv = phi->dest_vreg;
      if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP) {
        int dp = TCCIR_DECODE_VREG_POSITION(dv);
        if (dp < cap && !BM_TEST(dp)) {
          for (int pi = 0; pi < phi->num_operands; pi++) {
            IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->operands[pi].vreg);
            if (vi && vi->use_count > 0)
              vi->use_count--;
          }
          *pp = phi->next;
          changes++;
          continue;
        }
      }
      pp = &phi->next;
    }
  }

  if (changes)
    changes += dce_temp_worklist(ctx);

#undef BM_SET
#undef BM_TEST
#undef MARK_TEMP_LIVE

  tcc_free(live);
  return changes;
}

/* Dead-overwrite store elimination for stack memory.
 *
 * Walks each basic block forward, tracking pending STORE/STORE_INDEXED
 * to canonical (sym, stack-offset) pairs.  When a later STORE to the same
 * offset with the same access width is seen with no intervening read of
 * that location and no call, the earlier STORE is dead.
 *
 * The conventional pre-SSA store_redundant pass only handles plain STORE
 * with direct StackLoc/SymRef dests.  This sub-pass extends to
 * STORE_INDEXED with a LEA-resolved TEMP base — common after SSA load CSE
 * + algebraic folds collapse the read between two writes (pr60502.c). */
static int sl_store_byte_width(int btype)
{
  switch (btype) {
  case IROP_BTYPE_INT8:   return 1;
  case IROP_BTYPE_INT16:  return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32: return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64: return 8;
  default: return 0;
  }
}

/* Resolve a STORE/STORE_INDEXED instruction's destination to a stack-offset
 * range [off, off+width).  Returns 1 if successfully resolved, else 0.  Also
 * returns 0 if the store can alias unknown memory (unresolved pointer base).
 *
 * For STORE: dest is `T_lval_DEREF` where T resolves to LEA-StackLoc; or
 * direct STACKOFF with is_lval=1+is_local=1.
 * For STORE_INDEXED: dest is `T_base` (non-lval pointer), with constant
 * index + scale=0; combined via ssa_opt_indirect_stack_offset. */
static int sl_resolve_store_offset(IRSSAOptCtx *ctx, int instr_idx,
                                   int *out_off, int *out_width, int *out_unknown)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  *out_unknown = 0;

  if (q->op == TCCIR_OP_STORE_INDEXED) {
    int eff = ssa_opt_indirect_stack_offset(ctx, q, SSA_OPT_INDIRECT_DEST);
    if (eff == INT_MIN) {
      *out_unknown = 1;
      return 0;
    }
    /* Access width comes from the stored VALUE (src1), not the base pointer
     * dest — the dest's btype is the pointer's btype (typically 0/NONE), not
     * the access width. */
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int w = sl_store_byte_width(irop_get_btype(src1));
    if (w == 0) {
      *out_unknown = 1;
      return 0;
    }
    *out_off = eff;
    *out_width = w;
    return 1;
  }

  if (q->op != TCCIR_OP_STORE)
    return 0;

  /* Direct STACKOFF dest with is_lval=1+is_local=1 → known stack store. */
  if (dest.tag == IROP_TAG_STACKOFF && dest.is_lval && dest.is_local && !dest.is_llocal) {
    int32_t dv = irop_get_vreg(dest);
    if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_VAR) {
      int w = sl_store_byte_width(irop_get_btype(dest));
      if (w == 0) {
        *out_unknown = 1;
        return 0;
      }
      *out_off = irop_get_stack_offset(dest);
      *out_width = w;
      return 1;
    }
  }

  /* `T_DEREF = src` where T resolves to LEA(StackLoc). */
  if (dest.tag == IROP_TAG_VREG && dest.is_lval) {
    int32_t dv = irop_get_vreg(dest);
    if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP) {
      int eff = ssa_opt_resolve_lea_stackloc(ctx, dv);
      if (eff != INT_MIN) {
        int w = sl_store_byte_width(irop_get_btype(dest));
        if (w == 0) {
          *out_unknown = 1;
          return 0;
        }
        *out_off = eff;
        *out_width = w;
        return 1;
      }
    }
    /* TEMP-DEREF store through an unresolvable pointer — may alias. */
    *out_unknown = 1;
    return 0;
  }

  /* Other patterns (e.g. global symref): treat as may-alias. */
  *out_unknown = 1;
  return 0;
}

/* Resolve a LOAD/LOAD_INDEXED instruction's source to a stack-offset range.
 * Same return semantics as sl_resolve_store_offset. */
static int sl_resolve_load_offset(IRSSAOptCtx *ctx, int instr_idx,
                                  int *out_off, int *out_width, int *out_unknown)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  *out_unknown = 0;

  if (q->op == TCCIR_OP_LOAD_INDEXED) {
    int eff = ssa_opt_indirect_stack_offset(ctx, q, SSA_OPT_INDIRECT_SRC1);
    if (eff == INT_MIN) {
      *out_unknown = 1;
      return 0;
    }
    int w = sl_store_byte_width(irop_get_btype(tcc_ir_op_get_dest(ir, q)));
    if (w == 0) {
      *out_unknown = 1;
      return 0;
    }
    *out_off = eff;
    *out_width = w;
    return 1;
  }

  if (q->op != TCCIR_OP_LOAD)
    return 0;

  if (src1.tag == IROP_TAG_STACKOFF && src1.is_lval && src1.is_local && !src1.is_llocal) {
    int32_t sv = irop_get_vreg(src1);
    if (sv < 0 || TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_VAR) {
      int w = sl_store_byte_width(irop_get_btype(src1));
      if (w == 0) {
        *out_unknown = 1;
        return 0;
      }
      *out_off = irop_get_stack_offset(src1);
      *out_width = w;
      return 1;
    }
  }

  if (src1.tag == IROP_TAG_VREG && src1.is_lval) {
    int32_t sv = irop_get_vreg(src1);
    if (sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP) {
      int eff = ssa_opt_resolve_lea_stackloc(ctx, sv);
      if (eff != INT_MIN) {
        int w = sl_store_byte_width(irop_get_btype(tcc_ir_op_get_dest(ir, q)));
        if (w == 0) {
          *out_unknown = 1;
          return 0;
        }
        *out_off = eff;
        *out_width = w;
        return 1;
      }
    }
    *out_unknown = 1;
    return 0;
  }

  *out_unknown = 1;
  return 0;
}

static int dce_dead_overwrite_stores(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  int n = ir->next_instruction_index;
  int changes = 0;

  if (!cfg || cfg->num_blocks == 0 || n == 0)
    return 0;

#define DOS_PEND_MAX 32
  typedef struct { int idx; int off; int width; } DosPending;

  for (int b = 0; b < cfg->num_blocks; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    DosPending pending[DOS_PEND_MAX];
    int npending = 0;

    for (int i = bb->start_idx; i < bb->end_idx; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Calls or block terminators: clear pending state (may read/write
       * arbitrary memory through escaped pointers). */
      if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL ||
          q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
          q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
          q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID) {
        npending = 0;
        continue;
      }

      /* LOAD / LOAD_INDEXED of a known stack offset evicts overlapping
       * pending stores (the value is observed, so the prior store must
       * remain).  LOADs through external pointers (PARAM/unresolved
       * TEMPs) cannot reach our tracked local-stack offsets, so do NOT
       * clear pending in that case — local stack memory is unreachable
       * from caller-supplied pointers. */
      if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_LOAD_INDEXED ||
          q->op == TCCIR_OP_LOAD_POSTINC) {
        int lo = 0, lw = 0, lu = 0;
        if (sl_resolve_load_offset(ctx, i, &lo, &lw, &lu)) {
          for (int k = 0; k < npending;) {
            int po = pending[k].off, pw = pending[k].width;
            if (lo < po + pw && lo + lw > po)
              pending[k] = pending[--npending];
            else
              k++;
          }
        }
        continue;
      }

      /* STORE / STORE_INDEXED handling */
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
        int so = 0, sw = 0, su = 0;
        (void)su;
        int resolved = sl_resolve_store_offset(ctx, i, &so, &sw, &su);
        if (!resolved) {
          /* Unresolved STORE through an external/global pointer: cannot
           * reach our local-stack pending entries (only the caller's stack
           * or globals).  Leave pending alone — mirrors the conventional
           * store_redundant pass's handling. */
          continue;
        }

        /* Look for an exact same-offset same-width pending store: that one
         * is dead (overwritten without intervening read).  Also evict any
         * pending stores that overlap with the new write's range, since
         * their tracked value is now partially clobbered. */
        for (int k = 0; k < npending;) {
          int po = pending[k].off, pw = pending[k].width;
          if (po == so && pw == sw) {
            /* Exact overwrite — older store is dead. */
            ssa_opt_nop_instr(ctx, pending[k].idx);
            changes++;
            pending[k] = pending[--npending];
          } else if (so < po + pw && so + sw > po) {
            /* Partial overlap — drop tracking (can't prove older is dead). */
            pending[k] = pending[--npending];
          } else {
            k++;
          }
        }

        /* Track this store. */
        if (npending < DOS_PEND_MAX) {
          pending[npending].idx = i;
          pending[npending].off = so;
          pending[npending].width = sw;
          npending++;
        }
        continue;
      }

      /* STORE_POSTINC: clear pending — base updates make offset tracking
       * unreliable. */
      if (q->op == TCCIR_OP_STORE_POSTINC) {
        npending = 0;
        continue;
      }

      /* Other ops: may consume a LEA-typed TEMP as src1/src2.  If the src
       * is a STACKOFF/VREG that resolves to a tracked offset, treat as a
       * read (evict).  Common: BLOCK_COPY/memmove receives a stack address
       * to read from. */
      for (int side = 0; side < 2; side++) {
        IROperand s = side ? tcc_ir_op_get_src2(ir, q) : tcc_ir_op_get_src1(ir, q);
        if (s.is_lval || s.tag != IROP_TAG_VREG)
          continue;
        int32_t sv = irop_get_vreg(s);
        if (sv < 0 || TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_TEMP)
          continue;
        int eff = ssa_opt_resolve_lea_stackloc(ctx, sv);
        if (eff == INT_MIN)
          continue;
        /* This op holds an address into the stack — may read from there.
         * Without size info, conservatively evict any entries that could
         * overlap a 16-byte access window starting at eff. */
        for (int k = 0; k < npending;) {
          int po = pending[k].off, pw = pending[k].width;
          if (po + pw > eff && po < eff + 256)
            pending[k] = pending[--npending];
          else
            k++;
        }
      }
    }
  }

#undef DOS_PEND_MAX
  return changes;
}

int ssa_opt_dce(IRSSAOptCtx *ctx)
{
  int changes = 0;

  changes += dce_temp_worklist(ctx);
  changes += dce_unreachable(ctx);
  if (tcc_state->optimize >= 1) {
    int inner;
    do {
      inner = 0;
      inner += dce_dead_var_stores(ctx);
      if (inner)
        inner += dce_temp_worklist(ctx);
      changes += inner;
    } while (inner > 0);
    changes += dce_dead_overwrite_stores(ctx);
    changes += dce_dead_stackloc_stores(ctx);
    if (changes) {
      /* Repair stale TEMP use counts: some passes NOP instructions
       * without fully updating the use-def chains.  Rebuild accurate
       * counts in O(n) so the final temp worklist can cascade. */
      for (int p = 0; p < ctx->vinfo_cap; p++)
        ctx->vinfo[p].use_count = 0;
      for (int i = 0; i < ctx->ir->next_instruction_index; i++) {
        IRQuadCompact *q = &ctx->ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_src1) {
          IRSSAVregInfo *vi = ssa_opt_vinfo(ctx,
              irop_get_vreg(tcc_ir_op_get_src1(ctx->ir, q)));
          if (vi) vi->use_count++;
        }
        if (irop_config[q->op].has_src2) {
          IRSSAVregInfo *vi = ssa_opt_vinfo(ctx,
              irop_get_vreg(tcc_ir_op_get_src2(ctx->ir, q)));
          if (vi) vi->use_count++;
        }
        if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
            q->op == TCCIR_OP_STORE_POSTINC) {
          IROperand d = tcc_ir_op_get_dest(ctx->ir, q);
          /* STORE with non-lval VREG dest is a value def, not a memory
           * write — dest is not a use.  See ssa_opt_scan_instr_uses. */
          if (q->op != TCCIR_OP_STORE || d.is_lval) {
            IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(d));
            if (vi) vi->use_count++;
          }
        }
        if (q->op == TCCIR_OP_MLA) {
          IRSSAVregInfo *vi = ssa_opt_vinfo(ctx,
              irop_get_vreg(tcc_ir_op_get_accum(ctx->ir, q)));
          if (vi) vi->use_count++;
        }
      }
      /* Count phi operand uses */
      for (int b = 0; b < ctx->cfg->num_blocks; b++) {
        for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next) {
          for (int pi = 0; pi < phi->num_operands; pi++) {
            IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->operands[pi].vreg);
            if (vi) vi->use_count++;
          }
        }
      }
      changes += dce_temp_worklist(ctx);
    }
    changes += dce_dead_phi_cycles(ctx);
  }

  return changes;
}
