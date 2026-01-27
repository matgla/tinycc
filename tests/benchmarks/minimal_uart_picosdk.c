/*
 * Minimal test using Pico SDK for startup
 * Initializes UART and runs benchmark library
 */

#include "pico/stdlib.h"

#include <stdio.h>

/* External benchmark library entry point */
extern int benchmark_main(void);

#define UART_ID uart0
#define BAUD_RATE 115200
#define UART_TX_PIN 32
#define UART_RX_PIN 33
#define LED_PIN 25

int main(void)
{
  // Pico SDK initializes clocks, stdio, etc.
  stdio_init_all();

  // Configure UART explicitly
  uart_init(UART_ID, BAUD_RATE);
  gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

  // Send UART message
  printf("Starting benchmark:\r\n\r\n");

  // Run benchmark library
  int result = benchmark_main();

  // Print result
  if (result == 0)
  {
    printf("\r\nBenchmark completed successfully!\r\n");
    printf("benchmark stopped\r\n");
  }
  else
  {
    printf("\r\nBenchmark failed!\r\n");
  }

  // Slow blink forever
  while (1)
  {
    sleep_ms(500);
    sleep_ms(500);
    printf(".");
  }

  return 0;
}
