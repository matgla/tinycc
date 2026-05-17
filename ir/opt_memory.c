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

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_alias.h"

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
   * so no aliasing or clobber analysis is needed. */
  int n = ir->next_instruction_index;
  int changes = 0;

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
#define SL_FWD_MAX_DEAD_STORES 16
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
      if (lsrc1.is_local && !lsrc1.is_lval && d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        int src_tag = irop_get_tag(lsrc1);
        int32_t src_vr = irop_get_vreg(lsrc1);
        if (tmp_pos <= max_tmp)
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
          if (resolved)
          {
            lea_map[tmp_pos].offset = loff;
            lea_map[tmp_pos].sym = lsym;
            lea_map[tmp_pos].valid = 1;
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
      if (jtarget >= 0 && jtarget < n && pred_count[jtarget] == 1 && entry_count > 0)
      {
        /* Snapshot entries[] for the target to restore on entry */
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
        LOG_SL_FWD("BB@i=%d RESET: multi-pred target (preds=%d) — dropping %d tracked stores", i, pred_count[i],
                   entry_count);
        memset(hash_table, 0, sizeof(hash_table));
        entry_count = 0;
        write_tracker_gen++;
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
     *  - VAR vregs use abstract positions, not real stack offsets. */
    if (q->op == TCCIR_OP_LOAD ||
        (q->op == TCCIR_OP_ASSIGN && tcc_ir_op_get_src1(ir, q).is_lval &&
         !(irop_get_vreg(tcc_ir_op_get_src1(ir, q)) >= 0 &&
           TCCIR_DECODE_VREG_TYPE(irop_get_vreg(tcc_ir_op_get_src1(ir, q))) == TCCIR_VREG_TYPE_VAR)) ||
        (q->op == TCCIR_OP_FUNCPARAMVAL && tcc_ir_op_get_src1(ir, q).is_local && tcc_ir_op_get_src1(ir, q).is_lval &&
         !tcc_ir_op_get_src1(ir, q).is_complex && !irop_is_64bit(tcc_ir_op_get_src1(ir, q)) &&
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
              store_bits = 32;
              break;
            case IROP_BTYPE_INT64:
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
              load_bits = 32;
              break;
            default:
              load_bits = 0;
              break;
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
          for (lie = hash_table[lih]; lie != NULL; lie = lie->next)
          {
            if (!lie->valid)
              continue;
            if (lie->local_sym != eff_sym || lie->local_offset != eff_off)
              continue;
            if (lie->store_btype != li_src1.btype)
              continue;
            int li_tag = irop_get_tag(lie->stored_value);
            if (li_tag != IROP_TAG_IMM32 && li_tag != IROP_TAG_I64 && li_tag != IROP_TAG_STACKOFF)
              continue;
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
      /* Invalidate wider entries at lower offsets that partially overlap.
       * A store of N bytes at offset X overwrites part of any wider entry
       * at offset Y < X where Y + entry_bytes > X.  Example:
       *   int-store  #0 at -48   (creates 32-bit entry)
       *   short-store #73 at -48 (merges low bits into 32-bit entry)
       *   short-store #65531 at -46 (must invalidate 32-bit entry at -48) */
      {
        int max_delta = (new_bits_local > 0) ? (4 - new_bits_local / 8) : 0;
        if (max_delta < 0)
          max_delta = 0;
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
            if (entry_bytes > delta)
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
#define RSE_MAX_ACTIVE 16
  typedef struct
  {
    int64_t offset;
    const Sym *sym;
    int store_idx;
    int btype; /* VT_BYTE / VT_INT / etc. — width of the store */
  } RseSlot;

  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== REDUNDANT STORE ELIMINATION START ===");

  RseSlot active[RSE_MAX_ACTIVE];
  int active_count = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Block boundary: pending stores may be live on the other side → flush. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      active_count = 0;
      continue;
    }

    /* READ check: any instruction that uses a local address as src1 or src2
     * keeps the corresponding pending store alive. */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (src1.is_local)
      {
        int64_t off = irop_get_imm64_ex(ir, src1);
        const Sym *sym = irop_get_sym_ex(ir, src1);
        for (int k = 0; k < active_count; k++)
        {
          if (active[k].sym == sym && active[k].offset == off)
          {
            active[k] = active[--active_count]; /* evict (swap-remove) */
            break;
          }
        }
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      if (src2.is_local)
      {
        int64_t off = irop_get_imm64_ex(ir, src2);
        const Sym *sym = irop_get_sym_ex(ir, src2);
        for (int k = 0; k < active_count; k++)
        {
          if (active[k].sym == sym && active[k].offset == off)
          {
            active[k] = active[--active_count];
            break;
          }
        }
      }
    }

    /* STORE to a local non-addr-taken address. */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local)
        continue;

      /* Skip addr-taken locals: they may be read through a pointer. */
      int32_t addr_vr = irop_get_vreg(dest);
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
          continue;
      }

      int64_t off = irop_get_imm64_ex(ir, dest);
      const Sym *sym = irop_get_sym_ex(ir, dest);

      int store_btype = dest.btype;

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
        active_count++;
      }
      /* else: table full — skip this store conservatively */
    }
  }

  LOG_IR_GEN("=== REDUNDANT STORE ELIMINATION END: %d changes ===", changes);

  return changes;
#undef RSE_MAX_ACTIVE
}

int tcc_ir_opt_sl_forward_ex(IROptCtx *ctx) { return tcc_ir_opt_sl_forward(ctx->ir); }
int tcc_ir_opt_deref_fwd_ex(IROptCtx *ctx) { return tcc_ir_opt_deref_fwd(ctx->ir); }
int tcc_ir_opt_entry_store_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_entry_store_prop(ctx->ir); }
int tcc_ir_opt_store_redundant_ex(IROptCtx *ctx) { return tcc_ir_opt_store_redundant(ctx->ir); }
