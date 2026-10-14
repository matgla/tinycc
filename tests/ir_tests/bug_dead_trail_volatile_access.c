/*
 *  TCC IR - Trailing writes to a local that carry a volatile access
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* dead_trail_addrvar deleted a write to a local x after x's last read without
 * asking whether the write performs a volatile access: `x = *r` lost its load
 * of *r, a volatile x lost its store.  Such writes now stay.  The reads are
 * observed through SysTick's COUNTFLAG, which a CSR read clears; the stores
 * are counted by test_dead_trail_volatile_codegen.py.  ptr_local_fwd likewise
 * turned `*(volatile int *)&x` into a plain access of x (f_vcast*). */
#include <stdio.h>

static volatile unsigned *const SYST_CSR = (volatile unsigned *)0xE000E010;
static volatile unsigned *const SYST_RVR = (volatile unsigned *)0xE000E014;
static volatile unsigned *const SYST_CVR = (volatile unsigned *)0xE000E018;

#define COUNTFLAG (1u << 16)

/* x is read (into c), then overwritten by a volatile load and never read
 * again: the write to x is dead, the load feeding it is not. */
__attribute__((noinline)) int f_load(volatile unsigned *r, int k)
{
  int x = k;
  int c = x;
  x = *r;
  return c;
}

/* The volatile operand folded into an ALU op (V <- *T ADD #1). */
__attribute__((noinline)) int f_add(volatile unsigned *r, int k)
{
  int x = k;
  int c = x;
  x = *r + 1;
  return c;
}

/* x's address taken and read through before the overwrite. */
__attribute__((noinline)) int f_addr(volatile unsigned *r, int k)
{
  int x = k;
  int *p = &x;
  int c = *p;
  x = *r;
  return c;
}

/* The overwrite is a store through &x whose value is the volatile load. */
__attribute__((noinline)) int f_ptr(volatile unsigned *r, int k)
{
  int x = k;
  int *p = &x;
  int c = *p;
  *p = *r;
  return c;
}

/* No read of x before the overwrite: never touched by the pass. */
__attribute__((noinline)) int f_ctrl(volatile unsigned *r, int k)
{
  int x = k;
  x = *r;
  return k;
}

/* A volatile local: its trailing store must stay (asm-shape test). */
__attribute__((noinline)) int f_vlocal(int k)
{
  volatile int x = k;
  int c = x;
  x = c + 9;
  return c;
}

/* Through &x of a volatile local. */
__attribute__((noinline)) int f_vlocal_ptr(int k)
{
  volatile int x = k;
  volatile int *p = &x;
  int c = *p;
  *p = c + 9;
  return c;
}

/* A volatile access through &x of a plain local (ptr_local_fwd turned it
 * into a direct access of x, which then lived in a register). */
__attribute__((noinline)) int f_vcast(int k)
{
  int x = k;
  int c = x;
  *(volatile int *)&x = c + 9;
  return c;
}

__attribute__((noinline)) int f_vcast_read(int k)
{
  int x = k;
  int c = *(volatile int *)&x;
  return c + k;
}

/* Non-volatile neighbour: the trailing store through &x still goes. */
__attribute__((noinline)) int f_plain(int k)
{
  int x = k;
  int *p = &x;
  int c = *p;
  *p = c + 9;
  return c;
}

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
  int ok = 1;
  /* Control: the flag is set before the call under test. */
  ok &= arm_countflag();
  printf("armed %d\n", flag_left());

  int (*fns[])(volatile unsigned *, int) = {f_load, f_add, f_addr, f_ptr, f_ctrl};
  const char *names[] = {"f_load", "f_add", "f_addr", "f_ptr", "f_ctrl"};
  for (int i = 0; i < 5; i++)
  {
    ok &= arm_countflag();
    int v = fns[i](SYST_CSR, 3);
    printf("%s %d flag %d\n", names[i], v, flag_left());
  }
  printf("f_vlocal %d %d\n", f_vlocal(3), f_vlocal_ptr(4));
  printf("f_vcast %d %d\n", f_vcast(6), f_vcast_read(7));
  printf("f_plain %d\n", f_plain(5));
  printf("timer %s\n", ok ? "ok" : "stuck");
  *SYST_CSR = 0;
  return 0;
}
