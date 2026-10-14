/* ssa:pure_call_cse: the Zig symbol-table walk computes `size(p)` and then
 * `next(p) = p + size(p)` -- one call per record, not two.  size() is PURE
 * only through find_sentinel's cached verdict (purity:transitive). */
typedef unsigned int usize;

__attribute__((noinline)) usize find_sentinel(const unsigned char *p)
{
  usize i = 0;
  while (p[i] != 0)
    i++;
  return i;
}

__attribute__((noinline)) usize sym_size(const unsigned char *s, unsigned char align)
{
  usize n = find_sentinel(s + 4) + 5;
  usize a = align ? align : 1;
  return (n + a - 1) & ~(a - 1);
}

static const unsigned char *sym_next(const unsigned char *s, unsigned char align)
{
  return s + sym_size(s, align);
}

usize walk(const unsigned char *p, usize n, const unsigned char *alignp)
{
  usize total = 0;
  for (usize i = 0; i < n; i++)
  {
    total += sym_size(p, *alignp);
    p = sym_next(p, *alignp);
  }
  return total;
}

/* A store between the two scans keeps both. */
usize store_between(const unsigned char *s, unsigned char *alias)
{
  usize a = find_sentinel(s);
  alias[a] = 'y';
  return find_sentinel(s) * 100 + a;
}
