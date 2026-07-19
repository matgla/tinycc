/*
 *  TCC IR - Entry-block store propagation (flat, pre-SSA)
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

/* Forward constant entry-block stores into deref/indexed loads; entry stores dominate all code. */
int tcc_ir_opt_entry_store_prop(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
    return 0;

  /* Phase 1: collect constant stores from the entry BB (up to the first jump target). */
#define MAX_ENTRY_STORES 64
  struct
  {
    int64_t offset;
    IROperand value;
    int btype;
    int idx;
  } estores[MAX_ENTRY_STORES];
  int estore_count = 0;
#define ESTORE_COMPACT() \
  do { int _v = 0; \
       for (int k = 0; k < estore_count; k++) \
         if (estores[k].offset != 0x7FFFFFFFLL) estores[_v++] = estores[k]; \
       estore_count = _v; } while (0)

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
          estores[found].idx = i;
        }
        else
        {
          estores[estore_count].offset = off;
          estores[estore_count].value = imm;
          estores[estore_count].btype = IROP_BTYPE_INT32;
          estores[estore_count].idx = i;
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

    /* Only const or stack-address values; anything else invalidates the prior entry (last-write-wins). */
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

    /* Last-write-wins */
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
      estores[found].idx = i;
    }
    else if (estore_count < MAX_ENTRY_STORES)
    {
      estores[estore_count].offset = off;
      estores[estore_count].value = src1;
      estores[estore_count].btype = irop_get_btype(dest);
      estores[estore_count].idx = i;
      estore_count++;
    }
  }

  LOG_IR_GEN("ENTRY_STORE_PROP: %d entry-BB stores collected", estore_count);
  if (estore_count == 0)
    return 0;

  /* Phase 1.5: invalidate entries for offsets written anywhere after the entry BB. */
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
      int64_t soff = 0;
      int have_soff = 0;
      if (eq->op == TCCIR_OP_STORE)
      {
        if (sd.is_local && sd.is_lval && !sd.is_llocal && irop_get_tag(sd) == IROP_TAG_STACKOFF)
        {
          soff = irop_get_stack_offset(sd);
          have_soff = 1;
        }
      }
      else if (eq->op == TCCIR_OP_STORE_INDEXED)
      {
        /* disp_fusion lowers `st.field = x` to a STORE_INDEXED over a non-lval stack base; still invalidates the slot. */
        if (sd.is_local && !sd.is_lval && !sd.is_llocal && irop_get_tag(sd) == IROP_TAG_STACKOFF)
        {
          IROperand idx = tcc_ir_op_get_src2(ir, eq);
          IROperand scale_op = tcc_ir_op_get_scale(ir, eq);
          if (irop_is_immediate(idx) && !idx.is_sym && irop_is_immediate(scale_op))
          {
            soff = irop_get_stack_offset(sd) + (irop_get_imm64_ex(ir, idx) << irop_get_imm64_ex(ir, scale_op));
            have_soff = 1;
          }
        }
      }
      if (!have_soff)
        continue;
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset == soff)
        {
          /* Once overwritten after the entry BB, a later load may see the new value, not the initializer. */
          LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (rewritten at i=%d)", (long long)soff, j);
          estores[k].offset = 0x7FFFFFFFLL;
        }
      }
    }
    /* Invalidate stores whose address is taken anywhere: it may escape and be written through. */
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
    /* A stack addr passed to a call escapes the whole object; invalidate its entry stores within the object extent. */
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->op != TCCIR_OP_FUNCPARAMVAL && eq->op != TCCIR_OP_FUNCPARAMVOID)
        continue;
      IROperand arg = tcc_ir_op_get_src1(ir, eq);
      int64_t esc_off;
      int have = 0;
      if (arg.is_local && !arg.is_lval && irop_get_tag(arg) == IROP_TAG_STACKOFF)
      {
        esc_off = irop_get_stack_offset(arg);
        have = 1;
      }
      else if (irop_get_tag(arg) == IROP_TAG_VREG && irop_get_vreg(arg) >= 0)
      {
        int di = tcc_ir_find_defining_instruction(ir, irop_get_vreg(arg), j);
        if (di >= 0)
        {
          IRQuadCompact *dq = &ir->compact_instructions[di];
          if (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_LEA)
          {
            IROperand ds = tcc_ir_op_get_src1(ir, dq);
            if (ds.is_local && !ds.is_lval && irop_get_tag(ds) == IROP_TAG_STACKOFF)
            {
              esc_off = irop_get_stack_offset(ds);
              have = 1;
            }
          }
        }
      }
      if (!have)
        continue;
      /* Bound invalidation to the escaped object's extent [esc_off, obj_end): a BLOCK_COPY range, else the contiguous run of entry stores. */
      int64_t obj_end = esc_off;
      for (int r = 0; r < bc_range_count; r++)
      {
        if (esc_off >= bc_ranges[r].base && esc_off < bc_ranges[r].base + bc_ranges[r].size)
        {
          int64_t e = bc_ranges[r].base + bc_ranges[r].size;
          if (e > obj_end)
            obj_end = e;
        }
      }
      if (obj_end == esc_off)
      {
        int grew = 1;
        while (grew)
        {
          grew = 0;
          for (int k = 0; k < estore_count; k++)
          {
            if (estores[k].offset == 0x7FFFFFFFLL)
              continue;
            if (estores[k].offset >= esc_off && estores[k].offset <= obj_end + 64)
            {
              int64_t e = estores[k].offset + 8;
              if (e > obj_end)
              {
                obj_end = e;
                grew = 1;
              }
            }
          }
        }
      }
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset != 0x7FFFFFFFLL && estores[k].offset >= esc_off &&
            estores[k].offset < obj_end)
        {
          LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (call-arg escape [%lld,%lld) at i=%d)",
                     (long long)estores[k].offset, (long long)esc_off, (long long)obj_end, j);
          estores[k].offset = 0x7FFFFFFFLL;
        }
      }
    }

    ESTORE_COMPACT();
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
  /* TEMPs holding `array_base + RUNTIME_index` pointers, kept out of lea_map so exact-offset forwarding is unperturbed. */
  int64_t *rt_base = tcc_mallocz(sizeof(int64_t) * (max_tmp + 1));
  uint8_t *rt_valid = tcc_mallocz(max_tmp + 1);
  /* VAR analogue of rt_base: a `&arr[RUNTIME_index]` pointer materialised into a VAR local. */
  int64_t *var_rt_base = tcc_mallocz(sizeof(int64_t) * (max_var + 1));
  uint8_t *var_rt_valid = tcc_mallocz(max_var + 1);

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
        /* Same address landing directly in a VAR alias pointer: record it for later store invalidation. */
        else if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_var)
          {
            var_lea_map[p].offset = irop_get_stack_offset(src1);
            var_lea_map[p].valid = 1;
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
        /* Carry a runtime array base into the VAR alias pointer too. */
        else if (sp <= max_tmp && rt_valid[sp] && dp <= max_var)
        {
          var_rt_base[dp] = rt_base[sp];
          var_rt_valid[dp] = 1;
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
        /* A TEMP copied from a VAR runtime array pointer carries the runtime base. */
        else if (sp <= max_var && var_rt_valid[sp] && dp <= max_tmp)
        {
          rt_base[dp] = var_rt_base[sp];
          rt_valid[dp] = 1;
        }
      }
      /* ASSIGN: TEMP <-- TEMP → a plain pointer copy carries the resolved stack offset. */
      else if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP && s1_vr >= 0 &&
               TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP && !s1.is_lval)
      {
        int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (sp <= max_tmp && lea_map[sp].valid && dp <= max_tmp)
        {
          lea_map[dp].offset = lea_map[sp].offset;
          lea_map[dp].valid = 1;
        }
        else if (sp <= max_tmp && rt_valid[sp] && dp <= max_tmp)
        {
          rt_base[dp] = rt_base[sp];
          rt_valid[dp] = 1;
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
            else if (sp <= max_tmp && rt_valid[sp])
            {
              /* runtime array pointer + const stays a runtime pointer into the same array; carry the base. */
              rt_base[dp] = rt_base[sp];
              rt_valid[dp] = 1;
            }
          }
          else if (s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_STACKOFF && irop_is_immediate(s2) &&
                   !s2.is_sym)
          {
            lea_map[dp].offset = irop_get_stack_offset(s1) + irop_get_imm64_ex(ir, s2);
            lea_map[dp].valid = 1;
          }
          else if (!irop_is_immediate(s2))
          {
            /* base + RUNTIME index → record the array base (separate map). */
            int64_t base;
            int have = 0;
            if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
              if (sp <= max_tmp && lea_map[sp].valid) { base = lea_map[sp].offset; have = 1; }
              else if (sp <= max_tmp && rt_valid[sp]) { base = rt_base[sp]; have = 1; }
            }
            else if (s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_STACKOFF)
            {
              base = irop_get_stack_offset(s1);
              have = 1;
            }
            if (have) { rt_base[dp] = base; rt_valid[dp] = 1; }
          }
        }
      }
      /* VAR-dest alias pointer `V = &arr[1]`: record the constant offset for later store invalidation. */
      else if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (dp <= max_var)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          int32_t s1_vr = irop_get_vreg(s1);
          if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP &&
              irop_is_immediate(s2) && !s2.is_sym)
          {
            int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
            if (sp <= max_tmp && lea_map[sp].valid)
            {
              var_lea_map[dp].offset = lea_map[sp].offset + irop_get_imm64_ex(ir, s2);
              var_lea_map[dp].valid = 1;
            }
            else if (sp <= max_tmp && rt_valid[sp])
            {
              /* VAR analogue: `V = <runtime array pointer> + const` stays a runtime pointer into the same array. */
              var_rt_base[dp] = rt_base[sp];
              var_rt_valid[dp] = 1;
            }
          }
          else if (s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_STACKOFF &&
                   irop_is_immediate(s2) && !s2.is_sym)
          {
            var_lea_map[dp].offset = irop_get_stack_offset(s1) + irop_get_imm64_ex(ir, s2);
            var_lea_map[dp].valid = 1;
          }
          /* `V = base + RUNTIME index` into a VAR: record the array base for runtime-store invalidation. */
          else if (!irop_is_immediate(s2))
          {
            int64_t base;
            int have = 0;
            if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
              if (sp <= max_tmp && lea_map[sp].valid) { base = lea_map[sp].offset; have = 1; }
              else if (sp <= max_tmp && rt_valid[sp]) { base = rt_base[sp]; have = 1; }
            }
            else if (s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_STACKOFF)
            {
              base = irop_get_stack_offset(s1);
              have = 1;
            }
            if (have) { var_rt_base[dp] = base; var_rt_valid[dp] = 1; }
          }
        }
      }
    }
  }

  /* Phase 2.5: invalidate entries for later pointer stores through LEA-resolved TEMPs (idx-gated). */
  {
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->op != TCCIR_OP_STORE && eq->op != TCCIR_OP_STORE_INDEXED && eq->op != TCCIR_OP_STORE_POSTINC)
        continue;
      IROperand sd = tcc_ir_op_get_dest(ir, eq);
      if (eq->op == TCCIR_OP_STORE_INDEXED && sd.is_local && !sd.is_lval && !sd.is_llocal &&
          irop_get_tag(sd) == IROP_TAG_STACKOFF)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, eq);
        if (!irop_is_immediate(s2) || s2.is_sym)
          continue;
        IROperand scale_op = ir->iroperand_pool[eq->operand_base + 3];
        int scale = (int)irop_get_imm64_ex(ir, scale_op);
        int64_t soff = irop_get_stack_offset(sd) + (irop_get_imm64_ex(ir, s2) << scale);
        for (int k = 0; k < estore_count; k++)
        {
          if (j > estores[k].idx && estores[k].offset == soff)
          {
            LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (direct indexed store at i=%d)",
                       (long long)soff, j);
            estores[k].offset = 0x7FFFFFFFLL;
          }
        }
        continue;
      }
      if (sd.is_local)
        continue;
      /* STORE_INDEXED / STORE_POSTINC always write through their base; disp_fusion clears is_lval, so don't skip on it. */
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
        IROperand scale_op = ir->iroperand_pool[eq->operand_base + 3];
        int scale = (int)irop_get_imm64_ex(ir, scale_op);
        soff += (irop_get_imm64_ex(ir, s2) << scale);
      }
      for (int k = 0; k < estore_count; k++)
      {
        if (j > estores[k].idx && estores[k].offset == soff)
        {
          LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (ptr store via LEA at i=%d)", (long long)soff, j);
          estores[k].offset = 0x7FFFFFFFLL;
        }
      }
    }
    ESTORE_COMPACT();
  }
  if (estore_count == 0)
  {
    tcc_free(lea_map);
    tcc_free(var_lea_map);
    tcc_free(rt_base);
    tcc_free(rt_valid);
    tcc_free(var_rt_base);
    tcc_free(var_rt_valid);
    return 0;
  }

  /* Collect call-escaped base offsets (function-call params only) for the Phase 3b safety check. */
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

  /* BLOCK_COPY range: if an address within it escapes via a call param, invalidate all entries in the range. */
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
  ESTORE_COMPACT();

  /* Phase 2.6: a runtime-indexed array store may hit any element, so invalidate every entry at or above the array base. */
  {
    int any_inval = 0;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->op != TCCIR_OP_STORE && eq->op != TCCIR_OP_STORE_INDEXED && eq->op != TCCIR_OP_STORE_POSTINC)
        continue;
      IROperand sd = tcc_ir_op_get_dest(ir, eq);
      int64_t base = 0x7FFFFFFFLL;
      int32_t dv = irop_get_vreg(sd);
      if (eq->op == TCCIR_OP_STORE_INDEXED)
      {
        /* Runtime address when the index is runtime OR the base is a runtime array pointer; skip only the fully-constant case (Phase 2.5 handles it). */
        IROperand s2 = tcc_ir_op_get_src2(ir, eq);
        int imm_index = irop_is_immediate(s2) && !s2.is_sym;
        if (sd.is_local && irop_get_tag(sd) == IROP_TAG_STACKOFF)
        {
          if (imm_index)
            continue; /* fully constant address — Phase 2.5 handles it precisely */
          base = irop_get_stack_offset(sd);
        }
        else if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
        {
          int dp = TCCIR_DECODE_VREG_POSITION(dv);
          if (dp <= max_tmp && lea_map[dp].valid)
          {
            if (imm_index)
              continue; /* constant base + constant index — Phase 2.5 handles it */
            base = lea_map[dp].offset;
          }
          else if (dp <= max_tmp && rt_valid[dp])
            base = rt_base[dp]; /* runtime base: address is runtime even if index is immediate */
        }
      }
      else /* plain STORE / STORE_POSTINC through a TEMP / VAR deref */
      {
        if (sd.is_local) continue; /* direct stores handled by Phase 1 */
        if (dv < 0) continue;
        int dp = TCCIR_DECODE_VREG_POSITION(dv);
        if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
        {
          if (dp <= max_tmp && rt_valid[dp]) base = rt_base[dp];
        }
        else if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
        {
          /* `*p = ...` directly through a VAR runtime array pointer. */
          if (dp <= max_var && var_rt_valid[dp]) base = var_rt_base[dp];
        }
      }
      if (base == 0x7FFFFFFFLL)
        continue;
      for (int k = 0; k < estore_count; k++)
        if (estores[k].offset != 0x7FFFFFFFLL && estores[k].offset >= base)
        {
          LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (runtime store at i=%d, base=%lld)",
                     (long long)estores[k].offset, j, (long long)base);
          estores[k].offset = 0x7FFFFFFFLL;
          any_inval = 1;
        }
    }
    if (any_inval)
      ESTORE_COMPACT();
  }

  /* Phase 3: forward entry-BB stores into deref operands (T***DEREF*** via the LEA map). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    for (int si = 0; si < 2; si++)
    {
      if (si == 0 && !irop_config[q->op].has_src1)
        continue;
      if (si == 1 && !irop_config[q->op].has_src2)
        continue;

      IROperand src = (si == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);

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
        /* The collected store must precede the load it feeds. */
        if (i <= estores[k].idx)
          continue;

        /* Reuse the original stored operand to preserve its type encoding. */
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

  /* Phase 3b: forward entry-BB stores into LOAD_INDEXED (base via LEA map) as ASSIGN. */
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
    IROperand scale_op = ir->iroperand_pool[q->operand_base + 3];
    int scale = (int)irop_get_imm64_ex(ir, scale_op);
    int64_t eff_off = base_off + (irop_get_imm64_ex(ir, li_src2) << scale);

    /* If the LEA base's address escaped, a call may have modified it; skip forwarding. */
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
      /* The collected store must precede the load it is forwarded into. */
      if (i <= estores[k].idx)
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
  tcc_free(rt_base);
  tcc_free(rt_valid);
  tcc_free(var_rt_base);
  tcc_free(var_rt_valid);

  return changes;
}
int tcc_ir_opt_entry_store_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_entry_store_prop(ctx->ir); }
