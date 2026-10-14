/*
 *  TCC IR - Return each value where it is computed, not through a shared return block
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * `ret = -EINVAL; goto out; ... out: return ret;` and an inlined callee's
 * `return x` (its result variable, then the caller's `return result`) both end
 * in one return block fed by a carrier: `C <- x; JMP L`, with L holding only
 * `[T <- C] RETURNVALUE T|C`.  The carrier needs a register live from the
 * first arm to L, which the linear scan cannot share with anything live in
 * between, so every arm pays `mov rC, x` and L pays `mov r0, rC` (plus a
 * `uxtb` re-extending a bool carrier).  Here each such jump becomes
 * `RETURNVALUE x` (`mov r0, x; b epilogue`), and the carrier copy goes when
 * nothing else reads it.
 *
 * An arm `C <- x; JMP L` qualifies when:
 *   - only NOPs separate the copy from the JMP, and neither the JMP nor those
 *     NOPs is a jump target (the copy runs on every path to the JMP);
 *   - L's block is NOPs, an optional `T <- C` ASSIGN/LOAD, and RETURNVALUE of
 *     T (or of C directly) -- a plain int up to 32 bits;
 *   - reading x at the JMP gives what the return block returns: C is read at
 *     one width and written at it or as a word, T is that width or a word, and
 *     x is an immediate that fits C, or the vreg the instruction right before
 *     the copy defines -- any when C is a word, a SETIF's 0/1 when narrow.
 * A fallthrough arm is the block's last predecessor: when it is the only one
 * left, the block itself returns x and loses its copies.
 *
 * Disableable via TCC_DISABLE_PASS=ra:return_sink; gated -O1+ like its ra:
 * neighbours.
 */

#define USING_GLOBALS

#include "ir.h"
#include "regalloc.h"

/* An immediate with no symbol, or a vreg read as a value (a VAR or PARAM by
 * its slot form included). */
static int rs_plain(IROperand o)
{
  if (irop_is_immediate(o))
    return !o.is_sym;
  return irop_is_vreg_value(o) && !o.is_sym && !o.is_complex;
}

static int rs_int_btype(int bt)
{
  return bt == IROP_BTYPE_INT32 || bt == IROP_BTYPE_INT8 || bt == IROP_BTYPE_INT16;
}

static int rs_same_width(IROperand a, IROperand b)
{
  if (a.btype != b.btype)
    return 0;
  return a.btype == IROP_BTYPE_INT32 || a.is_unsigned == b.is_unsigned;
}

/* The register value of a `bt`-typed operand holding v: 0 when v does not fit. */
static int rs_fits(IROperand o, int64_t v)
{
  switch (o.btype) {
  case IROP_BTYPE_INT8:
    return o.is_unsigned ? (v >= 0 && v <= 255) : (v >= -128 && v <= 127);
  case IROP_BTYPE_INT16:
    return o.is_unsigned ? (v >= 0 && v <= 65535) : (v >= -32768 && v <= 32767);
  default:
    return v >= INT32_MIN && v <= (int64_t)UINT32_MAX;
  }
}

/* Reads of vreg other than as a plain destination; a STORE through it reads it. */
static int rs_reads(TCCIRState *ir, int32_t vreg, int *only)
{
  int n = 0;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int hit = 0;
    if (irop_config[q->op].has_dest) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (irop_has_vreg(d) && irop_get_vreg(d) == vreg && !irop_dest_defines_vreg(d))
        hit = 1;
    }
    if (irop_config[q->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_has_vreg(s) && irop_get_vreg(s) == vreg)
        hit = 1;
    }
    if (irop_config[q->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      if (irop_has_vreg(s) && irop_get_vreg(s) == vreg)
        hit = 1;
    }
    if (tcc_ir_op_is_mac(q->op)) {
      IROperand s = tcc_ir_op_get_accum(ir, q);
      if (irop_has_vreg(s) && irop_get_vreg(s) == vreg)
        hit = 1;
    }
    if (hit) {
      n++;
      *only = i;
    }
  }
  return n;
}

static IRLiveInterval *rs_var_info(TCCIRState *ir, int32_t vreg)
{
  if (TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_VAR)
    return NULL;
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  return pos >= 0 && pos < ir->variables_live_intervals_size ? &ir->variables_live_intervals[pos] : NULL;
}

static int rs_volatile_var(TCCIRState *ir, int32_t vreg)
{
  IRLiveInterval *iv = rs_var_info(ir, vreg);
  return iv && iv->is_volatile;
}

