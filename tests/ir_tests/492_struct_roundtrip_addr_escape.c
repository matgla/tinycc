/* struct_copy_roundtrip_elim removes `memmove(B, A); memmove(A, B)` -- A is
   unchanged -- when B is private to the pair.  Its privacy check waved
   through every address-of B and every call parameter touching B anywhere in
   the function as the pair's own plumbing, so `p = &t40.ok` read later, and
   `fill(&t40)` on another path, did not count: the copy into t40 went away
   and the switch read t40's slot unwritten.  This is the shape of Zig's
   Sema.coerceExtra as the Zig C backend emits it (`t43 = t41.payload;
   t40 = t43; t43 = t40;` then a switch through `&t40.payload.ok`), which
   crashed the tcc -O1 Zig compiler. */
#include <stdio.h>
#include <stdint.h>
struct Res { _Alignas(8) union { uint8_t ok; uint32_t w[16]; uint64_t d; } payload; uint8_t tag; };  /* 72 bytes */
struct ErrRes { struct Res payload; uint16_t error; };
__attribute__((noinline)) struct ErrRes check(int k) {
  struct ErrRes r = {0};
  for (int i = 0; i < 16; i++) r.payload.payload.w[i] = 0x01010101u * (unsigned)(k + i);
  r.payload.payload.ok = (uint8_t)(k % 5);
  r.payload.tag = (uint8_t)(k & 1);
  return r;
}
__attribute__((noinline)) int attrs(struct Res *r) { return r->payload.w[3] & 0xff; }
__attribute__((noinline)) int fill(struct Res *r, int k) { for (int i = 0; i < 16; i++) r->payload.w[i] = (uint32_t)(k * 3 + i); r->payload.ok = (uint8_t)k; r->tag = 1; return k; }
__attribute__((noinline)) int coerce(int k) {
  struct ErrRes t41;
  struct Res t43, t40;
  uint8_t const *t49;
  if (k > 100) {
    fill(&t40, k);
    t43 = t40;
    return t43.payload.ok + attrs(&t40);
  }
  t41 = check(k);
  if (t41.error) return -1;
  t43 = t41.payload;
  t40 = t43;
  t43 = t40;
  if (t43.tag == 0) return 100 + attrs(&t40);
  t49 = (uint8_t const *)&t40.payload.ok;
  switch (*t49) {
  case 0: return 10;
  case 1: return 11;
  case 2: return 12;
  case 3: return 13;
  case 4: return 14;
  default: return 99;
  }
}
int main(void) { printf("%d %d %d %d %d\n", coerce(1), coerce(2), coerce(3), coerce(8), coerce(103)); return 0; }
