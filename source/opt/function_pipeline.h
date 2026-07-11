/*
 * function_pipeline.h — Target-independent optimization pipeline entry point
 *
 * Runs every optimization pass that operates on a single function's IR, from
 * propagation through the post-pipeline cleanups, in the documented ordering.
 * This unit is optimization only: register allocation, stack layout and
 * codegen preparation are the backend's job and are driven by gen_function
 * (source/backend/generators/), which calls the backend RA pipeline
 * (regalloc.h) after this returns.
 *
 * See function_pipeline.c for the full pass list.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef SOURCE_OPT_FUNCTION_PIPELINE_H
#define SOURCE_OPT_FUNCTION_PIPELINE_H

#include "ir/core.h"

/* Run the target-independent optimization pipeline on `ir`.
 *
 * Writes *nonstatic_global_copier out (1 if the function copies an aggregate
 * from a non-static global and takes a parameter, used by the inline classify
 * code that follows gen_function).
 *
 * Side-effects: mutates the global `loc` (shrinks the stack frame when
 * optimization eliminates locals); reads tcc_state->optimize and the
 * tcc_state->opt_* feature flags.
 */
void tcc_ir_opt_run_function_pipeline(TCCIRState *ir, Sym *sym, int func_var,
                                      int *nonstatic_global_copier);

#endif /* SOURCE_OPT_FUNCTION_PIPELINE_H */
