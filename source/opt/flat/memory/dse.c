/*
 *  TCC IR - dead store elimination (flat, pre-SSA)
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
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_alias.h"

/* An instruction whose TEMP result is unused may be deleted only when its
 * opcode does nothing but compute that result: reading memory and the flags is
 * fine, any other effect (a store, a call, setjmp, __builtin_apply, asm, VLA
 * growth, ...) keeps it.  LOAD's source operand is checked by the callers. */
static inline int dse_op_may_die(int op)
{
  return !ir_op_has(op, IR_HZ_FROM_OP & ~(IR_HZ_MEM_READ | IR_HZ_FLAGS_SET | IR_HZ_FLAGS_READ));
}

static int tcc_ir_opt_dse__timed(TCCIRState *ir);
int tcc_ir_opt_dse(TCCIRState *ir)
{
  int r;
  TCC_PASS_TIMED(r, "dse", tcc_ir_opt_dse__timed(ir));
  return r;
}

/* A span of an anonymous stack object that may be read: [lo, hi). */
typedef struct
{
  const Sym *sym;
  int64_t lo, hi;
} DseRange;

static void dse_add_range(DseRange **r, int *n, int *cap, const Sym *sym, int64_t lo, int64_t hi)
{
  if (hi <= lo)
    return;
  if (*n == *cap)
  {
    *cap = *cap ? *cap * 2 : 32;
    *r = tcc_realloc(*r, sizeof(DseRange) * *cap);
  }
  (*r)[*n].sym = sym;
  (*r)[*n].lo = lo;
  (*r)[*n].hi = hi;
  (*n)++;
}

static int dse_ranges_overlap(const DseRange *r, int n, const Sym *sym, int64_t lo, int64_t hi)
{
  for (int k = 0; k < n; k++)
    if (r[k].sym == sym && r[k].lo < hi && lo < r[k].hi)
      return 1;
  return 0;
}

/* Whether `q` assigns its dest from its sources.  The stores do not: their
 * dest is the address written through (or, for STORE_INDEXED/POSTINC, the
 * base pointer), and an address in src1 is a value leaving for memory. */
static int dse_defines_dest(const IRQuadCompact *q, IROperand dest)
{
  switch (q->op)
  {
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
    return 0;
  case TCCIR_OP_STORE:
    return !dest.is_lval;
  default:
    return 1;
  }
}

/* Pull (sym, off) out of a local StackLoc operand: SYMREF carries sym+addend, else a bare stack offset. */
static void dse_stackloc_sym_off(TCCIRState *ir, IROperand op, const Sym **sym, int64_t *off)
{
  if (irop_get_tag(op) == IROP_TAG_SYMREF)
  {
    IRPoolSymref *sr = irop_get_symref_ex(ir, op);
    *sym = sr ? sr->sym : NULL;
    *off = sr ? sr->addend : 0;
  }
  else
  {
    *sym = NULL;
    *off = irop_get_stack_offset(op);
  }
}

#define STACKLOC_HASH_SIZE 256
#define STACKLOC_HASH(sym, off) (((uintptr_t)(sym) * 31 + (uint32_t)(off) * 17) % (STACKLOC_HASH_SIZE * 8))

/* The dead-StackLoc scan's read marks: a STACKLOC_HASH_SIZE-byte bit hash of
 * bytes read, the spans read through an address or as a struct, and the
 * highest StackLoc store offset that bounds an open-ended span. */
typedef struct DseStacklocMarks
{
  TCCIRState *ir;
  uint8_t *read;
  DseRange **ranges;
  int *nranges, *ranges_cap;
  int64_t max_stackloc_off;
} DseStacklocMarks;

/* Mark what operand `op` reads of a StackLoc (or of its object, when its
 * address is taken). */
static void dse_mark_stackloc_op(const DseStacklocMarks *m, IROperand op)
{
  TCCIRState *ir = m->ir;
  if (op.is_local && irop_get_vreg(op) < 0)
  {
    const Sym *_sym;
    int64_t _off;
    dse_stackloc_sym_off(ir, op, &_sym, &_off);
    if (op.is_lval)
    {
      int _width;
      switch (op.btype)
      {
      case IROP_BTYPE_INT8:
        _width = 1;
        break;
      case IROP_BTYPE_INT16:
        _width = 2;
        break;
      case IROP_BTYPE_FLOAT32:
        _width = 4;
        break;
      case IROP_BTYPE_INT64:
      case IROP_BTYPE_FLOAT64:
        _width = 8;
        break;
      case IROP_BTYPE_STRUCT:
      {
        /* Struct access: to the end of its object, else up to the max store offset */
        int64_t _shi = m->max_stackloc_off + 5;
        int _olo, _ohi;
        if (!_sym && tcc_ir_frame_object_at(ir, (int)_off, &_olo, &_ohi))
          _shi = _ohi;
        dse_add_range(m->ranges, m->nranges, m->ranges_cap, _sym, _off, _shi);
        _width = 0; /* already handled */
        break;
      }
      default:
        _width = 4;
        break;
      }
      if (op.is_complex)
        _width *= 2;
      for (int _b = 0; _b < _width; _b++)
      {
        uint32_t _h = STACKLOC_HASH(_sym, _off + _b);
        m->read[_h / 8] |= (1 << (_h % 8));
      }
    }
    else
    {
      /* Address taken: its whole object, else up to the max store offset */
      int64_t _rlo = _off, _rhi = m->max_stackloc_off + 5;
      int _olo, _ohi;
      if (!_sym && tcc_ir_frame_object_at(ir, (int)_off, &_olo, &_ohi))
        _rlo = _olo, _rhi = _ohi;
      dse_add_range(m->ranges, m->nranges, m->ranges_cap, _sym, _rlo, _rhi);
    }
  }
}

/* Mark VAR vr (if it is one, within max_var_pos) as read. */
static void dse_mark_var_used(uint8_t *var_used, int max_var_pos, int32_t vr)
{
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos <= max_var_pos)
      var_used[pos / 8] |= (1 << (pos % 8));
  }
}

/* The dead-TEMP sweep's tables: per-TEMP use counts and last defining
 * instruction (grown on demand), and the worklist of TEMPs whose count fell
 * to zero. */
