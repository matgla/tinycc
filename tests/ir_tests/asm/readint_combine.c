/* Zig `mem.readInt` byte loops (constant trip, exit test in the middle of the
 * body) must unroll and their byte chain must become one LDR/LDRH; byte chains
 * that are not a little-endian zero-extended word must stay as they are.
 * Functions only read through their argument (no external calls). */
#include <stdint.h>

struct a2 { uint8_t array[2]; };
struct a4 { uint8_t array[4]; };

uint32_t zig_rd4(const uint8_t *p)
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

uint32_t zig_rd2(const uint8_t *p)
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

/* zig.c's real shape: the byte passes through an inlined zig_u32_intCast_u8
 * whose uint8_t parameter is bound by a narrow STORE (forwarded by the
 * unroller); two reads share every variable (Wyhash.hash's small path). */
static inline uint32_t cast_u32_u8(uint8_t arg) { return arg; }

uint64_t zig_rd4x2_cast(const uint8_t *p, uintptr_t off)
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

/* a mid-exit loop that is NOT byte assembly: unrolled, no memory access at all */
uint32_t mid_sum(uint32_t seed)
{
  uint32_t i = 3, acc = seed;
  for (;;)
  {
    acc = acc * 3u + i;
    if (i == 0)
      break;
    i--;
  }
  return acc;
}

uint32_t le32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* must stay byte loads */
uint32_t be32(const uint8_t *p)
{
  return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
uint32_t le24(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
uint32_t vol_le32(const volatile uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint32_t store_between(uint8_t *p)
{
  uint32_t b0 = p[0];
  p[1] = 0x5c;
  uint32_t b1 = p[1];
  uint32_t b2 = p[2], b3 = p[3];
  return b0 | b1 << 8 | b2 << 16 | b3 << 24;
}
int32_t le16_signed(const int8_t *p) 
{
  return (int32_t)((uint32_t)(int32_t)p[0] | (uint32_t)(int32_t)p[1] << 8);
}
/* two little-endian words next to each other in one function: two LDRs, never
 * paired into an LDRD (which faults on an unaligned address even on v8-M) */
uint32_t adj_words(const uint8_t *p)
{
  uint32_t a = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  uint32_t b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
  return a ^ (b * 3u);
}
/* blake3's message words: the readInt loop nested in a 16-trip loop, the
 * byte array's address hoisted out of both; each word is one LDR */
struct w16 { uint32_t array[16]; };
void zig_load_words(struct w16 *out, const uint8_t *blk)
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

/* A 2-byte combine at base+#k: LDRH, never an LDR (the displacement folds once
 * took the access width from the u32 dest). */
static inline uint32_t rdle2_at(const uint8_t *p)
{
  uint32_t r = 0;
  for (int i = 0; i < 2; i++)
    r |= (uint32_t)p[i] << (8 * i);
  return r;
}
uint32_t le16_off16(const uint8_t *g) { return rdle2_at(g + 16); }
uint32_t le16_off6(const uint8_t *g) { return rdle2_at(g + 6); }
