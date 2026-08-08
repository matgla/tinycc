/*
 *  TCC IR - Constant folding of read-modify-write chains on non-escaping
 *           local aggregates (e.g. the unrolled `u.e.a++` double sequence in
 *           gcc.c-torture pr92904).
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Const-folds dadd/dsub read-modify-write chains (e.g. unrolled `u.e.a++`) on
 * local aggregates whose address never escapes: one forward walk keeps a
 * per-slot constant lattice with a control-flow JOIN, propagating each slot's
 * compile-time constant across calls and folding the chain. Only dadd/dsub
 * CALLs are rewritten (each a guaranteed win); STOREs are kept so aggregate
 * memory stays correct. Escape granularity is the LEA root base, derived from
 * the IR (stack layout isn't built until after RA). Bails on IJUMP, SWITCH,
 * setjmp, asm, VLA, nested frames. Kill-switch: TCC_NO_CONST_AGG=1. Motivating
 * case: gcc.c-torture pr92904. */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_alias.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "memory/small_sequence.h"
#include "memory/vector.h"

#define CAF_ROOT_NONE INT32_MIN     /* address value whose root we don't know */
#define CAF_ROOT_CONFLICT (INT32_MIN + 1)

/* A single-def TEMP that holds the address of StackLoc[off] (via LEA, or via
 * ADD/SUB of a constant, or an ASSIGN copy of another such temp). */
typedef struct
{
  int has_off;   /* 1 = single-def address of StackLoc[off] */
  int32_t off;   /* resolved byte offset */
  int32_t root;  /* root LEA base offset (object identity) */
  int def_count; /* counts ALL value-defs (cap 2 — single-def required) */
} CAggTmpAddr;

/* Per-tracked-slot constant lattice cell. */
typedef struct
{
  int64_t val; /* raw bit pattern of the constant currently in the slot */
  uint8_t known;
} CAggSlot;

/* Inline-first scratch buffers (heap only past the inline cap). */
TCC_SMALL_SEQUENCE_DEFINE(CAggTmpArr, CAggTmpAddr, 32)
TCC_SMALL_SEQUENCE_DEFINE(CAggSlotArr, CAggSlot, 32)
TCC_SMALL_SEQUENCE_DEFINE(CAggI64Arr, int64_t, 32)
TCC_SMALL_SEQUENCE_DEFINE(CAggByteArr, uint8_t, 256)

/* A FUNCCALL to a noreturn callee (abort/exit/...) does not fall through. */
static int caf_is_noreturn_call(TCCIRState *ir, int i)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
    return 0;
  return tcc_ir_callee_is_noreturn(irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q)));
}

static int caf_is_terminator(TCCIRState *ir, int i)
{
  int op = ir->compact_instructions[i].op;
  if (op == TCCIR_OP_JUMP || op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID ||
      op == TCCIR_OP_TRAP)
    return 1;
  return caf_is_noreturn_call(ir, i);
}

/* Is the call at call_idx a memmove/memcpy (whose SOURCE arg is read-only)? */
static int caf_is_mem_call(TCCIRState *ir, int call_idx)
{
  IRQuadCompact *q = &ir->compact_instructions[call_idx];
  if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
    return 0;
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!callee)
    return 0;
  const char *name = get_tok_str(callee->v, NULL);
  if (!name)
    return 0;
  return ir_opt_is_memcpy_or_memmove_name(name);
}

/* classify a dadd/dsub callee: returns 1=add, 2=sub, 0=neither */
static int caf_dop(TCCIRState *ir, IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_FUNCCALLVAL)
    return 0;
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!callee)
    return 0;
  const char *name = get_tok_str(callee->v, NULL);
  if (!name)
    return 0;
  if (strcmp(name, "__aeabi_dadd") == 0)
    return 1;
  if (strcmp(name, "__aeabi_dsub") == 0)
    return 2;
  return 0;
}

TCC_DBG_ENV_FLAG(cagg_disabled, "TCC_NO_CONST_AGG")

