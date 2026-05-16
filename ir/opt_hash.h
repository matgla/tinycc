/*
 *  TCC IR - Generic bump-allocated CSE hash table
 *
 *  Drop-in replacement for per-pass hand-rolled hash tables used in
 *  BB-scoped CSE passes.  Single pre-allocated pool avoids malloc per
 *  entry; clear is O(n_buckets) not O(entries).
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_HASH_H
#define TCC_IR_OPT_HASH_H

#include <stdint.h>

typedef struct IROptHashEntry
{
  uint32_t hash;
  int instruction_idx;
  int32_t result_vr;
  int extra[4];
  struct IROptHashEntry *next;
} IROptHashEntry;

typedef struct IROptHashTable
{
  IROptHashEntry **buckets;
  int n_buckets;
  IROptHashEntry *pool;
  int pool_count;
  int pool_capacity;
} IROptHashTable;

void ir_opt_hash_init(IROptHashTable *ht, int n_buckets, int max_entries);
void ir_opt_hash_clear(IROptHashTable *ht);
void ir_opt_hash_free(IROptHashTable *ht);

IROptHashEntry *ir_opt_hash_lookup(IROptHashTable *ht, uint32_t hash,
                                   int (*eq)(const IROptHashEntry *, const void *),
                                   const void *key);
IROptHashEntry *ir_opt_hash_insert(IROptHashTable *ht, uint32_t hash);

#endif /* TCC_IR_OPT_HASH_H */
