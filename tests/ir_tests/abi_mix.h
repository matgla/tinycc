/* AAPCS32 struct argument passing between tcc and gcc code.  abi_mix_a.c and
 * abi_mix_b.c each define the callees below and a driver calling the other
 * file's callees; test_abi_gcc_interop builds one with tcc and the other with
 * arm-none-eabi-gcc, so every call crosses compilers in both directions.
 * Composites of any size go by value: in the free core registers r0-r3, then
 * the stack, split between the two when they straddle r3 (AAPCS32 C.5); an
 * 8-aligned one starts at an even register.  No padding anywhere, so the
 * byte checksums are well defined. */
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct { unsigned char c[17]; } S17;
typedef struct { int32_t a[5]; } S20;
typedef struct { int32_t a[6]; } S24;
typedef struct { int64_t a, b, c; } S24L; /* 8-aligned */
typedef struct { int32_t a[8]; } S32;
typedef struct { int32_t a[9]; } S36;
typedef struct { int16_t s; unsigned char c[15]; int8_t t[3]; } S20H; /* 2-aligned */
typedef struct { int32_t a[40]; } S160; /* copied by memcpy on the tcc side */

static uint32_t mix_ck(const void *p, unsigned n)
{
  const unsigned char *b = (const unsigned char *)p;
  uint32_t h = 2166136261u;
  for (unsigned i = 0; i < n; i++)
    h = (h ^ b[i]) * 16777619u;
  return h;
}
#define CK(x) mix_ck(&(x), sizeof(x))

static void mix_fill(void *p, unsigned n, unsigned seed)
{
  unsigned char *b = (unsigned char *)p;
  for (unsigned i = 0; i < n; i++)
    b[i] = (unsigned char)(seed * 37u + i * 11u + 5u);
}

#define DECLARE_CALLEES(P)                                                                                              \
  uint32_t P##_f1(S17 s);                                                                                               \
  uint32_t P##_f2(int x, S20 s);                                                                                        \
  uint32_t P##_f3(int x, int y, int z, S24L s);                                                                         \
  uint32_t P##_f4(int a, int b, int c, int d, S20 s, int e);                                                            \
  uint32_t P##_f5(S24 s, S17 t, int u);                                                                                 \
  uint32_t P##_f6(int x, S24L s, S36 t);                                                                                \
  uint32_t P##_f7(S32 s, S32 t);                                                                                        \
  uint32_t P##_f8(S20H s, int x);                                                                                       \
  uint32_t P##_f9(int x, int y, S24L s, int z);                                                                         \
  uint32_t P##_f10(int x, S160 s, int y);                                                                               \
  S24 P##_r1(int k, S20 s);                                                                                             \
  uint32_t P##_v1(int n, ...);                                                                                          \
  uint32_t P##_m1(S32 s);

