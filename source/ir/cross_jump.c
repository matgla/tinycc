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
  return tcc_ir_op_is_mac(op) || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_SELECT;
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

/*
 * Tail merging by final location, run inside code generation.
 *
 * tcc_ir_cross_jump above pairs values by vreg, so two tails that differ only
 * in which vreg brings a value in stay apart even when both vregs sit in the
 * same register -- Zig's error return after each `try`:
 *
 *   R1(T43) <-- R1(T253) [ASSIGN]          R1(T73) <-- R1(T254) [ASSIGN]
 *   R10(T323)***DEREF*** <-- #0xaaaaaaaa   R10(T323)***DEREF*** <-- #0xaaaaaaaa
 *   R10(T323) <-- R1(T43) STORE_INDEXED #4 R10(T323) <-- R1(T73) STORE_INDEXED #4
 *   JMP to 361                             JMP to 361
 *
 * T253 and T254 are each the error code of a different call; on either path
 * the tail reads it from R1.  This pass merges such tails once the locations
 * are final: after the discovery pass of tcc_ir_codegen_generate, which is the
 * last thing that moves a vreg (scratch-conflict reassignment, demotion to a
 * frame slot).  A value flowing into the tail may then be a different vreg in
 * each copy when both have the same allocation -- same registers, or the same
 * frame slot -- and neither copy redefines it.  Everything else follows
 * tcc_ir_cross_jump: the same vreg, or defined inside its copy, used nowhere
 * else, and paired one to one.
 *
 * Tails ending in a return merge too (RETURNVOID with RETURNVOID; RETURNVALUE
 * with RETURNVALUE of the same value by those rules): both leave through the
 * one epilogue.  The removed copy's instructions become NOPs, one of them the
 * jump to the kept copy's first instruction.
 */

/* Do two vregs live in the same place for the whole function? */
static int cjl_same_location(TCCIRState *ir, int32_t va, int32_t vb)
{
  int ta = TCCIR_DECODE_VREG_TYPE(va), tb = TCCIR_DECODE_VREG_TYPE(vb);
  if (ta != tb || (ta != TCCIR_VREG_TYPE_TEMP && ta != TCCIR_VREG_TYPE_VAR))
    return 0;
  IRLiveInterval *a = tcc_ir_get_live_interval(ir, va), *b = tcc_ir_get_live_interval(ir, vb);
  if (!a || !b || a->addrtaken || b->addrtaken || a->is_lvalue || b->is_lvalue)
    return 0;
  if (a->is_float != b->is_float || a->is_double != b->is_double || a->is_llong != b->is_llong ||
      a->is_complex != b->is_complex || a->use_vfp != b->use_vfp || a->is_struct != b->is_struct)
    return 0;
  if (a->allocation.r0 != b->allocation.r0 || a->allocation.r1 != b->allocation.r1)
    return 0;
  if (a->allocation.r0 == (PREG_SPILLED | PREG_REG_NONE) || (a->allocation.r0 & PREG_SPILLED))
  {
    if (a->allocation.offset != b->allocation.offset)
      return 0;
  }
  else if ((a->allocation.r0 & PREG_REG_NONE) == PREG_REG_NONE)
    return 0; /* no location at all */
  if (a->remat_kind != b->remat_kind || (a->remat_kind && a->remat_imm != b->remat_imm))
    return 0;
  return 1;
}

/* Does any instruction of the tail define vreg vr? */
static int cjl_defined_in(TCCIRState *ir, const int *tail, int k, int32_t key)
{
  for (int m = 0; m < k; m++)
  {
    IRQuadCompact *q = &ir->compact_instructions[tail[m]];
    for (int s = 0; s < 4; s++)
    {
      IROperand o = cj_slot(ir, q, s);
      if (cj_vkey(ir, irop_get_vreg(o)) == key && cj_defines(q, s, o))
        return 1;
    }
  }
  return 0;
}

static int cjl_is_call(int op)
{
  return op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID || op == TCCIR_OP_FUNCCALLVAL ||
         op == TCCIR_OP_FUNCCALLVOID;
}

static int cjl_call_id(TCCIRState *ir, IRQuadCompact *q)
{
  return TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
}

/* cj_insn_same, and calls: a call's parameters and the call carry the call id
 * in src2's high half, which cjl_verify pairs; the low half (parameter index
 * or argument count) must match. */
static int cjl_insn_same(TCCIRState *ir, IRQuadCompact *a, IRQuadCompact *b)
{
  if (!cjl_is_call(a->op) || a->op != b->op)
    return cj_insn_same(ir, a, b);
  for (int s = 0; s < 4; s++)
  {
    IROperand oa = cj_slot(ir, a, s), ob = cj_slot(ir, b, s);
    if (s == 2)
    {
      if (oa.tag != ob.tag || (irop_get_imm64_ex(ir, oa) & 0xFFFF) != (irop_get_imm64_ex(ir, ob) & 0xFFFF))
        return 0;
      continue;
    }
    if (!cj_operand_same(ir, oa, ob))
      return 0;
  }
  return tcc_ir_barrel_shift_at(ir, a) == tcc_ir_barrel_shift_at(ir, b) &&
         tcc_ir_shift64_dead_half_at(ir, a) == tcc_ir_shift64_dead_half_at(ir, b) &&
         tcc_ir_zero_half64_at(ir, a) == tcc_ir_zero_half64_at(ir, b) &&
         tcc_ir_bfi_params_at(ir, a) == tcc_ir_bfi_params_at(ir, b);
}

/* Call ids of the tail paired one to one, every instruction of a paired call
 * inside its tail.  Returns 1, or 0 with *cut set to the number of tail
 * instructions after the first call found cut in two (the tail starts
 * between its parameters). */
