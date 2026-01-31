/*
 * Minimal test for cycle/timer counter on RP2350
 */

#include <stdint.h>
#include <stdio.h>

/* ARM DWT registers */
#define DWT_CTRL_ADDR   0xE0001000
#define DWT_CYCCNT_ADDR 0xE0001004
#define DEMCR_ADDR      0xE000EDFC

#define TRCENA_BIT      (1 << 24)
#define CYCCNTENA_BIT   (1 << 0)

/* RP2350 Timer registers */
#define TIMER_BASE      0x40054000
#define TIMER_TIMEHR    (TIMER_BASE + 0x08)
#define TIMER_TIMELR    (TIMER_BASE + 0x0c)

volatile uint32_t *dwt_ctrl = (volatile uint32_t *)DWT_CTRL_ADDR;
volatile uint32_t *dwt_cyccnt = (volatile uint32_t *)DWT_CYCCNT_ADDR;
volatile uint32_t *demcr = (volatile uint32_t *)DEMCR_ADDR;
volatile uint32_t *timer_lor = (volatile uint32_t *)TIMER_TIMELR;
volatile uint32_t *timer_hir = (volatile uint32_t *)TIMER_TIMEHR;

int main(void)
{
    printf("=== Timer/DWT Test ===\r\n");
    
    /* Read current values before init */
    printf("Before init:\r\n");
    printf("  DEMCR=0x%08X\r\n", (unsigned int)*demcr);
    printf("  DWT_CTRL=0x%08X\r\n", (unsigned int)*dwt_ctrl);
    printf("  DWT_CYCCNT=0x%08X\r\n", (unsigned int)*dwt_cyccnt);
    printf("  TIMER_HI=0x%08X LO=0x%08X\r\n", 
           (unsigned int)*timer_hir, (unsigned int)*timer_lor);
    
    /* Enable DWT */
    *demcr |= TRCENA_BIT;
    *dwt_cyccnt = 0;
    *dwt_ctrl |= CYCCNTENA_BIT;
    
    printf("\r\nAfter enabling DWT:\r\n");
    printf("  DEMCR=0x%08X\r\n", (unsigned int)*demcr);
    printf("  DWT_CTRL=0x%08X\r\n", (unsigned int)*dwt_ctrl);
    printf("  DWT_CYCCNT=0x%08X\r\n", (unsigned int)*dwt_cyccnt);
    
    /* Wait a bit */
    for (volatile int i = 0; i < 10000; i++);
    
    printf("\r\nAfter delay:\r\n");
    printf("  DWT_CYCCNT=0x%08X\r\n", (unsigned int)*dwt_cyccnt);
    printf("  TIMER_HI=0x%08X LO=0x%08X\r\n", 
           (unsigned int)*timer_hir, (unsigned int)*timer_lor);
    
    /* Another delay */
    for (volatile int i = 0; i < 10000; i++);
    
    printf("\r\nAfter second delay:\r\n");
    printf("  DWT_CYCCNT=0x%08X\r\n", (unsigned int)*dwt_cyccnt);
    printf("  TIMER_HI=0x%08X LO=0x%08X\r\n", 
           (unsigned int)*timer_hir, (unsigned int)*timer_lor);
    
    printf("\r\n=== Test Complete ===\r\n");
    
    return 0;
}
