/* Test where DEREF is embedded in ADD */
int test(int *p) {
    int sum = 0;
    sum += *p++;
    return sum;
}
