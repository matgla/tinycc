/*
 *  test_codegen_arith.c - backend unit tests for integer arithmetic IR ops
 *
 *  Covers the IR->machine-operand lowering path for arithmetic operations
 *  (ADD/SUB/MUL/DIV/IMOD and bitwise/shifts) plus the codegen helper
 *  accessors in ir/codegen.c (dest/src getters/setters and register queries).
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ir/ssa.h"
#include "ir/vreg.h"
#include "ir/regalloc.h"
#include "ir/codegen.h"
#include "ir/machine_op.h"
#include "source/backend/arch/arm/arm_regalloc.h"
#include "codegen_mop_stubs.h"
#include "ut.h"

/* JUMPIF condition tokens (see evaluate_compare_condition in opt_utils.c). */
#define TOK_EQ 0x94
#define TOK_NE 0x95

static SValue sv_var(int vreg)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = VT_INT;
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

static void setup_tcc_state(void)
{
  tcc_state->registers_for_allocator = 13;
  tcc_state->registers_map_for_allocator = (1ull << 13) - 1;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 0;
}

static TCCIRState *build_arith(TccIrOp op, int lhs, int rhs)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);

  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_c = sv_var(c);
  SValue s_lhs = sv_const(lhs);
  SValue s_rhs = sv_const(rhs);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_lhs, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_rhs, NULL, &s_b);
  tcc_ir_put(ir, op, &s_a, &s_b, &s_c);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_c, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  return ir;
}

/* -------------------------------------------------------------------------- */
/* Codegen operand accessors                                                  */
/* -------------------------------------------------------------------------- */

UT_TEST(test_codegen_arith_accessors)
{
  TCCIRState *ir = build_arith(TCCIR_OP_ADD, 5, 3);

  int add_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_ADD)
    {
      add_idx = i;
      break;
    }
  }
  UT_ASSERT(add_idx >= 0);

  IRQuadCompact *q = &ir->compact_instructions[add_idx];

  /* tcc_ir_codegen_* accessors must agree with tcc_ir_op_get_*. */
  IROperand dest = tcc_ir_codegen_dest_get(ir, q);
  IROperand src1 = tcc_ir_codegen_src1_get(ir, q);
  IROperand src2 = tcc_ir_codegen_src2_get(ir, q);

  UT_ASSERT(irop_has_vreg(dest));
  UT_ASSERT(irop_has_vreg(src1));
  UT_ASSERT(irop_has_vreg(src2));

  IROperand expected_dest = tcc_ir_op_get_dest(ir, q);
  IROperand expected_src1 = tcc_ir_op_get_src1(ir, q);
  IROperand expected_src2 = tcc_ir_op_get_src2(ir, q);

  UT_ASSERT_EQ(dest.vr, expected_dest.vr);
  UT_ASSERT_EQ(src1.vr, expected_src1.vr);
  UT_ASSERT_EQ(src2.vr, expected_src2.vr);

  /* dest-set round-trip */
  IROperand saved = dest;
  tcc_ir_codegen_dest_set(ir, q, IROP_NONE);
  UT_ASSERT(irop_is_none(tcc_ir_codegen_dest_get(ir, q)));
  tcc_ir_codegen_dest_set(ir, q, saved);
  UT_ASSERT_EQ(tcc_ir_codegen_dest_get(ir, q).vr, saved.vr);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Machine-operand lowering for immediate vs register operands                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_arith_immediate_and_register_operands)
{
  TCCIRState *ir = build_arith(TCCIR_OP_ADD, 5, 3);

  int add_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_ADD)
    {
      add_idx = i;
      break;
    }
  }
  UT_ASSERT(add_idx >= 0);

  IRQuadCompact *q = &ir->compact_instructions[add_idx];
  IROperand src1 = tcc_ir_codegen_src1_get(ir, q);
  IROperand src2 = tcc_ir_codegen_src2_get(ir, q);
  IROperand dest = tcc_ir_codegen_dest_get(ir, q);

  /* ADD src1 and src2 are register-resident temporaries (constants were folded
   * into ASSIGNs).  Verify machine_op_from_ir reflects the allocation. */
  MachineOperand m1 = machine_op_from_ir(ir, &src1);
  MachineOperand m2 = machine_op_from_ir(ir, &src2);
  MachineOperand md = machine_op_from_ir(ir, &dest);

  UT_ASSERT_EQ(md.kind, MACH_OP_REG);
  UT_ASSERT(md.u.reg.r0 < PREG_NONE);

  UT_ASSERT_EQ(m1.kind, MACH_OP_REG);
  UT_ASSERT_EQ(m2.kind, MACH_OP_REG);

  /* Register getters/setters */
  int dest_vr = irop_get_vreg(dest);
  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, dest_vr), md.u.reg.r0);
  tcc_ir_codegen_reg_set(ir, dest_vr, 7);
  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, dest_vr), 7);
  tcc_ir_codegen_reg_set(ir, dest_vr, md.u.reg.r0);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Coverage of arithmetic op shapes                                           */
