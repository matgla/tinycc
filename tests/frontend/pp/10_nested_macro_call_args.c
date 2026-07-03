#define ADD(a, b) ((a) + (b))
#define CALL_ADD(x, y) ADD(x, y)
#define TWICE(f, x) f(x, x)
int p = CALL_ADD(2, 3);
int q = TWICE(ADD, 5);
#define APPLY(f, ...) f(__VA_ARGS__)
int r = APPLY(ADD, 4, 6);
