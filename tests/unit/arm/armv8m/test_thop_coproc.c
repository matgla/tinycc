/*
 *  test_thop_coproc.c - suite for arch/arm/thumb/thop_coproc.c
 *
 *  Every expected encoding in this file was produced by
 *    arm-none-eabi-as -mcpu=cortex-m33 -mthumb
 *  and cross-checked against the RP2350 DCP encoding table in
 *    tests/benchmarks/libs/pico-sdk/tools/copro_dis.py
 *  so the vectors are independent of the encoder under test.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_coproc.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "ut.h"

static void setup_armv8m_main(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m33",
      .feat = (thop_feat){
          .t16 = 1, .t32 = 1, .it = 1, .mod_imm = 1,
          .movw_movt = 1, .bfx = 1, .clz_rbit = 1,
          .tbb_tbh = 1, .cbz = 1, .sat = 1, .div = 1,
          .dsp = 1, .ldaex = 1, .fp_armv8 = 1, .coproc = 1,
      },
      .is_secure_tz = false,
  };
}

static void setup_no_coproc(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m23",
      .feat = (thop_feat){.t16 = 1, .t32 = 1},
      .is_secure_tz = false,
  };
}

/* ═══════════════════════════════════════════════════════════════════
 *  CDP / CDP2
 * ═══════════════════════════════════════════════════════════════════ */

/* cdp p4, #0, c0, c0, c0, #0  -- DCP INIT */
UT_TEST(test_th_cdp_dcp_init)
{
  setup_armv8m_main();
  thumb_opcode op = th_cdp(4, 0, 0, 0, 0, 0, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE000400);
  return 0;
}

/* cdp p4, #0, c0, c0, c1, #0  -- DCP ADD0 */
UT_TEST(test_th_cdp_dcp_add0)
{
  setup_armv8m_main();
  thumb_opcode op = th_cdp(4, 0, 0, 0, 1, 0, 0);
  UT_ASSERT_EQ(op.opcode, 0xEE000401);
  return 0;
}

/* cdp p4, #1, c0, c0, c1, #0  -- DCP ADD1 (opc1 lands at bit 20) */
UT_TEST(test_th_cdp_dcp_add1)
{
  setup_armv8m_main();
  thumb_opcode op = th_cdp(4, 1, 0, 0, 1, 0, 0);
  UT_ASSERT_EQ(op.opcode, 0xEE100401);
  return 0;
}

/* cdp p4, #1, c0, c0, c1, #1  -- DCP SUB1 (opc2 lands at bit 5) */
UT_TEST(test_th_cdp_dcp_sub1)
{
  setup_armv8m_main();
  thumb_opcode op = th_cdp(4, 1, 0, 0, 1, 1, 0);
  UT_ASSERT_EQ(op.opcode, 0xEE100421);
  return 0;
}

/* cdp p4, #8, c0, c0, c0, #1  -- DCP NRDD */
UT_TEST(test_th_cdp_dcp_nrdd)
{
  setup_armv8m_main();
  thumb_opcode op = th_cdp(4, 8, 0, 0, 0, 1, 0);
  UT_ASSERT_EQ(op.opcode, 0xEE800420);
  return 0;
}

/* cdp p4, #8, c0, c0, c2, #1  -- DCP NRDF */
UT_TEST(test_th_cdp_dcp_nrdf)
{
  setup_armv8m_main();
  thumb_opcode op = th_cdp(4, 8, 0, 0, 2, 1, 0);
  UT_ASSERT_EQ(op.opcode, 0xEE800422);
  return 0;
}

/* cdp p4, #8, c0, c0, c0, #2  -- DCP NTDC */
UT_TEST(test_th_cdp_dcp_ntdc)
{
  setup_armv8m_main();
  thumb_opcode op = th_cdp(4, 8, 0, 0, 0, 2, 0);
  UT_ASSERT_EQ(op.opcode, 0xEE800440);
  return 0;
}

/* cdp p15, #15, c15, c15, c15, #7 -- every field saturated */
UT_TEST(test_th_cdp_all_fields_max)
{
  setup_armv8m_main();
  thumb_opcode op = th_cdp(15, 15, 15, 15, 15, 7, 0);
  UT_ASSERT_EQ(op.opcode, 0xEEFFFFEF);
  return 0;
}

