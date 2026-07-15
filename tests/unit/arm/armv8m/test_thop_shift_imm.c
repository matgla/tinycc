/*
 *  test_thop_shift_imm.c - suite for arch/arm/thumb/thop_shift_imm.c
 *
 *  Tests T1 (16-bit, low regs, imm5) and T3 (32-bit wide) for:
 *  LSL, LSR, ASR, ROR.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_shift_imm.h"
#include "source/backend/arch/arm/thumb/thumb.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

static void setup_armv7m(void)
{
    arm_target_dependent = (struct target_dependent_config){
        .mcpu_name = "cortex-m3",
        .feat =
            (thop_feat){
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

/* ------------------------------------------------------------------ T1: LSL */

UT_TEST(test_th_lsl_imm_t1_basic)
{
    setup_armv7m();

    /* T1: lsls r0, r1, #3 => base 0x0000 | (3<<6) | (1<<3) | 0 = 0x00C8 */
    thumb_opcode op = th_lsl_imm(0, 1, 3, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 2);
    UT_ASSERT_EQ(op.opcode, 0x00C8);

    return 0;
}

UT_TEST(test_th_lsl_imm_t1_imm31)
{
    setup_armv7m();

    /* T1: lsls r2, r3, #31 => base 0x0000 | (31<<6) | (3<<3) | 2 = 0x07DA */
    thumb_opcode op = th_lsl_imm(2, 3, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 2);
    UT_ASSERT_EQ(op.opcode, 0x07DA);

    return 0;
}

UT_TEST(test_th_lsl_imm_t1_imm0_shift32)
{
    setup_armv7m();

    /* T1: lsls r0, r1, #0 encodes as LSL #32 => base 0x0000 | (0<<6) | (1<<3) | 0 = 0x0008 */
    thumb_opcode op = th_lsl_imm(0, 1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 2);
    UT_ASSERT_EQ(op.opcode, 0x0008);

    return 0;
}

/* ------------------------------------------------------------------ T1: LSR */

UT_TEST(test_th_lsr_imm_t1_basic)
{
    setup_armv7m();

    /* T1: lsrs r2, r3, #4 => base 0x0000 | (4<<6) | (3<<3) | 2 = 0x091A */
    thumb_opcode op = th_lsr_imm(2, 3, 4, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 2);
    UT_ASSERT_EQ(op.opcode, 0x091A);

    return 0;
}

UT_TEST(test_th_lsr_imm_t1_imm31)
{
    setup_armv7m();

    /* T1: lsrs r2, r3, #31 => base 0x0000 | (31<<6) | (3<<3) | 2 = 0x0FDA */
    thumb_opcode op = th_lsr_imm(2, 3, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 2);
    UT_ASSERT_EQ(op.opcode, 0x0FDA);

    return 0;
}

/* ------------------------------------------------------------------ T1: ASR */

UT_TEST(test_th_asr_imm_t1_basic)
{
    setup_armv7m();

    /* T1: asrs r3, r4, #5 => base 0x0000 | (5<<6) | (4<<3) | 3 = 0x1163 */
    thumb_opcode op = th_asr_imm(3, 4, 5, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 2);
    UT_ASSERT_EQ(op.opcode, 0x1163);

    return 0;
}

UT_TEST(test_th_asr_imm_t1_imm1)
{
    setup_armv7m();

    /* T1: asrs r5, r6, #1 => base 0x0000 | (1<<6) | (6<<3) | 5 = 0x1075 */
    thumb_opcode op = th_asr_imm(5, 6, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 2);
    UT_ASSERT_EQ(op.opcode, 0x1075);

    return 0;
}

/* ------------------------------------------------------------------ T3: LSL */

UT_TEST(test_th_lsl_imm_t3_high_regs)
{
    setup_armv7m();

    /* T3: lsl.w r8, r9, #3 => 0xEA4F08C9 */
    thumb_opcode op = th_lsl_imm(8, 9, 3, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xEA4F08C9);

    return 0;
}

UT_TEST(test_th_lsl_imm_t3_low_regs)
{
    setup_armv7m();

    /* T3: lsl.w r0, r1, #5 => 0xEA4F1041 */
    thumb_opcode op = th_lsl_imm(0, 1, 5, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_32BIT);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xEA4F1041);

    return 0;
}

/* ------------------------------------------------------------------ T3: LSR */

UT_TEST(test_th_lsr_imm_t3_high_regs)
{
    setup_armv7m();

    /* T3: lsr.w r8, r9, #4 => 0xEA4F1819 */
    thumb_opcode op = th_lsr_imm(8, 9, 4, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xEA4F1819);

    return 0;
}

/* ------------------------------------------------------------------ T3: ASR */

UT_TEST(test_th_asr_imm_t3_high_regs)
{
    setup_armv7m();

    /* T3: asr.w r8, r9, #4 => 0xEA4F1829 */
    thumb_opcode op = th_asr_imm(8, 9, 4, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xEA4F1829);

    return 0;
}

/* ------------------------------------------------------------------ T3: ROR (T32 only) */

UT_TEST(test_th_ror_imm_t3_low_regs)
{
    setup_armv7m();

    /* T3: ror.w r0, r1, #5 => 0xEA4F1071 */
    thumb_opcode op = th_ror_imm(0, 1, 5, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xEA4F1071);

    return 0;
}

UT_TEST(test_th_ror_imm_t3_high_regs)
{
    setup_armv7m();

    /* T3: ror.w r8, r9, #7 => 0xEA4F18F9 */
    thumb_opcode op = th_ror_imm(8, 9, 7, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xEA4F18F9);

    return 0;
}

/* ------------------------------------------------------------------ constraint failures */

UT_TEST(test_th_lsl_imm_t1_high_reg_falls_to_t3)
{
    setup_armv7m();

    /* T1 requires low regs. R8 is high, so falls to T3. */
    thumb_opcode op = th_lsl_imm(8, 1, 3, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xEA4F08C1);

    return 0;
}

UT_TEST(test_th_lsr_imm_t1_high_reg_falls_to_t3)
{
    setup_armv7m();

    /* T1 requires low regs. R8 is high, so falls to T3. */
    thumb_opcode op = th_lsr_imm(8, 1, 4, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xEA4F1811);

    return 0;
}

UT_TEST(test_th_asr_imm_enforce_16bit_high_reg_fails)
{
    setup_armv7m();

    /* T1 requires low regs. Enforcing 16-bit with R8 fails. */
    thumb_opcode op = th_asr_imm(8, 1, 4, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_16BIT);
    UT_ASSERT_EQ(op.size, 0);
    UT_ASSERT_EQ(op.opcode, 0);

    return 0;
}

UT_TEST(test_th_lsl_imm_enforce_32bit_low_regs)
{
    setup_armv7m();

    /* Enforce T32 with low regs — should produce T3 encoding. */
    thumb_opcode op = th_lsl_imm(0, 1, 3, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_32BIT);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xEA4F00C1);

    return 0;
}

UT_TEST(test_th_lsl_imm_pc_in_rd_fails)
{
    setup_armv7m();

    /* T3 requires rd != PC. R15=PC is rejected. */
    thumb_opcode op = th_lsl_imm(R_PC, R1, 3, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 0);
    UT_ASSERT_EQ(op.opcode, 0);

    return 0;
}
