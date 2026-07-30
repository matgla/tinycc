/*
 *  test_thop_block.c - suite for arch/arm/thumb/thop_block.c
 *
 *  Tests PUSH, POP, LDM, STM, LDMDB, STMDB encodings.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_block.h"
#include "source/backend/arch/arm/thumb/thumb.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

static void setup_armv7m(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m3",
      .feat = (thop_feat){
          .t16 = 1,
          .t32 = 1,
          .it = 1,
          .mod_imm = 1,
          .movw_movt = 1,
          .bfx = 1,
          .clz_rbit = 1,
          .tbb_tbh = 1,
          .cbz = 1,
          .sat = 1,
          .div = 1,
      },
      .is_secure_tz = false,
  };
}

/* ------------------------------------------------------------------ tests */

UT_TEST(test_push_t1_basic)
{
  setup_armv7m();
  thumb_opcode op = th_push(0x05); /* {r0, r2} */
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB405);
  return 0;
}

UT_TEST(test_push_t1_with_lr)
{
  setup_armv7m();
  thumb_opcode op = th_push((1u << R_LR) | 0x05); /* {r0, r2, lr} */
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB505);
  return 0;
}

UT_TEST(test_push_t2)
{
  setup_armv7m();
  /* r0-r12 + LR = 0x1FFF + bit 14 */
  thumb_opcode op = th_push(0x1FFF | (1u << R_LR));
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE92D5FFF);
  return 0;
}

UT_TEST(test_pop_t1_basic)
{
  setup_armv7m();
  thumb_opcode op = th_pop(0x05); /* {r0, r2} */
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xBC05);
  return 0;
}

UT_TEST(test_pop_t1_with_pc)
{
  setup_armv7m();
  thumb_opcode op = th_pop(0x05 | (1u << R_PC)); /* {r0, r2, pc} */
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xBD05);
  return 0;
}

UT_TEST(test_pop_t2_with_pc)
{
  setup_armv7m();
  /* r0-r12 + PC */
  thumb_opcode op = th_pop(0x1FFF | (1u << R_PC));
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8BD9FFF);
  return 0;
}

UT_TEST(test_pop_t2_with_lr)
{
  setup_armv7m();
  /* r0-r12 + LR */
  thumb_opcode op = th_pop(0x1FFF | (1u << R_LR));
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8BD5FFF);
  return 0;
}

UT_TEST(test_ldm_t1)
{
  setup_armv7m();
  /* T1: low rn, low regset, writeback */
  thumb_opcode op = th_ldm(0, 0x06, 1, ENFORCE_ENCODING_NONE); /* ldm r0!, {r1, r2} */
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xC806);
  return 0;
}

UT_TEST(test_ldm_t3)
{
  setup_armv7m();
  /* T3: high rn forces wide encoding */
  thumb_opcode op = th_ldm(8, 0x05, 1, ENFORCE_ENCODING_NONE); /* ldm r8!, {r0, r2} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8B80005);
  return 0;
}

UT_TEST(test_ldm_sp_delegates_to_pop)
{
  setup_armv7m();
  /* rn=SP with writeback should delegate to POP T1 */
  thumb_opcode op = th_ldm(R_SP, 0x05, 1, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xBC05);
  return 0;
}

UT_TEST(test_ldm_sp_wide_delegates_to_pop_t2)
{
  setup_armv7m();
  /* rn=SP with wide regset + PC should delegate to POP T2 */
  thumb_opcode op = th_ldm(R_SP, 0x1FFF | (1u << R_PC), 1, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8BD9FFF);
  return 0;
}

UT_TEST(test_stm_t1)
{
  setup_armv7m();
  /* T1: low rn, low regset, writeback */
  thumb_opcode op = th_stm(0, 0x06, 1, ENFORCE_ENCODING_NONE); /* stm r0!, {r1, r2} */
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xC006);
  return 0;
}

UT_TEST(test_stm_no_writeback_forces_t32)
{
  setup_armv7m();
  /* No writeback forces T3 even with low rn and low regset */
  thumb_opcode op = th_stm(0, 0x06, 0, ENFORCE_ENCODING_NONE); /* stm r0, {r1, r2} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8800006);
  return 0;
}

UT_TEST(test_stm_t3)
{
  setup_armv7m();
  /* T3: high rn forces wide encoding */
  thumb_opcode op = th_stm(8, 0x05, 1, ENFORCE_ENCODING_NONE); /* stm r8!, {r0, r2} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8A80005);
  return 0;
}

UT_TEST(test_ldmdb_basic)
{
  setup_armv7m();
  thumb_opcode op = th_ldmdb(R_SP, 0x03, 1); /* ldmdb sp!, {r0, r1} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE93D0003);
  return 0;
}

UT_TEST(test_ldmdb_no_writeback)
{
  setup_armv7m();
  thumb_opcode op = th_ldmdb(R_SP, 0x03, 0); /* ldmdb sp, {r0, r1} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE91D0003);
  return 0;
}

UT_TEST(test_stmdb_basic)
{
  setup_armv7m();
  thumb_opcode op = th_stmdb(R_SP, 0x03, 1, ENFORCE_ENCODING_NONE); /* stmdb sp!, {r0, r1} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE92D0003);
  return 0;
}

UT_TEST(test_stmdb_no_writeback)
{
  setup_armv7m();
  thumb_opcode op = th_stmdb(R_SP, 0x03, 0, ENFORCE_ENCODING_NONE); /* stmdb sp, {r0, r1} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE90D0003);
  return 0;
}

UT_TEST(test_ldm_exclude_bit)
{
  setup_armv7m();
  /* When rn is in regset, exclude_bit clears it from raw placement */
  thumb_opcode op = th_ldm(0, 0x05, 1, ENFORCE_ENCODING_NONE); /* ldm r0!, {r0, r2} */
  UT_ASSERT_EQ(op.size, 2);
  /* r0 bit cleared from raw placement -> only r2 remains -> 0xC804 */
  UT_ASSERT_EQ(op.opcode, 0xC804);
  return 0;
}

UT_TEST(test_stm_exclude_bit)
{
  setup_armv7m();
  /* When rn is in regset, exclude_bit clears it from raw placement */
  thumb_opcode op = th_stm(0, 0x05, 1, ENFORCE_ENCODING_NONE); /* stm r0!, {r0, r2} */
  UT_ASSERT_EQ(op.size, 2);
  /* r0 bit cleared from raw placement -> only r2 remains -> 0xC004 */
  UT_ASSERT_EQ(op.opcode, 0xC004);
  return 0;
}
