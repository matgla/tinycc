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
enum { GAH_SLOT_SRC1 = 0, GAH_SLOT_SRC2 = 1, GAH_SLOT_ACCUM = 2, GAH_SLOT_DEST = 3 };

/* ---- expensive-immediate (pool-constant) classes ----
 *
 * All three variants also hoist plain 32-bit constants that the backend can
 * only materialize with a literal-pool load (csmix-style hash multipliers:
 * 0x9e3779b1 is reloaded 42x in one fuzz main).  A const class is keyed as
 * sym == NULL with the value in `addend`, so selection, budgets, thresholds
 * and rewrites are shared with address classes and both kinds compete for the
 * same hoist slots.  The machine level cannot fix these: imm_cache is fully
 * reset at every jump target and every call (ir/codegen.c), so only a live
 * range the allocator plans around survives either.
 *
 * Values below 65536 in magnitude are excluded even when MOV-unencodable:
 * MOVW covers all non-negative ones, and small negatives are reachable by the
 * op-specific imm12 rewrites (ADD->SUB, CMP->CMN) at most sites, where a
 * register read would be no better. */
#define GAH_CONST_MIN_MAG 65536

/* In a register-tight function (no free-window headroom left) parking a
 * constant displaces the allocator's own values into spill slots, so the
 * class must be hot enough that the removed reloads dwarf that: 389_fuzz's
 * 240-instr main lost 19 instructions parking two ~6-use constants, while
 * 252_fuzz's main wins 52 parking a 42-use and a 29-use one. */
#define GAH_CONST_TIGHT_BUDGET 2
#define GAH_CONST_TIGHT_RELOADS 16

/* Set once per pass entry; lets TCC_DISABLE_PASS=ssa:const_pool_hoist isolate
 * the constant half of each variant for bisection and A/B measurement. */
static int gah_consts_on;

static int gah_operand_hoistable_const(IROperand op, int32_t *out_val)
{
  int32_t v;
  if (op.is_sym || op.is_local || op.is_llocal || op.is_lval)
    return 0;
  if (irop_get_tag(op) != IROP_TAG_IMM32)
    return 0;
  if (op.btype != IROP_BTYPE_INT32)
    return 0;
  v = irop_get_imm32(op);
  if (v > -GAH_CONST_MIN_MAG && v < GAH_CONST_MIN_MAG)
    return 0;
  if (!tcc_gen_machine_const_needs_pool(v))
    return 0;
  *out_val = v;
  return 1;
}

/* Value slots where a pool constant may be replaced by a vreg: strictly the
 * sites the backend materializes into a scratch register today.  Narrower
 * than gah_op_hoistable: LOAD/LEA/indexed operands and STORE dest are
 * addresses whose immediate shapes (offsets, absolute derefs) must survive,
 * and shift amounts never pass the magnitude gate anyway. */
static int gah_const_slot_ok(TccIrOp op, uint8_t slot)
{
  switch (op) {
  case TCCIR_OP_ADD: case TCCIR_OP_ADC_USE: case TCCIR_OP_ADC_GEN:
  case TCCIR_OP_SUB: case TCCIR_OP_SUBC_USE: case TCCIR_OP_SUBC_GEN:
  case TCCIR_OP_MUL: case TCCIR_OP_UMULL: case TCCIR_OP_SMULL:
  case TCCIR_OP_DIV: case TCCIR_OP_UMOD: case TCCIR_OP_IMOD:
  case TCCIR_OP_PDIV: case TCCIR_OP_UDIV:
  case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR:
  case TCCIR_OP_CMP:
    return slot == GAH_SLOT_SRC1 || slot == GAH_SLOT_SRC2;
  case TCCIR_OP_MLA:
    return slot == GAH_SLOT_SRC1 || slot == GAH_SLOT_SRC2 ||
           slot == GAH_SLOT_ACCUM;
  case TCCIR_OP_SHL: case TCCIR_OP_SAR: case TCCIR_OP_SHR: case TCCIR_OP_ROR:
  case TCCIR_OP_STORE:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_RETURNVALUE:
    return slot == GAH_SLOT_SRC1;
  default:
    return 0;
  }
}

static int gah_operand_hoistable(const TCCIRState *ir, IROperand op,
                                 Sym **out_sym, int32_t *out_addend);

