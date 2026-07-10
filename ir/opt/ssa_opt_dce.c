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
#include "opt_dsl_phi.h"
#include <limits.h>

extern int tcc_ir_opt_pass_disabled(const char *name);
extern int tcc_ir_callee_is_noreturn(struct Sym *callee);

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

/* Transitive CFG reachability from entry: NOP any instruction unreachable
 * through jumps/branches/switch targets/fall-through.  Runs at every -O level
 * (GCC removes unreachable code even at -O0), so it must catch labeled blocks
 * whose only predecessors are themselves unreachable — a linear post-terminator
 * sweep can't.  Ported from the flat tcc_ir_opt_dce reachability walk. */
static int dce_unreachable(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* IJUMP targets are runtime-computed; reachability can't be proven. */
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;

  uint8_t *reach = tcc_mallocz((n + 7) / 8);
  int *wl = tcc_malloc(n * sizeof(int));
  int head = 0, tail = 0;
#define DCE_MARK(idx)                                                                                                   \
  do {                                                                                                                  \
    int _i = (idx);                                                                                                     \
    if (_i >= 0 && _i < n && !(reach[_i / 8] & (1 << (_i % 8)))) {                                                      \
      reach[_i / 8] |= (1 << (_i % 8));                                                                                 \
      wl[tail++] = _i;                                                                                                  \
    }                                                                                                                   \
  } while (0)

  DCE_MARK(0);
  while (head < tail) {
    int i = wl[head++];
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op) {
    case TCCIR_OP_JUMP:
      DCE_MARK((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      break;
    case TCCIR_OP_JUMPIF:
      DCE_MARK((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      DCE_MARK(i + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE: {
      int tid = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (tid >= 0 && tid < ir->num_switch_tables) {
        TCCIRSwitchTable *t = &ir->switch_tables[tid];
        for (int j = 0; j < t->num_entries; j++)
          DCE_MARK(t->targets[j]);
        DCE_MARK(t->default_target);
      }
      break;
    }
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      break;
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID: {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!tcc_ir_callee_is_noreturn(callee))
        DCE_MARK(i + 1);
      break;
    }
    default:
      DCE_MARK(i + 1);
      break;
    }
  }
#undef DCE_MARK

  int changes = 0;
  for (int i = 0; i < n; i++) {
    if (reach[i / 8] & (1 << (i % 8)))
      continue;
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    ssa_opt_nop_instr(ctx, i);
    changes++;
  }
  tcc_free(reach);
  tcc_free(wl);
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
     * is safe to eliminate.
     *
     * STORE_INDEXED/STORE_POSTINC dest is always a pointer use (base address
     * of the indexed access), even when the variable is local — the indexed
     * store reads the base address, it doesn't define it. */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR &&
          (q->op == TCCIR_OP_STORE_INDEXED || (d.is_lval && !d.is_local))) {
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
    /* Complex lval reads cover real+imag, not just the component btype. */
    int w = (irop_get_btype(op) == IROP_BTYPE_STRUCT || op.is_complex)
                ? 0
                : sl_access_width(op);
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

/* Orphaned FUNCPARAM elimination: NOP FUNCPARAMVAL/FUNCPARAMVOID whose call_id
 * has no matching (live) FUNCCALLVAL/FUNCCALLVOID.  Left behind when a call is
 * inlined away — the sret-buffer address param survives and range-marks the
 * whole frame as escaped, blocking dead-stackloc-store elimination (torture
 * 20020920-1: inlined f() leaves PARAM0 Addr[StackLoc], pinning 13 dead stores). */
static int dce_orphan_params(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int max_cid = 0, saw_param = 0;
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op != TCCIR_OP_FUNCPARAMVAL && op != TCCIR_OP_FUNCPARAMVOID &&
        op != TCCIR_OP_FUNCCALLVAL && op != TCCIR_OP_FUNCCALLVOID)
      continue;
    if (op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID)
      saw_param = 1;
    int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(
        ir, tcc_ir_op_get_src2(ir, &ir->compact_instructions[i])));
    if (cid > max_cid)
      max_cid = cid;
  }
  if (!saw_param || max_cid <= 0)
    return 0;

  int nbytes = (max_cid / 8) + 1;
  uint8_t *has_call = tcc_mallocz(nbytes);
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op != TCCIR_OP_FUNCCALLVAL && op != TCCIR_OP_FUNCCALLVOID)
      continue;
    int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(
        ir, tcc_ir_op_get_src2(ir, &ir->compact_instructions[i])));
    if (cid >= 0 && cid <= max_cid)
      has_call[cid / 8] |= (uint8_t)(1 << (cid % 8));
  }

  int changes = 0;
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op != TCCIR_OP_FUNCPARAMVAL && op != TCCIR_OP_FUNCPARAMVOID)
      continue;
    int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(
        ir, tcc_ir_op_get_src2(ir, &ir->compact_instructions[i])));
    if (cid < 0 || cid > max_cid ||
        !(has_call[cid / 8] & (1 << (cid % 8)))) {
      ssa_opt_nop_instr(ctx, i);
      changes++;
    }
  }
  tcc_free(has_call);
  return changes;
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
      /* FUNCPARAM scalar lval src is a by-value slot read; sl_mark_op already
       * range-escapes non-lval (address) and struct-typed param sources. */
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

