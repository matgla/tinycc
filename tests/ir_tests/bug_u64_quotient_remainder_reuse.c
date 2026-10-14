/*
 *  TCC regression - shared unsigned constant quotient and remainder
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdint.h>
#include <stdio.h>

typedef uint64_t U;
#define NOINLINE __attribute__((noinline))
#define PAIR(name, divisor) \
  NOINLINE void name(U n, U *out) { out[0] = n / divisor; out[1] = n % divisor; }

PAIR(pair_3, 3ULL)
PAIR(pair_7, 7ULL)
PAIR(pair_10, 10ULL)
PAIR(pair_1000000, 1000000ULL)
PAIR(pair_32, 4294967295ULL)
PAIR(pair_33, 4294967297ULL)
PAIR(pair_63, 9223372036854775809ULL)
PAIR(pair_max, 18446744073709551615ULL)

NOINLINE void timestamp_separate(U now, uint32_t *out)
{
  out[0] = (uint32_t)(now / 1000000ULL);
  out[1] = (uint32_t)((now % 1000000ULL) * 1000ULL);
}

NOINLINE void timestamp_reuse(U now, uint32_t *out)
{
  U q = now / 1000000ULL;
  out[0] = (uint32_t)q;
  out[1] = (uint32_t)((now - q * 1000000ULL) * 1000ULL);
}

NOINLINE void reverse_pair(U n, U *out)
{
  out[1] = n % 1000000ULL;
  out[0] = n / 1000000ULL;
}

NOINLINE void changed_pair(U n, U *out)
{
  out[0] = n / 10ULL;
  n += 0x100000001ULL;
  out[1] = n % 10ULL;
}

NOINLINE void memory_pair(U *n, U *out)
{
  out[0] = *n / 10ULL;
  out[1] = *n % 10ULL;
}

NOINLINE void conditional_pair(U n, int take, U *out)
{
  out[0] = take ? n / 1000000ULL : n;
  out[1] = n % 1000000ULL;
}

NOINLINE U reference_div(U n, U d) { return n / d; }
NOINLINE U reference_mod(U n, U d) { return n % d; }

int main(void)
{
  static const U ds[] = {3ULL, 7ULL, 10ULL, 1000000ULL, 4294967295ULL,
                        4294967297ULL, 9223372036854775809ULL, ~0ULL};
  void (*pairs[])(U, U *) = {pair_3, pair_7, pair_10, pair_1000000,
                            pair_32, pair_33, pair_63, pair_max};
  U state = 0x123456789abcdef0ULL;
  for (unsigned i = 0; i < sizeof(ds) / sizeof(ds[0]); ++i) {
    U d = ds[i];
    for (unsigned j = 0; j < 128; ++j) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      U n = state;
      if (j == 0) n = 0;
      if (j == 1) n = d - 1;
      if (j == 2) n = d;
      if (j == 3) n = d + 1;
      if (j == 4) n = ~0ULL;
      if (j == 5) n = 0xffffffffULL;
      if (j == 6) n = 0x100000000ULL;
      if (j == 7) n = 0x8000000000000000ULL;
      U out[2];
      pairs[i](n, out);
      if (out[0] != reference_div(n, d) || out[1] != reference_mod(n, d))
        return 1;
      uint32_t a[2], b[2];
      timestamp_separate(n, a);
      timestamp_reuse(n, b);
      U q = reference_div(n, 1000000ULL), r = reference_mod(n, 1000000ULL);
      if (a[0] != (uint32_t)q || a[1] != (uint32_t)(r * 1000ULL) ||
          a[0] != b[0] || a[1] != b[1])
        return 2;
      reverse_pair(n, out);
      if (out[0] != q || out[1] != r)
        return 3;
      conditional_pair(n, j & 1, out);
      if (out[0] != ((j & 1) ? q : n) || out[1] != r)
        return 4;
      changed_pair(n, out);
      if (out[0] != reference_div(n, 10ULL) ||
          out[1] != reference_mod(n + 0x100000001ULL, 10ULL))
        return 5;
      out[0] = n;
      memory_pair(out, out);
      q = reference_div(n, 10ULL);
      if (out[0] != q || out[1] != reference_mod(q, 10ULL)) {
        printf("alias failed %u %u n=%llx q=%llx out=%llx,%llx\n", i, j,
               (unsigned long long)n, (unsigned long long)q,
               (unsigned long long)out[0], (unsigned long long)out[1]);
        return 6;
      }
    }
  }
  puts("u64 quotient/remainder reuse ok");
  return 0;
}
