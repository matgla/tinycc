/*
 *  TCC IR - Store-load forwarding: local + global (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include <limits.h>

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "opt_loop_utils.h"



/* Returns 1 if all uses of target_vr from start_idx need only the low load_bits bits (BB-scoped). */
static int sl_fwd_narrow_demand_only(TCCIRState *ir, int32_t target_vr, int start_idx, int load_bits, int depth)
{
  if (depth > 6)
    return 0;
  if (target_vr < 0)
    return 0;
  uint32_t mask_lim = (load_bits >= 32) ? 0xFFFFFFFFu : ((1u << load_bits) - 1);
  int n = ir->next_instruction_index;
  int found_use = 0;
  for (int i = start_idx; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (i > start_idx && q->is_jump_target)
      return 0; /* BB end — conservative */
    if (q->op == TCCIR_OP_NOP)
      continue;

    int reads_target = 0;
    if (irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(s1) == target_vr)
        reads_target = 1;
    }
    if (!reads_target && irop_config[q->op].has_src2)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_get_vreg(s2) == target_vr)
        reads_target = 1;
    }
    if (!reads_target && q->op == TCCIR_OP_MLA)
    {
      IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (irop_get_vreg(acc) == target_vr)
        reads_target = 1;
    }

    /* Redefinition of target_vr: further uses refer to a new value. */
    int redefines = 0;
    if (reads_target == 0 && irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!d.is_lval && irop_get_vreg(d) == target_vr)
        redefines = 1;
    }

    /* Terminators / control flow without a use end the scan. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_RETURNVOID)
    {
      if (reads_target)
        return 0;
      return found_use ? 1 : 0;
    }

    if (!reads_target)
    {
      if (redefines)
        return found_use ? 1 : 0;
      continue;
    }

    found_use = 1;

    switch (q->op)
    {
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    {
      /* STORE_INDEXED width comes from src1; plain STORE from dest. */
      int access_btype;
      if (q->op == TCCIR_OP_STORE_INDEXED)
        access_btype = irop_get_btype(tcc_ir_op_get_src1(ir, q));
      else
        access_btype = irop_get_btype(tcc_ir_op_get_dest(ir, q));
      int store_bits = 0;
      switch (access_btype)
      {
      case IROP_BTYPE_INT8:  store_bits = 8; break;
      case IROP_BTYPE_INT16: store_bits = 16; break;
      case IROP_BTYPE_INT32: case IROP_BTYPE_FLOAT32: store_bits = 32; break;
      case IROP_BTYPE_INT64: case IROP_BTYPE_FLOAT64: store_bits = 64; break;
      default: break;
      }
      /* Only narrow if target_vr is the stored value (src1), not the address. */
      IROperand stored_src = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(stored_src) != target_vr)
        return 0;
      if (store_bits > 0 && store_bits <= load_bits)
        break;
      return 0;
    }
    case TCCIR_OP_AND:
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (!irop_is_immediate(s2))
        return 0;
      uint64_t imm = (uint64_t)(uint32_t)(int32_t)irop_get_imm64_ex(ir, s2);
      if ((imm & ~(uint64_t)mask_lim) != 0)
        return 0;
      /* AND with byte-fitting mask zeroes high bits — dest demand inherits ours. */
      IROperand dst = tcc_ir_op_get_dest(ir, q);
      int32_t dst_vr = irop_get_vreg(dst);
      if (dst_vr < 0)
        return 0;
      if (!sl_fwd_narrow_demand_only(ir, dst_vr, i + 1, load_bits, depth + 1))
        return 0;
      break;
    }
    case TCCIR_OP_OR:
    case TCCIR_OP_XOR:
    {
      /* Per-bit ops: operand demand equals dest demand. */
      IROperand dst = tcc_ir_op_get_dest(ir, q);
      int32_t dst_vr = irop_get_vreg(dst);
      if (dst_vr < 0)
        return 0;
      if (!sl_fwd_narrow_demand_only(ir, dst_vr, i + 1, load_bits, depth + 1))
        return 0;
      break;
    }
    case TCCIR_OP_ASSIGN:
    {
      IROperand dst = tcc_ir_op_get_dest(ir, q);
      int32_t dst_vr = irop_get_vreg(dst);
      if (dst_vr < 0)
        return 0;
      if (!sl_fwd_narrow_demand_only(ir, dst_vr, i + 1, load_bits, depth + 1))
        return 0;
      break;
    }
    default:
      return 0;
    }

    if (redefines)
      return 1;
  }
  return found_use ? 1 : 0;
}

/* A variadic call with argc>4 spills args to a stack area of unknown size; forwarding across it is unsound. */
static int ir_has_stack_arg_variadic_call(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *cq = &ir->compact_instructions[i];
    if (cq->op != TCCIR_OP_FUNCCALLVOID && cq->op != TCCIR_OP_FUNCCALLVAL)
      continue;
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, cq));
    if (!callee || !callee->type.ref || callee->type.ref->f.func_type != FUNC_ELLIPSIS)
      continue;
    int argc = TCCIR_DECODE_CALL_ARGC((int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, cq)));
    if (argc > 4)
      return 1;
  }
  return 0;
}

