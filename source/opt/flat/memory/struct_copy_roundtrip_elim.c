/*
 *  TCC IR - Struct-copy round-trip elimination & init-copy global load forwarding (pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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


/* ============================================================================
 * Struct-copy round-trip elimination (tcc_ir_opt_struct_copy_roundtrip_elim)
 * ============================================================================
 *
 * Inlining a by-value identity helper — `struct S retme(struct S x){return x;}`
 * called as `y = retme(y)` — lowers to a pair of struct copies through a fresh
 * temporary slot B (the inlined parameter/return home):
 *
 *     memmove(B, A, N)     ; B := y        (marshal the argument)
 *     memmove(A, B, N)     ; y := result   (copy the return value back)
 *
 * The net effect on A is nothing (A is copied out to B and immediately copied
 * back), and B is a dead temp afterwards.  Both copies are removable when:
 *
 *   - the two calls are adjacent, with no memory-writing op and no other call
 *     between them (so A's region is provably unmodified across the pair); and
 *   - region B [b,b+N) is referenced *only* as C1's destination and C2's
 *     source — i.e. B is a private round-trip buffer, never read elsewhere and
 *     never the destination of any other write.
 *
 * Removing the pair leaves the field load/store of `y.field += x` (which sat
 * between the init copy and the round-trip) directly followed by the return's
 * field load, so the existing sl_forward + bf_insert_extract cascade collapses
 * the bitfield poke/re-extract — which is why this runs just before the memory
 * group.  Targets the 20040709-2 fn1* bitfield idioms (memmove-marshalled
 * struct-by-value through retme).
 */

/* Resolve a call-param operand to a stack-slot address.  The operand is either
 * a direct `Addr[StackLoc[off]]` (is_lval=0) or a TEMP whose single prior def
 * is `T <- Addr[StackLoc[off]]` (LEA / ASSIGN-with-no-src2).  Returns 1 and
 * fills *off / *is_local on success. */
static int scre_resolve_slot_addr(TCCIRState *ir, IROperand op, int before_idx, int32_t *off, int *is_local)
{
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && !op.is_lval)
  {
    *off = irop_get_stack_offset(op);
    *is_local = op.is_local;
    return 1;
  }
  if (!irop_has_vreg(op))
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  for (int d = before_idx - 1; d >= 0; d--)
  {
    IRQuadCompact *dq = &ir->compact_instructions[d];
    if (dq->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[dq->op].has_dest)
      continue;
    IROperand dd = tcc_ir_op_get_dest(ir, dq);
    if (!irop_has_vreg(dd) || irop_get_vreg(dd) != vr)
      continue;
    /* Found the textually latest def of the param vreg BEFORE the use.  That
     * is only THE reaching def if the vreg has no other def anywhere: for a
     * struct-valued ternary `cond ? f() : g()` the address temp is a phi with
     * one `T <- Addr[StackLoc[..]]` per arm, and resolving through the
     * fall-through arm's def eliminated a real cross-slot transfer as a
     * "roundtrip" (the on-device tcc corrupted SELECT folds in its own
     * ssa_rewrite_flag_consumer this way). */
    for (int o = 0; o < ir->next_instruction_index; o++)
    {
      if (o == d)
        continue;
      IRQuadCompact *oq = &ir->compact_instructions[o];
      if (oq->op == TCCIR_OP_NOP || !irop_config[oq->op].has_dest)
        continue;
      if (oq->op == TCCIR_OP_STORE || oq->op == TCCIR_OP_STORE_INDEXED ||
          oq->op == TCCIR_OP_STORE_POSTINC)
        continue; /* store dest is an address use, not a def */
      IROperand od = tcc_ir_op_get_dest(ir, oq);
      if (irop_has_vreg(od) && irop_get_vreg(od) == vr && !od.is_lval)
        return 0;
    }
    if (dq->op != TCCIR_OP_LEA && dq->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
    if (dq->op == TCCIR_OP_ASSIGN && !irop_is_none(tcc_ir_op_get_src2(ir, dq)))
      return 0;
    if (irop_get_tag(ds1) != IROP_TAG_STACKOFF || ds1.is_lval)
      return 0;
    *off = irop_get_stack_offset(ds1);
    *is_local = ds1.is_local;
    return 1;
  }
  return 0;
}

