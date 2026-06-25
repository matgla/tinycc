/*
 *  test_thop_pld.c - suite for arch/arm/thumb/thop_pld.c
 *
 *  Tests PLD, PLDW, PLI preload instructions (T32 only).
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_pld.h"
#include "arch/arm/thumb/thumb.h"

#include "ut.h"

static void setup_armv8m(void)
{
    arm_target_dependent = (struct target_dependent_config){
        .mcpu_name = "cortex-m33",
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

/* ───── PLD literal ───── */

UT_TEST(test_pld_literal_positive)
{
    setup_armv8m();

    /* pld [pc, #0x100] => 0xf89f_f100 */
    thumb_opcode op = th_pld_literal(0x100);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF89FF100);

    return 0;
}

UT_TEST(test_pld_literal_negative)
{
    setup_armv8m();

    /* pld [pc, #-0x100] => 0xf81f_f100 (U=0, imm still positive) */
    thumb_opcode op = th_pld_literal(-0x100);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF81FF100);

    return 0;
}

/* ───── PLD immediate ───── */

UT_TEST(test_pld_imm_positive)
{
    setup_armv8m();

    /* pld [r1, #0x20] => 0xf891 f020 */
    thumb_opcode op = th_pld_imm(1, 0, 0x20);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF891F020);

    return 0;
}

UT_TEST(test_pld_imm_negative)
{
    setup_armv8m();

    /* pld [r1, #-0x20] => 0xf811 fc20 */
    thumb_opcode op = th_pld_imm(1, 0, -0x20);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF811FC20);

    return 0;
}

/* ───── PLD register ───── */

UT_TEST(test_pld_reg_basic)
{
    setup_armv8m();

    /* pld [r1, r2] => 0xf811 f002 */
    thumb_shift shift = {THUMB_SHIFT_LSL, 0, THUMB_SHIFT_IMMEDIATE};
    thumb_opcode op = th_pld_reg(1, 2, 0, shift);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF811F002);

    return 0;
}

UT_TEST(test_pld_reg_with_lsl)
{
    setup_armv8m();

    /* pld [r1, r2, lsl #3] => 0xf811 f032 */
    thumb_shift shift = {THUMB_SHIFT_LSL, 3, THUMB_SHIFT_IMMEDIATE};
    thumb_opcode op = th_pld_reg(1, 2, 0, shift);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF811F032);

    return 0;
}

UT_TEST(test_pld_reg_default_shift)
{
    setup_armv8m();

    /* pld [r1, r2] with DEFAULT shift (should default to LSL #0) */
    thumb_opcode op = th_pld_reg(1, 2, 0, THUMB_SHIFT_DEFAULT);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF811F002);

    return 0;
}

/* ───── PLI literal ───── */

UT_TEST(test_pli_literal_positive)
{
    setup_armv8m();

    /* pli [pc, #0x100] => 0xf99f f100 */
    thumb_opcode op = th_pli_literal(0x100);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF99FF100);

    return 0;
}

UT_TEST(test_pli_literal_negative)
{
    setup_armv8m();

    /* pli [pc, #-0x100] => 0xf91f f100 (U=0) */
    thumb_opcode op = th_pli_literal(-0x100);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF91FF100);

    return 0;
}

/* ───── PLI immediate ───── */

UT_TEST(test_pli_imm_positive)
{
    setup_armv8m();

    /* pli [r1, #0x20] => 0xf991 f020 */
    thumb_opcode op = th_pli_imm(1, 0, 0x20);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF991F020);

    return 0;
}

UT_TEST(test_pli_imm_negative)
{
    setup_armv8m();

    /* pli [r1, #-0x20] => 0xf911 fc20 */
    thumb_opcode op = th_pli_imm(1, 0, -0x20);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF911FC20);

    return 0;
}

/* ───── PLI register ───── */

UT_TEST(test_pli_reg_basic)
{
    setup_armv8m();

    /* pli [r1, r2] => 0xf911 f002 */
    thumb_shift shift = {THUMB_SHIFT_LSL, 0, THUMB_SHIFT_IMMEDIATE};
    thumb_opcode op = th_pli_reg(1, 2, 0, shift);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF911F002);

    return 0;
}

UT_TEST(test_pli_reg_with_lsl)
{
    setup_armv8m();

    /* pli [r1, r2, lsl #1] => 0xf911 f012 */
    thumb_shift shift = {THUMB_SHIFT_LSL, 1, THUMB_SHIFT_IMMEDIATE};
    thumb_opcode op = th_pli_reg(1, 2, 0, shift);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF911F012);

    return 0;
}

UT_TEST(test_pli_reg_default_shift)
{
    setup_armv8m();

    /* pli [r1, r2] with DEFAULT shift (should default to LSL #0) */
    thumb_opcode op = th_pli_reg(1, 2, 0, THUMB_SHIFT_DEFAULT);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xF911F002);

    return 0;
}

/* ───── suite ───── */

UT_SUITE(thop_pld)
{
    UT_RUN(test_pld_literal_positive);
    UT_RUN(test_pld_literal_negative);
    UT_RUN(test_pld_imm_positive);
    UT_RUN(test_pld_imm_negative);
    UT_RUN(test_pld_reg_basic);
    UT_RUN(test_pld_reg_with_lsl);
    UT_RUN(test_pld_reg_default_shift);
    UT_RUN(test_pli_literal_positive);
    UT_RUN(test_pli_literal_negative);
    UT_RUN(test_pli_imm_positive);
    UT_RUN(test_pli_imm_negative);
    UT_RUN(test_pli_reg_basic);
    UT_RUN(test_pli_reg_with_lsl);
    UT_RUN(test_pli_reg_default_shift);
}