typedef struct DseTempDce
{
  TCCIRState *ir;
  uint16_t *use_count;
  int *def_idx;
  int table_cap;
  int max_tmp_pos;
  int *worklist;
  int wl_top;
} DseTempDce;

static void dse_ensure_cap(DseTempDce *t, int p)
{
  if (p >= t->table_cap)
  {
    int new_cap = t->table_cap * 2;
    while (new_cap <= p)
      new_cap *= 2;
    t->use_count = tcc_realloc(t->use_count, new_cap * sizeof(uint16_t));
    memset(t->use_count + t->table_cap, 0, (new_cap - t->table_cap) * sizeof(uint16_t));
    t->def_idx = tcc_realloc(t->def_idx, new_cap * sizeof(int));
    for (int k = t->table_cap; k < new_cap; k++)
      t->def_idx[k] = -1;
    t->table_cap = new_cap;
  }
  if (p > t->max_tmp_pos)
    t->max_tmp_pos = p;
}

static void dse_inc_use(DseTempDce *t, int p)
{
  if (p >= 0)
  {
    dse_ensure_cap(t, p);
    if (t->use_count[p] < 0xFFFF)
      t->use_count[p]++;
  }
}

/* Pure CALL: by-value-returning aeabi helper, never uses an sret arg. */
static int dse_is_pure_call(TCCIRState *ir, IRQuadCompact *q)
{
  if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
  {
    Sym *callee = tcc_ir_op_src1_sym(ir, q);
    if (callee)
    {
      const char *name = get_tok_str(callee->v, NULL);
      if (name && tcc_ir_is_pure_aeabi(name))
        return 1;
    }
  }
  return 0;
}

/* A volatile access is mandated whether or not its value is wanted, so it is
 * never dead-eligible however ordinary its source operand looks -- a symref
 * can name a volatile global.  A LOAD is dead-eligible only when its source is
 * side-effect-free: immediate, symref, or LOCAL stack slot. */
static int dse_is_dead_eligible(TCCIRState *ir, IRQuadCompact *q)
{
  return !tcc_ir_instr_access_is_volatile(ir, q) &&
         ((dse_op_may_die(q->op) && q->op != TCCIR_OP_LOAD) ||
          (q->op == TCCIR_OP_LOAD &&
           (tcc_ir_op_src1_is_imm(ir, q) || tcc_ir_op_src1_is_sym(ir, q) ||
            (tcc_ir_op_src1_vreg(ir, q) <= -2 && tcc_ir_op_src1_vreg(ir, q) >= -9) ||
            (irop_get_tag(tcc_ir_op_get_src1(ir, q)) == IROP_TAG_STACKOFF && tcc_ir_op_src1_is_local(ir, q)))) ||
          dse_is_pure_call(ir, q));
}

static void dse_dec_temp(DseTempDce *t, IROperand s)
{
  if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
  {
    int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
    if (p >= 0 && p <= t->max_tmp_pos && t->use_count[p] > 0)
      if (--t->use_count[p] == 0)
        t->worklist[t->wl_top++] = p;
  }
}

static void dse_dec_sources(DseTempDce *t, IRQuadCompact *q)
{
  TCCIRState *ir = t->ir;
  if (irop_config[q->op].has_src1)
    dse_dec_temp(t, tcc_ir_op_get_src1(ir, q));
  if (irop_config[q->op].has_src2)
    dse_dec_temp(t, tcc_ir_op_get_src2(ir, q));
  if (q->op == TCCIR_OP_MLA)
    dse_dec_temp(t, tcc_ir_op_get_accum(ir, q));
}

/* NOP the parameters of the pure call at call_idx, releasing their sources;
 * returns how many it NOP'd. */
static int dse_cascade_pure_call_params(DseTempDce *t, int call_idx)
{
  TCCIRState *ir = t->ir;
  int changes = 0;
  IRQuadCompact *cq = &ir->compact_instructions[call_idx];
  int cid = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, cq));
  for (int pi = call_idx - 1; pi >= 0; --pi)
  {
    IRQuadCompact *pq = &ir->compact_instructions[pi];
    if (pq->op == TCCIR_OP_NOP)
      continue;
    if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
      continue;
    int pid = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, pq));
    if (pid != cid)
      continue;
    if (irop_config[pq->op].has_src1)
      dse_dec_temp(t, tcc_ir_op_get_src1(ir, pq));
    if (pq->op == TCCIR_OP_FUNCPARAMVAL)
      dse_dec_temp(t, tcc_ir_op_get_dest(ir, pq));
    LOG_IR_GEN("DCE PURE-CALL: nop PARAM i=%d (call_id=%d)", pi, cid);
    pq->op = TCCIR_OP_NOP;
    changes++;
  }
  return changes;
}

