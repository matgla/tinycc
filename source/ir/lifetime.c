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
  return tcc_state->yasc && tcc_state->ir && (tcc_state->warn_lifetime & WARN_ON) && !tcc_state->warn_none;
}

/* Called when a function's IR starts: in yasc with -Wlifetime its pointer
   variables are recorded and the body checked. */
void tcc_ir_lt_begin(TCCIRState *ir)
{
  ir->lt_on = tcc_ir_lt_active();
  tcc_ir_taint_begin(ir);
  tcc_ir_ab_begin(ir);
}

/* grow a record array by doubling when its count reaches a power of two */
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
  v->arg_report = 0;
  v->arg_index = -1;
  v->arg_callee = 0;
  v->dealloc = type_dealloc(&sym->type);
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

/* An inlined callee's parameter: what the body does with it is the callee's
   to report; the argument stored into it is the caller's, as for a call. */
void tcc_ir_lt_inline_param(TCCIRState *ir, int32_t vreg, Sym *callee, int index)
{
  int v = vreg >= 0 ? lt_var_by_vreg(ir, vreg) : -1;
  if (v < 0)
    return;
  ir->lt_vars[v].silent = 1;
  ir->lt_vars[v].arg_index = index;
  ir->lt_vars[v].arg_callee = callee ? callee->v : 0;
  /* the call is itself in an inlined body: that body's function reports it */
  ir->lt_vars[v].arg_report = !tcc_state->in_inline_expansion;
}

/* An inlined call's result variable (-O1+), at the end of the body: a `T *
   maybe` result may be null where the caller uses it, whatever the body
   returned -- as for a call that is not inlined, so that the diagnostics do
   not depend on -O.  It is silent; its value dereferenced is reported as a
   value with no name. */
