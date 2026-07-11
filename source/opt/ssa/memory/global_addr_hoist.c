/*
 *  TCC SSA opt - hoist repeated global-address materializations into a vreg
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"
#include "global_addr_hoist.h"
#include <string.h>

/* Max distinct global addresses parked per function: past this the extra
 * addresses spill to the stack (no cheaper than the pool reload they replace).
 * Measured suite-wide, 2 beats the prior 4. */
#define GAH_MAX_HOISTS 2

/* In a large function (many live values of its own competing for callee-saved
 * registers) the 2nd parked address only pays off when it is much hotter: it
 * must reload at least this many times, vs GAH_MIN_RELOADS for the 1st or in a
 * small function.  Separates strlen-4's good 2nd hoist (reload 16) from
 * memset-3's spilling one (reload 6). */
#define GAH_LARGE_FN_INSTRS 460
#define GAH_LARGE_2ND_RELOADS 12

/* Don't reserve a callee-saved register for an address first used only after
 * this many calls: the reservation cost across those calls outweighs the reload
 * savings (the value sits parked, spilling, before it does any work). */
#define GAH_MAX_CALLS_BEFORE 12

/* Only hoist an address reloaded from the pool at least this many times across
 * calls: a strong "genuinely hot" signal.  Marginal (3-4 reload) addresses are
 * skipped because they compete for the same callee-saved registers as the hot
 * ones and tip the allocator into spills that erase the win — measured to add
 * up to +1108 bytes on a single bitfield-struct function at a threshold of 3,
 * while 5 keeps the strlen-4 win (its hot addresses reload 8-13x) and nets
 * negative across the torture suite. */
#define GAH_MIN_RELOADS 5

/* slot codes for a symref operand within an instruction */
enum { GAH_SLOT_SRC1 = 0, GAH_SLOT_SRC2 = 1, GAH_SLOT_ACCUM = 2 };

typedef struct {
  int idx;       /* instruction index */
  uint8_t slot;  /* GAH_SLOT_* */
} GahUse;

typedef struct {
  Sym *sym;
  int32_t addend;
  GahUse *uses;
  int nuses, cap;
  int reload_estimate;
  int32_t tbase; /* assigned hoist vreg, -1 until selected */
} GahClass;

/* Ops whose src1/src2/accum symref operand is a plain value the backend
 * materializes into a register (so replacing the symref with a vreg holding
 * that address is identical).  A whitelist, not a blacklist: ops that require a
 * literal symref operand (BLOCK_COPY's memcpy source, a FUNCCALL branch target,
 * SWITCH data tables) or that don't carry an address value are simply absent. */
static int gah_op_hoistable(TccIrOp op)
{
  switch (op) {
  case TCCIR_OP_ADD: case TCCIR_OP_ADC_USE: case TCCIR_OP_ADC_GEN:
  case TCCIR_OP_SUB: case TCCIR_OP_SUBC_USE: case TCCIR_OP_SUBC_GEN:
  case TCCIR_OP_MUL: case TCCIR_OP_MLA: case TCCIR_OP_UMULL: case TCCIR_OP_SMULL:
  case TCCIR_OP_DIV: case TCCIR_OP_UMOD: case TCCIR_OP_IMOD:
  case TCCIR_OP_PDIV: case TCCIR_OP_UDIV:
  case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR:
  case TCCIR_OP_SHL: case TCCIR_OP_SAR: case TCCIR_OP_SHR: case TCCIR_OP_ROR:
  case TCCIR_OP_CMP:
  case TCCIR_OP_LOAD: case TCCIR_OP_STORE:
  case TCCIR_OP_LOAD_INDEXED: case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_LEA:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_RETURNVALUE:
    return 1;
  default:
    return 0;
  }
}

/* A hoistable symref operand: a global (non-local) symbol address materialized
 * via a literal-pool load.  is_lval is intentionally NOT constrained: for a
 * dereference use we still hoist the *address* (a link-time constant) and let
 * the rewritten operand keep its own is_lval bit (load-through-register). */
