/*
 *  TCC IR - Copy-source load forwarding (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * After `x = G` (a copy of a global aggregate G into a non-escaping local x), a
 * later read of `x.field` yields exactly `G.field` — provided neither x nor G is
 * written between the copy and the read.  This pass rewrites such a read to name
 * `G.field` directly, so the value comes from the same place as an independent
 * `G.field` read in the same expression.  The existing BB-local load-CSE +
 * `fold_same_value` + `if(a||b||c)` diamond folders then collapse redundant
 * self-comparisons such as the `if (x.field != G.field) abort();` self-checks in
 * the 20040709-2 drivers.
 *
 * This only READS the local address (resolves `Addr[StackLoc](+/-#c)` chains to a
 * frame offset to look x up in the copy map) and forwards to the GLOBAL address;
 * it never constructs or rebuilds a StackLoc operand, so it does not touch the
 * abstract-slot-id hazard.
 *
 * A copy is recognized in three shapes, because the frontend emits all three:
 *   - field-wise `StackLoc[s] <- Tv` where Tv is a single-def LOAD of `G+off`;
 *   - a `mem{cpy,move}` CALL for anything wider than a register;
 *   - store-forwarded, `G.f <- Tv; StackLoc[s] <- Tv` — no LOAD exists at all,
 *     which is what every struct that fits in a register compiles to.
 *
 * A read is rewritten wherever it appears: a LOAD's source, or an inline memory
 * operand of any value-consuming op (`T <- StackLoc[s] SHL #k`, `CMP StackLoc[s],
 * G`).  Slots that hold an ADDRESS or an opaque id are excluded — see
 * csf_slot_is_value_read.
 *
 * Soundness gates (all conservative — bail on doubt):
 *   - The region from the copy to the read must be single-entry: every jump
 *     target inside it is reached only from inside it, and only forwards.
 *   - No write may touch the bits the read actually DEMANDS.  Both halves are
 *     precise: a store's range is its own byte range (not the whole object), and
 *     a bitfield read-modify-write `(G.word & keep) | rest` is known to leave the
 *     `keep` bits alone, so `x.i` still forwards across `s.k += a`.
 *   - Calls must provably write neither the global field nor the local copy
 *     (purity first, then `tcc_ir_call_may_write` mod-ref).
 *   - Volatile sources are never forwarded, and nested frames / static chains /
 *     variadic functions bail the whole pass.
 *
 * TWO profitability gates, both load-bearing (see each for the regression it
 * prevents): an independent read of the same field must already exist, and — for
 * the inline-operand form — the value must reach a comparison.
 */

#define USING_GLOBALS

#include <limits.h>

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "licm.h"
#include "memref.h"

extern int tcc_ir_opt_pass_disabled(const char *name);

#define CSF_MAX_FIELDS 64

/* TCC_CSF_DBG=1 traces every rejected forward and why — the fastest way to tell
 * "the copy map never saw it" from "the region was not clean". */
static int csf_dbg(void)
{
  static int cached = -1;
  if (cached < 0)
    cached = getenv("TCC_CSF_DBG") != NULL;
  return cached;
}
#define CSF_DBG(...)                                                                               \
  do                                                                                               \
  {                                                                                                \
    if (csf_dbg())                                                                                 \
      fprintf(stderr, "csfwd: " __VA_ARGS__);                                                      \
  } while (0)

/* Resolve an lval address operand (a load/store target) to an own-frame offset
 * via the shared memory-reference resolver.  Returns 1 iff it names a frame slot. */
static int csf_addr_frame_off(TCCIRState *ir, IROperand addr, int at_idx, int *out_off)
{
  MemLoc m = memloc_of(ir, addr, at_idx);
  if (m.kind == MEMLOC_FRAME && m.off >= INT_MIN && m.off <= INT_MAX)
  {
    *out_off = (int)m.off;
    return 1;
  }
  return 0;
}

/* Conservative superset of the bits `op` (evaluated at `at_idx`) can have set.
 * Only 32-bit-or-narrower values are modelled; anything else is "all bits". */
static uint32_t csf_ones_mask(TCCIRState *ir, IROperand op, int at_idx, int depth)
{
  int sz = ir_opt_store_btype_size_bytes(irop_get_btype(op));
  if (depth <= 0 || sz > 4)
    return 0xFFFFFFFFu;
  if (irop_is_immediate(op) && !op.is_sym)
    return (uint32_t)irop_get_imm64_ex(ir, op);
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_lval || op.is_sym || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0xFFFFFFFFu;
  int d = tcc_ir_find_defining_instruction(ir, vr, at_idx);
  if (d < 0)
    return 0xFFFFFFFFu;
  IRQuadCompact *dq = &ir->compact_instructions[d];
  IROperand s1 = tcc_ir_op_get_src1(ir, dq);
  IROperand s2;
  int k;
  switch (dq->op)
  {
  case TCCIR_OP_ASSIGN:
    return csf_ones_mask(ir, s1, d, depth - 1);
  case TCCIR_OP_AND:
    s2 = tcc_ir_op_get_src2(ir, dq);
    return csf_ones_mask(ir, s1, d, depth - 1) & csf_ones_mask(ir, s2, d, depth - 1);
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    s2 = tcc_ir_op_get_src2(ir, dq);
    return csf_ones_mask(ir, s1, d, depth - 1) | csf_ones_mask(ir, s2, d, depth - 1);
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
    s2 = tcc_ir_op_get_src2(ir, dq);
    if (!irop_is_immediate(s2) || s2.is_sym)
      return 0xFFFFFFFFu;
    k = (int)irop_get_imm64_ex(ir, s2);
    if (k < 0 || k > 31)
      return 0xFFFFFFFFu;
    return dq->op == TCCIR_OP_SHL ? (csf_ones_mask(ir, s1, d, depth - 1) << k)
                                  : (csf_ones_mask(ir, s1, d, depth - 1) >> k);
  default:
    return 0xFFFFFFFFu;
  }
}

