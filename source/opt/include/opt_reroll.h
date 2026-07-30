/*
 *  TCC IR - Identical-Block Loop Re-Rolling
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_REROLL_H
#define TCC_IR_OPT_REROLL_H

struct TCCIRState;

/* Re-rolls runs of identical blocks (modulo vreg renaming) into a counted loop; returns runs rerolled. */
int tcc_ir_opt_reroll(struct TCCIRState *ir);

#endif
