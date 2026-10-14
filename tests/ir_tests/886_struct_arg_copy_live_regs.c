/* A struct passed by value on the stack (8+ words) is copied into the outgoing
 * argument area with r0-r3/ip/lr as scratch.  Only the registers that still
 * hold a value the call setup reads -- identity register arguments, the
 * source pointer, an indirect call target, a pending move source, a later
 * stack argument -- may be saved around the copy; the rest are free.  Every
 * call below keeps such a value in one of those registers, at several struct
 * sizes, and the callee checks every word, so a clobbered register or a
 * mis-biased frame offset shows up as a wrong sum. */

#define DEF(N)                                                                                                         \
  struct S##N { unsigned v[N]; };                                                                                      \
  static unsigned sum##N(const struct S##N *s)                                                                         \
  {                                                                                                                    \
    unsigned h = 0;                                                                                                    \
    for (int i = 0; i < N; i++)                                                                                        \
      h = h * 31u + s->v[i];                                                                                           \
    return h;                                                                                                          \
  }                                                                                                                    \
  /* four register ints, the struct on the stack, a stack int after it */                                              \
  static unsigned take4_##N(unsigned a, unsigned b, unsigned c, unsigned d, struct S##N s, unsigned e)                 \
  {                                                                                                                    \
    return (((a * 7u + b) * 7u + c) * 7u + d) * 7u + e + sum##N(&s) * 13u;                                              \
  }                                                                                                                    \
  /* the struct starts in r1 and spills to the stack */                                                                \
  static unsigned takesplit_##N(unsigned a, struct S##N s, unsigned e)                                                 \
  {                                                                                                                    \
    return a * 5u + e + sum##N(&s) * 3u;                                                                               \
  }                                                                                                                    \
  /* two big structs, then a 64-bit value on the stack */                                                              \
  static unsigned take2_##N(unsigned a, struct S##N s, struct S##N t, unsigned long long q)                            \
  {                                                                                                                    \
    return a + sum##N(&s) * 3u + sum##N(&t) * 5u + (unsigned)q + (unsigned)(q >> 32) * 11u;                            \
  }                                                                                                                    \
  static struct S##N make##N(unsigned seed)                                                                            \
  {                                                                                                                    \
    struct S##N r;                                                                                                     \
    for (int i = 0; i < N; i++)                                                                                        \
      r.v[i] = seed * 2654435761u + (unsigned)i * 40503u;                                                              \
    return r;                                                                                                          \
  }                                                                                                                    \
  static unsigned (*volatile p_take4_##N)(unsigned, unsigned, unsigned, unsigned, struct S##N, unsigned) = take4_##N;  \
  static unsigned (*volatile p_split_##N)(unsigned, struct S##N, unsigned) = takesplit_##N;                            \
  static unsigned (*volatile p_take2_##N)(unsigned, struct S##N, struct S##N, unsigned long long) = take2_##N;         \
  static struct S##N (*volatile p_make##N)(unsigned) = make##N;                                                        \
  /* identity arguments: r0-r3 all stay live across the copy */                                                       \
  __attribute__((noinline)) static unsigned pass_through_##N(unsigned a, unsigned b, unsigned c, unsigned d,           \
                                                             const struct S##N *p, unsigned e)                         \
  {                                                                                                                    \
    return p_take4_##N(a, b, c, d, *p, e);                                                                             \
  }                                                                                                                    \
  /* the source pointer and a stack scalar arrive in r0-r3 */                                                          \
  __attribute__((noinline)) static unsigned src_in_arg_reg_##N(const struct S##N *p, unsigned e, unsigned f)           \
  {                                                                                                                    \
    return p_take4_##N(e, f, e + f, e ^ f, *p, e - f);                                                                 \
  }                                                                                                                    \
  /* nothing live in r0-r3: the struct is a fresh local */                                                             \
  __attribute__((noinline)) static unsigned fresh_local_##N(unsigned x)                                                \
  {                                                                                                                    \
    struct S##N t = p_make##N(x);                                                                                      \
    return p_take4_##N(x, 1u, 2u, 3u, t, x + 1u);                                                                      \
  }                                                                                                                    \
  /* a register-split struct whose source pointer lives in r0 */                                                       \
  __attribute__((noinline)) static unsigned split_src_r0_##N(const struct S##N *p, unsigned a, unsigned e)             \
  {                                                                                                                    \
    return p_split_##N(a, *p, e);                                                                                      \
  }                                                                                                                    \
  /* an indirect target that is also an argument */                                                                    \
  __attribute__((noinline)) static unsigned target_is_arg_##N(unsigned (*f)(unsigned, struct S##N, unsigned),          \
                                                              const struct S##N *p, unsigned e)                        \
  {                                                                                                                    \
    return f(e, *p, e + 9u);                                                                                           \
  }                                                                                                                    \
  __attribute__((noinline)) static unsigned two_structs_##N(const struct S##N *p, const struct S##N *q, unsigned a)    \
  {                                                                                                                    \
    return p_take2_##N(a, *p, *q, 0x123456789abcdefull + a);                                                           \
  }                                                                                                                    \
  /* call result chained into the next call's struct argument */                                                       \
  __attribute__((noinline)) static unsigned chained_##N(unsigned x)                                                    \
  {                                                                                                                    \
    return p_take4_##N(x, x + 1u, x + 2u, x + 3u, p_make##N(x * 3u), x + 4u);                                          \
  }                                                                                                                    \
  static int check##N(void)                                                                                            \
  {                                                                                                                    \
    struct S##N a = make##N(1u), b = make##N(2u);                                                                      \
    if (pass_through_##N(11u, 22u, 33u, 44u, &a, 55u) != take4_##N(11u, 22u, 33u, 44u, a, 55u))                         \
      return 1;                                                                                                        \
    if (src_in_arg_reg_##N(&b, 100u, 7u) != take4_##N(100u, 7u, 107u, 100u ^ 7u, b, 93u))                               \
      return 2;                                                                                                        \
    if (fresh_local_##N(5u) != take4_##N(5u, 1u, 2u, 3u, make##N(5u), 6u))                                              \
      return 3;                                                                                                        \
    if (split_src_r0_##N(&a, 9u, 77u) != takesplit_##N(9u, a, 77u))                                                    \
      return 4;                                                                                                        \
    if (target_is_arg_##N(p_split_##N, &b, 6u) != takesplit_##N(6u, b, 15u))                                           \
      return 5;                                                                                                        \
    if (two_structs_##N(&a, &b, 3u) != take2_##N(3u, a, b, 0x123456789abcdefull + 3u))                                 \
      return 6;                                                                                                        \
    if (chained_##N(8u) != take4_##N(8u, 9u, 10u, 11u, make##N(24u), 12u))                                              \
      return 7;                                                                                                        \
    return 0;                                                                                                          \
  }

DEF(8)
DEF(9)
DEF(13)
DEF(21)
DEF(33)
DEF(64)

int main(void)
{
  int r;
  if ((r = check8()))
    return 10 + r;
  if ((r = check9()))
    return 20 + r;
  if ((r = check13()))
    return 30 + r;
  if ((r = check21()))
    return 40 + r;
  if ((r = check33()))
    return 50 + r;
  if ((r = check64()))
    return 60 + r;
  return 0;
}
