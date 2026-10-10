/* A bool-returning helper called from an inlined body must inline too: the
 * nested-inline gate's return-type whitelist once stopped at VT_LLONG and
 * left VT_BOOL (zig.h's zig_addo_u32 & co.) as a real call in every caller. */
typedef unsigned int u32;
typedef _Bool bool;
struct res { u32 payload; unsigned short error; };

static inline bool add_ovf(u32 *r, u32 a, u32 b)
{
  *r = a + b;
  return *r < a;
}

static struct res math_add(u32 a, u32 b)
{
  struct res t;
  u32 s;
  if (add_ovf(&s, a, b)) { t.payload = 0; t.error = 7; return t; }
  t.payload = s; t.error = 0; return t;
}

u32 sum(const u32 *v, u32 n)
{
  u32 acc = 0;
  for (u32 i = 0; i < n; i++) {
    struct res r = math_add(acc, v[i]);
    if (r.error)
      return 0;
    acc = r.payload;
  }
  return acc;
}
