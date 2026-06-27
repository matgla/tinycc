/* Exercise ssa:strength on unsigned modulus by a power of two.
 * Earlier pipeline passes leave UMOD #8 untouched, so the SSA strength pass
 * has a visible effect: UMOD x, 8  ->  AND x, 7.
 */
unsigned int test(unsigned int x) { return x % 8; }
