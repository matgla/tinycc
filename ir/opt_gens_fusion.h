/*
 *  TCC IR - Fusion generator table (pre-SSA engine)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_GENS_FUSION_H
#define TCC_IR_OPT_GENS_FUSION_H

#include "opt_engine.h"

extern const IROptGen fusion_gens[];
extern const int fusion_gens_count;

extern const IROptGen fusion_deref_indexed_gens[];
extern const int fusion_deref_indexed_gens_count;

extern const IROptGen fusion_disp_gens[];
extern const int fusion_disp_gens_count;

extern const IROptGen fusion_chain_gens[];
extern const int fusion_chain_gens_count;

extern const IROptGen fusion_pair_reorder_gens[];
extern const int fusion_pair_reorder_gens_count;

#endif /* TCC_IR_OPT_GENS_FUSION_H */
