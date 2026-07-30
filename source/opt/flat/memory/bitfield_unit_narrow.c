/*
 *  TCC IR - Narrow a whole-word bitfield read-modify-write to its byte/halfword
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "tccir_operand.h"

extern int tcc_ir_opt_pass_disabled(const char *name);

/* ============================================================================
 * Bitfield unit narrowing (tcc_ir_opt_bitfield_unit_narrow)
 * ============================================================================
 *
 * A bitfield lives in a storage unit of its declared type, so `d.k += x` on
 * `unsigned k : 8` reads and writes a whole word even when the field occupies
 * exactly one byte of it.  The frontend emits, after global_deref_cse has
 * collapsed the duplicate read:
 *
 *     Tv    <- G+A***DEREF***(INT32)  [ASSIGN]      word load
 *     Ta    <- Tv SHL #8                            \  extract the field
 *     Tf    <- Ta SHR #24                           /  (zero-extended)
 *     ...
 *     Tval  <- Tsrc AND #255                        mask the new value
 *     P     <- Tval SHL #16                         position it
 *     Tm    <- Tv AND #-16711681                    clear the field
 *     Tr    <- P OR Tm                              merge
 *     G+A***DEREF***(INT32) <- Tr [STORE]           word store
 *
 * When the field EXACTLY fills a naturally-aligned byte or halfword of that
 * word, every one of those steps is redundant: the extract, the mask, the
 * position, the clear and the merge all collapse into a narrow load and a
 * narrow store of the same unit.  This is what GCC emits (`ldrb`/`strb`), and
 * on this shape it is the difference between eight instructions and three.
 *
 * The rewrite is only applied when the whole dataflow is accounted for:
 *   - Tv is read only by the clear and by extractions of exactly that field,
 *     and not at all after the store;
 *   - the stored value reaches the merge through `AND #((1<<w)-1)`, so
 *     truncating it to the unit is provably the same value (a value with bits
 *     above the field would land OUTSIDE the unit in the original word merge,
 *     and narrowing must not silently change that);
 *   - the load and the store name the same symbol and addend at INT32.
 *
 * Little-endian only: the byte at bit offset `lsb` is at byte `lsb/8`.  The
 * ARMv8-M target this backend emits for is always little-endian.  The unit
 * address inherits the word access's alignment (A + lsb/8 is unit-aligned
 * whenever A is word-aligned, which the original LDR already assumes).
 */

#define BFUN_MAX_EXTRACTS 8

typedef struct {
  int shl_idx; /* index of the `Tv SHL #k` half, or -1 for the SHR-only form */
  int shr_idx; /* index of the instruction producing the extracted value */
} BfunExtract;

static int bfun_is_temp(int32_t vr)
{
  return vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP;
}

/* Immediate value of an operand, or 0 with *ok cleared. */
static int64_t bfun_imm(TCCIRState *ir, IROperand op, int *ok)
{
  if (!irop_is_immediate(op) || op.is_sym)
  {
    *ok = 0;
    return 0;
  }
  return irop_get_imm64_ex(ir, op);
}

/* The unit mask must be a naturally-aligned byte or halfword. */
static int bfun_unit_of_mask(uint32_t um, int *out_lsb, int *out_width)
{
  for (int k = 0; k < 4; k++)
    if (um == (0xFFu << (k * 8)))
    {
      *out_lsb = k * 8;
      *out_width = 8;
      return 1;
    }
  for (int k = 0; k < 2; k++)
    if (um == (0xFFFFu << (k * 16)))
    {
      *out_lsb = k * 16;
      *out_width = 16;
      return 1;
    }
  return 0;
}

