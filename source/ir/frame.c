/*
 *  TCC IR - Frame relayout
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * The frontend gives every stack object (aggregates, address-taken temporaries,
 * call and return slots) its own frame slot at the point it is declared, and
 * that layout was final.  After optimization many of those objects are no
 * longer referenced at all -- a copy forwarded away, a temporary whose only
 * reader was folded -- yet they still size the frame, and every live object
 * below them pays for the distance: offsets past 1020 no longer fit the 16-bit
 * LDR/STR [sp] forms, and past 4095 need an extra instruction.  Dropping them
 * shrinks the summed frames of the Zig compiler, built as C, by 16%.
 *
 * tcc_ir_frame_relayout removes them.  It runs just before the linear scan, once
 * every pass that adds or drops a frame reference is done:
 *
 *   - The frame region [loc, 0) is cut into SEGMENTS: every object the frontend
 *     allocated through tcc_ir_frame_alloc, with its alignment padding, plus
 *     the gaps between them.  A gap is an allocation made directly on `loc` --
 *     parameter homes the prologue stores to, setjmp and __label__ buffers,
 *     VLA pointer slots -- and is PINNED: never dropped, never moved.
 *   - A segment is live if any stack operand points into it.  An address one
 *     past an object's end lands in its padding or, without padding, keeps the
 *     NEXT segment alive.
 *   - Consecutive live segments form a RUN that moves as one block, so an
 *     address computed across segments inside it stays valid; dead segments
 *     between runs are dropped.  Each run moves up (towards 0) by a multiple of
 *     the strictest alignment in it, capped at 8, so every object keeps its
 *     alignment; a run holding a pinned gap stays where it is.
 *   - Every stack operand is rewritten by its segment's displacement, and *loc
 *     is raised to the new bottom, so the register allocator places spills
 *     directly below the live objects.
 *
 * Only operands WITHOUT a vreg name frontend objects: a vreg-backed StackLoc is
 * resolved relative to the vreg's own original offset and allocator slot.
 * Functions whose frame is reachable other than through IR operands are left
 * alone: -g (debug info records the original offsets), bounds checking,
 * variadic functions (register save area), nested functions and captures
 * (the static chain reads the parent's frame by offset), VLAs, inline asm,
 * setjmp.
 */

#define USING_GLOBALS
#include "ir.h"

int tcc_ir_frame_alloc(int loc, int size, int mask)
{
  const int off = (loc - size) & mask;
  TCCIRState *ir = tcc_state ? tcc_state->ir : NULL;
  /* Recorded with the alignment padding above it: the padding is freed or moved
   * together with the object, instead of forming an unknown (pinned) gap. */
  if (ir && loc > off)
  {
    if (ir->frame_obj_count == ir->frame_obj_cap)
    {
      ir->frame_obj_cap = ir->frame_obj_cap ? ir->frame_obj_cap * 2 : 32;
      ir->frame_objs = tcc_realloc(ir->frame_objs, sizeof(int32_t) * 2 * ir->frame_obj_cap);
    }
    ir->frame_objs[2 * ir->frame_obj_count] = off;
    ir->frame_objs[2 * ir->frame_obj_count + 1] = loc - off;
    ir->frame_obj_count++;
  }
  return off;
}

typedef struct
{
  int start, end; /* [start, end), start < end <= 0 */
  uint8_t live;
  uint8_t pinned; /* a gap: some untracked allocation, never dropped or moved */
  int delta;      /* displacement applied to offsets inside the segment */
} FrameSeg;

static int frame_obj_cmp(const void *a, const void *b)
{
  const int32_t *x = a, *y = b;
  return x[0] < y[0] ? -1 : x[0] > y[0];
}

/* Largest power of two (<= 8) dividing a frame offset. */
static int frame_offset_align(int off)
{
  int a = 8;
  while (a > 1 && (off & (a - 1)))
    a >>= 1;
  return a;
}

static int frame_seg_find(const FrameSeg *seg, int n, int off)
{
  int lo = 0, hi = n - 1;
  while (lo <= hi)
  {
    int mid = (lo + hi) / 2;
    if (off < seg[mid].start)
      hi = mid - 1;
    else if (off >= seg[mid].end)
      lo = mid + 1;
    else
      return mid;
  }
  return -1;
}

/* An operand machine_op_from_ir resolves to a concrete frame slot (its case 3:
 * no vreg, stack-relative) inside [bottom, 0).  Immediates and symbols with a
 * stray is_local are resolved before that case and never name the frame. */
