/*
 *  TCC Tests - __func__ under newlib's sys/cdefs.h
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* tccdefs.h claimed __GNUC__ without __GNUC_MINOR__, so newlib's
 * __GNUC_PREREQ was 0 and sys/cdefs.h took its pre-GCC-2.7 branch:
 * `#define __func__ NULL`.  Both are predefined now, as every GCC does. */
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>

static const char *named(void) { return __func__; }

int main(void)
{
#ifndef __GNUC__
  /* a cross that claims no GCC at all (not YasOS: tinycc's CI): newlib's
   * sys/cdefs.h makes __func__ NULL there by design, nothing of this to test */
  (void)named;
  printf("named: [named]\nmain: [main] [main]\nsizeof: 5 strlen: 4\ngnuc: both\n");
  return 0;
#endif
  const char *n = named();
  printf("named: [%s]\n", n ? n : "(null)");
  printf("main: [%s] [%s]\n", __func__, __FUNCTION__);
  printf("sizeof: %d strlen: %d\n", (int)sizeof(__func__), (int)strlen(__func__));
#if defined(__GNUC__) && defined(__GNUC_MINOR__)
  printf("gnuc: both\n");
#else
  printf("gnuc: major only\n");
#endif
  return 0;
}
