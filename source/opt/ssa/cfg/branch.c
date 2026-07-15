/*
 *  TCC IR - SSA Branch Folding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt/ssa/branch.h"
#include "opt/ssa/ssa_opt_helpers.h"
#include "memory/small_sequence.h"

/* Inline-first per-block scratch (heap only past the inline cap). */
TCC_SMALL_SEQUENCE_DEFINE(BranchIntSeq, int, 128)
TCC_SMALL_SEQUENCE_DEFINE(BranchU8Seq, uint8_t, 128)

/* ============================================================================
 * Branch Folding: when CMP or TEST_ZERO has constant operands (after cprop
 * propagated immediates), evaluate the comparison at compile time and convert
 * the JUMPIF to unconditional JUMP or NOP.
 *
 * Patterns:
 *   CMP #a, #b; JUMPIF cond → JUMP (if cond(a,b) is true)
 *   CMP #a, #b; JUMPIF cond → NOP  (if cond(a,b) is false)
 *   TEST_ZERO #a; JUMPIF EQ  → JUMP/NOP based on a==0
 *   CMP #a, #b; SETIF cond  → ASSIGN #0 or #1
 * ============================================================================ */

static int eval_cond(int64_t v1, int64_t v2, int tok)
{
  switch (tok) {
  case 0x94: return v1 == v2;
  case 0x95: return v1 != v2;
  case 0x9c: return v1 < v2;
  case 0x9d: return v1 >= v2;
  case 0x9e: return v1 <= v2;
  case 0x9f: return v1 > v2;
  case 0x92: return (uint64_t)v1 < (uint64_t)v2;
  case 0x93: return (uint64_t)v1 >= (uint64_t)v2;
  case 0x96: return (uint64_t)v1 <= (uint64_t)v2;
  case 0x97: return (uint64_t)v1 > (uint64_t)v2;
  default: return -1;
  }
}

/* Drop phi operands that flow from `dead_pred_block` to phis at
 * `target_block_idx`. Used after folding a JUMPIF: the dead edge no longer
 * exists, so phi resolution should not emit copies for it. */
void ssa_drop_phi_edge(IRSSAOptCtx *ctx, int dead_pred_block,
                       int target_block_idx)
{
  if (!ctx->ssa || !ctx->ssa->block_phis || !ctx->cfg) return;
  if (target_block_idx < 0 || target_block_idx >= ctx->cfg->num_blocks) return;

  for (IRPhiNode *phi = ctx->ssa->block_phis[target_block_idx]; phi; phi = phi->next) {
    /* Find every operand from dead_pred_block. Each removal shifts
     * remaining operands down, which means the vinfo `slot` field for those
     * operands must also be decremented to match. Process from low index
     * upward and recompute the loop bound after each removal. */
    int r = 0;
    while (r < phi->num_operands) {
      if (phi->operands[r].pred_block != dead_pred_block) {
        r++;
        continue;
      }

      /* Remove this operand's SSA_USE_PHI entry from its vreg's vinfo. */
      int32_t dropped_vr = phi->operands[r].vreg;
      if (dropped_vr >= 0) {
        IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, dropped_vr);
        if (dvi) {
          for (int u = 0; u < dvi->use_count; u++) {
            if (dvi->uses[u].kind == SSA_USE_PHI &&
                dvi->uses[u].idx == target_block_idx &&
                dvi->uses[u].slot == r) {
              dvi->uses[u] = dvi->uses[--dvi->use_count];
              break;
            }
          }
        }
      }

      /* Shift remaining operands down by one and decrement their vinfo
       * slots so SSA_USE_PHI entries keep pointing to the right operand. */
      for (int s = r + 1; s < phi->num_operands; s++) {
        phi->operands[s - 1] = phi->operands[s];
        int32_t v = phi->operands[s - 1].vreg;
        if (v < 0) continue;
        IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, v);
        if (!vi) continue;
        for (int u = 0; u < vi->use_count; u++) {
          if (vi->uses[u].kind == SSA_USE_PHI &&
              vi->uses[u].idx == target_block_idx &&
              vi->uses[u].slot == s) {
            vi->uses[u].slot = s - 1;
            break;
          }
        }
      }
      phi->num_operands--;
      /* Don't advance r: the operand we just removed has been replaced by
       * what was at r+1, which we still need to inspect. */
    }
  }
}

static int ssa_block_for_instr(IRCFG *cfg, int instr_idx)
{
  if (!cfg || !cfg->instr_to_block) return -1;
  /* instr_to_block is sized to num_instrs at CFG-build time; instructions
   * appended by later passes index past it, so bound-check both ends. */
  if (instr_idx < 0 || instr_idx >= cfg->num_instrs) return -1;
  return cfg->instr_to_block[instr_idx];
}

/* A vreg is provably in {0,1} when its single SSA definition is a SETIF (always
 * materialises 0 or 1) or a boolean AND/OR (idempotent boolean ops). */
static int ssa_vreg_is_bool01(IRSSAOptCtx *ctx, int32_t vr)
{
  if (vr < 0)
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0)
    return 0;
  int op = ctx->ir->compact_instructions[vi->def_instr].op;
  return op == TCCIR_OP_SETIF || op == TCCIR_OP_BOOL_AND ||
         op == TCCIR_OP_BOOL_OR;
}

/* Redundant boolean-normalisation: `CMP X,#0 ; SETIF NE` where X is already a
 * {0,1} boolean collapses to `V <- X` (the `!!bool` idiom the frontend emits
 * when a comparison is stored into a `_Bool` and read back). */
