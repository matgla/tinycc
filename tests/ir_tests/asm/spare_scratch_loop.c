/* Every low register a parameter and the callee-saved ones taken, the
 * inner loop's byte load still needs a scratch: a callee-saved register the
 * function leaves unused, saved by the prologue, rather than a store and
 * reload of r0 around it on every iteration (test_spare_scratch_no_loop_spill). */
#include <stdint.h>
struct sym { uint32_t a; char name[]; };
static inline uint32_t span(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static uint32_t size_of(const struct sym *s, uint8_t align)
{
  uint32_t n = 4 + span(s->name) + 1;
  uint32_t m = (uint32_t)align - 1;
  return (n + m) & ~m;
}
uint32_t walk(const uint8_t *base, uint32_t count, uint8_t align, uint32_t *out, uint32_t k1, uint32_t k2)
{
  uint32_t off = 0, acc = k1;
  for (uint32_t i = 0; i < count; i++)
  {
    const struct sym *s = (const struct sym *)(base + off);
    uint32_t sz = size_of(s, align);
    out[i] = sz ^ acc;
    acc = acc * k2 + sz;
    off += sz;
  }
  return acc;
}