/* Shared classifier: an address class (sym != NULL) or, when enabled, a
 * pool-constant class (sym == NULL, value in *out_addend). */
static int gah_slot_classify(const TCCIRState *ir, TccIrOp irop, uint8_t slot,
                             IROperand op, Sym **out_sym, int32_t *out_addend)
{
  if (gah_operand_hoistable(ir, op, out_sym, out_addend))
    return 1;
  if (gah_consts_on && gah_const_slot_ok(irop, slot) &&
      gah_operand_hoistable_const(op, out_addend)) {
    *out_sym = NULL;
    return 1;
  }
  return 0;
}

/* The hoisted definition: address classes materialize &sym+addend, const
 * classes the immediate itself. */
static IROperand gah_make_def_src(TCCIRState *ir, Sym *sym, int32_t addend)
{
  if (sym) {
    uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, addend, 0);
    return irop_make_symref(-1, sidx, 0, 0, 0, IROP_BTYPE_INT32);
  }
  return irop_make_imm32(-1, addend, IROP_BTYPE_INT32);
}

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
  /* A STRUCT-typed operand keeps its type identity in the split u.s encoding
   * (ctype_idx), which a plain vreg operand cannot carry: rewriting the
   * FUNCPARAMVAL of a by-value struct global to a hoisted vreg dropped the
   * callsite's struct size/layout (arm-thumb-gen.c marshaled its 12-byte
   * THUMB_SHIFT_DEFAULT as a 32-byte blob → th_mov_reg got garbage → invalid
   * opcode 0x0). Keep struct symrefs un-hoisted. */
  if (op.btype == IROP_BTYPE_STRUCT)
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
                         TccIrOp irop, IROperand op, int idx, uint8_t slot)
{
  Sym *sym;
  int32_t addend;
  if (!gah_slot_classify(ir, irop, slot, op, &sym, &addend))
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

  gah_consts_on = !tcc_ir_opt_pass_disabled("ssa:const_pool_hoist");

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
      gah_try_slot(ir, &classes, &nclasses, &ccap, q->op, tcc_ir_op_get_src1(ir, q), i, GAH_SLOT_SRC1);
    if (irop_config[q->op].has_src2)
      gah_try_slot(ir, &classes, &nclasses, &ccap, q->op, tcc_ir_op_get_src2(ir, q), i, GAH_SLOT_SRC2);
    if (q->op == TCCIR_OP_MLA)
      gah_try_slot(ir, &classes, &nclasses, &ccap, q->op, tcc_ir_op_get_accum(ir, q), i, GAH_SLOT_ACCUM);
  }

  /* Reload estimate: 1 for the first materialization, +1 per later use that
   * follows a call since the previous use of the same address.  Uses are
   * collected in ascending instruction order.
   *
   * Const classes reload at every use instead: the call-crossing model
   * describes the symbol-scratch cache (imm_cache + high-reg parking), which
   * survives straight-line stretches.  Plain literals get no parking and the
   * cache is also reset at every jump target, so in practice each use pays a
   * pool load (252_fuzz main: 42 uses of 0x9e3779b1 = 42 loads emitted). */
  for (int c = 0; c < nclasses; c++) {
    GahClass *cl = &classes[c];
    if (!cl->sym) {
      cl->reload_estimate = cl->nuses;
      continue;
    }
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
   * function's own values in, and only a much hotter address earns that.
   * Const classes additionally clear GAH_CONST_TIGHT_RELOADS when the function
   * has no register headroom (same peak-window estimate the loop variant
   * budgets against). */
  int is_large = (n_instrs > GAH_LARGE_FN_INSTRS);
  int fn_tight = -1; /* computed lazily: only when a const class is considered */
  for (int picked = 0; picked < GAH_MAX_HOISTS; picked++) {
    int thresh = (picked >= 1 && is_large) ? GAH_LARGE_2ND_RELOADS : GAH_MIN_RELOADS;
    int best = -1;
    for (int c = 0; c < nclasses; c++) {
      int cthresh = thresh;
      if (classes[c].tbase != -1)
        continue;
      if (!classes[c].sym) {
        if (fn_tight < 0)
          fn_tight = tcc_ir_estimate_hoist_budget(ir, 0, n_instrs - 1,
                                                  ir->parameters_count)
                     <= GAH_CONST_TIGHT_BUDGET;
        if (fn_tight && cthresh < GAH_CONST_TIGHT_RELOADS)
          cthresh = GAH_CONST_TIGHT_RELOADS;
      }
      if (classes[c].reload_estimate < cthresh)
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
      IROperand src = gah_make_def_src(ir, cl->sym, cl->addend);
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

/* ------------------------------------------------------------------ *
 * Local (intra-block, call-free) variant.
 *
 * global_addr_hoist above only pays for an address reloaded ACROSS calls,
 * because parking one costs a callee-saved register for the whole function.
 * The far more common waste is the same address materialized twice inside one
 * straight-line stretch: each symref operand independently emits its own
 * `ldr rX,[pc,#..]`.  Reusing a temp there costs nothing the allocator cares
 * about -- the live range spans a few instructions and never crosses a call --
 * so the break-even is 2 uses, not GAH_MIN_RELOADS.
 * ------------------------------------------------------------------ */

/* Max address classes tracked per region; past this the extras keep reloading. */
#define LAC_MAX_CLASSES 24
/* Max temps introduced per function, a bound on vreg growth. */
#define LAC_MAX_TEMPS 96
/* Function size past which the extra live temps cost more than they save. */
#define LAC_MAX_FN_INSTRS 400

typedef struct {
  Sym *sym;
  int32_t addend;
  int nuses;
  int first_idx;
  int32_t tbase;
} LacClass;

typedef struct {
  int pos;
  Sym *sym;
  int32_t addend;
  int32_t tbase;
} LacInsert;

static int lah_insert_cmp(const void *a, const void *b)
{
  return ((const LacInsert *)b)->pos - ((const LacInsert *)a)->pos;
}

/* An indirect jump's targets are not enumerable, so neither region bounds nor
 * CFG dominance can prove a definition reaches every use: `goto *a` lands on a
 * label mid-region and runs the uses after it without the definition
 * (gcc-execute/pr70460).  Both variants decline such a function outright. */
static int lac_fn_has_unenumerable_edges(const TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++) {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE ||
        op == TCCIR_OP_SWITCH_LOAD)
      return 1;
  }
  return 0;
}

/* Ends the straight-line region: control leaves, or a call clobbers the
 * caller-saved registers the short-lived temp wants to live in. */
static int lac_breaks_region(TccIrOp op)
{
  switch (op) {
  case TCCIR_OP_JUMP: case TCCIR_OP_JUMPIF: case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE: case TCCIR_OP_SWITCH_LOAD:
  case TCCIR_OP_RETURNVOID: case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_FUNCCALLVOID: case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_INLINE_ASM: case TCCIR_OP_ASM_INPUT: case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_SETJMP: case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_SETJMP: case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_BUILTIN_APPLY: case TCCIR_OP_BUILTIN_APPLY_ARGS:
  case TCCIR_OP_BUILTIN_RETURN:
  case TCCIR_OP_VLA_ALLOC: case TCCIR_OP_VLA_SP_SAVE: case TCCIR_OP_VLA_SP_RESTORE:
  case TCCIR_OP_CALLSEQ_BEGIN: case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_SET_CHAIN:
    return 1;
  default:
    return 0;
  }
}

/* A dereference operand materializes sym+0 and folds its addend into the
 * load/store displacement, so only addend 0 can become a bare vreg deref;
 * a value operand materializes sym+addend, which the temp reproduces.
 * Const classes (sym == NULL) are never lvalues, so the rule is vacuous
 * for them. */
static int lac_slot_key(const TCCIRState *ir, TccIrOp irop, uint8_t slot,
                        IROperand op, Sym **out_sym, int32_t *out_addend)
{
  Sym *sym;
  int32_t addend;
  if (!gah_slot_classify(ir, irop, slot, op, &sym, &addend))
    return 0;
  if (sym && op.is_lval && addend != 0)
    return 0;
  *out_sym = sym;
  *out_addend = addend;
  return 1;
}

static IROperand lac_get_slot(TCCIRState *ir, IRQuadCompact *q, uint8_t slot)
{
  switch (slot) {
  case GAH_SLOT_SRC1: return tcc_ir_op_get_src1(ir, q);
  case GAH_SLOT_SRC2: return tcc_ir_op_get_src2(ir, q);
  case GAH_SLOT_DEST: return tcc_ir_op_get_dest(ir, q);
  default:            return tcc_ir_op_get_accum(ir, q);
  }
}

static void lac_set_slot(TCCIRState *ir, IRQuadCompact *q, uint8_t slot, IROperand op)
{
  switch (slot) {
  case GAH_SLOT_SRC1: tcc_ir_op_set_src1(ir, q, op); break;
  case GAH_SLOT_SRC2: tcc_ir_op_set_src2(ir, q, op); break;
  case GAH_SLOT_DEST: tcc_ir_op_set_dest(ir, q, op); break;
  default:            tcc_ir_op_set_accum(ir, q, op); break;
  }
}

static int lac_slot_count(const TCCIRState *ir, IRQuadCompact *q, uint8_t *slots)
{
  int n = 0;
  if (irop_config[q->op].has_src1)
    slots[n++] = GAH_SLOT_SRC1;
  if (irop_config[q->op].has_src2)
    slots[n++] = GAH_SLOT_SRC2;
  if (q->op == TCCIR_OP_MLA)
    slots[n++] = GAH_SLOT_ACCUM;
  /* A plain STORE carries its target ADDRESS in dest, not a result register --
   * the one op where dest is a value the backend materializes.  Without this
   * a loop that writes a global keeps reloading the address for the store even
   * though the hoisted temp already holds it (pr64494). */
  if (q->op == TCCIR_OP_STORE)
    slots[n++] = GAH_SLOT_DEST;
  (void)ir;
  return n;
}

/* insert_instr_at retargets every branch to >= pos, so a label that sat on the
 * shifted instruction now points one past the new def; move it back or the
 * paths entering through it would use an undefined temp. */
static void lac_keep_label_ahead(TCCIRState *ir, int pos)
{
  IRQuadCompact *moved = &ir->compact_instructions[pos + 1];
  if (!moved->is_jump_target)
    return;
  moved->is_jump_target = 0;
  ir->compact_instructions[pos].is_jump_target = 1;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand jdest = tcc_ir_op_get_dest(ir, q);
    if ((int)irop_get_imm64_ex(ir, jdest) == pos + 1)
      tcc_ir_op_set_dest(ir, q, irop_make_imm32(-1, pos, IROP_BTYPE_INT32));
  }
  for (int ti = 0; ti < ir->num_switch_tables; ti++) {
    TCCIRSwitchTable *table = &ir->switch_tables[ti];
    if (table->default_target == pos + 1)
      table->default_target = pos;
    for (int tj = 0; tj < table->num_entries; tj++)
      if (table->targets[tj] == pos + 1)
        table->targets[tj] = pos;
  }
}

