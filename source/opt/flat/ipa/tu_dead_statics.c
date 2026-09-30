/*
 *  TCC IR - end-of-TU unread-static-global analysis
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
#include "tu_summary.h"

/* Which symbols a relocation of emitted DATA refers to (by ELF index): an
 * object whose address is stored in data escapes where no function summary
 * can see it, and a function named in data can be called through it.  Code
 * needs no such scan: every function's own address uses were classified by
 * tcc_ir_collect_tu_static_reads_preopt (anything but an address derivation
 * counts as a read).  The frontend's addrtaken is no substitute -- it is set
 * by every `&x`, and zig's C writes every field store as
 * `*(&((T *)&x)->f) = v`. */
static uint8_t *tu_named_in_data(int *count)
{
  *count = symtab_section ? symtab_section->data_offset / sizeof(ElfW(Sym)) : 0;
  uint8_t *in_data = tcc_mallocz(*count ? *count : 1);
  for (int sh = 1; sh < tcc_state->nb_sections; sh++)
  {
    Section *sec = tcc_state->sections[sh];
    if (!sec || !(sec->sh_flags & SHF_ALLOC) || (sec->sh_flags & SHF_EXECINSTR) || !sec->reloc)
      continue;
    ElfW_Rel *rel = (ElfW_Rel *)sec->reloc->data, *rel_end = (ElfW_Rel *)(sec->reloc->data + sec->reloc->data_offset);
    for (; rel < rel_end; rel++)
      if (ELFW(R_SYM)(rel->r_info) < (unsigned)*count)
        in_data[ELFW(R_SYM)(rel->r_info)] = 1;
  }
  return in_data;
}

/* Statics never read by a reachable function get tu_no_readers; writers get func_late_reopt. */
void tcc_ir_tu_analyze_dead_statics(void)
{
  /* A root is any function reachable from outside this TU. */
  int total = 0;
  for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
    total++;
  if (total == 0)
    return;
  int nsyms;
  uint8_t *in_data = tu_named_in_data(&nsyms);

  /* Mark reachable: BFS over the call graph. */
  for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
  {
    Sym *fs = e->func_sym;
    int is_root = 0;
    if (!fs)
      continue;
    if (!(fs->type.t & VT_STATIC))
      is_root = 1;
    if (fs->a.addrtaken || (fs->c > 0 && fs->c < nsyms && in_data[fs->c]))
      is_root = 1;
    /* Constructors / destructors are entry points called by the runtime. */
    if (fs->type.ref && (fs->type.ref->f.func_ctor || fs->type.ref->f.func_dtor))
      is_root = 1;
    if (is_root && fs->type.ref)
      fs->type.ref->f.tu_reachable = 1;
  }

  int changed = 1;
  while (changed)
  {
    changed = 0;
    for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
    {
      if (!e->func_sym || !e->func_sym->type.ref)
        continue;
      if (!e->func_sym->type.ref->f.tu_reachable)
        continue;
      for (int i = 0; i < e->calls.count; i++)
      {
        Sym *callee = e->calls.items[i];
        if (!callee || !callee->type.ref)
          continue;
        if (!callee->type.ref->f.tu_reachable)
        {
          callee->type.ref->f.tu_reachable = 1;
          changed = 1;
        }
      }
    }
  }

  /* Candidates = union of static writes across the TU. */
  TuSymSet candidates = {0};
  for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
  {
    for (int i = 0; i < e->static_writes.count; i++)
      tu_symset_add(&candidates, e->static_writes.items[i]);
  }

  for (int c = 0; c < candidates.count; c++)
  {
    Sym *g = candidates.items[c];
    if (!g)
      continue;
    if (g->c <= 0 || g->c >= nsyms || in_data[g->c])
      continue; /* address stored in data; cannot prove no readers */
    int read_by_reachable = tu_symset_contains(&tu_source_reads, g);
    for (TuFuncSummary *e = tu_summary_head; e && !read_by_reachable; e = e->next)
    {
      if (!e->func_sym || !e->func_sym->type.ref)
        continue;
      if (!e->func_sym->type.ref->f.tu_reachable)
        continue;
      for (int i = 0; i < e->static_reads.count; i++)
      {
        if (e->static_reads.items[i] == g)
        {
          read_by_reachable = 1;
          break;
        }
      }
    }
    if (!read_by_reachable)
    {
      g->a.tu_no_readers = 1;
      /* Unreachable writers need no re-compile; their stores never execute. */
      for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
      {
        if (!e->func_sym || !e->func_sym->type.ref)
          continue;
        if (!e->func_sym->type.ref->f.tu_reachable)
          continue;
        for (int i = 0; i < e->static_writes.count; i++)
        {
          if (e->static_writes.items[i] == g)
          {
            e->func_sym->type.ref->f.func_late_reopt = 1;
            break;
          }
        }
      }
    }
  }

  tu_symset_free(&candidates);
  tcc_free(in_data);
}