static int tcc_ir_opt_dse__timed(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;


  /* NOP orphaned FUNCPARAM whose call_id has no matching FUNCCALL (left by inlining). */
  {
    uint8_t *has_call = NULL;
    int has_call_bytes = 0;
    int max_call_id = 0;
    int saw_any = 0;

    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID && q->op != TCCIR_OP_FUNCCALLVAL &&
          q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      saw_any = 1;
      int cid = TCCIR_DECODE_CALL_ID((int32_t)tcc_ir_op_src2_imm(ir, q));
      if (cid > max_call_id)
        max_call_id = cid;

      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        int needed_bytes = (cid / 8) + 1;
        if (needed_bytes > has_call_bytes)
        {
          int new_bytes = has_call_bytes ? has_call_bytes * 2 : 32;
          while (new_bytes < needed_bytes)
            new_bytes *= 2;
          has_call = tcc_realloc(has_call, new_bytes);
          memset(has_call + has_call_bytes, 0, new_bytes - has_call_bytes);
          has_call_bytes = new_bytes;
        }
        has_call[cid / 8] |= (1 << (cid % 8));
      }
    }

    if (saw_any && max_call_id > 0)
    {
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
        {
          int cid = TCCIR_DECODE_CALL_ID((int32_t)tcc_ir_op_src2_imm(ir, q));
          int byte_idx = cid / 8;
          int has = (byte_idx < has_call_bytes) && (has_call[byte_idx] & (1 << (cid % 8)));
          if (cid <= max_call_id && !has)
          {
            LOG_IR_GEN("OPTIMIZE: Orphaned PARAM at i=%d (call_id=%d has no CALL)", i, cid);
            q->op = TCCIR_OP_NOP;
          }
        }
      }
    }

    if (has_call)
      tcc_free(has_call);
  }

  /* Must precede use_count[] build: FUNCCALLVOID has no dest TMP to seed the cascade. */
  int pure_call_changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID)
      continue;
    Sym *callee = tcc_ir_op_src1_sym(ir, q);
    if (!callee)
      continue;
    /* Only pure by-value helpers; exclude pure user fns — their sret target may be live. */
    const char *name = get_tok_str(callee->v, NULL);
    if (!name || (!tcc_ir_is_pure_aeabi(name) && !ir_opt_is_pure_helper_name(name) &&
                  !ir_opt_is_readonly_str_helper_name(name)))
      continue;
    LOG_IR_GEN("DCE PURE-CALL: nop FUNCCALLVOID at i=%d (callee=%s)", i,
               get_tok_str(callee->v, NULL) ? get_tok_str(callee->v, NULL) : "?");
    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_NOP;
    pure_call_changes++;
  }

  DseTempDce t;
  t.ir = ir;
  t.max_tmp_pos = 0;
  t.table_cap = 32;
  t.use_count = tcc_mallocz(t.table_cap * sizeof(uint16_t));
  t.def_idx = tcc_malloc(t.table_cap * sizeof(int));
  for (int i = 0; i < t.table_cap; i++)
    t.def_idx[i] = -1;



  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      if (TCCIR_DECODE_VREG_TYPE(tcc_ir_op_src1_vreg(ir, q)) == TCCIR_VREG_TYPE_TEMP)
        dse_inc_use(&t, TCCIR_DECODE_VREG_POSITION(tcc_ir_op_src1_vreg(ir, q)));
    }
    if (irop_config[q->op].has_src2)
    {
      if (TCCIR_DECODE_VREG_TYPE(tcc_ir_op_src2_vreg(ir, q)) == TCCIR_VREG_TYPE_TEMP)
        dse_inc_use(&t, TCCIR_DECODE_VREG_POSITION(tcc_ir_op_src2_vreg(ir, q)));
    }

    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    /* STORE/STORE_INDEXED dest is a pointer use, not a def -- and so is any
     * lvalue dest: `ASM_OUTPUT T3***DEREF***` (an asm "=m"(*p)) writes through
     * T3.  Not counting it deleted `T3 <-- &x`, x stopped being address-taken,
     * and its stale value was forwarded past the asm. */
    const int dest_is_lval_use = irop_config[q->op].has_dest && dest.is_lval;
    if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED || dest_is_lval_use) &&
        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
      dse_inc_use(&t, TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)));
    /* FUNCPARAMVAL dest carries the parameter value — it's a use */
    if (q->op == TCCIR_OP_FUNCPARAMVAL && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
      dse_inc_use(&t, TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)));
    /* MLA accumulator is a use */
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
        dse_inc_use(&t, TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc)));
    }

    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP &&
        q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_FUNCPARAMVAL &&
        !dest_is_lval_use)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
      if (pos >= 0)
      {
        dse_ensure_cap(&t, pos);
        t.def_idx[pos] = i; /* last def wins; OK since we only eliminate when use_count hits 0 */
      }
    }
  }

  if (t.max_tmp_pos == 0)
  {
    tcc_free(t.use_count);
    tcc_free(t.def_idx);
    return pure_call_changes;
  }

  int changes = pure_call_changes;
  LOG_IR_GEN("=== DEAD STORE ELIMINATION START ===");







  t.worklist = tcc_malloc((t.max_tmp_pos + 1) * sizeof(int));
  t.wl_top = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(tcc_ir_op_dest_vreg(ir, q)) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(tcc_ir_op_dest_vreg(ir, q));
    if (pos > t.max_tmp_pos || t.use_count[pos] != 0)
      continue;
    if (!dse_is_dead_eligible(ir, q))
      continue;

    dse_dec_sources(&t, q);
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      LOG_IR_GEN("DCE PURE-CALL: nop CALL at i=%d (result tmp dead)", i);
      changes += dse_cascade_pure_call_params(&t, i);
    }
    q->op = TCCIR_OP_NOP;
    t.def_idx[pos] = -1;
    changes++;
  }

  while (t.wl_top > 0)
  {
    int pos = t.worklist[--t.wl_top];
    int di = t.def_idx[pos];
    if (di < 0 || di >= n)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[di];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (t.use_count[pos] != 0)
      continue; /* someone used it again via a different def */
    if (!dse_is_dead_eligible(ir, q))
      continue;
    if (!irop_config[q->op].has_dest || TCCIR_DECODE_VREG_TYPE(tcc_ir_op_dest_vreg(ir, q)) != TCCIR_VREG_TYPE_TEMP)
      continue;

    dse_dec_sources(&t, q);
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      LOG_IR_GEN("DCE PURE-CALL: nop CALL at i=%d (cascade)", di);
      changes += dse_cascade_pure_call_params(&t, di);
    }
    q->op = TCCIR_OP_NOP;
    t.def_idx[pos] = -1;
    changes++;
  }


  LOG_IR_GEN("=== DEAD STORE ELIMINATION END (marked %d as NOP) ===", changes);
  tcc_free(t.worklist);
  tcc_free(t.def_idx);
  tcc_free(t.use_count);
  const int max_tmp_pos = t.max_tmp_pos;

  /* Eliminate VAR defs never used as a source, unless address-taken. */
  {
    int max_var_pos = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      int32_t vr = tcc_ir_op_dest_vreg(ir, q);
      if (irop_config[q->op].has_dest && vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var_pos)
          max_var_pos = pos;
      }
    }

    if (max_var_pos > 0)
    {
      uint8_t *var_used = tcc_mallocz((max_var_pos + 8) / 8);

      /* Volatile VAR store is a mandated side effect — mark used so its def survives. */
      for (int p = 0; p <= max_var_pos && p < ir->variables_live_intervals_size; p++)
        if (ir->variables_live_intervals[p].is_volatile)
          var_used[p / 8] |= (1 << (p % 8));


      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;

        if (irop_config[q->op].has_src1)
          dse_mark_var_used(var_used, max_var_pos, tcc_ir_op_src1_vreg(ir, q));

        if (irop_config[q->op].has_src2)
          dse_mark_var_used(var_used, max_var_pos, tcc_ir_op_src2_vreg(ir, q));

        /* STORE dest is a use only when non-local (deref); STORE_INDEXED dest is always a base-address use. */
        if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          if (!d.is_local || q->op == TCCIR_OP_STORE_INDEXED)
            dse_mark_var_used(var_used, max_var_pos, irop_get_vreg(d));
        }

        /* FUNCPARAMVAL dest carries the parameter value — it's a use, not a def */
        if (q->op == TCCIR_OP_FUNCPARAMVAL)
          dse_mark_var_used(var_used, max_var_pos, tcc_ir_op_dest_vreg(ir, q));

        /* MLA accumulator (4th operand) is a use not covered by src1/src2. */
        if (q->op == TCCIR_OP_MLA)
          dse_mark_var_used(var_used, max_var_pos, tcc_ir_op_accum_vreg(ir, q));
      }

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE)
          continue;
        const IROperand dest = tcc_ir_op_get_dest(ir, q);
        /* For STORE, only eliminate local stores (not pointer dereferences) */
        if (q->op == TCCIR_OP_STORE && !dest.is_local)
          continue;
        /* Skip stores to parent frame via static chain — externally visible */
        if (dest.is_llocal)
          continue;
        int32_t vr = irop_get_vreg(dest);
        if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
          continue;
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var_pos)
          continue;
        if (var_used[pos / 8] & (1 << (pos % 8)))
          continue;
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
        if (interval && interval->addrtaken)
          continue;
        q->op = TCCIR_OP_NOP;
        changes++;
      }

      tcc_free(var_used);
    }
  }

  /* Eliminate stores to never-read, never-addressed anonymous StackLoc offsets; skip if the function uses a static chain (cross-frame reads/writes). */
  {
    if (ir->has_static_chain)
      goto skip_dead_stackloc;

    for (int i = 0; i < n; i++)
    {
      if (ir->compact_instructions[i].op == TCCIR_OP_SET_CHAIN)
        goto skip_dead_stackloc;
    }
    uint8_t stackloc_read[STACKLOC_HASH_SIZE];
    memset(stackloc_read, 0, sizeof(stackloc_read));
    /* Spans read through an address or as a struct: bounded by the object
     * holding the offset when the frame still has the frontend's layout (C
     * does not let a pointer leave its object), else up to the highest
     * store.  Kept as spans, not hashed bytes, so a big object does not mark
     * everything read. */
    DseRange *ranges = NULL;
    int nranges = 0, ranges_cap = 0;

    /* Max StackLoc STORE offset — bounds how far an address-of range extends. */
    int64_t max_stackloc_off = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_STORE)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local || irop_get_vreg(dest) >= 0)
        continue;
      const Sym *off_sym;
      int64_t off;
      dse_stackloc_sym_off(ir, dest, &off_sym, &off);
      if (off > max_stackloc_off)
        max_stackloc_off = off;
    }
    const DseStacklocMarks slm = {ir, stackloc_read, &ranges, &nranges, &ranges_cap, max_stackloc_off};