/* Same global lvalue word: SYMREF, lval, non-local, INT32, same sym+addend. */
static int bfun_same_word(TCCIRState *ir, IROperand a, IROperand b)
{
  if (irop_get_tag(a) != IROP_TAG_SYMREF || irop_get_tag(b) != IROP_TAG_SYMREF)
    return 0;
  if (!a.is_lval || !b.is_lval || a.is_local || b.is_local)
    return 0;
  if (a.btype != IROP_BTYPE_INT32 || b.btype != IROP_BTYPE_INT32)
    return 0;
  IRPoolSymref *ra = irop_get_symref_ex(ir, a);
  IRPoolSymref *rb = irop_get_symref_ex(ir, b);
  if (!ra || !rb || !ra->sym || ra->sym != rb->sym || ra->addend != rb->addend)
    return 0;
  return 1;
}

/* Does this instruction end the straight-line region or write memory? */
static int bfun_region_break(TCCIRState *ir, IRQuadCompact *q)
{
  switch (q->op)
  {
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_VLA_ALLOC:
      return 1;
    case TCCIR_OP_STORE:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      return !d.is_local || d.is_llocal;
    }
    default:
      return 0;
  }
}

/* Count reads of `vr` in instruction i (dest counts only when it is an lvalue,
 * i.e. a store THROUGH the vreg).  MLA's accumulator lives at operand_base+3
 * where has_src1/has_src2 cannot see it; missing it would let the pass narrow
 * a value that is still read in full width. */
static int bfun_reads(TCCIRState *ir, int i, int32_t vr)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  int n = 0;
  if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vr)
    n++;
  if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == vr)
    n++;
  if (q->op == TCCIR_OP_MLA && irop_get_vreg(tcc_ir_op_get_accum(ir, q)) == vr)
    n++;
  if (irop_config[q->op].has_dest)
  {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(d) == vr && d.is_lval)
      n++;
  }
  return n;
}

/* Sole use of `vr` across the whole function, or -1 if not exactly one. */
static int bfun_sole_use(TCCIRState *ir, int32_t vr)
{
  int found = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    int c = bfun_reads(ir, i, vr);
    if (!c)
      continue;
    if (c > 1 || found >= 0)
      return -1;
    found = i;
  }
  return found;
}

/* Sole non-lvalue definition of `vr`, or -1 if it is not defined exactly once.
 * Every temp this pass rewrites or deletes must be single-def, otherwise the
 * value reaching the use it matched is not the one it inspected. */
static int bfun_sole_def(TCCIRState *ir, int32_t vr)
{
  int found = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(d) != vr || d.is_lval)
      continue;
    if (found >= 0)
      return -1;
    found = i;
  }
  return found;
}

/* `dst <- src OP #imm` with a TEMP dest; returns the dest vreg or -1. */
static int32_t bfun_match_op_imm(TCCIRState *ir, int idx, TccIrOp op, int32_t src_vr, int64_t imm)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op != op)
    return -1;
  if (irop_get_vreg(tcc_ir_op_get_src1(ir, q)) != src_vr)
    return -1;
  int ok = 1;
  if (bfun_imm(ir, tcc_ir_op_get_src2(ir, q), &ok) != imm || !ok)
    return -1;
  if (tcc_ir_barrel_shift_at(ir, q))
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (d.is_lval)
    return -1;
  int32_t dv = irop_get_vreg(d);
  return bfun_is_temp(dv) ? dv : -1;
}

/* Is `op` provably in [0, 2^width)?  The narrow store truncates to `width`
 * bits, so this is what licenses dropping the explicit mask: the original word
 * merge would have placed any higher bit OUTSIDE the unit, changing a
 * neighbouring field.  Conservative and depth-limited - an unproven value just
 * keeps the wide form. */
