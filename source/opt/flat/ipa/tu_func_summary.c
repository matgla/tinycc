/*
 *  TCC IR - TU-wide static-global read/write/call summary
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"



#include "opt_utils.h"
#include "memref.h"

#include "tu_summary.h"

void tu_symset_add(TuSymSet *s, Sym *sym)
{
  if (!sym)
    return;
  for (int i = 0; i < s->count; i++)
    if (s->items[i] == sym)
      return;
  if (s->count >= s->capacity)
  {
    int new_cap = s->capacity ? s->capacity * 2 : 4;
    s->items = tcc_realloc(s->items, sizeof(Sym *) * new_cap);
    s->capacity = new_cap;
  }
  s->items[s->count++] = sym;
}

int tu_symset_contains(const TuSymSet *s, const Sym *sym)
{
  for (int i = 0; i < s->count; i++)
    if (s->items[i] == sym)
      return 1;
  return 0;
}

void tu_symset_free(TuSymSet *s)
{
  if (s->items)
    tcc_free(s->items);
  s->items = NULL;
  s->count = s->capacity = 0;
}

TuFuncSummary *tu_summary_head = NULL;

static TuFuncSummary *tu_summary_lookup(Sym *func_sym)
{
  for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
    if (e->func_sym == func_sym)
      return e;
  return NULL;
}

/* A static read in the pre-opt IR is never a tu_no_readers candidate: the late reopt cannot reproduce folds that removed the read. */
TuSymSet tu_source_reads = {0};

void tcc_ir_tu_func_summary_clear_all(void)
{
  while (tu_summary_head)
  {
    TuFuncSummary *n = tu_summary_head->next;
    tu_symset_free(&tu_summary_head->calls);
    tu_symset_free(&tu_summary_head->static_reads);
    tu_symset_free(&tu_summary_head->static_writes);
    tu_symset_free(&tu_summary_head->global_writes);
    tcc_free(tu_summary_head);
    tu_summary_head = n;
  }
  tu_symset_free(&tu_source_reads);
}

static Sym *tu_extract_sym(TCCIRState *ir, IROperand op)
{
  if (!op.is_sym)
    return NULL;
  IRPoolSymref *ref = irop_get_symref_ex(ir, op);
  return ref ? ref->sym : NULL;
}

static int tu_is_static_global_candidate(const Sym *sym)
{
  if (!sym)
    return 0;
  if ((sym->type.t & VT_BTYPE) == VT_FUNC)
    return 0;
  if (sym->a.weak || sym->a.dllimport)
    return 0;
  /* VT_STATIC implies storage in this TU even when VT_EXTERN is also set. */
  if (!(sym->type.t & VT_STATIC))
    return 0;
  /* const-qualified globals are not writeable, so never dead-store candidates. */
  if (sym->type.t & VT_CONSTANT)
    return 0;
  /* Volatile statics must observe stores (hardware registers etc). */
  if (sym->type.t & VT_VOLATILE)
    return 0;
  return 1;
}

#define TU_VREG_MAP_MAX 128
typedef struct
{
  int32_t vreg;
  Sym *sym;
} TuVregSymEntry;

static Sym *tu_vreg_map_lookup(const TuVregSymEntry *map, int count, int32_t vr)
{
  for (int i = 0; i < count; i++)
    if (map[i].vreg == vr)
      return map[i].sym;
  return NULL;
}

static void tu_vreg_map_set(TuVregSymEntry *map, int *count, int32_t vr, Sym *sym)
{
  for (int i = 0; i < *count; i++)
  {
    if (map[i].vreg == vr)
    {
      map[i].sym = sym;
      return;
    }
  }
  if (*count < TU_VREG_MAP_MAX)
  {
    map[*count].vreg = vr;
    map[*count].sym = sym;
    (*count)++;
  }
}

static void tu_vreg_map_clear(TuVregSymEntry *map, int *count, int32_t vr)
{
  for (int i = 0; i < *count; i++)
  {
    if (map[i].vreg == vr)
    {
      map[i].sym = NULL;
      return;
    }
  }
}

