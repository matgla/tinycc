#include <stdio.h>

int main() {
    // From IR dump:
    // 0001: T0 <-- P0 [ASSIGN]
    // 0002: T1 <-- T0 ADD #4
    
    // P0 = PARAM[0] = 0x30000000 = 805306368
    // T0 = TEMP[0]  = 0x20000000 = 536870912
    // T1 = TEMP[1]  = 0x20000001 = 536870913
    
    printf("P0 (PARAM[0]) = %d = 0x%x\n", 0x30000000, 0x30000000);
    printf("T0 (TEMP[0])  = %d = 0x%x\n", 0x20000000, 0x20000000);
    printf("T1 (TEMP[1])  = %d = 0x%x\n", 0x20000001, 0x20000001);
    
    return 0;
}
