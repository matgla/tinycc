/*
 *  TCC IR - SSA join-local TEMP reaching-constant forwarding (ssa:tmp_block_const)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Every SSA propagation path (sccp Phase 1, cprop, branch's CMP resolution) is
 * def_count==1-gated, so a multi-def TEMP — the shape var promotion produces
 * for a mutated local — never propagates its flow-sensitive constants even
 * when the reaching def is a plain `T <-- #C`.  Temps have no memory aliasing:
 * between join points only another def changes a temp's value, so a forward
 * walk substituting the reaching constant into value reads is exact.  The
 * driver's fixpoint then lets sccp/fold/branch collapse what this exposes. */

#define USING_GLOBALS

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "opt/ssa/tmp_block_const.h"

/* barrel_shifts[] encoding: (type<<5)|amount; 1=SHL 2=SHR 3=SAR 4=ROR. */
static int32_t tbc_apply_barrel(int32_t v, uint8_t ann)
{
  uint32_t x = (uint32_t)v;
  int amt = ann & 0x1F;
  switch (ann >> 5)
  {
  case 1: x <<= amt; break;
  case 2: x >>= amt; break;
  case 3: x = (uint32_t)((int32_t)x >> amt); break;
  case 4: x = (x >> amt) | (x << ((32 - amt) & 31)); break;
  default: break;
  }
  return (int32_t)x;
}

/* Pure 32-bit binaries only: carry-coupled ops (ADC/SUBC) must keep executing. */
static int tbc_foldable_binop(int op)
{
  switch (op)
  {
  case TCCIR_OP_ADD: case TCCIR_OP_SUB: case TCCIR_OP_MUL:
  case TCCIR_OP_AND: case TCCIR_OP_OR:  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL: case TCCIR_OP_SHR: case TCCIR_OP_SAR: case TCCIR_OP_ROR:
  case TCCIR_OP_DIV: case TCCIR_OP_UDIV: case TCCIR_OP_IMOD: case TCCIR_OP_UMOD:
    return 1;
  default:
    return 0;
  }
}

typedef struct
{
  uint32_t *gen;   /* fact valid iff gen[pos] == cur_gen */
  int32_t *value;
  uint32_t cur_gen;
  int ntemp;
} TbcFacts;

static int tbc_lookup(const TbcFacts *f, int pos, int32_t *out)
{
  if (pos < 0 || pos >= f->ntemp || f->gen[pos] != f->cur_gen)
    return 0;
  *out = f->value[pos];
  return 1;
}

/* Plain integer immediate -> low 32 bits.  An I64-pool imm in an INT32
 * context is just how sccp materializes constants above INT32_MAX. */
static int tbc_imm32_of(TCCIRState *ir, IROperand o, int32_t *out)
{
  int tag = irop_get_tag(o);
  if (!irop_is_plain_imm(o) || (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64))
    return 0;
  *out = (int32_t)(uint32_t)(uint64_t)irop_get_imm64_ex(ir, o);
  return 1;
}

/* Operand -> known 32-bit value: a plain integer imm or a temp with a live fact. */
static int tbc_resolve(TCCIRState *ir, IROperand o, const TbcFacts *f, int32_t *out)
{
  if (tbc_imm32_of(ir, o, out))
    return 1;
  int32_t vr = irop_get_vreg(o);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  if (irop_get_tag(o) != IROP_TAG_VREG || o.is_lval || o.is_llocal)
    return 0;
  if (irop_get_btype(o) != IROP_BTYPE_INT32)
    return 0;
  return tbc_lookup(f, TCCIR_DECODE_VREG_POSITION(vr), out);
}

