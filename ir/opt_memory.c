/*
 *  TCC IR - Memory optimization passes (pre-SSA)
 *
 *  Store-load forwarding, entry store propagation, redundant store
 *  elimination, deref forwarding.
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

static const uint8_t *ir_opt_get_rodata_bytes(TCCIRState *ir, IROperand op, size_t *out_size)
{
  IRPoolSymref *symref;
  Sym *sym;
  ElfSym *esym;
  Section *sec;
  addr_t offset;

  if (!ir || irop_get_tag(op) != IROP_TAG_SYMREF)
    return NULL;

  symref = irop_get_symref_ex(ir, op);
  if (!symref || symref->addend < 0)
    return NULL;

  sym = symref->sym;
  if (!sym)
    return NULL;

  esym = elfsym(sym);
  if (!esym)
    return NULL;
  if (esym->st_shndx == SHN_UNDEF || esym->st_shndx >= (unsigned)tcc_state->nb_sections)
    return NULL;

  sec = tcc_state->sections[esym->st_shndx];
  if (!sec || !sec->data)
    return NULL;
  if (sec->sh_flags & SHF_WRITE)
    return NULL;
  if (esym->st_size == 0)
    return NULL;

  offset = esym->st_value + (addr_t)symref->addend;
  if (offset + esym->st_size > sec->data_offset)
    return NULL;

  if (sec->reloc && sec->reloc->data_offset > 0)
  {
    ElfW_Rel *rel = (ElfW_Rel *)sec->reloc->data;
    ElfW_Rel *rel_end = (ElfW_Rel *)(sec->reloc->data + sec->reloc->data_offset);
    for (; rel < rel_end; rel++)
    {
      if (rel->r_offset >= esym->st_value && rel->r_offset < esym->st_value + esym->st_size)
        return NULL;
    }
  }

  *out_size = (size_t)(esym->st_size - (addr_t)symref->addend);
  return sec->data + offset;
}

int tcc_ir_opt_deref_fwd(TCCIRState *ir)
{
  /* Forward a deref'd load to a subsequent use of the same deref.
   *
   *   i:   Vdest = Tsrc***DEREF***          (load from pointer)
   *   j:   CMP Rx, Tsrc***DEREF***          (same pointer deref)
   *                ^^^^^^^^^^^^^^^^^^
   *   =>   CMP Rx, Vdest                    (use already-loaded value)
   *
   * Only fires when i and j are adjacent (or separated only by NOPs)
   * so no aliasing or clobber analysis is needed.  Adjacency in the
   * instruction stream is not enough: j must not start a basic block,
   * otherwise a branch can reach the CMP without executing the load. */
  int n = ir->next_instruction_index;
  int changes = 0;
  uint8_t *block_starts = NULL;

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* Only match ASSIGN/LOAD/STORE — these are the opcodes that genuinely
     * load a value from a dereferenced pointer into a destination vreg. */
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_STORE)
      continue;
    if (!irop_config[q->op].has_dest || !irop_config[q->op].has_src1)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (!src1.is_lval)
      continue;
    int32_t load_addr_vr = irop_get_vreg(src1);
    if (load_addr_vr < 0)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || dest.is_lval)
      continue;

    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      break;

    IRQuadCompact *next = &ir->compact_instructions[j];
    if (next->op != TCCIR_OP_CMP)
      continue;

    /* The load must dominate the CMP: reject when any instruction after
     * the load up to and including the CMP is a jump target. */
    if (!block_starts)
      block_starts = ir_opt_build_block_starts_bitmap(ir, n);
    {
      int crosses_block = 0;
      for (int k = i + 1; k <= j; k++)
      {
        if (IR_IS_BLOCK_START(block_starts, k))
        {
          crosses_block = 1;
          break;
        }
      }
      if (crosses_block)
        continue;
    }

    /* Check src2 of CMP for matching deref. */
    if (irop_config[next->op].has_src2)
    {
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, next);
      if (cmp_src2.is_lval && irop_get_vreg(cmp_src2) == load_addr_vr)
      {
        IROperand replacement = irop_make_vreg(dest_vr, dest.btype);
        tcc_ir_set_src2(ir, j, replacement);
        changes++;
        continue;
      }
    }

    /* Check src1 of CMP for matching deref. */
    {
      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, next);
      if (cmp_src1.is_lval && irop_get_vreg(cmp_src1) == load_addr_vr)
      {
        IROperand replacement = irop_make_vreg(dest_vr, dest.btype);
        tcc_ir_set_src1(ir, j, replacement);
        changes++;
      }
    }
  }

  tcc_free(block_starts);
  return changes;
}


/* ============================================================================
 * Entry-Block Store Propagation
 * ============================================================================
 *
 * Forward constant stores from the function entry block into deref operands
 * anywhere in the function.  Entry-block stores dominate all subsequent code,
 * so their values are valid at every point unless overwritten.
 *
 * This specifically targets the pattern where struct fields are initialized
 * before a loop and accessed inside it via LEA + ADD + deref:
 *
 *   entry:  STORE StackLoc[-56] = #4         ; cont.count = 4
 *   loop:   T = Addr[StackLoc[-68]]          ; &cont
 *           T' = T + #12                     ; &cont.count
 *           CMP V4, T'***DEREF***            ; compare against cont.count
 *
 * The pass replaces T'***DEREF*** with #4.
 *
 * SL-FWD cannot do this because it drops tracked stores at loop headers
 * (multi-predecessor basic blocks).  This pass ignores BB boundaries since
 * entry-block stores are guaranteed to dominate all code.
 */
int tcc_ir_opt_entry_store_prop(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
    return 0;

  /* Phase 1: Collect constant stores from the entry basic block.
   * Entry BB = instructions before the first jump target. */
#define MAX_ENTRY_STORES 64
  struct
  {
    int64_t offset;
    IROperand value;
    int btype;
  } estores[MAX_ENTRY_STORES];
  int estore_count = 0;

#define MAX_BC_RANGES 8
  struct
  {
    int64_t base;
    int64_t size;
  } bc_ranges[MAX_BC_RANGES];
  int bc_range_count = 0;

  for (int i = 0; i < n && estore_count < MAX_ENTRY_STORES; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->is_jump_target)
    {
      LOG_IR_GEN("ENTRY_STORE_PROP: stopped at i=%d (jump_target)", i);
      break;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      LOG_IR_GEN("ENTRY_STORE_PROP: stopped at i=%d (jump/jumpif)", i);
      break;
    }

    if (q->op == TCCIR_OP_BLOCK_COPY)
    {
      IROperand bc_dest = tcc_ir_op_get_dest(ir, q);
      IROperand bc_src = tcc_ir_op_get_src1(ir, q);
      IROperand bc_sz = tcc_ir_op_get_src2(ir, q);

      if (!bc_dest.is_local || irop_get_tag(bc_dest) != IROP_TAG_STACKOFF)
        continue;
      if (!irop_is_immediate(bc_sz))
        continue;

      int64_t base_off = irop_get_stack_offset(bc_dest);
      int total_size = (int)irop_get_imm64_ex(ir, bc_sz);
      if (total_size <= 0 || (total_size & 3) || total_size > 256)
        continue;

      size_t avail = 0;
      const uint8_t *data = ir_opt_get_rodata_bytes(ir, bc_src, &avail);
      if (!data || avail < (size_t)total_size)
        continue;

      int nwords = total_size / 4;
      for (int w = 0; w < nwords && estore_count < MAX_ENTRY_STORES; w++)
      {
        int32_t val = (int32_t)read32le((unsigned char *)(data + w * 4));
        int64_t off = base_off + w * 4;
        int found = -1;
        for (int k = 0; k < estore_count; k++)
        {
          if (estores[k].offset == off)
          {
            found = k;
            break;
          }
        }
        IROperand imm = irop_make_imm32(-1, val, IROP_BTYPE_INT32);
        if (found >= 0)
        {
          estores[found].value = imm;
          estores[found].btype = IROP_BTYPE_INT32;
        }
        else
        {
          estores[estore_count].offset = off;
          estores[estore_count].value = imm;
          estores[estore_count].btype = IROP_BTYPE_INT32;
          estore_count++;
        }
      }
      if (bc_range_count < MAX_BC_RANGES)
      {
        bc_ranges[bc_range_count].base = base_off;
        bc_ranges[bc_range_count].size = total_size;
        bc_range_count++;
      }
      LOG_IR_GEN("ENTRY_STORE_PROP: BLOCK_COPY at i=%d expanded %d words from off=%lld", i, nwords,
                 (long long)base_off);
      continue;
    }

    if (q->op != TCCIR_OP_STORE)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);

    LOG_IR_GEN("ENTRY_STORE_PROP: STORE at i=%d: dest local=%d lval=%d llocal=%d tag=%d", i, dest.is_local,
               dest.is_lval, dest.is_llocal, irop_get_tag(dest));

    /* Only direct StackLoc stores (is_local, is_lval, not through pointer) */
    if (!dest.is_local || !dest.is_lval || dest.is_llocal)
      continue;
    if (irop_get_tag(dest) != IROP_TAG_STACKOFF)
      continue;

    int64_t off = irop_get_stack_offset(dest);

    /* Only constant or stack-address values.  If neither, this store
     * overwrites a previously collected constant for the same offset —
     * invalidate the earlier entry (last-write-wins). */
    int is_const = irop_is_immediate(src1);
    int is_stackaddr = src1.is_local && !src1.is_lval && irop_get_tag(src1) == IROP_TAG_STACKOFF;
    if (!is_const && !is_stackaddr)
    {
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset == off)
          estores[k].offset = 0x7FFFFFFFLL; /* invalidate */
      }
      continue;
    }

    /* Last-write-wins: update existing entry for same offset, or add new */
    int found = -1;
    for (int k = 0; k < estore_count; k++)
    {
      if (estores[k].offset == off)
      {
        found = k;
        break;
      }
    }
    if (found >= 0)
    {
      estores[found].value = src1;
      estores[found].btype = irop_get_btype(dest);
    }
    else if (estore_count < MAX_ENTRY_STORES)
    {
      estores[estore_count].offset = off;
      estores[estore_count].value = src1;
      estores[estore_count].btype = irop_get_btype(dest);
      estore_count++;
    }
  }

  LOG_IR_GEN("ENTRY_STORE_PROP: %d entry-BB stores collected", estore_count);
  if (estore_count == 0)
    return 0;

  /* Phase 1.5: Invalidate entries for offsets that are written to ANYWHERE
   * after the entry BB.  If a stack location is modified later (e.g., loop
   * counter gof.argc++), forwarding the entry-BB value is wrong. */
  {
    int entry_bb_end = 0;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->is_jump_target || eq->op == TCCIR_OP_JUMP || eq->op == TCCIR_OP_JUMPIF)
      {
        entry_bb_end = j;
        break;
      }
    }
    for (int j = entry_bb_end; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->op == TCCIR_OP_BLOCK_COPY)
      {
        IROperand bcd = tcc_ir_op_get_dest(ir, eq);
        IROperand bcsz = tcc_ir_op_get_src2(ir, eq);
        if (bcd.is_local && irop_get_tag(bcd) == IROP_TAG_STACKOFF && irop_is_immediate(bcsz))
        {
          int64_t bbase = irop_get_stack_offset(bcd);
          int64_t bsz = irop_get_imm64_ex(ir, bcsz);
          for (int k = 0; k < estore_count; k++)
          {
            if (estores[k].offset >= bbase && estores[k].offset < bbase + bsz)
              estores[k].offset = 0x7FFFFFFFLL;
          }
        }
        continue;
      }
      if (eq->op != TCCIR_OP_STORE && eq->op != TCCIR_OP_STORE_INDEXED && eq->op != TCCIR_OP_STORE_POSTINC)
        continue;
      IROperand sd = tcc_ir_op_get_dest(ir, eq);
      if (!sd.is_local || !sd.is_lval || sd.is_llocal)
        continue;
      if (irop_get_tag(sd) != IROP_TAG_STACKOFF)
        continue;
      int64_t soff = irop_get_stack_offset(sd);
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset == soff)
        {
          LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (rewritten at i=%d)", (long long)soff, j);
          estores[k].offset = 0x7FFFFFFFLL;
        }
      }
    }
    /* Also invalidate stores whose address is taken anywhere in the function.
     * A Addr[StackLoc[X]] operand means offset X's address may escape to a
     * function call, which could write through the pointer. */
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->op == TCCIR_OP_NOP)
        continue;
      for (int si = 0; si < 2; si++)
      {
        if (si == 0 && !irop_config[eq->op].has_src1)
          continue;
        if (si == 1 && !irop_config[eq->op].has_src2)
          continue;
        IROperand op = (si == 0) ? tcc_ir_op_get_src1(ir, eq) : tcc_ir_op_get_src2(ir, eq);
        if (!op.is_local || op.is_lval || irop_get_tag(op) != IROP_TAG_STACKOFF)
          continue;
        int64_t aoff = irop_get_stack_offset(op);
        for (int k = 0; k < estore_count; k++)
        {
          if (estores[k].offset == aoff)
          {
            LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (addr taken at i=%d)", (long long)aoff, j);
            estores[k].offset = 0x7FFFFFFFLL;
          }
        }
      }
    }

    /* Remove invalidated entries */
    int valid = 0;
    for (int k = 0; k < estore_count; k++)
    {
      if (estores[k].offset != 0x7FFFFFFFLL)
        estores[valid++] = estores[k];
    }
    estore_count = valid;
  }

  if (estore_count == 0)
    return 0;

  /* Phase 2: Build LEA map — track TEMPs holding addresses of stack locals. */
  int max_tmp = 0, max_var = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(d);
      if (vr >= 0)
      {
        int p = TCCIR_DECODE_VREG_POSITION(vr);
        if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && p > max_tmp)
          max_tmp = p;
        if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && p > max_var)
          max_var = p;
      }
    }
  }

  typedef struct
  {
    int64_t offset;
    int valid;
  } SimpleLeaEntry;

  SimpleLeaEntry *lea_map = tcc_mallocz(sizeof(SimpleLeaEntry) * (max_tmp + 1));
  SimpleLeaEntry *var_lea_map = tcc_mallocz(sizeof(SimpleLeaEntry) * (max_var + 1));

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* ASSIGN/LEA with Addr[StackLoc[X]] source → record in LEA map */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (src1.is_local && !src1.is_lval && irop_get_tag(src1) == IROP_TAG_STACKOFF)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int32_t vr = irop_get_vreg(dest);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_tmp)
          {
            lea_map[p].offset = irop_get_stack_offset(src1);
            lea_map[p].valid = 1;
          }
        }
      }
    }

    /* STORE/ASSIGN: VAR <-- LEA_temp → propagate into var_lea_map */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s1_vr = irop_get_vreg(s1);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR && s1_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (sp <= max_tmp && lea_map[sp].valid && dp <= max_var)
        {
          var_lea_map[dp].offset = lea_map[sp].offset;
          var_lea_map[dp].valid = 1;
        }
      }
    }

    /* ASSIGN: TEMP <-- VAR → propagate from var_lea_map */
    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s1_vr = irop_get_vreg(s1);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP && s1_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (sp <= max_var && var_lea_map[sp].valid && dp <= max_tmp)
        {
          lea_map[dp].offset = var_lea_map[sp].offset;
          lea_map[dp].valid = 1;
        }
      }
    }

    /* ADD: LEA_temp + constant or Addr[StackLoc] + constant → propagate in LEA map */
    if (q->op == TCCIR_OP_ADD)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (dp <= max_tmp)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          int32_t s1_vr = irop_get_vreg(s1);
          if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(s2) &&
              !s2.is_sym)
          {
            int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
            if (sp <= max_tmp && lea_map[sp].valid)
            {
              lea_map[dp].offset = lea_map[sp].offset + irop_get_imm64_ex(ir, s2);
              lea_map[dp].valid = 1;
            }
          }
          else if (s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_STACKOFF && irop_is_immediate(s2) &&
                   !s2.is_sym)
          {
            lea_map[dp].offset = irop_get_stack_offset(s1) + irop_get_imm64_ex(ir, s2);
            lea_map[dp].valid = 1;
          }
        }
      }
    }
  }

  /* Phase 2.5: Invalidate entries for pointer stores through LEA-resolved TEMPs.
   * Phase 1.5 only catches direct StackLoc stores; stores like T***DEREF*** <-- #0
   * where T resolves to a known stack offset via the LEA map are missed.  After
   * inlining, struct field writes go through pointer dereferences, so this is
   * needed to prevent forwarding a stale entry-BB value past an overwrite. */
  {
    int entry_bb_end = 0;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->is_jump_target || eq->op == TCCIR_OP_JUMP || eq->op == TCCIR_OP_JUMPIF)
      {
        entry_bb_end = j;
        break;
      }
    }
    for (int j = entry_bb_end; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->op != TCCIR_OP_STORE && eq->op != TCCIR_OP_STORE_INDEXED && eq->op != TCCIR_OP_STORE_POSTINC)
        continue;
      IROperand sd = tcc_ir_op_get_dest(ir, eq);
      if (sd.is_local)
        continue;
      /* STORE_INDEXED / STORE_POSTINC always write through their base pointer.
       * disp_fusion clears is_lval on the base (see comment in ir_opt_lea_fold),
       * so a plain is_lval check would wrongly skip them and miss invalidations
       * of entry-BB stores whose stack location is overwritten via the pointer. */
      if (!sd.is_lval && eq->op == TCCIR_OP_STORE)
        continue;
      int32_t dv = irop_get_vreg(sd);
      if (dv < 0)
        continue;
      int64_t soff;
      if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
      {
        int dp = TCCIR_DECODE_VREG_POSITION(dv);
        if (dp > max_tmp || !lea_map[dp].valid)
          continue;
        soff = lea_map[dp].offset;
      }
      else if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
      {
        int dp = TCCIR_DECODE_VREG_POSITION(dv);
        if (dp > max_var || !var_lea_map[dp].valid)
          continue;
        soff = var_lea_map[dp].offset;
      }
      else
        continue;
      if (eq->op == TCCIR_OP_STORE_INDEXED)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, eq);
        if (!irop_is_immediate(s2))
          continue;
        soff += irop_get_imm64_ex(ir, s2);
      }
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset == soff)
        {
          LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (ptr store via LEA at i=%d)", (long long)soff, j);
          estores[k].offset = 0x7FFFFFFFLL;
        }
      }
    }
    int v3 = 0;
    for (int k = 0; k < estore_count; k++)
      if (estores[k].offset != 0x7FFFFFFFLL)
        estores[v3++] = estores[k];
    estore_count = v3;
  }
  if (estore_count == 0)
  {
    tcc_free(lea_map);
    tcc_free(var_lea_map);
    return 0;
  }

  /* Collect call-escaped base offsets for Phase 3b safety check.
   * Only track stack addresses passed through actual function call parameters,
   * not addresses used within inlined code. */
#define MAX_ADDRTAKEN_BASES 32
  int64_t addrtaken_bases[MAX_ADDRTAKEN_BASES];
  int addrtaken_base_count = 0;
  for (int j = 0; j < n && addrtaken_base_count < MAX_ADDRTAKEN_BASES; j++)
  {
    IRQuadCompact *eq = &ir->compact_instructions[j];
    if (eq->op != TCCIR_OP_FUNCPARAMVAL)
      continue;
    IROperand op = tcc_ir_op_get_src1(ir, eq);
    if (op.is_local && !op.is_lval && irop_get_tag(op) == IROP_TAG_STACKOFF)
    {
      int64_t aoff = irop_get_stack_offset(op);
      int dup = 0;
      for (int ab = 0; ab < addrtaken_base_count; ab++)
        if (addrtaken_bases[ab] == aoff)
        {
          dup = 1;
          break;
        }
      if (!dup)
        addrtaken_bases[addrtaken_base_count++] = aoff;
    }
    else
    {
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(vr);
        if (p <= max_tmp && lea_map[p].valid)
        {
          int64_t aoff = lea_map[p].offset;
          int dup = 0;
          for (int ab = 0; ab < addrtaken_base_count; ab++)
            if (addrtaken_bases[ab] == aoff)
            {
              dup = 1;
              break;
            }
          if (!dup && addrtaken_base_count < MAX_ADDRTAKEN_BASES)
            addrtaken_bases[addrtaken_base_count++] = aoff;
        }
      }
    }
  }

  /* Range invalidation for BLOCK_COPY: when an address within a BLOCK_COPY
   * range escapes through a function call parameter, all fields of the struct
   * could be modified.  Invalidate ALL estores entries in the range. */
  for (int j = 0; j < n && bc_range_count > 0; j++)
  {
    IRQuadCompact *eq = &ir->compact_instructions[j];
    if (eq->op != TCCIR_OP_FUNCPARAMVAL)
      continue;
    IROperand fop = tcc_ir_op_get_src1(ir, eq);
    int64_t foff = 0x7FFFFFFFLL;
    if (fop.is_local && !fop.is_lval && irop_get_tag(fop) == IROP_TAG_STACKOFF)
      foff = irop_get_stack_offset(fop);
    else
    {
      int32_t fvr = irop_get_vreg(fop);
      if (fvr >= 0 && !fop.is_lval && TCCIR_DECODE_VREG_TYPE(fvr) == TCCIR_VREG_TYPE_TEMP)
      {
        int fp = TCCIR_DECODE_VREG_POSITION(fvr);
        if (fp <= max_tmp && lea_map[fp].valid)
          foff = lea_map[fp].offset;
      }
    }
    if (foff == 0x7FFFFFFFLL)
      continue;
    for (int br = 0; br < bc_range_count; br++)
    {
      if (foff >= bc_ranges[br].base && foff < bc_ranges[br].base + bc_ranges[br].size)
      {
        for (int k = 0; k < estore_count; k++)
        {
          if (estores[k].offset >= bc_ranges[br].base && estores[k].offset < bc_ranges[br].base + bc_ranges[br].size)
            estores[k].offset = 0x7FFFFFFFLL;
        }
        break;
      }
    }
  }
  {
    int v2 = 0;
    for (int k = 0; k < estore_count; k++)
      if (estores[k].offset != 0x7FFFFFFFLL)
        estores[v2++] = estores[k];
    estore_count = v2;
  }

  /* Phase 3: Forward entry-BB stores into deref operands.
   * For each instruction, check src1 and src2 for T***DEREF*** where T
   * is in the LEA map and the resolved offset matches an entry-BB store. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Process src1 and src2 */
    for (int si = 0; si < 2; si++)
    {
      if (si == 0 && !irop_config[q->op].has_src1)
        continue;
      if (si == 1 && !irop_config[q->op].has_src2)
        continue;

      IROperand src = (si == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);

      /* Only deref operands (is_lval) */
      if (!src.is_lval)
        continue;

      /* Resolve the address through LEA map */
      int32_t vr = irop_get_vreg(src);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;

      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p > max_tmp || !lea_map[p].valid)
        continue;

      int64_t resolved_offset = lea_map[p].offset;

      /* Look up in entry-BB store table */
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset != resolved_offset)
          continue;

        /* Match! Replace deref with the stored value.
         * Reuse the original stored operand directly to preserve
         * the correct type encoding (IMM32, I64, F32, F64, etc.). */
        IROperand replacement = estores[k].value;

        if (si == 0)
          tcc_ir_op_set_src1(ir, q, replacement);
        else
          tcc_ir_op_set_src2(ir, q, replacement);

        LOG_IR_GEN("ENTRY_STORE_PROP: i=%d si=%d replaced deref at off=%lld with stored value", i, si,
                   (long long)resolved_offset);
        changes++;
        break;
      }
    }
  }

  /* Phase 3b: Forward entry-BB stores into LOAD_INDEXED instructions.
   * LOAD_INDEXED dest, base, #imm — when base is in the LEA map and
   * (lea_offset + imm) matches an entry-BB store, replace the entire
   * LOAD_INDEXED with ASSIGN of the stored value. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LOAD_INDEXED)
      continue;

    IROperand li_src1 = tcc_ir_op_get_src1(ir, q);
    IROperand li_src2 = tcc_ir_op_get_src2(ir, q);

    int32_t base_vr = irop_get_vreg(li_src1);
    if (base_vr < 0 || TCCIR_DECODE_VREG_TYPE(base_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (!irop_is_immediate(li_src2) || li_src2.is_sym)
      continue;

    int bp = TCCIR_DECODE_VREG_POSITION(base_vr);
    if (bp > max_tmp || !lea_map[bp].valid)
      continue;

    int64_t base_off = lea_map[bp].offset;
    int64_t eff_off = base_off + irop_get_imm64_ex(ir, li_src2);

    /* If the LEA base's address was taken, the struct it points to could
     * have been modified by a function call.  Skip forwarding. */
    {
      int base_addrtaken = 0;
      for (int ab = 0; ab < addrtaken_base_count; ab++)
      {
        if (addrtaken_bases[ab] == base_off)
        {
          base_addrtaken = 1;
          break;
        }
      }
      if (base_addrtaken)
        continue;
    }

    for (int k = 0; k < estore_count; k++)
    {
      if (estores[k].offset != eff_off)
        continue;
      if (estores[k].btype != irop_get_btype(li_src1))
        continue;

      q->op = TCCIR_OP_ASSIGN;
      {
        int pool_off = q->operand_base + irop_config[TCCIR_OP_ASSIGN].has_dest;
        ir->iroperand_pool[pool_off] = estores[k].value;
      }
      tcc_ir_set_src2(ir, i, IROP_NONE);

      if (estores[k].value.is_local && !estores[k].value.is_lval && irop_get_tag(estores[k].value) == IROP_TAG_STACKOFF)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int32_t d_vr = irop_get_vreg(dest);
        if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
          if (dp <= max_tmp)
          {
            lea_map[dp].offset = irop_get_stack_offset(estores[k].value);
            lea_map[dp].valid = 1;
          }
        }
      }

      LOG_IR_GEN("ENTRY_STORE_PROP: i=%d LOAD_INDEXED forwarded at eff_off=%lld", i, (long long)eff_off);
      changes++;
      break;
    }
  }

  tcc_free(lea_map);
  tcc_free(var_lea_map);

  return changes;
}


/* Bit-demand classifier for SL_FWD narrow-forwarding.
 *
 * Returns 1 if all uses of `target_vr` starting from `start_idx` need only
 * the low `load_bits` bits — meaning a wider stored value (e.g. a constant
 * -1 stored to a byte slot) can be forwarded raw without losing the byte
 * narrowing semantics.  Returns 0 if any use observes bits beyond
 * `load_bits` (conservative: any op we don't recognize as bit-narrowing is
 * treated as wide-demand).
 *
 * Recognised narrow-demand uses:
 *   - STORE / STORE_INDEXED with access width <= load_bits
 *   - AND with an immediate whose set bits all fall within the load width
 *     mask; downstream demand of the AND dest is then narrow.
 *   - XOR / OR: per-bit ops — demand on the operand equals demand on dest,
 *     so recurse on the dest's uses.
 *   - ASSIGN: pure copy — recurse on the dest.
 *
 * Everything else (SAR, SHR, CMP, SUB, ADD, MUL, RETURNVALUE, FUNCPARAMVAL,
 * stores wider than load, BB boundary, unknown op) → wide.
 *
 * Scoped to the LOAD's basic block to keep the scan O(BB-size) and avoid
 * cross-BB control-flow reasoning.
 */
