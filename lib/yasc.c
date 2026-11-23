/*
 *  yasc.c - the yasc dialect's run time: error_name().
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

/* Every yasc file puts a record for each error name it uses into the
 * yasc_errname section -- {u16 value, u8 flags, u8 len, name, NUL}, padded
 * to 4 -- and a file calling error_name() adds every errno name its errno.h
 * defines.  The linker defines the section's bounds.  flags: 0 a set gave
 * the value, 1 a hash of the name, 2 an errno macro.  An errno name wins
 * over an alias (FileNotFound = ENOENT gives "ENOENT"). */

extern const unsigned char __start_yasc_errname[] __attribute__((weak));
extern const unsigned char __stop_yasc_errname[] __attribute__((weak));

const char *__yasc_error_name(int e)
{
  const unsigned char *p = __start_yasc_errname, *end = __stop_yasc_errname;
  const char *alias = 0;

  if (p && end)
    for (; p + 4 <= end; p += (4 + p[3] + 1 + 3) & ~3u)
    {
      if (!p[3] || (int)(p[0] | p[1] << 8) != e)
        continue;
      if (p[2] == 2)
        return (const char *)p + 4;
      if (!alias)
        alias = (const char *)p + 4;
    }
  if (alias)
    return alias;
  return e ? "(unknown error)" : "(no error)";
}
