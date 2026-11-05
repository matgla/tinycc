/*
 * Copies of packed structs whose fields are wider than the struct's alignment.
 * They are inlined as unaligned word chunks where the core allows it: every
 * size, placement and aliasing pattern below must still copy exactly the
 * object's bytes, and never touch the byte after it.
 */
#include <stdio.h>
#include <string.h>

typedef struct __attribute__((packed)) { int a; int b; unsigned char c; } P9;
typedef struct __attribute__((packed)) { int a; int b; short c; } P10;
typedef struct __attribute__((packed)) { int a; short b; char c; } P7Tail3;
typedef struct __attribute__((packed)) { int a; char b; short c; int d; } P11Split;
typedef struct __attribute__((packed)) { int a; int b; int c; unsigned char d; } P13;
typedef struct __attribute__((packed)) { unsigned p : 17; unsigned q : 15; union { int i; struct { short s, t; } h; } u; unsigned char f : 3; unsigned char g : 5; } PBits;

#define DEFINE(T)                                                                   \
  __attribute__((noinline)) void deref_##T(T *d, const T *s) { *d = *s; }           \
  __attribute__((noinline)) T ret_##T(const T *s) { return *s; }                    \
  __attribute__((noinline)) void local_##T(T *d, const T *s)                        \
  {                                                                                 \
    T t = *s;                                                                       \
    ((unsigned char *)&t)[0] ^= 0x55;                                               \
    ((unsigned char *)&t)[0] ^= 0x55;                                               \
    *d = t;                                                                         \
  }                                                                                 \
  T global_##T;                                                                     \
  __attribute__((noinline)) void toglobal_##T(const T *s) { global_##T = *s; }      \
  __attribute__((noinline)) void fromglobal_##T(T *d) { *d = global_##T; }          \
  __attribute__((noinline)) void arr_##T(T *a, int i, int j) { a[i] = a[j]; }

DEFINE(P9)
DEFINE(P10)
DEFINE(P7Tail3)
DEFINE(P11Split)
DEFINE(P13)
DEFINE(PBits)

static int bad;

static void check(const char *what, const unsigned char *dst, const unsigned char *src, unsigned off, unsigned size)
{
  for (unsigned i = 0; i < 48; i++)
  {
    unsigned char want = (i >= off && i < off + size) ? src[i - off] : 0x5a;
    if (dst[i] != want)
    {
      printf("%s off %u size %u: byte %u is %02x, want %02x\n", what, off, size, i, dst[i], want);
      bad++;
      return;
    }
  }
}

#define RUN(T)                                                                      \
  do                                                                                \
  {                                                                                 \
    for (unsigned sa = 0; sa < 4; sa++)                                             \
      for (unsigned da = 0; da < 4; da++)                                           \
      {                                                                             \
        _Alignas(8) unsigned char sbuf[48], dbuf[48];                               \
        T *s = (T *)(sbuf + sa);                                                    \
        T *d = (T *)(dbuf + 8 + da);                                                \
        for (unsigned i = 0; i < sizeof(T); i++)                                    \
          sbuf[sa + i] = (unsigned char)(0x81 + i * 7);                             \
        memset(dbuf, 0x5a, sizeof dbuf);                                            \
        deref_##T(d, s);                                                            \
        check("deref " #T, dbuf, sbuf + sa, 8 + da, sizeof(T));                     \
        memset(dbuf, 0x5a, sizeof dbuf);                                            \
        *d = ret_##T(s);                                                            \
        check("ret " #T, dbuf, sbuf + sa, 8 + da, sizeof(T));                       \
        memset(dbuf, 0x5a, sizeof dbuf);                                            \
        local_##T(d, s);                                                            \
        check("local " #T, dbuf, sbuf + sa, 8 + da, sizeof(T));                     \
        memset(dbuf, 0x5a, sizeof dbuf);                                            \
        toglobal_##T(s);                                                            \
        fromglobal_##T(d);                                                          \
        check("global " #T, dbuf, sbuf + sa, 8 + da, sizeof(T));                    \
      }                                                                             \
    /* the copy of an element onto itself, and onto its neighbour */                \
    _Alignas(8) unsigned char abuf[48], want[48];                                   \
    for (unsigned i = 0; i < sizeof abuf; i++)                                      \
      abuf[i] = (unsigned char)(0x11 + i * 3);                                      \
    memcpy(want, abuf, sizeof abuf);                                                \
    arr_##T((T *)(abuf + 1), 1, 1);                                                 \
    if (memcmp(abuf, want, sizeof abuf))                                            \
    {                                                                               \
      printf("self copy " #T " changed bytes\n");                                   \
      bad++;                                                                        \
    }                                                                               \
    arr_##T((T *)(abuf + 1), 0, 1);                                                 \
    memcpy(want + 1, want + 1 + sizeof(T), sizeof(T));                              \
    if (memcmp(abuf, want, sizeof abuf))                                            \
    {                                                                               \
      printf("neighbour copy " #T " wrong\n");                                      \
      bad++;                                                                        \
    }                                                                               \
  } while (0)

int main(void)
{
  RUN(P9);
  RUN(P10);
  RUN(P7Tail3);
  RUN(P11Split);
  RUN(P13);
  RUN(PBits);
  printf("bad %d\n", bad);
  return bad != 0;
}
