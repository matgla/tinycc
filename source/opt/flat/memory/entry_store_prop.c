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
  if (!sym || sym->a.tentative)
    return NULL; /* a tentative definition's bytes are not its value yet */

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


/* ---- Address escape scan (Phase 1.5) ---------------------------------------
 * Which vregs may hold the address of a frame object, and through which object.
 * An address reaching a call, a store as the VALUE, a return, asm or any other
 * use that is not a plain address derivation or a memory access through it
 * escapes its whole object: the receiver can reach every byte of it. */
enum { ESP_NONE = 0, ESP_ONE, ESP_MANY };
typedef struct
{
  int64_t off; /* an offset inside the object (state ESP_ONE) */
  uint8_t state;
} EspAddr;

typedef struct
{
  EspAddr *v[4]; /* indexed by TCCIR_VREG_TYPE */
  int max[4];
} EspAddrMap;

static EspAddr *esp_slot(EspAddrMap *m, int32_t vr)
{
  if (vr < 0)
    return NULL;
  int t = TCCIR_DECODE_VREG_TYPE(vr), p = TCCIR_DECODE_VREG_POSITION(vr);
  if (t < 1 || t > 3 || p > m->max[t])
    return NULL;
  return &m->v[t][p];
}

/* The vreg whose own value OP reads (not a dereference of it), or -1. */
static int32_t esp_value_vreg(IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_sym)
    return -1;
  int tag = irop_get_tag(op);
  if (tag == IROP_TAG_VREG)
    return op.is_lval ? -1 : vr;
  if (tag == IROP_TAG_STACKOFF) /* a VAR/PARAM named through its home; is_llocal derefs it */
    return op.is_llocal ? -1 : vr;
  return -1;
}

/* The address OP evaluates to, as far as frame objects go. */
static EspAddr esp_operand_addr(EspAddrMap *m, IROperand op)
{
  EspAddr a = {0, ESP_NONE};
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_get_vreg(op) < 0)
  {
    if (op.is_local && !op.is_lval && !op.is_llocal)
    {
      a.off = irop_get_stack_offset(op);
      a.state = ESP_ONE;
    }
    return a;
  }
  EspAddr *s = esp_slot(m, esp_value_vreg(op));
  if (s)
    a = *s;
  return a;
}

static int esp_merge(EspAddr *into, EspAddr a)
{
  if (a.state == ESP_NONE || into->state == ESP_MANY)
    return 0;
  if (into->state == ESP_NONE)
  {
    *into = a;
    return 1;
  }
  if (a.state == ESP_MANY || a.off != into->off)
  {
    into->state = ESP_MANY;
    return 1;
  }
  return 0;
}

/* An op whose vreg result is (an address derived from) its address operand. */
static int esp_is_derivation(TccIrOp op)
{
  return op == TCCIR_OP_ASSIGN || op == TCCIR_OP_LEA || op == TCCIR_OP_ADD || op == TCCIR_OP_SUB ||
         op == TCCIR_OP_SELECT || op == TCCIR_OP_LOAD || op == TCCIR_OP_STORE;
}

/* Calls fn(ctx, addr, i) for every escaping use of a frame address.  Returns 0
 * when the vreg map could not be built (caller must assume everything escapes). */
