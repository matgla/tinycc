/*
 *  test_thop_vfp.c - suite for arch/arm/thumb/thop_vfp.c
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_vfp.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "ut.h"

static void setup_armv8m_vfp(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m33",
      .feat = (thop_feat){
          .t16 = 1, .t32 = 1, .it = 1, .mod_imm = 1,
          .movw_movt = 1, .bfx = 1, .clz_rbit = 1,
          .tbb_tbh = 1, .cbz = 1, .sat = 1, .div = 1,
          .vfp_sp = 1, .vfp_dp = 1,
      },
      .is_secure_tz = false,
  };
}

static void setup_no_vfp_sp(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m0",
      .feat = (thop_feat){.t16 = 1, .t32 = 1},
      .is_secure_tz = false,
  };
}

static void setup_no_vfp_dp(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m0",
      .feat = (thop_feat){.t16 = 1, .t32 = 1, .vfp_sp = 1},
      .is_secure_tz = false,
  };
}

/* ═══════════════════════════════════════════════════════════════════
 *  Arithmetic (3-register)
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_th_vadd_f_sp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vadd_f(0, 1, 2, 0); /* S0, S1, S2 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE300A81);
  return 0;
}

UT_TEST(test_th_vadd_f_dp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vadd_f(0, 1, 2, 1); /* D0, D1, D2 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE310B02);
  return 0;
}

UT_TEST(test_th_vadd_f_sp_high_regs)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vadd_f(16, 17, 18, 0); /* S16, S17, S18 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE388A89);
  return 0;
}

UT_TEST(test_th_vadd_f_dp_high_regs)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vadd_f(8, 9, 10, 1); /* D8, D9, D10 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE398B0A);
  return 0;
}

UT_TEST(test_th_vsub_f_sp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vsub_f(0, 1, 2, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE300AC1);
  return 0;
}

UT_TEST(test_th_vsub_f_dp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vsub_f(0, 1, 2, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE310B42);
  return 0;
}

UT_TEST(test_th_vmul_f_sp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vmul_f(0, 1, 2, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE200A81);
  return 0;
}

UT_TEST(test_th_vmul_f_dp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vmul_f(0, 1, 2, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE210B02);
  return 0;
}

UT_TEST(test_th_vdiv_f_sp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vdiv_f(0, 1, 2, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE800A81);
  return 0;
}

UT_TEST(test_th_vdiv_f_dp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vdiv_f(0, 1, 2, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE810B02);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Two-register (vneg, vcmp)
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_th_vneg_f_sp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vneg_f(0, 1, 0); /* S0, S1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB10A60);
  return 0;
}

UT_TEST(test_th_vneg_f_dp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vneg_f(0, 1, 1); /* D0, D1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB10B41);
  return 0;
}

UT_TEST(test_th_vcmp_f_sp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcmp_f(0, 1, 0); /* S0, S1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB40A60);
  return 0;
}

UT_TEST(test_th_vcmp_f_dp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcmp_f(0, 1, 1); /* D0, D1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB40B41);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Register move (vmov_register)
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_th_vmov_register_sp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vmov_register(1, 2, 0); /* S1, S2 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEF00A41);
  return 0;
}

UT_TEST(test_th_vmov_register_dp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vmov_register(0, 1, 1); /* D0, D1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB00B41);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  GPR <-> SP/DP moves
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_th_vmov_gp_sp_to_arm)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vmov_gp_sp(0, 1, 1); /* R0 <- S1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE100A90);
  return 0;
}

UT_TEST(test_th_vmov_gp_sp_from_arm)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vmov_gp_sp(0, 1, 0); /* S1 <- R0 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE000A90);
  return 0;
}

UT_TEST(test_th_vmov_gp_sp_high_reg)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vmov_gp_sp(12, 31, 1); /* R12 <- S31 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE1FCA90);
  return 0;
}

UT_TEST(test_th_vmov_2gp_dp_to_arm)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vmov_2gp_dp(0, 1, 0, 1); /* R0, R1 <- D0 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEC510B10);
  return 0;
}

UT_TEST(test_th_vmov_2gp_dp_from_arm)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vmov_2gp_dp(0, 1, 0, 0); /* D0 <- R0, R1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEC410B10);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  System (vmrs)
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_th_vmrs)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vmrs(0); /* R0 <- FPSCR */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEF10A10);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Conversions (SP <-> DP)
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_th_vcvt_float_to_double)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_float_to_double(0, 1); /* D0 <- S1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB70AE0);
  return 0;
}

UT_TEST(test_th_vcvt_double_to_float)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_double_to_float(0, 1); /* S0 <- D1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB70BC1);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Conversions (floating-point <-> integer)
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_th_vcvt_fp_int_s32_f32)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_fp_int(0, 1, 5, 0, 1); /* s32.f32 S0, S1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEBD0AE0);
  return 0;
}

UT_TEST(test_th_vcvt_fp_int_u32_f32)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_fp_int(0, 1, 4, 0, 1); /* u32.f32 S0, S1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEBC0AE0);
  return 0;
}

UT_TEST(test_th_vcvt_fp_int_f32_s32)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_fp_int(0, 1, 0, 0, 1); /* f32.s32 S0, S1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB80AE0);
  return 0;
}

UT_TEST(test_th_vcvt_fp_int_f32_u32)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_fp_int(0, 1, 0, 0, 0); /* f32.u32 S0, S1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB80A60);
  return 0;
}

UT_TEST(test_th_vcvt_fp_int_s32_f64)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_fp_int(0, 1, 5, 1, 1); /* s32.f64 S0, D1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEBD0BC1);
  return 0;
}

UT_TEST(test_th_vcvt_fp_int_f64_s32)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_fp_int(0, 1, 0, 1, 1); /* f64.s32 D0, S1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB80BE0);
  return 0;
}

UT_TEST(test_th_vcvt_fp_int_f64_u32)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_fp_int(0, 1, 0, 1, 0); /* f64.u32 D0, S1 */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB80B60);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  th_vcvt_convert wrapper
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_th_vcvt_convert_s32_f32)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_convert(0, 1, "s32", "f32");
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEBD0AE0);
  return 0;
}