static int cjl_calls_whole(TCCIRState *ir, const int *ta, const int *tb, int k, const int *callocc, int ncalls,
                           int *ida, int *idb, int *cnt, int *cut)
{
  int np = 0;
  for (int m = 0; m < k; m++)
  {
    IRQuadCompact *qa = &ir->compact_instructions[ta[m]], *qb = &ir->compact_instructions[tb[m]];
    if (!cjl_is_call(qa->op))
      continue;
    int a = cjl_call_id(ir, qa), b = cjl_call_id(ir, qb);
    if (a < 0 || b < 0 || a >= ncalls || b >= ncalls)
    {
      *cut = m;
      return 0;
    }
    int p = -1;
    for (int x = 0; x < np; x++)
      if (ida[x] == a || idb[x] == b)
        p = x;
    if (p < 0)
    {
      ida[np] = a, idb[np] = b, cnt[np] = 0;
      p = np++;
    }
    else if (ida[p] != a || idb[p] != b)
    {
      *cut = m;
      return 0;
    }
    cnt[p]++;
  }
  for (int x = 0; x < np; x++)
    if (cnt[x] != callocc[ida[x]] || cnt[x] != callocc[idb[x]])
    {
      /* keep what follows this call's last instruction in the tail */
      int last = k;
      for (int m = 0; m < k; m++)
      {
        IRQuadCompact *qa = &ir->compact_instructions[ta[m]];
        if (cjl_is_call(qa->op) && cjl_call_id(ir, qa) == ida[x])
        {
          last = m;
          break;
        }
      }
      *cut = last;
      return 0;
    }
  return 1;
}

/* cj_verify with location-matched values flowing in.  The terminators ra/rb,
 * when >= 0, are RETURNVALUEs whose operand must match like any other read. */
static int cjl_verify(TCCIRState *ir, int *ta, int *tb, int k, int ra, int rb, const int *occ, int *pa, int *pb,
                      int *ca, int *cb, char *live_in)
{
  while (k >= 1)
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
          continue;
        if (va < 0 || vb < 0)
        {
          fail = m;
          continue;
        }
        int da = cj_defines(qa, s, oa), db = cj_defines(qb, s, ob);
        if (da && db)
        {
          pa[np] = va, pb[np] = vb, ca[np] = 1, cb[np] = 1, live_in[np] = 0;
          np++;
          continue;
        }
        if (!da && !db && cjl_same_location(ir, irop_get_vreg(oa), irop_get_vreg(ob)) &&
            !cjl_defined_in(ir, ta, k, va) && !cjl_defined_in(ir, tb, k, vb))
        {
          pa[np] = va, pb[np] = vb, ca[np] = 1, cb[np] = 1, live_in[np] = 1;
          np++;
          continue;
        }
        fail = m;
      }
    }
    if (fail < 0 && ra >= 0)
    {
      /* The returned value: same vreg, a pair from the tail, or a value
       * flowing in through the same location. */
      IROperand oa = tcc_ir_op_get_src1(ir, &ir->compact_instructions[ra]);
      IROperand ob = tcc_ir_op_get_src1(ir, &ir->compact_instructions[rb]);
      int va = cj_vkey(ir, irop_get_vreg(oa)), vb = cj_vkey(ir, irop_get_vreg(ob));
      if (va >= 0 || vb >= 0)
      {
        int p = -1;
        for (int x = 0; x < np; x++)
          if (pa[x] == va || pb[x] == vb)
            p = x;
        if (p >= 0)
        {
          if (pa[p] != va || pb[p] != vb)
            return 0;
          else
            ca[p]++, cb[p]++;
        }
        else if (va != vb)
        {
          if (va >= 0 && vb >= 0 && cjl_same_location(ir, irop_get_vreg(oa), irop_get_vreg(ob)) &&
              !cjl_defined_in(ir, ta, k, va) && !cjl_defined_in(ir, tb, k, vb))
          {
            pa[np] = va, pb[np] = vb, ca[np] = 1, cb[np] = 1, live_in[np] = 1;
            np++;
          }
          else
            return 0; /* the returned values differ whatever the tail */
        }
      }
    }
    if (fail < 0)
    {
      for (int x = 0; x < np; x++)
        if (!live_in[x] && (ca[x] != occ[pa[x]] || cb[x] != occ[pb[x]]))
          return 0; /* used outside its tail */
      return k;
    }
    k = fail; /* keep only what follows the failing instruction */
  }
  return 0;
}

static int cjl_is_exit(int op)
{
  return op == TCCIR_OP_JUMP || op == TCCIR_OP_RETURNVOID || op == TCCIR_OP_RETURNVALUE;
}

/* Do two exits leave to the same place? */
static int cjl_same_exit(TCCIRState *ir, IRQuadCompact *a, IRQuadCompact *b)
{
  if (a->op != b->op)
    return 0;
  if (a->op == TCCIR_OP_JUMP)
    return irop_get_imm32(tcc_ir_op_get_dest(ir, a)) == irop_get_imm32(tcc_ir_op_get_dest(ir, b));
  if (a->op == TCCIR_OP_RETURNVALUE)
  {
    IROperand oa = tcc_ir_op_get_src1(ir, a), ob = tcc_ir_op_get_src1(ir, b);
    return cj_operand_same(ir, oa, ob) && !tcc_ir_barrel_shift_at(ir, a) && !tcc_ir_barrel_shift_at(ir, b);
  }
  return 1;
}