static int sl_fwd_narrow_demand_only(TCCIRState *ir, int32_t target_vr, int start_idx, int load_bits, int depth)
{
  if (depth > 6)
    return 0; /* Limit recursion */
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

    /* Detect whether this op reads target_vr in src1 / src2 / accum. */
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

    /* Track redefinition of target_vr by anything that writes to it — stops
     * tracking further uses since they refer to a new value. */
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
      /* Access width: for plain STORE it's the dest's btype (the slot/deref
       * width); for STORE_INDEXED it's the src1 btype (since the dest is a
       * register-typed base address, not the access slot). */
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
      /* Must be storing target_vr as the value (src1).  If we get here only
       * because of an address-vreg match that happens to read target_vr,
       * that's a use of the address vreg, not a narrow-store consumption of
       * the loaded byte — fall through to wide. */
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
      /* Bitwise ops are per-bit: result bit i depends only on input bit i.
       * If any user of the result reads only the low load_bits, the operand's
       * demand is also narrow. */
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

/* Store-Load Forwarding
 * Phase 4: Replace loads from addresses that were just stored to with the stored value
 * Uses conservative basic-block-local alias analysis:
 *   - Stack locals (VT_LOCAL) never alias pointer derefs
 *   - Track base vreg + offset for array accesses
 *   - Clear all pointer-based stores at unknown stores
 *   - Clear all stores at basic block boundaries and function calls
 */
int tcc_ir_opt_sl_forward(TCCIRState *ir)
{
  typedef struct StoreEntry
  {
    int valid;
    int addr_addrtaken;     /* 1 if address of this local is taken */
    int addr_via_pointer;   /* 1 if store was resolved through LEA map (pointer) */
    int64_t local_offset;   /* stack offset or symref addend */
    const Sym *local_sym;   /* symbol for VT_LOCAL (NULL for pure stack offsets) */
    IROperand stored_value; /* IROperand of the stored value */
    int instruction_idx;    /* where the store happened */
    int store_dest_vr;      /* vreg of the store destination (address) */
    int store_btype;        /* btype of the store address (access width) */
    struct StoreEntry *next;
  } StoreEntry;

  /* Track last write index for each vreg to detect intervening writes.
   * When a LOAD's address vreg was written AFTER a matching store,
   * the store-load forward is invalid because the vreg now holds a
   * different value than what was stored. */
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

  /* Pre-pass: recompute is_jump_target flags from actual jump instructions.
   * After optimization passes (e.g. trivial JMP→NOP), some instructions may
   * still have is_jump_target set even though no JUMP/JUMPIF targets them
   * anymore.  These stale flags create artificial BB boundaries that prevent
   * store-load forwarding from seeing through.
   *
   * At the same time, compute pred_count[t] = number of control-flow
   * predecessors for each instruction t.  This is later used to extend
   * forwarding across BB boundaries when a target has exactly one
   * predecessor (either a single incoming JUMP, or plain fall-through). */
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
    }
    /* Fall-through predecessors: instruction i+1 is reached from i unless i
     * is a terminator (JUMP, RETURNVALUE, RETURNVOID). */
    for (i = 0; i + 1 < n; i++)
    {
      IRQuadCompact *fq = &ir->compact_instructions[i];
      if (fq->op != TCCIR_OP_JUMP && fq->op != TCCIR_OP_RETURNVALUE && fq->op != TCCIR_OP_RETURNVOID)
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

  /* Allocate vreg write trackers for all three vreg types.
   * Using generation counter so we don't need to clear on block boundaries. */
  int write_tracker_gen = 1;
  int max_var = ir->next_local_variable;
  int max_tmp = ir->next_temporary_variable;
  int max_par = ir->next_parameter;
  VregWriteTracker *var_writes = tcc_mallocz(sizeof(VregWriteTracker) * (max_var + 1));
  VregWriteTracker *tmp_writes = tcc_mallocz(sizeof(VregWriteTracker) * (max_tmp + 1));
  VregWriteTracker *par_writes = tcc_mallocz(sizeof(VregWriteTracker) * (max_par + 1));

  /* LEA map: track TEMPs that hold addresses of stack locals.
   * Used to resolve LEA-based memory accesses (e.g. struct field access via
   * LEA T = &StackLoc[X]; STORE V = T***DEREF***) back to direct StackLoc refs
   * so that store-load forwarding can propagate values through them. */
  typedef struct
  {
    int64_t offset; /* resolved stack offset */
    const Sym *sym; /* local symbol (NULL for anonymous) */
    int valid;      /* 1 if entry is valid */
    int lea_idx;    /* instruction index of the LEA that created this entry */
  } LeaMapEntry;

  LeaMapEntry *lea_map = tcc_mallocz(sizeof(LeaMapEntry) * (max_tmp + 1));

  /* Set of addrtaken stack slots — any (sym, offset) that appears as the
   * source of a LEA/address-of anywhere in the function.  Stores to such
   * slots must be invalidated across function calls since the address may
   * have escaped.  Anonymous stack slots (no vreg) don't have an
   * IRLiveInterval::addrtaken flag, so without this set the STORE handler
   * leaves addr_addrtaken=0 and the CALL handler keeps stale entries. */
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

  /* VAR LEA map: track VARs that hold addresses of stack locals.  Enables
   * forwarding through the pattern:  LEA T = &StackLoc; STORE V = T;
   * ASSIGN T' = V; LOAD _ = T'***DEREF***  — which otherwise loses the LEA
   * info at the VAR hand-off step.  Only single-def VARs are tracked, so the
   * LEA value read back is definitively the one stored. */
  LeaMapEntry *var_lea_map = tcc_mallocz(sizeof(LeaMapEntry) * (max_var + 1));
  uint8_t *var_def_count = tcc_mallocz(max_var + 1);

  /* Count VAR defs in one pass; cap at 2 since we only need the single-def bit.
   * Note: VAR STORE dests carry is_lval=1 (the VAR slot is written through its
   * storage address), but they are still definitions of the VAR. */
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

  /* Pre-scan: build set of active call_ids (those with a FUNCCALL instruction).
   * FUNCPARAMVAL instructions whose call_id has no matching FUNCCALL are dead
   * (e.g. from inlined function calls) and should not create addrtaken entries. */
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
      /* LEA from a StackLoc (is_local=1, is_lval=0 means address-of).
       * Only record concrete stack addresses (STACKOFF or SYMREF).  VAR-tagged
       * operands (e.g. `&V3`) have no resolved u.imm32 here — they all read as
       * offset=0, which would collide in the {sym,offset}-keyed hash table and
       * alias distinct VARs together.  Stores to VARs themselves are already
       * tracked via the normal VAR STORE path, so skipping them costs nothing. */
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
            /* VAR/PARAM-backed local: the IR operand's u.imm32 is unresolved
             * (typically 0) at this point.  Fall back to the allocated stack
             * slot for the vreg, so that distinct VARs don't collide at
             * (sym=NULL, offset=0) in the hash table. */
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
            /* Mark this slot as addrtaken: a LEA exposed its address, so any
             * function call after a STORE here could mutate it via the
             * escaping pointer.  Record the earliest LEA instruction index
             * so that CALL-time invalidation only fires when the CALL actually
             * happens after the LEA in program order (the pointer can't
             * escape until the LEA executes). */
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
          /* Check: LEA_temp + constant */
          int32_t s1_vr = irop_get_vreg(lsrc1);
          int32_t s2_vr = irop_get_vreg(lsrc2);
          if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(lsrc2) &&
              !lsrc2.is_sym)
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
          else if (s2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s2_vr) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(lsrc1) &&
                   !lsrc1.is_sym)
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
      /* VAR <-- TEMP_in_lea_map [STORE|ASSIGN] on a single-def VAR:
       * record VAR as holding the LEA's stack address. */
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
      /* TEMP <-- Addr[StackLoc[X]] [ASSIGN]: direct stack address assigned
       * to a TEMP, e.g. after const-prop folded an ADD+SHL chain into a
       * plain ASSIGN of the base address. */
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

    /* Direct-address escape tracking.  Any instruction whose src1/src2
     * carries an `Addr[StackLoc]` operand (STACKOFF, is_lval=0) effectively
     * takes the address of that stack slot — the most common case is
     * FUNCPARAMVAL passing `Addr[StackLoc[-N]]` directly without first
     * materializing a TEMP via LEA.  Without this the CALL-time
     * invalidation loop never learns the slot is reachable through the
     * callee's pointer argument, so a tracked store can be incorrectly
     * forwarded past the call (regression in pr86844.c where foo(a)
     * mutates *a but SL_FWD forwarded the pre-call init value). */
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
        /* STACKOFF with a vreg attached: offset comes from the vreg's
         * spill slot, not u.imm32 — skip for now (those paths are already
         * tracked via the vreg-based LEA map). */
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

  /* Compute max_access_end for each addrtaken slot from LEA+ADD patterns.
   * When a derived address (base + offset) exists in the lea_map, the owning
   * addrtaken slot's object extends at least to (derived_offset + word_size).
   * This allows precise overlap checks when the stack layout is unavailable. */
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

  /* Track constants assigned to TEMPs by forwarding, so that subsequent
   * stores of those TEMPs can use the resolved constant value instead. */
  IROperand *fwd_tmp_val = tcc_mallocz(sizeof(IROperand) * (max_tmp + 1));
  uint8_t *fwd_tmp_valid = tcc_mallocz(max_tmp + 1);

  /* Pre-populate fwd_tmp from existing constant ASSIGN/LOAD instructions.
   * Earlier optimization passes (value tracking, const prop) may have already
   * created T <-- #const [ASSIGN] or T <-- #const [LOAD] instructions.
   * The LOAD case arises when value tracking replaces a variable with its known
   * constant value (e.g., T0 <-- V0 [LOAD] → T0 <-- #7 [LOAD]).
   *
   * IMPORTANT: A TEMP with multiple definitions (e.g. on different control-flow
   * paths) must NOT be tracked, since the linear pre-scan cannot determine which
   * definition reaches a given use.  We use fwd_tmp_defs[] to count definitions
   * and permanently reject any TEMP defined more than once. */
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

    /* Single-def ASSIGN or LOAD with immediate constant — track it.
     * For LOAD, value tracking may have replaced the source variable with a
     * constant, producing T <-- #imm [LOAD] which means "T = imm". */
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
    /* Non-constant or unsupported definition — don't track */
  }
  tcc_free(fwd_tmp_defs);

  /* Cross-BB state preservation: when a JUMP/JUMPIF's target has exactly
   * one predecessor, we snapshot the current state to saved_entries[t] so
   * the target BB can restore and continue forwarding.  This extends the
   * scope of forwarding across BB boundaries without requiring a full
   * data-flow join analysis. */
  StoreEntry **saved_entries = tcc_mallocz(sizeof(StoreEntry *) * n);
  int *saved_entry_count = tcc_mallocz(sizeof(int) * n);

  LOG_IR_GEN("=== STORE-LOAD FORWARDING START ===");

  for (i = 0; i < n; i++)
  {
    q = &ir->compact_instructions[i];

    /* BB exits: JUMP and JUMPIF optionally snapshot their target's state for
     * single-predecessor targets.  JUMP terminates the current path (no
     * fall-through).  JUMPIF has both a target and a fall-through path —
     * the fall-through inherits the current state. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand jdest = tcc_ir_op_get_dest(ir, q);
      int jtarget = (int)jdest.u.imm32;
      if (jtarget >= 0 && jtarget < n && entry_count > 0)
      {
        /* Snapshot entries[] for the target to restore/join on entry */
        if (!saved_entries[jtarget])
          saved_entries[jtarget] = tcc_malloc(sizeof(StoreEntry) * n);
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
    if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      memset(hash_table, 0, sizeof(hash_table));
      entry_count = 0;
      write_tracker_gen++;
      continue;
    }
    if (q->is_jump_target)
    {
      /* Multi-predecessor target: reset (can't safely merge states).
       * Single-predecessor target: restore from snapshot if predecessor was
       * a JUMP (i.e. i-1 is JUMP or RETURN, so no fall-through).  Otherwise
       * the predecessor is the fall-through — keep current state intact. */
      if (pred_count[i] > 1)
      {
        /* Multi-pred join: if we have a saved snapshot from a JUMP predecessor,
         * intersect it with the current (fall-through) state.  Keep only stores
         * present in both with the same (sym, offset).  This preserves forwarding
         * across diamond patterns (if/else) where neither branch modifies the
         * tracked stack slots. */
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
              /* Safety: drop stores that could be aliased through an addrtaken
               * pointer (e.g. struct fields whose base address escapes).  After
               * LEA resolution, per-field addrtaken entries may be lost, so a
               * function call on any path could have modified this store
               * through the escaped pointer. */
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
                  /* Any addrtaken slot with same sym: conservatively assume
                   * the escaped pointer could reach this store's offset. */
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
          int had_entries = entry_count;
          /* Restore from snapshot if available; otherwise reset */
          memset(hash_table, 0, sizeof(hash_table));
          entry_count = 0;
          write_tracker_gen++;
          if (saved_entries[i])
          {
            int sc = saved_entry_count[i];
            LOG_SL_FWD("BB@i=%d RESTORE: single-pred target, restoring %d snapshot entries (was %d)", i, sc,
                       had_entries);
            memcpy(entries, saved_entries[i], sizeof(StoreEntry) * sc);
            entry_count = sc;
            /* Rebuild hash chains */
            for (int k = 0; k < sc; k++)
            {
              uint32_t h = ((uintptr_t)entries[k].local_sym * 31 + (uint32_t)entries[k].local_offset * 17) % 128;
              entries[k].next = hash_table[h];
              hash_table[h] = &entries[k];
            }
          }
          else if (had_entries > 0)
          {
            LOG_SL_FWD("BB@i=%d RESET: single-pred terminator target but no snapshot (dropped %d entries)", i,
                       had_entries);
          }
        }
        /* else: fall-through predecessor — keep current state intact */
      }
    }
    /* Function calls: only invalidate stores to escaped locals (addrtaken).
     * Stack locals whose address has NOT been taken cannot be modified
     * by any function call since no external code has a pointer to them.
     *
     * Anonymous stack slots (no IRLiveInterval) get their addrtaken
     * property from the pre-scanned addrtaken_slots set.  Only invalidate
     * if some LEA for this slot appeared at an instruction index <= this
     * CALL's index — otherwise the pointer hasn't escaped yet and the
     * CALL can't reach the slot. */
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      /* Known pure AEABI calls: these only compute a result from their
       * arguments and never modify memory, so tracked stores remain valid. */
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
          /* Match by sym only for named locals — a taken address may reach any
           * offset within the same symbol via pointer arithmetic (e.g. struct
           * fields, array elements, negative indexing).
           *
           * For anonymous STACKOFF (sym==NULL), use stack-layout overlap so an
           * escaped temp struct does not invalidate unrelated anonymous locals
           * that merely share the NULL sym namespace. */
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
      /* For FUNCCALLVAL, the dest vreg is redefined — invalidate stores
       * whose stored_value was that vreg and track the write. */
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

    /* Process LOAD, ASSIGN-with-deref, and single-value FUNCPARAMVAL
     * instructions: forward from a previous store.
     * ASSIGN with is_lval src1 is semantically a LOAD (e.g. after LEA fold).
     * FUNCPARAMVAL with is_lval stack-loc src1 is essentially an embedded
     * LOAD, but only when it passes a single scalar:
     *  - complex types copy multiple words from the stack reference, so a
     *    single store can't cover the entire value;
     *  - VAR vregs use abstract positions, not real stack offsets.
     * 64-bit scalars (double, long long) are allowed: the matching store
     * is a single 64-bit store and the width check in the entry-scan loop
     * keeps the forwarding well-defined. */
    if (q->op == TCCIR_OP_LOAD ||
        (q->op == TCCIR_OP_ASSIGN && tcc_ir_op_get_src1(ir, q).is_lval &&
         !(irop_get_vreg(tcc_ir_op_get_src1(ir, q)) >= 0 &&
           TCCIR_DECODE_VREG_TYPE(irop_get_vreg(tcc_ir_op_get_src1(ir, q))) == TCCIR_VREG_TYPE_VAR)) ||
        (q->op == TCCIR_OP_FUNCPARAMVAL && tcc_ir_op_get_src1(ir, q).is_local && tcc_ir_op_get_src1(ir, q).is_lval &&
         !tcc_ir_op_get_src1(ir, q).is_complex &&
         !(irop_get_vreg(tcc_ir_op_get_src1(ir, q)) >= 0 &&
           TCCIR_DECODE_VREG_TYPE(irop_get_vreg(tcc_ir_op_get_src1(ir, q))) == TCCIR_VREG_TYPE_VAR)))
    {
      /* LOAD: dest <- src1***DEREF***
       * src1 is the address to load from */
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t addr_vr = irop_get_vreg(src1);
      const Sym *addr_sym;
      int64_t addr_offset;
      uint32_t h;
      StoreEntry *e;

      /* CONSERVATIVE: Only forward for stack locals, or non-locals that
       * can be resolved to a known stack location via the LEA map. */
      int load_via_lea = 0;
      if (!src1.is_local)
      {
        /* Check if src1 is a TEMP***DEREF*** in the LEA map */
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

      /* Check if address is taken - if so, skip forwarding (may alias through pointer) */
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
        {
          LOG_SL_FWD("LOAD@i=%d SKIP: addr vreg:%d addrtaken", i, addr_vr);
          continue;
        }
      }

      /* Extract sym and offset from the local address operand.
       * For VAR-type vregs, the raw u.imm32 may be 0 (unresolved).
       * Fall back to the allocated stack slot to get a real offset
       * that matches LEA-resolved pointer stores. */
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
      /* VAR-backed locals use a stack offset that can numerically collide with
       * an anonymous StackLoc offset.  The STORE side disambiguates by stamping
       * VAR slots with a sentinel sym (see resolved_local_store); mirror that
       * here for direct VAR loads so a VAR load never alias-matches an anonymous
       * StackLoc store at the same offset.  LEA-resolved loads keep their real
       * sym (addr_vr is the TEMP holding the address, not a VAR). */
      if (!load_via_lea && addr_vr >= 0 && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
        addr_sym = (const Sym *)(uintptr_t)1; /* sentinel: VAR namespace */

      /* For VT_LOCAL, hash on symbol pointer and offset */
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

      /* Search for matching store.  addr_addrtaken entries are valid as long
       * as they haven't been invalidated by a CALL (handled at line ~6938)
       * or by an unknown-pointer STORE (handled at line ~7271) — both of
       * which clear the entry.  Within the same BB, between STORE and LOAD,
       * with no such invalidation, forwarding is safe even if the slot's
       * address was taken elsewhere. */
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
          /* Width check: don't forward if store and load access different widths.
           * E.g. a 32-bit store to StackLoc[-8] must not be forwarded to a
           * 64-bit load from StackLoc[-8] (the load reads additional bytes).
           *
           * Exception: narrower load from a wider *constant* store at the
           * same offset can forward the masked low bits as a new const.
           * Handles `int-store 4; byte-load` style patterns that show up
           * after struct sret + byte field reads. */
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
            /* Same-size cross-type (union punning): INT64↔FLOAT64, INT32↔FLOAT32.
             * Only forward immediate values — non-immediate (vreg) forwarding
             * across btype boundaries can interact badly with downstream passes
             * that key off operand btype (e.g. inlined-callee parameter slots).
             *
             * Skip when the load operand has a VAR base: a VAR vreg can be
             * allocated to a stack offset that overlaps an earlier tracked store
             * after stack reuse (e.g. call result stored into a slot that was
             * previously holding an argument).  The forwarding tracker doesn't
             * see the reuse, so cross-type forward could pick up a stale value.
             * Direct stack-offset loads (addr_vr < 0) are safe.
             *
             * For 64-bit pool-backed immediates the bits live in different pools
             * (i64 vs f64), so re-pool when crossing INT64↔FLOAT64.  For 32-bit
             * immediates the storage is shared via the union (u.imm32 /
             * u.f32_bits), so a btype/tag swap suffices. */
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
              /* Replace the LOAD with ASSIGN of the masked constant.
               * Keep FUNCPARAMVAL as-is — only replace the deref src1. */
              if (q->op != TCCIR_OP_FUNCPARAMVAL)
                q->op = TCCIR_OP_ASSIGN;
              int pool_off = q->operand_base + irop_config[q->op].has_dest;
              ir->iroperand_pool[pool_off] = irop_make_imm32(-1, narrow, src1.btype);
              /* Track forwarded value for transitive forwarding, same as the
               * regular forwarding path does below. */
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
              break; /* exit the entry-scan loop; move to next LOAD */
            }
            LOG_SL_FWD("LOAD@i=%d REJECT width: store btype=%d vs load btype=%d (store at i=%d)", i,
                       (int)e->store_btype, (int)src1.btype, e->instruction_idx);
            rejected_width++;
            continue;
          }

          /* Narrow-access width-correctness check.  Both STORE and LOAD have
           * matching access widths (e->store_btype == src1.btype) here, but
           * for sub-32-bit widths the STORE writes only the low N bits and
           * the LOAD reads only those N bits (zero/sign-extended).  The
           * stored_value carried in `e` may have wider 32-bit bits set
           * (e.g. constant -1 stored to a byte slot), so forwarding it raw
           * would skip the implicit narrowing.
           *
           * For immediates we can compute the value the LOAD would actually
           * produce.  If it matches stored_value's 32-bit representation,
           * forwarding raw is correct.  If it differs, we must either mask
           * or skip — but masking can be pessimistic when downstream uses
           * only need the low N bits (e.g. byte vector ops that XOR then
           * STORE byte).  Demand analysis decides: forward raw if all uses
           * of the LOAD's dest can be proven to need only the low N bits;
           * otherwise replace with a masked immediate. */
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

          /* Safety check: if the LOAD's address vreg was written AFTER the
           * matching store, the store entry is stale. This happens when:
           * 1. STORE val → stack_slot[-88]  (records stored_value)
           * 2. AND/ADD/etc → VARx           (writes to VARx which lives at -88)
           * 3. LOAD VARx → dest             (should read VARx's register value, not step 1's value)
           * Without this check, step 3 incorrectly forwards step 1's value.
           *
           * Skip this check when the LOAD was resolved via the LEA map: the
           * address TEMP/VAR just holds the LEA result, so its def time is
           * independent of the stack-slot's value timeline. */
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
              /* The LOAD's address vreg was written after the store — skip */
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
          /* For FUNCPARAMVAL: keep the op, just replace the deref src1 with
           * the stored value.  For LOAD: convert to ASSIGN. */
          if (q->op != TCCIR_OP_FUNCPARAMVAL)
            q->op = TCCIR_OP_ASSIGN;
          /* Write stored value to both pools for src1 slot */
          int pool_off = q->operand_base + irop_config[q->op].has_dest;
          ir->iroperand_pool[pool_off] = e->stored_value;
          /* Track the assigned value for transitive forwarding:
           * If T2 <-- #7 [ASSIGN], record so that later STORE loc <-- T2
           * can use #7 directly instead of T2. */
          {
            IROperand fwd_dest = tcc_ir_op_get_dest(ir, q);
            int32_t fwd_dest_vr = irop_get_vreg(fwd_dest);
            if (fwd_dest_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_dest_vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int fwd_pos = TCCIR_DECODE_VREG_POSITION(fwd_dest_vr);
              if (fwd_pos <= max_tmp)
              {
                IROperand fwd_sv = e->stored_value;
                /* Transitive resolution: if stored_value is a TEMP in fwd_tmp,
                 * resolve to its underlying constant */
                int32_t fwd_sv_vr = irop_get_vreg(fwd_sv);
                if (fwd_sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(fwd_sv_vr) == TCCIR_VREG_TYPE_TEMP && !fwd_sv.is_lval)
                {
                  int fwd_sv_pos = TCCIR_DECODE_VREG_POSITION(fwd_sv_vr);
                  if (fwd_sv_pos <= max_tmp && fwd_tmp_valid[fwd_sv_pos])
                    fwd_sv = fwd_tmp_val[fwd_sv_pos];
                }
                fwd_tmp_val[fwd_pos] = fwd_sv;
                fwd_tmp_valid[fwd_pos] = 1;
                /* LEA map propagation: if the stored value is a TEMP in the
                 * LEA map, propagate to the destination TEMP.  This handles
                 * the pattern: T28 = &StackLoc; V21 = T28; T29 = V21
                 * After forwarding, T29 = T28 [ASSIGN] — so T29 should
                 * inherit T28's LEA map entry for pointer-store resolution. */
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
          /* Record this store as a candidate for dead-store elimination.
           * After the main loop we check if anyone still reads from its offset. */
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
      /* Cross-offset: 32-bit load from offset X may read the upper half of
       * a 64-bit constant store at offset X-4. Probe the hash table there. */
      if (e == NULL && src1.btype == IROP_BTYPE_INT32)
      {
        int64_t lo_offset = addr_offset - 4;
        uint32_t lo_h = ((uintptr_t)addr_sym * 31 + (uint32_t)lo_offset * 17) % 128;
        StoreEntry *lo_e;
        for (lo_e = hash_table[lo_h]; lo_e != NULL; lo_e = lo_e->next)
        {
          if (!lo_e->valid || lo_e->local_sym != addr_sym || lo_e->local_offset != lo_offset)
            continue;
          if (lo_e->store_btype != IROP_BTYPE_INT64 || !irop_is_immediate(lo_e->stored_value))
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
      /* Wider 64-bit load synthesized from two adjacent 32-bit constant stores.
       * Pattern: STORE32 [X]=lo_imm; STORE32 [X+4]=hi_imm; LOAD64 [X] -> hi:lo.
       * Generated by struct/bitfield zero-init followed by a 64-bit bitfield
       * read of the container, where the IR generator emits per-word constant
       * stores but the read is a single 64-bit access. */
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
            /* Restrict to values that fit in a signed 32-bit immediate to avoid
             * exposing constprop bugs around 64-bit narrowing assigns. The common
             * bitfield zero-init case (small constants) is fully covered. */
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
      /* Sub-byte/sub-short LOAD from a wider constant STORE at a lower
       * offset.  E.g. LOAD8 at offset -1 may extract a byte from a
       * 32-bit constant STORE at offset -4 (delta=3, byte index 3).
       * Symmetric to the STORE-side CROSS-MERGE logic, but applied at
       * LOAD time to pick up bytes from a wider tracked store. */
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
            /* Skip when the LOAD's address vreg was written AFTER the
             * matching store — same staleness check as the regular path. */
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
            uint32_t full = (uint32_t)prev_e->stored_value.u.imm32;
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
                       "load_bytes=%d full=0x%x narrow=%d",
                       i, prev_e->instruction_idx, delta, entry_bytes, load_bytes, full, narrow);
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
      /* If we fell out of the search without forwarding, explain why in
       * aggregate. "no match" means the hash bucket had no matching entry
       * at all (either the STORE was never tracked, was invalidated, or is
       * in a different BB not reachable via snapshot). */
      if (e == NULL)
      {
        LOG_SL_FWD("LOAD@i=%d NOMATCH: sym=%p off=%lld matched=%d width_rej=%d stale_rej=%d invalid_rej=%d", i,
                   (const void *)addr_sym, (long long)addr_offset, matched_any, rejected_width, rejected_stale_tracker,
                   rejected_invalid);
      }
    }
    /* LOAD_INDEXED with LEA-mapped base + constant index:
     * dest = *(base + #imm) where base is in the LEA map → resolve to
     * base_offset + imm and forward from hash table. */
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
          int64_t eff_off = lea_map[bp].offset + irop_get_imm64_ex(ir, li_src2);
          const Sym *eff_sym = lea_map[bp].sym;
          uint32_t lih = ((uintptr_t)eff_sym * 31 + (uint32_t)eff_off * 17) % 128;
          StoreEntry *lie;
          /* Access width: for LOAD_INDEXED, src1 is the base pointer whose
           * btype is the pointer type (often 0/NONE) — not the access width.
           * The dest's btype encodes the loaded value's width, which is what
           * we must match against the store_btype recorded when the value
           * was stored. */
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
              /* TEMP/VAR/PARAM forwarding: stored_value is a register-holding
               * vreg.  Safe to forward only when (a) the value is a true vreg
               * (not an lvalue/deref), and (b) that vreg has not been written
               * to between the originating STORE and this LOAD_INDEXED.
               * TEMPs in TCC IR are mostly SSA-like, but a conservative check
               * via the write tracker mirrors what the plain-LOAD path does. */
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
    /* Process TEST_ZERO / CMP with memory operands: forward stored values.
     * TEST_ZERO StackLoc[X] implicitly loads from the stack location.
     * If we have a tracked store to that location, replace the memory
     * operand with the stored value (e.g. TEST_ZERO #0). */
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
            /* Replace TEST_ZERO's memory src1 with the stored value */
            int pool_off = q->operand_base; /* TEST_ZERO: has_dest=0, src1 at base */
            ir->iroperand_pool[pool_off] = e->stored_value;
            changes++;
            break;
          }
        }
      }
    skip_test_zero_fwd:;
    }
    /* Forward tracked IMM32 stores into lval sources of ALU/comparison ops.
     * Pattern: `R0 <-- StackLoc[-4] AND #15` where StackLoc[-4] has a
     * tracked IMM32 store gets rewritten to `R0 <-- #const AND #15`; the
     * subsequent const_prop pass then folds to `R0 <-- #(const & 15)`, and
     * branch_folding collapses any CMP that depends on it.  Without this
     * the forwarding only fires on explicit TCCIR_OP_LOAD ops, leaving
     * struct-field read-and-test patterns unfolded. */
    else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_OR ||
             q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_SHL || q->op == TCCIR_OP_SHR || q->op == TCCIR_OP_SAR ||
             q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_CMP)
    {
      for (int si = 0; si < 2; si++)
      {
        IROperand src = (si == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        if (!src.is_lval)
          continue;
        /* Resolve the stack/symbol address — either directly for locals,
         * or through the LEA map for TEMP***DEREF*** operands. */
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
          /* Skip VAR-type vregs: VARs can be redefined by ALU ops (e.g.
           * `V0 <-- #0 SUB V0`), which SL_FWD doesn't track as stores.
           * Forwarding from a stale VAR store would produce wrong values.
           * VAR constants are handled by const_prop/const_var_prop instead. */
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
          /* Accept integer-immediate or vreg stored values.
           * IMM32/I64: forward the constant into the ALU operand.
           * VREG: replace the lval deref with a direct vreg reference. */
          int sv_tag = irop_get_tag(e->stored_value);
          int32_t sv_vr = irop_get_vreg(e->stored_value);
          int sv_is_vreg = (sv_vr >= 0 && !e->stored_value.is_lval && sv_tag == IROP_TAG_VREG);
          if (!sv_is_vreg && sv_tag != IROP_TAG_IMM32 && sv_tag != IROP_TAG_I64)
            continue;

          if (sv_is_vreg && e->store_btype == src.btype)
          {
            IROperand replacement = e->stored_value;
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
          /* Extract the 64-bit value regardless of tag (IMM32 vs I64-via-pool),
           * then narrow to 32 bits.  For int stores the low 32 bits are the
           * entire payload; this keeps the rewriter simple and always emits
           * an IMM32 operand. */
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
        /* Wider 64-bit ALU operand synthesized from two adjacent 32-bit
         * constant stores at offset and offset+4 (little-endian). Pattern
         * from bitfield container zero-init followed by 64-bit RMW. */
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
            /* Same int32-fit restriction as the LOAD path — see comment there. */
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
    /* STORE_INDEXED / STORE_POSTINC are pointer-based stores.
     *
     * For STORE_INDEXED with scale=0, an immediate index, and a base TEMP
     * resolved by the LEA map, we know the exact stack location being
     * written: StackLoc[lea_map[base].offset + index].  Track such stores
     * the same way as a plain STORE so that subsequent loads/CMPs at that
     * (or aliasing) offset can be forwarded.  This lets inlined struct
     * field writes (the fill_big pattern: *p=v0; *(p+4)=v1; *(p+8)=v2; ...)
     * forward through to direct StackLoc reads in the caller.
     *
     * Otherwise — STORE_POSTINC, scale!=0, non-immediate index, or base
     * not in the LEA map — fall back to conservative blanket invalidation:
     * the pointer might alias any tracked slot.  Without this, disp_fusion's
     * STORE -> STORE_INDEXED rewrites would leave the forwarding table
     * thinking a slot still holds its initializer value after a real write
     * through that slot's address. */
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

            /* Access width comes from the value being stored (src1),
             * since STORE_INDEXED's dest is a register-typed base, not
             * the access slot.  Default to INT32 if the value's btype is
             * unknown (matches the most common 32-bit case). */
            int store_btype = si_src1.btype;
            if (store_btype != IROP_BTYPE_INT8 && store_btype != IROP_BTYPE_INT16 &&
                store_btype != IROP_BTYPE_INT32 && store_btype != IROP_BTYPE_INT64 &&
                store_btype != IROP_BTYPE_FLOAT32 && store_btype != IROP_BTYPE_FLOAT64)
              store_btype = IROP_BTYPE_INT32;

            /* Record the store. */
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
      /* Forward tracked StackLoc values into the src1 of this STORE.
       * Pattern: STORE StackLoc[X] <- val; ... ; STORE dest <- StackLoc[X]
       * Transform: STORE dest <- val (eliminates the stack load) */
      {
        IROperand stsrc1 = tcc_ir_op_get_src1(ir, q);
        if (stsrc1.is_local && stsrc1.is_lval)
        {
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

          int32_t s_vr = irop_get_vreg(stsrc1);
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
              if (fwd_store_count < SL_FWD_MAX_DEAD_STORES && !se->addr_addrtaken)
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

      /* STORE: dest***DEREF*** <- src1
       * dest is the address, src1 is the value to store */
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t addr_vr = irop_get_vreg(dest);
      const Sym *addr_sym;
      int64_t addr_offset;
      int addr_addrtaken = 0;
      int addr_via_pointer = 0;
      uint32_t h;
      StoreEntry *new_entry = NULL;
      int j;

      /* CONSERVATIVE: Only track stack locals for forwarding.
       * However, if the pointer is in the LEA map (i.e. we know it points to
       * a specific stack location), treat it as a local store instead. */
      if (!dest.is_local)
      {
        /* VAR dest: this is a write to a local variable's own storage,
         * not a pointer write.  It can't alias any tracked stack slot,
         * so leave the hash table intact.  (STOREs to VAR carry is_lval=1
         * since the VAR slot is written through its storage address;
         * ASSIGNs to VAR have is_lval=0.  Skip both.) */
        {
          int32_t dv = irop_get_vreg(dest);
          if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
            continue;
        }
        /* Check if dest is a TEMP in the LEA map — if so, resolve to local */
        int resolved_to_local = 0;
        if (dest.is_lval)
        {
          int32_t dv = irop_get_vreg(dest);
          if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
          {
            int dp = TCCIR_DECODE_VREG_POSITION(dv);
            if (dp <= max_tmp && lea_map[dp].valid)
            {
              /* Resolved: treat as local store at the resolved offset.
               * Mark as via_pointer so function calls invalidate it. */
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
        /* Fall through to normal store tracking with resolved addr_sym/addr_offset */
        goto resolved_local_store;
      }

      /* Check if address of this local is taken */
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
          addr_addrtaken = 1;
      }

      /* Extract sym and offset from the local address operand */
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
      /* VAR destinations use stack offsets that can coincidentally collide
       * with anonymous StackLoc offsets in the hash table.  Use a distinct
       * sentinel sym pointer so VARs hash to different buckets than StackLocs. */
      if (addr_vr >= 0 && TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_VAR)
        addr_sym = (const Sym *)(uintptr_t)1; /* sentinel: VAR namespace */

      /* For VT_LOCAL, hash on symbol pointer and offset */
      h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;

      /* Partial-overwrite merge: if an existing valid entry at the same
       * offset was a wider constant store, and the new store is a narrower
       * constant store that lands on the low bits, merge values into the
       * existing entry instead of invalidating it.  This lets a subsequent
       * wider load forward the merged constant — matching C's byte-overlay
       * semantics for compound-literal init + byte-field writes.
       *
       * Example (make_opcode's `(Opcode){4, encoded}` sret init):
       *   int-store  #0 at -56   (zero-init compound literal)
       *   byte-store #4 at -56   (size = 4)
       *   int-load   from -56    → forwards as int (0 & 0xFFFFFF00) | (4 & 0xFF) = 4
       */
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
          continue; /* keep other entries (if any) walking — don't invalidate the merged one */
        }
        /* Not a mergeable overwrite — invalidate as before */
        new_entry->valid = 0;
      }
      /* Wider-entry overlap with narrower store.  A store of N bytes at
       * offset X overlaps any wider entry at offset Y < X where
       * Y + entry_bytes > X.  When both values are constant immediates and
       * the wider entry is <=32 bits, merge the new bytes into the wider
       * entry instead of invalidating.  This preserves cross-width forwarding
       * for patterns like
       *   STORE32 -16=#0; STORE8 -16=#-1; STORE8 -15=#-1; ... LDRSB -16
       * where bytes 1..3 of the 32-bit entry get refreshed as each narrow
       * store lands.  Only fall back to invalidate when the merge isn't
       * representable (non-immediates, INT64 entries, etc.). */
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
            /* Try to merge the narrow store's bytes into the wider entry. */
            int ce_is_imm = irop_is_immediate(ce->stored_value);
            if (new_src_is_imm && ce_is_imm && new_bytes > 0 && entry_bytes <= 4 &&
                delta + new_bytes <= entry_bytes)
            {
              int32_t old_v = ce->stored_value.u.imm32;
              int32_t new_v = new_src1.u.imm32;
              uint32_t byte_mask = (new_bytes == 4) ? 0xFFFFFFFFu : ((1u << (new_bytes * 8)) - 1);
              uint32_t pos_mask = byte_mask << (delta * 8);
              uint32_t value_in_pos = ((uint32_t)new_v & byte_mask) << (delta * 8);
              int32_t merged = (int32_t)(((uint32_t)old_v & ~pos_mask) | value_in_pos);
              ce->stored_value.u.imm32 = merged;
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
        /* Skip the fresh-entry insert below; the existing entry now holds
         * the merged constant with the wider btype. */
        goto sl_fwd_store_done;
      }

      /* For wide stores, invalidate narrower entries at higher offsets
       * within the store's byte range.  A 32-bit store at X overwrites
       * any 16-bit entry at X+2; a 64-bit store at X overwrites entries
       * at X+2, X+4, X+6, etc. */
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
      new_entry->next = hash_table[h];
      hash_table[h] = new_entry;

      LOG_SL_FWD("STORE@i=%d TRACK: sym=%p off=%lld btype=%d addrtaken=%d via_ptr=%d", i, (const void *)addr_sym,
                 (long long)addr_offset, (int)dest.btype, addr_addrtaken, addr_via_pointer);

      /* Resolve stored value through forwarded-temp tracking:
       * If src1 is a TEMP that was assigned a value by earlier forwarding
       * (e.g. T2 <-- #7), use that value directly. This enables transitive
       * forwarding: STORE loc1 <-- #7; LOAD T2 <-- loc1 (forwarded to #7);
       * STORE loc2 <-- T2 → stored_value becomes #7 instead of T2. */
      {
        IROperand sv = new_entry->stored_value;
        int32_t sv_vr = irop_get_vreg(sv);
        if (sv_vr >= 0 && TCCIR_DECODE_VREG_TYPE(sv_vr) == TCCIR_VREG_TYPE_TEMP && !sv.is_lval)
        {
          int sv_pos = TCCIR_DECODE_VREG_POSITION(sv_vr);
          if (sv_pos <= max_tmp && fwd_tmp_valid[sv_pos])
          {
            new_entry->stored_value = fwd_tmp_val[sv_pos];
          }
        }
      }

      /* LEA-through / local-lval forwarding: if the stored value reads from
       * a memory location with a tracked constant, forward the constant.
       * Path 1: T***DEREF*** where T is in the LEA map → resolve to StackLoc.
       * Path 2: Direct StackLoc lval (vr<0, so irop_op_is_lval returns false,
       *         but is_lval bit is set — only safe when is_lval=1, not Addr[]). */
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
                if (rv_pos <= max_tmp && fwd_tmp_valid[rv_pos])
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

    /* Dynamic LEA map update: propagate LEA map through ADD instructions
     * encountered during forwarding.  The pre-scan LEA map handles ADDs
     * from the original IR, but forwarding may create new ASSIGN chains
     * (T29 = T28 where T28 is in the LEA map) followed by ADD (T32 = T29 + 4).
     * Without this, pointer-offset stores (fill_big field[1..3]) can't resolve. */
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
          if (s1v >= 0 && TCCIR_DECODE_VREG_TYPE(s1v) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(as2) && !as2.is_sym)
          {
            int s1p = TCCIR_DECODE_VREG_POSITION(s1v);
            if (s1p <= max_tmp && lea_map[s1p].valid)
            {
              lea_map[adp].offset = lea_map[s1p].offset + irop_get_imm64_ex(ir, as2);
              lea_map[adp].sym = lea_map[s1p].sym;
              lea_map[adp].valid = 1;
            }
          }
          else if (s2v >= 0 && TCCIR_DECODE_VREG_TYPE(s2v) == TCCIR_VREG_TYPE_TEMP && irop_is_immediate(as1) &&
                   !as1.is_sym)
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

    /* Dynamic LEA map propagation for ASSIGN instructions.
     * Handles: T29 <-- V21 [ASSIGN] where V21 was stored from a LEA-mapped TEMP.
     * The ASSIGN may already exist from a prior pass (not created by SL forwarding),
     * so we must also resolve VARs through the hash table to find their stored value. */
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

    /* If this instruction modifies a vreg that's used as a stored value,
     * invalidate those store entries */
    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_LOAD)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      int j;

      for (j = 0; j < entry_count; j++)
      {
        if (entries[j].valid)
        {
          /* If the stored value vreg is redefined, invalidate */
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

      /* Track this write for the LOAD address vreg safety check.
       * When a vreg is written by ANY instruction (AND, ADD, ASSIGN, etc.),
       * a later LOAD using that vreg as its address should NOT be forwarded
       * from a store that happened BEFORE this write. */
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

  /* Post-pass: eliminate stores whose only load was forwarded.
   * For each forwarded store, scan all remaining (non-NOP) instructions to
   * check if any src operand still references the same local offset.
   * Only anonymous stores (vreg < 0) are candidates — already filtered above. */
  for (int fi = 0; fi < fwd_store_count; fi++)
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

      /* Helper macro: check if operand is a local address-of that could alias
       * our store's offset (i.e. the store is within a struct whose base
       * address is passed somewhere). */
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

      /* Helper macro: check if operand reads a multi-byte range that covers
       * our store's offset.  A read at offset X with width W covers [X, X+W). */
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

      /* Check src1 */
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
      /* Check src2 */
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
  for (i = 0; i < n; i++)
    tcc_free(saved_entries[i]);
  tcc_free(saved_entries);
  tcc_free(saved_entry_count);
  tcc_free(pred_count);

  LOG_IR_GEN("=== STORE-LOAD FORWARDING END: %d changes ===", changes);

  return changes;
}

/* Return the byte width of an IROP_BTYPE_* value. */
static int irop_btype_byte_width(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
    return 4;
  case IROP_BTYPE_INT64:
    return 8;
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 4; /* struct, func, etc. — conservative */
  }
}

/* Resolve a single-def TEMP vreg to its base address (sym, byte_offset).
 *
 * The TEMP must have exactly one def anywhere in the function.  Recursion
 * through an ASSIGN/LEA/ADD chain whose src1 is itself a single-def TEMP is
 * supported up to a bounded depth, so chains like
 *   T1 = &local
 *   T2 = T1 + imm
 *   *T2 = v
 * resolve to (NULL, stack_off + imm). */
/* Single-def map for TEMP vregs, built once per tcc_ir_opt_store_redundant
 * invocation so rse_resolve_temp_addr_impl is an O(1) lookup instead of a full
 * instruction scan per call (which made the pass O(n^2) on functions with many
 * TEMP-base address computations).  Entry holds the defining instruction index,
 * -1 for no def, or RSE_DEF_MULTI for more than one def.  Stale entries are
 * harmless: the op-kind check below rejects anything that isn't a live
 * ADD/LEA/ASSIGN, so a def NOP'd mid-pass resolves conservatively to "no". */
#define RSE_DEF_MULTI (-2)
static int *rse_def_map;
static int rse_def_map_size;

static void rse_build_def_map(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int max_pos = -1;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *dq = &ir->compact_instructions[j];
    if (dq->op == TCCIR_OP_NOP || !irop_config[dq->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, dq);
    int32_t dvr = irop_get_vreg(d);
    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      int p = TCCIR_DECODE_VREG_POSITION(dvr);
      if (p > max_pos)
        max_pos = p;
    }
  }
  rse_def_map_size = max_pos + 1;
  rse_def_map = NULL;
  if (rse_def_map_size <= 0)
    return;
  rse_def_map = (int *)tcc_malloc(sizeof(int) * rse_def_map_size);
  for (int i = 0; i < rse_def_map_size; i++)
    rse_def_map[i] = -1;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *dq = &ir->compact_instructions[j];
    if (dq->op == TCCIR_OP_NOP)
      continue;
    if (dq->op == TCCIR_OP_STORE_INDEXED || dq->op == TCCIR_OP_STORE_POSTINC)
      continue;
    if (!irop_config[dq->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, dq);
    if (d.is_lval)
      continue;
    int32_t dvr = irop_get_vreg(d);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(dvr);
    rse_def_map[p] = (rse_def_map[p] == -1) ? j : RSE_DEF_MULTI;
  }
}

static void rse_free_def_map(void)
{
  tcc_free(rse_def_map);
  rse_def_map = NULL;
  rse_def_map_size = 0;
}

static int rse_resolve_temp_addr_impl(TCCIRState *ir, int32_t vr,
                                      const Sym **out_sym, int64_t *out_off,
                                      int depth)
{
  if (depth <= 0)
    return 0;
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (!rse_def_map || pos >= rse_def_map_size)
    return 0;
  int def_idx = rse_def_map[pos];
  if (def_idx < 0) /* -1 (none) or RSE_DEF_MULTI */
    return 0;

  IRQuadCompact *dq = &ir->compact_instructions[def_idx];
  if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_LEA && dq->op != TCCIR_OP_ASSIGN)
    return 0;
  IROperand s1 = tcc_ir_op_get_src1(ir, dq);

  int64_t base_off = 0;
  const Sym *base_sym = NULL;
  int resolved = 0;

  /* Case A: src1 is a SYMREF (&global + addend). */
  if (s1.is_sym && !s1.is_lval)
  {
    IRPoolSymref *sr = irop_get_symref_ex(ir, s1);
    if (!sr || !sr->sym)
      return 0;
    base_sym = sr->sym;
    base_off = (int64_t)sr->addend;
    resolved = 1;
  }
  /* Case B: src1 is a stack-local STACKOFF (Addr[StackLoc[off]]). */
  else if (s1.is_local && !s1.is_lval && !s1.is_llocal && irop_get_tag(s1) == IROP_TAG_STACKOFF)
  {
    base_sym = NULL;
    base_off = irop_get_stack_offset(s1);
    resolved = 1;
  }
  /* Case C: src1 is itself a TEMP — recurse. */
  else if (!s1.is_lval && irop_get_tag(s1) == IROP_TAG_VREG)
  {
    int32_t inner_vr = irop_get_vreg(s1);
    if (!rse_resolve_temp_addr_impl(ir, inner_vr, &base_sym, &base_off, depth - 1))
      return 0;
    resolved = 1;
  }

  if (!resolved)
    return 0;

  if (dq->op == TCCIR_OP_ADD)
  {
    IROperand s2 = tcc_ir_op_get_src2(ir, dq);
    if (!irop_is_immediate(s2))
      return 0;
    base_off += irop_get_imm64_ex(ir, s2);
  }

  *out_sym = base_sym;
  *out_off = base_off;
  return 1;
}

static int rse_resolve_temp_addr(TCCIRState *ir, int32_t vr,
                                 const Sym **out_sym, int64_t *out_off)
{
  return rse_resolve_temp_addr_impl(ir, vr, out_sym, out_off, 4);
}

/* A resolved (sym, off) access of `width` bytes is "key-safe" only when it
 * stays within `sym`'s own storage.  Global base-sharing emits stores like
 * `T = &g0; STORE_INDEXED T, #off` where off reaches a *different* global g1
 * (e.g. &hpart + 8 == &lpart).  Such a store gets keyed (g0, off) while a
 * direct access of g1 is keyed (g1, 0) — the same address under two keys.
 * Redundant/dead-store elimination compares these keys to find intervening
 * reads, so a cross-symbol key silently misses the read and wrongly drops a
 * live store.  Reject the resolution when the access escapes the symbol so the
 * caller treats it conservatively (untracked, never eliminated). */
static int rse_addr_escapes_sym(const Sym *sym, int64_t off, int width)
{
  if (!sym)
    return 0;
  int align;
  int size = type_size(&sym->type, &align);
  if (size <= 0)
    return 0; /* incomplete / unknown size — can't prove an escape */
  if (width <= 0)
    width = 1;
  return off < 0 || off + (int64_t)width > (int64_t)size;
}

/* Resolve a store's destination to a (sym, byte_offset) pair.
 * Handles direct SYMREF dest, TEMP-DEREF plain STORE, and STORE_INDEXED
 * with TEMP base.  Returns 1 on success. */
static int rse_resolve_store_addr(TCCIRState *ir, IRQuadCompact *q,
                                  const Sym **out_sym, int64_t *out_off)
{
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int op = q->op;
  if (op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED)
    return 0;

  int64_t extra = 0;
  if (op == TCCIR_OP_STORE_INDEXED)
  {
    IROperand idx = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(idx))
      return 0;
    int64_t scale = 0;
    IROperand sc = tcc_ir_op_get_scale(ir, q);
    if (irop_is_immediate(sc))
      scale = irop_get_imm64_ex(ir, sc);
    extra = irop_get_imm64_ex(ir, idx) << scale;
  }

  int store_width = (op == TCCIR_OP_STORE_INDEXED)
                        ? irop_btype_byte_width(tcc_ir_op_get_src1(ir, q).btype)
                        : irop_btype_byte_width(dest.btype);

  /* Direct SYMREF dest. For plain STORE the dest must be an lval; for
   * STORE_INDEXED the base may have been stripped of is_lval by disp_fusion. */
  if (dest.is_sym)
  {
    if (op == TCCIR_OP_STORE && !dest.is_lval)
      return 0;
    IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
    if (!sr || !sr->sym)
      return 0;
    int64_t off = (int64_t)sr->addend + extra;
    if (rse_addr_escapes_sym(sr->sym, off, store_width))
      return 0;
    *out_sym = sr->sym;
    *out_off = off;
    return 1;
  }

  /* TEMP base form: dest is the TEMP holding the address. */
  if (op == TCCIR_OP_STORE && !dest.is_lval)
    return 0;
  if (op == TCCIR_OP_STORE_INDEXED && dest.is_lval)
    return 0;
  int32_t vr = irop_get_vreg(dest);
  if (!rse_resolve_temp_addr(ir, vr, out_sym, out_off))
    return 0;
  *out_off += extra;
  if (rse_addr_escapes_sym(*out_sym, *out_off, store_width))
    return 0;
  return 1;
}

/* Redundant Store Elimination
 * Phase 4: Remove stores to memory locations that are overwritten before being read
 * (dead stores to memory)
 * CONSERVATIVE: Only handles stack locals whose address is not taken
 */
int tcc_ir_opt_store_redundant(TCCIRState *ir)
{
  /* Single forward pass: O(n) time, no heap allocation.
   *
   * Tracks at most RSE_MAX_ACTIVE pending stores since the last basic-block
   * boundary using a small on-stack table.  When a STORE to address A is seen
   * and A is already in the table, the previous store is overwritten without a
   * read → mark it NOP.  When a READ of A is seen, evict it from the table so
   * the producing store is not killed.  Block boundaries flush the table.
   *
   * If the table fills up (> RSE_MAX_ACTIVE distinct live stores in one block)
   * the excess stores are simply not tracked — conservative, never wrong. */
#define RSE_MAX_ACTIVE 64
  typedef struct
  {
    int64_t offset;
    const Sym *sym;
    int store_idx;
    int btype;     /* VT_BYTE / VT_INT / etc. — width of the store */
    int is_global;     /* 1 = global symref entry, 0 = local stack slot */
    int via_temp_base; /* 1 = entry was tracked via a single-def TEMP base
                        * (rse_resolve_store_addr).  For such entries, src
                        * operands carrying the same address-of-local with
                        * is_lval=0 are not reads — only true lval accesses
                        * (is_lval=1) evict.  Unknown-pointer STORE and CALL
                        * still flush them. */
  } RseSlot;

  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== REDUNDANT STORE ELIMINATION START ===");

  /* O(1) TEMP-def lookup for rse_resolve_temp_addr_impl (see comment there). */
  rse_build_def_map(ir);

  RseSlot active[RSE_MAX_ACTIVE];
  int active_count = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Block boundary: pending stores may be live on the other side → flush. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID)
    {
      active_count = 0;
      continue;
    }

    /* Function call: the callee can read globals but not non-escaping locals
     * unless their address is passed as a parameter.  When every PARAM is an
     * immediate constant (no pointer can reach any local), keep local entries
     * alive across the call; otherwise flush everything conservatively. */
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      int safe_for_locals = (active_count > 0);
      if (safe_for_locals)
      {
        IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
        int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2));
        for (int j = i - 1; j >= 0; j--)
        {
          IRQuadCompact *pq = &ir->compact_instructions[j];
          if (pq->op == TCCIR_OP_NOP)
            continue;
          if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
            break;
          IROperand penc = tcc_ir_op_get_src2(ir, pq);
          if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, penc)) != call_id)
            continue;
          if (pq->op == TCCIR_OP_FUNCPARAMVOID)
            continue;
          IROperand pval = tcc_ir_op_get_src1(ir, pq);
          if (!irop_is_immediate(pval))
          {
            safe_for_locals = 0;
            break;
          }
        }
      }
      if (safe_for_locals)
      {
        for (int k = 0; k < active_count;)
        {
          if (active[k].is_global && !active[k].via_temp_base)
            active[k] = active[--active_count];
          else
            k++;
        }
      }
      else
      {
        active_count = 0;
      }
      continue;
    }

    /* READ check: any instruction that uses a local OR global address as
     * src1 or src2 keeps the corresponding pending store alive.
     *
     * For an entry tracked via TEMP-base resolution, an address-of-local
     * src (is_local && !is_lval) is NOT a read — it's just computing the
     * address into a register.  Only an actual lvalue read (is_lval=1)
     * evicts those entries.  Escape risks (the address being PARAMmed or
     * stored to memory) are caught by the CALL flush and the unknown-pointer
     * STORE flush below.
     *
     * Additionally, a TEMP-DEREF src (vreg with is_lval=1) is a read through
     * the TEMP's address — resolve the TEMP to (sym, off) and evict any
     * matching entry.  This is required for soundness of via_temp_base
     * tracking: e.g. `T = &local; *T &= mask; *T |= bits; ...` does a
     * read of *local at each `*T` use. */
