/* Zig `mem.readInt` shape: copy N bytes into a local array, then a constant-trip
 * do-while whose exit test sits in the MIDDLE of the body.  ssa:loop_unroll_midexit
 * unrolls it (the trip count is simulated, not pattern-matched) and
 * ssa:load_combine turns the byte chain into one ldr/ldrh.  Every shape the
 * transformations must decline is here too, with boundary byte values (>= 0x80,
 * all-ones) and unaligned bases, so a miscompile shows up as a wrong hex value.
 * Expected output is from the host gcc (little-endian, same fixed-width types). */
#include <stdio.h>
#include <stdint.h>

static uint8_t buf[72] = {0x01, 0x80, 0xff, 0x7f, 0x00, 0xfe, 0x81, 0x55, 0xaa, 0x12, 0x34, 0x56, 0x78, 0x9a,
                          0xbc, 0xde, 0xf0, 0x0f, 0xff, 0xff, 0xff, 0xff, 0x80, 0x00, 0x00, 0x80, 0x7f, 0xff,
                          0xc3, 0x3c, 0xa5, 0x5a, 0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04,
                          0x99, 0xf1, 0x08, 0x80, 0x7e, 0x81, 0x00, 0xff, 0x13, 0x37, 0xc0, 0xde, 0xfa, 0xce,
                          0xb0, 0x0c, 0x5e, 0xed, 0x66, 0x6f, 0x6f, 0x21, 0x7f, 0x80, 0xfe, 0x01, 0xa0, 0x0a,
                          0x55, 0xaa, 0x33, 0xcc};

struct a2 { uint8_t array[2]; };
struct a4 { uint8_t array[4]; };
struct a8 { uint8_t array[8]; };

/* The shape the Zig C backend emits. */
__attribute__((noinline)) uint32_t zig_rd4(const uint8_t *p)
{
  struct a4 t21 = *(const struct a4 *)p;
  uintptr_t t23 = 3ul, t17;
  uint32_t t24 = 0, t26, t22, t27;
  uint8_t t25;
zig_loop:
  t17 = t23;
  t25 = t21.array[t17];
  t26 = (uint32_t)t25;
  t27 = t24;
  t27 = t27 << 8;
  t26 = t27 | t26;
  if (t17 == 0ul)
  {
    t22 = t26;
    goto out;
  }
  t24 = t26;
  t17 = t17 - 1ul;
  t23 = t17;
  goto zig_loop;
out:
  return t22;
}

__attribute__((noinline)) uint32_t zig_rd2(const uint8_t *p)
{
  struct a2 t21 = *(const struct a2 *)p;
  uintptr_t t23 = 1ul, t17;
  uint32_t t24 = 0, t26, t22, t27;
  uint8_t t25;
zig_loop:
  t17 = t23;
  t25 = t21.array[t17];
  t26 = (uint32_t)t25;
  t27 = t24;
  t27 = t27 << 8;
  t26 = t27 | t26;
  if (t17 == 0ul)
  {
    t22 = t26;
    goto out;
  }
  t24 = t26;
  t17 = t17 - 1ul;
  t23 = t17;
  goto zig_loop;
out:
  return t22;
}

__attribute__((noinline)) uint64_t zig_rd8(const uint8_t *p)
{
  struct a8 t21 = *(const struct a8 *)p;
  uintptr_t t23 = 7ul, t17;
  uint64_t t24 = 0, t26, t22, t27;
  uint8_t t25;
zig_loop:
  t17 = t23;
  t25 = t21.array[t17];
  t26 = (uint64_t)t25;
  t27 = t24;
  t27 = t27 << 8;
  t26 = t27 | t26;
  if (t17 == 0ul)
  {
    t22 = t26;
    goto out;
  }
  t24 = t26;
  t17 = t17 - 1ul;
  t23 = t17;
  goto zig_loop;
out:
  return t22;
}

/* Mid-exit loops with other trip counts / directions; the counter is read after
 * the loop and a store happens in the body (every copy must run). */
static volatile uint32_t sink[8];

__attribute__((noinline)) uint32_t mid_count(uint32_t seed, uint32_t *final_i)
{
  uint32_t i = 5, acc = seed;
  for (;;)
  {
    acc = acc * 3u + i;
    sink[i & 7] = acc;
    if (i == 2)
      break;
    i--;
  }
  *final_i = i;
  return acc;
}

/* Exit test on a counter that counts UP, exits on the third trip. */
__attribute__((noinline)) uint32_t mid_up(uint32_t seed)
{
  uint32_t i = 0, acc = seed;
  for (;;)
  {
    acc ^= (acc << 5) + i;
    if (i == 2)
      break;
    i++;
  }
  return acc;
}

