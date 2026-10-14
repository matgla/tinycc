typedef struct { int a, b; } P;
P f(int x);
int other(int x) { P r = f(x); return r.a * 100 + r.b; }
