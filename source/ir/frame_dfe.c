/*
 *  TCC IR - Dead frame bytes: drop the writes no read can see, then let the
 *  frame keep only the bytes something still names
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * The Zig C backend moves big values by value: an optional FileInfo (296 bytes)
 * is filled from a call's struct result or from a 0xaa "undefined" image, copied
 * into the optional, and of all that only `is_null` and the one-byte `kind` are
 * ever read.  Every copy and image write still lands in a frame object of the
 * full size, and frame relayout can only place or drop whole objects.
 *
 * tcc_ir_frame_dead_bytes runs just before the register allocator (after the
 * last pass that adds or drops a frame reference, before the call prefix and
 * the live intervals are built), on the final flat IR:
 *
 *   1. Every frame object the front end recorded (ir->frame_objs) is a
 *      candidate unless something may reach its bytes in a way this pass does
 *      not follow: its address stored, compared, offset, passed to any call
 *      but the copy/fill helpers or as a struct-result buffer, named by inline
 *      asm, or any access to it volatile.  Such an object ESCAPES: all of its
 *      bytes are read.
 *   2. Byte liveness, flow-insensitive: a byte is live if any instruction may
 *      read it -- a load, an operand, a by-value struct argument (to the end of
 *      the object), or a copy whose destination byte is live (to a fixpoint;
 *      a copy into anything but a candidate reads its whole source range).
 *   3. Writes into dead bytes go: a store, a .rodata BLOCK_COPY, a memset, or
 *      the copy itself.  A copy, fill or image write that still has live bytes
 *      shrinks to them (its start moved by a multiple of 8, so every helper's
 *      alignment promise holds), when both of its addresses can be moved.
 *      A struct-result buffer is written by the callee, whatever is read.
 *   4. A candidate's frame record is cut into PIECES: the byte ranges the
 *      remaining references reach, widened to 8-byte boundaries of the object
 *      and merged where they overlap, plus the gaps between them as records
 *      nobody names -- frame relayout then drops those and places each piece
 *      on its own.  No reference crosses a piece boundary, so a piece moved by
 *      a multiple of its start's alignment keeps every address in it valid.
 *
 * Deleting only never-read bytes needs no ordering argument: whatever path the
 * program takes, no instruction can observe them.
 */

#define USING_GLOBALS
#include "ir.h"
#include "opt_utils.h"

/* TCC_DFE_DBG: one line per function that changed. */
TCC_DBG_ENV_FLAG(dfe_dbg, "TCC_DFE_DBG")
/* TCC_DFE_ESC_DBG: every object left alone, with the check that refused it. */
TCC_DBG_ENV_FLAG(dfe_esc_dbg, "TCC_DFE_ESC_DBG")
/* TCC_DFE_FWD_OFF: no image forwarding (A/B). */
TCC_DBG_ENV_FLAG(dfe_fwd_off, "TCC_DFE_FWD_OFF")
/* TCC_DFE_LOWER_OFF: no copies replaced by word moves (A/B). */
TCC_DBG_ENV_FLAG(dfe_lower_off, "TCC_DFE_LOWER_OFF")

#define DFE_MAX_SPAN 65536 /* objects bigger than this are left alone */

enum
{
  DFE_READ,  /* the bytes are read: they stay live */
  DFE_STORE, /* a STORE into the bytes, deletable */
  DFE_BC,    /* a BLOCK_COPY (.rodata image) into the bytes, deletable/shrinkable */
  DFE_SET,   /* a memset family call, deletable/shrinkable */
  DFE_COPY,  /* the destination of a copy helper call, deletable/shrinkable */
  DFE_CSRC,  /* the source of a copy helper call: read where its destination is live */
  DFE_FIXED  /* written by a callee (struct result): kept, never makes anything live */
};

typedef struct
{
  int start, span; /* frame extent (record start and span, padding included) */
  int rec;         /* index into ir->frame_objs */
  uint8_t escaped;
  uint8_t escaped0; /* left alone whatever the references say */
  int esc_why;     /* TCC_DFE_ESC_DBG: the line of the first escape */
  int esc_at;      /* TCC_DFE_ESC_DBG: and its instruction */
  uint8_t *live;   /* span bytes */
} DfeObj;

typedef struct
{
  int kind, obj, lo, hi; /* object-relative [lo, hi) */
  int i;                 /* the instruction (the call for DFE_SET/COPY/CSRC) */
  int copy;              /* DFE_COPY/CSRC: index into the copy table */
  int pool;              /* DFE_STORE/FIXED: the operand naming the frame place directly, or -1 */
  int temp;              /* DFE_FIXED: the TEMP carrying the address, or -1 */
  int base;              /* reached through a pointer derived from this offset (-1: direct) */
  uint8_t dead;          /* deleted */
} DfeAcc;

/* A copy/fill helper call or a BLOCK_COPY. */
typedef struct
{
  int call;        /* the CALL (or the BLOCK_COPY) instruction */
  int kind;        /* DFE_COPY, DFE_SET or DFE_BC */
  int n;           /* bytes */
  int dobj, doff;  /* destination: candidate object and offset in it, or dobj -1 */
  int dpool;       /* pool index of the operand naming the destination address */
  int sobj, soff;  /* source in a frame object, or sobj -1 */
  int spool;       /* pool index of the operand naming the source, or -1 */
  int ssym;        /* the source is a SYMREF operand (spool names it) */
  int npool;       /* pool index of the byte count operand (not for DFE_BC) */
  int dtemp, stemp; /* the TEMP carrying the address, or -1 */
  int dbase, sbase; /* the offset that TEMP was derived from, or -1 */
  uint8_t keep;      /* either side may be volatile: never deleted, shrunk or forwarded */
  uint8_t gone;
} DfeCopy;

/* A copy replaced by word moves (dfe_plan_words): inserted at `at`. */
#define DFE_LOWER_WORDS 4
typedef struct
{
  int at;                     /* the CALL's index (a NOP by then) */
  int nw;
  int dst[DFE_LOWER_WORDS];   /* frame offsets of the destination words */
  int src[DFE_LOWER_WORDS];   /* frame offsets of the source words, or offsets from sptr */
  int has_sptr;
  IROperand sptr;             /* the source pointer value, when not a frame object */
  int is_const;               /* stores of val[] (a fill, an image) */
  uint32_t val[DFE_LOWER_WORDS];
} DfeLower;

typedef struct
{
  TCCIRState *ir;
  int n;
  DfeLower *low;
  int nlow, clow;
  DfeObj *obj;
  int nobj;
  DfeAcc *acc;
  int nacc, cacc;
  DfeCopy *cp;
  int ncp, ccp;
  int32_t ntemp;
  int *tdef;     /* TEMP: its one defining instruction, -1 none, -2 several */
  int *tuses;    /* TEMP: operand reads (non-definition operands) */
  int *tobj, *trel; /* TEMP: holds the address obj+rel (tobj -1: not one) */
  int *troot;       /* TEMP: the offset in obj of the frame operand it derives from */
  int *param_call; /* FUNCPARAMVAL -> its CALL, else -1 */
  int *call_cp;    /* CALL -> its copy table entry, else -1 */
  int *call_param; /* CALL -> 4 x FUNCPARAMVAL index of params 0..3 */
  int *first_param; /* CALL -> its first FUNCPARAMVAL, else the CALL */
  int cur;         /* TCC_DFE_ESC_DBG: the instruction being scanned */
} Dfe;

/* ---- tables ------------------------------------------------------------ */

static int dfe_obj_cmp(const void *a, const void *b)
{
  const DfeObj *x = a, *y = b;
  return x->start < y->start ? -1 : x->start > y->start;
}

static int dfe_find(const Dfe *d, int off)
{
  int lo = 0, hi = d->nobj - 1;
  while (lo <= hi)
  {
    int mid = (lo + hi) / 2;
    if (off < d->obj[mid].start)
      hi = mid - 1;
    else if (off >= d->obj[mid].start + d->obj[mid].span)
      lo = mid + 1;
    else
      return mid;
  }
  return -1;
}

#define dfe_escape(d, o) dfe_escape_at(d, o, __LINE__)
static void dfe_escape_at(Dfe *d, int o, int line)
{
  if (o >= 0 && !d->obj[o].escaped)
  {
    d->obj[o].escaped = 1;
    d->obj[o].esc_why = line;
    d->obj[o].esc_at = d->cur;
  }
}

/* Every object a range of frame bytes [lo, hi) touches. */
static void dfe_escape_range(Dfe *d, int lo, int hi)
{
  for (int o = 0; o < d->nobj; o++)
    if (d->obj[o].start < hi && d->obj[o].start + d->obj[o].span > lo)
      dfe_escape(d, o);
}

static DfeAcc *dfe_add(Dfe *d, int kind, int o, int lo, int hi, int i, int copy)
{
  static DfeAcc sink;
  if (o < 0)
    return &sink;
  if (lo < 0 || hi > d->obj[o].span)
  {
    /* reaches past the object: into a neighbour this pass would not see */
    dfe_escape_range(d, d->obj[o].start + lo, d->obj[o].start + hi);
    return &sink;
  }
  if (d->obj[o].escaped)
    return &sink;
  if (hi <= lo)
    lo = 0, hi = d->obj[o].span;
  if (d->nacc == d->cacc)
  {
    d->cacc = d->cacc ? 2 * d->cacc : 64;
    d->acc = tcc_realloc(d->acc, sizeof(DfeAcc) * d->cacc);
  }
  d->acc[d->nacc] = (DfeAcc){kind, o, lo, hi, i, copy, -1, -1, -1, 0};
  return &d->acc[d->nacc++];
}

/* A frame operand naming a front-end object (no vreg: not a VAR home). */
static int dfe_frame_off(IROperand op, int *off)
{
  if (irop_is_none(op) || irop_get_vreg(op) >= 0 || op.is_param)
    return 0;
  int tag = irop_get_tag(op);
  if (tag != IROP_TAG_STACKOFF && !(tag == IROP_TAG_VREG && (op.is_local || op.is_llocal)))
    return 0;
  *off = irop_get_stack_offset(op);
  return *off < 0;
}

/* Can operand `op` name frame offset `off`?  A STRUCT operand keeps it in
 * 16 bits. */
static int dfe_off_fits(IROperand op, int off)
{
  return op.btype != IROP_BTYPE_STRUCT || (off >= -32768 && off <= 32767);
}