#define STACKLOC_TEST(sym, off) (stackloc_read[STACKLOC_HASH(sym, off) / 8] & (1 << (STACKLOC_HASH(sym, off) % 8)))

    /* Identify write-only addr-of TEMPs: address used only in the store pipeline; any other use marks it read. */
    int max_tmp_stackloc = 0;
    int max_var_stackloc = -1;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      IROperand ops[3];
      int nops = 0;
      if (irop_config[q->op].has_dest)
        ops[nops++] = tcc_ir_op_get_dest(ir, q);
      if (irop_config[q->op].has_src1)
        ops[nops++] = tcc_ir_op_get_src1(ir, q);
      if (irop_config[q->op].has_src2)
        ops[nops++] = tcc_ir_op_get_src2(ir, q);
      for (int k = 0; k < nops; k++)
      {
        int32_t vr = irop_get_vreg(ops[k]);
        if (vr >= 0)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos > max_tmp_stackloc)
            max_tmp_stackloc = pos;
          else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos > max_var_stackloc)
            max_var_stackloc = pos;
        }
      }
    }

    /* addr_tmp[pos] = 1 if TMP pos was defined from an Addr[StackLoc] */
    /* addr_tmp_read[pos] = 1 if the addr or pointed-to data is observable */
    uint8_t *addr_tmp = NULL;
    uint8_t *addr_tmp_read = NULL;

    if (max_tmp_stackloc > 0)
    {
      addr_tmp = tcc_mallocz((max_tmp_stackloc + 8) / 8);
      addr_tmp_read = tcc_mallocz((max_tmp_stackloc + 8) / 8);

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_src1)
        {
          if (tcc_ir_op_src1_is_local(ir, q) && !tcc_ir_op_src1_is_lval(ir, q) && tcc_ir_op_src1_vreg(ir, q) < 0)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t dvr = irop_get_vreg(d);
            /* A memory read whose address operand is a StackLoc (a LOAD_INDEXED
             * over a local array) loads the array's CONTENT, it does not define
             * the array's address: the result is not an addr-TMP, and the array
             * must stay marked read so its initializer survives. */
            if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && dse_defines_dest(q, d) &&
                !ir_op_has(q->op, IR_HZ_MEM_READ))
            {
              int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
              if (dpos <= max_tmp_stackloc)
                addr_tmp[dpos / 8] |= (1 << (dpos % 8));
            }
          }
        }
      }

      /* prop_tmp/prop_var record origin addr-TMP position; -1 = not addr-prop, -2 = ambiguous. */
      int *prop_tmp = tcc_mallocz((max_tmp_stackloc + 1) * sizeof(int));
      int *prop_var = max_var_stackloc >= 0 ? tcc_mallocz((max_var_stackloc + 1) * sizeof(int)) : NULL;
      memset(prop_tmp, 0xFF, (max_tmp_stackloc + 1) * sizeof(int)); /* -1 */
      if (prop_var)
        memset(prop_var, 0xFF, (max_var_stackloc + 1) * sizeof(int));

      for (int pos = 0; pos <= max_tmp_stackloc; pos++)
        if (addr_tmp[pos / 8] & (1 << (pos % 8)))
          prop_tmp[pos] = pos;

      int prop_changed = 1;
      while (prop_changed)
      {
        prop_changed = 0;
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP)
            continue;

          if (!irop_config[q->op].has_src1 || !irop_config[q->op].has_dest)
            continue;
          IROperand dest = tcc_ir_op_get_dest(ir, q);
          int32_t svr = tcc_ir_op_src1_vreg(ir, q);
          int32_t dvr = irop_get_vreg(dest);
          if (svr < 0 || dvr < 0)
            continue;

          int stype = TCCIR_DECODE_VREG_TYPE(svr);
          int dtype = TCCIR_DECODE_VREG_TYPE(dvr);
          int spos = TCCIR_DECODE_VREG_POSITION(svr);
          int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
          int origin = -1;

          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN) && stype == TCCIR_VREG_TYPE_TEMP &&
              dtype == TCCIR_VREG_TYPE_VAR && spos <= max_tmp_stackloc && prop_var && dpos <= max_var_stackloc)
            origin = prop_tmp[spos];
          else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && stype == TCCIR_VREG_TYPE_VAR &&
                   dtype == TCCIR_VREG_TYPE_TEMP && prop_var && spos <= max_var_stackloc && dpos <= max_tmp_stackloc)
            origin = prop_var[spos];
          /* VAR -> VAR STORE too: an inlined pointer parameter lands as
           * `V79 <-- V22 [STORE]`, and Phase 3 counts it as a safe copy, so it
           * must carry the origin.  Zig's `get(&t8 + 28)` stepped back 28
           * bytes through such a param; the loads went unseen and t8's stores
           * died. */
          else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD ||
                    (q->op == TCCIR_OP_STORE && irop_dest_defines_vreg(dest))) &&
                   stype == TCCIR_VREG_TYPE_VAR && dtype == TCCIR_VREG_TYPE_VAR && prop_var &&
                   spos <= max_var_stackloc && dpos <= max_var_stackloc)
            origin = prop_var[spos];
          else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_ASSIGN) &&
                   stype == TCCIR_VREG_TYPE_TEMP && dtype == TCCIR_VREG_TYPE_TEMP && spos <= max_tmp_stackloc &&
                   dpos <= max_tmp_stackloc)
            origin = prop_tmp[spos];
          /* Pointer arithmetic into or out of a VAR carries the address as
           * well: Phase 3 counts every ADD/SUB of an address as safe, so one
           * whose result is not followed hides the reads through it.  Zig's
           * `t11 = &t4.path; t12 = *t11` is `V3 <-- T33 ADD #8`, and the
           * stores filling t4 died although the path was read through V3. */
          else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) &&
                   (stype == TCCIR_VREG_TYPE_VAR || dtype == TCCIR_VREG_TYPE_VAR))
          {
            if (stype == TCCIR_VREG_TYPE_TEMP && spos <= max_tmp_stackloc)
              origin = prop_tmp[spos];
            else if (stype == TCCIR_VREG_TYPE_VAR && prop_var && spos <= max_var_stackloc)
              origin = prop_var[spos];
            if ((dtype == TCCIR_VREG_TYPE_TEMP && dpos > max_tmp_stackloc) ||
                (dtype == TCCIR_VREG_TYPE_VAR && (!prop_var || dpos > max_var_stackloc)))
              origin = -1;
          }

          /* -2 travels on: a copy of an ambiguous address is still an address,
           * and the loads through it must reach MARK_ORIGIN_READ. */
          if (origin == -1)
            continue;

          if (dtype == TCCIR_VREG_TYPE_TEMP && dpos <= max_tmp_stackloc)
          {
            if (prop_tmp[dpos] == -1)
            {
              prop_tmp[dpos] = origin;
              prop_changed = 1;
            }
            else if (prop_tmp[dpos] != origin && prop_tmp[dpos] != -2)
            {
              prop_tmp[dpos] = -2;
              prop_changed = 1;
            }
          }
          else if (dtype == TCCIR_VREG_TYPE_VAR && prop_var && dpos <= max_var_stackloc)
          {
            if (prop_var[dpos] == -1)
            {
              prop_var[dpos] = origin;
              prop_changed = 1;
            }
            else if (prop_var[dpos] != origin && prop_var[dpos] != -2)
            {
              prop_var[dpos] = -2;
              prop_changed = 1;
            }
          }
        }
      }

      /* Mark origin addr-TMP read if any propagated value is used outside the write pipeline. */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;

