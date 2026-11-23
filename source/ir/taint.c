/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

/*
 * -Wtainted-format: a printf-style format that comes from untrusted input,
 * found on the raw IR of each function before any pass (as ir/lifetime.c).
 *
 * Sources: main's argv (what is read through it), the result of getenv /
 * secure_getenv, the buffer that fgets, gets, fread, read, pread, recv,
 * recvfrom, getline and getdelim fill, and a function declared with
 * __attribute__((tainted_result)) (its result) or tainted_arg(n) (what its
 * n-th argument points to, from 1, after the call).  Sinks: the format
 * argument of a format(printf, n, m) function, or of the printf family by
 * name when it has no attribute.  memcpy, memmove, strcpy, strncpy, stpcpy,
 * strcat, strncat, strlcpy, strlcat, strdup and strndup carry what they copy.
 *
 * The facts are masks: bit 0 "untrusted", bit 1 + j "what parameter j held
 * at the entry" (j < TM_NPARAM), so that one walk both reports and gives the
 * function's summary.  Kept for every variable -- local or parameter --
 * "holds or points to it", forward over the CFG (a join ORs); a pointer
 * variable and what it points to share theirs, unless every assignment of
 * the variable is an address into one object (`p = buf + 2`, `q = &s`):
 * then it is that object's alias.  Memory is followed by object -- a local
 * array or struct of the frame, a global, what a pointer variable points to
 * -- as a "whole" fact plus member paths at constant offsets from it
 * (lifetime.c's base + offset): a store at a known offset replaces that
 * path's fact (`s.fmt = getenv(...)` taints s.fmt, not s.name), a buffer
 * filled from an offset taints the object from there on, an offset that is
 * not constant reaches every path from its lower bound on.  Temps (single-
 * block values, mostly) take one mask for the whole function and the object
 * they point into.
 *
 * Across the functions of the unit: each function's summary says which
 * parameters' facts (and which sources) reach its result, what its pointer
 * parameters point to and the globals it writes, and which parameters reach
 * a format (with the sink's place).  The functions are kept in a compact
 * form until the end of the unit, where the summaries are iterated to a
 * fixpoint (a callee defined after its caller counts), then every function
 * is walked once more to report.  A warning about a format reached through
 * a call is said at the sink, with a note at the call.  An inline expansion
 * (-O1+) is the call it replaces for this: calls inside it are neither
 * sources nor sinks nor summaries -- the expanded function's summary is
 * applied where its body ends -- so the result does not depend on -O.
 * Bounded: TU_MAX_BYTES of kept functions (a function past it is checked at
 * once with the summaries known then), TF_MAX_LOCS memory facts and
 * TS_MAX_ENT summary entries (more fall back to whole objects), no walk
 * whose state passes TF_MAX_STATE bytes.
 *
 * Not followed: other units, memory through pointers of unknown origin,
 * indirect calls, scanf %s buffers, struct copies made by a helper call
 * (`s = *p` of a large struct), what a summary writes through a parameter
 * past the first TM_NPARAM.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"

#define TM_SRC 1u
#define TM_NPARAM 7
#define TM_PARAM(j) ((uint8_t)(1u << ((j) + 1)))

#define TU_MAX_BYTES (16u << 20)
#define TF_MAX_LOCS 256
#define TF_MAX_STATE (32u << 20)
#define TS_MAX_ENT 16
#define TF_MAX_ARGS 32
#define TF_OFF_ANY INT32_MIN

/* a call's facts, by call_id (tcc_ir_taint_note_call) */
struct IRTaintCall
{
  int callee;         /* token, or 0 */
  uint32_t taint_arg; /* what argument k points to is untrusted after it */
  unsigned char taint_ret, fmt_idx, noted;
  signed char copy_dst, copy_src; /* memcpy & co: what src points to goes where dst points */
};

/* an inline expansion (outermost), a call for the summaries */
struct IRTaintInl
{
  int callee, end, ret_vreg, nparams, line;
  int32_t pvreg[TM_NPARAM], pfo[TM_NPARAM]; /* the parameter's vreg, else its frame offset (1: none) */
};

int tcc_ir_taint_active(void)
{
  return tcc_state->ir && (tcc_state->warn_tainted_format & WARN_ON) && !tcc_state->warn_none;
}

void tcc_ir_taint_begin(TCCIRState *ir)
{
  ir->taint_on = tcc_ir_taint_active();
}

/* the C library's sources, printf family and copies, for declarations
   without the attributes */
static const struct
{
  const char *name;
  /* result untrusted; argument filled (from 0, -1 none); format (from 1, 0
     none); copy into dst from src (from 0, -1 none) */
  signed char ret, arg, fmt, dst, src;
} taint_known[] = {
    {"getenv", 1, -1, 0, -1, -1},    {"secure_getenv", 1, -1, 0, -1, -1}, {"fgets", 0, 0, 0, -1, -1},
    {"gets", 0, 0, 0, -1, -1},       {"fread", 0, 0, 0, -1, -1},          {"read", 0, 1, 0, -1, -1},
    {"pread", 0, 1, 0, -1, -1},      {"recv", 0, 1, 0, -1, -1},           {"recvfrom", 0, 1, 0, -1, -1},
    {"getline", 0, 0, 0, -1, -1},    {"getdelim", 0, 0, 0, -1, -1},       {"printf", 0, -1, 1, -1, -1},
    {"fprintf", 0, -1, 2, -1, -1},   {"dprintf", 0, -1, 2, -1, -1},       {"sprintf", 0, -1, 2, -1, -1},
    {"snprintf", 0, -1, 3, -1, -1},  {"vprintf", 0, -1, 1, -1, -1},       {"vfprintf", 0, -1, 2, -1, -1},
    {"vsprintf", 0, -1, 2, -1, -1},  {"vsnprintf", 0, -1, 3, -1, -1},     {"syslog", 0, -1, 2, -1, -1},
    {"asprintf", 0, -1, 2, -1, -1},  {"vasprintf", 0, -1, 2, -1, -1},     {"memcpy", 0, -1, 0, 0, 1},
    {"memmove", 0, -1, 0, 0, 1},     {"strcpy", 0, -1, 0, 0, 1},          {"strncpy", 0, -1, 0, 0, 1},
    {"stpcpy", 0, -1, 0, 0, 1},      {"strcat", 0, -1, 0, 0, 1},          {"strncat", 0, -1, 0, 0, 1},
    {"strlcpy", 0, -1, 0, 0, 1},     {"strlcat", 0, -1, 0, 0, 1},         {"strdup", 0, -1, 0, -1, 0},
    {"strndup", 0, -1, 0, -1, 0},
};

