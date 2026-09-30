/* A var-slot STORE of a symbol's address (a SYMREF without is_lval) or of a
   64-bit immediate defines the var like an immediate store does, so SSA
   renaming gives it a fresh name.  Kept a memory write, it stayed a second
   def under the previous store's TEMP name, and the zero a compound literal's
   fill had stored first reached the code before every Zig panic message
   slice (`movs r0, #0; ldr r0, =msg`).  Checks the values arrive -- by value
   in registers, through a join, and for a 64-bit field. */
#include <stdio.h>
#include <stdint.h>

struct sl { const char *p; unsigned n; };
struct opt { unsigned char is_null; unsigned payload; };
struct w64 { uint64_t v; const char *tag; };

static const char msg_a[] = "first message";
static const char msg_b[] = "second";

__attribute__((noinline)) static unsigned show(struct sl s, struct opt o)
{
    printf("%.*s %u %u\n", (int)s.n, s.p, o.is_null, o.payload);
    return s.n + o.payload;
}

__attribute__((noinline)) static unsigned show64(struct w64 w)
{
    printf("%llx %s\n", (unsigned long long)w.v, w.tag);
    return (unsigned)w.v;
}

__attribute__((noinline)) static unsigned pick(int k)
{
    struct sl s = (struct sl){msg_a, 5};
    if (k)
        s = (struct sl){msg_b, 6};
    return show(s, (struct opt){0, 7});
}

int main(void)
{
    unsigned r = show((struct sl){msg_a, 13}, (struct opt){1, 0xaaaaaaaa});
    r += pick(0) + pick(1);
    r += show64((struct w64){0x123456789abcdef0ull, msg_b});
    printf("%u\n", r);
    return 0;
}
