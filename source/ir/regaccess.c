/*
 *  TCC - Tiny C Compiler
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 */

/* yasc register fields: a write-1-to-clear field written back as it was read.
 *
 *   typedef union { uint32_t raw; struct { uint32_t en:1, irq:1 w1c, ...; } b; } ctrl_t;
 *   ctrl_t v = *r;  v.b.en = 1;  *r = v;      -- irq read as 1 is written back: cleared
 *   regs->status |= X;                        -- `status w1c`: every pending bit cleared
 *
 * The frontend notes each store to volatile memory whose type (or the field
 * stored) has w1c bits (vstore.c, tcc_ir_note_w1c).  Here, on the raw IR
 * before any pass (the same at every -O), each value carries the bits that
 * still hold what a volatile read gave, where it gave them: a read of
 * volatile memory gives all; AND with a constant keeps the bits it keeps, OR
 * with a constant takes the bits it sets out (both are known after it), XOR
 * with one the bits it flips; AND / OR / XOR of two values may keep either's
 * bits; a shift by a constant other than 0 moves every bit away from where
 * it was read, and any other operation (arithmetic, a compare, a bit-field
 * extract) makes a new value: none.  Locals follow it through their frame words and
 * variables, a join is the union.  A noted store whose value still holds a
 * w1c bit as read is said (-Wregister-access, strict; unsafe lowers it).
 * Parameters and other memory carry nothing: what the caller or a buffer
 * gives is theirs. */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"

#define RA_MAX_LOCS 256

void tcc_ir_note_w1c(TCCIRState *ir, const IRW1cNote *note)
{
  if (note->from >= note->to)
    return;
  ir->w1c_notes = tcc_realloc(ir->w1c_notes, (ir->nb_w1c_notes + 1) * sizeof *ir->w1c_notes);
  ir->w1c_notes[ir->nb_w1c_notes++] = *note;
}

void tcc_ir_w1c_free(TCCIRState *ir)
{
  tcc_free(ir->w1c_notes);
  ir->w1c_notes = NULL;
  ir->nb_w1c_notes = 0;
  tcc_free(ir->w1c_vcalls);
  ir->w1c_vcalls = NULL;
  ir->nb_w1c_vcalls = 0;
}

void tcc_ir_note_vcall(TCCIRState *ir, int call_id)
{
  if (ir->nb_w1c_vcalls && ir->w1c_vcalls[ir->nb_w1c_vcalls - 1] == call_id)
    return;
  ir->w1c_vcalls = tcc_realloc(ir->w1c_vcalls, (ir->nb_w1c_vcalls + 1) * sizeof *ir->w1c_vcalls);
  ir->w1c_vcalls[ir->nb_w1c_vcalls++] = call_id;
}

/* the call was handed a pointer to volatile memory: what it gives back --
   its result, what it writes through a local's address -- may be a read */
static int ra_vcall(const TCCIRState *ir, int call_id)
{
  for (int k = 0; k < ir->nb_w1c_vcalls; k++)
    if (ir->w1c_vcalls[k] == call_id)
      return 1;
  return 0;
}

/* the frame object (offset range) holding frame offset OFF: [*lo, *hi) */
static int ra_frame_obj(const TCCIRState *ir, int32_t off, int32_t *lo, int32_t *hi)
{
  for (int k = 0; k < ir->frame_obj_count; k++)
  {
    const int32_t *o = &ir->frame_objs[FRAME_OBJ_WORDS * k];
    if (o[0] <= off && off < o[0] + (o[1] > 0 ? o[1] : 1))
      return *lo = o[0], *hi = o[0] + (o[1] > 0 ? o[1] : 1), 1;
  }
  return 0;
}

typedef struct RaCtx
{
  TCCIRState *ir;
  IRCFG *cfg;
  int32_t loc[RA_MAX_LOCS]; /* a frame word (its offset, kind 0) or a variable (its vreg, kind 1) */
  uint8_t lkind[RA_MAX_LOCS];
  int nloc;
  uint32_t *tt; /* temp position -> its bits as read */
  int ntemp;
  int tgrew; /* a temp's bits grew in this round */
} RaCtx;

