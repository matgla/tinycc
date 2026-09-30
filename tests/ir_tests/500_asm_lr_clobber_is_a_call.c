/* An asm statement that clobbers LR -- a `bl` in its body -- needs LR saved
   like a call does.  asm_gen_code saves only r4-r11 around the statement,
   and a function whose only call is inside asm was a leaf, so its prologue
   did not save LR either: call_bump returned through the LR its own `bl`
   left, back into itself, forever. */
#include <stdio.h>

int counter;

void bump(void) { counter += 3; }

__attribute__((noinline)) void call_bump(void)
{
  __asm__ volatile("bl bump" : : : "r0", "r1", "r2", "r3", "ip", "lr", "memory", "cc");
}

/* the same with work around it, and a value live across the statement */
__attribute__((noinline)) int call_bump_twice(int x)
{
  int y = x * 5;
  __asm__ volatile("bl bump" : : : "r0", "r1", "r2", "r3", "ip", "lr", "memory", "cc");
  __asm__ volatile("bl bump" : : : "r0", "r1", "r2", "r3", "ip", "lr", "memory", "cc");
  return y + counter;
}

int main(void)
{
  call_bump();
  call_bump();
  printf("counter=%d\n", counter);
  int t = call_bump_twice(4);
  printf("twice=%d counter=%d\n", t, counter);
  return 0;
}
