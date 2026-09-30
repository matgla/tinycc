/*
 *  TCC IR - build a returned struct in the caller's buffer (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A function returning a struct through the hidden pointer P0 builds the value
 * in a local and copies it out at the return: the Zig C backend's
 * `t10 = t0; return t10;`, a 96-byte memmove at the end of every Wyhash.init.
 * When the local -- and a local that is copied into it whole and then dies,
 * t0 here -- is only ever reached through its own frame address, the object
 * lives in *P0 instead: its address becomes P0 + k, a direct slot access a
 * deref of a fresh `T <- P0 ADD #k`, and the copies out and between the two
 * objects go away.
 *
 * The caller may pass a buffer the callee can see through another pointer
 * (`*x = f(x)` passes x twice), so while the object is being built nothing
 * else may touch memory: from the first reference to either object to the
 * copy out there is no call except copies/fills between frame objects and
 * .rodata, no deref of anything but the object's own addresses, and no global
 * access.  After the copy only the return follows. */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_alias.h"

#define NRVO_MAX_OBJ 4
#define NRVO_MAX_REFS 512

/* TCC_NRVO_DBG: the line of the check that refused a function. */
TCC_DBG_ENV_FLAG(nrvo_dbg, "TCC_NRVO_DBG")
#define NRVO_FAIL()                                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    if (nrvo_dbg())                                                                                                    \
      fprintf(stderr, "[NRVO] %s: refused at line %d\n", funcname, __LINE__);                                          \
    return 0;                                                                                                          \
  } while (0)

static int nrvo_is_call(int op)
{
  return op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL;
}

static const char *nrvo_callee(TCCIRState *ir, IRQuadCompact *q)
{
  Sym *s = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  return s ? get_tok_str(s->v, NULL) : NULL;
}

static int nrvo_is_memop_name(const char *nm)
{
  return nm && (ir_opt_is_memcpy_or_memmove_name(nm) || !strcmp(nm, "__aeabi_memset") || !strcmp(nm, "memset"));
}

/* A frame address operand (not an access through the frame). */
static int nrvo_frame_addr(IROperand o)
{
  return irop_get_tag(o) == IROP_TAG_STACKOFF && o.is_local && !o.is_llocal && !o.is_lval && irop_get_vreg(o) < 0;
}

/* A direct access to a frame slot. */
static int nrvo_frame_slot(IROperand o)
{
  return irop_get_tag(o) == IROP_TAG_STACKOFF && o.is_local && !o.is_llocal && o.is_lval && irop_get_vreg(o) < 0;
}

/* The single def of TEMP `v` before `at`, or -1. */
static int nrvo_temp_def(TCCIRState *ir, int32_t v, int at)
{
  if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  int found = -1;
  for (int j = 0; j < ir->next_instruction_index; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval || irop_get_vreg(d) != v)
      continue;
    if (found >= 0 || j >= at)
      return -1;
    found = j;
  }
  return found;
}

/* What a call argument holds: a frame address (1, *off set), a symbol address
 * (2), P0 or a copy of it (3), else 0. */
static int nrvo_arg_kind(TCCIRState *ir, IROperand o, int at, int32_t p0, int *off)
{
  if (nrvo_frame_addr(o))
  {
    *off = irop_get_stack_offset(o);
    return 1;
  }
  if (irop_get_tag(o) == IROP_TAG_SYMREF && !o.is_lval)
    return 2;
  int32_t v = irop_get_vreg(o);
  if (o.is_lval || v < 0)
    return 0;
  if (v == p0)
    return 3;
  int d = nrvo_temp_def(ir, v, at);
  if (d < 0)
    return 0;
  IRQuadCompact *q = &ir->compact_instructions[d];
  if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA)
    return 0;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  if (nrvo_frame_addr(s))
  {
    *off = irop_get_stack_offset(s);
    return 1;
  }
  if (irop_get_tag(s) == IROP_TAG_SYMREF && !s.is_lval)
    return 2;
  int32_t sv = irop_get_vreg(s);
  if (!s.is_lval && sv == p0)
    return 3;
  /* P0's copy in a VAR, or the frontend's sret home slot */
  if (!s.is_lval && sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_VAR)
  {
    int defs = 0, ok = 1;
    for (int j = 0; j < ir->next_instruction_index && ok; j++)
    {
      IRQuadCompact *vq = &ir->compact_instructions[j];
      if (vq->op == TCCIR_OP_NOP || !irop_config[vq->op].has_dest || !irop_config[vq->op].has_src1)
        continue;
      IROperand vd = tcc_ir_op_get_dest(ir, vq);
      if (irop_get_vreg(vd) != sv)
        continue;
      defs++;
      IROperand vs = tcc_ir_op_get_src1(ir, vq);
      if (vs.is_lval || irop_get_vreg(vs) != p0)
        ok = 0;
    }
    return ok && defs == 1 ? 3 : 0;
  }
  if (nrvo_frame_slot(s) && irop_get_stack_offset(s) == (int)func_vc)
    return 3;
  return 0;
}

