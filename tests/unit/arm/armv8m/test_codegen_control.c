/*
 *  test_codegen_control.c - backend unit tests for control-flow IR ops
 *
 *  Exercises JUMP/JUMPIF/IJUMP operand accessors, switch-table layout helpers,
 *  and basic-block marking in ir/codegen.c.
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

static SValue sv_jump_target(int target_idx)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = target_idx;
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
/* JUMPIF operand layout                                                       */
/* -------------------------------------------------------------------------- */

UT_TEST(test_jumpif_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int cond = tcc_ir_vreg_alloc_temp(ir);
  SValue s_cond = sv_var(cond);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_cond);

  SValue jelse = sv_jump_target(5);
  int jif = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_cond, NULL, &jelse);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_cond, NULL, NULL);

  IRQuadCompact *q = &ir->compact_instructions[jif];
  UT_ASSERT_EQ(q->op, TCCIR_OP_JUMPIF);

  IROperand src = tcc_ir_codegen_src1_get(ir, q);
  IROperand dst = tcc_ir_codegen_dest_get(ir, q);

  UT_ASSERT(irop_has_vreg(src));
  UT_ASSERT(irop_is_immediate(dst));
  UT_ASSERT_EQ(irop_get_imm32(dst), 5);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* JUMP / IJUMP operand layout                                                 */
/* -------------------------------------------------------------------------- */

UT_TEST(test_jump_and_ijump_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int target = tcc_ir_vreg_alloc_temp(ir);
  SValue s_target = sv_var(target);
  SValue s_seven = sv_const(7);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_seven, NULL, &s_target);

  SValue jend = sv_jump_target(9);
  int j = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jend);
  int ij = tcc_ir_put(ir, TCCIR_OP_IJUMP, &s_target, NULL, NULL);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_target, NULL, NULL);

  IRQuadCompact *qj = &ir->compact_instructions[j];
  IRQuadCompact *qij = &ir->compact_instructions[ij];

  UT_ASSERT_EQ(qj->op, TCCIR_OP_JUMP);
  UT_ASSERT_EQ(qij->op, TCCIR_OP_IJUMP);

  IROperand jdst = tcc_ir_codegen_dest_get(ir, qj);
  IROperand ijsrc = tcc_ir_codegen_src1_get(ir, qij);

  UT_ASSERT(irop_is_immediate(jdst));
  UT_ASSERT_EQ(irop_get_imm32(jdst), 9);
  UT_ASSERT(irop_has_vreg(ijsrc));

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Diamond CFG: JUMPIF + JUMP backpatching                                     */
/* -------------------------------------------------------------------------- */

UT_TEST(test_diamond_backpatch)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v = sv_var(v);
  SValue s_one = sv_const(1);
  SValue s_ten = sv_const(10);
  SValue s_twenty = sv_const(20);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v);

  SValue jelse = sv_jump_target(-1);
  int branch = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_v, NULL, &jelse);

  int then_val = tcc_ir_vreg_alloc_temp(ir);
  SValue s_then = sv_var(then_val);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_ten, NULL, &s_then);

  SValue jmerge = sv_jump_target(-1);
  int skip = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jmerge);

  int else_label = ir->next_instruction_index;
  int else_val = tcc_ir_vreg_alloc_temp(ir);
  SValue s_else = sv_var(else_val);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_twenty, NULL, &s_else);

  int merge_label = ir->next_instruction_index;
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v, NULL, NULL);

  tcc_ir_codegen_backpatch(ir, branch, else_label);
  tcc_ir_codegen_backpatch(ir, skip, merge_label);

  UT_ASSERT_EQ(tcc_ir_op_get_dest(ir, &ir->compact_instructions[branch]).u.imm32, else_label);
  UT_ASSERT_EQ(tcc_ir_op_get_dest(ir, &ir->compact_instructions[skip]).u.imm32, merge_label);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Basic block start marker                                                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_bb_start)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int before = ir->basic_block_start;
  tcc_ir_codegen_bb_start(ir);
  UT_ASSERT_EQ(ir->basic_block_start, 1);
  (void)before;

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* SWITCH_TABLE / SWITCH_LOAD operand layout                                   */
/* -------------------------------------------------------------------------- */

