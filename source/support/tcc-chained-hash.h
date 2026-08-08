#ifndef TCC_CHAINED_HASH_H
#define TCC_CHAINED_HASH_H

#include "tcc.h"

/* Reusable append-only chained hash table.
 * Buckets store 1-based entry indices, matching TinyCC's ELF hash layout.
 * bucket_count must be a power of 2. */
typedef struct TCCChainedHash
{
  uint32_t bucket_count;
  uint32_t bucket_mask;
  uint32_t entry_capacity;
  uint32_t hashed_count;
  uint32_t *buckets;
  uint32_t *next;
  uint32_t *hashes;
} TCCChainedHash;

static inline void tcc_chained_hash_init(TCCChainedHash *hash, uint32_t bucket_count, uint32_t entry_capacity)
{
  hash->bucket_count = bucket_count;
  hash->bucket_mask = bucket_count - 1;
  hash->entry_capacity = entry_capacity;
  hash->hashed_count = 0;
  hash->buckets = tcc_mallocz(bucket_count * sizeof(*hash->buckets));
  hash->next = tcc_mallocz(entry_capacity * sizeof(*hash->next));
  hash->hashes = tcc_mallocz(entry_capacity * sizeof(*hash->hashes));
}

static inline void tcc_chained_hash_destroy(TCCChainedHash *hash)
{
  tcc_free(hash->buckets);
  tcc_free(hash->next);
  tcc_free(hash->hashes);
  hash->bucket_count = 0;
  hash->bucket_mask = 0;
  hash->entry_capacity = 0;
  hash->hashed_count = 0;
  hash->buckets = NULL;
  hash->next = NULL;
  hash->hashes = NULL;
}

static inline void tcc_chained_hash_clear(TCCChainedHash *hash)
{
  if (!hash->buckets)
    return;
  memset(hash->buckets, 0, hash->bucket_count * sizeof(*hash->buckets));
  hash->hashed_count = 0;
}

static inline void tcc_chained_hash_reserve(TCCChainedHash *hash, uint32_t entry_capacity)
{
  if (entry_capacity <= hash->entry_capacity)
    return;
  hash->next = tcc_realloc(hash->next, entry_capacity * sizeof(*hash->next));
  memset(hash->next + hash->entry_capacity, 0, (entry_capacity - hash->entry_capacity) * sizeof(*hash->next));
  hash->hashes = tcc_realloc(hash->hashes, entry_capacity * sizeof(*hash->hashes));
  memset(hash->hashes + hash->entry_capacity, 0, (entry_capacity - hash->entry_capacity) * sizeof(*hash->hashes));
  hash->entry_capacity = entry_capacity;
}

static inline void tcc_chained_hash_rebuild(TCCChainedHash *hash, uint32_t bucket_count)
{
  uint32_t *old_buckets = hash->buckets;
  uint32_t old_bucket_count = hash->bucket_count;
  uint32_t *new_buckets = tcc_mallocz(bucket_count * sizeof(*new_buckets));

  for (uint32_t bucket = 0; bucket < old_bucket_count; ++bucket)
  {
    uint32_t slot = old_buckets[bucket];
    while (slot)
    {
      uint32_t entry_index = slot - 1;
      uint32_t next_slot = hash->next[entry_index];
      uint32_t new_bucket = hash->hashes[entry_index] & (bucket_count - 1);
      hash->next[entry_index] = new_buckets[new_bucket];
      new_buckets[new_bucket] = slot;
      slot = next_slot;
    }
  }

  tcc_free(old_buckets);
  hash->buckets = new_buckets;
  hash->bucket_count = bucket_count;
  hash->bucket_mask = bucket_count - 1;
}

static inline uint32_t tcc_chained_hash_bucket_head(const TCCChainedHash *hash, uint32_t full_hash)
{
  return hash->buckets[full_hash & hash->bucket_mask];
}

static inline uint32_t tcc_chained_hash_slot_to_index(uint32_t slot)
{
  return slot - 1;
}

static inline uint32_t tcc_chained_hash_next_slot(const TCCChainedHash *hash, uint32_t slot)
{
  return slot ? hash->next[slot - 1] : 0;
}

static inline uint32_t tcc_chained_hash_entry_hash(const TCCChainedHash *hash, uint32_t entry_index)
{
  return hash->hashes[entry_index];
}

static inline void tcc_chained_hash_insert_head(TCCChainedHash *hash, uint32_t full_hash, uint32_t entry_index)
{
  uint32_t bucket = full_hash & hash->bucket_mask;
  hash->hashes[entry_index] = full_hash;
  hash->next[entry_index] = hash->buckets[bucket];
  hash->buckets[bucket] = entry_index + 1;
  hash->hashed_count++;
  if (hash->hashed_count > 2 * hash->bucket_count)
    tcc_chained_hash_rebuild(hash, hash->bucket_count << 1);
}

#endif