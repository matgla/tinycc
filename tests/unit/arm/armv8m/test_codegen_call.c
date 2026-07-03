/*
 *  test_codegen_call.c - backend unit tests for call/return IR ops
 *
 *  Exercises AAPCS incoming parameter setup (tcc_ir_codegen_params_setup),
 *  outgoing FUNCPARAMVAL/FUNCCALLVAL operand lowering, RETURNVALUE /
 *  drop_return helpers, and the SValue register-fill helper
 *  (tcc_ir_fill_registers) in ir/codegen.c.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ir/ssa.h"
#include "ir/vreg.h"
#include "ir/regalloc.h"
#include "ir/codegen.h"
#include "ir/machine_op.h"
#include "arch/arm/arm_regalloc.h"
#include "codegen_mop_stubs.h"
#include "ut.h"

/* Declared/defined in ir/codegen.c but not exported in tccir.h -- its only
 * real caller is tcc_ir_codegen_inline_asm_by_id() (also static to that
 * file), which this harness cannot reach without real frontend inline-asm
 * plumbing. tcc_ir_fill_registers() itself only touches TCCIRState/SValue/
 * IRLiveInterval, so it's independently unit-testable by declaring it here. */
extern void tcc_ir_fill_registers(TCCIRState *ir, SValue *sv);

static SValue sv_var(int vreg, int vt)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = vt;
  return sv;
}

static SValue sv_const(int v)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = v;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_param_marker(int call_id, int idx)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = (int64_t)TCCIR_ENCODE_PARAM(call_id, idx);
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_call_id(int call_id, int argc)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = (int64_t)TCCIR_ENCODE_CALL(call_id, argc);
  sv.type.t = VT_INT;
  return sv;
}

/* Full-control SValue builder for tcc_ir_fill_registers() tests: sets .r and
 * .vr directly (sv_var()/sv_const() above only cover the two shapes the
 * call-lowering tests need). */
static SValue sv_raw(int r, int vr)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = r;
  sv.vr = vr;
  sv.type.t = VT_INT;
  return sv;
}

static void setup_tcc_state(void)
{
  tcc_state->registers_for_allocator = 13;
  tcc_state->registers_map_for_allocator = (1ull << 13) - 1;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 0;
}

/* -------------------------------------------------------------------------- */
/* Incoming parameters: first four in r0-r3, fifth on stack                   */
/* -------------------------------------------------------------------------- */

UT_TEST(test_aapcs_incoming_params)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p0 = tcc_ir_vreg_alloc_param(ir);
  int p1 = tcc_ir_vreg_alloc_param(ir);
  int p2 = tcc_ir_vreg_alloc_param(ir);
  int p3 = tcc_ir_vreg_alloc_param(ir);
  int p4 = tcc_ir_vreg_alloc_param(ir);

  /* Use every parameter so they stay live and get an interval. */
  int sum = tcc_ir_vreg_alloc_temp(ir);
  SValue s_sum = sv_var(sum, VT_INT);
  SValue s_p0 = sv_var(p0, VT_INT);
  SValue s_p1 = sv_var(p1, VT_INT);
  SValue s_p2 = sv_var(p2, VT_INT);
  SValue s_p3 = sv_var(p3, VT_INT);
  SValue s_p4 = sv_var(p4, VT_INT);

  tcc_ir_put(ir, TCCIR_OP_ADD, &s_p0, &s_p1, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_sum, &s_p2, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_sum, &s_p3, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_sum, &s_p4, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_sum, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  tcc_ir_codegen_params_setup(ir);

  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, p0), 0);
  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, p1), 1);
  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, p2), 2);
  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, p3), 3);

  IRLiveInterval *li4 = tcc_ir_vreg_live_interval(ir, p4);
  UT_ASSERT(li4 != NULL);
  UT_ASSERT_EQ(li4->incoming_reg0, -1);
  UT_ASSERT_EQ(li4->original_offset, 0); /* first stack argument */

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* 64-bit incoming parameter uses an even register pair                        */
/* -------------------------------------------------------------------------- */

UT_TEST(test_aapcs_64bit_param)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p0 = tcc_ir_vreg_alloc_param(ir);
  int p1 = tcc_ir_vreg_alloc_param(ir);

  tcc_ir_vreg_type_set_64bit(ir, p0);

  SValue s_p0 = sv_var(p0, VT_LLONG);
  (void)p1;

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_p0, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  tcc_ir_codegen_params_setup(ir);

  IRLiveInterval *li0 = tcc_ir_vreg_live_interval(ir, p0);
  UT_ASSERT(li0 != NULL);
  UT_ASSERT_EQ(li0->incoming_reg0, 0);
  UT_ASSERT_EQ(li0->incoming_reg1, 1);

  IRLiveInterval *li1 = tcc_ir_vreg_live_interval(ir, p1);
  UT_ASSERT(li1 != NULL);
  UT_ASSERT_EQ(li1->incoming_reg0, 2);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_register_allocation_params: already-set incoming regs (alignment    */
/* gap) branch -- ir/codegen.c ~209-224.                                      */
/* -------------------------------------------------------------------------- */

/* When a param's IRLiveInterval already carries incoming_reg0/1 (mimicking
 * upstream ABI-layout code having run first, per the ~209-211 comment),
 * tcc_ir_register_allocation_params() must not overwrite it -- it only
 * advances argno past the highest register the pre-set param actually used,
 * so a later plain param lands right after the gap. p0 is pre-set to arrive
 * in r2 alone (as if a split/skipped-register case left r1 unused), so p1
 * (an ordinary int with no incoming regs set) must land in r3, not r1. */
UT_TEST(test_aapcs_param_with_preset_incoming_regs_advances_argno_past_gap)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p0 = tcc_ir_vreg_alloc_param(ir);
  int p1 = tcc_ir_vreg_alloc_param(ir);

  IRLiveInterval *li0 = tcc_ir_vreg_live_interval(ir, p0);
  UT_ASSERT(li0 != NULL);
  li0->incoming_reg0 = 2;
  li0->incoming_reg1 = -1;

  tcc_ir_codegen_params_setup(ir);

  /* p0 untouched -- its pre-set incoming regs are respected verbatim. */
  UT_ASSERT_EQ(li0->incoming_reg0, 2);
  UT_ASSERT_EQ(li0->incoming_reg1, -1);

  /* argno advanced to highest(2, -1) + 1 == 3, so p1 gets r3, not r1. */
  IRLiveInterval *li1 = tcc_ir_vreg_live_interval(ir, p1);
  UT_ASSERT(li1 != NULL);
  UT_ASSERT_EQ(li1->incoming_reg0, 3);
  UT_ASSERT_EQ(li1->incoming_reg1, -1);

  tcc_ir_free(ir);
  return 0;
}

/* Same branch, but the pre-set param's incoming_reg1 is the higher of the
 * pair (the 64-bit-param shape from tcc_ir_add_function_parameters): argno
 * must advance past incoming_reg1, not incoming_reg0. */