static int ra_loc(RaCtx *c, int kind, int32_t id, int add)
{
  for (int i = 0; i < c->nloc; i++)
    if (c->lkind[i] == kind && c->loc[i] == id)
      return i;
  if (!add || c->nloc == RA_MAX_LOCS)
    return -1;
  c->lkind[c->nloc] = kind, c->loc[c->nloc] = id;
  return c->nloc++;
}

/* the definition of temp VR before instruction AT in its block */
static int ra_def(TCCIRState *ir, int from, int at, int32_t vr)
{
  for (int j = at - 1; j >= from && j >= at - 32; j--)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!irop_is_none(d) && irop_dest_defines_vreg(d) && irop_get_vreg(d) == vr)
      return j;
  }
  return -1;
}

/* the memory operand OP (at I) is a frame word: its offset (word aligned), else -1 */
static int32_t ra_frame_word(RaCtx *c, int i, IROperand op)
{
  TCCIRState *ir = c->ir;
  if (irop_is_none(op) || !op.is_lval)
    return -1;
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_is_local(op) && !irop_is_llocal(op))
    return (int32_t)irop_get_imm64_ex(ir, op) & ~3;
  int32_t vr = irop_get_vreg(op), acc = 0;
  int from = c->cfg->blocks[c->cfg->instr_to_block[i]].start_idx, at = i;
  for (int d = 0; d < 6 && vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP; d++)
  {
    int j = ra_def(ir, from, at, vr);
    if (j < 0)
      return -1;
    IRQuadCompact *q = &ir->compact_instructions[j];
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA) && irop_get_tag(s1) == IROP_TAG_STACKOFF &&
        irop_is_local(s1) && !irop_is_llocal(s1) && (q->op == TCCIR_OP_LEA || !s1.is_lval))
      return ((int32_t)irop_get_imm64_ex(ir, s1) + acc) & ~3;
    if (q->op == TCCIR_OP_ADD && !irop_is_none(s2) && irop_is_immediate(s2) && !s2.is_lval)
      acc += (int32_t)irop_get_imm64_ex(ir, s2), vr = irop_get_vreg(s1);
    else if (q->op == TCCIR_OP_ASSIGN && !s1.is_lval)
      vr = irop_get_vreg(s1);
    else
      return -1;
    at = j;
  }
  return -1;
}

/* the address OP (at I, not an lvalue) is: a base (a non-temp vreg, a
   symbol, a constant) plus *OFF; 0 not followed */
static intptr_t ra_addr(RaCtx *c, int i, IROperand op, int32_t *off)
{
  TCCIRState *ir = c->ir;
  int from = c->cfg->blocks[c->cfg->instr_to_block[i]].start_idx, at = i;
  *off = 0;
  for (int d = 0; d < 8 && !irop_is_none(op); d++)
  {
    if (irop_get_tag(op) == IROP_TAG_SYMREF)
    {
      IRPoolSymref *r = irop_get_symref_ex(ir, op);
      return r && r->sym ? (*off += r->addend, (intptr_t)r->sym) : 0;
    }
    if (irop_is_immediate(op))
      return *off += (int32_t)irop_get_imm64_ex(ir, op), 1;
    int32_t vr = irop_get_vreg(op);
    if (vr < 0 || op.is_lval)
      return 0;
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 2 + (intptr_t)(uint32_t)vr * 4;
    int j = ra_def(ir, from, at, vr);
    if (j < 0)
      return 0;
    IRQuadCompact *q = &ir->compact_instructions[j];
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    if (q->op == TCCIR_OP_ASSIGN)
      op = s1;
    else if (q->op == TCCIR_OP_ADD && !irop_is_none(s2) && irop_is_immediate(s2) && !s2.is_lval)
      *off += (int32_t)irop_get_imm64_ex(ir, s2), op = s1;
    else
      return 0;
    at = j;
  }
  return 0;
}

