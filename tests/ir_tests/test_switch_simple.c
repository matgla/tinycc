#include <stdio.h>

int main(void) {
    int i = 7;
    int r = 1000;
    switch (i) {
    case 0: r += i + 1; break;
    case 1: r -= i; break;
    case 2: r *= 2; r /= 2; r += 1; break;
    case 3: r = r / 2 + 1; break;
    case 4: r ^= i; break;
    case 5: r &= (0xFFFF + i); break;
    case 6: r |= (i & 0x0F); break;
    case 7: r = (r ^ 0xFF) ^ 0xFF; break;
    }
    printf("switch result: %d\n", r);
    return 0;
}
