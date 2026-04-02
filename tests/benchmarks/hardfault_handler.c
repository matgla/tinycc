/*
 * HardFault handler and benchmark watchdog for RP2350 (Cortex-M33).
 * Uses direct UART register writes — no printf/stdio dependency.
 */

#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/uart.h"

/* Current benchmark name - set by benchmark_main.c before each run */
volatile const char *current_benchmark_name = NULL;
volatile const char *current_benchmark_phase = NULL;

/* RP2350 UART0 registers */
#define UART0_BASE_ADDR 0x40070000
#define UART_DR   (*(volatile uint32_t *)(UART0_BASE_ADDR + 0x000))
#define UART_FR   (*(volatile uint32_t *)(UART0_BASE_ADDR + 0x018))
#define UART_FR_TXFF (1u << 5)

/* ARM SCB fault status registers */
#define SCB_CFSR  (*(volatile uint32_t *)0xE000ED28)
#define SCB_HFSR  (*(volatile uint32_t *)0xE000ED2C)
#define SCB_MMFAR (*(volatile uint32_t *)0xE000ED34)
#define SCB_BFAR  (*(volatile uint32_t *)0xE000ED38)

static void fault_putc(char c)
{
  while (UART_FR & UART_FR_TXFF)
    ;
  UART_DR = c;
}

static void fault_puts(const char *s)
{
  while (*s) {
    if (*s == '\n')
      fault_putc('\r');
    fault_putc(*s++);
  }
}

static void fault_puthex(uint32_t val)
{
  fault_puts("0x");
  for (int i = 28; i >= 0; i -= 4) {
    uint8_t nibble = (val >> i) & 0xF;
    fault_putc(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
  }
}

static void fault_print_reg(const char *name, uint32_t val)
{
  fault_puts(name);
  fault_puthex(val);
  fault_puts("\n");
}

/* ====================================================================
 * HardFault Handler
 * ==================================================================== */

void hardfault_handler_c(uint32_t *stack_frame, uint32_t exc_return)
{
  uint32_t cfsr = SCB_CFSR;
  uint32_t hfsr = SCB_HFSR;
  uint32_t mmfar = SCB_MMFAR;
  uint32_t bfar = SCB_BFAR;

  fault_puts("\n\n!!! HARDFAULT !!!\n");

  if (current_benchmark_name) {
    fault_puts("Benchmark: ");
    fault_puts((const char *)current_benchmark_name);
    fault_puts(" (");
    fault_puts(current_benchmark_phase ? (const char *)current_benchmark_phase : "?");
    fault_puts(")\n");
  }

  fault_print_reg("PC:   ", stack_frame[6]);
  fault_print_reg("LR:   ", stack_frame[5]);
  fault_print_reg("R0:   ", stack_frame[0]);
  fault_print_reg("R1:   ", stack_frame[1]);
  fault_print_reg("R2:   ", stack_frame[2]);
  fault_print_reg("R3:   ", stack_frame[3]);
  fault_print_reg("R12:  ", stack_frame[4]);
  fault_print_reg("xPSR: ", stack_frame[7]);
  fault_print_reg("EXC_RETURN: ", exc_return);
  fault_print_reg("CFSR: ", cfsr);
  fault_print_reg("HFSR: ", hfsr);

  if (cfsr & 0x00FF) {
    fault_puts("MemManage: ");
    fault_puthex(cfsr & 0xFF);
    if (cfsr & 0x80) {
      fault_puts(" MMFAR=");
      fault_puthex(mmfar);
    }
    fault_puts("\n");
  }
  if (cfsr & 0xFF00) {
    fault_puts("BusFault: ");
    fault_puthex((cfsr >> 8) & 0xFF);
    if (cfsr & 0x8000) {
      fault_puts(" BFAR=");
      fault_puthex(bfar);
    }
    fault_puts("\n");
  }
  if (cfsr & 0xFFFF0000) {
    uint32_t ufsr = (cfsr >> 16) & 0xFFFF;
    fault_puts("UsageFault: ");
    fault_puthex(ufsr);
    if (ufsr & 0x0001) fault_puts(" UNDEFINSTR");
    if (ufsr & 0x0002) fault_puts(" INVSTATE");
    if (ufsr & 0x0004) fault_puts(" INVPC");
    if (ufsr & 0x0008) fault_puts(" NOCP");
    if (ufsr & 0x0010) fault_puts(" STKOF");
    if (ufsr & 0x0100) fault_puts(" UNALIGNED");
    if (ufsr & 0x0200) fault_puts(" DIVBYZERO");
    fault_puts("\n");
  }
  if (hfsr & (1u << 30))
    fault_puts("FORCED: escalated from configurable fault\n");
  if (hfsr & (1u << 1))
    fault_puts("VECTTBL: vector table read fault\n");

  fault_puts("benchmark stopped\n");

  while (1)
    __asm volatile("bkpt #0");
}

void __attribute__((naked)) isr_hardfault(void)
{
  __asm volatile(
      "tst lr, #4       \n"
      "ite eq            \n"
      "mrseq r0, msp     \n"
      "mrsne r0, psp     \n"
      "mov r1, lr        \n"
      "b hardfault_handler_c \n"
  );
}

/* ====================================================================
 * Benchmark Watchdog — catches infinite loops via hardware timer alarm
 * ==================================================================== */

#define BENCHMARK_TIMEOUT_MS 60000

static alarm_id_t watchdog_alarm_id = -1;

static int64_t watchdog_alarm_callback(alarm_id_t id, void *user_data)
{
  (void)id;
  (void)user_data;

  fault_puts("\n\n!!! BENCHMARK TIMEOUT !!!\n");

  if (current_benchmark_name) {
    fault_puts("Benchmark: ");
    fault_puts((const char *)current_benchmark_name);
    fault_puts(" (");
    fault_puts(current_benchmark_phase ? (const char *)current_benchmark_phase : "?");
    fault_puts(")\n");
  }

  fault_puts("Benchmark did not complete within 60 seconds — likely infinite loop\n");
  fault_puts("benchmark stopped\n");

  while (1)
    __asm volatile("bkpt #0");

  return 0;
}

void benchmark_watchdog_start(void)
{
  if (watchdog_alarm_id >= 0)
    cancel_alarm(watchdog_alarm_id);
  watchdog_alarm_id = add_alarm_in_ms(BENCHMARK_TIMEOUT_MS, watchdog_alarm_callback, NULL, true);
}

void benchmark_watchdog_stop(void)
{
  if (watchdog_alarm_id >= 0) {
    cancel_alarm(watchdog_alarm_id);
    watchdog_alarm_id = -1;
  }
}
