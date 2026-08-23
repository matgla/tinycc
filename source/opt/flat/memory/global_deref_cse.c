/*
 *  TCC IR - Global deref-operand CSE (late)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"

extern int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q);

/* ============================================================================
 * Global deref-operand CSE (tcc_ir_opt_global_deref_cse)
 * ============================================================================
 *
 * The frontend embeds a global memory read directly in the consuming ALU
 * instruction rather than emitting a separate LOAD:
 *
 *     T0 <- GlobalSym(b)+8***DEREF***  SHL #26
 *     T4 <- GlobalSym(b)+8***DEREF***  AND #-64
 *
 * Codegen materializes each such operand independently, so the same word is
 * loaded twice.  The existing load CSE passes only match `LOAD`/`ASSIGN`
 * shapes, so this form is invisible to them - and it is the dominant shape of
 * a bitfield read-modify-write (`b.i += x` reads the storage unit once to
 * extract the field and once to build the clear mask), where it costs one
 * redundant LDR in every such function.
 *
 * This pass finds a global lvalue operand read at least twice inside one
 * straight-line clobber-free region, materializes it once into a TEMP, and
 * rewrites the reads to that TEMP.
 *
 * Placement is LATE, for the same reason tcc_ir_opt_symaddr_cse_late exists
 * but is not run: interprocedural const cascades pattern-match SYMREF operand
 * shapes, and hiding reads behind a vreg early blinds them (20040629-1 main
 * 11 -> 2214).  Running after the const cascades keeps them intact, and unlike
 * the address CSE this transform *preserves* a direct SYMREF lvalue read (the
 * inserted ASSIGN), so the post-codegen TU analyses that look for readers of a
 * static still see one.
 */

#define GDCSE_MAX_ENTRIES 16
#define GDCSE_MAX_USES 16

typedef struct {
  Sym *sym;
  int64_t addend;
  uint8_t btype;
  uint8_t is_unsigned;
  int use_idx[GDCSE_MAX_USES]; /* instruction index of each read */
  uint8_t use_src2[GDCSE_MAX_USES]; /* 0 = src1, 1 = src2 */
  int num_uses;
} GDCseEntry;

/* Value-read positions only.  Ops whose src operands are addresses, block
 * descriptors, call-sequence bookkeeping or jump targets are excluded: an
 * lvalue SYMREF there does not denote "load this word". */
static int gdcse_op_reads_values(TccIrOp op)
{
  switch (op)
  {
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    case TCCIR_OP_MUL:
    case TCCIR_OP_DIV:
    case TCCIR_OP_UDIV:
    case TCCIR_OP_PDIV:
    case TCCIR_OP_UMOD:
    case TCCIR_OP_IMOD:
    case TCCIR_OP_AND:
    case TCCIR_OP_OR:
    case TCCIR_OP_XOR:
    case TCCIR_OP_SHL:
    case TCCIR_OP_SAR:
    case TCCIR_OP_SHR:
    case TCCIR_OP_ROR:
    case TCCIR_OP_CMP:
    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_SETIF:
      return 1;
    default:
      return 0;
  }
}

/* A global (non-stack) memory read: SYMREF, lvalue, not local. */
static int gdcse_is_global_read(TCCIRState *ir, IROperand op, IRPoolSymref **out)
{
  if (irop_get_tag(op) != IROP_TAG_SYMREF || !op.is_lval || op.is_local)
    return 0;
  /* Integer reads only.  INT64 is admitted as a pair TEMP (the plain
   * `unsigned long long x = g` load shape) — it is the storage-unit read of
   * every 64-bit bitfield extract, where each expression otherwise reloads
   * the same unit.  FLOAT operands stay excluded: FLOAT32 can be half of a
   * complex pair, and substituting a single TEMP would drop the other half.
   * STRUCT operands describe more than one value behind one operand. */
  if (op.btype != IROP_BTYPE_INT32 && op.btype != IROP_BTYPE_INT16 && op.btype != IROP_BTYPE_INT8 &&
      op.btype != IROP_BTYPE_INT64)
    return 0;
  if (op.is_complex)
    return 0;
  IRPoolSymref *sr = irop_get_symref_ex(ir, op);
  if (!sr || !sr->sym)
    return 0;
  /* The Sym only knows about a wholly-volatile object; `volatile int a[4]`
   * carries the qualifier on the element type and a volatile struct member
   * carries it on the member, so the access mark on the operand decides. */
  if ((sr->sym->type.t & VT_VOLATILE) || tcc_ir_access_is_volatile(ir, op))
    return 0;
  *out = sr;
  return 1;
}

/* Does this instruction potentially write memory that a global read aliases? */
static int gdcse_clobbers(TCCIRState *ir, IRQuadCompact *q)
{
  switch (q->op)
  {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_VLA_ALLOC:
      return 1;
    case TCCIR_OP_STORE:
    {
      /* Any store can alias a global unless it targets a stack local.  A
       * direct store to a *different* global would be provably disjoint, but
       * the bitfield RMW shape ends its region at its own store anyway, so
       * the extra precision buys nothing here. */
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      return !dest.is_local || dest.is_llocal;
    }
    default:
      return 0;
  }
}

/* Region boundary: control flow leaves for good.  A JUMPIF does NOT end the
 * region: control can only *enter* mid-region at a jump target, and the scan
 * loop flushes on those.  Reads after a conditional exit are reached solely by
 * fall-through, so the value materialized at the (dominating) first use is
 * still valid — this is what lets short-circuit `a || b || c` chains share
 * one load.  On the taken path the temp is simply unused. */
