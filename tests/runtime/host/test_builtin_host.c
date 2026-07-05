/*
 * Host-native algorithmic tests for lib/builtin.c helpers.
 *
 * Compile with: gcc -O2 -DHOST_TEST -fno-builtin test_builtin_host.c
 *   -o test_builtin_host
 *
 * The implementation under test is included directly.  We disable compiler
 * builtins so that the __builtin_* alias symbols in lib/builtin.c do not
 * clash with the compiler's own builtins.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../lib/builtin.c"

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
      FAIL("%s (%d) != %s (%d)", #a, (int)(a), #b, (int)(b));                  \
  } while (0)

#define CHECK_ULL(a, b)                                                       \
  do                                                                          \
  {                                                                           \
    test_count++;                                                             \
    if ((unsigned long long)(a) != (unsigned long long)(b))                   \
      FAIL("%s (%llu) != %s (%llu)", #a, (unsigned long long)(a), #b,          \
           (unsigned long long)(b));                                          \
  } while (0)

static int ref_ffs(int x)
{
  if (x == 0)
    return 0;
  int n = 1;
  while ((x & 1) == 0)
  {
    x >>= 1;
    n++;
  }
  return n;
}

static int ref_clz(unsigned int x)
{
  if (x == 0)
    return 32;
  int n = 0;
  while ((x & 0x80000000U) == 0)
  {
    x <<= 1;
    n++;
  }
  return n;
}

static int ref_ctz(unsigned int x)
{
  if (x == 0)
    return 32;
  int n = 0;
  while ((x & 1U) == 0)
  {
    x >>= 1;
    n++;
  }
  return n;
}

static int ref_popcount(unsigned int x)
{
  int n = 0;
  while (x)
  {
    n += x & 1U;
    x >>= 1;
  }
  return n;
}

static int ref_parity(unsigned int x)
{
  return ref_popcount(x) & 1;
}

static void test_bitops_int(void)
{
  for (unsigned i = 0; i < 32; i++)
  {
    unsigned int x = 1U << i;
    CHECK_EQ(__tcc_builtin_ffs((int)x), ref_ffs((int)x));
    CHECK_EQ(__tcc_builtin_clz(x), ref_clz(x));
    CHECK_EQ(__tcc_builtin_ctz(x), ref_ctz(x));
    CHECK_EQ(__tcc_builtin_popcount(x), ref_popcount(x));
    CHECK_EQ(__tcc_builtin_parity(x), ref_parity(x));
  }

  CHECK_EQ(__tcc_builtin_ffs(0), ref_ffs(0));
  CHECK_EQ(__tcc_builtin_popcount(0), 0);
  CHECK_EQ(__tcc_builtin_parity(0), 0);
  CHECK_EQ(__tcc_builtin_clz(0xFFFFFFFFU), ref_clz(0xFFFFFFFFU));
  CHECK_EQ(__tcc_builtin_popcount(0xFFFFFFFFU), 32);
  CHECK_EQ(__tcc_builtin_parity(0xFFFFFFFFU), 0);

  /* clrsb counts redundant sign bits (max 31 for a 32-bit int). */
  CHECK_EQ(__tcc_builtin_clrsb(0), 31);
  CHECK_EQ(__tcc_builtin_clrsb(-1), 31);
  CHECK_EQ(__tcc_builtin_clrsb(0x7FFFFFFF), 0);
  CHECK_EQ(__tcc_builtin_clrsb((int)0xC0000000U), 1);
}

static void test_bitops_longlong(void)
{
  CHECK_EQ(__tcc_builtin_ffsll(1LL << 33), 34);
  CHECK_EQ(__tcc_builtin_ffsll(0LL), 0);
  CHECK_EQ(__tcc_builtin_clzll(1ULL << 63), 0);
  CHECK_EQ(__tcc_builtin_clzll(1ULL << 32), 31);
  CHECK_EQ(__tcc_builtin_ctzll(1ULL << 40), 40);
  CHECK_EQ(__tcc_builtin_popcountll(0x5555555555555555ULL), 32);
  CHECK_EQ(__tcc_builtin_parityll(0x5555555555555555ULL), 0);
  CHECK_EQ(__tcc_builtin_clrsbll(0LL), 63);
  CHECK_EQ(__tcc_builtin_clrsbll(-1LL), 63);
}

static void test_abs_helpers(void)
{
  CHECK_EQ((int)__tcc_uabsu(-42), 42);
  CHECK_EQ((int)__tcc_uabsu(42), 42);
  CHECK_ULL(__tcc_ullabsu(-42LL), 42ULL);
  CHECK_ULL(__tcc_ullabsu(42LL), 42ULL);
  CHECK_ULL(__tcc_ullabsu(LLONG_MIN), (unsigned long long)LLONG_MAX + 1ULL);
  CHECK_ULL(__tcc_umaxabsu(-42LL), 42ULL);
}