#define GET_ORIGIN(vr)                                                                                                 \
  ({                                                                                                                   \
    int _o = -1;                                                                                                       \
    int _t = TCCIR_DECODE_VREG_TYPE(vr);                                                                               \
    int _p = TCCIR_DECODE_VREG_POSITION(vr);                                                                           \
    if (_t == TCCIR_VREG_TYPE_TEMP && _p <= max_tmp_stackloc)                                                          \
      _o = prop_tmp[_p];                                                                                               \
    else if (_t == TCCIR_VREG_TYPE_VAR && prop_var && _p <= max_var_stackloc)                                          \
      _o = prop_var[_p];                                                                                               \
    _o;                                                                                                                \
  })

#define MARK_ORIGIN_READ(origin)                                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((origin) == -2)                                                                                                \
      memset(addr_tmp_read, 0xFF, (max_tmp_stackloc + 8) / 8);                                                         \
    else if ((origin) >= 0 && (origin) <= max_tmp_stackloc)                                                            \
      addr_tmp_read[(origin) / 8] |= (1 << ((origin) % 8));                                                            \
  } while (0)

        if (irop_config[q->op].has_src1)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0)
          {
            int origin = GET_ORIGIN(vr);
            if (origin != -1)
            {
              int safe = 0;
              if (q->op == TCCIR_OP_STORE && (!s.is_lval || TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR))
              {
                /* For TMP src, is_lval=1 = deref read (unsafe); for VAR src it just means load (safe). */
                IROperand d = tcc_ir_op_get_dest(ir, q);
                int32_t dvr = irop_get_vreg(d);
                /* Only a store that defines the VAR is a copy Phase 2 followed;
                 * one through the pointer the VAR holds sends the address to
                 * memory. */
                if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && irop_dest_defines_vreg(d))
                  safe = 1;
              }
              else if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
              {
                /* Deref read (is_lval=1 on a TMP) makes data observable; VAR is_lval=1 is just a pointer copy. */
                int stype = TCCIR_DECODE_VREG_TYPE(vr);
                if (!s.is_lval || stype == TCCIR_VREG_TYPE_VAR)
                  safe = 1;
              }
              if (!safe)
              {
                LOG_IR_GEN("DSE-SL: Phase3 MARK READ origin=%d at i=%d op=%d src1 is_lval=%d", origin, i, q->op,
                           s.is_lval);
                MARK_ORIGIN_READ(origin);
              }
            }
          }
        }

        /* Check src2: addr-prop as src2 is unusual; conservatively mark read */
        if (irop_config[q->op].has_src2)
        {
          int32_t vr = tcc_ir_op_src2_vreg(ir, q);
          if (vr >= 0)
          {
            int origin = GET_ORIGIN(vr);
            if (origin != -1)
            {
              LOG_IR_GEN("DSE-SL: Phase3 MARK READ origin=%d at i=%d op=%d src2", origin, i, q->op);
              MARK_ORIGIN_READ(origin);
            }
          }
        }

        /* MLA accumulator (4th operand) is a use not surfaced by src1/src2 — conservatively mark origin read. */
        if (q->op == TCCIR_OP_MLA)
        {
          IROperand s = tcc_ir_op_get_accum(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0)
          {
            int origin = GET_ORIGIN(vr);
            if (origin != -1)
            {
              LOG_IR_GEN("DSE-SL: Phase3 MARK READ origin=%d at i=%d op=%d accum", origin, i, q->op);
              MARK_ORIGIN_READ(origin);
            }
          }
        }

        /* STORE dest as an addr-prop TMP is a deref write — safe, no marking,
         * unless it is a volatile access: that is observable by itself, so the
         * address and the object stay. */
        if (irop_config[q->op].has_dest && tcc_ir_instr_access_is_volatile(ir, q))
        {
          const int32_t dvr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
          if (dvr >= 0)
          {
            const int origin = GET_ORIGIN(dvr);
            if (origin != -1)
              MARK_ORIGIN_READ(origin);
          }
        }