static int bfun_fits_in_width(TCCIRState *ir, IROperand op, int width, int depth)
{
  if (depth > 6)
    return 0;
  const uint64_t limit = 1ull << width;
  if (irop_is_immediate(op) && !op.is_sym)
  {
    int64_t v = irop_get_imm64_ex(ir, op);
    return v >= 0 && (uint64_t)v < limit;
  }
  /* A zero-extended narrow access already carries the range. */
  if (op.is_unsigned &&
      ((op.btype == IROP_BTYPE_INT8 && width >= 8) || (op.btype == IROP_BTYPE_INT16 && width >= 16)))
    return 1;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_lval)
    return 0;
  int d = bfun_sole_def(ir, vr);
  if (d < 0)
    return 0;
  IRQuadCompact *q = &ir->compact_instructions[d];
  if (tcc_ir_barrel_shift_at(ir, q))
    return 0;
  IROperand s1 = irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE;
  IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE;
  switch (q->op)
  {
    case TCCIR_OP_ASSIGN:
      return bfun_fits_in_width(ir, s1, width, depth + 1);
    case TCCIR_OP_AND:
      /* Masking with anything in range bounds the result. */
      return bfun_fits_in_width(ir, s1, width, depth + 1) || bfun_fits_in_width(ir, s2, width, depth + 1);
    case TCCIR_OP_OR:
    case TCCIR_OP_XOR:
      return bfun_fits_in_width(ir, s1, width, depth + 1) && bfun_fits_in_width(ir, s2, width, depth + 1);
    case TCCIR_OP_SHR:
    {
      int ok = 1;
      int64_t k = bfun_imm(ir, s2, &ok);
      return ok && k >= 32 - width && k < 32;
    }
    case TCCIR_OP_UBFX:
    {
      int ok = 1;
      int64_t p = bfun_imm(ir, s2, &ok);
      if (!ok)
        return 0;
      int w = (int)((p >> 5) & 0x1F);
      return w != 0 && w <= width;
    }
    default:
      return 0;
  }
}

static void bfun_nop(TCCIRState *ir, int idx)
{
  ir->compact_instructions[idx].op = TCCIR_OP_NOP;
}

/* Rewrite instruction `idx` into `dest <- src [ASSIGN]`, keeping dest as is. */
static void bfun_to_assign(TCCIRState *ir, int idx, IROperand src)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  tcc_ir_pool_ensure(ir, 2);
  int base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, dest);
  tcc_ir_pool_add(ir, src);
  q->op = TCCIR_OP_ASSIGN;
  q->operand_base = base;
}

/* Build the narrowed SYMREF lvalue for unit `lsb/8` of the word at `base`. */
static IROperand bfun_unit_operand(TCCIRState *ir, IROperand word, int lsb, int width)
{
  IRPoolSymref *sr = irop_get_symref_ex(ir, word);
  uint32_t idx = tcc_ir_pool_add_symref(ir, sr->sym, (int32_t)(sr->addend + lsb / 8), 0);
  int bt = (width == 8) ? IROP_BTYPE_INT8 : IROP_BTYPE_INT16;
  IROperand op = irop_make_symref(-1, idx, 1 /* lval */, 0 /* not local */, 0, bt);
  op.is_unsigned = 1;
  return op;
}

