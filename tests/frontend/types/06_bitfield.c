struct S {
    int a : 4;
    int b : 4;
};

int f(struct S *s) {
    return s->a + s->b;
}
