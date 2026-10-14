/* Inlining a callee whose own parameters and locals (also a
 * typedef-declared one, a for-loop one) share names with
 * the enclosing inlined body's locals, and a nested
 * function inlined where it is defined: none stays a call.
 */
typedef int T;
struct S {
  int a, b;
};
static int h3(int x) {
  int y = x + 1;
  return y * 2;
}
static int h2(int x) {
  int y = h3(x) + 3;
  return y;
}
int h1(int x) {
  int y = h2(x) * 5;
  return y + x;
}
static int sq(int t) {
  T s = t * t, u = s + 1;
  return s + u;
}
static int w(int x) {
  int s = x + 1, t = 2, u = 3;
  return sq(s) + t + u;
}
int use_w(int x) { return w(x) + 1; }
static int fm(struct S *p) {
  int a = p->a;
  {
    int b = p->b;
    a += b;
  }
  return a;
}
static int om(int x) {
  int a = x, b = 2;
  struct S v = {a, b};
  return fm(&v) + a + b;
}
int use_om(int x) { return om(x) * 3; }
static int fl(int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += i;
  return s;
}
static int ol(int n) {
  int s = n, i = 1;
  return fl(n) + s + i;
}
int use_ol(int n) { return ol(n); }
int par(int a) {
  int k = a * 3;
  int add(int x) {
    int y = x + k;
    return y;
  }
  return add(1) + add(a);
}