void tcc_ir_taint_note_call(TCCIRState *ir, int call_id, Sym *callee)
{
  struct IRTaintCall *c;
  const SymLocalFacts *f;
  if (call_id < 0 || !callee || tcc_state->in_inline_expansion)
    return;
  if (call_id >= ir->taint_nb_calls)
  {
    int n = call_id + 16;
    ir->taint_calls = tcc_realloc(ir->taint_calls, n * sizeof *c);
    memset(ir->taint_calls + ir->taint_nb_calls, 0, (n - ir->taint_nb_calls) * sizeof *c);
    ir->taint_nb_calls = n;
  }
  c = &ir->taint_calls[call_id];
  c->callee = callee->v;
  c->copy_dst = c->copy_src = -1;
  f = callee->a.attr_facts ? sym_facts_peek(callee) : NULL;
  if (f)
  {
    c->taint_ret = f->tainted_result;
    c->taint_arg = f->tainted_args;
    if (f->fmt_kind == ATTR_FMT_PRINTF)
      c->fmt_idx = f->fmt_idx;
  }
  if (!c->taint_ret && !c->taint_arg && !c->fmt_idx)
  {
    const char *name = get_tok_str(callee->v, NULL);
    for (size_t i = 0; i < sizeof taint_known / sizeof taint_known[0]; i++)
      if (!strcmp(name, taint_known[i].name))
      {
        c->taint_ret = taint_known[i].ret;
        c->taint_arg = taint_known[i].arg >= 0 ? 1u << taint_known[i].arg : 0;
        c->fmt_idx = taint_known[i].fmt;
        c->copy_dst = taint_known[i].dst;
        c->copy_src = taint_known[i].src;
        break;
      }
  }
  c->noted = c->taint_ret || c->taint_arg || c->fmt_idx || c->copy_src >= 0;
  if (c->taint_ret || c->taint_arg)
    ir->taint_sources = 1;
}

/* a local the frame holds (an array, a struct, a scalar whose address is
   taken): its extent, so that its memory is an object of its own */
void tcc_ir_taint_add_local(TCCIRState *ir, Sym *sym)
{
  int align, size = type_size(&sym->type, &align);
  if (size <= 0)
    return;
  if (!(ir->taint_nb_fobjs & (ir->taint_nb_fobjs - 1)))
    ir->taint_fobjs = tcc_realloc(ir->taint_fobjs, (ir->taint_nb_fobjs ? 4 * ir->taint_nb_fobjs : 8) * sizeof(int));
  ir->taint_fobjs[2 * ir->taint_nb_fobjs] = (int)sym->c;
  ir->taint_fobjs[2 * ir->taint_nb_fobjs + 1] = size;
  ir->taint_nb_fobjs++;
}

/* an inline expansion's parameters, then its start and end */
static struct
{
  int n;
  int32_t vreg[TM_NPARAM], fo[TM_NPARAM];
} taint_pend;

void tcc_ir_taint_inline_param(TCCIRState *ir, int index, Sym *psym)
{
  (void)ir;
  if (index == 0)
    taint_pend.n = 0;
  if (index != taint_pend.n || index >= TM_NPARAM)
    return;
  taint_pend.vreg[index] = psym->vreg;
  taint_pend.fo[index] = psym->vreg >= 0 ? 1 : (int32_t)psym->c;
  taint_pend.n++;
}

void tcc_ir_taint_inline_mark(TCCIRState *ir, int begin, Sym *callee, int ret_vreg)
{
  struct IRTaintInl *x;
  if (begin)
  {
    if (!(ir->taint_nb_inl & (ir->taint_nb_inl - 1)))
      ir->taint_inl = tcc_realloc(ir->taint_inl, (ir->taint_nb_inl ? 2 * ir->taint_nb_inl : 4) * sizeof *x);
    x = &ir->taint_inl[ir->taint_nb_inl++];
    memset(x, 0, sizeof *x);
    x->callee = callee ? callee->v : 0;
    x->end = -1;
    x->ret_vreg = -1;
    x->nparams = taint_pend.n;
    x->line = file ? file->line_num : 0;
    memcpy(x->pvreg, taint_pend.vreg, sizeof x->pvreg);
    memcpy(x->pfo, taint_pend.fo, sizeof x->pfo);
    taint_pend.n = 0;
    return;
  }
  if (!ir->taint_nb_inl || (x = &ir->taint_inl[ir->taint_nb_inl - 1])->end >= 0)
    return;
  x->end = ir->next_instruction_index;
  x->ret_vreg = ret_vreg;
}

void tcc_ir_taint_free(TCCIRState *ir)
{
  tcc_free(ir->taint_calls);
  ir->taint_calls = NULL;
  ir->taint_nb_calls = 0;
  tcc_free(ir->taint_fobjs);
  ir->taint_fobjs = NULL;
  ir->taint_nb_fobjs = 0;
  tcc_free(ir->taint_inl);
  ir->taint_inl = NULL;
  ir->taint_nb_inl = 0;
  ir->taint_sources = 0;
}

/* ------------------------------------------------------------------------ */
/* The compact form of a function, kept to the end of the unit */

enum
{
  TK_NONE,
  TK_IMM,
  TK_TEMP,
  TK_SLOT,
  TK_FRAME, /* id: frame object (nfobj: the rest of the frame), off in it */
  TK_GLOBAL /* id: index in the function's globals, off from the symbol */
};

typedef struct TOp
{
  uint8_t kind, lval; /* lval: the memory at the address, not the address */
  int32_t id, off;
} TOp;

enum
{
  TI_NOP,
  TI_DEF,   /* d (a temp or variable) = a op b */
  TI_STORE, /* memory d = a */
  TI_CALL,  /* d = call aux */
  TI_RET    /* return a */
};

enum
{
  TS_MOVE,
  TS_ADD,
  TS_SUB,
  TS_OTHER
};

typedef struct TIns
{
  uint8_t op, sub;
  int32_t line, aux;
  TOp d, a, b;
} TIns;

typedef struct TArg
{
  TOp op;
  int line;
} TArg;

typedef struct TCall
{
  int callee, arg0, nargs, pos; /* pos: an inline expansion's end, else -1 */
  TOp ret;                      /* an inline expansion's result variable */
  uint32_t taint_arg;
  uint8_t taint_ret, fmt_idx, noted, line_set;
  int8_t copy_dst, copy_src;
  int line;
} TCall;

/* a summary entry: what a call leaves in memory */
typedef struct TEnt
{
  uint8_t fill, m; /* fill: from off on (else the path at off) */
  int8_t param;    /* the parameter whose pointee it is, -1: the global */
  Sym *g;
  int32_t off;
} TEnt;

typedef struct TaintSum
{
  int tok;
  uint8_t ret, sinkp;
  uint8_t pw[TM_NPARAM];
  int sink_line[TM_NPARAM], sink_tok[TM_NPARAM];
  const char *sink_file[TM_NPARAM];
  int nent;
  TEnt ent[TS_MAX_ENT];
} TaintSum;

typedef struct TLoc
{
  int8_t bk;  /* BK_* */
  uint8_t lb; /* off is a lower bound: somewhere from there on */
  int32_t id, off; /* off TF_OFF_ANY: not known */
  Sym *g;
} TLoc;

enum
{
  BK_NONE,
  BK_SLOT,   /* what variable slot id points to */
  BK_FRAME,  /* frame object id */
  BK_GLOBAL, /* the global g */
  BK_MULTI   /* several (an alias's join) */
};

typedef struct TaintFn
{
  struct TaintFn *next;
  int tok, line, rank;
  const char *file;
  int n, nb, nvar, nparam, ntemp, nfobj, nglob, ncalls, nargs, argv_slot, ninl;
  TIns *ins;
  int *bstart, *bend, *bsucc0, *bsucc, *rpo, nrpo;
  Sym **glob;
  TCall *calls;
  TArg *args;
  int *inl; /* the inline expansions' calls, by end */
  TLoc *tl, *sl; /* per temp / variable slot: the object it points into */
  TaintSum *sum;
} TaintFn;

