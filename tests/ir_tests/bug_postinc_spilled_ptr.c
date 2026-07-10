/* Regression: post-increment fusion (LOAD/STORE + ADD -> LOAD_POSTINC/
 * STORE_POSTINC) was unsound when the loop-carried pointer SPILLED.  The ARM
 * post-indexed writeback (ldr/str [rN],#imm) updates rN in place, but the IR
 * cannot model that side effect, so for a spilled base the increment was lost
 * (the pointer's stack home was never updated) and `*q` re-read the same byte
 * forever — tcc itself hung here, in parse_number() (tccpp.c), compiling ANY
 * integer literal (the self-hosted compiler froze on every input).
 *
 * Fixed by removing the post-increment fusion pass: `*q++` now lowers to an
 * explicit LOAD + ADD whose incremented result is written back to the
 * pointer's home register/spill slot correctly.
 *
 * This mirrors tccpp.c parse_number almost verbatim (local pointer `q` seeded
 * from a buffer, conditionally pre-incremented, then advanced with `*q++`
 * inside a loop carrying a 64-bit accumulator) — the exact shape that used to
 * fuse + spill.  Without the fix this test never returns (infinite loop) at -O1.
 */
#include <stdio.h>

typedef unsigned long long ull;

char token_buf[64];

static int parse(int b)
{
  ull n = 0, n1;
  int t, ov = 0;
  char *q;
  q = token_buf;
  if (b == 10 && *q == '0') {
    b = 8;
    q++;
  }
  n = 0;
  while (1) {
    t = *q++;
    if (t == '\0')
      break;
    if (t >= 'a')
      t = t - 'a' + 10;
    else if (t >= 'A')
      t = t - 'A' + 10;
    else
      t = t - '0';
    if (t >= b)
      return -1;
    n1 = n;
    n = n * b + t;
    if (n1 >= 0x1000000000000000ULL && n / b != n1)
      ov = 1;
  }
  return (int)n + ov;
}

int main(void)
{
  const char *s = "1234567";
  int i;
  for (i = 0; s[i]; i++)
    token_buf[i] = s[i];
  token_buf[i] = '\0';
  int v = parse(10);
  printf("v=%d\n", v);
  return (v == 1234567) ? 0 : 1;
}
