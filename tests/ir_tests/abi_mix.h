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
typedef struct { int32_t a, b; } P2;
/* 8-aligned only through a member's own alignment: gcc's doubleword test
 * reads each field's DECL_ALIGN, which counts _Alignas on the member -- also on
 * a packed member, whose type alone has alignment 1 (Zig's C backend emits
 * `zig_align(8) zig_packed(union {...}) payload`). */
typedef struct { _Alignas(8) int32_t a; int32_t b, c, d; } SA8;
typedef struct { _Alignas(8) union { uint64_t u; int32_t w[4]; } __attribute__((packed)) p; } SP8;

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
  uint32_t P##_f11(int x, SA8 s, int y);                                                                                \
  uint32_t P##_f12(int x, SP8 s, int y);                                                                                \
  uint32_t P##_f13(float f0, float f1, float f2, float f3, float f4, float f5, float f6, float f7,                     \
                   float f8, float f9, float f10, float f11, float f12, float f13, float f14, float f15,              \
                   float f16, int x, int y, int z, P2 s);                                                              \
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
  uint32_t P##_f11(int x, SA8 s, int y) { return (uint32_t)(x * 17 + y * 19) + CK(s); }                                 \
  uint32_t P##_f12(int x, SP8 s, int y) { return (uint32_t)(x * 23 + y * 29) + CK(s); }                                 \
  uint32_t P##_f13(float f0, float f1, float f2, float f3, float f4, float f5, float f6, float f7,                    \
                   float f8, float f9, float f10, float f11, float f12, float f13, float f14, float f15,             \
                   float f16, int x, int y, int z, P2 s) { return (uint32_t)(s.a * 10 + s.b); }                     \
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
  int run_##P(void)                                                                                                     \
  {                                                                                                                     \
    S17 s17;                                                                                                            \
    S20 s20;                                                                                                            \
    S24 s24;                                                                                                            \
    S24L s24l;                                                                                                          \
    S32 s32, t32;                                                                                                       \
    S36 s36;                                                                                                            \
    S20H s20h;                                                                                                          \
    S160 s160;                                                                                                          \
    SA8 sa8;                                                                                                            \
    SP8 sp8;                                                                                                            \
    P2 p2 = { 11, 22 };                                                                                                  \
    int bad = 0;                                                                                                        \
    mix_fill(&s160, sizeof s160, 9);                                                                                    \
    mix_fill(&sa8, sizeof sa8, 10);                                                                                     \
    mix_fill(&sp8, sizeof sp8, 11);                                                                                     \
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
    printf(#P " a8: %u %u\n", (unsigned)O##_f11(3, sa8, 4), (unsigned)O##_f12(5, sp8, 6));                              \
    {                                                                                                                   \
      uint32_t f13 = O##_f13(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,                                        \
                             8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f,                                 \
                             16.0f, 7, 8, 9, p2);                                                                      \
      if (f13 != 132)                                                                                                  \
        printf(#P " f13: %u\n", (unsigned)f13);                                                                        \
      bad |= f13 != 132;                                                                                                \
    }                                                                                                                   \
    printf(#P " v1: %u\n", (unsigned)O##_v1(2, s24, 7, s24, 8));                                                       \
    uint32_t before = CK(s32);                                                                                          \
    uint32_t m = O##_m1(s32);                                                                                           \
    printf(#P " m1: %u %d\n", (unsigned)m, CK(s32) == before);                                                          \
    return bad;                                                                                                         \
  }
