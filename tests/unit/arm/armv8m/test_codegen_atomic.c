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
#include "source/backend/arch/arm/arm_regalloc.h"
#include "codegen_mop_stubs.h"
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

/* ============================================================================
 * Dispatch-level tests (tcc_ir_codegen_generate) -- "misc" op family
 *
 * See test_codegen_arith.c's dispatch-level section header for the overall
 * rationale. TRAP/PREFETCH/SET_CHAIN/VLA_ALLOC/SETJMP/LONGJMP each have their
 * own case label in ir/codegen.c (~4111-4166). SETJMP/LONGJMP's actual libc
 * jmp_buf semantics aren't modeled -- these tests only check IR-op ->
 * mop-call routing, same as every other dispatch test in this project.
 * ============================================================================ */

UT_TEST(test_dispatch_trap_routes_to_trap_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  tcc_ir_put(ir, TCCIR_OP_TRAP, NULL, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  ir->noreturn = 1; /* TRAP never falls through */
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("trap_mop"), 1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_prefetch_routes_to_prefetch_mop_with_rw_hint)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int ptr = tcc_ir_vreg_alloc_temp(ir);
  SValue s_ptr = sv_var(ptr);
  SValue s_seven = sv_const(7);
  SValue s_rw_write = sv_const(1); /* 1 = write (PLDW) */
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_seven, NULL, &s_ptr);
  tcc_ir_put(ir, TCCIR_OP_PREFETCH, &s_ptr, &s_rw_write, NULL);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_ptr, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("prefetch_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("prefetch_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->aux0, 1); /* rw hint */
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_set_chain_routes_to_set_chain)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v = sv_var(v);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v);
  tcc_ir_put(ir, TCCIR_OP_SET_CHAIN, NULL, NULL, NULL);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("set_chain"), 1);

  tcc_ir_free(ir);
  return 0;
}

/* INIT_CHAIN_SLOT (ir/codegen.c ~4176-4178): src1 normally carries a SYMREF
 * (the chain slot symbol) rather than a vreg -- but neither the dispatch nor
 * the stub inspects the operand's tag, only its vreg (irop_get_vreg), so a
 * plain vreg operand exercises the same dispatch line without needing a real
 * Sym* (which stubs.c's always-NULL sym_push2/external_global_sym block, the
 * same reason BLOCK_COPY is out of scope -- see
 * docs/plan_codegen_unit_tests.md §9).
 *
 * ASM_INPUT/ASM_OUTPUT (~4179-4181) are no-op case labels (real inline-asm
 * handling lives elsewhere); this just confirms dispatch reaches their
 * `break` without misrouting to any mop. */
UT_TEST(test_dispatch_init_chain_slot_and_asm_noops)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v = sv_var(v);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v);

  tcc_ir_put(ir, TCCIR_OP_INIT_CHAIN_SLOT, &s_v, NULL, NULL);
  tcc_ir_put(ir, TCCIR_OP_ASM_INPUT, &s_v, NULL, NULL);
  tcc_ir_put(ir, TCCIR_OP_ASM_OUTPUT, NULL, NULL, &s_v);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("init_chain_slot"), 1);
  const CgStubCall *c = cgstub_nth_call("init_chain_slot", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->src1_vreg, v);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_vla_alloc_routes_to_vla_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int size = tcc_ir_vreg_alloc_temp(ir);
  int addr = tcc_ir_vreg_alloc_temp(ir);
  SValue s_size = sv_var(size);
  SValue s_addr = sv_var(addr);
  SValue s_sixteen = sv_const(16);
  SValue s_align = sv_const(8);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_sixteen, NULL, &s_size);
  tcc_ir_put(ir, TCCIR_OP_VLA_ALLOC, &s_size, &s_align, &s_addr);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_addr, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("vla_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("vla_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->ir_op, TCCIR_OP_VLA_ALLOC);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * Dispatch-level tests -- setjmp/longjmp/__builtin_apply family
 *
 * SETJMP/LONGJMP/NL_SETJMP/NL_LONGJMP/BUILTIN_APPLY_ARGS/BUILTIN_APPLY each
 * have their own case label in ir/codegen.c (~4121-4157). As with
 * SETJMP/LONGJMP above, real jmp_buf/callee-saved-registers/argument-block
 * semantics aren't modeled -- these only check IR-op -> mop-call routing.
 * LONGJMP/NL_LONGJMP never fall through (no RETURNVALUE follows), same as
 * the TRAP test above.
 * ============================================================================ */

