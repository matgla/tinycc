/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* longlong.c -- 64-bit value expand/build and the 64-bit operator lowering.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

#if PTR_SIZE == 4
/* expand 64bit on stack in two ints */
ST_FUNC void lexpand(void)
{
  int u, v;
  u = vtop->type.t & (VT_DEFSIGN | VT_UNSIGNED);
  v = vtop->r & (VT_VALMASK | VT_LVAL);
  if (v == VT_CONST)
  {
    vdup();
    vtop[0].c.i >>= 32;
  }
  else if (v == (VT_LVAL | VT_CONST) || v == (VT_LVAL | VT_LOCAL))
  {
    /* For IR mode, we need to generate explicit load operations */
    if (tcc_state->ir)
    {
      /* Load the full 64-bit value first, then split it */
      SValue full;
      SValue low32;
      SValue shifted64;
      SValue shift_amt;

      memset(&full, 0, sizeof(full));
      full.type.t = vtop->type.t;
      full.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      full.r = 0;
      if ((full.type.t & VT_BTYPE) == VT_LLONG)
        tcc_ir_set_llong_type(tcc_state->ir, full.vr);

      /* Force load of the 64-bit value */
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &full);

      /* Create explicit low32 = (uint32_t)full. */
      memset(&low32, 0, sizeof(low32));
      low32.type.t = VT_INT | u;
      low32.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      low32.r = 0;
      int old_prevent_coalescing = tcc_state->ir->prevent_coalescing;
      tcc_state->ir->prevent_coalescing = 1;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &full, NULL, &low32);
      tcc_state->ir->prevent_coalescing = old_prevent_coalescing;

      /* Bottom of stack becomes low32. */
      vtop->type.t = VT_INT | u;
      vtop->vr = low32.vr;
      vtop->r = 0;

      /* Duplicate and turn the new top into the high32 word. */
      vdup();
      vtop[0].type.t = VT_INT | u;
      vtop[0].vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      vtop[0].r = 0;

      memset(&shift_amt, 0, sizeof(shift_amt));
      shift_amt.type.t = VT_INT;
      shift_amt.r = VT_CONST;
      shift_amt.c.i = 32;
      shift_amt.vr = -1;

      /* shifted64 = full >> 32 (64-bit). */
      memset(&shifted64, 0, sizeof(shifted64));
      shifted64.type.t = VT_LLONG | u;
      shifted64.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      shifted64.r = 0;
      tcc_ir_set_llong_type(tcc_state->ir, shifted64.vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_SHR, &full, &shift_amt, &shifted64);

      /* high32 = (uint32_t)shifted64 (i.e. original high word).
       * IMPORTANT: prevent coalescing here! The SHR must remain a 64-bit operation
       * to correctly extract the high word. If coalesced with this 32-bit ASSIGN,
       * the SHR's dest type would become 32-bit and codegen would emit a 32-bit shift
       * instead of a 64-bit shift, causing the high word to be lost. */
      old_prevent_coalescing = tcc_state->ir->prevent_coalescing;
      tcc_state->ir->prevent_coalescing = 1;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &shifted64, NULL, &vtop[0]);
      tcc_state->ir->prevent_coalescing = old_prevent_coalescing;
    }
    else
    {
      vdup();
      vtop[0].c.i += 4;
    }
  }
  else
  {
    /* For IR mode: materialize the full 64-bit value into a temp vreg first,
     * then create two independent 32-bit values:
     * - low word: low32 = (uint32_t)full
     * - high word: high32 = (uint32_t)(full >> 32)
     *
     * IMPORTANT: do NOT reuse the 64-bit vreg as a 32-bit "view".
     * That causes later 64-bit ops (like lbuild's (high<<32)|low) to
     * accidentally see/propagate the full's high word via pr1.
     * Also, shifting by 32 must be done as a 64-bit shift; emitting a 32-bit
     * SHR #32 is not encodable on Thumb and leads to wrong codegen.
     */
    if (tcc_state->ir)
    {
      SValue full;
      SValue low32;
      SValue shifted64;
      SValue shift_amt;

      memset(&full, 0, sizeof(full));
      full.type.t = vtop->type.t;
      full.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      full.r = 0;
      if ((full.type.t & VT_BTYPE) == VT_LLONG)
        tcc_ir_set_llong_type(tcc_state->ir, full.vr);
      /* Force a value-producing vreg (loads from lvalues if needed). */
      int assign_pos = tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &full);

      /* If coalescing happened, update full.vr to match the coalesced instruction's dest */
      if (assign_pos < tcc_state->ir->next_instruction_index)
      {
        IROperand dest = tcc_ir_get_dest(tcc_state->ir, assign_pos);
        full.vr = irop_get_vreg(dest);
        /* Also update full.type to match the coalesced instruction's dest type! */
        full.type.t = irop_btype_to_vt_btype(irop_get_btype(dest));
        if (dest.is_unsigned)
          full.type.t |= VT_UNSIGNED;
      }

      /* Create explicit low32 = (uint32_t)full. */
      memset(&low32, 0, sizeof(low32));
      low32.type.t = VT_INT | u;
      low32.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      low32.r = 0;
      int old_prevent_coalescing = tcc_state->ir->prevent_coalescing;
      tcc_state->ir->prevent_coalescing = 1;
      int low_assign_pos = tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &full, NULL, &low32);
      tcc_state->ir->prevent_coalescing = old_prevent_coalescing;

      /* IMPORTANT (IR mode): prevent ASSIGN coalescing here.
       *
       * lexpand splits a 64-bit value `full` into low/high 32-bit words.
       * We still need `full` for the subsequent (full >> 32) extraction.
       *
       * The IR layer has an ASSIGN coalescing peephole that can rewrite the
       * previous instruction's destination to our `low32` and drop this ASSIGN
       * when the source is a TEMP produced by the previous instruction.
       *
       * That optimization is invalid for lexpand: it would make the original
       * `full` vreg undefined for the later shift, causing codegen to read from
       * uninitialized registers (observed as stray use of r9 in mul_s).
       */
      (void)low_assign_pos;

      /* NOTE: do not update full.vr based on this ASSIGN.
       * This instruction produces a 32-bit low word; if we overwrite full.vr
       * here, the later (full >> 32) would accidentally shift the low word,
       * yielding a zero high word and breaking 64-bit math.
       * (low_assign_pos is kept for debugging / symmetry with the earlier ASSIGN.) */
      /* low_assign_pos is kept only for debugging/symmetry. */

      /* Bottom of stack becomes low32. */
      vtop->type.t = VT_INT | u;
      vtop->vr = low32.vr;
      vtop->r = 0;

      /* Duplicate and turn the new top into the high32 word. */
      vdup();
      vtop[0].type.t = VT_INT | u;
      vtop[0].vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      vtop[0].r = 0;

      memset(&shift_amt, 0, sizeof(shift_amt));
      shift_amt.type.t = VT_INT;
      shift_amt.r = VT_CONST;
      shift_amt.c.i = 32;
      shift_amt.vr = -1;

      /* shifted64 = full >> 32 (64-bit). */
      memset(&shifted64, 0, sizeof(shifted64));
      shifted64.type.t = VT_LLONG | u;
      shifted64.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      shifted64.r = 0;
      tcc_ir_set_llong_type(tcc_state->ir, shifted64.vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_SHR, &full, &shift_amt, &shifted64);

      /* high32 = (uint32_t)shifted64 (i.e. original high word).
       * IMPORTANT: prevent coalescing here! The SHR must remain a 64-bit operation
       * to correctly extract the high word. If coalesced with this 32-bit ASSIGN,
       * the SHR's dest type would become 32-bit and codegen would emit a 32-bit shift
       * instead of a 64-bit shift, causing the high word to be lost. */
      old_prevent_coalescing = tcc_state->ir->prevent_coalescing;
      tcc_state->ir->prevent_coalescing = 1;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &shifted64, NULL, &vtop[0]);
      tcc_state->ir->prevent_coalescing = old_prevent_coalescing;
    }
  }
  vtop[0].type.t = vtop[-1].type.t = VT_INT | u;
}
#endif

