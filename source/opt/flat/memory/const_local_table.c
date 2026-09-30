/*
 *  TCC IR - constant local tables read from .rodata (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A frame object written only by one run of constant stores that covers every
 * byte of it, and whose address is only ever read through, holds the same
 * bytes at every read: reads through its address go to an image of those bytes
 * in .rodata instead.  The Zig C backend indexes an all-constant compound
 * literal with a loop counter -- `(struct arr_8_usize){{4,4,...,2}}.array[i]`,
 * in every MultiArrayList.slice -- which rebuilt the table on the stack on
 * every iteration.
 *
 * The object is the frontend's extent containing the address (before frame
 * relayout).  Its address goes into TEMPs -- by LEA, or folded into an
 * ASSIGN/ADD/SUB operand; every use of such a
 * TEMP, and of TEMPs derived from it by ASSIGN/ADD/SUB, must be a read through
 * it (a deref source, or the base of LOAD_INDEXED) or a further derivation.
 * Any other reference to the object's bytes -- a store outside the run, its
 * address passed or stored, an asm -- keeps it.  A read before the run has
 * executed reads indeterminate memory, so the image serves it too.  Direct
 * frame loads of the object are left alone, and the run with them; with none,
 * the run is deleted. */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_alias.h"
#include "opt_utils.h"

#define CLT_MAX_OBJ 512
#define CLT_MAX_SIZE 65536

typedef struct
{
  int lo, hi;         /* frame extent [lo, hi) */
  int first, last;    /* instruction range of the store run; -1 when none */
  int nstores;
  uint8_t bad;
  uint8_t direct_read;
  int zcall;          /* memset(obj, 0, size) the run starts with; -1 when none */
  int zfirst;         /* its earliest PARAM */
} CltObj;

static int clt_find(const CltObj *objs, int nobj, int off)
{
  for (int k = 0; k < nobj; k++)
    if (off >= objs[k].lo && off < objs[k].hi)
      return k;
  return -1;
}

static int clt_is_temp(int32_t vr)
{
  return vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP;
}

/* Tracked address TEMPs: TEMP position -> object index, -1 untracked. */
typedef struct
{
  int *obj;
  int n;
} CltTemps;

static int clt_temp_obj(const CltTemps *t, int32_t vr)
{
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  return pos < t->n ? t->obj[pos] : -1;
}

/* 1 when newly tracked, 0 when already tracked for `obj`, -1 when tracked for
 * another object. */
static int clt_temp_add(CltTemps *t, int32_t vr, int obj)
{
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (pos >= t->n)
    return -1;
  if (t->obj[pos] >= 0)
    return t->obj[pos] == obj ? 0 : -1;
  t->obj[pos] = obj;
  return 1;
}

static int clt_store_width(IROperand dest)
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

/* A direct frame operand (no vreg): the object's own bytes or address. */
static int clt_direct_frame(IROperand op)
{
  return irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_llocal && irop_get_vreg(op) < 0;
}

/* Operand k of q (0 dest, 1 src1, 2 src2, 3 accum), or 0 when absent. */
static int clt_operand(TCCIRState *ir, IRQuadCompact *q, int k, IROperand *op)
{
  switch (k)
  {
  case 0:
    if (!irop_config[q->op].has_dest)
      return 0;
    *op = tcc_ir_op_get_dest(ir, q);
    return 1;
  case 1:
    if (!irop_config[q->op].has_src1)
      return 0;
    *op = tcc_ir_op_get_src1(ir, q);
    return 1;
  case 2:
    if (!irop_config[q->op].has_src2)
      return 0;
    *op = tcc_ir_op_get_src2(ir, q);
    return 1;
  default:
    if (!tcc_ir_op_is_mac(q->op))
      return 0;
    *op = tcc_ir_op_get_accum(ir, q);
    return 1;
  }
}

/* An instruction putting a frame address into a TEMP: `T <- &slot` (LEA or
 * ASSIGN), or `T <- &slot +/- x` with the address folded into the ADD/SUB.
 * Returns the operand slot holding the address (1 or 2), else 0. */
