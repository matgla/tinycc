/* Test case for post-increment embedded dereference optimization */

int test1(int *p, int n) {
    int sum = 0;
    while (n-- > 0)
        sum += *p++;
    return sum;
}

void test2(int *dst, int *src1, int *src2, int n) {
    for (int i = 0; i < n; i++)
        *dst++ = *src1++ + *src2++;
}

int test3(int *a, int *b, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++)
        sum += *a++ * *b++;
    return sum;
}

int main() {
    int arr1[] = {1, 2, 3, 4, 5};
    int arr2[] = {10, 20, 30, 40, 50};
    int dst[5];
    
    int sum = test1(arr1, 5);
    if (sum != 15) return 1;
    
    test2(dst, arr1, arr2, 5);
    if (dst[0] != 11) return 2;
    if (dst[4] != 55) return 3;
    
    int prod = test3(arr1, arr2, 5);
    if (prod != 550) return 4;
    
    return 0;
}