static int ssa_bool_norm(IRSSAOptCtx *ctx, int cmp_idx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];

  IROperand cmp_s2 = tcc_ir_op_get_src2(ir, cmp_q);
  if (!irop_is_immediate(cmp_s2) || cmp_s2.is_sym || cmp_s2.is_lval)
    return 0;
  if (irop_get_imm64_ex(ir, cmp_s2) != 0)
    return 0;

  IROperand cmp_s1 = tcc_ir_op_get_src1(ir, cmp_q);
  int32_t vr = irop_get_vreg(cmp_s1);
  if (vr < 0 || cmp_s1.is_lval || cmp_s1.is_sym)
    return 0;
  if (!ssa_vreg_is_bool01(ctx, vr))
    return 0;

  int j = cmp_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n)
    return 0;
  IRQuadCompact *setif_q = &ir->compact_instructions[j];
  if (setif_q->op != TCCIR_OP_SETIF)
    return 0;

  IROperand cond_op = tcc_ir_op_get_src1(ir, setif_q);
  if ((int)irop_get_imm64_ex(ir, cond_op) != 0x95 /* TOK_NE */)
    return 0;

  ssa_opt_nop_instr(ctx, cmp_idx);
  setif_q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(ir, j, irop_make_vreg(vr, irop_get_btype(cmp_s1)));
  tcc_ir_set_src2(ir, j, IROP_NONE);
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (vi)
    ssa_opt_add_use_instr(vi, j);
  return 1;
}

/* Base value a CMP operand extracts its field from: either an SSA vreg (same
 * value ⇔ same vreg) or a symref load (same value ⇔ same sym/addend/flags/width
 * with no write between the two loads). */
typedef struct {
  int32_t vreg;      /* base value's vreg, or -1 when symref-based */
  struct Sym *sym;   /* non-NULL: base is a load of this symref */
  int32_t addend;
  uint32_t flags;
  int btype;         /* load access width/type — must match to fold */
  int load_pos;      /* instruction holding the (folded) load, for the write scan */
} SsaCmpBase;

/* No STORE/STORE_INDEXED/STORE_POSTINC/call lies strictly between p1 and p2,
 * and both are in the same basic block — so a global reloaded at both points
 * yields the identical value.  Conservative: bails on any memory write. */
static int ssa_no_write_between(TCCIRState *ir, IRCFG *cfg, int p1, int p2)
{
  if (!cfg)
    return 0;
  int lo = p1 < p2 ? p1 : p2, hi = p1 < p2 ? p2 : p1;
  if (lo == hi)
    return 1;
  int b = ssa_block_for_instr(cfg, lo);
  if (b < 0 || b != ssa_block_for_instr(cfg, hi))
    return 0;
  for (int k = lo + 1; k < hi; k++) {
    switch (ir->compact_instructions[k].op) {
    case TCCIR_OP_NOP:
      continue;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_FUNCCALLVAL:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

/* Decode CMP operand `slot` (0=src1, 1=src2) into the bitfield of a base value
 * it compares: base V, offset `lsb`, `width` bits, sign/zero extension.
 * Handles the two forms the same field lowers to — `UBFX(V,lsb,w)` and a
 * `SHL #a` def completed by the CMP's own `LSR/ASR #b` barrel shift (src2
 * only), which yields `(V<<a)>>b = UBFX(V, b-a, 32-b)` — plus bare `SHR`/`SAR`.
 * The base V is either a vreg or a symref load (Case 3 cross-reload).  Reads the
 * actual barrel annotation, so a missing/other barrel simply fails to match
 * rather than folding a non-equal pair.  Returns 1 on success. */
static int ssa_cmp_extract_desc(IRSSAOptCtx *ctx, int cmp_idx, int slot,
                                SsaCmpBase *base_out, int *lsb_out, int *width_out,
                                int *sext_out)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];
  IROperand s = (slot == 0) ? tcc_ir_op_get_src1(ir, cmp_q)
                            : tcc_ir_op_get_src2(ir, cmp_q);
  if (s.is_lval || s.tag != IROP_TAG_VREG)
    return 0;
  int32_t vr = irop_get_vreg(s);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  /* The CMP's barrel shift applies to src2 only. */
  int btype = 0, bamt = 0;
  if (slot == 1) {
    uint8_t bs = tcc_ir_barrel_shift_at(ir, cmp_q);
    if (bs) { btype = (bs >> 5) & 7; bamt = bs & 31; }
  }

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_instr < 0 || vi->def_count > 1)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  if (tcc_ir_barrel_shift_at(ir, dq) != 0)
    return 0;
  if (irop_is_64bit(tcc_ir_op_get_dest(ir, dq)))
    return 0;

  IROperand d1 = tcc_ir_op_get_src1(ir, dq);
  IROperand d2 = tcc_ir_op_get_src2(ir, dq);

  SsaCmpBase base = { -1, NULL, 0, 0, 0, -1 };
  if (d1.is_lval && d1.tag == IROP_TAG_SYMREF) {
    /* Base is a directly-folded symref load: `T <-- sym***DEREF*** <op>`. */
    IRPoolSymref *sr = irop_get_symref_ex(ir, d1);
    if (!sr || !sr->sym || (sr->sym->type.t & VT_VOLATILE))
      return 0;
    base.sym = sr->sym; base.addend = sr->addend; base.flags = sr->flags;
    base.btype = irop_get_btype(d1); base.load_pos = vi->def_instr;
  } else if (!d1.is_lval && d1.tag == IROP_TAG_VREG) {
    base.vreg = irop_get_vreg(d1);
    if (base.vreg < 0)
      return 0;
    /* If the base vreg is itself a single reload of a symref, also record the
     * symref identity so two distinct reloads of the same global can fold. */
    IRSSAVregInfo *bvi = ssa_opt_vinfo(ctx, base.vreg);
    if (bvi && bvi->def_instr >= 0 && bvi->def_count == 1) {
      IRQuadCompact *bd = &ir->compact_instructions[bvi->def_instr];
      if (bd->op == TCCIR_OP_LOAD && tcc_ir_barrel_shift_at(ir, bd) == 0) {
        IROperand ls = tcc_ir_op_get_src1(ir, bd);
        if (ls.is_lval && ls.tag == IROP_TAG_SYMREF) {
          IRPoolSymref *sr = irop_get_symref_ex(ir, ls);
          if (sr && sr->sym && !(sr->sym->type.t & VT_VOLATILE)) {
            base.sym = sr->sym; base.addend = sr->addend; base.flags = sr->flags;
            base.btype = irop_get_btype(ls); base.load_pos = bvi->def_instr;
          }
        }
      }
    }
  } else {
    return 0;
  }

  int lsb, width, sext, def_shl = -1;
  switch (dq->op) {
  case TCCIR_OP_UBFX:
    if (!irop_is_immediate(d2)) return 0;
    { int p = (int)irop_get_imm64_ex(ir, d2); lsb = p & 31; width = (p >> 5) & 63; sext = 0; }
    break;
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
    if (!irop_is_immediate(d2)) return 0;
    { int sft = (int)irop_get_imm64_ex(ir, d2);
      if (sft < 0 || sft > 31) return 0;
      lsb = sft; width = 32 - sft; sext = (dq->op == TCCIR_OP_SAR); }
    break;
  case TCCIR_OP_SHL:
    if (!irop_is_immediate(d2)) return 0;
    def_shl = (int)irop_get_imm64_ex(ir, d2);
    if (def_shl < 1 || def_shl > 31) return 0;
    lsb = 0; width = 0; sext = 0;
    break;
  default:
    return 0;
  }

  if (def_shl >= 0) {
    /* `V << a` only forms a low-aligned field when the CMP completes it with a
     * right shift `>> b` (b >= a) on src2: (V<<a)>>b = UBFX(V, b-a, 32-b). */
    if (slot != 1 || (btype != 2 && btype != 3) || bamt < def_shl || bamt > 31)
      return 0;
    lsb = bamt - def_shl; width = 32 - bamt; sext = (btype == 3);
  } else if (slot == 1 && btype) {
    /* An extra barrel on an already-extracted operand composes another shift
     * we don't model — stay conservative. */
    return 0;
  }

  if (width <= 0 || width > 32 || lsb < 0 || lsb > 31)
    return 0;

  *base_out = base; *lsb_out = lsb; *width_out = width; *sext_out = sext;
  return 1;
}