/* Operand is a read that can't touch frame memory: immediate, or a non-lval
 * TEMP vreg.  VAR/PARAM vregs share storage with their home slot — for an
 * address-taken one, a store through its address redefines the value the
 * vreg read observes (pr85095 `__builtin_add_overflow(a, b, &a); return a+i`). */
static int dce_ret_op_is_pure_read(TCCIRState *ir, IROperand op)
{
  (void)ir;
  if (op.is_lval)
    return 0;
  if (op.tag != IROP_TAG_VREG)
    return 1;
  int32_t vr = irop_get_vreg(op);
  return vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP;
}

/* Return-path frame-store DSE: a STORE to local frame memory (direct anon
 * StackLoc, or *TEMP resolving to a frame slot) followed only by pure
 * non-memory ops until RETURN is unobservable — the frame dies at return,
 * so even an escaped address cannot legally read it afterwards.  Catches the
 * bitfield write-back left dead once load_cse forwards its re-read
 * (20040709-2 fn1* after ssa:struct_copy_roundtrip). */
static int dce_ret_path_frame_store(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int changes = 0;

  /* Static chain: a nested function's "frame" store may resolve to the
   * PARENT's live frame (pr22061-3 `N += 4` through sl) — skip entirely,
   * like dce_dead_stackloc_stores. */
  if (ir->has_static_chain)
    return 0;
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!dest.is_lval || dest.is_llocal)
      continue;
    int frame_slot = 0;
    if (dest.is_local && irop_get_vreg(dest) < 0) {
      frame_slot = 1;
    } else if (!dest.is_local && !dest.is_sym && dest.tag == IROP_TAG_VREG) {
      int32_t pvr = irop_get_vreg(dest);
      if (pvr >= 0 && TCCIR_DECODE_VREG_TYPE(pvr) == TCCIR_VREG_TYPE_TEMP &&
          ssa_opt_resolve_lea_stackloc(ctx, pvr) != INT_MIN)
        frame_slot = 1;
    }
    if (!frame_slot)
      continue;

    int dead = 0;
    for (int j = i + 1; j < n; j++) {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP)
        continue;
      if (jq->op == TCCIR_OP_RETURNVOID) {
        dead = 1;
        break;
      }
      if (jq->op == TCCIR_OP_RETURNVALUE) {
        dead = dce_ret_op_is_pure_read(ir, tcc_ir_op_get_src1(ir, jq));
        break;
      }
      if (ssa_opt_has_side_effects(jq->op) || jq->op == TCCIR_OP_LOAD ||
          jq->op == TCCIR_OP_LOAD_INDEXED || jq->op == TCCIR_OP_LOAD_POSTINC ||
          jq->op == TCCIR_OP_SELECT)
        break;
      if ((irop_config[jq->op].has_src1 && !dce_ret_op_is_pure_read(ir, tcc_ir_op_get_src1(ir, jq))) ||
          (irop_config[jq->op].has_src2 && !dce_ret_op_is_pure_read(ir, tcc_ir_op_get_src2(ir, jq))))
        break;
      if (jq->op == TCCIR_OP_MLA && !dce_ret_op_is_pure_read(ir, tcc_ir_op_get_accum(ir, jq)))
        break;
    }
    if (dead) {
      ssa_opt_nop_instr(ctx, i);
      changes++;
    }
  }
  return changes;
}