/* Trip count 17 exceeds the unroll cap: must stay a correct loop. */
__attribute__((noinline)) uint32_t mid_long(uint32_t seed)
{
  uint32_t i = 16, acc = seed;
  for (;;)
  {
    acc = acc * 5u + i;
    if (i == 0)
      break;
    i--;
  }
  return acc;
}

/* Runtime trip count: must not unroll. */
__attribute__((noinline)) uint32_t mid_runtime(uint32_t n, uint32_t seed)
{
  uint32_t i = n, acc = seed;
  for (;;)
  {
    acc = acc * 7u + i;
    if (i == 0)
      break;
    i--;
  }
  return acc;
}

/* zig.c's real shape: the byte passes through an inlined zig_u32_intCast_u8,
 * whose uint8_t parameter is bound by a narrow STORE into a byte variable.
 * The unroller forwards that STORE (SSA would keep one multi-def TEMP for N
 * straight-line copies of it). */
static inline uint32_t cast_u32_u8(uint8_t arg) { return arg; }
static inline uint64_t cast_u64_u8(uint8_t arg) { return arg; }
static inline int32_t cast_i32_i8(int8_t arg) { return arg; }

__attribute__((noinline)) uint32_t zig_rd4_cast(const uint8_t *p)
{
  struct a4 t21 = *(const struct a4 *)p;
  uintptr_t t23 = 3ul, t17;
  uint32_t t24 = 0, t26, t22, t27;
  uint8_t t25;
zig_loop:
  t17 = t23;
  t25 = t21.array[t17];
  t26 = cast_u32_u8(t25);
  t27 = t24;
  t27 = t27 << 8;
  t26 = t27 | t26;
  if (t17 == 0ul)
  {
    t22 = t26;
    goto out;
  }
  t24 = t26;
  t17 = t17 - 1ul;
  t23 = t17;
  goto zig_loop;
out:
  return t22;
}

/* Wyhash.hash's small-input path: two reads sharing every variable, the
 * second at a run-time offset, packed into a u64. */
__attribute__((noinline)) uint64_t zig_rd4x2(const uint8_t *p, uintptr_t off)
{
  struct a4 t21;
  uintptr_t t23, t30, t17;
  uint32_t t6, t22, t24, t26, t27, t31;
  uint64_t t28;
  uint8_t t25;
  t21 = *(const struct a4 *)p;
  t23 = 3ul;
  t24 = 0;
loop1:
  t17 = t23;
  t25 = t21.array[t17];
  t26 = cast_u32_u8(t25);
  t27 = t24;
  t27 = t27 << 8;
  t26 = t27 | t26;
  if (t17 == 0ul)
  {
    t22 = t26;
    goto out1;
  }
  t24 = t26;
  t17 = t17 - 1ul;
  t23 = t17;
  goto loop1;
out1:
  t6 = t22;
  t28 = (uint64_t)t6 << 32;
  t21 = *(const struct a4 *)(p + off);
  t30 = 3ul;
  t31 = 0;
loop2:
  t17 = t30;
  t25 = t21.array[t17];
  t26 = cast_u32_u8(t25);
  t22 = t31;
  t22 = t22 << 8;
  t26 = t22 | t26;
  if (t17 == 0ul)
  {
    t27 = t26;
    goto out2;
  }
  t31 = t26;
  t17 = t17 - 1ul;
  t30 = t17;
  goto loop2;
out2:
  t6 = t27;
  return t28 | t6;
}

__attribute__((noinline)) uint64_t zig_rd8_cast(const uint8_t *p)
{
  struct a8 t47 = *(const struct a8 *)p;
  uintptr_t t48 = 7ul, t12;
  uint64_t t49 = 0, t50, t51, t28;
  uint8_t t25;
zig_loop:
  t12 = t48;
  t25 = t47.array[t12];
  t50 = cast_u64_u8(t25);
  t51 = t49;
  t51 = t51 << 8;
  t50 = t51 | t50;
  if (t12 == 0ul)
  {
    t28 = t50;
    goto out;
  }
  t49 = t50;
  t12 = t12 - 1ul;
  t48 = t12;
  goto zig_loop;
out:
  return t28;
}

/* The narrow parameter read sign-extended: `x & 0xff` is not its value, the
 * STORE is not forwarded and the loop stays. */
