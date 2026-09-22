/*
 * TinyCC - Tiny C Compiler
 *
 * Cross-jumping: merge identical block tails that jump to the same place.
 *
 * The Zig C backend spells out an error return at every `try`: build the error
 * union, store it through the result pointer, jump to the epilogue.  After
 * register allocation many of those tails are the same instructions on the
 * same registers and slots, differing only in which block they end.  A tail
 * that repeats another tail ending in a jump to the same target is replaced by
 * a jump into that other tail.
 *
 * Runs on the final IR, between register allocation and code generation.  The
 * code generator still moves some values while it emits (a value whose
 * register an instruction needs as scratch is moved or spilled), so equal
 * allocations do not make equal code.  Two tails match when their ops,
 * immediates, frame slots, symbols and annotations are equal and every vreg
 * operand is either the same vreg in both -- one value, wherever it ends up --
 * or defined inside its tail and used nowhere else, consistently paired with
 * its counterpart: only the kept copy runs, and it computes its own values.
 * Registers live inside the shared tail on the other path are ones the tail
 * reads or ones live at the common target, so the kept code, which never
 * takes a live register as scratch, leaves them alone.  The kept tail's first
 * instruction becomes a jump target, which resets the code generator's
 * register, flag and memory caches there.
 *
 * Not merged: calls and their arguments (per-call bookkeeping), control flow,
 * inline asm, VLA and setjmp ops, a tail that would begin by consuming flags
 * set before it, and a tail with a jump target inside the part removed.
 */

#include "ir.h"

#define CJ_MAX_TAIL 64

static int cj_mergeable_op(int op)
{
  switch (op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_SET_CHAIN:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
    return 0;
  default:
    return 1;
  }
}

static int cj_reads_flags(int op)
{
  return op == TCCIR_OP_SETIF || op == TCCIR_OP_SELECT || op == TCCIR_OP_JUMPIF;
}

static int cj_sets_flags(int op)
{
  return op == TCCIR_OP_CMP || op == TCCIR_OP_TEST_ZERO || op == TCCIR_OP_FCMP;
}

static int cj_has_slot3(int op)
{
  return op == TCCIR_OP_MLA || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_SELECT;
}

/* Struct operands name their type by an index into a per-operand pool, so the
 * same type has many indices: compare the types. */
static int cj_struct_same(IROperand a, IROperand b)
{
  if (a.u.s.aux_data != b.u.s.aux_data)
    return 0;
  CType *ta = irop_get_ctype(a), *tb = irop_get_ctype(b);
  return ta == tb || (ta && tb && ta->t == tb->t && ta->ref == tb->ref);
}

static int cj_operand_same(TCCIRState *ir, IROperand a, IROperand b)
{
  if (irop_is_none(a) || irop_is_none(b))
    return irop_is_none(a) && irop_is_none(b);
  if (a.tag != b.tag || a.btype != b.btype || a.is_lval != b.is_lval || a.is_llocal != b.is_llocal ||
      a.is_local != b.is_local || a.is_const != b.is_const || a.is_complex != b.is_complex ||
      a.is_unsigned != b.is_unsigned || a.is_static != b.is_static || a.is_sym != b.is_sym ||
      a.is_param != b.is_param || a.aux != b.aux)
    return 0;
  int32_t va = irop_get_vreg(a), vb = irop_get_vreg(b);
  if ((va < 0) != (vb < 0) || (va < 0 && va != vb))
    return 0;
  if (va >= 0 && TCCIR_DECODE_VREG_TYPE(va) != TCCIR_DECODE_VREG_TYPE(vb))
    return 0; /* which vreg is checked by cj_verify */
  switch (a.tag)
  {
  case IROP_TAG_VREG:
    return a.btype != IROP_BTYPE_STRUCT || cj_struct_same(a, b);
  case IROP_TAG_IMM32:
  case IROP_TAG_STACKOFF:
  case IROP_TAG_F32:
    if (a.btype == IROP_BTYPE_STRUCT)
      return cj_struct_same(a, b);
    return a.u.imm32 == b.u.imm32;
  case IROP_TAG_I64:
  case IROP_TAG_F64:
    return irop_get_imm64_ex(ir, a) == irop_get_imm64_ex(ir, b);
  case IROP_TAG_SYMREF:
  {
    IRPoolSymref *sa = irop_get_symref_ex(ir, a), *sb = irop_get_symref_ex(ir, b);
    return sa && sb && sa->sym == sb->sym && sa->addend == sb->addend;
  }
  default:
    return 0;
  }
}