UT_TEST(test_switch_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int idx = tcc_ir_vreg_alloc_temp(ir);
  SValue s_idx = sv_var(idx);
  SValue s_zero = sv_const(0);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_zero, NULL, &s_idx);

  /* SWITCH_TABLE: dest = table address, src1 = index. */
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(tcc_ir_vreg_alloc_temp(ir), IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(idx, IROP_BTYPE_INT32));

  int st_idx = ir->next_instruction_index;
  IRQuadCompact *qst = &ir->compact_instructions[st_idx];
  qst->op = TCCIR_OP_SWITCH_TABLE;
  qst->operand_base = pool_base;
  ir->next_instruction_index++;

  /* SWITCH_LOAD: dest = value, src1 = table address. */
  int pool_base2 = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(tcc_ir_vreg_alloc_temp(ir), IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(tcc_ir_vreg_alloc_temp(ir), IROP_BTYPE_INT32));

  int sl_idx = ir->next_instruction_index;
  IRQuadCompact *qsl = &ir->compact_instructions[sl_idx];
  qsl->op = TCCIR_OP_SWITCH_LOAD;
  qsl->operand_base = pool_base2;
  ir->next_instruction_index++;

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_idx, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  IRQuadCompact *qst2 = &ir->compact_instructions[st_idx];
  IRQuadCompact *qsl2 = &ir->compact_instructions[sl_idx];

  UT_ASSERT_EQ(qst2->op, TCCIR_OP_SWITCH_TABLE);
  UT_ASSERT_EQ(qsl2->op, TCCIR_OP_SWITCH_LOAD);

  IROperand st_src = tcc_ir_codegen_src1_get(ir, qst2);
  IROperand sl_src = tcc_ir_codegen_src1_get(ir, qsl2);
  MachineOperand mst_src = machine_op_from_ir(ir, &st_src);
  MachineOperand msl_src = machine_op_from_ir(ir, &sl_src);

  UT_ASSERT(mst_src.kind == MACH_OP_REG);
  UT_ASSERT(msl_src.kind == MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_codegen_cmp_jmp_set / tcc_ir_codegen_test_gen -- ir/codegen.c
 * ~516-787. These convert the frontend's lazy VT_CMP/VT_JMP/VT_JMPI value
 * representations (pending comparison, pending jump chain) into real
 * SETIF/JUMPIF/JUMP IR, driven by the *frontend value stack* (`vtop`), not
 * by dispatching an already-built IRQuadCompact like every other test in
 * this file. That value stack is normally tccgen.c's `vtop`/`_vstack`
 * globals (not linked into this unit-test binary); codegen_mop_stubs.c
 * provides a minimal fake (cgstub_vtop_push()/cgstub_vtop_get()) since
 * neither function needs any other frontend machinery (no gv()/vpush()) as
 * long as the pushed SValue's type never carries VT_BITFIELD (verified
 * below: svalue_init() zeroes type.t, so plain sv_var()-style values never
 * trigger the gv(RC_INT) call this harness cannot link).
 * -------------------------------------------------------------------------- */

/* Empty fake stack (vtop == _vstack, the ~522 guard): must no-op, not crash
 * reading vtop->r out of bounds. */
UT_TEST(test_cmp_jmp_set_empty_stack_is_noop)
{
  cgstub_reset(); /* resets the fake vtop/_vstack to empty, among other knobs */
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  tcc_ir_codegen_cmp_jmp_set(ir); /* must not crash */

  tcc_ir_free(ir);
  return 0;
}

/* VT_CMP with no pending jump chains (jtrue == jfalse == -1): the simple
 * case (~594-601) -- unlike tcc_ir_codegen_test_gen() below, this function
 * takes no `invert` argument at all; src.c.i is vtop->cmp_op verbatim, never
 * XORed. A single SETIF is emitted and vtop is rewritten to a plain vreg
 * holding the boolean (r = 0, i.e. a register-class value; vr = the new
 * temp). */
UT_TEST(test_cmp_jmp_set_simple_vt_cmp_emits_single_setif)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue *v = cgstub_vtop_push();
  v->r = VT_CMP;
  v->cmp_op = TOK_EQ;
  v->jtrue = -1;
  v->jfalse = -1;

  int before_instrs = ir->next_instruction_index;
  tcc_ir_codegen_cmp_jmp_set(ir);

  UT_ASSERT_EQ(ir->next_instruction_index, before_instrs + 1); /* one SETIF, no JUMPs */
  IRQuadCompact *q = &ir->compact_instructions[before_instrs];
  UT_ASSERT_EQ(q->op, TCCIR_OP_SETIF);
  IROperand src1 = tcc_ir_codegen_src1_get(ir, q);
  UT_ASSERT(irop_is_immediate(src1));
  UT_ASSERT_EQ(src1.u.imm32, TOK_EQ); /* cmp_op passed through verbatim, no invert */

  UT_ASSERT_EQ(cgstub_vtop_get()->r, 0); /* rewritten to a register-class value */
  UT_ASSERT(cgstub_vtop_get()->vr >= 0); /* holds the new SETIF dest temp */

  tcc_ir_free(ir);
  return 0;
}

/* VT_CMP with a pending jtrue chain (~539-593): SETIF + an unconditional
 * JUMP-to-end are emitted first, then (jtrue >= 0) the jtrue chain is
 * backpatched via tcc_ir_backpatch_to_here() to land right after that JUMP
 * (an ASSIGN dest=1 sits there), and finally end_jump's target is patched
 * to fall through past it. A prior JUMPIF at index `pending` stands in for
 * "an earlier `x == 1 || ...` already jumped here when true"; after the
 * call, `pending`'s own jump target must have been rewritten away from its
 * initial -1 sentinel by that tcc_ir_backpatch_to_here() call. */