static void test_bswap(void)
{
  CHECK_EQ(__tcc_builtin_bswap16((unsigned short)0x1234), (unsigned short)0x3412);
  CHECK_EQ(__tcc_builtin_bswap32(0x12345678U), 0x78563412U);
  CHECK_ULL(__tcc_builtin_bswap64(0x0123456789ABCDEFULL),
            0xEFCDAB8967452301ULL);
  CHECK_EQ(__bswapsi2(0x12345678U), 0x78563412U);
  CHECK_ULL(__bswapdi3(0x0123456789ABCDEFULL), 0xEFCDAB8967452301ULL);
}

static void test_string_helpers(void)
{
  char buf[64];

  CHECK_EQ(__tcc_strncmp("abc", "abd", 2), 0);
  CHECK(__tcc_strncmp("abc", "abd", 3) < 0);

  char hello[] = "hello";
  CHECK(__tcc_strchr(hello, 'e') == hello + 1);
  CHECK(__tcc_strchr(hello, 'x') == NULL);

  CHECK(__tcc_strstr(hello, "ell") == hello + 1);
  CHECK(__tcc_strstr(hello, "xyz") == NULL);
  CHECK(__tcc_strstr(hello, "") == hello);

  char rbuf[] = "abac";
  CHECK(__tcc_strrchr(rbuf, 'a') == rbuf + 2);
  CHECK(__tcc_strrchr(rbuf, 'z') == NULL);

  char nbuf[8] = {0};
  __tcc_strncpy(nbuf, "hello", 3);
  {
    const char expected1[8] = {'h', 'e', 'l', 0, 0, 0, 0, 0};
    CHECK_EQ(memcmp(nbuf, expected1, 8), 0);
  }
  __tcc_strncpy(nbuf, "hi", 5);
  {
    const char expected2[8] = {'h', 'i', 0, 0, 0, 0, 0, 0};
    CHECK_EQ(memcmp(nbuf, expected2, 8), 0);
  }

  char cbuf[16] = "hello";
  __tcc_strncat(cbuf, " world", 3);
  CHECK_EQ(strcmp(cbuf, "hello wo"), 0);

  CHECK_EQ(__tcc_strnlen("hello", 10), 5);
  CHECK_EQ(__tcc_strnlen("hello", 3), 3);

  CHECK(__tcc_strpbrk(hello, "aeiou") == hello + 1);
  CHECK(__tcc_strpbrk("xyz", "aeiou") == NULL);

  CHECK_EQ(__tcc_strcspn("hello", "xyz"), 5);
  CHECK_EQ(__tcc_strcspn("hello", "l"), 2);

  char *end = __tcc_stpcpy(buf, "abc");
  CHECK_EQ(end - buf, 3);
  CHECK_EQ(strcmp(buf, "abc"), 0);

  end = __tcc_stpncpy(buf, "abc", 5);
  CHECK_EQ(end - buf, 3);
  CHECK_EQ(memcmp(buf, "abc\0\0", 5), 0);

  unsigned char msrc[] = {1, 2, 3, 4, 5, 6, 7, 8};
  unsigned char mdst[8];
  __tcc_memmove(mdst, msrc, 8);
  CHECK_EQ(memcmp(mdst, msrc, 8), 0);

  /* Overlapping move (source inside destination range -> backwards copy). */
  char mover[] = "abcdef";
  __tcc_memmove(mover + 2, mover, 3);
  CHECK_EQ(memcmp(mover, "ababc", 5), 0);

  unsigned char mpcy_dst[4];
  unsigned char *p = __tcc_mempcpy(mpcy_dst, msrc, 4);
  CHECK_EQ((size_t)(p - mpcy_dst), 4);
  CHECK_EQ(memcmp(mpcy_dst, msrc, 4), 0);

  /* Word-at-a-time string helpers (now work on 64-bit hosts too). */
  CHECK_EQ(__tcc_strlen("hello"), 5);
  CHECK_EQ(__tcc_strlen(""), 0);

  CHECK(__tcc_strcpy(buf, "hello") == buf);
  CHECK_EQ(strcmp(buf, "hello"), 0);

  CHECK(__tcc_strcat(buf, " world") == buf);
  CHECK_EQ(strcmp(buf, "hello world"), 0);

  CHECK_EQ(__tcc_strcmp("abc", "abc"), 0);
  CHECK(__tcc_strcmp("abc", "abd") < 0);
  CHECK(__tcc_strcmp("abd", "abc") > 0);
}

int main(void)
{
  printf("=== Testing lib/builtin.c on host ===\n");

  test_bitops_int();
  test_bitops_longlong();
  test_abs_helpers();
  test_bswap();
  test_string_helpers();

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
