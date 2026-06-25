/*
 * TCC Bug: Volatile register access with large constants
 *
 * This test reproduces the volatile load/store patterns that previously
 * failed around ARM DWT cycle counter access, but uses fake registers so it
 * can run on any host without depending on hardware MMIO.
 */

#include <stdio.h>

static volatile unsigned int fake_demcr;
static volatile unsigned int fake_ctrl;
static volatile unsigned int fake_cyccnt;

static void fake_cycle_counter_tick(void)
{
  fake_cyccnt += 1;
}

/* Simplified cycle counter enable - version that triggered the bug */
void enable_cycle_counter_bug(void)
{
  /* These volatile accesses caused "load_to_dest_ir I64/F64" error */
  volatile unsigned int *demcr = &fake_demcr;
  volatile unsigned int *ctrl = &fake_ctrl;
  volatile unsigned int *cyccnt = &fake_cyccnt;

  /* Enable DWT trace - bit 24 */
  *demcr |= (1 << 24);

  /* Enable cycle counter - bit 0 */
  *ctrl |= (1 << 0);

  /* Reset counter */
  *cyccnt = 0;
}

/* Read cycle counter - simpler version */
unsigned int read_cyccnt(void)
{
  volatile unsigned int *cyccnt = &fake_cyccnt;
  return *cyccnt;
}

/* Direct register access without function calls */
unsigned int direct_reg_access(void)
{
  /* Write to memory-mapped register */
  *(volatile unsigned int *)&fake_cyccnt = 0;
  /* Read back */
  return *(volatile unsigned int *)&fake_cyccnt;
}

int main(void)
{
  printf("Testing volatile register access\n");

  /* This sequence caused compiler errors */
  enable_cycle_counter_bug();

  unsigned int count1 = read_cyccnt();
  printf("cyccnt1: %u\n", count1);

  /* Do some work */
  volatile int sum = 0;
  for (int i = 0; i < 100; i++)
  {
    sum += i;
    fake_cycle_counter_tick();
  }

  unsigned int count2 = read_cyccnt();
  printf("cyccnt2: %u\n", count2);
  printf("delta: %u\n", count2 - count1);

  unsigned int direct = direct_reg_access();
  printf("direct: %u\n", direct);

  printf("Tests completed\n");
  return 0;
}