/* Does `op` compute `<read of (sym, addend)> & imm`?  Returns the kept mask. */
static int csf_and_of_same_location(TCCIRState *ir, IROperand op, int at_idx,
                                    Sym *sym, int32_t addend, uint32_t *keep)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_lval || op.is_sym)
    return 0;
  int d = tcc_ir_find_defining_instruction(ir, vr, at_idx);
  if (d < 0)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[d];
  if (dq->op != TCCIR_OP_AND)
    return 0;
  IROperand s1 = tcc_ir_op_get_src1(ir, dq);
  IROperand s2 = tcc_ir_op_get_src2(ir, dq);
  IROperand mem, imm;
  if (irop_is_immediate(s2) && !s2.is_sym)
  {
    mem = s1;
    imm = s2;
  }
  else if (irop_is_immediate(s1) && !s1.is_sym)
  {
    mem = s2;
    imm = s1;
  }
  else
    return 0;
  /* The AND's other input must be the location's own value: either read inline
   * or through a LOAD temp.  Record where that read happens so the caller can
   * check nothing rewrote the location in between. */
  int read_at = d;
  MemLoc m = memloc_of(ir, mem, d);
  if (m.kind != MEMLOC_GLOBAL || m.sym != sym || m.off != addend)
  {
    int32_t mv = irop_get_vreg(mem);
    if (mv < 0 || mem.is_lval || mem.is_sym)
      return 0;
    int md = tcc_ir_find_defining_instruction(ir, mv, d);
    if (md < 0 || ir->compact_instructions[md].op != TCCIR_OP_LOAD)
      return 0;
    MemLoc lm = memloc_of(ir, tcc_ir_op_get_src1(ir, &ir->compact_instructions[md]), md);
    if (lm.kind != MEMLOC_GLOBAL || lm.sym != sym || lm.off != addend)
      return 0;
    read_at = md;
  }
  /* Nothing between the read and the store may write memory, or the "kept" half
   * would be a stale value and the store WOULD change those bits. */
  for (int j = read_at + 1; j < at_idx; j++)
  {
    IRQuadCompact *jq = &ir->compact_instructions[j];
    if (jq->op == TCCIR_OP_STORE || jq->op == TCCIR_OP_STORE_INDEXED ||
        jq->op == TCCIR_OP_STORE_POSTINC || jq->op == TCCIR_OP_BLOCK_COPY ||
        jq->op == TCCIR_OP_FUNCCALLVAL || jq->op == TCCIR_OP_FUNCCALLVOID ||
        jq->op == TCCIR_OP_INLINE_ASM)
      return 0;
  }
  *keep = (uint32_t)irop_get_imm64_ex(ir, imm);
  return 1;
}

/* Bits the STORE at `idx` to (sym, saddend) can CHANGE.  A bitfield update is a
 * read-modify-write, `(G.word & keep) | rest`, which leaves every other field of
 * the word alone — that is exactly what lets `x.i`/`x.j` still forward across
 * the drivers' `s.k += a`.  0xFFFFFFFF means "assume it changes everything". */
static uint32_t csf_store_changed_mask(TCCIRState *ir, int idx, Sym *sym, int32_t saddend)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand val = tcc_ir_op_get_src1(ir, q);
  if (ir_opt_store_btype_size_bytes(irop_get_btype(val)) > 4)
    return 0xFFFFFFFFu;
  int32_t vr = irop_get_vreg(val);
  if (vr < 0 || val.is_lval || val.is_sym)
    return 0xFFFFFFFFu;
  int d = tcc_ir_find_defining_instruction(ir, vr, idx);
  if (d < 0)
    return 0xFFFFFFFFu;
  IRQuadCompact *dq = &ir->compact_instructions[d];
  if (dq->op != TCCIR_OP_OR)
    return 0xFFFFFFFFu;
  IROperand sides[2] = {tcc_ir_op_get_src1(ir, dq), tcc_ir_op_get_src2(ir, dq)};
  for (int s = 0; s < 2; s++)
  {
    uint32_t keep;
    if (!csf_and_of_same_location(ir, sides[s], d, sym, saddend, &keep))
      continue;
    return ~keep | csf_ones_mask(ir, sides[1 - s], d, 6);
  }
  return 0xFFFFFFFFu;
}

/* ---- demanded bits of a memory read ------------------------------------- *
 * A bitfield read is a whole-word (or whole-doubleword) memory access whose
 * value is immediately narrowed by a shift pair or a mask.  Only the bits that
 * survive that narrowing matter, so an intervening store that rewrites OTHER
 * bits of the same word cannot disturb the forward.  This is what lets `x.i`
 * and `x.j` still forward across the drivers' `s.k += a`, where the byte ranges
 * do overlap.  Masks are 64-bit, relative to the read's own base. */