UT_TEST(test_aapcs_param_with_preset_64bit_incoming_regs_advances_past_reg1)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p0 = tcc_ir_vreg_alloc_param(ir);
  int p1 = tcc_ir_vreg_alloc_param(ir);

  IRLiveInterval *li0 = tcc_ir_vreg_live_interval(ir, p0);
  UT_ASSERT(li0 != NULL);
  li0->incoming_reg0 = 0;
  li0->incoming_reg1 = 1; /* pair r0/r1, as a pre-set 64-bit param would be */

  tcc_ir_codegen_params_setup(ir);

  IRLiveInterval *li1 = tcc_ir_vreg_live_interval(ir, p1);
  UT_ASSERT(li1 != NULL);
  UT_ASSERT_EQ(li1->incoming_reg0, 2); /* argno advanced to 1+1 == 2, not 0+1 */

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_register_allocation_params: 64-bit param at odd argno must skip a   */
/* register to realign to an even pair -- ir/codegen.c ~227-230.              */
/* -------------------------------------------------------------------------- */

UT_TEST(test_aapcs_64bit_param_at_odd_argno_skips_to_even_pair)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p0 = tcc_ir_vreg_alloc_param(ir); /* plain int -> r0, argno becomes 1 */
  int p1 = tcc_ir_vreg_alloc_param(ir); /* 64-bit -> argno is odd (1), must skip to r2/r3 */

  tcc_ir_vreg_type_set_64bit(ir, p1);

  tcc_ir_codegen_params_setup(ir);

  IRLiveInterval *li0 = tcc_ir_vreg_live_interval(ir, p0);
  UT_ASSERT(li0 != NULL);
  UT_ASSERT_EQ(li0->incoming_reg0, 0);

  IRLiveInterval *li1 = tcc_ir_vreg_live_interval(ir, p1);
  UT_ASSERT(li1 != NULL);
  /* Without the alignment skip this would land on r1/r2 (straddling the odd
   * boundary); the skip forces r2/r3 instead. */
  UT_ASSERT_EQ(li1->incoming_reg0, 2);
  UT_ASSERT_EQ(li1->incoming_reg1, 3);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_register_allocation_params: 64-bit param spilled to the caller's    */
/* stack (argno > 2 after alignment) -- ir/codegen.c ~245-264.                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_aapcs_64bit_param_beyond_r2_spills_to_caller_stack)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p0 = tcc_ir_vreg_alloc_param(ir); /* int -> r0, argno=1 */
  int p1 = tcc_ir_vreg_alloc_param(ir); /* int -> r1, argno=2 */
  int p2 = tcc_ir_vreg_alloc_param(ir); /* int -> r2, argno=3 */
  int p3 = tcc_ir_vreg_alloc_param(ir); /* 64-bit: argno=3 is odd -> skip to 4, 4>2 -> stack */

  tcc_ir_vreg_type_set_64bit(ir, p3);

  /* Pre-poison the allocator fields the way the linear-scan allocator would
   * have left them for a register-resident guess, to prove the spilled-to-
   * stack branch actively resets them rather than merely leaving them. */
  IRLiveInterval *li3 = tcc_ir_vreg_live_interval(ir, p3);
  UT_ASSERT(li3 != NULL);
  li3->allocation.r0 = 4;
  li3->allocation.r1 = 5;
  li3->allocation.offset = 999;

  tcc_ir_codegen_params_setup(ir);

  /* p0/p1/p2 are plain register-passed params -- tcc_ir_register_allocation_params
   * deliberately does NOT touch interval->allocation for them (~240-243
   * comment: that field belongs to the linear-scan allocator, which never
   * ran in this test), only incoming_reg0/1. */
  IRLiveInterval *li0 = tcc_ir_vreg_live_interval(ir, p0);
  IRLiveInterval *li1 = tcc_ir_vreg_live_interval(ir, p1);
  IRLiveInterval *li2 = tcc_ir_vreg_live_interval(ir, p2);
  UT_ASSERT(li0 != NULL && li1 != NULL && li2 != NULL);
  UT_ASSERT_EQ(li0->incoming_reg0, 0);
  UT_ASSERT_EQ(li1->incoming_reg0, 1);
  UT_ASSERT_EQ(li2->incoming_reg0, 2);

  UT_ASSERT_EQ(li3->incoming_reg0, -1);
  UT_ASSERT_EQ(li3->incoming_reg1, -1);
  UT_ASSERT_EQ(li3->original_offset, 0); /* (argno=4 - 4) * 4 == 0: first stack word */
  UT_ASSERT_EQ(li3->allocation.r0, PREG_NONE);
  UT_ASSERT_EQ(li3->allocation.r1, PREG_NONE);
  UT_ASSERT_EQ(li3->allocation.offset, 0);

  tcc_ir_free(ir);
  return 0;
}

/* Same shape, but original_offset was already set (mirroring the "ABI-
 * derived offset is more accurate" comment at ~250-256) -- the spill branch
 * must NOT overwrite a non-zero pre-existing original_offset. */
UT_TEST(test_aapcs_64bit_param_stack_spill_preserves_existing_original_offset)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p0 = tcc_ir_vreg_alloc_param(ir);
  int p1 = tcc_ir_vreg_alloc_param(ir);
  int p2 = tcc_ir_vreg_alloc_param(ir);
  int p3 = tcc_ir_vreg_alloc_param(ir);

  tcc_ir_vreg_type_set_64bit(ir, p3);

  IRLiveInterval *li3 = tcc_ir_vreg_live_interval(ir, p3);
  UT_ASSERT(li3 != NULL);
  li3->original_offset = 16; /* ABI-derived offset, already computed upstream */

  tcc_ir_codegen_params_setup(ir);

  UT_ASSERT_EQ(li3->original_offset, 16); /* untouched, not recomputed to 0 */

  (void)p0;
  (void)p1;
  (void)p2;
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_register_allocation_params: plain (32-bit) param beyond r3 spills   */
/* to the caller's stack -- ir/codegen.c ~275-292 (the non-64-bit sibling of  */
/* the 64-bit spill path above).                                             */
/* -------------------------------------------------------------------------- */