static struct
{
  TaintFn *first, **last;
  TaintSum **sums; /* open hash by token */
  int nsums, cap;
  size_t bytes;
  int has_source;
  /* the sinks reported (file, line, callee): once each, also when several
     calls reach one */
  struct
  {
    const char *file;
    int line, tok;
  } * said;
  int nsaid;
} tu;

static TaintSum *tu_sum(int tok, int create)
{
  unsigned h;
  if (!tok)
    return NULL;
  if (create && 2 * (tu.nsums + 1) > tu.cap)
  {
    int ocap = tu.cap;
    TaintSum **old = tu.sums;
    tu.cap = tu.cap ? 2 * tu.cap : 64;
    tu.sums = tcc_mallocz(tu.cap * sizeof *tu.sums);
    for (int i = 0; i < ocap; i++)
      if (old[i])
      {
        for (h = (unsigned)old[i]->tok * 2654435761u & (tu.cap - 1); tu.sums[h]; h = (h + 1) & (tu.cap - 1))
          ;
        tu.sums[h] = old[i];
      }
    tcc_free(old);
  }
  if (!tu.cap)
    return NULL;
  for (h = (unsigned)tok * 2654435761u & (tu.cap - 1); tu.sums[h]; h = (h + 1) & (tu.cap - 1))
    if (tu.sums[h]->tok == tok)
      return tu.sums[h];
  if (!create)
    return NULL;
  tu.sums[h] = tcc_mallocz(sizeof(TaintSum));
  tu.sums[h]->tok = tok;
  tu.nsums++;
  return tu.sums[h];
}

static void tf_free(TaintFn *f)
{
  tcc_free(f->ins);
  tcc_free(f->bstart);
  tcc_free(f->bend);
  tcc_free(f->bsucc0);
  tcc_free(f->bsucc);
  tcc_free(f->rpo);
  tcc_free(f->glob);
  tcc_free(f->calls);
  tcc_free(f->args);
  tcc_free(f->inl);
  tcc_free(f->tl);
  tcc_free(f->sl);
  tcc_free(f);
}

void tcc_ir_taint_unit_free(void)
{
  for (TaintFn *f = tu.first, *nx; f; f = nx)
  {
    nx = f->next;
    tf_free(f);
  }
  for (int i = 0; i < tu.cap; i++)
    tcc_free(tu.sums[i]);
  tcc_free(tu.sums);
  tcc_free(tu.said);
  memset(&tu, 0, sizeof tu);
}

/* ---- encoding ---- */

typedef struct TEnc
{
  TCCIRState *ir;
  TaintFn *f;
  int globcap;
} TEnc;

static int tf_glob(TEnc *e, Sym *s)
{
  TaintFn *f = e->f;
  for (int i = 0; i < f->nglob; i++)
    if (f->glob[i] == s)
      return i;
  if (f->nglob >= e->globcap)
  {
    e->globcap = e->globcap ? 2 * e->globcap : 8;
    f->glob = tcc_realloc(f->glob, e->globcap * sizeof(Sym *));
  }
  f->glob[f->nglob] = s;
  return f->nglob++;
}

static TOp tf_frame(TEnc *e, int32_t fo, int lval)
{
  TOp o = {TK_FRAME, (uint8_t)lval, e->f->nfobj, fo};
  const int *fobj = e->ir->taint_fobjs;
  /* the innermost object holding it: the last one declared */
  for (int j = e->f->nfobj; j-- > 0;)
    if (fo >= fobj[2 * j] && fo < fobj[2 * j] + fobj[2 * j + 1])
    {
      o.id = j;
      o.off = fo - fobj[2 * j];
      break;
    }
  return o;
}

static TOp tf_vreg(TEnc *e, int32_t vreg, int lval)
{
  TOp o = {TK_NONE, (uint8_t)lval, 0, 0};
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
  case TCCIR_VREG_TYPE_TEMP:
    if (pos < e->f->ntemp)
      o.kind = TK_TEMP, o.id = pos;
    break;
  case TCCIR_VREG_TYPE_VAR:
    if (pos < e->f->nvar)
      o.kind = TK_SLOT, o.id = pos;
    break;
  case TCCIR_VREG_TYPE_PARAM:
    if (pos < e->f->nparam)
      o.kind = TK_SLOT, o.id = e->f->nvar + pos;
    break;
  }
  return o;
}

static TOp tf_op(TEnc *e, IROperand op)
{
  TOp o = {TK_NONE, 0, 0, 0};
  int32_t vreg;
  if (irop_is_none(op))
    return o;
  if (irop_is_immediate(op))
  {
    int64_t v = irop_get_imm64_ex(e->ir, op);
    o.kind = TK_IMM;
    o.off = v >= INT32_MIN && v <= INT32_MAX ? (int32_t)v : 0;
    return o;
  }
  if ((vreg = irop_get_vreg(op)) >= 0)
  {
    /* a variable homed in the frame: StackLoc + lval is its value, an
       llocal what its value points to */
    if (irop_get_tag(op) != IROP_TAG_STACKOFF)
      return tf_vreg(e, vreg, op.is_lval);
    if (irop_is_llocal(op))
      return tf_vreg(e, vreg, 1);
    return op.is_lval ? tf_vreg(e, vreg, 0) : o;
  }
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_is_local(op) && !irop_is_llocal(op) && !op.is_param)
    return tf_frame(e, irop_get_stack_offset(op), op.is_lval);
  if (irop_get_tag(op) == IROP_TAG_SYMREF)
  {
    IRPoolSymref *r = irop_get_symref_ex(e->ir, op);
    if (r && r->sym)
    {
      o.kind = TK_GLOBAL, o.lval = op.is_lval;
      o.id = tf_glob(e, r->sym);
      o.off = r->addend;
    }
  }
  return o;
}

static void tf_scan_vreg(TaintFn *f, int32_t vreg)
{
  if (vreg < 0)
    return;
  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_VAR && TCCIR_DECODE_VREG_POSITION(vreg) >= f->nvar)
    f->nvar = TCCIR_DECODE_VREG_POSITION(vreg) + 1;
  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM && TCCIR_DECODE_VREG_POSITION(vreg) >= f->nparam)
    f->nparam = TCCIR_DECODE_VREG_POSITION(vreg) + 1;
}

static int tf_call_id(TCCIRState *ir, const IRQuadCompact *q, int *idx)
{
  int64_t enc;
  if (!irop_config[q->op].has_src2)
    return -1;
  enc = irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
  if (idx)
    *idx = TCCIR_DECODE_PARAM_IDX(enc);
  return TCCIR_DECODE_CALL_ID(enc);
}

static int tf_is_call(int op)
{
  return op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID;
}