static int esp_for_each_escape(TCCIRState *ir, void (*fn)(void *, EspAddr, int), void *ctx)
{
  int n = ir->next_instruction_index;
  EspAddrMap m;
  memset(&m, 0, sizeof m);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 3; k++)
    {
      int has = k == 0 ? irop_config[q->op].has_dest : k == 1 ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2;
      if (!has)
        continue;
      IROperand o = tcc_ir_op_get_slot(ir, q, k);
      int32_t vr = irop_get_vreg(o);
      if (vr < 0)
        continue;
      int t = TCCIR_DECODE_VREG_TYPE(vr), p = TCCIR_DECODE_VREG_POSITION(vr);
      if (t >= 1 && t <= 3 && p > m.max[t])
        m.max[t] = p;
    }
  }
  for (int t = 1; t <= 3; t++)
    m.v[t] = tcc_mallocz(sizeof(EspAddr) * (m.max[t] + 1));

  /* Propagate to a fixpoint: a VAR can be defined after a use (loops). The
     lattice NONE < ONE < MANY bounds the rounds. */
  int changed = 1, rounds = 0;
  while (changed && rounds++ < 64)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (!esp_is_derivation(q->op) || !irop_config[q->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!irop_dest_defines_vreg(d))
        continue;
      EspAddr *ds = esp_slot(&m, irop_get_vreg(d));
      if (!ds)
        continue;
      if (irop_config[q->op].has_src1)
        changed |= esp_merge(ds, esp_operand_addr(&m, tcc_ir_op_get_src1(ir, q)));
      if (irop_config[q->op].has_src2)
        changed |= esp_merge(ds, esp_operand_addr(&m, tcc_ir_op_get_src2(ir, q)));
    }
  }
  int ok = !changed;

  for (int i = 0; ok && i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    TccIrOp op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    int defines = irop_config[op].has_dest && irop_dest_defines_vreg(tcc_ir_op_get_dest(ir, q));
    if (esp_is_derivation(op) && defines)
      continue; /* the address moved into a tracked vreg; its uses are checked there */
    /* Comparing an address leaks nothing. */
    if (op == TCCIR_OP_CMP || op == TCCIR_OP_TEST_ZERO)
      continue;
    for (int k = 0; k < 3; k++)
    {
      int has = k == 0 ? irop_config[op].has_dest : k == 1 ? irop_config[op].has_src1 : irop_config[op].has_src2;
      if (!has)
        continue;
      IROperand o = tcc_ir_op_get_slot(ir, q, k);
      EspAddr a = esp_operand_addr(&m, o);
      if (a.state == ESP_NONE)
        continue;
      /* The address used as the base of an access is not leaked by it. */
      if ((k == 0 && (op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_STORE_POSTINC)) ||
          (k == 1 && (op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_LOAD_POSTINC || op == TCCIR_OP_BLOCK_COPY ||
                      op == TCCIR_OP_PREFETCH)))
        continue;
      /* A BLOCK_COPY destination is written whole: Phase 1.5 handles a direct
         StackLoc one; one through a vreg falls through as an escape. */
      if (k == 0 && op == TCCIR_OP_BLOCK_COPY && irop_get_vreg(o) < 0)
        continue;
      fn(ctx, a, i);
    }
  }

  for (int t = 1; t <= 3; t++)
    tcc_free(m.v[t]);
  return ok;
}

typedef struct
{
  int64_t *off;
  int count, cap;
  int many; /* an address of an unknown object escaped */
} EspEscapes;