#define RSE_EVICT_FOR_SRC(SRC_OP)                                                                                       \
  do                                                                                                                    \
  {                                                                                                                     \
    IROperand _src = (SRC_OP);                                                                                          \
    if (_src.is_local || (_src.is_sym && _src.is_lval))                                                                 \
    {                                                                                                                   \
      int64_t _off;                                                                                                     \
      const Sym *_sym;                                                                                                  \
      if (_src.is_sym)                                                                                                  \
      {                                                                                                                 \
        IRPoolSymref *_sr = irop_get_symref_ex(ir, _src);                                                               \
        _sym = _sr ? _sr->sym : NULL;                                                                                   \
        _off = _sr ? _sr->addend : 0;                                                                                   \
      }                                                                                                                 \
      else                                                                                                              \
      {                                                                                                                 \
        _off = irop_get_imm64_ex(ir, _src);                                                                             \
        _sym = irop_get_sym_ex(ir, _src);                                                                               \
      }                                                                                                                \
      for (int _k = 0; _k < active_count; _k++)                                                                         \
      {                                                                                                                 \
        if (active[_k].sym == _sym && active[_k].offset == _off)                                                        \
        {                                                                                                               \
          if (active[_k].via_temp_base && !_src.is_lval)                                                                \
            break; /* not a read of this TEMP-resolved entry — keep alive */                                            \
          active[_k] = active[--active_count];                                                                          \
          break;                                                                                                        \
        }                                                                                                               \
      }                                                                                                                 \
    }                                                                                                                   \
    else if (_src.is_lval && irop_get_tag(_src) == IROP_TAG_VREG)                                                       \
    {                                                                                                                   \
      /* TEMP-DEREF read: try to resolve the TEMP to a (sym, off). */                                                   \
      const Sym *_sym;                                                                                                  \
      int64_t _off;                                                                                                     \
      if (rse_resolve_temp_addr(ir, irop_get_vreg(_src), &_sym, &_off))                                                 \
      {                                                                                                                 \
        for (int _k = 0; _k < active_count; _k++)                                                                       \
        {                                                                                                               \
          if (active[_k].sym == _sym && active[_k].offset == _off)                                                      \
          {                                                                                                             \
            active[_k] = active[--active_count];                                                                        \
            break;                                                                                                      \
          }                                                                                                             \
        }                                                                                                               \
      }                                                                                                                 \
    }                                                                                                                   \
  } while (0)
    if (irop_config[q->op].has_src1)
      RSE_EVICT_FOR_SRC(tcc_ir_op_get_src1(ir, q));
    if (irop_config[q->op].has_src2)
      RSE_EVICT_FOR_SRC(tcc_ir_op_get_src2(ir, q));
#undef RSE_EVICT_FOR_SRC

    /* STORE / STORE_INDEXED to a local non-addr-taken address, or to a
     * global/anon SYMREF (directly, or via a single-def TEMP base that traces
     * to one). */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int dest_is_global = (dest.is_sym && (dest.is_lval || q->op == TCCIR_OP_STORE_INDEXED));
      int dest_is_local = (q->op == TCCIR_OP_STORE) && dest.is_local;
      if (!dest_is_local && !dest_is_global)
      {
        /* Try resolving a TEMP base through its single def. */
        const Sym *resolved_sym = NULL;
        int64_t resolved_off = 0;
        if (rse_resolve_store_addr(ir, q, &resolved_sym, &resolved_off))
        {
          /* For STORE_INDEXED, access width is from src1 (the stored value),
           * not dest (which is the base address). */
          int store_btype;
          if (q->op == TCCIR_OP_STORE_INDEXED)
            store_btype = tcc_ir_op_get_src1(ir, q).btype;
          else
            store_btype = dest.btype;
          int found = -1;
          for (int k = 0; k < active_count; k++)
          {
            if (active[k].sym == resolved_sym && active[k].offset == resolved_off)
            {
              found = k;
              break;
            }
          }
          if (found >= 0 && irop_btype_byte_width(store_btype) >= irop_btype_byte_width(active[found].btype))
          {
            LOG_IR_GEN("OPTIMIZE: Redundant store at i=%d (overwritten without read, indirect)",
                       active[found].store_idx);
            ir->compact_instructions[active[found].store_idx].op = TCCIR_OP_NOP;
            changes++;
            active[found].store_idx = i;
            active[found].btype = store_btype;
          }
          else if (found >= 0)
          {
            active[found] = active[--active_count];
          }
          else if (active_count < RSE_MAX_ACTIVE)
          {
            active[active_count].sym = resolved_sym;
            active[active_count].offset = resolved_off;
            active[active_count].store_idx = i;
            active[active_count].btype = store_btype;
            active[active_count].is_global = 1;
            active[active_count].via_temp_base = 1;
            active_count++;
          }
          continue;
        }

        /* STORE through unknown pointer — could alias any tracked global.
         * Local entries are safe (non-addrtaken locals can't be aliased). */
        for (int k = 0; k < active_count;)
        {
          if (active[k].is_global)
            active[k] = active[--active_count];
          else
            k++;
        }
        continue;
      }

      /* Skip addr-taken locals: they may be read through a pointer.
       * (Globals don't need this check — their address is always known but
       * any aliased access via a TEMP pointer is handled by the flush
       * above on STORE through unknown pointer.) */
      if (!dest_is_global)
      {
        int32_t addr_vr = irop_get_vreg(dest);
        if (addr_vr >= 0)
        {
          IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
          if (interval && interval->addrtaken)
            continue;
        }
      }

      int64_t off;
      const Sym *sym;
      if (dest_is_global)
      {
        IRPoolSymref *dr = irop_get_symref_ex(ir, dest);
        sym = dr ? dr->sym : NULL;
        off = dr ? dr->addend : 0;
        if (q->op == TCCIR_OP_STORE_INDEXED)
        {
          IROperand idx = tcc_ir_op_get_src2(ir, q);
          if (!irop_is_immediate(idx))
            continue;
          int64_t scale = 0;
          IROperand sc = tcc_ir_op_get_scale(ir, q);
          if (irop_is_immediate(sc))
            scale = irop_get_imm64_ex(ir, sc);
          off += irop_get_imm64_ex(ir, idx) << scale;
        }
      }
      else
      {
        off = irop_get_imm64_ex(ir, dest);
        sym = irop_get_sym_ex(ir, dest);
      }

      /* For STORE_INDEXED, access width is from src1 (the stored value),
       * not dest (which is the base address). */
      int store_btype;
      if (q->op == TCCIR_OP_STORE_INDEXED)
        store_btype = tcc_ir_op_get_src1(ir, q).btype;
      else
        store_btype = dest.btype;

      /* Look for a previous pending store to the same address. */
      int found = -1;
      for (int k = 0; k < active_count; k++)
      {
        if (active[k].sym == sym && active[k].offset == off)
        {
          found = k;
          break;
        }
      }

      if (found >= 0 && irop_btype_byte_width(store_btype) >= irop_btype_byte_width(active[found].btype))
      {
        /* Overwritten without a read AND the new store covers at least
         * as many bytes as the old one → the previous store is dead.
         * A byte-store must NOT kill a wider word-store at the same
         * offset, since the word-store covers additional bytes. */
        LOG_IR_GEN("OPTIMIZE: Redundant store at i=%d (overwritten without read)", active[found].store_idx);
        ir->compact_instructions[active[found].store_idx].op = TCCIR_OP_NOP;
        changes++;
        active[found].store_idx = i;
        active[found].btype = store_btype;
      }
      else if (found >= 0)
      {
        /* Same offset but narrower store — can't kill the wider store.
         * Evict the old entry and stop tracking this offset. */
        active[found] = active[--active_count];
      }
      else if (active_count < RSE_MAX_ACTIVE)
      {
        active[active_count].sym = sym;
        active[active_count].offset = off;
        active[active_count].store_idx = i;
        active[active_count].btype = store_btype;
        active[active_count].is_global = dest_is_global;
        active[active_count].via_temp_base = 0;
        active_count++;
      }
      /* else: table full — skip this store conservatively */
    }
  }

  LOG_IR_GEN("=== REDUNDANT STORE ELIMINATION END: %d changes ===", changes);

  rse_free_def_map();
  return changes;
#undef RSE_MAX_ACTIVE
}

/* Dead Local Slot Elimination
 *
 * Kill stores to stack-local offsets that are never read and whose address
 * never escapes — except as PARAM0 of a recognized write-only intrinsic
 * (memset / __aeabi_memset).  Also kill the memset call itself when its
 * target range is entirely dead.
 *
 * Catches the gcc.c-torture compile/931004-1.c pattern: a large local array
 * initialized to constants that nothing reads or escapes.  GCC reduces such
 * a function to a bare `return 0`; this pass closes most of that gap.
 *
 * CONSERVATIVE: bails on functions with IJUMP, or on any Addr[StackLoc[X]]
 * use outside memset PARAM0.  In those cases offset-level liveness is unsafe
 * without knowing object boundaries (the stack layout is not yet populated
 * during the IR optimization pipeline).
 */

/* Resolve a TEMP vreg that points into a local frame slot to its EXACT frame
 * offset, walking `Addr[StackLoc] (+/- #const)* (ASSIGN/LEA)*` def chains.
 * Returns 1 and sets *out_off iff every step is constant (so the offset is
 * provably exact); 0 otherwise (variable offset, non-TEMP, unresolved base).
 * Lets dead_local_slot_elim treat a precise vreg-deref like a direct
 * StackLoc[off] access — recording an exact live[] read range / eliminating an
 * exact dead store — instead of conservatively poisoning the whole slot.  TEMP-
 * only keeps the def lookup unambiguous (single-assignment). */
static int dls_vreg_frame_off(TCCIRState *ir, int32_t vr, int before_idx, int *out_off)
{
  long acc = 0;
  for (int guard = 0; guard < 32; guard++)
  {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    int d = tcc_ir_find_defining_instruction(ir, vr, before_idx);
    if (d < 0)
      return 0;
    IRQuadCompact *dq = &ir->compact_instructions[d];
    if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB &&
        dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_LEA)
      return 0;
    IROperand s1 = tcc_ir_op_get_src1(ir, dq);
    if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, dq);
      if (!irop_is_immediate(s2) || s2.is_sym)
        return 0;
      long c = (long)irop_get_imm64_ex(ir, s2);
      acc += (dq->op == TCCIR_OP_SUB) ? -c : c;
    }
    /* s1 is the base: a direct Addr[StackLoc] terminates the walk, else recurse. */
    if (irop_get_tag(s1) == IROP_TAG_STACKOFF && s1.is_local && !s1.is_lval &&
        irop_get_vreg(s1) == -1)
    {
      long off = (long)irop_get_stack_offset(s1) + acc;
      if (off < INT_MIN || off > INT_MAX)
        return 0;
      *out_off = (int)off;
      return 1;
    }
    if (s1.is_lval || irop_get_vreg(s1) < 0)
      return 0;
    vr = irop_get_vreg(s1);
    before_idx = d;
  }
  return 0;
}

int tcc_ir_opt_dead_local_slot_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Nested function: `StackLoc[X]` here is the parent's frame, accessed via
   * the static chain register.  We have no way to see the parent's reads, so
   * any store could be live there. */
  if (ir->captured_count > 0 || ir->has_static_chain)
    return 0;

  /* dls_has_indexed: the precise vreg-deref relaxation below (resolve a deref to
   * an exact frame offset instead of poisoning its slot) is only sound when ALL
   * slot reads are recorded in live[].  Direct StackLoc and plain vreg-derefs
   * are; indexed/postinc loads are NOT (their base is a bare vreg this pass
   * doesn't classify).  So if the function contains any indexed/postinc memory
   * op, disable the relaxation entirely and fall back to the conservative
   * poison behaviour. */
  int dls_has_indexed = 0;
  /* dls_has_backedge: the precise vreg-deref relaxation uses position-based
   * liveness (`read.pos > store.pos`).  That is only sound in straight-line /
   * forward-only control flow, where instruction order is a valid topological
   * order so a read at an earlier position can never execute AFTER a later
   * store.  A loop back-edge breaks that: a store in the loop body whose value
   * is read at the loop top (earlier position) is loop-carried-live, and
   * eliminating it miscompiles (20040307-1: dropped the `bit0--` write-back).
   * So disable the relaxation whenever any backward branch exists. */
  int dls_has_backedge = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
    if (op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED ||
        op == TCCIR_OP_LOAD_POSTINC || op == TCCIR_OP_STORE_POSTINC)
      dls_has_indexed = 1;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF)
    {
      int tg = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      if (tg >= 0 && tg <= i)
        dls_has_backedge = 1;
    }
    if (op == TCCIR_OP_SWITCH_TABLE)
      dls_has_backedge = 1; /* targets unknown here — conservatively a back-edge */
  }
  /* Single flag gating the precise vreg-deref relaxation (read-recording,
   * no-poison, and the known-offset STORE elim below). */
  int dls_precise_ok = !dls_has_indexed && !dls_has_backedge;

  int max_call_id = ir->next_call_id;
  /* Per-call bitmaps: write-through-PARAM0 helpers (memset/memcpy/memmove).
   * For memset and memmove4/8 the size param index is 1; for the standard
   * memcpy/memmove ABI the size param index is 2.  We track both so the
   * elimination phase below knows where to find the size. */
  uint8_t *is_writecall = NULL;       /* any of: memset/memcpy/memmove family */
  uint8_t *writecall_size_at_2 = NULL; /* 1 = size is param 2; 0 = size is param 1 */
  /* memcpy/memmove also READ through PARAM1: track them so the per-store
   * elimination below can convert that read into a bounded live[] range
   * instead of an unbounded non-tame escape that would gate every other
   * dead store. */
  uint8_t *is_memcpy_like = NULL;
  int writecall_count = 0;
  if (max_call_id > 0)
  {
    is_writecall = tcc_mallocz((max_call_id + 7) / 8);
    writecall_size_at_2 = tcc_mallocz((max_call_id + 7) / 8);
    is_memcpy_like = tcc_mallocz((max_call_id + 7) / 8);
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
      continue;
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;
    int sz_at_2 = -1;
    int memcpy_like = 0;
    if (strcmp(name, "__aeabi_memset") == 0 || strcmp(name, "memset") == 0)
      sz_at_2 = 0;
    else if (strcmp(name, "__aeabi_memmove4") == 0 || strcmp(name, "__aeabi_memmove8") == 0 ||
             strcmp(name, "__aeabi_memmove") == 0 || strcmp(name, "__aeabi_memcpy4") == 0 ||
             strcmp(name, "__aeabi_memcpy8") == 0 || strcmp(name, "__aeabi_memcpy") == 0 ||
             strcmp(name, "memmove") == 0 || strcmp(name, "memcpy") == 0)
    {
      sz_at_2 = 1;
      memcpy_like = 1;
    }
    if (sz_at_2 < 0)
      continue;
    int cid = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
    if (cid < 0 || cid >= max_call_id || !is_writecall)
      continue;
    is_writecall[cid / 8] |= (1 << (cid % 8));
    if (sz_at_2)
      writecall_size_at_2[cid / 8] |= (1 << (cid % 8));
    if (memcpy_like)
      is_memcpy_like[cid / 8] |= (1 << (cid % 8));
    writecall_count++;
  }

  /* ==========================================================================
   * Tameness analysis for address-of-local escapes.
   *
   * Goal: prove that some local slots' addresses, although taken, never reach
   * a context that reads the slot.  Such "tame" slots remain safely
   * eliminable.  Without this, the bail at the address-of escape below would
   * give up on the whole function.
   *
   * vreg_slot[V] tracks which slot V points to:
   *   -1 = unknown / no slot
   *   -2 = AMBIGUOUS (multiple slots)
   *   >=0 encoded as (offset - INT_MIN) so we can store negative offsets.
   *
   * Tame use of a derived-address vreg V (V → slot S, !ambiguous):
   *   - ADD/SUB with constant: derived; propagate
   *   - ASSIGN: derived; propagate
   *   - CMP src: harmless
   *   - STORE dest (write through V): harmless
   *   - PARAM0 of memset/memcpy/memmove: harmless
   * Anything else → non-tame for slot S (and ambiguity poisons every slot).
   * ========================================================================== */
  int max_vreg = ir->next_temporary_variable + ir->next_local_variable + ir->next_parameter + 16;
  int *vreg_slot = tcc_malloc((size_t)max_vreg * sizeof(int));
  for (int v = 0; v < max_vreg; v++)
    vreg_slot[v] = -1;
  /* vreg_external[V]=1 means V provably came from external memory (a PARAM
   * directly or pointer arithmetic on one) and therefore can't point into
   * this function's local frame. Used to relax the has_unknown_deref bail
   * for TEMP lval-loads whose base is a param-derived pointer. */
  unsigned char *vreg_external = tcc_mallocz((size_t)max_vreg);
#define VR_SLOT_AMBIG INT_MIN
#define VR_FLAT(_vr)                                                                                                   \
  ({                                                                                                                   \
    int _t = TCCIR_DECODE_VREG_TYPE(_vr);                                                                              \
    int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                          \
    int _b = -1;                                                                                                       \
    if (_t == TCCIR_VREG_TYPE_TEMP)                                                                                    \
      _b = _p;                                                                                                         \
    else if (_t == TCCIR_VREG_TYPE_VAR)                                                                                \
      _b = ir->next_temporary_variable + _p;                                                                           \
    else if (_t == TCCIR_VREG_TYPE_PARAM)                                                                              \
      _b = ir->next_temporary_variable + ir->next_local_variable + _p;                                                 \
    (_b >= 0 && _b < max_vreg) ? _b : -1;                                                                              \
  })

  /* Per-slot tame state, stored as parallel arrays.  Slots are added on
   * first observation (first addr-of-local escape).  `tame_slot_end[]`
   * carries an upper bound on the slot's extent (frame offset of the next
   * observed allocation above it); computed once after tameness analysis
   * and used to bound a non-tame deref's reachable bytes per-slot, so a
   * non-tame deref through one slot doesn't poison eliminations on other
   * non-overlapping slots. */
  int slot_cap = 32;
  int *tame_slot_off = tcc_malloc((size_t)slot_cap * sizeof(int));
  uint8_t *tame_slot_ok = tcc_malloc((size_t)slot_cap * sizeof(uint8_t));
  int *tame_slot_end = tcc_malloc((size_t)slot_cap * sizeof(int));
  int tame_slot_n = 0;
  /* Set when an unknown-slot TMP is dereferenced: such a deref could touch
   * any byte of the frame, even offsets whose address was never taken
   * explicitly.  Disables the "off not in tame_slot → eligible" shortcut. */
  int has_unknown_deref = 0;

#define TAME_FIND_OR_ADD(_off)                                                                                         \
  ({                                                                                                                   \
    int _x = -1;                                                                                                       \
    for (int _i = 0; _i < tame_slot_n; _i++)                                                                           \
      if (tame_slot_off[_i] == (_off))                                                                                 \
      {                                                                                                                \
        _x = _i;                                                                                                       \
        break;                                                                                                         \
      }                                                                                                                \
    if (_x < 0)                                                                                                        \
    {                                                                                                                  \
      if (tame_slot_n >= slot_cap)                                                                                     \
      {                                                                                                                \
        slot_cap *= 2;                                                                                                 \
        tame_slot_off = tcc_realloc(tame_slot_off, (size_t)slot_cap * sizeof(int));                                    \
        tame_slot_ok = tcc_realloc(tame_slot_ok, (size_t)slot_cap * sizeof(uint8_t));                                  \
        tame_slot_end = tcc_realloc(tame_slot_end, (size_t)slot_cap * sizeof(int));                                    \
      }                                                                                                                \
      _x = tame_slot_n++;                                                                                              \
      tame_slot_off[_x] = (_off);                                                                                      \
      tame_slot_ok[_x] = 1;                                                                                            \
      tame_slot_end[_x] = 0;                                                                                           \
    }                                                                                                                  \
    _x;                                                                                                                \
  })