UT_TEST(test_cmp_jmp_set_vt_cmp_merges_pending_jtrue_chain)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  /* A standalone JUMPIF instruction acting as the pending jtrue chain head
   * (dest.c.i == -1, i.e. "not yet patched" -- the same shape
   * tcc_ir_codegen_test_gen()'s own JUMPIF emission produces). */
  SValue jsrc, jdest;
  svalue_init(&jsrc);
  svalue_init(&jdest);
  jsrc.r = VT_CONST;
  jsrc.c.i = TOK_NE;
  jdest.r = VT_CONST;
  jdest.c.i = -1;
  int pending = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &jsrc, NULL, &jdest);

  SValue *v = cgstub_vtop_push();
  v->r = VT_CMP;
  v->cmp_op = TOK_LT;
  v->jtrue = pending;
  v->jfalse = -1;

  tcc_ir_codegen_cmp_jmp_set(ir);

  /* The pending JUMPIF's target got patched (by tcc_ir_backpatch_to_here())
   * to the ASSIGN dest=1 landing point rather than staying -1. */
  IRQuadCompact *qpending = &ir->compact_instructions[pending];
  IROperand pending_dest = tcc_ir_codegen_dest_get(ir, qpending);
  UT_ASSERT(pending_dest.u.imm32 != -1);

  UT_ASSERT_EQ(cgstub_vtop_get()->r, 0);

  tcc_ir_free(ir);
  return 0;
}

/* VT_JMP (v & 1 == 0) with an empty chain (vtop->c.i == -1): (~606-638)
 * unconditionally emits ASSIGN dest=0, an unconditional JUMP (end_jump,
 * initially unpatched), then (after backpatching the -- here empty, so a
 * no-op -- vtop->c.i chain to land here) ASSIGN dest=1, and finally patches
 * end_jump to land after that second ASSIGN. Net: exactly 3 new
 * instructions (ASSIGN, JUMP, ASSIGN) regardless of chain emptiness; vtop
 * becomes a plain register-class value holding the new temp. */
UT_TEST(test_cmp_jmp_set_vt_jmp_emits_default_and_flipped_assign_pair)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue *v = cgstub_vtop_push();
  v->r = VT_JMP; /* even: t = v & 1 == 0 */
  v->c.i = -1;    /* empty chain: tcc_ir_backpatch_to_here(-1) is a no-op */

  int before_instrs = ir->next_instruction_index;
  tcc_ir_codegen_cmp_jmp_set(ir);

  UT_ASSERT_EQ(ir->next_instruction_index, before_instrs + 3); /* ASSIGN, JUMP, ASSIGN */

  IRQuadCompact *q_assign0 = &ir->compact_instructions[before_instrs];
  UT_ASSERT_EQ(q_assign0->op, TCCIR_OP_ASSIGN);
  IROperand a0_src1 = tcc_ir_codegen_src1_get(ir, q_assign0);
  UT_ASSERT_EQ(a0_src1.u.imm32, 0); /* t == 0 for VT_JMP */

  IRQuadCompact *q_jump = &ir->compact_instructions[before_instrs + 1];
  UT_ASSERT_EQ(q_jump->op, TCCIR_OP_JUMP);

  IRQuadCompact *q_assign1 = &ir->compact_instructions[before_instrs + 2];
  UT_ASSERT_EQ(q_assign1->op, TCCIR_OP_ASSIGN);
  IROperand a1_src1 = tcc_ir_codegen_src1_get(ir, q_assign1);
  UT_ASSERT_EQ(a1_src1.u.imm32, 1); /* t ^ 1 == 1 */

  /* end_jump's target got patched to land after the second ASSIGN (not left
   * at its initial -1 sentinel). */
  IROperand jump_dest = tcc_ir_codegen_dest_get(ir, q_jump);
  UT_ASSERT_EQ(jump_dest.u.imm32, before_instrs + 3);

  UT_ASSERT_EQ(cgstub_vtop_get()->r, 0);
  UT_ASSERT(cgstub_vtop_get()->vr >= 0);

  tcc_ir_free(ir);
  return 0;
}

/* VT_JMPI (v & 1 == 1, t == 1) with a real nonempty chain (vtop->c.i points
 * at a genuine pending JUMPIF): tcc_ir_backpatch_to_here(vtop->c.i) must
 * actually rewrite that JUMPIF's target away from its initial -1 sentinel,
 * landing it at the first ASSIGN's *following* instruction (the JUMP), same
 * place any other control-flow path reaching "cond was true" would land. */
