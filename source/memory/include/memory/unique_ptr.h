/*
 *  TCC Memory Utilities - Scope-owned pointers
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

void tcc_unique_ptr_cleanup(void *owner);
void *tcc_unique_ptr_release(void *owner);
void tcc_unique_ptr_reset(void *owner, void *value);
void tcc_unique_ptr_move(void *destination, void *source);

#define unique_ptr(type) type *__attribute__((cleanup(tcc_unique_ptr_cleanup)))
#define unique_ptr_release(owner) ((__typeof__(owner))tcc_unique_ptr_release(&(owner)))
#define unique_ptr_reset(owner, value) tcc_unique_ptr_reset(&(owner), (value))
#define unique_ptr_move(destination, source) tcc_unique_ptr_move(&(destination), &(source))