#define TAME_FIND(_off)                                                                                                \
  ({                                                                                                                   \
    int _x = -1;                                                                                                       \
    for (int _i = 0; _i < tame_slot_n; _i++)                                                                           \
      if (tame_slot_off[_i] == (_off))                                                                                 \
      {                                                                                                                \
        _x = _i;                                                                                                       \
        break;                                                                                                         \
      }                                                                                                                \
    _x;                                                                                                                \
  })

  /* Step 1: seed vreg_slot[] from direct addr-of-local sources.
   * Patterns:
   *   V <-- Addr[StackLoc[X]]                       (ASSIGN or LEA)
   *   V <-- Addr[StackLoc[X]] ADD/SUB <anything>    (offset may be variable)
   */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA &&
        q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int dv = VR_FLAT(irop_get_vreg(dest));
    if (dv < 0)
      continue;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(s1) != IROP_TAG_STACKOFF || !s1.is_local || s1.is_lval || irop_get_vreg(s1) != -1)
      continue;
    int slot = irop_get_stack_offset(s1);
    TAME_FIND_OR_ADD(slot); /* register slot — default tame=1 */
    if (vreg_slot[dv] == -1)
      vreg_slot[dv] = slot;
    else if (vreg_slot[dv] != slot)
      vreg_slot[dv] = VR_SLOT_AMBIG;
  }

  /* Step 2: propagate slot membership through ASSIGN and ADD/SUB regardless
   * of whether the offset operand is a constant.  The result vreg still
   * points into the same slot — we just don't know the exact offset, which
   * is fine for tameness (we never use the offset value). */
  int changed = 1;
  while (changed)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA &&
          q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
        continue;
      if (!irop_config[q->op].has_dest)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int dv = VR_FLAT(irop_get_vreg(dest));
      if (dv < 0)
        continue;
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int sv1 = VR_FLAT(irop_get_vreg(s1));
      int s1_slot = (sv1 >= 0) ? vreg_slot[sv1] : -1;
      int next = s1_slot;
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        int sv2 = VR_FLAT(irop_get_vreg(s2));
        int s2_slot = (sv2 >= 0) ? vreg_slot[sv2] : -1;
        /* &slot + ptr-to-slot' is ambiguous (which slot does the result
         * point into?). */
        if (s2_slot != -1)
        {
          if (s1_slot == -1)
            next = s2_slot;
          else if (s1_slot != s2_slot)
            next = VR_SLOT_AMBIG;
        }
      }
      if (next == -1)
        continue;
      int prev = vreg_slot[dv];
      if (prev == -1)
      {
        vreg_slot[dv] = next;
        changed = 1;
      }
      else if (prev == VR_SLOT_AMBIG)
      {
        /* already poisoned */
      }
      else if (prev != next)
      {
        vreg_slot[dv] = VR_SLOT_AMBIG;
        changed = 1;
      }
    }
  }

  /* Step 2b: seed and propagate vreg_external.
   *   - Seed: dest <-- PARAM (ASSIGN), dest <-- imm (ADD/SUB const, etc.).
   *   - Propagate: dest <-- ext-src1 [ASSIGN/LEA], dest <-- ext OP {imm|ext}.
   * A vreg that has both a stack-slot and external provenance gets cleared
   * (ambiguous); we keep only the conservative case. */
  {
    int changed_ext = 1;
    while (changed_ext)
    {
      changed_ext = 0;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
          continue;
        if (!irop_config[q->op].has_dest)
          continue;
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int dv = VR_FLAT(irop_get_vreg(dest));
        if (dv < 0 || vreg_external[dv])
          continue;
        if (vreg_slot[dv] != -1)
          continue; /* known to carry a stack slot — not "external" */
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : s1;
        int sv1 = VR_FLAT(irop_get_vreg(s1));
        int sv2 = irop_config[q->op].has_src2 ? VR_FLAT(irop_get_vreg(s2)) : -1;
        int s1_param = (irop_get_vreg(s1) != -1 && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s1)) == TCCIR_VREG_TYPE_PARAM &&
                        !s1.is_lval);
        int s1_ext = s1_param || (sv1 >= 0 && vreg_external[sv1]);
        int s1_const = (irop_get_tag(s1) == IROP_TAG_IMM32 || irop_get_tag(s1) == IROP_TAG_I64);
        int s2_param = (irop_config[q->op].has_src2 && irop_get_vreg(s2) != -1 &&
                        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s2)) == TCCIR_VREG_TYPE_PARAM && !s2.is_lval);
        int s2_ext = s2_param || (sv2 >= 0 && vreg_external[sv2]);
        int s2_const = irop_config[q->op].has_src2 &&
                       (irop_get_tag(s2) == IROP_TAG_IMM32 || irop_get_tag(s2) == IROP_TAG_I64);
        int ok;
        if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA)
          ok = (s1_ext || s1_const);
        else /* ADD/SUB: both must be ext/const (and at least one non-const) */
          ok = (s1_ext && (s2_ext || s2_const)) || (s2_ext && (s1_ext || s1_const));
        if (ok && !vreg_external[dv])
        {
          vreg_external[dv] = 1;
          changed_ext = 1;
        }
      }
    }
  }

  /* Pre-scan: collect stack lval read ranges.  When an address-of-local is
   * stored to a StackLoc that is itself never read (no lval access, address
   * not taken), the escape is dead and the source slot should stay tame.
   * Without this, such an indirect escape poisons the whole function via
   * any_nontame, preventing dead-store elimination of independent slots. */
  typedef struct
  {
    int off;
    int width;
  } StackLvalRead;
  int slr_cap = 32, slr_count = 0;
  StackLvalRead *slr = tcc_malloc(sizeof(StackLvalRead) * slr_cap);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int is_store_q = (q->op == TCCIR_OP_STORE);
    for (int k = 0; k < 3; k++)
    {
      if (k == 0 && is_store_q)
        continue;
      IROperand op;
      int has_op;
      if (k == 0)
      {
        has_op = irop_config[q->op].has_dest;
        if (has_op)
          op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        has_op = irop_config[q->op].has_src1;
        if (has_op)
          op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        has_op = irop_config[q->op].has_src2;
        if (has_op)
          op = tcc_ir_op_get_src2(ir, q);
      }
      if (!has_op)
        continue;
      if (irop_get_tag(op) != IROP_TAG_STACKOFF || !op.is_local || !op.is_lval || irop_get_vreg(op) != -1)
        continue;
      if (q->op == TCCIR_OP_BLOCK_COPY && k == 0)
        continue;
      int soff = irop_get_stack_offset(op);
      int sw = ir_opt_store_btype_size_bytes(irop_get_btype(op));
      if (sw <= 0)
        sw = irop_is_64bit(op) ? 8 : 4;
      if (op.is_complex)
        sw *= 2;
      if (slr_count >= slr_cap)
      {
        slr_cap *= 2;
        slr = tcc_realloc(slr, sizeof(StackLvalRead) * slr_cap);
      }
      slr[slr_count].off = soff;
      slr[slr_count].width = sw;
      slr_count++;
    }
  }

  /* Step 3: classify every use of a derived-address vreg.  Anything that
   * isn't a recognized tame pattern marks its slot non-tame. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Check if this is a write-call PARAM0 (which is harmless). */
    int is_write_p0 = 0;
    /* PARAM1 of memcpy/memmove is a bounded READ. With a constant size we
     * can model it as a live[] read range later instead of an unbounded
     * escape — see the bounded_read live[] pass below.  Tag this PARAM
     * as tame for the slot classification step so it doesn't poison the
     * function-wide any_nontame gate. */
    int is_memcpy_src_bounded = 0;
    if ((q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) && is_writecall)
    {
      uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      int cid = TCCIR_DECODE_CALL_ID(enc);
      int pidx = TCCIR_DECODE_PARAM_IDX(enc);
      if (cid >= 0 && cid < max_call_id && (is_writecall[cid / 8] & (1 << (cid % 8))) && pidx == 0)
        is_write_p0 = 1;
      else if (cid >= 0 && cid < max_call_id && (is_memcpy_like[cid / 8] & (1 << (cid % 8))) && pidx == 1)
      {
        /* memcpy/memmove source pointer.  Need a constant size param to be
         * able to bound the read precisely; otherwise leave non-tame. */
        IROperand sz_op;
        for (int j = i + 1; j < n; j++)
        {
          IRQuadCompact *qj = &ir->compact_instructions[j];
          if (qj->op == TCCIR_OP_NOP)
            continue;
          if (qj->op != TCCIR_OP_FUNCPARAMVAL && qj->op != TCCIR_OP_FUNCPARAMVOID &&
              qj->op != TCCIR_OP_FUNCCALLVOID && qj->op != TCCIR_OP_FUNCCALLVAL)
            continue;
          if (qj->op == TCCIR_OP_FUNCPARAMVAL || qj->op == TCCIR_OP_FUNCPARAMVOID)
          {
            uint32_t encj = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, qj));
            if (TCCIR_DECODE_CALL_ID(encj) == cid && TCCIR_DECODE_PARAM_IDX(encj) == 2)
            {
              sz_op = tcc_ir_op_get_src1(ir, qj);
              if (irop_get_tag(sz_op) == IROP_TAG_IMM32)
                is_memcpy_src_bounded = 1;
              break;
            }
          }
          else if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, qj))) == cid)
            break;
        }
      }
    }

    /* Look at each operand role.  We're interested in:
     *   (a) an operand carrying Addr[StackLoc[X]] (direct addr-of), OR
     *   (b) an operand carrying a vreg whose vreg_slot[] is set.
     *
     * For each such address-of-local use, classify the containing op.
     * Mark the relevant slot(s) non-tame if the use isn't recognized. */
    for (int k = 0; k < 3; k++)
    {
      IROperand op;
      int has;
      if (k == 0)
      {
        has = irop_config[q->op].has_dest;
        if (has)
          op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        has = irop_config[q->op].has_src1;
        if (has)
          op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        has = irop_config[q->op].has_src2;
        if (has)
          op = tcc_ir_op_get_src2(ir, q);
      }
      if (!has)
        continue;

      /* Direct addr-of-local (Addr[StackLoc[X]], !is_lval). */
      int slot = INT_MIN; /* sentinel for "no slot involved" */
      if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_lval && irop_get_vreg(op) == -1)
        slot = irop_get_stack_offset(op);

      /* Vreg use that's a derived address. */
      int vr = irop_get_vreg(op);
      int v_slot = VR_SLOT_AMBIG + 1; /* "unknown" sentinel < AMBIG */
      if (vr != -1)
      {
        int vf = VR_FLAT(vr);
        if (vf >= 0)
          v_slot = vreg_slot[vf];
      }

      /* Vreg with is_lval=true (as a src) is a memory dereference.
       *   - VAR/PARAM: just loads the variable's value, *not* a read
       *     through that value — harmless.
       *   - TMP with known slot S: deref reads slot S → S non-tame.
       *   - TMP with unknown slot: we can't bound where the read lands.
       *     Poison all tame_slot entries AND set has_unknown_deref to
       *     also protect offsets that never had their address taken. */
      int vr_type = (vr != -1) ? TCCIR_DECODE_VREG_TYPE(vr) : 0;
      if (op.is_lval && vr != -1 && k != 0 && vr_type == TCCIR_VREG_TYPE_TEMP)
      {
        int vf2 = VR_FLAT(vr);
        int is_external = (vf2 >= 0 && vreg_external && vreg_external[vf2]);
        if ((v_slot == -1 || v_slot == VR_SLOT_AMBIG) && !is_external)
        {
          has_unknown_deref = 1;
          for (int t = 0; t < tame_slot_n; t++)
            tame_slot_ok[t] = 0;
        }
        else if (v_slot != -1 && v_slot != VR_SLOT_AMBIG && v_slot != VR_SLOT_AMBIG + 1)
        {
          int foff;
          /* A deref through a vreg whose exact frame offset we can resolve is a
           * precise read of those bytes — recorded as an exact live[] range in
           * the live-collection pass below, so we need NOT poison the slot.
           * Gated on !dls_has_indexed so live[] is guaranteed complete. */
          if (!dls_precise_ok || !dls_vreg_frame_off(ir, vr, i, &foff))
          {
            int idx = TAME_FIND_OR_ADD(v_slot);
            if (idx >= 0)
              tame_slot_ok[idx] = 0;
          }
        }
      }

      /* Direct addr-of-local: classify in-place. */
      if (slot != INT_MIN)
      {
        int idx = TAME_FIND_OR_ADD(slot);
        int tame_here = 0;
        switch (q->op)
        {
        case TCCIR_OP_ASSIGN:
        case TCCIR_OP_LEA:
          /* dest <-- &slot is the seed itself; harmless. */
          tame_here = (k == 1);
          break;
        case TCCIR_OP_ADD:
        case TCCIR_OP_SUB:
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          if (k == 1 && (irop_get_tag(s2) == IROP_TAG_IMM32 || irop_get_tag(s2) == IROP_TAG_I64))
            tame_here = 1;
          else if (k == 1)
          {
            IROperand adest = tcc_ir_op_get_dest(ir, q);
            int adv = VR_FLAT(irop_get_vreg(adest));
            if (adv >= 0 && vreg_slot[adv] == slot)
              tame_here = 1;
          }
          break;
        }
        case TCCIR_OP_CMP:
          tame_here = (k == 1 || k == 2);
          break;
        case TCCIR_OP_FUNCPARAMVAL:
        case TCCIR_OP_FUNCPARAMVOID:
          if ((is_write_p0 || is_memcpy_src_bounded) && k == 1)
            tame_here = 1;
          break;
        default:
          break;
        }
        if (!tame_here)
          tame_slot_ok[idx] = 0;
      }

      /* Vreg use of a derived-address vreg.
       *
       * Two flavors of "use" carry the slot address out of the IR opcode:
       *
       *   (a) Plain vreg-as-value (is_lval=0): the operand IS the address.
       *       Any non-tame use of the address makes the slot non-tame.
       *
       *   (b) VAR/PARAM lval load (is_lval=1, vr=VAR/PARAM): the IR loads
       *       the variable's value.  For our purposes "the value" IS the
       *       slot address (we got here only because vreg_slot[V] mapped V
       *       to a slot from Step 1/2 — that mapping reflects what the var
       *       holds).  So a PARAM use of the loaded address must also be
       *       classified, otherwise calls like `strlen(s)` where `s` is a
       *       local pointer variable would let us silently treat the
       *       pointed-to slot as write-only.
       *
       *   TMP lval (is_lval=1, vr=TMP) is handled separately below as a
       *   deref of the address — different shape, different handling.
       *
       * Skip the dest role (k=0) for ADD/SUB/ASSIGN: dest there is the
       * propagation target, already handled in step 2 — it isn't a "use"
       * of an existing addr-vreg, so it shouldn't influence tameness. */
      int classify_use = (vr != -1 && v_slot != -1 && v_slot != VR_SLOT_AMBIG + 1);
      if (classify_use && op.is_lval) {
        int vt = TCCIR_DECODE_VREG_TYPE(vr);
        if (vt != TCCIR_VREG_TYPE_VAR && vt != TCCIR_VREG_TYPE_PARAM)
          classify_use = 0;
      }
      if (classify_use)
      {
        int idx = (v_slot == VR_SLOT_AMBIG) ? -1 : TAME_FIND_OR_ADD(v_slot);
        int tame_here = 0;
        switch (q->op)
        {
        case TCCIR_OP_ASSIGN:
        case TCCIR_OP_LEA:
          /* V_new <-- V_old or V_new <-- &slot: propagation step.
           * dest (k=0) is the result; src1 (k=1) is the actual use,
           * already known harmless. */
          tame_here = 1;
          break;
        case TCCIR_OP_ADD:
        case TCCIR_OP_SUB:
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          if (k == 0)
            tame_here = 1; /* dest: propagation target, not a use */
          else if (k == 1 && (irop_get_tag(s2) == IROP_TAG_IMM32 || irop_get_tag(s2) == IROP_TAG_I64))
            tame_here = 1;
          else if (k == 1)
          {
            IROperand adest = tcc_ir_op_get_dest(ir, q);
            int adv = VR_FLAT(irop_get_vreg(adest));
            if (adv >= 0 && vreg_slot[adv] == v_slot)
              tame_here = 1;
          }
          /* k == 2 (V on the RHS as src2): only OK in narrow cases — bail. */
          break;
        }
        case TCCIR_OP_CMP:
          tame_here = 1;
          break;
        case TCCIR_OP_STORE:
        case TCCIR_OP_STORE_INDEXED:
        case TCCIR_OP_STORE_POSTINC:
          /* Vreg-as-dest is the address (write through V). */
          if (k == 0)
            tame_here = 1;
          else if (k == 1 && q->op == TCCIR_OP_STORE)
          {
            /* Address stored into a direct local slot.  If that slot is
             * never read back (no lval access, its address not taken),
             * the stored value is dead and cannot escape. */
            IROperand sdest = tcc_ir_op_get_dest(ir, q);
            if (irop_get_tag(sdest) == IROP_TAG_STACKOFF && sdest.is_local &&
                irop_get_vreg(sdest) == -1)
            {
              int sdoff = irop_get_stack_offset(sdest);
              int sdw = ir_opt_store_btype_size_bytes(irop_get_btype(sdest));
              if (sdw <= 0)
                sdw = 4;
              int target_read = 0;
              for (int sr = 0; sr < slr_count; sr++)
                if (sdoff < slr[sr].off + slr[sr].width &&
                    sdoff + sdw > slr[sr].off)
                {
                  target_read = 1;
                  break;
                }
              if (!target_read && TAME_FIND(sdoff) < 0)
                tame_here = 1;
            }
          }
          /* Vreg-as-src1 stored to a live slot: the address is
           * escaping into memory — non-tame (falls through). */
          break;
        case TCCIR_OP_FUNCPARAMVAL:
        case TCCIR_OP_FUNCPARAMVOID:
          if ((is_write_p0 || is_memcpy_src_bounded) && k == 1)
            tame_here = 1;
          /* Any other PARAM — callee may dereference, non-tame. */
          break;
        default:
          break;
        }
        if (!tame_here)
        {
          if (v_slot == VR_SLOT_AMBIG)
          {
            for (int t = 0; t < tame_slot_n; t++)
              tame_slot_ok[t] = 0;
          }
          else if (idx >= 0)
            tame_slot_ok[idx] = 0;
        }
      }
    }
  }

  /* Collect read/escape ranges as (offset, width, position).  Linear
   * array — typical functions have at most a few hundred distinct ranges,
   * so the O(n*r) intersection checks in the kill loop are fine.  Tracking
   * position lets the elimination loops do position-aware liveness: a
   * STORE at i is dead if no read AFTER i touches the same bytes. */
  typedef struct
  {
    int off;
    int width;
    int pos;
  } LiveRange;
  int cap = 64;
  LiveRange *live = tcc_malloc(sizeof(LiveRange) * cap);
  int live_count = 0;

#define DLS_LIVE_ADD(off_, width_, pos_)                                                                               \
  do                                                                                                                   \
  {                                                                                                                    \
    int _o = (off_);                                                                                                   \
    int _w = (width_);                                                                                                 \
    int _p = (pos_);                                                                                                   \
    if (live_count >= cap)                                                                                             \
    {                                                                                                                  \
      cap *= 2;                                                                                                        \
      live = tcc_realloc(live, sizeof(LiveRange) * cap);                                                               \
    }                                                                                                                  \
    live[live_count].off = _o;                                                                                         \
    live[live_count].width = _w;                                                                                       \
    live[live_count].pos = _p;                                                                                         \
    live_count++;                                                                                                      \
  } while (0)

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int is_store = (q->op == TCCIR_OP_STORE);

    for (int k = 0; k < 3; k++)
    {
      if (k == 0 && is_store)
        continue;
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }

      /* Precise vreg-deref read: a TEMP lval src that resolves to an exact
       * frame offset is an explicit read of those bytes — record it so the
       * dead-store passes below respect it.  Mirrors (same gate + resolver) the
       * no-poison decision in the tameness loop above, keeping live[] complete
       * for the slots that decision left tame. */
      if (dls_precise_ok && k != 0 && op.is_lval)
      {
        int rvr = irop_get_vreg(op);
        if (rvr != -1 && TCCIR_DECODE_VREG_TYPE(rvr) == TCCIR_VREG_TYPE_TEMP)
        {
          int foff;
          if (dls_vreg_frame_off(ir, rvr, i, &foff))
          {
            int w = ir_opt_store_btype_size_bytes(irop_get_btype(op));
            if (w <= 0)
              w = irop_is_64bit(op) ? 8 : 4;
            if (op.is_complex)
              w *= 2;
            DLS_LIVE_ADD(foff, w, i);
          }
        }
      }

      if (irop_get_tag(op) != IROP_TAG_STACKOFF)
        continue;
      if (!op.is_local)
        continue;
      if (irop_get_vreg(op) != -1)
        continue;
      /* STRUCT byte widths aren't recoverable from the operand alone (they
       * live in the ctype pool).  Disqualify only for LVAL STRUCT accesses
       * (they'd add an undersized live[] entry and mask a real read).  For
       * pure addr-of (!is_lval) operands the tameness analysis above already
       * classified the escape; if the address only flows to write-call
       * PARAM0 (or other tame uses), there is no untracked read to worry
       * about.  COMPLEX width IS recoverable: it's 2 * scalar size. */
      if (op.is_lval && irop_get_btype(op) == IROP_BTYPE_STRUCT)
      {
        /* BLOCK_COPY's dest is a wide write with size in src2 — not an
         * unbounded access, so it doesn't taint the slot's tameness. */
        if (q->op == TCCIR_OP_BLOCK_COPY && k == 0)
          continue;
        int off = irop_get_stack_offset(op);
        int tidx = TAME_FIND_OR_ADD(off);
        tame_slot_ok[tidx] = 0;
        continue;
      }
      int off = irop_get_stack_offset(op);

      if (op.is_lval)
      {
        int w = ir_opt_store_btype_size_bytes(irop_get_btype(op));
        if (w <= 0)
          w = irop_is_64bit(op) ? 8 : 4;
        /* _Complex T occupies 2 * sizeof(T) consecutive bytes — both
         * components share the same slot, and a complex-typed access
         * touches both halves. */
        if (op.is_complex)
          w *= 2;
        DLS_LIVE_ADD(off, w, i);
      }
      /* For !is_lval (address-of) the tameness analysis above already
       * decided whether the escape is benign; nothing to do in the live[]
       * pass here. */
    }
  }

  /* Bounded-read live[] for memcpy/memmove sources.  Their PARAM1 was tagged
   * tame for slot classification only when a constant size was found; emit
   * the corresponding read range here so dead-store elimination still
   * respects bytes the callee will actually read. */
  if (is_memcpy_like)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
        continue;
      int cid = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
      if (cid < 0 || cid >= max_call_id)
        continue;
      if (!(is_memcpy_like[cid / 8] & (1 << (cid % 8))))
        continue;
      IROperand src_p, sz_p;
      if (!ir_opt_get_call_param_operand(ir, i, 1, &src_p))
        continue;
      if (!ir_opt_get_call_param_operand(ir, i, 2, &sz_p))
        continue;
      if (irop_get_tag(sz_p) != IROP_TAG_IMM32)
        continue;
      int sz = (int)irop_get_imm64_ex(ir, sz_p);
      if (sz <= 0)
        continue;
      /* Direct addr-of-local source */
      if (irop_get_tag(src_p) == IROP_TAG_STACKOFF && src_p.is_local && !src_p.is_lval &&
          irop_get_vreg(src_p) == -1)
      {
        int off = irop_get_stack_offset(src_p);
        DLS_LIVE_ADD(off, sz, i);
      }
      else
      {
        int vr = irop_get_vreg(src_p);
        if (vr == -1)
          continue;
        int vf = VR_FLAT(vr);
        if (vf < 0)
          continue;
        int slot = vreg_slot[vf];
        if (slot == -1 || slot == VR_SLOT_AMBIG)
          continue;
        DLS_LIVE_ADD(slot, sz, i);
      }
    }
  }

  int changes = 0;

  /* Compute estimated upper bound per tame slot.  Use only OTHER tame slot
   * offsets (`Addr[StackLoc[X]]` was taken on them) as boundary markers —
   * those are confirmed allocation starts, since taking the address of a
   * local always yields the start of its allocation.  Direct StackLoc[X]
   * accesses can't be used: they may be field accesses inside the same
   * allocation as the lower slot (e.g. `s.b` at higher offset than `&s`),
   * which would underestimate the slot's actual extent and let a non-tame
   * deref through that slot reach bytes our bound thinks it can't.
   *
   * This bound is therefore a safe over-approximation: actual_size <=
   * next_tame_above - slot_offset.  When no tame slot is above, bound is 0
   * (frame top). */
  for (int t = 0; t < tame_slot_n; t++)
  {
    int s = tame_slot_off[t];
    int end = 0;
    int best_set = 0;
    for (int u = 0; u < tame_slot_n; u++)
    {
      int o = tame_slot_off[u];
      if (o > s && (!best_set || o < end))
      {
        end = o;
        best_set = 1;
      }
    }
    tame_slot_end[t] = end;
  }

  /* Per-store overlap check: returns 1 iff [_off, _off+_width) intersects
   * any non-tame slot's bounded extent.  A non-tame deref through a vreg
   * V → slot S' reads bytes within [S', tame_slot_end[idx(S')]); if our
   * store's range overlaps that, the deref might observe the write so
   * we keep it.  `has_unknown_deref` is a hard bail — a TMP deref with
   * unknown slot could touch any byte. */
#define DLS_NONTAME_RANGE_OVERLAPS(_off, _width)                                                                       \
  ({                                                                                                                   \
    int _o = (_off);                                                                                                   \
    int _w = (_width);                                                                                                 \
    int _hit = has_unknown_deref;                                                                                      \
    for (int _t = 0; !_hit && _t < tame_slot_n; _t++)                                                                  \
    {                                                                                                                  \
      if (tame_slot_ok[_t])                                                                                            \
        continue;                                                                                                      \
      int _ns = tame_slot_off[_t];                                                                                     \
      int _ne = tame_slot_end[_t];                                                                                     \
      if (_o < _ne && _ns < _o + _w)                                                                                   \
        _hit = 1;                                                                                                      \
    }                                                                                                                  \
    _hit;                                                                                                              \
  })

  /* STORE and direct-PARAM0 elimination must be conservative: a non-tame
   * escape's deref can read anywhere in the relevant slot, but we don't
   * know the slot's bounds.  So if any slot escaped non-tamely, skip these
   * offset-level eliminations entirely.  The vreg-PARAM0 path below is
   * still per-slot — it knows exactly which slot its target points into. */
  int any_nontame = has_unknown_deref;
  for (int t = 0; !any_nontame && t < tame_slot_n; t++)
    if (!tame_slot_ok[t])
      any_nontame = 1;

  if (!any_nontame)
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(dest) != IROP_TAG_STACKOFF)
      continue;
    if (!dest.is_local || irop_get_vreg(dest) != -1)
      continue;
    int off = irop_get_stack_offset(dest);
    int width;
    if (irop_get_btype(dest) == IROP_BTYPE_STRUCT)
    {
      /* STRUCT width isn't recoverable from the operand, but since
       * any_nontame is 0, STRUCT lval reads don't exist (they would have
       * triggered non-tame).  Use a conservative upper bound: the
       * distance from this offset to the frame top covers any possible
       * allocation at this slot. */
      width = off < 0 ? -off : 4;
    }
    else
    {
      width = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
      if (width <= 0)
        continue;
    }
    if (dest.is_complex)
      width *= 2;
    /* Position-aware liveness: only reads at positions AFTER this STORE
     * make it live.  Earlier reads were satisfied by an earlier definition. */
    int alive = 0;
    for (int k = 0; k < live_count; k++)
      if (live[k].pos > i &&
          off < live[k].off + live[k].width && off + width > live[k].off)
      {
        alive = 1;
        break;
      }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD LOCAL SLOT: nop STORE to StackLoc[%d] at i=%d w=%d", off, i, width);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* Plain STORE through a vreg that resolves to an EXACT frame offset (a poked
   * field of a non-escaping local accessed as `*(&slot + #k)`, before
   * disp_fusion turns it into STORE_INDEXED).  With the offset known we do
   * offset-level liveness exactly like the direct-StackLoc block above — gated
   * on !any_nontame (so live[] is complete) and !dls_has_indexed (so no
   * unrecorded indexed read of the slot exists).  This kills the dead bitfield
   * write-back left by `struct y=g; y.f+=x; return y.f;` (20040709-2 fn1*),
   * which also removes a latent wild store: the RA, treating that dead store as
   * dead, reuses its base register and writes to a garbage address. */
  if (!any_nontame && dls_precise_ok)
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!dest.is_lval)
      continue;
    int vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (irop_get_btype(dest) == IROP_BTYPE_STRUCT)
      continue;
    int foff;
    if (!dls_vreg_frame_off(ir, vr, i, &foff))
      continue;
    int width = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
    if (width <= 0)
      continue;
    if (dest.is_complex)
      width *= 2;
    if (DLS_NONTAME_RANGE_OVERLAPS(foff, width))
      continue;
    int alive = 0;
    for (int kk = 0; kk < live_count; kk++)
      if (live[kk].pos > i && foff < live[kk].off + live[kk].width && foff + width > live[kk].off)
      {
        alive = 1;
        break;
      }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD LOCAL SLOT: nop STORE via known-offset vreg at i=%d (off=%d w=%d)", i, foff, width);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* STORE_INDEXED / STORE_POSTINC elimination: the dest is a vreg V
   * (base pointer); V → tame slot S means the store hits some offset
   * within S.  Eliminate the store when:
   *   - V's slot S is tame (no untracked escape, no derived-vreg read of S),
   *   - no non-tame deref could touch any byte of S,
   *   - no live[] read AFTER this op intersects S.
   * The store's exact offset within S is unknown (a moving pointer walking
   * an array), so we use whole-slot liveness: any later read landing in
   * [S, tame_slot_end[idx(S)]) keeps the store. */
  if (!has_unknown_deref)
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int vr = irop_get_vreg(dest);
    if (vr == -1)
      continue;
    int vf = VR_FLAT(vr);
    if (vf < 0)
      continue;
    int slot = vreg_slot[vf];
    if (slot == -1 || slot == VR_SLOT_AMBIG)
      continue;
    int tidx = TAME_FIND(slot);
    if (tidx < 0 || !tame_slot_ok[tidx])
      continue;
    int slot_end = tame_slot_end[tidx];
    /* No non-tame deref through a different slot may reach into S's bytes. */
    if (DLS_NONTAME_RANGE_OVERLAPS(slot, slot_end - slot))
      continue;
    /* Whole-slot liveness: any later read in [slot, slot_end)? */
    int alive = 0;
    for (int k = 0; k < live_count; k++)
      if (live[k].pos > i && live[k].off < slot_end && live[k].off + live[k].width > slot)
      {
        alive = 1;
        break;
      }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD LOCAL SLOT: nop STORE_INDEXED via vreg at i=%d (slot=%d end=%d)", i, slot, slot_end);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* BLOCK_COPY dead-store elim: BLOCK_COPY writes `size` bytes from a const
   * data section into a local stack region [base, base+size).  If nothing
   * reads those bytes — neither a direct StackLoc[X] later in the IR, nor
   * a non-tame deref through a vreg pointing at a slot that overlaps — the
   * copy is dead.  Often kicks in after the consumer loop has been
   * eliminated by the STORE_INDEXED block + pure-call DCE cascade. */
  if (!has_unknown_deref)
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_BLOCK_COPY)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand sz = tcc_ir_op_get_src2(ir, q);
    if (irop_get_tag(dest) != IROP_TAG_STACKOFF || !dest.is_local || irop_get_vreg(dest) != -1)
      continue;
    if (irop_get_tag(sz) != IROP_TAG_IMM32)
      continue;
    int base = irop_get_stack_offset(dest);
    int width = (int)irop_get_imm64_ex(ir, sz);
    if (width <= 0)
      continue;
    if (DLS_NONTAME_RANGE_OVERLAPS(base, width))
      continue;
    int alive = 0;
    for (int k = 0; k < live_count; k++)
      if (live[k].pos > i && base < live[k].off + live[k].width && base + width > live[k].off)
      {
        alive = 1;
        break;
      }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD LOCAL SLOT: nop BLOCK_COPY at i=%d (base=%d size=%d)", i, base, width);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  if (!any_nontame && writecall_count > 0)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
        continue;
      int cid = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
      if (cid < 0 || cid >= max_call_id)
        continue;
      if (!(is_writecall[cid / 8] & (1 << (cid % 8))))
        continue;
      int sz_pidx = (writecall_size_at_2[cid / 8] & (1 << (cid % 8))) ? 2 : 1;
      IROperand p0, p_sz;
      if (!ir_opt_get_call_param_operand(ir, i, 0, &p0))
        continue;
      if (!ir_opt_get_call_param_operand(ir, i, sz_pidx, &p_sz))
        continue;
      if (irop_get_tag(p0) != IROP_TAG_STACKOFF || !p0.is_local || p0.is_lval)
        continue;
      if (irop_get_vreg(p0) != -1)
        continue;
      if (irop_get_tag(p_sz) != IROP_TAG_IMM32)
        continue;
      int base = irop_get_stack_offset(p0);
      int sz = (int)irop_get_imm64_ex(ir, p_sz);
      if (sz <= 0)
        continue;
      /* Position-aware: a read AT or BEFORE this call was satisfied by an
       * earlier write; only later reads keep the call alive. */
      int alive = 0;
      for (int k = 0; k < live_count; k++)
        if (live[k].pos > i &&
            base < live[k].off + live[k].width && base + sz > live[k].off)
        {
          alive = 1;
          break;
        }
      if (alive)
        continue;
      LOG_IR_GEN("DEAD LOCAL SLOT: nop write-call at i=%d (base=%d size=%d)", i, base, sz);
      ir_opt_nop_call_params(ir, i);
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }

  /* NEW: vreg-PARAM0 write-call elimination.
   * Handles the case where a memset/memcpy/memmove writes through a vreg
   * pointer derived from a tame local slot.  The runtime offset within the
   * slot is unknown (often a loop-induction-variable offset), so we use a
   * conservative whole-slot check: a write through V → slot S is dead iff
   *   - S is a tame slot (no untracked escape, no derived-vreg read), AND
   *   - no live[] entry has offset >= S (i.e., no direct lvalue read or
   *     store within the slot's range).
   *
   * Gated on any_nontame=0 because StackLoc offsets are not allocation
   * boundaries: `&a` and `&a[1]` of the same `char a[10]` show up as two
   * separate "slots" -10 and -9.  If the bigger one (slot -10) escaped
   * non-tamely (e.g. passed to puts), eliminating a memset that targets
   * the inner offset -9 silently loses the writes that the live read of
   * slot -10 would have observed.
   */
  if (!any_nontame && writecall_count > 0)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
        continue;
      int cid = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
      if (cid < 0 || cid >= max_call_id)
        continue;
      if (!(is_writecall[cid / 8] & (1 << (cid % 8))))
        continue;
      IROperand p0;
      if (!ir_opt_get_call_param_operand(ir, i, 0, &p0))
        continue;
      /* Skip the direct Addr[StackLoc[X]] case — handled above. */
      if (irop_get_tag(p0) == IROP_TAG_STACKOFF && !p0.is_lval && irop_get_vreg(p0) == -1)
        continue;
      /* Need a vreg PARAM0 with a known-single tame slot. */
      int vr = irop_get_vreg(p0);
      if (vr == -1)
        continue;
      int vf = VR_FLAT(vr);
      if (vf < 0)
        continue;
      int slot = vreg_slot[vf];
      if (slot == -1 || slot == VR_SLOT_AMBIG)
        continue;
      int tidx = TAME_FIND(slot);
      if (tidx < 0 || !tame_slot_ok[tidx])
        continue;
      /* Conservative whole-slot check: any live byte at offset >= slot
       * read AFTER this call?  (Earlier reads were satisfied upstream.)
       * Wide live[] entries (e.g. memcpy-source bounded reads of N bytes)
       * may start below `slot` and extend across it — check that the
       * range's end exceeds `slot`, not just its base. */
      int alive = 0;
      for (int k = 0; k < live_count; k++)
        if (live[k].pos > i && live[k].off + live[k].width > slot)
        {
          alive = 1;
          break;
        }
      if (alive)
        continue;
      LOG_IR_GEN("DEAD LOCAL SLOT: nop write-call (vreg PARAM0) at i=%d (slot=%d)", i, slot);
      ir_opt_nop_call_params(ir, i);
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }

  tcc_free(live);
  tcc_free(slr);
  tcc_free(is_writecall);
  tcc_free(writecall_size_at_2);
  tcc_free(is_memcpy_like);
  tcc_free(vreg_slot);
  tcc_free(vreg_external);
  tcc_free(tame_slot_off);
  tcc_free(tame_slot_ok);
  tcc_free(tame_slot_end);
  return changes;
