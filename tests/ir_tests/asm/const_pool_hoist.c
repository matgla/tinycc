/* Pool-constant hoisting (const classes in ssa:global_addr_hoist /
 * ssa:loop_addr_hoist / ssa:local_addr_cse): a 32-bit constant with no
 * MOV/MVN/MOVW encoding must be loaded from the literal pool once per
 * function and stay register-resident, instead of reloading at every use. */

void tick(void);

/* Entry hoist: call-separated uses force the constant into a callee-saved
 * register (the machine-level imm_cache is fully reset at each call). */
unsigned hash_calls(unsigned h)
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

/* Loop preheader hoist: a call-free loop reloads the multiplier every
 * iteration without the hoist (imm_cache dies at the backedge target). */
unsigned hash_loop(const unsigned *p, int n)
{
  unsigned h = 0u;
  for (int i = 0; i < n; i++)
    h = (h ^ p[i]) * 0x9e3779b1u;
  return h;
}

/* Straight-line CSE: repeated uses inside one region share one temp. */
unsigned hash_straight(unsigned a, unsigned b)
{
  unsigned x = a * 0x9e3779b1u;
  unsigned y = (b ^ 0x9e3779b1u) * 3u;
  unsigned z = (a + 0x9e3779b1u) ^ (b - 0x9e3779b1u);
  return x + y + z;
}