static void dfe_set_off(IROperand *op, int off)
{
  if (op->btype == IROP_BTYPE_STRUCT)
    op->u.s.aux_data = (int16_t)off;
  else
    op->u.imm32 = off;
}

static int dfe_width(IROperand op)
{
  /* a _Complex value is two of its btype: read, written and moved whole
   * as an access of unknown width (to the end of the object) */
  if (op.is_complex)
    return 0;
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

static int dfe_temp_index(const Dfe *d, int32_t vr)
{
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  int p = TCCIR_DECODE_VREG_POSITION(vr);
  return p < d->ntemp ? p : -1;
}

static int dfe_has_slot3(int op)
{
  return tcc_ir_op_is_mac(op) || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_SELECT;
}

static int dfe_nops(int op)
{
  return irop_config[op].has_dest + irop_config[op].has_src1 + irop_config[op].has_src2 + dfe_has_slot3(op);
}

static const char *dfe_callee(TCCIRState *ir, IRQuadCompact *q)
{
  IROperand c = tcc_ir_op_get_src1(ir, q);
  if (irop_get_tag(c) != IROP_TAG_SYMREF)
    return NULL;
  Sym *s = irop_get_sym_ex(ir, c);
  return s ? get_tok_str(s->v, NULL) : NULL;
}

/* For a helper: kind (DFE_COPY / DFE_SET), the parameter holding the count. */
static int dfe_helper(const char *nm, int *nidx)
{
  if (!nm)
    return -1;
  if (ir_opt_is_memcpy_or_memmove_name(nm))
  {
    *nidx = 2;
    return DFE_COPY;
  }
  if (!strcmp(nm, "memset"))
  {
    *nidx = 2;
    return DFE_SET;
  }
  if (ir_opt_name_in(nm, "__aeabi_memset\0__aeabi_memset4\0__aeabi_memset8\0__aeabi_memclr\0__aeabi_memclr4\0"
                         "__aeabi_memclr8\0"))
  {
    *nidx = 1;
    return DFE_SET;
  }
  return -1;
}

static int dfe_eligible(TCCIRState *ir)
{
  if (!tcc_state || TCC_OPT(tcc_state, optimize) <= 0 || tcc_state->do_debug || tcc_bounds_checking(tcc_state))
    return 0;
  if (ir->frame_relaid || ir->frame_obj_count == 0 || ir->is_variadic || ir->has_static_chain ||
      ir->captured_count > 0 || tcc_state->nb_nested_funcs > 0)
    return 0;
  /* Control the IR does not show: a computed goto lands on labels no
   * is_jump_target marks, and a returns-twice call (setjmp, vfork, by name
   * too) comes back a second time after whatever ran since -- forwarding
   * and word lowering both reason about straight runs of code. */
  if (ir->func_has_label_addr || tcc_ir_calls_returns_twice(ir))
    return 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
    switch (ir->compact_instructions[i].op)
    {
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
      return 0;
    default:
      break;
    }
  return 1;
}

/* ---- step 1: references and escapes ------------------------------------- */

/* Address `obj+off` handed to parameter `p` (a FUNCPARAMVAL) -- directly at
 * pool index `apool`, or through the TEMP `temp` defined at `tdef_i`. */
static void dfe_addr_param(Dfe *d, int p, int o, int off, int apool, int temp, int base)
{
  TCCIRState *ir = d->ir;
  IRQuadCompact *pq = &ir->compact_instructions[p];
  int call = d->param_call[p];
  if (call < 0)
  {
    dfe_escape(d, o);
    return;
  }
  IRQuadCompact *cq = &ir->compact_instructions[call];
  uint32_t enc = (uint32_t)tcc_ir_op_src2_imm(ir, pq);
  int idx = TCCIR_DECODE_PARAM_IDX(enc), cid = TCCIR_DECODE_CALL_ID(enc);
  int nidx = -1, kind = dfe_helper(dfe_callee(ir, cq), &nidx);
  const int rel = off - d->obj[o].start;
  /* memcpy/memset return their destination: only a result nobody reads */
  int result_unused = cq->op == TCCIR_OP_FUNCCALLVOID;
  if (cq->op == TCCIR_OP_FUNCCALLVAL)
  {
    int32_t rv = tcc_ir_op_dest_vreg(ir, cq);
    int rt = dfe_temp_index(d, rv);
    result_unused = rv < 0 || (rt >= 0 && d->tuses[rt] == 0);
  }
  if (kind >= 0 && result_unused && (idx == 0 || (kind == DFE_COPY && idx == 1)))
  {
    int np = d->call_param[4 * call + nidx];
    IROperand nv = np >= 0 ? tcc_ir_op_get_src1(ir, &ir->compact_instructions[np]) : IROP_NONE;
    int64_t n = np >= 0 && irop_is_immediate(nv) ? irop_get_imm64_ex(ir, nv) : -1;
    if (n <= 0 || n > DFE_MAX_SPAN || rel < 0 || rel + n > d->obj[o].span)
    {
      dfe_escape_range(d, off, n > 0 && n <= DFE_MAX_SPAN ? off + (int)n : 0);
      dfe_escape(d, o);
      return;
    }
    /* one table entry per call, shared by its two sides */
    int c = d->call_cp[call];
    if (c < 0)
    {
      if (d->ncp == d->ccp)
      {
        d->ccp = d->ccp ? 2 * d->ccp : 32;
        d->cp = tcc_realloc(d->cp, sizeof(DfeCopy) * d->ccp);
      }
      c = d->ncp++;
      d->cp[c] = (DfeCopy){.call = call, .kind = kind, .n = (int)n, .dobj = -1, .sobj = -1, .spool = -1,
                           .dpool = -1, .npool = -1, .dtemp = -1, .stemp = -1, .dbase = -1, .sbase = -1};
      int nb = ir->compact_instructions[np].operand_base + irop_config[TCCIR_OP_FUNCPARAMVAL].has_dest;
      d->cp[c].npool = nb;
      d->call_cp[call] = c;
    }
    DfeCopy *cp = &d->cp[c];
    if (idx == 0)
    {
      cp->dobj = o, cp->doff = rel, cp->dpool = apool, cp->dtemp = temp, cp->dbase = base;
      /* The front end proves a struct copy's destination non-volatile
       * (vstore.c); in a body with volatile accesses, any other write
       * through a helper stays as it is. */
      IROperand pa = tcc_ir_op_get_src1(ir, pq);
      if (ir->func_has_volatile_access && !(pa.aux & IROP_AUX_NONVOLATILE))
        cp->keep = 1;
    }
    else
    {
      cp->sobj = o, cp->soff = rel, cp->spool = apool, cp->stemp = temp, cp->sbase = base;
      /* nor are the reads of a source that may be volatile */
      IROperand pa = tcc_ir_op_get_src1(ir, pq);
      if (ir->func_has_volatile_access && !(pa.aux & IROP_AUX_NONVOLATILE))
        cp->keep = 1;
    }
    return;
  }
  if (idx == 0 && cid >= 0 && cid < ir->sret_calls_size && ir->sret_calls[cid])
  {
    DfeAcc *a = dfe_add(d, DFE_FIXED, o, rel, rel + ir->sret_calls[cid], call, -1); /* past the end: escapes */
    a->pool = apool, a->temp = temp, a->base = base;
    return;
  }
  dfe_escape(d, o);
}

/* The non-frame side of a copy: a source the pass can move (a SYMREF), or a
 * pointer it cannot; the destination side of a non-frame destination. */
static void dfe_note_other_side(Dfe *d, int call)
{
  TCCIRState *ir = d->ir;
  for (int c = 0; c < d->ncp; c++)
  {
    DfeCopy *cp = &d->cp[c];
    if (cp->call != call || cp->kind != DFE_COPY || cp->sobj >= 0 || cp->spool >= 0)
      continue;
    int p = d->call_param[4 * call + 1];
    if (p < 0)
      continue;
    int pool = ir->compact_instructions[p].operand_base + irop_config[TCCIR_OP_FUNCPARAMVAL].has_dest;
    IROperand s = ir->iroperand_pool[pool];
    if (irop_get_tag(s) == IROP_TAG_SYMREF && !s.is_lval)
      cp->spool = pool, cp->ssym = 1;
  }
}

/* `T <- &obj; T <- v STORE_INDEXED #288` names the object at its start and
 * reaches 288 bytes further: the piece holding the access would have to
 * stretch back to the start.  When every reader of such a TEMP is an indexed
 * access by a constant, the TEMP takes the lowest of them as its address and
 * the indices shrink by as much -- the same bytes, named where they are. */
static int dfe_indexed_base_slot(int op)
{
  return op == TCCIR_OP_STORE_INDEXED ? 0 : op == TCCIR_OP_LOAD_INDEXED ? irop_config[op].has_dest : -1;
}

static int dfe_rebase_indexed(Dfe *d)
{
  TCCIRState *ir = d->ir;
  const int n = d->n;
  int rebased = 0;
  if (d->ntemp <= 0)
    return 0;
  int *lowest = tcc_malloc(sizeof(int) * d->ntemp), *count = tcc_mallocz(sizeof(int) * d->ntemp);
  for (int t = 0; t < d->ntemp; t++)
    lowest[t] = DFE_MAX_SPAN;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const int bslot = dfe_indexed_base_slot(q->op);
    if (bslot < 0)
      continue;
    IROperand b = ir->iroperand_pool[q->operand_base + bslot];
    int t = dfe_temp_index(d, irop_get_vreg(b));
    if (t < 0 || b.is_lval)
      continue;
    IROperand ix = tcc_ir_op_get_src2(ir, q), sc = tcc_ir_op_get_scale(ir, q);
    int ok = irop_get_tag(ix) == IROP_TAG_IMM32 && !ix.is_lval &&
             (irop_is_none(sc) || (irop_is_immediate(sc) && irop_get_imm64_ex(ir, sc) == 0));
    int64_t at = ok ? irop_get_imm64_ex(ir, ix) : -1;
    if (!ok || at < 0 || at >= DFE_MAX_SPAN)
      lowest[t] = -1;
    else if (lowest[t] >= 0 && at < lowest[t])
      lowest[t] = (int)at;
    count[t]++;
  }
  for (int t = 0; t < d->ntemp; t++)
  {
    int off;
    /* only the address of a candidate: an escaped object may be reached
     * through T's register where no IR operand shows it (an inline asm "m"
     * operand lowered from a saved SValue) */
    if (lowest[t] <= 0 || lowest[t] >= DFE_MAX_SPAN || d->tdef[t] < 0 || count[t] != d->tuses[t] ||
        d->tobj[t] < 0 || d->obj[d->tobj[t]].escaped)
    {
      lowest[t] = 0;
      continue;
    }
    IRQuadCompact *dq = &ir->compact_instructions[d->tdef[t]];
    IROperand *src = (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_LEA) && irop_config[dq->op].has_src1
                         ? &ir->iroperand_pool[dq->operand_base + irop_config[dq->op].has_dest]
                         : NULL;
    if (!src || src->is_lval || !dfe_frame_off(*src, &off) ||
        (irop_config[dq->op].has_src2 && !tcc_ir_op_src2_is_none(ir, dq)) || off + lowest[t] >= 0 ||
        !dfe_off_fits(*src, off + lowest[t]))
    {
      lowest[t] = 0;
      continue;
    }
    dfe_set_off(src, off + lowest[t]);
    rebased++;
  }
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const int bslot = dfe_indexed_base_slot(q->op);
    if (bslot < 0)
      continue;
    int t = dfe_temp_index(d, irop_get_vreg(ir->iroperand_pool[q->operand_base + bslot]));
    if (t < 0 || lowest[t] <= 0)
      continue;
    IROperand *ip = &ir->iroperand_pool[q->operand_base + irop_config[q->op].has_dest + 1];
    *ip = irop_make_imm32(-1, (int32_t)(irop_get_imm64_ex(ir, *ip) - lowest[t]), ip->btype);
  }
  tcc_free(lowest);
  tcc_free(count);
  return rebased;
}

