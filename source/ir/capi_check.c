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
 * -Wyasc-c-api: regular C that writes through what a yasc function handed it
 * read-only.  A yasc declaration says `const char * c_api(char *) dlerror(void)`:
 * C sees `char *dlerror(void)` (POSIX), and the header written from the yasc
 * one tells tcc the rest with yasc_c_api(0, const char *) (decl/c_api.c).
 *
 * Sources: the result of such a call, and what it stores through an `out`
 * argument (`strtol(s, &end, 10)` when the yasc declaration's endptr is
 * `const char **`).  Sinks: a store through the pointer, or through one
 * derived from it by arithmetic, and passing it to a parameter that is a
 * pointer to non-const (strcpy's destination).
 *
 * The walk is the raw IR of each function, before any pass, in the order the
 * code is written -- no CFG: a value set on one branch counts after the join
 * ("may write"), a loop's back edge is not followed.  Values: temps and
 * variables (vregs) and the frame's address-taken locals (by offset).  Memory
 * reached through other pointers is not followed.  Only in a unit that met a
 * yasc_c_api attribute.
 */

#define USING_GLOBALS
#include "ir.h"

/* a call's facts, by call_id (tcc_ir_capi_note_call) */
struct IRCapiCall
{
  int callee;   /* token, or 0 */
  uint8_t ret;  /* its result points to what it hands back read-only */
  uint32_t out; /* bit k: what argument k points to is set to such a pointer */
  uint32_t wr;  /* bit k: parameter k is a pointer to non-const */
};

#define CK_NONE 0
#define CK_VREG 1  /* the value of a vreg */
#define CK_FRAME 2 /* the content of a frame object at an offset */

typedef struct CapiOp
{
  uint8_t kind;  /* CK_* */
  uint8_t deref; /* the memory the value points to (a store through it) */
  uint8_t addr;  /* the address of the place (&x), not its value */
  int32_t id;    /* the vreg or the frame offset */
} CapiOp;

/* where a read-only pointer came from */
typedef struct CapiOrigin
{
  int callee, line, arg; /* arg: 0 the result, else the out argument k + 1 */
} CapiOrigin;

typedef struct CapiEnt
{
  uint8_t kind, is_addr; /* is_addr: the value is the address of (akind, aid) */
  uint8_t akind;
  int32_t id, aid;
  int origin; /* index into the origins, -1 none */
} CapiEnt;

typedef struct CapiCtx
{
  TCCIRState *ir;
  CapiEnt *ent;
  int nent, cent;
  CapiOrigin *org;
  int norg, corg;
  int *said;
  int nsaid;
} CapiCtx;

void tcc_ir_capi_begin(TCCIRState *ir)
{
  ir->capi_on = yasc_capi_c_seen && (tcc_state->warn_yasc_c_api & WARN_ON) && !tcc_state->warn_none;
  ir->capi_sources = 0;
}

void tcc_ir_capi_free(TCCIRState *ir)
{
  tcc_free(ir->capi_calls);
  ir->capi_calls = NULL;
  ir->capi_nb_calls = 0;
  ir->capi_sources = 0;
}

void tcc_ir_capi_note_call(TCCIRState *ir, int call_id, int callee, Sym *ftype)
{
  struct IRCapiCall *c;
  int ret = 0;
  uint32_t out = 0, wr = 0;
  if (call_id < 0 || !ftype || tcc_state->in_inline_expansion)
    return;
  yasc_capi_call_masks(ftype, &ret, &out, &wr);
  if (!ret && !out && !wr)
    return;
  if (call_id >= ir->capi_nb_calls)
  {
    int n = call_id + 16;
    ir->capi_calls = tcc_realloc(ir->capi_calls, n * sizeof *c);
    memset(ir->capi_calls + ir->capi_nb_calls, 0, (n - ir->capi_nb_calls) * sizeof *c);
    ir->capi_nb_calls = n;
  }
  c = &ir->capi_calls[call_id];
  c->callee = callee;
  c->ret = (uint8_t)ret;
  c->out = out;
  c->wr = wr;
  if (ret || out)
    ir->capi_sources = 1;
}

/* a builtin redirected call FROM to the helper call TO (tcc_ir_lt_copy_call) */
void tcc_ir_capi_copy_call(TCCIRState *ir, int from, int to)
{
  struct IRCapiCall rec;
  if (from < 0 || from >= ir->capi_nb_calls || to < 0)
    return;
  rec = ir->capi_calls[from];
  if (!rec.ret && !rec.out && !rec.wr)
    return;
  if (to >= ir->capi_nb_calls)
  {
    int n = to + 16;
    ir->capi_calls = tcc_realloc(ir->capi_calls, n * sizeof rec);
    memset(ir->capi_calls + ir->capi_nb_calls, 0, (n - ir->capi_nb_calls) * sizeof rec);
    ir->capi_nb_calls = n;
  }
  ir->capi_calls[to] = rec;
}

/* OP as a place: STORE's destination is always memory */
static CapiOp capi_op(TCCIRState *ir, IROperand op, int store_dest)
{
  CapiOp o = {CK_NONE, 0, 0, 0};
  int32_t vreg;
  if (irop_is_none(op) || irop_is_immediate(op))
    return o;
  if ((vreg = irop_get_vreg(op)) >= 0)
  {
    o.kind = CK_VREG;
    o.id = vreg;
    if (irop_get_tag(op) != IROP_TAG_STACKOFF)
      o.deref = op.is_lval || store_dest; /* through the vreg's value */
    else if (irop_is_llocal(op))
      o.deref = 1; /* a variable homed in the frame: what its value points to */
    else if (!op.is_lval && !store_dest)
      o.addr = 1; /* &variable */
    return o;
  }
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_is_local(op) && !irop_is_llocal(op) && !op.is_param)
  {
    o.kind = CK_FRAME;
    o.id = irop_get_stack_offset(op);
    o.addr = !op.is_lval && !store_dest;
  }
  return o;
}

