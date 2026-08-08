/*
 *  test_gen_mach_operand.c - unit tests for the public MachineOperand
 *  materialisation helpers tcc_gen_mach_load_to_reg() and
 *  tcc_gen_mach_store_from_reg() in arm-thumb-gen.c (backend/ binary,
 *  build_backend/run_unit_tests_backend).
 *
 *  These entry points are used by arm-thumb-asm.c to move operands into/out of
 *  specific physical registers.  Tests call them directly with hand-built
 *  MachineOperand arguments and assert on the emitted Thumb-2 bytes.
 */

#define USING_GLOBALS
#include "ir.h"
#include "source/backend/arch/arm/arm.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "source/backend/arch/arm/thumb/thop_mem_imm.h"
#include "source/backend/arch/arm/thumb/thop_mov.h"
#include "source/backend/arch/arm/thumb/thop_alu_imm.h"
#include "source/ir/machine_op.h"

extern int offset_to_args;
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

static MachineOperand mop_imm(int64_t val, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_IMM;
  m.btype = btype;
  m.u.imm.val = val;
  return m;
}

static MachineOperand mop_spill(int32_t offset, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_SPILL;
  m.btype = btype;
  m.u.spill.offset = offset;
  return m;
}

static MachineOperand mop_spill_deref(int32_t offset, int btype)
{
  MachineOperand m = mop_spill(offset, btype);
  m.needs_deref = true;
  return m;
}

static MachineOperand mop_frame_addr(int32_t offset, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_FRAME_ADDR;
  m.btype = btype;
  m.u.frame.offset = offset;
  return m;
}

static MachineOperand mop_param_stack(int32_t offset, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_PARAM_STACK;
  m.btype = btype;
  m.u.param.offset = offset;
  return m;
}

static MachineOperand mop_reg64(int r0, int r1, int btype)
{
  MachineOperand m = mop_reg(r0, btype);
  m.is_64bit = true;
  m.u.reg.r1 = r1;
  return m;
}

static uint16_t read_le16(const unsigned char *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

static int bytes_match_opcode_at(int off, int n, thumb_opcode op)
{
  if (n != op.size)
    return 0;
  const unsigned char *d = cur_text_section->data + off;
  if (op.size == 2)
    return d[0] == (op.opcode & 0xff) && d[1] == ((op.opcode >> 8) & 0xff);
  uint16_t hw0 = (uint16_t)(op.opcode >> 16);
  uint16_t hw1 = (uint16_t)(op.opcode & 0xffff);
  return d[0] == (hw0 & 0xff) && d[1] == ((hw0 >> 8) & 0xff) && d[2] == (hw1 & 0xff) &&
         d[3] == ((hw1 >> 8) & 0xff);
}

#define bytes_match_opcode(n, op) bytes_match_opcode_at(0, n, op)

/* ------------------------------------------------------------------ load */

UT_TEST(test_mach_load_reg_same_emits_nothing)
{
  setup_gen();

  /* Source already in destination register: no MOV needed. */
  MachineOperand src = mop_reg(R1, IROP_BTYPE_INT32);
  tcc_gen_mach_load_to_reg(R1, &src);

  UT_ASSERT_EQ(ind, 0);

  return 0;
}

UT_TEST(test_mach_load_reg_diff_emits_mov)
{
  setup_gen();

  MachineOperand src = mop_reg(R1, IROP_BTYPE_INT32);
  tcc_gen_mach_load_to_reg(R0, &src);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_reg(R0, R1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                               ENFORCE_ENCODING_NONE, false)));

  return 0;
}

UT_TEST(test_mach_load_reg_deref_emits_ldr)
{
  setup_gen();

  /* Load [R1] into R0. */
  MachineOperand src = mop_reg_deref(R1, IROP_BTYPE_INT32);
  tcc_gen_mach_load_to_reg(R0, &src);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_ldr_imm(R0, R1, 0, 6 /* add */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_load_imm_emits_mov_imm)
{
  setup_gen();

  MachineOperand src = mop_imm(42, IROP_BTYPE_INT32);
  tcc_gen_mach_load_to_reg(R2, &src);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_imm(R2, 42, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_load_frame_addr_zero_emits_mov_fp)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  MachineOperand src = mop_frame_addr(0, IROP_BTYPE_INT32);
  tcc_gen_mach_load_to_reg(R0, &src);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_reg(R0, R_FP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                               ENFORCE_ENCODING_NONE, false)));

  return 0;
}

