/*
 *  ir_eval.h - a tiny reference interpreter over a TccIrOp subset (Track 4)
 *
 *  Part of the IR metamorphic / semantics-preservation fuzzer described in
 *  docs/plan_bug_hunting.md (Track 4). The interpreter is an *independent
 *  oracle*: it computes a result vector from input register vectors WITHOUT
 *  reference to the optimizer pass under test. The metamorphic driver asserts
 *
 *      eval(f) == eval(P(f))
 *
 *  for every linked legacy pass P; a mismatch is a candidate non-semantics-
 *  preserving pass (a miscompile).
 *
 *  ───────────────────────────────────────────────────────────────────────────
 *  VALUE MODEL (must match the compiler's constant-fold model exactly, see
 *  ir/opt_constprop.c ~line 2083 "Constant fold"):
 *
 *    Every register holds an int64_t.  After each compute the result is
 *    *canonicalized* to the destination operand's btype width:
 *      - INT8 / INT16 / INT32 : truncate to the low 32 bits, then sign-extend
 *        to int64 (so 0x80000000 + 0x80000000 wraps to 0; a 32-bit register is
 *        always stored as its sign-extended-to-64 value, exactly like the fold
 *        code: result = (int64_t)(int32_t)(uint32_t)result).
 *      - INT64 : keep the full 64 bits.
 *    Sub-word (INT8/INT16) destinations are NOT narrowed by the arithmetic ops
 *    themselves — the legacy passes fold sub-word arithmetic at 32-bit width
 *    too (the fold code only special-cases INT64). The narrowing to a byte/half
 *    is the job of ZEXT / explicit width ops, which we model separately.
 *
 *  PHASE 1 (this file): straight-line value computation over
 *    ADD SUB MUL AND OR XOR SHL SHR SAR DIV UDIV IMOD UMOD ROR
 *    ASSIGN ZEXT UBFX BFI BOOL_AND BOOL_OR  (+ NOP, ignored)
 *  Inputs arrive in PARAM vregs; TEMP/VAR vregs are computed. The "result
 *  vector" is the value of every defined TEMP at the end of the function
 *  (a deterministic fingerprint of straight-line execution).
 *
 *  PHASE 2/3 (load/store to a modeled stack; jump/jumpif/return control flow):
 *  NOT implemented — the generator only emits the phase-1 subset, so a
 *  metamorphic mismatch can never be blamed on an un-modeled op. See the
 *  TODO hooks at the bottom of this file.
 *  ───────────────────────────────────────────────────────────────────────────
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License.
 */

#ifndef TCC_UT_IR_EVAL_H
#define TCC_UT_IR_EVAL_H

#include "ir_build.h"

#include <stdint.h>
#include <string.h>

/* Max distinct vreg position the interpreter tracks per class.  The generator
 * stays well under this. */
#define IRE_MAX_POS 128

typedef enum IreStatus
{
  IRE_OK = 0,
  IRE_UNSUPPORTED_OP = 1, /* op outside the phase-1 subset was encountered */
  IRE_TRAP = 2,           /* division by zero / shift UB / other trap */
  IRE_OOB = 3,            /* vreg position out of the tracked range */
} IreStatus;

/* Per-class register file.  defined[] marks which positions have been written
 * (so the result fingerprint only includes computed values). */
typedef struct IreRegs
{
  int64_t temp[IRE_MAX_POS];
  uint8_t temp_def[IRE_MAX_POS];
  int64_t var[IRE_MAX_POS];
  uint8_t var_def[IRE_MAX_POS];
  int64_t param[IRE_MAX_POS]; /* inputs, set before eval */
  uint8_t param_def[IRE_MAX_POS];
} IreRegs;

/* The result vector: every defined TEMP value, in position order, plus a
 * status code. Two functions are "equal" iff status and the full temp vector
 * (over defined positions present in EITHER run) agree. */
typedef struct IreResult
{
  IreStatus status;
  int64_t temp[IRE_MAX_POS];
  uint8_t temp_def[IRE_MAX_POS];
} IreResult;

/* ---- value canonicalization (mirrors the fold truncation) ---- */

static inline int64_t ire_canon(int64_t v, int btype)
{
  if (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64)
    return v;
  /* INT8/INT16/INT32/etc: model the 32-bit register, sign-extended to 64. */
  return (int64_t)(int32_t)(uint32_t)v;
}

/* ---- operand read ---- */

/* Read the int64 value of a source operand.  Returns 1 on success, 0 if the
 * operand references an out-of-range / undefined vreg (caller treats as OOB). */