void tcc_ir_lt_inline_result(TCCIRState *ir, int32_t vreg, Sym *ftype)
{
  IRLtVar *v;
  int rq;
  if (!ir || !ir->lt_on || vreg < 0 || !ftype || (ftype->type.t & (VT_BTYPE | VT_ARRAY)) != VT_PTR)
    return;
  rq = type_yasc(&ftype->type);
  if ((rq & YASC_UNCHECKED) || (!(rq & YASC_MAYBE) && (rq & (YASC_OWNED | YASC_VIEW))))
    return;
  ir->lt_vars = lt_grow(ir->lt_vars, ir->lt_nb_vars, sizeof(IRLtVar));
  v = &ir->lt_vars[ir->lt_nb_vars++];
  memset(v, 0, sizeof *v);
  v->vreg = vreg;
  v->silent = 1;
  v->kind = LTV_MAYBE | LTV_RESULT;
  v->arg_index = -1;
  lt_add_event(ir, LT_EV_RESULT(ir->lt_nb_vars - 1), file ? file->line_num : 0);
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

/* the parameters of the function type `ftype` that take ownership */
void tcc_ir_lt_note_call(TCCIRState *ir, int call_id, Sym *ftype, Sym *callee)
{
  IRLtCall *c;
  uint32_t owned = 0, drop = 0, nonnull = 0, keep = 0, nonnull_attr = 0;
  int drop_dealloc = 0;
  uint32_t taken = 0, handles = 0, outown = 0, outown_out = 0, outown_maybe = 0, dropobj = 0, objout = 0;
  int dropobj_dealloc = 0;
  int i = 0;
  for (Sym *pa = sym_first(ftype); pa && i < 32; pa = pa->next, i++)
  {
    int q = type_yasc(&pa->type);
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
  c->outown = outown;
  c->outown_out = outown_out;
  c->outown_maybe = outown_maybe;
  c->dropobj = dropobj;
  c->objout = objout;
  c->dropobj_dealloc = dropobj_dealloc;
  c->borrows = (rq & YASC_VIEW) && !(rq & YASC_UNCHECKED) ? (uint32_t)type_borrows(&ftype->type) & YASC_BORROWS_ARGS : 0;
}

/* A builtin redirected call FROM to the helper call TO: the helper call
   stands for the declared one -- its result may be null as that one's, and
   its arguments may not be where that one's may not */
void tcc_ir_lt_copy_call(TCCIRState *ir, int from, int to)
{
  IRLtCall rec, *c;
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

void tcc_ir_lt_note_slot(TCCIRState *ir, int addr, CType *type, int tok, int obj_dealloc)
{
  int al, size = type_size(type, &al);
  if (size <= 0)
    return;
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
  ir->un_vars = NULL;
  ir->un_events = NULL;
  ir->un_nb_vars = ir->un_nb_events = 0;
}

void tcc_ir_lt_free(TCCIRState *ir)
{
  lt_free_records(ir);
  un_free_records(ir);
  tcc_ir_ab_free(ir);
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
                         -1 none, -2 several (not followed) */
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

/* what a drop does to v: an int handle is released, memory freed */
static const char *lt_freed(LtCtx *c, int v)
{
  return v < c->nvars && (c->ir->lt_vars[v].kind & LTV_HANDLE) ? "released" : "freed";
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

/* op (a temp) holds i * scale at instruction at: i, and *scale; -1 none */
static int32_t ix_scaled(TCCIRState *ir, int from, int at, IROperand op, int *scale)
{
  int32_t t, v;
  int j;
  IRQuadCompact *q;
  IROperand k;
  int64_t kv;
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
  if ((v = ix_var(ir, from, j, tcc_ir_op_get_src1(ir, q))) < 0 || ix_written(ir, j + 1, at, v))
    return -1;
  return v;
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
 *     Other calls keep the state: globals are not followed.
 *   - Stores through other pointers change nothing: they are assumed not to
 *     alias the member (as Cake does).
 *   - The base may also be an object (LT_OBJ): a local struct (`s.next`,
 *     its frame offset range from the frontend), a global struct (`g.next`,
 *     the symbol plus offset used in place) or a global pointer's value
 *     (`gp->next`, a load of the global).  A store into the object sets or
 *     forgets the paths it covers; storing to the global pointer forgets
 *     its paths.  Globals are not followed inside an inlined body (a call
 *     does not change them either), so the result does not depend on -O.
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
  pv->kind = LTV_MAYBE | (owned ? LTV_OWNED : 0);
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
  pv->kind = LTV_MAYBE | (owned ? LTV_OWNED : 0);
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
  /* a maybe member read in place */
  if ((v = lt_deref_path(c, op)) >= 0)
    return c->cur[v] & LT_NULLBITS;
  return LT_N_SET;
}

static const char *lt_null_what(int st)
{
  return st == LT_N_NULL ? "a null pointer" : "a pointer that may be null";
}

static int lt_handle(LtCtx *c, int v)
{
  return v < c->nvars && (c->ir->lt_vars[v].kind & LTV_HANDLE);
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

/* a read of v that does not move it */
static void lt_use(LtCtx *c, int v, int line)
{
  uint8_t st = c->cur[v] & LT_OWNBITS;
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
      if (call && call->known && (k -= call->implicit) >= 0 && k < 32 && !((call->outown >> k) & 1))
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
        (o = lt_pointer_var(c, src1)) >= 0 && o < c->nvars &&
        lt_owned(c, o))
      c->call_bsrc[call_id] = c->call_bsrc[call_id] == -1 || c->call_bsrc[call_id] == o ? o : -2;
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
    int keep = call && call->noted && k >= 0 && k < call->nparams && ((call->keep >> k) & 1);
    if (!keep && (t = lt_arg_target(c, src1, &self)) != -1)
    {
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
        lt_forget(c, t, self); /* the call is in another block */
    }
  }
  if ((q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID) && c->npend && cfg->has_src2)
  {
    int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
    for (int k = 0; k < c->npend; k++)
      if (c->pend[3 * k] == call_id)
      {
        lt_forget(c, c->pend[3 * k + 1], c->pend[3 * k + 2]);
        c->pend[3 * k] = -1;
      }
  }
  if (q->op == TCCIR_OP_FUNCPARAMVAL && (v = lt_read_value(c, src1)) >= 0 && lt_owned(c, v))
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
      c->cur[k] = (c->cur[k] & ~(LT_NULLBITS | LT_OWNBITS)) | LT_UNINIT | st;
    }
    else if (k >= 0 || (k = lt_store_hits_path(c, dest)) >= 0)
      lt_forget(c, k, 1);
    else if (direct && (db = lt_direct_base(c, dest, &doff)) != -1)
    {
      /* a struct's other member, or all of it: the paths it covers */
      int sz = irop_get_btype(dest) == IROP_BTYPE_STRUCT ? INT32_MAX
               : irop_get_btype(dest) == IROP_BTYPE_INT64 || irop_get_btype(dest) == IROP_BTYPE_FLOAT64 ? 8 : 4;
      for (int p = 0; p < c->npaths; p++)
        if (c->pbase[p] == db && c->pidx[p] < 0 && c->poff[p] >= doff && c->poff[p] - doff < sz)
          lt_forget(c, c->nvars + p, 1);
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
  if (ir->lt_vars[v].arg_index >= 0 && (ir->lt_vars[v].kind & LTV_MUT))
  {
    int self, t = lt_arg_target(c, src1, &self);
    lt_forget(c, t, self);
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
    int from = -1, bc = -1;
    if (is_copy && !(irop_get_tag(src1) == IROP_TAG_VREG && src1.is_lval))
      from = lt_pointer_var(c, src1);
    else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
      from = lt_pointer_var(c, src1) >= 0 ? lt_pointer_var(c, src1) : lt_pointer_var(c, src2);
    /* the result of a call that borrows from an owned argument (strchr(b, c)) */
    if (q->op == TCCIR_OP_FUNCCALLVAL)
      bc = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
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
    if (from >= 0 && (from == v || !lt_owned(c, from)))
      from = -1;
    if (c->collect && from >= 0)
      c->borrow[v] = c->borrow[v] == -1 || c->borrow[v] == from ? from : -2;
    c->cur[v] = (c->cur[v] & ~LT_OWNBITS) | (from >= 0 && c->borrow[v] == from ? LT_OWN : LT_UNINIT);
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
          ok = call && call->known && (k -= call->implicit) >= 0 && k < 32 && ((call->outown >> k) & 1);
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
        if (!ok)
          c.tout[TCCIR_DECODE_VREG_POSITION(sv)] = -1;
      }
    }
  for (int v = 0; v < nv; v++)
  {
    int32_t vreg = ir->lt_vars[v].vreg;
    int pos = TCCIR_DECODE_VREG_POSITION(vreg);
    if (lt_vreg_addrtaken(ir, vreg) && outok[v] != 1)
      ir->lt_vars[v].untracked = 1;
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
  if (ir->lt_nb_members + ir->lt_nb_gmembers)
  {
    c.pcap = ir->lt_nb_members + ir->lt_nb_gmembers;
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
  /* ... then the status locals, then the locals holding a null test */
  c.sbase = c.lbase + c.nlink;
  c.clbase = c.sbase + c.nstat;
  int nextra = c.nlink + c.nstat + c.nclink;
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
  tcc_free(c.take_call);
  tcc_free(c.taken_by);
  tcc_free(c.call_bsrc);
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
  return tcc_state->ir && ((tcc_state->warn_uninitialized | tcc_state->warn_maybe_uninitialized) & WARN_ON) &&
         !tcc_state->warn_none;
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

static void un_read(UnCtx *c, IROperand op, int line)
{
  int v;
  uint32_t bit = un_bit(c, op, &v);
  if (!bit || !(c->u & bit) || lt_is_addr(op))
    return;
  if (c->report && !c->ir->un_vars[v].silent)
  {
    if (c->s & bit)
      tcc_warning_c(warn_maybe_uninitialized)("%i:'%s' may be used uninitialized", line,
                                              get_tok_str(c->ir->un_vars[v].tok, NULL));
    else
      tcc_warning_c(warn_uninitialized)("%i:'%s' is used uninitialized", line, get_tok_str(c->ir->un_vars[v].tok, NULL));
  }
  /* once: from here on it counts as set */
  c->u &= ~bit;
  c->s |= bit;
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

void tcc_ir_uninit_check(TCCIRState *ir)
{
  UnCtx c;
  int nv = ir->un_nb_vars, n = ir->next_instruction_index;
  memset(&c, 0, sizeof c);
  c.ir = ir;
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
    if (!lt_vreg_addrtaken(ir, ir->un_vars[v].vreg))
      c.map[TCCIR_DECODE_VREG_POSITION(ir->un_vars[v].vreg)] = v;

  /* a variable whose address is taken may be set through it */
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
          (pos = TCCIR_DECODE_VREG_POSITION(vreg)) < c.nmap)
        c.map[pos] = -1;
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
  un_free_records(ir);
}

/* ------------------------------------------------------------------------ */
/* -Wreturn-local-addr: `return &x;`, `return buf;`, `return &s.m;`,
 * `return &a[2];`, a compound literal -- an address in this call's frame,
 * gone when it returns.  The frontend has already put &x into a temp, so this
 * reads the raw IR: one walk, a temp holding a frame address (that address
 * plus an offset too) reaching RETURNVALUE.  A pointer variable holding one is
 * not followed. */

void tcc_ir_return_local_check(TCCIRState *ir)
{
  int n = ir->next_instruction_index, nt = ir->next_temporary_variable;
  uint8_t *frame;
  if (!n)
    return;
  frame = tcc_mallocz(nt + 1); /* temp position -> holds a frame address */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand dest = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : irop_make_none();
    IROperand src1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : irop_make_none();
    IROperand src2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : irop_make_none();
    int f1 = 0, f2 = 0;
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
    if (q->op == TCCIR_OP_RETURNVALUE && f1)
      tcc_warning_c(warn_return_local_addr)("%i:'%s' returns the address of a local variable", q->line_num,
                                            funcname);
    if (!irop_is_none(dest) && irop_dest_defines_vreg(dest) &&
        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP &&
        TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)) < nt)
    {
      int f = 0;
      if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA)
        f = f1;
      else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
        f = f1 || (q->op == TCCIR_OP_ADD && f2);
      frame[TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest))] = f;
    }
  }
  tcc_free(frame);
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

void tcc_ir_dangling_check(TCCIRState *ir, Sym *func)
{
  DpCtx c;
  int n = ir->next_instruction_index;
  memset(&c, 0, sizeof c);
  c.ir = ir;
  if (!n)
    return;
  /* the variables ever given a frame address (a linear look is enough to
     find them: the dataflow decides where they hold one) */
  c.ntemps = ir->next_temporary_variable;
  c.tflag = tcc_mallocz((c.ntemps + 1) * sizeof(uint32_t));
  c.tgen = tcc_mallocz((c.ntemps + 1) * sizeof(uint32_t));
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
    if (!frame && sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP && irop_is_vreg_value(src1) &&
        TCCIR_DECODE_VREG_POSITION(sv) < c.ntemps)
      frame = c.tflag[TCCIR_DECODE_VREG_POSITION(sv)] & DP_FRAME;
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
            (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) &&
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
           object it is stored in (`s->peek = s->buf`) stays inside it. */
        uint64_t dm = ve_vreg(c, dv) & VE_ARGS, m = v1 & VE_ARGS & ~dm;
        int k = 0;
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
 * another deallocator, it is reported.  Plain `owned` says nothing and is
 * never compared.  One linear walk: a variable holds one kind of pointer. */
void tcc_ir_dealloc_check(TCCIRState *ir)
{
  int n = ir->next_instruction_index, ntemps = ir->next_temporary_variable, nv = ir->lt_nb_vars;
  int *tdeal, *vdeal;
  if (!n || !ir->lt_nb_calls)
    return;
  tdeal = tcc_mallocz((ntemps + 1) * sizeof(int));
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
        d = TCCIR_DECODE_VREG_POSITION(sv) < ntemps ? tdeal[TCCIR_DECODE_VREG_POSITION(sv)] : 0;
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
    }
    else if (q->op != TCCIR_OP_ASSIGN)
      d = 0;
    if (TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP)
    {
      if (TCCIR_DECODE_VREG_POSITION(dv) < ntemps)
      {
        tdeal[TCCIR_DECODE_VREG_POSITION(dv)] = d;
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
 * Any other loop joins the result into every element (vague).  An index that
 * is not a variable (`a[i + 1]`, `a[f()]`) is not followed: the array is not.
 * a[i] and a[j] (or a[1]) are not assumed to be the same element; an array
 * indexed by two variables never says that a[i] leaks. */

#define SC_UNINIT 1
#define SC_OWN 2
#define SC_MOVED 4
#define SC_NULL 8 /* owns nothing: null, an invalid handle, a constant */
#define SC_OWNBITS 15
#define SC_N_NULL 16 /* may be null / invalid */
#define SC_N_SET 32  /* may be set */
#define SC_STBITS 63
#define SC_VIA 64    /* set through a variable index: a leak is said once per class, by the index's name */
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
  int exact, lo, hi; /* exact: the elements lo..hi are visited, each once */
  int exit_b;        /* exact: the block the header's test leaves to */
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
  int32_t *tfa, *titem, *tcid; /* temp -> frame address, the item loaded, the call whose result it is */
  int32_t *tix, *tisc, *txat;  /* ... an element at a variable index: the index, the scale, where it was read */
  uint32_t *tgen, *ldone;      /* ldone [nslot]: the class's leak was said at this point (lgen) */
  uint32_t gen, lgen;
  ScPair *pair;
  int npair, ncand;
  int ntemps, report, ret_owned;
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

/* the vitem of the class whose element 0 is at fa, at index variable ix */
static int sc_vitem(ScCtx *c, int32_t fa, int32_t ix)
{
  for (int x = c->nslot; x < c->nslot + c->nvit; x++)
    if (c->it[x].off == fa && c->vivr[x] == ix && !c->untracked[x])
      return x;
  return -1;
}

/* OP reads (or, as a store's target, writes) an item: which, -1 */
static int sc_access(ScCtx *c, IROperand op)
{
  int pos;
  if (irop_is_none(op) || !op.is_lval)
    return -1;
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_is_local(op) && irop_get_vreg(op) < 0)
    return sc_item_at(c, (int32_t)irop_get_imm64_ex(c->ir, op));
  if (irop_get_tag(op) == IROP_TAG_VREG && (pos = sc_temp(c, op)) >= 0 && c->tfa[pos] != SC_NOADDR)
  {
    if (c->tix[pos] < 0)
      return sc_item_at(c, c->tfa[pos]);
    if (ix_written(c->ir, c->txat[pos] + 1, c->at, c->tix[pos]))
      return -1;
    return sc_vitem(c, c->tfa[pos], c->tix[pos]);
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
    if (c->multi[k])
      return;
    if (agg)
    {
      if (c->ldone[k] == c->lgen)
        return;
      c->ldone[k] = c->lgen;
    }
    if (c->kind[x] != SC_K_VIT)
    {
      int v = sc_first_vit(c, k);
      if (v < 0)
        return;
      name = c->it[v].name;
    }
    may = 1;
  }
  tcc_warning_c(warn_lifetime)("%i:'%s' leaks: it %s %s %s", line, name, may ? "may still own" : "still owns",
                               (c->it[x].kind & LTV_HANDLE)   ? "a handle"
                               : (c->it[x].kind & LTV_OBJECT) ? "what it holds"
                                                              : "memory",
                               where);
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
  c->cur[x] = (c->cur[x] & ~SC_OWNBITS) | SC_MOVED;
  c->move_line[x] = line;
  c->move_drop[x] = drop;
}

/* may X be null, and is that to be said */
static int sc_null_said(ScCtx *c, int x)
{
  return (c->cur[x] & SC_N_NULL) && !(c->cur[x] & SC_MOVED) && !((c->cur[x] & SC_VAGUE) && (c->cur[x] & SC_N_SET));
}

/* ---- variable indexes: the vitems, the loops' results ---- */

/* the elements of class k joined in state st */
static uint8_t sc_join(ScCtx *c, const uint8_t *st, int k)
{
  uint8_t s = 0;
  for (int x = 0; x < c->nslot; x++)
    if (c->cls[x] == k)
      s |= st[x];
  return s & ~SC_VIA;
}

/* state s of vitem v's element (any of its class) joined into the elements */
static void sc_flush(ScCtx *c, uint8_t *st, int v, uint8_t s)
{
  int k = c->cls[v];
  if (!(s & SC_STBITS))
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
    for (int x = 0; x < c->nslot; x++)
      if (c->cls[x] == k && (st[x] & SC_MOVED))
      {
        c->move_drop[v] = c->move_drop[x];
        break;
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
      st[pr->res] = 0;
    else if (pr->blk[b] && (t < 0 || !pr->blk[t]))
    {
      if (pr->exact && b == pr->hb && t == pr->exit_b)
      {
        uint8_t r = st[pr->res];
        for (int w = c->nslot; w < c->nslot + c->nvit; w++)
          if (w != v && c->cls[w] == k)
            sc_flush(c, st, w, st[w]);
        for (int x = 0; x < c->nslot; x++)
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
  for (int x = c->nslot; x < c->nslot + c->nvit; x++)
    if (sc_sel(c, x, sel))
    {
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
            sc_move(c, x, (call->drop >> k) & 1, line);
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
        int32_t fa = SC_NOADDR, it = -1, cid = -1, ix = -1, isc = 0, xat = i;
        if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA) && sc_addr(c, src1, &off))
          fa = off;
        else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && sc_addr(c, src1, &off) &&
                 irop_get_tag(src2) == IROP_TAG_IMM32)
          fa = off + (q->op == TCCIR_OP_ADD ? 1 : -1) * (int32_t)irop_get_imm64_ex(ir, src2);
        else if (q->op == TCCIR_OP_ADD && c->nvit && sc_addr(c, src1, &off) &&
                 (ix = ix_scaled(ir, bb->start_idx, i, src2, &isc)) >= 0)
          fa = off;
        else if ((q->op == TCCIR_OP_ASSIGN ||
                  ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_get_tag(src2) == IROP_TAG_IMM32)) &&
                 !src1.is_lval && (sp = sc_temp(c, src1)) >= 0 && c->tfa[sp] != SC_NOADDR && c->tix[sp] >= 0)
        {
          int32_t d = q->op == TCCIR_OP_ASSIGN ? 0 : (int32_t)irop_get_imm64_ex(ir, src2);
          fa = c->tfa[sp] + (q->op == TCCIR_OP_SUB ? -d : d);
          ix = c->tix[sp], isc = c->tisc[sp], xat = c->txat[sp];
        }
        if (fa == SC_NOADDR)
          ix = -1;
        if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN)
          it = (sp = sc_temp(c, src1)) >= 0 && !src1.is_lval ? c->titem[sp] : sc_access(c, src1);
        if (q->op == TCCIR_OP_FUNCCALLVAL && cfg->has_src2)
          cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2));
        c->tfa[pos] = fa, c->titem[pos] = it, c->tcid[pos] = cid;
        c->tix[pos] = ix, c->tisc[pos] = isc, c->txat[pos] = xat;
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
          (touch[3 * t + 2] < 0 || c->vivr[touch[3 * t + 2]] != iv))
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
      if (q1->op != TCCIR_OP_JUMPIF || irop_is_none(a) || !irop_is_vreg_value(a) || irop_get_vreg(a) != iv ||
          !irop_is_immediate(nk) || nk.is_lval || blk[pre])
        continue;
      int64_t nv = irop_get_imm64_ex(ir, nk), c0, lo, hi;
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
      if (step == 1 && (cond == TOK_LT || cond == TOK_ULT || cond == TOK_LE || cond == TOK_ULE))
        lo = c0, hi = cond == TOK_LT || cond == TOK_ULT ? nv - 1 : nv;
      else if (step == -1 && (cond == TOK_GE || cond == TOK_GT))
        hi = c0, lo = cond == TOK_GE ? nv : nv + 1;
      else
        continue;
      if (lo < 0 || lo > hi || hi >= c->it[c->cls[v]].nelem)
        continue;
      pr->exact = 1, pr->lo = (int)lo, pr->hi = (int)hi, pr->exit_b = e;
    }

  }
  tcc_free(work);
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
  int cap = nslot + SC_MAX_VIT + SC_MAX_PAIR + 1;
  c.it = tcc_malloc(cap * sizeof(IRLtSlot));
  memcpy(c.it, ir->lt_slots, nslot * sizeof(IRLtSlot));
  c.untracked = tcc_mallocz(cap);
  c.kind = tcc_mallocz(cap);
  c.cls = tcc_malloc(cap * sizeof(int));
  c.vivr = tcc_malloc(cap * sizeof(int32_t));
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
    int32_t *pfa = tcc_malloc((c.ntemps + 1) * 4 * sizeof(int32_t));
    int32_t *pix = pfa + (c.ntemps + 1), *pisc = pix + (c.ntemps + 1), *pxat = pisc + (c.ntemps + 1);
    for (int t = 0; t <= c.ntemps; t++)
      pfa[t] = SC_NOADDR, pix[t] = -1;
