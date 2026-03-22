/* uses alias to allow building with gcc/clang */
#ifdef __TINYC__
#define BUILTIN(x) __builtin_##x
#define BUILTINN(x) "__builtin_" #x
#else
#define BUILTIN(x) __tcc_builtin_##x
#define BUILTINN(x) "__tcc_builtin_" #x
#endif

/* ---------------------------------------------- */
/* This file implements:
 * __builtin_ffs
 * __builtin_clz
 * __builtin_ctz
 * __builtin_clrsb
 * __builtin_popcount
 * __builtin_parity
 * for int, long and long long
 */

static const unsigned char table_1_32[] = {0,  1,  28, 2,  29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4,  8,
                                           31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6,  11, 5,  10, 9};
static const unsigned char table_2_32[32] = {31, 22, 30, 21, 18, 10, 29, 2,  20, 17, 15, 13, 9, 6,  28, 1,
                                             23, 19, 11, 3,  16, 14, 7,  24, 12, 4,  8,  25, 5, 26, 27, 0};
static const unsigned char table_1_64[] = {0,  1,  2,  53, 3,  7,  54, 27, 4,  38, 41, 8,  34, 55, 48, 28,
                                           62, 5,  39, 46, 44, 42, 22, 9,  24, 35, 59, 56, 49, 18, 29, 11,
                                           63, 52, 6,  26, 37, 40, 33, 47, 61, 45, 43, 21, 23, 58, 17, 10,
                                           51, 25, 36, 32, 60, 20, 57, 16, 50, 31, 19, 15, 30, 14, 13, 12};
static const unsigned char table_2_64[] = {63, 16, 62, 7,  15, 36, 61, 3,  6,  14, 22, 26, 35, 47, 60, 2,
                                           9,  5,  28, 11, 13, 21, 42, 19, 25, 31, 34, 40, 46, 52, 59, 1,
                                           17, 8,  37, 4,  23, 27, 48, 10, 29, 12, 43, 20, 32, 41, 53, 18,
                                           38, 24, 49, 30, 44, 33, 54, 39, 50, 45, 55, 51, 56, 57, 58, 0};

#define FFSI(x) return table_1_32[((x & -x) * 0x077cb531u) >> 27] + (x != 0);
#define FFSL(x) return table_1_64[((x & -x) * 0x022fdd63cc95386dull) >> 58] + (x != 0);
#define CTZI(x) return table_1_32[((x & -x) * 0x077cb531u) >> 27];
#define CTZL(x) return table_1_64[((x & -x) * 0x022fdd63cc95386dull) >> 58];
#define CLZI(x)                                                                                                        \
  x |= x >> 1;                                                                                                         \
  x |= x >> 2;                                                                                                         \
  x |= x >> 4;                                                                                                         \
  x |= x >> 8;                                                                                                         \
  x |= x >> 16;                                                                                                        \
  return table_2_32[(x * 0x07c4acddu) >> 27];
#define CLZL(x)                                                                                                        \
  x |= x >> 1;                                                                                                         \
  x |= x >> 2;                                                                                                         \
  x |= x >> 4;                                                                                                         \
  x |= x >> 8;                                                                                                         \
  x |= x >> 16;                                                                                                        \
  x |= x >> 32;                                                                                                        \
  return table_2_64[x * 0x03f79d71b4cb0a89ull >> 58];
#define POPCOUNTI(x, m)                                                                                                \
  x = x - ((x >> 1) & 0x55555555);                                                                                     \
  x = (x & 0x33333333) + ((x >> 2) & 0x33333333);                                                                      \
  x = (x + (x >> 4)) & 0xf0f0f0f;                                                                                      \
  return ((x * 0x01010101) >> 24) & m;
#define POPCOUNTL(x, m)                                                                                                \
  x = x - ((x >> 1) & 0x5555555555555555ull);                                                                          \
  x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);                                                \
  x = (x + (x >> 4)) & 0xf0f0f0f0f0f0f0full;                                                                           \
  return ((x * 0x0101010101010101ull) >> 56) & m;

/* Returns one plus the index of the least significant 1-bit of x,
   or if x is zero, returns zero. */