static int dfe_scan(Dfe *d)
{
  TCCIRState *ir = d->ir;
  const int n = d->n;

  /* TEMP definitions and reads */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    const int nops = dfe_nops(q->op);
    for (int k = 0; k < nops; k++)
    {
      IROperand op = ir->iroperand_pool[q->operand_base + k];
      int t = dfe_temp_index(d, irop_get_vreg(op));
      if (t < 0)
        continue;
      if (k == 0 && irop_config[q->op].has_dest && irop_dest_defines_vreg(op) && q->op != TCCIR_OP_STORE_INDEXED)
        d->tdef[t] = d->tdef[t] == -1 ? i : -2;
      else
        d->tuses[t]++;
    }
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const int op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    d->cur = i;
    const int nops = dfe_nops(op);
    const int has_dest = irop_config[op].has_dest;
    for (int k = 0; k < nops; k++)
    {
      const int pool = q->operand_base + k;
      IROperand a = ir->iroperand_pool[pool];
      int off, o;
      const int is_dest = has_dest && k == 0;
      if (dfe_frame_off(a, &off))
      {
        if ((o = dfe_find(d, off)) < 0)
          continue;
        const int rel = off - d->obj[o].start;
        if (a.is_llocal || (a.is_lval && tcc_ir_access_is_volatile(ir, a)))
        {
          dfe_escape(d, o);
          continue;
        }
        if (op == TCCIR_OP_BLOCK_COPY && is_dest)
        {
          IROperand sz = tcc_ir_op_get_src2(ir, q), src = tcc_ir_op_get_src1(ir, q);
          int64_t nb = irop_is_immediate(sz) ? irop_get_imm64_ex(ir, sz) : -1;
          if (nb <= 0 || nb > DFE_MAX_SPAN || rel + nb > d->obj[o].span || irop_get_tag(src) != IROP_TAG_SYMREF)
          {
            dfe_escape_range(d, off, nb > 0 && nb <= DFE_MAX_SPAN ? off + (int)nb : 0);
            dfe_escape(d, o);
            continue;
          }
          if (d->ncp == d->ccp)
          {
            d->ccp = d->ccp ? 2 * d->ccp : 32;
            d->cp = tcc_realloc(d->cp, sizeof(DfeCopy) * d->ccp);
          }
          d->cp[d->ncp++] = (DfeCopy){.call = i, .kind = DFE_BC, .n = (int)nb, .dobj = o, .doff = rel,
                                      .dpool = pool, .sobj = -1, .spool = pool + 1, .ssym = 1, .npool = pool + 2,
                                      .dtemp = -1, .stemp = -1, .keep = (uint8_t)tcc_ir_access_is_volatile(ir, a)};
          continue;
        }
        if (a.is_lval)
        {
          int w = dfe_width(a);
          if (op == TCCIR_OP_FUNCPARAMVAL || w == 0)
            w = op == TCCIR_OP_FUNCPARAMVAL && w ? w : d->obj[o].span - rel; /* a by-value struct: to the end */
          dfe_add(d, is_dest && op == TCCIR_OP_STORE && dfe_width(a) ? DFE_STORE : DFE_READ, o, rel, rel + w, i,
                  -1)->pool = pool;
          continue;
        }
        /* the object's address */
        if (op == TCCIR_OP_FUNCPARAMVAL && !is_dest && k == has_dest)
        {
          dfe_addr_param(d, i, o, off, pool, -1, -1);
          continue;
        }
        if ((op == TCCIR_OP_ASSIGN || op == TCCIR_OP_LEA) && k == has_dest && has_dest)
        {
          IROperand dst = tcc_ir_op_get_dest(ir, q);
          int t = dfe_temp_index(d, irop_get_vreg(dst));
          if (t >= 0 && !dst.is_lval && irop_get_tag(dst) == IROP_TAG_VREG && d->tdef[t] == i &&
              !(op == TCCIR_OP_LEA && irop_config[op].has_src2 && !tcc_ir_op_src2_is_none(ir, q)))
            continue; /* followed through the TEMP below */
        }
        dfe_escape(d, o);
        continue;
      }
    }
  }

  /* TEMPs holding an object's address: defined once, to the address itself
   * or to such a TEMP moved or offset by a constant. */
  for (int round = 0, changed = 1; changed && round < 8; round++)
  {
    changed = 0;
    for (int t = 0; t < d->ntemp; t++)
    {
      if (d->tdef[t] < 0 || d->tobj[t] >= 0)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[d->tdef[t]];
      IROperand dst = tcc_ir_op_get_dest(ir, q), src = tcc_ir_op_get_src1(ir, q);
      if (dst.is_lval || irop_get_tag(dst) != IROP_TAG_VREG)
        continue;
      int off, o = -1, rel = 0, root = 0, st;
      if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA) && !src.is_lval && dfe_frame_off(src, &off) &&
          (o = dfe_find(d, off)) >= 0 &&
          !(irop_config[q->op].has_src2 && !tcc_ir_op_src2_is_none(ir, q)))
        rel = off - d->obj[o].start, root = rel;
      else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && !src.is_lval &&
               irop_get_tag(src) == IROP_TAG_VREG && (st = dfe_temp_index(d, irop_get_vreg(src))) >= 0 &&
               d->tobj[st] >= 0)
      {
        o = d->tobj[st], rel = d->trel[st], root = d->troot[st];
        if (q->op != TCCIR_OP_ASSIGN)
        {
          if (!tcc_ir_op_src2_is_imm(ir, q) || tcc_ir_op_src2_is_lval(ir, q))
            continue;
          int64_t k = tcc_ir_op_src2_imm(ir, q);
          if (k < -DFE_MAX_SPAN || k > DFE_MAX_SPAN)
            continue;
          rel += q->op == TCCIR_OP_ADD ? (int)k : -(int)k;
        }
        else if (irop_config[q->op].has_src2 && !tcc_ir_op_src2_is_none(ir, q))
          continue;
      }
      else
        continue;
      d->tobj[t] = o, d->trel[t] = rel, d->troot[t] = root;
      changed = 1;
    }
  }

  /* Every read of such a TEMP: an access through it, an argument, or the
   * source of another one.  Anything else lets the address go. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const int op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    d->cur = i;
    const int nops = dfe_nops(op);
    const int has_dest = irop_config[op].has_dest;
    for (int k = 0; k < nops; k++)
    {
      const int pool = q->operand_base + k;
      IROperand a = ir->iroperand_pool[pool];
      int t = dfe_temp_index(d, irop_get_vreg(a));
      if (t < 0 || d->tdef[t] < 0)
        continue;
      const int is_dest = has_dest && k == 0;
      if (is_dest && irop_dest_defines_vreg(a) && op != TCCIR_OP_STORE_INDEXED)
        continue;
      const int o = d->tobj[t], rel = d->trel[t];
      if (o < 0)
      {
        /* defined from a frame address in a way not followed */
        IRQuadCompact *dq = &ir->compact_instructions[d->tdef[t]];
        IROperand src = tcc_ir_op_get_src1(ir, dq);
        int off, o2;
        if (irop_config[dq->op].has_src1 && !src.is_lval && dfe_frame_off(src, &off) && (o2 = dfe_find(d, off)) >= 0)
          dfe_escape(d, o2);
        continue;
      }
      if ((op == TCCIR_OP_LOAD_INDEXED && k == has_dest) || (op == TCCIR_OP_STORE_INDEXED && k == 0))
      {
        /* base + (index << scale), a constant index: a known place */
        IROperand ix = tcc_ir_op_get_src2(ir, q), sc = tcc_ir_op_get_scale(ir, q);
        IROperand val = tcc_ir_op_get_dest_or_src1(ir, q, op != TCCIR_OP_LOAD_INDEXED);
        if (!irop_is_immediate(ix) || ix.is_lval || (!irop_is_none(sc) && !irop_is_immediate(sc)) ||
            tcc_ir_access_is_volatile(ir, a))
        {
          dfe_escape(d, o);
          continue;
        }
        int64_t at = irop_get_imm64_ex(ir, ix) << (irop_is_none(sc) ? 0 : irop_get_imm64_ex(ir, sc));
        int w = dfe_width(val);
        if (at < -DFE_MAX_SPAN || at > DFE_MAX_SPAN || w == 0)
        {
          dfe_escape(d, o);
          continue;
        }
        /* kept (a store too): its bytes stay live */
        dfe_add(d, DFE_READ, o, rel + (int)at, rel + (int)at + w, i, -1)->base = d->troot[t];
        continue;
      }
      if (a.is_lval)
      {
        /* through the pointer: *T */
        int w = dfe_width(a);
        if (tcc_ir_access_is_volatile(ir, a) || irop_get_tag(a) != IROP_TAG_VREG || a.is_llocal ||
            op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_LOAD_POSTINC ||
            op == TCCIR_OP_STORE_POSTINC)
          dfe_escape(d, o);
        else
          dfe_add(d, is_dest && op == TCCIR_OP_STORE && w ? DFE_STORE : DFE_READ, o, rel,
                  w ? rel + w : d->obj[o].span, i, -1)->base = d->troot[t];
        continue;
      }
      if (op == TCCIR_OP_FUNCPARAMVAL && !is_dest && k == has_dest)
      {
        /* the address can move with the call's range only when the TEMP is
         * the frame address itself */
        IRQuadCompact *dq = &ir->compact_instructions[d->tdef[t]];
        int off;
        int direct = (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_LEA) &&
                     dfe_frame_off(tcc_ir_op_get_src1(ir, dq), &off);
        dfe_addr_param(d, i, o, d->obj[o].start + rel,
                       direct ? (int)(dq->operand_base + irop_config[dq->op].has_dest) : -1, t, d->troot[t]);
        continue;
      }
      if (has_dest && (op == TCCIR_OP_ASSIGN || op == TCCIR_OP_ADD || op == TCCIR_OP_SUB) && k == has_dest)
      {
        int td = dfe_temp_index(d, tcc_ir_op_dest_vreg(ir, q));
        if (td >= 0 && d->tdef[td] == i && d->tobj[td] >= 0)
          continue; /* followed */
      }
      dfe_escape(d, o);
    }
  }

  /* Inline asm names frame slots outside the IR. */
  {
    int off, o;
    for (int k = 0; tcc_ir_asm_frame_ref(ir, k, &off); k++)
      if ((o = dfe_find(d, off)) >= 0)
        dfe_escape(d, o);
  }

  /* Copies: the non-frame side, and the copy accesses themselves. */
  for (int c = 0; c < d->ncp; c++)
  {
    DfeCopy *cp = &d->cp[c];
    if (cp->kind == DFE_COPY && cp->sobj < 0)
      dfe_note_other_side(d, cp->call);
    /* a source outside the frame that may be volatile is read as it is */
    if (cp->kind == DFE_COPY && ir->func_has_volatile_access)
    {
      int p1 = d->call_param[4 * cp->call + 1];
      if (p1 < 0 || !(tcc_ir_op_get_src1(ir, &ir->compact_instructions[p1]).aux & IROP_AUX_NONVOLATILE))
        cp->keep = 1;
    }
    if (cp->dobj >= 0)
      dfe_add(d, cp->kind, cp->dobj, cp->doff, cp->doff + cp->n, cp->call, c);
    if (cp->sobj >= 0)
      dfe_add(d, DFE_CSRC, cp->sobj, cp->soff, cp->soff + cp->n, cp->call, c);
  }
  return 1;
}

