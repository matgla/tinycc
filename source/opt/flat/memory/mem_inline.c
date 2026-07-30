/*
 *  TCC IR - small constant-size mem* call inline expansion
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* memcpy/memset calls with a small constant size become plain
 * LOAD_INDEXED/STORE_INDEXED quads so store-load forwarding, DSE and the
 * fusion group see the accesses (and the call + argument marshaling
 * disappear).  Thresholds mirror arm-none-eabi-gcc -O2 on Cortex-M33
 * (measured 2026-07-21):
 *
 *   memcpy   small const n -> word/halfword/byte copies.  ARMv8-M mainline
 *            tolerates unaligned ldr/ldrh/str/strh, so no alignment proof is
 *            needed; the pieces carry an UNDERALIGN hint which keeps the
 *            ldrd/strd pairing peepholes away from possibly-unaligned pairs.
 *   memset   small const n AND constant fill value -> immediate stores.
 *   memmove  NEVER inlined -- gcc calls it even for n == 8, and TCC emits
 *            __aeabi_memmove exactly where overlap is possible.
 *
 * Copies are emitted in groups of two pieces (ld,ld,st,st) - bounded register
 * pressure, and adjacent word loads stay pairable should alignment become
 * provable later.  Loads within a group precede stores, so an exactly
 * self-overlapping copy (s = s) stays correct; partial overlap is UB for
 * memcpy and cannot occur for the compiler-generated __aeabi_memcpy* struct
 * copies (those use __aeabi_memmove when overlap is possible). */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include <string.h>

/* Size ceilings are set where the expansion is a strict static win over the
 * call site (>= the `movs #n; bl` minimum), NOT at gcc's -O2 ceilings
 * (memcpy 32 / memset 28).  Mirroring gcc was measured at +46k corpus
 * instructions and ~zero cycle change: the static metric favours a bl over a
 * 6-18 instruction expansion, and the runtime helpers are already fast for
 * one-shot calls.  Kept core:
 *   memcpy, single piece (n in {1,2,4})   -> ld + st        (2 <= 2)
 *   memset, <= 2 pieces (n in 1,2,3..6,8) -> movs + <=2 str (3 <= 3)
 * plus n == 0 calls dropped outright.  These still delete a call, unlock
 * store-load forwarding through the copied bytes (the float-bits memcpy(&u,
 * &f,4) reinterpret idiom), and never grow code. */
#define MI_MEMCPY_MAX 4
#define MI_MEMSET_MAX 8
#define MI_MEMCPY_PIECES 1
#define MI_MEMSET_PIECES 2
#define MI_MAX_PIECES 10 /* 32 bytes -> at most 8 words + 1 half + 1 byte */

enum
{
  MI_NONE,
  MI_MEMCPY,      /* (dst, src, n) */
  MI_MEMSET,      /* (dst, val, n) */
  MI_MEMSET_AEABI,/* (dst, n, val) */
  MI_MEMCLR       /* (dst, n), fill = 0 */
};

static int mi_classify(const char *nm, int *aligned4)
{
  if (!nm)
    return MI_NONE;
  *aligned4 = 0;
  if (!strcmp(nm, "__aeabi_memcpy4") || !strcmp(nm, "__aeabi_memcpy8"))
  {
    *aligned4 = 1;
    return MI_MEMCPY;
  }
  if (!strcmp(nm, "memcpy") || !strcmp(nm, "__aeabi_memcpy"))
    return MI_MEMCPY;
  if (!strcmp(nm, "memset"))
    return MI_MEMSET;
  if (!strcmp(nm, "__aeabi_memset4") || !strcmp(nm, "__aeabi_memset8"))
  {
    *aligned4 = 1;
    return MI_MEMSET_AEABI;
  }
  if (!strcmp(nm, "__aeabi_memset"))
    return MI_MEMSET_AEABI;
  if (!strcmp(nm, "__aeabi_memclr4") || !strcmp(nm, "__aeabi_memclr8"))
  {
    *aligned4 = 1;
    return MI_MEMCLR;
  }
  if (!strcmp(nm, "__aeabi_memclr"))
    return MI_MEMCLR;
  return MI_NONE;
}

