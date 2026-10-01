/* Frame-word pairing and same-slot copies (arm-thumb-gen.c).

   frame_word_pair_rewind merges `ldr rA,[sp,#k]; ldr rB,[sp,#k+4]` (and the
   STR pair) into one LDRD/STRD after the fact, by rewinding over the first
   access.  Values kept alive across calls in more registers than the
   callee-saved set spill and reload in neighbouring words; the reloads must
   still deliver each value to its own register, in either order, and a pair
   must never swallow a load into the same register.

   tcc_gen_machine_assign_mop_ex skips a copy between two values spilled to
   the same slot: a phi copy of a rotating set of spilled values. */
#include <stdio.h>

__attribute__((noinline)) unsigned id(unsigned x) { return x; }

__attribute__((noinline)) unsigned pressure(unsigned a)
{
  unsigned v0 = id(a), v1 = id(a + 1), v2 = id(a + 2), v3 = id(a + 3), v4 = id(a + 4), v5 = id(a + 5);
  unsigned v6 = id(a + 6), v7 = id(a + 7), v8 = id(a + 8), v9 = id(a + 9), v10 = id(a + 10), v11 = id(a + 11);
  unsigned s = 0;
  for (int k = 0; k < 5; k++)
  {
    unsigned t = v0;
    v0 = v1; v1 = v2; v2 = v3; v3 = v4; v4 = v5; v5 = v6;
    v6 = v7; v7 = v8; v8 = v9; v9 = v10; v10 = v11; v11 = t;
    s += id(v0) ^ (v5 * 3u) ^ (v11 << 2);
  }
  return s + v0 * 1u + v1 * 2u + v2 * 3u + v3 * 5u + v4 * 7u + v5 * 11u + v6 * 13u + v7 * 17u + v8 * 19u +
         v9 * 23u + v10 * 29u + v11 * 31u;
}

struct quad
{
  unsigned a, b, c, d;
};

__attribute__((noinline)) struct quad mix(struct quad q, unsigned k)
{
  struct quad r;
  r.a = id(q.b + k);
  r.b = id(q.a ^ k);
  r.c = id(q.d - k);
  r.d = id(q.c * k);
  return r;
}

__attribute__((noinline)) unsigned swapper(unsigned x, unsigned y, int n)
{
  unsigned p = id(x), q = id(y), r = id(x ^ y), s = id(x + y);
  for (int i = 0; i < n; i++)
  {
    unsigned t = p;
    p = q;
    q = id(t + r);
    r = s;
    s = id(r ^ t);
  }
  return p * 3u + q * 5u + r * 7u + s * 11u;
}

int main(void)
{
  printf("pressure %u %u\n", pressure(1), pressure(1000));
  struct quad q = {1, 2, 3, 4};
  for (unsigned k = 1; k < 4; k++)
    q = mix(q, k);
  printf("mix %u %u %u %u\n", q.a, q.b, q.c, q.d);
  printf("swapper %u %u\n", swapper(3, 5, 7), swapper(100, 7, 12));
  return 0;
}