#undef GET_ORIGIN
#undef MARK_ORIGIN_READ
      }

      tcc_free(prop_tmp);
      if (prop_var)
        tcc_free(prop_var);
    }

    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Mark StackLoc operand: is_lval=1 → exact-offset read; is_lval=0 → address-of range; write-only addr-of skipped. */

      /* Check all operands for StackLoc reads / address-taken */
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        /* Skip range marking for address-of feeding a write-only TMP (writes only, no read). */
        if (s.is_local && !s.is_lval && irop_get_vreg(s) < 0 && addr_tmp != NULL &&
            dse_defines_dest(q, tcc_ir_op_get_dest(ir, q)))
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (TCC_LOG_IR_GEN)
          {
            fprintf(stderr, "[IR_GEN] DSE-SL: addr-of check i=%d dvr=0x%x type=%d pos=%d max=%d", i, (unsigned)dvr,
                    dvr >= 0 ? TCCIR_DECODE_VREG_TYPE(dvr) : -1, dvr >= 0 ? TCCIR_DECODE_VREG_POSITION(dvr) : -1,
                    max_tmp_stackloc);
            if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
            {
              int _dp = TCCIR_DECODE_VREG_POSITION(dvr);
              fprintf(stderr, " addr_tmp=%d addr_tmp_read=%d",
                      !!(_dp <= max_tmp_stackloc && (addr_tmp[_dp / 8] & (1 << (_dp % 8)))),
                      !!(_dp <= max_tmp_stackloc && (addr_tmp_read[_dp / 8] & (1 << (_dp % 8)))));
            }
            fprintf(stderr, "\n");
          }
          if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
          {
            int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
            if (dpos <= max_tmp_stackloc && (addr_tmp[dpos / 8] & (1 << (dpos % 8))) &&
                !(addr_tmp_read[dpos / 8] & (1 << (dpos % 8))))
            {
              LOG_IR_GEN("DSE-SL: SKIP addr-of at i=%d (write-only T%d)", i, dpos);
              goto after_src1_mark;
            }
          }
        }
        LOG_IR_GEN("DSE-SL: MARK src1 at i=%d op=%d is_lval=%d is_local=%d", i, q->op, s.is_lval, s.is_local);
        /* FUNCPARAM passing a STRUCT from a StackLoc may span multiple words — force range marking (scalars keep width-based). */
        if ((q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) && s.is_local &&
            irop_get_vreg(s) < 0 && s.btype == IROP_BTYPE_STRUCT)
        {
          IROperand s_range = s;
          s_range.is_lval = 0;
          dse_mark_stackloc_op(&slm, s_range);
        }
        else
        {
          dse_mark_stackloc_op(&slm, s);
        }
      after_src1_mark:;
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        LOG_IR_GEN("DSE-SL: MARK src2 at i=%d op=%d is_lval=%d is_local=%d", i, q->op, s.is_lval, s.is_local);
        dse_mark_stackloc_op(&slm, s);
      }
      /* MLA accumulator (4th operand) may reference a StackLoc */
      if (q->op == TCCIR_OP_MLA)
      {
        IROperand acc = tcc_ir_op_get_accum(ir, q);
        dse_mark_stackloc_op(&slm, acc);
      }
    }

#if TCC_LOG_IR_GEN
    {
      int any_set = 0;
      for (int bi = 0; bi <= max_stackloc_off / 8; bi++)
        if (stackloc_read[bi])
        {
          any_set = 1;
          break;
        }
      LOG_IR_GEN("DSE-SL: stackloc_read has %s bits set, max_stackloc_off=%lld", any_set ? "some" : "NO",
                 (long long)max_stackloc_off);
    }
