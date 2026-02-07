#include <stdio.h>

int main(void) {
    int i = 2;
    int r = 100;
    switch (i) {
    case 0: r += 10; break;
    case 1: r -= 10; break;
    case 2: r *= 2; break;
    }
    printf("small switch result: %d\n", r);
    return 0;
}
