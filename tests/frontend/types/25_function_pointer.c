int g(int x) {
    return x + 1;
}

int f(int (*fp)(int)) {
    return fp(1);
}