int BUILTIN(ffs)(int x)
{
  FFSI(x)
}
int BUILTIN(ffsll)(long long x)
{
  FFSL(x)
}
#if __SIZEOF_LONG__ == 4
int BUILTIN(ffsl)(long x) __attribute__((alias(BUILTINN(ffs))));
#else
int BUILTIN(ffsl)(long x) __attribute__((alias(BUILTINN(ffsll))));
#endif

/* Returns the number of leading 0-bits in x, starting at the most significant
   bit position. If x is 0, the result is undefined.  */
int BUILTIN(clz)(unsigned int x)
{
  CLZI(x)
}
int BUILTIN(clzll)(unsigned long long x)
{
  CLZL(x)
}
#if __SIZEOF_LONG__ == 4
int BUILTIN(clzl)(unsigned long x) __attribute__((alias(BUILTINN(clz))));
#else
int BUILTIN(clzl)(unsigned long x) __attribute__((alias(BUILTINN(clzll))));
#endif

/* Returns the number of trailing 0-bits in x, starting at the least
   significant bit position. If x is 0, the result is undefined. */
int BUILTIN(ctz)(unsigned int x)
{
  CTZI(x)
}

int __ctzsi2(unsigned int x)
{
  CTZI(x)
}

int BUILTIN(ctzll)(unsigned long long x)
{
  CTZL(x)
}
#if __SIZEOF_LONG__ == 4
int BUILTIN(ctzl)(unsigned long x) __attribute__((alias(BUILTINN(ctz))));
#else
int BUILTIN(ctzl)(unsigned long x) __attribute__((alias(BUILTINN(ctzll))));
#endif

/* Returns the number of leading redundant sign bits in x, i.e. the number
   of bits following the most significant bit that are identical to it.
   There are no special cases for 0 or other values. */
int BUILTIN(clrsb)(int x)
{
  if (x < 0)
    x = ~x;
  x <<= 1;
  CLZI(x)
}
int BUILTIN(clrsbll)(long long x)
{
  if (x < 0)
    x = ~x;
  x <<= 1;
  CLZL(x)
}
#if __SIZEOF_LONG__ == 4
int BUILTIN(clrsbl)(long x) __attribute__((alias(BUILTINN(clrsb))));
#else
int BUILTIN(clrsbl)(long x) __attribute__((alias(BUILTINN(clrsbll))));
#endif

/* Returns the number of 1-bits in x.*/
int BUILTIN(popcount)(unsigned int x)
{
  POPCOUNTI(x, 0x3f)
}

int __popcountsi2(unsigned int x)
{
  POPCOUNTI(x, 0x3f)
}

int BUILTIN(popcountll)(unsigned long long x)
{
  POPCOUNTL(x, 0x7f)
}
#if __SIZEOF_LONG__ == 4
int BUILTIN(popcountl)(unsigned long x) __attribute__((alias(BUILTINN(popcount))));
#else
int BUILTIN(popcountl)(unsigned long x) __attribute__((alias(BUILTINN(popcountll))));
#endif

/* Returns the parity of x, i.e. the number of 1-bits in x modulo 2. */
int BUILTIN(parity)(unsigned int x)
{
  POPCOUNTI(x, 0x01)
}
int BUILTIN(parityll)(unsigned long long x)
{
  POPCOUNTL(x, 0x01)
}
#if __SIZEOF_LONG__ == 4
int BUILTIN(parityl)(unsigned long x) __attribute__((alias(BUILTINN(parity))));
#else
int BUILTIN(parityl)(unsigned long x) __attribute__((alias(BUILTINN(parityll))));
#endif

