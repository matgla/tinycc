/*
 *  TCC IR - Byte-store merge + const-memcpy-to-dest (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include <limits.h>

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "opt_loop_utils.h"


/* Return the byte width of an IROP_BTYPE_* value. */
static int irop_btype_byte_width(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
    return 4;
  case IROP_BTYPE_INT64:
    return 8;
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 4; /* struct, func, etc. — conservative */
  }
}

/* Single-def map for TEMP vregs: entry = defining instr index, -1 none,
   RSE_DEF_MULTI multiple. Keeps address resolution O(1), not O(n^2). */
#define RSE_DEF_MULTI (-2)
static int *rse_def_map;
static int rse_def_map_size;

static void rse_build_def_map(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int max_pos = -1;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *dq = &ir->compact_instructions[j];
    if (dq->op == TCCIR_OP_NOP || !irop_config[dq->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, dq);
    int32_t dvr = irop_get_vreg(d);
    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      int p = TCCIR_DECODE_VREG_POSITION(dvr);
      if (p > max_pos)
        max_pos = p;
    }
  }
  /* Free any prior map before reallocating; rebuilt after each rewrite. */
  tcc_free(rse_def_map);
  rse_def_map = NULL;
  rse_def_map_size = max_pos + 1;
  if (rse_def_map_size <= 0)
    return;
  rse_def_map = (int *)tcc_malloc(sizeof(int) * rse_def_map_size);
  for (int i = 0; i < rse_def_map_size; i++)
    rse_def_map[i] = -1;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *dq = &ir->compact_instructions[j];
    if (dq->op == TCCIR_OP_NOP)
      continue;
    if (dq->op == TCCIR_OP_STORE_INDEXED || dq->op == TCCIR_OP_STORE_POSTINC)
      continue;
    if (!irop_config[dq->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, dq);
    if (d.is_lval)
      continue;
    int32_t dvr = irop_get_vreg(d);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(dvr);
    rse_def_map[p] = (rse_def_map[p] == -1) ? j : RSE_DEF_MULTI;
  }
}

static void rse_free_def_map(void)
{
  tcc_free(rse_def_map);
  rse_def_map = NULL;
  rse_def_map_size = 0;
}

static int rse_resolve_temp_addr_impl(TCCIRState *ir, int32_t vr,
                                      const Sym **out_sym, int64_t *out_off,
                                      int depth)
{
  if (depth <= 0)
    return 0;
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (!rse_def_map || pos >= rse_def_map_size)
    return 0;
  int def_idx = rse_def_map[pos];
  if (def_idx < 0) /* -1 (none) or RSE_DEF_MULTI */
    return 0;

  IRQuadCompact *dq = &ir->compact_instructions[def_idx];
  if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_LEA && dq->op != TCCIR_OP_ASSIGN)
    return 0;
  IROperand s1 = tcc_ir_op_get_src1(ir, dq);

  int64_t base_off = 0;
  const Sym *base_sym = NULL;
  int resolved = 0;

  /* Case A: src1 is a SYMREF (&global + addend). */
  if (s1.is_sym && !s1.is_lval)
  {
    IRPoolSymref *sr = irop_get_symref_ex(ir, s1);
    if (!sr || !sr->sym)
      return 0;
    base_sym = sr->sym;
    base_off = (int64_t)sr->addend;
    resolved = 1;
  }
  /* Case B: src1 is a stack-local STACKOFF (Addr[StackLoc[off]]).  A non-zero
     vreg_type marks a vreg's *potential* spill encoding, where the offset is only
     metadata and the program reads the vreg — see IROP_TAG_STACKOFF in
     tccir_operand.h.  Treating one as a real slot would key two unrelated
     addresses the same. */
  else if (s1.is_local && !s1.is_lval && !s1.is_llocal && irop_get_tag(s1) == IROP_TAG_STACKOFF &&
           irop_get_vreg(s1) < 0)
  {
    base_sym = NULL;
    base_off = irop_get_stack_offset(s1);
    resolved = 1;
  }
  /* Case C: src1 is itself a TEMP — recurse. */
  else if (!s1.is_lval && irop_get_tag(s1) == IROP_TAG_VREG)
  {
    int32_t inner_vr = irop_get_vreg(s1);
    if (!rse_resolve_temp_addr_impl(ir, inner_vr, &base_sym, &base_off, depth - 1))
      return 0;
    resolved = 1;
  }

  if (!resolved)
    return 0;

  if (dq->op == TCCIR_OP_ADD)
  {
    IROperand s2 = tcc_ir_op_get_src2(ir, dq);
    if (!irop_is_immediate(s2))
      return 0;
    base_off += irop_get_imm64_ex(ir, s2);
  }

  *out_sym = base_sym;
  *out_off = base_off;
  return 1;
}

