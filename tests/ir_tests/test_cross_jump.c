/* Identical block tails that jump to the same place share one copy
 * (cross_jump).  A chain of fallible steps, each failure returning the same
 * way; tails with a goto label inside them, whose entry must keep running only
 * the part after it; tails that compare and branch; identical continue paths
 * in a loop. */
#include <stdio.h>
#include <stdint.h>

struct EU { uint32_t payload; uint16_t error; };

__attribute__((noinline)) static struct EU step(uint32_t k, uint32_t fail_at, uint32_t id)
{
  struct EU r = {k * 3 + id, 0};
  if (id == fail_at)
    r.error = (uint16_t)(100 + id);
  return r;
}

__attribute__((noinline)) static struct EU chain(uint32_t k, uint32_t fail_at)
{
  struct EU r, t;
  t = step(k, fail_at, 1);
  if (t.error) { r.payload = 0xaaaaaaaau; r.error = t.error; return r; }
  uint32_t a = t.payload;
  t = step(a, fail_at, 2);
  if (t.error) { r.payload = 0xaaaaaaaau; r.error = t.error; return r; }
  uint32_t b = t.payload;
  t = step(a + b, fail_at, 3);
  if (t.error) { r.payload = 0xaaaaaaaau; r.error = t.error; return r; }
  uint32_t c = t.payload;
  t = step(c ^ a, fail_at, 4);
  if (t.error) { r.payload = 0xaaaaaaaau; r.error = t.error; return r; }
  r.payload = t.payload + b + c;
  r.error = 0;
  return r;
}

/* The same tail twice, with a label inside the second copy: entering at the
 * label must run only what follows it. */
__attribute__((noinline)) static uint32_t labels(uint32_t k, volatile uint32_t *log)
{
  uint32_t acc = k;
  if (k & 1)
  {
    acc += 11;
    log[0] += acc;
    log[1] ^= acc;
    goto out;
  }
  if (k & 2)
    goto mid;
  acc += 11;
mid:
  log[0] += acc;
  log[1] ^= acc;
out:
  return acc * 3 + log[0] + log[1];
}

/* Identical tails that compute a comparison before jumping on. */
__attribute__((noinline)) static uint32_t compares(uint32_t a, uint32_t b, uint32_t c)
{
  uint32_t r;
  if (a > b)
  {
    r = (a < c) + (b < c) * 2;
    goto done;
  }
  if (b > c)
  {
    r = (a < c) + (b < c) * 2;
    goto done;
  }
  r = 7;
done:
  return r * 10 + a;
}

/* Identical continue paths in a loop. */
__attribute__((noinline)) static uint32_t loop(uint32_t n)
{
  uint32_t s = 0, t = 1;
  for (uint32_t i = 0; i < n; i++)
  {
    if (i % 3 == 0)
    {
      s += t;
      t = t * 5 + 1;
      continue;
    }
    if (i % 5 == 0)
    {
      s += t;
      t = t * 5 + 1;
      continue;
    }
    s ^= i;
  }
  return s + t;
}

int main(void)
{
  for (uint32_t f = 0; f <= 5; f++)
  {
    struct EU r = chain(7, f);
    printf("%u:%u ", r.payload, r.error);
  }
  printf("\n");
  volatile uint32_t log[2] = {0, 0};
  for (uint32_t k = 0; k < 6; k++)
    printf("%u ", labels(k, log));
  printf("\n%u %u %u %u\n", compares(5, 3, 9), compares(1, 4, 2), compares(1, 2, 3), compares(9, 1, 4));
  printf("%u %u\n", loop(20), loop(47));
  return 0;
}