/* -------------------------------------------------------------------------- */

UT_TEST(test_arith_op_family_lowering)
{
  static const TccIrOp ops[] = {
      TCCIR_OP_SUB, TCCIR_OP_MUL, TCCIR_OP_DIV, TCCIR_OP_IMOD,
      TCCIR_OP_AND, TCCIR_OP_OR,  TCCIR_OP_XOR, TCCIR_OP_SHL,
      TCCIR_OP_SAR, TCCIR_OP_SHR,
  };

  for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++)
  {
    TCCIRState *ir = build_arith(ops[k], 10, 4);

    int idx = -1;
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      if (ir->compact_instructions[i].op == ops[k])
      {
        idx = i;
        break;
      }
    }
    UT_ASSERT(idx >= 0);

    IRQuadCompact *q = &ir->compact_instructions[idx];
    IROperand d = tcc_ir_codegen_dest_get(ir, q);
    IROperand s1 = tcc_ir_codegen_src1_get(ir, q);
    IROperand s2 = tcc_ir_codegen_src2_get(ir, q);
    MachineOperand md = machine_op_from_ir(ir, &d);
    MachineOperand m1 = machine_op_from_ir(ir, &s1);
    MachineOperand m2 = machine_op_from_ir(ir, &s2);

    /* Destination is always allocated to a register or stack slot. */
    UT_ASSERT(md.kind == MACH_OP_REG || md.kind == MACH_OP_SPILL || md.kind == MACH_OP_FRAME_ADDR);
    /* Sources are register operands (after regalloc). */
    UT_ASSERT(m1.kind == MACH_OP_REG);
    UT_ASSERT(m2.kind == MACH_OP_REG);

    tcc_ir_free(ir);
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
/* 64-bit arithmetic produces register pairs                                   */
/* -------------------------------------------------------------------------- */

UT_TEST(test_arith_64bit_pair)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);

  tcc_ir_vreg_type_set_64bit(ir, a);
  tcc_ir_vreg_type_set_64bit(ir, b);
  tcc_ir_vreg_type_set_64bit(ir, c);

  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_c = sv_var(c);
  s_a.type.t = VT_LLONG;
  s_b.type.t = VT_LLONG;
  s_c.type.t = VT_LLONG;

  SValue s_l = sv_const(0x12345678);
  SValue s_r = sv_const(0x9abcdef0);
  s_l.type.t = VT_LLONG;
  s_r.type.t = VT_LLONG;

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_l, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_r, NULL, &s_b);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_a, &s_b, &s_c);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_c, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  int add_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_ADD)
    {
      add_idx = i;
      break;
    }
  }
  UT_ASSERT(add_idx >= 0);

  IRQuadCompact *q = &ir->compact_instructions[add_idx];
  IROperand d = tcc_ir_codegen_dest_get(ir, q);
  MachineOperand md = machine_op_from_ir(ir, &d);

  UT_ASSERT(md.is_64bit);
  /* Either spilled or allocated to an even register pair. */
  if (md.kind == MACH_OP_REG)
  {
    UT_ASSERT(md.u.reg.r1 != PREG_NONE);
    UT_ASSERT((md.u.reg.r0 & 1) == 0);
  }

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * Dispatch-level tests (tcc_ir_codegen_generate)
 *
 * The tests above only exercise ir/codegen.c's small accessor helpers.
 * These drive the actual ~2670-line dispatch loop (tcc_ir_codegen_generate),
 * using the codegen_mop_stubs.c recording layer instead of a real backend,
 * and assert which tcc_gen_machine_*_mop got called, how many times, and
 * with what TccIrOp/operand kinds. See docs/plan_codegen_unit_tests.md.
 * ============================================================================ */

