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

typedef enum {
  IR_CONSTRAINT_ANY        = 0,
  IR_CONSTRAINT_VREG,
  IR_CONSTRAINT_IMM,
  IR_CONSTRAINT_STACKOFF,
} IROptConstraint;

typedef enum {
  IR_RANGE_PREV_INSTR  = -1,
  IR_RANGE_NEXT_INSTR  =  1,
} IRRangeBound;

typedef struct {
  IRRangeBound    lo;
  IRRangeBound    hi;
  uint32_t        stop_ops;
  uint32_t        flags;
} IROptRangeSpec;

/* Cross-instruction link for the SSA-only PAIR() companion (opt_dsl_ssa.h):
 * resolve the SSA def of a source operand.  IR_PAIR_NONE (the zero value) means
 * no paired instruction, so an unspecified .pair is inert. */
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

typedef struct {
  IROptConstraint dest;
  IROptConstraint src1;
  IROptConstraint src2;
} IROptConstraintSpec;

typedef struct {
  IROptConstraintSpec   constraints;
  IROptRangeSpec        range;
  IROptPairSpec         pair;
} IROptPatternSpec;

typedef struct {
  int         new_op;
  IROperand   dest;
  IROperand   src1;
  IROperand   src2;
} IROptRewriteSpec;