#define CSF_ALL64 (~(uint64_t)0)

static uint64_t csf_width_mask(int bytes)
{
  if (bytes <= 0 || bytes >= 8)
    return CSF_ALL64;
  return ((uint64_t)1 << (bytes * 8)) - 1;
}

static uint64_t csf_op_width_mask(IROperand op)
{
  return csf_width_mask(ir_opt_store_btype_size_bytes(irop_get_btype(op)));
}

static uint64_t csf_demanded_of_result(TCCIRState *ir, int idx, int depth, int *budget);

/* Bits of source slot `slot` of instruction `idx` that can affect anything. */
static uint64_t csf_demanded_of_input(TCCIRState *ir, int idx, int slot, int depth, int *budget)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand in = slot ? tcc_ir_op_get_src2(ir, q) : tcc_ir_op_get_src1(ir, q);
  uint64_t in_mask = csf_op_width_mask(in);
  if (depth <= 0 || --*budget < 0)
    return in_mask;

  /* A LOAD's memory operand is demanded exactly as much as the loaded value. */
  if (q->op == TCCIR_OP_LOAD)
    return csf_demanded_of_result(ir, idx, depth, budget) & in_mask;

  if (!irop_config[q->op].has_dest)
    return in_mask; /* CMP / STORE / param: assume every bit matters */

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  uint64_t d = csf_demanded_of_result(ir, idx, depth, budget) & csf_op_width_mask(dest);
  IROperand other = slot ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
  int64_t k;
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_ZEXT:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    return d & in_mask;
  case TCCIR_OP_AND:
    if (irop_config[q->op].has_src2 && irop_is_immediate(other) && !other.is_sym)
      return d & (uint64_t)irop_get_imm64_ex(ir, other) & in_mask;
    return d & in_mask;
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
    if (slot != 0 || !irop_is_immediate(other) || other.is_sym)
      return in_mask;
    k = irop_get_imm64_ex(ir, other);
    if (k < 0 || k > 63)
      return in_mask;
    return (q->op == TCCIR_OP_SHL ? (d >> k) : (d << k)) & in_mask;
  default:
    return in_mask;
  }
}

/* Bits of instruction `idx`'s destination that any consumer can observe. */
static uint64_t csf_demanded_of_result(TCCIRState *ir, int idx, int depth, int *budget)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (depth <= 0 || --*budget < 0 || !irop_config[q->op].has_dest)
    return CSF_ALL64;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t vr = irop_get_vreg(dest);
  if (vr < 0 || dest.is_lval || dest.is_sym ||
      TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return CSF_ALL64;
  /* A temp redefined later would need per-use reaching-def reasoning. */
  if (tcc_ir_find_defining_instruction(ir, vr, idx) >= 0)
    return CSF_ALL64;
  int n = ir->next_instruction_index;
  /* Missing ONE use under-approximates the demand and would let a clobbering
   * store through, so anything the forward scan below cannot classify has to
   * mean "all bits".  A use BEFORE the def is one such case: it can only be
   * reached around a back edge, which this linear scan does not model. */
  for (int j = 0; j < idx; j++)
  {
    IRQuadCompact *uq = &ir->compact_instructions[j];
    if (uq->op == TCCIR_OP_NOP)
      continue;
    for (int s = 0; s < 2; s++)
    {
      if (s == 0 && !irop_config[uq->op].has_src1)
        continue;
      if (s == 1 && !irop_config[uq->op].has_src2)
        continue;
      if (irop_get_vreg(s ? tcc_ir_op_get_src2(ir, uq) : tcc_ir_op_get_src1(ir, uq)) == vr)
        return CSF_ALL64;
    }
  }
  uint64_t acc = 0;
  for (int j = idx + 1; j < n; j++)
  {
    IRQuadCompact *uq = &ir->compact_instructions[j];
    if (uq->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[uq->op].has_dest && irop_get_vreg(tcc_ir_op_get_dest(ir, uq)) == vr)
    {
      /* An lval dest is a store THROUGH the temp — a use of every bit. */
      if (tcc_ir_op_get_dest(ir, uq).is_lval)
        return CSF_ALL64;
      break; /* redefined: later uses read a different value */
    }
    /* MLA's accumulator and SELECT's condition live outside src1/src2. */
    if ((uq->op == TCCIR_OP_MLA && irop_get_vreg(tcc_ir_op_get_accum(ir, uq)) == vr) ||
        (uq->op == TCCIR_OP_SELECT && irop_get_vreg(tcc_ir_op_get_cond(ir, uq)) == vr))
      return CSF_ALL64;
    for (int s = 0; s < 2; s++)
    {
      if (s == 0 && !irop_config[uq->op].has_src1)
        continue;
      if (s == 1 && !irop_config[uq->op].has_src2)
        continue;
      IROperand u = s ? tcc_ir_op_get_src2(ir, uq) : tcc_ir_op_get_src1(ir, uq);
      if (irop_get_vreg(u) != vr)
        continue;
      /* An MLA/SELECT-style extra operand or a barrel-shifted use is not
       * modelled here, so fall back to "everything matters". */
      if (s == 1 && tcc_ir_barrel_shift_at(ir, uq))
        return CSF_ALL64;
      acc |= csf_demanded_of_input(ir, j, s, depth - 1, budget);
    }
    if (acc == CSF_ALL64)
      return acc;
  }
  return acc;
}

