/*
 *  TCC IR - SSA struct-copy forwarding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * ssa:copy_fwd -- read a struct where it was copied FROM, so the copy goes.
 *
 * The Zig C backend passes `self: Self` by value: `t19 = *t14; f(t19)`, and
 * the callee copies its parameter into a local again (`t0 = a0; t1 = &t0;`).
 * After inlining, one field read costs a chain of whole-struct copies, each
 * a frame object of its own -- RankedMutex.lock in the yasos kernel copied a
 * 584-byte process interface three times to read `waiting_for`, 1952 bytes of
 * frame on every syscall's stack:
 *
 *     memcpy(&A, p, 584)  memcpy(&B, &A, 584)  memcpy(&C, &B, 584)
 *     x = C.waiting_for
 *
 * After a copy `memcpy(&D, S, n)`, and as long as nothing may write D or what
 * S points to, the bytes of D are those of S.  Wherever that holds on every
 * path from the copy (an AVAILABLE-copies dataflow over the CFG -- Zig's
 * `if (t.error) ...` between a copy and its reads used to end a block-local
 * window), every read of D is redirected to S:
 *
 *   - a load of D+k reads S+k;
 *   - another copy taking D+k as its source takes S+k (which breaks chains:
 *     B above becomes a copy of p, and A has no reader left);
 *   - a struct argument passed by value from D+k is passed from S+k (the
 *     callee gets its own copy in the argument area either way).
 *
 * and when no other read of the copied bytes is left anywhere, the copy is
 * deleted (so is a copy nothing reads at all).  S is another frame object, a
 * global's address (read-only data nothing writes; Zig copies its constants
 * into locals) or a pointer value.  A load from
 * a pointer at k != 0 needs the address S+k: the deleted copy's own PARAM and
 * CALL instructions become those ADDs (they all follow S's definition), so
 * nothing is inserted and the SSA side tables only need a rebuild.  A read
 * whose shape this does not rewrite keeps the copy -- and the other reads may
 * still be rewritten.
 *
 * Soundness:
 *   - An ESCAPE of a frame object is an event: an instruction that uses an
 *     address inside it other than as a load/store base or a copy helper's
 *     destination/source.  Until one may have happened (no escape on any path
 *     from the entry; a loop carries one back to its head), only the recorded
 *     accesses name its bytes and a call cannot reach it.  From there on any
 *     call, unknown store or load, or inline asm may.  An object an inline asm
 *     operand names is escaped everywhere (those are lowered from saved
 *     SValues no IR operand shows).  So is a copy helper's result in use
 *     (memcpy returns D's address: an escape at the CALL), and an address
 *     derived from the object counts for it even where arithmetic steps past
 *     it (one past the end).  Zig builds a value and takes its address
 *     later (`t1 = &t0; method(t1)`), often in another live range of a reused
 *     local, so an escape anywhere in the function must not block a copy.
 *   - D must not have escaped where the copy runs.  A read of D is forwarded
 *     only where D has not escaped yet, and where the copy may REACH an
 *     escape or anything after one, an unseen reader may get its bytes: it
 *     stays.
 *   - The copy stops holding at any instruction that may write D or S: a
 *     store or copy into them, any write that may alias S when S is a pointer
 *     (an unknown pointer, an escaped frame object, an address-taken VAR) or
 *     a writable global, a call (unless S is a frame object not escaped by
 *     then, or const read-only data), a volatile read when S is not private,
 *     a redefinition of S's vreg, inline asm, and everything not known to
 *     leave memory alone.  A read is forwarded only where the copy is
 *     available on every path; any read the copy may REACH (some path, no write covering
 *     all of D since) and that is not forwarded keeps the copy.
 *   - A read at a call (a by-value argument, a copy's source) happens at the
 *     CALL, where the copy must be available -- and its PARAM, which then
 *     follows S's definition, must sit in the CALL's block.
 *   - A read moved to a frame S is a read of S from then on: a later copy
 *     into S must see it.  A deleted copy no longer writes D nor reads S.
 *   - In a function that touches volatile memory, only a copy the front end
 *     marked as between non-volatile objects (IROP_AUX_NONVOLATILE on its
 *     source operand, vstore.c) is forwarded: a volatile struct read through
 *     S would turn into fewer reads, at other times.
 */

#define USING_GLOBALS
#include "ir.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "ssa_opt.h"
#include <limits.h>

TCC_DBG_ENV_FLAG(cfw_dbg, "TCC_CFW_DBG")
#define CFW_DBG(...)                                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    if (cfw_dbg())                                                                                                     \
      fprintf(stderr, "copy_fwd: " __VA_ARGS__);                                                                       \
  } while (0)

enum
{
  CFA_LOAD,     /* LOAD whose src1 names the bytes (StackLoc or a TEMP deref) */
  CFA_LOAD_IDX, /* LOAD_INDEXED from a frame base (src1) at a constant index */
  CFA_COPY_SRC, /* a copy helper's source address: read at the CALL */
  CFA_BYVAL,    /* a struct argument passed by value: read at the CALL */
  CFA_READ,     /* any other read */
  CFA_WRITE,
  CFA_ESCAPE /* an address into the object used otherwise: [lo, hi) is the object */
};

typedef struct
{
  int lo, hi; /* frame bytes [lo, hi) */
  int base;   /* CFA_LOAD_IDX: the frame address its base operand holds */
  int at;     /* where the access happens (the CALL for an argument) */
  int insn;   /* the instruction holding the operand */
  int kind;
} CfwAcc;

typedef struct
{
  TCCIRState *ir;
  IRSSAOptCtx *ctx;
  int n;
  CfwAcc *acc;
  int nacc, cap;
  int *param_call;   /* FUNCPARAM* -> its call instruction, else -1 */
  int *block_start;  /* 1 at the first instruction of a basic block */
  int *block_of;     /* the basic block of each instruction */
  int nb;            /* basic blocks */
  int *bstart;       /* nb + 1: first instruction of each block, then n */
  int *pred_at, *pred; /* predecessors of b: pred[pred_at[b] .. pred_at[b + 1]) */
  uint8_t *reach;    /* 1 for a block reachable from the entry */
  int *copy_len;     /* copy-helper CALL -> constant length, else 0 */
  int *copy_align;   /* copy-helper CALL -> the alignment its name promises */
  int *sret_param;   /* CALL -> its PARAM0 when that is the struct-return buffer, else -1 */
  int *copy_param;   /* 3 per instruction: PARAM0..2 of a copy-helper CALL */
} Cfw;

static void cfw_add(Cfw *c, int lo, int hi, int at, int insn, int kind)
{
  if (c->nacc == c->cap)
  {
    c->cap = c->cap ? 2 * c->cap : 64;
    c->acc = tcc_realloc(c->acc, sizeof(CfwAcc) * c->cap);
  }
  c->acc[c->nacc++] = (CfwAcc){lo, hi, lo, at, insn, kind};
}

/* 0 if `call` is no copy helper, else the alignment of both pointers its
 * name promises: memmove4/8 are the frontend's word copies (vstore.c), which
 * codegen expands to LDM/STM. */
static int cfw_copy_helper_align(TCCIRState *ir, IRQuadCompact *call)
{
  static const char *const names[] = {"memcpy",          "memmove",         "__aeabi_memcpy",
                                      "__aeabi_memcpy4", "__aeabi_memcpy8", "__aeabi_memmove",
                                      "__aeabi_memmove4", "__aeabi_memmove8"};
  IROperand callee = tcc_ir_op_get_src1(ir, call);
  if (irop_get_tag(callee) != IROP_TAG_SYMREF)
    return 0;
  Sym *sym = irop_get_sym_ex(ir, callee);
  const char *name = sym ? get_tok_str(sym->v, NULL) : NULL;
  for (unsigned k = 0; name && k < sizeof names / sizeof names[0]; k++)
    if (!strcmp(name, names[k]))
    {
      const char last = name[strlen(name) - 1];
      return last == '8' ? 8 : last == '4' ? 4 : 1;
    }
  return 0;
}

