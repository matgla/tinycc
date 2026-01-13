/* Minimal test for double union store bug */
#include <stdio.h>

union du {
    double d;
    unsigned long long u;
};

int main() {
    union du x;
    x.d = 8.0;
    printf("low=%08x high=%08x\n", (unsigned)(x.u & 0xFFFFFFFF), (unsigned)(x.u >> 32));
    return 0;
}
