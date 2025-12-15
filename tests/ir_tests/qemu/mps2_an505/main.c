
// extern int __libc_init_array = 0;
extern int __bss_start__ = 0;
extern int __bss_end__ = 0;
// extern int __libc_fini_array = 0;

unsigned long heap[1024 * 32];

unsigned long __end__ = (unsigned long)&heap[0] + sizeof(heap);
unsigned long end = (unsigned long)&heap[0] + sizeof(heap);

// extern int __errno = 0;
// extern int end = 0;

void puts(const char *s);

int main() {
  puts("Hello, World!\n");
  return 0;
}