static void esp_collect(void *ctx, EspAddr a, int i)
{
  EspEscapes *e = ctx;
  (void)i;
  if (a.state == ESP_MANY)
  {
    e->many = 1;
    return;
  }
  for (int k = 0; k < e->count; k++)
    if (e->off[k] == a.off)
      return;
  if (e->count == e->cap)
  {
    e->cap = e->cap ? 2 * e->cap : 8;
    e->off = tcc_realloc(e->off, sizeof(int64_t) * e->cap);
  }
  e->off[e->count++] = a.off;
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
    /* Store dest was an anonymous stack slot (no backing vreg).  Only those may be
       matched by offset against a *direct* StackLoc read (Phase 3c): a VAR-backed
       local reports stack offset 0 regardless of position, so offset alone does not
       identify it. */
    int anon_slot;
    int is_unsigned;
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

  /* Index one past the last instruction Phase 1 examined: the first jump/jump-target, or
     n for a straight-line function, or wherever the MAX_ENTRY_STORES cap stopped it.
     Phase 1.5 rescans from here, so it must not cover the entry stores themselves. */
  int entry_scan_end = 0;
  int i;
  for (i = 0; i < n && estore_count < MAX_ENTRY_STORES; i++)
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

      if (!bc_dest.is_local || irop_get_tag(bc_dest) != IROP_TAG_STACKOFF)
        continue;
      if (!tcc_ir_op_src2_is_imm(ir, q))
        continue;

      int64_t base_off = irop_get_stack_offset(bc_dest);
      int total_size = (int)tcc_ir_op_src2_imm(ir, q);
      size_t avail = 0;
      const uint8_t *data = total_size > 0 ? ir_opt_get_rodata_bytes(ir, bc_src, &avail) : NULL;

      /* A BLOCK_COPY this scan cannot expand into per-word entry stores is still a
       * write.  Drop every entry store it covers -- keeping one would let a later
       * load from the range be forwarded a value the copy has since overwritten.
       * A sub-word size reaches here via the strcpy fold (strlen+1 need not be a
       * whole number of words), so this is not a can't-happen path. */
      if (total_size <= 0 || (total_size & 3) || total_size > 256 || !data ||
          avail < (size_t)total_size)
      {
        if (total_size > 0)
          for (int k = 0; k < estore_count; k++)
            if (estores[k].offset >= base_off && estores[k].offset < base_off + total_size)
              estores[k].offset = 0x7FFFFFFFLL;
        ESTORE_COMPACT();
        continue;
      }

      /* The copy overwrites the whole range, so every entry inside it is stale --
       * including sub-word ones the per-word rewrite below would step over.  Drop
       * them all first, then record what the copy leaves behind. */
      for (int k = 0; k < estore_count; k++)
        if (estores[k].offset >= base_off && estores[k].offset < base_off + total_size)
          estores[k].offset = 0x7FFFFFFFLL;
      ESTORE_COMPACT();

      int nwords = total_size / 4;
      for (int w = 0; w < nwords && estore_count < MAX_ENTRY_STORES; w++)
      {
        int32_t val = (int32_t)read32le((unsigned char *)(data + w * 4));
        int64_t off = base_off + w * 4;
        IROperand imm = irop_make_imm32(-1, val, IROP_BTYPE_INT32);
        int bc_anon = (irop_get_vreg(bc_dest) < 0);
        estores[estore_count].offset = off;
        estores[estore_count].value = imm;
        estores[estore_count].btype = IROP_BTYPE_INT32;
        estores[estore_count].idx = i;
        estores[estore_count].anon_slot = bc_anon;
        estores[estore_count].is_unsigned = 0;
        estore_count++;
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
    {
      /* Kill every *overlapping* entry at a different offset, not just the exact-offset
         one: a `double` initializer stores 8 bytes at -96 while the preceding zero-init
         wrote 4-byte words at -96 and -92, and the stale -92 word would otherwise still
         read as 0.  Exact-offset entries are handled by last-write-wins below. */
      int store_sz = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
      int64_t st_end = off + (store_sz > 0 ? store_sz : 1);
      for (int k = 0; k < estore_count; k++)
      {
        if (estores[k].offset == 0x7FFFFFFFLL || estores[k].offset == off)
          continue;
        int esz = ir_opt_store_btype_size_bytes(estores[k].btype);
        int64_t e_end = estores[k].offset + (esz > 0 ? esz : 1);
        if (estores[k].offset < st_end && off < e_end)
          estores[k].offset = 0x7FFFFFFFLL;
      }
    }

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
      estores[found].anon_slot = (irop_get_vreg(dest) < 0);
      estores[found].is_unsigned = dest.is_unsigned;
    }
    else if (estore_count < MAX_ENTRY_STORES)
    {
      estores[estore_count].offset = off;
      estores[estore_count].value = src1;
      estores[estore_count].btype = irop_get_btype(dest);
      estores[estore_count].idx = i;
      estores[estore_count].anon_slot = (irop_get_vreg(dest) < 0);
      estores[estore_count].is_unsigned = dest.is_unsigned;
      estore_count++;
    }
  }

  entry_scan_end = i;

  LOG_IR_GEN("ENTRY_STORE_PROP: %d entry-BB stores collected", estore_count);
  if (estore_count == 0)
    return 0;

  /* Phase 1.5: invalidate entries for offsets written anywhere after the entry BB. */
  {
    int entry_bb_end = entry_scan_end;
    for (int j = entry_bb_end; j < n; j++)
    {
      IRQuadCompact *eq = &ir->compact_instructions[j];
      if (eq->op == TCCIR_OP_BLOCK_COPY)
      {
        IROperand bcd = tcc_ir_op_get_dest(ir, eq);
        if (bcd.is_local && irop_get_tag(bcd) == IROP_TAG_STACKOFF && tcc_ir_op_src2_is_imm(ir, eq))
        {
          int64_t bbase = irop_get_stack_offset(bcd);
          int64_t bsz = tcc_ir_op_src2_imm(ir, eq);
          for (int k = 0; k < estore_count; k++)
          {
            if (estores[k].offset >= bbase && estores[k].offset < bbase + bsz)
              estores[k].offset = 0x7FFFFFFFLL;
          }
        }
        continue;
      }
      /* Inline asm ("=m" outputs, memory clobbers) and non-local control write unseen memory. */
      if (ir_op_has(eq->op, IR_HZ_ASM | IR_HZ_NONLOCAL | IR_HZ_VLA))
      {
        for (int k = 0; k < estore_count; k++)
          estores[k].offset = 0x7FFFFFFFLL;
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
          IROperand scale_op = tcc_ir_op_get_scale(ir, eq);
          if (tcc_ir_op_src2_is_imm(ir, eq) && !tcc_ir_op_src2_is_sym(ir, eq) && irop_is_immediate(scale_op))
          {
            soff = irop_get_stack_offset(sd) + (tcc_ir_op_src2_imm(ir, eq) << irop_get_imm64_ex(ir, scale_op));
            have_soff = 1;
          }
        }
      }
      if (!have_soff)
        continue;
      {
        /* Overlap, not exact offset: an 8-byte store at -96 also kills a 4-byte entry at -92. */
        int ssz = ir_opt_store_btype_size_bytes(irop_get_btype(sd));
        int64_t s_end = soff + (ssz > 0 ? ssz : 1);
        for (int k = 0; k < estore_count; k++)
        {
          if (estores[k].offset == 0x7FFFFFFFLL)
            continue;
          int esz = ir_opt_store_btype_size_bytes(estores[k].btype);
          int64_t e_end = estores[k].offset + (esz > 0 ? esz : 1);
          if (estores[k].offset < s_end && soff < e_end)
          {
            /* Once overwritten after the entry BB, a later load may see the new value, not the initializer. */
            LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (rewritten at i=%d)", (long long)estores[k].offset, j);
            estores[k].offset = 0x7FFFFFFFLL;
          }
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
        IROperand op = tcc_ir_op_get_src1_or_2(ir, eq, si != 0);
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
    /* A stack address that escapes (passed to a call, stored as a value, ...) --
       directly or through any VAR/TEMP copy chain -- escapes its whole object:
       invalidate every entry store inside it. */
    {
      EspEscapes esc = {0};
      int all = !esp_for_each_escape(ir, esp_collect, &esc) || esc.many;
      for (int e = 0; !all && e < esc.count; e++)
      {
        int lo, hi;
        if (!tcc_ir_frame_object_at(ir, (int)esc.off[e], &lo, &hi))
        {
          all = 1; /* extent unknown */
          break;
        }
        for (int k = 0; k < estore_count; k++)
        {
          if (estores[k].offset != 0x7FFFFFFFLL && estores[k].offset >= lo && estores[k].offset < hi)
          {
            LOG_IR_GEN("ENTRY_STORE_PROP: invalidated off=%lld (object [%d,%d) escapes)",
                       (long long)estores[k].offset, lo, hi);
            estores[k].offset = 0x7FFFFFFFLL;
          }
        }
      }
      if (all && esc.count + esc.many > 0)
      {
        for (int k = 0; k < estore_count; k++)
          estores[k].offset = 0x7FFFFFFFLL;
      }
      tcc_free(esc.off);
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
      int32_t vr = tcc_ir_op_dest_vreg(ir, q);
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
    /* The binding is only a MAY-alias: some vreg on the chain that produced it
       has more than one definition, so at a given use the pointer need not hold
       this address at all (`s = cond ? param : &local;`, or a `p++` that already
       moved it).  Such an entry is still good enough to *invalidate* an entry
       store (Phases 2.5/2.6 want may-alias), but forwarding a stored value into
       a load through it (Phases 3/3b/3c) would be unsound. */
    int maybe;
  } SimpleLeaEntry;

  /* Definition counts, used only to set `maybe` above.  A write *through* a
     pointer (`T***DEREF*** <-- v`) is not a definition of the pointer; a VAR
     itself as the destination is (a STACKOFF lvalue).  Testing is_lval alone
     dropped the latter: `for (p = y; p != e; p++) s += *p` counted one of p's
     two definitions, and the walk got the last element's value forwarded into
     every iteration. */
  uint8_t *tmp_defs = tcc_mallocz(max_tmp + 1);
  uint8_t *var_defs = tcc_mallocz(max_var + 1);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(d);
    if (!irop_dest_defines_vreg(d))
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(vr);
    if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && p <= max_tmp && tmp_defs[p] < 2)
      tmp_defs[p]++;
    else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && p <= max_var && var_defs[p] < 2)
      var_defs[p]++;
  }

  SimpleLeaEntry *lea_map = tcc_mallocz(sizeof(SimpleLeaEntry) * (max_tmp + 1));
  SimpleLeaEntry *var_lea_map = tcc_mallocz(sizeof(SimpleLeaEntry) * (max_var + 1));
  /* TEMPs holding `array_base + RUNTIME_index` pointers, kept out of lea_map so exact-offset forwarding is unperturbed. */
  int64_t *rt_base = tcc_mallocz(sizeof(int64_t) * (max_tmp + 1));
  uint8_t *rt_valid = tcc_mallocz(max_tmp + 1);
  /* VAR analogue of rt_base: a `&arr[RUNTIME_index]` pointer materialised into a VAR local. */
  int64_t *var_rt_base = tcc_mallocz(sizeof(int64_t) * (max_var + 1));
  uint8_t *var_rt_valid = tcc_mallocz(max_var + 1);