int tcc_ir_ssa_opt_local_addr_cse(TCCIRState *ir)
{
  int n_instrs = ir->next_instruction_index;
  LacInsert *inserts = NULL;
  int ninserts = 0, icap = 0;
  int changes = 0;

  gah_consts_on = !tcc_ir_opt_pass_disabled("ssa:const_pool_hoist");

  /* Above this the allocator is already spilling and the temps cascade: each
   * one it parks in a callee-saved register perturbs allocation function-wide,
   * and the ldrd/strd pairs that lose their consecutive registers cost far
   * more than the reloads saved (73_arm64's stdarg: -8 reloads, +107 splits). */
  if (n_instrs > LAC_MAX_FN_INSTRS)
    return 0;
  if (lac_fn_has_unenumerable_edges(ir))
    return 0;

  for (int start = 0; start < n_instrs;) {
    LacClass classes[LAC_MAX_CLASSES];
    int nclasses = 0;
    int end = start;

    while (end < n_instrs) {
      IRQuadCompact *q = &ir->compact_instructions[end];
      if (end > start && q->is_jump_target)
        break;
      if (lac_breaks_region(q->op)) {
        end++;
        break;
      }
      end++;
    }

    for (int i = start; i < end; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      uint8_t slots[4];
      int nslots;
      if (!gah_op_hoistable(q->op))
        continue;
      nslots = lac_slot_count(ir, q, slots);
      for (int s = 0; s < nslots; s++) {
        Sym *sym;
        int32_t addend;
        int c;
        if (!lac_slot_key(ir, q->op, slots[s], lac_get_slot(ir, q, slots[s]), &sym, &addend))
          continue;
        for (c = 0; c < nclasses; c++)
          if (classes[c].sym == sym && classes[c].addend == addend)
            break;
        if (c == nclasses) {
          if (nclasses == LAC_MAX_CLASSES)
            continue;
          classes[nclasses].sym = sym;
          classes[nclasses].addend = addend;
          classes[nclasses].nuses = 0;
          classes[nclasses].first_idx = i;
          classes[nclasses].tbase = -1;
          nclasses++;
        }
        classes[c].nuses++;
      }
    }

    for (int c = 0; c < nclasses; c++) {
      /* Const classes need 3 uses: at 2 the machine-level imm_cache often
       * already reuses the loaded register in a call/branch-free region, and
       * the hoisted temp can cost a callee-saved push/pop (andok::foo 6->8).
       * At 3+ the saved reloads clear that reservation cost. */
      int min_uses = classes[c].sym ? 2 : 3;
      if (classes[c].nuses < min_uses || ninserts >= LAC_MAX_TEMPS)
        continue;
      classes[c].tbase = tcc_ir_vreg_alloc_temp(ir);
      if (classes[c].tbase < 0)
        continue;
      if (ninserts >= icap) {
        icap = icap ? icap * 2 : 8;
        inserts = tcc_realloc(inserts, icap * sizeof(LacInsert));
      }
      inserts[ninserts].pos = classes[c].first_idx;
      inserts[ninserts].sym = classes[c].sym;
      inserts[ninserts].addend = classes[c].addend;
      inserts[ninserts].tbase = classes[c].tbase;
      ninserts++;
    }

    for (int i = start; i < end; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      uint8_t slots[4];
      int nslots;
      if (!gah_op_hoistable(q->op))
        continue;
      nslots = lac_slot_count(ir, q, slots);
      for (int s = 0; s < nslots; s++) {
        IROperand orig = lac_get_slot(ir, q, slots[s]);
        Sym *sym;
        int32_t addend;
        IROperand nw;
        int c;
        if (!lac_slot_key(ir, q->op, slots[s], orig, &sym, &addend))
          continue;
        for (c = 0; c < nclasses; c++)
          if (classes[c].sym == sym && classes[c].addend == addend)
            break;
        if (c == nclasses || classes[c].tbase < 0)
          continue;
        nw = irop_make_vreg(classes[c].tbase, orig.btype);
        nw.is_lval = orig.is_lval;
        nw.is_unsigned = orig.is_unsigned;
        nw.is_static = orig.is_static;
        nw.is_complex = orig.is_complex;
        lac_set_slot(ir, q, slots[s], nw);
      }
    }

    start = end;
  }

  /* Descending, so an earlier recorded position is still valid after each
   * insert shifts the tail. */
  qsort(inserts, (size_t)ninserts, sizeof(LacInsert), lah_insert_cmp);
  for (int k = 0; k < ninserts; k++) {
    IROperand src = gah_make_def_src(ir, inserts[k].sym, inserts[k].addend);
    IROperand dst = irop_make_vreg(inserts[k].tbase, IROP_BTYPE_INT32);
    if (insert_instr_at(ir, inserts[k].pos, TCCIR_OP_ASSIGN, dst, src,
                        irop_make_none()) < 0)
      continue;
    lac_keep_label_ahead(ir, inserts[k].pos);
    changes++;
  }

  tcc_free(inserts);
  return changes;
}

