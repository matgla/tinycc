/*
 *  TCC IR - block-copy initializer materialization (flat, pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A memset(stack, 0, N) followed by a run of constant stores into the same
 * object is an initializer: `struct T x = {...}` or a compound literal.  The
 * run is replaced by one BLOCK_COPY from an image of the initialized bytes in
 * .rodata (or .data when it holds relocations and .rodata is shared), which
 * the backend emits as LDM/STM or a memcpy call.  Identical images are emitted
 * once per section, so a table the source repeats costs its bytes once.
 *
 * Each store writes its own width into the image; a run ends at the first
 * instruction that is not a constant store inside the object (a load, a
 * volatile or unaligned address store, one overwriting a relocation). */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"

typedef struct
{
  int off;       /* byte offset in the image */
  Sym *sym;      /* symbol whose address is stored there */
  int32_t addend;
} BciReloc;

typedef struct BciImage
{
  struct BciImage *next;
  TCCState *s1;
  Section *sec;
  addr_t sec_off;
  int size;
  uint32_t hash;
  BciReloc *rel;
  int nrel;
  Sym *sym;
} BciImage;

/* Images already emitted, for reuse by an identical initializer anywhere in
 * the output. */
static BciImage *bci_images;

static uint32_t bci_hash(const uint8_t *p, int n, const BciReloc *rel, int nrel)
{
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++)
    h = (h ^ p[i]) * 16777619u;
  for (int i = 0; i < nrel; i++)
    h = (h ^ (uint32_t)rel[i].off ^ (uint32_t)(uintptr_t)rel[i].sym) * 16777619u;
  return h;
}

static Sym *bci_image_sym(Section *sec, const uint8_t *bytes, int size, const BciReloc *rel, int nrel)
{
  uint32_t h = bci_hash(bytes, size, rel, nrel);
  BciImage **pp = &bci_images;
  while (*pp)
  {
    BciImage *e = *pp;
    if (e->s1 != tcc_state)
    {
      /* A previous compilation state's images: its sections are gone. */
      *pp = e->next;
      tcc_free(e->rel);
      tcc_free(e);
      continue;
    }
    if (e->sec == sec && e->size == size && e->hash == h && e->nrel == nrel &&
        !memcmp(sec->data + e->sec_off, bytes, size) && (!nrel || !memcmp(e->rel, rel, sizeof *rel * nrel)))
      return e->sym;
    pp = &e->next;
  }

  addr_t off = section_add(sec, size, 4);
  memcpy(sec->data + off, bytes, size);
  for (int i = 0; i < nrel; i++)
    greloc(sec, rel[i].sym, off + rel[i].off, R_DATA_PTR);
  CType ctype;
  ctype.t = VT_PTR | VT_CONST;
  ctype.ref = NULL;
  Sym *sym = get_sym_ref(&ctype, sec, off, size);

  BciImage *e = tcc_mallocz(sizeof *e);
  e->s1 = tcc_state;
  e->sec = sec;
  e->sec_off = off;
  e->size = size;
  e->hash = h;
  e->nrel = nrel;
  if (nrel)
  {
    e->rel = tcc_malloc(sizeof *rel * nrel);
    memcpy(e->rel, rel, sizeof *rel * nrel);
  }
  e->sym = sym;
  e->next = bci_images;
  bci_images = e;
  return sym;
}

/* Bytes a STORE writes, from its destination's type. */
static int bci_store_width(IROperand dest)
{
  if (irop_is_64bit(dest))
    return 8;
  switch (irop_get_btype(dest))
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    return 4;
  default:
    return 0;
  }
}