static int tcc_ir_opt_sl_forward__timed(TCCIRState *ir);
int tcc_ir_opt_sl_forward(TCCIRState *ir)
{
  if (tcc_ir_opt_pass_disabled("sl_forward")) return 0;
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_sl_forward__timed(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_sl_forward__timed(ir);
  tcc_pass_timing_add("sl_forward", tcc_pass_clk_us() - _t);
  return _r;
}
static int tcc_ir_opt_sl_forward__timed(TCCIRState *ir)
{
  typedef struct StoreEntry
  {
    int valid;
    int addr_addrtaken;     /* 1 if address of this local is taken */
    int addr_via_pointer;   /* 1 if store was resolved through LEA map (pointer) */
    int64_t local_offset;   /* stack offset or symref addend */
    const Sym *local_sym;   /* symbol for VT_LOCAL (NULL for pure stack offsets) */
    IROperand stored_value;
    int instruction_idx;    /* where the store happened */
    int store_dest_vr;      /* vreg of the store destination (address) */
    int store_btype;        /* btype of the store address (access width) */
    struct StoreEntry *next;
  } StoreEntry;

  /* Detects a LOAD address vreg written after a matching store (the forward is then stale). */
  typedef struct
  {
    int last_write_idx; /* instruction index of last write, -1 if none */
    int gen;            /* generation counter, valid only if gen == current_gen */
  } VregWriteTracker;

  int n = ir->next_instruction_index;
  int changes = 0;
  int i;
  IRQuadCompact *q;
  StoreEntry *hash_table[128];
  StoreEntry *entries;
  int entry_count;

  /* Track stores whose loads were forwarded — candidates for dead-store elim. */
#define SL_FWD_MAX_DEAD_STORES 256
  struct
  {
    int store_idx;
    int64_t offset;
    const Sym *sym;
  } fwd_stores[SL_FWD_MAX_DEAD_STORES];
  int fwd_store_count = 0;

  if (n == 0)
    return 0;
  /* Bail on a variadic body or a stack-arg variadic call (unsound forwarding). */
  if (ir->is_variadic || ir_has_stack_arg_variadic_call(ir))
    return 0;

  /* Recompute is_jump_target from actual jumps (stale flags block forwarding) and pred_count[t]. */
  int *pred_count = tcc_mallocz(sizeof(int) * n);
  {
    uint8_t *actual_targets = tcc_mallocz((n + 7) / 8);
    for (i = 0; i < n; i++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[i];
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, jq);
        int target = (int)dest.u.imm32;
        if (target >= 0 && target < n)
        {
          actual_targets[target / 8] |= (1 << (target % 8));
          pred_count[target]++;
        }
      }
      /* Switch case/default targets are predecessors too; count them for correct pred_count. */
      else if (jq->op == TCCIR_OP_SWITCH_TABLE)
      {
        IROperand src2 = tcc_ir_op_get_src2(ir, jq);
        int table_id = (int)irop_get_imm64_ex(ir, src2);
        if (table_id >= 0 && table_id < ir->num_switch_tables)
        {
          TCCIRSwitchTable *table = &ir->switch_tables[table_id];
          for (int j = 0; j < table->num_entries; j++)
          {
            int t = table->targets[j];
            if (t >= 0 && t < n)
            {
              actual_targets[t / 8] |= (1 << (t % 8));
              pred_count[t]++;
            }
          }
          if (table->default_target >= 0 && table->default_target < n)
          {
            actual_targets[table->default_target / 8] |= (1 << (table->default_target % 8));
            pred_count[table->default_target]++;
          }
        }
      }
    }
    /* Fall-through: i+1 reached from i unless i is a terminator. */
    for (i = 0; i + 1 < n; i++)
    {
      IRQuadCompact *fq = &ir->compact_instructions[i];
      if (fq->op != TCCIR_OP_JUMP && fq->op != TCCIR_OP_RETURNVALUE && fq->op != TCCIR_OP_RETURNVOID &&
          fq->op != TCCIR_OP_SWITCH_TABLE && fq->op != TCCIR_OP_IJUMP)
        pred_count[i + 1]++;
    }
    /* Instruction 0 is always a function entry — has an implicit predecessor. */
    if (n > 0)
      pred_count[0]++;
    for (i = 0; i < n; i++)
    {
      int is_actual = (actual_targets[i / 8] & (1 << (i % 8))) != 0;
      if (ir->compact_instructions[i].is_jump_target && !is_actual)
        ir->compact_instructions[i].is_jump_target = 0;
      else if (!ir->compact_instructions[i].is_jump_target && is_actual)
        ir->compact_instructions[i].is_jump_target = 1;
    }
    tcc_free(actual_targets);
  }

  memset(hash_table, 0, sizeof(hash_table));
  entries = tcc_malloc(sizeof(StoreEntry) * n);
  entry_count = 0;

  /* Generation counter avoids clearing write trackers on block boundaries. */
  int write_tracker_gen = 1;
  int max_var = ir->next_local_variable;
  int max_tmp = ir->next_temporary_variable;
  int max_par = ir->next_parameter;
  VregWriteTracker *var_writes = tcc_mallocz(sizeof(VregWriteTracker) * (max_var + 1));
  VregWriteTracker *tmp_writes = tcc_mallocz(sizeof(VregWriteTracker) * (max_tmp + 1));
  VregWriteTracker *par_writes = tcc_mallocz(sizeof(VregWriteTracker) * (max_par + 1));

  /* LEA map: TEMPs holding addresses of stack locals, to resolve derefs back to StackLoc refs. */
  typedef struct
  {
    int64_t offset; /* resolved stack offset */
    const Sym *sym; /* local symbol (NULL for anonymous) */
    int valid;      /* 1 if entry is valid */
    int lea_idx;    /* instruction index of the LEA that created this entry */
  } LeaMapEntry;

  LeaMapEntry *lea_map = tcc_mallocz(sizeof(LeaMapEntry) * (max_tmp + 1));

  /* Addrtaken stack slots (taken via LEA/address-of); stores here must be invalidated across calls (address may escape). */
  typedef struct
  {
    int64_t offset;
    const Sym *sym;
    int earliest_lea_idx;   /* lowest instruction index of any LEA for this slot */
    int64_t max_access_end; /* upper bound (exclusive) of accessed range from this base */
  } AddrTakenSlot;
  int addrtaken_cap = 16;
  int addrtaken_count = 0;
  AddrTakenSlot *addrtaken_slots = tcc_malloc(sizeof(AddrTakenSlot) * addrtaken_cap);

  /* VAR LEA map: single-def VARs holding stack addresses, to forward through the VAR hand-off. */
  LeaMapEntry *var_lea_map = tcc_mallocz(sizeof(LeaMapEntry) * (max_var + 1));
  uint8_t *var_def_count = tcc_mallocz(max_var + 1);

  /* Count VAR defs (cap 2); VAR STORE dests are is_lval=1 but still definitions. */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *cq = &ir->compact_instructions[i];
    if (cq->op == TCCIR_OP_NOP || !irop_config[cq->op].has_dest)
      continue;
    IROperand cd = tcc_ir_op_get_dest(ir, cq);
    int32_t cdv = irop_get_vreg(cd);
    if (cdv < 0 || TCCIR_DECODE_VREG_TYPE(cdv) != TCCIR_VREG_TYPE_VAR)
      continue;
    int cdp = TCCIR_DECODE_VREG_POSITION(cdv);
    if (cdp <= max_var && var_def_count[cdp] < 2)
      var_def_count[cdp]++;
  }

  /* Active call_ids: a FUNCPARAMVAL with no matching FUNCCALL is dead (inlined) and creates no addrtaken entry. */
  uint8_t *active_call_ids = NULL;
  int max_call_id = ir->next_call_id;
  if (max_call_id > 0)
  {
    active_call_ids = tcc_mallocz((max_call_id + 7) / 8);
    for (i = 0; i < n; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op == TCCIR_OP_FUNCCALLVOID || cq->op == TCCIR_OP_FUNCCALLVAL)
      {
        IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
        int cid = (int)((uint32_t)(int32_t)irop_get_imm64_ex(ir, cs2) >> 16);
        if (cid >= 0 && cid < max_call_id)
          active_call_ids[cid / 8] |= (1 << (cid % 8));
      }
    }
  }

  /* Pre-scan: build LEA map from LEA and LEA+ADD patterns */
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *lq = &ir->compact_instructions[i];
    if (lq->op == TCCIR_OP_LEA)
    {
      IROperand ldest = tcc_ir_op_get_dest(ir, lq);
      IROperand lsrc1 = tcc_ir_op_get_src1(ir, lq);
      int32_t d_vr = irop_get_vreg(ldest);
      /* Only record concrete stack addresses; VAR-tagged operands read offset=0 and would collide. */
      if (lsrc1.is_local && !lsrc1.is_lval && d_vr >= 0 &&
          (TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP ||
           TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR))
      {
        int dest_is_var = (TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR);
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        int src_tag = irop_get_tag(lsrc1);
        int32_t src_vr = irop_get_vreg(lsrc1);
        if ((dest_is_var ? (tmp_pos <= max_var && var_def_count[tmp_pos] == 1) : (tmp_pos <= max_tmp)))
        {
          const Sym *lsym = NULL;
          int64_t loff = 0;
          int resolved = 0;
          if (src_tag == IROP_TAG_SYMREF)
          {
            IRPoolSymref *sr = irop_get_symref_ex(ir, lsrc1);
            lsym = sr ? sr->sym : NULL;
            loff = sr ? sr->addend : 0;
            resolved = 1;
          }
          else if (src_vr >= 0)
          {
            /* VAR/PARAM local u.imm32 is unresolved; use the allocated stack slot to avoid collision. */
            const TCCStackSlot *slot = tcc_ir_stack_slot_by_vreg(ir, src_vr);
            if (slot)
            {
              loff = slot->offset;
              resolved = 1;
            }
          }
          else if (src_tag == IROP_TAG_STACKOFF)
          {
            /* Anonymous stack slot (no vreg): u.imm32 holds the frame offset. */
            loff = irop_get_stack_offset(lsrc1);
            resolved = 1;
          }
          if (resolved && dest_is_var)
          {
            var_lea_map[tmp_pos].offset = loff;
            var_lea_map[tmp_pos].sym = lsym;
            var_lea_map[tmp_pos].valid = 1;
            var_lea_map[tmp_pos].lea_idx = i;
          }
          if (resolved && !dest_is_var)
          {
            lea_map[tmp_pos].offset = loff;
            lea_map[tmp_pos].sym = lsym;
            lea_map[tmp_pos].valid = 1;
            lea_map[tmp_pos].lea_idx = i;
          }
          if (resolved)
          {
            /* Mark addrtaken; record earliest LEA idx so CALL-time invalidation fires only after the LEA. */
            int already = 0;
            for (int k = 0; k < addrtaken_count; k++)
            {
              if (addrtaken_slots[k].sym == lsym && addrtaken_slots[k].offset == loff)
              {
                already = 1;
                if (i < addrtaken_slots[k].earliest_lea_idx)
                  addrtaken_slots[k].earliest_lea_idx = i;
                break;
              }
            }
            if (!already)
            {
              if (addrtaken_count >= addrtaken_cap)
              {
                addrtaken_cap *= 2;
                addrtaken_slots = tcc_realloc(addrtaken_slots, sizeof(AddrTakenSlot) * addrtaken_cap);
              }
              addrtaken_slots[addrtaken_count].sym = lsym;
              addrtaken_slots[addrtaken_count].offset = loff;
              addrtaken_slots[addrtaken_count].earliest_lea_idx = i;
              addrtaken_slots[addrtaken_count].max_access_end = loff + 4;
              addrtaken_count++;
            }
          }
        }
      }
    }
    else if (lq->op == TCCIR_OP_ADD)
    {
      IROperand ldest = tcc_ir_op_get_dest(ir, lq);
      IROperand lsrc1 = tcc_ir_op_get_src1(ir, lq);
      IROperand lsrc2 = tcc_ir_op_get_src2(ir, lq);
      int32_t d_vr = irop_get_vreg(ldest);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int dest_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (dest_pos <= max_tmp)
        {
          /* LEA_temp + const; require !is_lval — a DEREF source (`*T + c`) is a value, not the address T. */
          int32_t s1_vr = irop_get_vreg(lsrc1);
          int32_t s2_vr = irop_get_vreg(lsrc2);
          if (s1_vr >= 0 && !lsrc1.is_lval && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP &&
              irop_is_immediate(lsrc2) && !lsrc2.is_sym)
          {
            int s1_pos = TCCIR_DECODE_VREG_POSITION(s1_vr);
            if (s1_pos <= max_tmp && lea_map[s1_pos].valid)
            {
              lea_map[dest_pos].offset = lea_map[s1_pos].offset + irop_get_imm64_ex(ir, lsrc2);
              lea_map[dest_pos].sym = lea_map[s1_pos].sym;
              lea_map[dest_pos].valid = 1;
              lea_map[dest_pos].lea_idx = lea_map[s1_pos].lea_idx;
            }
          }
          /* Check: constant + LEA_temp (ADD is commutative) */
          else if (s2_vr >= 0 && !lsrc2.is_lval && TCCIR_DECODE_VREG_TYPE(s2_vr) == TCCIR_VREG_TYPE_TEMP &&
                   irop_is_immediate(lsrc1) && !lsrc1.is_sym)
          {
            int s2_pos = TCCIR_DECODE_VREG_POSITION(s2_vr);
            if (s2_pos <= max_tmp && lea_map[s2_pos].valid)
            {
              lea_map[dest_pos].offset = lea_map[s2_pos].offset + irop_get_imm64_ex(ir, lsrc1);
              lea_map[dest_pos].sym = lea_map[s2_pos].sym;
              lea_map[dest_pos].valid = 1;
              lea_map[dest_pos].lea_idx = lea_map[s2_pos].lea_idx;
            }
          }
          /* Check: StackAddr + constant (address-of stack slot plus offset) */
          else if (lsrc1.is_local && !lsrc1.is_lval && irop_get_tag(lsrc1) == IROP_TAG_STACKOFF &&
                   irop_is_immediate(lsrc2) && !lsrc2.is_sym)
          {
            int64_t loff = irop_get_stack_offset(lsrc1) + irop_get_imm64_ex(ir, lsrc2);
            lea_map[dest_pos].offset = loff;
            lea_map[dest_pos].sym = NULL;
            lea_map[dest_pos].valid = 1;
            lea_map[dest_pos].lea_idx = i;
            int already = 0;
            for (int k = 0; k < addrtaken_count; k++)
            {
              if (addrtaken_slots[k].sym == NULL && addrtaken_slots[k].offset == loff)
              {
                already = 1;
                if (i < addrtaken_slots[k].earliest_lea_idx)
                  addrtaken_slots[k].earliest_lea_idx = i;
                break;
              }
            }
            if (!already)
            {
              if (addrtaken_count >= addrtaken_cap)
              {
                addrtaken_cap *= 2;
                addrtaken_slots = tcc_realloc(addrtaken_slots, sizeof(AddrTakenSlot) * addrtaken_cap);
              }
              addrtaken_slots[addrtaken_count].sym = NULL;
              addrtaken_slots[addrtaken_count].offset = loff;
              addrtaken_slots[addrtaken_count].earliest_lea_idx = i;
              addrtaken_slots[addrtaken_count].max_access_end = loff + 4;
              addrtaken_count++;
            }
          }
          /* Check: constant + StackAddr (commutative) */
          else if (lsrc2.is_local && !lsrc2.is_lval && irop_get_tag(lsrc2) == IROP_TAG_STACKOFF &&
                   irop_is_immediate(lsrc1) && !lsrc1.is_sym)
          {
            int64_t loff = irop_get_stack_offset(lsrc2) + irop_get_imm64_ex(ir, lsrc1);
            lea_map[dest_pos].offset = loff;
            lea_map[dest_pos].sym = NULL;
            lea_map[dest_pos].valid = 1;
            lea_map[dest_pos].lea_idx = i;
            int already = 0;
            for (int k = 0; k < addrtaken_count; k++)
            {
              if (addrtaken_slots[k].sym == NULL && addrtaken_slots[k].offset == loff)
              {
                already = 1;
                if (i < addrtaken_slots[k].earliest_lea_idx)
                  addrtaken_slots[k].earliest_lea_idx = i;
                break;
              }
            }
            if (!already)
            {
              if (addrtaken_count >= addrtaken_cap)
              {
                addrtaken_cap *= 2;
                addrtaken_slots = tcc_realloc(addrtaken_slots, sizeof(AddrTakenSlot) * addrtaken_cap);
              }
              addrtaken_slots[addrtaken_count].sym = NULL;
              addrtaken_slots[addrtaken_count].offset = loff;
              addrtaken_slots[addrtaken_count].earliest_lea_idx = i;
              addrtaken_slots[addrtaken_count].max_access_end = loff + 4;
              addrtaken_count++;
            }
          }
        }
      }
    }
    else if (lq->op == TCCIR_OP_STORE || lq->op == TCCIR_OP_ASSIGN)
    {
      /* VAR <- TEMP_in_lea_map [STORE|ASSIGN], single-def: record VAR's LEA address. */
      IROperand ldest = tcc_ir_op_get_dest(ir, lq);
      IROperand lsrc1 = tcc_ir_op_get_src1(ir, lq);
      int32_t d_vr = irop_get_vreg(ldest);
      int32_t s_vr = irop_get_vreg(lsrc1);
      int dest_ok = (lq->op == TCCIR_OP_STORE) ? 1 : !ldest.is_lval;
      if (dest_ok && d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR && !lsrc1.is_lval && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int vp = TCCIR_DECODE_VREG_POSITION(d_vr);
        int sp = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (vp <= max_var && sp <= max_tmp && lea_map[sp].valid && var_def_count[vp] == 1)
        {
          var_lea_map[vp].offset = lea_map[sp].offset;
          var_lea_map[vp].sym = lea_map[sp].sym;
          var_lea_map[vp].valid = 1;
          var_lea_map[vp].lea_idx = lea_map[sp].lea_idx;
        }
      }
      /* TEMP <-- VAR_in_var_lea_map [ASSIGN]: propagate VAR's LEA to TEMP. */
      if (lq->op == TCCIR_OP_ASSIGN && !ldest.is_lval && d_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        int vp = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (dp <= max_tmp && vp <= max_var && var_lea_map[vp].valid)
        {
          lea_map[dp].offset = var_lea_map[vp].offset;
          lea_map[dp].sym = var_lea_map[vp].sym;
          lea_map[dp].valid = 1;
          lea_map[dp].lea_idx = var_lea_map[vp].lea_idx;
        }
      }
      /* TEMP <- Addr[StackLoc] [ASSIGN]: direct stack address assigned to a TEMP. */
      if (lq->op == TCCIR_OP_ASSIGN && !ldest.is_lval && d_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP &&
          lsrc1.is_local && !lsrc1.is_lval && irop_get_tag(lsrc1) == IROP_TAG_STACKOFF)
      {
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (dp <= max_tmp)
        {
          int64_t loff = irop_get_stack_offset(lsrc1);
          lea_map[dp].offset = loff;
          lea_map[dp].sym = NULL;
          lea_map[dp].valid = 1;
          lea_map[dp].lea_idx = i;
          int already = 0;
          for (int k = 0; k < addrtaken_count; k++)
          {
            if (addrtaken_slots[k].sym == NULL && addrtaken_slots[k].offset == loff)
            {
              already = 1;
              if (i < addrtaken_slots[k].earliest_lea_idx)
                addrtaken_slots[k].earliest_lea_idx = i;
              break;
            }
          }
          if (!already)
          {
            if (addrtaken_count >= addrtaken_cap)
            {
              addrtaken_cap *= 2;
              addrtaken_slots = tcc_realloc(addrtaken_slots, sizeof(AddrTakenSlot) * addrtaken_cap);
            }
            addrtaken_slots[addrtaken_count].sym = NULL;
            addrtaken_slots[addrtaken_count].offset = loff;
            addrtaken_slots[addrtaken_count].earliest_lea_idx = i;
            addrtaken_slots[addrtaken_count].max_access_end = loff + 4;
            addrtaken_count++;
          }
        }
      }
    }

    /* Direct-address escape: a src carrying Addr[StackLoc] takes the slot's address (e.g. FUNCPARAMVAL). */
    {
      const IRRegistersConfig *cfg = &irop_config[lq->op];
      for (int src_i = 0; src_i < 2; src_i++)
      {
        if (src_i == 0 && !cfg->has_src1)
          continue;
        if (src_i == 1 && !cfg->has_src2)
          continue;
        IROperand src = (src_i == 0) ? tcc_ir_op_get_src1(ir, lq) : tcc_ir_op_get_src2(ir, lq);
        if (irop_get_tag(src) != IROP_TAG_STACKOFF)
          continue;
        if (src.is_lval || src.is_llocal)
          continue;
        /* STACKOFF with a vreg: offset is in the spill slot, not u.imm32 — tracked via the LEA map. */
        if (irop_get_vreg(src) != -1)
          continue;
        /* Skip FUNCPARAMVAL with dead call_ids (from inlined calls). */
        if (lq->op == TCCIR_OP_FUNCPARAMVAL && active_call_ids)
        {
          IROperand ps2 = tcc_ir_op_get_src2(ir, lq);
          int pcid = (int)((uint32_t)(int32_t)irop_get_imm64_ex(ir, ps2) >> 16);
          if (pcid >= 0 && pcid < max_call_id && !(active_call_ids[pcid / 8] & (1 << (pcid % 8))))
            continue;
        }
        int64_t off = irop_get_stack_offset(src);
        int already = 0;
        for (int k = 0; k < addrtaken_count; k++)
        {
          if (addrtaken_slots[k].sym == NULL && addrtaken_slots[k].offset == off)
          {
            already = 1;
            if (i < addrtaken_slots[k].earliest_lea_idx)
              addrtaken_slots[k].earliest_lea_idx = i;
            break;
          }
        }
        if (!already)
        {
          if (addrtaken_count >= addrtaken_cap)
          {
            addrtaken_cap *= 2;
            addrtaken_slots = tcc_realloc(addrtaken_slots, sizeof(AddrTakenSlot) * addrtaken_cap);
          }
          addrtaken_slots[addrtaken_count].sym = NULL;
          addrtaken_slots[addrtaken_count].offset = off;
          addrtaken_slots[addrtaken_count].earliest_lea_idx = i;
          addrtaken_slots[addrtaken_count].max_access_end = off + 4;
          addrtaken_count++;
        }
      }
    }
  }

  /* Extend each addrtaken slot's max_access_end from derived LEA+ADD offsets. */
  for (int t = 0; t <= max_tmp; t++)
  {
    if (!lea_map[t].valid)
      continue;
    int64_t derived_off = lea_map[t].offset;
    const Sym *derived_sym = lea_map[t].sym;
    int64_t access_end = derived_off + 4;
    for (int k = 0; k < addrtaken_count; k++)
    {
      if (addrtaken_slots[k].sym != derived_sym)
        continue;
      if (addrtaken_slots[k].offset > derived_off)
        continue;
      if (access_end > addrtaken_slots[k].max_access_end)
        addrtaken_slots[k].max_access_end = access_end;
      break;
    }
  }

  /* Track constants assigned to TEMPs so later stores use the resolved constant. */
  IROperand *fwd_tmp_val = tcc_mallocz(sizeof(IROperand) * (max_tmp + 1));
  uint8_t *fwd_tmp_valid = tcc_mallocz(max_tmp + 1);

  /* Pre-populate fwd_tmp from constant ASSIGN/LOAD; reject multiply-defined TEMPs (no single reaching def). */
  uint8_t *fwd_tmp_defs = tcc_mallocz(max_tmp + 1);
  for (i = 0; i < n; i++)
  {
    IRQuadCompact *aq = &ir->compact_instructions[i];
    int has_temp_dest = 0;
    int apos = -1;
    if (irop_config[aq->op].has_dest && aq->op != TCCIR_OP_STORE && aq->op != TCCIR_OP_NOP)
    {
      IROperand adest = tcc_ir_op_get_dest(ir, aq);
      int32_t adest_vr = irop_get_vreg(adest);
      if (adest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(adest_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        apos = TCCIR_DECODE_VREG_POSITION(adest_vr);
        if (apos <= max_tmp)
          has_temp_dest = 1;
      }
    }
    if (!has_temp_dest)
      continue;

    /* Count definitions; if >1, permanently invalidate */
    if (fwd_tmp_defs[apos] < 2)
      fwd_tmp_defs[apos]++;
    if (fwd_tmp_defs[apos] > 1)
    {
      fwd_tmp_valid[apos] = 0;
      continue;
    }

    /* Single-def ASSIGN/LOAD with immediate constant — track it. */
    if (aq->op == TCCIR_OP_ASSIGN || aq->op == TCCIR_OP_LOAD)
    {
      IROperand asrc1 = tcc_ir_op_get_src1(ir, aq);
      if (irop_is_immediate(asrc1) && !asrc1.is_sym && !asrc1.is_lval && !asrc1.is_local)
      {
        fwd_tmp_val[apos] = asrc1;
        fwd_tmp_valid[apos] = 1;
        continue;
      }
    }
  }
  /* Consumers re-check fwd_tmp_defs[t]<2 so a multiply-defined temp is never substituted for a merge. */

  /* Snapshot state at JUMP/JUMPIF to saved_entries[target] for cross-BB forwarding. */
  StoreEntry **saved_entries = tcc_mallocz(sizeof(StoreEntry *) * n);
  int *saved_entry_count = tcc_mallocz(sizeof(int) * n);
  /* Per-target snapshot capacity, grown lazily to avoid O(targets*n) memory. */
  int *saved_entry_cap = tcc_mallocz(sizeof(int) * n);

  LOG_IR_GEN("=== STORE-LOAD FORWARDING START ===");

  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];

    /* JUMP/JUMPIF snapshot target state; JUMP ends the path, JUMPIF keeps state for fall-through. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand jdest = tcc_ir_op_get_dest(ir, q);
      int jtarget = (int)jdest.u.imm32;
      if (jtarget >= 0 && jtarget < n && entry_count > 0)
      {
        /* Snapshot entries[] for the target, growing the slot to entry_count. */
        if (saved_entry_cap[jtarget] < entry_count)
        {
          saved_entries[jtarget] = tcc_realloc(saved_entries[jtarget], sizeof(StoreEntry) * entry_count);
          saved_entry_cap[jtarget] = entry_count;
        }
        memcpy(saved_entries[jtarget], entries, sizeof(StoreEntry) * entry_count);
        saved_entry_count[jtarget] = entry_count;
      }
      if (q->op == TCCIR_OP_JUMP)
      {
        /* No fall-through — clear state */
        memset(hash_table, 0, sizeof(hash_table));
        entry_count = 0;
        write_tracker_gen++;
      }
      /* JUMPIF: keep current state for the fall-through path */
      continue;
    }
    if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_IJUMP)
    {
      /* Terminators with no fall-through: drop tracked stores (next instruction is a fresh BB). */
      memset(hash_table, 0, sizeof(hash_table));
      entry_count = 0;
      write_tracker_gen++;
      continue;
    }
    if (q->is_jump_target)
    {
      /* Multi-pred: reset. Single-pred via JUMP: restore snapshot; via fall-through: keep state. */
      if (pred_count[i] > 1)
      {
        /* pred==2 join: intersect snapshot with fall-through state (diamond pattern). */
        if (pred_count[i] == 2 && saved_entries[i] && entry_count > 0)
        {
          int sc = saved_entry_count[i];
          int new_count = 0;
          for (int j = 0; j < entry_count; j++)
          {
            if (!entries[j].valid)
              continue;
            int found = 0;
            for (int k = 0; k < sc; k++)
            {
              if (!saved_entries[i][k].valid)
                continue;
              if (saved_entries[i][k].instruction_idx == entries[j].instruction_idx &&
                  saved_entries[i][k].local_sym == entries[j].local_sym &&
                  saved_entries[i][k].local_offset == entries[j].local_offset)
              {
                found = 1;
                break;
              }
            }
            if (found)
            {
              /* Drop stores aliasable through an addrtaken pointer (escaped base). */
              if (entries[j].addr_addrtaken || entries[j].addr_via_pointer)
                found = 0;
              if (found)
              {
                for (int ak = 0; ak < addrtaken_count; ak++)
                {
                  if (addrtaken_slots[ak].sym != entries[j].local_sym)
                    continue;
                  if (addrtaken_slots[ak].earliest_lea_idx > i)
                    continue;
                  /* Same-sym addrtaken slot: assume the escaped pointer reaches this store. */
                  found = 0;
                  break;
                }
              }
            }
            if (found)
            {
              if (new_count != j)
                entries[new_count] = entries[j];
              new_count++;
            }
          }
          LOG_SL_FWD("BB@i=%d JOIN: multi-pred (preds=2), kept %d of %d entries (snapshot had %d)", i, new_count,
                     entry_count, sc);
          entry_count = new_count;
          memset(hash_table, 0, sizeof(hash_table));
          for (int j = 0; j < entry_count; j++)
          {
            uint32_t h = ((uintptr_t)entries[j].local_sym * 31 + (uint32_t)entries[j].local_offset * 17) % 128;
            entries[j].next = hash_table[h];
            hash_table[h] = &entries[j];
          }
        }
        else
        {
          LOG_SL_FWD("BB@i=%d RESET: multi-pred target (preds=%d) — dropping %d tracked stores", i, pred_count[i],
                     entry_count);
          memset(hash_table, 0, sizeof(hash_table));
          entry_count = 0;
          write_tracker_gen++;
        }
      }
      else if (pred_count[i] == 1)
      {
        int prev_is_terminator = 0;
        if (i > 0)
        {
          int pop = ir->compact_instructions[i - 1].op;
          prev_is_terminator = (pop == TCCIR_OP_JUMP || pop == TCCIR_OP_RETURNVALUE || pop == TCCIR_OP_RETURNVOID);
        }
        if (prev_is_terminator)
        {
          /* Single-pred target reached only via JUMP: reset — a snapshot restore is not a real data-flow join. */
          LOG_SL_FWD("BB@i=%d RESET: single-pred terminator target (dropped %d entries)", i, entry_count);
          memset(hash_table, 0, sizeof(hash_table));
          entry_count = 0;
          write_tracker_gen++;
        }
        /* else: fall-through predecessor — keep current state intact */
      }
    }
    /* Calls only invalidate addrtaken stores whose LEA appeared at/before this call. */
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      /* Pure AEABI calls don't modify memory — tracked stores stay valid. */
      int is_pure_aeabi = 0;
      {
        IROperand csrc = tcc_ir_op_get_src1(ir, q);
        Sym *call_sym = irop_get_sym_ex(ir, csrc);
        if (call_sym)
        {
          const char *cname = get_tok_str(call_sym->v, NULL);
          LOG_SL_FWD("CALL@i=%d callee=%s (sym=%p)", i, cname ? cname : "(null)", (void *)call_sym);
          if (cname && cname[0] == '_' && cname[1] == '_')
            is_pure_aeabi = tcc_ir_is_pure_aeabi(cname);
        }
        else
        {
          LOG_SL_FWD("CALL@i=%d callee=NULL (tag=%d vr=%d is_sym=%d)", i, irop_get_tag(csrc), irop_get_vreg(csrc),
                     csrc.is_sym);
        }
        if (is_pure_aeabi)
          LOG_SL_FWD("CALL@i=%d PURE — skipping invalidation", i);
      }

      int j;
      if (!is_pure_aeabi)
      {
        for (j = 0; j < entry_count; j++)
        {
          if (!entries[j].valid)
            continue;
          if (entries[j].addr_addrtaken || entries[j].addr_via_pointer)
          {
            LOG_SL_FWD("CALL@i=%d INVALIDATE: store@i=%d sym=%p off=%lld (addrtaken/via_ptr)", i,
                       entries[j].instruction_idx, (const void *)entries[j].local_sym,
                       (long long)entries[j].local_offset);
            entries[j].valid = 0;
            continue;
          }
          /* Named locals: match by sym (ptr arithmetic reaches any offset). Anonymous: use stack overlap. */
          for (int k = 0; k < addrtaken_count; k++)
          {
            if (addrtaken_slots[k].sym != entries[j].local_sym || addrtaken_slots[k].earliest_lea_idx > i)
              continue;

            if (entries[j].local_sym == NULL)
            {
              int64_t escaped_base, escaped_end;
              int store_size = ir_opt_store_btype_size_bytes(entries[j].store_btype);
              int64_t store_base = entries[j].local_offset;
              int64_t store_end = store_base + (store_size > 0 ? store_size : 1);

              if (!ir_opt_stack_slot_range_for_offset(ir, addrtaken_slots[k].offset, &escaped_base, &escaped_end))
              {
                if (addrtaken_slots[k].max_access_end > addrtaken_slots[k].offset + 4)
                {
                  escaped_base = addrtaken_slots[k].offset;
                  escaped_end = addrtaken_slots[k].max_access_end;
                }
                else
                {
                  LOG_SL_FWD("CALL@i=%d INVALIDATE: store@i=%d sym=%p off=%lld (unknown anonymous slot range)", i,
                             entries[j].instruction_idx, (const void *)entries[j].local_sym,
                             (long long)entries[j].local_offset);
                  entries[j].valid = 0;
                  break;
                }
              }
              if (store_base >= escaped_end || store_end <= escaped_base)
                continue;
            }

            {
              LOG_SL_FWD("CALL@i=%d INVALIDATE: store@i=%d sym=%p off=%lld (earliest_lea@%d <= call)", i,
                         entries[j].instruction_idx, (const void *)entries[j].local_sym,
                         (long long)entries[j].local_offset, addrtaken_slots[k].earliest_lea_idx);
              entries[j].valid = 0;
              break;
            }
          }
        }
      }
      /* FUNCCALLVAL redefines its dest vreg — invalidate stores of that vreg and track the write. */
      if (q->op == TCCIR_OP_FUNCCALLVAL)
      {
        IROperand call_dest = tcc_ir_op_get_dest(ir, q);
        int32_t call_dest_vr = irop_get_vreg(call_dest);
        if (call_dest_vr >= 0)
        {
          for (j = 0; j < entry_count; j++)
          {
            if (entries[j].valid && irop_get_vreg(entries[j].stored_value) == call_dest_vr)
              entries[j].valid = 0;
          }
          if (!call_dest.is_lval)
          {
            int vr_type = TCCIR_DECODE_VREG_TYPE(call_dest_vr);
            int vr_pos = TCCIR_DECODE_VREG_POSITION(call_dest_vr);
            VregWriteTracker *tracker = NULL;
            if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
              tracker = &var_writes[vr_pos];
            else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
              tracker = &tmp_writes[vr_pos];
            else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
              tracker = &par_writes[vr_pos];
            if (tracker)
            {
              tracker->last_write_idx = i;
              tracker->gen = write_tracker_gen;
            }
          }
        }
      }
      continue;
    }

    /* Forward into LOAD, ASSIGN-with-deref, and single-scalar FUNCPARAMVAL (not complex, not VAR). */
    if (q->op == TCCIR_OP_LOAD ||
        (q->op == TCCIR_OP_ASSIGN && tcc_ir_op_get_src1(ir, q).is_lval &&
         !(irop_get_vreg(tcc_ir_op_get_src1(ir, q)) >= 0 &&
           TCCIR_DECODE_VREG_TYPE(irop_get_vreg(tcc_ir_op_get_src1(ir, q))) == TCCIR_VREG_TYPE_VAR)) ||
        (q->op == TCCIR_OP_FUNCPARAMVAL && tcc_ir_op_get_src1(ir, q).is_local && tcc_ir_op_get_src1(ir, q).is_lval &&
         !tcc_ir_op_get_src1(ir, q).is_complex &&
         !(irop_get_vreg(tcc_ir_op_get_src1(ir, q)) >= 0 &&
           TCCIR_DECODE_VREG_TYPE(irop_get_vreg(tcc_ir_op_get_src1(ir, q))) == TCCIR_VREG_TYPE_VAR)))
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t addr_vr = irop_get_vreg(src1);
      const Sym *addr_sym;
      int64_t addr_offset;
      uint32_t h;
      StoreEntry *e;

      /* Only forward for stack locals or LEA-resolvable non-locals. */
      int load_via_lea = 0;
      if (!src1.is_local)
      {
        if (src1.is_lval)
        {
          int32_t lv = irop_get_vreg(src1);
          if (lv >= 0 && TCCIR_DECODE_VREG_TYPE(lv) == TCCIR_VREG_TYPE_TEMP)
          {
            int lp = TCCIR_DECODE_VREG_POSITION(lv);
            if (lp <= max_tmp && lea_map[lp].valid)
            {
              addr_sym = lea_map[lp].sym;
              addr_offset = lea_map[lp].offset;
              load_via_lea = 1;
              goto resolved_local_load;
            }
            LOG_SL_FWD("LOAD@i=%d SKIP: TMP:%d src1 is_lval but no LEA-map entry", i, lp);
          }
          else
          {
            LOG_SL_FWD("LOAD@i=%d SKIP: src1 is_lval but not a TEMP (vr=%d)", i, lv);
          }
        }
        else
        {
          LOG_SL_FWD("LOAD@i=%d SKIP: src1 not is_local and not is_lval", i);
        }
        continue;
      }

      /* Skip if address taken (may alias through a pointer). */
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
        {
          LOG_SL_FWD("LOAD@i=%d SKIP: addr vreg:%d addrtaken", i, addr_vr);
          continue;
        }
      }

      /* Extract sym and offset from the local address operand. */
      if (irop_get_tag(src1) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *sr = irop_get_symref_ex(ir, src1);
        addr_sym = sr ? sr->sym : NULL;
        addr_offset = sr ? sr->addend : 0;
      }
      else
      {
        addr_sym = NULL;
        addr_offset = irop_get_imm64_ex(ir, src1);
      }

    resolved_local_load:
      /* Stamp direct VAR loads with a per-position sentinel sym so they don't alias an anonymous StackLoc (every VAR reads offset 0 here). */
      if (!load_via_lea && addr_vr >= 0 && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
        addr_sym = (const Sym *)(uintptr_t)(1 + (unsigned)TCCIR_DECODE_VREG_POSITION(addr_vr));

      h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;

      LOG_SL_FWD("LOAD@i=%d PROBE: sym=%p off=%lld btype=%d via_lea=%d | tag=%d vr_type=%d pos=%d is_local=%d "
                 "is_lval=%d is_llocal=%d is_sym=%d u.imm32=%d",
                 i, (const void *)addr_sym, (long long)addr_offset, (int)src1.btype, load_via_lea,
                 (int)irop_get_tag(src1), (int)TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src1)),
                 (int)TCCIR_DECODE_VREG_POSITION(irop_get_vreg(src1)), (int)src1.is_local, (int)src1.is_lval,
                 (int)src1.is_llocal, (int)src1.is_sym, (int)src1.u.imm32);

      int matched_any = 0;
      int rejected_width = 0;
      int rejected_stale_tracker = 0;
      int rejected_invalid = 0;

      /* addrtaken entries stay valid within a BB until a CALL or unknown-pointer STORE clears them. */
      for (e = hash_table[h]; e != NULL; e = e->next)
      {
        if (!e->valid)
        {
          if (e->local_sym == addr_sym && e->local_offset == addr_offset)
            rejected_invalid++;
          continue;
        }

        /* Both are stack locals - match on symbol and offset */
        if (e->local_sym == addr_sym && e->local_offset == addr_offset)
        {
          matched_any++;
          /* Width check: differing widths don't forward, except a narrower load of a wider constant store. */
          if (e->store_btype != src1.btype)
          {
            int store_bits = 0, load_bits = 0;
            switch (e->store_btype)
            {
            case IROP_BTYPE_INT8:
              store_bits = 8;
              break;
            case IROP_BTYPE_INT16:
              store_bits = 16;
              break;
            case IROP_BTYPE_INT32:
            case IROP_BTYPE_FLOAT32:
              store_bits = 32;
              break;
            case IROP_BTYPE_INT64:
            case IROP_BTYPE_FLOAT64:
              store_bits = 64;
              break;
            default:
              store_bits = 0;
              break;
            }
            switch (src1.btype)
            {
            case IROP_BTYPE_INT8:
              load_bits = 8;
              break;
            case IROP_BTYPE_INT16:
              load_bits = 16;
              break;
            case IROP_BTYPE_INT32:
            case IROP_BTYPE_FLOAT32:
              load_bits = 32;
              break;
            case IROP_BTYPE_INT64:
            case IROP_BTYPE_FLOAT64:
              load_bits = 64;
              break;
            default:
              load_bits = 0;
              break;
            }
            /* Same-size cross-type punning: immediates only, addr_vr<0 only (VAR stack reuse could be stale); re-pool 64-bit. */
            if (store_bits > 0 && store_bits == load_bits && irop_is_immediate(e->stored_value) && addr_vr < 0)
            {
              IROperand fwd = e->stored_value;
              int sv_tag = irop_get_tag(e->stored_value);
              int translated = 1;
              if (sv_tag == IROP_TAG_I64 && src1.btype == IROP_BTYPE_FLOAT64)
              {
                uint64_t bits = (uint64_t)irop_get_imm64_ex(ir, e->stored_value);
                uint32_t new_idx = tcc_ir_pool_add_f64(ir, bits);
                fwd = irop_make_f64(-1, new_idx);
              }
              else if (sv_tag == IROP_TAG_F64 && src1.btype == IROP_BTYPE_INT64)
              {
                int64_t val = irop_get_imm64_ex(ir, e->stored_value);
                uint32_t new_idx = tcc_ir_pool_add_i64(ir, val);
                fwd = irop_make_i64(-1, new_idx, IROP_BTYPE_INT64);
              }
              else if (sv_tag == IROP_TAG_IMM32 && src1.btype == IROP_BTYPE_FLOAT32)
              {
                fwd.tag = IROP_TAG_F32;
                fwd.btype = IROP_BTYPE_FLOAT32;
              }
              else if (sv_tag == IROP_TAG_F32 && src1.btype == IROP_BTYPE_INT32)
              {
                fwd.tag = IROP_TAG_IMM32;
                fwd.btype = IROP_BTYPE_INT32;
              }
              else
              {
                translated = 0;
              }
              if (translated)
              {
                LOG_SL_FWD("LOAD@i=%d FORWARD-PUN: store@i=%d store_btype=%d load_btype=%d bits=%d", i,
                           e->instruction_idx, (int)e->store_btype, (int)src1.btype, store_bits);
                if (q->op != TCCIR_OP_FUNCPARAMVAL)
                  q->op = TCCIR_OP_ASSIGN;
                int pool_off = q->operand_base + irop_config[q->op].has_dest;
                ir->iroperand_pool[pool_off] = fwd;
                {
                  IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
                  int32_t fwd_dest_vr = irop_get_vreg(fwd_dest);
                  if (fwd_dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr) == TCCIR_VREG_TYPE_TEMP)
                  {
                    int fwd_pos = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr);
                    if (fwd_pos <= max_tmp)
                    {
                      fwd_tmp_val[fwd_pos] = fwd;
                      fwd_tmp_valid[fwd_pos] = 1;
                    }
                  }
                }
                if (fwd_store_count < SL_FWD_MAX_DEAD_STORES && !e->addr_addrtaken &&
                    irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[e->instruction_idx])) < 0)
                {
                  fwd_stores[fwd_store_count].store_idx = e->instruction_idx;
                  fwd_stores[fwd_store_count].offset = e->local_offset;
                  fwd_stores[fwd_store_count].sym = e->local_sym;
                  fwd_store_count++;
                }
                changes++;
                break;
              }
            }
            if (store_bits > 0 && load_bits > 0 && store_bits > load_bits && irop_is_immediate(e->stored_value))
            {
              int64_t full64 = irop_get_imm64_ex(ir, e->stored_value);
              uint32_t mask = (load_bits == 32) ? 0xFFFFFFFFu : ((1u << load_bits) - 1);
              int32_t full = (int32_t)(uint32_t)full64;
              int32_t narrow = (int32_t)((uint32_t)full & mask);
              /* Sign-extend if the load is signed and narrower than 32. */
              if (!src1.is_unsigned && load_bits < 32)
              {
                int shift = 32 - load_bits;
                narrow = (int32_t)((uint32_t)narrow << shift);
                narrow = narrow >> shift; /* arithmetic shift preserves sign */
              }
              LOG_SL_FWD("LOAD@i=%d FORWARD-MASK: store@i=%d store_bits=%d load_bits=%d full=%d narrow=%d", i,
                         e->instruction_idx, store_bits, load_bits, full, narrow);
              /* Replace LOAD with ASSIGN of the masked const; keep FUNCPARAMVAL, replace deref src1. */
              if (q->op != TCCIR_OP_FUNCPARAMVAL)
                q->op = TCCIR_OP_ASSIGN;
              int pool_off = q->operand_base + irop_config[q->op].has_dest;
              ir->iroperand_pool[pool_off] = irop_make_imm32(-1, narrow, src1.btype);
              /* Track forwarded value for transitive forwarding. */
              {
                IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
                int32_t fwd_dest_vr = irop_get_vreg(fwd_dest);
                if (fwd_dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr) == TCCIR_VREG_TYPE_TEMP)
                {
                  int fwd_pos = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr);
                  if (fwd_pos <= max_tmp)
                  {
                    fwd_tmp_val[fwd_pos] = irop_make_imm32(-1, narrow, src1.btype);
                    fwd_tmp_valid[fwd_pos] = 1;
                  }
                }
              }
              changes++;
              break;
            }
            LOG_SL_FWD("LOAD@i=%d REJECT width: store btype=%d vs load btype=%d (store at i=%d)", i,
                       (int)e->store_btype, (int)src1.btype, e->instruction_idx);
            rejected_width++;
            continue;
          }

          /* Sub-32-bit same-width forward: a wider stored constant needs masking unless demand analysis proves only low N bits are used. */
          if (irop_is_immediate(e->stored_value))
          {
            int load_bits_n = 0;
            switch (src1.btype)
            {
            case IROP_BTYPE_INT8:  load_bits_n = 8; break;
            case IROP_BTYPE_INT16: load_bits_n = 16; break;
            default: break;
            }
            if (load_bits_n > 0)
            {
              int64_t full64_n = irop_get_imm64_ex(ir, e->stored_value);
              uint32_t mask_n = (1u << load_bits_n) - 1;
              int32_t narrow_n = (int32_t)((uint32_t)full64_n & mask_n);
              if (!src1.is_unsigned)
              {
                int shift_n = 32 - load_bits_n;
                narrow_n = (int32_t)((uint32_t)narrow_n << shift_n);
                narrow_n = narrow_n >> shift_n;
              }
              if ((int32_t)full64_n != narrow_n)
              {
                /* Wider stored value vs. what the LOAD would produce. */
                IROperand load_dest = tcc_ir_op_get_dest(ir, q);
                int32_t load_dest_vr = irop_get_vreg(load_dest);
                int narrow_safe = 0;
                if (load_dest_vr >= 0)
                  narrow_safe = sl_fwd_narrow_demand_only(ir, load_dest_vr, i + 1, load_bits_n, 0);
                LOG_SL_FWD("LOAD@i=%d NARROW-DEMAND-CHECK: dest_vr=%d safe=%d", i, load_dest_vr, narrow_safe);
                if (!narrow_safe)
                {
                  LOG_SL_FWD("LOAD@i=%d FORWARD-NARROW-IMM-MASK: store@i=%d load_bits=%d narrow=%d", i,
                             e->instruction_idx, load_bits_n, narrow_n);
                  if (q->op != TCCIR_OP_FUNCPARAMVAL)
                    q->op = TCCIR_OP_ASSIGN;
                  int pool_off_n = q->operand_base + irop_config[q->op].has_dest;
                  ir->iroperand_pool[pool_off_n] = irop_make_imm32(-1, narrow_n, src1.btype);
                  {
                    IROperand fwd_dest_n = tcc_ir_op_get_dest(ir, q);
                    int32_t fwd_dest_vr_n = irop_get_vreg(fwd_dest_n);
                    if (fwd_dest_vr_n >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr_n) == TCCIR_VREG_TYPE_TEMP)
                    {
                      int fwd_pos_n = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr_n);
                      if (fwd_pos_n <= max_tmp)
                      {
                        fwd_tmp_val[fwd_pos_n] = irop_make_imm32(-1, narrow_n, src1.btype);
                        fwd_tmp_valid[fwd_pos_n] = 1;
                      }
                    }
                  }
                  if (fwd_store_count < SL_FWD_MAX_DEAD_STORES && !e->addr_addrtaken &&
                      irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[e->instruction_idx])) < 0)
                  {
                    fwd_stores[fwd_store_count].store_idx = e->instruction_idx;
                    fwd_stores[fwd_store_count].offset = e->local_offset;
                    fwd_stores[fwd_store_count].sym = e->local_sym;
                    fwd_store_count++;
                  }
                  changes++;
                  break;
                }
              }
            }
          }

          /* Stale check: skip if the LOAD's address vreg was written after the store (unless LEA-resolved). */
          if (!load_via_lea && addr_vr >= 0)
          {
            int vr_type = TCCIR_DECODE_VREG_TYPE(addr_vr);
            int vr_pos = TCCIR_DECODE_VREG_POSITION(addr_vr);
            VregWriteTracker *tracker = NULL;
            if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
              tracker = &var_writes[vr_pos];
            else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
              tracker = &tmp_writes[vr_pos];
            else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
              tracker = &par_writes[vr_pos];
            if (tracker && tracker->gen == write_tracker_gen && tracker->last_write_idx > e->instruction_idx)
            {
              LOG_SL_FWD("LOAD@i=%d REJECT stale tracker: addr vr=%d last_write=%d > store@i=%d", i, addr_vr,
                         tracker->last_write_idx, e->instruction_idx);
              rejected_stale_tracker++;
              continue;
            }
          }