static int rse_resolve_temp_addr(TCCIRState *ir, int32_t vr,
                                 const Sym **out_sym, int64_t *out_off)
{
  return rse_resolve_temp_addr_impl(ir, vr, out_sym, out_off, 4);
}

/* Reject accesses reaching outside `sym`: a cross-symbol key would alias two
   addresses and could wrongly drop a live store. Returns 1 on escape. */
static int rse_addr_escapes_sym(const Sym *sym, int64_t off, int width)
{
  if (!sym)
    return 0;
  int align;
  int size = type_size(&sym->type, &align);
  if (size <= 0)
    return 0; /* incomplete / unknown size — can't prove an escape */
  if (width <= 0)
    width = 1;
  return off < 0 || off + (int64_t)width > (int64_t)size;
}

/* Resolve a store's destination to a (sym, byte_offset) pair; 1 on success. */
static int rse_resolve_store_addr(TCCIRState *ir, IRQuadCompact *q,
                                  const Sym **out_sym, int64_t *out_off)
{
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int op = q->op;
  if (op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED)
    return 0;

  int64_t extra = 0;
  if (op == TCCIR_OP_STORE_INDEXED)
  {
    IROperand idx = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(idx))
      return 0;
    int64_t scale = 0;
    IROperand sc = tcc_ir_op_get_scale(ir, q);
    if (irop_is_immediate(sc))
      scale = irop_get_imm64_ex(ir, sc);
    extra = irop_get_imm64_ex(ir, idx) << scale;
  }

  int store_width = (op == TCCIR_OP_STORE_INDEXED)
                        ? irop_btype_byte_width(tcc_ir_op_get_src1(ir, q).btype)
                        : irop_btype_byte_width(dest.btype);

  /* Direct SYMREF dest (STORE_INDEXED base may have lost is_lval via disp_fusion). */
  if (dest.is_sym)
  {
    if (op == TCCIR_OP_STORE && !dest.is_lval)
      return 0;
    IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
    if (!sr || !sr->sym)
      return 0;
    int64_t off = (int64_t)sr->addend + extra;
    if (rse_addr_escapes_sym(sr->sym, off, store_width))
      return 0;
    *out_sym = sr->sym;
    *out_off = off;
    return 1;
  }

  /* Direct anonymous stack slot: `StackLoc[off] <-- value`.  Keyed the same way
     as the LEA form above (NULL sym, frame offset), so the two unify.  Restricted
     to slots with no backing vreg: a VAR-backed local reports offset 0 whatever
     its position, so its offset would alias every other VAR's. */
  if (dest.is_local && dest.is_lval && !dest.is_llocal && irop_get_tag(dest) == IROP_TAG_STACKOFF &&
      irop_get_vreg(dest) < 0)
  {
    *out_sym = NULL;
    *out_off = irop_get_stack_offset(dest) + extra;
    return 1;
  }

  /* TEMP base form: dest is the TEMP holding the address. */
  if (op == TCCIR_OP_STORE && !dest.is_lval)
    return 0;
  if (op == TCCIR_OP_STORE_INDEXED && dest.is_lval)
    return 0;
  int32_t vr = irop_get_vreg(dest);
  if (!rse_resolve_temp_addr(ir, vr, out_sym, out_off))
    return 0;
  *out_off += extra;
  if (rse_addr_escapes_sym(*out_sym, *out_off, store_width))
    return 0;
  return 1;
}

/* True when `q` neither touches memory nor transfers control, so it can sit
   between two stores being merged without affecting the merge: the merged store
   lands at the first store's position, and only a memory access (which could
   read the half-filled word or alias it) or a branch (which could skip a
   constituent store) makes moving the later writes earlier observable.  Any
   lvalue operand is a deref, so those count as memory accesses too. */