int tcc_ir_cross_jump_alloc(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  if (n < 4 || ir->func_has_label_addr || tcc_ir_calls_returns_twice(ir))
    return 0;
  int *exits = tcc_malloc(sizeof(int) * n);
  int ne = 0;
  for (int i = 0; i < n; i++)
    if (cjl_is_exit(ir->compact_instructions[i].op))
      exits[ne++] = i;

  int *tail_a = tcc_malloc(sizeof(int) * (CJ_MAX_TAIL + 1)), *tail_b = tcc_malloc(sizeof(int) * (CJ_MAX_TAIL + 1));
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
  const int ncalls = ir->next_call_id + 1;
  int *callocc = tcc_mallocz(sizeof(int) * ncalls);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (cjl_is_call(q->op))
    {
      int id = cjl_call_id(ir, q);
      if (id >= 0 && id < ncalls)
        callocc[id]++;
    }
  }
  const int np_max = 4 * (CJ_MAX_TAIL + 1) + 1;
  int *pa = tcc_malloc(sizeof(int) * np_max), *pb = tcc_malloc(sizeof(int) * np_max);
  int *ca = tcc_malloc(sizeof(int) * np_max), *cb = tcc_malloc(sizeof(int) * np_max);
  char *live_in = tcc_malloc(np_max);
  int merged = 0;
  for (int x = 0; x < ne; x++)
  {
    int ja = exits[x];
    IRQuadCompact *qa = &ir->compact_instructions[ja];
    if (!cjl_is_exit(qa->op))
      continue;
    for (int y = x + 1; y < ne; y++)
    {
      int jb = exits[y];
      IRQuadCompact *qb = &ir->compact_instructions[jb];
      if (!cjl_is_exit(qb->op) || qb->is_jump_target || !cjl_same_exit(ir, qa, qb))
        continue;
      /* The longest common tail, walked backwards in step (see
       * tcc_ir_cross_jump). */
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
        if (entry || cjl_is_exit(a->op) || cjl_is_exit(b->op) || !cjl_insn_same(ir, a, b))
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
        k = drop;
      }
      int ra = qa->op == TCCIR_OP_RETURNVALUE ? ja : -1, rb = qa->op == TCCIR_OP_RETURNVALUE ? jb : -1;
      for (;;)
      {
        int cut;
        k = k >= 1 ? cjl_verify(ir, tail_a, tail_b, k, ra, rb, occ, pa, pb, ca, cb, live_in) : 0;
        if (k < 1 || cjl_calls_whole(ir, tail_a, tail_b, k, callocc, ncalls, pa, pb, ca, &cut))
          break;
        k = cut;
      }
      /* A plain jump pays for itself only with two instructions merged; a
       * return is replaced by a jump, so one instruction plus the return. */
      if (k < (qa->op == TCCIR_OP_JUMP ? 2 : 1))
        continue;
      int ok = 1;
      for (int m = 0; m < k - 1 && ok; m++)
        ok = !ir->compact_instructions[tail_b[m]].is_jump_target;
      if (!ok)
        continue;
      /* The jump goes into a removed instruction with a destination slot:
       * the removed exit itself when it has one (JUMP, RETURNVALUE). */
      int jslot = -1;
      if (qb->op != TCCIR_OP_RETURNVOID)
        jslot = jb;
      else
        for (int m = 0; m < k && jslot < 0; m++)
          if (irop_config[ir->compact_instructions[tail_b[m]].op].has_dest ||
              irop_config[ir->compact_instructions[tail_b[m]].op].has_src1)
            jslot = tail_b[m];
      if (jslot < 0)
        continue;
      int start_a = tail_a[k - 1];
      for (int m = 0; m < k; m++)
        ir->compact_instructions[tail_b[m]].op = TCCIR_OP_NOP;
      if (jslot != jb)
        ir->compact_instructions[jb].op = TCCIR_OP_NOP;
      IRQuadCompact *jq = &ir->compact_instructions[jslot];
      /* pool[operand_base] is the first operand slot of whatever op was here */
      ir->iroperand_pool[jq->operand_base] = irop_make_imm32(-1, start_a, IROP_BTYPE_INT32);
      jq->op = TCCIR_OP_JUMP;
      ir->compact_instructions[start_a].is_jump_target = 1;
      merged++;
    }
  }
  tcc_free(callocc);
  tcc_free(live_in);
  tcc_free(cb);
  tcc_free(ca);
  tcc_free(pb);
  tcc_free(pa);
  tcc_free(occ);
  tcc_free(tail_b);
  tcc_free(tail_a);
  tcc_free(exits);
  return merged;
}

/*
 * Region merging: whole single-entry regions that repeat.
 *
 * A switch over a Zig tagged union spells out one case body per tag, and after
 * SRA and frame colouring many of those bodies are the same code: loads from
 * the same slots, the same call, the same branches to the same places.  Each is
 * a region -- a straight run of instructions entered only at its first one,
 * ending in an unconditional transfer, with every branch inside it going either
 * to the same place in both copies or to the same position within its own copy.
 * A region that repeats an earlier one becomes a jump to it.
 *
 * Same rules as the tail merge above for values: a vreg is the same vreg in
 * both copies, or first defined in its copy and occurring nowhere else, paired
 * one to one.  Calls may be merged here: a call's parameters and the call carry
 * a call id, and ids are paired the same way (every instruction with the id
 * inside the region).  Not merged: indirect jumps, switch dispatch, inline asm,
 * VLA and setjmp ops, a region that begins by consuming flags set before it, and
 * a region whose inner jump targets are reached from outside it.
 */

#define RM_MAX_LEN 4096
#define RM_MIN_LEN 3
#define RM_BUCKET_TRIES 32

static int rm_op_ok(int op)
{
  switch (op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
    return 1;
  case TCCIR_OP_NOP:
    return 0;
  default:
    return cj_mergeable_op(op);
  }
}

static int rm_is_call_op(int op)
{
  return op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID || op == TCCIR_OP_FUNCCALLVAL ||
         op == TCCIR_OP_FUNCCALLVOID;
}

static int rm_is_jump(int op)
{
  return op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF;
}

static int rm_ends_region(int op)
{
  return op == TCCIR_OP_JUMP || op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID;
}