UT_TEST(test_aapcs_int_param_beyond_r3_spills_to_caller_stack)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p[6];
  for (int i = 0; i < 6; i++)
    p[i] = tcc_ir_vreg_alloc_param(ir);

  IRLiveInterval *li5 = tcc_ir_vreg_live_interval(ir, p[5]);
  UT_ASSERT(li5 != NULL);
  li5->allocation.r0 = 6;
  li5->allocation.offset = 123;

  tcc_ir_codegen_params_setup(ir);

  /* p[0..3] are plain register-passed params -- incoming_reg0 is what this
   * function sets for them; interval->allocation is left to the linear-scan
   * allocator (not run in this test), so we assert on incoming_reg0 here
   * rather than tcc_ir_codegen_reg_get() (which reads allocation.r0). */
  IRLiveInterval *li0 = tcc_ir_vreg_live_interval(ir, p[0]);
  IRLiveInterval *li1 = tcc_ir_vreg_live_interval(ir, p[1]);
  IRLiveInterval *li2 = tcc_ir_vreg_live_interval(ir, p[2]);
  IRLiveInterval *li3 = tcc_ir_vreg_live_interval(ir, p[3]);
  UT_ASSERT(li0 != NULL && li1 != NULL && li2 != NULL && li3 != NULL);
  UT_ASSERT_EQ(li0->incoming_reg0, 0);
  UT_ASSERT_EQ(li1->incoming_reg0, 1);
  UT_ASSERT_EQ(li2->incoming_reg0, 2);
  UT_ASSERT_EQ(li3->incoming_reg0, 3);

  IRLiveInterval *li4 = tcc_ir_vreg_live_interval(ir, p[4]);
  UT_ASSERT(li4 != NULL);
  UT_ASSERT_EQ(li4->incoming_reg0, -1);
  UT_ASSERT_EQ(li4->original_offset, 0); /* (4-4)*4 == 0: first stack word */

  UT_ASSERT_EQ(li5->incoming_reg0, -1);
  UT_ASSERT_EQ(li5->original_offset, 4); /* (5-4)*4 == 4: second stack word */
  UT_ASSERT_EQ(li5->allocation.r0, PREG_NONE);
  UT_ASSERT_EQ(li5->allocation.offset, 0);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_mark_return_value_incoming_regs -- ir/codegen.c ~298-386.           */
/* Not part of tcc_ir_codegen_params_setup's wrapped call; invoked separately */
/* upstream (tccgen.c) after regalloc, so tested directly here.               */
/* -------------------------------------------------------------------------- */

/* A FUNCCALLVAL's dest vreg gets marked incoming_reg0=0 (r0) unconditionally;
 * a 64-bit (is_llong) dest additionally gets incoming_reg1=1 (r1). */
UT_TEST(test_mark_return_value_incoming_regs_marks_call_dest_r0_r1)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int arg = tcc_ir_vreg_alloc_temp(ir);
  int ret32 = tcc_ir_vreg_alloc_temp(ir);
  int ret64 = tcc_ir_vreg_alloc_temp(ir);

  SValue s_arg = sv_var(arg, VT_INT);
  SValue s_ret32 = sv_var(ret32, VT_INT);
  SValue s_ret64 = sv_var(ret64, VT_LLONG);
  SValue s_param0 = sv_param_marker(0, 0);
  SValue s_call0 = sv_call_id(0, 1);
  SValue s_param1 = sv_param_marker(1, 0);
  SValue s_call1 = sv_call_id(1, 1);

  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_arg);
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &s_param0, NULL);
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &s_param0, &s_call0, &s_ret32);
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &s_param1, NULL);
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &s_param1, &s_call1, &s_ret64);

  tcc_ir_vreg_type_set_64bit(ir, ret64);

  tcc_ir_mark_return_value_incoming_regs(ir);

  IRLiveInterval *li32 = tcc_ir_vreg_live_interval(ir, ret32);
  UT_ASSERT(li32 != NULL);
  UT_ASSERT_EQ(li32->incoming_reg0, 0);
  UT_ASSERT_EQ(li32->incoming_reg1, -1);

  IRLiveInterval *li64 = tcc_ir_vreg_live_interval(ir, ret64);
  UT_ASSERT(li64 != NULL);
  UT_ASSERT_EQ(li64->incoming_reg0, 0);
  UT_ASSERT_EQ(li64->incoming_reg1, 1);

  tcc_ir_free(ir);
  return 0;
}

/* At -O0 (tcc_state->optimize < 1), the RETURNVALUE-hint second half of the
 * function (~330-385) must not run at all -- a RETURNVALUE sourced from a
 * non-param, non-r0-hinted temp keeps incoming_reg0 == -1. */
UT_TEST(test_mark_return_value_incoming_regs_skips_hint_pass_below_o1)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();
  tcc_state->optimize = 0;

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  SValue s_a = sv_var(a, VT_INT);
  SValue s_b = sv_var(b, VT_INT);
  SValue s_five = sv_const(5);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_five, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_a, NULL, &s_b);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_b, NULL, NULL);

  tcc_ir_mark_return_value_incoming_regs(ir);

  IRLiveInterval *li_a = tcc_ir_vreg_live_interval(ir, a);
  UT_ASSERT(li_a != NULL);
  UT_ASSERT_EQ(li_a->incoming_reg0, -1); /* hint pass never ran */

  tcc_ir_free(ir);
  return 0;
}

/* At -O1+, RETURNVALUE walks back through a chain of ASSIGN defs (up to 5
 * hops) to find the root non-param vreg and hints incoming_reg0=0 on it --
 * ir/codegen.c ~333-385. Walk stops as soon as it reaches a PARAM vreg
 * (~349-350, "break" without marking further).
 *
 * root/mid/leaf must be VAR vregs, not TEMP: tcc_ir_put()'s ASSIGN-coalescing
 * (ir/core.c ~535-613) silently collapses `ASSIGN x -> t; ASSIGN t -> y` into
 * a single instruction with dest=y whenever the ASSIGN's src1 is a TEMP that
 * was the immediately-preceding instruction's dest -- so an all-TEMP chain
 * here would never actually reach the IR as 3 separate ASSIGNs (it collapses
 * at insertion time to a single `ASSIGN 7 -> leaf`, and the walk-back would
 * hint `leaf`, not `root`, defeating the point of this test). VAR vregs are
 * exempt from that coalescing check, so they force real, separate ASSIGN
 * instructions and let the multi-hop walk-back actually be exercised. */
UT_TEST(test_mark_return_value_incoming_regs_hints_root_of_assign_chain_at_o1)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();
  tcc_state->optimize = 1;

  int root = tcc_ir_vreg_alloc_var(ir);
  int mid = tcc_ir_vreg_alloc_var(ir);
  int leaf = tcc_ir_vreg_alloc_var(ir);
  SValue s_root = sv_var(root, VT_INT);
  SValue s_mid = sv_var(mid, VT_INT);
  SValue s_leaf = sv_var(leaf, VT_INT);
  SValue s_seven = sv_const(7);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_seven, NULL, &s_root);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_root, NULL, &s_mid);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_mid, NULL, &s_leaf);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_leaf, NULL, NULL);

  tcc_ir_mark_return_value_incoming_regs(ir);

  IRLiveInterval *li_root = tcc_ir_vreg_live_interval(ir, root);
  UT_ASSERT(li_root != NULL);
  UT_ASSERT_EQ(li_root->incoming_reg0, 0); /* walked mid <- leaf back to root */

  tcc_ir_free(ir);
  return 0;
}

/* If RETURNVALUE's own src1 is directly a PARAM vreg (vr starts the depth
 * loop already a param), the very first ~349-350 check breaks the loop
 * immediately -- vr stays the param, and the ~378-379 "!= PARAM" guard then
 * skips marking any hint at all. Confirms params are deliberately left for
 * tcc_ir_register_allocation_params to handle instead of getting a
 * conflicting hint from this pass. */
