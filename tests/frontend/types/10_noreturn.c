_Noreturn void die(int x);

int f(int x) {
    if (x)
        die(x);
    return 0;
}
