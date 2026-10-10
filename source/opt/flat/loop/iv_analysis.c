/*
 *  TCC IR - Induction variable analysis
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "licm.h"
#include "opt.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

/* Find basic induction variables: V = V + const (V is a VAR vreg) */
int find_induction_vars_ex(TCCIRState *ir, IRLoop *loop, InductionVar *ivs, int max_ivs, int allow_copy_through)
{
  int num_ivs = 0;

  /* Scan the ORIGINAL loop range (not extended body) for IV increments */
  for (int i = loop->start_idx; i <= loop->end_idx && num_ivs < max_ivs; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_ADD || tcc_ir_barrel_shift_at(ir, q))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int dest_vr = irop_get_vreg(dest);
    int src1_vr = irop_get_vreg(src1);

    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    /* IV consumers (closed forms, unroll trip math, strength reduction) do
     * their step/init/final arithmetic in 32-bit ints and build INT32
     * operands for the rewritten IR; a 64-bit (or sub-word) IV would get its
     * high word silently dropped.  Only classic INT32 IVs are sound. */
    if (irop_get_btype(dest) != IROP_BTYPE_INT32)
      continue;

    /* A volatile VAR is never an induction variable: each read returns
     * whatever the object holds, and each read and write must happen, so its
     * trip count cannot be computed, nor the loop unrolled or eliminated --
     * `for (volatile int i = 0; i < 1000; i++);` is a delay loop because of
     * exactly that. */
    if (tcc_ir_operand_names_volatile_var(ir, dest))
      continue;

    /* Pattern: V = V + const  OR  V = T + const where T := V (copy-through) */
    int effective_src_vr = src1_vr;
    if (allow_copy_through && src1_vr != dest_vr && src1_vr >= 0 && irop_is_immediate(src2))
    {
      /* Check if src1 is a temp assigned from dest_vr just before */
      for (int k = i - 1; k >= loop->start_idx && k >= i - 3; k--)
      {
        IRQuadCompact *aq = &ir->compact_instructions[k];
        if (aq->op == TCCIR_OP_ASSIGN)
        {
          int32_t adest_vr = tcc_ir_op_dest_vreg(ir, aq);
          int32_t asrc_vr = tcc_ir_op_src1_vreg(ir, aq);
          if (adest_vr == src1_vr && asrc_vr == dest_vr)
          {
            effective_src_vr = dest_vr;
            break;
          }
        }
        if (aq->op != TCCIR_OP_NOP)
          break; /* stop at first non-NOP non-matching instr */
      }
    }
    if (effective_src_vr == dest_vr && irop_is_immediate(src2))
    {
      int step = (int)irop_get_imm64_ex(ir, src2);

      /* VAR must be defined exactly once in loop (the increment) */
      int def_count = 0;
      for (int j = loop->start_idx; j <= loop->end_idx; j++)
      {
        IRQuadCompact *dq = &ir->compact_instructions[j];
        int32_t ddest_vr = tcc_ir_op_dest_vreg(ir, dq);
        if (ddest_vr == dest_vr && dq->op != TCCIR_OP_NOP)
          def_count++;
      }

      if (def_count != 1)
        continue; /* IV has multiple definitions in loop - not simple */

      /* Look for initialization in preheader */
      int init_val = 0;
      int init_idx = -1;
      /* The ASSIGN must reach the loop entry: stop at any other definition of
       * the IV (a call result, a copy, an ADD) and at any jump target after
       * it (another path enters the loop without running the ASSIGN). */
      for (int j = loop->preheader_idx; j >= 0 && j >= loop->preheader_idx - 5; j--)
      {
        IRQuadCompact *pq = &ir->compact_instructions[j];
        if (pq->op != TCCIR_OP_NOP && irop_config[pq->op].has_dest &&
            irop_get_vreg(tcc_ir_op_get_dest(ir, pq)) == dest_vr)
        {
          IROperand psrc1 = tcc_ir_op_get_src1(ir, pq);
          if (pq->op == TCCIR_OP_ASSIGN && irop_is_immediate(psrc1))
          {
            init_val = (int)irop_get_imm64_ex(ir, psrc1);
            init_idx = j;
          }
          break;
        }
        if (pq->is_jump_target)
          break; /* joins here: an ASSIGN above is not on every path */
      }

      if (init_idx < 0)
        continue; /* No initialization found */

      /* A branch from outside the loop into (init, loop] -- `if (c) i = 2;`
       * jumping straight to the header -- enters without running the ASSIGN. */
      int init_bypassed = 0;
      for (int k = 0; k < ir->next_instruction_index && !init_bypassed; k++)
      {
        if (k >= loop->start_idx && k <= loop->end_idx)
          continue;
        IRQuadCompact *jq = &ir->compact_instructions[k];
        if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
          continue;
        int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jq));
        if (target > init_idx && target <= loop->end_idx)
          init_bypassed = 1;
      }
      if (init_bypassed)
        continue;

      ivs[num_ivs].vreg = dest_vr;
      ivs[num_ivs].init_val = init_val;
      ivs[num_ivs].step = step;
      ivs[num_ivs].def_idx = i;
      ivs[num_ivs].init_idx = init_idx;
      num_ivs++;

      LOG_IV_SR("IV_SR: Found BIV VAR%d (init=%d, step=%d) at idx=%d", TCCIR_DECODE_VREG_POSITION(dest_vr), init_val,
                step, i);
      LOG_LOOP_OPT("IV found: VAR%d init=%d step=%d def_idx=%d init_idx=%d", TCCIR_DECODE_VREG_POSITION(dest_vr),
                   init_val, step, i, init_idx);
    }
  }

  LOG_LOOP_OPT("find_induction_vars_ex: found %d IV(s) in loop [%d..%d]", num_ivs, loop->start_idx, loop->end_idx);
  return num_ivs;
}


/* Find derived induction variables: base + (IV << shift), used for indexing */
/* Queried per candidate inside the derived-IV scan — latch it. */
TCC_DBG_ENV_FLAG(dbg_mlaiv, "TCC_DBG_MLAIV")

/* Is the IV counter itself removable once its address uses become a pointer
 * walk?  The IV may only be read by: its own self-increment, a copy-through
 * `T = V` just before that increment, indexed uses where it is the index, up
 * to two `CMP V,#imm` (header pre-test + latch test), and the whitelisted
 * instruction(s) computing THIS derived IV's offset (the SHL/MUL, or a fused
 * MLA).  Anything else means try_eliminate_iv_counter will fail and the loop
 * would carry BOTH the index and the new pointer -- pure added work, measured
 * at +14.6% on mibench_dijkstra before this gate existed. */
