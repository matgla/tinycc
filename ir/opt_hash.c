/*
 *  TCC IR - Generic bump-allocated CSE hash table
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_hash.h"

void ir_opt_hash_init(IROptHashTable *ht, int n_buckets, int max_entries)
{
  ht->n_buckets = n_buckets;
  ht->buckets = tcc_mallocz(n_buckets * sizeof(IROptHashEntry *));
  ht->pool = tcc_malloc(max_entries * sizeof(IROptHashEntry));
  ht->pool_count = 0;
  ht->pool_capacity = max_entries;
}

void ir_opt_hash_clear(IROptHashTable *ht)
{
  memset(ht->buckets, 0, ht->n_buckets * sizeof(IROptHashEntry *));
  ht->pool_count = 0;
}

void ir_opt_hash_free(IROptHashTable *ht)
{
  tcc_free(ht->buckets);
  tcc_free(ht->pool);
  ht->buckets = NULL;
  ht->pool = NULL;
}

IROptHashEntry *ir_opt_hash_lookup(IROptHashTable *ht, uint32_t hash,
                                   int (*eq)(const IROptHashEntry *, const void *),
                                   const void *key)
{
  int bucket = (int)(hash % (uint32_t)ht->n_buckets);
  IROptHashEntry *e = ht->buckets[bucket];
  while (e) {
    if (e->hash == hash && eq(e, key))
      return e;
    e = e->next;
  }
  return NULL;
}

IROptHashEntry *ir_opt_hash_insert(IROptHashTable *ht, uint32_t hash)
{
  if (ht->pool_count >= ht->pool_capacity)
    return NULL;
  IROptHashEntry *e = &ht->pool[ht->pool_count++];
  e->hash = hash;
  int bucket = (int)(hash % (uint32_t)ht->n_buckets);
  e->next = ht->buckets[bucket];
  ht->buckets[bucket] = e;
  return e;
}