int tcc_ir_opt_const_aggregate_fold(TCCIRState *ir)
{
  if (cagg_disabled())
    return 0;

  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (ir->captured_count > 0 || ir->has_static_chain)
    return 0; /* nested fn: parent-frame slot reachable via static chain */

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_VLA_ALLOC ||
        op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT ||
        op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
      return 0;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF)
    {
      int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (t < 0 || t >= n)
        return 0;
    }
  }

  /* ---- Pass A: single-def TEMP -> (offset, root base) ------------------- */
  int max_tmp = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_tmp)
      max_tmp = pos;
  }
  if (max_tmp == 0)
    return 0;

  small_sequence(CAggTmpArr) ta_owner = {0};
  if (CAggTmpArr_init(&ta_owner, (size_t)(max_tmp + 1)) != 0)
    return 0;
  CAggTmpAddr *ta = CAggTmpArr_data(&ta_owner);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.is_lval)
      continue;
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    ta[pos].def_count++;
    if (ta[pos].def_count > 1)
    {
      ta[pos].has_off = 0;
      continue;
    }
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA) &&
        irop_get_tag(s1) == IROP_TAG_STACKOFF && s1.is_local && !s1.is_lval &&
        irop_get_vreg(s1) == -1)
    {
      ta[pos].has_off = 1;
      ta[pos].off = irop_get_stack_offset(s1);
      ta[pos].root = ta[pos].off; /* root LEA: base == offset */
      continue;
    }
    int32_t s1vr = irop_get_vreg(s1);
    if (s1vr >= 0 && !s1.is_lval && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int sp = TCCIR_DECODE_VREG_POSITION(s1vr);
      if (sp <= max_tmp && ta[sp].has_off)
      {
        if (q->op == TCCIR_OP_ASSIGN)
        {
          ta[pos] = ta[sp];
          ta[pos].def_count = 1;
          continue;
        }
        if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          if (irop_get_tag(s2) == IROP_TAG_IMM32 && !s2.is_lval)
          {
            int32_t k = (int32_t)irop_get_imm64_ex(ir, s2);
            ta[pos].has_off = 1;
            ta[pos].off = (q->op == TCCIR_OP_ADD) ? ta[sp].off + k : ta[sp].off - k;
            ta[pos].root = ta[sp].root;
            continue;
          }
        }
      }
    }
  }

/* Resolve a deref (lval) operand to (offset, root).  root == CAF_ROOT_NONE for
 * a direct StackLoc operand (no LEA chain observed for this access). */
#define CAF_RESOLVE_LVAL(_op, _outoff, _outroot)                             \
  ({                                                                         \
    int _ok = 0;                                                             \
    (_outroot) = CAF_ROOT_NONE;                                              \
    if ((_op).is_lval)                                                       \
    {                                                                        \
      if (irop_get_tag(_op) == IROP_TAG_STACKOFF && (_op).is_local &&        \
          irop_get_vreg(_op) == -1)                                          \
      {                                                                      \
        (_outoff) = irop_get_stack_offset(_op);                             \
        _ok = 1;                                                             \
      }                                                                      \
      else                                                                   \
      {                                                                      \
        int32_t _vr = irop_get_vreg(_op);                                    \
        if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_TEMP) \
        {                                                                    \
          int _p = TCCIR_DECODE_VREG_POSITION(_vr);                          \
          if (_p <= max_tmp && ta[_p].has_off)                               \
          {                                                                  \
            (_outoff) = ta[_p].off;                                          \
            (_outroot) = ta[_p].root;                                        \
            _ok = 1;                                                         \
          }                                                                  \
        }                                                                    \
      }                                                                      \
    }                                                                        \
    _ok;                                                                     \
  })

/* Resolve an address VALUE operand (is_lval==0) to its root base. */
#define CAF_RESOLVE_ADDR_ROOT(_op, _outroot)                                 \
  ({                                                                         \
    int _ok = 0;                                                             \
    if (!(_op).is_lval)                                                      \
    {                                                                        \
      if (irop_get_tag(_op) == IROP_TAG_STACKOFF && (_op).is_local &&        \
          irop_get_vreg(_op) == -1)                                          \
      {                                                                      \
        (_outroot) = irop_get_stack_offset(_op);                            \
        _ok = 1;                                                             \
      }                                                                      \
      else                                                                   \
      {                                                                      \
        int32_t _vr = irop_get_vreg(_op);                                    \
        if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_TEMP) \
        {                                                                    \
          int _p = TCCIR_DECODE_VREG_POSITION(_vr);                          \
          if (_p <= max_tmp && ta[_p].has_off)                               \
          {                                                                  \
            (_outroot) = ta[_p].root;                                        \
            _ok = 1;                                                         \
          }                                                                  \
        }                                                                    \
      }                                                                      \
    }                                                                        \
    _ok;                                                                     \
  })

  /* ---- Pass B: escape analysis -> set of escaped root bases -------------- */
  scoped_vector(int32_t) escaped = {0};      /* no alloc until first escape */