#define DEFINE_CALLEES(P)                                                                                               \
  uint32_t P##_f1(S17 s) { return CK(s); }                                                                              \
  uint32_t P##_f2(int x, S20 s) { return (uint32_t)x * 31u + CK(s); }                                                  \
  uint32_t P##_f3(int x, int y, int z, S24L s) { return (uint32_t)(x + y * 7 + z * 13) + CK(s); }                      \
  uint32_t P##_f4(int a, int b, int c, int d, S20 s, int e) { return (uint32_t)(a + b * 3 + c * 5 + d * 7 + e * 9) + CK(s); } \
  uint32_t P##_f5(S24 s, S17 t, int u) { return CK(s) ^ (CK(t) * 3u) ^ (uint32_t)u; }                                 \
  uint32_t P##_f6(int x, S24L s, S36 t) { return (uint32_t)x + CK(s) * 5u + CK(t); }                                   \
  uint32_t P##_f7(S32 s, S32 t) { return CK(s) + 2u * CK(t); }                                                          \
  uint32_t P##_f8(S20H s, int x) { return CK(s) + (uint32_t)x; }                                                        \
  uint32_t P##_f9(int x, int y, S24L s, int z) { return (uint32_t)(x * 3 + y * 5 + z * 7) + CK(s); }                  \
  uint32_t P##_f10(int x, S160 s, int y) { return (uint32_t)(x * 11 + y * 13) + CK(s); }                               \
  S24 P##_r1(int k, S20 s)                                                                                              \
  {                                                                                                                     \
    S24 r;                                                                                                              \
    for (int i = 0; i < 6; i++)                                                                                         \
      r.a[i] = k * i + s.a[i % 5];                                                                                      \
    return r;                                                                                                           \
  }                                                                                                                     \
  uint32_t P##_v1(int n, ...)                                                                                           \
  {                                                                                                                     \
    va_list ap;                                                                                                         \
    va_start(ap, n);                                                                                                    \
    uint32_t h = (uint32_t)n;                                                                                           \
    for (int i = 0; i < n; i++)                                                                                         \
    {                                                                                                                   \
      S24 s = va_arg(ap, S24);                                                                                          \
      h = h * 33u + CK(s);                                                                                              \
      h ^= (uint32_t)va_arg(ap, int);                                                                                   \
    }                                                                                                                   \
    va_end(ap);                                                                                                         \
    return h;                                                                                                           \
  }                                                                                                                     \
  /* A callee writing its by-value parameter must not touch the caller's. */                                           \
  uint32_t P##_m1(S32 s)                                                                                                \
  {                                                                                                                     \
    s.a[0] = 999;                                                                                                       \
    s.a[7] ^= 0x55;                                                                                                     \
    return CK(s);                                                                                                       \
  }

#define DEFINE_DRIVER(P, O)                                                                                             \
  void run_##P(void)                                                                                                    \
  {                                                                                                                     \
    S17 s17;                                                                                                            \
    S20 s20;                                                                                                            \
    S24 s24;                                                                                                            \
    S24L s24l;                                                                                                          \
    S32 s32, t32;                                                                                                       \
    S36 s36;                                                                                                            \
    S20H s20h;                                                                                                          \
    S160 s160;                                                                                                          \
    mix_fill(&s160, sizeof s160, 9);                                                                                    \
    mix_fill(&s17, sizeof s17, 1);                                                                                      \
    mix_fill(&s20, sizeof s20, 2);                                                                                      \
    mix_fill(&s24, sizeof s24, 3);                                                                                      \
    mix_fill(&s24l, sizeof s24l, 4);                                                                                    \
    mix_fill(&s32, sizeof s32, 5);                                                                                      \
    mix_fill(&t32, sizeof t32, 6);                                                                                      \
    mix_fill(&s36, sizeof s36, 7);                                                                                      \
    mix_fill(&s20h, sizeof s20h, 8);                                                                                    \
    printf(#P " f: %u %u %u %u %u\n", (unsigned)O##_f1(s17), (unsigned)O##_f2(7, s20), (unsigned)O##_f3(1, 2, 3, s24l), \
           (unsigned)O##_f4(1, 2, 3, 4, s20, 5), (unsigned)O##_f5(s24, s17, 9));                                        \
    printf(#P " g: %u %u %u %u\n", (unsigned)O##_f6(3, s24l, s36), (unsigned)O##_f7(s32, t32),                          \
           (unsigned)O##_f8(s20h, 11), (unsigned)O##_f9(4, 5, s24l, 6));                                                \
    S24 r = O##_r1(5, s20);                                                                                             \
    printf(#P " r1: %u\n", (unsigned)CK(r));                                                                           \
    printf(#P " f10: %u\n", (unsigned)O##_f10(3, s160, 4));                                                                           \
    printf(#P " v1: %u\n", (unsigned)O##_v1(2, s24, 7, s24, 8));                                                       \
    uint32_t before = CK(s32);                                                                                          \
    uint32_t m = O##_m1(s32);                                                                                           \
    printf(#P " m1: %u %d\n", (unsigned)m, CK(s32) == before);                                                          \
  }
