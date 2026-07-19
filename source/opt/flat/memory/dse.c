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

static int tcc_ir_opt_dse__timed(TCCIRState *ir);
int tcc_ir_opt_dse(TCCIRState *ir)
{
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_dse__timed(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_dse__timed(ir);
  tcc_pass_timing_add("dse", tcc_pass_clk_us() - _t);
  return _r;
}

static int dse_operand_is_wide(IROperand op)
{
  return op.btype == IROP_BTYPE_INT64 || op.btype == IROP_BTYPE_FLOAT64;
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

static int tcc_ir_opt_dse__timed(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_dest && dse_operand_is_wide(tcc_ir_op_get_dest(ir, q)))
      return 0;
    if (irop_config[q->op].has_src1 && dse_operand_is_wide(tcc_ir_op_get_src1(ir, q)))
      return 0;
    if (irop_config[q->op].has_src2 && dse_operand_is_wide(tcc_ir_op_get_src2(ir, q)))
      return 0;
    if (q->op == TCCIR_OP_MLA && dse_operand_is_wide(tcc_ir_op_get_accum(ir, q)))
      return 0;
  }

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
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(ir, src2));
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
          IROperand src2 = tcc_ir_op_get_src2(ir, q);
          int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(ir, src2));
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
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
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

  int max_tmp_pos = 0;
  int table_cap = 32;
  uint16_t *use_count = tcc_mallocz(table_cap * sizeof(uint16_t));
  int *def_idx = tcc_malloc(table_cap * sizeof(int));
  for (int i = 0; i < table_cap; i++)
    def_idx[i] = -1;

#define DSE_ENSURE_CAP(_p)                                                                                             \
  do                                                                                                                   \
  {                                                                                                                    \
    int _pp = (_p);                                                                                                    \
    if (_pp >= table_cap)                                                                                              \
    {                                                                                                                  \
      int _new_cap = table_cap * 2;                                                                                    \
      while (_new_cap <= _pp)                                                                                          \
        _new_cap *= 2;                                                                                                 \
      use_count = tcc_realloc(use_count, _new_cap * sizeof(uint16_t));                                                 \
      memset(use_count + table_cap, 0, (_new_cap - table_cap) * sizeof(uint16_t));                                     \
      def_idx = tcc_realloc(def_idx, _new_cap * sizeof(int));                                                          \
      for (int _k = table_cap; _k < _new_cap; _k++)                                                                    \
        def_idx[_k] = -1;                                                                                              \
      table_cap = _new_cap;                                                                                            \
    }                                                                                                                  \
    if (_pp > max_tmp_pos)                                                                                             \
      max_tmp_pos = _pp;                                                                                               \
  } while (0)

#define DSE_INC_USE(_pos)                                                                                              \
  do                                                                                                                   \
  {                                                                                                                    \
    int _p = (_pos);                                                                                                   \
    if (_p >= 0)                                                                                                       \
    {                                                                                                                  \
      DSE_ENSURE_CAP(_p);                                                                                              \
      if (use_count[_p] < 0xFFFF)                                                                                      \
        use_count[_p]++;                                                                                               \
    }                                                                                                                  \
  } while (0)

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      const IROperand s = tcc_ir_op_get_src1(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s)));
    }
    if (irop_config[q->op].has_src2)
    {
      const IROperand s = tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s)));
    }

    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    /* STORE/STORE_INDEXED dest is a pointer use, not a def */
    if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
      DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)));
    /* FUNCPARAMVAL dest carries the parameter value — it's a use */
    if (q->op == TCCIR_OP_FUNCPARAMVAL && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
      DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)));
    /* MLA accumulator is a use */
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc)));
    }

    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP &&
        q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_FUNCPARAMVAL)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
      if (pos >= 0)
      {
        DSE_ENSURE_CAP(pos);
        def_idx[pos] = i; /* last def wins; OK since we only eliminate when use_count hits 0 */
      }
    }
  }

  if (max_tmp_pos == 0)
  {
    tcc_free(use_count);
    tcc_free(def_idx);
    return pure_call_changes;
  }

  int changes = pure_call_changes;
  LOG_IR_GEN("=== DEAD STORE ELIMINATION START ===");

  /* Pure CALL: by-value-returning aeabi helper, never uses an sret arg. */