static int frame_operand_offset(IROperand op, int bottom, int *off)
{
  if (irop_is_none(op) || irop_get_vreg(op) >= 0 || op.is_param)
    return 0;
  int tag = irop_get_tag(op);
  if (tag != IROP_TAG_STACKOFF && !(tag == IROP_TAG_VREG && (op.is_local || op.is_llocal)))
    return 0;
  *off = irop_get_stack_offset(op);
  return *off >= bottom && *off < 0;
}

static int frame_function_eligible(TCCIRState *ir)
{
  if (tcc_state->do_debug || tcc_state->do_bounds_check)
    return 0;
  if (ir->is_variadic || ir->has_static_chain || ir->captured_count > 0 || tcc_state->nb_nested_funcs > 0)
    return 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    switch (ir->compact_instructions[i].op)
    {
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

int tcc_ir_frame_relayout(TCCIRState *ir, int *ploc)
{
  const int bottom = *ploc;
  if (!ir || bottom >= 0 || ir->frame_obj_count == 0 || !frame_function_eligible(ir))
    return 0;

  /* Objects -> disjoint segments tiling [bottom, 0), gaps included. */
  int nobj = ir->frame_obj_count;
  qsort(ir->frame_objs, nobj, sizeof(int32_t) * 2, frame_obj_cmp);
  FrameSeg *seg = tcc_malloc(sizeof(FrameSeg) * (2 * nobj + 2));
  int nseg = 0, cur = bottom;
  for (int k = 0; k < nobj; k++)
  {
    int s = ir->frame_objs[2 * k], e = s + ir->frame_objs[2 * k + 1];
    if (s < bottom)
      s = bottom;
    if (e > 0)
      e = 0;
    if (e <= cur)
      continue; /* inside an earlier object, or outside the frame */
    if (s < cur)
    {
      /* Overlapping objects share bytes, so they live and move as one. */
      seg[nseg - 1].end = e;
      cur = e;
      continue;
    }
    if (s > cur)
      seg[nseg++] = (FrameSeg){cur, s, 1, 1, 0};
    seg[nseg++] = (FrameSeg){s, e, 0, 0, 0};
    cur = e;
  }
  if (cur < 0)
    seg[nseg++] = (FrameSeg){cur, 0, 1, 1, 0};

  /* Liveness over the whole operand pool rather than per instruction: every
   * operand slot is covered whatever its position, and an orphaned or NOP'd
   * operand only keeps an object alive. */
  IROperand *pool = ir->iroperand_pool;
  const int npool = ir->iroperand_pool_count;
  for (int i = 0; i < npool; i++)
  {
    int off, s;
    if (frame_operand_offset(pool[i], bottom, &off) && (s = frame_seg_find(seg, nseg, off)) >= 0)
      seg[s].live = 1;
  }

  int dead = 0;
  for (int s = 0; s < nseg; s++)
    dead += !seg[s].live;
  if (!dead)
  {
    tcc_free(seg);
    return 0;
  }

  /* Pack runs of live segments towards 0, highest first.  `top` is the lowest
   * byte taken so far; a run moves up to it, rounded down to the run's
   * alignment, unless it holds a pinned gap. */
  int top = 0;
  for (int s = nseg - 1; s >= 0;)
  {
    if (!seg[s].live)
    {
      s--;
      continue;
    }
    int r_hi = s, r_lo = s, align = 1, pinned = 0;
    for (; r_lo >= 0 && seg[r_lo].live; r_lo--)
    {
      int a = frame_offset_align(seg[r_lo].start);
      if (a > align)
        align = a;
      pinned |= seg[r_lo].pinned;
    }
    r_lo++;
    int room = top - seg[r_hi].end;
    int delta = pinned ? 0 : room - room % align;
    for (int k = r_lo; k <= r_hi; k++)
      seg[k].delta = delta;
    top = seg[r_lo].start + delta;
    s = r_lo - 1;
  }

  for (int i = 0; i < npool; i++)
  {
    int off, s;
    if (!frame_operand_offset(pool[i], bottom, &off) || (s = frame_seg_find(seg, nseg, off)) < 0 || !seg[s].delta)
      continue;
    if (pool[i].btype == IROP_BTYPE_STRUCT)
      pool[i].u.s.aux_data = (int16_t)(off + seg[s].delta);
    else
      pool[i].u.imm32 = off + seg[s].delta;
  }

  tcc_free(seg);
  int new_bottom = top & ~7;
  if (new_bottom <= bottom)
    return 0;
  *ploc = new_bottom;
  return new_bottom - bottom;
}