/* Record `map[p] = off`, inheriting the may-alias taint of whatever produced the
   address (src_maybe) and adding this vreg's own if it has several definitions. */
#define LEA_SET(map, defs, p, off, src_maybe)                  \
  do {                                                         \
    int _p = (p);                                              \
    (map)[_p].offset = (off);                                  \
    (map)[_p].valid = 1;                                       \
    (map)[_p].maybe = ((src_maybe) || (defs)[_p] > 1) ? 1 : 0; \
  } while (0)


  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* ASSIGN/LEA with Addr[StackLoc[X]] source → record in LEA map.
       STORE too, and not only for symmetry: an inlined pointer parameter lands as
       `V <-- Addr[StackLoc[X]] [STORE]`, and missing it loses the base for every
       runtime-indexed store through V — which Phase 2.6 then fails to invalidate. */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_STORE)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (src1.is_local && !src1.is_lval && irop_get_tag(src1) == IROP_TAG_STACKOFF)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int32_t vr = irop_get_vreg(dest);
        /* A STORE with a TEMP dest writes *through* the temp; it does not define it. */
        if (q->op != TCCIR_OP_STORE && vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_tmp)
          {
            LEA_SET(lea_map, tmp_defs, p, irop_get_stack_offset(src1), 0);
          }
        }
        /* Same address landing directly in a VAR alias pointer: record it for later store invalidation. */
        else if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_var)
          {
            LEA_SET(var_lea_map, var_defs, p, irop_get_stack_offset(src1), 0);
          }
        }
      }
    }

    /* STORE/ASSIGN: VAR <-- LEA_temp → propagate into var_lea_map */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN)
    {
      int32_t d_vr = tcc_ir_op_dest_vreg(ir, q);
      int32_t s1_vr = tcc_ir_op_src1_vreg(ir, q);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR && s1_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (sp <= max_tmp && lea_map[sp].valid && dp <= max_var)
        {
          LEA_SET(var_lea_map, var_defs, dp, lea_map[sp].offset, lea_map[sp].maybe);
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
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = tcc_ir_op_dest_vreg(ir, q);
      int32_t s1_vr = irop_get_vreg(s1);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP && s1_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
        int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
        if (sp <= max_var && var_lea_map[sp].valid && dp <= max_tmp)
        {
          LEA_SET(lea_map, tmp_defs, dp, var_lea_map[sp].offset, var_lea_map[sp].maybe);
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
          LEA_SET(lea_map, tmp_defs, dp, lea_map[sp].offset, lea_map[sp].maybe);
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
          int32_t s1_vr = irop_get_vreg(s1);
          if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP && tcc_ir_op_src2_is_imm(ir, q) &&
              !tcc_ir_op_src2_is_sym(ir, q))
          {
            int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
            if (sp <= max_tmp && lea_map[sp].valid)
            {
              LEA_SET(lea_map, tmp_defs, dp, lea_map[sp].offset + tcc_ir_op_src2_imm(ir, q), lea_map[sp].maybe);
            }
            else if (sp <= max_tmp && rt_valid[sp])
            {
              /* runtime array pointer + const stays a runtime pointer into the same array; carry the base. */
              rt_base[dp] = rt_base[sp];
              rt_valid[dp] = 1;
            }
          }
          else if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_VAR &&
                   tcc_ir_op_src2_is_imm(ir, q) && !tcc_ir_op_src2_is_sym(ir, q))
          {
            /* Same, one indirection out: the base pointer lives in a VAR alias. */
            int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
            if (sp <= max_var && var_lea_map[sp].valid)
            {
              LEA_SET(lea_map, tmp_defs, dp, var_lea_map[sp].offset + tcc_ir_op_src2_imm(ir, q), var_lea_map[sp].maybe);
            }
            else if (sp <= max_var && var_rt_valid[sp])
            {
              rt_base[dp] = var_rt_base[sp];
              rt_valid[dp] = 1;
            }
          }
          else if (s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_STACKOFF && tcc_ir_op_src2_is_imm(ir, q) &&
                   !tcc_ir_op_src2_is_sym(ir, q))
          {
            LEA_SET(lea_map, tmp_defs, dp, irop_get_stack_offset(s1) + tcc_ir_op_src2_imm(ir, q), 0);
          }
          else if (!tcc_ir_op_src2_is_imm(ir, q))
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
            else if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_VAR)
            {
              int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
              if (sp <= max_var && var_lea_map[sp].valid) { base = var_lea_map[sp].offset; have = 1; }
              else if (sp <= max_var && var_rt_valid[sp]) { base = var_rt_base[sp]; have = 1; }
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
          int32_t s1_vr = irop_get_vreg(s1);
          if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP &&
              tcc_ir_op_src2_is_imm(ir, q) && !tcc_ir_op_src2_is_sym(ir, q))
          {
            int sp = TCCIR_DECODE_VREG_POSITION(s1_vr);
            if (sp <= max_tmp && lea_map[sp].valid)
            {
              LEA_SET(var_lea_map, var_defs, dp, lea_map[sp].offset + tcc_ir_op_src2_imm(ir, q), lea_map[sp].maybe);
            }
            else if (sp <= max_tmp && rt_valid[sp])
            {
              /* VAR analogue: `V = <runtime array pointer> + const` stays a runtime pointer into the same array. */
              var_rt_base[dp] = rt_base[sp];
              var_rt_valid[dp] = 1;
            }
          }
          else if (s1.is_local && !s1.is_lval && irop_get_tag(s1) == IROP_TAG_STACKOFF &&
                   tcc_ir_op_src2_is_imm(ir, q) && !tcc_ir_op_src2_is_sym(ir, q))
          {
            LEA_SET(var_lea_map, var_defs, dp, irop_get_stack_offset(s1) + tcc_ir_op_src2_imm(ir, q), 0);
          }
          /* `V = base + RUNTIME index` into a VAR: record the array base for runtime-store invalidation. */
          else if (!tcc_ir_op_src2_is_imm(ir, q))
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
        if (!tcc_ir_op_src2_is_imm(ir, eq) || tcc_ir_op_src2_is_sym(ir, eq))
          continue;
        IROperand scale_op = ir->iroperand_pool[eq->operand_base + 3];
        int scale = (int)irop_get_imm64_ex(ir, scale_op);
        int64_t soff = irop_get_stack_offset(sd) + (tcc_ir_op_src2_imm(ir, eq) << scale);
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
        if (!tcc_ir_op_src2_is_imm(ir, eq))
          continue;
        IROperand scale_op = ir->iroperand_pool[eq->operand_base + 3];
        int scale = (int)irop_get_imm64_ex(ir, scale_op);
        soff += (tcc_ir_op_src2_imm(ir, eq) << scale);
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
    tcc_free(tmp_defs);
    tcc_free(var_defs);
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
        int imm_index = tcc_ir_op_src2_is_imm(ir, eq) && !tcc_ir_op_src2_is_sym(ir, eq);
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

      IROperand src = tcc_ir_op_get_src1_or_2(ir, q, si != 0);

      if (!src.is_lval)
        continue;
      /* A volatile read returns what the object holds, not what was stored. */
      if (tcc_ir_access_is_volatile(ir, src))
        continue;

      /* Resolve the address through LEA map */
      int32_t vr = irop_get_vreg(src);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;

      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p > max_tmp || !lea_map[p].valid)
        continue;
      /* May-alias only (multiply-defined pointer): invalidation may use it,
         forwarding may not. */
      if (lea_map[p].maybe)
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
    if (tcc_ir_access_is_volatile(ir, li_src1))
      continue; /* a volatile read is never forwarded (the base carries the mark) */

    int bp = TCCIR_DECODE_VREG_POSITION(base_vr);
    if (bp > max_tmp || !lea_map[bp].valid)
      continue;
    if (lea_map[bp].maybe)
      continue; /* see Phase 3 */

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
      tcc_ir_set_src2_none(ir, i);

      if (estores[k].value.is_local && !estores[k].value.is_lval && irop_get_tag(estores[k].value) == IROP_TAG_STACKOFF)
      {
        int32_t d_vr = tcc_ir_op_dest_vreg(ir, q);
        if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int dp = TCCIR_DECODE_VREG_POSITION(d_vr);
          if (dp <= max_tmp)
          {
            LEA_SET(lea_map, tmp_defs, dp, irop_get_stack_offset(estores[k].value), 0);
          }
        }
      }

      LOG_IR_GEN("ENTRY_STORE_PROP: i=%d LOAD_INDEXED forwarded at eff_off=%lld", i, (long long)eff_off);
      changes++;
      break;
    }
  }

  /* Phase 3c: forward entry-BB stores into *direct* StackLoc lvalue reads.
     Phase 3 only rewrites derefs whose address sits in a TEMP (the LEA map); by the time
     this pass runs, earlier address-folding passes have already collapsed most of those
     into a plain `StackLoc[X]` lvalue operand, which Phase 3 then skips.  Those reads are
     the common shape for `local.field` / `vec[const]` after csfwd, and leaving them
     un-forwarded costs the whole downstream const-fold chain (soft-float helper calls in
     particular) once a call or a loop join has cleared the BB-local sl_forward state.

     The invalidation analysis above is read-form independent, so this reuses exactly the
     safety net Phase 3 already relies on.  Extra guards here: both sides must be anonymous
     stack slots (a VAR-backed local reports offset 0 regardless of position), the value
     types must match exactly, and struct/func operands are left alone. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (!src1.is_local || !src1.is_lval || src1.is_llocal || src1.is_complex)
      continue;
    if (irop_get_tag(src1) != IROP_TAG_STACKOFF)
      continue;
    if (irop_get_vreg(src1) >= 0)
      continue; /* VAR/TEMP-backed: its stack offset does not identify the slot */
    if (tcc_ir_access_is_volatile(ir, src1))
      continue; /* `volatile char b[4]`: the slot's reads all stay */

    int read_btype = irop_get_btype(src1);
    if (read_btype == IROP_BTYPE_STRUCT || read_btype == IROP_BTYPE_FUNC)
      continue; /* aggregates are not a single-operand forward */

    int64_t read_off = irop_get_stack_offset(src1);

    for (int k = 0; k < estore_count; k++)
    {
      if (estores[k].offset != read_off)
        continue;
      if (i <= estores[k].idx)
        continue;
      if (!estores[k].anon_slot)
        continue;
      if (estores[k].btype != read_btype)
        continue;
      if (estores[k].is_unsigned != (int)src1.is_unsigned)
        continue;
      if (!irop_is_immediate(estores[k].value))
        continue;

      if (q->op != TCCIR_OP_FUNCPARAMVAL)
        q->op = TCCIR_OP_ASSIGN;
      {
        int pool_off = q->operand_base + irop_config[q->op].has_dest;
        ir->iroperand_pool[pool_off] = estores[k].value;
      }
      LOG_IR_GEN("ENTRY_STORE_PROP: i=%d direct StackLoc read forwarded at off=%lld", i, (long long)read_off);
      changes++;
      break;
    }
  }