/* A direct frame operand: no vreg, not a parameter's slot. */
static int cfw_direct_frame(IROperand op, int *off)
{
  if (irop_get_tag(op) != IROP_TAG_STACKOFF || irop_get_vreg(op) >= 0 || op.is_param || !op.is_local)
    return 0;
  *off = irop_get_stack_offset(op);
  return 1;
}

/* The frame address a TEMP holds: single-definition hops through ASSIGN/LEA
 * of a direct StackLoc address and ADD/SUB of a constant.  *root (if given)
 * gets the StackLoc the chain starts from: the object the address was derived
 * from, which arithmetic may step out of (one past the end). */
static int cfw_temp_frame_addr_root(Cfw *c, int32_t vr, int *off, int *root)
{
  TCCIRState *ir = c->ir;
  int acc = 0;
  for (int hop = 0; hop < 16; hop++)
  {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    IRSSAVregInfo *vi = ssa_opt_vinfo(c->ctx, vr);
    if (!vi || vi->def_instr < 0 || ssa_opt_def_total(vi) != 1)
      return 0;
    IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
    IROperand src = tcc_ir_op_get_src1(ir, dq);
    int base;
    const int direct = !src.is_lval && !src.is_llocal && cfw_direct_frame(src, &base);
    const int copy = irop_get_tag(src) == IROP_TAG_VREG && !src.is_lval && !src.is_local && !src.is_llocal;
    if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB)
    {
      if (tcc_ir_op_src2_tag(ir, dq) != IROP_TAG_IMM32 || tcc_ir_op_src2_is_lval(ir, dq) || tcc_ir_op_src2_is_sym(ir, dq))
        return 0;
      acc += dq->op == TCCIR_OP_ADD ? tcc_ir_op_src2_imm32(ir, dq) : -tcc_ir_op_src2_imm32(ir, dq);
    }
    else if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_LEA)
      return 0;
    if (direct)
    {
      *off = base + acc;
      if (root)
        *root = base;
      return 1;
    }
    if (!copy || dq->op == TCCIR_OP_LEA)
      return 0;
    vr = irop_get_vreg(src);
  }
  return 0;
}

static int cfw_temp_frame_addr(Cfw *c, int32_t vr, int *off)
{
  return cfw_temp_frame_addr_root(c, vr, off, NULL);
}

/* The frame address an address-valued operand names; *root as above. */
static int cfw_frame_addr_root(Cfw *c, IROperand op, int *off, int *root)
{
  if (op.is_lval || op.is_llocal)
    return 0;
  if (cfw_direct_frame(op, off))
  {
    if (root)
      *root = *off;
    return 1;
  }
  return irop_get_tag(op) == IROP_TAG_VREG && cfw_temp_frame_addr_root(c, irop_get_vreg(op), off, root);
}

static int cfw_frame_addr(Cfw *c, IROperand op, int *off)
{
  return cfw_frame_addr_root(c, op, off, NULL);
}

/* The frame bytes an lvalue operand reads or writes: 1 with *off, 0 when it
 * is not frame memory, -1 when it is memory this cannot place. */
static int cfw_frame_mem(Cfw *c, IROperand op, int *off)
{
  if (!op.is_lval && !op.is_llocal)
    return 0;
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_get_vreg(op) < 0 && !op.is_param && op.is_local)
  {
    *off = irop_get_stack_offset(op);
    return op.is_llocal ? -1 : 1;
  }
  if (irop_get_tag(op) == IROP_TAG_VREG && op.is_lval && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(op)) == TCCIR_VREG_TYPE_TEMP)
    return cfw_temp_frame_addr(c, irop_get_vreg(op), off) ? 1 : -1;
  return -1;
}

static int cfw_width(IROperand op)
{
  if (irop_get_btype(op) == IROP_BTYPE_STRUCT)
  {
    CType *t = irop_get_ctype(op);
    int align;
    return t ? type_size(t, &align) : -1;
  }
  return ir_opt_store_btype_size_bytes(irop_get_btype(op));
}

static void cfw_escape(Cfw *c, int off, int at)
{
  int lo, hi;
  if (!tcc_ir_frame_object_at(c->ir, off, &lo, &hi))
    lo = off, hi = off + 1;
  cfw_add(c, lo, hi, at, at, CFA_ESCAPE);
}

/* An address-valued operand escapes at `at`: the object it points into, and
 * the one it was derived from -- `(char *)&d + sizeof d` is d's too, though
 * it names the byte after it. */
static int cfw_escape_op(Cfw *c, IROperand op, int at)
{
  int off, root;
  if (!cfw_frame_addr_root(c, op, &off, &root))
    return 0;
  cfw_escape(c, off, at);
  int lo, hi;
  if (root != off && !(tcc_ir_frame_object_at(c->ir, off, &lo, &hi) && lo <= root && root < hi))
    cfw_escape(c, root, at);
  return 1;
}

/* An indexed access through a frame base at a constant index: the frame
 * address of the base, and the bytes [*lo, *hi) it touches. */
static int cfw_indexed_frame(Cfw *c, int i, int *base, int *lo, int *hi)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (q->op != TCCIR_OP_LOAD_INDEXED && q->op != TCCIR_OP_STORE_INDEXED)
    return 0;
  const int load = q->op == TCCIR_OP_LOAD_INDEXED;
  IROperand b = tcc_ir_op_get_dest_or_src1(ir, q, load);
  IROperand idx = tcc_ir_op_get_src2(ir, q), sc = tcc_ir_op_get_scale(ir, q);
  if (!cfw_frame_addr(c, b, base) || irop_get_tag(idx) != IROP_TAG_IMM32 || idx.is_lval || idx.is_sym ||
      irop_get_tag(sc) != IROP_TAG_IMM32)
    return 0;
  const int scale = irop_get_imm32(sc);
  const int w = ir_opt_store_btype_size_bytes(irop_get_btype(tcc_ir_op_get_dest_or_src1(ir, q, !load)));
  if (scale < 0 || scale > 3 || w <= 0)
    return 0;
  *lo = *base + (irop_get_imm32(idx) << scale);
  *hi = *lo + w;
  return 1;
}

/* BLOCK_COPY fills a frame slot from .rodata (a constant initializer, Zig's
 * 0xaa "undefined" fills): the bytes [*lo, *hi) it writes. */
static int cfw_block_copy_frame(Cfw *c, int i, int *lo, int *hi)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand dst = tcc_ir_op_get_dest(ir, q), sz = tcc_ir_op_get_src2(ir, q);
  if (!cfw_direct_frame(dst, lo) || irop_get_tag(sz) != IROP_TAG_IMM32 || irop_get_imm32(sz) <= 0)
    return 0;
  *hi = *lo + irop_get_imm32(sz);
  return 1;
}

/* memcpy returns its destination: a result in use carries D's address where
 * no operand shows it. */