#undef DLS_LIVE_ADD
#undef VR_SLOT_AMBIG
#undef VR_FLAT
#undef TAME_FIND_OR_ADD
#undef TAME_FIND
#undef DLS_NONTAME_RANGE_OVERLAPS
}

/* ============================================================================
 * Dead TEMP_LOCAL write elimination (tcc_ir_opt_dead_temp_local_elim)
 * ============================================================================
 *
 * Eliminates non-call writes to anonymous TEMP_LOCAL slots (vreg in
 * [-9, -2], allocated via get_temp_local_var()) when no subsequent
 * instruction references the same slot.
 *
 * Companion to ir_gen_dead_call_result's TEMP_LOCAL branch: dead_call
 * handles CALL-into-TEMP_LOCAL, this handles every other op shape that
 * can target a TEMP_LOCAL (ASSIGN, plain arithmetic with sub-word
 * narrowing like AND/SHL/SAR, LOAD copied into TEMP_LOCAL via dest).
 *
 * Why this is needed: after dead_local_slot_elim NOPs the StackLoc
 * writeback for a temp_local-mediated chain, the writes _into_ the
 * temp_local become dead but no other pass picks them up — DCE only
 * tracks positive-typed vregs (VAR/TEMP/PARAM) and the temp_local
 * encoding falls outside that range.
 *
 * Safety: the forward-only scan refuses to eliminate when ANY later
 * reference exists to the same TEMP_LOCAL.  For complex types a single
 * slot may be written in halves across multiple instructions, so the
 * truly-last write becomes eligible first; the iterated pipeline picks
 * up the predecessors on later passes after DCE drains the dangling
 * temp-register chains.
 */
int tcc_ir_opt_dead_temp_local_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  int changes = 0;
  /* Reverse order so a single pass chains through back-to-back writes to
   * the same temp_local: the truly-last becomes eligible first, then once
   * NOP'd the previous write sees a clear forward window, and so on. */
  for (int i = n - 1; i >= 0; i--) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Calls have their own elimination in opt_gens_call_result.c — that
     * path also nops the matching PARAM operands; skip them here. */
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      continue;
    /* Need a dest that's a TEMP_LOCAL stack slot. */
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(dest) != IROP_TAG_STACKOFF || !dest.is_local)
      continue;
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr > -2 || dest_vr < -9)
      continue;
    int my_off = irop_get_stack_offset(dest);
    int my_w = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
    if (my_w <= 0) my_w = irop_is_64bit(dest) ? 8 : 4;
    if (dest.is_complex) my_w *= 2;
    if (my_w <= 0 || my_w > 64) continue;
    /* Byte-precise forward scan: live_mask bit b is set iff our byte
     * (my_off + b) still holds the value we just wrote.  A later WRITE
     * to overlapping bytes clobbers them.  A LVAL READ overlapping a
     * still-live byte keeps us alive.  Non-LVAL addr-of usually bails,
     * but a few common shapes (PARAM-src1 of memmove/memcpy/memset with
     * constant size, possibly through a one-use LEA/ASSIGN copy) carry
     * a known [offset, offset+size) range we can check precisely. */
    uint64_t live_mask = (my_w >= 64) ? ~(uint64_t)0 : ((uint64_t)1 << my_w) - 1;
    int alive = 0;
    int dead = 0;
    for (int j = i + 1; j < n && !alive && !dead; j++) {
      IRQuadCompact *p = &ir->compact_instructions[j];
      if (p->op == TCCIR_OP_NOP)
        continue;
      for (int k = 0; k < 3 && !alive && !dead; k++) {
        IROperand po;
        int has;
        if (k == 0) { has = irop_config[p->op].has_dest;
                      if (has) po = tcc_ir_op_get_dest(ir, p); }
        else if (k == 1) { has = irop_config[p->op].has_src1;
                           if (has) po = tcc_ir_op_get_src1(ir, p); }
        else { has = irop_config[p->op].has_src2;
               if (has) po = tcc_ir_op_get_src2(ir, p); }
        if (!has) continue;
        if (irop_get_vreg(po) != dest_vr) continue;
        int po_off = irop_get_stack_offset(po);
        int po_w = ir_opt_store_btype_size_bytes(irop_get_btype(po));
        if (po_w <= 0) po_w = irop_is_64bit(po) ? 8 : 4;
        if (po.is_complex) po_w *= 2;
        /* Helper: clamp [po_off, po_off+po_w) into our [my_off, my_off+my_w)
         * to byte-offsets [lo, hi) into live_mask. */
        int lo = po_off - my_off;
        int hi = lo + po_w;
        if (lo < 0) lo = 0;
        if (hi > my_w) hi = my_w;

        if (k == 0 && irop_config[p->op].has_dest) {
          /* Subsequent write to our vreg — clobber overlapping bytes. */
          if (lo < hi) {
            uint64_t mask = (hi - lo >= 64) ? ~(uint64_t)0 : ((uint64_t)1 << (hi - lo)) - 1;
            live_mask &= ~(mask << lo);
            if (live_mask == 0) dead = 1;
          }
          continue;
        }
        if (po.is_lval) {
          /* LVAL source — direct memory read of our slot. */
          if (lo < hi) {
            uint64_t mask = (hi - lo >= 64) ? ~(uint64_t)0 : ((uint64_t)1 << (hi - lo)) - 1;
            if (live_mask & (mask << lo)) alive = 1;
          }
          continue;
        }
        /* Non-LVAL addr-of of our slot.  Resolve through one optional
         * LEA/ASSIGN copy hop, then look for a PARAM-src1 use whose call
         * has a constant size operand. */
        int param_idx = -1;
        int pidx = -1;
        int sz = -1;
        if ((p->op == TCCIR_OP_FUNCPARAMVAL || p->op == TCCIR_OP_FUNCPARAMVOID) && k == 1) {
          param_idx = j;
        } else if ((p->op == TCCIR_OP_ASSIGN || p->op == TCCIR_OP_LEA) && k == 1) {
          IROperand pd = tcc_ir_op_get_dest(ir, p);
          int32_t pd_vr = irop_get_vreg(pd);
          if (pd_vr >= 0 && TCCIR_DECODE_VREG_TYPE(pd_vr) == TCCIR_VREG_TYPE_TEMP) {
            int hit = -1;
            int multi = 0;
            for (int m = j + 1; m < n && !multi; m++) {
              IRQuadCompact *mq = &ir->compact_instructions[m];
              if (mq->op == TCCIR_OP_NOP) continue;
              for (int mk = 0; mk < 3 && !multi; mk++) {
                int mhas;
                IROperand mo;
                if (mk == 0) { mhas = irop_config[mq->op].has_dest;
                               if (mhas) mo = tcc_ir_op_get_dest(ir, mq); }
                else if (mk == 1) { mhas = irop_config[mq->op].has_src1;
                                    if (mhas) mo = tcc_ir_op_get_src1(ir, mq); }
                else { mhas = irop_config[mq->op].has_src2;
                       if (mhas) mo = tcc_ir_op_get_src2(ir, mq); }
                if (!mhas) continue;
                if (irop_get_vreg(mo) != pd_vr) continue;
                if ((mq->op == TCCIR_OP_FUNCPARAMVAL || mq->op == TCCIR_OP_FUNCPARAMVOID) &&
                    mk == 1 && hit < 0) {
                  hit = m;
                } else {
                  multi = 1;
                }
              }
            }
            if (!multi && hit >= 0) param_idx = hit;
          }
        }
        if (param_idx >= 0) {
          IRQuadCompact *pp = &ir->compact_instructions[param_idx];
          uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, pp));
          int cid = TCCIR_DECODE_CALL_ID(enc);
          pidx = TCCIR_DECODE_PARAM_IDX(enc);
          for (int m = param_idx + 1; m < n; m++) {
            IRQuadCompact *cq = &ir->compact_instructions[m];
            if (cq->op != TCCIR_OP_FUNCCALLVOID && cq->op != TCCIR_OP_FUNCCALLVAL) continue;
            if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, cq))) != cid)
              continue;
            Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, cq));
            if (!callee) break;
            const char *nm = get_tok_str(callee->v, NULL);
            if (!nm) break;
            int sz_pidx = -1;
            if (!strcmp(nm, "__aeabi_memset") || !strcmp(nm, "memset"))
              sz_pidx = 2;
            else if (!strcmp(nm, "__aeabi_memmove") || !strcmp(nm, "__aeabi_memcpy") ||
                     !strcmp(nm, "memmove") || !strcmp(nm, "memcpy"))
              sz_pidx = 2;
            else if (!strcmp(nm, "__aeabi_memmove4") || !strcmp(nm, "__aeabi_memmove8") ||
                     !strcmp(nm, "__aeabi_memcpy4") || !strcmp(nm, "__aeabi_memcpy8"))
              sz_pidx = 1;
            if (sz_pidx < 0 || (pidx != 0 && pidx != 1)) break;
            IROperand sz_op;
            if (!ir_opt_get_call_param_operand(ir, m, sz_pidx, &sz_op)) break;
            if (irop_get_tag(sz_op) != IROP_TAG_IMM32) break;
            sz = (int)irop_get_imm64_ex(ir, sz_op);
            break;
          }
        }
        if (sz <= 0) {
          /* Couldn't bound the access — pessimistically alive. */
          alive = 1;
          continue;
        }
        int rlo = po_off - my_off;
        int rhi = rlo + sz;
        if (rlo < 0) rlo = 0;
        if (rhi > my_w) rhi = my_w;
        if (rlo >= rhi) continue; /* range disjoint from our live bytes */
        uint64_t mask = (rhi - rlo >= 64) ? ~(uint64_t)0 : ((uint64_t)1 << (rhi - rlo)) - 1;
        mask <<= rlo;
        if (pidx == 0) {
          /* Write through addr-of-our-slot. */
          live_mask &= ~mask;
          if (live_mask == 0) dead = 1;
        } else if (live_mask & mask) {
          alive = 1;
        }
      }
    }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD TEMP_LOCAL: nop op=%d at i=%d (vr=%d off=%d w=%d %s)",
               q->op, i, dest_vr, my_off, my_w, dead ? "overwritten" : "no-reader");
    q->op = TCCIR_OP_NOP;
    changes++;
  }
  return changes;
}

/* ============================================================================
 * Address-of-VAR Forwarding (tcc_ir_opt_addrof_var_fwd)
 * ============================================================================
 *
 * Within a basic block, forward a constant ASSIGN to a VAR through a LEA to
 * the deref of the LEA's result:
 *
 *   V0 <-- #N [ASSIGN]
 *   T0 <-- &V0           [LEA]
 *   ...T0***DEREF***...  →  ...#N...
 *
 * Companion to var_to_tmp (opt_promote.c), which handles VARs whose address
 * is NOT taken.  Here we handle the case where &V is taken but the address
 * only flows to local derefs (no escape via call, no store-through, no
 * second use of the LEA result as a value).
 *
 * This pattern is produced by __attribute__((cleanup)) — the inlined cleanup
 * call materializes &local just to re-dereference it in the inlined body.
 */
int tcc_ir_opt_addrof_var_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 3)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* A constant write to a local VAR can appear as either ASSIGN or STORE
     * depending on the frontend path that produced it. Both are equivalent
     * here: dest is the VAR's memory slot, src1 is the constant. */
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t v_dest_vr = irop_get_vreg(dest);
    if (v_dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(v_dest_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src) != IROP_TAG_IMM32)
      continue;

    int v_btype = irop_get_btype(dest);
    if (v_btype != IROP_BTYPE_INT32)
      continue;

    int32_t imm_val = src.u.imm32;

    /* Track aliases of &V_p propagated through `V_a = T_alias [STORE]` and
     * `T_b = V_alias [ASSIGN]` copy chains.  Each tracked vreg holds the
     * value of &V_p; any *T deref through it yields V_p's value (= imm_val). */
#define ADDROF_VAR_MAX_REWRITES 32
#define ADDROF_VAR_MAX_ALIASES 16
    int rewrites_idx[ADDROF_VAR_MAX_REWRITES];
    int rewrites_slot[ADDROF_VAR_MAX_REWRITES]; /* 0 = src1, 1 = src2 */
    int rewrite_count = 0;
    /* alias[]: vregs that currently hold &V_p (TEMP or VAR). */
    int32_t alias[ADDROF_VAR_MAX_ALIASES];
    int alias_count = 0;
    int aborted = 0;

    for (int j = i + 1; j < n && !aborted; j++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP)
        continue;

      if (jq->is_jump_target)
        break;
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF || jq->op == TCCIR_OP_IJUMP ||
          jq->op == TCCIR_OP_RETURNVALUE || jq->op == TCCIR_OP_RETURNVOID ||
          jq->op == TCCIR_OP_SWITCH_TABLE)
        break;
      if (jq->op == TCCIR_OP_FUNCCALLVAL || jq->op == TCCIR_OP_FUNCCALLVOID)
        break;

      int handled = 0;

      /* LEA T = &V_p creates the first alias. */
      if (jq->op == TCCIR_OP_LEA)
      {
        IROperand ldest = tcc_ir_op_get_dest(ir, jq);
        IROperand lsrc = tcc_ir_op_get_src1(ir, jq);
        int32_t lsrc_vr = irop_get_vreg(lsrc);
        int32_t ldest_vr = irop_get_vreg(ldest);
        if (lsrc_vr == v_dest_vr && !lsrc.is_lval &&
            TCCIR_DECODE_VREG_TYPE(ldest_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          if (alias_count >= ADDROF_VAR_MAX_ALIASES)
          {
            aborted = 1;
            break;
          }
          alias[alias_count++] = ldest_vr;
          handled = 1;
        }
      }

      /* STORE V_a <-- T_alias  (V_a becomes an alias holding &V_p).  The src
       * carries is_lval=0 because we're storing a TEMP's pointer value. */
      if (!handled && jq->op == TCCIR_OP_STORE)
      {
        IROperand sdest = tcc_ir_op_get_dest(ir, jq);
        IROperand ssrc = tcc_ir_op_get_src1(ir, jq);
        int32_t sdest_vr = irop_get_vreg(sdest);
        int32_t ssrc_vr = irop_get_vreg(ssrc);
        if (!ssrc.is_lval && sdest_vr >= 0 && ssrc_vr >= 0 &&
            TCCIR_DECODE_VREG_TYPE(sdest_vr) == TCCIR_VREG_TYPE_VAR &&
            TCCIR_DECODE_VREG_TYPE(ssrc_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          for (int k = 0; k < alias_count; k++)
          {
            if (alias[k] == ssrc_vr)
            {
              if (alias_count >= ADDROF_VAR_MAX_ALIASES)
              {
                aborted = 1;
              }
              else
              {
                alias[alias_count++] = sdest_vr;
                handled = 1;
              }
              break;
            }
          }
        }
      }

      /* ASSIGN T_b <-- V_alias  (T_b becomes an alias holding &V_p).  The src
       * carries is_lval=1 — this is "load the VAR's slot value", which holds
       * the pointer.  Note: this is NOT a deref of the pointer, it's a load
       * of the slot's contents — so we propagate the alias rather than the
       * underlying constant. */
      if (!handled && jq->op == TCCIR_OP_ASSIGN)
      {
        IROperand adest = tcc_ir_op_get_dest(ir, jq);
        IROperand asrc = tcc_ir_op_get_src1(ir, jq);
        int32_t adest_vr = irop_get_vreg(adest);
        int32_t asrc_vr = irop_get_vreg(asrc);
        if (adest_vr >= 0 && asrc_vr >= 0 &&
            TCCIR_DECODE_VREG_TYPE(adest_vr) == TCCIR_VREG_TYPE_TEMP &&
            TCCIR_DECODE_VREG_TYPE(asrc_vr) == TCCIR_VREG_TYPE_VAR &&
            asrc.is_lval)
        {
          for (int k = 0; k < alias_count; k++)
          {
            if (alias[k] == asrc_vr)
            {
              if (alias_count >= ADDROF_VAR_MAX_ALIASES)
              {
                aborted = 1;
              }
              else
              {
                alias[alias_count++] = adest_vr;
                handled = 1;
              }
              break;
            }
          }
        }
      }

      if (handled)
        continue;
      if (aborted)
        break;

      /* Any redefinition of V_p or any tracked alias invalidates. VAR dest
       * always has is_lval=1, so vreg comparison alone is the right check. */
      if (irop_config[jq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, jq);
        int32_t d_vr = irop_get_vreg(d);
        if (d_vr == v_dest_vr)
        {
          aborted = 1;
          break;
        }
        for (int k = 0; k < alias_count; k++)
        {
          if (d_vr == alias[k])
          {
            aborted = 1;
            break;
          }
        }
        if (aborted)
          break;
      }

      /* Scan src1/src2 for uses of any tracked alias. */
      for (int s = 0; s < 2 && !aborted; s++)
      {
        if (s == 0 && !irop_config[jq->op].has_src1)
          continue;
        if (s == 1 && !irop_config[jq->op].has_src2)
          continue;
        IROperand u = (s == 0) ? tcc_ir_op_get_src1(ir, jq) : tcc_ir_op_get_src2(ir, jq);
        int32_t u_vr = irop_get_vreg(u);
        if (u_vr < 0)
          continue;

        for (int k = 0; k < alias_count; k++)
        {
          if (u_vr != alias[k])
            continue;
          /* Rewrites are only safe when the alias is a TEMP and the read
           * is a deref of the pointer it holds (T***DEREF*** == *(&V) == V).
           * For VAR aliases, a read with is_lval=1 is a slot-load that
           * returns the stored pointer value, NOT a deref — chain-extension
           * (handled above) propagates the alias; if we reach here with a
           * VAR alias the use isn't a recognized chain shape, so bail. */
          if (TCCIR_DECODE_VREG_TYPE(u_vr) != TCCIR_VREG_TYPE_TEMP)
          {
            aborted = 1;
            break;
          }
          if (!u.is_lval)
          {
            /* TEMP alias read as a value but the consuming op isn't one of
             * our chain-extending shapes (already handled above). The
             * address escapes — bail. */
            aborted = 1;
            break;
          }
          if (irop_get_btype(u) != v_btype)
          {
            aborted = 1;
            break;
          }
          if (rewrite_count >= ADDROF_VAR_MAX_REWRITES)
          {
            aborted = 1;
            break;
          }
          rewrites_idx[rewrite_count] = j;
          rewrites_slot[rewrite_count] = s;
          rewrite_count++;
        }
      }
    }

    if (aborted || rewrite_count == 0)
      continue;

    for (int r = 0; r < rewrite_count; r++)
    {
      int idx = rewrites_idx[r];
      IROperand orig = (rewrites_slot[r] == 1) ? tcc_ir_get_src2(ir, idx) : tcc_ir_get_src1(ir, idx);
      IROperand newop = irop_make_imm32(-1, imm_val, irop_get_btype(orig));
      newop.is_unsigned = orig.is_unsigned;
      if (rewrites_slot[r] == 1)
        tcc_ir_set_src2(ir, idx, newop);
      else
        tcc_ir_set_src1(ir, idx, newop);
      changes++;
    }
#undef ADDROF_VAR_MAX_REWRITES
#undef ADDROF_VAR_MAX_ALIASES
  }

  return changes;
}

/* ============================================================================
 * Global Store-Load Forwarding (tcc_ir_opt_global_sl_fwd)
 * ============================================================================
 *
 * Within a single basic block, forward the value of a STORE to a GlobalSym
 * into subsequent uses of the same global as an lval (deref) source operand:
 *
 *   STORE GlobalSym(X)***DEREF*** <-- T_val
 *   ... (no call, no aliasing store) ...
 *   T_new <-- GlobalSym(X)***DEREF*** ADD #C
 *
 *     becomes
 *
 *   STORE GlobalSym(X)***DEREF*** <-- T_val
 *   ...
 *   T_new <-- T_val ADD #C
 *
 * Distinct from cse_global_load (which only deduplicates LOAD ops between
 * themselves and skips globals that are written) and from sl_forward (which
 * tracks stack locals only).  Targets the read-modify-write chain pattern
 * left after addrof_var_fwd collapses helper-inlined accumulator updates.
 *
 * Invalidation rules:
 *   - any CALL clears all tracked entries (callee may write any global)
 *   - a STORE through an unknown pointer clears all entries (alias unknown)
 *   - a STORE to a different GlobalSym keeps entries (globals don't alias)
 *   - a redefinition of the tracked T_val invalidates that entry
 *   - any BB boundary clears all entries
 */
int tcc_ir_opt_global_sl_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;



  /* Computed goto (IJUMP) can transfer control to any addr-taken label,
   * but TCC doesn't mark those labels with is_jump_target.  Without that
   * info, in-BB forwarding through fall-through into a label that's also
   * an IJUMP target is unsafe.  Skip the whole function in that case. */
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
    int     imm_is_i32; /* 1 if value_imm fits in int32 (build imm32 operand);
                           0 means build a fresh i64 pool entry on each use */
  } entries[GSLFWD_MAX_ENTRIES];
  int entry_count = 0;

  /* The clear-at-BB-boundary logic below relies on is_jump_target being
   * accurate, but earlier pipeline passes (e.g. fold/DCE turning a JMP into a
   * NOP, or jump threading retargeting a branch) can leave it stale.  A real
   * jump target whose flag was wrongly cleared would NOT reset the tracking
   * table, letting a store forward across a loop back-edge into the loop test
   * (miscompile: pr59387 collapsed to an infinite loop).  Recompute the set of
   * actual JUMP/JUMPIF targets here and treat any of them as a boundary.  We
   * only ADD boundaries (never drop is_jump_target), so switch-table/IJUMP
   * target labels — which aren't JUMP/JUMPIF destinations — stay protected. */
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

    /* Multi-predecessor join points clear everything — we can't know which
     * path's tracked state holds.  Clearing is for the state going *into*
     * this instruction; the instruction itself (e.g. a STORE that is also
     * the target of a forward JUMP) should still be processed below to seed
     * tracking for subsequent ops.  JUMP/JUMPIF themselves don't write
     * memory, so we don't clear there; the fall-through after a JUMPIF (or
     * any sequential successor that's not is_jump_target) safely inherits
     * state.  RETURN / SWITCH_TABLE / IJUMP transfer control without writing,
     * but their successors are unreachable as fall-through, so clearing is
     * just defensive. */
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

    /* Rewrite eligible deref uses of any tracked global before processing
     * this instruction's effect on the table.  Skip the dest slot — that
     * one is the store destination, not a value read. */
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
            /* Forward a still-valid TEMP vreg holding the stored value.
             * Restricting to TEMP keeps the forward sound: TEMPs are only ever
             * written as non-lval dests, so the redefinition scan at the bottom
             * of the loop reliably drops the entry the moment the value is
             * overwritten before a use.  Replacing the global deref with a plain
             * register read also removes a redundant memory load even when no
             * constant folding follows (LOAD b == the value just stored). */
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
          /* If we replaced a LOAD's deref source with a plain value, the LOAD
           * is now a pure copy — convert to ASSIGN so downstream const_prop /
           * branch_fold see it as a known-constant definition. */
          if (q->op == TCCIR_OP_LOAD && s == 0)
            q->op = TCCIR_OP_ASSIGN;
          changes++;
          break;
        }
      }
    }

    /* Now handle this instruction's effect on the tracking table. */
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
        /* Two trackable forms:
         *   (a) plain value vreg with no lval/sym flag — substitute with vreg
         *   (b) immediate constant — substitute as imm32/i64
         * Anything else (e.g. another lval, address-of) we conservatively
         * invalidate.  Both INT32 and INT64 widths are tracked. */
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

      /* STORE to a local stack slot is safe (can't alias globals).  Anything
       * else (unknown pointer write) invalidates everything. */
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

    /* If this op redefines a tracked value vreg, drop the entry.
     * Immediate-valued entries (value_vr == -1) are independent of any
     * specific vreg and need no invalidation here. */
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
int tcc_ir_opt_deref_fwd_ex(IROptCtx *ctx) { return tcc_ir_opt_deref_fwd(ctx->ir); }
int tcc_ir_opt_ptr_load_cse_ex(IROptCtx *ctx) { return tcc_ir_opt_ptr_load_cse(ctx->ir); }
int tcc_ir_opt_ptr_store_load_fwd_ex(IROptCtx *ctx) { return tcc_ir_opt_ptr_store_load_fwd(ctx->ir); }
int tcc_ir_opt_entry_store_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_entry_store_prop(ctx->ir); }
int tcc_ir_opt_store_redundant_ex(IROptCtx *ctx) { return tcc_ir_opt_store_redundant(ctx->ir); }
int tcc_ir_opt_dead_local_slot_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_local_slot_elim(ctx->ir); }
int tcc_ir_opt_dead_temp_local_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_temp_local_elim(ctx->ir); }
int tcc_ir_opt_addrof_var_fwd_ex(IROptCtx *ctx) { return tcc_ir_opt_addrof_var_fwd(ctx->ir); }
int tcc_ir_opt_global_sl_fwd_ex(IROptCtx *ctx) { return tcc_ir_opt_global_sl_fwd(ctx->ir); }

/* ============================================================================
 * Invariant Global LOAD Hoist (tcc_ir_opt_invariant_global_load_hoist)
 * ============================================================================
 *
 * Cross-BB CSE for `ASSIGN T <- GlobalSym(X)***DEREF***` and `LOAD` ops that
 * read from a non-static global X.  Targets the unrolled-check pattern from
 * gcc.c-torture/compile/961126-1.c where the same `*p` is reloaded across
 * every iteration of a goto-chain because the existing BB-local global LOAD
 * CSE clears its table at each jump target.
 *
 * Safety conditions (all required):
 *   - Function has only forward control flow:
 *     no IJUMP, no SWITCH_TABLE, no SETJMP/LONGJMP, no INLINE_ASM, and every
 *     JUMP/JUMPIF target is strictly greater than its source.
 *   - X is non-volatile and has no direct STORE to GlobalSym(X) in the function.
 *   - Between the anchor LOAD and the candidate reuse position, no instruction
 *     clobbers globals (CALL, STORE_INDEXED, STORE_POSTINC, STORE through a
 *     non-local non-direct-global destination, etc.).
 *   - The anchor LOAD dominates the reuse position: no JUMP/JUMPIF from a
 *     source outside [anchor, reuse] targets a position in (anchor, reuse].
 *
 * Replaces the reuse op with `ASSIGN T_reuse <- T_anchor`; copy_prop and DCE
 * collapse the chain.
 */
int tcc_ir_opt_invariant_global_load_hoist(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  /* Abort on any unusual control flow that we don't reason about. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
      case TCCIR_OP_IJUMP:
      case TCCIR_OP_SWITCH_TABLE:
      case TCCIR_OP_SETJMP:
      case TCCIR_OP_LONGJMP:
      case TCCIR_OP_NL_SETJMP:
      case TCCIR_OP_NL_LONGJMP:
      case TCCIR_OP_INLINE_ASM:
      case TCCIR_OP_ASM_INPUT:
      case TCCIR_OP_ASM_OUTPUT:
      case TCCIR_OP_BUILTIN_APPLY:
      case TCCIR_OP_BUILTIN_APPLY_ARGS:
      case TCCIR_OP_BUILTIN_RETURN:
        return 0;
      case TCCIR_OP_JUMP:
      case TCCIR_OP_JUMPIF:
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int target = (int)irop_get_imm64_ex(ir, dest);
        if (target <= i)
          return 0; /* backward jump - loop or weirdness */
        break;
      }
      default:
        break;
    }
  }

  /* Collect direct stores to globals: those globals are not eligible. */
#define IGLH_MAX_WRITTEN 16
  Sym *written_globals[IGLH_MAX_WRITTEN];
  int num_written = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand sdest = tcc_ir_op_get_dest(ir, q);
    if (!sdest.is_sym || !sdest.is_lval)
      continue;
    IRPoolSymref *sref = irop_get_symref_ex(ir, sdest);
    if (!sref || !sref->sym)
      continue;
    int already = 0;
    for (int k = 0; k < num_written; k++)
      if (written_globals[k] == sref->sym)
      {
        already = 1;
        break;
      }
    if (!already && num_written < IGLH_MAX_WRITTEN)
      written_globals[num_written++] = sref->sym;
  }

  /* Precompute per-instruction "clobbers any global" flag.  Calls and stores
   * through unknown addresses can write any global; direct stores to a known
   * GlobalSym do not alias other globals (that case is handled per-symbol via
   * the written_globals list) and stores to a local stack slot cannot alias
   * any global. */
  unsigned char *clobber = tcc_mallocz((size_t)n);
  if (!clobber)
    return 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_BLOCK_COPY:
      case TCCIR_OP_TRAP:
        clobber[i] = 1;
        break;
      case TCCIR_OP_STORE:
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (dest.is_sym && dest.is_lval)
        {
          IRPoolSymref *sref = irop_get_symref_ex(ir, dest);
          if (!sref || !sref->sym)
            clobber[i] = 1;
          /* else: direct global store - tracked via written_globals */
        }
        else if (!dest.is_local)
        {
          clobber[i] = 1;
        }
        break;
      }
      default:
        break;
    }
  }

#define IGLH_MAX_TRACKED 16
  struct
  {
    Sym *sym;
    int64_t addend;
    int btype;
    int32_t result_vr;
    int load_idx;
  } tracked[IGLH_MAX_TRACKED];
  int num_tracked = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (clobber[i])
    {
      num_tracked = 0;
      continue;
    }

    /* Identify a load of a global symbol: either an explicit LOAD op or an
     * ASSIGN whose src1 is a SYMREF lval (which the frontend emits for
     * `T = *g_ptr` reads of globals). */
    int is_load_like = 0;
    if ((q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) &&
        irop_config[q->op].has_dest && irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (src1.is_sym && src1.is_lval)
        is_load_like = 1;
    }

    if (!is_load_like)
    {
      /* If this op redefines a tracked vreg, drop that entry. */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t dvr = irop_get_vreg(d);
        if (dvr >= 0 && !d.is_lval)
        {
          for (int k = 0; k < num_tracked;)
          {
            if (tracked[k].result_vr == dvr)
              tracked[k] = tracked[--num_tracked];
            else
              k++;
          }
        }
      }
      continue;
    }

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || dest.is_lval)
      continue;

    IRPoolSymref *ref = irop_get_symref_ex(ir, src1);
    if (!ref || !ref->sym)
      continue;

    if (ref->sym->type.t & VT_VOLATILE)
      continue;

    /* Skip symbols that are directly stored to in this function. */
    int is_written = 0;
    for (int k = 0; k < num_written; k++)
      if (written_globals[k] == ref->sym)
      {
        is_written = 1;
        break;
      }
    if (is_written)
    {
      /* Also drop any existing tracked entry for this symbol. */
      for (int k = 0; k < num_tracked;)
      {
        if (tracked[k].sym == ref->sym)
          tracked[k] = tracked[--num_tracked];
        else
          k++;
      }
      continue;
    }

    int dest_btype = irop_get_btype(dest);
    int found = -1;
    for (int k = 0; k < num_tracked; k++)
    {
      if (tracked[k].sym == ref->sym && tracked[k].addend == ref->addend &&
          tracked[k].btype == dest_btype)
      {
        found = k;
        break;
      }
    }

    if (found >= 0)
    {
      int anchor_idx = tracked[found].load_idx;
      /* Dominance check: no JUMP/JUMPIF from a source outside [anchor, i]
       * may target a position in (anchor, i].  A jump that skips over the
       * anchor would mean the value isn't available on all paths into i.
       * Since we already verified there are no backward jumps, source > i
       * with target in (anchor, i] is impossible. So we only need to check
       * jumps with source < anchor. */
      int safe = 1;
      for (int j = 0; j < anchor_idx; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int tgt = (int)irop_get_imm64_ex(ir, jdest);
        if (tgt > anchor_idx && tgt <= i)
        {
          safe = 0;
          break;
        }
      }
      if (safe)
      {
        q->op = TCCIR_OP_ASSIGN;
        IROperand new_src = irop_make_vreg(tracked[found].result_vr, dest_btype);
        new_src.is_unsigned = src1.is_unsigned;
        tcc_ir_set_src1(ir, i, new_src);
        LOG_IR_GEN("IGLH@i=%d: replaced load of sym=%p with vreg %d (anchor i=%d)",
                   i, (void *)ref->sym, tracked[found].result_vr, anchor_idx);
        changes++;
        continue;
      }
      /* not safe - fall through and add a new tracking entry from this load */
    }

    if (num_tracked < IGLH_MAX_TRACKED)
    {
      tracked[num_tracked].sym = ref->sym;
      tracked[num_tracked].addend = ref->addend;
      tracked[num_tracked].btype = dest_btype;
      tracked[num_tracked].result_vr = dest_vr;
      tracked[num_tracked].load_idx = i;
      num_tracked++;
    }
  }

  tcc_free(clobber);
#undef IGLH_MAX_TRACKED
#undef IGLH_MAX_WRITTEN
  return changes;
}

int tcc_ir_opt_invariant_global_load_hoist_ex(IROptCtx *ctx) { return tcc_ir_opt_invariant_global_load_hoist(ctx->ir); }

extern int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q);

/* ============================================================================
 * Invariant TEMP-deref Hoist (tcc_ir_opt_invariant_temp_deref_hoist)
 * ============================================================================
 *
 * Companion to tcc_ir_opt_invariant_global_load_hoist.  After that pass has
 * collapsed cross-BB reloads of a global pointer T into a single
 *   T = ASSIGN GlobalSym(P)***DEREF***
 * the body of the function may still contain many `op T***DEREF***` uses
 * (e.g. `CMP T***DEREF***, X` repeated per iteration of an unrolled chain).
 * Each one re-emits an LDR before the operation.
 *
 * For TEMPs that are defined once (load of a pointer from memory) and never
 * redefined, this pass inserts a single ASSIGN T_v <- T***DEREF*** right
 * after T's definition and rewrites every `T***DEREF***` use within the same
 * clobber-free single-entry region to T_v (non-lval).  Subsequent codegen
 * keeps T_v in a register, removing the per-use LDR.
 *
 * Safety conditions match the global LOAD hoist pass: forward control flow
 * only, no aliasing stores or calls between def and last use, and no jump
 * skipping over the def to land in the use range.
 */