/* Everything but jump targets and call ids, which rm_match pairs itself. */
static int rm_insn_same(TCCIRState *ir, IRQuadCompact *a, IRQuadCompact *b)
{
  if (a->op != b->op || !rm_op_ok(a->op))
    return 0;
  for (int s = 0; s < 4; s++)
  {
    if (s == 0 && rm_is_jump(a->op))
      continue;
    IROperand oa = cj_slot(ir, a, s), ob = cj_slot(ir, b, s);
    if (s == 2 && rm_is_call_op(a->op))
    {
      if (oa.tag != ob.tag || (irop_get_imm64_ex(ir, oa) & 0xFFFF) != (irop_get_imm64_ex(ir, ob) & 0xFFFF))
        return 0;
      continue;
    }
    if (!cj_operand_same(ir, oa, ob))
      return 0;
  }
  return tcc_ir_barrel_shift_at(ir, a) == tcc_ir_barrel_shift_at(ir, b) &&
         tcc_ir_shift64_dead_half_at(ir, a) == tcc_ir_shift64_dead_half_at(ir, b) &&
         tcc_ir_zero_half64_at(ir, a) == tcc_ir_zero_half64_at(ir, b) &&
         tcc_ir_bfi_params_at(ir, a) == tcc_ir_bfi_params_at(ir, b);
}

static int rm_next(TCCIRState *ir, int i, int n)
{
  while (i < n && ir->compact_instructions[i].op == TCCIR_OP_NOP)
    i++;
  return i;
}

static int rm_call_id(TCCIRState *ir, IRQuadCompact *q)
{
  return TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
}

#define RM_NOCASE INT64_MIN
#define RM_MULTI (INT64_MIN + 1)
#define RM_MAX_PARAMS 8

typedef struct RMState
{
  TCCIRState *ir;
  int n;
  int *ord;     /* non-NOP instructions before each index */
  int *occ;     /* operand occurrences per vreg key */
  int *callocc; /* instructions per call id */
  int *jrefs;   /* JUMP/JUMPIF references per target */
  int *trefs;   /* switch table references per target */
  unsigned char *dead;
  int *ia, *ib; /* matched instruction pairs */
  /* pairing maps, valid where stamp == gen */
  int *vmap_a, *vmap_b, *vcnt, *vstamp_a, *vstamp_b;
  int *cmap_a, *cmap_b, *ccnt, *cstamp_a, *cstamp_b;
  int gen;
  /* Case-constant merging (NULL arrays when off). */
  int64_t *caseval;        /* case value reaching each entry only from its dispatch, or RM_NOCASE/RM_MULTI */
  int *selsw;              /* the SWITCH_TABLE dispatching to each entry */
  IROperand *swsel;        /* per SWITCH_TABLE: an operand holding the case value itself, or IROP_NONE */
  signed char *pslot;      /* operand slot to rewrite to the selector, or -1 */
  int *pown;               /* entry of the region owning a rewrite / a parameterised region's instructions */
  int *npar;               /* rewrites per parameterised entry */
  unsigned char *exact_in; /* an exact copy was merged into this entry */
  int nparam, param_at[RM_MAX_PARAMS], param_slot[RM_MAX_PARAMS];
} RMState;

/* Pair key ka (copy A) with kb (copy B); returns 0 on a conflict. */
static int rm_pair(RMState *st, int *map_a, int *map_b, int *cnt, int *stamp_a, int *stamp_b, int ka, int kb,
                   int may_start)
{
  int seen_a = stamp_a[ka] == st->gen, seen_b = stamp_b[kb] == st->gen;
  if (seen_a || seen_b)
  {
    if (!seen_a || !seen_b || map_a[ka] != kb || map_b[kb] != ka)
      return 0;
    cnt[ka]++;
    return 1;
  }
  if (!may_start)
    return 0;
  stamp_a[ka] = stamp_b[kb] = st->gen;
  map_a[ka] = kb;
  map_b[kb] = ka;
  cnt[ka] = 1;
  return 1;
}

/*
 * Case-constant merging.  A Zig `switch (x.tag) { .k => .{ .tag = k, ... } }`
 * spells out bodies that are the same except for the constant k, the value of
 * the selector on the way in.  Such bodies match when each copy's constant is
 * the case value of the single table entry reaching it; the kept copy then
 * reads the selector instead of its constant.  Rewrites are recorded while
 * matching and applied at the end, so later bodies still compare constants.
 *
 * The selector must still hold the case value where the kept copy reads it:
 * its location (register or frame slot, after allocation) must be reserved
 * over the whole copy and written by nothing in it.
 */

/* An operand naming a 32-bit-or-narrower integer value held in a vreg. */
static int rm_sel_operand_ok(IROperand o)
{
  int32_t vr = irop_get_vreg(o);
  if (vr < 0 || o.tag != IROP_TAG_VREG || o.is_lval || o.is_llocal || o.is_complex)
    return 0;
  if (o.btype != IROP_BTYPE_INT32 && o.btype != IROP_BTYPE_INT8 && o.btype != IROP_BTYPE_INT16)
    return 0;
  int t = TCCIR_DECODE_VREG_TYPE(vr);
  return t == TCCIR_VREG_TYPE_TEMP || t == TCCIR_VREG_TYPE_VAR || t == TCCIR_VREG_TYPE_PARAM;
}

/* Where a vreg lives after allocation: core registers (mask) or a frame
 * offset.  Returns 0 when that cannot be told. */
static int rm_loc(TCCIRState *ir, int32_t vr, uint32_t *regs, int *off, int *is_stack, int *size)
{
  IRLiveInterval *li = tcc_ir_try_get_live_interval(ir, vr);
  if (!li)
    return 0;
  unsigned r0 = li->allocation.r0, r1 = li->allocation.r1;
  *regs = 0;
  *off = 0;
  *is_stack = 0;
  *size = li->is_llong || li->is_double || li->is_complex ? 8 : 4;
  if (r0 != PREG_NONE && !(r0 & PREG_SPILLED) && li->allocation.offset == 0)
  {
    if (!LS_IS_VFP_REG(r0))
      *regs |= 1u << (r0 & PREG_REG_NONE);
    if (r1 != PREG_NONE && r1 != 0xFFFF && !(r1 & PREG_SPILLED) && !LS_IS_VFP_REG(r1))
      *regs |= 1u << (r1 & PREG_REG_NONE);
    return 1;
  }
  if (li->allocation.offset != 0)
  {
    *off = li->allocation.offset;
    *is_stack = 1;
    return 1;
  }
  return 0;
}

