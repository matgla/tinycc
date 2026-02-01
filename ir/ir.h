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

/* This header is INTERNAL to the IR implementation.
 * It should only be included by .c files in the ir/ directory.
 * The public API is in tccir.h at the project root.
 */

/* ============================================================================
 * Include tcc.h first (required for all definitions)
 * ============================================================================ */

#define USING_GLOBALS
#include "tcc.h"

/* ============================================================================
 * Module Headers
 * ============================================================================ */

#include "operand.h"
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