/* ------------------------------------------------------------------ *
 * Loop-invariant variant.
 *
 * A symref operand inside a loop re-materializes the address on every
 * iteration -- `for (i..) s += a[i];` reloads &a from the literal pool each
 * time round.  The address is a link-time constant, so it is loop-invariant by
 * construction; LICM cannot move it because it is an operand, not an
 * instruction.  Making it explicit in the preheader is what lets it stay in a
 * register for the whole loop, which is where most of TCC's pool-load traffic
 * against GCC comes from (2197 of 3765 in-loop vs GCC's 667 of 1794 over the
 * torture corpus).
 * ------------------------------------------------------------------ */

/* Addresses parked per loop.  Each costs a register for the loop's whole
 * extent, and the loop must be able to hold ALL of them (see the all-or-nothing
 * rule at the selection site), so this caps how wide that bet gets. */
#define LAH_MAX_HOISTS 4
/* Register headroom a loop must have before its addresses are worth parking.
 * 2 is the calibrated point: 1 lets pr111422/pr122066's saturated loops in
 * (+8..11 instructions of spill each), 3 excludes mibench_crc32's budget-4
 * inner loop and gives back its -131k cycles. */
#define LAH_MIN_BUDGET 2
#define LAH_MAX_LOOPS 32

typedef struct {
  int header;      /* CFG block index */
  int latch;
  int span;        /* end_idx - start_idx, for outermost-first ordering */
} LahLoop;

