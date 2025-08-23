.syntax unified
.thumb

.global _start 
_start:

.global test_adc_imm
test_adc_imm:
    adc r2, #0xde 
    adcs r4, #0xad
    adcs r10, r11, #0xa
    adc r8, r7, #0xb
    adcs r1, r2, #0xfafafafa
    adc r2, r3, #0xfafafafa

    itte gt 
    adcgt r1, r2, #1 
    adcgt r10, r11, #2 
    adcle r11, #2
    