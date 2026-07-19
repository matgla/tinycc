/*
 *  TCC IR - Local Copy Propagation (pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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


/*
 * Eliminates redundant copies through temporary stack slots.  When inline
 * struct copy produces:
 *
 *   [writes to temp slot A]          e.g. memset(A, 0, 56); A[0] = val;
 *   LOAD T = A[0];  STORE B[0] = T;
 *   LOAD T = A[4];  STORE B[4] = T;
 *   ...                              (copy chain: A → B)
 *   [only B is used afterwards]
 *
 * This pass redirects all writes from A to B and NOPs the copy chain,
 * producing:
 *
 *   [writes to B directly]           e.g. memset(B, 0, 56); B[0] = val;
 *   [B is used]
 */

static int lcp_find_next(TCCIRState *ir, int start, int n)
{
  for (int j = start; j < n; j++)
    if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
      return j;
  return -1;
}

int tcc_ir_opt_local_copy_prop(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 4)
    return 0;

  int changes = 0;

  for (int i = 0; i < n;) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LOAD) {
      i++;
      continue;
    }

    IROperand load_src = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(load_src) != IROP_TAG_STACKOFF || !load_src.is_local ||
        (load_src.btype != IROP_BTYPE_INT32 && load_src.btype != IROP_BTYPE_FLOAT32)) {
      i++;
      continue;
    }

    int store_i = lcp_find_next(ir, i + 1, n);
    if (store_i < 0) {
      i++;
      continue;
    }
    IRQuadCompact *sq = &ir->compact_instructions[store_i];
    if (sq->op != TCCIR_OP_STORE || sq->is_jump_target) {
      i++;
      continue;
    }

    IROperand store_dest = tcc_ir_op_get_dest(ir, sq);
    if (irop_get_tag(store_dest) != IROP_TAG_STACKOFF || !store_dest.is_local) {
      i++;
      continue;
    }

    IROperand load_dest = tcc_ir_op_get_dest(ir, q);
    IROperand store_src1 = tcc_ir_op_get_src1(ir, sq);
    if (irop_get_vreg(load_dest) < 0 ||
        irop_get_vreg(load_dest) != irop_get_vreg(store_src1)) {
      i++;
      continue;
    }

    int32_t src_base = irop_get_stack_offset(load_src);
    int32_t dst_base = irop_get_stack_offset(store_dest);
    if (src_base == dst_base) {
      i++;
      continue;
    }
    int32_t delta = dst_base - src_base;

    /* Extend: find more consecutive LOAD+STORE copy pairs */
    int pair_loads[64], pair_stores[64];
    pair_loads[0] = i;
    pair_stores[0] = store_i;
    int count = 1;
    int last_store = store_i;

    while (count < 64) {
      int nl = lcp_find_next(ir, last_store + 1, n);
      if (nl < 0 || ir->compact_instructions[nl].op != TCCIR_OP_LOAD ||
          ir->compact_instructions[nl].is_jump_target)
        break;

      IROperand nl_src = tcc_ir_op_get_src1(ir, &ir->compact_instructions[nl]);
      if (irop_get_tag(nl_src) != IROP_TAG_STACKOFF || !nl_src.is_local)
        break;
      if (irop_get_stack_offset(nl_src) != src_base + count * 4)
        break;

      int ns = lcp_find_next(ir, nl + 1, n);
      if (ns < 0 || ir->compact_instructions[ns].op != TCCIR_OP_STORE ||
          ir->compact_instructions[ns].is_jump_target)
        break;

      IROperand ns_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[ns]);
      if (irop_get_tag(ns_dest) != IROP_TAG_STACKOFF || !ns_dest.is_local)
        break;
      if (irop_get_stack_offset(ns_dest) != dst_base + count * 4)
        break;

      IROperand nl_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[nl]);
      IROperand ns_src1 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[ns]);
      if (irop_get_vreg(nl_dest) < 0 ||
          irop_get_vreg(nl_dest) != irop_get_vreg(ns_src1))
        break;

      pair_loads[count] = nl;
      pair_stores[count] = ns;
      count++;
      last_store = ns;
    }

    if (count < 4) {
      i++;
      continue;
    }

    int32_t src_end = src_base + count * 4;

    /* Bail out if a memset/memclr call initializes a range that overlaps
     * the source.  We can't redirect the call, so the dest would have
     * uninitialized bytes after we NOP the copy chain. */
    int has_overlapping_call = 0;
    for (int j = 0; j < n && !has_overlapping_call; j++) {
      IRQuadCompact *cq = &ir->compact_instructions[j];
      if (cq->op != TCCIR_OP_FUNCCALLVOID && cq->op != TCCIR_OP_FUNCCALLVAL)
        continue;
      IROperand csrc1 = tcc_ir_op_get_src1(ir, cq);
      Sym *callee = irop_get_sym_ex(ir, csrc1);
      if (!callee)
        continue;
      const char *name = get_tok_str(callee->v, NULL);
      if (strcmp(name, "__aeabi_memset") != 0 && strcmp(name, "memset") != 0 &&
          strcmp(name, "__aeabi_memclr") != 0 && strcmp(name, "__aeabi_memclr4") != 0 &&
          strcmp(name, "__aeabi_memclr8") != 0)
        continue;
      IROperand cenc = tcc_ir_op_get_src2(ir, cq);
      int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, cenc));
      for (int p = j - 1; p >= 0; p--) {
        IRQuadCompact *pq = &ir->compact_instructions[p];
        if (pq->op == TCCIR_OP_NOP)
          continue;
        if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
          break;
        IROperand penc = tcc_ir_op_get_src2(ir, pq);
        if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, penc)) != call_id)
          continue;
        IROperand pval = tcc_ir_op_get_src1(ir, pq);
        if (irop_get_tag(pval) == IROP_TAG_STACKOFF && pval.is_local && !pval.is_lval) {
          int32_t addr = irop_get_stack_offset(pval);
          if (addr < src_end && addr + 128 > src_base)
            has_overlapping_call = 1;
        }
      }
    }
    if (has_overlapping_call) {
      i++;
      continue;
    }

    /* Check safety: the source range must not be read outside the copy chain,
     * and its address must not escape (except to memset/memclr). */
    int safe = 1;
    int memset_param_instrs[8];
    int memset_param_count = 0;

    for (int j = 0; j < n && safe; j++) {
      IRQuadCompact *cq = &ir->compact_instructions[j];
      if (cq->op == TCCIR_OP_NOP)
        continue;

      int is_chain = 0;
      for (int k = 0; k < count; k++) {
        if (j == pair_loads[k] || j == pair_stores[k]) {
          is_chain = 1;
          break;
        }
      }
      if (is_chain)
        continue;

      /* Check src1 for reads from the source range.  Any STACKOFF src1
       * with is_lval=1 in the source range is a memory read. */
      if (irop_config[cq->op].has_src1) {
        IROperand op = tcc_ir_op_get_src1(ir, cq);
        if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && op.is_lval) {
          int32_t off = irop_get_stack_offset(op);
          if (off >= src_base && off < src_end) {
            safe = 0;
            break;
          }
        }
        /* Check for address-of source range in any src1 operand */
        if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_lval) {
          int32_t off = irop_get_stack_offset(op);
          if (off >= src_base && off < src_end) {
            if (cq->op == TCCIR_OP_FUNCPARAMVAL || cq->op == TCCIR_OP_FUNCPARAMVOID) {
              if (memset_param_count < 8)
                memset_param_instrs[memset_param_count++] = j;
              else
                safe = 0;
            }
          }
        }
      }

      /* Check for LEA / address-of source range */
      if (irop_config[cq->op].has_dest && cq->op != TCCIR_OP_STORE &&
          cq->op != TCCIR_OP_STORE_INDEXED && cq->op != TCCIR_OP_STORE_POSTINC) {
        IROperand op = tcc_ir_op_get_dest(ir, cq);
        if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_lval) {
          int32_t off = irop_get_stack_offset(op);
          if (off >= src_base && off < src_end)
            safe = 0;
        }
      }

      if (irop_config[cq->op].has_src2) {
        IROperand op = tcc_ir_op_get_src2(ir, cq);
        if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local) {
          int32_t off = irop_get_stack_offset(op);
          if (off >= src_base && off < src_end) {
            if (op.is_lval)
              safe = 0;
          }
        }
      }
    }

    if (!safe) {
      i++;
      continue;
    }

    /* Verify address-of uses are only for memset/memclr calls */
    for (int m = 0; m < memset_param_count && safe; m++) {
      int param_i = memset_param_instrs[m];
      IRQuadCompact *pq = &ir->compact_instructions[param_i];
      IROperand penc = tcc_ir_op_get_src2(ir, pq);
      int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, penc));
      int found_call = 0;
      for (int j = param_i + 1; j < n; j++) {
        IRQuadCompact *cq = &ir->compact_instructions[j];
        if (cq->op == TCCIR_OP_NOP)
          continue;
        if (cq->op == TCCIR_OP_FUNCPARAMVAL || cq->op == TCCIR_OP_FUNCPARAMVOID)
          continue;
        if (cq->op == TCCIR_OP_FUNCCALLVOID || cq->op == TCCIR_OP_FUNCCALLVAL) {
          IROperand csrc2 = tcc_ir_op_get_src2(ir, cq);
          if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, csrc2)) == call_id) {
            IROperand csrc1 = tcc_ir_op_get_src1(ir, cq);
            Sym *callee = irop_get_sym_ex(ir, csrc1);
            if (callee) {
              const char *name = get_tok_str(callee->v, NULL);
              if (strcmp(name, "__aeabi_memset") == 0 ||
                  strcmp(name, "memset") == 0 ||
                  strcmp(name, "__aeabi_memclr") == 0 ||
                  strcmp(name, "__aeabi_memclr4") == 0 ||
                  strcmp(name, "__aeabi_memclr8") == 0)
                found_call = 1;
            }
          }
          break;
        }
        break;
      }
      if (!found_call)
        safe = 0;
    }

    if (!safe) {
      i++;
      continue;
    }

    /* Apply: redirect all writes to source range → dest range */
    for (int j = 0; j < n; j++) {
      IRQuadCompact *cq = &ir->compact_instructions[j];
      if (cq->op == TCCIR_OP_NOP)
        continue;

      int is_chain = 0;
      for (int k = 0; k < count; k++) {
        if (j == pair_loads[k] || j == pair_stores[k]) {
          is_chain = 1;
          break;
        }
      }
      if (is_chain)
        continue;

      if (cq->op == TCCIR_OP_STORE || cq->op == TCCIR_OP_STORE_INDEXED ||
          cq->op == TCCIR_OP_STORE_POSTINC) {
        if (irop_config[cq->op].has_dest) {
          IROperand op = tcc_ir_op_get_dest(ir, cq);
          if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && op.is_lval) {
            int32_t off = irop_get_stack_offset(op);
            if (off >= src_base && off < src_end) {
              if (op.btype == IROP_BTYPE_STRUCT)
                op.u.s.aux_data = (uint32_t)(int32_t)(off + delta);
              else
                op.u.imm32 = off + delta;
              tcc_ir_op_set_dest(ir, cq, op);
            }
          }
        }
      }

      /* Redirect any src1 address-of (is_lval=0) StackLoc in source range.
       * Covers both direct FUNCPARAMVAL and LEA/ASSIGN instructions that
       * compute Addr[StackLoc[off]]. */
      if (irop_config[cq->op].has_src1) {
        IROperand op = tcc_ir_op_get_src1(ir, cq);
        if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_lval) {
          int32_t off = irop_get_stack_offset(op);
          if (off >= src_base && off < src_end) {
            if (op.btype == IROP_BTYPE_STRUCT)
              op.u.s.aux_data = (uint32_t)(int32_t)(off + delta);
            else
              op.u.imm32 = off + delta;
            tcc_ir_op_set_src1(ir, cq, op);
          }
        }
      }
    }

    for (int k = 0; k < count; k++) {
      ir->compact_instructions[pair_loads[k]].op = TCCIR_OP_NOP;
      ir->compact_instructions[pair_stores[k]].op = TCCIR_OP_NOP;
    }

    changes += count;
    i = last_store + 1;
  }

  return changes;
}

int tcc_ir_opt_local_copy_prop_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_local_copy_prop(ctx->ir);
}

