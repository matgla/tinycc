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
 * LDR/STR [sp] forms, and past 4095 need an extra instruction.  In the Zig
 * compiler built as C, dropping them shrinks the summed frames by 16%, and
 * letting objects whose lifetimes do not overlap share bytes by 35%.
 *
 * tcc_ir_frame_relayout lays the frame out again.  It runs just before the
 * linear scan, once every pass that adds or drops a frame reference is done:
 *
 *   - The frame region [loc, 0) is cut into SEGMENTS: every object the frontend
 *     allocated through tcc_ir_frame_alloc*, with its padding, plus the gaps
 *     between them.  A gap is an allocation made directly on `loc` --
 *     parameter homes the prologue stores to, setjmp and __label__ buffers,
 *     VLA pointer slots -- and is PINNED: never dropped, never moved.
 *   - A segment is live if any stack operand points into it.
 *   - Colouring (frame_colour, below) then gives each live object a lifetime
 *     and places objects with disjoint lifetimes on the same bytes, the most
 *     referenced ones nearest sp, where the short [sp, #imm] forms reach.
 *   - Where colouring does not apply (computed gotos, a call that returns
 *     twice) or does no better, the dead segments are dropped and the runs of
 *     consecutive live ones packed towards 0 as blocks, so an address computed
 *     across segments inside a run stays valid.  Each run moves by a multiple
 *     of the strictest alignment in it, capped at 8; a run holding a pinned
 *     gap stays where it is.
 *   - Every stack operand is rewritten by its segment's displacement, and *loc
 *     is raised to the new bottom, so the register allocator places spills
 *     directly below the objects.
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
#include <limits.h>

#define FRAME_OBJ_ARG_COPY 1 /* a by-value argument's copy, never user-visible */
#define FRAME_OBJ_RET_TEMP 2 /* a call's struct result */

static int frame_record(int loc, int size, int mask, int flags)
{
  TCCIRState *ir = tcc_state ? tcc_state->ir : NULL;
  /* When the layout may be recoloured, a guard byte above the object keeps its
   * one-past-end address inside its own record: otherwise it would equal the
   * start of the object allocated before it, and could not be told apart. */
  int guard = ir && tcc_state->optimize > 0 && !tcc_state->do_debug && !tcc_state->do_bounds_check;
  /* The guard must not cost an object the word alignment it would otherwise
   * have had: inline copies into a char array use LDM/STM, which fault on an
   * unaligned address. */
  if (guard && size >= 4 && !(((loc - size) & mask) & 3))
    mask &= -4;
  const int off = (loc - size - guard) & mask;
  /* Recorded with the padding above it: the padding is freed or moved together
   * with the object, instead of forming an unknown (pinned) gap. */
  if (ir && loc > off)
  {
    if (ir->frame_obj_count == ir->frame_obj_cap)
    {
      ir->frame_obj_cap = ir->frame_obj_cap ? ir->frame_obj_cap * 2 : 32;
      ir->frame_objs = tcc_realloc(ir->frame_objs, sizeof(int32_t) * 4 * ir->frame_obj_cap);
    }
    int32_t *o = &ir->frame_objs[4 * ir->frame_obj_count++];
    o[0] = off;
    o[1] = loc - off;
    o[2] = size > 0 ? size : 1;
    o[3] = flags;
  }
  return off;
}

int tcc_ir_frame_alloc(int loc, int size, int mask)
{
  return frame_record(loc, size, mask, 0);
}

int tcc_ir_frame_alloc_arg_copy(int loc, int size, int mask)
{
  return frame_record(loc, size, mask, FRAME_OBJ_ARG_COPY);
}

int tcc_ir_frame_alloc_ret_temp(int loc, int size, int mask)
{
  return frame_record(loc, size, mask, FRAME_OBJ_RET_TEMP);
}

void tcc_ir_frame_note_sret_call(int call_id)
{
  TCCIRState *ir = tcc_state ? tcc_state->ir : NULL;
  if (!ir || call_id < 0)
    return;
  if (call_id >= ir->sret_calls_size)
  {
    int n = ir->sret_calls_size ? ir->sret_calls_size : 64;
    while (n <= call_id)
      n *= 2;
    ir->sret_calls = tcc_realloc(ir->sret_calls, n);
    memset(ir->sret_calls + ir->sret_calls_size, 0, n - ir->sret_calls_size);
    ir->sret_calls_size = n;
  }
  ir->sret_calls[call_id] = 1;
}

static int frame_idx_cmp(const void *a, const void *b)
{
  const int32_t *x = a, *y = b;
  return x[0] < y[0] ? -1 : x[0] > y[0];
}

int tcc_ir_frame_object_at(TCCIRState *ir, int off, int *lo, int *hi)
{
  if (!ir || ir->frame_relaid || !ir->frame_obj_count)
    return 0;
  if (ir->frame_index_objs != ir->frame_obj_count)
  {
    /* Rebuild: extents sorted by start, overlapping ones merged. */
    int n = ir->frame_obj_count, m = 0;
    int32_t *e = tcc_malloc(sizeof(int32_t) * 2 * n);
    for (int k = 0; k < n; k++)
    {
      e[2 * k] = ir->frame_objs[4 * k];
      e[2 * k + 1] = ir->frame_objs[4 * k] + ir->frame_objs[4 * k + 1];
    }
    qsort(e, n, sizeof(int32_t) * 2, frame_idx_cmp);
    for (int k = 0; k < n; k++)
    {
      if (m && e[2 * k] < e[2 * m - 1])
      {
        if (e[2 * k + 1] > e[2 * m - 1])
          e[2 * m - 1] = e[2 * k + 1];
        continue;
      }
      e[2 * m] = e[2 * k];
      e[2 * m + 1] = e[2 * k + 1];
      m++;
    }
    tcc_free(ir->frame_index);
    ir->frame_index = e;
    ir->frame_index_n = m;
    ir->frame_index_objs = n;
  }
  int a = 0, b = ir->frame_index_n - 1;
  while (a <= b)
  {
    int mid = (a + b) / 2;
    if (off < ir->frame_index[2 * mid])
      b = mid - 1;
    else if (off >= ir->frame_index[2 * mid + 1])
      a = mid + 1;
    else
    {
      *lo = ir->frame_index[2 * mid];
      *hi = ir->frame_index[2 * mid + 1];
      return 1;
    }
  }
  return 0;
}

/* GCC's list (special_function_p), leading underscores ignored. */
static int frame_name_returns_twice(const char *name)
{
  static const char *const names[] = {"setjmp",   "sigsetjmp",  "savectx",       "vfork",
                                      "qsetjmp",  "getcontext", "setjmp_syscall", "secure_setjmp"};
  while (*name == '_')
    name++;
  for (unsigned k = 0; k < sizeof names / sizeof names[0]; k++)
    if (!strcmp(name, names[k]))
      return 1;
  return 0;
}

int tcc_ir_calls_returns_twice(TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_SETJMP || q->op == TCCIR_OP_NL_SETJMP)
      return 1;
    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;
    IROperand callee = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(callee) != IROP_TAG_SYMREF)
      continue;
    Sym *sym = irop_get_sym_ex(ir, callee);
    if (sym && frame_name_returns_twice(get_tok_str(sym->v, NULL)))
      return 1;
  }
  return 0;
}

