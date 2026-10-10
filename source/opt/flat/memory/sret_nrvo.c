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
 * `t10 = t0; return t10;`, a 96-byte memmove at the end of every Wyhash.init,
 * and its error unions, `t15 = f(); t13.payload = t15; t13.error = 0;
 * return t13;`.  The local lives in *P0 instead: its address becomes P0 + k,
 * a direct slot access a deref of a fresh `T <- P0 ADD #k`, and the copies
 * out go away.  So do the locals copied whole into it (or into a member of
 * it) whose lifetime ends at that copy -- t0 and t15 above: they live in the
 * bytes of *P0 they are copied into, and a call that returned into one of
 * them returns straight into the caller's buffer.
 *
 * This rests on the convention GCC and LLVM follow: the buffer a caller
 * passes for a struct return is reachable by the callee only through P0 (it
 * is noalias).  Nothing but this function then reads or writes *P0 while it
 * runs, so calls and stores through other pointers may sit anywhere among the
 * object's accesses.  tcc's callers keep their side of it in
 * tcc_ir_sret_dealias (below): a buffer the callee could see some other way --
 * a destination whose address was passed in the same call, stored somewhere,
 * or is only known as a pointer -- is replaced by a fresh temporary copied out
 * after the call.
 *
 * What still has to hold here:
 *   - every copy out (memmove(P0, &obj, N), then only the return) is of the
 *     same object, and P0 or its home slot is read for nothing else;
 *   - when a local is merged into the object, neither its address nor any
 *     other one of the object family's escapes, and on no path does a
 *     reference to one meet a reference to the other's bytes without the copy
 *     between them (CFG reachability, nrvo_merge_ok);
 *   - when one of the objects is passed to a call as ITS struct-return
 *     buffer, the call must be able to reach those bytes only through that
 *     pointer, so no address of the family may escape;
 *   - inline asm (operands invisible to the IR), computed gotos, setjmp,
 *     nested functions (their parent's frame is read by offset) are left
 *     alone. */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "memory/vector.h"

#define NRVO_MAX_OBJ 8
#define NRVO_MAX_SITES 16
#define NRVO_MAX_REFS 512
#define NRVO_MAX_CANDIDATES 64 /* merge candidates looked at per function */

static void nrvo_cache_end(void);

/* TCC_NRVO_DBG: the line of the check that refused a function. */
TCC_DBG_ENV_FLAG(nrvo_dbg, "TCC_NRVO_DBG")
TCC_DBG_ENV_FLAG(nrvo_nocapture_dbg, "TCC_NOCAPTURE_DBG")
/* Refuse the function: say where when TCC_NRVO_DBG asks, close the cache. */
static int nrvo_refuse(int line)
{
  if (nrvo_dbg())
    fprintf(stderr, "[NRVO] %s: refused at line %d\n", funcname, line);
  nrvo_cache_end();
  return 0;
}
#define NRVO_FAIL() return nrvo_refuse(__LINE__)

static int nrvo_is_call(int op)
{
  return ir_op_has(op, IR_HZ_CALL);
}

static const char *nrvo_callee(TCCIRState *ir, IRQuadCompact *q)
{
  Sym *s = tcc_ir_op_src1_sym(ir, q);
  return s ? get_tok_str(s->v, NULL) : NULL;
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

/* While the IR is only analysed (nothing inserted, removed or rewritten), the
 * definitions of TEMPs and what a VAR holds are looked up once, not by a scan
 * of the function each time: the merge analysis asks for them per access,
 * per candidate.  nrvo_cache_begin / nrvo_cache_end bracket those phases. */
static struct
{
  TCCIRState *ir;
  int ntemps, nvars;
  int *tdef;       /* by TEMP: its one definition, -1 none, -2 several */
  int8_t *vstate;  /* by VAR: 0 not looked at, 1 one frame address, 2 not */
  int *voff;       /* by VAR, with vstate 1: that address */
} nrvo_cache;

static void nrvo_cache_begin(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  nrvo_cache.ntemps = ir->next_temporary_variable + 1;
  nrvo_cache.nvars = ir->next_local_variable + 1;
  nrvo_cache.tdef = tcc_malloc(sizeof(int) * nrvo_cache.ntemps);
  for (int k = 0; k < nrvo_cache.ntemps; k++)
    nrvo_cache.tdef[k] = -1;
  nrvo_cache.vstate = tcc_mallocz(nrvo_cache.nvars);
  nrvo_cache.voff = tcc_mallocz(sizeof(int) * nrvo_cache.nvars);
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || q->op == TCCIR_OP_STORE_INDEXED)
      continue;
    const int32_t v = tcc_ir_op_dest_vreg(ir, q);
    if (tcc_ir_op_dest_is_lval(ir, q) || v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP)
      continue;
    const int x = TCCIR_DECODE_VREG_POSITION(v);
    if (x >= nrvo_cache.ntemps)
      continue;
    nrvo_cache.tdef[x] = nrvo_cache.tdef[x] == -1 ? j : -2;
  }
  nrvo_cache.ir = ir;
}

static void nrvo_cache_end(void)
{
  if (!nrvo_cache.ir)
    return;
  tcc_free(nrvo_cache.tdef);
  tcc_free(nrvo_cache.vstate);
  tcc_free(nrvo_cache.voff);
  memset(&nrvo_cache, 0, sizeof nrvo_cache);
}

/* The single def of TEMP `v` before `at`, or -1. */
static int nrvo_temp_def(TCCIRState *ir, int32_t v, int at)
{
  if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  if (nrvo_cache.ir == ir && TCCIR_DECODE_VREG_POSITION(v) < nrvo_cache.ntemps)
  {
    const int d = nrvo_cache.tdef[TCCIR_DECODE_VREG_POSITION(v)];
    return d >= 0 && d < at ? d : -1;
  }
  int found = -1;
  for (int j = 0; j < ir->next_instruction_index; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    /* (a STORE_INDEXED's dest is the base it stores through, not a def) */
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || q->op == TCCIR_OP_STORE_INDEXED)
      continue;
    if (tcc_ir_op_dest_is_lval(ir, q) || tcc_ir_op_dest_vreg(ir, q) != v)
      continue;
    if (found >= 0 || j >= at)
      return -1;
    found = j;
  }
  return found;
}

/* What a call argument holds: a frame address (1, *off set), a symbol address
 * (2), P0 or a copy of it (3), else 0. */
static int nrvo_arg_kind(TCCIRState *ir, IROperand o, int at, int32_t p0, int *off);
static int nrvo_kind_depth;

/* A VAR every definition of which is the same frame address (Zig keeps
 * `t1 = &t0` in one): that offset, or 0. */
static int nrvo_var_frame_addr_scan(TCCIRState *ir, int32_t var, int *off);
static int nrvo_var_frame_addr(TCCIRState *ir, int32_t var, int *off)
{
  const int x = TCCIR_DECODE_VREG_POSITION(var);
  if (nrvo_cache.ir != ir || x >= nrvo_cache.nvars)
    return nrvo_var_frame_addr_scan(ir, var, off);
  if (!nrvo_cache.vstate[x])
  {
    /* (a VAR met again while its own definitions are followed -- a cycle --
     * is not one frame address meanwhile; one cut off by the depth limit is
     * recorded as not one, which only refuses more) */
    nrvo_cache.vstate[x] = 2;
    int o = 0;
    if (nrvo_var_frame_addr_scan(ir, var, &o))
    {
      nrvo_cache.vstate[x] = 1;
      nrvo_cache.voff[x] = o;
    }
  }
  if (nrvo_cache.vstate[x] != 1)
    return 0;
  *off = nrvo_cache.voff[x];
  return 1;
}

static int nrvo_var_frame_addr_scan(TCCIRState *ir, int32_t var, int *off)
{
  int defs = 0, o0 = 0;
  if (nrvo_kind_depth > 6)
    return 0;
  nrvo_kind_depth++;
  for (int j = 0; j < ir->next_instruction_index; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || q->op == TCCIR_OP_STORE_INDEXED)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(d) != var || d.is_llocal)
      continue;
    int at = 0, add = 0;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (q->op == TCCIR_OP_ADD)
    {
      IROperand k = tcc_ir_op_get_src2(ir, q);
      if (irop_get_tag(k) != IROP_TAG_IMM32 || k.is_sym)
        goto fail;
      add = (int)irop_get_imm64_ex(ir, k);
    }
    else if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_STORE)
      goto fail;
    if (nrvo_frame_addr(s1))
      at = irop_get_stack_offset(s1);
    else if (irop_get_tag(s1) != IROP_TAG_VREG ||
             TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s1)) != TCCIR_VREG_TYPE_TEMP ||
             nrvo_arg_kind(ir, s1, j, -1, &at) != 1)
      goto fail;
    at += add;
    if (defs && at != o0)
      goto fail;
    o0 = at;
    defs++;
  }
  nrvo_kind_depth--;
  if (!defs)
    return 0;
  *off = o0;
  return 1;
