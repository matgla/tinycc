/*
 *  test_codegen_atomic.c - backend unit tests for atomic/exclusive codegen paths
 *
 *  There are no dedicated atomic IR ops; atomics are lowered via inline asm /
 *  exclusive thops.  This suite covers the machine-interface registration in
 *  tccmachine.c and the machine-operand lowering paths used by atomic-style
 *  memory accesses (spill slots, parameter stack, double-indirection llocals).
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ir/ssa.h"
#include "ir/vreg.h"
#include "ir/regalloc.h"
#include "ir/codegen.h"
#include "ir/machine_op.h"
#include "tccmachine.h"
#include "arch/arm/arm_regalloc.h"
#include "ut.h"

/* Declared in tccmachine.c but not exported in tccmachine.h. */
extern void tcc_machine_init_defaults(void);

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

/* -------------------------------------------------------------------------- */
/* tccmachine interface registration and defaults                              */
/* -------------------------------------------------------------------------- */

static int mock_init_called = 0;
static int mock_cleanup_called = 0;

static void mock_init(void)
{
  mock_init_called++;
}

static void mock_cleanup(void)
{
  mock_cleanup_called++;
}

static TCCScratchHandle *mock_acquire_scratch(unsigned flags, uint32_t exclude_regs)
{
  (void)flags;
  (void)exclude_regs;
  return NULL;
}

static void mock_release_scratch(TCCScratchHandle *handle)
{
  (void)handle;
}

static int mock_scratch_get_reg(TCCScratchHandle *handle, int idx)
{
  (void)handle;
  (void)idx;
  return 0;
}

static int mock_can_encode_directly(TCCIRState *ir, const TCCMatRequest *req)
{
  (void)ir;
  (void)req;
  return 1;
}

static int mock_materialize(TCCIRState *ir, const TCCMatRequest *req, TCCMatResult *result)
{
  (void)ir;
  (void)req;
  if (!result)
    return 0;
  result->success = 1;
  result->reg = 0;
  return 1;
}

static int mock_get_spill_offset(TCCIRState *ir, int vreg)
{
  (void)ir;
  (void)vreg;
  return 4;
}

static int mock_get_stack_align(void)
{
  return 8;
}

static const TCCMachineInterface mock_machine_interface = {
  .init = mock_init,
  .cleanup = mock_cleanup,
  .acquire_scratch = mock_acquire_scratch,
  .release_scratch = mock_release_scratch,
  .scratch_get_reg = mock_scratch_get_reg,
  .can_encode_directly = mock_can_encode_directly,
  .materialize = mock_materialize,
  .get_spill_offset = mock_get_spill_offset,
  .get_stack_align = mock_get_stack_align,
};

UT_TEST(test_machine_interface_registration)
{
  mock_init_called = 0;
  mock_cleanup_called = 0;

  UT_ASSERT(tcc_machine_get() != &mock_machine_interface);
  tcc_machine_register(&mock_machine_interface);
  UT_ASSERT(tcc_machine_get() == &mock_machine_interface);
  UT_ASSERT_EQ(mock_init_called, 1);

  /* Materialize compat reaches the mock backend. */
  TCCMatResult result;
  UT_ASSERT(tcc_machine_materialize_spill_compat(NULL, 8, 0, &result));
  UT_ASSERT(result.success);

  /* Default alignment query. */
  UT_ASSERT_EQ(tcc_machine_get_stack_align_ex(), 8);

  /* Register again to trigger cleanup/init chain if implemented. */
  tcc_machine_register(&mock_machine_interface);
  UT_ASSERT_EQ(mock_init_called, 2);

  tcc_machine_register(NULL);
  return 0;
}

UT_TEST(test_machine_defaults)
{
  tcc_machine_register(NULL);
  tcc_machine_init_defaults();

  UT_ASSERT(tcc_machine_get() != NULL);
  UT_ASSERT_EQ(tcc_machine_get_stack_align_ex(), 8);

  TCCScratchHandle *h = tcc_machine_acquire_scratch_compat(0, 0);
  UT_ASSERT(h == NULL);

  TCCMatResult result;
  memset(&result, 0, sizeof(result));
  UT_ASSERT(!tcc_machine_materialize_spill_compat(NULL, 8, 0, &result));

  return 0;
}

/* -------------------------------------------------------------------------- */
/* Atomic-style memory operands: spilled pointer with double indirection      */
/* -------------------------------------------------------------------------- */

UT_TEST(test_atomic_style_llocal_operand)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int t = tcc_ir_vreg_alloc_temp(ir);
  SValue s_t = sv_var(t);
  SValue s_zero = sv_const(0);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_zero, NULL, &s_t);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_t, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, t);
  UT_ASSERT(li != NULL);

  /* Construct a stackoff operand that represents a spilled pointer with
   * double indirection (the value loaded from the spill slot is itself an
   * address that must be dereferenced).  This is the shape used by atomic
   * loads/stores when the pointer has been spilled. */
  IROperand op = irop_make_stackoff(-1, li->allocation.offset, 1, 1, 0, IROP_BTYPE_INT32);
  MachineOperand m = machine_op_from_ir(ir, &op);

  UT_ASSERT(m.kind == MACH_OP_SPILL);
  UT_ASSERT(m.needs_deref);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Stack-passed parameter operand used as atomic pointer                       */
/* -------------------------------------------------------------------------- */

UT_TEST(test_atomic_style_param_operand)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p = tcc_ir_vreg_alloc_param(ir);
  SValue s_p = sv_var(p);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_p, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  tcc_ir_codegen_params_setup(ir);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, p);
  UT_ASSERT(li != NULL);

  /* A stack-passed pointer parameter appears as PARAM_STACK with lval set. */
  IROperand op = irop_make_stackoff(-1, li->original_offset, 1, 0, 1, IROP_BTYPE_INT32);
  op.is_param = 1;
  MachineOperand m = machine_op_from_ir(ir, &op);

  UT_ASSERT(m.kind == MACH_OP_PARAM_STACK);
  UT_ASSERT(m.needs_deref);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(codegen_atomic)
{
  UT_RUN(test_machine_interface_registration);
  UT_RUN(test_machine_defaults);
  UT_RUN(test_atomic_style_llocal_operand);
  UT_RUN(test_atomic_style_param_operand);
}