/* ADD/SUB/bitwise/shifts all route through data_processing_mop; MUL/DIV/IMOD
 * route through muldiv_mop -- confirmed by reading the case-label groups in
 * ir/codegen.c (ADD/SUB at ~2764, SHL/SHR/SAR/ROR/OR/AND/XOR at ~2964, both
 * ending in tcc_gen_machine_data_processing_mop; MUL/DIV/UDIV/IMOD/UMOD at
 * ~2522 ending in tcc_gen_machine_muldiv_mop). build_arith()'s 3-temp shape
 * trivially satisfies can_skip_dry_run (registers_for_allocator=13, so the
 * threshold is 11 -- see codegen.c ~2145), so each of these fires its mop
 * exactly once (single real-run pass, no dry-run duplication). */

UT_TEST(test_dispatch_add_routes_to_data_processing_mop)
{
  cgstub_reset();
  TCCIRState *ir = build_arith(TCCIR_OP_ADD, 5, 3);
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("data_processing_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("data_processing_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->ir_op, TCCIR_OP_ADD);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG);
  UT_ASSERT_EQ(c->src2_kind, MACH_OP_REG);
  /* Immediate ASSIGNs lower separately; RETURNVALUE separately too. */
  UT_ASSERT_EQ(cgstub_call_count("assign_mop"), 2);
  UT_ASSERT_EQ(cgstub_call_count("return_value_mop"), 1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_arith_op_family_routes_correctly)
{
  static const struct
  {
    TccIrOp op;
    const char *mop_name;
  } cases[] = {
      {TCCIR_OP_SUB, "data_processing_mop"}, {TCCIR_OP_AND, "data_processing_mop"},
      {TCCIR_OP_OR, "data_processing_mop"},  {TCCIR_OP_XOR, "data_processing_mop"},
      {TCCIR_OP_SHL, "data_processing_mop"}, {TCCIR_OP_SAR, "data_processing_mop"},
      {TCCIR_OP_SHR, "data_processing_mop"}, {TCCIR_OP_MUL, "muldiv_mop"},
      {TCCIR_OP_DIV, "muldiv_mop"},          {TCCIR_OP_IMOD, "muldiv_mop"},
      {TCCIR_OP_BOOL_OR, "bool_mop"},        {TCCIR_OP_BOOL_AND, "bool_mop"},
  };

  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++)
  {
    cgstub_reset();
    TCCIRState *ir = build_arith(cases[k].op, 10, 4);
    tcc_ir_codegen_generate(ir);

    UT_ASSERT_EQ(cgstub_call_count(cases[k].mop_name), 1);
    const CgStubCall *c = cgstub_nth_call(cases[k].mop_name, 0);
    UT_ASSERT(c != NULL);
    UT_ASSERT_EQ(c->ir_op, cases[k].op);

    tcc_ir_free(ir);
  }
  return 0;
}

/* UMULL/SMULL are distinct opcodes for 64-bit-widening multiply, directly
 * constructible via tcc_ir_put's 3-operand (src1,src2,dest) shape -- unlike
 * MLA, which needs a 4th (accumulator) operand tcc_ir_put has no slot for
 * and is normally synthesized by the fusion optimizer (see
 * ir/opt_gens_fusion.c, already covered by test_opt_fusion.c's fusion_mla
 * suite via the utb_emit4 hand-building API). Documented gap: MLA's direct
 * codegen.c dispatch (mla_mop / mlal_accum_mop) is not exercised here. */
UT_TEST(test_dispatch_umull_smull_route_to_dedicated_mops)
{
  static const struct
  {
    TccIrOp op;
    const char *mop_name;
  } cases[] = {
      {TCCIR_OP_UMULL, "umull_mop"},
      {TCCIR_OP_SMULL, "smull_mop"},
  };

  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++)
  {
    cgstub_reset();
    TCCIRState *ir = build_arith(cases[k].op, 10, 4);
    tcc_ir_codegen_generate(ir);

    UT_ASSERT_EQ(cgstub_call_count(cases[k].mop_name), 1);

    tcc_ir_free(ir);
  }
  return 0;
}

