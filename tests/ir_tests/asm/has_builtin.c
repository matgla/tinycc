/* __has_builtin answers for what tcc implements, in #if and in running text
 * (test_has_builtin): zig.h picks REV16 for its byte swap, and pastes the
 * answer into a name to choose between a builtin and a libm import. */
#define zig_has_builtin(b) __has_builtin(__builtin_##b)
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define pick_0 0
#define pick_1 1

unsigned short swap16(unsigned short x)
{
#if zig_has_builtin(bswap16)
  return __builtin_bswap16(x);
#else
  return (unsigned short)((x << 8) | (x >> 8));
#endif
}

unsigned swap32(unsigned x)
{
#if zig_has_builtin(bswap32)
  return __builtin_bswap32(x);
#else
  return (x << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) | (x >> 24);
#endif
}

int answers(void)
{
  /* bswap64 yes, an unknown builtin no, both through a pasted name. */
  return CAT(pick_, zig_has_builtin(bswap64)) * 10 + CAT(pick_, zig_has_builtin(no_such_builtin));
}

#if !defined(__has_builtin)
#error "__has_builtin is not defined"
#endif