#undef LEA_SET

  tcc_free(tmp_defs);
  tcc_free(var_defs);
  tcc_free(lea_map);
  tcc_free(var_lea_map);
  tcc_free(rt_base);
  tcc_free(rt_valid);
  tcc_free(var_rt_base);
  tcc_free(var_rt_valid);

  return changes;
}

// Immutable, unescaped parameter homes can use their incoming word values.
#define PHF_MAX 32
int tcc_ir_opt_param_home_fwd(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  if (n == 0 || ir->inline_asm_count || ir->func_has_label_addr || tcc_ir_calls_returns_twice(ir))
    return 0;
  int has_call = 0;
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_FUNCCALLVOID || ir->compact_instructions[i].op == TCCIR_OP_FUNCCALLVAL)
      has_call = 1;

  struct PhfHome
  {
    int off, idx, bad, keep;
    IROperand p;
  } h[PHF_MAX];
  int nh = 0;
  for (int i = 0; i < n && nh < PHF_MAX; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->is_jump_target && i > 0)
      break;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      break;
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q), s = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(d) != IROP_TAG_STACKOFF || !d.is_local || !d.is_lval || d.is_llocal ||
        irop_get_vreg(d) >= 0 || irop_get_btype(d) != IROP_BTYPE_INT32 || irop_is_64bit(d) || d.is_complex ||
        tcc_ir_access_is_volatile(ir, d))
      continue;
    int32_t pv = irop_get_vreg(s);
    if (pv < 0 || TCCIR_DECODE_VREG_TYPE(pv) != TCCIR_VREG_TYPE_PARAM || s.is_lval || s.is_local || s.is_llocal ||
        irop_get_btype(s) != IROP_BTYPE_INT32 || irop_is_64bit(s) || s.is_complex)
      continue;
    const int off = irop_get_stack_offset(d);
    if (off & 3)
      continue;
    int dup = 0;
    for (int k = 0; k < nh; k++)
      if (h[k].off == off)
        dup = h[k].bad = 1;
    if (!dup)
      h[nh++] = (struct PhfHome){.off = off, .idx = i, .bad = 0, .p = s};
  }
  if (!nh)
    return 0;
  int lo = h[0].off, hi = h[0].off + 4;
  for (int k = 1; k < nh; k++)
  {
    if (h[k].off < lo)
      lo = h[k].off;
    if (h[k].off + 4 > hi)
      hi = h[k].off + 4;
  }

  /* Every other reference to the home area. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int entry = 0;
    for (int k = 0; k < nh; k++)
      entry |= h[k].idx == i;
    if (entry)
      continue;
    const int store_class = q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                            q->op == TCCIR_OP_STORE_POSTINC;
    for (int s = 0; s < 4; s++)
    {
      IROperand op;
      if (s == 0) { if (!irop_config[q->op].has_dest) continue; op = tcc_ir_op_get_dest(ir, q); }
      else if (s == 1) { if (!irop_config[q->op].has_src1) continue; op = tcc_ir_op_get_src1(ir, q); }
      else if (s == 2) { if (!irop_config[q->op].has_src2) continue; op = tcc_ir_op_get_src2(ir, q); }
      else { if (!ir_op_has(q->op, IROP_A_SLOT3)) continue; op = ir->iroperand_pool[q->operand_base + 3]; }
      /* A parameter vreg written anywhere is not the entry value. */
      int32_t vr = irop_get_vreg(op);
      if (s == 0 && vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_PARAM &&
          q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
        for (int k = 0; k < nh; k++)
          if (irop_get_vreg(h[k].p) == vr)
            h[k].bad = 1;
      if (irop_get_tag(op) != IROP_TAG_STACKOFF || !op.is_local || op.is_llocal || vr >= 0)
        continue;
      const int o = irop_get_stack_offset(op);
      if (!op.is_lval)
      {
        /* its address: anything could be done through it */
        if (o >= lo && o < hi)
          return 0;
        continue;
      }
      int w = irop_is_64bit(op) ? 8 : ir_opt_store_btype_size_bytes(irop_get_btype(op));
      /* A complex read observes both components, including the next home. */
      if (op.is_complex)
        w *= 2;
      if (w <= 0)
        w = irop_get_btype(op) == IROP_BTYPE_STRUCT ? hi - lo : 4;
      if (o + w <= lo || o >= hi)
        continue;
      if (s == 0 && (store_class || q->op == TCCIR_OP_BLOCK_COPY))
      {
        if (q->op != TCCIR_OP_STORE)
          return 0; /* an indexed or block write into the homes */
        for (int k = 0; k < nh; k++)
          if (o < h[k].off + 4 && o + w > h[k].off)
            h[k].bad = 1;
      }
      else if (s == 0)
        return 0; /* some other write of the slot */
      else
        for (int k = 0; k < nh; k++)
          if (o < h[k].off + 4 && o + w > h[k].off &&
              (o != h[k].off || w != 4 || irop_get_btype(op) != IROP_BTYPE_INT32 ||
               op.is_complex || tcc_ir_access_is_volatile(ir, op)))
            h[k].keep = 1;
    }
  }

  if (has_call) {
    IRLoops *loops = tcc_ir_detect_loops(ir);
    int candidates = 0;
    for (int k = 0; k < nh; ++k)
      candidates += !h[k].bad;
    int budget = loops && loops->num_loops ? tcc_ir_loop_call_promotion_budget(ir, loops) : 0;
    tcc_ir_free_loops(loops);
    if (candidates > budget)
      return 0;
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int entry = 0;
    for (int k = 0; k < nh; k++)
      entry |= h[k].idx == i;
    if (entry)
      continue;
    for (int s = 1; s < 4; s++)
    {
      IROperand op;
      if (s == 1) { if (!irop_config[q->op].has_src1) continue; op = tcc_ir_op_get_src1(ir, q); }
      else if (s == 2) { if (!irop_config[q->op].has_src2) continue; op = tcc_ir_op_get_src2(ir, q); }
      else { if (!ir_op_has(q->op, IROP_A_SLOT3)) continue; op = ir->iroperand_pool[q->operand_base + 3]; }
      if (irop_get_tag(op) != IROP_TAG_STACKOFF || !op.is_local || !op.is_lval || op.is_llocal ||
          irop_get_vreg(op) >= 0 || irop_get_btype(op) != IROP_BTYPE_INT32 || irop_is_64bit(op) || op.is_complex ||
          tcc_ir_access_is_volatile(ir, op))
        continue;
      const int o = irop_get_stack_offset(op);
      for (int k = 0; k < nh; k++)
      {
        if (h[k].bad || h[k].off != o)
          continue;
        IROperand p = h[k].p;
        if (s == 1)
          tcc_ir_op_set_src1(ir, q, p);
        else if (s == 2)
          tcc_ir_op_set_src2(ir, q, p);
        else
          ir->iroperand_pool[q->operand_base + 3] = p;
        changes++;
        break;
      }
    }
  }
  for (int k = 0; k < nh; k++)
    if (!h[k].bad && !h[k].keep)
    {
      ir->compact_instructions[h[k].idx].op = TCCIR_OP_NOP;
      changes++;
    }
  return changes;
}