static int cfw_result_unused(Cfw *c, int ci)
{
  IRQuadCompact *q = &c->ir->compact_instructions[ci];
  if (q->op != TCCIR_OP_FUNCCALLVAL)
    return 1;
  IROperand r = tcc_ir_op_get_dest(c->ir, q);
  if (irop_is_none(r))
    return 1;
  if (irop_get_tag(r) != IROP_TAG_VREG || r.is_lval || irop_get_vreg(r) < 0)
    return 0; /* stored somewhere: in use */
  IRSSAVregInfo *vi = ssa_opt_vinfo(c->ctx, irop_get_vreg(r));
  return vi && vi->use_count == 0;
}

/* Record what instruction i does to the frame. */
static void cfw_scan(Cfw *c, int i)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  const int op = q->op;
  IROperand ops[4];
  int nops = 0, off, w;
  if (irop_config[op].has_dest)
    ops[nops++] = tcc_ir_op_get_dest(ir, q);
  if (irop_config[op].has_src1)
    ops[nops++] = tcc_ir_op_get_src1(ir, q);
  if (irop_config[op].has_src2)
    ops[nops++] = tcc_ir_op_get_src2(ir, q);
  if (tcc_ir_op_is_mac(op) || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_SELECT)
  {
    ops[nops] = ir->iroperand_pool[q->operand_base + nops];
    nops++;
  }

  /* A copy helper's result is its destination's address, out in a register
   * from the call on (the copy's own write and source are its PARAMs). */
  if (op == TCCIR_OP_FUNCCALLVAL && c->copy_len[i] > 0 && c->copy_param[3 * i] >= 0 && !cfw_result_unused(c, i))
    cfw_escape_op(c, tcc_ir_op_get_src1(ir, &ir->compact_instructions[c->copy_param[3 * i]]), i);

  if (op == TCCIR_OP_FUNCPARAMVAL && c->param_call[i] >= 0)
  {
    const int call = c->param_call[i];
    IROperand a = tcc_ir_op_get_src1(ir, q);
    const int pidx = TCCIR_DECODE_PARAM_IDX((uint32_t)tcc_ir_op_src2_imm(ir, q));
    if (c->copy_len[call] > 0 && pidx <= 1 && cfw_frame_addr(c, a, &off))
    {
      cfw_add(c, off, off + c->copy_len[call], call, i, pidx == 0 ? CFA_WRITE : CFA_COPY_SRC);
      return;
    }
    if (irop_get_btype(a) == IROP_BTYPE_STRUCT && a.is_lval && cfw_direct_frame(a, &off) && (w = cfw_width(a)) > 0)
    {
      cfw_add(c, off, off + w, call, i, CFA_BYVAL);
      return;
    }
    /* The struct-return buffer: the callee writes the result there and, the
     * pointer being no C value, cannot keep it -- a write at the call. */
    if (pidx == 0 && c->sret_param[call] == i && cfw_frame_addr(c, a, &off))
    {
      const int id = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
      cfw_add(c, off, off + ir->sret_calls[id], call, i, CFA_WRITE);
      return;
    }
  }
  int ibase, ilo, ihi;
  if (op == TCCIR_OP_BLOCK_COPY)
  {
    if (cfw_block_copy_frame(c, i, &ilo, &ihi))
      cfw_add(c, ilo, ihi, i, i, CFA_WRITE);
    else
      cfw_escape_op(c, ops[0], i);
    return;
  }
  if (cfw_indexed_frame(c, i, &ibase, &ilo, &ihi))
  {
    if (op == TCCIR_OP_LOAD_INDEXED)
    {
      cfw_add(c, ilo, ihi, i, i, CFA_LOAD_IDX);
      c->acc[c->nacc - 1].base = ibase;
    }
    else
    {
      cfw_add(c, ilo, ihi, i, i, CFA_WRITE);
      /* The stored value may itself be a frame address. */
      cfw_escape_op(c, ops[1], i);
    }
    return;
  }
  /* Indexed by a register: anywhere in the object its base points into. */
  if (op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED)
    for (int k = 0; k < nops; k++)
      cfw_escape_op(c, ops[k], i);
  if (op == TCCIR_OP_LOAD)
  {
    IROperand s = ops[irop_config[op].has_dest];
    if (cfw_frame_mem(c, s, &off) == 1 && !s.is_llocal && (w = cfw_width(s)) > 0)
    {
      cfw_add(c, off, off + w, i, i, CFA_LOAD);
      return;
    }
  }
  /* Address arithmetic into the frame stays traceable as long as the result
   * is a TEMP: its own uses are classified where they appear. */
  const int addr_arith = (op == TCCIR_OP_ASSIGN || op == TCCIR_OP_LEA ||
                          ((op == TCCIR_OP_ADD || op == TCCIR_OP_SUB) && nops == 3 &&
                           irop_get_tag(ops[2]) == IROP_TAG_IMM32 && !ops[2].is_lval && !ops[2].is_sym)) &&
                         irop_config[op].has_dest && irop_get_tag(ops[0]) == IROP_TAG_VREG && !ops[0].is_lval &&
                         TCCIR_DECODE_VREG_TYPE(irop_get_vreg(ops[0])) == TCCIR_VREG_TYPE_TEMP;
  /* ... provided the TEMP itself resolves (one definition), and no phi
   * merges it with another value: those uses would be invisible here. */
  int traced = 0;
  if (addr_arith && cfw_temp_frame_addr(c, irop_get_vreg(ops[0]), &off))
  {
    IRSSAVregInfo *vi = ssa_opt_vinfo(c->ctx, irop_get_vreg(ops[0]));
    traced = 1;
    for (int u = 0; vi && u < vi->use_count; u++)
      if (vi->uses[u].kind == SSA_USE_PHI)
        traced = 0;
  }
  for (int k = 0; k < nops; k++)
  {
    IROperand o = ops[k];
    const int is_dest = irop_config[op].has_dest && k == 0;
    int r = cfw_frame_mem(c, o, &off);
    if (r == 1)
    {
      w = cfw_width(o);
      if (w <= 0 || o.is_llocal)
        w = 4;
      cfw_add(c, off, off + w, i, i, is_dest && !o.is_llocal ? CFA_WRITE : CFA_READ);
    }
    else if (r == 0 && !is_dest && !(traced && k == 1))
      cfw_escape_op(c, o, i);
  }
  /* A post-increment access walks wherever its base goes. */
  if (op == TCCIR_OP_LOAD_POSTINC || op == TCCIR_OP_STORE_POSTINC)
    for (int k = 0; k < nops; k++)
      cfw_escape_op(c, ops[k], i);
}


typedef struct
{
  int align;     /* pointer S: the alignment the copy's helper promised */
  int is_frame;  /* S is a frame object */
  int is_sym;    /* S is a symbol's address (val), at an addend */
  int rodata;    /* symbol S: in a read-only section, nothing writes it */
  const uint8_t *esc; /* frame S: may have escaped by each instruction (NULL: never) */
  int off;       /* frame S: its offset */
  IROperand val; /* pointer S: the operand holding it */
  int32_t vr;
} CfwSrc;

static int cfw_overlaps(const CfwAcc *a, int lo, int hi)
{
  return a->lo < hi && lo < a->hi;
}

static int cfw_escaped(Cfw *c, int lo, int hi)
{
  for (int k = 0; k < c->nacc; k++)
    if (c->acc[k].kind == CFA_ESCAPE && cfw_overlaps(&c->acc[k], lo, hi))
      return 1;
  return 0;
}

