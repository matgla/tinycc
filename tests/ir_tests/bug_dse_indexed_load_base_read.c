/* A pointer array read through an indexed LOAD keeps its initializer.

 * dse's write-only addr-TMP scan took `src1 = Addr[array]` with a TEMP dest as
 * "the TEMP is defined as that address" -- but in a LOAD_INDEXED the dest is the
 * value *loaded from* the array, not the array's address. When that loaded
 * pointer was only ever used as a store base, the array was not marked read and
 * the stores filling it (records[j] = p, q, r) were deleted, so the scan read a
 * garbage base. The forced-inline scan then returned a stale length after the
 * buffer was rewritten. */
#include <stdio.h>

typedef unsigned u32;
typedef unsigned char u8;

__attribute__((always_inline)) static inline u32 forced_scan(const u8 *p)
{
  u32 n = 0;
again:
  if (!p[n])
    return n;
  ++n;
  goto again;
}

u32 forced_name(const u8 *p)
{
  return forced_scan(p + 4);
}

static const u8 *next_record(const u8 *p, u8 a)
{
  return p + 4u + (u32)a;
}

u32 walk_records(const u8 *p, u32 count, u8 a)
{
  u32 bytes = 0;
  for (u32 i = 0; i < count; ++i) {
    bytes += forced_name(p);
    p = next_record(p, a);
  }
  return bytes;
}

static volatile u32 sink;

int main(void)
{
  u8 p[192], q[192], r[192];
  u32 total = 0;
  for (u32 n = 0; n < 40; n++) {
    u32 lengths[3] = {n, (n * 5u + 7u) % 130u, (n * 11u + 3u) % 130u};
    u8 *records[3] = {p, q, r};
    for (u32 j = 0; j < 3; j++) {
      for (u32 i = 0; i < 4u + lengths[j]; i++)
        records[j][i] = (u8)(1u + i % 254u);
      records[j][4u + lengths[j]] = 0;
    }
    /* The scan through the rewritten buffer must see the new length. */
    if (forced_name(p) != n) {
      printf("bad n=%u forced=%u\n", n, forced_name(p));
      return 3;
    }
    /* The buffer itself must actually have been written: a fix that deletes the
     * stores but constant-folds the scan would still pass the check above. */
    for (u32 k = 0; k < n; k++)
      if (p[4u + k] != (u8)(1u + (4u + k) % 254u)) {
        printf("bad content n=%u\n", n);
        return 4;
      }
    total += walk_records(p, 3, (u8)n);
  }
  sink = total;
  puts("ok");
  return 0;
}
