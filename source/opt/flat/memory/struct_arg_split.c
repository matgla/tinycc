/*
 *  TCC IR - By-value struct arguments built in a frame slot (ra:struct_arg_split)
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
#include "opt_alias.h"

extern int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q);

/* A small struct passed by value is built in a frame slot and read back, word
 * by word, when the call is lowered:
 *
 *   StackLoc[-16] <-- P1 [STORE]
 *   StackLoc[-12] <-- P2 [STORE]
 *   PARAM0[call_0] P0
 *   PARAM1[call_0] StackLoc[-16]        <- the struct, read from the slot
 *   CALL rd
 *
 * The value of every word is known at the store, yet the stores stay (the
 * PARAM reads the slot), the frame stays, and a call that could have been a
 * tail call (`b rd`) becomes `push; sub sp; strd; bl; add sp; pop`.  The Zig C
 * backend passes every `[]u8` / `Slice` this way.
 *
 * AAPCS passes a composite of alignment <= 4 and no VFP candidacy exactly like
 * the same number of 32-bit scalars in a row: words fill r0-r3 in order, a
 * composite that does not fit splits between r3 and the stack, and what is left
 * goes to the stack in word slots.  So when every word of the argument is
 * written by a 32-bit store of a plain value in the PARAM's own basic block
 * (nothing else writes the bytes in between, the slot's address never taken)
 * the argument becomes one scalar PARAM per word, fed with the stored values.
 * When no other instruction reads the slot any more, its stores are dead and go.
 *
 * Soundness:
 *   - The slot's frame object must never have its address taken (a non-lvalue
 *     STACKOFF operand anywhere in the function, an indexed access, a copy
 *     helper, an sret buffer): only the direct accesses the scan sees can read
 *     or write it.  Inline asm, setjmp-style returns-twice ops, static chains
 *     and nested functions end the pass for the whole function.
 *   - A word's value is the one stored by the nearest preceding direct word
 *     store in the same basic block; any other write overlapping the word on
 *     the way (a partial store, a struct copy into the slot) rejects the
 *     argument.  The stored vreg must not be redefined between the store and
 *     the PARAM.  Nothing may write the argument's bytes between the PARAM and
 *     its CALL either (the original reads the slot at the CALL).
 *   - A call with a VFP-candidate argument (float, double, HFA, _Complex) under
 *     hard float is left alone: AAPCS32 C.5 forbids splitting a struct between
 *     r3 and the stack once an earlier VFP argument has used the stack.
 *   - Alignment 8 (even-register rule) and HFA / _Complex structs (VFP
 *     registers under hard float) are not word sequences and are left alone;
 *     so are sizes that are not 4, 8, 12 or 16 bytes and volatile accesses.
 *   - The slot's stores are removed only when every read of the object in the
 *     function is one of the arguments converted here.
 *
 * Knob: TCC_DISABLE_PASS=ra:struct_arg_split. */

#define SAS_MAX_WORDS 4

typedef struct SasObj
{
  int lo, hi;      /* frame object [lo, hi) */
  uint8_t escaped; /* its address is taken somewhere */
  uint8_t opaque;  /* an access whose extent is unknown or leaves the object */
  uint8_t stores_gone; /* its stores were removed already */
  int reads;       /* direct reads other than this pass's candidate arguments */
} SasObj;

typedef struct SasCand
{
  int idx;  /* the FUNCPARAMVAL */
  int call; /* its CALL */
  int off, size, words, obj;
  int ok;
  IROperand val[SAS_MAX_WORDS];
} SasCand;

typedef struct Sas
{
  TCCIRState *ir;
  int n;
  uint8_t *target; /* 1 at an instruction a jump or switch may enter */
  SasObj *obj;
  int nobj, capobj;
  SasCand *cand;
  int ncand, capcand;
} Sas;

static int sas_nops(int op)
{
  int n = irop_config[op].has_dest + irop_config[op].has_src1 + irop_config[op].has_src2;
  if (ir_op_has(op, IROP_A_SLOT3) && !ir_opset_has(IR_LEGACY_GAP_OPS(TCCIR_OP_UMAAL), op))
    n++;
  return n;
}

/* A direct frame operand of the function's own frame: 1 = an access of *w
 * bytes at *off, 2 = the address of the frame (escapes), 3 = an access whose
 * extent is unknown, 0 = not a frame reference. */