/* Any pre-opt value read or unrecognized address use of a static blocks tu_no_readers; store-address plumbing `T=&g+i; *T=v` does not. */
void tcc_ir_collect_tu_static_reads_preopt(TCCIRState *ir)
{
  TuVregSymEntry map[TU_VREG_MAP_MAX];
  int mc = 0;
  const int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    int is_store = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                    q->op == TCCIR_OP_STORE_POSTINC);
    int is_load = (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_LOAD_INDEXED ||
                   q->op == TCCIR_OP_LOAD_POSTINC);
    int is_addr_derive = (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD ||
                          q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_LEA);

    IROperand rops[3];
    int nrops = 0;
    if (irop_config[q->op].has_src1)
      rops[nrops++] = tcc_ir_op_get_src1(ir, q);
    if (irop_config[q->op].has_src2)
      rops[nrops++] = tcc_ir_op_get_src2(ir, q);
    if (q->op == TCCIR_OP_MLA)
      rops[nrops++] = tcc_ir_op_get_accum(ir, q);

    for (int oi = 0; oi < nrops; oi++)
    {
      IROperand op = rops[oi];
      Sym *sym = NULL;
      if (op.is_sym)
        sym = tu_extract_sym(ir, op);
      else if (irop_get_vreg(op) >= 0)
        sym = tu_vreg_map_lookup(map, mc, irop_get_vreg(op));
      if (!sym || !tu_is_static_global_candidate(sym))
        continue;
      if (op.is_lval || is_load) {
        tu_symset_add(&tu_source_reads, sym);
        continue;
      }
      /* Non-lval address: src1 of an address-derivation op continues the map; anything else escapes. */
      if (!(is_addr_derive && oi == 0) && !(q->op == TCCIR_OP_MLA && oi == 2))
        tu_symset_add(&tu_source_reads, sym);
    }

    if (irop_config[q->op].has_dest && !is_store)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && !dest.is_lval)
      {
        Sym *derived = NULL;
        if (is_addr_derive && irop_config[q->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          if (s1.is_sym && !s1.is_lval)
            derived = tu_extract_sym(ir, s1);
          else if (!s1.is_sym && !s1.is_lval && irop_get_vreg(s1) >= 0)
            derived = tu_vreg_map_lookup(map, mc, irop_get_vreg(s1));
        }
        if (!derived && q->op == TCCIR_OP_MLA)
        {
          IROperand acc = tcc_ir_op_get_accum(ir, q);
          if (acc.is_sym && !acc.is_lval)
            derived = tu_extract_sym(ir, acc);
          else if (!acc.is_sym && !acc.is_lval && irop_get_vreg(acc) >= 0)
            derived = tu_vreg_map_lookup(map, mc, irop_get_vreg(acc));
        }
        if (derived && tu_is_static_global_candidate(derived))
          tu_vreg_map_set(map, &mc, dvr, derived);
        else
          tu_vreg_map_clear(map, &mc, dvr);
      }
    }
  }
}

/* Block-copy / fill helpers whose only memory effect is through the destination
 * pointer (argument 0).  Callers resolve that pointer at the call site, so the
 * mod-ref walk may skip these even though they have no TU summary. */
static int tu_is_block_copy_helper(const char *nm)
{
  if (!nm)
    return 0;
  return !strcmp(nm, "memcpy") || !strcmp(nm, "memmove") || !strcmp(nm, "memset") ||
         !strcmp(nm, "__aeabi_memcpy") || !strcmp(nm, "__aeabi_memmove") ||
         !strcmp(nm, "__aeabi_memset") || !strcmp(nm, "__aeabi_memclr") ||
         !strcmp(nm, "__aeabi_memcpy4") || !strcmp(nm, "__aeabi_memmove4") ||
         !strcmp(nm, "__aeabi_memcpy8") || !strcmp(nm, "__aeabi_memmove8") ||
         !strcmp(nm, "__aeabi_memset4") || !strcmp(nm, "__aeabi_memset8") ||
         !strcmp(nm, "__aeabi_memclr4") || !strcmp(nm, "__aeabi_memclr8");
}

/* --- mod-ref query -------------------------------------------------------- */

#define TU_MODREF_MAX_VISIT 64

/* Can any function reachable from `callee` write `L`?  Walks the intra-TU call
 * graph collected above.  Every uncertainty is an immediate yes:
 *   - no summary (external callee, or a caller compiled before its callee —
 *     summaries are seeded post-codegen per function, so forward refs miss)
 *   - writes_unknown (a store this pass could not attribute to a symbol; this is
 *     also what makes a FRAME query safe, since reaching the caller's frame
 *     requires a pointer the callee could not have named)
 *   - more than TU_MODREF_MAX_VISIT distinct callees (give up rather than grow) */
