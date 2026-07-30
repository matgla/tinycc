/*
 *  TCC IR - If-conversion: control-flow diamonds -> SELECT (pre- and post-RA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct TCCIRState;

int tcc_ir_opt_setif_neg_to_select(struct TCCIRState *ir);
int tcc_ir_opt_select(struct TCCIRState *ir);
int tcc_ir_opt_post_ra_forward_diamond(struct TCCIRState *ir);

/* Codegen-time if-conversion: does the ALU op at instruction `i` feed an
 * else-identity SELECT (else reg == dest reg, post-RA) whose flags are still
 * live, such that the compute can be emitted predicated into the SELECT dest
 * inside an IT block?  Returns 1 and fills *sel_index_out (SELECT to skip) and
 * *cond_out (SELECT condition) on a match.  Pure detection — the backend still
 * gates emission via tcc_gen_machine_can_predicate_alu. */
int tcc_ir_ifconv_match_predicated_select(struct TCCIRState *ir, int i,
                                          int *sel_index_out, int *cond_out);