static int scre_is_memcpy_like(TCCIRState *ir, IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
    return 0;
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!callee)
    return 0;
  const char *name = get_tok_str(callee->v, NULL);
  if (!name)
    return 0;
  return ir_opt_is_memcpy_or_memmove_name(name) || strcmp(name, "__tcc_memmove") == 0;
}

/* dst (param0), src (param1), size (param2) as stack slots / constant. */
static int scre_get_copy(TCCIRState *ir, int call_idx, int32_t *dst, int32_t *src, int32_t *size)
{
  IROperand p0, p1, p2;
  int dl, sl;
  if (!ir_opt_get_call_param_operand(ir, call_idx, 0, &p0) ||
      !ir_opt_get_call_param_operand(ir, call_idx, 1, &p1) ||
      !ir_opt_get_call_param_operand(ir, call_idx, 2, &p2))
    return 0;
  if (irop_get_tag(p2) != IROP_TAG_IMM32)
    return 0;
  if (!scre_resolve_slot_addr(ir, p0, call_idx, dst, &dl) || !dl)
    return 0;
  if (!scre_resolve_slot_addr(ir, p1, call_idx, src, &sl) || !sl)
    return 0;
  *size = (int32_t)p2.u.imm32;
  return *size > 0;
}

/* Does instruction q reference (read/write/addr-of) any byte of [lo,lo+sz)? */
static int scre_touches_region(TCCIRState *ir, IRQuadCompact *q, int32_t lo, int32_t sz, int *is_write)
{
  *is_write = 0;
  const IRRegistersConfig *cfg = &irop_config[q->op];
  int touched = 0;
  IROperand ops[3];
  int which[3];
  int nops = 0;
  if (cfg->has_dest) { ops[nops] = tcc_ir_op_get_dest(ir, q); which[nops] = 0; nops++; }
  if (cfg->has_src1) { ops[nops] = tcc_ir_op_get_src1(ir, q); which[nops] = 1; nops++; }
  if (cfg->has_src2) { ops[nops] = tcc_ir_op_get_src2(ir, q); which[nops] = 2; nops++; }
  for (int k = 0; k < nops; k++)
  {
    IROperand o = ops[k];
    if (irop_get_tag(o) != IROP_TAG_STACKOFF || !o.is_local)
      continue;
    int32_t off = irop_get_stack_offset(o);
    if (off < lo + sz && off + 4 > lo) /* conservative 4-byte footprint */
    {
      touched = 1;
      /* A store to this slot, or an lval dest, is a write of the region. */
      if (which[k] == 0 && o.is_lval &&
          (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
           q->op == TCCIR_OP_STORE_POSTINC))
        *is_write = 1;
    }
  }
  return touched;
}