int tcc_ir_opt_invariant_temp_deref_hoist(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  /* Same control-flow preconditions as the global-load hoist. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
      case TCCIR_OP_IJUMP:
      case TCCIR_OP_SWITCH_TABLE:
      case TCCIR_OP_SETJMP:
      case TCCIR_OP_LONGJMP:
      case TCCIR_OP_NL_SETJMP:
      case TCCIR_OP_NL_LONGJMP:
      case TCCIR_OP_INLINE_ASM:
      case TCCIR_OP_ASM_INPUT:
      case TCCIR_OP_ASM_OUTPUT:
      case TCCIR_OP_BUILTIN_APPLY:
      case TCCIR_OP_BUILTIN_APPLY_ARGS:
      case TCCIR_OP_BUILTIN_RETURN:
        return 0;
      case TCCIR_OP_JUMP:
      case TCCIR_OP_JUMPIF:
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int target = (int)irop_get_imm64_ex(ir, dest);
        if (target <= i)
          return 0;
        break;
      }
      default:
        break;
    }
  }

  /* Clobber map (writes that could alias any pointer-derefed memory). */
  unsigned char *clobber = tcc_mallocz((size_t)n);
  if (!clobber)
    return 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_BLOCK_COPY:
      case TCCIR_OP_TRAP:
        clobber[i] = 1;
        break;
      case TCCIR_OP_STORE:
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        /* A direct store to any GlobalSym could alias an unknown pointer.
         * A store to a local stack slot cannot. */
        if (dest.is_sym && dest.is_lval)
          clobber[i] = 1;
        else if (!dest.is_local)
          clobber[i] = 1;
        break;
      }
      default:
        break;
    }
  }

#define ITDH_MAX_CANDS 16
  struct
  {
    int32_t temp_vr;
    int def_idx;
    int first_use;
    int last_use;
    int use_count;
    int btype;
    int is_unsigned;
    int32_t hoist_vr;
    int hoist_idx; /* position of the inserted ASSIGN — its own src must not be rewritten */
  } cands[ITDH_MAX_CANDS];
  int num_cands = 0;

  /* Pass 1: collect candidate TEMPs - defined by an ASSIGN/LOAD whose source
   * operand is an lval (so the TEMP holds a value loaded from memory).  These
   * are exactly the "loaded pointer" TEMPs whose subsequent T***DEREF*** uses
   * become per-iteration LDRs in the backend. */
  for (int i = 0; i < n && num_cands < ITDH_MAX_CANDS; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
      continue;
    if (!irop_config[q->op].has_dest || !irop_config[q->op].has_src1)
      continue;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!src1.is_lval || dest.is_lval)
      continue;
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    cands[num_cands].temp_vr = dest_vr;
    cands[num_cands].def_idx = i;
    cands[num_cands].first_use = -1;
    cands[num_cands].last_use = -1;
    cands[num_cands].use_count = 0;
    cands[num_cands].btype = -1;
    cands[num_cands].is_unsigned = 0;
    cands[num_cands].hoist_vr = -1;
    cands[num_cands].hoist_idx = -1;
    num_cands++;
  }

  if (num_cands == 0)
  {
    tcc_free(clobber);
    return 0;
  }

  /* Pass 2: scan IR.  For each candidate, check redefinitions and collect
   * lval-deref uses.  Mark candidates whose TEMP is redefined as invalid. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Redefinition check (skip the candidate's own def). */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr >= 0 && !d.is_lval)
      {
        for (int c = 0; c < num_cands; c++)
        {
          if (cands[c].use_count < 0)
            continue;
          if (cands[c].temp_vr == dvr && i != cands[c].def_idx)
            cands[c].use_count = -1; /* mark invalid */
        }
      }
    }

    /* Operand scan for lval uses of the candidate TEMP. */
    for (int s = 0; s < 3; s++)
    {
      int has;
      IROperand op;
      if (s == 0)
      {
        has = irop_config[q->op].has_dest;
        if (!has)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (s == 1)
      {
        has = irop_config[q->op].has_src1;
        if (!has)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        has = irop_config[q->op].has_src2;
        if (!has)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (!op.is_lval)
        continue;
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      for (int c = 0; c < num_cands; c++)
      {
        if (cands[c].use_count < 0)
          continue;
        if (cands[c].temp_vr != vr)
          continue;
        if (i <= cands[c].def_idx)
          continue; /* the def itself - ignore */
        int btype = irop_get_btype(op);
        if (cands[c].first_use < 0)
        {
          cands[c].first_use = i;
          cands[c].btype = btype;
          cands[c].is_unsigned = op.is_unsigned;
        }
        else if (btype != cands[c].btype)
        {
          cands[c].use_count = -1;
          continue;
        }
        cands[c].last_use = i;
        cands[c].use_count++;
      }
    }
  }

  /* Pass 3: safety filter — drop candidates whose use range has a clobber or
   * whose anchor doesn't dominate the uses (some external jump skips the def). */
  for (int c = 0; c < num_cands; c++)
  {
    if (cands[c].use_count < 2)
      continue;
    int anchor = cands[c].def_idx;
    int last = cands[c].last_use;
    int safe = 1;
    for (int j = anchor + 1; j <= last; j++)
    {
      if (clobber[j])
      {
        safe = 0;
        break;
      }
    }
    if (safe)
    {
      for (int j = 0; j < anchor; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int tgt = (int)irop_get_imm64_ex(ir, jdest);
        if (tgt > anchor && tgt <= last)
        {
          safe = 0;
          break;
        }
      }
    }
    if (!safe)
      cands[c].use_count = -1;
  }

  tcc_free(clobber);

  /* Pass 4: insert hoist ASSIGNs.  Process in REVERSE order of def_idx so
   * earlier-positioned candidates' indices are unaffected by later insertions
   * (each insertion shifts only positions at-or-after itself). */
  for (int pass = 0; pass < num_cands; pass++)
  {
    int best = -1;
    int best_idx = -1;
    for (int c = 0; c < num_cands; c++)
    {
      if (cands[c].use_count < 2 || cands[c].hoist_vr >= 0)
        continue;
      if (cands[c].def_idx > best_idx)
      {
        best_idx = cands[c].def_idx;
        best = c;
      }
    }
    if (best < 0)
      break;

    int c = best;
    int32_t t_new = tcc_ir_vreg_alloc_temp(ir);
    if (t_new < 0)
      continue;

    IROperand new_dest = irop_make_vreg(t_new, cands[c].btype);
    new_dest.is_unsigned = cands[c].is_unsigned;
    IROperand new_src = irop_make_vreg(cands[c].temp_vr, cands[c].btype);
    new_src.is_lval = 1;
    new_src.is_unsigned = cands[c].is_unsigned;

    if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
      tcc_ir_pool_ensure(ir, 2);

    IRQuadCompact new_q = {0};
    new_q.op = TCCIR_OP_ASSIGN;
    new_q.operand_base = tcc_ir_pool_add(ir, new_dest);
    tcc_ir_pool_add(ir, new_src);

    int insert_pos = cands[c].def_idx + 1;
    if (gsym_cse_insert_before(ir, insert_pos, &new_q) < 0)
      continue;

    cands[c].hoist_vr = t_new;
    cands[c].hoist_idx = insert_pos;
    /* Other candidates' positions at-or-after insert_pos shift by 1. */
    for (int c2 = 0; c2 < num_cands; c2++)
    {
      if (c2 == c)
        continue;
      if (cands[c2].def_idx >= insert_pos)
        cands[c2].def_idx++;
      if (cands[c2].first_use >= insert_pos)
        cands[c2].first_use++;
      if (cands[c2].last_use >= insert_pos)
        cands[c2].last_use++;
      if (cands[c2].hoist_idx >= 0 && cands[c2].hoist_idx >= insert_pos)
        cands[c2].hoist_idx++;
    }
    n++;
  }

  /* Pass 5: rewrite all `T***DEREF***` uses to the hoisted TEMP (non-lval).
   * Rewriting is by vreg identity so it's independent of position shifts.
   * Skip the inserted ASSIGN itself (whose src1 is the only legitimate
   * `T_old***DEREF***` use that must remain). */
  for (int c = 0; c < num_cands; c++)
  {
    if (cands[c].hoist_vr < 0)
      continue;
    for (int i = cands[c].def_idx + 1; i < n; i++)
    {
      if (i == cands[c].hoist_idx)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      for (int s = 0; s < 3; s++)
      {
        int has;
        IROperand op;
        if (s == 0)
        {
          has = irop_config[q->op].has_dest;
          if (!has)
            continue;
          op = tcc_ir_op_get_dest(ir, q);
        }
        else if (s == 1)
        {
          has = irop_config[q->op].has_src1;
          if (!has)
            continue;
          op = tcc_ir_op_get_src1(ir, q);
        }
        else
        {
          has = irop_config[q->op].has_src2;
          if (!has)
            continue;
          op = tcc_ir_op_get_src2(ir, q);
        }
        if (!op.is_lval)
          continue;
        if (irop_get_vreg(op) != cands[c].temp_vr)
          continue;
        if (irop_get_btype(op) != cands[c].btype)
          continue;
        IROperand repl = irop_make_vreg(cands[c].hoist_vr, cands[c].btype);
        repl.is_unsigned = cands[c].is_unsigned;
        if (s == 0)
          tcc_ir_op_set_dest(ir, q, repl);
        else if (s == 1)
          tcc_ir_set_src1(ir, i, repl);
        else
          tcc_ir_set_src2(ir, i, repl);
        changes++;
      }
    }
  }

#undef ITDH_MAX_CANDS
  return changes;
}

int tcc_ir_opt_invariant_temp_deref_hoist_ex(IROptCtx *ctx) { return tcc_ir_opt_invariant_temp_deref_hoist(ctx->ir); }

/* ============================================================================
 * Dead Static Store Elimination (tcc_ir_opt_dead_static_store_elim)
 * ----------------------------------------------------------------------------
 * Eliminate STORE / STORE_INDEXED / STORE_POSTINC operations whose destination
 * is a SYMREF to a file-scope static global that the end-of-TU read-set
 * analysis marked as tu_no_readers (no reachable function in the TU reads it,
 * and its address has not escaped).
 *
 * Runs only during the late_reopt phase — sym->a.tu_no_readers is set only
 * after the entire TU has been parsed and the call-graph reachability /
 * read-set analysis has completed.  During the initial per-function compile
 * we have no TU-wide information yet, so this pass is a no-op.
 *
 * Stores to such globals would never be observed by program execution, so
 * NOPing them is safe.  Cascade with DCE removes the materialization
 * sequence that fed the now-dead store (RHS computation, LEA for the
 * symbol's address, etc.).
 * ============================================================================ */
/* Helper: extract a SYMREF Sym* from a STORE's destination, accounting for
 * pre-fusion forms.  Direct shapes:
 *
 *   - dest = SYMREF (lval or, for STORE_INDEXED/POSTINC, possibly cleared lval)
 *
 * Indirect shape (pre-fusion):
 *
 *   - dest = TEMP (lval, "deref through pointer")
 *     where TEMP is defined exactly once by ADD/LEA/ASSIGN whose src1 is a
 *     SYMREF — i.e. `T = &sym + idx_scaled; *T = value`
 *
 * Returns the underlying Sym* on success, NULL otherwise. */
/* Forward-trace vreg map for resolving STORE destinations through temps.
 * Used by dead_static_store_elim to handle post-optimization IR where
 * STORE_INDEXED/STORE_POSTINC use temp vregs rather than direct SYMREFs.
 * Same logic as the TU summary vreg map in opt.c. */
#define DSS_VREG_MAP_MAX 128
typedef struct
{
  int32_t vreg;
  Sym *sym;
} DssVregEntry;

static Sym *dss_vreg_map_lookup(const DssVregEntry *map, int count, int32_t vr)
{
  for (int i = 0; i < count; i++)
    if (map[i].vreg == vr)
      return map[i].sym;
  return NULL;
}

static void dss_vreg_map_set(DssVregEntry *map, int *count, int32_t vr, Sym *sym)
{
  for (int i = 0; i < *count; i++)
  {
    if (map[i].vreg == vr)
    {
      map[i].sym = sym;
      return;
    }
  }
  if (*count < DSS_VREG_MAP_MAX)
  {
    map[*count].vreg = vr;
    map[*count].sym = sym;
    (*count)++;
  }
}

static void dss_vreg_map_clear(DssVregEntry *map, int *count, int32_t vr)
{
  for (int i = 0; i < *count; i++)
  {
    if (map[i].vreg == vr)
    {
      map[i].sym = NULL;
      return;
    }
  }
}

static void dss_build_vreg_map(TCCIRState *ir, DssVregEntry *map, int *count)
{
  const int n = ir->next_instruction_index;
  *count = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (dvr < 0 || dest.is_lval)
      continue;

    Sym *derived_sym = NULL;
    if (irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (s1.is_sym && !s1.is_lval)
      {
        IRPoolSymref *ref = irop_get_symref_ex(ir, s1);
        if (ref && ref->sym)
          derived_sym = ref->sym;
      }
    }
    if (!derived_sym && (q->op == TCCIR_OP_MLA))
    {
      IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (acc.is_sym && !acc.is_lval)
      {
        IRPoolSymref *ref = irop_get_symref_ex(ir, acc);
        if (ref && ref->sym)
          derived_sym = ref->sym;
      }
    }
    if (!derived_sym &&
        (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD ||
         q->op == TCCIR_OP_SUB))
    {
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        int32_t svr = irop_get_vreg(s1);
        if (svr >= 0 && !s1.is_sym)
          derived_sym = dss_vreg_map_lookup(map, *count, svr);
      }
    }
    if (!derived_sym && (q->op == TCCIR_OP_MLA))
    {
      IROperand acc = tcc_ir_op_get_accum(ir, q);
      int32_t avr = irop_get_vreg(acc);
      if (avr >= 0 && !acc.is_sym)
        derived_sym = dss_vreg_map_lookup(map, *count, avr);
    }

    if (derived_sym)
      dss_vreg_map_set(map, count, dvr, derived_sym);
    else
      dss_vreg_map_clear(map, count, dvr);
  }
}

static Sym *dss_resolve_store_dest_sym(TCCIRState *ir, IRQuadCompact *q,
                                       int store_idx,
                                       const DssVregEntry *vreg_map,
                                       int vreg_map_count)
{
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  if (dest.is_sym)
  {
    /* For STORE the dest must be the lval; STORE_INDEXED/POSTINC may have
     * had is_lval cleared on the base by disp_fusion. */
    if (!dest.is_lval && q->op == TCCIR_OP_STORE)
      return NULL;
    IRPoolSymref *ref = irop_get_symref_ex(ir, dest);
    return ref ? ref->sym : NULL;
  }

  /* Indirect TEMP-DEREF form for plain STORE. */
  if (q->op == TCCIR_OP_STORE && dest.is_lval)
  {
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return NULL;

    /* Find the single def of this TEMP.  Bail if multiply defined. */
    int def_idx = -1;
    int def_count = 0;
    for (int j = 0; j < ir->next_instruction_index; j++)
    {
      IRQuadCompact *dq = &ir->compact_instructions[j];
      if (dq->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[dq->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, dq);
      if (d.is_lval)
        continue;
      if (irop_get_vreg(d) == vr &&
          TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP)
      {
        def_idx = j;
        def_count++;
        if (def_count > 1)
          return NULL;
      }
    }
    if (def_idx < 0 || def_count != 1)
      return NULL;

    IRQuadCompact *dq = &ir->compact_instructions[def_idx];
    if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_LEA &&
        dq->op != TCCIR_OP_ASSIGN)
      return NULL;
    IROperand s1 = tcc_ir_op_get_src1(ir, dq);
    if (!s1.is_sym || s1.is_lval)
      return NULL;
    IRPoolSymref *ref = irop_get_symref_ex(ir, s1);
    return ref ? ref->sym : NULL;
  }

  /* STORE_INDEXED / STORE_POSTINC with temp dest: use the vreg map built
   * by forward tracing to find the originating static symbol. */
  if (vreg_map)
  {
    int32_t dvr = irop_get_vreg(dest);
    if (dvr >= 0)
      return dss_vreg_map_lookup(vreg_map, vreg_map_count, dvr);
  }

  return NULL;
}

int tcc_ir_opt_dead_static_store_elim(TCCIRState *ir)
{
  if (!ir || !tcc_state)
    return 0;
  /* Only fires during the end-of-TU re-optimization pass: tu_no_readers is
   * set only then, so running this pass during the first-pass compile would
   * always be a no-op anyway.  Gating keeps it cheap. */
  if (!tcc_state->ir_late_reopt_phase)
    return 0;

  const int n = ir->next_instruction_index;
  int changes = 0;

  /* Build vreg→sym map for resolving STORE_INDEXED/STORE_POSTINC through
   * temps.  After optimization, these ops often use temp vregs derived from
   * static SYMREFs rather than carrying the SYMREF directly. */
  DssVregEntry vreg_map[DSS_VREG_MAP_MAX];
  int vreg_map_count = 0;
  dss_build_vreg_map(ir, vreg_map, &vreg_map_count);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
        q->op != TCCIR_OP_STORE_POSTINC)
      continue;

    Sym *sym = dss_resolve_store_dest_sym(ir, q, i, vreg_map, vreg_map_count);
    if (!sym)
      continue;
    if (!sym->a.tu_no_readers)
      continue;
    if (sym->a.addrtaken)
      continue;
    /* Volatile stores must remain observable even if no C-level reader
     * exists (hardware register access). */
    if (sym->type.t & VT_VOLATILE)
      continue;

    LOG_IR_GEN("DEAD_STATIC_STORE: NOPed STORE at i=%d -> %s", i,
               get_tok_str(sym->v & ~SYM_FIELD, NULL));

    q->op = TCCIR_OP_NOP;
    changes++;
  }

  return changes;
}

int tcc_ir_opt_dead_static_store_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_static_store_elim(ctx->ir);
}

/* ============================================================================
 * Global Base Sharing (tcc_ir_opt_global_base_share)
 * ----------------------------------------------------------------------------
 * Detect clusters of consecutive STORE ops whose destination is a SYMREF-deref
 * to globals living in the same section (typically .bss/.data), with relative
 * offsets that fit in STR/STRD immediate encoding.  Replace the cluster with:
 *
 *   T_base = LEA &anchor                          (one LDR =anchor)
 *   STORE_INDEXED [T_base + delta_1], val_1       (STR/STRD val,[T_base,#d])
 *   STORE_INDEXED [T_base + delta_2], val_2
 *   ...
 *
 * This eliminates the per-store `LDR rN,[pc,#...]` that materializes each
 * symbol address separately.  The register allocator naturally keeps T_base
 * live across the cluster.
 *
 * Mirrors the behavior GCC uses to compact stores to adjacent globals into a
 * single base-register load plus offset stores.
 *
 * Conservative trigger conditions:
 *   - All cluster syms share the same writable, allocated section
 *   - st_shndx is a regular section (not SHN_UNDEF / SHN_COMMON / SHN_ABS)
 *   - No volatile / weak symbols
 *   - Deltas in [-1020,+1020] aligned to 4 (works for both STR and STRD T1)
 *   - Cluster contains >= 2 stores
 *   - No JUMP/JUMPIF/CALL/RETURN/IJUMP between cluster members
 *   - No jump-target instructions between cluster members
 * ============================================================================ */

#define GBS_MAX_CLUSTER 16
#define GBS_DELTA_MIN  (-1020)
#define GBS_DELTA_MAX  (1020)

/* Returns the symref pointer for a STORE/LOAD with a SYMREF-deref destination.
 * Also fills *out_esym.  Returns NULL on any mismatch.
 *
 * STORE codegen drives store width from `dest.btype` while STORE_INDEXED drives
 * it from `value.btype`.  Converting a STORE with mismatched dest/value btypes
 * (e.g. storing a u32 to a u16 global) would change the effective store width
 * and silently corrupt memory.  Reject such stores here. */
static IRPoolSymref *gbs_get_store_symref(TCCIRState *ir, IRQuadCompact *q, ElfSym **out_esym)
{
  if (q->op != TCCIR_OP_STORE)
    return NULL;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (irop_get_tag(dest) != IROP_TAG_SYMREF || !dest.is_lval)
    return NULL;
  /* Only handle INT32/INT64 (and matching float) stores.  Narrow stores
   * (INT8/INT16) interact subtly with subsequent SSA passes: dropping the
   * SYMREF-deref dest can cause downstream load forwarding to lose the
   * width-truncation effect on a write to a narrow global, leading to
   * stale loads that should have re-read truncated bytes.  Both dest and
   * src must match to avoid implicit narrowing within the STORE. */
  IROperand src = tcc_ir_op_get_src1(ir, q);
  int dest_bt = irop_get_btype(dest);
  int src_bt = irop_get_btype(src);
  if (dest_bt != src_bt)
    return NULL;
  if (dest_bt != IROP_BTYPE_INT32 && dest_bt != IROP_BTYPE_INT64 &&
      dest_bt != IROP_BTYPE_FLOAT32 && dest_bt != IROP_BTYPE_FLOAT64)
    return NULL;
  IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
  if (!sr || !sr->sym)
    return NULL;
  if (sr->addend != 0)
    return NULL;
  Sym *sym = sr->sym;
  if (sym->type.t & VT_VOLATILE)
    return NULL;
  /* Struct globals tend to mix access widths (bitfield read-modify-write
   * via multiple types overlapping the same bytes).  Skip these. */
  if ((sym->type.t & VT_BTYPE) == VT_STRUCT)
    return NULL;
  ElfSym *esym = elfsym(sym);
  if (!esym)
    return NULL;
  if (esym->st_shndx == SHN_UNDEF || esym->st_shndx >= (unsigned)tcc_state->nb_sections)
    return NULL;
  /* SHN_COMMON / SHN_ABS aren't real section indices */
  if (esym->st_shndx == SHN_COMMON || esym->st_shndx == SHN_ABS)
    return NULL;
  unsigned char bind = ELFW(ST_BIND)(esym->st_info);
  if (bind == STB_WEAK)
    return NULL;
  Section *sec = tcc_state->sections[esym->st_shndx];
  if (!sec || !(sec->sh_flags & SHF_ALLOC) || !(sec->sh_flags & SHF_WRITE))
    return NULL;
  *out_esym = esym;
  return sr;
}

int tcc_ir_opt_global_base_share(TCCIRState *ir)
{
  if (!ir || !tcc_state)
    return 0;
  if (!tcc_state->opt_indexed_memory)
    return 0;

  int n = ir->next_instruction_index;
  int changes = 0;

  /* If the function contains an IJUMP, computed-goto labels can target any
   * IR position via `orig_ir_to_code_mapping[s->jind]`.  Inserting a LEA
   * before a labeled STORE would change the mapping (the STORE's orig_index
   * still wins the mapping but the LEA's setup would be skipped on the jump
   * path).  Disable the pass for functions that use indirect jumps. */
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
      return 0;
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    /* Skip jump-target STOREs — joining the cluster would require the base
     * to be live across the merge, which we don't analyze. */
    if (q->is_jump_target)
      continue;

    ElfSym *anchor_esym = NULL;
    IRPoolSymref *anchor_sr = gbs_get_store_symref(ir, q, &anchor_esym);
    if (!anchor_sr)
      continue;

    int64_t anchor_base = (int64_t)anchor_esym->st_value + anchor_sr->addend;

    int cluster_stores[GBS_MAX_CLUSTER];
    int64_t cluster_deltas[GBS_MAX_CLUSTER];
    int cluster_size = 0;
    cluster_stores[cluster_size] = i;
    cluster_deltas[cluster_size] = 0;
    cluster_size++;

    int last_in_cluster = i;
    for (int j = i + 1; j < n && cluster_size < GBS_MAX_CLUSTER; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;

      /* Any control flow or call breaks the cluster — base reg may not
       * survive across these points without spill handling. */
      if (qj->op == TCCIR_OP_JUMP || qj->op == TCCIR_OP_JUMPIF ||
          qj->op == TCCIR_OP_IJUMP || qj->op == TCCIR_OP_SWITCH_TABLE ||
          qj->op == TCCIR_OP_SWITCH_LOAD ||
          qj->op == TCCIR_OP_FUNCCALLVOID || qj->op == TCCIR_OP_FUNCCALLVAL ||
          qj->op == TCCIR_OP_FUNCPARAMVAL || qj->op == TCCIR_OP_FUNCPARAMVOID ||
          qj->op == TCCIR_OP_RETURNVALUE || qj->op == TCCIR_OP_RETURNVOID ||
          qj->op == TCCIR_OP_INLINE_ASM)
        break;
      if (qj->is_jump_target)
        break;

      /* Non-STORE ops in between are fine — they don't disturb the base
       * reg (register allocator manages liveness). */
      if (qj->op != TCCIR_OP_STORE)
        continue;

      ElfSym *ej = NULL;
      IRPoolSymref *sj = gbs_get_store_symref(ir, qj, &ej);
      if (!sj)
        continue;
      if (ej->st_shndx != anchor_esym->st_shndx)
        continue;
      int64_t addr = (int64_t)ej->st_value + sj->addend;
      int64_t delta = addr - anchor_base;
      if (delta < GBS_DELTA_MIN || delta > GBS_DELTA_MAX)
        continue;
      if (delta & 3)
        continue; /* require 4-byte alignment for STRD compatibility */

      cluster_stores[cluster_size] = j;
      cluster_deltas[cluster_size] = delta;
      cluster_size++;
      last_in_cluster = j;
    }

    if (cluster_size < 2)
      continue;

    /* Allocate a new vreg for T_base. */
    int32_t base_vreg = tcc_ir_vreg_alloc_temp(ir);
    if (base_vreg < 0)
      continue;

    /* Build LEA src: SYMREF non-deref pointing at the anchor sym/addend. */
    uint32_t sym_pool = tcc_ir_pool_add_symref(ir, anchor_sr->sym,
                                               (int32_t)anchor_sr->addend, anchor_sr->flags);
    IROperand lea_src = irop_make_symref(-1, sym_pool, 0 /* is_lval */, 0 /* is_local */,
                                          0 /* is_const */, IROP_BTYPE_INT32);
    IROperand lea_dest = irop_make_vreg(base_vreg, IROP_BTYPE_INT32);
    IROperand null_op = {0};

    int inserted = insert_instr_at(ir, i, TCCIR_OP_LEA, lea_dest, lea_src, null_op);
    if (inserted < 0)
      continue;
    /* All cluster indices have shifted by +1 due to insertion. */
    for (int k = 0; k < cluster_size; k++)
      cluster_stores[k] += 1;
    n = ir->next_instruction_index;
    last_in_cluster += 1;

    /* Rewrite each STORE to STORE_INDEXED with base + delta addressing. */
    for (int k = 0; k < cluster_size; k++)
    {
      int sidx = cluster_stores[k];
      IRQuadCompact *sq = &ir->compact_instructions[sidx];
      IROperand st_src = tcc_ir_op_get_src1(ir, sq);
      int64_t delta = cluster_deltas[k];

      tcc_ir_pool_ensure(ir, 4);
      int new_base = ir->iroperand_pool_count;

      IROperand st_base = irop_make_vreg(base_vreg, IROP_BTYPE_INT32);
      st_base.is_lval = 0;
      tcc_ir_pool_add(ir, st_base);
      tcc_ir_pool_add(ir, st_src);
      tcc_ir_pool_add(ir, irop_make_imm32(-1, (int32_t)delta, IROP_BTYPE_INT32));
      tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));

      sq->op = TCCIR_OP_STORE_INDEXED;
      sq->operand_base = new_base;
    }

    LOG_IR_GEN("GLOBAL_BASE_SHARE: cluster of %d stores starting at i=%d (after LEA insertion at i=%d)",
               cluster_size, i + 1, i);

    changes++;
    /* Skip past the cluster; outer loop will advance past last_in_cluster. */
    i = last_in_cluster;
  }

  return changes;
}

int tcc_ir_opt_global_base_share_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_global_base_share(ctx->ir);
}

/* ============================================================================
 * Read-Modify-Write Byte Clear (tcc_ir_opt_rmw_byte_clear)
 * ============================================================================
 *
 * Detects a load–AND–store pattern that clears an aligned byte within a word
 * and replaces it with a single byte store of zero.
 *
 * Pattern:
 *   [ADD  T_addr = T_base, #offset]          (optional)
 *   AND  T_val  = T_addr***DEREF*** AND #mask (mask clears one byte)
 *   STORE T_addr***DEREF*** = T_val
 *
 * When mask == 0xFFFFFF00 (clears byte 0):
 *   With ADD:    → STORE_INDEXED T_base, #0(INT8), #offset  (+ NOP ADD, AND)
 *   Without ADD: → STORE T_addr***DEREF*** = #0 (INT8)      (+ NOP AND)
 *
 * Fires on bitfield clears like `s->rLogin = s->lock = s->goodExit = 0;`
 * where the front-end generates word-wide read-modify-write but all bits
 * within a byte are zeroed, making a byte store semantically equivalent.
 */
int tcc_ir_opt_rmw_byte_clear(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_AND)
      continue;

    IROperand and_dest = tcc_ir_op_get_dest(ir, q);
    IROperand and_src1 = tcc_ir_op_get_src1(ir, q);
    IROperand and_src2 = tcc_ir_op_get_src2(ir, q);

    if (!irop_op_is_lval(and_src1))
      continue;
    if (!irop_is_immediate(and_src2))
      continue;

    uint32_t mask = (uint32_t)irop_get_imm32(and_src2);
    if (mask != 0xFFFFFF00u)
      continue;

    int32_t and_dest_vr = irop_get_vreg(and_dest);
    int32_t addr_vr = irop_get_vreg(and_src1);
    if (and_dest_vr < 0 || addr_vr < 0)
      continue;

    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      continue;
    if (!ir_xform_same_block(ir, i, j))
      continue;

    IRQuadCompact *sq = &ir->compact_instructions[j];
    if (sq->op != TCCIR_OP_STORE)
      continue;

    IROperand store_dest = tcc_ir_op_get_dest(ir, sq);
    IROperand store_src = tcc_ir_op_get_src1(ir, sq);

    if (irop_get_vreg(store_src) != and_dest_vr)
      continue;
    if (!irop_op_is_lval(store_dest))
      continue;
    if (irop_get_vreg(store_dest) != addr_vr)
      continue;

    if (!tcc_ir_vreg_has_single_use(ir, and_dest_vr, -1))
      continue;

    int add_idx = -1;
    IROperand add_base_op = IROP_NONE;
    int32_t offset = 0;
    int can_fold_add = 0;

    for (int k = i - 1; k >= 0; k--) {
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[kq->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, kq);
      if (!irop_has_vreg(d) || irop_get_vreg(d) != addr_vr)
        continue;
      if (kq->op == TCCIR_OP_ADD) {
        IROperand s1 = tcc_ir_op_get_src1(ir, kq);
        IROperand s2 = tcc_ir_op_get_src2(ir, kq);
        if (irop_is_immediate(s2) && irop_has_vreg(s1)) {
          int32_t imm = irop_get_imm32(s2);
          if (imm >= -255 && imm <= 4095) {
            add_idx = k;
            add_base_op = s1;
            offset = imm;
          }
        }
      }
      break;
    }

    if (add_idx >= 0) {
      int extra = 0;
      for (int k = 0; k < n && !extra; k++) {
        if (k == i || k == j)
          continue;
        IRQuadCompact *kq = &ir->compact_instructions[k];
        if (kq->op == TCCIR_OP_NOP)
          continue;
        int is_store_op = (kq->op == TCCIR_OP_STORE || kq->op == TCCIR_OP_STORE_INDEXED ||
                           kq->op == TCCIR_OP_STORE_POSTINC);
        if (irop_config[kq->op].has_dest) {
          IROperand d = tcc_ir_op_get_dest(ir, kq);
          if (irop_has_vreg(d) && irop_get_vreg(d) == addr_vr && is_store_op) {
            extra = 1;
            break;
          }
        }
        if (irop_config[kq->op].has_src1) {
          IROperand s1 = tcc_ir_op_get_src1(ir, kq);
          if (irop_has_vreg(s1) && irop_get_vreg(s1) == addr_vr) {
            extra = 1;
            break;
          }
        }
        if (irop_config[kq->op].has_src2) {
          IROperand s2 = tcc_ir_op_get_src2(ir, kq);
          if (irop_has_vreg(s2) && irop_get_vreg(s2) == addr_vr) {
            extra = 1;
            break;
          }
        }
      }
      if (!extra && ir_xform_same_block(ir, add_idx, j))
        can_fold_add = 1;
    }

    if (can_fold_add) {
      tcc_ir_pool_ensure(ir, 4);
      int new_base = ir->iroperand_pool_count;
      if (new_base + 4 > ir->iroperand_pool_capacity)
        continue;

      IROperand base_op = add_base_op;
      base_op.is_lval = 0;
      IROperand value_op = irop_make_imm32(-1, 0, IROP_BTYPE_INT8);
      IROperand index_op = irop_make_imm32(0, offset, IROP_BTYPE_INT32);
      IROperand scale_op = irop_make_imm32(0, 0, IROP_BTYPE_INT32);

      tcc_ir_pool_add(ir, base_op);
      tcc_ir_pool_add(ir, value_op);
      tcc_ir_pool_add(ir, index_op);
      tcc_ir_pool_add(ir, scale_op);

      sq->op = TCCIR_OP_STORE_INDEXED;
      sq->operand_base = new_base;

      ir_xform_nop(ir, i);
      ir_xform_nop(ir, add_idx);
    } else {
      ir_xform_nop(ir, i);

      IROperand new_src = irop_make_imm32(-1, 0, IROP_BTYPE_INT8);
      tcc_ir_op_set_src1(ir, sq, new_src);

      IROperand new_dest = store_dest;
      new_dest.btype = IROP_BTYPE_INT8;
      tcc_ir_op_set_dest(ir, sq, new_dest);
    }

    changes++;
  }

  return changes;
}

/* ============================================================================
 * Byte Store Merge (tcc_ir_opt_byte_store_merge)
 * ============================================================================
 *
 * Merges groups of 4 consecutive constant INT8 STORE/STORE_INDEXED operations
 * to the same base address at word-aligned boundaries into a single INT32
 * store.  This enables the redundant-store pass to kill wider stores that
 * were previously invisible because byte stores are narrower.
 *
 * Pattern:
 *   STORE     base***DEREF*** = #C0  (INT8)   // byte at aligned offset N
 *   STORE_INDEXED base = #C1 at N+1  (INT8)
 *   STORE_INDEXED base = #C2 at N+2  (INT8)
 *   STORE_INDEXED base = #C3 at N+3  (INT8)
 *
 * Becomes:
 *   STORE     base***DEREF*** = #(C0|C1<<8|C2<<16|C3<<24)  (INT32)
 *   NOP
 *   NOP
 *   NOP
 */