static int gah_operand_hoistable(const TCCIRState *ir, IROperand op, Sym **out_sym, int32_t *out_addend)
{
  IRPoolSymref *sr;
  if (!op.is_sym || op.is_local)
    return 0;
  if (irop_get_tag(op) != IROP_TAG_SYMREF)
    return 0;
  sr = irop_get_symref_ex(ir, op);
  if (!sr || !sr->sym)
    return 0;
  if (sr->flags & IRPOOL_SYMREF_LOCAL)
    return 0;
  *out_sym = sr->sym;
  *out_addend = sr->addend;
  return 1;
}

static GahClass *gah_find_or_add(GahClass **classes, int *n, int *cap, Sym *sym, int32_t addend)
{
  for (int i = 0; i < *n; i++)
    if ((*classes)[i].sym == sym && (*classes)[i].addend == addend)
      return &(*classes)[i];
  if (*n >= *cap) {
    *cap = *cap ? *cap * 2 : 16;
    *classes = tcc_realloc(*classes, *cap * sizeof(GahClass));
  }
  GahClass *c = &(*classes)[*n];
  memset(c, 0, sizeof(*c));
  c->sym = sym;
  c->addend = addend;
  c->tbase = -1;
  (*n)++;
  return c;
}

static void gah_class_add_use(GahClass *c, int idx, uint8_t slot)
{
  if (c->nuses >= c->cap) {
    c->cap = c->cap ? c->cap * 2 : 4;
    c->uses = tcc_realloc(c->uses, c->cap * sizeof(GahUse));
  }
  c->uses[c->nuses].idx = idx;
  c->uses[c->nuses].slot = slot;
  c->nuses++;
}

static void gah_try_slot(const TCCIRState *ir, GahClass **classes, int *n, int *cap,
                         IROperand op, int idx, uint8_t slot)
{
  Sym *sym;
  int32_t addend;
  if (!gah_operand_hoistable(ir, op, &sym, &addend))
    return;
  GahClass *c = gah_find_or_add(classes, n, cap, sym, addend);
  gah_class_add_use(c, idx, slot);
}