static int ssa_fold_cmp_jumpif(IRSSAOptCtx *ctx, int cmp_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];
  int n = ir->next_instruction_index;

  if (ssa_bool_norm(ctx, cmp_idx))
    return 1;

  IROperand src1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand src2 = tcc_ir_op_get_src2(ir, cmp_q);

  int64_t v1, v2;
  int have_values = 0;

  /* Case 1: both operands are immediates (chase ASSIGN #const) */
  {
    IROperand ops[2] = { src1, src2 };
    int64_t vals[2];
    int got[2] = { 0, 0 };
    int cmp_block = ssa_block_for_instr(ctx->cfg, cmp_idx);
    IRBasicBlock *cmp_bb = (cmp_block >= 0) ? &ctx->cfg->blocks[cmp_block] : NULL;
    for (int oi = 0; oi < 2; oi++) {
      if (irop_is_immediate(ops[oi])) {
        vals[oi] = irop_get_imm64_ex(ir, ops[oi]);
        got[oi] = 1;
      } else {
        int32_t vr = irop_get_vreg(ops[oi]);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
            ops[oi].tag == IROP_TAG_VREG && !ops[oi].is_lval) {
          IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
          if (vi && vi->def_instr >= 0 && vi->def_count <= 1) {
            IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
            if (dq->op == TCCIR_OP_ASSIGN) {
              IROperand ds = tcc_ir_op_get_src1(ir, dq);
              if (irop_is_immediate(ds) && !ds.is_lval) {
                vals[oi] = irop_get_imm64_ex(ir, ds);
                got[oi] = 1;
              }
            }
          }
        }
        /* VAR operand: scan same block backward for the most recent def
         * of this VAR.  Bail on any potentially-aliasing intervening write
         * (call, indirect store, store through escaped pointer) or any
         * non-immediate definition. */
        if (!got[oi] && cmp_bb && vr >= 0 &&
            TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
          int var_pos = TCCIR_DECODE_VREG_POSITION(vr);
          for (int k = cmp_idx - 1; k >= cmp_bb->start_idx; k--) {
            IRQuadCompact *kq = &ir->compact_instructions[k];
            if (kq->op == TCCIR_OP_NOP)
              continue;
            if (kq->op == TCCIR_OP_FUNCCALLVOID || kq->op == TCCIR_OP_FUNCCALLVAL)
              break;
            if (kq->op == TCCIR_OP_STORE_INDEXED || kq->op == TCCIR_OP_STORE_POSTINC)
              break;  /* may alias VAR through pointer arithmetic */
            /* Any op that defines this VAR.  Distinguish *direct slot
             * writes* (which we can mine for an immediate value) from
             * *pointer-deref through V* (`STORE V_DEREF <-- val`) — the
             * latter writes to V's pointee, not V's slot, and must not
             * be treated as a def of V's value.
             *
             * Discriminator: a slot write has kd.is_local=1 (the dest
             * carries the VT_LOCAL svalue encoding); a pointer-deref
             * inherited its flags from a TEMP that had is_local=0, so
             * kd.is_local=0 for the deref case (introduced by
             * cprop_copy_var_stackoff). */
            if (irop_config[kq->op].has_dest) {
              IROperand kd = tcc_ir_op_get_dest(ir, kq);
              int32_t kdv = irop_get_vreg(kd);
              if (kdv >= 0 &&
                  TCCIR_DECODE_VREG_TYPE(kdv) == TCCIR_VREG_TYPE_VAR &&
                  TCCIR_DECODE_VREG_POSITION(kdv) == var_pos) {
                /* For STORE with a pointer-deref dest (is_local=0,
                 * is_lval=1), do not stop — this writes through V's
                 * value, not V's slot, so V's content is unchanged.
                 * The scan must continue past it to find an actual slot
                 * def (or hit a barrier). */
                if (kq->op == TCCIR_OP_STORE && kd.is_lval && !kd.is_local)
                  continue;
                /* Found the most recent slot def of this VAR.  Try to
                 * extract an immediate value. */
                if (kq->op == TCCIR_OP_ASSIGN || kq->op == TCCIR_OP_STORE) {
                  IROperand ks = tcc_ir_op_get_src1(ir, kq);
                  if (irop_is_immediate(ks) && !ks.is_lval) {
                    vals[oi] = irop_get_imm64_ex(ir, ks);
                    got[oi] = 1;
                  }
                }
                break;
              }
            }
            if (kq->op == TCCIR_OP_STORE) {
              IROperand kd = tcc_ir_op_get_dest(ir, kq);
              /* STORE to a different VAR slot or to a known stack slot
               * cannot alias this VAR. */
              if (kd.tag == IROP_TAG_STACKOFF && kd.is_local && kd.is_lval)
                continue;
              /* TEMP-DEREF or global STORE: could alias through escaped
               * pointers.  Bail. */
              break;
            }
          }
        }
      }
    }
    if (got[0] && got[1]) {
      v1 = vals[0];
      v2 = vals[1];
      /* Truncate to operand width to avoid sign-extension mismatches */
      int cmp_btype = irop_get_btype(src1);
      if (cmp_btype != IROP_BTYPE_INT64) {
        v1 = (int64_t)(int32_t)(uint32_t)v1;
        v2 = (int64_t)(int32_t)(uint32_t)v2;
      }
      have_values = 1;
    }
  }

  /* Case 2: both operands resolve to the same SSA TEMP vreg (CMP x, x).
   * Chase single-def ASSIGN copies to find the root vreg.
   * Use 0,0 as representative — all reflexive comparisons give the
   * same boolean result regardless of the actual value. */
  if (!have_values) {
    int32_t vr1 = irop_get_vreg(src1);
    int32_t vr2 = irop_get_vreg(src2);

    /* Chase ASSIGN copies: T6 = T0 → root is T0 */
    for (int hop = 0; hop < 4 && vr1 >= 0; hop++) {
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr1);
      if (!vi || vi->def_instr < 0 || vi->def_count > 1) break;
      IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
      if (dq->op != TCCIR_OP_ASSIGN) break;
      IROperand ds = tcc_ir_op_get_src1(ir, dq);
      if (ds.is_lval || ds.tag != IROP_TAG_VREG) break;
      int32_t nv = irop_get_vreg(ds);
      if (nv < 0 || TCCIR_DECODE_VREG_TYPE(nv) != TCCIR_VREG_TYPE_TEMP) break;
      vr1 = nv;
    }
    for (int hop = 0; hop < 4 && vr2 >= 0; hop++) {
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr2);
      if (!vi || vi->def_instr < 0 || vi->def_count > 1) break;
      IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
      if (dq->op != TCCIR_OP_ASSIGN) break;
      IROperand ds = tcc_ir_op_get_src1(ir, dq);
      if (ds.is_lval || ds.tag != IROP_TAG_VREG) break;
      int32_t nv = irop_get_vreg(ds);
      if (nv < 0 || TCCIR_DECODE_VREG_TYPE(nv) != TCCIR_VREG_TYPE_TEMP) break;
      vr2 = nv;
    }

    if (vr1 >= 0 && vr1 == vr2 &&
        TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_TEMP &&
        src1.is_lval == src2.is_lval &&
        src1.tag == IROP_TAG_VREG && src2.tag == IROP_TAG_VREG) {
      v1 = 0;
      v2 = 0;
      have_values = 1;
    }
  }

  /* Case 3: both operands extract the identical bitfield (same base vreg,
   * offset, width, extension) via possibly-different forms (UBFX vs
   * SHL+CMP-barrel), which GVN cannot syntactically coalesce.  The compared
   * values are then bit-identical, so the comparison is reflexive.  This is the
   * bitfield self-compare `x.f != s.f` (x a copy of s) that legacy const_prop
   * folded before the asymmetric UBFX/barrel lowering. */
  if (!have_values) {
    SsaCmpBase b1, b2;
    int l1, l2, w1, w2, x1, x2;
    if (ssa_cmp_extract_desc(ctx, cmp_idx, 0, &b1, &l1, &w1, &x1) &&
        ssa_cmp_extract_desc(ctx, cmp_idx, 1, &b2, &l2, &w2, &x2) &&
        l1 == l2 && w1 == w2 && x1 == x2) {
      int same_base = 0;
      if (b1.vreg >= 0 && b1.vreg == b2.vreg)
        same_base = 1;                         /* identical SSA base value */
      else if (b1.sym && b1.sym == b2.sym && b1.addend == b2.addend &&
               b1.flags == b2.flags && b1.btype == b2.btype &&
               ssa_no_write_between(ir, ctx->cfg, b1.load_pos, b2.load_pos))
        same_base = 1;                         /* two clean reloads of one global */
      if (same_base) {
        v1 = 0;
        v2 = 0;
        have_values = 1;
      }
    }
  }

  if (!have_values)
    return 0;

  /* Folding consumes the CMP; drop any barrel annotation so a later reuse of
   * this slot as a NOP/JUMP can't inherit a stale shift. */
  if (ir->barrel_shifts && cmp_q->orig_index >= 0 &&
      cmp_q->orig_index < ir->barrel_shifts_len)
    ir->barrel_shifts[cmp_q->orig_index] = 0;

  int j = cmp_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n)
    return 0;

  IRQuadCompact *next_q = &ir->compact_instructions[j];

  if (next_q->op == TCCIR_OP_JUMPIF) {
    IROperand cond = tcc_ir_op_get_src1(ir, next_q);
    int tok = (int)irop_get_imm64_ex(ir, cond);
    int result = eval_cond(v1, v2, tok);
    if (result < 0)
      return 0;

    /* Remove uses of CMP operands */
    IRSSAVregInfo *vi;
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src1));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src2));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);

    /* Identify the dead edge so we can prune corresponding phi operands.
     * Without this, phi resolution still emits copies for that edge, which
     * surface as dead writes to spilled carrier vregs. */
    int jumpif_block = ssa_block_for_instr(ctx->cfg, j);
    int target_idx = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, next_q));
    int target_block = ssa_block_for_instr(ctx->cfg, target_idx);
    int fallthru_block = ssa_block_for_instr(ctx->cfg, j + 1);

    if (result) {
      IROperand dest = tcc_ir_op_get_dest(ir, next_q);
      cmp_q->op = TCCIR_OP_NOP;
      next_q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, j, dest);
      /* Fall-through edge dies. */
      if (jumpif_block >= 0 && fallthru_block >= 0 && fallthru_block != target_block)
        ssa_drop_phi_edge(ctx, jumpif_block, fallthru_block);
    } else {
      cmp_q->op = TCCIR_OP_NOP;
      next_q->op = TCCIR_OP_NOP;
      /* Target edge dies; fall-through is the surviving path. */
      if (jumpif_block >= 0 && target_block >= 0 && target_block != fallthru_block)
        ssa_drop_phi_edge(ctx, jumpif_block, target_block);
    }
    return 1;
  }

  if (next_q->op == TCCIR_OP_SETIF) {
    IROperand cond = tcc_ir_op_get_src1(ir, next_q);
    int tok = (int)irop_get_imm64_ex(ir, cond);
    int result = eval_cond(v1, v2, tok);
    if (result < 0)
      return 0;

    IRSSAVregInfo *vi;
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src1));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src2));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);

    IROperand dest = tcc_ir_op_get_dest(ir, next_q);
    IROperand imm = irop_make_imm32(0, result ? 1 : 0, dest.btype);
    cmp_q->op = TCCIR_OP_NOP;
    next_q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, j, imm);
    tcc_ir_set_src2(ir, j, IROP_NONE);
    return 1;
  }

  return 0;
}