static int rs_shared_var(TCCIRState *ir, int32_t vreg)
{
  if (TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_VAR)
    return 0;
  IRLiveInterval *iv = rs_var_info(ir, vreg);
  return !iv || iv->is_volatile || iv->addrtaken;
}

typedef struct {
  int ret;          /* the RETURNVALUE */
  int copy;         /* `T <- C` in the return block, or -1 */
  int32_t carrier;  /* C */
  IROperand c_read; /* C as the copy (or the RETURNVALUE) reads it */
  IROperand t_dest; /* T as the copy writes it (== c_read without a copy) */
  IROperand r_src;  /* the returned operand */
} RSBlock;

/* The value `p: C <- x` leaves for the return block, as an operand to return
 * directly; 0 if the chain could change it. */
static int rs_arm_value(TCCIRState *ir, const RSBlock *rb, int p, IROperand *out)
{
  IRQuadCompact *pq = &ir->compact_instructions[p];
  if (pq->op != TCCIR_OP_ASSIGN && pq->op != TCCIR_OP_STORE)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, pq);
  IROperand x = tcc_ir_op_get_src1(ir, pq);
  if (!irop_dest_defines_vreg(d) || irop_get_vreg(d) != rb->carrier || !rs_plain(x))
    return 0;
  /* C is read at one width W and written at W or as a word (a bool VAR's
   * store is a word, its reads zero-extend a byte); the copy out of C keeps W
   * or widens to a word. */
  IROperand w = rb->c_read;
  if ((d.btype != IROP_BTYPE_INT32 && !rs_same_width(d, w)) || !rs_same_width(rb->t_dest, rb->r_src) ||
      (rb->t_dest.btype != IROP_BTYPE_INT32 && !rs_same_width(w, rb->t_dest)))
    return 0;
  if (irop_is_immediate(x)) {
    int64_t v = irop_get_imm64_ex(ir, x);
    if (x.btype == IROP_BTYPE_INT64 || !rs_fits(w, v) || !rs_fits(d, v))
      return 0;
    *out = irop_make_imm32(-1, (int32_t)v, IROP_BTYPE_INT32);
    return 1;
  }
  /* A vreg only as the arm's own result, defined right before the copy: an
   * older value passed through (a loop's zero-trip `i = 0` reaching the
   * return) coalesces with C and lets its guard branch straight to L, which
   * an arm that returns by itself would undo. */
  if (!rs_int_btype(x.btype))
    return 0;
  int k = p - 1;
  while (k >= 0 && ir->compact_instructions[k].op == TCCIR_OP_NOP && !ir->compact_instructions[k + 1].is_jump_target)
    k--;
  if (k < 0 || ir->compact_instructions[k + 1].is_jump_target)
    return 0;
  IRQuadCompact *sq = &ir->compact_instructions[k];
  if (!irop_config[sq->op].has_dest)
    return 0;
  IROperand sd = tcc_ir_op_get_dest(ir, sq);
  if (!irop_dest_defines_vreg(sd) || irop_get_vreg(sd) != irop_get_vreg(x) || !rs_int_btype(sd.btype))
    return 0;
  /* A word chain hands x through unchanged; a narrow one re-extends at every
   * step, which only a SETIF's 0/1 survives. */
  if (x.btype == IROP_BTYPE_INT32 && w.btype == IROP_BTYPE_INT32 && d.btype == IROP_BTYPE_INT32) {
    *out = x;
    return 1;
  }
  if (sq->op != TCCIR_OP_SETIF)
    return 0;
  *out = sd;
  return 1;
}

/* The copy before instruction j, past NOPs nothing jumps to; -1 if none. */
static int rs_copy_before(TCCIRState *ir, int j, int lo)
{
  int k = j - 1;
  while (k >= lo && ir->compact_instructions[k].op == TCCIR_OP_NOP) {
    if (ir->compact_instructions[k].is_jump_target)
      return -1;
    k--;
  }
  return k >= lo ? k : -1;
}

static int rs_all_nop(TCCIRState *ir, const IRBasicBlock *bb)
{
  for (int i = bb->start_idx; i < bb->end_idx; i++)
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      return 0;
  return 1;
}