/* Could instruction j write D [dlo, dhi) or what S names? */
static int cfw_clobbers(Cfw *c, int j, int dlo, int dhi, const CfwSrc *src, int slo, int shi)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[j];
  const int op = q->op;
  /* A frame S whose address may be out by now aliases like a pointer; so
   * does a writable symbol.  Read-only data nothing writes. */
  const int ptr_like = src->is_sym ? !src->rodata : !src->is_frame || (src->esc && src->esc[j]);
  /* Only a pointer S can point into an escaped frame object. */
  const int ptr_frame = !src->is_frame && !src->is_sym;
  int off;
  switch (op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_ADD:
  case TCCIR_OP_ADC_USE:
  case TCCIR_OP_ADC_GEN:
  case TCCIR_OP_SUB:
  case TCCIR_OP_SUBC_USE:
  case TCCIR_OP_SUBC_GEN:
  case TCCIR_OP_MUL:
  case TCCIR_OP_MLA:
  case TCCIR_OP_UMULL:
  case TCCIR_OP_UMAAL:
  case TCCIR_OP_SMULL:
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
  case TCCIR_OP_SETIF:
  case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_BFI:
  case TCCIR_OP_ZEXT:
  case TCCIR_OP_PACK64:
  case TCCIR_OP_BOOL_OR:
  case TCCIR_OP_BOOL_AND:
  case TCCIR_OP_SELECT:
  case TCCIR_OP_CLZ:
  case TCCIR_OP_RBIT:
  case TCCIR_OP_REV:
  case TCCIR_OP_REV16:
  case TCCIR_OP_FADD:
  case TCCIR_OP_FSUB:
  case TCCIR_OP_FMUL:
  case TCCIR_OP_FDIV:
  case TCCIR_OP_FNEG:
  case TCCIR_OP_FCMP:
  case TCCIR_OP_CVT_FTOF:
  case TCCIR_OP_CVT_ITOF:
  case TCCIR_OP_CVT_FTOI:
  case TCCIR_OP_STORE:
  case TCCIR_OP_SWITCH_LOAD:
  case TCCIR_OP_ASM_OUTPUT: /* its destination: a vreg, or memory the dest check places */
    break;
  /* A volatile read is an event (a device or another core may act on it):
   * memory a pointer reaches, or a writable global, is not assumed to hold
   * across one. */
  case TCCIR_OP_LOAD:
  case TCCIR_OP_LOAD_INDEXED:
    if (ptr_like && tcc_ir_access_is_volatile(ir, tcc_ir_op_get_src1(ir, q)))
      return 1;
    break;
  /* Control flow and reads: the dataflow carries the copy across them. */
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_ASM_INPUT:
    return 0;
  /* An asm statement reaches the frame only through its operands, and an
   * object one of them names counts as escaped; through memory it may write
   * anything a pointer reaches. */
  case TCCIR_OP_INLINE_ASM:
    return ptr_like;
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCCALLVAL:
    if (c->copy_len[j] > 0 && c->copy_param[3 * j] >= 0)
    {
      IROperand dst = tcc_ir_op_get_src1(ir, &ir->compact_instructions[c->copy_param[3 * j]]);
      if (cfw_frame_addr(c, dst, &off))
      {
        const int end = off + c->copy_len[j];
        if (off < dhi && dlo < end)
          return 1;
        if (src->is_frame && off < shi && slo < end)
          return 1;
        return ptr_frame && cfw_escaped(c, off, end);
      }
    }
    /* Its struct-return buffer it writes; anything else only if reachable. */
    if (c->sret_param[j] >= 0 &&
        cfw_frame_addr(c, tcc_ir_op_get_src1(ir, &ir->compact_instructions[c->sret_param[j]]), &off))
    {
      const int id = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
      const int end = off + ir->sret_calls[id];
      if (off < dhi && dlo < end)
        return 1;
      if (src->is_frame && off < shi && slo < end)
        return 1;
    }
    /* A callee cannot reach a frame object whose address never escaped. */
    return ptr_like;
  case TCCIR_OP_BLOCK_COPY:
  {
    int lo, hi;
    if (!cfw_block_copy_frame(c, j, &lo, &hi))
      return 1;
    if (lo < dhi && dlo < hi)
      return 1;
    if (src->is_frame && lo < shi && slo < hi)
      return 1;
    return ptr_frame && cfw_escaped(c, lo, hi);
  }
  case TCCIR_OP_STORE_INDEXED:
  {
    int base, lo, hi;
    if (!cfw_indexed_frame(c, j, &base, &lo, &hi))
      return ptr_like; /* through a pointer, or anywhere in an escaped object */
    if (lo < dhi && dlo < hi)
      return 1;
    if (src->is_frame && lo < shi && slo < hi)
      return 1;
    return ptr_frame && cfw_escaped(c, lo, hi);
  }
  default:
    return 1;
  }
  if (!irop_config[op].has_dest)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (irop_dest_defines_vreg(d))
  {
    /* A variable's own slot: S's vreg redefined, or a home S may point to. */
    const int32_t vr = irop_get_vreg(d);
    if (vr == src->vr)
      return 1;
    if (!ptr_like || TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      return 0;
    IRLiveInterval *li = tcc_ir_get_live_interval(ir, vr);
    return !li || li->addrtaken;
  }
  int r = cfw_frame_mem(c, d, &off);
  if (r == 0)
    return 0;
  if (r < 0)
    return ptr_like;
  int w = cfw_width(d);
  const int end = off + (w > 0 ? w : 4);
  if (off < dhi && dlo < end)
    return 1;
  if (src->is_frame && off < shi && slo < end)
    return 1;
  return ptr_frame && cfw_escaped(c, off, end);
}

/* A symbol S's pool entry at `k` further. */
static uint32_t cfw_sym_at(TCCIRState *ir, const CfwSrc *src, int k)
{
  /* Copied out first: adding to the pool may move it. */
  IRPoolSymref sr = *irop_get_symref_ex(ir, src->val);
  return tcc_ir_pool_add_symref(ir, sr.sym, sr.addend + k, sr.flags);
}

/* Does the symbol an address operand names live in read-only data, its bytes
 * final?  A const object (writing one is undefined), not a weak or tentative
 * definition another may replace, in a section without SHF_WRITE.  The
 * section alone does not tell: a variable put in a named section shared with
 * code is writable at run time although the section is not SHF_WRITE. */
static int cfw_sym_rodata(TCCIRState *ir, IROperand op)
{
  IRPoolSymref *sr = irop_get_symref_ex(ir, op);
  Sym *sym = sr ? sr->sym : NULL;
  if (!sym || sym->a.tentative || sym->a.weak || (sym->type.t & VT_VOLATILE))
    return 0;
  const CType *t = &sym->type;
  for (int depth = 0; (t->t & VT_ARRAY) && t->ref && depth < 16; depth++)
    t = &t->ref->type;
  if (!(t->t & VT_CONSTANT) || (t->t & VT_VOLATILE))
    return 0;
  ElfSym *esym = elfsym(sym);
  if (!esym || esym->st_shndx == SHN_UNDEF || esym->st_shndx >= (unsigned)tcc_state->nb_sections ||
      ELFW(ST_BIND)(esym->st_info) == STB_WEAK)
    return 0;
  Section *sec = tcc_state->sections[esym->st_shndx];
  return sec && !(sec->sh_flags & SHF_WRITE);
}