static int ssa_dce_block_in_backedge_region(IRCFG *cfg, int block)
{
  if (!cfg || block < 0 || block >= cfg->num_blocks)
    return 0;

  IRBasicBlock *bb = &cfg->blocks[block];
  for (int h = 0; h < cfg->num_blocks; h++) {
    IRBasicBlock *header = &cfg->blocks[h];
    for (int i = 0; i < header->num_preds; i++) {
      int pred = header->preds[i];
      if (pred < 0 || pred >= cfg->num_blocks)
        continue;
      IRBasicBlock *latch = &cfg->blocks[pred];
      if (pred != h && latch->start_idx < header->start_idx)
        continue;
      if (bb->start_idx >= header->start_idx &&
          bb->start_idx <= latch->start_idx)
        return 1;
    }
  }
  return 0;
}

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
    int in_backedge_region = ssa_dce_block_in_backedge_region(cfg, b);
    IRPhiNode **pp = &ssa->block_phis[b];
    while (*pp) {
      IRPhiNode *phi = *pp;
      int32_t dv = phi->dest_vreg;
      if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP) {
        int dp = TCCIR_DECODE_VREG_POSITION(dv);
        if (dp < cap && !BM_TEST(dp)) {
          /* Phis in a natural back-edge region are not just value uses: phi
           * resolution needs them to carry state through loop iterations.
           * Removing such a phi can make out-of-SSA conflate a loop-carried
           * value with its source even when the visible ASSIGN/phi graph
           * looks dead (fp_round seed 18960). */
          if (in_backedge_region) {
            pp = &phi->next;
            continue;
          }
          if (getenv("TCC_DBG_PHI_CYCLES")) {
            fprintf(stderr, "[phi_cycles] remove phi block=%d dest=T%d ops:", b, dp);
            for (int pi = 0; pi < phi->num_operands; pi++)
              fprintf(stderr, " %d", phi->operands[pi].vreg);
            fprintf(stderr, "\n");
          }
          if (!opt_dsl_phi_remove(ctx, b, pp)) {
            pp = &phi->next;
            continue;
          }
          changes++;
          continue;
        }
      }
      pp = &phi->next;
    }
  }

  if (changes) {
    /* Rebuild before cascading through dead TEMP definitions. */
    for (int p = 0; p < ctx->vinfo_cap; p++)
      ctx->vinfo[p].use_count = 0;
    for (int i = 0; i < ir->next_instruction_index; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      ssa_opt_scan_instr_uses(ctx, i, q);
    }
    for (int b = 0; b < cfg->num_blocks; b++) {
      for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
        for (int pi = 0; pi < phi->num_operands; pi++) {
          IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->operands[pi].vreg);
          if (vi)
            ssa_opt_add_use_phi(vi, b, pi);
        }
      }
    }
    changes += dce_temp_worklist(ctx);
  }

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
/* out_base names the memory object so distinct address-taken locals are not
 * conflated: -1 = a real anonymous stack slot whose out_off is authoritative;
 * >=0 = a `&VAR` address whose out_off is only a within-object field offset
 * shared (placeholder 0) by every distinct VAR (see ssa_opt_resolve_lea_stackloc_ex).
 * Two stores alias only when their (base, offset) ranges match — matching on
 * offset alone would fold `*p1=1;*p2=2` (both &VAR at offset 0) into one. */
