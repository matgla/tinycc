extern int printf(const char *, ...);
int main() {
    int a = 0;
    int b = 1;
    printf("a=%d, b=%d, a||b=%d\n", a, b, a || b);
    return 0;
}
