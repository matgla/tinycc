/*
 *  TCC IR - Post-phi-resolution constant-branch folding
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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

#define USING_GLOBALS
#include "ir.h"
#include "regalloc.h"

/* Only possible after phi resolution: SSA hides the loop-rotation guard's constant behind a phi dest, and phi resolution re-materializes it in the cmp's own block; no pass runs later. */
/* Killing a dead guard also drops the fall-through block of phi copies, shrinking every carrier's live range between chained loops (SHA-style code). */

static int ra_eval_cmp_cond(int64_t v1, int64_t v2, int tok)
{
  switch (tok) {
  case 0x94: return v1 == v2;
  case 0x95: return v1 != v2;
  case 0x9c: return v1 < v2;
  case 0x9d: return v1 >= v2;
  case 0x9e: return v1 <= v2;
  case 0x9f: return v1 > v2;
  case 0x92: return (uint64_t)v1 < (uint64_t)v2;
  case 0x93: return (uint64_t)v1 >= (uint64_t)v2;
  case 0x96: return (uint64_t)v1 <= (uint64_t)v2;
  case 0x97: return (uint64_t)v1 > (uint64_t)v2;
  default: return -1;
  }
}

/* Soundness: the walk must fail if anything in (def, cmp_idx] is a jump target, since multiple predecessors mean the def does not dominate cmp_idx. */
static int ra_try_resolve_const_local(TCCIRState *ir, const uint8_t *is_target,
                                      IROperand op, int cmp_idx, int64_t *out)
{
  if (irop_is_immediate(op)) {
    *out = irop_get_imm64_ex(ir, op);
    return 1;
  }
  if (op.tag != IROP_TAG_VREG || op.is_lval) return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0) return 0;

  /* cmp_idx itself a join point: another path can deliver a different value. */
  if (is_target && is_target[cmp_idx]) return 0;

  for (int k = cmp_idx - 1; k >= 0; k--) {
    IRQuadCompact *q = &ir->compact_instructions[k];
    TccIrOp opc = q->op;
    if (opc == TCCIR_OP_NOP) {
      /* A NOP at a jump-target position still marks a join. */
      if (is_target && is_target[k]) return 0;
      continue;
    }

    if (opc == TCCIR_OP_JUMP || opc == TCCIR_OP_JUMPIF ||
        opc == TCCIR_OP_IJUMP || opc == TCCIR_OP_SWITCH_TABLE ||
        opc == TCCIR_OP_RETURNVALUE || opc == TCCIR_OP_RETURNVOID)
      return 0;

    /* Stores define memory, not vregs. */
    if (opc == TCCIR_OP_STORE || opc == TCCIR_OP_STORE_INDEXED ||
        opc == TCCIR_OP_STORE_POSTINC) {
      if (is_target && is_target[k]) return 0;
      continue;
    }

    if (!irop_config[opc].has_dest) {
      if (is_target && is_target[k]) return 0;
      continue;
    }
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval) {
      if (is_target && is_target[k]) return 0;
      continue;
    }
    int32_t dv = irop_get_vreg(d);
    if (dv != vr) {
      if (is_target && is_target[k]) return 0;
      continue;
    }

    if (opc != TCCIR_OP_ASSIGN) return 0;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (!irop_is_immediate(s) || s.is_lval) return 0;
    *out = irop_get_imm64_ex(ir, s);
    return 1;
  }
  return 0;
}

/* Drop the now-unreachable fall-through: NOP from start_idx up to the next jump target. */
static int ra_nop_dead_block(TCCIRState *ir, const uint8_t *is_target, int start_idx)
{
  int n = ir->next_instruction_index;
  int nopped = 0;
  for (int i = start_idx; i < n; i++) {
    if (is_target[i]) break;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    q->op = TCCIR_OP_NOP;
    nopped++;
  }
  return nopped;
}