static CapiEnt *capi_find(CapiCtx *c, int kind, int32_t id, int create)
{
  for (int i = 0; i < c->nent; i++)
    if (c->ent[i].kind == kind && c->ent[i].id == id)
      return &c->ent[i];
  if (!create)
    return NULL;
  if (c->nent == c->cent)
  {
    c->cent = c->cent ? 2 * c->cent : 16;
    c->ent = tcc_realloc(c->ent, c->cent * sizeof *c->ent);
  }
  memset(&c->ent[c->nent], 0, sizeof c->ent[0]);
  c->ent[c->nent].kind = (uint8_t)kind;
  c->ent[c->nent].id = id;
  c->ent[c->nent].origin = -1;
  return &c->ent[c->nent++];
}

/* the read-only origin of the value of O, -1 none */
static int capi_val(CapiCtx *c, CapiOp o)
{
  CapiEnt *e;
  if (o.kind == CK_NONE || o.deref || o.addr)
    return -1;
  e = capi_find(c, o.kind, o.id, 0);
  return e ? e->origin : -1;
}

/* the place whose address the value of O is (&x, or a temp holding it): 1 */
static int capi_addr_of(CapiCtx *c, CapiOp o, int *kind, int32_t *id)
{
  CapiEnt *e;
  if (o.kind == CK_NONE || o.deref)
    return 0;
  if (o.addr)
  {
    *kind = o.kind, *id = o.id;
    return 1;
  }
  e = capi_find(c, o.kind, o.id, 0);
  if (!e || !e->is_addr)
    return 0;
  *kind = e->akind, *id = e->aid;
  return 1;
}

/* the value of place D is now ORIGIN's (-1: nothing read-only), or the address of (AK, AID) */
static void capi_set(CapiCtx *c, CapiOp d, int origin, int is_addr, int ak, int32_t aid)
{
  CapiEnt *e;
  if (d.kind == CK_NONE || d.deref || d.addr)
    return;
  e = capi_find(c, d.kind, d.id, origin >= 0 || is_addr);
  if (!e)
    return;
  e->origin = origin;
  e->is_addr = (uint8_t)is_addr;
  e->akind = (uint8_t)ak;
  e->aid = aid;
}

static int capi_origin(CapiCtx *c, int callee, int line, int arg)
{
  if (c->norg == c->corg)
  {
    c->corg = c->corg ? 2 * c->corg : 8;
    c->org = tcc_realloc(c->org, c->corg * sizeof *c->org);
  }
  c->org[c->norg].callee = callee;
  c->org[c->norg].line = line;
  c->org[c->norg].arg = arg;
  return c->norg++;
}

