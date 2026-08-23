/*
 *  TCC IR - Deref-operand CSE over a temp-held address (late)
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
 * Deref-operand CSE (tcc_ir_opt_deref_operand_cse)
 * ============================================================================
 *
 * Sibling of tcc_ir_opt_global_deref_cse, for the case that one cannot reach:
 * the address is held in a vreg rather than named by a GlobalSym.
 *
 *     CMP T1***DEREF***,#9999      <- ldr
 *     JMP if "=="
 *     CMP T1***DEREF***,T4         <- ldr again, same word
 *
 * The frontend embeds a memory read directly in its consuming instruction, so
 * neither load CSE sees these (both match `LOAD`/`ASSIGN` dests), and codegen
 * materializes each operand on its own.  This is the shape of every
 * `arr[i].field` tested twice in one condition — mibench_dijkstra's inner loop
 * is two of them — and of `*p + *p`.
 *
 * The pass runs LATE, after phi resolution, for one reason: the two reads are
 * only recognisable as the same read once GVN has proved the two address
 * temps equal and rewritten the second use onto the first.  Before that they
 * are `T1***DEREF***` and `T3***DEREF***` with `T3 = T8 + T2`, and no operand
 * comparison can connect them.  That is also why this is not just a widened
 * matcher inside global_deref_cse, which runs in the flat pipeline well before
 * SSA.
 *
 * Three things have to hold between the two reads, and each is checked by the
 * scan below rather than assumed:
 *
 *  1. **Nothing wrote memory.**  Unlike the GlobalSym pass, a store to a stack
 *     local is NOT harmless here: the address is opaque, so it may well point
 *     at an addressable local.  Every memory write ends the region.
 *  2. **The address temp was not redefined.**  The IR is de-SSA'd at this
 *     point (phi resolution has emitted copies), so a vreg can be assigned
 *     twice; a def of the base kills its entry.
 *  3. **The read is not volatile.**  There is no Sym to consult for a
 *     `T***DEREF***`, so the operand carries IROP_AUX_NONVOLATILE, set at
 *     svalue_to_iroperand from the C type.  Absent bit = not proven = declined.
 *
 * Control flow is handled exactly as in the GlobalSym pass: a region runs from
 * an entry point to the next entry point, clobber, or unconditional transfer.
 * A conditional jump does not end it — control can only *enter* mid-region at
 * a jump target, and those are region boundaries — so the value materialized
 * at the first (dominating) read is still live at a read the fall-through path
 * reaches.  Entry points are computed here from the jump operands and switch
 * tables rather than read off `is_jump_target`, and a computed jump makes the
 * whole function unanalysable, so it bails.
 */

#define DOCSE_MAX_ENTRIES 16
#define DOCSE_MAX_USES 16

typedef struct {
  int32_t base_vr;
  uint8_t btype;
  uint8_t is_unsigned;
  int use_idx[DOCSE_MAX_USES];
  uint8_t use_src2[DOCSE_MAX_USES]; /* 0 = src1, 1 = src2 */
  int num_uses;
} DOCseEntry;

/* Value-read positions only — same set as the GlobalSym pass.  Ops whose src
 * operands are addresses, block descriptors, call bookkeeping or jump targets
 * are excluded: an lvalue there does not denote "load this word".  Note the
 * set deliberately contains nothing that also writes memory, so an entry can
 * never be recorded from an instruction that invalidates it. */
static int docse_op_reads_values(TccIrOp op)
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

/* A read through an address held in a vreg: plain vreg tag, lvalue, and not
 * one of the stack-relative encodings (those name a frame slot, which the
 * stack forwarding passes already track precisely). */
static int docse_is_temp_deref(IROperand op, int32_t *out_vr)
{
  if (irop_get_tag(op) != IROP_TAG_VREG || !op.is_lval || op.is_local || op.is_llocal)
    return 0;
  if (!(op.aux & IROP_AUX_NONVOLATILE))
    return 0;
  /* Integer, single-word reads.  INT64 is excluded on purpose: it lands on
   * the pair-load path whose lowering keys off IROP_AUX_ALIGN4_OK, and a
   * single replacement TEMP would have to carry both halves. */
  if (op.btype != IROP_BTYPE_INT32 && op.btype != IROP_BTYPE_INT16 && op.btype != IROP_BTYPE_INT8)
    return 0;
  if (op.is_complex)
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  int vt = TCCIR_DECODE_VREG_TYPE(vr);
  if (vt != TCCIR_VREG_TYPE_TEMP && vt != TCCIR_VREG_TYPE_PARAM)
    return 0;
  *out_vr = vr;
  return 1;
}

