/* Loop-invariant loads of POINTER-typed struct fields must hoist out of loops
 * (ssa:licm_field_ptr, docs/bugs/pool-mark-per-page-invariant-reloads.md),
 * while the writes that can legally change such a field must keep the reload.
 *
 * The alias rule is C11 6.5p7: a store whose lvalue type is a non-pointer
 * scalar (`p->used[i>>5] |= bit`) can never modify an object of pointer type,
 * so the field-pointer reads are invariant under it.  The locks below pin the
 * cases where a reload MUST stay: a pointer-typed element store, union-member
 * punning, and a volatile field retargeted through an alias. */
#include <stdio.h>
#include <stdint.h>

typedef unsigned u32;

struct Pool { u32 *used; u32 *marked; u32 count; };

__attribute__((noinline))
static void mark(struct Pool *p, u32 start, u32 end)
{
  for (u32 i = start; i < end; i++) {
    if (!(p->marked[i >> 5] & (1u << (i & 31)))) {
      p->used[i >> 5] |= 1u << (i & 31);
      p->count += 1;
    }
  }
}

/* A store of a POINTER through a loaded base can legally retarget the very
 * field another load reads: parr names `cur`'s own bytes, so the element
 * store changes what `cur` holds before the loop reads it again.  The cur
 * read must NOT be hoisted. */
struct Ret { u32 **parr; u32 *cur; };

__attribute__((noinline))
static u32 retarget_walk(struct Ret *s, u32 **targets, u32 n)
{
  u32 acc = 0;
  for (u32 i = 0; i < n; i++) {
    s->parr[0] = targets[i]; /* pointer element store onto the cur field */
    acc = acc * 4u + (*s->cur == 0u ? 3u : 0u);
    *s->cur = 1u; /* marks which target cur named this iteration */
  }
  return acc;
}

/* Reading a union member other than the one last written is legal punning:
 * the `mword` stores must invalidate the `marked` reads even though one side
 * is an int and the other a pointer. */
union Mk { u32 *marked; u32 mword; };

__attribute__((noinline))
static u32 union_pun(union Mk *mk, u32 words)
{
  u32 acc = 0;
  for (u32 i = 0; i < words; i++) {
    if (((uintptr_t)mk->marked & 7u) != (i & 7u))
      acc++;
    mk->mword += 1;
  }
  return acc;
}

/* A volatile-qualified field member must be re-read every iteration, even
 * when the retargeting write goes through a different pointer to the same
 * struct (two params: no same-root reasoning applies). */
struct VPool { u32 *volatile used; };

__attribute__((noinline))
static u32 volatile_walk(struct VPool *p, struct VPool *alias, u32 n)
{
  u32 acc = 0;
  for (u32 i = 0; i < n; i++) {
    acc += *p->used;
    if (i == 0)
      alias->used = alias->used + 1; /* retarget through the alias */
  }
  return acc;
}

static volatile u32 sink;

int main(void)
{
  /* 1. the hoist case itself: the bitmaps live outside the descriptor, the
   *    element stores are uint32 writes and the only field store bumps
   *    `count` at a disjoint offset, so both pointer loads are invariant. */
  {
    static u32 ubuf[8], mbuf[8];
    struct Pool pool = { ubuf, mbuf, 0 };
    for (u32 w = 0; w < 8; w++) {
      ubuf[w] = 0;
      mbuf[w] = (w & 1) ? 0xAAAAAAAAu : 0x55555555u;
    }
    mark(&pool, 0, 256);
    u32 want_count = 0;
    for (u32 w = 0; w < 8; w++) {
      u32 want = ~mbuf[w];
      want_count += __builtin_popcount(want);
      if (ubuf[w] != want) {
        printf("bad ubuf[%u] = %x want %x\n", w, ubuf[w], want);
        return 1;
      }
    }
    if (pool.count != want_count) {
      printf("bad count %u != %u\n", pool.count, want_count);
      return 1;
    }
    /* zero-trip path: nothing may be touched at all */
    for (u32 w = 0; w < 8; w++) ubuf[w] = 0;
    struct Pool z = { ubuf, mbuf, 0 };
    mark(&z, 7, 7);
    if (z.count != 0 || ubuf[0] != 0) {
      printf("bad zero-trip\n");
      return 1;
    }
  }

  /* 2. pointer-typed element stores retarget the field mid-loop: each
   * iteration first rewrites `cur` through parr (which names cur's own
   * bytes), then reads it.  All four words get written exactly once and
   * every read sees 0 (3 in the base-4 accumulator).  A hoisted cur read
   * would keep targeting words[0]: acc 0x0ff... and only w0 written. */
  {
    static u32 w0, w1, w2, w3;
    static u32 *targets[4] = { &w0, &w1, &w2, &w3 };
    struct Ret s;
    s.parr = (u32 **)&s.cur; /* the array IS the cur field */
    s.cur = targets[0];
    u32 acc = retarget_walk(&s, targets, 4);
    if (acc != 3u * 64u + 3u * 16u + 3u * 4u + 3u || w0 != 1 || w1 != 1 || w2 != 1 || w3 != 1) {
      printf("bad retarget acc=%u %u %u %u %u\n", acc, w0, w1, w2, w3);
      return 2;
    }
  }

  /* 3. union-member pun: the pointer read must track the int writes.
   *    Reads see 0x10, 0x11, ... so the low 3 bits always match i and acc
   *    stays 0; a (wrongly) hoisted read would report 31 mismatches. */
  {
    union Mk mk;
    mk.mword = 0x10;
    u32 acc = union_pun(&mk, 32);
    if (acc != 0 || mk.mword != 0x10 + 32) {
      printf("bad pun acc=%u end=%x\n", acc, mk.mword);
      return 3;
    }
  }

  /* 4. volatile member: the field read must happen every iteration.
   *    words[0]=5, words[1]=9; after i=0 retargets, i=1 must read 9. */
  {
    static u32 words[2] = { 5, 9 };
    struct VPool v = { &words[0] };
    u32 acc = volatile_walk(&v, &v, 2);
    if (acc != 5u + 9u) {
      printf("bad volatile acc=%u\n", acc);
      return 4;
    }
  }

  puts("ok");
  return 0;
}