/* the function's raw IR in compact form; NULL when there is nothing to keep */
static TaintFn *tf_encode(TCCIRState *ir, Sym *func, size_t *bytes)
{
  TEnc e;
  TaintFn *f;
  IRCFG *cfg;
  int n = ir->next_instruction_index, maxid = -1, *cmap, ncalls = 0, nargs = 0;

  if (!n)
    return NULL;
  f = tcc_mallocz(sizeof *f);
  memset(&e, 0, sizeof e);
  e.ir = ir, e.f = f;
  f->n = n;
  f->ntemp = ir->next_temporary_variable;
  f->nfobj = ir->taint_nb_fobjs;
  f->argv_slot = -1;
  f->tok = func ? func->v : 0;
  f->file = tcc_diag_file(NULL);
  f->line = n ? ir->compact_instructions[0].line_num : 0;
  /* the variables named anywhere, the calls */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfgq = &irop_config[q->op];
    if (cfgq->has_dest)
      tf_scan_vreg(f, irop_get_vreg(tcc_ir_op_get_dest(ir, q)));
    if (cfgq->has_src1)
      tf_scan_vreg(f, irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
    if (cfgq->has_src2 && q->op != TCCIR_OP_FUNCPARAMVAL && !tf_is_call(q->op))
      tf_scan_vreg(f, irop_get_vreg(tcc_ir_op_get_src2(ir, q)));
    if (q->op == TCCIR_OP_FUNCPARAMVAL || tf_is_call(q->op))
    {
      int id = tf_call_id(ir, q, NULL);
      if (id > maxid)
        maxid = id;
    }
  }
  for (int k = 0; k < ir->taint_nb_inl; k++)
  {
    tf_scan_vreg(f, ir->taint_inl[k].ret_vreg);
    for (int j = 0; j < ir->taint_inl[k].nparams; j++)
      tf_scan_vreg(f, ir->taint_inl[k].pvreg[j]);
  }
  if (func && func->type.ref && !strcmp(get_tok_str(func->v, NULL), "main"))
  {
    Sym *p = sym_first(func->type.ref);
    if (p && p->next && (p->next->type.t & VT_BTYPE) == VT_PTR && f->nparam > 1)
      f->argv_slot = f->nvar + 1; /* main(int argc, char **argv): the second parameter */
  }
  /* nothing goes in or out: no call, no parameter */
  if (maxid < 0 && !ir->taint_nb_inl && !f->nparam)
  {
    tcc_free(f);
    return NULL;
  }
  /* the calls: arguments by call_id */
  cmap = tcc_malloc((maxid + 2) * sizeof(int));
  for (int k = 0; k <= maxid; k++)
    cmap[k] = -1;
  {
    int *na = tcc_mallocz((maxid + 2) * sizeof(int));
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      int idx = 0, id;
      if (q->op != TCCIR_OP_FUNCPARAMVAL && !tf_is_call(q->op))
        continue;
      if ((id = tf_call_id(ir, q, &idx)) < 0)
        continue;
      if (cmap[id] < 0)
        cmap[id] = ncalls++;
      if (q->op == TCCIR_OP_FUNCPARAMVAL && idx < TF_MAX_ARGS && idx + 1 > na[id])
        na[id] = idx + 1;
    }
    f->ncalls = ncalls + ir->taint_nb_inl;
    f->calls = tcc_mallocz((f->ncalls + 1) * sizeof(TCall));
    for (int id = 0; id <= maxid; id++)
      if (cmap[id] >= 0)
      {
        TCall *k = &f->calls[cmap[id]];
        k->arg0 = nargs;
        k->nargs = na[id];
        k->pos = -1;
        k->copy_dst = k->copy_src = -1;
        nargs += na[id];
        if (id < ir->taint_nb_calls)
        {
          struct IRTaintCall *t = &ir->taint_calls[id];
          k->callee = t->callee;
          if (t->noted)
          {
            k->noted = 1;
            k->taint_arg = t->taint_arg;
            k->taint_ret = t->taint_ret;
            k->fmt_idx = t->fmt_idx;
            k->copy_dst = t->copy_dst;
            k->copy_src = t->copy_src;
          }
        }
      }
    for (int x = 0; x < ir->taint_nb_inl; x++)
    {
      TCall *k = &f->calls[ncalls + x];
      k->callee = ir->taint_inl[x].callee;
      k->arg0 = nargs;
      k->nargs = ir->taint_inl[x].nparams;
      k->pos = ir->taint_inl[x].end >= 0 ? ir->taint_inl[x].end : n;
      k->copy_dst = k->copy_src = -1;
      k->line = ir->taint_inl[x].line;
      if (ir->taint_inl[x].ret_vreg >= 0)
        k->ret = tf_vreg(&e, ir->taint_inl[x].ret_vreg, 0);
      nargs += k->nargs;
    }
    tcc_free(na);
  }
  f->nargs = nargs;
  f->args = tcc_mallocz((nargs + 1) * sizeof(TArg));
  for (int x = 0; x < ir->taint_nb_inl; x++)
  {
    TCall *k = &f->calls[ncalls + x];
    for (int j = 0; j < k->nargs; j++)
      f->args[k->arg0 + j].op = ir->taint_inl[x].pvreg[j] >= 0 ? tf_vreg(&e, ir->taint_inl[x].pvreg[j], 0)
                                : ir->taint_inl[x].pfo[j] != 1 ? tf_frame(&e, ir->taint_inl[x].pfo[j], 1)
                                                                : (TOp){TK_NONE, 0, 0, 0};
  }
  f->ninl = ir->taint_nb_inl;
  if (f->ninl)
  {
    f->inl = tcc_malloc(f->ninl * sizeof(int));
    for (int x = 0; x < f->ninl; x++)
      f->inl[x] = ncalls + x;
    /* by end, as they are met on the walk */
    for (int a = 1; a < f->ninl; a++)
      for (int b = a; b > 0 && f->calls[f->inl[b]].pos < f->calls[f->inl[b - 1]].pos; b--)
      {
        int t = f->inl[b];
        f->inl[b] = f->inl[b - 1], f->inl[b - 1] = t;
      }
  }
  /* the instructions */
  f->ins = tcc_mallocz(n * sizeof(TIns));
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cq = &irop_config[q->op];
    TIns *t = &f->ins[i];
    IROperand dest = cq->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    t->line = q->line_num;
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      int idx = 0, id = tf_call_id(ir, q, &idx);
      TCall *k = id >= 0 && cmap[id] >= 0 ? &f->calls[cmap[id]] : NULL;
      if (k && idx < k->nargs)
      {
        f->args[k->arg0 + idx].op = tf_op(&e, tcc_ir_op_get_src1(ir, q));
        f->args[k->arg0 + idx].line = q->line_num;
      }
      continue;
    }
    if (tf_is_call(q->op))
    {
      int id = tf_call_id(ir, q, NULL);
      if (id < 0 || cmap[id] < 0)
        continue;
      t->op = TI_CALL;
      t->aux = cmap[id];
      f->calls[cmap[id]].line = q->line_num;
      if (q->op == TCCIR_OP_FUNCCALLVAL && !irop_is_none(dest))
        t->d = tf_op(&e, dest);
      continue;
    }
    if (q->op == TCCIR_OP_RETURNVALUE)
    {
      t->op = TI_RET;
      t->a = tf_op(&e, tcc_ir_op_get_src1(ir, q));
      continue;
    }
    if (irop_is_none(dest))
      continue;
    if (!irop_dest_defines_vreg(dest))
    {
      if (q->op == TCCIR_OP_STORE)
      {
        t->op = TI_STORE;
        t->d = tf_op(&e, dest);
        t->d.lval = 1;
        t->a = tf_op(&e, tcc_ir_op_get_src1(ir, q));
      }
      continue;
    }
    t->op = TI_DEF;
    t->d = tf_op(&e, dest);
    t->d.lval = 0;
    if (cq->has_src1)
      t->a = tf_op(&e, tcc_ir_op_get_src1(ir, q));
    if (cq->has_src2)
      t->b = tf_op(&e, tcc_ir_op_get_src2(ir, q));
    switch (q->op)
    {
    case TCCIR_OP_LEA:
      t->a.lval = 0; /* &x */
      /* fall through */
    case TCCIR_OP_ASSIGN:
    case TCCIR_OP_LOAD:
    case TCCIR_OP_STORE:
      t->sub = TS_MOVE;
      break;
    case TCCIR_OP_ADD:
      t->sub = TS_ADD;
      break;
    case TCCIR_OP_SUB:
      t->sub = TS_SUB;
      break;
    default:
      t->sub = TS_OTHER;
      break;
    }
  }
  tcc_free(cmap);
  /* the CFG */
  if (!(cfg = tcc_ir_cfg_build(ir)))
  {
    tf_free(f);
    return NULL;
  }
  tcc_ir_cfg_compute_rpo(cfg);
  f->nb = cfg->num_blocks;
  f->bstart = tcc_malloc((f->nb + 1) * sizeof(int));
  f->bend = tcc_malloc((f->nb + 1) * sizeof(int));
  f->bsucc0 = tcc_malloc((f->nb + 1) * sizeof(int));
  {
    int ns = 0;
    for (int b = 0; b < f->nb; b++)
      ns += cfg->blocks[b].num_succs;
    f->bsucc = tcc_malloc((ns + 1) * sizeof(int));
    ns = 0;
    for (int b = 0; b < f->nb; b++)
    {
      f->bstart[b] = cfg->blocks[b].start_idx;
      f->bend[b] = cfg->blocks[b].end_idx;
      f->bsucc0[b] = ns;
      for (int k = 0; k < cfg->blocks[b].num_succs; k++)
        f->bsucc[ns++] = cfg->blocks[b].succs[k];
    }
    f->bsucc0[f->nb] = ns;
  }
  f->nrpo = cfg->rpo_count;
  f->rpo = tcc_malloc((f->nrpo + 1) * sizeof(int));
  memcpy(f->rpo, cfg->rpo_order, f->nrpo * sizeof(int));
  tcc_ir_cfg_free(cfg);
  *bytes = sizeof *f + n * sizeof(TIns) + (size_t)f->ncalls * sizeof(TCall) + (size_t)nargs * sizeof(TArg) +
           (size_t)(4 * f->nb + f->bsucc0[f->nb] + f->nrpo) * sizeof(int) + f->nglob * sizeof(Sym *) +
           (size_t)(f->ntemp + f->nvar + f->nparam) * sizeof(TLoc);
  return f;
}

