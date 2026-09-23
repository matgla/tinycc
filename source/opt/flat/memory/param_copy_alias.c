/*
 *  TCC IR - Struct parameter copy aliasing (flat, pre-SSA)
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
#include "opt_engine.h"

/* A by-value struct parameter belongs to the callee: its words sit in memory
 * the callee may read and write at will -- the caller's outgoing area for a
 * parameter passed on the stack (or split across r3 and the stack), a home slot
 * the prologue stores the registers into for one passed in registers.  The Zig
 * C backend copies every such parameter into a local before using it:
 *
 *   t0 = a1;  t1 = &t0;  ... t1->field ...
 *
 * which tcc lowers to a word-by-word LOAD/STORE chain, or a memmove call for a
 * large one.  When the copy fills the whole local in the entry block, before
 * anything else touches the local, and nothing but the copy ever reads the
 * parameter, the local can simply be the parameter: every reference to the
 * local is renamed onto the parameter's memory and the copy is dropped.  Writes
 * through the local then land in the parameter, which nothing else reads. */

#define PCA_MAX_WORDS 256

typedef struct PcaCopy
{
  int first, last;   /* instruction range of the copy (inclusive) */
  int32_t dst, size; /* local destination [dst, dst + size) */
  int32_t src;       /* source offset */
  IROperand tmpl;    /* a source operand: the parameter's encoding */
  int nins;
  int entry_only; /* the copy sits in the entry block, where any shape is safe */
  int ins[3 * PCA_MAX_WORDS + 8]; /* instructions the copy consists of */
} PcaCopy;

static int pca_nops(int op)
{
  int n = irop_config[op].has_dest + irop_config[op].has_src1 + irop_config[op].has_src2;
  if (op == TCCIR_OP_MLA || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_SELECT)
    n++;
  return n;
}

/* A concrete frame slot of the function's own frame (no vreg, not a
 * parameter). */
static int pca_is_local_slot(IROperand op, int32_t *off)
{
  if (irop_is_none(op) || irop_get_tag(op) != IROP_TAG_STACKOFF || !op.is_local || op.is_param ||
      irop_get_vreg(op) >= 0)
    return 0;
  *off = irop_get_stack_offset(op);
  return 1;
}

/* The next non-NOP instruction at or after `j` before `limit`, or -1. */
static int pca_next(TCCIRState *ir, int j, int limit)
{
  for (; j < limit; j++)
    if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
      return j;
  return -1;
}

static int pca_word_btype(IROperand op)
{
  return op.btype == IROP_BTYPE_INT32 && !op.is_complex;
}

static int pca_temp_uses(TCCIRState *ir, int32_t vr)
{
  int uses = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int nops = pca_nops(q->op);
    for (int k = 0; k < nops; k++)
      uses += irop_get_vreg(ir->iroperand_pool[q->operand_base + k]) == vr;
  }
  return uses;
}

/* LOAD T <- S+4k ; STORE L+4k <- T, repeated, from `i`. */
static int pca_match_chain(TCCIRState *ir, int i, int limit, PcaCopy *c)
{
  int n = 0;
  int j = i, last = i;
  while (n < PCA_MAX_WORDS)
  {
    int js;
    if (j < 0 || (js = pca_next(ir, j + 1, limit)) < 0)
      break;
    IRQuadCompact *lq = &ir->compact_instructions[j], *sq = &ir->compact_instructions[js];
    if (lq->op != TCCIR_OP_LOAD || sq->op != TCCIR_OP_STORE)
      break;
    IROperand s = tcc_ir_op_get_src1(ir, lq), t = tcc_ir_op_get_dest(ir, lq);
    IROperand d = tcc_ir_op_get_dest(ir, sq), v = tcc_ir_op_get_src1(ir, sq);
    int32_t doff;
    if (irop_get_tag(s) != IROP_TAG_STACKOFF || !s.is_local || !s.is_lval || s.is_llocal || !pca_word_btype(s))
      break;
    if (!pca_is_local_slot(d, &doff) || !d.is_lval || !pca_word_btype(d))
      break;
    int32_t tv = irop_get_vreg(t);
    if (tv < 0 || TCCIR_DECODE_VREG_TYPE(tv) != TCCIR_VREG_TYPE_TEMP || irop_get_vreg(v) != tv || v.is_lval)
      break;
    int32_t soff = irop_get_stack_offset(s);
    if (n == 0)
    {
      c->tmpl = s;
      c->src = soff;
      c->dst = doff;
    }
    else if (soff != c->src + 4 * n || doff != c->dst + 4 * n || s.is_param != c->tmpl.is_param ||
             irop_get_vreg(s) != irop_get_vreg(c->tmpl))
      break;
    if (pca_temp_uses(ir, tv) != 2)
      break;
    c->ins[c->nins++] = j;
    c->ins[c->nins++] = js;
    n++;
    last = js;
    j = pca_next(ir, js + 1, limit);
  }
  if (n < 2)
    return 0;
  c->first = i;
  c->last = last;
  c->size = 4 * n;
  return 1;
}