/* The CALL's arguments 0..2 and whether they are PARAM instructions we can find. */
static int nrvo_call_args(TCCIRState *ir, int call, IROperand a[3])
{
  for (int k = 0; k < 3; k++)
    if (!ir_opt_get_call_param_operand(ir, call, k, &a[k]))
      return 0;
  return irop_get_tag(a[2]) == IROP_TAG_IMM32;
}

typedef struct
{
  int lo, size; /* frame extent */
} NrvoObj;

static int nrvo_obj_of(const NrvoObj *o, int no, int off)
{
  for (int k = 0; k < no; k++)
    if (off >= o[k].lo && off < o[k].lo + o[k].size)
      return k;
  return -1;
}

/* Operand k of q (0 dest, 1 src1, 2 src2, 3 accum). */
static int nrvo_operand(TCCIRState *ir, IRQuadCompact *q, int k, IROperand *op)
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

static void nrvo_set_operand(TCCIRState *ir, IRQuadCompact *q, int k, IROperand op)
{
  if (k == 0)
    tcc_ir_op_set_dest(ir, q, op);
  else if (k == 1)
    tcc_ir_op_set_src1(ir, q, op);
  else if (k == 2)
    tcc_ir_op_set_src2(ir, q, op);
  else
    tcc_ir_op_set_accum(ir, q, op);
}

/* `t <- P0 ADD #k` (or `t <- P0` for k == 0), inserted before `at` so that
 * every jump to `at` lands on it (tcc_ir_insert_instruction_before moves them
 * past it). */
static int32_t nrvo_insert_addr(TCCIRState *ir, int at, int32_t p0, int k)
{
  int32_t t = tcc_ir_get_vreg_temp(ir);
  IRQuadCompact q = {0};
  q.op = k ? TCCIR_OP_ADD : TCCIR_OP_ASSIGN;
  q.operand_base = tcc_ir_pool_add(ir, irop_make_vreg(t, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(p0, IROP_BTYPE_INT32));
  if (k)
    tcc_ir_pool_add(ir, irop_make_imm32(-1, k, IROP_BTYPE_INT32));
  const int target = ir->compact_instructions[at].is_jump_target;
  tcc_ir_insert_instruction_before(ir, at, &q);
  if (target)
  {
    ir->compact_instructions[at].is_jump_target = 1;
    ir->compact_instructions[at + 1].is_jump_target = 0;
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[i];
      if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
        continue;
      if ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jq)) == at + 1)
        tcc_ir_op_set_dest(ir, jq, irop_make_imm32(-1, at, IROP_BTYPE_INT32));
    }
  }
  return t;
}


/* A .rodata/.data symbol argument at a word-aligned address (the check the
 * backend's inline copy makes, arm-thumb-gen.c thumb_copy_arg_word_aligned). */
static int nrvo_sym_word_aligned(TCCIRState *ir, IROperand op)
{
  IRPoolSymref *sr = irop_get_symref_ex(ir, op);
  if (!sr || !sr->sym)
    return 0;
  ElfSym *esym = elfsym(sr->sym);
  if (!esym || esym->st_shndx == SHN_UNDEF || esym->st_shndx >= SHN_LORESERVE ||
      esym->st_shndx >= (unsigned)tcc_state->nb_sections || ELFW(ST_BIND)(esym->st_info) != STB_LOCAL)
    return 0;
  Section *sec = tcc_state->sections[esym->st_shndx];
  return sec && sec->sh_addralign >= 4 && !((esym->st_value + (addr_t)sr->addend) & 3);
}