static int lah_loop_cmp(const void *a, const void *b)
{
  return ((const LahLoop *)b)->span - ((const LahLoop *)a)->span;
}


static int lah_is_terminator(TccIrOp op)
{
  return op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF;
}

/* Marks the natural loop of back-edge latch->header; returns 0 if the shape is
 * not one we can place a preheader definition for. */
static int lah_collect_body(IRCFG *cfg, int header, int latch, char *in_loop,
                            int *worklist)
{
  int wl = 0;
  memset(in_loop, 0, (size_t)cfg->num_blocks);
  in_loop[header] = 1;
  if (latch != header) {
    in_loop[latch] = 1;
    worklist[wl++] = latch;
  }
  while (wl > 0) {
    IRBasicBlock *bb = &cfg->blocks[worklist[--wl]];
    for (int p = 0; p < bb->num_preds; p++) {
      int pr = bb->preds[p];
      if (pr < 0 || pr >= cfg->num_blocks || in_loop[pr])
        continue;
      in_loop[pr] = 1;
      worklist[wl++] = pr;
    }
  }
  return 1;
}

/* The single entry edge into the loop; the definition goes at its end.
 * Requires a *natural* loop: with irreducible flow (a goto into the body, as in
 * tests2/87_dead_code) an edge enters past the header and would run the loop
 * with the address temp undefined. */