static int clt_addr_operand(TCCIRState *ir, IRQuadCompact *q, IROperand *addr)
{
  if (q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (d.is_lval || !clt_is_temp(irop_get_vreg(d)))
    return 0;
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  if (clt_direct_frame(s1) && !s1.is_lval)
  {
    *addr = s1;
    return 1;
  }
  if (q->op == TCCIR_OP_ADD)
  {
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    if (clt_direct_frame(s2) && !s2.is_lval)
    {
      *addr = s2;
      return 2;
    }
  }
  return 0;
}

/* A constant STORE into a frame object: returns the object, else -1. */
static int clt_const_store(TCCIRState *ir, IRQuadCompact *q, const CltObj *objs, int nobj)
{
  if (q->op != TCCIR_OP_STORE)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q), s = tcc_ir_op_get_src1(ir, q);
  if (!clt_direct_frame(d) || !d.is_lval || tcc_ir_access_is_volatile(ir, d))
    return -1;
  int k = clt_find(objs, nobj, irop_get_stack_offset(d));
  if (k < 0)
    return -1;
  int w = clt_store_width(d);
  int off = irop_get_stack_offset(d);
  if (!w || off + w > objs[k].hi)
    return -1;
  int tag = irop_get_tag(s);
  if (s.is_lval || s.is_local || s.is_llocal || irop_get_vreg(s) >= 0)
    return -1;
  if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64 && tag != IROP_TAG_F32 && tag != IROP_TAG_F64)
    return -1;
  return k;
}

