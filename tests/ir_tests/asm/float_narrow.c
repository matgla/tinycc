/* Soft-FP double->float demotion fold: on soft-float targets `floor(f)` with a
 * float argument lowers to f2d(f) -> floor(double) [-> d2f], which must be
 * narrowed to the float variant floorf(f), killing the conversion calls.
 * Case 1 (result narrowed back to float) collapses to a tail call;
 * Case 2 (result stays double) swaps to floorf + f2d.  If the fold stops
 * firing, the f2d/d2f conversion calls and the double `floor` call reappear. */

double floor(double);
float floorf(float);

/* Case 1: f2d -> floor -> d2f  ==>  tail call to floorf. */
float q(float a) { return floor(a); }

/* Case 2: f2d -> floor, result stays double  ==>  floorf then f2d. */
double q1(float a) { return floor(a); }