UT_TEST(test_cmp_jmp_set_vt_jmpi_backpatches_real_chain)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue jsrc, jdest;
  svalue_init(&jsrc);
  svalue_init(&jdest);
  jsrc.r = VT_CONST;
  jsrc.c.i = TOK_EQ;
  jdest.r = VT_CONST;
  jdest.c.i = -1;
  int chain_head = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &jsrc, NULL, &jdest);

  SValue *v = cgstub_vtop_push();
  v->r = VT_JMPI; /* odd: t = v & 1 == 1 */
  v->c.i = chain_head;

  int before_instrs = ir->next_instruction_index;
  tcc_ir_codegen_cmp_jmp_set(ir);

  UT_ASSERT_EQ(ir->next_instruction_index, before_instrs + 3);

  IRQuadCompact *q_assign0 = &ir->compact_instructions[before_instrs];
  IROperand a0_src1 = tcc_ir_codegen_src1_get(ir, q_assign0);
  UT_ASSERT_EQ(a0_src1.u.imm32, 1); /* t == 1 for VT_JMPI */

  /* chain_head's JUMPIF got backpatched to land right after the JUMP (i.e.
   * at the second ASSIGN, index before_instrs+2 -- tcc_ir_backpatch_to_here()
   * uses ir->next_instruction_index at the point it's called, which is
   * right after the JUMP was appended but before the second ASSIGN), not
   * left at -1. */
  IRQuadCompact *qhead = &ir->compact_instructions[chain_head];
  IROperand head_dest = tcc_ir_codegen_dest_get(ir, qhead);
  UT_ASSERT_EQ(head_dest.u.imm32, before_instrs + 2);

  UT_ASSERT_EQ(cgstub_vtop_get()->r, 0);

  tcc_ir_free(ir);
  return 0;
}

/* Neither VT_CMP nor VT_JMP/VT_JMPI (e.g. a plain VT_CONST): the function
 * body does nothing at all -- no branch matches, so vtop is left completely
 * untouched (unlike test_gen's sibling constant-folding logic, cmp_jmp_set
 * has no `else` arm for this case). */
UT_TEST(test_cmp_jmp_set_plain_value_is_noop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue *v = cgstub_vtop_push();
  v->r = VT_CONST;
  v->c.i = 42;

  int before_instrs = ir->next_instruction_index;
  tcc_ir_codegen_cmp_jmp_set(ir);

  UT_ASSERT_EQ(ir->next_instruction_index, before_instrs); /* nothing emitted */
  UT_ASSERT_EQ(cgstub_vtop_get()->r, VT_CONST);             /* untouched */
  UT_ASSERT_EQ((int)cgstub_vtop_get()->c.i, 42);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* tcc_ir_codegen_test_gen -- the sibling that additionally pops vtop
 * (`--vtop` at the end) and takes an explicit `invert` argument, used by
 * `if`/`while`/`&&`/`||` condition lowering: VT_CMP (with/without invert),
 * VT_JMP/VT_JMPI's three sub-branches (empty-chain adopt, nonempty-chain
 * merge, mismatched-invert new JUMP), the compile-time-constant fold (taken
 * and not-taken), and one level of recursion through TCCIR_OP_TEST_ZERO for
 * plain non-constant values (~766-782 -- safe here since svalue_init()
 * leaves type.t == 0, never VT_BITFIELD, so the gv(RC_INT) call this
 * harness can't link is never reached).
 * -------------------------------------------------------------------------- */

/* VT_CMP, invert = 0, no pending chains: emits one JUMPIF (src1 = cmp_op
 * unchanged) and returns its own instruction index as the new chain head;
 * vtop is popped (stack depth decreases by one). */
UT_TEST(test_test_gen_vt_cmp_no_invert_emits_jumpif_and_returns_its_index)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue *v = cgstub_vtop_push();
  v->r = VT_CMP;
  v->cmp_op = TOK_LT;
  v->jtrue = -1;
  v->jfalse = -1;

  int before_instrs = ir->next_instruction_index;
  int result = tcc_ir_codegen_test_gen(ir, /*invert=*/0, /*test=*/-1);

  UT_ASSERT_EQ(ir->next_instruction_index, before_instrs + 1);
  UT_ASSERT_EQ(result, before_instrs); /* new JUMPIF's own index, chain head */

  IRQuadCompact *q = &ir->compact_instructions[before_instrs];
  UT_ASSERT_EQ(q->op, TCCIR_OP_JUMPIF);
  IROperand src1 = tcc_ir_codegen_src1_get(ir, q);
  UT_ASSERT(irop_is_immediate(src1));
  UT_ASSERT_EQ(src1.u.imm32, TOK_LT); /* invert == 0: cmp_op unchanged */

  UT_ASSERT(cgstub_vtop_get() == NULL); /* popped: stack is empty again */

  tcc_ir_free(ir);
  return 0;
}

/* VT_CMP, invert = 1: cmp_op is XORed with 1 (TOK_EQ -> TOK_NE) in the
 * emitted JUMPIF's immediate -- per the "TCC comparison tokens XOR with 1
 * to invert" comment at ~675-676. */
