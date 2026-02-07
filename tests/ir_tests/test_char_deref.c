#include <stdio.h>

int main() {
    char *a = "hello";
    char c = *a;  // Should be 'h' = 104
    printf("char c = %d\n", (int)c);
    
    int i = *a;  // Should also be 104
    printf("int i = %d\n", i);
    
    return 0;
}
