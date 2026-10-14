/*
 *  TCC regression - unsigned 64-bit constant division
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>
typedef unsigned long long U;
__attribute__((noinline)) U div_0(U n) { return n / 1ULL; }
__attribute__((noinline)) U mod_0(U n) { return n % 1ULL; }
__attribute__((noinline)) U div_1(U n) { return n / 2ULL; }
__attribute__((noinline)) U mod_1(U n) { return n % 2ULL; }
__attribute__((noinline)) U div_2(U n) { return n / 3ULL; }
__attribute__((noinline)) U mod_2(U n) { return n % 3ULL; }
__attribute__((noinline)) U div_3(U n) { return n / 7ULL; }
__attribute__((noinline)) U mod_3(U n) { return n % 7ULL; }
__attribute__((noinline)) U div_4(U n) { return n / 10ULL; }
__attribute__((noinline)) U mod_4(U n) { return n % 10ULL; }
__attribute__((noinline)) U div_5(U n) { return n / 31ULL; }
__attribute__((noinline)) U mod_5(U n) { return n % 31ULL; }
__attribute__((noinline)) U div_6(U n) { return n / 37ULL; }
__attribute__((noinline)) U mod_6(U n) { return n % 37ULL; }
__attribute__((noinline)) U div_7(U n) { return n / 1000000ULL; }
__attribute__((noinline)) U mod_7(U n) { return n % 1000000ULL; }
__attribute__((noinline)) U div_8(U n) { return n / 4294967295ULL; }
__attribute__((noinline)) U mod_8(U n) { return n % 4294967295ULL; }
__attribute__((noinline)) U div_9(U n) { return n / 4294967296ULL; }
__attribute__((noinline)) U mod_9(U n) { return n % 4294967296ULL; }
__attribute__((noinline)) U div_10(U n) { return n / 4294967297ULL; }
__attribute__((noinline)) U mod_10(U n) { return n % 4294967297ULL; }
__attribute__((noinline)) U div_11(U n) { return n / 9223372036854775807ULL; }
__attribute__((noinline)) U mod_11(U n) { return n % 9223372036854775807ULL; }
__attribute__((noinline)) U div_12(U n) { return n / 9223372036854775808ULL; }
__attribute__((noinline)) U mod_12(U n) { return n % 9223372036854775808ULL; }
__attribute__((noinline)) U div_13(U n) { return n / 9223372036854775809ULL; }
__attribute__((noinline)) U mod_13(U n) { return n % 9223372036854775809ULL; }
__attribute__((noinline)) U div_14(U n) { return n / 18446744073709551615ULL; }
__attribute__((noinline)) U mod_14(U n) { return n % 18446744073709551615ULL; }
__attribute__((noinline)) U variable_div(U n, U d) { return n / d; }
__attribute__((noinline)) U variable_mod(U n, U d) { return n % d; }
__attribute__((noinline)) long long signed_div(long long n) { return n / 10LL; }
__attribute__((noinline)) long long signed_mod(long long n) { return n % 7LL; }
__attribute__((noinline)) U zero_div(U n) { return n / 0ULL; }
__attribute__((noinline)) U zero_mod(U n) { return n % 0ULL; }
int main(void)
{
  U ds[] = {1ULL,2ULL,3ULL,7ULL,10ULL,31ULL,37ULL,1000000ULL,4294967295ULL,4294967296ULL,4294967297ULL,9223372036854775807ULL,9223372036854775808ULL,9223372036854775809ULL,18446744073709551615ULL};
  U (*divs[])(U) = {div_0,div_1,div_2,div_3,div_4,div_5,div_6,div_7,div_8,div_9,div_10,div_11,div_12,div_13,div_14};
  U (*mods[])(U) = {mod_0,mod_1,mod_2,mod_3,mod_4,mod_5,mod_6,mod_7,mod_8,mod_9,mod_10,mod_11,mod_12,mod_13,mod_14};
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
      if (j == 8) n = (~0ULL / d) * d;
      if (j == 9) n = (~0ULL / d) * d - 1;
      if (divs[i](n) != variable_div(n, d) || mods[i](n) != variable_mod(n, d)) {
        printf("failed %u %u\n", i, j);
        return 1;
      }
    }
  }
  if (signed_div(-101) != -10 || signed_mod(-101) != -3) return 2;
  if (signed_div(-9223372036854775807LL - 1) != -922337203685477580LL) return 3;
  puts("constant divmod ok");
  return 0;
}
