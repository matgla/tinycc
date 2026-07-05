#define TRIPLE(a, b, c) a ## b ## c
int xyz = TRIPLE(x, y, z);
#define MKID(a, b) a##_##b
int foo_bar = MKID(foo, bar);