int tcc_ir_opt_byte_store_merge(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  const Sym *grp_sym = NULL;
  int64_t grp_base = 0;
  int grp_count = 0;
  int grp_indices[4];
  int32_t grp_values[4];

  for (int i = 0; i <= n; i++)
  {
    int is_byte_store = 0;
    const Sym *cur_sym = NULL;
    int64_t cur_off = 0;
    int32_t cur_val = 0;

    if (i < n)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];

      if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_ASSIGN)
        continue;

      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int store_btype;
        if (q->op == TCCIR_OP_STORE_INDEXED)
          store_btype = irop_get_btype(src1);
        else
          store_btype = irop_get_btype(tcc_ir_op_get_dest(ir, q));

        if (store_btype == IROP_BTYPE_INT8 && irop_is_immediate(src1) &&
            rse_resolve_store_addr(ir, q, &cur_sym, &cur_off))
        {
          cur_val = (int32_t)irop_get_imm64_ex(ir, src1) & 0xFF;
          is_byte_store = 1;
        }
      }
    }

    if (is_byte_store)
    {
      int64_t aligned_base = cur_off & ~3LL;
      int byte_pos = (int)(cur_off & 3);

      if (grp_count > 0 && cur_sym == grp_sym && aligned_base == grp_base && byte_pos == grp_count)
      {
        grp_values[grp_count] = cur_val;
        grp_indices[grp_count] = i;
        grp_count++;
      }
      else
      {
        grp_count = 0;
        if (byte_pos == 0)
        {
          grp_sym = cur_sym;
          grp_base = aligned_base;
          grp_values[0] = cur_val;
          grp_indices[0] = i;
          grp_count = 1;
        }
      }

      if (grp_count == 4)
      {
        int32_t merged = grp_values[0] | (grp_values[1] << 8) | (grp_values[2] << 16) | (grp_values[3] << 24);

        IRQuadCompact *fq = &ir->compact_instructions[grp_indices[0]];
        if (fq->op == TCCIR_OP_STORE)
        {
          IROperand dest = tcc_ir_op_get_dest(ir, fq);
          dest.btype = IROP_BTYPE_INT32;
          tcc_ir_op_set_dest(ir, fq, dest);
        }
        IROperand new_src1 = irop_make_imm32(-1, merged, IROP_BTYPE_INT32);
        tcc_ir_op_set_src1(ir, fq, new_src1);

        for (int k = 1; k < 4; k++)
          ir->compact_instructions[grp_indices[k]].op = TCCIR_OP_NOP;

        changes++;
        grp_count = 0;
      }
    }
    else
    {
      grp_count = 0;
    }
  }

  return changes;
}

int tcc_ir_opt_byte_store_merge_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_byte_store_merge(ctx->ir);
}

/* ============================================================================
 * Constant-buffer memcpy/memmove forwarding (tcc_ir_opt_const_memcpy_to_dest)
 * ============================================================================
 *
 * Targets the GCC vector_size / aggregate-constant store idiom, e.g.
 * `*x |= *x ^ {-1,...}` (pr60502) which the optimizer folds to a constant
 * 16-byte all-0xFF value but then lowers as:
 *
 *     T <- &?buf                     // fresh non-escaping local result temp
 *     *(T+0..15) <- #-1   (16 byte stores)
 *     __aeabi_memmove8(x, &?buf, 16) // copy the constant buffer to *x
 *
 * GCC instead stores the constant straight to *x with wide stores.  When the
 * source of an *alignment-guaranteeing* AEABI mem* helper is a non-escaping
 * stack buffer filled exclusively with compile-time constants, the copy is
 * value-equivalent to storing those constants directly to the destination —
 * and crucially there is NO aliasing hazard (the source bytes are constants,
 * independent of the destination), which is what makes this safe where a
 * general "store direct instead of memmove" is not.  We rewrite the byte fill
 * into wide constant STORE_INDEXED ops to the destination pointer (which the
 * codegen STRD-imm peephole then pairs into `strd`), and drop the buffer +
 * the memmove.
 *
 * Alignment: only the `__aeabi_mem{cpy,move}{4,8}` variants are accepted.
 * Those imply the destination is 4-/8-byte aligned, so the word stores (and
 * the STRD they fold into) are safe.  Plain `memcpy`/`memmove` carry no
 * alignment guarantee and are rejected.
 *
 * Soundness gates (each conservative; the pass bails the whole function on
 * anything it can't fully reason about):
 *   - No untracked-memory opcodes (IJUMP/SETJMP/.../VLA_ALLOC), no nested fn.
 *   - The source buffer is a stack-local (NULL-sym) slot; its copied range
 *     [off_b, off_b+S) is treated as the buffer extent (distinct locals get
 *     distinct frame ranges, so an access overlapping the range is genuinely
 *     this buffer, never an adjacent object).
 *   - Every store overlapping the range before the call has a constant value
 *     and together they fully cover [0,S) (program-order, last-write-wins).
 *   - The buffer is referenced ONLY by those fills, the address-propagation
 *     temps feeding them, and this one memmove's source param — no other read,
 *     no escape, no other call (a full isolation scan proves non-escape, which
 *     in turn lets us ignore unresolved stores that cannot alias it).
 *   - The fills and the call sit in one straight-line region (no branch in or
 *     out between the first fill and the call) so coverage is control-flow
 *     sound and the new stores stay at the original copy point.
 *   - The destination operand is a register-class pointer value (VREG).
 */

#define CMD_MAX_BYTES 64 /* cap on copy size we expand inline */

/* Resolve a non-lval operand that holds an address into (sym, byte_off).
 * Handles direct `Addr[StackLoc[off]]` and a single-def TEMP holding such an
 * address (incl. +imm chains, via rse_resolve_temp_addr). */
static int cmd_op_addr(TCCIRState *ir, IROperand op, const Sym **sym, int64_t *off)
{
  if (op.is_lval)
    return 0;
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_llocal &&
      irop_get_vreg(op) == -1)
  {
    *sym = NULL;
    *off = irop_get_stack_offset(op);
    return 1;
  }
  int32_t vr = irop_get_vreg(op);
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    return rse_resolve_temp_addr(ir, vr, sym, off);
  return 0;
}

/* Resolve an lval operand (a memory access) to (sym, byte_off). */
static int cmd_op_lval(TCCIRState *ir, IROperand op, const Sym **sym, int64_t *off)
{
  if (!op.is_lval)
    return 0;
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_llocal &&
      irop_get_vreg(op) == -1)
  {
    *sym = NULL;
    *off = irop_get_stack_offset(op);
    return 1;
  }
  int32_t vr = irop_get_vreg(op);
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    return rse_resolve_temp_addr(ir, vr, sym, off);
  return 0;
}

/* Resolve a STORE / STORE_INDEXED destination address to (sym, byte_off) and
 * its width.  Handles the temp-deref / indexed forms (via the operand
 * resolvers, which cover direct `Addr[StackLoc]` temps too) AND the direct
 * `StackLoc[off]` lval form that rse_resolve_store_addr rejects (vreg == -1). */
static int cmd_store_addr(TCCIRState *ir, IRQuadCompact *q, const Sym **sym,
                          int64_t *off, int *width)
{
  if (q->op == TCCIR_OP_STORE)
  {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!cmd_op_lval(ir, d, sym, off))
      return 0;
    int w = ir_opt_store_btype_size_bytes(irop_get_btype(d));
    *width = w > 0 ? w : 4;
    return 1;
  }
  if (q->op == TCCIR_OP_STORE_INDEXED)
  {
    IROperand base = tcc_ir_op_get_dest(ir, q);
    if (!cmd_op_addr(ir, base, sym, off))
      return 0;
    IROperand idx = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(idx))
      return 0;
    IROperand sc = tcc_ir_op_get_scale(ir, q);
    int64_t scale = irop_is_immediate(sc) ? irop_get_imm64_ex(ir, sc) : 0;
    *off += irop_get_imm64_ex(ir, idx) << scale;
    int w = ir_opt_store_btype_size_bytes(irop_get_btype(tcc_ir_op_get_src1(ir, q)));
    *width = w > 0 ? w : 4;
    return 1;
  }
  return 0;
}

/* Constant value of a fill source: an immediate, or a single-def TEMP whose
 * def is `ASSIGN T <- #imm`. */
static int cmd_const_value(TCCIRState *ir, IROperand val, int64_t *out)
{
  if (irop_is_immediate(val))
  {
    *out = irop_get_imm64_ex(ir, val);
    return 1;
  }
  if (val.is_lval)
    return 0;
  int32_t vr = irop_get_vreg(val);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (!rse_def_map || pos >= rse_def_map_size)
    return 0;
  int d = rse_def_map[pos];
  if (d < 0)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[d];
  if (dq->op != TCCIR_OP_ASSIGN)
    return 0;
  IROperand s1 = tcc_ir_op_get_src1(ir, dq);
  if (!irop_is_immediate(s1))
    return 0;
  *out = irop_get_imm64_ex(ir, s1);
  return 1;
}

static int cmd_is_aligned_memcpy(TCCIRState *ir, IRQuadCompact *q)
{
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!callee)
    return 0;
  const char *name = get_tok_str(callee->v, NULL);
  if (!name)
    return 0;
  return strcmp(name, "__aeabi_memcpy4") == 0 || strcmp(name, "__aeabi_memcpy8") == 0 ||
         strcmp(name, "__aeabi_memmove4") == 0 || strcmp(name, "__aeabi_memmove8") == 0;
}

/* Try to rewrite one memmove/memcpy call at index ci.  Returns 1 on success. */
static int cmd_try_one(TCCIRState *ir, int ci)
{
  int n = ir->next_instruction_index;
  IRQuadCompact *call = &ir->compact_instructions[ci];

  if (!cmd_is_aligned_memcpy(ir, call))
    return 0;

  int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call)));

  /* The return value (memmove returns dest) must be dead. */
  if (call->op == TCCIR_OP_FUNCCALLVAL)
  {
    IROperand cd = tcc_ir_op_get_dest(ir, call);
    int32_t cdv = irop_get_vreg(cd);
    if (cdv >= 0)
    {
      for (int j = 0; j < n; j++)
      {
        if (j == ci)
          continue;
        IRQuadCompact *q = &ir->compact_instructions[j];
        if (q->op == TCCIR_OP_NOP)
          continue;
        for (int k = 1; k <= 3; k++)
        {
          IROperand op = (k == 1) ? tcc_ir_op_get_src1(ir, q)
                         : (k == 2) ? tcc_ir_op_get_src2(ir, q)
                                    : tcc_ir_op_get_accum(ir, q);
          if (irop_get_vreg(op) == cdv)
            return 0;
        }
      }
    }
  }

  /* Locate the three params. */
  int p0 = -1, p1 = -1, p2 = -1;
  for (int j = 0; j < ci; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;
    uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
    if (TCCIR_DECODE_CALL_ID(enc) != call_id)
      continue;
    int pidx = TCCIR_DECODE_PARAM_IDX(enc);
    if (pidx == 0) p0 = j;
    else if (pidx == 1) p1 = j;
    else if (pidx == 2) p2 = j;
  }
  if (p0 < 0 || p1 < 0 || p2 < 0)
    return 0;

  /* Size (param2) must be a small positive immediate. */
  IROperand sz_op = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p2]);
  if (!irop_is_immediate(sz_op))
    return 0;
  int64_t S = irop_get_imm64_ex(ir, sz_op);
  if (S <= 0 || S > CMD_MAX_BYTES)
    return 0;

  /* Destination (param0) must be a register-class pointer value. */
  IROperand D = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p0]);
  if (D.is_lval || irop_get_tag(D) != IROP_TAG_VREG)
    return 0;
  {
    int32_t dvr = irop_get_vreg(D);
    if (dvr < 0)
      return 0;
    int dty = TCCIR_DECODE_VREG_TYPE(dvr);
    if (dty != TCCIR_VREG_TYPE_PARAM && dty != TCCIR_VREG_TYPE_TEMP)
      return 0;
  }

  /* Source (param1) must resolve to a stack-local buffer start. */
  IROperand srcop = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p1]);
  const Sym *sym_b = NULL;
  int64_t off_b = 0;
  if (!cmd_op_addr(ir, srcop, &sym_b, &off_b))
    return 0;
  if (sym_b != NULL)
    return 0; /* stack-local only */

#define CMD_OVL(s, o, w) ((s) == sym_b && (int64_t)(o) < off_b + S && off_b < (int64_t)(o) + ((w) > 0 ? (w) : 1))

  /* --- Phase 1: collect const fills, build the byte image. --- */
  unsigned char image[CMD_MAX_BYTES];
  unsigned char defined[CMD_MAX_BYTES];
  memset(defined, 0, (size_t)S);
  int n_fills = 0;
  int fill_lo = ci;
  for (int i = 0; i < ci; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED)
      continue;
    const Sym *s;
    int64_t o;
    int w;
    if (!cmd_store_addr(ir, q, &s, &o, &w))
      continue;
    if (!CMD_OVL(s, o, w))
      continue;
    /* a store overlapping the buffer range: must be a constant fill */
    int64_t val;
    if (!cmd_const_value(ir, tcc_ir_op_get_src1(ir, q), &val))
      return 0;
    for (int b = 0; b < w; b++)
    {
      int64_t rel = o + b - off_b;
      if (rel >= 0 && rel < S)
      {
        image[rel] = (unsigned char)((uint64_t)val >> (8 * b));
        defined[rel] = 1;
      }
    }
    n_fills++;
    if (i < fill_lo) fill_lo = i;
  }
  if (n_fills == 0)
    return 0;
  for (int b = 0; b < S; b++)
    if (!defined[b])
      return 0; /* incomplete coverage */

  /* --- Phase 2: region gate — straight-line [fill_lo, ci] with no foreign
   * store.  No branch in/out (coverage is control-flow sound; the rewritten
   * stores stay at the original copy point) and no store other than a buffer
   * fill (so placing the new dest stores anywhere in the region cannot reorder
   * a write to the destination's memory). --- */
  for (int i = fill_lo; i <= ci; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (i > fill_lo && q->is_jump_target)
      return 0;
    switch (q->op)
    {
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      return 0;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    {
      const Sym *s;
      int64_t o;
      int w;
      if (!cmd_store_addr(ir, q, &s, &o, &w) || !CMD_OVL(s, o, w))
        return 0; /* a non-fill store inside the region */
      break;
    }
    default:
      break;
    }
  }

  /* D must be available throughout the region. PARAM: always. TEMP: its single
   * def must precede the first fill. */
  {
    int32_t dvr = irop_get_vreg(D);
    if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      int dp = TCCIR_DECODE_VREG_POSITION(dvr);
      if (!rse_def_map || dp >= rse_def_map_size)
        return 0;
      int dd = rse_def_map[dp];
      if (dd < 0 || dd >= fill_lo)
        return 0;
    }
  }

  /* --- Phase 3: isolation scan over the whole function. --- */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || i == ci)
      continue;

    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
      return 0; /* no other calls */

    if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
    {
      uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (TCCIR_DECODE_CALL_ID(enc) != call_id)
        return 0; /* param of another (gone) call */
      /* our memmove's params: param1 is the buffer source (intended); param0/2
       * are pointer/size, neither references the buffer interior. */
      continue;
    }

    const Sym *s;
    int64_t o;

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
    {
      int w;
      if (cmd_store_addr(ir, q, &s, &o, &w) && CMD_OVL(s, o, w))
      {
        /* a store into the buffer that is not a pre-call collected fill */
        if (i >= ci)
          return 0;
        /* collected fills were verified const above; nothing else to do */
        continue;
      }
      /* variable-indexed store whose base is the buffer? */
      if (q->op == TCCIR_OP_STORE_INDEXED &&
          cmd_op_addr(ir, tcc_ir_op_get_dest(ir, q), &s, &o) && CMD_OVL(s, o, 1))
        return 0;
      /* the stored value must not be the buffer address (escape) */
      if (cmd_op_addr(ir, tcc_ir_op_get_src1(ir, q), &s, &o) && CMD_OVL(s, o, 1))
        return 0;
      continue;
    }

    if (q->op == TCCIR_OP_LOAD)
    {
      if (cmd_op_lval(ir, tcc_ir_op_get_src1(ir, q), &s, &o) && CMD_OVL(s, o, 1))
        return 0; /* read of the buffer */
      continue;
    }
    if (q->op == TCCIR_OP_LOAD_INDEXED)
    {
      if (cmd_op_addr(ir, tcc_ir_op_get_src1(ir, q), &s, &o) && CMD_OVL(s, o, 1))
        return 0;
      continue;
    }

    /* generic op: no operand may touch the buffer except as a tame
     * address-propagation / compare. */
    for (int k = 0; k < 4; k++)
    {
      int has = (k == 0) ? irop_config[q->op].has_dest
                : (k == 1) ? irop_config[q->op].has_src1
                : (k == 2) ? irop_config[q->op].has_src2
                           : (q->op == TCCIR_OP_MLA);
      if (!has)
        continue;
      IROperand op = (k == 0) ? tcc_ir_op_get_dest(ir, q)
                     : (k == 1) ? tcc_ir_op_get_src1(ir, q)
                     : (k == 2) ? tcc_ir_op_get_src2(ir, q)
                                : tcc_ir_op_get_accum(ir, q);
      if (op.is_lval)
      {
        if (cmd_op_lval(ir, op, &s, &o) && CMD_OVL(s, o, 1))
          return 0;
        continue;
      }
      if (k == 0)
        continue; /* a def is not a use */
      if (cmd_op_addr(ir, op, &s, &o) && CMD_OVL(s, o, 1))
      {
        if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA &&
            q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB && q->op != TCCIR_OP_CMP)
          return 0;
      }
    }
  }

  /* --- Phase 4: build wide store descriptors from the image. --- */
  struct { int rel; int w; uint32_t val; } desc[CMD_MAX_BYTES];
  int n_desc = 0;
  for (int rel = 0; rel < S;)
  {
    int rem = (int)S - rel;
    int w = rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
    uint32_t v = 0;
    for (int b = 0; b < w; b++)
      v |= (uint32_t)image[rel + b] << (8 * b);
    desc[n_desc].rel = rel;
    desc[n_desc].w = w;
    desc[n_desc].val = v;
    n_desc++;
    rel += w;
  }

  /* Available slots: the collected fills (idx < ci) + the 3 params + the call,
   * in ascending index order. We place descriptors into the LAST n_desc of
   * them so the new stores stay at the original copy point (preserving order
   * vs any unrelated store in the region). */
  int slots[CMD_MAX_BYTES + 4];
  int n_slots = 0;
  for (int i = fill_lo; i <= ci && n_slots < (int)(sizeof(slots) / sizeof(slots[0])); i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int is_fill = 0;
    if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED))
    {
      const Sym *s;
      int64_t o;
      int w;
      if (cmd_store_addr(ir, q, &s, &o, &w) && CMD_OVL(s, o, w))
        is_fill = 1;
    }
    if (is_fill || i == p0 || i == p1 || i == p2 || i == ci)
      slots[n_slots++] = i;
  }
  if (n_slots < n_desc)
    return 0;

  /* Emit the descriptors into the chosen (last n_desc) slots; NOP the rest. */
  IROperand base_op = D;
  base_op.is_lval = 0;
  for (int si = 0; si < n_slots; si++)
  {
    int idx = slots[si];
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (si < n_slots - n_desc)
    {
      q->op = TCCIR_OP_NOP;
      continue;
    }
    int di = si - (n_slots - n_desc);
    int btype = desc[di].w == 4 ? IROP_BTYPE_INT32 : desc[di].w == 2 ? IROP_BTYPE_INT16 : IROP_BTYPE_INT8;
    tcc_ir_pool_ensure(ir, 4);
    int pb = ir->iroperand_pool_count;
    tcc_ir_pool_add(ir, base_op);
    tcc_ir_pool_add(ir, irop_make_imm32(-1, (int32_t)desc[di].val, btype));
    tcc_ir_pool_add(ir, irop_make_imm32(-1, desc[di].rel, IROP_BTYPE_INT32));
    tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
    q->op = TCCIR_OP_STORE_INDEXED;
    q->operand_base = pb;
  }

  /* --- Phase 5: targeted dead-code cleanup. --- */
  /* The buffer-address LEAs and the value-feeder ASSIGNs are now dead. NOP any
   * pure ASSIGN/LEA/ADD whose temp dest has no remaining use. */
  for (int pass = 0; pass < 3; pass++)
  {
    int removed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ADD)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (d.is_lval)
        continue;
      int32_t dv = irop_get_vreg(d);
      if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int used = 0;
      for (int j = 0; j < n && !used; j++)
      {
        if (j == i)
          continue;
        IRQuadCompact *qj = &ir->compact_instructions[j];
        if (qj->op == TCCIR_OP_NOP)
          continue;
        for (int k = 1; k <= 3 && !used; k++)
        {
          IROperand op = (k == 1) ? tcc_ir_op_get_src1(ir, qj)
                         : (k == 2) ? tcc_ir_op_get_src2(ir, qj)
                                    : tcc_ir_op_get_accum(ir, qj);
          if (irop_get_vreg(op) == dv)
            used = 1;
        }
      }
      if (!used)
      {
        q->op = TCCIR_OP_NOP;
        removed = 1;
      }
    }
    if (!removed)
      break;
  }

#undef CMD_OVL
  return 1;
}

int tcc_ir_opt_const_memcpy_to_dest(TCCIRState *ir)
{
  static int disabled = -1;
  if (disabled < 0)
    disabled = getenv("TCC_NO_CONST_MEMCPY") != NULL;
  if (disabled)
    return 0;

  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;
  if (ir->captured_count > 0 || ir->has_static_chain)
    return 0;
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_VLA_ALLOC ||
        op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  int changes = 0;
  rse_build_def_map(ir);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
      continue;
    if (cmd_try_one(ir, i))
    {
      changes++;
      rse_build_def_map(ir); /* indices stable (only in-place NOP/reconfig) */
    }
  }
  rse_free_def_map();
  return changes;
}

int tcc_ir_opt_const_memcpy_to_dest_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_const_memcpy_to_dest(ctx->ir);
}

/* ============================================================================
 * Local Copy Propagation (tcc_ir_opt_local_copy_prop)
 * ============================================================================
 *
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

      /* Skip copy chain instructions */
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

      /* Check src2 for reads */
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

      /* Redirect STORE/STORE_INDEXED dest from source to dest range */
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

    /* NOP the copy chain and any extra pre-forwarded pairs */
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

/* ============================================================================
 * Struct-copy round-trip elimination (tcc_ir_opt_struct_copy_roundtrip_elim)
 * ============================================================================
 *
 * Inlining a by-value identity helper — `struct S retme(struct S x){return x;}`
 * called as `y = retme(y)` — lowers to a pair of struct copies through a fresh
 * temporary slot B (the inlined parameter/return home):
 *
 *     memmove(B, A, N)     ; B := y        (marshal the argument)
 *     memmove(A, B, N)     ; y := result   (copy the return value back)
 *
 * The net effect on A is nothing (A is copied out to B and immediately copied
 * back), and B is a dead temp afterwards.  Both copies are removable when:
 *
 *   - the two calls are adjacent, with no memory-writing op and no other call
 *     between them (so A's region is provably unmodified across the pair); and
 *   - region B [b,b+N) is referenced *only* as C1's destination and C2's
 *     source — i.e. B is a private round-trip buffer, never read elsewhere and
 *     never the destination of any other write.
 *
 * Removing the pair leaves the field load/store of `y.field += x` (which sat
 * between the init copy and the round-trip) directly followed by the return's
 * field load, so the existing sl_forward + bf_insert_extract cascade collapses
 * the bitfield poke/re-extract — which is why this runs just before the memory
 * group.  Targets the 20040709-2 fn1* bitfield idioms (memmove-marshalled
 * struct-by-value through retme).
 */

/* Resolve a call-param operand to a stack-slot address.  The operand is either
 * a direct `Addr[StackLoc[off]]` (is_lval=0) or a TEMP whose single prior def
 * is `T <- Addr[StackLoc[off]]` (LEA / ASSIGN-with-no-src2).  Returns 1 and
 * fills *off / *is_local on success. */
static int scre_resolve_slot_addr(TCCIRState *ir, IROperand op, int before_idx, int32_t *off, int *is_local)
{
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && !op.is_lval)
  {
    *off = irop_get_stack_offset(op);
    *is_local = op.is_local;
    return 1;
  }
  if (!irop_has_vreg(op))
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  for (int d = before_idx - 1; d >= 0; d--)
  {
    IRQuadCompact *dq = &ir->compact_instructions[d];
    if (dq->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[dq->op].has_dest)
      continue;
    IROperand dd = tcc_ir_op_get_dest(ir, dq);
    if (!irop_has_vreg(dd) || irop_get_vreg(dd) != vr)
      continue;
    /* Found the (latest) def of the param vreg. */
    if (dq->op != TCCIR_OP_LEA && dq->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
    if (dq->op == TCCIR_OP_ASSIGN && !irop_is_none(tcc_ir_op_get_src2(ir, dq)))
      return 0;
    if (irop_get_tag(ds1) != IROP_TAG_STACKOFF || ds1.is_lval)
      return 0;
    *off = irop_get_stack_offset(ds1);
    *is_local = ds1.is_local;
    return 1;
  }
  return 0;
}

static int scre_is_memcpy_like(TCCIRState *ir, IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
    return 0;
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!callee)
    return 0;
  const char *name = get_tok_str(callee->v, NULL);
  if (!name)
    return 0;
  return strcmp(name, "memmove") == 0 || strcmp(name, "memcpy") == 0 ||
         strcmp(name, "__tcc_memmove") == 0 ||
         strcmp(name, "__aeabi_memmove") == 0 || strcmp(name, "__aeabi_memmove4") == 0 ||
         strcmp(name, "__aeabi_memmove8") == 0 || strcmp(name, "__aeabi_memcpy") == 0 ||
         strcmp(name, "__aeabi_memcpy4") == 0 || strcmp(name, "__aeabi_memcpy8") == 0;
}

/* dst (param0), src (param1), size (param2) as stack slots / constant. */
static int scre_get_copy(TCCIRState *ir, int call_idx, int32_t *dst, int32_t *src, int32_t *size)
{
  IROperand p0, p1, p2;
  int dl, sl;
  if (!ir_opt_get_call_param_operand(ir, call_idx, 0, &p0) ||
      !ir_opt_get_call_param_operand(ir, call_idx, 1, &p1) ||
      !ir_opt_get_call_param_operand(ir, call_idx, 2, &p2))
    return 0;
  if (irop_get_tag(p2) != IROP_TAG_IMM32)
    return 0;
  if (!scre_resolve_slot_addr(ir, p0, call_idx, dst, &dl) || !dl)
    return 0;
  if (!scre_resolve_slot_addr(ir, p1, call_idx, src, &sl) || !sl)
    return 0;
  *size = (int32_t)p2.u.imm32;
  return *size > 0;
}

/* Does instruction q reference (read/write/addr-of) any byte of [lo,lo+sz)? */
static int scre_touches_region(TCCIRState *ir, IRQuadCompact *q, int32_t lo, int32_t sz, int *is_write)
{
  *is_write = 0;
  const IRRegistersConfig *cfg = &irop_config[q->op];
  int touched = 0;
  IROperand ops[3];
  int which[3];
  int nops = 0;
  if (cfg->has_dest) { ops[nops] = tcc_ir_op_get_dest(ir, q); which[nops] = 0; nops++; }
  if (cfg->has_src1) { ops[nops] = tcc_ir_op_get_src1(ir, q); which[nops] = 1; nops++; }
  if (cfg->has_src2) { ops[nops] = tcc_ir_op_get_src2(ir, q); which[nops] = 2; nops++; }
  for (int k = 0; k < nops; k++)
  {
    IROperand o = ops[k];
    if (irop_get_tag(o) != IROP_TAG_STACKOFF || !o.is_local)
      continue;
    int32_t off = irop_get_stack_offset(o);
    if (off < lo + sz && off + 4 > lo) /* conservative 4-byte footprint */
    {
      touched = 1;
      /* A store to this slot, or an lval dest, is a write of the region. */
      if (which[k] == 0 && o.is_lval &&
          (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
           q->op == TCCIR_OP_STORE_POSTINC))
        *is_write = 1;
    }
  }
  return touched;
}

int tcc_ir_opt_struct_copy_roundtrip_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 4)
    return 0;

  for (int i1 = 0; i1 < n; i1++)
  {
    IRQuadCompact *c1 = &ir->compact_instructions[i1];
    if (!scre_is_memcpy_like(ir, c1))
      continue;

    int32_t b_off, a_off, sz1;
    if (!scre_get_copy(ir, i1, &b_off, &a_off, &sz1)) /* C1: B := A */
      continue;
    if (a_off == b_off)
      continue;
    /* A and B regions must be disjoint for A:=B:=A to be an identity on A. */
    if (!(a_off + sz1 <= b_off || b_off + sz1 <= a_off))
      continue;

    /* Scan forward for C2, allowing only benign (non-writing, non-call)
     * instructions in between.  The first call encountered must be C2. */
    int i2 = -1;
    int ok = 1;
    for (int k = i1 + 1; k < n && ok; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        i2 = k;
        break;
      }
      /* Any memory write between the two copies invalidates the "A unchanged"
       * premise. */
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
          q->op == TCCIR_OP_STORE_POSTINC)
      {
        ok = 0;
        break;
      }
      /* A control-flow edge means C2 (if any) is in another block — bail. */
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
          q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
          q->is_jump_target)
      {
        ok = 0;
        break;
      }
    }
    if (!ok || i2 < 0)
      continue;

    IRQuadCompact *c2 = &ir->compact_instructions[i2];
    if (!scre_is_memcpy_like(ir, c2))
      continue;
    if (c2->is_jump_target)
      continue;

    int32_t a2_off, b2_off, sz2;
    if (!scre_get_copy(ir, i2, &a2_off, &b2_off, &sz2)) /* C2: A := B */
      continue;
    /* C2 must be the exact reverse copy of C1 with the same length. */
    if (a2_off != a_off || b2_off != b_off || sz2 != sz1)
      continue;

    /* If either call returns a value (FUNCCALLVAL), its result must be dead. */
    int dead_result = 1;
    int calls[2] = {i1, i2};
    for (int ci = 0; ci < 2 && dead_result; ci++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[calls[ci]];
      if (cq->op != TCCIR_OP_FUNCCALLVAL)
        continue;
      IROperand res = tcc_ir_op_get_dest(ir, cq);
      if (!irop_has_vreg(res))
        continue;
      int32_t rv = irop_get_vreg(res);
      for (int u = calls[ci] + 1; u < n; u++)
      {
        IRQuadCompact *uq = &ir->compact_instructions[u];
        if (uq->op == TCCIR_OP_NOP)
          continue;
        const IRRegistersConfig *cfg = &irop_config[uq->op];
        if ((cfg->has_src1 && irop_has_vreg(tcc_ir_op_get_src1(ir, uq)) &&
             irop_get_vreg(tcc_ir_op_get_src1(ir, uq)) == rv) ||
            (cfg->has_src2 && irop_has_vreg(tcc_ir_op_get_src2(ir, uq)) &&
             irop_get_vreg(tcc_ir_op_get_src2(ir, uq)) == rv))
        {
          dead_result = 0;
          break;
        }
        if (cfg->has_dest && irop_has_vreg(tcc_ir_op_get_dest(ir, uq)) &&
            irop_get_vreg(tcc_ir_op_get_dest(ir, uq)) == rv)
          break; /* redefined */
      }
    }
    if (!dead_result)
      continue;

    /* Region B must be a private round-trip buffer: referenced only as C1's
     * dst-addr and C2's src-addr (and their param/LEA setup), nowhere else.
     * Any other read/write/addr-of of B is disqualifying. */
    int b_ok = 1;
    for (int k = 0; k < n && b_ok; k++)
    {
      if (k == i1 || k == i2)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;
      /* The LEA/ASSIGN that materialize B's address and the PARAM ops that
       * pass it belong to C1/C2 — those are allowed; everything else is not.
       * Distinguish by op kind: address-materialization (LEA/ASSIGN of
       * Addr[StackLoc[b]]) and FUNCPARAM* are the only legitimate references.
       * A direct lval load/store of region B, or B's address flowing into any
       * other op, is disqualifying. */
      int is_w;
      if (!scre_touches_region(ir, q, b_off, sz1, &is_w))
        continue;
      if (q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASSIGN ||
          q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
        continue; /* address-of / param plumbing for the two copies */
      b_ok = 0;
    }
    if (!b_ok)
      continue;

    /* Apply: NOP both calls and their param marshalling.  The now-dead address
     * LEAs are cleaned by the following DCE. */
    ir_opt_nop_call_params(ir, i1);
    c1->op = TCCIR_OP_NOP;
    ir_opt_nop_call_params(ir, i2);
    c2->op = TCCIR_OP_NOP;
    changes++;
    LOG_IR_GEN("STRUCT COPY ROUNDTRIP ELIM: calls @%d,%d  A=%d B=%d size=%d", i1, i2, a_off, b_off, sz1);
  }

  return changes;
}

/* ============================================================================
 * Init-copy-from-global load forwarding (tcc_ir_opt_memmove_global_load_fwd)
 * ============================================================================
 *
 * The ubiquitous `struct S y = global; ... return y.field;` idiom (and the
 * 20040709-2 fn1* family after the identity-retme round-trip is removed)
 * lowers to a `memmove(y, &global, N)` copy into a private stack slot followed
 * by a few loads of y's fields — y never escapes and is never written.  GCC
 * skips the copy and reads `global` directly.  This pass does the same: when
 * EVERY reference to the copied slot is a load that lies inside the copied
 * region, it rewrites each load to read the global at the matching offset and
 * NOPs the copy (its now-dead dest LEAs / stack slot are dropped by DCE).
 *
 * Safety — all required, each closes a hazard:
 *   - dest resolves to a LOCAL stack slot D=[dbase,dbase+N); src resolves to a
 *     GLOBAL symref (sym G, addend gA); N constant > 0;
 *   - every address derived from D (the copy's own dest-LEA chain, plus any
 *     `LEA StackLoc[D+k]` and `+#k` interposer) is used ONLY as a load operand
 *     within [0,N) or as the copy's own call params — any store through it, any
 *     escape into another call/op, or any non-load use disqualifies (so D is a
 *     read-only private snapshot);
 *   - the straight-line region between the copy and the last forwarded load
 *     contains NO call and NO store and no control-flow edge — this guarantees
 *     the global source is provably unmodified across the window, AND is
 *     exactly what makes the pass skip the `x=s; r=fn(a); compare x,s` snapshot
 *     idiom (a call sits between the copy and the reads there), whose copy must
 *     be preserved and whose elimination would regress register pressure.
 */