/* The source of a TEMP argument (its ASSIGN/LEA def), else the argument. */
static IROperand nrvo_arg_src(TCCIRState *ir, IROperand o, int at)
{
  int d = nrvo_temp_def(ir, irop_get_vreg(o), at);
  if (d >= 0 && (ir->compact_instructions[d].op == TCCIR_OP_ASSIGN || ir->compact_instructions[d].op == TCCIR_OP_LEA))
    return tcc_ir_op_get_src1(ir, &ir->compact_instructions[d]);
  return o;
}

/* A zero fill of one of the objects, followed by a straight run of stores and
 * copies into them: only the words the run leaves unwritten need the zeros.
 * Those become direct `StackLoc[w] <-- #0` stores (rewritten into *P0 with
 * the rest) and the call goes -- a 96-byte __aeabi_memset call in front of
 * Wyhash.init's field stores otherwise, where block_copy_init had made the
 * frame copy one image copy.  Returns the number of instructions inserted
 * before `site`. */
static int nrvo_lower_fills(TCCIRState *ir, const NrvoObj *obj, int nobj, int32_t p0, int site)
{
  int shift = 0;
  for (int i = site - 1; i >= 0; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!nrvo_is_call(q->op))
      continue;
    const char *nm = nrvo_callee(ir, q);
    int aeabi = nm && !strcmp(nm, "__aeabi_memset");
    if (!nm || (!aeabi && strcmp(nm, "memset")))
      continue;
    IROperand a[3];
    int off = 0;
    for (int k = 0; k < 3; k++)
      if (!ir_opt_get_call_param_operand(ir, i, k, &a[k]))
        goto next;
    {
      IROperand sz = aeabi ? a[1] : a[2], val = aeabi ? a[2] : a[1];
      if (irop_get_tag(sz) != IROP_TAG_IMM32 || irop_get_tag(val) != IROP_TAG_IMM32 || irop_get_imm64_ex(ir, val) != 0)
        continue;
      if (nrvo_arg_kind(ir, a[0], i, p0, &off) != 1)
        continue;
      int ob = nrvo_obj_of(obj, nobj, off);
      int n = (int)irop_get_imm64_ex(ir, sz);
      if (ob < 0 || (off & 3) || (n & 3) || n <= 0 || n > 256 || off + n > obj[ob].lo + obj[ob].size)
        continue;
      uint8_t cov[256];
      memset(cov, 0, sizeof cov);
      for (int j = i + 1; j < site; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_NOP || jq->op == TCCIR_OP_FUNCPARAMVAL || jq->op == TCCIR_OP_FUNCPARAMVOID)
          continue;
        if (jq->op == TCCIR_OP_ASSIGN || jq->op == TCCIR_OP_LEA)
        {
          /* an address TEMP for a copy below */
          IROperand d = tcc_ir_op_get_dest(ir, jq), sr = tcc_ir_op_get_src1(ir, jq);
          if (!d.is_lval && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP &&
              (nrvo_frame_addr(sr) || (irop_get_tag(sr) == IROP_TAG_SYMREF && !sr.is_lval)))
            continue;
          break;
        }
        if (jq->op == TCCIR_OP_STORE)
        {
          IROperand d = tcc_ir_op_get_dest(ir, jq), v = tcc_ir_op_get_src1(ir, jq);
          if (!nrvo_frame_slot(d) || v.is_lval)
            break;
          int w = irop_is_64bit(d) ? 8 : ir_opt_store_btype_size_bytes(irop_get_btype(d));
          int so = irop_get_stack_offset(d);
          for (int b = 0; b < w; b++)
            if (so + b >= off && so + b < off + n)
              cov[so + b - off] = 1;
          continue;
        }
        if (nrvo_is_call(jq->op) && ir_opt_is_memcpy_or_memmove_name(nrvo_callee(ir, jq)))
        {
          IROperand c[3];
          int co = 0, so2 = 0;
          if (!nrvo_call_args(ir, j, c) || nrvo_arg_kind(ir, c[0], j, p0, &co) != 1)
            break;
          int sk = nrvo_arg_kind(ir, c[1], j, p0, &so2);
          if (sk != 2 && !(sk == 1 && nrvo_obj_of(obj, nobj, so2) < 0))
            break; /* reads the object, or some pointer */
          int cn = (int)irop_get_imm64_ex(ir, c[2]);
          for (int b = 0; b < cn; b++)
            if (co + b >= off && co + b < off + n)
              cov[co + b - off] = 1;
          continue;
        }
        break;
      }
      int ins = 0;
      for (int w = n - 4; w >= 0; w -= 4)
      {
        if (cov[w] && cov[w + 1] && cov[w + 2] && cov[w + 3])
          continue;
        IRQuadCompact st = {0};
        st.op = TCCIR_OP_STORE;
        st.operand_base = tcc_ir_pool_add(ir, irop_make_stackoff(-1, off + w, 1, 0, 0, IROP_BTYPE_INT32));
        tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
        int pfirst = i;
        for (int k = 0; k < 3; k++)
        {
          int pi = ir_opt_get_call_param_index(ir, i, k);
          if (pi >= 0 && pi < pfirst)
            pfirst = pi;
        }
        tcc_ir_insert_instruction_before(ir, pfirst, &st);
        ins++;
      }
      int call = i + ins;
      ir_opt_nop_call_params(ir, call);
      ir->compact_instructions[call].op = TCCIR_OP_NOP;
      shift += ins;
      site += ins;
    }
  next:;
  }
  return shift;
}

