/* Force references to compiler runtime bit-operation helpers.
 *
 * The compiler lowers bswap, ctz and popcount builtins to libgcc-style
 * symbols (__bswapsi2, __ctzsi2, __popcountsi2, etc.) that are resolved by
 * the armv8m-libtcc1.a runtime library.
 */
volatile unsigned x;
volatile unsigned long xl;
volatile unsigned long long xll;

int force_bitops(void) {
    return __builtin_bswap16(x)
         + __builtin_bswap32(x)
         + (int)__builtin_bswap64((unsigned long long)x)
         + __builtin_ctz(x)
         + __builtin_ctzl(xl)
         + __builtin_ctzll(xll)
         + __builtin_popcount(x)
         + __builtin_popcountl(xl)
         + __builtin_popcountll(xll);
}