#define DSE_IS_PURE_CALL(_q)                                                                                           \
  ({                                                                                                                   \
    int _pure = 0;                                                                                                     \
    if ((_q)->op == TCCIR_OP_FUNCCALLVAL || (_q)->op == TCCIR_OP_FUNCCALLVOID)                                         \
    {                                                                                                                  \
      Sym *_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, (_q)));                                                \
      if (_callee)                                                                                                     \
      {                                                                                                                \
        const char *_name = get_tok_str(_callee->v, NULL);                                                             \
        if (_name && tcc_ir_is_pure_aeabi(_name))                                                                      \
          _pure = 1;                                                                                                   \
      }                                                                                                                \
    }                                                                                                                  \
    _pure;                                                                                                             \
  })

  /* A LOAD is dead-eligible only when its source is side-effect-free: immediate, symref, or LOCAL stack slot. */
#define DSE_IS_DEAD_ELIGIBLE(_q)                                                                                       \
  (((_q)->op != TCCIR_OP_STORE && (_q)->op != TCCIR_OP_STORE_INDEXED && (_q)->op != TCCIR_OP_STORE_POSTINC &&          \
    (_q)->op != TCCIR_OP_LOAD_POSTINC && (_q)->op != TCCIR_OP_LOAD && (_q)->op != TCCIR_OP_FUNCCALLVAL &&              \
    (_q)->op != TCCIR_OP_FUNCCALLVOID && (_q)->op != TCCIR_OP_FUNCPARAMVAL && (_q)->op != TCCIR_OP_FUNCPARAMVOID) ||   \
   ((_q)->op == TCCIR_OP_LOAD &&                                                                                       \
    (irop_is_immediate(tcc_ir_op_get_src1(ir, (_q))) || tcc_ir_op_get_src1(ir, (_q)).is_sym ||                         \
     (irop_get_vreg(tcc_ir_op_get_src1(ir, (_q))) <= -2 && irop_get_vreg(tcc_ir_op_get_src1(ir, (_q))) >= -9) ||       \
     (irop_get_tag(tcc_ir_op_get_src1(ir, (_q))) == IROP_TAG_STACKOFF &&                                               \
      tcc_ir_op_get_src1(ir, (_q)).is_local))) ||                                                                      \
   DSE_IS_PURE_CALL(_q))

#define DSE_DEC_TEMP(_op)                                                                                             \
  do                                                                                                                  \
  {                                                                                                                   \
    const IROperand _s = (_op);                                                                                       \
    if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(_s)) == TCCIR_VREG_TYPE_TEMP)                                            \
    {                                                                                                                 \
      int _p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(_s));                                                         \
      if (_p >= 0 && _p <= max_tmp_pos && use_count[_p] > 0)                                                          \
        if (--use_count[_p] == 0)                                                                                     \
          worklist[wl_top++] = _p;                                                                                    \
    }                                                                                                                 \
  } while (0)

#define DSE_DEC_SOURCES(_q)                                                                                           \
  do                                                                                                                  \
  {                                                                                                                   \
    if (irop_config[(_q)->op].has_src1)                                                                               \
      DSE_DEC_TEMP(tcc_ir_op_get_src1(ir, (_q)));                                                                     \
    if (irop_config[(_q)->op].has_src2)                                                                               \
      DSE_DEC_TEMP(tcc_ir_op_get_src2(ir, (_q)));                                                                     \
    if ((_q)->op == TCCIR_OP_MLA)                                                                                     \
      DSE_DEC_TEMP(tcc_ir_op_get_accum(ir, (_q)));                                                                    \
  } while (0)