static IROperand iv_canon_base(TCCIRState *ir, IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_lval || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return op;
  if (!tcc_ir_vreg_has_single_def(ir, vr))
    return op;
  int def = tcc_ir_find_defining_instruction(ir, vr, ir->next_instruction_index);
  if (def < 0)
    return op;
  IRQuadCompact *dq = &ir->compact_instructions[def];
  if (dq->op != TCCIR_OP_ASSIGN)
    return op;
  IROperand src = tcc_ir_op_get_src1(ir, dq);
  if (irop_get_vreg(src) >= 0)
    return op; /* only look through a copy of a constant address */
  return src;
}

int iv_same_base(TCCIRState *ir, IROperand a, IROperand b)
{
  a = iv_canon_base(ir, a);
  b = iv_canon_base(ir, b);
  int tag_a = irop_get_tag(a);
  int tag_b = irop_get_tag(b);
  if (tag_a != tag_b || a.is_lval != b.is_lval)
    return 0;
  if (tag_a == IROP_TAG_IMM32 || tag_a == IROP_TAG_STACKOFF)
    return a.u.imm32 == b.u.imm32;
  if (tag_a == IROP_TAG_VREG)
  {
    int32_t va = irop_get_vreg(a);
    int32_t vb = irop_get_vreg(b);
    return va >= 0 && va == vb;
  }
  return 0;
}

/* The loop's full index range: start..end plus a detached body (the un-rotated
 * shape keeps the body past the back-edge). */
static void iv_loop_range(const IRLoop *loop, int *lo, int *hi)
{
  *lo = loop->start_idx;
  *hi = loop->end_idx;
  if (loop->num_body_instrs > 0)
  {
    if (loop->body_instrs[0] < *lo)
      *lo = loop->body_instrs[0];
    if (loop->body_instrs[loop->num_body_instrs - 1] > *hi)
      *hi = loop->body_instrs[loop->num_body_instrs - 1];
  }
}

int iv_read_reachable_outside(TCCIRState *ir, IRLoop *loop, int32_t iv_vr)
{
  const int n = ir->next_instruction_index;
  int lo, hi;
  iv_loop_range(loop, &lo, &hi);
  if (ir_opt_vreg_address_taken_between(ir, iv_vr, 0, n - 1))
    return 1;

  uint8_t *seen = tcc_mallocz((size_t)n + 1);
  int *wl = tcc_malloc(sizeof(int) * ((size_t)n + 2));
  int nwl = 0;
  int result = 0;

  /* Every way out of the loop: a branch from inside to outside, and the fall
   * through past its last instruction when that is not an unconditional jump. */
  for (int j = lo; j <= hi && j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
    {
      result = 1;
      break;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t < lo || t > hi)
        wl[nwl++] = t;
    }
  }
  if (!result && hi + 1 < n && ir->compact_instructions[hi].op != TCCIR_OP_JUMP)
    wl[nwl++] = hi + 1;

  while (!result && nwl > 0)
  {
    int k = wl[--nwl];
    if (k < 0 || k >= n || seen[k])
      continue;
    seen[k] = 1;
    if (k >= lo && k <= hi)
    {
      result = 1; /* a path back into the loop that is not its own back-edge */
      break;
    }
    IRQuadCompact *q = &ir->compact_instructions[k];
    if (q->op == TCCIR_OP_NOP)
    {
      wl[nwl++] = k + 1;
      continue;
    }
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
    {
      result = 1;
      break;
    }
    /* reads */
    if ((irop_config[q->op].has_src1 && tcc_ir_op_src1_vreg(ir, q) == iv_vr) ||
        (irop_config[q->op].has_src2 && tcc_ir_op_src2_vreg(ir, q) == iv_vr) ||
        (q->op == TCCIR_OP_MLA && tcc_ir_op_accum_vreg(ir, q) == iv_vr))
    {
      result = 1;
      break;
    }
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (irop_get_vreg(d) == iv_vr)
      {
        int direct = !d.is_lval || (TCCIR_DECODE_VREG_TYPE(iv_vr) == TCCIR_VREG_TYPE_VAR && d.is_local);
        if (!direct)
        {
          result = 1; /* the IV used as an address */
          break;
        }
        continue; /* redefined: nothing past here sees the loop's value */
      }
    }
    if (q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_RETURNVALUE)
      continue;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      wl[nwl++] = (int)tcc_ir_op_dest_imm(ir, q);
      if (q->op == TCCIR_OP_JUMPIF)
        wl[nwl++] = k + 1;
      continue;
    }
    wl[nwl++] = k + 1;
  }
  tcc_free(seen);
  tcc_free(wl);
  return result;
}

/* `allow` lists instructions that read the IV only to compute an address this
 * transform is about to take over -- a DIV's SHL/MUL, or the `iv - k` of every
 * affine INDEXED-DIV in the loop.  A loop like SHA-1's message expansion reads
 * its counter in four such subtractions per iteration, so a fixed two-slot
 * whitelist could never admit it.
 *
 * With `loop` given, reads are judged INSIDE the loop by the rules below and
 * OUTSIDE it by reachability: SHA-1's six loops share one `i`, each starting
 * from a literal, and a whole-function scan called every one of them
 * non-eliminable because the next loop's `i = i + 1` was a read. */
static int iv_ctr_eliminable_list(TCCIRState *ir, IRLoop *loop, int32_t iv_vr, int iv_def_idx,
                                  const int *allow, int nallow)
{
  int safe = 1;
  int cmp_count = 0;
  int lo = 0, hi = ir->next_instruction_index - 1;
  if (loop)
  {
    iv_loop_range(loop, &lo, &hi);
    if (iv_read_reachable_outside(ir, loop, iv_vr))
      return 0;
  }
  for (int j = lo; j <= hi && safe; j++)
  {
    IRQuadCompact *uq = &ir->compact_instructions[j];
    if (uq->op == TCCIR_OP_NOP)
      continue;
    if (j == iv_def_idx)
      continue;
    {
      int allowed = 0;
      for (int a = 0; a < nallow && !allowed; a++)
        allowed = (allow[a] == j);
      if (allowed)
        continue;
    }

    if (uq->op == TCCIR_OP_LOAD_INDEXED || uq->op == TCCIR_OP_STORE_INDEXED)
    {
      if (tcc_ir_op_src2_vreg(ir, uq) == iv_vr)
        continue;
    }

    if (uq->op == TCCIR_OP_ASSIGN && j >= iv_def_idx - 3 && j < iv_def_idx)
    {
      if (tcc_ir_op_src1_vreg(ir, uq) == iv_vr)
        continue;
    }

    if (uq->op == TCCIR_OP_CMP)
    {
      int32_t cs1_vr = tcc_ir_op_src1_vreg(ir, uq);
      if (cs1_vr == iv_vr && tcc_ir_op_src2_is_imm(ir, uq) && cmp_count < 2)
      {
        cmp_count++;
        continue;
      }
    }

    if (irop_config[uq->op].has_src1 && tcc_ir_op_src1_vreg(ir, uq) == iv_vr)
      safe = 0;
    if (safe && irop_config[uq->op].has_src2 && tcc_ir_op_src2_vreg(ir, uq) == iv_vr)
      safe = 0;
  }
  return safe && cmp_count > 0;
}

