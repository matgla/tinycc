/*
 *  TCC IR - SSA loop: counted-down loop to decrement-to-carry rewrite
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_loop_utils.h"
#include "loop_cand.h"

#define SSA_DTC_MAX_PASSES 4

static int dtc_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                             uint8_t *member, uint8_t *scratch)
{
  LcsSpan sp;
  if (!lcs_cand_span(ir, cfg, header_b, member, scratch, 0, &sp))
    return 0;
  return dtc_try_region(ir, cfg, member, sp.start, sp.end, sp.header);
}

int ssa_opt_decrement_to_carry(TCCIRState *ir)
{
  return lcs_run_outermost(ir, SSA_DTC_MAX_PASSES, 0, dtc_try_candidate);
}
