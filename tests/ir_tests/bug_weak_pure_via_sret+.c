struct R { int a, b, c, d; };
extern int cnt;
struct R wk(int x)
{
  struct R r = {x, x, x, x};
  cnt++;
  return r;
}