typedef struct
{
  int start, end; /* [start, end), start < end <= 0: the object's record */
  int size;       /* the bytes the object itself occupies from start */
  uint8_t live;
  uint8_t pinned;  /* a gap: some untracked allocation, never dropped or moved */
  uint8_t kind;    /* FRAME_OBJ_* */
  uint8_t escaped; /* its address may be used anywhere after it is taken */
  int first[2], last[2]; /* lifetime in source order and in RPO (frame_rpo);
                          * first > last if none */
  int refs;        /* instructions referencing it: how hot it is */
  int unit;
  int delta; /* displacement applied to offsets inside the segment */
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
 * no vreg, stack-relative).  Immediates and symbols with a stray is_local are
 * resolved before that case and never name the frame. */
static int frame_operand_concrete(IROperand op, int *off)
{
  if (irop_is_none(op) || irop_get_vreg(op) >= 0 || op.is_param)
    return 0;
  int tag = irop_get_tag(op);
  if (tag != IROP_TAG_STACKOFF && !(tag == IROP_TAG_VREG && (op.is_local || op.is_llocal)))
    return 0;
  *off = irop_get_stack_offset(op);
  return 1;
}

static int frame_operand_offset(IROperand op, int bottom, int *off)
{
  return frame_operand_concrete(op, off) && *off >= bottom && *off < 0;
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

/* ---- Step 1 fallback: drop dead segments, pack live runs towards 0 ---- */

/* Consecutive live segments form a run that moves as one block; a run holding
 * a pinned gap stays where it is.  Returns the new bottom. */
static int frame_pack_runs(FrameSeg *seg, int nseg)
{
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
  return top;
}

/* ---- Lifetime colouring ----
 *
 * An object is live from its first to its last reference, where a reference is
 * any operand naming it or holding a pointer into it.  The range is kept in
 * two orders of the instructions: source order, and reverse post-order of the
 * control-flow graph (frame_rpo).
 * Pointers are followed through vregs (flow-insensitively: a vreg that may
 * point to two objects gives up on both).  An object whose address escapes --
 * stored to memory, passed to a call, returned, into a vreg held in memory --
 * stays live to the end of the function.  The address of a call temporary
 * passed to its call, of any object passed as a struct-return buffer, or of
 * any object passed to memcpy, strlen and the like is not an escape: the
 * callee cannot keep it.  Every range that intersects a loop (a back edge) is
 * widened to the whole loop, until no range changes.
 *
 * Two objects whose ranges are disjoint in either order may then share bytes:
 * in that order any execution path from a reference of the later one back to
 * the earlier one crosses a backward edge whose span the later one's range
 * already covers.  Neither order wins everywhere: tinycc emits a switch's
 * dispatch after its cases, so in source order its jumps into them look like
 * loops; the Zig C backend's `if (c) { ...; goto out; }` inside a loop puts
 * the rest of the function inside the loop's span in RPO. */

#define TAINT_NONE (-1)
#define TAINT_MULTI (-2)

enum
{
  FOP_NONE,
  FOP_MEM,     /* a frame object's bytes */
  FOP_ADDR,    /* a frame object's address */
  FOP_VAL,     /* a vreg's value */
  FOP_DEREF,   /* memory through a vreg's value */
  FOP_VAR_ADDR /* the address of a VAR/PARAM (lives in an allocator slot) */
};

typedef struct
{
  TCCIRState *ir;
  FrameSeg *seg;
  int nseg, bottom, n;
  int32_t *taint[4]; /* by vreg type: VAR, TEMP, PARAM */
  int taint_size[4];
  uint8_t *var_addr_taken[4];
  int changed, bad;
} FrameLive;

static int frame_classify(FrameLive *fl, IROperand op, int *id)
{
  if (irop_is_none(op))
    return FOP_NONE;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
  {
    int off, s;
    if (!frame_operand_offset(op, fl->bottom, &off) || (s = frame_seg_find(fl->seg, fl->nseg, off)) < 0)
      return FOP_NONE;
    *id = s;
    return (op.is_lval || op.is_llocal) ? FOP_MEM : FOP_ADDR;
  }
  int type = TCCIR_DECODE_VREG_TYPE(vr), pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (type < 1 || type > 3 || pos >= fl->taint_size[type])
  {
    fl->bad = 1;
    return FOP_NONE;
  }
  *id = vr;
  int tag = irop_get_tag(op);
  if (tag == IROP_TAG_VREG)
    return op.is_lval ? FOP_DEREF : FOP_VAL;
  if (tag == IROP_TAG_STACKOFF)
  {
    if (op.is_llocal)
      return FOP_DEREF;
    return op.is_lval ? FOP_VAL : FOP_VAR_ADDR;
  }
  return FOP_NONE;
}

static int32_t *frame_taint_ref(FrameLive *fl, int32_t vr)
{
  return &fl->taint[TCCIR_DECODE_VREG_TYPE(vr)][TCCIR_DECODE_VREG_POSITION(vr)];
}

static void frame_escape(FrameLive *fl, int t)
{
  if (t >= 0)
    fl->seg[t].escaped = 1;
}

static int frame_taint_merge(FrameLive *fl, int a, int b)
{
  if (b == TAINT_NONE || a == b)
    return a;
  if (a == TAINT_NONE)
    return b;
  frame_escape(fl, a);
  frame_escape(fl, b);
  return TAINT_MULTI;
}

static void frame_taint_join(FrameLive *fl, int32_t vr, int t)
{
  int32_t *p = frame_taint_ref(fl, vr);
  int m = frame_taint_merge(fl, *p, t);
  if (m != *p)
  {
    *p = m;
    fl->changed = 1;
  }
}

/* The object a value operand points into, if any. */
static int frame_op_taint(FrameLive *fl, IROperand op)
{
  int id, k = frame_classify(fl, op, &id);
  if (k == FOP_ADDR)
    return id;
  if (k == FOP_VAL)
    return *frame_taint_ref(fl, id);
  return TAINT_NONE;
}

/* A reference to segment `s` by instruction `i` (at RPO position `p`). */
static void frame_use(FrameLive *fl, int s, int i, int p)
{
  if (s < 0)
    return;
  FrameSeg *g = &fl->seg[s];
  if (i < g->first[0])
    g->first[0] = i;
  if (i > g->last[0])
    g->last[0] = i;
  if (p < g->first[1])
    g->first[1] = p;
  if (p > g->last[1])
    g->last[1] = p;
}

/* Ops whose result is computed from their sources' values: a pointer operand
 * may come out as (derived from) the result. */
static int frame_op_passes_value(int op)
{
  switch (op)
  {
  case TCCIR_OP_ADD:
  case TCCIR_OP_ADC_USE:
  case TCCIR_OP_ADC_GEN:
  case TCCIR_OP_SUB:
  case TCCIR_OP_SUBC_USE:
  case TCCIR_OP_SUBC_GEN:
  case TCCIR_OP_MUL:
  case TCCIR_OP_MLA:
  case TCCIR_OP_UMULL:
  case TCCIR_OP_SMULL:
  case TCCIR_OP_DIV:
  case TCCIR_OP_UDIV:
  case TCCIR_OP_PDIV:
  case TCCIR_OP_UMOD:
  case TCCIR_OP_IMOD:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
  case TCCIR_OP_ROR:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_SELECT:
  case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_BFI:
  case TCCIR_OP_ZEXT:
  case TCCIR_OP_PACK64:
  case TCCIR_OP_SETIF:
  case TCCIR_OP_BOOL_OR:
  case TCCIR_OP_BOOL_AND:
  case TCCIR_OP_CLZ:
  case TCCIR_OP_RBIT:
  case TCCIR_OP_REV:
  case TCCIR_OP_REV16:
    return 1;
  default:
    return 0;
  }
}

/* Ops that read their sources without letting a pointer among them escape. */
static int frame_op_keeps_pointers(int op)
{
  if (frame_op_passes_value(op))
    return 1;
  switch (op)
  {
  case TCCIR_OP_CMP:
  case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_LOAD:
  case TCCIR_OP_LOAD_INDEXED:
  case TCCIR_OP_LOAD_POSTINC:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SWITCH_LOAD:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_NOP:
    return 1;
  default:
    return 0;
  }
}

/* The fourth operand slot some ops keep at operand_base + 3. */
static int frame_op_has_slot3(int op)
{
  return op == TCCIR_OP_MLA || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_SELECT;
}

static int frame_call_id(TCCIRState *ir, IROperand enc)
{
  return TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, enc));
}