#define CAF_ESCAPE(_root)                                                    \
  do                                                                         \
  {                                                                          \
    int _dup = 0;                                                            \
    int32_t *_ed = vector_data(&escaped);                                    \
    for (size_t _e = 0; _e < escaped.size; _e++)                             \
      if (_ed[_e] == (_root)) { _dup = 1; break; }                           \
    if (!_dup)                                                               \
      vector_push_back(&escaped, (int32_t)(_root));                          \
  } while (0)

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    int is_param = (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID);
    int param_is_memsrc = 0;
    if (is_param)
    {
      uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (TCCIR_DECODE_PARAM_IDX(enc) == 1)
      {
        int cid = TCCIR_DECODE_CALL_ID(enc);
        for (int j = i + 1; j < n; j++)
        {
          IRQuadCompact *cj = &ir->compact_instructions[j];
          if (cj->op != TCCIR_OP_FUNCCALLVOID && cj->op != TCCIR_OP_FUNCCALLVAL)
            continue;
          uint32_t cenc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, cj));
          if (TCCIR_DECODE_CALL_ID(cenc) == cid)
          {
            param_is_memsrc = caf_is_mem_call(ir, j);
            break;
          }
        }
      }
    }

    /* A recognized address-propagation def (its dest is a tracked addr temp)
     * is a safe sink for an address value. */
    int def_is_addr_prop = 0;
    if (irop_config[q->op].has_dest &&
        (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA ||
         q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB))
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!d.is_lval)
      {
        int32_t dvr = irop_get_vreg(d);
        if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
        {
          int dp = TCCIR_DECODE_VREG_POSITION(dvr);
          if (dp <= max_tmp && ta[dp].has_off)
            def_is_addr_prop = 1;
        }
      }
    }

    for (int k = 1; k < 3; k++)
    {
      int has = (k == 1) ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2;
      if (!has)
        continue;
      IROperand op = (k == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      int32_t aroot;
      if (!CAF_RESOLVE_ADDR_ROOT(op, aroot))
        continue;
      if (def_is_addr_prop)
        continue;                       /* forwarded into a tracked addr temp */
      if (is_param && param_is_memsrc)
        continue;                       /* read-only memmove/memcpy source */
      CAF_ESCAPE(aroot);                /* any other use escapes the object */
    }
  }

#define CAF_ROOT_ESCAPED(_root)                                              \
  ({                                                                         \
    int _r = 0;                                                              \
    int32_t *_ed = vector_data(&escaped);                                    \
    for (size_t _e = 0; _e < escaped.size; _e++)                             \
      if (_ed[_e] == (_root)) { _r = 1; break; }                             \
    _r;                                                                      \
  })

  /* ---- Pass C: collect 8-byte candidate slots with a known, non-escaped
   * root base.  Track per-offset root; conflicting roots disqualify. -------- */
#define CAF_MAX_SLOTS 128
  int32_t cand_off[CAF_MAX_SLOTS];
  int32_t cand_root[CAF_MAX_SLOTS];
  int ncand = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_ASSIGN)
      continue;
    int32_t off, root;
    int sz8;
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!CAF_RESOLVE_LVAL(d, off, root))
        continue;
      sz8 = irop_is_64bit(d);
    }
    else
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (!s1.is_lval || !CAF_RESOLVE_LVAL(s1, off, root))
        continue;
      sz8 = irop_is_64bit(s1);
    }
    if (!sz8)
      continue;
    int slot = -1;
    for (int c = 0; c < ncand; c++)
      if (cand_off[c] == off) { slot = c; break; }
    if (slot < 0)
    {
      if (ncand >= CAF_MAX_SLOTS)
        continue;
      slot = ncand++;
      cand_off[slot] = off;
      cand_root[slot] = root;
    }
    else if (root != CAF_ROOT_NONE)
    {
      if (cand_root[slot] == CAF_ROOT_NONE)
        cand_root[slot] = root;
      else if (cand_root[slot] != root)
        cand_root[slot] = CAF_ROOT_CONFLICT;
    }
  }
  /* Keep only candidates with a known, non-escaped root base. */
  {
    int w = 0;
    for (int c = 0; c < ncand; c++)
    {
      int32_t r = cand_root[c];
      if (r == CAF_ROOT_NONE || r == CAF_ROOT_CONFLICT || CAF_ROOT_ESCAPED(r))
        continue;
      cand_off[w] = cand_off[c];
      cand_root[w] = r;
      w++;
    }
    ncand = w;
  }
  if (ncand == 0)
    return 0;

