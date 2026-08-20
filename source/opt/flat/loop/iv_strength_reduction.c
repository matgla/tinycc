/*
 *  TCC IR - Induction variable strength reduction
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


/* Taint-track the derived-IV address value; return 1 iff it never escapes to memory (docs/bugs.md #2, va-arg-24). */
#define SR_TAINT_MAX 64
static int sr_div_value_stays_in_regs(TCCIRState *ir, int lo, int hi, int32_t seed_vr)
{
  int32_t taint[SR_TAINT_MAX];
  int nt = 0;
  taint[nt++] = seed_vr;

  int changed = 1;
  while (changed)
  {
    changed = 0;
    for (int j = lo; j <= hi; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* STORE dest slot holds the write address; MLA accum lives at pool +3, invisible to src1/src2 (ptr-6869). */
      IROperand reads[4];
      int slots[4]; /* 1 = src1, 2 = src2, 3 = dest, 4 = accum */
      int nreads = 0;
      int dest_is_read = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                          q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_FUNCPARAMVAL);
      if (irop_config[q->op].has_src1)
      {
        slots[nreads] = 1;
        reads[nreads++] = tcc_ir_op_get_src1(ir, q);
      }
      if (irop_config[q->op].has_src2)
      {
        slots[nreads] = 2;
        reads[nreads++] = tcc_ir_op_get_src2(ir, q);
      }
      if (dest_is_read && irop_config[q->op].has_dest)
      {
        slots[nreads] = 3;
        reads[nreads++] = tcc_ir_op_get_dest(ir, q);
      }
      if (q->op == TCCIR_OP_MLA)
      {
        slots[nreads] = 4;
        reads[nreads++] = tcc_ir_op_get_accum(ir, q);
      }

      int reads_taint = 0;
      for (int r = 0; r < nreads; r++)
      {
        int32_t rv = irop_get_vreg(reads[r]);
        if (rv < 0)
          continue;
        int t = 0;
        for (int k = 0; k < nt; k++)
        {
          if (taint[k] == rv)
          {
            t = 1;
            break;
          }
        }
        if (!t)
          continue;
        /* lval read = deref of the tainted value, except the plain "fetch VAR" form (VAR vreg + is_local).
         *
         * ...and except when the deref IS the memory access: the tainted value
         * sits in the ADDRESS slot of a plain load/store.  That is the whole
         * point of an address derived from an IV (`a[i]`), and the rewrite
         * preserves it exactly -- transform_derived_iv turns the defining ADD
         * into `T = ptr`, and ptr holds the identical value (base + i*stride)
         * at that point in the iteration.  What this scan must still catch is
         * the address being PUBLISHED (stored to memory, passed to a call,
         * used to index something else); every one of those reads the taint
         * from a non-address slot and still falls through to the op-kind check
         * below.  Refusing the deref outright was the most common rejection
         * across the benchmark sources (`escape scan ... feeds_mem=1`). */
        if (reads[r].is_lval &&
            !(TCCIR_DECODE_VREG_TYPE(rv) == TCCIR_VREG_TYPE_VAR && reads[r].is_local))
        {
          int addr_slot =
              ((q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_LOAD_INDEXED) && slots[r] == 1) ||
              ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) && slots[r] == 3);
          if (!addr_slot)
            return 0;
          /* The address is consumed here; the loaded/stored VALUE is memory
           * contents, not the pointer, so nothing propagates and this op must
           * not be judged by the ALU-only rule below. */
          continue;
        }
        reads_taint = 1;
      }
      if (!reads_taint)
        continue;

      /* Tainted value consumed here — allow only plain ALU/copy/compare. */
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB &&
          q->op != TCCIR_OP_CMP)
        return 0;

      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (d.is_lval)
          return 0; /* store through an lval dest — memory write */
        int32_t dv = irop_get_vreg(d);
        if (dv >= 0)
        {
          int already = 0;
          for (int k = 0; k < nt; k++)
          {
            if (taint[k] == dv)
            {
              already = 1;
              break;
            }
          }
          if (!already)
          {
            if (nt >= SR_TAINT_MAX)
              return 0; /* capacity — be conservative */
            taint[nt++] = dv;
            changed = 1;
          }
        }
      }
    }
  }
  return 1;
}