/* C library functions that keep no pointer argument past the call: 1, or 2
 * when the result may point into the first argument (memcpy returns it,
 * strchr returns a pointer inside it). */
static int frame_callee_nocapture(TCCIRState *ir, IRQuadCompact *call)
{
  static const char *const keeps_none[] = {"memcmp",  "bcmp",    "strcmp",  "strncmp",
                                           "strlen",  "strnlen", "strspn",  "strcspn",
                                           "__aeabi_memcpy",  "__aeabi_memcpy4",  "__aeabi_memcpy8",
                                           "__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8",
                                           "__aeabi_memset",  "__aeabi_memset4",  "__aeabi_memset8",
                                           "__aeabi_memclr",  "__aeabi_memclr4",  "__aeabi_memclr8"};
  static const char *const returns_arg0[] = {"memcpy", "memmove", "memset", "strcpy", "strncpy", "strcat",
                                             "strncat", "strchr", "strrchr", "memchr", "strstr", "strpbrk"};
  IROperand callee = tcc_ir_op_get_src1(ir, call);
  if (irop_get_tag(callee) != IROP_TAG_SYMREF)
    return 0;
  Sym *sym = irop_get_sym_ex(ir, callee);
  const char *name = sym ? get_tok_str(sym->v, NULL) : NULL;
  if (!name)
    return 0;
  for (unsigned k = 0; k < sizeof keeps_none / sizeof keeps_none[0]; k++)
    if (!strcmp(name, keeps_none[k]))
      return 1;
  for (unsigned k = 0; k < sizeof returns_arg0 / sizeof returns_arg0[0]; k++)
    if (!strcmp(name, returns_arg0[k]))
      return 2;
  return 0;
}