#ifdef TCC_REGALLOC_DEBUG
          fprintf(stderr,
                  "[SL-FWD] i=%d LOAD replaced by ASSIGN from store at i=%d, stored_vr=0x%x, load_addr_vr=0x%x, "
                  "offset=%lld\n",
                  i, e->instruction_idx, irop_get_vreg(e->stored_value), addr_vr, (long long)addr_offset);
#endif
          LOG_SL_FWD("LOAD@i=%d FORWARD from store at i=%d", i, e->instruction_idx);
          /* FUNCPARAMVAL: replace deref src1; LOAD: convert to ASSIGN. */
          if (q->op != TCCIR_OP_FUNCPARAMVAL)
            q->op = TCCIR_OP_ASSIGN;
          int pool_off = q->operand_base + irop_config[q->op].has_dest;
          ir->iroperand_pool[pool_off] = e->stored_value;
          /* Track the assigned value for transitive forwarding. */
          {
            IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
            int32_t fwd_dest_vr = irop_get_vreg(fwd_dest);
            if (fwd_dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int fwd_pos = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr);
              if (fwd_pos <= max_tmp)
              {
                IROperand fwd_sv = e->stored_value;
                /* Transitive resolution: resolve a fwd_tmp TEMP to its underlying constant. */
                int32_t fwd_sv_vr = irop_get_vreg(fwd_sv);
                if (fwd_sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_sv_vr) == TCCIR_VREG_TYPE_TEMP && !fwd_sv.is_lval)
                {
                  int fwd_sv_pos = TCCIR_DECODE_VREG_POSITION(fwd_sv_vr);
                  if (fwd_sv_pos <= max_tmp && fwd_tmp_valid[fwd_sv_pos] && fwd_tmp_defs[fwd_sv_pos] < 2)
                    fwd_sv = fwd_tmp_val[fwd_sv_pos];
                }
                fwd_tmp_val[fwd_pos] = fwd_sv;
                fwd_tmp_valid[fwd_pos] = 1;
                /* Propagate LEA map from a stored-value TEMP to the dest TEMP. */
                {
                  int32_t sv_vr = irop_get_vreg(e->stored_value);
                  if (sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(sv_vr) == TCCIR_VREG_TYPE_TEMP && !e->stored_value.is_lval)
                  {
                    int sv_pos = TCCIR_DECODE_VREG_POSITION(sv_vr);
                    if (sv_pos <= max_tmp && lea_map[sv_pos].valid)
                    {
                      lea_map[fwd_pos].offset = lea_map[sv_pos].offset;
                      lea_map[fwd_pos].sym = lea_map[sv_pos].sym;
                      lea_map[fwd_pos].valid = 1;
                    }
                  }
                }
              }
            }
          }
          /* Record store as a dead-store-elim candidate. */
          if (fwd_store_count < SL_FWD_MAX_DEAD_STORES && !e->addr_addrtaken &&
              irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[e->instruction_idx])) < 0)
          {
            fwd_stores[fwd_store_count].store_idx = e->instruction_idx;
            fwd_stores[fwd_store_count].offset = e->local_offset;
            fwd_stores[fwd_store_count].sym = e->local_sym;
            fwd_store_count++;
          }
          changes++;
          break;
        }
      }
      /* 32-bit load may read the upper half of a 64-bit constant store at X-4. */
      if (e == NULL && src1.btype == IROP_BTYPE_INT32)
      {
        int64_t lo_offset = addr_offset - 4;
        uint32_t lo_h = ((uintptr_t)addr_sym * 31 + (uint32_t)lo_offset * 17) % 128;
        StoreEntry *lo_e;
        for (lo_e = hash_table[lo_h]; lo_e != NULL; lo_e = lo_e->next)
        {
          if (!lo_e->valid || lo_e->local_sym != addr_sym || lo_e->local_offset != lo_offset)
            continue;
          int lo_tag = irop_get_tag(lo_e->stored_value);
          if (lo_e->store_btype != IROP_BTYPE_INT64 || !irop_is_immediate(lo_e->stored_value) ||
              (lo_tag != IROP_TAG_I64 && lo_tag != IROP_TAG_F64))
            continue;
          int64_t full64 = irop_get_imm64_ex(ir, lo_e->stored_value);
          int32_t upper = (int32_t)(uint32_t)(full64 >> 32);
          LOG_SL_FWD("LOAD@i=%d FORWARD-HI: store@i=%d upper32=%d from 64-bit val", i, lo_e->instruction_idx, upper);
          if (q->op != TCCIR_OP_FUNCPARAMVAL)
            q->op = TCCIR_OP_ASSIGN;
          {
            int pool_off = q->operand_base + irop_config[q->op].has_dest;
            ir->iroperand_pool[pool_off] = irop_make_imm32(-1, upper, src1.btype);
          }
          {
            IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
            int32_t fwd_dest_vr = irop_get_vreg(fwd_dest);
            if (fwd_dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int fwd_pos = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr);
              if (fwd_pos <= max_tmp)
              {
                fwd_tmp_val[fwd_pos] = irop_make_imm32(-1, upper, src1.btype);
                fwd_tmp_valid[fwd_pos] = 1;
              }
            }
          }
          changes++;
          break;
        }
        e = lo_e;
      }
      /* 64-bit load from two adjacent 32-bit const stores: STORE32 [X], STORE32 [X+4]. */
      if (e == NULL && src1.btype == IROP_BTYPE_INT64)
      {
        int64_t hi_offset = addr_offset + 4;
        uint32_t lo_h2 = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;
        uint32_t hi_h2 = ((uintptr_t)addr_sym * 31 + (uint32_t)hi_offset * 17) % 128;
        StoreEntry *lo_e2 = NULL, *hi_e2 = NULL;
        for (StoreEntry *p = hash_table[lo_h2]; p != NULL; p = p->next)
        {
          if (!p->valid || p->local_sym != addr_sym || p->local_offset != addr_offset)
            continue;
          if (p->store_btype != IROP_BTYPE_INT32 || !irop_is_immediate(p->stored_value))
            continue;
          lo_e2 = p;
          break;
        }
        for (StoreEntry *p = hash_table[hi_h2]; p != NULL; p = p->next)
        {
          if (!p->valid || p->local_sym != addr_sym || p->local_offset != hi_offset)
            continue;
          if (p->store_btype != IROP_BTYPE_INT32 || !irop_is_immediate(p->stored_value))
            continue;
          hi_e2 = p;
          break;
        }
        if (lo_e2 && hi_e2)
        {
          int stale = 0;
          if (!load_via_lea && addr_vr >= 0)
          {
            int vr_type = TCCIR_DECODE_VREG_TYPE(addr_vr);
            int vr_pos = TCCIR_DECODE_VREG_POSITION(addr_vr);
            VregWriteTracker *tracker = NULL;
            if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
              tracker = &var_writes[vr_pos];
            else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
              tracker = &tmp_writes[vr_pos];
            else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
              tracker = &par_writes[vr_pos];
            int latest = lo_e2->instruction_idx > hi_e2->instruction_idx
                             ? lo_e2->instruction_idx
                             : hi_e2->instruction_idx;
            if (tracker && tracker->gen == write_tracker_gen && tracker->last_write_idx > latest)
              stale = 1;
          }
          if (!stale)
          {
            int64_t lo_val = irop_get_imm64_ex(ir, lo_e2->stored_value);
            int64_t hi_val = irop_get_imm64_ex(ir, hi_e2->stored_value);
            int64_t combined = (int64_t)(((uint64_t)(uint32_t)hi_val << 32) | (uint32_t)lo_val);
            /* Restrict to int32-fit values (avoids 64-bit narrowing-assign bugs). */
            if (combined >= INT32_MIN && combined <= INT32_MAX)
            {
              LOG_SL_FWD("LOAD@i=%d FORWARD-COMBINE64: lo_store@i=%d (val=%d) hi_store@i=%d (val=%d) -> %lld", i,
                         lo_e2->instruction_idx, (int)lo_val, hi_e2->instruction_idx, (int)hi_val, (long long)combined);
              if (q->op != TCCIR_OP_FUNCPARAMVAL)
                q->op = TCCIR_OP_ASSIGN;
              IROperand new_val = irop_make_imm32(-1, (int32_t)combined, src1.btype);
              int pool_off = q->operand_base + irop_config[q->op].has_dest;
              ir->iroperand_pool[pool_off] = new_val;
              {
                IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
                int32_t fwd_dest_vr = irop_get_vreg(fwd_dest);
                if (fwd_dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr) == TCCIR_VREG_TYPE_TEMP)
                {
                  int fwd_pos = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr);
                  if (fwd_pos <= max_tmp)
                  {
                    fwd_tmp_val[fwd_pos] = new_val;
                    fwd_tmp_valid[fwd_pos] = 1;
                  }
                }
              }
              e = lo_e2;
              changes++;
            }
          }
        }
      }
      /* Sub-byte/short LOAD extracting a byte from a wider const STORE at a lower offset. */
      if (e == NULL && (src1.btype == IROP_BTYPE_INT8 || src1.btype == IROP_BTYPE_INT16))
      {
        int load_bytes = (src1.btype == IROP_BTYPE_INT8) ? 1 : 2;
        for (int delta = 1; delta <= 7 && e == NULL; delta++)
        {
          int64_t prev_off = addr_offset - delta;
          uint32_t prev_h = ((uintptr_t)addr_sym * 31 + (uint32_t)prev_off * 17) % 128;
          StoreEntry *prev_e;
          for (prev_e = hash_table[prev_h]; prev_e != NULL; prev_e = prev_e->next)
          {
            if (!prev_e->valid || prev_e->local_sym != addr_sym || prev_e->local_offset != prev_off)
              continue;
            int entry_bytes = 0;
            switch (prev_e->store_btype)
            {
            case IROP_BTYPE_INT16:
              entry_bytes = 2;
              break;
            case IROP_BTYPE_INT32:
              entry_bytes = 4;
              break;
            default:
              break;
            }
            if (entry_bytes <= delta || delta + load_bytes > entry_bytes)
              continue;
            if (!irop_is_immediate(prev_e->stored_value))
              continue;
            /* Same staleness check as the regular path. */
            int stale = 0;
            if (!load_via_lea && addr_vr >= 0)
            {
              int vr_type = TCCIR_DECODE_VREG_TYPE(addr_vr);
              int vr_pos = TCCIR_DECODE_VREG_POSITION(addr_vr);
              VregWriteTracker *tracker = NULL;
              if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
                tracker = &var_writes[vr_pos];
              else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
                tracker = &tmp_writes[vr_pos];
              else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
                tracker = &par_writes[vr_pos];
              if (tracker && tracker->gen == write_tracker_gen &&
                  tracker->last_write_idx > prev_e->instruction_idx)
                stale = 1;
            }
            if (stale)
              continue;
            /* Use irop_get_imm64_ex: I64/F64 immediates hold a pool index in u.imm32, not the value. */
            uint32_t full = (uint32_t)irop_get_imm64_ex(ir, prev_e->stored_value);
            uint32_t bit_shift = (uint32_t)delta * 8;
            uint32_t byte_mask = (load_bytes == 1) ? 0xFFu : 0xFFFFu;
            int32_t narrow = (int32_t)((full >> bit_shift) & byte_mask);
            if (!src1.is_unsigned)
            {
              int shift = 32 - load_bytes * 8;
              narrow = (int32_t)((uint32_t)narrow << shift);
              narrow = narrow >> shift;
            }
            LOG_SL_FWD("LOAD@i=%d FORWARD-SUBBYTE: store@i=%d delta=%d entry_bytes=%d "
                       "load_bytes=%d full=0x%x narrow=%d sv_tag=%d sv_islval=%d sv_islocal=%d",
                       i, prev_e->instruction_idx, delta, entry_bytes, load_bytes, full, narrow,
                       (int)irop_get_tag(prev_e->stored_value), (int)prev_e->stored_value.is_lval,
                       (int)prev_e->stored_value.is_local);
            if (q->op != TCCIR_OP_FUNCPARAMVAL)
              q->op = TCCIR_OP_ASSIGN;
            int pool_off = q->operand_base + irop_config[q->op].has_dest;
            ir->iroperand_pool[pool_off] = irop_make_imm32(-1, narrow, src1.btype);
            {
              IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
              int32_t fwd_dest_vr = irop_get_vreg(fwd_dest);
              if (fwd_dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr) == TCCIR_VREG_TYPE_TEMP)
              {
                int fwd_pos = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr);
                if (fwd_pos <= max_tmp)
                {
                  fwd_tmp_val[fwd_pos] = irop_make_imm32(-1, narrow, src1.btype);
                  fwd_tmp_valid[fwd_pos] = 1;
                }
              }
            }
            e = prev_e;
            changes++;
            break;
          }
        }
      }
      if (e == NULL)
      {
        LOG_SL_FWD("LOAD@i=%d NOMATCH: sym=%p off=%lld matched=%d width_rej=%d stale_rej=%d invalid_rej=%d", i,
                   (const void *)addr_sym, (long long)addr_offset, matched_any, rejected_width, rejected_stale_tracker,
                   rejected_invalid);
      }
    }
    /* LOAD_INDEXED with LEA-mapped base + const index: resolve to base_offset+imm and forward. */
    else if (q->op == TCCIR_OP_LOAD_INDEXED)
    {
      IROperand li_src1 = tcc_ir_op_get_src1(ir, q);
      IROperand li_src2 = tcc_ir_op_get_src2(ir, q);
      int32_t base_vr = irop_get_vreg(li_src1);
      if (base_vr >= 0 && TCCIR_DECODE_VREG_TYPE(base_vr) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(li_src2) &&
          !li_src2.is_sym)
      {
        int bp = TCCIR_DECODE_VREG_POSITION(base_vr);
        if (bp <= max_tmp && lea_map[bp].valid)
        {
          /* Apply scale: address = base + (index << scale); a wrong offset would mis-match slots. */
          IROperand scale_op = ir->iroperand_pool[q->operand_base + 3];
          int scale = (int)irop_get_imm64_ex(ir, scale_op);
          int64_t eff_off = lea_map[bp].offset + (irop_get_imm64_ex(ir, li_src2) << scale);
          const Sym *eff_sym = lea_map[bp].sym;
          uint32_t lih = ((uintptr_t)eff_sym * 31 + (uint32_t)eff_off * 17) % 128;
          StoreEntry *lie;
          /* Access width is the dest's btype (src1 is the base pointer, often NONE). */
          int li_access_btype = (int)tcc_ir_op_get_dest(ir, q).btype;
          for (lie = hash_table[lih]; lie != NULL; lie = lie->next)
          {
            if (!lie->valid)
              continue;
            if (lie->local_sym != eff_sym || lie->local_offset != eff_off)
              continue;
            if (lie->store_btype != li_access_btype)
              continue;
            int li_tag = irop_get_tag(lie->stored_value);
            int li_is_const = (li_tag == IROP_TAG_IMM32 || li_tag == IROP_TAG_I64 || li_tag == IROP_TAG_STACKOFF);
            if (!li_is_const)
            {
              /* Forward a vreg value only if it's a true vreg (not lval) and not rewritten since the store. */
              if (lie->stored_value.is_lval)
                continue;
              int32_t sv_vr = irop_get_vreg(lie->stored_value);
              if (sv_vr < 0)
                continue;
              int sv_type = TCCIR_DECODE_VREG_TYPE(sv_vr);
              int sv_pos = TCCIR_DECODE_VREG_POSITION(sv_vr);
              VregWriteTracker *sv_tracker = NULL;
              if (sv_type == TCCIR_VREG_TYPE_VAR && sv_pos <= max_var)
                sv_tracker = &var_writes[sv_pos];
              else if (sv_type == TCCIR_VREG_TYPE_TEMP && sv_pos <= max_tmp)
                sv_tracker = &tmp_writes[sv_pos];
              else if (sv_type == TCCIR_VREG_TYPE_PARAM && sv_pos <= max_par)
                sv_tracker = &par_writes[sv_pos];
              if (sv_tracker && sv_tracker->gen == write_tracker_gen &&
                  sv_tracker->last_write_idx > lie->instruction_idx)
                continue;
            }
            if (li_tag == IROP_TAG_STACKOFF && lie->stored_value.is_lval)
              continue;
            q->op = TCCIR_OP_ASSIGN;
            int li_pool = q->operand_base + irop_config[TCCIR_OP_ASSIGN].has_dest;
            ir->iroperand_pool[li_pool] = lie->stored_value;
            tcc_ir_set_src2(ir, i, IROP_NONE);
            LOG_SL_FWD("LOAD_INDEXED@i=%d FORWARD: eff_off=%lld from store@i=%d", i, (long long)eff_off,
                       lie->instruction_idx);
            {
              IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
              int32_t fwd_vr = irop_get_vreg(fwd_dest);
              if (fwd_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_vr) == TCCIR_VREG_TYPE_TEMP)
              {
                int fp = TCCIR_DECODE_VREG_POSITION(fwd_vr);
                if (fp <= max_tmp)
                {
                  fwd_tmp_val[fp] = lie->stored_value;
                  fwd_tmp_valid[fp] = 1;
                  if (li_tag == IROP_TAG_STACKOFF)
                  {
                    lea_map[fp].offset = irop_get_stack_offset(lie->stored_value);
                    lea_map[fp].sym = lie->local_sym;
                    lea_map[fp].valid = 1;
                  }
                }
              }
            }
            changes++;
            break;
          }
        }
      }
    }
    /* TEST_ZERO StackLoc[X] implicitly loads; forward the tracked store into its memory operand. */
    else if (q->op == TCCIR_OP_TEST_ZERO)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t addr_vr = irop_get_vreg(src1);

      if (src1.is_local)
      {
        const Sym *addr_sym;
        int64_t addr_offset;

        /* Skip if address is taken */
        if (addr_vr >= 0)
        {
          IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
          if (interval && interval->addrtaken)
            goto skip_test_zero_fwd;
        }

        if (irop_get_tag(src1) == IROP_TAG_SYMREF)
        {
          IRPoolSymref *sr = irop_get_symref_ex(ir, src1);
          addr_sym = sr ? sr->sym : NULL;
          addr_offset = sr ? sr->addend : 0;
        }
        else
        {
          addr_sym = NULL;
          addr_offset = irop_get_imm64_ex(ir, src1);
        }

        uint32_t h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;
        StoreEntry *e;
        for (e = hash_table[h]; e != NULL; e = e->next)
        {
          if (!e->valid)
            continue;
          if (e->local_sym == addr_sym && e->local_offset == addr_offset)
          {
            if (e->store_btype != src1.btype)
              continue;
            /* Vreg write safety check (same as LOAD path) */
            if (addr_vr >= 0)
            {
              int vr_type = TCCIR_DECODE_VREG_TYPE(addr_vr);
              int vr_pos = TCCIR_DECODE_VREG_POSITION(addr_vr);
              VregWriteTracker *tracker = NULL;
              if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
                tracker = &var_writes[vr_pos];
              else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
                tracker = &tmp_writes[vr_pos];
              else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
                tracker = &par_writes[vr_pos];
              if (tracker && tracker->gen == write_tracker_gen && tracker->last_write_idx > e->instruction_idx)
                continue;
            }
            LOG_IR_GEN("OPTIMIZE: TEST_ZERO store-forward at i=%d from store at i=%d", i, e->instruction_idx);
            int pool_off = q->operand_base; /* TEST_ZERO: has_dest=0, src1 at base */
            ir->iroperand_pool[pool_off] = e->stored_value;
            changes++;
            break;
          }
        }
      }
    skip_test_zero_fwd:;
    }
    /* Forward tracked const stores into lval sources of ALU/CMP ops (later folded by const_prop). */
    else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_OR ||
             q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_SHL || q->op == TCCIR_OP_SHR || q->op == TCCIR_OP_SAR ||
             q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_CMP)
    {
      for (int si = 0; si < 2; si++)
      {
        /* Use explicit if/else, not a ternary — the self-host cross miscompiles the ternary here. */
        IROperand src;
        if (si == 0)
          src = tcc_ir_op_get_src1(ir, q);
        else
          src = tcc_ir_op_get_src2(ir, q);
        if (!src.is_lval)
          continue;
        /* Resolve address directly for locals or via the LEA map for TEMP derefs. */
        const Sym *addr_sym = NULL;
        int64_t addr_offset = 0;
        if (src.is_local && !src.is_llocal)
        {
          int src_tag = irop_get_tag(src);
          if (src_tag == IROP_TAG_SYMREF)
          {
            IRPoolSymref *sr = irop_get_symref_ex(ir, src);
            addr_sym = sr ? sr->sym : NULL;
            addr_offset = sr ? sr->addend : 0;
          }
          else if (src_tag == IROP_TAG_STACKOFF)
          {
            addr_offset = irop_get_stack_offset(src);
          }
          else
          {
            continue;
          }
          /* Skip VAR vregs: they can be redefined by ALU ops (untracked); const_prop handles those. */
          int32_t addr_vr = irop_get_vreg(src);
          if (addr_vr >= 0 && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
            continue;
          if (addr_vr >= 0)
          {
            IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
            if (interval && interval->addrtaken)
              continue;
          }
        }
        else
        {
          /* TEMP***DEREF***: resolve via LEA map to a known stack location */
          int32_t lea_vr = irop_get_vreg(src);
          if (lea_vr >= 0 && TCCIR_DECODE_VREG_TYPE(lea_vr) == TCCIR_VREG_TYPE_TEMP)
          {
            int lp = TCCIR_DECODE_VREG_POSITION(lea_vr);
            if (lp <= max_tmp && lea_map[lp].valid)
            {
              addr_sym = lea_map[lp].sym;
              addr_offset = lea_map[lp].offset;
            }
            else
              continue;
          }
          else
            continue;
        }
        uint32_t h2 = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;
        StoreEntry *e;
        for (e = hash_table[h2]; e != NULL; e = e->next)
        {
          if (!e->valid)
            continue;
          if (e->local_sym != addr_sym || e->local_offset != addr_offset)
            continue;
          /* Accept immediate (forward the constant) or vreg (replace deref with a vreg ref) stored values. */
          int sv_tag = irop_get_tag(e->stored_value);
          int32_t sv_vr = irop_get_vreg(e->stored_value);
          int sv_is_vreg = (sv_vr >= 0 && !e->stored_value.is_lval && sv_tag == IROP_TAG_VREG);
          if (!sv_is_vreg && sv_tag != IROP_TAG_IMM32 && sv_tag != IROP_TAG_I64)
            continue;

          if (sv_is_vreg && e->store_btype == src.btype)
          {
            IROperand replacement = e->stored_value;
            if (q->op == TCCIR_OP_CMP && i - e->instruction_idx > 32)
              continue;
            replacement.is_lval = 0;
            replacement.btype = src.btype;
            if (si == 0)
              tcc_ir_op_set_src1(ir, q, replacement);
            else
              tcc_ir_op_set_src2(ir, q, replacement);
            LOG_SL_FWD("ALU@i=%d FORWARD vreg: si=%d from store@i=%d", i, si, e->instruction_idx);
            changes++;
            break;
          }
          int store_bits, load_bits;
          switch (e->store_btype)
          {
          case IROP_BTYPE_INT8:
            store_bits = 8;
            break;
          case IROP_BTYPE_INT16:
            store_bits = 16;
            break;
          case IROP_BTYPE_INT32:
            store_bits = 32;
            break;
          default:
            store_bits = 0;
            break;
          }
          switch (src.btype)
          {
          case IROP_BTYPE_INT8:
            load_bits = 8;
            break;
          case IROP_BTYPE_INT16:
            load_bits = 16;
            break;
          case IROP_BTYPE_INT32:
            load_bits = 32;
            break;
          default:
            load_bits = 0;
            break;
          }
          if (store_bits <= 0 || load_bits <= 0 || store_bits < load_bits)
            continue;
          /* Extract the 64-bit value (IMM32 or I64-via-pool), then narrow to 32 bits. */
          int64_t full64 = irop_get_imm64_ex(ir, e->stored_value);
          int32_t val = (int32_t)full64;
          if (store_bits > load_bits)
          {
            uint32_t mask = (load_bits == 32) ? 0xFFFFFFFFu : ((1u << load_bits) - 1);
            val = (int32_t)((uint32_t)val & mask);
            if (!src.is_unsigned && load_bits < 32)
            {
              int shift = 32 - load_bits;
              val = (int32_t)((uint32_t)val << shift);
              val = val >> shift; /* arithmetic: preserve sign */
            }
          }
          IROperand new_op = irop_make_imm32(-1, val, src.btype);
          if (si == 0)
            tcc_ir_op_set_src1(ir, q, new_op);
          else
            tcc_ir_op_set_src2(ir, q, new_op);
          LOG_SL_FWD("ALU@i=%d FORWARD: si=%d from store@i=%d store_bits=%d load_bits=%d val=%d", i, si,
                     e->instruction_idx, store_bits, load_bits, val);
          changes++;
          break;
        }
        /* 64-bit ALU operand from two adjacent 32-bit const stores (offset, offset+4). */
        if (src.btype == IROP_BTYPE_INT64)
        {
          int64_t hi_off = addr_offset + 4;
          uint32_t lo_h64 = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;
          uint32_t hi_h64 = ((uintptr_t)addr_sym * 31 + (uint32_t)hi_off * 17) % 128;
          StoreEntry *lo_e = NULL, *hi_e = NULL;
          for (StoreEntry *p = hash_table[lo_h64]; p != NULL; p = p->next)
          {
            if (!p->valid || p->local_sym != addr_sym || p->local_offset != addr_offset)
              continue;
            if (p->store_btype != IROP_BTYPE_INT32 || !irop_is_immediate(p->stored_value))
              continue;
            lo_e = p;
            break;
          }
          for (StoreEntry *p = hash_table[hi_h64]; p != NULL; p = p->next)
          {
            if (!p->valid || p->local_sym != addr_sym || p->local_offset != hi_off)
              continue;
            if (p->store_btype != IROP_BTYPE_INT32 || !irop_is_immediate(p->stored_value))
              continue;
            hi_e = p;
            break;
          }
          if (lo_e && hi_e)
          {
            int64_t lo_val = irop_get_imm64_ex(ir, lo_e->stored_value);
            int64_t hi_val = irop_get_imm64_ex(ir, hi_e->stored_value);
            int64_t combined = (int64_t)(((uint64_t)(uint32_t)hi_val << 32) | (uint32_t)lo_val);
            /* Restrict to int32-fit values. */
            if (combined >= INT32_MIN && combined <= INT32_MAX)
            {
              IROperand new_op = irop_make_imm32(-1, (int32_t)combined, src.btype);
              if (si == 0)
                tcc_ir_op_set_src1(ir, q, new_op);
              else
                tcc_ir_op_set_src2(ir, q, new_op);
              LOG_SL_FWD("ALU@i=%d FORWARD-COMBINE64: si=%d lo@i=%d hi@i=%d -> %lld", i, si, lo_e->instruction_idx,
                         hi_e->instruction_idx, (long long)combined);
              changes++;
            }
          }
        }
      }
    }
    /* STORE_INDEXED with scale=0/imm index/LEA base: track as a plain store; otherwise blanket-invalidate. */
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    {
      int resolved_si = 0;
      if (q->op == TCCIR_OP_STORE_INDEXED)
      {
        IROperand si_dest = tcc_ir_op_get_dest(ir, q);
        IROperand si_src1 = tcc_ir_op_get_src1(ir, q);
        IROperand si_src2 = tcc_ir_op_get_src2(ir, q);
        IROperand si_scale = tcc_ir_op_get_scale(ir, q);
        int32_t base_vr = irop_get_vreg(si_dest);
        if (base_vr >= 0 && TCCIR_DECODE_VREG_TYPE(base_vr) == TCCIR_VREG_TYPE_TEMP &&
            irop_is_immediate(si_src2) && !si_src2.is_sym &&
            irop_get_tag(si_scale) == IROP_TAG_IMM32 && si_scale.u.imm32 == 0)
        {
          int bp = TCCIR_DECODE_VREG_POSITION(base_vr);
          if (bp <= max_tmp && lea_map[bp].valid)
          {
            const Sym *si_sym = lea_map[bp].sym;
            int64_t si_off = lea_map[bp].offset + irop_get_imm64_ex(ir, si_src2);

            /* Invalidate any existing entry at this exact offset (overwrite). */
            uint32_t sih = ((uintptr_t)si_sym * 31 + (uint32_t)si_off * 17) % 128;
            for (StoreEntry *sie = hash_table[sih]; sie != NULL; sie = sie->next)
            {
              if (sie->valid && sie->local_sym == si_sym && sie->local_offset == si_off)
                sie->valid = 0;
            }

            /* Access width comes from src1 (dest is a register base); default INT32. */
            int store_btype = si_src1.btype;
            if (store_btype != IROP_BTYPE_INT8 && store_btype != IROP_BTYPE_INT16 &&
                store_btype != IROP_BTYPE_INT32 && store_btype != IROP_BTYPE_INT64 &&
                store_btype != IROP_BTYPE_FLOAT32 && store_btype != IROP_BTYPE_FLOAT64)
              store_btype = IROP_BTYPE_INT32;

            /* Overlap invalidation: a narrow store kills a wider entry below; a wide store kills narrower above. */
            {
              int si_bytes = ir_opt_store_btype_size_bytes(store_btype);
              if (si_bytes <= 0)
                si_bytes = 4;
              for (int delta = 1; delta <= 7; delta++)
              {
                int64_t lo_off = si_off - delta;
                uint32_t loh = ((uintptr_t)si_sym * 31 + (uint32_t)lo_off * 17) % 128;
                for (StoreEntry *sie = hash_table[loh]; sie != NULL; sie = sie->next)
                {
                  if (!sie->valid || sie->local_sym != si_sym || sie->local_offset != lo_off)
                    continue;
                  int eb = ir_opt_store_btype_size_bytes(sie->store_btype);
                  if (eb <= 0)
                    eb = 4;
                  if (eb > delta)
                    sie->valid = 0;
                }
              }
              for (int fwd = 1; fwd < si_bytes; fwd++)
              {
                int64_t hi_off = si_off + fwd;
                uint32_t hih = ((uintptr_t)si_sym * 31 + (uint32_t)hi_off * 17) % 128;
                for (StoreEntry *sie = hash_table[hih]; sie != NULL; sie = sie->next)
                {
                  if (sie->valid && sie->local_sym == si_sym && sie->local_offset == hi_off)
                    sie->valid = 0;
                }
              }
            }

            StoreEntry *sne = &entries[entry_count++];
            sne->valid = 1;
            sne->addr_addrtaken = 0;
            sne->addr_via_pointer = 1; /* via pointer — call invalidates it */
            sne->local_offset = si_off;
            sne->local_sym = si_sym;
            sne->stored_value = si_src1;
            sne->instruction_idx = i;
            sne->store_dest_vr = -1;
            sne->store_btype = store_btype;
            sne->next = hash_table[sih];
            hash_table[sih] = sne;

            LOG_SL_FWD("STORE_INDEXED@i=%d TRACK via LEA: sym=%p off=%lld btype=%d",
                       i, (const void *)si_sym, (long long)si_off, store_btype);
            resolved_si = 1;
          }
        }
      }

      if (!resolved_si)
      {
        int j;
        for (j = 0; j < entry_count; j++)
        {
          if (entries[j].valid)
          {
            LOG_IR_GEN("STORE-LOAD: Invalidate local at i=%d due to indexed/postinc store at i=%d",
                       entries[j].instruction_idx, i);
            entries[j].valid = 0;
          }
        }
      }
    }
    /* Process STORE instructions: track them for later forwarding */
    if (q->op == TCCIR_OP_STORE)
    {
      /* Forward a tracked StackLoc value into this STORE's src1 (STORE dest <- StackLoc[X]). */
      {
        IROperand stsrc1 = tcc_ir_op_get_src1(ir, q);
        if (stsrc1.is_local && stsrc1.is_lval)
        {
          int32_t s_vr = irop_get_vreg(stsrc1);
          const Sym *s_sym;
          int64_t s_offset;
          if (irop_get_tag(stsrc1) == IROP_TAG_SYMREF)
          {
            IRPoolSymref *sr = irop_get_symref_ex(ir, stsrc1);
            s_sym = sr ? sr->sym : NULL;
            s_offset = sr ? sr->addend : 0;
          }
          else
          {
            s_sym = NULL;
            s_offset = irop_get_imm64_ex(ir, stsrc1);
          }

          /* Per-VAR sentinel sym so a VAR source doesn't alias an anonymous StackLoc (matches load side). */
          if (s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
            s_sym = (const Sym *)(uintptr_t)(1 + (unsigned)TCCIR_DECODE_VREG_POSITION(s_vr));

          int src_addrtaken = 0;
          if (s_vr >= 0)
          {
            IRLiveInterval *interval = tcc_ir_get_live_interval(ir, s_vr);
            if (interval && interval->addrtaken)
              src_addrtaken = 1;
          }

          if (!src_addrtaken)
          {
            uint32_t sh = ((uintptr_t)s_sym * 31 + (uint32_t)s_offset * 17) % 128;
            StoreEntry *se;
            for (se = hash_table[sh]; se != NULL; se = se->next)
            {
              if (!se->valid)
                continue;
              if (se->local_sym != s_sym || se->local_offset != s_offset)
                continue;
              if (se->store_btype != stsrc1.btype)
                continue;
              int32_t sv_vr = irop_get_vreg(se->stored_value);
              if (sv_vr >= 0)
              {
                int vr_type = TCCIR_DECODE_VREG_TYPE(sv_vr);
                int vr_pos = TCCIR_DECODE_VREG_POSITION(sv_vr);
                VregWriteTracker *tracker = NULL;
                if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
                  tracker = &var_writes[vr_pos];
                else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
                  tracker = &tmp_writes[vr_pos];
                else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
                  tracker = &par_writes[vr_pos];
                if (tracker && tracker->gen == write_tracker_gen &&
                    tracker->last_write_idx > se->instruction_idx)
                  continue;
              }
              LOG_SL_FWD("STORE@i=%d FORWARD src1 from store at i=%d off=%lld", i, se->instruction_idx,
                         (long long)s_offset);
              {
                int pool_off = q->operand_base + irop_config[TCCIR_OP_STORE].has_dest;
                ir->iroperand_pool[pool_off] = se->stored_value;
              }
              /* Only anonymous-slot stores (dest vreg<0) may enter DSE; a VAR-dest store also has vreg readers the scan can't see. */
              if (fwd_store_count < SL_FWD_MAX_DEAD_STORES && !se->addr_addrtaken &&
                  irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[se->instruction_idx])) < 0)
              {
                fwd_stores[fwd_store_count].store_idx = se->instruction_idx;
                fwd_stores[fwd_store_count].offset = se->local_offset;
                fwd_stores[fwd_store_count].sym = se->local_sym;
                fwd_store_count++;
              }
              changes++;
              break;
            }
          }
        }
      }

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t addr_vr = irop_get_vreg(dest);
      const Sym *addr_sym;
      int64_t addr_offset;
      int addr_addrtaken = 0;
      int addr_via_pointer = 0;
      uint32_t h;
      StoreEntry *new_entry = NULL;
      int j;

      /* Only track stack locals; a LEA-mapped pointer counts as a local store. */
      if (!dest.is_local)
      {
        /* VAR dest writes the VAR's own storage — can't alias tracked slots; skip. */
        {
          int32_t dv = irop_get_vreg(dest);
          if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
            continue;
        }
        int resolved_to_local = 0;
        if (dest.is_lval)
        {
          int32_t dv = irop_get_vreg(dest);
          if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
          {
            int dp = TCCIR_DECODE_VREG_POSITION(dv);
            if (dp <= max_tmp && lea_map[dp].valid)
            {
              /* Treat as a local store at the resolved offset; via_pointer so calls invalidate it. */
              addr_sym = lea_map[dp].sym;
              addr_offset = lea_map[dp].offset;
              addr_via_pointer = 1;
              resolved_to_local = 1;
            }
          }
        }
        if (!resolved_to_local)
        {
          /* Unknown pointer store — invalidate ALL tracked stores */
          for (j = 0; j < entry_count; j++)
          {
            if (entries[j].valid)
            {
              LOG_IR_GEN("STORE-LOAD: Invalidate local at i=%d due to pointer store at i=%d",
                         entries[j].instruction_idx, i);
              entries[j].valid = 0;
            }
          }
          continue;
        }
        goto resolved_local_store;
      }

      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
          addr_addrtaken = 1;
      }

      if (irop_get_tag(dest) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
        addr_sym = sr ? sr->sym : NULL;
        addr_offset = sr ? sr->addend : 0;
      }
      else
      {
        addr_sym = NULL;
        addr_offset = irop_get_imm64_ex(ir, dest);
      }

    resolved_local_store:
      /* Per-VAR sentinel sym (encoding vreg position) so VARs don't collide with anonymous StackLocs; must match load side. */
      if (addr_vr >= 0 && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
        addr_sym = (const Sym *)(uintptr_t)(1 + (unsigned)TCCIR_DECODE_VREG_POSITION(addr_vr));

      h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;

      /* Partial-overwrite merge: a narrower const store onto the low bits of a wider const entry merges into it (byte-overlay). */
      int new_bits_local = 0;
      int new_is_imm = (irop_get_tag(tcc_ir_op_get_src1(ir, q)) == IROP_TAG_IMM32);
      switch (dest.btype)
      {
      case IROP_BTYPE_INT8:
        new_bits_local = 8;
        break;
      case IROP_BTYPE_INT16:
        new_bits_local = 16;
        break;
      case IROP_BTYPE_INT32:
        new_bits_local = 32;
        break;
      default:
        new_bits_local = 0;
        break; /* FP/struct/64-bit: skip merge */
      }
      int merged_into_existing = 0;
      for (new_entry = hash_table[h]; new_entry != NULL; new_entry = new_entry->next)
      {
        if (new_entry->local_sym != addr_sym || new_entry->local_offset != addr_offset)
          continue;
        if (!new_entry->valid)
          continue;
        int old_bits = 0;
        switch (new_entry->store_btype)
        {
        case IROP_BTYPE_INT8:
          old_bits = 8;
          break;
        case IROP_BTYPE_INT16:
          old_bits = 16;
          break;
        case IROP_BTYPE_INT32:
          old_bits = 32;
          break;
        default:
          old_bits = 0;
          break;
        }
        int old_is_imm = (irop_get_tag(new_entry->stored_value) == IROP_TAG_IMM32);
        if (!merged_into_existing && new_is_imm && old_is_imm && old_bits > new_bits_local && new_bits_local > 0)
        {
          int32_t old_v = new_entry->stored_value.u.imm32;
          int32_t new_v = tcc_ir_op_get_src1(ir, q).u.imm32;
          uint32_t mask = (new_bits_local == 32) ? 0xFFFFFFFFu : ((1u << new_bits_local) - 1);
          int32_t merged = (int32_t)((uint32_t)old_v & ~mask) | (int32_t)((uint32_t)new_v & mask);
          new_entry->stored_value.u.imm32 = merged;
          new_entry->instruction_idx = i;
          LOG_SL_FWD("STORE@i=%d MERGE into store@i=? at off=%lld: old_bits=%d new_bits=%d old_v=%d new_v=%d merged=%d",
                     i, (long long)addr_offset, old_bits, new_bits_local, old_v, new_v, merged);
          merged_into_existing = 1;
          continue; /* don't invalidate the merged entry */
        }
        /* Not mergeable — invalidate. */
        new_entry->valid = 0;
      }
      /* Narrow store overlapping a wider const entry below: merge the new bytes instead of invalidating. */
      {
        int max_delta = (new_bits_local > 0) ? (4 - new_bits_local / 8) : 0;
        if (max_delta < 0)
          max_delta = 0;
        IROperand new_src1 = tcc_ir_op_get_src1(ir, q);
        int new_src_is_imm = irop_is_immediate(new_src1);
        int new_bytes = (new_bits_local > 0) ? (new_bits_local / 8) : 0;
        for (int delta = 1; delta <= max_delta; delta++)
        {
          int64_t check_off = addr_offset - delta;
          uint32_t ch2 = ((uintptr_t)addr_sym * 31 + (uint32_t)check_off * 17) % 128;
          StoreEntry *ce;
          for (ce = hash_table[ch2]; ce != NULL; ce = ce->next)
          {
            if (!ce->valid || ce->local_sym != addr_sym || ce->local_offset != check_off)
              continue;
            int entry_bytes = 0;
            switch (ce->store_btype)
            {
            case IROP_BTYPE_INT16:
              entry_bytes = 2;
              break;
            case IROP_BTYPE_INT32:
              entry_bytes = 4;
              break;
            case IROP_BTYPE_INT64:
              entry_bytes = 8;
              break;
            default:
              break;
            }
            if (entry_bytes <= delta)
              continue;
            /* Merge via irop_get_imm64_ex, rebuild as IMM32 (I64/F64 keep a pool index in u.imm32, not the value). */
            int ce_is_imm = irop_is_immediate(ce->stored_value);
            if (new_src_is_imm && ce_is_imm && new_bytes > 0 && entry_bytes <= 4 &&
                delta + new_bytes <= entry_bytes)
            {
              int32_t old_v = (int32_t)irop_get_imm64_ex(ir, ce->stored_value);
              int32_t new_v = (int32_t)irop_get_imm64_ex(ir, new_src1);
              uint32_t byte_mask = (new_bytes == 4) ? 0xFFFFFFFFu : ((1u << (new_bytes * 8)) - 1);
              uint32_t pos_mask = byte_mask << (delta * 8);
              uint32_t value_in_pos = ((uint32_t)new_v & byte_mask) << (delta * 8);
              int32_t merged = (int32_t)(((uint32_t)old_v & ~pos_mask) | value_in_pos);
              ce->stored_value = irop_make_imm32(-1, merged, irop_get_btype(ce->stored_value) == IROP_BTYPE_INT64
                                                                 ? IROP_BTYPE_INT32
                                                                 : irop_get_btype(ce->stored_value));
              ce->instruction_idx = i;
              LOG_SL_FWD("STORE@i=%d CROSS-MERGE into store@i=? at off=%lld delta=%d: "
                         "new_bytes=%d entry_bytes=%d old_v=%d new_v=%d merged=%d",
                         i, (long long)check_off, delta, new_bytes, entry_bytes, old_v, new_v, merged);
              continue;
            }
            ce->valid = 0;
          }
        }
      }

      if (merged_into_existing)
      {
        /* Existing entry now holds the merged constant; skip the fresh insert. */
        goto sl_fwd_store_done;
      }

      /* Wide store: invalidate narrower entries at higher offsets in its byte range. */
      {
        int store_bytes = new_bits_local / 8;
        if (store_bytes == 0 && (dest.btype == IROP_BTYPE_INT64 || dest.btype == IROP_BTYPE_FLOAT64))
          store_bytes = 8;
        StoreEntry *he;
        for (int fwd = 1; fwd < store_bytes; fwd++)
        {
          int64_t hi_offset = addr_offset + fwd;
          uint32_t hh = ((uintptr_t)addr_sym * 31 + (uint32_t)hi_offset * 17) % 128;
          for (he = hash_table[hh]; he != NULL; he = he->next)
          {
            if (he->valid && he->local_sym == addr_sym && he->local_offset == hi_offset)
              he->valid = 0;
          }
        }
      }

      /* Record the new store */
      new_entry = &entries[entry_count++];
      new_entry->valid = 1;
      new_entry->addr_addrtaken = addr_addrtaken;
      new_entry->addr_via_pointer = addr_via_pointer;
      new_entry->local_offset = addr_offset;
      new_entry->local_sym = addr_sym;
      new_entry->stored_value = tcc_ir_op_get_src1(ir, q);
      new_entry->instruction_idx = i;
      new_entry->store_dest_vr = addr_vr;
      new_entry->store_btype = dest.btype;
      /* Resolve the stored value through fwd_tmp before deriving width (the source TEMP interval may be stale). */
      {
        IROperand sv = new_entry->stored_value;
        int32_t sv_vr = irop_get_vreg(sv);
        if (sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(sv_vr) == TCCIR_VREG_TYPE_TEMP && !sv.is_lval)
        {
          int sv_pos = TCCIR_DECODE_VREG_POSITION(sv_vr);
          /* fwd_tmp_defs guard: a multiply-defined temp has no single reaching value. */
          if (sv_pos <= max_tmp && fwd_tmp_valid[sv_pos] && fwd_tmp_defs[sv_pos] < 2)
          {
            new_entry->stored_value = fwd_tmp_val[sv_pos];
          }
        }
      }
      /* Detect a genuine 64-bit store from the value's interval/immediate (not btype) and widen store_btype so a 64-bit-store -> 32-bit-read forward is rejected; skip LEA-resolved pointer stores. */
      if (!addr_via_pointer && dest.btype != IROP_BTYPE_INT64 && dest.btype != IROP_BTYPE_FLOAT64)
      {
        int sv_is_64 = 0, sv_is_double = 0;
        int sv_tag = irop_get_tag(new_entry->stored_value);
        if (sv_tag == IROP_TAG_I64)
        {
          /* I64-tagged also encodes unsigned 32-bit consts; only 64-bit if the upper word isn't a sign/zero extension. */
          int64_t v64 = irop_get_imm64_ex(ir, new_entry->stored_value);
          if (v64 != (int64_t)(int32_t)v64 && v64 != (int64_t)(uint32_t)v64)
            sv_is_64 = 1;
        }
        else if (sv_tag == IROP_TAG_F64)
          sv_is_64 = sv_is_double = 1;
        else
        {
          int32_t sv_vr = irop_get_vreg(new_entry->stored_value);
          if (sv_vr >= 0)
          {
            IRLiveInterval *sv_li = tcc_ir_get_live_interval(ir, sv_vr);
            if (sv_li && (sv_li->is_llong || sv_li->is_double))
            {
              sv_is_64 = 1;
              sv_is_double = sv_li->is_double;
            }
          }
        }
        if (sv_is_64)
          new_entry->store_btype = sv_is_double ? IROP_BTYPE_FLOAT64 : IROP_BTYPE_INT64;
      }
      new_entry->next = hash_table[h];
      hash_table[h] = new_entry;

      LOG_SL_FWD("STORE@i=%d TRACK: sym=%p off=%lld btype=%d addrtaken=%d via_ptr=%d", i, (const void *)addr_sym,
                 (long long)addr_offset, (int)dest.btype, addr_addrtaken, addr_via_pointer);

      /* If the stored value reads a tracked memory constant (LEA deref or direct StackLoc lval), forward it. */
      {
        IROperand sv = tcc_ir_op_get_src1(ir, q);
        const Sym *resolved_sym = NULL;
        int64_t resolved_off = 0;
        int sv_resolved = 0;
        if (irop_op_is_lval(sv))
        {
          int32_t sv_vr = irop_get_vreg(sv);
          if (sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(sv_vr) == TCCIR_VREG_TYPE_TEMP)
          {
            int sv_pos = TCCIR_DECODE_VREG_POSITION(sv_vr);
            if (sv_pos <= max_tmp && lea_map[sv_pos].valid)
            {
              resolved_off = lea_map[sv_pos].offset;
              resolved_sym = lea_map[sv_pos].sym;
              sv_resolved = 1;
            }
          }
        }
        if (!sv_resolved && sv.is_lval && sv.is_local && !sv.is_llocal)
        {
          int32_t sv_vr = irop_get_vreg(sv);
          int sv_tag = irop_get_tag(sv);
          if (sv_tag == IROP_TAG_STACKOFF)
          {
            resolved_off = irop_get_stack_offset(sv);
            sv_resolved = 1;
          }
          else if (sv_tag == IROP_TAG_SYMREF)
          {
            IRPoolSymref *sr = irop_get_symref_ex(ir, sv);
            resolved_sym = sr ? sr->sym : NULL;
            resolved_off = sr ? sr->addend : 0;
            sv_resolved = 1;
          }
          if (sv_resolved && sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(sv_vr) == TCCIR_VREG_TYPE_VAR)
            resolved_sym = (const Sym *)(uintptr_t)(1 + (unsigned)TCCIR_DECODE_VREG_POSITION(sv_vr));
        }
        if (sv_resolved)
        {
          uint32_t rh = ((uintptr_t)resolved_sym * 31 + (uint32_t)resolved_off * 17) % 128;
          StoreEntry *re;
          for (re = hash_table[rh]; re != NULL; re = re->next)
          {
            if (!re->valid)
              continue;
            if (re->local_sym != resolved_sym || re->local_offset != resolved_off)
              continue;
            if (re->store_btype != sv.btype)
              continue;
            IROperand resolved_val = re->stored_value;
            {
              int32_t rv_vr = irop_get_vreg(resolved_val);
              if (rv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(rv_vr) == TCCIR_VREG_TYPE_TEMP && !resolved_val.is_lval)
              {
                int rv_pos = TCCIR_DECODE_VREG_POSITION(rv_vr);
                if (rv_pos <= max_tmp && fwd_tmp_valid[rv_pos] && fwd_tmp_defs[rv_pos] < 2)
                  resolved_val = fwd_tmp_val[rv_pos];
              }
            }
            int rv_tag = irop_get_tag(resolved_val);
            if (rv_tag != IROP_TAG_IMM32 && rv_tag != IROP_TAG_I64)
              continue;
            int src1_off = q->operand_base + irop_config[TCCIR_OP_STORE].has_dest;
            ir->iroperand_pool[src1_off] = resolved_val;
            if (new_entry)
              new_entry->stored_value = resolved_val;
            LOG_IR_GEN("OPTIMIZE: LVAL forwarding at i=%d from store at i=%d (offset=%lld)", i, re->instruction_idx,
                       (long long)resolved_off);
            changes++;
            break;
          }
        }
      }

#ifdef TCC_REGALLOC_DEBUG
      fprintf(stderr, "[SL-STORE] i=%d store_val_vr=0x%x store_addr_vr=0x%x offset=%lld n=%d\n", i,
              irop_get_vreg(new_entry->stored_value), addr_vr, (long long)addr_offset, ir->next_instruction_index);
#endif

      LOG_IR_GEN("STORE-LOAD: Track store at i=%d (addrtaken=%d, offset=%lld)", i, addr_addrtaken,
                 (long long)addr_offset);
    sl_fwd_store_done:;
    }

    /* Propagate the LEA map through ADDs created by forwarding (new ASSIGN+ADD chains). */
    if (q->op == TCCIR_OP_ADD)
    {
      IROperand adest = tcc_ir_op_get_dest(ir, q);
      int32_t adv = irop_get_vreg(adest);
      if (adv >= 0 && TCCIR_DECODE_VREG_TYPE(adv) == TCCIR_VREG_TYPE_TEMP)
      {
        int adp = TCCIR_DECODE_VREG_POSITION(adv);
        if (adp <= max_tmp)
        {
          IROperand as1 = tcc_ir_op_get_src1(ir, q);
          IROperand as2 = tcc_ir_op_get_src2(ir, q);
          int32_t s1v = irop_get_vreg(as1);
          int32_t s2v = irop_get_vreg(as2);
          /* Require !is_lval on the LEA operand — a DEREF source (`*T + c`) is a value, not the address T. */
          if (s1v >= 0 && !as1.is_lval && TCCIR_DECODE_VREG_TYPE(s1v) == TCCIR_VREG_TYPE_TEMP &&
              irop_is_immediate(as2) && !as2.is_sym)
          {
            int s1p = TCCIR_DECODE_VREG_POSITION(s1v);
            if (s1p <= max_tmp && lea_map[s1p].valid)
            {
              lea_map[adp].offset = lea_map[s1p].offset + irop_get_imm64_ex(ir, as2);
              lea_map[adp].sym = lea_map[s1p].sym;
              lea_map[adp].valid = 1;
            }
          }
          else if (s2v >= 0 && !as2.is_lval && TCCIR_DECODE_VREG_TYPE(s2v) == TCCIR_VREG_TYPE_TEMP &&
                   irop_is_immediate(as1) && !as1.is_sym)
          {
            int s2p = TCCIR_DECODE_VREG_POSITION(s2v);
            if (s2p <= max_tmp && lea_map[s2p].valid)
            {
              lea_map[adp].offset = lea_map[s2p].offset + irop_get_imm64_ex(ir, as1);
              lea_map[adp].sym = lea_map[s2p].sym;
              lea_map[adp].valid = 1;
            }
          }
        }
      }
    }

    /* Propagate the LEA map through ASSIGN (T <- V), resolving VARs via the hash table. */
    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand adest = tcc_ir_op_get_dest(ir, q);
      int32_t adv = irop_get_vreg(adest);
      if (adv >= 0 && TCCIR_DECODE_VREG_TYPE(adv) == TCCIR_VREG_TYPE_TEMP && !adest.is_lval)
      {
        int adp = TCCIR_DECODE_VREG_POSITION(adv);
        if (adp <= max_tmp && !lea_map[adp].valid)
        {
          IROperand asrc = tcc_ir_op_get_src1(ir, q);
          int32_t sv = irop_get_vreg(asrc);
          if (sv >= 0)
          {
            int sv_type = TCCIR_DECODE_VREG_TYPE(sv);
            int sv_pos = TCCIR_DECODE_VREG_POSITION(sv);
            /* Case 1: src is a TEMP directly in LEA map (not a DEREF) */
            if (sv_type == TCCIR_VREG_TYPE_TEMP && !asrc.is_lval && sv_pos <= max_tmp && lea_map[sv_pos].valid)
            {
              lea_map[adp] = lea_map[sv_pos];
            }
            /* Case 2: src is a VAR/PARAM — check hash table for its stored value */
            else if (asrc.is_local)
            {
              const Sym *vs = NULL;
              int64_t vo;
              if (irop_get_tag(asrc) == IROP_TAG_SYMREF)
              {
                IRPoolSymref *sr = irop_get_symref_ex(ir, asrc);
                vs = sr ? sr->sym : NULL;
                vo = sr ? sr->addend : 0;
              }
              else
              {
                vo = irop_get_imm64_ex(ir, asrc);
              }
              uint32_t vh = ((uintptr_t)vs * 31 + (uint32_t)vo * 17) % 128;
              StoreEntry *ve;
              for (ve = hash_table[vh]; ve != NULL; ve = ve->next)
              {
                if (!ve->valid)
                  continue;
                if (ve->local_sym == vs && ve->local_offset == vo)
                {
                  /* Found the stored value for this VAR — check if it's a LEA-mapped TEMP */
                  int32_t svr = irop_get_vreg(ve->stored_value);
                  if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP && !ve->stored_value.is_lval)
                  {
                    int svp = TCCIR_DECODE_VREG_POSITION(svr);
                    if (svp <= max_tmp && lea_map[svp].valid)
                    {
                      lea_map[adp] = lea_map[svp];
                    }
                  }
                  break;
                }
              }
            }
          }
        }
      }
    }

    /* If this instruction redefines a stored-value vreg, invalidate those entries. */
    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_LOAD)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      int j;

      for (j = 0; j < entry_count; j++)
      {
        if (entries[j].valid)
        {
          if (irop_get_vreg(entries[j].stored_value) == dest_vr)
          {
#ifdef TCC_REGALLOC_DEBUG
            fprintf(stderr, "[SL-INVAL-VAL] i=%d invalidate store at si=%d (stored_val_vr=0x%x redefined) n=%d\n", i,
                    entries[j].instruction_idx, dest_vr, ir->next_instruction_index);
#endif
            entries[j].valid = 0;
          }
        }
      }

      /* Track this write for the LOAD-address staleness check. */
      if (dest_vr >= 0 && !dest.is_lval)
      {
        int vr_type = TCCIR_DECODE_VREG_TYPE(dest_vr);
        int vr_pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        VregWriteTracker *tracker = NULL;
        if (vr_type == TCCIR_VREG_TYPE_VAR && vr_pos <= max_var)
          tracker = &var_writes[vr_pos];
        else if (vr_type == TCCIR_VREG_TYPE_TEMP && vr_pos <= max_tmp)
          tracker = &tmp_writes[vr_pos];
        else if (vr_type == TCCIR_VREG_TYPE_PARAM && vr_pos <= max_par)
          tracker = &par_writes[vr_pos];
        if (tracker)
        {
          tracker->last_write_idx = i;
          tracker->gen = write_tracker_gen;
        }
      }
    }
  }

  /* Post-pass DSE: NOP each forwarded anonymous store with no remaining readers. */
  int skip_fwd_store_dse = 0;
  for (int sj = 0; sj < n; sj++)
  {
    IRQuadCompact *sq = &ir->compact_instructions[sj];
    if (sq->op != TCCIR_OP_STORE_INDEXED && sq->op != TCCIR_OP_LOAD_INDEXED)
      continue;
    IROperand idx = tcc_ir_op_get_src2(ir, sq);
    /* Runtime-indexed stack accesses may still read forwarded stores; skip DSE then. */
    if (!irop_is_immediate(idx) || idx.is_sym)
    {
      skip_fwd_store_dse = 1;
      break;
    }
  }
  for (int fi = 0; !skip_fwd_store_dse && fi < fwd_store_count; fi++)
  {
    int store_idx = fwd_stores[fi].store_idx;
    int64_t off = fwd_stores[fi].offset;
    const Sym *sym = fwd_stores[fi].sym;
    int still_read = 0;

    /* Check if the store was already NOP'd (e.g. by a later store overwrite) */
    if (ir->compact_instructions[store_idx].op == TCCIR_OP_NOP)
      continue;

    for (int j = 0; j < n && !still_read; j++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP || j == store_idx)
        continue;

      /* Macro: operand is a local address-of that could alias our store's offset. */
#define CHECK_ADDR_ALIAS(op)                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((op).is_local && !(op).is_lval && !(op).is_llocal && irop_get_sym_ex(ir, (op)) == sym)                         \
    {                                                                                                                  \
      int64_t base_off = irop_get_imm64_ex(ir, (op));                                                                  \
      if (base_off <= off && (off - base_off) < 1024)                                                                  \
        still_read = 1;                                                                                                \
    }                                                                                                                  \
  } while (0)

      /* Macro: operand reads a multi-byte range [X, X+W) covering our store's offset. */