/* ---- the objects pointers point into, for the whole function ---- */

static TLoc tl_none(void)
{
  TLoc l = {BK_NONE, 0, 0, 0, NULL};
  return l;
}

static TLoc tl_make(int bk, int32_t id, int32_t off, Sym *g)
{
  TLoc l = {(int8_t)bk, 0, id, off, g};
  return l;
}

static int tl_valid(TLoc l)
{
  return l.bk != BK_NONE && l.bk != BK_MULTI;
}

static int tl_join(TLoc *a, TLoc b)
{
  TLoc o = *a;
  if (a->bk == BK_MULTI || b.bk == BK_NONE)
    return 0;
  if (a->bk == BK_NONE)
    o = b;
  else if (b.bk == BK_MULTI || a->bk != b.bk || a->id != b.id || a->g != b.g)
    o.bk = BK_MULTI;
  else if (a->off != b.off || a->lb != b.lb)
  {
    o.off = a->off == TF_OFF_ANY || b.off == TF_OFF_ANY ? TF_OFF_ANY : a->off < b.off ? a->off : b.off;
    o.lb = 1;
  }
  if (o.bk == a->bk && o.id == a->id && o.off == a->off && o.lb == a->lb && o.g == a->g)
    return 0;
  *a = o;
  return 1;
}

static TLoc tl_slot(TaintFn *f, int s)
{
  return tl_valid(f->sl[s]) ? f->sl[s] : tl_make(BK_SLOT, s, 0, NULL);
}

/* the address OP's value is: the object and the offset in it */
static TLoc tl_of(TaintFn *f, TOp op)
{
  TLoc l = tl_none();
  if (op.lval)
    return l;
  switch (op.kind)
  {
  case TK_TEMP:
    return f->tl[op.id];
  case TK_SLOT:
    return tl_slot(f, op.id);
  case TK_FRAME:
    return tl_make(BK_FRAME, op.id, op.off, NULL);
  case TK_GLOBAL:
    return tl_make(BK_GLOBAL, 0, op.off, f->glob[op.id]);
  }
  return l;
}

static TLoc tl_add(TLoc l, TOp k, int sub)
{
  if (!tl_valid(l) || l.off == TF_OFF_ANY)
    return l;
  if (k.kind == TK_IMM && !k.lval)
  {
    int64_t o = (int64_t)l.off + (sub ? -(int64_t)k.off : k.off);
    l.off = o > INT32_MIN && o <= INT32_MAX ? (int32_t)o : TF_OFF_ANY;
  }
  else if (sub)
    l.off = TF_OFF_ANY;
  else
    l.lb = 1; /* an index added: from l.off on */
  return l;
}

/* the address an instruction defines */
static TLoc tl_def(TaintFn *f, const TIns *t)
{
  TLoc l;
  switch (t->sub)
  {
  case TS_MOVE:
    return tl_of(f, t->a);
  case TS_ADD:
    l = tl_of(f, t->a);
    if (tl_valid(l))
      return tl_add(l, t->b, 0);
    l = tl_of(f, t->b);
    return tl_valid(l) ? tl_add(l, t->a, 0) : tl_none();
  case TS_SUB:
    l = tl_of(f, t->a);
    return tl_valid(l) ? tl_add(l, t->b, 1) : tl_none();
  }
  return tl_none();
}

static void tf_points_to(TaintFn *f)
{
  int nslots = f->nvar + f->nparam, changed = 1;
  f->tl = tcc_mallocz((f->ntemp + 1) * sizeof(TLoc));
  f->sl = tcc_mallocz((nslots + 1) * sizeof(TLoc));
  for (int s = f->nvar; s < nslots; s++)
    f->sl[s].bk = BK_MULTI; /* a parameter: what the caller passed */
  for (int round = 0; changed && round < 16; round++)
  {
    changed = 0;
    for (int i = 0; i < f->n; i++)
    {
      const TIns *t = &f->ins[i];
      TLoc l;
      if (t->op == TI_CALL && t->d.kind == TK_SLOT)
      {
        changed |= tl_join(&f->sl[t->d.id], tl_make(BK_MULTI, 0, 0, NULL));
      }
      if (t->op != TI_DEF)
        continue;
      l = tl_def(f, t);
      if (t->d.kind == TK_TEMP)
        changed |= tl_join(&f->tl[t->d.id], l);
      else if (t->d.kind == TK_SLOT)
      {
        /* an alias only when every assignment is an address in one object
           (not its own: `p = p + 1` keeps what it had) */
        if (!tl_valid(l) || (l.bk == BK_SLOT && l.id == t->d.id))
        {
          if (!(l.bk == BK_SLOT && l.id == t->d.id && tl_valid(f->sl[t->d.id])))
            l.bk = BK_MULTI;
          else
            l = f->sl[t->d.id], l.lb = 1;
        }
        changed |= tl_join(&f->sl[t->d.id], l);
      }
    }
  }
}