#ifndef __TINYC__
#if defined(__GNUC__) && (__GNUC__ >= 6)
/* gcc overrides alias from __builtin_ffs... to ffs.. so use assembly code */
__asm__(".globl  __builtin_ffs");
__asm__(".set __builtin_ffs,__tcc_builtin_ffs");
__asm__(".globl  __builtin_ffsl");
__asm__(".set __builtin_ffsl,__tcc_builtin_ffsl");
__asm__(".globl  __builtin_ffsll");
__asm__(".set __builtin_ffsll,__tcc_builtin_ffsll");
#else
int __builtin_ffs(int x) __attribute__((alias("__tcc_builtin_ffs")));
int __builtin_ffsl(long x) __attribute__((alias("__tcc_builtin_ffsl")));
int __builtin_ffsll(long long x) __attribute__((alias("__tcc_builtin_ffsll")));
#endif
int __builtin_clz(unsigned int x) __attribute__((alias("__tcc_builtin_clz")));
int __builtin_clzl(unsigned long x) __attribute__((alias("__tcc_builtin_clzl")));
int __builtin_clzll(unsigned long long x) __attribute__((alias("__tcc_builtin_clzll")));
int __builtin_ctz(unsigned int x) __attribute__((alias("__tcc_builtin_ctz")));
int __builtin_ctzl(unsigned long x) __attribute__((alias("__tcc_builtin_ctzl")));
int __builtin_ctzll(unsigned long long x) __attribute__((alias("__tcc_builtin_ctzll")));
int __builtin_clrsb(int x) __attribute__((alias("__tcc_builtin_clrsb")));
int __builtin_clrsbl(long x) __attribute__((alias("__tcc_builtin_clrsbl")));
int __builtin_clrsbll(long long x) __attribute__((alias("__tcc_builtin_clrsbll")));
int __builtin_popcount(unsigned int x) __attribute__((alias("__tcc_builtin_popcount")));
int __builtin_popcountl(unsigned long x) __attribute__((alias("__tcc_builtin_popcountl")));
int __builtin_popcountll(unsigned long long x) __attribute__((alias("__tcc_builtin_popcountll")));
int __builtin_parity(unsigned int x) __attribute__((alias("__tcc_builtin_parity")));
int __builtin_parityl(unsigned long x) __attribute__((alias("__tcc_builtin_parityl")));
int __builtin_parityll(unsigned long long x) __attribute__((alias("__tcc_builtin_parityll")));
#endif

/* ---------------------------------------------- */
/* Unsigned absolute-value helpers used by the compiler for 64-bit lowering. */

unsigned int __tcc_uabsu(int x)
{
  return x < 0 ? -(unsigned int)x : (unsigned int)x;
}

unsigned long __tcc_ulabsu(long x)
{
  return x < 0 ? -(unsigned long)x : (unsigned long)x;
}

/* ---------------------------------------------- */
/* Soft-float FP classification and manipulation functions.
 * Override newlib/libm versions that have ABI issues with TCC soft-float.
 * Uses pure integer bit manipulation — no FP instructions needed.
 */
#if defined(__TINYC__) && defined(__arm__)

int isnan(double x)
{
  union
  {
    double d;
    unsigned long long u;
  } v;
  v.d = x;
  unsigned long long exp = (v.u >> 52) & 0x7FF;
  unsigned long long mant = v.u & 0x000FFFFFFFFFFFFFULL;
  return (exp == 0x7FF && mant != 0);
}

int isnanf(float x)
{
  union
  {
    float f;
    unsigned int u;
  } v;
  v.f = x;
  unsigned int exp = (v.u >> 23) & 0xFF;
  unsigned int mant = v.u & 0x7FFFFF;
  return (exp == 0xFF && mant != 0);
}

int isinf(double x)
{
  union
  {
    double d;
    unsigned long long u;
  } v;
  v.d = x;
  unsigned long long exp = (v.u >> 52) & 0x7FF;
  unsigned long long mant = v.u & 0x000FFFFFFFFFFFFFULL;
  return (exp == 0x7FF && mant == 0);
}

int isinff(float x)
{
  union
  {
    float f;
    unsigned int u;
  } v;
  v.f = x;
  unsigned int exp = (v.u >> 23) & 0xFF;
  unsigned int mant = v.u & 0x7FFFFF;
  return (exp == 0xFF && mant == 0);
}

int finite(double x)
{
  union
  {
    double d;
    unsigned long long u;
  } v;
  v.d = x;
  unsigned long long exp = (v.u >> 52) & 0x7FF;
  return (exp != 0x7FF);
}

int finitef(float x)
{
  union
  {
    float f;
    unsigned int u;
  } v;
  v.f = x;
  unsigned int exp = (v.u >> 23) & 0xFF;
  return (exp != 0xFF);
}

double copysign(double x, double y)
{
  union
  {
    double d;
    unsigned long long u;
  } vx, vy;
  vx.d = x;
  vy.d = y;
  vx.u = (vx.u & 0x7FFFFFFFFFFFFFFFULL) | (vy.u & 0x8000000000000000ULL);
  return vx.d;
}

