/*
 *  TCC IR - SSA loop: count-up to decrement-to-zero rewrite
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_loop_utils.h"
#include "loop_cand.h"

#define SSA_DTZ_MAX_PASSES 4

static int dtz_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                             uint8_t *member, uint8_t *scratch)
{
  LcsSpan sp;
  if (!lcs_cand_span(ir, cfg, header_b, member, scratch, 0, &sp))
    return 0;
  return dtz_try_region(ir, sp.start, sp.end, sp.header, sp.preheader);
}

int ssa_opt_decrement_to_zero(TCCIRState *ir)
{
  return lcs_run_outermost(ir, SSA_DTZ_MAX_PASSES, 0, dtz_try_candidate);
}