#define CHECK_WIDTH_OVERLAP(op)                                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((op).is_local && irop_get_sym_ex(ir, (op)) == sym)                                                             \
    {                                                                                                                  \
      int64_t _roff = irop_get_imm64_ex(ir, (op));                                                                     \
      if (_roff != off && _roff <= off)                                                                                \
      {                                                                                                                \
        int _w = 4;                                                                                                    \
        if ((op).btype == IROP_BTYPE_INT64 || (op).btype == IROP_BTYPE_FLOAT64)                                        \
          _w = 8;                                                                                                      \
        else if ((op).btype == IROP_BTYPE_STRUCT)                                                                      \
          _w = 1024;                                                                                                   \
        /* Complex types implicitly read both real and imag halves. */                                                 \
        if ((op).is_complex)                                                                                           \
          _w *= 2;                                                                                                     \
        if (off < _roff + _w)                                                                                          \
          still_read = 1;                                                                                              \
      }                                                                                                                \
    }                                                                                                                  \
  } while (0)

      if (irop_config[jq->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, jq);
        if (s1.is_local && irop_get_imm64_ex(ir, s1) == off && irop_get_sym_ex(ir, s1) == sym)
          still_read = 1;
        if (!still_read)
          CHECK_WIDTH_OVERLAP(s1);
        if (!still_read)
          CHECK_ADDR_ALIAS(s1);
      }
      if (!still_read && irop_config[jq->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, jq);
        if (s2.is_local && irop_get_imm64_ex(ir, s2) == off && irop_get_sym_ex(ir, s2) == sym)
          still_read = 1;
        if (!still_read)
          CHECK_WIDTH_OVERLAP(s2);
        if (!still_read)
          CHECK_ADDR_ALIAS(s2);
      }
      /* Check dest of non-STORE ops (e.g. LOAD dest references an address) */
      if (!still_read && jq->op != TCCIR_OP_STORE && irop_config[jq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, jq);
        if (d.is_local && irop_get_imm64_ex(ir, d) == off && irop_get_sym_ex(ir, d) == sym)
          still_read = 1;
        if (!still_read)
          CHECK_WIDTH_OVERLAP(d);
        if (!still_read)
          CHECK_ADDR_ALIAS(d);
      }
      /* Check STORE dest with deref (reads the pointer from the slot) */
      if (!still_read && jq->op == TCCIR_OP_STORE && irop_config[jq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, jq);
        if (d.is_local && (d.is_lval || d.is_llocal) && irop_get_imm64_ex(ir, d) == off &&
            irop_get_sym_ex(ir, d) == sym)
          still_read = 1;
        if (!still_read)
          CHECK_WIDTH_OVERLAP(d);
        if (!still_read)
          CHECK_ADDR_ALIAS(d);
      }