#if PTR_SIZE == 4
/* build a long long from two ints */
void lbuild(int t)
{
  /* For IR mode: combine low and high vregs into a single 64-bit vreg.
   * Generate an OR operation: (high << 32) | low
   *
   * Handle cases where one or both operands are constants (vr == -1).
   * Constants are encoded with VT_CONST in .r and the value in .c.i.
   */
  if (tcc_state->ir)
  {
    SValue low = vtop[-1];
    SValue high = vtop[0];
    /* Check if we have valid operands (either vreg or constant) */
    int low_is_const = (low.vr < 0) && ((low.r & VT_VALMASK) == VT_CONST);
    int high_is_const = (high.vr < 0) && ((high.r & VT_VALMASK) == VT_CONST);
    int low_is_vreg = (low.vr >= 0);
    int high_is_vreg = (high.vr >= 0);

    /* Only proceed if both operands are valid (vreg or constant) */
    if ((low_is_vreg || low_is_const) && (high_is_vreg || high_is_const))
    {
      /* Special case: both are constants - compute result directly */
      if (low_is_const && high_is_const)
      {
        uint64_t result_val = ((uint64_t)(uint32_t)high.c.i << 32) | (uint32_t)low.c.i;
        vtop[-1].c.i = (long long)result_val;
        vtop[-1].type.t = t;
        vtop[-1].r = VT_CONST;
        vtop[-1].vr = -1;
        vpop();
        return;
      }

      /* In IR mode, vtop entries may still carry address-like VT_LOCAL
       * flags. lbuild must operate on the VALUES, not addresses.
       * Force both operands to be treated as rvalues when emitting IR. */
      {
        const int low_kind = low.r & VT_VALMASK;
        if ((low_kind == VT_LOCAL || low_kind == VT_LLOCAL) && !(low.r & VT_LVAL))
          low.r |= VT_LVAL;
        const int high_kind = high.r & VT_VALMASK;
        if ((high_kind == VT_LOCAL || high_kind == VT_LLOCAL) && !(high.r & VT_LVAL))
          high.r |= VT_LVAL;
      }

      /* Create new 64-bit temp vreg for result */
      int result_vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      if ((t & VT_BTYPE) == VT_LLONG)
        tcc_ir_set_llong_type(tcc_state->ir, result_vr);
      /* Special case: high word is constant 0 — emit a dedicated zero-extend
       * op.  This is opaque to the IR optimizer's value tracker, which would
       * sign-extend the low half if we used `low OR 0_u64`.  The codegen
       * lowers ZEXT to "low half = low, high half = 0" (same as ASSIGN of a
       * 32-bit src into a 64-bit dest), but the dedicated opcode survives
       * copy-propagation so the widening always reaches the backend. */
      if (high_is_const && high.c.i == 0)
      {
        SValue result;
        memset(&result, 0, sizeof(result));
        result.type.t = t;
        result.vr = result_vr;
        result.r = 0;
        if ((result.type.t & VT_BTYPE) == VT_LLONG)
          tcc_ir_set_llong_type(tcc_state->ir, result.vr);

        tcc_ir_put(tcc_state->ir, TCCIR_OP_ZEXT, &low, NULL, &result);

        vtop[-1].vr = result_vr;
        vtop[-1].type.t = t;
        vtop[-1].r = 0;
        vpop();
        return;
      }

      /* First shift high word left by 32: high_shifted = high << 32 */
      SValue shift_amt;
      memset(&shift_amt, 0, sizeof(shift_amt));
      shift_amt.type.t = VT_INT;
      shift_amt.r = VT_CONST;
      shift_amt.c.i = 32;
      shift_amt.vr = -1;

      SValue high_shifted;
      memset(&high_shifted, 0, sizeof(high_shifted));
      high_shifted.type.t = VT_LLONG;
      high_shifted.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      tcc_ir_set_llong_type(tcc_state->ir, high_shifted.vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_SHL, &high, &shift_amt, &high_shifted);

      /* Then OR with low word: result = high_shifted | low */
      SValue result;
      memset(&result, 0, sizeof(result));
      result.type.t = t;
      result.vr = result_vr;
      if ((result.type.t & VT_BTYPE) == VT_LLONG)
        tcc_ir_set_llong_type(tcc_state->ir, result.vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_OR, &high_shifted, &low, &result);

      vtop[-1].vr = result_vr;
      vtop[-1].type.t = t;
      vtop[-1].r = 0;
      vpop();
      return;
    }
  }
}
#endif

