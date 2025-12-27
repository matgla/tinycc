extern int printf(const char *, ...);
int main() {
    int a = 0;
    int c = 0;
    printf("a=%d, c=%d, a||c=%d\n", a, c, a || c);
    return 0;
}
