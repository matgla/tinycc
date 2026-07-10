/*
 *  test_ssa_opt_bitop_const_fold.c - suite for source/opt/ssa/bitop_const_fold.c
 *
 *  The SSA driver resolves the bit-op by callee name (get_tok_str), which the
 *  UT harness stubs to "" -- so the positive fold cannot be driven through the
 *  IR here (that is covered by runtime IR tests 356/358).  Instead we exercise
 *  the fold MATH directly via tcc_ir_bitop_const_eval(name, val), which pins the
 *  classify + evaluate logic (popcount/clz/ctz/parity/ffs/clrsb, 32- and
 *  64-bit, the clz/ctz-of-0 guard, and unknown-name rejection).
 */

#include "bitop_const_fold.h"

#include "ut.h"

/* Foldable: assert the op on `val` yields `exp`. */
#define FOLD_EQ(name, val, exp)                                                                                          \
  do                                                                                                                    \
  {                                                                                                                     \
    int v_ = -12345;                                                                                                    \
    UT_ASSERT_EQ(tcc_ir_bitop_const_eval((name), (int64_t)(val), &v_), 1);                                              \
    UT_ASSERT_EQ(v_, (exp));                                                                                            \
  } while (0)

/* Not foldable: eval returns 0. */
#define NO_FOLD(name, val)                                                                                              \
  do                                                                                                                    \
  {                                                                                                                     \
    int v_ = 0;                                                                                                         \
    UT_ASSERT_EQ(tcc_ir_bitop_const_eval((name), (int64_t)(val), &v_), 0);                                              \
  } while (0)

UT_TEST(test_bcf_popcount_parity)
{
  FOLD_EQ("__popcountsi2", 0xa5a5a5a5u, 16);
  FOLD_EQ("__popcountsi2", 0u, 0);
  FOLD_EQ("__popcountsi2", 0xffffffffu, 32);
  FOLD_EQ("__paritysi2", 0xa5a5a5a5u, 0);  /* 16 bits -> even */
  FOLD_EQ("__paritysi2", 0x7u, 1);         /* 3 bits -> odd */
  FOLD_EQ("__popcountdi2", 0xa5a5a5a5a5a5a5a5ull, 32);
  FOLD_EQ("__paritydi2", 0xa5a5a5a5a5a5a5a5ull, 0);
  return 0;
}

UT_TEST(test_bcf_clz_ctz_ffs)
{
  FOLD_EQ("__clzsi2", 0x00010000, 15);
  FOLD_EQ("__ctzsi2", 0x00010000, 16);
  FOLD_EQ("__clzsi2", 0x80000000u, 0);
  FOLD_EQ("__ctzsi2", 0x1, 0);
  FOLD_EQ("__clzdi2", 0x1ULL, 63);
  FOLD_EQ("__ctzdi2", 0x100000000ULL, 32);
  FOLD_EQ("ffs", 0x50, 5);
  FOLD_EQ("ffs", 0, 0);
  FOLD_EQ("ffsll", 0x100000000ULL, 33);
  FOLD_EQ("ffsll", 0, 0);
  return 0;
}

UT_TEST(test_bcf_clrsb)
{
  FOLD_EQ("__builtin_clrsb", 0x00000f00, 19);
  FOLD_EQ("__builtin_clrsb", -1, 31);
  FOLD_EQ("__builtin_clrsb", 0, 31);
  FOLD_EQ("__builtin_clrsb", 0x40000000, 0);
  FOLD_EQ("__builtin_clrsb", 0x55555555, 0);
  FOLD_EQ("__builtin_clrsbl", 0x80000000u, 0); /* clrsbl is 32-bit: INT_MIN */
  FOLD_EQ("__builtin_clrsbl", 0xf00, 19);
  FOLD_EQ("__builtin_clrsbll", 0x100000000LL, 30);
  FOLD_EQ("__builtin_clrsbll", 0, 63);
  FOLD_EQ("__builtin_clrsbll", 0x7fffffffffffffffLL, 0);
  FOLD_EQ("__builtin_clrsbll", 2LL, 61);
  return 0;
}

UT_TEST(test_bcf_no_fold)
{
  /* clz/ctz of 0 are UB -> not folded. */
  NO_FOLD("__clzsi2", 0);
  NO_FOLD("__ctzsi2", 0);
  NO_FOLD("__clzdi2", 0);
  NO_FOLD("__ctzdi2", 0);
  /* unknown / unrelated callees. */
  NO_FOLD("printf", 0);
  NO_FOLD("__aeabi_idiv", 5);
  NO_FOLD("", 5);
  return 0;
}

UT_SUITE(ssa_opt_bitop_const_fold)
{
  UT_COVERS("bitop_const_fold");
  UT_RUN(test_bcf_popcount_parity);
  UT_RUN(test_bcf_clz_ctz_ffs);
  UT_RUN(test_bcf_clrsb);
  UT_RUN(test_bcf_no_fold);
}
