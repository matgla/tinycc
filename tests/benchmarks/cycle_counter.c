/*
 * ARM Cortex-M Cycle Counter Implementation
 * Uses DWT_CYCCNT for precise timing
 */

/* ARM DWT registers */
#define DWT_CTRL_ADDR   0xE0001000
#define DWT_CYCCNT_ADDR 0xE0001004
#define DEMCR_ADDR      0xE000EDFC

#define TRCENA_BIT      (1 << 24)
#define CYCCNTENA_BIT   (1 << 0)

void enable_cycle_counter(void) {
    volatile unsigned int *demcr = (volatile unsigned int *)DEMCR_ADDR;
    volatile unsigned int *ctrl = (volatile unsigned int *)DWT_CTRL_ADDR;
    volatile unsigned int *cyccnt = (volatile unsigned int *)DWT_CYCCNT_ADDR;
    
    /* Enable DWT trace */
    *demcr |= TRCENA_BIT;
    
    /* Enable cycle counter */
    *ctrl |= CYCCNTENA_BIT;
    
    /* Reset counter */
    *cyccnt = 0;
}

unsigned int get_cycle_count_low(void) {
    volatile unsigned int *cyccnt = (volatile unsigned int *)DWT_CYCCNT_ADDR;
    return *cyccnt;
}
