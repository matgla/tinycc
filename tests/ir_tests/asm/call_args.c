/* Phase 4: AAPCS parameter marshalling and return values.
 * Arguments are loaded from extern globals so the optimizer cannot constant-fold
 * the call, forcing real parameter marshalling to be emitted. */

extern int g_a, g_b, g_c, g_d, g_e;
extern long long g_x, g_y;

__attribute__((noinline)) int callee_int(int a, int b, int c, int d) { return a + b + c + d; }
__attribute__((noinline)) long long callee_long(long long a, long long b) { return a + b; }
__attribute__((noinline)) int callee_stack(int a, int b, int c, int d, int e) { return a + b + c + d + e; }

int caller_int(void) { return callee_int(g_a, g_b, g_c, g_d); }
long long caller_long(void) { return callee_long(g_x, g_y); }
int caller_stack(void) { return callee_stack(g_a, g_b, g_c, g_d, g_e); }