/* An operand reading the bytes at `k` into S, of the shape of `orig`. */
static IROperand cfw_src_mem(TCCIRState *ir, const CfwSrc *src, IROperand orig, int k, int32_t addr_vr)
{
  IROperand r;
  if (src->is_sym)
    r = irop_make_symref(-1, cfw_sym_at(ir, src, k), 1, 0, 0, irop_get_btype(orig)); /* never a struct */
  else if (src->is_frame)
  {
    r = irop_make_stackoff(-1, src->off + k, 1, 0, 0, irop_get_btype(orig));
    if (irop_get_btype(orig) == IROP_BTYPE_STRUCT)
    {
      r.u.s.ctype_idx = orig.u.s.ctype_idx;
      r.u.s.aux_data = (int16_t)(src->off + k);
    }
  }
  else
  {
    r = irop_make_vreg(addr_vr, irop_get_btype(orig));
    r.is_lval = 1;
    if (irop_get_btype(orig) == IROP_BTYPE_STRUCT)
    {
      r.u.s.ctype_idx = orig.u.s.ctype_idx;
      r.u.s.aux_data = 0;
    }
  }
  r.is_unsigned = orig.is_unsigned;
  r.is_complex = orig.is_complex;
  /* Not ALIGN4_OK: S may be less aligned than the frame object was. */
  r.aux = orig.aux & ~IROP_AUX_ALIGN4_OK;
  return r;
}

static void cfw_set_operand(TCCIRState *ir, int insn, int slot, IROperand v)
{
  ir->iroperand_pool[ir->compact_instructions[insn].operand_base + slot] = v;
}

/* Largest power of two (<= 8) dividing a frame offset. */
static int cfw_frame_align(int off)
{
  int a = 8;
  while (a > 1 && (off & (a - 1)))
    a >>= 1;
  return a;
}

/* Is S+k aligned for a read that needs `req`?  The frame D was, so the read
 * may assume it (a word copy expands to LDM, which faults unaligned): the
 * kernel's MBR parse read a partition entry out of a byte buffer at +446 with
 * a plain memcpy, then copied it on with memmove8 -- forwarded, the memmove8
 * read the buffer with LDM. */
static int cfw_src_aligned(const CfwSrc *src, int k, int req)
{
  if (req <= 1)
    return 1;
  if (src->is_frame)
    return cfw_frame_align(src->off + k) >= req;
  return src->align >= req && k % req == 0;
}

/* Predecessor lists from the terminators.  An edge too many only makes the
 * copy less available; an edge missed would make it wrongly available, so a
 * switch whose table cannot be told apart sends edges to every table. */
static void cfw_add_edge(int *cnt, int **lists, int from, int to, int pass)
{
  if (pass == 0)
    cnt[to]++;
  else
    (*lists)[cnt[to]++] = from;
}

static void cfw_build_cfg(Cfw *c)
{
  TCCIRState *ir = c->ir;
  const int n = c->n, nb = c->block_of[n - 1] + 1;
  c->nb = nb;
  c->bstart = tcc_malloc(sizeof(int) * (nb + 1));
  for (int i = n - 1; i >= 0; i--)
    c->bstart[c->block_of[i]] = i;
  c->bstart[nb] = n;
  int *cnt = tcc_mallocz(sizeof(int) * (nb + 1));
  for (int pass = 0; pass < 2; pass++)
  {
    if (pass == 1)
    {
      c->pred_at = tcc_malloc(sizeof(int) * (nb + 1));
      int acc = 0;
      for (int b = 0; b < nb; b++)
      {
        c->pred_at[b] = acc;
        acc += cnt[b];
        cnt[b] = c->pred_at[b];
      }
      c->pred_at[nb] = acc;
      c->pred = tcc_malloc(sizeof(int) * (acc > 0 ? acc : 1));
    }
    for (int b = 0; b < nb; b++)
    {
      IRQuadCompact *q = &ir->compact_instructions[c->bstart[b + 1] - 1];
      int fall = b + 1 < nb;
      switch (q->op)
      {
      case TCCIR_OP_JUMP:
        fall = 0;
        /* fallthrough */
      case TCCIR_OP_JUMPIF:
      {
        int t = (int)tcc_ir_op_dest_imm(ir, q);
        if (t >= 0 && t < n)
          cfw_add_edge(cnt, &c->pred, b, c->block_of[t], pass);
        break;
      }
      case TCCIR_OP_SWITCH_TABLE:
      {
        int id = (int)tcc_ir_op_src2_imm(ir, q);
        fall = 1 && b + 1 < nb;
        for (int t = 0; t < ir->num_switch_tables; t++)
        {
          if (id >= 0 && id < ir->num_switch_tables && t != id)
            continue;
          TCCIRSwitchTable *st = &ir->switch_tables[t];
          if (st->default_target >= 0 && st->default_target < n)
            cfw_add_edge(cnt, &c->pred, b, c->block_of[st->default_target], pass);
          for (int j = 0; st->targets && j < st->num_entries; j++)
            if (st->targets[j] >= 0 && st->targets[j] < n)
              cfw_add_edge(cnt, &c->pred, b, c->block_of[st->targets[j]], pass);
        }
        break;
      }
      case TCCIR_OP_RETURNVOID:
      case TCCIR_OP_RETURNVALUE:
      case TCCIR_OP_TRAP:
        fall = 0;
        break;
      default:
        break;
      }
      if (fall)
        cfw_add_edge(cnt, &c->pred, b, b + 1, pass);
    }
  }
  tcc_free(cnt);
  /* Unreachable code takes no part: an unreachable cycle would otherwise keep
   * the optimistic start of the AVAILABLE intersection. */
  c->reach = tcc_mallocz(nb > 0 ? nb : 1);
  if (nb > 0)
    c->reach[0] = 1;
  for (int changed = 1; changed;)
  {
    changed = 0;
    for (int b = 1; b < nb; b++)
      for (int p = c->pred_at[b]; !c->reach[b] && p < c->pred_at[b + 1]; p++)
        if (c->reach[c->pred[p]])
          c->reach[b] = 1, changed = 1;
  }
}

/* Where the address of the object [lo, hi) may be out: 1 at each instruction
 * that an escape of it reaches on some path from the entry, or that is one --
 * from there on a call, an unknown store or load, or inline asm may touch its
 * bytes.  Before (where this is 0) only the recorded accesses name them.  An
 * escape is never undone: a loop carries it back to its head.  NULL when the
 * object never escapes. */
static uint8_t *cfw_esc_map(Cfw *c, int lo, int hi)
{
  const int n = c->n, nb = c->nb;
  int any = 0;
  for (int k = 0; k < c->nacc; k++)
    if (c->acc[k].kind == CFA_ESCAPE && cfw_overlaps(&c->acc[k], lo, hi))
    {
      any = 1;
      if (c->acc[k].at < 0)
        any = 2; /* named where no instruction shows it: everywhere */
    }
  if (!any)
    return NULL;
  uint8_t *m = tcc_malloc(n);
  if (any == 2)
  {
    memset(m, 1, n);
    return m;
  }
  memset(m, 0, n);
  for (int k = 0; k < c->nacc; k++)
    if (c->acc[k].kind == CFA_ESCAPE && cfw_overlaps(&c->acc[k], lo, hi))
      m[c->acc[k].at] = 1;
  uint8_t *gen = tcc_mallocz(nb), *in = tcc_mallocz(nb), *out = tcc_mallocz(nb);
  for (int b = 0; b < nb; b++)
    for (int i = c->bstart[b]; i < c->bstart[b + 1]; i++)
      gen[b] |= m[i];
  for (int changed = 1; changed;)
  {
    changed = 0;
    for (int b = 0; b < nb; b++)
    {
      int e = 0;
      for (int p = c->pred_at[b]; p < c->pred_at[b + 1]; p++)
        e |= out[c->pred[p]];
      const int o = e | gen[b];
      if (in[b] != e || out[b] != o)
        changed = 1;
      in[b] = e, out[b] = o;
    }
  }
  for (int b = 0; b < nb; b++)
    for (int i = c->bstart[b], e = in[b]; i < c->bstart[b + 1]; i++)
    {
      e |= m[i];
      m[i] = e;
    }
  tcc_free(gen);
  tcc_free(in);
  tcc_free(out);
  return m;
}

