/* Symbol-address scratch placement (get_scratch_reg_for_sym_addr in
 * arm-thumb-gen.c): repeated materializations of the same global's address
 * must reuse the register still holding it (via imm_cache) instead of
 * reloading the literal-pool word each time.  Two rules under test:
 *
 *  1. a free register already holding &sym reused directly (the load
 *     elides to zero instructions);
 *  2. on a miss the address parks in the HIGHEST free low register (or
 *     IP via the fallback), away from the lowest-first scratch churn of
 *     spill reloads, so rule 1 can fire on the next access.
 *
 * Without this, every access re-materialized the base into r0 — an
 * unrolled table-indexed kernel (mibench_rijndael encrypt) reloaded the
 * same table base 14 times. */

unsigned tab[256];

/* Six reads through the same global table base: exactly ONE literal-pool
 * load; the other five accesses reuse the parked base register. */
unsigned lookup6(unsigned a, unsigned b, unsigned c, unsigned d)
{
  unsigned x = 0;
  x ^= tab[a & 0xff];
  x ^= tab[(b >> 8) & 0xff];
  x ^= tab[(c >> 16) & 0xff];
  x ^= tab[d >> 24];
  x ^= tab[b & 0xff];
  x ^= tab[(c >> 8) & 0xff];
  return x;
}

int acc[4];

/* Four stores through the same global base: one literal-pool load. */
void scatter(int v)
{
  acc[0] = v;
  acc[1] = v + 1;
  acc[2] = v + 2;
  acc[3] = v + 3;
}