/* ---- step 2: liveness ---------------------------------------------------- */

static void dfe_set_live(Dfe *d, int o, int lo, int hi)
{
  if (o < 0)
    return;
  uint8_t *l = d->obj[o].live;
  for (int b = lo; b < hi && b < d->obj[o].span; b++)
    l[b] = 1;
}

static void dfe_liveness(Dfe *d)
{
  for (int o = 0; o < d->nobj; o++)
    if (d->obj[o].escaped)
      memset(d->obj[o].live, 1, d->obj[o].span);
  for (int x = 0; x < d->nacc; x++)
    if (d->acc[x].kind == DFE_READ && !d->obj[d->acc[x].obj].escaped)
      dfe_set_live(d, d->acc[x].obj, d->acc[x].lo, d->acc[x].hi);
  int changed;
  do
  {
    changed = 0;
    for (int c = 0; c < d->ncp; c++)
    {
      DfeCopy *cp = &d->cp[c];
      if (cp->kind != DFE_COPY || cp->sobj < 0)
        continue;
      uint8_t *sl = d->obj[cp->sobj].live;
      if (cp->dobj < 0)
      {
        /* into memory the pass does not follow */
        for (int b = 0; b < cp->n; b++)
          if (!sl[cp->soff + b])
            sl[cp->soff + b] = 1, changed = 1;
        continue;
      }
      const uint8_t *dl = d->obj[cp->dobj].live;
      for (int b = 0; b < cp->n; b++)
        if (dl[cp->doff + b] && !sl[cp->soff + b])
          sl[cp->soff + b] = 1, changed = 1;
    }
  } while (changed);
}

/* ---- step 3: rewrites ----------------------------------------------------- */

static void dfe_nop_call(Dfe *d, int call)
{
  TCCIRState *ir = d->ir;
  ir_opt_nop_call_params(ir, call);
  ir->compact_instructions[call].op = TCCIR_OP_NOP;
}

/* A TEMP carrying a moved address must have no other reader. */
static int dfe_temp_movable(Dfe *d, int t)
{
  return t < 0 || (d->tdef[t] >= 0 && d->tuses[t] == 1);
}

static int dfe_shift_symref(Dfe *d, int pool, int by)
{
  TCCIRState *ir = d->ir;
  IROperand *op = &ir->iroperand_pool[pool];
  IRPoolSymref *sr = irop_get_symref_ex(ir, *op);
  if (!sr || !sr->sym)
    return 0;
  uint32_t idx = tcc_ir_pool_add_symref(ir, sr->sym, sr->addend + by, sr->flags);
  op = &ir->iroperand_pool[pool];
  if (op->btype == IROP_BTYPE_STRUCT)
  {
    if (idx > 0xFFFF)
      return 0;
    op->u.s.aux_data = (int16_t)(uint16_t)idx;
  }
  else
    op->u.pool_idx = idx;
  return 1;
}

/* A copy most of whose destination is dead, its live bytes in a few words
 * apart (an optional's tag and one field of its 296-byte payload): the hull a
 * shrunk copy would keep spans them all, so the object could not be cut.
 * Instead the words are moved one by one -- a load and a store each, inserted
 * where the call was.  Both sides must be word aligned (frame objects at word
 * offsets, or a pointer the helper's name promises 4-aligned), and the copy
 * a whole number of words. */
static int dfe_plan_words(Dfe *d, DfeCopy *cp, int hull)
{
  TCCIRState *ir = d->ir;
  if (cp->kind != DFE_COPY || cp->ssym || (cp->n & 3) || ir->compact_instructions[cp->call].is_jump_target)
    return 0;
  const int dabs = d->obj[cp->dobj].start + cp->doff;
  if ((dabs & 3) || (d->obj[cp->dobj].start & 3))
    return 0;
  const char *nm = dfe_callee(ir, &ir->compact_instructions[cp->call]);
  size_t ln = nm ? strlen(nm) : 0;
  const int helper4 = ln && (nm[ln - 1] == '4' || nm[ln - 1] == '8');
  int p1 = d->call_param[4 * cp->call + 1];
  if (p1 < 0)
    return 0;
  IROperand sv = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p1]);
  /* a source that may be volatile is read as the copy reads it */
  if (ir->func_has_volatile_access && !(sv.aux & IROP_AUX_NONVOLATILE))
    return 0;
  int sabs = 0, has_sptr = 0;
  if (cp->sobj >= 0)
  {
    sabs = d->obj[cp->sobj].start + cp->soff;
    if ((sabs & 3) || (d->obj[cp->sobj].start & 3))
      return 0;
    /* a memmove within one object: the words go in order, so a later one
     * would read what an earlier one wrote */
    if (sabs < dabs + cp->n && dabs < sabs + cp->n)
      return 0;
  }
  else
  {
    if (!helper4 || sv.is_lval || irop_get_tag(sv) != IROP_TAG_VREG || irop_get_vreg(sv) < 0)
      return 0;
    /* the pointer is read again where the call was: nothing between its
     * argument and the call may redefine it or write memory */
    for (int j = p1 + 1; j < cp->call; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_FUNCPARAMVAL)
        continue;
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_LOAD)
        return 0;
      IROperand dj = tcc_ir_op_get_dest(ir, q);
      if (dj.is_lval || irop_get_tag(dj) != IROP_TAG_VREG || irop_get_vreg(dj) == irop_get_vreg(sv))
        return 0;
    }
    has_sptr = 1;
  }
  const uint8_t *l = d->obj[cp->dobj].live;
  DfeLower lw = {.at = cp->call, .has_sptr = has_sptr, .sptr = sv};
  for (int w = 0; w < cp->n; w += 4)
  {
    int any = 0;
    for (int b = w; b < w + 4; b++)
      any |= l[cp->doff + b];
    if (!any)
      continue;
    if (lw.nw == DFE_LOWER_WORDS)
      return 0;
    lw.dst[lw.nw] = dabs + w;
    lw.src[lw.nw] = has_sptr ? w : sabs + w;
    lw.nw++;
  }
  /* worth it only when the words keep much less than the shrunk copy would */
  if (hull - 4 * lw.nw < 16)
    return 0;
  if (d->nlow == d->clow)
  {
    d->clow = d->clow ? 2 * d->clow : 8;
    d->low = tcc_realloc(d->low, sizeof(DfeLower) * d->clow);
  }
  d->low[d->nlow++] = lw;
  /* the new references, for the pieces */
  for (int k = 0; k < lw.nw; k++)
  {
    dfe_add(d, DFE_READ, cp->dobj, lw.dst[k] - d->obj[cp->dobj].start, lw.dst[k] - d->obj[cp->dobj].start + 4, cp->call,
            -1);
    if (!has_sptr)
      dfe_add(d, DFE_READ, cp->sobj, lw.src[k] - d->obj[cp->sobj].start, lw.src[k] - d->obj[cp->sobj].start + 4,
              cp->call, -1);
  }
  return 1;
}

/* The `len` bytes at `extra` into the .rodata image a SYMREF operand names,
 * when they are known: a defined, read-only, non-weak symbol and no
 * relocation over them. */
