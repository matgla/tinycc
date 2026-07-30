/*
 *  TCC IR - SSA DCE: dead anonymous StackLoc stores
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
#include "dce_common.h"
#include "dce_passes.h"


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

int dce_dead_stackloc_stores(IRSSAOptCtx *ctx)
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

  /* Reads by an instruction whose dest TEMP is itself dead must not keep a slot alive. */
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
        /* Addr[StackLoc] into a dead TEMP never escapes, so it marks nothing. */
        if (irop_config[q->op].has_dest) {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (dvr >= 0 &&
              TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP &&
              !sl_temp_has_live_uses(ctx, dvr))
            goto skip_src1;
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
