/*
 *  TCC - Read-only wrapper and call-reuse regression
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* TCC Tests - Read-only calls across forward branches and inlined address copies */
typedef unsigned usize;

__attribute__((noinline)) usize scan_forward(const unsigned char *p)
{
  usize i = 0;
  while (p[i]) ++i;
  return i;
}

__attribute__((noinline)) static void grow(unsigned char *p)
{
  usize n = scan_forward(p);
  p[n] = 'x';
  p[n + 1] = 0;
}

static volatile usize observed;

usize clean_diamond(const unsigned char *p, usize mode)
{
  usize a = scan_forward(p + 4);
  usize x;
  if (mode) x = (mode * 3u) ^ 17u;
  else x = mode + 7u;
  return a * 100u + scan_forward(p + 4) + x;
}

usize conditional_store(unsigned char *p, usize mode)
{
  usize a = scan_forward(p + 4);
  if (mode) { p[4 + a] = 'x'; p[5 + a] = 0; }
  return a * 100u + scan_forward(p + 4);
}

usize conditional_call(unsigned char *p, usize mode)
{
  usize a = scan_forward(p + 4);
  if (mode) grow(p + 4);
  return a * 100u + scan_forward(p + 4);
}

usize conditional_volatile(const unsigned char *p, usize mode)
{
  usize a = scan_forward(p + 4);
  if (mode) observed++;
  return a * 100u + scan_forward(p + 4);
}

usize changed_pointer(const unsigned char *p, usize mode)
{
  usize a = scan_forward(p + 4);
  if (mode) p++;
  return a * 100u + scan_forward(p + 4);
}

usize skipped_first(const unsigned char *p, usize mode)
{
  usize a = 7;
  if (mode) a = scan_forward(p + 4);
  return a * 100u + scan_forward(p + 4);
}

usize external_entry(const unsigned char *p, usize mode)
{
  usize a = 7;
  if (mode) goto join;
  a = scan_forward(p + 4);
  if (p[1]) a += p[1];
join:
  return a * 100u + scan_forward(p + 4);
}

usize loop_store(unsigned char *p, usize count)
{
  usize a = scan_forward(p + 4);
  for (usize i = 0; i < count; ++i) p[4 + i] = 'x';
  p[4 + count] = 0;
  return a * 100u + scan_forward(p + 4);
}

usize changed_base(const unsigned char *p, const unsigned char *q, usize mode)
{
  usize a = scan_forward(p + 4);
  if (mode) p = q;
  return a * 100u + scan_forward(p + 4);
}

usize asm_barrier(unsigned char *p)
{
  usize a = scan_forward(p + 4);
  __asm__ volatile ("" ::: "memory");
  return a * 100u + scan_forward(p + 4);
}
