/* Variable-trip `for (; n >= K; n -= K)` latches emit a redundant CMP.
 * The flag-setting SUBS already answers the compare it feeds:
 *
 *   ./bin/armv8m-tcc -O2 -fno-pic -c \
 *     docs/bugs/repro/loop_latch_cmp_after_subs_2026-10-09.c -o /tmp/latch.o
 *   arm-none-eabi-objdump -d /tmp/latch.o
 *
 * Look for `subs r,#K` immediately followed by `cmp r,#K` plus the
 * conditional branch: the CMP is dead, `bcs`/`bcc` on the SUBS' carry
 * is the same test.  The `for (; n; n -= K)` form (count_ne) shows what
 * the codegen already does when the test is the SUBS' own result.
 */
#include <stddef.h>
#include <stdint.h>

__attribute__((noinline))
int count_ge(unsigned n) {
  int hits = 0;
  for (; n >= 4; n -= 4)
    ++hits;
  return hits;
}

__attribute__((noinline))
int count_ne(unsigned n) {
  int hits = 0;
  for (; n; n -= 4)
    ++hits;
  return hits;
}

__attribute__((noinline))
void copy_words_ge(uint32_t *dw, const uint32_t *sw, size_t n) {
  for (; n >= 16; n -= 16, dw += 4, sw += 4) {
    dw[0] = sw[0];
    dw[1] = sw[1];
    dw[2] = sw[2];
    dw[3] = sw[3];
  }
}