static int cj_insn_same(TCCIRState *ir, IRQuadCompact *a, IRQuadCompact *b)
{
  if (a->op != b->op || !cj_mergeable_op(a->op))
    return 0;
  if ((irop_config[a->op].has_dest && !cj_operand_same(ir, tcc_ir_op_get_dest(ir, a), tcc_ir_op_get_dest(ir, b))) ||
      (irop_config[a->op].has_src1 && !cj_operand_same(ir, tcc_ir_op_get_src1(ir, a), tcc_ir_op_get_src1(ir, b))) ||
      (irop_config[a->op].has_src2 && !cj_operand_same(ir, tcc_ir_op_get_src2(ir, a), tcc_ir_op_get_src2(ir, b))))
    return 0;
  if (cj_has_slot3(a->op) &&
      !cj_operand_same(ir, ir->iroperand_pool[a->operand_base + 3], ir->iroperand_pool[b->operand_base + 3]))
    return 0;
  return tcc_ir_barrel_shift_at(ir, a) == tcc_ir_barrel_shift_at(ir, b) &&
         tcc_ir_shift64_dead_half_at(ir, a) == tcc_ir_shift64_dead_half_at(ir, b) &&
         tcc_ir_zero_half64_at(ir, a) == tcc_ir_zero_half64_at(ir, b) &&
         tcc_ir_bfi_params_at(ir, a) == tcc_ir_bfi_params_at(ir, b);
}

/* A dense index for a vreg (TEMP, VAR, PARAM), or -1. */
static int cj_vkey(TCCIRState *ir, int32_t vr)
{
  if (vr < 0)
    return -1;
  int pos = TCCIR_DECODE_VREG_POSITION(vr), nt = ir->next_temporary_variable, nv = ir->next_local_variable;
  switch (TCCIR_DECODE_VREG_TYPE(vr))
  {
  case TCCIR_VREG_TYPE_TEMP:
    return pos < nt ? pos : -1;
  case TCCIR_VREG_TYPE_VAR:
    return pos < nv ? nt + pos : -1;
  case TCCIR_VREG_TYPE_PARAM:
    return pos < ir->next_parameter ? nt + nv + pos : -1;
  default:
    return -1;
  }
}

static IROperand cj_slot(TCCIRState *ir, IRQuadCompact *q, int s)
{
  if (s == 0 && irop_config[q->op].has_dest)
    return tcc_ir_op_get_dest(ir, q);
  if (s == 1 && irop_config[q->op].has_src1)
    return tcc_ir_op_get_src1(ir, q);
  if (s == 2 && irop_config[q->op].has_src2)
    return tcc_ir_op_get_src2(ir, q);
  if (s == 3 && cj_has_slot3(q->op))
    return ir->iroperand_pool[q->operand_base + 3];
  return IROP_NONE;
}

/* Does slot s of q define the vreg it names (rather than read it)?  A store's
 * dest through a TEMP is the address it writes through, a use. */
static int cj_defines(IRQuadCompact *q, int s, IROperand o)
{
  if (s != 0 || q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(o)) == TCCIR_VREG_TYPE_TEMP)
    return !o.is_lval;
  return !o.is_llocal; /* a VAR is written through its slot operand */
}

/* How many of the tail instructions, counted from the end, can be merged:
 * every vreg operand is the same vreg in both copies, or a vreg first defined
 * in its copy, paired one to one with its counterpart and occurring nowhere
 * outside the tail.  Returns the verified length (0 if none). */
static int cj_verify(TCCIRState *ir, const int *ta, const int *tb, int k, const int *occ, int *pa, int *pb, int *ca,
                     int *cb)
{
  while (k >= 2)
  {
    int np = 0, fail = -1;
    for (int m = k - 1; m >= 0 && fail < 0; m--)
    {
      IRQuadCompact *qa = &ir->compact_instructions[ta[m]], *qb = &ir->compact_instructions[tb[m]];
      for (int s = 0; s < 4 && fail < 0; s++)
      {
        IROperand oa = cj_slot(ir, qa, s), ob = cj_slot(ir, qb, s);
        int va = cj_vkey(ir, irop_get_vreg(oa)), vb = cj_vkey(ir, irop_get_vreg(ob));
        if (va < 0 && vb < 0)
          continue;
        int p = -1;
        for (int x = 0; x < np; x++)
          if (pa[x] == va || pb[x] == vb)
            p = x;
        if (p >= 0)
        {
          if (pa[p] != va || pb[p] != vb)
            fail = m;
          else
            ca[p]++, cb[p]++;
          continue;
        }
        if (va == vb)
          continue; /* the same value in both */
        if (va < 0 || vb < 0 || !cj_defines(qa, s, oa) || !cj_defines(qb, s, ob))
        {
          fail = m;
          continue;
        }
        pa[np] = va, pb[np] = vb, ca[np] = 1, cb[np] = 1;
        np++;
      }
    }
    if (fail < 0)
    {
      for (int x = 0; x < np; x++)
        if (ca[x] != occ[pa[x]] || cb[x] != occ[pb[x]])
          return 0; /* used outside its tail */
      return k;
    }
    k = fail; /* keep only what follows the failing instruction */
  }
  return 0;
}

/* The previous non-NOP instruction before `i`, or -1. */
static int cj_prev(TCCIRState *ir, int i)
{
  for (i--; i >= 0; i--)
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      return i;
  return -1;
}