/* Byte width of a load/store operand's base type (0 = not a simple scalar). */
static int mglf_btype_width(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:    return 1;
  case IROP_BTYPE_INT16:   return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32: return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64: return 8;
  default:                 return 0;
  }
}

/* Resolve a memmove src param to a global symref (address-of, not a deref).
 * Handles the direct `GlobalSym` operand and a TEMP defined by `T = LEA sym`. */
static int mglf_resolve_global_src(TCCIRState *ir, IROperand op, int before_idx, IRPoolSymref **out)
{
  if (irop_get_tag(op) == IROP_TAG_SYMREF && !op.is_lval && !op.is_local)
  {
    *out = irop_get_symref_ex(ir, op);
    return *out != NULL;
  }
  if (!irop_has_vreg(op))
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  for (int d = before_idx - 1; d >= 0; d--)
  {
    IRQuadCompact *dq = &ir->compact_instructions[d];
    if (dq->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[dq->op].has_dest)
      continue;
    IROperand dd = tcc_ir_op_get_dest(ir, dq);
    if (!irop_has_vreg(dd) || irop_get_vreg(dd) != vr)
      continue;
    if (dq->op != TCCIR_OP_LEA && dq->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
    if (dq->op == TCCIR_OP_ASSIGN && !irop_is_none(tcc_ir_op_get_src2(ir, dq)))
      return 0;
    if (irop_get_tag(ds1) != IROP_TAG_SYMREF || ds1.is_lval || ds1.is_local)
      return 0;
    *out = irop_get_symref_ex(ir, ds1);
    return *out != NULL;
  }
  return 0;
}

/* Ops whose is_lval source operands are plain value-loads of that address
 * (the deref reads `width = operand-btype` bytes and uses them).  Forwarding
 * such an operand from a non-escaping local copy to the global source is sound
 * under the same byte-equality window (memmove + no intervening write) as a
 * standalone LOAD — only WHICH address the bytes are read from changes, not
 * which/how many bytes.  Whitelist keeps out address-only (LEA), stores,
 * indexed/postinc (complex addressing), calls/params (escape) and FP ops. */
static int mglf_is_value_read_op(int op)
{
  switch (op)
  {
  case TCCIR_OP_ADD: case TCCIR_OP_SUB: case TCCIR_OP_MUL:
  case TCCIR_OP_AND: case TCCIR_OP_OR:  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL: case TCCIR_OP_SAR: case TCCIR_OP_SHR:
  case TCCIR_OP_ROR: case TCCIR_OP_CMP: case TCCIR_OP_UBFX:
  case TCCIR_OP_ASSIGN:
    return 1;
  default:
    return 0;
  }
}

#define MGLF_MAX 32

int tcc_ir_opt_memmove_global_load_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int ci = 0; ci < n; ci++)
  {
    IRQuadCompact *c = &ir->compact_instructions[ci];
    if (!scre_is_memcpy_like(ir, c))
      continue;

    IROperand p0, p1, p2;
    if (!ir_opt_get_call_param_operand(ir, ci, 0, &p0) ||
        !ir_opt_get_call_param_operand(ir, ci, 1, &p1) ||
        !ir_opt_get_call_param_operand(ir, ci, 2, &p2))
      continue;
    if (irop_get_tag(p2) != IROP_TAG_IMM32)
      continue;
    int32_t N = (int32_t)p2.u.imm32;
    if (N <= 0 || N > 256)
      continue;

    int32_t dbase;
    int dl;
    if (!scre_resolve_slot_addr(ir, p0, ci, &dbase, &dl) || !dl)
      continue; /* dest must be a local stack slot */

    IRPoolSymref *gref = NULL;
    if (!mglf_resolve_global_src(ir, p1, ci, &gref) || !gref || !gref->sym)
      continue; /* src must be a global */

    /* Worklist of address vregs derived from D: (vreg, byte-offset-into-D,
     * defining-index).  The copy's own dest address (p0 / its LEA) is allowed
     * only as this call's params, captured by seeding it here. */
    int32_t wl_vr[MGLF_MAX];
    int32_t wl_off[MGLF_MAX];
    int wl_def[MGLF_MAX];
    int wl_n = 0;

    /* Seed: every LEA/ASSIGN of Addr[StackLoc[off]] with off in [dbase,dbase+N)
     * defines an address into D. */
    for (int k = 0; k < n; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (q->op == TCCIR_OP_ASSIGN && !irop_is_none(tcc_ir_op_get_src2(ir, q)))
        continue;
      if (irop_get_tag(s1) != IROP_TAG_STACKOFF || s1.is_lval || !s1.is_local)
        continue;
      int32_t off = irop_get_stack_offset(s1);
      if (off < dbase || off >= dbase + N)
        continue;
      IROperand dd = tcc_ir_op_get_dest(ir, q);
      if (!irop_has_vreg(dd))
        continue;
      if (wl_n >= MGLF_MAX)
        goto next_call;
      wl_vr[wl_n] = irop_get_vreg(dd);
      wl_off[wl_n] = off - dbase;
      wl_def[wl_n] = k;
      wl_n++;
    }
    if (wl_n == 0)
      continue;

    /* Collected forwardable load sites.  ld_which is the operand slot to
     * rewrite (1 = src1, 2 = src2); standalone LOADs and direct-StackLoc reads
     * are always src1, fused ALU-operand derefs may be either. */
    int ld_idx[MGLF_MAX];
    int32_t ld_delta[MGLF_MAX];
    int ld_which[MGLF_MAX];
    int ld_n = 0;
    int ok = 1;
    int last_load = ci;

    for (int w = 0; w < wl_n && ok; w++)
    {
      int32_t av = wl_vr[w];
      int32_t aoff = wl_off[w];
      int adef = wl_def[w];

      for (int k = ci + 1; k < n && ok; k++)
      {
        if (k == adef)
          continue;
        IRQuadCompact *q = &ir->compact_instructions[k];
        if (q->op == TCCIR_OP_NOP)
          continue;
        const IRRegistersConfig *cfg = &irop_config[q->op];
        IROperand s1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE;
        IROperand s2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE;
        IROperand d = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE;

        int in_s1 = cfg->has_src1 && irop_has_vreg(s1) && irop_get_vreg(s1) == av;
        int in_s2 = cfg->has_src2 && irop_has_vreg(s2) && irop_get_vreg(s2) == av;
        int in_d = cfg->has_dest && irop_has_vreg(d) && irop_get_vreg(d) == av;
        if (!in_s1 && !in_s2 && !in_d)
          continue;
        /* (The copy's own params reference D's dest address but always precede
         * the call, so they are never seen by this post-call use-scan; any
         * FUNCPARAM use found here is an escape into another call and falls
         * through to the disqualifying default below.) */

        /* Interposer `new = av + #k` (av as a plain pointer value). */
        if (q->op == TCCIR_OP_ADD && in_s1 && !s1.is_lval && !in_s2 && !in_d &&
            irop_get_tag(s2) == IROP_TAG_IMM32)
        {
          int32_t nv = irop_get_vreg(d);
          if (nv < 0 || d.is_lval || wl_n >= MGLF_MAX)
          {
            ok = 0;
            break;
          }
          wl_vr[wl_n] = nv;
          wl_off[wl_n] = aoff + (int32_t)s2.u.imm32;
          wl_def[wl_n] = k;
          wl_n++;
          continue;
        }

        /* A single clean LOAD whose address operand is av (load reads D). */
        int which = in_s1 ? 1 : in_s2 ? 2 : 0;
        IROperand dref = (which == 1) ? s1 : (which == 2) ? s2 : d;
        if (q->op == TCCIR_OP_LOAD && which == 1 && s1.is_lval && !in_s2 && !in_d)
        {
          int wbytes = mglf_btype_width(irop_get_btype(dref));
          if (wbytes == 0 || aoff < 0 || aoff + wbytes > N || ld_n >= MGLF_MAX)
          {
            ok = 0;
            break;
          }
          ld_idx[ld_n] = k;
          ld_delta[ld_n] = aoff;
          ld_which[ld_n] = 1;
          ld_n++;
          if (k > last_load)
            last_load = k;
          continue;
        }

        /* A value-read op (ADD/SHR/AND/CMP/...) whose is_lval operand is av
         * reads D's bytes exactly like a standalone LOAD — the field access of
         * a 64-bit bitfield (or any wider field) lowers to a deref fused into
         * the shift/mask/add rather than a bare LOAD.  Forward that single
         * operand to the global.  Require av to appear in exactly one src slot
         * (unambiguous offset), as a deref, never as the dest. */
        if (mglf_is_value_read_op(q->op) && which != 0 && dref.is_lval &&
            !in_d && (in_s1 ^ in_s2))
        {
          int wbytes = mglf_btype_width(irop_get_btype(dref));
          if (wbytes == 0 || aoff < 0 || aoff + wbytes > N || ld_n >= MGLF_MAX)
          {
            ok = 0;
            break;
          }
          ld_idx[ld_n] = k;
          ld_delta[ld_n] = aoff;
          ld_which[ld_n] = which;
          ld_n++;
          if (k > last_load)
            last_load = k;
          continue;
        }

        /* Anything else touching D's address (store, escape, indexed, struct
         * read, non-lval use) disqualifies. */
        ok = 0;
      }
    }

    /* Second scan: DIRECT StackLoc references to the slot (the plain-struct
     * field read `T = StackLoc[off] [LOAD]`, which has no LEA-temp).  A direct
     * lval LOAD src1 in the slot is forwardable; the address-of operand of the
     * seeding LEA/ASSIGN (is_lval==0) is skipped (handled by the worklist
     * above); anything else (a store, or any other direct use) disqualifies. */
    for (int k = ci + 1; k < n && ok; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;
      const IRRegistersConfig *cfg = &irop_config[q->op];
      IROperand ops3[3];
      int np = 0, posn[3];
      if (cfg->has_dest) { ops3[np] = tcc_ir_op_get_dest(ir, q); posn[np] = 0; np++; }
      if (cfg->has_src1) { ops3[np] = tcc_ir_op_get_src1(ir, q); posn[np] = 1; np++; }
      if (cfg->has_src2) { ops3[np] = tcc_ir_op_get_src2(ir, q); posn[np] = 2; np++; }
      for (int p = 0; p < np && ok; p++)
      {
        IROperand o = ops3[p];
        if (irop_get_tag(o) != IROP_TAG_STACKOFF || !o.is_local)
          continue;
        int32_t off = irop_get_stack_offset(o);
        if (off < dbase || off >= dbase + N)
          continue;
        /* Address-of in the seeding LEA/ASSIGN (already tracked by Pass 1). */
        if (!o.is_lval && posn[p] == 1 &&
            (q->op == TCCIR_OP_LEA ||
             (q->op == TCCIR_OP_ASSIGN && irop_is_none(tcc_ir_op_get_src2(ir, q)))))
          continue;
        /* Direct lval LOAD of the slot, or a value-read op (ADD/SHR/...) whose
         * is_lval StackLoc operand reads the slot — both forwardable.  A store
         * (dest, posn 0) or any other reference disqualifies. */
        if (((q->op == TCCIR_OP_LOAD && posn[p] == 1) ||
             (mglf_is_value_read_op(q->op) && posn[p] != 0)) && o.is_lval)
        {
          int wbytes = mglf_btype_width(irop_get_btype(o));
          int32_t delta = off - dbase;
          if (wbytes == 0 || delta < 0 || delta + wbytes > N || ld_n >= MGLF_MAX)
          {
            ok = 0;
            break;
          }
          ld_idx[ld_n] = k;
          ld_delta[ld_n] = delta;
          ld_which[ld_n] = posn[p];
          ld_n++;
          if (k > last_load)
            last_load = k;
          continue;
        }
        /* Any other direct reference (store, etc.) disqualifies. */
        ok = 0;
      }
    }

    if (!ok || ld_n == 0)
      continue;

    /* The window [ci+1, last_load] must be straight-line with no call, no
     * store and no control-flow edge, so the global is provably unmodified. */
    for (int k = ci + 1; k <= last_load && ok; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;
      switch (q->op)
      {
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_STORE:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_JUMP:
      case TCCIR_OP_JUMPIF:
      case TCCIR_OP_IJUMP:
      case TCCIR_OP_SWITCH_TABLE:
        ok = 0;
        break;
      default:
        if (q->is_jump_target)
          ok = 0;
        break;
      }
    }
    if (!ok)
      continue;

    /* Apply: rewrite every load operand to a global deref at the matching
     * offset, then NOP the copy call + its params.  Capture the symref fields
     * first — tcc_ir_pool_add_symref may reallocate the symref pool and
     * invalidate `gref`. */
    Sym *gsym = gref->sym;
    int32_t gaddend = gref->addend;
    uint32_t gflags = gref->flags;
    for (int r = 0; r < ld_n; r++)
    {
      IRQuadCompact *lq = &ir->compact_instructions[ld_idx[r]];
      IROperand old = (ld_which[r] == 2) ? tcc_ir_op_get_src2(ir, lq)
                                         : tcc_ir_op_get_src1(ir, lq);
      uint32_t pool = tcc_ir_pool_add_symref(ir, gsym, gaddend + ld_delta[r], gflags);
      IROperand g = irop_make_symref(-1, pool, /*is_lval*/ 1, /*is_local*/ 0, /*is_const*/ 0,
                                     irop_get_btype(old));
      g.is_unsigned = old.is_unsigned;
      if (ld_which[r] == 2)
        tcc_ir_op_set_src2(ir, lq, g);
      else
        tcc_ir_op_set_src1(ir, lq, g);
    }
    ir_opt_nop_call_params(ir, ci);
    c->op = TCCIR_OP_NOP;
    changes++;
    LOG_IR_GEN("MEMMOVE GLOBAL LOAD FWD: copy@%d D=%d N=%d -> %d loads forwarded to global", ci, dbase, N, ld_n);
  next_call:;
  }

  return changes;
}

/* ============================================================================
 * Diamond Store Forwarding (tcc_ir_opt_diamond_store_fwd)
 * ============================================================================
 *
 * When both arms of an if/else diamond STORE the same constant through a
 * computed address, and a post-merge LOAD_INDEXED reads from the same
 * address, forward the constant to the load.
 *
 * Uses a custom structural expression comparison (dsf_expr_equal) that
 * tolerates intervening STOREs between two definitions — the standard
 * ir_opt_pure_expr_equal bails when a STORE sits between def sites because
 * VAR reads are encoded as is_lval stack-slot reads, triggering a memory
 * stability check.  In this diamond pattern the intervening STORE writes to
 * the array, not to any VAR slot, so the comparison is safe.
 */

/* Structural expression equality across a diamond, ignoring memory stability.
 * Compares two operands by recursively unfolding their defining instruction
 * trees.  Only checks that the same operations are applied to the same
 * leaf VARs/immediates — does NOT verify that memory hasn't changed between
 * the two definition sites (the caller guarantees this by construction). */
static int dsf_expr_equal(TCCIRState *ir, IROperand a, int a_use,
                          IROperand b, int b_use, int depth)
{
  if (depth > 12)
    return 0;

  if (irop_is_immediate(a) && irop_is_immediate(b))
    return irop_get_imm64_ex(ir, a) == irop_get_imm64_ex(ir, b);
  if (irop_is_immediate(a) || irop_is_immediate(b))
    return 0;

  int32_t a_vr = irop_get_vreg(a);
  int32_t b_vr = irop_get_vreg(b);
  if (a_vr < 0 || b_vr < 0)
    return 0;

  int a_def = tcc_ir_find_defining_instruction(ir, a_vr, a_use);
  int b_def = tcc_ir_find_defining_instruction(ir, b_vr, b_use);
  if (a_def < 0 || b_def < 0)
    return a_vr == b_vr && a_def == b_def;
  if (a_def == b_def)
    return 1;
  if (!tcc_ir_vreg_has_single_def(ir, a_vr) || !tcc_ir_vreg_has_single_def(ir, b_vr))
    return 0;

  IRQuadCompact *qa = &ir->compact_instructions[a_def];
  IRQuadCompact *qb = &ir->compact_instructions[b_def];
  if (qa->op != qb->op)
    return 0;

  switch (qa->op) {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return dsf_expr_equal(ir, tcc_ir_op_get_src1(ir, qa), a_def,
                          tcc_ir_op_get_src1(ir, qb), b_def, depth + 1);
  case TCCIR_OP_ADD:
  case TCCIR_OP_MUL:
  case TCCIR_OP_OR:
  case TCCIR_OP_AND:
  case TCCIR_OP_XOR:
  {
    IROperand a1 = tcc_ir_op_get_src1(ir, qa), a2 = tcc_ir_op_get_src2(ir, qa);
    IROperand b1 = tcc_ir_op_get_src1(ir, qb), b2 = tcc_ir_op_get_src2(ir, qb);
    return (dsf_expr_equal(ir, a1, a_def, b1, b_def, depth+1) &&
            dsf_expr_equal(ir, a2, a_def, b2, b_def, depth+1)) ||
           (dsf_expr_equal(ir, a1, a_def, b2, b_def, depth+1) &&
            dsf_expr_equal(ir, a2, a_def, b1, b_def, depth+1));
  }
  case TCCIR_OP_SUB:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  {
    IROperand a1 = tcc_ir_op_get_src1(ir, qa), a2 = tcc_ir_op_get_src2(ir, qa);
    IROperand b1 = tcc_ir_op_get_src1(ir, qb), b2 = tcc_ir_op_get_src2(ir, qb);
    return dsf_expr_equal(ir, a1, a_def, b1, b_def, depth+1) &&
           dsf_expr_equal(ir, a2, a_def, b2, b_def, depth+1);
  }
  default:
    return 0;
  }
}

/* Decompose a STORE destination address into base + (index << shift). */
static int dsf_decompose_store_addr(TCCIRState *ir, int store_idx,
                                    IROperand *out_base, int *out_base_def,
                                    int32_t *out_index_vr, int *out_shift)
{
  IRQuadCompact *sq = &ir->compact_instructions[store_idx];
  if (sq->op != TCCIR_OP_STORE)
    return 0;
  IROperand dest = tcc_ir_op_get_dest(ir, sq);
  if (!dest.is_lval)
    return 0;
  int32_t addr_vr = irop_get_vreg(dest);
  if (addr_vr < 0)
    return 0;

  int addr_def = tcc_ir_find_defining_instruction(ir, addr_vr, store_idx);
  if (addr_def < 0)
    return 0;
  IRQuadCompact *aq = &ir->compact_instructions[addr_def];
  if (aq->op != TCCIR_OP_ADD)
    return 0;

  IROperand a1 = tcc_ir_op_get_src1(ir, aq);
  IROperand a2 = tcc_ir_op_get_src2(ir, aq);

  for (int side = 0; side < 2; side++) {
    IROperand idx_side = side == 0 ? a2 : a1;
    IROperand base_side = side == 0 ? a1 : a2;
    int32_t sv = irop_get_vreg(idx_side);
    if (sv < 0) continue;
    int sd = tcc_ir_find_defining_instruction(ir, sv, addr_def);
    if (sd < 0) continue;
    IRQuadCompact *sq2 = &ir->compact_instructions[sd];
    if (sq2->op != TCCIR_OP_SHL) continue;
    IROperand shl_src2 = tcc_ir_op_get_src2(ir, sq2);
    if (!irop_is_immediate(shl_src2)) continue;
    int shift = (int)irop_get_imm64_ex(ir, shl_src2);
    if (shift < 1 || shift > 3) continue;
    IROperand idx_op = tcc_ir_op_get_src1(ir, sq2);
    int32_t idx_vr = irop_get_vreg(idx_op);
    if (idx_vr < 0) continue;
    *out_base = base_side;
    *out_base_def = addr_def;
    *out_index_vr = idx_vr;
    *out_shift = shift;
    return 1;
  }
  return 0;
}

/* Find the STORE in a diamond branch [from..limit). */
static int dsf_find_branch_store(TCCIRState *ir, int from, int limit,
                                 int64_t *out_const, int *out_merge_target)
{
  int store_idx = -1;
  *out_merge_target = -1;
  for (int j = from; j < limit; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_STORE) {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (!irop_is_immediate(src1))
        return -1;
      *out_const = irop_get_imm64_ex(ir, src1);
      store_idx = j;
      continue;
    }
    if (q->op == TCCIR_OP_JUMP) {
      *out_merge_target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      break;
    }
    if (q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_INLINE_ASM)
      return -1;
  }
  return store_idx;
}

int tcc_ir_opt_diamond_store_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 8)
    return 0;

  for (int i = 0; i < n - 4; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMPIF)
      continue;

    int else_target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
    if (else_target <= i || else_target >= n)
      continue;

    int last_jumpif = i;
    for (int j = i + 1; j < else_target && j < n; j++) {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP || jq->op == TCCIR_OP_CMP)
        continue;
      if (jq->op == TCCIR_OP_JUMPIF) {
        int jt = (int)tcc_ir_op_get_dest(ir, jq).u.imm32;
        if (jt == else_target) last_jumpif = j; else break;
      } else break;
    }

    int then_start = last_jumpif + 1;
    if (then_start >= else_target)
      continue;

    int64_t then_const = 0;
    int then_merge = -1;
    int then_store = dsf_find_branch_store(ir, then_start, else_target,
                                           &then_const, &then_merge);
    if (then_store < 0 || then_merge < 0)
      continue;

    int else_limit = then_merge < n ? then_merge : n;
    int64_t else_const = 0;
    int else_merge = -1;
    int else_store = dsf_find_branch_store(ir, else_target, else_limit,
                                           &else_const, &else_merge);
    if (else_store < 0)
      continue;
    if (else_merge >= 0 && else_merge != then_merge)
      continue;
    if (then_const != else_const)
      continue;

    IROperand then_base;
    int then_base_def, then_shift;
    int32_t then_idx_vr;
    if (!dsf_decompose_store_addr(ir, then_store, &then_base,
                                  &then_base_def, &then_idx_vr, &then_shift))
      continue;

    IROperand else_base;
    int else_base_def, else_shift;
    int32_t else_idx_vr;
    if (!dsf_decompose_store_addr(ir, else_store, &else_base,
                                  &else_base_def, &else_idx_vr, &else_shift))
      continue;

    if (then_idx_vr != else_idx_vr || then_shift != else_shift)
      continue;

    if (!dsf_expr_equal(ir, then_base, then_base_def,
                        else_base, else_base_def, 0))
      continue;

    int merge = then_merge;
    int load_idx = -1;
    for (int j = merge; j < n; j++) {
      IRQuadCompact *mq = &ir->compact_instructions[j];
      if (mq->op == TCCIR_OP_NOP)
        continue;
      if (mq->op == TCCIR_OP_LOAD_INDEXED) {
        IROperand load_base = tcc_ir_op_get_src1(ir, mq);
        IROperand load_index = tcc_ir_op_get_src2(ir, mq);
        IROperand load_scale = tcc_ir_op_get_scale(ir, mq);
        int32_t load_idx_vr = irop_get_vreg(load_index);
        int load_shift = irop_is_immediate(load_scale)
                           ? (int)irop_get_imm64_ex(ir, load_scale) : -1;

        if (load_idx_vr == then_idx_vr && load_shift == then_shift &&
            dsf_expr_equal(ir, then_base, then_base_def, load_base, j, 0)) {
          load_idx = j;
          break;
        }
      }
      if (mq->op == TCCIR_OP_STORE || mq->op == TCCIR_OP_STORE_INDEXED ||
          mq->op == TCCIR_OP_STORE_POSTINC || mq->op == TCCIR_OP_BLOCK_COPY ||
          mq->op == TCCIR_OP_FUNCCALLVOID || mq->op == TCCIR_OP_FUNCCALLVAL ||
          mq->op == TCCIR_OP_INLINE_ASM || mq->op == TCCIR_OP_JUMP ||
          mq->op == TCCIR_OP_JUMPIF || mq->op == TCCIR_OP_RETURNVALUE ||
          mq->op == TCCIR_OP_RETURNVOID)
        break;
    }
    if (load_idx < 0)
      continue;

    IRQuadCompact *lq = &ir->compact_instructions[load_idx];
    IROperand load_dest = tcc_ir_op_get_dest(ir, lq);
    int dest_btype = irop_get_btype(load_dest);
    IROperand const_op;
    if (dest_btype == IROP_BTYPE_FLOAT32)
      const_op = irop_make_f32(-1, (uint32_t)then_const);
    else if (dest_btype == IROP_BTYPE_FLOAT64) {
      uint32_t pidx = tcc_ir_pool_add_f64(ir, (uint64_t)then_const);
      const_op = irop_make_f64(-1, pidx);
    } else if (then_const == (int32_t)then_const)
      const_op = irop_make_imm32(-1, (int32_t)then_const, dest_btype);
    else {
      uint32_t pidx = tcc_ir_pool_add_i64(ir, then_const);
      const_op = irop_make_i64(-1, pidx, dest_btype);
    }

    lq->op = TCCIR_OP_ASSIGN;
    ir->iroperand_pool[lq->operand_base + 0] = load_dest;
    ir->iroperand_pool[lq->operand_base + 1] = const_op;
    ir->iroperand_pool[lq->operand_base + 2] = IROP_NONE;
    ir->iroperand_pool[lq->operand_base + 3] = IROP_NONE;
    changes++;
  }

  return changes;
}

/* ============================================================================
 * Pointer-Deref Load CSE
 * ============================================================================
 *
 * Within a basic block, eliminate redundant loads through the same pointer
 * dereference.  When we see:
 *
 *   i:   T1 = Vaddr***DEREF***          (load through pointer)
 *   ...  (no stores, calls, or BB boundaries)
 *   j:   T2 = Vaddr***DEREF***          (same pointer deref)
 *
 * Replace all uses of T2 with T1 and NOP instruction j.
 *
 * This targets the pattern where struct field access generates repeated loads
 * of the same pointer (e.g. d->hstent loaded 5 times in a bitfield chain).
 * Conservative: kills tracking on any memory store or function call.
 * ============================================================================ */
int tcc_ir_opt_ptr_load_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  int max_tmp = ir->next_temporary_variable;
  if (max_tmp <= 0)
    return 0;

  int32_t *copy_src = tcc_mallocz(sizeof(int32_t) * (max_tmp + 1));
  for (int t = 0; t <= max_tmp; t++)
    copy_src[t] = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *cq = &ir->compact_instructions[i];
    if (cq->op != TCCIR_OP_ASSIGN)
      continue;
    IROperand cd = tcc_ir_op_get_dest(ir, cq);
    IROperand cs = tcc_ir_op_get_src1(ir, cq);
    int32_t cdv = irop_get_vreg(cd);
    int32_t csv = irop_get_vreg(cs);
    if (cdv < 0 || csv < 0 || cd.is_lval || cs.is_lval)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(cdv) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(cdv);
    if (pos <= max_tmp)
      copy_src[pos] = csv;
  }

#define PLCSE_RESOLVE(vr)                                                                                              \
  do                                                                                                                   \
  {                                                                                                                    \
    for (int _d = 0; _d < 8; _d++)                                                                                    \
    {                                                                                                                  \
      if ((vr) < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)                                             \
        break;                                                                                                         \
      int _p = TCCIR_DECODE_VREG_POSITION(vr);                                                                         \
      if (_p > max_tmp || copy_src[_p] < 0)                                                                            \
        break;                                                                                                         \
      (vr) = copy_src[_p];                                                                                             \
    }                                                                                                                  \
  } while (0)

#define PLCSE_MAX 16
  struct
  {
    int32_t canon_vr;
    int32_t dest_vr;
    int btype;
  } cache[PLCSE_MAX];
  int cache_count = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->is_jump_target)
      cache_count = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_FUNCCALLVAL ||
        q->op == TCCIR_OP_FUNCCALLVOID)
    {
      cache_count = 0;
      continue;
    }

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_BLOCK_COPY)
    {
      cache_count = 0;
      continue;
    }

    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src = tcc_ir_op_get_src1(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      int32_t src_vr = irop_get_vreg(src);
      int dest_type = (dest_vr >= 0) ? TCCIR_DECODE_VREG_TYPE(dest_vr) : -1;

      if (dest_type == TCCIR_VREG_TYPE_TEMP && src.is_lval && !dest.is_lval && src_vr >= 0)
      {
        int btype = irop_get_btype(src);
        int32_t canon = src_vr;
        PLCSE_RESOLVE(canon);

        for (int c = 0; c < cache_count; c++)
        {
          if (cache[c].canon_vr == canon && cache[c].btype == btype)
          {
            int32_t cached_vr = cache[c].dest_vr;
            for (int k = i + 1; k < n; k++)
            {
              IRQuadCompact *kq = &ir->compact_instructions[k];
              if (kq->op == TCCIR_OP_NOP)
                continue;
              if (kq->is_jump_target)
                break;
              if (kq->op == TCCIR_OP_JUMP || kq->op == TCCIR_OP_JUMPIF ||
                  kq->op == TCCIR_OP_RETURNVALUE || kq->op == TCCIR_OP_RETURNVOID)
                break;
              if (irop_config[kq->op].has_src1)
              {
                IROperand ks = tcc_ir_op_get_src1(ir, kq);
                if (irop_get_vreg(ks) == dest_vr)
                {
                  irop_set_vreg(&ks, cached_vr);
                  tcc_ir_set_src1(ir, k, ks);
                }
              }
              if (irop_config[kq->op].has_src2)
              {
                IROperand ks = tcc_ir_op_get_src2(ir, kq);
                if (irop_get_vreg(ks) == dest_vr)
                {
                  irop_set_vreg(&ks, cached_vr);
                  tcc_ir_set_src2(ir, k, ks);
                }
              }
              if (irop_config[kq->op].has_dest)
              {
                IROperand kd = tcc_ir_op_get_dest(ir, kq);
                if (irop_get_vreg(kd) == dest_vr)
                {
                  irop_set_vreg(&kd, cached_vr);
                  tcc_ir_set_dest(ir, k, kd);
                }
              }
            }
            q->op = TCCIR_OP_NOP;
            changes++;
            goto plcse_next;
          }
        }

        if (cache_count < PLCSE_MAX)
        {
          cache[cache_count].canon_vr = canon;
          cache[cache_count].dest_vr = dest_vr;
          cache[cache_count].btype = btype;
          cache_count++;
        }
        goto plcse_next;
      }
    }

    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      if (dest_vr >= 0 && !dest.is_lval)
      {
        int w = 0;
        for (int c = 0; c < cache_count; c++)
        {
          if (cache[c].dest_vr != dest_vr && cache[c].canon_vr != dest_vr)
            cache[w++] = cache[c];
        }
        cache_count = w;
      }
    }

  plcse_next:;
  }
#undef PLCSE_MAX
#undef PLCSE_RESOLVE
  tcc_free(copy_src);
  return changes;
}

/* ============================================================================
 * Pointer Store-to-Load Forwarding + Dead Store Elimination
 * ============================================================================
 *
 * Within a basic block, two optimizations on pointer-dereference memory ops:
 *
 * 1. Store-to-load forwarding: when a value is stored through a pointer
 *    dereference and later loaded from the same dereference, forward the
 *    stored value.
 *
 * 2. Dead store elimination: when a store to a pointer dereference is
 *    followed by another store to the same address with no intervening
 *    load from that address, the first store is dead and can be NOPed.
 *
 * Conservative: kills all entries on any store to a different address,
 * function calls, or BB boundaries.
 * ============================================================================ */
int tcc_ir_opt_ptr_store_load_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

#define PSLFWD_MAX 8
  struct
  {
    int32_t addr_vr;
    IROperand value_op;
    int store_idx;
    int was_loaded;
  } cache[PSLFWD_MAX];
  int cache_count = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->is_jump_target)
      cache_count = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_FUNCCALLVAL ||
        q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_BLOCK_COPY ||
        q->op == TCCIR_OP_INLINE_ASM)
    {
      cache_count = 0;
      continue;
    }

    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src = tcc_ir_op_get_src1(ir, q);
      int32_t addr_vr = irop_get_vreg(dest);
      int32_t val_vr = irop_get_vreg(src);

      if (dest.is_lval && addr_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(addr_vr) == TCCIR_VREG_TYPE_TEMP &&
          val_vr >= 0 && !src.is_lval)
      {
        int found = -1;
        for (int c = 0; c < cache_count; c++)
        {
          if (cache[c].addr_vr == addr_vr)
          {
            found = c;
            break;
          }
        }
        if (found >= 0)
        {
          if (!cache[found].was_loaded && cache[found].store_idx >= 0)
          {
            ir->compact_instructions[cache[found].store_idx].op = TCCIR_OP_NOP;
            changes++;
          }
          cache[found].value_op = src;
          cache[found].store_idx = i;
          cache[found].was_loaded = 0;
        }
        else
        {
          cache_count = 0;
          if (cache_count < PSLFWD_MAX)
          {
            cache[cache_count].addr_vr = addr_vr;
            cache[cache_count].value_op = src;
            cache[cache_count].store_idx = i;
            cache[cache_count].was_loaded = 0;
            cache_count++;
          }
        }
        continue;
      }
      cache_count = 0;
      continue;
    }

    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    {
      cache_count = 0;
      continue;
    }

    if (cache_count == 0)
      continue;

    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);
      if (dest_vr >= 0 && !dest.is_lval)
      {
        int w = 0;
        for (int c = 0; c < cache_count; c++)
        {
          int32_t val_vr = irop_get_vreg(cache[c].value_op);
          if (cache[c].addr_vr != dest_vr && val_vr != dest_vr)
            cache[w++] = cache[c];
        }
        cache_count = w;
      }
    }

    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (src1.is_lval)
      {
        int32_t src1_vr = irop_get_vreg(src1);
        for (int c = 0; c < cache_count; c++)
        {
          if (cache[c].addr_vr == src1_vr)
          {
            IROperand repl = cache[c].value_op;
            repl.btype = src1.btype;
            repl.is_unsigned = src1.is_unsigned;
            tcc_ir_set_src1(ir, i, repl);
            changes++;
            break;
          }
        }
      }
    }

    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      if (src2.is_lval)
      {
        int32_t src2_vr = irop_get_vreg(src2);
        for (int c = 0; c < cache_count; c++)
        {
          if (cache[c].addr_vr == src2_vr)
          {
            IROperand repl = cache[c].value_op;
            repl.btype = src2.btype;
            repl.is_unsigned = src2.is_unsigned;
            tcc_ir_set_src2(ir, i, repl);
            changes++;
            break;
          }
        }
      }
    }
  }
#undef PSLFWD_MAX
  return changes;
}