/* Does anything in [from, to] write the location of `sel`, or name another
 * value living there? */
static int rm_sel_clobbered(RMState *st, int from, int to, int32_t sel, uint32_t regs, int off, int is_stack)
{
  TCCIRState *ir = st->ir;
  for (int i = from; i <= to; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int s = 0; s < 4; s++)
    {
      IROperand o = cj_slot(ir, q, s);
      if (irop_is_none(o))
        continue;
      int32_t vr = irop_get_vreg(o);
      if (vr < 0)
      {
        if (is_stack && s == 0 && o.tag == IROP_TAG_STACKOFF && !o.is_llocal)
        {
          /* A direct frame write: a scalar store names its bytes, <= 8. */
          if (q->op != TCCIR_OP_STORE || o.btype == IROP_BTYPE_STRUCT || o.is_param)
            return 1;
          int32_t w = irop_get_stack_offset(o);
          int wsz = o.btype == IROP_BTYPE_INT64 || o.btype == IROP_BTYPE_FLOAT64 ? 8 : 4;
          if (w < off + 4 && off < w + wsz)
            return 1;
        }
        continue;
      }
      if (vr == sel)
      {
        if (cj_defines(q, s, o))
          return 1;
        continue;
      }
      uint32_t r;
      int o2, st2, sz2;
      if (!rm_loc(ir, vr, &r, &o2, &st2, &sz2))
      {
        if (is_stack)
          return 1;
        continue; /* not in a register */
      }
      if (!is_stack && (r & regs))
        return 1;
      if (is_stack && st2 && o2 < off + 4 && off < o2 + sz2)
        return 1;
    }
  }
  return 0;
}

/* The operand holding the case value itself at SWITCH_TABLE `w`, or IROP_NONE.
 * The table indexes with `sel - min_val`; for min_val != 0 look through the
 * `idx <-- sel SUB #min_val` right before the dispatch. */
static IROperand rm_case_selector(RMState *st, int w, int64_t min_val)
{
  TCCIRState *ir = st->ir;
  IRQuadCompact *qw = &ir->compact_instructions[w];
  IROperand idx = tcc_ir_op_get_src1(ir, qw);
  if (!rm_sel_operand_ok(idx))
    return IROP_NONE;
  if (min_val == 0)
    return idx;
  if (min_val != (int32_t)min_val || TCCIR_DECODE_VREG_TYPE(irop_get_vreg(idx)) != TCCIR_VREG_TYPE_TEMP)
    return IROP_NONE;
  for (int p = w; p-- > 0;)
  {
    IRQuadCompact *q = &ir->compact_instructions[p];
    if (ir->compact_instructions[p + 1].is_jump_target || st->trefs[p + 1] || st->jrefs[p + 1])
      return IROP_NONE; /* another way in between the index and the dispatch */
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_IJUMP || rm_is_call_op(q->op))
      return IROP_NONE;
    IROperand d = irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE;
    if (irop_get_vreg(d) != irop_get_vreg(idx))
      continue;
    if (q->op != TCCIR_OP_SUB || !cj_defines(q, 0, d) || d.btype != IROP_BTYPE_INT32)
      return IROP_NONE;
    IROperand x = tcc_ir_op_get_src1(ir, q), k = tcc_ir_op_get_src2(ir, q);
    if (k.tag != IROP_TAG_IMM32 || k.btype == IROP_BTYPE_STRUCT || k.u.imm32 != (int32_t)min_val ||
        !rm_sel_operand_ok(x) || tcc_ir_barrel_shift_at(ir, q))
      return IROP_NONE;
    uint32_t regs;
    int off, is_stack, size;
    if (!rm_loc(ir, irop_get_vreg(x), &regs, &off, &is_stack, &size) ||
        rm_sel_clobbered(st, p, w - 1, irop_get_vreg(x), regs, off, is_stack))
      return IROP_NONE;
    return x;
  }
  return IROP_NONE;
}

static int rm_imm_is_case(int32_t imm, int64_t cv)
{
  if (cv == RM_NOCASE || cv == RM_MULTI)
    return 0;
  return (int64_t)imm == cv || (int64_t)(uint32_t)imm == cv;
}

/* qa/qb differ only in one immediate that is each copy's own case value, read
 * by an op that can take the selector in its place instead. */
static int rm_insn_same_case(RMState *st, int a, int b, int ia, IRQuadCompact *qa, IRQuadCompact *qb)
{
  TCCIRState *ir = st->ir;
  if (!st->caseval || qa->op != qb->op ||
      (qa->op != TCCIR_OP_LOAD && qa->op != TCCIR_OP_STORE && qa->op != TCCIR_OP_ASSIGN))
    return 0;
  if (st->selsw[a] < 0 || st->selsw[a] != st->selsw[b] || irop_is_none(st->swsel[st->selsw[a]]))
    return 0;
  if (!cj_operand_same(ir, cj_slot(ir, qa, 0), cj_slot(ir, qb, 0)))
    return 0;
  IROperand oa = cj_slot(ir, qa, 1), ob = cj_slot(ir, qb, 1);
  if (oa.tag != IROP_TAG_IMM32 || ob.tag != IROP_TAG_IMM32 || oa.btype == IROP_BTYPE_STRUCT ||
      irop_get_vreg(oa) >= 0 || irop_get_vreg(ob) >= 0 || oa.btype != ob.btype || oa.is_lval || ob.is_lval ||
      oa.is_llocal != ob.is_llocal || oa.is_local != ob.is_local || oa.is_const != ob.is_const ||
      oa.is_complex != ob.is_complex || oa.is_unsigned != ob.is_unsigned || oa.is_static != ob.is_static ||
      oa.is_sym || ob.is_sym || oa.is_param != ob.is_param || oa.aux != ob.aux)
    return 0;
  if (!rm_imm_is_case(oa.u.imm32, st->caseval[a]) || !rm_imm_is_case(ob.u.imm32, st->caseval[b]))
    return 0;
  if (tcc_ir_barrel_shift_at(ir, qa) || tcc_ir_barrel_shift_at(ir, qb) || tcc_ir_shift64_dead_half_at(ir, qa) ||
      tcc_ir_shift64_dead_half_at(ir, qb) || tcc_ir_zero_half64_at(ir, qa) || tcc_ir_zero_half64_at(ir, qb) ||
      tcc_ir_bfi_params_at(ir, qa) != tcc_ir_bfi_params_at(ir, qb))
    return 0;
  if (st->nparam >= RM_MAX_PARAMS)
    return 0;
  st->param_at[st->nparam] = ia;
  st->param_slot[st->nparam] = 1;
  st->nparam++;
  return 1;
}