float copysignf(float x, float y)
{
  union
  {
    float f;
    unsigned int u;
  } vx, vy;
  vx.f = x;
  vy.f = y;
  vx.u = (vx.u & 0x7FFFFFFF) | (vy.u & 0x80000000);
  return vx.f;
}

double fabs(double x)
{
  union
  {
    double d;
    unsigned long long u;
  } v;
  v.d = x;
  v.u &= 0x7FFFFFFFFFFFFFFFULL;
  return v.d;
}

float fabsf(float x)
{
  union
  {
    float f;
    unsigned int u;
  } v;
  v.f = x;
  v.u &= 0x7FFFFFFF;
  return v.f;
}

double fmax(double x, double y)
{
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x > y)
    return x;
  return y;
}

double fmin(double x, double y)
{
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x < y)
    return x;
  return y;
}

float fmaxf(float x, float y)
{
  if (isnanf(x))
    return y;
  if (isnanf(y))
    return x;
  if (x > y)
    return x;
  return y;
}

float fminf(float x, float y)
{
  if (isnanf(x))
    return y;
  if (isnanf(y))
    return x;
  if (x < y)
    return x;
  return y;
}

double floor(double x)
{
  union
  {
    double d;
    unsigned long long u;
  } v;
  v.d = x;
  int exp = (int)((v.u >> 52) & 0x7FF) - 1023;
  int sign = (int)(v.u >> 63);

  /* NaN or Inf — return as-is */
  if (exp == 1024)
    return x;
  /* Already an integer (|x| >= 2^52) */
  if (exp >= 52)
    return x;
  /* |x| < 1 */
  if (exp < 0)
  {
    if (sign)
      return -1.0;
    return 0.0;
  }

  unsigned long long mask = ~((1ULL << (52 - exp)) - 1);
  unsigned long long truncated = v.u & mask;

  if (truncated == v.u)
    return x; /* no fractional part */

  /* For negative numbers, floor rounds towards -infinity */
  if (sign)
    truncated += (1ULL << (52 - exp));

  v.u = truncated;
  return v.d;
}

float floorf(float x)
{
  union
  {
    float f;
    unsigned int u;
  } v;
  v.f = x;
  int exp = (int)((v.u >> 23) & 0xFF) - 127;
  int sign = (int)(v.u >> 31);

  if (exp == 128)
    return x;
  if (exp >= 23)
    return x;
  if (exp < 0)
  {
    if (sign)
      return -1.0f;
    return 0.0f;
  }

  unsigned int mask = ~((1u << (23 - exp)) - 1);
  unsigned int truncated = v.u & mask;

  if (truncated == v.u)
    return x;

  if (sign)
    truncated += (1u << (23 - exp));

  v.u = truncated;
  return v.f;
}

#endif /* __TINYC__ && __arm__ */

unsigned long long __tcc_ullabsu(long long x)
{
  return x < 0 ? -(unsigned long long)x : (unsigned long long)x;
}

unsigned long long __tcc_umaxabsu(long long x)
{
  return x < 0 ? -(unsigned long long)x : (unsigned long long)x;
}

int __tcc_memcmp1(const void *lhs, const void *rhs)
{
  const unsigned char *a = (const unsigned char *)lhs;
  const unsigned char *b = (const unsigned char *)rhs;
  return (int)a[0] - (int)b[0];
}

int __tcc_strncmp(const char *lhs, const char *rhs, unsigned long n)
{
  const unsigned char *a = (const unsigned char *)lhs;
  const unsigned char *b = (const unsigned char *)rhs;

  while (n > 0)
  {
    unsigned char ca = *a++;
    unsigned char cb = *b++;
    if (ca == '\0' || ca != cb)
      return (int)ca - (int)cb;
    --n;
  }

  return 0;
}

void *__tcc_memmove(void *dst, const void *src, unsigned long n)
{
  unsigned char *dstp = (unsigned char *)dst;
  const unsigned char *srcp = (const unsigned char *)src;

  if (srcp < dstp)
  {
    while (n-- != 0)
      dstp[n] = srcp[n];
  }
  else
  {
    while (n-- != 0)
      *dstp++ = *srcp++;
  }

  return dst;
}

void __tcc_bcopy(const void *src, void *dst, unsigned long n)
{
  __tcc_memmove(dst, src, n);
}

