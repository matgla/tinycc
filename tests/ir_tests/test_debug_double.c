#include <stdio.h>

int main() {
    double d = 3.14;
    
    // Extract the bits using union
    union {
        double d;
        unsigned int u[2];
    } conv;
    conv.d = d;
    
    printf("Low word: 0x%08x\n", conv.u[0]);
    printf("High word: 0x%08x\n", conv.u[1]);
    printf("Double: %f\n", d);
    
    return 0;
}
