/*
 *  TCC IR - A volatile load into a dead char/short local
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* vstore's delayed char/short cast retyped the unloaded source to the
 * destination type before its LOAD, dropping VT_VOLATILE, so SSA DCE deleted
 * `short x = *r;` whenever x was dead -- at every level.  The cast now records
 * volatile_access first.  Word reads are observed through SysTick's
 * COUNTFLAG (a CSR read clears it); the narrow reads are counted by
 * test_narrow_volatile_load_codegen.py. */
#include <stdio.h>

static volatile unsigned *const SYST_CSR = (volatile unsigned *)0xE000E010;
static volatile unsigned *const SYST_RVR = (volatile unsigned *)0xE000E014;
static volatile unsigned *const SYST_CVR = (volatile unsigned *)0xE000E018;

#define COUNTFLAG (1u << 16)

__attribute__((noinline)) void w_short(volatile unsigned *r) { short x = *r; }
__attribute__((noinline)) void w_char(volatile unsigned *r) { char x = *r; }
__attribute__((noinline)) void w_schar(volatile unsigned *r) { signed char x = *r; (void)x; }
__attribute__((noinline)) void w_assign(volatile unsigned *r) { unsigned short x; x = *r; }
__attribute__((noinline)) void w_array(volatile unsigned *r) { short a[2]; a[0] = *r; }
__attribute__((noinline)) void w_bool(volatile unsigned *r) { _Bool x = *r; }
__attribute__((noinline)) void w_int(volatile unsigned *r) { int x = *r; }

/* Narrow reads, counted by the asm-shape test. */
__attribute__((noinline)) void h_short(volatile short *r) { short x = *r; }
__attribute__((noinline)) void b_uchar(volatile unsigned char *r) { unsigned char x = *r; (void)x; }
__attribute__((noinline)) void h_uchar(volatile unsigned short *r) { unsigned char x = *r; }
__attribute__((noinline)) void d_short(volatile unsigned long long *r) { short x = *r; }
__attribute__((noinline)) void h_twice(volatile short *r) { short x = *r; short y = *r; }

/* Non-volatile neighbour: the dead load still goes. */
__attribute__((noinline)) void p_short(short *r) { short x = *r; }
__attribute__((noinline)) void p_char(int *r) { char x = *r; }

/* A narrow local that is read keeps its value. */
__attribute__((noinline)) int v_value(volatile int *r) { signed char x = *r; short y = *r; return x + y; }

/* Run SysTick until it has wrapped (COUNTFLAG set), then stop it: a stopped
 * timer keeps the flag until something reads the CSR. */
static int arm_countflag(void)
{
  *SYST_CSR = 0;
  *SYST_RVR = 0xFF;
  *SYST_CVR = 0;
  *SYST_CSR = 5; /* enable, processor clock, no interrupt */
  unsigned wraps = 0, prev = *SYST_CVR;
  for (long i = 0; i < 50000000L && wraps < 3; i++)
  {
    unsigned cur = *SYST_CVR;
    if (cur > prev)
      wraps++;
    prev = cur;
  }
  *SYST_CSR = 4; /* stop; COUNTFLAG survives the write */
  return wraps >= 3;
}

static int flag_left(void)
{
  return (*SYST_CSR & COUNTFLAG) != 0;
}

int main(void)
{
  int ok = arm_countflag();
  printf("armed %d\n", flag_left());

  void (*fns[])(volatile unsigned *) = {w_short, w_char, w_schar, w_assign, w_array, w_bool, w_int};
  const char *names[] = {"w_short", "w_char", "w_schar", "w_assign", "w_array", "w_bool", "w_int"};
  for (int i = 0; i < 7; i++)
  {
    ok &= arm_countflag();
    fns[i](SYST_CSR);
    printf("%s flag %d\n", names[i], flag_left());
  }

  short hs = -5;
  unsigned char uc = 200;
  unsigned short us = 0x1234;
  unsigned long long ull = 0x123456789ULL;
  h_short(&hs);
  b_uchar(&uc);
  h_uchar(&us);
  d_short(&ull);
  h_twice(&hs);
  p_short(&hs);
  p_char((int *)&ull);
  int big = 0x12345681;
  printf("v_value %d\n", v_value(&big));
  printf("timer %s\n", ok ? "ok" : "stuck");
  *SYST_CSR = 0;
  return 0;
}
