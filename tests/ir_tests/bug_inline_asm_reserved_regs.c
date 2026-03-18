typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

#define CF (1u << 0)
#define PF (1u << 2)
#define AF (1u << 4)
#define ZF (1u << 6)
#define SF (1u << 7)
#define OF (1u << 11)

#define EFLAGS_BITS (CF | PF | AF | ZF | SF | OF)

int main(void)
{
  uint16_t x = 0x1234;
  uint32_t eflags = 0x56789abcU;
  uint16_t bsr_result;
  uint32_t bsr_eflags;

  __asm volatile("" : "=&r"(bsr_result), "=&r"(bsr_eflags) : "r"(x), "i"(~EFLAGS_BITS), "r"(eflags));

  return 0;
}