UT_TEST(test_mark_return_value_incoming_regs_returnvalue_direct_from_param_sets_no_hint)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();
  tcc_state->optimize = 1;

  int p0 = tcc_ir_vreg_alloc_param(ir);
  SValue s_p0 = sv_var(p0, VT_INT);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_p0, NULL, NULL);

  tcc_ir_mark_return_value_incoming_regs(ir);

  IRLiveInterval *li_p0 = tcc_ir_vreg_live_interval(ir, p0);
  UT_ASSERT(li_p0 != NULL);
  UT_ASSERT_EQ(li_p0->incoming_reg0, -1); /* untouched: fresh default, no hint set */

  tcc_ir_free(ir);
  return 0;
}

/* The walk-back reassignment itself refuses to step onto a PARAM def
 * (~365-366's `!= TCCIR_VREG_TYPE_PARAM` guard on the *candidate* src, not
 * just the current vr): when the only def of the RETURNVALUE's source is
 * `leaf = ASSIGN(p0)`, the walk cannot advance past `leaf` (its src1 is a
 * param), so `found` stays 0 and the loop stops with vr still == leaf --
 * leaf itself is what gets the incoming_reg0=0 hint, not p0. */
UT_TEST(test_mark_return_value_incoming_regs_chain_wont_step_onto_param_source)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();
  tcc_state->optimize = 1;

  int p0 = tcc_ir_vreg_alloc_param(ir);
  int leaf = tcc_ir_vreg_alloc_temp(ir);
  SValue s_p0 = sv_var(p0, VT_INT);
  SValue s_leaf = sv_var(leaf, VT_INT);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_p0, NULL, &s_leaf);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_leaf, NULL, NULL);

  tcc_ir_mark_return_value_incoming_regs(ir);

  IRLiveInterval *li_p0 = tcc_ir_vreg_live_interval(ir, p0);
  UT_ASSERT(li_p0 != NULL);
  UT_ASSERT_EQ(li_p0->incoming_reg0, -1); /* never reached by the walk */

  IRLiveInterval *li_leaf = tcc_ir_vreg_live_interval(ir, leaf);
  UT_ASSERT(li_leaf != NULL);
  UT_ASSERT_EQ(li_leaf->incoming_reg0, 0); /* the walk stops here and hints it instead */

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_avoid_spilling_stack_passed_params -- ir/codegen.c ~388-447.        */
/* Rewrites the linear-scan LSLiveInterval table (not the IRLiveInterval      */
/* table tcc_ir_register_allocation_params touches), so needs a real          */
/* tcc_ir_ssa_regalloc() pass to populate ir->ls.intervals[] first.           */
/* -------------------------------------------------------------------------- */

/* A 6th int parameter (stack-passed under AAPCS) that the linear-scan
 * allocator nonetheless assigned a register to (this stub harness's
 * allocator doesn't know about the caller-stack special case) gets forced
 * back to PREG_NONE/offset 0 by this pass so codegen doesn't emit a load
 * into a register the prolog never populates. */
UT_TEST(test_avoid_spilling_stack_passed_params_resets_stack_param_allocation)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p[6];
  SValue s_p[6];
  for (int i = 0; i < 6; i++)
  {
    p[i] = tcc_ir_vreg_alloc_param(ir);
    s_p[i] = sv_var(p[i], VT_INT);
  }

  int sum = tcc_ir_vreg_alloc_temp(ir);
  SValue s_sum = sv_var(sum, VT_INT);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_p[0], &s_p[1], &s_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_sum, &s_p[2], &s_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_sum, &s_p[3], &s_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_sum, &s_p[4], &s_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_sum, &s_p[5], &s_sum);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_sum, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  /* Find p[4]/p[5]'s LSLiveInterval entries (the stack-passed pair, argno
   * 4 and 5) and force them to look register-resident, simulating an
   * allocator that (incorrectly, absent this pass) gave them a register. */
  int found4 = 0, found5 = 0;
  for (int i = 0; i < ir->ls.next_interval_index; i++)
  {
    LSLiveInterval *lsi = &ir->ls.intervals[i];
    if (TCCIR_DECODE_VREG_TYPE((int)lsi->vreg) != TCCIR_VREG_TYPE_PARAM)
      continue;
    int pidx = TCCIR_DECODE_VREG_POSITION((int)lsi->vreg);
    if (pidx == 4)
    {
      lsi->r0 = 8;
      lsi->stack_location = 0;
      found4 = 1;
    }
    else if (pidx == 5)
    {
      lsi->r0 = 9;
      lsi->stack_location = 0;
      found5 = 1;
    }
  }
  UT_ASSERT(found4 && found5);

  tcc_ir_avoid_spilling_stack_passed_params(ir);

  found4 = found5 = 0;
  for (int i = 0; i < ir->ls.next_interval_index; i++)
  {
    LSLiveInterval *lsi = &ir->ls.intervals[i];
    if (TCCIR_DECODE_VREG_TYPE((int)lsi->vreg) != TCCIR_VREG_TYPE_PARAM)
      continue;
    int pidx = TCCIR_DECODE_VREG_POSITION((int)lsi->vreg);
    if (pidx == 4)
    {
      UT_ASSERT_EQ(lsi->r0, PREG_NONE);
      UT_ASSERT_EQ(lsi->r1, PREG_NONE);
      UT_ASSERT_EQ((int)lsi->stack_location, 0);
      found4 = 1;
    }
    else if (pidx == 5)
    {
      UT_ASSERT_EQ(lsi->r0, PREG_NONE);
      UT_ASSERT_EQ(lsi->r1, PREG_NONE);
      UT_ASSERT_EQ((int)lsi->stack_location, 0);
      found5 = 1;
    }
  }
  UT_ASSERT(found4 && found5);

  tcc_ir_free(ir);
  return 0;
}

/* Register-passed params (r0-r3) are left completely untouched -- the
 * function's early-continue on !is_stack_passed[pidx] (~434-435). */
