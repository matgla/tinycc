/*
 *  ra_link_stubs.c - link stubs for register-allocation unit tests
 *
 *  Linking ir/regalloc.c pulls in debug scanners, the SSA optimizer driver,
 *  and a few legacy optimization passes.  Those subsystems have their own
 *  unit-test suites; the RA suites only need the allocator itself, so this
 *  file provides minimal no-op definitions that let the RA tests link without
 *  dragging in the entire optimizer/backend dependency graph.
 */

#include <limits.h>

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

/* From tccgen.c - used only for debug/dump messages. */
const char *funcname = "unit_test";

/* Debug scanners declared in ir/regalloc.c / ir/opt_pipeline.c. */
void dbg_scan_overlap(struct TCCIRState *ir, const char *pass)
{
  (void)ir;
  (void)pass;
}

void dbg_scan_imm_dest(struct TCCIRState *ir, const char *pass)
{
  (void)ir;
  (void)pass;
}

/* SSA optimizer driver - enough to satisfy tcc_ir_ssa_regalloc's call sites
 * without running any real optimization passes. */
void tcc_ir_ssa_opt_init(IRSSAOptCtx *ctx, struct TCCIRState *ir,
                         struct IRSSAState *ssa, struct IRCFG *cfg)
{
  memset(ctx, 0, sizeof(*ctx));
  ctx->ir = ir;
  ctx->ssa = ssa;
  ctx->cfg = cfg;
}

void tcc_ir_ssa_opt_rebuild(IRSSAOptCtx *ctx)
{
  (void)ctx;
}

void tcc_ir_ssa_opt_free(IRSSAOptCtx *ctx)
{
  if (!ctx)
    return;
  ctx->ir = NULL;
  ctx->ssa = NULL;
  ctx->cfg = NULL;
}

int tcc_ir_ssa_opt_run(IRSSAOptCtx *ctx)
{
  (void)ctx;
  return 0;
}

int tcc_ir_ssa_opt_run_target(IRSSAOptCtx *ctx)
{
  (void)ctx;
  return 0;
}

/* Target generator registration - no target generators for RA tests. */
void tcc_ir_ssa_opt_register_target(const struct IRSSAOptGen *gens, int count)
{
  (void)gens;
  (void)count;
}

/* Individual SSA optimization passes - all no-ops for RA isolation. */
int ssa_opt_dce(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_cprop(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_fold(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_phi_simplify(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_strength(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_gvn(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_reassoc(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_narrow(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_branch(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_cmp_eq_prop(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_sccp(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_load_cse(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_var_forward(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_var_to_param_forward(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_var_const_fold(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_dead_loop(IRSSAOptCtx *ctx) { (void)ctx; return 0; }

/* Use-def helpers - stubs; passes are no-ops so chains are unused. */
struct IRSSAVregInfo *ssa_opt_vinfo(IRSSAOptCtx *ctx, int32_t vreg)
{
  (void)ctx;
  (void)vreg;
  return NULL;
}

void ssa_opt_add_use_instr(struct IRSSAVregInfo *vi, int instr_idx)
{
  (void)vi;
  (void)instr_idx;
}

void ssa_opt_add_use_phi(struct IRSSAVregInfo *vi, int block, int slot)
{
  (void)vi;
  (void)block;
  (void)slot;
}

void ssa_opt_remove_use_instr(struct IRSSAVregInfo *vi, int instr_idx)
{
  (void)vi;
  (void)instr_idx;
}

void ssa_opt_nop_instr(IRSSAOptCtx *ctx, int idx)
{
  (void)ctx;
  (void)idx;
}

int ssa_opt_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr, int32_t new_vr)
{
  (void)ctx;
  (void)old_vr;
  (void)new_vr;
  return 0;
}

void ssa_drop_phi_edge(IRSSAOptCtx *ctx, int dead_pred_block, int target_block_idx)
{
  (void)ctx;
  (void)dead_pred_block;
  (void)target_block_idx;
}

int ssa_opt_resolve_lea_stackloc(IRSSAOptCtx *ctx, int32_t vr)
{
  (void)ctx;
  (void)vr;
  return INT_MIN;
}

int ssa_opt_resolve_temp_to_base_off(IRSSAOptCtx *ctx, int32_t vr,
                                      int32_t *out_base, int32_t *out_off)
{
  (void)ctx;
  (void)vr;
  (void)out_base;
  (void)out_off;
  return 0;
}

int ssa_opt_indirect_stack_offset(IRSSAOptCtx *ctx, const struct IRQuadCompact *q,
                                   int side)
{
  (void)ctx;
  (void)q;
  (void)side;
  return INT_MIN;
}

/* Legacy optimization passes referenced from the allocator pipeline. */
int tcc_ir_opt_switch_to_data(struct TCCIRState *ir)
{
  (void)ir;
  return 0;
}

int tcc_ir_opt_const_memcpy_to_dest(struct TCCIRState *ir)
{
  (void)ir;
  return 0;
}