int tcc_ir_opt_struct_copy_roundtrip_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 4)
    return 0;

  for (int i1 = 0; i1 < n; i1++)
  {
    IRQuadCompact *c1 = &ir->compact_instructions[i1];
    if (!scre_is_memcpy_like(ir, c1))
      continue;

    int32_t b_off, a_off, sz1;
    if (!scre_get_copy(ir, i1, &b_off, &a_off, &sz1)) /* C1: B := A */
      continue;
    if (a_off == b_off)
      continue;
    /* A and B regions must be disjoint for A:=B:=A to be an identity on A. */
    if (!(a_off + sz1 <= b_off || b_off + sz1 <= a_off))
      continue;

    /* Scan forward for C2, allowing only benign (non-writing, non-call)
     * instructions in between.  The first call encountered must be C2. */
    int i2 = -1;
    int ok = 1;
    for (int k = i1 + 1; k < n && ok; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        i2 = k;
        break;
      }
      /* Any memory write between the two copies invalidates the "A unchanged"
       * premise. */
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
          q->op == TCCIR_OP_STORE_POSTINC)
      {
        ok = 0;
        break;
      }
      /* A control-flow edge means C2 (if any) is in another block — bail. */
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
          q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
          q->is_jump_target)
      {
        ok = 0;
        break;
      }
    }
    if (!ok || i2 < 0)
      continue;

    IRQuadCompact *c2 = &ir->compact_instructions[i2];
    if (!scre_is_memcpy_like(ir, c2))
      continue;
    if (c2->is_jump_target)
      continue;

    int32_t a2_off, b2_off, sz2;
    if (!scre_get_copy(ir, i2, &a2_off, &b2_off, &sz2)) /* C2: A := B */
      continue;
    /* C2 must be the exact reverse copy of C1 with the same length. */
    if (a2_off != a_off || b2_off != b_off || sz2 != sz1)
      continue;

    /* If either call returns a value (FUNCCALLVAL), its result must be dead. */
    int dead_result = 1;
    int calls[2] = {i1, i2};
    for (int ci = 0; ci < 2 && dead_result; ci++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[calls[ci]];
      if (cq->op != TCCIR_OP_FUNCCALLVAL)
        continue;
      IROperand res = tcc_ir_op_get_dest(ir, cq);
      if (!irop_has_vreg(res))
        continue;
      int32_t rv = irop_get_vreg(res);
      for (int u = calls[ci] + 1; u < n; u++)
      {
        IRQuadCompact *uq = &ir->compact_instructions[u];
        if (uq->op == TCCIR_OP_NOP)
          continue;
        const IRRegistersConfig *cfg = &irop_config[uq->op];
        if ((cfg->has_src1 && irop_has_vreg(tcc_ir_op_get_src1(ir, uq)) &&
             irop_get_vreg(tcc_ir_op_get_src1(ir, uq)) == rv) ||
            (cfg->has_src2 && irop_has_vreg(tcc_ir_op_get_src2(ir, uq)) &&
             irop_get_vreg(tcc_ir_op_get_src2(ir, uq)) == rv))
        {
          dead_result = 0;
          break;
        }
        if (cfg->has_dest && irop_has_vreg(tcc_ir_op_get_dest(ir, uq)) &&
            irop_get_vreg(tcc_ir_op_get_dest(ir, uq)) == rv)
          break; /* redefined */
      }
    }
    if (!dead_result)
      continue;

    /* Region B must be a private round-trip buffer: referenced only as C1's
     * dst-addr and C2's src-addr (and their param/LEA setup), nowhere else.
     * Any other read/write/addr-of of B is disqualifying. */
    int b_ok = 1;
    for (int k = 0; k < n && b_ok; k++)
    {
      if (k == i1 || k == i2)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;
      /* The LEA/ASSIGN that materialize B's address and the PARAM ops that
       * pass it belong to C1/C2 — those are allowed; everything else is not.
       * Distinguish by op kind: address-materialization (LEA/ASSIGN of
       * Addr[StackLoc[b]]) and FUNCPARAM* are the only legitimate references.
       * A direct lval load/store of region B, or B's address flowing into any
       * other op, is disqualifying. */
      int is_w;
      if (!scre_touches_region(ir, q, b_off, sz1, &is_w))
        continue;
      if (q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASSIGN ||
          q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
        continue; /* address-of / param plumbing for the two copies */
      b_ok = 0;
    }
    if (!b_ok)
      continue;

    /* Apply: NOP both calls and their param marshalling.  The now-dead address
     * LEAs are cleaned by the following DCE. */
    ir_opt_nop_call_params(ir, i1);
    c1->op = TCCIR_OP_NOP;
    ir_opt_nop_call_params(ir, i2);
    c2->op = TCCIR_OP_NOP;
    changes++;
    LOG_IR_GEN("STRUCT COPY ROUNDTRIP ELIM: calls @%d,%d  A=%d B=%d size=%d", i1, i2, a_off, b_off, sz1);
  }

  return changes;
}

