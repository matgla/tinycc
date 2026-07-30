/*
 *  TCC IR - end-of-TU noreturn/pure propagation to callers
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
#include "opt_loop_utils.h"
#include "tu_summary.h"



/* Callee noreturn/purity facts are only final once every function in the TU is compiled. */
void tcc_ir_tu_propagate_noreturn_to_callers(void)
{
  for (TuFuncSummary *e = tu_summary_head; e; e = e->next)
  {
    Sym *fs = e->func_sym;
    if (!fs || !fs->type.ref)
      continue;
    if (!fs->type.ref->f.func_keep_tokens_for_noreturn)
      continue;
    if (fs->type.ref->f.func_late_reopt)
      continue;
    for (int i = 0; i < e->calls.count; i++)
    {
      Sym *callee = e->calls.items[i];
      if (!callee || !callee->type.ref)
        continue;
      int inferred_purity = tcc_ir_lookup_func_purity(tcc_state, callee->v);
      if (callee->type.ref->f.func_noreturn)
      {
        fs->type.ref->f.func_late_reopt = 1;
        break;
      }
      if (inferred_purity >= TCC_FUNC_PURITY_PURE && !e->body_elide_blocker &&
          ((fs->type.ref->type.t & VT_BTYPE) == VT_VOID))
      {
        int all_calls_elidable = 1;
        for (int j = 0; j < e->calls.count; j++)
        {
          Sym *other = e->calls.items[j];
          if (!other || tcc_ir_lookup_func_purity(tcc_state, other->v) < TCC_FUNC_PURITY_PURE)
          {
            all_calls_elidable = 0;
            break;
          }
        }
        if (all_calls_elidable)
        {
          fs->type.ref->f.func_late_reopt = 1;
          break;
        }
      }
    }
  }
}