static int iv_ctr_eliminable(TCCIRState *ir, int32_t iv_vr, int iv_def_idx, int allow_a, int allow_b)
{
  int allow[2] = {0, 0};
  int n = 0;
  if (allow_a >= 0)
    allow[n++] = allow_a;
  if (allow_b >= 0)
    allow[n++] = allow_b;
  return iv_ctr_eliminable_list(ir, NULL, iv_vr, iv_def_idx, allow, n);
}

/* Lookups find_derived_ivs makes per candidate, each a scan of the loop or
 * the function; built once per call, on first use.  find_derived_ivs only
 * reads the IR, so they stay valid for the whole call. */
typedef struct IVScan
{
  int built_deref;
  int deref_all_canon;    /* every dereferenced vreg is canonically encoded */
  int deref_max_pos;
  uint8_t *deref;         /* [type * (deref_max_pos + 1) + position] */
  int built_shl;
  int shl_max_pos;
  int *shl_pos;           /* per TEMP position: first body position of a SHL/MUL defining it */
  int *body_max;          /* per body position: the largest instruction index up to it */
} IVScan;

static int iv_vreg_canon(int32_t vr)
{
  return vr >= 0 && vr == TCCIR_ENCODE_VREG(TCCIR_DECODE_VREG_TYPE(vr), TCCIR_DECODE_VREG_POSITION(vr));
}

static void iv_scan_free(IVScan *sc)
{
  tcc_free(sc->deref);
  tcc_free(sc->shl_pos);
  tcc_free(sc->body_max);
}

/* The vreg a LOAD reads through or a STORE writes through at instruction j,
 * or -1. */
static int32_t iv_deref_vreg_at(TCCIRState *ir, IRQuadCompact *uq)
{
  if (uq->op == TCCIR_OP_LOAD || uq->op == TCCIR_OP_LOAD_INDEXED)
  {
    if (tcc_ir_op_src1_is_lval(ir, uq))
      return tcc_ir_op_src1_vreg(ir, uq);
  }
  else if (uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED)
  {
    if (tcc_ir_op_dest_is_lval(ir, uq))
      return tcc_ir_op_dest_vreg(ir, uq);
  }
  return -1;
}

/* Does this derived address value reach a real memory access (rather than
 * being consumed by an indexed op's addressing mode)?  Those are the DIVs the
 * escape scan in transform_derived_iv used to refuse outright. */
static int iv_div_addr_is_dereffed(TCCIRState *ir, IVScan *sc, int32_t dest_vr)
{
  if (dest_vr < 0)
    return 0;
  if (!sc->built_deref)
  {
    sc->built_deref = 1;
    sc->deref_all_canon = 1;
    sc->deref_max_pos = -1;
    for (int j = 0; j < ir->next_instruction_index; j++)
    {
      int32_t vr = iv_deref_vreg_at(ir, &ir->compact_instructions[j]);
      if (vr < 0)
        continue;
      if (!iv_vreg_canon(vr))
        sc->deref_all_canon = 0;
      else if (TCCIR_DECODE_VREG_POSITION(vr) > sc->deref_max_pos)
        sc->deref_max_pos = TCCIR_DECODE_VREG_POSITION(vr);
    }
    if (sc->deref_all_canon && sc->deref_max_pos >= 0)
    {
      int stride = sc->deref_max_pos + 1;
      sc->deref = tcc_mallocz((size_t)8 * stride);
      for (int j = 0; j < ir->next_instruction_index; j++)
      {
        int32_t vr = iv_deref_vreg_at(ir, &ir->compact_instructions[j]);
        if (vr >= 0)
          sc->deref[TCCIR_DECODE_VREG_TYPE(vr) * stride + TCCIR_DECODE_VREG_POSITION(vr)] = 1;
      }
    }
  }
  if (sc->deref_all_canon && iv_vreg_canon(dest_vr))
  {
    int p = TCCIR_DECODE_VREG_POSITION(dest_vr);
    return p <= sc->deref_max_pos && sc->deref[TCCIR_DECODE_VREG_TYPE(dest_vr) * (sc->deref_max_pos + 1) + p];
  }
  for (int j = 0; j < ir->next_instruction_index; j++)
    if (iv_deref_vreg_at(ir, &ir->compact_instructions[j]) == dest_vr)
      return 1;
  return 0;
}

/* Will the pointer walk this SCALED deref DIV becomes end in a post-indexed
 * access?  A scaled deref used to be rejected outright: the indexed-memory
 * fusion folds `base + (i << k)` into the addressing mode anyway, so the walk
 * merely traded `str.w r,[base,i,lsl #2]` for `str r,[p]` + `adds p,#k` —
 * measured a LOSS in situ (mibench_stringsearch +12.4%).  With
 * ra:load_postinc / ra:store_postinc the bump disappears into the access
 * (`str r,[p],#k`), which drops an instruction per iteration AND the scaled
 * index's +1 cycle, so the walk wins exactly when that fusion will fire.
 *
 * Admit the DIV only when the fused shape is assured:
 *   - stride within the post-index immediate range (1..255);
 *   - every use of the address value is the ADDRESS SLOT of a plain
 *     LOAD/STORE whose access is INT32 with a register value — the shape the
 *     ra matcher folds (INT16/INT8 loads widen, which the matcher refuses);
 *   - at least one such access is inside the loop body;
 *   - the counter is provably removable (iv_ctr_eliminable), else the loop
 *     carries BOTH the index and the pointer.
 * allow_a/allow_b whitelist the instruction(s) computing THIS DIV's offset
 * for the eliminability check (the SHL/MUL, or the fused MLA itself). */