static int sl_resolve_store_offset(IRSSAOptCtx *ctx, int instr_idx,
                                   int32_t *out_base, int *out_off, int *out_width,
                                   int *out_unknown)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  *out_unknown = 0;
  *out_base = -1;

  if (q->op == TCCIR_OP_STORE_INDEXED) {
    int32_t bv = -1;
    int eff = ssa_opt_indirect_stack_offset_ex(ctx, q, SSA_OPT_INDIRECT_DEST, &bv);
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
    *out_base = bv;
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
      int32_t bv = -1;
      int eff = ssa_opt_resolve_lea_stackloc_ex(ctx, dv, &bv);
      if (eff != INT_MIN) {
        int w = sl_store_byte_width(irop_get_btype(dest));
        if (w == 0) {
          *out_unknown = 1;
          return 0;
        }
        *out_base = bv;
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

  /* ASSIGN with deref source is a load — `T3 <-- T2***DEREF*** [ASSIGN]`. */
  if (q->op != TCCIR_OP_LOAD &&
      !(q->op == TCCIR_OP_ASSIGN && src1.is_lval))
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
  typedef struct { int idx; int32_t base; int off; int width; } DosPending;

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

      /* Source-side memory reads: any op with an is_lval source operand is a
       * memory load through that operand (LOAD/ASSIGN/LOAD-fused arithmetic
       * like `T <-- *P ADD #1`).  Each such read evicts overlapping pending
       * stores so the prior store stays live.  An unresolvable deref could
       * alias any tracked store — clear pending entirely. */
      int saw_unresolved_deref = 0;
      for (int side = 0; side < 2; side++) {
        if (side == 0 && !irop_config[q->op].has_src1)
          continue;
        if (side == 1 && !irop_config[q->op].has_src2)
          continue;
        IROperand s = side ? tcc_ir_op_get_src2(ir, q) : tcc_ir_op_get_src1(ir, q);
        if (!s.is_lval)
          continue;
        int eff = INT_MIN;
        int width = sl_store_byte_width(irop_get_btype(s));
        if (s.tag == IROP_TAG_STACKOFF && s.is_local && !s.is_llocal) {
          int32_t sv = irop_get_vreg(s);
          if (sv < 0 || TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_VAR)
            eff = irop_get_stack_offset(s);
        } else if (s.tag == IROP_TAG_VREG) {
          int32_t sv = irop_get_vreg(s);
          if (sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP)
            eff = ssa_opt_resolve_lea_stackloc(ctx, sv);
        }
        if (eff == INT_MIN || width == 0) {
          saw_unresolved_deref = 1;
          break;
        }
        for (int k = 0; k < npending;) {
          int po = pending[k].off, pw = pending[k].width;
          if (eff < po + pw && eff + width > po)
            pending[k] = pending[--npending];
          else
            k++;
        }
      }
      if (saw_unresolved_deref) {
        npending = 0;
        continue;
      }

      /* LOAD_INDEXED / LOAD_POSTINC: pointer + index form (src1 is non-lval
       * base).  The generic sweep above won't catch these; use the dedicated
       * resolver. */
      if (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_LOAD_POSTINC) {
        int lo = 0, lw = 0, lu = 0;
        if (sl_resolve_load_offset(ctx, i, &lo, &lw, &lu)) {
          for (int k = 0; k < npending;) {
            int po = pending[k].off, pw = pending[k].width;
            if (lo < po + pw && lo + lw > po)
              pending[k] = pending[--npending];
            else
              k++;
          }
        } else if (lu) {
          npending = 0;
        }
        continue;
      }

      /* Plain LOAD with a resolvable address — already evicted via the
       * source-side sweep above when src1.is_lval.  If the LOAD's src1 isn't
       * is_lval (degenerate IR), bail conservatively. */
      if (q->op == TCCIR_OP_LOAD)
        continue;

      /* STORE / STORE_INDEXED handling */
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
        int so = 0, sw = 0, su = 0;
        int32_t sbase = -1;
        (void)su;
        int resolved = sl_resolve_store_offset(ctx, i, &sbase, &so, &sw, &su);
        if (!resolved) {
          /* Unresolved STORE through an external/global pointer: cannot
           * reach our local-stack pending entries (only the caller's stack
           * or globals).  Leave pending alone — mirrors the conventional
           * store_redundant pass's handling. */
          continue;
        }

        /* Look for an exact same-object same-offset same-width pending store:
         * that one is dead (overwritten without intervening read).  Also evict
         * any same-object pending stores that overlap the new write's range,
         * since their tracked value is now partially clobbered.  Stores to a
         * different object (base) never alias. */
        for (int k = 0; k < npending;) {
          if (pending[k].base != sbase) {
            k++;
            continue;
          }
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
          pending[npending].base = sbase;
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
        if (side == 0 && !irop_config[q->op].has_src1)
          continue;
        if (side == 1 && !irop_config[q->op].has_src2)
          continue;
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

/* ============================================================================
 * Dead-overwrite store elimination for GLOBAL memory (symref targets).
 *
 * Companion to dce_dead_overwrite_stores, which only tracks stack slots.  A
 * `STORE g <- a; STORE g <- b` with no intervening read of `g` makes the
 * first store dead.  Globals appear either as a direct
 * `GlobalSym(S)***DEREF***` operand, or — after global_base_share rewrites a
 * store cluster — as `STORE_INDEXED T,#idx` with `T = GlobalSym(S) (+ #k)`.
 *
 * The SSA analog of the legacy flat tcc_ir_opt_store_redundant global path.
 * Kept as a separate, block-local pass so the validated stack DSE above is
 * untouched.  Two globals with distinct Sym* never alias; a global never
 * aliases the stack — so keying every pending entry on (sym, offset) isolates
 * the classes.  Conservative flushes: any call / side-effecting op (callee may
 * read globals), any read through an unresolved pointer, and any store through
 * an unresolved pointer (may alias a tracked global).
 * ============================================================================ */

/* Resolve a TEMP holding a global address `GlobalSym(S) (+ #k)` — through
 * single-def ASSIGN/LEA copies and ADD/SUB #imm — to (sym, addend).  Mirrors
 * ssa_opt_resolve_lea_stackloc but terminates on a non-lval symref source. */
static int gs_resolve_global_base(IRSSAOptCtx *ctx, int32_t vr,
                                  const Sym **out_sym, int64_t *out_off)
{
  TCCIRState *ir = ctx->ir;
  int64_t acc = 0;
  for (int hop = 0; hop < 64; hop++) {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_instr < 0 || vi->def_count > 1)
      return 0;
    IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
    if (dq->op == TCCIR_OP_LEA || dq->op == TCCIR_OP_ASSIGN ||
        (dq->op == TCCIR_OP_STORE && !tcc_ir_op_get_dest(ir, dq).is_lval)) {
      IROperand src = tcc_ir_op_get_src1(ir, dq);
      if (src.is_sym && !src.is_lval) {
        IRPoolSymref *sr = irop_get_symref_ex(ir, src);
        if (!sr || !sr->sym)
          return 0;
        *out_sym = sr->sym;
        *out_off = sr->addend + acc;
        return 1;
      }
      int32_t sv = irop_get_vreg(src);
      if (sv >= 0 && !src.is_lval && irop_get_tag(src) == IROP_TAG_VREG) {
        vr = sv;
        continue;
      }
      return 0;
    }
    if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB) {
      IROperand s1 = tcc_ir_op_get_src1(ir, dq);
      IROperand s2 = tcc_ir_op_get_src2(ir, dq);
      if (!s1.is_lval && irop_is_immediate(s2)) {
        int32_t s1vr = irop_get_vreg(s1);
        if (s1vr >= 0) {
          int d = irop_get_imm32(s2);
          acc += (dq->op == TCCIR_OP_ADD) ? d : -d;
          vr = s1vr;
          continue;
        }
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

typedef struct { const Sym *sym; int64_t off; int width; } GsRef;

/* Classify a memory reference for the GLOBAL table:
 *   2  -> exact global ref (GsRef fully populated)
 *   1  -> whole-global range (sym known, offset runtime — GsRef.sym only)
 *   0  -> not a global access (stack / value-def / immediate — no effect)
 *  -1  -> unresolved pointer that may alias any global (flush the table)  */
static int gs_classify_read_op(IRSSAOptCtx *ctx, IROperand s, GsRef *r)
{
  if (!s.is_lval)
    return 0;
  TCCIRState *ir = ctx->ir;
  if (s.is_sym) {
    IRPoolSymref *sr = irop_get_symref_ex(ir, s);
    if (!sr || !sr->sym)
      return -1;
    int w = sl_store_byte_width(irop_get_btype(s));
    if (w == 0)
      return -1;
    r->sym = sr->sym;
    r->off = sr->addend;
    r->width = w;
    return 2;
  }
  if (irop_get_tag(s) == IROP_TAG_STACKOFF)
    return 0;
  if (irop_get_tag(s) == IROP_TAG_VREG) {
    int32_t sv = irop_get_vreg(s);
    if (sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP) {
      if (ssa_opt_resolve_lea_stackloc(ctx, sv) != INT_MIN)
        return 0;
      const Sym *gs;
      int64_t go;
      if (gs_resolve_global_base(ctx, sv, &gs, &go)) {
        int w = sl_store_byte_width(irop_get_btype(s));
        if (w == 0)
          return -1;
        r->sym = gs;
        r->off = go;
        r->width = w;
        return 2;
      }
    }
    return -1;
  }
  return -1;
}

/* Classify the base+index address of a STORE_INDEXED (dest) or
 * LOAD_INDEXED/LOAD_POSTINC (src1).  `width` is the access width. */
static int gs_classify_indexed(IRSSAOptCtx *ctx, const IRQuadCompact *q,
                               int side, int width, GsRef *r)
{
  TCCIRState *ir = ctx->ir;
  if (ssa_opt_indirect_stack_offset(ctx, q, side) != INT_MIN)
    return 0;
  IROperand base = (side == SSA_OPT_INDIRECT_DEST) ? tcc_ir_op_get_dest(ir, q)
                                                   : tcc_ir_op_get_src1(ir, q);
  int32_t bvr = irop_get_vreg(base);
  if (bvr < 0)
    return -1;
  if (ssa_opt_resolve_lea_stackloc(ctx, bvr) != INT_MIN)
    return 0;
  const Sym *gs;
  int64_t go;
  if (!gs_resolve_global_base(ctx, bvr, &gs, &go))
    return -1;
  IROperand idx = tcc_ir_op_get_src2(ir, q);
  if (!irop_is_immediate(idx)) {
    r->sym = gs;
    return 1;
  }
  if (width == 0)
    return -1;
  int64_t sc = 0;
  IROperand scop = tcc_ir_op_get_scale(ir, q);
  if (irop_is_immediate(scop))
    sc = irop_get_imm64_ex(ir, scop);
  r->sym = gs;
  r->off = go + (irop_get_imm64_ex(ir, idx) << sc);
  r->width = width;
  return 2;
}

static int gs_classify_store(IRSSAOptCtx *ctx, const IRQuadCompact *q, GsRef *r)
{
  TCCIRState *ir = ctx->ir;
  if (q->op == TCCIR_OP_STORE_INDEXED) {
    int w = sl_store_byte_width(irop_get_btype(tcc_ir_op_get_src1(ir, q)));
    return gs_classify_indexed(ctx, q, SSA_OPT_INDIRECT_DEST, w, r);
  }
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (!dest.is_lval)
    return 0;
  if (dest.is_sym) {
    IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
    if (!sr || !sr->sym)
      return -1;
    int w = sl_store_byte_width(irop_get_btype(dest));
    if (w == 0)
      return -1;
    r->sym = sr->sym;
    r->off = sr->addend;
    r->width = w;
    return 2;
  }
  if (irop_get_tag(dest) == IROP_TAG_STACKOFF)
    return 0;
  if (irop_get_tag(dest) == IROP_TAG_VREG) {
    int32_t dv = irop_get_vreg(dest);
    if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP) {
      if (ssa_opt_resolve_lea_stackloc(ctx, dv) != INT_MIN)
        return 0;
      const Sym *gs;
      int64_t go;
      if (gs_resolve_global_base(ctx, dv, &gs, &go)) {
        int w = sl_store_byte_width(irop_get_btype(dest));
        if (w == 0)
          return -1;
        r->sym = gs;
        r->off = go;
        r->width = w;
        return 2;
      }
    }
    return -1;
  }
  return -1;
}

static int dce_dead_global_stores(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  int changes = 0;

  if (!cfg || cfg->num_blocks == 0)
    return 0;

#define GS_PEND_MAX 32
  typedef struct { int idx; const Sym *sym; int64_t off; int width; } GsPending;

  for (int b = 0; b < cfg->num_blocks; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    GsPending pending[GS_PEND_MAX];
    int npending = 0;

    for (int i = bb->start_idx; i < bb->end_idx; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      int is_store = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED);
      int flush = 0;

      /* Reads: scan operands of any non-side-effecting op, plus STOREs
       * (whose value operand may itself be a global deref). */
      if (!ssa_opt_has_side_effects(q->op) || is_store) {
        for (int side = 0; side < 3 && !flush; side++) {
          IROperand s;
          if (side == 0 && irop_config[q->op].has_src1)
            s = tcc_ir_op_get_src1(ir, q);
          else if (side == 1 && irop_config[q->op].has_src2)
            s = tcc_ir_op_get_src2(ir, q);
          else if (side == 2 && q->op == TCCIR_OP_MLA)
            s = tcc_ir_op_get_accum(ir, q);
          else
            continue;
          GsRef r;
          int k = gs_classify_read_op(ctx, s, &r);
          if (k == -1)
            flush = 1;
          else if (k == 1) {
            for (int p = 0; p < npending;)
              if (pending[p].sym == r.sym)
                pending[p] = pending[--npending];
              else
                p++;
          } else if (k == 2) {
            for (int p = 0; p < npending;)
              if (pending[p].sym == r.sym && r.off < pending[p].off + pending[p].width &&
                  r.off + r.width > pending[p].off)
                pending[p] = pending[--npending];
              else
                p++;
          }
        }
        if (!flush &&
            (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_LOAD_POSTINC)) {
          int w = sl_store_byte_width(irop_get_btype(tcc_ir_op_get_dest(ir, q)));
          GsRef r;
          int k = gs_classify_indexed(ctx, q, SSA_OPT_INDIRECT_SRC1, w, &r);
          if (k == -1)
            flush = 1;
          else if (k == 1) {
            for (int p = 0; p < npending;)
              if (pending[p].sym == r.sym)
                pending[p] = pending[--npending];
              else
                p++;
          } else if (k == 2) {
            for (int p = 0; p < npending;)
              if (pending[p].sym == r.sym && r.off < pending[p].off + pending[p].width &&
                  r.off + r.width > pending[p].off)
                pending[p] = pending[--npending];
              else
                p++;
          }
        }
      }

      if (flush) {
        npending = 0;
        continue;
      }

      /* Any side-effecting op other than the stores we model (calls, asm,
       * block copies, terminators, postinc, VLA, ...) may read or alias a
       * global — flush conservatively. */
      if (ssa_opt_has_side_effects(q->op) && !is_store) {
        npending = 0;
        continue;
      }

      if (!is_store)
        continue;

      GsRef r;
      int k = gs_classify_store(ctx, q, &r);
      if (k == 0 || k == 1)
        continue; /* stack / value-def, or runtime-offset global store */
      if (k == -1) {
        npending = 0; /* store through unknown pointer — may alias any global */
        continue;
      }
      /* k == 2: exact global store. */
      for (int p = 0; p < npending;) {
        if (pending[p].sym == r.sym && pending[p].off == r.off &&
            pending[p].width == r.width) {
          ssa_opt_nop_instr(ctx, pending[p].idx);
          changes++;
          pending[p] = pending[--npending];
        } else if (pending[p].sym == r.sym && r.off < pending[p].off + pending[p].width &&
                   r.off + r.width > pending[p].off) {
          pending[p] = pending[--npending];
        } else {
          p++;
        }
      }
      if (npending < GS_PEND_MAX) {
        pending[npending].idx = i;
        pending[npending].sym = r.sym;
        pending[npending].off = r.off;
        pending[npending].width = r.width;
        npending++;
      }
    }
  }

#undef GS_PEND_MAX
  return changes;
}

#define VL_BIT(bm, p) ((bm)[(p) >> 5] & (1u << ((p) & 31)))
#define VL_SET(bm, p) ((bm)[(p) >> 5] |= (1u << ((p) & 31)))
#define VL_CLR(bm, p) ((bm)[(p) >> 5] &= ~(1u << ((p) & 31)))

static int vl_var_pos(IROperand op, int num_vars)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return -1;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  return pos < num_vars ? pos : -1;
}

/* STORE dest that writes the VAR's own slot (value-def or direct local
 * lval), as opposed to a write through a pointer held in the VAR. */
static int vl_store_is_slot_def(IROperand d)
{
  if (d.is_sym)
    return 0;
  return !d.is_lval || (d.is_local && !d.is_llocal);
}

/* Full-slot definition of a non-excluded VAR: var position or -1.
 * Side-effect ops never kill (except the STORE slot-def form), so every
 * def this returns is also safe to NOP when dead. */
static int vl_def_pos(TCCIRState *ir, IRQuadCompact *q, int num_vars)
{
  if (!irop_config[q->op].has_dest)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (ssa_opt_has_side_effects(q->op)) {
    if (q->op != TCCIR_OP_STORE || !vl_store_is_slot_def(d))
      return -1;
  } else {
    if (d.is_sym || (d.is_lval && (!d.is_local || d.is_llocal)))
      return -1;
  }
  int p = vl_var_pos(d, num_vars);
  if (p < 0)
    return -1;
  int bt = irop_get_btype(d);
  if (bt == IROP_BTYPE_STRUCT)
    return -1;
  IRLiveInterval *iv =
      tcc_ir_get_live_interval(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p));
  if (iv && (iv->is_llong || iv->is_double) &&
      bt != IROP_BTYPE_INT64 && bt != IROP_BTYPE_FLOAT64)
    return -1;
  return p;
}

static void vl_mark_uses(TCCIRState *ir, IRQuadCompact *q, int num_vars,
                         uint32_t *live)
{
  int p;
  if (irop_config[q->op].has_src1 &&
      (p = vl_var_pos(tcc_ir_op_get_src1(ir, q), num_vars)) >= 0)
    VL_SET(live, p);
  if (irop_config[q->op].has_src2 &&
      (p = vl_var_pos(tcc_ir_op_get_src2(ir, q), num_vars)) >= 0)
    VL_SET(live, p);
  if (q->op == TCCIR_OP_MLA &&
      (p = vl_var_pos(tcc_ir_op_get_accum(ir, q), num_vars)) >= 0)
    VL_SET(live, p);
  if (irop_config[q->op].has_dest &&
      vl_def_pos(ir, q, num_vars) < 0 &&
      (p = vl_var_pos(tcc_ir_op_get_dest(ir, q), num_vars)) >= 0)
    VL_SET(live, p);
}

static int vl_removable(TCCIRState *ir, IRQuadCompact *q)
{
  for (int side = 0; side < 2; side++) {
    IROperand s;
    if (side == 0 && irop_config[q->op].has_src1)
      s = tcc_ir_op_get_src1(ir, q);
    else if (side == 1 && irop_config[q->op].has_src2)
      s = tcc_ir_op_get_src2(ir, q);
    else
      continue;
    int32_t vr = irop_get_vreg(s);
    if (vr >= 0) {
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, vr);
      if (iv && iv->is_volatile)
        return 0;
    }
  }
  return 1;
}

