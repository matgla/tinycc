/*
 * ARM Cortex-M DWT Cycle Counter Implementation
 * Uses DWT_CYCCNT with 64-bit overflow tracking
 */

#include <stdint.h>

/* ARM DWT registers */
#define DWT_CTRL_ADDR   0xE0001000
#define DWT_CYCCNT_ADDR 0xE0001004
#define DEMCR_ADDR      0xE000EDFC

#define TRCENA_BIT      (1 << 24)
#define CYCCNTENA_BIT   (1 << 0)

static volatile uint64_t cycle_count_high = 0;
static volatile uint32_t last_cycle_low = 0;
static volatile int dwt_enabled = 0;

void enable_cycle_counter(void)
{
    volatile uint32_t *demcr = (volatile uint32_t *)DEMCR_ADDR;
    volatile uint32_t *ctrl = (volatile uint32_t *)DWT_CTRL_ADDR;
    volatile uint32_t *cyccnt = (volatile uint32_t *)DWT_CYCCNT_ADDR;

    /* Reset overflow tracking */
    cycle_count_high = 0;
    last_cycle_low = 0;

    /* Enable TRCENA in DEMCR */
    *demcr |= TRCENA_BIT;
    
    /* Reset and enable CYCCNT */
    *cyccnt = 0;
    *ctrl |= CYCCNTENA_BIT;

    /* Verify it's working */
    uint32_t start = *cyccnt;
    for (volatile int i = 0; i < 100; i++);
    uint32_t end = *cyccnt;

    dwt_enabled = (end > start) ? 1 : 0;
}

/* Get 64-bit cycle count with overflow detection */
uint64_t get_cycle_count(void)
{
    if (!dwt_enabled) {
        return 0;
    }
    
    uint32_t current = *(volatile uint32_t *)DWT_CYCCNT_ADDR;
    
    /* Detect overflow: if current < last, we wrapped around */
    if (current < last_cycle_low) {
        cycle_count_high += 0x100000000ULL;
    }
    last_cycle_low = current;
    
    return cycle_count_high + current;
}

/* Check if DWT is available */
int using_dwt_counter(void)
{
    return dwt_enabled;
}
