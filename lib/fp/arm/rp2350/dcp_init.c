/*
 * RP2350 Double Coprocessor Initialization
 * Configures DCP (Double Coprocessor) for double-precision floating point
 * Reference: RP2350 Datasheet - Section on Double Coprocessor
 */

#include "tcc_stdint.h"

/* RP2350 Double Coprocessor Register Definitions */
#define DCP_BASE 0x50200000

/* DCP Control Registers */
#define DCP_CTRL (*(volatile uint32_t *)(DCP_BASE + 0x00))
#define DCP_STATUS (*(volatile uint32_t *)(DCP_BASE + 0x04))
#define DCP_INSTR (*(volatile uint32_t *)(DCP_BASE + 0x08))

/* Coprocessor register mappings */
typedef struct
{
  volatile uint32_t reg_lo;
  volatile uint32_t reg_hi;
} dcp_reg_pair_t;

#define DCP_REGS ((dcp_reg_pair_t *)(DCP_BASE + 0x100))

/* Initialize RP2350 double coprocessor */
void rp2350_dcp_init(void)
{
  /* TODO: Implement DCP initialization:
   * 1. Enable coprocessor clock
   * 2. Reset coprocessor
   * 3. Configure rounding mode
   * 4. Clear any pending interrupts
   */

  /* Write initialization sequence */
  DCP_CTRL = 0x01; /* Enable DCP */
}

/* Check if DCP is ready */
int rp2350_dcp_ready(void)
{
  return (DCP_STATUS & 0x01) != 0;
}

/* Wait for DCP operation to complete */
void rp2350_dcp_wait(void)
{
  while (!rp2350_dcp_ready())
  {
    /* Spin until ready */
  }
}