/* A single-use TEMP defined at `k` as the address of a stack slot. */
static int pca_addr_temp(TCCIRState *ir, int k, int32_t tv, IROperand *slot)
{
  IRQuadCompact *q = &ir->compact_instructions[k];
  if ((q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ASSIGN) || irop_get_vreg(tcc_ir_op_get_dest(ir, q)) != tv)
    return 0;
  IROperand a = tcc_ir_op_get_src1(ir, q);
  if (irop_get_tag(a) != IROP_TAG_STACKOFF || !a.is_local || a.is_lval || a.is_llocal)
    return 0;
  *slot = a;
  return 1;
}

static int pca_is_copy_fn(TCCIRState *ir, IRQuadCompact *call)
{
  Sym *s = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, call));
  if (!s)
    return 0;
  const char *nm = get_tok_str(s->v, NULL);
  return !strcmp(nm, "memmove") || !strcmp(nm, "memcpy") || !strcmp(nm, "__aeabi_memmove") ||
         !strcmp(nm, "__aeabi_memmove4") || !strcmp(nm, "__aeabi_memmove8") || !strcmp(nm, "__aeabi_memcpy") ||
         !strcmp(nm, "__aeabi_memcpy4") || !strcmp(nm, "__aeabi_memcpy8");
}

/* Ta <- &L ; Tb <- &S ; PARAM0 Ta ; PARAM1 Tb ; PARAM2 #size ; CALL memmove
 * (the two address definitions may come in either order). */
static int pca_match_call(TCCIRState *ir, int i, int limit, PcaCopy *c)
{
  int at[6];
  at[0] = i;
  for (int k = 1; k < 6; k++)
    if ((at[k] = pca_next(ir, at[k - 1] + 1, limit)) < 0)
      return 0;
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ASSIGN)
    return 0;
  IRQuadCompact *p0 = &ir->compact_instructions[at[2]], *p1 = &ir->compact_instructions[at[3]],
                *p2 = &ir->compact_instructions[at[4]], *call = &ir->compact_instructions[at[5]];
  if (p0->op != TCCIR_OP_FUNCPARAMVAL || p1->op != TCCIR_OP_FUNCPARAMVAL || p2->op != TCCIR_OP_FUNCPARAMVAL ||
      call->op != TCCIR_OP_FUNCCALLVOID || !pca_is_copy_fn(ir, call))
    return 0;
  int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call)));
  IRQuadCompact *ps[3] = {p0, p1, p2};
  for (int k = 0; k < 3; k++)
  {
    uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, ps[k]));
    if (TCCIR_DECODE_CALL_ID(enc) != call_id || TCCIR_DECODE_PARAM_IDX(enc) != k)
      return 0;
  }
  IROperand a0 = tcc_ir_op_get_src1(ir, p0), a1 = tcc_ir_op_get_src1(ir, p1), a2 = tcc_ir_op_get_src1(ir, p2);
  int32_t t0 = irop_get_vreg(a0), t1 = irop_get_vreg(a1);
  if (t0 < 0 || t1 < 0 || a0.is_lval || a1.is_lval || irop_get_tag(a2) != IROP_TAG_IMM32)
    return 0;
  IROperand ld, ls;
  if (!(pca_addr_temp(ir, at[0], t0, &ld) && pca_addr_temp(ir, at[1], t1, &ls)) &&
      !(pca_addr_temp(ir, at[1], t0, &ld) && pca_addr_temp(ir, at[0], t1, &ls)))
    return 0;
  if (pca_temp_uses(ir, t0) != 2 || pca_temp_uses(ir, t1) != 2)
    return 0;
  int32_t doff;
  if (!pca_is_local_slot(ld, &doff))
    return 0;
  int32_t size = irop_get_imm32(a2);
  if (size <= 0 || (size & 3))
    return 0;
  c->dst = doff;
  c->src = irop_get_stack_offset(ls);
  c->size = size;
  c->tmpl = ls;
  c->tmpl.is_lval = 1;
  c->tmpl.btype = IROP_BTYPE_INT32;
  c->first = i;
  c->last = at[5];
  for (int k = 0; k < 6; k++)
    c->ins[c->nins++] = at[k];
  return 1;
}