#define CFW_MAX_READS 64
#define CFW_AF_UNKNOWN (-2)

/* Could any instruction of block b (which does not hold the copy) write D
 * [d, d + len) or what S names? */
static int cfw_block_clobbers(Cfw *c, int b, int d, int len, const CfwSrc *src)
{
  for (int i = c->bstart[b]; i < c->bstart[b + 1]; i++)
    if (c->ir->compact_instructions[i].op != TCCIR_OP_NOP &&
        cfw_clobbers(c, i, d, d + len, src, src->off, src->off + len))
      return 1;
  return 0;
}

/* Try the copy at CALL `ci`.  Returns the number of rewrites. */
static int cfw_copy(Cfw *c, int ci)
{
  TCCIRState *ir = c->ir;
  const int len = c->copy_len[ci];
  const int p0 = c->copy_param[3 * ci], p1 = c->copy_param[3 * ci + 1], p2 = c->copy_param[3 * ci + 2];
  if (len <= 0 || p0 < 0 || p1 < 0 || p2 < 0 || !cfw_result_unused(c, ci))
    return 0;
  int d, dlo, dhi;
  if (!cfw_frame_addr(c, tcc_ir_op_get_src1(ir, &ir->compact_instructions[p0]), &d) ||
      !tcc_ir_frame_object_at(ir, d, &dlo, &dhi) || d + len > dhi)
    return 0;

  CfwSrc src = {0};
  IROperand sop = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p1]);
  int slo = 0, shi = 0;
  src.vr = -1;
  /* The front end marks a copy between non-volatile objects (vstore). */
  if (ir->func_has_volatile_access && !(sop.aux & IROP_AUX_NONVOLATILE))
    return 0;
  if (cfw_frame_addr(c, sop, &src.off))
  {
    if (!tcc_ir_frame_object_at(ir, src.off, &slo, &shi) || src.off + len > shi)
      return 0;
    if (src.off < d + len && d < src.off + len)
      return 0;
    src.is_frame = 1;
  }
  else if (irop_get_tag(sop) == IROP_TAG_VREG && !sop.is_lval && !sop.is_local && !sop.is_llocal &&
           irop_get_vreg(sop) >= 0 && irop_get_btype(sop) == IROP_BTYPE_INT32)
  {
    src.val = sop;
    src.vr = irop_get_vreg(sop);
    src.align = c->copy_align[ci];
  }
  else if (irop_get_tag(sop) == IROP_TAG_SYMREF && !sop.is_lval && !sop.is_local && !sop.is_llocal &&
           irop_get_vreg(sop) < 0 && irop_get_btype(sop) != IROP_BTYPE_STRUCT && irop_get_symref_ex(ir, sop) &&
           irop_get_symref_ex(ir, sop)->sym && !(irop_get_symref_ex(ir, sop)->sym->type.t & VT_VOLATILE))
  {
    /* A global: a read of it costs nothing more than a read of D. */
    src.is_sym = 1;
    src.val = sop;
    src.align = c->copy_align[ci];
    src.rodata = cfw_sym_rodata(ir, sop);
  }
  else
    return 0;

  /* Escapes are events: where D's address may be out, a call or an unknown
   * access may read or write it.  One that may precede the copy leaves nothing
   * to prove; one the copy reaches keeps it (below). */
  int changes = 0;
  uint8_t *desc = cfw_esc_map(c, dlo, dhi), *sesc = NULL;
  uint8_t *cover = NULL, *ain = NULL, *aout = NULL, *rin = NULL, *rout = NULL;
  int8_t *af = NULL, *rf = NULL;
  if (desc && desc[ci])
    goto done;
  if (src.is_frame)
    src.esc = sesc = cfw_esc_map(c, slo, shi);

  /* Where the copy holds, over the whole CFG.  AVAILABLE before an instruction:
   * on every path to it the copy ran with nothing since that may write D or
   * S -- D's bytes are S's there.  REACHES: on some path the copy ran with no
   * write covering all of D since -- a read there may see the copy's bytes.
   * A block is straight-line code, so each one transfers both facts as
   * identity, "set" (the copy is in it, last) or "cleared". */
  const int nb = c->nb, cb = c->block_of[ci];
  cover = tcc_mallocz(c->n);
  af = tcc_malloc(nb), rf = tcc_malloc(nb);
  ain = tcc_mallocz(nb), aout = tcc_malloc(nb), rin = tcc_mallocz(nb), rout = tcc_mallocz(nb);
  memset(af, CFW_AF_UNKNOWN, nb);
  memset(rf, -1, nb);
  /* "Cleared" for REACHES: a write covering all of D (in the copy's own
   * block, only one after it). */
  for (int a = 0; a < c->nacc; a++)
    if (c->acc[a].kind == CFA_WRITE && c->acc[a].at != ci && c->acc[a].lo <= d && c->acc[a].hi >= d + len)
    {
      const int at = c->acc[a].at;
      cover[at] = 1;
      if (ir->compact_instructions[at].op != TCCIR_OP_NOP && (c->block_of[at] != cb || at > ci))
        rf[c->block_of[at]] = 0;
    }
  if (rf[cb] < 0)
    rf[cb] = 1;
  /* The copy's own block sets AVAILABLE unless something after the copy
   * may write D or S. */
  af[cb] = 1;
  for (int i = ci + 1; i < c->bstart[cb + 1]; i++)
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP && cfw_clobbers(c, i, d, d + len, &src, src.off, src.off + len))
    {
      af[cb] = 0;
      break;
    }
  for (int b = 0; b < nb; b++)
    aout[b] = af[b] != 0 && c->reach[b]; /* optimistic start for the intersection */
  for (int changed = 1, guard = 0; changed && guard < 4 * nb + 8; guard++)
  {
    changed = 0;
    for (int b = 0; b < nb; b++)
    {
      int a = b > 0 && c->reach[b] && c->pred_at[b + 1] > c->pred_at[b], r = 0;
      for (int p = c->pred_at[b]; p < c->pred_at[b + 1]; p++)
      {
        a &= aout[c->pred[p]];
        r |= rout[c->pred[p]];
      }
      /* Whether another block clears AVAILABLE matters only where it may be
       * available on entry: identity and "cleared" both give 0 from 0.  Most
       * blocks are never looked at (the whole-function scan per copy was
       * quadratic).  The optimistic start above takes an unknown block as
       * identity, which is no lower than the greatest fixpoint either. */
      if (a && af[b] == CFW_AF_UNKNOWN)
        af[b] = cfw_block_clobbers(c, b, d, len, &src) ? 0 : -1;
      const int ao = af[b] < 0 ? a : af[b] && c->reach[b], ro = rf[b] < 0 ? r : rf[b];
      if (ain[b] != a || aout[b] != ao || rin[b] != r || rout[b] != ro)
        changed = 1;
      ain[b] = a, aout[b] = ao, rin[b] = r, rout[b] = ro;
    }
  }

  /* The reads of the copied bytes: every one this copy may reach must be
   * forwarded for the copy to go. */
  int reads[CFW_MAX_READS], nreads = 0, kept = 0;
  for (int a = 0; a < c->nacc; a++)
  {
    CfwAcc *x = &c->acc[a];
    if (x->kind == CFA_WRITE || x->kind == CFA_ESCAPE || !cfw_overlaps(x, d, d + len))
      continue;
    const int b = c->block_of[x->at];
    int av = ain[b], re = rin[b];
    for (int i = c->bstart[b]; i < x->at; i++)
    {
      if (i == ci)
      {
        av = re = 1;
        continue;
      }
      if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
        continue;
      if (av && cfw_clobbers(c, i, d, d + len, &src, src.off, src.off + len))
        av = 0;
      if (cover[i])
        re = 0;
    }
    if (!re)
      continue; /* reads what another write put there */
    /* The operand moves to S, which must be defined there: the instruction
     * holding it (a call's PARAM) sits after the copy when they share a block,
     * and in the block of the call. */
    const int where_ok =
        x->at != ci && c->block_of[x->insn] == b && (c->block_of[ci] != b || x->insn > ci || ci > x->at);
    /* An indexed load's base must point into the copied bytes too, so that
     * it points into S's after the rewrite. */
    const int base_out = x->kind == CFA_LOAD_IDX && (x->base < d || x->base >= d + len);
    /* Where D's address may be out, an unknown store may have changed it. */
    const int escaped = desc && desc[x->at];
    if (!av || escaped || !where_ok || x->kind == CFA_READ || base_out || x->lo < d || x->hi > d + len ||
        nreads == CFW_MAX_READS)
    {
      kept = 1;
      continue;
    }
    reads[nreads++] = a;
  }
  /* Where the copy's bytes may still be in D and its address may be out --
   * the escape itself, or anything after it -- an unseen reader may get them:
   * the copy stays.  (Its known reads may still go to S.) */
  for (int b = 0; desc && !kept && b < nb; b++)
    for (int i = c->bstart[b], re = rin[b]; i < c->bstart[b + 1]; i++)
    {
      if (re && desc[i])
      {
        kept = 1;
        break;
      }
      if (i == ci)
        re = 1;
      else if (cover[i])
        re = 0;
    }
  if (!nreads && kept)
    goto done;

  /* A pointer source at k != 0 needs S+k in a register: one ADD each, in the
   * copy's own instructions after PARAM1 (where S is live), in its block. */
  int slots[4], nslots = 0, adds_k[4], adds_vr[4], nadds = 0;
  const int cand[4] = {p0, p1, p2, ci};
  for (int s = 0; s < 4; s++)
    if (cand[s] >= p1 && c->block_of[cand[s]] == c->block_of[ci])
      slots[nslots++] = cand[s];
  int fwd[CFW_MAX_READS];
  for (int r = 0; r < nreads; r++)
  {
    CfwAcc *x = &c->acc[reads[r]];
    IROperand o = tcc_ir_op_get_src1(ir, &ir->compact_instructions[x->insn]);
    const int k = (x->kind == CFA_LOAD_IDX ? x->base : x->lo) - d;
    fwd[r] = 0;
    if ((x->kind == CFA_LOAD || x->kind == CFA_LOAD_IDX) && tcc_ir_access_is_volatile(ir, o))
    {
      kept = 1;
      continue;
    }
    /* What the read may assume about its address: a copy its helper's
     * alignment, a by-value struct a word (LDM), a load its width. */
    int req = x->hi - x->lo >= 4 ? 4 : x->hi - x->lo;
    if (x->kind == CFA_COPY_SRC)
      req = c->copy_align[x->at];
    else if (x->kind == CFA_BYVAL)
      req = 4;
    else if ((x->kind == CFA_LOAD || x->kind == CFA_LOAD_IDX) && (o.aux & IROP_AUX_UNDERALIGN))
      req = 1;
    if (!cfw_src_aligned(&src, x->lo - d, req) || !cfw_src_aligned(&src, k, x->kind == CFA_LOAD_IDX ? 1 : req))
    {
      kept = 1;
      continue;
    }
    if (src.is_sym)
    {
      /* An addend, no ADD.  An indexed load (a field past the immediate
       * range: `T6 <-- &t LOAD_INDEXED #412`) becomes a plain one; a struct
       * operand would need a shape a symbol does not take. */
      const int lbt = irop_get_btype(
          x->kind == CFA_LOAD_IDX ? tcc_ir_op_get_dest(ir, &ir->compact_instructions[x->insn]) : o);
      if (x->kind == CFA_COPY_SRC || ((x->kind == CFA_LOAD || x->kind == CFA_LOAD_IDX) && lbt != IROP_BTYPE_STRUCT))
        fwd[r] = 1;
      else
        kept = 1;
      continue;
    }
    if (src.is_frame)
    {
      /* A struct operand keeps its offset in 16 bits. */
      if (irop_get_btype(o) == IROP_BTYPE_STRUCT && (src.off + k < -32768 || src.off + k > 32767))
      {
        kept = 1;
        continue;
      }
      fwd[r] = 1;
      continue;
    }
    if (k == 0)
    {
      fwd[r] = 1;
      continue;
    }
    int a = 0;
    while (a < nadds && adds_k[a] != k)
      a++;
    if (a == nadds && nadds == nslots)
    {
      kept = 1;
      continue;
    }
    if (a == nadds)
      adds_k[nadds++] = k;
    fwd[r] = 1;
  }
  /* A copy that stays keeps its source alive anyway: then only break chains
   * (copies reading D), which is free; loads and arguments through a pointer
   * would just lengthen S's live range. */
  if (kept)
  {
    nadds = 0;
    for (int r = 0; r < nreads; r++)
      if (fwd[r] && (c->acc[reads[r]].kind != CFA_COPY_SRC ||
                     (!src.is_frame && !src.is_sym && c->acc[reads[r]].lo != d)))
        fwd[r] = 0;
  }
  int any = !kept; /* a copy nothing reads goes too */
  for (int r = 0; r < nreads; r++)
    any |= fwd[r];
  if (!any)
    goto done;
  CFW_DBG("%s: copy at %d len %d dst %d src %s%d: %d reads, kept=%d, adds=%d\n", funcname ? funcname : "?", ci, len, d,
          src.is_frame ? "frame " : src.is_sym ? "symbol +" : "vreg ",
          src.is_frame ? src.off : src.is_sym ? (int)irop_get_symref_ex(ir, src.val)->addend : (int)src.vr,
          nreads, kept, nadds);

  if (!kept)
  {
    /* The copy goes: its instructions host the ADDs.  Its write no longer
     * happens, so it can no longer cover another copy's reads either, and it
     * no longer reads S. */
    ir_opt_nop_call_params(ir, ci);
    ir->compact_instructions[ci].op = TCCIR_OP_NOP;
    for (int a = 0; a < c->nacc; a++)
      if (c->acc[a].at == ci && (c->acc[a].kind == CFA_WRITE || c->acc[a].kind == CFA_COPY_SRC))
        c->acc[a].kind = CFA_READ, c->acc[a].lo = c->acc[a].hi = 0;
    for (int a = 0; a < nadds; a++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[slots[a]];
      adds_vr[a] = tcc_ir_vreg_alloc_temp(ir);
      int base = tcc_ir_pool_add(ir, irop_make_vreg(adds_vr[a], IROP_BTYPE_INT32));
      tcc_ir_pool_add(ir, src.val);
      tcc_ir_pool_add(ir, irop_make_imm32(-1, adds_k[a], IROP_BTYPE_INT32));
      sq->operand_base = base;
      sq->op = TCCIR_OP_ADD;
    }
    changes++;
  }
  for (int r = 0; r < nreads; r++)
  {
    if (!fwd[r])
      continue;
    CfwAcc *x = &c->acc[reads[r]];
    const int k = (x->kind == CFA_LOAD_IDX ? x->base : x->lo) - d;
    int32_t addr_vr = src.vr;
    for (int a = 0; a < nadds; a++)
      if (adds_k[a] == k && k != 0)
        addr_vr = adds_vr[a];
    IROperand o = tcc_ir_op_get_src1(ir, &ir->compact_instructions[x->insn]);
    IROperand v;
    if (src.is_sym && x->kind == CFA_LOAD_IDX)
    {
      /* A plain load of the bytes it read, at the symbol's address. */
      IROperand dst = tcc_ir_op_get_dest(ir, &ir->compact_instructions[x->insn]);
      v = irop_make_symref(-1, cfw_sym_at(ir, &src, x->lo - d), 1, 0, 0, irop_get_btype(dst));
      v.is_unsigned = dst.is_unsigned;
      irop_carry_access_marks(&v, o);
      ir->compact_instructions[x->insn].op = TCCIR_OP_LOAD;
      tcc_ir_set_dest(ir, x->insn, dst);
      tcc_ir_set_src2_none(ir, x->insn);
    }
    else if (x->kind == CFA_COPY_SRC || x->kind == CFA_LOAD_IDX)
    {
      if (src.is_frame)
        v = irop_make_stackoff(-1, src.off + k, 0, 0, 0, IROP_BTYPE_INT32);
      else if (src.is_sym)
      {
        v = src.val;
        v.u.pool_idx = cfw_sym_at(ir, &src, k);
      }
      else if (addr_vr == src.vr)
        v = src.val;
      else
        v = irop_make_vreg(addr_vr, IROP_BTYPE_INT32);
      /* An indexed load's access marks live on its base; S may be less
       * aligned than the frame object (no LDRD through it). */
      if (x->kind == CFA_LOAD_IDX)
        v.aux = (o.aux & ~IROP_AUX_ALIGN4_OK) | (src.is_frame ? 0 : IROP_AUX_UNDERALIGN);
    }
    else
      v = cfw_src_mem(ir, &src, o, k, addr_vr);
    cfw_set_operand(ir, x->insn, irop_config[ir->compact_instructions[x->insn].op].has_dest ? 1 : 0, v);
    if (src.is_frame)
    {
      /* It reads S now: a later copy into S must see it. */
      const int delta = src.off - d;
      x->lo += delta, x->hi += delta, x->base += delta;
    }
    else
    {
      /* Through a pointer or a symbol: no frame bytes this pass tracks. */
      x->kind = CFA_READ;
      x->lo = x->hi = 0;
    }
    changes++;
  }