UT_TEST(test_test_gen_vt_cmp_invert_xors_cmp_op)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue *v = cgstub_vtop_push();
  v->r = VT_CMP;
  v->cmp_op = TOK_EQ;
  v->jtrue = -1;
  v->jfalse = -1;

  int before_instrs = ir->next_instruction_index;
  tcc_ir_codegen_test_gen(ir, /*invert=*/1, /*test=*/-1);

  IRQuadCompact *q = &ir->compact_instructions[before_instrs];
  IROperand src1 = tcc_ir_codegen_src1_get(ir, q);
  UT_ASSERT_EQ(src1.u.imm32, TOK_EQ ^ 1); /* == TOK_NE */

  tcc_ir_free(ir);
  return 0;
}

/* Plain non-constant value (a register/local-class SValue, not VT_CONST):
 * the ~766-782 recursive path -- emits TCCIR_OP_TEST_ZERO on the original
 * value, rewrites vtop in place to a synthetic VT_CMP (TOK_NE, no pending
 * chains), then recurses. The recursive call's own VT_CMP branch is what
 * actually emits the JUMPIF and pops vtop, so the net effect from the
 * caller's perspective is: two new instructions (TEST_ZERO then JUMPIF),
 * vtop popped once (not twice -- recursion reuses the same stack slot). */
UT_TEST(test_test_gen_plain_value_recurses_through_test_zero)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int t = tcc_ir_vreg_alloc_temp(ir);
  SValue *v = cgstub_vtop_push();
  v->vr = t;
  v->r = 0; /* plain register-class value, not VT_CONST/VT_CMP/VT_JMP */
  v->type.t = VT_INT;

  int before_instrs = ir->next_instruction_index;
  int result = tcc_ir_codegen_test_gen(ir, /*invert=*/0, /*test=*/-1);

  UT_ASSERT_EQ(ir->next_instruction_index, before_instrs + 2); /* TEST_ZERO, JUMPIF */

  IRQuadCompact *q_tz = &ir->compact_instructions[before_instrs];
  UT_ASSERT_EQ(q_tz->op, TCCIR_OP_TEST_ZERO);
  IROperand tz_src1 = tcc_ir_codegen_src1_get(ir, q_tz);
  UT_ASSERT(irop_has_vreg(tz_src1));
  UT_ASSERT_EQ(irop_get_vreg(tz_src1), t);

  IRQuadCompact *q_ji = &ir->compact_instructions[before_instrs + 1];
  UT_ASSERT_EQ(q_ji->op, TCCIR_OP_JUMPIF);
  IROperand ji_src1 = tcc_ir_codegen_src1_get(ir, q_ji);
  UT_ASSERT_EQ(ji_src1.u.imm32, TOK_NE); /* synthetic cmp_op, invert == 0 */

  UT_ASSERT_EQ(result, before_instrs + 1); /* the JUMPIF's own index */
  UT_ASSERT(cgstub_vtop_get() == NULL); /* popped exactly once overall */

  tcc_ir_free(ir);
  return 0;
}

/* VT_JMP/VT_JMPI, (v & 1) == invert (~719-724): the pending chain is empty
 * (vtop->c.i == -1) -- adopted directly (vtop->c.i = test) with no new IR
 * emitted at all; the returned `test` is the caller's original value,
 * unchanged (only vtop->c.i is written here, not the local `test`). */
UT_TEST(test_test_gen_vt_jmp_matching_invert_adopts_empty_chain)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue *v = cgstub_vtop_push();
  v->r = VT_JMP; /* even: v & 1 == 0 */
  v->c.i = -1;    /* empty chain */

  int before_instrs = ir->next_instruction_index;
  int result = tcc_ir_codegen_test_gen(ir, /*invert=*/0, /*test=*/42);

  UT_ASSERT_EQ(ir->next_instruction_index, before_instrs); /* nothing emitted */
  UT_ASSERT_EQ(result, 42);                                /* caller's test, unchanged */

  tcc_ir_free(ir);
  return 0;
}

/* VT_JMP/VT_JMPI, (v & 1) == invert, nonempty chain (~725-732): the new
 * `test` chain gets merged into vtop's existing chain via
 * tcc_ir_backpatch_first(), and the function adopts vtop->c.i as its
 * returned chain head. A standalone JUMPIF stands in for both chains. */