/* Positions of the instructions in reverse post-order of the control-flow
 * graph, and its back edges as (target, source) position pairs.  In RPO every
 * edge that is not a back edge goes forward, so a path can only return to an
 * earlier position across a back edge -- which the lifetime widening needs.
 * Source order would not do: a switch's dispatch follows its cases, and its
 * forward jumps into them would all look like loops.  Returns 0 on a switch the
 * graph cannot follow.  Unreachable blocks go last, in source order. */
static int frame_rpo(TCCIRState *ir, int n, int *pos, int **be_out, int *nbe_out, int **sbe_out, int *nsbe_out)
{
  uint8_t *leader = tcc_mallocz(n + 1);
  int *block_of = tcc_malloc(sizeof(int) * n);
  int ok = 0, nb = 0;
  int *bstart = NULL, *succ = NULL, *succ_at = NULL, *post = NULL, *stack = NULL, *next_succ = NULL;
  uint8_t *seen = NULL;
  int *be = NULL, nbe = 0, cap = 0;
  int *sbe = NULL, nsbe = 0, scap2 = 0;

  leader[0] = 1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      /* A target past the last instruction is the epilogue: an exit. */
      int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (t >= 0 && t < n)
        leader[t] = 1;
    }
    else if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      int tid = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (tid < 0 || tid >= ir->num_switch_tables)
        goto done;
      TCCIRSwitchTable *tab = &ir->switch_tables[tid];
      for (int k = -1; k < tab->num_entries; k++)
      {
        int t = k < 0 ? tab->default_target : tab->targets[k];
        if (t >= 0 && t < n)
          leader[t] = 1;
      }
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_IJUMP)
      leader[i + 1] = 1;
  }
  for (int i = 0; i < n; i++)
  {
    if (leader[i])
      nb++;
    block_of[i] = nb - 1;
  }
  bstart = tcc_malloc(sizeof(int) * (nb + 1));
  for (int i = 0, b = 0; i < n; i++)
    if (leader[i])
      bstart[b++] = i;
  bstart[nb] = n;

  /* Successor lists. */
  succ_at = tcc_malloc(sizeof(int) * (nb + 1));
  int nsucc = 0, scap = nb * 2 + 4;
  succ = tcc_malloc(sizeof(int) * scap);
  for (int b = 0; b < nb; b++)
  {
    succ_at[b] = nsucc;
    IRQuadCompact *q = &ir->compact_instructions[bstart[b + 1] - 1];
    int fall = q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_SWITCH_TABLE && q->op != TCCIR_OP_RETURNVALUE &&
               q->op != TCCIR_OP_RETURNVOID && q->op != TCCIR_OP_IJUMP;
    int extra = q->op == TCCIR_OP_SWITCH_TABLE ? ir->switch_tables[irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q))].num_entries + 1 : 1;
    if (nsucc + extra + 1 > scap)
    {
      scap = (nsucc + extra + 1) * 2;
      succ = tcc_realloc(succ, sizeof(int) * scap);
    }
    /* Visit order, which decides the layout: jumps forward in the source
     * first, then the fallthrough, then jumps back.  The first visited comes
     * last in RPO, so a loop's body follows its header and precedes its exits
     * whether the loop tests at the top or the bottom, and a switch's cases
     * follow its dispatch however the dispatch was laid out. */
    for (int pass = 0; pass < 3; pass++)
    {
      if (pass == 1)
      {
        if (fall && b + 1 < nb)
          succ[nsucc++] = b + 1;
        continue;
      }
      int back = pass == 2;
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        int ti = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
        if (ti >= 0 && ti < n && (block_of[ti] <= b) == back)
          succ[nsucc++] = block_of[ti];
      }
      else if (q->op == TCCIR_OP_SWITCH_TABLE)
      {
        TCCIRSwitchTable *tab = &ir->switch_tables[irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q))];
        for (int k = -1; k < tab->num_entries; k++)
        {
          int t = k < 0 ? tab->default_target : tab->targets[k];
          if (t >= 0 && t < n && (block_of[t] <= b) == back)
            succ[nsucc++] = block_of[t];
        }
      }
    }
  }
  succ_at[nb] = nsucc;

  /* Iterative DFS post-order from the entry. */
  post = tcc_malloc(sizeof(int) * nb);
  stack = tcc_malloc(sizeof(int) * nb);
  next_succ = tcc_malloc(sizeof(int) * nb);
  seen = tcc_mallocz(nb);
  int npost = 0, sp = 0;
  stack[sp++] = 0;
  seen[0] = 1;
  next_succ[0] = succ_at[0];
  while (sp)
  {
    int b = stack[sp - 1];
    if (next_succ[b] < succ_at[b + 1])
    {
      int v = succ[next_succ[b]++];
      if (!seen[v])
      {
        seen[v] = 1;
        next_succ[v] = succ_at[v];
        stack[sp++] = v;
      }
    }
    else
    {
      post[npost++] = b;
      sp--;
    }
  }

  int p = 0;
  for (int k = npost - 1; k >= 0; k--)
    for (int i = bstart[post[k]]; i < bstart[post[k] + 1]; i++)
      pos[i] = p++;
  for (int b = 0; b < nb; b++)
    if (!seen[b])
      for (int i = bstart[b]; i < bstart[b + 1]; i++)
        pos[i] = p++;

  for (int b = 0; b < nb; b++)
  {
    if (!seen[b])
      continue;
    int src = pos[bstart[b + 1] - 1], src_i = bstart[b + 1] - 1;
    for (int k = succ_at[b]; k < succ_at[b + 1]; k++)
    {
      int dst = pos[bstart[succ[k]]], dst_i = bstart[succ[k]];
      if (dst <= src)
      {
        if (nbe == cap)
        {
          cap = cap ? cap * 2 : 16;
          be = tcc_realloc(be, sizeof(int) * 2 * cap);
        }
        be[2 * nbe] = dst;
        be[2 * nbe + 1] = src;
        nbe++;
      }
      if (dst_i <= src_i)
      {
        if (nsbe == scap2)
        {
          scap2 = scap2 ? scap2 * 2 : 16;
          sbe = tcc_realloc(sbe, sizeof(int) * 2 * scap2);
        }
        sbe[2 * nsbe] = dst_i;
        sbe[2 * nsbe + 1] = src_i;
        nsbe++;
      }
    }
  }
  ok = 1;

