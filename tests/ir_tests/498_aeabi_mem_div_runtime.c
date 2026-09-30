/* The ARMv8-M runtime helpers: __aeabi_memcpy/memmove/memset from
   lib/arm_mem.S (LDM/STM blocks, an aligned-destination loop with unaligned
   source loads, word/halfword/byte tails picked from the count's bits, and a
   top-down memmove when the destination starts inside the source), and the
   64-bit division from lib/armeabi.c + armeabi_divmod.S (a 32-bit hardware
   divide when both high words are zero, Hacker's Delight divlu/divdu
   otherwise).  Every alignment, every tail length and every overlap shift is
   checked against byte loops, and the divisions against a bit-at-a-time
   reference that uses no library call. */
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

void *__aeabi_memcpy(void *, const void *, size_t);
void *__aeabi_memmove(void *, const void *, size_t);
void *__aeabi_memmove4(void *, const void *, size_t);
void *__aeabi_memset(void *, size_t, int);
void *__aeabi_memclr(void *, size_t);

static unsigned char buf[400], ref[400];
static uint32_t seed = 12345;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 4; }
static void fill(void) {
  for (int i = 0; i < (int)sizeof buf; i++) buf[i] = ref[i] = (unsigned char)rnd();
}
static int same(void) {
  for (int i = 0; i < (int)sizeof buf; i++) if (buf[i] != ref[i]) return 0;
  return 1;
}
static void refmove(unsigned char *d, const unsigned char *s, int n) {
  unsigned char t[128];
  for (int i = 0; i < n; i++) t[i] = s[i];
  for (int i = 0; i < n; i++) d[i] = t[i];
}

/* restoring division, no helper calls: 32-bit operations on the halves */
static void refdiv(uint32_t nl, uint32_t nh, uint32_t dl, uint32_t dh,
                   uint32_t *ql, uint32_t *qh, uint32_t *rl, uint32_t *rh) {
  uint32_t a = 0, b = 0, x = 0, y = 0;
  for (int i = 63; i >= 0; i--) {
    b = (b << 1) | (a >> 31); a <<= 1;
    a |= (i >= 32 ? nh >> (i - 32) : nl >> i) & 1;
    if (b > dh || (b == dh && a >= dl)) {
      b = b - dh - (a < dl); a -= dl;
      if (i >= 32) y |= 1u << (i - 32); else x |= 1u << i;
    }
  }
  *ql = x; *qh = y; *rl = a; *rh = b;
}

int main(void) {
  int bad = 0, n, d, s, sh;

  for (n = 0; n <= 70; n++)
    for (d = 0; d < 4; d++)
      for (s = 0; s < 4; s++) {
        fill(); refmove(ref + 20 + d, ref + 200 + s, n);
        if (__aeabi_memcpy(buf + 20 + d, buf + 200 + s, n) != buf + 20 + d || !same()) bad++;
        for (sh = -20; sh <= 20; sh++) {
          int dd = 100 + d, ss = 100 + d + sh + s;
          fill(); refmove(ref + dd, ref + ss, n);
          if (__aeabi_memmove(buf + dd, buf + ss, n) != buf + dd || !same()) bad++;
          if (((dd | ss | n) & 3) == 0) {
            fill(); refmove(ref + dd, ref + ss, n);
            if (__aeabi_memmove4(buf + dd, buf + ss, n) != buf + dd || !same()) bad++;
          }
        }
        if (s == 0) {
          int c = 0x1a5 + n;            /* bits above the byte are ignored */
          fill(); for (int i = 0; i < n; i++) ref[20 + d + i] = (unsigned char)c;
          if (__aeabi_memset(buf + 20 + d, n, c) != buf + 20 + d || !same()) bad++;
          fill(); for (int i = 0; i < n; i++) ref[20 + d + i] = 0;
          if (__aeabi_memclr(buf + 20 + d, n) != buf + 20 + d || !same()) bad++;
        }
      }
  printf("mem bad=%d\n", bad);

  bad = 0;
  uint32_t h = 0;
  for (int i = 0; i < 3000; i++) {
    int wn = rnd() % 65, wd = 1 + rnd() % 64;
    uint64_t nv = ((uint64_t)rnd() << 40) ^ ((uint64_t)rnd() << 20) ^ rnd() ^ ((uint64_t)rnd() << 52);
    uint64_t dv = ((uint64_t)rnd() << 40) ^ ((uint64_t)rnd() << 20) ^ rnd() ^ ((uint64_t)rnd() << 52);
    if (wn < 64) nv &= ((uint64_t)1 << wn) - 1;
    if (wd < 64) dv &= ((uint64_t)1 << wd) - 1;
    if (dv == 0) dv = 7;
    volatile uint64_t vn = nv, vd = dv;
    uint64_t q = vn / vd, r = vn % vd;
    uint32_t ql, qh, rl, rh;
    refdiv((uint32_t)nv, (uint32_t)(nv >> 32), (uint32_t)dv, (uint32_t)(dv >> 32), &ql, &qh, &rl, &rh);
    if ((uint32_t)q != ql || (uint32_t)(q >> 32) != qh || (uint32_t)r != rl || (uint32_t)(r >> 32) != rh) bad++;
    /* signed: negate either operand; the quotient truncates toward zero */
    volatile int64_t sn = (i & 1) ? -(int64_t)(nv >> 1) : (int64_t)(nv >> 1);
    volatile int64_t sd = (i & 2) ? -(int64_t)(dv >> 1 | 1) : (int64_t)(dv >> 1 | 1);
    int64_t sq = sn / sd, sr = sn % sd;
    if (sq * sd + sr != sn || (sr != 0 && (sr < 0) != (sn < 0)) || (sr < 0 ? -sr : sr) >= (sd < 0 ? -sd : sd)) bad++;
    h = h * 31 + (uint32_t)q + (uint32_t)(r >> 3) + (uint32_t)sq;
  }
  printf("div bad=%d hash=%08x\n", bad, (unsigned)h);
  return 0;
}
