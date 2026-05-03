/*
 *  test_thop_rev.c - suite for arch/arm/thumb/thop_rev.c
 *
 *  Tests REV, REV16, REVSH, RBIT byte/reverse-bit instructions.
 *  REV, REV16, REVSH have T1 (16-bit, low regs) and T2 (32-bit, any reg).
 *  RBIT is T2 only (32-bit).
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_rev.h"
#include "arch/arm/thumb/thumb.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

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

static void setup_no_rbit(void)
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
            .clz_rbit = 0,
            .tbb_tbh = 1,
            .cbz = 1,
            .sat = 1,
            .div = 1,
        },
        .is_secure_tz = false,
    };
}

/* ───── REV ───── */

UT_TEST(test_rev_t1_low_regs)
{
    setup_armv8m();

    /* rev r0, r1 => 0xba08 */
    thumb_opcode op = th_rev(0, 1, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 2);
    UT_ASSERT_EQ(op.opcode, 0xBA08);

    return 0;
}

UT_TEST(test_rev_t2_high_regs)
{
    setup_armv8m();

    /* rev.w r8, r9 => 0xfa99 f889 */
    thumb_opcode op = th_rev(8, 9, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xFA99F889);

    return 0;
}

UT_TEST(test_rev_t1_auto_high_reg_falls_to_t2)
{
    setup_armv8m();

    /* rev r8, r9 (high reg) should fall to T2 */
    thumb_opcode op = th_rev(8, 9, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xFA99F889);

    return 0;
}

UT_TEST(test_rev_enforce_16bit_high_reg_fails)
{
    setup_armv8m();

    /* rev r8, r9 with ENFORCE_ENCODING_16BIT should fail */
    thumb_opcode op = th_rev(8, 9, ENFORCE_ENCODING_16BIT);
    UT_ASSERT_EQ(op.size, 0);
    UT_ASSERT_EQ(op.opcode, 0);

    return 0;
}

/* ───── REV16 ───── */

UT_TEST(test_rev16_t1_low_regs)
{
    setup_armv8m();

    /* rev16 r2, r3 => 0xba5a */
    thumb_opcode op = th_rev16(2, 3, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 2);
    UT_ASSERT_EQ(op.opcode, 0xBA5A);

    return 0;
}

UT_TEST(test_rev16_t2_high_regs)
{
    setup_armv8m();

    /* rev16.w r8, r9 => 0xfa99 f899 */
    thumb_opcode op = th_rev16(8, 9, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xFA99F899);

    return 0;
}

UT_TEST(test_rev16_t1_auto_high_reg_falls_to_t2)
{
    setup_armv8m();

    /* rev16 r8, r9 (high reg) should fall to T2 */
    thumb_opcode op = th_rev16(8, 9, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xFA99F899);

    return 0;
}

/* ───── REVSH ───── */

UT_TEST(test_revsh_t1_low_regs)
{
    setup_armv8m();

    /* revsh r2, r3 => 0xbada */
    thumb_opcode op = th_revsh(2, 3, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 2);
    UT_ASSERT_EQ(op.opcode, 0xBADA);

    return 0;
}

UT_TEST(test_revsh_t2_high_regs)
{
    setup_armv8m();

    /* revsh.w r8, r9 => 0xfa99 f8b9 */
    thumb_opcode op = th_revsh(8, 9, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xFA99F8B9);

    return 0;
}

UT_TEST(test_revsh_t1_auto_high_reg_falls_to_t2)
{
    setup_armv8m();

    /* revsh r8, r9 (high reg) should fall to T2 */
    thumb_opcode op = th_revsh(8, 9, ENFORCE_ENCODING_NONE);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xFA99F8B9);

    return 0;
}

/* ───── RBIT ───── */

UT_TEST(test_rbit_t2_basic)
{
    setup_armv8m();

    /* rbit r0, r1 => 0xfa91 f0a1 */
    thumb_opcode op = th_rbit(0, 1);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xFA91F0A1);

    return 0;
}

UT_TEST(test_rbit_t2_high_regs)
{
    setup_armv8m();

    /* rbit r8, r9 => 0xfa99 f8a9 */
    thumb_opcode op = th_rbit(8, 9);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xFA99F8A9);

    return 0;
}

UT_TEST(test_rbit_without_clz_rbit_feature_fails)
{
    setup_no_rbit();

    /* rbit should fail when clz_rbit feature is not set */
    thumb_opcode op = th_rbit(0, 1);
    UT_ASSERT_EQ(op.size, 0);
    UT_ASSERT_EQ(op.opcode, 0);

    return 0;
}

/* ───── suite ───── */

UT_SUITE(thop_rev)
{
    UT_RUN(test_rev_t1_low_regs);
    UT_RUN(test_rev_t2_high_regs);
    UT_RUN(test_rev_t1_auto_high_reg_falls_to_t2);
    UT_RUN(test_rev_enforce_16bit_high_reg_fails);
    UT_RUN(test_rev16_t1_low_regs);
    UT_RUN(test_rev16_t2_high_regs);
    UT_RUN(test_rev16_t1_auto_high_reg_falls_to_t2);
    UT_RUN(test_revsh_t1_low_regs);
    UT_RUN(test_revsh_t2_high_regs);
    UT_RUN(test_revsh_t1_auto_high_reg_falls_to_t2);
    UT_RUN(test_rbit_t2_basic);
    UT_RUN(test_rbit_t2_high_regs);
    UT_RUN(test_rbit_without_clz_rbit_feature_fails);
}