UT_TEST(test_th_cdp2)
{
  setup_armv8m_main();
  thumb_opcode op = th_cdp(4, 0, 0, 0, 0, 0, 1);
  UT_ASSERT_EQ(op.opcode, 0xFE000400);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  MCRR / MCRR2  (the DCP operand-write forms)
 * ═══════════════════════════════════════════════════════════════════ */

/* mcrr p4, #1, r0, r1, c0  -- DCP WXUP r0,r1 */
UT_TEST(test_th_mcrr_dcp_wxup)
{
  setup_armv8m_main();
  thumb_opcode op = th_mcrr(4, 1, R0, R1, 0, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEC410410);
  return 0;
}

/* mcrr p4, #1, r4, r5, c1  -- DCP WYUP r4,r5 */
UT_TEST(test_th_mcrr_dcp_wyup)
{
  setup_armv8m_main();
  thumb_opcode op = th_mcrr(4, 1, R4, R5, 1, 0);
  UT_ASSERT_EQ(op.opcode, 0xEC454411);
  return 0;
}

/* mcrr p4, #1, r2, r3, c2  -- DCP WXYU r2,r3 (float operand pair) */
UT_TEST(test_th_mcrr_dcp_wxyu)
{
  setup_armv8m_main();
  thumb_opcode op = th_mcrr(4, 1, R2, R3, 2, 0);
  UT_ASSERT_EQ(op.opcode, 0xEC432412);
  return 0;
}

/* mcrr p4, #9, r6, r7, c2  -- DCP WXFC (opc1 = 9 exercises the full nibble) */
UT_TEST(test_th_mcrr_dcp_wxfc)
{
  setup_armv8m_main();
  thumb_opcode op = th_mcrr(4, 9, R6, R7, 2, 0);
  UT_ASSERT_EQ(op.opcode, 0xEC476492);
  return 0;
}

/* mcrr p4, #0, r12, r14, c0  -- DCP WXMD ip,lr (high registers) */
UT_TEST(test_th_mcrr_dcp_wxmd_high_regs)
{
  setup_armv8m_main();
  thumb_opcode op = th_mcrr(4, 0, R12, R_LR, 0, 0);
  UT_ASSERT_EQ(op.opcode, 0xEC4EC400);
  return 0;
}

UT_TEST(test_th_mcrr2)
{
  setup_armv8m_main();
  thumb_opcode op = th_mcrr(4, 0, R0, R1, 0, 1);
  UT_ASSERT_EQ(op.opcode, 0xFC410400);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  MRRC / MRRC2  (the DCP 64-bit result-read forms)
 * ═══════════════════════════════════════════════════════════════════ */

/* mrrc p4, #1, r0, r1, c0  -- DCP RDDA r0,r1 (double add result) */
UT_TEST(test_th_mrrc_dcp_rdda)
{
  setup_armv8m_main();
  thumb_opcode op = th_mrrc(4, 1, R0, R1, 0, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEC510410);
  return 0;
}

/* mrrc p4, #3, r0, r1, c0  -- DCP RDDS (double sub result) */
UT_TEST(test_th_mrrc_dcp_rdds)
{
  setup_armv8m_main();
  thumb_opcode op = th_mrrc(4, 3, R0, R1, 0, 0);
  UT_ASSERT_EQ(op.opcode, 0xEC510430);
  return 0;
}

/* mrrc p4, #0, r2, r3, c8  -- DCP RXMD r2,r3 */
UT_TEST(test_th_mrrc_dcp_rxmd)
{
  setup_armv8m_main();
  thumb_opcode op = th_mrrc(4, 0, R2, R3, 8, 0);
  UT_ASSERT_EQ(op.opcode, 0xEC532408);
  return 0;
}

/* mrrc2 p4, #0, r0, r1, c8 -- DCP PXMD, the non-engaging "peek" read */
UT_TEST(test_th_mrrc2_dcp_pxmd)
{
  setup_armv8m_main();
  thumb_opcode op = th_mrrc(4, 0, R0, R1, 8, 1);
  UT_ASSERT_EQ(op.opcode, 0xFC510408);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  MRC / MRC2  (the DCP 32-bit result-read forms)
 * ═══════════════════════════════════════════════════════════════════ */

/* mrc p4, #0, r0, c0, c0, #1  -- DCP RCMP r0 */
UT_TEST(test_th_mrc_dcp_rcmp)
{
  setup_armv8m_main();
  thumb_opcode op = th_mrc(4, 0, R0, 0, 0, 1, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE100430);
  return 0;
}

/* mrc p4, #0, r5, c0, c0, #0  -- DCP RXVD r5 */
UT_TEST(test_th_mrc_dcp_rxvd)
{
  setup_armv8m_main();
  thumb_opcode op = th_mrc(4, 0, R5, 0, 0, 0, 0);
  UT_ASSERT_EQ(op.opcode, 0xEE105410);
  return 0;
}

/* mrc p4, #0, r3, c0, c2, #0  -- DCP RDFA r3 (float add result) */
UT_TEST(test_th_mrc_dcp_rdfa)
{
  setup_armv8m_main();
  thumb_opcode op = th_mrc(4, 0, R3, 0, 2, 0, 0);
  UT_ASSERT_EQ(op.opcode, 0xEE103412);
  return 0;
}

/* mrc p4, #0, r7, c0, c3, #0  -- DCP RDIC r7 (double->int result) */
UT_TEST(test_th_mrc_dcp_rdic)
{
  setup_armv8m_main();
  thumb_opcode op = th_mrc(4, 0, R7, 0, 3, 0, 0);
  UT_ASSERT_EQ(op.opcode, 0xEE107413);
  return 0;
}

/* mrc p4, #0, apsr_nzcv, c0, c0, #1 -- rt == PC is the flag-setting form.
 * This is what makes an inline double compare feed a plain conditional
 * branch, so it must encode rather than be rejected as "PC not allowed". */
UT_TEST(test_th_mrc_dcp_rcmp_apsr_nzcv)
{
  setup_armv8m_main();
  thumb_opcode op = th_mrc(4, 0, R_PC, 0, 0, 1, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE10F430);
  return 0;
}

/* mrc2 p4, #0, apsr_nzcv, c0, c0, #1 -- DCP PCMP, the engaged-flag probe */
UT_TEST(test_th_mrc2_dcp_pcmp_apsr_nzcv)
{
  setup_armv8m_main();
  thumb_opcode op = th_mrc(4, 0, R_PC, 0, 0, 1, 1);
  UT_ASSERT_EQ(op.opcode, 0xFE10F430);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  MCR / MCR2
 * ═══════════════════════════════════════════════════════════════════ */

/* mcr p15, #0, r0, c7, c10, #5 -- the encoding lib/stdatomic.c open-codes
 * today as a raw `.int 0xee070fba`. */
UT_TEST(test_th_mcr_cp15_dmb)
{
  setup_armv8m_main();
  thumb_opcode op = th_mcr(15, 0, R0, 7, 10, 5, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEE070FBA);
  return 0;
}

/* mcr p4, #7, r9, c5, c11, #3 -- opc1 is only 3 bits wide on MCR/MRC */
UT_TEST(test_th_mcr_all_fields)
{
  setup_armv8m_main();
  thumb_opcode op = th_mcr(4, 7, R9, 5, 11, 3, 0);
  UT_ASSERT_EQ(op.opcode, 0xEEE5947B);
  return 0;
}

UT_TEST(test_th_mcr2)
{
  setup_armv8m_main();
  thumb_opcode op = th_mcr(4, 0, R0, 0, 0, 0, 1);
  UT_ASSERT_EQ(op.opcode, 0xFE000410);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Feature gating
 * ═══════════════════════════════════════════════════════════════════ */

UT_TEST(test_coproc_blocked_without_feat)
{
  setup_no_coproc();
  UT_ASSERT_EQ(th_cdp(4, 0, 0, 0, 0, 0, 0).size, 0);
  UT_ASSERT_EQ(th_mcr(4, 0, R0, 0, 0, 0, 0).size, 0);
  UT_ASSERT_EQ(th_mrc(4, 0, R0, 0, 0, 0, 0).size, 0);
  UT_ASSERT_EQ(th_mcrr(4, 0, R0, R1, 0, 0).size, 0);
  UT_ASSERT_EQ(th_mrrc(4, 0, R0, R1, 0, 0).size, 0);
  return 0;
}

UT_TEST(test_coproc_allowed_with_feat)
{
  setup_armv8m_main();
  UT_ASSERT_EQ(th_cdp(4, 0, 0, 0, 0, 0, 0).size, 4);
  UT_ASSERT_EQ(th_mcr(4, 0, R0, 0, 0, 0, 0).size, 4);
  UT_ASSERT_EQ(th_mrc(4, 0, R0, 0, 0, 0, 0).size, 4);
  UT_ASSERT_EQ(th_mcrr(4, 0, R0, R1, 0, 0).size, 4);
  UT_ASSERT_EQ(th_mrrc(4, 0, R0, R1, 0, 0).size, 4);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Whole canned sequences
 *
 *  These are the exact instruction streams TCC will emit inline, taken
 *  from pico-sdk's dcp_canned.inc.S (vendored under tests/benchmarks).
 *  If a sequence here ever stops matching, inline DCP codegen is wrong.
 * ═══════════════════════════════════════════════════════════════════ */

/* dcp_dadd_m rz=r0:r1, rx=r0:r1, ry=r2:r3 */
UT_TEST(test_dcp_canned_dadd_sequence)
{
  setup_armv8m_main();
  UT_ASSERT_EQ(th_mcrr(4, 1, R0, R1, 0, 0).opcode, 0xEC410410); /* WXUP r0,r1 */
  UT_ASSERT_EQ(th_mcrr(4, 1, R2, R3, 1, 0).opcode, 0xEC432411); /* WYUP r2,r3 */
  UT_ASSERT_EQ(th_cdp(4, 0, 0, 0, 1, 0, 0).opcode, 0xEE000401);  /* ADD0 */
  UT_ASSERT_EQ(th_cdp(4, 1, 0, 0, 1, 0, 0).opcode, 0xEE100401);  /* ADD1 */
  UT_ASSERT_EQ(th_cdp(4, 8, 0, 0, 0, 1, 0).opcode, 0xEE800420);  /* NRDD */
  UT_ASSERT_EQ(th_mrrc(4, 1, R0, R1, 0, 0).opcode, 0xEC510410); /* RDDA r0,r1 */
  return 0;
}

/* dcp_dsub_m rz=r0:r1, rx=r0:r1, ry=r2:r3 */
UT_TEST(test_dcp_canned_dsub_sequence)
{
  setup_armv8m_main();
  UT_ASSERT_EQ(th_mcrr(4, 1, R0, R1, 0, 0).opcode, 0xEC410410); /* WXUP r0,r1 */
  UT_ASSERT_EQ(th_mcrr(4, 1, R2, R3, 1, 0).opcode, 0xEC432411); /* WYUP r2,r3 */
  UT_ASSERT_EQ(th_cdp(4, 0, 0, 0, 1, 0, 0).opcode, 0xEE000401);  /* ADD0 */
  UT_ASSERT_EQ(th_cdp(4, 1, 0, 0, 1, 1, 0).opcode, 0xEE100421);  /* SUB1 */
  UT_ASSERT_EQ(th_cdp(4, 8, 0, 0, 0, 1, 0).opcode, 0xEE800420);  /* NRDD */
  UT_ASSERT_EQ(th_mrrc(4, 3, R0, R1, 0, 0).opcode, 0xEC510430); /* RDDS r0,r1 */
  return 0;
}

/* dcp_dcmp_m apsr_nzcv, rx=r0:r1, ry=r2:r3 -- four instructions and the
 * relation is already in the flags. */
UT_TEST(test_dcp_canned_dcmp_sequence)
{
  setup_armv8m_main();
  UT_ASSERT_EQ(th_mcrr(4, 1, R0, R1, 0, 0).opcode, 0xEC410410);  /* WXUP r0,r1 */
  UT_ASSERT_EQ(th_mcrr(4, 1, R2, R3, 1, 0).opcode, 0xEC432411);  /* WYUP r2,r3 */
  UT_ASSERT_EQ(th_cdp(4, 0, 0, 0, 1, 0, 0).opcode, 0xEE000401);   /* ADD0 */
  UT_ASSERT_EQ(th_mrc(4, 0, R_PC, 0, 0, 1, 0).opcode, 0xEE10F430); /* RCMP apsr_nzcv */
  return 0;
}

/* dcp_int2double_m rz=r0:r1, rx=r0 */
UT_TEST(test_dcp_canned_int2double_sequence)
{
  setup_armv8m_main();
  UT_ASSERT_EQ(th_mcrr(4, 7, R0, R0, 0, 0).opcode, 0xEC400470); /* WXIC r0,r0 */
  UT_ASSERT_EQ(th_cdp(4, 0, 0, 0, 1, 0, 0).opcode, 0xEE000401);  /* ADD0 */
  UT_ASSERT_EQ(th_cdp(4, 1, 0, 0, 1, 1, 0).opcode, 0xEE100421);  /* SUB1 */
  UT_ASSERT_EQ(th_cdp(4, 8, 0, 0, 0, 1, 0).opcode, 0xEE800420);  /* NRDD */
  UT_ASSERT_EQ(th_mrrc(4, 3, R0, R1, 0, 0).opcode, 0xEC510430); /* RDDS r0,r1 */
  return 0;
}

/* dcp_double2float_m rz=r0, rx=r0:r1 */
UT_TEST(test_dcp_canned_double2float_sequence)
{
  setup_armv8m_main();
  UT_ASSERT_EQ(th_mcrr(4, 1, R0, R1, 0, 0).opcode, 0xEC410410); /* WXUP r0,r1 */
  UT_ASSERT_EQ(th_cdp(4, 8, 0, 0, 2, 1, 0).opcode, 0xEE800422);  /* NRDF */
  UT_ASSERT_EQ(th_mrc(4, 0, R0, 0, 2, 5, 0).opcode, 0xEE1004B2); /* RDFG r0 */
  return 0;
}
