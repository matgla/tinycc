/*
 *  TCC IR - constant-size clear expansion regression
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <string.h>

#define CLEAR(N) \
  __attribute__((noinline)) void clear##N(char *p) { memset(p, 0, N); }
CLEAR(0)
CLEAR(1)
CLEAR(2)
CLEAR(3)
CLEAR(4)
CLEAR(5)
CLEAR(6)
CLEAR(7)
CLEAR(8)
CLEAR(9)
CLEAR(10)
CLEAR(11)
CLEAR(12)
CLEAR(13)
CLEAR(14)
CLEAR(15)
CLEAR(16)
CLEAR(17)

extern void __aeabi_memset(void *, unsigned, int);
extern void __aeabi_memclr(void *, unsigned);
extern void __aeabi_memclr4(void *, unsigned);
__attribute__((noinline)) void aeabi_set(char *p) { __aeabi_memset(p, 16, 0); }
__attribute__((noinline)) void aeabi_clear(char *p) { __aeabi_memclr(p, 15); }
__attribute__((noinline)) void aeabi_aligned(unsigned *p) { __aeabi_memclr4(p, 16); }
__attribute__((noinline)) void truncated_zero(char *p) { memset(p, 256, 16); }
__attribute__((noinline)) void fill16(char *p) { memset(p, 0x1a5, 16); }
__attribute__((noinline)) void *return16(char *p) { return memset(p, 0, 16); }
__attribute__((noinline)) void dynamic(char *p, unsigned n) { memset(p, 0, n); }

static int evaluations;
__attribute__((noinline)) char *next(char *p) { ++evaluations; return p + 1; }
__attribute__((noinline)) void evaluated(char *p) { memset(next(p), 0, 15); }

int main(void)
{
  void (*clears[])(char *) = {
    clear0, clear1, clear2, clear3, clear4, clear5, clear6, clear7, clear8,
    clear9, clear10, clear11, clear12, clear13, clear14, clear15, clear16, clear17
  };
  unsigned char buf[32];
  int errors = 0;
  for (unsigned n = 0; n <= 17; ++n)
    for (unsigned off = 0; off < 4; ++off) {
      for (unsigned i = 0; i < sizeof buf; ++i) buf[i] = 0xa5;
      clears[n]((char *)buf + off);
      for (unsigned i = 0; i < sizeof buf; ++i)
        errors += buf[i] != (i >= off && i < off + n ? 0 : 0xa5);
    }
  for (unsigned off = 0; off < 4; ++off) {
    for (unsigned i = 0; i < sizeof buf; ++i) buf[i] = 0x5a;
    fill16((char *)buf + off);
    for (unsigned i = 0; i < sizeof buf; ++i)
      errors += buf[i] != (i >= off && i < off + 16 ? 0xa5 : 0x5a);
    errors += return16((char *)buf + off) != buf + off;
    dynamic((char *)buf + off, 17);
    for (unsigned i = 0; i < sizeof buf; ++i)
      errors += buf[i] != (i >= off && i < off + 17 ? 0 : 0x5a);
  }
  for (unsigned i = 0; i < sizeof buf; ++i) buf[i] = 0xa5;
  evaluated((char *)buf);
  errors += evaluations != 1;
  for (unsigned i = 0; i < sizeof buf; ++i)
    errors += buf[i] != (i >= 1 && i < 16 ? 0 : 0xa5);
  for (unsigned i = 0; i < sizeof buf; ++i) buf[i] = 0xa5;
  aeabi_set((char *)buf + 1);
  for (unsigned i = 0; i < sizeof buf; ++i)
    errors += buf[i] != (i >= 1 && i < 17 ? 0 : 0xa5);
  for (unsigned i = 0; i < sizeof buf; ++i) buf[i] = 0xa5;
  aeabi_clear((char *)buf + 1);
  for (unsigned i = 0; i < sizeof buf; ++i)
    errors += buf[i] != (i >= 1 && i < 16 ? 0 : 0xa5);
  unsigned aligned[5] = {1, 2, 3, 4, 5};
  aeabi_aligned(aligned);
  errors += aligned[0] != 0 || aligned[1] != 0 || aligned[2] != 0 || aligned[3] != 0 || aligned[4] != 5;
  for (unsigned i = 0; i < sizeof buf; ++i) buf[i] = 0xa5;
  truncated_zero((char *)buf + 1);
  for (unsigned i = 0; i < sizeof buf; ++i)
    errors += buf[i] != (i >= 1 && i < 17 ? 0 : 0xa5);
  printf("errors=%d\n", errors);
  return errors != 0;
}