static inline int ire_read(const TCCIRState *ir, const IreRegs *rf, IROperand op, int64_t *out)
{
  if (irop_is_immediate(op))
  {
    *out = irop_get_imm64_ex(ir, op);
    return 1;
  }
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
  {
    /* NONE / non-vreg non-immediate (symref/stackoff) — unsupported as a value
     * source in phase 1.  Signal OOB so the caller bails the whole eval. */
    return 0;
  }
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (pos < 0 || pos >= IRE_MAX_POS)
    return 0;
  switch (type)
  {
  case TCCIR_VREG_TYPE_TEMP:
    if (!rf->temp_def[pos])
      return 0;
    *out = rf->temp[pos];
    return 1;
  case TCCIR_VREG_TYPE_VAR:
    if (!rf->var_def[pos])
      return 0;
    *out = rf->var[pos];
    return 1;
  case TCCIR_VREG_TYPE_PARAM:
    if (!rf->param_def[pos])
      return 0;
    *out = rf->param[pos];
    return 1;
  default:
    return 0;
  }
}

/* ---- operand write ---- */

static inline int ire_write(IreRegs *rf, IROperand dest, int64_t val)
{
  int32_t vr = irop_get_vreg(dest);
  if (vr < 0)
    return 0;
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (pos < 0 || pos >= IRE_MAX_POS)
    return 0;
  val = ire_canon(val, irop_get_btype(dest));
  switch (type)
  {
  case TCCIR_VREG_TYPE_TEMP:
    rf->temp[pos] = val;
    rf->temp_def[pos] = 1;
    return 1;
  case TCCIR_VREG_TYPE_VAR:
    rf->var[pos] = val;
    rf->var_def[pos] = 1;
    return 1;
  case TCCIR_VREG_TYPE_PARAM:
    rf->param[pos] = val;
    rf->param_def[pos] = 1;
    return 1;
  default:
    return 0;
  }
}

/* ---- the binary/unary compute (mirrors opt_constprop.c fold semantics) ---- */

/* Compute one op into *out. Returns IRE_OK / IRE_TRAP / IRE_UNSUPPORTED_OP.
 * btype is the *source* btype (= what the fold code keys division width on). */
static inline IreStatus ire_compute(TccIrOp op, int64_t a, int64_t b, int btype, int dest_btype, int64_t *out)
{
  int is64 = (btype == IROP_BTYPE_INT64);
  switch (op)
  {
  case TCCIR_OP_ADD:
    *out = (int64_t)((uint64_t)a + (uint64_t)b);
    return IRE_OK;
  case TCCIR_OP_SUB:
    *out = (int64_t)((uint64_t)a - (uint64_t)b);
    return IRE_OK;
  case TCCIR_OP_MUL:
    *out = (int64_t)((uint64_t)a * (uint64_t)b);
    return IRE_OK;
  case TCCIR_OP_AND:
    *out = a & b;
    return IRE_OK;
  case TCCIR_OP_OR:
    *out = a | b;
    return IRE_OK;
  case TCCIR_OP_XOR:
    *out = a ^ b;
    return IRE_OK;
  case TCCIR_OP_SHL:
    /* The generator constrains the shift amount to 0..31 (32-bit types) so
     * this is well defined and matches `(uint64_t)a << b`. */
    *out = (int64_t)((uint64_t)a << b);
    return IRE_OK;
  case TCCIR_OP_SHR:
    if (is64)
      *out = (int64_t)((uint64_t)a >> b);
    else
      *out = (int64_t)((uint32_t)a >> b);
    return IRE_OK;
  case TCCIR_OP_SAR:
    *out = a >> b;
    return IRE_OK;
  case TCCIR_OP_ROR:
  {
    uint32_t v = (uint32_t)a;
    uint32_t n = (uint32_t)b & 31;
    if (n == 0)
      *out = (int64_t)(int32_t)v;
    else
      *out = (int64_t)(int32_t)((v >> n) | (v << (32 - n)));
    return IRE_OK;
  }
  case TCCIR_OP_BOOL_AND:
    *out = (a != 0) && (b != 0) ? 1 : 0;
    return IRE_OK;
  case TCCIR_OP_BOOL_OR:
    *out = (a != 0) || (b != 0) ? 1 : 0;
    return IRE_OK;
  case TCCIR_OP_DIV:
    if (b == 0)
      return IRE_TRAP;
    if (b == -1 && ((is64 && a == INT64_MIN) || (!is64 && (int32_t)a == INT32_MIN)))
      return IRE_TRAP; /* two's-complement overflow */
    *out = a / b;
    return IRE_OK;
  case TCCIR_OP_IMOD:
    if (b == 0)
      return IRE_TRAP;
    if (b == -1 && ((is64 && a == INT64_MIN) || (!is64 && (int32_t)a == INT32_MIN)))
      return IRE_TRAP;
    *out = a % b;
    return IRE_OK;
  case TCCIR_OP_UDIV:
    if (b == 0)
      return IRE_TRAP;
    if (is64)
      *out = (int64_t)((uint64_t)a / (uint64_t)b);
    else
      *out = (int64_t)((uint32_t)a / (uint32_t)b);
    return IRE_OK;
  case TCCIR_OP_UMOD:
    if (b == 0)
      return IRE_TRAP;
    if (is64)
      *out = (int64_t)((uint64_t)a % (uint64_t)b);
    else
      *out = (int64_t)((uint32_t)a % (uint32_t)b);
    return IRE_OK;
  case TCCIR_OP_UBFX:
  {
    /* dest = (a >> lsb) & ((1<<width)-1); b = lsb | (width<<5) */
    int lsb = (int)b & 0x1F;
    int width = ((int)b >> 5) & 0x1F;
    if (width <= 0 || width > 32)
      return IRE_UNSUPPORTED_OP;
    if (width == 32)
      *out = (int64_t)(uint32_t)((uint32_t)a >> lsb);
    else
      *out = (int64_t)(((uint32_t)a >> lsb) & (((uint32_t)1 << width) - 1));
    (void)dest_btype;
    return IRE_OK;
  }
  default:
    return IRE_UNSUPPORTED_OP;
  }
}

