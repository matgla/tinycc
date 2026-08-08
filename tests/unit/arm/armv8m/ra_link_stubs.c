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
#include "source/opt/ssa/include/ssa_opt.h"
#include "opt/ssa/strength.h"
#include "opt/ssa/reassoc.h"
#include "opt/ssa/cprop.h"

/* From the frontend (source/frontend/gen/core/state.c, formerly tccgen.c) -
 * used only for debug/dump messages. */
const char *funcname = "unit_test";

/* Also from the frontend: ir/regalloc.c's ra_may_need_frame_pointer() reads
 * it to decide whether a variadic function needs a frame pointer.  The RA
 * suites build IR directly and never parse a function, so 0 (not variadic)
 * is the right answer for every test. */
int func_var = 0;

/* dbg_scan_overlap / dbg_scan_imm_dest used to be stubbed here too, but
 * ir/opt_pipeline.c (linked for tests/unit/arm/armv8m/test_opt_fusion.c's
 * gens_*_ex adapters) now provides the real, non-static definitions --
 * duplicating them here would be a link error (multiple definition). */

/* SSA optimizer driver - guarded by UT_SSA_OPT_REAL so build_ssaopt can
 * link the real ssa_opt*.c files without multiple-definition clashes.
 * When UT_SSA_OPT_REAL is defined, this block is skipped and the real
 * ssa_opt*.c files are linked instead (see the build_ssaopt target). */
#ifndef UT_SSA_OPT_REAL
/* Individual SSA optimization passes - all no-ops for RA isolation. */
void tcc_ir_ssa_opt_init(IRSSAOptCtx *ctx, struct TCCIRState *ir,
                         struct IRSSAState *ssa, struct IRCFG *cfg)
{
  memset(ctx, 0, sizeof(*ctx));
  ctx->ir = ir;
  ctx->ssa = ssa;
  ctx->cfg = cfg;
  ctx->vinfo_cap = ir ? ir->next_temporary_variable : 0;
  if (ctx->vinfo_cap <= 0)
    ctx->vinfo_cap = 1;
  ctx->vinfo = tcc_mallocz(ctx->vinfo_cap * sizeof(struct IRSSAVregInfo));
}

void tcc_ir_ssa_opt_rebuild(IRSSAOptCtx *ctx)
{
  (void)ctx;
}