#define SC_ESCAPE(a)                                                                                                     \
  do                                                                                                                     \
  {                                                                                                                      \
    for (int x_ = 0; x_ < nslot; x_++)                                                                                   \
      if (c.it[x_].base <= (a) && (a) < c.it[x_].base + c.it[x_].base_size)                                              \
        c.untracked[x_] = 1;                                                                                             \
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
            /* an element at a variable index: its class's vitem */
            int e0 = -1, hit = 0;
            for (int x = 0; x < nslot && e0 < 0; x++)
              if (c.it[x].esize == pisc[tp] && c.it[x].idx == 0 && c.it[x].off == fo && c.cls[x] == x)
                e0 = x;
            if (ix_written(ir, pxat[tp] + 1, i, pix[tp]))
              hit = 1;
            else if (e0 >= 0)
            {
              w = -1;
              for (int x = nslot; x < nslot + c.nvit; x++)
                if (c.cls[x] == e0 && c.vivr[x] == pix[tp])
                  w = x;
              int in = ix_name(ir, pix[tp]);
              if (w < 0 && in && c.nvit < SC_MAX_VIT)
              {
                IRLtSlot *s = &c.it[w = nslot + c.nvit++];
                *s = c.it[e0];
                snprintf(s->name, sizeof s->name, "%.*s[%s]%s", (int)c.it[e0].pre, c.it[e0].name, get_tok_str(in, NULL),
                         c.it[e0].name + c.it[e0].suf);
                s->idx = -1;
                c.cls[w] = e0;
                c.vivr[w] = pix[tp];
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
              SC_ESCAPE(fo);
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
        int escape = 1, ixsrc = sp >= 0 && pix[sp] >= 0;
        if (dpos >= 0 && (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA))
        {
          pfa[dpos] = a, escape = 0;
          if (ixsrc)
            pix[dpos] = pix[sp], pisc[dpos] = pisc[sp], pxat[dpos] = pxat[sp];
        }
        else if (dpos >= 0 && (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB))
        {
          int32_t iv;
          int sc;
          if (irop_get_tag(ops[2]) == IROP_TAG_IMM32)
          {
            pfa[dpos] = a + (q->op == TCCIR_OP_ADD ? 1 : -1) * (int32_t)irop_get_imm64_ex(ir, ops[2]), escape = 0;
            if (ixsrc)
              pix[dpos] = pix[sp], pisc[dpos] = pisc[sp], pxat[dpos] = pxat[sp];
          }
          else if (!ixsrc && q->op == TCCIR_OP_ADD && (iv = ix_scaled(ir, bs, i, ops[2], &sc)) >= 0)
            pfa[dpos] = a, pix[dpos] = iv, pisc[dpos] = sc, pxat[dpos] = i, escape = 0;
          /* else an index that is not a variable: escape (not followed) */
        }
        else if (q->op == TCCIR_OP_FUNCPARAMVAL && !ixsrc)
        {
          int64_t enc = irop_get_imm64_ex(ir, ops[2]);
          int cid = TCCIR_DECODE_CALL_ID(enc), kk = TCCIR_DECODE_PARAM_IDX(enc);
          IRLtCall *call = cid >= 0 && cid < ir->lt_nb_calls ? &ir->lt_calls[cid] : NULL;
          escape = !(call && call->known && (kk -= call->implicit) >= 0 && kk < 32 &&
                     (((call->outown | call->dropobj | call->keep) >> kk) & 1));
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
        if (escape)
          SC_ESCAPE(a);
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
  }
  tcc_free(touch);
  c.move_line = tcc_mallocz((c.ni + 1) * sizeof(int));
  c.move_drop = tcc_mallocz(c.ni + 1);
  c.out_call = tcc_malloc((c.ni + 1) * sizeof(int32_t));
  memset(c.out_call, 0xff, (c.ni + 1) * sizeof(int32_t));
  c.tfa = tcc_malloc((c.ntemps + 1) * 6 * sizeof(int32_t));
  c.titem = c.tfa + c.ntemps + 1;
  c.tcid = c.titem + c.ntemps + 1;
  c.tix = c.tcid + c.ntemps + 1;
  c.tisc = c.tix + c.ntemps + 1;
  c.txat = c.tisc + c.ntemps + 1;
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
  tcc_free(c.it);
  tcc_free(c.untracked);
  tcc_free(c.kind);
  tcc_free(c.cls);
  tcc_free(c.vivr);
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
void tcc_ir_flow_checks(TCCIRState *ir, Sym *func)
{
  /* once per function: a body compiled again (late re-optimisation at the
     end of the unit) said everything the first time */
  if (func && func->type.ref && func->type.ref->f.func_compiled)
  {
    lt_free_records(ir);
    un_free_records(ir);
    tcc_ir_taint_free(ir);
    tcc_ir_ab_free(ir);
    return;
  }
  /* the checks hold their scratch until they return: -fmax-errors stops
     the compile after them */
  tcc_max_errors_defer(tcc_state);
  if (ir->ab_nb_sites)
    tcc_ir_ab_check(ir);
  if (ir->taint_on)
    tcc_ir_taint_check(ir, func);
  if (ir->lt_on)
  {
    /* before the lifetime check, which frees the variables' records */
    tcc_ir_view_escape_check(ir, func);
    tcc_ir_dealloc_check(ir);
    tcc_ir_slot_check(ir, func);
    tcc_ir_lifetime_check(ir, func);
  }
  if (ir->un_nb_vars)
    tcc_ir_uninit_check(ir);
  if ((tcc_state->warn_return_local_addr & WARN_ON) && !tcc_state->warn_none && func && func->type.ref &&
      (func->type.ref->type.t & VT_BTYPE) == VT_PTR)
    tcc_ir_return_local_check(ir);
  if (tcc_ir_dp_wanted())
    tcc_ir_dangling_check(ir, func);
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