static int tu_modref_walk(Sym *callee, MemLoc L)
{
  Sym *visited[TU_MODREF_MAX_VISIT];
  int nvisited = 0;
  Sym *stack[TU_MODREF_MAX_VISIT];
  int nstack = 0;

  stack[nstack++] = callee;
  while (nstack > 0)
  {
    Sym *f = stack[--nstack];
    int seen = 0;
    for (int i = 0; i < nvisited; i++)
      if (visited[i] == f)
      {
        seen = 1;
        break;
      }
    if (seen)
      continue;
    if (nvisited >= TU_MODREF_MAX_VISIT)
      return 1;
    visited[nvisited++] = f;

    TuFuncSummary *s = tu_summary_lookup(f);
    if (!s)
    {
      /* No summary.  A block-copy helper's effect was already resolved into the
       * CALLER's summary from its destination argument, so it is not an unknown
       * here; anything else (external, or not yet compiled) is. */
      if (tu_is_block_copy_helper(get_tok_str(f->v, NULL)))
        continue;
      return 1;
    }
    if (s->writes_unknown)
      return 1;
    if (L.kind == MEMLOC_GLOBAL && L.sym &&
        tu_symset_contains(&s->global_writes, L.sym))
      return 1;

    for (int i = 0; i < s->calls.count; i++)
    {
      if (nstack >= TU_MODREF_MAX_VISIT)
        return 1;
      stack[nstack++] = s->calls.items[i];
    }
  }
  return 0;
}

int tcc_ir_call_may_write(TCCIRState *ir, int call_idx, MemLoc L)
{
  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return 1;
  IRQuadCompact *q = &ir->compact_instructions[call_idx];
  if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
    return 1;
  /* An UNKNOWN location aliases everything, so no summary can rule it out. */
  if (L.kind != MEMLOC_GLOBAL && L.kind != MEMLOC_FRAME)
    return 1;
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!callee)
    return 1; /* indirect call */
  return tu_modref_walk(callee, L);
}

