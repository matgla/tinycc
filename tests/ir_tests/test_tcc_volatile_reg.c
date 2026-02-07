/*
 * TCC Bug: Volatile register access with large constants
 * 
 * This test reproduces issues with accessing ARM DWT cycle counter registers.
 * The bug appears when using volatile pointer dereferencing with memory-mapped registers.
 */

#include <stdio.h>

/* Simplified cycle counter enable - version that triggered the bug */
void enable_cycle_counter_bug(void) {
    /* These volatile accesses caused "load_to_dest_ir I64/F64" error */
    volatile unsigned int *demcr = (volatile unsigned int *)0xE000EDFC;
    volatile unsigned int *ctrl = (volatile unsigned int *)0xE0001000;
    volatile unsigned int *cyccnt = (volatile unsigned int *)0xE0001004;
    
    /* Enable DWT trace - bit 24 */
    *demcr |= (1 << 24);
    
    /* Enable cycle counter - bit 0 */
    *ctrl |= (1 << 0);
    
    /* Reset counter */
    *cyccnt = 0;
}

/* Read cycle counter - simpler version */
unsigned int read_cyccnt(void) {
    volatile unsigned int *cyccnt = (volatile unsigned int *)0xE0001004;
    return *cyccnt;
}

/* Direct register access without function calls */
unsigned int direct_reg_access(void) {
    /* Write to memory-mapped register */
    *(volatile unsigned int *)0xE0001004 = 0;
    /* Read back */
    return *(volatile unsigned int *)0xE0001004;
}

int main(void) {
    printf("Testing volatile register access\n");
    
    /* This sequence caused compiler errors */
    enable_cycle_counter_bug();
    
    unsigned int count1 = read_cyccnt();
    printf("cyccnt1: %u\n", count1);
    
    /* Do some work */
    volatile int sum = 0;
    for (int i = 0; i < 100; i++) {
        sum += i;
    }
    
    unsigned int count2 = read_cyccnt();
    printf("cyccnt2: %u\n", count2);
    printf("delta: %u\n", count2 - count1);
    
    unsigned int direct = direct_reg_access();
    printf("direct: %u\n", direct);
    
    printf("Tests completed\n");
    return 0;
}
