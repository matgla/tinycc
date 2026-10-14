/* Reduced from kernel.c parser_Parser_validate__9133 (Zig CBE output of
 * dynamic_loader/source/parser.zig Parser.validate), the data_relocations loop
 * `for (parser.data_relocations.relocations) |rel| { ... }` over a
 * packed struct(u64).  The CBE copies each element into a local (t99), takes
 * its address once (t100 = &t99) and re-reads the record for EVERY field access
 * through the same two reused pointer locals: `t101 = t100; t74 = t101; *t74`.
 * t101/t74 therefore have one definition per field read (multi-def copies).
 *
 * Default build: the record makes a strd/add/ldrd stack round-trip and is
 * re-read from the slot at every field access.
 * -DSINGLE_DEF_COPIES: the same chain spelled with one fresh local per hop
 * (single-def copies) -- ptr_local_fwd resolves it, no stack traffic.
 *
 * libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -c kernel-validate-reused-copy-local-roundtrip.c
 */
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifdef SINGLE_DEF_COPIES
#define REREAD() do { DataRelocation const *a_ = t100; uint64_t const *b_ = a_; t75 = *b_; } while (0)
#else
#define REREAD() do { t101 = t100; t74 = t101; t75 = (*t74); } while (0) /* CBE spelling */
#endif

typedef uint64_t DataRelocation;            /* packed struct(u64) { to: u32, from: u30, section: u2 } */
struct slice_DataRelocation { DataRelocation *ptr; uintptr_t len; };
struct Header { uint32_t pad[30]; uint32_t const_rodata_length; uint32_t got_length; };
struct errunion_usize { uintptr_t payload; uint16_t error; };
struct errunion_void { uint16_t error; };

static inline uint64_t zig_shr_u64(uint64_t v, uint8_t s) { return v >> s; }
static inline uint32_t zig_u32_truncate_u64(uint64_t v, uint8_t b) { return (uint32_t)v & (b == 32 ? 0xffffffffu : ((1u << b) - 1)); }
static inline uint8_t zig_u8_truncate_u64(uint64_t v, uint8_t b) { return (uint8_t)v & (uint8_t)((1u << b) - 1); }

static struct errunion_usize span_end(uintptr_t const a0, uintptr_t const a1, uintptr_t const a2, uintptr_t const a3)
{
  uintptr_t t5; bool t2, t3, t4; struct errunion_usize t6;
  t2 = a0 > a3;
  if (t2) { t3 = true; goto b1; }
  t2 = a2 != 0;
  if (t2) { t5 = a3 - a0; t5 = t5 / a2; t2 = a1 > t5; t4 = t2; goto b2; }
  t4 = false;
b2: t3 = t4;
b1: if (t3) return (struct errunion_usize){ .error = 67, .payload = 0xaaaaaaaau };
  t5 = a1 * a2; t5 = a0 + t5;
  t6.payload = t5; t6.error = 0;
  return t6;
}

struct errunion_void validate_data_relocs(struct Header const *h_in, struct slice_DataRelocation const *relocs, uintptr_t data_bss)
{
  struct Header const *t9 = h_in;
  struct Header const *const *t10 = &t9;
  struct Header const *t8;
  DataRelocation t98, t99;
  DataRelocation const *t100, *t101;
  uint64_t const *t74;
  uint64_t t75;
  uint32_t t20, t21;
  uint8_t t92, t93;
  uintptr_t t31, t33, t94 = 0;
  bool t5, t14;
  struct slice_DataRelocation t97 = *relocs;
  struct errunion_usize t41;
  t31 = t97.len;
loop:
  t33 = t94;
  t14 = t33 < t31;
  if (t14) {
    t98 = t97.ptr[t33];
    t99 = t98;
    t100 = (DataRelocation const *)&t99;
    REREAD();            /* rel.to */
    t21 = zig_u32_truncate_u64(zig_shr_u64(t75, 0), 32);
    t8 = *t10;
    memcpy(&t20, &t8->const_rodata_length, sizeof(uint32_t));
    t14 = t21 < t20;
    if (t14) { t5 = true; goto b44; }
    REREAD();            /* rel.to again */
    t20 = zig_u32_truncate_u64(zig_shr_u64(t75, 0), 32);
    t20 = t20 % 4u;
    t5 = t20 != 0;
b44:
    if (t5) return (struct errunion_void){ .error = 67 };
    REREAD();            /* rel.to a third time */
    t21 = zig_u32_truncate_u64(zig_shr_u64(t75, 0), 32);
    t41 = span_end(t21, 1, 4, data_bss);            /* _ = try span_end(rel.to, 1, 4, data_bss) */
    if (t41.error) return (struct errunion_void){ .error = t41.error };
    REREAD();            /* rel.section */
    t92 = zig_u8_truncate_u64(zig_shr_u64(t75, 32), 2);
    t93 = t92;
    t14 = t93 == 3;
    if (t14) {
      REREAD();          /* rel.from */
      t21 = zig_u32_truncate_u64(zig_shr_u64(t75, 34), 30);
      t8 = *t10;
      memcpy(&t20, &t8->got_length, sizeof(uint32_t));
      t20 = t20 / 8u;
      t5 = t21 >= t20;
    } else
      t5 = false;
    if (t5) return (struct errunion_void){ .error = 67 };
    t33 = t33 + 1;
    t94 = t33;
    goto loop;
  }
  return (struct errunion_void){ .error = 0 };
}
