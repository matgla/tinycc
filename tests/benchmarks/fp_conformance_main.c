/*
 * fp_conformance_main.c - RP2350 entry point for the FP conformance suite.
 *
 * Deliberately a separate executable from the benchmark image rather than
 * another registered benchmark:
 *
 *   - The vector tables are ~143 KB of .rodata.  The benchmark image is
 *     copy_to_ram and its cycle counts are tracked per-commit by the CI
 *     dashboard (docs/metrics_dashboard.md); dropping 143 KB into it could
 *     shift those numbers for reasons that have nothing to do with codegen.
 *   - Conformance is pass/fail, not a timing measurement, so it does not want
 *     the benchmark harness's calibration or watchdog machinery.
 *
 * The code under test is tests/fp/fp_conformance.c, compiled by TCC (or GCC
 * for a reference run); this file is always built by GCC and only provides
 * UART bring-up, so a miscompile in the unit under test cannot take the
 * reporting path down with it.
 *
 * Serial protocol matches the benchmark harness so the same host-side
 * synchronisation works: '~' preamble, then ===SYNC_START===.
 */

#include "pico/stdlib.h"

#include <stdio.h>

#include "fp_conformance.h"

#define UART_ID uart0
#define BAUD_RATE 460800
#define UART_TX_PIN 32
#define UART_RX_PIN 33

int main(void)
{
  int failures;

  stdio_init_all();

  uart_init(UART_ID, BAUD_RATE);
  gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

  uart_tx_wait_blocking(UART_ID);
  sleep_ms(100);

  /* Discard power-up garbage, then emit the marker the host syncs on. */
  for (int i = 0; i < 20; i++)
    uart_putc_raw(UART_ID, '~');
  printf("\r\n");
  uart_tx_wait_blocking(UART_ID);
  sleep_ms(50);

  printf("===SYNC_START===\r\n");
  uart_tx_wait_blocking(UART_ID);

  printf("FP conformance build: %s\r\n", FP_CONFORMANCE_BUILD_ID);

  failures = fp_conformance_run();

  if (failures == 0)
    printf("\r\nFP CONFORMANCE: PASS\r\n");
  else
    printf("\r\nFP CONFORMANCE: FAIL failures=%d\r\n", failures);

  /* run_benchmark.py's shared capture script (_build_run_script) stops
   * reading on this exact phrase, so keep it verbatim even though this is not
   * a benchmark. */
  printf("benchmark stopped\r\n");
  uart_tx_wait_blocking(UART_ID);

  while (1)
    sleep_ms(1000);

  return 0;
}