/* ============================================================================
 * Init-copy-from-global load forwarding (tcc_ir_opt_memmove_global_load_fwd)
 * ============================================================================
 *
 * The ubiquitous `struct S y = global; ... return y.field;` idiom (and the
 * 20040709-2 fn1* family after the identity-retme round-trip is removed)
 * lowers to a `memmove(y, &global, N)` copy into a private stack slot followed
 * by a few loads of y's fields — y never escapes and is never written.  GCC
 * skips the copy and reads `global` directly.  This pass does the same: when
 * EVERY reference to the copied slot is a load that lies inside the copied
 * region, it rewrites each load to read the global at the matching offset and
 * NOPs the copy (its now-dead dest LEAs / stack slot are dropped by DCE).
 *
 * Safety — all required, each closes a hazard:
 *   - dest resolves to a LOCAL stack slot D=[dbase,dbase+N); src resolves to a
 *     GLOBAL symref (sym G, addend gA); N constant > 0;
 *   - every address derived from D (the copy's own dest-LEA chain, plus any
 *     `LEA StackLoc[D+k]` and `+#k` interposer) is used ONLY as a load operand
 *     within [0,N) or as the copy's own call params — any store through it, any
 *     escape into another call/op, or any non-load use disqualifies (so D is a
 *     read-only private snapshot);
 *   - the straight-line region between the copy and the last forwarded load
 *     contains NO call and NO store and no control-flow edge — this guarantees
 *     the global source is provably unmodified across the window, AND is
 *     exactly what makes the pass skip the `x=s; r=fn(a); compare x,s` snapshot
 *     idiom (a call sits between the copy and the reads there), whose copy must
 *     be preserved and whose elimination would regress register pressure.
 */

/* Byte width of a load/store operand's base type (0 = not a simple scalar). */
static int mglf_btype_width(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:    return 1;
  case IROP_BTYPE_INT16:   return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32: return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64: return 8;
  default:                 return 0;
  }
}

/* Resolve a memmove src param to a global symref (address-of, not a deref).
 * Handles the direct `GlobalSym` operand and a TEMP defined by `T = LEA sym`. */