int transform_derived_iv(TCCIRState *ir, IRLoop *loop, InductionVar *iv, DerivedIV *div, int *out_ptr_vreg,
                                int *out_idx_shift, int *out_postnop_origpos, int *out_stride_pos, int shared_ptr_vreg)
{
  if (out_ptr_vreg)
    *out_ptr_vreg = -1;
  if (out_idx_shift)
    *out_idx_shift = 0;
  if (out_postnop_origpos)
    *out_postnop_origpos = -1;
  if (out_stride_pos)
    *out_stride_pos = -1;

  /* Kill-switch for bisection: TCC_DISABLE_PASS=derived_iv (docs/bugs.md #2). */
  if (tcc_ir_opt_pass_disabled("derived_iv"))
    return 0;

  /* Shared-pointer rewrites unsupported (no escape analysis for the duplicate's use site); defer to re-detection (docs/bugs.md #2). */
  if (shared_ptr_vreg >= 0)
    return 0;

  /* Bail if the DIV's address value can reach a memory access or escape the register domain; scan the full body range including the detached body_instrs (va-arg-24). */
  if (div->use_idx >= 0 && div->use_idx < ir->next_instruction_index)
  {
    int uop = ir->compact_instructions[div->use_idx].op;
    /* An INDEXED use consumes the derived address as an ADDRESSING MODE, so no
     * address value is produced and nothing can escape -- it is the safest
     * shape, not the most dangerous one.  (The escape scan below would track
     * a LOAD_INDEXED's dest, which is the loaded VALUE, not an address.)
     *
     * This path used to be turned off here and one gate above, on the premise
     * that "the backend already forms efficient indexed addressing".  Measured
     * on the RP2350 Cortex-M33 with hand-written asm probes over a 256-word
     * walk (cycles per element, same body otherwise):
     *
     *   ldr.w r3,[base,i,lsl #2] + adds i + cmp + blt   5 instr   8.012
     *   ldr   r3,[p]  + adds p,#4 + cmp p,end + bne     5 instr   7.012
     *   ldr   r3,[p],#4           + cmp p,end + bne     4 instr   6.012
     *
     * A scaled register offset costs a full extra cycle over a plain base
     * register at equal instruction count, so the indexed form is the SLOWEST
     * of the three, not the most efficient.  Strength-reducing it to the
     * pointer walk (which is what this pass then does, including replacing the
     * loop test with a pointer compare) is the 7.012 row; that is also exactly
     * the shape gcc emits for these loops. */
    int feeds_mem = 0;
    if (uop != TCCIR_OP_STORE_INDEXED && uop != TCCIR_OP_LOAD_INDEXED)
    {
      IROperand ud = tcc_ir_op_get_dest(ir, &ir->compact_instructions[div->use_idx]);
      int32_t ud_vr = irop_get_vreg(ud);
      if (ud_vr >= 0)
      {
        int lo = loop->start_idx >= 0 ? loop->start_idx : 0;
        int hi = loop->end_idx;
        if (loop->num_body_instrs > 0)
        {
          int b_first = loop->body_instrs[0];
          int b_last = loop->body_instrs[loop->num_body_instrs - 1];
          if (b_first >= 0 && b_first < lo)
            lo = b_first;
          if (b_last > hi)
            hi = b_last;
        }
        if (hi >= ir->next_instruction_index)
          hi = ir->next_instruction_index - 1;
        feeds_mem = !sr_div_value_stays_in_regs(ir, lo, hi, ud_vr);
        LOG_IV_SR("IV_SR: escape scan [%d..%d] for DIV at use_idx=%d: feeds_mem=%d", lo, hi, div->use_idx, feeds_mem);
      }
    }
    if (feeds_mem)
      return 0;
  }

  int ptr_vreg = tcc_ir_vreg_alloc_temp(ir);
  if (ptr_vreg < 0)
    return 0;

  LOG_IV_SR("IV_SR: Transforming DIV at idx=%d, new ptr vreg=TMP%d, iv_init=%d, stride=%d", div->use_idx,
            TCCIR_DECODE_VREG_POSITION(ptr_vreg), iv->init_val, div->stride);

  /* Step 1: insert ptr = base + iv_init*stride once, at preheader_idx+1 (after preheader, before header). */
  if (loop->preheader_idx < 0)
    return 0;
  int insert_pos = loop->preheader_idx + 1;

  /* Don't split a CMP→JUMPIF pair: an inserted ADD (init_offset != 0) would clobber the flags. */
  {
    int element_size_check = div->stride / iv->step;
    int init_offset_check = iv->init_val * element_size_check;
    if (init_offset_check != 0 && insert_pos > 0 && ir->compact_instructions[insert_pos - 1].op == TCCIR_OP_CMP &&
        insert_pos < ir->next_instruction_index && ir->compact_instructions[insert_pos].op == TCCIR_OP_JUMPIF)
    {
      LOG_IV_SR("IV_SR: Skipping DIV transform — would split CMP→JUMPIF at %d→%d", insert_pos - 1, insert_pos);
      return 0;
    }
  }

  LOG_IV_SR("IV_SR: transform_derived_iv: header_idx=%d, preheader_idx=%d, start_idx=%d, end_idx=%d, insert_pos=%d",
            loop->header_idx, loop->preheader_idx, loop->start_idx, loop->end_idx, insert_pos);

  /* Verify base vreg is defined before insert_pos (LICM can hoist a base def after the header → use-before-def). */
  {
    int32_t base_vr = irop_get_vreg(div->base_op);
    /* A PARAM is live-in: it has no defining quad anywhere in the function, so
     * the scan below can never find one and every `f(const int *a) { ... a[i]
     * ... }` loop -- the single most common array-walk shape there is -- was
     * rejected here.  The guard exists to catch a base whose def LICM hoisted
     * to *after* the header; an incoming argument cannot have that problem. */
    if (base_vr >= 0 && TCCIR_DECODE_VREG_TYPE(base_vr) != TCCIR_VREG_TYPE_PARAM)
    {
      int def_found_before = 0;
      for (int i = 0; i < insert_pos; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (irop_config[q->op].has_dest)
        {
          IROperand qd = tcc_ir_op_get_dest(ir, q);
          if (irop_get_vreg(qd) == base_vr)
          {
            def_found_before = 1;
            break;
          }
        }
      }
      if (!def_found_before)
      {
        LOG_IV_SR("IV_SR: Skipping DIV transform — base vreg not defined before insert_pos %d", insert_pos);
        return 0;
      }

      /* Also require base loop-invariant: not redefined in the body, else the reduced pointer diverges. */
      for (int i = loop->start_idx; i <= loop->end_idx; i++)
      {
        IRQuadCompact *lq = &ir->compact_instructions[i];
        if (lq->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[lq->op].has_dest)
        {
          IROperand ld = tcc_ir_op_get_dest(ir, lq);
          if (irop_get_vreg(ld) == base_vr)
          {
            LOG_IV_SR("IV_SR: Skipping DIV transform — base vreg redefined inside loop at idx %d", i);
            return 0;
          }
        }
      }
    }
  }

  IROperand ptr_op = irop_make_vreg(ptr_vreg, IROP_BTYPE_INT32);
  IROperand null_op = {0};

  int idx_shift = 0;

  /* element_size = stride/step; init_offset = init_val*element_size (NOT init_val*stride when step != 1). */
  int element_size = div->stride / iv->step;
  int init_offset = iv->init_val * element_size;

  if (init_offset == 0)
  {
    /* Simple case: ptr = base */
    int inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ASSIGN, ptr_op, div->base_op, null_op);
    LOG_IV_SR("IV_SR: init insert at pos=%d, result=%d (base_vr=%d)", insert_pos, inserted,
              irop_get_vreg(div->base_op));
    if (inserted < 0)
      return 0;
    idx_shift = 1;
  }
  else
  {
    /* ptr = base; ptr += init_offset */
    int inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ASSIGN, ptr_op, div->base_op, null_op);
    LOG_IV_SR("IV_SR: init insert at pos=%d, result=%d", insert_pos, inserted);
    if (inserted < 0)
      return 0;
    idx_shift = 1;

    IROperand offset_op = irop_make_imm32(-1, init_offset, IROP_BTYPE_INT32);
    inserted = insert_instr_at(ir, insert_pos + 1, TCCIR_OP_ADD, ptr_op, ptr_op, offset_op);
    if (inserted < 0)
      return 1; /* Partial - at least did the assignment */
    idx_shift = 2;
  }

  /* Insertion shifted all indices >= insert_pos; update our tracked indices. */
  int new_use_idx = div->use_idx + idx_shift;
  int new_shl_idx = (div->shl_idx >= 0) ? div->shl_idx + idx_shift : -1;
  int new_iv_def_idx = iv->def_idx;
  if (iv->def_idx >= insert_pos)
    new_iv_def_idx += idx_shift;

  /* Step 2: rewrite the use site to consume ptr — ADD/MLA→ASSIGN, LOAD_INDEXED→LOAD, STORE_INDEXED→STORE. */
  IRQuadCompact *add_q = &ir->compact_instructions[new_use_idx];
  int rewrote_to_load_or_store = 0;
  if (add_q->op == TCCIR_OP_LOAD_INDEXED)
  {
    IROperand ptr_lval = ptr_op;
    ptr_lval.is_lval = 1;
    add_q->op = TCCIR_OP_LOAD;
    tcc_ir_op_set_src1(ir, add_q, ptr_lval);
    rewrote_to_load_or_store = 1;
    /* dest (loaded value temp) is preserved at slot 0. */
  }
  else if (add_q->op == TCCIR_OP_STORE_INDEXED)
  {
    IROperand ptr_lval = ptr_op;
    ptr_lval.is_lval = 1;
    add_q->op = TCCIR_OP_STORE;
    tcc_ir_op_set_dest(ir, add_q, ptr_lval);
    rewrote_to_load_or_store = 1;
    /* src1 (value to store) is preserved at slot 1. */
  }
  else
  {
    add_q->op = TCCIR_OP_ASSIGN;
    tcc_ir_op_set_src1(ir, add_q, ptr_op);
    tcc_ir_op_set_src2(ir, add_q, null_op);
    /* dest stays (the address temp); an MLA's accum at +3 is now orphaned. */
  }

  /* Step 2b (INDEXED-DIV): insert a vestigial NOP after the LOAD/STORE; report its pos via out_postnop_origpos for the caller's APPLY_SHIFT. */
  int postnop_inserted = 0;
  if (rewrote_to_load_or_store)
  {
    int nop_pos = new_use_idx + 1;
    int inserted_nop = insert_instr_at(ir, nop_pos, TCCIR_OP_NOP, null_op, null_op, null_op);
    if (inserted_nop >= 0)
    {
      /* Update local indices: anything strictly after new_use_idx shifts by 1. */
      postnop_inserted = 1;
      if (new_iv_def_idx > new_use_idx)
        new_iv_def_idx++;
      if (out_postnop_origpos)
        *out_postnop_origpos = div->use_idx;
    }
  }

  /* Step 3: NOP the SHL/MUL (skipped for fused MLA — no separate multiply instr). */
  if (new_shl_idx >= 0)
  {
    IRQuadCompact *shl_q = &ir->compact_instructions[new_shl_idx];
    shl_q->op = TCCIR_OP_NOP;
  }

  /* Step 4: insert ptr += stride after the IV increment, pushed past all uses of the derived address (copy-prop/coalescing can merge ptr with the address temp). */
  int stride_insert_pos = new_iv_def_idx + 1;
  if (new_use_idx > new_iv_def_idx)
  {
    int safe_to_push = 1;
    for (int si = new_iv_def_idx + 1; si <= new_use_idx; si++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[si];
      if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF || sq->op == TCCIR_OP_FUNCCALLVAL ||
          sq->op == TCCIR_OP_FUNCCALLVOID)
      {
        safe_to_push = 0;
        break;
      }
    }
    if (safe_to_push)
    {
      /* Stride must land after the last deref through this pointer. */
      IROperand use_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[new_use_idx]);
      int32_t use_dest_vr = irop_get_vreg(use_dest);
      int last_use = new_use_idx;
      if (use_dest_vr >= 0)
      {
        /* Extend the scan window by idx_shift + postnop so it doesn't stop short of a trailing FUNCCALL. */
        int loop_end = loop->end_idx + idx_shift + (postnop_inserted ? 1 : 0);
        int saw_param_use = 0;
        for (int si = new_use_idx + 1; si <= loop_end; si++)
        {
          IRQuadCompact *sq = &ir->compact_instructions[si];
          if (sq->op == TCCIR_OP_NOP)
            continue;
          if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF)
            break;
          if (sq->op == TCCIR_OP_FUNCCALLVAL || sq->op == TCCIR_OP_FUNCCALLVOID)
          {
            /* Params materialize at the call: if one passed our address, push the stride past the CALL. */
            if (saw_param_use)
              last_use = si;
            break;
          }
          int uses_it = 0;
          int defines_it = 0;
          if (irop_config[sq->op].has_dest)
          {
            IROperand d = tcc_ir_op_get_dest(ir, sq);
            if (irop_get_vreg(d) == use_dest_vr)
            {
              if (sq->op == TCCIR_OP_STORE || sq->op == TCCIR_OP_STORE_INDEXED ||
                  sq->op == TCCIR_OP_STORE_POSTINC)
                uses_it = 1;
              else
                defines_it = 1;
            }
          }
          if (irop_config[sq->op].has_src1)
          {
            IROperand s1 = tcc_ir_op_get_src1(ir, sq);
            if (irop_get_vreg(s1) == use_dest_vr)
              uses_it = 1;
          }
          if (irop_config[sq->op].has_src2)
          {
            IROperand s2 = tcc_ir_op_get_src2(ir, sq);
            if (irop_get_vreg(s2) == use_dest_vr)
              uses_it = 1;
          }
          if (defines_it && !uses_it)
            break;
          if (uses_it)
          {
            last_use = si;
            if (sq->op == TCCIR_OP_FUNCPARAMVAL)
              saw_param_use = 1;
          }
        }
      }
      stride_insert_pos = last_use + 1;
    }
  }
  IROperand stride_op = irop_make_imm32(-1, div->stride, IROP_BTYPE_INT32);

  int stride_inserted = insert_instr_at(ir, stride_insert_pos, TCCIR_OP_ADD, ptr_op, ptr_op, stride_op);
  LOG_IV_SR("IV_SR: stride insert at pos=%d, result=%d, new_iv_def=%d", stride_insert_pos, stride_inserted,
            new_iv_def_idx);
  if (stride_inserted < 0)
    return 2; /* Partial success - at least did the pointer init and use replacement */

  /* Report the ACTUAL stride insertion position so the caller's APPLY_SHIFT
   * bookkeeping shifts later indices by the real insertion point.  This is in
   * post-(init+postnop) index space — exactly the space the caller's index is
   * in after it applies the init/postnop shifts.  For post-increment / param-use
   * loops the stride is pushed PAST the FUNCCALL (stride_insert_pos can be far
   * beyond iv->def_idx+1), and assuming def_idx+1 over-shifts every index
   * between the def and the call, corrupting a second derived IV's use_idx. */
  if (out_stride_pos)
    *out_stride_pos = stride_insert_pos;

  if (out_ptr_vreg)
    *out_ptr_vreg = ptr_vreg;
  if (out_idx_shift)
    *out_idx_shift = idx_shift;

  return 3; /* Full success: init + replace + stride */
}