static int dfe_image_bytes(TCCIRState *ir, IROperand op, int extra, int len, uint8_t *out)
{
  IRPoolSymref *sr = irop_get_symref_ex(ir, op);
  if (!sr || !sr->sym || (sr->flags & IRPOOL_SYMREF_LVAL) || op.is_lval)
    return 0;
  Sym *sym = sr->sym;
  if (sym->a.weak || sym->a.tentative || sym->a.dllimport)
    return 0;
  ElfSym *esym = elfsym(sym);
  if (!esym || esym->st_shndx == SHN_UNDEF || esym->st_shndx >= (unsigned)tcc_state->nb_sections)
    return 0;
  Section *sec = tcc_state->sections[esym->st_shndx];
  if (!sec || !sec->data || (sec->sh_flags & SHF_WRITE) || sec->sh_type == SHT_NOBITS)
    return 0;
  int64_t rel = (int64_t)sr->addend + extra;
  if (rel < 0 || (esym->st_size && rel + len > (int64_t)esym->st_size))
    return 0;
  addr_t off = esym->st_value + (addr_t)rel;
  if (off + len > sec->data_offset)
    return 0;
  if (sec->reloc && sec->reloc->data && sec->reloc->data_offset)
  {
    ElfW_Rel *r;
    for_each_elem(sec->reloc, 0, r, ElfW_Rel)
    {
      addr_t ro = (addr_t)r->r_offset;
      if (ro + 4 > off && ro < off + len)
        return 0;
    }
  }
  memcpy(out, sec->data + off, len);
  return 1;
}

/* A fill or an image write most of whose bytes are dead, the live ones in a
 * few words: constant stores of those words instead (dfe_plan_words). */
static int dfe_plan_const(Dfe *d, DfeCopy *cp, int hull)
{
  TCCIRState *ir = d->ir;
  if (ir->compact_instructions[cp->call].is_jump_target)
    return 0;
  if (cp->kind == DFE_COPY && !cp->ssym)
    return 0;
  const int dabs = d->obj[cp->dobj].start + cp->doff;
  if (d->obj[cp->dobj].start & 3)
    return 0;
  uint8_t fill = 0;
  IROperand img = IROP_NONE;
  if (cp->kind == DFE_SET)
  {
    const char *nm = dfe_callee(ir, &ir->compact_instructions[cp->call]);
    int vidx = nm && !strcmp(nm, "memset") ? 1 : 2;
    int pv = strstr(nm ? nm : "", "memclr") ? -1 : d->call_param[4 * cp->call + vidx];
    if (pv >= 0)
    {
      IROperand v = tcc_ir_op_get_src1(ir, &ir->compact_instructions[pv]);
      if (irop_get_tag(v) != IROP_TAG_IMM32 || v.is_lval)
        return 0;
      fill = (uint8_t)irop_get_imm64_ex(ir, v);
    }
  }
  else
    img = ir->iroperand_pool[cp->spool];
  const uint8_t *l = d->obj[cp->dobj].live;
  DfeLower lw = {.at = cp->call, .is_const = 1};
  /* words at frame offsets that are multiples of 4 */
  for (int a = (dabs & ~3) - dabs; a < cp->n; a += 4)
  {
    int any = 0;
    for (int b = a; b < a + 4; b++)
      if (b >= 0 && b < cp->n)
        any |= l[cp->doff + b];
    if (!any)
      continue;
    if (a < 0 || a + 4 > cp->n || lw.nw == DFE_LOWER_WORDS)
      return 0;
    uint8_t by[4] = {fill, fill, fill, fill};
    if (cp->kind != DFE_SET && !dfe_image_bytes(ir, img, a, 4, by))
      return 0;
    lw.dst[lw.nw] = dabs + a;
    lw.val[lw.nw] = (uint32_t)by[0] | (uint32_t)by[1] << 8 | (uint32_t)by[2] << 16 | (uint32_t)by[3] << 24;
    lw.nw++;
  }
  if (lw.nw == 0 || hull - 4 * lw.nw < 16)
    return 0;
  if (d->nlow == d->clow)
  {
    d->clow = d->clow ? 2 * d->clow : 8;
    d->low = tcc_realloc(d->low, sizeof(DfeLower) * d->clow);
  }
  d->low[d->nlow++] = lw;
  for (int k = 0; k < lw.nw; k++)
    dfe_add(d, DFE_READ, cp->dobj, lw.dst[k] - d->obj[cp->dobj].start, lw.dst[k] - d->obj[cp->dobj].start + 4, cp->call,
            -1);
  return 1;
}

/* Insert the planned word moves, last call first so the earlier indices
 * stay put.  Returns the instructions inserted. */
static int dfe_lower_cmp(const void *a, const void *b)
{
  const DfeLower *x = a, *y = b;
  return y->at - x->at;
}

static IROperand dfe_frame_word(int off)
{
  IROperand o = irop_make_stackoff(-1, off, 1, 0, 0, IROP_BTYPE_INT32);
  o.aux |= IROP_AUX_NONVOLATILE;
  return o;
}

/* A fresh instruction at the call's line.  Its orig_index is its own: the
 * side tables keyed by it (barrel_shifts, zero_half64, shift64_dead_half,
 * the label address map) must not hand it instruction 0's entries. */
static IRQuadCompact dfe_new_quad(TCCIRState *ir, int op, int line)
{
  IRQuadCompact q = {0};
  q.op = op;
  q.line_num = line;
  q.orig_index = ++ir->max_orig_index;
  return q;
}

static int dfe_insert_words(Dfe *d)
{
  TCCIRState *ir = d->ir;
  int inserted = 0;
  tcc_qsort(d->low, d->nlow, sizeof(DfeLower), dfe_lower_cmp);
  for (int x = 0; x < d->nlow; x++)
  {
    DfeLower *lw = &d->low[x];
    int at = lw->at;
    const int line = ir->compact_instructions[at].line_num;
    for (int k = 0; k < lw->nw && lw->is_const; k++)
    {
      IRQuadCompact sq = dfe_new_quad(ir, TCCIR_OP_STORE, line);
      sq.operand_base = tcc_ir_pool_add(ir, dfe_frame_word(lw->dst[k]));
      tcc_ir_pool_add(ir, irop_make_imm32(-1, (int32_t)lw->val[k], IROP_BTYPE_INT32));
      tcc_ir_insert_instruction_before(ir, at++, &sq);
      inserted++;
    }
    for (int k = 0; k < lw->nw && !lw->is_const; k++)
    {
      int32_t tv = tcc_ir_get_vreg_temp(ir);
      IROperand src;
      if (lw->has_sptr)
      {
        IROperand base = lw->sptr;
        if (lw->src[k])
        {
          /* T2 <- p ADD #k */
          int32_t ta = tcc_ir_get_vreg_temp(ir);
          IRQuadCompact aq = dfe_new_quad(ir, TCCIR_OP_ADD, line);
          aq.operand_base = tcc_ir_pool_add(ir, irop_make_vreg(ta, IROP_BTYPE_INT32));
          tcc_ir_pool_add(ir, base);
          tcc_ir_pool_add(ir, irop_make_imm32(-1, lw->src[k], IROP_BTYPE_INT32));
          tcc_ir_insert_instruction_before(ir, at++, &aq);
          inserted++;
          base = irop_make_vreg(ta, IROP_BTYPE_INT32);
        }
        src = irop_make_vreg(irop_get_vreg(base), IROP_BTYPE_INT32);
        src.is_lval = 1;
        src.aux |= IROP_AUX_NONVOLATILE;
      }
      else
        src = dfe_frame_word(lw->src[k]);
      IRQuadCompact lq = dfe_new_quad(ir, TCCIR_OP_LOAD, line);
      lq.operand_base = tcc_ir_pool_add(ir, irop_make_vreg(tv, IROP_BTYPE_INT32));
      tcc_ir_pool_add(ir, src);
      tcc_ir_insert_instruction_before(ir, at++, &lq);
      IRQuadCompact sq = dfe_new_quad(ir, TCCIR_OP_STORE, line);
      sq.operand_base = tcc_ir_pool_add(ir, dfe_frame_word(lw->dst[k]));
      tcc_ir_pool_add(ir, irop_make_vreg(tv, IROP_BTYPE_INT32));
      tcc_ir_insert_instruction_before(ir, at++, &sq);
      inserted += 2;
    }
  }
  return inserted;
}

static int dfe_rewrite(Dfe *d, int *deleted, int *shrunk)
{
  TCCIRState *ir = d->ir;
  int changes = 0;
  /* stores into dead bytes */
  for (int x = 0; x < d->nacc; x++)
  {
    DfeAcc *a = &d->acc[x];
    if (a->kind != DFE_STORE || d->obj[a->obj].escaped)
      continue;
    const uint8_t *l = d->obj[a->obj].live;
    int any = 0;
    for (int b = a->lo; b < a->hi && !any; b++)
      any = l[b];
    if (any)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[a->i];
    /* a store through a TEMP: that read of the TEMP goes */
    if (irop_config[q->op].has_dest)
    {
      int t = dfe_temp_index(d, tcc_ir_op_dest_vreg(ir, q));
      if (t >= 0)
        d->tuses[t]--;
      int tv = dfe_temp_index(d, tcc_ir_op_src1_vreg(ir, q));
      if (tv >= 0)
        d->tuses[tv]--;
    }
    q->op = TCCIR_OP_NOP;
    a->dead = 1;
    (*deleted)++;
    changes++;
  }
  /* copies, fills and images */
  for (int c = 0; c < d->ncp; c++)
  {
    DfeCopy *cp = &d->cp[c];
    if (cp->dobj < 0 || d->obj[cp->dobj].escaped || cp->keep)
      continue;
    const uint8_t *l = d->obj[cp->dobj].live;
    int lo = -1, hi = -1;
    for (int b = 0; b < cp->n; b++)
      if (l[cp->doff + b])
      {
        if (lo < 0)
          lo = b;
        hi = b + 1;
      }
    if (lo < 0)
    {
      if (cp->kind == DFE_BC)
        ir->compact_instructions[cp->call].op = TCCIR_OP_NOP;
      else
        dfe_nop_call(d, cp->call);
      if (cp->dtemp >= 0)
        d->tuses[cp->dtemp]--;
      if (cp->stemp >= 0)
        d->tuses[cp->stemp]--;
      cp->gone = 1;
      (*deleted)++;
      changes++;
      continue;
    }
    int by = lo & ~7;
    /* the start moves only when every address can */
    if (by && (cp->dpool < 0 || !dfe_temp_movable(d, cp->dtemp) ||
               (cp->kind == DFE_COPY &&
                (cp->spool < 0 || (!cp->ssym && (cp->sobj < 0 || !dfe_temp_movable(d, cp->stemp)))))))
      by = 0;
    /* the helpers' size unit: __aeabi_memmove4 promises a multiple of 4 */
    int unit = 1;
    if (cp->kind != DFE_BC)
    {
      const char *nm = dfe_callee(ir, &ir->compact_instructions[cp->call]);
      size_t ln = nm ? strlen(nm) : 0;
      if (ln && (nm[ln - 1] == '4' || nm[ln - 1] == '8'))
        unit = nm[ln - 1] - '0';
    }
    int nn = (hi - by + unit - 1) / unit * unit;
    if (nn > cp->n - by)
      nn = cp->n - by;
    /* an inline BLOCK_COPY (under 64 bytes) is LDM/STM: word aligned only */
    if (cp->kind == DFE_BC && cp->n >= TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES && nn < TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES)
    {
      IRPoolSymref *sr = irop_get_symref_ex(ir, ir->iroperand_pool[cp->spool]);
      int doff = irop_get_stack_offset(ir->iroperand_pool[cp->dpool]);
      if (!sr || (sr->addend & 3) || (doff & 3))
        nn = cp->n - by < TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES ? cp->n - by : TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES;
    }
    if (!dfe_lower_off() && (dfe_plan_words(d, cp, nn) || dfe_plan_const(d, cp, nn)))
    {
      if (cp->kind == DFE_BC)
        ir->compact_instructions[cp->call].op = TCCIR_OP_NOP;
      else
        dfe_nop_call(d, cp->call);
      if (cp->dtemp >= 0)
        d->tuses[cp->dtemp]--;
      if (cp->stemp >= 0)
        d->tuses[cp->stemp]--;
      cp->gone = 1;
      (*shrunk)++;
      changes++;
      continue;
    }
    if (by == 0 && nn == cp->n)
      continue;
    if (by && cp->ssym && !dfe_shift_symref(d, cp->spool, by))
      continue;
    if (by)
    {
      IROperand *dp = &ir->iroperand_pool[cp->dpool];
      dfe_set_off(dp, irop_get_stack_offset(*dp) + by);
      if (cp->kind == DFE_COPY && !cp->ssym)
      {
        IROperand *sp = &ir->iroperand_pool[cp->spool];
        dfe_set_off(sp, irop_get_stack_offset(*sp) + by);
      }
    }
    IROperand *np = &ir->iroperand_pool[cp->npool];
    *np = irop_make_imm32(-1, nn, np->btype == IROP_BTYPE_STRUCT ? IROP_BTYPE_INT32 : np->btype);
    cp->doff += by;
    cp->soff += by;
    cp->n = nn;
    (*shrunk)++;
    changes++;
  }
  return changes;
}