/* A usable INDEXED base: the pointer VALUE in a vreg, or a symbol address.
 * An is_lval operand is the pointee, not the pointer.  A STACKOFF address
 * (`Addr[StackLoc]`, the direct `memcpy(&u, &f, 4)` shape) does NOT work as
 * an INDEXED base -- the backend reads a STACKOFF operand as the slot's
 * VALUE, so the expansion dereferenced the copied bits as an address
 * (221_fuzz repro: BusFault with BFAR = the float pattern being copied).
 * Those accesses become direct StackLoc LOAD/STOREs instead (*is_stack) --
 * the canonical slot shape sl_forward and the promotion passes understand,
 * so the copy collapses and the &var address-take disappears (20141107-1:
 * LEA+INDEXED left a 17-instruction opaque stack dance per inlined site).
 * is_llocal slots hold a POINTER to the data (VLA), not the data: decline. */
static int mi_base_ok(IROperand op, int *is_stack)
{
  int tag = irop_get_tag(op);
  *is_stack = 0;
  if (op.is_lval)
    return 0;
  if (tag == IROP_TAG_VREG)
    return irop_get_vreg(op) >= 0;
  if (tag == IROP_TAG_SYMREF)
    return 1;
  if (tag == IROP_TAG_STACKOFF)
  {
    if (op.is_llocal)
      return 0;
    *is_stack = 1;
    return 1;
  }
  return 0;
}

/* If op is a TEMP with a single def `LEA T <- &V` (frontend-inlined &var
 * arguments), return the VAR operand in is_lval slot form: the expansion can
 * then access the var's slot directly, sl_forward/promotion see through it,
 * and the LEA dies -> the var stops being address-taken and can promote
 * (20141107-1: the LEA+INDEXED form left an opaque 6-instruction stack dance
 * per inlined checkf site).  Only valid for offset-0 single-piece accesses:
 * a VAR operand cannot carry a byte offset. */
static int mi_resolve_lea_base(TCCIRState *ir, IROperand op, IROperand *out_slot)
{
  int32_t vr;
  int def_idx = -1;
  if (irop_get_tag(op) != IROP_TAG_VREG || op.is_lval)
    return 0;
  vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand d;
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
      continue; /* store dest is a use, not a def */
    d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval)
      continue;
    if (irop_get_vreg(d) == vr)
    {
      if (def_idx >= 0)
        return 0; /* multiple defs */
      def_idx = i;
    }
  }
  if (def_idx < 0)
    return 0;
  {
    IRQuadCompact *dq = &ir->compact_instructions[def_idx];
    IROperand ls;
    if (dq->op != TCCIR_OP_LEA)
      return 0;
    ls = tcc_ir_op_get_src1(ir, dq);
    if (ls.is_llocal)
      return 0;
    /* LEA of a stack slot: hand back the STACKOFF address form -- the caller
     * routes it through the per-piece direct-slot path (offset adjustable). */
    if (irop_get_tag(ls) == IROP_TAG_STACKOFF)
    {
      *out_slot = ls;
      out_slot->is_lval = 0;
      return 2;
    }
    if (irop_get_tag(ls) != IROP_TAG_VREG)
      return 0;
    if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(ls)) != TCCIR_VREG_TYPE_VAR)
      return 0;
    *out_slot = ls;
    out_slot->is_lval = 1;
    return 1;
  }
}

static int mi_btype(int w)
{
  return w == 4 ? IROP_BTYPE_INT32 : w == 2 ? IROP_BTYPE_INT16 : IROP_BTYPE_INT8;
}

static int mi_pieces(int n, int offs[MI_MAX_PIECES], int ws[MI_MAX_PIECES])
{
  int cnt = 0, off = 0;
  while (n > 0 && cnt < MI_MAX_PIECES)
  {
    int w = n >= 4 ? 4 : n >= 2 ? 2 : 1;
    offs[cnt] = off;
    ws[cnt] = w;
    cnt++;
    off += w;
    n -= w;
  }
  return n == 0 ? cnt : -1;
}