static int rse_is_merge_transparent(TCCIRState *ir, IRQuadCompact *q)
{
  switch (q->op)
  {
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
    break;
  default:
    return 0;
  }
  if (q->is_jump_target)
    return 0;
  if (irop_config[q->op].has_src1 && tcc_ir_op_get_src1(ir, q).is_lval)
    return 0;
  if (irop_config[q->op].has_src2 && tcc_ir_op_get_src2(ir, q).is_lval)
    return 0;
  if (irop_config[q->op].has_dest && tcc_ir_op_get_dest(ir, q).is_lval)
    return 0;
  return 1;
}

/* Regions handed to a mem* helper as its source buffer.
 *
 * Widening the stores that fill such a buffer is semantically fine but loses
 * code: tcc_ir_opt_const_memcpy_to_dest (which runs much later, from regalloc)
 * rewrites the copy into stores of the buffer's constant contents *at the
 * granularity it finds them*, and only same-width stores forward into the
 * destination's loads.  20030408-1 fills `const char X[8]` byte by byte, copies
 * it to `buffer`, then compares `buffer` byte by byte: left alone the whole
 * function folds to `return 0`, merged it becomes 8 loads and 8 compares,
 * because a word store does not forward into a byte load at offset +1.
 *
 * This is a quality heuristic, not a correctness gate — an address or size that
 * does not resolve simply imposes no constraint. */
#define RSE_MAX_COPY_SRC 32
typedef struct
{
  const Sym *sym;
  int64_t base;
  int64_t end;
} RseRegion;

/* Resolve a call-argument operand to the (sym, offset) address it denotes. */
static int rse_resolve_arg_addr(TCCIRState *ir, IROperand arg, const Sym **out_sym, int64_t *out_off)
{
  if (arg.is_lval)
    return 0;
  if (arg.is_sym)
  {
    IRPoolSymref *sr = irop_get_symref_ex(ir, arg);
    if (!sr || !sr->sym)
      return 0;
    *out_sym = sr->sym;
    *out_off = (int64_t)sr->addend;
    return 1;
  }
  if (arg.is_local && !arg.is_llocal && irop_get_tag(arg) == IROP_TAG_STACKOFF && irop_get_vreg(arg) < 0)
  {
    *out_sym = NULL;
    *out_off = irop_get_stack_offset(arg);
    return 1;
  }
  return rse_resolve_temp_addr(ir, irop_get_vreg(arg), out_sym, out_off);
}

static int rse_collect_copy_sources(TCCIRState *ir, RseRegion *out, int max)
{
  int n = ir->next_instruction_index;
  int count = 0;

  for (int i = 0; i < n && count < max; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    /* mem{cpy,move}(dst, src, size): args are looked up by call id rather than
       by position in the stream — the address computation for one argument is
       emitted between the FUNCPARAM ops of the others. */
    IROperand size_op, src_op;
    if (!ir_opt_get_call_param_operand(ir, i, 2, &size_op))
      continue;
    if (!irop_is_immediate(size_op) || size_op.is_sym)
      continue;
    int64_t size = irop_get_imm64_ex(ir, size_op);
    if (size <= 0 || size > 4096)
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 1, &src_op))
      continue;

    const Sym *sym = NULL;
    int64_t off = 0;
    if (!rse_resolve_arg_addr(ir, src_op, &sym, &off))
      continue;

    out[count].sym = sym;
    out[count].base = off;
    out[count].end = off + size;
    count++;
  }
  return count;
}

/* Merge consecutive constant sub-word stores that together fill one aligned word
   into a single INT32 store, so the redundant-store pass can see the wider write.
   Handles 4 INT8, 2 INT16, and INT8/INT16 mixtures alike — a `vector(8, short)`
   built lane by lane is two constant `strh`s per word, the same shape as four
   constant `strb`s. */
