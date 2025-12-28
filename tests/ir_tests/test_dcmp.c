#include <stdio.h>

int main(void) {
    double a = 3.14;
    double b = 2.0;
    
    // Test comparisons
    if (a > b) {
        printf("3.14 > 2.0: PASS\n");
    } else {
        printf("3.14 > 2.0: FAIL\n");
    }
    
    if (a == 3.14) {
        printf("a == 3.14: PASS\n");
    } else {
        printf("a == 3.14: FAIL\n");
    }
    
    return 0;
}
