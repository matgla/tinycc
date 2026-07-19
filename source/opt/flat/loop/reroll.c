/*
 *  TCC IR - Loop re-rolling: collapse N identical consecutive blocks into a counted loop
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "tcc.h"
#include "tccir.h"
#include "tccir_operand.h"
#include "ir.h"
#include "opt.h"
#include "opt_reroll.h"
#include "log.h"

#define REROLL_MIN_PERIOD  3
#define REROLL_MAX_PERIOD  32
#define REROLL_MIN_REPEATS 4

#ifndef LOG_REROLL
#ifdef TCC_LOG_REROLL
#define LOG_REROLL(...) fprintf(stderr, "[REROLL] " __VA_ARGS__), fprintf(stderr, "\n")
#else
#define LOG_REROLL(...) ((void)0)
#endif
#endif

/* Inserts at pos and shifts later jump targets accordingly. */
int insert_instr_at(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2);

/* Re-lays the operand pool for the new opcode; needed for ops without a dest, since insert_instr_at assumes a 3-slot layout. */
void write_instr_at_nop(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2);

/* Per-iteration rename: vreg-in-iter-0 -> vreg-in-iter-k. */
typedef struct VregRenameMap {
  int *src;
  int *dst;
  int  count;
  int  cap;
} VregRenameMap;

static void vrmap_init(VregRenameMap *m)
{
  m->cap = 32;
  m->src = (int *)tcc_malloc(m->cap * sizeof(int));
  m->dst = (int *)tcc_malloc(m->cap * sizeof(int));
  m->count = 0;
}

static void vrmap_free(VregRenameMap *m)
{
  tcc_free(m->src);
  tcc_free(m->dst);
  m->src = m->dst = NULL;
  m->cap = m->count = 0;
}

static void vrmap_reset(VregRenameMap *m) { m->count = 0; }

/* Map is injective: each dst comes from exactly one src; 0 on conflict. */
static int vrmap_bind(VregRenameMap *m, int src, int dst)
{
  for (int i = 0; i < m->count; i++) {
    if (m->src[i] == src) return m->dst[i] == dst;
    if (m->dst[i] == dst) return 0;
  }
  if (m->count == m->cap) {
    m->cap *= 2;
    m->src = (int *)tcc_realloc(m->src, m->cap * sizeof(int));
    m->dst = (int *)tcc_realloc(m->dst, m->cap * sizeof(int));
  }
  m->src[m->count] = src;
  m->dst[m->count] = dst;
  m->count++;
  return 1;
}

/* Vregs DEFINED (as dest) within the canonical body; everything else is an external input whose value is observable. */
typedef struct VregSet {
  int *v;
  int  count;
  int  cap;
} VregSet;

static void vrset_init(VregSet *s) { s->cap = 16; s->count = 0; s->v = (int *)tcc_malloc(s->cap * sizeof(int)); }
static void vrset_free(VregSet *s) { tcc_free(s->v); s->v = NULL; s->cap = s->count = 0; }
static void vrset_reset(VregSet *s) { s->count = 0; }

static int vrset_contains(const VregSet *s, int v)
{
  for (int i = 0; i < s->count; i++) if (s->v[i] == v) return 1;
  return 0;
}

static void vrset_add(VregSet *s, int v)
{
  if (vrset_contains(s, v)) return;
  if (s->count == s->cap) {
    s->cap *= 2;
    s->v = (int *)tcc_realloc(s->v, s->cap * sizeof(int));
  }
  s->v[s->count++] = v;
}

static int op_is_unsafe_for_reroll(TccIrOp op)
{
  switch (op) {
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_CALLSEQ_BEGIN:
    case TCCIR_OP_CALLSEQ_END:
    case TCCIR_OP_CALLARG_REG:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_NOP:
      return 1;
    default:
      return 0;
  }
}

/* src2 is (call_id << 16) | argc-or-param-idx; call_id is fresh per iteration, so only the low 16 bits may be compared. */
static int call_meta_src2_equiv(TccIrOp op, IROperand a, IROperand b)
{
  if (op != TCCIR_OP_FUNCPARAMVAL && op != TCCIR_OP_FUNCPARAMVOID &&
      op != TCCIR_OP_FUNCCALLVAL && op != TCCIR_OP_FUNCCALLVOID)
    return -1; /* not a call-meta op; defer to default comparison */
  if (irop_get_tag(a) != IROP_TAG_IMM32 || irop_get_tag(b) != IROP_TAG_IMM32)
    return -1;
  return (a.u.imm32 & 0xFFFF) == (b.u.imm32 & 0xFFFF);
}