__attribute__((noinline)) int32_t mid_signed_cast(const int8_t *p)
{
  uintptr_t i = 3;
  int32_t acc = 0;
  for (;;)
  {
    acc = (int32_t)((uint32_t)acc * 263u + (uint32_t)cast_i32_i8(p[i]));
    if (i == 0)
      break;
    i--;
  }
  return acc;
}

/* A byte variable carried from one iteration to the next and read after the
 * loop: not private to an iteration, so the loop stays. */
__attribute__((noinline)) uint32_t mid_narrow_carry(const uint8_t *p)
{
  uint8_t last = 0x5a;
  uintptr_t i = 3;
  uint32_t acc = 1;
  for (;;)
  {
    acc = acc * 31u + cast_u32_u8(last);
    last = (uint8_t)(p[i] + i);
    if (i == 0)
      break;
    i--;
  }
  return acc + last;
}

/* The counter's address escapes and the body changes it through a pointer:
 * its entry value says nothing about the trip count. */
__attribute__((noinline)) uint32_t mid_counter_escapes(const uint8_t *p)
{
  uintptr_t i = 3;
  uintptr_t *pi = &i;
  uint32_t acc = 0;
  for (;;)
  {
    acc = acc << 4 | p[*pi & 3];
    if (p[0] & 1)
      *pi = *pi ^ 1;
    if (i == 0)
      break;
    i--;
  }
  return acc;
}

