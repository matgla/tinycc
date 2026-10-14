/* kernel-romfs-load-name-scan-and-reader-tail: reduced from kernel.c
 * fs_romfs_file_header_FileHeader_load__16417 (Zig C backend shape).
 *
 *   libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -ffunction-sections -fdata-sections \
 *     -DYASLIBC_REAL_BOOL -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fvisibility=hidden \
 *     -include apps/zig/zig_c_prelude.h -I ~/.zvm/master/lib \
 *     -c libs/tinycc/docs/bugs/repro/kernel-romfs-load-name-scan-and-reader-tail.c -o /tmp/r.o
 *
 * reader_tail(): the `self._reader = FileReader.init_at(self._device_file, off, data_offset)`
 *   tail on its own -- 8 loads of the 32-byte IFile held across the u64 field stores, the
 *   i64 PACK64 goes through a stack slot (strd; ldr same slot), and spills.
 * load(): the whole hot path; the 16-byte name scan is the inlined mem.findScalarPos.
 *   In this small TU the scan IS rotated (5 insns/byte); in the full kernel.c it is not
 *   (6 insns/byte, header-dup prefix cap) -- see the report for the TCC_DUMP_FUNC recipe. */
#define ZIG_TARGET_SOFT_COMPILER_RT_F80_ABI
#define ZIG_TARGET_SOFT_COMPILER_RT_F128_ABI
#define ZIG_TARGET_MAX_INT_ALIGNMENT 8
#include "zig.h"

struct slice_u8 { uint8_t const *ptr; uintptr_t len; };
struct opt_usize { uintptr_t payload; bool is_null; };
struct IFile { void const *vtable; void *ptr; uint32_t mf[4]; int32_t *refcount; uint32_t pad; }; /* 32 B */
struct FileReader { uint64_t _offset; uint64_t _data_offset; struct IFile _device_file; };      /* 48 B */
struct FileHeader {
  struct FileReader _reader;
  struct IFile _device_file;
  void const *_mapped_memory;
  uint32_t _allocator[2];
  int32_t _filesystem_offset;
  uint32_t _image_size;
  struct slice_u8 _name_heap;
  uint32_t _size, _specinfo, _next;
  uint8_t _filetype;
  uint8_t _name_storage[31];
  uint8_t _name_len;
};
struct errunion_void { uint16_t error; };

extern uint64_t slow_long_name(struct FileHeader *self, int32_t off); /* rare path */