static int operand_equiv(TCCIRState *ir, IROperand a, IROperand b,
                         VregRenameMap *map, const VregSet *internal_defs)
{
  if (irop_is_none(a) != irop_is_none(b)) return 0;
  if (irop_is_none(a)) return 1;

  int tag_a = irop_get_tag(a);
  int tag_b = irop_get_tag(b);
  /* VREG and STACKOFF are interchangeable encodings of one vreg: compare the underlying vreg and ignore encoding flags, since the opcode carries the deref. */
  int va_is_vlike = (tag_a == IROP_TAG_VREG || tag_a == IROP_TAG_STACKOFF);
  int vb_is_vlike = (tag_b == IROP_TAG_VREG || tag_b == IROP_TAG_STACKOFF);
  if (va_is_vlike && vb_is_vlike) {
    if (a.btype != b.btype) return 0;
    if (a.vreg_type == TCCIR_VREG_TYPE_PARAM && b.vreg_type != TCCIR_VREG_TYPE_PARAM) return 0;
    if (b.vreg_type == TCCIR_VREG_TYPE_PARAM && a.vreg_type != TCCIR_VREG_TYPE_PARAM) return 0;
    int vra = irop_get_vreg(a);
    int vrb = irop_get_vreg(b);
    if (vra < 0 && vrb < 0) {
      /* Pure STACKOFF: the raw frame offset literal is the identity. */
      if (tag_a == IROP_TAG_STACKOFF && tag_b == IROP_TAG_STACKOFF)
        return a.u.imm32 == b.u.imm32;
      return 1; /* both none-vreg of same other shape */
    }
    if (vra < 0 || vrb < 0) return 0;
    /* External vregs are observable inputs: strict equality, no renaming. */
    if (internal_defs && !vrset_contains(internal_defs, vra))
      return vra == vrb;
    return vrmap_bind(map, vra, vrb);
  }

  if (tag_a != tag_b) return 0;
  if (a.is_lval != b.is_lval) return 0;
  if (a.is_llocal != b.is_llocal) return 0;
  if (a.is_local != b.is_local) return 0;
  if (a.is_const != b.is_const) return 0;
  if (a.btype != b.btype) return 0;
  if (a.vreg_type != b.vreg_type) return 0;
  if (a.is_unsigned != b.is_unsigned) return 0;
  if (a.is_static != b.is_static) return 0;
  if (a.is_sym != b.is_sym) return 0;
  if (a.is_param != b.is_param) return 0;

  switch (tag_a) {
    case IROP_TAG_IMM32:
      return a.u.imm32 == b.u.imm32;
    case IROP_TAG_F32:
      return a.u.f32_bits == b.u.f32_bits;
    case IROP_TAG_I64:
    case IROP_TAG_F64:
      return irop_get_imm64_ex(ir, a) == irop_get_imm64_ex(ir, b);
    case IROP_TAG_SYMREF: {
      IRPoolSymref *ra = irop_get_symref_ex(ir, a);
      IRPoolSymref *rb = irop_get_symref_ex(ir, b);
      if (!ra || !rb) return ra == rb;
      /* Addend must match too: foo[0] and foo[1] are foo+0 vs foo+sizeof_elt. */
      return ra->sym == rb->sym && ra->addend == rb->addend && ra->flags == rb->flags;
    }
  }
  return 0;
}

/* 4th-slot operand (scale/accum/cond); trivially equivalent when the opcode has none. */
static int extra_operand_equiv(TCCIRState *ir, const IRQuadCompact *qa, const IRQuadCompact *qb,
                               VregRenameMap *map, const VregSet *internal_defs)
{
  TccIrOp op = qa->op;
  if (op != TCCIR_OP_LOAD_INDEXED && op != TCCIR_OP_STORE_INDEXED &&
      op != TCCIR_OP_MLA && op != TCCIR_OP_SELECT)
    return 1;
  IROperand a = tcc_ir_op_get_scale(ir, qa); /* same slot as accum/cond */
  IROperand b = tcc_ir_op_get_scale(ir, qb);
  return operand_equiv(ir, a, b, map, internal_defs);
}