/* CFG-wide backward liveness over local VAR slots: a full-slot def of a
 * VAR that is dead at that point is NOPed.  Generalizes the legacy
 * redundant_init_elim / redundant_var_assign passes to overwrites on any
 * path, not just function-entry inits or straight-line code. */
static int dce_var_liveness(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int num_vars = ir->next_local_variable;
  int changes = 0;

  if (num_vars <= 0 || n == 0 || ir->has_static_chain)
    return 0;

  for (int i = 0; i < n; i++) {
    switch (ir->compact_instructions[i].op) {
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
      return 0;
    default:
      break;
    }
  }

  /* ctx->cfg may be stale after branch rewrites — build a fresh one */
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg || cfg->num_blocks == 0) {
    tcc_ir_cfg_free(cfg);
    return 0;
  }

  int nb = cfg->num_blocks;
  int nw = (num_vars + 31) / 32;
  size_t bmsz = (size_t)nw * sizeof(uint32_t);
  uint32_t *excl = tcc_mallocz(bmsz);
  uint32_t *use = tcc_mallocz((size_t)nb * bmsz);
  uint32_t *def = tcc_mallocz((size_t)nb * bmsz);
  uint32_t *lin = tcc_mallocz((size_t)nb * bmsz);
  uint32_t *live = tcc_mallocz(bmsz);

  for (int p = 0; p < num_vars; p++) {
    IRLiveInterval *iv =
        tcc_ir_get_live_interval(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p));
    if (!iv || iv->addrtaken || iv->is_volatile)
      VL_SET(excl, p);
  }
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int side = 0; side < 3; side++) {
      IROperand s;
      if (side == 0 && irop_config[q->op].has_src1)
        s = tcc_ir_op_get_src1(ir, q);
      else if (side == 1 && irop_config[q->op].has_src2)
        s = tcc_ir_op_get_src2(ir, q);
      else if (side == 2 && q->op == TCCIR_OP_MLA)
        s = tcc_ir_op_get_accum(ir, q);
      else
        continue;
      int p = vl_var_pos(s, num_vars);
      if (p < 0)
        continue;
      /* Addr[V] escapes; any VAR feeding a LEA is address-taken */
      if ((s.is_local && !s.is_lval) || q->op == TCCIR_OP_LEA)
        VL_SET(excl, p);
    }
  }

  for (int b = 0; b < nb; b++) {
    uint32_t *ub = use + (size_t)b * nw;
    uint32_t *db = def + (size_t)b * nw;
    for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      memset(live, 0, bmsz);
      vl_mark_uses(ir, q, num_vars, live);
      for (int w = 0; w < nw; w++)
        ub[w] |= live[w] & ~db[w];
      int dp = vl_def_pos(ir, q, num_vars);
      if (dp >= 0 && !VL_BIT(excl, dp))
        VL_SET(db, dp);
    }
  }

  int unstable = 1, rounds = 0;
  while (unstable && rounds++ <= nb + 2) {
    unstable = 0;
    for (int b = nb - 1; b >= 0; b--) {
      memset(live, 0, bmsz);
      for (int s = 0; s < cfg->blocks[b].num_succs; s++) {
        uint32_t *sin = lin + (size_t)cfg->blocks[b].succs[s] * nw;
        for (int w = 0; w < nw; w++)
          live[w] |= sin[w];
      }
      uint32_t *ub = use + (size_t)b * nw;
      uint32_t *db = def + (size_t)b * nw;
      uint32_t *ib = lin + (size_t)b * nw;
      for (int w = 0; w < nw; w++) {
        uint32_t v = ub[w] | (live[w] & ~db[w]);
        if (v != ib[w]) {
          ib[w] = v;
          unstable = 1;
        }
      }
    }
  }

  if (!unstable) {
    for (int b = 0; b < nb; b++) {
      memset(live, 0, bmsz);
      for (int s = 0; s < cfg->blocks[b].num_succs; s++) {
        uint32_t *sin = lin + (size_t)cfg->blocks[b].succs[s] * nw;
        for (int w = 0; w < nw; w++)
          live[w] |= sin[w];
      }
      for (int i = cfg->blocks[b].end_idx - 1; i >= cfg->blocks[b].start_idx; i--) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        int dp = vl_def_pos(ir, q, num_vars);
        if (dp >= 0 && !VL_BIT(excl, dp)) {
          if (!VL_BIT(live, dp) && vl_removable(ir, q)) {
            ssa_opt_nop_instr(ctx, i);
            changes++;
            continue;
          }
          VL_CLR(live, dp);
        }
        vl_mark_uses(ir, q, num_vars, live);
      }
    }
  }

  tcc_free(excl);
  tcc_free(use);
  tcc_free(def);
  tcc_free(lin);
  tcc_free(live);
  tcc_ir_cfg_free(cfg);
  return changes;
}

