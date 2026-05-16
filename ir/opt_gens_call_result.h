/*
 *  TCC IR - Call-result dead elimination generator table (pre-SSA engine)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_GENS_CALL_RESULT_H
#define TCC_IR_OPT_GENS_CALL_RESULT_H

#include "opt_engine.h"

extern const IROptGen call_result_gens[];
extern const int call_result_gens_count;

extern const IROptGen call_result_post_gens[];
extern const int call_result_post_gens_count;

#endif /* TCC_IR_OPT_GENS_CALL_RESULT_H */
