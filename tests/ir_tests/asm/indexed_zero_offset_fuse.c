/* mem_inline's `[addr, #0]` STORE_INDEXED/LOAD_INDEXED (an align(1) word
 * memcpy through a pointer, as the Zig backend writes it) must fuse into the
 * shl+add address like a plain LOAD/STORE does: one `str.w/ldr.w [Rn, Rm,
 * lsl #2]`, not a separate `lsls; adds`. */
typedef unsigned u32;
typedef unsigned long size_t;
void *memcpy(void *, const void *, size_t);

void put(unsigned char *base, const u32 *idx, const u32 *val, u32 n) {
  for (u32 i = 0; i < n; i++) {
    u32 v = val[i];
    memcpy(base + idx[i] * 4, &v, 4); /* store side: [Rn, Rm, lsl #2] */
  }
}

u32 get(unsigned char *base, const u32 *idx, u32 n) {
  u32 t = 0;
  for (u32 i = 0; i < n; i++) {
    u32 v;
    memcpy(&v, base + idx[i] * 4, 4); /* load side: [Rn, Rm, lsl #2] */
    t += v;
  }
  return t;
}