#if PTR_SIZE == 4
/* Inspect the direct IR producer of a 64-bit vreg to see if it is a 32->64
 * extension emitted by gen_cast.  Returns:
 *   1 for zero-extension via TCCIR_OP_ZEXT (out *out_low_vr is the 32-bit src)
 *   2 for sign-extension via the canonical SHL #32 + OR low pattern
 *   0 otherwise.
 *
 * Intentionally strict: only looks at the direct producer of `vr`, no
 * ASSIGN-chain walking.  gen_cast for 32->64 unsigned emits ZEXT directly into
 * the destination vreg, and the signed path emits OR(SHL(SAR(low,31),32),low)
 * directly.  Anything in between (extra ASSIGNs from gv_dup, etc.) is opaque
 * and we conservatively bail. */
static int detect_ll_ext_provenance(int vr, int *out_low_vr)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir || vr < 0)
    return 0;
  int n = ir->next_instruction_index;
  if (n <= 0)
    return 0;

  int def = tcc_ir_find_defining_instruction(ir, vr, n);
  if (def < 0)
    return 0;
  IRQuadCompact *q = &ir->compact_instructions[def];

  if (q->op == TCCIR_OP_ZEXT)
  {
    /* Verify the ZEXT's source vreg is 32-bit (not a wider value being
     * truncated-and-zero-extended in one step).  A genuine 32->64 ZEXT
     * means the source operand width is 32 bits. */
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (irop_is_64bit(s1))
      return 0;
    int s_vr = irop_get_vreg(s1);
    if (s_vr < 0)
      return 0;
    *out_low_vr = s_vr;
    return 1;
  }

  if (q->op == TCCIR_OP_OR)
  {
    IROperand or_s1 = tcc_ir_op_get_src1(ir, q);
    IROperand or_s2 = tcc_ir_op_get_src2(ir, q);
    int or_s1_vr = irop_get_vreg(or_s1);
    int or_s2_vr = irop_get_vreg(or_s2);
    if (or_s1_vr < 0 || or_s2_vr < 0)
      return 0;
    /* Try each operand as the "shifted high" side; the other is the low. */
    for (int swap = 0; swap < 2; ++swap)
    {
      int shifted_vr = swap ? or_s2_vr : or_s1_vr;
      int low_vr = swap ? or_s1_vr : or_s2_vr;
      int shl_def = tcc_ir_find_defining_instruction(ir, shifted_vr, def);
      if (shl_def < 0)
        continue;
      IRQuadCompact *shl_q = &ir->compact_instructions[shl_def];
      if (shl_q->op != TCCIR_OP_SHL)
        continue;
      IROperand shl_s2 = tcc_ir_op_get_src2(ir, shl_q);
      if (!irop_is_immediate(shl_s2))
        continue;
      if ((int64_t)irop_get_imm64_ex(ir, shl_s2) != 32)
        continue;
      IROperand shl_s1 = tcc_ir_op_get_src1(ir, shl_q);
      int high_vr = irop_get_vreg(shl_s1);
      if (high_vr < 0)
        continue;
      int sar_def = tcc_ir_find_defining_instruction(ir, high_vr, shl_def);
      if (sar_def < 0)
        continue;
      IRQuadCompact *sar_q = &ir->compact_instructions[sar_def];
      if (sar_q->op != TCCIR_OP_SAR)
        continue;
      IROperand sar_s2 = tcc_ir_op_get_src2(ir, sar_q);
      if (!irop_is_immediate(sar_s2))
        continue;
      if ((int64_t)irop_get_imm64_ex(ir, sar_s2) != 31)
        continue;
      IROperand sar_s1 = tcc_ir_op_get_src1(ir, sar_q);
      int sar_src_vr = irop_get_vreg(sar_s1);
      if (sar_src_vr < 0)
        continue;
      /* The SAR's source must be exactly the OR's low operand. */
      if (sar_src_vr != low_vr)
        continue;
      /* Both must be 32-bit. */
      if (irop_is_64bit(sar_s1) || irop_is_64bit(swap ? or_s1 : or_s2))
        continue;
      *out_low_vr = low_vr;
      return 2;
    }
  }

  return 0;
}