/* Compares [base,base+P) against iteration k under vreg renaming; resets `map` to a fresh binding for k. */
static int block_matches(TCCIRState *ir, int base, int P, int k,
                         VregRenameMap *map, const VregSet *internal_defs)
{
  vrmap_reset(map);
  for (int i = 0; i < P; i++) {
    IRQuadCompact *qa = &ir->compact_instructions[base + i];
    IRQuadCompact *qb = &ir->compact_instructions[base + k * P + i];
    if (qa->op != qb->op) return 0;
    /* Reject every branch target in iterations k>=1, i==0 included: an edge landing on an iteration boundary cannot be represented by the single-entry rerolled loop. */
    if (qb->is_jump_target) return 0;
    if (!operand_equiv(ir, tcc_ir_op_get_dest(ir, qa), tcc_ir_op_get_dest(ir, qb), map, internal_defs)) return 0;
    if (!operand_equiv(ir, tcc_ir_op_get_src1(ir, qa), tcc_ir_op_get_src1(ir, qb), map, internal_defs)) return 0;

    IROperand sa2 = tcc_ir_op_get_src2(ir, qa);
    IROperand sb2 = tcc_ir_op_get_src2(ir, qb);
    int cm = call_meta_src2_equiv(qa->op, sa2, sb2);
    if (cm == 0) return 0;
    if (cm < 0 && !operand_equiv(ir, sa2, sb2, map, internal_defs)) return 0;

    if (!extra_operand_equiv(ir, qa, qb, map, internal_defs)) return 0;
  }
  return 1;
}

/* Vregs written in [base, base+P): exactly those eligible for cross-iteration renaming. */
static void collect_body_defs(TCCIRState *ir, int base, int P, VregSet *defs)
{
  vrset_reset(defs);
  for (int i = 0; i < P; i++) {
    IRQuadCompact *q = &ir->compact_instructions[base + i];
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_is_none(d)) continue;
    int tag = irop_get_tag(d);
    if (tag != IROP_TAG_VREG && tag != IROP_TAG_STACKOFF) continue;
    int v = irop_get_vreg(d);
    if (v >= 0) vrset_add(defs, v);
  }
}

/* Every call group must stay whole in [base, base+P): a phase-shifted boundary would NOP a call's params while its CALL survives past the run, and the backend callsite scan then aborts. */
static int body_calls_balanced(TCCIRState *ir, int base, int P)
{
  /* Groups opened by params but not yet closed by their call; at most P fit in a P-length body. */
  int open_ids[REROLL_MAX_PERIOD];
  int n_open = 0;
  for (int i = 0; i < P; i++) {
    IRQuadCompact *q = &ir->compact_instructions[base + i];
    if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_get_tag(s2) != IROP_TAG_IMM32) return 0; /* can't verify -> reject */
      int cid = TCCIR_DECODE_CALL_ID((uint32_t)s2.u.imm32);
      int found = 0;
      for (int j = 0; j < n_open; j++) if (open_ids[j] == cid) { found = 1; break; }
      if (!found) open_ids[n_open++] = cid;
    } else if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID) {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_is_none(s2)) continue; /* untracked call (no id) -- backend won't scan it */
      if (irop_get_tag(s2) != IROP_TAG_IMM32) return 0;
      if (TCCIR_DECODE_CALL_ARGC((uint32_t)s2.u.imm32) == 0) continue; /* zero-arg: self-contained */
      int cid = TCCIR_DECODE_CALL_ID((uint32_t)s2.u.imm32);
      int idx = -1;
      for (int j = 0; j < n_open; j++) if (open_ids[j] == cid) { idx = j; break; }
      if (idx < 0) return 0; /* CALL whose params are outside the body -> split */
      open_ids[idx] = open_ids[--n_open];
    }
  }
  return n_open == 0; /* still-open group ends past the body boundary -> split */
}

static int body_is_safe(TCCIRState *ir, int base, int P)
{
  for (int i = 0; i < P; i++) {
    IRQuadCompact *q = &ir->compact_instructions[base + i];
    if (op_is_unsafe_for_reroll(q->op)) return 0;
    if (i > 0 && q->is_jump_target) return 0;
  }
  return body_calls_balanced(ir, base, P);
}

static int count_repeats(TCCIRState *ir, int base, int P,
                         VregRenameMap *map, const VregSet *internal_defs)
{
  int n = ir->next_instruction_index;
  int reps = 1;
  while (base + (reps + 1) * P <= n) {
    if (!block_matches(ir, base, P, reps, map, internal_defs)) break;
    reps++;
  }
  return reps;
}