#define DSE_CASCADE_PURE_CALL_PARAMS(_call_idx)                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    int _ci = (_call_idx);                                                                                             \
    IRQuadCompact *_cq = &ir->compact_instructions[_ci];                                                               \
    int _cid = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, _cq)));                     \
    for (int _pi = _ci - 1; _pi >= 0; --_pi)                                                                           \
    {                                                                                                                  \
      IRQuadCompact *_pq = &ir->compact_instructions[_pi];                                                             \
      if (_pq->op == TCCIR_OP_NOP)                                                                                     \
        continue;                                                                                                      \
      if (_pq->op != TCCIR_OP_FUNCPARAMVAL && _pq->op != TCCIR_OP_FUNCPARAMVOID)                                       \
        continue;                                                                                                      \
      int _pid = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, _pq)));                   \
      if (_pid != _cid)                                                                                                \
        continue;                                                                                                      \
      if (irop_config[_pq->op].has_src1)                                                                               \
        DSE_DEC_TEMP(tcc_ir_op_get_src1(ir, _pq));                                                                     \
      if (_pq->op == TCCIR_OP_FUNCPARAMVAL)                                                                            \
        DSE_DEC_TEMP(tcc_ir_op_get_dest(ir, _pq));                                                                     \
      LOG_IR_GEN("DCE PURE-CALL: nop PARAM i=%d (call_id=%d)", _pi, _cid);                                             \
      _pq->op = TCCIR_OP_NOP;                                                                                          \
      changes++;                                                                                                       \
    }                                                                                                                  \
  } while (0)

  int *worklist = tcc_malloc((max_tmp_pos + 1) * sizeof(int));
  int wl_top = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_config[q->op].has_dest)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
    if (pos > max_tmp_pos || use_count[pos] != 0)
      continue;
    if (!DSE_IS_DEAD_ELIGIBLE(q))
      continue;

    DSE_DEC_SOURCES(q);
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      LOG_IR_GEN("DCE PURE-CALL: nop CALL at i=%d (result tmp dead)", i);
      DSE_CASCADE_PURE_CALL_PARAMS(i);
    }
    q->op = TCCIR_OP_NOP;
    def_idx[pos] = -1;
    changes++;
  }

  while (wl_top > 0)
  {
    int pos = worklist[--wl_top];
    int di = def_idx[pos];
    if (di < 0 || di >= n)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[di];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (use_count[pos] != 0)
      continue; /* someone used it again via a different def */
    if (!DSE_IS_DEAD_ELIGIBLE(q))
      continue;
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_config[q->op].has_dest || TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) != TCCIR_VREG_TYPE_TEMP)
      continue;

    DSE_DEC_SOURCES(q);
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      LOG_IR_GEN("DCE PURE-CALL: nop CALL at i=%d (cascade)", di);
      DSE_CASCADE_PURE_CALL_PARAMS(di);
    }
    q->op = TCCIR_OP_NOP;
    def_idx[pos] = -1;
    changes++;
  }

#undef DSE_INC_USE
#undef DSE_ENSURE_CAP
#undef DSE_IS_DEAD_ELIGIBLE
#undef DSE_IS_PURE_CALL
#undef DSE_CASCADE_PURE_CALL_PARAMS
#undef DSE_DEC_TEMP
#undef DSE_DEC_SOURCES

  LOG_IR_GEN("=== DEAD STORE ELIMINATION END (marked %d as NOP) ===", changes);
  tcc_free(worklist);
  tcc_free(def_idx);
  tcc_free(use_count);

  /* Eliminate VAR defs never used as a source, unless address-taken. */
  {
    int max_var_pos = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      const IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
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

#define MARK_VAR_USED(_op)                                                                                            \
  do                                                                                                                  \
  {                                                                                                                   \
    int32_t _vr = irop_get_vreg(_op);                                                                                 \
    if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_VAR)                                               \
    {                                                                                                                 \
      int _pos = TCCIR_DECODE_VREG_POSITION(_vr);                                                                     \
      if (_pos <= max_var_pos)                                                                                        \
        var_used[_pos / 8] |= (1 << (_pos % 8));                                                                      \
    }                                                                                                                 \
  } while (0)

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;

        if (irop_config[q->op].has_src1)
          MARK_VAR_USED(tcc_ir_op_get_src1(ir, q));

        if (irop_config[q->op].has_src2)
          MARK_VAR_USED(tcc_ir_op_get_src2(ir, q));

        /* STORE dest is a use only when non-local (deref); STORE_INDEXED dest is always a base-address use. */
        if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          if (!d.is_local || q->op == TCCIR_OP_STORE_INDEXED)
            MARK_VAR_USED(d);
        }

        /* FUNCPARAMVAL dest carries the parameter value — it's a use, not a def */
        if (q->op == TCCIR_OP_FUNCPARAMVAL)
          MARK_VAR_USED(tcc_ir_op_get_dest(ir, q));

        /* MLA accumulator (4th operand) is a use not covered by src1/src2. */
        if (q->op == TCCIR_OP_MLA)
          MARK_VAR_USED(tcc_ir_op_get_accum(ir, q));
      }
#undef MARK_VAR_USED

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
#define STACKLOC_HASH_SIZE 256
    uint8_t stackloc_read[STACKLOC_HASH_SIZE];
    memset(stackloc_read, 0, sizeof(stackloc_read));

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

