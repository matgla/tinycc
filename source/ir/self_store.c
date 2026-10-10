/*
 * TinyCC - Tiny C Compiler
 *
 * Self-store removal: stores that write back the value just loaded from the
 * same frame bytes.
 *
 * Frame relayout (frame.c) gives objects with disjoint lifetimes the same
 * bytes.  A copy `u = t` where t dies at the copy and u starts there is such a
 * pair, so after relayout the word-by-word copy reads and writes the same slot:
 *
 *   T1 <-- StackLoc[-16] [LOAD]
 *   T2 <-- StackLoc[-12] [LOAD]
 *   StackLoc[-16] <-- T1 [STORE]
 *   StackLoc[-12] <-- T2 [STORE]
 *
 * The stores change nothing.  In the Zig compiler built as C these are 5,367
 * groups, 56 KB of code (22 KB in main_buildOutputType alone).
 *
 * A STORE to a frame slot is deleted when its value is a TEMP loaded from the
 * same slot earlier in the same straight run of code, the store writes no more
 * bytes than the load read, and nothing between them can write memory there:
 * no call, no store through a pointer, no store overlapping the slot, no inline
 * asm, and no jump target (another path could arrive with other bytes in the
 * slot).  The load is deleted too when the store was its value's only use.
 *
 * Runs on the final IR, after register allocation, when every frame operand
 * carries its final offset.
 */

#include "ir.h"

#define SC_MAX_DIST 64

static int sc_width(IROperand op)
{
  switch (irop_get_btype(op))
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 0;
  }
}

/* A direct access to frame bytes: an lvalue stack operand without a vreg.
 * Sets its offset and width. */
static int sc_frame_access(TCCIRState *ir, IROperand op, int *off, int *width)
{
  if (irop_is_none(op) || irop_get_vreg(op) >= 0 || op.is_param || !op.is_lval)
    return 0;
  int tag = irop_get_tag(op);
  if (tag != IROP_TAG_STACKOFF && !(tag == IROP_TAG_VREG && (op.is_local || op.is_llocal)))
    return 0;
  if (tcc_ir_access_is_volatile(ir, op))
    return 0;
  *off = irop_get_stack_offset(op);
  *width = sc_width(op);
  return *width > 0;
}

/* A TEMP read as a plain value (not dereferenced). */
static int32_t sc_temp_value(IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_lval || op.is_llocal || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  if (irop_get_tag(op) != IROP_TAG_VREG)
    return -1;
  return vr;
}

static int sc_has_slot3(int op)
{
  return ir_op_has(op, IROP_A_SLOT3);
}

static IROperand sc_slot(TCCIRState *ir, IRQuadCompact *q, int s)
{
  if (s == 0 && irop_config[q->op].has_dest)
    return tcc_ir_op_get_dest(ir, q);
  if (s == 1 && irop_config[q->op].has_src1)
    return tcc_ir_op_get_src1(ir, q);
  if (s == 2 && irop_config[q->op].has_src2)
    return tcc_ir_op_get_src2(ir, q);
  if (s == 3 && sc_has_slot3(q->op))
    return ir->iroperand_pool[q->operand_base + 3];
  return IROP_NONE;
}

/* Ops that neither transfer control nor write memory other than through
 * their destination operand. */
static int sc_plain_op(int op)
{
  switch (op)
  {
  case TCCIR_OP_ADD:
  case TCCIR_OP_ADC_USE:
  case TCCIR_OP_ADC_GEN:
  case TCCIR_OP_SUB:
  case TCCIR_OP_SUBC_USE:
  case TCCIR_OP_SUBC_GEN:
  case TCCIR_OP_MUL:
  case TCCIR_OP_MLA:
  case TCCIR_OP_UMULL:
  case TCCIR_OP_DIV:
  case TCCIR_OP_UMOD:
  case TCCIR_OP_IMOD:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
  case TCCIR_OP_PDIV:
  case TCCIR_OP_UDIV:
  case TCCIR_OP_CMP:
  case TCCIR_OP_SETIF:
  case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_LOAD:
  case TCCIR_OP_STORE:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_LOAD_INDEXED:
  case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_BFI:
  case TCCIR_OP_FADD:
  case TCCIR_OP_FSUB:
  case TCCIR_OP_FMUL:
  case TCCIR_OP_FDIV:
  case TCCIR_OP_FNEG:
  case TCCIR_OP_FCMP:
  case TCCIR_OP_CVT_FTOF:
  case TCCIR_OP_CVT_ITOF:
  case TCCIR_OP_CVT_FTOI:
  case TCCIR_OP_ZEXT:
  case TCCIR_OP_PACK64:
  case TCCIR_OP_BOOL_OR:
  case TCCIR_OP_BOOL_AND:
  case TCCIR_OP_SELECT:
  case TCCIR_OP_ROR:
  case TCCIR_OP_NOP:
    return 1;
  default:
    return tcc_ir_op_is_mac(op);
  }
}