/* Try to eliminate the original IV counter after strength reduction has created
 * a derived pointer.  If the IV's only remaining uses are its own increment
 * and the loop exit CMP, we can replace the CMP with a pointer comparison
 * against a precomputed end address, making the IV completely dead.
 *
 * Before:  CMP i, #5; JUMPIF >=S exit   (signed comparison of index)
 *          i = i + 1
 *          ptr = ptr + 4
 *
 * After:   CMP ptr, end_ptr; JUMPIF >=U exit   (unsigned pointer comparison)
 *          ptr = ptr + 4
 *          (i is dead, eliminated by DCE)
 *
 * Parameters:
 *   ir       - IR state
 *   loop     - Loop structure (indices already shifted by transform_derived_iv)
 *   iv       - The basic induction variable (indices already shifted)
 *   div      - The derived IV info
 *   ptr_vreg - The vreg allocated for the pointer by transform_derived_iv
 *   idx_shift - Number of instructions inserted at the header by transform_derived_iv
 *
 * Returns 1 if elimination succeeded, 0 otherwise.
 */
int try_eliminate_iv_counter(TCCIRState *ir, IRLoop *loop, InductionVar *iv, DerivedIV *div, int ptr_vreg,
                                    int idx_shift)
{
  int n = ir->next_instruction_index;
  int iv_vr = iv->vreg;

  /* Adjusted loop indices (transform_derived_iv inserted instructions at header) */
  int adj_header = loop->header_idx + idx_shift;
  int adj_end = loop->end_idx + idx_shift;
  int adj_iv_def = iv->def_idx;
  if (iv->def_idx >= loop->header_idx)
    adj_iv_def += idx_shift;

  /* Step 1a: Find the CMP + JUMPIF pre-test guard that tests the IV.
   * After loop rotation and IV strength reduction insertions, the pre-test
   * guard CMP is typically in the preheader area (just before the header).
   * Scan from the preheader up through a few instructions past the header. */
  int hdr_cmp_idx = -1, hdr_jmpif_idx = -1;
  int limit_val = 0, hdr_cond_token = 0;

  {
    int scan_start = loop->preheader_idx;
    if (scan_start < 0)
      scan_start = adj_header > 4 ? adj_header - 4 : 0;
    int scan_end = adj_header + 4;
    if (scan_end >= n - 1)
      scan_end = n - 2;

    for (int i = scan_start; i <= scan_end; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
        continue;

      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);

      if (irop_get_vreg(cmp_src1) != iv_vr || !irop_is_immediate(cmp_src2))
        continue;

      /* Look forward for the JUMPIF, skipping NOPs and ASSIGNs (which don't clobber flags) */
      int jq_idx = i + 1;
      while (jq_idx < n && (ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP ||
                            ir->compact_instructions[jq_idx].op == TCCIR_OP_ASSIGN))
        jq_idx++;

      if (jq_idx >= n)
        continue;
      IRQuadCompact *jq = &ir->compact_instructions[jq_idx];
      if (jq->op != TCCIR_OP_JUMPIF)
        continue;

      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      hdr_cond_token = (int)irop_get_imm64_ex(ir, cond_op);

      /* Exit target must be outside the loop */
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
      int exit_target = (int)irop_get_imm64_ex(ir, jmp_dest);
      if (exit_target <= adj_end + 1) /* +1 for the stride ADD we inserted */
        continue;

      limit_val = (int)irop_get_imm64_ex(ir, cmp_src2);
      hdr_cmp_idx = i;
      hdr_jmpif_idx = jq_idx;
      break;
    }
  }

  /* Step 1b: Find the CMP + JUMPIF near the back-edge (post-test / latch test).
   * This is typically just before the back-edge JUMP at adj_end. Scan backward
   * from adj_end looking for a CMP of the IV against the same limit. */
  int be_cmp_idx = -1, be_jmpif_idx = -1;
  int be_cond_token = 0;

  for (int i = adj_end; i >= adj_end - 5 && i >= 0; i--)
  {
    IRQuadCompact *cq = &ir->compact_instructions[i];
    if (cq->op != TCCIR_OP_CMP)
      continue;

    IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
    IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);

    if (irop_get_vreg(cmp_src1) != iv_vr || !irop_is_immediate(cmp_src2))
      continue;

    /* Look forward for the JUMPIF */
    int jq_idx = i + 1;
    while (jq_idx < n && (ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP ||
                          ir->compact_instructions[jq_idx].op == TCCIR_OP_ASSIGN))
      jq_idx++;

    if (jq_idx >= n)
      continue;
    IRQuadCompact *jq = &ir->compact_instructions[jq_idx];
    if (jq->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
    be_cond_token = (int)irop_get_imm64_ex(ir, cond_op);

    /* Back-edge target must be inside the loop (jumps back) */
    IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
    int back_target = (int)irop_get_imm64_ex(ir, jmp_dest);
    if (back_target > adj_end)
      continue; /* Not a back-edge */

    int be_limit = (int)irop_get_imm64_ex(ir, cmp_src2);

    /* Use limit_val from header if found, otherwise from back-edge */
    if (hdr_cmp_idx < 0)
      limit_val = be_limit;

    be_cmp_idx = i;
    be_jmpif_idx = jq_idx;
    break;
  }

  /* We need at least one CMP to proceed */
  if (hdr_cmp_idx < 0 && be_cmp_idx < 0)
  {
    LOG_IV_SR("IV_SR_ELIM: No CMP+JUMPIF found for IV VAR%d at header %d or back-edge %d",
              TCCIR_DECODE_VREG_POSITION(iv_vr), adj_header, adj_end);
    return 0;
  }

  if (TCC_LOG_IV_SR)
    fprintf(stderr, "[IV_SR_ELIM] hdr_cmp_idx=%d, be_cmp_idx=%d, adj_iv_def=%d, adj_iv_init=%d\n", hdr_cmp_idx,
            be_cmp_idx, adj_iv_def, iv->init_idx);

  /* Step 2: Check that the IV has no other uses besides:
   *   - The header CMP instruction (pre-test, if present)
   *   - The back-edge CMP instruction (post-test, if present)
   *   - Its own increment (adj_iv_def)
   *   - A copy-through temp (ASSIGN T=V just before the ADD)
   * If the IV is used elsewhere (e.g., as a function argument), we can't eliminate it. */
  int other_uses = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (i == hdr_cmp_idx || i == be_cmp_idx)
      continue; /* The CMPs we'll replace/remove */
    if (i == adj_iv_def)
      continue; /* The IV increment */

    /* Allow the copy-through pattern: T = V just before V = T + 1 */
    if (q->op == TCCIR_OP_ASSIGN && i >= adj_iv_def - 2 && i < adj_iv_def)
    {
      IROperand asrc = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(asrc) == iv_vr)
        continue; /* This is the copy-through temp */
    }

    /* Check src1 and src2 for uses of the IV */
    if (irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(s1) == iv_vr)
      {
        other_uses++;
        if (TCC_LOG_IV_SR)
          fprintf(stderr, "[IV_SR_ELIM] other_uses++ at idx=%d (src1) op=%d\n", i, q->op);
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_get_vreg(s2) == iv_vr)
      {
        other_uses++;
        if (TCC_LOG_IV_SR)
          fprintf(stderr, "[IV_SR_ELIM] other_uses++ at idx=%d (src2) op=%d\n", i, q->op);
      }
    }
  }

  if (other_uses > 0)
  {
    LOG_IV_SR("IV_SR_ELIM: IV VAR%d has %d other uses, cannot eliminate", TCCIR_DECODE_VREG_POSITION(iv_vr),
              other_uses);
    return 0;
  }

  /* Step 3: Compute end_ptr = base + limit * element_size
   * element_size = stride / step (e.g., stride=4, step=1 → element_size=4)
   * end_value = limit * element_size */
  int element_size = div->stride / iv->step;
  int end_offset = limit_val * element_size;

  /* Allocate a vreg for end_ptr */
  int end_vreg = tcc_ir_vreg_alloc_temp(ir);
  if (end_vreg < 0)
    return 0;

  IROperand end_op = irop_make_vreg(end_vreg, IROP_BTYPE_INT32);
  IROperand ptr_op = irop_make_vreg(ptr_vreg, IROP_BTYPE_INT32);
  IROperand null_op = {0};

  /* Insert end_ptr computation in preheader (before the loop header).
   * We insert at adj_header (which is already shifted).
   *
   * Use the strength-reduced ptr_vreg as the source rather than div->base_op
   * whenever ptr's value at this point equals base.  That holds when
   * iv.init_val == 0 — transform_derived_iv inserted `ptr = base` (a single
   * ASSIGN) and nothing has mutated ptr yet.  Using ptr_op saves an extra
   * materialization of the base address: ptr is already register-resident,
   * whereas base_op (a SYMREF/STACKOFF/etc) would require a second LDR [PC]
   * or LEA to bring into a register.
   *
   * When end_offset != 0, also emit a single non-destructive ADD (dest != src1)
   *   end_ptr = src + end_offset
   * rather than ASSIGN end_ptr = src; end_ptr += off.  On Thumb-2 the former
   * is one wide `add.w rd, rn, #imm` instruction; the latter is two. */
  IROperand end_src = (iv->init_val == 0) ? ptr_op : div->base_op;
  int insert_pos = adj_header;
  int end_shift = 0;
  int inserted;

  if (end_offset != 0)
  {
    IROperand offset_op = irop_make_imm32(-1, end_offset, IROP_BTYPE_INT32);
    inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ADD, end_op, end_src, offset_op);
    if (inserted < 0)
      return 0;
    end_shift = 1;
  }
  else
  {
    inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ASSIGN, end_op, end_src, null_op);
    if (inserted < 0)
      return 0;
    end_shift = 1;
  }

  /* Update indices after insertion — only shift those at or after insert_pos */
  if (hdr_cmp_idx >= 0 && hdr_cmp_idx >= insert_pos)
  {
    hdr_cmp_idx += end_shift;
    hdr_jmpif_idx += end_shift;
  }
  if (be_cmp_idx >= 0 && be_cmp_idx >= insert_pos)
  {
    be_cmp_idx += end_shift;
    be_jmpif_idx += end_shift;
  }

  /* Step 4: Replace the back-edge CMP with pointer comparison (ptr vs end_ptr).
   * This is the primary loop continuation test. */
  if (be_cmp_idx >= 0)
  {
    int be_unsigned_cond = signed_to_unsigned_cond(be_cond_token);
    if (be_unsigned_cond < 0)
    {
      LOG_IV_SR("IV_SR_ELIM: Unsupported back-edge condition token 0x%x", be_cond_token);
      return 0;
    }

    IRQuadCompact *be_cmp_q = &ir->compact_instructions[be_cmp_idx];
    tcc_ir_op_set_src1(ir, be_cmp_q, ptr_op);
    tcc_ir_op_set_src2(ir, be_cmp_q, end_op);

    IRQuadCompact *be_jmp_q = &ir->compact_instructions[be_jmpif_idx];
    IROperand be_new_cond = irop_make_imm32(-1, be_unsigned_cond, IROP_BTYPE_INT32);
    tcc_ir_op_set_src1(ir, be_jmp_q, be_new_cond);
  }

  /* Step 5: Handle the header pre-test guard.
   * If the initial value satisfies the exit condition (e.g., init=0 < limit=5),
   * the pre-test is always false (loop always executes), so we can NOP it out.
   * Otherwise, replace with pointer comparison. */
  if (hdr_cmp_idx >= 0)
  {
    /* If there's a back-edge CMP, the pre-test is just a guard and can potentially
     * be NOP'd away. If the pre-test is the ONLY loop exit test (no back-edge CMP),
     * we must keep it and replace with pointer comparison — NOP'ing it would make
     * the loop infinite. */
    if (be_cmp_idx >= 0)
    {
      /* Back-edge test exists. Check if the pre-test can be constant-folded away.
       * The header tests: if (iv_init <cond> limit) goto exit
       * If this is always false for the initial value, the guard is redundant. */
      int guard_always_false = evaluate_compare_condition((int64_t)iv->init_val, (int64_t)limit_val, hdr_cond_token);

      if (!guard_always_false)
      {
        /* Guard is never taken — NOP out both CMP and JUMPIF */
        ir->compact_instructions[hdr_cmp_idx].op = TCCIR_OP_NOP;
        ir->compact_instructions[hdr_jmpif_idx].op = TCCIR_OP_NOP;
      }
      else
      {
        /* Guard might be taken — replace with pointer comparison */
        int hdr_unsigned_cond = signed_to_unsigned_cond(hdr_cond_token);
        if (hdr_unsigned_cond >= 0)
        {
          IRQuadCompact *hdr_cmp_q = &ir->compact_instructions[hdr_cmp_idx];
          tcc_ir_op_set_src1(ir, hdr_cmp_q, ptr_op);
          tcc_ir_op_set_src2(ir, hdr_cmp_q, end_op);

          IRQuadCompact *hdr_jmp_q = &ir->compact_instructions[hdr_jmpif_idx];
          IROperand hdr_new_cond = irop_make_imm32(-1, hdr_unsigned_cond, IROP_BTYPE_INT32);
          tcc_ir_op_set_src1(ir, hdr_jmp_q, hdr_new_cond);
        }
      }
    }
    else
    {
      /* Pre-test is the only loop exit — must replace with pointer comparison */
      int hdr_unsigned_cond = signed_to_unsigned_cond(hdr_cond_token);
      if (hdr_unsigned_cond >= 0)
      {
        IRQuadCompact *hdr_cmp_q = &ir->compact_instructions[hdr_cmp_idx];
        tcc_ir_op_set_src1(ir, hdr_cmp_q, ptr_op);
        tcc_ir_op_set_src2(ir, hdr_cmp_q, end_op);

        IRQuadCompact *hdr_jmp_q = &ir->compact_instructions[hdr_jmpif_idx];
        IROperand hdr_new_cond = irop_make_imm32(-1, hdr_unsigned_cond, IROP_BTYPE_INT32);
        tcc_ir_op_set_src1(ir, hdr_jmp_q, hdr_new_cond);
      }
    }
  }

  /* Step 6: NOP out the now-dead IV initialization and increment.
   * DCE cannot eliminate self-referential cycles (V = V + 1 uses itself),
   * so we must explicitly remove them.
   *
   * Index adjustments:
   * - iv->init_idx is in the preheader (before header_idx), not shifted by any insertions
   * - adj_iv_def was already adjusted for idx_shift; needs end_shift added for our insertions */
  {
    /* NOP the IV initialization (in preheader, not shifted) */
    int adj_iv_init = iv->init_idx;
    if (adj_iv_init >= 0 && adj_iv_init < ir->next_instruction_index)
    {
      IRQuadCompact *init_q = &ir->compact_instructions[adj_iv_init];
      IROperand init_dest = tcc_ir_op_get_dest(ir, init_q);
      if (irop_get_vreg(init_dest) == iv_vr)
        init_q->op = TCCIR_OP_NOP;
    }

    /* NOP the IV increment (in loop body, shifted by both idx_shift and end_shift) */
    int adj_iv_inc = adj_iv_def + end_shift;
    if (adj_iv_inc >= 0 && adj_iv_inc < ir->next_instruction_index)
    {
      IRQuadCompact *inc_q = &ir->compact_instructions[adj_iv_inc];
      IROperand inc_dest = tcc_ir_op_get_dest(ir, inc_q);
      if (irop_get_vreg(inc_dest) == iv_vr)
        inc_q->op = TCCIR_OP_NOP;

      /* Also NOP the copy-through temp (T1 = V1) that precedes V1 = T1 + 1 */
      for (int k = adj_iv_inc - 1; k >= adj_iv_inc - 3 && k >= 0; k--)
      {
        IRQuadCompact *cq = &ir->compact_instructions[k];
        if (cq->op == TCCIR_OP_NOP)
          continue;
        if (cq->op == TCCIR_OP_ASSIGN)
        {
          IROperand csrc = tcc_ir_op_get_src1(ir, cq);
          if (irop_get_vreg(csrc) == iv_vr)
          {
            cq->op = TCCIR_OP_NOP;
            break;
          }
        }
        break; /* Stop at first non-NOP, non-matching */
      }
    }
  }

  LOG_IV_SR("IV_SR_ELIM: Eliminated IV VAR%d, replaced CMP with ptr(TMP%d) vs end(TMP%d), "
            "end_offset=%d, hdr_cmp=%d, be_cmp=%d",
            TCCIR_DECODE_VREG_POSITION(iv_vr), TCCIR_DECODE_VREG_POSITION(ptr_vreg),
            TCCIR_DECODE_VREG_POSITION(end_vreg), end_offset, hdr_cmp_idx, be_cmp_idx);

  return 1;
}

