/* Guard for pool-constant hoisting (const classes in ssa:global_addr_hoist /
 * ssa:loop_addr_hoist / ssa:local_addr_cse): constants with no MOV/MVN/MOVW
 * encoding are parked in a register across calls and loops; the parked value
 * must survive call clobbers, loop backedges and register pressure. */
#include <stdio.h>

volatile unsigned seed = 0x12345678u;
volatile unsigned side;

static void tick(void) { side++; }

static unsigned hash_calls(unsigned h)
{
  h = h * 0x9e3779b1u;
  tick();
  h = (h ^ 0x9e3779b1u) + 1u;
  tick();
  h += h * 0x9e3779b1u;
  tick();
  h ^= 0x9e3779b1u;
  tick();
  h = h * 0x9e3779b1u;
  tick();
  h = h - (h >> 3) * 0x9e3779b1u;
  return h;
}

static unsigned hash_loop(const unsigned *p, int n)
{
  unsigned h = 0u;
  for (int i = 0; i < n; i++)
    h = (h ^ p[i]) * 0x9e3779b1u;
  return h;
}

static unsigned hash_straight(unsigned a, unsigned b)
{
  unsigned x = a * 0x9e3779b1u;
  unsigned y = (b ^ 0x9e3779b1u) * 3u;
  unsigned z = (a + 0x9e3779b1u) ^ (b - 0x9e3779b1u);
  return x + y + z;
}

int main(void)
{
  unsigned data[8], cs;
  int i;
  for (i = 0; i < 8; i++)
    data[i] = (unsigned)(i * 0x01234567) ^ 0x89abcdefu;
  cs = hash_calls(seed);
  cs = (cs << 1) ^ hash_loop(data, 8);
  cs = (cs << 1) ^ hash_straight(cs, 0xdeadbeefu);
  printf("cs=%08x side=%u\n", cs, side);
  return 0;
}