int ssa_opt_dce_light(IRSSAOptCtx *ctx)
{
  int changes = dce_unreachable(ctx);
  changes += dce_temp_worklist(ctx);
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
    if (!tcc_ir_opt_pass_disabled("ssa:dce:global_store"))
      changes += dce_dead_global_stores(ctx);
    if (!tcc_ir_opt_pass_disabled("ssa:dce:orphan_params"))
      changes += dce_orphan_params(ctx);
    changes += dce_dead_stackloc_stores(ctx);
    if (!tcc_ir_opt_pass_disabled("ssa:dce:ret_store"))
      changes += dce_ret_path_frame_store(ctx);
    if (!tcc_ir_opt_pass_disabled("ssa:dce:var_live")) {
      int vl = dce_var_liveness(ctx);
      if (vl)
        changes += vl + dce_temp_worklist(ctx);
    }
    if (changes) {
      /* Repair stale TEMP use chains: some passes NOP or rewrite
       * instructions without fully updating the use-def chains.  Rebuild
       * the FULL use lists in O(n), not just the counts — truncating
       * use_count while keeping the old uses[] entries desynchronizes the
       * two, so the surviving prefix can hold a stale entry while a live
       * use falls off the end.  A later replace_all_uses then walks the
       * wrong list, leaves the live use un-rewritten, and this DCE deletes
       * a def that is still referenced (ptr fuzz seed 7226: *p9's pointer
       * temp lost its deref use and the deref read an undefined vreg). */
      for (int p = 0; p < ctx->vinfo_cap; p++)
        ctx->vinfo[p].use_count = 0;
      for (int i = 0; i < ctx->ir->next_instruction_index; i++) {
        IRQuadCompact *q = &ctx->ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        ssa_opt_scan_instr_uses(ctx, i, q);
      }
      /* Rebuild phi operand uses */
      for (int b = 0; b < ctx->cfg->num_blocks; b++) {
        for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next) {
          for (int pi = 0; pi < phi->num_operands; pi++) {
            IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->operands[pi].vreg);
            if (vi)
              ssa_opt_add_use_phi(vi, b, pi);
          }
        }
      }
      changes += dce_temp_worklist(ctx);
    }
    if (!tcc_ir_opt_pass_disabled("ssa:dce:phi_cycles"))
      changes += dce_dead_phi_cycles(ctx);
  }

  return changes;
}
