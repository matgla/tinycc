.syntax unified
.thumb

.global _start 
_start:

.global test_adc_reg
test_adc_reg:
    // T1 
    adcs.n r2, r2, r5
    itte eq 
    adceq r1, r1, r2
    adceq r3, r5 
    adcne r5, r5, r1

    adc r1, r1, r4
    // T2
    adc r1, r2, r3, rrx 
    adc r10, r11, r12, rrx 
    itte eq
    adceq r10, r11, r12 
    adceq r10, r12
    adcne r11, r1

    adc r1, r1, r2, lsl #1
    adc r6, r6, r8, lsr #2
    adc r1, r1, r2, asr #3
    adc r6, r6, r8, ror #4
    
    adc r1, r4, r2, lsl #1
    adc r6, r4, r8, lsr #2
    adc r1, r8, r2, asr #3
    adc r6, r7, r8, ror #4


    adcs r1, r2, r3, rrx 
    adcs r10, r11, r12, rrx 

    adcs r1, r1, r2, lsl #1
    adcs r6, r6, r8, lsr #2
    adcs r1, r1, r2, asr #3
    adcs r6, r6, r8, ror #4
    
    adcs r1, r4, r2, lsl #1
    adcs r6, r4, r8, lsr #2
    adcs r1, r8, r2, asr #3
    adcs r6, r7, r8, ror #4

    ittee eq
    adceq r1, r1, r2, lsl #1
    adceq r6, r6, r8, lsr #2
    adcne r1, r1, r2, asr #3
    adcne r6, r6, r8, ror #4
    
    ittee eq
    adcseq r1, r4, r2, lsl #1
    adcseq r6, r4, r8, lsr #2
    adcsne r1, r8, r2, asr #3
    adcsne r6, r7, r8, ror #4