#define STACKLOC_HASH(sym, off) (((uintptr_t)(sym) * 31 + (uint32_t)(off) * 17) % (STACKLOC_HASH_SIZE * 8))
#define STACKLOC_SET(sym, off)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    uint32_t _h = STACKLOC_HASH(sym, off);                                                                             \
    stackloc_read[_h / 8] |= (1 << (_h % 8));                                                                          \
  } while (0)
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
          IROperand s = tcc_ir_op_get_src1(ir, q);
          if (s.is_local && !s.is_lval && irop_get_vreg(s) < 0)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t dvr = irop_get_vreg(d);
            if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
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
          IROperand src = tcc_ir_op_get_src1(ir, q);
          IROperand dest = tcc_ir_op_get_dest(ir, q);
          int32_t svr = irop_get_vreg(src);
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
          else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && stype == TCCIR_VREG_TYPE_VAR &&
                   dtype == TCCIR_VREG_TYPE_VAR && prop_var && spos <= max_var_stackloc && dpos <= max_var_stackloc)
            origin = prop_var[spos];
          else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_ASSIGN) &&
                   stype == TCCIR_VREG_TYPE_TEMP && dtype == TCCIR_VREG_TYPE_TEMP && spos <= max_tmp_stackloc &&
                   dpos <= max_tmp_stackloc)
            origin = prop_tmp[spos];

          if (origin < 0)
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
                if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
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
          IROperand s = tcc_ir_op_get_src2(ir, q);
          int32_t vr = irop_get_vreg(s);
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

        /* STORE dest as an addr-prop TMP is a deref write — safe, no marking. */

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
#define MARK_STACKLOC_OP(op)                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((op).is_local && irop_get_vreg(op) < 0)                                                                        \
    {                                                                                                                  \
      const Sym *_sym;                                                                                                 \
      int64_t _off;                                                                                                    \
      dse_stackloc_sym_off(ir, (op), &_sym, &_off);                                                                    \
      if ((op).is_lval)                                                                                                \
      {                                                                                                                \
        int _width;                                                                                                    \
        switch ((op).btype)                                                                                            \
        {                                                                                                              \
        case IROP_BTYPE_INT8:                                                                                          \
          _width = 1;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_INT16:                                                                                         \
          _width = 2;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_FLOAT32:                                                                                       \
          _width = 4;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_INT64:                                                                                         \
        case IROP_BTYPE_FLOAT64:                                                                                       \
          _width = 8;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_STRUCT:                                                                                        \
        {                                                                                                              \
          /* Struct access — conservatively mark range up to max store offset */                                       \
          int64_t _send = max_stackloc_off + 4;                                                                        \
          for (int64_t _s = _off; _s <= _send; _s++)                                                                   \
            STACKLOC_SET(_sym, _s);                                                                                    \
          _width = 0; /* already handled */                                                                            \
          break;                                                                                                       \
        }                                                                                                              \
        default:                                                                                                       \
          _width = 4;                                                                                                  \
          break;                                                                                                       \
        }                                                                                                              \
        if ((op).is_complex)                                                                                           \
          _width *= 2;                                                                                                 \
        for (int _b = 0; _b < _width; _b++)                                                                            \
          STACKLOC_SET(_sym, _off + _b);                                                                               \
      }                                                                                                                \
      else                                                                                                             \
      {                                                                                                                \
        int64_t _range_end = max_stackloc_off + 4;                                                                     \
        int64_t _range_len = _range_end - _off + 1;                                                                    \
        if (_range_len > STACKLOC_HASH_SIZE * 8)                                                                       \
        {                                                                                                              \
          memset(stackloc_read, 0xFF, sizeof(stackloc_read));                                                          \
        }                                                                                                              \
        else if (_range_len > 0)                                                                                       \
        {                                                                                                              \
          for (int64_t _k = _off; _k <= _range_end; _k++)                                                              \
            STACKLOC_SET(_sym, _k);                                                                                    \
        }                                                                                                              \
      }                                                                                                                \
    }                                                                                                                  \
  } while (0)

      /* Check all operands for StackLoc reads / address-taken */
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        /* Skip range marking for address-of feeding a write-only TMP (writes only, no read). */
        if (s.is_local && !s.is_lval && irop_get_vreg(s) < 0 && addr_tmp != NULL)
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
          MARK_STACKLOC_OP(s_range);
        }
        else
        {
          MARK_STACKLOC_OP(s);
        }
      after_src1_mark:;
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        LOG_IR_GEN("DSE-SL: MARK src2 at i=%d op=%d is_lval=%d is_local=%d", i, q->op, s.is_lval, s.is_local);
        MARK_STACKLOC_OP(s);
      }
      /* MLA accumulator (4th operand) may reference a StackLoc */
      if (q->op == TCCIR_OP_MLA)
      {
        IROperand acc = tcc_ir_op_get_accum(ir, q);
        MARK_STACKLOC_OP(acc);
      }