UT_TEST(test_avoid_spilling_stack_passed_params_leaves_register_params_alone)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p0 = tcc_ir_vreg_alloc_param(ir);
  int p1 = tcc_ir_vreg_alloc_param(ir);
  SValue s_p0 = sv_var(p0, VT_INT);
  SValue s_p1 = sv_var(p1, VT_INT);

  int sum = tcc_ir_vreg_alloc_temp(ir);
  SValue s_sum = sv_var(sum, VT_INT);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_p0, &s_p1, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_sum, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  /* Snapshot p0/p1's LSLiveInterval r0 before the pass. Sentinel -999 can't
   * collide with a real register number (0-15), PREG_NONE (0x1F), or the
   * pre-allocation "-1" placeholder some intervals legitimately carry. */
  enum
  {
    NOT_FOUND = -999
  };
  int r0_before = NOT_FOUND, r1_before = NOT_FOUND;
  int found0 = 0, found1 = 0;
  for (int i = 0; i < ir->ls.next_interval_index; i++)
  {
    LSLiveInterval *lsi = &ir->ls.intervals[i];
    if (TCCIR_DECODE_VREG_TYPE((int)lsi->vreg) != TCCIR_VREG_TYPE_PARAM)
      continue;
    int pidx = TCCIR_DECODE_VREG_POSITION((int)lsi->vreg);
    if (pidx == 0)
    {
      r0_before = lsi->r0;
      found0 = 1;
    }
    else if (pidx == 1)
    {
      r1_before = lsi->r0;
      found1 = 1;
    }
  }
  UT_ASSERT(found0 && found1);

  tcc_ir_avoid_spilling_stack_passed_params(ir);

  int r0_after = NOT_FOUND, r1_after = NOT_FOUND;
  found0 = found1 = 0;
  for (int i = 0; i < ir->ls.next_interval_index; i++)
  {
    LSLiveInterval *lsi = &ir->ls.intervals[i];
    if (TCCIR_DECODE_VREG_TYPE((int)lsi->vreg) != TCCIR_VREG_TYPE_PARAM)
      continue;
    int pidx = TCCIR_DECODE_VREG_POSITION((int)lsi->vreg);
    if (pidx == 0)
    {
      r0_after = lsi->r0;
      found0 = 1;
    }
    else if (pidx == 1)
    {
      r1_after = lsi->r0;
      found1 = 1;
    }
  }
  UT_ASSERT(found0 && found1);
  UT_ASSERT_EQ(r0_after, r0_before);
  UT_ASSERT_EQ(r1_after, r1_before);

  tcc_ir_free(ir);
  return 0;
}

/* param_count <= 0 (no parameters at all) is an early return (~397-399) --
 * must not crash or allocate the is_stack_passed scratch array. */
UT_TEST(test_avoid_spilling_stack_passed_params_noop_with_no_params)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  SValue s_a = sv_var(a, VT_INT);
  SValue s_const = sv_const(42);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_const, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_a, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  UT_ASSERT_EQ(ir->next_parameter, 0);
  tcc_ir_avoid_spilling_stack_passed_params(ir); /* must not crash */

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_fill_registers -- ir/codegen.c ~21-186. Rewrites a legacy SValue's  */
/* pr0/pr1/.r/.c.i fields from the vreg's post-allocation IRLiveInterval.     */
/* Entirely untested before this section (its only real caller is inline-asm */
/* operand filling, which this harness can't reach) but self-contained       */
/* enough to call directly: no dispatch loop, no mop stubs, no regalloc pass */
/* needed -- a hand-built SValue plus a directly-mutated IRLiveInterval is    */
/* the whole input surface.                                                  */
/* -------------------------------------------------------------------------- */

/* vr == -1 with old .r == VT_LOCAL is a concrete stack slot (e.g. a VLA save
 * slot) -- must not be rewritten into a register; early-return path
 * (~36-43) just clears pr0/pr1 and leaves .r/.c.i untouched. */
UT_TEST(test_fill_registers_concrete_local_slot_not_rewritten)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue sv = sv_raw(VT_LOCAL | VT_LVAL, -1);
  sv.pr0_reg = 3; /* pre-poison to confirm the early-return path resets these */
  sv.pr0_spilled = 1;
  sv.c.i = 0x1234;

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.pr0_reg, PREG_REG_NONE);
  UT_ASSERT_EQ(sv.pr0_spilled, 0);
  UT_ASSERT_EQ(sv.pr1_reg, PREG_REG_NONE);
  UT_ASSERT_EQ(sv.pr1_spilled, 0);
  UT_ASSERT_EQ(sv.r, VT_LOCAL | VT_LVAL); /* untouched */
  UT_ASSERT_EQ((int)sv.c.i, 0x1234);      /* untouched */

  tcc_ir_free(ir);
  return 0;
}

/* Same VT_LOCAL shape, but vr != -1 (a logical local tracked by the IR) must
 * NOT take the early-return path -- it falls through into the valid-vreg
 * branch below instead. Register-resident (not spilled) local: r0 is
 * written as a plain register number. */
UT_TEST(test_fill_registers_local_with_vreg_falls_through_to_register_path)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int t = tcc_ir_vreg_alloc_temp(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, t);
  UT_ASSERT(li != NULL);
  li->allocation.r0 = 5;
  li->allocation.r1 = PREG_NONE;
  li->allocation.offset = 0;

  SValue sv = sv_raw(VT_LOCAL | VT_LVAL, t);

  tcc_ir_fill_registers(ir, &sv);

  /* old_v == VT_LOCAL: interval->allocation.r0 != PREG_NONE and not spilled
   * -> sv->r = allocation.r0 | preserve_flags. old_r has VT_LVAL but old_v
   * == VT_LOCAL, so the "(old_r & VT_LVAL) && old_v < VT_CONST..." guard
   * (~99) excludes VT_LOCAL, so preserve_flags carries no VT_LVAL. */
  UT_ASSERT_EQ(sv.r, 5);

  tcc_ir_free(ir);
  return 0;
}

/* Stack-passed PARAM (incoming_reg0 < 0, never allocated a register, no
 * spill offset): rewritten to VT_LOCAL|VT_PARAM with c.i set to
 * original_offset -- the "resides in the incoming argument area" path
 * (~56-71). old_v is VT_CONST-class (not a pointer-deref shape), so
 * need_lval stays whatever old_r's VT_LVAL bit was (here: unset). */
UT_TEST(test_fill_registers_stack_passed_param_becomes_vt_local_param)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p = tcc_ir_vreg_alloc_param(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, p);
  UT_ASSERT(li != NULL);
  UT_ASSERT_EQ(li->incoming_reg0, -1);      /* fresh interval default */
  UT_ASSERT_EQ(li->allocation.r0, PREG_NONE); /* fresh interval default */
  UT_ASSERT_EQ(li->allocation.offset, 0);
  li->original_offset = 24;

  SValue sv = sv_raw(0, p); /* old_r = 0: no VT_LVAL, old_v = 0 (< VT_CONST) */

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.pr0_reg, PREG_REG_NONE);
  UT_ASSERT_EQ(sv.pr0_spilled, 0);
  UT_ASSERT_EQ((int)sv.c.i, 24);
  UT_ASSERT_EQ(sv.r, VT_LOCAL | VT_PARAM); /* no VT_LVAL: old_r had none, is_lvalue not set */

  tcc_ir_free(ir);
  return 0;
}

/* Same stack-passed-param path, but interval->is_lvalue is set and old_v is
 * a plain computed-value shape (< VT_CONST, not LOCAL/LLOCAL) -- the
 * ~66-67 sub-branch forces need_lval = VT_LVAL even though old_r itself
 * didn't carry VT_LVAL. */