/* the bits of OP (at I) that still hold what a volatile read gave */
static uint32_t ra_value(RaCtx *c, const uint32_t *st, int i, IROperand op)
{
  if (irop_is_none(op) || (irop_is_immediate(op) && !op.is_lval))
    return 0;
  if (op.is_lval)
  {
    /* a variable's home slot is the variable */
    int32_t vr = irop_get_vreg(op);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && irop_get_tag(op) == IROP_TAG_STACKOFF)
    {
      int l = ra_loc(c, 1, vr, 0);
      return l >= 0 ? st[l] : 0;
    }
    int32_t fw = ra_frame_word(c, i, op);
    if (fw != -1)
    {
      int l = ra_loc(c, 0, fw, 0);
      return l >= 0 ? st[l] : 0;
    }
    return irop_access_is_volatile(op) ? ~0u : 0; /* a register read */
  }
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
  {
    int p = TCCIR_DECODE_VREG_POSITION(vr);
    return p < c->ntemp ? c->tt[p] : 0;
  }
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
  {
    int l = ra_loc(c, 1, vr, 0);
    return l >= 0 ? st[l] : 0;
  }
  return 0;
}

static uint32_t ra_result(RaCtx *c, const uint32_t *st, int i, IRQuadCompact *q)
{
  TCCIRState *ir = c->ir;
  const IRRegistersConfig *rc = &irop_config[q->op];
  IROperand s1 = rc->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
  IROperand s2 = rc->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
  uint32_t a = ra_value(c, st, i, s1), b = ra_value(c, st, i, s2);
  int k1 = !irop_is_none(s1) && irop_is_immediate(s1) && !s1.is_lval;
  int k2 = !irop_is_none(s2) && irop_is_immediate(s2) && !s2.is_lval;
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return a;
  case TCCIR_OP_FUNCCALLVAL: /* the result of a call that may read a register */
    return k2 && ra_vcall(ir, TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, s2))) ? ~0u : 0;
  case TCCIR_OP_AND:
    if (k2)
      return a & (uint32_t)irop_get_imm64_ex(ir, s2);
    if (k1)
      return b & (uint32_t)irop_get_imm64_ex(ir, s1);
    return a | b;
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    if (k2)
      return a & ~(uint32_t)irop_get_imm64_ex(ir, s2);
    if (k1)
      return b & ~(uint32_t)irop_get_imm64_ex(ir, s1);
    return a | b;
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
    return k2 && irop_get_imm64_ex(ir, s2) == 0 ? a : 0;
  case TCCIR_OP_ZEXT:
    return a;
  default:
    return 0;
  }
}

/* the word of noted store N that the store at I (destination D) writes:
   its offset from the lowest store the note's range makes */
static int ra_word(RaCtx *c, const IRW1cNote *n, int i, IROperand d)
{
  TCCIRState *ir = c->ir;
  int32_t off, lowest = 0x7fffffff, o;
  IROperand a = d;
  a.is_lval = 0;
  intptr_t base = ra_addr(c, i, a, &off);
  if (!base)
    return 0;
  for (int j = n->from; j < n->to; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dj = tcc_ir_op_get_dest(ir, q);
    if (irop_is_none(dj) || !dj.is_lval || irop_dest_defines_vreg(dj))
      continue;
    dj.is_lval = 0;
    if (ra_addr(c, j, dj, &o) == base && o < lowest)
      lowest = o;
  }
  return lowest == 0x7fffffff || off < lowest ? 0 : (off - lowest) / 4;
}

/* the value with bits V as read written to word W of noted store N, at LINE */
static void ra_say(RaCtx *c, const IRW1cNote *n, int w, uint32_t v, int line)
{
  if (w < 0 || w >= n->nw || !(v & n->wmask[w]))
    return;
  for (int f = 0; f < n->nf; f++)
    if (n->fword[f] == w && (v & n->fbits[f]))
      tcc_warning_c(warn_register_access)("%i:w1c field '%s' is written back as it was read, which clears it: set "
                                          "it to 0 to keep it (1 clears it)",
                                          line, get_tok_str(n->ftok[f], NULL));
}

