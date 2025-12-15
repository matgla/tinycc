void Reset_Handler(void);

const unsigned long vectors[] __attribute__((section(".text"))) = {
    (const unsigned long)0x20001000,     // Initial Stack Pointer
    (const unsigned long)&Reset_Handler, // Reset Handler
    0,                                   // NMI Handler
    0,                                   // Hard Fault Handler
    0,                                   // MPU Fault Handler
    0,                                   // Bus Fault Handler
    0,                                   // Usage Fault Handler
    0,                                   // Reserved
    0,                                   // Reserved
    0,                                   // Reserved
    0,                                   // Reserved
    0,                                   // SVCall Handler
    0,                                   // Debug Monitor Handler
    0,                                   // Reserved
    0,                                   // PendSV Handler
    0,                                   // SysTick Handler
};

#include <stdint.h>

uint32_t stack[16 * 1024];
extern uint32_t __StackTop = (uint32_t)&stack[16 * 1024];

extern void _mainCRTStartup(int);

void Reset_Handler(void) {
  _mainCRTStartup(0);
  //   while (1)
  // ;
}
