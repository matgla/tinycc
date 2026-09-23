/* A frame address parked in a register must not be reused AHEAD of where it
 * was parked.
 *
 * stackoff_indexed_base_cse rewrites an indexed access's STACKOFF base to a
 * TEMP the prologue already loaded with that address, on the grounds that the
 * prologue dominates the whole function.  It does -- but only from the ASSIGN
 * onwards.  An access EARLIER in the same straight-line prologue read the temp
 * before anything had written it, so at -O2 `arr9[i]` loaded through whatever
 * the caller left in the register (with a poisoned one it faulted).
 *
 * The shape is what the two reads of `m212` build: the second reaches it as
 * `*(&m212[a][0] + b)`, which the frontend materialises as an address TEMP,
 * and the first is a plain indexed read of the same object earlier in the same
 * branch-free run.  Reduced from agg_deep fuzz seed 1637. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct N { unsigned a; unsigned b; };
struct N2 { struct N n; unsigned t; };
int main(void)
{
  unsigned cs = 0x12345678u;
  char s4 = (char)(863460459u & 0xff);
  unsigned u6 = 3478674980u;
  unsigned u7 = 2606935024u;
  unsigned arr8[8] = { 3266956212u, 2405417304u, 2824046361u, 1788383746u, 3555012821u, 714619954u, 639637460u, 2773089431u };
  unsigned arr9[8] = { 733865748u, 1153198824u, 3056370527u, 560033498u, 1853620413u, 121005395u, 1229005950u, 1711230968u };
  struct N2 n211 = { { 1457867030u, 3834342564u }, 44554686u };
  unsigned m212[4][4] = { { 1994247921u, 1515947663u, 704051171u, 1509225025u }, { 113208333u, 3075160707u, 3725319848u, 316329586u }, { 2695622949u, 1579111367u, 3875827252u, 2455408258u }, { 1504295279u, 697374028u, 3482915202u, 778016309u } };
  unsigned *pa213 = &u6;
  unsigned **ppa214 = &pa213;
  m212[((unsigned)(2820465294u) & 3u)][((unsigned)(u7) & 3u)] = (unsigned)(((unsigned)(arr9[((unsigned)(u6) & 7u)]) + (unsigned)((-((unsigned)(u7) | 0u)))));
  cs = csmix(cs, *(&m212[((unsigned)(2820465294u) & 3u)][0] + ((unsigned)(u7) & 3u)));
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((~((unsigned)(((unsigned)(n211.n.b) * (unsigned)(((unsigned)(n211.n.b) ^ cs)))) | 0u))) >> ((unsigned)((((unsigned)(((unsigned)(arr9[((unsigned)(u7) & 7u)]) + (unsigned)((unsigned)(s4)))) & 1u) ? (unsigned)(((unsigned)(u6) % ((unsigned)(arr8[((unsigned)(3839205814u) & 7u)]) | 1u))) : (unsigned)(((unsigned)(m212[((unsigned)(u6) & 3u)][((unsigned)(2229302978u) & 3u)]) & (unsigned)(m212[((unsigned)(u7) & 3u)][((unsigned)(u7) & 3u)]))))) & 31u))) % ((unsigned)(((unsigned)(2546737391u) + (unsigned)(1675086101u))) | 1u))));
  printf("checksum=%08x\n", cs);
}