int tcc_ir_opt_sret_nrvo(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (!func_vc || n == 0 || ir->inline_asm_count || ir->func_has_label_addr || tcc_ir_calls_returns_twice(ir) ||
      ir->num_switch_tables)
    NRVO_FAIL();
  const int32_t p0 = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0);

  /* The one copy out: memmove/memcpy(sret, &obj, N), then only the return. */
  int site = -1;
  NrvoObj obj[NRVO_MAX_OBJ];
  int nobj = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* P0 written, or read other than by copying it: not ours to reason about */
    int32_t dv = irop_config[q->op].has_dest ? irop_get_vreg(tcc_ir_op_get_dest(ir, q)) : -1;
    if (dv == p0 && q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      NRVO_FAIL();
    if (!nrvo_is_call(q->op) || !ir_opt_is_memcpy_or_memmove_name(nrvo_callee(ir, q)))
      continue;
    IROperand a[3];
    int dst_off = 0, src_off = 0;
    if (!nrvo_call_args(ir, i, a))
      continue;
    if (nrvo_arg_kind(ir, a[0], i, p0, &dst_off) != 3)
      continue;
    if (site >= 0 || nrvo_arg_kind(ir, a[1], i, p0, &src_off) != 1)
      NRVO_FAIL(); /* two copies out, or one from somewhere else */
    int size = (int)irop_get_imm64_ex(ir, a[2]);
    if (size <= 0 || (size & 3) || (src_off & 3) || tcc_ir_frame_object_size_at(ir, src_off) != size)
      NRVO_FAIL();
    site = i;
    obj[nobj++] = (NrvoObj){.lo = src_off, .size = size};
  }
  if (site < 0)
    NRVO_FAIL();
  /* then only the return: NOPs, and one jump straight to the return or the end */
  {
    int j = site + 1, jumped = 0;
    while (j < n)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
      {
        j++;
        continue;
      }
      if (q->op == TCCIR_OP_JUMP && !jumped)
      {
        int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
        if (t <= site)
          NRVO_FAIL();
        j = t;
        jumped = 1;
        continue;
      }
      if (q->op != TCCIR_OP_RETURNVOID)
        NRVO_FAIL();
      break;
    }
  }
  const int size = obj[0].size;

  /* Merge a same-sized object copied whole into one of ours, when that copy is
   * the last reference to the source and the first to the destination. */
  for (int round = 0; round < NRVO_MAX_OBJ - 1 && nobj < NRVO_MAX_OBJ; round++)
  {
    int added = 0;
    for (int i = 0; i < site && !added; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (!nrvo_is_call(q->op) || !ir_opt_is_memcpy_or_memmove_name(nrvo_callee(ir, q)))
        continue;
      IROperand a[3];
      int d = 0, s = 0;
      if (!nrvo_call_args(ir, i, a) || nrvo_arg_kind(ir, a[0], i, p0, &d) != 1 ||
          nrvo_arg_kind(ir, a[1], i, p0, &s) != 1 || (int)irop_get_imm64_ex(ir, a[2]) != size)
        continue;
      int od = nrvo_obj_of(obj, nobj, d);
      if (od < 0 || d != obj[od].lo || nrvo_obj_of(obj, nobj, s) >= 0 || (s & 3) ||
          tcc_ir_frame_object_size_at(ir, s) != size)
        continue;
      /* s referenced after i, or d before it (other than through this call's args)? */
      int bad = 0;
      int pfirst = i, argdef[2] = {-1, -1};
      for (int k = 0; k < 3; k++)
      {
        int pi = ir_opt_get_call_param_index(ir, i, k);
        if (pi >= 0 && pi < pfirst)
          pfirst = pi;
      }
      for (int k = 0; k < 2; k++)
        argdef[k] = nrvo_temp_def(ir, irop_get_vreg(a[k]), i);
      for (int j = 0; j < n && !bad; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        /* the copy itself: its PARAMs, CALL and the address TEMPs they pass */
        if (jq->op == TCCIR_OP_NOP || (j >= pfirst && j <= i) || j == argdef[0] || j == argdef[1])
          continue;
        for (int k = 0; k < 4 && !bad; k++)
        {
          IROperand o;
          if (!nrvo_operand(ir, jq, k, &o) || irop_get_tag(o) != IROP_TAG_STACKOFF || irop_get_vreg(o) >= 0)
            continue;
          int off = irop_get_stack_offset(o);
          if (off >= s && off < s + size && j > i)
            bad = 1;
          if (off >= d && off < d + size && j < pfirst)
            bad = 1;
        }
      }
      if (bad)
        continue;
      obj[nobj++] = (NrvoObj){.lo = s, .size = size};
      added = 1;
    }
    if (!added)
      break;
  }

  /* Every reference to the objects, and the window they are built in. */
  int first = site;
  int nrefs = 0;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 4; k++)
    {
      IROperand o;
      if (!nrvo_operand(ir, q, k, &o) || irop_get_tag(o) != IROP_TAG_STACKOFF || !o.is_local || o.is_llocal ||
          irop_get_vreg(o) >= 0)
        continue;
      int ob = nrvo_obj_of(obj, nobj, irop_get_stack_offset(o));
      if (ob < 0)
        continue;
      if (j > site)
        NRVO_FAIL();
      if (j < first)
        first = j;
      if ((o.is_lval && irop_get_btype(o) == IROP_BTYPE_STRUCT) || q->op == TCCIR_OP_BLOCK_COPY || o.is_complex ||
          tcc_ir_access_is_volatile(ir, o))
      {
        if (nrvo_dbg())
          fprintf(stderr, "[NRVO] %s: instr %d op %s operand %d btype %d\n", funcname, j, tcc_ir_get_op_name(q->op), k,
                  irop_get_btype(o));
        NRVO_FAIL(); /* whole-object value uses, block copies: not rewritten here */
      }
      if (!o.is_lval && q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ADD &&
          q->op != TCCIR_OP_FUNCPARAMVAL)
        NRVO_FAIL();
      /* a write only by a STORE; a read anywhere (an ALU operand is loaded
       * into a TEMP first) */
      if (o.is_lval && k == 0 && q->op != TCCIR_OP_STORE)
        NRVO_FAIL();
      if (++nrefs > NRVO_MAX_REFS)
        NRVO_FAIL();
    }
  }

  /* TEMPs and VARs holding one of the objects' addresses: derefs through them
   * are ours.  Zig keeps field addresses in pointer locals (`t1 = &t0.state;
   * t2 = &t1->array[0]; *t2 = x`), so a VAR counts when every def of it is
   * such an address. */
  int ntemps = ir->next_temporary_variable + 1, nvars = ir->next_local_variable + 1;
  /* own[] for TEMPs, then VARs.  Greatest fixpoint: every vreg with a def is
   * assumed owned, and loses it once any of its defs is not an address
   * derivation from an owned address (a loop's `p = p + 8` keeps it). */
  uint8_t *own = tcc_malloc(ntemps + nvars), *hasdef = tcc_mallocz(ntemps + nvars);
