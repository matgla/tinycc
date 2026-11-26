/*
 * -Wlifetime: yasc ownership flow, checked on the raw IR.
 *
 * The frontend records what the IR cannot say: which locals and parameters
 * are owned pointers (by vreg), where their scopes end, and which parameters
 * of each call take ownership.  Once the body is parsed, before any pass
 * touches it, one forward dataflow over the CFG follows each owned variable:
 *
 *   uninitialised -> owns (a call's result, any value) or null (0)
 *   owns -> moved (into another owned variable, an owned parameter, an owned
 *           return, owned memory -- `n->next = p` where next is owned), with
 *           "freed" when the parameter is `drop`
 *
 * and reports a moved variable read or moved again (double free), and one
 * that still owns memory when it is assigned again, when its scope ends or
 * at a return.  The state is per path: a join ORs the states, so "owns on one
 * path, moved on another" reads as "may still own".  A branch on `p == 0`
 * takes ownership out of the zero edge, so `if (p) free(p);` is clean.
 *
 * Maybe variables are followed for null the same way, and so are `T * maybe`
 * struct members reached through a pointer variable (`q->next`): each
 * (base, member offset) is one more variable in the dataflow (see "maybe
 * members" below for what sets and forgets one).
 *
 * It runs before the passes so that the diagnostics do not depend on -O:
 * the inliner (-O1+) only adds bodies whose variables are "silent" (their
 * function reports them when it is compiled itself), and an argument bound
 * to an inlined owned parameter is a move like any other.  It reads the IR
 * and changes nothing.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ssa.h"

/* per-variable state bits; a join ORs them.  Ownership (owned variables): */
#define LT_UNINIT 1
#define LT_OWN 2
#define LT_MOVED 4
#define LT_NULL 8
#define LT_OWNBITS 15
/* nullness (maybe variables, and every pointer value) */
#define LT_N_NULL 16 /* may be null */
#define LT_N_SET 32  /* may be a pointer */
#define LT_NULLBITS (LT_N_NULL | LT_N_SET)
/* out parameters: has *p been written */
#define LT_W_NO 64   /* may still be unwritten */
#define LT_W_YES 128 /* may be written */
#define LT_WBITS (LT_W_NO | LT_W_YES)

int tcc_ir_lt_active(void)
{
  /* -w hides warnings, not the strict lifetime errors */
  return tcc_state->yasc && tcc_state->ir && (tcc_state->warn_lifetime & WARN_ON) &&
         (!tcc_state->warn_none || tcc_state->strict_lifetime);
}

/* Called when a function's IR starts: in yasc with -Wlifetime its pointer
   variables are recorded and the body checked. */
void tcc_ir_lt_begin(TCCIRState *ir)
{
  ir->lt_on = tcc_ir_lt_active();
  tcc_ir_taint_begin(ir);
  tcc_ir_capi_begin(ir);
  tcc_ir_ab_begin(ir);
}

/* grow a record array by doubling when its count reaches a power of two */
static void lt_sobj_pmask(CType *t, int off, uint64_t *mask, int depth);

static void *lt_grow(void *p, int n, int size)
{
  if (n & (n - 1))
    return p;
  return tcc_realloc(p, (n ? 2 * n : 4) * size);
}

/* a struct (*esize 0), or an array of structs (*esize: an element's size) */
static int lt_struct_or_array(CType *t, int *esize)
{
  int al;
  *esize = 0;
  if ((t->t & (VT_BTYPE | VT_ARRAY)) == VT_STRUCT)
    return 1;
  if ((t->t & VT_ARRAY) && !(t->t & VT_VLA) && t->ref && (t->ref->type.t & (VT_BTYPE | VT_ARRAY)) == VT_STRUCT)
    return (*esize = type_size(&t->ref->type, &al)) > 0;
  return 0;
}

/* a pointer local or parameter: owned ones are followed for ownership,
   maybe ones for null, and plain ones may never hold null */
void tcc_ir_lt_add_var(TCCIRState *ir, int32_t vreg, Sym *sym)
{
  IRLtVar *v;
  int q = type_yasc(&sym->type), handle = type_is_handle(&sym->type);
  /* a local struct, or array of them: their maybe members are followed
     (`s.next`, `a[1].next`) */
  int esize;
  /* a local array or struct: its name, for messages about its memory */
  if (((sym->type.t & VT_ARRAY) || (sym->type.t & VT_BTYPE) == VT_STRUCT) && (sym->r & VT_VALMASK) == VT_LOCAL &&
      !(sym->v & SYM_FIELD) && (sym->v & ~SYM_STRUCT) < SYM_FIRST_ANOM && ir->lt_nb_fnames < 256)
  {
    ir->lt_fnames = lt_grow(ir->lt_fnames, 2 * ir->lt_nb_fnames, sizeof(int32_t));
    ir->lt_fnames[2 * ir->lt_nb_fnames] = (int32_t)sym->c;
    ir->lt_fnames[2 * ir->lt_nb_fnames + 1] = sym->v;
    ir->lt_nb_fnames++;
  }
  if (lt_struct_or_array(&sym->type, &esize) && (sym->r & VT_VALMASK) == VT_LOCAL && ((sym->r & VT_LVAL) || esize) &&
      !(sym->v & SYM_FIELD) && (sym->v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
  {
    int align, size = type_size(&sym->type, &align);
    if (size > 0)
    {
      ir->lt_fobjs = lt_grow(ir->lt_fobjs, ir->lt_nb_fobjs, 5 * sizeof(int));
      int *o = &ir->lt_fobjs[5 * ir->lt_nb_fobjs++];
      o[0] = (int)sym->c, o[1] = size, o[2] = sym->v, o[3] = tcc_state->in_inline_expansion != 0, o[4] = esize;
    }
    return;
  }
  /* an int local or parameter: its name, should it index an array (`a[i]`) */
  {
    int bt = sym->type.t & VT_BTYPE;
    if (vreg >= 0 && !handle && !(sym->type.t & VT_ARRAY) &&
        (bt == VT_BYTE || bt == VT_SHORT || bt == VT_INT || bt == VT_LLONG || bt == VT_BOOL) &&
        (sym->v & ~SYM_STRUCT) < SYM_FIRST_ANOM && ir->lt_nb_inames < 256)
    {
      ir->lt_inames = lt_grow(ir->lt_inames, 2 * ir->lt_nb_inames, sizeof(int32_t));
      ir->lt_inames[2 * ir->lt_nb_inames] = vreg;
      ir->lt_inames[2 * ir->lt_nb_inames + 1] = sym->v;
      ir->lt_nb_inames++;
    }
  }
  if (vreg < 0 || ((sym->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR && !handle))
    return;
  ir->lt_vars = lt_grow(ir->lt_vars, ir->lt_nb_vars, sizeof(IRLtVar));
  v = &ir->lt_vars[ir->lt_nb_vars++];
  v->vreg = vreg;
  v->tok = sym->v;
  v->line = file ? file->line_num : 0;
  v->silent = tcc_state->in_inline_expansion != 0;
  v->untracked = 0;
  /* a pointer declared in regular C (no owned or view: yasc forbids that) is
     unchecked: a view nothing is checked on (docs/yasc_modules.md, "Legacy
     C"); without the mark (a unit with no yasc file), `view maybe` */
  if (q & YASC_UNCHECKED)
    q |= YASC_VIEW;
  else if (!(q & (YASC_OWNED | YASC_VIEW)) && !handle)
  {
    q |= YASC_VIEW | YASC_MAYBE;
    /* in a yasc unit a plain pointer was an error already (error recovery
       goes on after it): nothing more is said of it */
    if (tcc_yasc_unit(tcc_state))
      v->silent = 1;
  }
  v->kind = (q & YASC_OWNED ? LTV_OWNED : 0) | (q & YASC_MAYBE ? LTV_MAYBE : 0) | (q & YASC_OUT ? LTV_OUT : 0) |
            ((q & (YASC_OWNED | YASC_TAKEN)) == (YASC_OWNED | YASC_TAKEN) && !(q & YASC_UNCHECKED) ? LTV_TAKEN : 0) |
            (handle ? LTV_HANDLE : 0) |
            (sym->type.ref && !(sym->type.ref->type.t & VT_CONSTANT) ? LTV_MUT : 0);
  v->outarg = ((q | (sym->type.ref ? type_yasc(&sym->type.ref->type) : 0)) & YASC_OUT) != 0;
  v->arg_report = 0;
  v->arg_index = -1;
  v->arg_callee = 0;
  v->dealloc = type_dealloc(&sym->type);
  /* -fyasc-facts: an exclusive parameter (O4) */
  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM && !v->silent && tcc_state->yasc_facts &&
      tcc_state->strict_lifetime && tcc_state->optimize && ir->yf_nb_excl < 8 &&
      (sym->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && !(type_yasc(&sym->type) & YASC_UNCHECKED) &&
      ((type_yasc(&sym->type) & (YASC_RESTRICT | YASC_VIEW)) == (YASC_RESTRICT | YASC_VIEW) ||
       (type_yasc(&sym->type) & YASC_OWNED)))
    ir->yf_excl[ir->yf_nb_excl++] = vreg;
  /* a checked `struct T * view` parameter: its struct's owned members are
     checked where the function returns; `drop`: they must be released.  A
     read-only one (in, the default, or const) too: it cannot give a member
     a new value, so a member freed or moved through it is left dangling. */
  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM && !v->silent && sym->type.ref &&
      (sym->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (q & YASC_VIEW) &&
      !(type_yasc(&sym->type) & (YASC_UNCHECKED | YASC_OWNED | YASC_OUT)) && (type_yasc(&sym->type) & YASC_VIEW) &&
      (sym->type.ref->type.t & (VT_BTYPE | VT_ARRAY)) == VT_STRUCT && !type_pointee_owned(&sym->type) &&
      ir->lt_nb_selfp < 32)
  {
    int drop = (type_yasc(&sym->type) & YASC_DROP) != 0;
    int ro = (sym->type.ref->type.t & VT_CONSTANT) != 0;
    ir->lt_selfp = lt_grow(ir->lt_selfp, ir->lt_nb_selfp, 2 * sizeof(int32_t));
    ir->lt_selfp[2 * ir->lt_nb_selfp] = vreg;
    ir->lt_selfp[2 * ir->lt_nb_selfp + 1] = drop | ro << 1; /* bit 1: its target is read-only */
    ir->lt_nb_selfp++;
    for (Sym *f = drop && sym->type.ref->type.ref ? sym_first(sym->type.ref->type.ref) : NULL; f; f = f->next)
    {
      int fq = type_yasc(&f->type), fh = type_is_handle(&f->type);
      if (!(fq & YASC_OWNED) || (fq & YASC_UNCHECKED) || (f->type.t & VT_BITFIELD) ||
          ((f->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR && !fh) || (f->v & ~SYM_FIELD) >= SYM_FIRST_ANOM)
        continue;
      ir->lt_dmembers = lt_grow(ir->lt_dmembers, ir->lt_nb_dmembers, sizeof(IRLtMember));
      IRLtMember *m = &ir->lt_dmembers[ir->lt_nb_dmembers++];
      m->vreg = vreg;
      m->offset = m->foff = f->c;
      m->tok = f->v & ~SYM_FIELD;
      m->owned = LT_MEMBER_OWNED | ((fq & YASC_MAYBE) ? 0 : LT_MEMBER_NOTMAYBE) | (fh ? LT_MEMBER_HANDLE : 0);
      m->esize = 0;
    }
  }
  /* a restrict local: its borrow is checked while it is in use */
  if ((type_yasc(&sym->type) & YASC_RESTRICT) && TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_VAR && !v->silent)
  {
    ir->lt_rlocals = lt_grow(ir->lt_rlocals, 2 * ir->lt_nb_rlocals, sizeof(int32_t));
    ir->lt_rlocals[2 * ir->lt_nb_rlocals] = vreg;
    ir->lt_rlocals[2 * ir->lt_nb_rlocals + 1] = sym->v;
    ir->lt_nb_rlocals++;
  }
}

static void lt_add_event(TCCIRState *ir, int var, int line)
{
  IRLtEvent *e;
  ir->lt_events = lt_grow(ir->lt_events, ir->lt_nb_events, sizeof(IRLtEvent));
  e = &ir->lt_events[ir->lt_nb_events++];
  e->idx = ir->next_instruction_index;
  e->var = var;
  e->line = line;
}

/* the jump about to be emitted is a `return` */
void tcc_ir_lt_return(TCCIRState *ir, int line)
{
  lt_add_event(ir, LT_EV_RETURN, line);
}

/* the frontend's mark: &x, or captured by a nested function, which may
   read or set it behind the analysis */
static int lt_vreg_addrtaken(TCCIRState *ir, int32_t vreg)
{
  IRLiveInterval *iv = tcc_ir_try_get_live_interval(ir, vreg);
  return iv && iv->addrtaken;
}

static int lt_var_by_vreg(const TCCIRState *ir, int32_t vreg)
{
  for (int i = 0; i < ir->lt_nb_vars; i++)
    if (ir->lt_vars[i].vreg == vreg)
      return i;
  return -1;
}

static int lt_self_kind(Sym *pa);

/* yasc: the local of VREG, a `view maybe` while its body was parsed, is a
   plain pointer the body owns, an error already (plain_param.c): nothing
   more is said of it */
void tcc_ir_lt_silence(TCCIRState *ir, int32_t vreg)
{
  int v = ir && ir->lt_on && vreg >= 0 ? lt_var_by_vreg(ir, vreg) : -1;
  if (v >= 0)
    ir->lt_vars[v].silent = 1;
}


/* An inlined callee's parameter: what the body does with it is the callee's
   to report; the argument stored into it is the caller's, as for a call. */
void tcc_ir_lt_inline_param(TCCIRState *ir, int32_t vreg, Sym *callee, int index)
{
  int v = vreg >= 0 ? lt_var_by_vreg(ir, vreg) : -1;
  /* the outermost inlined call's parameters, for its borrowing-struct result */
  if (!tcc_state->in_inline_expansion && index >= 0 && index < YASC_BORROWS_MAX_PARAMS)
    ir->lt_ip_var[index] = v;
  if (v < 0)
    return;
  ir->lt_vars[v].silent = 1;
  ir->lt_vars[v].arg_index = index;
  ir->lt_vars[v].arg_callee = callee ? callee->v : 0;
  /* a self parameter: binding the argument is the handing over */
  if (callee && index >= 0 && (callee->type.t & VT_BTYPE) == VT_FUNC && callee->type.ref)
  {
    Sym *pa = sym_first(callee->type.ref);
    for (int k = 0; pa && k < index; k++)
      pa = pa->next;
    int sk = pa ? lt_self_kind(pa) : 0;
      if (sk)
    {
      ir->lt_iself = lt_grow(ir->lt_iself, ir->lt_nb_iself, 3 * sizeof(int32_t));
      ir->lt_iself[3 * ir->lt_nb_iself] = vreg;
      ir->lt_iself[3 * ir->lt_nb_iself + 1] = sk;
      ir->lt_iself[3 * ir->lt_nb_iself + 2] = callee->v;
      ir->lt_nb_iself++;
    }
  }
  /* the call is itself in an inlined body: that body's function reports it */
  ir->lt_vars[v].arg_report = !tcc_state->in_inline_expansion;
}

/* the result variable of an inlined call: a silent pointer variable */
static int lt_result_var(TCCIRState *ir, int32_t vreg, int kind)
{
  IRLtVar *v;
  ir->lt_vars = lt_grow(ir->lt_vars, ir->lt_nb_vars, sizeof(IRLtVar));
  v = &ir->lt_vars[ir->lt_nb_vars++];
  memset(v, 0, sizeof *v);
  v->vreg = vreg;
  v->silent = 1;
  v->kind = kind;
  v->arg_index = -1;
  return ir->lt_nb_vars - 1;
}

/* An inlined call's result variable (-O1+, outermost), at the end of the
   body: a `T * maybe` result may be null where the caller uses it, whatever
   the body returned, and a view result borrows what the parameters it
   borrows from (`borrows(s)`, or all of them) were bound to -- as for a call
   that is not inlined, so that the diagnostics do not depend on -O.  It is
   silent; its value dereferenced is reported as a value with no name. */
void tcc_ir_lt_inline_result(TCCIRState *ir, int32_t vreg, Sym *ftype)
{
  int rq, bw, var = -1, n = 0;
  if (!ir || !ir->lt_on || vreg < 0 || !ftype || (ftype->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR)
    return;
  rq = type_yasc(&ftype->type);
  if (rq & YASC_UNCHECKED)
    return;
  if ((rq & YASC_MAYBE) || !(rq & (YASC_OWNED | YASC_VIEW)))
  {
    var = lt_result_var(ir, vreg, LTV_MAYBE | LTV_RESULT);
    lt_add_event(ir, LT_EV_RESULT(var), file ? file->line_num : 0);
  }
  bw = (rq & YASC_VIEW) ? (int)(type_borrows(&ftype->type) & YASC_BORROWS_ARGS) : 0;
  if (!bw)
    return;
  if (var < 0)
    var = lt_result_var(ir, vreg, 0);
  ir->lt_pres = lt_grow(ir->lt_pres, ir->lt_nb_pres, 6 * sizeof(int32_t));
  int32_t *r = &ir->lt_pres[6 * ir->lt_nb_pres++];
  r[0] = var;
  for (int k = 0; k < YASC_BORROWS_MAX_PARAMS; k++)
    if ((bw >> k) & 1)
    {
      if (n == 4)
      {
        n = -1;
        break;
      }
      r[2 + n++] = ir->lt_ip_var[k];
    }
  r[1] = n;
}


/* An inlined call's result (-O1+, outermost) at frame offset LOC, of
   function type FTYPE: a borrowing struct borrows what the parameters its
   type names (`struct T borrows(src)`) were bound to -- as the call's result
   would, so that the diagnostics do not depend on -O.  The result's place
   is followed as a local struct (silent: it has no name). */
void tcc_ir_lt_inline_sresult(TCCIRState *ir, int loc, Sym *ftype, Sym *callee)
{
  int code, n = 0, k;
  int32_t *r;
  if (!ftype || !callee || !yasc_borrowing_struct(&ftype->type) || (code = yasc_struct_result_code(callee->v)) <= 0)
    return;
  code &= YASC_BORROWS_ARGS;
  for (k = 0; k < ir->lt_nb_sobjs && ir->lt_sobjs[k].off != loc; k++)
    ;
  if (k == ir->lt_nb_sobjs)
  {
    int al, size = type_size(&ftype->type, &al);
    uint64_t mask = 0;
    if (size <= 0 || size > 256 || ir->lt_nb_sobjs >= 32)
      return;
    lt_sobj_pmask(&ftype->type, 0, &mask, 0);
    if (!mask)
      return;
    ir->lt_sobjs = lt_grow(ir->lt_sobjs, ir->lt_nb_sobjs, sizeof(IRLtSobj));
    IRLtSobj *o = &ir->lt_sobjs[ir->lt_nb_sobjs++];
    o->off = loc, o->size = size, o->tok = 0, o->pmask = mask, o->silent = 1;
  }
  ir->lt_sres = lt_grow(ir->lt_sres, ir->lt_nb_sres, 7 * sizeof(int32_t));
  r = &ir->lt_sres[7 * ir->lt_nb_sres++];
  r[0] = ir->next_instruction_index;
  r[1] = loc;
  for (k = 0; k < YASC_BORROWS_MAX_PARAMS; k++)
    if ((code >> k) & 1)
    {
      if (n == 4)
      {
        n = -1;
        break;
      }
      r[3 + n++] = ir->lt_ip_var[k];
    }
  r[2] = n;
}

/* The scope that declared the symbols from `top` down to `bottom` (not
   included) ends here, or is left by break/continue. */
void tcc_ir_lt_scope_end(TCCIRState *ir, Sym *top, Sym *bottom)
{
  if (!ir || !ir->lt_nb_vars)
    return;
  for (Sym *s = top; s && s != bottom; s = s->prev)
  {
    int var;
    if (s->vreg >= 0 && (var = lt_var_by_vreg(ir, s->vreg)) >= 0)
      lt_add_event(ir, var, file ? file->line_num : 0);
  }
}

static IRLtCall *lt_call(TCCIRState *ir, int call_id)
{
  if (call_id < 0)
    return NULL;
  if (call_id >= ir->lt_nb_calls)
  {
    int n = call_id + 16;
    ir->lt_calls = tcc_realloc(ir->lt_calls, n * sizeof(IRLtCall));
    memset(ir->lt_calls + ir->lt_nb_calls, 0, (n - ir->lt_nb_calls) * sizeof(IRLtCall));
    ir->lt_nb_calls = n;
  }
  return &ir->lt_calls[call_id];
}

/* -fyasc-facts (S4).  From the callee's declared yasc parameter types --
 * the contract the view-escape check holds its body to in the safe default:
 * a `view` (or borrows) parameter does not escape the call unless the
 * result says it points into it (borrows(...), a borrowing struct). */
/* The IR keeps 16 bits of a call id (TCCIR_ENCODE_CALL / _PARAM): past
   that, a decoded id names an earlier call too, so a function with more
   calls than that takes no facts (yf_lookup) */
#define YF_CALL_ID_MAX 0xFFFF

static const IRYfCall *yf_lookup(const TCCIRState *ir, int call_id)
{
  if (call_id < 0 || call_id >= ir->yf_nb_calls || ir->next_call_id > YF_CALL_ID_MAX + 1 ||
      !ir->yf_calls[call_id].noted)
    return NULL;
  return &ir->yf_calls[call_id];
}

void tcc_ir_yf_note_call(TCCIRState *ir, int call_id, Sym *ftype)
{
  uint32_t noescape = 0, readonly = 0, through_result = 0, drop = 0;
  int i = 0, rq = type_yasc(&ftype->type);
  if (call_id < 0 || call_id > YF_CALL_ID_MAX)
    return;
  if ((rq & YASC_VIEW) && !(rq & YASC_UNCHECKED))
    through_result = (uint32_t)type_borrows(&ftype->type) & YASC_BORROWS_ARGS;
  if (yasc_borrowing_struct(&ftype->type))
    through_result = ~0u; /* a borrowing struct result: what it borrows is not said here */
  for (Sym *pa = sym_first(ftype); pa && i < 32; pa = pa->next, i++)
  {
    int q = type_yasc(&pa->type);
    if ((pa->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (q & (YASC_OWNED | YASC_DROP)) == (YASC_OWNED | YASC_DROP) &&
        !(q & YASC_UNCHECKED) && !(q & YASC_TAKEN))
      drop |= 1u << i;
    if ((pa->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR || !(q & YASC_VIEW) || (q & (YASC_OWNED | YASC_UNCHECKED)) ||
        ((through_result >> i) & 1))
      continue;
    noescape |= 1u << i;
    if (pa->type.ref && (pa->type.ref->type.t & VT_CONSTANT))
      readonly |= 1u << i;
  }
  if (call_id >= ir->yf_nb_calls)
  {
    int n = call_id + 16;
    ir->yf_calls = tcc_realloc(ir->yf_calls, n * sizeof(IRYfCall));
    memset(ir->yf_calls + ir->yf_nb_calls, 0, (n - ir->yf_nb_calls) * sizeof(IRYfCall));
    ir->yf_nb_calls = n;
  }
  ir->yf_calls[call_id].noescape = noescape;
  ir->yf_calls[call_id].readonly = readonly;
  ir->yf_calls[call_id].drop = drop;
  /* an owned result is new memory; takes(p) hands back what p owned */
  ir->yf_calls[call_id].fresh = (ftype->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (rq & YASC_OWNED) &&
                                !(rq & YASC_UNCHECKED) && !(rq & YASC_TAKEN);
  ir->yf_calls[call_id].noted = 1;
}

int tcc_ir_yf_param(const TCCIRState *ir, int call_id, int pidx, int *readonly)
{
  const IRYfCall *y = yf_lookup(ir, call_id);
  if (!y)
    return 0;
  /* the struct-return buffer is IR parameter 0 */
  if (call_id < ir->sret_calls_size && ir->sret_calls[call_id])
    pidx--;
  if (pidx < 0 || pidx >= 32 || !((y->noescape >> pidx) & 1))
    return 0;
  if (readonly)
    *readonly = (y->readonly >> pidx) & 1;
  return 1;
}

int tcc_ir_yf_param_drop(const TCCIRState *ir, int call_id, int pidx)
{
  const IRYfCall *y = yf_lookup(ir, call_id);
  if (!y)
    return 0;
  if (call_id < ir->sret_calls_size && ir->sret_calls[call_id])
    pidx--;
  return pidx >= 0 && pidx < 32 && ((y->drop >> pidx) & 1);
}

int tcc_ir_yf_result_fresh(const TCCIRState *ir, int call_id)
{
  const IRYfCall *y = yf_lookup(ir, call_id);
  return y && y->fresh;
}

/* ---- -fyasc-facts (S4, O4 and O2): exclusive parameters, fresh results -----
 *
 * A `T * restrict view` or `T * owned` parameter P: nothing else the caller
 * has reaches what P points to while this call runs (the caller's S3 checks,
 * the move).  Inside the function, the pointers into that object are the
 * values derived from P (copies, P + n) -- as long as none of them leaves:
 * every use of such a value must be a memory access through it, a
 * comparison, a parameter that does not escape (tcc_ir_yf_param) or another
 * derivation; otherwise the fact is dropped for the function.  Flow-
 * insensitive over the flat IR (vars and params have several defs):
 *   m: may be derived from P (any def derives from it),
 *   d: is derived from P (every def derives from d; P's entry value is).
 * A load through a base in d and a store through a base not in m (or the
 * other way round) cannot touch the same memory.
 *
 * O2: a call's `owned` result is the same kind of root -- new memory that
 * nothing older points into -- and handing it to a `drop` parameter (free)
 * is a use that keeps it in: the callee destroys it, and the lifetime check
 * forbids using it after. */
typedef struct YfAlias
{
  int n;          /* exclusive parameters kept */
  uint8_t *m[8], *d[8];
  int nvar, ntemp, nparam;
} YfAlias;

static int yfa_idx(const YfAlias *a, int32_t vr)
{
  if (vr < 0)
    return -1;
  int t = TCCIR_DECODE_VREG_TYPE(vr), p = TCCIR_DECODE_VREG_POSITION(vr);
  if (t == TCCIR_VREG_TYPE_VAR)
    return p < a->nvar ? p : -1;
  if (t == TCCIR_VREG_TYPE_TEMP)
    return p < a->ntemp ? a->nvar + p : -1;
  if (t == TCCIR_VREG_TYPE_PARAM)
    return p < a->nparam ? a->nvar + a->ntemp + p : -1;
  return -1;
}

static int yfa_derives(int op)
{
  return op == TCCIR_OP_ASSIGN || op == TCCIR_OP_LEA || op == TCCIR_OP_ADD || op == TCCIR_OP_SUB;
}

/* operand O read as a value (not memory through it) */
static int yfa_val(const YfAlias *a, IROperand o)
{
  /* a VREG operand, or a variable's own slot named in place (lval STACKOFF:
     irop_is_vreg_value); a frame address has no vreg */
  if (irop_is_none(o) || !irop_is_vreg_value(o))
    return -1;
  return yfa_idx(a, irop_get_vreg(o));
}

/* every phi (SSA: a VAR's values meet at joins in IRSSAState.block_phis,
   outside the instruction array; flat IR: none) */
#define YFA_FOR_PHI(ssa, ph)                                                                                         \
  for (int b_ = 0; (ssa) && b_ < (ssa)->cfg->num_blocks; b_++)                                                       \
    for (IRPhiNode *ph = (ssa)->block_phis[b_]; ph; ph = ph->next)

static void yfa_build(TCCIRState *ir, struct IRSSAState *ssa, YfAlias *a)
{
  int n = ir->next_instruction_index;
  memset(a, 0, sizeof *a);
  a->nvar = ir->next_local_variable, a->ntemp = ir->next_temporary_variable, a->nparam = ir->next_parameter;
  if (ssa && ssa->cfg && TCCIR_DECODE_VREG_POSITION(ssa->next_ssa_vreg) > a->ntemp)
    a->ntemp = TCCIR_DECODE_VREG_POSITION(ssa->next_ssa_vreg);
  if (ssa && (!ssa->cfg || !ssa->block_phis))
    ssa = NULL;
  int nv = a->nvar + a->ntemp + a->nparam + 1;
  /* the roots: exclusive parameters (O4), then fresh owned call results (O2:
     new memory nothing older points into), 8 at most */
  int32_t root[8];
  int root_def[8], nroot = 0;
  for (int e = 0; e < ir->yf_nb_excl && nroot < 8; e++)
    root[nroot] = ir->yf_excl[e], root_def[nroot++] = -1;
  for (int i = 0; i < n && nroot < 8; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL ||
        !tcc_ir_yf_result_fresh(ir, TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q))))
      continue;
    IROperand dst = tcc_ir_op_get_dest(ir, q);
    if (!dst.is_lval && irop_dest_defines_vreg(dst) && irop_get_vreg(dst) >= 0)
      root[nroot] = irop_get_vreg(dst), root_def[nroot++] = i;
  }
  for (int e = 0; e < nroot; e++)
  {
    int pi = yfa_idx(a, root[e]), ok = 1;
    if (pi < 0)
      continue;
    uint8_t *m = tcc_mallocz(nv), *d;
    m[pi] = 1;
    /* m: closure over copies and arithmetic */
    for (int changed = 1; changed;)
    {
      changed = 0;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (!yfa_derives(q->op) || !irop_config[q->op].has_dest)
          continue;
        IROperand dst = tcc_ir_op_get_dest(ir, q);
        int di = irop_dest_defines_vreg(dst) ? yfa_idx(a, irop_get_vreg(dst)) : -1, s1, s2;
        if (di < 0 || m[di])
          continue;
        s1 = yfa_val(a, tcc_ir_op_get_src1(ir, q));
        s2 = irop_config[q->op].has_src2 ? yfa_val(a, tcc_ir_op_get_src2(ir, q)) : -1;
        if ((s1 >= 0 && m[s1]) || (s2 >= 0 && m[s2]))
          m[di] = 1, changed = 1;
      }
      /* a phi with an operand in m may be that operand */
      YFA_FOR_PHI(ssa, ph)
      {
        int di = yfa_idx(a, ph->dest_vreg);
        if (di < 0 || m[di])
          continue;
        for (int k = 0; k < ph->num_operands; k++)
        {
          int si = yfa_idx(a, ph->operands[k].vreg);
          if (si >= 0 && m[si])
          {
            m[di] = 1, changed = 1;
            break;
          }
        }
      }
    }
    /* every use of a value in m keeps it in */
    for (int i = 0; i < n && ok; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      const IRRegistersConfig *rc = &irop_config[q->op];
      IROperand ops[3] = {rc->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none(),
                          rc->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none(),
                          rc->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none()};
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (tcc_ir_op_is_mac(q->op) || q->op == TCCIR_OP_SELECT || q->op == TCCIR_OP_LOAD_INDEXED ||
          q->op == TCCIR_OP_STORE_INDEXED)
      {
        /* the fourth operand (an index, an accumulator, a select arm) */
        IROperand x = ir->iroperand_pool[q->operand_base + (rc->has_dest + rc->has_src1 + rc->has_src2)];
        int xi = yfa_idx(a, irop_get_vreg(x));
        if (xi >= 0 && m[xi] && irop_is_vreg_value(x))
          ok = 0;
      }
      for (int k = 0; k < 3 && ok; k++)
      {
        IROperand o = ops[k];
        int vi = irop_is_none(o) ? -1 : yfa_idx(a, irop_get_vreg(o)), ro = 0;
        if (vi < 0 || !m[vi])
          continue;
        if (o.is_lval && !irop_is_vreg_value(o))
          continue; /* memory through it */
        if (k == 0 && irop_dest_defines_vreg(o))
          continue; /* its definition */
        if ((q->op == TCCIR_OP_LOAD_INDEXED && k == 1) || (q->op == TCCIR_OP_STORE_INDEXED && k == 0))
          continue; /* an indexed access's base */
        if (q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_TEST_ZERO)
          continue;
        if (yfa_derives(q->op) && k > 0)
          continue; /* derives the dest, which is in m */
        if (q->op == TCCIR_OP_FUNCPARAMVAL && k == 1)
        {
          /* a parameter that does not escape it, or one that destroys it */
          uint32_t enc = (uint32_t)tcc_ir_op_src2_imm(ir, q);
          if (tcc_ir_yf_param(ir, TCCIR_DECODE_CALL_ID(enc), TCCIR_DECODE_PARAM_IDX(enc), &ro) ||
              tcc_ir_yf_param_drop(ir, TCCIR_DECODE_CALL_ID(enc), TCCIR_DECODE_PARAM_IDX(enc)))
            continue;
        }
        ok = 0;
      }
    }
    if (!ok)
    {
      tcc_free(m);
      continue;
    }
    /* d: the members of m every def of which derives from d */
    d = tcc_malloc(nv);
    memcpy(d, m, nv);
    for (int changed = 1; changed;)
    {
      changed = 0;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (!irop_config[q->op].has_dest || q->op == TCCIR_OP_NOP)
          continue;
        IROperand dst = tcc_ir_op_get_dest(ir, q);
        int di = irop_dest_defines_vreg(dst) ? yfa_idx(a, irop_get_vreg(dst)) : -1, s1, s2;
        if (di < 0 || !d[di] || i == root_def[e])
          continue; /* (the root's own call defines it) */
        s1 = irop_config[q->op].has_src1 ? yfa_val(a, tcc_ir_op_get_src1(ir, q)) : -1;
        s2 = irop_config[q->op].has_src2 ? yfa_val(a, tcc_ir_op_get_src2(ir, q)) : -1;
        if (!yfa_derives(q->op) || !((s1 >= 0 && d[s1]) || (s2 >= 0 && d[s2])))
          d[di] = 0, changed = 1;
      }
      /* a phi is in d when every operand is */
      YFA_FOR_PHI(ssa, ph)
      {
        int di = yfa_idx(a, ph->dest_vreg);
        if (di < 0 || !d[di])
          continue;
        for (int k = 0; k < ph->num_operands; k++)
        {
          int si = yfa_idx(a, ph->operands[k].vreg);
          if (si < 0 || !d[si])
          {
            d[di] = 0, changed = 1;
            break;
          }
        }
      }
    }
    a->m[a->n] = m, a->d[a->n] = d, a->n++;
  }
}

static void yfa_free(YfAlias *a)
{
  for (int k = 0; k < a->n; k++)
    tcc_free(a->m[k]), tcc_free(a->d[k]);
  a->n = 0;
}

/* can memory through bases X and Y not be the same (one derived from an
   exclusive parameter, the other not)? */
int tcc_ir_yf_alias_apart(const struct YfAlias *a, int32_t x, int32_t y)
{
  int xi = yfa_idx(a, x), yi = yfa_idx(a, y);
  if (!a || xi < 0 || yi < 0)
    return 0;
  for (int k = 0; k < a->n; k++)
    if ((a->d[k][xi] && !a->m[k][yi]) || (a->d[k][yi] && !a->m[k][xi]))
      return 1;
  return 0;
}

/* The exclusive-parameter alias facts for other passes (LICM, opt.h). */
struct YfAlias *tcc_ir_yf_alias_new(TCCIRState *ir, struct IRSSAState *ssa)
{
  YfAlias *a;
  if (!ir->yf_nb_excl && !ir->yf_calls)
    return NULL;
  a = tcc_malloc(sizeof *a);
  yfa_build(ir, ssa, a);
  if (!a->n)
  {
    tcc_free(a);
    return NULL;
  }
  return a;
}

void tcc_ir_yf_alias_delete(struct YfAlias *a)
{
  if (a)
    yfa_free(a), tcc_free(a);
}

/* 1 + k when every value of VR derives from exclusive parameter k, 0 none */
int tcc_ir_yf_alias_excl(const struct YfAlias *a, int32_t vr)
{
  int i = a ? yfa_idx(a, vr) : -1;
  for (int k = 0; i >= 0 && k < a->n; k++)
    if (a->d[k][i])
      return k + 1;
  return 0;
}

/* may a value of VR derive from exclusive parameter EXCL - 1 (an unknown
   vreg: yes) */
int tcc_ir_yf_alias_may(const struct YfAlias *a, int excl, int32_t vr)
{
  int i = yfa_idx(a, vr);
  return i < 0 || a->m[excl - 1][i];
}

/* the parameters of the function type `ftype` that take ownership */
/* parameter pa as a method's self: 1 a checked `T * view` to memory that is
   not const, 2 a const one, 3 `drop struct T * view`; 0 none (owned, out,
   unchecked, an object owned as a whole) */
static int lt_self_kind(Sym *pa)
{
  int q = type_yasc(&pa->type);
  if (!(q & YASC_VIEW) || (q & (YASC_UNCHECKED | YASC_OWNED | YASC_OUT)) ||
      (pa->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR || !pa->type.ref || type_pointee_owned(&pa->type))
    return 0;
  if (q & YASC_DROP)
    return (pa->type.ref->type.t & (VT_BTYPE | VT_ARRAY)) == VT_STRUCT ? 3 : 0;
  return (pa->type.ref->type.t & VT_CONSTANT) ? 2 : 1;
}

/* an inlined callee's self parameter (lt_self_kind), *tok its callee */
static int lt_iself_of(const TCCIRState *ir, int32_t vreg, int *tok)
{
  for (int k = 0; vreg >= 0 && k < ir->lt_nb_iself; k++)
    if (ir->lt_iself[3 * k] == vreg)
    {
      *tok = ir->lt_iself[3 * k + 2];
      return ir->lt_iself[3 * k + 1];
    }
  return 0;
}

void tcc_ir_lt_note_call(TCCIRState *ir, int call_id, Sym *ftype, Sym *callee)
{
  IRLtCall *c;
  uint32_t owned = 0, drop = 0, nonnull = 0, keep = 0, nonnull_attr = 0;
  int drop_dealloc = 0;
  uint32_t taken = 0, handles = 0, restricts = 0, outown = 0, outown_out = 0, outown_maybe = 0, dropobj = 0, objout = 0;
  uint32_t vmem = 0, dropmem = 0, tborrow = 0, tbsrc = 0, out = 0;
  int dropobj_dealloc = 0;
  int i = 0;
  for (Sym *pa = sym_first(ftype); pa && i < 32; pa = pa->next, i++)
  {
    int q = type_yasc(&pa->type);
    /* `out T * borrows(s) * view p`: the callee writes a pointer into s there */
    uint32_t tb = (uint32_t)type_target_borrows(&pa->type) & YASC_BORROWS_ARGS;
    if (tb && !(q & YASC_UNCHECKED))
      tborrow |= 1u << i, tbsrc |= tb;
    /* the callee cannot write through this argument: maybe members of what
       it points to stay as they were tested */
    if ((pa->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR || !pa->type.ref || (pa->type.ref->type.t & VT_CONSTANT))
      keep |= 1u << i;
    if (q & YASC_OWNED)
      owned |= 1u << i;
    if ((q & YASC_DROP) && !drop)
      drop_dealloc = type_dealloc(&pa->type);
    if ((q & (YASC_OWNED | YASC_TAKEN)) == (YASC_OWNED | YASC_TAKEN) && !(q & YASC_UNCHECKED))
      taken |= 1u << i;
    if (type_is_handle(&pa->type))
      handles |= 1u << i;
    if ((q & YASC_RESTRICT) && !(q & YASC_UNCHECKED))
      restricts |= 1u << i;
    /* `out T * p`; `out T * owned * view p` says it on the pointer p points at */
    if (!(q & YASC_UNCHECKED) && (pa->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR &&
        ((q & YASC_OUT) || (pa->type.ref && (type_yasc(&pa->type.ref->type) & YASC_OUT))))
      out |= 1u << i;
    /* `out T owned(release) * view p` writes such an object, `drop` releases it */
    if (type_pointee_owned(&pa->type))
    {
      if (q & YASC_DROP)
      {
        if (!dropobj)
          dropobj_dealloc = type_pointee_owned(&pa->type);
        dropobj |= 1u << i;
      }
      else
      {
        outown |= 1u << i;
        objout |= 1u << i;
        if (q & YASC_OUT)
          outown_out |= 1u << i;
      }
    }
    /* `char * owned maybe * view lineptr`, `out void * owned maybe * view p` */
    else if ((pa->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && pa->type.ref)
    {
      CType *pt = &pa->type.ref->type;
      int tq = type_yasc(pt);
      /* (`char * owned const * view`: the callee cannot replace it, only read it) */
      if ((tq & YASC_OWNED) && !(tq & YASC_UNCHECKED) && !(pt->t & VT_CONSTANT) &&
          ((pt->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR || type_is_handle(pt)))
      {
        outown |= 1u << i;
        if ((tq | q) & YASC_OUT)
          outown_out |= 1u << i;
        if (tq & YASC_MAYBE)
          outown_maybe |= 1u << i;
      }
    }
    if (q & YASC_DROP)
      drop |= 1u << i;
    /* a checked `T * view` (a method's self): the owned members of what it
       points at stay valid; `drop struct T * view` releases them */
    if (!((outown >> i) & 1))
    {
      int sk = lt_self_kind(pa);
      if (sk == 3)
        dropmem |= 1u << i;
      else if (sk)
        vmem |= 1u << i;
    }
    /* declared in yasc (owned or view) without maybe: never null; an
       unchecked pointer from regular C takes anything */
    if ((q & (YASC_OWNED | YASC_VIEW)) && !(q & YASC_MAYBE))
      nonnull |= 1u << i;
    /* __attribute__((nonnull)) (a regular-C header): not maybe either,
       unless yasc says maybe */
    else if (pa->a.nonnull && (pa->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR &&
             !((q & YASC_MAYBE) && (q & (YASC_OWNED | YASC_VIEW)) && !(q & YASC_UNCHECKED)))
      nonnull |= 1u << i, nonnull_attr |= 1u << i;
  }
  /* a result that may be null: maybe -- not an unchecked pointer from regular
     C, which says nothing (a malloc attribute makes it `owned maybe`) */
  int rq = type_yasc(&ftype->type);
  int ret_maybe = ((ftype->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && !(rq & YASC_UNCHECKED) &&
                   ((rq & YASC_MAYBE) || !(rq & (YASC_OWNED | YASC_VIEW)))) ||
                  (type_is_handle(&ftype->type) && (rq & YASC_MAYBE));
  /* every call is recorded: which arguments it may write through */
  if (!(c = lt_call(ir, call_id)))
    return;
  c->keep = keep;
  /* which globals it cannot write: a direct call only, and no callback */
  c->gkeep = 0;
  if (callee)
  {
    int fnptr = 0;
    for (Sym *pa = sym_first(ftype); pa && !fnptr; pa = pa->next)
      fnptr = (pa->type.t & VT_BTYPE) == VT_FUNC ||
              ((pa->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && pa->type.ref &&
               (pa->type.ref->type.t & VT_BTYPE) == VT_FUNC);
    c->gkeep = ftype->f.func_leaf ? LT_GKEEP_LEAF : ftype->f.func_sys_decl && !fnptr ? LT_GKEEP_SYS : 0;
  }
  c->nparams = i;
  c->noted = 1;
  c->ret_maybe = ret_maybe;
  c->nonnull = nonnull;
  c->nonnull_attr = nonnull_attr;
  if (nonnull)
    ir->lt_nonnull_calls = 1;
  if (ret_maybe)
    ir->lt_maybe_calls = 1;
  c->callee = callee ? callee->v : 0;
  c->in_inline = tcc_state->in_inline_expansion != 0;
  c->owned = owned;
  c->drop = drop;
  c->known = 1;
  c->ret_dealloc = type_dealloc(&ftype->type);
  c->drop_dealloc = drop_dealloc;
  c->taken = taken;
  c->handles = handles;
  c->restricts = restricts;
  c->out = out;
  c->outown = outown;
  c->outown_out = outown_out;
  c->outown_maybe = outown_maybe;
  c->dropobj = dropobj;
  c->objout = objout;
  c->vmem = vmem;
  c->dropmem = dropmem;
  c->dropobj_dealloc = dropobj_dealloc;
  c->borrows = (rq & YASC_VIEW) && !(rq & YASC_UNCHECKED) ? (uint32_t)type_borrows(&ftype->type) & YASC_BORROWS_ARGS : 0;
  c->tborrow = tborrow;
  c->tbsrc = tbsrc;
  c->sborrows = 0;
  c->ssize = 0;
  c->spmask = 0;
  if (yasc_borrowing_struct(&ftype->type))
  {
    int code = callee ? yasc_struct_result_code(callee->v) : -1, al, size = type_size(&ftype->type, &al);
    c->sborrows = code > 0 ? (uint32_t)code & YASC_BORROWS_ARGS : 0;
    if (size > 0 && size <= 256)
    {
      c->ssize = size;
      lt_sobj_pmask(&ftype->type, 0, &c->spmask, 0);
    }
  }
}

/* A builtin redirected call FROM to the helper call TO: the helper call
   stands for the declared one -- its result may be null as that one's, and
   its arguments may not be where that one's may not */
void tcc_ir_lt_copy_call(TCCIRState *ir, int from, int to)
{
  IRLtCall rec, *c;
  tcc_ir_capi_copy_call(ir, from, to); /* -Wyasc-c-api: the same */
  if (from < 0 || from >= ir->lt_nb_calls || !ir->lt_calls[from].noted)
    return;
  rec = ir->lt_calls[from];
  if ((c = lt_call(ir, to)))
    *c = rec;
}

/* The instructions [from, to) stored a value into an owned pointer in memory
   (vstore: `n->text = text` with text a `char * owned` member): an owned
   variable stored there moves into it. */
void tcc_ir_lt_note_owned_store(TCCIRState *ir, int from, int to)
{
  if (to <= from)
    return;
  ir->lt_ostores = lt_grow(ir->lt_ostores, ir->lt_nb_ostores, 2 * sizeof(int));
  ir->lt_ostores[2 * ir->lt_nb_ostores] = from;
  ir->lt_ostores[2 * ir->lt_nb_ostores + 1] = to;
  ir->lt_nb_ostores++;
}

/* move(p) handed to a regular-C (unchecked) parameter or stored in a
   regular-C place: temp `vreg` holds it, and p moves where the temp is
   passed or stored */
void tcc_ir_lt_note_moved_temp(TCCIRState *ir, int32_t vreg)
{
  if (!ir->lt_on || vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP)
    return;
  ir->lt_mtemps = lt_grow(ir->lt_mtemps, ir->lt_nb_mtemps, sizeof(int32_t));
  ir->lt_mtemps[ir->lt_nb_mtemps++] = vreg;
}

static int lt_moved_temp(const TCCIRState *ir, IROperand op)
{
  int32_t v = irop_is_none(op) || op.is_lval ? -1 : irop_get_vreg(op);
  for (int k = 0; v >= 0 && k < ir->lt_nb_mtemps; k++)
    if (ir->lt_mtemps[k] == v)
      return 1;
  return 0;
}

/* `->`/`.` reached a `T * maybe` member: `vreg`, a temp, holds its address */
void tcc_ir_lt_note_member(TCCIRState *ir, int32_t vreg, int offset, int tok, int owned, int esize)
{
  IRLtMember *m;
  ir->lt_members = lt_grow(ir->lt_members, ir->lt_nb_members, sizeof(IRLtMember));
  m = &ir->lt_members[ir->lt_nb_members++];
  m->vreg = vreg;
  m->offset = offset;
  m->tok = tok;
  m->owned = owned;
  m->foff = offset;
  m->esize = esize;
}

/* `g.next`: a maybe member of a global struct, used in place */
void tcc_ir_lt_note_gmember(TCCIRState *ir, Sym *sym, int offset, int tok, int owned, int foff, int esize)
{
  for (int k = 0; k < ir->lt_nb_gmembers; k++)
    if (ir->lt_gm_syms[k] == sym && ir->lt_gmembers[k].offset == offset)
      return;
  ir->lt_gmembers = lt_grow(ir->lt_gmembers, ir->lt_nb_gmembers, sizeof(IRLtMember));
  ir->lt_gm_syms = lt_grow(ir->lt_gm_syms, ir->lt_nb_gmembers, sizeof(Sym *));
  ir->lt_gm_syms[ir->lt_nb_gmembers] = sym;
  IRLtMember *m = &ir->lt_gmembers[ir->lt_nb_gmembers++];
  m->vreg = -1;
  m->offset = offset;
  m->tok = tok;
  m->owned = owned;
  m->foff = foff;
  m->esize = esize;
}

/* an inlined body (or a cleanup) starts or ends: the globals it touches are
   not followed there, as a call does not change them */
void tcc_ir_lt_inline_mark(TCCIRState *ir, int begin)
{
  if (ir->ab_on)
    tcc_ir_ab_inline_mark(ir, begin);
  if (!ir->lt_on)
    return;
  if (begin)
  {
    ir->lt_inl = lt_grow(ir->lt_inl, ir->lt_nb_inl, 2 * sizeof(int));
    ir->lt_inl[2 * ir->lt_nb_inl] = ir->next_instruction_index;
    ir->lt_inl[2 * ir->lt_nb_inl + 1] = INT32_MAX;
    ir->lt_nb_inl++;
  }
  else if (ir->lt_nb_inl)
    ir->lt_inl[2 * ir->lt_nb_inl - 1] = ir->next_instruction_index;
}

static int lt_owned_store(const TCCIRState *ir, int i)
{
  for (int k = 0; k < ir->lt_nb_ostores; k++)
    if (i >= ir->lt_ostores[2 * k] && i < ir->lt_ostores[2 * k + 1])
      return 1;
  return 0;
}

void tcc_ir_lt_note_implicit(TCCIRState *ir, int call_id)
{
  if (call_id < ir->lt_nb_calls)
    ir->lt_calls[call_id].implicit++;
}

/* the lifetime records; tcc_ir_lt_free (from tcc_ir_free) drops both kinds */
/* a LOAD gave temp VREG a pointer */
void tcc_ir_lt_note_pointer(TCCIRState *ir, int vreg)
{
  int pos, w;
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP)
    return;
  pos = TCCIR_DECODE_VREG_POSITION(vreg);
  w = pos / 32;
  if (w >= ir->lt_ptr_temps_words)
  {
    int n = w + 64;
    ir->lt_ptr_temps = tcc_realloc(ir->lt_ptr_temps, n * sizeof(uint32_t));
    memset(ir->lt_ptr_temps + ir->lt_ptr_temps_words, 0, (n - ir->lt_ptr_temps_words) * sizeof(uint32_t));
    ir->lt_ptr_temps_words = n;
  }
  ir->lt_ptr_temps[w] |= 1u << (pos % 32);
}

static int lt_ptr_temp(TCCIRState *ir, int32_t vreg)
{
  int pos;
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  pos = TCCIR_DECODE_VREG_POSITION(vreg);
  return pos / 32 < ir->lt_ptr_temps_words && (ir->lt_ptr_temps[pos / 32] >> (pos % 32)) & 1;
}

/* the owned items of a local of TYPE at frame offset OFF, named NAME */
typedef struct LtElem
{
  int esize, idx, nelem, pre, suf; /* see IRLtSlot */
} LtElem;

static void lt_slot_items(TCCIRState *ir, int base, int base_size, int off, CType *t, const char *name, int depth,
                          const LtElem *el)
{
  int q = type_yasc(t), al;
  if (depth > 4 || ir->lt_nb_slots >= 64)
    return;
  if ((((t->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR) || type_is_handle(t)) && (q & YASC_OWNED) && !(q & YASC_UNCHECKED))
  {
    IRLtSlot *s;
    ir->lt_slots = lt_grow(ir->lt_slots, ir->lt_nb_slots, sizeof(IRLtSlot));
    s = &ir->lt_slots[ir->lt_nb_slots++];
    memset(s, 0, sizeof *s);
    s->off = off;
    s->base = base;
    s->base_size = base_size;
    s->dealloc = type_dealloc(t);
    s->kind = LTV_OWNED | (q & YASC_MAYBE ? LTV_MAYBE : 0) | (type_is_handle(t) ? LTV_HANDLE : 0);
    s->silent = tcc_state->in_inline_expansion != 0;
    snprintf(s->name, sizeof s->name, "%s", name);
    if (el && el->suf < (int)sizeof s->name - 1 && el->suf <= (int)strlen(s->name))
    {
      s->esize = el->esize, s->idx = el->idx, s->nelem = el->nelem;
      s->pre = el->pre, s->suf = el->suf;
    }
    return;
  }
  if ((t->t & VT_BTYPE) == VT_STRUCT && !(t->t & VT_ARRAY) && t->ref)
  {
    for (Sym *f = sym_first(t->ref); f; f = f->next)
    {
      char sub[40];
      if (f->type.t & VT_BITFIELD)
        continue;
      snprintf(sub, sizeof sub, "%s.%s", name, get_tok_str(f->v & ~SYM_FIELD, NULL));
      lt_slot_items(ir, base, base_size, off + f->c, &f->type, sub, depth + 1, el);
    }
  }
  else if ((t->t & VT_ARRAY) && t->ref && t->ref->c > 0 && t->ref->c <= 16)
  {
    int esize = type_size(&t->ref->type, &al);
    for (int i = 0; esize > 0 && i < t->ref->c; i++)
    {
      char sub[40];
      LtElem e;
      snprintf(sub, sizeof sub, "%s[%d]", name, i);
      e.esize = esize, e.idx = i, e.nelem = t->ref->c;
      e.pre = (int)strlen(name), e.suf = (int)strlen(sub);
      lt_slot_items(ir, base, base_size, off + i * esize, &t->ref->type, sub, depth + 1, &e);
    }
  }
}

/* the words of a value of type T at OFF that hold a pointer */
static void lt_sobj_pmask(CType *t, int off, uint64_t *mask, int depth)
{
  int al;
  if (depth > 4 || off < 0 || off >= 256)
    return;
  if ((t->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR)
    *mask |= 1ull << (off >> 2);
  else if ((t->t & VT_BTYPE) == VT_STRUCT && !(t->t & VT_ARRAY) && t->ref)
  {
    for (Sym *f = sym_first(t->ref); f; f = f->next)
      if (!(f->type.t & VT_BITFIELD))
        lt_sobj_pmask(&f->type, off + f->c, mask, depth + 1);
  }
  else if ((t->t & VT_ARRAY) && t->ref && t->ref->c > 0)
  {
    int esize = type_size(&t->ref->type, &al);
    for (int i = 0; esize > 0 && i < t->ref->c && off + i * esize < 256; i++)
      lt_sobj_pmask(&t->ref->type, off + i * esize, mask, depth + 1);
  }
}

void tcc_ir_lt_note_slot(TCCIRState *ir, int addr, CType *type, int tok, int obj_dealloc)
{
  int al, size = type_size(type, &al);
  if (size <= 0)
    return;
  /* a struct whose values borrow (the flow check follows it by frame offset) */
  if (yasc_borrowing_struct(type) && size <= 256 && ir->lt_nb_sobjs < 32)
  {
    IRLtSobj *o;
    uint64_t mask = 0;
    lt_sobj_pmask(type, 0, &mask, 0);
    if (mask)
    {
      ir->lt_sobjs = lt_grow(ir->lt_sobjs, ir->lt_nb_sobjs, sizeof(IRLtSobj));
      o = &ir->lt_sobjs[ir->lt_nb_sobjs++];
      o->off = addr, o->size = size, o->tok = tok, o->pmask = mask;
      o->silent = tcc_state->in_inline_expansion != 0;
    }
  }
  if (obj_dealloc && (type->t & (VT_BTYPE | VT_ARRAY)) == VT_STRUCT && ir->lt_nb_slots < 64)
  {
    /* `regex_t owned(regfree) re`: the whole object is one owned item */
    IRLtSlot *s;
    ir->lt_slots = lt_grow(ir->lt_slots, ir->lt_nb_slots, sizeof(IRLtSlot));
    s = &ir->lt_slots[ir->lt_nb_slots++];
    memset(s, 0, sizeof *s);
    s->off = s->base = addr;
    s->base_size = size;
    s->dealloc = obj_dealloc;
    s->kind = LTV_OWNED | LTV_OBJECT;
    s->silent = tcc_state->in_inline_expansion != 0;
    snprintf(s->name, sizeof s->name, "%s", get_tok_str(tok, NULL));
    return;
  }
  lt_slot_items(ir, addr, size, addr, type, get_tok_str(tok, NULL), 0, NULL);
}

void tcc_ir_lt_note_field_store(TCCIRState *ir, int from, int to, int entry)
{
  if (to <= from)
    return;
  ir->lt_fstores = lt_grow(ir->lt_fstores, ir->lt_nb_fstores, 3 * sizeof(int));
  ir->lt_fstores[3 * ir->lt_nb_fstores] = from;
  ir->lt_fstores[3 * ir->lt_nb_fstores + 1] = to;
  ir->lt_fstores[3 * ir->lt_nb_fstores + 2] = entry;
  ir->lt_nb_fstores++;
}

void tcc_ir_lt_note_field_load(TCCIRState *ir, int vreg, int entry)
{
  ir->lt_floads = lt_grow(ir->lt_floads, ir->lt_nb_floads, 2 * sizeof(int));
  ir->lt_floads[2 * ir->lt_nb_floads] = vreg;
  ir->lt_floads[2 * ir->lt_nb_floads + 1] = entry;
  ir->lt_nb_floads++;
}

static void lt_free_records(TCCIRState *ir)
{
  tcc_free(ir->lt_sres);
  ir->lt_sres = NULL;
  ir->lt_nb_sres = 0;
  tcc_free(ir->lt_pres);
  ir->lt_pres = NULL;
  ir->lt_nb_pres = 0;
  tcc_free(ir->lt_sobjs);
  ir->lt_sobjs = NULL;
  ir->lt_nb_sobjs = 0;
  tcc_free(ir->lt_slots);
  ir->lt_slots = NULL;
  ir->lt_nb_slots = 0;
  tcc_free(ir->lt_inames);
  ir->lt_inames = NULL;
  ir->lt_nb_inames = 0;
  tcc_free(ir->lt_floads);
  ir->lt_floads = NULL;
  ir->lt_nb_floads = 0;
  tcc_free(ir->lt_fstores);
  ir->lt_fstores = NULL;
  ir->lt_nb_fstores = 0;
  tcc_free(ir->lt_ptr_temps);
  ir->lt_ptr_temps = NULL;
  ir->lt_ptr_temps_words = 0;
  tcc_free(ir->lt_vars);
  tcc_free(ir->lt_events);
  tcc_free(ir->lt_calls);
  tcc_free(ir->lt_ostores);
  tcc_free(ir->lt_mtemps);
  ir->lt_mtemps = NULL;
  ir->lt_nb_mtemps = 0;
  tcc_free(ir->lt_rlocals);
  ir->lt_rlocals = NULL;
  ir->lt_nb_rlocals = 0;
  tcc_free(ir->lt_selfp);
  ir->lt_selfp = NULL;
  ir->lt_nb_selfp = 0;
  tcc_free(ir->lt_dmembers);
  ir->lt_dmembers = NULL;
  ir->lt_nb_dmembers = 0;
  tcc_free(ir->lt_iself);
  ir->lt_iself = NULL;
  ir->lt_nb_iself = 0;
  tcc_free(ir->lt_fnames);
  ir->lt_fnames = NULL;
  ir->lt_nb_fnames = 0;
  tcc_free(ir->lt_members);
  ir->lt_members = NULL;
  ir->lt_nb_members = 0;
  tcc_free(ir->lt_gmembers);
  tcc_free(ir->lt_gm_syms);
  ir->lt_gmembers = NULL;
  ir->lt_gm_syms = NULL;
  ir->lt_nb_gmembers = 0;
  tcc_free(ir->lt_fobjs);
  ir->lt_fobjs = NULL;
  ir->lt_nb_fobjs = 0;
  tcc_free(ir->lt_inl);
  ir->lt_inl = NULL;
  ir->lt_nb_inl = 0;
  ir->lt_ostores = NULL;
  ir->lt_nb_ostores = 0;
  ir->lt_vars = NULL;
  ir->lt_events = NULL;
  ir->lt_calls = NULL;
  ir->lt_nb_vars = ir->lt_nb_events = ir->lt_nb_calls = 0;
}

static void un_free_records(TCCIRState *ir)
{
  tcc_free(ir->dp_names);
  ir->dp_names = NULL;
  ir->dp_nb_names = 0;
  tcc_free(ir->un_vars);
  tcc_free(ir->un_events);
  tcc_free(ir->un_outparam);
  ir->un_outparam = NULL;
  ir->un_outparam_words = 0;
  ir->un_vars = NULL;
  ir->un_events = NULL;
  ir->un_nb_vars = ir->un_nb_events = 0;
}

void tcc_ir_lt_free(TCCIRState *ir)
{
  lt_free_records(ir);
  un_free_records(ir);
  tcc_ir_ab_free(ir);
  /* the notes the flow checks free as they go: a body with an error, or an
     error that leaves the function, never reaches them */
  tcc_free(ir->tag_notes), ir->tag_notes = NULL, ir->nb_tag_notes = 0;
  tcc_ir_w1c_free(ir);
}

/* ------------------------------------------------------------------------ */

typedef struct LtCtx
{
  TCCIRState *ir;
  IRCFG *cfg;
  int nv, n;
  int32_t *map_var, *map_param; /* vreg position -> variable, -1 none */
  int nmap_var, nmap_param;
  int32_t *alias;     /* temp position -> variable whose value it copied */
  int32_t *palias;    /* temp position -> variable it points into (a copy, p + n) */
  uint8_t *tnull;     /* temp position -> nullness of its value */
  uint32_t *alias_gen; /* valid when == gen */
  int nalias;
  uint32_t gen;
  uint8_t *cur;       /* [nv]: the state walking a block */
  uint8_t *scratch;   /* [(max successors + 1) * nv]: edges in the reporting walk */
  int *move_line;     /* [nv] */
  uint8_t *move_drop; /* [nv] */
  uint8_t *arg_bound; /* [nv]: an inlined parameter's binding was looked at (reporting walk) */
  int32_t *borrow;    /* [nv]: the owned variable a pointer that owns nothing borrows from;
                         -1 none, -2 several (a row below, or not followed) */
  /* A pointer that owns nothing borrowing from several owners (`v = c ? p :
     q`, p on one path and q on another), LT_MAX_BOWN at most, and a global
     temp whose definitions point into different owners (the `?:` join): a
     row of state entries, one per owner (bbase + brow_at[row] + j):
     LT_OWN "may borrow from it now", LT_MOVED "... and it is gone", LT_UNINIT
     "may not".  The owners are found in the collect walk. */
  int32_t *vbset;     /* [nvars * LT_MAX_BOWN] */
  uint8_t *vbcnt;     /* [nvars]: owners found, LT_MAX_BOWN + 1 too many; LT_BOWN_VIA through a temp */
  uint8_t *vbsaid;    /* [nvars]: "more than LT_MAX_BOWN owners" said */
  int32_t *gbset;     /* [nglob * LT_MAX_BOWN] */
  uint8_t *gbcnt;     /* [nglob] */
  int32_t *vbrow, *gbrow; /* -> row, -1 none */
  int32_t *brow_own;  /* [nbrow * LT_MAX_BOWN] */
  uint8_t *brow_n;    /* [nbrow]: its owners */
  int32_t *brow_at;   /* [nbrow]: its first state entry, from bbase (rows are brow_n long) */
  int nbrow, bbase;
  /* local structs whose values borrow (ir->lt_sobjs), followed by frame
     offset: a row each, as a pointer borrowing from several owners */
  int nsobj, nso_calls;
  uint8_t *so_untracked; /* [nsobj]: its address goes somewhere not followed */
  int32_t *so_tfa;    /* temp position -> the frame offset it holds (a temp defined once), LT_NOADDR */
  int32_t *so_tld;    /* temp position -> the object a word of which it was loaded from, -1 */
  int32_t *so_sret;   /* call_id -> the object its result is written into, -1 */
  int32_t *sbset;     /* [nsobj * LT_MAX_BOWN] */
  uint8_t *sbcnt;     /* [nsobj] */
  int32_t *sbrow;     /* [nsobj] -> row, -1 */
  int32_t *so_tcw;    /* temp position -> 2 * call_id + word: a word of a borrowing struct returned in registers, -1 */
  int32_t *so_cown;   /* call_id -> the owned variables passed where its struct result borrows from (4) */
  int8_t *so_ncown;   /* call_id -> how many, -1 too many */
  int32_t *bind_owner; /* [nvars] an inlined call's parameter: the owner its argument points into, -1; -3 unbound */
  int *sres_first;    /* [n + 2]: inlined results before instruction i: sres_first[i]..sres_first[i+1] */
  int collect;        /* the walk that fills borrow[] */
  int *ev_first;      /* [n + 2]: events before instruction i are ev_first[i]..ev_first[i+1] */
  IRLtEvent *ev;      /* sorted by idx */
  int report;
  int ret_owned;
  int ret_plain_ptr; /* returns a pointer that is not maybe */
  int end_line;
  /* maybe members (paths): path k is state index nvars + k, `base->member`,
     base a variable or another path */
  int nvars;          /* the variables; nv = nvars + room for paths */
  int npaths, pcap;
  int32_t *pbase;     /* [pcap] */
  int32_t *poff;      /* [pcap]: the member's offset */
  char **pname;       /* [pcap]: "q->next" */
  uint8_t *pmark;     /* [pcap]: lt_forget's scratch */
  int32_t *trec;      /* temp position -> its lt_members record, -1 none */
  int32_t *tbase;     /* temp position -> the variable or path whose pointer value it holds plus ... */
  int32_t *toff;      /* ... this constant offset; tbase -1 none */
  int32_t *taddr;     /* temp position -> the path whose address it holds, -1 none */
  /* an element at a variable index (`a[i].next`): a path per (base, member
     offset, index variable), forgotten when the index is written; a temp's
     value base + i * scale + offset (xidx -1 none), xat where i was read */
  int32_t *pidx;      /* [pcap]: the index variable's vreg, -1 a constant offset */
  int32_t *xbase, *xoff, *xidx, *xsc, *xat;
  int nixpaths;
  int32_t *pend;      /* (call_id, state index, with itself) per argument whose paths a call forgets */
  int32_t *rarg;      /* (call_id, argument, source, line) of a call with restrict parameters (reporting walk) */
  int nrarg, rargcap;
  int npend, pendcap;
  int32_t *call_at;   /* call_id -> the call's instruction, -1 none */
  int ncall_at;
  /* takes(p) with a maybe result: p may have moved until the result is
     tested -- valid: it did; null: it did not */
  int32_t *take_call; /* [nv]: the call that may have taken v, -1 */
  int32_t *taken_by;  /* [nv]: the variable holding that call's result, -1 */
  int32_t *tcall;     /* temp position -> the call whose result it holds */
  uint32_t *tcall_gen;
  /* `getline(&line, ...)`: a temp holding a variable's address, passed only to
     parameters that point at an owned pointer -- the variable stays followed */
  int32_t *tout;      /* temp position -> the variable, -1 */
  int32_t *out_call;  /* [nv]: the call that wrote it through its address, its result a status; -1 */
  /* Temps used in another block than the one defining them (the `?:` join,
     a member loaded before a branch): what a temp's definition says about it
     (alias, palias, tbase/toff, taddr, tcall) is kept per global temp, joined
     over its definitions in a walk (they agree, or the field is unknown), and
     its nullness is a state entry (gslot + index) so that it follows paths. */
  int32_t *tglob;     /* temp position -> global temp index, -1 block-local */
  int nglob, gslot;
  int32_t *g_alias, *g_palias, *g_tbase, *g_toff, *g_taddr, *g_tcall, *g_line;
  uint8_t *g_known;   /* [nglob]: a definition was seen in this walk */
  int32_t *tline;     /* temp position -> the line of its definition (global temps) */
  int32_t *tderiv;    /* temp position -> the temp it is an offset from (`find(k)->next`), -1 */
  int unnamed_lines[8]; /* the last lines a value with no name was reported at: once per line */
  int nunnamed;
  /* a call whose result borrows from an argument (`borrows(s)`): the owned
     variable passed there; -1 none, -2 several */
  int32_t *call_bsrc;
  int ncall_bsrc;
  /* `out T * borrows(s) * view p` (call->tborrow): per call, the variable
     whose address went there (-1 none, -2 several) and the owned variable
     passed for s (call_bsrc's rules); [ncall_bsrc] each */
  int32_t *call_tvar, *call_tsrc;
  /* a local defined once, by a call's result (`int r = posix_memalign(&p,
     ...)`): VAR position -> the call, -1; vdefs: its definitions */
  int32_t *vcall;
  uint8_t *vdefs;
  int nvcall;
  /* ... assigned more than once: the call whose result it holds is followed
     per path, a state entry each (sbase + row): bit j "may hold call j's
     result" (stcalls[row * LT_STAT_CALLS + j]), LT_STAT_OTHER anything else */
  int32_t *vstat;     /* [nvcall] VAR position -> row, -1 */
  int32_t *stcalls;
  int nstat, sbase;   /* sbase -1 before the entries exist */
  /* `int ok = p != NULL;`: a local holding a null test of a pointer variable
     (cl_x, cl_pol: nonzero means not null) tells about it when tested, while
     neither changed; a state entry each (clbase + row), LT_N_NULL "broken" */
  int32_t *cl_var;    /* [nvcall] VAR position -> row, -1 */
  int32_t *cl_x;
  uint8_t *cl_pol;
  int nclink, clbase;
  /* `n = q->next`: a variable whose every copy of a maybe member is of the
     same path is linked to it; a test of one tells about the other while
     neither changed.  link: [nvars] the path, -1 none, -2 several (found in
     the collect walk); lslot: [nvars] its state entry (lbase + index), whose
     LT_N_NULL bit says "may no longer hold the path's value" */
  int32_t *link, *lslot;
  int nlink, lbase;
  /* bases of maybe members that are not pointer variables: base index
     LT_OBJ(j) for object j -- a local struct (s.next), a global struct
     (g.next), a global pointer's value (gp->next).  They hold no state;
     their paths do. */
  int nobj, objcap;
  uint8_t *okind;     /* LT_OK_* */
  Sym **osym;         /* the global */
  int32_t *ooff;      /* frame: its offset; global pointer: the addend */
  int32_t *osize;     /* frame: its size */
  int32_t *otok;      /* its name */
  uint8_t *osilent;   /* declared in an inlined body */
  int32_t *oesize;    /* an array of structs: an element's size; 0 a struct */
  uint8_t *inl;       /* [n]: the instruction is in an inlined body (globals not followed); NULL none */
  int cur_inl;
  /* Path conditions: a predicate is `local == K` (K = 0 for a truth test) on
     a local or parameter whose address is not taken, tested more than once,
     or tested and assigned a constant.  Each block's state is a set of up
     to c->nctx contexts, each knowing the value of some predicates (bit i of
     known / val) with its own state vector; a test whose value a context
     knows sends it down one edge only. */
  int npred;
  int32_t pr_vreg[32];
  int64_t pr_k[32];
  uint32_t pk, pv;     /* walking a block: the predicates known, their values */
  uint32_t *eknown, *eval; /* [max successors + 1]: what each edge out of the block knows */
  uint8_t *efeas;      /* [max successors + 1]: the edge can be taken */
  const uint8_t *exit_join; /* reporting a joined block: the state leaving the function, */
  int exit_mode;       /* 1 use exit_join, 2 no context leaves there, 3 leave nothing; 0 the block's own */
} LtCtx;

#define LT_MAX_BOWN 8
#define LT_BOWN_VIA 0x80

/* owner O found for a borrower whose set is SET / *CNT */
static void lt_bset_add(int32_t *set, uint8_t *cnt, int32_t o, int via)
{
  int n = *cnt & ~LT_BOWN_VIA;
  if (via)
    *cnt |= LT_BOWN_VIA;
  if (n > LT_MAX_BOWN)
    return;
  for (int j = 0; j < n; j++)
    if (set[j] == o)
      return;
  if (n < LT_MAX_BOWN)
    set[n] = o;
  *cnt = (*cnt & LT_BOWN_VIA) | (n + 1);
}

/* what a drop does to v: an int handle (a variable, or a member path:
   `c->fd`) is released, memory freed */
static const char *lt_freed(LtCtx *c, int v)
{
  return v < c->nvars + c->npaths && (c->ir->lt_vars[v].kind & LTV_HANDLE) ? "released" : "freed";
}

static const char *lt_name(LtCtx *c, int v)
{
  if (v >= c->nvars)
    return c->pname[v - c->nvars];
  return get_tok_str(c->ir->lt_vars[v].tok, NULL);
}

static int lt_silent(LtCtx *c, int v)
{
  return !c->report || c->ir->lt_vars[v].silent;
}

static int lt_owned(LtCtx *c, int v)
{
  return c->ir->lt_vars[v].kind & LTV_OWNED;
}

static int lt_maybe(LtCtx *c, int v)
{
  return c->ir->lt_vars[v].kind & LTV_MAYBE;
}

static int lt_out(LtCtx *c, int v)
{
  return c->ir->lt_vars[v].kind & LTV_OUT;
}

/* Is what lt_instr recorded for temp POS valid here?  Set in this block, or
   a global temp defined in another one: then its joined facts are loaded. */
static int lt_tpos_valid(LtCtx *c, int pos)
{
  int g;
  if (pos < 0 || pos >= c->nalias)
    return 0;
  if (c->alias_gen[pos] == c->gen)
    return 1;
  if (!c->tglob || (g = c->tglob[pos]) < 0 || !c->g_known[g])
    return 0;
  c->alias[pos] = c->g_alias[g];
  c->palias[pos] = c->g_palias[g];
  c->tbase[pos] = c->g_tbase[g];
  c->toff[pos] = c->g_toff[g];
  c->taddr[pos] = c->g_taddr[g];
  c->tnull[pos] = LT_N_SET; /* read from the state entry */
  c->tcall[pos] = c->g_tcall[g];
  c->tcall_gen[pos] = c->g_tcall[g] >= 0 ? c->gen : 0;
  c->tline[pos] = c->g_line[g];
  c->tderiv[pos] = -1;
  if (c->xidx)
    c->xidx[pos] = -1;
  c->alias_gen[pos] = c->gen;
  return 1;
}

/* where *op is reported: a global temp's pointer where it was computed (a
   switch's value is loaded at the dispatch, after the body) */
static int lt_deref_line(LtCtx *c, IROperand op, int line)
{
  int32_t vreg = irop_get_vreg(op);
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (!c->tglob || vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP || pos >= c->nalias ||
      c->tglob[pos] < 0 || !lt_tpos_valid(c, pos))
    return line;
  return c->tline[pos];
}

/* the call whose result temp POS holds (through copies), -1 */
static int lt_tcall(LtCtx *c, int pos)
{
  if (!lt_tpos_valid(c, pos) || c->tcall_gen[pos] != c->gen)
    return -1;
  return c->tcall[pos];
}

/* a temp's value: may it be null (a global temp's on this path) */
static int lt_tnull(LtCtx *c, int pos)
{
  int g = c->tglob ? c->tglob[pos] : -1;
  return g >= 0 ? c->cur[c->gslot + g] : c->tnull[pos];
}

static int lt_map(LtCtx *c, int32_t vreg)
{
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
  case TCCIR_VREG_TYPE_VAR:
    return pos < c->nmap_var ? c->map_var[pos] : -1;
  case TCCIR_VREG_TYPE_PARAM:
    return pos < c->nmap_param ? c->map_param[pos] : -1;
  case TCCIR_VREG_TYPE_TEMP:
    return lt_tpos_valid(c, pos) ? c->alias[pos] : -1;
  }
  return -1;
}

/* &V: the operand is the variable's address, not its value */
static int lt_is_addr(IROperand op)
{
  return irop_get_tag(op) == IROP_TAG_STACKOFF && irop_is_local(op) && !op.is_lval && !irop_is_llocal(op);
}

/* LEA dest, x: x's address -- x is a frame slot or a variable's home */
static int lt_lea_of_local(const IRQuadCompact *q, IROperand src1)
{
  return q->op == TCCIR_OP_LEA && !irop_is_none(src1) && irop_get_tag(src1) == IROP_TAG_STACKOFF &&
         irop_is_local(src1) && !irop_is_llocal(src1);
}

/* The call records, kept for the checks after tcc_ir_lifetime_check (which
   frees them): the return-local and dangling checks follow a frame address
   through a call whose result borrows from an argument. */
typedef struct LtCalls
{
  IRLtCall *v;
  int n;
} LtCalls;

/* a call instruction's (or argument's) encoded src2: the call's id, -1 none */
static int lt_call_id(const TCCIRState *ir, const LtCalls *calls, IROperand enc_op)
{
  int id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, enc_op));
  return id >= 0 && id < calls->n && calls->v[id].noted ? id : -1;
}

/* FUNCPARAMVAL's src2: the call's id when its view result points into this
   argument (borrows(...), or the elision), else -1 */
static int lt_borrowed_arg(const TCCIRState *ir, const LtCalls *calls, IROperand enc_op)
{
  int id = lt_call_id(ir, calls, enc_op), k = TCCIR_DECODE_PARAM_IDX(irop_get_imm64_ex(ir, enc_op));
  const IRLtCall *call = id >= 0 ? &calls->v[id] : NULL;
  if (!call || !call->borrows)
    return -1;
  k -= call->implicit;
  return k >= 0 && k < YASC_BORROWS_MAX_PARAMS && ((call->borrows >> k) & 1) ? id : -1;
}

/* ---- a variable index: `a[i]`, the address base + i * scale -------------
 *
 * The raw IR computes an element's address again at each use: `T1 <- i SHL
 * #2` (or MUL #12), `T2 <- &a ADD T1`.  The index is followed by the int
 * local or parameter it is (address not taken), looked up through the temps'
 * definitions in the same block, a few instructions back; a write to it in
 * between makes the address another element's. */

#define IX_REACH 16

/* the instruction in [from, at) that last defines vreg v, -1 none (looked at
   up to IX_REACH instructions back) */
static int ix_def(TCCIRState *ir, int from, int at, int32_t v)
{
  for (int j = at - 1; j >= from && j >= at - IX_REACH; j--)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!irop_is_none(d) && irop_dest_defines_vreg(d) && irop_get_vreg(d) == v)
        return j;
    }
  }
  return -1;
}

/* is vreg v written in [from, at)?  Too far apart counts as written. */
static int ix_written(TCCIRState *ir, int from, int at, int32_t v)
{
  if (at - from > 4 * IX_REACH)
    return 1;
  for (int j = from; j < at; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!irop_is_none(d) && irop_dest_defines_vreg(d) && irop_get_vreg(d) == v)
        return 1;
    }
  }
  return 0;
}

/* The int local or parameter whose value op is at instruction at (its block
   starts at from): itself, or a temp copy or widening of it made with no
   write to it since; -1 none, or its address is taken. */
static int32_t ix_var(TCCIRState *ir, int from, int at, IROperand op)
{
  int32_t v;
  int j = at;
  if (irop_is_none(op) || !irop_is_vreg_value(op) || lt_is_addr(op) || (v = irop_get_vreg(op)) < 0)
    return -1;
  for (int depth = 0; TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP; depth++)
  {
    IRQuadCompact *q;
    IROperand s;
    if (depth == 3 || (j = ix_def(ir, from, j, v)) < 0)
      return -1;
    q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_ZEXT)
      return -1;
    s = tcc_ir_op_get_src1(ir, q);
    if (irop_is_none(s) || !irop_is_vreg_value(s) || lt_is_addr(s) || (v = irop_get_vreg(s)) < 0)
      return -1;
  }
  if (TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_VAR && TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_PARAM)
    return -1;
  if (lt_vreg_addrtaken(ir, v) || (j < at && ix_written(ir, j + 1, at, v)))
    return -1;
  return v;
}

/* The int local or parameter i such that op's value at instruction at is
   i + *delta: as ix_var, or a temp `i + c` / `i - c` / `c + i` (c a
   constant) made with no write to i since; -1 none. */
static int32_t ix_var_delta(TCCIRState *ir, int from, int at, IROperand op, int *delta)
{
  int32_t t, v;
  int j;
  *delta = 0;
  if (!irop_is_none(op) && irop_is_vreg_value(op) && irop_get_tag(op) == IROP_TAG_VREG && (t = irop_get_vreg(op)) >= 0 &&
      TCCIR_DECODE_VREG_TYPE(t) == TCCIR_VREG_TYPE_TEMP && (j = ix_def(ir, from, at, t)) >= 0)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    {
      IROperand a = tcc_ir_op_get_src1(ir, q), b = tcc_ir_op_get_src2(ir, q);
      int64_t d;
      if (q->op == TCCIR_OP_ADD && irop_is_immediate(a) && !a.is_lval)
      {
        IROperand x = a;
        a = b, b = x;
      }
      if (!irop_is_immediate(b) || b.is_lval)
        return -1;
      d = irop_get_imm64_ex(ir, b);
      if (q->op == TCCIR_OP_SUB)
        d = -d;
      if (d < -65536 || d > 65536 || (v = ix_var(ir, from, j, a)) < 0 || ix_written(ir, j + 1, at, v))
        return -1;
      *delta = (int)d;
      return v;
    }
  }
  return ix_var(ir, from, at, op);
}

/* op (a temp) holds (i + *delta) * scale at instruction at: i, *scale and
   *delta; -1 none */
static int32_t ix_scaled_d(TCCIRState *ir, int from, int at, IROperand op, int *scale, int *delta)
{
  int32_t t, v;
  int j;
  IRQuadCompact *q;
  IROperand k;
  int64_t kv;
  *delta = 0;
  if (irop_is_none(op) || !irop_is_vreg_value(op) || irop_get_tag(op) != IROP_TAG_VREG || (t = irop_get_vreg(op)) < 0 ||
      TCCIR_DECODE_VREG_TYPE(t) != TCCIR_VREG_TYPE_TEMP || (j = ix_def(ir, from, at, t)) < 0)
    return -1;
  q = &ir->compact_instructions[j];
  if (q->op != TCCIR_OP_SHL && q->op != TCCIR_OP_MUL)
    return -1;
  k = tcc_ir_op_get_src2(ir, q);
  if (!irop_is_immediate(k) || k.is_lval)
    return -1;
  kv = irop_get_imm64_ex(ir, k);
  if (q->op == TCCIR_OP_SHL ? kv < 1 || kv > 16 : kv < 2 || kv > 65536)
    return -1;
  *scale = q->op == TCCIR_OP_SHL ? 1 << kv : (int)kv;
  if ((v = ix_var_delta(ir, from, j, tcc_ir_op_get_src1(ir, q), delta)) < 0 || ix_written(ir, j + 1, at, v))
    return -1;
  return v;
}

/* op (a temp) holds i * scale at instruction at: i, and *scale; -1 none */
static int32_t ix_scaled(TCCIRState *ir, int from, int at, IROperand op, int *scale)
{
  int delta;
  int32_t v = ix_scaled_d(ir, from, at, op, scale, &delta);
  return delta ? -1 : v;
}

/* the name of an index variable, 0 none */
static int ix_name(TCCIRState *ir, int32_t v)
{
  for (int k = 0; k < ir->lt_nb_inames; k++)
    if (ir->lt_inames[2 * k] == v)
      return ir->lt_inames[2 * k + 1];
  return 0;
}

/* The tracked variable an operand reads -- its value, or memory through it
   (`*p`) -- or -1. */
static int lt_read_var(LtCtx *c, IROperand op)
{
  int32_t vreg;
  if (irop_is_none(op) || (vreg = irop_get_vreg(op)) < 0 || lt_is_addr(op))
    return -1;
  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP && !irop_is_vreg_value(op))
    return -1;
  return lt_map(c, vreg);
}

static int lt_temp_valid(LtCtx *c, int32_t vreg)
{
  return TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP && lt_tpos_valid(c, TCCIR_DECODE_VREG_POSITION(vreg));
}

/* the variable whose pointee an operand's pointer value points into */
static int lt_pointer_var(LtCtx *c, IROperand op)
{
  int32_t vreg = irop_get_vreg(op);
  if (vreg < 0)
    return -1;
  if (lt_temp_valid(c, vreg))
    return c->palias[TCCIR_DECODE_VREG_POSITION(vreg)];
  return TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP ? -1 : lt_map(c, vreg);
}

/* *op: memory read or written through a pointer held in a vreg */
static int lt_deref_var(LtCtx *c, IROperand op)
{
  if (irop_is_none(op) || irop_get_tag(op) != IROP_TAG_VREG || !op.is_lval)
    return -1;
  return lt_pointer_var(c, op);
}

/* ---- maybe members --------------------------------------------------------
 *
 * A `T * maybe` member read or written through a followed pointer is a path:
 * (base, offset), where the base is a pointer variable or, for `a->b->c`,
 * another path.  The frontend records the temp holding the member's address
 * (tcc_ir_lt_note_member); here each temp's value is known as "the pointer
 * value of base, plus a constant", and a recorded address of that shape is
 * the path.  A path is one more maybe variable in the same dataflow:
 *
 *   - a load of it gives the loaded value its state (it starts as "may be
 *     null"), and that value dereferenced checks the path's state;
 *   - a null test of the loaded value, or of the member in place, refines
 *     the path on each edge, as for a variable;
 *   - a store to the member through the same base sets it from the stored
 *     value; any other store that hits (base, offset) forgets it;
 *   - assigning the base again, or the end of its scope, forgets every path
 *     of that base (and the paths based on those);
 *   - a call forgets the paths of a pointer passed to a parameter that points
 *     to memory that is not const (directly, or a pointer derived from it;
 *     `&q->next` forgets q->next itself), and so does binding it to such a
 *     parameter of an inlined callee: `const T * view` keeps them.  Past the
 *     declared parameters (varargs, no prototype) everything is forgotten.
 *     Every call, and the start of every inlined body (so that -O does not
 *     matter), forgets the paths of the global objects below.
 *   - A store to a member forgets the same member (offset and name) through
 *     every other base: two pointers may reach one struct.  A store through
 *     a pointer whose base is not known changes nothing.
 *   - The base may also be an object (LT_OBJ): a local struct (`s.next`,
 *     its frame offset range from the frontend), a global struct (`g.next`,
 *     the symbol plus offset used in place) or a global pointer's value
 *     (`gp->next`, a load of the global).  A store into the object sets or
 *     forgets the paths it covers; storing to the global pointer forgets
 *     its paths.  Globals are not followed inside an inlined body, and any
 *     call or inlined body forgets them, so the result does not depend on -O.
 *   - An `owned maybe` member is followed for ownership too: passed to an
 *     owned parameter (`free(q->next)`) or moved into an owned variable, it
 *     is moved, and read or freed again is reported.  A store gives it a new
 *     value; it never leaks on its own (its struct owns it).
 *
 * "Forgotten" is "may be null": the member must be tested again.  A temp
 * used in another block than its definition (`c ? q->next->v : 0`, the join
 * of `?:`) keeps what its definitions agree on, and its nullness per path
 * (the global temps of LtCtx). */

/* path k changes value: the variables linked to it no longer hold it */
static void lt_unlink(LtCtx *c, int k)
{
  if (!c->nlink)
    return;
  for (int v = 0; v < c->nvars; v++)
    if (c->lslot[v] >= 0 && c->link[v] == k)
      c->cur[c->lslot[v]] |= LT_N_NULL;
}

static void lt_path_reset(LtCtx *c, int k)
{
  lt_unlink(c, k);
  c->cur[k] = (c->cur[k] & ~(LT_NULLBITS | LT_OWNBITS)) | LT_UNINIT | LT_N_NULL | LT_N_SET;
}

/* Base b (a variable or a path) changed: every path of it, and every path
   based on those, may be null again; `self` forgets the path b itself too. */
static void lt_forget(LtCtx *c, int b, int self)
{
  if (b == -1 || !c->npaths)
    return;
  if (self && b >= c->nvars)
    lt_path_reset(c, b);
  memset(c->pmark, 0, c->npaths);
  /* a path's base was found before it: one pass in order */
  for (int k = 0; k < c->npaths; k++)
  {
    int pb = c->pbase[k];
    if (pb == b || (pb >= c->nvars && c->pmark[pb - c->nvars]))
    {
      c->pmark[k] = 1;
      lt_path_reset(c, c->nvars + k);
    }
  }
}

/* path k was written: the same member through another base may be the same
   memory (two pointers to one struct), so those paths are forgotten -- the
   same member is the same offset and member name */
static void lt_forget_aliases(LtCtx *c, int k)
{
  int tok = c->ir->lt_vars[k].tok, off = c->poff[k - c->nvars], b = c->pbase[k - c->nvars];
  for (int p = 0; p < c->npaths; p++)
    if (c->nvars + p != k && c->pbase[p] != b && c->poff[p] == off && c->ir->lt_vars[c->nvars + p].tok == tok)
      lt_forget(c, c->nvars + p, 1);
}

/* ---- objects: bases that are not pointer variables ---- */
#define LT_STAT_CALLS 7
#define LT_STAT_OTHER 0x80
#define LT_MAX_STAT 32
#define LT_MAX_CLINK 32

/* Instruction i sets a local from a null test of vreg *x (`ok = p != 0`,
   `z = !p`): CMP x,#0 or TEST_ZERO x, then SETIF.  *pol: 1 when the local
   is nonzero for a pointer that is not null. */
static int lt_setif_test(TCCIRState *ir, int i, int32_t *x, int *pol)
{
  IRQuadCompact *q = &ir->compact_instructions[i], *t;
  int cond;
  if (q->op != TCCIR_OP_SETIF || i == 0)
    return 0;
  t = &ir->compact_instructions[i - 1];
  cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (cond != TOK_EQ && cond != TOK_NE)
    return 0;
  if (t->op == TCCIR_OP_CMP)
  {
    IROperand b = tcc_ir_op_get_src2(ir, t);
    if (!irop_is_immediate(b) || b.is_lval || irop_get_imm64_ex(ir, b) != 0)
      return 0;
  }
  else if (t->op != TCCIR_OP_TEST_ZERO)
    return 0;
  IROperand a = tcc_ir_op_get_src1(ir, t);
  if (irop_is_none(a) || !irop_is_vreg_value(a) || lt_is_addr(a) || (*x = irop_get_vreg(a)) < 0)
    return 0;
  *pol = cond == TOK_NE;
  return 1;
}

#define LT_OBJ(j) (-2 - (j))
#define LT_OK_FRAME 0  /* a local struct */
#define LT_OK_GLOBAL 1 /* a global struct */
#define LT_OK_GPTR 2   /* the value of a global pointer */
#define LT_MAX_OBJ 256

static int lt_obj_new(LtCtx *c, int kind, Sym *sym, int off, int size, int tok, int silent, int esize)
{
  if (c->nobj >= c->objcap)
  {
    if (c->objcap >= LT_MAX_OBJ)
      return -1;
    c->objcap = c->objcap ? 2 * c->objcap : 8;
    c->okind = tcc_realloc(c->okind, c->objcap);
    c->osilent = tcc_realloc(c->osilent, c->objcap);
    c->osym = tcc_realloc(c->osym, c->objcap * sizeof(Sym *));
    c->ooff = tcc_realloc(c->ooff, c->objcap * sizeof(int32_t));
    c->osize = tcc_realloc(c->osize, c->objcap * sizeof(int32_t));
    c->otok = tcc_realloc(c->otok, c->objcap * sizeof(int32_t));
    c->oesize = tcc_realloc(c->oesize, c->objcap * sizeof(int32_t));
  }
  int j = c->nobj++;
  c->okind[j] = kind, c->osym[j] = sym, c->ooff[j] = off, c->osize[j] = size, c->otok[j] = tok;
  c->osilent[j] = silent;
  c->oesize[j] = esize;
  return LT_OBJ(j);
}

/* a call, or an inlined body (the same at every -O): the globals' members
   may have changed */
static void lt_forget_globals(LtCtx *c)
{
  for (int j = 0; j < c->nobj; j++)
    if (c->okind[j] == LT_OK_GLOBAL || c->okind[j] == LT_OK_GPTR)
      lt_forget(c, LT_OBJ(j), 0);
}

/* the global SYM may change in a call whose IRLtCall.gkeep is GKEEP */
int tcc_ir_call_writes_global(int gkeep, Sym *sym)
{
  if (!sym)
    return 1;
  if (gkeep == LT_GKEEP_SYS)
    return tcc_sys_global(sym->v); /* libc writes its own: errno, stdout */
  if (gkeep == LT_GKEEP_LEAF)
    return !(sym->type.t & VT_STATIC); /* a leaf cannot reach this unit's statics */
  return 1;
}

/* the globals CALL (NULL: not recorded) cannot write (LT_GKEEP_*) */
static int lt_call_gkeep(const IRLtCall *call)
{
  return call && call->noted && !call->galias ? call->gkeep : 0;
}

/* FUNCPARAMVAL i hands op to argument k of CALL (k past the hidden ones): a
   libc or leaf callee may write a global through it unless it cannot write
   through it (const, no pointer), takes ownership of it, or op is a local's
   address or a constant -- followed through copies and offsets in the block */
static void lt_arg_galias(TCCIRState *ir, IRCFG *cfg, int i, IRLtCall *call, int k, IROperand op)
{
  if (!call || !call->noted || !call->gkeep || call->galias || k < 0)
    return;
  if (k < 32 && k < call->nparams && (((call->keep | call->owned) >> k) & 1))
    return;
  int from = cfg->blocks[cfg->instr_to_block[i]].start_idx;
  for (int d = 0, at = i; d < 6 && !irop_is_none(op); d++)
  {
    if (lt_is_addr(op) || irop_is_plain_imm(op))
      return;
    int32_t vr = irop_get_vreg(op);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || op.is_lval)
      break;
    int j = ix_def(ir, from, at, vr);
    if (j < 0)
      break;
    IRQuadCompact *q = &ir->compact_instructions[j];
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (lt_lea_of_local(q, s1))
      return;
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      break;
    IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    /* the pointer side of an addition */
    op = (q->op == TCCIR_OP_ADD && !irop_is_none(s2) && lt_is_addr(s2) && !lt_is_addr(s1)) ? s2 : s1;
    at = j;
  }
  call->galias = 1;
}

/* ... a call CALL (NULL: not recorded) */
static void lt_forget_globals_call(LtCtx *c, const IRLtCall *call)
{
  const int gkeep = lt_call_gkeep(call);
  for (int j = 0; j < c->nobj; j++)
    if ((c->okind[j] == LT_OK_GLOBAL || c->okind[j] == LT_OK_GPTR) && tcc_ir_call_writes_global(gkeep, c->osym[j]))
      lt_forget(c, LT_OBJ(j), 0);
}

/* a global (struct, or pointer at sym + off): not followed in an inlined body */
static int lt_gobj(LtCtx *c, int kind, Sym *sym, int off)
{
  if (c->cur_inl || !sym)
    return -1;
  for (int j = 0; j < c->nobj; j++)
    if (c->okind[j] == kind && c->osym[j] == sym && (kind != LT_OK_GPTR || c->ooff[j] == off))
      return LT_OBJ(j);
  int esize = 0;
  if (kind == LT_OK_GLOBAL)
    lt_struct_or_array(&sym->type, &esize);
  return lt_obj_new(c, kind, sym, off, 0, sym->v, 0, esize);
}

/* the local struct holding frame offset fo; *rel its offset in it */
static int lt_fobj(LtCtx *c, int fo, int *rel)
{
  for (int j = 0; j < c->nobj; j++)
    if (c->okind[j] == LT_OK_FRAME && fo >= c->ooff[j] && fo < c->ooff[j] + c->osize[j])
    {
      *rel = fo - c->ooff[j];
      return LT_OBJ(j);
    }
  return -1;
}

/* memory named in place: a local struct's (StackLoc[off]) or a global's
   (sym + off); its base and the offset in it, -1 none */
static int lt_direct_base(LtCtx *c, IROperand op, int *off)
{
  if (irop_is_none(op) || !op.is_lval)
    return -1;
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_is_local(op) && !irop_is_llocal(op) && !op.is_param && c->nobj)
    return lt_fobj(c, irop_get_stack_offset(op), off);
  if (irop_get_tag(op) == IROP_TAG_SYMREF && c->ir->lt_nb_gmembers)
  {
    IRPoolSymref *r = irop_get_symref_ex(c->ir, op);
    int es;
    if (!r || !r->sym || !lt_struct_or_array(&r->sym->type, &es))
      return -1;
    *off = r->addend;
    return lt_gobj(c, LT_OK_GLOBAL, r->sym, 0);
  }
  return -1;
}

/* an address in place: &s (+ off), &g (+ off); its base and offset, -1 none */
static int lt_obj_addr(LtCtx *c, const IRQuadCompact *q, IROperand op, int *off)
{
  if (irop_is_none(op))
    return -1;
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && (lt_is_addr(op) || (q && lt_lea_of_local(q, op))) && !op.is_param && c->nobj)
    return lt_fobj(c, irop_get_stack_offset(op), off);
  if (irop_get_tag(op) == IROP_TAG_SYMREF && !op.is_lval && c->ir->lt_nb_gmembers)
  {
    IRPoolSymref *r = irop_get_symref_ex(c->ir, op);
    int es;
    if (!r || !r->sym || !lt_struct_or_array(&r->sym->type, &es))
      return -1;
    *off = r->addend;
    return lt_gobj(c, LT_OK_GLOBAL, r->sym, 0);
  }
  return -1;
}

/* The member (base b, offset off) of record rec, when the offset is the
   member's: in place (`q->next`, `s.next`), or in an element at a constant
   index (`a[1].next` of a local or global array, `p[1].next` through a
   pointer).  *idx the index, -1 none. */
static int lt_member_at(LtCtx *c, int b, int off, const IRLtMember *rec, int *idx)
{
  int j = b <= -2 ? -2 - b : -1;
  *idx = -1;
  if (j >= 0 && c->okind[j] != LT_OK_GPTR && c->oesize[j] > 0)
  {
    /* an array object: off is from its start */
    if (off < 0 || off % c->oesize[j] != rec->foff)
      return 0;
    *idx = off / c->oesize[j];
    return 1;
  }
  if (off == rec->offset)
    return 1;
  /* through a pointer: element k of what it points at */
  if ((j < 0 || c->okind[j] == LT_OK_GPTR) && rec->offset == rec->foff && rec->esize > 0 && off > rec->foff &&
      (off - rec->foff) % rec->esize == 0)
  {
    *idx = (off - rec->foff) / rec->esize;
    return 1;
  }
  return 0;
}

/* the path (b, off), made when `make`; -1 none (or no room); idx >= 0: an
   array element's member, named `a[idx].m` */
static int lt_path(LtCtx *c, int b, int off, int tok, int owned, int idx)
{
  for (int k = 0; k < c->npaths; k++)
    if (c->pbase[k] == b && c->poff[k] == off && c->pidx[k] < 0)
      return c->nvars + k;
  if (!tok || c->npaths >= c->pcap)
    return -1;
  int k = c->npaths++, v = c->nvars + k, j = b <= -2 ? -2 - b : -1;
  c->pidx[k] = -1;
  const char *mn = get_tok_str(tok, NULL);
  char bn[256];
  /* lt_name and get_tok_str may share a buffer: copy the base's name */
  pstrcpy(bn, sizeof bn, j >= 0 ? get_tok_str(c->otok[j], NULL) : lt_name(c, b));
  if (idx >= 0)
    snprintf(bn + strlen(bn), sizeof bn - strlen(bn), "[%d]", idx);
  mn = get_tok_str(tok, NULL);
  c->pbase[k] = b;
  c->poff[k] = off;
  c->pname[k] = tcc_malloc(strlen(bn) + strlen(mn) + 3);
  sprintf(c->pname[k], "%s%s%s", bn, idx >= 0 || (j >= 0 && c->okind[j] != LT_OK_GPTR) ? "." : "->", mn);
  /* a path is a maybe variable; an inlined body's are its callee's to report */
  IRLtVar *pv = &c->ir->lt_vars[v];
  memset(pv, 0, sizeof *pv);
  pv->vreg = -1;
  pv->tok = tok;
  pv->kind = ((owned & LT_MEMBER_NOTMAYBE) ? 0 : LTV_MAYBE) | ((owned & LT_MEMBER_OWNED) ? LTV_OWNED : 0) |
             ((owned & LT_MEMBER_HANDLE) ? LTV_HANDLE : 0);
  pv->silent = j >= 0 ? c->osilent[j] : c->ir->lt_vars[b].silent;
  pv->arg_index = -1;
  return v;
}

/* the path of member offset off in the element at index variable ix of
   base b (`a[i].next`, `p[i].next`), made when tok; -1 none */
static int lt_path_ix(LtCtx *c, int b, int off, int32_t ix, int tok, int owned)
{
  for (int k = 0; k < c->npaths; k++)
    if (c->pbase[k] == b && c->poff[k] == off && c->pidx[k] == ix)
      return c->nvars + k;
  int in = ix_name(c->ir, ix);
  if (!tok || !in || c->npaths >= c->pcap)
    return -1;
  int k = c->npaths++, v = c->nvars + k, j = b <= -2 ? -2 - b : -1;
  char bn[256];
  pstrcpy(bn, sizeof bn, j >= 0 ? get_tok_str(c->otok[j], NULL) : lt_name(c, b));
  snprintf(bn + strlen(bn), sizeof bn - strlen(bn), "[%s].", get_tok_str(in, NULL));
  const char *mn = get_tok_str(tok, NULL);
  c->pbase[k] = b;
  c->poff[k] = off;
  c->pidx[k] = ix;
  c->pname[k] = tcc_malloc(strlen(bn) + strlen(mn) + 1);
  sprintf(c->pname[k], "%s%s", bn, mn);
  c->nixpaths++;
  IRLtVar *pv = &c->ir->lt_vars[v];
  memset(pv, 0, sizeof *pv);
  pv->vreg = -1;
  pv->tok = tok;
  pv->kind = ((owned & LT_MEMBER_NOTMAYBE) ? 0 : LTV_MAYBE) | ((owned & LT_MEMBER_OWNED) ? LTV_OWNED : 0) |
             ((owned & LT_MEMBER_HANDLE) ? LTV_HANDLE : 0);
  pv->silent = j >= 0 ? c->osilent[j] : c->ir->lt_vars[b].silent;
  pv->arg_index = -1;
  return v;
}

/* op's value as a pointer: the variable or path whose pointer value it is,
   plus *off; -1 when it is not one */
static int lt_addr_base(LtCtx *c, IROperand op, int *off)
{
  int32_t vreg;
  *off = 0;
  if (irop_is_none(op) || !irop_is_vreg_value(op) || lt_is_addr(op) || (vreg = irop_get_vreg(op)) < 0)
    return -1;
  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vreg);
    if (!lt_temp_valid(c, vreg))
      return -1;
    *off = c->toff[pos];
    return c->tbase[pos];
  }
  return lt_map(c, vreg);
}

/* the path a temp's value is the address of, -1 none */
static int lt_temp_path(LtCtx *c, IROperand op)
{
  int32_t vreg = irop_get_vreg(op);
  if (vreg < 0 || !lt_temp_valid(c, vreg))
    return -1;
  return c->taddr[TCCIR_DECODE_VREG_POSITION(vreg)];
}

/* *T, T holding a path's address: the member read or written in place */
static int lt_deref_path(LtCtx *c, IROperand op)
{
  int b, off;
  if (irop_is_none(op) || !op.is_lval)
    return -1;
  if (irop_get_tag(op) == IROP_TAG_VREG)
    return lt_temp_path(c, op);
  /* a local or global struct's member named in place (`g.next`) */
  if (!c->pcap || (b = lt_direct_base(c, op, &off)) == -1)
    return -1;
  if (c->okind[-2 - b] == LT_OK_GLOBAL)
    for (int r = 0; r < c->ir->lt_nb_gmembers; r++)
      if (c->ir->lt_gm_syms[r] == c->osym[-2 - b] && c->ir->lt_gmembers[r].offset == off)
      {
        const IRLtMember *gm = &c->ir->lt_gmembers[r];
        int idx = c->oesize[-2 - b] > 0 ? (off - gm->foff) / c->oesize[-2 - b] : -1;
        return lt_path(c, b, off, gm->tok, gm->owned, idx);
      }
  return lt_path(c, b, off, 0, 0, -1);
}

/* what a store through dest (not a path's own store) overwrites: a path
   (base, offset) reached without the member's record */
static int lt_store_hits_path(LtCtx *c, IROperand dest)
{
  int off, b;
  if (!c->npaths || irop_is_none(dest) || irop_get_tag(dest) != IROP_TAG_VREG || !dest.is_lval)
    return -1;
  IROperand val = dest;
  val.is_lval = 0;
  if ((b = lt_addr_base(c, val, &off)) == -1)
    return -1;
  return lt_path(c, b, off, 0, 0, -1);
}

/* An argument handed to a parameter that may write through it: what the
   call forgets -- the state index (a variable or path) whose paths go, and
   whether that index itself goes (`&q->next`: the path).  -1 nothing. */
static int lt_arg_target(LtCtx *c, IROperand op, int *self)
{
  int k;
  *self = 0;
  if (!c->npaths || irop_is_none(op))
    return -1;
  /* the member's value itself (q->next passed): what it points to */
  if ((k = lt_deref_path(c, op)) >= 0)
    return k;
  if (irop_get_tag(op) == IROP_TAG_VREG && op.is_lval)
    return -1; /* some other memory's value: not followed */
  if ((k = lt_temp_path(c, op)) >= 0)
  {
    *self = 1;
    return k;
  }
  /* &s, &g, or into them: every maybe member of the object */
  if ((k = lt_pointer_var(c, op)) >= 0)
    return k;
  {
    int off, b = lt_addr_base(c, op, &off);
    if (b <= -2)
      return b;
    if (b == -1 && (b = lt_obj_addr(c, NULL, op, &off)) <= -2)
      return b;
  }
  return -1;
}

static void lt_note_move(LtCtx *c, int v);

/* a member named tok of a tagged union (the variants): what it owns is the
   tag check's (tcc_ir_tag_check), live only while the tag selects it */
static int lt_tagged_tok(int tok)
{
  for (int t = 0; t < yasc_ntagged; t++)
    if (yasc_tagged_member(&yasc_tagged[t], tok) >= 0)
      return 1;
  return 0;
}

/* T (lt_arg_target) handed to a `T * view` parameter of call (a method's
   self): the owned members of it must be valid there -- the callee may read
   or replace them, and a `drop` one frees them.  A local struct's are the
   slot check's. */
static void lt_members_handed(LtCtx *c, int t, const IRLtCall *call, int line)
{
  if (t == -1 || (t <= -2 && c->okind[-2 - t] == LT_OK_FRAME))
    return;
  for (int k = 0; k < c->npaths; k++)
  {
    int v = c->nvars + k;
    uint8_t st = c->cur[v] & LT_OWNBITS;
    if (c->pbase[k] != t || c->pidx[k] >= 0 || !lt_owned(c, v) || lt_silent(c, v) || !(st & LT_MOVED) ||
        (lt_maybe(c, v) && (c->cur[v] & LT_NULLBITS) == LT_N_NULL) || lt_tagged_tok(c->ir->lt_vars[v].tok))
      continue;
    char bn[128];
    pstrcpy(bn, sizeof bn, t >= 0 ? lt_name(c, t) : get_tok_str(c->otok[-2 - t], NULL));
    if (call->callee)
      tcc_warning_c(warn_lifetime)("%i:'%s' %s %s when '%s' is handed to '%s': give it a new value first", line,
                                   lt_name(c, v), st == LT_MOVED ? "is" : "may be",
                                   c->move_drop[v] ? lt_freed(c, v) : "moved", bn, get_tok_str(call->callee, NULL));
    else
      tcc_warning_c(warn_lifetime)("%i:'%s' %s %s when '%s' is handed to a call: give it a new value first", line,
                                   lt_name(c, v), st == LT_MOVED ? "is" : "may be",
                                   c->move_drop[v] ? lt_freed(c, v) : "moved", bn);
    lt_note_move(c, v);
  }
}

/* the call happened: what it may have written is forgotten (flags & 1: T
   itself too); flags & 2, a `drop struct T * view` parameter: T's owned
   members are released */
static void lt_forget_call(LtCtx *c, int t, int flags, int line)
{
  lt_forget(c, t, flags & 1);
  if (!(flags & 2) || (t <= -2 && c->okind[-2 - t] == LT_OK_FRAME))
    return;
  for (int k = 0; k < c->npaths; k++)
  {
    int v = c->nvars + k;
    if (c->pbase[k] != t || c->pidx[k] >= 0 || !lt_owned(c, v))
      continue;
    c->cur[v] = (c->cur[v] & ~LT_OWNBITS) | LT_MOVED;
    c->move_line[v] = line;
    c->move_drop[v] = 1;
  }
}

/* lt_read_var, and a maybe member read in place (`*T`, T its address) */
static int lt_read_value(LtCtx *c, IROperand op)
{
  int v = lt_read_var(c, op);
  return v >= 0 ? v : lt_deref_path(c, op);
}

/* Is a value that may be null in op?  A maybe variable or member says so by
   its state, a temp by what defined it, the constant 0 always; anything else
   -- a plain pointer, other memory, arithmetic -- is taken to be a pointer. */
static int lt_value_null(LtCtx *c, IROperand op)
{
  int v;
  int32_t vreg;
  if (irop_is_none(op))
    return LT_N_SET;
  if (irop_is_immediate(op) && !op.is_lval)
    return irop_get_imm64_ex(c->ir, op) == 0 ? LT_N_NULL : LT_N_SET;
  if ((v = lt_read_var(c, op)) >= 0)
    return lt_maybe(c, v) ? c->cur[v] & LT_NULLBITS : LT_N_SET;
  vreg = irop_get_vreg(op);
  if (vreg >= 0 && irop_is_vreg_value(op) && lt_temp_valid(c, vreg))
    return lt_tnull(c, TCCIR_DECODE_VREG_POSITION(vreg));
  /* a maybe member read in place (one that is never null: not null) */
  if ((v = lt_deref_path(c, op)) >= 0)
    return lt_maybe(c, v) ? c->cur[v] & LT_NULLBITS : LT_N_SET;
  return LT_N_SET;
}

static const char *lt_null_what(int st)
{
  return st == LT_N_NULL ? "a null pointer" : "a pointer that may be null";
}

/* v (a variable, or a member path: `c->fd`) holds an int handle */
static int lt_handle(LtCtx *c, int v)
{
  return v >= 0 && v < c->nvars + c->npaths && (c->ir->lt_vars[v].kind & LTV_HANDLE);
}

/* an int handle's value: invalid (< 0), valid, or either */
static int lt_handle_null(LtCtx *c, IROperand op)
{
  int v;
  if (irop_is_none(op))
    return LT_N_SET;
  if (irop_is_immediate(op) && !op.is_lval)
    return irop_get_imm64_ex(c->ir, op) < 0 ? LT_N_NULL : LT_N_SET;
  if ((v = lt_read_var(c, op)) >= 0 && lt_handle(c, v))
    return lt_maybe(c, v) ? c->cur[v] & LT_NULLBITS : LT_N_SET;
  if (v >= 0)
    return LT_N_SET;
  return lt_value_null(c, op) == (LT_N_NULL | LT_N_SET) ? LT_N_NULL | LT_N_SET : LT_N_SET;
}

/* argument K (from 0) of CALLEE, a parameter that may not be null, gets a
   value that may be */
static void lt_null_arg(LtCtx *c, int st, int k, int callee, int line)
{
  if (callee)
    tcc_warning_c(warn_lifetime)("%i:%s is passed as argument %d of '%s', which is not maybe", line,
                                 lt_null_what(st), k + 1, get_tok_str(callee, NULL));
  else
    tcc_warning_c(warn_lifetime)("%i:%s is passed as argument %d, which is not maybe", line, lt_null_what(st), k + 1);
}

/* a value with no name dereferenced where it may be null, once per line */
static void lt_report_unnamed(LtCtx *c, int st, int line)
{
  if (!c->report || c->cur_inl)
    return;
  for (int k = 0; k < 8 && k < c->nunnamed; k++)
    if (c->unnamed_lines[k] == line)
      return;
  c->unnamed_lines[c->nunnamed++ % 8] = line;
  tcc_warning_c(warn_lifetime)("%i:%s is dereferenced", line, lt_null_what(st));
}

/* *p where p is maybe */
static void lt_deref(LtCtx *c, int v, int line)
{
  uint8_t st = c->cur[v] & LT_NULLBITS;
  if (!lt_maybe(c, v) || !(st & LT_N_NULL))
    return;
  if (c->ir->lt_vars[v].kind & LTV_RESULT)
  {
    /* an inlined call's result: a value with no name, as the call's */
    lt_report_unnamed(c, st, line);
  }
  else if (!lt_silent(c, v))
    tcc_warning_c(warn_lifetime)("%i:'%s' %s null where it is dereferenced", line, lt_name(c, v),
                                 st == LT_N_NULL ? "is" : "may be");
  /* reported once: past here it was not */
  c->cur[v] = (c->cur[v] & ~LT_NULLBITS) | LT_N_SET;
}

/* *T, T a temp holding a value with no name that may be null: a call's
   result (`find(k)->v`), the join of `?:` (`(c ? p : q)->v`).  A value
   copied from a variable or member is that one's to report (lt_deref). */
static void lt_deref_value(LtCtx *c, IROperand op, int line)
{
  int32_t vreg;
  int pos, g, st;
  if (irop_is_none(op) || irop_get_tag(op) != IROP_TAG_VREG || !op.is_lval || (vreg = irop_get_vreg(op)) < 0 ||
      !lt_temp_valid(c, vreg))
    return;
  pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (c->palias[pos] >= 0 || c->alias[pos] >= 0)
    return;
  /* a member of it (`find(k)->next`): the value it is an offset from */
  if (c->tderiv[pos] >= 0)
  {
    pos = c->tderiv[pos];
    if (!lt_tpos_valid(c, pos) || c->palias[pos] >= 0 || c->alias[pos] >= 0)
      return;
  }
  st = lt_tnull(c, pos) & LT_NULLBITS;
  if (!(st & LT_N_NULL))
    return;
  lt_report_unnamed(c, st, lt_deref_line(c, op, line));
  /* reported once */
  g = c->tglob ? c->tglob[pos] : -1;
  if (g >= 0)
    c->cur[c->gslot + g] = LT_N_SET;
  else
    c->tnull[pos] = LT_N_SET;
}

/* *p of an out parameter: read before it was written? */
static void lt_out_read(LtCtx *c, int v, int line)
{
  uint8_t st = c->cur[v] & LT_WBITS;
  if (!lt_out(c, v) || !(st & LT_W_NO))
    return;
  if (!lt_silent(c, v))
    tcc_warning_c(warn_lifetime)("%i:'*%s' %s read before it is written", line, lt_name(c, v),
                                 st == LT_W_NO ? "is" : "may be");
  c->cur[v] = (c->cur[v] & ~LT_WBITS) | LT_W_YES;
}

/* *p written -- or p handed somewhere that may write it */
static void lt_out_write(LtCtx *c, int v)
{
  if (v >= 0 && lt_out(c, v))
    c->cur[v] = (c->cur[v] & ~LT_WBITS) | LT_W_YES;
}

static void lt_note_move(LtCtx *c, int v)
{
  tcc_note("%i:'%s' was %s here", c->move_line[v], lt_name(c, v), c->move_drop[v] ? lt_freed(c, v) : "moved");
}

/* borrower row R holds, on this path, a pointer into owner O (an owned
   variable; -1 none) in state OST, or what row SR holds (-1 none) */
static void lt_brow_set_st(LtCtx *c, int r, int o, uint8_t ost, int sr)
{
  uint8_t *e = c->cur + c->bbase + c->brow_at[r];
  for (int j = 0; j < c->brow_n[r]; j++)
  {
    int32_t oj = c->brow_own[r * LT_MAX_BOWN + j];
    uint8_t s = oj == o ? ost : LT_UNINIT;
    for (int k = 0; sr >= 0 && s == LT_UNINIT && k < c->brow_n[sr]; k++)
      if (c->brow_own[sr * LT_MAX_BOWN + k] == oj && (c->cur[c->bbase + c->brow_at[sr] + k] & LT_OWNBITS))
        s = c->cur[c->bbase + c->brow_at[sr] + k] & LT_OWNBITS;
    e[j] = (e[j] & ~LT_OWNBITS) | s;
  }
}

static void lt_brow_set(LtCtx *c, int r, int o, int sr)
{
  lt_brow_set_st(c, r, o, LT_OWN, sr);
}

/* row R (named NAME) read: any owner it may borrow from gone is reported,
   once -- from there on it borrows nothing known */
static void lt_brow_use(LtCtx *c, int r, const char *name, int silent, int line)
{
  uint8_t *e = c->cur + c->bbase + c->brow_at[r];
  for (int j = 0; j < c->brow_n[r]; j++)
    if (e[j] & LT_MOVED)
    {
      int o = c->brow_own[r * LT_MAX_BOWN + j];
      if (!silent && c->report)
      {
        tcc_warning_c(warn_lifetime)("%i:'%s' %s used after '%s', which it %s from, was %s", line, name,
                                     (e[j] & LT_OWNBITS) == LT_MOVED ? "is" : "may be", lt_name(c, o),
                                     (e[j] & LT_UNINIT) ? "may borrow" : "borrows", c->move_drop[o] ? "freed" : "moved");
        lt_note_move(c, o);
      }
      for (int k = 0; k < c->brow_n[r]; k++)
        e[k] = (e[k] & ~LT_OWNBITS) | LT_UNINIT;
      break;
    }
}

/* the global temp a pointer value is read from (a copy of the temp, or an
   offset from it), -1 none */
static int lt_src_glob(LtCtx *c, const IRQuadCompact *q, IROperand src1, IROperand src2, int is_copy)
{
  if (!c->tglob || (!is_copy && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB))
    return -1;
  for (int k = 0; k < 1 + (q->op == TCCIR_OP_ADD); k++)
  {
    IROperand o = k ? src2 : src1;
    int32_t vr = irop_is_none(o) ? -1 : irop_get_vreg(o);
    int pos;
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || !irop_is_vreg_value(o) || o.is_lval ||
        (pos = TCCIR_DECODE_VREG_POSITION(vr)) >= c->nalias || c->tglob[pos] < 0)
      continue;
    return c->tglob[pos];
  }
  return -1;
}

/* ... its row, -1 none */
static int lt_src_bvar(LtCtx *c, const IRQuadCompact *q, IROperand src1, IROperand src2, int is_copy);

static int lt_src_brow(LtCtx *c, const IRQuadCompact *q, IROperand src1, IROperand src2, int is_copy)
{
  int g = c->gbrow ? lt_src_glob(c, q, src1, src2, is_copy) : -1, v;
  if (g >= 0)
    return c->gbrow[g];
  /* a variable borrowing from several owners */
  if (c->vbrow && (v = lt_src_bvar(c, q, src1, src2, is_copy)) >= 0)
    return c->vbrow[v];
  return -1;
}

/* a read of v that does not move it */
static void lt_use(LtCtx *c, int v, int line)
{
  uint8_t st = c->cur[v] & LT_OWNBITS;
  if (c->vbrow && v < c->nvars && c->vbrow[v] >= 0)
  {
    lt_brow_use(c, c->vbrow[v], lt_name(c, v), lt_silent(c, v), line);
    return;
  }
  if (!(st & LT_MOVED))
    return;
  if (!lt_owned(c, v))
  {
    /* a borrowing pointer whose owner was moved or freed */
    int o = c->borrow[v];
    if (!lt_silent(c, v) && o >= 0)
    {
      tcc_warning_c(warn_lifetime)("%i:'%s' %s used after '%s', which it borrows from, was %s", line, lt_name(c, v),
                                   (st & (LT_OWN | LT_UNINIT)) ? "may be" : "is", lt_name(c, o),
                                   c->move_drop[o] ? "freed" : "moved");
      lt_note_move(c, o);
    }
    c->cur[v] = (c->cur[v] & ~LT_OWNBITS) | LT_UNINIT;
    return;
  }
  if (!lt_silent(c, v))
  {
    tcc_warning_c(warn_lifetime)("%i:'%s' %s used after it was %s", line, lt_name(c, v),
                                 st == LT_MOVED ? "is" : "may be", c->move_drop[v] ? lt_freed(c, v) : "moved");
    lt_note_move(c, v);
  }
  /* reported once: from here on nothing is known about it */
  c->cur[v] = (c->cur[v] & ~LT_OWNBITS) | LT_UNINIT;
}

/* o was moved or freed: what borrows from it now points at what it no longer owns */
static void lt_owner_gone(LtCtx *c, int o)
{
  for (int v = 0; v < c->nv; v++)
    if (c->borrow[v] == o && (c->cur[v] & LT_OWN))
      c->cur[v] = (c->cur[v] & ~LT_OWNBITS) | LT_MOVED;
  for (int r = 0; r < c->nbrow; r++)
    for (int j = 0; j < c->brow_n[r]; j++)
      if (c->brow_own[r * LT_MAX_BOWN + j] == o)
      {
        uint8_t *e = c->cur + c->bbase + c->brow_at[r] + j;
        if (*e & LT_OWN)
          *e = (*e & ~LT_OWN) | LT_MOVED;
      }
}

/* v handed on: into an owned parameter (`drop`: freed) or returned */
static void lt_move(LtCtx *c, int v, int drop, int line)
{
  uint8_t st = c->cur[v] & LT_OWNBITS;
  if ((st & LT_MOVED) && !lt_silent(c, v))
  {
    if (drop && c->move_drop[v])
      tcc_warning_c(warn_lifetime)("%i:'%s' %s %s twice", line, lt_name(c, v), st == LT_MOVED ? "is" : "may be",
                                   lt_freed(c, v));
    else
      tcc_warning_c(warn_lifetime)("%i:'%s' %s used after it was %s", line, lt_name(c, v),
                                   st == LT_MOVED ? "is" : "may be", c->move_drop[v] ? lt_freed(c, v) : "moved");
    lt_note_move(c, v);
  }
  c->cur[v] = (c->cur[v] & ~LT_OWNBITS) | LT_MOVED;
  c->move_line[v] = line;
  c->move_drop[v] = drop;
  lt_owner_gone(c, v);
}

/* v still owns memory where it is about to be lost */
static void lt_leak(LtCtx *c, int v, int line, const char *where)
{
  uint8_t st = c->cur[v] & LT_OWNBITS;
  /* a member's memory belongs to its struct: it leaks with it, if at all */
  if (!lt_owned(c, v) || !(st & LT_OWN) || lt_silent(c, v) || v >= c->nvars)
    return;
  tcc_warning_c(warn_lifetime)("%i:'%s' leaks: it %s %s %s", line, lt_name(c, v),
                               st == LT_OWN ? "still owns" : "may still own",
                               v < c->nvars && (c->ir->lt_vars[v].kind & LTV_HANDLE) ? "a handle" : "memory", where);
}

static void lt_events_at(LtCtx *c, int i)
{
  for (int k = c->ev_first[i]; k < c->ev_first[i + 1]; k++)
  {
    int v = c->ev[k].var;
    if (v <= LT_EV_RESULT(0))
    {
      /* an inlined body's result: as maybe as its type says */
      v = LT_EV_RESULT(0) - v;
      if (!c->ir->lt_vars[v].untracked)
        c->cur[v] = (c->cur[v] & ~LT_NULLBITS) | LT_N_NULL | LT_N_SET;
      continue;
    }
    if (v == LT_EV_RETURN || c->ir->lt_vars[v].untracked)
      continue;
    lt_leak(c, v, c->ev[k].line, "at the end of its scope");
    c->cur[v] = LT_UNINIT | LT_N_SET | (c->cur[v] & LT_WBITS);
    for (int row = 0; row < c->nclink && c->clbase >= 0; row++)
      if (c->cl_x[row] == v)
        c->cur[c->clbase + row] = LT_N_NULL;
    if (c->nlink && v < c->nvars && c->lslot[v] >= 0)
      c->cur[c->lslot[v]] |= LT_N_NULL;
    lt_forget(c, v, 0);
  }
}

static int lt_ends_flow(TCCIRState *ir, IRQuadCompact *q)
{
  switch (q->op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
    return 1;
  default:
    return tcc_ir_cfg_instr_ends_flow(ir, q);
  }
}

/* V, an inlined call's view result variable: the owner the parameters its
   result borrows from were bound to (one, the same for all), -1 none; -2 V
   is no such variable */
static int lt_pres_owner(LtCtx *c, int v)
{
  const TCCIRState *ir = c->ir;
  for (int e = 0; e < ir->lt_nb_pres; e++)
  {
    const int32_t *r = &ir->lt_pres[6 * e];
    int o = -1;
    if (r[0] != v)
      continue;
    for (int j = 0; j < r[1]; j++)
    {
      int b = r[2 + j] >= 0 && r[2 + j] < c->nvars ? c->bind_owner[r[2 + j]] : -1;
      if (b >= 0 && o >= 0 && b != o)
        return -1; /* several: as a call's, not followed */
      if (b >= 0)
        o = b;
    }
    return r[1] < 0 ? -1 : o;
  }
  return -2;
}

/* OP, a call's result in a temp: the owned variable passed where that
   call's result borrows from, -1 */
static int lt_call_result_owner(LtCtx *c, IROperand op)
{
  int32_t vr = irop_is_none(op) || op.is_lval || !irop_is_vreg_value(op) ? -1 : irop_get_vreg(op);
  int bc;
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  bc = lt_tcall(c, TCCIR_DECODE_VREG_POSITION(vr));
  return bc >= 0 && bc < c->ncall_bsrc ? c->call_bsrc[bc] : -1;
}

/* the owned variable (or owned member path) a pointer value computed by Q
   points into: a copy of it, an offset from it, the result of a call that
   borrows from it (strchr(b, c)); -1 none.  SELF: the variable assigned. */
static int lt_borrow_from(LtCtx *c, const IRQuadCompact *q, IROperand src1, IROperand src2, int is_copy, int self)
{
  int from = -1, bc = -1;
  if (is_copy && !(irop_get_tag(src1) == IROP_TAG_VREG && src1.is_lval))
    from = lt_pointer_var(c, src1);
  else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    from = lt_pointer_var(c, src1) >= 0 ? lt_pointer_var(c, src1) : lt_pointer_var(c, src2);
  /* the result of a call that borrows from an owned argument (strchr(b, c)) */
  if (q->op == TCCIR_OP_FUNCCALLVAL)
    bc = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(c->ir, src2));
  else if (from < 0 && (is_copy || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB))
    for (int k = 0; k < 1 + (q->op != TCCIR_OP_SUB && !is_copy) && bc < 0; k++)
    {
      IROperand o = k ? src2 : src1;
      if (!irop_is_none(o) && irop_is_vreg_value(o) && !o.is_lval && irop_get_vreg(o) >= 0 &&
          TCCIR_DECODE_VREG_TYPE(irop_get_vreg(o)) == TCCIR_VREG_TYPE_TEMP)
        bc = lt_tcall(c, TCCIR_DECODE_VREG_POSITION(irop_get_vreg(o)));
    }
  if (from < 0 && bc >= 0 && bc < c->ncall_bsrc && c->call_bsrc[bc] >= 0)
    from = c->call_bsrc[bc];
  /* an inlined call's view result (-O1+): what its declared result borrows,
     whatever the body returned -- a call's result at -O0 */
  if (c->bind_owner && c->ir->lt_nb_pres)
  {
    int r = self >= 0 && self < c->nvars ? lt_pres_owner(c, self) : -2;
    if (r != -2)
      from = r;
    /* a copy of (or an offset from) such a result: what it borrows */
    else if (from >= 0 && from < c->nvars && !lt_owned(c, from) && lt_pres_owner(c, from) != -2)
      from = c->borrow[from] >= 0 ? c->borrow[from] : -1;
  }
  /* a copy of (or an offset from) a pointer borrowing from one owner: that
     owner (several owners: lt_src_bvar) */
  if (from >= 0 && from < c->nvars && from != self && !lt_owned(c, from) && c->borrow[from] >= 0)
    from = c->borrow[from];
  if (from >= 0 && (from == self || !lt_owned(c, from)))
    from = -1;
  return from;
}

/* the variable a pointer value is read from (a copy, or an offset from it)
   when it borrows from several owners (more than LT_MAX_BOWN too: the copy
   is not followed either, and says so), -1 none */
static int lt_src_bvar(LtCtx *c, const IRQuadCompact *q, IROperand src1, IROperand src2, int is_copy)
{
  int v = -1;
  if (is_copy && !(irop_get_tag(src1) == IROP_TAG_VREG && src1.is_lval))
    v = lt_pointer_var(c, src1);
  else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    v = lt_pointer_var(c, src1) >= 0 ? lt_pointer_var(c, src1) : lt_pointer_var(c, src2);
  if (v < 0 || v >= c->nvars || lt_owned(c, v) || c->borrow[v] != -2 || !c->vbcnt ||
      (c->vbcnt[v] & ~LT_BOWN_VIA) < 1)
    return -1;
  return v;
}

/* `out T * borrows(s) * view p` written at a call: V, whose address went
   there, now points into FROM (the owned variable passed for s, -1 none
   known) -- as when a call's view result is assigned to it */
static void lt_out_borrows(LtCtx *c, int v, int from)
{
  if (v < 0 || v >= c->nvars || lt_owned(c, v) || c->ir->lt_vars[v].untracked)
    return;
  if (from >= 0 && (from == v || from >= c->nvars || !lt_owned(c, from)))
    from = -1;
  if (c->collect && from >= 0)
    c->borrow[v] = c->borrow[v] == -1 || c->borrow[v] == from ? from : -2;
  if (c->collect && c->vbcnt && from >= 0)
    lt_bset_add(c->vbset + v * LT_MAX_BOWN, &c->vbcnt[v], from, 0);
  lt_forget(c, v, 0);
  c->cur[v] = (c->cur[v] & ~LT_OWNBITS) | (from >= 0 && c->borrow[v] == from ? LT_OWN : LT_UNINIT);
  if (c->vbrow && c->vbrow[v] >= 0)
    lt_brow_set(c, c->vbrow[v], from, -1);
}

/* ---- local structs whose values borrow ----------------------------------
 *
 * `struct tokenizer t = tok_init(buf);` -- t borrows what tok_init's result
 * borrows (`struct T borrows(src) f(...)`): the owned variable passed there.
 * Followed by frame offset: the call writing its result into t (the hidden
 * first argument), a copy of another such local (word by word: a temp loaded
 * from it stored into t), a pointer stored into one of t's pointer members
 * (it borrows what that pointer points into: an owned variable, or the one a
 * borrowing pointer borrows from -- the inlined body of tok_init at -O1+).
 * A pointer member of t read after that owner was freed or moved is
 * reported.  Its address passed to a parameter that only reads is a read; to
 * any other parameter, t forgets what it borrows; anywhere else (stored,
 * assigned) t is not followed. */
#define LT_NOADDR INT32_MIN

/* the object frame offset OFF is in (WORD: the word in it), -1 */
static int lt_so_at(LtCtx *c, int32_t off, int *word)
{
  const IRLtSobj *so = c->ir->lt_sobjs;
  for (int k = 0; k < c->nsobj; k++)
    if (!c->so_untracked[k] && off >= so[k].off && off < so[k].off + so[k].size)
    {
      *word = (off - so[k].off) >> 2;
      return k;
    }
  return -1;
}

static int lt_so_tpos(LtCtx *c, IROperand op)
{
  int32_t vr = irop_is_none(op) ? -1 : irop_get_vreg(op);
  int pos;
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || (pos = TCCIR_DECODE_VREG_POSITION(vr)) >= c->nalias)
    return -1;
  return pos;
}

/* the frame address an operand's value is (not dereferenced), LT_NOADDR */
static int32_t lt_so_addr(LtCtx *c, const IRQuadCompact *q, IROperand op, int first)
{
  int pos;
  if (irop_is_none(op))
    return LT_NOADDR;
  if ((lt_is_addr(op) || (first && lt_lea_of_local(q, op))) && irop_get_vreg(op) < 0)
    return (int32_t)irop_get_imm64_ex(c->ir, op);
  if (!op.is_lval && irop_get_tag(op) == IROP_TAG_VREG && (pos = lt_so_tpos(c, op)) >= 0)
    return c->so_tfa[pos];
  return LT_NOADDR;
}

/* the frame address memory operand OP reads or writes, LT_NOADDR */
static int32_t lt_so_mem(LtCtx *c, IROperand op)
{
  int pos;
  if (irop_is_none(op) || !op.is_lval)
    return LT_NOADDR;
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_is_local(op) && irop_get_vreg(op) < 0)
    return (int32_t)irop_get_imm64_ex(c->ir, op);
  if (irop_get_tag(op) == IROP_TAG_VREG && (pos = lt_so_tpos(c, op)) >= 0)
    return c->so_tfa[pos];
  return LT_NOADDR;
}

static int lt_so_word_ptr(LtCtx *c, int k, int w)
{
  return w >= 0 && w < 64 && ((c->ir->lt_sobjs[k].pmask >> w) & 1);
}

/* object K now holds pointers into the N owners OWN (all of them, -1
   entries none; N -1 too many: nothing known) */
static void lt_so_set_list(LtCtx *c, int k, const int32_t *own, int n)
{
  if (n < 0)
  {
    if (c->collect)
      c->sbcnt[k] = LT_MAX_BOWN + 1;
    n = 0;
  }
  for (int j = 0; c->collect && j < n; j++)
    if (own[j] >= 0)
      lt_bset_add(c->sbset + k * LT_MAX_BOWN, &c->sbcnt[k], own[j], 0);
  if (c->sbrow && c->sbrow[k] >= 0)
  {
    int r = c->sbrow[k];
    uint8_t *e = c->cur + c->bbase + c->brow_at[r];
    for (int j = 0; j < c->brow_n[r]; j++)
    {
      uint8_t st = LT_UNINIT;
      for (int m = 0; m < n; m++)
        if (own[m] >= 0 && own[m] == c->brow_own[r * LT_MAX_BOWN + j])
          st = LT_OWN;
      e[j] = (e[j] & ~LT_OWNBITS) | st;
    }
  }
}

/* object K now holds what owner O (state OST; -1 none) or object SK (-1
   none) holds; in the collect walk, the owners it may borrow from */
static void lt_so_set(LtCtx *c, int k, int o, uint8_t ost, int sk)
{
  if (c->collect)
  {
    if (o >= 0)
      lt_bset_add(c->sbset + k * LT_MAX_BOWN, &c->sbcnt[k], o, 0);
    else if (sk >= 0 && sk != k)
    {
      int n = c->sbcnt[sk] & ~LT_BOWN_VIA;
      if (n > LT_MAX_BOWN)
        c->sbcnt[k] = LT_MAX_BOWN + 1;
      for (int j = 0; j < n && j < LT_MAX_BOWN; j++)
        lt_bset_add(c->sbset + k * LT_MAX_BOWN, &c->sbcnt[k], c->sbset[sk * LT_MAX_BOWN + j], 0);
    }
  }
  if (c->sbrow && c->sbrow[k] >= 0 && sk != k)
    lt_brow_set_st(c, c->sbrow[k], o, ost, sk >= 0 ? c->sbrow[sk] : -1);
}

static void lt_so_use(LtCtx *c, int k, int line)
{
  if (c->sbrow && c->sbrow[k] >= 0)
    lt_brow_use(c, c->sbrow[k], get_tok_str(c->ir->lt_sobjs[k].tok, NULL), c->ir->lt_sobjs[k].silent || !c->report,
                line);
}

static void lt_sobj_instr(LtCtx *c, int i, IRQuadCompact *q, IROperand dest, IROperand src1, IROperand src2,
                          int defines)
{
  TCCIRState *ir = c->ir;
  int w, k;
  int32_t off;
  /* reads of a pointer member */
  for (int n = 0; n < 2; n++)
  {
    IROperand o = n ? src2 : src1;
    if ((off = lt_so_mem(c, o)) != LT_NOADDR && (k = lt_so_at(c, off, &w)) >= 0 && lt_so_word_ptr(c, k, w))
      lt_so_use(c, k, q->line_num);
  }
  /* a write */
  if (!defines && (off = lt_so_mem(c, dest)) != LT_NOADDR && (k = lt_so_at(c, off, &w)) >= 0)
  {
    int sp = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN) && !src1.is_lval ? lt_so_tpos(c, src1) : -1;
    int sk = sp >= 0 ? c->so_tld[sp] : -1;
    if (sp >= 0 && c->so_tcw[sp] >= 0)
    {
      /* a word of a call's result returned in registers */
      int cid = c->so_tcw[sp] >> 1;
      lt_so_set_list(c, k, c->so_cown + cid * 4, c->so_ncown[cid]);
    }
    else if (sk >= 0)
      lt_so_set(c, k, -1, 0, sk); /* a copy of another such local */
    else if (lt_so_word_ptr(c, k, w))
    {
      /* a pointer stored into a pointer member: what it points into */
      int o = -1;
      uint8_t ost = LT_OWN;
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN)
      {
        o = lt_borrow_from(c, q, src1, irop_make_none(), 1, -1);
        if (o < 0)
        {
          int bw = src1.is_lval ? -1 : lt_pointer_var(c, src1);
          if (bw >= 0 && bw < c->nvars && !lt_owned(c, bw) && c->borrow[bw] >= 0 &&
              (c->collect || (c->cur[bw] & LT_OWNBITS & ~LT_UNINIT)))
            o = c->borrow[bw], ost = c->collect ? LT_OWN : c->cur[bw] & LT_OWNBITS;
        }
      }
      lt_so_set(c, k, o, ost, -1);
    }
  }
  if (q->op == TCCIR_OP_FUNCPARAMVAL && (off = lt_so_addr(c, q, src1, 0)) != LT_NOADDR &&
      (k = lt_so_at(c, off, &w)) >= 0)
  {
    int64_t enc = irop_get_imm64_ex(ir, src2);
    int call_id = TCCIR_DECODE_CALL_ID(enc), pk = TCCIR_DECODE_PARAM_IDX(enc);
    IRLtCall *call = call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
    if (call_id >= 0 && call_id < c->nso_calls && c->so_sret[call_id] == k && pk == 0)
      ; /* the call writes its result here */
    else if (call && call->noted && (pk -= call->implicit) >= 0 && pk < call->nparams && ((call->keep >> pk) & 1))
      lt_so_use(c, k, q->line_num);
    else
      lt_so_set(c, k, -1, 0, -1);
  }
  /* an argument where a call's struct result borrows from: the owner it
     points into (found in the collect walk; the same on every walk) */
  if (q->op == TCCIR_OP_FUNCPARAMVAL && c->collect)
  {
    int64_t enc = irop_get_imm64_ex(ir, src2);
    int call_id = TCCIR_DECODE_CALL_ID(enc), pk = TCCIR_DECODE_PARAM_IDX(enc);
    IRLtCall *call = call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
    if (call && call->sborrows && call_id < c->nso_calls && (pk -= call->implicit) >= 0 &&
        pk < 32 && ((call->sborrows >> pk) & 1) && c->so_ncown[call_id] >= 0)
    {
      int o = lt_borrow_from(c, q, src1, irop_make_none(), 1, -1), n = c->so_ncown[call_id], j;
      for (j = 0; o >= 0 && j < n && c->so_cown[call_id * 4 + j] != o; j++)
        ;
      if (o >= 0 && j == n)
      {
        if (n == 4)
          c->so_ncown[call_id] = -1;
        else
          c->so_cown[call_id * 4 + n] = o, c->so_ncown[call_id] = n + 1;
      }
    }
  }
  if ((q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID) && irop_config[q->op].has_src2)
  {
    int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
    if (call_id >= 0 && call_id < c->nso_calls && (k = c->so_sret[call_id]) >= 0 && !c->so_untracked[k])
      lt_so_set_list(c, k, c->so_cown + call_id * 4, c->so_ncown[call_id]);
  }
}

/* before instruction I: inlined calls' borrowing-struct results */
static void lt_sres_at(LtCtx *c, int i)
{
  for (int e = c->sres_first[i]; e < c->sres_first[i + 1]; e++)
  {
    const int32_t *r = &c->ir->lt_sres[7 * e];
    int32_t own[4];
    int w, k = lt_so_at(c, r[1], &w);
    if (k < 0)
      continue;
    for (int j = 0; j < r[2]; j++)
      own[j] = r[3 + j] >= 0 && r[3 + j] < c->nvars && c->bind_owner[r[3 + j]] >= 0 ? c->bind_owner[r[3 + j]] : -1;
    lt_so_set_list(c, k, own, r[2]);
  }
}

/* Before the walks: which temps hold an object's address, which were loaded
   from one, which calls write their result into one, and which objects have
   an address going somewhere else (not followed). */
static void lt_sobj_prepare(LtCtx *c)
{
  TCCIRState *ir = c->ir;
  uint8_t *ndef = tcc_mallocz(c->nalias + 1);
  int w;
  c->nsobj = ir->lt_nb_sobjs;
  c->so_untracked = tcc_mallocz(c->nsobj);
  c->so_tfa = tcc_malloc((c->nalias + 1) * sizeof(int32_t));
  c->so_tld = tcc_malloc((c->nalias + 1) * sizeof(int32_t));
  for (int p = 0; p <= c->nalias; p++)
    c->so_tfa[p] = LT_NOADDR, c->so_tld[p] = -1;
  c->nso_calls = ir->next_call_id;
  c->so_sret = tcc_malloc((c->nso_calls + 1) * sizeof(int32_t));
  memset(c->so_sret, 0xff, (c->nso_calls + 1) * sizeof(int32_t));
  c->sbset = tcc_malloc((c->nsobj * LT_MAX_BOWN + 1) * sizeof(int32_t));
  c->sbcnt = tcc_mallocz(c->nsobj + 1);
  c->so_cown = tcc_malloc((c->nso_calls * 4 + 1) * sizeof(int32_t));
  c->so_ncown = tcc_mallocz(c->nso_calls + 1);
  c->sres_first = tcc_mallocz((c->n + 2) * sizeof(int));
  for (int e = 0; e < ir->lt_nb_sres; e++)
    c->sres_first[(ir->lt_sres[7 * e] < c->n ? ir->lt_sres[7 * e] : c->n) + 1]++;
  for (int i = 0; i <= c->n; i++)
    c->sres_first[i + 1] += c->sres_first[i];
  c->so_tcw = tcc_malloc((c->nalias + 1) * sizeof(int32_t));
  memset(c->so_tcw, 0xff, (c->nalias + 1) * sizeof(int32_t));
  for (int i = 0; i < c->n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand d = irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    int pos = !irop_is_none(d) && irop_dest_defines_vreg(d) ? lt_so_tpos(c, d) : -1;
    if (pos >= 0 && ndef[pos] < 2)
      ndef[pos]++;
  }
  /* a borrowing struct returned in registers (4 bytes, or a static callee
     at -O1+): the temps holding its words, and where they are stored -- a
     place followed as a local struct (silent: it has no name) */
  for (int i = 0; i < c->n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand d = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    IROperand s1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
    IROperand s2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    int pos = !irop_is_none(d) && irop_dest_defines_vreg(d) ? lt_so_tpos(c, d) : -1;
    int sp = !irop_is_none(s1) && !s1.is_lval && irop_is_vreg_value(s1) ? lt_so_tpos(c, s1) : -1;
    if (pos >= 0 && ndef[pos] == 1)
    {
      if (q->op == TCCIR_OP_FUNCCALLVAL && cfg->has_src2)
      {
        int cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, s2));
        if (cid >= 0 && cid < ir->lt_nb_calls && cid < c->nso_calls && ir->lt_calls[cid].ssize &&
            !ir->lt_calls[cid].implicit)
          c->so_tcw[pos] = 2 * cid;
      }
      else if (sp >= 0 && c->so_tcw[sp] >= 0 && (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD))
        c->so_tcw[pos] = c->so_tcw[sp];
      else if (sp >= 0 && c->so_tcw[sp] >= 0 && !(c->so_tcw[sp] & 1) && q->op == TCCIR_OP_SHR &&
               irop_is_immediate(s2) && irop_get_imm64_ex(ir, s2) == 32)
        c->so_tcw[pos] = c->so_tcw[sp] | 1;
    }
    else if (q->op == TCCIR_OP_STORE && sp >= 0 && c->so_tcw[sp] >= 0 && !irop_is_none(d) && d.is_lval &&
             irop_get_tag(d) == IROP_TAG_STACKOFF && irop_is_local(d) && irop_get_vreg(d) < 0)
    {
      const IRLtCall *call = &ir->lt_calls[c->so_tcw[sp] >> 1];
      int32_t base = (int32_t)irop_get_imm64_ex(ir, d) - 4 * (c->so_tcw[sp] & 1);
      int k, w;
      for (k = 0; k < ir->lt_nb_sobjs; k++)
        if (base >= ir->lt_sobjs[k].off && base < ir->lt_sobjs[k].off + ir->lt_sobjs[k].size)
          break;
      if (k == ir->lt_nb_sobjs && ir->lt_nb_sobjs < 32)
      {
        IRLtSobj *o;
        ir->lt_sobjs = lt_grow(ir->lt_sobjs, ir->lt_nb_sobjs, sizeof(IRLtSobj));
        o = &ir->lt_sobjs[ir->lt_nb_sobjs++];
        o->off = base, o->size = call->ssize, o->tok = 0, o->pmask = call->spmask, o->silent = 1;
      }
      (void)w;
    }
  }
  c->nsobj = ir->lt_nb_sobjs;
  c->so_untracked = tcc_realloc(c->so_untracked, c->nsobj + 1);
  memset(c->so_untracked, 0, c->nsobj + 1);
  c->sbset = tcc_realloc(c->sbset, (c->nsobj * LT_MAX_BOWN + 1) * sizeof(int32_t));
  c->sbcnt = tcc_realloc(c->sbcnt, c->nsobj + 1);
  memset(c->sbcnt, 0, c->nsobj + 1);
  for (int i = 0; i < c->n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand ops[3];
    int has[3] = {cfg->has_dest, cfg->has_src1, cfg->has_src2};
    ops[0] = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    ops[1] = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
    ops[2] = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    int dpos = has[0] && !irop_is_none(ops[0]) && irop_dest_defines_vreg(ops[0]) ? lt_so_tpos(c, ops[0]) : -1;
    int once = dpos >= 0 && ndef[dpos] == 1;
    for (int n = 0; n < 3; n++)
    {
      int32_t a;
      int k;
      if (!has[n] || irop_is_none(ops[n]) || (n == 0 && dpos >= 0))
        continue;
      /* a load from an object: the temp is a copy of a word of it */
      if (n == 1 && once && (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) &&
          (a = lt_so_mem(c, ops[1])) != LT_NOADDR && (k = lt_so_at(c, a, &w)) >= 0)
        c->so_tld[dpos] = k;
      if ((a = lt_so_addr(c, q, ops[n], n == 1)) == LT_NOADDR || (k = lt_so_at(c, a, &w)) < 0)
        continue;
      /* an object's address: into a temp (a copy, plus a constant), or an
         argument -- anything else and the object is not followed */
      if (n == 1 && once && (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA))
        c->so_tfa[dpos] = a;
      else if (n == 1 && once && (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_is_immediate(ops[2]) &&
               !ops[2].is_lval)
      {
        int64_t dlt = irop_get_imm64_ex(ir, ops[2]);
        c->so_tfa[dpos] = a + (int32_t)(q->op == TCCIR_OP_ADD ? dlt : -dlt);
      }
      else if (n == 1 && q->op == TCCIR_OP_FUNCPARAMVAL)
      {
        int64_t enc = irop_get_imm64_ex(ir, ops[2]);
        int call_id = TCCIR_DECODE_CALL_ID(enc);
        IRLtCall *call = call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
        if (TCCIR_DECODE_PARAM_IDX(enc) == 0 && call && call->implicit && call_id < c->nso_calls)
        {
          /* the hidden result pointer: the call writes the whole object */
          if (a != ir->lt_sobjs[k].off || (c->so_sret[call_id] >= 0 && c->so_sret[call_id] != k))
            c->so_untracked[k] = 1;
          else
            c->so_sret[call_id] = k;
        }
      }
      else
        c->so_untracked[k] = 1;
    }
  }
  tcc_free(ndef);
  /* an inlined call's result: what the body did with it is overridden by
     what the call's type says, at its end */
  for (int e = 0; e < ir->lt_nb_sres; e++)
    for (int k = 0; k < c->nsobj; k++)
      if (ir->lt_sobjs[k].off == ir->lt_sres[7 * e + 1])
        c->so_untracked[k] = 0;
}

/* ---- restrict parameters (S3) -------------------------------------------
 *
 * A call with `T * restrict view` parameters: no other argument may point
 * into the object a restrict argument points into.  An argument's object: the
 * variable whose pointer value (or a pointer into whose memory) it is -- for a
 * pointer that owns nothing, the owner it borrows from -- or the local frame
 * object whose address it is, through copies and offsets in its block.
 * Checked in the reporting walk; 0 when not known. */
static int lt_arg_source(LtCtx *c, int i, IROperand op)
{
  TCCIRState *ir = c->ir;
  int v = irop_is_none(op) ? -1 : lt_pointer_var(c, op), from = c->cfg->blocks[c->cfg->instr_to_block[i]].start_idx;
  if (v >= 0 && v < c->nvars)
  {
    if (!lt_owned(c, v) && c->borrow[v] >= 0)
      v = c->borrow[v];
    return 1 + v;
  }
  for (int d = 0, at = i; d < 6 && !irop_is_none(op); d++)
  {
    int32_t vr = irop_get_vreg(op);
    if (lt_is_addr(op) && vr < 0)
    {
      int32_t off = (int32_t)irop_get_imm64_ex(ir, op);
      for (int k = 0; k < ir->frame_obj_count; k++)
      {
        const int32_t *o = &ir->frame_objs[FRAME_OBJ_WORDS * k];
        if (o[0] <= off && off < o[0] + (o[1] > 0 ? o[1] : 1))
          return -2 - k;
      }
      return 0;
    }
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    int j = ix_def(ir, from, at, vr);
    if (j < 0)
      return 0;
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      return 0;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    /* the pointer side of an addition */
    op = (q->op == TCCIR_OP_ADD && !irop_is_none(s2) && lt_is_addr(s2) && !lt_is_addr(s1)) ? s2 : s1;
    at = j;
  }
  return 0;
}

static const char *lt_source_name(LtCtx *c, int key, char *buf, int size)
{
  if (key > 0)
    return lt_name(c, key - 1);
  if (key <= -2)
  {
    const int32_t *o = &c->ir->frame_objs[FRAME_OBJ_WORDS * (-2 - key)];
    for (int k = 0; k < c->ir->lt_nb_fnames; k++)
      if (c->ir->lt_fnames[2 * k] == o[0])
      {
        snprintf(buf, size, "%s", get_tok_str(c->ir->lt_fnames[2 * k + 1], NULL));
        return buf;
      }
  }
  return "the same object";
}

/* the call's arguments recorded: a restrict one sharing its object with
   another is reported */
static void lt_restrict_call(LtCtx *c, int call_id, IRLtCall *call, int line)
{
  for (int a = 0; a < c->nrarg; a++)
  {
    int32_t *ra = &c->rarg[4 * a];
    if (ra[0] != call_id || ra[2] == 0 || !((call->restricts >> ra[1]) & 1))
      continue;
    for (int b = 0; b < c->nrarg; b++)
    {
      int32_t *rb = &c->rarg[4 * b];
      if (b == a || rb[0] != call_id || rb[2] != ra[2] || (((call->restricts >> rb[1]) & 1) && b < a))
        continue;
      char nb[64];
      const char *what = lt_source_name(c, ra[2], nb, sizeof nb);
      tcc_warning_c(warn_lifetime)("%i:argument %d of '%s' is restrict, but argument %d points into %s%s%s too", line,
                                   ra[1] + 1, call->callee ? get_tok_str(call->callee, NULL) : "the function", rb[1] + 1,
                                   ra[2] != 0 && strcmp(what, "the same object") ? "'" : "", what,
                                   ra[2] != 0 && strcmp(what, "the same object") ? "'" : "");
      break;
    }
  }
  /* this call's entries are done */
  int n = 0;
  for (int a = 0; a < c->nrarg; a++)
    if (c->rarg[4 * a] != call_id)
      memmove(&c->rarg[4 * n++], &c->rarg[4 * a], 4 * sizeof(int32_t));
  c->nrarg = n;
}

static void lt_instr(LtCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  const IRRegistersConfig *cfg = &irop_config[q->op];
  const int line = q->line_num;
  IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
  IROperand src1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
  IROperand src2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
  int defines = !irop_is_none(dest) && irop_dest_defines_vreg(dest);
  int moved = -1; /* src1's variable, when this instruction moves it */
  int v;

  c->cur_inl = c->inl ? c->inl[i] : 0;
  /* an inlined body starts: what a call would have changed */
  if (c->cur_inl && (i == 0 || !c->inl[i - 1]) && c->npaths)
    lt_forget_globals(c);
  if (c->nsobj)
  {
    if (c->sres_first[i] != c->sres_first[i + 1])
      lt_sres_at(c, i);
    lt_sobj_instr(c, i, q, dest, src1, src2, defines);
  }

  /* memory through a maybe pointer; an out parameter's target read or written */
  if ((v = lt_deref_var(c, src1)) >= 0)
    lt_deref(c, v, lt_deref_line(c, src1, line)), lt_out_read(c, v, line);
  if ((v = lt_deref_var(c, src2)) >= 0)
    lt_deref(c, v, lt_deref_line(c, src2, line)), lt_out_read(c, v, line);
  if (!defines && (v = lt_deref_var(c, dest)) >= 0)
    lt_deref(c, v, lt_deref_line(c, dest, line)), lt_out_write(c, v);
  lt_deref_value(c, src1, line);
  lt_deref_value(c, src2, line);
  if (!defines)
    lt_deref_value(c, dest, line);
  /* an out parameter's pointer passed on, stored or returned: whoever gets it
     may write the target (an `out` parameter of the callee certainly does) */
  if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_RETURNVALUE ||
      ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && !irop_is_none(dest) &&
       TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) != TCCIR_VREG_TYPE_TEMP))
    if (irop_is_vreg_value(src1) || irop_get_tag(src1) == IROP_TAG_VREG)
      if (!src1.is_lval)
        lt_out_write(c, lt_pointer_var(c, src1));

  /* getline(&line, ...): the callee may replace the owned pointer in line
     (it is still line's after the call); out: it writes one, whatever was
     there before is lost */
  if (q->op == TCCIR_OP_FUNCPARAMVAL && c->tout)
  {
    int32_t sv = irop_is_none(src1) ? -1 : irop_get_vreg(src1);
    int sp = sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP ? TCCIR_DECODE_VREG_POSITION(sv) : -1;
    int ov = sp >= 0 && sp < ir->next_temporary_variable ? c->tout[sp] : -1;
    if (ov >= 0)
    {
      int64_t enc = irop_get_imm64_ex(ir, src2);
      int call_id = TCCIR_DECODE_CALL_ID(enc), k = TCCIR_DECODE_PARAM_IDX(enc);
      IRLtCall *call = call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
      if (call && call->known && (k -= call->implicit) >= 0 && k < 32 && ((call->tborrow >> k) & 1))
      {
        /* out T * borrows(s) * view: written at the call (lt_out_borrows) */
        if (call_id < c->ncall_bsrc)
          c->call_tvar[call_id] = c->call_tvar[call_id] == -1 || c->call_tvar[call_id] == ov ? ov : -2;
      }
      else if (call && call->known && k >= 0 && k < 32 && !((call->outown >> k) & 1))
        lt_use(c, ov, line); /* a parameter that only reads: the callee reads v */
      else if (call && call->known && k >= 0 && k < 32 && lt_owned(c, ov))
      {
        uint8_t st = c->cur[ov] & LT_OWNBITS;
        if ((call->outown_out >> k) & 1)
          lt_leak(c, ov, line, "when it is written through its address");
        else if ((st & LT_MOVED) && !lt_silent(c, ov))
        {
          tcc_warning_c(warn_lifetime)("%i:'%s' %s used after it was %s", line, lt_name(c, ov),
                                       st == LT_MOVED ? "is" : "may be", c->move_drop[ov] ? lt_freed(c, ov) : "moved");
          lt_note_move(c, ov);
        }
        c->cur[ov] = (c->cur[ov] & ~(LT_OWNBITS | LT_NULLBITS | LT_WBITS)) | LT_OWN |
                     (((call->outown_maybe >> k) & 1) ? LT_N_NULL | LT_N_SET : LT_N_SET);
        lt_forget(c, ov, 0);
        /* out, the result a status (posix_memalign, getaddrinfo): written only
           on success -- the result's test decides (0 / >= 0: success) */
        if (((call->outown_out >> k) & 1) && !call->ret_maybe)
        {
          c->cur[ov] |= LT_NULL;
          c->out_call[ov] = call_id;
        }
      }
    }
  }
  if (q->op == TCCIR_OP_FUNCPARAMVAL && c->call_bsrc)
  {
    /* an owned variable passed where the result borrows from */
    int64_t enc = irop_get_imm64_ex(ir, src2);
    int call_id = TCCIR_DECODE_CALL_ID(enc), k = TCCIR_DECODE_PARAM_IDX(enc), o;
    IRLtCall *call = call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
    if (call && call->borrows && call_id < c->ncall_bsrc && (k -= call->implicit) >= 0 && k < 32 &&
        ((call->borrows >> k) & 1) && !lt_is_addr(src1) && irop_get_vreg(src1) >= 0 &&
        (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src1)) != TCCIR_VREG_TYPE_TEMP || irop_is_vreg_value(src1)) &&
        ((o = lt_pointer_var(c, src1)) >= 0 || (o = lt_call_result_owner(c, src1)) >= 0) && o < c->nvars &&
        lt_owned(c, o))
      c->call_bsrc[call_id] = c->call_bsrc[call_id] == -1 || c->call_bsrc[call_id] == o ? o : -2;
    /* ... where an out parameter's target borrows from */
    k = TCCIR_DECODE_PARAM_IDX(enc) - (call ? call->implicit : 0);
    if (call && call->tbsrc && call_id < c->ncall_bsrc && k >= 0 && k < 32 && ((call->tbsrc >> k) & 1) &&
        !lt_is_addr(src1) && irop_get_vreg(src1) >= 0 &&
        (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src1)) != TCCIR_VREG_TYPE_TEMP || irop_is_vreg_value(src1)) &&
        ((o = lt_pointer_var(c, src1)) >= 0 || (o = lt_call_result_owner(c, src1)) >= 0) && o < c->nvars &&
        lt_owned(c, o))
      c->call_tsrc[call_id] = c->call_tsrc[call_id] == -1 || c->call_tsrc[call_id] == o ? o : -2;
  }
  if (q->op == TCCIR_OP_FUNCPARAMVAL && c->report)
  {
    /* a value that may be null for a parameter that may not be */
    int64_t enc = irop_get_imm64_ex(ir, src2);
    int call_id = TCCIR_DECODE_CALL_ID(enc), k = TCCIR_DECODE_PARAM_IDX(enc);
    IRLtCall *call = call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
    if (call && !call->in_inline && (k -= call->implicit) >= 0 && k < 32 && (call->nonnull & (1u << k)))
    {
      int handle = (call->handles >> k) & 1;
      int st = handle ? lt_handle_null(c, src1) : lt_value_null(c, src1);
      if ((st & LT_N_NULL) && handle && call->callee)
        tcc_warning_c(warn_lifetime)("%i:%s is passed as argument %d of '%s', which is not maybe", line,
                                     st == LT_N_NULL ? "an invalid handle" : "a handle that may be invalid", k + 1,
                                     get_tok_str(call->callee, NULL));
      else if ((call->nonnull_attr >> k) & 1)
      {
        /* a null constant is -Wnonnull's */
        if ((st & LT_N_NULL) && !(irop_is_immediate(src1) && !src1.is_lval))
        {
          if (call->callee)
            tcc_warning_c(warn_lifetime)("%i:%s is passed as argument %d of '%s', which is declared nonnull", line,
                                         lt_null_what(st), k + 1, get_tok_str(call->callee, NULL));
          else
            tcc_warning_c(warn_lifetime)("%i:%s is passed as argument %d, which is declared nonnull", line,
                                         lt_null_what(st), k + 1);
        }
      }
      else if (st & LT_N_NULL)
        lt_null_arg(c, st, k, call->callee, line);
    }
  }
  if (q->op == TCCIR_OP_FUNCPARAMVAL && c->npaths)
  {
    /* an argument the callee may write through: its maybe members are
       forgotten -- at the call, which comes after the other arguments (they
       may read the members first) */
    int64_t enc = irop_get_imm64_ex(ir, src2);
    int call_id = TCCIR_DECODE_CALL_ID(enc), k = TCCIR_DECODE_PARAM_IDX(enc), self, t;
    IRLtCall *call = call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
    if (call)
      k -= call->implicit;
    lt_arg_galias(ir, c->cfg, i, call, k, src1);
    int keep = call && call->noted && k >= 0 && k < call->nparams && ((call->keep >> k) & 1);
    int dropm = call && call->noted && k >= 0 && k < 32 && ((call->dropmem >> k) & 1);
    if (call && call->noted && !call->in_inline && k >= 0 && k < 32 && (((call->vmem | call->dropmem) >> k) & 1) &&
        c->report && (t = lt_arg_target(c, src1, &self)) != -1)
      lt_members_handed(c, t, call, line);
    if (!keep && (t = lt_arg_target(c, src1, &self)) != -1)
    {
      self |= dropm ? 2 : 0;
      int at = call_id < c->ncall_at ? c->call_at[call_id] : -1;
      if (at >= 0 && c->cfg->instr_to_block[at] == c->cfg->instr_to_block[i] && at > i)
      {
        if (c->npend + 1 > c->pendcap)
        {
          c->pendcap = c->pendcap ? 2 * c->pendcap : 16;
          c->pend = tcc_realloc(c->pend, c->pendcap * 3 * sizeof(int32_t));
        }
        c->pend[3 * c->npend] = call_id;
        c->pend[3 * c->npend + 1] = t;
        c->pend[3 * c->npend + 2] = self;
        c->npend++;
      }
      else
        lt_forget_call(c, t, self, line); /* the call is in another block */
    }
  }
  /* restrict parameters: each argument's object (reporting walk) */
  if (c->report && q->op == TCCIR_OP_FUNCPARAMVAL)
  {
    int64_t enc = irop_get_imm64_ex(ir, src2);
    int call_id = TCCIR_DECODE_CALL_ID(enc), k = TCCIR_DECODE_PARAM_IDX(enc);
    IRLtCall *call = call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
    if (call && call->restricts && !call->in_inline && (k -= call->implicit) >= 0 && k < 32)
    {
      if (c->nrarg == c->rargcap)
        c->rargcap = c->rargcap ? 2 * c->rargcap : 16, c->rarg = tcc_realloc(c->rarg, c->rargcap * 4 * sizeof(int32_t));
      int32_t *ra = &c->rarg[4 * c->nrarg++];
      ra[0] = call_id, ra[1] = k, ra[2] = lt_arg_source(c, i, src1), ra[3] = line;
    }
  }
  if (c->report && c->nrarg && (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID) && cfg->has_src2)
  {
    int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
    if (call_id >= 0 && call_id < ir->lt_nb_calls && ir->lt_calls[call_id].restricts)
      lt_restrict_call(c, call_id, &ir->lt_calls[call_id], line);
  }
  /* a call may change any global's members (not those a libc or leaf callee cannot reach) */
  if ((q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID) && c->npaths)
  {
    int call_id = cfg->has_src2 ? TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2)) : -1;
    lt_forget_globals_call(c, call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL);
  }
  if ((q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID) && c->npend && cfg->has_src2)
  {
    int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
    for (int k = 0; k < c->npend; k++)
      if (c->pend[3 * k] == call_id)
      {
        lt_forget_call(c, c->pend[3 * k + 1], c->pend[3 * k + 2], line);
        c->pend[3 * k] = -1;
      }
  }
  /* `out T * borrows(s) * view p`: the variable whose address went there
     points into what was passed for s from here on */
  if ((q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID) && cfg->has_src2 && c->call_tvar)
  {
    int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
    if (call_id >= 0 && call_id < c->ncall_bsrc && c->call_tvar[call_id] >= 0)
      lt_out_borrows(c, c->call_tvar[call_id], c->call_tsrc[call_id]);
  }
  /* move(p) passed to a regular-C parameter: p moves into the callee */
  if (q->op == TCCIR_OP_FUNCPARAMVAL && ir->lt_nb_mtemps && lt_moved_temp(ir, src1) &&
      (v = lt_read_value(c, src1)) >= 0 && lt_owned(c, v))
  {
    lt_move(c, v, 0, line);
    moved = v;
  }
  else if (q->op == TCCIR_OP_FUNCPARAMVAL && (v = lt_read_value(c, src1)) >= 0 && lt_owned(c, v))
  {
    int64_t enc = irop_get_imm64_ex(ir, src2);
    int call_id = TCCIR_DECODE_CALL_ID(enc), k = TCCIR_DECODE_PARAM_IDX(enc);
    IRLtCall *call = call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
    if (call && call->known)
    {
      k -= call->implicit;
      if (k >= 0 && k < 32 && (call->owned & (1u << k)))
      {
        uint8_t was = c->cur[v] & LT_OWNBITS;
        lt_move(c, v, (call->drop >> k) & 1, line);
        moved = v;
        /* takes(p), the result maybe: p is the caller's again if it is null */
        if ((call->taken & (1u << k)) && call->ret_maybe && (was & LT_OWN))
        {
          c->cur[v] |= LT_OWN;
          c->take_call[v] = call_id;
        }
      }
    }
  }
  else if (q->op == TCCIR_OP_RETURNVALUE)
  {
    int st = c->ret_plain_ptr ? lt_value_null(c, src1) : LT_N_SET;
    if ((st & LT_N_NULL) && c->report)
      tcc_warning_c(warn_lifetime)("%i:%s is returned from '%s', whose result is not maybe", line, lt_null_what(st),
                                   funcname);
    if (c->ret_owned && (v = lt_read_value(c, src1)) >= 0 && lt_owned(c, v))
    {
      lt_move(c, v, 0, line);
      moved = v;
    }
  }

  /* move(p) stored in a regular-C place: p moves there */
  if (moved < 0 && q->op == TCCIR_OP_STORE && !defines && ir->lt_nb_mtemps && lt_moved_temp(ir, src1) &&
      (v = lt_read_value(c, src1)) >= 0 && lt_owned(c, v))
  {
    lt_move(c, v, 0, line);
    moved = v;
  }

  /* a store into owned memory moves the owned variable stored */
  if (moved < 0 && q->op == TCCIR_OP_STORE && !defines && !src1.is_lval && (v = lt_read_var(c, src1)) >= 0 &&
      lt_owned(c, v) && lt_owned_store(ir, i))
  {
    lt_move(c, v, 0, line);
    moved = v;
  }

  /* reads: the sources, and a destination that is memory through a pointer */
  if (moved < 0 && (v = lt_read_value(c, src1)) >= 0)
    lt_use(c, v, line);
  if ((v = lt_read_value(c, src2)) >= 0)
    lt_use(c, v, line);
  if (!irop_is_none(dest) && !defines && (v = lt_read_var(c, dest)) >= 0)
    lt_use(c, v, line);

  /* a store into a maybe member through its base sets it from the value;
     any other write there forgets it */
  int direct = !irop_is_none(dest) && irop_get_tag(dest) != IROP_TAG_VREG && dest.is_lval && c->nobj;
  if ((!defines || direct) && c->npaths)
  {
    int k = lt_deref_path(c, dest), db, doff;
    if (k >= 0 && q->op == TCCIR_OP_STORE)
    {
      int st = lt_value_null(c, src1);
      lt_forget(c, k, 0);
      lt_unlink(c, k);
      lt_forget_aliases(c, k);
      c->cur[k] = (c->cur[k] & ~(LT_NULLBITS | LT_OWNBITS)) | LT_UNINIT | st;
    }
    else if (k >= 0 || (k = lt_store_hits_path(c, dest)) >= 0)
    {
      lt_forget(c, k, 1);
      lt_forget_aliases(c, k);
    }
    else if (direct && (db = lt_direct_base(c, dest, &doff)) != -1)
    {
      /* a struct's other member, or all of it: the paths it covers */
      int sz = irop_get_btype(dest) == IROP_BTYPE_STRUCT ? INT32_MAX
               : irop_get_btype(dest) == IROP_BTYPE_INT64 || irop_get_btype(dest) == IROP_BTYPE_FLOAT64 ? 8 : 4;
      for (int p = 0; p < c->npaths; p++)
        if (c->pbase[p] == db && c->pidx[p] < 0 && c->poff[p] >= doff && c->poff[p] - doff < sz)
        {
          lt_forget(c, c->nvars + p, 1);
          lt_forget_aliases(c, c->nvars + p);
        }
    }
  }
  /* a global pointer assigned: its members are not the ones tested */
  if (!irop_is_none(dest) && irop_get_tag(dest) == IROP_TAG_SYMREF && dest.is_lval && c->nobj)
  {
    IRPoolSymref *r = irop_get_symref_ex(ir, dest);
    for (int j = 0; r && r->sym && j < c->nobj; j++)
      if (c->okind[j] == LT_OK_GPTR && c->osym[j] == r->sym)
        lt_forget(c, LT_OBJ(j), 0);
  }

  if (!defines)
    return;
  int32_t vreg = irop_get_vreg(dest);
  /* an index variable written: its elements' members are other elements' */
  if (c->nixpaths && TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP)
    for (int k = 0; k < c->npaths; k++)
      if (c->pidx[k] == vreg)
        lt_forget(c, c->nvars + k, 1);
  /* a predicate's local assigned: a constant decides it, anything else not */
  for (int k = 0; k < c->npred; k++)
    if (c->pr_vreg[k] == vreg)
    {
      int known = (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) && irop_is_immediate(src1) && !src1.is_lval;
      int64_t val = known ? irop_get_imm64_ex(ir, src1) : 0;
      int bt = irop_get_btype(dest);
      if (known && (bt == IROP_BTYPE_INT8 || bt == IROP_BTYPE_INT16) && (val < 0 || val >= (bt == IROP_BTYPE_INT8 ? 128 : 32768)))
        known = 0;
      if (known && bt == IROP_BTYPE_INT32 && val != (int32_t)val)
        known = 0;
      if (known)
      {
        c->pk |= 1u << k;
        c->pv = (c->pv & ~(1u << k)) | ((uint32_t)(val == c->pr_k[k]) << k);
      }
      else
        c->pk &= ~(1u << k), c->pv &= ~(1u << k);
    }
  /* a STORE into a variable's home is an assignment too (the inliner binds
     arguments to parameters that way) */
  /* ZEXT too: the same value widened -- a pointer becoming an error union's
     payload (`return p;` in a `!T *` function) */
  int is_copy = q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ZEXT;
  int copy = is_copy ? lt_read_value(c, src1) : -1;
  int null_in = LT_N_SET;
  if (is_copy)
    null_in = lt_value_null(c, src1);
  else if (q->op == TCCIR_OP_FUNCCALLVAL)
  {
    int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
    if (call_id < ir->lt_nb_calls && ir->lt_calls[call_id].ret_maybe)
      null_in = LT_N_NULL | LT_N_SET;
  }
  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vreg);
    if (pos < c->nalias)
    {
      /* the call whose result it holds (takes(p)), through copies too */
      int32_t s1v = irop_is_none(src1) ? -1 : irop_get_vreg(src1);
      int s1p = s1v >= 0 && TCCIR_DECODE_VREG_TYPE(s1v) == TCCIR_VREG_TYPE_TEMP ? TCCIR_DECODE_VREG_POSITION(s1v) : -1;
      if (q->op == TCCIR_OP_FUNCCALLVAL)
        c->tcall[pos] = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2)), c->tcall_gen[pos] = c->gen;
      else if (is_copy && s1p >= 0 && irop_is_vreg_value(src1) && lt_tcall(c, s1p) >= 0)
        c->tcall[pos] = c->tcall[s1p], c->tcall_gen[pos] = c->gen;
      else
        c->tcall_gen[pos] = 0;
      int into = is_copy ? lt_pointer_var(c, src1) : -1;
      if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
        into = lt_pointer_var(c, src1) >= 0 ? lt_pointer_var(c, src1) : lt_pointer_var(c, src2);
      if (is_copy && irop_get_tag(src1) == IROP_TAG_VREG && src1.is_lval)
        into = -1; /* loaded through the pointer, not a copy of it */
      /* maybe members: the pointer value it holds (a base plus a constant),
         a maybe member's value loaded (a copy of the path), or the address
         of one the frontend recorded (the path itself) */
      int tb = -1, to = 0, lp = is_copy ? lt_deref_path(c, src1) : -1, ta = -1;
      if (lp >= 0)
        copy = into = tb = lp;
      else if (is_copy)
        tb = lt_addr_base(c, src1, &to);
      else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_is_immediate(src2) && !src2.is_lval &&
               (tb = lt_addr_base(c, src1, &to)) != -1)
      {
        int64_t d = irop_get_imm64_ex(ir, src2);
        to += (int)(q->op == TCCIR_OP_ADD ? d : -d);
      }
      /* &s, &g; gp's value (a global pointer loaded) */
      if (tb == -1 && lp < 0 && (is_copy || q->op == TCCIR_OP_LEA) && c->pcap)
      {
        tb = lt_obj_addr(c, q, src1, &to);
        if (tb == -1 && is_copy && irop_get_tag(src1) == IROP_TAG_SYMREF && src1.is_lval && c->trec)
        {
          IRPoolSymref *r = irop_get_symref_ex(ir, src1);
          if (r && r->sym && !r->addend && (r->sym->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR)
            tb = lt_gobj(c, LT_OK_GPTR, r->sym, 0), to = 0;
        }
      }
      int idx;
      if (tb != -1 && c->trec && c->trec[pos] >= 0 && lt_member_at(c, tb, to, &ir->lt_members[c->trec[pos]], &idx))
        ta = lt_path(c, tb, to, ir->lt_members[c->trec[pos]].tok, ir->lt_members[c->trec[pos]].owned, idx);
      /* an element at a variable index: &a + i * scale (+ a constant) */
      if (c->xidx)
      {
        int32_t xb = -1, xo = 0, xi = -1, xs = 0, xa = i;
        if (q->op == TCCIR_OP_ADD && tb == -1 && lp < 0 && !irop_is_immediate(src2))
        {
          int bs = c->cfg->blocks[c->cfg->instr_to_block[i]].start_idx;
          if ((xb = lt_addr_base(c, src1, &xo)) == -1 && (xb = lt_obj_addr(c, q, src1, &xo)) == -1 &&
              !irop_is_none(src1) && irop_get_tag(src1) == IROP_TAG_SYMREF && !src1.is_lval && !c->cur_inl)
          {
            /* a global array of structs, whether or not a constant index named it */
            IRPoolSymref *r = irop_get_symref_ex(ir, src1);
            int es;
            if (r && r->sym && lt_struct_or_array(&r->sym->type, &es) && es)
              xb = lt_gobj(c, LT_OK_GLOBAL, r->sym, 0), xo = r->addend;
          }
          if (xb == -1 || (xi = ix_scaled(ir, bs, i, src2, &xs)) < 0)
            xb = -1, xi = -1;
        }
        else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_ASSIGN) && s1p >= 0 &&
                 irop_is_vreg_value(src1) && !src1.is_lval && lt_tpos_valid(c, s1p) && c->xidx[s1p] >= 0 &&
                 (q->op == TCCIR_OP_ASSIGN || (irop_is_immediate(src2) && !src2.is_lval)))
        {
          int64_t d = q->op == TCCIR_OP_ASSIGN ? 0 : irop_get_imm64_ex(ir, src2);
          xb = c->xbase[s1p], xi = c->xidx[s1p], xs = c->xsc[s1p], xa = c->xat[s1p];
          xo = c->xoff[s1p] + (int)(q->op == TCCIR_OP_SUB ? -d : d);
        }
        c->xbase[pos] = xb, c->xoff[pos] = xo, c->xidx[pos] = xi, c->xsc[pos] = xs, c->xat[pos] = xa;
        if (ta == -1 && xi >= 0 && c->trec && c->trec[pos] >= 0 && !ix_written(ir, xa + 1, i, xi))
        {
          const IRLtMember *rec = &ir->lt_members[c->trec[pos]];
          int j = xb <= -2 ? -2 - xb : -1;
          int es = j >= 0 && c->okind[j] != LT_OK_GPTR ? c->oesize[j] : rec->offset == rec->foff ? rec->esize : 0;
          if (es > 0 && xs == es && xo == rec->foff)
            ta = lt_path_ix(c, xb, xo, xi, rec->tok, rec->owned);
        }
      }
      c->tbase[pos] = tb;
      c->toff[pos] = to;
      c->taddr[pos] = ta;
      /* an owned member reached through a call's result has no path (a new
         value per call): said, not followed silently */
      if (ta == -1 && tb == -1 && c->report && c->trec && c->trec[pos] >= 0 && (ir->lt_members[c->trec[pos]].owned & LT_MEMBER_OWNED) &&
          !c->cur_inl && (q->op == TCCIR_OP_FUNCCALLVAL || (s1p >= 0 && lt_tcall(c, s1p) >= 0)))
        tcc_warning_c(warn_lifetime)("%i:'%s' of a call's result is owned and the lifetime check does not follow it: "
                                     "store the result in a variable",
                                     line, get_tok_str(ir->lt_members[c->trec[pos]].tok, NULL));
      /* an offset from a temp's value that may be null (a member's address,
         an element's), or a copy of one */
      c->tderiv[pos] = -1;
      if (is_copy && s1p >= 0 && irop_is_vreg_value(src1) && !src1.is_lval && lt_tpos_valid(c, s1p))
        c->tderiv[pos] = c->tderiv[s1p];
      else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
        for (int k = 0; k < 1 + (q->op == TCCIR_OP_ADD) && c->tderiv[pos] < 0; k++)
        {
          IROperand o = k ? src2 : src1;
          int32_t ov = irop_is_none(o) ? -1 : irop_get_vreg(o);
          int op_ = ov >= 0 && TCCIR_DECODE_VREG_TYPE(ov) == TCCIR_VREG_TYPE_TEMP ? TCCIR_DECODE_VREG_POSITION(ov) : -1;
          if (op_ < 0 || !irop_is_vreg_value(o) || o.is_lval || !lt_tpos_valid(c, op_))
            continue;
          int r = c->tderiv[op_] >= 0 ? c->tderiv[op_] : op_;
          if (lt_tpos_valid(c, r) && (lt_tnull(c, r) & LT_N_NULL))
            c->tderiv[pos] = r;
        }
      c->alias[pos] = copy;
      c->palias[pos] = into;
      c->tnull[pos] = null_in;
      c->alias_gen[pos] = c->gen;
      int g = c->tglob ? c->tglob[pos] : -1;
      if (g >= 0 && (c->gbcnt || c->gbrow))
      {
        /* the owners its definitions point into (the `?:` join) */
        int bo = lt_borrow_from(c, q, src1, src2, is_copy, -1);
        int sg = bo < 0 ? lt_src_glob(c, q, src1, src2, is_copy) : -1;
        if (c->collect && bo >= 0)
          lt_bset_add(c->gbset + g * LT_MAX_BOWN, &c->gbcnt[g], bo, 0);
        else if (c->collect && sg >= 0 && sg != g && (c->gbcnt[sg] & ~LT_BOWN_VIA))
        {
          int n = c->gbcnt[sg] & ~LT_BOWN_VIA;
          if (n > LT_MAX_BOWN)
            c->gbcnt[g] = LT_MAX_BOWN + 1;
          for (int j = 0; j < n && j < LT_MAX_BOWN; j++)
            lt_bset_add(c->gbset + g * LT_MAX_BOWN, &c->gbcnt[g], c->gbset[sg * LT_MAX_BOWN + j], 0);
        }
        if (c->gbrow && c->gbrow[g] >= 0)
          lt_brow_set(c, c->gbrow[g], bo, sg >= 0 && sg != g ? c->gbrow[sg] : -1);
      }
      if (g >= 0)
      {
        int tc = c->tcall_gen[pos] == c->gen ? c->tcall[pos] : -1;
        c->cur[c->gslot + g] = null_in;
        c->tline[pos] = line;
        if (!c->g_known[g])
        {
          c->g_known[g] = 1;
          c->g_line[g] = line;
          c->g_alias[g] = copy, c->g_palias[g] = into, c->g_tbase[g] = tb, c->g_toff[g] = to;
          c->g_taddr[g] = ta, c->g_tcall[g] = tc;
        }
        else
        {
          /* another definition: what they do not agree on is unknown */
          if (c->g_alias[g] != copy)
            c->g_alias[g] = -1;
          if (c->g_palias[g] != into)
            c->g_palias[g] = -1;
          if (c->g_tbase[g] != tb || c->g_toff[g] != to)
            c->g_tbase[g] = -1;
          if (c->g_taddr[g] != ta)
            c->g_taddr[g] = -1;
          if (c->g_tcall[g] != tc)
            c->g_tcall[g] = -1;
        }
      }
    }
    return;
  }
  if (c->vcall && TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_VAR && TCCIR_DECODE_VREG_POSITION(vreg) < c->nvcall)
  {
    int vp = TCCIR_DECODE_VREG_POSITION(vreg), vc = -1;
    int32_t s1v = irop_is_none(src1) ? -1 : irop_get_vreg(src1);
    if (q->op == TCCIR_OP_FUNCCALLVAL)
      vc = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
    else if (is_copy && s1v >= 0 && TCCIR_DECODE_VREG_TYPE(s1v) == TCCIR_VREG_TYPE_TEMP && irop_is_vreg_value(src1))
      vc = lt_tcall(c, TCCIR_DECODE_VREG_POSITION(s1v));
    if (c->vdefs[vp] == 1)
      c->vcall[vp] = vc;
    /* assigned more than once: which call's result it holds on this path */
    if (c->vstat && c->vstat[vp] >= 0 && c->sbase >= 0)
    {
      int row = c->vstat[vp];
      uint8_t bits = LT_STAT_OTHER;
      for (int j = 0; j < LT_STAT_CALLS && vc >= 0; j++)
        if (c->stcalls[row * LT_STAT_CALLS + j] == vc)
          bits = 1u << j;
      c->cur[c->sbase + row] = bits;
    }
    /* `ok = p != 0`: linked to p's test, any other value breaks it */
    if (c->cl_var && c->cl_var[vp] >= 0 && c->clbase >= 0)
    {
      int row = c->cl_var[vp], pol;
      int32_t xv;
      c->cur[c->clbase + row] = lt_setif_test(ir, i, &xv, &pol) && lt_map(c, xv) == c->cl_x[row] && pol == c->cl_pol[row]
                                    ? 0
                                    : LT_N_NULL;
    }
  }
  if ((v = lt_map(c, vreg)) < 0)
    return;
  /* a pointer variable assigned: what a local said about its test is old */
  for (int row = 0; row < c->nclink && c->clbase >= 0; row++)
    if (c->cl_x[row] == v)
      c->cur[c->clbase + row] = LT_N_NULL;
  /* assigned again: a status tested from here on is not about this value */
  if (c->out_call[v] >= 0)
    c->out_call[v] = -1;

  /* takes(p): the variable holding the call's result decides for p */
  if (q->op == TCCIR_OP_FUNCCALLVAL)
  {
    int cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
    for (int p = 0; p < c->nvars; p++)
      if (c->take_call[p] == cid)
        c->taken_by[p] = v;
  }
  else if (is_copy && !irop_is_none(src1) && irop_is_vreg_value(src1))
  {
    int32_t s1v = irop_get_vreg(src1);
    int s1p = s1v >= 0 && TCCIR_DECODE_VREG_TYPE(s1v) == TCCIR_VREG_TYPE_TEMP ? TCCIR_DECODE_VREG_POSITION(s1v) : -1;
    int tc = s1p >= 0 ? lt_tcall(c, s1p) : -1;
    if (tc >= 0)
      for (int p = 0; p < c->nvars; p++)
        if (c->take_call[p] == tc)
          c->taken_by[p] = v;
  }

  /* a copy of a maybe member links v to it */
  if (v < c->nvars && c->link)
  {
    int pk = copy >= c->nvars ? copy : -1;
    if (c->collect && pk >= 0)
      c->link[v] = c->link[v] == -1 || c->link[v] == pk ? pk : -2;
    if (c->lslot[v] >= 0)
      c->cur[c->lslot[v]] = pk >= 0 && pk == c->link[v] ? 0 : LT_N_NULL;
  }

  /* assigned again: its maybe members are not the ones tested.  An inlined
     callee's parameter that may write through the argument forgets the
     argument's, as the call would. */
  lt_forget(c, v, 0);
  {
    /* an inlined method's self: the handing over, as at a call */
    int itok = 0, ik = ir->lt_nb_iself ? lt_iself_of(ir, ir->lt_vars[v].vreg, &itok) : 0;
    if (ik && c->report && !c->arg_bound[v])
    {
      int self, t = lt_arg_target(c, src1, &self);
      IRLtCall fake;
      memset(&fake, 0, sizeof fake);
      fake.callee = itok;
      lt_members_handed(c, t, &fake, line);
    }
    if (ir->lt_vars[v].arg_index >= 0 && (ir->lt_vars[v].kind & LTV_MUT))
    {
      int self, t = lt_arg_target(c, src1, &self);
      lt_forget_call(c, t, self | (ik == 3 ? 2 : 0), line);
    }
  }

  /* an int handle: a constant (-1, ...) owns nothing; invalid when < 0 */
  if (lt_handle(c, v))
    null_in = is_copy ? lt_handle_null(c, src1) : null_in;
  /* nullness: a maybe variable takes the value's; any other must not get null */
  if (lt_maybe(c, v))
    c->cur[v] = (c->cur[v] & ~LT_NULLBITS) | null_in;
  else if ((null_in & LT_N_NULL) && !lt_silent(c, v))
    tcc_warning_c(warn_lifetime)("%i:%s is stored in '%s', which is not maybe", line, lt_null_what(null_in),
                                 lt_name(c, v));
  else if ((null_in & LT_N_NULL) && c->report && ir->lt_vars[v].arg_report && !c->arg_bound[v])
    /* an inlined callee's parameter bound to its argument: the call's check */
    lt_null_arg(c, null_in, ir->lt_vars[v].arg_index, ir->lt_vars[v].arg_callee, line);
  if (c->report && ir->lt_vars[v].arg_index >= 0)
    c->arg_bound[v] = 1;

  if (!lt_owned(c, v))
  {
    /* a pointer that owns nothing: does it borrow from an owned variable now
       (a copy of it, or into what it owns: p + n) */
    int from = lt_borrow_from(c, q, src1, src2, is_copy, v);
    /* more owners than a row holds: said, not followed silently */
    if (c->report && c->vbcnt && v < c->nvars && (c->vbcnt[v] & ~LT_BOWN_VIA) > LT_MAX_BOWN && !lt_silent(c, v) &&
        !c->vbsaid[v] &&
        (from >= 0 || lt_src_glob(c, q, src1, src2, is_copy) >= 0 || lt_src_bvar(c, q, src1, src2, is_copy) >= 0) &&
        (c->vbsaid[v] = 1))
      tcc_warning_c(warn_lifetime)("%i:'%s' may borrow from more than %d owners: the lifetime check does not follow it",
                                   line, lt_name(c, v), LT_MAX_BOWN);
    if (c->collect && from >= 0)
      c->borrow[v] = c->borrow[v] == -1 || c->borrow[v] == from ? from : -2;
    /* an inlined call's parameter bound to its argument (its first value) */
    if (c->collect && c->bind_owner && v < c->nvars && c->bind_owner[v] == -3 && ir->lt_vars[v].arg_index >= 0)
      c->bind_owner[v] = from;
    if (c->collect && c->vbcnt && v < c->nvars)
    {
      /* the owners it may borrow from: directly, or through a global temp */
      int sg = from < 0 ? lt_src_glob(c, q, src1, src2, is_copy) : -1;
      int sv = from < 0 && sg < 0 ? lt_src_bvar(c, q, src1, src2, is_copy) : -1;
      if (from >= 0)
        lt_bset_add(c->vbset + v * LT_MAX_BOWN, &c->vbcnt[v], from, 0);
      else if (sv >= 0 && sv != v)
      {
        /* a copy of a pointer borrowing from several owners: all of them
           (too many there: too many here) */
        int n = c->vbcnt[sv] & ~LT_BOWN_VIA;
        if (n > LT_MAX_BOWN)
          c->vbcnt[v] = (c->vbcnt[v] & LT_BOWN_VIA) | (LT_MAX_BOWN + 1);
        for (int j = 0; j < n && j < LT_MAX_BOWN; j++)
          lt_bset_add(c->vbset + v * LT_MAX_BOWN, &c->vbcnt[v], c->vbset[sv * LT_MAX_BOWN + j], 0);
        c->borrow[v] = -2;
      }
      else if (sg >= 0 && (c->gbcnt[sg] & ~LT_BOWN_VIA))
      {
        int n = c->gbcnt[sg] & ~LT_BOWN_VIA;
        if (n > LT_MAX_BOWN)
          c->vbcnt[v] = LT_BOWN_VIA | (LT_MAX_BOWN + 1);
        for (int j = 0; j < n && j < LT_MAX_BOWN; j++)
          lt_bset_add(c->vbset + v * LT_MAX_BOWN, &c->vbcnt[v], c->gbset[sg * LT_MAX_BOWN + j], 1);
      }
    }
    c->cur[v] = (c->cur[v] & ~LT_OWNBITS) | (from >= 0 && c->borrow[v] == from ? LT_OWN : LT_UNINIT);
    if (c->vbrow && v < c->nvars && c->vbrow[v] >= 0)
      lt_brow_set(c, c->vbrow[v], from, from < 0 ? lt_src_brow(c, q, src1, src2, is_copy) : -1);
    return;
  }
  lt_leak(c, v, line, "when it is assigned again");
  uint8_t nb = c->cur[v] & ~LT_OWNBITS;
  if (copy >= 0 && copy != v && lt_owned(c, copy))
  {
    /* owned = owned: the value moves */
    uint8_t from = c->cur[copy] & LT_OWNBITS;
    c->cur[copy] = (c->cur[copy] & ~LT_OWNBITS) | LT_MOVED;
    c->move_line[copy] = line;
    c->move_drop[copy] = 0;
    lt_owner_gone(c, copy);
    c->cur[v] = nb | ((from & (LT_OWN | LT_NULL)) ? from & (LT_OWN | LT_NULL) : LT_UNINIT);
  }
  else if (null_in == LT_N_NULL || (lt_handle(c, v) && is_copy && irop_is_immediate(src1) && !src1.is_lval))
    c->cur[v] = nb | LT_NULL;
  else
    c->cur[v] = nb | LT_OWN;
}

/* Leaving the function in state c->cur: what still owns memory leaks, and an
   out parameter's target must have been written. */
static void lt_exit(LtCtx *c, int at_end, int line)
{
  for (int v = 0; v < c->nv; v++)
  {
    uint8_t w = c->cur[v] & LT_WBITS;
    if (c->ir->lt_vars[v].untracked)
      continue;
    lt_leak(c, v, line, at_end ? "at the end of the function" : "at return");
    if (lt_out(c, v) && (w & LT_W_NO) && !lt_silent(c, v))
      tcc_warning_c(warn_lifetime)("%i:'*%s' %s written when '%s' returns", line, lt_name(c, v),
                                   w == LT_W_NO ? "is not" : "may not be", funcname);
  }
  /* a `struct T * view` parameter's owned members (the method self rules):
     none may be left freed -- the caller's struct still owns them; `drop`:
     every one must be released (or null) */
  for (int k = 0; k < c->npaths && c->ir->lt_nb_selfp; k++)
  {
    int v = c->nvars + k, b = c->pbase[k], drop = -1;
    if (b < 0 || b >= c->nvars || c->pidx[k] >= 0 || !lt_owned(c, v) || lt_silent(c, v) ||
        lt_tagged_tok(c->ir->lt_vars[v].tok))
      continue;
    for (int s = 0; s < c->ir->lt_nb_selfp; s++)
      if (c->ir->lt_selfp[2 * s] == c->ir->lt_vars[b].vreg)
        drop = c->ir->lt_selfp[2 * s + 1];
    if (drop < 0)
      continue;
    int ro = drop >> 1;
    drop &= 1;
    uint8_t st = c->cur[v] & LT_OWNBITS;
    char bn[128];
    pstrcpy(bn, sizeof bn, lt_name(c, b));
    /* moved out but null there (`n = move(q->next); if (!n) ...`): owns nothing */
    int null = lt_maybe(c, v) && (c->cur[v] & LT_NULLBITS) == LT_N_NULL;
    if (!drop && (st & LT_MOVED) && !null && ro)
    {
      /* through a read-only parameter: nothing can give it a new value */
      tcc_warning_c(warn_lifetime)("%i:'%s' %s %s when '%s' returns: '%s' still owns it, and its target is "
                                   "read-only -- say inout to give it a new value%s",
                                   line, lt_name(c, v), st == LT_MOVED ? "is" : "may be",
                                   c->move_drop[v] ? lt_freed(c, v) : "moved", funcname, bn,
                                   lt_maybe(c, v) ? " or null" : "");
      lt_note_move(c, v);
    }
    else if (!drop && (st & LT_MOVED) && !null)
    {
      tcc_warning_c(warn_lifetime)("%i:'%s' %s %s when '%s' returns: '%s' still owns it -- give it a new value%s",
                                   line, lt_name(c, v), st == LT_MOVED ? "is" : "may be",
                                   c->move_drop[v] ? lt_freed(c, v) : "moved", funcname, bn,
                                   lt_maybe(c, v) ? " or null" : "");
      lt_note_move(c, v);
    }
    else if (drop && (st & (LT_UNINIT | LT_OWN)) && !null)
      tcc_warning_c(warn_lifetime)("%i:'%s' %s %s when '%s' returns, which drops '%s'", line, lt_name(c, v),
                                   (st & (LT_MOVED | LT_NULL)) ? "may not be" : "is not", lt_freed(c, v), funcname, bn);
  }
}

static void lt_entry_state(LtCtx *c, uint8_t *in)
{
  /* a maybe member nothing has tested may be null; a global temp holds
     nothing yet */
  for (int v = c->nvars; v < c->nv; v++)
    in[v] = v >= c->gslot ? 0 : LT_UNINIT | LT_N_NULL | LT_N_SET;
  for (int row = 0; row < c->nclink && c->clbase >= 0; row++)
    in[c->clbase + row] = LT_N_NULL;
  for (int v = 0; v < c->nvars; v++)
  {
    int param = TCCIR_DECODE_VREG_TYPE(c->ir->lt_vars[v].vreg) == TCCIR_VREG_TYPE_PARAM;
    in[v] = (param && lt_owned(c, v) ? LT_OWN : LT_UNINIT) |
            (param && lt_maybe(c, v) ? LT_N_NULL | LT_N_SET : LT_N_SET) | (param && lt_out(c, v) ? LT_W_NO : 0);
  }
}

/* The predicate a block's last jump tests, its value on the taken edge in
   *taken_val; -1 none */
static int lt_pred_test(LtCtx *c, int last, int *taken_val)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *lq = &ir->compact_instructions[last], *tq;
  if (!c->npred || lq->op != TCCIR_OP_JUMPIF || last == 0)
    return -1;
  tq = &ir->compact_instructions[last - 1];
  if (tq->op != TCCIR_OP_TEST_ZERO && tq->op != TCCIR_OP_CMP)
    return -1;
  int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, lq));
  IROperand a = tcc_ir_op_get_src1(ir, tq);
  int64_t k = 0;
  if (cond != TOK_EQ && cond != TOK_NE)
    return -1;
  if (tq->op == TCCIR_OP_CMP)
  {
    IROperand b = tcc_ir_op_get_src2(ir, tq);
    if (!irop_is_immediate(b) || b.is_lval)
      return -1;
    k = irop_get_imm64_ex(ir, b);
  }
  if (irop_is_none(a) || !irop_is_vreg_value(a) || lt_is_addr(a))
    return -1;
  int32_t vr = irop_get_vreg(a);
  for (int p = 0; p < c->npred; p++)
    if (c->pr_vreg[p] == vr && c->pr_k[p] == k)
    {
      *taken_val = cond == TOK_EQ;
      return p;
    }
  return -1;
}

/* What each edge out of block bb knows, and whether a context can take it:
   a test of a predicate the context knows goes one way only. */
static void lt_edges(LtCtx *c, IRBasicBlock *bb, int last)
{
  IRQuadCompact *lq = &c->ir->compact_instructions[last];
  int tv, p = lt_pred_test(c, last, &tv);
  int target = (int)tcc_ir_op_dest_imm(c->ir, lq);
  int taken_block = p >= 0 && target >= 0 && target < c->n ? c->cfg->instr_to_block[target] : -1;
  /* both edges into one block: nothing to tell apart */
  if (p >= 0 && bb->end_idx < c->n && target >= 0 && target < c->n &&
      c->cfg->instr_to_block[target] == c->cfg->instr_to_block[bb->end_idx])
    p = -1;
  for (int s = 0; s <= bb->num_succs; s++)
  {
    c->eknown[s] = c->pk, c->eval[s] = c->pv, c->efeas[s] = 1;
    if (p < 0)
      continue;
    int taken = s < bb->num_succs ? bb->succs[s] == taken_block : target >= c->n;
    uint32_t val = taken ? tv : !tv;
    if (((c->pk >> p) & 1) && ((c->pv >> p) & 1) != val)
      c->efeas[s] = 0;
    c->eknown[s] |= 1u << p;
    c->eval[s] = (c->eval[s] & ~(1u << p)) | (val << p);
  }
}

/* Walk block b from state `in`, and write what each successor receives in
   out[0..num_succs-1], and what leaves the function in out[num_succs]. */
static void lt_block(LtCtx *c, int b, const uint8_t *in, uint8_t *out)
{
  TCCIRState *ir = c->ir;
  IRBasicBlock *bb = &c->cfg->blocks[b];
  int last = bb->end_idx - 1;
  IRQuadCompact *lq = &ir->compact_instructions[last];

  memcpy(c->cur, in, c->nv);
  c->gen++;
  c->npend = 0;
  for (int i = bb->start_idx; i < bb->end_idx; i++)
  {
    lt_events_at(c, i);
    lt_instr(c, i);
  }
  if (!out)
    out = c->scratch;
  for (int s = 0; s <= bb->num_succs; s++)
    memcpy(out + s * c->nv, c->cur, c->nv);
  lt_edges(c, bb, last);

  /* `if (p)` / `if (p == 0)`: the zero edge owns nothing */
  int target = (lq->op == TCCIR_OP_JUMP || lq->op == TCCIR_OP_JUMPIF) ? (int)tcc_ir_op_dest_imm(ir, lq) : -1;
  int zero_v = -1, zero_taken = 0;
  if (lq->op == TCCIR_OP_JUMPIF && last > bb->start_idx)
  {
    IRQuadCompact *tq = &ir->compact_instructions[last - 1];
    int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, lq));
    IROperand a1 =
        tq->op == TCCIR_OP_TEST_ZERO || tq->op == TCCIR_OP_CMP ? tcc_ir_op_get_src1(ir, tq) : irop_make_none();
    int handle_test = 0;
    if (tq->op == TCCIR_OP_TEST_ZERO)
    {
      zero_v = lt_read_var(c, a1);
      if (zero_v >= 0 && lt_handle(c, zero_v))
        zero_v = -1; /* fd 0 is a valid handle */
    }
    else if (tq->op == TCCIR_OP_CMP)
    {
      IROperand b2 = tcc_ir_op_get_src2(ir, tq);
      int hv = lt_read_var(c, a1);
      /* ... or a handle member tested in place: `if (c->fd >= 0)` */
      if (hv < 0 && irop_is_immediate(b2))
      {
        int pv = lt_deref_path(c, a1);
        if (pv >= 0 && lt_handle(c, pv))
          hv = pv;
      }
      if (hv >= 0 && lt_handle(c, hv) && irop_is_immediate(b2))
      {
        /* fd < 0, fd >= 0, fd == -1, fd != -1, fd <= -1, fd > -1: which edge
           holds an invalid handle (as the zero edge of a pointer) */
        int64_t k = irop_get_imm64_ex(ir, b2);
        int inv = -1; /* 1: the taken edge is invalid, 0: valid */
        if ((k == 0 && cond == TOK_LT) || (k == -1 && (cond == TOK_EQ || cond == TOK_LE)))
          inv = 1;
        else if ((k == 0 && cond == TOK_GE) || (k == -1 && (cond == TOK_NE || cond == TOK_GT)))
          inv = 0;
        if (inv >= 0)
          zero_v = hv, handle_test = 1, zero_taken = inv;
        a1 = irop_make_none();
      }
      else if (irop_is_immediate(b2) && irop_get_imm64_ex(ir, b2) == 0 && !(hv >= 0 && lt_handle(c, hv)))
        zero_v = lt_read_var(c, a1);
      else
        a1 = irop_make_none();
    }
    else
      a1 = irop_make_none();
    /* a maybe member tested in place: `if (q->next)` */
    if (zero_v < 0 && (tq->op == TCCIR_OP_TEST_ZERO || tq->op == TCCIR_OP_CMP))
      zero_v = lt_deref_path(c, a1);
    if (!handle_test && cond != TOK_EQ && cond != TOK_NE)
      zero_v = -1;
    if (!handle_test)
      zero_taken = cond == TOK_EQ;
    /* `if (ok)`, ok holding p's null test: a test of p */
    if (zero_v < 0 && c->nclink && c->clbase >= 0 && (cond == TOK_EQ || cond == TOK_NE) &&
        (tq->op == TCCIR_OP_TEST_ZERO ||
         (tq->op == TCCIR_OP_CMP && irop_is_immediate(tcc_ir_op_get_src2(ir, tq)) &&
          irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, tq)) == 0)))
    {
      IROperand t1 = tcc_ir_op_get_src1(ir, tq);
      int32_t tv = irop_is_none(t1) || !irop_is_vreg_value(t1) ? -1 : irop_get_vreg(t1);
      int tp = tv >= 0 && TCCIR_DECODE_VREG_TYPE(tv) == TCCIR_VREG_TYPE_VAR ? TCCIR_DECODE_VREG_POSITION(tv) : -1;
      int row = tp >= 0 && tp < c->nvcall ? c->cl_var[tp] : -1;
      if (row >= 0 && c->cur[c->clbase + row] == 0)
      {
        zero_v = c->cl_x[row];
        zero_taken = c->cl_pol[row] ? cond == TOK_EQ : cond == TOK_NE;
      }
    }
    /* both edges into one block: nothing to tell apart */
    if (target >= 0 && target < c->n && bb->end_idx < c->n &&
        c->cfg->instr_to_block[target] == c->cfg->instr_to_block[bb->end_idx])
      zero_v = -1;
  }
  if (zero_v >= 0)
  {
    int taken_block = target >= 0 && target < c->n ? c->cfg->instr_to_block[target] : -1;
    /* out[num_succs]: the edge out of the function, when the jump leaves it */
    for (int s = 0; s <= bb->num_succs; s++)
    {
      int taken = s < bb->num_succs ? bb->succs[s] == taken_block : target >= c->n;
      uint8_t *st = &out[s * c->nv + zero_v];
      uint8_t own = *st & LT_OWNBITS;
      uint8_t w = *st & LT_WBITS;
      if (taken == zero_taken)
      {
        own = (own & LT_OWN) ? (own & ~LT_OWN) | LT_NULL : own;
        /* a null out parameter has nothing to write */
        *st = own | LT_N_NULL | (w ? LT_W_YES : 0);
      }
      else
      {
        if ((own & LT_NULL) && own != LT_NULL)
          own &= ~LT_NULL;
        *st = own | LT_N_SET | w;
      }
      /* takes(p): the result null, p is still the caller's; valid, it moved */
      for (int p = 0; p < c->nvars; p++)
        if (c->taken_by[p] == zero_v && p != zero_v)
        {
          uint8_t *ps = &out[s * c->nv + p];
          if (taken == zero_taken)
            *ps = (*ps & LT_OWN) ? (*ps & ~LT_OWNBITS) | LT_OWN : *ps;
          else
            *ps = (*ps & LT_OWNBITS) ? (*ps & ~LT_OWNBITS) | LT_MOVED : *ps;
        }
    }
  }

  /* the variable linked to the path tested, or the path of the variable
     tested: refined the same way while they hold the same value */
  if (zero_v >= 0 && c->nlink && !lt_handle(c, zero_v))
  {
    int taken_block = target >= 0 && target < c->n ? c->cfg->instr_to_block[target] : -1;
    for (int v = 0; v < c->nvars; v++)
    {
      int partner = -1;
      if (c->lslot[v] < 0 || (c->cur[c->lslot[v]] & LT_N_NULL))
        continue;
      if (v == zero_v)
        partner = c->link[v];
      else if (c->link[v] == zero_v)
        partner = v;
      if (partner < 0)
        continue;
      for (int s = 0; s <= bb->num_succs; s++)
      {
        int taken = s < bb->num_succs ? bb->succs[s] == taken_block : target >= c->n;
        uint8_t *st = &out[s * c->nv + partner];
        *st = (*st & ~LT_NULLBITS) | (taken == zero_taken ? LT_N_NULL : LT_N_SET);
      }
    }
  }

  /* an out parameter's status tested: `if (posix_memalign(&p, ...)) return;` */
  if (lq->op == TCCIR_OP_JUMPIF && last > bb->start_idx && c->out_call)
  {
    IRQuadCompact *tq = &ir->compact_instructions[last - 1];
    int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, lq));
    IROperand a1 = tq->op == TCCIR_OP_TEST_ZERO || tq->op == TCCIR_OP_CMP ? tcc_ir_op_get_src1(ir, tq) : irop_make_none();
    int32_t av = irop_is_none(a1) ? -1 : irop_get_vreg(a1);
    int ap = av >= 0 && TCCIR_DECODE_VREG_TYPE(av) == TCCIR_VREG_TYPE_TEMP ? TCCIR_DECODE_VREG_POSITION(av) : -1;
    int64_t k0 = tq->op == TCCIR_OP_CMP && irop_is_immediate(tcc_ir_op_get_src2(ir, tq))
                     ? irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, tq))
                     : (tq->op == TCCIR_OP_TEST_ZERO ? 0 : 1);
    int fail_taken = -1;
    if (k0 == 0 && (cond == TOK_NE || cond == TOK_LT))
      fail_taken = 1;
    else if (k0 == 0 && (cond == TOK_EQ || cond == TOK_GE))
      fail_taken = 0;
    int tc = ap >= 0 ? lt_tcall(c, ap) : -1;
    /* ... or through a local holding the status: `int r = ...; if (r)` */
    if (tc < 0 && av >= 0 && c->vcall && TCCIR_DECODE_VREG_TYPE(av) == TCCIR_VREG_TYPE_VAR &&
        TCCIR_DECODE_VREG_POSITION(av) < c->nvcall)
      tc = c->vcall[TCCIR_DECODE_VREG_POSITION(av)];
    /* ... assigned more than once: the calls whose result it may hold here
       (on each path the one that wrote; nothing else) */
    int tcs[LT_STAT_CALLS], ntc = 0;
    if (tc >= 0)
      tcs[ntc++] = tc;
    else if (av >= 0 && c->vstat && c->sbase >= 0 && TCCIR_DECODE_VREG_TYPE(av) == TCCIR_VREG_TYPE_VAR &&
             TCCIR_DECODE_VREG_POSITION(av) < c->nvcall && c->vstat[TCCIR_DECODE_VREG_POSITION(av)] >= 0)
    {
      int row = c->vstat[TCCIR_DECODE_VREG_POSITION(av)];
      uint8_t bits = c->cur[c->sbase + row];
      for (int j = 0; j < LT_STAT_CALLS && !(bits & LT_STAT_OTHER); j++)
        if (bits & (1u << j))
          tcs[ntc++] = c->stcalls[row * LT_STAT_CALLS + j];
    }
    if (ntc && fail_taken >= 0)
    {
      int taken_block = target >= 0 && target < c->n ? c->cfg->instr_to_block[target] : -1;
      for (int v = 0; v < c->nvars; v++)
      {
        int hit = 0;
        for (int k = 0; k < ntc; k++)
          hit |= c->out_call[v] == tcs[k];
        if (hit)
          for (int s = 0; s <= bb->num_succs; s++)
          {
            int taken = s < bb->num_succs ? bb->succs[s] == taken_block : target >= c->n;
            uint8_t *st = &out[s * c->nv + v];
            if (!(*st & (LT_OWN | LT_NULL)))
              continue;
            *st = (*st & ~LT_OWNBITS) | (taken == fail_taken ? LT_NULL : LT_OWN);
          }
      }
    }
  }

  /* Leaving the function.  A return checks what still owns memory; falling
     off the end, or a jump past the last instruction that is no return (the
     join of an if at the end), first ends the outermost scopes. */
  int exit_kind = 0; /* 1 return, 2 end of the function */
  int exit_state = 0, exit_line = lq->line_num;
  if (lq->op == TCCIR_OP_RETURNVALUE || lq->op == TCCIR_OP_RETURNVOID)
    exit_kind = 1;
  else if (lq->op == TCCIR_OP_JUMP && target >= c->n)
  {
    exit_kind = 2;
    for (int k = c->ev_first[last]; k < c->ev_first[last + 1]; k++)
      if (c->ev[k].var == LT_EV_RETURN)
        exit_kind = 1, exit_line = c->ev[k].line;
  }
  else if (lq->op == TCCIR_OP_JUMPIF && target >= c->n)
    exit_kind = 2, exit_state = bb->num_succs;
  else if (bb->end_idx == c->n && !lt_ends_flow(ir, lq))
    exit_kind = 2, exit_state = bb->num_succs;
  /* a context that cannot leave there; a joined block's report uses the
     state of the contexts that can */
  if (exit_kind && (!c->efeas[exit_state] || c->exit_mode == 3 ||
                    (exit_state == bb->num_succs && exit_kind == 2 && c->exit_mode == 2)))
    exit_kind = 0;
  if (exit_kind && exit_state == bb->num_succs && exit_kind == 2 && c->exit_mode == 1)
    memcpy(out + exit_state * c->nv, c->exit_join, c->nv);
  if (exit_kind)
  {
    uint8_t *save = c->cur;
    c->cur = out + exit_state * c->nv;
    if (exit_kind == 2)
      lt_events_at(c, c->n);
    /* takes(p) in the callee: returning null leaves p to the caller -- it
       must still be there, and does not leak here */
    if (lq->op == TCCIR_OP_RETURNVALUE)
    {
      int rs = lt_value_null(c, tcc_ir_op_get_src1(ir, lq));
      if ((rs & LT_N_NULL) && !(rs & LT_N_SET))
        for (int v = 0; v < c->nvars; v++)
          if (c->ir->lt_vars[v].kind & LTV_TAKEN)
          {
            if ((c->cur[v] & LT_MOVED) && !lt_silent(c, v))
              tcc_warning_c(warn_lifetime)("%i:'%s' returns null but '%s' %s %s: takes(%s) leaves it to the caller",
                                           exit_line, funcname, lt_name(c, v),
                                           (c->cur[v] & LT_OWNBITS) == LT_MOVED ? "was" : "may have been",
                                           c->move_drop[v] ? "freed" : "moved", lt_name(c, v));
            c->cur[v] = (c->cur[v] & ~LT_OWNBITS) | LT_UNINIT;
          }
    }
    lt_exit(c, exit_kind == 2, exit_kind == 2 ? c->end_line : exit_line);
    c->cur = save;
  }
}

/* a walk over the blocks starts: global temps are joined over its definitions */
static void lt_walk_begin(LtCtx *c)
{
  if (c->nglob)
    memset(c->g_known, 0, c->nglob);
}

static void lt_sort_events(IRLtEvent *ev, int n)
{
  for (int i = 1; i < n; i++)
  {
    IRLtEvent e = ev[i];
    int j = i;
    for (; j > 0 && ev[j - 1].idx > e.idx; j--)
      ev[j] = ev[j - 1];
    ev[j] = e;
  }
}

#define LT_MAX_CTX 8
#define LT_CTX_BUDGET (1u << 20)
#define LT_CTX_ITERS 24
#define LT_CTX_TOTAL 16384 /* contexts in a function, at most */

/* Merge a context (known k, values v, state st) into a block's contexts:
   the one knowing the same, a new one, or -- when there are `max` -- the
   one sharing most known values, keeping only what both know.  1 when the
   block's states changed. */
static int lt_ctx_merge(uint8_t *bs, uint32_t *bk, uint32_t *bv, uint8_t *bn, int max, int nv, uint32_t k, uint32_t v,
                        const uint8_t *st)
{
  int m, best = 0, bestn = -1, changed = 0;
  v &= k;
  for (m = 0; m < *bn; m++)
    if (bk[m] == k && bv[m] == v)
      break;
  if (m == *bn && *bn < max)
  {
    memcpy(bs + (size_t)m * nv, st, nv);
    bk[m] = k, bv[m] = v;
    (*bn)++;
    return 1;
  }
  if (m == *bn)
  {
    for (int j = 0; j < *bn; j++)
    {
      int n = __builtin_popcount(k & bk[j] & ~(v ^ bv[j]));
      if (n > bestn)
        bestn = n, best = j;
    }
    m = best;
    uint32_t nk = k & bk[m] & ~(v ^ bv[m]);
    if (nk != bk[m])
      bk[m] = nk, bv[m] &= nk, changed = 1;
  }
  uint8_t *d = bs + (size_t)m * nv;
  for (int i = 0; i < nv; i++)
    if ((d[i] | st[i]) != d[i])
      d[i] |= st[i], changed = 1;
  /* a context that now knows what another does: one */
  for (int j = 0; j < *bn; j++)
    if (j != m && bk[j] == bk[m] && bv[j] == bv[m])
    {
      for (int i = 0; i < nv; i++)
        d[i] |= bs[(size_t)j * nv + i];
      int last = --(*bn);
      if (j != last)
      {
        memcpy(bs + (size_t)j * nv, bs + (size_t)last * nv, nv);
        bk[j] = bk[last], bv[j] = bv[last];
        if (m == last)
          m = j, d = bs + (size_t)m * nv;
      }
      j = -1; /* again from the start */
      changed = 1;
    }
  return changed;
}

/* a block's contexts joined into one that knows what they all agree on */
static int lt_ctx_collapse(uint8_t *bs, uint32_t *bk, uint32_t *bv, uint8_t *bn, int nv)
{
  if (*bn <= 1)
    return 0;
  for (int m = 1; m < *bn; m++)
  {
    for (int i = 0; i < nv; i++)
      bs[i] |= bs[(size_t)m * nv + i];
    bk[0] &= bk[m] & ~(bv[m] ^ bv[0]);
    bv[0] &= bk[0];
  }
  *bn = 1;
  return 1;
}

/* the block's edge out[num_succs] leaves the function past its last
   instruction (a jump there, or falling off the end) */
static int lt_leaves_past_end(LtCtx *c, IRBasicBlock *bb)
{
  IRQuadCompact *lq = &c->ir->compact_instructions[bb->end_idx - 1];
  if (lq->op == TCCIR_OP_JUMPIF && (int)tcc_ir_op_dest_imm(c->ir, lq) >= c->n)
    return 1;
  return bb->end_idx == c->n && !lt_ends_flow(c->ir, lq);
}

/* Path conditions: the locals and parameters (address not taken) tested
   against a constant more than once, or tested and assigned a constant --
   not in an inlined body, whose tests are its own -- become predicates. */
static void lt_find_preds(LtCtx *c)
{
  TCCIRState *ir = c->ir;
  int32_t cvr[64];
  int64_t cvk[64];
  uint8_t ntest[64], nconst[64];
  int nc = 0;
  for (int i = 1; i < c->n; i++)
  {
    IRQuadCompact *lq = &ir->compact_instructions[i], *tq = &ir->compact_instructions[i - 1];
    if (lq->op != TCCIR_OP_JUMPIF || (tq->op != TCCIR_OP_TEST_ZERO && tq->op != TCCIR_OP_CMP) || (c->inl && c->inl[i]))
      continue;
    int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, lq));
    IROperand a = tcc_ir_op_get_src1(ir, tq);
    int64_t k = 0;
    int32_t vr;
    if (cond != TOK_EQ && cond != TOK_NE)
      continue;
    if (tq->op == TCCIR_OP_CMP)
    {
      IROperand b = tcc_ir_op_get_src2(ir, tq);
      if (!irop_is_immediate(b) || b.is_lval)
        continue;
      k = irop_get_imm64_ex(ir, b);
    }
    if (irop_is_none(a) || !irop_is_vreg_value(a) || lt_is_addr(a) || (vr = irop_get_vreg(a)) < 0 ||
        (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR && TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_PARAM) ||
        irop_get_btype(a) == IROP_BTYPE_FLOAT32 || irop_get_btype(a) == IROP_BTYPE_FLOAT64 ||
        irop_get_btype(a) == IROP_BTYPE_STRUCT || lt_vreg_addrtaken(ir, vr))
      continue;
    int j;
    for (j = 0; j < nc; j++)
      if (cvr[j] == vr && cvk[j] == k)
        break;
    if (j == nc)
    {
      if (nc == 64)
        continue;
      cvr[nc] = vr, cvk[nc] = k, ntest[nc] = nconst[nc] = 0, nc++;
    }
    if (ntest[j] < 2)
      ntest[j]++;
  }
  if (!nc)
    return;
  for (int i = 0; i < c->n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cf = &irop_config[q->op];
    if ((q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE) || !cf->has_dest || !cf->has_src1)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q), s1 = tcc_ir_op_get_src1(ir, q);
    if (irop_is_none(d) || !irop_dest_defines_vreg(d) || !irop_is_immediate(s1) || s1.is_lval)
      continue;
    for (int j = 0; j < nc; j++)
      if (cvr[j] == irop_get_vreg(d))
        nconst[j] = 1;
  }
  for (int j = 0; j < nc && c->npred < 32; j++)
    if (ntest[j] >= 2 || nconst[j])
    {
      c->pr_vreg[c->npred] = cvr[j];
      c->pr_k[c->npred] = cvk[j];
      c->npred++;
    }
}

void tcc_ir_lifetime_check(TCCIRState *ir, Sym *func)
{
  int end_line = ir->lt_end_line;
  LtCtx c;
  int nv = ir->lt_nb_vars;
  int ret_plain_ptr = func && func->type.ref && (func->type.ref->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR &&
                      !(type_yasc(&func->type.ref->type) & YASC_MAYBE);
  if (!nv && !ret_plain_ptr && !ir->lt_nonnull_calls && !ir->lt_maybe_calls && !ir->lt_nb_members && !ir->lt_nb_gmembers)
  {
    lt_free_records(ir);
    return;
  }
  if (!ir->next_instruction_index)
  {
    /* an empty body leaves at once: only the parameters can be wrong */
    uint8_t *st = tcc_malloc(nv + 1);
    memset(&c, 0, sizeof c);
    c.ir = ir;
    c.nv = c.nvars = nv;
    c.report = 1;
    c.cur = st;
    lt_entry_state(&c, st);
    tcc_state->diag_noabort++;
    lt_exit(&c, 1, end_line);
    tcc_state->diag_noabort--;
    tcc_free(st);
    lt_free_records(ir);
    return;
  }
  memset(&c, 0, sizeof c);
  c.ir = ir;
  c.nv = c.nvars = nv;
  c.n = ir->next_instruction_index;
  c.end_line = end_line;
  {
    /* an owned result, or an error union (!T) whose payload is one */
    CType *rt = func && func->type.ref ? &func->type.ref->type : NULL;
    if (rt && (rt->t & VT_BTYPE) == VT_LLONG && rt->ref && (rt->ref->yasc & YASC_ERRUNION))
      rt = &rt->ref->type;
    c.ret_owned = rt && (type_yasc(rt) & YASC_OWNED);
  }
  c.ret_plain_ptr = ret_plain_ptr;

  /* vreg -> variable */
  for (int v = 0; v < nv; v++)
  {
    int32_t vreg = ir->lt_vars[v].vreg;
    int pos = TCCIR_DECODE_VREG_POSITION(vreg);
    if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_VAR && pos >= c.nmap_var)
      c.nmap_var = pos + 1;
    if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM && pos >= c.nmap_param)
      c.nmap_param = pos + 1;
  }
  c.map_var = tcc_malloc((c.nmap_var + c.nmap_param + 1) * sizeof(int32_t));
  c.map_param = c.map_var + c.nmap_var;
  memset(c.map_var, 0xff, (c.nmap_var + c.nmap_param + 1) * sizeof(int32_t));
  /* &v passed to an owned out-parameter only (getline(&line, ...)): every
     address of v taken goes straight into such an argument */
  c.out_call = tcc_malloc((nv + 1) * sizeof(int32_t));
  memset(c.out_call, 0xff, (nv + 1) * sizeof(int32_t));
  c.tout = tcc_malloc((ir->next_temporary_variable + 1) * sizeof(int32_t));
  memset(c.tout, 0xff, (ir->next_temporary_variable + 1) * sizeof(int32_t));
  uint8_t *outok = tcc_mallocz(nv + 1); /* 1: some address taken that way; 2: some other way */
  int *escline = tcc_mallocz((nv + 1) * sizeof(int)); /* where an address went some other way */
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < c.n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      const IRRegistersConfig *cfg = &irop_config[q->op];
      IROperand d = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
      IROperand s1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
      IROperand s2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
      int32_t dv = irop_is_none(d) ? -1 : irop_get_vreg(d), sv = irop_is_none(s1) ? -1 : irop_get_vreg(s1);
      if (pass == 0 && (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA) && dv >= 0 &&
          TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP &&
          TCCIR_DECODE_VREG_POSITION(dv) < ir->next_temporary_variable && sv >= 0 &&
          (lt_is_addr(s1) || lt_lea_of_local(q, s1)) && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_VAR)
      {
        int v = lt_var_by_vreg(ir, sv);
        if (v >= 0)
          c.tout[TCCIR_DECODE_VREG_POSITION(dv)] = v;
      }
      if (pass == 1 && sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP &&
          TCCIR_DECODE_VREG_POSITION(sv) < ir->next_temporary_variable && c.tout[TCCIR_DECODE_VREG_POSITION(sv)] >= 0)
      {
        int v = c.tout[TCCIR_DECODE_VREG_POSITION(sv)], ok = 0;
        if (q->op == TCCIR_OP_FUNCPARAMVAL && irop_is_vreg_value(s1))
        {
          int64_t enc = irop_get_imm64_ex(ir, s2);
          int call_id = TCCIR_DECODE_CALL_ID(enc), k = TCCIR_DECODE_PARAM_IDX(enc);
          IRLtCall *call = call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
          ok = call && call->known && (k -= call->implicit) >= 0 && k < 32 &&
               (((call->outown | call->tborrow) >> k) & 1);
          /* ... or to a parameter that only reads (`const T * view`) */
          if (!ok && call && call->noted && k >= 0 && k < call->nparams && ((call->keep >> k) & 1))
            ok = 1;
        }
        /* the same bound to an inlined callee's read-only parameter */
        else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) && irop_is_vreg_value(s1) && !s1.is_lval &&
                 dv >= 0 && irop_dest_defines_vreg(d) && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR)
        {
          int pv = lt_var_by_vreg(ir, dv);
          ok = pv >= 0 && ir->lt_vars[pv].arg_index >= 0 && !(ir->lt_vars[pv].kind & LTV_MUT);
        }
        outok[v] |= ok ? 1 : 2;
        if (!ok && !escline[v])
          escline[v] = q->line_num;
        if (!ok)
          c.tout[TCCIR_DECODE_VREG_POSITION(sv)] = -1;
      }
    }
  for (int v = 0; v < nv; v++)
  {
    int32_t vreg = ir->lt_vars[v].vreg;
    int pos = TCCIR_DECODE_VREG_POSITION(vreg);
    if (lt_vreg_addrtaken(ir, vreg) && outok[v] != 1)
    {
      ir->lt_vars[v].untracked = 1;
      /* an owned variable the check stops following: said, not silent */
      if ((ir->lt_vars[v].kind & LTV_OWNED) && !ir->lt_vars[v].silent && !(ir->lt_vars[v].kind & LTV_RESULT))
      {
        const char *name = get_tok_str(ir->lt_vars[v].tok, NULL);
        tcc_state->diag_noabort++;
        tcc_warning_c(warn_lifetime)("%i:the address of '%s' goes where it may be written: the lifetime check does not "
                                     "follow '%s'",
                                     escline[v] ? escline[v] : ir->lt_vars[v].line, name, name);
        tcc_state->diag_noabort--;
      }
    }
    else if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_VAR)
      c.map_var[pos] = v;
    else if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM)
      c.map_param[pos] = v;
    else
      ir->lt_vars[v].untracked = 1;
  }

  /* a variable whose address is taken can change behind the analysis */
  for (int i = 0; i < c.n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand ops[3];
    int nops = 0;
    if (cfg->has_dest)
      ops[nops++] = tcc_ir_op_get_dest(ir, q);
    if (cfg->has_src1)
      ops[nops++] = tcc_ir_op_get_src1(ir, q);
    if (cfg->has_src2)
      ops[nops++] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < nops; k++)
    {
      int32_t vreg = irop_get_vreg(ops[k]);
      int v;
      int addr = lt_is_addr(ops[k]) || (k == nops - 1 - cfg->has_src2 && cfg->has_src1 && lt_lea_of_local(q, ops[k]));
      if (!irop_is_none(ops[k]) && vreg >= 0 && addr && TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP &&
          (v = lt_map(&c, vreg)) >= 0 && outok[v] != 1)
        ir->lt_vars[v].untracked = 1;
    }
  }
  tcc_free(outok);
  tcc_free(escline);
  for (int v = 0; v < nv; v++)
    if (ir->lt_vars[v].untracked)
    {
      int32_t vreg = ir->lt_vars[v].vreg;
      int pos = TCCIR_DECODE_VREG_POSITION(vreg);
      if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_VAR)
        c.map_var[pos] = -1;
      else if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM)
        c.map_param[pos] = -1;
    }

  c.cfg = tcc_ir_cfg_build(ir);
  if (!c.cfg)
  {
    tcc_free(c.map_var);
    lt_free_records(ir);
    return;
  }
  tcc_ir_cfg_compute_rpo(c.cfg);
  int nb = c.cfg->num_blocks, maxs = 1;
  for (int b = 0; b < nb; b++)
    if (c.cfg->blocks[b].num_succs > maxs)
      maxs = c.cfg->blocks[b].num_succs;

  c.nalias = ir->next_temporary_variable;
  /* maybe members: room for as many paths as there are records (each finds
     at most one), as state entries after the variables, and as lt_vars
     entries for their names and kind; trimmed to those found below */
  if (ir->lt_nb_members + ir->lt_nb_gmembers + ir->lt_nb_dmembers)
  {
    c.pcap = ir->lt_nb_members + ir->lt_nb_gmembers + ir->lt_nb_dmembers;
    ir->lt_vars = tcc_realloc(ir->lt_vars, (nv + c.pcap) * sizeof(IRLtVar));
    memset(ir->lt_vars + nv, 0, c.pcap * sizeof(IRLtVar));
    c.pbase = tcc_malloc(c.pcap * sizeof(int32_t));
    c.poff = tcc_malloc(c.pcap * sizeof(int32_t));
    c.pidx = tcc_malloc(c.pcap * sizeof(int32_t));
    c.xbase = tcc_malloc(5 * (c.nalias + 1) * sizeof(int32_t));
    c.xoff = c.xbase + (c.nalias + 1);
    c.xidx = c.xoff + (c.nalias + 1);
    c.xsc = c.xidx + (c.nalias + 1);
    c.xat = c.xsc + (c.nalias + 1);
    memset(c.xidx, 0xff, (c.nalias + 1) * sizeof(int32_t));
    c.pname = tcc_mallocz(c.pcap * sizeof(char *));
    c.pmark = tcc_malloc(c.pcap);
    c.trec = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
    memset(c.trec, 0xff, (c.nalias + 1) * sizeof(int32_t));
    for (int r = ir->lt_nb_members - 1; r >= 0; r--)
    {
      int32_t vreg = ir->lt_members[r].vreg;
      int pos = TCCIR_DECODE_VREG_POSITION(vreg);
      if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP && pos < c.nalias)
        c.trec[pos] = r;
    }
    /* each call's instruction: an argument's members are forgotten there */
    c.ncall_at = ir->next_call_id;
    c.call_at = tcc_malloc((c.ncall_at + 1) * sizeof(int32_t));
    memset(c.call_at, 0xff, (c.ncall_at + 1) * sizeof(int32_t));
    for (int i = 0; i < c.n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if ((q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID) && irop_config[q->op].has_src2)
      {
        int id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
        if (id >= 0 && id < c.ncall_at)
          c.call_at[id] = i;
      }
    }
    c.nv = nv + c.pcap;
  }
  /* global temps: defined in more than one block, or used in a block that
     does not define them */
  {
    int32_t *tdef = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
    memset(tdef, 0xff, (c.nalias + 1) * sizeof(int32_t));
    c.tglob = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
    memset(c.tglob, 0xff, (c.nalias + 1) * sizeof(int32_t));
    for (int pass = 0; pass < 2; pass++)
      for (int i = 0; i < c.n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        const IRRegistersConfig *cfg = &irop_config[q->op];
        int b = c.cfg->instr_to_block[i];
        IROperand ops[3];
        int nops = 0, def = -1;
        if (cfg->has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          if (!irop_is_none(d) && irop_dest_defines_vreg(d))
            def = nops;
          ops[nops++] = d;
        }
        if (cfg->has_src1)
          ops[nops++] = tcc_ir_op_get_src1(ir, q);
        if (cfg->has_src2)
          ops[nops++] = tcc_ir_op_get_src2(ir, q);
        for (int k = 0; k < nops; k++)
        {
          int32_t vr = irop_is_none(ops[k]) ? -1 : irop_get_vreg(ops[k]);
          int pos;
          if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP ||
              (pos = TCCIR_DECODE_VREG_POSITION(vr)) >= c.nalias)
            continue;
          if (pass == 0 && k == def)
          {
            if (tdef[pos] == -1)
              tdef[pos] = b;
            else if (tdef[pos] != b)
              tdef[pos] = -2;
          }
          else if (pass == 1 && k != def && tdef[pos] != b && tdef[pos] != -1)
            tdef[pos] = -2;
        }
      }
    for (int pos = 0; pos < c.nalias; pos++)
      if (tdef[pos] == -2 && c.nglob < 1024)
        c.tglob[pos] = c.nglob++;
    tcc_free(tdef);
    if (!c.nglob)
    {
      tcc_free(c.tglob);
      c.tglob = NULL;
    }
    else
    {
      c.g_alias = tcc_malloc(7 * c.nglob * sizeof(int32_t));
      c.g_line = c.g_alias + 6 * c.nglob;
      c.tline = tcc_mallocz((c.nalias + 1) * sizeof(int32_t));
      c.g_palias = c.g_alias + c.nglob;
      c.g_tbase = c.g_palias + c.nglob;
      c.g_toff = c.g_tbase + c.nglob;
      c.g_taddr = c.g_toff + c.nglob;
      c.g_tcall = c.g_taddr + c.nglob;
      c.g_known = tcc_mallocz(c.nglob);
      ir->lt_vars = tcc_realloc(ir->lt_vars, (c.nv + c.nglob) * sizeof(IRLtVar));
      memset(ir->lt_vars + c.nv, 0, c.nglob * sizeof(IRLtVar));
      for (int g = 0; g < c.nglob; g++)
        ir->lt_vars[c.nv + g].untracked = 1, ir->lt_vars[c.nv + g].vreg = -1;
    }
    c.gslot = c.nv;
    c.nv += c.nglob;
  }
  /* the local structs, and where the inlined bodies are */
  if (c.pcap)
  {
    for (int k = 0; k < ir->lt_nb_fobjs; k++)
    {
      int *o = &ir->lt_fobjs[5 * k];
      lt_obj_new(&c, LT_OK_FRAME, NULL, o[0], o[1], o[2], o[3], o[4]);
    }
    /* a `drop struct T * view` parameter: every owned member of its struct is
       a path from the start, whether the body names it or not */
    for (int k = 0; k < ir->lt_nb_dmembers; k++)
    {
      const IRLtMember *m = &ir->lt_dmembers[k];
      int b = lt_map(&c, m->vreg);
      if (b >= 0)
        lt_path(&c, b, m->offset, m->tok, m->owned, -1);
    }
  }
  if (ir->lt_nb_inl)
  {
    c.inl = tcc_mallocz(c.n + 1);
    for (int k = 0; k < ir->lt_nb_inl; k++)
      for (int i = ir->lt_inl[2 * k]; i < ir->lt_inl[2 * k + 1] && i < c.n; i++)
        c.inl[i] = 1;
  }
  c.tbase = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
  c.toff = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
  c.taddr = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
  c.alias = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
  c.tcall = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
  c.tcall_gen = tcc_mallocz((c.nalias + 1) * sizeof(uint32_t));
  c.palias = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
  c.tnull = tcc_malloc(c.nalias + 1);
  c.tderiv = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
  c.alias_gen = tcc_mallocz((c.nalias + 1) * sizeof(uint32_t));
  c.cur = tcc_malloc(c.nv + 1);
  c.move_line = tcc_mallocz((c.nv + 1) * sizeof(int));
  c.move_drop = tcc_mallocz(c.nv + 1);
  c.arg_bound = tcc_mallocz(c.nv + 1);
  c.borrow = tcc_malloc((c.nv + 1) * sizeof(int32_t));
  memset(c.borrow, 0xff, (c.nv + 1) * sizeof(int32_t));
  /* locals defined once (a status kept in a variable) */
  c.nvcall = ir->next_local_variable;
  if (c.nvcall > 0)
  {
    c.vcall = tcc_malloc(c.nvcall * sizeof(int32_t));
    memset(c.vcall, 0xff, c.nvcall * sizeof(int32_t));
    c.vdefs = tcc_mallocz(c.nvcall);
    for (int i = 0; i < c.n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      IROperand d = irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
      int32_t dv = irop_is_none(d) ? -1 : irop_get_vreg(d);
      if (dv >= 0 && irop_dest_defines_vreg(d) && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(dv) < c.nvcall && c.vdefs[TCCIR_DECODE_VREG_POSITION(dv)] < 2)
        c.vdefs[TCCIR_DECODE_VREG_POSITION(dv)]++;
    }
    for (int p = 0; p < c.nvcall; p++)
      if (lt_vreg_addrtaken(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p)))
        c.vdefs[p] = 3;
    /* locals assigned more than once, some value an owned out-parameter's
       status (the call's result, or a temp holding it); locals holding a
       pointer variable's null test */
    int32_t *tc0 = tcc_malloc((c.nalias + 1) * sizeof(int32_t));
    memset(tc0, 0xff, (c.nalias + 1) * sizeof(int32_t));
    c.sbase = c.clbase = -1;
    for (int i = 0; i < c.n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      const IRRegistersConfig *cf = &irop_config[q->op];
      IROperand d = cf->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
      IROperand s1 = cf->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
      int32_t dv = irop_is_none(d) || !irop_dest_defines_vreg(d) ? -1 : irop_get_vreg(d);
      int32_t sv = irop_is_none(s1) ? -1 : irop_get_vreg(s1), xv;
      int cid = -1, pol, p;
      if (dv < 0)
        continue;
      if (q->op == TCCIR_OP_FUNCCALLVAL && cf->has_src2)
        cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
      if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
      {
        if (cid >= 0 && TCCIR_DECODE_VREG_POSITION(dv) < c.nalias)
          tc0[TCCIR_DECODE_VREG_POSITION(dv)] = cid;
        continue;
      }
      if (TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_VAR || (p = TCCIR_DECODE_VREG_POSITION(dv)) >= c.nvcall ||
          c.vdefs[p] == 3)
        continue;
      if (cid < 0 && (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_STORE ||
                      q->op == TCCIR_OP_ZEXT) &&
          sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP && irop_is_vreg_value(s1) && !s1.is_lval &&
          TCCIR_DECODE_VREG_POSITION(sv) < c.nalias)
        cid = tc0[TCCIR_DECODE_VREG_POSITION(sv)];
      if (c.vdefs[p] == 2 && cid >= 0 && cid < ir->lt_nb_calls && ir->lt_calls[cid].known &&
          ir->lt_calls[cid].outown_out && !ir->lt_calls[cid].ret_maybe)
      {
        if (!c.vstat)
        {
          c.vstat = tcc_malloc(c.nvcall * sizeof(int32_t));
          memset(c.vstat, 0xff, c.nvcall * sizeof(int32_t));
          c.stcalls = tcc_malloc(LT_MAX_STAT * LT_STAT_CALLS * sizeof(int32_t));
          memset(c.stcalls, 0xff, LT_MAX_STAT * LT_STAT_CALLS * sizeof(int32_t));
        }
        if (c.vstat[p] < 0 && c.nstat < LT_MAX_STAT)
          c.vstat[p] = c.nstat++;
        for (int j = 0, row = c.vstat[p]; row >= 0 && j < LT_STAT_CALLS; j++)
          if (c.stcalls[row * LT_STAT_CALLS + j] == cid || c.stcalls[row * LT_STAT_CALLS + j] < 0)
          {
            c.stcalls[row * LT_STAT_CALLS + j] = cid;
            break;
          }
      }
      if (lt_setif_test(ir, i, &xv, &pol) && (TCCIR_DECODE_VREG_TYPE(xv) == TCCIR_VREG_TYPE_VAR ||
                                              TCCIR_DECODE_VREG_TYPE(xv) == TCCIR_VREG_TYPE_PARAM))
      {
        int x = lt_map(&c, xv);
        if (x < 0 || lt_handle(&c, x))
          continue;
        if (!c.cl_var)
        {
          c.cl_var = tcc_malloc(c.nvcall * sizeof(int32_t));
          memset(c.cl_var, 0xff, c.nvcall * sizeof(int32_t));
          c.cl_x = tcc_malloc(LT_MAX_CLINK * sizeof(int32_t));
          c.cl_pol = tcc_malloc(LT_MAX_CLINK);
        }
        if (c.cl_var[p] >= 0)
        {
          /* linked twice: to the same test, or to none */
          if (c.cl_x[c.cl_var[p]] != x || c.cl_pol[c.cl_var[p]] != pol)
            c.cl_x[c.cl_var[p]] = -1;
        }
        else if (c.nclink < LT_MAX_CLINK)
        {
          c.cl_var[p] = c.nclink++;
          c.cl_x[c.cl_var[p]] = x;
          c.cl_pol[c.cl_var[p]] = pol;
        }
      }
    }
    tcc_free(tc0);
  }
  c.ncall_bsrc = ir->next_call_id;
  c.call_bsrc = tcc_malloc((c.ncall_bsrc + 1) * sizeof(int32_t));
  memset(c.call_bsrc, 0xff, (c.ncall_bsrc + 1) * sizeof(int32_t));
  c.call_tvar = tcc_malloc(2 * (c.ncall_bsrc + 1) * sizeof(int32_t));
  memset(c.call_tvar, 0xff, 2 * (c.ncall_bsrc + 1) * sizeof(int32_t));
  c.call_tsrc = c.call_tvar + c.ncall_bsrc + 1;
  if (c.npaths || c.pcap)
  {
    c.link = tcc_malloc((nv + 1) * sizeof(int32_t));
    c.lslot = tcc_malloc((nv + 1) * sizeof(int32_t));
    memset(c.link, 0xff, (nv + 1) * sizeof(int32_t));
    memset(c.lslot, 0xff, (nv + 1) * sizeof(int32_t));
  }
  c.take_call = tcc_malloc((c.nv + 1) * sizeof(int32_t));
  c.taken_by = tcc_malloc((c.nv + 1) * sizeof(int32_t));
  memset(c.take_call, 0xff, (c.nv + 1) * sizeof(int32_t));
  memset(c.taken_by, 0xff, (c.nv + 1) * sizeof(int32_t));

  /* scope ends, sorted by position (records keep their order for one index) */
  for (int k = 0; k < ir->lt_nb_events; k++)
    if (ir->lt_events[k].idx > c.n)
      ir->lt_events[k].idx = c.n;
  lt_sort_events(ir->lt_events, ir->lt_nb_events);
  c.ev = ir->lt_events;
  c.ev_first = tcc_mallocz((c.n + 2) * sizeof(int));
  for (int k = 0; k < ir->lt_nb_events; k++)
    c.ev_first[c.ev[k].idx + 1]++;
  for (int i = 0; i <= c.n; i++)
    c.ev_first[i + 1] += c.ev_first[i];

  c.eknown = tcc_malloc((maxs + 1) * sizeof(uint32_t));
  c.eval = tcc_malloc((maxs + 1) * sizeof(uint32_t));
  c.efeas = tcc_malloc(maxs + 1);
  lt_find_preds(&c);
  uint8_t *in = tcc_mallocz(c.nv + 1);
  uint8_t *reached = tcc_mallocz(nb);
  uint8_t *out = tcc_malloc((maxs + 1) * c.nv + 1);
  c.scratch = tcc_malloc((maxs + 1) * c.nv + 1);
  lt_entry_state(&c, in);
  /* which owned variable each pointer borrows from, and which maybe members
     there are: one walk in source order, its states thrown away.  What it
     finds depends on each block's instructions only, never on the state. */
  /* what each inlined call's parameter was bound to: for its borrowing
     struct or view pointer result */
  if (ir->lt_nb_sobjs || ir->lt_nb_pres)
  {
    c.bind_owner = tcc_malloc((c.nvars + 1) * sizeof(int32_t));
    for (int v = 0; v <= c.nvars; v++)
      c.bind_owner[v] = -3;
  }
  if (ir->lt_nb_sobjs)
    lt_sobj_prepare(&c);
  c.vbset = tcc_malloc((c.nvars * LT_MAX_BOWN + 1) * sizeof(int32_t));
  c.vbcnt = tcc_mallocz(c.nvars + 1);
  c.vbsaid = tcc_mallocz(c.nvars + 1);
  if (c.nglob)
  {
    c.gbset = tcc_malloc((c.nglob * LT_MAX_BOWN + 1) * sizeof(int32_t));
    c.gbcnt = tcc_mallocz(c.nglob + 1);
  }
  c.collect = 1;
  lt_walk_begin(&c);
  for (int b = 0; b < nb; b++)
    lt_block(&c, b, in, out);
  c.collect = 0;
  reached[0] = 1;
  c.gslot = nv + c.npaths;
  c.nv = c.gslot + c.nglob;
  c.pcap = c.npaths;
  /* the linked variables get a state entry each, after the global temps */
  c.lbase = c.nv;
  if (c.link)
    for (int v = 0; v < nv; v++)
      if (c.link[v] >= 0 && lt_maybe(&c, v) && !lt_handle(&c, v))
        c.lslot[v] = c.lbase + c.nlink++;
  /* ... then the status locals, then the locals holding a null test, then
     the rows of what borrows from several owners: a global temp whose
     definitions disagree (one owner agreed on is the temp's palias), a
     variable with several owners or owners through such a temp */
  c.sbase = c.lbase + c.nlink;
  c.clbase = c.sbase + c.nstat;
  c.bbase = c.clbase + c.nclink;
  for (int g = 0; g < c.nglob; g++)
    if ((c.gbcnt[g] & ~LT_BOWN_VIA) >= 1 && (c.gbcnt[g] & ~LT_BOWN_VIA) <= LT_MAX_BOWN &&
        ((c.gbcnt[g] & ~LT_BOWN_VIA) > 1 || c.g_palias[g] < 0))
    {
      if (!c.gbrow)
      {
        c.gbrow = tcc_malloc(c.nglob * sizeof(int32_t));
        memset(c.gbrow, 0xff, c.nglob * sizeof(int32_t));
      }
      c.gbrow[g] = c.nbrow++;
    }
  for (int v = 0; v < c.nvars; v++)
  {
    int n = c.vbcnt[v] & ~LT_BOWN_VIA;
    if (n > LT_MAX_BOWN)
      c.borrow[v] = -2; /* too many: not followed */
    else if (n >= 1 && (c.borrow[v] == -2 || ((c.vbcnt[v] & LT_BOWN_VIA) && c.gbrow)))
    {
      if (!c.vbrow)
      {
        c.vbrow = tcc_malloc(c.nvars * sizeof(int32_t));
        memset(c.vbrow, 0xff, c.nvars * sizeof(int32_t));
      }
      c.vbrow[v] = c.nbrow++;
      c.borrow[v] = -2;
    }
  }
  /* ... and the local structs that borrow */
  for (int k = 0; k < c.nsobj; k++)
    if (!c.so_untracked[k] && (c.sbcnt[k] & ~LT_BOWN_VIA) >= 1 && (c.sbcnt[k] & ~LT_BOWN_VIA) <= LT_MAX_BOWN)
    {
      if (!c.sbrow)
      {
        c.sbrow = tcc_malloc(c.nsobj * sizeof(int32_t));
        memset(c.sbrow, 0xff, c.nsobj * sizeof(int32_t));
      }
      c.sbrow[k] = c.nbrow++;
    }
  if (c.nbrow)
  {
    c.brow_own = tcc_malloc(c.nbrow * LT_MAX_BOWN * sizeof(int32_t));
    c.brow_n = tcc_malloc(c.nbrow);
    c.brow_at = tcc_malloc(c.nbrow * sizeof(int32_t));
    for (int g = 0; c.gbrow && g < c.nglob; g++)
      if (c.gbrow[g] >= 0)
      {
        c.brow_n[c.gbrow[g]] = c.gbcnt[g] & ~LT_BOWN_VIA;
        memcpy(c.brow_own + c.gbrow[g] * LT_MAX_BOWN, c.gbset + g * LT_MAX_BOWN, LT_MAX_BOWN * sizeof(int32_t));
      }
    for (int v = 0; c.vbrow && v < c.nvars; v++)
      if (c.vbrow[v] >= 0)
      {
        c.brow_n[c.vbrow[v]] = c.vbcnt[v] & ~LT_BOWN_VIA;
        memcpy(c.brow_own + c.vbrow[v] * LT_MAX_BOWN, c.vbset + v * LT_MAX_BOWN, LT_MAX_BOWN * sizeof(int32_t));
      }
    for (int k = 0; c.sbrow && k < c.nsobj; k++)
      if (c.sbrow[k] >= 0)
      {
        c.brow_n[c.sbrow[k]] = c.sbcnt[k] & ~LT_BOWN_VIA;
        memcpy(c.brow_own + c.sbrow[k] * LT_MAX_BOWN, c.sbset + k * LT_MAX_BOWN, LT_MAX_BOWN * sizeof(int32_t));
      }
  }
  /* each row as long as its owners: a block's state holds every row */
  int nbent = 0;
  for (int r = 0; r < c.nbrow; r++)
    c.brow_at[r] = nbent, nbent += c.brow_n[r];
  int nextra = c.nlink + c.nstat + c.nclink + nbent;
  if (nextra)
  {
    /* the per-variable arrays sized by c.nv grow with them */
    ir->lt_vars = tcc_realloc(ir->lt_vars, (c.nv + nextra) * sizeof(IRLtVar));
    c.move_line = tcc_realloc(c.move_line, (c.nv + nextra + 1) * sizeof(int));
    c.move_drop = tcc_realloc(c.move_drop, c.nv + nextra + 1);
    c.arg_bound = tcc_realloc(c.arg_bound, c.nv + nextra + 1);
    c.borrow = tcc_realloc(c.borrow, (c.nv + nextra + 1) * sizeof(int32_t));
    c.take_call = tcc_realloc(c.take_call, (c.nv + nextra + 1) * sizeof(int32_t));
    c.taken_by = tcc_realloc(c.taken_by, (c.nv + nextra + 1) * sizeof(int32_t));
    c.cur = tcc_realloc(c.cur, c.nv + nextra + 1);
    for (int k = c.nv; k < c.nv + nextra + 1; k++)
      c.move_line[k] = 0, c.move_drop[k] = 0, c.arg_bound[k] = 0, c.borrow[k] = -1, c.take_call[k] = -1,
      c.taken_by[k] = -1;
    c.nv += nextra;
    tcc_free(out);
    tcc_free(c.scratch);
    out = tcc_malloc((maxs + 1) * c.nv + 1);
    c.scratch = tcc_malloc((maxs + 1) * c.nv + 1);
  }
  for (int g = 0; g < c.nglob + nextra; g++)
  {
    memset(&ir->lt_vars[c.gslot + g], 0, sizeof(IRLtVar));
    ir->lt_vars[c.gslot + g].untracked = 1, ir->lt_vars[c.gslot + g].vreg = -1;
  }
  tcc_free(in);
  in = NULL;
  nv = c.nv;

  /* Each block's state: up to `nctx` contexts (path conditions), stride
     `ctxmax`; without predicates one, as a plain join.  At most LT_CTX_TOTAL
     contexts in all, and LT_CTX_BUDGET bytes of states beyond one per block. */
  int ctxmax = c.npred ? LT_MAX_CTX : 1;
  /* a big function: fewer contexts per block (the walks cost one block
     walk per context) */
  if (ctxmax > LT_CTX_TOTAL / nb)
    ctxmax = LT_CTX_TOTAL / nb > 1 ? LT_CTX_TOTAL / nb : 1;
  if (ctxmax > 1 && (size_t)nb * nv * ctxmax > LT_CTX_BUDGET)
  {
    ctxmax = (int)(LT_CTX_BUDGET / ((size_t)nb * nv + 1));
    if (ctxmax < 2)
      ctxmax = 1;
  }
  int nctx = ctxmax;
  uint8_t *cst = tcc_mallocz((size_t)nb * ctxmax * nv + 1);
  uint32_t *ck = tcc_mallocz((size_t)nb * ctxmax * sizeof(uint32_t) + 1);
  uint32_t *cv = tcc_mallocz((size_t)nb * ctxmax * sizeof(uint32_t) + 1);
  uint8_t *cn = tcc_mallocz(nb + 1);
  uint8_t *snap = tcc_malloc((size_t)ctxmax * nv + 1);
  uint32_t snk[LT_MAX_CTX], snv[LT_MAX_CTX];
  lt_entry_state(&c, cst);
  cn[0] = 1;

  /* forward to a fixpoint: states only gain bits, contexts only lose known
     predicates (past LT_CTX_ITERS walks each block's contexts are joined) */
  for (int changed = 1, iter = 0; changed; iter++)
  {
    changed = 0;
    if (iter == LT_CTX_ITERS && nctx > 1)
    {
      for (int b = 0; b < nb; b++)
        changed |= lt_ctx_collapse(cst + (size_t)b * ctxmax * nv, ck + b * ctxmax, cv + b * ctxmax, &cn[b], nv);
      nctx = 1;
    }
    lt_walk_begin(&c);
    for (int r = 0; r < c.cfg->rpo_count; r++)
    {
      int b = c.cfg->rpo_order[r];
      IRBasicBlock *bb = &c.cfg->blocks[b];
      int nm = cn[b];
      if (!reached[b])
        continue;
      /* the contexts as they are now: merging may change a self loop's */
      memcpy(snap, cst + (size_t)b * ctxmax * nv, (size_t)nm * nv);
      memcpy(snk, ck + b * ctxmax, nm * sizeof(uint32_t));
      memcpy(snv, cv + b * ctxmax, nm * sizeof(uint32_t));
      for (int m = 0; m < nm; m++)
      {
        c.pk = snk[m], c.pv = snv[m];
        lt_block(&c, b, snap + (size_t)m * nv, out);
        for (int s = 0; s < bb->num_succs; s++)
        {
          int t = bb->succs[s];
          if (!c.efeas[s])
            continue;
          changed |= lt_ctx_merge(cst + (size_t)t * ctxmax * nv, ck + t * ctxmax, cv + t * ctxmax, &cn[t], nctx, nv,
                                  c.eknown[s], c.eval[s], out + (size_t)s * nv);
          if (!reached[t])
            reached[t] = 1, changed = 1;
        }
      }
    }
  }

  /* then once more, in source order, saying what was found; -Werror=lifetime
     fails the compile but still shows every finding and its notes.  A block
     reached by several contexts reports on their join (what each knows in
     common); leaving the function past its last jump takes the contexts
     that can. */
  uint8_t *joined = tcc_malloc(nv + 1), *xjoin = tcc_malloc(nv + 1);
  c.report = 1;
  tcc_state->diag_noabort++;
  lt_walk_begin(&c);
  for (int b = 0; b < nb; b++)
  {
    IRBasicBlock *bb = &c.cfg->blocks[b];
    uint8_t *bs = cst + (size_t)b * ctxmax * nv;
    uint32_t *bk = ck + b * ctxmax, *bv = cv + b * ctxmax;
    if (!reached[b] || !cn[b])
      continue;
    memcpy(joined, bs, nv);
    c.pk = bk[0], c.pv = bv[0];
    for (int m = 1; m < cn[b]; m++)
    {
      for (int v = 0; v < nv; v++)
        joined[v] |= bs[(size_t)m * nv + v];
      c.pk &= bk[m] & ~(bv[m] ^ c.pv);
      c.pv &= c.pk;
    }
    c.exit_mode = 0;
    if (cn[b] > 1 && lt_leaves_past_end(&c, bb))
    {
      int any = 0;
      c.report = 0;
      c.exit_mode = 3;
      for (int m = 0; m < cn[b]; m++)
      {
        uint32_t jk = c.pk, jv = c.pv;
        c.pk = bk[m], c.pv = bv[m];
        lt_block(&c, b, bs + (size_t)m * nv, out);
        c.pk = jk, c.pv = jv;
        if (!c.efeas[bb->num_succs])
          continue;
        for (int v = 0; v < nv; v++)
          xjoin[v] = (any ? xjoin[v] : 0) | out[(size_t)bb->num_succs * nv + v];
        any = 1;
      }
      c.report = 1;
      c.exit_mode = any ? 1 : 2;
      c.exit_join = xjoin;
    }
    lt_block(&c, b, joined, NULL);
    c.exit_mode = 0;
  }
  tcc_state->diag_noabort--;

  tcc_free(joined);
  tcc_free(xjoin);
  tcc_free(cst);
  tcc_free(ck);
  tcc_free(cv);
  tcc_free(cn);
  tcc_free(snap);
  tcc_free(c.eknown);
  tcc_free(c.eval);
  tcc_free(c.efeas);
  tcc_free(reached);
  tcc_free(out);
  tcc_free(c.scratch);
  tcc_free(c.ev_first);
  tcc_free(c.move_line);
  tcc_free(c.move_drop);
  tcc_free(c.arg_bound);
  tcc_free(c.borrow);
  tcc_free(c.vbset);
  tcc_free(c.vbcnt);
  tcc_free(c.vbsaid);
  tcc_free(c.gbset);
  tcc_free(c.gbcnt);
  tcc_free(c.vbrow);
  tcc_free(c.gbrow);
  tcc_free(c.brow_own);
  tcc_free(c.brow_n);
  tcc_free(c.brow_at);
  tcc_free(c.so_untracked);
  tcc_free(c.so_tfa);
  tcc_free(c.so_tld);
  tcc_free(c.so_sret);
  tcc_free(c.sbset);
  tcc_free(c.sbcnt);
  tcc_free(c.sbrow);
  tcc_free(c.so_tcw);
  tcc_free(c.so_cown);
  tcc_free(c.so_ncown);
  tcc_free(c.bind_owner);
  tcc_free(c.sres_first);
  tcc_free(c.take_call);
  tcc_free(c.taken_by);
  tcc_free(c.call_bsrc);
  tcc_free(c.call_tvar);
  tcc_free(c.vcall);
  tcc_free(c.vdefs);
  tcc_free(c.vstat);
  tcc_free(c.stcalls);
  tcc_free(c.cl_var);
  tcc_free(c.cl_x);
  tcc_free(c.cl_pol);
  tcc_free(c.link);
  tcc_free(c.lslot);
  tcc_free(c.okind);
  tcc_free(c.osilent);
  tcc_free(c.osym);
  tcc_free(c.ooff);
  tcc_free(c.osize);
  tcc_free(c.otok);
  tcc_free(c.oesize);
  tcc_free(c.inl);
  tcc_free(c.tcall);
  tcc_free(c.tcall_gen);
  tcc_free(c.tout);
  tcc_free(c.out_call);
  tcc_free(c.cur);
  tcc_free(c.alias);
  tcc_free(c.palias);
  tcc_free(c.tnull);
  tcc_free(c.tderiv);
  tcc_free(c.alias_gen);
  tcc_free(c.tbase);
  tcc_free(c.toff);
  tcc_free(c.taddr);
  tcc_free(c.trec);
  tcc_free(c.call_at);
  tcc_free(c.pend);
  tcc_free(c.rarg);
  tcc_free(c.tglob);
  tcc_free(c.g_alias);
  tcc_free(c.g_known);
  tcc_free(c.tline);
  for (int k = 0; k < c.npaths; k++)
    tcc_free(c.pname[k]);
  tcc_free(c.pname);
  tcc_free(c.pbase);
  tcc_free(c.poff);
  tcc_free(c.pidx);
  tcc_free(c.xbase);
  tcc_free(c.pmark);
  tcc_free(c.map_var);
  tcc_ir_cfg_free(c.cfg);
  lt_free_records(ir);
}

/* ------------------------------------------------------------------------ */
/* -Wuninitialized / -Wmaybe-uninitialized: a local declared without a value,
 * read on a path where nothing stored it.  The same raw IR and CFG as the
 * lifetime check, so the same answer at every -O level.  Variables go 32 at a
 * time, two bits each per block (may be uninitialised, may be set): the memory
 * stays a few bytes per block whatever the function's size. */

int tcc_ir_un_wanted(void)
{
  return tcc_state->ir && ((((tcc_state->warn_uninitialized | tcc_state->warn_maybe_uninitialized) & WARN_ON) &&
                            !tcc_state->warn_none) ||
                           tcc_ir_lt_active());
}

void tcc_ir_un_add_var(TCCIRState *ir, int32_t vreg, Sym *sym)
{
  IRLtVar *v;
  IRLtEvent *e;
  if (TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_VAR)
    return;
  ir->un_vars = lt_grow(ir->un_vars, ir->un_nb_vars, sizeof(IRLtVar));
  v = &ir->un_vars[ir->un_nb_vars];
  memset(v, 0, sizeof *v);
  v->vreg = vreg;
  v->tok = sym->v;
  v->line = file ? file->line_num : 0;
  v->silent = tcc_state->in_inline_expansion != 0;
  v->arg_index = -1;
  /* yasc enforces initialization: the reads are errors (under -Wlifetime),
     and a variable whose address is taken is followed too */
  v->yinit = tcc_ir_lt_active();
  v->yundef = v->yinit && yasc_decl_undefined;
  ir->un_events = lt_grow(ir->un_events, ir->un_nb_events, sizeof(IRLtEvent));
  e = &ir->un_events[ir->un_nb_events++];
  e->idx = ir->next_instruction_index;
  e->var = ir->un_nb_vars++;
  e->line = v->line;
}

typedef struct UnCtx
{
  TCCIRState *ir;
  IRCFG *cfg;
  int32_t *map; /* VAR position -> variable */
  int nmap;
  int base;     /* first variable of the chunk */
  int *ev_first;
  int report;
  uint32_t u, s; /* walking a block: may be uninitialised / may be set */
  /* yasc's enforced initialization follows a variable whose address is taken */
  const LtCalls *calls;
  int32_t *tvar; /* temp position -> the yasc variable whose address it holds, -1 */
  uint8_t *tuse; /* ... 1: it only goes to `out` parameters, 2: somewhere else too */
  int ntemp;
} UnCtx;

/* the chunk bit of the variable an operand names, 0 if none */
static uint32_t un_bit(UnCtx *c, IROperand op, int *var)
{
  int32_t vreg;
  int pos, v;
  if (irop_is_none(op) || (vreg = irop_get_vreg(op)) < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_VAR)
    return 0;
  pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (pos >= c->nmap || (v = c->map[pos]) < 0 || v < c->base || v >= c->base + 32)
    return 0;
  *var = v;
  return 1u << (v - c->base);
}

/* v's address is taken (yasc): whatever gets it may read it, so it must have
   a value already -- unless it said `= undefined`.  From here on it counts
   as set. */
static void un_addr(UnCtx *c, uint32_t bit, int v, int line)
{
  const IRLtVar *var = &c->ir->un_vars[v];
  if (!(c->u & bit))
    return;
  if (c->report && !var->silent && !var->yundef)
    tcc_warning_c(warn_lifetime)("%i:the address of '%s' is taken where it may have no value: set it first, "
                                 "pass it to an 'out' parameter, or declare it '= undefined'",
                                 line, get_tok_str(var->tok, NULL));
  c->u &= ~bit;
  c->s |= bit;
}

static void un_read(UnCtx *c, IROperand op, int line)
{
  int v;
  uint32_t bit = un_bit(c, op, &v);
  if (!bit)
    return;
  if (lt_is_addr(op))
  {
    if (c->ir->un_vars[v].yinit)
      un_addr(c, bit, v, line);
    return;
  }
  if (!(c->u & bit))
    return;
  if (c->report && !c->ir->un_vars[v].silent)
  {
    const char *name = get_tok_str(c->ir->un_vars[v].tok, NULL);
    /* yasc enforces initialization: an error in the safe default, at every
       -O level (the raw IR), which only unsafe { } lowers */
    if (c->ir->un_vars[v].yinit)
      tcc_warning_c(warn_lifetime)(c->s & bit ? "%i:'%s' may be used uninitialized: yasc wants a value on every path"
                                              : "%i:'%s' is used uninitialized",
                                   line, name);
    else if (c->s & bit)
      tcc_warning_c(warn_maybe_uninitialized)("%i:'%s' may be used uninitialized", line, name);
    else
      tcc_warning_c(warn_uninitialized)("%i:'%s' is used uninitialized", line, name);
  }
  /* once: from here on it counts as set */
  c->u &= ~bit;
  c->s |= bit;
}

/* FUNCPARAMVAL q passes its argument to an `out T *` parameter */
static int un_out_arg(UnCtx *c, IRQuadCompact *q)
{
  int64_t enc;
  int id, k;
  const IRLtCall *call;
  if (q->op != TCCIR_OP_FUNCPARAMVAL || !c->calls)
    return 0;
  enc = irop_get_imm64_ex(c->ir, tcc_ir_op_get_src2(c->ir, q));
  id = TCCIR_DECODE_CALL_ID(enc);
  k = TCCIR_DECODE_PARAM_IDX(enc);
  call = id >= 0 && id < c->calls->n ? &c->calls->v[id] : NULL;
  return call && call->noted && (k -= call->implicit) >= 0 && k < 32 && k < call->nparams && ((call->out >> k) & 1);
}

/* ASSIGN / STORE q binds its src1 to an inlined callee's `out` parameter */
static int un_out_bind(UnCtx *c, IRQuadCompact *q, IROperand dest)
{
  int32_t dv;
  int pos;
  if ((q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE) || !c->ir->un_outparam || irop_is_none(dest) ||
      !irop_dest_defines_vreg(dest) || (dv = irop_get_vreg(dest)) < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_VAR)
    return 0;
  pos = TCCIR_DECODE_VREG_POSITION(dv);
  return pos < c->ir->un_outparam_words * 32 && ((c->ir->un_outparam[pos >> 5] >> (pos & 31)) & 1);
}

/* the yasc variable (any chunk) whose address OP is, directly (&v, or LEA's
   src1 when LEA is set) or through a temp that holds it; -1 none */
static int un_addr_var(UnCtx *c, IRQuadCompact *q, IROperand op, int lea)
{
  int32_t vreg;
  int pos, v;
  if (irop_is_none(op) || (vreg = irop_get_vreg(op)) < 0)
    return -1;
  pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP)
    return c->tvar && pos < c->ntemp && !op.is_lval ? c->tvar[pos] : -1;
  if (TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_VAR || pos >= c->nmap || (v = c->map[pos]) < 0 ||
      !c->ir->un_vars[v].yinit)
    return -1;
  return lt_is_addr(op) || (lea && lt_lea_of_local(q, op)) ? v : -1;
}

/* the chunk bit of variable v, 0 if it is in another chunk */
static uint32_t un_var_bit(UnCtx *c, int v)
{
  return v >= c->base && v < c->base + 32 ? 1u << (v - c->base) : 0;
}

/* yasc: an instruction that takes a variable's address -- or passes it to an
   `out` parameter, which sets it.  1 when it handled the instruction. */
static int un_addr_insn(UnCtx *c, IRQuadCompact *q, IROperand dest, IROperand src1)
{
  int v;
  uint32_t bit;
  if (!c->tvar)
    return 0;
  /* t = &v: an address only for `out` parameters waits for them */
  if ((q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASSIGN) && !irop_is_none(dest) && irop_dest_defines_vreg(dest) &&
      irop_get_vreg(dest) >= 0 && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
  {
    int t = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
    if (t >= c->ntemp || (v = c->tvar[t]) < 0 || un_addr_var(c, q, src1, q->op == TCCIR_OP_LEA) != v)
      return 0;
    if (c->tuse[t] != 1 && (bit = un_var_bit(c, v)))
      un_addr(c, bit, v, q->line_num);
    return 1;
  }
  /* *t = value with t = &v (__builtin_add_overflow's result): a store to v */
  if (q->op == TCCIR_OP_STORE && !irop_is_none(dest) && dest.is_lval && (v = un_addr_var(c, q, dest, 0)) < 0 &&
      irop_get_vreg(dest) >= 0 && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP &&
      TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)) < c->ntemp &&
      (v = c->tvar[TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest))]) >= 0)
  {
    un_read(c, src1, q->line_num);
    if ((bit = un_var_bit(c, v)))
    {
      c->u &= ~bit;
      c->s |= bit;
    }
    return 1;
  }
  /* &v to an `out` parameter (a call's, or an inlined callee's): set */
  if ((un_out_arg(c, q) || un_out_bind(c, q, dest)) && (v = un_addr_var(c, q, src1, 0)) >= 0)
  {
    if ((bit = un_var_bit(c, v)))
    {
      c->u &= ~bit;
      c->s |= bit;
    }
    return q->op == TCCIR_OP_FUNCPARAMVAL;
  }
  return 0;
}

/* declarations before instruction i: those variables are uninitialised
   (again, for one declared in a loop body) */
static void un_declare(UnCtx *c, int i, uint32_t *u, uint32_t *s)
{
  for (int k = c->ev_first[i]; k < c->ev_first[i + 1]; k++)
  {
    int ev = c->ir->un_events[k].var;
    if (ev >= c->base && ev < c->base + 32)
    {
      *u |= 1u << (ev - c->base);
      *s &= ~(1u << (ev - c->base));
    }
  }
}

/* The declarations at a block's first instruction are applied on the edges
   into it from code written before them (un_edge), not here: a loop header
   right after `int x;` would otherwise forget, on every back edge, what the
   body stored. */
static void un_block(UnCtx *c, int b)
{
  TCCIRState *ir = c->ir;
  IRBasicBlock *bb = &c->cfg->blocks[b];
  for (int i = bb->start_idx; i < bb->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    int v;
    uint32_t bit;
    if (i > bb->start_idx)
      un_declare(c, i, &c->u, &c->s);
    if (un_addr_insn(c, q, dest, cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none()))
      continue;
    if (cfg->has_src1)
      un_read(c, tcc_ir_op_get_src1(ir, q), q->line_num);
    if (cfg->has_src2)
      un_read(c, tcc_ir_op_get_src2(ir, q), q->line_num);
    if (irop_is_none(dest))
      continue;
    if (!irop_dest_defines_vreg(dest))
      un_read(c, dest, q->line_num);
    else if ((bit = un_bit(c, dest, &v)))
    {
      c->u &= ~bit;
      c->s |= bit;
    }
  }
}

/* before the lifetime check frees its variables: which VARs are inlined
   callees' `out` parameters (&v bound to one sets v) */
static void un_note_outparams(TCCIRState *ir)
{
  int any = 0;
  for (int v = 0; v < ir->un_nb_vars && !any; v++)
    any = ir->un_vars[v].yinit;
  if (!any)
    return;
  for (int i = 0; i < ir->lt_nb_vars; i++)
  {
    const IRLtVar *lv = &ir->lt_vars[i];
    int pos;
    if (lv->arg_index < 0 || !lv->outarg || TCCIR_DECODE_VREG_TYPE(lv->vreg) != TCCIR_VREG_TYPE_VAR)
      continue;
    pos = TCCIR_DECODE_VREG_POSITION(lv->vreg);
    if (pos >= ir->un_outparam_words * 32)
    {
      int words = pos / 32 + 1;
      ir->un_outparam = tcc_realloc(ir->un_outparam, words * sizeof(uint32_t));
      memset(ir->un_outparam + ir->un_outparam_words, 0, (words - ir->un_outparam_words) * sizeof(uint32_t));
      ir->un_outparam_words = words;
    }
    ir->un_outparam[pos >> 5] |= 1u << (pos & 31);
  }
}

static void un_check(TCCIRState *ir, const LtCalls *calls)
{
  UnCtx c;
  int nv = ir->un_nb_vars, n = ir->next_instruction_index;
  memset(&c, 0, sizeof c);
  c.ir = ir;
  c.calls = calls;
  if (!n)
    goto done;

  for (int v = 0; v < nv; v++)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(ir->un_vars[v].vreg);
    if (pos >= c.nmap)
      c.nmap = pos + 1;
  }
  c.map = tcc_malloc((c.nmap + 1) * sizeof(int32_t));
  memset(c.map, 0xff, (c.nmap + 1) * sizeof(int32_t));
  for (int v = 0; v < nv; v++)
    if (ir->un_vars[v].yinit || !lt_vreg_addrtaken(ir, ir->un_vars[v].vreg))
      c.map[TCCIR_DECODE_VREG_POSITION(ir->un_vars[v].vreg)] = v;

  /* a variable whose address is taken may be set through it -- in yasc it
     must have a value where its address is taken (un_addr), unless the
     address only goes to `out` parameters */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand ops[3];
    int nops = 0;
    if (cfg->has_dest)
      ops[nops++] = tcc_ir_op_get_dest(ir, q);
    if (cfg->has_src1)
      ops[nops++] = tcc_ir_op_get_src1(ir, q);
    if (cfg->has_src2)
      ops[nops++] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < nops; k++)
    {
      int32_t vreg = irop_get_vreg(ops[k]);
      int pos;
      int addr = lt_is_addr(ops[k]) || (k == nops - 1 - cfg->has_src2 && cfg->has_src1 && lt_lea_of_local(q, ops[k]));
      if (!irop_is_none(ops[k]) && vreg >= 0 && addr && TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_VAR &&
          (pos = TCCIR_DECODE_VREG_POSITION(vreg)) < c.nmap && c.map[pos] >= 0 && !ir->un_vars[c.map[pos]].yinit)
        c.map[pos] = -1;
    }
  }

  /* yasc: the temps holding a variable's address, and where they go */
  for (int v = 0; v < nv && !c.tvar; v++)
    if (ir->un_vars[v].yinit && c.map[TCCIR_DECODE_VREG_POSITION(ir->un_vars[v].vreg)] == v)
    {
      c.ntemp = ir->next_temporary_variable;
      c.tvar = tcc_malloc((c.ntemp + 1) * sizeof(int32_t));
      memset(c.tvar, 0xff, (c.ntemp + 1) * sizeof(int32_t));
      c.tuse = tcc_mallocz(c.ntemp + 1);
    }
  if (c.tvar)
  {
    uint8_t *ndef = tcc_mallocz(c.ntemp + 1);
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      const IRRegistersConfig *cfg = &irop_config[q->op];
      IROperand d = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
      int32_t dv;
      int t, v;
      if (irop_is_none(d) || !irop_dest_defines_vreg(d) || (dv = irop_get_vreg(d)) < 0 ||
          TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP || (t = TCCIR_DECODE_VREG_POSITION(dv)) >= c.ntemp)
        continue;
      if (ndef[t] < 2)
        ndef[t]++;
      if ((q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASSIGN) && cfg->has_src1 &&
          (v = un_addr_var(&c, q, tcc_ir_op_get_src1(ir, q), q->op == TCCIR_OP_LEA)) >= 0)
        c.tvar[t] = v;
    }
    for (int t = 0; t < c.ntemp; t++)
      if (ndef[t] > 1)
        c.tvar[t] = -1;
    tcc_free(ndef);
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      const IRRegistersConfig *cfg = &irop_config[q->op];
      IROperand d = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
      IROperand ops[3];
      int nops = 0;
      if (cfg->has_src1)
        ops[nops++] = tcc_ir_op_get_src1(ir, q);
      if (cfg->has_src2)
        ops[nops++] = tcc_ir_op_get_src2(ir, q);
      if (!irop_is_none(d) && !irop_dest_defines_vreg(d))
        ops[nops++] = d;
      for (int k = 0; k < nops; k++)
      {
        int32_t vr = irop_is_none(ops[k]) ? -1 : irop_get_vreg(ops[k]);
        int t;
        if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || (t = TCCIR_DECODE_VREG_POSITION(vr)) >= c.ntemp ||
            c.tvar[t] < 0)
          continue;
        /* 1: it only goes to `out` parameters, or is stored through */
        c.tuse[t] |= (k == 0 && cfg->has_src1 && !ops[k].is_lval && (un_out_arg(&c, q) || un_out_bind(&c, q, d))) ||
                             (k == nops - 1 && q->op == TCCIR_OP_STORE && !irop_is_none(d) && !irop_dest_defines_vreg(d) &&
                              ops[k].is_lval)
                         ? 1
                         : 2;
      }
    }
  }

  c.cfg = tcc_ir_cfg_build(ir);
  if (!c.cfg)
    goto done;
  tcc_ir_cfg_compute_rpo(c.cfg);
  int nb = c.cfg->num_blocks;

  for (int k = 0; k < ir->un_nb_events; k++)
    if (ir->un_events[k].idx > n)
      ir->un_events[k].idx = n;
  lt_sort_events(ir->un_events, ir->un_nb_events);
  c.ev_first = tcc_mallocz((n + 2) * sizeof(int));
  for (int k = 0; k < ir->un_nb_events; k++)
    c.ev_first[ir->un_events[k].idx + 1]++;
  for (int i = 0; i <= n; i++)
    c.ev_first[i + 1] += c.ev_first[i];

  uint32_t *in_u = tcc_malloc(nb * sizeof(uint32_t)), *in_s = tcc_malloc(nb * sizeof(uint32_t));
  uint8_t *reached = tcc_malloc(nb);
  tcc_state->diag_noabort++;
  for (c.base = 0; c.base < nv; c.base += 32)
  {
    memset(in_u, 0, nb * sizeof(uint32_t));
    memset(in_s, 0, nb * sizeof(uint32_t));
    memset(reached, 0, nb);
    reached[0] = 1;
    un_declare(&c, c.cfg->blocks[0].start_idx, &in_u[0], &in_s[0]);
    c.report = 0;
    for (int changed = 1; changed;)
    {
      changed = 0;
      for (int r = 0; r < c.cfg->rpo_count; r++)
      {
        int b = c.cfg->rpo_order[r];
        IRBasicBlock *bb = &c.cfg->blocks[b];
        if (!reached[b])
          continue;
        c.u = in_u[b];
        c.s = in_s[b];
        un_block(&c, b);
        for (int k = 0; k < bb->num_succs; k++)
        {
          int t = bb->succs[k];
          uint32_t eu = c.u, es = c.s;
          /* a forward edge passes the declarations at the target's start */
          if (c.cfg->blocks[t].start_idx > bb->start_idx)
            un_declare(&c, c.cfg->blocks[t].start_idx, &eu, &es);
          uint32_t nu = in_u[t] | eu, ns = in_s[t] | es;
          if (nu != in_u[t] || ns != in_s[t] || !reached[t])
            in_u[t] = nu, in_s[t] = ns, reached[t] = 1, changed = 1;
        }
      }
    }
    c.report = 1;
    for (int b = 0; b < nb; b++)
      if (reached[b])
      {
        c.u = in_u[b];
        c.s = in_s[b];
        un_block(&c, b);
      }
  }
  tcc_state->diag_noabort--;
  tcc_free(in_u);
  tcc_free(in_s);
  tcc_free(reached);
  tcc_ir_cfg_free(c.cfg);
done:
  tcc_free(c.ev_first);
  tcc_free(c.map);
  tcc_free(c.tvar);
  tcc_free(c.tuse);
  un_free_records(ir);
}

/* ------------------------------------------------------------------------ */
/* -Wreturn-local-addr: `return &x;`, `return buf;`, `return &s.m;`,
 * `return &a[2];`, a compound literal -- an address in this call's frame,
 * gone when it returns.  The frontend has already put &x into a temp, so this
 * reads the raw IR: one walk, a temp holding a frame address (that address
 * plus an offset too, or the result of a call that borrows from an argument
 * given one: `return f(buf);`) reaching RETURNVALUE.  A pointer variable
 * holding one is lt_dangling_check's. */

static void lt_return_local_check(TCCIRState *ir, const LtCalls *calls)
{
  int n = ir->next_instruction_index, nt = ir->next_temporary_variable;
  int32_t *frame;
  uint8_t *callfr;
  if (!n)
    return;
  /* temp position -> holds a frame address: 1 straight, 2 + call id through
     that call's result */
  frame = tcc_mallocz((nt + 1) * sizeof(int32_t));
  callfr = tcc_mallocz(calls->n + 1); /* call id -> an argument its result borrows from is one */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    IROperand src1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
    IROperand src2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    int f1 = 0, f2 = 0, id;
    for (int k = 0; k < 2; k++)
    {
      IROperand op = k ? src2 : src1;
      int32_t vreg;
      int f = 0;
      if (irop_is_none(op))
        continue;
      if (lt_is_addr(op) || (!k && lt_lea_of_local(q, op)))
        f = 1;
      else if ((vreg = irop_get_vreg(op)) >= 0 && TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP &&
               irop_is_vreg_value(op) && TCCIR_DECODE_VREG_POSITION(vreg) < nt)
        f = frame[TCCIR_DECODE_VREG_POSITION(vreg)];
      if (k)
        f2 = f;
      else
        f1 = f;
    }
    if (q->op == TCCIR_OP_RETURNVALUE && f1 == 1)
      tcc_warning_c(warn_return_local_addr)("%i:'%s' returns the address of a local variable", q->line_num,
                                            funcname);
    else if (q->op == TCCIR_OP_RETURNVALUE && f1 >= 2)
    {
      int callee = calls->v[f1 - 2].callee;
      tcc_warning_c(warn_return_local_addr)("%i:'%s' returns the address of a local variable (through the result of "
                                            "%s%s%s)", q->line_num, funcname, callee ? "'" : "",
                                            callee ? get_tok_str(callee, NULL) : "a call", callee ? "'" : "");
    }
    if (q->op == TCCIR_OP_FUNCPARAMVAL && f1 && (id = lt_borrowed_arg(ir, calls, src2)) >= 0)
      callfr[id] = 1;
    if (!irop_is_none(dest) && irop_dest_defines_vreg(dest) &&
        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP &&
        TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)) < nt)
    {
      int f = 0;
      if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA)
        f = f1;
      else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
        f = f1 ? f1 : q->op == TCCIR_OP_ADD ? f2 : 0;
      else if (q->op == TCCIR_OP_FUNCCALLVAL && cfg->has_src2 && (id = lt_call_id(ir, calls, src2)) >= 0 && callfr[id])
        f = 2 + id;
      frame[TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest))] = f;
    }
  }
  tcc_free(frame);
  tcc_free(callfr);
}

/* ------------------------------------------------------------------------ */
/* -Wdangling-pointer, and -Wreturn-local-addr through a variable: a pointer
 * variable holding an address in this call's frame -- `p = &x;`, `p = buf;`
 * -- returned, or stored into a global or through a parameter, where it
 * outlives the function.  A bit per such variable (may hold a frame address),
 * forward over the raw-IR CFG, 32 variables at a time. */

int tcc_ir_dp_wanted(void)
{
  TCCState *s1 = tcc_state;
  return tcc_state->ir && !s1->warn_none && ((s1->warn_dangling_pointer | s1->warn_return_local_addr) & WARN_ON);
}

void tcc_ir_dp_add_name(TCCIRState *ir, int32_t vreg, Sym *sym)
{
  IRLtVar *v;
  ir->dp_names = lt_grow(ir->dp_names, ir->dp_nb_names, sizeof(IRLtVar));
  v = &ir->dp_names[ir->dp_nb_names++];
  memset(v, 0, sizeof *v);
  v->vreg = vreg;
  v->tok = sym->v;
  v->silent = tcc_state->in_inline_expansion != 0;
}

static const IRLtVar *dp_name(TCCIRState *ir, int32_t vreg)
{
  for (int i = 0; i < ir->dp_nb_names; i++)
    if (ir->dp_names[i].vreg == vreg)
      return &ir->dp_names[i];
  return NULL;
}

/* temp flags, block-local: a kind, and in bits 8.. an index */
#define DP_FRAME 1  /* a frame address, straight from &x / an array */
#define DP_VIA 2    /* a frame address read from a followed variable (index: the variable) */
#define DP_PARAM 4  /* a parameter's pointer, or into what it points at (index: its position) */
#define DP_KIND 7
#define DP_INDEX(f) ((f) >> 8)

/* A global written with a frame address: followed like a variable, reported
   when one may still be there as the function returns (`g = &x; use(); g =
   0;` is fine). */
typedef struct DpLoc
{
  Sym *sym;
  int32_t addend;
  int line;       /* the store that left the frame address (reporting walk) */
  uint8_t reported;
} DpLoc;

typedef struct DpCtx
{
  TCCIRState *ir;
  IRCFG *cfg;
  int32_t *vars; /* the followed variables' vregs; then the globals (locs) */
  int nv, base, report;
  DpLoc *locs;
  int nlocs;
  uint32_t f;    /* walking a block: may hold a frame address */
  int ne_succ;   /* after a block: the successor where `p != &x` holds (-1 none) */
  uint32_t ne_clear; /* ... and the bit it clears there */
  uint32_t *tflag; /* temp position -> DP_* | var << 8 */
  uint32_t *tgen;
  uint32_t gen;
  int ntemps;
  const LtCalls *calls;
  uint32_t *callv; /* call id -> a frame address passed where its result borrows from (DP_* | var << 8) */
  Sym *func;
} DpCtx;

static int dp_var(DpCtx *c, int32_t vreg)
{
  for (int i = 0; i < c->nv; i++)
    if (c->vars[i] == vreg)
      return i;
  return -1;
}

static int dp_frame_source(const IRQuadCompact *q, IROperand op, int first)
{
  return !irop_is_none(op) && (lt_is_addr(op) || (first && lt_lea_of_local(q, op)));
}

/* a parameter's value: DP_PARAM with its position */
static uint32_t dp_param(int32_t vreg)
{
  return DP_PARAM | ((uint32_t)TCCIR_DECODE_VREG_POSITION(vreg) << 8);
}

/* what an operand's value is: DP_* flags (| var << 8 for DP_VIA) */
static uint32_t dp_value(DpCtx *c, const IRQuadCompact *q, IROperand op, int first)
{
  int32_t vreg;
  int v;
  if (irop_is_none(op))
    return 0;
  if (dp_frame_source(q, op, first))
    return DP_FRAME;
  if ((vreg = irop_get_vreg(op)) < 0 || !irop_is_vreg_value(op))
    return 0;
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
  case TCCIR_VREG_TYPE_TEMP:
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vreg);
    return pos < c->ntemps && c->tgen[pos] == c->gen ? c->tflag[pos] : 0;
  }
  case TCCIR_VREG_TYPE_PARAM:
    return dp_param(vreg);
  case TCCIR_VREG_TYPE_VAR:
    v = dp_var(c, vreg);
    if (v >= c->base && v < c->base + 32 && (c->f & (1u << (v - c->base))))
      return DP_VIA | ((uint32_t)v << 8);
    return 0;
  }
  return 0;
}


/* the memory a destination operand writes: through a parameter's pointer, a
   global's -- or neither (0) */
static int dp_lasting_dest(DpCtx *c, IROperand dest, const char **what, int32_t *who)
{
  int32_t vreg;
  if (irop_is_none(dest) || irop_dest_defines_vreg(dest))
    return 0;
  if (irop_get_tag(dest) == IROP_TAG_SYMREF && dest.is_lval)
  {
    Sym *sym = irop_get_sym_ex(c->ir, dest);
    *what = sym ? get_tok_str(sym->v, NULL) : "?";
    return 1;
  }
  if (irop_get_tag(dest) == IROP_TAG_VREG && dest.is_lval && (vreg = irop_get_vreg(dest)) >= 0)
  {
    if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM)
    {
      *who = vreg;
      return 2;
    }
    if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vreg);
      if (pos < c->ntemps && c->tgen[pos] == c->gen && (c->tflag[pos] & DP_KIND) == DP_PARAM)
      {
        *who = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, DP_INDEX(c->tflag[pos]));
        return 2;
      }
    }
  }
  return 0;
}

/* the followed global DEST writes (its item index: nv + location), or -1 */
static int dp_loc(DpCtx *c, IROperand dest, int add)
{
  Sym *sym;
  IRPoolSymref *ref;
  int32_t addend;
  if (irop_is_none(dest) || irop_get_tag(dest) != IROP_TAG_SYMREF || !dest.is_lval ||
      !(sym = irop_get_sym_ex(c->ir, dest)))
    return -1;
  ref = irop_get_symref_ex(c->ir, dest);
  addend = ref ? ref->addend : 0;
  for (int i = 0; i < c->nlocs; i++)
    if (c->locs[i].sym == sym && c->locs[i].addend == addend)
      return c->nv + i;
  if (!add)
    return -1;
  c->locs = lt_grow(c->locs, c->nlocs, sizeof(DpLoc));
  memset(&c->locs[c->nlocs], 0, sizeof(DpLoc));
  c->locs[c->nlocs].sym = sym;
  c->locs[c->nlocs].addend = addend;
  return c->nv + c->nlocs++;
}

static const char *dp_var_name(DpCtx *c, int v)
{
  const IRLtVar *n = dp_name(c->ir, c->vars[v]);
  return n ? get_tok_str(n->tok, NULL) : "a pointer";
}

static void dp_block(DpCtx *c, int b)
{
  TCCIRState *ir = c->ir;
  IRBasicBlock *bb = &c->cfg->blocks[b];
  c->gen++;
  c->ne_succ = -1;
  c->ne_clear = 0;
  for (int i = bb->start_idx; i < bb->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    IROperand src1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
    IROperand src2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    uint32_t v1 = dp_value(c, q, src1, 1), v2 = dp_value(c, q, src2, 0);
    int id;

    if (q->op == TCCIR_OP_RETURNVALUE && (v1 & DP_KIND) == DP_VIA && c->report)
      tcc_warning_c(warn_return_local_addr)("%i:'%s' may return the address of a local variable (held in '%s')",
                                            q->line_num, funcname, dp_var_name(c, DP_INDEX(v1)));
    /* a straight frame address is reported with the first chunk, a variable's
       with its own */
    if (q->op == TCCIR_OP_STORE && c->report &&
        ((v1 & DP_KIND) == DP_VIA || ((v1 & DP_KIND) == DP_FRAME && c->base == 0)))
    {
      const char *what = NULL;
      int32_t who = -1;
      int kind = dp_lasting_dest(c, dest, &what, &who);
      if (kind == 2)
      {
        const IRLtVar *n = who >= 0 ? dp_name(ir, who) : NULL;
        if (n)
          tcc_warning_c(warn_dangling_pointer)("%i:the address of a local variable is stored through '%s', which "
                                               "outlives '%s'", q->line_num, get_tok_str(n->tok, NULL), funcname);
        else
          tcc_warning_c(warn_dangling_pointer)("%i:the address of a local variable is stored through a parameter, "
                                               "which outlives '%s'", q->line_num, funcname);
      }
    }

    /* a store into a followed global: it holds a frame address from here, or not */
    if (q->op == TCCIR_OP_STORE)
    {
      int l = dp_loc(c, dest, 0);
      if (l >= c->base && l < c->base + 32)
      {
        uint32_t bit = 1u << (l - c->base);
        if ((v1 & DP_KIND) == DP_FRAME || (v1 & DP_KIND) == DP_VIA)
        {
          c->f |= bit;
          if (c->report)
            c->locs[l - c->nv].line = q->line_num;
        }
        else
          c->f &= ~bit;
      }
    }

    /* an argument the call's view result points into: the result is what it was */
    if (q->op == TCCIR_OP_FUNCPARAMVAL && ((v1 & DP_KIND) == DP_FRAME || (v1 & DP_KIND) == DP_VIA) &&
        (id = lt_borrowed_arg(ir, c->calls, src2)) >= 0 && (c->callv[id] & DP_KIND) != DP_VIA)
      c->callv[id] = v1;

    if (irop_is_none(dest) || !irop_dest_defines_vreg(dest))
      continue;
    int32_t vreg = irop_get_vreg(dest);
    uint32_t val = 0;
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_STORE)
    {
      /* a load through a pointer is what it points at, not the pointer */
      if (!(q->op == TCCIR_OP_LOAD && src1.is_lval && irop_get_tag(src1) == IROP_TAG_VREG))
        val = v1;
    }
    else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
      val = (v1 & DP_KIND) ? v1 : (q->op == TCCIR_OP_ADD ? v2 : 0);
    else if (q->op == TCCIR_OP_FUNCCALLVAL && cfg->has_src2 && (id = lt_call_id(ir, c->calls, src2)) >= 0)
    {
      val = c->callv[id];
      c->callv[id] = 0;
    }
    if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vreg);
      if (pos < c->ntemps)
      {
        c->tflag[pos] = val;
        c->tgen[pos] = c->gen;
      }
      continue;
    }
    int v = TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_VAR ? dp_var(c, vreg) : -1;
    if (v < c->base || v >= c->base + 32)
      continue;
    if ((val & DP_KIND) == DP_FRAME || (val & DP_KIND) == DP_VIA)
      c->f |= 1u << (v - c->base);
    else
      c->f &= ~(1u << (v - c->base));
  }

  /* `if (p == &x)`: where they differ, p does not hold x's address */
  if (bb->end_idx - bb->start_idx >= 2 && bb->num_succs == 2)
  {
    IRQuadCompact *jq = &ir->compact_instructions[bb->end_idx - 1], *cq = jq - 1;
    if (jq->op == TCCIR_OP_JUMPIF && cq->op == TCCIR_OP_CMP)
    {
      IROperand a = tcc_ir_op_get_src1(ir, cq), b2 = tcc_ir_op_get_src2(ir, cq);
      int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jq));
      int target = (int)tcc_ir_op_dest_imm(ir, jq);
      /* one side a followed variable holding a frame address (or a temp
         read from it), the other a frame address */
      uint32_t fa = dp_value(c, cq, a, 0), fb = dp_value(c, cq, b2, 0);
      int v = -1;
      if ((fa & DP_KIND) == DP_VIA && (fb & DP_KIND) == DP_FRAME)
        v = DP_INDEX(fa);
      else if ((fb & DP_KIND) == DP_VIA && (fa & DP_KIND) == DP_FRAME)
        v = DP_INDEX(fb);
      if (v >= c->base && v < c->base + 32 && (cond == TOK_EQ || cond == TOK_NE) && target >= 0 &&
          target < c->cfg->num_instrs && bb->end_idx < c->cfg->num_instrs)
      {
        int taken = c->cfg->instr_to_block[target], fall = c->cfg->instr_to_block[bb->end_idx];
        if (taken != fall)
        {
          c->ne_succ = cond == TOK_NE ? taken : fall;
          c->ne_clear = 1u << (v - c->base);
        }
      }
    }
  }

  /* leaving the function (not by a noreturn call or a trap): a followed
     global that may hold a frame address keeps it past the frame */
  if (c->report && bb->num_succs == 0 && bb->end_idx > bb->start_idx)
  {
    IRQuadCompact *lq = &ir->compact_instructions[bb->end_idx - 1];
    if (lq->op == TCCIR_OP_IJUMP || lq->op == TCCIR_OP_SWITCH_TABLE || tcc_ir_cfg_instr_ends_flow(ir, lq))
      return;
    for (int l = 0; l < c->nlocs; l++)
    {
      int item = c->nv + l;
      DpLoc *loc = &c->locs[l];
      if (item < c->base || item >= c->base + 32 || !(c->f & (1u << (item - c->base))) || loc->reported)
        continue;
      loc->reported = 1;
      tcc_warning_c(warn_dangling_pointer)("%i:the address of a local variable is stored in global '%s', which "
                                           "outlives '%s'",
                                           loc->line, get_tok_str(loc->sym->v, NULL), funcname);
    }
  }
}

static void lt_dangling_check(TCCIRState *ir, Sym *func, const LtCalls *calls)
{
  DpCtx c;
  int n = ir->next_instruction_index;
  memset(&c, 0, sizeof c);
  c.ir = ir;
  c.calls = calls;
  if (!n)
    return;
  /* the variables ever given a frame address (a linear look is enough to
     find them: the dataflow decides where they hold one) */
  c.ntemps = ir->next_temporary_variable;
  c.tflag = tcc_mallocz((c.ntemps + 1) * sizeof(uint32_t));
  c.tgen = tcc_mallocz((c.ntemps + 1) * sizeof(uint32_t));
  c.callv = tcc_mallocz((calls->n + 1) * sizeof(uint32_t));
  c.vars = NULL;
  c.gen = 1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    IROperand src1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
    int frame = dp_frame_source(q, src1, 1);
    int32_t sv = irop_get_vreg(src1);
    int id;
    if (!frame && sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP && irop_is_vreg_value(src1) &&
        TCCIR_DECODE_VREG_POSITION(sv) < c.ntemps)
      frame = c.tflag[TCCIR_DECODE_VREG_POSITION(sv)] & DP_FRAME;
    /* the result of a call that borrows from an argument given one */
    if (q->op == TCCIR_OP_FUNCPARAMVAL && frame && (id = lt_borrowed_arg(ir, calls, tcc_ir_op_get_src2(ir, q))) >= 0)
      c.callv[id] = DP_FRAME;
    if (q->op == TCCIR_OP_FUNCCALLVAL)
      frame = cfg->has_src2 && (id = lt_call_id(ir, calls, tcc_ir_op_get_src2(ir, q))) >= 0 && c.callv[id];
    if (irop_is_none(dest) || !irop_dest_defines_vreg(dest))
    {
      /* a global given a frame address, or a value that may be one (a
         variable, a temp) */
      if (q->op == TCCIR_OP_STORE && (frame || sv >= 0))
        dp_loc(&c, dest, 1);
      continue;
    }
    int32_t dv = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
    {
      if (TCCIR_DECODE_VREG_POSITION(dv) < c.ntemps)
        c.tflag[TCCIR_DECODE_VREG_POSITION(dv)] =
            (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB ||
             q->op == TCCIR_OP_FUNCCALLVAL) &&
                    frame
                ? DP_FRAME
                : 0;
      continue;
    }
    if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR && frame && dp_var(&c, dv) < 0 && !lt_vreg_addrtaken(ir, dv))
    {
      c.vars = lt_grow(c.vars, c.nv, sizeof(int32_t));
      c.vars[c.nv++] = dv;
    }
  }
  memset(c.callv, 0, (calls->n + 1) * sizeof(uint32_t));
  /* main's frame lasts as long as the program: what it stores in globals stays valid */
  if (func && !strcmp(get_tok_str(func->v, NULL), "main"))
    c.nlocs = 0;
  /* the globals follow the variables: items nv .. nv + nlocs - 1 (dp_loc
     numbers them from the final nv) */
  c.cfg = tcc_ir_cfg_build(ir);
  if (!c.cfg)
    goto done;
  tcc_ir_cfg_compute_rpo(c.cfg);
  int nb = c.cfg->num_blocks;
  uint32_t *in = tcc_malloc(nb * sizeof(uint32_t));
  uint8_t *reached = tcc_malloc(nb);
  tcc_state->diag_noabort++;
  int nitems = c.nv + c.nlocs;
  for (c.base = 0; c.base == 0 || c.base < nitems; c.base += 32)
  {
    memset(in, 0, nb * sizeof(uint32_t));
    memset(reached, 0, nb);
    reached[0] = 1;
    c.report = 0;
    for (int changed = nitems > 0; changed;)
    {
      changed = 0;
      for (int r = 0; r < c.cfg->rpo_count; r++)
      {
        int b = c.cfg->rpo_order[r];
        IRBasicBlock *bb = &c.cfg->blocks[b];
        if (!reached[b])
          continue;
        c.f = in[b];
        dp_block(&c, b);
        for (int k = 0; k < bb->num_succs; k++)
        {
          int t = bb->succs[k];
          uint32_t m = in[t] | (t == c.ne_succ ? c.f & ~c.ne_clear : c.f);
          if (m != in[t] || !reached[t])
            in[t] = m, reached[t] = 1, changed = 1;
        }
      }
    }
    /* the stores of a frame address straight into lasting memory are
       reported once: with the first chunk */
    c.report = 1;
    for (int b = 0; b < nb; b++)
      if (reached[b] || !nitems)
      {
        c.f = in[b];
        dp_block(&c, b);
      }
    if (!nitems)
      break;
  }
  tcc_state->diag_noabort--;
  tcc_free(in);
  tcc_free(reached);
  tcc_ir_cfg_free(c.cfg);
done:
  tcc_free(c.vars);
  tcc_free(c.locs);
  tcc_free(c.tflag);
  tcc_free(c.tgen);
  tcc_free(c.callv);
}


/* ------------------------------------------------------------------------ */
/* -Wlifetime: a view does not escape (docs/static_analysis_todo.md, "`view`
 * does not escape").  A bit per view parameter, at its declared position:
 * the values that may point into what it points at -- the parameter, its
 * copies, p + n -- forward over the raw-IR CFG.  Reported where such a value
 * is returned from a function whose result does not borrow from that
 * parameter (borrows(...), or the elision: a view result borrows from every
 * view parameter), and where it is stored into a global or through a
 * parameter, which outlive the call. */

#define VE_ARGS YASC_BORROWS_ARGS
#define SC_NOADDR INT32_MIN /* not a frame address (the slot and dealloc checks) */
#define VE_PARAM (1ull << 63)
#define VE_FRAME_BASE (-0x7ffffff0) /* the base of an address in this frame (StackLoc) */
#define VE_BASE_LENIENT (-2)        /* copied from a local whose origin is not known: not reported */
#define VE_OFF_ANY INT32_MIN        /* somewhere past the base: an index that is not constant */
/* an owned variable's own value, not a view of it (bit 62); the owned
   variables are bits 32..61 */
#define VE_DIRECT (1ull << 62)
#define VE_OWNED_SHIFT 32
#define VE_MAX_OWNED 30
#define VE_OWNED (((1ull << VE_MAX_OWNED) - 1) << VE_OWNED_SHIFT)
#define VE_SRC (VE_ARGS | VE_OWNED) /* points into what some parameter points at: a lasting destination */

typedef struct VeCtx
{
  TCCIRState *ir;
  IRCFG *cfg;
  int32_t *vars; /* the followed variables' vregs */
  int nv;
  uint64_t *cur; /* [nv] walking a block */
  uint64_t *tmask;
  uint32_t *tgen;
  uint8_t *tmulti; /* a temp given its value in more than one place (`c ? a : b`): followed like a variable */
  uint32_t gen;
  int ntemps;
  int ptok[YASC_BORROWS_MAX_PARAMS]; /* a view parameter's name, by position */
  /* `out T * borrows(s) * view p`: what may be stored through each
     parameter (the positions its target borrows from) */
  uint64_t ptb[YASC_BORROWS_MAX_PARAMS];
  uint64_t allowed;                  /* the result's sources; ~0 when the result is no view */
  int report;
  int32_t *done; /* reported: line * 32 + position */
  int ndone;
  uint64_t *callarg; /* call_id -> the bits of its arguments the result borrows from */
  int ncalls;
  /* owned variables as sources: a view of one stored where it outlives the
     function, unless the variable itself moves somewhere lasting */
  int32_t ovreg[VE_MAX_OWNED];
  int otok[VE_MAX_OWNED], no;
  uint64_t moved;  /* owned variables handed on: into an owned destination, an owned parameter, the result */
  int *cand;       /* (line, owned index, where kind, where token) per view of one stored */
  int ncand;
  int ret_owned;   /* the function's result is owned */
  /* borrows(member) members: per temp, block-local, the address it holds
     (base vreg + offset) and, for a pointer loaded from memory, where from */
  int32_t *tab, *tao, *tlb, *tlo;
  /* the state past the followed variables (nw slots in all): for each local
     variable a borrows(member) store reads, where its value came from (two
     slots: a loaded pointer's origin, an address's) */
  int nw;
  int32_t ovar[16];
  int nov;
  /* borrows(member) members whose source member was freed or changed: a
     bit per (base, the object's start from it, entry), in the slot past the
     origins (nv + 2 * nov) */
  int32_t ibase[64], istart[64], ient[64];
  int nitems;
  int fent[16], nfent; /* the entries this function stores or reads */
} VeCtx;

static int ve_item(VeCtx *c, int32_t base, int32_t start, int e)
{
  for (int i = 0; i < c->nitems; i++)
    if (c->ibase[i] == base && c->istart[i] == start && c->ient[i] == e)
      return i;
  if (c->nitems == 64)
    return -1;
  c->ibase[c->nitems] = base, c->istart[c->nitems] = start, c->ient[c->nitems] = e;
  return c->nitems++;
}

/* the temp VREG holds a borrows(member) member's address: its entry */
static int ve_fload(TCCIRState *ir, int32_t vreg)
{
  for (int k = 0; k < ir->lt_nb_floads; k++)
    if (ir->lt_floads[2 * k] == vreg)
      return ir->lt_floads[2 * k + 1];
  return -1;
}

/* the source member at (base, off) is freed or changed: the members that
   borrow from it are dead */
static void ve_kill_sources(VeCtx *c, int32_t base, int32_t off)
{
  if (base == -1 || base == VE_BASE_LENIENT || off == VE_OFF_ANY)
    return;
  for (int f = 0; f < c->nfent; f++)
  {
    YascFieldBorrows *e = yasc_field_borrows(c->fent[f]);
    for (int k = 0; e && k < e->nsrc; k++)
      if (e->src_off[k] >= 0)
      {
        int it = ve_item(c, base, off - e->src_off[k], c->fent[f]);
        if (it >= 0)
          c->cur[c->nv + 2 * c->nov] |= 1ull << it;
      }
  }
}

#define VE_ORIGIN_NONE 0ull     /* nothing reached this point yet */
#define VE_ORIGIN_UNKNOWN (~0ull) /* paths disagree, or not a followed origin */
#define VE_ORIGIN_NOTHING (1ull << 32) /* known to have none of that kind (a loaded pointer is no member's address) */

static uint64_t ve_origin(int32_t base, int32_t off)
{
  uint64_t o = ((uint64_t)(uint32_t)base << 32) | (uint32_t)off;
  return base == -1 || o == VE_ORIGIN_NONE || o == VE_ORIGIN_UNKNOWN ? VE_ORIGIN_UNKNOWN : o;
}

static uint64_t ve_origin_join(uint64_t a, uint64_t b)
{
  return a == VE_ORIGIN_NONE ? b : b == VE_ORIGIN_NONE || a == b ? a : VE_ORIGIN_UNKNOWN;
}

static int ve_ovar(VeCtx *c, int32_t vreg)
{
  for (int i = 0; i < c->nov; i++)
    if (c->ovar[i] == vreg)
      return i;
  return -1;
}


/* the address operand vreg VR holds: a base and an offset */
static void ve_addr(VeCtx *c, int32_t vr, int32_t *base, int32_t *off)
{
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos < c->ntemps && c->tgen[pos] == c->gen &&
      c->tab[pos] != -1)
  {
    *base = c->tab[pos];
    *off = c->tao[pos];
    return;
  }
  *base = vr;
  *off = 0;
}

/* instruction I stores into a borrows(member) member: its entry, or -1 */
static int ve_fstore(TCCIRState *ir, int i)
{
  for (int k = 0; k < ir->lt_nb_fstores; k++)
    if (i >= ir->lt_fstores[3 * k] && i < ir->lt_fstores[3 * k + 1])
      return ir->lt_fstores[3 * k + 2];
  return -1;
}

/* the value stored into member entry E at address (db, doff) points into a
   member it names, of the same object (or is null / static) */
static int ve_field_ok(VeCtx *c, YascFieldBorrows *e, IROperand val, int32_t db, int32_t doff)
{
  int32_t vr = irop_is_none(val) ? -1 : irop_get_vreg(val), pos;
  if (irop_is_none(val) || !irop_is_vreg_value(val) || vr < 0)
    return 1; /* a constant: null, or an address of static storage */
  pos = TCCIR_DECODE_VREG_POSITION(vr);
  int32_t ab, ao, lb, lo;
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
  {
    /* a local variable: where its value came from, on every path here */
    int k = ve_ovar(c, vr);
    uint64_t lo64 = k >= 0 ? c->cur[c->nv + 2 * k] : VE_ORIGIN_UNKNOWN,
             ao64 = k >= 0 ? c->cur[c->nv + 2 * k + 1] : VE_ORIGIN_UNKNOWN;
    lb = lo64 == VE_ORIGIN_NOTHING ? -1 : lo64 == VE_ORIGIN_UNKNOWN || lo64 == VE_ORIGIN_NONE ? VE_BASE_LENIENT
                                                                                              : (int32_t)(lo64 >> 32);
    lo = (int32_t)(uint32_t)lo64;
    ab = ao64 == VE_ORIGIN_NOTHING ? -1 : ao64 == VE_ORIGIN_UNKNOWN || ao64 == VE_ORIGIN_NONE ? VE_BASE_LENIENT
                                                                                              : (int32_t)(ao64 >> 32);
    ao = (int32_t)(uint32_t)ao64;
  }
  else if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || pos >= c->ntemps || c->tgen[pos] != c->gen)
    return 0;
  else
    ab = c->tab[pos], ao = c->tao[pos], lb = c->tlb[pos], lo = c->tlo[pos];
  if (lb == VE_BASE_LENIENT && ab == VE_BASE_LENIENT)
    return 1;
  if (doff == VE_OFF_ANY)
    return 1; /* the member's own address is not known exactly */
  for (int k = 0; k < e->nsrc; k++)
  {
    int arr = e->src_off[k] < 0, so = arr ? ~e->src_off[k] : e->src_off[k];
    /* an array member: anywhere in it (or just past it); a pointer member:
       what was loaded from it, plus anything */
    /* copied from a local whose origin of that kind is not known: not reported */
    if ((arr && ab == VE_BASE_LENIENT) || (!arr && lb == VE_BASE_LENIENT))
      return 1;
    if (arr && ab == db &&
        (ao == VE_OFF_ANY || (ao - doff >= so - e->self_off && ao - doff <= so - e->self_off + e->src_size[k])))
      return 1;
    if (!arr && lb == db && lo - doff == so - e->self_off)
      return 1;
  }
  return 0;
}

static int ve_owned(VeCtx *c, int32_t vreg)
{
  for (int i = 0; i < c->no; i++)
    if (c->ovreg[i] == vreg)
      return i;
  return -1;
}

/* instruction I is part of a store into an owned pointer (vstore's record) */
static int ve_ostore(TCCIRState *ir, int i)
{
  for (int k = 0; k < ir->lt_nb_ostores; k++)
    if (i >= ir->lt_ostores[2 * k] && i < ir->lt_ostores[2 * k + 1])
      return 1;
  return 0;
}

static void ve_candidate(VeCtx *c, int line, uint64_t m, int kind, int tok)
{
  for (int j = 0; j < c->no; j++)
    if (m & (1ull << (VE_OWNED_SHIFT + j)))
    {
      c->cand = lt_grow(c->cand, c->ncand, 4 * sizeof(int));
      c->cand[4 * c->ncand] = line;
      c->cand[4 * c->ncand + 1] = j;
      c->cand[4 * c->ncand + 2] = kind;
      c->cand[4 * c->ncand + 3] = tok;
      c->ncand++;
    }
}

/* the call an argument or a call instruction belongs to, its record */
static IRLtCall *ve_call(VeCtx *c, IROperand enc_op, int *call_id, int *k)
{
  int64_t enc = irop_get_imm64_ex(c->ir, enc_op);
  *call_id = TCCIR_DECODE_CALL_ID(enc);
  *k = TCCIR_DECODE_PARAM_IDX(enc);
  return *call_id >= 0 && *call_id < c->ir->lt_nb_calls && *call_id < c->ncalls ? &c->ir->lt_calls[*call_id] : NULL;
}

static int ve_var(VeCtx *c, int32_t vreg)
{
  for (int i = 0; i < c->nv; i++)
    if (c->vars[i] == vreg)
      return i;
  return -1;
}

/* a vreg's value: the bits of the view parameters it may point into */
static uint64_t ve_vreg(VeCtx *c, int32_t vreg)
{
  int v;
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
  case TCCIR_VREG_TYPE_TEMP:
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vreg);
    if (pos < c->ntemps && c->tmulti[pos] && (v = ve_var(c, vreg)) >= 0)
      return c->cur[v];
    return pos < c->ntemps && c->tgen[pos] == c->gen ? c->tmask[pos] : 0;
  }
  case TCCIR_VREG_TYPE_PARAM:
  case TCCIR_VREG_TYPE_VAR:
    if ((v = ve_owned(c, vreg)) >= 0)
      return (1ull << (VE_OWNED_SHIFT + v)) | VE_DIRECT |
             (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM ? VE_PARAM : 0);
    if ((v = ve_var(c, vreg)) >= 0)
      return c->cur[v];
    return TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM ? VE_PARAM : 0;
  }
  return 0;
}

static uint64_t ve_value(VeCtx *c, IROperand op)
{
  int32_t vreg;
  if (irop_is_none(op) || (vreg = irop_get_vreg(op)) < 0 || !irop_is_vreg_value(op))
    return 0;
  return ve_vreg(c, vreg);
}

static void ve_report(VeCtx *c, int line, uint64_t m, const char *how, const char *where)
{
  for (int k = 0; k < YASC_BORROWS_MAX_PARAMS; k++)
  {
    int key = line * 32 + k, seen = 0;
    if (!(m & (1ull << k)) || !c->ptok[k])
      continue;
    for (int i = 0; i < c->ndone && !seen; i++)
      seen = c->done[i] == key;
    if (seen)
      continue;
    c->done = lt_grow(c->done, c->ndone, sizeof(int32_t));
    c->done[c->ndone++] = key;
    tcc_warning_c(warn_lifetime)("%i:'%s' is a view: it cannot leave the function (%s%s%s)", line,
                                 get_tok_str(c->ptok[k], NULL), how, where ? where : "", where ? "'" : "");
  }
}

static void ve_block(VeCtx *c, int b)
{
  TCCIRState *ir = c->ir;
  IRBasicBlock *bb = &c->cfg->blocks[b];
  c->gen++;
  for (int i = bb->start_idx; i < bb->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    IROperand src1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
    IROperand src2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    uint64_t v1 = ve_value(c, src1), v2 = ve_value(c, src2);
    int call_id, k;
    IRLtCall *call;

    /* borrows(member) members going dead and read while dead */
    for (int so = 0; so < 2 && c->nfent; so++)
    {
      /* a read through the member's address (an operand, not a store's target) */
      IROperand mo = so ? src2 : src1;
      int32_t mb, moff, mv;
      int fe;
      YascFieldBorrows *e;
      if (irop_is_none(mo) || !mo.is_lval || irop_get_tag(mo) != IROP_TAG_VREG || (mv = irop_get_vreg(mo)) < 0 ||
          (fe = ve_fload(ir, mv)) < 0 || !(e = yasc_field_borrows(fe)))
        continue;
      ve_addr(c, mv, &mb, &moff);
      int it = moff != VE_OFF_ANY ? ve_item(c, mb, moff - e->self_off, fe) : -1;
      if (c->report && it >= 0 && (c->cur[c->nv + 2 * c->nov] >> it) & 1)
        tcc_warning_c(warn_lifetime)("%i:'%s' may be used after '%s', which it borrows from, was freed or changed",
                                     q->line_num, get_tok_str(e->self_tok, NULL), get_tok_str(e->src_tok[0], NULL));
    }
    {
      if (q->op == TCCIR_OP_STORE && !irop_is_none(dest) && dest.is_lval && irop_get_tag(dest) == IROP_TAG_VREG &&
          irop_get_vreg(dest) >= 0 && c->nfent)
      {
        int32_t db, doff;
        int se = ve_fstore(ir, i);
        YascFieldBorrows *se_e = se >= 0 ? yasc_field_borrows(se) : NULL;
        ve_addr(c, irop_get_vreg(dest), &db, &doff);
        /* into a source member: what borrows from it is dead */
        ve_kill_sources(c, db, doff);
        /* into the member itself: alive again */
        if (se_e && doff != VE_OFF_ANY)
        {
          int it = ve_item(c, db, doff - se_e->self_off, se);
          if (it >= 0)
            c->cur[c->nv + 2 * c->nov] &= ~(1ull << it);
        }
      }
      /* a source member handed to a drop parameter (freed) */
      if (q->op == TCCIR_OP_FUNCPARAMVAL && c->nfent && (call = ve_call(c, src2, &call_id, &k)) && call->known &&
          (k -= call->implicit) >= 0 && k < 32 && (call->drop & (1u << k)))
      {
        int32_t s1v = irop_is_none(src1) ? -1 : irop_get_vreg(src1), lb = -1, lo = 0;
        if (s1v >= 0 && src1.is_lval && irop_get_tag(src1) == IROP_TAG_VREG)
          ve_addr(c, s1v, &lb, &lo); /* free(c->buf) with the load folded in */
        else if (s1v >= 0 && TCCIR_DECODE_VREG_TYPE(s1v) == TCCIR_VREG_TYPE_TEMP &&
                 TCCIR_DECODE_VREG_POSITION(s1v) < c->ntemps && c->tgen[TCCIR_DECODE_VREG_POSITION(s1v)] == c->gen)
          lb = c->tlb[TCCIR_DECODE_VREG_POSITION(s1v)], lo = c->tlo[TCCIR_DECODE_VREG_POSITION(s1v)];
        ve_kill_sources(c, lb, lo);
      }
    }

    /* a store into a borrows(member) member: what it stores points into the
       same object's member it names */
    if (c->report && q->op == TCCIR_OP_STORE && !irop_is_none(dest) && dest.is_lval &&
        irop_get_tag(dest) == IROP_TAG_VREG && irop_get_vreg(dest) >= 0)
    {
      int fe = ve_fstore(ir, i);
      YascFieldBorrows *e = fe >= 0 ? yasc_field_borrows(fe) : NULL;
      int32_t db, doff;
      if (e)
      {
        ve_addr(c, irop_get_vreg(dest), &db, &doff);
        if (!ve_field_ok(c, e, src1, db, doff))
          tcc_warning_c(warn_lifetime)("%i:'%s' borrows from '%s': it can hold only a pointer into the same "
                                       "object's '%s'",
                                       q->line_num, get_tok_str(e->self_tok, NULL), get_tok_str(e->src_tok[0], NULL),
                                       get_tok_str(e->src_tok[0], NULL));
      }
    }

    /* an argument the call's view result may point into */
    if (q->op == TCCIR_OP_FUNCPARAMVAL && (v1 & VE_SRC) && (call = ve_call(c, src2, &call_id, &k)) && call->borrows &&
        (k -= call->implicit) >= 0 && k < YASC_BORROWS_MAX_PARAMS && (call->borrows & (1u << k)))
      c->callarg[call_id] |= v1 & VE_SRC;
    /* an owned variable handed to an owned parameter that is not drop: it
       may live on in the callee */
    if (q->op == TCCIR_OP_FUNCPARAMVAL && c->report && (v1 & VE_DIRECT) && (call = ve_call(c, src2, &call_id, &k)) &&
        call->known && (k -= call->implicit) >= 0 && k < 32 && (call->owned & (1u << k)) && !(call->drop & (1u << k)))
      c->moved |= v1 & VE_OWNED;
    if (c->report && q->op == TCCIR_OP_RETURNVALUE && c->ret_owned && (v1 & VE_DIRECT))
      c->moved |= v1 & VE_OWNED;

    if (c->report && q->op == TCCIR_OP_RETURNVALUE && (v1 & VE_ARGS & ~c->allowed))
      ve_report(c, q->line_num, v1 & VE_ARGS & ~c->allowed, "it is returned, and the result does not borrow from it",
                NULL);
    if (c->report && q->op == TCCIR_OP_STORE && (v1 & VE_OWNED) && !irop_is_none(dest) &&
        !irop_dest_defines_vreg(dest) && dest.is_lval)
    {
      int32_t dv;
      /* into an owned pointer: a move (the owner itself, or what move()
         took out of it), never a view */
      if (ve_ostore(ir, i))
      {
        if (v1 & VE_DIRECT)
          c->moved |= v1 & VE_OWNED;
      }
      else if (irop_get_tag(dest) == IROP_TAG_SYMREF)
      {
        Sym *sym = irop_get_sym_ex(ir, dest);
        ve_candidate(c, q->line_num, v1 & VE_OWNED, 1, sym ? sym->v : 0);
      }
      else if (irop_get_tag(dest) == IROP_TAG_VREG && (dv = irop_get_vreg(dest)) >= 0 && (ve_vreg(c, dv) & VE_PARAM))
      {
        uint64_t dm = ve_vreg(c, dv);
        int k2 = 0;
        while (k2 < YASC_BORROWS_MAX_PARAMS && !((dm >> k2) & 1))
          k2++;
        ve_candidate(c, q->line_num, v1 & VE_OWNED & ~(dm & VE_OWNED), 2,
                     k2 < YASC_BORROWS_MAX_PARAMS ? c->ptok[k2] : 0);
      }
    }
    if (c->report && q->op == TCCIR_OP_STORE && (v1 & VE_ARGS) && !irop_is_none(dest) &&
        !irop_dest_defines_vreg(dest) && dest.is_lval)
    {
      int32_t dv;
      if (irop_get_tag(dest) == IROP_TAG_SYMREF)
      {
        Sym *sym = irop_get_sym_ex(ir, dest);
        ve_report(c, q->line_num, v1 & VE_ARGS, "it is stored in global '", sym ? get_tok_str(sym->v, NULL) : "?");
      }
      else if (irop_get_tag(dest) == IROP_TAG_VREG && (dv = irop_get_vreg(dest)) >= 0 && (ve_vreg(c, dv) & VE_PARAM))
      {
        /* through a view parameter: name it.  What points into the same
           object it is stored in (`s->peek = s->buf`) stays inside it, and
           what that parameter's target borrows from (`out T * borrows(s) *
           view p`: every pointer reached through p points into s) may go
           there -- through every parameter the destination may be in. */
        uint64_t dm = ve_vreg(c, dv) & VE_ARGS, m = v1 & VE_ARGS & ~dm, tb = VE_ARGS;
        int k = 0;
        for (int j = 0; j < YASC_BORROWS_MAX_PARAMS; j++)
          if ((dm >> j) & 1)
            tb &= c->ptb[j];
        if (dm)
          m &= ~tb;
        while (k < YASC_BORROWS_MAX_PARAMS && !((dm >> k) & 1))
          k++;
        if (m && k < YASC_BORROWS_MAX_PARAMS && c->ptok[k])
          ve_report(c, q->line_num, m, "it is stored through '", get_tok_str(c->ptok[k], NULL));
        else if (m)
          ve_report(c, q->line_num, m, "it is stored through a parameter", NULL);
      }
    }

    if (irop_is_none(dest) || !irop_dest_defines_vreg(dest))
      continue;
    int32_t vreg = irop_get_vreg(dest);
    uint64_t val = 0;
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
    {
      /* a load through a pointer is what it points at: a pointer read
         through a view (`s->next`) points into what the view reaches */
      if (!(src1.is_lval && irop_get_tag(src1) == IROP_TAG_VREG))
        val = v1;
      else if (lt_ptr_temp(ir, vreg))
        val = ve_vreg(c, irop_get_vreg(src1)) & VE_SRC;
    }
    else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && src1.is_lval && irop_get_tag(src1) == IROP_TAG_VREG)
      /* a load folded into the arithmetic: `s->name + 1` */
      val = lt_ptr_temp(ir, vreg) ? ve_vreg(c, irop_get_vreg(src1)) & VE_SRC : 0;
    else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
      val = (v1 & VE_SRC ? v1 : (q->op == TCCIR_OP_ADD ? v2 : 0)) & ~VE_DIRECT;
    else if (q->op == TCCIR_OP_FUNCCALLVAL && cfg->has_src2 && (call = ve_call(c, src2, &call_id, &k)))
    {
      val = c->callarg[call_id];
      c->callarg[call_id] = 0;
    }
    int v = ve_var(c, vreg);
    /* the address it holds, and where a loaded pointer came from */
    int32_t ab = -1, ao = 0, lb = -1, lo = 0, s1 = irop_is_none(src1) ? -1 : irop_get_vreg(src1), sp = -1;
    int s1temp = s1 >= 0 && TCCIR_DECODE_VREG_TYPE(s1) == TCCIR_VREG_TYPE_TEMP &&
                 (sp = TCCIR_DECODE_VREG_POSITION(s1)) < c->ntemps && c->tgen[sp] == c->gen;
    int s1ov = s1 >= 0 && TCCIR_DECODE_VREG_TYPE(s1) == TCCIR_VREG_TYPE_VAR ? ve_ovar(c, s1) : -1;
    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) &&
        s1 >= 0 && src1.is_lval && irop_get_tag(src1) == IROP_TAG_VREG)
      /* a pointer loaded from memory, maybe with arithmetic folded in */
      ve_addr(c, s1, &lb, &lo);
    else if (!irop_is_none(src1) && irop_get_tag(src1) == IROP_TAG_STACKOFF && !src1.is_lval)
    {
      /* an address in this frame: &local, a local struct's member */
      ab = VE_FRAME_BASE;
      ao = (int32_t)irop_get_imm64_ex(ir, src1);
    }
    else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && s1ov >= 0 && irop_is_vreg_value(src1))
    {
      /* a copy of a followed local: its origins, or "not known" */
      uint64_t l64 = c->cur[c->nv + 2 * s1ov], a64 = c->cur[c->nv + 2 * s1ov + 1];
      int lk = l64 != VE_ORIGIN_UNKNOWN && l64 != VE_ORIGIN_NONE, ak = a64 != VE_ORIGIN_UNKNOWN && a64 != VE_ORIGIN_NONE;
      lb = l64 == VE_ORIGIN_NOTHING ? -1 : lk ? (int32_t)(l64 >> 32) : VE_BASE_LENIENT, lo = (int32_t)(uint32_t)l64;
      ab = a64 == VE_ORIGIN_NOTHING ? -1 : ak ? (int32_t)(a64 >> 32) : VE_BASE_LENIENT, ao = (int32_t)(uint32_t)a64;
    }
    else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && s1 >= 0 && irop_is_vreg_value(src1))
    {
      ve_addr(c, s1, &ab, &ao);
      if (irop_get_tag(src2) == IROP_TAG_IMM32 && ao != VE_OFF_ANY)
      {
        int64_t imm = irop_get_imm64_ex(ir, src2);
        ao += q->op == TCCIR_OP_ADD ? (int32_t)imm : -(int32_t)imm;
      }
      else
        ao = VE_OFF_ANY;
      if (s1temp)
        lb = c->tlb[sp], lo = c->tlo[sp] != VE_OFF_ANY && irop_get_tag(src2) == IROP_TAG_IMM32 ? c->tlo[sp] : VE_OFF_ANY;
    }
    else if (q->op == TCCIR_OP_ASSIGN && s1 >= 0 && irop_is_vreg_value(src1))
    {
      ve_addr(c, s1, &ab, &ao);
      if (s1temp)
        lb = c->tlb[sp], lo = c->tlo[sp];
    }
    if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP && v < 0)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vreg);
      if (pos < c->ntemps)
      {
        c->tab[pos] = ab, c->tao[pos] = ao, c->tlb[pos] = lb, c->tlo[pos] = lo;
        c->tmask[pos] = val;
        c->tgen[pos] = c->gen;
      }
      continue;
    }
    if (v >= 0)
      c->cur[v] = val & ~VE_DIRECT; /* a variable that is not owned holds a view */
    /* a local variable a borrows(member) store reads: where its value came from */
    int ov = ve_ovar(c, vreg);
    if (ov >= 0)
    {
      int lk = lb >= 0 || lb < VE_BASE_LENIENT, ak = ab >= 0 || ab < VE_BASE_LENIENT;
      /* one kind known: the other is known to be absent */
      c->cur[c->nv + 2 * ov] = lk ? ve_origin(lb, lo) : ak && lb == -1 ? VE_ORIGIN_NOTHING : VE_ORIGIN_UNKNOWN;
      c->cur[c->nv + 2 * ov + 1] = ak ? ve_origin(ab, ao) : lk && ab == -1 ? VE_ORIGIN_NOTHING : VE_ORIGIN_UNKNOWN;
    }
  }
}

void tcc_ir_view_escape_check(TCCIRState *ir, Sym *func)
{
  VeCtx c;
  Sym *pa;
  uint64_t entry_bits = 0;
  int n = ir->next_instruction_index, k, nb;
  uint64_t *init = NULL;
  memset(&c, 0, sizeof c);
  c.ir = ir;
  if (!n || !func || !func->type.ref)
    return;
  /* the view parameters: the prototype's pointers that say view (or
     borrows), followed from their own variables */
  for (k = 0, pa = sym_first(func->type.ref); pa && k < YASC_BORROWS_MAX_PARAMS; pa = pa->next, k++)
  {
    int q = type_yasc(&pa->type);
    if (((pa->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR && !type_is_handle(&pa->type)) || !(q & YASC_VIEW) ||
        (q & YASC_UNCHECKED))
      continue;
    c.ptb[k] = (uint64_t)(type_target_borrows(&pa->type) & YASC_BORROWS_ARGS);
    for (int i = 0; i < ir->lt_nb_vars; i++)
    {
      IRLtVar *lv = &ir->lt_vars[i];
      if (lv->tok == (pa->v & ~SYM_FIELD) && !lv->silent && !lv->untracked && lv->vreg >= 0 &&
          TCCIR_DECODE_VREG_TYPE(lv->vreg) == TCCIR_VREG_TYPE_PARAM && ve_var(&c, lv->vreg) < 0)
      {
        c.vars = lt_grow(c.vars, c.nv, sizeof(int32_t));
        init = lt_grow(init, c.nv, sizeof(uint64_t));
        init[c.nv] = (1ull << k) | VE_PARAM;
        c.vars[c.nv++] = lv->vreg;
        c.ptok[k] = lv->tok;
        entry_bits |= 1ull << k;
        break;
      }
    }
  }
  /* the owned variables: sources too */
  for (int i = 0; i < ir->lt_nb_vars && c.no < VE_MAX_OWNED; i++)
  {
    IRLtVar *lv = &ir->lt_vars[i];
    if ((lv->kind & LTV_OWNED) && !lv->silent && !lv->untracked && lv->vreg >= 0 && ve_owned(&c, lv->vreg) < 0 &&
        ve_var(&c, lv->vreg) < 0)
    {
      c.ovreg[c.no] = lv->vreg;
      c.otok[c.no++] = lv->tok;
    }
  }
  if (!entry_bits && !c.no && !ir->lt_nb_fstores && !ir->lt_nb_floads)
    goto done;
  /* the borrows(member) entries the function stores or reads */
  for (int i = 0; i < ir->lt_nb_fstores + ir->lt_nb_floads; i++)
  {
    int e = i < ir->lt_nb_fstores ? ir->lt_fstores[3 * i + 2] : ir->lt_floads[2 * (i - ir->lt_nb_fstores) + 1], seen = 0;
    for (int f = 0; f < c.nfent && !seen; f++)
      seen = c.fent[f] == e;
    if (!seen && c.nfent < 16)
      c.fent[c.nfent++] = e;
  }
  c.ret_owned = (func->type.ref->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR &&
                (type_yasc(&func->type.ref->type) & YASC_OWNED);
  {
    CType *rt = &func->type.ref->type;
    int rq = type_yasc(rt);
    c.allowed = (rt->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (rq & YASC_VIEW) && !(rq & YASC_UNCHECKED)
                    ? (uint64_t)type_borrows(rt) & VE_ARGS
                    : ~0ull;
  }
  /* the variables that may get such a value: copies of a followed variable
     or of a temp holding one, until no more appear */
  c.ntemps = ir->next_temporary_variable;
  c.tmask = tcc_mallocz((c.ntemps + 1) * sizeof(uint64_t));
  c.tgen = tcc_mallocz((c.ntemps + 1) * sizeof(uint32_t));
  c.tab = tcc_malloc((c.ntemps + 1) * 4 * sizeof(int32_t));
  int32_t *tsrc = tcc_malloc((c.ntemps + 1) * sizeof(int32_t));
  memset(tsrc, 0xff, (c.ntemps + 1) * sizeof(int32_t));
  c.tao = c.tab + c.ntemps + 1;
  c.tlb = c.tao + c.ntemps + 1;
  c.tlo = c.tlb + c.ntemps + 1;
  c.ncalls = ir->lt_nb_calls;
  c.callarg = tcc_mallocz((c.ncalls + 1) * sizeof(uint64_t));
  c.tmulti = tcc_mallocz(c.ntemps + 1);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    int32_t dv;
    if (!irop_is_none(dest) && irop_dest_defines_vreg(dest) && (dv = irop_get_vreg(dest)) >= 0 &&
        TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP && TCCIR_DECODE_VREG_POSITION(dv) < c.ntemps &&
        c.tmulti[TCCIR_DECODE_VREG_POSITION(dv)] < 2)
      c.tmulti[TCCIR_DECODE_VREG_POSITION(dv)]++;
  }
  for (int i = 0; i <= c.ntemps; i++)
    c.tmulti[i] = c.tmulti[i] > 1;
  for (int grew = 1, round = 0; grew && round < 8; round++)
  {
    grew = 0;
    memset(c.tmask, 0, (c.ntemps + 1) * sizeof(uint64_t));
    memset(c.callarg, 0, (c.ncalls + 1) * sizeof(uint64_t));
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      const IRRegistersConfig *cfg = &irop_config[q->op];
      IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
      IROperand src1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
      IROperand src2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
      int carry = 0, call_id, k;
      IRLtCall *call;
      /* a local variable stored into a borrows(member) member, or copied to
         a temp that is (tsrc: temp -> the variable it copied) */
      if (round == 0 && !irop_is_none(src1) && irop_is_vreg_value(src1))
      {
        int32_t s1v = irop_get_vreg(src1), dv0 = irop_is_none(dest) ? -1 : irop_get_vreg(dest);
        if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && dv0 >= 0 &&
            TCCIR_DECODE_VREG_TYPE(dv0) == TCCIR_VREG_TYPE_TEMP && TCCIR_DECODE_VREG_POSITION(dv0) < c.ntemps)
          tsrc[TCCIR_DECODE_VREG_POSITION(dv0)] = TCCIR_DECODE_VREG_TYPE(s1v) == TCCIR_VREG_TYPE_VAR ? s1v : -1;
        if (q->op == TCCIR_OP_STORE && ve_fstore(ir, i) >= 0)
        {
          int32_t var = TCCIR_DECODE_VREG_TYPE(s1v) == TCCIR_VREG_TYPE_VAR ? s1v
                        : TCCIR_DECODE_VREG_TYPE(s1v) == TCCIR_VREG_TYPE_TEMP && TCCIR_DECODE_VREG_POSITION(s1v) < c.ntemps
                            ? tsrc[TCCIR_DECODE_VREG_POSITION(s1v)]
                            : -1;
          if (var >= 0 && ve_ovar(&c, var) < 0 && c.nov < 16)
            c.ovar[c.nov++] = var;
        }
      }
      if (q->op == TCCIR_OP_FUNCPARAMVAL && (call = ve_call(&c, src2, &call_id, &k)) && call->borrows)
      {
        int32_t sv = irop_get_vreg(src1);
        if (!irop_is_none(src1) && sv >= 0 && irop_is_vreg_value(src1) &&
            (TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP
                 ? TCCIR_DECODE_VREG_POSITION(sv) < c.ntemps &&
                       (c.tmask[TCCIR_DECODE_VREG_POSITION(sv)] || ve_var(&c, sv) >= 0)
                 : ve_var(&c, sv) >= 0 || ve_owned(&c, sv) >= 0))
          c.callarg[call_id] = 1;
        continue;
      }
      if (irop_is_none(dest) || !irop_dest_defines_vreg(dest))
        continue;
      if (q->op == TCCIR_OP_FUNCCALLVAL && cfg->has_src2 && (call = ve_call(&c, src2, &call_id, &k)))
        carry = c.callarg[call_id] != 0;
      if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
        for (int s = 0; s < 2; s++)
        {
          IROperand op = s ? src2 : src1;
          int32_t sv = irop_get_vreg(op);
          int deref_ptr = !s && op.is_lval && irop_get_tag(op) == IROP_TAG_VREG && lt_ptr_temp(ir, irop_get_vreg(dest));
          if (irop_is_none(op) || sv < 0 || (!irop_is_vreg_value(op) && !deref_ptr) || (s && q->op != TCCIR_OP_ADD))
            continue;
          if (TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP)
            carry |= TCCIR_DECODE_VREG_POSITION(sv) < c.ntemps &&
                     (c.tmask[TCCIR_DECODE_VREG_POSITION(sv)] || ve_var(&c, sv) >= 0);
          else
            carry |= ve_var(&c, sv) >= 0 || ve_owned(&c, sv) >= 0;
        }
      int32_t dv = irop_get_vreg(dest);
      int multi = TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP && TCCIR_DECODE_VREG_POSITION(dv) < c.ntemps &&
                  c.tmulti[TCCIR_DECODE_VREG_POSITION(dv)];
      if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP && !multi)
      {
        if (TCCIR_DECODE_VREG_POSITION(dv) < c.ntemps)
          c.tmask[TCCIR_DECODE_VREG_POSITION(dv)] = carry;
      }
      else if (carry && (multi || (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR && !lt_vreg_addrtaken(ir, dv))) &&
               ve_var(&c, dv) < 0)
      {
        c.vars = lt_grow(c.vars, c.nv, sizeof(int32_t));
        init = lt_grow(init, c.nv, sizeof(uint64_t));
        init[c.nv] = 0;
        c.vars[c.nv++] = dv;
        grew = 1;
      }
    }
  }
  memset(c.tgen, 0, (c.ntemps + 1) * sizeof(uint32_t));
  memset(c.callarg, 0, (c.ncalls + 1) * sizeof(uint64_t));
  tcc_free(tsrc);
  c.gen = 1;

  c.cfg = tcc_ir_cfg_build(ir);
  if (!c.cfg)
    goto done;
  tcc_ir_cfg_compute_rpo(c.cfg);
  nb = c.cfg->num_blocks;
  {
    c.nw = c.nv + 2 * c.nov + 1; /* + the dead borrows(member) members */
    uint64_t *in = tcc_mallocz((size_t)nb * c.nw * sizeof(uint64_t));
    uint8_t *reached = tcc_mallocz(nb);
    c.cur = tcc_malloc((c.nw + 1) * sizeof(uint64_t));
    memcpy(in, init, c.nv * sizeof(uint64_t));
    reached[0] = 1;
    for (int changed = 1; changed;)
    {
      changed = 0;
      for (int r = 0; r < c.cfg->rpo_count; r++)
      {
        int b = c.cfg->rpo_order[r];
        IRBasicBlock *bb = &c.cfg->blocks[b];
        if (!reached[b])
          continue;
        memcpy(c.cur, in + (size_t)b * c.nw, c.nw * sizeof(uint64_t));
        ve_block(&c, b);
        for (int s = 0; s < bb->num_succs; s++)
        {
          int t = bb->succs[s];
          uint64_t *ti = in + (size_t)t * c.nw;
          for (int v = 0; v < c.nv; v++)
            if ((ti[v] | c.cur[v]) != ti[v])
              ti[v] |= c.cur[v], changed = 1;
          if ((ti[c.nw - 1] | c.cur[c.nw - 1]) != ti[c.nw - 1])
            ti[c.nw - 1] |= c.cur[c.nw - 1], changed = 1;
          for (int v = c.nv; v < c.nw - 1; v++)
          {
            uint64_t j = ve_origin_join(ti[v], c.cur[v]);
            if (j != ti[v])
              ti[v] = j, changed = 1;
          }
          if (!reached[t])
            reached[t] = 1, changed = 1;
        }
      }
    }
    c.report = 1;
    tcc_state->diag_noabort++;
    for (int b = 0; b < nb; b++)
      if (reached[b])
      {
        memcpy(c.cur, in + (size_t)b * c.nw, c.nw * sizeof(uint64_t));
        ve_block(&c, b);
      }
    /* a view of an owned variable that stays where it outlives the function,
       while the variable itself is freed or dropped there */
    for (int i = 0; i < c.ncand; i++)
    {
      int line = c.cand[4 * i], j = c.cand[4 * i + 1], kind = c.cand[4 * i + 2], tok = c.cand[4 * i + 3];
      int dup = 0;
      if (c.moved & (1ull << (VE_OWNED_SHIFT + j)))
        continue;
      for (int k = 0; k < i && !dup; k++)
        dup = c.cand[4 * k] == line && c.cand[4 * k + 1] == j;
      if (dup)
        continue;
      if (kind == 1)
        tcc_warning_c(warn_lifetime)("%i:a view of '%s' is stored in global '%s', which outlives it", line,
                                     get_tok_str(c.otok[j], NULL), tok ? get_tok_str(tok, NULL) : "?");
      else if (tok)
        tcc_warning_c(warn_lifetime)("%i:a view of '%s' is stored through '%s', which outlives it", line,
                                     get_tok_str(c.otok[j], NULL), get_tok_str(tok, NULL));
      else
        tcc_warning_c(warn_lifetime)("%i:a view of '%s' is stored through a parameter, which outlives it", line,
                                     get_tok_str(c.otok[j], NULL));
    }
    tcc_state->diag_noabort--;
    tcc_free(in);
    tcc_free(reached);
    tcc_free(c.cur);
  }
  tcc_ir_cfg_free(c.cfg);
done:
  tcc_free(init);
  tcc_free(c.vars);
  tcc_free(c.tmask);
  tcc_free(c.tgen);
  tcc_free(c.done);
  tcc_free(c.callarg);
  tcc_free(c.tmulti);
  tcc_free(c.cand);
  tcc_free(c.tab);
}

/* ------------------------------------------------------------------------ */
/* -Wlifetime: `owned(dealloc)`.  An owned pointer is released by its own
 * deallocator: what `owned(fclose)` says, or `malloc(fclose, 1)` on the
 * allocator.  A plain `owned` variable takes on the deallocator of what it
 * holds (a call's result, another owned variable); handed to a `drop`
 * parameter -- the parameter's own owned(...), else the callee's name -- of
 * another deallocator, it is reported.  A copy (`move(p)`: `T <-- V [LOAD]`)
 * carries the deallocator and the variable it was made from.  Plain `owned` says nothing and is
 * never compared.  One linear walk: a variable holds one kind of pointer. */
/* -Wlifetime: a `T * restrict view r` local (S3).  While r is still to be
 * used, what it borrows is reached through r only: a variable it was made
 * from (an owned pointer) or a local object whose address it holds, read or
 * written directly, is reported.  r's source comes from its definitions (all
 * must agree; several sources: not checked), through copies and offsets in
 * the block; liveness is r's own (a pointer derived from r and kept is not
 * followed).  Inlined bodies are their callee's. */
static int rs_frame_obj(TCCIRState *ir, int32_t off)
{
  for (int k = 0; k < ir->frame_obj_count; k++)
  {
    const int32_t *o = &ir->frame_objs[FRAME_OBJ_WORDS * k];
    if (o[0] <= off && off < o[0] + (o[1] > 0 ? o[1] : 1))
      return k;
  }
  return -1;
}

/* the object OP (at instruction I) points into: a variable's vreg (>= 0) or
   -2 - a frame object; -1 none.  CHAIN marks the instructions followed.  A
   value loaded from memory (`s->buf`) ends the chain: it points to whatever
   was stored there, not into the object it was read from. */
static int32_t rs_source(TCCIRState *ir, IRCFG *cfg, int i, IROperand op, uint8_t *chain)
{
  int from = cfg->blocks[cfg->instr_to_block[i]].start_idx;
  for (int d = 0, at = i; d < 6 && !irop_is_none(op); d++)
  {
    int32_t vr = irop_get_vreg(op);
    if (lt_is_addr(op) && vr < 0)
    {
      int k = rs_frame_obj(ir, (int32_t)irop_get_imm64_ex(ir, op));
      return k >= 0 ? -2 - k : -1;
    }
    if (vr < 0)
      return -1;
    /* a variable or parameter: its value (memory through it goes through a
       temp copy first in the raw IR) */
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return vr;
    int j = ix_def(ir, from, at, vr);
    if (j < 0)
      return -1;
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB &&
        q->op != TCCIR_OP_LOAD)
      return -1;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (q->op == TCCIR_OP_LOAD && !irop_is_vreg_value(s1))
      return -1;
    chain[j] = 1;
    IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    op = (q->op == TCCIR_OP_ADD && !irop_is_none(s2) && lt_is_addr(s2) && !lt_is_addr(s1)) ? s2 : s1;
    at = j;
  }
  return -1;
}

/* does OP reach object KEY directly */
static int rs_touches(TCCIRState *ir, IROperand op, int32_t key)
{
  int32_t vr;
  if (irop_is_none(op))
    return 0;
  vr = irop_get_vreg(op);
  if (key >= 0)
    return vr == key;
  if (vr >= 0)
    return 0;
  if (lt_is_addr(op) || (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_is_local(op)))
    return rs_frame_obj(ir, (int32_t)irop_get_imm64_ex(ir, op)) == -2 - key;
  return 0;
}

static int rs_in_inline(const TCCIRState *ir, int i)
{
  for (int k = 0; k < ir->lt_nb_inl; k++)
    if (i >= ir->lt_inl[2 * k] && i < ir->lt_inl[2 * k + 1])
      return 1;
  return 0;
}

static const char *rs_name(TCCIRState *ir, int32_t key, char *buf, int size)
{
  if (key >= 0)
  {
    for (int v = 0; v < ir->lt_nb_vars; v++)
      if (ir->lt_vars[v].vreg == key)
        return get_tok_str(ir->lt_vars[v].tok, NULL);
    return NULL;
  }
  const int32_t *o = &ir->frame_objs[FRAME_OBJ_WORDS * (-2 - key)];
  for (int k = 0; k < ir->lt_nb_fnames; k++)
    if (ir->lt_fnames[2 * k] == o[0])
    {
      snprintf(buf, size, "%s", get_tok_str(ir->lt_fnames[2 * k + 1], NULL));
      return buf;
    }
  return NULL;
}

void tcc_ir_restrict_check(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  IRCFG *cfg;
  if (!ir->lt_nb_rlocals || !n || !(cfg = tcc_ir_cfg_build(ir)))
    return;
  int nb = cfg->num_blocks;
  uint8_t *chain = tcc_mallocz(n + 1), *use = tcc_malloc(nb + 1), *def = tcc_malloc(nb + 1);
  uint8_t *lin = tcc_malloc(nb + 1), *lout = tcc_malloc(nb + 1);
  tcc_state->diag_noabort++;
  for (int r = 0; r < ir->lt_nb_rlocals; r++)
  {
    int32_t rv = ir->lt_rlocals[2 * r], key = -1;
    int ok = 1, ndef = 0, last_line = 0;
    memset(chain, 0, n + 1);
    /* the source: every definition's, the same */
    for (int i = 0; i < n && ok; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      IROperand d = irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
      if (irop_is_none(d) || !irop_dest_defines_vreg(d) || irop_get_vreg(d) != rv)
        continue;
      IROperand s1 = irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
      int32_t k = (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA ||
                   (q->op == TCCIR_OP_LOAD && irop_is_vreg_value(s1)))
                      ? rs_source(ir, cfg, i, s1, chain)
                      : -1;
      chain[i] = 1;
      if (k == -1 || (ndef && k != key))
        ok = 0;
      key = k, ndef++;
    }
    if (!ok || !ndef || key == -1)
      continue;
    /* r's liveness: per block, then per instruction going back */
    for (int b = 0; b < nb; b++)
    {
      use[b] = def[b] = 0;
      for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        const IRRegistersConfig *rc = &irop_config[q->op];
        IROperand s1 = rc->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
        IROperand s2 = rc->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
        IROperand d = rc->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
        int reads = (!irop_is_none(s1) && irop_get_vreg(s1) == rv) || (!irop_is_none(s2) && irop_get_vreg(s2) == rv) ||
                    (!irop_is_none(d) && !irop_dest_defines_vreg(d) && irop_get_vreg(d) == rv);
        if (reads && !def[b])
          use[b] = 1;
        if (!irop_is_none(d) && irop_dest_defines_vreg(d) && irop_get_vreg(d) == rv)
          def[b] = 1;
      }
      lin[b] = use[b], lout[b] = 0;
    }
    for (int changed = 1; changed;)
    {
      changed = 0;
      for (int b = nb; b-- > 0;)
      {
        uint8_t o = 0;
        for (int s = 0; s < cfg->blocks[b].num_succs; s++)
          o |= lin[cfg->blocks[b].succs[s]];
        uint8_t in = use[b] | (o & !def[b]);
        if (o != lout[b] || in != lin[b])
          lout[b] = o, lin[b] = in, changed = 1;
      }
    }
    for (int b = 0; b < nb; b++)
    {
      int live = lout[b];
      for (int i = cfg->blocks[b].end_idx; i-- > cfg->blocks[b].start_idx;)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        const IRRegistersConfig *rc = &irop_config[q->op];
        IROperand s1 = rc->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
        IROperand s2 = rc->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
        IROperand d = rc->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
        int defs = !irop_is_none(d) && irop_dest_defines_vreg(d) && irop_get_vreg(d) == rv;
        int reads = (!irop_is_none(s1) && irop_get_vreg(s1) == rv) || (!irop_is_none(s2) && irop_get_vreg(s2) == rv) ||
                    (!irop_is_none(d) && !defs && irop_get_vreg(d) == rv);
        if (live && !chain[i] && !reads && q->line_num != last_line && !rs_in_inline(ir, i) &&
            (rs_touches(ir, s1, key) || rs_touches(ir, s2, key) || (!defs && rs_touches(ir, d, key))))
        {
          char buf[64], rn[64];
          const char *what = rs_name(ir, key, buf, sizeof buf);
          last_line = q->line_num;
          pstrcpy(rn, sizeof rn, get_tok_str(ir->lt_rlocals[2 * r + 1], NULL));
          if (what)
            tcc_warning_c(warn_lifetime)("%i:'%s' is reached directly while '%s', a restrict borrow of it, is still in use",
                                         q->line_num, what, rn);
          else
            tcc_warning_c(warn_lifetime)("%i:what '%s' borrows is reached directly while '%s' is still in use",
                                         q->line_num, rn, rn);
        }
        if (defs)
          live = 0;
        if (reads)
          live = 1;
      }
    }
  }
  tcc_state->diag_noabort--;
  tcc_free(chain);
  tcc_free(use);
  tcc_free(def);
  tcc_free(lin);
  tcc_free(lout);
  tcc_ir_cfg_free(cfg);
}

void tcc_ir_dealloc_check(TCCIRState *ir)
{
  int n = ir->next_instruction_index, ntemps = ir->next_temporary_variable, nv = ir->lt_nb_vars;
  int *tdeal, *tdvar, *vdeal;
  if (!n || !ir->lt_nb_calls)
    return;
  /* per temp: the deallocator of its value, and 1 + the owned variable it
     is a copy of (0: none) */
  tdeal = tcc_mallocz(2 * (ntemps + 1) * sizeof(int));
  tdvar = tdeal + ntemps + 1;
  vdeal = tcc_mallocz((nv + 1) * sizeof(int));
  int32_t *tfa = tcc_malloc((ntemps + 1) * sizeof(int32_t));
  for (int t = 0; t <= ntemps; t++)
    tfa[t] = SC_NOADDR;
  for (int i = 0; i < nv; i++)
    vdeal[i] = (ir->lt_vars[i].kind & LTV_OWNED) ? ir->lt_vars[i].dealloc : -1;
  tcc_state->diag_noabort++;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    IROperand src1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
    IROperand src2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    int d = 0, var = -1;
    int32_t sv = irop_is_none(src1) ? -1 : irop_get_vreg(src1);
    /* the deallocator of src1's value, and the owned variable it is */
    if (sv >= 0 && irop_is_vreg_value(src1))
    {
      if (TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP)
      {
        if (TCCIR_DECODE_VREG_POSITION(sv) < ntemps)
          d = tdeal[TCCIR_DECODE_VREG_POSITION(sv)], var = tdvar[TCCIR_DECODE_VREG_POSITION(sv)] - 1;
      }
      else
        for (int k = 0; k < nv; k++)
          if (ir->lt_vars[k].vreg == sv && !ir->lt_vars[k].silent && vdeal[k] >= 0)
          {
            var = k, d = vdeal[k];
            break;
          }
    }
    if (d < 0)
      d = 0; /* several kinds: not followed */

    /* regcomp(&re, ...) with `regex_t re` not declared owned(regfree): what
       the callee hands over is never released */
    if (q->op == TCCIR_OP_FUNCPARAMVAL && !irop_is_none(src1) && !src1.is_lval)
    {
      int32_t off = SC_NOADDR, sv1 = irop_get_vreg(src1);
      if (lt_is_addr(src1) && sv1 < 0)
        off = (int32_t)irop_get_imm64_ex(ir, src1);
      else if (sv1 >= 0 && TCCIR_DECODE_VREG_TYPE(sv1) == TCCIR_VREG_TYPE_TEMP && TCCIR_DECODE_VREG_POSITION(sv1) < ntemps)
        off = tfa[TCCIR_DECODE_VREG_POSITION(sv1)];
      if (off != SC_NOADDR)
      {
        int64_t enc = irop_get_imm64_ex(ir, src2);
        int call_id = TCCIR_DECODE_CALL_ID(enc), k = TCCIR_DECODE_PARAM_IDX(enc), held = 0;
        IRLtCall *call = call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
        if (call && call->known && !call->in_inline && (k -= call->implicit) >= 0 && k < 32 &&
            ((call->outown >> k) & 1) && ((call->outown_out >> k) & 1) && call->callee)
        {
          for (int s2 = 0; s2 < ir->lt_nb_slots; s2++)
            if (ir->lt_slots[s2].base <= off && off < ir->lt_slots[s2].base + ir->lt_slots[s2].base_size)
              held = 1;
          if (!held && ((call->objout >> k) & 1))
            tcc_warning_c(warn_lifetime)("%i:what '%s' writes is owned, but the local it is written into is not "
                                         "declared owned(...): it is never released",
                                         q->line_num, get_tok_str(call->callee, NULL));
        }
      }
    }

    if (q->op == TCCIR_OP_FUNCPARAMVAL && d > 0)
    {
      int64_t enc = irop_get_imm64_ex(ir, src2);
      int call_id = TCCIR_DECODE_CALL_ID(enc), k = TCCIR_DECODE_PARAM_IDX(enc);
      IRLtCall *call = call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
      if (call && call->known && !call->in_inline && (k -= call->implicit) >= 0 && k < 32 &&
          (call->drop & (1u << k)))
      {
        int p = call->drop_dealloc ? call->drop_dealloc : call->callee;
        if (p && p != d)
        {
          if (var >= 0)
            tcc_warning_c(warn_lifetime)("%i:'%s' is released by %s, not %s", q->line_num,
                                         get_tok_str(ir->lt_vars[var].tok, NULL), get_tok_str(d, NULL),
                                         get_tok_str(p, NULL));
          else
            tcc_warning_c(warn_lifetime)("%i:a pointer released by %s is passed to %s", q->line_num,
                                         get_tok_str(d, NULL), get_tok_str(p, NULL));
        }
      }
      continue;
    }

    if (irop_is_none(dest) || !irop_dest_defines_vreg(dest))
      continue;
    int32_t dv = irop_get_vreg(dest);
    if (q->op == TCCIR_OP_FUNCCALLVAL && cfg->has_src2)
    {
      int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
      d = call_id >= 0 && call_id < ir->lt_nb_calls ? ir->lt_calls[call_id].ret_dealloc : 0;
      var = -1;
    }
    else if (q->op != TCCIR_OP_ASSIGN && (q->op != TCCIR_OP_LOAD || !irop_is_vreg_value(src1)))
      d = 0, var = -1; /* not a copy of src1 */
    if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
    {
      if (TCCIR_DECODE_VREG_POSITION(dv) < ntemps)
      {
        tdeal[TCCIR_DECODE_VREG_POSITION(dv)] = d;
        tdvar[TCCIR_DECODE_VREG_POSITION(dv)] = d ? var + 1 : 0;
        tfa[TCCIR_DECODE_VREG_POSITION(dv)] =
            (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA) && lt_is_addr(src1) && irop_get_vreg(src1) < 0
                ? (int32_t)irop_get_imm64_ex(ir, src1)
                : SC_NOADDR;
      }
      continue;
    }
    /* an owned variable given a pointer: its deallocator from here */
    for (int k = 0; k < nv; k++)
    {
      IRLtVar *lv = &ir->lt_vars[k];
      if (lv->vreg != dv || lv->silent || vdeal[k] < 0 || !d)
        continue;
      if (lv->dealloc && lv->dealloc != d)
        tcc_warning_c(warn_lifetime)("%i:a pointer released by %s is stored in '%s', which is released by %s",
                                     q->line_num, get_tok_str(d, NULL), get_tok_str(lv->tok, NULL),
                                     get_tok_str(lv->dealloc, NULL));
      else if (!lv->dealloc)
        vdeal[k] = !vdeal[k] || vdeal[k] == d ? d : -2; /* -2: several kinds, not followed */
      break;
    }
  }
  tcc_state->diag_noabort--;
  tcc_free(tdeal);
  tcc_free(vdeal);
  tcc_free(tfa);
}

/* ------------------------------------------------------------------------ */
/* -Wlifetime: the owned items of local structs and arrays (lt_slots: `b.buf`,
 * `arr[0]`, `fds[1]`).  They have no vreg, so this follows them by frame
 * offset: stores, reads (folded loads too), arguments, returns, the end of
 * the local's scope (frame_objs), and the address of the local passed to an
 * owned out-parameter (`pipe(fds)`, a status result's test deciding).  A
 * local whose address goes anywhere else is not followed.
 *
 * Elements at a variable index (`a[i]`, `b[i].buf`, `fds[i]`): the elements
 * of one innermost array, one member of each, are a class; per class and
 * index variable one more item (a vitem, named `a[i]`) stands for the element
 * at i while i is not written.  Writing i, or leaving the array's scope,
 * settles it: its state is joined into every element of the class (it may
 * have been any of them), which become vague (SC_VAGUE: only a definite state
 * is reported there).  Loops over i are summarised instead: in a loop whose
 * only write of i is `i += c` (c != 0) at the end of every trip (in the block
 * jumping back to the header, the header's only predecessor in the loop), and
 * which reaches the array only through elements at i, each trip has a new
 * element.  The element at i starts each trip as the elements were before
 * the loop, and the trips' results (a state entry, joined over the back edge)
 * go into the elements where the loop is left.  `for (i = C0; i < N; i++)`
 * (<=, and the same counting down) with C0 and the last value inside the
 * array, left only by its test, sets the elements it visits to that result:
 * `for (...) a[i] = malloc(...)` then `for (...) free(a[i])` is clean, and
 * without the second loop the elements leak (said once, as 'a[i]', "may").
 * Any other loop joins the result into every element (vague).  An index
 * variable plus a constant (`a[i + 1]`, `a[i - 1]`) has a vitem of its own
 * ('a[i + 1]'), and a loop over it visits the elements shifted by the
 * constant; a loop reaching the array at two offsets of i is not summarised.
 * Any other index that is not a variable (`a[i * 2]`, `a[f()]`) is not
 * followed: the array is not.  a[i] and a[j] (or a[1]) are not assumed to be
 * the same element; an array indexed by two variables (or one at two
 * offsets) never says that a[i] leaks -- an element the loops left exact is
 * said by its own name ('a[3]').  A vitem not written since it was refreshed
 * is the element as it was: settling it changes nothing, and it reports
 * nothing of its own.  An exact loop's trip starts with the element at i as
 * the elements it visits are (not a constant element written before it). */

#define SC_UNINIT 1
#define SC_OWN 2
#define SC_MOVED 4
#define SC_NULL 8 /* owns nothing: null, an invalid handle, a constant */
#define SC_OWNBITS 15
#define SC_N_NULL 16 /* may be null / invalid */
#define SC_N_SET 32  /* may be set */
#define SC_STBITS 63
#define SC_VIA 64    /* set through a variable index: a leak is said once per class, by the index's name;
                        on a vitem: written since it was refreshed (else it is as the elements are) */
#define SC_VAGUE 128 /* joined from a variable index: only a definite state is reported */

#define SC_K_ITEM 0
#define SC_K_VIT 1
#define SC_K_RES 2
#define SC_MAX_VIT 16
#define SC_MAX_PAIR 16
#define SC_MAX_TOUCH 8192
#define SC_MAX_CAND 64 /* loops looked at in a function */

typedef struct ScPair
{
  int vit, res;      /* the vitem, the state entry of the trips' results */
  int hb, step;      /* the loop's header block, the instruction stepping i */
  int exact, lo, hi; /* exact: the elements lo..hi are visited, each once (1); a variable
                        bound n: those of lo..hi below n, each once (2) */
  int exit_b;        /* exact: the block the header's test leaves to */
  int rng;           /* exact 2: the state entry of the elements lo..n-1, shared by the
                        loops over the same range (class, lo, bound, test) */
  int32_t broot;     /* exact 2: the bound's value as a vreg the same in every loop, 0 none */
  int bcond;         /* exact 2: the test that stays in the loop */
  uint8_t *blk;      /* [blocks]: in the loop */
} ScPair;

typedef struct ScCtx
{
  TCCIRState *ir;
  IRCFG *cfg;
  int ni, n, nslot, nvit, at;
  IRLtSlot *it;               /* the frontend's items, the vitems, then the loops' results */
  uint8_t *cur, *untracked, *kind;
  int *end, *move_line, *cls; /* cls: an element's or vitem's class (its element 0's item), -1 */
  uint8_t *move_drop, *multi; /* multi [nslot]: the class is indexed by more than one variable */
  int32_t *out_call, *vivr;   /* vivr: a vitem's index variable */
  int32_t *vdelta;            /* a vitem's constant added to the index: `a[i + 1]` */
  int32_t *tfa, *titem, *tcid; /* temp -> frame address, the item loaded, the call whose result it is */
  int32_t *tix, *tisc, *txat;  /* ... an element at a variable index: the index, the scale, where it was read */
  int32_t *tdelta;             /* ... the constant added to the index */
  uint32_t *tgen, *ldone;      /* ldone [nslot]: the class's leak was said at this point (lgen) */
  uint32_t gen, lgen;
  ScPair *pair;
  int npair, ncand;
  int ntemps, report, ret_owned;
  int32_t *dg; /* i != j guards (sc_find_guards): i, j, the block the edge where they differ enters */
  int ndg;
} ScCtx;

static int sc_item_at(ScCtx *c, int32_t off)
{
  for (int i = 0; i < c->nslot; i++)
    if (c->it[i].off == off && !c->untracked[i])
      return i;
  return -1;
}

static int sc_temp(ScCtx *c, IROperand op)
{
  int32_t v = irop_is_none(op) ? -1 : irop_get_vreg(op);
  int pos;
  if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP || (pos = TCCIR_DECODE_VREG_POSITION(v)) >= c->ntemps ||
      c->tgen[pos] != c->gen)
    return -1;
  return pos;
}

/* OP's value is a frame address (not one at a variable index): which */
static int sc_addr(ScCtx *c, IROperand op, int32_t *off)
{
  int pos;
  if (irop_is_none(op))
    return 0;
  if (lt_is_addr(op) && irop_get_vreg(op) < 0)
  {
    *off = (int32_t)irop_get_imm64_ex(c->ir, op);
    return 1;
  }
  if (!op.is_lval && (pos = sc_temp(c, op)) >= 0 && c->tfa[pos] != SC_NOADDR && c->tix[pos] < 0)
  {
    *off = c->tfa[pos];
    return 1;
  }
  return 0;
}

/* the vitem of the element at fa + (ix + d) * isc: its class (an element at
   fa, element k adding k to d), index variable ix, constant d */
static int sc_vitem(ScCtx *c, int32_t fa, int isc, int32_t ix, int d)
{
  int e0 = -1;
  for (int x = 0; x < c->nslot && e0 < 0; x++)
    if (c->it[x].off == fa && c->it[x].esize == isc && c->cls[x] >= 0)
      e0 = c->cls[x], d += c->it[x].idx;
  for (int x = c->nslot; e0 >= 0 && x < c->nslot + c->nvit; x++)
    if (c->cls[x] == e0 && c->vivr[x] == ix && c->vdelta[x] == d && !c->untracked[x])
      return x;
  return -1;
}

static void sc_fold_ranges(ScCtx *c, uint8_t *st, int k);

/* element x reached at a constant index: whether it is below a loop's
   variable bound is not known, so the ranges go into the elements */
static int sc_const_elem(ScCtx *c, int x)
{
  if (x >= 0 && c->npair && c->cls[x] >= 0)
    sc_fold_ranges(c, c->cur, c->cls[x]);
  return x;
}

/* OP reads (or, as a store's target, writes) an item: which, -1 */
static int sc_access(ScCtx *c, IROperand op)
{
  int pos;
  if (irop_is_none(op) || !op.is_lval)
    return -1;
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_is_local(op) && irop_get_vreg(op) < 0)
    return sc_const_elem(c, sc_item_at(c, (int32_t)irop_get_imm64_ex(c->ir, op)));
  if (irop_get_tag(op) == IROP_TAG_VREG && (pos = sc_temp(c, op)) >= 0 && c->tfa[pos] != SC_NOADDR)
  {
    if (c->tix[pos] < 0)
      return sc_const_elem(c, sc_item_at(c, c->tfa[pos]));
    if (ix_written(c->ir, c->txat[pos] + 1, c->at, c->tix[pos]))
      return -1;
    return sc_vitem(c, c->tfa[pos], c->tisc[pos], c->tix[pos], c->tdelta[pos]);
  }
  return -1;
}

/* OP's value is an item's: a temp loaded from one, or a folded read */
static int sc_value(ScCtx *c, IROperand op)
{
  int pos = op.is_lval ? -1 : sc_temp(c, op);
  if (pos >= 0)
    return c->titem[pos];
  return sc_access(c, op);
}

static const char *sc_freed(ScCtx *c, int x)
{
  return (c->it[x].kind & (LTV_HANDLE | LTV_OBJECT)) ? "released" : "freed";
}

static int sc_first_vit(ScCtx *c, int k)
{
  for (int x = c->nslot; x < c->nslot + c->nvit; x++)
    if (c->cls[x] == k)
      return x;
  return -1;
}

static const char *sc_what(ScCtx *c, int x)
{
  return (c->it[x].kind & LTV_HANDLE) ? "a handle" : (c->it[x].kind & LTV_OBJECT) ? "what it holds" : "memory";
}

/* X leaks.  agg: at a scope's end or a return, where the elements a variable
   index set are said once, by its name */
static void sc_leak_ex(ScCtx *c, int x, int line, const char *where, int agg)
{
  uint8_t full = c->cur[x], st = full & SC_OWNBITS;
  const char *name = c->it[x].name;
  int may = st != SC_OWN, k = c->cls[x];
  if (!c->report || c->it[x].silent || !(st & SC_OWN) || c->untracked[x] || c->kind[x] == SC_K_RES)
    return;
  if ((full & SC_VAGUE) && st != SC_OWN)
    return;
  if (c->kind[x] == SC_K_VIT || ((full & SC_VIA) && k >= 0 && agg))
  {
    /* indexed by two variables: an element the loops left exact is said by
       its own name, once */
    int own_name = c->multi[k] && c->kind[x] != SC_K_VIT && !(full & SC_VAGUE);
    if (c->multi[k] && !own_name)
      return;
    if (agg)
    {
      if (c->ldone[k] == c->lgen)
        return;
      c->ldone[k] = c->lgen;
    }
    if (own_name)
      ;
    else if (c->kind[x] != SC_K_VIT)
    {
      int v = sc_first_vit(c, k);
      if (v < 0)
        return;
      name = c->it[v].name;
      may = 1;
    }
    else
      may = 1;
  }
  tcc_warning_c(warn_lifetime)("%i:'%s' leaks: it %s %s %s", line, name, may ? "may still own" : "still owns",
                               sc_what(c, x), where);
}

static void sc_leak(ScCtx *c, int x, int line, const char *where)
{
  sc_leak_ex(c, x, line, where, 0);
}

static void sc_use(ScCtx *c, int x, int line)
{
  uint8_t st = c->cur[x] & SC_OWNBITS;
  if (c->report && !c->it[x].silent && (st & SC_MOVED) && !((c->cur[x] & SC_VAGUE) && st != SC_MOVED))
    tcc_warning_c(warn_lifetime)("%i:'%s' %s used after it was %s", line, c->it[x].name,
                                 st == SC_MOVED ? "is" : "may be", c->move_drop[x] ? sc_freed(c, x) : "moved");
}

/* OP is an int local or parameter's value, its address not taken: *v */
static int sc_guard_var(TCCIRState *ir, IROperand op, int32_t *v)
{
  int t;
  if (irop_is_none(op) || !irop_is_vreg_value(op) || lt_is_addr(op) || (*v = irop_get_vreg(op)) < 0)
    return 0;
  t = TCCIR_DECODE_VREG_TYPE(*v);
  return (t == TCCIR_VREG_TYPE_VAR || t == TCCIR_VREG_TYPE_PARAM) && !lt_vreg_addrtaken(ir, *v);
}

/* `if (i == j) return;`, `if (i != j) {...}`: a block CMP i, j + JUMPIF ==
   or != ends, whose edge where they differ is the only way into its target
   S.  Wherever S dominates, i != j -- unless i or j is written in a block
   S dominates (then the guard is dropped).  Two index variables of one
   array only. */
static void sc_find_guards(ScCtx *c)
{
  TCCIRState *ir = c->ir;
  IRCFG *cfg = c->cfg;
  int cap = 0, doms = 0;
  for (int b = 0; b < cfg->num_blocks; b++)
  {
    IRBasicBlock *bb = &cfg->blocks[b];
    int last = bb->end_idx - 1, s = -1;
    int32_t va, vb;
    if (last <= bb->start_idx || bb->num_succs != 2 || bb->succs[0] == bb->succs[1])
      continue;
    IRQuadCompact *lq = &ir->compact_instructions[last], *tq = &ir->compact_instructions[last - 1];
    if (lq->op != TCCIR_OP_JUMPIF || tq->op != TCCIR_OP_CMP)
      continue;
    int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, lq));
    if ((cond != TOK_EQ && cond != TOK_NE) || !sc_guard_var(ir, tcc_ir_op_get_src1(ir, tq), &va) ||
        !sc_guard_var(ir, tcc_ir_op_get_src2(ir, tq), &vb) || va == vb)
      continue;
    int target = (int)tcc_ir_op_dest_imm(ir, lq);
    int tb = target >= 0 && target < c->n ? cfg->instr_to_block[target] : -1;
    for (int k = 0; k < 2; k++)
      if ((bb->succs[k] == tb) == (cond == TOK_NE))
        s = bb->succs[k];
    if (s < 0 || cfg->blocks[s].num_preds != 1)
      continue;
    if (!doms)
      tcc_ir_cfg_compute_dominators(cfg), doms = 1;
    int written = 0;
    for (int d = 0; d < cfg->num_blocks && !written; d++)
      if (tcc_ir_cfg_dominates(cfg, s, d))
        for (int i = cfg->blocks[d].start_idx; i < cfg->blocks[d].end_idx && !written; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          IROperand dst = irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
          written = !irop_is_none(dst) && irop_dest_defines_vreg(dst) &&
                    (irop_get_vreg(dst) == va || irop_get_vreg(dst) == vb);
        }
    if (written)
      continue;
    if (c->ndg == cap)
      cap = cap ? 2 * cap : 4, c->dg = tcc_realloc(c->dg, cap * 3 * sizeof(int32_t));
    c->dg[3 * c->ndg] = va, c->dg[3 * c->ndg + 1] = vb, c->dg[3 * c->ndg + 2] = s;
    c->ndg++;
  }
}

/* is i != j known at the instruction being walked */
static int sc_distinct(ScCtx *c, int32_t i, int32_t j)
{
  int b = c->cfg->instr_to_block[c->at];
  for (int k = 0; k < c->ndg; k++)
    if (((c->dg[3 * k] == i && c->dg[3 * k + 1] == j) || (c->dg[3 * k] == j && c->dg[3 * k + 1] == i)) &&
        tcc_ir_cfg_dominates(c->cfg, c->dg[3 * k + 2], b))
      return 1;
  return 0;
}

static void sc_move(ScCtx *c, int x, int drop, int line)
{
  uint8_t st = c->cur[x] & SC_OWNBITS;
  if (c->report && !c->it[x].silent && (st & SC_MOVED) && !((c->cur[x] & SC_VAGUE) && st != SC_MOVED))
  {
    if (drop && c->move_drop[x])
      tcc_warning_c(warn_lifetime)("%i:'%s' %s %s twice", line, c->it[x].name, st == SC_MOVED ? "is" : "may be",
                                   sc_freed(c, x));
    else
      sc_use(c, x, line);
  }
  c->cur[x] = (c->cur[x] & ~SC_OWNBITS) | SC_MOVED | (c->kind[x] == SC_K_VIT ? SC_VIA : 0);
  c->move_line[x] = line;
  c->move_drop[x] = drop;
  /* a[i] and a[j] may be the same element: the element at j may be moved
     too (read or released there, it is said).  Not written into the
     elements (no SC_VIA): j may no longer index the array.  a[i] and
     a[i + 1] are different elements, and so are a[i] and a[j] past a
     guard that says i != j (sc_find_guards). */
  if (c->kind[x] == SC_K_VIT)
    for (int w = c->nslot; w < c->nslot + c->nvit; w++)
      if (w != x && c->cls[w] == c->cls[x] && c->vivr[w] != c->vivr[x] && !c->untracked[w] &&
          (c->cur[w] & SC_OWNBITS) && !(c->cur[w] & SC_MOVED) &&
          (c->vdelta[w] != c->vdelta[x] || !sc_distinct(c, c->vivr[w], c->vivr[x])))
      {
        c->cur[w] |= SC_MOVED;
        c->move_line[w] = line;
        c->move_drop[w] = drop;
      }
}

/* X, an owned member of a local handed to a `T * view` parameter (a
   method's self): the callee takes it for valid, so it must be set and not
   freed (`drop`: freeing it again is sc_move's to say).  1 when a freed one
   was said. */
static int sc_handed(ScCtx *c, int x, const IRLtCall *call, int drop, int line)
{
  uint8_t st = c->cur[x] & SC_OWNBITS;
  const char *callee = call->callee ? get_tok_str(call->callee, NULL) : NULL;
  if (!c->report || c->it[x].silent || ((c->cur[x] & SC_VAGUE) && st != SC_UNINIT && st != SC_MOVED))
    return 0;
  if ((st & SC_MOVED) && !drop)
  {
    tcc_warning_c(warn_lifetime)("%i:'%s' %s %s when its struct is handed to %s%s%s: give it a new value first", line,
                                 c->it[x].name, st == SC_MOVED ? "is" : "may be",
                                 c->move_drop[x] ? sc_freed(c, x) : "moved", callee ? "'" : "a call",
                                 callee ? callee : "", callee ? "'" : "");
    return 1;
  }
  if ((st & SC_UNINIT) && !(st & SC_MOVED))
    tcc_warning_c(warn_lifetime)("%i:'%s' %s not set when its struct is handed to %s%s%s", line, c->it[x].name,
                                 st == SC_UNINIT ? "is" : "may be", callee ? "'" : "a call", callee ? callee : "",
                                 callee ? "'" : "");
  return 0;
}

/* &local (+ off) handed to a method's self (lt_self_kind sk): `drop`
   releases its owned members, a const view reads them, a view keeps them
   valid -- each owns what it did, or what the callee put there instead; a
   maybe one may be null now */
static void sc_self_arg(ScCtx *c, int32_t off, const IRLtCall *call, int sk, int line)
{
  for (int x = 0; x < c->ni; x++)
  {
    if (c->untracked[x] || c->kind[x] == SC_K_RES || c->it[x].base > off || off >= c->it[x].base + c->it[x].base_size)
      continue;
    if (c->it[x].kind & LTV_OBJECT)
    {
      sc_use(c, x, line);
      continue;
    }
    if (sk == 3)
    {
      sc_handed(c, x, call, 1, line);
      sc_move(c, x, 1, line);
      continue;
    }
    if (sc_handed(c, x, call, 0, line))
      continue; /* said: left as it was */
    sc_use(c, x, line);
    if (sk == 2)
      continue;
    c->cur[x] = (c->cur[x] & ~SC_STBITS) | SC_OWN |
                ((c->it[x].kind & LTV_MAYBE) ? SC_NULL | SC_N_NULL | SC_N_SET : SC_N_SET) |
                (c->kind[x] == SC_K_VIT ? SC_VIA : 0);
    c->out_call[x] = -1;
  }
}

/* may X be null, and is that to be said */
static int sc_null_said(ScCtx *c, int x)
{
  return (c->cur[x] & SC_N_NULL) && !(c->cur[x] & SC_MOVED) && !((c->cur[x] & SC_VAGUE) && (c->cur[x] & SC_N_SET));
}

/* ---- variable indexes: the vitems, the loops' results ---- */

/* the ranges of class k a loop with a variable bound left (state entries,
   see sc_find_loops) that may hold elements of lo..hi, but entry `except` */
static uint8_t sc_ranges(ScCtx *c, const uint8_t *st, int k, int lo, int hi, int except)
{
  uint8_t s = 0;
  for (int p = 0; p < c->npair; p++)
  {
    const ScPair *pr = &c->pair[p];
    if (pr->exact == 2 && c->cls[pr->vit] == k && pr->rng != except && pr->lo <= hi && pr->hi >= lo)
      s |= st[pr->rng];
  }
  return s & ~SC_VIA;
}

/* the elements of class k (those lo..hi of them) joined in state st */
static uint8_t sc_join_range(ScCtx *c, const uint8_t *st, int k, int lo, int hi)
{
  uint8_t s = 0;
  for (int x = 0; x < c->nslot; x++)
    if (c->cls[x] == k && c->it[x].idx >= lo && c->it[x].idx <= hi)
      s |= st[x];
  return (s & ~SC_VIA) | sc_ranges(c, st, k, lo, hi, -1);
}

/* the ranges of class k go into its elements: something other than a loop
   over the same range reaches them, and which elements are below the bound
   is not known */
static void sc_fold_ranges(ScCtx *c, uint8_t *st, int k)
{
  for (int p = 0; p < c->npair; p++)
  {
    ScPair *pr = &c->pair[p];
    uint8_t s;
    if (pr->exact != 2 || c->cls[pr->vit] != k || !(s = st[pr->rng]))
      continue;
    for (int x = 0; x < c->nslot; x++)
      if (c->cls[x] == k && c->it[x].idx >= pr->lo && c->it[x].idx <= pr->hi)
      {
        if ((s & SC_MOVED) && !(st[x] & SC_MOVED))
          c->move_drop[x] = c->move_drop[pr->rng];
        st[x] |= s | SC_VIA;
      }
    st[pr->rng] = 0;
  }
}

static uint8_t sc_join(ScCtx *c, const uint8_t *st, int k)
{
  return sc_join_range(c, st, k, 0, 0x7fff);
}

/* an exact loop's trip starts: the element at i is one of those it visits
   (a constant element written before the loop is not) */
static void sc_trip_start(ScCtx *c, uint8_t *st, const ScPair *pr)
{
  int k = c->cls[pr->vit];
  uint8_t s;
  /* a loop over a range an earlier one set: its elements are the range's */
  if (pr->exact == 2 && st[pr->rng])
  {
    s = (st[pr->rng] & ~SC_VIA) | sc_ranges(c, st, k, pr->lo, pr->hi, pr->rng);
    if (st[pr->rng] & SC_MOVED)
      c->move_drop[pr->vit] = c->move_drop[pr->rng];
  }
  else
    s = sc_join_range(c, st, k, pr->lo, pr->hi);
  if (s & SC_STBITS)
    st[pr->vit] = s;
}

/* state s of vitem v's element (any of its class) joined into the elements */
static void sc_flush(ScCtx *c, uint8_t *st, int v, uint8_t s)
{
  int k = c->cls[v];
  /* not written since its refresh: the element at i is as it was */
  if (!(s & SC_STBITS) || !(s & SC_VIA))
    return;
  for (int x = 0; x < c->nslot; x++)
    if (c->cls[x] == k && ((st[x] | s) & SC_STBITS) != (st[x] & SC_STBITS))
    {
      if ((s & SC_MOVED) && !(st[x] & SC_MOVED))
        c->move_drop[x] = c->move_drop[v];
      st[x] |= s | SC_VIA | SC_VAGUE;
    }
}

/* vitem v is a new element: as its class's elements are */
static void sc_refresh(ScCtx *c, uint8_t *st, int v)
{
  int k = c->cls[v];
  st[v] = sc_join(c, st, k);
  if (st[v] & SC_MOVED)
  {
    for (int x = 0; x < c->nslot; x++)
      if (c->cls[x] == k && (st[x] & SC_MOVED))
      {
        c->move_drop[v] = c->move_drop[x];
        return;
      }
    for (int p = 0; p < c->npair; p++)
      if (c->pair[p].exact == 2 && c->cls[c->pair[p].vit] == k && (st[c->pair[p].rng] & SC_MOVED))
      {
        c->move_drop[v] = c->move_drop[c->pair[p].rng];
        return;
      }
  }
}

/* instruction i writes vreg v: the elements it indexes are others now */
static void sc_index_written(ScCtx *c, int32_t v, int i)
{
  for (int x = c->nslot; x < c->nslot + c->nvit; x++)
  {
    int p;
    if (c->vivr[x] != v || c->untracked[x])
      continue;
    for (p = 0; p < c->npair; p++)
      if (c->pair[p].vit == x && c->pair[p].step == i)
        break;
    if (p < c->npair)
      c->cur[c->pair[p].res] = c->cur[x]; /* a loop's trip ends */
    else
      sc_flush(c, c->cur, x, c->cur[x]);
    sc_refresh(c, c->cur, x);
    if (p < c->npair && c->pair[p].exact)
      sc_trip_start(c, c->cur, &c->pair[p]);
  }
}

/* the edge from block b to t (-1: leaving the function), state st: entering
   or leaving a loop */
static void sc_loop_edge(ScCtx *c, int b, int t, uint8_t *st)
{
  for (int p = 0; p < c->npair; p++)
  {
    ScPair *pr = &c->pair[p];
    int v = pr->vit, k = c->cls[v];
    if (!pr->blk[b] && t == pr->hb)
    {
      st[pr->res] = 0;
      if (pr->exact && !(st[v] & SC_VIA))
        sc_trip_start(c, st, pr);
    }
    else if (pr->blk[b] && (t < 0 || !pr->blk[t]))
    {
      if (pr->exact == 2 && b == pr->hb && t == pr->exit_b)
      {
        /* a variable bound: the trips' results are the range's (the elements
           below the bound); trips that wrote nothing leave it */
        uint8_t r = st[pr->res];
        for (int w = c->nslot; w < c->nslot + c->nvit; w++)
          if (w != v && c->cls[w] == k)
            sc_flush(c, st, w, st[w]);
        if (r & SC_VIA)
        {
          if (r & SC_MOVED)
            c->move_drop[pr->rng] = c->move_drop[v];
          st[pr->rng] = r;
        }
        for (int w = c->nslot; w < c->nslot + c->nvit; w++)
          if (c->cls[w] == k)
            sc_refresh(c, st, w);
      }
      else if (pr->exact && b == pr->hb && t == pr->exit_b)
      {
        uint8_t r = st[pr->res];
        for (int w = c->nslot; w < c->nslot + c->nvit; w++)
          if (w != v && c->cls[w] == k)
            sc_flush(c, st, w, st[w]);
        sc_fold_ranges(c, st, k);
        /* trips that wrote nothing (r clean, not 0) leave the elements */
        for (int x = 0; x < c->nslot && (!r || (r & SC_VIA)); x++)
          if (c->cls[x] == k && c->it[x].idx >= pr->lo && c->it[x].idx <= pr->hi)
          {
            if (r & SC_MOVED)
              c->move_drop[x] = c->move_drop[v];
            st[x] = r ? r | SC_VIA : 0;
          }
        for (int w = c->nslot; w < c->nslot + c->nvit; w++)
          if (c->cls[w] == k)
            sc_refresh(c, st, w);
      }
      else
      {
        sc_flush(c, st, v, st[pr->res] | st[v]);
        sc_refresh(c, st, v);
      }
      st[pr->res] = 0;
    }
  }
}

/* is x's scope ending at sel (-1: it lasts to the function's end) */
static int sc_sel(ScCtx *c, int x, int sel)
{
  return sel >= 0 ? c->end[x] == sel : c->end[x] < 0 || c->end[x] >= c->n;
}

/* the items whose scope ends at sel (-1: leaving the function) leak; reset:
   their scope is over */
static void sc_leak_point(ScCtx *c, int line, const char *where, int sel, int reset)
{
  c->lgen++;
  /* the loops' results and the elements at each index go into the elements */
  for (int p = 0; p < c->npair; p++)
  {
    ScPair *pr = &c->pair[p];
    if (sc_sel(c, pr->vit, sel) && c->cur[pr->res])
    {
      sc_flush(c, c->cur, pr->vit, c->cur[pr->res]);
      c->cur[pr->res] = 0;
    }
  }
  /* a range a loop left owning, said once per class by the first loop's
     index (whether any element is below the bound is not known: "may") */
  for (int p = 0; p < c->npair; p++)
  {
    ScPair *pr = &c->pair[p];
    int k = c->cls[pr->vit];
    if (pr->exact != 2 || !sc_sel(c, pr->vit, sel) || !c->cur[pr->rng])
      continue;
    if (c->report && (c->cur[pr->rng] & SC_OWN) && !c->it[pr->vit].silent && !c->untracked[pr->vit] &&
        c->ldone[k] != c->lgen)
    {
      c->ldone[k] = c->lgen;
      tcc_warning_c(warn_lifetime)("%i:'%s' leaks: it may still own %s %s", line, c->it[pr->vit].name,
                                   sc_what(c, pr->vit), where);
    }
    if (reset)
      c->cur[pr->rng] = 0;
  }
  /* (a vitem not written since its refresh says nothing of its own: the
     elements say it) */
  for (int x = c->nslot; x < c->nslot + c->nvit; x++)
    if (sc_sel(c, x, sel))
    {
      if (c->cur[x] & SC_VIA)
        sc_leak_ex(c, x, line, where, 1);
      sc_flush(c, c->cur, x, c->cur[x]);
      if (reset)
        c->cur[x] = SC_UNINIT | SC_N_SET;
    }
  for (int x = 0; x < c->nslot; x++)
    if (sc_sel(c, x, sel))
    {
      sc_leak_ex(c, x, line, where, 1);
      if (reset)
        c->cur[x] = SC_UNINIT | SC_N_SET;
    }
}

static void sc_block(ScCtx *c, int b, uint8_t *out)
{
  TCCIRState *ir = c->ir;
  IRBasicBlock *bb = &c->cfg->blocks[b];
  int last = bb->end_idx - 1;
  IRQuadCompact *lq = &ir->compact_instructions[last];
  c->gen++;
  for (int i = bb->start_idx; i < bb->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    IROperand src1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
    IROperand src2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    int line = q->line_num, x, call_id = -1, k = -1;
    IRLtCall *call = NULL;
    int32_t off;
    c->at = i;

    /* a local's scope ends here (at a block's start: its predecessors saw it) */
    if (i != bb->start_idx)
      for (x = 0; x < c->ni; x++)
        if (c->end[x] == i)
        {
          sc_leak_point(c, line, "at the end of its scope", i, 1);
          break;
        }

    /* an inlined method's self bound to &local: the handing over */
    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) && ir->lt_nb_iself && !irop_is_none(dest) &&
        sc_addr(c, src1, &off))
    {
      int itok = 0, ik = lt_iself_of(ir, irop_get_vreg(dest), &itok);
      if (ik)
      {
        IRLtCall fake;
        memset(&fake, 0, sizeof fake);
        fake.callee = itok;
        sc_self_arg(c, off, &fake, ik, line);
      }
    }
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      int64_t enc = irop_get_imm64_ex(ir, src2);
      call_id = TCCIR_DECODE_CALL_ID(enc), k = TCCIR_DECODE_PARAM_IDX(enc);
      call = call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
      if (call && call->known && (k -= call->implicit) >= 0 && k < 32)
      {
        if ((x = sc_value(c, src1)) >= 0)
        {
          if ((call->owned >> k) & 1)
          {
            /* released: by the deallocator its type names (owned(fclose))? */
            int p = call->drop_dealloc ? call->drop_dealloc : call->callee;
            if (((call->drop >> k) & 1) && c->report && !c->it[x].silent && c->it[x].dealloc && p &&
                p != c->it[x].dealloc)
              tcc_warning_c(warn_lifetime)("%i:'%s' is released by %s, not %s", line, c->it[x].name,
                                           get_tok_str(c->it[x].dealloc, NULL), get_tok_str(p, NULL));
            sc_move(c, x, (call->drop >> k) & 1, line);
          }
          else
          {
            sc_use(c, x, line);
            if (c->report && !c->it[x].silent && ((call->nonnull >> k) & 1) && sc_null_said(c, x) && call->callee)
              tcc_warning_c(warn_lifetime)("%i:'%s' %s %s, and is passed as argument %d of '%s', which is not maybe",
                                           line, c->it[x].name, (c->cur[x] & SC_N_SET) ? "may be" : "is",
                                           (c->it[x].kind & LTV_HANDLE) ? "invalid" : "null", k + 1,
                                           get_tok_str(call->callee, NULL));
          }
        }
        else if (sc_addr(c, src1, &off) && ((call->dropobj >> k) & 1))
        {
          /* regfree(&re): the object is released -- by its own deallocator? */
          int p = call->dropobj_dealloc ? call->dropobj_dealloc : call->callee;
          for (x = 0; x < c->ni; x++)
            if (!c->untracked[x] && (c->it[x].kind & LTV_OBJECT) && c->it[x].base <= off &&
                off < c->it[x].base + c->it[x].base_size)
            {
              if (c->report && !c->it[x].silent && c->it[x].dealloc && p && p != c->it[x].dealloc)
                tcc_warning_c(warn_lifetime)("%i:'%s' is released by %s, not %s", line, c->it[x].name,
                                             get_tok_str(c->it[x].dealloc, NULL), get_tok_str(p, NULL));
              sc_move(c, x, 1, line);
            }
        }
        else if (sc_addr(c, src1, &off) && (((call->vmem | call->dropmem) >> k) & 1))
          sc_self_arg(c, off, call, ((call->dropmem >> k) & 1) ? 3 : ((call->keep >> k) & 1) ? 2 : 1, line);
        else if (sc_addr(c, src1, &off) && ((call->keep >> k) & 1) && !((call->outown >> k) & 1))
        {
          /* read through a pointer that cannot write: a use */
          for (x = 0; x < c->ni; x++)
            if (!c->untracked[x] && c->it[x].base <= off && off < c->it[x].base + c->it[x].base_size &&
                (c->it[x].kind & LTV_OBJECT))
              sc_use(c, x, line);
        }
        else if (sc_addr(c, src1, &off) && ((call->outown >> k) & 1))
        {
          /* the local's address to an owned out-parameter: its items are written */
          c->lgen++;
          for (x = 0; x < c->ni; x++)
            if (!c->untracked[x] && c->it[x].base <= off && off < c->it[x].base + c->it[x].base_size)
            {
              if ((call->outown_out >> k) & 1)
                sc_leak(c, x, line, "when it is written through its address");
              else
                sc_use(c, x, line);
              c->cur[x] = SC_OWN | (((call->outown_maybe >> k) & 1) ? SC_N_NULL | SC_N_SET : SC_N_SET);
              if (c->kind[x] == SC_K_VIT)
                c->cur[x] |= SC_VIA;
              c->out_call[x] = -1;
              if (((call->outown_out >> k) & 1) && !call->ret_maybe)
              {
                c->cur[x] |= SC_NULL;
                c->out_call[x] = call_id;
              }
            }
        }
      }
    }
    else if (q->op == TCCIR_OP_RETURNVALUE && (x = sc_value(c, src1)) >= 0)
    {
      if (c->ret_owned)
        sc_move(c, x, 0, line);
      else
        sc_use(c, x, line);
    }
    else if (q->op == TCCIR_OP_STORE)
    {
      int to = sc_access(c, dest), from = sc_value(c, src1);
      if (to >= 0)
      {
        uint8_t nb = SC_N_SET;
        int pos = sc_temp(c, src1);
        sc_leak(c, to, line, "when it is assigned again");
        if (irop_is_immediate(src1) && !src1.is_lval)
        {
          int64_t v = irop_get_imm64_ex(ir, src1);
          int inval = (c->it[to].kind & LTV_HANDLE) ? v < 0 : v == 0;
          c->cur[to] = SC_NULL | (inval ? SC_N_NULL : SC_N_SET);
        }
        else
        {
          if (from >= 0 && from != to)
          {
            nb = c->cur[from] & (SC_N_NULL | SC_N_SET);
            sc_move(c, from, 0, line);
          }
          else if (pos >= 0 && c->tcid[pos] >= 0 && c->tcid[pos] < ir->lt_nb_calls && ir->lt_calls[c->tcid[pos]].ret_maybe)
            nb = SC_N_NULL | SC_N_SET;
          c->cur[to] = SC_OWN | nb;
        }
        if (c->kind[to] == SC_K_VIT)
          c->cur[to] |= SC_VIA;
      }
      else if (from >= 0 && ve_ostore(ir, i))
        sc_move(c, from, 0, line); /* into an owned pointer in memory */
    }
    else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && (x = sc_value(c, src1)) >= 0 && !irop_is_none(dest) &&
             irop_dest_defines_vreg(dest) && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) != TCCIR_VREG_TYPE_TEMP)
    {
      /* into a variable: owned, it moves */
      int v = lt_var_by_vreg(ir, irop_get_vreg(dest));
      if (v >= 0 && (ir->lt_vars[v].kind & LTV_OWNED))
        sc_move(c, x, 0, line);
    }

    /* a pointer read from an item, dereferenced */
    for (int so = 0; so < 3; so++)
    {
      IROperand o = so == 0 ? src1 : so == 1 ? src2 : (q->op == TCCIR_OP_STORE ? dest : irop_make_none());
      int pos;
      if (irop_is_none(o) || !o.is_lval || irop_get_tag(o) != IROP_TAG_VREG || (pos = sc_temp(c, o)) < 0 ||
          (x = c->titem[pos]) < 0)
        continue;
      sc_use(c, x, line);
      if (c->report && !c->it[x].silent && sc_null_said(c, x) && !(c->it[x].kind & LTV_HANDLE))
        tcc_warning_c(warn_lifetime)("%i:'%s' %s null where it is dereferenced", line, c->it[x].name,
                                     (c->cur[x] & SC_N_SET) ? "may be" : "is");
    }

    /* temps: a frame address (at a variable index too), an item's value, a call's result */
    if (!irop_is_none(dest) && irop_dest_defines_vreg(dest) &&
        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)), sp;
      if (pos < c->ntemps)
      {
        int32_t fa = SC_NOADDR, it = -1, cid = -1, ix = -1, isc = 0, xat = i, dl = 0;
        if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA) && sc_addr(c, src1, &off))
          fa = off;
        else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && sc_addr(c, src1, &off) &&
                 irop_get_tag(src2) == IROP_TAG_IMM32)
          fa = off + (q->op == TCCIR_OP_ADD ? 1 : -1) * (int32_t)irop_get_imm64_ex(ir, src2);
        else if (q->op == TCCIR_OP_ADD && c->nvit && sc_addr(c, src1, &off) &&
                 (ix = ix_scaled_d(ir, bb->start_idx, i, src2, &isc, &dl)) >= 0)
          fa = off;
        else if ((q->op == TCCIR_OP_ASSIGN ||
                  ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_get_tag(src2) == IROP_TAG_IMM32)) &&
                 !src1.is_lval && (sp = sc_temp(c, src1)) >= 0 && c->tfa[sp] != SC_NOADDR && c->tix[sp] >= 0)
        {
          int32_t d = q->op == TCCIR_OP_ASSIGN ? 0 : (int32_t)irop_get_imm64_ex(ir, src2);
          fa = c->tfa[sp] + (q->op == TCCIR_OP_SUB ? -d : d);
          ix = c->tix[sp], isc = c->tisc[sp], xat = c->txat[sp], dl = c->tdelta[sp];
        }
        if (fa == SC_NOADDR)
          ix = -1;
        if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN)
          it = (sp = sc_temp(c, src1)) >= 0 && !src1.is_lval ? c->titem[sp] : sc_access(c, src1);
        if (q->op == TCCIR_OP_FUNCCALLVAL && cfg->has_src2)
          cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
        c->tfa[pos] = fa, c->titem[pos] = it, c->tcid[pos] = cid;
        c->tix[pos] = ix, c->tisc[pos] = isc, c->txat[pos] = xat, c->tdelta[pos] = dl;
        c->tgen[pos] = c->gen;
      }
    }
    /* an index variable written */
    else if (c->nvit && !irop_is_none(dest) && irop_dest_defines_vreg(dest))
      sc_index_written(c, irop_get_vreg(dest), i);
  }

  /* a scope that ends right after this block: checked before paths join */
  if (bb->end_idx < c->n)
    for (int x = 0; x < c->ni; x++)
      if (c->end[x] == bb->end_idx)
      {
        sc_leak_point(c, ir->compact_instructions[bb->end_idx].line_num, "at the end of its scope", bb->end_idx, 1);
        break;
      }
  for (int s = 0; s <= bb->num_succs; s++)
    memcpy(out + s * c->ni, c->cur, c->ni);

  /* `if (b.buf)`, `if (fds[0] < 0)`, `if (pipe(fds) < 0)`: what each edge holds */
  int target = (lq->op == TCCIR_OP_JUMP || lq->op == TCCIR_OP_JUMPIF) ? (int)tcc_ir_op_dest_imm(ir, lq) : -1;
  if (lq->op == TCCIR_OP_JUMPIF && last > bb->start_idx)
  {
    IRQuadCompact *tq = &ir->compact_instructions[last - 1];
    int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, lq));
    IROperand a1 = tq->op == TCCIR_OP_TEST_ZERO || tq->op == TCCIR_OP_CMP ? tcc_ir_op_get_src1(ir, tq) : irop_make_none();
    int64_t k0 = tq->op == TCCIR_OP_CMP && irop_is_immediate(tcc_ir_op_get_src2(ir, tq))
                     ? irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, tq))
                     : (tq->op == TCCIR_OP_TEST_ZERO ? 0 : 1 << 30);
    int x, ap;
    c->at = last - 1;
    x = irop_is_none(a1) ? -1 : sc_value(c, a1), ap = irop_is_none(a1) ? -1 : sc_temp(c, a1);
    int taken_block = target >= 0 && target < c->n ? c->cfg->instr_to_block[target] : -1;
    for (int s = 0; s <= bb->num_succs; s++)
    {
      int taken = s < bb->num_succs ? bb->succs[s] == taken_block : target >= c->n;
      int inv = -1; /* the edge holds null / an invalid handle (1), a value (0) */
      if (x >= 0 && !(c->it[x].kind & LTV_HANDLE) && k0 == 0 && (cond == TOK_EQ || cond == TOK_NE))
        inv = (taken == (cond == TOK_EQ));
      else if (x >= 0 && (c->it[x].kind & LTV_HANDLE))
      {
        int t_inv = (k0 == 0 && cond == TOK_LT) || (k0 == -1 && (cond == TOK_EQ || cond == TOK_LE)) ? 1
                    : (k0 == 0 && cond == TOK_GE) || (k0 == -1 && (cond == TOK_NE || cond == TOK_GT)) ? 0
                                                                                                    : -1;
        if (t_inv >= 0)
          inv = taken ? t_inv : !t_inv;
      }
      if (inv >= 0)
      {
        uint8_t *st = &out[s * c->ni + x];
        uint8_t own = *st & SC_OWNBITS, fl = *st & (SC_VIA | SC_VAGUE);
        if (inv)
          *st = ((own & SC_OWN) ? (own & ~SC_OWN) | SC_NULL : own) | SC_N_NULL | fl;
        else
          *st = ((own & SC_NULL) && own != SC_NULL ? own & ~SC_NULL : own) | SC_N_SET | fl;
      }
      /* an out-parameter's status: 0 / >= 0 success */
      int fail = -1;
      if (k0 == 0 && (cond == TOK_NE || cond == TOK_LT))
        fail = taken;
      else if (k0 == 0 && (cond == TOK_EQ || cond == TOK_GE))
        fail = !taken;
      if (ap >= 0 && c->tcid[ap] >= 0 && fail >= 0)
        for (int y = 0; y < c->ni; y++)
          if (c->out_call[y] == c->tcid[ap])
          {
            uint8_t *st = &out[s * c->ni + y];
            if (*st & (SC_OWN | SC_NULL))
              *st = (*st & ~SC_OWNBITS) | (fail ? SC_NULL : SC_OWN);
          }
    }
  }
  /* entering and leaving the loops over an index */
  if (c->npair)
    for (int s = 0; s < bb->num_succs; s++)
      sc_loop_edge(c, b, bb->succs[s], out + s * c->ni);

  /* leaving the function */
  if (lq->op == TCCIR_OP_RETURNVALUE || lq->op == TCCIR_OP_RETURNVOID ||
      ((lq->op == TCCIR_OP_JUMP || lq->op == TCCIR_OP_JUMPIF) && target >= c->n) ||
      (bb->end_idx == c->n && bb->num_succs == 0))
  {
    uint8_t *save = c->cur;
    c->cur = out + bb->num_succs * c->ni;
    if (c->npair)
      sc_loop_edge(c, b, -1, c->cur);
    if (lq->op == TCCIR_OP_RETURNVALUE || lq->op == TCCIR_OP_RETURNVOID)
      sc_leak_point(c, lq->line_num, "at return", -1, 0);
    else
      sc_leak_point(c, ir->lt_end_line ? ir->lt_end_line : lq->line_num, "at the end of its scope", -1, 0);
    c->cur = save;
  }
}

/* A loop's variable bound v as a vreg holding the same value in every loop
   over it: a parameter never written, or a local written once with a copy
   of one (`int m = n;`); 0 when it may change. */
static int32_t sc_bound_root(TCCIRState *ir, int n, int32_t v)
{
  for (int depth = 0; depth < 4; depth++)
  {
    int nd = 0, di = -1;
    IRQuadCompact *q;
    IROperand s;
    if (lt_vreg_addrtaken(ir, v))
      return 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *qi = &ir->compact_instructions[i];
      IROperand d = irop_config[qi->op].has_dest ? tcc_ir_op_get_dest(ir, qi) : irop_make_none();
      if (!irop_is_none(d) && irop_dest_defines_vreg(d) && irop_get_vreg(d) == v)
        nd++, di = i;
    }
    if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_PARAM)
      return nd ? 0 : v;
    if (TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_VAR || nd != 1)
      return 0;
    q = &ir->compact_instructions[di];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
      return 0;
    s = tcc_ir_op_get_src1(ir, q);
    if (irop_is_none(s) || !irop_is_vreg_value(s) || lt_is_addr(s) || (v = irop_get_vreg(s)) < 0 ||
        (TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_VAR && TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_PARAM))
      return 0;
  }
  return 0;
}

/* The loops summarised for vitem v (see above): a pair each, at most
   SC_MAX_PAIR.  touch: (instruction, local's base, vitem or -1) for every
   access to a followed local. */
static void sc_find_loops(ScCtx *c, int v, const int32_t *touch, int ntouch)
{
  TCCIRState *ir = c->ir;
  IRCFG *cfg = c->cfg;
  int nb = cfg->num_blocks, n = c->n;
  int32_t iv = c->vivr[v];
  int *work = NULL;
  for (int s = 0; s < n && c->npair < SC_MAX_PAIR; s++)
  {
    IRQuadCompact *q = &ir->compact_instructions[s];
    if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q), s1 = tcc_ir_op_get_src1(ir, q), s2 = tcc_ir_op_get_src2(ir, q);
    if (irop_is_none(d) || !irop_dest_defines_vreg(d) || irop_get_vreg(d) != iv || !irop_is_immediate(s2) ||
        s2.is_lval)
      continue;
    int64_t step = irop_get_imm64_ex(ir, s2);
    if (q->op == TCCIR_OP_SUB)
      step = -step;
    int sb = cfg->instr_to_block[s];
    if (!step || step != (int32_t)step ||
        ix_var(ir, cfg->blocks[sb].start_idx, s, s1) != iv)
      continue;
    /* the trip's end: the block jumping back to the header */
    IRQuadCompact *lq = &ir->compact_instructions[cfg->blocks[sb].end_idx - 1];
    if (lq->op != TCCIR_OP_JUMP)
      continue;
    int target = (int)tcc_ir_op_dest_imm(ir, lq);
    if (target < 0 || target > s || target >= n)
      continue;
    int hb = cfg->instr_to_block[target];
    if (cfg->blocks[hb].start_idx != target || ++c->ncand > SC_MAX_CAND)
      continue;
    /* the loop: the blocks reaching the back edge without the header */
    uint8_t *blk = tcc_mallocz(nb + 1);
    int nw = 0, ok = 1;
    if (!work)
      work = tcc_malloc((nb + 1) * sizeof(int));
    blk[hb] = 1;
    if (!blk[sb])
      blk[sb] = 1, work[nw++] = sb;
    while (nw)
    {
      IRBasicBlock *bb = &cfg->blocks[work[--nw]];
      for (int p = 0; p < bb->num_preds; p++)
        if (!blk[bb->preds[p]])
          blk[bb->preds[p]] = 1, work[nw++] = bb->preds[p];
    }
    /* every trip ends with the step: the header's only predecessor in the loop */
    for (int p = 0; p < cfg->blocks[hb].num_preds; p++)
      if (blk[cfg->blocks[hb].preds[p]] && cfg->blocks[hb].preds[p] != sb)
        ok = 0;
    /* i written nowhere else in the loop; the array reached only at i; its
       scope does not end inside */
    for (int b = 0; b < nb && ok; b++)
      if (blk[b])
        for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx && ok; i++)
        {
          IRQuadCompact *qi = &ir->compact_instructions[i];
          if (i != s && irop_config[qi->op].has_dest)
          {
            IROperand di = tcc_ir_op_get_dest(ir, qi);
            if (!irop_is_none(di) && irop_dest_defines_vreg(di) && irop_get_vreg(di) == iv)
              ok = 0;
          }
          if (c->end[v] == i)
            ok = 0;
        }
    for (int t = 0; t < ntouch && ok; t++)
      if (blk[cfg->instr_to_block[touch[3 * t]]] && touch[3 * t + 1] == c->it[v].base &&
          (touch[3 * t + 2] < 0 || c->vivr[touch[3 * t + 2]] != iv || c->vdelta[touch[3 * t + 2]] != c->vdelta[v]))
        ok = 0;
    if (!ok)
    {
      tcc_free(blk);
      continue;
    }
    ScPair *pr = &c->pair[c->npair++];
    memset(pr, 0, sizeof *pr);
    pr->vit = v;
    pr->hb = hb;
    pr->step = s;
    pr->blk = blk;
    /* exact: `for (i = C0; i < N; i++)` (or counting down), left only by its
       test, every element visited inside the array */
    IRBasicBlock *h = &cfg->blocks[hb];
    IRQuadCompact *q1 = &ir->compact_instructions[h->end_idx - 1];
    int t2 = q1->op == TCCIR_OP_JUMPIF ? (int)tcc_ir_op_dest_imm(ir, q1) : -1;
    /* the test's way out: a block, or the function's end (-1) */
    int e = -2, in = -1;
    if (h->num_succs == 2 && blk[h->succs[0]] != blk[h->succs[1]])
      e = blk[h->succs[0]] ? h->succs[1] : h->succs[0], in = blk[h->succs[0]] ? h->succs[0] : h->succs[1];
    else if (h->num_succs == 1 && t2 >= n && blk[h->succs[0]])
      e = -1, in = h->succs[0];
    if ((step == 1 || step == -1) && h->end_idx - h->start_idx == 2 && e != -2 && h->num_preds == 2)
    {
      IRQuadCompact *q0 = &ir->compact_instructions[h->start_idx];
      IROperand a = q0->op == TCCIR_OP_CMP ? tcc_ir_op_get_src1(ir, q0) : irop_make_none();
      IROperand nk = q0->op == TCCIR_OP_CMP ? tcc_ir_op_get_src2(ir, q0) : irop_make_none();
      int pre = h->preds[0] == sb ? h->preds[1] : h->preds[0];
      int32_t bv = -1;
      if (q1->op != TCCIR_OP_JUMPIF || irop_is_none(a) || !irop_is_vreg_value(a) || irop_get_vreg(a) != iv ||
          irop_is_none(nk) || blk[pre])
        continue;
      if (!irop_is_immediate(nk) || nk.is_lval)
      {
        /* a variable bound: a local or parameter the loop does not write */
        bv = ix_var(ir, h->start_idx, h->start_idx, nk);
        if (bv < 0 || bv == iv || step != 1)
          continue;
        for (int b = 0; b < nb && ok; b++)
          if (blk[b])
            for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx && ok; i++)
            {
              IRQuadCompact *qi = &ir->compact_instructions[i];
              IROperand di = irop_config[qi->op].has_dest ? tcc_ir_op_get_dest(ir, qi) : irop_make_none();
              if (!irop_is_none(di) && irop_dest_defines_vreg(di) && irop_get_vreg(di) == bv)
                ok = 0;
            }
        if (!ok)
          continue;
      }
      int64_t nv = bv < 0 ? irop_get_imm64_ex(ir, nk) : 0, c0, lo, hi;
      int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, q1));
      int taken = t2 >= n ? -1 : t2 >= 0 ? cfg->instr_to_block[t2] : -3;
      /* the condition that stays in the loop */
      if (taken == e)
        cond = cond == TOK_GE ? TOK_LT : cond == TOK_UGE ? TOK_ULT : cond == TOK_GT ? TOK_LE : cond == TOK_UGT ? TOK_ULE
             : cond == TOK_LT ? TOK_GE : cond == TOK_LE ? TOK_GT : 0;
      else if (taken != in)
        continue;
      /* left only by the header's test */
      for (int b = 0; b < nb && ok; b++)
      {
        IRBasicBlock *bb = &cfg->blocks[b];
        IRQuadCompact *bq;
        if (!blk[b])
          continue;
        bq = &ir->compact_instructions[bb->end_idx - 1];
        if (!bb->num_succs || bq->op == TCCIR_OP_RETURNVALUE || bq->op == TCCIR_OP_RETURNVOID ||
            ((bq->op == TCCIR_OP_JUMP || bq->op == TCCIR_OP_JUMPIF) && tcc_ir_op_dest_imm(ir, bq) >= n && b != hb))
          ok = 0;
        for (int k = 0; k < bb->num_succs; k++)
          if (!blk[bb->succs[k]] && !(b == hb && bb->succs[k] == e))
            ok = 0;
      }
      /* the start: a constant stored to i last before the loop */
      int i;
      for (i = cfg->blocks[pre].end_idx - 1; i >= cfg->blocks[pre].start_idx; i--)
      {
        IRQuadCompact *qi = &ir->compact_instructions[i];
        IROperand di = irop_config[qi->op].has_dest ? tcc_ir_op_get_dest(ir, qi) : irop_make_none();
        if (!irop_is_none(di) && irop_dest_defines_vreg(di) && irop_get_vreg(di) == iv)
          break;
      }
      if (!ok || i < cfg->blocks[pre].start_idx || ir->compact_instructions[i].op != TCCIR_OP_ASSIGN)
        continue;
      IROperand iv0 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[i]);
      if (!irop_is_immediate(iv0) || iv0.is_lval)
        continue;
      c0 = irop_get_imm64_ex(ir, iv0);
      if (bv >= 0)
      {
        /* `for (i = C0; i < n; i++)`: the elements C0.. below n, n not known */
        if (cond != TOK_LT && cond != TOK_ULT && cond != TOK_LE && cond != TOK_ULE)
          continue;
        lo = c0 + c->vdelta[v], hi = c->it[c->cls[v]].nelem - 1;
        if (lo < 0 || lo > hi)
          continue;
        pr->exact = 2, pr->lo = (int)lo, pr->hi = (int)hi, pr->exit_b = e, pr->bcond = cond;
        pr->broot = sc_bound_root(ir, n, bv);
        continue;
      }
      if (step == 1 && (cond == TOK_LT || cond == TOK_ULT || cond == TOK_LE || cond == TOK_ULE))
        lo = c0, hi = cond == TOK_LT || cond == TOK_ULT ? nv - 1 : nv;
      else if (step == -1 && (cond == TOK_GE || cond == TOK_GT))
        hi = c0, lo = cond == TOK_GE ? nv : nv + 1;
      else
        continue;
      /* `a[i + d]` visits lo + d .. hi + d */
      lo += c->vdelta[v], hi += c->vdelta[v];
      if (lo < 0 || lo > hi || hi >= c->it[c->cls[v]].nelem)
        continue;
      pr->exact = 1, pr->lo = (int)lo, pr->hi = (int)hi, pr->exit_b = e;
    }

  }
  tcc_free(work);
}

/* item x of a local stops being followed because an element is reached at an
   index the check cannot follow (`a[i * 2]`, `&a[i]` handed on): said once
   per local, at the instruction that did it */
static void sc_escape_said(ScCtx *c, int x, int line)
{
  char name[40];
  int n = 0;
  for (int y = 0; y < c->nslot; y++)
    if (c->it[y].base == c->it[x].base && c->untracked[y])
      return; /* said for this local already */
  if (c->it[x].silent)
    return;
  while (c->it[x].name[n] && c->it[x].name[n] != '[' && c->it[x].name[n] != '.' && n < (int)sizeof name - 1)
    name[n] = c->it[x].name[n], n++;
  name[n] = 0;
  tcc_state->diag_noabort++;
  tcc_warning_c(warn_lifetime)("%i:'%s' is reached in a way the lifetime check does not follow: what it owns is not "
                               "checked",
                               line, name);
  tcc_state->diag_noabort--;
}

void tcc_ir_slot_check(TCCIRState *ir, Sym *func)
{
  ScCtx c;
  int n = ir->next_instruction_index, nb, nslot = ir->lt_nb_slots;
  if (!n || !nslot)
    return;
  memset(&c, 0, sizeof c);
  c.ir = ir;
  c.n = n;
  c.nslot = nslot;
  c.ntemps = ir->next_temporary_variable;
  c.cfg = tcc_ir_cfg_build(ir);
  if (!c.cfg)
    return;
  tcc_ir_cfg_compute_rpo(c.cfg);
  nb = c.cfg->num_blocks;
  /* the items, the vitems (SC_MAX_VIT) and the loops' results (SC_MAX_PAIR) */
  int cap = nslot + SC_MAX_VIT + 2 * SC_MAX_PAIR + 1;
  c.it = tcc_malloc(cap * sizeof(IRLtSlot));
  memcpy(c.it, ir->lt_slots, nslot * sizeof(IRLtSlot));
  c.untracked = tcc_mallocz(cap);
  c.kind = tcc_mallocz(cap);
  c.cls = tcc_malloc(cap * sizeof(int));
  c.vivr = tcc_malloc(cap * sizeof(int32_t));
  c.vdelta = tcc_mallocz(cap * sizeof(int32_t));
  c.multi = tcc_mallocz(nslot + 1);
  c.ldone = tcc_mallocz((nslot + 1) * sizeof(uint32_t));
  memset(c.vivr, 0xff, cap * sizeof(int32_t));
  /* each element's class: its array's element 0 */
  for (int x = 0; x < cap; x++)
    c.cls[x] = -1;
  for (int x = 0; x < nslot; x++)
    if (c.it[x].esize > 0)
      for (int y = 0; y < nslot; y++)
        if (c.it[y].esize == c.it[x].esize && c.it[y].idx == 0 && c.it[y].base == c.it[x].base &&
            c.it[y].off == c.it[x].off - c.it[x].idx * c.it[x].esize)
        {
          c.cls[x] = y;
          break;
        }
  /* a local's address that goes anywhere but an owned out-parameter, or an
     element at an index that is not a variable: its items are not followed.
     touch: what each instruction reaches of a followed local, for the loops */
  int32_t *touch = NULL;
  int ntouch = 0, tcap = 0;
  {
    int32_t *pfa = tcc_malloc((c.ntemps + 1) * 5 * sizeof(int32_t));
    int32_t *pix = pfa + (c.ntemps + 1), *pisc = pix + (c.ntemps + 1), *pxat = pisc + (c.ntemps + 1);
    int32_t *pdl = pxat + (c.ntemps + 1);
    for (int t = 0; t <= c.ntemps; t++)
      pfa[t] = SC_NOADDR, pix[t] = -1;
#define SC_ESCAPE(a, say)                                                                                                \
  do                                                                                                                     \
  {                                                                                                                      \
    for (int x_ = 0; x_ < nslot; x_++)                                                                                   \
      if (c.it[x_].base <= (a) && (a) < c.it[x_].base + c.it[x_].base_size)                                              \
      {                                                                                                                  \
        if ((say) && !c.untracked[x_])                                                                                   \
          sc_escape_said(&c, x_, q->line_num);                                                                           \
        c.untracked[x_] = 1;                                                                                             \
      }                                                                                                                  \
  } while (0)
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      const IRRegistersConfig *cfg = &irop_config[q->op];
      IROperand ops[3] = {cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none(),
                          cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none(),
                          cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none()};
      int32_t dv = irop_is_none(ops[0]) || !irop_dest_defines_vreg(ops[0]) ? -1 : irop_get_vreg(ops[0]);
      int dpos = dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP ? TCCIR_DECODE_VREG_POSITION(dv) : -1;
      int bs = c.cfg->blocks[c.cfg->instr_to_block[i]].start_idx;
      int32_t a = SC_NOADDR;
      int is_addr_src = 0, sp = -1;
      if (dpos >= c.ntemps)
        dpos = -1;
      /* src1's address value */
      if (!irop_is_none(ops[1]) && !ops[1].is_lval)
      {
        int32_t sv = irop_get_vreg(ops[1]);
        if (lt_is_addr(ops[1]) && sv < 0)
          a = (int32_t)irop_get_imm64_ex(ir, ops[1]), is_addr_src = 1;
        else if (sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP &&
                 TCCIR_DECODE_VREG_POSITION(sv) < c.ntemps && pfa[TCCIR_DECODE_VREG_POSITION(sv)] != SC_NOADDR)
          sp = TCCIR_DECODE_VREG_POSITION(sv), a = pfa[sp], is_addr_src = 1;
      }
      /* what the instruction reads or writes of a followed local */
      for (int k = 0; k < 3; k++)
      {
        IROperand o = ops[k];
        int32_t fo, ov, w = -1, base = SC_NOADDR;
        int tp;
        if (irop_is_none(o) || !o.is_lval || (k == 0 && dv >= 0))
          continue;
        ov = irop_get_vreg(o);
        if (irop_get_tag(o) == IROP_TAG_STACKOFF && irop_is_local(o) && ov < 0)
          fo = (int32_t)irop_get_imm64_ex(ir, o);
        else if (irop_get_tag(o) == IROP_TAG_VREG && ov >= 0 && TCCIR_DECODE_VREG_TYPE(ov) == TCCIR_VREG_TYPE_TEMP &&
                 (tp = TCCIR_DECODE_VREG_POSITION(ov)) < c.ntemps && pfa[tp] != SC_NOADDR)
        {
          fo = pfa[tp];
          if (pix[tp] >= 0)
          {
            /* an element at a variable index (plus a constant): its class's
               vitem -- an element at fo, its index added to the constant */
            int e0 = -1, hit = 0, dl = pdl[tp];
            for (int x = 0; x < nslot && e0 < 0; x++)
              if (c.it[x].esize == pisc[tp] && c.it[x].off == fo && c.cls[x] >= 0)
                e0 = c.cls[x], dl += c.it[x].idx;
            if (ix_written(ir, pxat[tp] + 1, i, pix[tp]))
              hit = 1;
            else if (e0 >= 0)
            {
              w = -1;
              for (int x = nslot; x < nslot + c.nvit; x++)
                if (c.cls[x] == e0 && c.vivr[x] == pix[tp] && c.vdelta[x] == dl)
                  w = x;
              int in = ix_name(ir, pix[tp]);
              if (w < 0 && in && c.nvit < SC_MAX_VIT)
              {
                IRLtSlot *s = &c.it[w = nslot + c.nvit++];
                char dn[16] = "";
                *s = c.it[e0];
                if (dl)
                  snprintf(dn, sizeof dn, " %c %d", dl < 0 ? '-' : '+', dl < 0 ? -dl : dl);
                snprintf(s->name, sizeof s->name, "%.*s[%s%s]%s", (int)c.it[e0].pre, c.it[e0].name,
                         get_tok_str(in, NULL), dn, c.it[e0].name + c.it[e0].suf);
                s->idx = -1;
                c.cls[w] = e0;
                c.vivr[w] = pix[tp];
                c.vdelta[w] = dl;
                c.kind[w] = SC_K_VIT;
              }
              if (w < 0)
                hit = 1;
            }
            else
            {
              /* no class there: an owned item it may reach is not followed */
              for (int x = 0; x < nslot; x++)
                if (c.it[x].base <= fo && fo < c.it[x].base + c.it[x].base_size &&
                    (irop_get_btype(o) == IROP_BTYPE_STRUCT || (c.it[x].off - fo) % pisc[tp] == 0))
                  hit = 1;
              if (!hit)
                continue;
            }
            if (hit)
            {
              SC_ESCAPE(fo, 1);
              continue;
            }
          }
        }
        else
          continue;
        for (int x = 0; x < nslot; x++)
          if (c.it[x].base <= fo && fo < c.it[x].base + c.it[x].base_size)
            base = c.it[x].base;
        if (base == SC_NOADDR)
          continue;
        if (ntouch >= SC_MAX_TOUCH)
          continue;
        if (ntouch >= tcap)
        {
          tcap = tcap ? 2 * tcap : 64;
          touch = tcc_realloc(touch, tcap * 3 * sizeof(int32_t));
        }
        touch[3 * ntouch] = i, touch[3 * ntouch + 1] = base, touch[3 * ntouch + 2] = w;
        ntouch++;
      }
      if (dpos >= 0)
        pix[dpos] = -1;
      if (is_addr_src)
      {
        int escape = 1, ixsrc = sp >= 0 && pix[sp] >= 0, say = 0;
        if (dpos >= 0 && (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA))
        {
          pfa[dpos] = a, escape = 0;
          if (ixsrc)
            pix[dpos] = pix[sp], pisc[dpos] = pisc[sp], pxat[dpos] = pxat[sp], pdl[dpos] = pdl[sp];
        }
        else if (dpos >= 0 && (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB))
        {
          int32_t iv;
          int sc, dl;
          if (irop_get_tag(ops[2]) == IROP_TAG_IMM32)
          {
            pfa[dpos] = a + (q->op == TCCIR_OP_ADD ? 1 : -1) * (int32_t)irop_get_imm64_ex(ir, ops[2]), escape = 0;
            if (ixsrc)
              pix[dpos] = pix[sp], pisc[dpos] = pisc[sp], pxat[dpos] = pxat[sp], pdl[dpos] = pdl[sp];
          }
          else if (!ixsrc && q->op == TCCIR_OP_ADD && (iv = ix_scaled_d(ir, bs, i, ops[2], &sc, &dl)) >= 0)
            pfa[dpos] = a, pix[dpos] = iv, pisc[dpos] = sc, pxat[dpos] = i, pdl[dpos] = dl, escape = 0;
          else
            say = 1; /* an index that is not a variable: escape (not followed), said */
        }
        else if ((q->op == TCCIR_OP_FUNCPARAMVAL ||
                  ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) && ir->lt_nb_iself)) && !ixsrc)
        {
          if (q->op != TCCIR_OP_FUNCPARAMVAL)
          {
            /* bound to an inlined method's self: as the call's argument */
            int itok;
            escape = !lt_iself_of(ir, irop_get_vreg(tcc_ir_op_get_dest(ir, q)), &itok);
          }
          else
          {
            int64_t enc = irop_get_imm64_ex(ir, ops[2]);
            int cid = TCCIR_DECODE_CALL_ID(enc), kk = TCCIR_DECODE_PARAM_IDX(enc);
            IRLtCall *call = cid >= 0 && cid < ir->lt_nb_calls ? &ir->lt_calls[cid] : NULL;
            escape = !(call && call->known && (kk -= call->implicit) >= 0 && kk < 32 &&
                       (((call->outown | call->dropobj | call->keep | call->vmem | call->dropmem) >> kk) & 1));
          }
          if (!escape && ntouch < SC_MAX_TOUCH)
          {
            /* the local reached through its address */
            int32_t base = SC_NOADDR;
            for (int x = 0; x < nslot; x++)
              if (c.it[x].base <= a && a < c.it[x].base + c.it[x].base_size)
                base = c.it[x].base;
            if (base != SC_NOADDR)
            {
              if (ntouch >= tcap)
              {
                tcap = tcap ? 2 * tcap : 64;
                touch = tcc_realloc(touch, tcap * 3 * sizeof(int32_t));
              }
              touch[3 * ntouch] = i, touch[3 * ntouch + 1] = base, touch[3 * ntouch + 2] = -1;
              ntouch++;
            }
          }
        }
        /* an element's address at a variable index going anywhere is said; the
           whole local's address to a call (a method's self) is not */
        if (escape && ixsrc)
          say = 1;
        if (escape)
          SC_ESCAPE(a, say);
      }
    }
#undef SC_ESCAPE
    tcc_free(pfa);
  }
  /* a vitem of a local not followed is not either; a class indexed by two
     variables never says a[i] leaks */
  for (int x = nslot; x < nslot + c.nvit; x++)
  {
    if (c.untracked[c.cls[x]])
      c.untracked[x] = 1;
    for (int y = nslot; y < x; y++)
      if (c.cls[y] == c.cls[x])
        c.multi[c.cls[x]] = 1;
  }
  for (int x = nslot; x < nslot + c.nvit; x++)
    if (c.multi[c.cls[x]])
    {
      sc_find_guards(&c);
      break;
    }
  /* each item's scope end: its local's frame object */
  c.ni = nslot + c.nvit;
  c.end = tcc_malloc(cap * sizeof(int));
  for (int x = 0; x < c.ni; x++)
  {
    c.end[x] = -1;
    for (int k = 0; k < ir->frame_obj_count; k++)
    {
      int32_t *o = &ir->frame_objs[FRAME_OBJ_WORDS * k];
      if (o[0] <= c.it[x].base && c.it[x].base < o[0] + (o[2] > 0 ? o[2] : 1))
      {
        int e = o[3] >> 4;
        c.end[x] = e && e < (1 << 26) ? e - 1 : -1;
        break;
      }
    }
  }
  /* the loops over each index; their results get a state entry each */
  if (c.nvit)
  {
    c.pair = tcc_mallocz(SC_MAX_PAIR * sizeof(ScPair));
    for (int x = nslot; x < nslot + c.nvit; x++)
      if (!c.untracked[x])
        sc_find_loops(&c, x, touch, ntouch);
    for (int p = 0; p < c.npair; p++)
    {
      int r = c.pair[p].res = c.ni++;
      memset(&c.it[r], 0, sizeof c.it[r]);
      c.kind[r] = SC_K_RES;
      c.cls[r] = c.cls[c.pair[p].vit];
      c.end[r] = c.end[c.pair[p].vit];
    }
    /* the ranges of the loops with a variable bound: one entry per class,
       first element, bound and test; a bound that may change has its own */
    for (int p = 0; p < c.npair; p++)
    {
      ScPair *pr = &c.pair[p];
      int q;
      if (pr->exact != 2)
        continue;
      for (q = 0; q < p; q++)
      {
        ScPair *o = &c.pair[q];
        if (o->exact == 2 && pr->broot && o->broot == pr->broot && c.cls[o->vit] == c.cls[pr->vit] &&
            o->lo == pr->lo && o->bcond == pr->bcond)
          break;
      }
      if (q < p)
        pr->rng = c.pair[q].rng;
      else
      {
        int r = pr->rng = c.ni++;
        memset(&c.it[r], 0, sizeof c.it[r]);
        c.kind[r] = SC_K_RES;
        c.cls[r] = c.cls[pr->vit];
        c.end[r] = c.end[pr->vit];
      }
    }
  }
  tcc_free(touch);
  c.move_line = tcc_mallocz((c.ni + 1) * sizeof(int));
  c.move_drop = tcc_mallocz(c.ni + 1);
  c.out_call = tcc_malloc((c.ni + 1) * sizeof(int32_t));
  memset(c.out_call, 0xff, (c.ni + 1) * sizeof(int32_t));
  c.tfa = tcc_malloc((c.ntemps + 1) * 7 * sizeof(int32_t));
  c.titem = c.tfa + c.ntemps + 1;
  c.tcid = c.titem + c.ntemps + 1;
  c.tix = c.tcid + c.ntemps + 1;
  c.tisc = c.tix + c.ntemps + 1;
  c.txat = c.tisc + c.ntemps + 1;
  c.tdelta = c.txat + c.ntemps + 1;
  c.tgen = tcc_mallocz((c.ntemps + 1) * sizeof(uint32_t));
  c.ret_owned = func && func->type.ref && (type_yasc(&func->type.ref->type) & YASC_OWNED);
  {
    int maxs = 1;
    for (int b = 0; b < nb; b++)
      if (c.cfg->blocks[b].num_succs + 1 > maxs)
        maxs = c.cfg->blocks[b].num_succs + 1;
    uint8_t *in = tcc_mallocz((size_t)nb * c.ni);
    uint8_t *out = tcc_malloc((size_t)maxs * c.ni + 1);
    uint8_t *reached = tcc_mallocz(nb);
    c.cur = tcc_malloc(c.ni + 1);
    for (int x = 0; x < c.ni; x++)
      in[x] = c.kind[x] == SC_K_RES ? 0 : SC_UNINIT | SC_N_SET;
    reached[0] = 1;
    for (int changed = 1; changed;)
    {
      changed = 0;
      for (int r = 0; r < c.cfg->rpo_count; r++)
      {
        int b = c.cfg->rpo_order[r];
        IRBasicBlock *bb = &c.cfg->blocks[b];
        if (!reached[b])
          continue;
        memcpy(c.cur, in + (size_t)b * c.ni, c.ni);
        sc_block(&c, b, out);
        for (int s = 0; s < bb->num_succs; s++)
        {
          int t = bb->succs[s];
          uint8_t *ti = in + (size_t)t * c.ni;
          for (int x = 0; x < c.ni; x++)
            if ((ti[x] | out[s * c.ni + x]) != ti[x] || !reached[t])
              ti[x] |= out[s * c.ni + x], changed = 1;
          reached[t] = 1;
        }
      }
    }
    c.report = 1;
    tcc_state->diag_noabort++;
    for (int b = 0; b < nb; b++)
      if (reached[b])
      {
        memcpy(c.cur, in + (size_t)b * c.ni, c.ni);
        sc_block(&c, b, out);
      }
    tcc_state->diag_noabort--;
    tcc_free(in);
    tcc_free(out);
    tcc_free(reached);
    tcc_free(c.cur);
  }
  tcc_ir_cfg_free(c.cfg);
  for (int p = 0; p < c.npair; p++)
    tcc_free(c.pair[p].blk);
  tcc_free(c.pair);
  tcc_free(c.dg);
  tcc_free(c.it);
  tcc_free(c.untracked);
  tcc_free(c.kind);
  tcc_free(c.cls);
  tcc_free(c.vivr);
  tcc_free(c.vdelta);
  tcc_free(c.multi);
  tcc_free(c.ldone);
  tcc_free(c.end);
  tcc_free(c.move_line);
  tcc_free(c.move_drop);
  tcc_free(c.out_call);
  tcc_free(c.tfa);
  tcc_free(c.tgen);
}

/* ------------------------------------------------------------------------ */

/* The checks of the raw IR of FUNC's body, before any pass. */
/* ------------------------------------------------------------------------ */
/* yasc tagged unions: `union(k) { long i when(V_INT); char * view s
 * when(V_STR); } u;` in a struct.  A member of u is used (read, written,
 * its address taken) only where k is known to select it: k compared with a
 * constant (if, switch, an early return) or a constant stored in it, on
 * every path to the use.  The state is, per tag (base, offset), the mask of
 * values it may hold (a bit per value the members name, YASC_TAG_OTHER for
 * any other); the edges of a compare narrow it, a store of a constant sets
 * it, a store of anything else, assigning the base again or handing it to
 * a call that may write through it forgets it (any call forgets a global's).
 * On the raw IR, before any pass: the same at every -O.  Bases: a pointer
 * variable or parameter, a local struct, a global struct; the tag tested
 * through the same base as the member is used.  Inlined bodies are their
 * callee's to report. */

void tcc_ir_note_tagged(TCCIRState *ir, int kind, int32_t vr, int32_t off, Sym *sym, int tu, int m, int line)
{
  ir->tag_notes = lt_grow(ir->tag_notes, ir->nb_tag_notes, sizeof(IRTagNote));
  IRTagNote *n = &ir->tag_notes[ir->nb_tag_notes++];
  n->at = ir->next_instruction_index;
  n->kind = kind, n->vr = vr, n->off = off, n->sym = sym;
  n->tu = tu, n->m = m, n->line = line;
}

typedef struct TgKey
{
  int kind;    /* 1 a pointer variable's value, 2 a frame object, 3 a symbol,
                  4 the value of a global pointer (`gp->k`: symbol + id) */
  int32_t id;  /* the variable's vreg, the frame object, the pointer's offset in its symbol */
  Sym *sym;
  int32_t off; /* from the variable's value / the frame / the symbol */
} TgKey;

/* temp position -> its only definition in the function, -1 none or several
   (a temp defined in another block: the switch's base pointer) */
static int32_t *tg_tdef;
static int tg_ntdef;

static int tg_def(TCCIRState *ir, int from, int at, int32_t vr)
{
  int j = ix_def(ir, from, at, vr);
  if (j >= 0)
    return j;
  int p = TCCIR_DECODE_VREG_POSITION(vr);
  return p < tg_ntdef ? tg_tdef[p] : -1;
}

static int tg_same_base(const TgKey *a, const TgKey *b)
{
  return a->kind == b->kind && a->id == b->id && a->sym == b->sym;
}

/* the address VR (a temp, before instruction AT) holds, plus ACC */
static int tg_vreg_addr(TCCIRState *ir, int from, int at, IROperand op, int32_t acc, TgKey *k)
{
  for (int d = 0; d < 8 && !irop_is_none(op); d++)
  {
    if (lt_is_addr(op))
    {
      int32_t fo = (int32_t)irop_get_imm64_ex(ir, op);
      k->kind = 2, k->sym = NULL, k->id = rs_frame_obj(ir, fo), k->off = fo + acc;
      return k->id >= 0;
    }
    if (irop_get_tag(op) == IROP_TAG_SYMREF)
    {
      IRPoolSymref *r = irop_get_symref_ex(ir, op);
      if (!r || !r->sym)
        return 0;
      if (op.is_lval) /* a pointer loaded from a global: its value is the base */
        k->kind = 4, k->id = r->addend, k->sym = r->sym, k->off = acc;
      else
        k->kind = 3, k->id = 0, k->sym = r->sym, k->off = r->addend + acc;
      return 1;
    }
    int32_t vr = irop_get_vreg(op);
    if (vr < 0 || op.is_lval)
      return 0;
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    {
      k->kind = 1, k->id = vr, k->sym = NULL, k->off = acc;
      return 1;
    }
    int j = tg_def(ir, from, at, vr);
    if (j < 0)
      return 0;
    IRQuadCompact *q = &ir->compact_instructions[j];
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (j < from)
      from = j; /* another block's: only its only definitions from here */
    IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    if (q->op == TCCIR_OP_ASSIGN || (q->op == TCCIR_OP_LOAD && irop_get_tag(s1) == IROP_TAG_SYMREF && s1.is_lval))
      op = s1;
    else if (q->op == TCCIR_OP_LEA && !irop_is_none(s1) && irop_get_tag(s1) == IROP_TAG_STACKOFF && irop_is_local(s1))
    {
      int32_t fo = (int32_t)irop_get_imm64_ex(ir, s1);
      k->kind = 2, k->sym = NULL, k->id = rs_frame_obj(ir, fo), k->off = fo + acc;
      return k->id >= 0;
    }
    else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && !irop_is_none(s2) && irop_is_immediate(s2) &&
             !s2.is_lval)
    {
      int64_t v = irop_get_imm64_ex(ir, s2);
      acc += q->op == TCCIR_OP_ADD ? (int32_t)v : -(int32_t)v;
      op = s1;
    }
    else if (q->op == TCCIR_OP_ADD && !irop_is_none(s1) && irop_is_immediate(s1) && !s1.is_lval)
    {
      acc += (int32_t)irop_get_imm64_ex(ir, s1);
      op = s2;
    }
    else
      return 0;
    at = j;
  }
  return 0;
}

/* the memory operand OP (at instruction I) is at K */
static int tg_mem(TCCIRState *ir, IRCFG *cfg, int i, IROperand op, TgKey *k)
{
  if (irop_is_none(op) || !op.is_lval)
    return 0;
  IROperand a = op;
  a.is_lval = 0;
  return tg_vreg_addr(ir, cfg->blocks[cfg->instr_to_block[i]].start_idx, i, a, 0, k);
}

/* an address operand OP (at instruction I) is K */
static int tg_addr(TCCIRState *ir, IRCFG *cfg, int i, IROperand op, TgKey *k)
{
  if (irop_is_none(op) || op.is_lval)
    return 0;
  return tg_vreg_addr(ir, cfg->blocks[cfg->instr_to_block[i]].start_idx, i, op, 0, k);
}

typedef struct TgCtx
{
  TCCIRState *ir;
  IRCFG *cfg;
  TgKey *keys;
  int16_t *ktu; /* the tagged union whose value table a key's mask uses */
  int nkeys;
  int32_t *tkey; /* temp position -> the key it was loaded from, -1 none, -2 several */
  int ntemp;
  /* locals a tag is copied into (`enum vkind k = v->k;`): state entry
     nkeys + j holds the key + 1 that local j mirrors now, 0 none -- until
     the local or the tag changes; a join keeps it where all paths agree */
  int32_t *vl;
  int nvl;
  /* owned members (YascTagged.own_*): state entry nkeys + nvl + key holds a
     bit per owned item of that key's union that may still own memory */
  int32_t *titem; /* temp position -> key * YASC_TAG_MAXOWN + item + 1 it was loaded from, 0 none, -1 several */
  int report;     /* the reporting walk */
} TgCtx;

#define TG_OWN(t, st, x) ((st)[(t)->nkeys + (t)->nvl + (x)])
/* ... and after them a bit per owned item released here (freed, moved out)
   and not given a new value since: through a pointer or a global, the
   caller still sees the tag select it when the function returns */
#define TG_REL(t, st, x) ((st)[(t)->nkeys + (t)->nvl + (t)->nkeys + (x)])

/* owned item IT (key * YASC_TAG_MAXOWN + item + 1) handed on: owns nothing now */
static void tg_released(TgCtx *t, uint32_t *st, int it)
{
  const int x = (it - 1) / YASC_TAG_MAXOWN;
  const uint32_t bit = 1u << ((it - 1) % YASC_TAG_MAXOWN);
  TG_OWN(t, st, x) &= ~bit;
  TG_REL(t, st, x) |= bit;
}

static int tg_find(const TgCtx *t, const TgKey *k)
{
  for (int i = 0; i < t->nkeys; i++)
    if (tg_same_base(&t->keys[i], k) && t->keys[i].off == k->off)
      return i;
  return -1;
}

/* the bit of value V in tagged union TU's masks */
static uint32_t tg_bit(int tu, int64_t v)
{
  const YascTagged *y = &yasc_tagged[tu];
  for (int i = 0; i < y->nvals; i++)
    if (y->vals[i] == v)
      return 1u << i;
  return YASC_TAG_OTHER;
}

static int tg_negate(int cond)
{
  switch (cond)
  {
  case TOK_EQ: return TOK_NE;
  case TOK_NE: return TOK_EQ;
  case TOK_ULT: return TOK_UGE;
  case TOK_UGE: return TOK_ULT;
  case TOK_ULE: return TOK_UGT;
  case TOK_UGT: return TOK_ULE;
  case TOK_LT: return TOK_GE;
  case TOK_GE: return TOK_LT;
  case TOK_LE: return TOK_GT;
  case TOK_GT: return TOK_LE;
  }
  return 0;
}

static int tg_holds(int cond, int64_t x, int64_t c)
{
  uint32_t ux = (uint32_t)x, uc = (uint32_t)c;
  int32_t sx = (int32_t)x, sc = (int32_t)c;
  switch (cond)
  {
  case TOK_EQ: return ux == uc;
  case TOK_NE: return ux != uc;
  case TOK_ULT: return ux < uc;
  case TOK_UGE: return ux >= uc;
  case TOK_ULE: return ux <= uc;
  case TOK_UGT: return ux > uc;
  case TOK_LT: return sx < sc;
  case TOK_GE: return sx >= sc;
  case TOK_LE: return sx <= sc;
  case TOK_GT: return sx > sc;
  }
  return 1;
}

/* the values a tag of TU may hold where `tag COND c` holds */
static uint32_t tg_where(int tu, int cond, int64_t c)
{
  const YascTagged *y = &yasc_tagged[tu];
  uint32_t m = 0;
  if (!cond)
    return ~0u;
  for (int i = 0; i < y->nvals; i++)
    if (tg_holds(cond, y->vals[i], c))
      m |= 1u << i;
  /* another value: unless the values that hold are few and all named */
  int64_t lo = 0, hi = -1;
  if (cond == TOK_EQ)
    lo = hi = (uint32_t)c;
  else if (cond == TOK_ULT)
    hi = (int64_t)(uint32_t)c - 1;
  else if (cond == TOK_ULE)
    hi = (uint32_t)c;
  else
    return m | YASC_TAG_OTHER;
  if (hi - lo > 64)
    return m | YASC_TAG_OTHER;
  for (int64_t x = lo; x <= hi; x++)
    if (tg_bit(tu, x) == YASC_TAG_OTHER)
      return m | YASC_TAG_OTHER;
  return m;
}

/* the key the value of OP (at I) is a tag loaded from, -1 */
static int tg_vl(const TgCtx *t, int32_t vr)
{
  for (int j = 0; j < t->nvl; j++)
    if (t->vl[j] == vr)
      return j;
  return -1;
}

static int tg_value_key(TgCtx *t, const uint32_t *st, int i, IROperand op)
{
  TgKey k;
  if (irop_is_none(op))
    return -1;
  int32_t vr = irop_get_vreg(op);
  /* a local the tag was copied into (its home may be a frame slot), or a
     temp copied from one (a switch's dispatch value) */
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_PARAM &&
      (!op.is_lval || irop_get_tag(op) == IROP_TAG_STACKOFF))
  {
    int j = tg_vl(t, vr);
    if (j >= 0)
      return st[t->nkeys + j] ? (int)st[t->nkeys + j] - 1 : -1;
  }
  if (op.is_lval)
    return tg_mem(t->ir, t->cfg, i, op, &k) ? tg_find(t, &k) : -1;
  if (vr < 0)
    return -1;
  if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
  {
    int j = tg_vl(t, vr);
    return j >= 0 && st[t->nkeys + j] ? (int)st[t->nkeys + j] - 1 : -1;
  }
  int p = TCCIR_DECODE_VREG_POSITION(vr);
  return p < t->ntemp && t->tkey[p] >= 0 ? t->tkey[p] : -1;
}

/* forget every key of BASE (any offset) */
/* key X may have changed: what it held is unknown, a local copied from it
   no longer tells about it */
static void tg_unknown(TgCtx *t, uint32_t *st, int x)
{
  st[x] = ~0u;
  TG_OWN(t, st, x) = (1u << yasc_tagged[t->ktu[x]].nown) - 1;
  TG_REL(t, st, x) = 0; /* what the callee or the other pointer left is theirs */
  for (int j = 0; j < t->nvl; j++)
    if (st[t->nkeys + j] == (uint32_t)x + 1)
      st[t->nkeys + j] = 0;
}

static void tg_forget_base(TgCtx *t, uint32_t *st, const TgKey *base)
{
  for (int i = 0; i < t->nkeys; i++)
    if (tg_same_base(&t->keys[i], base))
      tg_unknown(t, st, i);
}

/* the owned items of key X that may still own memory and may be live */
static uint32_t tg_live_own(TgCtx *t, const uint32_t *st, int x)
{
  const YascTagged *y = &yasc_tagged[t->ktu[x]];
  uint32_t live = 0;
  for (int k = 0; k < y->nown; k++)
    if ((TG_OWN(t, st, x) >> k & 1) && (y->own_mask[k] & st[x]))
      live |= 1u << k;
  return live;
}

/* memory key K is owned item *ITEM of tag key (returned), -1 none */
static int tg_item_at(TgCtx *t, const TgKey *k, int *item)
{
  for (int x = 0; x < t->nkeys; x++)
  {
    const YascTagged *y = &yasc_tagged[t->ktu[x]];
    if (!y->nown || !tg_same_base(&t->keys[x], k))
      continue;
    for (int j = 0; j < y->nown; j++)
      if (t->keys[x].off - y->tag_rel + y->own_off[j] == k->off)
        return *item = j, x;
  }
  return -1;
}

/* the owned item (key * YASC_TAG_MAXOWN + item + 1) the value of OP at I was read from, 0 */
static int tg_value_item(TgCtx *t, int i, IROperand op)
{
  TgKey k;
  int item, x;
  if (irop_is_none(op))
    return 0;
  if (op.is_lval)
    return tg_mem(t->ir, t->cfg, i, op, &k) && (x = tg_item_at(t, &k, &item)) >= 0 ? x * YASC_TAG_MAXOWN + item + 1 : 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || !t->titem)
    return 0;
  int p = TCCIR_DECODE_VREG_POSITION(vr);
  return p < t->ntemp && t->titem[p] > 0 ? t->titem[p] : 0;
}

static void tg_own_said(TgCtx *t, uint32_t live, int x, int line, const char *fmt)
{
  const YascTagged *y = &yasc_tagged[t->ktu[x]];
  if (!t->report)
    return;
  for (int k = 0; k < y->nown; k++)
    if (live >> k & 1)
      tcc_warning_c(warn_lifetime)(fmt, line, y->own_name[k], get_tok_str(y->tag_tok, NULL));
}

/* a way out of the function (a block with no successor) after LINE: a
   member released here that the tag still selects dangles for the caller */
static void tg_exit(TgCtx *t, const uint32_t *st, int line)
{
  for (int x = 0; x < t->nkeys; x++)
  {
    const YascTagged *y = &yasc_tagged[t->ktu[x]];
    uint32_t dangling = 0;
    if (t->keys[x].kind == 2)
      continue;
    for (int k = 0; k < y->nown; k++)
      if ((TG_REL(t, st, x) >> k & 1) && (y->own_mask[k] & st[x]))
        dangling |= 1u << k;
    tg_own_said(t, dangling, x, line,
                "%i:'%s' is freed or moved out, and '%s' still selects it when the function returns: change the "
                "variant or give it a new value");
  }
}

static void tg_instr(TgCtx *t, uint32_t *st, int i)
{
  TCCIRState *ir = t->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  const IRRegistersConfig *rc = &irop_config[q->op];
  IROperand d = rc->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
  IROperand s1 = rc->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
  TgKey k;
  if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
  {
    int call_id = irop_config[q->op].has_src2 ? TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q))) : -1;
    const IRLtCall *call = call_id >= 0 && call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
    const int gkeep = lt_call_gkeep(call);
    for (int j = 0; j < t->nkeys; j++)
      if (t->keys[j].kind >= 3 && tcc_ir_call_writes_global(gkeep, t->keys[j].sym))
        tg_unknown(t, st, j);
    return;
  }
  if (q->op == TCCIR_OP_FUNCPARAMVAL)
  {
    int64_t enc = irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
    int call_id = TCCIR_DECODE_CALL_ID(enc), pk = TCCIR_DECODE_PARAM_IDX(enc);
    IRLtCall *call = call_id < ir->lt_nb_calls ? &ir->lt_calls[call_id] : NULL;
    if (call)
      pk -= call->implicit;
    lt_arg_galias(ir, t->cfg, i, call, pk, s1);
    int keep = call && call->noted && pk >= 0 && pk < call->nparams && ((call->keep >> pk) & 1);
    /* free(v->u.s), take(move(v->u.s)): the owned item is released */
    int it = call && call->noted && pk >= 0 && pk < 32 && ((call->owned >> pk) & 1) ? tg_value_item(t, i, s1) : 0;
    if (it)
      tg_released(t, st, it);
    if (!keep && tg_addr(ir, t->cfg, i, s1, &k))
      tg_forget_base(t, st, &k);
    return;
  }
  /* a local struct's owned member left at return: the slot check says it
     (tcc_ir_slot_check follows the slot's value, whatever the tag).
     Through a pointer or a global, a member released here while the tag
     still selects it leaves the caller a dangling pointer */
  if (irop_is_none(d))
    return;
  if (irop_dest_defines_vreg(d))
  {
    int32_t vr = irop_get_vreg(d);
    /* `char * owned p = move(v->u.s);`: the local owns it now */
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD))
    {
      int it = tg_value_item(t, i, s1);
      if (it)
        tg_released(t, st, it);
    }
    int jt = vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP ? tg_vl(t, vr) : -1;
    if (jt >= 0)
    {
      int x = q->op == TCCIR_OP_ASSIGN ? tg_value_key(t, st, i, s1) : -1;
      st[t->nkeys + jt] = x >= 0 ? (uint32_t)x + 1 : 0;
    }
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    {
      k.kind = 1, k.id = vr, k.sym = NULL;
      tg_forget_base(t, st, &k);
      /* a local the tag is copied into, or anything else */
      int j = tg_vl(t, vr);
      if (j >= 0)
      {
        int x = (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) ? tg_value_key(t, st, i, s1) : -1;
        st[t->nkeys + j] = x >= 0 ? (uint32_t)x + 1 : 0;
      }
    }
    return;
  }
  /* a store */
  if (!tg_mem(ir, t->cfg, i, d, &k))
    return;
  int j = tg_find(t, &k), item;
  const int null_store = !irop_is_none(s1) && irop_is_immediate(s1) && !s1.is_lval && irop_get_imm64_ex(ir, s1) == 0;
  if (j >= 0)
  {
    /* the variant changes: what an owned member held is lost */
    tg_own_said(t, tg_live_own(t, st, j), j, q->line_num,
                "%i:'%s' may still own memory when '%s' changes: free or move it first");
    tg_unknown(t, st, j);
    TG_OWN(t, st, j) = 0;
    TG_REL(t, st, j) = 0;
    if (!irop_is_none(s1) && irop_is_immediate(s1) && !s1.is_lval)
      st[j] = tg_bit(t->ktu[j], irop_get_imm64_ex(ir, s1));
  }
  else if ((j = tg_item_at(t, &k, &item)) >= 0)
  {
    /* an owned member (or what shares its place) written over */
    tg_own_said(t, tg_live_own(t, st, j) & (1u << item), j, q->line_num,
                "%i:'%s' may still own memory: free or move it before it is written over");
    if (null_store || !(yasc_tagged[t->ktu[j]].own_mask[item] & st[j]))
      TG_OWN(t, st, j) &= ~(1u << item);
    else
      TG_OWN(t, st, j) |= 1u << item;
    TG_REL(t, st, j) &= ~(1u << item); /* a new value (null too: nothing to free twice) */
    return;
  }
  /* a global pointer stored to: what was known through it */
  if (k.kind == 3)
    for (int x = 0; x < t->nkeys; x++)
      if (t->keys[x].kind == 4 && t->keys[x].sym == k.sym && t->keys[x].id == k.off)
        tg_unknown(t, st, x);
  /* the same member through another base: two pointers may reach one struct */
  for (int x = 0; x < t->nkeys; x++)
    if (x != j && t->keys[x].off == k.off && !tg_same_base(&t->keys[x], &k))
      tg_unknown(t, st, x);
}

static char *tg_values(char *buf, int size, int tu, uint32_t mask)
{
  const YascTagged *y = &yasc_tagged[tu];
  int n = 0;
  buf[0] = 0;
  for (int i = 0; i < y->nvals; i++)
    if (mask & (1u << i))
    {
      int l = strlen(buf);
      if (y->vnames[i])
        snprintf(buf + l, size - l, "%s%s", n++ ? " or " : "", get_tok_str(y->vnames[i], NULL));
      else
        snprintf(buf + l, size - l, "%s%lld", n++ ? " or " : "", (long long)y->vals[i]);
    }
  return buf;
}

void tcc_ir_tag_check(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  IRCFG *cfg;
  if (!ir->nb_tag_notes)
    return;
  if (!n || !(cfg = tcc_ir_cfg_build(ir)))
  {
    tcc_free(ir->tag_notes), ir->tag_notes = NULL, ir->nb_tag_notes = 0;
    tcc_ir_w1c_free(ir);
    return;
  }
  TgCtx t;
  memset(&t, 0, sizeof t);
  t.ir = ir, t.cfg = cfg;
  int nn = ir->nb_tag_notes, nb = cfg->num_blocks;
  int32_t *nkey = tcc_malloc(nn * sizeof *nkey);
  t.keys = tcc_malloc(nn * sizeof *t.keys);
  t.ktu = tcc_malloc(nn * sizeof *t.ktu);
  tcc_state->diag_noabort++;
  /* each temp's only definition */
  tg_ntdef = 0;
  tg_tdef = NULL;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_is_none(d) || !irop_dest_defines_vreg(d))
      continue;
    int32_t vr = irop_get_vreg(d);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(vr);
    if (p >= tg_ntdef)
    {
      int nt = p + 64;
      tg_tdef = tcc_realloc(tg_tdef, nt * sizeof *tg_tdef);
      for (int x = tg_ntdef; x < nt; x++)
        tg_tdef[x] = -2;
      tg_ntdef = nt;
    }
    tg_tdef[p] = tg_tdef[p] == -2 ? i : -1;
  }
  for (int x = 0; x < tg_ntdef; x++)
    if (tg_tdef[x] == -2)
      tg_tdef[x] = -1;
  /* the tag each access needs, as a key */
  for (int a = 0; a < nn; a++)
  {
    IRTagNote *no = &ir->tag_notes[a];
    TgKey k;
    int ok = 0;
    nkey[a] = -1;
    if (no->at >= n)
      continue;
    if (no->kind == 1)
    {
      k.kind = 2, k.sym = NULL, k.id = rs_frame_obj(ir, no->off), k.off = no->off;
      ok = k.id >= 0;
    }
    else if (no->kind == 2)
      k.kind = 3, k.id = 0, k.sym = no->sym, k.off = no->off, ok = 1;
    else
    {
      int from = cfg->blocks[cfg->instr_to_block[no->at > 0 ? no->at - 1 : 0]].start_idx;
      int j = ix_def(ir, from, no->at, no->vr);
      if (j >= 0)
      {
        IRQuadCompact *q = &ir->compact_instructions[j];
        IROperand dd = tcc_ir_op_get_dest(ir, q);
        ok = tg_vreg_addr(ir, from, j + 1, dd, 0, &k);
      }
    }
    if (!ok)
      continue;
    if (no->m >= 0)
      k.off += yasc_tagged[no->tu].tag_rel; /* a member's: the tag is beside it */
    int x = tg_find(&t, &k);
    if (x < 0)
      x = t.nkeys++, t.keys[x] = k, t.ktu[x] = no->tu;
    nkey[a] = x;
  }
  /* temps holding a tag's value */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_is_none(d) || !irop_dest_defines_vreg(d))
      continue;
    int32_t vr = irop_get_vreg(d);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(vr);
    if (p >= t.ntemp)
    {
      int nt = p + 64;
      t.tkey = tcc_realloc(t.tkey, nt * sizeof *t.tkey);
      for (int x = t.ntemp; x < nt; x++)
        t.tkey[x] = -1;
      t.ntemp = nt;
    }
    TgKey k;
    int x = -2;
    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && tg_mem(ir, cfg, i, tcc_ir_op_get_src1(ir, q), &k) &&
        (x = tg_find(&t, &k)) < 0)
      x = -2;
    t.tkey[p] = t.tkey[p] == -1 || t.tkey[p] == x ? x : -2;
  }
  /* locals a tag is copied into */
  for (int i = 0; i < n && t.nkeys; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if ((q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD) || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_is_none(d) || !irop_dest_defines_vreg(d))
      continue;
    int32_t vr = irop_get_vreg(d);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR || tg_vl(&t, vr) >= 0)
      continue;
    uint32_t none[1] = {0};
    if (tg_value_key(&t, none, i, tcc_ir_op_get_src1(ir, q)) < 0)
      continue;
    t.vl = tcc_realloc(t.vl, (t.nvl + 1) * sizeof *t.vl);
    t.vl[t.nvl++] = vr;
  }
  /* ... and temps copied from such a local (`switch (k)` copies k) */
  for (int i = 0, nvars_vl = t.nvl; i < n && nvars_vl; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q), s1 = tcc_ir_op_get_src1(ir, q);
    if (irop_is_none(d) || !irop_dest_defines_vreg(d) || irop_is_none(s1))
      continue;
    int32_t vr = irop_get_vreg(d), sv = irop_get_vreg(s1);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || sv < 0 || tg_vl(&t, sv) < 0 ||
        tg_vl(&t, vr) >= 0)
      continue;
    t.vl = tcc_realloc(t.vl, (t.nvl + 1) * sizeof *t.vl);
    t.vl[t.nvl++] = vr;
  }
  /* temps holding an owned item's value */
  if (t.ntemp)
  {
    t.titem = tcc_mallocz(t.ntemp * sizeof *t.titem);
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if ((q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD) || !irop_config[q->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (irop_is_none(d) || !irop_dest_defines_vreg(d))
        continue;
      int32_t vr = irop_get_vreg(d);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int p = TCCIR_DECODE_VREG_POSITION(vr), it = tg_value_item(&t, i, tcc_ir_op_get_src1(ir, q));
      if (p < t.ntemp && it)
        t.titem[p] = t.titem[p] == 0 || t.titem[p] == it ? it : -1;
    }
  }
  /* the masks per block: entry knows nothing, a join is the union; the
     locals' links after them, kept where the paths agree; then the owned
     items, joined as the masks */
  const int own0 = t.nkeys + t.nvl, rel0 = own0 + t.nkeys;
  int nk = rel0 + t.nkeys > 0 ? rel0 + t.nkeys : 1;
  uint32_t *in = tcc_mallocz((size_t)nb * nk * sizeof *in), *cur = tcc_malloc(nk * sizeof *cur);
  uint32_t *edge = tcc_malloc(nk * sizeof *edge);
  uint8_t *seen = tcc_mallocz(nb), *queued = tcc_mallocz(nb);
  int *work = tcc_malloc((nb + 1) * sizeof *work), nw = 0;
  if (nb)
  {
    for (int x = 0; x < t.nkeys; x++)
    {
      in[x] = ~0u;
      /* a local struct starts owning nothing; through a pointer or a global,
         an owned member may hold something */
      in[own0 + x] = t.keys[x].kind == 2 ? 0 : (1u << yasc_tagged[t.ktu[x]].nown) - 1;
    }
    seen[0] = queued[0] = 1, work[nw++] = 0;
  }
  while (nw)
  {
    int b = work[--nw];
    queued[b] = 0;
    IRBasicBlock *bb = &cfg->blocks[b];
    memcpy(cur, in + (size_t)b * nk, nk * sizeof *cur);
    int last = bb->end_idx - 1, cond = 0, key = -1, target = -1;
    int64_t c = 0;
    for (int i = bb->start_idx; i < bb->end_idx; i++)
      tg_instr(&t, cur, i);
    /* `if (v->k == V_STR)`, a switch on v->k: the compare before the jump */
    if (last > bb->start_idx && ir->compact_instructions[last].op == TCCIR_OP_JUMPIF)
    {
      IRQuadCompact *lq = &ir->compact_instructions[last], *tq = &ir->compact_instructions[last - 1];
      if (tq->op == TCCIR_OP_CMP || tq->op == TCCIR_OP_TEST_ZERO)
      {
        IROperand b2 = tq->op == TCCIR_OP_CMP ? tcc_ir_op_get_src2(ir, tq) : irop_make_none();
        if (tq->op == TCCIR_OP_TEST_ZERO || (!irop_is_none(b2) && irop_is_immediate(b2) && !b2.is_lval))
        {
          key = tg_value_key(&t, cur, last - 1, tcc_ir_op_get_src1(ir, tq));
          c = tq->op == TCCIR_OP_CMP ? irop_get_imm64_ex(ir, b2) : 0;
          cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, lq));
          target = (int)tcc_ir_op_dest_imm(ir, lq);
        }
      }
    }
    int ntaken = 0;
    for (int s = 0; s < bb->num_succs; s++)
      ntaken += cfg->blocks[bb->succs[s]].start_idx == target;
    for (int s = 0; s < bb->num_succs; s++)
    {
      int sb = bb->succs[s];
      memcpy(edge, cur, nk * sizeof *edge);
      /* the jump's target may be no block (the function's end): then the
         block that remains is the edge where it was not taken */
      if (key >= 0 && (ntaken == 1 || (ntaken == 0 && target >= 0)))
      {
        edge[key] &= tg_where(t.ktu[key], cfg->blocks[sb].start_idx == target ? cond : tg_negate(cond), c);
        /* an owned member of a variant the tag cannot be holds nothing */
        const YascTagged *y = &yasc_tagged[t.ktu[key]];
        for (int k = 0; k < y->nown; k++)
          if (!(y->own_mask[k] & edge[key]))
            edge[own0 + key] &= ~(1u << k), edge[rel0 + key] &= ~(1u << k);
      }
      uint32_t *si = in + (size_t)sb * nk;
      int changed = !seen[sb];
      for (int x = 0; x < t.nkeys; x++)
        if ((si[x] | edge[x]) != si[x])
          si[x] |= edge[x], changed = 1;
      for (int x = t.nkeys; x < t.nkeys + t.nvl; x++)
        if (!seen[sb])
          si[x] = edge[x];
        else if (si[x] && si[x] != edge[x])
          si[x] = 0, changed = 1;
      for (int x = own0; x < rel0 + t.nkeys; x++) /* owned and released bits: the union */
        if ((si[x] | edge[x]) != si[x])
          si[x] |= edge[x], changed = 1;
      seen[sb] = 1;
      if (changed && !queued[sb])
        queued[sb] = 1, work[nw++] = sb;
    }
  }
  /* the reporting walk of the owned members: each block once, from its fixed point */
  t.report = 1;
  for (int b = 0; b < nb; b++)
  {
    if (!seen[b])
      continue;
    memcpy(cur, in + (size_t)b * nk, nk * sizeof *cur);
    int last_line = 0;
    for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++)
    {
      t.report = !rs_in_inline(ir, i); /* an inlined body's are its callee's to say */
      tg_instr(&t, cur, i);
      if (ir->compact_instructions[i].op != TCCIR_OP_JUMP && ir->compact_instructions[i].op != TCCIR_OP_NOP)
        last_line = ir->compact_instructions[i].line_num;
    }
    if (!cfg->blocks[b].num_succs && cfg->blocks[b].end_idx > cfg->blocks[b].start_idx &&
        !rs_in_inline(ir, cfg->blocks[b].end_idx - 1))
    {
      t.report = 1;
      tg_exit(&t, cur, last_line);
    }
  }
  t.report = 0;
  /* each access, against the state before it */
  for (int a = 0; a < nn; a++)
  {
    IRTagNote *no = &ir->tag_notes[a];
    const YascTagged *y = &yasc_tagged[no->tu];
    char un[64], vals[256];
    if (no->m < 0 || no->at >= n || rs_in_inline(ir, no->at))
      continue; /* a tag's access: only a key */
    int b = cfg->instr_to_block[no->at];
    if (!seen[b])
      continue;
    snprintf(un, sizeof un, "%s.%s", get_tok_str(y->name_tok, NULL), get_tok_str(y->mtok[no->m], NULL));
    if (nkey[a] < 0)
    {
      tcc_warning_c(warn_lifetime)("%i:'%s' of a tagged union is used where its tag is not followed: test '%s' "
                                   "through the same pointer or variable first",
                                   no->line, un, get_tok_str(y->tag_tok, NULL));
      continue;
    }
    memcpy(cur, in + (size_t)b * nk, nk * sizeof *cur);
    for (int i = cfg->blocks[b].start_idx; i < no->at; i++)
      tg_instr(&t, cur, i);
    uint32_t m = cur[nkey[a]], allowed = y->mmask[no->m];
    if (m & ~allowed)
      tcc_warning_c(warn_lifetime)("%i:'%s' is used where '%s' may not be %s: test it first", no->line, un,
                                   get_tok_str(y->tag_tok, NULL), tg_values(vals, sizeof vals, no->tu, allowed));
  }
  tcc_state->diag_noabort--;
  tcc_free(in), tcc_free(cur), tcc_free(edge), tcc_free(seen), tcc_free(queued), tcc_free(work);
  tcc_free(nkey), tcc_free(t.keys), tcc_free(t.ktu), tcc_free(t.tkey), tcc_free(t.vl), tcc_free(t.titem);
  tcc_free(tg_tdef), tg_tdef = NULL, tg_ntdef = 0;
  tcc_free(ir->tag_notes), ir->tag_notes = NULL, ir->nb_tag_notes = 0;
  tcc_ir_cfg_free(cfg);
}

void tcc_ir_flow_checks(TCCIRState *ir, Sym *func)
{
  /* once per function: a body compiled again (late re-optimisation at the
     end of the unit) said everything the first time */
  if (func && func->type.ref && func->type.ref->f.func_compiled)
  {
    tcc_ir_lt_free(ir);
    tcc_ir_taint_free(ir);
    tcc_ir_capi_free(ir);
    return;
  }
  /* the checks hold their scratch until they return: -fmax-errors stops
     the compile after them */
  tcc_max_errors_defer(tcc_state);
  if (ir->ab_on || ir->ab_uaf)
    tcc_ir_ab_check(ir);
  if (ir->taint_on)
    tcc_ir_taint_check(ir, func);
  if (ir->capi_on)
    tcc_ir_capi_check(ir, func);
  tcc_ir_w1c_check(ir); /* yasc register fields (regaccess.c); frees its records */
  LtCalls calls = {NULL, 0};
  if (ir->lt_on && ir->lt_nb_calls)
  {
    calls.v = tcc_malloc(ir->lt_nb_calls * sizeof(IRLtCall));
    memcpy(calls.v, ir->lt_calls, ir->lt_nb_calls * sizeof(IRLtCall));
    calls.n = ir->lt_nb_calls;
  }
  if (ir->lt_on)
  {
    /* before the lifetime check, which frees the variables' records */
    tcc_ir_view_escape_check(ir, func);
    tcc_ir_dealloc_check(ir);
    tcc_ir_restrict_check(ir);
    tcc_ir_tag_check(ir);
    tcc_ir_slot_check(ir, func);
    if (ir->un_nb_vars)
      un_note_outparams(ir);
    tcc_ir_lifetime_check(ir, func);
  }
  if (ir->un_nb_vars)
    un_check(ir, &calls);
  if ((tcc_state->warn_return_local_addr & WARN_ON) && !tcc_state->warn_none && func && func->type.ref &&
      (func->type.ref->type.t & VT_BTYPE) == VT_PTR)
    lt_return_local_check(ir, &calls);
  if (tcc_ir_dp_wanted())
    lt_dangling_check(ir, func, &calls);
  tcc_free(calls.v);
  tcc_max_errors_stop(tcc_state);
}

/* Is any of them on?  Then a body nothing generates still wants a parse. */
int tcc_ir_flow_checks_wanted(void)
{
  TCCState *s1 = tcc_state;
  if (s1->warn_none)
    return 0;
  return ((s1->warn_lifetime & WARN_ON) && s1->nb_yasc_files) || tcc_ir_ab_active() ||
         ((s1->warn_uninitialized | s1->warn_maybe_uninitialized | s1->warn_return_local_addr |
           s1->warn_dangling_pointer | s1->warn_tainted_format) &
          WARN_ON);
}

/* ------------------------------------------------------------------------ */
/* -Wnull-dereference: memory read or written at a constant address below
 * 4096 -- null, or a member through null -- after optimisation, which turns a
 * pointer it proved null into such an address.  The second kind of check:
 * on the optimised IR, so -O2 may find what -O0 does not. */

/* vreg -> the address it holds in the block being walked, when that is a
   null pointer plus an offset (0, &((struct S *)0)->m, ...) */
typedef struct NdMap
{
  int64_t *val;
  uint32_t *gen;
  int n[4]; /* by vreg type: VAR, TEMP, PARAM sizes */
  int base[4];
  uint32_t cur;
} NdMap;

static int nd_slot(NdMap *m, int32_t vreg)
{
  int t = TCCIR_DECODE_VREG_TYPE(vreg), pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (t < 1 || t > 3 || pos >= m->n[t])
    return -1;
  return m->base[t] + pos;
}

/* the null-based address an operand's value is: the constant 0, or a
   vreg the walk knows to hold null plus an offset */
static int nd_null(TCCIRState *ir, NdMap *m, IROperand op, int64_t *out)
{
  int32_t vreg;
  int slot;
  if (irop_is_none(op))
    return 0;
  if (irop_is_immediate(op) && !op.is_lval && irop_get_vreg(op) < 0)
  {
    *out = 0;
    return irop_get_imm64_ex(ir, op) == 0;
  }
  if ((vreg = irop_get_vreg(op)) < 0 || !irop_is_vreg_value(op) || (slot = nd_slot(m, vreg)) < 0 ||
      m->gen[slot] != m->cur)
    return 0;
  *out = m->val[slot];
  return 1;
}

void tcc_ir_null_deref_check(TCCIRState *ir)
{
  int total, nlines = 0;
  int lines[16]; /* reported already: once per line (the last 16) */
  NdMap m;
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg)
    return;
  memset(&m, 0, sizeof m);
  m.n[TCCIR_VREG_TYPE_VAR] = ir->next_local_variable;
  m.n[TCCIR_VREG_TYPE_TEMP] = ir->next_temporary_variable;
  m.n[TCCIR_VREG_TYPE_PARAM] = ir->next_parameter;
  m.base[TCCIR_VREG_TYPE_TEMP] = m.n[TCCIR_VREG_TYPE_VAR];
  m.base[TCCIR_VREG_TYPE_PARAM] = m.base[TCCIR_VREG_TYPE_TEMP] + m.n[TCCIR_VREG_TYPE_TEMP];
  total = m.base[TCCIR_VREG_TYPE_PARAM] + m.n[TCCIR_VREG_TYPE_PARAM] + 1;
  m.val = tcc_malloc(total * sizeof(int64_t));
  m.gen = tcc_mallocz(total * sizeof(uint32_t));
  /* a variable whose address is taken can change through it (a call, a
     store): never followed */
  uint8_t *taken = tcc_mallocz(total);
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *rc = &irop_config[q->op];
    IROperand ops[3];
    int nops = 0, slot;
    if (rc->has_dest)
      ops[nops++] = tcc_ir_op_get_dest(ir, q);
    if (rc->has_src1)
      ops[nops++] = tcc_ir_op_get_src1(ir, q);
    if (rc->has_src2)
      ops[nops++] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < nops; k++)
    {
      int32_t vreg = irop_get_vreg(ops[k]);
      int is_src1 = rc->has_src1 && k == rc->has_dest;
      if (!irop_is_none(ops[k]) && vreg >= 0 && (lt_is_addr(ops[k]) || (is_src1 && lt_lea_of_local(q, ops[k]))) &&
          (slot = nd_slot(&m, vreg)) >= 0)
        taken[slot] = 1;
    }
  }
  for (int v = 0; v < total - 1; v++)
    if (v < m.base[TCCIR_VREG_TYPE_TEMP] && lt_vreg_addrtaken(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, v)))
      taken[v] = 1;
  for (int b = 0; b < cfg->num_blocks; b++)
  {
    IRBasicBlock *bb = &cfg->blocks[b];
    m.cur++;
    for (int i = bb->start_idx; i < bb->end_idx; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      const IRRegistersConfig *rc = &irop_config[q->op];
      IROperand ops[3];
      int nops = 0, def = -1;
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (rc->has_dest)
        ops[nops++] = tcc_ir_op_get_dest(ir, q);
      if (rc->has_src1)
        ops[nops++] = tcc_ir_op_get_src1(ir, q);
      if (rc->has_src2)
        ops[nops++] = tcc_ir_op_get_src2(ir, q);
      for (int k = 0; k < nops; k++)
      {
        IROperand op = ops[k];
        int64_t addr;
        int32_t vreg;
        int slot, known = 0;
        if (irop_is_none(op) || !op.is_lval)
          continue;
        if (k == 0 && rc->has_dest && irop_dest_defines_vreg(op))
          continue;
        vreg = irop_get_vreg(op);
        /* a literal address: only 0 (any other is the MMIO idiom, *(T *)0x40) */
        if (vreg < 0 && (irop_get_tag(op) == IROP_TAG_IMM32 || irop_get_tag(op) == IROP_TAG_I64))
          addr = irop_get_imm64_ex(ir, op), known = addr == 0;
        else if (vreg >= 0 && irop_get_tag(op) == IROP_TAG_VREG && (slot = nd_slot(&m, vreg)) >= 0 &&
                 m.gen[slot] == m.cur)
          addr = m.val[slot], known = 1;
        int seen = 0;
        if (!known || addr < 0 || addr >= 4096)
          continue;
        for (int l = 0; l < nlines && l < 16; l++)
          seen |= lines[l] == (int)q->line_num;
        if (seen)
          continue;
        lines[nlines++ % 16] = q->line_num;
        tcc_warning_c(warn_null_dereference)("%i:a null pointer is dereferenced", q->line_num);
        break;
      }
      /* what the destination holds now */
      if (rc->has_dest && irop_dest_defines_vreg(ops[0]) && (def = nd_slot(&m, irop_get_vreg(ops[0]))) >= 0)
      {
        int64_t v, off;
        IROperand other = nops > 2 ? ops[2] : irop_make_none();
        if (q->op == TCCIR_OP_ASSIGN && rc->has_src1 && !taken[def] && nd_null(ir, &m, ops[1], &v))
        {
          m.val[def] = v;
          m.gen[def] = m.cur;
        }
        /* null + a constant offset: a member through null */
        else if (q->op == TCCIR_OP_ADD && rc->has_src1 && rc->has_src2 && !taken[def] && nd_null(ir, &m, ops[1], &v) &&
                 irop_is_immediate(other) && !other.is_lval && (off = irop_get_imm64_ex(ir, other)) >= 0 &&
                 off < 4096)
        {
          m.val[def] = v + off;
          m.gen[def] = m.cur;
        }
        else
          m.gen[def] = 0;
      }
    }
  }
  tcc_free(m.val);
  tcc_free(m.gen);
  tcc_free(taken);
  tcc_ir_cfg_free(cfg);
}