/* ZEXT: zero-extend the low 32 bits of src into the dest.  This mirrors the
 * backend, which lowers ZEXT exactly like ASSIGN of a 32-bit src (low = src,
 * high = 0) — see ir/codegen.c TCCIR_OP_ZEXT.  It is NOT a sub-word mask: the
 * frontend only emits ZEXT with an INT32 or INT64 (VT_LLONG) dest.
 *   - INT64 dest : value is the unsigned 32-bit src zero-extended to 64 bits.
 *   - INT32 dest : value is the unsigned 32-bit src (ire_canon then re-signs it
 *     for register storage, which is the correct 32-bit-register model). */
static inline int64_t ire_zext(int64_t a, int dest_btype)
{
  if (dest_btype == IROP_BTYPE_INT64)
    return (int64_t)(uint64_t)(uint32_t)a; /* zero-extend low 32 to 64 */
  return (int64_t)(uint32_t)a;             /* 32-bit unsigned; canon re-signs */
}

/* ============================================================================
 *  Evaluator
 * ============================================================================
 * Runs the straight-line phase-1 subset.  `inputs`/`input_count` provide the
 * PARAM values (param position i <- inputs[i]).  The result captures every
 * defined TEMP and a status code.  Control-flow ops, loads/stores, calls, and
 * any op outside the subset set status=IRE_UNSUPPORTED_OP and stop (so the
 * caller skips that function rather than risk a false mismatch).
 */
