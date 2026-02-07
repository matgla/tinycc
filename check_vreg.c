#include <stdio.h>
#include "tccir.h"

int main() {
    // T0 would be position 0, type TEMP (2)
    int32_t t0 = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 0);
    printf("T0 encoded: %d\n", t0);
    printf("T0 type: %d, position: %d\n", 
           TCCIR_DECODE_VREG_TYPE(t0), 
           TCCIR_DECODE_VREG_POSITION(t0));
    
    // 268435456 in hex
    printf("268435456 = 0x%x\n", 268435456);
    printf("vr=279707648 = 0x%x\n", 279707648);
    printf("vr=539230208 = 0x%x\n", 539230208);
    
    return 0;
}
