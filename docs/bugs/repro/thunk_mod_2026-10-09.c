#include <stdint.h>
#include <string.h>

struct Entry { uint32_t at; uint32_t value; };
struct Slice { const struct Entry *ptr; uintptr_t len; };
struct Code { const uint8_t *ptr; uintptr_t len; };

/* Shape of the thunk replay loop (kernel.c 65728+): per entry, re-read a
 * by-value struct's field and take a runtime remainder by it. */
__attribute__((noinline))
void thunk_replay(struct Slice list, struct Code code, uint8_t *dest,
                  uintptr_t base) {
  for (uintptr_t i = 0; i < list.len; i++) {
    struct Entry e = list.ptr[i];
    uintptr_t off = base + (e.at >> 8) * 4;
    if (off % code.len == 8)
      memcpy(dest + i * 8, &e.value, 8);
  }
}