UT_TEST(test_test_gen_vt_jmp_matching_invert_merges_nonempty_chain)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue jsrc, jdest;
  svalue_init(&jsrc);
  svalue_init(&jdest);
  jsrc.r = VT_CONST;
  jsrc.c.i = TOK_EQ;
  jdest.r = VT_CONST;
  jdest.c.i = -1;
  int vtop_chain = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &jsrc, NULL, &jdest);

  SValue jsrc2, jdest2;
  svalue_init(&jsrc2);
  svalue_init(&jdest2);
  jsrc2.r = VT_CONST;
  jsrc2.c.i = TOK_NE;
  jdest2.r = VT_CONST;
  jdest2.c.i = -1;
  int test_chain = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &jsrc2, NULL, &jdest2);

  SValue *v = cgstub_vtop_push();
  v->r = VT_JMP; /* even: v & 1 == 0 */
  v->c.i = vtop_chain;

  int result = tcc_ir_codegen_test_gen(ir, /*invert=*/0, /*test=*/test_chain);

  /* tcc_ir_backpatch_first(ir, vtop->c.i, test) walks the chain STARTING AT
   * vtop->c.i (== vtop_chain) to find its last link, then patches THAT
   * link's target to `test` (== test_chain) -- a single-element chain here,
   * so vtop_chain's own JUMPIF gets its target set to test_chain, linking
   * the two chains together (not resolved to a real address yet). */
  IRQuadCompact *q_vtop_chain = &ir->compact_instructions[vtop_chain];
  IROperand vtop_chain_dest = tcc_ir_codegen_dest_get(ir, q_vtop_chain);
  UT_ASSERT_EQ(vtop_chain_dest.u.imm32, test_chain);

  /* The function adopts vtop's chain (vtop_chain) as its own return value. */
  UT_ASSERT_EQ(result, vtop_chain);

  tcc_ir_free(ir);
  return 0;
}

/* VT_JMP/VT_JMPI, (v & 1) != invert (~734-743): emits an unconditional JUMP
 * and backpatches vtop's existing chain to land right after it. */
UT_TEST(test_test_gen_vt_jmp_mismatched_invert_emits_jump)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  SValue jsrc, jdest;
  svalue_init(&jsrc);
  svalue_init(&jdest);
  jsrc.r = VT_CONST;
  jsrc.c.i = TOK_EQ;
  jdest.r = VT_CONST;
  jdest.c.i = -1;
  int chain = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &jsrc, NULL, &jdest);

  SValue *v = cgstub_vtop_push();
  v->r = VT_JMP; /* even: v & 1 == 0 */
  v->c.i = chain;

  int before_instrs = ir->next_instruction_index;
  int result = tcc_ir_codegen_test_gen(ir, /*invert=*/1, /*test=*/-1); /* invert=1 != (v&1)=0 */

  UT_ASSERT_EQ(ir->next_instruction_index, before_instrs + 1); /* one JUMP emitted */
  IRQuadCompact *q = &ir->compact_instructions[before_instrs];
  UT_ASSERT_EQ(q->op, TCCIR_OP_JUMP);
  UT_ASSERT_EQ(result, before_instrs); /* the new JUMP's own index */

  /* chain's JUMPIF got backpatched to land right AFTER the new JUMP
   * (tcc_ir_backpatch_to_here() uses ir->next_instruction_index at the
   * point it's called, i.e. right after the JUMP was appended), not left
   * at its initial -1 sentinel. */
  IRQuadCompact *qchain = &ir->compact_instructions[chain];
  IROperand chain_dest = tcc_ir_codegen_dest_get(ir, qchain);
  UT_ASSERT_EQ(chain_dest.u.imm32, before_instrs + 1);

  tcc_ir_free(ir);
  return 0;
}

/* Compile-time-constant condition (VT_CONST, no lvalue/sym), true-branch
 * taken (`(c.i != 0) != invert`, ~747-764): emits an unconditional JUMP and
 * sets CODE_OFF_BIT in nocode_wanted to suppress the (unreachable)
 * fallthrough code, mirroring gjmp_acs()'s CODE_OFF() call. */
UT_TEST(test_test_gen_constant_condition_taken_emits_jump_and_sets_nocode)
{
  cgstub_reset(); /* also resets nocode_wanted to 0 (see codegen_mop_stubs.c) */
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();
  extern int nocode_wanted;

  SValue *v = cgstub_vtop_push();
  v->r = VT_CONST;
  v->c.i = 1; /* nonzero: (1 != 0) != 0 (invert) -> true, jump taken */

  int before_instrs = ir->next_instruction_index;
  int result = tcc_ir_codegen_test_gen(ir, /*invert=*/0, /*test=*/-1);

  UT_ASSERT_EQ(ir->next_instruction_index, before_instrs + 1);
  IRQuadCompact *q = &ir->compact_instructions[before_instrs];
  UT_ASSERT_EQ(q->op, TCCIR_OP_JUMP);
  UT_ASSERT_EQ(result, before_instrs);
  UT_ASSERT((nocode_wanted & 0x20000000) != 0); /* CODE_OFF_BIT set */

  tcc_ir_free(ir);
  return 0;
}

/* Compile-time-constant condition, NOT taken (`(c.i != 0) != invert` is
 * false): no IR emitted at all, `test` is returned unchanged, nocode_wanted
 * untouched. */