fail:
  nrvo_kind_depth--;
  return 0;
}

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
  if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR && !o.is_llocal && nrvo_var_frame_addr(ir, v, off))
    return 1;
  if (o.is_lval || v < 0)
    return 0;
  if (v == p0)
    return 3;
  int d = nrvo_temp_def(ir, v, at);
  if (d < 0)
    return 0;
  IRQuadCompact *q = &ir->compact_instructions[d];
  if (q->op == TCCIR_OP_ADD)
  {
    /* a member's address: `T <- &obj ADD #k`, through TEMPs */
    IROperand k = tcc_ir_op_get_src2(ir, q), b = tcc_ir_op_get_src1(ir, q);
    int boff = 0;
    if (irop_get_tag(k) != IROP_TAG_IMM32 || k.is_sym || (!nrvo_frame_addr(b) && irop_get_tag(b) != IROP_TAG_VREG))
      return 0;
    if (nrvo_frame_addr(b))
      boff = irop_get_stack_offset(b);
    else if (b.is_lval || b.is_llocal || TCCIR_DECODE_VREG_TYPE(irop_get_vreg(b)) != TCCIR_VREG_TYPE_TEMP ||
             nrvo_arg_kind(ir, b, d, p0, &boff) != 1)
      return 0;
    *off = boff + (int)irop_get_imm64_ex(ir, k);
    return 1;
  }
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
  if (sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_VAR && !s.is_llocal && nrvo_var_frame_addr(ir, sv, off))
    return 1;
  /* P0's copy in a VAR, or the frontend's sret home slot */
  if (!s.is_lval && sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_VAR)
  {
    int defs = 0, ok = 1;
    for (int j = 0; j < ir->next_instruction_index && ok; j++)
    {
      IRQuadCompact *vq = &ir->compact_instructions[j];
      if (vq->op == TCCIR_OP_NOP || !irop_config[vq->op].has_dest || !irop_config[vq->op].has_src1)
        continue;
      if (tcc_ir_op_dest_vreg(ir, vq) != sv)
        continue;
      defs++;
      if (tcc_ir_op_src1_is_lval(ir, vq) || tcc_ir_op_src1_vreg(ir, vq) != p0)
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
  int base;     /* where its bytes go in *P0 */
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

/* Every instruction this pass makes gets its own orig_index: codegen maps
 * orig_index to code addresses, and a copy sharing instruction 0's would move
 * a `&&label` on it to wherever the copy lands (and share its side-table
 * annotations). */
static void nrvo_insert(TCCIRState *ir, int at, IRQuadCompact *q)
{
  q->orig_index = ++ir->max_orig_index;
  tcc_ir_insert_instruction_before(ir, at, q);
}

/* Insert `q` before `at` so that every jump -- and switch-table entry -- to
 * `at` lands on it (tcc_ir_insert_instruction_before moves them past it). */
static void nrvo_insert_landing(TCCIRState *ir, int at, IRQuadCompact *q)
{
  const int target = ir->compact_instructions[at].is_jump_target;
  nrvo_insert(ir, at, q);
  if (target)
  {
    ir->compact_instructions[at].is_jump_target = 1;
    ir->compact_instructions[at + 1].is_jump_target = 0;
  }
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *jq = &ir->compact_instructions[i];
    if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
      continue;
    if ((int)tcc_ir_op_dest_imm(ir, jq) == at + 1)
      tcc_ir_op_set_dest_imm32(ir, jq, at, IROP_BTYPE_INT32);
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *tb = &ir->switch_tables[t];
    if (tb->default_target == at + 1)
      tb->default_target = at;
    for (int j = 0; tb->targets && j < tb->num_entries; j++)
      if (tb->targets[j] == at + 1)
        tb->targets[j] = at;
  }
}

/* `t <- P0 ADD #k` (or `t <- P0` for k == 0), inserted before `at` so that
 * every jump to `at` lands on it. */
static int32_t nrvo_insert_addr(TCCIRState *ir, int at, int32_t p0, int k)
{
  int32_t t = tcc_ir_get_vreg_temp(ir);
  IRQuadCompact q = {0};
  q.op = k ? TCCIR_OP_ADD : TCCIR_OP_ASSIGN;
  q.operand_base = tcc_ir_pool_add(ir, irop_make_vreg(t, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(p0, IROP_BTYPE_INT32));
  if (k)
    tcc_ir_pool_add(ir, irop_make_imm32(-1, k, IROP_BTYPE_INT32));
  q.line_num = ir->compact_instructions[at].line_num;
  nrvo_insert_landing(ir, at, &q);
  return t;
}


/* Remove the copy or fill call at `i`, whose destination argument is `a0`.
 * memcpy/memmove/memset return that destination: a result someone reads
 * (`q = memset(&r, 0, n)`) becomes `q <- a0`. */
static void nrvo_drop_memop(TCCIRState *ir, int i, IROperand a0)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  ir_opt_nop_call_params(ir, i);
  if (q->op != TCCIR_OP_FUNCCALLVAL)
  {
    q->op = TCCIR_OP_NOP;
    return;
  }
  IROperand d = tcc_ir_op_get_dest(ir, q);
  a0.is_lval = 0;
  a0.is_llocal = 0;
  if (irop_get_tag(a0) == IROP_TAG_VREG)
    a0.btype = IROP_BTYPE_INT32;
  const int base = tcc_ir_pool_add(ir, d);
  tcc_ir_pool_add(ir, a0);
  q->op = TCCIR_OP_ASSIGN;
  q->operand_base = base;
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
        st.line_num = ir->compact_instructions[pfirst].line_num;
        nrvo_insert_landing(ir, pfirst, &st); /* where the fill was, on every path */
        ins++;
      }
      int call = i + ins;
      nrvo_drop_memop(ir, call, a[0]);
      shift += ins;
      site += ins;
    }
  next:;
  }
  return shift;
}

/* ------------------------------------------------------------------------- */
/* Where the addresses of frame objects go.                                  */
/* ------------------------------------------------------------------------- */

typedef struct
{
  int lo, hi; /* frame bytes [lo, hi) */
} NrvoRange;

typedef struct
{
  uint64_t esc;  /* an address into it flows where it cannot be followed */
  uint64_t sret; /* an address into it is a call's struct-return buffer */
  uint64_t *ref; /* optional, per instruction: the objects it accesses */
  uint64_t *ind; /* with ref: those it accesses other than as a plain frame slot */
  uint64_t *at;  /* optional, per instruction: the objects whose address escapes there */
  int bad;       /* no fixpoint */
  int params;       /* track incoming parameter values instead of frame ranges */
  int borrow_calls; /* use summaries only for callee-side object merges */
} NrvoUses;

/* Copy and fill helpers: they read or write through their pointer arguments
 * and keep none of them. */
static int nrvo_is_memop_callee(const char *nm)
{
  if (!nm)
    return 0;
  if (ir_opt_is_memcpy_or_memmove_name(nm))
    return 1;
  return ir_opt_name_in(nm, "memset\0__aeabi_memset\0__aeabi_memset4\0__aeabi_memset8\0__aeabi_memclr\0"
                            "__aeabi_memclr4\0__aeabi_memclr8\0");
}

static uint64_t nrvo_off_mask(const NrvoRange *r, int nr, int off)
{
  uint64_t m = 0;
  for (int k = 0; k < nr; k++)
    if (off >= r[k].lo && off < r[k].hi)
      m |= 1ull << k;
  return m;
}

/* Ops whose result is computed from their operands' values -- an address
 * among them makes the result an address into the same object. */
static int nrvo_derives(int op)
{
  switch (op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_MUL:
  case TCCIR_OP_ROR:
  case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_BFI:
  case TCCIR_OP_PACK64:
  case TCCIR_OP_ZEXT:
  case TCCIR_OP_SELECT:
  case TCCIR_OP_LOAD:
  case TCCIR_OP_STORE:
  case TCCIR_OP_LOAD_INDEXED:
  case TCCIR_OP_LOAD_POSTINC:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
    return 1;
  }
  return 0;
}

/* Ops that only look at their operands' values. */
static int nrvo_inspects(int op)
{
  return op == TCCIR_OP_CMP || op == TCCIR_OP_TEST_ZERO || op == TCCIR_OP_JUMPIF || op == TCCIR_OP_JUMP ||
         op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SETIF;
}

/* Track every address into the ranges r[] through the vregs holding it (a
 * least fixpoint over all definitions, so a VAR assigned in a loop is
 * covered), and classify every use of such an address: a load/store base or a
 * copy helper's argument is an access, a call's parameter 0 that is its
 * struct-return buffer is noted in u->sret, anything else -- a call argument,
 * a value stored to memory, a return value, an op not known to keep it in a
 * vreg -- is an escape. */
static void nrvo_uses(TCCIRState *ir, const NrvoRange *r, int nr, NrvoUses *u)
{
  const int n = ir->next_instruction_index;
  const int ntemps = ir->next_temporary_variable + 1, nvars = ir->next_local_variable + 1;
  const int ncalls = ir->next_call_id + 1;
  /* A private frame word is a local pointer variable too. Track its value
   * through CFG joins, killing the old value at each full-word store. A
   * flow-insensitive union would confuse an inline-name pointer with the heap
   * pointer that replaces it on another path. Any address-taken object or
   * overlapping non-word write falls back to the ordinary escape rule. */
  typedef struct { int off, lo, hi; } ParamWord;
  scoped_vector(ParamWord) words = {0};
  scoped_vector(uint64_t) mem_out = {0}, mem_state = {0};
  IRCFG *mem_cfg = NULL;
  if (u->params && !ir->frame_relaid) {
    for (int i = 0; i < n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int off = irop_get_stack_offset(d), lo, hi;
      if (irop_get_tag(d) != IROP_TAG_STACKOFF || irop_get_vreg(d) >= 0 || !d.is_local || !d.is_lval ||
          d.is_llocal || d.is_complex || d.btype != IROP_BTYPE_INT32 || !tcc_ir_frame_object_at(ir, off, &lo, &hi) || off + 4 > hi)
        continue;
      int seen = 0;
      for (size_t k = 0; k < words.size; k++)
        seen |= words.data[k].off == off;
      if (!seen)
        vector_push_back(&words, ((ParamWord){off, lo, hi}));
    }
    for (int i = 0; i < n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      for (int k = 0; k < 4; k++) {
        IROperand o;
        if (!nrvo_operand(ir, q, k, &o) || irop_get_tag(o) != IROP_TAG_STACKOFF || irop_get_vreg(o) >= 0)
          continue;
        int off = irop_get_stack_offset(o);
        int addr = !o.is_lval && !o.is_llocal;
        int size = ir_opt_store_btype_size_bytes(o.btype);
        if (o.is_complex)
          size *= 2;
        for (size_t w = 0; w < words.size; w++) {
          ParamWord *p = &words.data[w];
          if ((addr && off >= p->lo && off < p->hi) ||
              (k == 0 && o.is_lval && (size != 4 || q->op == TCCIR_OP_BLOCK_COPY) &&
               (size <= 0 || (off < p->off + 4 && off + size > p->off))))
            p->hi = p->lo; /* not private */
        }
      }
    }
    for (size_t k = 0; k < words.size;)
      if (words.data[k].hi == words.data[k].lo)
        vector_erase(&words, k);
      else
        k++;
    if (words.size) {
      mem_cfg = tcc_ir_cfg_build(ir);
      if (!mem_cfg || words.size * mem_cfg->num_blocks > 131072) {
        tcc_ir_cfg_free(mem_cfg);
        mem_cfg = NULL;
        vector_clear(&words);
      } else {
        vector_resize(&mem_out, words.size * mem_cfg->num_blocks);
        vector_resize(&mem_state, words.size);
      }
    }
  }
  uint64_t *taint = tcc_mallocz(sizeof(uint64_t) * (ntemps + nvars));
  int *call_at = tcc_malloc(sizeof(int) * ncalls);
  /* by call id: the addresses a copy or fill helper got as its destination,
   * which memcpy/memmove/memset hand back as their result */
  uint64_t *memret = tcc_malloc(sizeof(uint64_t) * ncalls);
  for (int c = 0; c < ncalls; c++)
    call_at[c] = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!nrvo_is_call(q->op))
      continue;
    int c = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
    if (c >= 0 && c < ncalls)
      call_at[c] = i;
  }
