/*
 *  test_gen_dispatch_smoke.c - Phase 0 feasibility spike for the backend/
 *  binary (build_backend/run_unit_tests_backend).
 *
 *  Proves the codegen_backend_stubs.c link works and that
 *  tcc_gen_machine_*_mop functions can be called DIRECTLY (bypassing
 *  ir/codegen.c's dispatch loop entirely) with hand-built MachineOperand
 *  arguments, emitting real Thumb-2 bytes into a real Section via the real
 *  o()/section_add machinery. No IR, no dispatch loop, no frontend --
 *  see test_thop_alu_reg.c for the analogous "call the low-level encoder
 *  directly, assert on the exact opcode" style this mirrors one level up.
 */

#define USING_GLOBALS
#include "ir.h"
#include "arch/arm/arm.h"
#include "arch/arm/thumb/thumb.h"
#include "ir/machine_op.h"
#include "codegen_backend_stubs.h"
#include "elfsec_stubs.h"

#include "ut.h"

static void setup_gen(void)
{
  elfsec_reset();
  cgb_reset();
  arm_target_init("armv8-m.main", NULL, "cortex-m33", 0);
  cur_text_section = elfsec_new_section(".text");
  ind = 0;
  tcc_state->registers_for_allocator = 13;
  tcc_state->float_abi = ARM_HARD_FLOAT;
}

static MachineOperand mop_reg(int r, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_REG;
  m.btype = btype;
  m.u.reg.r0 = r;
  m.u.reg.r1 = -1;
  return m;
}

static MachineOperand mop_reg_deref(int r, int btype)
{
  MachineOperand m = mop_reg(r, btype);
  m.needs_deref = true;
  return m;
}

static uint16_t read_le16(const unsigned char *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

/* ------------------------------------------------------------------ arith */

UT_TEST(test_dispatch_add_reg_reg_reg_emits_real_bytes)
{
  setup_gen();

  /* ADD R0, R1, R2 -> real Thumb-1 T1 encoding 0x1888. */
  tcc_gen_machine_data_processing_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                      mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_ADD, 0);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x1888);

  return 0;
}

/* ------------------------------------------------------------------ mem */

UT_TEST(test_dispatch_load_reg_offset_zero_emits_real_bytes)
{
  setup_gen();

  /* LDR R3, [R1] -> real Thumb-1 T1 encoding 0x680b. */
  tcc_gen_machine_load_mop(mop_reg_deref(R1, IROP_BTYPE_INT32), mop_reg(R3, IROP_BTYPE_INT32), TCCIR_OP_LOAD);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x680b);

  return 0;
}

UT_TEST(test_dispatch_store_reg_offset_zero_emits_real_bytes)
{
  setup_gen();

  /* STR R3, [R1] -> real Thumb-1 T1 encoding 0x600b. */
  tcc_gen_machine_store_mop(mop_reg_deref(R1, IROP_BTYPE_INT32), mop_reg(R3, IROP_BTYPE_INT32), TCCIR_OP_STORE);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x600b);

  return 0;
}

/* ------------------------------------------------------------------ branch */

UT_TEST(test_dispatch_jump_forward_uses_32bit_encoding)
{
  setup_gen();

  /* A forward branch (target_ir >= ir_idx, and/or tcc_state->ir unset) can't
   * be proven backward-narrowable, so tcc_gen_machine_jump_mop must choose
   * the 32-bit B.W T4 form: first halfword 0xf000, second 0xb800. */
  int size = tcc_gen_machine_jump_mop(TCCIR_OP_JUMP, /*target_ir=*/5, /*ir_idx=*/0);

  UT_ASSERT_EQ(size, 4);
  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xf000);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xb800);

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(gen_dispatch_smoke)
{
  UT_RUN(test_dispatch_add_reg_reg_reg_emits_real_bytes);
  UT_RUN(test_dispatch_load_reg_offset_zero_emits_real_bytes);
  UT_RUN(test_dispatch_store_reg_offset_zero_emits_real_bytes);
  UT_RUN(test_dispatch_jump_forward_uses_32bit_encoding);
}
