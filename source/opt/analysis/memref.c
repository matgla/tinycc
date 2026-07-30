/*
 *  TCC IR - Memory reference model: canonical MemLoc + aliasing
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include <limits.h>

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "memref.h"

/* Resolve a pointer TEMP's def-chain to a (sym | frame) base + accumulated
 * offset.  `sym_out` NULL means a frame (StackLoc) base.  Returns 1 on success. */
static int memref_resolve_ptr(TCCIRState *ir, int32_t vr, int at_idx, struct Sym **sym_out, int64_t *off_out)
{
  int64_t acc = 0;
  int before = at_idx;
  for (int guard = 0; guard < 32; guard++)
  {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    int d = tcc_ir_find_defining_instruction(ir, vr, before);
    if (d < 0)
      return 0;
    IRQuadCompact *dq = &ir->compact_instructions[d];
    if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB && dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_LEA)
      return 0;
    IROperand s1 = tcc_ir_op_get_src1(ir, dq);
    if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, dq);
      if (!irop_is_immediate(s2) || s2.is_sym)
        return 0;
      int64_t c = irop_get_imm64_ex(ir, s2);
      acc += (dq->op == TCCIR_OP_SUB) ? -c : c;
    }
    /* Terminal: a global &sym+addend. */
    if (s1.is_sym && !s1.is_lval)
    {
      IRPoolSymref *sr = irop_get_symref_ex(ir, s1);
      if (!sr || !sr->sym)
        return 0;
      *sym_out = sr->sym;
      *off_out = (int64_t)sr->addend + acc;
      return 1;
    }
    /* Terminal: an own-frame StackLoc address. */
    if (irop_get_tag(s1) == IROP_TAG_STACKOFF && s1.is_local && !s1.is_lval && irop_get_vreg(s1) == -1)
    {
      *sym_out = NULL;
      *off_out = (int64_t)irop_get_stack_offset(s1) + acc;
      return 1;
    }
    /* Chain through another TEMP. */
    if (s1.is_lval || irop_get_vreg(s1) < 0)
      return 0;
    vr = irop_get_vreg(s1);
    before = d;
  }
  return 0;
}

MemLoc memloc_of(TCCIRState *ir, IROperand op, int at_idx)
{
  MemLoc m = {MEMLOC_NONE, NULL, 0, 0};
  int sz = ir_opt_store_btype_size_bytes(irop_get_btype(op));
  m.size = sz > 0 ? sz : 0;

  /* Direct global symref deref: &sym+addend. */
  if (op.is_sym && op.is_lval)
  {
    IRPoolSymref *sr = irop_get_symref_ex(ir, op);
    if (sr && sr->sym)
    {
      m.kind = MEMLOC_GLOBAL;
      m.sym = sr->sym;
      m.off = (int64_t)sr->addend;
      return m;
    }
    m.kind = MEMLOC_UNKNOWN;
    return m;
  }

  /* Direct own-frame StackLoc. */
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && irop_get_vreg(op) == -1)
  {
    m.kind = MEMLOC_FRAME;
    m.off = (int64_t)irop_get_stack_offset(op);
    return m;
  }

  /* Pointer deref through a TEMP: chase its def-chain. */
  int32_t vr = irop_get_vreg(op);
  if (op.is_lval && vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
  {
    struct Sym *sym = NULL;
    int64_t off = 0;
    if (memref_resolve_ptr(ir, vr, at_idx, &sym, &off))
    {
      if (sym)
      {
        m.kind = MEMLOC_GLOBAL;
        m.sym = sym;
        m.off = off;
      }
      else
      {
        m.kind = MEMLOC_FRAME;
        m.off = off;
      }
      return m;
    }
    m.kind = MEMLOC_UNKNOWN;
    return m;
  }

  /* Any other lval (VAR/PARAM deref, indexed, ...) is an unresolved pointer. */
  if (op.is_lval)
    m.kind = MEMLOC_UNKNOWN;
  return m;
}

MemLoc memloc_of_pointer(TCCIRState *ir, IROperand op, int at_idx)
{
  MemLoc m = {MEMLOC_NONE, NULL, 0, 0};

  /* A symref operand names the address of a global, whether or not is_lval. */
  if (op.is_sym)
  {
    IRPoolSymref *sr = irop_get_symref_ex(ir, op);
    if (sr && sr->sym)
    {
      m.kind = MEMLOC_GLOBAL;
      m.sym = sr->sym;
      m.off = (int64_t)sr->addend;
      return m;
    }
    m.kind = MEMLOC_UNKNOWN;
    return m;
  }

  /* A direct own-frame StackLoc address value. */
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && irop_get_vreg(op) == -1)
  {
    m.kind = MEMLOC_FRAME;
    m.off = (int64_t)irop_get_stack_offset(op);
    return m;
  }

  /* A TEMP holding a pointer: chase its def-chain. */
  int32_t vr = irop_get_vreg(op);
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
  {
    struct Sym *sym = NULL;
    int64_t off = 0;
    if (memref_resolve_ptr(ir, vr, at_idx, &sym, &off))
    {
      if (sym)
      {
        m.kind = MEMLOC_GLOBAL;
        m.sym = sym;
        m.off = off;
      }
      else
      {
        m.kind = MEMLOC_FRAME;
        m.off = off;
      }
      return m;
    }
  }

  m.kind = MEMLOC_UNKNOWN;
  return m;
}

/* Byte ranges [aoff, aoff+asz) and [boff, boff+bsz) overlap.  A 0 size is
 * treated as "at least one byte" so a same-base unknown-width access is not
 * wrongly proven disjoint. */
static int ranges_overlap(int64_t aoff, int asz, int64_t boff, int bsz)
{
  int64_t ae = aoff + (asz > 0 ? asz : 1);
  int64_t be = boff + (bsz > 0 ? bsz : 1);
  return aoff < be && boff < ae;
}

int mem_may_alias(MemLoc a, MemLoc b)
{
  if (a.kind == MEMLOC_NONE || b.kind == MEMLOC_NONE)
    return 0;
  if (a.kind == MEMLOC_UNKNOWN || b.kind == MEMLOC_UNKNOWN)
    return 1; /* an unresolved pointer may reach any global or escaped frame slot */
  if (a.kind != b.kind)
    return 0; /* a global and a frame slot are disjoint address spaces */
  if (a.kind == MEMLOC_GLOBAL)
  {
    if (a.sym != b.sym)
      return 0;
    return ranges_overlap(a.off, a.size, b.off, b.size);
  }
  /* both FRAME */
  return ranges_overlap(a.off, a.size, b.off, b.size);
}