/* Resolve a TEST_ZERO source to a compile-time constant: either an immediate
 * operand, or a single-def TEMP whose def is `ASSIGN #imm` (ssa:cprop keeps
 * immediate-through-ASSIGN forwarding disabled, so this shape survives). */
static int ssa_tz_const_src(IRSSAOptCtx *ctx, IROperand src1, int64_t *out)
{
  if (irop_is_immediate(src1)) {
    *out = irop_get_imm64_ex(ctx->ir, src1);
    return 1;
  }
  int32_t vr = irop_get_vreg(src1);
  if (vr < 0 || src1.is_lval || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0)
    return 0;
  IRQuadCompact *dq = &ctx->ir->compact_instructions[vi->def_instr];
  if (dq->op != TCCIR_OP_ASSIGN)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ctx->ir, dq);
  if (d.is_lval || irop_needs_pair(d))
    return 0;
  IROperand s = tcc_ir_op_get_src1(ctx->ir, dq);
  if (!irop_is_immediate(s) || s.is_lval)
    return 0;
  *out = irop_get_imm64_ex(ctx->ir, s);
  return 1;
}

static int ssa_fold_test_zero(IRSSAOptCtx *ctx, int tz_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *tz_q = &ir->compact_instructions[tz_idx];
  int n = ir->next_instruction_index;

  IROperand src1 = tcc_ir_op_get_src1(ir, tz_q);
  int64_t val;
  if (!ssa_tz_const_src(ctx, src1, &val))
    return 0;

  int j = tz_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n)
    return 0;

  IRQuadCompact *next_q = &ir->compact_instructions[j];
  if (next_q->op != TCCIR_OP_JUMPIF)
    return 0;

  IROperand cond = tcc_ir_op_get_src1(ir, next_q);
  int tok = (int)irop_get_imm64_ex(ir, cond);

  int branch_taken;
  if (tok == 0x94)
    branch_taken = (val == 0);
  else if (tok == 0x95)
    branch_taken = (val != 0);
  else
    return 0;

  if (branch_taken) {
    IROperand dest = tcc_ir_op_get_dest(ir, next_q);
    ssa_opt_nop_instr(ctx, tz_idx);
    next_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, dest);
  } else {
    ssa_opt_nop_instr(ctx, tz_idx);
    next_q->op = TCCIR_OP_NOP;
    /* Fold a fall-through SETIF that reads the same (now-NOPed) flag
     * state.  Without this, codegen would lower the SETIF consuming
     * garbage flags.  See the matching fold in ir_gen_branch_fold_test_zero. */
    int k = j + 1;
    while (k < n && ir->compact_instructions[k].op == TCCIR_OP_NOP)
      k++;
    if (k < n)
    {
      IRQuadCompact *setif_q = &ir->compact_instructions[k];
      if (setif_q->op == TCCIR_OP_SETIF && !setif_q->is_jump_target)
      {
        IROperand setif_cond = tcc_ir_op_get_src1(ir, setif_q);
        int setif_tok = (int)irop_get_imm64_ex(ir, setif_cond);
        int setif_result = -1;
        if (setif_tok == 0x95)
          setif_result = (val != 0) ? 1 : 0;
        else if (setif_tok == 0x94)
          setif_result = (val == 0) ? 1 : 0;
        if (setif_result >= 0)
        {
          IROperand dest = tcc_ir_op_get_dest(ir, setif_q);
          IROperand imm = irop_make_imm32(-1, setif_result, irop_get_btype(dest));
          setif_q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, k, imm);
          tcc_ir_set_src2(ir, k, IROP_NONE);
        }
      }
    }
  }
  return 1;
}