static int run_safe_no_external_use(TCCIRState *ir, int base, int P, int N)
{
  int run_lo = base;
  int run_hi = base + P * N;

  /* Open-addressed set of vregs written anywhere in the run. */
  unsigned ht_size = 64;
  while (ht_size < (unsigned)(run_hi - run_lo) * 2u)
    ht_size <<= 1;
  int *ht = (int *)tcc_malloc(ht_size * sizeof(int));
  for (unsigned j = 0; j < ht_size; j++) ht[j] = -1;
#define RS_HASH(v) (((uint32_t)(v) * 2654435761u) & (ht_size - 1))

  for (int i = run_lo; i < run_hi; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_is_none(d)) continue;
    int tag = irop_get_tag(d);
    if (tag != IROP_TAG_VREG && tag != IROP_TAG_STACKOFF) continue;
    int v = irop_get_vreg(d);
    if (v < 0) continue;
    unsigned h = RS_HASH(v);
    while (ht[h] >= 0 && ht[h] != v)
      h = (h + 1) & (ht_size - 1);
    ht[h] = v;
  }

  /* Any reference from outside the run makes it unsafe to reroll. */
  int unsafe = 0;
  for (int i = 0; i < ir->next_instruction_index && !unsafe; i++) {
    if (i >= run_lo && i < run_hi) continue;
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand ops[3] = {
      tcc_ir_op_get_dest(ir, q),
      tcc_ir_op_get_src1(ir, q),
      tcc_ir_op_get_src2(ir, q),
    };
    for (int s = 0; s < 3 && !unsafe; s++) {
      IROperand op = ops[s];
      if (irop_is_none(op)) continue;
      int tag = irop_get_tag(op);
      if (tag != IROP_TAG_VREG && tag != IROP_TAG_STACKOFF) continue;
      int v = irop_get_vreg(op);
      if (v < 0) continue;
      unsigned h = RS_HASH(v);
      while (ht[h] >= 0 && ht[h] != v)
        h = (h + 1) & (ht_size - 1);
      if (ht[h] == v) unsafe = 1;
    }
  }

#undef RS_HASH
  tcc_free(ht);
  return !unsafe;
}

/* Identity rename (V->V for every internal vreg) means the rerolled loop leaves the same final vreg values, so external uses need no further check. */
static int run_has_identity_rename(TCCIRState *ir, int base, int P,
                                   const VregSet *internal_defs)
{
  VregRenameMap map;
  vrmap_init(&map);
  if (!block_matches(ir, base, P, 1, &map, internal_defs)) {
    vrmap_free(&map);
    return 0;
  }
  for (int i = 0; i < map.count; i++) {
    if (vrset_contains(internal_defs, map.src[i]) && map.src[i] != map.dst[i]) {
      vrmap_free(&map);
      return 0;
    }
  }
  vrmap_free(&map);
  return 1;
}

/* Safe iff the iteration rename is the identity, or no vreg defined in the run is referenced outside it. */
static int reroll_is_safe(TCCIRState *ir, int base, int P, int N)
{
  VregSet defs;
  vrset_init(&defs);
  collect_body_defs(ir, base, P, &defs);
  int ok = run_has_identity_rename(ir, base, P, &defs)
        || run_safe_no_external_use(ir, base, P, N);
  vrset_free(&defs);
  return ok;
}