#define NRVO_IDX(v)                                                                                                    \
  ((v) < 0 ? -1                                                                                                        \
   : TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP                                                                 \
       ? (TCCIR_DECODE_VREG_POSITION(v) < ntemps ? TCCIR_DECODE_VREG_POSITION(v) : -1)                               \
   : TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR                                                                  \
       ? (TCCIR_DECODE_VREG_POSITION(v) < nvars ? ntemps + TCCIR_DECODE_VREG_POSITION(v) : -1)                       \
       : -1)
#define NRVO_OWNED(v) (NRVO_IDX(v) >= 0 && own[NRVO_IDX(v)])
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int x = (d.is_lval || irop_get_tag(d) != IROP_TAG_VREG) ? -1 : NRVO_IDX(irop_get_vreg(d));
    if (x >= 0)
      hasdef[x] = 1;
  }
  memcpy(own, hasdef, ntemps + nvars);
  int converged = 0;
  for (int round = 0; round < 32 && !converged; round++)
  {
    converged = 1;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int x = (d.is_lval || irop_get_tag(d) != IROP_TAG_VREG) ? -1 : NRVO_IDX(irop_get_vreg(d));
      if (x < 0 || !own[x])
        continue;
      int from_own = 0;
      if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB ||
          q->op == TCCIR_OP_STORE)
        for (int k = 1; k <= 2 && !from_own; k++)
        {
          IROperand s2;
          if (!nrvo_operand(ir, q, k, &s2) || (k == 2 && q->op == TCCIR_OP_SUB))
            continue;
          /* a VAR's value is read through its slot (is_lval); anything else
           * with is_lval is a load, not an address */
          const int32_t sv2 = irop_get_vreg(s2);
          const int var_value = sv2 >= 0 && TCCIR_DECODE_VREG_TYPE(sv2) == TCCIR_VREG_TYPE_VAR && !s2.is_llocal;
          if (s2.is_lval && !var_value)
            continue;
          if ((nrvo_frame_addr(s2) && nrvo_obj_of(obj, nobj, irop_get_stack_offset(s2)) >= 0) ||
              NRVO_OWNED(irop_get_vreg(s2)))
            from_own = 1;
        }
      if (!from_own)
      {
        own[x] = 0;
        converged = 0;
        if (nrvo_dbg() && getenv("TCC_NRVO_OWNDBG"))
          fprintf(stderr, "[NRVO]   lose %s%d at %d op %s\n", x < ntemps ? "T" : "V", x < ntemps ? x : x - ntemps, j,
                  tcc_ir_get_op_name(q->op));
      }
    }
  }
  if (!converged)
    memset(own, 0, ntemps + nvars);

  int ok = 1;
  for (int j = first; j < site && ok; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_INLINE_ASM || q->op == TCCIR_OP_ASM_INPUT || q->op == TCCIR_OP_ASM_OUTPUT)
      ok = 0;
    if (nrvo_is_call(q->op))
    {
      /* a copy or fill among frame objects and .rodata only */
      IROperand a[3];
      int o0 = 0, o1 = 0;
      if (!nrvo_is_memop_name(nrvo_callee(ir, q)) || !nrvo_call_args(ir, j, a))
      {
        ok = 0;
        break;
      }
      int k0 = nrvo_arg_kind(ir, a[0], j, p0, &o0), k1 = nrvo_arg_kind(ir, a[1], j, p0, &o1);
      int fill = strcmp(nrvo_callee(ir, q), "__aeabi_memset") == 0 || strcmp(nrvo_callee(ir, q), "memset") == 0;
      if (k0 != 1 || (!fill && k1 != 1 && k1 != 2))
      {
        ok = 0;
        if (nrvo_dbg())
          fprintf(stderr, "[NRVO]   mem call at %d kinds %d %d\n", j, k0, k1);
      }
      continue;
    }
    for (int k = 0; k < 4 && ok; k++)
    {
      IROperand o;
      if (!nrvo_operand(ir, q, k, &o))
        continue;
      if (irop_get_tag(o) == IROP_TAG_SYMREF && o.is_lval)
      {
        ok = 0; /* a global's memory */
        if (nrvo_dbg())
          fprintf(stderr, "[NRVO]   global at %d\n", j);
      }
      int32_t v = irop_get_vreg(o);
      int deref = o.is_lval && irop_get_tag(o) == IROP_TAG_VREG && v >= 0 && TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_VAR;
      if (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC ||
          q->op == TCCIR_OP_LOAD_POSTINC)
        deref |= (k == (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_LOAD_POSTINC ? 1 : 0)) && v >= 0;
      if (deref && !NRVO_OWNED(v))
      {
        ok = 0; /* memory through some other pointer */
        if (nrvo_dbg())
          fprintf(stderr, "[NRVO]   foreign deref at %d operand %d\n", j, k);
      }
    }
  }
  if (!ok && nrvo_dbg())
    fprintf(stderr, "[NRVO] %s: window refused\n", funcname);