/* ---- the walk ---- */

enum
{
  LK_WHOLE,
  LK_STORE, /* what was stored at off */
  LK_FILL   /* written from off on */
};

typedef struct TLocKey
{
  uint8_t kind;
  TLoc l;
} TLocKey;

typedef struct TCtx
{
  TaintFn *f;
  int nslots, width, nloc;
  uint8_t *cur, *tt;
  TLocKey locs[TF_MAX_LOCS];
  int changed, report, final;
  TaintSum ns;
} TCtx;

static int tl_exact(TLoc l)
{
  return l.off != TF_OFF_ANY && !l.lb;
}

static int same_base(TLoc a, TLoc b)
{
  return a.bk == b.bk && a.id == b.id && a.g == b.g;
}

static int loc_find(TCtx *c, int kind, TLoc l, int create)
{
  for (int k = 0; k < c->nloc; k++)
    if (c->locs[k].kind == kind && same_base(c->locs[k].l, l) && (kind == LK_WHOLE || c->locs[k].l.off == l.off))
      return c->nslots + k;
  if (!create || c->nloc >= TF_MAX_LOCS || c->nslots + c->nloc >= c->width)
    return -1;
  c->locs[c->nloc].kind = (uint8_t)kind;
  c->locs[c->nloc].l = l;
  c->locs[c->nloc].l.lb = 0;
  if (kind == LK_WHOLE)
    c->locs[c->nloc].l.off = 0;
  return c->nslots + c->nloc++;
}

static uint8_t loc_whole(TCtx *c, TLoc l)
{
  int k;
  if (l.bk == BK_SLOT)
    return c->cur[l.id];
  k = loc_find(c, LK_WHOLE, l, 0);
  return k >= 0 ? c->cur[k] : 0;
}

/* what reading at l (load), or the bytes from l on (string), may give */
static uint8_t mem_read(TCtx *c, TLoc l, int string)
{
  uint8_t m;
  int exact;
  if (!tl_valid(l))
    return 0;
  m = loc_whole(c, l);
  exact = tl_exact(l);
  for (int k = 0; k < c->nloc; k++)
  {
    TLocKey *x = &c->locs[k];
    if (x->kind == LK_WHOLE || !same_base(x->l, l))
      continue;
    /* a fill reaches what is after it; a store, its own place (a string
       from l: every place after l) */
    if (x->kind == LK_FILL ? (!exact || x->l.off <= l.off)
                           : exact && !string ? x->l.off == l.off : l.off == TF_OFF_ANY || x->l.off >= l.off)
      m |= c->cur[c->nslots + k];
  }
  return m;
}

/* memory at l gets m: a store at a constant offset replaces that path's */
static void mem_write(TCtx *c, TLoc l, uint8_t m, int fill)
{
  int k;
  if (!tl_valid(l))
    return;
  if (tl_exact(l))
  {
    k = loc_find(c, fill ? LK_FILL : LK_STORE, l, m != 0);
    if (k >= 0)
    {
      c->cur[k] = fill ? c->cur[k] | m : m;
      return;
    }
    if (!m)
      return;
  }
  else if (!m)
    return;
  else if (l.off != TF_OFF_ANY && (k = loc_find(c, LK_FILL, l, 1)) >= 0)
  {
    /* somewhere from a lower bound on: as a fill from there */
    c->cur[k] |= m;
    return;
  }
  if (l.bk == BK_SLOT)
    c->cur[l.id] |= m;
  else if ((k = loc_find(c, LK_WHOLE, l, 1)) >= 0)
    c->cur[k] |= m;
}

static void tt_or(TCtx *c, int t, uint8_t m)
{
  if ((c->tt[t] | m) != c->tt[t])
    c->tt[t] |= m, c->changed = 1;
}

/* OP's value: its mask, and the object it points into */
static uint8_t op_val(TCtx *c, TOp op, TLoc *pl)
{
  TaintFn *f = c->f;
  TLoc l = tl_none();
  uint8_t m = 0;
  if (op.kind == TK_NONE || op.kind == TK_IMM)
    goto out;
  if (!op.lval)
  {
    l = tl_of(f, op);
    if (op.kind == TK_TEMP)
      m = c->tt[op.id];
    else if (op.kind == TK_SLOT)
      m = c->cur[op.id];
    goto out;
  }
  /* read through the address */
  op.lval = 0;
  l = tl_of(f, op);
  if (tl_valid(l))
    m = mem_read(c, l, 0);
  else if (op.kind == TK_TEMP)
    m = c->tt[op.id]; /* a pointer of unknown origin: what it points to is as it */
  l = tl_none();
out:
  if (pl)
    *pl = l;
  return m;
}

/* the value or what it points to */
static uint8_t op_any(TCtx *c, TOp op)
{
  TLoc l;
  uint8_t m = op_val(c, op, &l);
  return m | (tl_valid(l) ? mem_read(c, l, 1) : 0);
}

/* what OP points to gets m from off on (a buffer filled) */
static void op_fill(TCtx *c, TOp op, uint8_t m, int32_t off, int fill)
{
  TLoc l;
  if (!m || op.lval)
    return;
  l = tl_of(c->f, op);
  if (tl_valid(l))
  {
    if (l.off != TF_OFF_ANY)
    {
      int64_t o = (int64_t)l.off + off;
      l.off = o > INT32_MIN && o <= INT32_MAX ? (int32_t)o : TF_OFF_ANY;
    }
    mem_write(c, l, m, fill);
  }
  else if (op.kind == TK_TEMP)
    tt_or(c, op.id, m);
}

static void slot_def(TCtx *c, int s, uint8_t m)
{
  c->cur[s] = m;
  /* a new pointer: what the old one pointed to is not its */
  for (int k = 0; k < c->nloc; k++)
    if (c->locs[k].l.bk == BK_SLOT && c->locs[k].l.id == s)
      c->cur[c->nslots + k] = 0;
}

static void def(TCtx *c, TOp d, uint8_t m, TLoc l)
{
  if (d.kind == TK_TEMP)
    tt_or(c, d.id, m);
  else if (d.kind == TK_SLOT)
    slot_def(c, d.id, m | (tl_valid(l) ? mem_read(c, l, 1) : 0));
}

static const char *tok_copy(int tok, char *buf, size_t n)
{
  pstrcpy(buf, n, get_tok_str(tok, NULL));
  return buf;
}