/* Compute block reachability based on the IR's current terminators (not the
 * statically-built CFG succs/preds, which aren't updated when JUMPIFs are
 * folded to JUMPs).  Returns a malloc'd uint8_t array of size cfg->num_blocks
 * with 1 = reachable from entry, 0 = unreachable.  Caller frees. */
uint8_t *ssa_opt_compute_reachable_blocks(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  TCCIRState *ir = ctx->ir;
  if (!cfg || cfg->num_blocks <= 0) return NULL;

  int nb = cfg->num_blocks;
  int n_instrs = ir->next_instruction_index;
  uint8_t *reachable = tcc_mallocz(nb);
  small_sequence(BranchIntSeq) worklist_owner = {0};
  BranchIntSeq_init(&worklist_owner, (size_t)nb);
  int *worklist = BranchIntSeq_data(&worklist_owner);
  int wl_head = 0, wl_tail = 0;

  /* Entry = block containing instruction 0.  Conservative fallback: if the
   * function is empty or the mapping is bad, assume all blocks reachable. */
  int entry = (cfg->num_instrs > 0) ? cfg->instr_to_block[0] : -1;
  if (entry < 0 || entry >= nb) {
    for (int i = 0; i < nb; i++) reachable[i] = 1;
    return reachable;
  }

  reachable[entry] = 1;
  worklist[wl_tail++] = entry;

#define MARK(blk_)                                                            \
  do {                                                                        \
    int _b = (blk_);                                                          \
    if (_b >= 0 && _b < nb && !reachable[_b]) {                               \
      reachable[_b] = 1;                                                      \
      worklist[wl_tail++] = _b;                                               \
    }                                                                         \
  } while (0)

  while (wl_head < wl_tail) {
    int b = worklist[wl_head++];
    IRBasicBlock *bb = &cfg->blocks[b];

    /* Find terminator: last non-NOP instruction in the block. */
    int term = -1;
    for (int i = bb->end_idx - 1; i >= bb->start_idx; i--) {
      if (ir->compact_instructions[i].op != TCCIR_OP_NOP) {
        term = i;
        break;
      }
    }

    /* Helper: fall through to the block containing bb->end_idx. */
    int fall_block = -1;
    if (bb->end_idx < n_instrs)
      fall_block = cfg->instr_to_block[bb->end_idx];

    if (term < 0) {
      /* All NOPs: fall through. */
      MARK(fall_block);
      continue;
    }

    IRQuadCompact *q = &ir->compact_instructions[term];
    if (q->op == TCCIR_OP_JUMP) {
      int target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      int tb = (target >= 0 && target < cfg->num_instrs) ?
               cfg->instr_to_block[target] : -1;
      MARK(tb);
    } else if (q->op == TCCIR_OP_JUMPIF) {
      int target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      int tb = (target >= 0 && target < cfg->num_instrs) ?
               cfg->instr_to_block[target] : -1;
      MARK(tb);
      MARK(fall_block);
    } else if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
               q->op == TCCIR_OP_TRAP) {
      /* No successors. */
    } else if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE) {
      /* Conservative: keep all CFG successors reachable. */
      for (int si = 0; si < bb->num_succs; si++)
        MARK(bb->succs[si]);
    } else {
      MARK(fall_block);
    }
  }