int ssa_opt_tmp_block_const(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int ntemp = ir->next_temporary_variable;
  int changes = 0;

  if (n <= 0 || ntemp <= 0)
    return 0;

  /* Join points derived from the CURRENT jumps: the SSA-phase CFG blocks and
   * is_jump_target flags both go stale after branch folds/retargets — a
   * stale-fine partition drops facts mid-straight-line, a stale-coarse one
   * would be unsound.  IJUMP targets are unknowable: bail. */
  uint8_t *leader = tcc_mallocz(n);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP)
    {
      tcc_free(leader);
      return 0;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      if (t >= 0 && t < n)
        leader[t] = 1;
    }
  }
  for (int ti = 0; ti < ir->num_switch_tables; ti++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[ti];
    for (int j = 0; j < table->num_entries; j++)
      if (table->targets[j] >= 0 && table->targets[j] < n)
        leader[table->targets[j]] = 1;
    if (table->default_target >= 0 && table->default_target < n)
      leader[table->default_target] = 1;
  }

  TbcFacts f;
  f.gen = tcc_mallocz(ntemp * sizeof(uint32_t));
  f.value = tcc_malloc(ntemp * sizeof(int32_t));
  f.cur_gen = 1;
  f.ntemp = ntemp;

  for (int i = 0; i < n; i++)
  {
    if (leader[i])
      f.cur_gen++;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Postinc ops modify their pointer operand in place: drop every temp
     * they touch, then treat the op as opaque. */
    if (q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC)
    {
      for (int s = 0; s < 3; s++)
      {
        IROperand o;
        if (s == 0 && irop_config[q->op].has_dest)
          o = tcc_ir_op_get_dest(ir, q);
        else if (s == 1 && irop_config[q->op].has_src1)
          o = tcc_ir_op_get_src1(ir, q);
        else if (s == 2 && irop_config[q->op].has_src2)
          o = tcc_ir_op_get_src2(ir, q);
        else
          continue;
        int32_t vr = irop_get_vreg(o);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
            TCCIR_DECODE_VREG_POSITION(vr) < ntemp)
          f.gen[TCCIR_DECODE_VREG_POSITION(vr)] = 0;
      }
      continue;
    }

    /* Whole-op fold first: the only legal fold when src2 carries a
     * barrel-shift annotation (a raw immediate there would skip the shift),
     * and it saves a fold round otherwise. */
    if (tbc_foldable_binop(q->op) && irop_config[q->op].has_dest &&
        irop_config[q->op].has_src1 && irop_config[q->op].has_src2)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && !d.is_lval &&
          irop_get_btype(d) == IROP_BTYPE_INT32)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        uint8_t ann = tcc_ir_barrel_shift_at(ir, q);
        int32_t a1, a2;
        int64_t res;
        if (!(ann && irop_is_immediate(s2)) && /* imm+annotation is malformed: leave it */
            tbc_resolve(ir, s1, &f, &a1) && tbc_resolve(ir, s2, &f, &a2))
        {
          if (ann)
            a2 = tbc_apply_barrel(a2, ann);
          if (ssa_opt_eval_binary(q->op, (int64_t)a1, (int64_t)a2, &res, 0))
          {
            int32_t v1 = irop_get_vreg(s1), v2 = irop_get_vreg(s2);
            if (v1 >= 0)
            {
              IRSSAVregInfo *vi1 = ssa_opt_vinfo(ctx, v1);
              if (vi1)
                ssa_opt_remove_use_instr(vi1, i);
            }
            if (v2 >= 0)
            {
              IRSSAVregInfo *vi2 = ssa_opt_vinfo(ctx, v2);
              if (vi2)
                ssa_opt_remove_use_instr(vi2, i);
            }
            q->op = TCCIR_OP_ASSIGN;
            tcc_ir_set_src1(ir, i, irop_make_imm32(0, (int32_t)res, IROP_BTYPE_INT32));
            tcc_ir_set_src2(ir, i, IROP_NONE);
            changes++;
            /* falls through: the def-update below records the new fact */
          }
        }
      }
    }

    /* Substitute known constants into pure INT32 value reads.  Control ops
     * that interpret the vreg specially keep their register form. */
    if (q->op != TCCIR_OP_SWITCH_TABLE && q->op != TCCIR_OP_LEA)
    {
      for (int s = 0; s < 2; s++)
      {
        if (s == 0 && !irop_config[q->op].has_src1)
          continue;
        if (s == 1 && !irop_config[q->op].has_src2)
          continue;
        /* A barrel-shift annotation makes src2 a shifted register: the
         * immediate would bypass the shift. */
        if (s == 1 && tcc_ir_barrel_shift_at(ir, q))
          continue;
        IROperand o = (s == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        int32_t vr = irop_get_vreg(o);
        if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
          continue;
        if (irop_get_tag(o) != IROP_TAG_VREG || o.is_lval || o.is_llocal)
          continue;
        if (irop_get_btype(o) != IROP_BTYPE_INT32)
          continue;
        int32_t cv;
        if (!tbc_lookup(&f, TCCIR_DECODE_VREG_POSITION(vr), &cv))
          continue;

        IROperand imm = irop_make_imm32(0, cv, irop_get_btype(o));
        imm.is_unsigned = o.is_unsigned;
        IRSSAVregInfo *ovi = ssa_opt_vinfo(ctx, vr);
        if (ovi)
          ssa_opt_remove_use_instr(ovi, i);
        if (s == 0)
          tcc_ir_set_src1(ir, i, imm);
        else
          tcc_ir_set_src2(ir, i, imm);
        changes++;
      }
    }

    /* Update facts from defs: only a non-lval TEMP dest changes the value. */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && !d.is_lval)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos < ntemp)
        {
          f.gen[pos] = 0;
          if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) &&
              irop_get_btype(d) == IROP_BTYPE_INT32)
          {
            int32_t cv;
            if (tbc_imm32_of(ir, tcc_ir_op_get_src1(ir, q), &cv))
            {
              f.gen[pos] = f.cur_gen;
              f.value[pos] = cv;
            }
          }
        }
      }
    }
  }

  tcc_free(f.gen);
  tcc_free(f.value);
  tcc_free(leader);
  return changes;
}
