/* A 64-bit shift by a variable count: inline at -O2, the __aeabi_* helper
 * call at -Os (test_shift64_var_os_calls_helper). */
unsigned long long shl(unsigned long long x, int n) { return x << n; }
unsigned long long shr(unsigned long long x, int n) { return x >> n; }
long long sar(long long x, int n) { return x >> n; }
unsigned long long shl_const(unsigned long long x) { return x << 5; }
