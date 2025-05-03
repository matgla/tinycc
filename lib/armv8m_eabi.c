#include <stdint.h>
#include <stdlib.h>

void __aeabi_uldivmod(uint64_t num, uint64_t den) {}

void __aeabi_ldivmod(uint64_t num, uint64_t den) {}
void __aeabi_memset(void *s, size_t n, int c) {
  char *p = s;
  while (n--) {
    *p++ = c;
  }
  return s;
}