/* one instruction; REPORT: say the noted stores that write w1c bits back */
static void ra_instr(RaCtx *c, uint32_t *st, int i, int report)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  const IRRegistersConfig *rc = &irop_config[q->op];
  /* a register struct copied through memmove: from a local (a noted
     store: its words are checked) or into one (vread: it holds what was
     read) */
  if (q->op == TCCIR_OP_FUNCPARAMVAL)
  {
    IROperand a = tcc_ir_op_get_src1(ir, q);
    int32_t lo, hi, fw = -1;
    if (!irop_is_none(a) && !a.is_lval)
    {
      IROperand m = a;
      m.is_lval = 1;
      fw = ra_frame_word(c, i, m);
    }
    for (int k = 0; fw != -1 && k < ir->nb_w1c_notes; k++)
    {
      const IRW1cNote *n = &ir->w1c_notes[k];
      if (i < n->from || i >= n->to || !ra_frame_obj(ir, fw, &lo, &hi))
        continue;
      for (int l = 0; l < c->nloc; l++)
      {
        if (c->lkind[l] != 0 || c->loc[l] < (lo & ~3) || c->loc[l] >= hi)
          continue;
        if (n->vread)
          st[l] = ~0u;
        else if (report)
          ra_say(c, n, (c->loc[l] - lo) / 4, st[l], q->line_num);
      }
    }
  }
  /* a local's address handed to a call that may read a register: the
     local may hold what it read */
  if (q->op == TCCIR_OP_FUNCPARAMVAL && ir->nb_w1c_vcalls &&
      ra_vcall(ir, TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)))))
  {
    IROperand a = tcc_ir_op_get_src1(ir, q);
    int32_t fw = -1, lo, hi;
    if (!irop_is_none(a) && !a.is_lval)
    {
      IROperand m = a;
      m.is_lval = 1; /* the word it points at */
      fw = ra_frame_word(c, i, m);
    }
    if (fw != -1 && ra_frame_obj(ir, fw, &lo, &hi))
      for (int l = 0; l < c->nloc; l++)
        if (c->lkind[l] == 0 && c->loc[l] >= (lo & ~3) && c->loc[l] < hi)
          st[l] = ~0u;
    return;
  }
  if (!rc->has_dest)
    return;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (irop_is_none(d))
    return;
  if (irop_dest_defines_vreg(d))
  {
    int32_t vr = irop_get_vreg(d);
    uint32_t v = ra_result(c, st, i, q);
    if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p < c->ntemp && (c->tt[p] | v) != c->tt[p])
        c->tt[p] |= v, c->tgrew = 1;
    }
    else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int l = ra_loc(c, 1, vr, 0);
      if (l >= 0)
        st[l] = v;
    }
    return;
  }
  if (!d.is_lval)
    return;
  /* a store */
  uint32_t v = ra_value(c, st, i, rc->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none());
  int32_t fw = ra_frame_word(c, i, d);
  if (fw != -1)
  {
    int l = ra_loc(c, 0, fw, 0);
    if (l >= 0)
      st[l] = v;
    return;
  }
  if (!report || !v)
    return;
  for (int k = 0; k < ir->nb_w1c_notes; k++)
  {
    const IRW1cNote *n = &ir->w1c_notes[k];
    if (i < n->from || i >= n->to || n->vread)
      continue;
    ra_say(c, n, ra_word(c, n, i, d), v, q->line_num);
  }
}

static int ra_in_inline(const TCCIRState *ir, int i)
{
  for (int k = 0; k < ir->lt_nb_inl; k++)
    if (i >= ir->lt_inl[2 * k] && i < ir->lt_inl[2 * k + 1])
      return 1;
  return 0;
}

