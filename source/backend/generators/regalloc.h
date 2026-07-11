/*
 *  TCC Backend - Register-Allocation & Codegen-Prep Pipeline (regalloc.h)
 *
 * Extracted from source/opt/function_pipeline.c.  The optimization pipeline
 * (target-independent IR transforms) lives in source/opt; everything from
 * register allocation onward — leaf/tail analysis, RA setup, SSA regalloc,
 * post-RA micro-opts, register coalescing, stack-frame layout, nested-function
 * finalization and post-allocation cleanup — is the backend/generator's job
 * and lives here.
 *
 * gen_function (function.c) drives the sequence: it runs the optimizer
 * (source/opt), then calls into these two entry points.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "ir/core.h"

/* Analyze whether the function is a leaf / tail-call-only, setting
 * ir->leaffunc and ir->tail_call_only.  Backend property consumed by
 * prologue/epilogue generation. */
void tcc_ir_backend_analyze_leaf_and_tail_calls(TCCIRState *ir, int func_var);

/* Run the full register-allocation + codegen-prep tail: RA setup, SSA
 * regalloc, post-RA micro-opts, jump threading, register coalescing, final
 * stack layout, nested-function finalization and post-allocation passes.
 *
 * Updates *phase_start (bench "func-alloc" checkpoint) and mutates the global
 * `loc` (final stack frame size). */
void tcc_ir_backend_regalloc_pipeline(TCCIRState *ir, Sym *sym, int func_var,
                                      unsigned *phase_start, const char *funcname,
                                      Sym *global_label_stack_start);