int tcc_ir_ssa_opt_global_addr_hoist(TCCIRState *ir)
{
  int n_instrs = ir->next_instruction_index;
  GahClass *classes = NULL;
  int nclasses = 0, ccap = 0;
  int *call_prefix = NULL; /* call_prefix[i] = #calls at index < i */
  int changes = 0;

  /* Prefix count of call instructions, for the reload estimate. */
  call_prefix = tcc_malloc((n_instrs + 1) * sizeof(int));
  call_prefix[0] = 0;
  for (int i = 0; i < n_instrs; i++) {
    TccIrOp op = ir->compact_instructions[i].op;
    int is_call = (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID);
    call_prefix[i + 1] = call_prefix[i] + is_call;
  }
  if (call_prefix[n_instrs] == 0) {
    tcc_free(call_prefix);
    return 0; /* no calls => nothing to keep alive across a call */
  }

  /* Collect symref-address uses in value slots (src1/src2, MLA accum) of ops
   * that materialize the address into a register. */
  for (int i = 0; i < n_instrs; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!gah_op_hoistable(q->op))
      continue;
    if (irop_config[q->op].has_src1)
      gah_try_slot(ir, &classes, &nclasses, &ccap, tcc_ir_op_get_src1(ir, q), i, GAH_SLOT_SRC1);
    if (irop_config[q->op].has_src2)
      gah_try_slot(ir, &classes, &nclasses, &ccap, tcc_ir_op_get_src2(ir, q), i, GAH_SLOT_SRC2);
    if (q->op == TCCIR_OP_MLA)
      gah_try_slot(ir, &classes, &nclasses, &ccap, tcc_ir_op_get_accum(ir, q), i, GAH_SLOT_ACCUM);
  }

  /* Reload estimate: 1 for the first materialization, +1 per later use that
   * follows a call since the previous use of the same address.  Uses are
   * collected in ascending instruction order. */
  for (int c = 0; c < nclasses; c++) {
    GahClass *cl = &classes[c];
    int est = cl->nuses ? 1 : 0;
    for (int k = 1; k < cl->nuses; k++) {
      int a = cl->uses[k - 1].idx, b = cl->uses[k].idx;
      if (call_prefix[b] - call_prefix[a + 1] > 0)
        est++;
    }
    cl->reload_estimate = est;
  }

  /* Select up to GAH_MAX_HOISTS hot classes, highest savings first.  In a large
   * function the 2nd+ parked address must clear a higher reload bar: it competes
   * for the callee-saved registers the allocator would otherwise keep the
   * function's own values in, and only a much hotter address earns that. */
  int is_large = (n_instrs > GAH_LARGE_FN_INSTRS);
  for (int picked = 0; picked < GAH_MAX_HOISTS; picked++) {
    int thresh = (picked >= 1 && is_large) ? GAH_LARGE_2ND_RELOADS : GAH_MIN_RELOADS;
    int best = -1;
    for (int c = 0; c < nclasses; c++) {
      if (classes[c].tbase != -1 || classes[c].reload_estimate < thresh)
        continue;
      if (classes[c].nuses && call_prefix[classes[c].uses[0].idx] > GAH_MAX_CALLS_BEFORE)
        continue;
      if (best < 0 || classes[c].reload_estimate > classes[best].reload_estimate)
        best = c;
    }
    if (best < 0)
      break;
    classes[best].tbase = tcc_ir_vreg_alloc_temp(ir);
  }

  /* Rewrite use operands to reference the hoist vreg (indices stable). Each use
   * keeps its own is_lval/btype/sign so a dereference stays a load-through. */
  for (int c = 0; c < nclasses; c++) {
    GahClass *cl = &classes[c];
    if (cl->tbase == -1)
      continue;
    for (int k = 0; k < cl->nuses; k++) {
      IRQuadCompact *q = &ir->compact_instructions[cl->uses[k].idx];
      IROperand orig;
      switch (cl->uses[k].slot) {
      case GAH_SLOT_SRC1: orig = tcc_ir_op_get_src1(ir, q); break;
      case GAH_SLOT_SRC2: orig = tcc_ir_op_get_src2(ir, q); break;
      default:            orig = tcc_ir_op_get_accum(ir, q); break;
      }
      IROperand nw = irop_make_vreg(cl->tbase, orig.btype);
      nw.is_lval = orig.is_lval;
      nw.is_unsigned = orig.is_unsigned;
      nw.is_static = orig.is_static;
      nw.is_complex = orig.is_complex;
      switch (cl->uses[k].slot) {
      case GAH_SLOT_SRC1: tcc_ir_op_set_src1(ir, q, nw); break;
      case GAH_SLOT_SRC2: tcc_ir_op_set_src2(ir, q, nw); break;
      default:            tcc_ir_op_set_accum(ir, q, nw); break;
      }
    }
  }

  /* Prepend the address materializations at entry so each dominates all its
   * uses.  Done after all rewrites, so shifting indices is harmless. */
  {
    int pos = 0;
    for (int c = 0; c < nclasses; c++) {
      GahClass *cl = &classes[c];
      if (cl->tbase == -1)
        continue;
      uint32_t sidx = tcc_ir_pool_add_symref(ir, cl->sym, cl->addend, 0);
      IROperand src = irop_make_symref(-1, sidx, 0, 0, 0, IROP_BTYPE_INT32);
      IROperand dst = irop_make_vreg(cl->tbase, IROP_BTYPE_INT32);
      if (insert_instr_at(ir, pos, TCCIR_OP_ASSIGN, dst, src, irop_make_none()) == 0) {
        pos++;
        changes++;
      }
    }
  }

  for (int c = 0; c < nclasses; c++)
    tcc_free(classes[c].uses);
  tcc_free(classes);
  tcc_free(call_prefix);

  return changes;
}