void tcc_ir_w1c_check(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  IRCFG *cfg;
  if (!ir->nb_w1c_notes)
  {
    tcc_ir_w1c_free(ir);
    return;
  }
  if (!n || !(cfg = tcc_ir_cfg_build(ir)))
  {
    tcc_ir_w1c_free(ir);
    return;
  }
  RaCtx c;
  memset(&c, 0, sizeof c);
  c.ir = ir, c.cfg = cfg;
  /* temps and locations */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_is_none(d) ? -1 : irop_get_vreg(d);
    if (vr >= 0 && irop_dest_defines_vreg(d) && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
        TCCIR_DECODE_VREG_POSITION(vr) >= c.ntemp)
      c.ntemp = TCCIR_DECODE_VREG_POSITION(vr) + 1;
    else if (vr >= 0 && irop_dest_defines_vreg(d) && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      ra_loc(&c, 1, vr, 1);
    else if (!irop_is_none(d) && d.is_lval && !irop_dest_defines_vreg(d))
    {
      int32_t fw = ra_frame_word(&c, i, d);
      if (fw != -1)
        ra_loc(&c, 0, fw, 1);
    }
  }
  /* every word of a local a register struct is copied into or out of
     through memmove (a noted range's frame address argument) */
  for (int k = 0; k < ir->nb_w1c_notes; k++)
    for (int i = ir->w1c_notes[k].from; i < ir->w1c_notes[k].to && i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCPARAMVAL)
        continue;
      IROperand a = tcc_ir_op_get_src1(ir, q);
      int32_t lo, hi, fw;
      if (irop_is_none(a) || a.is_lval)
        continue;
      a.is_lval = 1;
      if ((fw = ra_frame_word(&c, i, a)) == -1 || !ra_frame_obj(ir, fw, &lo, &hi))
        continue;
      for (int32_t w = lo & ~3; w < hi && w < (lo & ~3) + 4 * IR_W1C_WORDS; w += 4)
        ra_loc(&c, 0, w, 1);
    }
  c.tt = tcc_mallocz((c.ntemp + 1) * sizeof *c.tt);
  int nb = cfg->num_blocks, nl = c.nloc > 0 ? c.nloc : 1;
  uint32_t *in = tcc_mallocz((size_t)nb * nl * sizeof *in), *cur = tcc_malloc(nl * sizeof *cur);
  uint8_t *seen = tcc_mallocz(nb + 1), *queued = tcc_mallocz(nb + 1);
  int *work = tcc_malloc((nb + 1) * sizeof *work), nw = 0;
  if (nb)
    seen[0] = queued[0] = 1, work[nw++] = 0;
  /* the temps' bits grow as the walk goes on: walk until nothing changes */
  for (int round = 0; round < 8; round++)
  {
    c.tgrew = 0;
    while (nw)
    {
      int b = work[--nw];
      queued[b] = 0;
      memcpy(cur, in + (size_t)b * nl, nl * sizeof *cur);
      for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++)
        ra_instr(&c, cur, i, 0);
      for (int s = 0; s < cfg->blocks[b].num_succs; s++)
      {
        int sb = cfg->blocks[b].succs[s], changed = !seen[sb];
        uint32_t *si = in + (size_t)sb * nl;
        for (int l = 0; l < c.nloc; l++)
          if ((si[l] | cur[l]) != si[l])
            si[l] |= cur[l], changed = 1;
        seen[sb] = 1;
        if (changed && !queued[sb])
          queued[sb] = 1, work[nw++] = sb;
      }
    }
    if (!c.tgrew)
      break;
    for (int b = 0; b < nb; b++)
      if (seen[b] && !queued[b])
        queued[b] = 1, work[nw++] = b;
  }
  /* the reporting walk */
  tcc_state->diag_noabort++;
  for (int b = 0; b < nb; b++)
  {
    if (!seen[b])
      continue;
    memcpy(cur, in + (size_t)b * nl, nl * sizeof *cur);
    for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++)
      ra_instr(&c, cur, i, !ra_in_inline(ir, i));
  }
  tcc_state->diag_noabort--;
  tcc_free(in), tcc_free(cur), tcc_free(seen), tcc_free(queued), tcc_free(work), tcc_free(c.tt);
  tcc_ir_w1c_free(ir);
  tcc_ir_cfg_free(cfg);
}