/* Forces codegen.c's non-skip (two-pass) path -- see the identical
 * construction in test_codegen_dispatch_smoke.c's
 * test_dispatch_smoke_forces_two_pass_when_register_pressure_high, which
 * establishes that >=12 simultaneously-live temporaries does it. Confirms
 * dry-run and real-run agree on the same dispatch decision for ADD. */
UT_TEST(test_dispatch_add_agrees_across_dry_and_real_pass)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  enum
  {
    NPARAM = 12
  };
  int t[NPARAM];
  SValue s[NPARAM];
  for (int i = 0; i < NPARAM; i++)
  {
    t[i] = tcc_ir_vreg_alloc_temp(ir);
    s[i] = sv_var(t[i]);
    SValue s_imm = sv_const(i + 1);
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_imm, NULL, &s[i]);
  }

  int acc = tcc_ir_vreg_alloc_temp(ir);
  SValue s_acc = sv_var(acc);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s[0], NULL, &s_acc);
  for (int i = 1; i < NPARAM; i++)
  {
    int next_acc = tcc_ir_vreg_alloc_temp(ir);
    SValue s_next = sv_var(next_acc);
    tcc_ir_put(ir, TCCIR_OP_ADD, &s_acc, &s[i], &s_next);
    s_acc = s_next;
  }
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_acc, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 1);
  int dry_adds = cgstub_call_count_pass("data_processing_mop", 0);
  int real_adds = cgstub_call_count_pass("data_processing_mop", 1);
  UT_ASSERT_EQ(dry_adds, NPARAM - 1);
  UT_ASSERT_EQ(real_adds, NPARAM - 1);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * ADD/SUB -> CMP #0 flags-fusion peephole (ir/codegen.c ~2764-2821)
 *
 * When an ADD/SUB is immediately followed by `CMP dest, #0` and then (modulo
 * NOPs) a `JUMPIF EQ/NE`, codegen.c skips the CMP and instead emits the
 * ADD/SUB with flag-setting encoding (data_processing_mop_flags instead of
 * plain data_processing_mop) -- ARM Thumb SUBS/ADDS already sets the Z flag
 * a CMP #0 would test. The CMP is IR-index-skipped (codegen_skip_cmp), not
 * NOP'd, so both dry- and real-run agree on which instruction to drop.
 *
 * `JUMPIF`'s src1 here is the *comparison token* (TOK_EQ/TOK_NE), not a
 * boolean value -- this is how `tcc_ir_codegen_test_gen()` (ir/codegen.c
 * ~661-683) actually emits a JUMPIF following a CMP; a plain boolean-valued
 * JUMPIF (as built in test_codegen_control.c's dispatch tests) never
 * triggers this peephole since there's no preceding CMP.
 * ============================================================================ */

UT_TEST(test_dispatch_add_cmp_zero_jumpif_eq_fuses_into_flags_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_c = sv_var(c);
  SValue s_five = sv_const(5);
  SValue s_three = sv_const(3);
  SValue s_zero = sv_const(0);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_five, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_three, NULL, &s_b);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_a, &s_b, &s_c);
  tcc_ir_put(ir, TCCIR_OP_CMP, &s_c, &s_zero, NULL);

  /* JUMPIF condition operand is the TOK_EQ token itself (an immediate),
   * mirroring tcc_ir_codegen_test_gen()'s real emission shape. Target is the
   * RETURNVALUE right after -- only routing is under test here, not the
   * branch's actual semantics. */
  SValue s_cond = sv_const(TOK_EQ);
  SValue s_target = sv_const(ir->next_instruction_index + 1);
  tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_cond, NULL, &s_target);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_c, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("data_processing_mop_flags"), 1);
  UT_ASSERT_EQ(cgstub_call_count("data_processing_mop"), 0); /* fused away, not double-emitted */
  const CgStubCall *c_flags = cgstub_nth_call("data_processing_mop_flags", 0);
  UT_ASSERT(c_flags != NULL);
  UT_ASSERT_EQ(c_flags->ir_op, TCCIR_OP_ADD);
  UT_ASSERT_EQ(c_flags->dest_kind, MACH_OP_REG);

  /* The CMP is skipped (not dispatched at all); JUMPIF still dispatches
   * normally to conditional_jump_mop. */
  UT_ASSERT_EQ(cgstub_call_count("conditional_jump_mop"), 1);

  tcc_ir_free(ir);
  return 0;
}

