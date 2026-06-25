/*
 * TCC Bug: I64/F64 IR spill error
 *
 * This test reproduces the compiler error:
 * "load_to_dest_ir I64/F64: dest.pr1 is spilled, need IR-level handling"
 *
 * The bug occurs when:
 * 1. Returning 64-bit values from functions that access volatile memory
 * 2. Using 1UL constants that may promote to 64-bit in certain contexts
 *
 * Uses volatile stack variables instead of hardware registers so the test
 * can run on any host without requiring specific hardware.
 */

#include <stdio.h>

/* Shared volatile variables that mimic hardware register accesses */
static volatile unsigned int fake_reg = 0xDEADBEEF;
static volatile unsigned int fake_ctrl = 0x00FF00FF;

/* Minimal reproduction: 64-bit return from volatile access */
unsigned long long test_i64_return(void)
{
  /* Access a volatile location and return as 64-bit - triggers the bug */
  return (unsigned long long)fake_reg;
}

/* Simpler case: just cast to unsigned long long and return */
unsigned long long test_i64_cast(unsigned int x)
{
  return (unsigned long long)x;
}

/* Test 1UL constant in volatile context */
unsigned int test_ul_constant(void)
{
  /* 1UL << 24 may be treated as 64-bit */
  fake_ctrl |= (1UL << 24);
  return fake_ctrl;
}

/* 64-bit arithmetic result */
unsigned long long test_i64_mul(unsigned int a, unsigned int b)
{
  return (unsigned long long)a * (unsigned long long)b;
}

int main(void)
{
  printf("Testing I64/F64 IR bug reproductions\n");

  unsigned long long v1 = test_i64_return();
  printf("i64_return: %llu\n", v1);

  unsigned long long v2 = test_i64_cast(0x12345678);
  printf("i64_cast: 0x%llx\n", v2);

  unsigned int v3 = test_ul_constant();
  printf("ul_constant: 0x%x\n", v3);

  unsigned long long v4 = test_i64_mul(100000, 200000);
  printf("i64_mul: %llu\n", v4);

  printf("Tests completed\n");
  return 0;
}