int tcc_ir_opt_byte_store_merge(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  /* rse_resolve_store_addr resolves a TEMP base through this map; without it
     every store whose address sits in a temp silently failed to resolve, and
     only the direct-SYMREF form could ever merge. */
  rse_build_def_map(ir);

  RseRegion copy_srcs[RSE_MAX_COPY_SRC];
  int copy_src_count = rse_collect_copy_sources(ir, copy_srcs, RSE_MAX_COPY_SRC);

  const Sym *grp_sym = NULL;
  int64_t grp_base = 0;
  int grp_filled = 0; /* bytes of the word covered so far, always from byte 0 */
  int grp_count = 0;  /* number of stores in the group */
  int grp_indices[4];
  int32_t grp_merged = 0;

  for (int i = 0; i <= n; i++)
  {
    int is_sub_word_store = 0;
    int cur_width = 0;
    const Sym *cur_sym = NULL;
    int64_t cur_off = 0;
    uint32_t cur_val = 0;

    if (i < n)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];

      /* Lanes of an aggregate are built value-then-address, so the constituent
         stores are separated by the ADD/LEA that forms the next lane's address. */
      if (q->op == TCCIR_OP_NOP || rse_is_merge_transparent(ir, q))
        continue;

      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int store_btype;
        if (q->op == TCCIR_OP_STORE_INDEXED)
          store_btype = irop_get_btype(src1);
        else
          store_btype = irop_get_btype(tcc_ir_op_get_dest(ir, q));

        if ((store_btype == IROP_BTYPE_INT8 || store_btype == IROP_BTYPE_INT16) && irop_is_immediate(src1) &&
            rse_resolve_store_addr(ir, q, &cur_sym, &cur_off))
        {
          cur_width = (store_btype == IROP_BTYPE_INT8) ? 1 : 2;
          /* A misaligned halfword store straddles the word boundary. */
          if (cur_width == 1 || (cur_off & 1) == 0)
          {
            uint32_t mask = (cur_width == 1) ? 0xFFu : 0xFFFFu;
            cur_val = (uint32_t)irop_get_imm64_ex(ir, src1) & mask;
            is_sub_word_store = 1;
          }
        }
      }
    }

    if (is_sub_word_store)
    {
      int64_t aligned_base = cur_off & ~3LL;
      int byte_pos = (int)(cur_off & 3);

      if (grp_count > 0 && cur_sym == grp_sym && aligned_base == grp_base && byte_pos == grp_filled)
      {
        grp_merged |= cur_val << (byte_pos * 8);
        grp_indices[grp_count++] = i;
        grp_filled += cur_width;
      }
      else
      {
        grp_count = 0;
        grp_filled = 0;
        if (byte_pos == 0)
        {
          grp_sym = cur_sym;
          grp_base = aligned_base;
          grp_merged = cur_val;
          grp_indices[0] = i;
          grp_count = 1;
          grp_filled = cur_width;
        }
      }

      if (grp_filled == 4)
      {
        int feeds_copy = 0;
        for (int k = 0; k < copy_src_count; k++)
          if (copy_srcs[k].sym == grp_sym && grp_base < copy_srcs[k].end && copy_srcs[k].base < grp_base + 4)
          {
            feeds_copy = 1;
            break;
          }
        if (feeds_copy)
        {
          grp_count = 0;
          grp_filled = 0;
          continue;
        }

        IRQuadCompact *fq = &ir->compact_instructions[grp_indices[0]];
        if (fq->op == TCCIR_OP_STORE)
        {
          IROperand dest = tcc_ir_op_get_dest(ir, fq);
          dest.btype = IROP_BTYPE_INT32;
          tcc_ir_op_set_dest(ir, fq, dest);
        }
        IROperand new_src1 = irop_make_imm32(-1, (int32_t)grp_merged, IROP_BTYPE_INT32);
        tcc_ir_op_set_src1(ir, fq, new_src1);

        for (int k = 1; k < grp_count; k++)
          ir->compact_instructions[grp_indices[k]].op = TCCIR_OP_NOP;

        changes++;
        grp_count = 0;
        grp_filled = 0;
      }
    }
    else
    {
      grp_count = 0;
      grp_filled = 0;
    }
  }

  rse_free_def_map();
  return changes;
}

int tcc_ir_opt_byte_store_merge_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_byte_store_merge(ctx->ir);
}