static int lah_preheader_pos(TCCIRState *ir, IRCFG *cfg, int header, const char *in_loop)
{
  IRBasicBlock *hb = &cfg->blocks[header];
  int pre = -1;
  for (int b = 0; b < cfg->num_blocks; b++)
    if (in_loop[b] && !tcc_ir_cfg_dominates(cfg, header, b))
      return -1;
  for (int p = 0; p < hb->num_preds; p++) {
    int pr = hb->preds[p];
    if (pr < 0 || pr >= cfg->num_blocks || in_loop[pr])
      continue;
    if (pre >= 0)
      return -1; /* several entries: no single preheader */
    pre = pr;
  }
  if (pre < 0)
    return -1;
  if (!tcc_ir_cfg_dominates(cfg, pre, header))
    return -1;
  {
    IRBasicBlock *pb = &cfg->blocks[pre];
    int last = pb->end_idx - 1; /* end_idx is exclusive */
    IRQuadCompact *term = &ir->compact_instructions[last];
    if (lah_is_terminator(term->op)) {
      /* Before the branch -- but only if no edge lands on the branch itself,
       * which would enter the loop past the definition. */
      if (term->is_jump_target)
        return -1;
      return last;
    }
    if (pb->end_idx != hb->start_idx)
      return -1; /* not the fall-through predecessor */
    /* insert_instr_at retargets the back-edge to the shifted header, so the
     * definition stays out of the loop on the fall-through path. */
    return hb->start_idx;
  }
}

/* How many more values the loop can keep in registers, via the same peak-window
 * estimate LICM and symaddr_cse budget against.  Parking an address costs one
 * register for the loop's whole extent; in a loop that is already at capacity
 * the allocator pays it back with spills -- stack slots, or the backend's
 * `push {r0} ... pop {r0}` scratch saves -- that dwarf the reload removed. */
static int lah_loop_budget(TCCIRState *ir, IRCFG *cfg, const char *in_loop)
{
  int lo = ir->next_instruction_index, hi = -1;
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!in_loop[b])
      continue;
    if (cfg->blocks[b].start_idx < lo)
      lo = cfg->blocks[b].start_idx;
    if (cfg->blocks[b].end_idx - 1 > hi)
      hi = cfg->blocks[b].end_idx - 1;
  }
  if (hi < lo)
    return 0;
  return tcc_ir_estimate_hoist_budget(ir, lo, hi, ir->parameters_count);
}