static int rs_block(TCCIRState *ir, IRCFG *cfg, int b, uint8_t *dead_copy)
{
  IRBasicBlock *bb = &cfg->blocks[b];
  int r = bb->end_idx - 1; /* end_idx is exclusive */
  if (r < bb->start_idx || r >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *rq = &ir->compact_instructions[r];
  if (rq->op != TCCIR_OP_RETURNVALUE)
    return 0;
  RSBlock rb = { .ret = r, .copy = -1 };
  rb.r_src = tcc_ir_op_get_src1(ir, rq);
  if (!rs_plain(rb.r_src) || irop_is_immediate(rb.r_src) || !rs_int_btype(rb.r_src.btype))
    return 0;
  rb.carrier = irop_get_vreg(rb.r_src);
  rb.c_read = rb.t_dest = rb.r_src;
  for (int i = bb->start_idx; i < r; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (rb.copy >= 0 || (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD))
      return 0;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (!irop_dest_defines_vreg(d) || irop_get_vreg(d) != rb.carrier || !rs_plain(s) || irop_is_immediate(s))
      return 0;
    rb.copy = i;
    rb.t_dest = d;
    rb.c_read = s;
    rb.carrier = irop_get_vreg(s);
  }
  /* A volatile carrier's read is an access of its own: it stays. */
  if (tcc_ir_access_is_volatile(ir, rb.c_read) || rs_volatile_var(ir, rb.carrier))
    return 0;
  /* What nothing else reads, the arms may stop writing. */
  int only = -1;
  int reads = rs_reads(ir, rb.carrier, &only);
  int carrier_private = reads == 1 && only == (rb.copy >= 0 ? rb.copy : r) && !rs_shared_var(ir, rb.carrier);

  /* a label's address or an asm goto enters blocks the CFG shows no edge into */
  const int hidden_entries = ir->func_has_label_addr || ir->func_has_asm_goto;
  int changes = 0, left = 0, fall_p = -1;
  IROperand fall_x = IROP_NONE;
  for (int pi = 0; pi < bb->num_preds; pi++) {
    IRBasicBlock *pb = &cfg->blocks[bb->preds[pi]];
    int j = pb->end_idx - 1;
    /* a block nothing enters (the NOPs a threaded join left) */
    if (bb->preds[pi] != 0 && pb->num_preds == 0 && rs_all_nop(ir, pb) && !hidden_entries)
      continue;
    if (j < pb->start_idx) {
      left++;
      continue;
    }
    IRQuadCompact *jq = &ir->compact_instructions[j];
    IROperand x;
    if (jq->op == TCCIR_OP_JUMP) {
      int p = rs_copy_before(ir, j, pb->start_idx);
      if (p < 0 || jq->is_jump_target || !rs_arm_value(ir, &rb, p, &x)) {
        left++;
        continue;
      }
      jq->op = TCCIR_OP_RETURNVALUE;
      tcc_ir_set_src1(ir, j, x);
      if (carrier_private)
        dead_copy[p] = 1;
      changes++;
      continue;
    }
    left++;
    if (pb->end_idx != bb->start_idx || ir_op_has(jq->op, IROP_ENDS_BLOCK))
      continue;
    int p = jq->op == TCCIR_OP_NOP ? rs_copy_before(ir, j + 1, pb->start_idx) : j;
    if (p >= 0 && rs_arm_value(ir, &rb, p, &x)) {
      fall_p = p;
      fall_x = x;
    }
  }
  /* Every arm returns by itself: nothing reaches the block, whose read of
   * the carrier would otherwise keep it live from the function's entry. */
  if (left == 0 && changes > 0 && b != 0 && !hidden_entries) {
    if (rb.copy >= 0)
      dead_copy[rb.copy] = 1;
    dead_copy[r] = 1;
  }
  /* The fallthrough arm is the block's only way in: return x from the block. */
  if (left == 1 && fall_p >= 0 && carrier_private) {
    tcc_ir_set_src1(ir, r, fall_x);
    if (rb.copy >= 0)
      dead_copy[rb.copy] = 1;
    dead_copy[fall_p] = 1;
    changes++;
  }
  return changes;
}

int ra_return_sink(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg || cfg->num_blocks <= 1) {
    tcc_ir_cfg_free(cfg);
    return 0;
  }
  int n = ir->next_instruction_index;
  uint8_t *dead_copy = tcc_mallocz(n);
  int changes = 0;
  for (int b = 0; b < cfg->num_blocks; b++)
    changes += rs_block(ir, cfg, b, dead_copy);
  for (int i = 0; i < n; i++)
    if (dead_copy[i])
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
  tcc_free(dead_copy);
  tcc_ir_cfg_free(cfg);
  return changes;
}