/* If both 64-bit operands on the vstack are 32->64 extensions, emit a single
 * SMULL/UMULL (32x32->64) and replace the two operands with the 64-bit result.
 * Returns 1 if it handled the multiply, 0 to fall back to the generic 64x64
 * expansion in gen_opl '*'. */
static int try_emit_widening_mul64(int t)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir)
    return 0;
  /* Need IR-tracked vregs for both operands. */
  if (vtop->vr < 0 || vtop[-1].vr < 0)
    return 0;
  /* Constants don't have a defining IR op; skip them. */
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    return 0;
  if ((vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    return 0;
  /* Both operands must be 64-bit. */
  if ((vtop->type.t & VT_BTYPE) != VT_LLONG)
    return 0;
  if ((vtop[-1].type.t & VT_BTYPE) != VT_LLONG)
    return 0;

  int low1 = -1, low2 = -1;
  int p1 = detect_ll_ext_provenance(vtop[-1].vr, &low1);
  int p2 = detect_ll_ext_provenance(vtop->vr, &low2);
  if (p1 == 0 || p2 == 0)
    return 0;
  if (low1 < 0 || low2 < 0)
    return 0;
  /* Mixing signedness is unsafe — only handle pure unsigned×unsigned or
   * pure signed×signed.  (If the source operands' actual signedness differs
   * from how they were extended, the multiply still matches the bit pattern
   * required by the C standard for two same-class operands.) */
  if (p1 != p2)
    return 0;

  int tok = (p1 == 1) ? TOK_UMULL : TOK_SMULL;
  int dest_t = VT_LLONG | ((p1 == 1) ? VT_UNSIGNED : 0);

  /* Replace each operand on the stack with a 32-bit SValue pointing at the
   * extension source vreg.  Then emit a single 32x32->64 multiply. */
  int u_lo = (p1 == 1) ? VT_UNSIGNED : 0;
  vtop[-1].vr = low1;
  vtop[-1].type.t = VT_INT | u_lo;
  vtop[-1].r = 0;
  vtop[-1].c.i = 0;

  vtop[0].vr = low2;
  vtop[0].type.t = VT_INT | u_lo;
  vtop[0].r = 0;
  vtop[0].c.i = 0;

  gen_op(tok);
  /* gen_op leaves the 64-bit product on the stack; ensure type reflects that. */
  vtop->type.t = dest_t;
  (void)t;
  return 1;
}

/* generate CPU independent (unsigned) long long operations */
void gen_opl(int op)
{
  int t, op1, c, i;
  int func;
  unsigned short reg_iret = REG_IRET;
  SValue tmp;

  switch (op)
  {
  case '/':
  case TOK_PDIV:
    func = TOK___divdi3;
    goto gen_func;
  case TOK_UDIV:
    func = TOK___udivdi3;
    goto gen_func;
  case '%':
    func = TOK___moddi3;
    goto gen_mod_func;
  case TOK_UMOD:
    func = TOK___umoddi3;
  gen_mod_func:
#ifdef TCC_ARM_EABI
    reg_iret = TREG_R2;
#endif
  gen_func:
    /* call generic long long function */
    vpush_helper_func(func);
    vrott(3);
    /* Stack after vrott(3): func, arg1, arg2 (arg2 is at vtop) */
    {
      SValue param_num;
      SValue dest;
      const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
      svalue_init(&param_num);
      param_num.vr = -1;
      param_num.r = VT_CONST;
      /* Generate FUNCPARAMVAL for arg1 (param 0) */
      param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
      LOG_CODEGEN("FUNCPARAMVAL push: site=llong_helper call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                  TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[-1].r, vtop[-1].vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &param_num, NULL);
      /* Generate FUNCPARAMVAL for arg2 (param 1) */
      param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
      LOG_CODEGEN("FUNCPARAMVAL push: site=llong_helper call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                  TCCIR_DECODE_PARAM_IDX((uint32_t)param_num.c.i), vtop[0].r, vtop[0].vr);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[0], &param_num, NULL);
      /* Generate FUNCCALLVAL for the function call (returns long long) */
      svalue_init(&dest);
      dest.type.t = VT_LLONG;
      dest.r = 0;
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 2);
      tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[-2], &call_id_sv, &dest);
      /* Pop all 3 values (arg1, arg2, func) and push result */
      vtop -= 3;
      vpushi(0);
      vtop->type.t = VT_LLONG;
      vtop->vr = dest.vr;
      vtop->r = reg_iret;
    }
    break;
  case '^':
  case '&':
  case '|':
  case '+':
  case '-':
    /* For IR mode: generate 64-bit operations directly without lexpand/lbuild */
    if (tcc_state->ir)
    {
      t = vtop->type.t;
      int dest_type = VT_LLONG | (t & VT_UNSIGNED);
      if (op == '+' || op == '-')
      {
        /* 64-bit add/sub - generate single IR operation */
        SValue dest;
        svalue_init(&dest);
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        dest.type.t = dest_type;
        dest.r = 0;
        if ((dest_type & VT_BTYPE) == VT_LLONG)
          tcc_ir_set_llong_type(tcc_state->ir, dest.vr);
        TccIrOp ir_op = (op == '+') ? TCCIR_OP_ADD : TCCIR_OP_SUB;
        tcc_ir_put(tcc_state->ir, ir_op, &vtop[-1], &vtop[0], &dest);
        vtop--;
        vtop->vr = dest.vr;
        vtop->type.t = dest_type;
        vtop->r = 0;
      }
      else
      {
        /* 64-bit bitwise ops (^, &, |) - generate single IR operation */
        SValue dest;
        svalue_init(&dest);
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        dest.type.t = dest_type;
        dest.r = 0;
        if ((dest_type & VT_BTYPE) == VT_LLONG)
          tcc_ir_set_llong_type(tcc_state->ir, dest.vr);
        TccIrOp ir_op = TCCIR_OP_NOP;
        switch (op)
        {
        case '^':
          ir_op = TCCIR_OP_XOR;
          break;
        case '&':
          ir_op = TCCIR_OP_AND;
          break;
        case '|':
          ir_op = TCCIR_OP_OR;
          break;
        }
        tcc_ir_put(tcc_state->ir, ir_op, &vtop[-1], &vtop[0], &dest);
        vtop--;
        vtop->vr = dest.vr;
        vtop->type.t = dest_type;
        vtop->r = 0;
      }
      break;
    }
    /* Fall through for non-IR mode */
    /* FALLTHROUGH */
  case '*':
    t = vtop->type.t; /* Save type for lbuild at end */
    /* Speculative / code-suppressed contexts (try_inline_const_eval, if(0)
     * dead branches, constant-expression and data-only evaluation) run with
     * nocode_wanted set, where tcc_ir_put is a no-op (see ir/core.c) and gv()
     * is suppressed.  The generic 64x64 lexpand/lbuild expansion below assumes
     * real register codegen and walks vtop off the vstack into the heap in
     * that state.  No code is emitted here, so just collapse the two operands
     * into a single 64-bit result, mirroring the +/-/&/|/^ IR paths above.
     * (CODE_OFF_BIT-only dead code after return still needs real IR for
     * backpatching, so exclude it — same predicate tcc_ir_put uses.) */
    if (nocode_wanted & ~CODE_OFF_BIT)
    {
      vtop--;
      vtop->type.t = VT_LLONG | (t & VT_UNSIGNED);
      vtop->r = 0;
      if (tcc_state->ir)
      {
        vtop->vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        tcc_ir_set_llong_type(tcc_state->ir, vtop->vr);
      }
      else
        vtop->vr = -1;
      break;
    }
    /* Widening-multiply peephole: when both 64-bit operands are 32->64
     * extensions (zero or sign), emit a single 32x32->64 UMULL/SMULL
     * instead of the generic 64x64 expansion. */
    if (op == '*' && tcc_state->ir && try_emit_widening_mul64(t))
      break;
    vswap();
    lexpand();
    vrotb(3);
    lexpand();
    /* stack: L1 H1 L2 H2 */
    tmp = vtop[0];
    vtop[0] = vtop[-3];
    vtop[-3] = tmp;
    tmp = vtop[-2];
    vtop[-2] = vtop[-3];
    vtop[-3] = tmp;
    vswap();
    /* stack: H1 H2 L1 L2 */
    // pv("gen_opl B", 0, 4);
    if (op == '*')
    {
      vpushv(vtop - 1);
      vpushv(vtop - 1);
      gen_op(TOK_UMULL);
      lexpand();
      /* stack: H1 H2 L1 L2 ML MH */
      for (i = 0; i < 4; i++)
        vrotb(6);
      /* stack: ML MH H1 H2 L1 L2 */
      tmp = vtop[0];
      vtop[0] = vtop[-2];
      vtop[-2] = tmp;
      /* stack: ML MH H1 L2 H2 L1 */
      gen_op('*');
      vrotb(3);
      vrotb(3);
      gen_op('*');
      /* stack: ML MH M1 M2 */
      gen_op('+');
      gen_op('+');
    }
    else if (op == '+' || op == '-')
    {
      /* XXX: add non carry method too (for MIPS or alpha) */
      if (op == '+')
        op1 = TOK_ADDC1;
      else
        op1 = TOK_SUBC1;
      gen_op(op1);
      /* stack: H1 H2 (L1 op L2) */
      vrotb(3);
      vrotb(3);
      gen_op(op1 + 1); /* TOK_xxxC2 */
    }
    else
    {
      gen_op(op);
      /* stack: H1 H2 (L1 op L2) */
      vrotb(3);
      vrotb(3);
      /* stack: (L1 op L2) H1 H2 */
      gen_op(op);
      /* stack: (L1 op L2) (H1 op H2) */
    }
    /* stack: L H */
    lbuild(t);
    break;
  case TOK_SAR:
  case TOK_SHR:
  case TOK_SHL:
    if (tcc_state->ir && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    {
      /* IR mode: generate a single 64-bit shift instruction directly.
       * The lexpand/lbuild decomposition produces intermediate 32-bit values
       * that lbuild then recombines via SHL-by-32 + OR, but that inner SHL
       * has a 32-bit source operand causing incorrect codegen on ARM Thumb
       * (32-bit LSL by 32 produces zero).  Emitting the shift as a native
       * 64-bit IR op lets the backend handle it correctly. */
      t = vtop[-1].type.t;
      c = (int)vtop->c.i;
      int dest_type = VT_LLONG | (t & VT_UNSIGNED);
      TccIrOp ir_op;
      switch (op)
      {
      case TOK_SHL:
        ir_op = TCCIR_OP_SHL;
        break;
      case TOK_SHR:
        ir_op = TCCIR_OP_SHR;
        break;
      default: /* TOK_SAR */
        ir_op = TCCIR_OP_SAR;
        break;
      }
      SValue dest;
      svalue_init(&dest);
      dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
      dest.type.t = dest_type;
      dest.r = 0;
      if ((dest_type & VT_BTYPE) == VT_LLONG)
        tcc_ir_set_llong_type(tcc_state->ir, dest.vr);
      tcc_ir_put(tcc_state->ir, ir_op, &vtop[-1], &vtop[0], &dest);
      vtop--;
      vtop->vr = dest.vr;
      vtop->type.t = dest_type;
      vtop->r = 0;
    }
    else if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    {
      t = vtop[-1].type.t;
      vswap();
      lexpand();
      vrotb(3);
      /* stack: L H shift */
      c = (int)vtop->c.i;
      /* constant: simpler */
      /* NOTE: all comments are for SHL. the other cases are
         done by swapping words */
      vpop();
      if (op != TOK_SHL)
        vswap();
      if (c >= 32)
      {
        /* stack: L H */
        vpop();
        if (c > 32)
        {
          vpushi(c - 32);
          gen_op(op);
        }
        if (op != TOK_SAR)
        {
          vpushi(0);
        }
        else
        {
          gv_dup();
          vpushi(31);
          gen_op(TOK_SAR);
        }
        vswap();
      }
      else
      {
        vswap();
        gv_dup();
        /* stack: H L L */
        vpushi(c);
        gen_op(op);
        vswap();
        vpushi(32 - c);
        if (op == TOK_SHL)
          gen_op(TOK_SHR);
        else
          gen_op(TOK_SHL);
        vrotb(3);
        /* stack: L L H */
        vpushi(c);
        if (op == TOK_SHL)
          gen_op(TOK_SHL);
        else
          gen_op(TOK_SHR);
        gen_op('|');
      }
      if (op != TOK_SHL)
        vswap();
      lbuild(t);
    }
    else
    {
      /* XXX: should provide a faster fallback on x86 ? */
      switch (op)
      {
      case TOK_SAR:
        func = TOK___ashrdi3;
        goto gen_func;
      case TOK_SHR:
        func = TOK___lshrdi3;
        goto gen_func;
      case TOK_SHL:
        func = TOK___ashldi3;
        goto gen_func;
      }
    }
    break;
  default:
    /* 64-bit compare operations */
    t = vtop->type.t;
    if (tcc_state->ir)
    {
      /* Inline 64-bit comparison via CMP+SBCS.
       *
       * CMP+SBCS correctly sets N/V/C flags for the full 64-bit comparison,
       * so LT, GE, ULT, UGE conditions work directly.
       *
       * GT, LE, UGT, ULE also depend on the Z flag, which SBCS sets only
       * for the high-word result (not the full 64-bit equality).  We handle
       * these by swapping operands: GT(a,b)=LT(b,a), LE(a,b)=GE(b,a), etc.
       *
       * EQ/NE: decompose into (a ^ b) then test if the 64-bit result is 0
       * by splitting into lo|hi and doing a 32-bit comparison.
       */
      int cmp_op = op;
      if (op == TOK_EQ || op == TOK_NE)
      {
        /* EQ/NE: emit 64-bit CMP directly.
         * The backend emits CMP hi,hi; IT EQ; CMPEQ lo,lo
         * which correctly sets Z for full 64-bit equality. */
        tcc_ir_put(tcc_state->ir, TCCIR_OP_CMP, &vtop[-1], &vtop[0], NULL);
        vtop--;
        vtop->r = VT_CMP;
        vtop->cmp_op = op;
        vtop->jfalse = -1;
        vtop->jtrue = -1;
      }
      else
      {
        if (op == TOK_GT || op == TOK_LE || op == TOK_UGT || op == TOK_ULE)
        {
          vswap();
          switch (op)
          {
          case TOK_GT:
            cmp_op = TOK_LT;
            break;
          case TOK_LE:
            cmp_op = TOK_GE;
            break;
          case TOK_UGT:
            cmp_op = TOK_ULT;
            break;
          case TOK_ULE:
            cmp_op = TOK_UGE;
            break;
          }
        }
        tcc_ir_put(tcc_state->ir, TCCIR_OP_CMP, &vtop[-1], &vtop[0], NULL);
        vtop--;
        vtop->r = VT_CMP;
        vtop->cmp_op = cmp_op;
        vtop->jfalse = -1;
        vtop->jtrue = -1;
      }
      /* Materialize VT_CMP immediately so the SETIF IR instruction is
       * emitted right after the CMP. Without this, the SETIF would be
       * deferred until the value is consumed, and a subsequent
       * comparison would clobber the ARM flags register. */
      if ((vtop->r & VT_VALMASK) == VT_CMP)
      {
        gv(RC_INT);
      }
    }
    else
    {
      int is_unsigned = (op == TOK_ULT || op == TOK_ULE || op == TOK_UGT || op == TOK_UGE);
      func = is_unsigned ? TOK___aeabi_ulcmp : TOK___aeabi_lcmp;
      vpush_helper_func(func);
      vrott(3);
      {
        SValue param_num;
        SValue dest;
        const int call_id = 0;
        svalue_init(&param_num);
        param_num.vr = -1;
        param_num.r = VT_CONST;
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &param_num, NULL);
        param_num.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[0], &param_num, NULL);
        svalue_init(&dest);
        dest.type.t = VT_INT;
        dest.r = 0;
        dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
        SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 2);
        tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[-2], &call_id_sv, &dest);
        vtop -= 3;
        vpushi(0);
        vtop->type.t = VT_INT;
        vtop->vr = dest.vr;
        vtop->r = REG_IRET;
      }
      vpushi(0);
      switch (op)
      {
      case TOK_LT:
      case TOK_ULT:
        gen_op(TOK_LT);
        break;
      case TOK_LE:
      case TOK_ULE:
        gen_op(TOK_LE);
        break;
      case TOK_GT:
      case TOK_UGT:
        gen_op(TOK_GT);
        break;
      case TOK_GE:
      case TOK_UGE:
        gen_op(TOK_GE);
        break;
      case TOK_EQ:
        gen_op(TOK_EQ);
        break;
      case TOK_NE:
        gen_op(TOK_NE);
        break;
      }
      if ((vtop->r & VT_VALMASK) == VT_CMP)
      {
        gv(RC_INT);
      }
    }
    break;
  }
}
#endif