static int iv_scaled_deref_postinc_viable(TCCIRState *ir, IRLoop *loop, int32_t dest_vr,
                                          int stride, InductionVar *iv, int allow_a, int allow_b)
{
  if (tcc_ir_opt_pass_disabled("iv_scaled_deref"))
    return 0;
  if (dest_vr < 0 || stride < 1 || stride > 255)
    return 0;

  int in_loop_access = 0;
  for (int j = 0; j < ir->next_instruction_index; j++)
  {
    IRQuadCompact *uq = &ir->compact_instructions[j];
    if (uq->op == TCCIR_OP_NOP)
      continue;

    int is_load = (uq->op == TCCIR_OP_LOAD);
    int is_store = (uq->op == TCCIR_OP_STORE);

    /* Address-slot use: src1 of a LOAD, dest of a STORE. */
    if (is_load || is_store)
    {
      IROperand addr = tcc_ir_op_get_dest_or_src1(ir, uq, is_load);
      IROperand val = tcc_ir_op_get_dest_or_src1(ir, uq, !is_load);
      if (addr.is_lval && irop_get_vreg(addr) == dest_vr)
      {
        if (val.is_lval || irop_get_vreg(val) < 0)
          return 0;
        if (irop_get_btype(addr) != IROP_BTYPE_INT32 || irop_get_btype(val) != IROP_BTYPE_INT32)
          return 0;
        if (is_load && val.is_unsigned != addr.is_unsigned)
          return 0;
        if (j >= loop->start_idx && j <= loop->end_idx)
          in_loop_access = 1;
        /* The other slot (the loaded/stored VALUE) may still read dest_vr —
         * fall through to the generic check below for it. */
        if (irop_get_vreg(val) != dest_vr)
          continue;
        return 0; /* address also stored/loaded as a value — not a pure walk */
      }
    }

    /* Any other read of the address value disqualifies: it escapes the
     * addressing role (stored, compared, passed, used to index...). */
    if (irop_config[uq->op].has_src1 && tcc_ir_op_src1_vreg(ir, uq) == dest_vr)
      return 0;
    if (irop_config[uq->op].has_src2 && tcc_ir_op_src2_vreg(ir, uq) == dest_vr)
      return 0;
    if ((uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED ||
         uq->op == TCCIR_OP_STORE_POSTINC || uq->op == TCCIR_OP_FUNCPARAMVAL) &&
        irop_config[uq->op].has_dest)
    {
      int32_t d_vr = tcc_ir_op_dest_vreg(ir, uq);
      if (d_vr == dest_vr)
        return 0;
    }
    if (uq->op == TCCIR_OP_MLA && tcc_ir_op_accum_vreg(ir, uq) == dest_vr)
      return 0;
  }
  if (!in_loop_access)
    return 0;

  {
    int allow[2];
    int n = 0;
    if (allow_a >= 0)
      allow[n++] = allow_a;
    if (allow_b >= 0)
      allow[n++] = allow_b;
    return iv_ctr_eliminable_list(ir, loop, iv->vreg, iv->def_idx, allow, n);
  }
}

static int iv_find_by_vreg(const InductionVar *ivs, int num_ivs, int32_t vr)
{
  for (int k = 0; k < num_ivs; k++)
    if (ivs[k].vreg == vr)
      return k;
  return -1;
}

/* Index of the IV named by iv_vr, or by the source of the ASSIGN/STORE defining it before idx. */
static int iv_find_through_copy(TCCIRState *ir, const InductionVar *ivs, int num_ivs, int32_t iv_vr, int idx)
{
  int iv_idx = iv_find_by_vreg(ivs, num_ivs, iv_vr);
  if (iv_idx >= 0 || iv_vr < 0)
    return iv_idx;
  int def = tcc_ir_find_defining_instruction(ir, iv_vr, idx);
  if (def < 0)
    return -1;
  IRQuadCompact *dq = &ir->compact_instructions[def];
  if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_STORE)
    return -1;
  return iv_find_by_vreg(ivs, num_ivs, tcc_ir_op_src1_vreg(ir, dq));
}

/* Source reads of vr outside instruction skip; with_store_dest also counts a store's dest. */
static int iv_vreg_use_count(TCCIRState *ir, int32_t vr, int skip, int with_store_dest)
{
  int uses = 0;
  for (int j = 0; j < ir->next_instruction_index; j++)
  {
    if (j == skip)
      continue;
    IRQuadCompact *uq = &ir->compact_instructions[j];
    if (tcc_ir_op_src1_vreg(ir, uq) == vr)
      uses++;
    if (tcc_ir_op_src2_vreg(ir, uq) == vr)
      uses++;
    if (with_store_dest &&
        (uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED || uq->op == TCCIR_OP_STORE_POSTINC) &&
        tcc_ir_op_dest_vreg(ir, uq) == vr)
      uses++;
  }
  return uses;
}

/* In-loop SHL/MUL before index i that defines TEMP operand t, else -1: the
 * first such body instruction, provided no body instruction up to it is at or
 * past i. */
static int iv_find_shl_mul_def(TCCIRState *ir, IVScan *sc, IRLoop *loop, int i, IROperand t)
{
  int vr = irop_get_vreg(t);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  if (!sc->built_shl)
  {
    sc->built_shl = 1;
    sc->shl_max_pos = -1;
    sc->body_max = tcc_malloc(((size_t)loop->num_body_instrs + 1) * sizeof(int));
    for (int j = 0, m = -1; j < loop->num_body_instrs; j++)
    {
      if (loop->body_instrs[j] > m)
        m = loop->body_instrs[j];
      sc->body_max[j] = m;
      IRQuadCompact *sq = &ir->compact_instructions[loop->body_instrs[j]];
      if (sq->op != TCCIR_OP_SHL && sq->op != TCCIR_OP_MUL)
        continue;
      int32_t dv = tcc_ir_op_dest_vreg(ir, sq);
      if (iv_vreg_canon(dv) && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP &&
          TCCIR_DECODE_VREG_POSITION(dv) > sc->shl_max_pos)
        sc->shl_max_pos = TCCIR_DECODE_VREG_POSITION(dv);
    }
    sc->shl_pos = tcc_malloc(((size_t)sc->shl_max_pos + 2) * sizeof(int));
    for (int p = 0; p <= sc->shl_max_pos; p++)
      sc->shl_pos[p] = -1;
    for (int j = 0; j < loop->num_body_instrs; j++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[loop->body_instrs[j]];
      if (sq->op != TCCIR_OP_SHL && sq->op != TCCIR_OP_MUL)
        continue;
      int32_t dv = tcc_ir_op_dest_vreg(ir, sq);
      if (iv_vreg_canon(dv) && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP &&
          sc->shl_pos[TCCIR_DECODE_VREG_POSITION(dv)] < 0)
        sc->shl_pos[TCCIR_DECODE_VREG_POSITION(dv)] = j;
    }
  }
  if (iv_vreg_canon(vr))
  {
    int p = TCCIR_DECODE_VREG_POSITION(vr);
    int j = p <= sc->shl_max_pos ? sc->shl_pos[p] : -1;
    return j >= 0 && sc->body_max[j] < i ? loop->body_instrs[j] : -1;
  }
  for (int j = 0; j < loop->num_body_instrs; j++)
  {
    int sj = loop->body_instrs[j];
    if (sj >= i)
      break;
    IRQuadCompact *sq = &ir->compact_instructions[sj];
    if ((sq->op == TCCIR_OP_SHL || sq->op == TCCIR_OP_MUL) &&
        tcc_ir_op_dest_vreg(ir, sq) == vr)
      return sj;
  }
  return -1;
}