done:
  tcc_free(desc);
  tcc_free(sesc);
  tcc_free(cover);
  tcc_free(af);
  tcc_free(rf);
  tcc_free(ain);
  tcc_free(aout);
  tcc_free(rin);
  tcc_free(rout);
  return changes;
}

int ssa_opt_copy_fwd(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  const int n = ir->next_instruction_index;
  if (n == 0 || ir->has_static_chain || ir->captured_count > 0 || tcc_state->nb_nested_funcs > 0 ||
      tcc_ir_calls_returns_twice(ir))
    return 0;

  Cfw c = {0};
  c.ir = ir;
  c.ctx = ctx;
  c.n = n;
  c.param_call = tcc_malloc(sizeof(int) * n);
  c.block_start = tcc_mallocz(sizeof(int) * (n + 1));
  c.copy_len = tcc_mallocz(sizeof(int) * n);
  c.copy_align = tcc_mallocz(sizeof(int) * n);
  c.sret_param = tcc_malloc(sizeof(int) * n);
  c.copy_param = tcc_malloc(sizeof(int) * 3 * n);

  /* Calls: each PARAM's call, and the copy helpers' PARAMs and lengths. */
  int max_id = 0, ncopies = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    c.param_call[i] = -1;
    c.sret_param[i] = -1;
    c.copy_param[3 * i] = c.copy_param[3 * i + 1] = c.copy_param[3 * i + 2] = -1;
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      int id = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
      if (id > max_id)
        max_id = id;
    }
    if (q->op == TCCIR_OP_IJUMP)
      goto out;
  }
  int *call_at = tcc_malloc(sizeof(int) * (max_id + 1));
  for (int k = 0; k <= max_id; k++)
    call_at[k] = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
      call_at[TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q))] = i;
  }
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;
    uint32_t enc = (uint32_t)tcc_ir_op_src2_imm(ir, q);
    int id = TCCIR_DECODE_CALL_ID(enc), pidx = TCCIR_DECODE_PARAM_IDX(enc);
    int call = id <= max_id ? call_at[id] : -1;
    if (call < i)
      continue;
    c.param_call[i] = call;
    if (q->op == TCCIR_OP_FUNCPARAMVAL && pidx < 3)
      c.copy_param[3 * call + pidx] = i;
    if (q->op == TCCIR_OP_FUNCPARAMVAL && pidx == 0 && id < ir->sret_calls_size && ir->sret_calls[id] > 0)
      c.sret_param[call] = i;
  }
  tcc_free(call_at);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if ((q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL) && c.copy_param[3 * i + 2] >= 0 &&
        (c.copy_align[i] = cfw_copy_helper_align(ir, q)) > 0)
    {
      IROperand l = tcc_ir_op_get_src1(ir, &ir->compact_instructions[c.copy_param[3 * i + 2]]);
      if (irop_get_tag(l) == IROP_TAG_IMM32 && !l.is_lval && !l.is_sym && irop_get_imm32(l) > 0)
      {
        c.copy_len[i] = irop_get_imm32(l);
        ncopies++;
      }
    }
  }
  if (!ncopies)
    goto out;

  /* Basic blocks. */
  c.block_start[0] = 1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t >= 0 && t < n)
        c.block_start[t] = 1;
    }
      /* fallthrough */
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_TRAP:
      c.block_start[i + 1] = 1;
      break;
    default:
      break;
    }
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *st = &ir->switch_tables[t];
    if (st->default_target >= 0 && st->default_target < n)
      c.block_start[st->default_target] = 1;
    for (int j = 0; st->targets && j < st->num_entries; j++)
      if (st->targets[j] >= 0 && st->targets[j] < n)
        c.block_start[st->targets[j]] = 1;
  }

  c.block_of = tcc_malloc(sizeof(int) * n);
  for (int i = 0, b = -1; i < n; i++)
    c.block_of[i] = b += c.block_start[i];
  cfw_build_cfg(&c);
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      cfw_scan(&c, i);
  /* Inline asm names some objects only in the SValues saved with it: unseen
   * reads and writes, so those objects count as escaped. */
  {
    int off;
    for (int k = 0; tcc_ir_asm_frame_ref(ir, k, &off); k++)
      cfw_escape(&c, off, -1); /* no instruction: escaped everywhere */
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
    if (c.copy_len[i] > 0 && ir->compact_instructions[i].op != TCCIR_OP_NOP)
      changes += cfw_copy(&c, i);
  if (changes)
    tcc_ir_ssa_opt_rebuild(ctx);
  tcc_free(c.acc);
  tcc_free(c.block_of);
  tcc_free(c.bstart);
  tcc_free(c.pred_at);
  tcc_free(c.pred);
  tcc_free(c.reach);
  tcc_free(c.param_call);
  tcc_free(c.block_start);
  tcc_free(c.copy_len);
  tcc_free(c.copy_align);
  tcc_free(c.sret_param);
  tcc_free(c.copy_param);
  return changes;

out:
  tcc_free(c.param_call);
  tcc_free(c.block_start);
  tcc_free(c.copy_len);
  tcc_free(c.copy_align);
  tcc_free(c.sret_param);
  tcc_free(c.copy_param);
  return 0;
}