static int gdcse_ends_region(IRQuadCompact *q)
{
  switch (q->op)
  {
    case TCCIR_OP_JUMP:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
      return 1;
    default:
      return 0;
  }
}

static void gdcse_record(TCCIRState *ir, GDCseEntry *entries, int *num_entries,
                         IROperand op, int idx, int is_src2)
{
  IRPoolSymref *sr;
  if (!gdcse_is_global_read(ir, op, &sr))
    return;
  for (int k = 0; k < *num_entries; k++)
  {
    GDCseEntry *e = &entries[k];
    if (e->sym != sr->sym || e->addend != sr->addend || e->btype != op.btype ||
        e->is_unsigned != op.is_unsigned)
      continue;
    if (e->num_uses < GDCSE_MAX_USES)
    {
      e->use_src2[e->num_uses] = (uint8_t)is_src2;
      e->use_idx[e->num_uses++] = idx;
    }
    return;
  }
  if (*num_entries >= GDCSE_MAX_ENTRIES)
    return;
  GDCseEntry *e = &entries[(*num_entries)++];
  e->sym = sr->sym;
  e->addend = sr->addend;
  e->btype = (uint8_t)op.btype;
  e->is_unsigned = (uint8_t)op.is_unsigned;
  e->use_src2[0] = (uint8_t)is_src2;
  e->use_idx[0] = idx;
  e->num_uses = 1;
}

/* Materialize one entry: insert `Tv <- <read>` before its first use and
 * rewrite every recorded use to Tv.  Returns 1 if applied. */
static int gdcse_apply(TCCIRState *ir, GDCseEntry *e)
{
  int first = e->use_idx[0];
  /* Inserting before a jump target would place the load outside the block the
   * incoming edge lands in (gsym_cse_insert_before repoints the edge past the
   * new instruction), leaving Tv undefined on that path. */
  if (ir->compact_instructions[first].is_jump_target)
    return 0;

  IRQuadCompact *fq = &ir->compact_instructions[first];
  IROperand read = e->use_src2[0] ? tcc_ir_op_get_src2(ir, fq) : tcc_ir_op_get_src1(ir, fq);

  int32_t vreg = tcc_ir_vreg_alloc_temp(ir);
  if (vreg < 0)
    return 0;
  IROperand tmp = irop_make_vreg(vreg, read.btype);
  tmp.is_unsigned = read.is_unsigned;

  IRQuadCompact assign = {0};
  assign.op = TCCIR_OP_ASSIGN;
  assign.operand_base = tcc_ir_pool_add(ir, tmp);
  tcc_ir_pool_add(ir, read);
  if (gsym_cse_insert_before(ir, first, &assign) < 0)
    return 0;

  for (int u = 0; u < e->num_uses; u++)
  {
    IRQuadCompact *q = &ir->compact_instructions[e->use_idx[u] + 1]; /* shifted by the insert */
    if (e->use_src2[u])
      tcc_ir_op_set_src2(ir, q, tmp);
    else
      tcc_ir_op_set_src1(ir, q, tmp);
  }
  return 1;
}

/* Materialize every entry read more than once, then clear the table.  Each
 * insertion shifts the instruction array, so *cur (the scan cursor) and *n are
 * bumped along with the still-pending entries' recorded indices. */
static int gdcse_flush(TCCIRState *ir, GDCseEntry *entries, int *num_entries, int *cur, int *n)
{
  int changes = 0;
  for (int k = 0; k < *num_entries; k++)
  {
    if (entries[k].num_uses < 2)
      continue;
    int at = entries[k].use_idx[0];
    if (!gdcse_apply(ir, &entries[k]))
      continue;
    changes++;
    for (int m = k + 1; m < *num_entries; m++)
      for (int u = 0; u < entries[m].num_uses; u++)
        if (entries[m].use_idx[u] >= at)
          entries[m].use_idx[u]++;
    (*cur)++;
    (*n)++;
  }
  *num_entries = 0;
  return changes;
}

int tcc_ir_opt_global_deref_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2 || tcc_ir_opt_pass_disabled("global_deref_cse"))
    return 0;

  GDCseEntry entries[GDCSE_MAX_ENTRIES];
  int num_entries = 0;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* A jump target can be entered from elsewhere, so it ends the region
     * before this instruction.  Tested BEFORE the NOP skip, because earlier
     * passes blank the instruction that sits at a branch target often enough
     * (a folded guard, a collapsed else arm) that skipping it would let a
     * region span the join and hand the merge block a temp only one
     * predecessor defines. */
    if (i > 0 && q->is_jump_target)
    {
      changes += gdcse_flush(ir, entries, &num_entries, &i, &n);
      q = &ir->compact_instructions[i];
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* A clobber can rewrite what was read: it ends the region too. */
    if (gdcse_clobbers(ir, q))
    {
      changes += gdcse_flush(ir, entries, &num_entries, &i, &n);
      q = &ir->compact_instructions[i];
    }

    if (gdcse_op_reads_values(q->op))
    {
      if (irop_config[q->op].has_src1)
        gdcse_record(ir, entries, &num_entries, tcc_ir_op_get_src1(ir, q), i, 0);
      if (irop_config[q->op].has_src2)
        gdcse_record(ir, entries, &num_entries, tcc_ir_op_get_src2(ir, q), i, 1);
    }

    if (gdcse_ends_region(q))
      changes += gdcse_flush(ir, entries, &num_entries, &i, &n);
  }

  changes += gdcse_flush(ir, entries, &num_entries, &n, &n);
  return changes;
}
