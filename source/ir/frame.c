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
 *     and places objects with disjoint lifetimes on the same bytes.
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
  int first, last; /* lifetime in instruction indices, first > last if none */
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
 * An object is live from its first to its last reference in instruction order,
 * where a reference is any operand naming it or holding a pointer into it.
 * Pointers are followed through vregs (flow-insensitively: a vreg that may
 * point to two objects gives up on both).  An object whose address escapes --
 * stored to memory, passed to a call, returned, into a vreg held in memory --
 * stays live to the end of the function.  The address of a call temporary
 * passed to its call, or of any object passed as a struct-return buffer, is not
 * an escape: the callee cannot keep it.  Every range that intersects a loop
 * (a backward jump) is widened to the whole loop, until no range changes.
 *
 * Two objects whose ranges are disjoint may then share bytes: any execution
 * path from a reference of the later one back to the earlier one crosses a
 * backward jump whose loop the later one's range already covers. */

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

static void frame_use(FrameLive *fl, int s, int i)
{
  if (s < 0)
    return;
  if (i < fl->seg[s].first)
    fl->seg[s].first = i;
  if (i > fl->seg[s].last)
    fl->seg[s].last = i;
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
  for (int c = 0; c < ncall; c++)
    call_at[c] = -1;
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
          int t = frame_op_taint(fl, tcc_ir_op_get_src1(ir, &ir->compact_instructions[p]));
          if (t >= 0 && fl->seg[t].kind == FRAME_OBJ_RET_TEMP)
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
      frame_use(fl, t, i);
      /* A parameter is read by its call. */
      if (op == TCCIR_OP_FUNCPARAMVAL)
        frame_use(fl, t, param_call[i]);
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
        if (!fl->seg[t].kind && !sret)
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
    if (fl->seg[s].escaped && fl->seg[s].first <= fl->seg[s].last)
      fl->seg[s].last = n - 1;

  /* Widen every range over the loops it intersects. */
  int nbe = 0, cap = 16;
  int *be = tcc_malloc(sizeof(int) * 2 * cap);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int targets[2], nt = 0;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      targets[nt++] = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    else if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      int tid = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (tid < 0 || tid >= ir->num_switch_tables)
      {
        tcc_free(be);
        goto out;
      }
      TCCIRSwitchTable *tab = &ir->switch_tables[tid];
      int lo = tab->default_target;
      for (int t = 0; t < tab->num_entries; t++)
        if (tab->targets[t] < lo || lo < 0)
          lo = tab->targets[t];
      targets[nt++] = lo;
    }
    for (int k = 0; k < nt; k++)
    {
      if (targets[k] < 0 || targets[k] > i)
        continue;
      if (nbe == cap)
      {
        cap *= 2;
        be = tcc_realloc(be, sizeof(int) * 2 * cap);
      }
      be[2 * nbe] = targets[k];
      be[2 * nbe + 1] = i;
      nbe++;
    }
  }
  for (int s = 0; s < fl->nseg; s++)
  {
    FrameSeg *g = &fl->seg[s];
    if (g->first > g->last)
      continue;
    int grew;
    do
    {
      grew = 0;
      for (int b = 0; b < nbe; b++)
        if (g->first <= be[2 * b + 1] && g->last >= be[2 * b])
        {
          if (be[2 * b] < g->first)
            g->first = be[2 * b], grew = 1;
          if (be[2 * b + 1] > g->last)
            g->last = be[2 * b + 1], grew = 1;
        }
    } while (grew);
  }
  tcc_free(be);
  ok = 1;

out:
  for (int k = 0; k < 3; k++)
  {
    tcc_free(fl->taint[types[k]]);
    tcc_free(fl->var_addr_taken[types[k]]);
  }
  tcc_free(param_call);
  tcc_free(call_at);
  tcc_free(param_next);
  tcc_free(call_params);
  return ok;
}

/* A colouring unit: one live object, placed by the bytes it occupies. */
typedef struct
{
  int start, end, align;
  int first, last;
  uint8_t pinned;
  int new_start;
} FrameUnit;

typedef struct
{
  int start, end;
} FrameSpan;

static int frame_span_end_desc(const void *a, const void *b)
{
  const FrameSpan *x = a, *y = b;
  return x->end > y->end ? -1 : x->end < y->end;
}

/* Place the `nm` movable units in `order`, each as high as the units already
 * placed with an overlapping lifetime allow.  Returns the frame bottom. */
