/*
 *  TCC IR - Internal Header
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_INTERNAL_H
#define TCC_IR_INTERNAL_H

#include <stdbool.h>

/* ============================================================================
 * Include tcc.h first (required for all definitions)
 * This must be included before any other headers to ensure VT_*, etc are defined
 * ============================================================================ */

#define USING_GLOBALS
#include "tcc.h"

/* ============================================================================
 * Module Headers
 * ============================================================================ */

/* Note: tccir.h and tccir_operand.h are already included via tcc.h */
#include "codegen.h"
#include "core.h"
#include "dump.h"
#include "machine_op.h"
#include "opt.h"
#include "pool.h"
#include "regalloc.h"
#include "ssa.h"
#include "opt/ssa_opt.h"
#include "stack.h"
#include "type.h"
#include "vreg.h"

#endif /* TCC_IR_INTERNAL_H */
