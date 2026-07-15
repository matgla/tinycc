/*
 *  TCC Memory Utilities - Scope-owned pointers
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "memory/unique_ptr.h"

#include "tcc.h"

#include <string.h>

static void *tcc_unique_ptr_load(const void *owner)
{
  void *value;

  memcpy(&value, owner, sizeof(value));
  return value;
}

static void tcc_unique_ptr_store(void *owner, void *value)
{
  memcpy(owner, &value, sizeof(value));
}

void *tcc_unique_ptr_release(void *owner)
{
  void *value = tcc_unique_ptr_load(owner);

  tcc_unique_ptr_store(owner, NULL);
  return value;
}

void tcc_unique_ptr_reset(void *owner, void *value)
{
  void *old_value = tcc_unique_ptr_load(owner);

  if (old_value == value) {
    return;
  }

  tcc_unique_ptr_store(owner, value);
  tcc_free(old_value);
}

void tcc_unique_ptr_move(void *destination, void *source)
{
  void *value;

  if (destination == source) {
    return;
  }

  value = tcc_unique_ptr_release(source);
  tcc_unique_ptr_reset(destination, value);
}

void tcc_unique_ptr_cleanup(void *owner)
{
  tcc_free(tcc_unique_ptr_release(owner));
}
