/* gens rotate fusion on a VAR's own slot.  mix_loop's `(value << 3) |
 * (value >> 29)` reads the loop-carried local in place and must become one
 * ror; redef writes v between the shifts and the OR and must not. */
unsigned mix_loop(int n)
{
    unsigned value = 0x13579BDFu;
    for (int k = 0; k < n; k++) {
        value ^= value << 7;
        value ^= value >> 9;
        value += 0x9E3779B9u;
        value = (value << 3) | (value >> 29);
    }
    return value;
}

unsigned redef(unsigned v, unsigned w)
{
    unsigned a = v << 3, b = v >> 29;
    v = w;
    return (a | b) ^ v;
}