/* Rewrite an aligned AEABI mem{cpy,move}{4,8} whose source is a non-escaping
   stack buffer filled only with compile-time constants into wide constant stores
   to the destination, dropping the buffer and the call. Safe because the source
   bytes are constants (no aliasing hazard) and the 4-byte/8-byte variants
   guarantee alignment. Gated on: full isolation/non-escape scan, complete const coverage,
   and a straight-line fill..call region. */

#define CMD_MAX_BYTES 64 /* cap on copy size we expand inline */

/* Resolve an address-holding operand to (sym, byte_off): direct
   `Addr[StackLoc[off]]` or a single-def TEMP (incl. +imm chains). */
static int cmd_resolve_addr_op(TCCIRState *ir, IROperand op, const Sym **sym, int64_t *off)
{
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_llocal &&
      irop_get_vreg(op) == -1)
  {
    *sym = NULL;
    *off = irop_get_stack_offset(op);
    return 1;
  }
  int32_t vr = irop_get_vreg(op);
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    return rse_resolve_temp_addr(ir, vr, sym, off);
  return 0;
}

/* Resolve a non-lval operand that holds an address into (sym, byte_off). */
static int cmd_op_addr(TCCIRState *ir, IROperand op, const Sym **sym, int64_t *off)
{
  if (op.is_lval)
    return 0;
  return cmd_resolve_addr_op(ir, op, sym, off);
}

/* Resolve an lval operand (a memory access) to (sym, byte_off). */
static int cmd_op_lval(TCCIRState *ir, IROperand op, const Sym **sym, int64_t *off)
{
  if (!op.is_lval)
    return 0;
  return cmd_resolve_addr_op(ir, op, sym, off);
}

/* Resolve a STORE/STORE_INDEXED dest to (sym, byte_off) and width; also handles
   the direct `StackLoc[off]` lval form that rse_resolve_store_addr rejects. */
static int cmd_store_addr(TCCIRState *ir, IRQuadCompact *q, const Sym **sym,
                          int64_t *off, int *width)
{
  if (q->op == TCCIR_OP_STORE)
  {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!cmd_op_lval(ir, d, sym, off))
      return 0;
    int w = ir_opt_store_btype_size_bytes(irop_get_btype(d));
    *width = w > 0 ? w : 4;
    return 1;
  }
  if (q->op == TCCIR_OP_STORE_INDEXED)
  {
    IROperand base = tcc_ir_op_get_dest(ir, q);
    if (!cmd_op_addr(ir, base, sym, off))
      return 0;
    IROperand idx = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(idx))
      return 0;
    IROperand sc = tcc_ir_op_get_scale(ir, q);
    int64_t scale = irop_is_immediate(sc) ? irop_get_imm64_ex(ir, sc) : 0;
    *off += irop_get_imm64_ex(ir, idx) << scale;
    int w = ir_opt_store_btype_size_bytes(irop_get_btype(tcc_ir_op_get_src1(ir, q)));
    *width = w > 0 ? w : 4;
    return 1;
  }
  return 0;
}

/* Constant value of a fill source: immediate, or single-def TEMP assigned #imm. */
static int cmd_const_value(TCCIRState *ir, IROperand val, int64_t *out)
{
  if (irop_is_immediate(val))
  {
    *out = irop_get_imm64_ex(ir, val);
    return 1;
  }
  if (val.is_lval)
    return 0;
  int32_t vr = irop_get_vreg(val);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (!rse_def_map || pos >= rse_def_map_size)
    return 0;
  int d = rse_def_map[pos];
  if (d < 0)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[d];
  if (dq->op != TCCIR_OP_ASSIGN)
    return 0;
  IROperand s1 = tcc_ir_op_get_src1(ir, dq);
  if (!irop_is_immediate(s1))
    return 0;
  *out = irop_get_imm64_ex(ir, s1);
  return 1;
}

static int cmd_is_aligned_memcpy(TCCIRState *ir, IRQuadCompact *q)
{
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!callee)
    return 0;
  const char *name = get_tok_str(callee->v, NULL);
  if (!name)
    return 0;
  return strcmp(name, "__aeabi_memcpy4") == 0 || strcmp(name, "__aeabi_memcpy8") == 0 ||
         strcmp(name, "__aeabi_memmove4") == 0 || strcmp(name, "__aeabi_memmove8") == 0;
}