#endif
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_STORE)
        continue;
      /* a volatile store is observable even into a slot nothing reads */
      if (tcc_ir_instr_access_is_volatile(ir, q))
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local || irop_get_vreg(dest) >= 0)
        continue; /* Not an anonymous StackLoc */
      if (dest.is_llocal)
        continue; /* Store to parent frame via static chain — externally visible */

      const Sym *sym;
      int64_t off;
      dse_stackloc_sym_off(ir, dest, &sym, &off);

      /* A store is live if ANY byte it writes is read, not just its first one:
       * `struct S x = g;` stores 4 bytes at StackLoc[-4] while `x.topfield`
       * reads 2 bytes at StackLoc[-2].  Testing only the base byte declared
       * that store dead and left the field reading an uninitialized slot. */
      int swidth = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
      if (swidth <= 0 || irop_get_btype(dest) == IROP_BTYPE_STRUCT)
        swidth = -1; /* unknown extent: keep the store */
      else if (dest.is_complex)
        swidth *= 2;
      int live = (swidth < 0) || dse_ranges_overlap(ranges, nranges, sym, off, off + swidth);
      for (int b = 0; !live && b < swidth; b++)
        if (STACKLOC_TEST(sym, off + b))
          live = 1;

      LOG_IR_GEN("DSE-SL: i=%d off=%lld sym=%p width=%d live=%d", i, (long long)off, (void *)sym, swidth, live);
      if (!live)
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }

    /* Eliminate dead pointer chains: NOP the LEA of a write-only addr-TMP with a dead range, then forward-propagate deadness. */
    if (addr_tmp != NULL)
    {
      int max_var_pos = -1;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_dest)
        {
          int32_t vr = tcc_ir_op_dest_vreg(ir, q);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos > max_var_pos)
              max_var_pos = pos;
          }
        }
      }

      /* Deadness is only sound for a vreg whose EVERY definition is dead: a
       * `?:` result or a reassigned pointer VAR may hold the dead local's
       * address on one path and a live pointer (a parameter, a load) on
       * another.  So: count the definitions, and compute the chain on a scratch
       * `kill` map.  An instruction that survives yet reads a vreg the chain
       * calls dead would read a deleted definition -- that vreg is tainted and
       * the chain recomputed without it, until nothing survivor-visible is
       * dead. */
      const int ntmp = max_tmp_stackloc + 1, nvar = max_var_pos + 1;
      int *def_tmp = tcc_mallocz(sizeof(int) * (ntmp > 0 ? ntmp : 1));
      int *def_var = tcc_mallocz(sizeof(int) * (nvar > 0 ? nvar : 1));
      uint8_t *dead_tmp = tcc_mallocz((max_tmp_stackloc + 8) / 8);
      uint8_t *dead_var = max_var_pos >= 0 ? tcc_mallocz((max_var_pos + 8) / 8) : NULL;
      uint8_t *taint_tmp = tcc_mallocz((max_tmp_stackloc + 8) / 8);
      uint8_t *taint_var = max_var_pos >= 0 ? tcc_mallocz((max_var_pos + 8) / 8) : NULL;
      uint8_t *kill = tcc_mallocz(n);
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        const int32_t dvr = irop_get_vreg(d);
        if (dvr < 0 || q->op == TCCIR_OP_STORE_INDEXED || !irop_dest_defines_vreg(d))
          continue;
        const int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && dpos < ntmp)
          def_tmp[dpos]++;
        else if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && dpos < nvar)
          def_var[dpos]++;
      }
#define DSE_VREG_IN(set_tmp, set_var, vr)                                                                              \
  ({                                                                                                                   \
    int _in = 0;                                                                                                       \
    if ((vr) >= 0)                                                                                                     \
    {                                                                                                                  \
      const int _p = TCCIR_DECODE_VREG_POSITION(vr);                                                                   \
      if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && _p <= max_tmp_stackloc)                                \
        _in = ((set_tmp)[_p / 8] >> (_p % 8)) & 1;                                                                     \
      else if ((set_var) && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && _p <= max_var_pos)                   \
        _in = ((set_var)[_p / 8] >> (_p % 8)) & 1;                                                                     \
    }                                                                                                                  \
    _in;                                                                                                               \
  })