#define NRVO_VIDX(v)                                                                                                   \
  (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP                                                                   \
       ? (TCCIR_DECODE_VREG_POSITION(v) < ntemps ? TCCIR_DECODE_VREG_POSITION(v) : -1)                                 \
   : TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR                                                                  \
       ? (TCCIR_DECODE_VREG_POSITION(v) < nvars ? ntemps + TCCIR_DECODE_VREG_POSITION(v) : -1)                         \
       : -1)
  u->bad = 0;
  for (int round = 0;; round++)
  {
    int changed = 0;
    uint64_t esc = 0, sret = 0;
    if (u->ref)
    {
      memset(u->ref, 0, sizeof(uint64_t) * n);
      memset(u->ind, 0, sizeof(uint64_t) * n);
    }
    if (u->at)
      memset(u->at, 0, sizeof(uint64_t) * n);
    memset(memret, 0, sizeof(uint64_t) * ncalls);
    for (int i = 0; i < n; i++)
    {
      int mb = mem_cfg ? mem_cfg->instr_to_block[i] : -1;
      if (mb >= 0 && i == mem_cfg->blocks[mb].start_idx) {
        memset(mem_state.data, 0, words.size * sizeof(uint64_t));
        IRBasicBlock *bb = &mem_cfg->blocks[mb];
        for (int p = 0; p < bb->num_preds; p++)
          for (size_t w = 0; w < words.size; w++)
            mem_state.data[w] |= mem_out.data[bb->preds[p] * words.size + w];
      }
      IRQuadCompact *q = &ir->compact_instructions[i];
      const int op = q->op;
      uint64_t val = 0, ref = 0, ind = 0;
      int def = -1, store_mem = 0, mem_def = -1;
      uint64_t eh = 0; /* escapes here */
      for (int k = 0; k < 4; k++)
      {
        IROperand o;
        if (!nrvo_operand(ir, q, k, &o))
          continue;
        const int tag = irop_get_tag(o);
        const int32_t v = irop_get_vreg(o);
        if (tag == IROP_TAG_STACKOFF)
        {
          /* a vreg-backed slot names its vreg (its offset is only the front
           * end's watermark), handled as one below */
          if (v < 0)
          {
            if (mem_cfg && (o.is_lval || o.is_llocal)) {
              int off = irop_get_stack_offset(o);
              int size = ir_opt_store_btype_size_bytes(o.btype);
              if (o.is_complex)
                size *= 2;
              int lo, hi;
              if (o.btype == IROP_BTYPE_STRUCT && tcc_ir_frame_object_at(ir, off, &lo, &hi))
                size = hi - off;
              for (size_t w = 0; w < words.size; w++) {
                if (off >= words.data[w].off + 4 || size <= 0 || off + size <= words.data[w].off)
                  continue;
                if (k == 0 && !o.is_llocal && off == words.data[w].off && size == 4 &&
                    (op == TCCIR_OP_STORE || op == TCCIR_OP_ASSIGN))
                  mem_def = (int)w;
                else if (k != 0)
                  val |= mem_state.data[w];
              }
            }
            const uint64_t m = nrvo_off_mask(r, nr, irop_get_stack_offset(o));
            if (o.is_lval || o.is_llocal)
            {
              ref |= m; /* the slot itself */
              if (k == 0)
                store_mem = 1;
            }
            else
            {
              val |= m; /* its address */
              ref |= m;
              ind |= m;
            }
            continue;
          }
        }
        if (v < 0)
        {
          if (k == 0 && tag == IROP_TAG_SYMREF && o.is_lval)
            store_mem = 1;
          continue;
        }
        const int x = NRVO_VIDX(v);
        uint64_t t = x >= 0 ? taint[x] : 0;
        if (u->params && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_PARAM &&
            TCCIR_DECODE_VREG_POSITION(v) < 32)
          t |= 1ull << TCCIR_DECODE_VREG_POSITION(v);
        if (tag == IROP_TAG_STACKOFF && !o.is_lval && !o.is_llocal)
        {
          eh |= t; /* the address of the vreg's home: its value is in memory */
          continue;
        }
        int deref = TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR ? o.is_llocal : o.is_lval;
        if (u->params)
          deref = !irop_is_vreg_value(o);
        if (k == 0 && (op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_STORE_POSTINC))
          deref = 1;
        if (k == 1 && (op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_LOAD_POSTINC))
          deref = 1;
        if (k == 2 && (op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED))
          deref = 1;
        if (deref)
        {
          ref |= t;
          ind |= t;
          if (k == 0)
            store_mem = 1;
          continue;
        }
        if (k == 0)
        {
          def = x;
          continue;
        }
        val |= t;
      }
      if (op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID)
      {
        const uint32_t enc = (uint32_t)tcc_ir_op_src2_imm(ir, q);
        const int c = TCCIR_DECODE_CALL_ID(enc), pidx = TCCIR_DECODE_PARAM_IDX(enc);
        const int at = c >= 0 && c < ncalls ? call_at[c] : -1;
        if (val)
        {
          if (at >= 0 && nrvo_is_memop_callee(nrvo_callee(ir, &ir->compact_instructions[at])))
          {
            if (pidx == 0)
              memret[c] |= val;
          }
          else if (at >= 0 && pidx == 0 && ir->sret_calls && c < ir->sret_calls_size && ir->sret_calls[c] > 0)
            sret |= val;
          else
          {
            Sym *callee = at >= 0 ? tcc_ir_op_src1_sym(ir, &ir->compact_instructions[at]) : NULL;
            uint32_t nocapture = callee ? SYM_FACTS(callee)->param_nocapture : 0;
            if (!(u->params || u->borrow_calls) || !nocapture || pidx >= 32 ||
                (ir->sret_calls && c < ir->sret_calls_size && ir->sret_calls[c] > 0) ||
                !(nocapture & (1u << pidx)))
              eh |= val;
          }
        }
        /* the call is where the access happens */
        if (u->ref && at > i)
        {
          u->ref[at] |= ref | val;
          u->ind[at] |= ref | val;
        }
      }
      else if (nrvo_is_call(op))
      {
        eh |= val; /* a call through such an address */
        /* `p = memset(&x, ...)`: p is an address of x too */
        const int c = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
        const uint64_t mr = c >= 0 && c < ncalls ? memret[c] : 0;
        if (mr && op == TCCIR_OP_FUNCCALLVAL)
        {
          if (def >= 0 && !store_mem)
          {
            if ((taint[def] | mr) != taint[def])
            {
              taint[def] |= mr;
              changed = 1;
            }
          }
          else if (store_mem || tcc_ir_op_dest_vreg(ir, q) >= 0)
            eh |= mr;
        }
      }
      else if (nrvo_inspects(op))
        ;
      else if (nrvo_derives(op))
      {
        if (mem_def >= 0)
          mem_state.data[mem_def] = val;
        else if (store_mem)
          eh |= val;
        else if (def >= 0)
        {
          if ((taint[def] | val) != taint[def])
          {
            taint[def] |= val;
            changed = 1;
          }
        }
        else
          eh |= val;
      }
      else
        eh |= val;
      if (u->ref)
      {
        u->ref[i] |= ref;
        u->ind[i] |= ind;
      }
      if (mb >= 0 && i == mem_cfg->blocks[mb].end_idx - 1)
        for (size_t w = 0; w < words.size; w++) {
          uint64_t *out = &mem_out.data[mb * words.size + w];
          if (*out != mem_state.data[w]) {
            *out = mem_state.data[w];
            changed = 1;
          }
        }
      esc |= eh;
      if (u->at)
        u->at[i] |= eh;
    }
    if (!changed)
    {
      u->esc = esc;
      u->sret = sret;
      break;
    }
    if (round >= 32)
    {
      u->bad = 1;
      u->esc = ~0ull;
      break;
    }
  }
#undef NRVO_VIDX
  tcc_ir_cfg_free(mem_cfg);
  tcc_free(taint);
  tcc_free(call_at);
  tcc_free(memret);
}

/* The frame is reachable other than through IR operands -- or, unless
 * `flow_ok`, control flow is not all in the IR (a computed goto).  The caller
 * side only needs the first: a label address hands no frame byte to a callee,
 * and its reachability walk treats an IJUMP as reaching everything; giving up
 * there gave a 256 KB struct result a second buffer in any function with a
 * computed goto (tests2/119 then overflowed its 1 MB stack on the board). */