void *__tcc_mempcpy(void *dst, const void *src, unsigned long n)
{
  unsigned char *dstp = (unsigned char *)dst;
  const unsigned char *srcp = (const unsigned char *)src;

  while (n-- != 0)
    *dstp++ = *srcp++;

  return dstp;
}

int __tcc_strcpy_count(char *dst, const char *src)
{
  char *start = dst;

  for (;;)
  {
    char ch = *src++;
    *dst++ = ch;
    if (ch == '\0')
      return (int)(dst - start - 1);
  }
}

char *__tcc_strcat(char *dst, const char *src)
{
  char *p = dst;

  while (*p)
    p++;
  while ((*p++ = *src++) != '\0')
    ;

  return dst;
}

char *__tcc_strchr(const char *s, int c)
{
  for (;;)
  {
    if (*s == c)
      return (char *)s;
    if (*s == '\0')
      return 0;
    s++;
  }
}

int __tcc_strcmp(const char *s1, const char *s2)
{
  while (*s1 != 0 && *s1 == *s2)
    s1++, s2++;

  if (*s1 == 0 || *s2 == 0)
    return (unsigned char)*s1 - (unsigned char)*s2;
  return *s1 - *s2;
}

unsigned long __tcc_strlen(const char *s)
{
  const char *p = s;

  while (*p)
    p++;

  return (unsigned long)(p - s);
}

extern volatile int chk_calls __attribute__((weak));
extern void __chk_fail(void) __attribute__((weak));
extern void abort(void);

static void __tcc_chk_record_call(void)
{
  if (&chk_calls != 0)
    ++chk_calls;
}

static void __tcc_chk_fail_or_abort(void)
{
  if (__chk_fail)
    __chk_fail();
  abort();
}

unsigned long __tcc_strnlen(const char *s, unsigned long n)
{
  unsigned long len = 0;

  while (len < n && s[len] != '\0')
    len++;

  return len;
}

char *__tcc_strpbrk(const char *s1, const char *s2)
{
  while (*s1)
  {
    const char *p;

    for (p = s2; *p; p++)
      if (*s1 == *p)
        return (char *)s1;
    s1++;
  }

  return 0;
}

char *__tcc_strrchr(const char *s, int c)
{
  const char *last = 0;

  do
  {
    if (*s == c)
      last = s;
  } while (*s++ != '\0');

  return (char *)last;
}

char *__tcc_strstr(const char *haystack, const char *needle)
{
  if (*needle == '\0')
    return (char *)haystack;

  for (; *haystack; haystack++)
  {
    const char *h = haystack;
    const char *n = needle;

    while (*n && *h == *n)
    {
      h++;
      n++;
    }

    if (*n == '\0')
      return (char *)haystack;
  }

  return 0;
}

unsigned long __tcc_strcspn(const char *s1, const char *s2)
{
  const char *p;

  for (p = s1; *p; p++)
  {
    const char *q;

    for (q = s2; *q; q++)
      if (*p == *q)
        return (unsigned long)(p - s1);
  }

  return (unsigned long)(p - s1);
}

char *__tcc_strncpy(char *dst, const char *src, unsigned long n)
{
  char *ret = dst;

  while (*src && n)
  {
    *dst++ = *src++;
    --n;
  }

  while (n)
  {
    *dst++ = '\0';
    --n;
  }

  return ret;
}

char *__tcc_strncat(char *dst, const char *src, unsigned long n)
{
  char *ret = dst;

  while (*dst)
    dst++;

  while (n > 0)
  {
    char ch = *src++;
    *dst++ = ch;
    if (ch == '\0')
      return ret;
    --n;
  }

  *dst = '\0';
  return ret;
}

char *__tcc_strcpy(char *d, const char *s)
{
  char *r = d;

  while ((*d++ = *s++) != '\0')
    ;

  return r;
}

char *__tcc_stpcpy(char *dst, const char *src)
{
  while (*src != '\0')
    *dst++ = *src++;

  *dst = '\0';
  return dst;
}

char *__tcc_stpncpy(char *dst, const char *src, unsigned long n)
{
  while (*src != '\0' && n != 0)
  {
    *dst++ = *src++;
    --n;
  }

  char *ret = dst;

  while (n-- != 0)
    *dst++ = '\0';

  return ret;
}

char *__tcc_strcpy_chk(char *d, const char *s, unsigned long size)
{
  if (size == (unsigned long)-1)
    __tcc_chk_fail_or_abort();
  __tcc_chk_record_call();
  if (__tcc_strlen(s) >= size)
    __tcc_chk_fail_or_abort();
  return __tcc_strcpy(d, s);
}

