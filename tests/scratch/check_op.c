#include <stdio.h>
#include "../tccir_operand.h"

int main() {
    // Values from debug output
    int vr1 = 279707648;  // src1
    int vr2 = 539230208;  // src2
    
    printf("vr1 = %d = 0x%x\n", vr1, vr1);
    printf("  type = %d, position = %d\n", 
           TCCIR_DECODE_VREG_TYPE(vr1),
           TCCIR_DECODE_VREG_POSITION(vr1));
    
    printf("vr2 = %d = 0x%x\n", vr2, vr2);
    printf("  type = %d, position = %d\n",
           TCCIR_DECODE_VREG_TYPE(vr2),
           TCCIR_DECODE_VREG_POSITION(vr2));
    
    // What would T0 look like?
    int t0 = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 0);
    printf("\nT0 encoded = %d = 0x%x\n", t0, t0);
    
    return 0;
}