static int sas_frame_ref(IROperand op, int *off, int *w)
{
  if (irop_get_tag(op) != IROP_TAG_STACKOFF || irop_get_vreg(op) >= 0 || op.is_param || !op.is_local)
    return 0;
  *off = irop_get_stack_offset(op);
  if (!op.is_lval)
    return 2;
  if (op.is_llocal)
  {
    *w = 4;
    return 1;
  }
  if (irop_get_btype(op) == IROP_BTYPE_STRUCT)
  {
    CType *t = irop_get_ctype(op);
    int align;
    int sz = t ? type_size(t, &align) : -1;
    if (sz <= 0)
      return 3;
    *w = sz;
    return 1;
  }
  if (op.is_complex)
  {
    *w = (irop_get_btype(op) == IROP_BTYPE_FLOAT64 || irop_get_btype(op) == IROP_BTYPE_INT64) ? 16 : 8;
    return 1;
  }
  int sz = ir_opt_store_btype_size_bytes(irop_get_btype(op));
  if (sz <= 0)
    return 3;
  *w = sz;
  return 1;
}

static SasObj *sas_obj_at(Sas *s, int off)
{
  int lo, hi;
  if (!tcc_ir_frame_object_at(s->ir, off, &lo, &hi))
    return NULL;
  for (int k = 0; k < s->nobj; k++)
    if (s->obj[k].lo == lo)
      return &s->obj[k];
  if (s->nobj == s->capobj)
  {
    s->capobj = s->capobj ? 2 * s->capobj : 16;
    s->obj = tcc_realloc(s->obj, sizeof(SasObj) * s->capobj);
  }
  SasObj *o = &s->obj[s->nobj++];
  memset(o, 0, sizeof *o);
  o->lo = lo;
  o->hi = hi;
  return o;
}

/* Could instruction q give vreg `v` a new value? */
static int sas_may_def(TCCIRState *ir, IRQuadCompact *q, int32_t v)
{
  const int op = q->op;
  if (op == TCCIR_OP_NOP)
    return 0;
  const int nops = sas_nops(op);
  const int exotic = ir_op_has(op, IR_HZ_UPDATES_SRC) || ir_op_has(op, IROP_A_SLOT3) || tcc_ir_op_is_mac(op);
  for (int k = 0; k < nops; k++)
  {
    IROperand o = ir->iroperand_pool[q->operand_base + k];
    if (irop_get_vreg(o) != v)
      continue;
    if ((irop_config[op].has_dest && k == 0 && !o.is_lval) || exotic)
      return 1;
  }
  return 0;
}

static int sas_is_cand_param(Sas *s, int i, int *off, int *size, int *words)
{
  TCCIRState *ir = s->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (q->op != TCCIR_OP_FUNCPARAMVAL)
    return 0;
  IROperand a = tcc_ir_op_get_src1(ir, q);
  int w;
  if (irop_get_btype(a) != IROP_BTYPE_STRUCT || a.is_complex || sas_frame_ref(a, off, &w) != 1 || a.is_llocal)
    return 0;
  if (tcc_ir_access_is_volatile(ir, a))
    return 0;
  int align = 0;
  const int sz = irop_type_size_align(a, &align);
  if (sz != 4 && sz != 8 && sz != 12 && sz != 16)
    return 0;
  if (w != sz)
    return 0;
  const int aapcs = irop_aapcs_alignment(a);
  if ((aapcs < align ? aapcs : align) > 4)
    return 0;
  CType *ct = irop_get_ctype(a);
  int hfa_base = 0;
  if (!ct || gfunc_hfa(ct, &hfa_base) > 0)
    return 0;
  *size = sz;
  *words = sz / 4;
  return 1;
}

/* The CALL that consumes the PARAM at `i`: the next call of its call id. */
static int sas_call_of(Sas *s, int i)
{
  TCCIRState *ir = s->ir;
  const int cid = TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, &ir->compact_instructions[i]));
  for (int j = i + 1; j < s->n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;
    if (TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, q)) == cid)
      return j;
  }
  return -1;
}

/* Does instruction q write any of the frame bytes [lo, hi)?  *exact_word is
 * set when it is a 32-bit STORE of exactly [lo, hi) and nothing else. */