#undef MARK

  return reachable;
}

/* After branch folding, blocks may be transitively unreachable.  Walk every
 * phi in the function and drop operands whose pred_block is no longer
 * reachable.  This is what unblocks SCCP/cprop on values like
 *   merge_phi(rA from live_block, rB from now-dead_block)
 * which previously kept def_count > 1 and prevented constant folding. */
static int ssa_branch_prune_unreachable_phis(IRSSAOptCtx *ctx)
{
  if (!ctx->ssa || !ctx->ssa->block_phis || !ctx->cfg) return 0;

  uint8_t *reachable = ssa_opt_compute_reachable_blocks(ctx);
  if (!reachable) return 0;

  int nb = ctx->cfg->num_blocks;
  int changes = 0;

  /* For each block whose phis we want to clean, gather the unique set of
   * unreachable predecessors and drop each in turn.  ssa_drop_phi_edge
   * walks every phi at the target and removes all matching operands, so
   * one call per (dead_pred, target_block) pair handles all phis there. */
  small_sequence(BranchU8Seq) seen_pred_owner = {0};
  BranchU8Seq_init(&seen_pred_owner, (size_t)nb);
  uint8_t *seen_pred = BranchU8Seq_data(&seen_pred_owner);
  for (int b = 0; b < nb; b++) {
    if (!reachable[b]) continue;
    if (!ctx->ssa->block_phis[b]) continue;

    memset(seen_pred, 0, nb);
    int has_dead = 0;
    for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next) {
      for (int i = 0; i < phi->num_operands; i++) {
        int pred = phi->operands[i].pred_block;
        if (pred >= 0 && pred < nb && !reachable[pred] && !seen_pred[pred]) {
          seen_pred[pred] = 1;
          has_dead = 1;
        }
      }
    }
    if (!has_dead) continue;

    /* Count operands before drop for change accounting. */
    int before = 0;
    for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next)
      before += phi->num_operands;

    for (int p = 0; p < nb; p++) {
      if (seen_pred[p])
        ssa_drop_phi_edge(ctx, p, b);
    }

    int after = 0;
    for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next)
      after += phi->num_operands;
    changes += before - after;
  }

  tcc_free(reachable);
  return changes;
}

