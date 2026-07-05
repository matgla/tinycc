struct S {
    int a;
    int b;
};

int f(void) {
    struct S s = { .b = 2, .a = 1 };
    return s.a + s.b;
}