/* ---- image forwarding ---------------------------------------------------- */

/* `memmove(&D + dk, &S + sk, n)` where S is built just before it -- a .rodata
 * image, a fill, stores, copies, or a callee's struct result -- in the same
 * straight run of code, and read by nothing else: those writes go to D
 * directly and S and the copy go.  The Zig C backend builds every
 * `error.X` result this way: the payload's 0xaa image in a temporary, then a
 * copy into the error union (fatfs.File.open: 552 bytes twice over).
 *
 * D is a candidate (nothing reaches it but its own references), so nothing in
 * the run may name it -- then whatever runs there, D's old bytes cannot be
 * observed between the write's new place and the copy's.  The writes must
 * cover S's copied range and stay inside it, and move by a multiple of 4
 * (8 for an 8-aligned helper) so the alignment each promised still holds. */
static int dfe_block_end(int op)
{
  switch (op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SWITCH_LOAD:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_BUILTIN_APPLY:
  case TCCIR_OP_BUILTIN_RETURN:
  case TCCIR_OP_INLINE_ASM:
    return 1;
  default:
    return 0;
  }
}

/* Does instruction j name object o (directly or through a followed TEMP)? */
static int dfe_names(Dfe *d, int j, int o)
{
  TCCIRState *ir = d->ir;
  IRQuadCompact *q = &ir->compact_instructions[j];
  const int nops = dfe_nops(q->op);
  for (int k = 0; k < nops; k++)
  {
    IROperand a = ir->iroperand_pool[q->operand_base + k];
    int off, t;
    if (dfe_frame_off(a, &off) && off >= d->obj[o].start && off < d->obj[o].start + d->obj[o].span)
      return 1;
    if ((t = dfe_temp_index(d, irop_get_vreg(a))) >= 0 && d->tobj[t] == o)
      return 1;
  }
  return 0;
}

/* The pool index of the operand that names the write's destination place,
 * and whether moving it is safe (the TEMP carrying it has no other reader). */
static int dfe_write_pool(Dfe *d, DfeAcc *a, int *unit)
{
  TCCIRState *ir = d->ir;
  *unit = 4;
  if (a->kind == DFE_STORE)
    return a->pool;
  if (a->kind == DFE_FIXED)
    return dfe_temp_movable(d, a->temp) ? a->pool : -1;
  DfeCopy *cp = &d->cp[a->copy];
  if (cp->keep || !dfe_temp_movable(d, cp->dtemp))
    return -1;
  if (cp->kind != DFE_BC)
  {
    const char *nm = dfe_callee(ir, &ir->compact_instructions[cp->call]);
    size_t ln = nm ? strlen(nm) : 0;
    if (ln && nm[ln - 1] == '8')
      *unit = 8;
  }
  return cp->dpool;
}

static int dfe_forward_images(Dfe *d)
{
  TCCIRState *ir = d->ir;
  int done = 0;
  int *head = tcc_malloc(sizeof(int) * (d->nobj > 0 ? d->nobj : 1)), *next = tcc_malloc(sizeof(int) * (d->nacc + 1));
  for (int o = 0; o < d->nobj; o++)
    head[o] = -1;
  for (int x = d->nacc - 1; x >= 0; x--)
  {
    next[x] = head[d->acc[x].obj];
    head[d->acc[x].obj] = x;
  }
  uint8_t *used = tcc_mallocz(d->nobj > 0 ? d->nobj : 1); /* one rewrite per object per round */
  for (int c = 0; c < d->ncp; c++)
  {
    DfeCopy *cp = &d->cp[c];
    const int S = cp->sobj, D = cp->dobj;
    if (cp->kind != DFE_COPY || S < 0 || D < 0 || S == D || cp->keep || d->obj[S].escaped || d->obj[D].escaped ||
        used[S] || used[D] || (d->obj[D].start & 3))
      continue;
    const int call = cp->call;
    /* S: written, then read by this copy alone */
    int first = call, ok = 1, nw = 0;
    for (int x = head[S]; x >= 0 && ok; x = next[x])
    {
      DfeAcc *a = &d->acc[x];
      if (a->kind == DFE_CSRC && a->copy == c)
        continue;
      if (a->kind != DFE_STORE && a->kind != DFE_BC && a->kind != DFE_SET && a->kind != DFE_COPY &&
          a->kind != DFE_FIXED)
      {
        ok = 0;
        break;
      }
      int lo = a->lo, hi = a->hi;
      if (a->copy >= 0)
        lo = d->cp[a->copy].doff, hi = lo + d->cp[a->copy].n;
      if (a->i >= call || lo < cp->soff || hi > cp->soff + cp->n)
        ok = 0;
      int unit, pool = dfe_write_pool(d, a, &unit);
      if (pool < 0)
        ok = 0;
      int from = a->kind == DFE_STORE || a->kind == DFE_BC ? a->i : d->first_param[a->i];
      if (from < first)
        first = from;
      nw++;
    }
    if (!ok || !nw)
      continue;
    const int delta = (d->obj[D].start + cp->doff) - (d->obj[S].start + cp->soff);
    if (delta & 3)
      continue;
    /* the writes cover the copied range */
    uint8_t *cov = tcc_mallocz(cp->n);
    for (int x = head[S]; x >= 0; x = next[x])
    {
      DfeAcc *a = &d->acc[x];
      if (a->kind == DFE_CSRC)
        continue;
      int lo = a->lo, hi = a->hi, unit;
      if (a->copy >= 0)
        lo = d->cp[a->copy].doff, hi = lo + d->cp[a->copy].n;
      int pool = dfe_write_pool(d, a, &unit);
      if (delta % unit)
        ok = 0;
      if (pool >= 0 && !dfe_off_fits(ir->iroperand_pool[pool], irop_get_stack_offset(ir->iroperand_pool[pool]) + delta))
        ok = 0;
      for (int b = lo; b < hi; b++)
        cov[b - cp->soff] = 1;
    }
    for (int b = 0; b < cp->n && ok; b++)
      ok = cov[b];
    tcc_free(cov);
    if (!ok)
      continue;
    /* one straight run from the first write to the copy, D untouched in it
     * but by the copy's own arguments and the TEMP carrying its address */
    for (int j = first; j <= call && ok; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (j > first && q->is_jump_target)
        ok = 0;
      if (q->op == TCCIR_OP_NOP || j == call)
        continue;
      if (dfe_block_end(q->op))
        ok = 0;
      else if ((q->op == TCCIR_OP_FUNCPARAMVAL && d->param_call[j] == call) ||
               (cp->dtemp >= 0 && d->tdef[cp->dtemp] == j))
        continue;
      else if (dfe_names(d, j, D))
        ok = 0;
    }
    if (!ok)
      continue;
    /* move the writes onto D */
    for (int x = head[S]; x >= 0; x = next[x])
    {
      DfeAcc *a = &d->acc[x];
      if (a->kind == DFE_CSRC)
        continue;
      int unit, pool = dfe_write_pool(d, a, &unit);
      IROperand *op = &ir->iroperand_pool[pool];
      dfe_set_off(op, irop_get_stack_offset(*op) + delta);
    }
    dfe_nop_call(d, call);
    if (cp->dtemp >= 0)
      d->tuses[cp->dtemp]--;
    if (cp->stemp >= 0)
      d->tuses[cp->stemp]--;
    cp->gone = 1;
    used[S] = used[D] = 1;
    done++;
  }
  tcc_free(used);
  tcc_free(head);
  tcc_free(next);
  return done;
}

/* ---- step 4: pieces ------------------------------------------------------- */

static int dfe_range_cmp(const void *a, const void *b)
{
  const int *x = a, *y = b;
  return x[0] != y[0] ? (x[0] < y[0] ? -1 : 1) : (x[1] < y[1] ? -1 : x[1] > y[1]);
}

