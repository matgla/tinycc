int sccp_loop(int n) {
    int i = 0;
    int sum = 0;
    while (i < n) {
        sum = sum + i;
        i = i + 1;
    }
    return sum;
}