done:
  tcc_free(leader);
  tcc_free(block_of);
  tcc_free(bstart);
  tcc_free(succ);
  tcc_free(succ_at);
  tcc_free(post);
  tcc_free(stack);
  tcc_free(next_succ);
  tcc_free(seen);
  if (!ok)
  {
    tcc_free(be);
    tcc_free(sbe);
    be = sbe = NULL, nbe = nsbe = 0;
  }
  *be_out = be;
  *nbe_out = nbe;
  *sbe_out = sbe;
  *nsbe_out = nsbe;
  return ok;
}

/* Lifetimes of the live segments.  Returns 0 if they cannot be computed. */
static int frame_lifetimes(FrameLive *fl)
{
  TCCIRState *ir = fl->ir;
  const int n = fl->n;
  const int ncall = ir->next_call_id + 1;
  int ok = 0;

  static const int types[3] = {TCCIR_VREG_TYPE_VAR, TCCIR_VREG_TYPE_TEMP, TCCIR_VREG_TYPE_PARAM};
  fl->taint_size[TCCIR_VREG_TYPE_VAR] = ir->variables_live_intervals_size;
  fl->taint_size[TCCIR_VREG_TYPE_TEMP] = ir->temporary_variables_live_intervals_size;
  fl->taint_size[TCCIR_VREG_TYPE_PARAM] = ir->parameters_live_intervals_size;
  for (int k = 0; k < 3; k++)
  {
    int t = types[k], sz = fl->taint_size[t] > 0 ? fl->taint_size[t] : 1;
    fl->taint[t] = tcc_malloc(sizeof(int32_t) * sz);
    for (int j = 0; j < sz; j++)
      fl->taint[t][j] = TAINT_NONE;
    fl->var_addr_taken[t] = tcc_mallocz(sz);
  }

  /* Each FUNCPARAM's call (the next call with its id), and its call's params. */
  int *param_call = tcc_malloc(sizeof(int) * (n > 0 ? n : 1));
  int *call_at = tcc_malloc(sizeof(int) * ncall);
  int *param_next = tcc_malloc(sizeof(int) * (n > 0 ? n : 1));
  int *call_params = tcc_malloc(sizeof(int) * (n > 0 ? n : 1));
  uint8_t *nocapture = tcc_mallocz(n > 0 ? n : 1);
  int *pos = tcc_malloc(sizeof(int) * (n > 0 ? n : 1));
  int *be = NULL, nbe = 0, *sbe = NULL, nsbe = 0;
  for (int c = 0; c < ncall; c++)
    call_at[c] = -1;
  if (!frame_rpo(ir, n, pos, &be, &nbe, &sbe, &nsbe))
    goto out;
  for (int i = n - 1; i >= 0; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    param_call[i] = -1;
    call_params[i] = -1;
    param_next[i] = -1;
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      int c = frame_call_id(ir, tcc_ir_op_get_src2(ir, q));
      if (c < 0 || c >= ncall)
        goto out;
      call_at[c] = i;
      nocapture[i] = (uint8_t)frame_callee_nocapture(ir, q);
    }
    else if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      int c = frame_call_id(ir, tcc_ir_op_get_src2(ir, q));
      if (c < 0 || c >= ncall)
        goto out;
      int ci = call_at[c];
      param_call[i] = ci >= 0 ? ci : i;
      if (ci >= 0)
      {
        param_next[i] = call_params[ci];
        call_params[ci] = i;
      }
    }
  }

  /* Pointer flow through vregs, to a fixpoint. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int nops = irop_config[q->op].has_dest + irop_config[q->op].has_src1 + irop_config[q->op].has_src2 +
               frame_op_has_slot3(q->op);
    for (int k = 0; k < nops; k++)
    {
      int id;
      if (frame_classify(fl, ir->iroperand_pool[q->operand_base + k], &id) == FOP_VAR_ADDR)
        fl->var_addr_taken[TCCIR_DECODE_VREG_TYPE(id)][TCCIR_DECODE_VREG_POSITION(id)] = 1;
    }
  }
  do
  {
    fl->changed = 0;
    for (int i = 0; i < n && !fl->bad; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      int op = q->op, id;
      if (op == TCCIR_OP_NOP || !irop_config[op].has_dest)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (frame_classify(fl, dest, &id) != FOP_VAL)
        continue;
      if (op == TCCIR_OP_FUNCCALLVAL)
      {
        /* The result may point into a struct result passed on (`f().arr`).
         * Any other object reaching a call escapes anyway, and an argument's
         * copy is never seen by the callee. */
        for (int p = call_params[i]; p >= 0; p = param_next[p])
        {
          IRQuadCompact *pq = &ir->compact_instructions[p];
          int t = frame_op_taint(fl, tcc_ir_op_get_src1(ir, pq));
          int idx = TCCIR_DECODE_PARAM_IDX((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, pq)));
          if (t >= 0 && (fl->seg[t].kind == FRAME_OBJ_RET_TEMP || (nocapture[i] == 2 && idx == 0)))
            frame_taint_join(fl, id, t);
        }
      }
      else if (op == TCCIR_OP_STORE || frame_op_passes_value(op))
      {
        if (irop_config[op].has_src1)
          frame_taint_join(fl, id, frame_op_taint(fl, tcc_ir_op_get_src1(ir, q)));
        if (irop_config[op].has_src2)
          frame_taint_join(fl, id, frame_op_taint(fl, tcc_ir_op_get_src2(ir, q)));
        if (op == TCCIR_OP_MLA)
          frame_taint_join(fl, id, frame_op_taint(fl, tcc_ir_op_get_accum(ir, q)));
      }
    }
  } while (fl->changed && !fl->bad);
  if (fl->bad)
    goto out;

  /* References and escapes.  Pool entries no instruction operand covers are
   * orphans a pass replaced; they are never executed. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    int nops = irop_config[op].has_dest + irop_config[op].has_src1 + irop_config[op].has_src2 + frame_op_has_slot3(op);
    for (int k = 0; k < nops; k++)
    {
      uint32_t pi = q->operand_base + k;
      if (pi >= (uint32_t)ir->iroperand_pool_count)
        continue;
      int id, kind = frame_classify(fl, ir->iroperand_pool[pi], &id);
      int t = TAINT_NONE;
      if (kind == FOP_MEM || kind == FOP_ADDR)
        t = id;
      else if (kind == FOP_VAL || kind == FOP_DEREF)
        t = *frame_taint_ref(fl, id);
      frame_use(fl, t, i, pos[i]);
      if (t >= 0)
        fl->seg[t].refs++;
      /* A parameter is read by its call. */
      if (op == TCCIR_OP_FUNCPARAMVAL)
        frame_use(fl, t, param_call[i], pos[param_call[i]]);
    }

    int has_dest = irop_config[op].has_dest, id;
    switch (op)
    {
    case TCCIR_OP_STORE:
      if (frame_classify(fl, tcc_ir_op_get_dest(ir, q), &id) != FOP_VAL)
        frame_escape(fl, frame_op_taint(fl, tcc_ir_op_get_src1(ir, q)));
      break;
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
      frame_escape(fl, frame_op_taint(fl, tcc_ir_op_get_src1(ir, q)));
      break;
    case TCCIR_OP_FUNCPARAMVAL:
    {
      int t = frame_op_taint(fl, tcc_ir_op_get_src1(ir, q));
      if (t >= 0)
      {
        IROperand enc = tcc_ir_op_get_src2(ir, q);
        int c = frame_call_id(ir, enc);
        int idx = TCCIR_DECODE_PARAM_IDX((uint32_t)irop_get_imm64_ex(ir, enc));
        int sret = idx == 0 && c < ir->sret_calls_size && ir->sret_calls[c];
        if (!fl->seg[t].kind && !sret && !(param_call[i] != i && nocapture[param_call[i]]))
          frame_escape(fl, t);
      }
      break;
    }
    default:
      if (!frame_op_keeps_pointers(op))
      {
        /* Any other consumer of a pointer value lets it go. */
        if (irop_config[op].has_src1)
          frame_escape(fl, frame_op_taint(fl, tcc_ir_op_get_src1(ir, q)));
        if (irop_config[op].has_src2)
          frame_escape(fl, frame_op_taint(fl, tcc_ir_op_get_src2(ir, q)));
        if (has_dest && op != TCCIR_OP_FUNCCALLVAL)
          frame_escape(fl, frame_op_taint(fl, tcc_ir_op_get_dest(ir, q)));
      }
      break;
    }
  }
  for (int k = 0; k < 3; k++)
  {
    int t = types[k];
    for (int j = 0; j < fl->taint_size[t]; j++)
      if (fl->var_addr_taken[t][j])
        frame_escape(fl, fl->taint[t][j]);
  }
  if (fl->bad)
    goto out;

  for (int s = 0; s < fl->nseg; s++)
    if (fl->seg[s].escaped && fl->seg[s].first[0] <= fl->seg[s].last[0])
      fl->seg[s].last[0] = fl->seg[s].last[1] = n - 1;

  /* Widen every range over the loops it intersects, in each order. */
  for (int o = 0; o < 2; o++)
  {
    const int *e = o ? be : sbe;
    const int ne = o ? nbe : nsbe;
    for (int s = 0; s < fl->nseg; s++)
    {
      FrameSeg *g = &fl->seg[s];
      if (g->first[o] > g->last[o])
        continue;
      int grew;
      do
      {
        grew = 0;
        for (int k = 0; k < ne; k++)
          if (g->first[o] <= e[2 * k + 1] && g->last[o] >= e[2 * k])
          {
            if (e[2 * k] < g->first[o])
              g->first[o] = e[2 * k], grew = 1;
            if (e[2 * k + 1] > g->last[o])
              g->last[o] = e[2 * k + 1], grew = 1;
          }
      } while (grew);
    }
  }
  ok = 1;