static void taint_report(TCtx *c, const char *file, int line, int sink, int via, int call_line)
{
  TCCState *s1 = tcc_state;
  const char *saved = s1->diag_replay_file;
  char name[128], vname[128], fname[128];
  for (int i = 0; i < tu.nsaid; i++)
    if (tu.said[i].file == file && tu.said[i].line == line && tu.said[i].tok == sink)
      return;
  if (!(tu.nsaid & (tu.nsaid - 1)))
    tu.said = tcc_realloc(tu.said, (tu.nsaid ? 2 * tu.nsaid : 8) * sizeof *tu.said);
  tu.said[tu.nsaid].file = file, tu.said[tu.nsaid].line = line, tu.said[tu.nsaid].tok = sink;
  tu.nsaid++;
  tok_copy(sink, name, sizeof name);
  s1->diag_replay_file = file;
  tcc_warning_once_at(warn_tainted_format, line, "the format of '%s' comes from untrusted input: write %s(\"%%s\", ...)",
                      name, name);
  if (via)
  {
    s1->diag_replay_file = c->f->file;
    tok_copy(via, vname, sizeof vname);
    tok_copy(c->f->tok, fname, sizeof fname);
    tcc_note("%i:untrusted input reaches it through the call to '%s' in '%s'", call_line, vname, fname);
  }
  s1->diag_replay_file = saved;
}

/* a format argument with mask m */
static void sink_hit(TCtx *c, uint8_t m, const char *file, int line, int sink, int via, int call_line)
{
  if ((m & TM_SRC) && c->report)
    taint_report(c, file, line, sink, via, call_line);
  if (!c->final)
    return;
  for (int j = 0; j < TM_NPARAM; j++)
    if ((m & TM_PARAM(j)) && !(c->ns.sinkp & (1u << j)))
    {
      c->ns.sinkp |= (uint8_t)(1u << j);
      c->ns.sink_line[j] = line;
      c->ns.sink_tok[j] = sink;
      c->ns.sink_file[j] = file;
    }
}

/* a callee's mask in the caller's terms */
static uint8_t sum_map(uint8_t m, const uint8_t *argm, int nargs)
{
  uint8_t r = m & TM_SRC;
  for (int j = 0; j < TM_NPARAM && j < nargs; j++)
    if (m & TM_PARAM(j))
      r |= argm[j];
  return r;
}

static void do_call(TCtx *c, const TCall *k, TOp d)
{
  TaintFn *f = c->f;
  const TArg *args = f->args + k->arg0;
  uint8_t argm[TF_MAX_ARGS], ret = 0;
  TaintSum *s;
  TLoc none = tl_none();
  for (int j = 0; j < k->nargs; j++)
    argm[j] = op_any(c, args[j].op);
  if (k->noted)
  {
    if (k->fmt_idx && k->fmt_idx <= k->nargs)
      sink_hit(c, argm[k->fmt_idx - 1], f->file, args[k->fmt_idx - 1].line, k->callee, 0, 0);
    if (k->taint_ret)
      ret |= TM_SRC;
    for (int j = 0; j < k->nargs && j < 32; j++)
      if ((k->taint_arg >> j) & 1)
        op_fill(c, args[j].op, TM_SRC, 0, 1);
    if (k->copy_src >= 0 && k->copy_src < k->nargs)
    {
      if (k->copy_dst >= 0 && k->copy_dst < k->nargs)
      {
        op_fill(c, args[k->copy_dst].op, argm[k->copy_src], 0, 1);
        ret |= argm[k->copy_dst];
      }
      ret |= argm[k->copy_src];
    }
  }
  else if ((s = tu_sum(k->callee, 0)) != NULL)
  {
    int nargs = k->nargs < TF_MAX_ARGS ? k->nargs : TF_MAX_ARGS;
    for (int j = 0; j < TM_NPARAM && j < nargs; j++)
    {
      if (s->pw[j])
        op_fill(c, args[j].op, sum_map(s->pw[j], argm, nargs), 0, 1);
      if (s->sinkp & (1u << j))
        sink_hit(c, argm[j], s->sink_file[j], s->sink_line[j], s->sink_tok[j], k->callee, k->line);
    }
    for (int e = 0; e < s->nent; e++)
    {
      const TEnt *x = &s->ent[e];
      uint8_t m = sum_map(x->m, argm, nargs);
      if (!m)
        continue;
      if (x->param >= 0)
      {
        if (x->param < nargs)
          op_fill(c, args[x->param].op, m, x->off, x->fill);
      }
      else
        mem_write(c, tl_make(BK_GLOBAL, 0, x->off, x->g), m, x->fill);
    }
    ret |= sum_map(s->ret, argm, nargs);
  }
  if (k->pos >= 0)
  {
    /* an inline expansion: its result variable has the body's value too */
    if (k->ret.kind == TK_SLOT && ret)
      c->cur[k->ret.id] |= ret;
    return;
  }
  def(c, d, ret, none);
}

static void sum_ent(TaintSum *s, int param, Sym *g, int32_t off, int fill, uint8_t m)
{
  if (!m)
    return;
  for (int e = 0; e < s->nent; e++)
    if (s->ent[e].param == param && s->ent[e].g == g && s->ent[e].off == off && s->ent[e].fill == fill)
    {
      s->ent[e].m |= m;
      return;
    }
  if (s->nent < TS_MAX_ENT)
  {
    TEnt *x = &s->ent[s->nent++];
    x->param = (int8_t)param, x->g = g, x->off = off, x->fill = (uint8_t)fill, x->m = m;
  }
  else if (param >= 0)
    s->pw[param] |= m;
}

/* the function returns here: what its parameters point to, the globals */
static void at_exit(TCtx *c)
{
  TaintFn *f = c->f;
  for (int j = 0; j < TM_NPARAM && j < f->nparam; j++)
    c->ns.pw[j] |= c->cur[f->nvar + j] & ~TM_PARAM(j);
  for (int k = 0; k < c->nloc; k++)
  {
    TLocKey *x = &c->locs[k];
    uint8_t m = c->cur[c->nslots + k];
    if (!m)
      continue;
    if (x->l.bk == BK_SLOT && x->l.id >= f->nvar && x->l.id - f->nvar < TM_NPARAM)
    {
      int j = x->l.id - f->nvar;
      if (x->kind == LK_WHOLE)
        c->ns.pw[j] |= m;
      else
        sum_ent(&c->ns, j, NULL, x->l.off, x->kind == LK_FILL, m);
    }
    else if (x->l.bk == BK_GLOBAL)
      sum_ent(&c->ns, -1, x->l.g, x->kind == LK_WHOLE ? 0 : x->l.off, x->kind != LK_STORE, m);
  }
}

static void walk_ins(TCtx *c, const TIns *t)
{
  TLoc l;
  uint8_t m;
  switch (t->op)
  {
  case TI_DEF:
    m = op_val(c, t->a, NULL) | op_val(c, t->b, NULL);
    l = tl_def(c->f, t);
    def(c, t->d, m, l);
    break;
  case TI_STORE:
  {
    TOp a = t->d;
    a.lval = 0;
    l = tl_of(c->f, a);
    m = op_any(c, t->a);
    if (tl_valid(l))
      mem_write(c, l, m, 0);
    else if (a.kind == TK_TEMP && m)
      tt_or(c, a.id, m);
    break;
  }
  case TI_CALL:
    do_call(c, &c->f->calls[t->aux], t->d);
    break;
  case TI_RET:
    if (c->final)
      c->ns.ret |= op_any(c, t->a);
    break;
  }
}