/* Try to rewrite one memmove/memcpy call at index ci.  Returns 1 on success. */
static int cmd_try_one(TCCIRState *ir, int ci)
{
  int n = ir->next_instruction_index;
  IRQuadCompact *call = &ir->compact_instructions[ci];

  if (!cmd_is_aligned_memcpy(ir, call))
    return 0;

  int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call)));

  /* The return value (memmove returns dest) must be dead. */
  if (call->op == TCCIR_OP_FUNCCALLVAL)
  {
    IROperand cd = tcc_ir_op_get_dest(ir, call);
    int32_t cdv = irop_get_vreg(cd);
    if (cdv >= 0)
    {
      for (int j = 0; j < n; j++)
      {
        if (j == ci)
          continue;
        IRQuadCompact *q = &ir->compact_instructions[j];
        if (q->op == TCCIR_OP_NOP)
          continue;
        for (int k = 1; k <= 3; k++)
        {
          IROperand op = (k == 1) ? tcc_ir_op_get_src1(ir, q)
                         : (k == 2) ? tcc_ir_op_get_src2(ir, q)
                                    : tcc_ir_op_get_accum(ir, q);
          if (irop_get_vreg(op) == cdv)
            return 0;
        }
      }
    }
  }

  /* Locate the three params. */
  int p0 = -1, p1 = -1, p2 = -1;
  for (int j = 0; j < ci; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;
    uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
    if (TCCIR_DECODE_CALL_ID(enc) != call_id)
      continue;
    int pidx = TCCIR_DECODE_PARAM_IDX(enc);
    if (pidx == 0) p0 = j;
    else if (pidx == 1) p1 = j;
    else if (pidx == 2) p2 = j;
  }
  if (p0 < 0 || p1 < 0 || p2 < 0)
    return 0;

  /* Size (param2) must be a small positive immediate. */
  IROperand sz_op = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p2]);
  if (!irop_is_immediate(sz_op))
    return 0;
  int64_t S = irop_get_imm64_ex(ir, sz_op);
  if (S <= 0 || S > CMD_MAX_BYTES)
    return 0;

  /* Destination (param0) must be a register-class pointer value. */
  IROperand D = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p0]);
  if (D.is_lval || irop_get_tag(D) != IROP_TAG_VREG)
    return 0;
  {
    int32_t dvr = irop_get_vreg(D);
    if (dvr < 0)
      return 0;
    int dty = TCCIR_DECODE_VREG_TYPE(dvr);
    if (dty != TCCIR_VREG_TYPE_PARAM && dty != TCCIR_VREG_TYPE_TEMP)
      return 0;
  }

  /* Source (param1) must resolve to a stack-local buffer start. */
  IROperand srcop = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p1]);
  const Sym *sym_b = NULL;
  int64_t off_b = 0;
  if (!cmd_op_addr(ir, srcop, &sym_b, &off_b))
    return 0;
  if (sym_b != NULL)
    return 0; /* stack-local only */