#undef NRVO_OWNED
#undef NRVO_IDX
  tcc_free(own);
  tcc_free(hasdef);
  if (!ok)
    NRVO_FAIL();

  site += nrvo_lower_fills(ir, obj, nobj, p0, site);
  n = ir->next_instruction_index;
  first = site;
  for (int j = 0; j < site; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 4 && first > j; k++)
    {
      IROperand o;
      if (nrvo_operand(ir, q, k, &o) && irop_get_tag(o) == IROP_TAG_STACKOFF && o.is_local && !o.is_llocal &&
          irop_get_vreg(o) < 0 && nrvo_obj_of(obj, nobj, irop_get_stack_offset(o)) >= 0)
        first = j;
    }
  }

  /* Copies into the objects keep their inline expansion: the buffer has the
   * return type's alignment, so a word-aligned destination and source make
   * them __aeabi_memcpy4 (the backend expands only copies it can prove
   * aligned, and P0 + k is no frame slot). */
  {
    int align = 1;
    type_size(&func_vt, &align);
    for (int j = first; align >= 4 && j < site; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      const char *nm = nrvo_is_call(q->op) ? nrvo_callee(ir, q) : NULL;
      if (!nm || (strcmp(nm, "__aeabi_memcpy") && strcmp(nm, "__aeabi_memmove") && strcmp(nm, "memcpy") &&
                  strcmp(nm, "memmove")))
        continue;
      IROperand a[3];
      int d = 0, so = 0;
      if (!nrvo_call_args(ir, j, a) || nrvo_arg_kind(ir, a[0], j, p0, &d) != 1 || nrvo_obj_of(obj, nobj, d) < 0)
        continue;
      int sk = nrvo_arg_kind(ir, a[1], j, p0, &so), cn = (int)irop_get_imm64_ex(ir, a[2]);
      int src_ok = (sk == 1 && !(so & 3)) || (sk == 2 && nrvo_sym_word_aligned(ir, nrvo_arg_src(ir, a[1], j)));
      if (((d - obj[nrvo_obj_of(obj, nobj, d)].lo) & 3) || (cn & 3) || !src_ok)
        continue;
      if (!strcmp(nm, "memcpy") || !strcmp(nm, "memmove"))
        continue; /* ISO order is the same, but keep to the AEABI names */
      change_callee_sym_keep_type(ir, j, !strcmp(nm, "__aeabi_memcpy") ? "__aeabi_memcpy4" : "__aeabi_memmove4");
    }
  }

  /* Rewrite, last instruction first so the insertions do not move what is left. */
  int changes = 0;
  int buf_align = 1;
  type_size(&func_vt, &buf_align);
  for (int j = site; j >= first; j--)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 4; k++)
    {
      IROperand o;
      q = &ir->compact_instructions[j];
      if (!nrvo_operand(ir, q, k, &o) || irop_get_tag(o) != IROP_TAG_STACKOFF || !o.is_local || o.is_llocal ||
          irop_get_vreg(o) >= 0)
        continue;
      int off = irop_get_stack_offset(o);
      int ob = nrvo_obj_of(obj, nobj, off);
      if (ob < 0)
        continue;
      const int disp = off - obj[ob].lo;
      if (!o.is_lval && (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA) && k == 1)
      {
        /* `T <- &obj + disp` is `T <- P0 + disp` */
        IROperand dd = tcc_ir_op_get_dest(ir, q);
        IROperand np = irop_make_vreg(p0, IROP_BTYPE_INT32);
        if (disp)
        {
          int base = tcc_ir_pool_add(ir, dd);
          tcc_ir_pool_add(ir, np);
          tcc_ir_pool_add(ir, irop_make_imm32(-1, disp, IROP_BTYPE_INT32));
          q->op = TCCIR_OP_ADD;
          q->operand_base = base;
        }
        else
        {
          int base = tcc_ir_pool_add(ir, dd);
          tcc_ir_pool_add(ir, np);
          q->op = TCCIR_OP_ASSIGN;
          q->operand_base = base;
        }
        changes++;
        continue;
      }
      /* A 64-bit field: an indexed access off P0 directly.  disp.c will not
       * fuse `T <- P0 ADD #k; *T` for 64-bit (LDRD/STRD need word alignment it
       * cannot see), which left an ADD and two STRs per u64 field; here the
       * buffer has the return type's alignment. */
      if (o.is_lval && irop_is_64bit(o) && buf_align >= 4 && !(disp & 3) && disp <= 1020 &&
          ((k == 0 && q->op == TCCIR_OP_STORE) || (k == 1 && q->op == TCCIR_OP_LOAD)))
      {
        IROperand base = irop_make_vreg(p0, IROP_BTYPE_INT32);
        irop_carry_access_marks(&base, o);
        IROperand other = q->op == TCCIR_OP_STORE ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_dest(ir, q);
        if (q->op == TCCIR_OP_LOAD &&
            (other.is_lval || TCCIR_DECODE_VREG_TYPE(irop_get_vreg(other)) != TCCIR_VREG_TYPE_TEMP))
          goto generic;
        int nb = tcc_ir_pool_add(ir, q->op == TCCIR_OP_STORE ? base : other);
        tcc_ir_pool_add(ir, q->op == TCCIR_OP_STORE ? other : base);
        tcc_ir_pool_add(ir, irop_make_imm32(0, disp, IROP_BTYPE_INT32));
        tcc_ir_pool_add(ir, irop_make_imm32(0, 0, IROP_BTYPE_INT32));
        q->op = q->op == TCCIR_OP_STORE ? TCCIR_OP_STORE_INDEXED : TCCIR_OP_LOAD_INDEXED;
        q->operand_base = nb;
        changes++;
        continue;
      }
    generic:;
      /* A field read as an operand of some other op: load it first. */
      if (o.is_lval && k >= 1 && !(k == 1 && (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN)))
      {
        int32_t ta = nrvo_insert_addr(ir, j, p0, disp);
        int32_t tv = tcc_ir_get_vreg_temp(ir);
        IROperand vd = irop_make_vreg(tv, irop_get_btype(o));
        vd.is_unsigned = o.is_unsigned;
        IROperand vs = irop_make_vreg(ta, irop_get_btype(o));
        vs.is_unsigned = o.is_unsigned;
        vs.is_lval = 1;
        IRQuadCompact ld = {0};
        ld.op = TCCIR_OP_LOAD;
        ld.operand_base = tcc_ir_pool_add(ir, vd);
        tcc_ir_pool_add(ir, vs);
        tcc_ir_insert_instruction_before(ir, j + 1, &ld); /* after the address, before q */
        site += 2;
        j += 2;
        q = &ir->compact_instructions[j];
        nrvo_set_operand(ir, q, k, vd);
        changes++;
        continue;
      }
      /* anything else takes the address in a fresh TEMP just before it; a
       * call argument before the call's first PARAM */
      int at = j;
      if (q->op == TCCIR_OP_FUNCPARAMVAL)
      {
        while (at > 0 && ir->compact_instructions[at - 1].op == TCCIR_OP_FUNCPARAMVAL)
          at--;
      }
      int32_t t = nrvo_insert_addr(ir, at, p0, disp);
      site++;
      j++; /* q moved down by one */
      q = &ir->compact_instructions[j];
      IROperand no = irop_make_vreg(t, irop_get_btype(o));
      no.is_lval = o.is_lval;
      no.is_unsigned = o.is_unsigned;
      if (!o.is_lval)
        no.btype = IROP_BTYPE_INT32;
      nrvo_set_operand(ir, q, k, no);
      changes++;
    }
  }

  /* The copies out and between the objects now copy *P0 onto itself. */
  n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!nrvo_is_call(q->op) || !ir_opt_is_memcpy_or_memmove_name(nrvo_callee(ir, q)))
      continue;
    IROperand a[3];
    int d = 0, s = 0;
    if (!nrvo_call_args(ir, i, a) || nrvo_arg_kind(ir, a[0], i, p0, &d) != 3 || nrvo_arg_kind(ir, a[1], i, p0, &s) != 3)
      continue;
    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_NOP;
  }
  if (nrvo_dbg())
    fprintf(stderr, "[NRVO] %s: built in *P0, %d objects, %d rewrites\n", funcname, nobj, changes);
  return changes;
}