int tcc_ir_opt_block_copy_init(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  int cap = 0, nstores;
  int *store_indices = NULL;
  uint8_t *image = NULL, *is_reloc = NULL;
  BciReloc *rel = NULL;
  int image_cap = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;
    if (strcmp(name, "__aeabi_memset") != 0 && strcmp(name, "memset") != 0)
      continue;

    /* Param order is __aeabi_memset(dest, size, fill_value). */
    IROperand param_dest, param_size, param_fill;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &param_dest))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 1, &param_size))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 2, &param_fill))
      continue;

    if (irop_get_tag(param_fill) != IROP_TAG_IMM32)
      continue;
    if ((int)irop_get_imm64_ex(ir, param_fill) != 0)
      continue;

    if (irop_get_tag(param_size) != IROP_TAG_IMM32)
      continue;
    int total_size = (int)irop_get_imm64_ex(ir, param_size);
    if (total_size <= 0 || (total_size & 3))
      continue;

    if (irop_get_tag(param_dest) != IROP_TAG_STACKOFF || irop_get_vreg(param_dest) >= 0)
      continue;
    int base_offset = (int)irop_get_imm64_ex(ir, param_dest);
    /* The inline copy moves words (LDM/STM, LDRD/STRD), which fault unaligned. */
    if (base_offset & 3)
      continue;

    if (total_size > image_cap)
    {
      image_cap = total_size;
      image = tcc_realloc(image, image_cap);
      is_reloc = tcc_realloc(is_reloc, image_cap);
    }
    memset(image, 0, total_size);
    memset(is_reloc, 0, total_size);
    nstores = 0;
    int nrel = 0;

    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[j];
      if (sq->op == TCCIR_OP_NOP)
        continue;
      if (sq->op != TCCIR_OP_STORE)
        break;

      IROperand st_dest = tcc_ir_op_get_dest(ir, sq);
      IROperand st_src = tcc_ir_op_get_src1(ir, sq);

      if (irop_get_tag(st_dest) != IROP_TAG_STACKOFF || !st_dest.is_local || !st_dest.is_lval ||
          st_dest.is_llocal || irop_get_vreg(st_dest) >= 0 || tcc_ir_access_is_volatile(ir, st_dest))
        break;

      int rel_off = (int)irop_get_imm64_ex(ir, st_dest) - base_offset;
      int st_size = bci_store_width(st_dest);
      if (st_size == 0 || rel_off < 0 || rel_off + st_size > total_size)
        break;

      int src_tag = irop_get_tag(st_src);
      if (st_src.is_lval || st_src.is_local || st_src.is_llocal || irop_get_vreg(st_src) >= 0)
        break;
      if (src_tag != IROP_TAG_SYMREF && src_tag != IROP_TAG_IMM32 && src_tag != IROP_TAG_I64 &&
          src_tag != IROP_TAG_F32 && src_tag != IROP_TAG_F64)
        break;
      if (src_tag == IROP_TAG_SYMREF && (st_size != 4 || (rel_off & 3)))
        break;
      IRPoolSymref *symref = src_tag == IROP_TAG_SYMREF ? irop_get_symref_ex(ir, st_src) : NULL;
      if (src_tag == IROP_TAG_SYMREF && (!symref || !symref->sym))
        break;

      /* A relocation's word is written only whole, by the symref itself. */
      int hits_reloc = 0;
      for (int b = rel_off; b < rel_off + st_size; b++)
        hits_reloc |= is_reloc[b];
      if (hits_reloc)
        break;

      if (nstores == cap)
      {
        cap = cap ? cap * 2 : 256;
        store_indices = tcc_realloc(store_indices, sizeof(int) * cap);
        rel = tcc_realloc(rel, sizeof(BciReloc) * cap);
      }
      store_indices[nstores++] = j;

      if (symref)
      {
        write32le(image + rel_off, symref->addend);
        memset(is_reloc + rel_off, 1, 4);
        rel[nrel].off = rel_off;
        rel[nrel].sym = symref->sym;
        rel[nrel].addend = symref->addend;
        nrel++;
      }
      else
      {
        uint64_t val = (uint64_t)irop_get_imm64_ex(ir, st_src);
        for (int b = 0; b < st_size; b++)
          image[rel_off + b] = (uint8_t)(val >> (8 * b));
      }
    }

    if (nstores < 2)
      continue;
    /* Past the old 1 KiB limit, only when the image costs no more than the
     * code it replaces: a constant store takes 4-10 bytes of Thumb-2. */
    if (total_size > 1024 && nstores * 6 < total_size)
      continue;

    /* A block holding a symref needs a relocation, so it cannot go in shared
     * read-only .rodata; place it in the writable data segment instead. */
    Section *block_sec = (nrel && tcc_state->share_rodata) ? data_section : rodata_section;
    Sym *rodata_sym = bci_image_sym(block_sec, image, total_size, rel, nrel);

    IROperand bc_dest = irop_make_stackoff(-1, base_offset, 1, 0, 0, IROP_BTYPE_INT32);
    uint32_t sym_pool_idx = tcc_ir_pool_add_symref(ir, rodata_sym, 0, 0);
    IROperand bc_src = irop_make_symref(-1, sym_pool_idx, 0, 0, 1, IROP_BTYPE_INT32);
    IROperand bc_size = irop_make_imm32(-1, total_size, VT_INT);

    int pool_base = tcc_ir_iroperand_pool_add(ir, bc_dest);
    tcc_ir_iroperand_pool_add(ir, bc_src);
    tcc_ir_iroperand_pool_add(ir, bc_size);

    IRQuadCompact *first_store = &ir->compact_instructions[store_indices[0]];
    first_store->op = TCCIR_OP_BLOCK_COPY;
    first_store->operand_base = pool_base;

    for (int s = 1; s < nstores; s++)
      ir->compact_instructions[store_indices[s]].op = TCCIR_OP_NOP;

    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_NOP;

    changes++;
  }

  tcc_free(store_indices);
  tcc_free(rel);
  tcc_free(image);
  tcc_free(is_reloc);
  return changes;
}