UT_TEST(test_test_gen_constant_condition_not_taken_is_noop)
{
  cgstub_reset(); /* also resets nocode_wanted to 0 (see codegen_mop_stubs.c) */
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();
  extern int nocode_wanted;

  SValue *v = cgstub_vtop_push();
  v->r = VT_CONST;
  v->c.i = 0; /* zero: (0 != 0) != 0 (invert) -> false, not taken */

  int before_instrs = ir->next_instruction_index;
  int result = tcc_ir_codegen_test_gen(ir, /*invert=*/0, /*test=*/1234);

  UT_ASSERT_EQ(ir->next_instruction_index, before_instrs); /* nothing emitted */
  UT_ASSERT_EQ(result, 1234);                              /* test passed through unchanged */
  UT_ASSERT_EQ(nocode_wanted, 0);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * Dispatch-level tests (tcc_ir_codegen_generate)
 *
 * See test_codegen_arith.c's dispatch-level section header for the overall
 * rationale. JUMP/JUMPIF/IJUMP/SETIF each have dedicated case labels in
 * ir/codegen.c (~4003/4011/4028/4075). JUMPIF falls through to
 * conditional_jump_mop unless a preceding TEST_ZERO peephole set
 * codegen_cbz_reg (not exercised here, so plain JUMPIF always takes the
 * conditional_jump_mop path in these tests).
 * ============================================================================ */

UT_TEST(test_dispatch_jump_routes_to_jump_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v = sv_var(v);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v);

  SValue jend = sv_jump_target(3);
  tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jend);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("jump_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("jump_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->ir_op, TCCIR_OP_JUMP);
  UT_ASSERT_EQ(c->aux0, 3); /* target_ir */

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_jumpif_routes_to_conditional_jump_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int cond = tcc_ir_vreg_alloc_temp(ir);
  SValue s_cond = sv_var(cond);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_cond);

  SValue jelse = sv_jump_target(4);
  tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_cond, NULL, &jelse);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_cond, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("conditional_jump_mop"), 1);
  UT_ASSERT_EQ(cgstub_call_count("cbz_jump_mop"), 0);
  const CgStubCall *c = cgstub_nth_call("conditional_jump_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->ir_op, TCCIR_OP_JUMPIF);
  UT_ASSERT_EQ(c->aux0, 4); /* target_ir */

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_ijump_routes_to_indirect_jump_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int target = tcc_ir_vreg_alloc_temp(ir);
  SValue s_target = sv_var(target);
  SValue s_seven = sv_const(7);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_seven, NULL, &s_target);
  tcc_ir_put(ir, TCCIR_OP_IJUMP, &s_target, NULL, NULL);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_target, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("indirect_jump_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("indirect_jump_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->ir_op, TCCIR_OP_IJUMP);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_setif_routes_to_setif_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int cmp_val = tcc_ir_vreg_alloc_temp(ir);
  int flag = tcc_ir_vreg_alloc_temp(ir);
  SValue s_cmp = sv_var(cmp_val);
  SValue s_flag = sv_var(flag);
  SValue s_zero = sv_const(0);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_zero, NULL, &s_cmp);
  tcc_ir_put(ir, TCCIR_OP_SETIF, &s_cmp, NULL, &s_flag);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_flag, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("setif_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("setif_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->ir_op, TCCIR_OP_SETIF);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

/* SWITCH_TABLE calls a *different* mop per pass (switch_table_dry_run_size
 * during dry-run vs switch_table_mop during real-run -- unlike the
 * SCRATCH_WRAP'd arithmetic ops, which call the same mop in both passes; see
 * ir/codegen.c ~4035). Forces the non-skip two-pass path (>=12 live
 * temporaries, same construction as
 * test_dispatch_add_agrees_across_dry_and_real_pass in test_codegen_arith.c)
 * so both branches actually run. cgstub's switch_table_mop stub never
 * dereferences the TCCIRSwitchTable*, so the target/default indices below are
 * placeholders -- only num_entries (read for the dry-run size calc) matters. */