/* Classify a store's destination against the exact byte range the forward reads:
 * the global range [gaddend, gaddend+width) of `gsym` and the local range
 * [lbase, lbase+width).  Returns 1 if it writes the global range, 2 if it writes
 * the local range, 0 if provably disjoint from both, -1 if unresolvable.
 *
 * Ranges, not whole objects: a driver's `s.k += x` writes one field of the same
 * global the copy came from, which must not block forwarding the OTHER fields.
 * `demand` is the set of read bits that actually matter, relative to gaddend. */
static int csf_store_effect(TCCIRState *ir, int idx, Sym *gsym, int32_t gaddend,
                            int lbase, int width, uint64_t demand)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int wbytes = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
  if (wbytes <= 0)
    wbytes = 1;

  /* STORE_INDEXED writes base + (index << scale) with an unknown index, so the
   * base alone does not bound the written range: only a different global is
   * provably disjoint. */
  if (q->op == TCCIR_OP_STORE_INDEXED)
  {
    IROperand base = tcc_ir_op_get_dest(ir, q);
    if (base.is_sym)
    {
      IRPoolSymref *bref = irop_get_symref_ex(ir, base);
      if (bref && bref->sym)
        return (bref->sym == gsym) ? 1 : 0;
    }
    return -1; /* indexed via a pointer/unknown base */
  }

  /* Global symref destination. */
  if (dest.is_sym && dest.is_lval)
  {
    IRPoolSymref *dref = irop_get_symref_ex(ir, dest);
    if (!dref || !dref->sym)
      return -1;
    if (dref->sym != gsym)
      return 0; /* a different global cannot alias G or a stack local */
    if (dref->addend + wbytes <= gaddend || dref->addend >= gaddend + width)
      return 0; /* same global, disjoint field */
    /* The byte ranges overlap, but a bitfield update only rewrites part of the
     * word.  Move the store's changed-bit mask into the read's coordinates
     * (little-endian) and let it through if it touches no demanded bit. */
    if (wbytes <= 4)
    {
      uint64_t changed = csf_store_changed_mask(ir, idx, gsym, dref->addend);
      int64_t shift = ((int64_t)dref->addend - (int64_t)gaddend) * 8;
      if (shift >= 64 || shift <= -64)
        changed = 0;
      else if (shift >= 0)
        changed <<= shift;
      else
        changed >>= -shift;
      if ((changed & demand) == 0)
        return 0;
    }
    return 1;
  }

  /* A direct write to a scalar local — `V6 <- V3 [STORE]`, dest is_local with a
   * VAR/PARAM vreg — touches only that variable's own storage, which is a
   * distinct C object from the global G and from the copied aggregate x.  (A
   * store THROUGH a pointer is a TEMP deref and is not is_local; VT_LLOCAL is
   * a double indirection and is excluded.)  Without this, the fn3 driver bodies
   * bail on their own inlined parameter copy. */
  if (dest.is_local && !dest.is_sym && !dest.is_llocal)
  {
    int32_t dvr = irop_get_vreg(dest);
    if (dvr >= 0)
    {
      int vt = TCCIR_DECODE_VREG_TYPE(dvr);
      if (vt == TCCIR_VREG_TYPE_VAR || vt == TCCIR_VREG_TYPE_PARAM)
        return 0;
    }
  }

  /* Local frame destination (direct StackLoc or resolvable Addr[StackLoc]+c). */
  int off;
  if (csf_addr_frame_off(ir, dest, idx, &off))
  {
    if (off + wbytes <= lbase || off >= lbase + width)
      return 0; /* disjoint local */
    return 2;   /* overlaps the copied range being forwarded */
  }
  return -1; /* opaque pointer store */
}

/* Is source slot `slot` (0 = src1, 1 = src2) of `op` a plain VALUE read, so that
 * an lvalue operand there means "the contents of this place"?  Everything that
 * treats a source as an ADDRESS (LEA, the indexed/post-inc bases, block copy),
 * as an opaque id (call ids, table ids, bitfield params) or as a jump/asm
 * artifact is excluded: rewriting those changes what is addressed, not what is
 * read.  LOAD is handled by the caller — its src1 is the place it reads. */
static int csf_slot_is_value_read(int op, int slot)
{
  switch (op)
  {
  /* address / opaque operands */
  case TCCIR_OP_LEA:
  case TCCIR_OP_LOAD:
  case TCCIR_OP_LOAD_INDEXED:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_LOAD_POSTINC:
  case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
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
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SWITCH_LOAD:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_INIT_CHAIN_SLOT:
  case TCCIR_OP_CALLSEQ_BEGIN:
  case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_CALLARG_REG:
  case TCCIR_OP_CALLARG_STACK:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_IJUMP:
    return 0;
  /* src2 carries the call id, not a value */
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_FUNCPARAMVAL:
  /* src2 carries the (lsb, width) field descriptor */
  case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_BFI:
    return slot == 0;
  default:
    return 1;
  }
}

/* An aligned/unaligned AEABI or libc block-copy helper: `f(dst, src, n)`.
 * These all copy n bytes verbatim, which is what makes a byte-offset forward
 * from the destination to the source valid. */