int tcc_ir_opt_const_local_table(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  if (n == 0 || ir->frame_relaid || !ir->frame_obj_count || ir->inline_asm_count || tcc_ir_calls_returns_twice(ir))
    return 0;

  /* Candidate objects: those whose address goes into a TEMP. */
  CltObj *objs = tcc_malloc(sizeof(CltObj) * CLT_MAX_OBJ);
  int nobj = 0;
  for (int i = 0; i < n && nobj < CLT_MAX_OBJ; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand s;
    if (!clt_addr_operand(ir, q, &s))
      continue;
    int off = irop_get_stack_offset(s), lo, hi;
    if (clt_find(objs, nobj, off) >= 0 || !tcc_ir_frame_object_at(ir, off, &lo, &hi))
      continue;
    /* The extent carries a guard byte and alignment padding; the image covers
     * the object itself -- the largest recorded at that start, should a later
     * scope's object reuse it. */
    int size = -1;
    for (int k = 0; k < ir->frame_obj_count; k++)
      if (ir->frame_objs[4 * k] == lo && ir->frame_objs[4 * k + 2] > size)
        size = ir->frame_objs[4 * k + 2];
    hi = lo + size;
    if (size <= 0 || size > CLT_MAX_SIZE || (lo & 3) || off >= hi)
      continue;
    objs[nobj++] = (CltObj){.lo = lo, .hi = hi, .first = -1, .last = -1, .zcall = -1, .zfirst = -1};
  }
  if (!nobj)
  {
    tcc_free(objs);
    return 0;
  }

  /* The address TEMPs, to a fixpoint: a derivation may precede, in
   * instruction order, the def of the TEMP it derives from. */
  /* Only objects with a constant store can be tables. */
  {
    int any = 0;
    for (int i = 0; i < n; i++)
    {
      int k = clt_const_store(ir, &ir->compact_instructions[i], objs, nobj);
      if (k >= 0)
        objs[k].nstores++;
    }
    for (int k = 0; k < nobj; k++)
    {
      if (!objs[k].nstores)
        objs[k].bad = 1;
      any |= !objs[k].bad;
      objs[k].nstores = 0;
    }
    if (!any)
    {
      tcc_free(objs);
      return 0;
    }
  }

  /* A big initializer is `memset(obj, 0, size)` and then stores of its
   * non-zero bytes only: the zero fill is part of the run. Its address PARAM
   * is the only reference to the object it makes. */
  uint8_t *zmark = tcc_mallocz(n);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID)
      continue;
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
    if (!name || (strcmp(name, "__aeabi_memset") && strcmp(name, "memset")))
      continue;
    IROperand pd, ps, pf;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &pd) || !ir_opt_get_call_param_operand(ir, i, 1, &ps) ||
        !ir_opt_get_call_param_operand(ir, i, 2, &pf))
      continue;
    if (!clt_direct_frame(pd) || pd.is_lval || irop_get_tag(ps) != IROP_TAG_IMM32 ||
        irop_get_tag(pf) != IROP_TAG_IMM32 || irop_get_imm64_ex(ir, pf) != 0)
      continue;
    int k = clt_find(objs, nobj, irop_get_stack_offset(pd));
    if (k < 0 || irop_get_stack_offset(pd) != objs[k].lo || irop_get_imm64_ex(ir, ps) != objs[k].hi - objs[k].lo)
      continue;
    if (objs[k].zcall >= 0)
    {
      objs[k].bad = 1; /* zeroed twice: not one run */
      continue;
    }
    int lo_p = i, ok = 1;
    for (int a = 0; a < 3; a++)
    {
      int pi = ir_opt_get_call_param_index(ir, i, a);
      if (pi < 0)
        ok = 0;
      else if (pi < lo_p)
        lo_p = pi;
    }
    if (!ok)
      continue;
    objs[k].zcall = i;
    objs[k].zfirst = lo_p;
    for (int a = 0; a < 3; a++)
      zmark[ir_opt_get_call_param_index(ir, i, a)] = 1;
    zmark[i] = 1;
  }

  CltTemps temps;
  temps.n = ir->next_temporary_variable + 1;
  temps.obj = tcc_malloc(sizeof(int) * temps.n);
  for (int k = 0; k < temps.n; k++)
    temps.obj[k] = -1;
  int changed = 1;
  for (int rounds = 0; changed && rounds < 8; rounds++)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (!irop_config[q->op].has_dest || !irop_config[q->op].has_src1)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(d);
      if (d.is_lval || !clt_is_temp(dv))
        continue;
      IROperand s1 = tcc_ir_op_get_src1(ir, q), a;
      int k = -1;
      if (clt_addr_operand(ir, q, &a))
        k = clt_find(objs, nobj, irop_get_stack_offset(a));
      else if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
      {
        if (!s1.is_lval && clt_is_temp(irop_get_vreg(s1)))
          k = clt_temp_obj(&temps, irop_get_vreg(s1));
        if (k < 0 && q->op == TCCIR_OP_ADD)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          if (!s2.is_lval && clt_is_temp(irop_get_vreg(s2)))
            k = clt_temp_obj(&temps, irop_get_vreg(s2));
        }
      }
      if (k < 0)
        continue;
      int r = clt_temp_add(&temps, dv, k);
      if (r < 0)
      {
        /* One TEMP holding addresses in two objects (or one past the
         * TEMP table): keep both. */
        int other = clt_temp_obj(&temps, dv);
        objs[k].bad = 1;
        if (other >= 0)
          objs[other].bad = 1;
      }
      changed |= r > 0;
    }
  }

  if (changed)
  {
    /* Not converged: an unchecked derived TEMP could still write. */
    tcc_free(temps.obj);
    tcc_free(objs);
    tcc_free(zmark);
    return 0;
  }

  /* Classify every reference. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || zmark[i])
      continue;
    int sk = clt_const_store(ir, q, objs, nobj);
    if (sk >= 0)
    {
      CltObj *o = &objs[sk];
      if (o->first < 0)
        o->first = i;
      o->last = i;
      o->nstores++;
      continue;
    }
    for (int k = 0; k < 4; k++)
    {
      IROperand op;
      if (!clt_operand(ir, q, k, &op))
        continue;
      if (clt_direct_frame(op))
      {
        int ob = clt_find(objs, nobj, irop_get_stack_offset(op));
        if (ob < 0)
          continue;
        IROperand a;
        if (k != 0 && clt_addr_operand(ir, q, &a) == k)
          continue; /* the address TEMP's uses are checked below */
        if (k != 0 && op.is_lval)
          objs[ob].direct_read = 1;
        else
          objs[ob].bad = 1;
        continue;
      }
      int32_t vr = irop_get_vreg(op);
      if (!clt_is_temp(vr))
        continue;
      int ob = clt_temp_obj(&temps, vr);
      if (ob < 0)
        continue;
      if (k == 0)
      {
        /* A def of the TEMP (by one of the derivations above) is fine; a
         * store through it writes the object. */
        if (op.is_lval)
          objs[ob].bad = 1;
        continue;
      }
      if (op.is_lval)
        continue; /* read through the address */
      if (q->op == TCCIR_OP_LOAD_INDEXED && k == 1)
        continue;
      if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) &&
          clt_temp_obj(&temps, irop_get_vreg(tcc_ir_op_get_dest(ir, q))) == ob &&
          !tcc_ir_op_get_dest(ir, q).is_lval && !(q->op == TCCIR_OP_SUB && k == 2))
        continue;
      objs[ob].bad = 1; /* the address escapes, or is compared or stored */
    }
  }

  int changes = 0;
  uint8_t *image = tcc_malloc(CLT_MAX_SIZE), *written = tcc_malloc(CLT_MAX_SIZE);
  for (int ob = 0; ob < nobj; ob++)
  {
    CltObj *o = &objs[ob];
    if (o->bad || o->first < 0)
      continue;
    const int size = o->hi - o->lo;
    memset(image, 0, size);
    memset(written, o->zcall >= 0, size);
    /* One contiguous run: nothing but this object's constant stores between
     * its first and last -- after its zero fill, when it has one. */
    int ok = o->zcall < 0 || o->zcall < o->first;
    const int run_lo = o->zcall >= 0 ? o->zfirst : o->first;
    for (int i = run_lo; i <= o->last && ok; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (zmark[i] && i <= o->zcall)
      {
        /* one of this fill's PARAMs or its CALL; another call's PARAM
         * interleaved would be a different call nested in the run */
        if (i == o->zcall)
          continue;
        int own = 0;
        for (int a = 0; a < 3; a++)
          own |= ir_opt_get_call_param_index(ir, o->zcall, a) == i;
        if (own)
          continue;
        ok = 0;
        break;
      }
      if (clt_const_store(ir, q, objs, nobj) != ob)
      {
        ok = 0;
        break;
      }
      IROperand d = tcc_ir_op_get_dest(ir, q), s = tcc_ir_op_get_src1(ir, q);
      int rel = irop_get_stack_offset(d) - o->lo, w = clt_store_width(d);
      uint64_t val = (uint64_t)irop_get_imm64_ex(ir, s);
      for (int b = 0; b < w; b++)
      {
        image[rel + b] = (uint8_t)(val >> (8 * b));
        written[rel + b] = 1;
      }
    }
    for (int b = 0; b < size && ok; b++)
      ok = written[b];
    if (!ok)
      continue;

    Sym *sym = bci_image_sym(rodata_section, image, size, NULL, 0);
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      IROperand s;
      int slot = clt_addr_operand(ir, q, &s);
      if (!slot || clt_find(objs, nobj, irop_get_stack_offset(s)) != ob)
        continue;
      uint32_t pidx = tcc_ir_pool_add_symref(ir, sym, irop_get_stack_offset(s) - o->lo, 0);
      IROperand sref = irop_make_symref(-1, pidx, 0, 0, 1, IROP_BTYPE_INT32);
      IROperand d = tcc_ir_op_get_dest(ir, q), s1 = tcc_ir_op_get_src1(ir, q);
      const int has_src2 = q->op != TCCIR_OP_LEA && irop_config[q->op].has_src2;
      IROperand s2 = has_src2 ? tcc_ir_op_get_src2(ir, q) : s1;
      int base = tcc_ir_iroperand_pool_add(ir, d);
      tcc_ir_iroperand_pool_add(ir, slot == 1 ? sref : s1);
      if (has_src2)
        tcc_ir_iroperand_pool_add(ir, slot == 2 ? sref : s2);
      if (q->op == TCCIR_OP_LEA)
        q->op = TCCIR_OP_ASSIGN;
      q->operand_base = base;
      changes++;
    }
    if (!o->direct_read)
    {
      for (int i = o->first; i <= o->last; i++)
        ir->compact_instructions[i].op = TCCIR_OP_NOP;
      if (o->zcall >= 0)
      {
        ir_opt_nop_call_params(ir, o->zcall);
        ir->compact_instructions[o->zcall].op = TCCIR_OP_NOP;
      }
    }
  }
  tcc_free(image);
  tcc_free(written);
  tcc_free(zmark);
  tcc_free(temps.obj);
  tcc_free(objs);
  return changes;
}

TCC_DBG_ENV_FLAG(clt_disabled, "TCC_NO_CONST_LOCAL_TABLE")

int tcc_ir_opt_const_local_table_ex(IROptCtx *ctx)
{
  if (clt_disabled())
    return 0;
  return tcc_ir_opt_const_local_table(ctx->ir);
}
