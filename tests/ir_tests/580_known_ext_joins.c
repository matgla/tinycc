/* ra:known_ext (ir/known_ext.c): a UXTB/UXTH of a value already zero-extended
   becomes a copy, from the defining instruction's own extension (LDRB/LDRH,
   AND, UBFX, SHR, SETIF, a constant) carried through copies and phis.  Also
   the second of two identical extensions of one value on the same path, and
   a BFI that reinserts a field the word already holds, and a word loaded
   only for its low byte or halfword (loaded narrow instead).

   What must still extend: an ADD/SUB result (it wraps), a signed narrow read
   of an unsigned load, a phi one of whose inputs is not extended, a word
   loaded whole and then cut. */
#include <stdio.h>

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned u32;

struct eu
{
  u16 error;
};

/* An error union word: payload in the upper half, error in the lower. */
union ew
{
  u32 w;
  struct
  {
    u16 error;
    u16 pad;
  } s;
};

static const u16 codes[6] = {0, 7, 0xFFFF, 0x8001, 0, 300};
static const u8 bytes[6] = {0, 200, 255, 1, 128, 0};
static volatile u32 sink;

/* The error code read in one block and returned from the next. */
__attribute__((noinline)) struct eu pick(int i)
{
  u16 e = codes[i];
  if (e)
  {
    struct eu r;
    r.error = e;
    return r;
  }
  struct eu r = {0};
  return r;
}

/* A join of a LDRH and a LDRB: both extended, the phi too. */
__attribute__((noinline)) u32 join(int c, int i)
{
  u16 v;
  if (c)
    v = codes[i];
  else
    v = bytes[i];
  sink = v;
  return (u32)v * 3u + (v > 255);
}

/* A join where one side is an ADD (wraps past 16 bits before the cut). */
__attribute__((noinline)) u32 join_add(int c, int i)
{
  u16 v;
  if (c)
    v = codes[i];
  else
    v = (u16)(codes[i] + 0xFFF0u);
  return (u32)v;
}

/* Loop-carried narrow value: the cut stays, the phi is not extended by it. */
__attribute__((noinline)) u32 loop_u16(int n)
{
  u16 x = 1;
  for (int i = 0; i < n; i++)
    x = (u16)(x * 3u + 1u);
  return x;
}

__attribute__((noinline)) u32 loop_u8(int n)
{
  u8 x = 5;
  u32 sum = 0;
  for (int i = 0; i < n; i++)
  {
    x = (u8)(x + bytes[i % 6]);
    sum += x;
  }
  return sum;
}

/* LDRB of an unsigned byte read as signed in a later block. */
__attribute__((noinline)) int signed_later(int i, int k)
{
  u8 b = bytes[i];
  if (k)
    sink = b;
  s8 c = (s8)b;
  return c < 0 ? -1000 + c : c;
}

/* A word loaded whole, its low half tested and then returned. */
__attribute__((noinline)) u32 word_cut(const union ew *p)
{
  union ew t = *p;
  if (t.s.error)
    return t.s.error;
  return t.w >> 16;
}

/* Two cuts of one union word: tested, then returned from the next block. */
__attribute__((noinline)) struct eu word_twice(const union ew *p, int k)
{
  u32 w = p->w;
  if (!(w & 0xFFFF))
  {
    struct eu z = {0};
    return z;
  }
  sink = k;
  struct eu r;
  r.error = (u16)w;
  return r;
}

/* The payload/error word rebuilt with its own error field reinserted. */
__attribute__((noinline)) u32 rebuild(const union ew *p)
{
  union ew t = *p;
  u16 e = t.s.error;
  if (e)
  {
    union ew r = t;
    r.s.error = e;
    return r.w;
  }
  return 0;
}

/* A four-byte header copied as one word, then only its low field used. */
struct hdr
{
  u8 bits;
  u8 flags;
  u16 len;
};

__attribute__((noinline)) u32 hdr_bits(const struct hdr *h)
{
  struct hdr c = *h;
  return 1u << (c.bits & 31);
}

__attribute__((noinline)) u32 hdr_word_low(const u32 *p)
{
  u32 w = *p;
  return (u16)w + 1u;
}

static volatile u32 vreg32 = 0xA5A5C3C3u;

__attribute__((noinline)) u32 volatile_low(void)
{
  u32 w = vreg32;
  return (u8)w;
}

/* A zero-extended byte reinterpreted as signed: the SXTB is not a copy. */
__attribute__((noinline)) u32 sx_of_temp(u32 a, int k)
{
  u8 b = (u8)(a >> 4);
  if (k)
    sink = b;
  s8 c = (s8)b;
  u16 d = (u16)c;
  return d;
}

/* SETIF / shifts / masks bound the value. */
__attribute__((noinline)) u32 shapes(u32 a, u32 b)
{
  u32 f = a < b;
  u32 h = a >> 24;
  u32 m = b & 0x3F;
  u16 x = (u16)(f + h + m);
  return (u32)(u8)h + (u32)(u8)m + x;
}

int main(void)
{
  for (int i = 0; i < 6; i++)
  {
    struct eu e = pick(i);
    printf("pick(%d) = %u\n", i, (unsigned)e.error);
  }
  for (int i = 0; i < 6; i++)
    printf("join %d: %u %u  add %u %u\n", i, join(1, i), join(0, i), join_add(1, i), join_add(0, i));
  printf("loop_u16 %u %u %u\n", loop_u16(0), loop_u16(5), loop_u16(40));
  printf("loop_u8 %u %u\n", loop_u8(3), loop_u8(50));
  for (int i = 0; i < 6; i++)
    printf("signed_later(%d) = %d %d\n", i, signed_later(i, 0), signed_later(i, 1));
  union ew w[4];
  w[0].w = 0x12340000u;
  w[1].w = 0x1234ABCDu;
  w[2].w = 0xFFFF0001u;
  w[3].w = 0x0000FFFFu;
  for (int i = 0; i < 4; i++)
  {
    struct eu t = word_twice(&w[i], i);
    printf("word %d: cut %u twice %u rebuild %08x\n", i, word_cut(&w[i]), (unsigned)t.error, rebuild(&w[i]));
  }
  struct hdr hs[2] = {{5, 0xFF, 0xFFFF}, {31, 1, 2}};
  u32 words[2] = {0xFFFFFFFFu, 0x0001FFFEu};
  printf("hdr %u %u low %u %u vol %u\n", hdr_bits(&hs[0]), hdr_bits(&hs[1]), hdr_word_low(&words[0]),
         hdr_word_low(&words[1]), volatile_low());
  printf("sx %u %u %u\n", sx_of_temp(0xC80, 0), sx_of_temp(0x7F0, 1), sx_of_temp(0xFFF0, 1));
  printf("shapes %u %u %u\n", shapes(1, 2), shapes(0xFF000000u, 0x7Fu), shapes(0xABCDEF01u, 0));
  return 0;
}