int tcc_ir_cross_jump(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  if (n < 4 || ir->func_has_label_addr)
    return 0;
  int *jumps = tcc_malloc(sizeof(int) * n);
  int nj = 0;
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_JUMP)
      jumps[nj++] = i;

  int *tail_a = tcc_malloc(sizeof(int) * n), *tail_b = tcc_malloc(sizeof(int) * n);
  /* Every vreg's operand occurrences, to tell a tail-local value. */
  const int nkeys = ir->next_temporary_variable + ir->next_local_variable + ir->next_parameter + 1;
  int *occ = tcc_mallocz(sizeof(int) * nkeys);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int s = 0; s < 4; s++)
    {
      int key = cj_vkey(ir, irop_get_vreg(cj_slot(ir, q, s)));
      if (key >= 0)
        occ[key]++;
    }
  }
  /* Scratch for cj_verify: at most 4 pairs per instruction. */
  int *pa = tcc_malloc(sizeof(int) * 4 * (CJ_MAX_TAIL + 1)), *pb = tcc_malloc(sizeof(int) * 4 * (CJ_MAX_TAIL + 1));
  int *ca = tcc_malloc(sizeof(int) * 4 * (CJ_MAX_TAIL + 1)), *cb = tcc_malloc(sizeof(int) * 4 * (CJ_MAX_TAIL + 1));
  int merged = 0;
  for (int x = 0; x < nj; x++)
  {
    int ja = jumps[x];
    IRQuadCompact *qa = &ir->compact_instructions[ja];
    if (qa->op != TCCIR_OP_JUMP)
      continue;
    int target = irop_get_imm32(tcc_ir_op_get_dest(ir, qa));
    for (int y = x + 1; y < nj; y++)
    {
      int jb = jumps[y];
      IRQuadCompact *qb = &ir->compact_instructions[jb];
      if (qb->op != TCCIR_OP_JUMP || qb->is_jump_target || irop_get_imm32(tcc_ir_op_get_dest(ir, qb)) != target)
        continue;
      /* The longest common tail, walked backwards in step.  A path entering
       * the removed copy anywhere but at its first instruction -- a jump target
       * on a skipped NOP included -- would run the whole kept tail instead of
       * the part after its entry, so the walk stops before such an entry.  The
       * copy's first instruction may be a target (its paths then fall through
       * NOPs to the redirected jump and run exactly that tail).  The kept copy
       * is left unchanged. */
      int k = 0;
      for (int ia = ja, ib = jb;;)
      {
        int na = cj_prev(ir, ia), nb = cj_prev(ir, ib);
        if (na < 0 || nb < 0 || na == nb || na == jb || nb == ja)
          break;
        int entry = 0;
        for (int m = nb + 1; m < ib && !entry; m++)
          entry = ir->compact_instructions[m].is_jump_target;
        IRQuadCompact *a = &ir->compact_instructions[na], *b = &ir->compact_instructions[nb];
        if (entry || !cj_insn_same(ir, a, b))
          break;
        tail_a[k] = na;
        tail_b[k] = nb;
        k++;
        if (k >= CJ_MAX_TAIL)
          break;
        ia = na;
        ib = nb;
        if (b->is_jump_target || a->is_jump_target)
          break;
      }
      /* Do not begin with a flag consumer whose flags were set before the
       * tail: trim from the front until every consumer follows a producer. */
      for (;;)
      {
        int flags_ok = 1, seen_producer = 0, drop = -1;
        for (int m = k - 1; m >= 0; m--)
        {
          int op = ir->compact_instructions[tail_a[m]].op;
          if (cj_reads_flags(op) && !seen_producer)
          {
            flags_ok = 0;
            drop = m;
            break;
          }
          if (cj_sets_flags(op))
            seen_producer = 1;
        }
        if (flags_ok)
          break;
        k = drop; /* keep only the instructions after the consumer */
      }
      k = k >= 2 ? cj_verify(ir, tail_a, tail_b, k, occ, pa, pb, ca, cb) : 0;
      if (k < 2)
        continue;
      /* Removed copy: tail_b[k-1] .. jb.  A jump target inside it other than
       * its first instruction would lose its code. */
      int ok = 1;
      for (int m = 0; m < k - 1 && ok; m++)
        ok = !ir->compact_instructions[tail_b[m]].is_jump_target;
      if (!ok)
        continue;
      int start_a = tail_a[k - 1], start_b = tail_b[k - 1];
      for (int m = 0; m < k; m++)
        ir->compact_instructions[tail_b[m]].op = TCCIR_OP_NOP;
      tcc_ir_set_dest(ir, jb, irop_make_imm32(-1, start_a, IROP_BTYPE_INT32));
      ir->compact_instructions[start_a].is_jump_target = 1;
      (void)start_b;
      merged++;
    }
  }
  tcc_free(cb);
  tcc_free(ca);
  tcc_free(pb);
  tcc_free(pa);
  tcc_free(occ);
  tcc_free(tail_b);
  tcc_free(tail_a);
  tcc_free(jumps);
  return merged;
}