/* A match with case-constant differences (region A = st->ia[0..k-1]): may A
 * read the selector in place of its constants?  On success A is recorded as
 * parameterised. */
static int rm_case_accept(RMState *st, int a, int k)
{
  TCCIRState *ir = st->ir;
  int end_a = st->ia[k - 1];
  if (st->exact_in[a])
    return 0; /* a copy that is not this case already runs A */
  if (st->npar[a])
  {
    /* A already reads the selector: the same places, nothing more. */
    if (st->nparam != st->npar[a])
      return 0;
    for (int p = 0; p < st->nparam; p++)
      if (st->pslot[st->param_at[p]] != st->param_slot[p] || st->pown[st->param_at[p]] != a)
        return 0;
    return 1;
  }
  for (int t = a; t <= end_a; t++)
    if (st->pown[t] >= 0 || (t > a && (st->exact_in[t] || st->trefs[t])))
      return 0;
  /* Inner jump targets of A reached only from inside A. */
  for (int t = a + 1; t <= end_a; t++)
  {
    if (!st->jrefs[t])
      continue;
    int inside = 0;
    for (int m = 0; m < k; m++)
    {
      IRQuadCompact *q = &ir->compact_instructions[st->ia[m]];
      if (rm_is_jump(q->op) && irop_get_imm32(tcc_ir_op_get_dest(ir, q)) == t)
        inside++;
    }
    if (inside != st->jrefs[t])
      return 0;
  }
  /* The selector's location is reserved over A and written by nothing in it. */
  IROperand sel = st->swsel[st->selsw[a]];
  int32_t vr = irop_get_vreg(sel);
  uint32_t regs;
  int off, is_stack, size;
  if (!rm_loc(ir, vr, &regs, &off, &is_stack, &size))
    return 0;
  IRLiveInterval *li = tcc_ir_try_get_live_interval(ir, vr);
  if (!li || li->addrtaken || li->start > (uint32_t)a || li->end < (uint32_t)end_a)
    return 0;
  if (!is_stack)
  {
    if (regs & ~0x0FF0u)
      return 0; /* only callee-saved r4-r11 survive what A may call */
    if (ir->ls.live_regs_by_instruction)
      for (int t = a; t <= end_a; t++)
        if (t >= ir->ls.live_regs_by_instruction_size || (ir->ls.live_regs_by_instruction[t] & regs) != regs)
          return 0;
  }
  if (rm_sel_clobbered(st, a, end_a, vr, regs, off, is_stack))
    return 0;
  for (int p = 0; p < st->nparam; p++)
    st->pslot[st->param_at[p]] = (signed char)st->param_slot[p];
  for (int t = a; t <= end_a; t++)
    st->pown[t] = a;
  st->npar[a] = st->nparam;
  int key = cj_vkey(ir, vr);
  if (key >= 0)
    st->occ[key] += st->nparam; /* the new reads keep regions from pairing it */
  return 1;
}

/* The instruction before `i` (NOPs skipped) never falls into it. */
static int rm_no_fallthrough(TCCIRState *ir, int i)
{
  for (int p = i - 1; p >= 0; p--)
  {
    int op = ir->compact_instructions[p].op;
    if (op == TCCIR_OP_NOP)
      continue;
    return op == TCCIR_OP_JUMP || op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID ||
           op == TCCIR_OP_SWITCH_TABLE;
  }
  return 1;
}

/* Match the region at entry `a` against the one at entry `b` (a < b).
 * Returns the number of matched instructions, 0 if they differ. */