#define DSE_VREG_SET(set_tmp, set_var, vr)                                                                             \
  do                                                                                                                   \
  {                                                                                                                    \
    const int _p = TCCIR_DECODE_VREG_POSITION(vr);                                                                     \
    if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && _p <= max_tmp_stackloc)                                  \
      (set_tmp)[_p / 8] |= (1 << (_p % 8));                                                                            \
    else if ((set_var) && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && _p <= max_var_pos)                     \
      (set_var)[_p / 8] |= (1 << (_p % 8));                                                                            \
  } while (0)

      for (;;)
      {
        memset(dead_tmp, 0, (max_tmp_stackloc + 8) / 8);
        if (dead_var)
          memset(dead_var, 0, (max_var_pos + 8) / 8);
        memset(kill, 0, n);

        /* Seeds: LEA/Addr for write-only addr-TMPs whose StackLoc range is dead. */
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_src1)
            continue;
          IROperand s = tcc_ir_op_get_src1(ir, q);
          if (!s.is_local || s.is_lval || irop_get_vreg(s) >= 0)
            continue;
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
            continue;
          int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
          if (dpos > max_tmp_stackloc || def_tmp[dpos] != 1 || (taint_tmp[dpos / 8] & (1 << (dpos % 8))))
            continue;
          if (!(addr_tmp[dpos / 8] & (1 << (dpos % 8))))
            continue;
          if (addr_tmp_read[dpos / 8] & (1 << (dpos % 8)))
            continue;

          /* Verify range dead: any offset in [base, max+4] still read? Catches mixed read/write-only TMPs to one StackLoc. */
          const Sym *addr_sym;
          int64_t addr_off;
          dse_stackloc_sym_off(ir, s, &addr_sym, &addr_off);
          int range_is_read = 0;
          int64_t range_end = max_stackloc_off + 4;
          int64_t range_len = range_end - addr_off + 1;
          if (range_len > STACKLOC_HASH_SIZE * 8 ||
              dse_ranges_overlap(ranges, nranges, addr_sym, addr_off, range_end + 1))
          {
            range_is_read = 1; /* Too large — conservatively assume read */
          }
          else
          {
            for (int64_t k = addr_off; k <= range_end; k++)
            {
              if (STACKLOC_TEST(addr_sym, k))
              {
                range_is_read = 1;
                break;
              }
            }
          }
          if (range_is_read)
            continue;

          kill[i] = 1;
          dead_tmp[dpos / 8] |= (1 << (dpos % 8));
        }

        /* Forward-propagate deadness: kill instructions whose sources are dead, then mark their dests dead. */
        int prop_changed = 1;
        while (prop_changed)
        {
          prop_changed = 0;
          for (int i = 0; i < n; i++)
          {
            IRQuadCompact *q = &ir->compact_instructions[i];
            if (q->op == TCCIR_OP_NOP || kill[i])
              continue;
            /* Only propagate through data-flow instructions */
            if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
                q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
                q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID || q->op == TCCIR_OP_SWITCH_TABLE)
              continue;

            int has_dead_src = 0;
            if (irop_config[q->op].has_src1)
              has_dead_src = DSE_VREG_IN(dead_tmp, dead_var, irop_get_vreg(tcc_ir_op_get_src1(ir, q)));

            /* STORE/STORE_INDEXED dest is the pointer base - a dead base kills the store. */
            if (!has_dead_src && (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED))
            {
              const int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
              if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
                has_dead_src = DSE_VREG_IN(dead_tmp, dead_var, vr);
            }
            if (!has_dead_src)
              continue;

            /* The destination turns dead only when this is its sole definition. */
            int32_t dvr = -1;
            if (irop_config[q->op].has_dest)
            {
              IROperand d = tcc_ir_op_get_dest(ir, q);
              if (irop_get_vreg(d) >= 0 && irop_dest_defines_vreg(d) && q->op != TCCIR_OP_STORE_INDEXED)
              {
                dvr = irop_get_vreg(d);
                const int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
                const int single = TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP
                                       ? (dpos < ntmp && def_tmp[dpos] == 1 && !(taint_tmp[dpos / 8] & (1 << (dpos % 8))))
                                       : (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && dpos < nvar &&
                                          def_var[dpos] == 1 && !(taint_var[dpos / 8] & (1 << (dpos % 8))));
                if (!single)
                  continue;
              }
            }
            if (dvr >= 0)
              DSE_VREG_SET(dead_tmp, dead_var, dvr);
            kill[i] = 1;
            prop_changed = 1;
          }
        }

        /* A survivor that reads a dead vreg would read a deleted definition. */
        int tainted = 0;
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP || kill[i])
            continue;
          int32_t uses[4];
          int nuse = 0;
          if (irop_config[q->op].has_src1)
            uses[nuse++] = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
          if (irop_config[q->op].has_src2)
            uses[nuse++] = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
          if (q->op == TCCIR_OP_MLA)
            uses[nuse++] = irop_get_vreg(tcc_ir_op_get_accum(ir, q));
          if (irop_config[q->op].has_dest)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            if (!irop_dest_defines_vreg(d) || q->op == TCCIR_OP_STORE_POSTINC)
              uses[nuse++] = irop_get_vreg(d); /* the base a store writes through */
          }
          for (int k = 0; k < nuse; k++)
            if (uses[k] >= 0 && DSE_VREG_IN(dead_tmp, dead_var, uses[k]))
            {
              DSE_VREG_SET(taint_tmp, taint_var, uses[k]);
              tainted = 1;
            }
        }
        if (!tainted)
          break;
      }
#undef DSE_VREG_IN
#undef DSE_VREG_SET

      for (int i = 0; i < n; i++)
        if (kill[i])
        {
          ir->compact_instructions[i].op = TCCIR_OP_NOP;
          changes++;
        }
      tcc_free(kill);
      tcc_free(taint_tmp);
      if (taint_var)
        tcc_free(taint_var);
      tcc_free(def_tmp);
      tcc_free(def_var);
      tcc_free(dead_tmp);
      if (dead_var)
        tcc_free(dead_var);
    }

    if (addr_tmp)
      tcc_free(addr_tmp);
    if (addr_tmp_read)
      tcc_free(addr_tmp_read);
    tcc_free(ranges);

#undef STACKLOC_HASH_SIZE
#undef STACKLOC_HASH
#undef STACKLOC_TEST
  skip_dead_stackloc:;
  }

  /* Re-run TMP DCE: StackLoc store elimination may have freed TMPs the first pass couldn't. */
  {
    uint8_t *used2 = tcc_mallocz((max_tmp_pos + 8) / 8);
    int iter2_changes;
    do
    {
      iter2_changes = 0;
      memset(used2, 0, (max_tmp_pos + 8) / 8);
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_src1)
        {
          if (TCCIR_DECODE_VREG_TYPE(tcc_ir_op_src1_vreg(ir, q)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(tcc_ir_op_src1_vreg(ir, q));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        if (irop_config[q->op].has_src2)
        {
          if (TCCIR_DECODE_VREG_TYPE(tcc_ir_op_src2_vreg(ir, q)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(tcc_ir_op_src2_vreg(ir, q));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        /* STORE/STORE_INDEXED dest is a use (pointer deref) */
        {
          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED || tcc_ir_op_dest_is_lval(ir, q)) &&
              TCCIR_DECODE_VREG_TYPE(tcc_ir_op_dest_vreg(ir, q)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(tcc_ir_op_dest_vreg(ir, q));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
          if (q->op == TCCIR_OP_FUNCPARAMVAL && TCCIR_DECODE_VREG_TYPE(tcc_ir_op_dest_vreg(ir, q)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(tcc_ir_op_dest_vreg(ir, q));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        if (q->op == TCCIR_OP_MLA)
        {
          const IROperand acc = tcc_ir_op_get_accum(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
      }

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        const IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP &&
            !dest.is_lval) /* an lvalue dest is a write through the TEMP, never its def */
        {
          int is_dead_eligible = dse_op_may_die(q->op) && q->op != TCCIR_OP_LOAD;
          if (!is_dead_eligible && q->op == TCCIR_OP_LOAD)
          {
            if (tcc_ir_op_src1_is_imm(ir, q))
              is_dead_eligible = 1;
          }
          /* Same rule as the first sweep (DSE_IS_DEAD_ELIGIBLE): an ALU op can
           * embed a volatile deref operand -- `(void)(*p + 1)` is one ADD --
           * and that read is mandated whether or not the sum is wanted. */
          if (is_dead_eligible && tcc_ir_instr_access_is_volatile(ir, q))
            is_dead_eligible = 0;
          if (is_dead_eligible)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
            if (pos <= max_tmp_pos && !(used2[pos / 8] & (1 << (pos % 8))))
            {
              q->op = TCCIR_OP_NOP;
              iter2_changes++;
            }
          }
        }
      }
      changes += iter2_changes;
    } while (iter2_changes > 0);
    tcc_free(used2);
  }

  return changes;
}