UT_TEST(test_fill_registers_stack_passed_param_is_lvalue_forces_lval)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p = tcc_ir_vreg_alloc_param(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, p);
  UT_ASSERT(li != NULL);
  li->original_offset = 8;
  li->is_lvalue = 1;

  SValue sv = sv_raw(0, p);

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, VT_LOCAL | VT_LVAL | VT_PARAM);

  tcc_ir_free(ir);
  return 0;
}

/* Register-passed param (incoming_reg0 >= 0), allocated a register, old_r
 * carried VT_LVAL (as if from '&param' handling upstream) -- is_register_param
 * excludes it from the pointer-deref preserve_flags computation (~99, the
 * `!is_register_param` guard), so VT_LVAL is dropped even though old_v was a
 * plain (< VT_CONST) value. */
UT_TEST(test_fill_registers_register_passed_param_drops_lval)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p = tcc_ir_vreg_alloc_param(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, p);
  UT_ASSERT(li != NULL);
  li->incoming_reg0 = 0;
  li->allocation.r0 = 0;
  li->allocation.offset = 0;

  SValue sv = sv_raw(VT_LVAL, p); /* old_v = 0 (< VT_CONST), old_r has VT_LVAL */

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, 0); /* register 0, no VT_LVAL preserved */

  tcc_ir_free(ir);
  return 0;
}

/* Non-param vreg (a plain computed temp) with old_r carrying VT_LVAL and
 * old_v < VT_CONST: this IS the pointer-deref shape (~91-92,99) --
 * preserve_flags picks up VT_LVAL, and since the interval is register-
 * resident (not spilled), sv->r = allocation.r0 | VT_LVAL (~166-169). */
UT_TEST(test_fill_registers_temp_pointer_deref_preserves_lval_in_register)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int t = tcc_ir_vreg_alloc_temp(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, t);
  UT_ASSERT(li != NULL);
  li->allocation.r0 = 6;
  li->allocation.offset = 0;

  SValue sv = sv_raw(VT_LVAL, t); /* old_v = 0, has VT_LVAL: pointer needing deref */

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, 6 | VT_LVAL);

  tcc_ir_free(ir);
  return 0;
}

/* Spilled computed value (old_v was a plain register-class value, not
 * LOCAL/LLOCAL): the spilled branch (~110-165) always sets need_lval =
 * VT_LVAL ("COMPUTED VALUE CASE", ~131-142) and base_kind stays VT_LOCAL
 * (old_r has no VT_LVAL, so the LLOCAL double-indirection branch at ~145
 * doesn't trigger). */
UT_TEST(test_fill_registers_spilled_computed_value_gets_vt_local_lval)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int t = tcc_ir_vreg_alloc_temp(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, t);
  UT_ASSERT(li != NULL);
  li->allocation.r0 = PREG_NONE;
  li->allocation.offset = -12; /* spilled: offset != 0 */

  SValue sv = sv_raw(0, t); /* old_r = 0: no VT_LVAL, old_v = 0 (computed value) */

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, VT_LOCAL | VT_LVAL);
  UT_ASSERT_EQ((int)sv.c.i, -12);

  tcc_ir_free(ir);
  return 0;
}

/* Spilled value where old_r carried VT_LVAL and old_v was NOT LOCAL/LLOCAL:
 * double-indirection case (~117-122,145-153) -- base_kind becomes VT_LLOCAL
 * instead of VT_LOCAL (pointer-in-spill-slot needs load-then-deref). */
UT_TEST(test_fill_registers_spilled_pointer_deref_uses_vt_llocal)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int t = tcc_ir_vreg_alloc_temp(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, t);
  UT_ASSERT(li != NULL);
  li->allocation.r0 = PREG_NONE;
  li->allocation.offset = -8;

  SValue sv = sv_raw(VT_LVAL, t); /* old_v = 0, has VT_LVAL: pointer deref, spilled */

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, VT_LLOCAL | VT_LVAL);
  UT_ASSERT_EQ((int)sv.c.i, -8);

  tcc_ir_free(ir);
  return 0;
}

/* Spilled VT_LOCAL (a spilled local variable, address-of shape: old_r ==
 * VT_LOCAL without VT_LVAL) -- need_lval preserves old_r's (unset) VT_LVAL
 * bit exactly (~134-138, the "Local variable" sub-branch), giving a plain
 * VT_LOCAL with no VT_LVAL (address-of the spill slot, not its contents). */
UT_TEST(test_fill_registers_spilled_local_address_of_has_no_lval)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int t = tcc_ir_vreg_alloc_temp(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, t);
  UT_ASSERT(li != NULL);
  li->allocation.r0 = PREG_NONE;
  li->allocation.offset = -4;

  SValue sv = sv_raw(VT_LOCAL, t); /* old_v == VT_LOCAL, no VT_LVAL: address-of */

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, VT_LOCAL); /* no VT_LVAL added */
  UT_ASSERT_EQ((int)sv.c.i, -4);

  tcc_ir_free(ir);
  return 0;
}

/* Spilled register-passed param (incoming_reg0 >= 0 but allocation.offset !=
 * 0, i.e. the allocator spilled it to the callee's local stack): the
 * spilled_param_flag sub-branch (~154-163) must NOT set VT_PARAM, since
 * VT_PARAM on a spilled register param would wrongly add offset_to_args. */
UT_TEST(test_fill_registers_spilled_register_param_drops_vt_param_flag)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p = tcc_ir_vreg_alloc_param(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, p);
  UT_ASSERT(li != NULL);
  li->incoming_reg0 = 0; /* arrived in r0, but the allocator then spilled it */
  li->allocation.r0 = PREG_NONE;
  li->allocation.offset = -16;

  SValue sv = sv_raw(VT_PARAM, p); /* old_r carries VT_PARAM */

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, VT_LOCAL | VT_LVAL); /* no VT_PARAM: incoming_reg0 >= 0 */
  UT_ASSERT_EQ((int)sv.c.i, -16);

  tcc_ir_free(ir);
  return 0;
}

/* Spilled STACK-passed param (incoming_reg0 < 0, but allocation.offset != 0
 * so it doesn't take the ~56-71 early path -- e.g. address-taken forcing a
 * real spill slot distinct from the incoming stack home): spilled_param_flag
 * DOES get set (~160-163), preserving VT_PARAM. */
UT_TEST(test_fill_registers_spilled_stack_param_keeps_vt_param_flag)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p = tcc_ir_vreg_alloc_param(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, p);
  UT_ASSERT(li != NULL);
  /* incoming_reg0 stays -1 (fresh default), but give it a nonzero spill
   * offset so the ~56-57 "not allocated, offset==0" early-path guard fails
   * and this falls through into the general spilled branch instead. */
  li->allocation.r0 = PREG_NONE;
  li->allocation.offset = -20;

  SValue sv = sv_raw(VT_PARAM, p);

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, VT_LOCAL | VT_LVAL | VT_PARAM);
  UT_ASSERT_EQ((int)sv.c.i, -20);

  tcc_ir_free(ir);
  return 0;
}