static int lah_body_has_call(TCCIRState *ir, IRCFG *cfg, const char *in_loop)
{
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!in_loop[b])
      continue;
    for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++) {
      TccIrOp op = ir->compact_instructions[i].op;
      if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID ||
          op == TCCIR_OP_BLOCK_COPY)
        return 1;
    }
  }
  return 0;
}

int tcc_ir_ssa_opt_loop_addr_hoist(TCCIRState *ir)
{
  IRCFG *cfg;
  LahLoop loops[LAH_MAX_LOOPS];
  int nloops = 0;
  LacInsert *inserts = NULL;
  int ninserts = 0, icap = 0;
  char *in_loop;
  int *worklist;
  int changes = 0;

  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (ir->next_instruction_index > LAC_MAX_FN_INSTRS)
    return 0;
  if (lac_fn_has_unenumerable_edges(ir))
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;
  gah_consts_on = !tcc_ir_opt_pass_disabled("ssa:const_pool_hoist");
  cfg = tcc_ir_cfg_build(ir);
  if (!cfg || cfg->num_blocks <= 1) {
    tcc_ir_cfg_free(cfg);
    return 0;
  }
  tcc_ir_cfg_compute_dominators(cfg);

  for (int b = 0; b < cfg->num_blocks && nloops < LAH_MAX_LOOPS; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int s = 0; s < bb->num_succs && nloops < LAH_MAX_LOOPS; s++) {
      int h = bb->succs[s];
      if (h < 0 || h >= cfg->num_blocks)
        continue;
      if (!tcc_ir_cfg_dominates(cfg, h, b))
        continue;
      loops[nloops].header = h;
      loops[nloops].latch = b;
      loops[nloops].span = bb->end_idx - cfg->blocks[h].start_idx;
      nloops++;
    }
  }
  if (nloops == 0) {
    tcc_ir_cfg_free(cfg);
    return 0;
  }
  /* Outermost first: its preheader also covers the uses inside nested loops,
   * which then have no symref left to hoist. */
  qsort(loops, (size_t)nloops, sizeof(LahLoop), lah_loop_cmp);

  in_loop = tcc_mallocz((size_t)cfg->num_blocks);
  worklist = tcc_malloc((size_t)cfg->num_blocks * sizeof(int));

  for (int l = 0; l < nloops; l++) {
    LacClass classes[LAC_MAX_CLASSES];
    int nclasses = 0, picked = 0, pos, max_hoists;

    lah_collect_body(cfg, loops[l].header, loops[l].latch, in_loop, worklist);
    pos = lah_preheader_pos(ir, cfg, loops[l].header, in_loop);
    if (pos < 0)
      continue;
    /* A call in the body forces the parked address into a callee-saved
     * register -- an extra push/pop plus pressure on the values the loop
     * already keeps there, which outweighs the reload it removes.  In a
     * call-free loop the caller-saved registers are free and it is pure win. */
    if (lah_body_has_call(ir, cfg, in_loop))
      continue;

    for (int b = 0; b < cfg->num_blocks; b++) {
      if (!in_loop[b])
        continue;
      for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        uint8_t slots[4];
        int nslots;
        if (!gah_op_hoistable(q->op))
          continue;
        nslots = lac_slot_count(ir, q, slots);
        for (int s = 0; s < nslots; s++) {
          Sym *sym;
          int32_t addend;
          int c;
          if (!lac_slot_key(ir, q->op, slots[s], lac_get_slot(ir, q, slots[s]), &sym, &addend))
            continue;
          for (c = 0; c < nclasses; c++)
            if (classes[c].sym == sym && classes[c].addend == addend)
              break;
          if (c == nclasses) {
            if (nclasses == LAC_MAX_CLASSES)
              continue;
            classes[nclasses].sym = sym;
            classes[nclasses].addend = addend;
            classes[nclasses].nuses = 0;
            classes[nclasses].first_idx = i;
            classes[nclasses].tbase = -1;
            nclasses++;
          }
          classes[c].nuses++;
        }
      }
    }

    /* All the loop's addresses or none of them.  Parking only some is the worst
     * outcome: the parked one takes the register the remaining pool load then
     * needs as a scratch, so the loop pays a spill AND still reloads
     * (mibench_crc32's inner loop, +65k cycles that way, faster than baseline
     * once both of its addresses are parked).  The estimate is clamped to >= 1
     * even for a saturated loop, so keep clear of that floor. */
    max_hoists = lah_loop_budget(ir, cfg, in_loop) - LAH_MIN_BUDGET;
    if (max_hoists > LAH_MAX_HOISTS)
      max_hoists = LAH_MAX_HOISTS;
    /* Const classes are opportunistic passengers: they ride only when the
     * loop keeps a register of slack beyond the all-or-nothing minimum, and
     * they must never veto the established address hoists.  loop-2b::f is
     * both lessons in one: its INT_MAX compare first canceled the &a hoists
     * via the all-or-nothing rule (+4), and once prioritized in it consumed
     * the budget's last register, forcing a push/pop plus spill slots (+7)
     * to save a single in-loop reload. */
    {
      int nconst = 0;
      for (int c = 0; c < nclasses; c++)
        if (!classes[c].sym)
          nconst++;
      if (nconst && nclasses > max_hoists - 2) {
        int kept = 0;
        for (int c = 0; c < nclasses; c++)
          if (classes[c].sym)
            classes[kept++] = classes[c];
        nclasses = kept;
      }
    }
    if (nclasses > max_hoists)
      continue;

    while (picked < nclasses) {
      int best = -1;
      for (int c = 0; c < nclasses; c++) {
        if (classes[c].tbase != -1)
          continue;
        if (best < 0 || classes[c].nuses > classes[best].nuses)
          best = c;
      }
      if (best < 0)
        break;
      classes[best].tbase = tcc_ir_vreg_alloc_temp(ir);
      if (classes[best].tbase < 0)
        break;
      if (ninserts >= icap) {
        icap = icap ? icap * 2 : 8;
        inserts = tcc_realloc(inserts, (size_t)icap * sizeof(LacInsert));
      }
      inserts[ninserts].pos = pos;
      inserts[ninserts].sym = classes[best].sym;
      inserts[ninserts].addend = classes[best].addend;
      inserts[ninserts].tbase = classes[best].tbase;
      ninserts++;
      picked++;
    }
    if (picked == 0)
      continue;

    for (int b = 0; b < cfg->num_blocks; b++) {
      if (!in_loop[b])
        continue;
      for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        uint8_t slots[4];
        int nslots;
        if (!gah_op_hoistable(q->op))
          continue;
        nslots = lac_slot_count(ir, q, slots);
        for (int s = 0; s < nslots; s++) {
          IROperand orig = lac_get_slot(ir, q, slots[s]);
          Sym *sym;
          int32_t addend;
          IROperand nw;
          int c;
          if (!lac_slot_key(ir, q->op, slots[s], orig, &sym, &addend))
            continue;
          for (c = 0; c < nclasses; c++)
            if (classes[c].sym == sym && classes[c].addend == addend)
              break;
          if (c == nclasses || classes[c].tbase < 0)
            continue;
          nw = irop_make_vreg(classes[c].tbase, orig.btype);
          nw.is_lval = orig.is_lval;
          nw.is_unsigned = orig.is_unsigned;
          nw.is_static = orig.is_static;
          nw.is_complex = orig.is_complex;
          lac_set_slot(ir, q, slots[s], nw);
        }
      }
    }
  }

  tcc_free(in_loop);
  tcc_free(worklist);
  tcc_ir_cfg_free(cfg);

  /* Loops are visited outermost-first, so the recorded positions are unsorted;
   * applying them in anything but descending order would shift the ones still
   * pending out from under their recorded index. */
  qsort(inserts, (size_t)ninserts, sizeof(LacInsert), lah_insert_cmp);
  for (int k = 0; k < ninserts; k++) {
    IROperand src = gah_make_def_src(ir, inserts[k].sym, inserts[k].addend);
    IROperand dst = irop_make_vreg(inserts[k].tbase, IROP_BTYPE_INT32);
    if (insert_instr_at(ir, inserts[k].pos, TCCIR_OP_ASSIGN, dst, src,
                        irop_make_none()) < 0)
      continue;
    changes++;
  }
  tcc_free(inserts);
  return changes;
}