/* Plain byte-assembly idioms around the combiner. */
__attribute__((noinline)) uint32_t le32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
__attribute__((noinline)) uint32_t be32(const uint8_t *p)
{
  return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
__attribute__((noinline)) uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
__attribute__((noinline)) uint32_t be16(const uint8_t *p) { return (uint32_t)p[1] | (uint32_t)p[0] << 8; }
__attribute__((noinline)) uint32_t le24(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
/* a store between the loads: the second byte must be the stored one */
__attribute__((noinline)) uint32_t le32_store_between(uint8_t *p)
{
  uint32_t b0 = p[0];
  p[1] = 0x5c;
  uint32_t b1 = p[1];
  uint32_t b2 = p[2], b3 = p[3];
  return b0 | b1 << 8 | b2 << 16 | b3 << 24;
}
/* signed bytes sign-extend: not a zero-extended word */
__attribute__((noinline)) int32_t le16_signed(const int8_t *p) 
{
  return (int32_t)((uint32_t)(int32_t)p[0] | (uint32_t)(int32_t)p[1] << 8);
}
/* the same base loaded twice, shifts out of order */
__attribute__((noinline)) uint32_t shuffled(const uint8_t *p)
{
  return (uint32_t)p[2] | (uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[3] << 24;
}
/* uses of the individual bytes besides the word */
__attribute__((noinline)) uint32_t le32_and_byte(const uint8_t *p, uint32_t *b2)
{
  *b2 = p[2];
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* two words next to each other: each stays one load, results independent */
__attribute__((noinline)) uint32_t two_words(const uint8_t *p) { return le32(p) ^ (le32(p + 4) * 3u); }

/* two little-endian words next to each other in one function: two LDRs, never
 * paired into an LDRD (which faults on an unaligned address even on v8-M) */
__attribute__((noinline)) uint32_t adj_words(const uint8_t *p)
{
  uint32_t a = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  uint32_t b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
  return a ^ (b * 3u);
}

/* blake3's message words: the readInt loop nested in a 16-trip loop, the
 * byte array's address hoisted out of both; each word is one LDR */
struct w16 { uint32_t array[16]; };
__attribute__((noinline)) void zig_load_words(struct w16 *out, const uint8_t *blk)
{
  uintptr_t t9 = 0, t10, t14, t22;
  uint32_t t21, t23, t25, t26, t11;
  uint32_t *t13;
  struct a4 t20;
outer:
  t10 = t9;
  if (t10 < 16)
  {
    t13 = &out->array[t10];
    t14 = t10 * 4;
    t20 = *(const struct a4 *)(blk + t14);
    t22 = 3;
    t23 = 0;
  inner:
    t14 = t22;
    t25 = cast_u32_u8(t20.array[t14]);
    t26 = t23;
    t26 = t26 << 8;
    t25 = t26 | t25;
    if (t14 == 0)
    {
      t21 = t25;
      goto done;
    }
    t23 = t25;
    t14 = t14 - 1;
    t22 = t14;
    goto inner;
  done:
    t11 = t21;
    *t13 = t11;
    t10 = t10 + 1;
    t9 = t10;
    goto outer;
  }
}

/* The exit test's bound is shifted: `j-- * 2` becomes `T = j SHL #1`, which
 * barrel fusion (before SSA) folds into the CMP as `CMP i, j lsl #1`, recorded
 * only in the barrel side table.  The trip simulation must see the shift (it
 * used to compare i == j and unroll one trip short: 4 instead of 5). */
__attribute__((noinline)) uint32_t mid_cmp_lsl(const uint8_t *p)
{
  uint32_t acc = 0;
  int i = 0, j = 6;
  for (;;)
  {
    acc = acc * 3 + p[i];
    if (i == j-- * 2)
      break;
    i++;
  }
  return acc + (uint32_t)i * 1000u;
}
__attribute__((noinline)) uint32_t mid_cmp_lsl_t(const uint8_t *p)
{
  uint32_t acc = 0;
  int i = 0, j = 6;
  for (;;)
  {
    acc = (acc << 8) | p[i];
    int t = j--;
    if (i == (t << 1))
      break;
    i++;
  }
  return acc ^ (uint32_t)i;
}
__attribute__((noinline)) uint32_t mid_cmp_asr(const uint8_t *p)
{
  uint32_t acc = 0;
  int i = 0, j = 13;
  for (;;)
  {
    acc = acc * 3 + p[i];
    if (i == (j-- >> 1))
      break;
    i++;
  }
  return acc + (uint32_t)i * 1000u;
}
__attribute__((noinline)) uint32_t mid_cmp_lsr(const uint8_t *p)
{
  uint32_t acc = 0;
  uint32_t i = 0, j = 20;
  for (;;)
  {
    acc = acc * 3 + p[i];
    if (i >= (j-- >> 2))
      break;
    i++;
  }
  return acc + i * 1000u;
}

/* The lowest byte of a combined chain is also read on its own.  The wide
 * LOAD sits at the same address (same pointer vreg) as that byte load, so a
 * load CSE that matches deref loads by pointer alone (not by width) turned
 * the LDR/LDRH back into the LDRB (or the reverse). */
__attribute__((noinline)) uint32_t cse_h_pre(const uint8_t *p)
{
  uint32_t pre = p[0];
  uint32_t v = p[0] | (uint32_t)p[1] << 8;
  return v ^ pre << 12;
}
__attribute__((noinline)) uint32_t cse_w_pre(const uint8_t *p)
{
  uint32_t pre = p[0];
  uint32_t v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  return v + pre;
}
__attribute__((noinline)) uint32_t cse_w_after(const uint8_t *p)
{
  uint32_t v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  uint32_t post = p[0];
  return v + post;
}
__attribute__((noinline)) uint32_t cse_w_two(const uint8_t *p, uint32_t *o)
{
  uint32_t h = p[0] | (uint32_t)p[1] << 8;
  uint32_t v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  *o = h;
  return v;
}
__attribute__((noinline)) uint32_t cse_f29(const uint8_t *p)
{
  uint32_t pre = p[0];
  uint32_t v = ((uint32_t)p[1] << 8) | p[0] | ((uint32_t)p[2] << 16);
  return v ^ pre << 1;
}
__attribute__((noinline)) uint32_t cse_ix_pre(const uint8_t *p)
{
  uint32_t pre = p[1];
  uint32_t v = p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
  return v + pre;
}
__attribute__((noinline)) uint32_t cse_ix_h(const uint8_t *p)
{
  uint32_t pre = p[3];
  uint32_t v = p[3] | (uint32_t)p[4] << 8;
  return v * 5u + pre;
}
/* readInt then a byte check of the same buffer, as a parser would. */
__attribute__((noinline)) uint32_t cse_parse(const uint8_t *p)
{
  uint32_t len = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  if (p[0] == 0)
    return 0xffffffffu;
  return len;
}
/* Mixed widths through one pointer without the combiner (latent in base). */
__attribute__((noinline)) uint32_t cse_mix(const uint8_t *p)
{
  uint32_t a = p[0];
  uint32_t w;
  w = *(const uint32_t *)(const void *)p;
  return a + w;
}
__attribute__((noinline)) uint32_t cse_mix2(const uint8_t *p)
{
  uint32_t w = *(const uint32_t *)(const void *)p;
  uint32_t a = p[0];
  return a ^ w;
}
__attribute__((noinline)) uint32_t cse_mix3(const uint16_t *p)
{
  uint32_t a = p[0];
  uint32_t b = *(const uint8_t *)(const void *)p;
  uint32_t c = *(const int16_t *)(const void *)p;
  return a * 3u + b + c * 7u;
}

int main(void)
{
  printf("cmp_shift lsl=%08x lsl_t=%08x asr=%08x lsr=%08x\n", (unsigned)mid_cmp_lsl(buf + 1),
         (unsigned)mid_cmp_lsl_t(buf + 2), (unsigned)mid_cmp_asr(buf), (unsigned)mid_cmp_lsr(buf + 3));
  for (int off = 0; off < 4; off++)
  {
    const uint8_t *p = buf + off;
    uint64_t r8 = zig_rd8(p);
    printf("off%d rd4=%08x rd2=%04x rd8=%08x%08x\n", off, (unsigned)zig_rd4(p), (unsigned)zig_rd2(p),
           (unsigned)(r8 >> 32), (unsigned)r8);
    printf("off%d le32=%08x be32=%08x le16=%04x be16=%04x le24=%06x\n", off, (unsigned)le32(p), (unsigned)be32(p),
           (unsigned)le16(p), (unsigned)be16(p), (unsigned)le24(p));
    printf("off%d signed=%d shuf=%08x two=%08x\n", off, (int)le16_signed((const int8_t *)p), (unsigned)shuffled(p),
           (unsigned)two_words(p));
    uint32_t b2;
    uint32_t w = le32_and_byte(p, &b2);
    printf("off%d w=%08x b2=%02x\n", off, (unsigned)w, (unsigned)b2);
  }
  for (int off = 0; off < 4; off++)
  {
    const uint8_t *p = buf + off;
    uint64_t c8 = zig_rd8_cast(p), x2 = zig_rd4x2(p, (uintptr_t)(off * 5 + 3));
    printf("off%d cast4=%08x cast8=%08x%08x x2=%08x%08x\n", off, (unsigned)zig_rd4_cast(p), (unsigned)(c8 >> 32),
           (unsigned)c8, (unsigned)(x2 >> 32), (unsigned)x2);
    printf("off%d scast=%d carry=%08x esc=%08x adj=%08x\n", off, (int)mid_signed_cast((const int8_t *)p),
           (unsigned)mid_narrow_carry(p), (unsigned)mid_counter_escapes(p), (unsigned)adj_words(p));
  }
  {
    struct w16 w;
    zig_load_words(&w, buf + 3);
    uint32_t h = 0;
    for (int k = 0; k < 16; k++)
      h = (h ^ w.array[k]) * 0x01000193u;
    printf("words w0=%08x w9=%08x w15=%08x h=%08x\n", (unsigned)w.array[0], (unsigned)w.array[9],
           (unsigned)w.array[15], (unsigned)h);
  }
  uint8_t tmp[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
  printf("store_between=%08x\n", (unsigned)le32_store_between(tmp + 1));
  uint32_t fi = 99, mc = mid_count(0x9e3779b9u, &fi);
  printf("mid_count=%08x i=%u\n", (unsigned)mc, (unsigned)fi);
  printf("mid_up=%08x mid_long=%08x\n", (unsigned)mid_up(0xdeadbeefu), (unsigned)mid_long(0x12345678u));
  printf("rt0=%08x rt3=%08x rt9=%08x\n", (unsigned)mid_runtime(0, 1), (unsigned)mid_runtime(3, 1),
         (unsigned)mid_runtime(9, 1));
  {
    static uint32_t wbuf[4];
    uint8_t *wb = (uint8_t *)wbuf;
    for (int k = 0; k < 16; k++)
      wb[k] = buf[k + 1];
    for (int off = 0; off < 3; off++)
    {
      const uint8_t *p = buf + 1 + off * 5;
      uint32_t h;
      uint32_t w2 = cse_w_two(p, &h);
      printf("cse%d h_pre=%08x w_pre=%08x w_after=%08x w_two=%08x/%08x f29=%08x\n", off, (unsigned)cse_h_pre(p),
             (unsigned)cse_w_pre(p), (unsigned)cse_w_after(p), (unsigned)w2, (unsigned)h, (unsigned)cse_f29(p));
      printf("cse%d ix_pre=%08x ix_h=%08x parse=%08x\n", off, (unsigned)cse_ix_pre(buf + off),
             (unsigned)cse_ix_h(buf + off), (unsigned)cse_parse(buf + 3 + off));
    }
    printf("cse parse0=%08x mix=%08x mix2=%08x mix3=%08x\n", (unsigned)cse_parse(buf + 46), (unsigned)cse_mix(wb + 4),
           (unsigned)cse_mix2(wb + 8), (unsigned)cse_mix3((const uint16_t *)(const void *)(wb + 12)));
  }
  return 0;
}