static int mglf_resolve_global_src(TCCIRState *ir, IROperand op, int before_idx, IRPoolSymref **out)
{
  if (irop_get_tag(op) == IROP_TAG_SYMREF && !op.is_lval && !op.is_local)
  {
    *out = irop_get_symref_ex(ir, op);
    return *out != NULL;
  }
  if (!irop_has_vreg(op))
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  for (int d = before_idx - 1; d >= 0; d--)
  {
    IRQuadCompact *dq = &ir->compact_instructions[d];
    if (dq->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[dq->op].has_dest)
      continue;
    IROperand dd = tcc_ir_op_get_dest(ir, dq);
    if (!irop_has_vreg(dd) || irop_get_vreg(dd) != vr)
      continue;
    if (dq->op != TCCIR_OP_LEA && dq->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
    if (dq->op == TCCIR_OP_ASSIGN && !irop_is_none(tcc_ir_op_get_src2(ir, dq)))
      return 0;
    if (irop_get_tag(ds1) != IROP_TAG_SYMREF || ds1.is_lval || ds1.is_local)
      return 0;
    *out = irop_get_symref_ex(ir, ds1);
    return *out != NULL;
  }
  return 0;
}

/* Ops whose is_lval source operands are plain value-loads of that address
 * (the deref reads `width = operand-btype` bytes and uses them).  Forwarding
 * such an operand from a non-escaping local copy to the global source is sound
 * under the same byte-equality window (memmove + no intervening write) as a
 * standalone LOAD — only WHICH address the bytes are read from changes, not
 * which/how many bytes.  Whitelist keeps out address-only (LEA), stores,
 * indexed/postinc (complex addressing), calls/params (escape) and FP ops. */
static int mglf_is_value_read_op(int op)
{
  switch (op)
  {
  case TCCIR_OP_ADD: case TCCIR_OP_SUB: case TCCIR_OP_MUL:
  case TCCIR_OP_AND: case TCCIR_OP_OR:  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL: case TCCIR_OP_SAR: case TCCIR_OP_SHR:
  case TCCIR_OP_ROR: case TCCIR_OP_CMP: case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_ASSIGN:
    return 1;
  default:
    return 0;
  }
}

#define MGLF_MAX 32

int tcc_ir_opt_memmove_global_load_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int ci = 0; ci < n; ci++)
  {
    IRQuadCompact *c = &ir->compact_instructions[ci];
    if (!scre_is_memcpy_like(ir, c))
      continue;

    IROperand p0, p1, p2;
    if (!ir_opt_get_call_param_operand(ir, ci, 0, &p0) ||
        !ir_opt_get_call_param_operand(ir, ci, 1, &p1) ||
        !ir_opt_get_call_param_operand(ir, ci, 2, &p2))
      continue;
    if (irop_get_tag(p2) != IROP_TAG_IMM32)
      continue;
    int32_t N = (int32_t)p2.u.imm32;
    if (N <= 0 || N > 256)
      continue;

    int32_t dbase;
    int dl;
    if (!scre_resolve_slot_addr(ir, p0, ci, &dbase, &dl) || !dl)
      continue; /* dest must be a local stack slot */

    IRPoolSymref *gref = NULL;
    if (!mglf_resolve_global_src(ir, p1, ci, &gref) || !gref || !gref->sym)
      continue; /* src must be a global */

    /* Worklist of address vregs derived from D: (vreg, byte-offset-into-D,
     * defining-index).  The copy's own dest address (p0 / its LEA) is allowed
     * only as this call's params, captured by seeding it here. */
    int32_t wl_vr[MGLF_MAX];
    int32_t wl_off[MGLF_MAX];
    int wl_def[MGLF_MAX];
    int wl_n = 0;

    /* Seed: every LEA/ASSIGN of Addr[StackLoc[off]] with off in [dbase,dbase+N)
     * defines an address into D. */
    for (int k = 0; k < n; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (q->op == TCCIR_OP_ASSIGN && !irop_is_none(tcc_ir_op_get_src2(ir, q)))
        continue;
      if (irop_get_tag(s1) != IROP_TAG_STACKOFF || s1.is_lval || !s1.is_local)
        continue;
      int32_t off = irop_get_stack_offset(s1);
      if (off < dbase || off >= dbase + N)
        continue;
      IROperand dd = tcc_ir_op_get_dest(ir, q);
      if (!irop_has_vreg(dd))
        continue;
      if (wl_n >= MGLF_MAX)
        goto next_call;
      wl_vr[wl_n] = irop_get_vreg(dd);
      wl_off[wl_n] = off - dbase;
      wl_def[wl_n] = k;
      wl_n++;
    }
    if (wl_n == 0)
      continue;

    /* Collected forwardable load sites.  ld_which is the operand slot to
     * rewrite (1 = src1, 2 = src2); standalone LOADs and direct-StackLoc reads
     * are always src1, fused ALU-operand derefs may be either. */
    int ld_idx[MGLF_MAX];
    int32_t ld_delta[MGLF_MAX];
    int ld_which[MGLF_MAX];
    int ld_n = 0;
    int ok = 1;
    int last_load = ci;

    for (int w = 0; w < wl_n && ok; w++)
    {
      int32_t av = wl_vr[w];
      int32_t aoff = wl_off[w];
      int adef = wl_def[w];

      for (int k = ci + 1; k < n && ok; k++)
      {
        if (k == adef)
          continue;
        IRQuadCompact *q = &ir->compact_instructions[k];
        if (q->op == TCCIR_OP_NOP)
          continue;
        const IRRegistersConfig *cfg = &irop_config[q->op];
        /* NB: explicit if/else, not `cond ? get_srcN() : IROP_NONE`.  The
         * armv8m self-host cross miscompiles a ternary whose arms are a
         * struct-returning call and a struct constant (it materializes the
         * call result and the constant in DIFFERENT sret buffers, then reads
         * the merged value from the constant's buffer), so the call branch
         * silently yields a stale operand.  Same class as the sl_forward
         * ternary fixes (tinycc f0a85c86 / 21305c35). */
        IROperand s1 = IROP_NONE, s2 = IROP_NONE, d = IROP_NONE;
        if (cfg->has_src1)
          s1 = tcc_ir_op_get_src1(ir, q);
        if (cfg->has_src2)
          s2 = tcc_ir_op_get_src2(ir, q);
        if (cfg->has_dest)
          d = tcc_ir_op_get_dest(ir, q);

        int in_s1 = cfg->has_src1 && irop_has_vreg(s1) && irop_get_vreg(s1) == av;
        int in_s2 = cfg->has_src2 && irop_has_vreg(s2) && irop_get_vreg(s2) == av;
        int in_d = cfg->has_dest && irop_has_vreg(d) && irop_get_vreg(d) == av;
        if (!in_s1 && !in_s2 && !in_d)
          continue;
        /* (The copy's own params reference D's dest address but always precede
         * the call, so they are never seen by this post-call use-scan; any
         * FUNCPARAM use found here is an escape into another call and falls
         * through to the disqualifying default below.) */

        /* Interposer `new = av + #k` (av as a plain pointer value). */
        if (q->op == TCCIR_OP_ADD && in_s1 && !s1.is_lval && !in_s2 && !in_d &&
            irop_get_tag(s2) == IROP_TAG_IMM32)
        {
          int32_t nv = irop_get_vreg(d);
          if (nv < 0 || d.is_lval || wl_n >= MGLF_MAX)
          {
            ok = 0;
            break;
          }
          wl_vr[wl_n] = nv;
          wl_off[wl_n] = aoff + (int32_t)s2.u.imm32;
          wl_def[wl_n] = k;
          wl_n++;
          continue;
        }

        /* A single clean LOAD whose address operand is av (load reads D). */
        int which = in_s1 ? 1 : in_s2 ? 2 : 0;
        IROperand dref;
        if (which == 1)
          dref = s1;
        else if (which == 2)
          dref = s2;
        else
          dref = d;
        if (q->op == TCCIR_OP_LOAD && which == 1 && s1.is_lval && !in_s2 && !in_d)
        {
          int wbytes = mglf_btype_width(irop_get_btype(dref));
          if (wbytes == 0 || aoff < 0 || aoff + wbytes > N || ld_n >= MGLF_MAX)
          {
            ok = 0;
            break;
          }
          ld_idx[ld_n] = k;
          ld_delta[ld_n] = aoff;
          ld_which[ld_n] = 1;
          ld_n++;
          if (k > last_load)
            last_load = k;
          continue;
        }

        /* A value-read op (ADD/SHR/AND/CMP/...) whose is_lval operand is av
         * reads D's bytes exactly like a standalone LOAD — the field access of
         * a 64-bit bitfield (or any wider field) lowers to a deref fused into
         * the shift/mask/add rather than a bare LOAD.  Forward that single
         * operand to the global.  Require av to appear in exactly one src slot
         * (unambiguous offset), as a deref, never as the dest. */
        if (mglf_is_value_read_op(q->op) && which != 0 && dref.is_lval &&
            !in_d && (in_s1 ^ in_s2))
        {
          int wbytes = mglf_btype_width(irop_get_btype(dref));
          if (wbytes == 0 || aoff < 0 || aoff + wbytes > N || ld_n >= MGLF_MAX)
          {
            ok = 0;
            break;
          }
          ld_idx[ld_n] = k;
          ld_delta[ld_n] = aoff;
          ld_which[ld_n] = which;
          ld_n++;
          if (k > last_load)
            last_load = k;
          continue;
        }

        /* Anything else touching D's address (store, escape, indexed, struct
         * read, non-lval use) disqualifies. */
        ok = 0;
      }
    }

    /* Second scan: DIRECT StackLoc references to the slot (the plain-struct
     * field read `T = StackLoc[off] [LOAD]`, which has no LEA-temp).  A direct
     * lval LOAD src1 in the slot is forwardable; the address-of operand of the
     * seeding LEA/ASSIGN (is_lval==0) is skipped (handled by the worklist
     * above); anything else (a store, or any other direct use) disqualifies. */
    for (int k = ci + 1; k < n && ok; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;
      const IRRegistersConfig *cfg = &irop_config[q->op];
      IROperand ops3[3];
      int np = 0, posn[3];
      if (cfg->has_dest) { ops3[np] = tcc_ir_op_get_dest(ir, q); posn[np] = 0; np++; }
      if (cfg->has_src1) { ops3[np] = tcc_ir_op_get_src1(ir, q); posn[np] = 1; np++; }
      if (cfg->has_src2) { ops3[np] = tcc_ir_op_get_src2(ir, q); posn[np] = 2; np++; }
      for (int p = 0; p < np && ok; p++)
      {
        IROperand o = ops3[p];
        if (irop_get_tag(o) != IROP_TAG_STACKOFF || !o.is_local)
          continue;
        int32_t off = irop_get_stack_offset(o);
        if (off < dbase || off >= dbase + N)
          continue;
        /* Address-of in the seeding LEA/ASSIGN (already tracked by Pass 1). */
        if (!o.is_lval && posn[p] == 1 &&
            (q->op == TCCIR_OP_LEA ||
             (q->op == TCCIR_OP_ASSIGN && irop_is_none(tcc_ir_op_get_src2(ir, q)))))
          continue;
        /* Direct lval LOAD of the slot, or a value-read op (ADD/SHR/...) whose
         * is_lval StackLoc operand reads the slot — both forwardable.  A store
         * (dest, posn 0) or any other reference disqualifies. */
        if (((q->op == TCCIR_OP_LOAD && posn[p] == 1) ||
             (mglf_is_value_read_op(q->op) && posn[p] != 0)) && o.is_lval)
        {
          int wbytes = mglf_btype_width(irop_get_btype(o));
          int32_t delta = off - dbase;
          if (wbytes == 0 || delta < 0 || delta + wbytes > N || ld_n >= MGLF_MAX)
          {
            ok = 0;
            break;
          }
          ld_idx[ld_n] = k;
          ld_delta[ld_n] = delta;
          ld_which[ld_n] = posn[p];
          ld_n++;
          if (k > last_load)
            last_load = k;
          continue;
        }
        /* Any other direct reference (store, etc.) disqualifies. */
        ok = 0;
      }
    }

    if (!ok || ld_n == 0)
      continue;

    /* The window [ci+1, last_load] must be straight-line with no call, no
     * store and no control-flow edge, so the global is provably unmodified. */
    for (int k = ci + 1; k <= last_load && ok; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;
      switch (q->op)
      {
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_STORE:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_JUMP:
      case TCCIR_OP_JUMPIF:
      case TCCIR_OP_IJUMP:
      case TCCIR_OP_SWITCH_TABLE:
        ok = 0;
        break;
      default:
        if (q->is_jump_target)
          ok = 0;
        break;
      }
    }
    if (!ok)
      continue;

    /* Apply: rewrite every load operand to a global deref at the matching
     * offset, then NOP the copy call + its params.  Capture the symref fields
     * first — tcc_ir_pool_add_symref may reallocate the symref pool and
     * invalidate `gref`. */
    Sym *gsym = gref->sym;
    int32_t gaddend = gref->addend;
    uint32_t gflags = gref->flags;
    for (int r = 0; r < ld_n; r++)
    {
      IRQuadCompact *lq = &ir->compact_instructions[ld_idx[r]];
      /* Explicit if/else, not a ternary — see the s1/s2/d note above: the
       * self-host cross miscompiles `cond ? get_src2() : get_src1()` (two
       * struct-returning calls) by giving each arm its own sret buffer and
       * reading the merge from the wrong one, so `old` would carry a stale
       * btype/is_unsigned (here that mis-forwarded gB.k as a word read of
       * gB.l — see tests/ir_tests/178_dead_store_sroa.c). */
      IROperand old;
      if (ld_which[r] == 2)
        old = tcc_ir_op_get_src2(ir, lq);
      else
        old = tcc_ir_op_get_src1(ir, lq);
      uint32_t pool = tcc_ir_pool_add_symref(ir, gsym, gaddend + ld_delta[r], gflags);
      IROperand g = irop_make_symref(-1, pool, /*is_lval*/ 1, /*is_local*/ 0, /*is_const*/ 0,
                                     irop_get_btype(old));
      g.is_unsigned = old.is_unsigned;
      if (ld_which[r] == 2)
        tcc_ir_op_set_src2(ir, lq, g);
      else
        tcc_ir_op_set_src1(ir, lq, g);
    }
    ir_opt_nop_call_params(ir, ci);
    c->op = TCCIR_OP_NOP;
    changes++;
    LOG_IR_GEN("MEMMOVE GLOBAL LOAD FWD: copy@%d D=%d N=%d -> %d loads forwarded to global", ci, dbase, N, ld_n);
  next_call:;
  }

  return changes;
}
