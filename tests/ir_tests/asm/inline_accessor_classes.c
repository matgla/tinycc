/* Inliner classes behind the kernel loader's accessor chains.  The helpers are
 * written the way Zig's C backend writes them -- temporaries, casts, dead
 * debug values, more than the 100 tokens -O2 registration inlines on length
 * alone -- so each case is decided by the post-optimization classes in
 * gen_function. */
typedef unsigned u32;
typedef unsigned char u8;

/* Out of line: it loops. */
__attribute__((noinline)) u32 count_to_zero(const u8 *s)
{
  u32 n = 0;
  while (s[n])
    n++;
  return n;
}

struct slice { const u8 *ptr; u32 len; };

/* inline:wrappers -- acyclic, one call (Zig's mem.span). */
static struct slice span(const u8 *const a0)
{
  const u8 *t0;
  const u8 *const *t1;
  u32 t2;
  const u8 *t3;
  struct slice t4;
  t0 = a0;
  t1 = (const u8 *const *)&t0;
  /* dbg_var_val padding, as Zig's C backend leaves it */
  {
    u32 d0 = (u32)0u, d1 = (u32)1u, d2 = (u32)2u, d3 = (u32)3u, d4 = (u32)4u;
    d0 = d0 + d1; d1 = d1 + d2; d2 = d2 + d3; d3 = d3 + d4; d4 = d4 + d0;
    d0 = d0 ^ d2; d1 = d1 ^ d3; d2 = d2 ^ d4; d3 = d3 ^ d0; d4 = d4 ^ d1;
    d0 = d0 | d3; d1 = d1 | d4; d2 = d2 | d0; d3 = d3 | d1; d4 = d4 | d2;
    (void)d0; (void)d1; (void)d2; (void)d3; (void)d4;
  }
  t2 = count_to_zero(a0);
  t3 = (*t1);
  t3 = (const u8 *)(((unsigned long)t3) + ((unsigned long)0ul * sizeof(u8)));
  t2 = t2 - (u32)0ul;
  t4.ptr = t3;
  t4.len = t2;
  return t4;
}

/* A wrapper over a wrapper (Zig's Symbol.name). */
static struct slice name_of(const u8 *const a0)
{
  unsigned long t0;
  const u8 *t1;
  struct slice t2;
  struct slice t3;
  t0 = (unsigned long)a0;
  t0 = t0 + (unsigned long)4ul;
  t1 = (const u8 *)t0;
  /* dbg_var_val padding, as Zig's C backend leaves it */
  {
    u32 d0 = (u32)0u, d1 = (u32)1u, d2 = (u32)2u, d3 = (u32)3u, d4 = (u32)4u;
    d0 = d0 + d1; d1 = d1 + d2; d2 = d2 + d3; d3 = d3 + d4; d4 = d4 + d0;
    d0 = d0 ^ d2; d1 = d1 ^ d3; d2 = d2 ^ d4; d3 = d3 ^ d0; d4 = d4 ^ d1;
    d0 = d0 | d3; d1 = d1 | d4; d2 = d2 | d0; d3 = d3 | d1; d4 = d4 | d2;
    (void)d0; (void)d1; (void)d2; (void)d3; (void)d4;
  }
  t2 = span(t1);
  t3.ptr = (const u8 *)t2.ptr;
  t3.len = t2.len;
  return t3;
}

u32 one_name(const u8 *rec)
{
  return name_of(rec).len;
}

/* inline:barrier_leaf -- an acquire load: the barrier is one DMB, not a call. */
static u32 load_acquire(const u32 *const a0)
{
  const u32 *t0;
  const u32 *const *t1;
  u32 t2;
  t0 = a0;
  t1 = (const u32 *const *)&t0;
  /* dbg_var_val padding, as Zig's C backend leaves it */
  {
    u32 d0 = (u32)0u, d1 = (u32)1u, d2 = (u32)2u, d3 = (u32)3u, d4 = (u32)4u;
    d0 = d0 + d1; d1 = d1 + d2; d2 = d2 + d3; d3 = d3 + d4; d4 = d4 + d0;
    d0 = d0 ^ d2; d1 = d1 ^ d3; d2 = d2 ^ d4; d3 = d3 ^ d0; d4 = d4 ^ d1;
    d0 = d0 | d3; d1 = d1 | d4; d2 = d2 | d0; d3 = d3 | d1; d4 = d4 | d2;
    (void)d0; (void)d1; (void)d2; (void)d3; (void)d4;
  }
  t2 = __atomic_load_n((*t1), 2 /* acquire */);
  t2 = t2 + (u32)0ul;
  t2 = t2 | (u32)0ul;
  return t2;
}

u32 use_acquire(const u32 *p)
{
  return load_acquire(p) + load_acquire(p + 1);
}

