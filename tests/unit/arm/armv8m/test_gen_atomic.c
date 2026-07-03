/*
 *  test_gen_atomic.c - backend/ direct-call tests for the "misc" _mop family:
 *  tcc_gen_machine_trap_mop, tcc_gen_machine_prefetch_mop and
 *  tcc_gen_machine_vla_mop.
 *
 *  Same level as test_gen_dispatch_smoke.c: calls the real arm-thumb-gen.c
 *  _mop functions directly (bypassing ir/codegen.c's dispatch loop) with
 *  hand-built MachineOperand arguments, and asserts on the real emitted
 *  Thumb-2 bytes via the real o()/section_add machinery.
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

static MachineOperand mop_frame_addr(int32_t offset, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_FRAME_ADDR;
  m.btype = btype;
  m.u.frame.offset = offset;
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

static MachineOperand mop_none(void)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_NONE;
  return m;
}

static uint16_t read_le16(const unsigned char *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

/* Thumb-2 32-bit instructions are emitted high-halfword-first: the first
 * 16-bit halfword in memory (each itself little-endian) is the *upper* 16
 * bits of the canonical 32-bit opcode value used in encoding tables (see
 * test_dispatch_jump_forward_uses_32bit_encoding in
 * test_gen_dispatch_smoke.c: opcode 0xf000b800 -> data[0..1]=0xf000,
 * data[2..3]=0xb800). */
static uint32_t read_le32(const unsigned char *p)
{
  return ((uint32_t)read_le16(p) << 16) | (uint32_t)read_le16(p + 2);
}

/* ------------------------------------------------------------------ trap */

UT_TEST(test_trap_mop_emits_udf_t16)
{
  setup_gen();

  /* trap_mop always emits UDF #0xfe -- imm fits in 8 bits so th_udf() picks
   * the T16 encoding 0xDE00 | imm (see test_thop_system.c's
   * test_th_udf_t16 oracle for the 0xDEFF/0xff case; 0xDEFE is the same
   * family for imm=0xfe). */
  tcc_gen_machine_trap_mop();

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xDEFE);

  return 0;
}

/* ------------------------------------------------------------------ prefetch */

UT_TEST(test_prefetch_mop_reg_read_emits_pld)
{
  setup_gen();

  /* addr = R1 (plain register, no deref materialization needed).
   * th_pld_imm(1, 0, 0) => 0xf890f000 | (1<<16) = 0xF891F000
   * (see test_thop_pld.c's test_pld_imm_positive oracle for the encoding
   * shape; offset 0 collapses the U-bit/imm12 field to all zero). */
  tcc_gen_machine_prefetch_mop(mop_reg(R1, IROP_BTYPE_INT32), /*rw=*/0);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le32(cur_text_section->data), 0xF891F000u);

  return 0;
}

UT_TEST(test_prefetch_mop_reg_write_hint_same_encoding)
{
  setup_gen();

  /* rw is documented as ignored by tcc_gen_machine_prefetch_mop ("(void)rw;
   * PLD/PLDW distinction may not be supported on all ARM variants") -- the
   * write-hint call must produce byte-for-byte the same PLD encoding as the
   * read-hint call above, not a PLDW. This test locks in that documented
   * behavior. */
  tcc_gen_machine_prefetch_mop(mop_reg(R2, IROP_BTYPE_INT32), /*rw=*/1);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le32(cur_text_section->data), 0xF892F000u);

  return 0;
}

UT_TEST(test_prefetch_mop_frame_addr_zero_offset_emits_pld_fp)
{
  setup_gen();

  /* FRAME_ADDR with offset==0 takes the direct path: th_pld_imm(R_FP,0,0).
   * R_FP == R7, so opcode = 0xf890f000 | (7<<16) = 0xF897F000. */
  tcc_gen_machine_prefetch_mop(mop_frame_addr(0, IROP_BTYPE_INT32), /*rw=*/0);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le32(cur_text_section->data), 0xF897F000u);

  return 0;
}

/* ------------------------------------------------------------------ vla */

UT_TEST(test_vla_alloc_reg_size_imm_align_emits_sub_bic_mov)
{
  setup_gen();

  /* VLA_ALLOC: src1=size (R2, plain reg -- mach_ensure_in_reg returns it
   * with no extra code), src2=align (IMM 8), dest unused.
   *   SUB r2, SP, r2      (T32: SP as rn has no T16 reg3 form)
   *   BIC r2, r2, #7      (align-1; T32-only, no T16 BIC-immediate)
   *   MOV SP, r2          (T16 high-register MOV)
   * Exact bytes verified empirically against th_sub_reg/th_bic_imm oracle
   * shapes in test_thop_alu_reg.c / test_thop_alu_imm.c. */
  MachineOperand dest = mop_none();
  MachineOperand src1 = mop_reg(R2, IROP_BTYPE_INT32);
  MachineOperand src2 = mop_imm(8, IROP_BTYPE_INT32);

  tcc_gen_machine_vla_mop(dest, src1, src2, TCCIR_OP_VLA_ALLOC);

  UT_ASSERT_EQ(ind, 10);
  UT_ASSERT_EQ(read_le32(cur_text_section->data), 0xEBAD0202u);      /* SUB r2, SP, r2 */
  UT_ASSERT_EQ(read_le32(cur_text_section->data + 4), 0xF0220207u);  /* BIC r2, r2, #7 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 8), 0x4695u);      /* MOV SP, r2 */

  return 0;
}

UT_TEST(test_vla_sp_save_reg_dest_fast_path_emits_mov)
{
  setup_gen();

  /* VLA_SP_SAVE fast path: dest is a plain register (no deref) -> a single
   * MOV dest, SP (high-register T16 form), no scratch push/pop. */
  MachineOperand dest = mop_reg(R2, IROP_BTYPE_INT32);
  MachineOperand src1 = mop_none();
  MachineOperand src2 = mop_none();

  tcc_gen_machine_vla_mop(dest, src1, src2, TCCIR_OP_VLA_SP_SAVE);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x466Au); /* MOV r2, SP */

  return 0;
}

UT_TEST(test_vla_sp_restore_reg_src_emits_mov)
{
  setup_gen();

  /* VLA_SP_RESTORE: src1 is a plain register (mach_ensure_in_reg returns it
   * directly, no code) -> a single MOV SP, src1. */
  MachineOperand dest = mop_none();
  MachineOperand src1 = mop_reg(R3, IROP_BTYPE_INT32);
  MachineOperand src2 = mop_none();

  tcc_gen_machine_vla_mop(dest, src1, src2, TCCIR_OP_VLA_SP_RESTORE);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x469Du); /* MOV SP, r3 */

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(gen_atomic)
{
  UT_RUN(test_trap_mop_emits_udf_t16);
  UT_RUN(test_prefetch_mop_reg_read_emits_pld);
  UT_RUN(test_prefetch_mop_reg_write_hint_same_encoding);
  UT_RUN(test_prefetch_mop_frame_addr_zero_offset_emits_pld_fp);
  UT_RUN(test_vla_alloc_reg_size_imm_align_emits_sub_bic_mov);
  UT_RUN(test_vla_sp_save_reg_dest_fast_path_emits_mov);
  UT_RUN(test_vla_sp_restore_reg_src_emits_mov);
}