static int csf_is_block_copy_helper(const char *nm)
{
  if (!nm)
    return 0;
  return !strcmp(nm, "memcpy") || !strcmp(nm, "memmove") ||
         !strcmp(nm, "__aeabi_memcpy") || !strcmp(nm, "__aeabi_memmove") ||
         !strcmp(nm, "__aeabi_memcpy4") || !strcmp(nm, "__aeabi_memmove4") ||
         !strcmp(nm, "__aeabi_memcpy8") || !strcmp(nm, "__aeabi_memmove8");
}

/* Largest whole-aggregate copy admitted into the map, in bytes. */
#define CSF_MAX_BLOCK_COPY 64

/* PROFITABILITY GATE.  Redirecting `x.f` to `G.f` replaces a cheap frame load
 * with a global one (literal-pool address + load) and lengthens the value's live
 * range.  That only pays back if the redirected load then CSEs / folds against an
 * independent read of the SAME field — which is the whole point in
 * `if (x.f != G.f)`, but is a pure loss anywhere else.  Ungated this costs 111
 * corpus regressions (20041011-1::t1..t11 alone are +40..+58 each).
 * So require that some other instruction already reads exactly this field. */
static int csf_global_field_read_exists(TCCIRState *ir, Sym *sym, int32_t addend,
                                        int btype, int exclude_idx, int exclude_idx2)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    if (i == exclude_idx || i == exclude_idx2)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int slot = 0; slot < 2; slot++)
    {
      if (slot == 0 && !irop_config[q->op].has_src1)
        continue;
      if (slot == 1 && !irop_config[q->op].has_src2)
        continue;
      IROperand op = slot ? tcc_ir_op_get_src2(ir, q) : tcc_ir_op_get_src1(ir, q);
      if (!op.is_sym || !op.is_lval || irop_get_btype(op) != btype)
        continue;
      IRPoolSymref *r = irop_get_symref_ex(ir, op);
      if (r && r->sym == sym && r->addend == addend)
        return 1;
    }
  }
  return 0;
}

/* SECOND PROFITABILITY GATE, for reads that are not already plain LOADs.
 * Widening the rewrite to inline memory operands (`T <- StackLoc[s] SHL #k`,
 * `CMP StackLoc[s], G`) reaches the bitfield read-modify-write idiom too, where
 * redirecting the read splits one global load into two and destroys the BFI
 * collapse (ir/174_bitfield_extract_fold::main, +11).  The forward only pays
 * when the exposed value ends up in a comparison that then folds against the
 * matching independent read, so require exactly that: the value must reach a
 * CMP/TEST_ZERO within a few pure steps. */
static int csf_reaches_compare(TCCIRState *ir, int idx, int depth, int *budget)
{
  if (depth <= 0 || --*budget < 0)
    return 0;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_TEST_ZERO || q->op == TCCIR_OP_FCMP)
    return 1;
  if (!irop_config[q->op].has_dest)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  int32_t vr = irop_get_vreg(d);
  if (vr < 0 || d.is_lval || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  int n = ir->next_instruction_index;
  for (int j = idx + 1; j < n; j++)
  {
    IRQuadCompact *uq = &ir->compact_instructions[j];
    if (uq->op == TCCIR_OP_NOP)
      continue;
    int uses = (irop_config[uq->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, uq)) == vr) ||
               (irop_config[uq->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, uq)) == vr);
    if (uses && csf_reaches_compare(ir, j, depth - 1, budget))
      return 1;
  }
  return 0;
}

/* A copy the frontend established by STORE-FORWARDING rather than by reloading:
 *
 *     G.f = v;      ->  G.f <- Tv  [STORE]
 *     x   = G;      ->  Tw  <- Tv  [ASSIGN];  x <- Tw [STORE]
 *
 * x still holds G.f's value, but there is no LOAD to recognize.  Every driver
 * whose struct fits in a register (the 2- and 4-byte ones) takes this path, so
 * without it they never get a copy map at all.  On success `*out_addr` is the
 * global store's own destination operand — the right thing to clone flags from —
 * and `*out_src` the index to exclude from the profitability gate. */
static int csf_store_forwarded_source(TCCIRState *ir, int copy_idx, int32_t vvr,
                                      IROperand *out_addr, int *out_src)
{
  /* Chase ASSIGN copies back to the root value. */
  int32_t chain[8];
  int nchain = 0;
  int32_t cv = vvr;
  int cat = copy_idx;
  while (nchain < 8 && cv >= 0)
  {
    chain[nchain++] = cv;
    int cd = tcc_ir_find_defining_instruction(ir, cv, cat);
    if (cd < 0)
      break;
    IRQuadCompact *cq = &ir->compact_instructions[cd];
    if (cq->op != TCCIR_OP_ASSIGN)
      break;
    IROperand cs = tcc_ir_op_get_src1(ir, cq);
    if (cs.is_lval || cs.is_sym)
      break;
    cv = irop_get_vreg(cs);
    cat = cd;
  }

  for (int j = copy_idx - 1; j >= 0; j--)
  {
    IRQuadCompact *jq = &ir->compact_instructions[j];
    if (jq->op != TCCIR_OP_STORE)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, jq);
    if (!d.is_sym || !d.is_lval)
      continue;
    IRPoolSymref *r = irop_get_symref_ex(ir, d);
    if (!r || !r->sym || (r->sym->type.t & VT_VOLATILE))
      return 0;
    int32_t sv = irop_get_vreg(tcc_ir_op_get_src1(ir, jq));
    int match = 0;
    for (int c = 0; c < nchain; c++)
      if (chain[c] == sv)
        match = 1;
    if (!match)
      return 0; /* the nearest global store wrote something else */
    /* Nothing between the store and the copy may write memory or call. */
    for (int k = j + 1; k < copy_idx; k++)
    {
      TccIrOp op = ir->compact_instructions[k].op;
      if (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED ||
          op == TCCIR_OP_STORE_POSTINC || op == TCCIR_OP_BLOCK_COPY ||
          op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_FUNCCALLVAL ||
          op == TCCIR_OP_FUNCCALLVOID)
        return 0;
    }
    *out_addr = d;
    *out_src = j;
    return 1;
  }
  return 0;
}