/* Can instruction q write any of the frame bytes [off, off + width), or
 * redefine `vr`? */
static int sc_clobbers(TCCIRState *ir, IRQuadCompact *q, int off, int width, int32_t vr)
{
  if (!sc_plain_op(q->op))
    return 1;
  if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (irop_is_none(d))
    return 0;
  int32_t dv = irop_get_vreg(d);
  if (dv == vr)
    return 1;
  int doff, dwidth;
  if (sc_frame_access(ir, d, &doff, &dwidth))
    return doff < off + width && off < doff + dwidth;
  if (dv < 0)
    return d.is_lval; /* some other memory destination */
  if (q->op == TCCIR_OP_STORE)
    return 1; /* through a pointer */
  int t = TCCIR_DECODE_VREG_TYPE(dv);
  if (t == TCCIR_VREG_TYPE_TEMP)
    return d.is_lval; /* a TEMP destination is a register; lval = through it */
  return 0;         /* a VAR or PARAM lives in its own register or spill slot */
}

/* A side annotation that changes what the instruction reads or writes. */
static int sc_annotated(TCCIRState *ir, IRQuadCompact *q)
{
  return tcc_ir_barrel_shift_at(ir, q) || tcc_ir_shift64_dead_half_at(ir, q) || tcc_ir_zero_half64_at(ir, q) ||
         tcc_ir_bfi_params_at(ir, q);
}

int tcc_ir_self_store(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  if (n < 2 || ir->func_has_label_addr)
    return 0;
  const int nt = ir->next_temporary_variable;
  if (nt <= 0)
    return 0;
  /* Operand occurrences per TEMP, to tell when a load loses its last use. */
  int *occ = tcc_mallocz(sizeof(int) * nt);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int s = 0; s < 4; s++)
    {
      int32_t vr = irop_get_vreg(sc_slot(ir, q, s));
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos < nt)
        occ[pos]++;
    }
  }

  int removed = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *st = &ir->compact_instructions[i];
    if (st->op != TCCIR_OP_STORE)
      continue;
    int off, width;
    if (!sc_frame_access(ir, tcc_ir_op_get_dest(ir, st), &off, &width))
      continue;
    int32_t vr = sc_temp_value(tcc_ir_op_get_src1(ir, st));
    if (vr < 0)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos >= nt)
      continue;
    if (sc_annotated(ir, st) || st->is_jump_target)
      continue;

    /* Walk back to the value's definition through a straight run. */
    int ld = -1;
    for (int j = i - 1, dist = 0; j >= 0 && dist < SC_MAX_DIST; j--)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
      {
        if (q->is_jump_target)
          break;
        continue;
      }
      dist++;
      if (q->op == TCCIR_OP_LOAD && tcc_ir_op_dest_vreg(ir, q) == vr)
      {
        int loff, lwidth;
        if (!tcc_ir_op_dest_is_lval(ir, q) && sc_frame_access(ir, tcc_ir_op_get_src1(ir, q), &loff, &lwidth) && loff == off &&
            lwidth >= width && !sc_annotated(ir, q))
          ld = j;
        break;
      }
      if (sc_clobbers(ir, q, off, width, vr))
        break;
      if (q->is_jump_target)
        break;
    }
    if (ld < 0)
      continue;

    st->op = TCCIR_OP_NOP;
    removed++;
    occ[pos]--;
    /* The load's register may still be read under another name: reload
     * elimination dropped a later reload of this slot because the stored
     * register already held it, and an elided identity phi copy reads it as
     * its own.  Both pin the interval (phi_pinned); the load then stays. */
    IRLiveInterval *li = tcc_ir_vreg_is_valid(ir, vr) ? tcc_ir_vreg_live_interval(ir, vr) : NULL;
    if (occ[pos] == 1 && !(li && li->phi_pinned))
    {
      ir->compact_instructions[ld].op = TCCIR_OP_NOP;
      occ[pos] = 0;
    }
  }
  tcc_free(occ);
  return removed;
}