static int nrvo_frame_hidden_ex(TCCIRState *ir, int flow_ok)
{
  if (ir->inline_asm_count || (!flow_ok && ir->func_has_label_addr) || ir->has_static_chain ||
      tcc_state->nb_nested_funcs || ir->captured_count || ir->emits_set_chain || tcc_bounds_checking(tcc_state))
    return 1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    const int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_ASM_OUTPUT ||
        op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT || op == TCCIR_OP_BUILTIN_APPLY_ARGS ||
        op == TCCIR_OP_BUILTIN_APPLY || (!flow_ok && op == TCCIR_OP_IJUMP))
      return 1;
  }
  return 0;
}

static int nrvo_frame_hidden(TCCIRState *ir)
{
  return nrvo_frame_hidden_ex(ir, 0);
}

/* Record physical parameter indices after SSA lowering has exposed all uses.
 * Only a completed, local, non-interposable body may supply this proof. Unknown
 * calls, pointer returns, escaped local storage and analysis limits all keep
 * the corresponding bit clear. Caller-side sret alias checks remain stricter:
 * a borrowed argument can still alias the same call's return buffer. */
void tcc_ir_analyze_param_nocapture(TCCIRState *ir, Sym *sym)
{
  if (!ir || !sym || !ir->next_parameter || !(sym->type.t & VT_STATIC) || sym->a.weak || nrvo_frame_hidden(ir) ||
      tcc_ir_opt_pass_disabled("param_nocapture"))
    return;
  tcc_ir_dump_after_pass(ir, "param_nocapture");
  NrvoUses u = {.params = 1};
  scoped_vector(uint64_t) escapes = {0};
  if (nrvo_nocapture_dbg())
  {
    vector_resize(&escapes, ir->next_instruction_index);
    u.at = vector_data(&escapes);
  }
  nrvo_uses(ir, NULL, 0, &u);
  uint32_t valid = ir->next_parameter >= 32 ? ~0u : (1u << ir->next_parameter) - 1;
  uint32_t mask = u.bad ? 0 : (uint32_t)~u.esc & valid;
  if (mask || sym_facts_peek(sym))
    sym_facts(sym)->param_nocapture = mask;
  if (u.at)
    for (int i = 0; i < ir->next_instruction_index; i++)
      if (u.at[i])
        fprintf(stderr, "[NOCAPTURE] %s at=%d escapes=%llx\n", funcname, i, (unsigned long long)u.at[i]);
}

int tcc_ir_frame_range_escapes(TCCIRState *ir, int lo, int hi)
{
  if (nrvo_frame_hidden(ir))
    return 1;
  NrvoRange r = {lo, hi};
  NrvoUses u = {0};
  nrvo_uses(ir, &r, 1, &u);
  return u.bad || u.esc;
}

static int nrvo_succ(TCCIRState *ir, int j, int *out, int cap);

/* Every instruction reachable from the marked ones (themselves included). */
static uint8_t *nrvo_reach_from(TCCIRState *ir, const uint8_t *start)
{
  const int n = ir->next_instruction_index;
  uint8_t *seen = tcc_mallocz(n + 1);
  int *stack = tcc_malloc(sizeof(int) * (n + 1));
  int cap = 64, sp = 0;
  for (int t = 0; t < ir->num_switch_tables; t++)
    if (ir->switch_tables[t].num_entries + 2 > cap)
      cap = ir->switch_tables[t].num_entries + 2;
  int *succ = tcc_malloc(sizeof(int) * cap);
  for (int j = 0; j < n; j++)
    if (start[j])
    {
      seen[j] = 1;
      stack[sp++] = j;
    }
  while (sp)
  {
    const int j = stack[--sp];
    const int ns = nrvo_succ(ir, j, succ, cap);
    if (ns < 0)
    {
      memset(seen, 1, n); /* unknown control flow: everything */
      break;
    }
    for (int k = 0; k < ns; k++)
      if (succ[k] >= 0 && succ[k] < n && !seen[succ[k]])
      {
        seen[succ[k]] = 1;
        stack[sp++] = succ[k];
      }
  }
  tcc_free(stack);
  tcc_free(succ);
  return seen;
}

/* ------------------------------------------------------------------------- */
/* Caller side: no struct-return buffer the callee can reach otherwise.      */
/* ------------------------------------------------------------------------- */

/* The frame offset a call's buffer operand points at, through a TEMP's single
 * ASSIGN/LEA/ADD #imm definition chain, or 0 when it is not a frame address. */
static int nrvo_buffer_frame_off(TCCIRState *ir, IROperand o, int at, int *off)
{
  int add = 0;
  for (int depth = 0; depth < 8; depth++)
  {
    if (nrvo_frame_addr(o))
    {
      *off = irop_get_stack_offset(o) + add;
      return 1;
    }
    int32_t v = irop_get_vreg(o);
    if (irop_get_tag(o) != IROP_TAG_VREG || o.is_lval || o.is_llocal || v < 0)
      return 0;
    int d = nrvo_temp_def(ir, v, at);
    if (d < 0)
      return 0;
    IRQuadCompact *q = &ir->compact_instructions[d];
    if (q->op == TCCIR_OP_ADD)
    {
      if (tcc_ir_op_src2_tag(ir, q) != IROP_TAG_IMM32 || tcc_ir_op_src2_is_sym(ir, q))
        return 0;
      add += (int)tcc_ir_op_src2_imm(ir, q);
    }
    else if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA)
      return 0;
    o = tcc_ir_op_get_src1(ir, q);
    at = d;
  }
  return 0;
}

/* Call `call` writes its struct result (`size` bytes) through the parameter-0
 * operand of instruction `p0i`: make that a fresh temporary instead, copied
 * to the original destination right after the call returns.  Returns the
 * number of instructions inserted at or before `call`. */
static int nrvo_reroute(TCCIRState *ir, int call, int p0i, int size)
{
  IROperand op = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p0i]);
  const int line = ir->compact_instructions[call].line_num;
  loc = tcc_ir_frame_alloc_ret_temp(loc, size, size >= 8 ? -8 : -4);
  const int tmp = loc;
  int shift = 0;
  IROperand dst = op;
  const int32_t v = irop_get_vreg(op);
  /* a frame address -- a parameter's home `&P1` included -- or a TEMP's one
   * value is the same after the call */
  const int stable =
      (irop_get_tag(op) == IROP_TAG_STACKOFF && !op.is_lval && !op.is_llocal) ||
      (irop_get_tag(op) == IROP_TAG_VREG && !op.is_lval && !op.is_llocal && v >= 0 &&
       TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP && nrvo_temp_def(ir, v, p0i) >= 0);
  if (!stable)
  {
    /* the destination address as it is when the call is made */
    const int32_t t = tcc_ir_get_vreg_temp(ir);
    int deref = irop_get_tag(op) == IROP_TAG_STACKOFF ? (op.is_lval || op.is_llocal)
                : v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR ? op.is_llocal
                                                                              : op.is_lval;
    IRQuadCompact a = {0};
    a.op = deref ? TCCIR_OP_LOAD : TCCIR_OP_ASSIGN;
    a.operand_base = tcc_ir_pool_add(ir, irop_make_vreg(t, IROP_BTYPE_INT32));
    tcc_ir_pool_add(ir, op);
    a.line_num = line;
    nrvo_insert_landing(ir, p0i, &a);
    p0i++;
    call++;
    shift++;
    dst = irop_make_vreg(t, IROP_BTYPE_INT32);
  }
  tcc_ir_op_set_src1(ir, &ir->compact_instructions[p0i], irop_make_stackoff(-1, tmp, 0, 0, 0, IROP_BTYPE_INT32));

  /* A TEMP holding a frame address -- `T <- &P1`, a struct parameter's home,
   * which the front end hands over as `s = f(s)`'s destination -- is computed
   * again after the call (an LEA, like every other `&P1`) instead of being
   * kept in a register across it: the address does not change, and an
   * ASSIGN of it held across a call was lowered as a load of the slot. */
  int recompute = 0;
  IROperand addr = dst;
  if (stable && irop_get_tag(dst) == IROP_TAG_VREG)
  {
    const int d = nrvo_temp_def(ir, irop_get_vreg(dst), p0i);
    if (d >= 0 && (ir->compact_instructions[d].op == TCCIR_OP_ASSIGN || ir->compact_instructions[d].op == TCCIR_OP_LEA))
    {
      IROperand src = tcc_ir_op_get_src1(ir, &ir->compact_instructions[d]);
      if (irop_get_tag(src) == IROP_TAG_STACKOFF && !src.is_lval && !src.is_llocal)
      {
        recompute = 1;
        addr = src;
      }
    }
  }

  /* memmove(dst, &tmp, size) on the call's fall-through only: inserted after
   * it, jumps to the instruction after the call keep skipping it */
  const int cid = ir->next_call_id++;
  SValue fsv;
  svalue_init(&fsv);
  fsv.type = func_old_type;
  fsv.r = VT_CONST | VT_SYM;
  fsv.sym = external_helper_sym(TOK_memmove);
  fsv.vr = -1;
  IRQuadCompact seq[4];
  memset(seq, 0, sizeof seq);
  const IROperand args[3] = {dst, irop_make_stackoff(-1, tmp, 0, 0, 0, IROP_BTYPE_INT32),
                             irop_make_imm32(-1, size, IROP_BTYPE_INT32)};
  for (int k = 0; k < 3; k++)
  {
    seq[k].op = TCCIR_OP_FUNCPARAMVAL;
    seq[k].operand_base = tcc_ir_pool_add(ir, args[k]);
    tcc_ir_pool_add(ir, irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_PARAM(cid, k), IROP_BTYPE_INT32));
    seq[k].line_num = line;
  }
  seq[3].op = TCCIR_OP_FUNCCALLVOID;
  seq[3].operand_base = tcc_ir_pool_add(ir, svalue_to_iroperand(ir, &fsv));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_CALL(cid, 3), IROP_BTYPE_INT32));
  seq[3].line_num = line;
  for (int k = 3; k >= 0; k--)
    nrvo_insert(ir, call + 1, &seq[k]);
  if (recompute)
  {
    IRQuadCompact l = {0};
    l.op = TCCIR_OP_LEA;
    l.operand_base = tcc_ir_pool_add(ir, dst);
    tcc_ir_pool_add(ir, addr);
    l.line_num = line;
    const int32_t t = tcc_ir_get_vreg_temp(ir);
    IROperand nt = irop_make_vreg(t, IROP_BTYPE_INT32);
    ir->iroperand_pool[l.operand_base] = nt;
    nrvo_insert(ir, call + 1, &l);
    tcc_ir_op_set_src1(ir, &ir->compact_instructions[call + 2], nt); /* the copy's PARAM0 */
  }
  return shift;
}