/* Does this instruction write memory the read could alias?  Deliberately
 * coarser than the GlobalSym pass: the base is opaque, so a store to a stack
 * local is a clobber too — it may be the very slot the pointer points at. */
static int docse_clobbers(TCCIRState *ir, IRQuadCompact *q)
{
  switch (q->op)
  {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_LOAD_POSTINC:
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
    default:
      break;
  }
  /* Catch-all for any op not named above that writes through an lvalue dest. */
  if (irop_config[q->op].has_dest)
  {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.is_lval)
      return 1;
  }
  return 0;
}

/* Control flow leaves for good; a JUMPIF does not (see the header comment). */
static int docse_ends_region(IRQuadCompact *q)
{
  switch (q->op)
  {
    case TCCIR_OP_JUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
      return 1;
    default:
      return 0;
  }
}

/* Every instruction control can arrive at other than by falling into it. */
static uint8_t *docse_entry_map(TCCIRState *ir, int n)
{
  uint8_t *entry = tcc_mallocz((size_t)n);
  if (!entry)
    return NULL;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->is_jump_target)
      entry[i] = 1;
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    if (t >= 0 && t < n)
      entry[t] = 1;
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *tbl = &ir->switch_tables[t];
    if (tbl->default_target >= 0 && tbl->default_target < n)
      entry[tbl->default_target] = 1;
    if (!tbl->targets)
      continue;
    for (int j = 0; j < tbl->num_entries; j++)
      if (tbl->targets[j] >= 0 && tbl->targets[j] < n)
        entry[tbl->targets[j]] = 1;
  }
  return entry;
}

static void docse_record(TCCIRState *ir, DOCseEntry *entries, int *num_entries,
                         IROperand op, int idx, int is_src2)
{
  int32_t base_vr;
  if (!docse_is_temp_deref(op, &base_vr))
    return;
  for (int k = 0; k < *num_entries; k++)
  {
    DOCseEntry *e = &entries[k];
    if (e->base_vr != base_vr || e->btype != op.btype || e->is_unsigned != op.is_unsigned)
      continue;
    if (e->num_uses < DOCSE_MAX_USES)
    {
      e->use_src2[e->num_uses] = (uint8_t)is_src2;
      e->use_idx[e->num_uses++] = idx;
    }
    return;
  }
  if (*num_entries >= DOCSE_MAX_ENTRIES)
    return;
  DOCseEntry *e = &entries[(*num_entries)++];
  e->base_vr = base_vr;
  e->btype = (uint8_t)op.btype;
  e->is_unsigned = (uint8_t)op.is_unsigned;
  e->use_src2[0] = (uint8_t)is_src2;
  e->use_idx[0] = idx;
  e->num_uses = 1;
}

/* Does this instruction give one of the tracked address temps a new value?
 * The IR is de-SSA'd here, so that is possible, and every read recorded so far
 * refers to the OLD address.  The caller ends the region on a hit — after
 * recording this instruction's own reads, which still see the old value. */
static int docse_defs_tracked_base(TCCIRState *ir, IRQuadCompact *q, const DOCseEntry *entries,
                                   int num_entries)
{
  if (!num_entries || !irop_config[q->op].has_dest)
    return 0;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (dest.is_lval)
    return 0; /* a memory write, not a vreg def — handled as a clobber */
  int32_t dvr = irop_get_vreg(dest);
  if (dvr < 0)
    return 0;
  for (int k = 0; k < num_entries; k++)
    if (entries[k].base_vr == dvr)
      return 1;
  return 0;
}

/* Materialize one entry: `Tv <- <read>` before its first use, then rewrite
 * every recorded use to Tv.  Returns 1 if applied. */
