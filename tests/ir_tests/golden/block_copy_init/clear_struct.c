struct S { int a, b, c; };

struct S make_zero(void) {
    struct S s = {0, 0, 0};
    return s;
}