/* Every struct-returning call gets a buffer it can reach only through the
 * pointer it is handed: the caller side of the convention tcc_ir_opt_sret_nrvo
 * builds on.  The front end passes a named destination directly
 * (`x = f(...)`, `s.m = f(...)`, `*p = f(...)`, expr_eq); that stays only for
 * a frame object none of whose addresses escapes -- it is not passed to the
 * same call, stored, or handed to anything but a copy helper or as another
 * call's result buffer -- in a function that calls nothing returning twice.
 * Anything else, a pointer included, gets a fresh temporary.  Runs at every
 * optimization level: an optimized callee may be called from unoptimized
 * code. */
int tcc_ir_sret_dealias(TCCIRState *ir)
{
  if (!ir || !ir->sret_calls || !ir->sret_calls_size)
    return 0;
  const int n = ir->next_instruction_index;
  nrvo_cache_begin(ir); /* until the reroutes below */
  typedef struct
  {
    int cid, call, size, range;
  } NrvoSret;
  NrvoSret *sc = NULL;
  int nsc = 0, cap = 0;
  NrvoRange *rg = NULL;
  int nrg = 0, rcap = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!nrvo_is_call(q->op))
      continue;
    const int c = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
    if (c < 0 || c >= ir->sret_calls_size || ir->sret_calls[c] <= 0)
      continue;
    /* an empty struct: nothing is written through the buffer, and a copy of
     * the 1 byte sret_calls records would write past the destination */
    if (ir->sret_calls_empty && ir->sret_calls_empty[c])
      continue;
    const int p0i = ir_opt_get_call_param_index(ir, i, 0);
    if (p0i < 0 || ir->compact_instructions[p0i].op != TCCIR_OP_FUNCPARAMVAL)
      continue;
    IROperand o = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p0i]);
    int off = 0, lo = 0, hi = 0, range = -1;
    /* kept only at a word-aligned frame address: the callee may assume its
     * buffer has the return type's alignment (LDM/LDRD fault on less), and a
     * member of a packed struct need not have it */
    if (nrvo_buffer_frame_off(ir, o, p0i, &off) && !(off & 3) && tcc_ir_frame_object_at(ir, off, &lo, &hi))
    {
      for (int k = 0; k < nrg && range < 0; k++)
        if (rg[k].lo == lo && rg[k].hi == hi)
          range = k;
      if (range < 0)
      {
        if (nrg == rcap)
          rg = tcc_realloc(rg, sizeof(*rg) * (rcap = rcap ? 2 * rcap : 16));
        rg[nrg] = (NrvoRange){lo, hi};
        range = nrg++;
      }
    }
    if (nsc == cap)
      sc = tcc_realloc(sc, sizeof(*sc) * (cap = cap ? 2 * cap : 16));
    sc[nsc++] = (NrvoSret){c, i, ir->sret_calls[c], range};
  }
  if (!nsc)
  {
    nrvo_cache_end();
    tcc_free(rg);
    return 0;
  }

  /* Where the destinations' addresses escape, 64 objects at a time: a call
   * that one of those points reaches -- the call itself, when the address
   * is one of its own arguments -- could see its buffer through the escaped
   * pointer. */
  uint8_t **reach = tcc_mallocz(sizeof(uint8_t *) * (nrg + 1));
  uint8_t *bad = tcc_mallocz(nrg + 1);
  const int all = nrvo_frame_hidden_ex(ir, 1) || tcc_ir_calls_returns_twice(ir);
  for (int b = 0; b < nrg && !all; b += 64)
  {
    const int m = nrg - b < 64 ? nrg - b : 64;
    NrvoUses u = {0};
    u.at = tcc_malloc(sizeof(uint64_t) * (n + 1));
    nrvo_uses(ir, rg + b, m, &u);
    uint8_t *start = tcc_malloc(n + 1);
    for (int k = 0; k < m; k++)
    {
      if (u.bad)
      {
        bad[b + k] = 1;
        continue;
      }
      if (!((u.esc >> k) & 1))
        continue;
      for (int j = 0; j < n; j++)
        start[j] = (u.at[j] >> k) & 1;
      reach[b + k] = nrvo_reach_from(ir, start);
    }
    tcc_free(start);
    tcc_free(u.at);
  }

  /* Decide on the original indices first. */
  for (int s = 0; s < nsc; s++)
  {
    const int r = sc[s].range;
    if (all || r < 0 || bad[r] || (reach[r] && reach[r][sc[s].call]))
      sc[s].range = -2; /* reroute */
  }
  for (int k = 0; k < nrg; k++)
    tcc_free(reach[k]);
  tcc_free(reach);
  tcc_free(bad);

  nrvo_cache_end();

  /* Reroute.  Each one inserts instructions, so the call is looked up again
   * by its id. */
  int changes = 0;
  for (int s = 0; s < nsc; s++)
  {
    if (sc[s].range != -2)
      continue;
    int call = -1;
    for (int i = 0; i < ir->next_instruction_index && call < 0; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (nrvo_is_call(q->op) &&
          TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q)) == sc[s].cid)
        call = i;
    }
    const int p0i = call < 0 ? -1 : ir_opt_get_call_param_index(ir, call, 0);
    if (p0i < 0)
      continue;
    if (nrvo_dbg())
      fprintf(stderr, "[NRVO] %s: call at %d gets a fresh result buffer\n", funcname, call);
    nrvo_reroute(ir, call, p0i, sc[s].size);
    changes++;
  }
  tcc_free(sc);
  tcc_free(rg);
  return changes;
}

/* ------------------------------------------------------------------------- */
/* Callee side.                                                              */
/* ------------------------------------------------------------------------- */

/* Carrier bookkeeping for nrvo_p0_only_copies: car[] has one byte per TEMP
 * (0..ntemps-1) then per VAR (ntemps..). */
typedef struct NrvoCarriers
{
  const uint8_t *car;
  int ntemps, nvars;
  int32_t p0;
} NrvoCarriers;

/* the carrier index of vreg v, else -1 */
static int nrvo_cidx(const NrvoCarriers *c, int32_t v)
{
  if (v < 0)
    return -1;
  if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP)
    return TCCIR_DECODE_VREG_POSITION(v) < c->ntemps ? TCCIR_DECODE_VREG_POSITION(v) : -1;
  if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
    return TCCIR_DECODE_VREG_POSITION(v) < c->nvars ? c->ntemps + TCCIR_DECODE_VREG_POSITION(v) : -1;
  return -1;
}

/* the carrier index of a VREG operand or a vreg-backed slot, else -1 */
static int nrvo_cx(const NrvoCarriers *c, IROperand o)
{
  if (irop_get_tag(o) == IROP_TAG_VREG || (irop_get_tag(o) == IROP_TAG_STACKOFF && irop_get_vreg(o) >= 0))
    return nrvo_cidx(c, irop_get_vreg(o));
  return -1;
}

/* P0, the home slot, or a carrier, read as a value (not dereferenced) */
static int nrvo_p0_value(const NrvoCarriers *c, IROperand o)
{
  if (irop_get_tag(o) == IROP_TAG_STACKOFF && irop_get_vreg(o) < 0 && irop_get_stack_offset(o) == (int)func_vc &&
      o.is_lval && !o.is_llocal)
    return 1;
  if (irop_get_tag(o) == IROP_TAG_VREG && irop_get_vreg(o) == c->p0 && !o.is_lval)
    return 1;
  const int x = nrvo_cx(c, o);
  return x >= 0 && c->car[x] &&
         (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(o)) == TCCIR_VREG_TYPE_VAR
              ? !o.is_llocal && (irop_get_tag(o) == IROP_TAG_VREG || o.is_lval)
              : !o.is_lval);
}

/* P0 -- and its home slot -- is read for nothing but the copies out: the
 * object family then owns all of *P0. */