#undef MARK_STACKLOC_OP
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
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local || irop_get_vreg(dest) >= 0)
        continue; /* Not an anonymous StackLoc */
      if (dest.is_llocal)
        continue; /* Store to parent frame via static chain — externally visible */

      const Sym *sym;
      int64_t off;
      dse_stackloc_sym_off(ir, dest, &sym, &off);

      LOG_IR_GEN("DSE-SL: i=%d off=%lld sym=%p test=%d", i, (long long)off, (void *)sym, !!STACKLOC_TEST(sym, off));
      if (!STACKLOC_TEST(sym, off))
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
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t vr = irop_get_vreg(d);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos > max_var_pos)
              max_var_pos = pos;
          }
        }
      }

      uint8_t *dead_tmp = tcc_mallocz((max_tmp_stackloc + 8) / 8);
      uint8_t *dead_var = max_var_pos >= 0 ? tcc_mallocz((max_var_pos + 8) / 8) : NULL;

      /* NOP LEA/Addr for write-only addr-TMPs whose StackLoc range is dead. */
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
        if (dpos > max_tmp_stackloc)
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
        if (range_len > STACKLOC_HASH_SIZE * 8)
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

        q->op = TCCIR_OP_NOP;
        changes++;
        dead_tmp[dpos / 8] |= (1 << (dpos % 8));
      }

      /* Forward-propagate deadness: NOP instructions whose sources are all dead, then mark their dests dead. */
      int prop_changed = 1;
      while (prop_changed)
      {
        prop_changed = 0;
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP)
            continue;
          /* Only propagate through data-flow instructions */
          if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
              q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
              q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID || q->op == TCCIR_OP_SWITCH_TABLE)
            continue;

          int has_dead_src = 0;

          if (irop_config[q->op].has_src1)
          {
            IROperand s = tcc_ir_op_get_src1(ir, q);
            int32_t vr = irop_get_vreg(s);
            if (vr >= 0)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos <= max_tmp_stackloc &&
                  (dead_tmp[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
              else if (dead_var && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos <= max_var_pos &&
                       (dead_var[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
            }
          }

          /* STORE/STORE_INDEXED dest is the pointer base — a dead base kills the store. */
          if (!has_dead_src && (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED))
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t vr = irop_get_vreg(d);
            if (vr >= 0)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos <= max_tmp_stackloc &&
                  (dead_tmp[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
            }
          }

          if (has_dead_src)
          {
            if (irop_config[q->op].has_dest)
            {
              IROperand d = tcc_ir_op_get_dest(ir, q);
              int32_t dvr = irop_get_vreg(d);
              if (dvr >= 0)
              {
                int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
                if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && dpos <= max_tmp_stackloc)
                  dead_tmp[dpos / 8] |= (1 << (dpos % 8));
                else if (dead_var && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && dpos <= max_var_pos)
                  dead_var[dpos / 8] |= (1 << (dpos % 8));
              }
            }
            q->op = TCCIR_OP_NOP;
            changes++;
            prop_changed = 1;
          }
        }
      }

      tcc_free(dead_tmp);
      if (dead_var)
        tcc_free(dead_var);
    }

    if (addr_tmp)
      tcc_free(addr_tmp);
    if (addr_tmp_read)
      tcc_free(addr_tmp_read);

#undef STACKLOC_HASH_SIZE
#undef STACKLOC_HASH
#undef STACKLOC_SET
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
          const IROperand s = tcc_ir_op_get_src1(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        if (irop_config[q->op].has_src2)
        {
          const IROperand s = tcc_ir_op_get_src2(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        /* STORE/STORE_INDEXED dest is a use (pointer deref) */
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
              TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(d));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
          if (q->op == TCCIR_OP_FUNCPARAMVAL && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(d));
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
        if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
        {
          int is_dead_eligible =
              (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC &&
               q->op != TCCIR_OP_LOAD_POSTINC && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_FUNCCALLVAL &&
               q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID);
          if (!is_dead_eligible && q->op == TCCIR_OP_LOAD)
          {
            const IROperand src1 = tcc_ir_op_get_src1(ir, q);
            if (irop_is_immediate(src1))
              is_dead_eligible = 1;
          }
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

int tcc_ir_opt_dse_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dse(ctx->ir);
}