#define CMD_OVL(s, o, w) ((s) == sym_b && (int64_t)(o) < off_b + S && off_b < (int64_t)(o) + ((w) > 0 ? (w) : 1))

  /* Phase 1: collect const fills, build the byte image. */
  unsigned char image[CMD_MAX_BYTES];
  unsigned char defined[CMD_MAX_BYTES];
  memset(defined, 0, (size_t)S);
  int n_fills = 0;
  int fill_lo = ci;
  for (int i = 0; i < ci; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED)
      continue;
    const Sym *s;
    int64_t o;
    int w;
    if (!cmd_store_addr(ir, q, &s, &o, &w))
      continue;
    if (!CMD_OVL(s, o, w))
      continue;
    /* a store overlapping the buffer range: must be a constant fill */
    int64_t val;
    if (!cmd_const_value(ir, tcc_ir_op_get_src1(ir, q), &val))
      return 0;
    for (int b = 0; b < w; b++)
    {
      int64_t rel = o + b - off_b;
      if (rel >= 0 && rel < S)
      {
        image[rel] = (unsigned char)((uint64_t)val >> (8 * b));
        defined[rel] = 1;
      }
    }
    n_fills++;
    if (i < fill_lo) fill_lo = i;
  }
  if (n_fills == 0)
    return 0;
  for (int b = 0; b < S; b++)
    if (!defined[b])
      return 0; /* incomplete coverage */

  /* Phase 2: region gate — [fill_lo, ci] must be straight-line (no branch
     in/out) with no non-fill store, so the new dest stores can't reorder. */
  for (int i = fill_lo; i <= ci; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (i > fill_lo && q->is_jump_target)
      return 0;
    switch (q->op)
    {
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      return 0;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    {
      const Sym *s;
      int64_t o;
      int w;
      if (!cmd_store_addr(ir, q, &s, &o, &w) || !CMD_OVL(s, o, w))
        return 0; /* a non-fill store inside the region */
      break;
    }
    default:
      break;
    }
  }

  /* D must be live across the region: PARAM always, TEMP def must precede fill_lo. */
  {
    int32_t dvr = irop_get_vreg(D);
    if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      int dp = TCCIR_DECODE_VREG_POSITION(dvr);
      if (!rse_def_map || dp >= rse_def_map_size)
        return 0;
      int dd = rse_def_map[dp];
      if (dd < 0 || dd >= fill_lo)
        return 0;
    }
  }

  /* Phase 3: isolation scan over the whole function. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || i == ci)
      continue;

    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
      return 0; /* no other calls */

    if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
    {
      uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (TCCIR_DECODE_CALL_ID(enc) != call_id)
        return 0; /* param of another (gone) call */
      /* our own params: buffer source (p1) + pointer/size (p0/p2), all fine */
      continue;
    }

    const Sym *s;
    int64_t o;

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
    {
      int w;
      if (cmd_store_addr(ir, q, &s, &o, &w) && CMD_OVL(s, o, w))
      {
        /* a store into the buffer that is not a pre-call collected fill */
        if (i >= ci)
          return 0;
        /* collected fills were verified const above; nothing else to do */
        continue;
      }
      /* variable-indexed store whose base is the buffer? */
      if (q->op == TCCIR_OP_STORE_INDEXED &&
          cmd_op_addr(ir, tcc_ir_op_get_dest(ir, q), &s, &o) && CMD_OVL(s, o, 1))
        return 0;
      /* the stored value must not be the buffer address (escape) */
      if (cmd_op_addr(ir, tcc_ir_op_get_src1(ir, q), &s, &o) && CMD_OVL(s, o, 1))
        return 0;
      continue;
    }

    if (q->op == TCCIR_OP_LOAD)
    {
      if (cmd_op_lval(ir, tcc_ir_op_get_src1(ir, q), &s, &o) && CMD_OVL(s, o, 1))
        return 0; /* read of the buffer */
      continue;
    }
    if (q->op == TCCIR_OP_LOAD_INDEXED)
    {
      if (cmd_op_addr(ir, tcc_ir_op_get_src1(ir, q), &s, &o) && CMD_OVL(s, o, 1))
        return 0;
      continue;
    }

    /* generic op: no operand may touch the buffer except address-prop/compare */
    for (int k = 0; k < 4; k++)
    {
      int has = (k == 0) ? irop_config[q->op].has_dest
                : (k == 1) ? irop_config[q->op].has_src1
                : (k == 2) ? irop_config[q->op].has_src2
                           : (q->op == TCCIR_OP_MLA);
      if (!has)
        continue;
      IROperand op = (k == 0) ? tcc_ir_op_get_dest(ir, q)
                     : (k == 1) ? tcc_ir_op_get_src1(ir, q)
                     : (k == 2) ? tcc_ir_op_get_src2(ir, q)
                                : tcc_ir_op_get_accum(ir, q);
      if (op.is_lval)
      {
        if (cmd_op_lval(ir, op, &s, &o) && CMD_OVL(s, o, 1))
          return 0;
        continue;
      }
      if (k == 0)
        continue; /* a def is not a use */
      if (cmd_op_addr(ir, op, &s, &o) && CMD_OVL(s, o, 1))
      {
        if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA &&
            q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB && q->op != TCCIR_OP_CMP)
          return 0;
      }
    }
  }

  /* Phase 4: build wide store descriptors from the image. */
  struct { int rel; int w; uint32_t val; } desc[CMD_MAX_BYTES];
  int n_desc = 0;
  for (int rel = 0; rel < S;)
  {
    int rem = (int)S - rel;
    int w = rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
    uint32_t v = 0;
    for (int b = 0; b < w; b++)
      v |= (uint32_t)image[rel + b] << (8 * b);
    desc[n_desc].rel = rel;
    desc[n_desc].w = w;
    desc[n_desc].val = v;
    n_desc++;
    rel += w;
  }

  /* Slots = collected fills + 3 params + call, ascending. Descriptors go into
     the LAST n_desc so the new stores stay at the original copy point. */
  int slots[CMD_MAX_BYTES + 4];
  int n_slots = 0;
  for (int i = fill_lo; i <= ci && n_slots < (int)(sizeof(slots) / sizeof(slots[0])); i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int is_fill = 0;
    if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED))
    {
      const Sym *s;
      int64_t o;
      int w;
      if (cmd_store_addr(ir, q, &s, &o, &w) && CMD_OVL(s, o, w))
        is_fill = 1;
    }
    if (is_fill || i == p0 || i == p1 || i == p2 || i == ci)
      slots[n_slots++] = i;
  }
  if (n_slots < n_desc)
    return 0;

  /* Emit the descriptors into the chosen (last n_desc) slots; NOP the rest. */
  IROperand base_op = D;
  base_op.is_lval = 0;
  for (int si = 0; si < n_slots; si++)
  {
    int idx = slots[si];
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (si < n_slots - n_desc)
    {
      q->op = TCCIR_OP_NOP;
      continue;
    }
    int di = si - (n_slots - n_desc);
    int btype = desc[di].w == 4 ? IROP_BTYPE_INT32 : desc[di].w == 2 ? IROP_BTYPE_INT16 : IROP_BTYPE_INT8;
    tcc_ir_pool_ensure(ir, 4);
    int pb = ir->iroperand_pool_count;
    tcc_ir_pool_add(ir, base_op);
    tcc_ir_pool_add(ir, irop_make_imm32(-1, (int32_t)desc[di].val, btype));
    tcc_ir_pool_add(ir, irop_make_imm32(-1, desc[di].rel, IROP_BTYPE_INT32));
    tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
    q->op = TCCIR_OP_STORE_INDEXED;
    q->operand_base = pb;
  }

  /* Phase 5: NOP now-dead ASSIGN/LEA/ADD (buffer-address / value feeders). */
  for (int pass = 0; pass < 3; pass++)
  {
    int removed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ADD)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (d.is_lval)
        continue;
      int32_t dv = irop_get_vreg(d);
      if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int used = 0;
      for (int j = 0; j < n && !used; j++)
      {
        if (j == i)
          continue;
        IRQuadCompact *qj = &ir->compact_instructions[j];
        if (qj->op == TCCIR_OP_NOP)
          continue;
        for (int k = 1; k <= 3 && !used; k++)
        {
          IROperand op = (k == 1) ? tcc_ir_op_get_src1(ir, qj)
                         : (k == 2) ? tcc_ir_op_get_src2(ir, qj)
                                    : tcc_ir_op_get_accum(ir, qj);
          if (irop_get_vreg(op) == dv)
            used = 1;
        }
      }
      if (!used)
      {
        q->op = TCCIR_OP_NOP;
        removed = 1;
      }
    }
    if (!removed)
      break;
  }

#undef CMD_OVL
  return 1;
}

TCC_DBG_ENV_FLAG(bsm_no_const_memcpy, "TCC_NO_CONST_MEMCPY")

int tcc_ir_opt_const_memcpy_to_dest(TCCIRState *ir)
{
  if (bsm_no_const_memcpy())
    return 0;

  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;
  if (ir->captured_count > 0 || ir->has_static_chain)
    return 0;
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_VLA_ALLOC ||
        op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  int changes = 0;
  rse_build_def_map(ir);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
      continue;
    if (cmd_try_one(ir, i))
    {
      changes++;
      rse_build_def_map(ir); /* indices stable (only in-place NOP/reconfig) */
    }
  }
  rse_free_def_map();
  return changes;
}

int tcc_ir_opt_const_memcpy_to_dest_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_const_memcpy_to_dest(ctx->ir);
}
