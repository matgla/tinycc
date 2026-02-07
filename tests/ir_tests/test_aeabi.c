// Test __aeabi functions directly
extern int __aeabi_dcmpeq(double a, double b);
extern int __aeabi_dcmplt(double a, double b);
extern int __aeabi_dcmple(double a, double b);

// Simple putchar for output
extern int putchar(int c);

void print_hex(unsigned int val) {
    const char* hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        putchar(hex[(val >> (i*4)) & 0xF]);
    }
}

int main(void) {
    double a = 3.14;
    double b = 2.0;
    
    // Print the raw bits of a
    unsigned int *pa = (unsigned int*)&a;
    putchar('a'); putchar(':'); putchar(' ');
    print_hex(pa[1]); putchar(' '); print_hex(pa[0]); putchar('\n');
    
    // Print the raw bits of b
    unsigned int *pb = (unsigned int*)&b;
    putchar('b'); putchar(':'); putchar(' ');
    print_hex(pb[1]); putchar(' '); print_hex(pb[0]); putchar('\n');
    
    // Test comparisons
    int eq = __aeabi_dcmpeq(a, b);
    int lt = __aeabi_dcmplt(a, b);
    int le = __aeabi_dcmple(a, b);
    
    putchar('e'); putchar('q'); putchar(':'); putchar(' ');
    putchar('0' + eq); putchar('\n');
    
    putchar('l'); putchar('t'); putchar(':'); putchar(' ');
    putchar('0' + lt); putchar('\n');
    
    putchar('l'); putchar('e'); putchar(':'); putchar(' ');
    putchar('0' + le); putchar('\n');
    
    return 0;
}