static int pca_in(int32_t off, int32_t base, int32_t size)
{
  return off >= base && off < base + size;
}

/* Does `op` name the copy's source? */
static int pca_is_src_ref(IROperand op, const PcaCopy *c)
{
  if (irop_is_none(op))
    return 0;
  int32_t svr = irop_get_vreg(c->tmpl);
  if (svr >= 0 && irop_get_vreg(op) == svr)
    return 1;
  if (irop_get_tag(op) != IROP_TAG_STACKOFF || !op.is_local || op.is_param != c->tmpl.is_param)
    return 0;
  if (!c->tmpl.is_param && irop_get_vreg(op) >= 0)
    return 0;
  return pca_in(irop_get_stack_offset(op), c->src, c->size);
}

static int pca_is_copy_ins(const PcaCopy *c, int i)
{
  for (int k = 0; k < c->nins; k++)
    if (c->ins[k] == i)
      return 1;
  return 0;
}

/* Operand `k` of `q` is the hidden pointer a struct-returning call writes its
 * result through, and it points at the copy's source: the call fills exactly
 * those bytes, which is what the copy then reads.  The pointer is the call's
 * parameter 0 and never outlives the call (tcc_ir_frame_note_sret_call
 * records which calls have one, and frame.c already treats such a buffer's
 * address as not escaping). */
static int pca_is_sret_buffer_arg(TCCIRState *ir, IRQuadCompact *q, int k, IROperand op, const PcaCopy *c)
{
  if (q->op != TCCIR_OP_FUNCPARAMVAL || k != 0 || op.is_lval || op.is_llocal)
    return 0;
  if (irop_get_stack_offset(op) != c->src)
    return 0;
  const uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
  if (TCCIR_DECODE_PARAM_IDX(enc) != 0)
    return 0;
  const int call_id = TCCIR_DECODE_CALL_ID(enc);
  if (call_id < 0 || call_id >= ir->sret_calls_size)
    return 0;
  return ir->sret_calls[call_id] == c->size;
}

/* The copy reads a buffer the call right before it filled, and that call is
 * the only thing that writes it: the instructions between the hidden pointer
 * and the copy are that call and its own arguments.  Then the source and the
 * destination hold the same bytes at every point after the copy, whatever the
 * control flow around them -- each is written once, next to the other -- so
 * the destination's references may be renamed onto the source wherever they
 * are, not only in the entry block.  Reading the destination before the copy
 * has run reads an uninitialised object either way. */
static int pca_sret_copy_follows_its_call(TCCIRState *ir, const PcaCopy *c, int arg_idx)
{
  const uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, &ir->compact_instructions[arg_idx]));
  const int call_id = TCCIR_DECODE_CALL_ID(enc);
  int saw_call = 0;
  for (int i = arg_idx + 1; i < c->first; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID || q->op == TCCIR_OP_FUNCCALLVAL ||
        q->op == TCCIR_OP_FUNCCALLVOID)
    {
      IROperand enc_op = q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID
                             ? tcc_ir_op_get_src2(ir, q)
                             : tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, enc_op)) != call_id)
        return 0;
      saw_call |= q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID;
      continue;
    }
    return 0;
  }
  return saw_call;
}

