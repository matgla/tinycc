/* Wyhash's mum through zig.h's 128-bit multiply without __int128: two u64s
 * widened to u128 (zero high halves) and multiplied by __multi3. */
#include <stdint.h>
typedef struct { __attribute__((aligned(8))) uint64_t lo; int64_t hi; } i128;
i128 __multi3(i128 a, i128 b);

uint64_t mum(uint64_t *a, uint64_t *b)
{
  i128 x = {*a, 0}, y = {*b, 0};
  i128 r = __multi3(x, y);
  *a = r.lo;
  *b = (uint64_t)r.hi;
  return *a ^ *b;
}