static int docse_apply(TCCIRState *ir, DOCseEntry *e, const uint8_t *entry_map)
{
  int first = e->use_idx[0];
  /* Inserting before an entry point would place the load outside the block the
   * incoming edge lands in (gsym_cse_insert_before repoints the edge past the
   * new instruction), leaving Tv undefined on that path.  The scan already
   * ends a region at every entry point, so this can only be the very first
   * instruction — which is exactly the case the loop's `i > 0` guard skips. */
  if (entry_map[first] || ir->compact_instructions[first].is_jump_target)
    return 0;

  IRQuadCompact *fq = &ir->compact_instructions[first];
  IROperand read = e->use_src2[0] ? tcc_ir_op_get_src2(ir, fq) : tcc_ir_op_get_src1(ir, fq);

  int32_t vreg = tcc_ir_vreg_alloc_temp(ir);
  if (vreg < 0)
    return 0;
  IROperand tmp = irop_make_vreg(vreg, read.btype);
  tmp.is_unsigned = read.is_unsigned;

  if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
    tcc_ir_pool_ensure(ir, 2);

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
 * insertion shifts the instruction array, so the scan cursor, the instruction
 * count and the still-pending entries' recorded indices move with it. */
static int docse_flush(TCCIRState *ir, DOCseEntry *entries, int *num_entries, int *cur, int *n,
                       uint8_t **entry_map)
{
  int changes = 0;
  for (int k = 0; k < *num_entries; k++)
  {
    if (entries[k].num_uses < 2)
      continue;
    int at = entries[k].use_idx[0];
    if (!docse_apply(ir, &entries[k], *entry_map))
      continue;
    changes++;
    for (int m = k + 1; m < *num_entries; m++)
      for (int u = 0; u < entries[m].num_uses; u++)
        if (entries[m].use_idx[u] >= at)
          entries[m].use_idx[u]++;
    /* The entry map is indexed by instruction position, so it shifts too. */
    uint8_t *em = tcc_mallocz((size_t)(*n + 1));
    if (em)
    {
      for (int j = 0; j < at; j++)
        em[j] = (*entry_map)[j];
      em[at] = 0; /* the inserted ASSIGN is only ever reached by fall-through */
      for (int j = at; j < *n; j++)
        em[j + 1] = (*entry_map)[j];
      tcc_free(*entry_map);
      *entry_map = em;
    }
    (*cur)++;
    (*n)++;
  }
  *num_entries = 0;
  return changes;
}

int tcc_ir_opt_deref_operand_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2 || tcc_ir_opt_pass_disabled("deref_operand_cse"))
    return 0;

  /* A computed jump can land anywhere, so no region boundary is trustworthy. */
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;

  uint8_t *entry_map = docse_entry_map(ir, n);
  if (!entry_map)
    return 0;

  DOCseEntry entries[DOCSE_MAX_ENTRIES];
  int num_entries = 0;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* The entry-point flush comes BEFORE the NOP skip: a branch target is an
     * index, and earlier passes routinely blank the instruction sitting at one
     * (a JUMPIF's else-arm, a collapsed block).  Control still arrives there
     * and falls through, so skipping the flush would let a region span the
     * join and hand the merge block a temp only one predecessor defines. */
    if (i > 0 && entry_map[i])
    {
      changes += docse_flush(ir, entries, &num_entries, &i, &n, &entry_map);
      q = &ir->compact_instructions[i];
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    if (docse_clobbers(ir, q))
    {
      changes += docse_flush(ir, entries, &num_entries, &i, &n, &entry_map);
      continue; /* never record a read from an instruction that invalidates it */
    }

    if (docse_op_reads_values(q->op))
    {
      if (irop_config[q->op].has_src1)
        docse_record(ir, entries, &num_entries, tcc_ir_op_get_src1(ir, q), i, 0);
      if (irop_config[q->op].has_src2)
        docse_record(ir, entries, &num_entries, tcc_ir_op_get_src2(ir, q), i, 1);
    }

    /* Order matters: the reads above are of the address this instruction is
     * about to overwrite, so they are recorded first and the region ends after
     * them.  Reads of the new address then start a fresh region. */
    if (docse_ends_region(q) || docse_defs_tracked_base(ir, q, entries, num_entries))
      changes += docse_flush(ir, entries, &num_entries, &i, &n, &entry_map);
  }

  int tail = n;
  changes += docse_flush(ir, entries, &num_entries, &tail, &n, &entry_map);
  tcc_free(entry_map);
  return changes;
}