static inline void ire_eval(const TCCIRState *ir, const int64_t *inputs, int input_count, IreResult *res)
{
  IreRegs rf;
  memset(&rf, 0, sizeof(rf));
  for (int i = 0; i < input_count && i < IRE_MAX_POS; ++i)
  {
    rf.param[i] = inputs[i];
    rf.param_def[i] = 1;
  }

  memset(res, 0, sizeof(*res));
  res->status = IRE_OK;

  int n = ir->next_instruction_index;
  for (int i = 0; i < n; ++i)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    TccIrOp op = q->op;

    if (op == TCCIR_OP_NOP)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);

    if (op == TCCIR_OP_ASSIGN)
    {
      int64_t v;
      if (!ire_read(ir, &rf, s1, &v))
      {
        res->status = IRE_OOB;
        goto done;
      }
      if (!ire_write(&rf, dest, v))
      {
        res->status = IRE_OOB;
        goto done;
      }
      continue;
    }

    if (op == TCCIR_OP_ZEXT)
    {
      int64_t v;
      if (!ire_read(ir, &rf, s1, &v))
      {
        res->status = IRE_OOB;
        goto done;
      }
      v = ire_zext(v, irop_get_btype(dest));
      if (!ire_write(&rf, dest, v))
      {
        res->status = IRE_OOB;
        goto done;
      }
      continue;
    }

    if (op == TCCIR_OP_BFI)
    {
      /* dest = (s1 with field[lsb,width] := low `width` bits of s2).
       * lsb/width live in ir->bfi_params[orig_index] (width>=1 => entry!=0).
       * The unit harness builds these directly. */
      int64_t host, val;
      if (!ire_read(ir, &rf, s1, &host) || !ire_read(ir, &rf, s2, &val))
      {
        res->status = IRE_OOB;
        goto done;
      }
      uint16_t enc = 0;
      if (ir->bfi_params)
        enc = ir->bfi_params[q->orig_index];
      if (enc == 0)
      {
        res->status = IRE_UNSUPPORTED_OP;
        goto done;
      }
      int lsb = enc & 0xFF;
      int width = (enc >> 8) & 0xFF;
      if (width <= 0 || width > 32 || lsb < 0 || lsb + width > 32)
      {
        res->status = IRE_UNSUPPORTED_OP;
        goto done;
      }
      uint32_t field_mask = (width >= 32) ? 0xFFFFFFFFu : (((uint32_t)1 << width) - 1);
      uint32_t fld = ((uint32_t)val & field_mask) << lsb;
      uint32_t clr = (uint32_t)host & ~(field_mask << lsb);
      int64_t out = (int64_t)(int32_t)(clr | fld);
      if (!ire_write(&rf, dest, out))
      {
        res->status = IRE_OOB;
        goto done;
      }
      continue;
    }

    /* Generic binary / unary compute ops. */
    if (irop_config[op].has_dest && irop_config[op].has_src1 && irop_config[op].has_src2)
    {
      int64_t a, b;
      if (!ire_read(ir, &rf, s1, &a) || !ire_read(ir, &rf, s2, &b))
      {
        res->status = IRE_OOB;
        goto done;
      }
      int btype = irop_get_btype(s1);
      int64_t out;
      IreStatus st = ire_compute(op, a, b, btype, irop_get_btype(dest), &out);
      if (st != IRE_OK)
      {
        res->status = st;
        goto done;
      }
      if (!ire_write(&rf, dest, out))
      {
        res->status = IRE_OOB;
        goto done;
      }
      continue;
    }

    /* Anything else (control flow, memory, calls, CMP/SETIF, ...) is outside
     * the phase-1 subset — bail so we never blame an un-modeled op. */
    res->status = IRE_UNSUPPORTED_OP;
    goto done;
  }

done:
  memcpy(res->temp, rf.temp, sizeof(rf.temp));
  memcpy(res->temp_def, rf.temp_def, sizeof(rf.temp_def));
}

/* ---- result comparison ---- */

/* Returns 1 if two results are equivalent (same status; same value on every
 * TEMP defined in either run).  When the status is non-OK in either run we
 * only require the *statuses* to match — a trap/unsupported function carries
 * no meaningful value vector. */
static inline int ire_result_equal(const IreResult *a, const IreResult *b)
{
  if (a->status != b->status)
    return 0;
  if (a->status != IRE_OK)
    return 1;
  for (int i = 0; i < IRE_MAX_POS; ++i)
  {
    if (a->temp_def[i] || b->temp_def[i])
    {
      if (a->temp_def[i] != b->temp_def[i])
        return 0;
      if (a->temp[i] != b->temp[i])
        return 0;
    }
  }
  return 1;
}

/* Index of the first differing TEMP position (for diagnostics); -1 if equal
 * value-wise. */
static inline int ire_first_diff(const IreResult *a, const IreResult *b)
{
  for (int i = 0; i < IRE_MAX_POS; ++i)
  {
    if (a->temp_def[i] != b->temp_def[i])
      return i;
    if (a->temp_def[i] && a->temp[i] != b->temp[i])
      return i;
  }
  return -1;
}

/* ============================================================================
 *  TODO hooks (phases 2 & 3 — left intentionally unimplemented)
 * ============================================================================
 *  Phase 2 (modeled stack): add an `int64_t stack[]` byte/word array to IreRegs;
 *    handle TCCIR_OP_LOAD / TCCIR_OP_STORE on direct StackLoc[off] lvalues
 *    (is_lval STACKOFF operands, honoring load width + sign/zero extension as
 *    in opt_knownbits kb_apply_load_width). The generator would then emit a
 *    fixed set of disjoint slots so stores never alias unpredictably.
 *  Phase 3 (control flow): replace the linear `for i` loop with a PC + a step
 *    budget; handle JUMP (set PC = target), JUMPIF (CMP result + cond token),
 *    RETURNVALUE / RETURNVOID (capture a return value into the result). Targets
 *    are instruction indices; the generator already keeps them in range.
 *  Until those land, ire_eval() reports IRE_UNSUPPORTED_OP for any such op and
 *  the generator never emits them — so a metamorphic mismatch is never a
 *  false positive from an un-modeled op.
 */

#endif /* TCC_UT_IR_EVAL_H */
