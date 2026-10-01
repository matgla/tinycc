/* A compare against an end of its operand's own range is constant
 * (test_cmp_type_extreme_folds): VRP folds it written directly and after
 * inlining turns a parameter into the extreme. */
#include <stdint.h>

int u_above_max(uint32_t x) { return x > 0xFFFFFFFFu; }
int s_below_min(int32_t x) { return x < INT32_MIN; }

static inline int above(uint32_t x, uint32_t m) { return x > m; }
static inline int below(int32_t x, int32_t m) { return x < m; }
extern int use(int);
int inlined(uint32_t a, int32_t b) { return use(above(a, 0xFFFFFFFFu)) + use(below(b, INT32_MIN)); }