static int rm_match(RMState *st, int a, int b)
{
  TCCIRState *ir = st->ir;
  const int n = st->n;
  int ia = a, ib = b, k = 0, pend = a, seen_producer = 0, shared_min = -1;
  st->nparam = 0;
  for (;;)
  {
    ia = rm_next(ir, ia, n);
    ib = rm_next(ir, ib, n);
    if (ia >= b || ib >= n || k >= RM_MAX_LEN || st->dead[ia] || st->dead[ib])
      return 0;
    IRQuadCompact *qa = &ir->compact_instructions[ia], *qb = &ir->compact_instructions[ib];
    if (!rm_insn_same(ir, qa, qb) && !rm_insn_same_case(st, a, b, ia, qa, qb))
      return 0;
    if (cj_reads_flags(qa->op) && !seen_producer)
      return 0;
    if (cj_sets_flags(qa->op))
      seen_producer = 1;
    if (rm_is_jump(qa->op))
    {
      int ta = irop_get_imm32(tcc_ir_op_get_dest(ir, qa)), tb = irop_get_imm32(tcc_ir_op_get_dest(ir, qb));
      if (ta != tb)
      {
        /* Both inside their own copy, at the same position. */
        if (ta < a || ta >= b || tb < b || tb >= n || st->ord[ta] - st->ord[a] != st->ord[tb] - st->ord[b])
          return 0;
        if (ta > pend)
          pend = ta;
      }
      else if (ta >= b && (shared_min < 0 || ta < shared_min))
        shared_min = ta; /* must lie past the copy being removed */
    }
    st->ia[k] = ia;
    st->ib[k] = ib;
    k++;
    if (rm_ends_region(qa->op) && pend <= ia)
      break;
    ia++;
    ib++;
  }
  if (k < RM_MIN_LEN)
    return 0;
  int end_b = st->ib[k - 1];
  if (shared_min >= 0 && shared_min <= end_b)
    return 0; /* a shared target inside the copy being removed */

  /* Inner jump targets of B reached only from inside B. */
  for (int t = b + 1; t <= end_b; t++)
  {
    if (st->trefs[t])
      return 0;
    if (!st->jrefs[t])
      continue;
    int inside = 0;
    for (int m = 0; m < k; m++)
    {
      IRQuadCompact *q = &ir->compact_instructions[st->ib[m]];
      if (rm_is_jump(q->op) && irop_get_imm32(tcc_ir_op_get_dest(ir, q)) == t)
        inside++;
    }
    if (inside != st->jrefs[t])
      return 0;
  }

  /* Values and calls paired one to one. */
  st->gen++;
  for (int m = 0; m < k; m++)
  {
    IRQuadCompact *qa = &ir->compact_instructions[st->ia[m]], *qb = &ir->compact_instructions[st->ib[m]];
    if (rm_is_call_op(qa->op) &&
        !rm_pair(st, st->cmap_a, st->cmap_b, st->ccnt, st->cstamp_a, st->cstamp_b, rm_call_id(ir, qa),
                 rm_call_id(ir, qb), 1))
      return 0;
    for (int s = 0; s < 4; s++)
    {
      IROperand oa = cj_slot(ir, qa, s), ob = cj_slot(ir, qb, s);
      int va = cj_vkey(ir, irop_get_vreg(oa)), vb = cj_vkey(ir, irop_get_vreg(ob));
      if (va < 0 && vb < 0)
        continue;
      if (va < 0 || vb < 0)
        return 0;
      int paired = st->vstamp_a[va] == st->gen || st->vstamp_b[vb] == st->gen;
      if (!paired && va == vb)
        continue; /* the same value in both */
      if (!rm_pair(st, st->vmap_a, st->vmap_b, st->vcnt, st->vstamp_a, st->vstamp_b, va, vb,
                   cj_defines(qa, s, oa) && cj_defines(qb, s, ob)))
        return 0;
    }
  }
  for (int m = 0; m < k; m++)
  {
    IRQuadCompact *qa = &ir->compact_instructions[st->ia[m]], *qb = &ir->compact_instructions[st->ib[m]];
    if (rm_is_call_op(qa->op))
    {
      int ca = rm_call_id(ir, qa), cb = rm_call_id(ir, qb);
      if (st->ccnt[ca] != st->callocc[ca] || st->ccnt[ca] != st->callocc[cb])
        return 0;
    }
    for (int s = 0; s < 4; s++)
    {
      int va = cj_vkey(ir, irop_get_vreg(cj_slot(ir, qa, s)));
      if (va < 0 || st->vstamp_a[va] != st->gen)
        continue;
      int vb = st->vmap_a[va];
      if (st->vcnt[va] != st->occ[va] || st->vcnt[va] != st->occ[vb])
        return 0; /* used outside its region */
    }
  }
  return k;
}

static unsigned rm_hash(TCCIRState *ir, int i, int n)
{
  unsigned h = 2166136261u;
  for (int c = 0; c < 4; c++)
  {
    i = rm_next(ir, i, n);
    if (i >= n)
      break;
    int op = ir->compact_instructions[i].op;
    h = (h ^ op) * 16777619u;
    if (rm_ends_region(op))
      break; /* what follows belongs to another region */
    i++;
  }
  return h;
}