static int sas_writes(TCCIRState *ir, IRQuadCompact *q, int lo, int hi, int *exact_word)
{
  const int op = q->op;
  *exact_word = 0;
  if (op == TCCIR_OP_NOP || !irop_config[op].has_dest)
    return 0;
  IROperand d = ir->iroperand_pool[q->operand_base];
  int off, w;
  const int r = sas_frame_ref(d, &off, &w);
  if (r == 0 || !d.is_lval)
    return 0;
  if (r == 3)
    return 1;
  if (off >= hi || off + w <= lo)
    return 0;
  if (op == TCCIR_OP_STORE && off == lo && w == hi - lo && w == 4 && irop_get_btype(d) == IROP_BTYPE_INT32 &&
      !d.is_complex && !d.is_llocal)
    *exact_word = 1;
  return 1;
}

static int sas_value_ok(IROperand v)
{
  if (v.is_lval || v.is_llocal || v.is_complex || irop_get_btype(v) != IROP_BTYPE_INT32)
    return 0;
  /* A symbol's address (a string literal's pointer) is as good an argument as a constant. */
  if (irop_get_tag(v) == IROP_TAG_SYMREF)
    return 1;
  if (v.is_sym)
    return 0;
  if (irop_get_tag(v) == IROP_TAG_IMM32)
    return 1;
  return irop_get_tag(v) == IROP_TAG_VREG && irop_get_vreg(v) >= 0;
}

/* Under the hard-float ABI a call with a VFP-candidate argument (a float, a
 * double, an HFA, a _Complex) can have used the stack before the struct is
 * placed -- the 17th float -- and then AAPCS32 C.5 keeps the struct from
 * splitting between r3 and the stack, which a row of 32-bit scalars would
 * do.  Such calls are left alone. */
static int sas_call_has_vfp_arg(Sas *s, int call)
{
  TCCIRState *ir = s->ir;
  if (!tcc_state || tcc_state->float_abi != ARM_HARD_FLOAT)
    return 0;
  const int cid = TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, &ir->compact_instructions[call]));
  for (int j = call - 1; j >= 0; j--)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_FUNCPARAMVAL || TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, q)) != cid)
      continue;
    IROperand a = tcc_ir_op_get_src1(ir, q);
    const int bt = irop_get_btype(a);
    if (bt == IROP_BTYPE_FLOAT32 || bt == IROP_BTYPE_FLOAT64 || a.is_complex)
      return 1;
    if (bt == IROP_BTYPE_STRUCT)
    {
      CType *ct = irop_get_ctype(a);
      int base = 0;
      if (!ct || gfunc_hfa(ct, &base) > 0)
        return 1;
    }
  }
  return 0;
}

/* Resolve every word of candidate c from the stores that feed it. */
static int sas_resolve(Sas *s, SasCand *c)
{
  TCCIRState *ir = s->ir;
  for (int w = 0; w < c->words; w++)
  {
    const int lo = c->off + 4 * w, hi = lo + 4;
    int found = -1;
    for (int j = c->idx - 1; j >= 0; j--)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op != TCCIR_OP_NOP && (ir_op_has(q->op, IR_HZ_BRANCH | IR_HZ_RETURN) || ir_op_has(q->op, IROP_A_NO_FALLTHROUGH)))
        break;
      int exact;
      if (sas_writes(ir, q, lo, hi, &exact))
      {
        if (!exact)
          return 0;
        found = j;
        break;
      }
      if (s->target[j])
        break;
    }
    if (found < 0)
      return 0;
    IRQuadCompact *sq = &ir->compact_instructions[found];
    IROperand d = tcc_ir_op_get_dest(ir, sq);
    IROperand v = tcc_ir_op_get_src1(ir, sq);
    if (tcc_ir_access_is_volatile(ir, d) || !sas_value_ok(v))
      return 0;
    const int32_t vr = irop_get_vreg(v);
    if (vr >= 0)
      for (int j = found + 1; j < c->idx; j++)
        if (sas_may_def(ir, &ir->compact_instructions[j], vr))
          return 0;
    c->val[w] = v;
  }
  /* Nothing may rewrite the argument between the PARAM and its CALL. */
  for (int j = c->idx + 1; j < c->call; j++)
  {
    int exact;
    if (sas_writes(ir, &ir->compact_instructions[j], c->off, c->off + c->size, &exact))
      return 0;
  }
  return 1;
}

