/* Guard: entry_store_prop must not forward an entry-block store through a
 * pointer that only *may* point at the stack slot it stored to.
 *
 * The pass builds a LEA map (vreg -> stack offset) from `T <-- Addr[StackLoc[X]]`
 * and then forwards constant entry-block stores into every deref of that vreg.
 * Nothing ever invalidated a map entry, so a pointer assigned the local's
 * address on ONE path kept that binding on all of them -- and the `s++` moved
 * it on to the array's second element, whose entry store is the NULL
 * terminator:
 *
 *     char *fallback[2] = {".", 0};        // entry stores: [0]=".", [1]=0
 *     s = argv ? argv : fallback;          // s: phi{param, &fallback}
 *     for (; *s; s++) use(*s);             // use(*s) folded to use(0)
 *
 * so the argument became a constant NULL even when s walked the caller's array.
 * Found via toybox `ls`: every path argument reached the kernel as NULL, so
 * `ls /dev` silently listed "/" instead.
 *
 * The call is what matters -- returning `*s` was always compiled correctly, it
 * is the argument slot that got the folded constant.  Expected output is the
 * -O0 / gcc behaviour; the unfixed build prints seen=0 hash=0 at -O1/-O2/-Os.
 */

#include <stdio.h>

unsigned hash = 0;
int seen = 0;

/* External linkage + a global side effect: keeps the call a real call. */
void use(const char *p)
{
  seen++;
  hash = hash * 31u + (p ? (unsigned char)p[0] : 0u);
}

void walk(char **argv_like)
{
  char **s, *fallback[] = {".", 0};

  for (s = *argv_like ? argv_like : fallback; *s; s++)
    use(*s);
}

int main(void)
{
  char *args[3];
  char *none[1];

  args[0] = "one";
  args[1] = "two";
  args[2] = 0;
  none[0] = 0;

  walk(args);
  printf("args seen=%d hash=%u\n", seen, hash);

  seen = 0;
  hash = 0;
  /* Empty argv: the fallback array really is used, so its stores are live. */
  walk(none);
  printf("fallback seen=%d hash=%u\n", seen, hash);

  return 0;
}
