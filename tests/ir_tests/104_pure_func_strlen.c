// Test functions for disassembly comparison

int sum_array(int *p, int n) {
    int sum = 0;
    while (n-- > 0)
        sum += *p++;
    return sum;
}

int dot_product(int *a, int *b, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int fibonacci(int n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

int max(int a, int b) {
    return (a > b) ? a : b;
}

int absolute(int x) {
    return (x < 0) ? -x : x;
}
