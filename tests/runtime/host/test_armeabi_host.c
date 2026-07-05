/*
 * Host-native algorithmic tests for lib/armeabi.c helpers.
 *
 * Compile with: gcc -O2 -DHOST_TEST test_armeabi_host.c -o test_armeabi_host
 *
 * The implementation under test is included directly so we exercise the same
 * source that is built into the ARMv8-M runtime library.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Pull in the EABI implementation under test. */
#define __ARM_EABI__ 1
#include "../../../lib/armeabi.c"

static int test_count = 0;
static int fail_count = 0;

#define FAIL(fmt, ...)                                                        \
  do                                                                          \
  {                                                                           \
    printf("FAIL: " fmt "\n", ##__VA_ARGS__);                                 \
    fail_count++;                                                             \
  } while (0)

#define CHECK(cond)                                                           \
  do                                                                          \
  {                                                                           \
    test_count++;                                                             \
    if (!(cond))                                                              \
      FAIL("%s", #cond);                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                        \
  do                                                                          \
  {                                                                           \
    test_count++;                                                             \
    if ((a) != (b))                                                           \
      FAIL("%s (%u) != %s (%u)", #a, (unsigned)(a), #b, (unsigned)(b));        \
  } while (0)

#define CHECK_ULL(a, b)                                                       \
  do                                                                          \
  {                                                                           \
    test_count++;                                                             \
    if ((unsigned long long)(a) != (unsigned long long)(b))                   \
      FAIL("%s (%llu) != %s (%llu)", #a, (unsigned long long)(a), #b,          \
           (unsigned long long)(b));                                          \
  } while (0)

#define CHECK_LL(a, b)                                                        \
  do                                                                          \
  {                                                                           \
    test_count++;                                                             \
    if ((long long)(a) != (long long)(b))                                     \
      FAIL("%s (%lld) != %s (%lld)", #a, (long long)(a), #b, (long long)(b));  \
  } while (0)

static void test_uidiv(void)
{
  CHECK_EQ(__aeabi_uidiv(0, 1), 0);
  CHECK_EQ(__aeabi_uidiv(10, 3), 3);
  CHECK_EQ(__aeabi_uidiv(100, 7), 14);
  CHECK_EQ(__aeabi_uidiv(7, 7), 1);
  CHECK_EQ(__aeabi_uidiv(UINT_MAX, 1), UINT_MAX);
  CHECK_EQ(__aeabi_uidiv(UINT_MAX, 2), UINT_MAX / 2);
  CHECK_EQ(__aeabi_uidiv(123456789U, 1), 123456789U);
}

static void test_idiv(void)
{
  CHECK_EQ(__aeabi_idiv(0, 1), 0);
  CHECK_EQ(__aeabi_idiv(10, 3), 3);
  CHECK_EQ(__aeabi_idiv(-10, 3), -3);
  CHECK_EQ(__aeabi_idiv(10, -3), -3);
  CHECK_EQ(__aeabi_idiv(-10, -3), 3);
  CHECK_EQ(__aeabi_idiv(INT_MIN, -1), INT_MIN); /* EABI leaves this to caller */
  CHECK_EQ(__aeabi_idiv(INT_MIN, 2), INT_MIN / 2);
}

static void test_uldivmod_helper(void)
{
  u32 q_lo, q_hi, r_lo, r_hi;

  __tcc_aeabi_uldivmod_helper(10, 0, 3, 0, &q_lo, &q_hi, &r_lo, &r_hi);
  CHECK_EQ(q_lo, 3);
  CHECK_EQ(q_hi, 0);
  CHECK_EQ(r_lo, 1);
  CHECK_EQ(r_hi, 0);

  __tcc_aeabi_uldivmod_helper(0, 1, 0, 1, &q_lo, &q_hi, &r_lo, &r_hi);
  CHECK_EQ(q_lo, 1);
  CHECK_EQ(q_hi, 0);
  CHECK_EQ(r_lo, 0);
  CHECK_EQ(r_hi, 0);

  __tcc_aeabi_uldivmod_helper(0x00000001U, 0x00000000U, 0x00000002U,
                              0x00000000U, &q_lo, &q_hi, &r_lo, &r_hi);
  CHECK_EQ(q_lo, 0);
  CHECK_EQ(q_hi, 0);
  CHECK_EQ(r_lo, 1);
  CHECK_EQ(r_hi, 0);

  /* Large values: (2^64-1) / 3 */
  __tcc_aeabi_uldivmod_helper(0xFFFFFFFFU, 0xFFFFFFFFU, 3, 0, &q_lo, &q_hi,
                              &r_lo, &r_hi);
  CHECK_ULL(((unsigned long long)q_hi << 32) | q_lo,
            0xFFFFFFFFFFFFFFFFULL / 3);
  CHECK_ULL(((unsigned long long)r_hi << 32) | r_lo,
            0xFFFFFFFFFFFFFFFFULL % 3);
}

static void test_ldivmod_helper(void)
{
  u32 q_lo, q_hi, r_lo, r_hi;

  __tcc_aeabi_ldivmod_helper(7, 0, 3, 0, &q_lo, &q_hi, &r_lo, &r_hi);
  CHECK_EQ((int)q_lo, 2);
  CHECK_EQ(q_hi, 0);
  CHECK_EQ((int)r_lo, 1);
  CHECK_EQ(r_hi, 0);

  __tcc_aeabi_ldivmod_helper(7, 0, -3, -1, &q_lo, &q_hi, &r_lo, &r_hi);
  CHECK_EQ((int)q_lo, -2);
  CHECK_EQ((int)q_hi, -1);
  CHECK_EQ((int)r_lo, 1);
  CHECK_EQ((int)r_hi, 0);

  __tcc_aeabi_ldivmod_helper(-7, -1, 3, 0, &q_lo, &q_hi, &r_lo, &r_hi);
  CHECK_EQ((int)q_lo, -2);
  CHECK_EQ((int)q_hi, -1);
  CHECK_EQ((int)r_lo, -1);
  CHECK_EQ((int)r_hi, -1);
}

static void test_lcmp_ulcmp(void)
{
  CHECK_EQ(__aeabi_lcmp(0, 0, 0, 0), 0);
  CHECK_EQ(__aeabi_lcmp(1, 0, 2, 0), -1);
  CHECK_EQ(__aeabi_lcmp(2, 0, 1, 0), 1);
  CHECK_EQ(__aeabi_lcmp(0, -1, 0, 0), -1); /* -2^63 < 0 */
  CHECK_EQ(__aeabi_lcmp(0, 0, 0, -1), 1);

  CHECK_EQ(__aeabi_ulcmp(0, 0, 0, 0), 0);
  CHECK_EQ(__aeabi_ulcmp(1, 0, 2, 0), -1);
  CHECK_EQ(__aeabi_ulcmp(0, 1, 0, 0), 1);
  CHECK_EQ(__aeabi_ulcmp(0, 0, 0, 1), -1);
}

static void test_clz(void)
{
  CHECK_EQ(__aeabi_clz(1), 31);
  CHECK_EQ(__aeabi_clz(0x80000000U), 0);
  CHECK_EQ(__aeabi_clz(0x0F000000U), 4);
  CHECK_EQ(__aeabi_clz(0), 32);
  CHECK_EQ(__aeabi_clz(0xFFFFFFFFU), 0);
}

static void test_ll_shifts(void)
{
  /* Logical shift right */
  CHECK_ULL(__aeabi_llsr(0x123456789ABCDEF0ULL, 4),
            0x0123456789ABCDEFULL);
  CHECK_ULL(__aeabi_llsr(0x123456789ABCDEF0ULL, 32),
            0x0000000012345678ULL);
  CHECK_ULL(__aeabi_llsr(0x123456789ABCDEF0ULL, 33),
            0x00000000091A2B3CULL);
  CHECK_ULL(__aeabi_llsr(0x123456789ABCDEF0ULL, 0),
            0x123456789ABCDEF0ULL);
  CHECK_ULL(__aeabi_llsr(0x123456789ABCDEF0ULL, 63), 0);

  /* Logical shift left */
  CHECK_ULL(__aeabi_llsl(0x0000000012345678ULL, 4),
            0x0000000123456780ULL);
  CHECK_ULL(__aeabi_llsl(0x0000000012345678ULL, 32),
            0x1234567800000000ULL);
  CHECK_ULL(__aeabi_llsl(0x0000000012345678ULL, 33),
            0x2468ACF000000000ULL);
  CHECK_ULL(__aeabi_llsl(0x0000000012345678ULL, 0),
            0x0000000012345678ULL);
  CHECK_ULL(__aeabi_llsl(0x0000000012345678ULL, 63), 0);

  /* Arithmetic shift right */
  CHECK_LL(__aeabi_lasr(0x123456789ABCDEF0LL, 4),
           0x0123456789ABCDEFLL);
  CHECK_LL(__aeabi_lasr(0xF23456789ABCDEF0LL, 4),
           0xFF23456789ABCDEFLL);
  CHECK_LL(__aeabi_lasr(0xF23456789ABCDEF0LL, 32),
           0xFFFFFFFFF2345678LL);
  CHECK_LL(__aeabi_lasr(0xF23456789ABCDEF0LL, 0),
           0xF23456789ABCDEF0LL);
  CHECK_LL(__aeabi_lasr(0xF23456789ABCDEF0LL, 63), -1);
}

static void test_mem_helpers(void)
{
  unsigned char src[32];
  unsigned char dst[32];
  for (int i = 0; i < 32; i++)
    src[i] = (unsigned char)i;

  memset(dst, 0, sizeof(dst));
  __aeabi_memcpy(dst, src, 32);
  CHECK(memcmp(dst, src, 32) == 0);

  memset(dst, 0, sizeof(dst));
  __aeabi_memmove(dst + 4, src, 16);
  CHECK(memcmp(dst + 4, src, 16) == 0);

  memset(dst, 0, sizeof(dst));
  __aeabi_memset(dst, 16, 0xAB);
  for (int i = 0; i < 16; i++)
    CHECK(dst[i] == 0xAB);
  for (int i = 16; i < 32; i++)
    CHECK(dst[i] == 0);

  /* Overlapping memmove (dest inside source range). */
  char buf[] = "abcdefghij";
  __aeabi_memmove(buf + 2, buf, 5);
  CHECK(memcmp(buf, "ababcdehij", 11) == 0);
}

static void test_int2fp(void)
{
  union
  {
    float f;
    uint32_t u;
  } gotf, expf;
  union
  {
    double d;
    uint64_t u;
  } gotd, expd;

  gotf.f = __aeabi_ul2f(0x123456789ABCDEFULL);
  expf.f = (float)0x123456789ABCDEFULL;
  CHECK(gotf.u == expf.u);

  gotf.f = __aeabi_l2f(-123456789123456789LL);
  expf.f = (float)-123456789123456789LL;
  CHECK(gotf.u == expf.u);

  gotd.d = __aeabi_ul2d(0x123456789ABCDEFULL);
  expd.d = (double)0x123456789ABCDEFULL;
  CHECK(gotd.u == expd.u);

  gotd.d = __aeabi_l2d(-123456789123456789LL);
  expd.d = (double)-123456789123456789LL;
  CHECK(gotd.u == expd.u);
}

int main(void)
{
  printf("=== Testing lib/armeabi.c on host ===\n");

  test_uidiv();
  test_idiv();
  test_uldivmod_helper();
  test_ldivmod_helper();
  test_lcmp_ulcmp();
  test_clz();
  test_ll_shifts();
  test_mem_helpers();
  test_int2fp();

  printf("=== Results: %d/%d checks passed ===\n", test_count - fail_count,
         test_count);

  if (fail_count == 0)
  {
    printf("ALL TESTS PASSED\n");
    return 0;
  }
  printf("FAILURES: %d\n", fail_count);
  return 1;
}
