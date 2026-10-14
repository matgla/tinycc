/*
 *  TCC IR — Optimization DSL: Type Definitions
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation.
 */

#pragma once

#include <stdint.h>

/* Requires ir.h (for IROperand) to be included first. See docs/optimizations/opt_dsl_framework.md. */

/* Which source operand's SSA def PAIR() (opt_dsl_ssa.h) resolves. */
typedef enum {
  IR_PAIR_NONE         = 0,
  IR_PAIR_DEF_OF_SRC1,
  IR_PAIR_DEF_OF_SRC2,
} IRPairLink;

typedef struct {
  IRPairLink  link;        /* which source's def to resolve (NONE = disabled) */
  int         op;          /* required opcode of that def, or -1 for any */
  int         single_use;  /* also require the def's result has exactly one use */
} IROptPairSpec;