static void capi_report(CapiCtx *c, int line, int origin, int callee, int param)
{
  CapiOrigin *o = &c->org[origin];
  char what[160];
  for (int i = 0; i < c->nsaid; i++)
    if (c->said[i] == line)
      return;
  if (!(c->nsaid & (c->nsaid - 1)))
    c->said = tcc_realloc(c->said, (c->nsaid ? 2 * c->nsaid : 8) * sizeof *c->said);
  c->said[c->nsaid++] = line;
  if (o->arg)
    snprintf(what, sizeof what, "what '%s' stored through its argument %d", get_tok_str(o->callee, NULL), o->arg);
  else
    snprintf(what, sizeof what, "the result of '%s'", get_tok_str(o->callee, NULL));
  if (param)
    tcc_warning_once_at(warn_yasc_c_api, line,
                        "%s goes to parameter %d of '%s', which may write through it: its yasc declaration says "
                        "it is read-only",
                        what, param, callee ? get_tok_str(callee, NULL) : "the function");
  else
    tcc_warning_once_at(warn_yasc_c_api, line, "writing through %s: its yasc declaration says it is read-only", what);
  if (o->line != line)
    tcc_note("%i:'%s' is called here", o->line, get_tok_str(o->callee, NULL));
}

static int capi_call_id(TCCIRState *ir, const IRQuadCompact *q, int *idx)
{
  int64_t enc;
  if (!irop_config[q->op].has_src2)
    return -1;
  enc = irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
  if (idx)
    *idx = TCCIR_DECODE_PARAM_IDX(enc);
  return TCCIR_DECODE_CALL_ID(enc);
}

/* the facts of a direct call to CALLEE (a function symbol): 1 when it has any */
static int capi_direct(TCCIRState *ir, IROperand callee, struct IRCapiCall *k)
{
  IRPoolSymref *r;
  int ret = 0;
  uint32_t out = 0, wr = 0;
  if (irop_get_tag(callee) != IROP_TAG_SYMREF || !(r = irop_get_symref_ex(ir, callee)) || !r->sym ||
      (r->sym->type.t & VT_BTYPE) != VT_FUNC || !r->sym->type.ref)
    return 0;
  yasc_capi_call_masks(r->sym->type.ref, &ret, &out, &wr);
  k->callee = r->sym->v;
  k->ret = (uint8_t)ret;
  k->out = out;
  k->wr = wr;
  return ret || out || wr;
}

#define CAPI_MAX_ARGS 32

/* a call whose arguments are being passed (a call among them nests) */
typedef struct CapiPending
{
  int id, nargs;
  CapiOp args[CAPI_MAX_ARGS];
  int line[CAPI_MAX_ARGS];
} CapiPending;

static CapiPending *capi_pending(CapiPending **v, int *n, int *cap, int id, int create)
{
  for (int i = *n; i-- > 0;)
    if ((*v)[i].id == id)
      return &(*v)[i];
  if (!create)
    return NULL;
  if (*n == *cap)
  {
    *cap = *cap ? 2 * *cap : 4;
    *v = tcc_realloc(*v, *cap * sizeof **v);
  }
  memset(&(*v)[*n], 0, sizeof **v);
  (*v)[*n].id = id;
  return &(*v)[(*n)++];
}