/* the inline expansions that end at i (from *x on) */
static void walk_inl(TCtx *c, int i, int *x)
{
  TaintFn *f = c->f;
  while (*x < f->ninl && f->calls[f->inl[*x]].pos <= i)
  {
    const TCall *k = &f->calls[f->inl[*x]];
    if (k->pos == i)
      do_call(c, k, k->ret);
    (*x)++;
  }
}

static int inl_first(TaintFn *f, int pos)
{
  int lo = 0, hi = f->ninl;
  while (lo < hi)
  {
    int mid = (lo + hi) / 2;
    if (f->calls[f->inl[mid]].pos < pos)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* the walk of a function: its summary (and the reports, when REPORT) */
static void tf_run(TaintFn *f, int report)
{
  TCtx *c;
  uint8_t *in, *reached;
  int nb = f->nb, again = 1;
  size_t w;

  if (!f->tl)
    tf_points_to(f);
  c = tcc_mallocz(sizeof *c);
  c->f = f;
  c->nslots = f->nvar + f->nparam;
  {
    int want = 16 + f->nfobj + 2 * f->nglob + 4 * f->ncalls + 2 * f->n / 8;
    c->width = c->nslots + (want < TF_MAX_LOCS ? want : TF_MAX_LOCS);
  }
  w = (size_t)c->width;
  if (!f->nrpo || (size_t)nb * w > TF_MAX_STATE)
  {
    tcc_free(c);
    return;
  }
  in = tcc_mallocz((size_t)nb * w + 1);
  reached = tcc_mallocz(nb + 1);
  c->cur = tcc_mallocz(w + 1);
  c->tt = tcc_mallocz(f->ntemp + 1);
  {
    /* the entry: each parameter is its own (main's argv: untrusted) */
    uint8_t *e0 = in + (size_t)f->rpo[0] * w;
    for (int j = 0; j < TM_NPARAM && j < f->nparam; j++)
      e0[f->nvar + j] = TM_PARAM(j);
    if (f->argv_slot >= 0)
      e0[f->argv_slot] |= TM_SRC;
    reached[f->rpo[0]] = 1;
  }
  tcc_state->diag_noabort++;
  for (c->final = 0; c->final < 2; c->final++)
  {
    c->report = c->final && report;
    again = 1;
    while (again)
    {
      again = 0;
      c->changed = 0;
      for (int r = 0; r < f->nrpo; r++)
      {
        int b = f->rpo[r], x;
        if (!reached[b])
          continue;
        memcpy(c->cur, in + (size_t)b * w, w);
        x = inl_first(f, f->bstart[b]);
        for (int i = f->bstart[b]; i < f->bend[b]; i++)
        {
          walk_inl(c, i, &x);
          walk_ins(c, &f->ins[i]);
        }
        if (f->bend[b] >= f->n)
          walk_inl(c, f->n, &x);
        if (c->final)
        {
          if (f->bsucc0[b] == f->bsucc0[b + 1])
            at_exit(c);
          continue;
        }
        for (int k = f->bsucc0[b]; k < f->bsucc0[b + 1]; k++)
        {
          int t = f->bsucc[k];
          uint8_t *ti = in + (size_t)t * w;
          if (!reached[t])
            reached[t] = 1, again = 1;
          for (size_t s = 0; s < w; s++)
            if ((c->cur[s] | ti[s]) != ti[s])
              ti[s] |= c->cur[s], again = 1;
        }
      }
      if (c->final)
        break;
      if (c->changed)
        again = 1;
    }
  }
  tcc_state->diag_noabort--;
  /* the summary grows: a caller walked before sees it on the next round */
  if (!f->sum)
    f->sum = tu_sum(f->tok, 1);
  if (f->sum)
  {
    TaintSum *s = f->sum, *n = &c->ns;
    s->ret |= n->ret;
    for (int j = 0; j < TM_NPARAM; j++)
    {
      s->pw[j] |= n->pw[j];
      if ((n->sinkp & (1u << j)) && !(s->sinkp & (1u << j)))
      {
        s->sinkp |= (uint8_t)(1u << j);
        s->sink_line[j] = n->sink_line[j];
        s->sink_tok[j] = n->sink_tok[j];
        s->sink_file[j] = n->sink_file[j];
      }
    }
    for (int e = 0; e < n->nent; e++)
      sum_ent(s, n->ent[e].param, n->ent[e].g, n->ent[e].off, n->ent[e].fill, n->ent[e].m);
  }
  tcc_free(in);
  tcc_free(reached);
  tcc_free(c->cur);
  tcc_free(c->tt);
  tcc_free(c);
}

/* The function's raw IR: kept to the end of the unit, else (past the
   budget) checked now with the summaries known */
void tcc_ir_taint_check(TCCIRState *ir, Sym *func)
{
  size_t bytes = 0;
  TaintFn *f = tf_encode(ir, func, &bytes);
  if (ir->taint_sources || (f && f->argv_slot >= 0))
    tu.has_source = 1;
  tcc_ir_taint_free(ir);
  if (!f)
    return;
  if (tu.bytes + bytes > TU_MAX_BYTES)
  {
    if (tu.has_source)
      tf_run(f, 1);
    tf_free(f);
    return;
  }
  tu.bytes += bytes;
  if (!tu.last)
    tu.last = &tu.first;
  *tu.last = f;
  tu.last = &f->next;
}

/* End of the unit: the summaries to a fixpoint, then the reports, in the
   order of the functions' files and lines */
void tcc_ir_taint_unit_end(void)
{
  int nf = 0, changed = 1;
  TaintFn **v;
  if (!tu.first || !tu.has_source)
  {
    tcc_ir_taint_unit_free();
    return;
  }
  for (TaintFn *f = tu.first; f; f = f->next)
    nf++;
  v = tcc_malloc(nf * sizeof *v);
  nf = 0;
  for (TaintFn *f = tu.first; f; f = f->next)
  {
    f->rank = nf;
    for (TaintFn *g = tu.first; g != f; g = g->next)
      if (g->file == f->file)
      {
        f->rank = g->rank;
        break;
      }
    v[nf++] = f;
  }
  for (int a = 1; a < nf; a++)
    for (int b = a; b > 0 && (v[b]->rank < v[b - 1]->rank || (v[b]->rank == v[b - 1]->rank && v[b]->line < v[b - 1]->line));
         b--)
    {
      TaintFn *t = v[b];
      v[b] = v[b - 1], v[b - 1] = t;
    }
  for (int round = 0; changed && round < 64; round++)
  {
    changed = 0;
    for (int i = 0; i < nf; i++)
    {
      TaintSum *s = tu_sum(v[i]->tok, 0), before;
      if (s)
        memcpy(&before, s, sizeof before);
      tf_run(v[i], 0);
      if (v[i]->sum && (!s || memcmp(&before, v[i]->sum, sizeof before)))
        changed = 1;
    }
  }
  for (int i = 0; i < nf; i++)
    tf_run(v[i], 1);
  tcc_free(v);
  tcc_ir_taint_unit_free();
}