static void dfe_add_rec(TCCIRState *ir, int start, int size, int flags, int scope_start)
{
  if (ir->frame_obj_count == ir->frame_obj_cap)
  {
    ir->frame_obj_cap = ir->frame_obj_cap ? ir->frame_obj_cap * 2 : 32;
    ir->frame_objs = tcc_realloc(ir->frame_objs, sizeof(int32_t) * FRAME_OBJ_WORDS * ir->frame_obj_cap);
  }
  int32_t *o = &ir->frame_objs[FRAME_OBJ_WORDS * ir->frame_obj_count++];
  o[0] = start;
  o[1] = size;
  o[2] = size;
  o[3] = flags;
  o[4] = scope_start;
}

static int dfe_split(Dfe *d)
{
  TCCIRState *ir = d->ir;
  int pieces = 0;
  int *rng = NULL, cap = 0;
  /* accesses grouped by object */
  int *head = tcc_malloc(sizeof(int) * (d->nobj > 0 ? d->nobj : 1)), *next = tcc_malloc(sizeof(int) * (d->nacc + 1));
  for (int o = 0; o < d->nobj; o++)
    head[o] = -1;
  for (int x = d->nacc - 1; x >= 0; x--)
  {
    next[x] = head[d->acc[x].obj];
    head[d->acc[x].obj] = x;
  }
  for (int o = 0; o < d->nobj; o++)
  {
    DfeObj *ob = &d->obj[o];
    if (ob->escaped || head[o] < 0 || ob->span < 16)
      continue;
    int nr = 0;
    for (int x = head[o]; x >= 0; x = next[x])
    {
      DfeAcc *a = &d->acc[x];
      int lo = a->lo, hi = a->hi;
      if (a->dead)
        continue;
      int base = a->base;
      if (a->copy >= 0)
      {
        DfeCopy *cp = &d->cp[a->copy];
        if (cp->gone)
          continue;
        if (a->kind == DFE_CSRC)
          lo = cp->soff, hi = cp->soff + cp->n, base = cp->sbase;
        else
          lo = cp->doff, hi = cp->doff + cp->n, base = cp->dbase;
      }
      /* reached through a pointer: the place it was taken from moves with it */
      if (base >= 0 && base < lo)
        lo = base;
      if (base >= 0 && base + 1 > hi)
        hi = base + 1;
      lo &= ~7;
      hi = (hi + 7) & ~7;
      if (hi > ob->span)
        hi = ob->span;
      if (nr + 1 > cap)
      {
        cap = cap ? 2 * cap : 64;
        rng = tcc_realloc(rng, sizeof(int) * 2 * cap);
      }
      rng[2 * nr] = lo;
      rng[2 * nr + 1] = hi;
      nr++;
    }
    /* no reference left: relayout drops the whole object as it is */
    if (nr == 0)
      continue;
    tcc_qsort(rng, nr, sizeof(int) * 2, dfe_range_cmp);
    int m = 0;
    for (int r = 0; r < nr; r++)
    {
      if (m && rng[2 * r] < rng[2 * m - 1])
      {
        if (rng[2 * r + 1] > rng[2 * m - 1])
          rng[2 * m - 1] = rng[2 * r + 1];
        continue;
      }
      rng[2 * m] = rng[2 * r];
      rng[2 * m + 1] = rng[2 * r + 1];
      m++;
    }
    int covered = 0;
    for (int r = 0; r < m; r++)
      covered += rng[2 * r + 1] - rng[2 * r];
    if (covered >= ob->span)
      continue;
    if (dfe_dbg())
    {
      fprintf(stderr, "[DFE]   %s obj %d +%d:", funcname, ob->start, ob->span);
      for (int r = 0; r < m; r++)
        fprintf(stderr, " [%d,%d)", rng[2 * r], rng[2 * r + 1]);
      fprintf(stderr, "\n");
    }
    /* this record becomes the first range; the rest are appended */
    int32_t *rec = &ir->frame_objs[FRAME_OBJ_WORDS * ob->rec];
    const int flags = rec[3], scope_start = rec[4], base = ob->start;
    int at = 0, first = 1;
    for (int r = 0; r <= m; r++)
    {
      int lo = r < m ? rng[2 * r] : ob->span, hi = r < m ? rng[2 * r + 1] : ob->span;
      if (lo > at)
      {
        /* a gap nobody names */
        if (first)
        {
          rec = &ir->frame_objs[FRAME_OBJ_WORDS * ob->rec];
          rec[0] = base + at, rec[1] = rec[2] = lo - at, rec[3] = flags;
          first = 0;
        }
        else
          dfe_add_rec(ir, base + at, lo - at, flags, scope_start);
      }
      if (hi > lo)
      {
        if (first)
        {
          rec = &ir->frame_objs[FRAME_OBJ_WORDS * ob->rec];
          rec[0] = base + lo, rec[1] = rec[2] = hi - lo, rec[3] = flags;
          first = 0;
        }
        else
          dfe_add_rec(ir, base + lo, hi - lo, flags, scope_start);
        pieces++;
      }
      at = hi;
    }
  }
  tcc_free(rng);
  tcc_free(head);
  tcc_free(next);
  if (pieces)
    ir->frame_index_objs = -1; /* tcc_ir_frame_object_at rebuilds */
  return pieces;
}

/* ---- undefined values ----------------------------------------------------- */

/* TCC_DFE_UNDEF_DBG: one line per function that lost stores of undefined values. */
TCC_DBG_ENV_FLAG(dfe_undef_dbg, "TCC_DFE_UNDEF_DBG")

/* A TEMP no instruction defines holds an undefined value.  The Zig C backend
 * builds a tagged value in a temporary whose payload union is wider than the
 * member written, then moves the whole temporary on (`t13 = t10; t8 = t13;
 * arr[i] = t8`): the words past the member were never written, so once the
 * temporaries are split into words they are vregs with no definition, and the
 * stores into the argument array copy a garbage register (and cost the
 * callee-saved register that register ended up in).
 *
 * A store of such a value leaves the destination holding a value the program
 * may not rely on, which is what the destination held already: it goes.
 * Only TEMPs whose EVERY reference is the value of a plain STORE qualify --
 * nothing defines them, nothing else reads them -- so no use sees a value
 * other than the one it was going to read anyway.  A volatile destination
 * keeps its store, and a function with inline asm is left alone (an asm
 * output defines a TEMP without any instruction naming it). */
static int dfe_undef_stores(TCCIRState *ir)
{
  if (!ir || !tcc_state || TCC_OPT(tcc_state, optimize) <= 0 || tcc_state->do_debug || tcc_bounds_checking(tcc_state) ||
      ir->inline_asm_count || tcc_ir_opt_pass_disabled("frame_undef"))
    return 0;
  const int n = ir->next_instruction_index;
  const int ntemp = ir->next_temporary_variable + 1;
  if (n <= 0 || ntemp <= 0)
    return 0;
  uint8_t *other = tcc_mallocz(ntemp);
  uint8_t *stores = tcc_mallocz(ntemp);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    const int nops = dfe_nops(q->op);
    for (int k = 0; k < nops; k++)
    {
      IROperand op = ir->iroperand_pool[q->operand_base + k];
      int32_t vr = irop_get_vreg(op);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int t = TCCIR_DECODE_VREG_POSITION(vr);
      if (t >= ntemp)
        continue;
      if (q->op == TCCIR_OP_STORE && k == 1 && irop_get_tag(op) == IROP_TAG_VREG && !op.is_lval && !op.is_llocal &&
          !op.is_local && op.btype != IROP_BTYPE_STRUCT)
        stores[t] = 1;
      else
        other[t] = 1;
    }
  }
  int deleted = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand val = tcc_ir_op_get_src1(ir, q), dst = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(val);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || irop_get_tag(val) != IROP_TAG_VREG ||
        val.is_lval || val.is_llocal || val.is_local || val.btype == IROP_BTYPE_STRUCT)
      continue;
    int t = TCCIR_DECODE_VREG_POSITION(vr);
    if (t >= ntemp || other[t] || !stores[t] || dst.is_llocal || tcc_ir_access_is_volatile(ir, dst))
      continue;
    q->op = TCCIR_OP_NOP;
    deleted++;
  }
  tcc_free(other);
  tcc_free(stores);
  if (deleted && dfe_undef_dbg())
    fprintf(stderr, "[DFE-UNDEF] %s: %d stores of undefined values\n", funcname, deleted);
  return deleted;
}

/* A LOAD of frame bytes nothing writes yields an undefined value too: the
 * temporary is an object (its address used, or a payload that SRA left in
 * memory) and the union's tail is read back out of it word by word, to be
 * stored into the next copy.  The loads that only feed such stores go, which
 * leaves their TEMPs defined nowhere, and dfe_undef_stores then drops the
 * stores.
 *
 * "Written" is whatever may put a byte there: a store, a fill, an image, a
 * callee's result, a copy -- and any access this pass cannot tell from one (an
 * access through a pointer, a destination operand of any other op).  An
 * escaped object has no accesses recorded and is skipped: something may have
 * written it through its address.  Volatile accesses escape in dfe_scan. */