out:
  tcc_free(be);
  tcc_free(sbe);
  tcc_free(pos);
  for (int k = 0; k < 3; k++)
  {
    tcc_free(fl->taint[types[k]]);
    tcc_free(fl->var_addr_taken[types[k]]);
  }
  tcc_free(param_call);
  tcc_free(nocapture);
  tcc_free(call_at);
  tcc_free(param_next);
  tcc_free(call_params);
  return ok;
}

/* A colouring unit: one live object, placed by the bytes it occupies. */
typedef struct
{
  int start, end, align;
  int first[2], last[2];
  int refs;
  uint8_t pinned;
  int new_start;
} FrameUnit;

typedef struct
{
  int start, end;
} FrameSpan;

static int frame_span_start_asc(const void *a, const void *b)
{
  const FrameSpan *x = a, *y = b;
  return x->start < y->start ? -1 : x->start > y->start;
}

/* Place the `nm` movable units in `order` bottom-up: each at the lowest
 * distance above the frame bottom that the units already placed with an
 * overlapping lifetime leave free.  The code addresses the frame from sp,
 * just below it, so the first units placed get the short [sp, #imm]
 * encodings.  The block goes under the lowest pinned unit.  Returns the new
 * frame bottom. */
static int frame_place(FrameUnit *u, int nu, const int *order, int nm, FrameSpan *spans)
{
  int ceiling = 0, height = 0;
  for (int k = 0; k < nu; k++)
    if (u[k].pinned && u[k].start < ceiling)
      ceiling = u[k].start;
  for (int oi = 0; oi < nm; oi++)
  {
    FrameUnit *x = &u[order[oi]];
    int nsp = 0;
    for (int pi = 0; pi < oi; pi++)
    {
      FrameUnit *y = &u[order[pi]];
      /* Disjoint in either order is enough (see frame_lifetimes). */
      if (y->first[0] <= x->last[0] && x->first[0] <= y->last[0] && y->first[1] <= x->last[1] &&
          x->first[1] <= y->last[1])
      {
        spans[nsp].start = y->new_start;
        spans[nsp].end = y->new_start + (y->end - y->start);
        nsp++;
      }
    }
    qsort(spans, nsp, sizeof *spans, frame_span_start_asc);
    int size = x->end - x->start, d = 0;
    /* The bottom is 8-aligned, so a distance keeps the object's alignment
     * when it is congruent with the object's original offset. */
    d += ((x->start - d) % x->align + x->align) % x->align;
    for (int k = 0; k < nsp; k++)
    {
      if (spans[k].start >= d + size)
        break;
      if (spans[k].end <= d)
        continue;
      d = spans[k].end;
      d += ((x->start - d) % x->align + x->align) % x->align;
    }
    x->new_start = d; /* a distance for now */
    if (d + size > height)
      height = d + size;
  }
  int base = (ceiling - height) & ~7;
  for (int oi = 0; oi < nm; oi++)
    u[order[oi]].new_start += base;
  for (int k = 0; k < nu; k++)
    if (u[k].pinned)
      u[k].new_start = u[k].start;
  return base < ceiling ? base : ceiling;
}

