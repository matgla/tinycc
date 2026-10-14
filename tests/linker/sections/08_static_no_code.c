/* A -static link whose image has nothing allocatable with any size: the
 * entry point is an absolute symbol and no input carries code or data.
 * layout_sections found the slot for PT_GNU_RELRO by incrementing the last
 * PT_LOAD pointer, which was still NULL here -- a segfault instead of an
 * image (or, for an object that did not define _start, instead of the
 * "_start not defined" diagnostic). */
__asm__(".globl _start\n.set _start, 0x1001");