int find_derived_ivs(TCCIRState *ir, IRLoop *loop, InductionVar *ivs, int num_ivs, DerivedIV *divs, int max_divs)
{
  int num_divs = 0;
  IVScan sc = {0};

  /* MLA-only extended scan: include j>end_idx whose JMP/JUMPIF targets back into body (rotated loops); MLA-only avoids regressing the ADD-based scan */
  int mla_scan_start = loop->start_idx;
  int mla_scan_end = loop->end_idx;
  {
    int extended;
    do
    {
      extended = 0;
      for (int j = mla_scan_end + 1; j < ir->next_instruction_index; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
          continue;
        int64_t jdest_imm = tcc_ir_op_dest_imm(ir, jq);
        int jtarget = (int)jdest_imm;
        if (jtarget >= mla_scan_start && jtarget <= mla_scan_end)
        {
          mla_scan_end = j;
          extended = 1;
        }
      }
    } while (extended);
  }

  if (TCC_LOG_IV_SR)
  {
    fprintf(stderr, "[IV_SR] Loop body_instrs:");
    for (int bi = 0; bi < loop->num_body_instrs; bi++)
      fprintf(stderr, " %d", loop->body_instrs[bi]);
    fprintf(stderr, " (MLA scan range: [%d..%d])\n", mla_scan_start, mla_scan_end);
  }

  /* Affine INDEXED-DIV candidates, found before anything else because two of
   * the passes below consult them: `T = iv +/- k` (a single-use temp) feeding
   * the index of an indexed access in this loop.  SHA-1's message expansion,
   * `W[i] = W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16]`, reached codegen as four
   * `subs r3, r2, #k` and four scaled-index loads per iteration, because only
   * `W[i]` itself was a DIV and the subtractions then counted as extra reads
   * of the counter that made it non-eliminable -- so even that one was
   * refused.  Recognising the offset accesses as DIVs on the SAME base and
   * stride lets the driver walk one pointer for all of them, each access an
   * immediate off it (gcc's `ldr r0, [r3, #32]`), with the subtractions and
   * the counter gone.  On the M33 an immediate offset is also a cycle cheaper
   * than a scaled register index. */
#define IV_AFFINE_MAX 16
  int affine_def[IV_AFFINE_MAX];
  IROperand affine_base[IV_AFFINE_MAX];
  int affine_stride[IV_AFFINE_MAX];
  int naffine = 0;
  for (int bi = 0; bi < loop->num_body_instrs && naffine < IV_AFFINE_MAX; bi++)
  {
    int i = loop->body_instrs[bi];
    IRQuadCompact *q = &ir->compact_instructions[i];
    if ((q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB) || tcc_ir_barrel_shift_at(ir, q))
      continue;
    int32_t dv = tcc_ir_op_dest_vreg(ir, q);
    if (dv < 0 || tcc_ir_op_dest_is_lval(ir, q) || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int32_t a_vr = tcc_ir_op_src1_vreg(ir, q);
    if (!tcc_ir_op_src2_is_imm(ir, q) || a_vr < 0)
      continue;
    int iv_k = iv_find_by_vreg(ivs, num_ivs, a_vr);
    if (iv_k < 0)
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, dv, -1))
      continue;
    /* The one use must be an indexed access's INDEX slot inside this loop. */
    for (int bj = 0; bj < loop->num_body_instrs; bj++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[loop->body_instrs[bj]];
      if ((uq->op != TCCIR_OP_LOAD_INDEXED && uq->op != TCCIR_OP_STORE_INDEXED) ||
          tcc_ir_op_src2_vreg(ir, uq) != dv)
        continue;
      IROperand sc = tcc_ir_op_get_scale(ir, uq);
      if (!irop_is_immediate(sc))
        break;
      int scale = (int)irop_get_imm64_ex(ir, sc);
      if (scale < 0 || scale > 3)
        break;
      affine_def[naffine] = i;
      affine_base[naffine] = tcc_ir_op_get_dest_or_src1(ir, uq, uq->op == TCCIR_OP_LOAD_INDEXED);
      affine_stride[naffine] = ivs[iv_k].step * (1 << scale);
      naffine++;
      break;
    }
  }
  /* The SHL of every deref-form DIV admitted into a group below joins the
   * allow list, so the group's eliminability check sees all of its own reads. */
  int group_allow[IV_AFFINE_MAX + 8];
  int ngroup_allow = 0;
  for (int a = 0; a < naffine; a++)
    group_allow[ngroup_allow++] = affine_def[a];

  /* Scan the extended body for ADD instructions (DIV computation) */
  for (int bi = 0; bi < loop->num_body_instrs && num_divs < max_divs; bi++)
  {
    int i = loop->body_instrs[bi];
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_ADD || tcc_ir_barrel_shift_at(ir, q))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Pattern: T = base + T_mul_shl  OR  T = T_mul_shl + base */
    IROperand *base_op = &src1;
    int shl_idx = iv_find_shl_mul_def(ir, &sc, loop, i, src2);
    if (shl_idx < 0)
    {
      shl_idx = iv_find_shl_mul_def(ir, &sc, loop, i, src1);
      base_op = &src2;
    }
    int shl_vr = -1, base_vr = -1, is_mul = 0;
    if (shl_idx >= 0)
    {
      IRQuadCompact *sq = &ir->compact_instructions[shl_idx];
      shl_vr = tcc_ir_op_dest_vreg(ir, sq);
      base_vr = irop_get_vreg(*base_op);
      is_mul = (sq->op == TCCIR_OP_MUL);
    }

    if (shl_idx < 0)
      continue; /* Not a base + SHL/MUL pattern */

    /* Check that the SHL/MUL input is an IV */
    IRQuadCompact *shl_q = &ir->compact_instructions[shl_idx];
    IROperand shl_src1 = tcc_ir_op_get_src1(ir, shl_q);
    IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);

    int iv_vr = irop_get_vreg(shl_src1);
    if (iv_vr < 0 || !irop_is_immediate(shl_src2))
    {
      /* Check if src1 is immediate and src2 is IV (for MUL) */
      if (is_mul && irop_is_immediate(shl_src1))
      {
        iv_vr = irop_get_vreg(shl_src2);
        if (iv_vr >= 0)
        {
          IROperand tmp = shl_src1;
          shl_src1 = shl_src2;
          shl_src2 = tmp;
        }
        else
        {
          continue;
        }
      }
      else
      {
        continue;
      }
    }

    int iv_idx = iv_find_through_copy(ir, ivs, num_ivs, iv_vr, shl_idx);

    if (iv_idx < 0)
      continue; /* SHL/MUL operand is not an IV */

    int stride;
    if (is_mul)
    {
      int mul_const = (int)irop_get_imm64_ex(ir, shl_src2);
      stride = ivs[iv_idx].step * mul_const;
    }
    else
    {
      int shift = (int)irop_get_imm64_ex(ir, shl_src2);
      stride = ivs[iv_idx].step * (1 << shift);
    }

    /* ADD result must be used (dead-code check); multiple uses are fine */
    int dest_vr = irop_get_vreg(dest);
    int use_count = iv_vreg_use_count(ir, dest_vr, i, 1);

    if (use_count < 1)
      continue; /* Dead code — skip */

    /* SHL result must be used only by this ADD, else we can't NOP it */
    int shl_vr_uses = iv_vreg_use_count(ir, shl_vr, shl_idx, 0);

    if (shl_vr_uses != 1)
    {
      LOG_IV_SR("IV_SR: Skipping DIV at idx=%d: SHL result has %d uses (not 1)", i, shl_vr_uses);
      continue; /* SHL result used by other instructions - can't NOP it */
    }

    /* A SCALED address that is actually dereferenced buys nothing here.  The
     * SHL+ADD this transform would delete does not survive to the machine
     * anyway: the indexed-memory fusion folds `base + (i << k)` straight into
     * the load/store addressing mode, so the loop is the same length either
     * way and the transform merely trades `str.w r,[base,i,lsl #2]` for
     * `str r,[p]` + `adds p,#4`.  Measured, that trade is a LOSS in situ --
     * mibench_stringsearch's 256-entry fill loop went +12.4% (a reproducible
     * +1 cycle per element) even though a stand-alone asm replica of the two
     * loops is neutral (6.274 vs 6.285 cycles/element) and the isolated
     * addressing-mode probes favour the pointer form.  A per-TU bisect
     * (TCC_DISABLE_PASS=derived_iv on that one file) pinned it to exactly this
     * transform.
     *
     * The exception is the post-indexed shape: when every use of the address
     * is a plain 32-bit access and the counter goes away, the walk's bump
     * folds into the access (`str r,[p],#4`) and the loop DROPS an
     * instruction instead of trading even — see
     * iv_scaled_deref_postinc_viable.
     *
     * The unscaled case is different and is handled by the fourth pass below:
     * there the address computation genuinely stays in the loop, because a
     * byte access off a stack base never gets folded. */
    int joins_group = 0;
    if (iv_div_addr_is_dereffed(ir, &sc, dest_vr) &&
        !iv_scaled_deref_postinc_viable(ir, loop, dest_vr, stride, &ivs[iv_idx], shl_idx, -1))
    {
      /* Not worth a walk on its own -- but as a member of an affine group on
       * the same base it reads through the group's pointer at an immediate,
       * and the group is a win whether or not its own access post-increments
       * (the offset subtractions and the scaled indexes go regardless). */
      IROperand base_here = tcc_ir_op_get_src1(ir, q);
      for (int a = 0; a < naffine && !joins_group; a++)
        if (affine_stride[a] == stride && iv_same_base(ir, affine_base[a], base_here))
          joins_group = 1;
      if (joins_group && ngroup_allow < IV_AFFINE_MAX + 8)
      {
        int allow_tmp[IV_AFFINE_MAX + 8];
        int nt = ngroup_allow;
        memcpy(allow_tmp, group_allow, sizeof(int) * (size_t)ngroup_allow);
        allow_tmp[nt++] = shl_idx;
        if (!iv_ctr_eliminable_list(ir, loop, ivs[iv_idx].vreg, ivs[iv_idx].def_idx, allow_tmp, nt))
          joins_group = 0;
        else
          group_allow[ngroup_allow++] = shl_idx;
      }
    }
    if (iv_div_addr_is_dereffed(ir, &sc, dest_vr) && !joins_group &&
        !iv_scaled_deref_postinc_viable(ir, loop, dest_vr, stride, &ivs[iv_idx], shl_idx, -1))
    {
      LOG_IV_SR("IV_SR: Skipping scaled deref DIV at idx=%d — address folds into the addressing mode anyway", i);
      continue;
    }

    divs[num_divs].iv_idx = iv_idx;
    divs[num_divs].base_vreg = base_vr;
    divs[num_divs].base_op = *base_op;
    divs[num_divs].stride = stride;
    divs[num_divs].use_idx = i;
    divs[num_divs].shl_idx = shl_idx;
    divs[num_divs].share_with = -1;
    divs[num_divs].offset = 0;
    divs[num_divs].off_idx = -1;
    divs[num_divs].origin = 0;
    num_divs++;

    LOG_IV_SR("IV_SR: Found DIV base+%d*VAR%d at ADD idx=%d (SHL idx=%d)", stride, TCCIR_DECODE_VREG_POSITION(iv_vr), i,
              shl_idx);
  }

  /* Second pass: MLA-fused DIV — dest = IV*stride + invariant base */
  if (dbg_mlaiv()) {
    fprintf(stderr, "[MLAIV] scan loop [%d..%d]\n", mla_scan_start, mla_scan_end);
    for (int dbg = mla_scan_start; dbg <= mla_scan_end; dbg++)
      fprintf(stderr, "[MLAIV]   idx %d op=%d\n", dbg, ir->compact_instructions[dbg].op);
  }
  for (int i = mla_scan_start; i <= mla_scan_end && num_divs < max_divs; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_MLA)
      continue;
    if (dbg_mlaiv())
      fprintf(stderr, "[MLAIV] candidate MLA at idx %d\n", i);

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    IROperand accum = tcc_ir_op_get_accum(ir, q);

    /* src2 must be the stride immediate */
    if (!irop_is_immediate(src2))
      continue;

    /* src1 must be an IV (with one level of copy-through) */
    int iv_vr = irop_get_vreg(src1);
    if (iv_vr < 0)
      continue;

    int iv_idx = iv_find_through_copy(ir, ivs, num_ivs, iv_vr, i);
    if (iv_idx < 0)
      continue;

    /* accum (base) must be loop-invariant: not redefined in loop */
    int base_vr = irop_get_vreg(accum);
    if (base_vr >= 0)
    {
      int redefined = 0;
      for (int j = mla_scan_start; j <= mla_scan_end; j++)
      {
        IRQuadCompact *lq = &ir->compact_instructions[j];
        if (lq->op == TCCIR_OP_NOP || j == i)
          continue;
        if (irop_config[lq->op].has_dest)
        {
          int32_t ld_vr = tcc_ir_op_dest_vreg(ir, lq);
          if (ld_vr == base_vr)
          {
            redefined = 1;
            break;
          }
        }
      }
      if (redefined)
        continue;
    }

    int mul_const = (int)irop_get_imm64_ex(ir, src2);
    int stride = ivs[iv_idx].step * mul_const;

    /* Dead-code check: must have at least one use of this MLA's dest. */
    int dest_vr = irop_get_vreg(dest);
    int use_count = iv_vreg_use_count(ir, dest_vr, i, 1);
    if (use_count < 1)
      continue;

    /* Same reasoning as the scaled ADD case above: an MLA-derived address is
     * scaled by construction, so the deref would fold into the addressing
     * mode regardless — unless the walk ends post-indexed (the MLA itself is
     * the whitelisted IV use here; there is no separate SHL/MUL). */
    if (iv_div_addr_is_dereffed(ir, &sc, dest_vr) &&
        !iv_scaled_deref_postinc_viable(ir, loop, dest_vr, stride, &ivs[iv_idx], i, -1))
    {
      LOG_IV_SR("IV_SR: Skipping scaled deref MLA-DIV at idx=%d — folds into the addressing mode anyway", i);
      continue;
    }

    divs[num_divs].iv_idx = iv_idx;
    divs[num_divs].base_vreg = base_vr;
    divs[num_divs].base_op = accum;
    divs[num_divs].stride = stride;
    divs[num_divs].use_idx = i;
    divs[num_divs].shl_idx = -1; /* fused into MLA — nothing to NOP */
    divs[num_divs].share_with = -1;
    divs[num_divs].offset = 0;
    divs[num_divs].off_idx = -1;
    divs[num_divs].origin = 0;
    num_divs++;

    if (dbg_mlaiv())
      fprintf(stderr, "[MLAIV] FOUND MLA-DIV at idx %d, stride=%d, iv_vr=%d, base_vr=%d\n", i, stride, iv_vr, base_vr);
    LOG_IV_SR("IV_SR: Found MLA-DIV base+%d*VAR%d at MLA idx=%d (fused)", stride, TCCIR_DECODE_VREG_POSITION(iv_vr), i);
  }

  /* Third pass: LOAD_INDEXED/STORE_INDEXED DIV — index is a BIV, or a single-use
   * `BIV + k` / `BIV - k`; stride = iv.step*(1<<scale).
   *
   * The affine form is what a sliding window compiles to.  SHA-1's message
   * expansion, `W[i] = W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16]`, reached codegen as
   * four `subs r3, r2, #k` and four scaled-index loads per iteration, because
   * only `W[i]` itself was a DIV and the four subtractions then counted as
   * extra reads of the counter that made it non-eliminable -- so even that one
   * was refused.  Recognising the offset accesses as DIVs on the SAME base and
   * stride lets the driver walk one pointer for all of them, each access an
   * immediate off it (gcc's `ldr r0, [r3, #32]`), with the subtractions and the
   * counter gone.  Measured on the M33 an immediate offset is also a cycle
   * cheaper than a scaled register index. */
  for (int bi = 0; bi < loop->num_body_instrs && num_divs < max_divs; bi++)
  {
    int i = loop->body_instrs[bi];
    IRQuadCompact *q = &ir->compact_instructions[i];

    int is_load = (q->op == TCCIR_OP_LOAD_INDEXED);
    int is_store = (q->op == TCCIR_OP_STORE_INDEXED);
    if (!is_load && !is_store)
      continue;
    /* A fused-shift annotation on the index would be lost by substituting an
     * immediate for it (tccir_operand.c refuses exactly that). */
    if (tcc_ir_barrel_shift_at(ir, q))
      continue;

    IROperand base_op = tcc_ir_op_get_dest_or_src1(ir, q, is_load);
    int32_t iv_vr = tcc_ir_op_src2_vreg(ir, q);
    IROperand scale_op = tcc_ir_op_get_scale(ir, q);
    IROperand val_op = tcc_ir_op_get_dest_or_src1(ir, q, !is_load);

    if (!irop_is_immediate(scale_op))
      continue;

    /* Restrict to INT32: POSTINC fusion only handles INT32, else net regression */
    if (val_op.btype != IROP_BTYPE_INT32)
      continue;
    if (iv_vr < 0)
      continue;

    int iv_idx = iv_find_by_vreg(ivs, num_ivs, iv_vr);

    int aff_off_idx = -1;
    int aff_k = 0; /* index units; scaled into bytes once the scale is known */
    /* Copy-through chase, then the affine `iv +/- k` chase */
    if (iv_idx < 0)
    {
      int def = tcc_ir_find_defining_instruction(ir, iv_vr, i);
      if (def >= 0)
      {
        IRQuadCompact *dq = &ir->compact_instructions[def];
        if (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_STORE)
        {
          iv_idx = iv_find_by_vreg(ivs, num_ivs, tcc_ir_op_src1_vreg(ir, dq));
        }
        else if ((dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB) && !tcc_ir_barrel_shift_at(ir, dq))
        {
          int listed = 0;
          for (int a = 0; a < naffine && !listed; a++)
            listed = (affine_def[a] == def);
          if (listed)
          {
            int32_t src = tcc_ir_op_src1_vreg(ir, dq);
            int64_t k = tcc_ir_op_src2_imm(ir, dq);
            iv_idx = iv_find_by_vreg(ivs, num_ivs, src);
            if (iv_idx >= 0 && k >= -1024 && k <= 1024)
            {
              aff_off_idx = def;
              aff_k = (dq->op == TCCIR_OP_SUB) ? -(int)k : (int)k;
            }
            else
              iv_idx = -1;
          }
        }
      }
    }
    if (iv_idx < 0)
      continue;

    /* Base must be loop-invariant; STORE* dest slot is a use, not a def — skip */
    int base_vr = irop_get_vreg(base_op);
    if (base_vr >= 0)
    {
      int redefined = 0;
      for (int j = loop->start_idx; j <= loop->end_idx; j++)
      {
        IRQuadCompact *lq = &ir->compact_instructions[j];
        if (lq->op == TCCIR_OP_NOP || j == i)
          continue;
        if (lq->op == TCCIR_OP_STORE || lq->op == TCCIR_OP_STORE_INDEXED ||
            lq->op == TCCIR_OP_STORE_POSTINC)
          continue;
        if (irop_config[lq->op].has_dest)
        {
          int32_t ld_vr = tcc_ir_op_dest_vreg(ir, lq);
          if (ld_vr == base_vr)
          {
            redefined = 1;
            break;
          }
        }
      }
      if (redefined)
        continue;
    }

    int scale = (int)irop_get_imm64_ex(ir, scale_op);
    if (scale < 0 || scale > 3)
      continue;
    int stride = ivs[iv_idx].step * (1 << scale);
    int byte_offset = aff_k * (1 << scale);
    if (byte_offset < -4095 || byte_offset > 4095)
      continue; /* not an LDR/STR immediate on this target */

    /* Eliminability gate.  With no affine `iv +/- k` access in the loop this is
     * exactly the historical whole-function, no-whitelist check -- so plain
     * `a[j]` walks (including one read twice, or under a `break`) behave as
     * before.  Only when the loop really has the sliding-window shape do the
     * affine offset computations and group members' SHLs join the whitelist. */
    int elig;
    if (naffine > 0)
      elig = iv_ctr_eliminable_list(ir, loop, ivs[iv_idx].vreg, ivs[iv_idx].def_idx, group_allow, ngroup_allow);
    else
      elig = iv_ctr_eliminable(ir, ivs[iv_idx].vreg, ivs[iv_idx].def_idx, -1, -1);
    if (!elig)
    {
      LOG_IV_SR("IV_SR: Skipping INDEXED-DIV at idx=%d — IV VAR%d not eliminable", i,
                TCCIR_DECODE_VREG_POSITION(ivs[iv_idx].vreg));
      continue;
    }

    divs[num_divs].iv_idx = iv_idx;
    divs[num_divs].base_vreg = base_vr;
    divs[num_divs].base_op = base_op;
    divs[num_divs].stride = stride;
    divs[num_divs].use_idx = i;
    divs[num_divs].shl_idx = -1; /* shift is encoded in the scale field */
    divs[num_divs].share_with = -1;
    divs[num_divs].offset = byte_offset;
    divs[num_divs].off_idx = aff_off_idx;
    divs[num_divs].origin = 0;
    num_divs++;

    LOG_IV_SR("IV_SR: Found INDEXED-DIV base+%d*VAR%d at %s idx=%d (scale=%d, stride=%d)", stride,
              TCCIR_DECODE_VREG_POSITION(iv_vr), is_load ? "LOAD_INDEXED" : "STORE_INDEXED", i, scale, stride);
  }

  /* Fourth pass: UNSCALED derived IV — `T = base + iv`, no shift, no multiply.
   *
   * That is what a byte array produces (`dst[j]`), and nothing else sees it:
   * the first pass keys on a SHL/MUL feeding the ADD and there is none, and a
   * byte access off a stack base never gets folded into LOAD_INDEXED either
   * (the unscaled fusion path rejects a local base).  So the address really
   * does stay in the loop -- `bench_memcpy`'s checksum loop recomputed
   * `mov r2,sp` + `adds r1,r2,r0` every iteration around its `ldrb`.  Unlike
   * the scaled cases rejected above, replacing that with a pointer walk
   * genuinely removes instructions.
   *
   * Restricted to a dereferenced address (this is about addressing, not
   * arithmetic) whose counter is provably removable -- with stride 1 the
   * pointer bump costs exactly what the index bump cost, so the win only
   * exists if the index goes away. */
  for (int bi = 0; bi < loop->num_body_instrs && num_divs < max_divs; bi++)
  {
    int i = loop->body_instrs[bi];
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD || tcc_ir_barrel_shift_at(ir, q))
      continue;

    int32_t dest_vr = tcc_ir_op_dest_vreg(ir, q);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP || tcc_ir_op_dest_is_lval(ir, q))
      continue;
    if (!iv_div_addr_is_dereffed(ir, &sc, dest_vr))
      continue;

    /* Skip an ADD an earlier pass already claimed. */
    int dup = 0;
    for (int d = 0; d < num_divs; d++)
      if (divs[d].use_idx == i)
        dup = 1;
    if (dup)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Exactly one operand is a BIV of this loop; the other is the base. */
    int iv_idx = -1;
    IROperand base_op = IROP_NONE;
    for (int side = 0; side < 2 && iv_idx < 0; side++)
    {
      IROperand cand = side ? src2 : src1;
      IROperand other = side ? src1 : src2;
      int32_t cand_vr = irop_get_vreg(cand);
      /* A local VAR is read through an lval operand (`V0` + is_local) -- the
       * plain "fetch the variable" form, not a dereference, and exactly how
       * the IV appears here. */
      if (cand_vr < 0 ||
          (cand.is_lval && !(TCCIR_DECODE_VREG_TYPE(cand_vr) == TCCIR_VREG_TYPE_VAR && cand.is_local)))
        continue;
      if (irop_is_immediate(other))
        continue;
      for (int k = 0; k < num_ivs; k++)
      {
        if (ivs[k].vreg != cand_vr)
          continue;
        int32_t other_vr = irop_get_vreg(other);
        int other_is_iv = 0;
        for (int k2 = 0; k2 < num_ivs; k2++)
          if (other_vr >= 0 && ivs[k2].vreg == other_vr)
            other_is_iv = 1;
        if (other_is_iv)
          break; /* `i + j` is not a base plus an index */
        iv_idx = k;
        base_op = other;
        break;
      }
    }
    if (iv_idx < 0)
      continue;

    /* Base must be loop-invariant. */
    int32_t base_vr = irop_get_vreg(base_op);
    if (base_vr >= 0)
    {
      int redefined = 0;
      for (int bj = 0; bj < loop->num_body_instrs && !redefined; bj++)
      {
        int j = loop->body_instrs[bj];
        IRQuadCompact *lq = &ir->compact_instructions[j];
        if (lq->op == TCCIR_OP_NOP || j == i)
          continue;
        if (irop_config[lq->op].has_dest && tcc_ir_op_dest_vreg(ir, lq) == base_vr)
          redefined = 1;
      }
      if (redefined)
        continue;
    }

    if (!iv_ctr_eliminable(ir, ivs[iv_idx].vreg, ivs[iv_idx].def_idx, i, -1))
    {
      LOG_IV_SR("IV_SR: Skipping unscaled DIV at idx=%d — IV VAR%d not eliminable", i,
                TCCIR_DECODE_VREG_POSITION(ivs[iv_idx].vreg));
      continue;
    }

    divs[num_divs].iv_idx = iv_idx;
    divs[num_divs].base_vreg = base_vr;
    divs[num_divs].base_op = base_op;
    divs[num_divs].stride = ivs[iv_idx].step;
    divs[num_divs].use_idx = i;
    divs[num_divs].shl_idx = -1; /* no shift to NOP */
    divs[num_divs].share_with = -1;
    divs[num_divs].offset = 0;
    divs[num_divs].off_idx = -1;
    divs[num_divs].origin = 0;
    num_divs++;

    LOG_IV_SR("IV_SR: Found unscaled DIV base+%d*VAR%d at ADD idx=%d", ivs[iv_idx].step,
              TCCIR_DECODE_VREG_POSITION(ivs[iv_idx].vreg), i);
  }

  iv_scan_free(&sc);
  return num_divs;
}