int tcc_ir_opt_copy_source_load_fwd(TCCIRState *ir)
{
  if (tcc_ir_opt_pass_disabled("copy_source_load_fwd"))
    return 0;

  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (ir->captured_count > 0 || ir->has_static_chain)
    return 0;
  if (ir->is_variadic)
    return 0;

  /* Record, per instruction, the lowest and highest index that jumps to it.  A
   * copy->use region stays a single forward path as long as every jump target
   * inside it is reached ONLY from inside it and only by a forward jump — which
   * is what makes the degenerate `JMP to <next>` markers the frontend leaves
   * around inlined bodies harmless (they are not real joins).  A target with an
   * outside or backward predecessor is a genuine join and forbids forwarding. */
  int *tgt_min = tcc_malloc(sizeof(int) * n);
  int *tgt_max = tcc_malloc(sizeof(int) * n);
  for (int i = 0; i < n; i++)
  {
    tgt_min[i] = INT_MAX;
    tgt_max[i] = INT_MIN;
  }
  int has_ijump = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP)
      has_ijump = 1;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      if (t >= 0 && t < n)
      {
        if (i < tgt_min[t])
          tgt_min[t] = i;
        if (i > tgt_max[t])
          tgt_max[t] = i;
      }
    }
    else if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      has_ijump = 1; /* switch targets not modeled here; bail whole function */
    }
  }
  if (has_ijump)
  {
    tcc_free(tgt_min);
    tcc_free(tgt_max);
    return 0;
  }

  /* Copy map: StackLoc[off] holds a copy of global (sym, addend), `width` bytes,
   * established by the store at `store_idx`. */
  struct
  {
    int base_off;
    int width;
    Sym *sym;
    int32_t addend;
    int store_idx;
    /* The copy source's own address operand, kept so the rewrite can CLONE its
     * flags.  ir_opt_pure_expr_equal compares ALL operand flags, so a
     * hand-built symref with different is_const/is_local/aux never compares
     * equal to the frontend's `G.field` deref — and then the self-comparison
     * this pass exists to expose never folds. */
    IROperand src_addr;
    /* Index of the LOAD that feeds this copy, or -1.  It reads the very field we
     * are forwarding to, so it must NOT count as the independent read the
     * profitability gate looks for — otherwise every field-wise copy looks
     * foldable and fn2J/fn2V-style helpers (no self-compare at all) regress. */
    int src_load_idx;
  } fields[CSF_MAX_FIELDS];
  int nfields = 0;

  for (int i = 0; i < n && nfields < CSF_MAX_FIELDS; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* A whole-aggregate `x = G` is emitted as a block-copy CALL, not as
     * field-wise load/store pairs, for anything the frontend does not expand
     * inline — which is every struct in the 20040709-2 drivers except the
     * 2-byte one.  Record the copy as a single wide field so loads anywhere
     * inside it forward to the matching offset of the source global. */
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      Sym *cs = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      IROperand p0, p1, p2;
      if (cs && csf_is_block_copy_helper(get_tok_str(cs->v, NULL)) &&
          ir_opt_get_call_param_operand(ir, i, 0, &p0) &&
          ir_opt_get_call_param_operand(ir, i, 1, &p1) &&
          ir_opt_get_call_param_operand(ir, i, 2, &p2) &&
          irop_is_immediate(p2))
      {
        int64_t sz = irop_get_imm64_ex(ir, p2);
        MemLoc dl = memloc_of_pointer(ir, p0, i);
        MemLoc sl = memloc_of_pointer(ir, p1, i);
        if (sz > 0 && sz <= CSF_MAX_BLOCK_COPY && dl.kind == MEMLOC_FRAME &&
            sl.kind == MEMLOC_GLOBAL && sl.sym && !(sl.sym->type.t & VT_VOLATILE))
        {
          fields[nfields].base_off = (int)dl.off;
          fields[nfields].width = (int)sz;
          fields[nfields].sym = sl.sym;
          fields[nfields].addend = (int32_t)sl.off;
          fields[nfields].store_idx = i;
          /* p1 is `&G` (a non-lval symref); the forward reads THROUGH it. */
          fields[nfields].src_addr = p1;
          fields[nfields].src_addr.is_lval = 1;
          fields[nfields].src_load_idx = -1;
          nfields++;
        }
      }
      continue;
    }

    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand val = tcc_ir_op_get_src1(ir, q);
    /* value must be a plain single-def TEMP loaded from a global. */
    int32_t vvr = irop_get_vreg(val);
    if (vvr < 0 || val.is_lval || val.is_sym || TCCIR_DECODE_VREG_TYPE(vvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int vdef = tcc_ir_find_defining_instruction(ir, vvr, i);
    if (vdef < 0)
      continue;
    /* single def: no earlier def of the same vreg. */
    if (tcc_ir_find_defining_instruction(ir, vvr, vdef) >= 0)
      continue;
    IRQuadCompact *dq = &ir->compact_instructions[vdef];
    IROperand lsrc;
    int src_load = vdef;
    if (dq->op == TCCIR_OP_LOAD)
      lsrc = tcc_ir_op_get_src1(ir, dq);
    else if (!csf_store_forwarded_source(ir, i, vvr, &lsrc, &src_load))
      continue;
    if (!lsrc.is_sym || !lsrc.is_lval)
      continue;
    IRPoolSymref *sref = irop_get_symref_ex(ir, lsrc);
    if (!sref || !sref->sym)
      continue;
    /* A volatile source must be re-read exactly where the program says: the
     * copy holds the value ONE read produced, and redirecting a later use to the
     * global would both add an observable access and read a different value. */
    if (sref->sym->type.t & VT_VOLATILE)
      continue;
    /* destination must be a concrete local frame slot. */
    int doff;
    if (!csf_addr_frame_off(ir, dest, i, &doff))
      continue;
    int w = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
    if (w <= 0)
      continue;
    /* The store value load width must match the store width (a real field copy). */
    if (ir_opt_store_btype_size_bytes(irop_get_btype(lsrc)) != w)
      continue;
    fields[nfields].base_off = doff;
    fields[nfields].width = w;
    fields[nfields].sym = sref->sym;
    fields[nfields].addend = sref->addend;
    fields[nfields].store_idx = i;
    fields[nfields].src_addr = lsrc;
    fields[nfields].src_load_idx = src_load;
    nfields++;
  }

  if (nfields == 0)
  {
    tcc_free(tgt_min);
    tcc_free(tgt_max);
    return 0;
  }

  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int slot = 0; slot < 2; slot++)
    {
      if (slot == 0 && !irop_config[q->op].has_src1)
        continue;
      if (slot == 1 && !irop_config[q->op].has_src2)
        continue;
      if (q->op == TCCIR_OP_LOAD)
      {
        if (slot != 0)
          continue; /* LOAD has no src2 anyway */
      }
      else if (!csf_slot_is_value_read(q->op, slot))
        continue;
      /* A barrel-shift annotation makes the real RHS `src2 SHIFT #k`; the
       * shift rides on a register operand, so leave such a slot alone. */
      if (slot == 1 && tcc_ir_barrel_shift_at(ir, q))
        continue;

      IROperand addr = slot ? tcc_ir_op_get_src2(ir, q) : tcc_ir_op_get_src1(ir, q);
      /* Only forward a local memory read; skip reads already from a global and
       * anything that is not an lvalue (an immediate, an address, a register).
       * LOAD keeps its historical shape: src1 IS the place it reads. */
      if (addr.is_sym)
        continue;
      if (q->op != TCCIR_OP_LOAD && !addr.is_lval)
        continue;
      int loff;
      if (!csf_addr_frame_off(ir, addr, i, &loff))
        continue;
      int lw = ir_opt_store_btype_size_bytes(irop_get_btype(addr));
      if (lw <= 0 && q->op == TCCIR_OP_LOAD)
        lw = ir_opt_store_btype_size_bytes(irop_get_btype(tcc_ir_op_get_dest(ir, q)));
      if (lw <= 0)
        continue;

      /* Find the copied field covering [loff, loff+lw).  Take the LATEST such
       * copy before the read, not the first: a driver that re-copies `x = G` per
       * check has several entries for the same slot, and anchoring on the
       * earliest makes the clean region span every later copy — whose own stores
       * then clobber it.  The newest copy is both valid and the shortest region
       * to prove clean. */
      int fi = -1;
      for (int f = 0; f < nfields; f++)
      {
        if (fields[f].store_idx >= i)
          continue;
        if (loff >= fields[f].base_off && loff + lw <= fields[f].base_off + fields[f].width)
        {
          if (fi < 0 || fields[f].store_idx > fields[fi].store_idx)
            fi = f;
        }
      }
      if (fi < 0)
      {
        CSF_DBG("i=%d slot=%d: no copy covers [%d,%d)\n", i, slot, loff, loff + lw);
        continue;
      }

      int cstore = fields[fi].store_idx;
      /* The exact bytes this read forwards to — the clean-region test uses this
       * range, not the whole copy, so a write to a DIFFERENT field of the same
       * global (the drivers' `s.k += x`) does not block it. */
      int32_t new_addend = fields[fi].addend + (loff - fields[fi].base_off);
      /* Bits of the read that any consumer can observe (relative to its base). */
      int dm_budget = 256;
      uint64_t demand = csf_demanded_of_input(ir, i, slot, 8, &dm_budget) & csf_width_mask(lw);

      /* Region (cstore, i] must be a clean single forward path. */
      int clean = 1;
      int dirty_at = -1;
      for (int j = cstore + 1; j <= i && clean; j++)
      {
        dirty_at = j;
        /* A jump target reached only from inside the region, and only forwards,
         * keeps the region single-entry: execution still gets here by falling
         * out of `cstore`.  Anything else is a real join. */
        if (tgt_min[j] != INT_MAX && (tgt_min[j] <= cstore || tgt_max[j] >= j))
        {
          clean = 0;
          break;
        }
        if (j == i)
          break; /* the read itself: only its join status matters */
        IRQuadCompact *jq = &ir->compact_instructions[j];
        switch (jq->op)
        {
        case TCCIR_OP_NOP:
        case TCCIR_OP_JUMP:
        case TCCIR_OP_JUMPIF:
        case TCCIR_OP_LOAD:
        case TCCIR_OP_LOAD_INDEXED:
        case TCCIR_OP_CMP:
        case TCCIR_OP_TEST_ZERO:
          break;
        case TCCIR_OP_STORE:
        case TCCIR_OP_STORE_INDEXED:
        {
          int eff = csf_store_effect(ir, j, fields[fi].sym, new_addend, loff, lw, demand);
          if (eff != 0)
            clean = 0;
          break;
        }
        case TCCIR_OP_FUNCCALLVOID:
        case TCCIR_OP_FUNCCALLVAL:
        {
          Sym *cs = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, jq));
          int p = cs ? tcc_ir_get_func_purity(ir, cs) : TCC_FUNC_PURITY_IMPURE;
          if (p == TCC_FUNC_PURITY_PURE || p == TCC_FUNC_PURITY_CONST)
            break;
          /* Not pure — but purity is all-or-nothing, and the self-check drivers
           * call helpers that write only their OWN frame.  Fall back to mod-ref:
           * the forward stays valid if the call provably writes neither the
           * source global's field nor the local copy.  Both queries are needed;
           * the frame one is what rules out the callee scribbling through a
           * pointer to x. */
          MemLoc gl = {0};
          gl.kind = MEMLOC_GLOBAL;
          gl.sym = fields[fi].sym;
          gl.off = new_addend;
          gl.size = lw;
          MemLoc fl = {0};
          fl.kind = MEMLOC_FRAME;
          fl.off = loff;
          fl.size = lw;
          if (tcc_ir_call_may_write(ir, j, gl) || tcc_ir_call_may_write(ir, j, fl))
            clean = 0;
          break;
        }
        default:
          /* any op that could write memory or is opaque: be safe. */
          if (irop_config[jq->op].has_dest)
          {
            IROperand d = tcc_ir_op_get_dest(ir, jq);
            if (d.is_lval)
            {
              clean = 0;
            }
          }
          /* opaque side-effecting ops. */
          if (jq->op == TCCIR_OP_BLOCK_COPY || jq->op == TCCIR_OP_INLINE_ASM ||
              jq->op == TCCIR_OP_STORE_POSTINC || jq->op == TCCIR_OP_LOAD_POSTINC ||
              jq->op == TCCIR_OP_VLA_ALLOC)
            clean = 0;
          break;
        }
      }
      if (!clean)
      {
        CSF_DBG("i=%d slot=%d: region (%d,%d] not clean at %d (op %d)\n", i, slot, cstore, i,
                dirty_at, dirty_at >= 0 ? (int)ir->compact_instructions[dirty_at].op : -1);
        continue;
      }

      /* Rewrite: read from G + (addend + (loff - base_off)), same width, as a
       * global deref. */
      if (!csf_global_field_read_exists(ir, fields[fi].sym, new_addend,
                                        irop_get_btype(addr), i,
                                        fields[fi].src_load_idx))
      {
        CSF_DBG("i=%d slot=%d: no independent read of +%d (btype %d)\n", i, slot, new_addend,
                irop_get_btype(addr));
        continue;
      }
      if (q->op != TCCIR_OP_LOAD)
      {
        int budget = 128;
        if (!csf_reaches_compare(ir, i, 6, &budget))
        {
          CSF_DBG("i=%d slot=%d: value never reaches a compare\n", i, slot);
          continue;
        }
      }
      /* Carry the copy source's POOL flags, not just the operand's.
       * ir_opt_nonvreg_expr_equal compares IRPoolSymref::flags too, so a pool
       * entry built with 0 never compares equal to the frontend's own `G.field`
       * deref (which carries IRPOOL_SYMREF_LVAL) — and the self-comparison this
       * pass exists to expose then never folds.  The rewritten operand IS an lval
       * deref, so LVAL must be set regardless of how the source was spelled (the
       * block-copy source is `&G`, a non-lval pointer argument). */
      IRPoolSymref *src_ref = irop_get_symref_ex(ir, fields[fi].src_addr);
      uint32_t sym_flags = (src_ref ? src_ref->flags : 0u) | IRPOOL_SYMREF_LVAL;
      uint32_t pool_idx = tcc_ir_pool_add_symref(ir, fields[fi].sym, new_addend, sym_flags);
      /* Clone the copy source's operand and override only what this access
       * changes, so every other flag matches an independent `G.field` deref. */
      IROperand newop = fields[fi].src_addr;
      newop.tag = IROP_TAG_SYMREF;
      newop.is_sym = 1;
      newop.is_lval = 1;
      newop.u.pool_idx = pool_idx;
      newop.btype = irop_get_btype(addr);
      newop.is_unsigned = addr.is_unsigned;
      if (slot)
        tcc_ir_set_src2(ir, i, newop);
      else
        tcc_ir_set_src1(ir, i, newop);
      changes++;
    }
  }

  tcc_free(tgt_min);
  tcc_free(tgt_max);
  return changes;
}
