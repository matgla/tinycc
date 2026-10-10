/*
 * TinyCC - Tiny C Compiler
 *
 * Dead TEMP definitions reaching register allocation.
 *
 * Two shapes reach the code generator as instructions whose result nothing
 * reads:
 *
 *   T8 <-- Addr[StackLoc[-88]]      address of an object whose copy frame
 *   T9 <-- Addr[StackLoc[-88]]      colouring merged away: the copy is gone,
 *                                   the operands' LEAs are not
 *
 *   T13 <-- #0 [ASSIGN]             a compound literal's zero fill, split into
 *   T13 <-- GlobalSym [STORE]       field TEMPs by SRA: the field's real value
 *   PARAM1 T13                      overwrites the zero before anything reads it
 *
 * In the Zig compiler built as C they were `add rX,sp,#k` pairs and
 * `movs rX,#0; ldr rX,[pc,#k]` pairs, 18 KB.
 *
 * A definition of a TEMP by a side-effect-free op is deleted when
 *   - no instruction names the TEMP any more (other than as this plain
 *     destination), or
 *   - the same TEMP is redefined, as a plain destination and without being
 *     read, later in the same straight run of code, before any instruction
 *     names it: every path from the first definition passes the second one.
 * A TEMP's value is a whole register (a spilled TEMP's slot is written as a
 * word), so any definition of it replaces all of the old value.
 *
 * Runs inside the allocator, after its SSA passes and before its intervals,
 * where a TEMP is still read only under its own name.  After it, graph coalescing erases the copies between vregs that
 * share a register (a phi's `T21 <- T23`), and T23's value is then read as
 * T21 at the join: an unnamed use this pass cannot see.
 */

#include "ir.h"

#define DD_MAX_DIST 64

static int dd_has_slot3(int op)
{
  return ir_op_has(op, IROP_A_SLOT3);
}

static IROperand dd_slot(TCCIRState *ir, IRQuadCompact *q, int s)
{
  if (s == 0 && irop_config[q->op].has_dest)
    return tcc_ir_op_get_dest(ir, q);
  if (s == 1 && irop_config[q->op].has_src1)
    return tcc_ir_op_get_src1(ir, q);
  if (s == 2 && irop_config[q->op].has_src2)
    return tcc_ir_op_get_src2(ir, q);
  if (s == 3 && dd_has_slot3(q->op))
    return ir->iroperand_pool[q->operand_base + 3];
  return IROP_NONE;
}

/* Ops that compute their destination from their operands and do nothing
 * else: no memory write, no control transfer, no flags another op consumes
 * (the carry-generating halves of a 64-bit add/subtract are left alone). */
static int dd_pure_op(int op)
{
  switch (op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_LOAD:
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
  case TCCIR_OP_ROR:
  case TCCIR_OP_ZEXT:
  case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_SETIF:
    return 1;
  default:
    return 0;
  }
}

/* Ops that write their whole destination and read it only through their
 * explicit operands (BFI reads the destination it inserts into; the
 * post-increment forms write their base too). */
static int dd_full_def_op(int op)
{
  return dd_pure_op(op) || op == TCCIR_OP_STORE || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_FUNCCALLVAL;
}

/* The TEMP a plain (register) destination names, or -1. */
static int32_t dd_plain_temp_dest(TCCIRState *ir, IRQuadCompact *q)
{
  if (!irop_config[q->op].has_dest)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  int32_t vr = irop_get_vreg(d);
  if (vr < 0 || d.is_lval || d.is_llocal || irop_get_tag(d) != IROP_TAG_VREG ||
      TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  return vr;
}

static int dd_width(IROperand op)
{
  int bt = irop_get_btype(op);
  return (bt == IROP_BTYPE_INT64 || bt == IROP_BTYPE_FLOAT64) ? 8 : 4;
}

/* A side annotation that changes what the instruction reads or writes. */
static int dd_annotated(TCCIRState *ir, IRQuadCompact *q)
{
  return tcc_ir_barrel_shift_at(ir, q) || tcc_ir_shift64_dead_half_at(ir, q) || tcc_ir_zero_half64_at(ir, q) ||
         tcc_ir_bfi_params_at(ir, q);
}

/* Instructions that end a straight run: after them the next instruction in
 * the array is not the only continuation. */
static int dd_ends_run(int op)
{
  return ir_op_has(op, IROP_ENDS_BLOCK) || op == TCCIR_OP_SWITCH_LOAD;
}

static void dd_count(TCCIRState *ir, IRQuadCompact *q, int *occ, int nt, int delta)
{
  for (int s = 0; s < 4; s++)
  {
    int32_t vr = irop_get_vreg(dd_slot(ir, q, s));
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos < nt)
      occ[pos] += delta;
  }
}

/* Is the definition at i overwritten before any read, on the straight run
 * that follows it? */
static int dd_overwritten(TCCIRState *ir, int i, int32_t vr, int width)
{
  const int n = ir->next_instruction_index;
  IRQuadCompact *def = &ir->compact_instructions[i];
  if (dd_ends_run(def->op))
    return 0;
  for (int j = i + 1, dist = 0; j < n && dist < DD_MAX_DIST; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    dist++;
    int named = 0;
    for (int s = 1; s < 4 && !named; s++)
      named = irop_get_vreg(dd_slot(ir, q, s)) == vr;
    if (named)
      return 0;
    if (irop_config[q->op].has_dest && tcc_ir_op_dest_vreg(ir, q) == vr)
      return dd_plain_temp_dest(ir, q) == vr && dd_full_def_op(q->op) && !dd_annotated(ir, q) &&
             dd_width(tcc_ir_op_get_dest(ir, q)) >= width;
    if (dd_ends_run(q->op))
      return 0;
  }
  return 0;
}

int tcc_ir_dead_def(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  const int nt = ir->next_temporary_variable;
  if (n < 2 || nt <= 0 || ir->func_has_label_addr)
    return 0;
  /* Operand occurrences per TEMP, destinations included. */
  int *occ = tcc_mallocz(sizeof(int) * nt);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_NOP)
      dd_count(ir, q, occ, nt, 1);
  }

  int removed = 0, changed = 1;
  while (changed)
  {
    changed = 0;
    for (int i = n - 1; i >= 0; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (!dd_pure_op(q->op) || dd_annotated(ir, q))
        continue;
      int32_t vr = dd_plain_temp_dest(ir, q);
      if (vr < 0)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos >= nt)
        continue;
      /* Any memory operand may be volatile (`T <- *p ADD #1` reads *p). */
      int vol = 0;
      for (int s = 1; s < 4 && !vol; s++)
      {
        IROperand o = dd_slot(ir, q, s);
        vol = !irop_is_none(o) && tcc_ir_access_is_volatile(ir, o);
      }
      if (vol)
        continue;
      /* Named only as this destination, or overwritten before a read.  A
       * TEMP the op itself also reads (`T <- T + 1`) counts more than once. */
      if (occ[pos] != 1 && !dd_overwritten(ir, i, vr, dd_width(tcc_ir_op_get_dest(ir, q))))
        continue;
      dd_count(ir, q, occ, nt, -1);
      q->op = TCCIR_OP_NOP;
      removed++;
      changed = 1;
    }
  }
  tcc_free(occ);
  return removed;
}