UT_TEST(test_dispatch_switch_table_uses_distinct_mop_per_pass)
{
  cgstub_reset();
  cgstub_set_switch_entry_sizes(4, 4);

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

  static int targets[3] = {100, 101, 102};
  TCCIRSwitchTable tables[1];
  tables[0].min_val = 0;
  tables[0].max_val = 2;
  tables[0].default_target = 103;
  tables[0].targets = targets;
  tables[0].num_entries = 3;
  tables[0].table_code_addr = 0;
  ir->switch_tables = tables;
  ir->num_switch_tables = 1;

  SValue s_table_id = sv_const(0);
  tcc_ir_put(ir, TCCIR_OP_SWITCH_TABLE, &s_acc, &s_table_id, NULL);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_acc, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 1); /* two-pass forced */
  /* Called twice per dry-run pass: once as reserve_pool_bytes()'s argument
   * (unconditional, every pass) and once more for the dry-run-only `ind +=`
   * size estimate; the real-run pass only hits the first (unconditional)
   * call site. See ir/codegen.c ~4045-4048. */
  UT_ASSERT_EQ(cgstub_call_count_pass("switch_table_dry_run_size", 0), 2);
  UT_ASSERT_EQ(cgstub_call_count_pass("switch_table_dry_run_size", 1), 1);
  UT_ASSERT_EQ(cgstub_call_count_pass("switch_table_mop", 1), 1);
  UT_ASSERT_EQ(cgstub_call_count_pass("switch_table_mop", 0), 0);

  const CgStubCall *sz = cgstub_nth_call("switch_table_dry_run_size", 0);
  UT_ASSERT(sz != NULL);
  UT_ASSERT_EQ(sz->aux0, 3); /* num_entries */

  /* tcc_ir_free() walks and tcc_free()s switch_tables[i].targets and
   * switch_tables itself (see ir/core.c ~266) -- both point at this test's
   * stack/static arrays, not tcc_malloc'd memory. Detach before freeing,
   * same reason test_opt_switch_collapse.c uses the lighter utb_free(). */
  ir->switch_tables = NULL;
  ir->num_switch_tables = 0;
  tcc_ir_free(ir);
  return 0;
}

/* SWITCH_LOAD: a distinct data-table variant of switch dispatch (dest =
 * loaded value, src1 = index, src2 = value_table_id) -- ir/codegen.c
 * ~4070-4086. A 3-temp function trivially skips the dry-run (see
 * can_skip_dry_run in test_codegen_dispatch_smoke.c), so this fires
 * switch_load_mop directly without needing the two-pass forcing trick
 * test_dispatch_switch_table_uses_distinct_mop_per_pass needed. The stub
 * never dereferences the TCCIRSwitchValueTable* (see codegen_mop_stubs.c),
 * so only num_entries here is meaningful. */
UT_TEST(test_dispatch_switch_load_routes_to_switch_load_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int idx = tcc_ir_vreg_alloc_temp(ir);
  int dest = tcc_ir_vreg_alloc_temp(ir);
  SValue s_idx = sv_var(idx);
  SValue s_dest = sv_var(dest);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_idx);

  TCCIRSwitchValueTable vtabs[1];
  memset(&vtabs[0], 0, sizeof(vtabs[0]));
  vtabs[0].num_entries = 3;
  ir->switch_value_tables = vtabs;
  ir->num_switch_value_tables = 1;

  SValue s_table_id = sv_const(0);
  tcc_ir_put(ir, TCCIR_OP_SWITCH_LOAD, &s_idx, &s_table_id, &s_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("switch_load_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("switch_load_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG);

  /* ir->switch_value_tables points at this test's stack array -- detach
   * before freeing, same reason as the SWITCH_TABLE test above. */
  ir->switch_value_tables = NULL;
  ir->num_switch_value_tables = 0;
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(codegen_control)
{
  UT_RUN(test_jumpif_operands);
  UT_RUN(test_jump_and_ijump_operands);
  UT_RUN(test_diamond_backpatch);
  UT_RUN(test_bb_start);
  UT_RUN(test_switch_operands);
  UT_RUN(test_cmp_jmp_set_empty_stack_is_noop);
  UT_RUN(test_cmp_jmp_set_simple_vt_cmp_emits_single_setif);
  UT_RUN(test_cmp_jmp_set_vt_cmp_merges_pending_jtrue_chain);
  UT_RUN(test_cmp_jmp_set_vt_jmp_emits_default_and_flipped_assign_pair);
  UT_RUN(test_cmp_jmp_set_vt_jmpi_backpatches_real_chain);
  UT_RUN(test_cmp_jmp_set_plain_value_is_noop);
  UT_RUN(test_test_gen_vt_cmp_no_invert_emits_jumpif_and_returns_its_index);
  UT_RUN(test_test_gen_vt_cmp_invert_xors_cmp_op);
  UT_RUN(test_test_gen_plain_value_recurses_through_test_zero);
  UT_RUN(test_test_gen_vt_jmp_matching_invert_adopts_empty_chain);
  UT_RUN(test_test_gen_vt_jmp_matching_invert_merges_nonempty_chain);
  UT_RUN(test_test_gen_vt_jmp_mismatched_invert_emits_jump);
  UT_RUN(test_test_gen_constant_condition_taken_emits_jump_and_sets_nocode);
  UT_RUN(test_test_gen_constant_condition_not_taken_is_noop);
  UT_RUN(test_dispatch_jump_routes_to_jump_mop);
  UT_RUN(test_dispatch_jumpif_routes_to_conditional_jump_mop);
  UT_RUN(test_dispatch_ijump_routes_to_indirect_jump_mop);
  UT_RUN(test_dispatch_setif_routes_to_setif_mop);
  UT_RUN(test_dispatch_switch_table_uses_distinct_mop_per_pass);
  UT_RUN(test_dispatch_switch_load_routes_to_switch_load_mop);
}