static int dfe_undef_loads(Dfe *d)
{
  TCCIRState *ir = d->ir;
  const int n = d->n;
  if (ir->inline_asm_count || tcc_ir_opt_pass_disabled("frame_undef"))
    return 0;
  /* the bytes that may have been written, per object */
  uint8_t **wr = tcc_mallocz(sizeof(uint8_t *) * (d->nobj > 0 ? d->nobj : 1));
  for (int o = 0; o < d->nobj; o++)
    wr[o] = tcc_mallocz(d->obj[o].span > 0 ? d->obj[o].span : 1);
  int anyload = 0;
  for (int x = 0; x < d->nacc; x++)
  {
    const DfeAcc *a = &d->acc[x];
    if (a->dead || d->obj[a->obj].escaped)
      continue;
    int pure = a->kind == DFE_CSRC;
    if (a->kind == DFE_READ && a->pool >= 0 && a->base < 0)
    {
      const IRQuadCompact *q = &ir->compact_instructions[a->i];
      pure = !(irop_config[q->op].has_dest && a->pool == (int)q->operand_base);
      anyload |= pure && q->op == TCCIR_OP_LOAD;
    }
    if (pure)
      continue;
    for (int b = a->lo; b < a->hi && b < d->obj[a->obj].span; b++)
      wr[a->obj][b] = 1;
  }
  int deleted = 0;
  if (anyload)
  {
    /* how each TEMP is referenced: by the value of a plain STORE, or otherwise */
    uint8_t *stores = tcc_mallocz(d->ntemp);
    int *other = tcc_mallocz(sizeof(int) * d->ntemp);
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      const int nops = dfe_nops(q->op);
      for (int k = 0; k < nops; k++)
      {
        IROperand op = ir->iroperand_pool[q->operand_base + k];
        int t = dfe_temp_index(d, irop_get_vreg(op));
        if (t < 0)
          continue;
        if (q->op == TCCIR_OP_STORE && k == 1 && irop_get_tag(op) == IROP_TAG_VREG && !op.is_lval && !op.is_llocal &&
            !op.is_local && op.btype != IROP_BTYPE_STRUCT)
          stores[t] = 1;
        else
          other[t]++;
      }
    }
    for (int x = 0; x < d->nacc; x++)
    {
      DfeAcc *a = &d->acc[x];
      if (a->dead || a->kind != DFE_READ || a->pool < 0 || a->base >= 0 || d->obj[a->obj].escaped)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[a->i];
      if (q->op != TCCIR_OP_LOAD || a->pool != (int)q->operand_base + 1)
        continue;
      IROperand dst = tcc_ir_op_get_dest(ir, q), src = ir->iroperand_pool[a->pool];
      const int w = dfe_width(src);
      int t = dfe_temp_index(d, irop_get_vreg(dst));
      if (t < 0 || d->tdef[t] != a->i || dst.is_lval || irop_get_tag(dst) != IROP_TAG_VREG || !stores[t] ||
          other[t] != 1 || !src.is_lval || tcc_ir_access_is_volatile(ir, src) || w <= 0 || a->hi - a->lo != w)
        continue;
      int clean = 1;
      for (int b = a->lo; b < a->hi && clean; b++)
        clean = !wr[a->obj][b];
      if (!clean)
        continue;
      q->op = TCCIR_OP_NOP;
      a->dead = 1;
      deleted++;
    }
    tcc_free(stores);
    tcc_free(other);
  }
  for (int o = 0; o < d->nobj; o++)
    tcc_free(wr[o]);
  tcc_free(wr);
  if (deleted && dfe_undef_dbg())
    fprintf(stderr, "[DFE-UNDEF] %s: %d loads of unwritten frame bytes\n", funcname, deleted);
  return deleted;
}

/* ---- driver ------------------------------------------------------------- */

int tcc_ir_frame_dead_bytes(TCCIRState *ir, int *inserted)
{
  *inserted = 0;
  if (!ir)
    return 0;
  const int undef = dfe_undef_stores(ir);
  if (!dfe_eligible(ir) || tcc_ir_opt_pass_disabled("frame_dfe"))
    return undef;
  Dfe d = {0};
  d.ir = ir;
  d.cur = -1;
  d.n = ir->next_instruction_index;
  const int n = d.n;

  /* objects */
  d.obj = tcc_mallocz(sizeof(DfeObj) * ir->frame_obj_count);
  for (int k = 0; k < ir->frame_obj_count; k++)
  {
    const int32_t *o = &ir->frame_objs[FRAME_OBJ_WORDS * k];
    d.obj[d.nobj++] = (DfeObj){.start = o[0], .span = o[1], .rec = k};
  }
  tcc_qsort(d.obj, d.nobj, sizeof(DfeObj), dfe_obj_cmp);
  for (int k = 0; k < d.nobj; k++)
  {
    DfeObj *ob = &d.obj[k];
    if (ob->span <= 0 || ob->span > DFE_MAX_SPAN || ob->start + ob->span > 0 || (func_vc && ob->start <= func_vc &&
                                                                                  func_vc < ob->start + ob->span))
      ob->escaped = 1;
    if (k && ob->start < d.obj[k - 1].start + d.obj[k - 1].span)
      ob->escaped = d.obj[k - 1].escaped = 1; /* objects sharing bytes */
  }
  /* the overlap marking above only reaches the neighbour; spread it */
  for (int k = 0; k < d.nobj; k++)
    for (int j = k + 1; j < d.nobj && d.obj[j].start < d.obj[k].start + d.obj[k].span; j++)
      d.obj[k].escaped = d.obj[j].escaped = 1;

  /* calls and their parameters */
  const int ncall = ir->next_call_id + 1;
  int *call_at = tcc_malloc(sizeof(int) * (ncall > 0 ? ncall : 1));
  for (int c = 0; c < ncall; c++)
    call_at[c] = -1;
  d.param_call = tcc_malloc(sizeof(int) * (n > 0 ? n : 1));
  d.call_param = tcc_malloc(sizeof(int) * 4 * (n > 0 ? n : 1));
  d.call_cp = tcc_malloc(sizeof(int) * (n > 0 ? n : 1));
  int ok = 1;
  for (int i = n - 1; i >= 0 && ok; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    d.param_call[i] = -1;
    d.call_cp[i] = -1;
    for (int k = 0; k < 4; k++)
      d.call_param[4 * i + k] = -1;
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      int c = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
      if (c < 0 || c >= ncall)
        ok = 0;
      else
        call_at[c] = i;
    }
    else if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      uint32_t enc = (uint32_t)tcc_ir_op_src2_imm(ir, q);
      int c = TCCIR_DECODE_CALL_ID(enc), idx = TCCIR_DECODE_PARAM_IDX(enc);
      if (c < 0 || c >= ncall)
        ok = 0;
      else if (call_at[c] >= 0)
      {
        d.param_call[i] = call_at[c];
        if (idx < 4 && d.call_param[4 * call_at[c] + idx] < 0)
          d.call_param[4 * call_at[c] + idx] = i;
      }
    }
  }
  tcc_free(call_at);

  int deleted = 0, shrunk = 0, pieces = 0, changes = 0, forwarded = 0, rebased = 0, rebase_done = 0;
  int undef_done = 0, undef_loads = 0;
  if (ok)
  {
    d.first_param = tcc_malloc(sizeof(int) * (n > 0 ? n : 1));
    for (int i = 0; i < n; i++)
      d.first_param[i] = i;
    for (int i = 0; i < n; i++)
      if (d.param_call[i] >= 0 && i < d.first_param[d.param_call[i]])
        d.first_param[d.param_call[i]] = i;
    d.ntemp = ir->next_temporary_variable + 1;
    d.tdef = tcc_malloc(sizeof(int) * d.ntemp);
    d.tuses = tcc_malloc(sizeof(int) * d.ntemp);
    d.tobj = tcc_malloc(sizeof(int) * d.ntemp);
    d.trel = tcc_malloc(sizeof(int) * d.ntemp);
    d.troot = tcc_malloc(sizeof(int) * d.ntemp);
    for (int o = 0; o < d.nobj; o++)
    {
      d.obj[o].escaped0 = d.obj[o].escaped;
      d.obj[o].live = tcc_malloc(d.obj[o].span > 0 ? d.obj[o].span : 1);
    }
    for (int round = 0; round < 4; round++)
    {
      /* (re)analyse the IR as it now stands */
      for (int t = 0; t < d.ntemp; t++)
        d.tdef[t] = -1, d.tobj[t] = -1, d.tuses[t] = 0, d.trel[t] = 0, d.troot[t] = 0;
      for (int o = 0; o < d.nobj; o++)
      {
        d.obj[o].escaped = d.obj[o].escaped0;
        memset(d.obj[o].live, 0, d.obj[o].span > 0 ? d.obj[o].span : 1);
      }
      for (int i = 0; i < n; i++)
        d.call_cp[i] = -1;
      d.nacc = d.ncp = 0;
      dfe_scan(&d);
      /* indexed accesses named from their lowest index, then the IR as
       * that left it (nothing is escaped by the rebase) */
      if (!rebase_done)
      {
        rebase_done = 1;
        if (!ir->inline_asm_count && (rebased = dfe_rebase_indexed(&d)) > 0)
        {
          round--;
          continue;
        }
      }
      /* loads of bytes nothing writes, and the stores of what they loaded */
      if (!undef_done)
      {
        undef_done = 1;
        if ((undef_loads = dfe_undef_loads(&d)) > 0)
        {
          dfe_undef_stores(ir);
          round--;
          continue;
        }
      }
      if (round == 3 || dfe_fwd_off())
        break;
      int f = dfe_forward_images(&d);
      if (!f)
        break;
      forwarded += f;
    }
    dfe_liveness(&d);
    changes = dfe_rewrite(&d, &deleted, &shrunk) + forwarded + rebased + undef_loads;
    /* address TEMPs nothing reads any more, and the ones they came from */
    for (int again = 1; changes && again;)
    {
      again = 0;
      for (int t = 0; t < d.ntemp; t++)
      {
        if (d.tdef[t] < 0 || d.tuses[t] != 0 || d.tobj[t] < 0)
          continue;
        IRQuadCompact *q = &ir->compact_instructions[d.tdef[t]];
        if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
          continue;
        int st = dfe_temp_index(&d, tcc_ir_op_src1_vreg(ir, q));
        q->op = TCCIR_OP_NOP;
        d.tdef[t] = -1;
        if (st >= 0 && d.tuses[st] > 0)
          d.tuses[st]--, again = 1;
      }
    }
    pieces = dfe_split(&d);
    if (d.nlow)
      *inserted = dfe_insert_words(&d);
    for (int o = 0; o < d.nobj; o++)
      tcc_free(d.obj[o].live);
    tcc_free(d.tdef);
    tcc_free(d.tuses);
    tcc_free(d.tobj);
    tcc_free(d.trel);
    tcc_free(d.troot);
    tcc_free(d.first_param);
  }
  if (dfe_dbg() && (changes || pieces))
    fprintf(stderr, "[DFE] %s: %d forwarded, %d deleted, %d shrunk, %d pieces\n", funcname, forwarded, deleted, shrunk,
            pieces);
  if (dfe_esc_dbg())
    for (int o = 0; o < d.nobj; o++)
      if (d.obj[o].escaped)
        fprintf(stderr, "[DFE-ESC] %s obj %d +%d line %d op %s\n", funcname, d.obj[o].start, d.obj[o].span,
                d.obj[o].esc_why,
                d.obj[o].esc_at >= 0 && d.obj[o].esc_at < n
                    ? tcc_ir_get_op_name((TccIrOp)ir->compact_instructions[d.obj[o].esc_at].op)
                    : "-");
  tcc_free(d.obj);
  tcc_free(d.acc);
  tcc_free(d.cp);
  tcc_free(d.param_call);
  tcc_free(d.call_param);
  tcc_free(d.call_cp);
  tcc_free(d.low);
  return changes + pieces + undef;
}
