/* Two miscompiles the readInt work (ssa:loop_unroll_midexit + ssa:load_combine)
 * exposed at -O2/-Os.
 *
 * 1. A 2-byte combine at base+#k became an LDR, not an LDRH.  load_combine's
 *    plain-LOAD form kept the u32 dest type and narrowed only the deref to
 *    16 bits; the LOAD -> LOAD_INDEXED displacement folds (flat disp fusion,
 *    ssa_gen_arm_fuse_load_through_add_imm) take the access width from the
 *    dest, so the two bytes after the value landed in the high half.
 *
 * 2. ssa:dead_loop's guarded rewrite (trip count not provable) turned the
 *    header phis whose operands are constants into SELECTs and dropped the
 *    loop, but left every other header phi -- still read after the loop -- at
 *    its entry value.  Unrolling a constant-trip inner loop makes the outer
 *    loop look dead (its exit-live inner IV is a constant 11 per iteration);
 *    acc and r then lost all their updates.  The hand-unrolled form failed on
 *    base too. */
#include <stdio.h>
#include <stdint.h>
#define NI __attribute__((noinline))

static uint8_t buf[64];

/* --- 1. 2-byte readInt at a nonzero constant offset --- */
static inline uint32_t rdle2(const uint8_t *p)
{
  uint32_t r = 0;
  for (int i = 0; i < 2; i++)
    r |= (uint32_t)p[i] << (8 * i);
  return r;
}
static inline uint32_t rdbe2(const uint8_t *p)
{
  uint32_t r = 0;
  for (int i = 0; i < 2; i++)
    r = (r << 8) | p[i];
  return r;
}
NI uint32_t le2_off16(const uint8_t *g) { return rdle2(g + 16); }
NI uint32_t le2_off6(const uint8_t *g) { return rdle2(g + 6); }
NI uint32_t be2_off16(const uint8_t *g) { return rdbe2(g + 16); }
NI uint32_t be2_off6(const uint8_t *g) { return rdbe2(g + 6); }
NI uint32_t le2_twice(const uint8_t *pp) { return rdle2(pp) * 0x10001u ^ rdle2(pp + 2); }
NI uint32_t le2_loop(const uint8_t *p, int n)
{
  uint32_t h = 0;
  for (int j = 0; j < n; j++)
    h = h * 31u + rdle2(p + 3 * j + 1);
  return h;
}
/* The Zig shape: u32 accumulator, index counting down, q = p + 1. */
NI uint32_t zig_le2_q1(const uint8_t *p)
{
  const uint8_t *q = p + 1;
  uintptr_t i = 1;
  uint32_t acc = 0, v;
loop:
  v = (acc << 8) | q[i];
  if (i == 0)
    goto out;
  acc = v;
  i = i - 1;
  goto loop;
out:
  return v;
}

/* --- 2. dead_loop with header phis live out of the loop --- */
NI uint32_t mx_outer_live(uint32_t x)
{
  uint32_t i = 0, acc = x, r = 0;
  for (uint32_t j = 0; j < (x & 3); j++)
  {
    i = 0;
  L:
    i = i + 1;
    if (i > 10)
    {
      r = acc ^ i;
      goto out;
    }
    goto L;
  out:
    acc = r;
  }
  return r * 0x9e3779b9u ^ acc ^ i * 0x85ebca6bu;
}
NI uint32_t mx_nested_for(const uint8_t *p, int n)
{
  uint32_t acc = 5, i = 0;
  int k;
  for (k = 0; k < n; k++)
  {
    i = 0;
    for (;;)
    {
      acc = acc * 7 + p[i + k];
      if (i == 3)
        break;
      i++;
    }
    acc ^= k;
  }
  return acc + i * 100 + k;
}
/* Already the shape after unrolling: failed on base. */
NI uint32_t dl_manual4(uint32_t x)
{
  uint32_t i = 0, acc = x, r = 0;
  for (uint32_t j = 0; j < (x & 3); j++)
  {
    i = 0;
    i = i + 1;
    if (i > 3) { r = acc ^ i; goto out; }
    i = i + 1;
    if (i > 3) { r = acc ^ i; goto out; }
    i = i + 1;
    if (i > 3) { r = acc ^ i; goto out; }
    i = i + 1;
    if (i > 3) { r = acc ^ i; goto out; }
  out:
    acc = r;
  }
  return r * 0x9e3779b9u ^ acc ^ i * 0x85ebca6bu;
}
/* First-trip exit inner loop: failed on base. */
NI uint32_t first_trip_exit(uint32_t x)
{
  uint32_t i = 0xbu, acc = x, r = 0, tot = 0;
  for (uint32_t j = 0; j < (x & 3); j++)
  {
    acc ^= j * 0x10001u;
    i = 0xbu;
  L7:
    i = i + 1;
    if (i == 0xcu) { r = acc * 3u + 1; goto out7; }
    if (i < 0xdu) { r = acc ^ i; goto out7; }
    goto L7;
  out7:
    tot += r + acc + i;
    acc = r ^ tot;
  }
  return r * 0x9e3779b9u ^ acc ^ i * 0x85ebca6bu ^ tot;
}

int main(void)
{
  for (int k = 0; k < 64; k++)
    buf[k] = (uint8_t)(k * 37 + 0x81);
  for (int off = 0; off < 3; off++)
  {
    const uint8_t *g = buf + off * 7;
    printf("rd%d le16=%08x le6=%08x be16=%08x be6=%08x twice=%08x q1=%08x loop=%08x\n", off,
           (unsigned)le2_off16(g), (unsigned)le2_off6(g), (unsigned)be2_off16(g), (unsigned)be2_off6(g),
           (unsigned)le2_twice(g + 5), (unsigned)zig_le2_q1(g), (unsigned)le2_loop(g, off + 2));
  }
  static const uint32_t xs[6] = {0u, 1u, 2u, 3u, 0xdeadbeefu, 0x80000001u};
  for (int xi = 0; xi < 6; xi++)
    printf("dl x=%08x outer=%08x manual=%08x first=%08x\n", (unsigned)xs[xi], (unsigned)mx_outer_live(xs[xi]),
           (unsigned)dl_manual4(xs[xi]), (unsigned)first_trip_exit(xs[xi]));
  for (int n = 0; n < 6; n++)
    printf("nested n=%d %08x\n", n, (unsigned)mx_nested_for(buf + n, n));
  return 0;
}