int tcc_ir_region_merge(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  if (n < 8 || ir->func_has_label_addr)
    return 0;
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;

  RMState st = {0};
  int merged = 0;
  st.ir = ir;
  st.n = n;
  const int nkeys = ir->next_temporary_variable + ir->next_local_variable + ir->next_parameter + 1;
  const int ncalls = ir->next_call_id + 1;
  st.ord = tcc_malloc(sizeof(int) * (n + 1));
  st.occ = tcc_mallocz(sizeof(int) * nkeys);
  st.callocc = tcc_mallocz(sizeof(int) * ncalls);
  st.jrefs = tcc_mallocz(sizeof(int) * n);
  st.trefs = tcc_mallocz(sizeof(int) * n);
  st.dead = tcc_mallocz(n);
  st.ia = tcc_malloc(sizeof(int) * RM_MAX_LEN);
  st.ib = tcc_malloc(sizeof(int) * RM_MAX_LEN);
  st.vmap_a = tcc_malloc(sizeof(int) * nkeys);
  st.vmap_b = tcc_malloc(sizeof(int) * nkeys);
  st.vcnt = tcc_malloc(sizeof(int) * nkeys);
  st.vstamp_a = tcc_mallocz(sizeof(int) * nkeys);
  st.vstamp_b = tcc_mallocz(sizeof(int) * nkeys);
  st.cmap_a = tcc_malloc(sizeof(int) * ncalls);
  st.cmap_b = tcc_malloc(sizeof(int) * ncalls);
  st.ccnt = tcc_malloc(sizeof(int) * ncalls);
  st.cstamp_a = tcc_mallocz(sizeof(int) * ncalls);
  st.cstamp_b = tcc_mallocz(sizeof(int) * ncalls);
  if (!tcc_ir_opt_pass_disabled("region_merge_case") && ir->num_switch_tables > 0)
  {
    st.caseval = tcc_malloc(sizeof(int64_t) * n);
    st.selsw = tcc_malloc(sizeof(int) * n);
    st.swsel = tcc_malloc(sizeof(IROperand) * n);
    st.pslot = tcc_malloc(n);
    st.pown = tcc_malloc(sizeof(int) * n);
    st.npar = tcc_mallocz(sizeof(int) * n);
    st.exact_in = tcc_mallocz(n);
    for (int i = 0; i < n; i++)
    {
      st.caseval[i] = RM_NOCASE;
      st.selsw[i] = -1;
      st.swsel[i] = IROP_NONE;
      st.pslot[i] = -1;
      st.pown[i] = -1;
    }
  }

  int c = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    st.ord[i] = c;
    if (q->op == TCCIR_OP_NOP)
      continue;
    c++;
    for (int s = 0; s < 4; s++)
    {
      int key = cj_vkey(ir, irop_get_vreg(cj_slot(ir, q, s)));
      if (key >= 0)
        st.occ[key]++;
    }
    if (rm_is_call_op(q->op))
    {
      int id = rm_call_id(ir, q);
      if (id < 0 || id >= ncalls)
        goto out; /* unknown encoding: leave the function alone */
      st.callocc[id]++;
    }
    if (rm_is_jump(q->op))
    {
      int t = irop_get_imm32(tcc_ir_op_get_dest(ir, q));
      if (t >= 0 && t < n)
        st.jrefs[t]++;
    }
    if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      int id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (id >= 0 && id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *tab = &ir->switch_tables[id];
        for (int e = 0; e < tab->num_entries; e++)
          if (tab->targets[e] >= 0 && tab->targets[e] < n)
          {
            int t = tab->targets[e];
            st.trefs[t]++;
            if (st.caseval)
            {
              st.caseval[t] = st.caseval[t] == RM_NOCASE ? tab->min_val + e : RM_MULTI;
              st.selsw[t] = i;
            }
          }
        if (tab->default_target >= 0 && tab->default_target < n)
        {
          st.trefs[tab->default_target]++;
          if (st.caseval)
            st.caseval[tab->default_target] = RM_MULTI;
        }
      }
      else if (st.caseval)
        goto out; /* a table we cannot read: leave the function alone */
    }
  }
  st.ord[n] = c;
  if (st.caseval)
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_SWITCH_TABLE)
      {
        TCCIRSwitchTable *tab = &ir->switch_tables[irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q))];
        if (tab->num_entries > 0)
          st.swsel[i] = rm_case_selector(&st, i, tab->min_val);
      }
      if (st.jrefs[i] || !rm_no_fallthrough(ir, i))
        st.caseval[i] = RM_MULTI;
    }

  /* Entries bucketed by the ops they start with. */
  enum { NB = 1024 };
  int *head = tcc_malloc(sizeof(int) * NB), *next = tcc_malloc(sizeof(int) * n);
  for (int b = 0; b < NB; b++)
    head[b] = -1;
  for (int b = 0; b < n; b++)
  {
    IRQuadCompact *qb = &ir->compact_instructions[b];
    if (!qb->is_jump_target && !st.trefs[b])
      continue;
    if (st.dead[b])
      continue;
    unsigned h = rm_hash(ir, b, n) & (NB - 1);
    int done = 0, tries = 0;
    for (int a = head[h]; a >= 0 && !done && tries < RM_BUCKET_TRIES; a = next[a], tries++)
    {
      if (st.dead[a])
        continue;
      int k = rm_match(&st, a, b);
      if (!k)
        continue;
      int fb = st.ib[0];
      IRQuadCompact *qf = &ir->compact_instructions[fb];
      if (!irop_config[qf->op].has_dest && !irop_config[qf->op].has_src1 && !irop_config[qf->op].has_src2)
        continue; /* no operand slot for the jump's target */
      if (st.caseval)
      {
        if (st.nparam ? !rm_case_accept(&st, a, k) : st.pown[a] >= 0)
          continue; /* an exact copy cannot run a region that reads the selector */
        if (!st.nparam)
          st.exact_in[a] = 1;
      }
      for (int m = 1; m < k; m++)
      {
        ir->compact_instructions[st.ib[m]].op = TCCIR_OP_NOP;
        st.dead[st.ib[m]] = 1;
      }
      for (int t = b + 1; t <= st.ib[k - 1]; t++)
        st.dead[t] = 1;
      qf->op = TCCIR_OP_JUMP;
      tcc_ir_op_set_dest(ir, qf, irop_make_imm32(-1, a, IROP_BTYPE_INT32));
      st.dead[fb] = 1;
      ir->compact_instructions[a].is_jump_target = 1;
      merged++;
      done = 1;
    }
    if (!done)
    {
      next[b] = head[h];
      head[h] = b;
    }
  }
  tcc_free(next);
  tcc_free(head);
  /* Kept copies read the selector where they had their case value. */
  if (st.caseval)
    for (int i = 0; i < n; i++)
      if (st.pslot[i] >= 0)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        tcc_ir_op_set_src1(ir, q, st.swsel[st.selsw[st.pown[i]]]);
      }
out:
  tcc_free(st.exact_in);
  tcc_free(st.npar);
  tcc_free(st.pown);
  tcc_free(st.pslot);
  tcc_free(st.swsel);
  tcc_free(st.selsw);
  tcc_free(st.caseval);
  tcc_free(st.cstamp_b);
  tcc_free(st.cstamp_a);
  tcc_free(st.ccnt);
  tcc_free(st.cmap_b);
  tcc_free(st.cmap_a);
  tcc_free(st.vstamp_b);
  tcc_free(st.vstamp_a);
  tcc_free(st.vcnt);
  tcc_free(st.vmap_b);
  tcc_free(st.vmap_a);
  tcc_free(st.ib);
  tcc_free(st.ia);
  tcc_free(st.dead);
  tcc_free(st.trefs);
  tcc_free(st.jrefs);
  tcc_free(st.callocc);
  tcc_free(st.occ);
  tcc_free(st.ord);
  return merged;
}