static int nrvo_p0_only_copies(TCCIRState *ir, const int *sites, int nsites, int32_t p0)
{
  const int n = ir->next_instruction_index;
  const int ntemps = ir->next_temporary_variable + 1, nvars = ir->next_local_variable + 1;
  uint8_t *car = tcc_mallocz(ntemps + nvars);
  int *site_param = tcc_malloc(sizeof(int) * (nsites + 1));
  for (int s = 0; s < nsites; s++)
    site_param[s] = ir_opt_get_call_param_index(ir, sites[s], 0);
  const NrvoCarriers cc = {car, ntemps, nvars, p0};
  for (int changed = 1, round = 0; changed && round < 16; round++)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_STORE)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q), s1 = tcc_ir_op_get_src1(ir, q);
      const int32_t dv = irop_get_vreg(d);
      const int x = nrvo_cx(&cc, d);
      const int dval = x >= 0 && (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR ? !d.is_llocal : !d.is_lval);
      if (dval && !car[x] && nrvo_p0_value(&cc, s1))
        car[x] = changed = 1;
    }
  }
  int ok = 1;
  for (int i = 0; i < n && ok; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int is_site_param = 0;
    for (int s = 0; s < nsites; s++)
      is_site_param |= site_param[s] == i;
    for (int k = 0; k < 4 && ok; k++)
    {
      IROperand o;
      if (!nrvo_operand(ir, q, k, &o))
        continue;
      const int32_t v = irop_get_vreg(o);
      const int x = nrvo_cx(&cc, o);
      const int home = irop_get_tag(o) == IROP_TAG_STACKOFF && v < 0 && irop_get_stack_offset(o) == (int)func_vc;
      const int touches = home || (irop_get_tag(o) == IROP_TAG_VREG && v == p0) || (x >= 0 && car[x]);
      if (!touches)
        continue;
      if (k == 0)
      {
        /* the home store, or the definition of a carrier */
        if (home && q->op == TCCIR_OP_STORE && o.is_lval && !o.is_llocal)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          ok = irop_get_tag(s1) == IROP_TAG_VREG && irop_get_vreg(s1) == p0 && !s1.is_lval;
        }
        else
          ok = x >= 0 && car[x] && (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_STORE) &&
               nrvo_p0_value(&cc, tcc_ir_op_get_src1(ir, q)) &&
               (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR ? !o.is_llocal : !o.is_lval);
        continue;
      }
      if (k == 1 && nrvo_p0_value(&cc, o))
      {
        if (is_site_param)
          continue;
        if (q->op == TCCIR_OP_STORE)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          const int32_t dv = irop_get_vreg(d);
          const int dx = nrvo_cx(&cc, d);
          const int dhome = irop_get_tag(d) == IROP_TAG_STACKOFF && dv < 0 && irop_get_stack_offset(d) == (int)func_vc;
          if ((dx >= 0 && car[dx]) || (dhome && v == p0))
            continue;
        }
        else if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          const int dx = nrvo_cx(&cc, d);
          if (dx >= 0 && car[dx])
            continue;
        }
      }
      ok = 0;
      if (nrvo_dbg())
        fprintf(stderr, "[NRVO]   P0 used at %d op %s operand %d\n", i, tcc_ir_get_op_name(q->op), k);
    }
  }
  tcc_free(car);
  tcc_free(site_param);
  return ok;
}

/* The control-flow successors of instruction j (at most 2, or a switch). */
static int nrvo_succ(TCCIRState *ir, int j, int *out, int cap)
{
  const int n = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[j];
  int k = 0;
  switch (q->op)
  {
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_RETURNVALUE:
    return 0;
  case TCCIR_OP_IJUMP:
    return -1; /* a computed goto: any label */
  case TCCIR_OP_JUMP:
    out[k++] = (int)tcc_ir_op_dest_imm(ir, q);
    return k;
  case TCCIR_OP_JUMPIF:
    out[k++] = (int)tcc_ir_op_dest_imm(ir, q);
    break;
  case TCCIR_OP_SWITCH_TABLE:
  {
    const int t = (int)tcc_ir_op_src2_imm(ir, q);
    if (t < 0 || t >= ir->num_switch_tables)
      return -1;
    TCCIRSwitchTable *tb = &ir->switch_tables[t];
    if (k < cap)
      out[k++] = tb->default_target;
    for (int e = 0; tb->targets && e < tb->num_entries; e++)
    {
      if (k >= cap)
        return -1;
      out[k++] = tb->targets[e];
    }
    break; /* and the fall-through, to be safe */
  }
  default:
    break;
  }
  if (j + 1 < n && k < cap)
    out[k++] = j + 1;
  return k;
}

/* The bytes an access of btype `o` covers, or 0 when unknown. */
static int nrvo_access_width(IROperand o)
{
  if (irop_get_btype(o) == IROP_BTYPE_STRUCT || o.is_complex)
    return 0;
  return irop_is_64bit(o) ? 8 : ir_opt_store_btype_size_bytes(irop_get_btype(o));
}

#define NRVO_RD 1 /* reads some of the bytes */
#define NRVO_WR 2 /* writes some of them */
#define NRVO_KILL 4 /* writes all of them, reading none first */

/* What frame bytes [off, off + w) of the objects in `ov` (w <= 0: to the end
 * of the object) are to *P0 bytes [B, E): 0 apart, 1 overlapping, 2 covering. */
static int nrvo_frame_cover(const NrvoObj *obj, int nobj, uint64_t ov, int off, int w, int B, int E)
{
  const int ob = nrvo_obj_of(obj, nobj, off);
  if (ob < 0 || !((ov >> ob) & 1))
    return 0;
  if (w <= 0)
    w = obj[ob].lo + obj[ob].size - off;
  const int p = obj[ob].base + off - obj[ob].lo;
  if (p >= E || p + w <= B)
    return 0;
  return p <= B && p + w >= E ? 2 : 1;
}

/* How instruction j accesses *P0 bytes [B, E) through the objects in `ov`:
 * NRVO_RD | NRVO_WR | NRVO_KILL.  Computing an address is no access; a frame
 * slot, a load or store through a TEMP holding `&obj + k`, a copy helper's or
 * a call's result buffer at such an address are placed byte by byte; any
 * other access through an address of the objects reads and writes them all. */
static int nrvo_access(TCCIRState *ir, const NrvoObj *obj, int nobj, const NrvoUses *u, uint64_t ov, int j, int B,
                       int E, int32_t p0)
{
  if (!(u->ref[j] & ov))
    return 0;
  const int unknown = (u->ind[j] & ov) ? NRVO_RD | NRVO_WR : 0;
  IRQuadCompact *q = &ir->compact_instructions[j];
  const int op = q->op;
  if (nrvo_is_call(op))
  {
    const char *nm = nrvo_callee(ir, q);
    IROperand a[3];
    if (nrvo_is_memop_callee(nm))
    {
      const int fill = !ir_opt_is_memcpy_or_memmove_name(nm);
      const int aeabi_fill = fill && strcmp(nm, "memset");
      if (!nrvo_call_args(ir, j, a))
        return NRVO_RD | NRVO_WR;
      /* the size: memcpy/memmove/memset (d, s|c, n); __aeabi_memset (d, n, c) */
      IROperand sz = aeabi_fill ? a[1] : a[2];
      if (irop_get_tag(sz) != IROP_TAG_IMM32)
        return unknown;
      const int cnt = (int)irop_get_imm64_ex(ir, sz);
      int acc = 0, rd = 0;
      for (int k = (fill ? 0 : 1); k >= 0; k--) /* the source first */
      {
        int off = 0;
        const int kind = nrvo_arg_kind(ir, a[k], j, p0, &off);
        if (kind == 1)
        {
          const int c = nrvo_frame_cover(obj, nobj, ov, off, cnt, B, E);
          if (!c)
            continue;
          if (k == 1)
            acc |= NRVO_RD, rd = 1;
          else
            acc |= NRVO_WR | (c == 2 && !rd ? NRVO_KILL : 0);
        }
        else if (kind != 2 && kind != 3)
          return unknown;
      }
      return acc;
    }
    const int c = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
    IROperand r0;
    int off = 0;
    if (ir->sret_calls && c >= 0 && c < ir->sret_calls_size && ir->sret_calls[c] > 0 &&
        ir_opt_get_call_param_operand(ir, j, 0, &r0) && nrvo_arg_kind(ir, r0, j, p0, &off) == 1)
    {
      /* the result buffer is the only address of the objects a call may get
       * (any other is an escape, and then nothing is merged), and a callee
       * only writes it -- what was there before is not its business */
      const int cv = nrvo_frame_cover(obj, nobj, ov, off, ir->sret_calls[c], B, E);
      return cv == 2 ? NRVO_WR | NRVO_KILL : cv ? NRVO_WR : 0;
    }
    return unknown;
  }
  if (op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID)
  {
    /* an argument read from a frame slot (a struct passed by value) is read
     * here; an address passed is accessed at the call */
    IROperand o = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(o) == IROP_TAG_STACKOFF && irop_get_vreg(o) < 0 && (o.is_lval || o.is_llocal))
      return nrvo_frame_cover(obj, nobj, ov, irop_get_stack_offset(o), o.is_llocal ? 4 : nrvo_access_width(o), B, E)
                 ? NRVO_RD
                 : 0;
    if (irop_get_tag(o) == IROP_TAG_VREG && irop_get_vreg(o) >= 0 &&
        (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(o)) == TCCIR_VREG_TYPE_VAR ? o.is_llocal : o.is_lval))
      return unknown ? NRVO_RD : 0;
    return 0;
  }
  const int indexed = op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED;
  const int postinc = op == TCCIR_OP_LOAD_POSTINC || op == TCCIR_OP_STORE_POSTINC;
  int acc = 0;
  for (int k = 0; k < 4; k++)
  {
    IROperand o;
    if (!nrvo_operand(ir, q, k, &o))
      continue;
    const int tag = irop_get_tag(o);
    const int32_t v = irop_get_vreg(o);
    /* a store's destination is written, everything else read */
    const int wr = k == 0 && (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_STORE_POSTINC);
    if (tag == IROP_TAG_STACKOFF && v < 0)
    {
      if (!(o.is_lval || o.is_llocal))
        continue;
      const int c =
          nrvo_frame_cover(obj, nobj, ov, irop_get_stack_offset(o), o.is_llocal ? 4 : nrvo_access_width(o), B, E);
      if (c)
        acc |= o.is_llocal || !wr ? NRVO_RD : NRVO_WR;
      continue;
    }
    if (tag != IROP_TAG_VREG || v < 0)
      continue;
    const int base_role = ((op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_STORE_POSTINC) && k == 0) ||
                          ((op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_LOAD_POSTINC) && k == 1);
    const int index_role = indexed && k == 2;
    const int deref =
        base_role || index_role ||
        (!indexed && !postinc && (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR ? o.is_llocal : o.is_lval));
    if (!deref)
      continue;
    /* where it points: `&obj + k` through TEMPs and VARs, plus the index of
     * an indexed access when that is a constant */
    int off = 0, w = 0, ok = 0;
    if (!index_role && !postinc)
    {
      IROperand val = o;
      val.is_lval = 0;
      val.is_llocal = 0;
      ok = nrvo_arg_kind(ir, val, j, p0, &off) == 1;
      if (ok && indexed)
      {
        IROperand ix = tcc_ir_op_get_src2(ir, q), sc = tcc_ir_op_get_scale(ir, q);
        ok = irop_get_tag(ix) == IROP_TAG_IMM32 && !ix.is_sym &&
             (irop_is_none(sc) || (irop_get_tag(sc) == IROP_TAG_IMM32 && !sc.is_sym));
        if (ok)
          off += (int)irop_get_imm64_ex(ir, ix) << (irop_is_none(sc) ? 0 : (int)irop_get_imm64_ex(ir, sc));
        w = nrvo_access_width(tcc_ir_op_get_dest_or_src1(ir, q, op == TCCIR_OP_STORE_INDEXED));
      }
      else
        w = nrvo_access_width(o);
    }
    const int is_wr = (k == 0 && !(op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_LOAD_POSTINC)) &&
                      (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_STORE_POSTINC);
    if (!ok)
    {
      acc |= unknown;
      continue;
    }
    if (nrvo_frame_cover(obj, nobj, ov, off, w, B, E))
      acc |= is_wr ? NRVO_WR : NRVO_RD;
  }
  return acc;
}