/* Rewrite the quad at an existing slot in place.  Pool layout matches the
 * INDEXED convention: [dest, src1, src2, scale].  Slots are recycled from the
 * call's own PARAM/CALL quads (never inserted): inserting quads grows the
 * NOP-inclusive slot count that the auto-inline gates read, flipping inline
 * decisions in callers (20141107-1 main 15->84). */
static void mi_emit_at(TCCIRState *ir, int slot, int op, IROperand dest, IROperand src1, IROperand src2)
{
  IRQuadCompact *q = &ir->compact_instructions[slot];
  q->op = (TccIrOp)op;
  q->operand_base = (uint32_t)tcc_ir_iroperand_pool_add(ir, dest);
  tcc_ir_iroperand_pool_add(ir, src1);
  tcc_ir_iroperand_pool_add(ir, src2);
  tcc_ir_iroperand_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
}

static int mi_result_used(TCCIRState *ir, int ci)
{
  IRQuadCompact *call = &ir->compact_instructions[ci];
  IROperand cd;
  int32_t cdv;
  if (call->op != TCCIR_OP_FUNCCALLVAL)
    return 0;
  cd = tcc_ir_op_get_dest(ir, call);
  cdv = irop_get_vreg(cd);
  if (cdv < 0)
    return 0;
  for (int j = 0; j < ir->next_instruction_index; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (j == ci || q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 1; k <= 3; k++)
    {
      IROperand op = (k == 1)   ? tcc_ir_op_get_src1(ir, q)
                     : (k == 2) ? tcc_ir_op_get_src2(ir, q)
                                : tcc_ir_op_get_accum(ir, q);
      if (irop_get_vreg(op) == cdv)
        return 1;
    }
    /* an is_lval dest is an address USE (store through the pointer) */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (d.is_lval && irop_get_vreg(d) == cdv)
        return 1;
    }
  }
  return 0;
}