UT_TEST(test_th_vcvt_convert_f64_f32)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_convert(0, 1, "f64", "f32");
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB70AE0);
  return 0;
}

UT_TEST(test_th_vcvt_convert_f32_s32)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_convert(0, 1, "f32", "s32");
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEB80AE0);
  return 0;
}

UT_TEST(test_th_vcvt_convert_u32_f64)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_convert(0, 1, "u32", "f64");
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEEBC0BC1);
  return 0;
}

UT_TEST(test_th_vcvt_convert_unknown)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vcvt_convert(0, 1, "xxx", "yyy");
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Push / Pop
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_th_vpush_sp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vpush(0x0F, 0); /* {S0-S3} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xED2D0A04);
  return 0;
}

UT_TEST(test_th_vpush_sp_high)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vpush(0x0F0000, 0); /* {S16-S19} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xED2D8A04);
  return 0;
}

UT_TEST(test_th_vpush_dp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vpush(0x0F, 1); /* {D0-D3} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xED2D0B08);
  return 0;
}

UT_TEST(test_th_vpush_dp_high)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vpush(0x0F00, 1); /* {D8-D11} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xED2D8B08);
  return 0;
}

UT_TEST(test_th_vpop_sp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vpop(0x0F, 0); /* {S0-S3} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xECBD0A04);
  return 0;
}

UT_TEST(test_th_vpop_dp)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vpop(0x0F, 1); /* {D0-D3} */
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xECBD0B08);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Feature gates
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_vfp_sp_blocked_without_feat)
{
  setup_no_vfp_sp();
  thumb_opcode op = th_vadd_f(0, 1, 2, 0);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_vfp_dp_blocked_without_feat)
{
  setup_no_vfp_dp();
  thumb_opcode op = th_vadd_f(0, 1, 2, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_vfp_sp_allowed_with_feat)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vadd_f(0, 1, 2, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE300A81);
  return 0;
}

UT_TEST(test_vfp_dp_allowed_with_feat)
{
  setup_armv8m_vfp();
  thumb_opcode op = th_vadd_f(0, 1, 2, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE310B02);
  return 0;
}
