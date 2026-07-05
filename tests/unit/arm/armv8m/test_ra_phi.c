/*
 *  test_ra_phi.c - suite for ir/regalloc.c phi resolution coverage
 *
 *  Exercises phi copy insertion and the allocation of phi destinations
 *  in a small diamond CFG.  The tests build raw IR and let
 *  tcc_ir_ssa_regalloc construct SSA, resolve phis, and allocate
 *  registers internally.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ir/ssa.h"
#include "ir/vreg.h"
#include "ir/regalloc.h"
#include "arch/arm/arm_regalloc.h"
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

static void run_regalloc(TCCIRState *ir)
{
  setup_tcc_state();
  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
}

/*
 * Build a diamond CFG with a phi at the merge block:
 *
 *   v1 = p0; v2 = p1;
 *   if (0) goto else;
 * then:
 *   v0 = v1;
 *   goto merge;
 * else:
 *   v0 = v2;
 * merge:
 *   return v0;
 *
 * Parameters are used for the phi operands so that -O0 constant
 * propagation cannot fold the phi away.
 */
static TCCIRState *build_diamond_phi(void)
{
  TCCIRState *ir = tcc_ir_alloc();
  int p0 = tcc_ir_vreg_alloc_param(ir);
  int p1 = tcc_ir_vreg_alloc_param(ir);
  int v0 = tcc_ir_vreg_alloc_var(ir);
  int v1 = tcc_ir_vreg_alloc_var(ir);
  int v2 = tcc_ir_vreg_alloc_var(ir);

  SValue s_p0 = sv_var(p0);
  SValue s_p1 = sv_var(p1);
  SValue s_v0 = sv_var(v0);
  SValue s_v1 = sv_var(v1);
  SValue s_v2 = sv_var(v2);
  SValue s_zero = sv_const(0);
  SValue j_else = sv_jump_target(5);
  SValue j_merge = sv_jump_target(6);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_p0, NULL, &s_v1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_p1, NULL, &s_v2);
  tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_zero, NULL, &j_else);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_v1, NULL, &s_v0);
  tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &j_merge);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_v2, NULL, &s_v0);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v0, NULL, NULL);

  return ir;
}

static int count_assigns(const TCCIRState *ir)
{
  int n = 0;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_ASSIGN)
      n++;
  }
  return n;
}

/* -------------------------------------------------------------------------- */
/* Phi destination receives a valid allocation                                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_phi_diamond_allocation)
{
  TCCIRState *ir = build_diamond_phi();
  UT_ASSERT(ir != NULL);

  int before = count_assigns(ir);
  run_regalloc(ir);
  int after = count_assigns(ir);

  /* Phi resolution must have inserted at least one copy. */
  UT_ASSERT(after > before);

  /* The value returned from the merge block must have a valid allocation. */
  int found = 0;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_RETURNVALUE)
      continue;
    IROperand src = tcc_ir_op_get_src1(ir, q);
    int vr = irop_get_vreg(src);
    if (vr < 0)
      continue;
    IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, vr);
    UT_ASSERT(li != NULL);
    int valid = (li->allocation.offset != 0) || (li->allocation.r0 < PREG_NONE);
    UT_ASSERT(valid);
    found = 1;
  }
  UT_ASSERT(found);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Phi resolution inserts explicit ASSIGN copies at predecessor block ends    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_phi_copies_inserted)
{
  TCCIRState *ir = build_diamond_phi();
  UT_ASSERT(ir != NULL);

  int assigns_before = count_assigns(ir);
  run_regalloc(ir);
  int assigns_after = count_assigns(ir);

  /* Pre-RA phi resolution turns the implicit phi into explicit ASSIGN copies
   * at the predecessor block ends, so the number of ASSIGN instructions grows. */
  UT_ASSERT(assigns_after > assigns_before);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Phi destination live interval covers the merge block                       */
/* -------------------------------------------------------------------------- */

UT_TEST(test_phi_dest_liveness)
{
  TCCIRState *ir = build_diamond_phi();
  UT_ASSERT(ir != NULL);

  run_regalloc(ir);

  int merge_instr = -1;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_RETURNVALUE) {
      merge_instr = i;
      break;
    }
  }
  UT_ASSERT(merge_instr >= 0);

  IROperand ret_src = tcc_ir_op_get_src1(ir, &ir->compact_instructions[merge_instr]);
  int32_t ret_vr = irop_get_vreg(ret_src);
  UT_ASSERT(ret_vr >= 0);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, ret_vr);
  UT_ASSERT(li != NULL);
  UT_ASSERT(li->start != INTERVAL_NOT_STARTED);
  UT_ASSERT(li->end >= li->start);
  UT_ASSERT(li->start <= (uint32_t)merge_instr);
  UT_ASSERT(li->end >= (uint32_t)merge_instr);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(ra_phi)
{
  UT_RUN(test_phi_diamond_allocation);
  UT_RUN(test_phi_copies_inserted);
  UT_RUN(test_phi_dest_liveness);
}