/* Is any `hit` instruction reached from a successor of a `start` one, along
 * paths that end at a `stop` (checked after `hit`)? */
static int nrvo_live_after(TCCIRState *ir, const uint8_t *start, const uint8_t *hit, const uint8_t *stop)
{
  const int n = ir->next_instruction_index;
  uint8_t *seen = tcc_mallocz(n + 1);
  int *stack = tcc_malloc(sizeof(int) * (n + 1));
  int cap = 64, sp = 0, found = 0;
  for (int t = 0; t < ir->num_switch_tables; t++)
    if (ir->switch_tables[t].num_entries + 2 > cap)
      cap = ir->switch_tables[t].num_entries + 2;
  int *succ = tcc_malloc(sizeof(int) * cap);
  for (int j = 0; j < n && !found; j++)
  {
    if (!start[j])
      continue;
    const int ns = nrvo_succ(ir, j, succ, cap);
    if (ns < 0)
      found = 1;
    for (int k = 0; k < ns; k++)
      if (succ[k] >= 0 && succ[k] < n && !seen[succ[k]])
      {
        seen[succ[k]] = 1;
        stack[sp++] = succ[k];
      }
  }
  while (sp && !found)
  {
    const int j = stack[--sp];
    if (hit[j])
    {
      found = 1;
      break;
    }
    if (stop[j])
      continue;
    const int ns = nrvo_succ(ir, j, succ, cap);
    if (ns < 0)
    {
      found = 1;
      break;
    }
    for (int k = 0; k < ns; k++)
      if (succ[k] >= 0 && succ[k] < n && !seen[succ[k]])
      {
        seen[succ[k]] = 1;
        stack[sp++] = succ[k];
      }
  }
  tcc_free(seen);
  tcc_free(stack);
  tcc_free(succ);
  return found;
}

/* The *P0 byte an argument of a copy names: P0 itself (0), or one of the
 * objects'; else -1. */
static int nrvo_arg_pos(TCCIRState *ir, IROperand a, int at, int32_t p0, const NrvoObj *obj, int nobj)
{
  int off = 0;
  const int kind = nrvo_arg_kind(ir, a, at, p0, &off);
  if (kind == 3)
    return 0;
  if (kind != 1)
    return -1;
  const int ob = nrvo_obj_of(obj, nobj, off);
  return ob < 0 ? -1 : obj[ob].base + off - obj[ob].lo;
}

/* A whole local S copied into the family's bytes [B, B + size) of *P0 (R):
 * can S live in those bytes?  Neither may escape.  The copies between S and
 * R at matching bytes -- the one that merges them, and any the other way --
 * become no-ops, and where one runs the two hold the same value.  Elsewhere
 * no path may carry a reference to one of them to a reference to the other
 * (a copy reading it counts) without such a copy in between: then S's value
 * is dead wherever R's bytes are live, and R's wherever S is. */
static int nrvo_merge_ok(TCCIRState *ir, const NrvoObj *obj, int nobj, const NrvoObj *s, int copy, int32_t p0)
{
  const int n = ir->next_instruction_index;
  NrvoRange r[NRVO_MAX_OBJ + 1];
  for (int k = 0; k < nobj; k++)
    r[k] = (NrvoRange){obj[k].lo, obj[k].lo + obj[k].size};
  r[nobj] = (NrvoRange){s->lo, s->lo + s->size};
  NrvoUses u = {.borrow_calls = 1};
  u.ref = tcc_malloc(sizeof(uint64_t) * n);
  u.ind = tcc_malloc(sizeof(uint64_t) * n);
  nrvo_uses(ir, r, nobj + 1, &u);
  int ok = !u.bad && !u.esc;
  if (!ok && nrvo_dbg())
    fprintf(stderr, "[NRVO]   merge of %d at %d: an address escapes (%llx)\n", s->lo, copy, (unsigned long long)u.esc);
  if (ok)
  {
    const int B = s->base, E = s->base + s->size;
    uint64_t ov = 0;
    NrvoObj all[NRVO_MAX_OBJ + 1];
    memcpy(all, obj, sizeof(NrvoObj) * nobj);
    all[nobj] = *s;
    for (int k = 0; k < nobj; k++)
      if (obj[k].base < E && obj[k].base + obj[k].size > B)
        ov |= 1ull << k;
    /* the copies between S and R: 1 from S, 2 from R; they, their PARAMs and
     * the argument TEMPs those pass are not references */
    uint8_t *idc = tcc_mallocz(n), *own = tcc_mallocz(n);
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (!nrvo_is_call(q->op) || !ir_opt_is_memcpy_or_memmove_name(nrvo_callee(ir, q)))
        continue;
      IROperand a[3];
      if (!nrvo_call_args(ir, j, a) || (int)irop_get_imm64_ex(ir, a[2]) != s->size)
        continue;
      const int pd = nrvo_arg_pos(ir, a[0], j, p0, all, nobj + 1), ps = nrvo_arg_pos(ir, a[1], j, p0, all, nobj + 1);
      if (pd != B || ps != B)
        continue;
      int od = 0, os = 0;
      const int dk = nrvo_arg_kind(ir, a[0], j, p0, &od), sk = nrvo_arg_kind(ir, a[1], j, p0, &os);
      const int src_s = sk == 1 && os >= s->lo && os < s->lo + s->size;
      const int dst_s = dk == 1 && od >= s->lo && od < s->lo + s->size;
      if (src_s == dst_s)
        continue;
      idc[j] = src_s ? 1 : 2;
      own[j] = 1;
      for (int k = 0; k < 3; k++)
      {
        int pi = ir_opt_get_call_param_index(ir, j, k);
        if (pi < 0)
          continue;
        own[pi] = 1;
        int d = nrvo_temp_def(ir, tcc_ir_op_src1_vreg(ir, &ir->compact_instructions[pi]), pi);
        if (d >= 0)
          own[d] = 1;
      }
    }
    /* S and R are one set of bytes after the merge: wrong exactly when one
     * is written while the other's value is still to be read (live).  A copy
     * between them at matching bytes writes one with the other's value: no
     * conflict, and it ends the life of the old value of what it writes. */
    uint8_t *sw = tcc_mallocz(n), *sr = tcc_mallocz(n), *sk = tcc_mallocz(n);
    uint8_t *rw = tcc_mallocz(n), *rr = tcc_mallocz(n), *rk = tcc_mallocz(n);
    const uint64_t sbit = 1ull << nobj;
    for (int j = 0; j < n; j++)
    {
      if (idc[j])
      {
        /* 1: S -> R, 2: R -> S */
        (idc[j] == 1 ? sr : rr)[j] = 1;
        (idc[j] == 1 ? rk : sk)[j] = 1;
        continue;
      }
      if (own[j])
        continue;
      const int as = nrvo_access(ir, all, nobj + 1, &u, sbit, j, B, E, p0);
      const int ar = nrvo_access(ir, all, nobj + 1, &u, ov, j, B, E, p0);
      sw[j] = (as & NRVO_WR) != 0;
      sr[j] = (as & NRVO_RD) != 0;
      sk[j] = (as & NRVO_KILL) != 0;
      rw[j] = (ar & NRVO_WR) != 0;
      rr[j] = (ar & NRVO_RD) != 0;
      rk[j] = (ar & NRVO_KILL) != 0;
      /* one instruction touching both: only a copy between them is known */
      if (as && ar)
        ok = 0;
    }
    if (ok)
      ok = !nrvo_live_after(ir, sw, rr, rk) && !nrvo_live_after(ir, rw, sr, sk);
    if (!ok && nrvo_dbg())
    {
      fprintf(stderr, "[NRVO]   merge of %d at %d: lifetimes meet (%d/%d)\n", s->lo, copy,
              nrvo_live_after(ir, sw, rr, rk), nrvo_live_after(ir, rw, sr, sk));
    }
    tcc_free(sw);
    tcc_free(sr);
    tcc_free(sk);
    tcc_free(rw);
    tcc_free(rr);
    tcc_free(rk);
    tcc_free(idc);
    tcc_free(own);
  }
  tcc_free(u.ref);
  tcc_free(u.ind);
  return ok;
}