static int bfun_try_one(TCCIRState *ir, int li)
{
  IRQuadCompact *lq = &ir->compact_instructions[li];
  if (lq->op != TCCIR_OP_ASSIGN)
    return 0;
  IROperand word = tcc_ir_op_get_src1(ir, lq);
  IROperand ldest = tcc_ir_op_get_dest(ir, lq);
  if (irop_get_tag(word) != IROP_TAG_SYMREF || !word.is_lval || word.is_local ||
      word.btype != IROP_BTYPE_INT32 || ldest.is_lval)
    return 0;
  IRPoolSymref *wr = irop_get_symref_ex(ir, word);
  if (!wr || !wr->sym || (wr->sym->type.t & VT_VOLATILE))
    return 0;
  int32_t tv = irop_get_vreg(ldest);
  if (!bfun_is_temp(tv) || bfun_sole_def(ir, tv) != li)
    return 0;

  /* Locate the write-back store, staying inside one clobber-free region. */
  int si = -1;
  int n = ir->next_instruction_index;
  for (int i = li + 1; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->is_jump_target)
      return 0;
    if (q->op == TCCIR_OP_STORE && bfun_same_word(ir, tcc_ir_op_get_dest(ir, q), word))
    {
      si = i;
      break;
    }
    if (bfun_region_break(ir, q))
      return 0;
  }
  if (si < 0)
    return 0;

  /* Every read of Tv must lie in (li, si); the store must not read it. */
  int clear_idx = -1;
  BfunExtract ext[BFUN_MAX_EXTRACTS];
  int num_ext = 0;
  for (int i = 0; i < n; i++)
  {
    if (i == li || ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    int c = bfun_reads(ir, i, tv);
    if (!c)
      continue;
    if (c > 1 || i > si || i < li)
      return 0;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_AND && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == tv)
    {
      if (clear_idx >= 0)
        return 0;
      clear_idx = i;
      continue;
    }
    if (num_ext >= BFUN_MAX_EXTRACTS)
      return 0;
    ext[num_ext].shl_idx = -1;
    ext[num_ext].shr_idx = i;
    num_ext++;
  }
  if (clear_idx < 0)
    return 0;

  /* The clear's mask names the unit. */
  IRQuadCompact *cq = &ir->compact_instructions[clear_idx];
  int ok = 1;
  uint32_t keep = (uint32_t)bfun_imm(ir, tcc_ir_op_get_src2(ir, cq), &ok);
  if (!ok || tcc_ir_barrel_shift_at(ir, cq))
    return 0;
  int lsb, width;
  if (!bfun_unit_of_mask(~keep, &lsb, &width))
    return 0;
  const uint32_t fmask = (width == 8) ? 0xFFu : 0xFFFFu;

  /* clear -> merge */
  int32_t tm = irop_get_vreg(tcc_ir_op_get_dest(ir, cq));
  if (!bfun_is_temp(tm) || bfun_sole_def(ir, tm) != clear_idx)
    return 0;
  int or_idx = bfun_sole_use(ir, tm);
  if (or_idx < 0 || or_idx > si)
    return 0;
  IRQuadCompact *oq = &ir->compact_instructions[or_idx];
  if (oq->op != TCCIR_OP_OR || tcc_ir_barrel_shift_at(ir, oq))
    return 0;
  int32_t or_s1 = irop_get_vreg(tcc_ir_op_get_src1(ir, oq));
  int32_t or_s2 = irop_get_vreg(tcc_ir_op_get_src2(ir, oq));
  int32_t positioned = (or_s1 == tm) ? or_s2 : or_s1;
  if (!bfun_is_temp(positioned))
    return 0;

  /* merge -> store, and nothing else. */
  int32_t tr = irop_get_vreg(tcc_ir_op_get_dest(ir, oq));
  if (!bfun_is_temp(tr) || bfun_sole_def(ir, tr) != or_idx || bfun_sole_use(ir, tr) != si)
    return 0;
  IRQuadCompact *sq = &ir->compact_instructions[si];
  if (irop_get_vreg(tcc_ir_op_get_src1(ir, sq)) != tr)
    return 0;

  /* Undo the positioning shift, then require the explicit field mask: the
   * narrow store truncates to `width` bits, which equals `AND #fmask`, so the
   * stored value is provably unchanged. */
  int shl_idx = -1;
  int32_t masked = positioned;
  if (lsb > 0)
  {
    shl_idx = bfun_sole_def(ir, positioned);
    if (shl_idx < 0 || shl_idx <= li || shl_idx >= or_idx)
      return 0;
    int32_t src = irop_get_vreg(tcc_ir_op_get_src1(ir, &ir->compact_instructions[shl_idx]));
    if (bfun_match_op_imm(ir, shl_idx, TCCIR_OP_SHL, src, lsb) != positioned)
      return 0;
    if (bfun_sole_use(ir, positioned) != or_idx)
      return 0;
    masked = src;
    if (!bfun_is_temp(masked))
      return 0;
  }

  /* Preferred: the value reaches the merge through an explicit `AND #fmask`,
   * which the narrow store subsumes - store the AND's source and drop the AND.
   * Otherwise the value must be provably in range on its own. */
  int value_def = bfun_sole_def(ir, masked);
  int mask_idx = value_def;
  IROperand masked_op = irop_make_vreg(masked, IROP_BTYPE_INT32);
  IROperand stored_op = masked_op;
  if (mask_idx >= 0 && mask_idx < or_idx &&
      bfun_sole_use(ir, masked) == (lsb > 0 ? shl_idx : or_idx) &&
      bfun_match_op_imm(ir, mask_idx, TCCIR_OP_AND,
                        irop_get_vreg(tcc_ir_op_get_src1(ir, &ir->compact_instructions[mask_idx])),
                        (int64_t)fmask) == masked)
  {
    stored_op = tcc_ir_op_get_src1(ir, &ir->compact_instructions[mask_idx]);
  }
  else if (mask_idx >= 0 && mask_idx < or_idx && bfun_fits_in_width(ir, masked_op, width, 0))
  {
    mask_idx = -1; /* nothing to elide; store the value as it stands */
  }
  else
    return 0;
  /* The stored value moves from its definition down to the store, so it must
   * be a plain register/immediate value (not another memory read to re-order)
   * and must not be redefined in between. */
  if (stored_op.is_lval || (irop_get_tag(stored_op) == IROP_TAG_SYMREF && !irop_is_immediate(stored_op)))
    return 0;
  {
    /* Last point the value is known live: the AND we are eliding, or its own
     * (single) definition when there is no AND. */
    int live_from = (mask_idx >= 0) ? mask_idx : value_def;
    int32_t sv = irop_get_vreg(stored_op);
    if (sv >= 0)
      for (int i = live_from + 1; i <= si; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
          continue;
        if (irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == sv)
          return 0;
      }
  }

  /* Each remaining read of Tv must extract exactly this field, so that the
   * zero-extended narrow load can replace it verbatim. */
  for (int e = 0; e < num_ext; e++)
  {
    int i = ext[e].shr_idx;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (irop_get_vreg(tcc_ir_op_get_src1(ir, q)) != tv)
      return 0;
    if (lsb + width == 32 && bfun_match_op_imm(ir, i, TCCIR_OP_SHR, tv, lsb) >= 0)
      continue; /* top field: a single SHR is the whole extraction */
    int32_t ta = bfun_match_op_imm(ir, i, TCCIR_OP_SHL, tv, 32 - lsb - width);
    if (ta < 0)
      return 0;
    int j = bfun_sole_use(ir, ta);
    if (j < 0 || j > si || bfun_match_op_imm(ir, j, TCCIR_OP_SHR, ta, 32 - width) < 0)
      return 0;
    ext[e].shl_idx = i;
    ext[e].shr_idx = j;
  }

  /* --- commit --- */
  IROperand unit = bfun_unit_operand(ir, word, lsb, width);
  IROperand narrow_tv = ldest;
  narrow_tv.btype = (width == 8) ? IROP_BTYPE_INT8 : IROP_BTYPE_INT16;
  narrow_tv.is_unsigned = 1;
  tcc_ir_op_set_dest(ir, lq, narrow_tv);
  tcc_ir_op_set_src1(ir, lq, unit);

  IROperand tv_val = irop_make_vreg(tv, IROP_BTYPE_INT32);
  tv_val.is_unsigned = 1;
  for (int e = 0; e < num_ext; e++)
  {
    if (ext[e].shl_idx >= 0)
      bfun_nop(ir, ext[e].shl_idx);
    bfun_to_assign(ir, ext[e].shr_idx, tv_val);
  }
  bfun_nop(ir, clear_idx);
  if (mask_idx >= 0)
    bfun_nop(ir, mask_idx);
  if (shl_idx >= 0)
    bfun_nop(ir, shl_idx);
  bfun_nop(ir, or_idx);

  IROperand store_dest = bfun_unit_operand(ir, word, lsb, width);
  tcc_ir_op_set_dest(ir, sq, store_dest);
  tcc_ir_op_set_src1(ir, sq, stored_op);
  return 1;
}

int tcc_ir_opt_bitfield_unit_narrow(TCCIRState *ir)
{
  if (tcc_ir_opt_pass_disabled("bitfield_unit_narrow"))
    return 0;
  int changes = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
    changes += bfun_try_one(ir, i);
  return changes;
}