/* inline:trivial_leaf -- a trivial body is inlined at a non-constant site. */
static u32 low_bits(u32 const a0, u32 const a1)
{
  u32 t0;
  u32 t1;
  u32 t2;
  t0 = a0;
  t1 = a1;
  /* dbg_var_val padding, as Zig's C backend leaves it */
  {
    u32 d0 = (u32)0u, d1 = (u32)1u, d2 = (u32)2u, d3 = (u32)3u, d4 = (u32)4u;
    d0 = d0 + d1; d1 = d1 + d2; d2 = d2 + d3; d3 = d3 + d4; d4 = d4 + d0;
    d0 = d0 ^ d2; d1 = d1 ^ d3; d2 = d2 ^ d4; d3 = d3 ^ d0; d4 = d4 ^ d1;
    d0 = d0 | d3; d1 = d1 | d4; d2 = d2 | d0; d3 = d3 | d1; d4 = d4 | d2;
    (void)d0; (void)d1; (void)d2; (void)d3; (void)d4;
  }
  t2 = (t0 & (u32)0xffu) + (t1 & (u32)0u);
  t2 = t2 | ((u32)0u << (u32)0u);
  t2 = t2 + (u32)0ul - (u32)0ul;
  return t2;
}

u32 use_low_bits(u32 x, u32 y)
{
  return low_bits(x, y) * 3u;
}

/* Second call sites: a helper called once is inlined whatever its size
 * (called-once), which would hide the classes above. */
u32 two_names(const u8 *a, const u8 *b)
{
  return name_of(a).len + span(b).len * 3u;
}

u32 use_low_bits2(u32 x)
{
  return low_bits(x, x + 1u) ^ 5u;
}

/* inline:barrier_leaf one level up (Zig's itemPtr over Entry.acquire): with
 * the acquire inlined its body holds a DMB, still a leaf. */
static u32 item_at(const u32 *const a0, u32 const a1)
{
  const u32 *t0;
  u32 t1;
  u32 t2;
  /* dbg_var_val padding, as Zig's C backend leaves it */
  {
    u32 d0 = (u32)0u, d1 = (u32)1u, d2 = (u32)2u, d3 = (u32)3u, d4 = (u32)4u;
    d0 = d0 + d1; d1 = d1 + d2; d2 = d2 + d3; d3 = d3 + d4; d4 = d4 + d0;
    d0 = d0 ^ d2; d1 = d1 ^ d3; d2 = d2 ^ d4; d3 = d3 ^ d0; d4 = d4 ^ d1;
    d0 = d0 | d3; d1 = d1 | d4; d2 = d2 | d0; d3 = d3 | d1; d4 = d4 | d2;
    (void)d0; (void)d1; (void)d2; (void)d3; (void)d4;
  }
  t0 = a0 + (unsigned long)(a1 & 7u);
  t1 = load_acquire(t0);
  t2 = t1 * (u32)3u + a1;
  return t2;
}

u32 use_item(const u32 *p, u32 i)
{
  return item_at(p, i) + item_at(p, i + 1u);
}

u32 use_item2(const u32 *p)
{
  return item_at(p, 2u) ^ 9u;
}

/* inline:leaf_rescue: a straight leaf short enough to be registered at
 * -finline-limit=200 (and over the 60-token "naturally small" bound) stays
 * inlined over the IR-size revoke (raising the limit used to UN-inline it).
 * The body is sized for the post-opt revoke's economics, which count IR
 * slots after the late barrel-shift fusion has folded single-use shifts
 * into their consumers: the revoke demotes above 24 slots, while
 * leaf_rescue still classifies the body at <= 24 real ops.  Deliberately
 * shift-free (non-pow2 multiplies instead) so that fusion cannot move the
 * slot count between compiler builds; the multi-use w-tail pins the body
 * in the window (23 real ops, 3 join NOPs => 26 slots: above the revoke's
 * gate, one op under the small-leaf budget). */
static u32 mix(u32 a, u32 b, u32 c)
{
  u32 x = a ^ (b * 2654435761u);
  u32 y = b + c;
  u32 z = (x & 0xff00ff00u) | (y & 0x00ff00ffu);
  x = x * 2246822519u + (z ^ c);
  y = (y ^ x) - z;
  z = z ^ x ^ (y + 61u);
  x = x + (z * 7u);
  u32 w = (x ^ z) + y;
  w = (w ^ 0x5bd1e995u) + c;
  return x + y + (z * 3u) + (w & c);
}

u32 use_mix(const u32 *v, u32 n)
{
  u32 acc = 0;
  for (u32 i = 0; i + 2 < n; i++)
    acc += mix(v[i], v[i + 1], v[i + 2]);
  return acc;
}

u32 use_mix2(u32 a)
{
  return mix(a, a + 1u, a + 2u);
}