static uint8_t *ra_build_jump_target_map(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 0) return NULL;
  uint8_t *map = tcc_mallocz((size_t)n);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (t >= 0 && t < n) map[t] = 1;
    } else if (q->op == TCCIR_OP_SWITCH_TABLE) {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, s2);
      if (table_id >= 0 && table_id < ir->num_switch_tables) {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++) {
          int t = table->targets[j];
          if (t >= 0 && t < n) map[t] = 1;
        }
        int dt = table->default_target;
        if (dt >= 0 && dt < n) map[dt] = 1;
      }
    }
  }
  return map;
}

int ra_fold_const_branches(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 0) return 0;

  uint8_t *is_target = ra_build_jump_target_map(ir);
  if (!is_target) return 0;

  int folds = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMPIF) continue;

    /* Find the CMP feeding this JUMPIF; phi resolution interleaves unrelated ASSIGN copies. */
    int cmp_idx = -1;
    for (int j = i - 1; j >= 0; j--) {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      TccIrOp pop = pq->op;
      if (pop == TCCIR_OP_NOP) continue;
      if (pop == TCCIR_OP_CMP) { cmp_idx = j; break; }
      /* Another flag-setter invalidates the CMP we want to read. */
      if (pop == TCCIR_OP_TEST_ZERO || pop == TCCIR_OP_FCMP) break;
      /* Calls clobber CPSR, and the soft-float compare helpers (__aeabi_cfcmple/cdcmple) ARE the branch's flag source: striding past them mis-attributes the branch to an earlier CMP and NOPs it, orphaning a SELECT (fuzz seed 2049). */
      if (pop == TCCIR_OP_FUNCCALLVAL || pop == TCCIR_OP_FUNCCALLVOID) break;
      /* A flag consumer between CMP and JUMPIF is a second reader that folding would break. */
      if (pop == TCCIR_OP_SETIF || pop == TCCIR_OP_SELECT) break;
      if (pop == TCCIR_OP_JUMP || pop == TCCIR_OP_JUMPIF ||
          pop == TCCIR_OP_IJUMP || pop == TCCIR_OP_SWITCH_TABLE ||
          pop == TCCIR_OP_RETURNVALUE || pop == TCCIR_OP_RETURNVOID)
        break;
      /* Everything else (ASSIGN, ADD, LOAD, STORE, ...) is flag-neutral. */
    }
    if (cmp_idx < 0) continue;

    IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];
    IROperand src1 = tcc_ir_op_get_src1(ir, cmp_q);
    IROperand src2 = tcc_ir_op_get_src2(ir, cmp_q);

    int64_t v1, v2;
    if (!ra_try_resolve_const_local(ir, is_target, src1, cmp_idx, &v1)) continue;
    if (!ra_try_resolve_const_local(ir, is_target, src2, cmp_idx, &v2)) continue;

    /* Truncate to operand width to match comparison semantics. */
    int cmp_btype = irop_get_btype(src1);
    if (cmp_btype != IROP_BTYPE_INT64) {
      v1 = (int64_t)(int32_t)(uint32_t)v1;
      v2 = (int64_t)(int32_t)(uint32_t)v2;
    }

    IROperand cond = tcc_ir_op_get_src1(ir, q);
    int tok = (int)irop_get_imm64_ex(ir, cond);
    int result = ra_eval_cmp_cond(v1, v2, tok);
    if (result < 0) continue;

    if (result) {
      /* Always taken. */
      IROperand target = tcc_ir_op_get_dest(ir, q);
      cmp_q->op = TCCIR_OP_NOP;
      q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, i, target);
      tcc_ir_set_src1(ir, i, IROP_NONE);
      ra_nop_dead_block(ir, is_target, i + 1);
    } else {
      /* Never taken. */
      cmp_q->op = TCCIR_OP_NOP;
      q->op = TCCIR_OP_NOP;
    }
    folds++;
  }

  tcc_free(is_target);
  return folds;
}