/* Non-negative branch folding: a soft-float flag-setting compare
 * (__aeabi_cdcmple / __aeabi_cfcmple) of a provably non-negative value against
 * 0.0 has a compile-time-known outcome for the >= / < family, so the following
 * JUMPIF folds.  See docs/plan_legacy_flat_ir_ssa_retire.md (nonneg_branch_fold
 * → ssa:branch). */
static const char *ssa_nonneg_func_names[] = {
    "fabs", "fabsf", "abs", "labs", "llabs", "strlen", "sizeof",
};
static const char *ssa_flag_cmp_funcs[] = {
    "__aeabi_cdcmple", "__aeabi_cfcmple",
};

static const char *ssa_call_callee_name(TCCIRState *ir, IRQuadCompact *q)
{
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  return callee ? get_tok_str(callee->v, NULL) : NULL;
}

static int ssa_name_in(const char *name, const char **tbl, int cnt)
{
  if (!name) return 0;
  for (int j = 0; j < cnt; j++)
    if (strcmp(name, tbl[j]) == 0) return 1;
  return 0;
}

/* First param operand of the call at call_idx (scans the contiguous
 * FUNCPARAMVAL block preceding it for param_idx matching this call). */
static int ssa_call_param0_imm(TCCIRState *ir, int call_idx, int64_t *out)
{
  IRQuadCompact *cq = &ir->compact_instructions[call_idx];
  uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, cq));
  int call_id = TCCIR_DECODE_CALL_ID(enc);
  for (int k = call_idx - 1; k >= 0; k--) {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    if (kq->op == TCCIR_OP_NOP || kq->op == TCCIR_OP_FUNCPARAMVOID) continue;
    if (kq->op != TCCIR_OP_FUNCPARAMVAL) break;
    uint32_t penc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, kq));
    if (TCCIR_DECODE_CALL_ID(penc) != call_id) break;
    if (TCCIR_DECODE_PARAM_IDX(penc) == 0) {
      IROperand ps = tcc_ir_op_get_src1(ir, kq);
      if (!irop_is_immediate(ps)) return 0;
      *out = irop_get_imm64_ex(ir, ps);
      return 1;
    }
  }
  return 0;
}

/* A call whose result is provably non-negative: fabs/abs/strlen/... or
 * __aeabi_f2d of a non-negative single-precision constant. */
static int ssa_call_result_nonneg(TCCIRState *ir, int def_idx)
{
  IRQuadCompact *dq = &ir->compact_instructions[def_idx];
  if (dq->op != TCCIR_OP_FUNCCALLVAL) return 0;
  const char *name = ssa_call_callee_name(ir, dq);
  if (!name) return 0;
  if (ssa_name_in(name, ssa_nonneg_func_names,
                  (int)(sizeof(ssa_nonneg_func_names) / sizeof(ssa_nonneg_func_names[0]))))
    return 1;
  if (strcmp(name, "__aeabi_f2d") == 0) {
    int64_t fimm;
    if (ssa_call_param0_imm(ir, def_idx, &fimm)) {
      uint32_t fbits = (uint32_t)fimm;
      uint32_t sign = (fbits >> 31) & 1;
      uint32_t exp = (fbits >> 23) & 0xFF;
      uint32_t mant = fbits & 0x7FFFFF;
      if (!sign && !(exp == 0xFF && mant != 0)) return 1;
    }
  }
  return 0;
}

/* Is the value read from `vreg` at `use_idx` provably non-negative?  TEMP
 * vregs resolve through SSA vinfo; unpromoted VAR slots (e.g. a `double`
 * local holding fabs(x)) are resolved by scanning back to the nearest slot
 * def, bailing on any potentially-aliasing intervening write. */