static int sas_scan(Sas *s)
{
  TCCIRState *ir = s->ir;
  const int n = s->n;
  int any = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const int op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (ir_op_has(op, IR_HZ_ASM | IR_HZ_NONLOCAL | IR_HZ_CHAIN) || (ir_op_props[op] & IROP_A_RETURNS_TWICE))
      return -1;
    if (op == TCCIR_OP_FUNCPARAMVAL)
    {
      int off, size, words;
      if (sas_is_cand_param(s, i, &off, &size, &words))
        any = 1;
    }
  }
  if (!any)
    return 0;

  s->target = tcc_mallocz(n + 2);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->is_jump_target)
      s->target[i] = 1;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t >= 0 && t <= n)
        s->target[t] = 1;
    }
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *tb = &ir->switch_tables[t];
    if (tb->default_target >= 0 && tb->default_target <= n)
      s->target[tb->default_target] = 1;
    for (int k = 0; tb->targets && k < tb->num_entries; k++)
      if (tb->targets[k] >= 0 && tb->targets[k] <= n)
        s->target[tb->targets[k]] = 1;
  }

  /* Every reference to the frame, by object. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const int op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    const int nops = sas_nops(op);
    int off, size, words;
    const int cand_param = op == TCCIR_OP_FUNCPARAMVAL && sas_is_cand_param(s, i, &off, &size, &words);
    for (int k = 0; k < nops; k++)
    {
      IROperand o = ir->iroperand_pool[q->operand_base + k];
      int ro, rw = 0;
      const int r = sas_frame_ref(o, &ro, &rw);
      if (!r)
        continue;
      SasObj *ob = sas_obj_at(s, ro);
      if (!ob)
        continue;
      const int is_dest = irop_config[op].has_dest && k == 0;
      if (r == 2)
      {
        ob->escaped = 1;
        continue;
      }
      if (r == 3 || ro + rw > ob->hi || ro < ob->lo)
      {
        ob->opaque = 1;
        continue;
      }
      if (!is_dest && !(cand_param && k == (int)irop_config[op].has_dest))
        ob->reads++;
      else if (is_dest && op != TCCIR_OP_STORE)
        ob->opaque = 1; /* a write of another kind: keep it simple */
    }
  }
  return 1;
}

static int sas_add_cand(Sas *s, int i, int off, int size, int words)
{
  if (s->ncand == s->capcand)
  {
    s->capcand = s->capcand ? 2 * s->capcand : 16;
    s->cand = tcc_realloc(s->cand, sizeof(SasCand) * s->capcand);
  }
  SasCand *c = &s->cand[s->ncand++];
  memset(c, 0, sizeof *c);
  c->idx = i;
  c->off = off;
  c->size = size;
  c->words = words;
  c->call = -1;
  c->obj = -1;
  return s->ncand - 1;
}

static void sas_set_param_idx(TCCIRState *ir, IRQuadCompact *q, int cid, int idx)
{
  tcc_ir_op_set_src2(ir, q, irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_PARAM(cid, idx), IROP_BTYPE_INT32));
}

