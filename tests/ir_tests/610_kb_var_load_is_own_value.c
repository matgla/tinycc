// The YasOS kernel through Zig's C backend at -O2: MaxProcFile.sync formats
// one value, so Zig builds `args = [1]Value{2048}` on the stack, keeps &args
// in a VAR and copies it into the argument slice -- `T <-- V14 [LOAD]`, the
// VAR reading its own value.  known_bits resolved that as a load THROUGH the
// address V14 holds and folded it to the slot's contents, so the slice
// pointer became 2048 and `ps` faulted formatting /proc.
#include <stdint.h>
#include <stdio.h>

struct slice_u8 { const uint8_t *ptr; uintptr_t len; };

struct value
{
  union
  {
    uint64_t u;
    int64_t s;
    struct slice_u8 string;
    uintptr_t pointer;
  } payload;
  uint8_t tag;
}; /* 16 bytes, 8-aligned, like vfmt.Value */

struct arr_1_value { struct value array[1]; };

struct slice_value { const struct value *ptr; uintptr_t len; };

__attribute__((noinline)) static uint32_t vprint(struct slice_u8 fmt, struct slice_value args)
{
  uint32_t t = (uint32_t)fmt.len;
  for (uintptr_t i = 0; i < args.len; i++)
    t += (uint32_t)args.ptr[i].payload.u + args.ptr[i].tag;
  return t;
}

static const struct { uint8_t array[5]; } fmt_str = {"{d}\n"};

/* MaxProcFile.sync as Zig's C backend wrote it. */
__attribute__((noinline)) static uint32_t maxproc_sync(void)
{
  struct arr_1_value t10, t16, t9, t17;
  struct value *t11;
  struct value t13, t15, t12;
  uint64_t *t14;
  const struct arr_1_value *t18;
  struct slice_value t19;
  t11 = &t10.array[(uintptr_t)0ul];
  t13.tag = UINT8_C(0);
  t14 = (uint64_t *)&t13.payload.u;
  (*t14) = UINT64_C(2048);
  t15 = t13;
  t12 = t15;
  goto b4;
b4:;
  (*t11) = t12;
  goto b3;
b3:;
  t16 = t10;
  t9 = t16;
  goto b2;
b2:;
  t17 = t9;
  t18 = (const struct arr_1_value *)&t17;
  t19.ptr = (const struct value *)t18;
  t19.len = (uintptr_t)1ul;
  return vprint((struct slice_u8){fmt_str.array, 4}, t19);
}

int main(void)
{
  printf("maxproc=%u\n", (unsigned)maxproc_sync());
  return 0;
}
