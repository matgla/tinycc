struct S {
    int a;
    int b;
};

int f(void) {
    struct S s = (struct S){ 1, 2 };
    return s.a + s.b;
}
