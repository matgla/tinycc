// The YasOS kernel through Zig's C backend at -O2: yasld's find_thunk (as Zig
// emitted it) reads each thunk slot's {r9, fn} words into two zeroed locals
// with memcpy through `asBytes(&local)`, then compares them.  Built into the
// kernel by tcc -O2 it never matched, so a data-initializer pointer to a
// function got a different value than `&f` (gcc-torture 930608-1).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct slice_u8 { uint8_t *ptr; uintptr_t len; };
struct holder { struct slice_u8 data; uint8_t generated; };
struct unique { struct holder *thunks; };
struct arr_4_u8 { uint8_t array[4]; };
struct opt_usize { uintptr_t payload; uint8_t is_null; };

const uintptr_t indirect_call_thunk_template_size = 16;

static struct arr_4_u8 *mem_asBytes(uintptr_t *const a0) { return (struct arr_4_u8 *)a0; }

static struct opt_usize find_thunk(struct unique *const a0, uintptr_t const a1, uintptr_t const a2, uintptr_t const a3)
{
  struct unique *const *t1;
  struct unique *t2, *t0;
  struct holder **t3;
  struct holder *t4, *t6, *t7;
  struct holder *const *t8;
  uintptr_t t10, t13, t16, t9, t17, t18;
  uint32_t t11, t12;
  struct slice_u8 *t14;
  uintptr_t *t15;
  struct arr_4_u8 *t19;
  struct slice_u8 t20;
  uint8_t *t21, *t24;
  struct arr_4_u8 *t22;
  struct opt_usize t25;
  int t5, t23;
  t0 = a0;
  t1 = (struct unique *const *)&t0;
  t2 = (*t1);
  t3 = (struct holder **)&t2->thunks;
  t4 = (*t3);
  t5 = t4 != NULL;
  if (t5) {
    t6 = t4;
    t7 = t6;
    t8 = (struct holder *const *)&t7;
    t9 = 0;
  loop:
    t10 = t9;
    t11 = (uint32_t)t10;
    t12 = (uint32_t)a1;
    t5 = t11 < t12;
    if (t5) {
      t10 = t9;
      t13 = (*(((uintptr_t const *)&indirect_call_thunk_template_size)));
      t13 = t10 * t13;
      t10 = (*(((uintptr_t const *)&indirect_call_thunk_template_size)));
      t10 = t13 + t10;
      t6 = (*t8);
      t14 = (struct slice_u8 *)&t6->data;
      t15 = &t14->len;
      t16 = (*t15);
      t12 = (uint32_t)t10;
      t11 = (uint32_t)t16;
      t5 = t12 > t11;
      if (t5)
        goto b1;
      goto b4;
    b4:;
      t17 = 0;
      t18 = 0;
      t19 = mem_asBytes(&t17);
      t6 = (*t8);
      t14 = (struct slice_u8 *)&t6->data;
      t16 = t13 + 8;
      t10 = t13 + 8;
      t10 = t10 + 4;
      t20 = (*t14);
      t21 = t20.ptr;
      t21 = (uint8_t *)(((uintptr_t)t21) + (t16 * sizeof(uint8_t)));
      t16 = t10 - t16;
      t20.ptr = t21;
      t20.len = t16;
      t21 = t20.ptr;
      t22 = (struct arr_4_u8 *)t21;
      memcpy(t19->array, t22->array, 4 * sizeof(uint8_t));
      t19 = mem_asBytes(&t18);
      t6 = (*t8);
      t14 = (struct slice_u8 *)&t6->data;
      t16 = t13 + 12;
      t10 = t13 + 12;
      t10 = t10 + 4;
      t20 = (*t14);
      t21 = t20.ptr;
      t21 = (uint8_t *)(((uintptr_t)t21) + (t16 * sizeof(uint8_t)));
      t16 = t10 - t16;
      t20.ptr = t21;
      t20.len = t16;
      t21 = t20.ptr;
      t22 = (struct arr_4_u8 *)t21;
      memcpy(t19->array, t22->array, 4 * sizeof(uint8_t));
      t16 = t17;
      t12 = (uint32_t)t16;
      t11 = (uint32_t)a2;
      t5 = t12 == t11;
      if (t5) {
        t16 = t18;
        t11 = (uint32_t)t16;
        t12 = (uint32_t)a3;
        t5 = t11 == t12;
        t23 = t5;
        goto b6;
      }
      t23 = 0;
      goto b6;
    b6:;
      if (t23) {
        t6 = (*t8);
        t14 = (struct slice_u8 *)&t6->data;
        t20 = (*t14);
        t24 = &t20.ptr[t13];
        t13 = (uintptr_t)t24;
        t13 = t13 | 1;
        t25.is_null = 0;
        t25.payload = t13;
        return t25;
      }
      t16 = t9;
      t16 = t16 + 1;
      t9 = t16;
      goto loop;
    }
    goto b1;
  b1:;
  }
  return (struct opt_usize){.payload = 0xaaaaaaaa, .is_null = 1};
}

static uint32_t slots[3 * 4] __attribute__((aligned(4)));

/* process_data_relocations' shape: one find_thunk per relocation. */
__attribute__((noinline)) static int relocate(struct unique *u, uintptr_t count, uintptr_t got,
                                              const uintptr_t *from, uintptr_t *to, int n)
{
  int hits = 0;
  for (int i = 0; i < n; i++)
  {
    struct opt_usize r = find_thunk(u, count, got, from[i]);
    if (!r.is_null)
    {
      to[i] = r.payload;
      hits++;
      continue;
    }
    to[i] = from[i];
  }
  return hits;
}

int main(void)
{
  struct holder h = {{(uint8_t *)slots, sizeof slots}, 1};
  struct unique u = {&h};
  for (int i = 0; i < 3; i++)
  {
    slots[i * 4 + 2] = 0x20001000u;      /* r9 */
    slots[i * 4 + 3] = 0x10000101u + i * 0x40; /* fn */
  }
  uintptr_t from[4] = {0x10000141u, 0x10000999u, 0x10000101u, 0x10000181u};
  uintptr_t to[4];
  int hits = relocate(&u, 3, 0x20001000u, from, to, 4);
  printf("hits=%d\n", hits);
  for (int i = 0; i < 4; i++)
    printf("to%d=%s\n", i, to[i] == from[i] ? "raw" : (to[i] == ((uintptr_t)&slots[0] + (to[i] - (uintptr_t)&slots[0])) ? "thunk" : "?"));
  printf("slot=%d\n", (int)((to[0] & ~1u) - (uintptr_t)slots) / 16);
  return 0;
}