/* Hottest first: references per byte, then more references. */
static FrameUnit *frame_units_gl;
static int frame_unit_hot_first(const void *a, const void *b)
{
  const FrameUnit *x = &frame_units_gl[*(const int *)a], *y = &frame_units_gl[*(const int *)b];
  int64_t lx = (int64_t)x->refs * (y->end - y->start), ly = (int64_t)y->refs * (x->end - x->start);
  if (lx != ly)
    return lx > ly ? -1 : 1;
  if (x->refs != y->refs)
    return x->refs > y->refs ? -1 : 1;
  return x->start < y->start ? -1 : x->start > y->start;
}

/* Colour the live segments.  Returns the new bottom, or 1 when colouring does
 * not apply or does worse than packing. */
static int frame_colour(TCCIRState *ir, FrameSeg *seg, int nseg, int bottom)
{
  FrameLive fl = {0};
  fl.ir = ir;
  fl.seg = seg;
  fl.nseg = nseg;
  fl.bottom = bottom;
  fl.n = ir->next_instruction_index;
  for (int s = 0; s < nseg; s++)
  {
    seg[s].first[0] = seg[s].first[1] = INT_MAX;
    seg[s].last[0] = seg[s].last[1] = -1;
  }
  if (!frame_lifetimes(&fl))
    return 1;

  FrameUnit *u = tcc_malloc(sizeof(FrameUnit) * (nseg > 0 ? nseg : 1));
  int nu = 0;
  for (int s = 0; s < nseg; s++)
  {
    FrameSeg *g = &seg[s];
    g->unit = -1;
    if (!g->live || (!g->pinned && g->first[0] > g->last[0]))
      continue;
    FrameUnit *x = &u[nu];
    g->unit = nu++;
    x->start = g->start;
    x->end = g->pinned ? g->end : g->start + g->size;
    x->align = frame_offset_align(g->start);
    x->pinned = g->pinned;
    for (int o = 0; o < 2; o++)
    {
      x->first[o] = g->pinned ? 0 : g->first[o];
      x->last[o] = g->pinned ? fl.n : g->last[o];
    }
    x->refs = g->refs;
    x->new_start = 1;
  }

  int *order = tcc_malloc(sizeof(int) * (nu > 0 ? nu : 1));
  FrameSpan *spans = tcc_malloc(sizeof(FrameSpan) * (nu > 0 ? nu : 1));
  int nm = 0;
  for (int k = 0; k < nu; k++)
    if (!u[k].pinned)
      order[nm++] = k;
  frame_units_gl = u;
  qsort(order, nm, sizeof(int), frame_unit_hot_first);
  frame_units_gl = NULL;
  int placed = frame_place(u, nu, order, nm, spans);

  /* Keep it only if its frame is no bigger than packing live runs in place. */
  FrameSeg *tmp = tcc_malloc(sizeof(FrameSeg) * (nseg > 0 ? nseg : 1));
  memcpy(tmp, seg, sizeof(FrameSeg) * nseg);
  int packed = frame_pack_runs(tmp, nseg);
  tcc_free(tmp);
  int result = 1;
  if (placed >= packed)
  {
    for (int s = 0; s < nseg; s++)
      seg[s].delta = seg[s].unit >= 0 ? u[seg[s].unit].new_start - u[seg[s].unit].start : 0;
    result = placed;
  }
  tcc_free(order);
  tcc_free(spans);
  tcc_free(u);
  return result;
}