/* Valid vreg, but allocation.r0 == PREG_NONE and offset == 0 (never
 * allocated at all, e.g. a dead/unused temp) -- neither the spilled branch
 * (~110) nor the register branch (~166) triggers, so sv->r is left
 * completely untouched from whatever it was before the call. */
UT_TEST(test_fill_registers_unallocated_temp_leaves_r_untouched)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int t = tcc_ir_vreg_alloc_temp(ir);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, t);
  UT_ASSERT(li != NULL);
  UT_ASSERT_EQ(li->allocation.r0, PREG_NONE); /* fresh interval default */
  UT_ASSERT_EQ(li->allocation.offset, 0);

  SValue sv = sv_raw(0x7777, t);

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, 0x7777); /* untouched -- neither branch matched */
  /* pr0_reg/pr1_reg are still written from the (PREG_NONE) allocation,
   * unconditionally, before the branch that decides sv->r. */
  UT_ASSERT_EQ(sv.pr0_reg, PREG_REG_NONE);

  tcc_ir_free(ir);
  return 0;
}

/* Invalid vreg (sv->vr == -1), old_v already >= VT_CONST (a global symbol
 * reference, VT_CONST|VT_SYM): the constant/symbol fallback (~172-178)
 * rewrites to VT_CONST, preserving only the VT_LVAL/VT_SYM bits out of
 * old_r's full flag set (~177: `sv->r & (VT_LVAL | VT_SYM)`). */
UT_TEST(test_fill_registers_invalid_vreg_constant_becomes_vt_const)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue sv = sv_raw(VT_CONST | VT_SYM, -1); /* old_v = VT_CONST (>= VT_CONST) */

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, VT_CONST | VT_SYM); /* VT_SYM preserved, rebuilt as VT_CONST|VT_SYM */

  tcc_ir_free(ir);
  return 0;
}

/* Invalid vreg with old_r == PREG_REG_NONE (sentinel "no register"): also
 * takes the constant fallback path via the `sv->r == PREG_REG_NONE` disjunct
 * (~173), independent of old_v. */
UT_TEST(test_fill_registers_invalid_vreg_preg_none_becomes_vt_const)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue sv = sv_raw(PREG_REG_NONE, -1);

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, VT_CONST);

  tcc_ir_free(ir);
  return 0;
}

/* Invalid vreg, old_r == 0, but sv->sym is set: the function-symbol special
 * case (~180-185) -- rewritten to VT_CONST|VT_SYM even though old_r==0
 * doesn't match either of the first two branches' triggers on its own. */
UT_TEST(test_fill_registers_invalid_vreg_zero_r_with_sym_becomes_const_sym)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue sv = sv_raw(0, -1);
  /* sv.vr == -1, old_r == 0 -> old_v == 0 (< VT_CONST), so the first
   * fallback's "(sv->r == -1 || sv->r == PREG_REG_NONE || old_v >=
   * VT_CONST)" disjunct is false with old_r == 0 -- must fall through to
   * the sym-specific branch instead. */
  static Sym dummy_sym;
  sv.sym = &dummy_sym;

  tcc_ir_fill_registers(ir, &sv);

  UT_ASSERT_EQ(sv.r, VT_CONST | VT_SYM);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Outgoing call operand layout                                                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_outgoing_call_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int arg = tcc_ir_vreg_alloc_temp(ir);
  int ret = tcc_ir_vreg_alloc_temp(ir);

  SValue s_arg = sv_var(arg, VT_INT);
  SValue s_ret = sv_var(ret, VT_INT);
  SValue s_param = sv_param_marker(0, 0);
  SValue s_call = sv_call_id(0, 1);

  SValue s_const123 = sv_const(123);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_const123, NULL, &s_arg);
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &s_param, NULL);
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &s_param, &s_call, &s_ret);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_ret, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  int call_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_FUNCCALLVAL)
    {
      call_idx = i;
      break;
    }
  }
  UT_ASSERT(call_idx >= 0);

  IRQuadCompact *q = &ir->compact_instructions[call_idx];
  IROperand d = tcc_ir_codegen_dest_get(ir, q);
  IROperand s1 = tcc_ir_codegen_src1_get(ir, q);
  MachineOperand mret = machine_op_from_ir(ir, &d);
  MachineOperand mparam = machine_op_from_ir(ir, &s1);

  /* Return value arrives in r0; the marker operand is an immediate constant. */
  UT_ASSERT(mret.kind == MACH_OP_REG || mret.kind == MACH_OP_SPILL);
  UT_ASSERT(mparam.kind == MACH_OP_IMM);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* drop_return kills an unused FUNCCALLVAL return value                        */
/* -------------------------------------------------------------------------- */

UT_TEST(test_drop_return)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int arg = tcc_ir_vreg_alloc_temp(ir);
  int ret = tcc_ir_vreg_alloc_temp(ir);

  SValue s_arg = sv_var(arg, VT_INT);
  SValue s_ret = sv_var(ret, VT_INT);
  SValue s_param = sv_param_marker(0, 0);
  SValue s_call = sv_call_id(0, 1);

  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_arg);
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &s_param, NULL);
  int call_idx = tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &s_param, &s_call, &s_ret);

  tcc_ir_codegen_drop_return(ir);

  IRQuadCompact *q = &ir->compact_instructions[call_idx];
  IROperand dst = tcc_ir_codegen_dest_get(ir, q);
  UT_ASSERT(!irop_has_vreg(dst));

  tcc_ir_free(ir);
  return 0;
}

/* NULL guard (ir/codegen.c ~801-804) -- must not crash. */
UT_TEST(test_drop_return_null_ir_is_noop)
{
  tcc_ir_codegen_drop_return(NULL); /* must not crash */
  return 0;
}

/* Empty-function guard (ir/codegen.c ~806-809): next_instruction_index == 0
 * -- must not crash indexing compact_instructions[-1]. */
UT_TEST(test_drop_return_empty_function_is_noop)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  UT_ASSERT_EQ(ir->next_instruction_index, 0);
  tcc_ir_codegen_drop_return(ir); /* must not crash */

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * Dispatch-level tests (tcc_ir_codegen_generate)
 *
 * See test_codegen_arith.c's dispatch-level section header for the overall
 * rationale. FUNCPARAMVAL/FUNCPARAMVOID share one case label (~4009);
 * FUNCCALLVOID/FUNCCALLVAL share another (~4096); RETURNVALUE is separate
 * (~3812). func_call_mop's drop_value arg is `cq->op == TCCIR_OP_FUNCCALLVOID`
 * (ir/codegen.c ~4099).
 * ============================================================================ */

