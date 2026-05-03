/*
 *  TCC IR - SSA Optimization Engine
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_SSA_OPT_H
#define TCC_IR_SSA_OPT_H

#include "cfg.h"
#include "ssa.h"

struct TCCIRState;

/* ============================================================================
 * SSA Use-Def Chains
 * ============================================================================ */

enum { SSA_USE_INSTR = 0, SSA_USE_PHI = 1 };

typedef struct IRSSAUse {
  int idx;       /* instruction index (INSTR) or block (PHI) */
  int slot;      /* phi operand slot (PHI only) */
  uint8_t kind;
} IRSSAUse;

typedef struct IRSSAVregInfo {
  int def_instr;      /* instruction index, -1 if phi/entry */
  int def_phi_block;  /* block ID if phi, -1 otherwise */
  int def_count;      /* number of definitions (>1 means non-SSA multi-def TEMP) */
  IRSSAUse *uses;
  int use_count;
  int use_cap;
} IRSSAVregInfo;

/* ============================================================================
 * Optimization Context
 * ============================================================================ */

typedef struct IRSSAOptCtx {
  struct TCCIRState *ir;
  IRSSAState *ssa;
  IRCFG *cfg;
  IRSSAVregInfo *vinfo;   /* indexed by TEMP vreg position */
  int vinfo_cap;
  int changes;
  int no_stack_fwd;       /* disable stack store-load forwarding in load_cse */
} IRSSAOptCtx;

/* ============================================================================
 * Generator: explicit per-opcode rewrite rule (like thop_* instruction builders)
 * ============================================================================ */

typedef int (*ssa_gen_fn)(IRSSAOptCtx *ctx, int instr_idx);

typedef struct IRSSAOptGen {
  int op;
  ssa_gen_fn fn;
  const char *name;
} IRSSAOptGen;

/* ============================================================================
 * Pass Descriptor
 * ============================================================================ */

typedef int (*ssa_pass_fn)(IRSSAOptCtx *ctx);

typedef struct IRSSAOptPass {
  const char *name;
  ssa_pass_fn run;              /* custom pass function, or NULL */
  const IRSSAOptGen *gens;      /* generator table, or NULL */
  int gen_count;
} IRSSAOptPass;

/* ============================================================================
 * Driver API
 * ============================================================================ */

void tcc_ir_ssa_opt_init(IRSSAOptCtx *ctx, struct TCCIRState *ir,
                         IRSSAState *ssa, IRCFG *cfg);
void tcc_ir_ssa_opt_rebuild(IRSSAOptCtx *ctx);
void tcc_ir_ssa_opt_free(IRSSAOptCtx *ctx);
int tcc_ir_ssa_opt_run(IRSSAOptCtx *ctx);

/* Run a generator table over all instructions */
int ssa_opt_run_gens(IRSSAOptCtx *ctx, const IRSSAOptGen *gens, int count);

/* ============================================================================
 * Use-Def Helpers
 * ============================================================================ */

IRSSAVregInfo *ssa_opt_vinfo(IRSSAOptCtx *ctx, int32_t vreg);
void ssa_opt_add_use_instr(IRSSAVregInfo *vi, int instr_idx);
void ssa_opt_add_use_phi(IRSSAVregInfo *vi, int block, int slot);
void ssa_opt_remove_use_instr(IRSSAVregInfo *vi, int instr_idx);
void ssa_opt_nop_instr(IRSSAOptCtx *ctx, int idx);
int ssa_opt_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr, int32_t new_vr);
int ssa_opt_has_side_effects(int op);

/* ============================================================================
 * Target-Independent Passes
 * ============================================================================ */

int ssa_opt_dce(IRSSAOptCtx *ctx);
int ssa_opt_cprop(IRSSAOptCtx *ctx);
int ssa_opt_fold(IRSSAOptCtx *ctx);
int ssa_opt_phi_simplify(IRSSAOptCtx *ctx);
int ssa_opt_strength(IRSSAOptCtx *ctx);
int ssa_opt_gvn(IRSSAOptCtx *ctx);
int ssa_opt_reassoc(IRSSAOptCtx *ctx);
int ssa_opt_narrow(IRSSAOptCtx *ctx);
int ssa_opt_branch(IRSSAOptCtx *ctx);
int ssa_opt_sccp(IRSSAOptCtx *ctx);
int ssa_opt_load_cse(IRSSAOptCtx *ctx);
int ssa_opt_var_forward(IRSSAOptCtx *ctx);

/* ============================================================================
 * Target-Specific Generator Registration
 *
 * Backends call tcc_ir_ssa_opt_register_target() once at startup to provide
 * their generator table. The driver runs them as the last pass.
 * ============================================================================ */

void tcc_ir_ssa_opt_register_target(const IRSSAOptGen *gens, int count);

#endif /* TCC_IR_SSA_OPT_H */