/* SUB variant + TOK_NE, to confirm the peephole isn't ADD/EQ-specific. */
UT_TEST(test_dispatch_sub_cmp_zero_jumpif_ne_fuses_into_flags_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_c = sv_var(c);
  SValue s_ten = sv_const(10);
  SValue s_four = sv_const(4);
  SValue s_zero = sv_const(0);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_ten, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_four, NULL, &s_b);
  tcc_ir_put(ir, TCCIR_OP_SUB, &s_a, &s_b, &s_c);
  tcc_ir_put(ir, TCCIR_OP_CMP, &s_c, &s_zero, NULL);

  SValue s_cond = sv_const(TOK_NE);
  SValue s_target = sv_const(ir->next_instruction_index + 1);
  tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_cond, NULL, &s_target);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_c, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("data_processing_mop_flags"), 1);
  UT_ASSERT_EQ(cgstub_call_count("data_processing_mop"), 0);
  const CgStubCall *c_flags = cgstub_nth_call("data_processing_mop_flags", 0);
  UT_ASSERT(c_flags != NULL);
  UT_ASSERT_EQ(c_flags->ir_op, TCCIR_OP_SUB);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * CMP + SELECT -> SUBS+IT peephole (ir/codegen.c ~2925-2962)
 *
 * `CMP x, #K` immediately followed by `SELECT dest, #1, #0, NE` (or
 * `#0, #1, EQ`) collapses cmp+ite+movne+moveq (4 instr) into subs+it+movne
 * (3 instr) via subs_eq_select_01 -- one of the "always returns 0" fusion
 * stubs (see docs/plan_codegen_unit_tests.md §1/§8), so this is an
 * attempt-only test: the CMP falls through to its normal data_processing_mop
 * dispatch and the SELECT still dispatches to select_mop, same discipline as
 * the STRD/LDRD attempt tests in test_codegen_mem.c.
 * ============================================================================ */

