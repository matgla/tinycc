/* ipa:pure_via_sret's flag lives in SymAttr: a nested function named like a
 * flagged file-scope function shadows it. */
#define NI __attribute__((noinline))
struct P { int a, b, c; };
static NI struct P mk(int x) { struct P p = {x, x, x}; return p; }
NI int user(int y)
{
  struct P mk(int z) { struct P p = {z + 100, 0, 0}; return p; }
  return mk(y).a;
}