#define CAF_SLOT_IDX(_off)                                                   \
  ({                                                                         \
    int _idx = -1;                                                           \
    for (int _c = 0; _c < ncand; _c++)                                       \
      if (cand_off[_c] == (_off)) { _idx = _c; break; }                      \
    _idx;                                                                    \
  })

  /* ---- Pass D: forward dataflow + dadd/dsub fold ------------------------- */
  small_sequence(CAggByteArr) is_jt_owner = {0}, has_back_pred_owner = {0}, need_save_owner = {0};
  if (CAggByteArr_init(&is_jt_owner, (size_t)n) != 0 ||
      CAggByteArr_init(&has_back_pred_owner, (size_t)n) != 0 ||
      CAggByteArr_init(&need_save_owner, (size_t)n) != 0)
    return 0;
  uint8_t *is_jt = CAggByteArr_data(&is_jt_owner);
  uint8_t *has_back_pred = CAggByteArr_data(&has_back_pred_owner);
  uint8_t *need_save = CAggByteArr_data(&need_save_owner);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    is_jt[t] = 1;
    if (t <= i)
      has_back_pred[t] = 1;
    need_save[i] = 1;
    if (t - 1 >= 0 && !caf_is_terminator(ir, t - 1))
      need_save[t - 1] = 1;
  }

  small_sequence(CAggSlotArr) cur_owner = {0};
  small_sequence(CAggI64Arr) tcv_owner = {0};
  small_sequence(CAggByteArr) tck_owner = {0};
  if (CAggSlotArr_init(&cur_owner, (size_t)ncand) != 0 ||
      CAggI64Arr_init(&tcv_owner, (size_t)(max_tmp + 1)) != 0 ||
      CAggByteArr_init(&tck_owner, (size_t)(max_tmp + 1)) != 0)
    return 0;
  CAggSlot *cur = CAggSlotArr_data(&cur_owner);
  int64_t *tcv = CAggI64Arr_data(&tcv_owner);
  uint8_t *tck = CAggByteArr_data(&tck_owner);
  CAggSlot **saved = tcc_mallocz(sizeof(CAggSlot *) * n);  /* sparse per-instr snapshots, freed below */
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (is_jt[i])
    {
      if (has_back_pred[i])
        memset(cur, 0, sizeof(CAggSlot) * ncand);
      else
      {
        small_sequence(CAggSlotArr) acc_owner = {0};
        CAggSlotArr_init(&acc_owner, (size_t)ncand);
        CAggSlot *acc = CAggSlotArr_data(&acc_owner);
        int first = 1;
        if (i - 1 >= 0 && !caf_is_terminator(ir, i - 1))
        {
          if (saved[i - 1])
            memcpy(acc, saved[i - 1], sizeof(CAggSlot) * ncand);
          first = 0;
        }
        for (int j = 0; j < i; j++)
        {
          IRQuadCompact *jq = &ir->compact_instructions[j];
          if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
            continue;
          if ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jq)) != i)
            continue;
          CAggSlot *ps = saved[j];
          if (!ps)
          {
            memset(acc, 0, sizeof(CAggSlot) * ncand);
            first = 0;
            continue;
          }
          if (first)
          {
            memcpy(acc, ps, sizeof(CAggSlot) * ncand);
            first = 0;
          }
          else
            for (int c = 0; c < ncand; c++)
              if (!(acc[c].known && ps[c].known && acc[c].val == ps[c].val))
                acc[c].known = 0;
        }
        if (first)
          memset(acc, 0, sizeof(CAggSlot) * ncand);
        memcpy(cur, acc, sizeof(CAggSlot) * ncand);
      }
      memset(tck, 0, max_tmp + 1);
    }

    if (q->op == TCCIR_OP_NOP)
      goto save;

    /* dadd/dsub fold */
    {
      int dk = caf_dop(ir, q);
      if (dk)
      {
        IROperand p0, p1;
        int64_t a0 = 0, a1 = 0;
        int a0_ok = 0, a1_ok = 0;
        if (ir_opt_get_call_param_operand(ir, i, 0, &p0) &&
            ir_opt_get_call_param_operand(ir, i, 1, &p1))
        {
          if (irop_is_immediate(p0)) { a0 = irop_get_imm64_ex(ir, p0); a0_ok = 1; }
          else
          {
            int32_t vr = irop_get_vreg(p0);
            if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int p = TCCIR_DECODE_VREG_POSITION(vr);
              if (p <= max_tmp && tck[p]) { a0 = tcv[p]; a0_ok = 1; }
            }
          }
          if (irop_is_immediate(p1)) { a1 = irop_get_imm64_ex(ir, p1); a1_ok = 1; }
          else
          {
            int32_t vr = irop_get_vreg(p1);
            if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int p = TCCIR_DECODE_VREG_POSITION(vr);
              if (p <= max_tmp && tck[p]) { a1 = tcv[p]; a1_ok = 1; }
            }
          }
        }
        if (a0_ok && a1_ok)
        {
          union { double d; uint64_t u; } da, db, dr;
          da.u = (uint64_t)a0;
          db.u = (uint64_t)a1;
          dr.d = (dk == 1) ? da.d + db.d : da.d - db.d;
          int64_t result = (int64_t)dr.u;
          IROperand call_dest = tcc_ir_op_get_dest(ir, q);
          IROperand imm_src = irop_make_f64(-1, tcc_ir_pool_add_f64(ir, (uint64_t)result));
          ir_opt_nop_call_params(ir, i);
          q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_dest(ir, i, call_dest);
          tcc_ir_set_src1(ir, i, imm_src);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          int32_t dv = irop_get_vreg(call_dest);
          if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
          {
            int dp = TCCIR_DECODE_VREG_POSITION(dv);
            if (dp <= max_tmp) { tcv[dp] = result; tck[dp] = 1; }
          }
          changes++;
        }
        goto save;
      }
    }

    /* LOAD of a tracked slot -> record temp constant (no rewrite). */
    if ((q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) && irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t off, root;
      if (s1.is_lval && !d.is_lval && irop_is_64bit(s1) && CAF_RESOLVE_LVAL(s1, off, root))
      {
        (void)root;
        int idx = CAF_SLOT_IDX(off);
        int32_t dv = irop_get_vreg(d);
        if (idx >= 0 && cur[idx].known && dv >= 0 &&
            TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
        {
          int dp = TCCIR_DECODE_VREG_POSITION(dv);
          if (dp <= max_tmp) { tcv[dp] = cur[idx].val; tck[dp] = 1; }
        }
        goto save;
      }
    }

    /* STORE: update the slot lattice. */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t off, root;
      if (CAF_RESOLVE_LVAL(d, off, root))
      {
        (void)root;
        int ssize = irop_is_64bit(d) ? 8 : 4;
        IROperand v = tcc_ir_op_get_src1(ir, q);
        int64_t cval = 0;
        int cval_ok = 0;
        if (ssize == 8)
        {
          if (irop_is_immediate(v)) { cval = irop_get_imm64_ex(ir, v); cval_ok = 1; }
          else
          {
            int32_t vv = irop_get_vreg(v);
            if (vv >= 0 && TCCIR_DECODE_VREG_TYPE(vv) == TCCIR_VREG_TYPE_TEMP)
            {
              int vp = TCCIR_DECODE_VREG_POSITION(vv);
              if (vp <= max_tmp && tck[vp]) { cval = tcv[vp]; cval_ok = 1; }
            }
          }
        }
        for (int c = 0; c < ncand; c++)
        {
          int32_t toff = cand_off[c];
          if (off < toff + 8 && toff < off + ssize)
          {
            if (toff == off && ssize == 8 && cval_ok)
            {
              cur[c].known = 1;
              cur[c].val = cval;
            }
            else
              cur[c].known = 0;
          }
        }
      }
      goto save;
    }

    /* Any other def of a TEMP invalidates its recorded constant. */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!d.is_lval)
      {
        int32_t dv = irop_get_vreg(d);
        if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
        {
          int dp = TCCIR_DECODE_VREG_POSITION(dv);
          if (dp <= max_tmp)
            tck[dp] = 0;
        }
      }
    }

  save:
    if (need_save[i])
    {
      if (!saved[i])
        saved[i] = tcc_malloc(sizeof(CAggSlot) * ncand);
      memcpy(saved[i], cur, sizeof(CAggSlot) * ncand);
    }
  }

  for (int i = 0; i < n; i++)
    if (saved[i])
      tcc_free(saved[i]);
  tcc_free(saved);
  return changes;
}

int tcc_ir_opt_const_aggregate_fold_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_const_aggregate_fold(ctx->ir);
}