UT_TEST(test_dispatch_cmp_select_01_attempts_subs_eq_select)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int x = tcc_ir_vreg_alloc_temp(ir);
  int dest = tcc_ir_vreg_alloc_temp(ir);
  SValue s_x = sv_var(x);
  SValue s_five = sv_const(5);
  SValue s_k = sv_const(3);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_five, NULL, &s_x);
  tcc_ir_put(ir, TCCIR_OP_CMP, &s_x, &s_k, NULL);

  /* SELECT: dest = (cond) ? src1 : src2 -- pool layout [dest, src1, src2,
   * cond], built directly since tcc_ir_put has no 4-operand form. */
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(dest, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 1, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_imm32(-1, TOK_NE, IROP_BTYPE_INT32));
  int sel_idx = ir->next_instruction_index;
  IRQuadCompact *sq = &ir->compact_instructions[sel_idx];
  sq->op = TCCIR_OP_SELECT;
  sq->operand_base = pool_base;
  ir->next_instruction_index++;

  SValue s_dest = sv_var(dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("subs_eq_select_01"), 1);
  const CgStubCall *c = cgstub_nth_call("subs_eq_select_01", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG); /* CMP's x */
  UT_ASSERT_EQ(c->src2_kind, MACH_OP_IMM); /* CMP's #K */
  /* Stub returns 0 -- fusion doesn't land, so both the CMP (falls through to
   * data_processing_mop) and the SELECT (select_mop) still dispatch. */
  UT_ASSERT_EQ(cgstub_call_count("data_processing_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("select_mop"), 1);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * ZEXT / PACK64 dispatch (ir/codegen.c ~3984-4001)
 *
 * Both are 64-bit-widening ops with their own case labels, unrelated to the
 * peephole work above. ZEXT lowers via assign_mop like a plain ASSIGN would
 * (with cq->op forced to TCCIR_OP_ASSIGN even though the dispatched
 * instruction is ZEXT -- ZEXT exists only to stay opaque to the IR
 * optimizer's sign-extension tracking, not for any codegen difference).
 * ============================================================================ */

UT_TEST(test_dispatch_zext_routes_to_assign_mop_forced_to_assign_op)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int src = tcc_ir_vreg_alloc_temp(ir);
  int dest = tcc_ir_vreg_alloc_temp(ir);
  tcc_ir_vreg_type_set_64bit(ir, dest);
  SValue s_src = sv_var(src);
  SValue s_dest = sv_var(dest);
  s_dest.type.t = VT_LLONG;
  SValue s_five = sv_const(5);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_five, NULL, &s_src);
  tcc_ir_put(ir, TCCIR_OP_ZEXT, &s_src, NULL, &s_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("assign_mop"), 2); /* setup ASSIGN + the ZEXT itself */
  const CgStubCall *c = cgstub_nth_call("assign_mop", 1);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->ir_op, TCCIR_OP_ASSIGN); /* forced, not TCCIR_OP_ZEXT */

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_pack64_routes_to_pack64_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int lo = tcc_ir_vreg_alloc_temp(ir);
  int hi = tcc_ir_vreg_alloc_temp(ir);
  int dest = tcc_ir_vreg_alloc_temp(ir);
  tcc_ir_vreg_type_set_64bit(ir, dest);
  SValue s_lo = sv_var(lo);
  SValue s_hi = sv_var(hi);
  SValue s_dest = sv_var(dest);
  s_dest.type.t = VT_LLONG;
  SValue s_c1 = sv_const(0x1111);
  SValue s_c2 = sv_const(0x2222);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c1, NULL, &s_lo);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_c2, NULL, &s_hi);
  tcc_ir_put(ir, TCCIR_OP_PACK64, &s_lo, &s_hi, &s_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("pack64_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("pack64_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG); /* lo */
  UT_ASSERT_EQ(c->src2_kind, MACH_OP_REG); /* hi */

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * MUL-by-const + ADD -> fused shifted-add peephole (ir/codegen.c ~2530-2607)
 *
 * When a MUL(var, #const) result feeds directly (and solely) into an
 * immediately-following ADD, the trailing shift folds into the ADD via ARM's
 * flexible second operand (mul_const_add_fused_mop) -- one of the
 * "always returns 0" fusion stubs, so this is an attempt-only test. Notable
 * because the real bug this peephole's safety check (mul_dest not used
 * elsewhere) guards against once corrupted the heap in self-host builds
 * (see the comment at ir/codegen.c ~2572-2584) -- not a toy peephole.
 * ============================================================================ */

UT_TEST(test_dispatch_mul_const_add_attempts_fused_shifted_add)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v = tcc_ir_vreg_alloc_temp(ir);
  int base = tcc_ir_vreg_alloc_temp(ir);
  int mul_dest = tcc_ir_vreg_alloc_temp(ir);
  int add_dest = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v = sv_var(v);
  SValue s_base = sv_var(base);
  SValue s_mul_dest = sv_var(mul_dest);
  SValue s_add_dest = sv_var(add_dest);
  SValue s_five = sv_const(5);
  SValue s_hundred = sv_const(100);
  SValue s_three = sv_const(3);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_five, NULL, &s_v);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_hundred, NULL, &s_base);
  tcc_ir_put(ir, TCCIR_OP_MUL, &s_v, &s_three, &s_mul_dest);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_base, &s_mul_dest, &s_add_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_add_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("mul_const_add_fused_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("mul_const_add_fused_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG); /* mul_dest */
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG); /* mul_var (v) */
  UT_ASSERT_EQ(c->src2_kind, MACH_OP_REG); /* add_base */
  /* Stub returns 0 -- fusion doesn't land, MUL and ADD still dispatch
   * individually. */
  UT_ASSERT_EQ(cgstub_call_count("muldiv_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("data_processing_mop"), 1);

  tcc_ir_free(ir);
  return 0;
}

/* Regression for the safety check itself (ir_codegen_vreg_used_elsewhere,
 * ir/codegen.c ~1627-1662): a stray read of mul_dest anywhere else in the
 * function -- here as a third instruction's src2, after the would-be fusing
 * ADD -- must block the fusion attempt entirely, not just prevent it from
 * landing. This is the exact shape that once miscompiled a self-host build
 * (base+idx*3 instead of base+idx*12, see the comment at ~2572-2584): any
 * other reader of mul_dest sees only the unscaled partial product. */
UT_TEST(test_dispatch_mul_const_add_used_elsewhere_blocks_fusion_attempt)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v = tcc_ir_vreg_alloc_temp(ir);
  int base = tcc_ir_vreg_alloc_temp(ir);
  int mul_dest = tcc_ir_vreg_alloc_temp(ir);
  int add_dest = tcc_ir_vreg_alloc_temp(ir);
  int extra = tcc_ir_vreg_alloc_temp(ir);
  int stray_dest = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v = sv_var(v);
  SValue s_base = sv_var(base);
  SValue s_mul_dest = sv_var(mul_dest);
  SValue s_add_dest = sv_var(add_dest);
  SValue s_extra = sv_var(extra);
  SValue s_stray_dest = sv_var(stray_dest);
  SValue s_five = sv_const(5);
  SValue s_hundred = sv_const(100);
  SValue s_three = sv_const(3);
  SValue s_seven = sv_const(7);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_five, NULL, &s_v);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_hundred, NULL, &s_base);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_seven, NULL, &s_extra);
  tcc_ir_put(ir, TCCIR_OP_MUL, &s_v, &s_three, &s_mul_dest);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_base, &s_mul_dest, &s_add_dest);
  /* Stray use of mul_dest as src2, after the fusing ADD. */
  tcc_ir_put(ir, TCCIR_OP_XOR, &s_extra, &s_mul_dest, &s_stray_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_add_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("mul_const_add_fused_mop"), 0);
  UT_ASSERT_EQ(cgstub_call_count("muldiv_mop"), 1);      /* MUL still dispatches */
  UT_ASSERT_EQ(cgstub_call_count("data_processing_mop"), 2); /* ADD and XOR, individually */

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * TCCIR_OP_MLA direct dispatch (ir/codegen.c ~2660-2687), 32-bit path
 *
 * Documented as a gap in docs/plan_codegen_unit_tests.md §3: "MLA needs a 4th
 * (accumulator) operand tcc_ir_put's 3-operand API has no slot for." True for
 * tcc_ir_put, but not for direct pool construction -- MLA's accumulator lives
 * at operand_base+3 (tcc_ir_op_get_accum), the same slot SELECT's condition
 * and STORE_INDEXED's scale already use elsewhere in this file. mla_mop is a
 * real (void, always-emitting) function, not one of the "returns 0" fusion
 * stubs, so this is genuine dispatch coverage, not an attempt-only test.
 *
 * The 64-bit path (mlal_accum_mop) is NOT covered here: it's one of the
 * always-0 fusion stubs, and unlike the UMULL/SMULL peephole's optional
 * fusion, TCCIR_OP_MLA's own 64-bit dispatch treats failure as fatal
 * (`if (!fused) tcc_error(...)`) -- stubs.c's _tcc_error aborts the whole
 * test binary, so it's unsafe to exercise without a per-test knob to make
 * mlal_accum_mop succeed (not attempted here, see docs/plan_codegen_unit_tests.md
 * §8's _tcc_error gap).
 * ============================================================================ */

UT_TEST(test_dispatch_mla_32bit_routes_to_mla_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int accum = tcc_ir_vreg_alloc_temp(ir);
  int dest = tcc_ir_vreg_alloc_temp(ir);
  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_accum = sv_var(accum);
  SValue s_five = sv_const(5);
  SValue s_three = sv_const(3);
  SValue s_ten = sv_const(10);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_five, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_three, NULL, &s_b);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_ten, NULL, &s_accum);

  /* MLA: pool layout [dest, src1, src2, accum] -- accum at operand_base+3,
   * the slot tcc_ir_put has no parameter for. */
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(dest, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(a, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(b, IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(accum, IROP_BTYPE_INT32));
  int mla_idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[mla_idx];
  q->op = TCCIR_OP_MLA;
  q->operand_base = pool_base;
  ir->next_instruction_index++;

  SValue s_dest = sv_var(dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("mla_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("mla_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG);
  UT_ASSERT_EQ(c->src2_kind, MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * UMULL/SMULL -> MLAL fusion attempt (ir/codegen.c ~2693-2726)
 *
 * When a UMULL/SMULL's 64-bit result feeds (as its sole use) directly into a
 * 64-bit ADD with a 64-bit accumulator, the pair maps to (S/U)MLAL via
 * mlal_accum_mop -- another always-0 fusion stub, so an attempt-only test.
 * Unlike TCCIR_OP_MLA's own 64-bit dispatch (tested above), failure here is
 * NOT fatal: the code falls through to the normal umull_mop/smull_mop
 * dispatch, so this is safe to exercise without risking a stubs.c
 * _tcc_error() abort.
 * ============================================================================ */

UT_TEST(test_dispatch_umull_add_attempts_mlal_fusion)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int lo = tcc_ir_vreg_alloc_temp(ir);
  int accum = tcc_ir_vreg_alloc_temp(ir);
  int add_dest = tcc_ir_vreg_alloc_temp(ir);
  tcc_ir_vreg_type_set_64bit(ir, lo);
  tcc_ir_vreg_type_set_64bit(ir, accum);
  tcc_ir_vreg_type_set_64bit(ir, add_dest);

  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_lo = sv_var(lo);
  SValue s_accum = sv_var(accum);
  SValue s_add_dest = sv_var(add_dest);
  s_lo.type.t = VT_LLONG;
  s_accum.type.t = VT_LLONG;
  s_add_dest.type.t = VT_LLONG;
  SValue s_five = sv_const(5);
  SValue s_three = sv_const(3);
  SValue s_hundred = sv_const(100);
  s_hundred.type.t = VT_LLONG;
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_five, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_three, NULL, &s_b);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_hundred, NULL, &s_accum);
  tcc_ir_put(ir, TCCIR_OP_UMULL, &s_a, &s_b, &s_lo);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_lo, &s_accum, &s_add_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_add_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("mlal_accum_mop"), 1);
  /* Stub returns 0 -- fusion doesn't land, UMULL and ADD still dispatch
   * individually. */
  UT_ASSERT_EQ(cgstub_call_count("umull_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("data_processing_mop"), 1);

  tcc_ir_free(ir);
  return 0;
}

/* Regression for the safety check itself (ir_codegen_count_vreg_uses,
 * ir/codegen.c ~1604-1625): the UMULL's dest must be used *exactly once* for
 * the fusion attempt to even look at the following instruction. A second use
 * (here, an XOR reading `lo` again after the would-be fusing ADD) must block
 * the attempt entirely -- the mirror of the MUL-const-ADD safety check above,
 * same class of "partial value read by someone else" hazard. */
UT_TEST(test_dispatch_umull_used_twice_blocks_mlal_fusion_attempt)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int lo = tcc_ir_vreg_alloc_temp(ir);
  int accum = tcc_ir_vreg_alloc_temp(ir);
  int add_dest = tcc_ir_vreg_alloc_temp(ir);
  int extra = tcc_ir_vreg_alloc_temp(ir);
  int stray_dest = tcc_ir_vreg_alloc_temp(ir);
  tcc_ir_vreg_type_set_64bit(ir, lo);
  tcc_ir_vreg_type_set_64bit(ir, accum);
  tcc_ir_vreg_type_set_64bit(ir, add_dest);
  tcc_ir_vreg_type_set_64bit(ir, extra);
  tcc_ir_vreg_type_set_64bit(ir, stray_dest);

  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_lo = sv_var(lo);
  SValue s_accum = sv_var(accum);
  SValue s_add_dest = sv_var(add_dest);
  SValue s_extra = sv_var(extra);
  SValue s_stray_dest = sv_var(stray_dest);
  s_lo.type.t = VT_LLONG;
  s_accum.type.t = VT_LLONG;
  s_add_dest.type.t = VT_LLONG;
  s_extra.type.t = VT_LLONG;
  s_stray_dest.type.t = VT_LLONG;
  SValue s_five = sv_const(5);
  SValue s_three = sv_const(3);
  SValue s_hundred = sv_const(100);
  SValue s_seven = sv_const(7);
  s_hundred.type.t = VT_LLONG;
  s_seven.type.t = VT_LLONG;
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_five, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_three, NULL, &s_b);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_hundred, NULL, &s_accum);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_seven, NULL, &s_extra);
  tcc_ir_put(ir, TCCIR_OP_UMULL, &s_a, &s_b, &s_lo);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_lo, &s_accum, &s_add_dest);
  /* Second use of lo, after the would-be fusing ADD. */
  tcc_ir_put(ir, TCCIR_OP_XOR, &s_lo, &s_extra, &s_stray_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_add_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("mlal_accum_mop"), 0);
  UT_ASSERT_EQ(cgstub_call_count("umull_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("data_processing_mop"), 2); /* ADD and XOR, individually */

  tcc_ir_free(ir);
  return 0;
}
