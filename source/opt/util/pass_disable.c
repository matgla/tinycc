/*
 *  TCC IR - TCC_DISABLE_PASS lookup
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include <ctype.h>

#include "ir.h"
#include "opt_utils.h"

#if CONFIG_TCC_DEBUG_ENV

TCC_DBG_ENV_STR(pass_disable_list, "TCC_DISABLE_PASS")

/* Bisection helper: TCC_DISABLE_PASS is a comma/space separated pass-name list. */
int tcc_ir_opt_pass_disabled(const char *name)
{
  const char *disabled = pass_disable_list();
  if (!disabled || !name)
    return 0;
  const char *p = disabled;
  size_t nlen = strlen(name);
  while (*p) {
    while (*p == ',' || isspace((unsigned char)*p))
      p++;
    if (!*p)
      break;
    const char *start = p;
    while (*p && *p != ',' && !isspace((unsigned char)*p))
      p++;
    size_t len = p - start;
    if (len == nlen && strncmp(start, name, len) == 0)
      return 1;
  }
  return 0;
}

#endif /* CONFIG_TCC_DEBUG_ENV */
