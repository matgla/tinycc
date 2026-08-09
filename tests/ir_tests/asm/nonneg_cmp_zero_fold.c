/* ssa:branch folds a compare of a provably non-negative value against 0.0.
 * fabs() can still hand back a NaN, so only the half of the directions whose
 * answer is the same ordered and unordered may fold:
 *
 *   fabs(x) <  0.0   false for every x, NaN included   -- folds to 0
 *   0.0 >  fabs(x)   likewise                          -- folds to 0
 *   fabs(x) >= 0.0   false for a NaN                   -- must NOT fold
 *   0.0 <= fabs(x)   likewise                          -- must NOT fold
 *
 * Under -mfpu=rp2350 a double compare is an inline DCP RCMP whose result is
 * read with *unsigned* conditions, not an __aeabi_cdcmple call, so the fold
 * decides on a different token spelling than the soft-float one.  Only the
 * runtime answer distinguishes the two halves, and no host or QEMU target has
 * a DCP to produce it -- hence this assertion on the emitted code.
 */
extern double fabs(double);

int lt_folds(double x) { return fabs(x) < 0.0; }
int gt0_folds(double x) { return 0.0 > fabs(x); }
int ge_must_not_fold(double x) { return fabs(x) >= 0.0; }
int le0_must_not_fold(double x) { return 0.0 <= fabs(x); }
