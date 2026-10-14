/* kernel-romfs-fileheader-return-copies: reduced from kernel.c
 *   fs_romfs_file_header_FileHeader_deinit__16419 (called once per directory step).
 *   (The `return try f(...)` half is fixed; its shape is tests/ir_tests/bug_sret_nrvo_partial_forward.c.)
 *
 *   libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -ffunction-sections -mfloat-abi=hard \
 *     -mfpu=fpv5-sp-d16 -c libs/tinycc/docs/bugs/repro/kernel-romfs-fileheader-return-copies.c -o /tmp/r.o
 */
#include <stdint.h>
#include <stddef.h>

struct slice_u8 { uint8_t *ptr; uintptr_t len; };
struct Allocator { void *ptr; void const *vtable; };
struct Hdr {                                           /* FileHeader: 160 bytes */
  uint32_t head[21];
  struct Allocator _allocator;                         /* +84 */
  uint32_t mid[2];
  struct slice_u8 _name_heap;                          /* +100 */
  uint32_t tail[13];
};

extern void allocator_free(struct Allocator a, struct slice_u8 s);
extern int32_t step(struct Hdr *h);

/* FileHeader.deinit: a null test guarding a cold call, spelled as kernel.c spells it. */
static void deinit(struct Hdr *const a0) {
 struct Hdr *const *t1;
 struct Hdr *t2;
 struct Hdr *t0;
 struct slice_u8 *t3;
 struct slice_u8 t4;
 struct slice_u8 t6;
 struct Allocator *t7;
 struct Allocator t8;
 _Bool t5;
 t0 = a0;
 t1 = (struct Hdr *const *)&t0;
 t2 = (*t1);
 t3 = (struct slice_u8 *)&t2->_name_heap;
 t4 = (*t3);
 t5 = t4.ptr != NULL;
 if (t5) {
  t6 = t4;
  t2 = (*t1);
  t7 = (struct Allocator *)&t2->_allocator;
  t8 = (*t7);
  allocator_free(t8, t6);
  t2 = (*t1);
  t3 = (struct slice_u8 *)&t2->_name_heap;
  (*t3) = (struct slice_u8){NULL,(uintptr_t)0xaaaaaaaaul};
  goto zig_block_0;
 }
 goto zig_block_0;

zig_block_0:;
 return;
}

/* The directory walk's step loop: deinit runs once per entry stepped over. */
int32_t walk(struct Hdr *h, int32_t n) {
  int32_t found = 0;
  for (int32_t i = 0; i < n; i++) {
    deinit(h);
    found += step(h);
  }
  return found;
}

/* deinit has 23 call sites in kernel.c: it stays out of line, and each site tests heap.ptr first (inline:guard_head). */
void drop(struct Hdr *h) { deinit(h); }
void drop_twice(struct Hdr *a, struct Hdr *b) { deinit(a); deinit(b); }
