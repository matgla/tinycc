/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

/* -ffortify's sprintf / vsprintf (builtin/chk.c): what a call runs when the
 * compiler knows the destination's SIZE but not the output's length.  The
 * output is formatted into SIZE bytes at most; one that needed more ends the
 * program (__chk_fail, builtin.c) -- nothing past the destination is
 * written.  glibc's __sprintf_chk arguments (FLAG unused).  Apart from
 * builtin.c: only a program that calls them links vsnprintf. */

#include <stdarg.h>

typedef __SIZE_TYPE__ size_t;

extern int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
extern void __chk_fail(void) __attribute__((noreturn));

int __tcc_vsprintf_chk(char *s, int flag, size_t size, const char *fmt, va_list ap)
{
  int n;
  (void)flag;
  if (size == 0)
    __chk_fail();
  n = vsnprintf(s, size, fmt, ap);
  if (n >= 0 && (size_t)n >= size)
    __chk_fail();
  return n;
}

int __tcc_sprintf_chk(char *s, int flag, size_t size, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = __tcc_vsprintf_chk(s, flag, size, fmt, ap);
  va_end(ap);
  return n;
}