int tcc_ir_opt_struct_arg_split(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index < 3 || ir->frame_relaid || !ir->frame_obj_count)
    return 0;
  if (ir->has_static_chain || ir->captured_count > 0 || tcc_state->nb_nested_funcs > 0)
    return 0;
  if (!TCC_OPT(tcc_state, optimize) || tcc_ir_opt_pass_disabled("ra:struct_arg_split"))
    return 0;

  Sas s = {0};
  s.ir = ir;
  s.n = ir->next_instruction_index;

  int changes = 0;
  if (sas_scan(&s) <= 0)
    goto done;

  /* Candidates, in program order. */
  for (int i = 0; i < s.n; i++)
  {
    int off, size, words;
    if (!sas_is_cand_param(&s, i, &off, &size, &words))
      continue;
    const int ci = sas_add_cand(&s, i, off, size, words);
    SasCand *c = &s.cand[ci];
    SasObj *ob = sas_obj_at(&s, off);
    c->obj = ob ? (int)(ob - s.obj) : -1;
    c->call = sas_call_of(&s, i);
    c->ok = ob && c->call >= 0 && !ob->escaped && !ob->opaque && off >= ob->lo && off + size <= ob->hi &&
            !s.target[i + 1] && !sas_call_has_vfp_arg(&s, c->call) && sas_resolve(&s, c);
    const int argc = c->call >= 0 ? TCCIR_DECODE_CALL_ARGC(tcc_ir_op_src2_imm(ir, &ir->compact_instructions[c->call])) : 0;
    if (c->ok && argc + c->words - 1 > 0x7FFF)
      c->ok = 0;
  }

  /* A candidate left as it is still reads its slot: the stores must stay. */
  for (int k = 0; k < s.ncand; k++)
    if (!s.cand[k].ok && s.cand[k].obj >= 0)
      s.obj[s.cand[k].obj].reads++;
  for (int k = 0; k < s.ncand; k++)
  {
    SasCand *c = &s.cand[k];
    if (c->ok && (s.obj[c->obj].reads || s.obj[c->obj].escaped || s.obj[c->obj].opaque))
      c->ok = 0;
  }

  /* Kill the stores of every slot whose last readers are converted. */
  for (int k = 0; k < s.ncand; k++)
  {
    SasCand *c = &s.cand[k];
    if (!c->ok)
      continue;
    SasObj *ob = &s.obj[c->obj];
    if (ob->stores_gone)
      continue;
    for (int i = 0; i < s.n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_STORE)
        continue;
      int ro, rw = 0;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (sas_frame_ref(d, &ro, &rw) == 1 && d.is_lval && ro >= ob->lo && ro < ob->hi && !tcc_ir_access_is_volatile(ir, d))
        q->op = TCCIR_OP_NOP;
    }
    ob->stores_gone = 1;
  }

  /* Rewrite the arguments, last first so earlier indices stay put. */
  for (int k = s.ncand - 1; k >= 0; k--)
  {
    SasCand *c = &s.cand[k];
    if (!c->ok)
      continue;
    const int p = c->idx;
    IRQuadCompact *pq = &ir->compact_instructions[p];
    const int encoded = (int)tcc_ir_op_src2_imm(ir, pq);
    const int cid = TCCIR_DECODE_CALL_ID(encoded);
    const int pidx = TCCIR_DECODE_PARAM_IDX(encoded);
    const int extra = c->words - 1;
    IRQuadCompact *cq = &ir->compact_instructions[c->call];
    const int argc = TCCIR_DECODE_CALL_ARGC(tcc_ir_op_src2_imm(ir, cq));

    /* The arguments after this one move up by the words it gains. */
    if (extra)
    {
      int seen = 0;
      for (int j = c->call - 1; j >= 0 && seen < argc; j--)
      {
        IRQuadCompact *q = &ir->compact_instructions[j];
        if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
          continue;
        const int e = (int)tcc_ir_op_src2_imm(ir, q);
        if (TCCIR_DECODE_CALL_ID(e) != cid)
          continue;
        seen++;
        if (j != p && TCCIR_DECODE_PARAM_IDX(e) > pidx)
          sas_set_param_idx(ir, q, cid, TCCIR_DECODE_PARAM_IDX(e) + extra);
      }
    }

    tcc_ir_op_set_src1(ir, pq, c->val[0]);
    if (extra && ir->iroperand_pool_count + 2 * extra > ir->iroperand_pool_capacity)
      tcc_ir_pool_ensure(ir, 2 * extra);
    for (int w = 1; w <= extra; w++)
    {
      pq = &ir->compact_instructions[p];
      IRQuadCompact nq = *pq;
      nq.orig_index = ++ir->max_orig_index;
      nq.is_jump_target = 0;
      nq.operand_base = tcc_ir_pool_add(ir, c->val[w]);
      tcc_ir_pool_add(ir, irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_PARAM(cid, pidx + w), IROP_BTYPE_INT32));
      if (gsym_cse_insert_before(ir, p + w, &nq) < 0)
        tcc_ice("struct_arg_split: insert failed");
    }
    if (extra)
    {
      /* Everything after p moved by `extra`: the call, and the not yet
       * rewritten candidates whose calls lie after it. */
      for (int m = 0; m < k; m++)
        if (s.cand[m].call > p)
          s.cand[m].call += extra;
      ir_opt_change_call_argc(ir, c->call + extra, argc + extra);
    }
    changes++;
  }

done:
  if (changes)
    tcc_ir_vreg_index_dirty(ir);
  tcc_free(s.target);
  tcc_free(s.obj);
  tcc_free(s.cand);
  return changes;
}