void tcc_ir_ssa_opt_free(IRSSAOptCtx *ctx)
{
  if (!ctx)
    return;
  if (ctx->vinfo) {
    for (int i = 0; i < ctx->vinfo_cap; i++)
      tcc_free(ctx->vinfo[i].uses);
    tcc_free(ctx->vinfo);
  }
  ctx->vinfo = NULL;
  ctx->vinfo_cap = 0;
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

int tcc_ir_ssa_opt_guard_collapse(IRSSAOptCtx *ctx)
{
  (void)ctx;
  return 0;
}

int tcc_ir_ssa_opt_ptr_store_dse(IRSSAOptCtx *ctx)
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
int ssa_opt_reassoc(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_narrow(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_branch(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_cmp_eq_prop(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_vrp(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_setif_or_taut(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_setif_mask_fold(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_bool_norm(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_cmp_offset_fold(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_sccp(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_load_cse(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int tcc_ir_ssa_opt_const_string_fold(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int tcc_ir_ssa_opt_const_string_fold_flat(TCCIRState *ir) { (void)ir; return 0; }
int tcc_ir_ssa_opt_bitop_const_fold(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int tcc_ir_ssa_opt_global_addr_hoist(TCCIRState *ir) { (void)ir; return 0; }
int tcc_ir_ssa_opt_local_addr_cse(TCCIRState *ir) { (void)ir; return 0; }
int tcc_ir_ssa_opt_loop_addr_hoist(TCCIRState *ir) { (void)ir; return 0; }
int ssa_opt_var_forward(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_var_to_param_forward(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_var_const_fold(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_var_imm_prop(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_const_prop_tmp(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
int ssa_opt_dead_loop(IRSSAOptCtx *ctx) { (void)ctx; return 0; }

/* Use-def helpers - real implementations (mirrors ir/opt/ssa_opt.c, which is
 * not linked into this harness). The individual ssa_opt_<pass> functions
 * above are no-ops so *they* never build/consult chains via these helpers,
 * but arch/arm/ssa_opt_arm.c's target-specific generators call these
 * directly and need real def/use-chain semantics to be exercisable at all
 * (see tcc_ir_ssa_opt_init's comment). */
struct IRSSAVregInfo *ssa_opt_vinfo(IRSSAOptCtx *ctx, int32_t vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP)
    return NULL;
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (pos >= ctx->vinfo_cap)
    return NULL;
  return &ctx->vinfo[pos];
}

void ssa_opt_add_use_instr(struct IRSSAVregInfo *vi, int instr_idx)
{
  if (vi->use_count >= vi->use_cap) {
    int nc = vi->use_cap ? vi->use_cap * 2 : 4;
    vi->uses = tcc_realloc(vi->uses, nc * sizeof(*vi->uses));
    vi->use_cap = nc;
  }
  vi->uses[vi->use_count].idx = instr_idx;
  vi->uses[vi->use_count].slot = 0;
  vi->uses[vi->use_count].kind = SSA_USE_INSTR;
  vi->use_count++;
}

void ssa_opt_add_use_phi(struct IRSSAVregInfo *vi, int block, int slot)
{
  if (vi->use_count >= vi->use_cap) {
    int nc = vi->use_cap ? vi->use_cap * 2 : 4;
    vi->uses = tcc_realloc(vi->uses, nc * sizeof(*vi->uses));
    vi->use_cap = nc;
  }
  vi->uses[vi->use_count].idx = block;
  vi->uses[vi->use_count].slot = slot;
  vi->uses[vi->use_count].kind = SSA_USE_PHI;
  vi->use_count++;
}

void ssa_opt_remove_use_instr(struct IRSSAVregInfo *vi, int instr_idx)
{
  for (int i = 0; i < vi->use_count; i++) {
    if (vi->uses[i].kind == SSA_USE_INSTR && vi->uses[i].idx == instr_idx) {
      vi->uses[i] = vi->uses[--vi->use_count];
      return;
    }
  }
}

void ssa_opt_scan_instr_uses(IRSSAOptCtx *ctx, int i, IRQuadCompact *q)
{
  TCCIRState *ir = ctx->ir;
  if (irop_config[q->op].has_src1) {
    struct IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
    if (vi)
      ssa_opt_add_use_instr(vi, i);
  }
  if (irop_config[q->op].has_src2) {
    struct IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(tcc_ir_op_get_src2(ir, q)));
    if (vi)
      ssa_opt_add_use_instr(vi, i);
  }
  if (q->op == TCCIR_OP_MLA) {
    struct IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(tcc_ir_op_get_accum(ir, q)));
    if (vi)
      ssa_opt_add_use_instr(vi, i);
  }
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_STORE_POSTINC) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!(q->op == TCCIR_OP_STORE && !d.is_lval)) {
      struct IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(d));
      if (vi)
        ssa_opt_add_use_instr(vi, i);
    }
  }
}

void ssa_opt_nop_instr(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op == TCCIR_OP_NOP)
    return;

  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    struct IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(s));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    struct IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(s));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (q->op == TCCIR_OP_MLA) {
    IROperand a = tcc_ir_op_get_accum(ir, q);
    struct IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(a));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_STORE_POSTINC) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    struct IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(d));
    if (vi)
      ssa_opt_remove_use_instr(vi, idx);
  }

  q->op = TCCIR_OP_NOP;
}

int ssa_opt_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr, int32_t new_vr)
{
  (void)ctx;
  (void)old_vr;
  (void)new_vr;
  return 0;
}

int ssa_opt_can_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr)
{
  (void)ctx;
  (void)old_vr;
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

/* Guard end - see comment at top of file. */
#endif /* UT_SSA_OPT_REAL */
