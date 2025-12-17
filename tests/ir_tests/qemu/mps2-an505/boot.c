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

// extern int __libc_init_array = 0;
extern int __bss_start__ = 0;
extern int __bss_end__ = 0;
// extern int __libc_fini_array = 0;

unsigned long heap[1024 * 32];

unsigned long __end__ = (unsigned long)&heap[0] + sizeof(heap);
unsigned long end = (unsigned long)&heap[0] + sizeof(heap);

// extern int __errno = 0;

extern void _mainCRTStartup(int);

void Reset_Handler(void) {
  _mainCRTStartup(0);
  //   while (1)
  // ;
}