/* Replaces the run with counter + canonical body + back-edge. */
static void reroll_rewrite(TCCIRState *ir, int base, int P, int N)
{
  /* NOP-out iterations 1..N-1 in place, so no index shifting is needed. */
  for (int i = base + P; i < base + P * N; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    q->op = TCCIR_OP_NOP;
    q->is_jump_target = 0;
  }

  int counter_vreg = tcc_ir_get_vreg_var(ir);
  IROperand counter_op = irop_make_vreg(counter_vreg, IROP_BTYPE_INT32);
  IROperand zero_imm   = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
  IROperand one_imm    = irop_make_imm32(-1, 1, IROP_BTYPE_INT32);
  IROperand n_imm      = irop_make_imm32(-1, N, IROP_BTYPE_INT32);

  /* insert_instr_at shifts existing jump targets >= base for us. */
  int rc = insert_instr_at(ir, base, TCCIR_OP_ASSIGN, counter_op, zero_imm, irop_make_none());
  if (rc < 0) return; /* OOM; leave IR untouched */

  int body_start = base + 1;
  int after_run  = base + 1 + P * N;  /* first index past NOPs */

  rc = insert_instr_at(ir, after_run, TCCIR_OP_ADD, counter_op, counter_op, one_imm);
  if (rc < 0) return;

  /* CMP has no dest, so insert_instr_at's fixed 3-slot layout is wrong: insert a NOP, then re-lay the slots CMP uses. */
  rc = insert_instr_at(ir, after_run + 1, TCCIR_OP_NOP,
                       irop_make_none(), irop_make_none(), irop_make_none());
  if (rc < 0) return;
  write_instr_at_nop(ir, after_run + 1, TCCIR_OP_CMP,
                     irop_make_none(), counter_op, n_imm);

  /* body_start < pos, so insert_instr_at's shift of targets >= pos leaves this back-edge put. */
  IROperand jmp_target = irop_make_imm32(-1, body_start, IROP_BTYPE_INT32);
  IROperand cond_imm   = irop_make_imm32(-1, TOK_LT, IROP_BTYPE_INT32);
  rc = insert_instr_at(ir, after_run + 2, TCCIR_OP_JUMPIF, jmp_target, cond_imm, irop_make_none());
  if (rc < 0) return;

  ir->compact_instructions[after_run + 2].no_unroll = 1;

  /* Downstream passes (compact, jump-threading, SSA build) only preserve marked targets. */
  ir->compact_instructions[body_start].is_jump_target = 1;

  LOG_REROLL("rerolled run @%d P=%d N=%d into counter=%d loop", base, P, N, counter_vreg);
}

/* Linear scan picking the highest-coverage (P,N) at each position. */
static int tcc_ir_opt_reroll__timed(TCCIRState *ir);
int tcc_ir_opt_reroll(TCCIRState *ir)
{
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_reroll__timed(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_reroll__timed(ir);
  tcc_pass_timing_add("reroll", tcc_pass_clk_us() - _t);
  return _r;
}
static int tcc_ir_opt_reroll__timed(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index < REROLL_MIN_PERIOD * REROLL_MIN_REPEATS)
    return 0;

  VregRenameMap map;
  vrmap_init(&map);
  VregSet defs;
  vrset_init(&defs);

  int rerolled = 0;
  int i = 0;
  while (i + REROLL_MIN_PERIOD * REROLL_MIN_REPEATS <= ir->next_instruction_index) {
    int best_P = 0;
    int best_N = 0;
    int best_score = 0;

    int max_P = REROLL_MAX_PERIOD;
    if (i + REROLL_MIN_REPEATS * max_P > ir->next_instruction_index)
      max_P = (ir->next_instruction_index - i) / REROLL_MIN_REPEATS;

    for (int P = REROLL_MIN_PERIOD; P <= max_P; P++) {
      if (!body_is_safe(ir, i, P)) continue;

      /* Opcode-only prematch: skips collect_body_defs + full block_matches on the common no-match case; i+2P<=n holds since max_P caps i+REROLL_MIN_REPEATS*P<=n. */
      {
        int opmatch = 1;
        for (int k = 0; k < P; k++)
          if (ir->compact_instructions[i + k].op != ir->compact_instructions[i + P + k].op) { opmatch = 0; break; }
        if (!opmatch) continue;
      }

      collect_body_defs(ir, i, P, &defs);
      int reps = count_repeats(ir, i, P, &map, &defs);
      if (reps < REROLL_MIN_REPEATS) continue;
      int score = reps * P;
      if (score > best_score) {
        best_score = score;
        best_P = P;
        best_N = reps;
      }
    }

    if (best_P > 0 && reroll_is_safe(ir, i, best_P, best_N)) {
      reroll_rewrite(ir, i, best_P, best_N);
      rerolled++;
      i = i + 1 /*ASSIGN*/ + best_P /*body*/ + (best_N - 1) * best_P /*NOPs*/ + 3 /*ADD+CMP+JUMPIF*/;
    } else {
      i++;
    }
  }

  vrmap_free(&map);
  vrset_free(&defs);
  return rerolled;
}

/* Runs on flat IR at the head of the tcc_ir_ssa_regalloc flat region. */
int ssa_opt_reroll(TCCIRState *ir)
{
  return tcc_ir_opt_reroll(ir);
}
