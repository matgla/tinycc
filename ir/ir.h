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
#include "type.h"
#include "pool.h"
#include "vreg.h"
#include "live.h"
#include "stack.h"
#include "mat.h"
#include "opt.h"
#include "codegen.h"
#include "dump.h"
#include "core.h"

#endif /* TCC_IR_INTERNAL_H */