void tcc_ir_capi_check(TCCIRState *ir, Sym *func)
{
  CapiCtx c;
  int n = ir->next_instruction_index;
  CapiPending *pend = NULL;
  int npend = 0, cpend = 0;
  (void)func;
  if (!ir->capi_sources)
  {
    tcc_ir_capi_free(ir);
    return;
  }
  memset(&c, 0, sizeof c);
  c.ir = ir;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cq = &irop_config[q->op];
    IROperand dest = cq->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      int idx = 0, id = capi_call_id(ir, q, &idx);
      CapiPending *p = id >= 0 ? capi_pending(&pend, &npend, &cpend, id, 1) : NULL;
      if (p && idx >= 0 && idx < CAPI_MAX_ARGS)
      {
        p->args[idx] = capi_op(ir, tcc_ir_op_get_src1(ir, q), 0);
        p->line[idx] = q->line_num;
        if (idx + 1 > p->nargs)
          p->nargs = idx + 1;
      }
      continue;
    }
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      int id = capi_call_id(ir, q, NULL);
      struct IRCapiCall *k = id >= 0 && id < ir->capi_nb_calls ? &ir->capi_calls[id] : NULL, direct;
      /* a call the frontend made again under another id (a string builtin): its callee's facts */
      if ((!k || (!k->ret && !k->out && !k->wr)) && capi_direct(ir, tcc_ir_op_get_src1(ir, q), &direct))
        k = &direct;
      CapiPending *p = id >= 0 ? capi_pending(&pend, &npend, &cpend, id, 0) : NULL;
      int nargs = p ? p->nargs : 0;
      if (k && k->wr)
        for (int a = 0; a < nargs && a < 32; a++)
        {
          int org;
          if ((k->wr & (1u << a)) && (org = capi_val(&c, p->args[a])) >= 0)
            capi_report(&c, p->line[a], org, k->callee, a + 1);
        }
      if (k && k->out)
        for (int a = 0; a < nargs && a < 32; a++)
        {
          int ak;
          int32_t aid;
          if ((k->out & (1u << a)) && capi_addr_of(&c, p->args[a], &ak, &aid))
          {
            CapiOp place = {(uint8_t)ak, 0, 0, aid};
            capi_set(&c, place, capi_origin(&c, k->callee, q->line_num, a + 1), 0, 0, 0);
          }
        }
      if (q->op == TCCIR_OP_FUNCCALLVAL && !irop_is_none(dest))
        capi_set(&c, capi_op(ir, dest, 0), k && k->ret ? capi_origin(&c, k->callee, q->line_num, 0) : -1, 0, 0, 0);
      if (p)
        *p = pend[--npend]; /* done with it */
      continue;
    }
    if (irop_is_none(dest))
      continue;
    if (q->op == TCCIR_OP_STORE || !irop_dest_defines_vreg(dest))
    {
      CapiOp d = capi_op(ir, dest, q->op == TCCIR_OP_STORE);
      CapiOp a = cq->has_src1 ? capi_op(ir, tcc_ir_op_get_src1(ir, q), 0) : (CapiOp){0, 0, 0, 0};
      int org, ak;
      int32_t aid;
      if (d.deref && d.kind != CK_NONE)
      {
        CapiOp base = d;
        base.deref = 0;
        if ((org = capi_val(&c, base)) >= 0)
          capi_report(&c, q->line_num, org, 0, 0);
        continue;
      }
      if (q->op != TCCIR_OP_STORE)
        continue;
      if (capi_addr_of(&c, a, &ak, &aid))
        capi_set(&c, d, -1, 1, ak, aid);
      else
        capi_set(&c, d, capi_val(&c, a), 0, 0, 0);
      continue;
    }
    {
      CapiOp d = capi_op(ir, dest, 0);
      CapiOp a = cq->has_src1 ? capi_op(ir, tcc_ir_op_get_src1(ir, q), 0) : (CapiOp){0, 0, 0, 0};
      CapiOp b = cq->has_src2 ? capi_op(ir, tcc_ir_op_get_src2(ir, q), 0) : (CapiOp){0, 0, 0, 0};
      int org = -1, ak;
      int32_t aid;
      d.deref = 0, d.addr = 0;
      switch (q->op)
      {
      case TCCIR_OP_LEA:
        a.addr = 1, a.deref = 0;
        /* fall through */
      case TCCIR_OP_ASSIGN:
      case TCCIR_OP_LOAD:
        if (capi_addr_of(&c, a, &ak, &aid))
        {
          capi_set(&c, d, -1, 1, ak, aid);
          continue;
        }
        org = capi_val(&c, a);
        break;
      case TCCIR_OP_ADD:
      case TCCIR_OP_SUB:
        /* pointer arithmetic: into the same object */
        org = capi_val(&c, a);
        if (org < 0 && q->op == TCCIR_OP_ADD)
          org = capi_val(&c, b);
        break;
      default:
        break;
      }
      capi_set(&c, d, org, 0, 0, 0);
    }
  }
  tcc_free(pend);
  tcc_free(c.ent);
  tcc_free(c.org);
  tcc_free(c.said);
  tcc_ir_capi_free(ir);
}
