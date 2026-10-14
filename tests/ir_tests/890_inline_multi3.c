/* __multi3 calls expand at the call site (try_inline_multi3): zig.h without
 * __int128 multiplies its 128-bit integers through compiler-rt's __multi3.
 * The reference below is what -O0 calls; -O1 and up must agree with it on
 * full 128-bit operands, negative ones, wrap-around, zero-extended u64s
 * (Wyhash's mum) and a result written over its own operand. */
#include <stdint.h>

typedef struct { __attribute__((aligned(8))) uint64_t lo; uint64_t hi; } u128;
typedef struct { __attribute__((aligned(8))) uint64_t lo; int64_t hi; } i128;

/* reference (compiler-rt's algorithm on 32-bit words) */
__attribute__((noinline)) i128 __multi3(i128 a, i128 b)
{
  uint32_t aw[4] = {(uint32_t)a.lo, (uint32_t)(a.lo >> 32), (uint32_t)a.hi, (uint32_t)((uint64_t)a.hi >> 32)};
  uint32_t bw[4] = {(uint32_t)b.lo, (uint32_t)(b.lo >> 32), (uint32_t)b.hi, (uint32_t)((uint64_t)b.hi >> 32)};
  uint32_t r[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; i++)
  {
    uint64_t carry = 0;
    for (int j = 0; i + j < 4; j++)
    {
      uint64_t t = (uint64_t)aw[i] * bw[j] + r[i + j] + carry;
      r[i + j] = (uint32_t)t;
      carry = t >> 32;
    }
  }
  i128 out;
  out.lo = r[0] | (uint64_t)r[1] << 32;
  out.hi = (int64_t)(r[2] | (uint64_t)r[3] << 32);
  return out;
}

static i128 as_i(u128 x) { i128 r; r.lo = x.lo; r.hi = (int64_t)x.hi; return r; }
static u128 as_u(i128 x) { u128 r; r.lo = x.lo; r.hi = (uint64_t)x.hi; return r; }
static u128 mul_u128(u128 a, u128 b) { return as_u(__multi3(as_i(a), as_i(b))); }
static u128 widen(uint64_t x) { u128 r; r.lo = x; r.hi = 0; return r; }

__attribute__((noinline)) static uint64_t mum(uint64_t *a, uint64_t *b)
{
  u128 r = mul_u128(widen(*a), widen(*b));
  *a = r.lo;
  *b = r.hi;
  return *a ^ *b;
}

/* the result buffer is an operand's own storage */
__attribute__((noinline)) static u128 square_in_place(u128 x)
{
  x = mul_u128(x, x);
  return x;
}

__attribute__((noinline)) static i128 chain(i128 a, i128 b, i128 c)
{
  i128 t = __multi3(a, b);
  t = __multi3(t, c);
  return __multi3(t, a);
}

static volatile uint64_t vin[6] = {0x9e3779b97f4a7c15ull, 0xffffffffffffffffull, 0x0123456789abcdefull,
                                   0xfedcba9876543210ull, 0x00000000ffffffffull, 0x8000000000000001ull};

#define CHECK(n, got, lo_, hi_) do { if ((got).lo != (lo_) || (uint64_t)(got).hi != (hi_)) return n; } while (0)

int main(void)
{
  uint64_t a = vin[0], b = vin[3];
  if (mum(&a, &b) != (0x5534de8ee5c7db50ull ^ 0x9d8375c53037f2ffull) || a != 0x5534de8ee5c7db50ull || b != 0x9d8375c53037f2ffull)
    return 1;
  u128 x = {vin[1], vin[1]};
  u128 y = {vin[2], vin[3]};
  CHECK(2, mul_u128(x, y), 0xfedcba9876543211ull, 0x0123456789abcdefull);
  CHECK(3, square_in_place(y), 0xdca5e20890f2a521ull, 0x446efc86a6f7108cull);
  i128 m1 = {vin[1], -1}, p = {vin[2], 7}, q = {vin[5], (int64_t)vin[4]};
  CHECK(4, __multi3(m1, p), 0xfedcba9876543211ull, 0xfffffffffffffff8ull);
  CHECK(5, chain(p, q, m1), 0xa35a1df76f0d5adfull, 0xcd7134d2e4e018d1ull);
  CHECK(6, mul_u128(widen(vin[4]), widen(vin[4])), 0xfffffffe00000001ull, 0x0000000000000000ull);
  return 0;
}