int tcc_ir_frame_relayout(TCCIRState *ir, int *ploc)
{
  const int bottom = *ploc;
  /* Once only: after it, the operands no longer match frame_objs. */
  if (!ir || ir->frame_relaid || bottom >= 0 || ir->frame_obj_count == 0 || !frame_function_eligible(ir))
    return 0;
  ir->frame_relaid = 1;

  /* Objects -> disjoint segments tiling [bottom, 0), gaps included. */
  int nobj = ir->frame_obj_count;
  qsort(ir->frame_objs, nobj, sizeof(int32_t) * 4, frame_obj_cmp);
  FrameSeg *seg = tcc_mallocz(sizeof(FrameSeg) * (2 * nobj + 2));
  int nseg = 0, cur = bottom;
  for (int k = 0; k < nobj; k++)
  {
    const int32_t *o = &ir->frame_objs[4 * k];
    int s = o[0], e = s + o[1];
    if (s < bottom || e > 0)
    {
      /* Not wholly inside the frame the optimizer left: keep it where it is. */
      if (e > 0)
        e = 0;
      if (s < bottom)
        s = bottom;
      if (e <= cur)
        continue;
      if (s < cur)
        s = cur;
      if (s > cur)
        seg[nseg++] = (FrameSeg){.start = cur, .end = s, .size = s - cur, .live = 1, .pinned = 1};
      seg[nseg++] = (FrameSeg){.start = s, .end = e, .size = e - s, .live = 1, .pinned = 1};
      cur = e;
      continue;
    }
    if (e <= cur)
      continue; /* inside an earlier object */
    if (s < cur)
    {
      /* Overlapping objects share bytes, so they live and move as one. */
      FrameSeg *g = &seg[nseg - 1];
      g->end = e;
      if (s + o[2] > g->start + g->size)
        g->size = s + o[2] - g->start;
      g->kind &= o[3];
      cur = e;
      continue;
    }
    if (s > cur)
      seg[nseg++] = (FrameSeg){.start = cur, .end = s, .size = s - cur, .live = 1, .pinned = 1};
    seg[nseg++] = (FrameSeg){.start = s, .end = e, .size = o[2] < e - s ? o[2] : e - s, .kind = (uint8_t)o[3]};
    cur = e;
  }
  if (cur < 0)
    seg[nseg++] = (FrameSeg){.start = cur, .end = 0, .size = -cur, .live = 1, .pinned = 1};

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

  int new_top = 1;
  if (tcc_state->optimize > 0 && !tcc_ir_opt_pass_disabled("frame_colour") && !ir->func_has_label_addr &&
      !tcc_ir_calls_returns_twice(ir))
  {
    int ijump = 0;
    for (int i = 0; i < ir->next_instruction_index; i++)
      ijump |= ir->compact_instructions[i].op == TCCIR_OP_IJUMP;
    if (!ijump)
      new_top = frame_colour(ir, seg, nseg, bottom);
  }
  if (new_top > 0)
  {
    int dead = 0;
    for (int s = 0; s < nseg; s++)
      dead += !seg[s].live;
    if (!dead)
    {
      tcc_free(seg);
      return 0;
    }
    new_top = frame_pack_runs(seg, nseg);
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
  int new_bottom = new_top & ~7;
  if (new_bottom <= bottom)
    return 0;
  *ploc = new_bottom;
  return new_bottom - bottom;
}