static int mi_try_expand(TCCIRState *ir, int ci, int kind, int aligned4)
{
  IROperand dst, p1, p2, src;
  int has1, has2;
  int64_t nn, val = 0;
  int offs[MI_MAX_PIECES], ws[MI_MAX_PIECES];
  int np;
  int dst_stk = 0, src_stk = 0;
  int dst_var = 0, src_var = 0;
  IROperand dst_varop, src_varop;

  if (!ir_opt_get_call_param_operand(ir, ci, 0, &dst))
    return 0;
  has1 = ir_opt_get_call_param_operand(ir, ci, 1, &p1);
  has2 = ir_opt_get_call_param_operand(ir, ci, 2, &p2);

  memset(&src, 0, sizeof(src));
  switch (kind)
  {
  case MI_MEMCPY:
    if (!has1 || !has2 || !irop_is_plain_imm(p2))
      return 0;
    src = p1;
    nn = irop_get_imm64_ex(ir, p2);
    if (nn < 0 || nn > MI_MEMCPY_MAX)
      return 0;
    if (nn > 0 && !mi_base_ok(src, &src_stk))
      return 0;
    break;
  case MI_MEMSET:
    if (!has1 || !has2 || !irop_is_plain_imm(p1) || !irop_is_plain_imm(p2))
      return 0;
    val = irop_get_imm64_ex(ir, p1);
    nn = irop_get_imm64_ex(ir, p2);
    if (nn < 0 || nn > MI_MEMSET_MAX)
      return 0;
    break;
  case MI_MEMSET_AEABI:
    if (!has1 || !has2 || !irop_is_plain_imm(p1) || !irop_is_plain_imm(p2))
      return 0;
    nn = irop_get_imm64_ex(ir, p1);
    val = irop_get_imm64_ex(ir, p2);
    if (nn < 0 || nn > MI_MEMSET_MAX)
      return 0;
    break;
  case MI_MEMCLR:
    if (!has1 || !irop_is_plain_imm(p1))
      return 0;
    nn = irop_get_imm64_ex(ir, p1);
    if (nn < 0 || nn > MI_MEMSET_MAX)
      return 0;
    break;
  default:
    return 0;
  }

  if (nn > 0 && !mi_base_ok(dst, &dst_stk))
    return 0;
  if (mi_result_used(ir, ci))
    return 0;

  np = mi_pieces((int)nn, offs, ws);
  if (np < 0)
    return 0;
  /* Piece caps enforce the strict static-win policy (see header) and keep
   * the expansion within the call's own quad slots, so function IR size
   * stays stable and auto-inline decisions for callers don't flip (the
   * slot-count trap: 20141107-1 main 15->84 with insertion-based emission). */
  if (np > (kind == MI_MEMCPY ? MI_MEMCPY_PIECES : MI_MEMSET_PIECES))
    return 0;
  /* STACKOFF offsets are abstract slot IDs remapped by frame allocation --
   * NEVER do arithmetic on them, and never rebuild them: reuse the original
   * operand verbatim (only is_lval/btype/width flags changed).  Hence every
   * direct-slot access requires a single offset-0 piece; multi-piece stack
   * cases decline (a STACKOFF INDEXED base would read the slot VALUE). */
  if ((dst_stk || src_stk) && np > 1)
    return 0;
  /* Bases that are single-def `LEA T <- &slot` temps resolve to the slot
   * (offset-0 single piece only, for the same reason). */
  if (np == 1 && !dst_stk)
  {
    int r = mi_resolve_lea_base(ir, dst, &dst_varop);
    if (r == 2)
    {
      dst = dst_varop;
      dst_stk = 1;
    }
    else if (r == 1)
      dst_var = 1;
  }
  if (np == 1 && kind == MI_MEMCPY && nn > 0 && !src_stk)
  {
    int r = mi_resolve_lea_base(ir, src, &src_varop);
    if (r == 2)
    {
      src = src_varop;
      src_stk = 1;
    }
    else if (r == 1)
      src_var = 1;
  }

  /* Collect the call's own quad slots (params + CALL) to recycle in place.
   * Foreign instructions may sit inside the span (LEAs and other argument
   * evaluation interleave with PARAM quads); the expansion only fills slots
   * AFTER the last foreign instruction, so every def feeding the bases — and
   * any store that could alias them — still executes before the copy. */
  int slots[4];
  int n_slots = 0;
  {
    int pidx[3] = {-1, -1, -1};
    int n_params = (kind == MI_MEMCLR) ? 2 : 3;
    int lo = ci;
    int last_foreign = -1;
    int all_slots[4];
    int n_all = 0;
    for (int k = 0; k < n_params; k++)
    {
      pidx[k] = ir_opt_get_call_param_index(ir, ci, k);
      if (pidx[k] < 0)
        return 0;
      if (pidx[k] < lo)
        lo = pidx[k];
    }
    for (int j = lo; j <= ci; j++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP)
        continue;
      if (j == ci || j == pidx[0] || j == pidx[1] || j == pidx[2])
      {
        if (n_all >= 4)
          return 0;
        all_slots[n_all++] = j;
        continue;
      }
      last_foreign = j;
    }
    /* Usable slots sit after the last foreign instruction; params before it
     * are NOPed by ir_opt_nop_call_params at commit (no mutation here — the
     * expansion may still decline below). */
    for (int k = 0; k < n_all; k++)
      if (all_slots[k] > last_foreign)
        slots[n_slots++] = all_slots[k];
  }
  {
    int quads_needed = (kind == MI_MEMCPY) ? 2 * np : np;
    if (quads_needed > n_slots)
      return 0;

    /* mem* arguments carry no alignment guarantee (aeabi 4/8 variants
     * excepted), but codegen's LDRD/STRD peepholes assume INT32 indexed
     * accesses are C-aligned.  The UNDERALIGN hint keeps them from pairing
     * word pieces into LDRD/STRD, which ALWAYS fault on unaligned addresses
     * on ARMv7-M/v8-M -- exactly why gcc emits plain ldr/ldr/str/str for
     * memcpy(char*,char*,8).  The hint rides on dest/value operands as well
     * as the bases: base-materialization passes (global_base_share et al.)
     * rebuild base operands and would drop an aux carried only there. */
    if (!aligned4)
    {
      dst.aux |= IROP_AUX_UNDERALIGN;
      src.aux |= IROP_AUX_UNDERALIGN;
    }

    /* NOP everything, then fill the LAST quads_needed slots so the accesses
     * sit at the original call position. */
    ir_opt_nop_call_params(ir, ci);
    ir->compact_instructions[ci].op = TCCIR_OP_NOP;
    int si = n_slots - quads_needed;

    if (kind == MI_MEMCPY)
    {
      /* groups of two pieces: ld,ld,st,st */
      for (int g = 0; g < np; g += 2)
      {
        int gend = (g + 1 < np) ? g + 2 : g + 1;
        int tv[2];
        for (int k = g; k < gend; k++)
        {
          IROperand d;
          tv[k - g] = tcc_ir_get_vreg_temp(ir);
          d = irop_make_vreg(tv[k - g], mi_btype(ws[k]));
          d.is_unsigned = 1;
          if (src_stk || src_var)
          {
            IROperand slot = irop_retype_scalar(src_stk ? src : src_varop, mi_btype(ws[k]));
            slot.is_lval = 1;
            slot.is_unsigned = 1;
            mi_emit_at(ir, slots[si++], TCCIR_OP_LOAD, d, slot,
                       irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
          }
          else
          {
            if (!aligned4)
              d.aux |= IROP_AUX_UNDERALIGN;
            mi_emit_at(ir, slots[si++], TCCIR_OP_LOAD_INDEXED, d, src,
                       irop_make_imm32(-1, offs[k], IROP_BTYPE_INT32));
          }
        }
        for (int k = g; k < gend; k++)
        {
          IROperand v = irop_make_vreg(tv[k - g], mi_btype(ws[k]));
          v.is_unsigned = 1;
          if (dst_stk || dst_var)
          {
            IROperand slot = irop_retype_scalar(dst_stk ? dst : dst_varop, mi_btype(ws[k]));
            slot.is_lval = 1;
            slot.is_unsigned = 1;
            mi_emit_at(ir, slots[si++], TCCIR_OP_STORE, slot, v,
                       irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
          }
          else
          {
            if (!aligned4)
              v.aux |= IROP_AUX_UNDERALIGN;
            mi_emit_at(ir, slots[si++], TCCIR_OP_STORE_INDEXED, dst, v,
                       irop_make_imm32(-1, offs[k], IROP_BTYPE_INT32));
          }
        }
      }
    }
    else
    {
      uint32_t b = (uint32_t)(val & 0xff);
      uint32_t word = b * 0x01010101u;
      for (int k = 0; k < np; k++)
      {
        uint32_t pv = ws[k] == 4 ? word : ws[k] == 2 ? (word & 0xffffu) : b;
        IROperand v = irop_make_imm32(-1, (int32_t)pv, mi_btype(ws[k]));
        if (dst_stk || dst_var)
        {
          IROperand slot = irop_retype_scalar(dst_stk ? dst : dst_varop, mi_btype(ws[k]));
          slot.is_lval = 1;
          slot.is_unsigned = 1;
          mi_emit_at(ir, slots[si++], TCCIR_OP_STORE, slot, v,
                     irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
        }
        else
        {
          if (!aligned4)
            v.aux |= IROP_AUX_UNDERALIGN;
          mi_emit_at(ir, slots[si++], TCCIR_OP_STORE_INDEXED, dst, v,
                     irop_make_imm32(-1, offs[k], IROP_BTYPE_INT32));
        }
      }
    }
  }
  return 1;
}

int tcc_ir_opt_mem_inline(TCCIRState *ir)
{
  static int disabled = -1;
  int changes = 0;

  if (disabled < 0)
    disabled = getenv("TCC_NO_MEM_INLINE") != NULL;
  if (disabled || tcc_ir_opt_pass_disabled("mem_inline"))
    return 0;
  if (tcc_state->no_builtin_funcs & NO_BUILTIN_MEMFUNCS)
    return 0;

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    Sym *callee;
    int kind;
    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;
    callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    int aligned4 = 0;
    kind = mi_classify(get_tok_str(callee->v, NULL), &aligned4);
    if (kind == MI_NONE)
      continue;
    changes += mi_try_expand(ir, i, kind, aligned4);
  }
  return changes;
}

int tcc_ir_opt_mem_inline_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_mem_inline(ctx->ir);
}