UT_TEST(test_dispatch_setjmp_routes_to_setjmp_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int buf = tcc_ir_vreg_alloc_temp(ir);
  int area = tcc_ir_vreg_alloc_temp(ir);
  int dest = tcc_ir_vreg_alloc_temp(ir);
  SValue s_buf = sv_var(buf);
  SValue s_area = sv_var(area);
  SValue s_dest = sv_var(dest);
  SValue s_buf_addr = sv_const(0x1000);
  SValue s_area_addr = sv_const(0x2000);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_buf_addr, NULL, &s_buf);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_area_addr, NULL, &s_area);
  tcc_ir_put(ir, TCCIR_OP_SETJMP, &s_buf, &s_area, &s_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("setjmp_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("setjmp_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG); /* buf */
  UT_ASSERT_EQ(c->src2_kind, MACH_OP_REG); /* area */

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_longjmp_routes_to_longjmp_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int buf = tcc_ir_vreg_alloc_temp(ir);
  SValue s_buf = sv_var(buf);
  SValue s_buf_addr = sv_const(0x1000);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_buf_addr, NULL, &s_buf);
  tcc_ir_put(ir, TCCIR_OP_LONGJMP, &s_buf, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  ir->noreturn = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("longjmp_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("longjmp_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_nl_setjmp_routes_to_nl_setjmp_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int buf = tcc_ir_vreg_alloc_temp(ir);
  int dest = tcc_ir_vreg_alloc_temp(ir);
  SValue s_buf = sv_var(buf);
  SValue s_dest = sv_var(dest);
  SValue s_buf_addr = sv_const(0x1000);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_buf_addr, NULL, &s_buf);
  tcc_ir_put(ir, TCCIR_OP_NL_SETJMP, &s_buf, NULL, &s_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("nl_setjmp_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("nl_setjmp_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_nl_longjmp_routes_to_nl_longjmp_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int buf = tcc_ir_vreg_alloc_temp(ir);
  SValue s_buf = sv_var(buf);
  SValue s_buf_addr = sv_const(0x1000);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_buf_addr, NULL, &s_buf);
  tcc_ir_put(ir, TCCIR_OP_NL_LONGJMP, &s_buf, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  ir->noreturn = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("nl_longjmp_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("nl_longjmp_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_builtin_apply_args_routes_to_builtin_apply_args_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int dest = tcc_ir_vreg_alloc_temp(ir);
  SValue s_dest = sv_var(dest);
  tcc_ir_put(ir, TCCIR_OP_BUILTIN_APPLY_ARGS, NULL, NULL, &s_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("builtin_apply_args_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("builtin_apply_args_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_builtin_apply_routes_to_builtin_apply_mop)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int fn = tcc_ir_vreg_alloc_temp(ir);
  int args = tcc_ir_vreg_alloc_temp(ir);
  int dest = tcc_ir_vreg_alloc_temp(ir);
  SValue s_fn = sv_var(fn);
  SValue s_args = sv_var(args);
  SValue s_dest = sv_var(dest);
  SValue s_fn_addr = sv_const(0x1000);
  SValue s_args_addr = sv_const(0x2000);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_fn_addr, NULL, &s_fn);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_args_addr, NULL, &s_args);
  tcc_ir_put(ir, TCCIR_OP_BUILTIN_APPLY, &s_fn, &s_args, &s_dest);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_dest, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("builtin_apply_mop"), 1);
  const CgStubCall *c = cgstub_nth_call("builtin_apply_mop", 0);
  UT_ASSERT(c != NULL);
  UT_ASSERT_EQ(c->dest_kind, MACH_OP_REG);
  UT_ASSERT_EQ(c->src1_kind, MACH_OP_REG); /* fn */
  UT_ASSERT_EQ(c->src2_kind, MACH_OP_REG); /* args */

  tcc_ir_free(ir);
  return 0;
}