static int frame_place(FrameUnit *u, int nu, const int *order, int nm, FrameSpan *spans)
{
  int bottom = 0;
  for (int k = 0; k < nu; k++)
    if (u[k].pinned)
    {
      u[k].new_start = u[k].start;
      if (u[k].start < bottom)
        bottom = u[k].start;
    }
  for (int oi = 0; oi < nm; oi++)
  {
    FrameUnit *x = &u[order[oi]];
    int nsp = 0;
    /* Units placed so far: the pinned ones and those earlier in `order`. */
    for (int k = 0; k < nu; k++)
    {
      FrameUnit *y = &u[k];
      if (y == x || y->new_start > 0)
        continue;
      if (y->first <= x->last && x->first <= y->last)
      {
        spans[nsp].start = y->new_start;
        spans[nsp].end = y->new_start + (y->end - y->start);
        nsp++;
      }
    }
    qsort(spans, nsp, sizeof *spans, frame_span_end_desc);
    int size = x->end - x->start, top = 0, cand = 0;
    for (int k = 0;; k++)
    {
      int r = ((top - size - x->start) % x->align + x->align) % x->align;
      cand = top - size - r;
      if (k == nsp || spans[k].end <= cand)
        break;
      if (spans[k].start >= cand + size)
        continue;
      if (spans[k].start < top)
        top = spans[k].start;
    }
    x->new_start = cand;
    if (cand < bottom)
      bottom = cand;
  }
  return bottom;
}

static FrameUnit *frame_units_gl;
static int frame_unit_size_desc(const void *a, const void *b)
{
  const FrameUnit *x = &frame_units_gl[*(const int *)a], *y = &frame_units_gl[*(const int *)b];
  int sx = x->end - x->start, sy = y->end - y->start;
  if (sx != sy)
    return sx > sy ? -1 : 1;
  return x->first < y->first ? -1 : x->first > y->first;
}
static int frame_unit_first_asc(const void *a, const void *b)
{
  const FrameUnit *x = &frame_units_gl[*(const int *)a], *y = &frame_units_gl[*(const int *)b];
  if (x->first != y->first)
    return x->first < y->first ? -1 : 1;
  int sx = x->end - x->start, sy = y->end - y->start;
  return sx > sy ? -1 : sx < sy;
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
    seg[s].first = INT_MAX;
    seg[s].last = -1;
  }
  if (!frame_lifetimes(&fl))
    return 1;

  FrameUnit *u = tcc_malloc(sizeof(FrameUnit) * (nseg > 0 ? nseg : 1));
  int nu = 0;
  for (int s = 0; s < nseg; s++)
  {
    FrameSeg *g = &seg[s];
    g->unit = -1;
    if (!g->live || (!g->pinned && g->first > g->last))
      continue;
    FrameUnit *x = &u[nu];
    g->unit = nu++;
    x->start = g->start;
    x->end = g->pinned ? g->end : g->start + g->size;
    x->align = frame_offset_align(g->start);
    x->pinned = g->pinned;
    x->first = g->pinned ? 0 : g->first;
    x->last = g->pinned ? fl.n : g->last;
    x->new_start = 1;
  }

  int *order = tcc_malloc(sizeof(int) * (nu > 0 ? nu : 1));
  int *best = tcc_malloc(sizeof(int) * (nu > 0 ? nu : 1));
  FrameSpan *spans = tcc_malloc(sizeof(FrameSpan) * (nu > 0 ? nu : 1));
  int nm = 0;
  for (int k = 0; k < nu; k++)
    if (!u[k].pinned)
      order[nm++] = k;
  int result = 1, best_bottom = INT_MIN;
  frame_units_gl = u;
  for (int pass = 0; pass < 2; pass++)
  {
    qsort(order, nm, sizeof(int), pass == 0 ? frame_unit_size_desc : frame_unit_first_asc);
    for (int k = 0; k < nu; k++)
      if (!u[k].pinned)
        u[k].new_start = 1;
    int b = frame_place(u, nu, order, nm, spans);
    if (b > best_bottom)
    {
      best_bottom = b;
      for (int k = 0; k < nu; k++)
        best[k] = u[k].new_start;
    }
  }
  frame_units_gl = NULL;

  /* Keep it only if it beats packing live runs in place. */
  FrameSeg *tmp = tcc_malloc(sizeof(FrameSeg) * (nseg > 0 ? nseg : 1));
  memcpy(tmp, seg, sizeof(FrameSeg) * nseg);
  int packed = frame_pack_runs(tmp, nseg);
  tcc_free(tmp);
  if (best_bottom > packed)
  {
    for (int s = 0; s < nseg; s++)
      seg[s].delta = seg[s].unit >= 0 ? best[seg[s].unit] - u[seg[s].unit].start : 0;
    result = best_bottom;
  }
  tcc_free(order);
  tcc_free(best);
  tcc_free(spans);
  tcc_free(u);
  return result;
}

int tcc_ir_frame_relayout(TCCIRState *ir, int *ploc)
{
  const int bottom = *ploc;
  if (!ir || bottom >= 0 || ir->frame_obj_count == 0 || !frame_function_eligible(ir))
    return 0;

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