/* Main entry point: Induction Variable Strength Reduction
 * Returns number of transformations applied
 */
/* Core IV strength reduction using pre-detected loops */

int iv_strength_reduction_core(TCCIRState *ir, IRLoops *loops)
{
  int total_changes = 0;

  LOG_IV_SR("IV_SR: Found %d loop(s)", loops->num_loops);

  /* Process each loop, but only process loops with valid preheaders */
  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    if (loop->preheader_idx < 0)
      continue;

    /* Skip if inserting at preheader+1 would land inside a CHILD loop's
     * body range (a smaller loop contained within ours).  Inserting inside
     * a parent loop is fine — that's the expected case for inner loops. */
    {
      int insert_pos = loop->preheader_idx + 1;
      int loop_size = loop->end_idx - loop->start_idx;
      int skip = 0;
      for (int other = 0; other < loops->num_loops; other++)
      {
        if (other == li)
          continue;
        IRLoop *oloop = &loops->loops[other];
        int oloop_size = oloop->end_idx - oloop->start_idx;
        if (insert_pos > oloop->start_idx && insert_pos <= oloop->end_idx && oloop_size < loop_size)
        {
          skip = 1;
          break;
        }
      }
      if (skip)
        continue;
    }

    InductionVar ivs[MAX_IV];
    DerivedIV divs[MAX_DIV];

    int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1);
    if (num_ivs == 0)
      continue;

    int num_divs = find_derived_ivs(ir, loop, ivs, num_ivs, divs, MAX_DIV);
    if (num_divs == 0)
      continue;

    LOG_IV_SR("IV_SR: Found %d DIV(s) in loop %d", num_divs, li);

    /* Deduplicate DIVs that compute identical (iv, stride, base) recurrences.
     * A duplicate (share_with >= 0) is only attempted when its primary FAILED
     * to transform, and transform_derived_iv refuses shared rewrites outright
     * (no escape analysis ran for the duplicate's use site — docs/bugs.md #2),
     * so marking a duplicate effectively defers it: the driver's re-detection
     * loop revisits it as an independent primary with exact indices. */
    for (int dj = 1; dj < num_divs; dj++)
    {
      for (int dk = 0; dk < dj; dk++)
      {
        if (divs[dk].share_with >= 0)
          continue; /* only chain to primaries */
        if (divs[dj].iv_idx != divs[dk].iv_idx || divs[dj].stride != divs[dk].stride)
          continue;
        /* Compare base operands: same vreg, or same immediate value, or same stack offset. */
        IROperand a = divs[dj].base_op;
        IROperand b = divs[dk].base_op;
        int tag_a = irop_get_tag(a);
        int tag_b = irop_get_tag(b);
        if (tag_a != tag_b)
          continue;
        int eq = 0;
        if (tag_a == IROP_TAG_IMM32 || tag_a == IROP_TAG_STACKOFF)
          eq = (a.u.imm32 == b.u.imm32 && a.is_lval == b.is_lval);
        else if (tag_a == IROP_TAG_VREG)
        {
          int32_t va = irop_get_vreg(a);
          int32_t vb = irop_get_vreg(b);
          if (va >= 0 && va == vb && a.is_lval == b.is_lval)
            eq = 1;
        }
        if (eq)
        {
          divs[dj].share_with = dk;
          LOG_IV_SR("IV_SR: DIV %d shares pointer with DIV %d (iv_idx=%d stride=%d)", dj, dk, divs[dj].iv_idx,
                    divs[dj].stride);
          break;
        }
      }
    }

    /* Transform each derived IV, deferring IV elimination to pick the
     * cheapest end-pointer across all transformed DIVs. */
    int div_ptr_vregs[MAX_DIV];
    int div_changes[MAX_DIV];
    for (int di = 0; di < num_divs; di++)
    {
      div_ptr_vregs[di] = -1;
      div_changes[di] = 0;
    }

    for (int di = 0; di < num_divs; di++)
    {
      int sr_ptr_vreg = -1, sr_idx_shift = 0;
      int sr_postnop_origpos = -1;
      int sr_stride_pos = -1;
      int shared = -1;
      if (divs[di].share_with >= 0)
      {
        /* Use primary's already-allocated ptr (must have been processed first
         * given the dedup invariant share_with < di and we iterate in order). */
        shared = div_ptr_vregs[divs[di].share_with];
        if (shared < 0)
        {
          LOG_IV_SR("IV_SR: skipping shared DIV %d — primary %d not transformed", di, divs[di].share_with);
          continue;
        }
      }
      int changes =
          transform_derived_iv(ir, loop, &ivs[divs[di].iv_idx], &divs[di], &sr_ptr_vreg, &sr_idx_shift,
                               &sr_postnop_origpos, &sr_stride_pos, shared);
      total_changes += changes;
      div_ptr_vregs[di] = sr_ptr_vreg;
      div_changes[di] = changes;

      /* After transformation, indices have shifted.  insert_instr_at
       * shifts all instructions >= pos by +1 per insertion.  Apply the
       * same position-aware shift to remaining loops' metadata.
       *
       * Insertion points (before any shifting):
       *   init_pos   = preheader_idx + 1 (sr_idx_shift instructions)
       *   postnop    = div->use_idx + 1 (1 instruction, INDEXED-DIV only)
       *   stride_pos = iv->def_idx + sr_idx_shift + 1 (1 instruction)
       */
      if (changes > 0)
      {
        /* Compute shift for each original-space index.  Insertions:
         *   sr_idx_shift instructions at init_pos = preheader_idx + 1
         *   1 instruction at use_idx + 1 (only when sr_postnop_origpos >= 0)
         *   1 instruction at def_idx + 1 (only when changes >= 3)
         * Since init_pos < use_idx + 1 < def_idx + 1 always holds for a
         * single loop body, the total shift for original index X:
         *   X < init_pos                  → 0
         *   init_pos <= X <= use_idx      → sr_idx_shift
         *   use_idx < X <= def_idx        → sr_idx_shift + 1 (postnop only)
         *   X > def_idx (and changes>=3)  → sr_idx_shift + (1|0) + 1
         */
        int init_pos = loop->preheader_idx + 1;
        int has_stride = (changes >= 3);
        int has_postnop = (sr_postnop_origpos >= 0);
        int orig_use_for_nop = sr_postnop_origpos; /* div->use_idx in pre-call space */
        /* Actual stride ADD insertion position, in post-(init+postnop) index
         * space (see transform_derived_iv).  Compared against the already
         * init/postnop-shifted index below — NOT against iv->def_idx, which is
         * wrong whenever the stride was pushed past a FUNCCALL. */
        int stride_pos = sr_stride_pos;

#define APPLY_SHIFT(idx)                                                                                               \
  do                                                                                                                   \
  {                                                                                                                    \
    int _orig = (idx);                                                                                                 \
    if (_orig >= init_pos)                                                                                             \
    {                                                                                                                  \
      (idx) = _orig + sr_idx_shift;                                                                                    \
      if (has_postnop && _orig > orig_use_for_nop)                                                                     \
        (idx)++;                                                                                                       \
      if (has_stride && stride_pos >= 0 && (idx) >= stride_pos)                                                        \
        (idx)++;                                                                                                       \
    }                                                                                                                  \
  } while (0)

        /* Shift indices of remaining DIVs and all IVs so we can
         * continue processing more DIVs in this loop. */
        for (int dj = di + 1; dj < num_divs; dj++)
        {
          APPLY_SHIFT(divs[dj].use_idx);
          APPLY_SHIFT(divs[dj].shl_idx);
        }
        for (int ij = 0; ij < num_ivs; ij++)
        {
          APPLY_SHIFT(ivs[ij].def_idx);
          APPLY_SHIFT(ivs[ij].init_idx);
        }

        /* Shift current loop metadata */
        APPLY_SHIFT(loop->header_idx);
        APPLY_SHIFT(loop->start_idx);
        APPLY_SHIFT(loop->end_idx);
        if (loop->preheader_idx >= 0)
          APPLY_SHIFT(loop->preheader_idx);
        for (int bi = 0; bi < loop->num_body_instrs; bi++)
          APPLY_SHIFT(loop->body_instrs[bi]);

#undef APPLY_SHIFT

        /* Transform only ONE derived IV per loop per call, then bail to IV
         * elimination.  Processing several DIVs in one call requires shifting
         * every remaining DIV's recorded indices (APPLY_SHIFT) by the exact
         * number of instructions this transform inserted (init + optional
         * postnop + stride, some pushed past calls); a single off-by-one there
         * lands a later DIV's use_idx on an unrelated instruction — e.g. a
         * FUNCPARAMVAL — which the next transform then rewrites/NOPs, corrupting
         * the call's parameter sequence ("missing FUNCPARAMVAL for call_id=N").
         * The driver (ssa_opt_iv_strength_reduction) re-detects loops and DIVs
         * from scratch and calls us again, so the remaining DIVs are handled on
         * later iterations with exact indices and no stale shifts. */
        goto try_elim;
      }
    }
    continue;

  try_elim:
    /* All DIVs in this loop are transformed.  Now try IV elimination
     * with each candidate, preferring the one whose end-pointer is
     * cheapest to materialize.
     *
     * Heuristic: prefer stack-based base (SP-relative end = single ADD)
     * over immediate base.  Among same-kind bases, prefer smaller
     * absolute end_offset (fewer bits to encode). */
    {
      int best_di = -1;
      int best_cost = 0x7fffffff;

      for (int di = 0; di < num_divs; di++)
      {
        if (div_changes[di] != 3 || div_ptr_vregs[di] < 0)
          continue;

        int element_size = divs[di].stride / ivs[divs[di].iv_idx].step;
        int abs_end_offset = ivs[divs[di].iv_idx].init_val * element_size;
        if (abs_end_offset < 0)
          abs_end_offset = -abs_end_offset;

        int cost;
        if (irop_get_tag(divs[di].base_op) == IROP_TAG_STACKOFF)
          cost = abs_end_offset;
        else
          cost = abs_end_offset + 0x10000;

        if (cost < best_cost)
        {
          best_cost = cost;
          best_di = di;
        }
      }

      if (best_di >= 0)
      {
        /* idx_shift=0 because APPLY_SHIFT already updated all loop/IV indices
         * to current (post-all-transforms) positions. */
        int elim =
            try_eliminate_iv_counter(ir, loop, &ivs[divs[best_di].iv_idx], &divs[best_di], div_ptr_vregs[best_di], 0);
        total_changes += elim;
      }
    }
    goto done;
  }

done:
  LOG_IV_SR("=== IV STRENGTH REDUCTION END: %d changes ===", total_changes);

  return total_changes;
}