void tcc_ir_collect_tu_func_summary(TCCIRState *ir, Sym *func_sym)
{
  if (!ir || !func_sym)
    return;
  if (tu_summary_lookup(func_sym))
    return; /* Already collected for this Sym. */

  TuFuncSummary *s = tcc_mallocz(sizeof(*s));
  s->func_sym = func_sym;
  int tu_dbg = getenv("TCC_MODREF_DBG") != NULL;

  const int n = ir->next_instruction_index;
  int writes_any_static = 0;

  /* Phase 1: build the vreg -> static-sym map and collect address escapes. */
  TuVregSymEntry vreg_map[TU_VREG_MAP_MAX];
  int vreg_map_count = 0;
  TuSymSet addr_only_syms = {0}; /* statics referenced only by address (non-lval) */
  TuSymSet escaped_syms = {0};   /* address-of refs that escaped (call/store-as-value) */

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* A STORE dest is an address operand, not a value definition, so it must not disturb the map. */
    if (irop_config[q->op].has_dest &&
        q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
        q->op != TCCIR_OP_STORE_POSTINC)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && !dest.is_lval)
      {
        Sym *derived_sym = NULL;

        /* After fusion an ADD's SYMREF base can migrate into the MLA accumulator. */
        if (irop_config[q->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          if (s1.is_sym && !s1.is_lval)
          {
            Sym *sym = tu_extract_sym(ir, s1);
            if (sym && tu_is_static_global_candidate(sym))
              derived_sym = sym;
          }
        }
        if (!derived_sym && (q->op == TCCIR_OP_MLA))
        {
          IROperand acc = tcc_ir_op_get_accum(ir, q);
          if (acc.is_sym && !acc.is_lval)
          {
            Sym *sym = tu_extract_sym(ir, acc);
            if (sym && tu_is_static_global_candidate(sym))
              derived_sym = sym;
          }
        }

        if (!derived_sym &&
            (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD ||
             q->op == TCCIR_OP_SUB))
        {
          if (irop_config[q->op].has_src1)
          {
            IROperand s1 = tcc_ir_op_get_src1(ir, q);
            int32_t svr = irop_get_vreg(s1);
            if (svr >= 0 && !s1.is_sym)
              derived_sym = tu_vreg_map_lookup(vreg_map, vreg_map_count, svr);
          }
        }
        if (!derived_sym && (q->op == TCCIR_OP_MLA))
        {
          IROperand acc = tcc_ir_op_get_accum(ir, q);
          int32_t avr = irop_get_vreg(acc);
          if (avr >= 0 && !acc.is_sym)
            derived_sym = tu_vreg_map_lookup(vreg_map, vreg_map_count, avr);
        }

        if (derived_sym)
          tu_vreg_map_set(vreg_map, &vreg_map_count, dvr, derived_sym);
        else
          tu_vreg_map_clear(vreg_map, &vreg_map_count, dvr);
      }
    }

    /* STORE src1 is the stored value: a static address there escapes. */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
    {
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        int32_t svr = irop_get_vreg(s1);
        if (svr >= 0)
        {
          Sym *esc = tu_vreg_map_lookup(vreg_map, vreg_map_count, svr);
          if (esc)
            tu_symset_add(&escaped_syms, esc);
        }
      }
    }

    /* LOAD src1 is the address read from: a vreg derived from a static is a value read. */
    if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_LOAD_INDEXED ||
        q->op == TCCIR_OP_LOAD_POSTINC)
    {
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        int32_t svr = irop_get_vreg(s1);
        if (svr >= 0)
        {
          Sym *esc = tu_vreg_map_lookup(vreg_map, vreg_map_count, svr);
          if (esc)
            tu_symset_add(&escaped_syms, esc);
        }
      }
    }

    /* A static address passed as a call argument escapes. */
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        int32_t svr = irop_get_vreg(s1);
        if (svr >= 0)
        {
          Sym *esc = tu_vreg_map_lookup(vreg_map, vreg_map_count, svr);
          if (esc)
            tu_symset_add(&escaped_syms, esc);
        }
        if (s1.is_sym && !s1.is_lval)
        {
          Sym *sym = tu_extract_sym(ir, s1);
          if (sym && tu_is_static_global_candidate(sym))
            tu_symset_add(&escaped_syms, sym);
        }
      }
    }
  }

  /* Phase 2: classify reads and writes using the phase-1 map and escape sets. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_TRAP:
      s->body_elide_blocker = 1;
      break;
    default:
      break;
    }

    /* Mod-ref: opaque memory writers that the precise STORE handling below does
     * not model.  (Plain STOREs are excluded — they are attributed exactly.) */
    switch (q->op)
    {
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_IJUMP:
      s->writes_unknown = 1;
      break;
    default:
      break;
    }

    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (callee)
        tu_symset_add(&s->calls, callee);

      /* Mod-ref: a block-copy/fill helper writes ONLY through its destination
       * pointer, so resolve that here rather than letting the (summary-less,
       * external) callee poison the walk.  Without this a helper that merely
       * copies into the caller's OWN local — `y = retme(y)` in the 20040709-2
       * fn1* helpers — makes the caller look like it may write any global.
       * The callee still goes into `calls` above so tu_noreturn/tu_dead_statics
       * keep their existing view; tu_modref_walk skips it by name. */
      if (callee && tu_is_block_copy_helper(get_tok_str(callee->v, NULL)))
      {
        IROperand p0;
        if (ir_opt_get_call_param_operand(ir, i, 0, &p0))
        {
          MemLoc dl = memloc_of_pointer(ir, p0, i);
          if (dl.kind == MEMLOC_GLOBAL && dl.sym)
            tu_symset_add(&s->global_writes, dl.sym);
          else if (dl.kind != MEMLOC_FRAME)
            s->writes_unknown = 1;
        }
        else
          s->writes_unknown = 1;
      }
    }

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      Sym *write_sym = NULL;

      int dest_is_direct_sym =
          dest.is_sym &&
          (dest.is_lval || q->op == TCCIR_OP_STORE_INDEXED ||
           q->op == TCCIR_OP_STORE_POSTINC);
      if (dest_is_direct_sym)
      {
        write_sym = tu_extract_sym(ir, dest);
      }
      else
      {
        int32_t dvr = irop_get_vreg(dest);
        if (dvr >= 0)
          write_sym = tu_vreg_map_lookup(vreg_map, vreg_map_count, dvr);
      }

      if (write_sym && tu_is_static_global_candidate(write_sym))
      {
        tu_symset_add(&s->static_writes, write_sym);
        writes_any_static = 1;
      }

      /* Mod-ref: attribute this write to a named global or to our own frame.
       * memloc_of resolves direct StackLoc, `&sym+addend` derefs and pointer
       * def-chains; anything it cannot name (a param pointer, a loaded pointer)
       * may alias any global, so it poisons the whole summary. */
      MemLoc wl = memloc_of(ir, dest, i);
      if (wl.kind == MEMLOC_GLOBAL && wl.sym)
        tu_symset_add(&s->global_writes, wl.sym);
      else if (wl.kind != MEMLOC_FRAME)
        s->writes_unknown = 1;
    }

    /* A LOAD whose address operand is a static's SYMREF is a value read; the generic scan below would file it as address-only. */
    if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_LOAD_INDEXED ||
        q->op == TCCIR_OP_LOAD_POSTINC)
    {
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (s1.is_sym)
        {
          Sym *sym = tu_extract_sym(ir, s1);
          if (sym && tu_is_static_global_candidate(sym))
            tu_symset_add(&s->static_reads, sym);
        }
      }
    }

    /* Lval SYMREFs are value reads; non-lval (address-of) count only if the address escapes. */
    if (irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      Sym *sym = tu_extract_sym(ir, s1);
      if (sym && tu_is_static_global_candidate(sym))
      {
        if (s1.is_lval)
        {
          tu_symset_add(&s->static_reads, sym);
        }
        else
        {
          tu_symset_add(&addr_only_syms, sym);
        }
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      Sym *sym = tu_extract_sym(ir, s2);
      if (sym && tu_is_static_global_candidate(sym))
      {
        if (s2.is_lval)
        {
          tu_symset_add(&s->static_reads, sym);
        }
        else
        {
          tu_symset_add(&addr_only_syms, sym);
        }
      }
    }
    /* The MLA accumulator operand can also carry a SYMREF. */
    if (q->op == TCCIR_OP_MLA)
    {
      IROperand acc = tcc_ir_op_get_accum(ir, q);
      Sym *sym = tu_extract_sym(ir, acc);
      if (sym && tu_is_static_global_candidate(sym))
      {
        if (acc.is_lval)
          tu_symset_add(&s->static_reads, sym);
        else
          tu_symset_add(&addr_only_syms, sym);
      }
    }

    /* A deref operand whose vreg traces to a static is a read even when the deref was folded into a non-LOAD op. */
    {
      IROperand rops[3];
      int nrops = 0;
      if (irop_config[q->op].has_src1)
        rops[nrops++] = tcc_ir_op_get_src1(ir, q);
      if (irop_config[q->op].has_src2)
        rops[nrops++] = tcc_ir_op_get_src2(ir, q);
      if (q->op == TCCIR_OP_MLA)
        rops[nrops++] = tcc_ir_op_get_accum(ir, q);
      for (int oi = 0; oi < nrops; oi++)
      {
        IROperand op = rops[oi];
        if (!op.is_lval || op.is_sym)
          continue;
        int32_t vr = irop_get_vreg(op);
        if (vr < 0)
          continue;
        Sym *rsym = tu_vreg_map_lookup(vreg_map, vreg_map_count, vr);
        if (rsym && tu_is_static_global_candidate(rsym))
          tu_symset_add(&s->static_reads, rsym);
      }
    }

    /* Opaque ops: promote every touched static to read so no store is killed. */
    if (q->op == TCCIR_OP_INLINE_ASM || q->op == TCCIR_OP_TRAP ||
        q->op == TCCIR_OP_SETJMP)
    {
      for (int k = 0; k < s->static_writes.count; k++)
        tu_symset_add(&s->static_reads, s->static_writes.items[k]);
      for (int k = 0; k < addr_only_syms.count; k++)
        tu_symset_add(&s->static_reads, addr_only_syms.items[k]);
    }
  }

  /* An escaped address-of ref counts as a read: a callee may read through the pointer. */
  for (int k = 0; k < addr_only_syms.count; k++)
  {
    Sym *sym = addr_only_syms.items[k];
    if (!sym)
      continue;
    int is_escaped = 0;
    for (int e = 0; e < escaped_syms.count; e++)
    {
      if (escaped_syms.items[e] == sym)
      {
        is_escaped = 1;
        break;
      }
    }
    if (is_escaped)
      tu_symset_add(&s->static_reads, sym);
  }

  tu_symset_free(&addr_only_syms);
  tu_symset_free(&escaped_syms);

  if (writes_any_static && func_sym->type.ref)
    func_sym->type.ref->f.tu_static_writer = 1;

  if (tu_dbg)
  {
    fprintf(stderr, "[modref] %s: writes_unknown=%d global_writes=[",
            get_tok_str(func_sym->v, NULL), s->writes_unknown);
    for (int k = 0; k < s->global_writes.count; k++)
      fprintf(stderr, "%s%s", k ? "," : "",
              get_tok_str(s->global_writes.items[k]->v, NULL));
    fprintf(stderr, "] calls=[");
    for (int k = 0; k < s->calls.count; k++)
      fprintf(stderr, "%s%s", k ? "," : "", get_tok_str(s->calls.items[k]->v, NULL));
    fprintf(stderr, "]\n");
  }

  s->next = tu_summary_head;
  tu_summary_head = s;
}
