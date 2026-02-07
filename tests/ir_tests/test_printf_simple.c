// Direct test - print registers before printf
extern int printf(const char*, ...);

__attribute__((noinline))
void test_print(unsigned int r0, unsigned int r1, unsigned int r2, unsigned int r3) {
    printf("r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\n", r0, r1, r2, r3);
}

int main() {
    double d = 3.14;
    // Extract values
    union { double d; unsigned int u[2]; } conv;
    conv.d = d;
    
    printf("Before printf:\n");
    printf("low=0x%08x high=0x%08x\n", conv.u[0], conv.u[1]);
    printf("Double: %f\n", d);
    printf("After printf\n");
    
    return 0;
}