/* Grow the family by locals copied whole into its bytes. */
static void nrvo_merge(TCCIRState *ir, NrvoObj *obj, int *pnobj, int32_t p0, int buf_align)
{
  const int n = ir->next_instruction_index;
  int nobj = *pnobj;
  /* each candidate costs a pass over the function: a bounded number of them
   * keeps a function of thousands of copies linear */
  int budget = NRVO_MAX_CANDIDATES;
  for (int round = 0; nobj < NRVO_MAX_OBJ && round < NRVO_MAX_OBJ; round++)
  {
    int added = 0;
    for (int i = 0; i < n && !added && budget > 0; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (!nrvo_is_call(q->op) || !ir_opt_is_memcpy_or_memmove_name(nrvo_callee(ir, q)))
        continue;
      IROperand a[3];
      int d = 0, s = 0;
      if (!nrvo_call_args(ir, i, a) || nrvo_arg_kind(ir, a[0], i, p0, &d) != 1 ||
          nrvo_arg_kind(ir, a[1], i, p0, &s) != 1)
        continue;
      const int cn = (int)irop_get_imm64_ex(ir, a[2]);
      const int od = nrvo_obj_of(obj, nobj, d);
      if (od < 0 || nrvo_obj_of(obj, nobj, s) >= 0 || cn <= 0 || (s & 3) || d + cn > obj[od].lo + obj[od].size ||
          tcc_ir_frame_object_size_at(ir, s) != cn)
        continue;
      /* S must not overlap the family's frame bytes */
      int clash = 0;
      for (int k = 0; k < nobj; k++)
        clash |= s < obj[k].lo + obj[k].size && s + cn > obj[k].lo;
      const int B = obj[od].base + d - obj[od].lo;
      if (clash || (B & 3) || buf_align < 4)
        continue;
      NrvoObj cand = {.lo = s, .size = cn, .base = B};
      budget--;
      if (!nrvo_merge_ok(ir, obj, nobj, &cand, i, p0))
        continue;
      obj[nobj++] = cand;
      added = 1;
    }
    if (!added)
      break;
  }
  *pnobj = nobj;
}

int tcc_ir_opt_sret_nrvo(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  /* a result returned in s0-s7 has no caller's buffer: P0 is no pointer */
  if (!func_vc || n == 0 || ir->vfp_ret_words)
    return 0;
  if (nrvo_frame_hidden(ir) || tcc_ir_calls_returns_twice(ir))
    NRVO_FAIL();
  const int32_t p0 = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0);
  int buf_align = 1;
  type_size(&func_vt, &buf_align);
  nrvo_cache_begin(ir); /* until the IR is changed (nrvo_lower_fills) */

  /* The copies out: memmove/memcpy(sret, src, N), each followed only by the
   * return.  The object is the local most of them copy; the others copy from
   * another local or from a symbol (`return (struct E){.error = ...}`), whose
   * bytes are no part of *P0, and stay. */
  int sites[NRVO_MAX_SITES], site_off[NRVO_MAX_SITES], site_obj[NRVO_MAX_SITES];
  int nsites = 0, size = -1;
  NrvoObj obj[NRVO_MAX_OBJ];
  int nobj = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int32_t dv = irop_config[q->op].has_dest ? tcc_ir_op_dest_vreg(ir, q) : -1;
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
    const int kind = nrvo_arg_kind(ir, a[1], i, p0, &src_off);
    if (nsites == NRVO_MAX_SITES || (kind != 1 && kind != 2))
      NRVO_FAIL(); /* a copy out from a pointer, or too many */
    const int sz = (int)irop_get_imm64_ex(ir, a[2]);
    if (sz <= 0 || (size >= 0 && sz != size))
      NRVO_FAIL();
    size = sz;
    site_obj[nsites] = kind == 1 && !(src_off & 3) && !(size & 3) && tcc_ir_frame_object_size_at(ir, src_off) == size;
    site_off[nsites] = src_off;
    sites[nsites++] = i;
  }
  if (!nsites)
    NRVO_FAIL();
  {
    int best = -1, best_n = 0;
    for (int s = 0; s < nsites; s++)
    {
      if (!site_obj[s])
        continue;
      int c = 0;
      for (int t = 0; t < nsites; t++)
        c += site_obj[t] && site_off[t] == site_off[s];
      if (c > best_n)
        best = s, best_n = c;
    }
    if (best < 0)
      NRVO_FAIL();
    obj[nobj++] = (NrvoObj){.lo = site_off[best], .size = size, .base = 0};
  }
  /* each then only returns: NOPs, and one jump straight to the return or the end */
  for (int s = 0; s < nsites; s++)
  {
    int j = sites[s] + 1, jumped = 0;
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
        int t = (int)tcc_ir_op_dest_imm(ir, q);
        if (t <= sites[s])
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
  if (!nrvo_p0_only_copies(ir, sites, nsites, p0))
    NRVO_FAIL();

  nrvo_merge(ir, obj, &nobj, p0, buf_align);

  /* Where the family's addresses go. */
  {
    NrvoRange r[NRVO_MAX_OBJ];
    for (int k = 0; k < nobj; k++)
      r[k] = (NrvoRange){obj[k].lo, obj[k].lo + obj[k].size};
    NrvoUses u = {.borrow_calls = 1};
    u.at = tcc_malloc(sizeof(uint64_t) * (n + 1));
    nrvo_uses(ir, r, nobj, &u);
    int bad = u.bad;
    /* An escaped address may be read through by a callee, so the bytes must
     * be the object's alone -- no other object merged into them -- and no
     * callee the escape reaches may get them as its result buffer, which it
     * assumes nothing else can reach. */
    if (!bad && u.esc && (nobj > 1 || u.sret))
    {
      bad = nobj > 1;
      uint8_t *start = tcc_malloc(n + 1);
      for (int j = 0; j < n; j++)
        start[j] = u.at[j] != 0;
      uint8_t *reach = bad ? NULL : nrvo_reach_from(ir, start);
      for (int j = 0; j < n && !bad; j++)
      {
        IRQuadCompact *q = &ir->compact_instructions[j];
        if (!reach[j] || !nrvo_is_call(q->op))
          continue;
        const int c = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
        IROperand r0;
        int off = 0;
        if (ir->sret_calls && c >= 0 && c < ir->sret_calls_size && ir->sret_calls[c] > 0 &&
            ir_opt_get_call_param_operand(ir, j, 0, &r0) &&
            (nrvo_arg_kind(ir, r0, j, p0, &off) != 1 || nrvo_obj_of(obj, nobj, off) >= 0))
          bad = 1; /* (a buffer not known to be elsewhere counts) */
      }
      tcc_free(start);
      tcc_free(reach);
      if (bad && nrvo_dbg())
        fprintf(stderr, "[NRVO] %s: escaping family used as a result buffer\n", funcname);
    }
    tcc_free(u.at);
    if (bad)
      NRVO_FAIL();
  }

  /* Every reference to the objects: shapes the rewrite handles. */
  int nrefs = 0;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 4; k++)
    {
      IROperand o;
      /* (a vreg-backed slot's offset is only the front end's watermark: it
       * names the vreg, not frame bytes) */
      if (!nrvo_operand(ir, q, k, &o) || irop_get_tag(o) != IROP_TAG_STACKOFF || irop_get_vreg(o) >= 0)
        continue;
      int ob = nrvo_obj_of(obj, nobj, irop_get_stack_offset(o));
      if (ob < 0)
        continue;
      if (!o.is_local || o.is_llocal)
      {
        if (nrvo_dbg())
          fprintf(stderr, "[NRVO] %s: instr %d op %s operand %d: local %d llocal %d vreg %d\n", funcname, j,
                  tcc_ir_get_op_name(q->op), k, o.is_local, o.is_llocal, (int)irop_get_vreg(o));
        NRVO_FAIL(); /* not a plain slot or address: not rewritten here */
      }
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

  nrvo_cache_end();
  n += nrvo_lower_fills(ir, obj, nobj, p0, n);
  n = ir->next_instruction_index;

  /* Copies into and out of the objects keep their inline expansion: the
   * buffer has the return type's alignment, so a word-aligned destination and
   * source make them __aeabi_memcpy4 (the backend expands only copies it can
   * prove aligned, and P0 + k is no frame slot). */
  for (int j = 0; buf_align >= 4 && j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    const char *nm = nrvo_is_call(q->op) ? nrvo_callee(ir, q) : NULL;
    if (!nm || (strcmp(nm, "__aeabi_memcpy") && strcmp(nm, "__aeabi_memmove")))
      continue;
    IROperand a[3];
    if (!nrvo_call_args(ir, j, a))
      continue;
    int fam = 0, aligned = 1;
    for (int k = 0; k < 2; k++)
    {
      int off = 0;
      const int kind = nrvo_arg_kind(ir, a[k], j, p0, &off);
      const int ob = kind == 1 ? nrvo_obj_of(obj, nobj, off) : -1;
      if (ob >= 0)
      {
        fam = 1;
        aligned &= !((obj[ob].base + off - obj[ob].lo) & 3);
      }
      else if (kind == 1)
        aligned &= !(off & 3);
      else if (kind == 2 && k == 1)
        aligned &= nrvo_sym_word_aligned(ir, nrvo_arg_src(ir, a[1], j));
      else
        aligned = 0;
    }
    if (!fam || !aligned || ((int)irop_get_imm64_ex(ir, a[2]) & 3))
      continue;
    change_callee_sym_keep_type(ir, j, !strcmp(nm, "__aeabi_memcpy") ? "__aeabi_memcpy4" : "__aeabi_memmove4");
  }

  /* The copies out, and the copies that merged an object into the family,
   * now copy bytes of *P0 onto themselves. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!nrvo_is_call(q->op) || !ir_opt_is_memcpy_or_memmove_name(nrvo_callee(ir, q)))
      continue;
    IROperand a[3];
    if (!nrvo_call_args(ir, i, a))
      continue;
    const int pd = nrvo_arg_pos(ir, a[0], i, p0, obj, nobj), ps = nrvo_arg_pos(ir, a[1], i, p0, obj, nobj);
    if (pd < 0 || pd != ps)
      continue;
    nrvo_drop_memop(ir, i, a[0]);
  }

  /* Rewrite, last instruction first so the insertions do not move what is left. */
  int changes = 0;
  for (int j = n - 1; j >= 0; j--)
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
      const int disp = obj[ob].base + off - obj[ob].lo;
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
        IROperand other = tcc_ir_op_get_dest_or_src1(ir, q, q->op == TCCIR_OP_STORE);
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
        ld.line_num = ir->compact_instructions[j + 1].line_num;
        nrvo_insert(ir, j + 1, &ld); /* after the address, before q */
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

  if (nrvo_dbg())
    fprintf(stderr, "[NRVO] %s: built in *P0, %d objects, %d copies out, %d rewrites\n", funcname, nobj, nsites,
            changes);
  return changes + 1;
}