/* mem.findScalarPos / debug.assert / math.isPowerOfTwo: copied from kernel.c (renamed). */
static void debug_assert(bool const a0) {
 bool t0;
 t0 = !a0;
 if (t0) {
  zig_unreachable();
 }
 goto zig_block_0;
zig_block_0:;
 return;
}
static bool math_isPowerOfTwo(void) {
 debug_assert(true);
 return true;
}
static struct opt_usize findScalarPos(struct slice_u8 const a0, uintptr_t const a1, uint8_t const a2) {
 struct slice_u8 const *t1;
 uintptr_t const *t2;
 uintptr_t t3, t11, t12, t14, t7, t8;
 uint32_t t4, t5;
 struct slice_u8 t9, t0;
 uint8_t const *t10 zig_nonstring;
 struct opt_usize t15;
 bool t6;
 uint8_t t13;
 t0 = a0;
 t1 = (struct slice_u8 const *)&t0;
 t2 = &t1->len;
 t3 = (*t2);
 t4 = zig_u32_bitCast_u32(a1, UINT8_C(32));
 t5 = zig_u32_bitCast_u32(t3, UINT8_C(32));
 t6 = t4 >= t5;
 if (t6) {
  return (struct opt_usize){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
 }
 goto zig_block_0;
zig_block_0:;
 t7 = a1;
 goto zig_block_2;
zig_block_2:;
 t6 = math_isPowerOfTwo();
 if (t6) {
  goto zig_block_1;
 }
 goto zig_block_1;
zig_block_1:;
 t8 = (uintptr_t)0ul;
 t3 = t7;
 t9 = (*t1);
 t10 = t9.ptr;
 t10 = (uint8_t const *)(((uintptr_t)t10) + (t3*sizeof(uint8_t)));
 t11 = t9.len;
 t3 = t11 - t3;
 t9.ptr = t10;
 t9.len = t3;
 t3 = t7;
 t11 = t9.len;
 zig_loop_71:
 t12 = t8;
 t5 = zig_u32_bitCast_u32(t12, UINT8_C(32));
 t4 = zig_u32_bitCast_u32(t11, UINT8_C(32));
 t6 = t5 < t4;
 if (t6) {
  t13 = t9.ptr[t12];
  t14 = t3 + t12;
  t6 = t13 == a2;
  if (t6) {
   t15.is_null = false;
   t15.payload = t14;
   return t15;
  }
  goto zig_block_5;
zig_block_5:;
  (void)0;
  goto zig_block_4;
 }
 goto zig_block_3;
zig_block_4:;
 t12 = t12 + (uintptr_t)1ul;
 t8 = t12;
 goto zig_loop_71;
zig_block_3:;
 return (struct opt_usize){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
}
static struct opt_usize findScalar(struct slice_u8 const a0, uint8_t const a1) {
  return findScalarPos(a0, 0, a1);
}
static struct FileReader init_at(struct IFile const a0, uint64_t const a1, uint64_t const a2) {
  struct FileReader t0;
  t0._device_file = a0;
  t0._offset = a1;
  t0._data_offset = a2;
  return t0;
}

struct errunion_void load(struct FileHeader *const a0, int32_t const a1) {
  uint8_t buf[32];
  int64_t entry = (int64_t)(a1 - a0->_filesystem_offset);
  uint8_t const *img = (uint8_t const *)a0->_mapped_memory;
  if (img == NULL || entry < 0 || (uint32_t)entry + 32u > a0->_image_size)
    return (struct errunion_void){ .error = 5 };
  memcpy(buf, img + (uint32_t)entry, 32);
  uint32_t w0, w1, w2;
  memcpy(&w0, buf, 4); memcpy(&w1, buf + 4, 4); memcpy(&w2, buf + 8, 4);
  w0 = __builtin_bswap32(w0); w1 = __builtin_bswap32(w1); w2 = __builtin_bswap32(w2);
  a0->_filetype = (uint8_t)(w0 & 7);
  a0->_name_len = 0;
  a0->_name_heap = (struct slice_u8){ NULL, (uintptr_t)0xaaaaaaaaul };
  a0->_size = w2;
  a0->_specinfo = w1;
  a0->_next = w0 & 0xfffffff0u;
  uint64_t data_offset = 32;                       /* `var data_offset: u64 = first_read_bytes` */
  struct slice_u8 chunk = { buf + 16, 16 };
  struct opt_usize r = findScalar(chunk, 0);       /* std.mem.indexOfScalar over a [16]u8 */
  if (!r.is_null) {
    a0->_name_len = (uint8_t)r.payload;
    if (r.payload != 0) memcpy(a0->_name_storage, buf + 16, r.payload);
  } else {
    data_offset = slow_long_name(a0, a1);          /* long names: rare */
  }
  struct FileReader *dst = &a0->_reader;
  struct IFile df = a0->_device_file;
  uint64_t off = (uint64_t)(int64_t)a1;            /* zig_u64_intCast_i32 */
  struct FileReader t64 = init_at(df, off, data_offset);
  *dst = t64;
  return (struct errunion_void){ .error = 0 };
}

/* The tail alone (no scan, no calls). */
void reader_tail(struct FileHeader *const h, int32_t const a1, uint64_t const data_offset) {
  struct IFile df = h->_device_file;
  struct FileReader t64 = init_at(df, (uint64_t)(int64_t)a1, data_offset);
  h->_reader = t64;
}
