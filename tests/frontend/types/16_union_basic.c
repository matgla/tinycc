union U {
    int i;
    char c;
};

int f(union U *u) {
    return u->i;
}
