/*
 *  test_thop_system.c - suite for arch/arm/thumb/thop_system.c
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_system.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "ut.h"

static void setup_armv8m(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m33",
      .feat = (thop_feat){
          .t16 = 1, .t32 = 1, .it = 1, .mod_imm = 1,
          .movw_movt = 1, .bfx = 1, .clz_rbit = 1,
          .tbb_tbh = 1, .cbz = 1, .sat = 1, .div = 1,
      },
      .is_secure_tz = false,
  };
}

UT_TEST(test_th_nop_t16)
{
  setup_armv8m();
  thumb_opcode op = th_nop(ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xBF00);
  return 0;
}

UT_TEST(test_th_nop_t32)
{
  setup_armv8m();
  thumb_opcode op = th_nop(ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3AF8000);
  return 0;
}

UT_TEST(test_th_sev_t16)
{
  setup_armv8m();
  thumb_opcode op = th_sev(ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xBF40);
  return 0;
}

UT_TEST(test_th_sev_t32)
{
  setup_armv8m();
  thumb_opcode op = th_sev(ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3AF8004);
  return 0;
}

UT_TEST(test_th_wfe_t16)
{
  setup_armv8m();
  thumb_opcode op = th_wfe(ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xBF20);
  return 0;
}

UT_TEST(test_th_wfe_t32)
{
  setup_armv8m();
  thumb_opcode op = th_wfe(ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3AF8002);
  return 0;
}

UT_TEST(test_th_wfi_t16)
{
  setup_armv8m();
  thumb_opcode op = th_wfi(ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xBF30);
  return 0;
}

UT_TEST(test_th_wfi_t32)
{
  setup_armv8m();
  thumb_opcode op = th_wfi(ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3AF8003);
  return 0;
}

UT_TEST(test_th_yield_t16)
{
  setup_armv8m();
  thumb_opcode op = th_yield(ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xBF10);
  return 0;
}

UT_TEST(test_th_yield_t32)
{
  setup_armv8m();
  thumb_opcode op = th_yield(ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3AF8001);
  return 0;
}

UT_TEST(test_th_svc)
{
  setup_armv8m();
  thumb_opcode op = th_svc(0xFF);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xDFFF);
  return 0;
}

UT_TEST(test_th_bkpt)
{
  setup_armv8m();
  thumb_opcode op = th_bkpt(0xAB);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xBEAB);
  return 0;
}

UT_TEST(test_th_udf_t16)
{
  setup_armv8m();
  thumb_opcode op = th_udf(0xFF, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xDEFF);
  return 0;
}

UT_TEST(test_th_udf_t32)
{
  setup_armv8m();
  thumb_opcode op = th_udf(0xABC, ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF7F0AABC);
  return 0;
}

UT_TEST(test_th_cps)
{
  setup_armv8m();
  thumb_opcode op = th_cps(1, 0, 1);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB671);
  return 0;
}

UT_TEST(test_th_cps_zero)
{
  setup_armv8m();
  thumb_opcode op = th_cps(0, 0, 0);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB660);
  return 0;
}

UT_TEST(test_th_clrex)
{
  setup_armv8m();
  thumb_opcode op = th_clrex();
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3BF8F2F);
  return 0;
}

UT_TEST(test_th_csdb)
{
  setup_armv8m();
  thumb_opcode op = th_csdb();
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3AF8014);
  return 0;
}

UT_TEST(test_th_dmb)
{
  setup_armv8m();
  thumb_opcode op = th_dmb(0x5);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3BF8F55);
  return 0;
}

UT_TEST(test_th_dsb)
{
  setup_armv8m();
  thumb_opcode op = th_dsb(0x4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3BF8F44);
  return 0;
}

UT_TEST(test_th_isb)
{
  setup_armv8m();
  thumb_opcode op = th_isb(0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3BF8F66);
  return 0;
}

UT_TEST(test_th_ssbb)
{
  setup_armv8m();
  thumb_opcode op = th_ssbb();
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3BF8F40);
  return 0;
}

UT_TEST(test_th_clz)
{
  setup_armv8m();
  thumb_opcode op = th_clz(1, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFAB2F182);
  return 0;
}