UT_TEST(test_dispatch_call_routes_funcparam_and_funccall_mops)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int arg = tcc_ir_vreg_alloc_temp(ir);
  int ret = tcc_ir_vreg_alloc_temp(ir);

  SValue s_arg = sv_var(arg, VT_INT);
  SValue s_ret = sv_var(ret, VT_INT);
  SValue s_param = sv_param_marker(0, 0);
  SValue s_call = sv_call_id(0, 1);

  SValue s_const123 = sv_const(123);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_const123, NULL, &s_arg);
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &s_param, NULL);
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &s_param, &s_call, &s_ret);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_ret, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 0; /* has a call, so not a leaf function */
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("func_parameter_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("func_call_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("return_value_mop"), 1);

  const CgStubCall *pc = cgstub_nth_call("func_parameter_mop", 0);
  UT_ASSERT(pc != NULL);
  UT_ASSERT_EQ(pc->ir_op, TCCIR_OP_FUNCPARAMVAL);

  const CgStubCall *cc = cgstub_nth_call("func_call_mop", 0);
  UT_ASSERT(cc != NULL);
  UT_ASSERT_EQ(cc->aux0, 0); /* drop_value: return value is used (s_ret feeds RETURNVALUE) */
  /* call_idx is codegen.c's dispatch-loop instruction index `i` at the
   * FUNCCALLVAL, not a sequential call counter -- ASSIGN(0), FUNCPARAMVAL(1),
   * FUNCCALLVAL(2), RETURNVALUE(3). */
  UT_ASSERT_EQ(cc->aux1, 2);
  UT_ASSERT_EQ(cc->dest_kind, MACH_OP_REG); /* r0 return value, live into RETURNVALUE */

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_funccallvoid_drops_return_value)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int arg = tcc_ir_vreg_alloc_temp(ir);
  SValue s_arg = sv_var(arg, VT_INT);
  SValue s_param = sv_param_marker(0, 0);
  SValue s_call = sv_call_id(0, 1);

  SValue s_const7 = sv_const(7);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_const7, NULL, &s_arg);
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &s_param, NULL);
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVOID, &s_param, &s_call, NULL);
  /* No explicit RETURNVALUE -- void function, falls through to the epilogue. */

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 0;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("func_call_mop"), 1);
  const CgStubCall *cc = cgstub_nth_call("func_call_mop", 0);
  UT_ASSERT(cc != NULL);
  UT_ASSERT_EQ(cc->aux0, 1); /* drop_value: FUNCCALLVOID always drops */

  tcc_ir_free(ir);
  return 0;
}

/* A 5th argument forces a stack-passed parameter under the stub's minimal
 * AAPCS-shaped thumb_build_call_layout_from_ir() (first 4 in R0-R3, rest on
 * stack -- see codegen_mop_stubs.c). Asserts func_parameter_mop fires once
 * per argument, in order; doesn't assert on exact stack offsets since that's
 * the stub's own (documented, minimal) classification, not codegen.c logic. */
UT_TEST(test_dispatch_call_with_five_args_calls_func_parameter_mop_per_arg)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  enum
  {
    NARGS = 5
  };
  int argv[NARGS];
  SValue s_argv[NARGS];
  SValue s_param[NARGS];
  for (int i = 0; i < NARGS; i++)
  {
    argv[i] = tcc_ir_vreg_alloc_temp(ir);
    s_argv[i] = sv_var(argv[i], VT_INT);
    SValue s_imm = sv_const(i + 1);
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_imm, NULL, &s_argv[i]);
    s_param[i] = sv_param_marker(0, i);
    tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_argv[i], &s_param[i], NULL);
  }

  int ret = tcc_ir_vreg_alloc_temp(ir);
  SValue s_ret = sv_var(ret, VT_INT);
  SValue s_call = sv_call_id(0, NARGS);
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &s_param[0], &s_call, &s_ret);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_ret, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 0;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("func_parameter_mop"), NARGS);
  UT_ASSERT_EQ(cgstub_call_count("func_call_mop"), 1);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(codegen_call)
{
  UT_RUN(test_aapcs_incoming_params);
  UT_RUN(test_aapcs_64bit_param);
  UT_RUN(test_aapcs_param_with_preset_incoming_regs_advances_argno_past_gap);
  UT_RUN(test_aapcs_param_with_preset_64bit_incoming_regs_advances_past_reg1);
  UT_RUN(test_aapcs_64bit_param_at_odd_argno_skips_to_even_pair);
  UT_RUN(test_aapcs_64bit_param_beyond_r2_spills_to_caller_stack);
  UT_RUN(test_aapcs_64bit_param_stack_spill_preserves_existing_original_offset);
  UT_RUN(test_aapcs_int_param_beyond_r3_spills_to_caller_stack);
  UT_RUN(test_mark_return_value_incoming_regs_marks_call_dest_r0_r1);
  UT_RUN(test_mark_return_value_incoming_regs_skips_hint_pass_below_o1);
  UT_RUN(test_mark_return_value_incoming_regs_hints_root_of_assign_chain_at_o1);
  UT_RUN(test_mark_return_value_incoming_regs_returnvalue_direct_from_param_sets_no_hint);
  UT_RUN(test_mark_return_value_incoming_regs_chain_wont_step_onto_param_source);
  UT_RUN(test_avoid_spilling_stack_passed_params_resets_stack_param_allocation);
  UT_RUN(test_avoid_spilling_stack_passed_params_leaves_register_params_alone);
  UT_RUN(test_avoid_spilling_stack_passed_params_noop_with_no_params);
  UT_RUN(test_fill_registers_concrete_local_slot_not_rewritten);
  UT_RUN(test_fill_registers_local_with_vreg_falls_through_to_register_path);
  UT_RUN(test_fill_registers_stack_passed_param_becomes_vt_local_param);
  UT_RUN(test_fill_registers_stack_passed_param_is_lvalue_forces_lval);
  UT_RUN(test_fill_registers_register_passed_param_drops_lval);
  UT_RUN(test_fill_registers_temp_pointer_deref_preserves_lval_in_register);
  UT_RUN(test_fill_registers_spilled_computed_value_gets_vt_local_lval);
  UT_RUN(test_fill_registers_spilled_pointer_deref_uses_vt_llocal);
  UT_RUN(test_fill_registers_spilled_local_address_of_has_no_lval);
  UT_RUN(test_fill_registers_spilled_register_param_drops_vt_param_flag);
  UT_RUN(test_fill_registers_spilled_stack_param_keeps_vt_param_flag);
  UT_RUN(test_fill_registers_unallocated_temp_leaves_r_untouched);
  UT_RUN(test_fill_registers_invalid_vreg_constant_becomes_vt_const);
  UT_RUN(test_fill_registers_invalid_vreg_preg_none_becomes_vt_const);
  UT_RUN(test_fill_registers_invalid_vreg_zero_r_with_sym_becomes_const_sym);
  UT_RUN(test_outgoing_call_operands);
  UT_RUN(test_drop_return);
  UT_RUN(test_drop_return_null_ir_is_noop);
  UT_RUN(test_drop_return_empty_function_is_noop);
  UT_RUN(test_dispatch_call_routes_funcparam_and_funccall_mops);
  UT_RUN(test_dispatch_funccallvoid_drops_return_value);
  UT_RUN(test_dispatch_call_with_five_args_calls_func_parameter_mop_per_arg);
}