UT_TEST(test_mach_load_frame_addr_nonzero_emits_add_fp)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* Offset +4 fits the T1 ADD imm3 encoding (2 bytes). */
  MachineOperand src = mop_frame_addr(4, IROP_BTYPE_INT32);
  tcc_gen_mach_load_to_reg(R0, &src);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_add_imm(R0, R_FP, 4, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_load_param_stack_emits_ldr)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;
  offset_to_args = 0;

  MachineOperand src = mop_param_stack(8, IROP_BTYPE_INT32);
  tcc_gen_mach_load_to_reg(R2, &src);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_ldr_imm(R2, R_FP, 8, 6 /* add */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_load_spill_emits_ldr)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* Negative FP-relative offsets use the subtract (puw=4) form, which falls
   * to a 32-bit T3/T4 encoding for LDR.  Assert bytes match the oracle. */
  MachineOperand src = mop_spill(-8, IROP_BTYPE_INT32);
  tcc_gen_mach_load_to_reg(R0, &src);

  UT_ASSERT(bytes_match_opcode(ind, th_ldr_imm(R0, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_load_spill_deref_emits_double_load)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* VT_LLOCAL: spill slot holds a pointer; load pointer, then load [pointer]. */
  MachineOperand src = mop_spill_deref(-8, IROP_BTYPE_INT32);
  tcc_gen_mach_load_to_reg(R0, &src);

  UT_ASSERT(bytes_match_opcode_at(0, 4, th_ldr_imm(R0, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));
  UT_ASSERT(bytes_match_opcode_at(4, 2, th_ldr_imm(R0, R0, 0, 6 /* add */, ENFORCE_ENCODING_NONE)));

  return 0;
}

/* ----------------------------------------------------------------- store */

UT_TEST(test_mach_store_reg_emits_mov)
{
  setup_gen();

  MachineOperand dest = mop_reg(R0, IROP_BTYPE_INT32);
  tcc_gen_mach_store_from_reg(R1, &dest);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_reg(R0, R1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                               ENFORCE_ENCODING_NONE, false)));

  return 0;
}

UT_TEST(test_mach_store_reg_deref_emits_str)
{
  setup_gen();

  /* Store R2 to [R1]. */
  MachineOperand dest = mop_reg_deref(R1, IROP_BTYPE_INT32);
  tcc_gen_mach_store_from_reg(R2, &dest);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_str_imm(R2, R1, 0, 6 /* add */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_store_spill_emits_str)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* Negative FP-relative offset uses subtract (puw=4), 32-bit encoding. */
  MachineOperand dest = mop_spill(-8, IROP_BTYPE_INT32);
  tcc_gen_mach_store_from_reg(R2, &dest);

  UT_ASSERT(bytes_match_opcode(ind, th_str_imm(R2, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_store_frame_addr_emits_str)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* Negative FP-relative offset uses subtract (puw=4), 32-bit encoding. */
  MachineOperand dest = mop_frame_addr(-8, IROP_BTYPE_INT32);
  tcc_gen_mach_store_from_reg(R2, &dest);

  UT_ASSERT(bytes_match_opcode(ind, th_str_imm(R2, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_store_param_stack_emits_str)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;
  offset_to_args = 0;

  MachineOperand dest = mop_param_stack(8, IROP_BTYPE_INT32);
  tcc_gen_mach_store_from_reg(R2, &dest);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_str_imm(R2, R_FP, 8, 6 /* add */, ENFORCE_ENCODING_NONE)));

  return 0;
}

/* ------------------------------------------------------------------ assign_mop */

UT_TEST(test_mach_assign_zero_extends_32bit_imm_to_64bit_pair)
{
  setup_gen();

  MachineOperand src = mop_imm(42, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg64(R0, R1, IROP_BTYPE_INT32);
  tcc_gen_machine_assign_mop(src, dest, TCCIR_OP_ASSIGN);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT(bytes_match_opcode_at(0, 2, th_mov_imm(R0, 42, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));
  UT_ASSERT(bytes_match_opcode_at(2, 2, th_mov_imm(R1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_assign_truncates_64bit_reg_pair_to_32bit)
{
  setup_gen();

  MachineOperand src = mop_reg64(R2, R3, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_INT32);
  tcc_gen_machine_assign_mop(src, dest, TCCIR_OP_ASSIGN);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_reg(R0, R2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                               ENFORCE_ENCODING_NONE, false)));

  return 0;
}

UT_TEST(test_mach_assign_reg_deref_to_reg_emits_ldr)
{
  setup_gen();

  MachineOperand src = mop_reg_deref(R1, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_INT32);
  tcc_gen_machine_assign_mop(src, dest, TCCIR_OP_ASSIGN);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_ldr_imm(R0, R1, 0, 6 /* add */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_assign_spill_deref_to_reg_emits_double_load)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  MachineOperand src = mop_spill_deref(-8, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_INT32);
  tcc_gen_machine_assign_mop(src, dest, TCCIR_OP_ASSIGN);

  UT_ASSERT(bytes_match_opcode_at(0, 4, th_ldr_imm(R0, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));
  UT_ASSERT(bytes_match_opcode_at(4, 2, th_ldr_imm(R0, R0, 0, 6 /* add */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_mach_assign_frame_addr_zero_to_reg_emits_mov_fp)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  MachineOperand src = mop_frame_addr(0, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_INT32);
  tcc_gen_machine_assign_mop(src, dest, TCCIR_OP_ASSIGN);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_reg(R0, R_FP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                               ENFORCE_ENCODING_NONE, false)));

  return 0;
}

UT_TEST(test_mach_assign_param_stack_to_reg_emits_ldr)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;
  offset_to_args = 0;

  MachineOperand src = mop_param_stack(8, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_INT32);
  tcc_gen_machine_assign_mop(src, dest, TCCIR_OP_ASSIGN);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_ldr_imm(R0, R_FP, 8, 6 /* add */, ENFORCE_ENCODING_NONE)));

  return 0;
}