static int pca_try(TCCIRState *ir, PcaCopy *c)
{
  const int n = ir->next_instruction_index;
  int32_t obj_size = tcc_ir_frame_object_size_at(ir, c->dst);
  if (obj_size <= 0 || obj_size > c->size || c->size - obj_size >= 4)
    return 0;
  /* A local source is itself a whole frame object the copy reads completely. */
  if (!c->tmpl.is_param)
  {
    int32_t ssz = tcc_ir_frame_object_size_at(ir, c->src);
    if (ssz <= 0 || ssz > c->size || c->size - ssz >= 4 || pca_in(c->src, c->dst, c->size) ||
        pca_in(c->dst, c->src, c->size))
      return 0;
    if (irop_get_stack_offset(c->tmpl) + c->size > 0)
      return 0;
  }
  else if (c->tmpl.btype == IROP_BTYPE_STRUCT || c->src < 0 || c->src + c->size > 32767)
    return 0;

  int sret_src = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || pca_is_copy_ins(c, i))
      continue;
    int nops = pca_nops(q->op);
    for (int k = 0; k < nops; k++)
    {
      IROperand op = ir->iroperand_pool[q->operand_base + k];
      int32_t off;
      if (pca_is_src_ref(op, c))
      {
        /* A local source may be written before the copy: the prologue storing
         * register-passed words into it, or the call that returned it, which
         * fills it whole through the hidden pointer it is handed (Zig's C
         * copies a call's result into a second local before reading it).
         * Nothing else may touch it. */
        if (c->tmpl.is_param || i > c->first)
          return 0;
        if (q->op == TCCIR_OP_STORE && k == 0 && op.is_lval)
          continue;
        if (pca_is_sret_buffer_arg(ir, q, k, op, c) && pca_sret_copy_follows_its_call(ir, c, i))
        {
          sret_src = 1;
          continue;
        }
        return 0;
      }
      /* A vreg-backed operand's offset is only the frontend's watermark (see
       * compute_stack_layout); concrete slots are what can name the local. */
      if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_param && irop_get_vreg(op) < 0 &&
          pca_in(irop_get_stack_offset(op), c->dst, c->size))
      {
        /* The destination: only as a plain slot, and -- unless the source is
         * the call's own buffer, where the two agree from the copy onwards
         * whatever the control flow -- only after the copy. */
        if ((i < c->first && !sret_src) || !pca_is_local_slot(op, &off) || op.is_llocal)
          return 0;
        if (q->op == TCCIR_OP_STORE && k == 0 && op.is_lval && sret_src)
          return 0; /* written by something other than the copy */
      }
    }
  }

  if (!c->entry_only && !sret_src)
    return 0;

  /* Rename every destination reference onto the source. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || pca_is_copy_ins(c, i))
      continue;
    int nops = pca_nops(q->op);
    for (int k = 0; k < nops; k++)
    {
      IROperand *p = &ir->iroperand_pool[q->operand_base + k];
      int32_t off;
      if (!pca_is_local_slot(*p, &off) || !pca_in(off, c->dst, c->size))
        continue;
      int32_t noff = c->src + (off - c->dst);
      IROperand r;
      if (c->tmpl.is_param)
      {
        r = c->tmpl;
        r.btype = p->btype;
        r.is_lval = p->is_lval;
        r.is_unsigned = p->is_unsigned;
        r.is_complex = p->is_complex;
        r.aux = p->aux;
      }
      else
        r = *p;
      if (p->btype == IROP_BTYPE_STRUCT)
      {
        r.u.s.ctype_idx = p->u.s.ctype_idx;
        r.u.s.aux_data = (int16_t)noff;
      }
      else
        r.u.imm32 = noff;
      *p = r;
    }
  }
  for (int k = 0; k < c->nins; k++)
    ir->compact_instructions[c->ins[k]].op = TCCIR_OP_NOP;
  return 1;
}

int tcc_ir_opt_param_copy_alias(TCCIRState *ir)
{
  if (!ir || !tcc_state || tcc_state->do_debug || tcc_bounds_checking(tcc_state) || ir->has_static_chain ||
      ir->captured_count > 0 || tcc_state->nb_nested_funcs > 0 || ir->func_has_label_addr)
    return 0;
  const int n = ir->next_instruction_index;
  /* The entry block: up to the first jump target or control transfer. */
  int entry_end = n;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (i > 0 && q->is_jump_target)
    {
      entry_end = i;
      break;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_SETJMP || q->op == TCCIR_OP_NL_SETJMP ||
        q->op == TCCIR_OP_VLA_ALLOC || q->op == TCCIR_OP_INLINE_ASM || q->op == TCCIR_OP_ASM_INPUT ||
        q->op == TCCIR_OP_ASM_OUTPUT)
    {
      entry_end = i;
      break;
    }
  }
  if (ir->compact_instructions[0].is_jump_target)
    return 0;

  int changes = 0;
  PcaCopy *c = tcc_malloc(sizeof(PcaCopy));
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    memset(c, 0, sizeof(*c));
    /* Beyond the entry block only the call-buffer shape qualifies; pca_try
     * rejects the rest, which the entry-block limit used to keep out. */
    const int limit = i < entry_end ? entry_end : n;
    if (!pca_match_chain(ir, i, limit, c))
    {
      memset(c, 0, sizeof(*c));
      if (!pca_match_call(ir, i, limit, c))
        continue;
    }
    c->entry_only = i < entry_end;
    if (pca_try(ir, c))
    {
      changes++;
      i = c->last;
    }
  }
  tcc_free(c);
  return changes;
}

int tcc_ir_opt_param_copy_alias_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_param_copy_alias(ctx->ir);
}