static int ssa_vreg_is_nonneg(IRSSAOptCtx *ctx, int32_t vreg, int use_idx)
{
  if (vreg < 0) return 0;
  TCCIRState *ir = ctx->ir;

  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vreg);
    if (!vi || vi->def_instr < 0 || vi->def_count > 1) return 0;
    return ssa_call_result_nonneg(ir, vi->def_instr);
  }

  if (TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_VAR) return 0;
  int var_pos = TCCIR_DECODE_VREG_POSITION(vreg);
  for (int k = use_idx - 1; k >= 0; k--) {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    if (kq->op == TCCIR_OP_NOP) continue;
    if (irop_config[kq->op].has_dest) {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      int32_t kdv = irop_get_vreg(kd);
      if (kdv >= 0 && TCCIR_DECODE_VREG_TYPE(kdv) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(kdv) == var_pos) {
        if (kq->op == TCCIR_OP_STORE && kd.is_lval && !kd.is_local)
          continue; /* pointer-deref through V, not a slot def */
        return ssa_call_result_nonneg(ir, k);
      }
    }
    if (kq->op == TCCIR_OP_STORE_INDEXED || kq->op == TCCIR_OP_STORE_POSTINC)
      return 0;
    if (kq->op == TCCIR_OP_STORE) {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      if (kd.tag == IROP_TAG_STACKOFF && kd.is_local && kd.is_lval)
        continue;
      return 0;
    }
  }
  return 0;
}

static int ssa_fold_nonneg_cmp(IRSSAOptCtx *ctx, int call_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *call_q = &ir->compact_instructions[call_idx];
  int n = ir->next_instruction_index;

  const char *cmp_name = ssa_call_callee_name(ir, call_q);
  if (!ssa_name_in(cmp_name, ssa_flag_cmp_funcs,
                   (int)(sizeof(ssa_flag_cmp_funcs) / sizeof(ssa_flag_cmp_funcs[0]))))
    return 0;

  uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call_q));
  int call_id = TCCIR_DECODE_CALL_ID(enc);

  IROperand p0 = IROP_NONE, p1 = IROP_NONE;
  int have_p0 = 0, have_p1 = 0;
  for (int k = call_idx - 1; k >= 0; k--) {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    if (kq->op == TCCIR_OP_NOP || kq->op == TCCIR_OP_FUNCPARAMVOID) continue;
    if (kq->op != TCCIR_OP_FUNCPARAMVAL) break;
    uint32_t penc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, kq));
    if (TCCIR_DECODE_CALL_ID(penc) != call_id) break;
    int pidx = TCCIR_DECODE_PARAM_IDX(penc);
    if (pidx == 0 && !have_p0) { p0 = tcc_ir_op_get_src1(ir, kq); have_p0 = 1; }
    else if (pidx == 1 && !have_p1) { p1 = tcc_ir_op_get_src1(ir, kq); have_p1 = 1; }
  }
  if (!have_p0 || !have_p1) return 0;

  int nonneg_is_arg0;
  if (!irop_is_immediate(p0) && irop_is_immediate(p1) &&
      irop_get_imm64_ex(ir, p1) == 0 && ssa_vreg_is_nonneg(ctx, irop_get_vreg(p0), call_idx))
    nonneg_is_arg0 = 1;
  else if (irop_is_immediate(p0) && irop_get_imm64_ex(ir, p0) == 0 &&
           !irop_is_immediate(p1) && ssa_vreg_is_nonneg(ctx, irop_get_vreg(p1), call_idx))
    nonneg_is_arg0 = 0;
  else
    return 0;

  int j = call_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP) j++;
  if (j >= n) return 0;
  IRQuadCompact *jump_q = &ir->compact_instructions[j];
  if (jump_q->op != TCCIR_OP_JUMPIF) return 0;

  int cond_tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump_q));
  int fold_result = -1;
  if (nonneg_is_arg0) {
    if (cond_tok == TOK_GE || cond_tok == TOK_UGE) fold_result = 1;
    else if (cond_tok == TOK_LT || cond_tok == TOK_ULT) fold_result = 0;
  } else {
    if (cond_tok == TOK_LE || cond_tok == TOK_ULE) fold_result = 1;
    else if (cond_tok == TOK_GT || cond_tok == TOK_UGT) fold_result = 0;
  }
  if (fold_result < 0) return 0;

  int jumpif_block = ssa_block_for_instr(ctx->cfg, j);
  int target_idx = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jump_q));
  int target_block = ssa_block_for_instr(ctx->cfg, target_idx);
  int fallthru_block = ssa_block_for_instr(ctx->cfg, j + 1);

  if (fold_result == 1) {
    IROperand dest = tcc_ir_op_get_dest(ir, jump_q);
    jump_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, dest);
    if (jumpif_block >= 0 && fallthru_block >= 0 && fallthru_block != target_block)
      ssa_drop_phi_edge(ctx, jumpif_block, fallthru_block);
  } else {
    jump_q->op = TCCIR_OP_NOP;
    if (jumpif_block >= 0 && target_block >= 0 && target_block != fallthru_block)
      ssa_drop_phi_edge(ctx, jumpif_block, target_block);
  }
  return 1;
}

static const IRSSAOptGen branch_gens[] = {
  { TCCIR_OP_CMP,          ssa_fold_cmp_jumpif, "branch_cmp" },
  { TCCIR_OP_TEST_ZERO,    ssa_fold_test_zero,  "branch_tz" },
  { TCCIR_OP_FUNCCALLVOID, ssa_fold_nonneg_cmp, "branch_nonneg" },
};

int ssa_opt_branch(IRSSAOptCtx *ctx)
{
  int changes = ssa_opt_run_gens(ctx, branch_gens,
                                 sizeof(branch_gens) / sizeof(branch_gens[0]));
  /* Folding may have created transitively-unreachable blocks whose phi
   * operands still pollute multi-def merges.  Prune them so SCCP/cprop on
   * the next iteration sees clean single-def values.  Only run when we
   * actually folded — unreachable blocks can only be created by folding. */
  if (changes > 0)
    changes += ssa_branch_prune_unreachable_phis(ctx);
  return changes;
}