#undef CHECK_ADDR_ALIAS
#undef CHECK_WIDTH_OVERLAP
    }

    if (!still_read)
    {
      LOG_IR_GEN("OPTIMIZE: Dead store at i=%d (offset %lld, no remaining readers after SL fwd)", store_idx,
                 (long long)off);
      ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;
      changes++;
    }
  }
#undef SL_FWD_MAX_DEAD_STORES

  tcc_free(entries);
  tcc_free(var_writes);
  tcc_free(tmp_writes);
  tcc_free(par_writes);
  tcc_free(lea_map);
  tcc_free(var_lea_map);
  tcc_free(var_def_count);
  tcc_free(addrtaken_slots);
  tcc_free(active_call_ids);
  tcc_free(fwd_tmp_val);
  tcc_free(fwd_tmp_valid);
  tcc_free(fwd_tmp_defs);
  for (i = 0; i < n; i++)
    tcc_free(saved_entries[i]);
  tcc_free(saved_entries);
  tcc_free(saved_entry_count);
  tcc_free(saved_entry_cap);
  tcc_free(pred_count);

  LOG_IR_GEN("=== STORE-LOAD FORWARDING END: %d changes ===", changes);

  return changes;
}

/* Global store-load forwarding: BB-local, forward a STORE to a GlobalSym into later deref uses of it. */
static int tcc_ir_opt_global_sl_fwd__timed(TCCIRState *ir);
int tcc_ir_opt_global_sl_fwd(TCCIRState *ir)
{
  if (tcc_ir_opt_pass_disabled("global_sl_fwd")) return 0;
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_global_sl_fwd__timed(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_global_sl_fwd__timed(ir);
  tcc_pass_timing_add("global_sl_fwd", tcc_pass_clk_us() - _t);
  return _r;
}
static int tcc_ir_opt_global_sl_fwd__timed(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;
  /* Same rationale as tcc_ir_opt_sl_forward. */
  if (ir->is_variadic || ir_has_stack_arg_variadic_call(ir))
    return 0;



  /* IJUMP (computed goto) targets aren't marked is_jump_target — skip the whole function. */
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;

#define GSLFWD_MAX_ENTRIES 16
  struct
  {
    Sym *sym;
    int64_t addend;
    int btype;
    int32_t value_vr;   /* vreg holding the stored value, or -1 if immediate */
    int64_t value_imm;  /* immediate value when value_vr == -1 */
    int     imm_is_i32; /* value_imm fits in int32 (else build a fresh i64 pool entry per use) */
  } entries[GSLFWD_MAX_ENTRIES];
  int entry_count = 0;

  /* Recompute actual JUMP/JUMPIF targets (is_jump_target can be stale); only ADD boundaries, never drop. */
  uint8_t *actual_targets = tcc_mallocz((n + 7) / 8);
  for (int t = 0; t < n; t++)
  {
    IRQuadCompact *jq = &ir->compact_instructions[t];
    if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
    {
      int tg = (int)tcc_ir_op_get_dest(ir, jq).u.imm32;
      if (tg >= 0 && tg < n)
        actual_targets[tg / 8] |= (1 << (tg % 8));
    }
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Join points (jump targets) clear the state going in; the instruction itself is still processed below. */
    if (q->is_jump_target || (actual_targets[i / 8] & (1 << (i % 8))))
      entry_count = 0;
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_SWITCH_TABLE)
    {
      entry_count = 0;
      continue;
    }
    /* Calls may write any global. */
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      entry_count = 0;
      continue;
    }

    /* Rewrite tracked-global deref uses before updating the table (skip the store dest). */
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
    {
      for (int s = 0; s < 2; s++)
      {
        int has = (s == 0) ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2;
        if (!has)
          continue;
        IROperand u = (s == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        if (!u.is_sym || !u.is_lval)
          continue;
        IRPoolSymref *uref = irop_get_symref_ex(ir, u);
        if (!uref || !uref->sym)
          continue;
        int btype = irop_get_btype(u);
        for (int k = 0; k < entry_count; k++)
        {
          if (entries[k].sym != uref->sym || entries[k].addend != uref->addend ||
              entries[k].btype != btype)
            continue;
          IROperand newop;
          if (entries[k].value_vr >= 0)
          {
            /* Forward only a TEMP vreg — the redefinition scan soundly drops the entry when it is overwritten. */
            if (TCCIR_DECODE_VREG_TYPE(entries[k].value_vr) != TCCIR_VREG_TYPE_TEMP)
              continue;
            newop = irop_make_vreg(entries[k].value_vr, btype);
          }
          else if (entries[k].imm_is_i32)
          {
            newop = irop_make_imm32(-1, (int32_t)entries[k].value_imm, btype);
          }
          else
          {
            uint32_t pool_idx = tcc_ir_pool_add_i64(ir, entries[k].value_imm);
            newop = irop_make_i64(-1, pool_idx, btype);
          }
          newop.is_unsigned = u.is_unsigned;
          if (s == 0)
            tcc_ir_set_src1(ir, i, newop);
          else
            tcc_ir_set_src2(ir, i, newop);
          /* A LOAD whose deref source became a value is now a pure copy — convert to ASSIGN. */
          if (q->op == TCCIR_OP_LOAD && s == 0)
            q->op = TCCIR_OP_ASSIGN;
          changes++;
          break;
        }
      }
    }

    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);

      /* STORE to a GlobalSym: refresh that entry. */
      if (dest.is_sym && dest.is_lval)
      {
        IRPoolSymref *dref = irop_get_symref_ex(ir, dest);
        if (!dref || !dref->sym)
          continue;
        int dbtype = irop_get_btype(dest);
        /* Track plain value vregs and immediate constants (INT32/INT64); invalidate anything else. */
        int32_t val_vr = irop_get_vreg(src1);
        int store_is_plain_vreg = (val_vr >= 0 && !src1.is_lval && !src1.is_sym);
        int store_is_imm = irop_is_immediate(src1);
        if ((dbtype != IROP_BTYPE_INT32 && dbtype != IROP_BTYPE_INT64) ||
            (!store_is_plain_vreg && !store_is_imm))
        {
          /* still invalidate any existing entry for this sym/addend */
          for (int k = 0; k < entry_count;)
          {
            if (entries[k].sym == dref->sym && entries[k].addend == dref->addend)
              entries[k] = entries[--entry_count];
            else
              k++;
          }
          continue;
        }
        /* Find existing entry to refresh, else append. */
        int found = 0;
        for (int k = 0; k < entry_count; k++)
        {
          if (entries[k].sym == dref->sym && entries[k].addend == dref->addend)
          {
            entries[k].btype = dbtype;
            if (store_is_plain_vreg)
            {
              entries[k].value_vr = val_vr;
              entries[k].value_imm = 0;
              entries[k].imm_is_i32 = 0;
            }
            else
            {
              int64_t v = irop_get_imm64_ex(ir, src1);
              entries[k].value_vr = -1;
              entries[k].value_imm = v;
              entries[k].imm_is_i32 = (v == (int32_t)v);
            }
            found = 1;
            break;
          }
        }
        if (!found && entry_count < GSLFWD_MAX_ENTRIES)
        {
          entries[entry_count].sym = dref->sym;
          entries[entry_count].addend = dref->addend;
          entries[entry_count].btype = dbtype;
          if (store_is_plain_vreg)
          {
            entries[entry_count].value_vr = val_vr;
            entries[entry_count].value_imm = 0;
            entries[entry_count].imm_is_i32 = 0;
          }
          else
          {
            int64_t v = irop_get_imm64_ex(ir, src1);
            entries[entry_count].value_vr = -1;
            entries[entry_count].value_imm = v;
            entries[entry_count].imm_is_i32 = (v == (int32_t)v);
          }
          entry_count++;
        }
        continue;
      }

      /* A local stack store can't alias globals; an unknown pointer write invalidates everything. */
      if (!dest.is_local)
      {
        entry_count = 0;
      }
      continue;
    }
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    {
      /* These could hit any address — invalidate. */
      entry_count = 0;
      continue;
    }

    /* Redefining a tracked value vreg drops the entry (immediate entries are vreg-independent). */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(d);
      if (d_vr >= 0 && !d.is_lval)
      {
        for (int k = 0; k < entry_count;)
        {
          if (entries[k].value_vr >= 0 && entries[k].value_vr == d_vr)
            entries[k] = entries[--entry_count];
          else
            k++;
        }
      }
    }
  }

  tcc_free(actual_targets);
  return changes;
#undef GSLFWD_MAX_ENTRIES
}
int tcc_ir_opt_sl_forward_ex(IROptCtx *ctx) { return tcc_ir_opt_sl_forward(ctx->ir); }
int tcc_ir_opt_global_sl_fwd_ex(IROptCtx *ctx) { return tcc_ir_opt_global_sl_fwd(ctx->ir); }