char *__tcc_stpcpy_chk(char *d, const char *s, unsigned long size)
{
  if (size == (unsigned long)-1)
    __tcc_chk_fail_or_abort();
  __tcc_chk_record_call();
  if (__tcc_strlen(s) >= size)
    __tcc_chk_fail_or_abort();
  return __tcc_stpcpy(d, s);
}

char *__tcc_stpncpy_chk(char *s1, const char *s2, unsigned long n, unsigned long size)
{
  if (size == (unsigned long)-1)
    __tcc_chk_fail_or_abort();
  __tcc_chk_record_call();
  if (n > size)
    __tcc_chk_fail_or_abort();
  return __tcc_stpncpy(s1, s2, n);
}

char *__tcc_strncpy_chk(char *s1, const char *s2, unsigned long n, unsigned long size)
{
  if (size == (unsigned long)-1)
    __tcc_chk_fail_or_abort();
  __tcc_chk_record_call();
  if (n > size)
    __tcc_chk_fail_or_abort();
  return __tcc_strncpy(s1, s2, n);
}

char *__tcc_strcat_chk(char *d, const char *s, unsigned long size)
{
  if (size == (unsigned long)-1)
    __tcc_chk_fail_or_abort();
  __tcc_chk_record_call();
  if (__tcc_strlen(d) + __tcc_strlen(s) >= size)
    __tcc_chk_fail_or_abort();
  return __tcc_strcat(d, s);
}

char *__tcc_strncat_chk(char *d, const char *s, unsigned long n, unsigned long size)
{
  unsigned long len = __tcc_strlen(d);
  unsigned long n1 = n;
  const char *s1 = s;

  if (size == (unsigned long)-1)
    __tcc_chk_fail_or_abort();
  __tcc_chk_record_call();
  while (len < size && n1 > 0)
  {
    if (*s1++ == '\0')
      break;
    ++len;
    --n1;
  }

  if (len >= size)
    __tcc_chk_fail_or_abort();
  return __tcc_strncat(d, s, n);
}

/* ---------------------------------------------- */
/* Byte swap builtins: __builtin_bswap16, __builtin_bswap32, __builtin_bswap64 */

static inline unsigned short bswap16_impl(unsigned short x)
{
  return ((x & 0x00FF) << 8) | ((x & 0xFF00) >> 8);
}

static inline unsigned int bswap32_impl(unsigned int x)
{
  return ((x & 0x000000FFU) << 24) | ((x & 0x0000FF00U) << 8) | ((x & 0x00FF0000U) >> 8) | ((x & 0xFF000000U) >> 24);
}

static inline unsigned long long bswap64_impl(unsigned long long x)
{
  return ((x & 0x00000000000000FFULL) << 56) | ((x & 0x000000000000FF00ULL) << 40) |
         ((x & 0x0000000000FF0000ULL) << 24) | ((x & 0x00000000FF000000ULL) << 8) | ((x & 0x000000FF00000000ULL) >> 8) |
         ((x & 0x0000FF0000000000ULL) >> 24) | ((x & 0x00FF000000000000ULL) >> 40) |
         ((x & 0xFF00000000000000ULL) >> 56);
}

unsigned short BUILTIN(bswap16)(unsigned short x)
{
  return bswap16_impl(x);
}
unsigned int BUILTIN(bswap32)(unsigned int x)
{
  return bswap32_impl(x);
}
unsigned long long BUILTIN(bswap64)(unsigned long long x)
{
  return bswap64_impl(x);
}

/* Runtime library functions for 64-bit byte swap (used by compiler) */
unsigned long long __bswapdi3(unsigned long long x)
{
  return bswap64_impl(x);
}
unsigned int __bswapsi2(unsigned int x)
{
  return bswap32_impl(x);
}

#ifndef __TINYC__
unsigned short __builtin_bswap16(unsigned short x) __attribute__((alias("__tcc_builtin_bswap16")));
unsigned int __builtin_bswap32(unsigned int x) __attribute__((alias("__tcc_builtin_bswap32")));
unsigned long long __builtin_bswap64(unsigned long long x) __attribute__((alias("__tcc_builtin_bswap64")));
#endif
